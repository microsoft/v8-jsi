# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [System.IO.DirectoryInfo]$OutputPath = "$PSScriptRoot\out",
    [System.IO.DirectoryInfo]$SourcesPath = $PSScriptRoot,
    [string]$Platform = "x64",
    [string]$Configuration = "Release",
    [string]$AppPlatform = "win32",
    [switch]$UseClang,
    [switch]$UseLibCpp
)

$workpath = Join-Path $SourcesPath "build"
$jsigitpath = Join-Path $SourcesPath "src"

Remove-Item (Join-Path $workpath "v8build\v8\jsi") -Recurse -Force -ErrorAction Ignore | Out-Null
New-Item -Path (Join-Path $workpath "v8build\v8\jsi") -ItemType Directory | Out-Null
Copy-Item -Path (Join-Path $jsigitpath "*") -Destination (Join-Path $workpath "v8build\v8\jsi") -Recurse -Force | Out-Null

Push-Location (Join-Path $workpath "v8build\v8")

# TODO: v8_enable_webassembly=false will reduce the binary size by > 20%, but it causes crashes on x86; needs deeper investigation

# Generate the build system
$gnargs = 'v8_enable_i18n_support=false is_component_build=false v8_monolithic=true v8_use_external_startup_data=false treat_warnings_as_errors=false'

if ($AppPlatform -eq "android") {
    $gnargs += ' v8jsi_enable_napi=false use_goma=false target_os=\"android\" target_cpu=\"' + $Platform + '\"'
}
elseif ($AppPlatform -eq "linux") {
    $gnargs += ' v8jsi_enable_napi=false use_goma=false target_os=\"linux\" target_cpu=\"' + $Platform + '\"'
}
else {
if (-not ($UseLibCpp)) {
    $gnargs += ' use_custom_libcxx=false'
}

if ($AppPlatform -eq "uwp") {
    # the default target_winuwp_family="app" (which translates to WINAPI_FAMILY=WINAPI_FAMILY_PC_APP) blows up with too many errors
    $gnargs += ' target_os=\"winuwp\" target_winuwp_family=\"desktop\"'
}

$gnargs += ' target_cpu=\"' + $Platform + '\"'

}

if (($AppPlatform -ne "android") -and ($AppPlatform -ne "linux")) {
    if ($UseClang) {
        #TODO (#2): we need to figure out how to actually build DEBUG with clang-cl (won't work today due to STL iterator issues)
        $gnargs += ' is_clang=true'
    }
    else {
        $gnargs += ' is_clang=false'
    }
}

if ($Platform -like "?64") {
    # Pointer compression only makes sense on 64-bit builds
    $gnargs += ' v8_enable_pointer_compression=true'
}
# }

if ($Configuration -like "*ebug*") {
    $gnargs += ' is_debug=true'

    if (($AppPlatform -ne "android") -and ($AppPlatform -ne "linux")) {
        $gnargs += ' enable_iterator_debugging=true'
    }
}
else {
    $gnargs += ' enable_iterator_debugging=false is_debug=false'
}

$buildoutput = Join-Path $workpath "v8build\v8\out\$AppPlatform\$Platform\$Configuration"

Write-Host "gn command line: gn gen $buildoutput --args='$gnargs'"
& gn gen $buildoutput --args="$gnargs"
if (!$?) {
    Write-Host "Failed during build system generation (gn)"
    exit 1
}

$numberOfThreads = 2
if ($PSVersionTable.Platform -and !$IsWindows) {
    $numberOfThreads = [int](Invoke-Command -ScriptBlock { nproc }) * 2
} else {
    # We'll use 2x the number of cores for parallel execution
    $numberOfThreads = [int]((Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors) * 2
}

$ninjaExtraTargets = @()

if (($AppPlatform -ne "uwp") -and ($AppPlatform -ne "android")) {
    $ninjaExtraTargets += "jsitests"

    if ($AppPlatform -ne "linux") {
        $ninjaExtraTargets += "v8windbg"
    }
}

& ninja -v -j $numberOfThreads -C $buildoutput v8jsi $ninjaExtraTargets | Tee-Object -FilePath "$SourcesPath\build.log"
if (!$?) {
    Write-Host "Build failure, check logs for details"
    exit 1
}

Pop-Location

if (!(Test-Path -Path "$buildoutput\v8jsi.dll") -and !(Test-Path -Path "$buildoutput\libv8jsi.so")) {
    Write-Host "Build failure"
    exit 1
}

# Build is complete, prepare for NuGet packaging
if (!(Test-Path -Path "$OutputPath\build\native\include\jsi")) {
    New-Item -ItemType "directory" -Path "$OutputPath\build\native\include\jsi" | Out-Null
}
if (!(Test-Path -Path "$OutputPath\build\native\jsi\jsi")) {
    New-Item -ItemType "directory" -Path "$OutputPath\build\native\jsi\jsi" | Out-Null
}
if (!(Test-Path -Path "$OutputPath\license")) {
    New-Item -ItemType "directory" -Path "$OutputPath\license" | Out-Null
}
if (!(Test-Path -Path "$OutputPath\lib\$AppPlatform\$Configuration\$Platform")) {
    New-Item -ItemType "directory" -Path "$OutputPath\lib\$AppPlatform\$Configuration\$Platform" | Out-Null
}

# Binaries
if (!$PSVersionTable.Platform -or $IsWindows) {
    Copy-Item "$buildoutput\v8jsi.dll" -Destination "$OutputPath\lib\$AppPlatform\$Configuration\$Platform"
    Copy-Item "$buildoutput\v8jsi.dll.lib" -Destination "$OutputPath\lib\$AppPlatform\$Configuration\$Platform"

    if ($Platform -eq "arm64") {
        # Due to size limitations, copy only the stripped PDBs for ARM64
        Copy-Item "$buildoutput\v8jsi_stripped.dll.pdb" -Destination "$OutputPath\lib\$AppPlatform\$Configuration\$Platform\v8jsi.dll.pdb"
    } else {
        Copy-Item "$buildoutput\v8jsi.dll.pdb" -Destination "$OutputPath\lib\$AppPlatform\$Configuration\$Platform"
    }

    # Debugging extension
    if ($AppPlatform -ne "uwp") {
        Copy-Item "$buildoutput\v8windbg.dll" -Destination "$OutputPath\lib\$AppPlatform\$Configuration\$Platform"
        Copy-Item "$buildoutput\v8windbg.dll.pdb" -Destination "$OutputPath\lib\$AppPlatform\$Configuration\$Platform"
    }
}
else {
    #TODO (#2): .so
}

Copy-Item "$buildoutput\args.gn" -Destination "$OutputPath\lib\$AppPlatform\$Configuration\$Platform"

# Headers
Copy-Item "$jsigitpath\public\compat.h" -Destination "$OutputPath\build\native\include\"
Copy-Item "$jsigitpath\public\js_native_api.h" -Destination "$OutputPath\build\native\include\"
Copy-Item "$jsigitpath\public\js_native_api_types.h" -Destination "$OutputPath\build\native\include\"
Copy-Item "$jsigitpath\public\js_native_ext_api.h" -Destination "$OutputPath\build\native\include\"
Copy-Item "$jsigitpath\public\NapiJsiRuntime.cpp" -Destination "$OutputPath\build\native\include\"
Copy-Item "$jsigitpath\public\NapiJsiRuntime.h" -Destination "$OutputPath\build\native\include\"
Copy-Item "$jsigitpath\public\Readme.md" -Destination "$OutputPath\build\native\include\"
Copy-Item "$jsigitpath\public\ScriptStore.h" -Destination "$OutputPath\build\native\include\"
Copy-Item "$jsigitpath\public\V8JsiRuntime.h" -Destination "$OutputPath\build\native\include\"

Copy-Item "$jsigitpath\jsi\jsi.h" -Destination "$OutputPath\build\native\jsi\jsi\"
Copy-Item "$jsigitpath\jsi\jsi-inl.h" -Destination "$OutputPath\build\native\jsi\jsi\"

# Source code - won't be needed after we have a proper ABI layer
Copy-Item "$jsigitpath\jsi\jsi.cpp" -Destination "$OutputPath\build\native\jsi\jsi\"
Copy-Item "$jsigitpath\jsi\instrumentation.h" -Destination "$OutputPath\build\native\jsi\jsi\"

# Miscellaneous
Copy-Item "$SourcesPath\ReactNative.V8Jsi.Windows.targets" -Destination "$OutputPath\build\native\"
Copy-Item "$SourcesPath\ReactNative.V8Jsi.Windows.nuspec" -Destination $OutputPath

Copy-Item "$SourcesPath\config.json"    -Destination "$OutputPath\"
Copy-Item "$SourcesPath\LICENSE"        -Destination "$OutputPath\license\"
Copy-Item "$SourcesPath\LICENSE.jsi.md" -Destination "$OutputPath\license\"
Copy-Item "$SourcesPath\LICENSE.napi.md"  -Destination "$OutputPath\license\"
Copy-Item "$SourcesPath\LICENSE.v8.md"  -Destination "$OutputPath\license\"

Write-Host "Done!"
