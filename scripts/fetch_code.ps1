# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [System.IO.DirectoryInfo]$SourcesPath = $PSScriptRoot,
    [string]$AppPlatform = "win32"
)

$workpath = Join-Path $SourcesPath "build"

$env:GIT_REDIRECT_STDERR = '2>&1'

Push-Location $workpath
& fetch --no-history --nohooks v8

if ("android linux mac".contains($AppPlatform)) {
    $target_os = "`ntarget_os= ['" + $AppPlatform + "']`n"
    Add-Content -Path (Join-Path $workpath ".gclient") $target_os
}

Push-Location (Join-Path $workpath "v8")

$config = Get-Content (Join-Path $SourcesPath "config.json") | Out-String | ConvertFrom-Json

& git fetch origin $config.v8ref
$CheckOutVersion = (git checkout FETCH_HEAD) | Out-String

# Apply patches
& git apply --ignore-whitespace (Join-Path $SourcesPath "scripts\patch\src.diff")

& gclient runhooks
& gclient sync

Push-Location (Join-Path $workpath "v8\build")
& git apply --ignore-whitespace (Join-Path $SourcesPath "scripts\patch\build.diff")

Push-Location (Join-Path $workpath "v8\third_party\zlib")
& git apply --ignore-whitespace (Join-Path $SourcesPath "scripts\patch\zlib.diff")

Pop-Location
Pop-Location
Pop-Location
Pop-Location

Push-Location $SourcesPath
$ourGitHash = ((git rev-parse --verify HEAD) | Out-String).Trim()
Pop-Location

$version = [version]$config.version

$gitRevision = ""
$v8Version = ""

$Matches = $CheckOutVersion | Select-String -Pattern 'HEAD is now at (.+) Version (.+)'
if ($Matches.Matches.Success) {
    $gitRevision = $Matches.Matches.Groups[1].Value
    $v8Version = $Matches.Matches.Groups[2].Value.Trim()
    $verString = $verString + "-v8_" + $v8Version.Replace('.', '_')
}

# Save the revision information in the version RC and env variable
(Get-Content "$SourcesPath\src\version.rc") `
    -replace ('V8JSIVER_MAJOR', $version.Major) `
    -replace ('V8JSIVER_MINOR', $version.Minor) `
    -replace ('V8JSIVER_BUILD', $version.Build) `
    -replace ('V8JSIVER_V8REF', $v8Version.Replace('.', '_')) |`
    Set-Content "$SourcesPath\src\version_gen.rc"

Write-Host "##vso[task.setvariable variable=V8JSI_VERSION;]$verString"

# Generate the source_link.json file
(Get-Content "$SourcesPath\src\source_link.json") `
    -replace ('LOCAL_PATH', "$SourcesPath".Replace('\', '\\')) `
    -replace ('V8JSI_GIT_HASH', $ourGitHash) `
    -replace ('V8JSIVER_V8REF', $v8Version) |`
    Set-Content "$SourcesPath\src\source_link_gen.json"

# Update ADO build version string when run from ADO pipeline
if ($env:BUILD_BUILDNUMBER) {
    $buildVersion = $env:BUILD_BUILDNUMBER
    if (!$buildVersion.EndsWith($v8Version.Replace('.', '_'))) {
        $buildVersion = $buildVersion + " - " + $version.Major + "." + $version.Minor + "." + $version.Build + "." + $v8Version.Replace('.', '_')
        Write-Host "##vso[build.updateBuildNumber]$buildVersion"
    }
}

# Install build dependencies for Android
if ($AppPlatform -eq "android") {
    $install_script_path = Join-Path $workpath "v8/build/install-build-deps-android.sh"

    & sudo bash $install_script_path
}

if ($AppPlatform -eq "linux") {
    $install_script_path = Join-Path $workpath "v8/build/install-build-deps.sh"

    & sudo bash $install_script_path
}

# Remove unused code
Remove-Item -Recurse -Force (Join-Path $workpath "depot_tools\external_bin\gsutil")
Remove-Item -Recurse -Force (Join-Path $workpath "v8\test\test262\data\tools")
Remove-Item -Recurse -Force (Join-Path $workpath "v8\third_party\depot_tools\external_bin\gsutil")
Remove-Item -Recurse -Force (Join-Path $workpath "v8\third_party\perfetto")
Remove-Item -Recurse -Force (Join-Path $workpath "v8\third_party\protobuf")
Remove-Item -Recurse -Force (Join-Path $workpath "v8\third_party\rust")
Remove-Item -Recurse -Force (Join-Path $workpath "v8\third_party\rust-toolchain")
Remove-Item -Recurse -Force (Join-Path $workpath "v8\bazel")
Remove-Item -Recurse -Force (Join-Path $workpath "v8\tools\clusterfuzz")
Remove-Item -Recurse -Force (Join-Path $workpath "v8\tools\package-lock.json")
Remove-Item -Recurse -Force (Join-Path $workpath "v8\tools\package.json")
Remove-Item -Recurse -Force (Join-Path $workpath "v8\tools\turbolizer")