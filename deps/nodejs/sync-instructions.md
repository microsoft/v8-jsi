# V8-JSI Fork Patches on Node.js

This Node.js tree is vendored. The hunks below are v8-jsi-side additions
that the fork-sync must keep when upstream Node.js diverges around them.
Defaults are unchanged: every patch is gated on opt-in (`--build-v8jsi`,
`v8jsi` / `no-*` keywords, `target_arch=="ia32"`, etc.), so upstream
behavior is preserved when the new options aren't requested. Two exceptions:
the libnode embedding API and its tests are part of every Node.js build of
this tree, and Windows shared builds (`--shared`, vcbuild `dll`), which here
are always libnode, get the Hybrid CRT and hardening.

## Rule of thumb

Preserve the downstream contracts below while merging upstream's surrounding
changes. Do not blindly keep an entire conflicting hunk: upstream can replace
the API, compiler phase, or default that our patch originally extended.

## Contracts to preserve

- **`v8jsi` / `v8jsisb`** (vcbuild.bat keywords) and
  **`--build-v8jsi` / `--build-v8jsisb`** / **`build_v8jsi` / `build_v8jsisb`**
  (configure.py flag/var, node.gyp variable + `build_v8jsi==1` conditional
  `includes:` of `../../src/v8jsi.gyp` and `../../src/node-api/test/node_api_tests.gyp`).
- **Sandbox profile:** v8jsisb is jitless/lite with Turbofan, Maglev, Sparkplug,
  and WebAssembly off. The x64/ARM64 variants enable the V8 sandbox with a
  pointer-compression shared cage; x86 remains jitless without that 64-bit cage.
  Keep the ordinary v8jsi profile and non-v8jsi upstream defaults unchanged.
- **Build-time snapshot compiler:** mksnapshot still needs the compiler to
  generate embedded builtins when the shipped engine has Turbofan disabled.
  Keep the compiler dependencies and exclusion of both Turbolev frontend
  sources when Maglev is off, retaining `maglev-node-type.cc` for shared
  operation printing. The Maglev entry point in V8's
  `src/compiler/turboshaft/pipelines.h` is guarded by `V8_ENABLE_MAGLEV`,
  replacing the old phase stub with a disabled entry point.
- **Deterministic narrow profile:** v8-jsi configure disables Temporal before
  Rust detection and opts out of Node's LIEF/SEA tooling. An explicit Temporal
  opt-in is rejected; upstream Node builds retain their original defaults.
- **`no-node` / `no-cctest` / `no-embedtest` / `no-openssl-cli` / `no-fuzzers` /
  `no-nop` / `no-overlapped-checker`** in vcbuild.bat and the **"Additive
  target-list mode"** block that consumes them (`target_list`,
  `use_target_list`, the strip-leading-`;` line). Our explicit targets include
  `v8jsi_test`, `node_api_tests`, and `mkv8snapshot`.
- **x86 re-enablement** in vcbuild.bat (removal of *"32-bit Windows builds
  are not supported anymore"*, `msvs_host_arch` detection, `msbplatform=Win32`
  default, `--no-cross-compiling` for AMD64→x86), in configure.py
  (`'x86' : 'ia32'` mapping in `host_arch_win`), in node.gyp
  (`target_arch=="ia32"` block with `ImageHasSafeExceptionHandlers=false`),
  and in tools/v8_gypfiles/{toolchain.gypi, v8.gyp}
  (`OS=="win" and v8_target_arch=="ia32"` block, ia32
  `push_registers_asm.cc` source).
- **`where /R ..\..\src /T *.gyp*`** line in vcbuild.bat (reconfigure
  stamp watches v8-jsi-side gyp files).
- **`v8jsi_source_link_json`** in configure.py: an absolute path to the
  generated SourceLink map used by our GYP linker options.
- **Local metadata and licenses:** preserve `.syncignore`, these instructions,
  the SQLite and fast_float license additions, and the Git-ignore exceptions
  that keep them trackable. Snapshot mode with `keepLocalDeletions` preserves
  pruning, but newly introduced build inputs still need explicit review.
- **No eMudhra roots:** `src/node_root_certs.h` and `tools/certdata.txt` omit
  "emSign Root CA - G1", which upstream still ships after removing the other
  eMudhra roots. Keep it removed when a sync touches either file.
  `scripts/check-root-certs.ts` fails CI if an eMudhra root returns or Node's
  `kBlockedSPKIs` list loses an entry. Drop this contract once upstream
  removes the root.
- **libnode embedding API (version 1):** the local-only files
  `src/node_embedding_api.h`, `src/node_embedding_api.cc`, and
  `src/node_embedding_api_cpp.h`, listed in node.gyp's `node_sources` and
  headers. The header is a published ABI: `api/node_embedding.model.json` and
  `stela.libnode.config.json` (repository root) fail the build on any change.
  The `.cc` uses Node's embedder API and internals (`CommonEnvironmentSetup`,
  `LoadEnvironment`, `node_napi_env__::New`, `napi_env__`'s scope counters, the
  event loop, cppgc setup); adapt it when upstream changes them.
- **libnode embedding tests:** the local-only `test/embedding` files
  (`embedtest_c_api*`, `embedtest_c_cpp_api*`, `embedtest_main.cc`,
  `embedding-api-test-cases.js`, `test-embedding-c-api.js`,
  `test-embedding-c-cpp-api.js`, `preload-with-worker.js`,
  `use-linked-modules.js`); in `embedtest.cc`, `test_main_cpp_api` instead of
  `NODE_MAIN` and the `uv.h` include, because `embedtest_main.cc` owns `main`;
  and in node.gyp, those sources in `embedtest`, which is also built for the
  Windows shared library. vcbuild.bat's target list builds `shared_embedtest`
  with `embedtest`.
- **libnode on Windows:** `class NODE_EXTERN EmbedderSnapshotData` and its
  `struct NODE_EXTERN DeleteSnapshotData` in `src/node.h` (upstream exports
  neither, so `embedtest` can't link against libnode.dll); the static C++
  runtime for shared builds in common.gypi (`MSVC_runtimeType`) and
  configure.py (`force_dynamic_crt`); common.gypi's include of
  `../../src/libnode_settings.gypi` (the Hybrid CRT and hardening); and
  node.gyp's include of `../../src/libnode.gyp` with the libnode target's
  `libnode_stela_gen` dependency (the embedded API manifest).
- **Windows Temporal:** `deps/crates/crates.gyp` links `synchronization.lib` for
  Rust's `WaitOnAddress` and `WakeByAddress*` imports. Keep this link setting
  for both host tools (including `mksnapshot`) and the target library. Since
  Rust 1.100 the standard library gets these imports from synchronization.lib
  (`rustc --print native-static-libs`) instead of embedding them in the static
  library, as Rust 1.99 did. Drop this contract once upstream links the library.
- **OpenSSL version probe:** configure.py's `get_openssl_macros` adds
  `-DOPENSSL_NO_ASM` when the build uses no-asm OpenSSL (`openssl_no_asm`,
  which Windows ARM64 builds need). Otherwise, on a native Windows ARM64 host,
  the asm dispatch headers fall back to the pruned `linux-elf` configuration,
  the probe fails, and ncrypto is built for a pre-3.0 OpenSSL.
- **Full-Node inputs for libnode:** ICU (`deps/icu-small`), LIEF, amaro,
  undici, acorn, minimatch, `deps/uv/src/unix`, the Linux and macOS OpenSSL
  configurations, the OpenSSL command-line sources (`deps/openssl/openssl/apps`,
  which `node.gypi` makes every Node.js target depend on), the V8 valgrind
  header, `test/common`, `test/testpy`,
  `test/root.status`, and the four fixtures the embedding tests load are
  synced in full (see `.syncignore`), except the npm lockfiles in undici and
  minimatch and the sample keys, certificates, and requests in
  `deps/openssl/openssl/apps/*.pem`, which are not build inputs. Never restore
  key or certificate files that the build doesn't use. The inputs were copied
  in once from the upstream commit, because `keepLocalDeletions` keeps a
  pruned path absent even after its ignore rule is removed. Upstream marks a
  few of these files executable, and Windows checkouts drop that bit, so stage
  new copies with `git update-index --chmod=+x` where upstream has it. A few
  upstream files also match Git ignore rules here (undici's `src/build/`,
  mbedtls's `error.c` under LIEF); add those with `git add -f`.

## When to call human

- Upstream removed something we patched against (e.g. the `cctest` keyword
  goes away). Our `no-cctest` / additive-list logic depends on the symbol
  existing. Mark unresolved.
- Upstream re-enables x86 itself (then our re-enablement hunks are
  duplicates — drop ours, keep upstream).
- Upstream changes the `configure_flags` assembly inside the `if defined …`
  chain in a way that breaks ordering relative to our
  `if defined build_v8jsi` line.
