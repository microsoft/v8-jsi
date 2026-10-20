/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT license.
 *
 * V8-specific exports for the JSI ABI: runtime creation, config setters,
 * process-global V8 flags, startup snapshots, and the inspector.
 *
 * jsi_config (declared in jsi_abi.h) is the engine-agnostic opaque config
 * type; the v8_jsi_abi_config_set_* setters here are V8-specific. Another
 * engine can add its own create function and setters against the same type.
 *
 * The DLL owns the config layout AND its lifetime: v8_jsi_abi_runtime_create
 * allocates the jsi_config, hands it to the consumer's
 * jsi_configure_runtime_cb to be populated via the setters, then frees it.
 * Consumers never create or destroy a config; they only see the opaque handle
 * inside the callback. A new V8 option is a new setter — no struct version to
 * bump, no field-order contract to preserve.
 *
 * Every export has an _fn typedef for callers that resolve it with
 * GetProcAddress.
 *
 * Pure C header. Safe to include from C or C++.
 */

/* ===========================================================================
 * EXPERIMENTAL API - NOT YET ABI-STABLE
 *
 * This API is experimental and subject to change. Although it is C-based, it is
 * NOT ABI-safe yet: function signatures, function-table layouts, struct fields, enum
 * values, and error codes may change without notice while the API remains in
 * experimental mode. Do not depend on binary compatibility across builds. Once
 * the API leaves experimental mode it will become ABI-stable and this notice
 * will be removed.
 * =========================================================================== */

#ifndef V8_JSI_ABI_H_
#define V8_JSI_ABI_H_

#include "jsi_abi/jsi_abi.h"

/**
 * Exports only while v8jsi builds (V8_JSI_ABI_IMPL defined). Consumers get it
 * empty, or may predefine it as __declspec(dllimport).
 */
#ifndef V8_JSI_ABI_API
#ifdef V8_JSI_ABI_IMPL
#ifdef _WIN32
#define V8_JSI_ABI_API __declspec(dllexport)
#else
#define V8_JSI_ABI_API __attribute__((visibility("default")))
#endif
#else
#define V8_JSI_ABI_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Runtime creation
 *============================================================================*/

/**
 * Create a V8-backed jsi_runtime.
 *
 * abi_version: the JSI_ABI_VERSION the consumer compiled against. A version
 *   this implementation does not support fails with
 *   jsi_status_version_mismatch.
 * configure: invoked with a factory-owned jsi_config so the consumer can set
 *   fields via the v8_jsi_abi_config_set_* setters below. May be NULL
 *   (defaults). The config handle is valid only for the duration of that
 *   callback. If configure returns a failure status, creation fails with it.
 *   If a setter failed (poisoning the config), creation fails with that
 *   setter's status even if configure returned jsi_status_ok.
 * user_data: passed straight through to configure.
 * out_runtime: receives the runtime (refcount 1; release via
 *   runtime->api->release), or NULL on failure.
 *
 * Returns jsi_status_ok, jsi_status_invalid_arg (out_runtime NULL),
 * jsi_status_version_mismatch, the configure/poison status, or
 * jsi_status_native_error if V8 could not create the isolate.
 */
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL v8_jsi_abi_runtime_create(
    uint32_t abi_version,
    JSI_ABI_NULLABLE jsi_configure_runtime_cb configure,
    JSI_ABI_NULLABLE void *user_data,
    jsi_runtime **out_runtime);
typedef jsi_status(JSI_ABI_CDECL *v8_jsi_abi_runtime_create_fn)(
    uint32_t abi_version,
    JSI_ABI_NULLABLE jsi_configure_runtime_cb configure,
    JSI_ABI_NULLABLE void *user_data,
    jsi_runtime **out_runtime);

/*============================================================================
 * Config setters
 *
 * Valid only on the jsi_config handed to the configure callback. Each setter
 * returns jsi_status_ok, or jsi_status_invalid_arg for a NULL config or a bad
 * value. The first failure poisons the config: every later setter returns
 * that same status without applying its value, and v8_jsi_abi_runtime_create
 * fails with it. A setter that takes ownership of data (startup snapshot,
 * script cache, task runner) releases that data through its deleter when it
 * fails, so nothing leaks.
 *============================================================================*/

/*----------------------------------------------------------------------------
 * Heap / threading
 *----------------------------------------------------------------------------*/

/// Initial V8 heap size in bytes. 0 means the V8 default.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_initial_heap_size(jsi_config config, size_t bytes);
typedef jsi_status(JSI_ABI_CDECL *v8_jsi_abi_config_set_initial_heap_size_fn)(
    jsi_config config, size_t bytes);

/// Maximum V8 heap size in bytes. 0 means the V8 default.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_maximum_heap_size(jsi_config config, size_t bytes);
typedef jsi_status(JSI_ABI_CDECL *v8_jsi_abi_config_set_maximum_heap_size_fn)(
    jsi_config config, size_t bytes);

/**
 * Cap the V8 worker thread pool. 0 means default
 * (V8 picks min(num_cores - 1, 16)).
 */
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_thread_pool_size(jsi_config config, uint8_t size);
typedef jsi_status(JSI_ABI_CDECL *v8_jsi_abi_config_set_thread_pool_size_fn)(
    jsi_config config, uint8_t size);

/*----------------------------------------------------------------------------
 * Inspector / debugger
 *----------------------------------------------------------------------------*/

/**
 * Name shown in the inspector UI to distinguish parallel runtimes. The text
 * is copied. length may be JSI_ABI_AUTO_LENGTH; value may be NULL only when
 * length is 0 or JSI_ABI_AUTO_LENGTH (an empty name).
 */
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_debugger_runtime_name_utf8(
    jsi_config config,
    JSI_ABI_NULLABLE const char *value,
    size_t length);
typedef jsi_status(
    JSI_ABI_CDECL *v8_jsi_abi_config_set_debugger_runtime_name_utf8_fn)(
    jsi_config config,
    JSI_ABI_NULLABLE const char *value,
    size_t length);

/// Enable the V8 inspector. Default: false.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_inspector_enabled(jsi_config config, bool value);
typedef jsi_status(JSI_ABI_CDECL *v8_jsi_abi_config_set_inspector_enabled_fn)(
    jsi_config config, bool value);

/// Inspector TCP port.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_inspector_port(jsi_config config, uint16_t port);
typedef jsi_status(JSI_ABI_CDECL *v8_jsi_abi_config_set_inspector_port_fn)(
    jsi_config config, uint16_t port);

/// Pause on the first statement until a debugger attaches. Default: false.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_inspector_break_on_start(jsi_config config, bool value);
typedef jsi_status(
    JSI_ABI_CDECL *v8_jsi_abi_config_set_inspector_break_on_start_fn)(
    jsi_config config, bool value);

/*----------------------------------------------------------------------------
 * Microtasks / promises
 *----------------------------------------------------------------------------*/

/**
 * If true, the runtime uses MicrotasksPolicy::kExplicit and the host must
 * drain microtasks itself. Default is false (V8 default policy).
 */
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_explicit_microtask_policy(jsi_config config, bool value);
typedef jsi_status(
    JSI_ABI_CDECL *v8_jsi_abi_config_set_explicit_microtask_policy_fn)(
    jsi_config config, bool value);

/**
 * If true, the PromiseRejectCallback that tracks unhandled rejections is
 * NOT installed. Default is false (track rejections).
 */
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_ignore_unhandled_promises(jsi_config config, bool value);
typedef jsi_status(
    JSI_ABI_CDECL *v8_jsi_abi_config_set_ignore_unhandled_promises_fn)(
    jsi_config config, bool value);

/*----------------------------------------------------------------------------
 * GC
 *----------------------------------------------------------------------------*/

/// Expose the global gc() function (passes --expose_gc to V8).
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_gc_api_enabled(jsi_config config, bool value);
typedef jsi_status(JSI_ABI_CDECL *v8_jsi_abi_config_set_gc_api_enabled_fn)(
    jsi_config config, bool value);

/*----------------------------------------------------------------------------
 * Concurrency
 *----------------------------------------------------------------------------*/

/// Wrap isolate access in v8::Locker for multi-threaded use.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_multi_thread_enabled(jsi_config config, bool value);
typedef jsi_status(
    JSI_ABI_CDECL *v8_jsi_abi_config_set_multi_thread_enabled_fn)(
    jsi_config config, bool value);

/*----------------------------------------------------------------------------
 * Tracing
 *----------------------------------------------------------------------------*/

/// Log JIT code events.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_jit_tracing_enabled(jsi_config config, bool value);
typedef jsi_status(
    JSI_ABI_CDECL *v8_jsi_abi_config_set_jit_tracing_enabled_fn)(
    jsi_config config, bool value);

/// Log inspector protocol messages.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_message_tracing_enabled(jsi_config config, bool value);
typedef jsi_status(
    JSI_ABI_CDECL *v8_jsi_abi_config_set_message_tracing_enabled_fn)(
    jsi_config config, bool value);

/// Log GC events.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_gc_tracing_enabled(jsi_config config, bool value);
typedef jsi_status(
    JSI_ABI_CDECL *v8_jsi_abi_config_set_gc_tracing_enabled_fn)(
    jsi_config config, bool value);

/**
 * V8 ETW provider GUID 57277741-3638-4A4B-BDBA-0AC6E45DA56C
 * (passes --enable-system-instrumentation to V8).
 */
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_system_instrumentation_enabled(jsi_config config,
                                                     bool value);
typedef jsi_status(
    JSI_ABI_CDECL *v8_jsi_abi_config_set_system_instrumentation_enabled_fn)(
    jsi_config config, bool value);

/*============================================================================
 * Process-global V8 engine flags
 *
 * V8 command-line flags are PROCESS-GLOBAL and consumed only during the first
 * platform initialization (the first v8_jsi_abi_runtime_create). They are
 * exposed raw, CLI-style, NOT as per-runtime config setters — a per-runtime
 * field for a process-global value would be silently ignored after the first
 * runtime.
 *
 * Call this BEFORE the first v8_jsi_abi_runtime_create. argv uses V8 CLI flag
 * syntax (e.g. "--sparkplug", "--jitless", "--optimize_for_size",
 * "--max-old-space-size=512"). Forwarded to V8::SetFlagsFromCommandLine.
 *
 * remove_flags=true: V8 removes the flags it recognized from argv, compacts
 * the remaining (unrecognized) entries to the front, and updates *argc to
 * that count — letting the caller report leftover/unknown flags.
 *
 * Returns jsi_status_invalid_arg for a NULL argc, or a NULL argv with a
 * nonzero *argc.
 *============================================================================*/

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_set_v8_flags(size_t *argc, char **argv, bool remove_flags);
typedef jsi_status(JSI_ABI_CDECL *v8_jsi_abi_set_v8_flags_fn)(
    size_t *argc, char **argv, bool remove_flags);

/*============================================================================
 * Startup-snapshot creation (offline build-step API)
 *
 * Builds a V8 startup-snapshot blob from a pure-JS builder script: a dedicated
 * SnapshotCreator isolate runs the script in a fresh context, drains
 * microtasks, and serializes the context's heap. Only pure-JS state can be
 * captured (no host objects/functions). A runtime later deserializes the
 * context from the blob instead of re-evaluating the script.
 *
 * The output is a self-describing CONTAINER: a fixed header (magic, format
 * version, V8 CachedDataVersionTag, engine build-flag digest, raw size)
 * followed by the raw V8 StartupData bytes. The header lets an engine reject
 * a stale or cross-build blob before V8 sees it (a raw V8 startup blob is
 * only loadable by an engine with the identical V8 version and cage /
 * pointer-compression / lite build). Callers (e.g. mkv8snapshot) write the
 * container verbatim; v8_jsi_abi_config_set_startup_snapshot / the runtime
 * strip and validate it. Use v8_jsi_abi_snapshot_check to pre-check a blob
 * read back from disk.
 *
 * script_length may be JSI_ABI_AUTO_LENGTH. On success returns jsi_status_ok
 * and sets *out_data to a freshly allocated container of *out_size bytes
 * (free it with v8_jsi_abi_snapshot_destroy). Returns jsi_status_invalid_arg
 * for a NULL script or output, and jsi_status_native_error if the script
 * fails or V8 cannot build the blob; the outputs are zeroed on failure.
 *============================================================================*/
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL v8_jsi_abi_snapshot_create(
    JSI_ABI_ENCODING(utf8) const char *script,
    size_t script_length,
    JSI_ABI_NULLABLE JSI_ABI_ENCODING(utf8) JSI_ABI_ZERO_TERMINATED
        const char *source_url,
    bool jitless,
    uint8_t **out_data,
    size_t *out_size);
typedef jsi_status(JSI_ABI_CDECL *v8_jsi_abi_snapshot_create_fn)(
    JSI_ABI_ENCODING(utf8) const char *script,
    size_t script_length,
    JSI_ABI_NULLABLE JSI_ABI_ENCODING(utf8) JSI_ABI_ZERO_TERMINATED
        const char *source_url,
    bool jitless,
    uint8_t **out_data,
    size_t *out_size);

/// Free a container returned by v8_jsi_abi_snapshot_create. NULL is a no-op.
V8_JSI_ABI_API void JSI_ABI_CDECL
v8_jsi_abi_snapshot_destroy(JSI_ABI_NULLABLE uint8_t *data);
typedef void(JSI_ABI_CDECL *v8_jsi_abi_snapshot_destroy_fn)(
    JSI_ABI_NULLABLE uint8_t *data);

/**
 * Supply a V8 startup-snapshot container (built with
 * v8_jsi_abi_snapshot_create) to a runtime. The isolate is created from it
 * (Isolate::CreateParams::snapshot_blob) so the embedded script's heap is
 * pre-materialized — neither parsed nor run.
 *
 * Lifetime: the config takes ownership of data when the setter is called; on
 * v8_jsi_abi_runtime_create the runtime takes over. The runtime keeps the
 * bytes alive for the isolate's lifetime and calls
 * data_delete_cb(data_delete_data, data) exactly once on destroy. If creation
 * fails before the runtime is built, the config fires the deleter instead.
 * data_delete_cb may be NULL (the bytes are then caller-owned and must
 * outlive the runtime). NULL data with size 0 clears any previously set
 * snapshot; NULL data with a nonzero size is jsi_status_invalid_arg.
 */
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL v8_jsi_abi_config_set_startup_snapshot(
    jsi_config config,
    JSI_ABI_NULLABLE const uint8_t *data,
    size_t size,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_ASYNC JSI_ABI_CLOSURE(data_delete_data)
        jsi_data_delete_cb data_delete_cb,
    JSI_ABI_NULLABLE void *data_delete_data);
typedef jsi_status(
    JSI_ABI_CDECL *v8_jsi_abi_config_set_startup_snapshot_fn)(
    jsi_config config,
    JSI_ABI_NULLABLE const uint8_t *data,
    size_t size,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_ASYNC JSI_ABI_CLOSURE(data_delete_data)
        jsi_data_delete_cb data_delete_cb,
    JSI_ABI_NULLABLE void *data_delete_data);

/*============================================================================
 * Startup-snapshot container compatibility check
 *
 * A startup blob built by v8_jsi_abi_snapshot_create carries a container
 * header (see that function). Pass the WHOLE container (header + bytes) here
 * to check whether THIS engine can load it. Use this before handing a blob
 * read from disk to a runtime, and on a non-ok result skip the snapshot
 * (create a normal runtime) rather than apply a mismatched blob. The runtime
 * ALSO validates internally as a safety net, so a bad blob never reaches V8
 * even if this check is skipped; this export just lets the consumer choose
 * its fallback up front.
 *
 * Ordering: the version check folds in V8's flag hash, so call this only
 * AFTER any process-global engine flags are set (v8_jsi_abi_set_v8_flags) —
 * the same flags the snapshot was built with. This call initializes V8
 * (idempotent) so the tag is read in its final, flag-implication-enforced
 * state.
 *
 * Returns v8_jsi_abi_snapshot_compat_ok when the container is loadable by
 * this engine, otherwise the reason it is not. This is a classification, not
 * a failure status.
 *============================================================================*/
/// Snapshot-container compatibility. See the compatibility notes above.
JSI_ABI_ENUM(int32_t, v8_jsi_abi_snapshot_compat){
    v8_jsi_abi_snapshot_compat_ok = 0,
    v8_jsi_abi_snapshot_compat_truncated = 1, ///< smaller than the container header
    v8_jsi_abi_snapshot_compat_bad_magic = 2, ///< not a v8jsi snapshot container
    v8_jsi_abi_snapshot_compat_bad_format = 3, ///< unknown container format version
    v8_jsi_abi_snapshot_compat_version_mismatch = 4, ///< V8 CachedDataVersionTag differs
    /// engine build flags differ (cage, pointer compression, lite mode)
    v8_jsi_abi_snapshot_compat_flags_mismatch = 5,
    v8_jsi_abi_snapshot_compat_size_mismatch = 6, ///< header raw size != actual bytes
};

/// Checks whether this engine can load a snapshot container. See above.
V8_JSI_ABI_API v8_jsi_abi_snapshot_compat JSI_ABI_CDECL
v8_jsi_abi_snapshot_check(const uint8_t *data, size_t size);
typedef v8_jsi_abi_snapshot_compat(
    JSI_ABI_CDECL *v8_jsi_abi_snapshot_check_fn)(const uint8_t *data,
                                                 size_t size);

/**
 * Human-readable description of a v8_jsi_abi_snapshot_compat (never NULL;
 * an unknown value gets a generic string).
 */
V8_JSI_ABI_API const char *JSI_ABI_CDECL
v8_jsi_abi_snapshot_compat_to_string(v8_jsi_abi_snapshot_compat value);
typedef const char *(
    JSI_ABI_CDECL *v8_jsi_abi_snapshot_compat_to_string_fn)(
    v8_jsi_abi_snapshot_compat value);

/*============================================================================
 * Script cache (prepared-script storage)
 *
 * Consumers register a pair of C callbacks that load and store compiled
 * bytecode on behalf of prepare_javascript. NULL callbacks mean "no cache" —
 * prepare always falls back to fresh V8 compilation.
 *
 * load_cb: on a hit, sets *out_data / *out_size to the cached bytes and
 *   *out_data_delete_cb / *out_data_delete_data to how the runtime releases
 *   them (called as out_data_delete_cb(out_data_delete_data, out_data) once V8
 *   has consumed them). On a miss, leaves *out_data NULL.
 * store_cb: receives freshly produced bytes. The consumer must call
 *   data_delete_cb(data_delete_data, data) exactly once when done with them.
 *
 * Both callbacks receive NUL-terminated UTF-8 source_url, runtime_name, and
 * cache_tag strings that the runtime owns; they are valid only for the call.
 *
 * Lifetime: the config takes ownership of user_data when the setter is
 * called; on runtime construction the runtime takes over. When the runtime is
 * destroyed it calls user_data_delete_cb(user_data_delete_data, user_data)
 * exactly once. If creation fails before the runtime is built, the config
 * fires the deleter instead.
 *============================================================================*/

/// Loads cached bytecode. See the script cache notes above.
typedef void(JSI_ABI_CDECL *v8_jsi_abi_script_cache_load_cb)(
    JSI_ABI_NULLABLE void *user_data,
    JSI_ABI_ENCODING(utf8) JSI_ABI_ZERO_TERMINATED const char *source_url,
    uint64_t source_hash,
    JSI_ABI_ENCODING(utf8) JSI_ABI_ZERO_TERMINATED const char *runtime_name,
    uint64_t runtime_version,
    JSI_ABI_ENCODING(utf8) JSI_ABI_ZERO_TERMINATED const char *cache_tag,
    const uint8_t **out_data,
    size_t *out_size,
    jsi_data_delete_cb *out_data_delete_cb,
    void **out_data_delete_data);

/// Stores cached bytecode. See the script cache notes above.
typedef void(JSI_ABI_CDECL *v8_jsi_abi_script_cache_store_cb)(
    JSI_ABI_NULLABLE void *user_data,
    JSI_ABI_ENCODING(utf8) JSI_ABI_ZERO_TERMINATED const char *source_url,
    uint64_t source_hash,
    JSI_ABI_ENCODING(utf8) JSI_ABI_ZERO_TERMINATED const char *runtime_name,
    uint64_t runtime_version,
    JSI_ABI_ENCODING(utf8) JSI_ABI_ZERO_TERMINATED const char *cache_tag,
    const uint8_t *data,
    size_t size,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_ASYNC JSI_ABI_CLOSURE(data_delete_data)
        jsi_data_delete_cb data_delete_cb,
    JSI_ABI_NULLABLE void *data_delete_data);

/// Configures callbacks described in the script cache notes above.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL v8_jsi_abi_config_set_script_cache(
    jsi_config config,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_NOTIFIED JSI_ABI_CLOSURE(user_data)
        JSI_ABI_DESTROY(user_data_delete_cb)
            v8_jsi_abi_script_cache_load_cb load_cb,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_NOTIFIED
        JSI_ABI_DESTROY(user_data_delete_cb)
            v8_jsi_abi_script_cache_store_cb store_cb,
    JSI_ABI_NULLABLE void *user_data,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_ASYNC
        JSI_ABI_CLOSURE(user_data_delete_data)
            jsi_data_delete_cb user_data_delete_cb,
    JSI_ABI_NULLABLE void *user_data_delete_data);
typedef jsi_status(JSI_ABI_CDECL *v8_jsi_abi_config_set_script_cache_fn)(
    jsi_config config,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_NOTIFIED JSI_ABI_CLOSURE(user_data)
        JSI_ABI_DESTROY(user_data_delete_cb)
            v8_jsi_abi_script_cache_load_cb load_cb,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_NOTIFIED
        JSI_ABI_DESTROY(user_data_delete_cb)
            v8_jsi_abi_script_cache_store_cb store_cb,
    JSI_ABI_NULLABLE void *user_data,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_ASYNC
        JSI_ABI_CLOSURE(user_data_delete_data)
            jsi_data_delete_cb user_data_delete_cb,
    JSI_ABI_NULLABLE void *user_data_delete_data);

/*============================================================================
 * Task runner (foreground-thread dispatch)
 *
 * The foreground task runner is how the inspector dispatches incoming
 * debugger messages onto the JS thread. NULL post_task_cb means "no task
 * runner" — V8 falls back to its default platform runner.
 *
 * post_task_cb: the consumer must eventually call task_run_cb(task_data) on
 *   the JS thread (or skip it at shutdown), then
 *   task_data_delete_cb(task_data_delete_data, task_data) exactly once.
 *
 * Lifetime: the config takes ownership of user_data when the setter is
 * called; on runtime construction the runtime takes over. When the runtime is
 * destroyed it calls user_data_delete_cb(user_data_delete_data, user_data)
 * exactly once. If creation fails before the runtime is built, the config
 * fires the deleter instead.
 *============================================================================*/

/// Runs a foreground task. See the task runner notes above.
typedef void(JSI_ABI_CDECL *v8_jsi_abi_task_run_cb)(
    JSI_ABI_NULLABLE void *user_data);

/// Posts a foreground task. See the task runner notes above.
typedef void(JSI_ABI_CDECL *v8_jsi_abi_task_runner_post_task_cb)(
    JSI_ABI_NULLABLE void *user_data,
    JSI_ABI_NULLABLE void *task_data,
    JSI_ABI_SCOPE_ASYNC JSI_ABI_CLOSURE(task_data)
        v8_jsi_abi_task_run_cb task_run_cb,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_ASYNC
        JSI_ABI_CLOSURE(task_data_delete_data)
            jsi_data_delete_cb task_data_delete_cb,
    JSI_ABI_NULLABLE void *task_data_delete_data);

/// Configures the foreground task runner described above.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL v8_jsi_abi_config_set_task_runner(
    jsi_config config,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_NOTIFIED
        JSI_ABI_DESTROY(user_data_delete_cb)
            v8_jsi_abi_task_runner_post_task_cb post_task_cb,
    JSI_ABI_NULLABLE void *user_data,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_ASYNC
        JSI_ABI_CLOSURE(user_data_delete_data)
            jsi_data_delete_cb user_data_delete_cb,
    JSI_ABI_NULLABLE void *user_data_delete_data);
typedef jsi_status(JSI_ABI_CDECL *v8_jsi_abi_config_set_task_runner_fn)(
    jsi_config config,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_NOTIFIED
        JSI_ABI_DESTROY(user_data_delete_cb)
            v8_jsi_abi_task_runner_post_task_cb post_task_cb,
    JSI_ABI_NULLABLE void *user_data,
    JSI_ABI_NULLABLE JSI_ABI_SCOPE_ASYNC
        JSI_ABI_CLOSURE(user_data_delete_data)
            jsi_data_delete_cb user_data_delete_cb,
    JSI_ABI_NULLABLE void *user_data_delete_data);

/*============================================================================
 * Inspector control
 *
 * The inspector is wired into v8_jsi_abi_runtime_create when the config
 * enables it (v8_jsi_abi_config_set_inspector_enabled).
 * v8_jsi_abi_runtime_open_inspector starts a previously quiet inspector on
 * such a runtime; calling it again is harmless. Returns
 * jsi_status_invalid_arg for a NULL runtime, and jsi_status_ok otherwise
 * (including when the runtime has no inspector).
 *============================================================================*/

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_runtime_open_inspector(jsi_runtime *runtime);
typedef jsi_status(JSI_ABI_CDECL *v8_jsi_abi_runtime_open_inspector_fn)(
    jsi_runtime *runtime);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* V8_JSI_ABI_H_ */

/*============================================================================
 * Note: v8_attach_node_api lives in src/jsi_abi/v8_node_api_attach.h. It is
 * kept out of this header so consumers of the V8 JSI config do not have to
 * pull in the Node-API headers (napi_env / napi_status types).
 *============================================================================*/
