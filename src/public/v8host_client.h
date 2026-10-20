// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

/* ===========================================================================
 * EXPERIMENTAL API - NOT YET ABI-STABLE
 *
 * This is the consumer-compiled client ABI for v8host.dll. A consuming app
 * compiles against this header, LoadLibrary's v8host.dll, and calls the
 * v8host_* entry points; no import library is required. Every entry
 * point and every function pointer uses a fixed calling convention
 * (V8HOST_CDECL) so a consumer built with a different toolchain still agrees on
 * the ABI. The client connects the app to a broker that hosts guest JavaScript
 * in a sandboxed worker and relays results back on the app thread.
 *
 * This API is experimental and subject to change. Although it is C-based, it is
 * NOT ABI-safe yet: function signatures, struct fields, and enum values may
 * change without notice while the API remains in experimental mode. Do not
 * depend on binary compatibility across builds. Once the API leaves
 * experimental mode it will become ABI-stable and this notice will be removed.
 * =========================================================================== */

#ifndef V8HOST_CLIENT_H_
#define V8HOST_CLIENT_H_

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "v8host_stela.h"

// Every export and every function pointer uses V8HOST_CDECL, so a consumer
// built with a different toolchain still agrees on the calling convention.
#ifndef V8HOST_CDECL
#ifdef _WIN32
#define V8HOST_CDECL __cdecl
#else
#define V8HOST_CDECL
#endif
#endif

// v8host.dll's own build defines V8HOST_IMPL to export the entry points.
// Consumers bind by name via LoadLibrary + GetProcAddress (see the _fn
// typedefs), so by default the declarations carry no import decoration. A
// consumer that links an import library may predefine V8HOST_API as
// __declspec(dllimport).
#ifndef V8HOST_API
#ifdef V8HOST_IMPL
#ifdef _WIN32
#define V8HOST_API __declspec(dllexport)
#else
#define V8HOST_API __attribute__((visibility("default")))
#endif
#else
#define V8HOST_API
#endif
#endif

// The consumer passes the value it compiled against to v8host_initialize. The
// DLL serves older consumers and rejects newer ones. The end-to-end runtime
// contract is the broker wire version, negotiated separately.
#define V8HOST_ABI_VERSION 1u

// A string length meaning "the string is NUL-terminated".
#define V8HOST_AUTO_LENGTH SIZE_MAX

#ifdef __cplusplus
extern "C" {
#endif

/// A UTF-16 code unit.
#ifdef __cplusplus
typedef char16_t v8host_char16;
#else
typedef uint16_t v8host_char16;
#endif

/** Broker mode. The values match the wire endpoint_mode numbering. */
V8HOST_ENUM(int32_t, v8host_broker_mode){
    v8host_broker_mode_dedicated = 0, ///< a broker owned by this session
    v8host_broker_mode_shared = 1,    ///< a broker shared across sessions
};

/** Security tier. */
V8HOST_ENUM(int32_t, v8host_tier){
    v8host_tier_untrusted = 0, ///< jitless plus ACG (default)
    v8host_tier_trusted = 1,   ///< JIT, ACG off
};

/** The worker's mandatory integrity level. The values are sbox's. */
V8HOST_ENUM(int32_t, v8host_integrity_level){
    v8host_integrity_level_low = 0,
    v8host_integrity_level_untrusted = 1,
};

/**
 * Restricted-token levels for ordinary (non-AppContainer) mode, from most to
 * least restrictive. The values are sbox's.
 */
V8HOST_ENUM(int32_t, v8host_token_level){
    v8host_token_level_lockdown = 0,               ///< null SID only
    v8host_token_level_limited = 1,                ///< restricting SIDs
    v8host_token_level_interactive = 2,            ///< plus Owner
    v8host_token_level_restricted_non_admin = 3,   ///< keeps user SIDs
    v8host_token_level_restricted_same_access = 4, ///< all SIDs
};

/** The kind of a relay message frame. */
V8HOST_ENUM(int32_t, v8host_message_kind){
    v8host_message_kind_string = 0, ///< a string frame
    v8host_message_kind_binary = 1, ///< a binary frame
};

/** Session states, reported through v8host_session_state_cb. */
V8HOST_ENUM(int32_t, v8host_session_state){
    v8host_session_state_creating = 0,
    v8host_session_state_ready = 1,
    v8host_session_state_worker_startup_ready = 2,
    v8host_session_state_worker_security_ready = 3,
    v8host_session_state_closed = 4,
};

/**
 * Run events, reported through v8host_run_event_cb. Every event except
 * started is terminal.
 */
V8HOST_ENUM(int32_t, v8host_run_event){
    v8host_run_event_started = 0,
    v8host_run_event_completed = 1,
    v8host_run_event_failed = 2,
    v8host_run_event_cancelled = 3,
    v8host_run_event_worker_exited = 4,
    v8host_run_event_broker_lost = 5,
};

/** Broker states, reported through v8host_broker_state_cb. */
V8HOST_ENUM(int32_t, v8host_broker_state){
    v8host_broker_state_connected = 0,
    v8host_broker_state_disconnected = 1, ///< terminal; the status says why
};

/** Worker states, reported through v8host_worker_state_cb, in this order. */
V8HOST_ENUM(int32_t, v8host_worker_state){
    v8host_worker_state_startup_ready = 0,  ///< warmed up; the process ID is known
    v8host_worker_state_security_ready = 1, ///< lockdown observed
    v8host_worker_state_closed = 2,         ///< terminal; the status says why
};

/** Runtime states, reported through v8host_runtime_state_cb, in this order. */
V8HOST_ENUM(int32_t, v8host_runtime_state){
    v8host_runtime_state_ready = 0,
    v8host_runtime_state_closed = 1, ///< terminal; the status says why
};

/** Worker lifetime flags. */
V8HOST_FLAGS(uint32_t, v8host_worker_flags){
    v8host_worker_flags_none = 0,
    /// Close the worker when its last runtime closes (a host close, a failure,
    /// a refused open, or callback backpressure). The worker then reports
    /// closed with v8host_status_ok.
    v8host_worker_flags_close_when_empty = 1u << 0,
    /// max_runtimes caps the runtimes opened over the worker's lifetime, not
    /// just those open at once.
    v8host_worker_flags_single_use = 1u << 1,
};

/**
 * Result codes. 0 is success. Errors are grouped into bands by failure class,
 * so new codes can be added to a band without renumbering, and a consumer can
 * map an unknown code to its band (status & 0xF000):
 * - 0x1000: the caller's contract and quotas (fail-closed);
 * - 0x2000: lifecycle, state, and local resources;
 * - 0x3000: the broker transport and connection;
 * - 0x4000: asynchronous session and run terminal conditions;
 * - 0x7000: an unexpected internal failure.
 */
V8HOST_ENUM(int32_t, v8host_status){
    /// Success. For asynchronous calls, the inputs were validated and accepted.
    v8host_status_ok = 0,

    v8host_status_invalid_arg = 0x1001,         ///< NULL or invalid pointer, enum, length, or text
    v8host_status_invalid_struct_size = 0x1002, ///< struct_size is below the version 1 size
    v8host_status_version_mismatch = 0x1003,    ///< the consumer's ABI version is newer than the DLL's
    v8host_status_quota_exceeded = 0x1004,      ///< a documented cap was exceeded (for example 64)

    v8host_status_not_initialized = 0x2001, ///< called before v8host_initialize
    v8host_status_invalid_state = 0x2002,   ///< the handle is closed or invalid for the operation
    v8host_status_no_memory = 0x2003,       ///< a local allocation failed

    v8host_status_connect_failed = 0x3001, ///< rendezvous or connect-or-launch failed
    v8host_status_protocol_error = 0x3002, ///< malformed or unexpected wire traffic
    v8host_status_broker_lost = 0x3003,    ///< the broker transport dropped (terminal)

    v8host_status_run_terminal = 0x4001,          ///< post or cancel on an ended run
    v8host_status_callback_backpressure = 0x4002, ///< the app thread isn't draining callbacks
    /// A later run's tier, engine, or snapshot conflicts with the profile that
    /// the session's first run bound, or a run on a runtime sets any of them:
    /// a runtime's profile belongs to its worker.
    v8host_status_profile_already_bound = 0x4003,
    /// The worker process ended without being asked to. The closed status of
    /// the worker and of each of its runtimes.
    v8host_status_worker_exited = 0x4004,
    /// The engine couldn't create the runtime, or the runtime failed.
    v8host_status_runtime_failed = 0x4005,

    v8host_status_internal_error = 0x7001, ///< an unexpected internal failure
};

/// An opaque session handle. A session owns all of its runs.
typedef struct v8host_session_s* v8host_session;
/// An opaque run handle. There is no per-run release: run handles live until
/// v8host_session_close, or until v8host_runtime_close for a runtime's run.
typedef struct v8host_run_s* v8host_run;
/// An opaque broker connection. It owns its workers.
typedef struct v8host_broker_s* v8host_broker;
/// An opaque worker: one sandboxed process. It owns its runtimes. A worker
/// handle stays valid after its broker closes, until the host closes it.
typedef struct v8host_worker_s* v8host_worker;
/// An opaque runtime: one V8 isolate on its own thread in a worker. It owns its
/// runs. Runtimes in one worker are separated only by their isolates.
typedef struct v8host_runtime_s* v8host_runtime;

// Configuration builders (pull model). v8host_session_open and
// v8host_session_start_run create a builder, pass it to the consumer's
// configure callback, and free it when the callback returns; the handle never
// escapes the callback. A fresh builder holds the defaults. Every setter
// returns a status, and the first failure poisons the builder: later setters
// return that same status and change nothing, and the open or start_run call
// fails with it.

/// The session configuration builder, valid only inside the configure callback.
typedef struct v8host_config_s* v8host_config;
/// The run configuration builder, valid only inside the configure callback.
typedef struct v8host_run_config_s* v8host_run_config;
/// The broker configuration builder, valid only inside the configure callback.
typedef struct v8host_broker_config_s* v8host_broker_config;
/// The worker configuration builder, valid only inside the configure callback.
typedef struct v8host_worker_config_s* v8host_worker_config;
/// The runtime configuration builder, valid only inside the configure callback.
typedef struct v8host_runtime_config_s* v8host_runtime_config;

/**
 * Fills the session builder. Called synchronously, once, on the calling
 * thread, from inside v8host_session_open. A non-ok return fails the open with
 * that status.
 */
typedef v8host_status(V8HOST_CDECL* v8host_configure_session_cb)(
    V8HOST_NULLABLE void* user_data,
    v8host_config config);
/**
 * Fills the run builder. Called synchronously, once, on the calling thread,
 * from inside v8host_session_start_run. A non-ok return fails the start with
 * that status.
 */
typedef v8host_status(V8HOST_CDECL* v8host_configure_run_cb)(
    V8HOST_NULLABLE void* user_data,
    v8host_run_config config);
/**
 * Fills the broker builder. Called synchronously, once, on the calling thread,
 * from inside v8host_broker_open. A non-ok return fails the open with that
 * status.
 */
typedef v8host_status(V8HOST_CDECL* v8host_configure_broker_cb)(
    V8HOST_NULLABLE void* user_data,
    v8host_broker_config config);
/**
 * Fills the worker builder. Called synchronously, once, on the calling thread,
 * from inside v8host_worker_open. A non-ok return fails the open with that
 * status.
 */
typedef v8host_status(V8HOST_CDECL* v8host_configure_worker_cb)(
    V8HOST_NULLABLE void* user_data,
    v8host_worker_config config);
/**
 * Fills the runtime builder. Called synchronously, once, on the calling
 * thread, from inside v8host_runtime_open. A non-ok return fails the open with
 * that status.
 */
typedef v8host_status(V8HOST_CDECL* v8host_configure_runtime_cb)(
    V8HOST_NULLABLE void* user_data,
    v8host_runtime_config config);

// Session configuration setters. String setters take a length in code units
// (bytes for _utf8, v8host_char16 units for _utf16), or V8HOST_AUTO_LENGTH for
// a NUL-terminated string. A NULL string, an embedded NUL, invalid UTF-8, or an
// unpaired surrogate returns v8host_status_invalid_arg.

/// Default: v8host_broker_mode_dedicated.
V8HOST_API v8host_status V8HOST_CDECL v8host_config_set_broker_mode(
    v8host_config config,
    v8host_broker_mode value);
/// The session's default tier. Default: v8host_tier_untrusted.
V8HOST_API v8host_status V8HOST_CDECL v8host_config_set_tier(
    v8host_config config,
    v8host_tier value);
/// The worker's mandatory integrity level. Default: v8host_integrity_level_low.
V8HOST_API v8host_status V8HOST_CDECL v8host_config_set_integrity_level(
    v8host_config config,
    v8host_integrity_level value);
/// The integrity level applied at lockdown. Default: v8host_integrity_level_low.
V8HOST_API v8host_status V8HOST_CDECL v8host_config_set_delayed_integrity_level(
    v8host_config config,
    v8host_integrity_level value);
/// The token the worker starts with. Default: v8host_token_level_restricted_same_access.
/// A lockdown-level initial token cannot load the DLLs needed to start the process.
V8HOST_API v8host_status V8HOST_CDECL v8host_config_set_initial_token_level(
    v8host_config config,
    v8host_token_level value);
/// The token level applied at lockdown. Default: v8host_token_level_lockdown.
V8HOST_API v8host_status V8HOST_CDECL v8host_config_set_lockdown_token_level(
    v8host_config config,
    v8host_token_level value);
/// ACG. A trusted run requires this to be false. Default: false.
V8HOST_API v8host_status V8HOST_CDECL v8host_config_set_prohibit_dynamic_code(
    v8host_config config,
    bool value);
/// Adds a path pattern the worker may access. At most 64 rules
/// (v8host_status_quota_exceeded beyond that).
V8HOST_API v8host_status V8HOST_CDECL v8host_config_add_file_rule_utf8(
    v8host_config config,
    const char* pattern,
    size_t length,
    bool readonly);
V8HOST_API v8host_status V8HOST_CDECL v8host_config_add_file_rule_utf16(
    v8host_config config,
    const v8host_char16* pattern,
    size_t length,
    bool readonly);
/// AppContainer requires a non-empty profile name. Default: false.
V8HOST_API v8host_status V8HOST_CDECL v8host_config_set_use_app_container(
    v8host_config config,
    bool value);
/// LPAC requires AppContainer. Default: false.
V8HOST_API v8host_status V8HOST_CDECL
v8host_config_set_low_privilege_app_container(v8host_config config, bool value);
/// Required (non-empty) with AppContainer, and must stay empty without it.
V8HOST_API v8host_status V8HOST_CDECL v8host_config_set_profile_name_utf8(
    v8host_config config,
    const char* value,
    size_t length);
V8HOST_API v8host_status V8HOST_CDECL v8host_config_set_profile_name_utf16(
    v8host_config config,
    const v8host_char16* value,
    size_t length);
/// Adds a non-empty capability SID. Permitted only with AppContainer. At most
/// 64 capabilities.
V8HOST_API v8host_status V8HOST_CDECL v8host_config_add_capability_utf8(
    v8host_config config,
    const char* value,
    size_t length);
V8HOST_API v8host_status V8HOST_CDECL v8host_config_add_capability_utf16(
    v8host_config config,
    const v8host_char16* value,
    size_t length);

// Run configuration setters.

/// Overrides the session tier for this run. It may not weaken the session
/// envelope: a trusted run is rejected unless the session has ACG off.
V8HOST_API v8host_status V8HOST_CDECL v8host_run_config_set_tier(
    v8host_run_config config,
    v8host_tier value);
/// A bare filename resolved within the worker's payload directory, never a
/// path; the broker signature-verifies it. Unset: the tier default.
V8HOST_API v8host_status V8HOST_CDECL v8host_run_config_set_engine_dll_utf8(
    v8host_run_config config,
    const char* value,
    size_t length);
V8HOST_API v8host_status V8HOST_CDECL v8host_run_config_set_engine_dll_utf16(
    v8host_run_config config,
    const v8host_char16* value,
    size_t length);
/// A non-empty payload-relative path, read before lockdown. Unset: none.
V8HOST_API v8host_status V8HOST_CDECL
v8host_run_config_set_startup_snapshot_path_utf8(v8host_run_config config,
                                                 const char* value,
                                                 size_t length);
V8HOST_API v8host_status V8HOST_CDECL
v8host_run_config_set_startup_snapshot_path_utf16(v8host_run_config config,
                                                  const v8host_char16* value,
                                                  size_t length);
/// The guest script or bundle reference, copied. data is NULL iff size is 0.
V8HOST_API v8host_status V8HOST_CDECL v8host_run_config_set_payload(
    v8host_run_config config,
    V8HOST_NULLABLE const void* data,
    size_t size);

// Worker configuration setters. A worker gets its whole profile when it opens:
// the policy setters mirror the session's, with the same defaults, limits, and
// text rules. ACG follows the tier: untrusted is jitless with ACG, trusted is
// JIT without it. The broker mode is the broker's.

/// Default: v8host_tier_untrusted.
V8HOST_API v8host_status V8HOST_CDECL v8host_worker_config_set_tier(
    v8host_worker_config config,
    v8host_tier value);
/// Default: v8host_integrity_level_low.
V8HOST_API v8host_status V8HOST_CDECL v8host_worker_config_set_integrity_level(
    v8host_worker_config config,
    v8host_integrity_level value);
/// Default: v8host_integrity_level_low.
V8HOST_API v8host_status V8HOST_CDECL
v8host_worker_config_set_delayed_integrity_level(v8host_worker_config config,
                                                 v8host_integrity_level value);
/// The token the worker starts with. Default: v8host_token_level_restricted_same_access.
/// A lockdown-level initial token cannot load the DLLs needed to start the process.
V8HOST_API v8host_status V8HOST_CDECL
v8host_worker_config_set_initial_token_level(v8host_worker_config config,
                                             v8host_token_level value);
/// Default: v8host_token_level_lockdown.
V8HOST_API v8host_status V8HOST_CDECL
v8host_worker_config_set_lockdown_token_level(v8host_worker_config config,
                                              v8host_token_level value);
/// At most 64 rules (v8host_status_quota_exceeded beyond that).
V8HOST_API v8host_status V8HOST_CDECL v8host_worker_config_add_file_rule_utf8(
    v8host_worker_config config,
    const char* pattern,
    size_t length,
    bool readonly);
V8HOST_API v8host_status V8HOST_CDECL v8host_worker_config_add_file_rule_utf16(
    v8host_worker_config config,
    const v8host_char16* pattern,
    size_t length,
    bool readonly);
/// AppContainer requires a non-empty profile name. Default: false.
V8HOST_API v8host_status V8HOST_CDECL v8host_worker_config_set_use_app_container(
    v8host_worker_config config,
    bool value);
/// LPAC requires AppContainer. Default: false.
V8HOST_API v8host_status V8HOST_CDECL
v8host_worker_config_set_low_privilege_app_container(
    v8host_worker_config config,
    bool value);
/// Required (non-empty) with AppContainer, and must stay empty without it.
V8HOST_API v8host_status V8HOST_CDECL v8host_worker_config_set_profile_name_utf8(
    v8host_worker_config config,
    const char* value,
    size_t length);
V8HOST_API v8host_status V8HOST_CDECL
v8host_worker_config_set_profile_name_utf16(v8host_worker_config config,
                                            const v8host_char16* value,
                                            size_t length);
/// A non-empty capability SID, only with AppContainer. At most 64.
V8HOST_API v8host_status V8HOST_CDECL v8host_worker_config_add_capability_utf8(
    v8host_worker_config config,
    const char* value,
    size_t length);
V8HOST_API v8host_status V8HOST_CDECL v8host_worker_config_add_capability_utf16(
    v8host_worker_config config,
    const v8host_char16* value,
    size_t length);
/// A bare filename in the payload directory, never a path; the broker
/// signature-verifies it. Default: the tier's engine.
V8HOST_API v8host_status V8HOST_CDECL v8host_worker_config_set_engine_dll_utf8(
    v8host_worker_config config,
    const char* value,
    size_t length);
V8HOST_API v8host_status V8HOST_CDECL
v8host_worker_config_set_engine_dll_utf16(v8host_worker_config config,
                                          const v8host_char16* value,
                                          size_t length);
/// A non-empty payload-relative startup snapshot the worker reads before
/// lockdown, for its runtimes to choose by index (the order added, from 0). At
/// most 8 (v8host_status_quota_exceeded beyond that).
V8HOST_API v8host_status V8HOST_CDECL
v8host_worker_config_add_startup_snapshot_path_utf8(v8host_worker_config config,
                                                    const char* value,
                                                    size_t length);
V8HOST_API v8host_status V8HOST_CDECL
v8host_worker_config_add_startup_snapshot_path_utf16(
    v8host_worker_config config,
    const v8host_char16* value,
    size_t length);
/// The runtimes the worker may host at once: 1..16, default 1. 0 returns
/// v8host_status_invalid_arg; above 16, v8host_status_quota_exceeded.
V8HOST_API v8host_status V8HOST_CDECL v8host_worker_config_set_max_runtimes(
    v8host_worker_config config,
    uint32_t value);
/// Default: v8host_worker_flags_none. An unknown bit returns
/// v8host_status_invalid_arg.
V8HOST_API v8host_status V8HOST_CDECL v8host_worker_config_set_flags(
    v8host_worker_config config,
    v8host_worker_flags value);

// Runtime configuration setters.

/// An index into the worker's startup snapshots. Default: no snapshot.
/// v8host_runtime_open returns v8host_status_invalid_arg for an index the
/// worker doesn't have.
V8HOST_API v8host_status V8HOST_CDECL
v8host_runtime_config_set_startup_snapshot_index(v8host_runtime_config config,
                                                 uint32_t index);

// Callback contract (design §9.1/§9.3): every callback is invoked on the app
// (callback) thread captured by the first successful v8host_initialize,
// never on an internal I/O thread, and never while an internal lock is held. Any
// buffer passed to a callback is owned by v8host and valid ONLY for the duration
// of that callback - copy it if you need to retain it; the client retains none
// of your pointers. Reentrant start/post/cancel/set_callbacks/close from inside
// a callback is supported.

/// Session lifecycle transition or terminal error. `status` is v8host_status_ok
/// for a benign transition, or a terminal error code when the session has failed
/// (e.g. v8host_status_callback_backpressure, v8host_status_broker_lost).
typedef void(V8HOST_CDECL* v8host_session_state_cb)(
    V8HOST_NULLABLE void* user_data,
    v8host_session session,
    v8host_session_state state,
    v8host_status status);

/// Run lifecycle event, including the single app-visible terminal event selected
/// by the run-state arbiter. A terminal event carries the run's final `status`:
/// v8host_status_ok on clean completion, else an error code (e.g.
/// v8host_status_run_terminal on a post-terminal outcome,
/// v8host_status_broker_lost on disconnect). A broker relay-cap overflow may end
/// a run with failed / v8host_status_quota_exceeded. A per-connection overflow
/// also closes the connection; its other live runs then end with broker_lost /
/// v8host_status_broker_lost.
typedef void(V8HOST_CDECL* v8host_run_event_cb)(V8HOST_NULLABLE void* user_data,
                                                v8host_run run,
                                                v8host_run_event event,
                                                v8host_status status);

/// Inbound relay frame from the worker for `run`. `data`/`size` are borrowed for
/// the callback only; `data` is NULL when `size` is 0.
typedef void(V8HOST_CDECL* v8host_relay_message_cb)(
    V8HOST_NULLABLE void* user_data,
    v8host_run run,
    v8host_message_kind kind,
    V8HOST_NULLABLE const void* data,
    size_t size);

/// The broker transport dropped. Every live session and run on this connection is
/// independently marked terminal with v8host_status_broker_lost through the
/// session and run callbacks; the client does not replay prior requests.
typedef void(V8HOST_CDECL* v8host_broker_disconnect_cb)(
    V8HOST_NULLABLE void* user_data);

/**
 * Versioned callback table, installed per session via
 * v8host_session_set_callbacks. The caller sets struct_size = sizeof(T); the
 * DLL rejects a smaller table with v8host_status_invalid_struct_size and
 * ignores unknown trailing fields. The table is copied on install and
 * atomically replaces the prior table for FUTURE dispatch; already-queued
 * events use the table snapshot captured when they were queued. Any member may
 * be NULL to decline that notification.
 */
typedef struct v8host_callbacks {
  uint32_t struct_size;  ///< sizeof(v8host_callbacks)
  /// Passed to every callback below.
  V8HOST_NULLABLE void* user_data;
  /// Session state transitions and terminal session errors.
  V8HOST_NULLABLE V8HOST_SCOPE_OBJECT v8host_session_state_cb on_session_state;
  /// Run lifecycle events.
  V8HOST_NULLABLE V8HOST_SCOPE_OBJECT v8host_run_event_cb on_run_event;
  /// Inbound relay frames.
  V8HOST_NULLABLE V8HOST_SCOPE_OBJECT v8host_relay_message_cb on_relay_message;
  /// Broker transport loss.
  V8HOST_NULLABLE V8HOST_SCOPE_OBJECT v8host_broker_disconnect_cb
      on_broker_disconnect;
} v8host_callbacks;

#define V8HOST_CALLBACKS_INIT \
  { sizeof(v8host_callbacks), NULL, NULL, NULL, NULL, NULL }

/// Broker state transition. `status` is v8host_status_ok for connected, and
/// says why for disconnected: v8host_status_connect_failed,
/// v8host_status_protocol_error, v8host_status_broker_lost, or
/// v8host_status_callback_backpressure. Every live run, runtime, and worker
/// gets its terminal first, in that order; nothing is replayed.
typedef void(V8HOST_CDECL* v8host_broker_state_cb)(
    V8HOST_NULLABLE void* user_data,
    v8host_broker broker,
    v8host_broker_state state,
    v8host_status status);

/// Worker state transition. Closed carries v8host_status_ok for a
/// close_when_empty close, v8host_status_worker_exited when the process ended
/// unasked (its runs get run_event_worker_exited, then its runtimes close,
/// first), a refused open's mapped status, or the broker's disconnect status.
typedef void(V8HOST_CDECL* v8host_worker_state_cb)(
    V8HOST_NULLABLE void* user_data,
    v8host_worker worker,
    v8host_worker_state state,
    v8host_status status);

/// Runtime state transition. Closed carries v8host_status_runtime_failed (its
/// live runs fail with that status first), v8host_status_worker_exited,
/// v8host_status_callback_backpressure, a refused open's mapped status, or the
/// broker's disconnect status.
typedef void(V8HOST_CDECL* v8host_runtime_state_cb)(
    V8HOST_NULLABLE void* user_data,
    v8host_runtime runtime,
    v8host_runtime_state state,
    v8host_status status);

/**
 * A broker's callback table, copied by v8host_broker_config_set_callbacks and
 * fixed for the broker's life. The caller sets struct_size = sizeof(T). Every
 * event of the broker, its workers, runtimes, and runs arrives in one global
 * order, on the app thread, never under an internal lock. Each runtime may
 * have at most 256 queued callbacks; beyond that it closes with
 * v8host_status_callback_backpressure. The broker may have at most 4096
 * queued callbacks and 16 MiB of queued relay bytes; beyond that it
 * disconnects with v8host_status_callback_backpressure. A host close
 * suppresses every queued callback for that object and its children. Any
 * member may be NULL to decline that notification.
 */
typedef struct v8host_broker_callbacks {
  uint32_t struct_size;  ///< sizeof(v8host_broker_callbacks)
  /// Passed to every callback below.
  V8HOST_NULLABLE void* user_data;
  V8HOST_NULLABLE V8HOST_SCOPE_OBJECT v8host_broker_state_cb on_broker_state;
  V8HOST_NULLABLE V8HOST_SCOPE_OBJECT v8host_worker_state_cb on_worker_state;
  V8HOST_NULLABLE V8HOST_SCOPE_OBJECT v8host_runtime_state_cb on_runtime_state;
  V8HOST_NULLABLE V8HOST_SCOPE_OBJECT v8host_run_event_cb on_run_event;
  V8HOST_NULLABLE V8HOST_SCOPE_OBJECT v8host_relay_message_cb on_relay_message;
} v8host_broker_callbacks;

#define V8HOST_BROKER_CALLBACKS_INIT \
  { sizeof(v8host_broker_callbacks), NULL, NULL, NULL, NULL, NULL, NULL }

// Broker configuration setters.

/// Default: v8host_broker_mode_dedicated.
V8HOST_API v8host_status V8HOST_CDECL v8host_broker_config_set_mode(
    v8host_broker_config config,
    v8host_broker_mode value);
/// Copies the table. Default: no callbacks. A smaller struct_size returns
/// v8host_status_invalid_struct_size.
V8HOST_API v8host_status V8HOST_CDECL v8host_broker_config_set_callbacks(
    v8host_broker_config config,
    const v8host_broker_callbacks* callbacks);

/// Process-idempotent and thread-safe. `abi_version` is the V8HOST_ABI_VERSION
/// the consumer compiled against; a newer value (or 0) returns
/// v8host_status_version_mismatch. The first successful call must occur on the
/// consumer's callback/app thread, which must have a Windows message queue;
/// that thread becomes the callback thread for the process.
V8HOST_API v8host_status V8HOST_CDECL v8host_initialize(uint32_t abi_version);

/// Build and validate the session configuration through `configure` (NULL means
/// all defaults), then begin connecting a session. A synchronous
/// v8host_status_ok means the inputs were accepted, not that the session is
/// connected; the outcome arrives via the session callback. On failure
/// *out_session = NULL.
V8HOST_API v8host_status V8HOST_CDECL v8host_session_open(
    V8HOST_NULLABLE v8host_configure_session_cb configure,
    V8HOST_NULLABLE void* user_data,
    v8host_session* out_session);

/// Install (copy) the callback table for `session`, replacing any prior table.
/// Status-returning so callers can observe a rejected table (bad struct_size,
/// closed session, allocation failure) rather than install silently.
V8HOST_API v8host_status V8HOST_CDECL v8host_session_set_callbacks(
    v8host_session session,
    const v8host_callbacks* callbacks);

/// Build and validate the run configuration through `configure` (NULL means all
/// defaults), then begin a run on `session`. A synchronous v8host_status_ok
/// means the inputs were accepted; the run outcome arrives via the run callback.
/// On failure *out_run = NULL.
V8HOST_API v8host_status V8HOST_CDECL v8host_session_start_run(
    v8host_session session,
    V8HOST_NULLABLE v8host_configure_run_cb configure,
    V8HOST_NULLABLE void* user_data,
    v8host_run* out_run);

/// Queue an outbound relay frame to the worker for `run`. `data`/`size` are
/// copied before this call returns. A `kind` outside v8host_message_kind returns
/// v8host_status_invalid_arg. A terminal run rejects with
/// v8host_status_run_terminal. Transport admission may immediately return
/// v8host_status_connect_failed, v8host_status_protocol_error, or
/// v8host_status_quota_exceeded (nothing queued; the run continues).
V8HOST_API v8host_status V8HOST_CDECL v8host_run_post_message(
    v8host_run run,
    v8host_message_kind kind,
    V8HOST_NULLABLE const void* data,
    size_t size);

/// Request cancellation of `run`. Success means the request was accepted, not
/// that guest code had not already produced effects. A terminal run rejects with
/// v8host_status_run_terminal. Transport admission may immediately return
/// v8host_status_connect_failed, v8host_status_protocol_error, or
/// v8host_status_quota_exceeded (nothing queued).
V8HOST_API v8host_status V8HOST_CDECL v8host_run_cancel(v8host_run run);

/// Close `session` and invalidate all of its run handles. Nonblocking: it marks
/// the session closed, suppresses queued callbacks, starts a best-effort close,
/// and returns without waiting on broker I/O. Safe to call from within a
/// callback. Always succeeds, so there is no status to report.
V8HOST_API void V8HOST_CDECL v8host_session_close(v8host_session session);

/// Build the broker configuration through `configure` (NULL means defaults),
/// then begin connecting. A synchronous v8host_status_ok means the inputs were
/// accepted; the outcome arrives through on_broker_state. A broker has its own
/// connection, never shared with a session. On failure *out_broker = NULL.
V8HOST_API v8host_status V8HOST_CDECL v8host_broker_open(
    V8HOST_NULLABLE v8host_configure_broker_cb configure,
    V8HOST_NULLABLE void* user_data,
    v8host_broker* out_broker);

/// Close the broker and every worker, runtime, and run in it, and suppress
/// their queued callbacks. Worker and runtime handles stay valid until the
/// host closes them; calls on them return v8host_status_invalid_state.
/// Nonblocking; safe inside a callback.
V8HOST_API void V8HOST_CDECL v8host_broker_close(v8host_broker broker);

/// Build the worker profile through `configure` (NULL means defaults), then
/// ask the broker to spawn the worker. Allowed before the broker is connected;
/// the request waits for the connection. Returns v8host_status_quota_exceeded
/// when the broker already has 32 workers the host hasn't closed, or when the
/// profile would exceed 4096 bytes. On failure *out_worker = NULL.
/// A replacement is admitted immediately after a close, but startup waits for
/// retiring workers when necessary to keep at most 32 live worker processes.
V8HOST_API v8host_status V8HOST_CDECL v8host_worker_open(
    v8host_broker broker,
    V8HOST_NULLABLE v8host_configure_worker_cb configure,
    V8HOST_NULLABLE void* user_data,
    v8host_worker* out_worker);

/// Close the worker and all its runtimes and runs, and suppress their queued
/// callbacks. Runtime handles stay valid until the host closes them.
/// Releases the logical worker admission immediately; process retirement is
/// asynchronous and may delay startup of a replacement at the process cap.
/// Nonblocking; safe inside a callback.
V8HOST_API void V8HOST_CDECL v8host_worker_close(v8host_worker worker);

/// The worker's process ID, or 0 before startup_ready or once the worker is
/// closed. Valid only until the worker's closed callback; after that the OS
/// may reuse it.
V8HOST_API uint32_t V8HOST_CDECL v8host_worker_get_process_id(
    v8host_worker worker);

/// The worker's ID, unique on its broker, for logs. 0 once closed.
V8HOST_API uint32_t V8HOST_CDECL v8host_worker_get_id(v8host_worker worker);

/// Build the runtime configuration through `configure` (NULL means defaults),
/// then ask the worker for a new runtime. Allowed before the worker is ready;
/// the runtime is created after security_ready. Returns
/// v8host_status_quota_exceeded when the worker already has max_runtimes open
/// runtimes (with single_use, when it has opened that many), and
/// v8host_status_invalid_state when the worker is closed or closing. On
/// failure *out_runtime = NULL.
V8HOST_API v8host_status V8HOST_CDECL v8host_runtime_open(
    v8host_worker worker,
    V8HOST_NULLABLE v8host_configure_runtime_cb configure,
    V8HOST_NULLABLE void* user_data,
    v8host_runtime* out_runtime);

/// Close the runtime and its runs, and suppress their queued callbacks. Its
/// run handles become invalid. Nonblocking; safe inside a callback. JS that
/// never yields can't be stopped: after 5 s the whole worker closes, and its
/// other runtimes end with v8host_status_worker_exited.
V8HOST_API void V8HOST_CDECL v8host_runtime_close(v8host_runtime runtime);

/// The runtime's ID, unique on its broker, for logs. 0 once closed.
V8HOST_API uint32_t V8HOST_CDECL v8host_runtime_get_id(v8host_runtime runtime);

/// Start a run on `runtime`, configured with the v8host_run_config setters
/// (NULL means defaults). The runtime's profile belongs to its worker, so any
/// tier, engine, or snapshot setting returns
/// v8host_status_profile_already_bound. At most 4 runs are in flight per
/// runtime. On failure *out_run = NULL.
V8HOST_API v8host_status V8HOST_CDECL v8host_runtime_start_run(
    v8host_runtime runtime,
    V8HOST_NULLABLE v8host_configure_run_cb configure,
    V8HOST_NULLABLE void* user_data,
    v8host_run* out_run);

/// The run's ID within its runtime or session, for logs.
V8HOST_API uint32_t V8HOST_CDECL v8host_run_get_id(v8host_run run);

// Function-pointer types for GetProcAddress consumers.

typedef v8host_status(V8HOST_CDECL* v8host_initialize_fn)(uint32_t abi_version);
typedef v8host_status(V8HOST_CDECL* v8host_session_open_fn)(
    V8HOST_NULLABLE v8host_configure_session_cb configure,
    V8HOST_NULLABLE void* user_data,
    v8host_session* out_session);
typedef v8host_status(V8HOST_CDECL* v8host_session_set_callbacks_fn)(
    v8host_session session,
    const v8host_callbacks* callbacks);
typedef v8host_status(V8HOST_CDECL* v8host_session_start_run_fn)(
    v8host_session session,
    V8HOST_NULLABLE v8host_configure_run_cb configure,
    V8HOST_NULLABLE void* user_data,
    v8host_run* out_run);
typedef v8host_status(V8HOST_CDECL* v8host_run_post_message_fn)(
    v8host_run run,
    v8host_message_kind kind,
    V8HOST_NULLABLE const void* data,
    size_t size);
typedef v8host_status(V8HOST_CDECL* v8host_run_cancel_fn)(v8host_run run);
typedef void(V8HOST_CDECL* v8host_session_close_fn)(v8host_session session);
typedef v8host_status(V8HOST_CDECL* v8host_config_set_broker_mode_fn)(
    v8host_config config,
    v8host_broker_mode value);
typedef v8host_status(V8HOST_CDECL* v8host_config_set_tier_fn)(
    v8host_config config,
    v8host_tier value);
typedef v8host_status(V8HOST_CDECL* v8host_config_set_integrity_level_fn)(
    v8host_config config,
    v8host_integrity_level value);
typedef v8host_status(
    V8HOST_CDECL* v8host_config_set_delayed_integrity_level_fn)(
    v8host_config config,
    v8host_integrity_level value);
typedef v8host_status(V8HOST_CDECL* v8host_config_set_initial_token_level_fn)(
    v8host_config config,
    v8host_token_level value);
typedef v8host_status(V8HOST_CDECL* v8host_config_set_lockdown_token_level_fn)(
    v8host_config config,
    v8host_token_level value);
typedef v8host_status(
    V8HOST_CDECL* v8host_config_set_prohibit_dynamic_code_fn)(
    v8host_config config,
    bool value);
typedef v8host_status(V8HOST_CDECL* v8host_config_add_file_rule_utf8_fn)(
    v8host_config config,
    const char* pattern,
    size_t length,
    bool readonly);
typedef v8host_status(V8HOST_CDECL* v8host_config_add_file_rule_utf16_fn)(
    v8host_config config,
    const v8host_char16* pattern,
    size_t length,
    bool readonly);
typedef v8host_status(V8HOST_CDECL* v8host_config_set_use_app_container_fn)(
    v8host_config config,
    bool value);
typedef v8host_status(
    V8HOST_CDECL* v8host_config_set_low_privilege_app_container_fn)(
    v8host_config config,
    bool value);
typedef v8host_status(V8HOST_CDECL* v8host_config_set_profile_name_utf8_fn)(
    v8host_config config,
    const char* value,
    size_t length);
typedef v8host_status(V8HOST_CDECL* v8host_config_set_profile_name_utf16_fn)(
    v8host_config config,
    const v8host_char16* value,
    size_t length);
typedef v8host_status(V8HOST_CDECL* v8host_config_add_capability_utf8_fn)(
    v8host_config config,
    const char* value,
    size_t length);
typedef v8host_status(V8HOST_CDECL* v8host_config_add_capability_utf16_fn)(
    v8host_config config,
    const v8host_char16* value,
    size_t length);
typedef v8host_status(V8HOST_CDECL* v8host_run_config_set_tier_fn)(
    v8host_run_config config,
    v8host_tier value);
typedef v8host_status(V8HOST_CDECL* v8host_run_config_set_engine_dll_utf8_fn)(
    v8host_run_config config,
    const char* value,
    size_t length);
typedef v8host_status(V8HOST_CDECL* v8host_run_config_set_engine_dll_utf16_fn)(
    v8host_run_config config,
    const v8host_char16* value,
    size_t length);
typedef v8host_status(
    V8HOST_CDECL* v8host_run_config_set_startup_snapshot_path_utf8_fn)(
    v8host_run_config config,
    const char* value,
    size_t length);
typedef v8host_status(
    V8HOST_CDECL* v8host_run_config_set_startup_snapshot_path_utf16_fn)(
    v8host_run_config config,
    const v8host_char16* value,
    size_t length);
typedef v8host_status(V8HOST_CDECL* v8host_run_config_set_payload_fn)(
    v8host_run_config config,
    V8HOST_NULLABLE const void* data,
    size_t size);
typedef v8host_status(V8HOST_CDECL* v8host_broker_open_fn)(
    V8HOST_NULLABLE v8host_configure_broker_cb configure,
    V8HOST_NULLABLE void* user_data,
    v8host_broker* out_broker);
typedef void(V8HOST_CDECL* v8host_broker_close_fn)(v8host_broker broker);
typedef v8host_status(V8HOST_CDECL* v8host_broker_config_set_mode_fn)(
    v8host_broker_config config,
    v8host_broker_mode value);
typedef v8host_status(V8HOST_CDECL* v8host_broker_config_set_callbacks_fn)(
    v8host_broker_config config,
    const v8host_broker_callbacks* callbacks);
typedef v8host_status(V8HOST_CDECL* v8host_worker_open_fn)(
    v8host_broker broker,
    V8HOST_NULLABLE v8host_configure_worker_cb configure,
    V8HOST_NULLABLE void* user_data,
    v8host_worker* out_worker);
typedef void(V8HOST_CDECL* v8host_worker_close_fn)(v8host_worker worker);
typedef uint32_t(V8HOST_CDECL* v8host_worker_get_process_id_fn)(
    v8host_worker worker);
typedef uint32_t(V8HOST_CDECL* v8host_worker_get_id_fn)(v8host_worker worker);
typedef v8host_status(V8HOST_CDECL* v8host_worker_config_set_tier_fn)(
    v8host_worker_config config,
    v8host_tier value);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_set_integrity_level_fn)(
    v8host_worker_config config,
    v8host_integrity_level value);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_set_delayed_integrity_level_fn)(
    v8host_worker_config config,
    v8host_integrity_level value);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_set_initial_token_level_fn)(
    v8host_worker_config config,
    v8host_token_level value);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_set_lockdown_token_level_fn)(
    v8host_worker_config config,
    v8host_token_level value);
typedef v8host_status(V8HOST_CDECL* v8host_worker_config_add_file_rule_utf8_fn)(
    v8host_worker_config config,
    const char* pattern,
    size_t length,
    bool readonly);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_add_file_rule_utf16_fn)(
    v8host_worker_config config,
    const v8host_char16* pattern,
    size_t length,
    bool readonly);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_set_use_app_container_fn)(
    v8host_worker_config config,
    bool value);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_set_low_privilege_app_container_fn)(
    v8host_worker_config config,
    bool value);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_set_profile_name_utf8_fn)(
    v8host_worker_config config,
    const char* value,
    size_t length);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_set_profile_name_utf16_fn)(
    v8host_worker_config config,
    const v8host_char16* value,
    size_t length);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_add_capability_utf8_fn)(
    v8host_worker_config config,
    const char* value,
    size_t length);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_add_capability_utf16_fn)(
    v8host_worker_config config,
    const v8host_char16* value,
    size_t length);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_set_engine_dll_utf8_fn)(
    v8host_worker_config config,
    const char* value,
    size_t length);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_set_engine_dll_utf16_fn)(
    v8host_worker_config config,
    const v8host_char16* value,
    size_t length);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_add_startup_snapshot_path_utf8_fn)(
    v8host_worker_config config,
    const char* value,
    size_t length);
typedef v8host_status(
    V8HOST_CDECL* v8host_worker_config_add_startup_snapshot_path_utf16_fn)(
    v8host_worker_config config,
    const v8host_char16* value,
    size_t length);
typedef v8host_status(V8HOST_CDECL* v8host_worker_config_set_max_runtimes_fn)(
    v8host_worker_config config,
    uint32_t value);
typedef v8host_status(V8HOST_CDECL* v8host_worker_config_set_flags_fn)(
    v8host_worker_config config,
    v8host_worker_flags value);
typedef v8host_status(V8HOST_CDECL* v8host_runtime_open_fn)(
    v8host_worker worker,
    V8HOST_NULLABLE v8host_configure_runtime_cb configure,
    V8HOST_NULLABLE void* user_data,
    v8host_runtime* out_runtime);
typedef void(V8HOST_CDECL* v8host_runtime_close_fn)(v8host_runtime runtime);
typedef uint32_t(V8HOST_CDECL* v8host_runtime_get_id_fn)(
    v8host_runtime runtime);
typedef v8host_status(
    V8HOST_CDECL* v8host_runtime_config_set_startup_snapshot_index_fn)(
    v8host_runtime_config config,
    uint32_t index);
typedef v8host_status(V8HOST_CDECL* v8host_runtime_start_run_fn)(
    v8host_runtime runtime,
    V8HOST_NULLABLE v8host_configure_run_cb configure,
    V8HOST_NULLABLE void* user_data,
    v8host_run* out_run);
typedef uint32_t(V8HOST_CDECL* v8host_run_get_id_fn)(v8host_run run);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // V8HOST_CLIENT_H_
