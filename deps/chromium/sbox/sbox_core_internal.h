// sbox_core_internal.h — the INTERNAL C ABI of the generic sandbox core.
//
// This is NOT the plugin ABI. Plugin authors include sbox.h; this header is
// compiled only INTO the single-image sbox.exe container (both personas link
// sbox_broker.cc statically), so the sbox_* symbols resolve at link time and stay
// out of the EXE's export table. It is **V8-agnostic** — it knows nothing about
// v8jsi, JavaScript, jitless, or any guest engine. One image uses it in two
// personas:
//   * the broker persona calls sbox_broker_* to spawn and lock down a worker,
//     and
//   * the worker persona calls sbox_target_* to run the sandboxed role and host
//     whatever the loaded plugin wants.
//
// Requires Windows 10 version 1809 (RS5, build 17763), Windows Server 2019,
// or later, for every mode. Broker and target must have the same architecture.
//
// Ordinary mode uses restricted tokens, a job, and integrity levels. Profile
// AppContainer/LPAC mode uses the Windows-created AppContainer token, its
// package/capabilities, a job, and integrity levels; it does not combine that
// identity with the ordinary mode's token-level or restricted-default-DACL
// settings. File rules are an additional broker-mediated access channel.
#ifndef SANDBOX_DLL_SBOX_CORE_INTERNAL_H_
#define SANDBOX_DLL_SBOX_CORE_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include "sbox.h"

#if defined(SBOX_STATIC)
// The broker/target core is linked directly into an executable (the product
// sbox.exe and the trust-transition test both compile sbox_broker.cc), so the
// sbox_* ABI is resolved at link time, not across a DLL boundary. Plain
// extern "C" keeps the symbols out of the export table (the EXE must export
// nothing). Checked BEFORE SBOX_DLL_IMPL because sbox_broker.cc self-defines that.
#define SBOX_CORE_API extern "C"
#elif defined(SBOX_DLL_IMPL)
#define SBOX_CORE_API extern "C" __declspec(dllexport)
#else
#define SBOX_CORE_API extern "C" __declspec(dllimport)
#endif

// --- abstract, ABI-safe levels (mapped to sandbox enums inside the DLL) ---
// Token levels, most- to least-restrictive (mirrors Chromium's TokenLevel 1:1).
// Unknown -> RESTRICTED_SAME_ACCESS.
enum SboxTokenLevel {
  SBOX_TOKEN_LOCKDOWN = 0,                 // null SID only; most restrictive
  SBOX_TOKEN_LIMITED = 1,                  // restricting SIDs: Users, Everyone, RESTRICTED
  SBOX_TOKEN_INTERACTIVE = 2,              // + Owner; more deny-only exceptions than LIMITED
  SBOX_TOKEN_RESTRICTED_NON_ADMIN = 3,     // keeps user/authenticated SIDs, drops admin/other groups
  SBOX_TOKEN_RESTRICTED_SAME_ACCESS = 4,   // all SIDs (~ caller access); least restrictive
};
enum SboxIntegrityLevel {
  SBOX_INTEGRITY_LOW = 0,
  SBOX_INTEGRITY_UNTRUSTED = 1,
};

// A single selectively-allowed (broker-proxied) file rule.
// No file rules means no file-brokering hooks are installed. Windows access
// restrictions, required process/token hooks, and final lockdown still apply.
typedef struct SboxFileRule {
  const wchar_t* pattern;  // full path, or a wildcard pattern ('*')
  int32_t readonly;        // 1 = read-only access; 0 = any access
} SboxFileRule;

// The policy the broker host describes; the DLL compiles it into sandbox rules.
// Engine-generic: ACG (prohibit_dynamic_code) is an OS mitigation the DLL
// applies; whether the guest can survive it (e.g. running V8 jitless) is the
// target EXE's concern, not the DLL's.
typedef struct SboxPolicy {
  uint32_t struct_size;           // size in bytes; at least sizeof(SboxPolicy)
  int32_t initial_token;          // SboxTokenLevel; ordinary mode only
  int32_t lockdown_token;         // SboxTokenLevel; ordinary mode only
  int32_t integrity;              // SboxIntegrityLevel; AppContainer requires LOW
  int32_t delayed_integrity;      // SboxIntegrityLevel (applied at LowerToken)
  int32_t prohibit_dynamic_code;  // 1 = arm ACG (MITIGATION_DYNAMIC_CODE_DISABLE)
  const SboxFileRule* file_rules;
  size_t file_rule_count;
  int32_t use_app_container;  // 1 = create or reuse an AppContainer profile
  int32_t low_privilege_app_container;  // 1 = opt out of ALL_APP_PACKAGES
  const wchar_t* app_container_profile_name;
  const wchar_t* const* capabilities;  // capability SID strings
  size_t capability_count;
  // --- generic broker->worker passthrough: the container forwards these to the
  // worker persona but NEVER interprets them (only the loaded plugin does) ---
  // Appended to the spawned worker's command line as --plugin=<name> so a
  // renameable host and its plugin stay in sync (NULL = append nothing).
  const wchar_t* worker_plugin_name;
  // Opaque app-knowledge blob copied into the same-image worker seed; the worker
  // reads it back via sbox_target_plugin_data. NULL/0 = none. A blob larger than
  // the seed's fixed capacity fails the spawn (fail closed).
  const void* plugin_data;
  size_t plugin_data_len;
} SboxPolicy;

// A zero use_app_container selects ordinary restricted-token mode. Nonzero
// selects profile-based AppContainer; nonzero low_privilege_app_container then
// selects LPAC. LPAC without AppContainer is invalid.
//
// In profile mode, initial_token and lockdown_token are ignored, including
// values from existing callers. They do not request additional restricted-token
// enforcement. Initial integrity must be LOW; delayed_integrity may be LOW or
// UNTRUSTED and is applied as requested. Final target lockdown is still required.
// Profile name/capability fields do not apply in ordinary mode.
//
// The current complete structure is required. Smaller structures are rejected;
// larger structures may append fields, but this version reads only this prefix.

// Same-image handoff: the broker and target are the SAME statically-linked
// image, so every sandbox global sits at the same RVA in both. While the target
// is suspended the broker writes its sandbox globals (and a small channel seed)
// directly at those shared RVAs — there is no exported bootstrap struct and no
// export-table walk. The only per-process unknown is the target's ASLR base,
// read from one documented PEB field, which keeps the scheme independent of
// where the image loads in either process.

// --- broker role (used by sbox.exe) ---
// Spawn `target_exe` as a locked-down sandbox target per `policy`, wire the
// broker<->target IPC channel, relay the bootstrap, run it to completion, and
// return the target's exit code (0 = the target reported success).
SBOX_CORE_API int sbox_broker_run(const wchar_t* target_exe, const SboxPolicy* policy);

// Opaque handles (defined inside the sbox core).
typedef struct SboxTarget SboxTarget;

// --- WebView2-style host<->target message channel (fire-and-forget, duplex) ---
// A dedicated duplex shared-memory channel, independent of the sandbox IPC. The
// channel carries opaque byte frames tagged with a kind so a host can post
// strings or binary; it grants no capability — the host MUST treat target->host
// messages as untrusted input.
enum SboxMsgKind {
  SBOX_MSG_STRING = 0,
  SBOX_MSG_BINARY = 1,
  SBOX_MSG_LIFECYCLE = 2  // container-emitted readiness marker (see sbox.h)
};

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
// Creation returns nullptr on unsupported platforms or invalid policies and
// writes a broker diagnostic. No target is resumed after setup failure.
SBOX_CORE_API SboxSession* sbox_broker_spawn(const wchar_t* target_exe,
                                        const SboxPolicy* policy,
                                        SboxMessageCb on_message, void* ctx);
SBOX_CORE_API int sbox_broker_post_message(SboxSession* session, int kind,
                                      const void* data, size_t len);
// Signal the target to close the channel (host-initiated, out-of-band). A target
// running an event loop on the channel can wait on sbox_target_close_event and
// exit cleanly when this fires. Idempotent (the close event is manual-reset).
SBOX_CORE_API int sbox_broker_close(SboxSession* session);
// Returns zero on failure; the session must not have been waited on.
SBOX_CORE_API uint32_t sbox_broker_get_process_id(SboxSession* session);
SBOX_CORE_API int sbox_broker_wait(SboxSession* session);  // -> target exit code

// Target message API. Post is fire-and-forget; inbound messages are delivered by
// the target draining its inbound ring whenever its inbound event is signaled
// (so the host controls which thread runs the handler — e.g. the JS thread).
SBOX_CORE_API int sbox_target_post_message(SboxTarget* target, int kind,
                                      const void* data, size_t len);
// Container-only readiness markers posted on the worker->broker ring with kind
// SBOX_MSG_LIFECYCLE and a u32 phase payload. Emitted by the generic RunWorker at
// its own observation points; the plugin cannot post this kind (the worker-api
// post_message rejects any kind other than string/binary).
using SboxLifecyclePhase = sbox_lifecycle_phase;
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
extern uint32_t g_sbox_lifecycle_test_failure;
#endif
SBOX_CORE_API int sbox_target_post_lifecycle(SboxTarget* target, uint32_t phase);
SBOX_CORE_API void* sbox_target_inbound_event(SboxTarget* target);  // HANDLE to wait on
SBOX_CORE_API int sbox_target_drain_messages(SboxTarget* target, SboxMessageCb cb,
                                        void* ctx);
// HANDLE the broker SIGNALS (via sbox_broker_close) to ask the target to shut
// down its channel loop. Manual-reset, so once closed it stays signaled. NULL if
// the channel was not mapped.
SBOX_CORE_API void* sbox_target_close_event(SboxTarget* target);

// --- target role (used by the sbox.exe worker) ---

// Adopt the broker's same-image seed (written into this image's sandbox globals
// while the process was suspended), init the sandbox target services, and stand
// up the IPC client. Returns NULL on failure, including a missing seed (fail
// closed). Call BEFORE doing any guest warmup; the returned handle is used for
// the calls below.
SBOX_CORE_API SboxTarget* sbox_target_begin(void);

// Prove the broker<->target IPC channel (a cross-call ping). Returns 1 on OK.
SBOX_CORE_API int sbox_target_test_ipc(SboxTarget* target);

// Install the ntdll file interceptions (so denied, policy-allowed opens auto-
// route to the broker) and drop to the restricted token + delayed integrity +
// mitigations. Call AFTER all guest warmup (ACG forbids patching/codegen after).
SBOX_CORE_API int sbox_target_lower_token(SboxTarget* target);

// Worker-only: return the broker's opaque plugin_data blob (carried inline in
// the same-image seed), or (nullptr, 0) if none was set. The bytes live for the
// worker's lifetime; the generic core never interprets them. Returns 1 on
// success, 0 on bad args.
SBOX_CORE_API int sbox_target_plugin_data(SboxTarget* target, const void** data,
                                     size_t* len);

// Worker-only: 1 if ACG (dynamic-code-disable) is armed for this worker (from
// the delayed mitigations the broker seeded), else 0.
SBOX_CORE_API int sbox_target_acg_enabled(SboxTarget* target);

// Release the target handle (does not terminate the process).
SBOX_CORE_API void sbox_target_end(SboxTarget* target);

#endif  // SANDBOX_DLL_SBOX_CORE_INTERNAL_H_
