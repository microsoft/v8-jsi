// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#define NAPI_EXPERIMENTAL
#include "js_native_api.h"

//
// N-API extensions required for JavaScript engine hosting.
//
// It is a very early version of the APIs which we consider to be experimental.
// These APIs are not stable yet and are subject to change while we continue
// their development. After some time we will stabilize the APIs and make them
// "officially stable".
//

EXTERN_C_START

typedef enum {
  napi_ext_env_attribute_none = 0x00000000,
  napi_ext_env_attribute_enable_gc_api = 0x00000001,
  napi_ext_env_attribute_ignore_unhandled_promises = 0x00000002,
} napi_ext_env_attributes;

typedef struct napi_ext_env_scope__ *napi_ext_env_scope;
typedef struct napi_ext_ref__ *napi_ext_ref;

// A callback to return buffer synchronously
typedef void (*napi_ext_buffer_callback)(napi_env env, uint8_t const *buffer, size_t buffer_length, void *buffer_hint);

NAPI_EXTERN napi_status napi_ext_create_env(napi_ext_env_attributes attributes, napi_env *env);

NAPI_EXTERN napi_status napi_ext_env_ref(napi_env env);

NAPI_EXTERN napi_status napi_ext_env_unref(napi_env env);

NAPI_EXTERN napi_status napi_ext_open_env_scope(napi_env env, napi_ext_env_scope *result);

NAPI_EXTERN napi_status napi_ext_close_env_scope(napi_env env, napi_ext_env_scope scope);

NAPI_EXTERN napi_status
napi_ext_run_script(napi_env env, napi_value source, const char *source_url, napi_value *result);

NAPI_EXTERN napi_status napi_ext_run_serialized_script(
    napi_env env,
    uint8_t const *buffer,
    size_t buffer_length,
    napi_value source,
    char const *source_url,
    napi_value *result);

NAPI_EXTERN napi_status napi_ext_serialize_script(
    napi_env env,
    napi_value source,
    char const *source_url,
    napi_ext_buffer_callback buffer_cb,
    void *buffer_hint);

NAPI_EXTERN napi_status napi_ext_collect_garbage(napi_env env);

NAPI_EXTERN napi_status napi_ext_has_unhandled_promise_rejection(napi_env env, bool *result);

NAPI_EXTERN napi_status napi_get_and_clear_last_unhandled_promise_rejection(napi_env env, napi_value *result);

// Use to enable fast string equality check by comparing napi_refs as addresses.
// The caller is responsible for calling napi_reference_unref on the result
// after the use. The caller must not call the napi_delete_reference.
NAPI_EXTERN napi_status
napi_ext_get_unique_string_utf8_ref(napi_env env, const char *str, size_t length, napi_ext_ref *result);

NAPI_EXTERN napi_status
napi_ext_get_unique_string_ref(napi_env env, napi_value str_value, napi_ext_ref *result);

// Methods to control object lifespan.
// The NAPI's napi_ref can be used only for objects.
// The napi_ext_ref can be used for any value type.

// Creates new napi_ext_ref with ref counter set to 1.
NAPI_EXTERN napi_status napi_ext_create_reference(napi_env env, napi_value value, napi_ext_ref *result);

// Creates new napi_ext_ref and associates native data with the reference.
// The ref counter is set to 1.
NAPI_EXTERN napi_status napi_ext_create_reference_with_data(
    napi_env env,
    napi_value value,
    void *native_object,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_ext_ref *result);

// Creates new napi_ext_ref with ref counter set to 1.
// The napi_ext_ref wraps up a weak reference to value.
NAPI_EXTERN napi_status napi_ext_create_weak_reference(napi_env env, napi_value value, napi_ext_ref *result);

// Increments the reference count.
NAPI_EXTERN napi_status napi_ext_reference_ref(napi_env env, napi_ext_ref ref);

// Decrements the reference count.
// The provided ref must not be used after this call because it could be deleted
// if the internal ref counter became zero.
NAPI_EXTERN napi_status napi_ext_reference_unref(napi_env env, napi_ext_ref ref);

// Gets the referenced value.
NAPI_EXTERN napi_status napi_ext_get_reference_value(napi_env env, napi_ext_ref ref, napi_value *result);

EXTERN_C_END
