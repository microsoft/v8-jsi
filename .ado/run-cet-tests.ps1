# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [Parameter(Mandatory)][string] $RepoRoot,
    [Parameter(Mandatory)][string] $Archive,
    [string] $LegacyArchive,
    [ValidateSet('Release', 'Debug')][string] $Configuration = 'Release',
    [switch] $Sandbox
)

function Invoke-CetCommand([string] $Exe, [string[]] $Arguments, [switch] $Broker) {
    $unavailable = [Collections.Generic.List[string]]::new()
    & $Exe @Arguments 2>&1 | ForEach-Object {
        $line = $_.ToString()
        Write-Output $line
        if ($line.Contains('SKIP: user CET not available')) { $unavailable.Add($line) }
    }
    if ($LASTEXITCODE -ne 0) { throw "CET command failed: $Exe (exit $LASTEXITCODE)" }
    if ($Broker -and $unavailable.Count) { throw 'Broker skipped: user CET is required' }
}

function Assert-LegacyStartup([string] $Output, [string] $Expected, [int] $ShadowStacks) {
    $lines = $Output -split '\r?\n'
    $flags = @($lines | Where-Object { $_ -cmatch '^--(?:no-)?cet-compatible$' })
    $summary = "Startup shadow-stacks=$ShadowStacks callbacks=1"
    if ($flags.Count -ne 1 -or $flags[0] -cne $Expected -or @($lines | Where-Object { $_ -ceq $summary }).Count -ne 1) {
        throw 'Legacy startup policy mismatch'
    }
}

function Invoke-LegacyStartup([string] $Directory, [string] $Mode, [string] $Flag, [string] $Expected) {
    $info = [Diagnostics.ProcessStartInfo]::new((Join-Path $Directory 'cetrun_legacy.exe'))
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.WorkingDirectory = $Directory
    foreach ($arg in $Mode, (Join-Path $Directory 'v8jsi_legacy_startup_tests.exe')) { $info.ArgumentList.Add($arg) }
    if ($Flag) { $info.ArgumentList.Add($Flag) }
    $child = [Diagnostics.Process]::Start($info)
    try {
        $stdout = $child.StandardOutput.ReadToEndAsync()
        $stderr = $child.StandardError.ReadToEndAsync()
        if (-not $child.WaitForExit(90000)) { $child.Kill($true); throw 'Legacy startup timed out' }
        $output = $stdout.GetAwaiter().GetResult()
        Write-Output $output
        Write-Output $stderr.GetAwaiter().GetResult()
        if ($child.ExitCode -ne 0) { throw "Legacy startup failed (exit $($child.ExitCode))" }
        Assert-LegacyStartup $output $Expected ([int]($Mode -eq 'strict'))
    } finally { $child.Dispose() }
}

if ($MyInvocation.InvocationName -ne '.') {
    $ErrorActionPreference = 'Stop'
    $PSNativeCommandUseErrorActionPreference = $false
    if (-not $Sandbox -and -not $LegacyArchive) { throw 'Legacy startup archive is required' }
    $controls = '^(?:CETRUN_|V8JSI_TEST_|GTEST_|V8JSI_CET_TESTS_DEFERRED$)'
    $saved = @{}
    $changed = @(Get-ChildItem Env: | Where-Object {
        $_.Name -match $controls -or
        ($_.Name -like 'CODEQL_*' -and $_.Name -notin 'CODEQL_DIST', 'CODEQL_PLATFORM')
    })
    foreach ($entry in $changed) { $saved[$entry.Name] = $entry.Value }
    try {
        $changed | Remove-Item
        foreach ($tar in @($Archive, $LegacyArchive) | Where-Object { $_ }) {
            Invoke-CetCommand 'tar.exe' @('-xf', $tar, '-C', $RepoRoot)
        }
        $out = Join-Path $RepoRoot "deps/nodejs/out/$Configuration"
        $node = Join-Path $RepoRoot 'tooling/node.exe'
        $arguments = @((Join-Path $RepoRoot 'scripts/build.ts'), '--test-cet-runtime', $out)
        if ($Sandbox) { $arguments += '--build-v8jsisb' }
        Invoke-CetCommand $node $arguments
        if ($Sandbox) {
            $sandboxOut = Join-Path $RepoRoot 'deps/chromium/out/sandbox-x64'
            # Downloads lose LPAC ACLs; grant only read/execute on the extracted sandbox tree.
            Invoke-CetCommand 'icacls.exe' @($sandboxOut, '/grant',
                '*S-1-15-2-1:(OI)(CI)(RX)', '*S-1-15-2-2:(OI)(CI)(RX)', '/T', '/Q')
            $broker = Join-Path $sandboxOut 'v8sbhost_broker_tests.exe'
            Invoke-CetCommand $broker @('--suite=client', '--case=cet-catch-stress') -Broker
            foreach ($suite in 'topology', 'security', 'boundary', 'failure', 'client', 'launch') {
                Invoke-CetCommand $broker @("--suite=$suite") -Broker
            }
        } else {
            $env:CETRUN_REQUIRE_CET = '1'
            $env:CETRUN_TIMEOUT_MS = '60000'
            $legacy = Join-Path $RepoRoot "legacy/$Configuration"
            Invoke-LegacyStartup $legacy 'off' '' '--no-cet-compatible'
            Invoke-LegacyStartup $legacy 'off' '--cet-compatible' '--cet-compatible'
            Invoke-LegacyStartup $legacy 'strict' '' '--cet-compatible'
            Invoke-LegacyStartup $legacy 'strict' '--no-cet-compatible' '--no-cet-compatible'
        }
    } finally {
        Get-ChildItem Env: | Where-Object { $_.Name -match $controls } | Remove-Item
        foreach ($name in $saved.Keys) { Set-Item "Env:$name" $saved[$name] }
    }
}
