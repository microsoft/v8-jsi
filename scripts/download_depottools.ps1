# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [System.IO.DirectoryInfo]$SourcesPath = $PSScriptRoot,
    [switch]$NoSetup,
    [switch]$NoADO
)

$workpath = Join-Path $SourcesPath "build"

if (!(Test-Path -Path "$workpath\v8build")) {
    New-Item -ItemType "directory" -Path "$workpath\v8build" | Out-Null
}

$asioPath = Join-Path $workpath "v8build"

if (! $NoSetup.IsPresent) {
    Write-Host "Downloading depot-tools.zip..."

    # This is the recommended way to get depot-tools on Windows, but the git checkout is much faster (shaves off about 5 minutes from CI loop runtime)
    $UseArchive = $false

    if ($UseArchive) {
        $output = [System.IO.Path]::GetTempFileName()
        Invoke-WebRequest -Uri "https://storage.googleapis.com/chrome-infra/depot_tools.zip" -OutFile "$output.zip"
        Write-Host "Unzipping depot-tools.zip..."
        Expand-Archive -path "$output.zip" -DestinationPath "$workpath\v8build\depot_tools"
        Remove-Item "$output.zip"
    }
    else {
        $env:GIT_REDIRECT_STDERR = '2>&1'
        Push-Location "$workpath\v8build"
        git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
        Pop-Location
    }

    # Download dependencies (ASIO used by Inspector implementation)
    $asioUrl ="https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-18-1.zip"
    $asioDownload = Join-Path $asioPath $(Split-Path -Path $asioUrl -Leaf)

    Invoke-WebRequest -Uri $asioUrl -OutFile $asioDownload
    $asioDownload | Expand-Archive -DestinationPath $asioPath -Force
}

Write-Host "Modifying PATH..."

$depot_tools_path = Join-Path $workpath "v8build\depot_tools"

if (!$PSVersionTable.Platform -or $IsWindows) {
    $path = "$depot_tools_path;$Env:PATH"
    $path = ($path.Split(';') | Where-Object { $_ -notlike '*Chocolatey*' }) -join ';'
}
else {
    $path = "$depot_tools_path`:$Env:PATH"
}

$env:PATH = $path
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = 0
$env:GCLIENT_PY3 = 1
$env:ASIO_ROOT = Join-Path $asioPath "asio-asio-1-18-1\asio\include"

if (! $NoADO.IsPresent) {
    Write-Host "##vso[task.setvariable variable=PATH;]$path"
    Write-Host "##vso[task.setvariable variable=DEPOT_TOOLS_WIN_TOOLCHAIN;]0"
    Write-Host "##vso[task.setvariable variable=GCLIENT_PY3;]1"
    Write-Host "##vso[task.setvariable variable=ASIO_ROOT;]$env:ASIO_ROOT"
}