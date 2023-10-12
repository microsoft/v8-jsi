# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [System.IO.DirectoryInfo]$SourcesPath = $PSScriptRoot
)

# Details about the V8 release process: https://v8.dev/docs/release-process

# TODO: a cron ADO task should trigger this script to update the version

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

  # TODO: do this for every new $patchLevel as well?
  return $buildNumber
}

function BumpSemVer($version) {
  $versionParts = $version.Split('.')
  $versionParts[2] = [int]$versionParts[2] + 1
  return $versionParts -join '.'
}

$buildNumber = GetLatestBuildNumberForVersion($latestVersion)

Write-Host "Latest build number upstream is $buildNumber"
Write-Host "Build number currently being built is $($config.buildNumber)"

if ($buildNumber -eq $config.buildNumber) {
  Write-Host "Latest build number is already being built. All good!"
  exit 0
}

Write-Host "New stable build number released, attempting to bump it"

$config.buildNumber = $buildNumber
$config.version = BumpSemVer($config.version)

ConvertTo-Json -InputObject $config | Set-Content (Join-Path $SourcesPath "config.json")

git config user.name github-actions
git config user.email github-actions@github.com
git add config.json
git commit -m "Updating version to $($config.version) (new upstream build number $buildNumber)"
git push