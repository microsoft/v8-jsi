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
harness is **x64-only** and needs `nuget.exe` + Visual Studio (MSBuild via vswhere)
on the machine. The sandbox-engine (`v8jsisb.dll`) and cross-engine legs run only
when the pack actually carries the sandbox binaries; otherwise they auto-skip.

## Layout

- `consumer.vcxproj` / `main.cpp` — one consumer exe (`v8jsi_smoke.exe`) that
  serves every scenario. It links `v8jsi.dll.lib` and consumes the public JSI C++
  API (`makeV8Runtime`, `jsi::Runtime`). Built with `/p:UseV8Sandbox=true` it
  delay-load-redirects to `v8jsisb.dll` and links the `sbox.dll` surface. Scenario
  is chosen by argv (see `main.cpp`).
- `test-helpers.ps1` — shared helpers: restore the per-RID `.nupkg`, locate MSBuild,
  build the consumer. Dot-sourced by every driver.
- `run-smoke.ps1` — **scenario 1 (smoke):** evaluates `1 + 2` and four cross-CRT
  exercises (utf8 round-trip, global set/get, HostFunction, JSError) through the
  JSI ABI; with `-UseV8Sandbox` also proves the `v8jsisb.dll` + `sbox.dll`
  consumer surface.
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
