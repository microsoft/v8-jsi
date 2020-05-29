# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [string]$Platform = "x64", #"arm64",
    [string]$Configuration = "Debug" # "UWP-Release" "Release-Clang"
)

$OutputPath = "$PSScriptRoot\out"
$SourcesPath = $PSScriptRoot
$Platforms = "x64", "x86", "arm64"
$Configurations = "Debug", "Release", "Release-Clang", "EXPERIMENTAL-libcpp-Clang"

Write-Host "Downloading environment..."
& ".\scripts\download_depottools.ps1" -SourcesPath $SourcesPath

if (!$?) {
    Write-Host "Failed to download depot-tools"
    exit 1
}

Write-Host "Fetching code..."
& ".\scripts\fetch_code.ps1" -SourcesPath $SourcesPath -OutputPath $OutputPath -Configuration $Configuration

if (!$?) {
    Write-Host "Failed to retrieve the v8 code"
    exit 1
}

if ($Platform -like "all") {
    foreach ($Plat in $Platforms) {
        foreach ($Config in $Configurations) {
            Write-Host "Building $Plat $Config..."
            & ".\scripts\build.ps1" -SourcesPath $SourcesPath -OutputPath $OutputPath -Platform $Plat -Configuration $Config
        }
    }
}
else {
    & ".\scripts\build.ps1" -SourcesPath $SourcesPath -OutputPath $OutputPath -Platform $Platform -Configuration $Configuration
}

if (!$?) {
    Write-Host "Build failure"
    exit 1
}