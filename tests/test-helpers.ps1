# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
#
# Shared helpers for the tests/ integration suite. Each scenario driver
# (run-smoke.ps1, run-snapshot.ps1) dot-sources this file, then restores the
# freshly packed per-RID NuGet and builds the consumer.vcxproj against it. The
# v8-jsi NuGet is multi-package (meta + per-RID); the per-RID
# Microsoft.JavaScript.V8.<rid>.<version>.nupkg carries the runtime binaries,
# import lib, headers, consumer sources, and templated .props/.targets - that is
# the one a consumer actually builds against.

$ErrorActionPreference = 'Stop'

# Resolve the directory holding the packed .nupkg(s). scripts/build.ts writes to
# scripts/out/pkg; CI stages outside the repo and passes -PkgDir explicitly.
function Get-PkgDir {
    param([string]$RepoRoot, [string]$PkgDir)
    if ($PkgDir) {
        if (-not (Test-Path $PkgDir)) { throw "Provided -PkgDir does not exist: $PkgDir" }
        return $PkgDir
    }
    foreach ($candidate in @(
        (Join-Path $RepoRoot 'scripts\out\pkg'),
        (Join-Path $RepoRoot 'out\pkg')
    )) {
        if (Test-Path $candidate) { return $candidate }
    }
    throw "Package staging dir not found under scripts/out/pkg or out/pkg. Run ``node scripts/build.ts --no-build --pack`` first."
}

# Map a consumer platform to the .NET RID the package layout uses.
function Get-Rid {
    param([string]$Platform)
    switch ($Platform.ToLower()) {
        'x64'   { return 'win-x64' }
        'x86'   { return 'win-x86' }
        'arm64' { return 'win-arm64' }
        default { throw "Unsupported Platform: $Platform" }
    }
}

# Restore the latest per-RID .nupkg into tests/packages/ against a local feed.
# Returns the package id/version + the resolved .props/.targets and the native
# binary dir (runtimes/<rid>/native, where v8jsi.dll / v8jsisb.dll /
# mkv8snapshot.exe live).
function Restore-V8Package {
    param([string]$HarnessDir, [string]$PkgDir, [string]$Rid)

    $packagesDir = Join-Path $HarnessDir 'packages'
    $packagesConfig = Join-Path $HarnessDir 'packages.config'

    $nupkg = Get-ChildItem -Path $PkgDir -Filter "*$Rid*.nupkg" -File |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $nupkg) {
        throw "No per-RID .nupkg (*$Rid*.nupkg) found in $PkgDir. Run ``node scripts/build.ts --no-build --pack`` first."
    }

    # <id>.<version>.nupkg, where the version starts with a digit (the id itself
    # contains dots, so split on the first digit-prefixed dot).
    $pkgBaseName = [System.IO.Path]::GetFileNameWithoutExtension($nupkg.Name)
    if ($pkgBaseName -notmatch '^(?<id>.+?)\.(?<ver>\d.*)$') {
        throw "Cannot parse package id/version from filename: $($nupkg.Name)"
    }
    $pkgId = $Matches.id
    $pkgVersion = $Matches.ver
    Write-Host "tests: package = $pkgId, version = $pkgVersion ($($nupkg.FullName))"

    # Pin the version under test and re-restore from scratch.
    $packagesXml = @"
<?xml version="1.0" encoding="utf-8"?>
<packages>
  <package id="$pkgId" version="$pkgVersion" targetFramework="native" />
</packages>
"@
    Set-Content -Path $packagesConfig -Value $packagesXml -Encoding UTF8
    if (Test-Path $packagesDir) { Remove-Item -Recurse -Force $packagesDir }

    $nuget = (Get-Command nuget.exe -ErrorAction SilentlyContinue).Source
    if (-not $nuget) { throw "nuget.exe not found on PATH. Install NuGet CLI or add it to PATH." }
    # Pipe to Out-Host so nuget's stdout goes to the console, NOT into this
    # function's return value (PowerShell folds uncaptured output into the result).
    & $nuget install $pkgId -Version $pkgVersion -Source $PkgDir -OutputDirectory $packagesDir | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "nuget install failed (exit $LASTEXITCODE)" }

    $packageDir = Join-Path $packagesDir "$pkgId.$pkgVersion"
    $pkgProps = Join-Path $packageDir "build\native\$pkgId.props"
    $pkgTargets = Join-Path $packageDir "build\native\$pkgId.targets"
    if (-not (Test-Path $pkgProps)) { throw "Expected package .props not found: $pkgProps" }
    if (-not (Test-Path $pkgTargets)) { throw "Expected package .targets not found: $pkgTargets" }

    return [pscustomobject]@{
        PkgId      = $pkgId
        PkgVersion = $pkgVersion
        PackageDir = $packageDir
        PkgProps   = $pkgProps
        PkgTargets = $pkgTargets
        NativeDir  = Join-Path $packageDir "runtimes\$Rid\native"
    }
}

# Locate MSBuild via vswhere.
function Get-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found at $vswhere" }
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
        -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw "MSBuild.exe not located via vswhere" }
    return $msbuild
}

# Build consumer.vcxproj against the restored package. -UseV8Sandbox opts into
# the sandbox surface (sbox include + sbox.dll.lib, v8jsisb.dll/sbox.dll copied,
# SMOKE_SANDBOX defined). Returns the path to the produced v8jsi_smoke.exe.
function Build-Consumer {
    param(
        [string]$HarnessDir, [string]$Configuration, [string]$Platform,
        [string]$Rid, [string]$PkgProps, [string]$PkgTargets, [switch]$UseV8Sandbox
    )
    $msbuild = Get-MSBuild
    $vcxproj = Join-Path $HarnessDir 'consumer.vcxproj'
    # VC++ has no 'x86' platform - 32-bit is 'Win32'. Translate the harness
    # platform vocabulary (x64/x86/arm64, matching the RID and CI buildPlatform)
    # to the MSBuild platform the consumer project defines. The explicit
    # V8JsiRuntimeIdentifier below still selects the win-x86 package payload.
    $msbuildPlatform = if ($Platform -eq 'x86') { 'Win32' } else { $Platform }
    # Pass V8JsiRuntimeIdentifier explicitly to bypass the .props RID
    # auto-detection (a real PackageReference consumer lets that run on its own).
    $mbArgs = @(
        $vcxproj,
        "/p:Configuration=$Configuration",
        "/p:Platform=$msbuildPlatform",
        "/p:V8JsiPackageProps=$PkgProps",
        "/p:V8JsiPackageTargets=$PkgTargets",
        "/p:V8JsiRuntimeIdentifier=$Rid"
    )
    if ($UseV8Sandbox) {
        $mbArgs += '/p:UseV8Sandbox=true'
        Write-Host "tests: UseV8Sandbox=true (sandbox engine surface)"
    }
    # Out-Host: keep msbuild's stdout on the console, out of the return value.
    & $msbuild @mbArgs /restore:false /nologo /v:minimal | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "msbuild failed (exit $LASTEXITCODE)" }

    $exe = Join-Path $HarnessDir "bin\$Configuration\$msbuildPlatform\v8jsi_smoke.exe"
    if (-not (Test-Path $exe)) { throw "Expected EXE not produced: $exe" }
    return $exe
}
