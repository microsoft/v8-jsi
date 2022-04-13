# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [System.IO.DirectoryInfo]$SourcesPath = $PSScriptRoot,

    [System.IO.DirectoryInfo]$OutputPath = "$PSScriptRoot\out",

    [ValidateSet('x64', 'x86', 'arm64')]
    [String[]]$Platform = @('x64'),

    [ValidateSet('Debug', 'Release', 'ReleaseAndroid', 'ReleaseLinux')]
    [String[]]$Configuration = @('Debug'),

    [ValidateSet('win32', 'uwp')]
    [String[]]$AppPlatform = @('win32'),

    [switch]$NoSetup
)

if (! $NoSetup.IsPresent) {
    Write-Host "Downloading environment..."
    & ".\scripts\download_depottools.ps1" -SourcesPath $SourcesPath

    if (!$?) {
        Write-Host "Failed to download depot-tools"
        exit 1
    }

    Write-Host "Fetching code..."
    & ".\scripts\fetch_code.ps1" -SourcesPath $SourcesPath -OutputPath $OutputPath -Configuration $Configuration[0]

    if (!$?) {
        Write-Host "Failed to retrieve the v8 code"
        exit 1
    }
} else {
    # TODO: duplicate from download_depottools.ps1
    $depot_tools_path = Join-Path $SourcesPath "build\v8build\depot_tools"

    if (!$PSVersionTable.Platform -or $IsWindows) {
        $path = "$depot_tools_path;$Env:PATH"
        $path = ($path.Split(';') | Where-Object { $_ -notlike '*Chocolatey*' }) -join ';'
    }
    else {
        $path = "$depot_tools_path`:$Env:PATH"
    }
    
    # This syntax is used for setting variables in an ADO environment; in a normal local build it would just print these strings out to the console
    Write-Host "##vso[task.setvariable variable=PATH;]$path"
    Write-Host "##vso[task.setvariable variable=DEPOT_TOOLS_WIN_TOOLCHAIN;]0"
    Write-Host "##vso[task.setvariable variable=GCLIENT_PY3;]1"
    
    $env:PATH = $path
    $env:DEPOT_TOOLS_WIN_TOOLCHAIN = 0
    $env:GCLIENT_PY3 = 1

    # TODO: duplicate from fetch_code.ps1
    $asioPath = Join-Path $SourcesPath "build\v8build"
    $env:ASIO_ROOT = Join-Path $asioPath "asio-asio-1-18-1\asio\include"
    Write-Host "##vso[task.setvariable variable=ASIO_ROOT;]$env:ASIO_ROOT"
}

foreach ($Plat in $Platform) {
    foreach ($Config in $Configuration) {
        foreach ($AppPlat in $AppPlatform) {
            Write-Host "Building $AppPlat $Plat $Config..."
            & ".\scripts\build.ps1" -SourcesPath $SourcesPath -OutputPath $OutputPath -Platform $Plat -Configuration $Config -AppPlatform $AppPlat
        }
    }
}

if (!$?) {
    Write-Host "Build failure"
    Pop-Location
    exit 1
}
