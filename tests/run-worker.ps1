# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
# Registration-free, unsigned build-output worker coverage on native x64.
[CmdletBinding()]
param([string]$SandboxOut, [string]$EngineOut)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:caseCount = 0
$harnessDir = $PSScriptRoot
if (-not $harnessDir) { $harnessDir = Split-Path -Parent $MyInvocation.MyCommand.Definition }
if (-not $SandboxOut) { $SandboxOut = Join-Path $harnessDir '..\deps\chromium\out\sandbox-x64' }
if (-not $EngineOut) { $EngineOut = Join-Path $harnessDir '..\deps\nodejs\out\Release' }
$SandboxOut = (Resolve-Path $SandboxOut).Path
$EngineOut = (Resolve-Path $EngineOut).Path
$exe = Join-Path $SandboxOut 'v8host_cross_arch_tests.exe'
$dll = Join-Path $SandboxOut 'v8host.dll'
foreach ($file in @($exe, $dll, (Join-Path $SandboxOut 'v8jsisb.dll'),
                    (Join-Path $SandboxOut 'v8jsi.dll'),
                    (Join-Path $EngineOut 'mkv8snapshot.exe'))) {
    if (-not (Test-Path $file)) { throw "Required build output missing: $file" }
}
function Stop-OwnedProcessTree([int]$RootId) {
    # Snapshot ancestry while the owned root still exists; never terminate by name.
    $processes = @(Get-CimInstance Win32_Process)
    $owned = [System.Collections.Generic.List[int]]::new()
    $owned.Add($RootId)
    for ($i = 0; $i -lt $owned.Count; ++$i) {
        foreach ($child in $processes) {
            if ($child.ParentProcessId -eq $owned[$i] -and -not $owned.Contains([int]$child.ProcessId)) {
                $owned.Add([int]$child.ProcessId)
            }
        }
    }
    foreach ($processId in $owned) {
        Stop-Process -Id $processId -Force -ErrorAction SilentlyContinue
    }
}
function Invoke-WorkerProcess([string]$FilePath, [string[]]$Arguments, [int]$TimeoutSeconds = 120) {
    # Start-Process joins arguments; quote each one using Windows argv escaping.
    $quoted = @($Arguments | ForEach-Object {
        '"' + ($_ -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
    })
    $process = Start-Process -FilePath $FilePath -ArgumentList $quoted -PassThru -NoNewWindow
    try {
        # Cache the handle before waiting so Windows PowerShell retains ExitCode.
        $null = $process.Handle
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            Stop-OwnedProcessTree $process.Id
            throw "Worker process timed out after $TimeoutSeconds seconds: $Arguments"
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "Worker process failed: $Arguments (exit $($process.ExitCode))" }
    } finally {
        if (-not $process.HasExited) { Stop-OwnedProcessTree $process.Id }
        $process.Dispose()
    }
}
function Invoke-WorkerCase([string[]]$Arguments) {
    Invoke-WorkerProcess $exe $Arguments
    ++$script:caseCount
}
# Generate blobs with the actual engines: compatibility includes build and flags.
$builder = Join-Path $SandboxOut 'worker-builder.js'
$matched = Join-Path $SandboxOut 'worker-matched.bin'
$mismatched = Join-Path $SandboxOut 'worker-mismatched.bin'
try {
    Invoke-WorkerCase @('--scenario=consoleless-session', '--client-dll', $dll)
    foreach ($case in @('guest-async', 'guest-pressure', 'queued-close', 'wrong-snapshot-index')) {
        Invoke-WorkerCase @('--role=client', '--client-dll', $dll, "--scenario=$case")
    }
    Set-Content -Path $builder -Value 'globalThis.snapshotMarker = 42;' -Encoding ascii
    foreach ($pair in @(@('v8jsisb.dll', $matched), @('v8jsi.dll', $mismatched))) {
        Invoke-WorkerProcess (Join-Path $EngineOut 'mkv8snapshot.exe') @(
            '--engine', $pair[0], '--engine-dir', $SandboxOut, '--builder', $builder, '--out', $pair[1])
    }
    # Each worker prechecks its bound blob before any guest executes.
    # --expect-missing also expects rejection of an incompatible (present) blob.
    foreach ($case in @(@('worker-matched.bin', $false),
                        @('worker-mismatched.bin', $true),
                        @('worker-absent.bin', $true))) {
        $arguments = @('--role=client', '--client-dll', $dll, '--scenario=worker-snapshot', '--snapshot-path', $case[0])
        if ($case[1]) { $arguments += '--expect-missing' }
        Invoke-WorkerCase $arguments
    }
} finally {
    foreach ($file in @($builder, $matched, $mismatched)) {
        if (Test-Path $file) { Remove-Item -LiteralPath $file }
    }
}
Write-Host "worker coverage: $script:caseCount/$script:caseCount PASS"
