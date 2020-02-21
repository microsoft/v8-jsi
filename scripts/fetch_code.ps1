# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [string]$SourcesPath = $PSScriptRoot,
    [string]$OutputPath = "$PSScriptRoot\out",
    [string]$Configuration = "Release"
)

$workpath = Join-Path $SourcesPath "build"

Push-Location (Join-Path $workpath "v8build")
& fetch v8

if ($Configuration -like "*android") {
    $target_os = @'
target_os= ['android']
'@

    Add-Content -Path (Join-Path $workpath "v8build\.gclient") $target_os
}

& gclient sync --with_branch_heads

Push-Location (Join-Path $workpath "v8build\v8")

$env:GIT_REDIRECT_STDERR = '2>&1'

$config = Get-Content (Join-Path $SourcesPath "config.json") | Out-String | ConvertFrom-Json

& git fetch origin $config.v8ref
$CheckOutVersion = (git checkout FETCH_HEAD) | Out-String
& gclient sync

#TODO (#2): Submit PR upstream to Google for this fix
$FixNeededPath = Join-Path $workpath "v8build\v8\src\base\template-utils.h"
(Get-Content $FixNeededPath) -replace ("#include <utility>", "#include <utility>`n#include <functional>") | Set-Content $FixNeededPath

if (!$PSVersionTable.Platform -or $IsWindows) {
    #TODO (#2): once the fix for the upstream hack lands, we can remove this patch (see https://bugs.chromium.org/p/chromium/issues/detail?id=1033106)
    Copy-Item -Path ( Join-Path $SourcesPath "scripts\patch\tool_wrapper.py") -Destination (Join-Path $workpath "v8build\v8\build\toolchain\win\tool_wrapper.py") -Force
}

Pop-Location
Pop-Location

$verString = $config.version

$gitRevision = ""
$v8Version = ""

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

(Get-Content "$SourcesPath\ReactNative.V8Jsi.Windows.nuspec") -replace ('VERSION_DETAILS', "V8 version: $v8Version; Git revision: $gitRevision") | Set-Content "$OutputPath\ReactNative.V8Jsi.Windows.nuspec"

Write-Host "##vso[task.setvariable variable=V8JSI_VERSION;]$verString"

# Install build depndencies for Android
if ($PSVersionTable.Platform -and !$IsWindows) {
    $install_script_path = Join-Path $workpath "v8build/v8/build/install-build-deps-android.sh"

    & sudo bash $install_script_path
}

#TODO (#2): Use the .gzip for Android / Linux builds
# Verify the Boost installation
if (-not (Test-Path "$env:BOOST_ROOT\boost\asio.hpp")) {

    if (-not (Test-Path (Join-Path $workpath "v8build\boost\boost_1_72_0\boost\asio.hpp"))) {
        Write-Host "Boost ASIO not found, downloading..."

        # This operation will take a long time, but it's required on the Azure Agents because they don't have ASIO headers
        $output = [System.IO.Path]::GetTempFileName()

        # If 7-zip is available on path, use it as it's much faster and lets us unpack just the folder we need
        if (Get-Command "7z.exe" -ErrorAction SilentlyContinue) { 
            Invoke-WebRequest -Uri "https://dl.bintray.com/boostorg/release/1.72.0/source/boost_1_72_0.7z" -OutFile "$output.7z"
            Write-Host "Unzipping archive..."
            7z x "$output.7z" -o"$workpath\v8build\boost" -i!boost_1_72_0\boost
            Remove-Item "$output.7z"
        }
        else {
            Invoke-WebRequest -Uri "https://dl.bintray.com/boostorg/release/1.72.0/source/boost_1_72_0.zip" -OutFile "$output.zip"
            Write-Host "Unzipping archive..."
            Expand-Archive -path "$output.zip" -DestinationPath "$workpath\v8build\boost"
            Remove-Item "$output.zip"
        }
    }

    $env:BOOST_ROOT = Join-Path $workpath "v8build\boost\boost_1_72_0"
    Write-Host "##vso[task.setvariable variable=BOOST_ROOT;]$workpath\v8build\boost\boost_1_72_0"
}
