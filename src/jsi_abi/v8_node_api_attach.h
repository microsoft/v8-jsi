/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT license.
 *
 * v8_attach_node_api — attach a Node-API surface (napi_env) onto an existing
 * jsi_runtime that was created via v8_create_runtime. This is the seam that
 * preserves the dual-API contract now: a consumer can hold one V8
 * isolate exposed simultaneously through JSI (jsi_runtime / JsiAbiRuntime)
 * and Node-API (napi_env). Internally jsr_create_runtime is now a thin
 * wrapper that calls v8_create_runtime + v8_attach_node_api.
 *
 * Lifetime: the jsi_runtime is the primary owner. Releasing the jsi_runtime
 * (via the ABI vtable's release hook, or implicitly via JsiAbiRuntime's
 * destructor) tears down the napi_env first. The consumer should not call
 * Unref on the napi_env unless they also called Ref on it explicitly.
 *
 * This header lives separate from v8_jsi_config.h so consumers of the JSI
 * ABI config surface do not have to pull in Node-API headers.
 */

/* ===========================================================================
 * EXPERIMENTAL API - NOT YET ABI-STABLE
 *
 * This API is experimental and subject to change. Although it is C-based, it is
 * NOT ABI-safe yet: function signatures, vtable layouts, struct fields, enum
 * values, and error codes may change without notice while the API remains in
 * experimental mode. Do not depend on binary compatibility across builds. Once
 * the API leaves experimental mode it will become ABI-stable and this notice
 * will be removed.
 * =========================================================================== */

#ifndef V8_NODE_API_ATTACH_H
#define V8_NODE_API_ATTACH_H

#include "jsi_abi/jsi_abi.h"
#include "node-api/js_runtime_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build a Node-API surface (napi_env) over an existing jsi_runtime. The new
 * env shares the runtime's V8 isolate and context. */
JSI_API napi_status JSI_CDECL v8_attach_node_api(struct jsi_runtime *runtime,
                                                  int32_t api_version,
                                                  napi_env *env_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* V8_NODE_API_ATTACH_H */
