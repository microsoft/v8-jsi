# tools/footprint — sandbox-vs-WebView2 footprint harness

Measurement-only (nothing here ships). Quantifies the headline product driver:
per-instance **RAM** + **startup** for the AI-JS sandbox vs a WebView2 control.
**No product or build code is modified** — these are standalone tools that drive
the already-built sandbox binaries.

## Pieces

- **`footprint_broker.cc` → `footprint_broker.exe`** — a `test_app.exe` stand-in
  (one broker = one Office app) that spawns **`--count N`** (default 1) **unmodified**
  `v8host.exe` targets via the **unmodified** `sbox.dll` — the real one-app→N-targets
  topology — runs the same demo handshake on each, then **holds** all channels open so
  the sandboxed targets sit at post-lockdown steady state for sampling. The sampled
  process set is exactly **1 broker + N v8hosts**.
  Args: `--count N`. Env: `SBOX_FP_COUNT` (count fallback), `SBOX_TIER=trusted`
  (JIT, ACG off), `SBOX_FP_HOLD_MS`, `SBOX_FP_RELEASE_EVENT`.
  Built into `deps/chromium/out/sandbox-x64/` (beside `v8host.exe` + `sbox.dll`).
- **`wv2host.cc` → `wv2host.exe`** — minimal real WebView2 host (SDK + Evergreen
  runtime). `--controls N --udf <dir> --hold-ms M`. One host × N controls = shared
  browser process; N hosts × 1 control (distinct UDF) = isolated.
- **`memprobe.cc` → `memprobe.exe`** — per-PID sampler. `<pid>...` |
  `--name <img.exe>` | `--tree <root_pid>`. Emits JSON: WS total/private/shareable,
  private commit, reserved vs committed, largest reserved region (the V8 cage).
  Use `--tree` for WebView2 (the machine has many unrelated `msedgewebview2.exe`).
- **`footprint.ts`** — orchestrator (Node 24, `node footprint.ts [--quick]`). Runs
  the matrix, times startup from stdout markers, sums (excluding `conhost.exe`),
  writes `footprint-results.json` + `footprint-table.md`.

## Build

```powershell
Set-Location tools\footprint
$vc  = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
$dir = $PWD.Path
$repo = (Resolve-Path '..\..').Path
$sandboxOut = Join-Path $repo 'deps\chromium\out\sandbox-x64'
$sdk = "$env:USERPROFILE\.nuget\packages\microsoft.web.webview2\1.0.3405.78\build\native"
$inc = Join-Path $repo 'deps\chromium\sandbox_dll'
cmd /c "`"$vc`" >nul 2>&1 && cl /nologo /O2 /EHsc /std:c++17 `"$dir\memprobe.cc`" /Fe:`"$dir\memprobe.exe`""
cmd /c "`"$vc`" >nul 2>&1 && cl /nologo /O2 /EHsc /std:c++17 /I`"$inc`" `"$dir\footprint_broker.cc`" /Fe:`"$sandboxOut\footprint_broker.exe`" /link `"$sandboxOut\sbox.dll.lib`" wintrust.lib"
cmd /c "`"$vc`" >nul 2>&1 && cl /nologo /O2 /EHsc /std:c++17 /I`"$sdk\include`" `"$dir\wv2host.cc`" /Fe:`"$dir\wv2host.exe`" /link `"$sdk\x64\WebView2LoaderStatic.lib`""
```

Prereqs: the `generic` sandbox build is staged in `out/sandbox-x64` (`v8host.exe`, `sbox.dll`,
`v8jsi.dll`); WebView2 Evergreen runtime installed; WebView2 SDK in the NuGet cache.

## Run

```powershell
Set-Location tools\footprint
node footprint.ts            # full matrix: N=1,2,4,8 x3
node footprint.ts --quick    # N=1,2 x1 (CI smoke / fast iteration)
```
