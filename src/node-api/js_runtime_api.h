// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#ifndef SRC_JS_RUNTIME_API_H_
#define SRC_JS_RUNTIME_API_H_

#include "js_native_api.h"

//
// Node-API extensions required for JavaScript engine hosting.
//
// It is a very early version of the APIs which we consider to be experimental.
// These APIs are not stable yet and are subject to change while we continue
// their development. After some time we will stabilize the APIs and make them
// "officially stable".
//

#define JSR_API NAPI_EXTERN napi_status NAPI_CDECL

EXTERN_C_START

typedef struct jsr_runtime_s* jsr_runtime;
typedef struct jsr_config_s* jsr_config;
typedef struct jsr_prepared_script_s* jsr_prepared_script;
typedef struct jsr_napi_env_scope_s* jsr_napi_env_scope;

// Heap snapshot options to control what is included when capturing a heap snapshot
typedef struct {
  bool capture_numeric_value;
} jsr_heap_snapshot_options;

// Heap statistics struct corresponding to v8::HeapStatistics
typedef struct {
  size_t total_heap_size;
  size_t total_heap_size_executable;
  size_t total_physical_size;
  size_t total_available_size;
  size_t used_heap_size;
  size_t heap_size_limit;
  size_t malloced_memory;
  size_t external_memory;
  size_t peak_malloced_memory;
  size_t number_of_native_contexts;
  size_t number_of_detached_contexts;
  size_t total_global_handles_size;
  size_t used_global_handles_size;
  bool does_zap_garbage;
} jsr_heap_statistics;

typedef void(NAPI_CDECL* jsr_data_delete_cb)(void* data, void* deleter_data);

// Generic callback for string output
typedef napi_status(NAPI_CDECL* jsr_string_output_cb)(void* ctx, const char* data, size_t len);

//=============================================================================
// jsr_runtime
//=============================================================================

JSR_API jsr_create_runtime(jsr_config config, jsr_runtime* runtime);
JSR_API jsr_delete_runtime(jsr_runtime runtime);
JSR_API jsr_runtime_get_node_api_env(jsr_runtime runtime, napi_env* env);

//=============================================================================
// Instrumentation
//=============================================================================

// Gets garbage collection statistics as a JSON-encoded string
JSR_API jsr_get_recorded_gc_stats(napi_env env, void* ctx, jsr_string_output_cb cb);

// Gets current heap information as a struct
JSR_API jsr_get_heap_info(napi_env env, bool include_expensive, jsr_heap_statistics* stats);

// Starts tracking heap object stack traces
JSR_API jsr_start_tracking_heap_object_stack_traces(napi_env env);

// Stops tracking heap object stack traces
JSR_API jsr_stop_tracking_heap_object_stack_traces(napi_env env);

// Starts heap sampling profiler
JSR_API jsr_start_heap_sampling(napi_env env, size_t sampling_interval);

// Stops heap sampling profiler and returns the result as JSON
JSR_API jsr_stop_heap_sampling(napi_env env, void* ctx, jsr_string_output_cb cb);

// Creates a heap snapshot and saves it to a file
JSR_API jsr_create_heap_snapshot_to_file(napi_env env, const char* path, const jsr_heap_snapshot_options* options);

// Writes a heap snapshot to a provided buffer as a JSON string.
// If the buffer is too small, returns napi_invalid_arg and required_length is set.
JSR_API jsr_create_heap_snapshot_to_buffer(
    napi_env env,
    char* buffer,
    size_t buffer_length,
    const jsr_heap_snapshot_options* options,
    size_t* required_length);

// Writes a heap snapshot to a callback as a JSON string.
JSR_API jsr_create_heap_snapshot_to_string(
    napi_env env,
    void* ctx,
    jsr_string_output_cb cb,
    const jsr_heap_snapshot_options* options);

// Flushes bridge traffic trace and returns it
JSR_API jsr_flush_and_disable_bridge_traffic_trace(napi_env env, void* ctx, jsr_string_output_cb cb);

// Writes basic block profile trace to a file
JSR_API jsr_write_basic_block_profile_trace(napi_env env, const char* file_name);

// Dumps profiler symbols to a file
JSR_API jsr_dump_profiler_symbols(napi_env env, const char* file_name);

//=============================================================================
// jsr_config
//=============================================================================

JSR_API jsr_create_config(jsr_config* config);
JSR_API jsr_delete_config(jsr_config config);

JSR_API jsr_config_enable_inspector(jsr_config config, bool value);
JSR_API jsr_config_set_inspector_runtime_name(jsr_config config,
                                              const char* name);
JSR_API jsr_config_set_inspector_port(jsr_config config, uint16_t port);
JSR_API jsr_config_set_inspector_break_on_start(jsr_config config, bool value);

JSR_API jsr_config_enable_gc_api(jsr_config config, bool value);

//=============================================================================
// jsr_config task runner
//=============================================================================

// A callback to run task
typedef void(NAPI_CDECL* jsr_task_run_cb)(void* task_data);

// A callback to post task to the task runner
typedef void(NAPI_CDECL* jsr_task_runner_post_task_cb)(
    void* task_runner_data,
    void* task_data,
    jsr_task_run_cb task_run_cb,
    jsr_data_delete_cb task_data_delete_cb,
    void* deleter_data);

JSR_API jsr_config_set_task_runner(
    jsr_config config,
    void* task_runner_data,
    jsr_task_runner_post_task_cb task_runner_post_task_cb,
    jsr_data_delete_cb task_runner_data_delete_cb,
    void* deleter_data);

//=============================================================================
// jsr_config script cache
//=============================================================================

typedef void(NAPI_CDECL* jsr_script_cache_load_cb)(
    void* script_cache_data,
    const char* source_url,
    uint64_t source_hash,
    const char* runtime_name,
    uint64_t runtime_version,
    const char* cache_tag,
    const uint8_t** buffer,
    size_t* buffer_size,
    jsr_data_delete_cb* buffer_delete_cb,
    void** deleter_data);

typedef void(NAPI_CDECL* jsr_script_cache_store_cb)(
    void* script_cache_data,
    const char* source_url,
    uint64_t source_hash,
    const char* runtime_name,
    uint64_t runtime_version,
    const char* cache_tag,
    const uint8_t* buffer,
    size_t buffer_size,
    jsr_data_delete_cb buffer_delete_cb,
    void* deleter_data);

JSR_API jsr_config_set_script_cache(
    jsr_config config,
    void* script_cache_data,
    jsr_script_cache_load_cb script_cache_load_cb,
    jsr_script_cache_store_cb script_cache_store_cb,
    jsr_data_delete_cb script_cache_data_delete_cb,
    void* deleter_data);

//=============================================================================
// napi_env scope
//=============================================================================

// Opens the napi_env scope in the current thread.
// Calling Node-API functions without the opened scope may cause a failure.
// The scope must be closed by the jsr_close_napi_env_scope call.
JSR_API jsr_open_napi_env_scope(napi_env env, jsr_napi_env_scope* scope);

// Closes the napi_env scope in the current thread. It must match to the
// jsr_open_napi_env_scope call.
JSR_API jsr_close_napi_env_scope(napi_env env, jsr_napi_env_scope scope);

//=============================================================================
// Additional functions to implement JSI
//=============================================================================

// To implement JSI description()
JSR_API jsr_get_description(napi_env env, const char** result);

// To implement JSI queueMicrotask()
JSR_API
jsr_queue_microtask(napi_env env, napi_value callback);

// To implement JSI drainMicrotasks()
JSR_API
jsr_drain_microtasks(napi_env env, int32_t max_count_hint, bool* result);

// To implement JSI isInspectable()
JSR_API jsr_is_inspectable(napi_env env, bool* result);

//=============================================================================
// Script preparing and running.
//
// Script is usually converted to byte code, or in other words - prepared - for
// execution. Then, we can run the prepared script.
//=============================================================================

// Run script with source URL.
JSR_API jsr_run_script(napi_env env,
                       napi_value source,
                       const char* source_url,
                       napi_value* result);

// Prepare the script for running.
JSR_API jsr_create_prepared_script(napi_env env,
                                   const uint8_t* script_data,
                                   size_t script_length,
                                   jsr_data_delete_cb script_delete_cb,
                                   void* deleter_data,
                                   const char* source_url,
                                   jsr_prepared_script* result);

// Delete the prepared script.
JSR_API jsr_delete_prepared_script(napi_env env,
                                   jsr_prepared_script prepared_script);

// Run the prepared script.
JSR_API jsr_prepared_script_run(napi_env env,
                                jsr_prepared_script prepared_script,
                                napi_value* result);

//=============================================================================
// Functions to support unit tests.
//=============================================================================

// Provides a hint to run garbage collection.
// It is typically used for unit tests.
// It requires enabling GC by calling jsr_config_enable_gc_api.
JSR_API jsr_collect_garbage(napi_env env);

// Checks if the environment has an unhandled promise rejection.
JSR_API jsr_has_unhandled_promise_rejection(napi_env env, bool* result);

// Gets and clears the last unhandled promise rejection.
JSR_API jsr_get_and_clear_last_unhandled_promise_rejection(napi_env env,
                                                           napi_value* result);

// Create new napi_env for the runtime.
JSR_API jsr_create_node_api_env(napi_env root_env,
                                int32_t api_version,
                                napi_env* env);

// Run task in the environment context.
JSR_API jsr_run_task(napi_env env, jsr_task_run_cb task_cb, void* data);

EXTERN_C_END

#endif  // !SRC_JS_RUNTIME_API_H_
