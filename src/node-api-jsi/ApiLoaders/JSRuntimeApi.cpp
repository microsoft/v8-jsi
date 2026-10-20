// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "JSRuntimeApi.h"

EXTERN_C_START

// Default JSR function implementations if they are not found in the engine DLL.
extern napi_status NAPI_CDECL default_v8_node_api_env_scope_open(napi_env env, v8_node_api_env_scope *scope);
extern napi_status NAPI_CDECL default_v8_node_api_env_scope_close(v8_node_api_env_scope scope, napi_env env);
extern napi_status NAPI_CDECL default_v8_node_api_env_get_description_utf8(napi_env env, const char **result);
extern napi_status NAPI_CDECL default_v8_node_api_env_queue_microtask(napi_env env, napi_value callback);
extern napi_status NAPI_CDECL default_v8_node_api_env_drain_microtasks(napi_env env, int32_t max_count_hint, bool *result);
extern napi_status NAPI_CDECL default_v8_node_api_env_is_inspectable(napi_env env, bool *result);

extern napi_status NAPI_CDECL
default_v8_node_api_env_get_recorded_gc_stats_utf8(napi_env env, v8_node_api_utf8_output_cb cb, void *cb_ctx);
extern napi_status NAPI_CDECL
default_v8_node_api_env_get_heap_info(napi_env env, bool include_expensive, v8_node_api_heap_info_cb cb, void *cb_ctx);
extern napi_status NAPI_CDECL
default_v8_node_api_env_collect_garbage_utf8(napi_env env, const char *cause, size_t cause_length);
extern napi_status NAPI_CDECL default_v8_node_api_env_start_heap_sampling(napi_env env, size_t sampling_interval);
extern napi_status NAPI_CDECL
default_v8_node_api_env_stop_heap_sampling_utf8(napi_env env, v8_node_api_utf8_output_cb cb, void *cb_ctx);
extern napi_status NAPI_CDECL default_v8_node_api_env_create_heap_snapshot_utf8(
    napi_env env,
    bool capture_numeric_value,
    v8_node_api_utf8_output_cb cb,
    void *cb_ctx);

extern napi_status NAPI_CDECL default_v8_node_api_prepared_script_create_utf8(
    napi_env env,
    const uint8_t *script_utf8,
    size_t script_length,
    v8_node_api_data_delete_cb script_delete_cb,
    void *deleter_data,
    const char *source_url,
    size_t source_url_length,
    v8_node_api_prepared_script *result);
extern napi_status NAPI_CDECL
default_v8_node_api_prepared_script_destroy(v8_node_api_prepared_script prepared_script, napi_env env);
extern napi_status NAPI_CDECL
default_v8_node_api_prepared_script_run(v8_node_api_prepared_script prepared_script, napi_env env, napi_value *result);

EXTERN_C_END

namespace Microsoft::NodeApiJsi {

namespace {

struct JSRuntimeApiNames {
#define JSR_FUNC(func) static constexpr const char func[] = #func;
#define JSR_JSI_FUNC JSR_FUNC
#define JSR_PREPARED_SCRIPT JSR_FUNC
#include "JSRuntimeApi.inc"
};

// Prepared script functions either should be all loaded or we use all default functions.
void loadPreparedScriptFuncs() {
  JSRuntimeApi *current = JSRuntimeApi::current();
  bool useDefault = false;
#define JSR_PREPARED_SCRIPT(func)                                                         \
  func##_fn loaded_##func =                                                              \
      reinterpret_cast<func##_fn>(current->getFuncPtr(JSRuntimeApiNames::func));          \
  useDefault = useDefault || loaded_##func == nullptr;
#include "JSRuntimeApi.inc"
#define JSR_PREPARED_SCRIPT(func)                                                             \
  size_t offset_##func = offsetof(JSRuntimeApi, func);                                        \
  *reinterpret_cast<func##_fn *>(reinterpret_cast<char *>(current) + offset_##func) =         \
      useDefault ? &default_##func : loaded_##func;
#include "JSRuntimeApi.inc"
}

} // namespace

thread_local JSRuntimeApi *JSRuntimeApi::current_{};

JSRuntimeApi::JSRuntimeApi(IFuncResolver *funcResolver)
    : NodeApi(funcResolver)
#define JSR_FUNC(func)                                                                                                 \
  ,                                                                                                                    \
      func(&ApiFuncResolver<JSRuntimeApi, func##_fn, JSRuntimeApiNames::func, offsetof(JSRuntimeApi, func)>::          \
               stub)
#define JSR_JSI_FUNC(func)                                                                                             \
  ,                                                                                                                    \
      func(&ApiFuncResolver<JSRuntimeApi, func##_fn, JSRuntimeApiNames::func, offsetof(JSRuntimeApi, func)>::          \
               optionalStub<&default_##func>)
#define JSR_PREPARED_SCRIPT(func)                                                                                      \
  ,                                                                                                                    \
      func(&ApiFuncResolver<JSRuntimeApi, func##_fn, JSRuntimeApiNames::func, offsetof(JSRuntimeApi, func)>::          \
               preloadStub<&loadPreparedScriptFuncs>)
#include "JSRuntimeApi.inc"
{
}

} // namespace Microsoft::NodeApiJsi
