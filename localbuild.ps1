# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [string]$SourcesPath = $PSScriptRoot,
    [string]$OutputPath = "$PSScriptRoot\out",
    [string]$Platform = "all",
    [string]$Configuration = "all",
    [string]$Architecture = "all",
    [bool]$Setup = $true
)

if ($Setup) {
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
}

if ($Configuration -eq "all") {
    $Configurations = "debug", "release"
} else {
    $Configurations = @($Configuration)
}

if ($Platform -eq "all") {
    $Platforms = "x64", "x86" #, "arm64"
} else {
    $Platforms = @($Platform)
}

if ($Architecture -eq "all") {
    $Architectures = "win32", "uwp"
} else {
    $Architectures = @($Architecture)
}

foreach ($Plat in $Platforms) {
    foreach ($Config in $Configurations) {
        foreach ($Arch in $Architectures) {
            Write-Host "Building $Arch $Plat $Config..."
            & ".\scripts\build.ps1" -SourcesPath $SourcesPath -OutputPath $OutputPath -Platform $Plat -Configuration $Config -Architecture $Arch
        }
    }
}

if (!$?) {
    Write-Host "Build failure"
    exit 1
}