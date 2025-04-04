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
$buildingWindows = !"android linux mac".contains($AppPlatform)

Remove-Item (Join-Path $workpath "v8\jsi") -Recurse -Force -ErrorAction Ignore | Out-Null
New-Item -Path (Join-Path $workpath "v8\jsi") -ItemType Directory | Out-Null
Copy-Item -Path (Join-Path $jsigitpath "*") -Destination (Join-Path $workpath "v8\jsi") -Recurse -Force | Out-Null

Push-Location (Join-Path $workpath "v8")

# TODO: v8_enable_webassembly=false will reduce the binary size by > 20%, but it causes crashes on x86; needs deeper investigation

# Generate the build system
$gnargs = 'v8_enable_i18n_support=false is_component_build=false v8_monolithic=true v8_use_external_startup_data=false treat_warnings_as_errors=false'

if (!$buildingWindows) {
    $gnargs += " use_goma=false target_os=""$AppPlatform"""
} else {
    if (-not ($UseLibCpp)) {
        $gnargs += ' use_custom_libcxx=false'
    }
}

$gnargs += ' target_cpu=\"' + $Platform + '\"'

if ($buildingWindows) {
    if ($UseClang) {
        #TODO (#2): we need to figure out how to actually build DEBUG with clang-cl (won't work today due to STL iterator issues)
        $gnargs += ' is_clang=true'
    } else {
        $gnargs += ' is_clang=false'
    }
}

if ($Platform -like "?64") {
    # Pointer compression only makes sense on 64-bit builds
    $gnargs += ' v8_enable_pointer_compression=true'
}

# TODO: N-API implementation of external ArrayBuffer doesn't comply with the Sandbox requirements for all memory allocations to be owned by the VM
$gnargs += ' v8_enable_sandbox=false'

if ($Configuration -like "*ebug*") {
    $gnargs += ' is_debug=true'
    # Building with iterator debugging turned on for ARM64 causes an error with mksnapshot (this MSVC option is no longer supported in upstream)
    if ($buildingWindows -and ($Platform -ne "arm64")) {
        $gnargs += ' enable_iterator_debugging=true'
    }
} else {
    $gnargs += ' enable_iterator_debugging=false is_debug=false'
}

$buildoutput = Join-Path $workpath "v8\out\$AppPlatform\$Platform\$Configuration"

Write-Host "gn command line: gn gen $buildoutput --args='$gnargs'"
& gn gen $buildoutput --args="$gnargs"
if (!$?) {
    Write-Host "Failed during build system generation (gn)"
    exit 1
}

# We'll use 2x the number of cores for parallel execution
$numberOfThreads = 2
if ($PSVersionTable.Platform -and !$IsWindows) {
    if ($IsMacOS) {
        $numberOfThreads = [int](Invoke-Command -ScriptBlock { sysctl -n hw.physicalcpu }) * 2
    } else {
        $numberOfThreads = [int](Invoke-Command -ScriptBlock { nproc }) * 2
    }
} else {
    $numberOfThreads = [int]((Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors) * 2
}

$ninjaExtraTargets = @()
if ($AppPlatform -ne "android") {
    $ninjaExtraTargets += "jsitests"
    $ninjaExtraTargets += "node_api_tests"

    if (($AppPlatform -ne "linux") -and ($AppPlatform -ne "mac")) {
        $ninjaExtraTargets += "v8windbg"
    }
}

& ninja -v -j $numberOfThreads -C $buildoutput v8jsi $ninjaExtraTargets | Tee-Object -FilePath "$SourcesPath\build.log"
if (!$?) {
    Write-Host "Build failure, check logs for details"
    exit 1
}

Pop-Location

if (!(Test-Path -Path "$buildoutput\v8jsi.dll") -and !(Test-Path -Path "$buildoutput\libv8jsi.so") -and !(Test-Path -Path "$buildoutput\libv8jsi.dylib")) {
    Write-Host "Build failure"
    exit 1
}

# Build is complete, prepare for NuGet packaging
if (!(Test-Path -Path "$OutputPath\build\native\include\node-api")) {
    New-Item -ItemType "directory" -Path "$OutputPath\build\native\include\node-api" | Out-Null
}
if (!(Test-Path -Path "$OutputPath\build\native\include\node-api-jsi")) {
    New-Item -ItemType "directory" -Path "$OutputPath\build\native\include\node-api-jsi" | Out-Null
}
if (!(Test-Path -Path "$OutputPath\build\native\include\node-api-jsi\ApiLoaders")) {
    New-Item -ItemType "directory" -Path "$OutputPath\build\native\include\node-api-jsi\ApiLoaders" | Out-Null
}
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
    Copy-Item "$buildoutput\v8windbg.dll" -Destination "$OutputPath\lib\$AppPlatform\$Configuration\$Platform"
    Copy-Item "$buildoutput\v8windbg.dll.pdb" -Destination "$OutputPath\lib\$AppPlatform\$Configuration\$Platform"
}
else {
    #TODO (#2): .so
}

Copy-Item "$buildoutput\args.gn" -Destination "$OutputPath\lib\$AppPlatform\$Configuration\$Platform"

# Headers
Copy-Item "$jsigitpath\node-api\js_native_api_types.h" -Destination "$OutputPath\build\native\include\node-api\"
Copy-Item "$jsigitpath\node-api\js_native_api.h" -Destination "$OutputPath\build\native\include\node-api\"
Copy-Item "$jsigitpath\node-api\js_runtime_api.h" -Destination "$OutputPath\build\native\include\node-api\"

Copy-Item "$jsigitpath\node-api-jsi\ApiLoaders\JSRuntimeApi.cpp" -Destination "$OutputPath\build\native\include\node-api-jsi\ApiLoaders\"
Copy-Item "$jsigitpath\node-api-jsi\ApiLoaders\JSRuntimeApi.h" -Destination "$OutputPath\build\native\include\node-api-jsi\ApiLoaders\"
Copy-Item "$jsigitpath\node-api-jsi\ApiLoaders\JSRuntimeApi.inc" -Destination "$OutputPath\build\native\include\node-api-jsi\ApiLoaders\"
Copy-Item "$jsigitpath\node-api-jsi\ApiLoaders\NodeApi_win.cpp" -Destination "$OutputPath\build\native\include\node-api-jsi\ApiLoaders\"
Copy-Item "$jsigitpath\node-api-jsi\ApiLoaders\NodeApi.cpp" -Destination "$OutputPath\build\native\include\node-api-jsi\ApiLoaders\"
Copy-Item "$jsigitpath\node-api-jsi\ApiLoaders\NodeApi.h" -Destination "$OutputPath\build\native\include\node-api-jsi\ApiLoaders\"
Copy-Item "$jsigitpath\node-api-jsi\ApiLoaders\NodeApi.inc" -Destination "$OutputPath\build\native\include\node-api-jsi\ApiLoaders\"
Copy-Item "$jsigitpath\node-api-jsi\ApiLoaders\V8Api.cpp" -Destination "$OutputPath\build\native\include\node-api-jsi\ApiLoaders\"
Copy-Item "$jsigitpath\node-api-jsi\ApiLoaders\V8Api.h" -Destination "$OutputPath\build\native\include\node-api-jsi\ApiLoaders\"
Copy-Item "$jsigitpath\node-api-jsi\ApiLoaders\V8Api.inc" -Destination "$OutputPath\build\native\include\node-api-jsi\ApiLoaders\"
Copy-Item "$jsigitpath\node-api-jsi\NodeApiJsiRuntime.cpp" -Destination "$OutputPath\build\native\include\node-api-jsi\"
Copy-Item "$jsigitpath\node-api-jsi\NodeApiJsiRuntime.h" -Destination "$OutputPath\build\native\include\node-api-jsi\"

Copy-Item "$jsigitpath\public\compat.h" -Destination "$OutputPath\build\native\include\"
Copy-Item "$jsigitpath\public\Readme.md" -Destination "$OutputPath\build\native\include\"
Copy-Item "$jsigitpath\public\ScriptStore.h" -Destination "$OutputPath\build\native\include\"
Copy-Item "$jsigitpath\public\v8_api.h" -Destination "$OutputPath\build\native\include\"
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
