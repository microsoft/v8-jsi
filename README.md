# React Native V8 JSI adapter
A V8 adapter implemention of the JSI interface for the react-native framework.

[![Build Status](https://dev.azure.com/ms/v8-jsi/_apis/build/status/microsoft.v8-jsi?branchName=master)](https://dev.azure.com/ms/v8-jsi/_build/latest?definitionId=321&branchName=master)

## Requirements
[Optionally] download and install/unzip [Boost](https://www.boost.org/users/download/) and make sure the BOOST_ROOT environment variable is set (for example `SET BOOST_ROOT=d:\work\boost_1_71_0\` or `$Env:BOOST_ROOT="d:\work\boost_1_71_0\"`)

## Building
Run ./localbuild.ps1 in a PowerShell terminal; by default, this will only build the win32 x64 release version of the binary. Edit the file to specify other platforms, architectures or flavors.

### Out-of-sync issues
Until the JSI headers find a more suitable home, they're currently duplicated between the various repos. Code in jsi\jsi should be synchronized with the matching version of JSI from react-native (from https://github.com/facebook/hermes/tree/master/API/jsi/jsi).

### Build script patches
To regenerate after manual fix-ups, run `git diff --output=..\..\..\..\scripts\patch\build.diff --ignore-cr-at-eol` from `\build\v8build\v8\build\`.

## Contributing
See [Contributing guidelines](./docs/CONTRIBUTING.md) for how to setup your fork of the repo and start a PR to contribute to React Native V8 JSI adapter.

## License

The V8 JSI adapter, and all newly contributed code is provided under the [MIT License](LICENSE). Portions of the JSI interface derived from Hermes are copyright Facebook.

## Code of Conduct

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/). For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.