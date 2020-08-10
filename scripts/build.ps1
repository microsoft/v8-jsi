# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
param(
    [string]$OutputPath = "$PSScriptRoot\out",
    [string]$SourcesPath = $PSScriptRoot,
    [string]$Platform = "x64",
    [string]$Configuration = "Release",
    [string]$Architecture = "win32",
    [switch]$UseClang,
    [switch]$UseLibCpp
)

$workpath = Join-Path $SourcesPath "build"
$jsigitpath = Join-Path $SourcesPath "src"

Remove-Item (Join-Path $workpath "v8build\v8\jsi") -Recurse -ErrorAction Ignore
Copy-Item $jsigitpath -Destination (Join-Path $workpath "v8build\v8\jsi") -Recurse -Force

Push-Location (Join-Path $workpath "v8build\v8")

# Generate the build system
$gnargs = 'v8_enable_i18n_support=false is_component_build=false v8_monolithic=true v8_use_external_startup_data=false treat_warnings_as_errors=false'

if ($Configuration -like "*android") {
    $gnargs += ' use_goma=false target_os=\"android\" target_cpu=\"' + $Platform + '\"'
}
else {
    if (-not ($UseLibCpp)) {
        $gnargs += ' use_custom_libcxx=false'
    }

    if ($Architecture -eq "uwp") {
        # the default target_winuwp_family="app" (which translates to WINAPI_FAMILY=WINAPI_FAMILY_PC_APP) blows up with too many errors
        $gnargs += ' target_os=\"winuwp\" target_winuwp_family=\"desktop\"'
    }

    $gnargs += ' target_cpu=\"' + $Platform + '\"'

    if ($UseClang) {
        #TODO (#2): we need to figure out how to actually build DEBUG with clang-cl (won't work today due to STL iterator issues)
        $gnargs += ' is_clang=true'
    }
    else {
        $gnargs += ' is_clang=false'
    }

    if ($Platform -like "?64") {
        # Pointer compression only makes sense on 64-bit builds
        $gnargs += ' v8_enable_pointer_compression=true'
    }
}

if ($Configuration -like "*ebug*") {
    $gnargs += ' enable_iterator_debugging=true is_debug=true'
}
else {
    $gnargs += ' enable_iterator_debugging=false is_debug=false'
}

$buildoutput = Join-Path $workpath "v8build\v8\out\$Architecture\$Platform\$Configuration"

Write-Host "gn command line: gn gen $buildoutput --args='$gnargs'"
& gn gen $buildoutput --args="$gnargs"
if (!$?) {
    Write-Host "Failed during build system generation (gn)"
    exit 1
}

& ninja -v -j 16 -C $buildoutput v8jsi jsitests | Tee-Object -FilePath "$SourcesPath\build.log"
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
if (!(Test-Path -Path "$OutputPath\license")) {
    New-Item -ItemType "directory" -Path "$OutputPath\license" | Out-Null
}
if (!(Test-Path -Path "$OutputPath\lib\$Architecture\$Configuration\$Platform")) {
    New-Item -ItemType "directory" -Path "$OutputPath\lib\$Architecture\$Configuration\$Platform" | Out-Null
}

# Binaries
if (!$PSVersionTable.Platform -or $IsWindows) {
    Copy-Item "$buildoutput\v8jsi.dll" -Destination "$OutputPath\lib\$Architecture\$Configuration\$Platform"
    Copy-Item "$buildoutput\v8jsi.dll.lib" -Destination "$OutputPath\lib\$Architecture\$Configuration\$Platform"
    Copy-Item "$buildoutput\v8jsi.dll.pdb" -Destination "$OutputPath\lib\$Architecture\$Configuration\$Platform"
}
else {
    #TODO (#2): .so
}

Copy-Item "$buildoutput\args.gn" -Destination "$OutputPath\lib\$Architecture\$Configuration\$Platform"

# Headers
Copy-Item "$jsigitpath\public\ScriptStore.h" -Destination "$OutputPath\build\native\include\"
Copy-Item "$jsigitpath\public\V8JsiRuntime.h" -Destination "$OutputPath\build\native\include\"
Copy-Item "$jsigitpath\jsi\jsi.h" -Destination "$OutputPath\build\native\include\jsi\"
Copy-Item "$jsigitpath\jsi\jsi-inl.h" -Destination "$OutputPath\build\native\include\jsi\"

# Source code - won't be needed after we have a proper ABI layer
Copy-Item "$jsigitpath\jsi\jsi.cpp" -Destination "$OutputPath\build\native\include\jsi\"
Copy-Item "$jsigitpath\jsi\instrumentation.h" -Destination "$OutputPath\build\native\include\jsi\"

# Miscellaneous
#Copy-Item "$SourcesPath\ReactNative.V8Jsi.Windows.nuspec" -Destination "$OutputPath\"
Copy-Item "$SourcesPath\ReactNative.V8Jsi.Windows.targets" -Destination "$OutputPath\build\native\"
Copy-Item "$SourcesPath\config.json" -Destination "$OutputPath\"
Copy-Item "$SourcesPath\LICENSE" -Destination "$OutputPath\license\"
Copy-Item "$SourcesPath\LICENSE.jsi.md" -Destination "$OutputPath\license\"
Copy-Item "$SourcesPath\LICENSE.v8.md" -Destination "$OutputPath\license\"

Write-Host "Done!"