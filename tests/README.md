# tests/ — real-consumer integration (e2e) tests

These tests build a **real consumer** against the freshly packed
`Microsoft.JavaScript.V8.<rid>` NuGet and run it — validating the shipped package
surface (headers, import lib, consumer-side source TUs, runtime binaries, and the
offline tools) end to end, the way an external consumer would use it. They
complement the in-process gtests (`v8jsi_test.exe` / `node_api_tests.exe`, run via
`build.ts --test`), which exercise the engine from inside the DLL.

Run after a pack, from the repo root:

```powershell
node scripts/build.ts --no-build --pack --test-e2e --platform x64 --configuration release
```

`--test-e2e` runs the whole suite; `--smoke` runs just the consumer smoke. The
harness supports native x64, x86, and ARM64; pass `--platform x86` or
`--platform arm64` to target those (x86 builds the 32-bit Win32 consumer, which
runs via WOW64 on an x64 host). It needs `nuget.exe` + Visual Studio (MSBuild via
vswhere) on the machine. The sandbox-engine (`v8jsisb.dll`) and cross-engine legs
run only when the pack actually carries the sandbox binaries; otherwise they
auto-skip.

## Layout

- `consumer.vcxproj` / `main.cpp` — one consumer exe (`v8jsi_smoke.exe`) that
  serves every scenario. It links `v8jsi.dll.lib` and consumes the public JSI C++
  API (`makeV8Runtime`, `jsi::Runtime`). Built with `/p:UseV8Sandbox=true` it
  delay-load-redirects to `v8jsisb.dll` and normally imports `v8host.dll` through
  `v8host_client.h` and `v8host.dll.lib` (no client delay-load). Scenario
  is chosen by argv (see `main.cpp`).
- `test-helpers.ps1` — shared helpers: restore the per-RID `.nupkg`, locate MSBuild,
  build the consumer. Dot-sourced by every driver.
- `run-smoke.ps1` — **scenario 1 (smoke):** evaluates `1 + 2` and four cross-CRT
  exercises (utf8 round-trip, global set/get, HostFunction, JSError) through the
  JSI ABI; with `-UseV8Sandbox` also initializes the client API, opens a dedicated
  broker/session, runs a guest assertion, pumps callbacks with a bounded timeout,
  and closes the session. Requires `sbox.exe`, `v8host.dll`, and `v8jsisb.dll`
  beside the consumer in `bin\<Configuration>\<arch>\` (`x64` or
  `arm64`, case-insensitive); signed payloads are required by production
  verification. The smoke first checks, with MSBuild only, that an output
  directory not named for the architecture fails with `V8HOST001`.
  x86 consumers require an installed native payload layout (x64 on x64 Windows),
  not an x86 worker; this package-surface smoke does not qualify cross-arch pairs.
  With `-UseV8Sandbox` on x86 it only asserts that the package rejects
  `UseV8Sandbox` (`run-snapshot.ps1` does the same).
  Shipped (release-trust) sandbox binaries accept only a validly signed client,
  so the signed-pack acceptance (`build.ts --test-acceptance`) passes
  `-ReleaseTrustPayload`: the unsigned consumer must be rejected with
  `TRUST_E_NOSIGNATURE` (`0x800b0100`). CI then signs
  `bin\Release\<arch>\v8jsi_smoke.exe` and runs
  `run-smoke.ps1 -UseV8Sandbox -RunSigned`, which requires a valid signature and
  a successful session.
- `run-snapshot.ps1` — **scenario 2 (startup snapshot):** builds a snapshot with
  the packaged `mkv8snapshot.exe` and consumes it via the public
  `V8RuntimeArgs::startupSnapshotBlob`, asserting the baked-in global is restored
  with no script evaluated. Also feeds a **cross-engine** blob and asserts the
  engine rejects it and falls back (no crash). Runs for both engines.

## Adding a scenario

Add a `run-<name>.ps1` that dot-sources `test-helpers.ps1` (reuse `Restore-V8Package`
+ `Build-Consumer`), add the matching mode to `main.cpp` if it needs new consumer
behavior, then call it from a `run<Name>()` in `scripts/build.ts` (mirror
`runSnapshotE2E`) under `--test-e2e`.

> Authoring note: keep the `.ps1` files **ASCII-only**. `build.ts` runs them with
> Windows PowerShell 5.1, which reads BOM-less files as ANSI — a stray non-ASCII
> character (e.g. an em-dash) inside a string breaks parsing. Also pipe any
> external command whose output you don't want in a function's return value to
> `| Out-Host` (PowerShell folds uncaptured stdout into the return).

Build outputs (`bin/`, `obj/`, `packages/`, `snap/`) are git-ignored.
