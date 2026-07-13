# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
#
# Startup-snapshot e2e scenario (one of the tests/ integration suite).
#
# Proves a consumer can BUILD a startup snapshot with the packaged
# mkv8snapshot.exe and USE it through the public V8RuntimeArgs::startupSnapshotBlob
# surface - for both engines. Everything below runs against the freshly packed
# per-RID NuGet (packaged mkv8snapshot.exe + engine DLL + headers + consumer
# sources), so it exercises the real shipped chain end-to-end.
#
# For the engine this surface targets (v8jsi by default; v8jsisb with
# -UseV8Sandbox):
#   1. run `mkv8snapshot.exe --engine <dll>` on a tiny builder
#      (globalThis.snapshotMarker = 41 + 1) to produce a container blob;
#   2. build the consumer and run it `--snapshot <blob>` - asserts the baked-in
#      global is restored (marker === 42) with NO script evaluated;
#   3. if the OTHER engine's DLL is also in the package, build a blob with it and
#      run the consumer `--snapshot-reject <that blob>` - asserts the engine
#      rejects the cross-engine blob and falls back (marker absent), never crashes.
#
# Exit codes:
#   0 = snapshot e2e green
#   non-0 = any failure (no .nupkg, missing tool/engine, blob build, EXE assert)
#
# Run after `node scripts/build.ts --no-build --pack`. Shared restore/build logic
# lives in test-helpers.ps1.

[CmdletBinding()]
param(
    [string]$RepoRoot,
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64',
    [string]$PkgDir,
    [switch]$UseV8Sandbox
)

$ErrorActionPreference = 'Stop'
$harnessDir = $PSScriptRoot
if (-not $harnessDir) { $harnessDir = Split-Path -Parent $MyInvocation.MyCommand.Definition }
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $harnessDir '..')).Path }
. (Join-Path $harnessDir 'test-helpers.ps1')

$PkgDir = Get-PkgDir -RepoRoot $RepoRoot -PkgDir $PkgDir
$rid = Get-Rid -Platform $Platform
$pkg = Restore-V8Package -HarnessDir $harnessDir -PkgDir $PkgDir -Rid $rid

# The packaged offline snapshot builder + engine DLLs all live in the per-RID
# native dir. mkv8snapshot LoadLibrary's the engine from --engine-dir.
$nativeDir = $pkg.NativeDir
$mkv8 = Join-Path $nativeDir 'mkv8snapshot.exe'
if (-not (Test-Path $mkv8)) {
    throw "Packaged mkv8snapshot.exe not found at $mkv8 - the pack did not stage the snapshot tool."
}

# This surface's own engine, plus the other engine (for the cross-engine
# rejection check). mkv8snapshot auto-derives --jitless from the engine name
# (on for *sb*), matching how each engine is configured at runtime.
if ($UseV8Sandbox) {
    $ownDll = 'v8jsisb.dll'; $otherDll = 'v8jsi.dll'
} else {
    $ownDll = 'v8jsi.dll';   $otherDll = 'v8jsisb.dll'
}

$snapDir = Join-Path $harnessDir 'snap'
if (Test-Path $snapDir) { Remove-Item -Recurse -Force $snapDir }
New-Item -ItemType Directory -Force -Path $snapDir | Out-Null
$builderJs = Join-Path $snapDir 'builder.js'
Set-Content -Path $builderJs -Value 'globalThis.snapshotMarker = 41 + 1;' -Encoding ascii

# Build a snapshot blob with a given packaged engine. Returns the blob path, or
# $null if that engine isn't in the package.
function New-Snapshot {
    param([string]$EngineDll)
    $enginePath = Join-Path $nativeDir $EngineDll
    if (-not (Test-Path $enginePath)) { return $null }
    $out = Join-Path $snapDir ([System.IO.Path]::GetFileNameWithoutExtension($EngineDll) + '.bin')
    Write-Host "tests: building snapshot with $EngineDll -> $out"
    # Out-Host: mkv8snapshot's stdout to the console, not into the return value.
    & $mkv8 --engine $EngineDll --engine-dir $nativeDir --builder $builderJs --out $out | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "mkv8snapshot failed for $EngineDll (exit $LASTEXITCODE)" }
    if (-not (Test-Path $out)) { throw "mkv8snapshot did not produce $out" }
    return $out
}

$ownBlob = New-Snapshot -EngineDll $ownDll
if (-not $ownBlob) { throw "Own engine $ownDll not found in the package native dir $nativeDir" }
$otherBlob = New-Snapshot -EngineDll $otherDll   # may be $null if not packaged

$exe = Build-Consumer -HarnessDir $harnessDir -Configuration $Configuration `
    -Platform $Platform -Rid $rid -PkgProps $pkg.PkgProps -PkgTargets $pkg.PkgTargets `
    -UseV8Sandbox:$UseV8Sandbox

# Positive: matching blob -> marker restored with no eval.
Write-Host "tests: snapshot consume ($ownDll) ..."
& $exe --snapshot $ownBlob
if ($LASTEXITCODE -ne 0) { throw "snapshot consume FAILED for $ownDll (exit $LASTEXITCODE)" }

# Negative: cross-engine blob -> rejected + graceful fallback (marker absent).
if ($otherBlob) {
    Write-Host "tests: cross-engine rejection ($otherDll blob into $ownDll) ..."
    & $exe --snapshot-reject $otherBlob
    if ($LASTEXITCODE -ne 0) { throw "cross-engine rejection FAILED ($otherDll into $ownDll, exit $LASTEXITCODE)" }
} else {
    Write-Host "tests: cross-engine rejection skipped ($otherDll not in this package)"
}

Write-Host "tests: snapshot e2e OK ($ownDll)"
