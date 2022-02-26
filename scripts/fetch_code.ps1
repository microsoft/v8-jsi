# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [System.IO.DirectoryInfo]$SourcesPath = $PSScriptRoot,
    [System.IO.DirectoryInfo]$OutputPath = "$PSScriptRoot\out",
    [string]$Configuration = "Release",
    [string]$AppPlatform = "win32",
    [string]$NugetDownloadLocation = 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe'
)

$workpath = Join-Path $SourcesPath "build"

$env:GIT_REDIRECT_STDERR = '2>&1'

Push-Location (Join-Path $workpath "v8build")
& fetch v8

# commenting this out since the Android build is in android/build.sh
# if ($Configuration -like "*android") {
#     $target_os = @'
# target_os= ['android']
# '@

#     Add-Content -Path (Join-Path $workpath "v8build\.gclient") $target_os
# }

& gclient sync --with_branch_heads

Push-Location (Join-Path $workpath "v8build\v8")

$config = Get-Content (Join-Path $SourcesPath "config.json") | Out-String | ConvertFrom-Json

& git fetch origin $config.v8ref
$CheckOutVersion = (git checkout FETCH_HEAD) | Out-String

# Apply patches
& git apply --ignore-whitespace (Join-Path $SourcesPath "scripts\patch\src.diff")

& gclient runhooks
& gclient sync

Push-Location (Join-Path $workpath "v8build\v8\build")
& git apply --ignore-whitespace (Join-Path $SourcesPath "scripts\patch\build.diff")

Push-Location (Join-Path $workpath "v8build\v8\third_party\zlib")
& git apply --ignore-whitespace (Join-Path $SourcesPath "scripts\patch\zlib.diff")

Pop-Location
Pop-Location
Pop-Location
Pop-Location

$version = [version]$config.version

$gitRevision = ""
$v8Version = ""

#TODO: Remove before merging
Write-Warning "CheckoutVersion: [$CheckoutVersion]"
$Matches = $CheckOutVersion | Select-String -Pattern 'HEAD is now at (.+) Version (.+)'
if ($Matches.Matches.Success) {
    $gitRevision = $Matches.Matches.Groups[1].Value
    $v8Version = $Matches.Matches.Groups[2].Value.Trim()
    $verString = $verString + "-v8_" + $v8Version.Replace('.', '_')
}

# Save the revision information in the NuGet description
if (!(Test-Path -Path $OutputPath)) {
    New-Item -ItemType "directory" -Path $OutputPath | Out-Null
}

$buildoutput = Join-Path $workpath "v8build\v8\out\$Platform\$Configuration"

(Get-Content "$SourcesPath\src\version.rc") `
    -replace ('V8JSIVER_MAJOR', $version.Major) `
    -replace ('V8JSIVER_MINOR', $version.Minor) `
    -replace ('V8JSIVER_BUILD', $version.Build) `
    -replace ('V8JSIVER_V8REF', $v8Version.Replace('.', '_')) |`
    Set-Content "$SourcesPath\src\version_gen.rc"

Copy-Item $SourcesPath\ReactNative.V8Jsi.Windows.nuspec $OutputPath

Write-Host "##vso[task.setvariable variable=V8JSI_VERSION;]$verString"

# Install build depndencies for Android
# commenting this since Android build is in android/build.sh
# if ($PSVersionTable.Platform -and !$IsWindows) {
#     $install_script_path = Join-Path $workpath "v8build/v8/build/install-build-deps-android.sh"

#     & sudo bash $install_script_path
# }

# Download dependencies (ASIO used by Inspector implementation)
$asioUrl ="https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-18-1.zip"
$asioPath = Join-Path $workpath "v8build"
$asioDownload = Join-Path $asioPath $(Split-Path -Path $asioUrl -Leaf)

Invoke-WebRequest -Uri $asioUrl -OutFile $asioDownload
$asioDownload | Expand-Archive -DestinationPath $asioPath -Force

$env:ASIO_ROOT = Join-Path $asioPath "asio-asio-1-18-1\asio\include"
Write-Host "##vso[task.setvariable variable=ASIO_ROOT;]$env:ASIO_ROOT"

# Remove unused code
Remove-Item -Recurse -Force (Join-Path $workpath "v8build\v8\third_party\perfetto")