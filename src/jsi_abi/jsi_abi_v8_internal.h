// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Internal accessor functions for JsiRuntimeState. Used only by code
// compiled into v8jsi.dll (currently jsi_abi_v8.cpp + node-api/v8_api_abi.cpp).
// JsiRuntimeState's full layout is anonymous-namespaced inside
// jsi_abi_v8.cpp; these free functions expose the bits the Node-API surface
// needs without leaking the struct shape (host-object proxies, host-function
// contexts, ABI handle types, etc.) into other TUs.
//
// Do NOT include this from a public header.

#pragma once

#include "jsi_abi/jsi_abi.h"
#include "jsi_abi/v8_jsi_config.h"
#include "../v8_core.h"
#include "v8.h"

#include <memory>

namespace v8rt_internal {

/// Direct V8 isolate accessor for the runtime.
v8::Isolate *getIsolate(jsi_runtime *runtime) noexcept;

/// Direct V8 context accessor (persistent global).
v8::Global<v8::Context> &getContext(jsi_runtime *runtime) noexcept;

/// Direct V8 context accessor (handle-scoped local).
v8::Local<v8::Context> getContextLocal(jsi_runtime *runtime) noexcept;

/// Whether the runtime was constructed with multi-threaded isolate access.
/// Used to decide whether v8::Locker should be acquired around scoped work.
bool getEnableMultiThread(jsi_runtime *runtime) noexcept;

/// True if a previous unhandled promise rejection has been recorded and
/// not yet cleared.
bool hasUnhandledPromiseRejection(jsi_runtime *runtime) noexcept;

/// Take ownership of the most recent unhandled-promise-rejection record.
/// Subsequent calls return null until a new rejection is recorded.
std::unique_ptr<v8rt::UnhandledPromiseRejection>
takeLastUnhandledPromiseRejection(jsi_runtime *runtime) noexcept;

/// Snapshot of the consumer-supplied script-cache C callbacks. Held inside
/// JsiRuntimeState; Node-API's prepareScript/runPreparedScript path uses
/// these to honor the same cache the JSI side honors.
struct ScriptCacheCallbacks {
  void *data;
  v8_jsi_script_cache_load_cb load_cb;
  v8_jsi_script_cache_store_cb store_cb;
};
ScriptCacheCallbacks getScriptCacheCallbacks(jsi_runtime *runtime) noexcept;

/// Type for the attached-Node-API teardown callback. Called from
/// ~JsiRuntimeState before any V8 state is freed.
typedef void (*RuntimeAttachedDestroyCb)(void *attached);

/// Register an attached-owner cleanup hook on the runtime. Used by
/// v8_attach_node_api so ~JsiRuntimeState tears down the napi surface before
/// disposing the V8 isolate. Only one attachment is supported per runtime;
/// re-attaching while a previous attachment is live is a programming error.
void setAttachedOwner(jsi_runtime *runtime,
                      void *attached,
                      RuntimeAttachedDestroyCb destroy_cb) noexcept;

/// Clear the attached-owner hook without invoking it. Called from the
/// attached-owner's own destructor when it is being destroyed manually
/// (e.g. via RuntimeWrapper teardown ordering) so ~JsiRuntimeState does not
/// double-fire the destroy callback.
void clearAttachedOwner(jsi_runtime *runtime) noexcept;

}  // namespace v8rt_internal
