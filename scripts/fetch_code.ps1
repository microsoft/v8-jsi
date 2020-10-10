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

# Apply patches
& git apply --ignore-whitespace (Join-Path $SourcesPath "scripts\patch\src.diff")

& gclient sync

Push-Location (Join-Path $workpath "v8build\v8\build")
& git apply --ignore-whitespace (Join-Path $SourcesPath "scripts\patch\build.diff")

Pop-Location
Pop-Location
Pop-Location

$verString = $config.version

$gitRevision = ""
$v8Version = ""

$vermap = $verString.Split(".")

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

(Get-Content "$SourcesPath\src\version.rc") -replace ('V8JSIVER_MAJOR', $vermap[0]) -replace ('V8JSIVER_MINOR', $vermap[1]) -replace ('V8JSIVER_BUILD', $vermap[2]) -replace ('V8JSIVER_V8REF', $v8Version.Replace('.', '_')) | Set-Content "$SourcesPath\src\version_gen.rc"

if ($AppPlatform -eq "uwp") {
    (Get-Content "$SourcesPath\ReactNative.V8Jsi.Windows.nuspec") -replace ('VERSION_DETAILS', "UWP, V8 version: $v8Version; Git revision: $gitRevision") -replace ('ipdb;', 'pdb;') -replace ('V8Jsi.Windows', 'V8Jsi.Windows.UWP') | Set-Content "$OutputPath\ReactNative.V8Jsi.Windows.UWP.nuspec"
} else {
    (Get-Content "$SourcesPath\ReactNative.V8Jsi.Windows.nuspec") -replace ('VERSION_DETAILS', "V8 version: $v8Version; Git revision: $gitRevision") | Set-Content "$OutputPath\ReactNative.V8Jsi.Windows.nuspec"
}

Write-Host "##vso[task.setvariable variable=V8JSI_VERSION;]$verString"

# Install build depndencies for Android
if ($PSVersionTable.Platform -and !$IsWindows) {
    $install_script_path = Join-Path $workpath "v8build/v8/build/install-build-deps-android.sh"

    & sudo bash $install_script_path
}

#TODO (#2): Use the .gzip for Android / Linux builds

# Restore NuGet packages
$nugetExe = Join-Path $workpath 'nuget.exe'
Invoke-WebRequest -Uri "$NugetDownloadLocation" -OutFile $nugetExe
& $nugetExe restore

$boostPkg = Select-Xml  -Path (Join-Path $SourcesPath 'packages.config') -XPath '//packages/package' |
            Select-Object -ExpandProperty Node |
            Where-Object { $_.id -eq 'boost' }

$env:BOOST_ROOT = Join-Path $workpath "v8build/boost.$($boostPkg.version)/lib/native/include"
Write-Host "##vso[task.setvariable variable=BOOST_ROOT;]$env:BOOST_ROOT"
