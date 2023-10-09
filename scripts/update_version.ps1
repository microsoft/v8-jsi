# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [System.IO.DirectoryInfo]$SourcesPath = $PSScriptRoot
)

# https://omahaproxy.appspot.com is deprecated in favor of https://chromiumdash.appspot.com
$latestVersion = (Invoke-WebRequest "https://chromiumdash.appspot.com/fetch_releases?channel=Stable&platform=Windows&num=1" -UseBasicParsing | ConvertFrom-Json).milestone / 10

Write-Host "Latest stable version is $latestVersion"

$config = Get-Content (Join-Path $SourcesPath "config.json") | Out-String | ConvertFrom-Json
$builtVersion = (($config.v8ref | Select-String -Pattern "refs/branch-heads/(\d+\.\d+)").Matches[0].Groups[1].Value).Trim()

Write-Host "Version currently being built is $builtVersion"

if ($builtVersion -eq $latestVersion) {
  Write-Host "Latest stable version is already being built"
} else {
  Write-Host "New stable version released, manual intervention required to bump the version!"
  exit 1
}

function GetLatestBuildNumberForVersion($buildingVersion) {
  $versionUrl = "https://chromium.googlesource.com/v8/v8.git/+/refs/heads/$buildingVersion-lkgr/include/v8-version.h?format=text"
  $content = Invoke-WebRequest -ContentType 'text/plain' -Uri $versionUrl -UseBasicParsing
  $content = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($content.Content))
  $majorVersion = (($content | Select-String -Pattern "V8_MAJOR_VERSION (\d+)").Matches[0].Groups[1].Value).Trim()
  $minorVersion = (($content | Select-String -Pattern "V8_MINOR_VERSION (\d+)").Matches[0].Groups[1].Value).Trim()
  $buildNumber = (($content | Select-String -Pattern "V8_BUILD_NUMBER (\d+)").Matches[0].Groups[1].Value).Trim()
  $patchLevel = (($content | Select-String -Pattern "V8_PATCH_LEVEL (\d+)").Matches[0].Groups[1].Value).Trim()
  return "$buildNumber.$patchLevel"
}

$buildNumber = GetLatestBuildNumberForVersion($latestVersion)

Write-Host "Latest build number upstream is $buildNumber"

# TODO: continue implementation here (config.json should hold buildNumber.patchLevel as well, and a cron ADO task should trigger this script to update the version)