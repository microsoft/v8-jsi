# Installs an image-managed public Rust toolchain with the targets v8-jsi
# builds, then checks each target. Embedded base64-encoded as the "Script" of a
# windows-powershell-invokecommand artifact in both image JSONs; see README.md,
# "Rust", for why and "WinGet during image builds" for how to regenerate the
# JSON entries.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$ToolchainVersion = 'stable'
# V8's Temporal crates build as a Rust static library that MSVC links, so each
# target needs only its standard library. Agents cannot add targets later: the
# image-managed toolchain is read-only for them.
$Targets = @(
    'x86_64-pc-windows-msvc'    # x64
    'i686-pc-windows-msvc'      # x86 (Win32), cross-built on the x64 image
    'aarch64-pc-windows-msvc'   # ARM64
    'arm64ec-pc-windows-msvc'   # ARM64EC
)
$RustRoot = Join-Path $env:SystemDrive 'Rust'
$EnvironmentKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment'

$RustupHome = Join-Path $RustRoot '.rustup'
# CARGO_HOME is used only to place the rustup proxies. It is not persisted, so
# Cargo caches stay per user.
$InstallerCargoHome = Join-Path $RustRoot '.image-cargo'
$CargoBin = Join-Path $InstallerCargoHome 'bin'

function Invoke-Native([string] $File, [string[]] $Arguments) {
    # rustup reports progress on stderr; it must not become a terminating error.
    # Collect all output before checking the exit code: stopping a native
    # command's pipeline early (e.g. Select-Object -First) loses its exit code.
    $ErrorActionPreference = 'Continue'
    $output = @(& $File @Arguments 2>&1 | ForEach-Object { "$_" })
    $output | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "$File $($Arguments -join ' ') failed with exit code $LASTEXITCODE" }
    return $output
}

function Invoke-Download([string] $Uri, [string] $OutFile) {
    for ($attempt = 1; ; $attempt++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $OutFile
            return
        } catch {
            if ($attempt -ge 5) { throw }
            Write-Host "Download of $Uri failed (attempt $attempt): $($_.Exception.Message)"
            Start-Sleep -Seconds (5 * $attempt)
        }
    }
}

# PowerShell runs as emulated x64 on the ARM64 builder; RuntimeInformation
# still reports the OS architecture.
$hostTriple = switch ([System.Runtime.InteropServices.RuntimeInformation,mscorlib]::OSArchitecture.ToString()) {
    'X64' { 'x86_64-pc-windows-msvc' }
    'Arm64' { 'aarch64-pc-windows-msvc' }
    default { throw "Unsupported OS architecture: $_" }
}
Write-Host "Installing Rust $ToolchainVersion for $hostTriple into $RustRoot"

$work = Join-Path $env:TEMP "install-rust-$PID"
New-Item -Path $work -ItemType Directory -Force | Out-Null
try {
    $installer = Join-Path $work 'rustup-init.exe'
    $uri = "https://static.rust-lang.org/rustup/dist/$hostTriple/rustup-init.exe"
    Invoke-Download $uri $installer
    Invoke-Download "$uri.sha256" "$installer.sha256"
    $expected = ((Get-Content "$installer.sha256" -Raw).Trim() -split '\s+')[0]
    $actual = (Get-FileHash $installer -Algorithm SHA256).Hash
    if ($actual -ne $expected) { throw "rustup-init.exe SHA256 $actual does not match published $expected" }

    $env:RUSTUP_HOME = $RustupHome
    $env:CARGO_HOME = $InstallerCargoHome
    $null = Invoke-Native $installer @('-y', '--no-modify-path', '--profile', 'minimal', '--default-toolchain', $ToolchainVersion)
    $env:Path = "$CargoBin;$env:Path"
    $rustup = Join-Path $CargoBin 'rustup.exe'
    $rustc = Join-Path $CargoBin 'rustc.exe'
    $cargo = Join-Path $CargoBin 'cargo.exe'

    $null = Invoke-Native $rustup (@('target', 'add') + $Targets)

    # Persist for agents. Write Path through the registry to keep it
    # REG_EXPAND_SZ with its %VAR% entries unexpanded.
    Set-ItemProperty -Path $EnvironmentKey -Name 'RUSTUP_HOME' -Type String -Value $RustupHome
    $machinePath = (Get-Item $EnvironmentKey).GetValue('Path', '', 'DoNotExpandEnvironmentNames')
    $pathEntries = @($machinePath -split ';' | Where-Object { $_ })
    if (-not ($pathEntries | Where-Object { $_.TrimEnd('\') -ieq $CargoBin })) {
        Set-ItemProperty -Path $EnvironmentKey -Name 'Path' -Type ExpandString -Value ((@($CargoBin) + $pathEntries) -join ';')
    }

    $active = Invoke-Native $rustup @('show', 'active-toolchain')
    if ("$($active | Select-Object -First 1)" -notmatch "^$([regex]::Escape($ToolchainVersion))-") { throw "Unexpected active Rust toolchain: $active" }
    $installed = Invoke-Native $rustup @('target', 'list', '--installed')
    $missing = @($Targets | Where-Object { $installed -notcontains $_ })
    if ($missing.Count) { throw "Rust targets missing after install: $($missing -join ', ')" }
    $null = Invoke-Native $rustc @('-vV')
    $null = Invoke-Native $cargo @('--version')

    # A std-using static library proves each target's standard library is
    # usable. rustc archives static libraries itself, so no linker is involved.
    $source = Join-Path $work 'probe.rs'
    Set-Content -Path $source -Encoding Ascii -Value '#[no_mangle] pub extern "C" fn rust_probe() -> usize { std::env::args().count() }'
    foreach ($target in $Targets) {
        $lib = Join-Path $work "probe-$target.lib"
        $null = Invoke-Native $rustc @('--edition', '2021', '--crate-type', 'staticlib', '-O', '--target', $target, '-o', $lib, $source)
        if (-not (Test-Path $lib)) { throw "rustc reported success but $lib is missing" }
        Write-Host "Built $target static library"
    }

    if ((Get-Item $EnvironmentKey).GetValue('RUSTUP_HOME') -ne $RustupHome) { throw 'RUSTUP_HOME was not persisted at Machine scope' }
    $persistedPath = (Get-Item $EnvironmentKey).GetValue('Path', '', 'DoNotExpandEnvironmentNames')
    if (-not (@($persistedPath -split ';') | Where-Object { $_.TrimEnd('\') -ieq $CargoBin })) { throw "$CargoBin was not persisted in the Machine Path" }
} finally {
    Remove-Item -Path $work -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "Rust ready in ${RustRoot}: $($Targets -join ', ')"
