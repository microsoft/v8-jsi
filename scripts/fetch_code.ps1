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

# Apply patches
& git apply (Join-Path $SourcesPath "scripts\patch\src.diff")

Push-Location (Join-Path $workpath "v8build\v8\build")
& git apply (Join-Path $SourcesPath "scripts\patch\build.diff")

# To re-generate these patches in the future use something like: 'git diff --output=path\src.diff --ignore-cr-at-eol' in the respective folders

Pop-Location
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
    if (-not (Test-Path (Join-Path $workpath "v8build/boost.1.71.0.0/lib/native/include/boost/asio.hpp"))) {
        Write-Host "Boost ASIO not found, downloading..."

        $targetNugetExe = Join-Path $workpath "nuget.exe"
        Invoke-WebRequest "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe" -OutFile $targetNugetExe

        & $targetNugetExe install -OutputDirectory (Join-Path $workpath "v8build") boost -Version 1.71.0
    }

    $env:BOOST_ROOT = Join-Path $workpath "v8build/boost.1.71.0.0/lib/native/include"
    Write-Host "##vso[task.setvariable variable=BOOST_ROOT;]$env:BOOST_ROOT"
}
