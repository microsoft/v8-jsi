/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 *
 * V8-specific Node-API hosting extensions.
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
 * NOT ABI-safe yet: signatures, types, and status behavior may change without
 * notice while the API remains experimental. Do not depend on binary
 * compatibility across builds.
 * =========================================================================== */

#ifndef V8_NODE_API_H_
#define V8_NODE_API_H_

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "js_native_api.h"
#include "jsi_abi/jsi_abi.h"
#include "v8_node_api_stela.h"

/// The current V8 Node-API hosting ABI version.
#define V8_NODE_API_ABI_VERSION 1u

/// Sentinel for a NUL-terminated UTF-8 input.
#define V8_NODE_API_AUTO_LENGTH ((size_t)-1)

/// Calling convention: __cdecl on Windows (significant on x86).
#ifndef V8_NODE_API_CDECL
#ifdef _WIN32
#define V8_NODE_API_CDECL __cdecl
#else
#define V8_NODE_API_CDECL
#endif
#endif

/**
 * Exports only while v8jsi builds (V8_NODE_API_IMPL defined). Consumers get
 * it empty, or may predefine it as __declspec(dllimport).
 */
#ifndef V8_NODE_API_API
#ifdef V8_NODE_API_IMPL
#ifdef _WIN32
#define V8_NODE_API_API __declspec(dllexport)
#else
#define V8_NODE_API_API __attribute__((visibility("default")))
#endif
#else
#define V8_NODE_API_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v8_node_api_runtime_s *v8_node_api_runtime;
typedef struct v8_node_api_config_s *v8_node_api_config;
typedef struct v8_node_api_prepared_script_s *v8_node_api_prepared_script;
typedef struct v8_node_api_env_scope_s *v8_node_api_env_scope;

/**
 * Releases data transferred to the implementation. The callback receives its
 * closure first and the owned data second. It runs synchronously on whichever
 * thread releases the data and must not reenter an environment being torn down.
 */
typedef void(V8_NODE_API_CDECL *v8_node_api_data_delete_cb)(
    V8_NODE_API_NULLABLE void *user_data,
    V8_NODE_API_TRANSFER_FULL void *data);

/**
 * Runs one posted task on the host-selected runtime thread. It enters the
 * runtime and must not be invoked reentrantly or after delete_task.
 */
typedef void(V8_NODE_API_CDECL *v8_node_api_task_run_cb)(void *user_data);

/**
 * Posts a task to the host runner. The host owns task_data until it calls
 * delete_task exactly once, normally after run_task. The callback may be
 * invoked from any V8 thread and must not reenter the runtime synchronously.
 */
typedef void(V8_NODE_API_CDECL *v8_node_api_task_runner_post_task_cb)(
    V8_NODE_API_NULLABLE void *user_data,
    V8_NODE_API_TRANSFER_FULL void *task_data,
    V8_NODE_API_SCOPE_ASYNC V8_NODE_API_CLOSURE(task_data)
        v8_node_api_task_run_cb run_task,
    V8_NODE_API_SCOPE_ASYNC V8_NODE_API_CLOSURE(delete_data)
        v8_node_api_data_delete_cb delete_task,
    V8_NODE_API_NULLABLE void *delete_data);

/**
 * Synchronously loads a cache buffer. A returned buffer remains valid until
 * the implementation calls out_delete_buffer(out_delete_data, out_buffer).
 * It runs on the thread preparing the script and preserves the existing
 * callback's ability to reenter host code.
 */
typedef void(V8_NODE_API_CDECL *v8_node_api_script_cache_load_cb)(
    V8_NODE_API_NULLABLE void *user_data,
    V8_NODE_API_ZERO_TERMINATED const char *source_url_utf8,
    uint64_t source_hash,
    V8_NODE_API_ZERO_TERMINATED const char *runtime_name_utf8,
    uint64_t runtime_version,
    V8_NODE_API_ZERO_TERMINATED const char *cache_tag_utf8,
    V8_NODE_API_TRANSFER_FULL const uint8_t **out_buffer,
    size_t *out_buffer_size,
    V8_NODE_API_SCOPE_ASYNC V8_NODE_API_CLOSURE(out_delete_data)
        v8_node_api_data_delete_cb *out_delete_buffer,
    void **out_delete_data);

/**
 * Synchronously stores a cache buffer. The callback takes ownership of buffer
 * and must eventually invoke delete_buffer(delete_data, buffer). It runs on
 * the thread preparing the script and may reenter host code as before.
 */
typedef void(V8_NODE_API_CDECL *v8_node_api_script_cache_store_cb)(
    V8_NODE_API_NULLABLE void *user_data,
    V8_NODE_API_ZERO_TERMINATED const char *source_url_utf8,
    uint64_t source_hash,
    V8_NODE_API_ZERO_TERMINATED const char *runtime_name_utf8,
    uint64_t runtime_version,
    V8_NODE_API_ZERO_TERMINATED const char *cache_tag_utf8,
    V8_NODE_API_TRANSFER_FULL const uint8_t *buffer,
    size_t buffer_size,
    V8_NODE_API_SCOPE_ASYNC V8_NODE_API_CLOSURE(delete_data)
        v8_node_api_data_delete_cb delete_buffer,
    V8_NODE_API_NULLABLE void *delete_data);

/**
 * Synchronous visitor on the API caller's thread. data is borrowed only for
 * the call; the visitor must not reenter the same environment.
 */
typedef void(V8_NODE_API_CDECL *v8_node_api_utf8_output_cb)(
    V8_NODE_API_NULLABLE void *user_data,
    const char *data,
    size_t length);

/**
 * Synchronous visitor on the API caller's thread. key_utf8 is borrowed only
 * for the call; the visitor must not reenter the same environment.
 */
typedef void(V8_NODE_API_CDECL *v8_node_api_heap_info_cb)(
    V8_NODE_API_NULLABLE void *user_data,
    V8_NODE_API_ZERO_TERMINATED const char *key_utf8,
    int64_t value);

/**
 * Configures a factory-owned config. The config is valid only during the
 * synchronous call on the factory caller's thread. Only config setters may be
 * called; no runtime exists to reenter. Returning a failure status aborts
 * creation with that status.
 */
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_configure_runtime_cb)(
    V8_NODE_API_NULLABLE void *user_data,
    v8_node_api_config config);

/**
 * Creates a hosted V8 runtime. configure may be NULL for defaults. The first
 * failing setter poisons config and creation returns that status. out_runtime
 * is set to NULL before validation.
 */
V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_runtime_create(
    uint32_t abi_version,
    V8_NODE_API_NULLABLE V8_NODE_API_SCOPE_CALL
        v8_node_api_configure_runtime_cb configure,
    V8_NODE_API_NULLABLE void *user_data,
    v8_node_api_runtime *out_runtime);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_runtime_create_fn)(
    uint32_t abi_version,
    V8_NODE_API_NULLABLE V8_NODE_API_SCOPE_CALL
        v8_node_api_configure_runtime_cb configure,
    V8_NODE_API_NULLABLE void *user_data,
    v8_node_api_runtime *out_runtime);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_runtime_destroy(v8_node_api_runtime runtime);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_runtime_destroy_fn)(
    v8_node_api_runtime runtime);

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_runtime_get_env(
    v8_node_api_runtime runtime,
    napi_env *out_env);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_runtime_get_env_fn)(
    v8_node_api_runtime runtime,
    napi_env *out_env);

/**
 * Returns a JSI ABI runtime borrowed for the lifetime of runtime. The caller
 * must not release it.
 */
V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_runtime_get_jsi_runtime(
    v8_node_api_runtime runtime,
    jsi_runtime **out_runtime);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_runtime_get_jsi_runtime_fn)(
    v8_node_api_runtime runtime,
    jsi_runtime **out_runtime);

/* Factory-owned config setters. A config is valid only in configure. */
V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_inspector_enabled(
    v8_node_api_config config,
    bool value);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_config_set_inspector_enabled_fn)(
    v8_node_api_config config,
    bool value);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_inspector_runtime_name_utf8(
    v8_node_api_config config,
    const char *value,
    size_t length);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_config_set_inspector_runtime_name_utf8_fn)(
    v8_node_api_config config,
    const char *value,
    size_t length);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_inspector_port(
    v8_node_api_config config,
    uint16_t value);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_config_set_inspector_port_fn)(
    v8_node_api_config config,
    uint16_t value);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_inspector_break_on_start(
    v8_node_api_config config,
    bool value);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_config_set_inspector_break_on_start_fn)(
    v8_node_api_config config,
    bool value);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_gc_api_enabled(
    v8_node_api_config config,
    bool value);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_config_set_gc_api_enabled_fn)(
    v8_node_api_config config,
    bool value);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_multithreading_enabled(
    v8_node_api_config config,
    bool value);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_config_set_multithreading_enabled_fn)(
    v8_node_api_config config,
    bool value);

/**
 * Retains post_task and user_data until runtime destruction. On failure, or
 * at runtime destruction, delete_user_data(delete_data, user_data) is invoked
 * exactly once when non-NULL.
 */
V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_task_runner(
    v8_node_api_config config,
    V8_NODE_API_SCOPE_NOTIFIED V8_NODE_API_DESTROY(delete_user_data)
        v8_node_api_task_runner_post_task_cb post_task,
    V8_NODE_API_NULLABLE void *user_data,
    V8_NODE_API_NULLABLE V8_NODE_API_SCOPE_ASYNC
        V8_NODE_API_CLOSURE(delete_data)
            v8_node_api_data_delete_cb delete_user_data,
    V8_NODE_API_NULLABLE void *delete_data);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_config_set_task_runner_fn)(
    v8_node_api_config config,
    V8_NODE_API_SCOPE_NOTIFIED V8_NODE_API_DESTROY(delete_user_data)
        v8_node_api_task_runner_post_task_cb post_task,
    V8_NODE_API_NULLABLE void *user_data,
    V8_NODE_API_NULLABLE V8_NODE_API_SCOPE_ASYNC
        V8_NODE_API_CLOSURE(delete_data)
            v8_node_api_data_delete_cb delete_user_data,
    V8_NODE_API_NULLABLE void *delete_data);

/**
 * Retains the callbacks and user_data until runtime destruction. Calls may be
 * synchronous and reentrant. On failure, or at runtime destruction,
 * delete_user_data(delete_data, user_data) is invoked exactly once when
 * non-NULL.
 */
V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_script_cache(
    v8_node_api_config config,
    V8_NODE_API_SCOPE_NOTIFIED V8_NODE_API_CLOSURE(user_data)
        V8_NODE_API_DESTROY(delete_user_data)
            v8_node_api_script_cache_load_cb load,
    V8_NODE_API_SCOPE_NOTIFIED V8_NODE_API_DESTROY(delete_user_data)
        v8_node_api_script_cache_store_cb store,
    V8_NODE_API_NULLABLE void *user_data,
    V8_NODE_API_NULLABLE V8_NODE_API_SCOPE_ASYNC
        V8_NODE_API_CLOSURE(delete_data)
            v8_node_api_data_delete_cb delete_user_data,
    V8_NODE_API_NULLABLE void *delete_data);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_config_set_script_cache_fn)(
    v8_node_api_config config,
    V8_NODE_API_SCOPE_NOTIFIED V8_NODE_API_CLOSURE(user_data)
        V8_NODE_API_DESTROY(delete_user_data)
            v8_node_api_script_cache_load_cb load,
    V8_NODE_API_SCOPE_NOTIFIED V8_NODE_API_DESTROY(delete_user_data)
        v8_node_api_script_cache_store_cb store,
    V8_NODE_API_NULLABLE void *user_data,
    V8_NODE_API_NULLABLE V8_NODE_API_SCOPE_ASYNC
        V8_NODE_API_CLOSURE(delete_data)
            v8_node_api_data_delete_cb delete_user_data,
    V8_NODE_API_NULLABLE void *delete_data);

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_scope_open(
    napi_env env,
    v8_node_api_env_scope *out_scope);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_env_scope_open_fn)(
    napi_env env,
    v8_node_api_env_scope *out_scope);

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_scope_close(
    v8_node_api_env_scope scope,
    napi_env env);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_env_scope_close_fn)(
    v8_node_api_env_scope scope,
    napi_env env);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_get_description_utf8(
    napi_env env,
    const char **out_description);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_env_get_description_utf8_fn)(
    napi_env env,
    const char **out_description);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_queue_microtask(napi_env env, napi_value callback);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_env_queue_microtask_fn)(
    napi_env env,
    napi_value callback);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_drain_microtasks(
    napi_env env,
    int32_t max_count_hint,
    bool *out_drained);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_env_drain_microtasks_fn)(
    napi_env env,
    int32_t max_count_hint,
    bool *out_drained);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_is_inspectable(
    napi_env env,
    bool *out_inspectable);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_env_is_inspectable_fn)(
    napi_env env,
    bool *out_inspectable);

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_run_script_utf8(
    napi_env env,
    napi_value source,
    V8_NODE_API_NULLABLE V8_NODE_API_COUNT(source_url_length)
        const char *source_url_utf8,
    size_t source_url_length,
    napi_value *out_result);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_env_run_script_utf8_fn)(
    napi_env env,
    napi_value source,
    V8_NODE_API_NULLABLE V8_NODE_API_COUNT(source_url_length)
        const char *source_url_utf8,
    size_t source_url_length,
    napi_value *out_result);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_prepared_script_create_utf8(
    napi_env env,
    V8_NODE_API_TRANSFER_FULL const uint8_t *script_data,
    size_t script_length,
    V8_NODE_API_NULLABLE V8_NODE_API_SCOPE_ASYNC
        V8_NODE_API_CLOSURE(delete_data)
            v8_node_api_data_delete_cb delete_script,
    V8_NODE_API_NULLABLE void *delete_data,
    V8_NODE_API_COUNT(source_url_length) const char *source_url_utf8,
    size_t source_url_length,
    v8_node_api_prepared_script *out_script);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_prepared_script_create_utf8_fn)(
    napi_env env,
    V8_NODE_API_TRANSFER_FULL const uint8_t *script_data,
    size_t script_length,
    V8_NODE_API_NULLABLE V8_NODE_API_SCOPE_ASYNC
        V8_NODE_API_CLOSURE(delete_data)
            v8_node_api_data_delete_cb delete_script,
    V8_NODE_API_NULLABLE void *delete_data,
    V8_NODE_API_COUNT(source_url_length) const char *source_url_utf8,
    size_t source_url_length,
    v8_node_api_prepared_script *out_script);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_prepared_script_destroy(
    v8_node_api_prepared_script script,
    napi_env env);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_prepared_script_destroy_fn)(
    v8_node_api_prepared_script script,
    napi_env env);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_prepared_script_run(
    v8_node_api_prepared_script script,
    napi_env env,
    napi_value *out_result);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_prepared_script_run_fn)(
    v8_node_api_prepared_script script,
    napi_env env,
    napi_value *out_result);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_get_recorded_gc_stats_utf8(
    napi_env env,
    V8_NODE_API_SCOPE_CALL v8_node_api_utf8_output_cb callback,
    V8_NODE_API_NULLABLE void *user_data);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_env_get_recorded_gc_stats_utf8_fn)(
    napi_env env,
    V8_NODE_API_SCOPE_CALL v8_node_api_utf8_output_cb callback,
    V8_NODE_API_NULLABLE void *user_data);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_get_heap_info(
    napi_env env,
    bool include_expensive,
    V8_NODE_API_SCOPE_CALL v8_node_api_heap_info_cb callback,
    V8_NODE_API_NULLABLE void *user_data);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_env_get_heap_info_fn)(
    napi_env env,
    bool include_expensive,
    V8_NODE_API_SCOPE_CALL v8_node_api_heap_info_cb callback,
    V8_NODE_API_NULLABLE void *user_data);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_collect_garbage_utf8(
    napi_env env,
    V8_NODE_API_COUNT(cause_length) const char *cause_utf8,
    size_t cause_length);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_env_collect_garbage_utf8_fn)(
    napi_env env,
    V8_NODE_API_COUNT(cause_length) const char *cause_utf8,
    size_t cause_length);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_start_heap_sampling(napi_env env, size_t sampling_interval);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_env_start_heap_sampling_fn)(
    napi_env env,
    size_t sampling_interval);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_stop_heap_sampling_utf8(
    napi_env env,
    V8_NODE_API_SCOPE_CALL v8_node_api_utf8_output_cb callback,
    V8_NODE_API_NULLABLE void *user_data);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_env_stop_heap_sampling_utf8_fn)(
    napi_env env,
    V8_NODE_API_SCOPE_CALL v8_node_api_utf8_output_cb callback,
    V8_NODE_API_NULLABLE void *user_data);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_create_heap_snapshot_utf8(
    napi_env env,
    bool capture_numeric_value,
    V8_NODE_API_SCOPE_CALL v8_node_api_utf8_output_cb callback,
    V8_NODE_API_NULLABLE void *user_data);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_env_create_heap_snapshot_utf8_fn)(
    napi_env env,
    bool capture_numeric_value,
    V8_NODE_API_SCOPE_CALL v8_node_api_utf8_output_cb callback,
    V8_NODE_API_NULLABLE void *user_data);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_request_garbage_collection(napi_env env);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_env_request_garbage_collection_fn)(
    napi_env env);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_has_unhandled_promise_rejection(
    napi_env env,
    bool *out_has_rejection);
typedef napi_status(
    V8_NODE_API_CDECL *v8_node_api_env_has_unhandled_promise_rejection_fn)(
    napi_env env,
    bool *out_has_rejection);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_get_and_clear_last_unhandled_promise_rejection(
    napi_env env,
    napi_value *out_rejection);
typedef napi_status(
    V8_NODE_API_CDECL
        *v8_node_api_env_get_and_clear_last_unhandled_promise_rejection_fn)(
    napi_env env,
    napi_value *out_rejection);

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_create_module(
    napi_env root_env,
    int32_t api_version,
    napi_env *out_env);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_env_create_module_fn)(
    napi_env root_env,
    int32_t api_version,
    napi_env *out_env);

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_run_task(
    napi_env env,
    v8_node_api_task_run_cb run_task,
    V8_NODE_API_NULLABLE void *user_data);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_env_run_task_fn)(
    napi_env env,
    v8_node_api_task_run_cb run_task,
    V8_NODE_API_NULLABLE void *user_data);

/// Disposes the process-wide V8 platform.
V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_dispose_platform(void);
typedef napi_status(V8_NODE_API_CDECL *v8_node_api_dispose_platform_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* V8_NODE_API_H_ */
