param(
    [Parameter(Mandatory)][string] $RepoRoot,
    [ValidateSet('Release', 'Debug')][string] $Configuration = 'Release',
    [switch] $Sandbox,
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$out = Join-Path $RepoRoot "deps/nodejs/out/$Configuration"
$launcher = Join-Path $out 'cetrun.exe'
$jsi = Join-Path $out 'v8jsi_test.exe'
$nodeApi = Join-Path $out 'node_api_tests.exe'
$node = Join-Path $RepoRoot 'tooling/node.exe'
$contract = Join-Path $RepoRoot 'scripts/cetrun.test.ts'
$broker = Join-Path $RepoRoot 'deps/chromium/out/sandbox-x64/v8sbhost_broker_tests.exe'
$engine = if ($Sandbox) { 'v8jsi2sb.dll' } else { 'v8jsi2.dll' }
foreach ($file in @($launcher, $jsi, $nodeApi, $node, $contract,
    (Join-Path $out $engine), (Join-Path $out 'node_lite.exe'), (Join-Path $out 'test-js-files'))) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Required CET payload missing: $file" }
}
if ($Sandbox -and -not (Test-Path -LiteralPath $broker)) { throw "Required CET payload missing: $broker" }

function Invoke-Row([string] $Label, [string] $Exe, [string[]] $Arguments, [switch] $RequireDiagnostic) {
    Write-Host "=== $Label ==="
    Write-Host "> $Exe $($Arguments -join ' ')"
    Write-Host "controls: require_cet=$env:CETRUN_REQUIRE_CET node_api_cet=$env:V8JSI_TEST_CET v8_flags='$env:V8JSI_TEST_V8_FLAGS'"
    if ($DryRun) { return }
    $skipped = $false
    $diagnostic = $false
    & $Exe @Arguments 2>&1 | ForEach-Object {
        $line = $_.ToString()
        Write-Host $line
        if ($line -match 'SKIP: user CET not available') { $skipped = $true }
        if ($line -match 'cetrun: .*user_cet=yes forced_unavailable=0') { $diagnostic = $true }
    }
    $code = $LASTEXITCODE
    if ($code -ne 0) { throw "$Label FAILED (exit $code)" }
    if ($skipped) { throw "$Label skipped: user CET is required" }
    if ($RequireDiagnostic -and -not $diagnostic) { throw "$Label did not confirm user CET availability" }
}

$savedEnv = @{}
$controls = @('CETRUN_REQUIRE_CET', 'CETRUN_TIMEOUT_MS', 'CETRUN_TEST_OUT', 'V8JSI_TEST_CET', 'V8JSI_TEST_V8_FLAGS')
Get-ChildItem Env: | Where-Object { $_.Name -like 'CETRUN_*' -or $_.Name -like 'CODEQL_*' -or $_.Name -in $controls } | ForEach-Object {
    $savedEnv[$_.Name] = $_.Value
}
try {
    Write-Host 'Runtime tests only; static header, Stela, Source Link, and BinSkim gates remain on the build agents.'
    Get-ChildItem Env: | Where-Object { $_.Name -like 'CETRUN_*' -or $_.Name -in $controls } | Remove-Item
    $env:CETRUN_REQUIRE_CET = '1'
    $env:CETRUN_TIMEOUT_MS = '1800000'
    # A real strict child must succeed before any suite can report a skip or pass.
    Invoke-Row 'Required user CET probe' $launcher @('strict', $jsi, '--cetrun-child', 'policy') -RequireDiagnostic
    Invoke-Row 'Ordinary full JSI' $jsi @('--gtest_brief=1')
    Invoke-Row 'Ordinary full Node-API' $nodeApi @('--gtest_brief=1')
    $env:CETRUN_TEST_OUT = $out
    Invoke-Row 'CET launcher contract tests' $node @('--test', $contract)
    Remove-Item Env:CETRUN_TEST_OUT
    Invoke-Row 'CET off: full JSI' $launcher @('off', $jsi, '--gtest_brief=1') -RequireDiagnostic
    $env:V8JSI_TEST_V8_FLAGS = '--allow-natives-syntax'
    Invoke-Row 'Strict CET: full JSI' $launcher @('strict', $jsi, '--gtest_brief=1') -RequireDiagnostic
    if (-not $Sandbox) {
        $interpreter = '--allow-natives-syntax --no-sparkplug --no-maglev --no-turbofan'
        $rows = @(
            @('interpreter', 'strict', $interpreter, '*CetShadowStack.*'),
            @('jitless', 'strict', '--allow-natives-syntax --jitless', '*CetShadowStack.*'),
            @('native-stack', 'strict', "$interpreter --interpreted-frames-native-stack", '*CetShadowStack.*'),
            @('Wasm off', 'off', '--allow-natives-syntax', '*CetShadowStack.Jspi*:*CetShadowStack.Wasm*'),
            @('Wasm compat', 'compat', '--allow-natives-syntax', '*CetShadowStack.Jspi*:*CetShadowStack.Wasm*'),
            @('compatible code', 'off', '--allow-natives-syntax --cet-compatible', '*CetShadowStack.*:*JsiAbiInspector.Cet*'),
            @('JSPI override', 'strict', '--allow-natives-syntax --no-cet-compatible', '*CetShadowStack.Jspi*')
        )
        foreach ($row in $rows) {
            $env:V8JSI_TEST_V8_FLAGS = $row[2]
            Invoke-Row "CET $($row[0])" $launcher @($row[1], $jsi, "--gtest_filter=$($row[3])",
                '--gtest_fail_if_no_test_selected=1', '--gtest_brief=1') -RequireDiagnostic
        }
    }
    Remove-Item Env:V8JSI_TEST_V8_FLAGS
    $env:V8JSI_TEST_CET = 'strict'
    Invoke-Row 'Strict CET: per-child Node-API' $nodeApi @('--gtest_brief=1')
    Remove-Item Env:V8JSI_TEST_CET
    if ($Sandbox) {
        # Keep CodeQL tracing out of LPAC workers, matching the sandbox build job.
        Get-ChildItem Env:CODEQL_* | Where-Object Name -notin 'CODEQL_DIST', 'CODEQL_PLATFORM' | Remove-Item
        # Downloaded files do not retain the build agent's LPAC read/execute ACLs.
        Invoke-Row 'Sandbox LPAC payload access' 'icacls.exe' @((Split-Path $broker), '/grant',
            '*S-1-15-2-1:(OI)(CI)(RX)', '*S-1-15-2-2:(OI)(CI)(RX)', '/T')
        Invoke-Row 'Sandbox CET catch stress' $broker @('--suite=client', '--case=cet-catch-stress')
        foreach ($suite in @('topology', 'security', 'boundary', 'failure', 'client', 'launch')) {
            Invoke-Row "Sandbox broker $suite" $broker @("--suite=$suite")
        }
    }
    Write-Host "CET runtime rows passed ($Configuration, sandbox=$Sandbox, dry-run=$DryRun)"
} finally {
    Get-ChildItem Env: | Where-Object { $_.Name -like 'CETRUN_*' -or $_.Name -in $controls } | Remove-Item
    foreach ($name in $savedEnv.Keys) { Set-Item "Env:$name" $savedEnv[$name] }
}
