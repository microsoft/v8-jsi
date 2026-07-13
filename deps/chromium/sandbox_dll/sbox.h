// sbox.h — public C ABI of the generic sandbox DLL (sbox.dll).
//
// This DLL packages the Chromium sandbox engine behind a small, ABI-safe C API.
// It is built with the Chromium toolchain, signed, and is **V8-agnostic** — it
// knows nothing about v8jsi, JavaScript, jitless, or any guest engine. Two
// different binaries load it:
//   * a broker host (e.g. an application or test harness) calls sbox_broker_*
//     to spawn and lock down a target process, and
//   * an app-defined target EXE calls sbox_target_* to run the sandboxed role
//     and host whatever it wants (the target EXE decides the "prime use").
//
// The sandbox's primary enforcement is the restricted token + job + integrity
// (kernel-enforced). File rules below are a *selective re-allow* channel: the
// locked-down target is denied by default; rules name the few paths the broker
// will open on its behalf and hand back.
#ifndef SANDBOX_DLL_SBOX_H_
#define SANDBOX_DLL_SBOX_H_

#include <cstddef>
#include <cstdint>

#ifdef SBOX_DLL_IMPL
#define SBOX_API extern "C" __declspec(dllexport)
#else
#define SBOX_API extern "C" __declspec(dllimport)
#endif

// --- abstract, ABI-safe levels (mapped to sandbox enums inside the DLL) ---
enum SboxTokenLevel {
  SBOX_TOKEN_RESTRICTED_SAME_ACCESS = 0,  // initial (warmup) token
  SBOX_TOKEN_LOCKDOWN = 1,                // post-LowerToken restricted token
};
enum SboxIntegrityLevel {
  SBOX_INTEGRITY_LOW = 0,
  SBOX_INTEGRITY_UNTRUSTED = 1,
};

// A single selectively-allowed (broker-proxied) file rule.
typedef struct SboxFileRule {
  const wchar_t* pattern;  // full path, or a wildcard pattern ('*')
  int32_t readonly;        // 1 = read-only access; 0 = any access
} SboxFileRule;

// The policy the broker host describes; the DLL compiles it into sandbox rules.
// Engine-generic: ACG (prohibit_dynamic_code) is an OS mitigation the DLL
// applies; whether the guest can survive it (e.g. running V8 jitless) is the
// target EXE's concern, not the DLL's.
typedef struct SboxPolicy {
  int32_t initial_token;          // SboxTokenLevel
  int32_t lockdown_token;         // SboxTokenLevel
  int32_t integrity;              // SboxIntegrityLevel (initial)
  int32_t delayed_integrity;      // SboxIntegrityLevel (applied at LowerToken)
  int32_t prohibit_dynamic_code;  // 1 = arm ACG (MITIGATION_DYNAMIC_CODE_DISABLE)
  const SboxFileRule* file_rules;
  size_t file_rule_count;
} SboxPolicy;

// Bootstrap struct: the target EXE exports an instance named `g_sbox_bootstrap`;
// the broker fills it (by parsing the target EXE's export table) while the
// target is still suspended — the EXE image is mapped at creation, unlike the
// target's not-yet-loaded sbox.dll. The fields carry only values (a handle, two
// sizes, the delayed integrity + mitigations), never DLL addresses, so the
// scheme is independent of where sbox.dll loads in either process.
typedef struct SboxBootstrap {
  uint64_t magic;           // SBOX_BOOTSTRAP_MAGIC once the broker has written it
  uint64_t section_handle;  // child-side shared-section HANDLE value
  uint32_t ipc_size;        // shared IPC region size
  uint32_t policy_size;     // policy blob size (in the section)
  int32_t  integrity;       // delayed integrity (raw sandbox value)
  uint64_t mitigations;     // delayed mitigations (raw sandbox value)
  uint8_t  handle_closer[8];  // opaque: the sandbox HandleCloserConfig (relayed)
  uint64_t msg_section;     // child-side message-channel section handle
  uint64_t msg_evt_t2b;     // child-side event the target SIGNALS (target->broker)
  uint64_t msg_evt_b2t;     // child-side event the target WAITS on (broker->target)
  uint64_t msg_evt_close;   // child-side event the broker SIGNALS to close the channel
} SboxBootstrap;

#define SBOX_BOOTSTRAP_MAGIC 0x5342584F4F540003ull  // 'SBXOOT' + v3

// --- broker role (used by the host, e.g. test_app.exe) ---
// Spawn `target_exe` as a locked-down sandbox target per `policy`, wire the
// broker<->target IPC channel, relay the bootstrap, run it to completion, and
// return the target's exit code (0 = the target reported success).
SBOX_API int sbox_broker_run(const wchar_t* target_exe, const SboxPolicy* policy);

// Opaque handles (defined inside sbox.dll).
typedef struct SboxTarget SboxTarget;

// --- WebView2-style host<->target message channel (fire-and-forget, duplex) ---
// A dedicated duplex shared-memory channel, independent of the sandbox IPC. The
// channel carries opaque byte frames tagged with a kind so a host can post
// strings or binary; it grants no capability — the host MUST treat target->host
// messages as untrusted input.
enum SboxMsgKind { SBOX_MSG_STRING = 0, SBOX_MSG_BINARY = 1 };

// Received-message callback. For the broker it is invoked on an internal reader
// thread; the target drains explicitly (see sbox_target_drain_messages). `data`
// is owned by the channel and valid only for the duration of the call.
typedef void (*SboxMessageCb)(void* ctx, int kind, const void* data, size_t len);

// Broker session API (non-blocking — the host can message the target while it
// runs). sbox_broker_run is just sbox_broker_spawn(no handler) + sbox_broker_wait.
//
// One broker process may spawn MANY targets: the first sbox_broker_spawn (or
// sbox_broker_run) initializes the process-wide broker once, and every later
// spawn reuses it. Each returned SboxSession is fully independent (its own
// process/thread/log/IPC section/message channel/reader thread), so N sessions
// coexist and can be waited on / closed in any order.
//
// THREAD-SAFETY: sbox_broker_spawn is thread-safe -- it may be called
// concurrently from multiple threads. The underlying Chromium spawn path
// requires every spawn to run on a single thread, so the DLL marshals the actual
// spawn onto one internal launcher thread and blocks the caller until it
// completes. Spawns are thus serialized internally (briefly), but the spawned
// targets run fully in parallel and callers do NOT need to coordinate. The
// per-session calls below (post_message / close / wait) are likewise safe to use
// concurrently across distinct live sessions.
typedef struct SboxSession SboxSession;
SBOX_API SboxSession* sbox_broker_spawn(const wchar_t* target_exe,
                                        const SboxPolicy* policy,
                                        SboxMessageCb on_message, void* ctx);
SBOX_API int sbox_broker_post_message(SboxSession* session, int kind,
                                      const void* data, size_t len);
// Signal the target to close the channel (host-initiated, out-of-band). A target
// running an event loop on the channel can wait on sbox_target_close_event and
// exit cleanly when this fires. Idempotent (the close event is manual-reset).
SBOX_API int sbox_broker_close(SboxSession* session);
SBOX_API int sbox_broker_wait(SboxSession* session);  // -> target exit code

// Target message API. Post is fire-and-forget; inbound messages are delivered by
// the target draining its inbound ring whenever its inbound event is signaled
// (so the host controls which thread runs the handler — e.g. the JS thread).
SBOX_API int sbox_target_post_message(SboxTarget* target, int kind,
                                      const void* data, size_t len);
SBOX_API void* sbox_target_inbound_event(SboxTarget* target);  // HANDLE to wait on
SBOX_API int sbox_target_drain_messages(SboxTarget* target, SboxMessageCb cb,
                                        void* ctx);
// HANDLE the broker SIGNALS (via sbox_broker_close) to ask the target to shut
// down its channel loop. Manual-reset, so once closed it stays signaled. NULL if
// the channel was not mapped.
SBOX_API void* sbox_target_close_event(SboxTarget* target);

// --- target role (used by the app-defined target EXE, e.g. v8host.exe) ---

// Adopt the broker-relayed bootstrap, init the sandbox target services, and
// stand up the IPC client. Returns NULL on failure. Call BEFORE doing any guest
// warmup; the returned handle is used for the calls below.
SBOX_API SboxTarget* sbox_target_begin(const SboxBootstrap* boot);

// Prove the broker<->target IPC channel (a cross-call ping). Returns 1 on OK.
SBOX_API int sbox_target_test_ipc(SboxTarget* target);

// Install the ntdll file interceptions (so denied, policy-allowed opens auto-
// route to the broker) and drop to the restricted token + delayed integrity +
// mitigations. Call AFTER all guest warmup (ACG forbids patching/codegen after).
SBOX_API int sbox_target_lower_token(SboxTarget* target);

// Release the target handle (does not terminate the process).
SBOX_API void sbox_target_end(SboxTarget* target);

#endif  // SANDBOX_DLL_SBOX_H_
