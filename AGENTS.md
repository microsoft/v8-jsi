# V8-JSI — Project Guide

V8-JSI implements the JavaScript Interface (JSI) on top of the **V8** JavaScript
engine, bridging native code and JavaScript. It ships as `v8jsi2.dll` and the
legacy `v8jsi.dll`, consumed mainly by React Native for Windows and related
Microsoft projects.

The project is transitioning from a standalone Google V8 build to building V8
from the **vendored Node.js sources** under `deps/nodejs/`. New development
targets the Node.js-based build.

## Environment

This is a Windows project. Builds and tooling run on Windows with **PowerShell
7.x (`pwsh`)**, not Windows PowerShell 5.1. Use `pwsh` YAML tasks and invoke
PowerShell scripts directly inside them. Paths use backslashes and `$null`/`$env:`
rather than POSIX equivalents. Exception: `scripts/install-prerequisites.ps1`
targets Windows PowerShell 5.1 so it runs on a new machine:
`powershell.exe -NoProfile -File .\scripts\install-prerequisites.ps1`.
The TypeScript tooling under `scripts/` runs on Node.js v24 directly (it uses
`--strip-types`, so there is no separate transpile step — run `node script.ts`).

## Build

`v8jsi2.dll` is built from the vendored Node.js v26.x tree via `scripts/build.ts`
(which wraps `deps/nodejs/vcbuild.bat` with the right narrow-build flags).
Node.js v24 remains the tooling runtime. Both engine variants disable Temporal
and Node-only LIEF tooling, so Rust is not a build prerequisite.

NASM must be preinstalled for x64/x86 targets; the build no longer downloads it.
CI uses only the build image's NASM. Developers can run
`scripts/install-prerequisites.ps1` or
`winget install --id NASM.NASM --exact --source winget`.

Run each example from the repository root.

Canonical entry point:
```powershell
node scripts\build.ts --platform x64 --configuration release   # or: debug
```

The legacy C++ JSI build uses `scripts/legacy-build.ts`, which passes the
`v8jsilegacy` token and `cfg` to `vcbuild.bat`. It supports x64, x86, and arm64,
with release or debug configuration:
```powershell
node scripts\legacy-build.ts --platform x64 --configuration release
node scripts\legacy-build.ts --platform x64 --configuration release --no-build --test --check --stage out\legacy\stage\x64-release
```
`--stage <dir>` requires `--no-build`; staging keeps PDBs separate from the
package payload. `ReactNative.V8Jsi.Windows` is released only to the internal
ms/react-native-public and office/office feeds, not nuget.org.

The legacy build shares `deps/nodejs/out/<Config>` with the other builds but
uses a different engine configuration. After switching, build again until
nearly a no-op before testing; never build variants concurrently there.

Raw `vcbuild.bat` (narrow-build flags spelled out):
```powershell
Set-Location .\deps\nodejs
.\vcbuild.bat release v8jsi without-intl no-node no-cctest no-embedtest no-openssl-cli no-fuzzers no-nop no-overlapped-checker
```

The `no-*` flags switch MSBuild from the solution-level `Build` meta-target to an
explicit target list, skipping every Node.js binary v8-jsi does not ship
(node.exe, cctest, embedtest, openssl-cli, fuzzers, nop, overlapped-checker).
Running `vcbuild.bat release v8jsi without-intl` with no `no-*` flags falls
through to the legacy whole-solution build.

Clean build (for stale-artifact issues):
```powershell
Set-Location .\deps\nodejs; .\vcbuild.bat clean
```

**Outputs** (`Release` or `Debug` per configuration):
- `deps/nodejs/out/<config>/v8jsi2.dll` — the shared library
- `deps/nodejs/out/<config>/v8jsi_test.exe` — JSI test runner
- `deps/nodejs/out/<config>/node_api_tests.exe` — Node-API test runner
- `deps/nodejs/out/<config>/v8jsi.dll` — legacy shared library
- `deps/nodejs/out/<config>/jsitests_legacy.exe`, `node_api_tests_legacy.exe`,
  `inspector_tests_legacy.exe` — legacy JSI, Node-API, and inspector test runners

**Verifying a build succeeded:** `vcbuild.bat` prints a `Build took N m N s` line
on success and the output binaries get fresh timestamps. These are more reliable
signals than a shell pipeline's exit code, which can be misleading.

**The sandbox variant shares the output folder.** `node build.ts
--build-v8jsisb` builds `v8jsi2sb.dll` (jitless, V8 sandbox) into the same
`deps/nodejs/out/<config>` as `v8jsi2.dll`. The first build after switching
between the two variants can leave V8's embedded builtins stale: it reports
success, but every runtime then crashes with 0xc0000005. After a switch, build
again until the build is nearly a no-op, then test. To run the sandbox with a
fresh engine, copy `v8jsi2sb.dll` and its PDB into
`deps/chromium/out/sandbox-x64` yourself; no script does it.

Architecture switches share those output folders too. The first build after
switching architecture can report missing embedded-blob symbols or leave stale
builtins. Build again until nearly a no-op before testing; never run two
architectures or variants concurrently in the same Node output directory.

**Key build files:**
- `src/v8jsi.gyp` — main GYP target for v8jsi
- `deps/nodejs/node.gyp` — includes `v8jsi.gyp` when `build_v8jsi=1`
- `deps/nodejs/vcbuild.bat`, `deps/nodejs/configure.py` (`--build-v8jsi`)

**Build variables:**
- `v8jsi_root` = `../..` (from `deps/nodejs` to the repo root)
- `v8jsi_enable_inspector` = 1 — the inspector is **always enabled**; v8-jsi must
  ship with it on
- `v8jsi_enable_node_api` — always-on in `v8jsi.gyp` (defines `V8JSI_ENABLE_NODE_API`)
- `v8jsi_test_hooks` = 1 (default) — when 1, `v8jsi2.dll` exports test-only hooks
  guarded by `#ifdef JSI_TESTING_ONLY` (e.g. `v8_jsi_test_post_foreground_task`).
  CI/release builds may set 0; test targets that reference a gated symbol then
  fail to link.

## Testing

```powershell
& '.\deps\nodejs\out\Release\v8jsi_test.exe' --gtest_brief=1
& '.\deps\nodejs\out\Release\node_api_tests.exe' --gtest_brief=1
```

`v8jsi_test.exe` runs each JSI test against two runtime parameterizations,
`.../0` and `.../1` (filter with `--gtest_filter=*0` / `*1`). Both are
ABI-backed; `/0` goes through the public `makeV8Runtime`, `/1` through
`makeJsiAbiRuntime` directly. Test sources: `src/jsitests_main.cpp`,
`src/jsi/test/testlib.cpp` (from Meta), `src/jsi/test/testlib_ext.cpp`
(V8-specific). Swap `Release`→`Debug` to exercise the Debug build (which compiles
in V8's DCHECKs).

Run both test runners for the configuration being validated; test counts change
as coverage is added or removed. `legacy-build.ts --no-build --test` runs the
legacy suites and supporting task, startup-policy, and CET tests.
`tests/legacy-acceptance/` exercises package consumers, 0.79.5 DLL replacement,
and coexistence with `v8jsi2.dll`.

### Hardware shadow-stack tests (Windows x64)

`node scripts\build.ts --platform x64 --configuration release --no-build --test`
runs the ordinary JSI and Node-API suites, static CET header gates (also for
cross-architecture builds), and the x64 launcher and CET rows. The launcher
is `deps/nodejs/out/Release/cetrun.exe`:

```powershell
& '.\deps\nodejs\out\Release\cetrun.exe' strict '.\deps\nodejs\out\Release\v8jsi_test.exe' --gtest_brief=1
```

`cetrun <off|compat|strict> <exe> [args]` creates a suspended child, sets its
policy, verifies it, and resumes it. `off` disables shadow stacks; `compat`
and `strict` enable them. `SKIP: user CET not available` is a coverage gap,
not a pass. `CETRUN_REQUIRE_CET=1` turns unavailability into failure;
`build.ts` sets it for evidence rows after probing a capable host.
CI CET coverage runs on Windows Server agents with required user-mode CET.
Death-test children do not inherit the launcher's policy: the CETCOMPAT
JSI test executable runs those children in compat mode on capable hosts.
Startup-policy tests instead launch explicit-policy children through cetrun.

For Node-API, `V8JSI_TEST_CET=strict` wraps each node_lite child (including
nested child launches), rather than just the parent test runner. Unavailable
CET is counted as a GoogleTest skip. `V8JSI_TEST_V8_FLAGS` supplies flags to
the JSI runner before V8 initialization; do not forward it to node_lite.

The x64 matrix runs the full JSI suite under `off` without extra flags.
The normal x64 matrix also runs the full strict JSI suite with
`--allow-natives-syntax`, then focused `*CetShadowStack.*` rows for interpreter
(`--no-sparkplug --no-maglev --no-turbofan`), jitless (`--jitless`), and native
interpreter stack (`--interpreted-frames-native-stack` plus interpreter flags).
All focused rows allow natives syntax and reject an empty selection.
Off/compat rows exercise Wasm and JSPI. Off plus `--cet-compatible` covers
`*CetShadowStack.*` and `*JsiAbiInspector.Cet*`, including debugger code generation
without hardware CET. Strict plus `--no-cet-compatible` tests JSPI policy gating
only, not unsafe deopt. Unavailable strict coverage prints a message locally
and emits a pipeline warning in Azure Pipelines.
The per-child strict Node-API suite follows, without JSI V8 flags. The jitless
sandbox engine runs its full JSI suite once under `off` and once under
`strict`, plus its per-child strict Node-API suite once, not the normal-engine
flag matrix.

## Symbols and Source Link

Every shipped Windows binary (v8jsi.dll, v8jsi2.dll, v8jsi2sb.dll, mkv8snapshot.exe,
sbox.exe, v8sbhost.dll, libnode.dll) has a PDB with a GitHub Source Link map:
`src/generate_source_link.ts` maps the repository root to
`raw.githubusercontent.com/microsoft/v8-jsi/<commit>/`, and the link embeds it
with lld-link `/pdbstream:sourcelink=` (`src/v8jsi.gyp`, `src/libnode.gyp`,
the sandbox `BUILD.gn` files). Three things keep the sources resolvable:

- The sandbox build uses gn `symbol_level = 2`, so Chromium records real source
  paths instead of `o:\fake\prefix`-relative ones (and the PDBs carry full
  debug information).
- `.gitattributes` checks out text files with LF line endings, the bytes GitHub
  serves; a debugger rejects a fetched file whose checksum differs from the PDB.
  Build from a checkout made after that change (or re-check out the files).
- `scripts/source-link-check.ts` fails a PDB whose map is missing, whose own
  sources aren't tracked files with the same case, or (on CI, or with
  `--strict`) whose map commit isn't HEAD or whose source checksums differ from
  Git. `build.ts --test`, `sbox-build.ts --check`, and `libnode-build.ts --check`
  run it; `node scripts/source-link-check.ts [--commit <sha>] <pdb>...` checks
  any PDB, such as one from a package.

The release pipeline publishes the PDBs to the internal symbol service
(`.ado/templates/publish-symbols.yml`).

## LibNode

`Microsoft.JavaScript.LibNode` packages Node.js from `deps/nodejs` built as a
shared library (libnode), with the C embedding API
(`deps/nodejs/src/node_embedding_api.h`, version 1) that node-api-dotnet uses.
`scripts/libnode-build.ts` builds, tests, checks, stages, packs, and
smoke-tests it for win-x64, win-arm64, linux-x64, linux-arm64, osx-x64, and
osx-arm64, each on a machine of its own architecture (an Apple silicon Mac also
builds and tests osx-x64, under Rosetta 2):

```powershell
node scripts\libnode-build.ts --platform x64 --configuration release --test --check --stage out\libnode\stage\win-x64
node scripts\libnode-build.ts --pack out\libnode\stage --output out\libnode\packages
node scripts\libnode-build.ts --smoke out\libnode\packages
```

- Windows runs `vcbuild.bat <config> <arch> dll cfg full-icu v8temporal` with
  the `no-*` flags; Linux and macOS run `configure --shared
  --with-intl=full-icu` and make the `node`, `embedtest`, and
  `shared_embedtest` targets. Each builds full ICU, Temporal (it needs Rust),
  and Node's other defaults. The build shares `deps\nodejs\out\<Config>` with
  the v8jsi build: use a separate worktree, or build again until nearly a
  no-op after switching.
- An Apple silicon Mac builds osx-x64 with `--dest-cpu=x64
  --no-cross-compiling`: clang targets x86_64 and Rosetta 2 runs the build's
  own x86_64 tools and the tests, so the startup snapshot and code cache stay
  on (configure turns them off in a cross build). It needs Rosetta 2
  (`softwareupdate --install-rosetta --agree-to-license`) and Rust's
  `x86_64-apple-darwin` target (`rustup target add x86_64-apple-darwin`).
- CI's Windows jobs build with Microsoft Rust, which BinSkim's BA2030 requires
  for Windows binaries: `RustInstaller@1` installs it from the office
  `RustTools` feed ahead of the image's public Rust on PATH. Builds of pull
  requests from forks skip that install and use the image's public Rust: their
  restricted token may lack access to that feed, and they skip Guardian. Linux
  and macOS use public Rust (the build container and the hosted Macs).
- `src/libnode_settings.gypi`, which `common.gypi` includes for shared builds,
  gives Windows the Hybrid CRT and v8jsi's hardening. On Linux it links
  libstdc++, libgcc, and libatomic into libnode.so and hides their symbols
  (with `src/libnode.version`), so the library needs only glibc 2.35 (Ubuntu
  22.04). Linux and macOS build the library as `libnode.so` and
  `libnode.dylib` (SONAME and install name), the names the packages ship.
  macOS Release builds compile without default debug information (GYP's Xcode
  default would put DWARF in every object and nearly fill the CI agents' disk),
  so source-level debugging of a local macOS LibNode build needs
  `--configuration debug`.
- Linux runs every step in the Ubuntu 22.04 containers that the Linux agent
  images provide: `bash scripts/libnode-docker.sh build node scripts/libnode-build.ts ...`
  for the build, tests, checks, and staging, and `... smoke ...` for
  `--smoke`. CI's network isolation blocks installing anything, so the agent
  images build both containers (`.ado/image/linux-image-provision.sh`) from
  `.ado/image/libnode-linux-build.Dockerfile` and
  `libnode-linux-smoke.Dockerfile`, tagged with each Dockerfile's hash; a job
  fails early when its agent image is older than the Dockerfile. After
  changing a Dockerfile or the provisioning script, run
  `node scripts/linux-image-recipe.ts`, which rewrites
  `.ado/image/azurelinux3-1espt-*.json`, and rebuild both Linux images.
- `--test` runs `tools/test.py embedding`: Node's embedding tests plus the C and
  C++ API tests in `deps/nodejs/test/embedding`. `--smoke` builds the C consumer
  and the .NET app in `tests/libnode/` from a packed package and runs
  `tests/libnode/features.js` with each; the .NET app restores only from the
  package folder.
- `--check` fails on any dependency outside the system libraries: on Windows
  any imported DLL outside `allowedImports`, on Linux any needed library
  outside glibc's (`allowedLinuxLibraries`), a glibc symbol newer than 2.35,
  or an exported C++ runtime symbol, and on macOS anything outside
  `/usr/lib` and `/System/Library`. Review a new dependency before allowing it.
- CI runs these steps in `.ado/libnode-jobs.yml`: one job per platform
  (Windows arm64 and Linux arm64 on the ARM64 pool, Linux x64 on
  `fabric-internal-pool-large`, both macOS builds on the hosted
  `macos-15-arm64`), then pack, which makes the six per-RID packages and the
  `Microsoft.JavaScript.LibNode` meta package, then a smoke job per platform
  (osx-x64 on the hosted Intel `macos-15`).
  Publishing builds sign libnode.dll and the packages. Its key should be
  CP-231522, the key for third-party and open-source components (libnode.dll
  is Node.js); until the pipeline's ESRP client can use it,
  `.ado/libnode-jobs.yml` passes CP-230012, v8jsi2.dll's key.
- The release pipeline (`.ado/windows-release.yml`) pushes the packages to
  ms/react-native-public and the libnode.dll symbols to the Symbol Server, and
  publishes the packages to nuget.org through ESRP Release, as hermes-windows
  does. It runs after every successful CI run of main, so each main commit
  publishes a new LibNode version there; nuget.org versions can't be replaced
  or deleted. Its Gate stage releases only CI runs of main: a manual run that
  picks another branch's CI run succeeds as "Skipped" and publishes nothing.
- The packages share `Microsoft.JavaScript.V8`'s version (`docs/versioning.md`):
  `--pack` reads `v8jsi_version` from `config.json`, which CI computes, unless
  `--semantic-version` overrides it.
- Package metadata takes the Node.js version from `deps/nodejs/src/node_version.h`.
- The packages' NOTICE is `scripts/nuget-libnode/NOTICE.txt`. Regenerate it
  with `node scripts/gen-notice.ts` after a fork-sync, and update the script's
  Rust standard library entry when the Rust toolchain links other crates into
  libnode.
- The embedding API has its own Stela configuration, `stela.libnode.config.json`,
  and baseline, `api/node_embedding.model.json`. The header predates Stela's
  conventions and can't change, so `header-check.ts` doesn't lint it.
  libnode.dll embeds its manifest through `src/libnode.gyp`.

## Source layout

### Core (`src/`)
- `V8JsiRuntime.cpp` / `V8JsiRuntime_impl.h` — legacy JSI runtime (uses C++
  exceptions)
- `v8_core.{h,cpp}` — shared V8 infrastructure (platform, isolate data, `TryCatch`
  wrapper) used by both the ABI and the Node-API surfaces
- `IsolateData.h` — V8 isolate data management
- `V8Instrumentation.{cpp,h}` — performance/heap instrumentation (backs the
  Node-API `v8_node_api_env_*` instrumentation surface)
- `MurmurHash.{cpp,h}` — hashing
- `v8jsi.cpp` — DLL exports / test entry points
- `jsitests_main.cpp` — JSI test main + runtime factories

### JSI interface (`src/jsi/`)
`jsi.h` / `jsi.cpp` / `jsi-inl.h` / `decorator.h` / `instrumentation.h` — the core
JSI interface (synced from `facebook/hermes`). `src/jsi/test/` holds the shared
test library.

### Legacy C++ JSI (`src/legacy/`)
The direct runtime preserves the 0.79.5 C++ JSI API. `sources.json` records
restored-source provenance and hashes, plus modern shared-source and inspector
donor pins. After reviewing a modern shared-file change (including whether an
inspector fix needs porting), run `node scripts/legacy-checks.ts --update-sources`
and review the manifest diff. This refreshes only modern shared and donor
hashes; restored-file and legacy inspector-copy hashes remain enforced.

### ABI layer (`src/jsi_abi/`)
The binary-stable C ABI for JSI (see "ABI architecture" below).
- `jsi_abi.h` — engine-agnostic C ABI header (shared with hermes-windows)
- `jsi_abi_stela.h` — the `JSI_ABI_*` annotation macros `jsi_abi.h` uses
- `jsi_abi_helpers.h` — C++ helpers for creating handles and checking value kinds
  (shared)
- `jsi_abi_v8.cpp` — V8 implementation of the ABI (exception-free)
- `v8_jsi_abi.h` — V8-specific exports: `v8_jsi_abi_runtime_create`, the
  `v8_jsi_abi_config_set_*` setters, `v8_jsi_abi_set_v8_flags`, and the
  startup-snapshot functions
- `JsiAbiRuntime.{h,cpp}` — C++ wrapper implementing `jsi::Runtime` over the ABI
  (shared with hermes-windows; uses exceptions for JSI compatibility)
- `v8_node_api_attach.h` — seam that attaches a Node-API surface to a `jsi_runtime`
- `jsi_abi_v8_internal.h` — in-DLL-only accessors (not a public/consumer header)

### Public headers (`src/public/`)
- `v8jsi_names.h` — narrow and wide DLL-name macros for Microsoft.JavaScript.V8
- `V8JsiRuntime.h` + `V8JsiRuntime.cpp` — the legacy public `makeV8Runtime` API
  (the `.cpp` is consumer-compiled source)
- `ScriptStore.h` — prepared-script storage interface
- `v8sbhost_client.h` + `v8sbhost_stela.h` — the C client API apps use to load
  `v8sbhost.dll` and run JS in the sandbox (ships in `include\v8sbhost`)

### Sandbox (`deps/chromium/`)
- `sbox/sbox.h` + `sbox_stela.h` — the plugin ABI between `sbox.exe`
  and `v8sbhost.dll`
- `v8sbhost/` — `v8sbhost.dll`: the client, coordinator, and engine personas (see
  `deps/chromium/v8sbhost/README.md`)
- Built by `node scripts/sbox-build.ts --target-cpu x64 --target generic` into
  `deps/chromium/out/sandbox-x64`; `--check` runs the export, CRT, size, and
  BinSkim gates

### Node-API (`src/node-api/`)
- `v8_node_api.h` — V8-specific Node-API runtime hosting and configuration
  extensions (ships with `v8_node_api_stela.h` in `include\node-api`)
- `v8_api_abi.cpp` — Node-API → JSI-ABI bridge
- standard Node-API headers (`js_native_api*.h`, `util-inl.h`, `env-inl.h`) are
  vendored from Node.js

### Platform / inspector
- `src/jsi/jsilib-windows.cpp`, `src/etw/` — Windows + ETW
- `src/inspector/` — V8 inspector integration (enabled)

The inspector listens only on `127.0.0.1`. Discovery and WebSocket upgrades accept
no Origin or exactly `devtools://devtools` and reject every other Origin.
Outbound limits are 64 MiB per connection, 128 MiB per listener, and 32
connections; overflow closes the connection.

## ABI architecture

> The JSI ABI, Node-API hosting, and sandbox surfaces (`src/jsi_abi/*.h`,
> `src/node-api/v8_node_api.h`, `src/public/v8sbhost_client.h`,
> `deps/chromium/sbox/sbox.h`) are **experimental and not yet ABI-stable** — each header
> carries a banner saying so. Nothing depends on it as a stable contract yet,
> which is why it can still be reshaped freely.

The ABI gives JSI a stable C interface so a single `v8jsi2.dll` can serve
consumers compiled with different toolchains.

- **Creation is a flat function.** `v8_jsi_abi_runtime_create(uint32_t
  abi_version, jsi_configure_runtime_cb configure, void *user_data,
  jsi_runtime **out_runtime)` — there is no factory vtable. It returns a
  `jsi_status` and sets `*out_runtime` to a `jsi_runtime*` (refcount 1), or NULL
  on failure.
- **Versioning is out of band (Model B).** `JSI_ABI_VERSION` (currently 2) is a
  single monotonic integer; the consumer passes the version it compiled against
  and the implementation pins to it, or fails with `jsi_status_version_mismatch`
  if it exceeds the implementation's max or predates version 2. The
  C++ wrapper `makeJsiAbiRuntime` injects this for free. Vtables hold only
  function pointers (no version/reserved fields); an optional `get_abi_version()`
  reports the implementation's max.
- **Configuration uses a factory-owned pull-callback.** The factory allocates the
  opaque `jsi_config`, hands it to the consumer's `configure` callback to populate
  via the typed `v8_jsi_abi_config_set_*` setters, then frees it — the handle
  never escapes the callback. Consumers do not allocate or free the config. The
  first failing setter poisons the config, and creation then fails with its
  status.
- **Optional capabilities go through `query_interface`** (COM-style, two outputs:
  vtable + instance) with immutable interface IDs. No optional interfaces are
  registered yet; it is the extension seam.
- **Process-global V8 engine flags** (`sparkplug`, `jitless`, …) are set via the
  process-level `v8_jsi_abi_set_v8_flags` (forwarded to `V8::SetFlagsFromCommandLine`),
  not per-runtime config, because V8 reads them only at first process init.
  Calls after V8 initialization are rejected.

The V8 implementation (`jsi_abi_v8.cpp`) is **exception-free** to match
V8/Node.js style: it uses a `TryCatch` wrapper and V8's `Maybe`/`MaybeLocal`
patterns, returns `jsi_status` codes, and can compile with exceptions disabled.

`makeV8Runtime` for `v8jsi2.dll` is itself ABI-backed — it builds a
runtime through the same ABI path as the Node-API surface.
See [Runtime hosting and migration notes](docs/runtime-hosting.md) for microtask
policies, code-cache behavior, and host task-runner and teardown rules.

## V8 API compatibility notes

When updating to newer V8 versions, watch for these renames/changes:
- `Utf8Length()` → `Utf8LengthV2()`; `WriteUtf8()` → `WriteUtf8V2()`; `Write()` → `WriteV2()`
- `GetPrototype()` → `GetPrototypeV2()`; `SetPrototype()` → `SetPrototypeV2()`
- `ScriptOrigin` no longer takes `Isolate*` as its first parameter
- `promise->GetIsolate()` removed — use `v8::Isolate::GetCurrent()`
- Property interceptor handlers now return the `v8::Intercepted` enum

## Preprocessor defines
- `V8JSI_EXPORT` / `V8JSI_IMPORT` — DLL export/import
- `V8JSI_ENABLE_INSPECTOR` — inspector support (enabled)
- `V8JSI_ENABLE_NODE_API` — Node-API integration
- `JSI_TESTING_ONLY` — gates test-only DLL exports (see `v8jsi_test_hooks`)
- `V8JSI_CET_STARTUP_CHECK` — process-dependent CET startup default; normal
  Windows x64 engine only, not the sandbox engine
- `V8JSI_TEST_WEBASSEMBLY` — enables flag tests that require WebAssembly

## Dependencies & binary size
V8 + `v8_libplatform`, ICU, OpenSSL, libuv, etc. — all from the vendored Node.js
tree. ICU adds ~30 MB; a full build is ~67 MB, and `--without-intl` (the default
narrow build) is ~33 MB.

## Scripts and tooling
The `scripts/` folder holds the TypeScript build tooling (`build.ts`,
`sbox-build.ts`, `header-check.ts`, and their helpers). In `scripts/`, `npm run typecheck`
type-checks it and `npm run header-check` runs the header gate below.
`npm run native-rid-test` (or `node --test scripts/native-rid.test.ts` from the
repo root) tests native package RID selection. Stela's own tests are `npm test`
in `tools/stela/`. Run `node --test scripts/nasm.test.ts scripts/install-prerequisites.test.ts`
from the repo root to test NASM discovery and prerequisite setup. Run TypeScript with
`node <script>.ts` directly.
Run `node --test scripts/cet-headers.test.ts` from the repo root to test CET header validation.

The public C ABI headers have a compile gate: `node --test
scripts/header-check.ts` extracts the Stela model once and compiles Stela's
generated header tests: each entry header and each generated `<lib>_stela.h`
on its own, as C and C++, with MSVC and VS clang, warnings as errors, for
x64, x86, and arm64. The same gate runs
Stela's generated layout tests, which check each library's struct layouts and
enumerator values on every target. It also requires `stela lint` to report no
findings and `stela diff` to find no breaks against the committed baselines in
`api/<library>.model.json`. A deliberate ABI break refreshes the baseline in
the same change (`node tools/stela/src/stela.ts baseline`), and an addition
needs the library's `*_ABI_VERSION` raised.
`tools/stela/` holds Stela, the C ABI header tool;
`node tools/stela/src/stela.ts vendor --check` verifies the generated
`<lib>_stela.h` files, and `stela.ts extract` writes the merged API model to
`out/stela/stela.model.json` (see `tools/stela/README.md`).

Every shipped binary that implements a C ABI embeds a Stela manifest (PE
resource `STELA` 1) describing the API it was built against; the
`binaries` entries in `stela.config.json` list each binary's libraries. The
gyp `v8jsi_stela_gen` action extracts and writes it for v8jsi2.dll and
v8jsi2sb.dll, so the v8jsi build needs the VS "C++ Clang Compiler for Windows"
component, as the sandbox build already does; `scripts/sbox-build.ts` writes
the v8sbhost.dll and sbox.exe manifests before ninja links. `stela show
<binary>` prints a binary's API, `stela verify <binary>` checks its exports
against the manifest, and `stela check <binary> --config stela.config.json`
diffs it against the current headers. `build.ts --test` runs verify and
check on the engine DLL, and `sbox-build.ts --check` on v8sbhost.dll and
sbox.exe (whose `otherExports: false` makes verify the exact export gate; there
is no separate allowlist). `build.ts --pack` ships each public header's model as
`build\native\stela\<library>.model.json`, including `v8sbhost` for
`include\v8sbhost\v8sbhost_client.h`, the client header apps compile against to
load v8sbhost.dll.
