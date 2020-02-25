# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
$OutputPath = "$PSScriptRoot\out"
$SourcesPath = $PSScriptRoot
$Platforms = "x64", "x86"
$Configurations = "Release", "Debug", "Release-Clang", "EXPERIMENTAL-libcpp-Clang"

Write-Host "Downloading environment..."
& ".\scripts\download_depottools.ps1" -SourcesPath $SourcesPath

if (!$?) {
    Write-Host "Failed to download depot-tools"
    exit 1
}

Write-Host "Fetching code..."
& ".\scripts\fetch_code.ps1" -SourcesPath $SourcesPath -OutputPath $OutputPath -Configuration $Configurations[0]

if (!$?) {
    Write-Host "Failed to retrieve the v8 code"
    exit 1
}

<#foreach ($Platform in $Platforms) {
    foreach ($Configuration in $Configurations) {
        Write-Host "Building $Platform $Configuration..."
        & ".\scripts\build.ps1" -SourcesPath $SourcesPath -OutputPath $OutputPath -Platform $Platform -Configuration $Configuration
    }
}#>

& ".\scripts\build.ps1" -SourcesPath $SourcesPath -OutputPath $OutputPath -Platform $Platforms[0] -Configuration $Configurations[0]

if (!$?) {
    Write-Host "Build failure"
    exit 1
}