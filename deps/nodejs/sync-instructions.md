# V8-JSI Fork Patches on Node.js

This Node.js tree is vendored. The hunks below are v8-jsi-side additions
that the fork-sync must keep when upstream Node.js diverges around them.
Defaults are unchanged: every patch is gated on opt-in (`--build-v8jsi`,
`v8jsi` / `no-*` keywords, `target_arch=="ia32"`, etc.), so upstream
behavior is preserved when the new options aren't requested.

## Rule of thumb

If a conflict hunk contains any of the markers below, KEEP OURS for that
hunk and merge upstream's surrounding edits around it. Our hunks are
additive — there is no "either/or" with upstream.

## Markers to preserve (keep ours)

- **`v8jsi`** (vcbuild.bat keyword) and **`--build-v8jsi`** / **`build_v8jsi`**
  (configure.py flag/var, node.gyp variable + `build_v8jsi==1` conditional
  `includes:` of `../../src/v8jsi.gyp` and `../../src/node-api/test/node_api_tests.gyp`).
- **`no-node` / `no-cctest` / `no-embedtest` / `no-openssl-cli` / `no-fuzzers` /
  `no-nop` / `no-overlapped-checker`** in vcbuild.bat and the **"Additive
  target-list mode"** block that consumes them (`target_list`,
  `use_target_list`, the strip-leading-`;` line).
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

## When to call human

- Upstream removed something we patched against (e.g. the `cctest` keyword
  goes away). Our `no-cctest` / additive-list logic depends on the symbol
  existing. Mark unresolved.
- Upstream re-enables x86 itself (then our re-enablement hunks are
  duplicates — drop ours, keep upstream).
- Upstream changes the `configure_flags` assembly inside the `if defined …`
  chain in a way that breaks ordering relative to our
  `if defined build_v8jsi` line.
