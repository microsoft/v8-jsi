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
current Windows SDK, Node.js 24, the latest Python, and .NET 10** — so x64, x86,
and ARM64 all build and test with the same tools. The two JSONs are intentionally
kept as close to identical as possible; they differ only where the architecture
forces it (see [Per-architecture differences](#per-architecture-differences)).

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
| `windows-AzPipeline-7zip` | **x64 only** — install 7-zip via Chocolatey |
| `windows-AzPipeline-Install-7zip` | **ARM64 only** — install 7-zip by direct download (pinned version + SHA256); see [7-zip note](#7-zip-on-arm64) |
| `windows-visualstudio-bootstrapper` | VS 2026 Enterprise + the workload above |
| `Windows-NodeJS` | Node.js `24.x` (`UseARM` selects the architecture) |
| `windows-install-python` | Latest python.org build; see [Python note](#python) |
| `windows-chrome` | Google Chrome (UI/WinAppDriver tests) |
| `windows-AzPipeline-WinAppDriver` | WinAppDriver |
| `windows-dotnetcore-sdk` | .NET SDK; see [.NET note](#net-sdk) |
| `windows-setenvvar` (`DOTNET_ROOT_X64`) | **ARM64 only** — point x64 .NET hosts at the x64 runtime; see [.NET note](#net-sdk) |
| `Windows-AzureCLI` | Azure CLI |

## Per-architecture differences

Everything else is identical; only these entries differ between the two JSONs:

| Aspect | x64 image | ARM64 image |
|--------|-----------|-------------|
| 7-zip | `windows-AzPipeline-7zip` (Chocolatey) | `windows-AzPipeline-Install-7zip` (direct download, pinned) |
| winget | not provisioned separately | `windows-1es-install-winget` added |
| `Windows-NodeJS` | `Version: 24.x`, `UseARM: false` | `Version: 24.x`, `UseARM: true` |
| `windows-install-python` | `Architecture: x64` | `Architecture: arm64` (native) |
| `.NET` | one `windows-dotnetcore-sdk` (native) | **two** — native arm64 **plus** an x64 SDK at `C:\Program Files\dotnet\x64` + `DOTNET_ROOT_X64` |

## Key decisions

### Python

`windows-install-python` (`Version: latest`) installs the newest stable python.org
release to `C:\Python` and adds it to the machine `PATH`. It does **not** populate
the Azure Pipelines *hosted tool cache*. Consequently the pipelines do **not** use
the `UsePythonVersion@0` task (which resolves **only** from the tool cache) — the
build picks up `python` from `PATH`. If you reintroduce `UsePythonVersion@0`, you
must switch back to a tool-cache-populating Python artifact, or the task will fail
to find a version.

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
arm64 SDK (`Architecture: x64`, `InstallDir: C:\Program Files\dotnet\x64`) and sets
the machine variable `DOTNET_ROOT_X64` to that path. Reason: the SBoM (Software
Bill of Materials) generation tool used by the release build is an **x64** process;
on a native ARM64 agent it can't load the arm64 `hostfxr.dll`
(`HRESULT 0x800700C1`, bad-image-format). Providing an x64 runtime it can discover
(via `DOTNET_ROOT_X64`, which an x64 .NET host probes first on an arm64 OS) lets
SBoM run on the ARM64 cells.

### 7-zip on ARM64

The ARM64 image can't use the Chocolatey/winget 7-zip paths during provisioning
(winget isn't on the provisioning `PATH` at that point). It uses
`windows-AzPipeline-Install-7zip`, which downloads the installer directly from the
official `ip7z/7zip` GitHub release. That artifact **requires an integrity hash**:
it verifies the download against either an inline `SpecificVersionExpectedHash` or
its internal approved-hash list (which ships x64 hashes only). So the ARM64 entry
pins an explicit `InstallSpecificVersion` + `SpecificVersionExpectedHash` for the
arm64 installer. This is intentional — "install latest" is not possible here
without dropping the hash check. To move to a newer 7-zip, update the version and
its arm64 SHA256 together.

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
