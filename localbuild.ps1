# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [System.IO.DirectoryInfo]$SourcesPath = $PSScriptRoot,

    [System.IO.DirectoryInfo]$OutputPath = "$PSScriptRoot\out",

    [ValidateSet("x64", "x86", "arm64")]
    [String[]]$Platform = @("x64"),

    [ValidateSet("debug", "release")]
    [String[]]$Configuration = @("debug"),

    [ValidateSet("win32", "uwp")]
    [String[]]$AppPlatform = @("win32"),

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
    & ".\scripts\fetch_code.ps1" -SourcesPath $SourcesPath -OutputPath $OutputPath -Configuration $Configuration[0]

    if (!$?) {
        Write-Host "Failed to retrieve the v8 code"
        exit 1
    }
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
    exit 1
}
