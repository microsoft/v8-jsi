# Temporary check of the Microsoft Rust toolchain that RustInstaller@1 installs
# on the Windows agent images. Not for merging.
#
# With -ReportOnly it lists the Rust tools visible before the installation.
# Otherwise it checks that the Microsoft toolchain wins on PATH, has the Rust/C2
# backend and the standard libraries, compiles the vendored Node.js crates
# offline, and links a DLL that BinSkim's BA2030 rule accepts. Failed required
# checks fail the script; informational checks only report.
[CmdletBinding()]
param(
    [switch] $ReportOnly,
    [string] $HostTriple,
    [string] $ExtraTargets = '',
    [ValidateSet('amd64', 'arm64')]
    [string] $VcArch = 'amd64',
    [string] $WorkDir = (Join-Path ([IO.Path]::GetTempPath()) 'msrust-check'),
    [string] $ReportDir = (Join-Path ([IO.Path]::GetTempPath()) 'msrust-check-report'),
    [string] $BinSkimExe = '',
    # The internal BinSkim that Guardian runs in 1ES PT; the public package has no BA2030.
    [string] $BinSkimPackage = 'Microsoft.CodeAnalysis.BinSkim.Internal',
    [string] $BinSkimVersion = '4.4.9.13',
    [string] $BinSkimSource = 'https://pkgs.dev.azure.com/office/_packaging/Guardian1ESPTUpstreamOrgFeed/nuget/v3/index.json'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
# The image's public toolchain (windows-install-public-rust-toolchain).
$publicRustRoot = Join-Path $env:SystemDrive 'Rust'

$probeManifest = @'
[package]
name = "msrust_probe"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]

[profile.release]
debug = true
'@

$probeSource = @'
use std::collections::HashMap;

// Uses collections, formatting, and allocation so that the DLL links the
// precompiled std, core, and alloc objects that BA2030 inspects.
#[no_mangle]
pub extern "C" fn msrust_probe(count: u32) -> u64 {
    let mut map = HashMap::new();
    for i in 0..count {
        map.insert(i, format!("value-{i}"));
    }
    map.values().map(|s| s.len() as u64).sum()
}
'@

function Show-RustEnvironment {
    foreach ($tool in 'cargo', 'rustc', 'rustup', 'msrustup') {
        $found = @(Get-Command $tool -All -CommandType Application -ErrorAction SilentlyContinue)
        if ($found.Count -eq 0) { Write-Host "  ${tool}: not on PATH" }
        foreach ($f in $found) { Write-Host "  ${tool}: $($f.Source)" }
    }
    foreach ($name in 'RUSTUP_HOME', 'RUSTUP_TOOLCHAIN', 'CARGO_HOME', 'MSRUSTUP_HOME', 'MSRUSTUP_TOOLCHAIN', 'MSRUSTUP_FEED_URL') {
        Write-Host ('  {0}={1}' -f $name, [Environment]::GetEnvironmentVariable($name))
    }
}

if ($ReportOnly) {
    Write-Host 'Rust tools before the installation:'
    Show-RustEnvironment
    if (Get-Command rustc -CommandType Application -ErrorAction SilentlyContinue) { rustc -vV }
    exit 0
}

if (-not $HostTriple) { throw '-HostTriple is required unless -ReportOnly is set.' }

# Runs a native command, echoes its output, and returns the output lines.
function Invoke-Native([string] $File, [string[]] $Arguments) {
    Write-Host "> $File $($Arguments -join ' ')"
    $ErrorActionPreference = 'Continue'
    $output = @(& $File @Arguments 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }
    if ($code -ne 0) { throw "$File exited with code $code" }
    return $output
}

$results = [Collections.Generic.List[object]]::new()

function Invoke-Check([string] $Name, [scriptblock] $Body, [switch] $Informational) {
    Write-Host "##[group]$Name"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $status = if ($Informational) { 'info' } else { 'pass' }
    try {
        $detail = @(& $Body) -join '; '
    } catch {
        $status = if ($Informational) { 'warn' } else { 'fail' }
        $detail = $_.Exception.Message
    }
    $timer.Stop()
    Write-Host '##[endgroup]'
    switch ($status) {
        'fail' { Write-Host "##[error]${Name}: $detail" }
        'warn' { Write-Host "##[warning]${Name}: $detail" }
        default { Write-Host "${Name}: $detail" }
    }
    $results.Add([pscustomobject]@{
        Check   = $Name
        Status  = $status
        Seconds = [math]::Round($timer.Elapsed.TotalSeconds, 1)
        Detail  = $detail
    })
}

function Test-StdLib([string] $Target) {
    if (-not $script:sysroot) { throw 'the toolchain sysroot is unknown' }
    $lib = Join-Path $script:sysroot "lib\rustlib\$Target\lib"
    $std = Get-ChildItem -LiteralPath $lib -Filter 'libstd-*.rlib' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $std) { throw "no standard library for $Target in $lib" }
    "$Target ($($std.Name))"
}

function Get-BinSkim {
    if ($BinSkimExe) { return $BinSkimExe }
    $nuget = Get-Command nuget.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $nuget) { throw 'nuget.exe is not on PATH' }
    $dir = Join-Path $WorkDir 'binskim'
    $null = Invoke-Native $nuget.Source @('install', $BinSkimPackage, '-Version', $BinSkimVersion,
        '-Source', $BinSkimSource, '-OutputDirectory', $dir, '-NonInteractive')
    $exe = Get-ChildItem -LiteralPath (Join-Path $dir "$BinSkimPackage.$BinSkimVersion\tools") -Recurse -Filter 'BinSkim.exe' |
        Where-Object { $_.Directory.Name -eq 'win-x64' } | Select-Object -First 1
    if (-not $exe) { throw 'BinSkim.exe is not in the package' }
    $exe.FullName
}

# Results of one rule from the BinSkim SARIF. SARIF omits the default kind
# ("fail") and level ("warning"). One rule ID can have several rule entries
# (BA2030 checks PE, ELF, and DWARF separately), each with its own messages.
function Get-SarifResults([string] $RuleId) {
    $run = $script:binskimRun
    $rules = @($run.tool.driver.rules) + @($run.tool.PSObject.Properties['extensions']?.Value | ForEach-Object { $_.rules })
    $ruleEntries = @($rules | Where-Object { $_ -and $_.id -eq $RuleId })
    foreach ($r in @($run.results)) {
        if ($r.ruleId -ne $RuleId) { continue }
        $text = $null
        $messageId = $r.message.PSObject.Properties['id']?.Value
        foreach ($entry in $ruleEntries) {
            $strings = $entry.PSObject.Properties['messageStrings']?.Value
            if ($messageId -and $strings -and $strings.PSObject.Properties[$messageId]) { $text = $strings.$messageId.text; break }
        }
        if (-not $text) { $text = $r.message.PSObject.Properties['text']?.Value }
        if (-not $text) { $text = "[$messageId]" }
        $arguments = @($r.message.PSObject.Properties['arguments']?.Value)
        for ($i = 0; $i -lt $arguments.Count; $i++) { $text = "$text".Replace("{$i}", "$($arguments[$i])") }
        [pscustomobject]@{
            Rule    = $RuleId
            Kind    = $r.PSObject.Properties['kind']?.Value ?? 'fail'
            Level   = $r.PSObject.Properties['level']?.Value ?? 'warning'
            Message = $text
        }
    }
}

Write-Host 'Rust tools after the installation:'
Show-RustEnvironment

$script:sysroot = $null
$script:probeDll = $null
$script:binskimRun = $null

Invoke-Check 'Microsoft Rust is first on PATH' {
    foreach ($tool in 'cargo', 'rustc') {
        $first = Get-Command $tool -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $first) { throw "$tool is not on PATH" }
        if ($first.Source.StartsWith("$publicRustRoot\", [StringComparison]::OrdinalIgnoreCase)) {
            throw "$tool resolves to the image's public Rust: $($first.Source)"
        }
        "$tool=$($first.Source)"
    }
}

Invoke-Check 'rustc has the Rust/C2 backend for the host' {
    $info = @{}
    foreach ($line in (Invoke-Native 'rustc' @('-vV'))) {
        if ($line -match '^([^:]+):\s*(.*)$') { $info[$Matches[1].Trim()] = $Matches[2].Trim() }
    }
    if ($info['host'] -ne $HostTriple) { throw "rustc host is '$($info['host'])', expected '$HostTriple'" }
    $arch = $HostTriple.Split('-')[0]
    foreach ($key in 'codegen-utc-hash', "$arch-utc-builder-version") {
        if (-not $info.ContainsKey($key)) { throw "rustc -vV has no '$key', so this is not the Rust/C2 toolchain" }
    }
    "rustc $($info['release']), $arch C2 builder $($info["$arch-utc-builder-version"]), msvc-drop $($info['msvc-drop'])"
}

Invoke-Check 'Node.js configure accepts cargo and rustc' {
    $manifest = Get-Content -LiteralPath (Join-Path $repoRoot 'deps\nodejs\deps\crates\Cargo.toml') -Raw
    if ($manifest -notmatch '(?m)^rust-version\s*=\s*"(\d+)\.(\d+)') { throw 'no rust-version in the vendored Cargo.toml' }
    $minimum = [version]"$($Matches[1]).$($Matches[2])"
    foreach ($tool in 'cargo', 'rustc') {
        # The same patterns as get_cargo_version and get_rustc_version in deps/nodejs/configure.py.
        $line = (Invoke-Native $tool @('--version')) | Select-Object -First 1
        if ($line -notmatch "^$tool ([0-9]+)\.([0-9]+)\.([0-9]+)") { throw "configure.py would not parse '$line'" }
        if ([version]"$($Matches[1]).$($Matches[2])" -lt $minimum) { throw "'$line' is older than rust-version $minimum" }
        $line
    }
}

Invoke-Check 'Host standard library' {
    $script:sysroot = (Invoke-Native 'rustc' @('--print', 'sysroot')) | Select-Object -Last 1
    Test-StdLib $HostTriple
}

foreach ($target in ($ExtraTargets -split '\s+' | Where-Object { $_ })) {
    Invoke-Check "Standard library for $target" -Informational { Test-StdLib $target }
}

Invoke-Check 'Toolchain size on disk' -Informational {
    if (-not $script:sysroot) { throw 'the toolchain sysroot is unknown' }
    $bytes = (Get-ChildItem -LiteralPath $script:sysroot -Recurse -File -Force -ErrorAction SilentlyContinue |
        Measure-Object -Property Length -Sum).Sum
    '{0:N0} MB in {1}' -f ($bytes / 1MB), $script:sysroot
}

# Node.js's vcbuild.bat runs the build, and so Cargo, in this environment.
Invoke-Check "Visual Studio $VcArch environment" {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    # The same requirement as deps/nodejs/tools/msvs/vswhere_usability_wrapper.cmd.
    $component = if ($VcArch -eq 'arm64') { 'Microsoft.VisualStudio.Component.VC.Tools.ARM64' } else { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
    $vs = & $vswhere -latest -prerelease -products * -requires $component -property installationPath
    if (-not $vs) { throw "no Visual Studio with $component" }
    New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
    $cmdFile = Join-Path $WorkDir 'vcvars-env.cmd'
    Set-Content -LiteralPath $cmdFile -Encoding ascii -Value "@call `"$vs\VC\Auxiliary\Build\vcvarsall.bat`" $VcArch >nul 2>&1 || exit /b 1`r`n@set"
    $lines = & cmd.exe /d /c $cmdFile
    if ($LASTEXITCODE -ne 0) { throw "vcvarsall.bat $VcArch failed" }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
    }
    "$vs, target $env:VSCMD_ARG_TGT_ARCH"
}

Invoke-Check 'Vendored Node.js crates compile offline' {
    $targetDir = Join-Path $WorkDir 'node-crates'
    Push-Location (Join-Path $repoRoot 'deps\nodejs\deps\crates')
    try {
        # The same command as deps/nodejs/deps/crates/cargo_build.py.
        $null = Invoke-Native 'cargo' @('rustc', '--target', $HostTriple, '--release', '--frozen', '--target-dir', $targetDir)
    } finally {
        Pop-Location
    }
    $lib = Get-Item -LiteralPath (Join-Path $targetDir "$HostTriple\release\node_crates.lib")
    '{0} ({1:N1} MB)' -f $lib.FullName, ($lib.Length / 1MB)
}

Invoke-Check 'Link a Rust DLL with debug information' {
    $probe = Join-Path $WorkDir 'probe'
    New-Item -ItemType Directory -Force -Path (Join-Path $probe 'src') | Out-Null
    Set-Content -LiteralPath (Join-Path $probe 'Cargo.toml') -Encoding utf8NoBOM -Value $probeManifest
    Set-Content -LiteralPath (Join-Path $probe 'src\lib.rs') -Encoding utf8NoBOM -Value $probeSource
    Push-Location $probe
    try {
        $null = Invoke-Native 'cargo' @('build', '--release', '--offline', '--target', $HostTriple, '--target-dir', (Join-Path $probe 'target'))
    } finally {
        Pop-Location
    }
    $dll = Get-Item -LiteralPath (Join-Path $probe "target\$HostTriple\release\msrust_probe.dll")
    $null = Get-Item -LiteralPath ([IO.Path]::ChangeExtension($dll.FullName, '.pdb'))
    $script:probeDll = $dll.FullName
    $script:probeDll
}

Invoke-Check 'BinSkim BA2030 accepts the Rust DLL' {
    if (-not $script:probeDll) { throw 'no DLL to analyze' }
    $exe = Get-BinSkim
    New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null
    $sarifPath = Join-Path $ReportDir "binskim-$HostTriple.sarif"
    Remove-Item -LiteralPath $sarifPath -ErrorAction SilentlyContinue
    try {
        $null = Invoke-Native $exe @('analyze', '--config', 'default', '--kind', 'Fail;Pass;NotApplicable',
            '--level', 'Error;Warning;Note;None', '--output', $sarifPath, '--hashes', '--disable-telemetry', 'True', $script:probeDll)
    } catch {
        # BinSkim exits non-zero when it reports a failure; the SARIF holds the results.
        Write-Host $_.Exception.Message
    }
    if (-not (Test-Path -LiteralPath $sarifPath)) { throw 'BinSkim wrote no SARIF' }
    $script:binskimRun = (Get-Content -LiteralPath $sarifPath -Raw | ConvertFrom-Json -Depth 100).runs[0]
    $ba2030 = @(Get-SarifResults 'BA2030')
    foreach ($r in $ba2030) { Write-Host "BA2030 $($r.Kind) ($($r.Level)): $($r.Message)" }
    # The ELF and DWARF variants of BA2030 are not applicable to a PE file.
    $failed = @($ba2030 | Where-Object Kind -eq 'fail')
    if ($failed.Count -gt 0) { throw "BA2030 failed: $($failed[0].Message)" }
    if (@($ba2030 | Where-Object Kind -eq 'pass').Count -eq 0) { throw 'BinSkim reported no BA2030 pass, so the check is inconclusive' }
    'BA2030 pass'
}

Invoke-Check 'BinSkim BA2014 (stack protection)' -Informational {
    if (-not $script:binskimRun) { throw 'no BinSkim results' }
    $ba2014 = @(Get-SarifResults 'BA2014')
    if ($ba2014.Count -eq 0) { throw 'BinSkim reported no BA2014 result' }
    foreach ($r in $ba2014) {
        $message = "$($r.Message)"
        if ($message.Length -gt 3000) { $message = $message.Substring(0, 3000) + ' ... (full text in the SARIF)' }
        Write-Host "BA2014 $($r.Kind) ($($r.Level)): $message"
    }
    "BA2014 $(($ba2014 | ForEach-Object Kind) -join ', ')"
}

Invoke-Check 'Other BinSkim failures' -Informational {
    if (-not $script:binskimRun) { throw 'no BinSkim results' }
    $ids = @($script:binskimRun.results | ForEach-Object ruleId | Sort-Object -Unique |
        Where-Object { $_ -notin 'BA2030', 'BA2014' })
    $failed = foreach ($id in $ids) {
        Get-SarifResults $id | Where-Object Kind -eq 'fail' | ForEach-Object {
            Write-Host "$id $($_.Level): $($_.Message)"
            "$id ($($_.Level))"
        }
    }
    if (-not $failed) { return 'none' }
    $failed
}

Write-Host ($results | Format-Table -AutoSize -Wrap Check, Status, Seconds, Detail | Out-String -Width 220)
New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $ReportDir "summary-$HostTriple.json") -Encoding utf8NoBOM
if (@($results | Where-Object Status -eq 'fail').Count -gt 0) { exit 1 }
