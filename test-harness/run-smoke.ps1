# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
#
# B1 real-consumer smoke build driver.
#
# Stage D: the v8-jsi NuGet is now multi-package (meta + per-RID, per the
# Microsoft.JavaScript.LibNode pattern). For an x64 consumer the smoke
# restores the matching Microsoft.JavaScript.V8.win-x64.<version>.nupkg from
# out/pkg directly — there is no meta dependency to resolve.
#
# Picks the most recent Microsoft.JavaScript.V8.<rid>.<version>.nupkg from
# out/pkg, restores it into test-harness/packages/ against a local feed,
# msbuilds test-harness/consumer.vcxproj as the requested Configuration|x64,
# and runs the resulting v8jsi_smoke.exe -- which evals `1 + 2` through the
# JSI ABI runtime and asserts the result is 3, plus four cross-CRT
# exercises (utf8 round-trip, global property set/get, HostFunction call,
# JSError catch).
#
# Exit codes:
#   0 = smoke build green
#   non-0 = any failure (no .nupkg, restore/build/run failure, EXE assert)
#
# Run after a successful `node scripts/build.ts --no-build --pack` so
# out/pkg contains a freshly-packed .nupkg.

[CmdletBinding()]
param(
    [string]$RepoRoot,
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64',
    [string]$PkgDir
)

$ErrorActionPreference = 'Stop'
$harnessDir = $PSScriptRoot
if (-not $harnessDir) { $harnessDir = Split-Path -Parent $MyInvocation.MyCommand.Definition }
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $harnessDir '..')).Path }
# scripts/build.ts writes the packed .nupkg to scripts/out/pkg (the build.ts
# script roots itself at scripts/, so "out/pkg" is relative to that, not the
# repo). Probe both locations so a future relocation doesn't silently break
# the harness. The optional -PkgDir overrides auto-detection for CI, which
# stages packages outside the repo tree (e.g., $(Build.StagingDirectory)\out\pkg).
if ($PkgDir) {
    if (-not (Test-Path $PkgDir)) {
        throw "Provided -PkgDir does not exist: $PkgDir"
    }
} else {
    foreach ($candidate in @(
        (Join-Path $RepoRoot 'scripts\out\pkg'),
        (Join-Path $RepoRoot 'out\pkg')
    )) {
        if (Test-Path $candidate) { $PkgDir = $candidate; break }
    }
}
$packagesDir = Join-Path $harnessDir 'packages'
$packagesConfig = Join-Path $harnessDir 'packages.config'
$vcxproj = Join-Path $harnessDir 'consumer.vcxproj'

if (-not $pkgDir) {
    throw "Package staging dir not found under scripts/out/pkg or out/pkg. Run `node scripts/build.ts --no-build --pack` first."
}

# Map consumer platform (x86/x64/arm64) to the .NET runtime identifier the
# Microsoft.JavaScript.V8.<rid>.<version>.nupkg layout uses.
switch ($Platform.ToLower()) {
    'x64'   { $rid = 'win-x64' }
    'x86'   { $rid = 'win-x86' }
    'arm64' { $rid = 'win-arm64' }
    default { throw "Unsupported Platform for smoke harness: $Platform" }
}

# Pick the per-RID .nupkg for this platform. The aggregator pack flow also
# produces a meta Microsoft.JavaScript.V8.<version>.nupkg (without a RID
# suffix); the per-RID name is `Microsoft.JavaScript.V8.<rid>.<version>.nupkg`.
# We want the per-RID one — it carries the v8jsi.dll, .lib, headers, and
# templated .props/.targets. The meta only carries dependencies.
$nupkg = Get-ChildItem -Path $pkgDir -Filter "*$rid*.nupkg" -File |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $nupkg) {
    throw "No per-RID .nupkg (*$rid*.nupkg) found in $pkgDir. Run `node scripts/build.ts --no-build --pack` first."
}

# Extract version from the .nupkg filename: <id>.<version>.nupkg, where the
# version starts with a digit. Splitting on a digit-prefixed dot is safer
# than just splitting on '.' because the id itself contains dots.
$pkgBaseName = [System.IO.Path]::GetFileNameWithoutExtension($nupkg.Name)
if ($pkgBaseName -notmatch '^(?<id>.+?)\.(?<ver>\d.*)$') {
    throw "Cannot parse package id/version from filename: $($nupkg.Name)"
}
$pkgId = $Matches.id
$pkgVersion = $Matches.ver
Write-Host "test-harness: package = $pkgId, version = $pkgVersion ($($nupkg.FullName))"

# Refresh packages.config to pin the version we're testing.
$packagesXml = @"
<?xml version="1.0" encoding="utf-8"?>
<packages>
  <package id="$pkgId" version="$pkgVersion" targetFramework="native" />
</packages>
"@
Set-Content -Path $packagesConfig -Value $packagesXml -Encoding UTF8

# Clean prior restore so we always test against the latest pack.
if (Test-Path $packagesDir) {
    Remove-Item -Recurse -Force $packagesDir
}

# Locate nuget.exe (PATH or the one Node.js fork-syncs as part of the build).
$nuget = (Get-Command nuget.exe -ErrorAction SilentlyContinue).Source
if (-not $nuget) {
    throw "nuget.exe not found on PATH. Install NuGet CLI or add it to PATH."
}

# Restore against the local feed pointing at out/pkg.
& $nuget install $pkgId -Version $pkgVersion -Source $pkgDir -OutputDirectory $packagesDir
if ($LASTEXITCODE -ne 0) {
    throw "nuget install failed (exit $LASTEXITCODE)"
}

# Locate msbuild via vswhere.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at $vswhere"
}
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) {
    throw "MSBuild.exe not located via vswhere"
}
Write-Host "test-harness: msbuild = $msbuild"

# Resolve the freshly-restored package's .props/.targets files and pass them
# through to MSBuild as properties. nuget install drops the package at
# $packagesDir\<pkgId>.<version>\. Under Stage D the .props/.targets
# filenames match the package id (Microsoft.JavaScript.V8.<rid>.{props,targets})
# for NuGet's auto-import contract.
$packageDir = Join-Path $packagesDir "$pkgId.$pkgVersion"
$pkgProps = Join-Path $packageDir "build\native\$pkgId.props"
$pkgTargets = Join-Path $packageDir "build\native\$pkgId.targets"
if (-not (Test-Path $pkgProps)) {
    throw "Expected package .props not found: $pkgProps"
}
if (-not (Test-Path $pkgTargets)) {
    throw "Expected package .targets not found: $pkgTargets"
}

# Build. Pass V8JsiRuntimeIdentifier explicitly to bypass the .props RID
# auto-detection (which a real PackageReference consumer would let run on
# its own).
& $msbuild $vcxproj "/p:Configuration=$Configuration" "/p:Platform=$Platform" "/p:V8JsiPackageProps=$pkgProps" "/p:V8JsiPackageTargets=$pkgTargets" "/p:V8JsiRuntimeIdentifier=$rid" /restore:false /nologo /v:minimal
if ($LASTEXITCODE -ne 0) {
    throw "msbuild failed (exit $LASTEXITCODE)"
}

# Run.
$exe = Join-Path $harnessDir "bin\$Configuration\$Platform\v8jsi_smoke.exe"
if (-not (Test-Path $exe)) {
    throw "Expected EXE not produced: $exe"
}
Write-Host "test-harness: running $exe"
& $exe
if ($LASTEXITCODE -ne 0) {
    throw "v8jsi_smoke.exe failed (exit $LASTEXITCODE)"
}
Write-Host "test-harness: OK"
