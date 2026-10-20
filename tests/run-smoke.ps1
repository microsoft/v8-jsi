# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
#
# Real-consumer smoke scenario (one of the tests/ integration suite).
#
# Restores the freshly packed per-RID Microsoft.JavaScript.V8.<rid>.<version>.nupkg
# from out/pkg, msbuilds tests/consumer.vcxproj as the requested Configuration|x64
# against it, and runs the resulting v8jsi_smoke.exe with no args - which evals
# `1 + 2` through the JSI ABI runtime and asserts 3, plus four cross-CRT exercises
# (utf8 round-trip, global property set/get, HostFunction call, JSError catch).
#
# -UseV8Sandbox additionally validates the sandbox package surface: it sets
# /p:UseV8Sandbox=true (the package .targets add the v8host include dir +
# v8host.dll.lib and copy v8jsisb.dll / sbox.exe / v8host.dll) and the EXE then
# drives v8jsisb.dll through JSI and runs a session through the client API. Use it
# only when the restored package actually carries the sandbox binaries.
# consumer.vcxproj puts the EXE and copy-local payload in
# bin\<Configuration>\<Platform>\ (x64 or arm64 for sandbox consumers). For x86
# consumers the package rejects UseV8Sandbox; the script asserts that and stops.
#
# Shipped (release-trust) sandbox binaries accept only a validly signed client
# image, so the signed-pack acceptance runs the sandbox leg in two parts:
#   -ReleaseTrustPayload  build the consumer and run it unsigned: the broker must
#                         reject it with TRUST_E_NOSIGNATURE (0x800b0100);
#   -RunSigned            after the EXE is signed in place, skip restore and
#                         build, require a valid signature, run, expect success.
#
# Exit codes:
#   0 = smoke green
#   non-0 = any failure (no .nupkg, restore/build/run failure, EXE assert)
#
# Run after a successful `node scripts/build.ts --no-build --pack` so out/pkg
# contains a freshly-packed .nupkg. Shared restore/build logic lives in
# test-helpers.ps1.

[CmdletBinding()]
param(
    [string]$RepoRoot,
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64',
    [string]$PkgDir,
    [switch]$UseV8Sandbox,
    [switch]$ReleaseTrustPayload,
    [switch]$RunSigned
)

$ErrorActionPreference = 'Stop'
$harnessDir = $PSScriptRoot
if (-not $harnessDir) { $harnessDir = Split-Path -Parent $MyInvocation.MyCommand.Definition }
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $harnessDir '..')).Path }
. (Join-Path $harnessDir 'test-helpers.ps1')

if ($RunSigned) {
    if (-not $UseV8Sandbox) { throw '-RunSigned applies only with -UseV8Sandbox.' }
    $exe = Join-Path $harnessDir "bin\$Configuration\$(Get-MSBuildPlatform $Platform)\v8jsi_smoke.exe"
    if (-not (Test-Path $exe)) { throw "-RunSigned: $exe not found. Build it first with -ReleaseTrustPayload." }
    $signature = Get-AuthenticodeSignature -FilePath $exe
    if ($signature.Status -ne 'Valid') {
        throw "-RunSigned requires a validly signed consumer; $exe is $($signature.Status)."
    }
    Write-Host "tests: running signed $exe ($($signature.SignerCertificate.Subject))"
    # Require the sandbox session's own success line, not just exit 0: the exe at
    # this path must be the sandbox build.
    $ErrorActionPreference = 'Continue'
    $output = & $exe 2>&1 | ForEach-Object { "$_" }
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $output | Out-Host
    if ($exitCode -ne 0) { throw "signed v8jsi_smoke.exe failed (exit $exitCode)" }
    if (($output -join "`n") -notmatch 'test-harness: sandbox smoke OK') {
        throw 'signed v8jsi_smoke.exe did not run the sandbox session (not the sandbox build?)'
    }
    Write-Host 'tests: signed sandbox smoke OK'
    exit 0
}

$PkgDir = Get-PkgDir -RepoRoot $RepoRoot -PkgDir $PkgDir
$rid = Get-Rid -Platform $Platform
$pkg = Restore-V8Package -HarnessDir $harnessDir -PkgDir $PkgDir -Rid $rid

if ($UseV8Sandbox -and -not (Test-SandboxConsumerPlatform $Platform)) {
    Assert-SandboxUnsupported -HarnessDir $harnessDir -Configuration $Configuration `
        -Platform $Platform -Rid $rid -PkgProps $pkg.PkgProps -PkgTargets $pkg.PkgTargets
    Write-Host "tests: smoke OK (no sandbox leg for $Platform consumers)"
    exit 0
}

if ($UseV8Sandbox) {
    # Exercise only the package validation target: no compilation or binary copy.
    $msbuild = Get-MSBuild
    $invalidOutDir = Join-Path $harnessDir 'bin\invalid-sandbox-layout'
    $validation = & $msbuild (Join-Path $harnessDir 'consumer.vcxproj') `
        "/t:V8HostSandboxValidateOutputDirectory_$rid" `
        "/p:Configuration=$Configuration" "/p:Platform=$Platform" `
        "/p:V8JsiPackageProps=$($pkg.PkgProps)" `
        "/p:V8JsiPackageTargets=$($pkg.PkgTargets)" `
        "/p:V8JsiRuntimeIdentifier=$rid" /p:UseV8Sandbox=true `
        "/p:OutDir=$invalidOutDir" /nologo /v:minimal 2>&1
    $validationExit = $LASTEXITCODE
    $validation | Out-Host
    if ($validationExit -eq 0 -or ($validation -join "`n") -notmatch 'V8HOST001') {
        throw 'Expected invalid sandbox output directory to fail with V8HOST001.'
    }
    Write-Host 'tests: invalid sandbox output directory rejected (V8HOST001)'
}

$exe = Build-Consumer -HarnessDir $harnessDir -Configuration $Configuration `
    -Platform $Platform -Rid $rid -PkgProps $pkg.PkgProps -PkgTargets $pkg.PkgTargets `
    -UseV8Sandbox:$UseV8Sandbox

Write-Host "tests: running $exe"
if ($UseV8Sandbox -and $ReleaseTrustPayload) {
    # Native stderr must not become a terminating error here.
    $ErrorActionPreference = 'Continue'
    $output = & $exe 2>&1 | ForEach-Object { "$_" }
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $output | Out-Host
    if ($exitCode -eq 0) { throw 'The release-trust broker accepted an unsigned consumer.' }
    if (($output -join "`n") -notmatch 'client authentication failed category=\d+ \(0x800b0100\)') {
        throw 'Expected the broker to reject the unsigned consumer with TRUST_E_NOSIGNATURE (0x800b0100).'
    }
    Write-Host 'tests: unsigned consumer rejected by the release-trust broker (0x800b0100)'
    Write-Host 'tests: sign the consumer, then rerun with -UseV8Sandbox -RunSigned'
    exit 0
}
& $exe
if ($LASTEXITCODE -ne 0) { throw "v8jsi_smoke.exe failed (exit $LASTEXITCODE)" }
Write-Host "tests: smoke OK"
