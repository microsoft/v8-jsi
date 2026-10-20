# Sandbox engine payload (`v8host.dll`)

`v8host.dll` is the engine payload driven by the product `sbox.exe` container.
`sbox.exe` is generic: the app knowledge lives here, in a plugin resolved by
`--plugin v8host.dll`. The container owns `main()` and drives the plugin-ABI
lifecycle — `configure` (broker) → `warmup` pre-lockdown → `lower_token` → `run`
post-lockdown → `shutdown`. The plugin side is the single `sbox_plugin_main`
entry (see [`sbox.h`](../sbox/sbox.h), `SBOX_ABI_VERSION` 1), which returns
the `sbox_plugin_api` vtable of those functions. The DLL also exports the
`v8host_*` client API for hosts that drive `sbox.exe` (see
[`v8host_client.h`](../../../src/public/v8host_client.h), `V8HOST_ABI_VERSION`
3): `v8host_initialize`, the session shortcut and run functions, the
broker/worker/runtime topology (`v8host_broker_*`, `v8host_worker_*`,
`v8host_runtime_*`), and their builder setters (62 exports in all). It links the JSI C++ API (`jsi_cpp`) but **never
the sandbox core** — it reaches the message channel only through the
`sbox_worker_api` function pointers the container hands it.

## Status: real V8/JSI engine

The current `v8host.dll` is built from
[`v8host_engine.cc`](./v8host_engine.cc) — the **real V8/JSI engine**, split at
the `LowerToken` boundary and exposed as the plugin vtable:

- **`configure` (broker):** describes the sandbox the worker will run in — arms
  ACG and sets integrity for the Untrusted (jitless) tier, leaves ACG off for
  Trusted (JIT), declares the engine DLL for the broker to Authenticode-verify,
  and encodes the run profile (engine DLL, jitless flag, optional snapshot path)
  into the opaque `plugin_data` the container carries to the worker. The tier and
  engine are overridable for testing via `SBOX_TIER` / `V8HOST_ENGINE_DLL` /
  `V8HOST_SNAPSHOT` in the broker's environment in developer builds only.
  Release-trust builds ignore these overrides and `V8HOST_GUEST_JS`, do not echo
  worker output, and require held payload paths to remain below the install root.
- **`warmup` (pre-lockdown):** decodes `plugin_data` (a V1 session profile or a
  V2 worker profile), loads the engine DLL by full path from the app dir
  (`v8jsisb.dll` jitless for Untrusted, `v8jsi.dll` for Trusted), sets
  `--jitless` when the profile says so, loads and prechecks the startup
  snapshots (a snapshot without the compatibility export fails closed), reads
  the guest JS, and sets V8 up with a throwaway JSI runtime on the main thread
  (`makeJsiAbiRuntime` over `v8_jsi_abi_runtime_create`). A V1 session worker
  also creates its one runtime (slot 0) on its own thread here.
  All codegen (engine load, snapshot deserialize, V8 setup) happens
  **here**, because ACG forbids it after `lower_token`.
- **`run` (post-lockdown):** proves ACG is in force with a blocked executable
  allocation, then the main thread is the only reader and writer of the
  channel. It routes inbound frames by the run envelope's runtime slot, creates
  a runtime for each `CREATE_RUNTIME` (V2 workers create runtimes only on
  request, up to `max_runtimes`), posts each slot's outbound frames round-robin,
  and returns relay credit (window 8). Each runtime has its own thread (1 MiB
  stack reserve) and task queue: it evaluates the guest (interpreted under
  jitless, so safe post-ACG) and runs the WebView2-style message loop
  (`host.postMessage` / `host.postMessageBinary` → the channel; inbound frames →
  `host.onmessage`). `host.postMessage` blocks at 8 queued guest frames. A
  frame for a slot past `max_runtimes` or for a slot that never held a
  runtime, or a `CREATE_RUNTIME` for an occupied slot, ends the worker; late
  frames for a slot whose runtime has closed are dropped. `RUNTIME_CLOSED` is sent after the slot's thread has
  joined and its outbound frames have drained.
- **`shutdown`:** joins every runtime thread within one shared 1 s wait. If a
  runtime is stuck, the worker ends itself with exit code 18; otherwise it
  disposes the engine's V8 platform (`v8_node_api_dispose_platform`, when the
  engine exports it) and releases the engine handle.

## Engine host behavior

- With no nonempty `V8HOST_GUEST_JS`, the host installs a small built-in demo
  guest (`host.onmessage = m => host.postMessage('js echo: ' + m)`, with a binary
  branch that tags byte 0 of an in-cage `ArrayBuffer`). It posts nothing
  unprompted, so each broker request maps to exactly one `js echo:` reply.
- With `V8HOST_GUEST_JS`, the host reads that file before lockdown and runs it as
  the custom guest. An empty file is valid. A requested file that cannot be read
  is an input error, not permission to fall back to the demo.
- The host stays a neutral JS host: it installs the `host` object and evaluates
  whatever guest it is handed. A clean worker exit (`0`) means the ACG check
  passed, guest evaluation completed, and the message loop ended normally when
  the broker closed it. It does **not** certify an application-specific result
  sent by the guest — the broker and guest define that protocol.
- Failed message sends throw a JavaScript error (surfaced as a run failure).

Mandatory lockdown and verification of requested ACG are retained. Neither
custom-guest selection nor result reporting disables any security check; all
engine setup (the only place codegen is allowed) completes before
`lower_token`.

## Smoke test

Build the `generic` group and drive the single-image container, which spawns its
own `--worker`, loads `v8host.dll`, and runs the warmup → `lower_token` → run
round trip from the repository root:

```powershell
node .\scripts\sbox-build.ts --target-cpu x64 --target generic
& .\deps\chromium\out\sandbox-x64\sbox.exe --broker --plugin v8host.dll
```

A passing run shows the real-engine signals: `engine = v8jsisb.dll`, `v8jsi
runtime created (warmup, pre-lockdown)`, `LowerToken survived`,
`exec-alloc(post)=BLOCKED (ACG in force) (err=1655)`, the guest `JS evaluated =
OK`, the broker's `worker reply = "js echo: ping from broker"` (real guest JS
executed under lockdown), and `worker_exit=0 ... -> PASS`.

## Public topology tests

`v8host_broker_tests.exe --suite=topology,security,boundary,failure` exercises
the DLL's public broker, worker, and runtime API in owned child processes from
the build output directory. The cases query ACG after worker security readiness
and before the first runtime open, run isolated JS globals behind explicit
message barriers, check live worker PIDs, run an explicit LPAC worker, and
verify ordered worker/broker loss and replacement without replay.

The boundary and failure compositions cover shared worker admission, runtime
and run caps, closing-slot replacement, lifetime admission, shared relay and
mailbox bounds, local/global callback backpressure, refused creates, queued
parent close, and runaway cancellation. Parallelism uses started callbacks and
guest messages before releasing the barrier, not wall-time ratios. Intentional
worker termination and runaway shutdown retain their raw exit diagnostics;
only the exact expected diagnostic is allowed, once, in each owning case.
