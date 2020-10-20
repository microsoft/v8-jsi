# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [System.IO.DirectoryInfo]$SourcesPath = $PSScriptRoot,

    [System.IO.DirectoryInfo]$OutputPath = "$PSScriptRoot\out",

    [ValidateSet('x64', 'x86', 'arm64')]
    [String[]]$Platform = @('x64'),

    [ValidateSet('Debug', 'Release')]
    [String[]]$Configuration = @('Debug'),

    [ValidateSet('win32', 'uwp')]
    [String[]]$AppPlatform = @('win32'),

    [bool]$Setup = $true,

    [switch] $UseOldGn
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

    if ($UseOldGn) {
        $gnzip = "$([System.IO.Path]::GetTempFileName()).zip"
        # From https://chrome-infra-packages.appspot.com/p/gn/gn/windows-amd64/+/2PLNlcZUEpB2kqsVjfC_JbdH8Cy0-8euFKq-BTPNcJ8C
        Invoke-WebRequest `
            -Uri 'https://storage.googleapis.com/chrome-infra-packages/store/SHA256/d8f2cd95c65412907692ab158df0bf25b747f02cb4fbc7ae14aabe0533cd709f?Expires=1603154260&GoogleAccessId=chrome-infra-packages%40appspot.gserviceaccount.com&Signature=E6mVPDUwiKYcO6HtemYQ8C7%2BuPzu%2Bqco1soBxtaEtukfLfBE9MnY%2FtPZGlNgRVBh0MX1%2FtcO62edOaV0Qmp511oQTyBnhyrnoaNz1eem4XAOmHP6RsQ2DGceM524wbW95gYHQQyOmgdnTtwbLXcGRzJ6naW%2B1wttzp5oOKkgBzu3lt1ToeWu1pPT7BzgfqzdF7mG2JaYtiQpd2uknDdVKIrXBEnXVHOT6wWYtIXZawB3phzZJKKVCOjR1XxQf2psvxUeAhhROCitY5%2BoRNvQgc5M09lnsWqA3SesAgLBzJMVHJ7GzPuusi8Y7Pw%2FpTRaJPhTnmY3Qv5mlxhoTzWrEw%3D%3D&response-content-disposition=attachment%3B+filename%3D%22gn-windows-amd64.zip%22' `
            -OutFile $gnzip

        Expand-Archive -Path $gnzip -DestinationPath $SourcesPath\build\v8build\v8\buildtools\win\ -Force
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
