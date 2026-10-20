# Sandboxed JavaScript through v8host.dll

## Status and build

The Windows C client API in [v8host_client.h](../src/public/v8host_client.h)
is **experimental, not ABI-stable** (`V8HOST_ABI_VERSION` 1). Signatures,
fields, and values may change between builds; do not assume binary compatibility.
The payload contains `sbox.exe`, the generic sandbox container; `v8host.dll`,
which provides the client, broker plugin, and worker engine plugin; and engine
DLLs: `v8jsisb.dll` for the untrusted tier (jitless with Arbitrary Code Guard,
ACG), and `v8jsi.dll` for the trusted tier (JIT, without ACG).
From the repository root, build the sandbox container and plugin with
`node scripts\sbox-build.ts --target-cpu x64 --target generic`. That default is
the **developer** trust policy: unsigned payloads and non-architecture layouts
are accepted so the suites can run on unsigned outputs. The shipped binaries
are built with `--release-trust`, which compiles the developer policy out:
every payload file must carry a valid signature, and the layout must be the
`<install-root>\<native-arch>\` one below. The broker also verifies the client
process image, so an application that uses the shipped binaries must itself be
Authenticode-signed; an unsigned application fails to connect
(`v8host_status_connect_failed`). The hygiene gate (`--check`) fails a
release-trust build that still contains developer hooks. Release payload opens
reject final-component reparse points, and the resolved container, plugin and
engine paths must be strictly below the container-derived install root.
Environment overrides (`V8HOST_GUEST_JS`, `SBOX_TIER`, `V8HOST_ENGINE_DLL`,
`V8HOST_SNAPSHOT`), worker-output echo and `--pipe-bench` are developer-only.
The engine DLLs are built separately; see the [project guide](../AGENTS.md).
The [plugin overview](../deps/chromium/v8host/README.md) describes the internal
lifecycle. Use the untrusted tier for untrusted JavaScript.

## Architectures and installation

The deployment architecture contract is:

| Client process | Native payload | Notes |
| --- | --- | --- |
| x64 on x64 Windows | x64 | Same-architecture client DLL. |
| ARM64 on ARM64 Windows | ARM64 | Same-architecture client DLL. |
| x86 on x64 Windows | x64 | The client loads an x86 client DLL. |
| ARM64EC on ARM64 Windows | ARM64 | The process loads the x64-built client DLL; no ARM64EC-compiled client DLL is provided. |

Unsupported combinations include 32-bit Windows and any x86 broker or worker.
An x64 client on ARM64 Windows resolves by the same rule as ARM64EC (the sibling
`arm64` payload), but it is not qualified. The NuGet `UseV8Sandbox` option
supports x64 and ARM64 consumers only; x86 and ARM64EC consumers need the
cross-architecture layout, which the package does not stage yet. There is no weaker-profile or
other-architecture fallback: a qualification failure fails closed, never falls
back.

Package each native payload in this layout:

```text
<install-root>\
  x64\                         # or arm64\
    sbox.exe
    v8host.dll
    v8jsisb.dll
    v8jsi.dll                  # if the trusted tier is shipped
```

The native files must be one signed, same-architecture version set. Keep
different versions in separate install roots rather than mixing files.
A cross-architecture client DLL belongs in its own architecture directory,
beside the native payload directory.

NuGet stages the payload and import library under `build\native\<rid>` before
packing. The per-RID package puts the binaries in `runtimes\<rid>\native` and
keeps import libraries in `build\native\<rid>`; the legacy package keeps both
under `build\native\<rid>`. With `UseV8Sandbox=True`, the targets copy
`sbox.exe`, `v8host.dll`, and `v8jsisb.dll` beside the application, without
relocating its output. Set `<OutDir>...\x64\</OutDir>` (or `arm64`) and keep
`TargetDir` equal to `OutDir`. The directory name is case-insensitive.
Otherwise the build fails with `V8HOST001`: "UseV8Sandbox requires the
application and payload together in an output directory named 'x64'"
(or 'arm64'), followed by the property to set and the same-version-set rule.
This puts the application beneath the broker's install root for shared mode.
The same output layout is required for local developer-trust packages.

The required selection rule uses the native OS architecture, not the client's
pointer size. A same-architecture client uses the directory of its own
`v8host.dll`; a cross-architecture client uses the sibling
`<install-root>\<native-arch>` directory, with no search for alternatives.
These pairs describe the deployment contract, not qualification results from
a build on a different architecture.

The broker derives the install root from its own canonical image path, never
from a client-supplied root. Production layout requires the native architecture
directory. Developer layouts are an explicit non-production build option, not
a production fallback.

## Choose the isolation boundary

- **Broker:** `v8host_broker_open` opens a connection. Shared mode uses one
  broker process per user, exact native payload set, and canonical install root.
  The V2 rendezvous also includes the install root in dedicated mode. Dedicated mode gives
  the opening client connection its own broker; dedicated is the default.
  Sharing a broker does not share workers across connections.
- **Worker:** `v8host_worker_open` creates one sandboxed OS process with a
  fixed security and engine profile. A worker belongs to one connection.
  Its initial token defaults to `restricted_same_access`, so the process can
  load its startup DLLs; the token applied at lockdown still defaults to
  `lockdown`. A lockdown-level initial token cannot load startup DLLs.
- **Runtime:** `v8host_runtime_open` creates one V8 isolate on its own thread
  in that worker, after security readiness. Several runtimes can occupy one
  worker; their separation is by isolates, not by OS processes.
  Use one runtime per worker when process isolation is required.
- **Run:** `v8host_runtime_start_run` starts work in a runtime. Runs share that
  runtime's JavaScript state.
- **Session shortcut:** `v8host_session_open` opens its own connection and
  owns one implicit private worker and one runtime. The first run binds the
  worker profile. A session is not a child of an explicit broker handle.

A runtime failure closes that runtime and fails its live runs with
`v8host_status_runtime_failed`. A worker exit affects every runtime and live
run in that process (`v8host_status_worker_exited`), not other workers.
A broker loss affects every connection it serves and their live work.
Putting runtimes in one worker therefore shares a process-failure blast radius.
In particular, closing a runtime whose JavaScript never yields can close the
whole worker after five seconds; its other runtimes then report worker exit.

Explicit workers bind tier, policy, engine, and snapshot list at creation.
The runtime selects an index from that snapshot list. Worker lifetime flags
can close a worker when empty or cap lifetime runtime admission; see the header.

A host that runs as a service (a non-interactive window station) can start
AppContainer and LPAC workers, because the worker never initializes user32.
The client DLL loads user32 on first use, when `v8host_initialize` creates its
message window.

## Authentication and profile validation

Before any protocol exchange, client and broker authenticate each other.
Checks bind the connection to the live peer process, same-user identity,
held image files, and the payload signature policy. The client verifies the
expected broker image and plugin; the broker verifies the client image.
Files remain held during the relevant launch or connection lifetime so path
replacement cannot substitute a different image after verification.
Shared mode additionally requires the client's canonical image path to be
strictly beneath the broker's install root. Dedicated mode relaxes only that
co-location requirement, not peer or signature checks. Authentication failure
does not switch modes or try a weaker profile.

For a session, the first accepted run binds the worker's tier, engine DLL,
and startup snapshot. Later conflicting requests fail with
`v8host_status_profile_already_bound`; they do not replace the worker.
For an explicit runtime, setting *any* run-level tier, engine, or snapshot
override is rejected with that status, even if the value matches its worker.

Configuration uses synchronous builder callbacks. Builders exist only during
the callback; never retain them. Return a non-success status to reject the
operation. The first failed setter poisons the builder, and the enclosing
open or start call fails with that status.

- Invalid enums, text, paths, pointer/length pairs, or incompatible policy
  combinations return `v8host_status_invalid_arg`.
- An engine override must be a bare filename in the worker payload directory,
  not an arbitrary path; the broker signature-verifies it.
  A snapshot path is nonempty and payload-relative, read before lockdown.
- A trusted session run cannot override an envelope that prohibits dynamic
  code. For an untrusted session, explicitly set
  `v8host_config_set_prohibit_dynamic_code` to true; its builder default is
  false. Explicit workers derive ACG from their tier.
- AppContainer requires a nonempty profile name; LPAC requires AppContainer.
  A capability is permitted only with AppContainer (at most 64 capabilities).
- Exceeding documented caps returns `v8host_status_quota_exceeded`.
  Undersized callback tables return `v8host_status_invalid_struct_size`;
  an unsupported initialization version returns `v8host_status_version_mismatch`.

## Threads, callbacks, and ownership

Call `v8host_initialize(V8HOST_ABI_VERSION)` first on the callback thread.
That first successful call captures the process-wide callback thread, which
must have a Windows message queue. Keep its message pump running; do not block
it waiting for an asynchronous callback. Later initialization does not move
callbacks to another thread.

Lifecycle, run, relay, and disconnect callbacks execute on that thread, never
an internal I/O thread or under an internal lock. Configuration callbacks are
different: they run synchronously on the thread calling open or start.

Inside notification callbacks, reentrant start, post, cancel, callback-table
replacement, and close are supported. Copy any incoming relay buffer before
returning if it must be retained; it is borrowed only for that callback.
Do not retain a builder or block the pump waiting for another notification.
Keep callback context alive while queued notifications may still reference it.
Session callback replacement affects future events; already-queued events
retain their previous table. The broker callback table is fixed at open.

Undrained callback queues are bounded. A runtime can close, or a broker
connection can disconnect, with `v8host_status_callback_backpressure`.
A synchronous `v8host_status_ok` means accepted, not completed: observe
callbacks for asynchronous outcomes. Every run event other than
`v8host_run_event_started` is terminal.

Close calls are nonblocking, safe inside callbacks, and suppress queued
callbacks for the closed object and its children. Close a session to invalidate
its run handles; there is no per-run release. Explicit runtime close invalidates
its runs. After broker close, worker and runtime handles still require explicit
close; after worker close, runtime handles still require explicit close.
Operations on closed parents' remaining handles return
`v8host_status_invalid_state`.

## Minimal C sketch

This sketch assumes the declared entry points have been bound from the client
DLL (the header provides matching `_fn` typedefs for `GetProcAddress`).
Call `begin` on the callback thread before returning to the application's
Windows message pump. It posts one string after the run starts and closes on
the first reply or terminal event. Both configuration callbacks select shared
mode. The explicit broker connection is shown separately to illustrate
`v8host_broker_open`: the session does **not** attach to it. A session-only
client normally omits that broker handle.

```c
#include "v8host_client.h"
static v8host_broker broker;
static v8host_session session;
static void close_all(void) {
  if (session) { v8host_session_close(session); session = NULL; }
  if (broker) { v8host_broker_close(broker); broker = NULL; }
}
static v8host_status V8HOST_CDECL broker_config(void *p, v8host_broker_config c) {
  (void)p;
  return v8host_broker_config_set_mode(c, v8host_broker_mode_shared);
}
static v8host_status V8HOST_CDECL session_config(void *p, v8host_config c) {
  v8host_status s; (void)p;
  s = v8host_config_set_broker_mode(c, v8host_broker_mode_shared);
  return s == v8host_status_ok ?
      v8host_config_set_prohibit_dynamic_code(c, true) : s;
}
static v8host_status V8HOST_CDECL run_config(void *p, v8host_run_config c) {
  static const char js[] = "host.onmessage = m => host.postMessage(m);";
  (void)p;
  return v8host_run_config_set_payload(c, js, sizeof(js) - 1);
}
static void V8HOST_CDECL on_run(void *p, v8host_run r,
                              v8host_run_event e, v8host_status s) {
  (void)p; (void)s;
  if (e != v8host_run_event_started) { close_all(); return; }
  if (v8host_run_post_message(r, v8host_message_kind_string, "ping", 4)
      != v8host_status_ok) close_all();
}
static void V8HOST_CDECL on_message(void *p, v8host_run r,
    v8host_message_kind k, const void *data, size_t size) {
  (void)p; (void)r; (void)k; (void)data; (void)size;
  close_all();
}
static void V8HOST_CDECL on_session(void *p, v8host_session h,
    v8host_session_state state, v8host_status status) {
  v8host_run run;
  (void)p;
  if (status != v8host_status_ok) { close_all(); return; }
  if (state == v8host_session_state_ready &&
      v8host_session_start_run(h, run_config, NULL, &run)
      != v8host_status_ok) close_all();
}
static v8host_status begin(void) {
  v8host_callbacks cb = V8HOST_CALLBACKS_INIT;
  v8host_status s = v8host_initialize(V8HOST_ABI_VERSION);
  if (s != v8host_status_ok) return s;
  s = v8host_broker_open(broker_config, NULL, &broker);
  if (s != v8host_status_ok) return s;
  s = v8host_session_open(session_config, NULL, &session);
  if (s == v8host_status_ok) {
    cb.on_session_state = on_session;
    cb.on_run_event = on_run;
    cb.on_relay_message = on_message;
    s = v8host_session_set_callbacks(session, &cb);
  }
  if (s != v8host_status_ok) close_all();
  return s;
}
```

For the explicit topology instead, open a worker under the broker, open a
runtime under the worker, and start with `v8host_runtime_start_run`. Install
`v8host_broker_callbacks` through the broker builder to observe that topology.

## Recovery and troubleshooting

After broker loss or an ambiguous outcome, **nothing is replayed**: no session
creation, run, or relay message. Live runs fail with the disconnect status;
transport loss is `v8host_status_broker_lost`. Explicit topology notifications
terminate runs, then runtimes, then workers, before the broker notification.
Close old handles and explicitly open new ones for new work. Do not infer that
lost work had no effects, or retry it automatically. Cancellation acceptance
likewise does not prove that JavaScript produced no effects.

- **Payload not found:** `v8host_status_connect_failed` can mean native
  payload resolution or broker launch failed. Check that the complete version
  set is present in the required native directory.
- **Trust failure:** connection establishment also reports
  `v8host_status_connect_failed`; there is no separate public trust status.
  Check signatures (the payload and, with the shipped binaries, the
  application's own executable) and shared-mode client co-location. The broker
  logs the failing check as `client authentication failed category=<code>`;
  `0x800b0100` means the application is unsigned. Do not disable
  verification or change broker mode as a recovery fallback.
- **Wrong architecture or layout:** `v8host_status_connect_failed` can also
  mean rejected payload identity or install-root layout. Check native machine
  types and architecture directory names; moving files into a different
  directory does not change their architecture.
- **Connection ended:** distinguish `v8host_status_protocol_error`,
  `v8host_status_broker_lost`, and `v8host_status_callback_backpressure` in
  callbacks. Repair the cause and open new handles; never replay prior runs.
