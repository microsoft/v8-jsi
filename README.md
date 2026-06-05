# React Native V8 JSI adapter
A V8 adapter implemention of the JSI interface for the react-native framework.

[![Build Status](https://dev.azure.com/ms/v8-jsi/_apis/build/status/microsoft.v8-jsi?branchName=master)](https://dev.azure.com/ms/v8-jsi/_build/latest?definitionId=321&branchName=master)

## Building

#### Windows

##### Initial Windows Build Setup

1. Download and Install  
   **Windows 10 Windows SDK**:  
   <https://developer.microsoft.com/en-us/windows/downloads/sdk-archive>  
   Note: Windows SDK has to be **Windows 10** version, NOT **Windows 11**
1. Download and Install  
   **Microsoft Visual C++ Redistributable x64**  
   **Microsoft Visual C++ Redistributable x86**  
   <https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist>
1. Download and Install  
   **Visual Studio 2022**  
   <https://visualstudio.microsoft.com/vs>  
   In Visual Studio Setup, Select the following **Individual Components**:
   * MSVC v143 - VS 2022 C++ x64/x86 Spectre-mitigated libs (Latest)
   * C++ ATL for latest v143 build tools with Spectre Mitigations (x86 & x64)
1. Enable PowerShell script execution  
   Launch Cmd as **Administrator**
   ```Cmd
   powershell Set-ExecutionPolicy RemoteSigned
   ```
1. Perform Initial Build  
   Launch Cmd as **Standard** user, NOT **Administrator**  
   From V8-Jsi Repo directory:
   ```
   powershell ./localbuild.ps1
   ```

##### Windows Build

To build Win32 X64 Debug:

Launch Cmd as **Standard** user, NOT **Administrator**  
From V8-Jsi Repo Directory:
```Cmd
powershell ./localbuild.ps1 -NoSetup
```

To build the specific platform and flavor, use appropriate build flags: 
```Cmd
powershell ./localbuild.ps1 -NoSetup -Platform x86 -Configuration Release 
```

##### Building v8jsi.dll from Node.js sources (new build, in development)

A second build path is being developed that compiles v8jsi.dll from a
vendored copy of Node.js (under [`deps/nodejs/`](deps/nodejs/)) without
`depot_tools` / `gclient`. It produces its own `v8jsi.dll`,
`v8jsi_test.exe`, and `node_api_tests.exe` into
`deps/nodejs/out/{Release,Debug}/` — the legacy build's outputs in
`out/{Release,Debug}/` are unaffected and continue to be the canonical
artifacts shipped in the `ReactNative.V8Jsi.Windows` NuGet today.

The two builds will coexist until downstream consumers migrate to the
new `Microsoft.JavaScript.V8` NuGet, at which point the legacy build
above is removed.

###### Additional prerequisites

In addition to the prerequisites listed under *Initial Windows Build
Setup*, the new build also needs:

1. **Python 3.x** on PATH (required by the Node.js `configure` step).
1. **Node.js v24+** on PATH (required by [`scripts/build.ts`](scripts/build.ts)).

###### Developer task runner: `dev.ps1`

[`dev.ps1`](dev.ps1) at the repo root is the canonical entry point for
the new build. Run `.\dev` with no arguments to see available commands.

**Build** (Release x64 by default):
```PowerShell
.\dev build
```

**Other configurations**:
```PowerShell
.\dev build --configuration debug
.\dev build --platform x86
.\dev build --platform arm64
```

**Build + run tests**:
```PowerShell
.\dev build --test
```

**Run the fork-sync tool** (sync vendored Node.js / asio from upstream):
```PowerShell
.\dev fork-sync --dep nodejs --status
```

See `.\dev build --help` and `.\dev fork-sync --help` for the full
option set. The `build` command forwards to
[`scripts/build.ts`](scripts/build.ts), which drives
[`deps/nodejs/vcbuild.bat`](deps/nodejs/vcbuild.bat) with narrow-build
flags (skips `node.exe`, `cctest`, `embedtest`, etc.). See
[`CLAUDE.md`](CLAUDE.md) for the underlying details.

`.\dev build` also runs a version-sync check that compares
`config.json.nodejs_version` against the vendored Node.js version in
`deps/nodejs/src/node_version.h`, and fails fast on mismatch. See
[`docs/versioning.md`](docs/versioning.md) for the `24.x.y` policy.

##### [EXPERIMENTAL!] Building with WSL2 on Windows 10/11
* [Enable](https://docs.microsoft.com/en-us/windows/wsl/install) Windows Subsystem for Linux
* Install debian: `wsl --install -d Debian`
* [Install PowerShell](https://docs.microsoft.com/en-us/powershell/scripting/install/install-debian?view=powershell-7.2) in the Debian VM
* Install minimal dependencies on the Debian VM: `sudo apt install lsb-release`
* Make sure you have at least 15Gb of disk space on the drive where the WSL image lives (usually C:)
* Build with `pwsh ./localbuild.ps1 -AppPlatform android`
* If setup is completed successfully, build incrementally with `pwsh ./localbuild.ps1 -AppPlatform android -NoSetup`

##### [EXPERIMENTAL!] Building on macOS
* [Install PowerShell](https://docs.microsoft.com/en-us/powershell/scripting/install/installing-powershell-on-macos?view=powershell-7.3) by running `brew install --cask powershell`
* Build with `pwsh ./localbuild.ps1 -AppPlatform mac`
* If setup is completed successfully, build incrementally with `pwsh ./localbuild.ps1 -AppPlatform mac -NoSetup`
* **Note**: there are several test failures on macOS currently

### Out-of-sync issues
Until the JSI headers find a more suitable home, they're currently duplicated between the various repos. Code in jsi\jsi should be synchronized with the matching version of JSI from react-native (from https://github.com/facebook/hermes/tree/master/API/jsi/jsi).

### Build script patches
To regenerate after manual fix-ups, run:
* `git diff --output=..\..\..\scripts\patch\build.diff --ignore-cr-at-eol` from `\build\v8\build\`.
* `git diff --output=..\..\scripts\patch\src.diff --ignore-cr-at-eol` from `\build\v8\`.
* `git diff --output=..\..\..\..\scripts\patch\zlib.diff --ignore-cr-at-eol` from `\build\v8\third_party\zlib\`.

## Contributing
See [Contributing guidelines](./docs/CONTRIBUTING.md) for how to setup your fork of the repo and start a PR to contribute to React Native V8 JSI adapter.

## License

The V8 JSI adapter, and all newly contributed code is provided under the [MIT License](LICENSE). Portions of the JSI interface derived from Hermes are copyright Facebook.

## Code of Conduct

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/). For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.
