# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [string]$SourcesPath = $PSScriptRoot
)

$workpath = Join-Path $SourcesPath "build"

if (!(Test-Path -Path "$workpath\v8build")) {
    New-Item -ItemType "directory" -Path "$workpath\v8build" | Out-Null
}

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

Write-Host "Modifying PATH..."

$depot_tools_path = Join-Path $workpath "v8build\depot_tools"

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

$env:PATH = $path
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = 0
