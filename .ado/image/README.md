# v8-jsi CI/Release VM images

This folder holds the **1ES managed-image definitions** used by the v8-jsi
Azure DevOps pipelines. Each JSON is an ordered list of provisioning *artifacts*
(tools/features) applied to a base Windows Server 2025 image to produce the VM
image the build agents run on.

| File | Architecture | Built image name |
|------|--------------|------------------|
| [`windows-2025-1espt-x64.json`](./windows-2025-1espt-x64.json) | x64 (also runs x86) | `windows-2025-1espt` |
| [`windows-2025-1espt-arm64.json`](./windows-2025-1espt-arm64.json) | ARM64 (native) | `windows-2025-1espt-arm64` |

Both images share one toolchain — **Visual Studio 2026 Enterprise + Clang, the
current Windows SDK, Node.js 24, Python 3.13 in the Azure Pipelines tool cache,
the latest Python on `PATH`, a native Python 3.14 off `PATH`, and .NET 10** —
so x64, x86, and ARM64 all build and
test with the same tools. The two JSONs are intentionally kept as close to
identical as possible; they differ only where the architecture forces it (see
[Per-architecture differences](#per-architecture-differences)).

## Pool ↔ image mapping (how pipelines select an image)

The build/test jobs run in two different ways depending on target architecture:

- **x64 / x86** → pool **`fabric-internal-pool-large`**, image **`windows-2025-1espt`**,
  selected with an explicit image demand:
  ```yaml
  pool:
    name: fabric-internal-pool-large
    demands: ImageOverride -equals windows-2025-1espt
  ```
  `fabric-internal-pool-large` hosts multiple images, so the image **must** be
  named via `ImageOverride`. This is set once on the top-level pool in
  `.ado/windows-jobs.yml`, and every x64/x86 job inherits it (the x64 sandbox job
  also states it explicitly in its non-ARM64 branch).

- **ARM64** → pool **`windows-2025-1espt-arm64`**, image **`windows-2025-1espt-arm64`**,
  with **no** image demand:
  ```yaml
  pool:
    name: windows-2025-1espt-arm64
    os: windows
    hostArchitecture: arm64
  ```
  `windows-2025-1espt-arm64` is a **single-image pool** — `windows-2025-1espt-arm64`
  is its one default image — so **no `ImageOverride` demand is used (or needed)**.
  ARM64 binaries can't execute on an x64 agent, so ARM64 cells build *and run their
  tests* natively on this pool. `hostArchitecture: arm64` is required: 1ES Pipeline
  Templates default it to `amd64`, and the 1ES PT pre-job fails an `amd64` host on
  an ARM VM SKU (`An amd64 architecture has been specified, but this is an arm64
  pool`).

> The `windows-release.yml` pipeline runs its publish/download jobs on the hosted
> `Azure-Pipelines-1ESPT-ExDShared` pool (it does not build v8-jsi), so it does not
> use `fabric-internal-pool-large` and needs no `ImageOverride`.

## How the images are built and refreshed

These JSONs are the **source of truth** for the image contents, but they are not
applied by the CI pipeline itself. They are consumed by the 1ES managed-image
build, which provisions a fresh VM, runs each artifact in order, and captures the
result as the named image. Building + replicating a new image takes time (hours),
so a pipeline run always uses the *currently published* image, not the JSON on the
branch. **After editing a JSON, the image must be rebuilt** before pipelines see
the change.

Most artifacts accept parameters; the managed-image build passes each JSON
`parameters` entry to the artifact **by name**.

## Visual Studio toolset (shared by both images)

Both images install VS 2026 Enterprise via `windows-visualstudio-bootstrapper`
(`VSBootstrapperURL: https://aka.ms/vs/18/stable/vs_Enterprise.exe` — VS "18" is
the 2026 product line), with the **same** workload string ending in
`--includeRecommended --includeOptional`:

| Component(s) | Why |
|--------------|-----|
| `Workload.ManagedDesktop`, `Workload.NativeDesktop`, `Workload.Universal` | .NET desktop, C++ desktop, and UWP workloads |
| `ComponentGroup.NativeDesktop.Core`, `ComponentGroup.UWP.Support`, `ComponentGroup.UWP.VC` | core native + UWP support |
| `Component.MSBuild`, `VC.CoreBuildTools`, `VC.CoreIde` | MSBuild + core C++ build tooling |
| `VC.Tools.x86.x64` **and** `VC.Tools.ARM64` | native MSVC for x64 **and** ARM64 |
| `VC.Llvm.Clang`, `VC.Llvm.ClangToolset` | Clang/LLVM (Clang 22 ships in VS 2026); the Node.js ≥ 24 build uses `clang-cl` |
| `VC.CMake.Project` | CMake (used by the sandbox `gn`/`ninja` build) |
| `Windows11SDK.26100` | current Windows SDK |
| `Windows11SDK.22621` | **kept deliberately** — consumers (e.g. react-native-windows default props) still target `10.0.22621.0` |
| `Windows11Sdk.WindowsPerformanceToolkit` | WPA/WPR/xperf |
| `VC.ATL`, `VC.ATLMFC`, `VC.ATL.ARM64`, `VC.MFC.ARM64`, `UWP.VC.ARM64` | ATL/MFC/UWP for x64 and ARM64 |
| `VC.Runtimes.x86.x64.Spectre`, `VC.Runtimes.ARM64.Spectre`, `VC.ATL.Spectre`, `VC.ATL.ARM64.Spectre` | Spectre-mitigated runtimes required by SDL |

## Artifacts (in provisioning order)

Shared by both images unless marked. Ordering matters — helper/runtime artifacts
come before the artifacts that depend on them.

| Artifact | Purpose |
|----------|---------|
| `windows-EnableDeveloperMode` | Enable Windows Developer Mode |
| `windows-enable-long-paths` | Enable NTFS long paths (deep `node_modules` / vendored source trees) |
| `Windows-ServerAddFeature` (`Web-Server`) | IIS Web Server role |
| `Windows-ServerAddFeature` (`Web-Scripting-Tools`) | IIS scripting tools |
| `windows-gitinstall` | Git for Windows |
| `windows-git-lfs` | Git LFS (required — some build inputs, e.g. the vendored `gn.exe`, are LFS objects) |
| `windows-AzPipeline-ImageHelpers` | Azure Pipelines image-helper PowerShell modules used by later artifacts |
| `windows-AzPipeline-InitializeVM` | Baseline VM initialization |
| `windows-AzPipeline-powershellCore` | PowerShell 7 (`pwsh`); arch-aware (native on both) |
| `windows-1es-install-winget` | **ARM64 only** — provision winget (all-users, sysprep-safe) into the shipped image |
| `windows-AzPipeline-7zip` | **x64 only** — install 7-zip through Chocolatey; see [7-zip note](#7-zip) |
| `windows-powershell-invokecommand` ([`install-winget-packages.ps1`](./install-winget-packages.ps1)) | **ARM64 only** — install the latest native 7-zip and native Python `3.14.x` (off `PATH`) through winget; see [7-zip note](#7-zip) and [Python note](#python) |
| `windows-add-to-path` | Add the 7-zip installation directory to the machine `PATH` |
| `windows-chocolatey` (`nasm`) | **x64 only** — install NASM for native builds |
| `windows-visualstudio-bootstrapper` | VS 2026 Enterprise + the workload above |
| `Windows-NodeJS` | Node.js `24.x` (`UseARM` selects the architecture) |
| `windows-1es-starter-install-python` | Latest Python `3.13.*` x64 build in the Azure Pipelines tool cache; see [Python note](#python) |
| `windows-1es-starter-install-python` (`3.14.*`) | **x64 only** — latest Python `3.14.*` x64 build in the tool cache, the build scripts' fallback; see [Python note](#python) |
| `windows-install-python` | Latest native python.org build on `PATH`; see [Python note](#python) |
| `windows-chrome` | Google Chrome (UI/WinAppDriver tests) |
| `windows-AzPipeline-WinAppDriver` | WinAppDriver |
| `windows-dotnetcore-sdk` | .NET SDK; see [.NET note](#net-sdk) |
| `windows-setenvvar` (`DOTNET_ROOT_X64`) | **ARM64 only** — point x64 .NET hosts at the x64 runtime; see [.NET note](#net-sdk) |
| `windows-1es-pt-prerequisites-v2` | Provision the private tool cache and dependencies required by 1ES Pipeline Templates; see [1ES prerequisites](#1es-pipeline-template-prerequisites) |
| `Windows-AzureCLI` | Azure CLI |
| `windows-updateregistry` (`BackgroundDownloadDisabled`) | Disable Visual Studio Installer background downloads in both Setup registry hives; see [Visual Studio background downloads](#visual-studio-background-downloads) |

## Per-architecture differences

Everything else is identical; only these entries differ between the two JSONs:

| Aspect | x64 image | ARM64 image |
|--------|-----------|-------------|
| 7-zip | `windows-AzPipeline-7zip` through Chocolatey | latest native release through winget ([`install-winget-packages.ps1`](./install-winget-packages.ps1)) |
| winget | not provisioned separately | `windows-1es-install-winget` added |
| NASM | installed through `windows-chocolatey` | not installed separately |
| `Windows-NodeJS` | `Version: 24.x`, `UseARM: false` | `Version: 24.x`, `UseARM: true` |
| Tool-cache Python 3.13 | x64 | x64 (runs under emulation) |
| Latest Python on `PATH` | `Architecture: x64` | `Architecture: arm64` (native) |
| Native Python `3.14.x` off `PATH` | tool cache, through a second `windows-1es-starter-install-python` (`versions: 3.14.*`) | through winget ([`install-winget-packages.ps1`](./install-winget-packages.ps1)); the tool-cache artifact installs only x64 builds |
| `.NET` | one `windows-dotnetcore-sdk` (native) | **two** — native arm64 **plus** an x64 SDK under `%ProgramFiles%\dotnet\x64` + `DOTNET_ROOT_X64` |

## Key decisions

### Python

The images install Python twice because the artifacts serve different consumers:

1. `windows-1es-starter-install-python` installs the latest matching Python
   `3.13.*` x64 release into the Azure Pipelines hosted tool cache. Tasks such as
   `UsePythonVersion@0`, including tasks invoked by SDL tooling, resolve Python
   only from this cache. The factory artifact currently publishes x64 Python, so
   the ARM64 image runs this cached copy under emulation when a task requests it.
2. `windows-install-python` (`Version: latest`) then installs the newest stable
   native python.org release to `C:\Python` and adds it to the machine `PATH`.
   Direct build scripts that invoke `python` continue to use this floating,
   architecture-native installation.

Keep the tool-cache artifact before the `latest` artifact. The second install
remains the default on `PATH` while the pinned minor version remains available
to Azure Pipelines tasks.

The floating `PATH` Python can be newer than the vendored Node.js `configure`
accepts (Python 3.15.0 was rejected by Node.js 24's 3.9–3.14 range).
`scripts/build.ts` and `scripts/sbox-build.ts` handle this without pinning the
`PATH` Python: they read the accepted versions from `deps/nodejs/configure`. If
the `PATH` Python is outside that range, they put the best accepted Python
found in the registry (PEP 514) or the Azure Pipelines tool cache first on
`PATH` for the build, preferring a native interpreter over an emulated one,
then the newest version.

- x64 image: a second `windows-1es-starter-install-python` entry
  (`versions: 3.14.*`) adds native Python 3.14 to the tool cache, so the build
  uses the newest version Node.js accepts; the tool-cache 3.13 remains a native
  fallback. Keep the `3.13.*` entry, which the CodeQL step and the 1ES
  prerequisites artifact use.
- ARM64 image: `windows-1es-starter-install-python` installs only x64 builds
  (the tool cache holds `…\Python\<version>\x64`), so a `3.14.*` entry there
  would add another emulated copy. Windows does not set
  `PROCESSOR_ARCHITEW6432` for emulated x64 processes, so `configure` would see
  an x64 host and cross-compile arm64 with emulated host tools. The ARM64 image
  therefore installs native Python `3.14.x` through winget
  (`Python.Python.3.14`) instead. It is registered under
  `HKLM\SOFTWARE\Python\PythonCore` but kept off `PATH`, without the `py`
  launcher or file associations. The manifest's machine installer defaults to
  `PrependPath=1`, so the script overrides its arguments.

Both 3.14 installs float on 3.14 servicing releases. Raise the minor version
only when the vendored Node.js drops 3.14 from `acceptable_pythons`.

Do not remove the tool-cache artifact, and do not pin `windows-install-python`
to work around a new Python release.

### 1ES Pipeline Template prerequisites

`windows-1es-pt-prerequisites-v2` is required on images used by 1ES Pipeline
Templates. It initializes the private tool cache and provisions the tools and
packages used by injected pipeline tasks without adding them to the general
`PATH`. Its `KVSecret_AppSecret` value identifies the shared read-only 1ES
identity used to retrieve those packages and should not be changed.

The explicit `windows-1es-starter-install-python` step runs earlier, so Python
3.13 is already present in the tool cache when the prerequisites artifact runs.
The artifact reuses a matching major/minor installation instead of downloading
another Python 3.13 copy.

### .NET SDK

All `windows-dotnetcore-sdk` entries float on the latest **.NET 10.0** servicing
build via `Channel: 10.0` + `DotNetCoreVersion: latest`, rather than pinning an
exact build. This picks up servicing/security patches automatically and avoids a
pinned build being de-listed. Note: the installer has **no `10.x` wildcard** for a
specific version — the supported "track a band" mechanism is the 2-part `Channel`,
and a 3-part `DotNetCoreVersion` would override (pin) the channel, so `latest` is
required for the channel to take effect.

### x64 .NET on the ARM64 image

The ARM64 image installs a **second, x64** .NET SDK side-by-side with the native
arm64 SDK (`Architecture: x64`, `InstallDir:
$env:ProgramFiles\dotnet\x64`) and sets the machine variable `DOTNET_ROOT_X64`
to `%ProgramFiles%\dotnet\x64`. The installer parameter uses PowerShell
expansion, while the persisted environment variable uses Windows environment
expansion; both remain independent of the system drive. Reason: the SBoM
(Software Bill of Materials) generation tool used by the release build is an
**x64** process; on a native ARM64 agent it can't load the arm64 `hostfxr.dll`
(`HRESULT 0x800700C1`, bad-image-format). Providing an x64 runtime it can
discover (via `DOTNET_ROOT_X64`, which an x64 .NET host probes first on an arm64
OS) lets SBoM run on the ARM64 cells.

### 7-zip

The x64 image uses `windows-AzPipeline-7zip`, which installs 7-zip through
Chocolatey.

The ARM64 image installs the latest native 7-zip through winget, using
[`install-winget-packages.ps1`](./install-winget-packages.ps1), embedded
base64-encoded as the `Script` of a `windows-powershell-invokecommand`
artifact. winget selects the
installer for the OS architecture and verifies its SHA-256 against the
community manifest, so there is no version or hash to maintain. Pinning is not
an option: the image rebuilds weekly, and the pinned-version artifact
(`windows-AzPipeline-Install-7zip`) finds installers only on the current 7-zip
download page, so every new 7-zip release broke the next rebuild.

`windows-add-to-path` then adds `%ProgramFiles%\7-Zip` to the machine `PATH`.
Using the environment variable keeps the image definition independent of the
system drive. This entry is required because
`windows-1es-starter-install-python`, which runs later, invokes `7z.exe` while
populating the Python tool cache.

### WinGet during image builds

The factory winget artifacts (`windows-AzPipeline-7zip` on ARM64,
`windows-install-7zip`, `windows-1es-install-winget-packages`) cannot use winget
in this managed-image build. Probing the ARM64 builder showed why, and the
script handles each point in order:

1. Artifacts run as the `vmadmin` build user in a service logon (session 0),
   started by `ImageFactoryV3.VMAgent.exe`. PowerShell runs as an emulated x64
   process on the ARM64 builder, so `PROCESSOR_ARCHITECTURE` reports `AMD64`.
2. The base image provisions winget (App Installer) for all users, so
   `windows-1es-install-winget` leaves it as is — but it is not registered for
   `vmadmin`. Without a registration there is no
   `%LOCALAPPDATA%\Microsoft\WindowsApps\winget.exe` alias, and Windows refuses
   to start `winget.exe` from its package folder. Adding a directory to `PATH`
   cannot help. The script registers the provisioned package with
   `Add-AppxPackage -RegisterByFamilyName` and calls the alias.
3. winget then fails to deploy its own catalog package in this session
   (`0x8A15000F`, source data missing). Deploying the same Microsoft-signed
   `source.msix` with `Add-AppxPackage` works, so the script does that when the
   catalog is unusable.
4. Sysprep fails (`0x80073cf2`) on per-user packages that are not provisioned,
   so the script removes the per-user `Microsoft.Winget.*` catalog packages. It
   keeps the `DesktopAppInstaller` registration, which matches the provisioned
   version, and sets `SkipDesktopAppInstallerRemoval` under
   `HKLM:\SOFTWARE\ImageFactory` so ImageFactory's sysprep preparation does not
   remove it either.

The JSON carries the script base64-encoded because ImageFactory pastes the
`Script` value into a generated command line as a double-quoted PowerShell
string: quotes would end the string, and `$` would be expanded there. The base64
form contains neither. After editing the script, regenerate the entry from this
folder in PowerShell 7:

```powershell
$json = [IO.File]::ReadAllText("$PWD\windows-2025-1espt-arm64.json")
$b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(([IO.File]::ReadAllText("$PWD\install-winget-packages.ps1") -replace "`r`n", "`n")))
$value = "& ([ScriptBlock]::Create([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('$b64'))))" | ConvertTo-Json
$json = [regex]::Replace($json, '(?s)("name": "windows-powershell-invokecommand",\s*"parameters": \{\s*"Script": )"(?:[^"\\]|\\.)*"', { param($m) $m.Groups[1].Value + $value })
[IO.File]::WriteAllText("$PWD\windows-2025-1espt-arm64.json", $json, (New-Object Text.UTF8Encoding $false))
```

### Visual Studio background downloads

Both images set `BackgroundDownloadDisabled` to `1` under the Visual Studio
Setup policy hive and the non-policy Setup hive. This prevents the Visual
Studio Installer background updater from contacting the Microsoft CDN during a
build, which would violate pipeline network isolation. Both registry locations
are set because Visual Studio setup components can consult either location.

### Both Windows SDKs are kept

`Windows11SDK.26100` (current) and `Windows11SDK.22621` are both installed.
Consumers of these images still target `10.0.22621.0` in some projects, so 22621
must remain until those consumers migrate.

## Updating an image

1. Edit the relevant JSON (and keep the two in sync where the change is not
   architecture-specific).
2. Trigger a managed-image rebuild for the affected image(s).
3. Once the new image is published, the pools serve it automatically — pipelines
   need no change (they select by pool + `ImageOverride`, not by image version).

When changing anything that a build step depends on at runtime (a tool version, a
new SDK, an environment variable), rebuild the image **before** relying on it in
the pipeline; a branch edit to the JSON alone does not change the running agents.
