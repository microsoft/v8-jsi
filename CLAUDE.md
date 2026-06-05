# V8-JSI — Project Guide

V8-JSI implements the JavaScript Interface (JSI) on top of the **V8** JavaScript
engine, bridging native code and JavaScript. It ships as `v8jsi.dll` and is
consumed mainly by React Native for Windows and related Microsoft projects.

The project is transitioning from a standalone Google V8 build to building V8
from the **vendored Node.js sources** under `deps/nodejs/`. New development
targets the Node.js-based build.

## Environment

This is a Windows project. Builds and tooling run on Windows with **PowerShell**;
paths use backslashes and `$null`/`$env:` rather than POSIX equivalents. The
TypeScript tooling under `scripts/` runs on Node.js v24 directly (it uses
`--strip-types`, so there is no separate transpile step — run `node script.ts`).

## Build

`v8jsi.dll` is built from the vendored Node.js v24.x tree via `scripts/build.ts`
(which wraps `deps/nodejs/vcbuild.bat` with the right narrow-build flags).

Canonical entry point:
```powershell
Set-Location 'e:\GitHub\microsoft\v8-jsi\scripts'
node build.ts --platform x64 --configuration release   # or: debug
```

Raw `vcbuild.bat` (narrow-build flags spelled out):
```powershell
Set-Location 'e:\GitHub\microsoft\v8-jsi\deps\nodejs'
.\vcbuild.bat release v8jsi without-intl no-node no-cctest no-embedtest no-openssl-cli no-fuzzers no-nop no-overlapped-checker
```

The `no-*` flags switch MSBuild from the solution-level `Build` meta-target to an
explicit target list, skipping every Node.js binary v8-jsi does not ship
(node.exe, cctest, embedtest, openssl-cli, fuzzers, nop, overlapped-checker).
Running `vcbuild.bat release v8jsi without-intl` with no `no-*` flags falls
through to the legacy whole-solution build. See
[design-part-a.md](e:/GitHub/vmoroz/kb/v8-jsi/new-v8jsi-dll-phase1-cleanup/design-part-a.md)
for the full mechanism.

Clean build (for stale-artifact issues):
```powershell
Set-Location 'e:\GitHub\microsoft\v8-jsi\deps\nodejs'; .\vcbuild.bat clean
```

**Outputs** (`Release` or `Debug` per configuration):
- `deps/nodejs/out/<config>/v8jsi.dll` — the shared library
- `deps/nodejs/out/<config>/v8jsi_test.exe` — JSI test runner
- `deps/nodejs/out/<config>/node_api_tests.exe` — Node-API test runner

**Verifying a build succeeded:** `vcbuild.bat` prints a `Build took N m N s` line
on success and the output binaries get fresh timestamps. These are more reliable
signals than a shell pipeline's exit code, which can be misleading.

**Key build files:**
- `src/v8jsi.gyp` — main GYP target for v8jsi
- `deps/nodejs/node.gyp` — includes `v8jsi.gyp` when `build_v8jsi=1`
- `deps/nodejs/vcbuild.bat`, `deps/nodejs/configure.py` (`--build-v8jsi`)

**Build variables:**
- `v8jsi_root` = `../..` (from `deps/nodejs` to the repo root)
- `v8jsi_enable_inspector` = 1 — the inspector is **always enabled**; v8-jsi must
  ship with it on
- `v8jsi_enable_node_api` — always-on in `v8jsi.gyp` (defines `V8JSI_ENABLE_NODE_API`)
- `v8jsi_test_hooks` = 1 (default) — when 1, `v8jsi.dll` exports test-only hooks
  guarded by `#ifdef JSI_TESTING_ONLY` (e.g. `v8_jsi_test_post_foreground_task`).
  CI/release builds may set 0; test targets that reference a gated symbol then
  fail to link.

## Testing

```powershell
& 'e:\GitHub\microsoft\v8-jsi\deps\nodejs\out\Release\v8jsi_test.exe' --gtest_brief=1
& 'e:\GitHub\microsoft\v8-jsi\deps\nodejs\out\Release\node_api_tests.exe' --gtest_brief=1
```

`v8jsi_test.exe` runs each JSI test against two runtime parameterizations,
`.../0` and `.../1` (filter with `--gtest_filter=*0` / `*1`). Both are
ABI-backed; `/0` goes through the public `makeV8Runtime`, `/1` through
`makeJsiAbiRuntime` directly. Test sources: `src/jsitests_main.cpp`,
`src/jsi/test/testlib.cpp` (from Meta), `src/jsi/test/testlib_ext.cpp`
(V8-specific). Swap `Release`→`Debug` to exercise the Debug build (which compiles
in V8's DCHECKs).

Recent baseline: 116 JSI tests + 52 Node-API tests, x64 Release and Debug
(the exact counts shift as tests are added or removed).

## Source layout

### Core (`src/`)
- `V8JsiRuntime.cpp` / `V8JsiRuntime_impl.h` — legacy JSI runtime (uses C++
  exceptions)
- `v8_core.{h,cpp}` — shared V8 infrastructure (platform, isolate data, `TryCatch`
  wrapper) used by both the ABI and the Node-API surfaces
- `IsolateData.h` — V8 isolate data management
- `V8Instrumentation.{cpp,h}` — performance/heap instrumentation (backs the
  Node-API `jsr_instrumentation_*` surface)
- `MurmurHash.{cpp,h}` — hashing
- `v8jsi.cpp` — DLL exports / test entry points
- `jsitests_main.cpp` — JSI test main + runtime factories

### JSI interface (`src/jsi/`)
`jsi.h` / `jsi.cpp` / `jsi-inl.h` / `decorator.h` / `instrumentation.h` — the core
JSI interface (synced from `facebook/hermes`). `src/jsi/test/` holds the shared
test library.

### ABI layer (`src/jsi_abi/`)
The binary-stable C ABI for JSI (see "ABI architecture" below).
- `jsi_abi.h` — engine-agnostic C ABI header (shared with hermes-windows)
- `jsi_abi_helpers.h` — C++ helpers for the OrError encodings (shared)
- `jsi_abi_v8.cpp` — V8 implementation of the ABI (exception-free)
- `v8_jsi_config.h` — V8-specific config + `v8_create_runtime` / `v8_jsi_set_v8_flags`
- `JsiAbiRuntime.{h,cpp}` — C++ wrapper implementing `jsi::Runtime` over the ABI
  (shared with hermes-windows; uses exceptions for JSI compatibility)
- `v8_node_api_attach.h` — seam that attaches a Node-API surface to a `jsi_runtime`
- `jsi_abi_v8_internal.h` — in-DLL-only accessors (not a public/consumer header)

### Public headers (`src/public/`)
- `V8JsiRuntime.h` + `V8JsiRuntime.cpp` — the legacy public `makeV8Runtime` API
  (the `.cpp` is consumer-compiled source)
- `ScriptStore.h` — prepared-script storage interface
- `v8_api.h` — V8-specific Node-API config extensions

### Node-API (`src/node-api/`)
- `js_runtime_api.h` — the `jsr_*` JSI runtime API
- `v8_api_abi.cpp` — Node-API → JSI-ABI bridge
- standard Node-API headers (`js_native_api*.h`, `util-inl.h`, `env-inl.h`) are
  vendored from Node.js

### Platform / inspector
- `src/jsi/jsilib-windows.cpp`, `src/etw/` — Windows + ETW
- `src/inspector/` — V8 inspector integration (enabled)

## ABI architecture

> The JSI ABI surface (`src/jsi_abi/*.h`, `src/node-api/js_runtime_api.h`,
> `src/public/v8_api.h`) is **experimental and not yet ABI-stable** — each header
> carries a banner saying so. Nothing depends on it as a stable contract yet,
> which is why it can still be reshaped freely.

The ABI gives JSI a stable C interface so a single `v8jsi.dll` can serve
consumers compiled with different toolchains.

- **Creation is a flat function.** `v8_create_runtime(uint32_t
  requested_abi_version, jsi_configure_runtime_cb configure, void *configure_data)`
  — there is no factory vtable. It returns a `jsi_runtime*` (refcount 1) or NULL.
- **Versioning is out of band (Model B).** `JSI_ABI_VERSION` is a single
  monotonic integer; the consumer passes the version it compiled against and the
  implementation pins to it or fails if it exceeds the implementation's max. The
  C++ wrapper `makeJsiAbiRuntime` injects this for free. Vtables hold only
  function pointers (no version/reserved fields); an optional `get_abi_version()`
  reports the implementation's max.
- **Configuration uses a factory-owned pull-callback.** The factory allocates the
  opaque `jsi_config`, hands it to the consumer's `configure` callback to populate
  via the typed `v8_jsi_config_*` setters, then frees it — the handle never
  escapes the callback. Consumers do not allocate or free the config.
- **Optional capabilities go through `query_interface`** (COM-style, two outputs:
  vtable + instance) with immutable interface IDs. No optional interfaces are
  registered yet; it is the extension seam.
- **Process-global V8 engine flags** (`sparkplug`, `jitless`, …) are set via the
  process-level `v8_jsi_set_v8_flags` (forwarded to `V8::SetFlagsFromCommandLine`),
  not per-runtime config, because V8 reads them only at first process init.

The V8 implementation (`jsi_abi_v8.cpp`) is **exception-free** to match
V8/Node.js style: it uses a `TryCatch` wrapper and V8's `Maybe`/`MaybeLocal`
patterns, returns `jsi_error_*` codes, and can compile with exceptions disabled.

`makeV8Runtime` (the public JSI entry point) is itself ABI-backed — it builds a
runtime through the same ABI path as the Node-API surface.

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

## Dependencies & binary size
V8 + `v8_libplatform`, ICU, OpenSSL, libuv, etc. — all from the vendored Node.js
tree. ICU adds ~30 MB; a full build is ~67 MB, and `--without-intl` (the default
narrow build) is ~33 MB.

## Scripts and tooling
The `scripts/` folder holds the TypeScript build/sync tooling (`build.ts`,
`sync.ts`, AI-merge, plus tests via `npm test`, type-checking via
`npm run typecheck`, and lint/format via `npm run fix`). Run TypeScript with
`node <script>.ts` directly.
