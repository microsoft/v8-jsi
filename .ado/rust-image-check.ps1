# TEMPORARY, ci-test branch only: checks that a CI image has the Rust setup
# needed to build V8 Temporal for v8jsi.dll and v8jsisb.dll. It replaces the
# real build in windows-jobs.yml on this branch.
#
# For each Rust target (x64, x86, ARM64, ARM64EC) it:
#   1. builds the vendored Node.js crates (temporal_capi) with the same
#      `cargo rustc --target <triple> --release --frozen` call as
#      deps/nodejs/deps/crates/cargo_build.py, outside a VS developer
#      environment, as MSBuild runs it;
#   2. links a Rust static library into a C program with MSVC for that target
#      and runs it where the host can execute it.
param(
    [Parameter(Mandatory)] [ValidateSet('x64', 'arm64')] [string] $ExpectedHost,
    [Parameter(Mandatory)] [string] $SourcesDirectory,
    [Parameter(Mandatory)] [string] $WorkDirectory
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Targets = [ordered]@{
    x64     = 'x86_64-pc-windows-msvc'
    x86     = 'i686-pc-windows-msvc'
    arm64   = 'aarch64-pc-windows-msvc'
    arm64ec = 'arm64ec-pc-windows-msvc'
}
# std::env::consts::ARCH for each target; the C side prints the same names.
$ArchNames = @{ x64 = 'x86_64'; x86 = 'x86'; arm64 = 'aarch64'; arm64ec = 'arm64ec' }
# ARM64EC images carry the x64 machine type.
$PeMachines = @{ x64 = 0x8664; x86 = 0x014C; arm64 = 0xAA64; arm64ec = 0x8664 }
# vcvarsall.bat argument per host and target.
$VcVarsArgs = @{
    x64   = @{ x64 = 'x64'; x86 = 'x64_x86'; arm64 = 'x64_arm64'; arm64ec = 'x64_arm64' }
    arm64 = @{ x64 = 'arm64_x64'; x86 = 'arm64_x86'; arm64 = 'arm64'; arm64ec = 'arm64' }
}
# Probe programs that must run on each host; x86 emulation on ARM64 only warns.
$MustRun = @{ x64 = @('x64', 'x86'); arm64 = @('arm64', 'arm64ec', 'x64') }
$TryRun = @{ x64 = @(); arm64 = @('x86') }

$HostTriple = $Targets[$ExpectedHost]
$RustRoot = Join-Path $env:SystemDrive 'Rust'
$CratesDir = Join-Path $SourcesDirectory 'deps\nodejs\deps\crates'
$Results = [System.Collections.Generic.List[object]]::new()

function Invoke-Native([string] $File, [string[]] $Arguments, [string] $Directory = $PWD.Path) {
    # Tools report progress on stderr; collect all output, then check the exit code.
    $ErrorActionPreference = 'Continue'
    Push-Location $Directory
    try {
        $output = @(& $File @Arguments 2>&1 | ForEach-Object { "$_" })
    } finally {
        Pop-Location
    }
    $output | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "$File $($Arguments -join ' ') failed with exit code $LASTEXITCODE" }
    return $output
}

function Invoke-Check([string] $Name, [scriptblock] $Body, [switch] $WarnOnly) {
    Write-Host "##[group]$Name"
    try {
        $detail = "$(& $Body)"
        $status = 'PASS'
    } catch {
        $detail = $_.Exception.Message
        $status = if ($WarnOnly) { 'WARN' } else { 'FAIL' }
        $type = if ($WarnOnly) { 'warning' } else { 'error' }
        Write-Host "##vso[task.logissue type=$type]${Name}: $detail"
    } finally {
        Write-Host '##[endgroup]'
    }
    Write-Host "[$status] ${Name}: $detail"
    $Results.Add([pscustomobject]@{ Status = $status; Check = $Name; Detail = $detail })
}

function Get-PeMachine([string] $Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    return [BitConverter]::ToUInt16($bytes, $pe + 4)
}

New-Item -Path $WorkDirectory -ItemType Directory -Force | Out-Null

# --- Image environment --------------------------------------------------------

Invoke-Check 'Host architecture' {
    $os = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    $expected = @{ x64 = 'X64'; arm64 = 'Arm64' }[$ExpectedHost]
    if ($os -ne $expected) { throw "OS architecture is $os, expected $expected" }
    "OS=$os process=$env:PROCESSOR_ARCHITECTURE user=$(whoami)"
}

Invoke-Check 'Machine environment' {
    $rustupHome = Join-Path $RustRoot '.rustup'
    $cargoBin = Join-Path $RustRoot '.image-cargo\bin'
    $machineRustupHome = [Environment]::GetEnvironmentVariable('RUSTUP_HOME', 'Machine')
    if ($machineRustupHome -ne $rustupHome) { throw "Machine RUSTUP_HOME is '$machineRustupHome', expected '$rustupHome'" }
    if ($env:RUSTUP_HOME -ne $rustupHome) { throw "Agent RUSTUP_HOME is '$env:RUSTUP_HOME', expected '$rustupHome'" }
    $machineCargoHome = [Environment]::GetEnvironmentVariable('CARGO_HOME', 'Machine')
    if ($machineCargoHome) { throw "Machine CARGO_HOME is set to '$machineCargoHome'; Cargo caches must stay per user" }
    if (-not (@($env:Path -split ';') | Where-Object { $_.TrimEnd('\') -ieq $cargoBin })) { throw "$cargoBin is not on the agent PATH" }
    "RUSTUP_HOME=$env:RUSTUP_HOME; $cargoBin on PATH; CARGO_HOME unset"
}

Invoke-Check 'Rust tools on PATH' {
    $found = foreach ($tool in 'rustup', 'rustc', 'cargo') {
        $cmd = Get-Command "$tool.exe" -CommandType Application | Select-Object -First 1
        if (-not $cmd) { throw "$tool.exe not found on PATH" }
        if (-not $cmd.Source.StartsWith($RustRoot, [StringComparison]::OrdinalIgnoreCase)) { throw "$tool.exe resolves to $($cmd.Source), outside $RustRoot" }
        $cmd.Source
    }
    $found -join '; '
}

Invoke-Check 'rustc host and minimum version' {
    $info = Invoke-Native 'rustc' @('-vV')
    $hostLine = ($info | Where-Object { $_ -like 'host: *' }) -replace '^host: ', ''
    $release = ($info | Where-Object { $_ -like 'release: *' }) -replace '^release: ', ''
    if ($hostLine -ne $HostTriple) { throw "rustc host is $hostLine, expected $HostTriple" }
    $minimum = [regex]::Match((Get-Content (Join-Path $CratesDir 'Cargo.toml') -Raw), 'rust-version\s*=\s*"([\d.]+)"').Groups[1].Value
    if (-not $minimum) { throw 'rust-version not found in the vendored Cargo.toml' }
    if ([version]($release -replace '-.*$', '') -lt [version]$minimum) { throw "rustc $release is older than the required $minimum" }
    $cargo = Invoke-Native 'cargo' @('--version')
    "rustc $release ($hostLine) >= $minimum; $cargo"
}

Invoke-Check 'Active toolchain' {
    $active = Invoke-Native 'rustup' @('show', 'active-toolchain')
    if ("$($active | Select-Object -First 1)" -notmatch "^stable-$([regex]::Escape($HostTriple))\b") { throw "Unexpected active toolchain: $active" }
    "$active"
}

Invoke-Check 'Installed targets' {
    $installed = Invoke-Native 'rustup' @('target', 'list', '--installed')
    $missing = @($Targets.Values | Where-Object { $installed -notcontains $_ })
    if ($missing.Count) { throw "Missing targets: $($missing -join ', ')" }
    $installed -join ', '
}

$VsPath = $null
Invoke-Check 'Visual Studio C++ tools' {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $script:VsPath = (& $vswhere -latest -products * -property installationPath | Select-Object -First 1)
    if (-not $script:VsPath) { throw 'vswhere found no Visual Studio installation' }
    $msvc = Get-ChildItem (Join-Path $script:VsPath 'VC\Tools\MSVC') -Directory | Sort-Object { [version]$_.Name } | Select-Object -Last 1
    $libs = (Get-ChildItem (Join-Path $msvc.FullName 'lib') -Directory).Name -join ','
    $hosts = (Get-ChildItem (Join-Path $msvc.FullName 'bin') -Directory).Name -join ','
    $ec = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64EC -property installationPath
    "$script:VsPath; MSVC $($msvc.Name); bin=$hosts; lib=$libs; ARM64EC component=$([bool]$ec)"
}

# --- Vendored Node.js crates (what Temporal builds) ----------------------------

$cratesTargetDir = Join-Path $WorkDirectory 'node-crates'
foreach ($arch in $Targets.Keys) {
    $triple = $Targets[$arch]
    Invoke-Check "node_crates $arch ($triple)" {
        $cargoArgs = @('rustc', '--target', $triple, '--target-dir', $cratesTargetDir, '--release', '--frozen')
        $note = ''
        try {
            $null = Invoke-Native 'cargo' $cargoArgs $CratesDir
        } catch {
            # Antivirus scans can briefly lock new archives (os error 32).
            # Retry once and report it, so a real failure still fails twice.
            Write-Host "##vso[task.logissue type=warning]node_crates ${arch}: first attempt failed ($($_.Exception.Message)); retrying once"
            $null = Invoke-Native 'cargo' $cargoArgs $CratesDir
            $note = ' (passed on retry; see the warning)'
        }
        $lib = Join-Path $cratesTargetDir "$triple\release\node_crates.lib"
        if (-not (Test-Path $lib)) { throw "$lib is missing" }
        "{0} ({1:N1} MB){2}" -f $lib, ((Get-Item $lib).Length / 1MB), $note
    }
}

# --- Rust static library linked by MSVC ----------------------------------------

$probeDir = Join-Path $WorkDirectory 'probe'
New-Item -Path (Join-Path $probeDir 'src') -ItemType Directory -Force | Out-Null
Set-Content -Path (Join-Path $probeDir 'Cargo.toml') -Encoding Ascii -Value @'
[package]
name = "rust_probe"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["staticlib"]
'@
Set-Content -Path (Join-Path $probeDir 'src\lib.rs') -Encoding Ascii -Value @'
use std::os::raw::c_char;

#[no_mangle]
pub extern "C" fn rust_probe_describe(buf: *mut c_char, len: usize) -> usize {
    let worker = std::thread::spawn(|| (1..=10u64).map(|n| n * n).sum::<u64>());
    let sum = worker.join().unwrap_or(0);
    let text = format!("rust={} sum={}", std::env::consts::ARCH, sum);
    if buf.is_null() || len <= text.len() {
        return 0;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(text.as_ptr(), buf as *mut u8, text.len());
        *buf.add(text.len()) = 0;
    }
    text.len()
}
'@
$mainC = Join-Path $probeDir 'main.c'
Set-Content -Path $mainC -Encoding Ascii -Value @'
#include <stddef.h>
#include <stdio.h>

size_t rust_probe_describe(char *buf, size_t len);

#if defined(_M_ARM64EC)
#define C_ARCH "arm64ec"
#elif defined(_M_ARM64)
#define C_ARCH "aarch64"
#elif defined(_M_X64)
#define C_ARCH "x86_64"
#elif defined(_M_IX86)
#define C_ARCH "x86"
#else
#error unknown architecture
#endif

int main(void) {
    char buf[256];
    if (rust_probe_describe(buf, sizeof buf) == 0) return 2;
    printf("c=%s %s\n", C_ARCH, buf);
    return 0;
}
'@
$vcvarsall = if ($VsPath) { Join-Path $VsPath 'VC\Auxiliary\Build\vcvarsall.bat' }
$probeTargetDir = Join-Path $WorkDirectory 'probe-target'

foreach ($arch in $Targets.Keys) {
    $triple = $Targets[$arch]
    $exe = Join-Path $WorkDirectory "probe-$arch.exe"
    Invoke-Check "MSVC link $arch ($triple)" {
        if (-not $vcvarsall) { throw 'No Visual Studio installation' }
        $cargoOut = Invoke-Native 'cargo' @('rustc', '--target', $triple, '--target-dir', $probeTargetDir, '--release', '--', '--print', 'native-static-libs') $probeDir
        $nativeLibs = ($cargoOut | Where-Object { $_ -match 'native-static-libs: ' } | Select-Object -Last 1) -replace '^.*native-static-libs: ', ''
        if (-not $nativeLibs) { throw 'rustc did not report native-static-libs' }
        $lib = Join-Path $probeTargetDir "$triple\release\rust_probe.lib"
        $obj = Join-Path $WorkDirectory "probe-$arch.obj"
        $clFlags = if ($arch -eq 'arm64ec') { '/arm64EC' } else { '' }
        $linkFlags = if ($arch -eq 'arm64ec') { '/MACHINE:ARM64EC' } else { '' }
        $buildCmd = Join-Path $WorkDirectory "probe-$arch.cmd"
        Set-Content -Path $buildCmd -Encoding Ascii -Value @(
            '@echo off'
            "call `"$vcvarsall`" $($VcVarsArgs[$ExpectedHost][$arch]) >nul || exit /b 1"
            'echo VSCMD_ARG_HOST_ARCH=%VSCMD_ARG_HOST_ARCH% VSCMD_ARG_TGT_ARCH=%VSCMD_ARG_TGT_ARCH%'
            "cl /nologo /W4 /WX /MD $clFlags /c `"$mainC`" /Fo`"$obj`" || exit /b 1"
            "link /nologo $linkFlags /OUT:`"$exe`" `"$obj`" `"$lib`" $nativeLibs || exit /b 1"
        )
        $null = Invoke-Native 'cmd.exe' @('/d', '/c', $buildCmd)
        $machine = Get-PeMachine $exe
        if ($machine -ne $PeMachines[$arch]) { throw ('{0} has PE machine 0x{1:X4}, expected 0x{2:X4}' -f $exe, $machine, $PeMachines[$arch]) }
        "linked $exe (machine 0x{0:X4}); native libs: $nativeLibs" -f $machine
    }

    $runRequired = $MustRun[$ExpectedHost] -contains $arch
    if (-not $runRequired -and $TryRun[$ExpectedHost] -notcontains $arch) { continue }
    Invoke-Check "Run probe $arch" {
        if (-not (Test-Path $exe)) { throw "$exe was not built" }
        $out = "$(Invoke-Native $exe @())".Trim()
        $expected = "c=$($ArchNames[$arch]) rust=$($ArchNames[$arch]) sum=385"
        if ($out -ne $expected) { throw "Output '$out', expected '$expected'" }
        $out
    } -WarnOnly:(-not $runRequired)
}

# --- Summary --------------------------------------------------------------------

$table = $Results | Format-Table -AutoSize -Wrap | Out-String -Width 4096
Write-Host $table
$summary = Join-Path $WorkDirectory 'rust-image-check.md'
$lines = @("# Rust image check: $ExpectedHost", '', '| Status | Check | Detail |', '|---|---|---|')
$lines += $Results | ForEach-Object { "| $($_.Status) | $($_.Check) | $(($_.Detail -replace '\|', '\|') -replace '\r?\n', ' ') |" }
Set-Content -Path $summary -Encoding UTF8 -Value $lines
Write-Host "##vso[task.uploadsummary]$summary"

$failed = @($Results | Where-Object Status -eq 'FAIL')
if ($failed.Count) { throw "$($failed.Count) Rust image check(s) failed: $(($failed.Check) -join '; ')" }
Write-Host "All required Rust image checks passed on $ExpectedHost ($(@($Results | Where-Object Status -eq 'WARN').Count) warning(s))."
