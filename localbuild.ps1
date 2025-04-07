# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [System.IO.DirectoryInfo]$SourcesPath = $PSScriptRoot,

    [System.IO.DirectoryInfo]$OutputPath = "$PSScriptRoot\out",

    [ValidateSet('x64', 'x86', 'arm64')]
    [String[]]$Platform = @('x64'),

    [ValidateSet('Debug', 'Release')]
    [String[]]$Configuration = @('Debug'),

    [ValidateSet('win32', 'android', 'linux', 'mac')]
    [String[]]$AppPlatform = @('win32'),

    [switch]$NoSetup,

    [switch]$FakeBuild
)

if (!$NoSetup -and !$FakeBuild) {
    Write-Host "Downloading environment..."
    & ".\scripts\download_depottools.ps1" -SourcesPath $SourcesPath -NoADO

    if (!$?) {
        Write-Host "Failed to download depot-tools"
        exit 1
    }

    Write-Host "Fetching code..."
    & ".\scripts\fetch_code.ps1" -SourcesPath $SourcesPath -AppPlatform $AppPlatform[0]

    if (!$?) {
        Write-Host "Failed to retrieve the v8 code"
        exit 1
    }
} else {
    & ".\scripts\download_depottools.ps1" -SourcesPath $SourcesPath -NoSetup -NoADO
}

foreach ($Plat in $Platform) {
    foreach ($Config in $Configuration) {
        foreach ($AppPlat in $AppPlatform) {
            Write-Host "Building $AppPlat $Plat $Config..."
            & ".\scripts\build.ps1" -SourcesPath $SourcesPath -OutputPath $OutputPath `
              -Platform $Plat -Configuration $Config -AppPlatform $AppPlat -FakeBuild:$FakeBuild
        }
    }
}

if (!$?) {
    Write-Host "Build failure"
    Pop-Location
    exit 1
}
