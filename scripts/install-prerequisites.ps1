#Requires -Version 5.1
<#
.SYNOPSIS
Checks and, with consent, installs NASM, Python and Node.js through WinGet.
.DESCRIPTION
Run from a source checkout with deps\nodejs\configure present. Existing accepted
tools are kept. Machine installers may prompt for UAC; -Yes is not unattended.
No persistent PATH or execution policy is changed. Open a new terminal afterward.
.PARAMETER CheckOnly
Report prerequisites without calling WinGet or installing anything.
.PARAMETER Yes
Skip the single consent question and accept WinGet source/package agreements.
.EXAMPLE
powershell.exe -NoProfile -File .\scripts\install-prerequisites.ps1
#>
[CmdletBinding()]
param([switch]$CheckOnly, [switch]$Yes)

# Copyright (c) Microsoft Corporation. Licensed under the MIT license.
# Minimum stable WinGet: 1.6.2631 (--installer-type); --no-upgrade arrived in 1.4.
# https://github.com/microsoft/winget-cli/releases/tag/v1.6.2631
# https://github.com/microsoft/winget-cli/releases/tag/v1.4.10173
# All install flags: https://learn.microsoft.com/windows/package-manager/winget/install

function Get-AcceptedPythons([string]$ConfigurePath) {
    if (-not (Test-Path -LiteralPath $ConfigurePath -PathType Leaf)) {
        throw "Source tree required: missing $ConfigurePath (deps\nodejs\configure)."
    }
    $text = Get-Content -LiteralPath $ConfigurePath -Raw
    $start = [regex]::Match($text, 'acceptable_pythons\s*=\s*\(')
    if (-not $start.Success) { throw "Cannot parse acceptable_pythons in $ConfigurePath." }
    $open = $start.Index + $start.Length - 1
    $depth = 0
    $end = $open
    for (; $end -lt $text.Length; $end++) {
        if ($text[$end] -eq '(') { $depth++ }
        if ($text[$end] -eq ')') { $depth--; if ($depth -eq 0) { break } }
    }
    $versions = @([regex]::Matches($text.Substring($open, $end - $open), '\(\s*(\d+)\s*,\s*(\d+)\s*\)') |
        ForEach-Object { '{0}.{1}' -f $_.Groups[1].Value, $_.Groups[2].Value })
    if ($versions.Count -eq 0 -or $depth -ne 0) { throw "Cannot parse acceptable_pythons in $ConfigurePath." }
    return $versions
}

function Get-HostArchitecture {
    if ($env:PROCESSOR_ARCHITEW6432) { return $env:PROCESSOR_ARCHITEW6432 }
    return $env:PROCESSOR_ARCHITECTURE
}

function Test-Interactive {
    return [Environment]::UserInteractive -and -not [Console]::IsInputRedirected -and
        -not ([Environment]::GetCommandLineArgs() -contains '-NonInteractive')
}

function Test-Elevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$identity).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-Nasm {
    $programFiles = $env:ProgramFiles
    if ($env:ProgramW6432) { $programFiles = $env:ProgramW6432 }
    $folders = @($env:PATH -split ';') + @(
        "$programFiles\NASM", "${env:ProgramFiles(x86)}\NASM", "$env:LOCALAPPDATA\bin\NASM")
    foreach ($folder in $folders) {
        if (-not $folder) { continue }
        $exe = Join-Path $folder.Trim('"') 'nasm.exe'
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { continue }
        try {
            $version = (& $exe -v 2>$null | Out-String).Trim()
            if ($LASTEXITCODE -ne 0 -or -not $version) { throw 'failing or empty -v output' }
            return @{ Path = $exe; Version = $version; Error = $null }
        } catch { return @{ Path = $exe; Version = ''; Error = "NASM version probe failed at ${exe}: $_" } }
    }
}

function Get-Pythons([string[]]$Accepted) {
    $candidates = @(Get-Command python.exe -CommandType Application -All -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Source })
    foreach ($root in @('HKCU:\Software\Python\PythonCore', 'HKLM:\Software\Python\PythonCore',
        'HKLM:\Software\WOW6432Node\Python\PythonCore')) {
        foreach ($key in @(Get-ChildItem $root -ErrorAction SilentlyContinue)) {
            $install = Get-Item -LiteralPath "$($key.PSPath)\InstallPath" -ErrorAction SilentlyContinue
            if ($install -and $install.GetValue('')) {
                $candidates += Join-Path $install.GetValue('') 'python.exe'
            }
        }
    }
    foreach ($exe in @($candidates | Select-Object -Unique)) {
        if ($exe -match '\\WindowsApps\\' -or -not (Test-Path -LiteralPath $exe -PathType Leaf)) { continue }
        try {
            $version = (& $exe -c 'import sys; print(*sys.version_info[:3])' 2>$null | Out-String).Trim()
            if ($LASTEXITCODE -eq 0 -and $version -match '^(\d+) (\d+) (\d+)$') {
                if ($Accepted -contains "$($Matches[1]).$($Matches[2])") {
                    @{ Path = $exe; Version = "$($Matches[1]).$($Matches[2]).$($Matches[3])" }
                }
            }
        } catch { continue }
    }
}

function Get-Node {
    $command = Get-Command node.exe -CommandType Application -ErrorAction SilentlyContinue
    if (-not $command) { return }
    $exe = $command.Source
    try {
        $version = (& $exe --version 2>$null | Out-String).Trim()
        if ($LASTEXITCODE -ne 0 -or $version -notmatch '^v(\d+)\.') { throw 'invalid --version output' }
        return @{ Path = $exe; Version = $version; Error = $null }
    } catch { return @{ Path = $exe; Version = ''; Error = "Node.js version probe failed at ${exe}: $_" } }
}

function Invoke-WinGet([string[]]$Arguments) {
    if ($Arguments[0] -eq '--version') {
        $output = & winget.exe @Arguments | Out-String
        return @{ ExitCode = $LASTEXITCODE; Output = $output.Trim() }
    }
    # Keep agreement/installer prompts visible while WinGet is running.
    $winget = (Get-Command winget.exe -CommandType Application -ErrorAction Stop).Source
    $process = Start-Process -FilePath $winget -ArgumentList $Arguments -NoNewWindow -Wait -PassThru
    return @{ ExitCode = $process.ExitCode; Output = '' }
}

function Get-RegistryPath([string]$Scope) {
    return [Environment]::GetEnvironmentVariable('Path', $Scope)
}

function Merge-ProcessPath {
    $entries = New-Object 'System.Collections.Generic.List[string]'
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    # Preserve even duplicate/empty process entries; deduplicate only appended entries.
    foreach ($entry in ($env:PATH -split ';')) {
        $entries.Add($entry)
        [void]$seen.Add($entry.TrimEnd('\'))
    }
    foreach ($scope in @('Machine', 'User')) {
        foreach ($entry in ((Get-RegistryPath $scope) -split ';')) {
            if ($entry -and $seen.Add($entry.TrimEnd('\'))) { $entries.Add($entry) }
        }
    }
    $env:PATH = $entries -join ';'
}

function Install-Prerequisite([string]$Id, [string[]]$Extra, [switch]$AcceptAgreements) {
    $arguments = @('install', '--id', $Id, '--exact', '--source', 'winget', '--no-upgrade') + $Extra
    if ($AcceptAgreements) { $arguments += @('--accept-source-agreements', '--accept-package-agreements') }
    $command = 'winget ' + ($arguments -join ' ')
    Write-Host $command
    try {
        $result = Invoke-WinGet $arguments
        Write-Host $result.Output
        if ($result.ExitCode -ne 0) { Write-Host "FAILED: exit $($result.ExitCode): $command" }
        Merge-ProcessPath
        return ($result.ExitCode -eq 0)
    } catch {
        Write-Host "FAILED: ${command}: $_"
        return $false
    }
}

function Write-Detected([string]$Name, $Tools) {
    if (-not $Tools) { Write-Host "${Name}: missing"; return }
    foreach ($tool in $Tools) {
        Write-Host "${Name}: $($tool.Version) at $($tool.Path)"
        if ($tool.Error) { Write-Host $tool.Error }
    }
}

function Test-NodeVersion($Node) {
    if (-not $Node -or $Node.Error) { return $false }
    if ($Node.Version -match '^v(\d+)\.' -and [int]$Matches[1] -ge 24) { return $true }
    Write-Host "Node.js $($Node.Version) at $($Node.Path) is first on PATH. Update it (or its version manager) to 24.x or remove it and rerun."
    return $false
}

function Invoke-Prerequisites([switch]$CheckOnly, [switch]$Yes,
    [string]$ConfigurePath = (Join-Path $PSScriptRoot '..\deps\nodejs\configure')) {
    $ErrorActionPreference = 'Stop'
    $nasmOK = $pythonOK = $nodeOK = $false
    try {
        $accepted = @(Get-AcceptedPythons $ConfigurePath)
        Write-Host "Accepted Python versions: $($accepted -join ', ')"
        if ((Get-HostArchitecture) -notin @('AMD64', 'ARM64')) { throw 'Unsupported host: requires 64-bit Windows (x64 or ARM64).' }
        $nasm = Get-Nasm
        $pythons = @(Get-Pythons $accepted)
        $node = Get-Node
        Write-Detected 'NASM' @($nasm)
        Write-Detected 'Python' $pythons
        Write-Detected 'Node.js' @($node)
        $nasmOK = $nasm -and -not $nasm.Error
        $pythonOK = $pythons.Count -gt 0
        $nodeOK = Test-NodeVersion $node
        if (-not $pythonOK -and $accepted -notcontains '3.14') { throw 'Python 3.14 is no longer accepted by deps\nodejs\configure; update the prerequisite installer.' }
        $missing = @()
        if (-not $nasm) { $missing += 'NASM.NASM' }
        if (-not $pythonOK) { $missing += 'Python.Python.3.14 --scope user --installer-type burn' }
        if (-not $node) { $missing += 'OpenJS.NodeJS.LTS --version 24.22.1' }
        if ($missing.Count -gt 0) {
            Write-Host "Install plan (existing tools are kept): $($missing -join '; ')"
            if (-not $CheckOnly) {
                if (-not $Yes) {
                    if (-not (Test-Interactive)) { throw 'Non-interactive session: rerun with -Yes to consent, or -CheckOnly to report.' }
                    $answer = Read-Host 'Install missing prerequisites? [Y/n]'
                    if ($answer -and $answer -notmatch '^(y|yes)$') { throw 'Installation declined.' }
                }
                if (-not $pythonOK -and (Test-Elevated)) { Write-Warning 'User-scope Python installs into the elevated account.' }
                try { $winget = Invoke-WinGet @('--version') }
                catch { throw 'WinGet is required to install. See https://learn.microsoft.com/windows/package-manager/winget/ (App Installer).' }
                Write-Host "WinGet: $($winget.Output)"
                if ($winget.ExitCode -ne 0 -or $winget.Output -notmatch '^v?(\d+\.\d+\.\d+)' -or
                    [version]$Matches[1] -lt [version]'1.6.2631') {
                    throw 'WinGet 1.6.2631 or newer is required. See https://learn.microsoft.com/windows/package-manager/winget/'
                }
                if (-not $nasm) {
                    $installed = Install-Prerequisite 'NASM.NASM' @() -AcceptAgreements:$Yes
                    $nasm = Get-Nasm
                    Write-Detected 'NASM' @($nasm)
                    $nasmOK = $installed -and $nasm -and -not $nasm.Error
                    if (-not $nasm) { Write-Host 'NASM still missing after install.' }
                }
                if (-not $pythonOK) {
                    $installed = Install-Prerequisite 'Python.Python.3.14' @('--scope', 'user', '--installer-type', 'burn') -AcceptAgreements:$Yes
                    $pythons = @(Get-Pythons $accepted)
                    Write-Detected 'Python' $pythons
                    $pythonOK = $installed -and $pythons.Count -gt 0
                    if ($pythons.Count -eq 0) { Write-Host 'Accepted Python still missing after install.' }
                }
                if (-not $node) {
                    $installed = Install-Prerequisite 'OpenJS.NodeJS.LTS' @('--version', '24.22.1') -AcceptAgreements:$Yes
                    $node = Get-Node
                    Write-Detected 'Node.js' @($node)
                    $detected = Test-NodeVersion $node
                    $nodeOK = $installed -and $detected
                    if (-not $node) { Write-Host 'Node.js still missing after install.' }
                }
            }
        }
        if ($nasmOK -and $pythonOK -and $nodeOK) { return 0 }
        return 1
    } catch {
        Write-Host "FAILED: $_"
        return 1
    } finally {
        Write-Host "Summary: NASM=$([bool]$nasmOK); Python=$([bool]$pythonOK); Node.js=$([bool]$nodeOK)"
        Write-Host 'Not covered: Visual Studio C++/Clang components, Windows SDK, and Git.'
        Write-Host 'Open a new terminal after installation. No restart is performed.'
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    exit (Invoke-Prerequisites -CheckOnly:$CheckOnly -Yes:$Yes)
}
