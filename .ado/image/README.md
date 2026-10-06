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
the latest Python on `PATH`, and .NET 10** — so x64, x86, and ARM64 all build and
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
  ```
  `windows-2025-1espt-arm64` is a **single-image pool** — `windows-2025-1espt-arm64`
  is its one default image — so **no `ImageOverride` demand is used (or needed)**.
  ARM64 binaries can't execute on an x64 agent, so ARM64 cells build *and run their
  tests* natively on this pool.

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
| `windows-AzPipeline-Install-7zip` | **ARM64 only** — install a pinned native 7-zip with an approved hash; see [7-zip note](#7-zip) |
| `windows-add-to-path` | Add the 7-zip installation directory to the machine `PATH` |
| `windows-chocolatey` (`nasm`) | **x64 only** — install NASM for native builds |
| `windows-visualstudio-bootstrapper` | VS 2026 Enterprise + the workload above |
| `Windows-NodeJS` | Node.js `24.x` (`UseARM` selects the architecture) |
| `windows-1es-starter-install-python` | Latest Python `3.13.*` x64 build in the Azure Pipelines tool cache; see [Python note](#python) |
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
| 7-zip | `windows-AzPipeline-7zip` through Chocolatey | pinned native direct download with SHA-256 verification |
| winget | not provisioned separately | `windows-1es-install-winget` added |
| NASM | installed through `windows-chocolatey` | not installed separately |
| `Windows-NodeJS` | `Version: 24.x`, `UseARM: false` | `Version: 24.x`, `UseARM: true` |
| Tool-cache Python 3.13 | x64 | x64 (runs under emulation) |
| Latest Python on `PATH` | `Architecture: x64` | `Architecture: arm64` (native) |
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

`windows-1es-install-winget-packages` does not currently work on either
managed-image builder. When `winget.exe` is unavailable, the artifact's
`Microsoft.WinGet.Client` fallback crashes while installing 7-zip with exit code
`0xC0000409`. The ARM64 image therefore uses
`windows-AzPipeline-Install-7zip` with an explicit version and approved SHA-256.
Update both values together when the pinned release is removed from the current
7-zip download page.

`windows-add-to-path` then adds `%ProgramFiles%\7-Zip` to the machine `PATH`.
Using the environment variable keeps the image definition independent of the
system drive. This entry is required because
`windows-1es-starter-install-python`, which runs later, invokes `7z.exe` while
populating the Python tool cache.

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
