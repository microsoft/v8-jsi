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
1. The Visual Studio **C++ Clang Compiler for Windows** component
   (`Microsoft.VisualStudio.Component.VC.Llvm.Clang`). The build uses its
   libclang to embed the API manifest in `v8jsi.dll` (see
   [`tools/stela/README.md`](tools/stela/README.md)), and the sandbox build
   ([`scripts/sbox-build.ts`](scripts/sbox-build.ts)) compiles with its clang.

From the repo root, check and install NASM, an accepted Python, and Node.js 24+:
```PowerShell
powershell.exe -NoProfile -File .\scripts\install-prerequisites.ps1
```
The script asks once before installing missing tools through WinGet; `-CheckOnly`
reports without installing, and `-Yes` consents and accepts WinGet agreements
(installer/UAC prompts may remain). Existing accepted tools are kept.
WinGet is needed only to install missing tools.
If PowerShell blocks execution, consult Microsoft's
[execution-policy documentation](https://learn.microsoft.com/powershell/module/microsoft.powershell.core/about/about_execution_policies)
and your organization's policy. The script does not change policy.
It does not install Visual Studio C++/Clang, the Windows SDK, or Git.

##### Building v8jsi.dll from Node.js sources

The supported build uses vendored [Node.js 26.x sources](deps/nodejs/) through
[`dev.ps1`](dev.ps1) / [`scripts/build.ts`](scripts/build.ts). Outputs are
`v8jsi.dll`, `v8jsi_test.exe`, and `node_api_tests.exe` in
`deps/nodejs/out/{Release,Debug}/`, packaged as `Microsoft.JavaScript.V8`.

###### Developer task runner: `dev.ps1`

[`dev.ps1`](dev.ps1) at the repo root is the canonical entry point for
the supported build. Run `.\dev` with no arguments to see available commands.

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

Node.js syncs use snapshot mode with `keepLocalDeletions` to preserve the
pruned build closure. For a branch change, set the tracking branch in
`deps/nodejs/sync-config.json` before starting, but retain its old base commit.
Pass both the release tag and its peeled commit SHA to pin an annotated tag:
`.\dev fork-sync --dep nodejs --tag <tag> --commit <sha>`.
Without `--mergetool`, unresolved conflicts pause for manual resolution.
A dedicated sync worktree and its `.sync` cache can be reused on fresh branches
once the previous changes are committed and the worktree is clean.

See `.\dev build --help` and `.\dev fork-sync --help` for the full
option set. The `build` command forwards to
[`scripts/build.ts`](scripts/build.ts), which drives
[`deps/nodejs/vcbuild.bat`](deps/nodejs/vcbuild.bat) with narrow-build
flags (skips `node.exe`, `cctest`, `embedtest`, etc.). See
[`CLAUDE.md`](CLAUDE.md) for the underlying details.

`.\dev build` also runs a version-sync check that compares
`config.json.nodejs_version` against the vendored Node.js version in
`deps/nodejs/src/node_version.h`, and fails fast on mismatch. See
[`docs/versioning.md`](docs/versioning.md) for the `26.x.y` policy.

### Out-of-sync issues
Until the JSI headers find a more suitable home, they're currently duplicated between the various repos. Code in jsi\jsi should be synchronized with the matching version of JSI from react-native (from https://github.com/facebook/hermes/tree/master/API/jsi/jsi).

For the experimental Windows sandbox client API, see
[Sandboxed JavaScript through v8host.dll](docs/v8host-sandbox.md).

## Contributing
See [Contributing guidelines](./docs/CONTRIBUTING.md) for how to setup your fork of the repo and start a PR to contribute to React Native V8 JSI adapter.

## License

The V8 JSI adapter, and all newly contributed code is provided under the [MIT License](LICENSE). Portions of the JSI interface derived from Hermes are copyright Facebook.

## Code of Conduct

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/). For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.
