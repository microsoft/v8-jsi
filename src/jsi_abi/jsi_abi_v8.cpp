/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT license.
 *
 * Portions derived from facebook/hermes (Hermes ABI):
 *   Copyright (c) Meta Platforms, Inc. and affiliates.
 *   Licensed under the MIT license.
 */

/// \file jsi_abi_v8.cpp
/// \brief Implementation of the JSI ABI v2 interface for V8.
///
/// This file implements the jsi_abi.h v2 interface using V8 directly via
/// v8_core.h. It follows the Hermes ABI patterns: jsi_runtime inheritance,
/// jsi_pointer-based handles, status returns with out parameters, and separate
/// JS/native error storage.
///
/// Error Handling Design
/// =====================
/// Two error types following the Hermes ABI pattern:
/// - jsi_status_js_error: JavaScript exception (value stored in pendingJSError)
/// - jsi_status_native_error: Native exception (message in nativeExceptionMessage)
///
/// The TryCatch class inherits from v8::TryCatch and automatically captures
/// JavaScript exceptions in its destructor.
///
/// This file is designed to work without C++ exceptions.

#include "jsi_abi/jsi_abi.h"
#include "jsi_abi/jsi_abi_helpers.h"
#include "jsi_abi/v8_jsi_abi.h"
#include "jsi_abi/v8_snapshot_container.h"
#include "jsi_abi/jsi_abi_v8_internal.h"
#include "../v8_core.h"
#include "../MurmurHash.h"
#include "v8-profiler.h"

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
#include "../inspector/inspector_agent.h"
#endif

#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <list>
#include <optional>
#include <sstream>
#include <ostream>
#include <fstream>

namespace abi = jsi::abi;

//==============================================================================
// V8 jsi_config (opaque type definition)
//==============================================================================
//
// jsi_config is forward-declared in jsi_abi.h as `typedef struct jsi_config_s*
// jsi_config`. The struct definition lives here in the V8 implementation —
// only setter functions cross the DLL boundary.
//
// Mirrors v8runtime::V8RuntimeArgs field-for-field so a future revision
// applyArgs() helper can simply call the setters one-to-one.
struct jsi_config_s {
  // Heap / threading
  size_t initial_heap_size_in_bytes{0};
  size_t maximum_heap_size_in_bytes{0};
  uint8_t thread_pool_size{0};

  // Inspector / debugger settings applied by JsiRuntimeState::create.
  std::string debugger_runtime_name;
  uint16_t inspector_port{9223};
  bool enable_inspector{false};
  bool inspector_break_on_start{false};

  // Microtasks / promises
  bool explicit_microtask_policy{false};
  bool ignore_unhandled_promises{false};

  // GC
  bool enable_gc_api{false};

  // Concurrency (stored for future revision; currently inert)
  bool enable_multi_thread{false};

  // Tracing (stored for future revision; currently inert except for system instr)
  bool enable_jit_tracing{false};
  bool enable_message_tracing{false};
  bool enable_gc_tracing{false};
  bool enable_system_instrumentation{false};

  // Note: process-global V8 engine flags (sparkplug, predictable,
  // optimize_for_size, always_compact, jitless, lite_mode) are NOT per-runtime
  // config — they are set via the process-level v8_jsi_abi_set_v8_flags before the
  // first runtime. See v8_jsi_abi.h.

  // Script cache — null callbacks mean "no cache".
  // The config takes ownership of script_cache_data when the setter is called.
  // On v8_jsi_abi_runtime_create the runtime takes over (we clear these pointers so
  // ~jsi_config_s does not double-delete). If the config is destroyed before
  // any runtime claims the data, ~jsi_config_s runs the deleter once.
  void *script_cache_data{nullptr};
  v8_jsi_abi_script_cache_load_cb script_cache_load_cb{nullptr};
  v8_jsi_abi_script_cache_store_cb script_cache_store_cb{nullptr};
  jsi_data_delete_cb script_cache_data_delete_cb{nullptr};
  void *script_cache_delete_data{nullptr};

  // Task runner — null callbacks mean "no task runner".
  // Same lifetime contract as the script-cache fields above: config takes
  // ownership at setter time; on v8_jsi_abi_runtime_create the runtime takes over
  // (these pointers are cleared); the deleter runs exactly once via the
  // internal adapter's destructor (held inside IsolateData by shared_ptr).
  // If the config is destroyed before any runtime claims the data,
  // ~jsi_config_s runs the deleter once.
  void *task_runner_data{nullptr};
  v8_jsi_abi_task_runner_post_task_cb task_runner_post_task_cb{nullptr};
  jsi_data_delete_cb task_runner_data_delete_cb{nullptr};
  void *task_runner_delete_data{nullptr};

  // Startup-snapshot blob — null means none. Same lifetime contract as the
  // fields above: config owns it at setter time; on v8_jsi_abi_runtime_create the
  // runtime takes over (these are cleared); ~jsi_config_s fires the deleter only
  // if no runtime claimed it.
  const uint8_t *startup_snapshot_blob{nullptr};
  size_t startup_snapshot_blob_size{0};
  jsi_data_delete_cb startup_snapshot_delete_cb{nullptr};
  void *startup_snapshot_delete_data{nullptr};

  // First setter failure; sticky. v8_jsi_abi_runtime_create fails with it.
  jsi_status status{jsi_status_ok};

  ~jsi_config_s() {
    if (script_cache_data_delete_cb) {
      script_cache_data_delete_cb(script_cache_delete_data, script_cache_data);
    }
    if (task_runner_data_delete_cb) {
      task_runner_data_delete_cb(task_runner_delete_data, task_runner_data);
    }
    if (startup_snapshot_delete_cb) {
      startup_snapshot_delete_cb(
          startup_snapshot_delete_data,
          const_cast<uint8_t *>(startup_snapshot_blob));
    }
  }
};

namespace {

static size_t resolveUtf8Length(const char *value, size_t length) {
  return length == JSI_ABI_AUTO_LENGTH ? (value ? std::strlen(value) : 0)
                                       : length;
}

// The ABI owns a jsi_buffer passed to it, including on early failure.
static jsi_status releaseBufferAndFail(jsi_buffer *buffer, jsi_status status) {
  if (buffer && buffer->api && buffer->api->release)
    buffer->api->release(buffer);
  return status;
}

static size_t resolveUtf16Length(const jsi_char16 *value, size_t length) {
  return length == JSI_ABI_AUTO_LENGTH
      ? (value ? std::char_traits<jsi_char16>::length(value) : 0)
      : length;
}

//==============================================================================
// Forward Declarations
//==============================================================================

struct JsiRuntimeState;
struct AbiHostObjectProxy;
struct HostFunctionContext;

//==============================================================================
// TaskRunner adapter (C-callbacks → v8rt::TaskRunner)
//==============================================================================
//
// Internal-only adapter held inside v8rt::IsolateData via shared_ptr. The
// engine-internal v8rt::TaskRunner interface is what V8's inspector and other
// platform plumbing speak; this adapter forwards postTask() calls into the
// consumer-supplied C callback pair.
//
// The adapter owns the consumer's user_data and runs its deleter exactly once
// in its destructor — the single point where the consumer's runner is
// released.
class CTaskRunner final : public v8rt::TaskRunner {
 public:
  CTaskRunner(v8_jsi_abi_task_runner_post_task_cb post_task_cb,
              void *user_data,
              jsi_data_delete_cb user_data_delete_cb,
              void *user_data_delete_data) noexcept
      : post_task_cb_(post_task_cb),
        user_data_(user_data),
        user_data_delete_cb_(user_data_delete_cb),
        user_data_delete_data_(user_data_delete_data) {}

  ~CTaskRunner() override {
    if (user_data_delete_cb_) {
      user_data_delete_cb_(user_data_delete_data_, user_data_);
    }
  }

  CTaskRunner(const CTaskRunner &) = delete;
  CTaskRunner &operator=(const CTaskRunner &) = delete;

  void postTask(std::unique_ptr<Task> task) override {
    post_task_cb_(
        user_data_,
        static_cast<void *>(task.release()),
        [](void *task_data) {
          static_cast<Task *>(task_data)->run();
        },
        [](void * /*delete_data*/, void *task_data) {
          delete static_cast<Task *>(task_data);
        },
        /*task_data_delete_data:*/ nullptr);
  }

 private:
  v8_jsi_abi_task_runner_post_task_cb post_task_cb_;
  void *user_data_;
  jsi_data_delete_cb user_data_delete_cb_;
  void *user_data_delete_data_;
};

//==============================================================================
// Handle Wrapper Types (inherit from jsi_pointer)
//==============================================================================

/// Base wrapper for ABI handles that wrap V8 persistent handles.
/// Inherits from jsi_pointer so the wrapper IS a jsi_pointer with an
/// invalidate callback in its api.
template <typename V8Type>
struct HandleWrapper : public jsi_pointer {
  static void JSI_ABI_CDECL invalidate(jsi_pointer *self) {
    delete static_cast<HandleWrapper *>(self);
  }
  static constexpr jsi_pointer_api pointerVt{invalidate};

  v8::Persistent<V8Type> persistent;

  HandleWrapper(v8::Isolate *iso, v8::Local<V8Type> local)
      : jsi_pointer{&pointerVt} {
    persistent.Reset(iso, local);
  }

  ~HandleWrapper() { persistent.Reset(); }

  v8::Local<V8Type> get(v8::Isolate *isolate) const {
    return persistent.Get(isolate);
  }
};

using ObjectHandle = HandleWrapper<v8::Object>;
using StringHandle = HandleWrapper<v8::String>;
using SymbolHandle = HandleWrapper<v8::Symbol>;
using BigIntHandle = HandleWrapper<v8::BigInt>;

/// PreparedJavaScript implementation. Inherits from jsi_prepared_javascript
/// (not jsi_pointer) — prepared scripts have their own lifecycle and
/// api in the ABI.
struct PreparedScriptImpl : public jsi_prepared_javascript {
  static void JSI_ABI_CDECL release(jsi_prepared_javascript *self) {
    delete static_cast<PreparedScriptImpl *>(self);
  }
  static constexpr jsi_prepared_javascript_api preparedVt{release};

  v8::Persistent<v8::UnboundScript> persistent;

  PreparedScriptImpl(v8::Isolate *iso, v8::Local<v8::UnboundScript> local)
      : jsi_prepared_javascript{&preparedVt} {
    persistent.Reset(iso, local);
  }

  ~PreparedScriptImpl() { persistent.Reset(); }

  v8::Local<v8::UnboundScript> get(v8::Isolate *isolate) const {
    return persistent.Get(isolate);
  }
};

/// Wrapper for property name IDs (can be string or symbol).
struct PropNameIdHandle : public jsi_pointer {
  static void JSI_ABI_CDECL invalidate(jsi_pointer *self) {
    delete static_cast<PropNameIdHandle *>(self);
  }
  static constexpr jsi_pointer_api pointerVt{invalidate};

  v8::Persistent<v8::Value> persistent; // String or Symbol

  PropNameIdHandle(v8::Isolate *iso, v8::Local<v8::Value> local)
      : jsi_pointer{&pointerVt} {
    persistent.Reset(iso, local);
  }

  ~PropNameIdHandle() { persistent.Reset(); }

  v8::Local<v8::Value> get(v8::Isolate *isolate) const {
    return persistent.Get(isolate);
  }
};

/// Wrapper for weak object references.
struct WeakObjectHandle : public jsi_pointer {
  static void JSI_ABI_CDECL invalidate(jsi_pointer *self) {
    delete static_cast<WeakObjectHandle *>(self);
  }
  static constexpr jsi_pointer_api pointerVt{invalidate};

  v8::Persistent<v8::Object> persistent;

  WeakObjectHandle(v8::Isolate *iso, v8::Local<v8::Object> local)
      : jsi_pointer{&pointerVt} {
    persistent.Reset(iso, local);
    persistent.SetWeak();
  }

  ~WeakObjectHandle() { persistent.Reset(); }

  v8::Local<v8::Object> get(v8::Isolate *isolate) const {
    return persistent.Get(isolate);
  }
};

//==============================================================================
// Cast Helpers
//==============================================================================

inline ObjectHandle *toObjectHandle(jsi_object obj) {
  return static_cast<ObjectHandle *>(obj.pointer);
}
inline StringHandle *toStringHandle(jsi_string str) {
  return static_cast<StringHandle *>(str.pointer);
}
inline SymbolHandle *toSymbolHandle(jsi_symbol sym) {
  return static_cast<SymbolHandle *>(sym.pointer);
}
inline BigIntHandle *toBigIntHandle(jsi_bigint bi) {
  return static_cast<BigIntHandle *>(bi.pointer);
}
inline PropNameIdHandle *toPropNameIdHandle(jsi_propnameid name) {
  return static_cast<PropNameIdHandle *>(name.pointer);
}
inline WeakObjectHandle *toWeakObjectHandle(jsi_weak_object weak) {
  return static_cast<WeakObjectHandle *>(weak.pointer);
}

//==============================================================================
// Growable Buffer Helper
//==============================================================================

/// Write data into a growable buffer.
void writeToBuf(jsi_growable_buffer *buf, const char *data, size_t len) {
  if (buf->size < len)
    buf->api->try_grow_to(buf, len);
  if (buf->size >= len) {
    std::memcpy(buf->data, data, len);
    buf->used = len;
  }
}

void writeToBuf(jsi_growable_buffer *buf, const std::string &str) {
  writeToBuf(buf, str.data(), str.size());
}

//==============================================================================
// V8 Value <-> jsi_value Conversion
//==============================================================================

/// Convert a V8 local value to a jsi_value, allocating handles for pointer
/// types.
jsi_value createJsiValue(v8::Isolate *isolate, v8::Local<v8::Value> val) {
  if (val->IsUndefined())
    return abi::create_undefined_value();
  if (val->IsNull())
    return abi::create_null_value();
  if (val->IsBoolean())
    return abi::create_bool_value(val->BooleanValue(isolate));
  if (val->IsNumber())
    return abi::create_number_value(
        val->NumberValue(isolate->GetCurrentContext()).FromJust());
  if (val->IsString())
    return abi::create_string_value(
        new StringHandle(isolate, v8::Local<v8::String>::Cast(val)));
  if (val->IsSymbol())
    return abi::create_symbol_value(
        new SymbolHandle(isolate, v8::Local<v8::Symbol>::Cast(val)));
  if (val->IsBigInt())
    return abi::create_bigint_value(
        new BigIntHandle(isolate, v8::Local<v8::BigInt>::Cast(val)));
  if (val->IsObject())
    return abi::create_object_value(
        new ObjectHandle(isolate, v8::Local<v8::Object>::Cast(val)));
  return abi::create_undefined_value();
}

/// Convert a jsi_value to a V8 local value.
v8::Local<v8::Value> toV8Value(v8::Isolate *isolate, const jsi_value *val) {
  switch (val->kind) {
  case jsi_valuekind_undefined:
    return v8::Undefined(isolate);
  case jsi_valuekind_null:
    return v8::Null(isolate);
  case jsi_valuekind_boolean:
    return v8::Boolean::New(isolate, val->data.boolean);
  case jsi_valuekind_number:
    return v8::Number::New(isolate, val->data.number);
  case jsi_valuekind_string:
    return static_cast<StringHandle *>(val->data.pointer)->get(isolate);
  case jsi_valuekind_symbol:
    return static_cast<SymbolHandle *>(val->data.pointer)->get(isolate);
  case jsi_valuekind_bigint:
    return static_cast<BigIntHandle *>(val->data.pointer)->get(isolate);
  case jsi_valuekind_object:
    return static_cast<ObjectHandle *>(val->data.pointer)->get(isolate);
  default:
    return v8::Undefined(isolate);
  }
}

//==============================================================================
// Internal Runtime State (extends jsi_runtime)
//==============================================================================

/// Wrapper for native state stored via ABI.
struct NativeStateWrapper {
  jsi_native_state *state;
  v8::Global<v8::Object> weak_ref;

  NativeStateWrapper(v8::Isolate *isolate, v8::Local<v8::Object> obj,
                     jsi_native_state *s)
      : state(s) {
    weak_ref.Reset(isolate, obj);
    weak_ref.SetWeak(this, weak_callback, v8::WeakCallbackType::kParameter);
  }

  ~NativeStateWrapper() {
    if (state) {
      state->api->release(state);
    }
    weak_ref.Reset();
  }

  static void
  weak_callback(const v8::WeakCallbackInfo<NativeStateWrapper> &info) {
    delete info.GetParameter();
  }
};

/// Internal runtime state that manages V8 isolate and context directly.
/// Inherits from jsi_runtime so the pointer IS a jsi_runtime*.
struct JsiRuntimeState : public jsi_runtime {
  // Reference count. Starts at 1 in create(); incremented by jsi_add_ref;
  // decremented by jsi_release, which destroys the runtime at zero.
  std::atomic<uint32_t> refcount{1};

  // ABI version negotiated in v8_jsi_abi_runtime_create (the consumer's compiled
  // JSI_ABI_VERSION). Reserved for future behavioral pinning; get_abi_version
  // reports the implementation's max, not this value.
  uint32_t abi_version{JSI_ABI_VERSION};

  v8::Isolate *isolate;
  v8::Global<v8::Context> context;
  v8rt::IsolateData *isolateData;

  // True if the consumer requested v8::Locker-protected isolate access.
  // Read by V8Scope to construct an optional Locker before entering scope.
  bool enableMultiThread{false};

  // Script cache — copied from the config in create(). Runtime owns
  // script_cache_data after handoff: the deleter runs in ~JsiRuntimeState.
  void *script_cache_data{nullptr};
  v8_jsi_abi_script_cache_load_cb script_cache_load_cb{nullptr};
  v8_jsi_abi_script_cache_store_cb script_cache_store_cb{nullptr};
  jsi_data_delete_cb script_cache_data_delete_cb{nullptr};
  void *script_cache_delete_data{nullptr};

  // Startup-snapshot blob — copied from the config in create(). The runtime
  // owns the bytes after handoff: V8 references them for the isolate's whole
  // lifetime, so the deleter runs in ~JsiRuntimeState AFTER isolate disposal.
  const uint8_t *startup_snapshot_blob{nullptr};
  jsi_data_delete_cb startup_snapshot_delete_cb{nullptr};
  void *startup_snapshot_delete_data{nullptr};

  // Error state (v2 pattern)
  jsi_value pendingJSError{}; // JS exception value (inline tagged union)
  std::string nativeExceptionMessage;

  // Host object infrastructure
  v8::Persistent<v8::Function> hostObjectConstructor;
  bool hostObjectConstructorInitialized{false};
  std::list<AbiHostObjectProxy *> hostObjectProxies;

  // Host function infrastructure
  v8::Persistent<v8::Private> hostFunctionKey;
  std::list<HostFunctionContext *> hostFunctionContexts;

  // unhandled-promise tracking. Migrated from the legacy V8Runtime so the
  // Node-API path (v8_node_api_env_has_unhandled_promise_rejection /
  // v8_node_api_env_get_and_clear_last_unhandled_promise_rejection) keeps
  // working.
  // The PromiseRejectCallback finds the runtime via context embedder slot 0.
  bool ignore_unhandled_promises{false};
  std::unique_ptr<v8rt::UnhandledPromiseRejection> last_unhandled_promise;

  // optional attached-owner hook. v8_attach_node_api stores the
  // V8RuntimeEnv pointer and a destroy callback here so the Node-API surface
  // is torn down before V8 state is disposed in ~JsiRuntimeState.
  void *attached_owner{nullptr};
  v8rt_internal::RuntimeAttachedDestroyCb attached_owner_destroy{nullptr};

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
  // per-runtime inspector agent. nullptr when enable_inspector is false.
  // Held by shared_ptr so multiple JsiRuntimeStates on the same isolate
  // share one Agent (matches the legacy V8Runtime behavior; see slot 1).
  std::shared_ptr<inspector::Agent> inspector_agent;
#endif

  /// Create a new runtime state with its own V8 isolate and context.
  /// \param config Optional jsi_config (NULL preserves legacy defaults:
  ///   --expose_gc, MicrotasksPolicy::kExplicit, enable_gc_api=true).
  static JsiRuntimeState *create(const jsi_config_s *config);

  ~JsiRuntimeState();

  /// Look up the JsiRuntimeState owning a context. Reads context embedder
  /// slot 0 (set in create()).
  static JsiRuntimeState *fromContext(v8::Local<v8::Context> context) noexcept {
    if (context.IsEmpty()) return nullptr;
    if (context->GetNumberOfEmbedderDataFields() <=
        v8rt::ContextEmbedderIndex::kContextTag) {
      return nullptr;
    }
    void *tag = context->GetAlignedPointerFromEmbedderData(
        v8rt::ContextEmbedderIndex::kContextTag);
    if (tag != v8rt::RuntimeContextTagPtr) return nullptr;
    return static_cast<JsiRuntimeState *>(
        context->GetAlignedPointerFromEmbedderData(
            v8rt::ContextEmbedderIndex::kRuntime));
  }

  /// V8 PromiseRejectCallback. Finds the runtime via the context embedder
  /// slot and records the most recent unhandled rejection on it. Adapted
  /// from V8 d8 / legacy V8Runtime.
  static void PromiseRejectCallback(v8::PromiseRejectMessage data);

  void SetUnhandledPromise(v8::Local<v8::Promise> promise,
                            v8::Local<v8::Message> message,
                            v8::Local<v8::Value> exception);
  void RemoveUnhandledPromise(v8::Local<v8::Promise> promise);

  v8::Local<v8::Private> getNativeStateKey() {
    return isolateData->nativeStateKey();
  }

  v8::Local<v8::Private> getHostObjectKey() {
    return isolateData->hostObjectKey();
  }

  v8::Local<v8::Private> getHostFunctionKey() {
    return hostFunctionKey.Get(isolate);
  }

  v8::Local<v8::Context> getContextLocal() const {
    return context.Get(isolate);
  }

  v8::Local<v8::Function> getHostObjectConstructor();

  void setNativeError(const char *message) {
    nativeExceptionMessage = message ? message : "";
  }
  void setNativeError(std::string message) {
    nativeExceptionMessage = std::move(message);
  }
};

inline JsiRuntimeState *getState(jsi_runtime *rt) {
  return static_cast<JsiRuntimeState *>(rt);
}

//==============================================================================
// TryCatch with Auto-Capture
//==============================================================================

class TryCatch : public v8::TryCatch {
public:
  explicit TryCatch(JsiRuntimeState *state)
      : v8::TryCatch(state->isolate), state_(state) {}

  ~TryCatch() {
    if (HasCaught()) {
      v8::Local<v8::Value> exception = Exception();
      state_->pendingJSError = createJsiValue(state_->isolate, exception);
    }
  }

  TryCatch(const TryCatch &) = delete;
  TryCatch &operator=(const TryCatch &) = delete;

private:
  JsiRuntimeState *state_;
};

//==============================================================================
// ABI Host Object Proxy
//==============================================================================

struct AbiHostObjectProxy {
  jsi_runtime *runtime;
  jsi_host_object *hostObject;
  v8::Global<v8::Object> weakRef;
  std::list<AbiHostObjectProxy *>::iterator listIter;
  bool registered{false};

  AbiHostObjectProxy(jsi_runtime *rt, jsi_host_object *ho,
                     v8::Isolate *isolate, v8::Local<v8::Object> obj)
      : runtime(rt), hostObject(ho) {
    weakRef.Reset(isolate, obj);
    weakRef.SetWeak(this, weak_callback, v8::WeakCallbackType::kParameter);

    auto *state = getState(runtime);
    state->hostObjectProxies.push_back(this);
    listIter = std::prev(state->hostObjectProxies.end());
    registered = true;
  }

  ~AbiHostObjectProxy() {
    if (hostObject) {
      hostObject->api->release(hostObject);
    }
    weakRef.Reset();
  }

  void unregister() {
    if (registered) {
      auto *state = getState(runtime);
      state->hostObjectProxies.erase(listIter);
      registered = false;
    }
  }

  static void
  weak_callback(const v8::WeakCallbackInfo<AbiHostObjectProxy> &info) {
    AbiHostObjectProxy *proxy = info.GetParameter();
    proxy->unregister();
    delete proxy;
  }

  // V8 property interceptor callbacks

  static v8::Intercepted
  Get(v8::Local<v8::Name> v8PropName,
      const v8::PropertyCallbackInfo<v8::Value> &info) {
    AbiHostObjectProxy *proxy = GetProxy(info);
    if (!proxy || !proxy->hostObject)
      return v8::Intercepted::kNo;

    v8::Isolate *isolate = info.GetIsolate();

    jsi_propnameid propNameId{new PropNameIdHandle(isolate, v8PropName)};

    jsi_value result = abi::create_undefined_value();
    jsi_status status = proxy->hostObject->api->get(
        proxy->hostObject, proxy->runtime, propNameId, &result);

    propNameId.pointer->api->invalidate(propNameId.pointer);

    if (status != jsi_status_ok) {
      auto *state = getState(proxy->runtime);
      if (status == jsi_status_js_error) {
        jsi_value jsErr = state->pendingJSError;
        if (abi::is_pointer_value(jsErr)) {
          isolate->ThrowException(toV8Value(isolate, &jsErr));
        }
        state->pendingJSError = abi::create_undefined_value();
      } else {
        v8::Local<v8::String> message =
            v8::String::NewFromUtf8(isolate,
                                    state->nativeExceptionMessage.c_str())
                .ToLocalChecked();
        isolate->ThrowException(v8::Exception::Error(message));
        state->nativeExceptionMessage.clear();
      }
      return v8::Intercepted::kYes;
    }

    info.GetReturnValue().Set(toV8Value(isolate, &result));
    abi::release_value(result);
    return v8::Intercepted::kYes;
  }

  static v8::Intercepted
  Set(v8::Local<v8::Name> v8PropName, v8::Local<v8::Value> value,
      const v8::PropertyCallbackInfo<void> &info) {
    AbiHostObjectProxy *proxy = GetProxy(info);
    if (!proxy || !proxy->hostObject)
      return v8::Intercepted::kNo;

    v8::Isolate *isolate = info.GetIsolate();

    jsi_propnameid propNameId{new PropNameIdHandle(isolate, v8PropName)};
    jsi_value jsiValue = createJsiValue(isolate, value);

    jsi_status result = proxy->hostObject->api->set(
        proxy->hostObject, proxy->runtime, propNameId, &jsiValue);

    propNameId.pointer->api->invalidate(propNameId.pointer);
    abi::release_value(jsiValue);

    if (result != jsi_status_ok) {
      auto *state = getState(proxy->runtime);
      if (result == jsi_status_js_error &&
          abi::is_pointer_value(state->pendingJSError)) {
        isolate->ThrowException(toV8Value(isolate, &state->pendingJSError));
        state->pendingJSError = abi::create_undefined_value();
      } else {
        v8::Local<v8::String> message =
            v8::String::NewFromUtf8(isolate,
                                    state->nativeExceptionMessage.c_str())
                .ToLocalChecked();
        isolate->ThrowException(v8::Exception::Error(message));
        state->nativeExceptionMessage.clear();
      }
    }

    return v8::Intercepted::kYes;
  }

  static v8::Intercepted
  GetIndexed(uint32_t index,
             const v8::PropertyCallbackInfo<v8::Value> &info) {
    v8::Isolate *isolate = info.GetIsolate();
    v8::Local<v8::String> indexStr =
        v8::String::NewFromUtf8(isolate, std::to_string(index).c_str())
            .ToLocalChecked();
    return Get(indexStr, info);
  }

  static v8::Intercepted
  SetIndexed(uint32_t index, v8::Local<v8::Value> value,
             const v8::PropertyCallbackInfo<void> &info) {
    v8::Isolate *isolate = info.GetIsolate();
    v8::Local<v8::String> indexStr =
        v8::String::NewFromUtf8(isolate, std::to_string(index).c_str())
            .ToLocalChecked();
    return Set(indexStr, value, info);
  }

  static void Enumerator(const v8::PropertyCallbackInfo<v8::Array> &info) {
    AbiHostObjectProxy *proxy =
        GetProxyFromThis(info.This(), info.GetIsolate());
    if (!proxy || !proxy->hostObject) {
      info.GetReturnValue().Set(v8::Array::New(info.GetIsolate()));
      return;
    }

    v8::Isolate *isolate = info.GetIsolate();
    v8::Local<v8::Context> context = isolate->GetCurrentContext();

    jsi_propnameid_list *list = nullptr;
    jsi_status status = proxy->hostObject->api->get_own_keys(
        proxy->hostObject, proxy->runtime, &list);

    if (status != jsi_status_ok) {
      info.GetReturnValue().Set(v8::Array::New(isolate));
      return;
    }

    v8::Local<v8::Array> result =
        v8::Array::New(isolate, static_cast<int>(list->size));
    for (size_t i = 0; i < list->size; i++) {
      v8::Local<v8::Value> propValue =
          toPropNameIdHandle(list->props[i])->get(isolate);
      result->Set(context, static_cast<uint32_t>(i), propValue)
          .FromMaybe(false);
    }
    list->api->release(list);

    info.GetReturnValue().Set(result);
  }

private:
  template <typename T>
  static AbiHostObjectProxy *
  GetProxy(const v8::PropertyCallbackInfo<T> &info) {
    return GetProxyFromThis(info.This(), info.GetIsolate());
  }

  static AbiHostObjectProxy *GetProxyFromThis(v8::Local<v8::Object> obj,
                                               v8::Isolate * /*isolate*/) {
    while (obj->InternalFieldCount() != 1) {
      v8::Local<v8::Value> proto = obj->GetPrototypeV2();
      if (proto.IsEmpty() || !proto->IsObject())
        return nullptr;
      obj = v8::Local<v8::Object>::Cast(proto);
    }

    v8::Local<v8::Value> externalValue =
        obj->GetInternalField(0).As<v8::Value>();
    if (externalValue.IsEmpty() || !externalValue->IsExternal())
      return nullptr;

    v8::Local<v8::External> data = v8::Local<v8::External>::Cast(externalValue);
    return reinterpret_cast<AbiHostObjectProxy *>(data->Value());
  }
};

//==============================================================================
// JsiRuntimeState Implementation
//==============================================================================

JsiRuntimeState *JsiRuntimeState::create(const jsi_config_s *config) {
  // Legacy default behavior (config==nullptr): match what the ABI runtime
  // shipped earlier — --expose_gc, kExplicit microtasks, enable_gc_api=true.
  // This preserves the existing test corpus (117/117) since JsiAbiRuntime's
  // C++ constructor passes config=nullptr by default.
  const bool useDefaults = (config == nullptr);

  // Build the V8 platform-init flag string. V8 only consumes these flags on
  // first platform init in the process; subsequent runtimes inherit them
  // (V8 design constraint, not ours).
  std::string flags;
  auto appendFlag = [&flags](const char *flag) {
    if (!flags.empty())
      flags += ' ';
    flags += flag;
  };

  if (useDefaults) {
    appendFlag("--expose_gc");
  } else {
    if (config->enable_gc_api)
      appendFlag("--expose_gc");
    if (config->enable_system_instrumentation)
      appendFlag("--enable-system-instrumentation");
  }

  const int thread_pool_size = useDefaults ? 0 : config->thread_pool_size;

  // Capture flags by value into the lambda so the string survives to first
  // init even if the caller frees the config immediately after.
  v8rt::V8PlatformHolder::initializePlatform(thread_pool_size, [flags]() {
    if (!flags.empty()) {
      v8::V8::SetFlagsFromString(flags.c_str());
    }
  });

  v8rt::IsolateConfig isolateConfig;
  if (useDefaults) {
    isolateConfig.microtasks_policy = v8::MicrotasksPolicy::kExplicit;
    isolateConfig.enable_gc_api = true;
  } else {
    isolateConfig.initial_heap_size = config->initial_heap_size_in_bytes;
    isolateConfig.maximum_heap_size = config->maximum_heap_size_in_bytes;
    isolateConfig.microtasks_policy = config->explicit_microtask_policy
        ? v8::MicrotasksPolicy::kExplicit
        : v8::MicrotasksPolicy::kAuto;
    isolateConfig.enable_gc_api = config->enable_gc_api;

    // Borrow the startup-snapshot blob for createIsolate (the config still owns
    // the bytes here; ownership is transferred to the runtime state after the
    // isolate is built, below). createIsolate references the bytes during
    // Isolate::Initialize, so they must be alive now — and they are.
    isolateConfig.startup_snapshot_blob = config->startup_snapshot_blob;
    isolateConfig.startup_snapshot_blob_size = config->startup_snapshot_blob_size;

    // take ownership of the task runner from the config. The shared_ptr
    // is handed to IsolateData by createIsolate; once IsolateData is freed in
    // ~JsiRuntimeState, the shared_ptr drops and the CTaskRunner destructor
    // runs the consumer's deleter exactly once. We clear the config fields
    // so ~jsi_config_s does not double-fire the deleter.
    if (config->task_runner_post_task_cb) {
      auto *mutableConfig = const_cast<jsi_config_s *>(config);
      isolateConfig.task_runner = std::make_shared<CTaskRunner>(
          mutableConfig->task_runner_post_task_cb,
          mutableConfig->task_runner_data,
          mutableConfig->task_runner_data_delete_cb,
          mutableConfig->task_runner_delete_data);
      mutableConfig->task_runner_data = nullptr;
      mutableConfig->task_runner_post_task_cb = nullptr;
      mutableConfig->task_runner_data_delete_cb = nullptr;
      mutableConfig->task_runner_delete_data = nullptr;
    }
  }

  v8::Isolate *isolate = v8rt::createIsolate(isolateConfig);
  if (!isolate)
    return nullptr;

  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  const bool fromSnapshot =
      isolateConfig.startup_snapshot_blob != nullptr &&
      isolateConfig.startup_snapshot_blob_size > 0;
  v8::Local<v8::Context> context = v8rt::createContext(isolate, fromSnapshot);

  // Forward-declare the api (defined below)
  extern const jsi_runtime_api g_vtable;

  auto *state = new JsiRuntimeState();
  state->api = &g_vtable;
  state->isolate = isolate;
  state->context.Reset(isolate, context);
  state->isolateData = v8rt::IsolateData::fromIsolate(isolate);
  state->enableMultiThread = !useDefaults && config->enable_multi_thread;
  state->ignore_unhandled_promises =
      !useDefaults && config->ignore_unhandled_promises;

  // stamp the context with a back-pointer to this runtime + the v8jsi
  // marker tag. The PromiseRejectCallback (and any future static V8 callback
  // that only has a context handle) walks slot 0 to recover the runtime.
  context->SetAlignedPointerInEmbedderData(
      v8rt::ContextEmbedderIndex::kRuntime, state);
  context->SetAlignedPointerInEmbedderData(
      v8rt::ContextEmbedderIndex::kContextTag, v8rt::RuntimeContextTagPtr);

  // install the unhandled-promise-rejection tracker unless the consumer
  // explicitly opted out. Mirrors legacy V8Runtime behavior — Node-API's
  // v8_node_api_env_has_unhandled_promise_rejection depends on this.
  if (!state->ignore_unhandled_promises) {
    isolate->SetPromiseRejectCallback(JsiRuntimeState::PromiseRejectCallback);
  }

  // take ownership of the script cache from the config. After this
  // handoff the config no longer owns script_cache_data, so freeing the
  // config does not invoke the deleter — only ~JsiRuntimeState does.
  if (!useDefaults) {
    auto *mutableConfig = const_cast<jsi_config_s *>(config);
    state->script_cache_data = mutableConfig->script_cache_data;
    state->script_cache_load_cb = mutableConfig->script_cache_load_cb;
    state->script_cache_store_cb = mutableConfig->script_cache_store_cb;
    state->script_cache_data_delete_cb =
        mutableConfig->script_cache_data_delete_cb;
    state->script_cache_delete_data = mutableConfig->script_cache_delete_data;
    mutableConfig->script_cache_data = nullptr;
    mutableConfig->script_cache_load_cb = nullptr;
    mutableConfig->script_cache_store_cb = nullptr;
    mutableConfig->script_cache_data_delete_cb = nullptr;
    mutableConfig->script_cache_delete_data = nullptr;

    // take ownership of the startup-snapshot blob from the config. The runtime
    // now keeps the bytes alive for the isolate's lifetime and fires the
    // deleter once in ~JsiRuntimeState (after isolate disposal). Clearing the
    // config fields prevents ~jsi_config_s from double-freeing.
    state->startup_snapshot_blob = mutableConfig->startup_snapshot_blob;
    state->startup_snapshot_delete_cb = mutableConfig->startup_snapshot_delete_cb;
    state->startup_snapshot_delete_data =
        mutableConfig->startup_snapshot_delete_data;
    mutableConfig->startup_snapshot_blob = nullptr;
    mutableConfig->startup_snapshot_blob_size = 0;
    mutableConfig->startup_snapshot_delete_cb = nullptr;
    mutableConfig->startup_snapshot_delete_data = nullptr;
  }

  state->pendingJSError = abi::create_undefined_value();
  state->hostFunctionKey.Reset(
      isolate,
      v8::Private::New(
          isolate,
          v8::String::NewFromUtf8Literal(isolate, "v8:jsi:hostFunction")));

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
  if (!useDefaults && config->enable_inspector) {
    void *existing = isolate->GetData(v8rt::kIsolateInspectorSlot);
    std::shared_ptr<inspector::Agent> agent;
    if (existing) {
      agent = reinterpret_cast<inspector::Agent *>(existing)->getShared();
    } else {
      agent = std::make_shared<inspector::Agent>(isolate, config->inspector_port);
      isolate->SetData(v8rt::kIsolateInspectorSlot, agent.get());
    }
    const char *name = config->debugger_runtime_name.empty()
        ? "JSIRuntime context"
        : config->debugger_runtime_name.c_str();
    agent->addContext(context, name);
    agent->start();
    if (config->inspector_break_on_start) {
      agent->waitForDebugger();
    }
    state->inspector_agent = std::move(agent);
  }
#endif

  return state;
}

// ~JsiRuntimeState defined after HostFunctionContext and AbiHostObjectProxy

v8::Local<v8::Function> JsiRuntimeState::getHostObjectConstructor() {
  if (!hostObjectConstructorInitialized) {
    v8::Local<v8::FunctionTemplate> constructorTemplate =
        v8::FunctionTemplate::New(isolate);
    v8::Local<v8::ObjectTemplate> instanceTemplate =
        constructorTemplate->InstanceTemplate();

    instanceTemplate->SetHandler(v8::NamedPropertyHandlerConfiguration(
        AbiHostObjectProxy::Get, AbiHostObjectProxy::Set, nullptr, nullptr,
        AbiHostObjectProxy::Enumerator));
    instanceTemplate->SetHandler(v8::IndexedPropertyHandlerConfiguration(
        AbiHostObjectProxy::GetIndexed, AbiHostObjectProxy::SetIndexed));

    instanceTemplate->SetInternalFieldCount(1);

    v8::Local<v8::Context> ctx = getContextLocal();
    v8::Local<v8::Function> constructor =
        constructorTemplate->GetFunction(ctx).ToLocalChecked();
    hostObjectConstructor.Reset(isolate, constructor);
    hostObjectConstructorInitialized = true;
  }
  return hostObjectConstructor.Get(isolate);
}

//==============================================================================
// RAII Helper for V8 Scopes
//==============================================================================

struct V8Scope {
  // Order matters: Locker must be acquired before any other scope when the
  // isolate is shared across threads.
  std::optional<v8::Locker> locker;
  v8::Isolate::Scope isolate_scope;
  v8::HandleScope handle_scope;
  v8::Context::Scope context_scope;

  V8Scope(JsiRuntimeState *state)
      : locker(makeLocker(state)), isolate_scope(state->isolate),
        handle_scope(state->isolate),
        context_scope(state->getContextLocal()) {}

 private:
  static std::optional<v8::Locker> makeLocker(JsiRuntimeState *state) {
    if (state->enableMultiThread)
      return std::make_optional<v8::Locker>(state->isolate);
    return std::nullopt;
  }
};

//==============================================================================
// Host Function Wrapper
//==============================================================================

struct HostFunctionContext {
  jsi_runtime *runtime;
  jsi_host_function *hostFunction;
  v8::Global<v8::Function> weakRef;
  std::list<HostFunctionContext *>::iterator listIter;
  bool registered{false};

  HostFunctionContext(jsi_runtime *rt, jsi_host_function *hf)
      : runtime(rt), hostFunction(hf) {}

  /// Set up weak ref tracking after the V8 function has been created.
  void trackFunction(v8::Isolate *isolate, v8::Local<v8::Function> func) {
    weakRef.Reset(isolate, func);
    weakRef.SetWeak(this, weak_callback, v8::WeakCallbackType::kParameter);

    auto *state = getState(runtime);
    state->hostFunctionContexts.push_back(this);
    listIter = std::prev(state->hostFunctionContexts.end());
    registered = true;
  }

  ~HostFunctionContext() {
    if (hostFunction) {
      hostFunction->api->release(hostFunction);
    }
    weakRef.Reset();
  }

  void unregister() {
    if (registered) {
      auto *state = getState(runtime);
      state->hostFunctionContexts.erase(listIter);
      registered = false;
    }
  }

  static void
  weak_callback(const v8::WeakCallbackInfo<HostFunctionContext> &info) {
    HostFunctionContext *ctx = info.GetParameter();
    ctx->unregister();
    delete ctx;
  }
};

// Deferred definition — requires HostFunctionContext and AbiHostObjectProxy
// to be complete types.
JsiRuntimeState::~JsiRuntimeState() {
  // tear down the attached Node-API surface (if any) before disposing
  // V8 state. The destroy callback owns deleting the attached object.
  if (attached_owner_destroy) {
    auto cb = attached_owner_destroy;
    void *attached = attached_owner;
    attached_owner_destroy = nullptr;
    attached_owner = nullptr;
    cb(attached);
  }

  // Drop the unhandled-promise record (if any) before the isolate goes away;
  // its v8::Globals must release while the isolate is still live.
  last_unhandled_promise.reset();

  for (HostFunctionContext *ctx : hostFunctionContexts) {
    ctx->registered = false;
    delete ctx;
  }
  hostFunctionContexts.clear();

  for (AbiHostObjectProxy *proxy : hostObjectProxies) {
    proxy->registered = false;
    delete proxy;
  }
  hostObjectProxies.clear();

  abi::release_value(pendingJSError);

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
  // Tear down the inspector before the context is reset — the Agent's
  // removeContext takes a v8::Local<v8::Context>.
  if (inspector_agent) {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    inspector_agent->removeContext(getContextLocal());
    inspector_agent.reset();
  }
#endif

  hostObjectConstructor.Reset();
  hostFunctionKey.Reset();
  context.Reset();

  if (isolate) {
    delete isolateData;
    isolate->Dispose();
  }

  // release the consumer-supplied script cache after the isolate is gone
  // (no more cache calls can be in flight). Runs in the consumer's CRT
  // because the consumer also supplied the deleter callback.
  if (script_cache_data_delete_cb) {
    script_cache_data_delete_cb(script_cache_delete_data, script_cache_data);
  }

  // release the startup-snapshot blob only after isolate disposal — V8 keeps a
  // pointer to these bytes for the whole isolate lifetime.
  if (startup_snapshot_delete_cb) {
    startup_snapshot_delete_cb(
        startup_snapshot_delete_data,
        const_cast<uint8_t *>(startup_snapshot_blob));
  }
}

// Adapted from V8 d8 utility code (was V8Runtime::PromiseRejectCallback).
void JsiRuntimeState::PromiseRejectCallback(v8::PromiseRejectMessage data) {
  if (data.GetEvent() == v8::kPromiseRejectAfterResolved ||
      data.GetEvent() == v8::kPromiseResolveAfterResolved) {
    return;
  }
  v8::Local<v8::Promise> promise = data.GetPromise();
  v8::Isolate *isolate = v8::Isolate::GetCurrent();
  v8::MaybeLocal<v8::Context> maybeContext = promise->GetCreationContext(isolate);
  if (maybeContext.IsEmpty()) return;
  JsiRuntimeState *runtime = JsiRuntimeState::fromContext(
      maybeContext.ToLocalChecked());
  if (!runtime) return;

  if (data.GetEvent() == v8::kPromiseHandlerAddedAfterReject) {
    runtime->RemoveUnhandledPromise(promise);
    return;
  }

  isolate->SetCaptureStackTraceForUncaughtExceptions(true);
  v8::Local<v8::Value> exception = data.GetValue();
  v8::Local<v8::Message> message;
  if (exception->IsObject()) {
    message = v8::Exception::CreateMessage(isolate, exception);
  }
  if (!exception->IsNativeError() &&
      (message.IsEmpty() || message->GetStackTrace().IsEmpty())) {
    v8::TryCatch try_catch(isolate);
    try_catch.SetVerbose(true);
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(isolate, "Unhandled Promise.")));
    message = try_catch.Message();
    exception = try_catch.Exception();
  }

  runtime->SetUnhandledPromise(promise, message, exception);
}

void JsiRuntimeState::SetUnhandledPromise(v8::Local<v8::Promise> promise,
                                           v8::Local<v8::Message> message,
                                           v8::Local<v8::Value> exception) {
  if (ignore_unhandled_promises) return;
  last_unhandled_promise =
      std::make_unique<v8rt::UnhandledPromiseRejection>(
          v8rt::UnhandledPromiseRejection{
              v8::Global<v8::Promise>(isolate, promise),
              v8::Global<v8::Message>(isolate, message),
              v8::Global<v8::Value>(isolate, exception)});
}

void JsiRuntimeState::RemoveUnhandledPromise(v8::Local<v8::Promise> promise) {
  if (ignore_unhandled_promises) return;
  if (last_unhandled_promise &&
      last_unhandled_promise->promise.Get(isolate) == promise) {
    last_unhandled_promise.reset();
  }
}

void JSI_ABI_CDECL HostFunctionCallbackTrampoline(
    const v8::FunctionCallbackInfo<v8::Value> &info) {
  v8::Isolate *isolate = info.GetIsolate();
  v8::HandleScope handle_scope(isolate);

  v8::Local<v8::External> data = v8::Local<v8::External>::Cast(info.Data());
  auto *ctx = static_cast<HostFunctionContext *>(data->Value());

  std::vector<jsi_value> args;
  args.reserve(info.Length());
  for (int i = 0; i < info.Length(); ++i)
    args.push_back(createJsiValue(isolate, info[i]));

  jsi_value thisVal = createJsiValue(isolate, info.This());

  jsi_value result = abi::create_undefined_value();
  jsi_status status = ctx->hostFunction->api->call(
      ctx->hostFunction, ctx->runtime, &thisVal, args.data(), args.size(),
      &result);

  for (auto &arg : args)
    abi::release_value(arg);
  abi::release_value(thisVal);

  if (status != jsi_status_ok) {
    auto *state = getState(ctx->runtime);
    if (status == jsi_status_js_error &&
        abi::is_pointer_value(state->pendingJSError)) {
      isolate->ThrowException(toV8Value(isolate, &state->pendingJSError));
      abi::release_value(state->pendingJSError);
      state->pendingJSError = abi::create_undefined_value();
    } else {
      std::string errMessage = "Exception in HostFunction: " +
                                state->nativeExceptionMessage;
      v8::Local<v8::String> message =
          v8::String::NewFromUtf8(isolate, errMessage.c_str())
              .ToLocalChecked();
      isolate->ThrowException(v8::Exception::Error(message));
      state->nativeExceptionMessage.clear();
    }
    return;
  }

  info.GetReturnValue().Set(toV8Value(isolate, &result));
  abi::release_value(result);
}

//==============================================================================
// Helper to get NativeStateWrapper from an object
//==============================================================================

NativeStateWrapper *getNativeStateWrapper(JsiRuntimeState *state,
                                          v8::Local<v8::Context> context,
                                          v8::Local<v8::Object> obj) {
  v8::Local<v8::Private> key = state->getNativeStateKey();
  v8::MaybeLocal<v8::Value> maybeVal = obj->GetPrivate(context, key);
  if (maybeVal.IsEmpty())
    return nullptr;
  v8::Local<v8::Value> val = maybeVal.ToLocalChecked();
  if (!val->IsExternal())
    return nullptr;
  return static_cast<NativeStateWrapper *>(val.As<v8::External>()->Value());
}

//==============================================================================
// VTable Function Implementations
//==============================================================================

//------------------------------------------------------------------------------
// QueryInterface
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_query_interface(
    jsi_runtime *rt, const jsi_interface_id *iid, const void **out_api,
    void **out_instance) {
  if (!out_api || !out_instance)
    return jsi_status_invalid_arg;
  *out_api = nullptr;
  *out_instance = nullptr;
  (void)iid;
  // No optional interfaces are currently supported. query_interface is
  // retained as the COM-style extension seam for future engine-specific
  // interfaces; callers must handle the not-supported result.
  getState(rt)->setNativeError("Interface not supported");
  return jsi_status_native_error;
}

//------------------------------------------------------------------------------
// ABI version
//------------------------------------------------------------------------------

// Report the implementation's max supported ABI version (Node-API
// napi_get_version analog). The create-time negotiation in v8_jsi_abi_runtime_create is
// what actually gates; this is a feature-probe.
uint32_t JSI_ABI_CDECL jsi_get_abi_version(jsi_runtime *rt) {
  (void)rt;
  return JSI_ABI_VERSION;
}

//------------------------------------------------------------------------------
// Lifecycle
//------------------------------------------------------------------------------

void JSI_ABI_CDECL jsi_add_ref(jsi_runtime *rt) {
  getState(rt)->refcount.fetch_add(1, std::memory_order_relaxed);
}

void JSI_ABI_CDECL jsi_release(jsi_runtime *rt) {
  auto *state = getState(rt);
  // acq_rel ensures the destructor sees all writes from threads that held
  // a ref; relaxed isn't enough across thread boundaries.
  if (state->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete state;
  }
}

//------------------------------------------------------------------------------
// Error Handling
//------------------------------------------------------------------------------

jsi_value JSI_ABI_CDECL jsi_get_and_clear_js_error_value(jsi_runtime *rt) {
  auto *state = getState(rt);
  jsi_value result = state->pendingJSError;
  state->pendingJSError = abi::create_undefined_value();
  return result;
}

void JSI_ABI_CDECL jsi_get_and_clear_native_exception_message(
    jsi_runtime *rt, jsi_growable_buffer *buffer) {
  auto *state = getState(rt);
  writeToBuf(buffer, state->nativeExceptionMessage);
  state->nativeExceptionMessage.clear();
  state->nativeExceptionMessage.shrink_to_fit();
}

void JSI_ABI_CDECL jsi_set_js_error_value(jsi_runtime *rt,
                                       const jsi_value *error_value) {
  auto *state = getState(rt);
  V8Scope scope(state);
  abi::release_value(state->pendingJSError);
  if (abi::is_pointer_value(*error_value)) {
    v8::Local<v8::Value> v8val = toV8Value(state->isolate, error_value);
    state->pendingJSError = createJsiValue(state->isolate, v8val);
  } else {
    state->pendingJSError = *error_value;
  }
}

void JSI_ABI_CDECL jsi_set_native_exception_message_utf8(
    jsi_runtime *rt, const char *value, size_t length) {
  length = resolveUtf8Length(value, length);
  if (!value)
    length = 0;
  getState(rt)->nativeExceptionMessage.assign(value ? value : "", length);
}

//------------------------------------------------------------------------------
// Clone Operations
//------------------------------------------------------------------------------

jsi_propnameid JSI_ABI_CDECL jsi_clone_propnameid(jsi_runtime *rt,
                                               jsi_propnameid name) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Value> v8val = toPropNameIdHandle(name)->get(state->isolate);
  return {new PropNameIdHandle(state->isolate, v8val)};
}

jsi_string JSI_ABI_CDECL jsi_clone_string(jsi_runtime *rt, jsi_string str) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::String> v8str = toStringHandle(str)->get(state->isolate);
  return {new StringHandle(state->isolate, v8str)};
}

jsi_symbol JSI_ABI_CDECL jsi_clone_symbol(jsi_runtime *rt, jsi_symbol sym) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Symbol> v8sym = toSymbolHandle(sym)->get(state->isolate);
  return {new SymbolHandle(state->isolate, v8sym)};
}

jsi_object JSI_ABI_CDECL jsi_clone_object(jsi_runtime *rt, jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  return {new ObjectHandle(state->isolate, v8obj)};
}

jsi_bigint JSI_ABI_CDECL jsi_clone_bigint(jsi_runtime *rt, jsi_bigint bigint) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::BigInt> v8bi = toBigIntHandle(bigint)->get(state->isolate);
  return {new BigIntHandle(state->isolate, v8bi)};
}

//------------------------------------------------------------------------------
// Description & Capabilities
//------------------------------------------------------------------------------

void JSI_ABI_CDECL jsi_get_description(jsi_runtime *rt,
                                    jsi_growable_buffer *buffer) {
  (void)rt;
  const char *v8_version = v8::V8::GetVersion();
  std::string desc = std::string("V8Runtime [") + v8_version + "]";
  writeToBuf(buffer, desc);
}

bool JSI_ABI_CDECL jsi_is_inspectable(jsi_runtime *rt) {
  (void)rt;
  return false;
}

//------------------------------------------------------------------------------
// Handle Scopes
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_push_scope(jsi_runtime *rt,
                                            void **out_scope) {
  if (!out_scope)
    return jsi_status_invalid_arg;
  *out_scope = nullptr;
  (void)rt;
  *out_scope = reinterpret_cast<void *>(1);
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_pop_scope(jsi_runtime *rt, void *scope) {
  (void)rt;
  (void)scope;
  return jsi_status_ok;
}

//------------------------------------------------------------------------------
// Script Evaluation
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_evaluate_javascript_source(
    jsi_runtime *rt, jsi_buffer *buffer, const char *source_url,
    size_t source_url_length, jsi_value *out_value) {
  if (!out_value)
    return releaseBufferAndFail(buffer, jsi_status_invalid_arg);
  *out_value = abi::create_undefined_value();
  source_url_length = resolveUtf8Length(source_url, source_url_length);
  if (!source_url && source_url_length != 0)
    return releaseBufferAndFail(buffer, jsi_status_invalid_arg);
  source_url = source_url ? source_url : "";
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;
  TryCatch try_catch(state);

  v8::Local<v8::String> sourceStr;
  if (!v8::String::NewFromUtf8(isolate,
                                reinterpret_cast<const char *>(buffer->data),
                                v8::NewStringType::kNormal,
                                static_cast<int>(buffer->size))
           .ToLocal(&sourceStr)) {
    if (buffer->api && buffer->api->release)
      buffer->api->release(buffer);
    state->setNativeError("Failed to create source string");
    return jsi_status_native_error;
  }

  v8::Local<v8::String> urlV8Str;
  if (!v8::String::NewFromUtf8(isolate, source_url,
                                v8::NewStringType::kNormal,
                                static_cast<int>(source_url_length))
           .ToLocal(&urlV8Str)) {
    if (buffer->api && buffer->api->release)
      buffer->api->release(buffer);
    state->setNativeError("Failed to create source URL string");
    return jsi_status_native_error;
  }

  if (buffer->api && buffer->api->release)
    buffer->api->release(buffer);

  v8::ScriptOrigin origin(urlV8Str);

  v8::Local<v8::Script> compiled;
  if (!v8::Script::Compile(state->getContextLocal(), sourceStr, &origin)
           .ToLocal(&compiled))
    return jsi_status_js_error;

  v8::Local<v8::Value> resultValue;
  if (!compiled->Run(state->getContextLocal()).ToLocal(&resultValue))
    return jsi_status_js_error;

  *out_value = createJsiValue(isolate, resultValue);
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_prepare_javascript(
    jsi_runtime *rt, jsi_buffer *buffer, const char *source_url,
    size_t source_url_length, jsi_prepared_javascript **out_prepared) {
  if (!out_prepared)
    return releaseBufferAndFail(buffer, jsi_status_invalid_arg);
  *out_prepared = nullptr;
  source_url_length = resolveUtf8Length(source_url, source_url_length);
  if (!source_url && source_url_length != 0)
    return releaseBufferAndFail(buffer, jsi_status_invalid_arg);
  source_url = source_url ? source_url : "";
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;
  TryCatch try_catch(state);

  // compute MurmurHash3 over the source bytes for the cache key. Must
  // happen before buffer->release() since the buffer pointer becomes invalid.
  uint64_t source_hash = 0;
  if (state->script_cache_load_cb || state->script_cache_store_cb) {
    murmurhash(buffer->data, buffer->size, source_hash);
  }

  v8::Local<v8::String> sourceStr;
  if (!v8::String::NewFromUtf8(isolate,
                                reinterpret_cast<const char *>(buffer->data),
                                v8::NewStringType::kNormal,
                                static_cast<int>(buffer->size))
           .ToLocal(&sourceStr)) {
    if (buffer->api && buffer->api->release)
      buffer->api->release(buffer);
    state->setNativeError("Failed to create source string");
    return jsi_status_native_error;
  }

  v8::Local<v8::String> urlV8Str;
  if (!v8::String::NewFromUtf8(isolate, source_url,
                                v8::NewStringType::kNormal,
                                static_cast<int>(source_url_length))
           .ToLocal(&urlV8Str)) {
    if (buffer->api && buffer->api->release)
      buffer->api->release(buffer);
    state->setNativeError("Failed to create source URL string");
    return jsi_status_native_error;
  }

  if (buffer->api && buffer->api->release)
    buffer->api->release(buffer);

  v8::ScriptOrigin origin(urlV8Str);

  // cache_tag = "perf" matches the legacy V8Runtime convention.
  // runtime_name = "V8" likewise — preserves cache keys for deployed
  // consumers whose stores already contain entries under that name.
  static constexpr const char *kCacheTag = "perf";
  static constexpr const char *kRuntimeName = "V8";
  const uint64_t runtime_version = v8::ScriptCompiler::CachedDataVersionTag();
  // The callbacks get a NUL-terminated copy; the caller's URL may not be.
  std::string cache_source_url;
  if (state->script_cache_load_cb || state->script_cache_store_cb)
    cache_source_url.assign(source_url, source_url_length);

  // try cache load.
  v8::ScriptCompiler::CompileOptions options =
      v8::ScriptCompiler::CompileOptions::kNoCompileOptions;
  v8::ScriptCompiler::CachedData *cached_data = nullptr;
  const uint8_t *cache_buffer = nullptr;
  size_t cache_buffer_size = 0;
  jsi_data_delete_cb cache_buffer_delete_cb = nullptr;
  void *cache_buffer_delete_data = nullptr;

  if (state->script_cache_load_cb) {
    state->script_cache_load_cb(
        state->script_cache_data,
        cache_source_url.c_str(),
        source_hash,
        kRuntimeName,
        runtime_version,
        kCacheTag,
        &cache_buffer,
        &cache_buffer_size,
        &cache_buffer_delete_cb,
        &cache_buffer_delete_data);
    if (cache_buffer && cache_buffer_size > 0) {
      // BufferNotOwned: V8 does not free `cache_buffer`; we own it and
      // release via cache_buffer_delete_cb after Compile returns.
      cached_data = new v8::ScriptCompiler::CachedData(
          cache_buffer, static_cast<int>(cache_buffer_size));
      options = v8::ScriptCompiler::CompileOptions::kConsumeCodeCache;
    }
  }

  v8::ScriptCompiler::Source script_source(sourceStr, origin, cached_data);

  v8::Local<v8::Script> compiled;
  bool compile_ok =
      v8::ScriptCompiler::Compile(state->getContextLocal(), &script_source,
                                  options)
          .ToLocal(&compiled);

  // Release the consumer's cache buffer (if any) now that V8 is done with it.
  // Compile is synchronous, so the bytes are no longer needed past this point.
  if (cache_buffer_delete_cb) {
    cache_buffer_delete_cb(cache_buffer_delete_data,
                           const_cast<uint8_t *>(cache_buffer));
  }

  if (!compile_ok)
    return jsi_status_js_error;

  v8::Local<v8::UnboundScript> unbound = compiled->GetUnboundScript();

  // cache miss → produce code cache and hand it to the consumer.
  if (state->script_cache_store_cb &&
      options != v8::ScriptCompiler::CompileOptions::kConsumeCodeCache) {
    v8::ScriptCompiler::CachedData *codeCache =
        v8::ScriptCompiler::CreateCodeCache(unbound);
    if (codeCache) {
      // The consumer reads codeCache->data and then calls our delete_cb,
      // which deletes the V8 CachedData object (and its owned buffer) in the
      // DLL's CRT — the same CRT that allocated it.
      auto delete_cb = [](void *delete_data, void * /*data*/) {
        delete static_cast<v8::ScriptCompiler::CachedData *>(delete_data);
      };
      state->script_cache_store_cb(
          state->script_cache_data,
          cache_source_url.c_str(),
          source_hash,
          kRuntimeName,
          runtime_version,
          kCacheTag,
          codeCache->data,
          static_cast<size_t>(codeCache->length),
          delete_cb,
          codeCache);
    }
  }

  *out_prepared = new PreparedScriptImpl(isolate, unbound);
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_evaluate_prepared_javascript(
    jsi_runtime *rt, jsi_prepared_javascript *prepared,
    jsi_value *out_value) {
  if (!out_value)
    return jsi_status_invalid_arg;
  *out_value = abi::create_undefined_value();
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;
  TryCatch try_catch(state);

  auto *impl = static_cast<PreparedScriptImpl *>(prepared);
  v8::Local<v8::UnboundScript> unbound = impl->get(state->isolate);
  v8::Local<v8::Script> script = unbound->BindToCurrentContext();

  v8::Local<v8::Value> resultValue;
  if (!script->Run(state->getContextLocal()).ToLocal(&resultValue))
    return jsi_status_js_error;
  *out_value = createJsiValue(isolate, resultValue);
  return jsi_status_ok;
}

//------------------------------------------------------------------------------
// Microtasks
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_drain_microtasks(
    jsi_runtime *rt, int32_t max_hint, bool *out_drained) {
  if (!out_drained)
    return jsi_status_invalid_arg;
  *out_drained = false;
  (void)max_hint;
  auto *state = getState(rt);
  V8Scope scope(state);

  if (state->isolate->GetMicrotasksPolicy() == v8::MicrotasksPolicy::kExplicit)
    state->isolate->PerformMicrotaskCheckpoint();

  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_queue_microtask(jsi_runtime *rt,
                                                  jsi_function callback) {
  auto *state = getState(rt);
  V8Scope scope(state);

  v8::Local<v8::Object> obj = toObjectHandle({callback.pointer})->get(state->isolate);
  if (!obj->IsFunction()) {
    state->setNativeError("callback must be a function");
    return jsi_status_native_error;
  }

  state->isolate->EnqueueMicrotask(v8::Local<v8::Function>::Cast(obj));
  return jsi_status_ok;
}

//------------------------------------------------------------------------------
// Global Object
//------------------------------------------------------------------------------

jsi_object JSI_ABI_CDECL jsi_get_global_object(jsi_runtime *rt) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Object> global = state->getContextLocal()->Global();
  return {new ObjectHandle(state->isolate, global)};
}

//------------------------------------------------------------------------------
// String Operations
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_create_string_from_utf8(
    jsi_runtime *rt, const char *value, size_t length,
    jsi_string *out_string) {
  if (!out_string)
    return jsi_status_invalid_arg;
  *out_string = {};
  length = resolveUtf8Length(value, length);
  if (!value && length != 0)
    return jsi_status_invalid_arg;
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::String> v8str;
  if (!v8::String::NewFromUtf8(state->isolate,
                                value ? value : "",
                                v8::NewStringType::kNormal,
                                static_cast<int>(length))
           .ToLocal(&v8str)) {
    state->setNativeError("Failed to create string");
    return jsi_status_native_error;
  }
  *out_string = abi::create_string(
      static_cast<jsi_pointer *>(new StringHandle(state->isolate, v8str)));
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_create_string_from_utf16(
    jsi_runtime *rt, const jsi_char16 *value, size_t length,
    jsi_string *out_string) {
  if (!out_string)
    return jsi_status_invalid_arg;
  *out_string = {};
  length = resolveUtf16Length(value, length);
  if (!value && length != 0)
    return jsi_status_invalid_arg;
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::String> v8str;
  if (!v8::String::NewFromTwoByte(
           state->isolate,
           reinterpret_cast<const uint16_t *>(value ? value : u""),
           v8::NewStringType::kNormal, static_cast<int>(length))
           .ToLocal(&v8str)) {
    state->setNativeError("Failed to create string");
    return jsi_status_native_error;
  }
  *out_string = abi::create_string(
      static_cast<jsi_pointer *>(new StringHandle(state->isolate, v8str)));
  return jsi_status_ok;
}

void JSI_ABI_CDECL jsi_get_utf8_from_string(jsi_runtime *rt, jsi_string str,
                                         jsi_growable_buffer *buffer) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;
  v8::Local<v8::String> v8str = toStringHandle(str)->get(state->isolate);

  size_t utf8_len = v8str->Utf8LengthV2(isolate);
  if (buffer->size < utf8_len)
    buffer->api->try_grow_to(buffer, utf8_len);
  if (buffer->size >= utf8_len) {
    v8str->WriteUtf8V2(isolate, reinterpret_cast<char *>(buffer->data),
                        static_cast<int>(utf8_len));
    buffer->used = utf8_len;
  }
}

void JSI_ABI_CDECL jsi_get_utf16_from_string(jsi_runtime *rt, jsi_string str,
                                          jsi_growable_buffer *buffer) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;
  v8::Local<v8::String> v8str = toStringHandle(str)->get(state->isolate);

  int len = v8str->Length();
  size_t bytes = static_cast<size_t>(len) * sizeof(uint16_t);
  if (buffer->size < bytes)
    buffer->api->try_grow_to(buffer, bytes);
  if (buffer->size >= bytes) {
    v8str->WriteV2(isolate, 0, len,
                    reinterpret_cast<uint16_t *>(buffer->data));
    buffer->used = bytes;
  }
}

//------------------------------------------------------------------------------
// Symbol Operations
//------------------------------------------------------------------------------

void JSI_ABI_CDECL jsi_get_utf8_from_symbol(jsi_runtime *rt, jsi_symbol sym,
                                         jsi_growable_buffer *buffer) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;

  v8::Local<v8::Symbol> v8sym = toSymbolHandle(sym)->get(state->isolate);
  v8::Local<v8::Value> descVal = v8sym->Description(isolate);

  std::string result_str = "Symbol(";
  if (descVal->IsString()) {
    v8::Local<v8::String> descStr = v8::Local<v8::String>::Cast(descVal);
    size_t len = descStr->Utf8LengthV2(isolate);
    std::string desc(len, '\0');
    descStr->WriteUtf8V2(isolate, &desc[0], static_cast<int>(len));
    result_str += desc;
  }
  result_str += ")";
  writeToBuf(buffer, result_str);
}

//------------------------------------------------------------------------------
// PropNameID Operations
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_create_propnameid_from_utf8(
    jsi_runtime *rt, const char *value, size_t length,
    jsi_propnameid *out_propnameid) {
  if (!out_propnameid)
    return jsi_status_invalid_arg;
  *out_propnameid = {};
  length = resolveUtf8Length(value, length);
  if (!value && length != 0)
    return jsi_status_invalid_arg;
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::String> v8str;
  if (!v8::String::NewFromUtf8(state->isolate,
                                value ? value : "",
                                v8::NewStringType::kInternalized,
                                static_cast<int>(length))
           .ToLocal(&v8str)) {
    state->setNativeError("Failed to create property name");
    return jsi_status_native_error;
  }
  *out_propnameid = abi::create_propnameid(
      static_cast<jsi_pointer *>(new PropNameIdHandle(state->isolate, v8str)));
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_create_propnameid_from_string(
    jsi_runtime *rt, jsi_string str, jsi_propnameid *out_propnameid) {
  if (!out_propnameid)
    return jsi_status_invalid_arg;
  *out_propnameid = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::String> v8str = toStringHandle(str)->get(state->isolate);
  *out_propnameid = abi::create_propnameid(
      static_cast<jsi_pointer *>(new PropNameIdHandle(state->isolate, v8str)));
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_create_propnameid_from_symbol(
    jsi_runtime *rt, jsi_symbol sym, jsi_propnameid *out_propnameid) {
  if (!out_propnameid)
    return jsi_status_invalid_arg;
  *out_propnameid = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Symbol> v8sym = toSymbolHandle(sym)->get(state->isolate);
  *out_propnameid = abi::create_propnameid(
      static_cast<jsi_pointer *>(new PropNameIdHandle(state->isolate, v8sym)));
  return jsi_status_ok;
}

bool JSI_ABI_CDECL jsi_prop_name_id_equals(jsi_runtime *rt, jsi_propnameid a,
                                        jsi_propnameid b) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Value> va = toPropNameIdHandle(a)->get(state->isolate);
  v8::Local<v8::Value> vb = toPropNameIdHandle(b)->get(state->isolate);
  return va->StrictEquals(vb);
}

void JSI_ABI_CDECL jsi_get_utf8_from_propnameid(jsi_runtime *rt,
                                             jsi_propnameid name,
                                             jsi_growable_buffer *buffer) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;

  v8::Local<v8::Value> v8val = toPropNameIdHandle(name)->get(state->isolate);
  v8::Local<v8::String> v8str;

  if (v8val->IsString()) {
    v8str = v8::Local<v8::String>::Cast(v8val);
  } else if (v8val->IsSymbol()) {
    v8::Local<v8::Symbol> sym = v8::Local<v8::Symbol>::Cast(v8val);
    v8::Local<v8::Value> desc = sym->Description(isolate);
    if (desc->IsString())
      v8str = v8::Local<v8::String>::Cast(desc);
  }

  if (v8str.IsEmpty()) {
    buffer->used = 0;
    return;
  }

  size_t utf8_len = v8str->Utf8LengthV2(isolate);
  if (buffer->size < utf8_len)
    buffer->api->try_grow_to(buffer, utf8_len);
  if (buffer->size >= utf8_len) {
    v8str->WriteUtf8V2(isolate, reinterpret_cast<char *>(buffer->data),
                        static_cast<int>(utf8_len));
    buffer->used = utf8_len;
  }
}

//------------------------------------------------------------------------------
// Object Operations
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_create_object(jsi_runtime *rt,
                                           jsi_object *out_object) {
  if (!out_object)
    return jsi_status_invalid_arg;
  *out_object = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Object> obj = v8::Object::New(state->isolate);
  *out_object = abi::create_object(
      static_cast<jsi_pointer *>(new ObjectHandle(state->isolate, obj)));
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_create_object_with_prototype(
    jsi_runtime *rt, const jsi_value *prototype, jsi_object *out_object) {
  if (!out_object)
    return jsi_status_invalid_arg;
  *out_object = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;

  v8::Local<v8::Value> proto =
      (prototype && !abi::is_undefined_value(*prototype) &&
       !abi::is_null_value(*prototype))
          ? toV8Value(isolate, prototype)
          : v8::Null(isolate).As<v8::Value>();

  v8::Local<v8::Object> obj =
      v8::Object::New(isolate, proto, nullptr, nullptr, 0);
  *out_object = abi::create_object(
      static_cast<jsi_pointer *>(new ObjectHandle(isolate, obj)));
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_get_prototype_of(
    jsi_runtime *rt, jsi_object obj, jsi_value *out_value) {
  if (!out_value)
    return jsi_status_invalid_arg;
  *out_value = abi::create_undefined_value();
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> proto = v8obj->GetPrototypeV2();
  *out_value = createJsiValue(state->isolate, proto);
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_set_prototype_of(jsi_runtime *rt,
                                                   jsi_object obj,
                                                   const jsi_value *prototype) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);
  v8::Isolate *isolate = state->isolate;

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> proto = prototype
                                    ? toV8Value(isolate, prototype)
                                    : v8::Null(isolate).As<v8::Value>();

  v8::Maybe<bool> success =
      v8obj->SetPrototypeV2(state->getContextLocal(), proto);
  if (success.IsNothing())
    return jsi_status_js_error;
  if (!success.FromJust()) {
    state->setNativeError("Failed to set prototype");
    return jsi_status_native_error;
  }
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_has_object_property_from_propnameid(
    jsi_runtime *rt, jsi_object obj, jsi_propnameid name, bool *out_result) {
  if (!out_result)
    return jsi_status_invalid_arg;
  *out_result = false;
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8name = toPropNameIdHandle(name)->get(state->isolate);

  v8::Maybe<bool> has = v8obj->Has(state->getContextLocal(), v8name);
  if (has.IsNothing())
    return jsi_status_js_error;
  *out_result = has.FromJust();
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_get_object_property_from_propnameid(
    jsi_runtime *rt, jsi_object obj, jsi_propnameid name,
    jsi_value *out_value) {
  if (!out_value)
    return jsi_status_invalid_arg;
  *out_value = abi::create_undefined_value();
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8name = toPropNameIdHandle(name)->get(state->isolate);

  v8::Local<v8::Value> val;
  if (!v8obj->Get(state->getContextLocal(), v8name).ToLocal(&val))
    return jsi_status_js_error;

  *out_value = createJsiValue(state->isolate, val);
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_set_object_property_from_propnameid(
    jsi_runtime *rt, jsi_object obj, jsi_propnameid name,
    const jsi_value *value) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8name = toPropNameIdHandle(name)->get(state->isolate);
  v8::Local<v8::Value> v8val = toV8Value(state->isolate, value);

  v8::Maybe<bool> success =
      v8obj->Set(state->getContextLocal(), v8name, v8val);
  if (success.IsNothing())
    return jsi_status_js_error;
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_delete_property_from_propnameid(
    jsi_runtime *rt, jsi_object obj, jsi_propnameid name) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8name = toPropNameIdHandle(name)->get(state->isolate);

  v8::Maybe<bool> deleted = v8obj->Delete(state->getContextLocal(), v8name);
  if (deleted.IsNothing())
    return jsi_status_js_error;
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_has_object_property_from_value(
    jsi_runtime *rt, jsi_object obj, const jsi_value *key, bool *out_result) {
  if (!out_result)
    return jsi_status_invalid_arg;
  *out_result = false;
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8key = toV8Value(state->isolate, key);

  v8::Maybe<bool> has = v8obj->Has(state->getContextLocal(), v8key);
  if (has.IsNothing())
    return jsi_status_js_error;
  *out_result = has.FromJust();
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_get_object_property_from_value(
    jsi_runtime *rt, jsi_object obj, const jsi_value *key,
    jsi_value *out_value) {
  if (!out_value)
    return jsi_status_invalid_arg;
  *out_value = abi::create_undefined_value();
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8key = toV8Value(state->isolate, key);

  v8::Local<v8::Value> val;
  if (!v8obj->Get(state->getContextLocal(), v8key).ToLocal(&val))
    return jsi_status_js_error;

  *out_value = createJsiValue(state->isolate, val);
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_set_object_property_from_value(
    jsi_runtime *rt, jsi_object obj, const jsi_value *key,
    const jsi_value *value) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8key = toV8Value(state->isolate, key);
  v8::Local<v8::Value> v8val = toV8Value(state->isolate, value);

  v8::Maybe<bool> success =
      v8obj->Set(state->getContextLocal(), v8key, v8val);
  if (success.IsNothing())
    return jsi_status_js_error;
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_delete_property_from_value(
    jsi_runtime *rt, jsi_object obj, const jsi_value *key) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8key = toV8Value(state->isolate, key);

  v8::Maybe<bool> deleted = v8obj->Delete(state->getContextLocal(), v8key);
  if (deleted.IsNothing())
    return jsi_status_js_error;
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_get_object_property_names(
    jsi_runtime *rt, jsi_object obj, jsi_array *out_array) {
  if (!out_array)
    return jsi_status_invalid_arg;
  *out_array = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Array> names;
  if (!v8obj
           ->GetPropertyNames(
               state->getContextLocal(),
               v8::KeyCollectionMode::kIncludePrototypes,
               static_cast<v8::PropertyFilter>(v8::ONLY_ENUMERABLE |
                                               v8::SKIP_SYMBOLS),
               v8::IndexFilter::kIncludeIndices,
               v8::KeyConversionMode::kConvertToString)
           .ToLocal(&names))
    return jsi_status_js_error;

  *out_array = abi::create_array(
      static_cast<jsi_pointer *>(new ObjectHandle(state->isolate, names)));
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_set_object_external_memory_pressure(
    jsi_runtime *rt, jsi_object obj, size_t amount) {
  (void)obj;
  auto *state = getState(rt);
  V8Scope scope(state);
  state->isolate->AdjustAmountOfExternalAllocatedMemory(
      static_cast<int64_t>(amount));
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_create_error(
    jsi_runtime *rt, const char *message, size_t length,
    jsi_object *out_object) {
  if (!out_object)
    return jsi_status_invalid_arg;
  *out_object = {};
  length = resolveUtf8Length(message, length);
  if (!message && length != 0)
    return jsi_status_invalid_arg;
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;

  v8::Local<v8::String> msgStr;
  if (message && length > 0) {
    if (!v8::String::NewFromUtf8(isolate, message,
                                  v8::NewStringType::kNormal,
                                  static_cast<int>(length))
             .ToLocal(&msgStr)) {
      state->setNativeError("Failed to create message string");
      return jsi_status_native_error;
    }
  } else {
    msgStr = v8::String::Empty(isolate);
  }

  v8::Local<v8::Value> error = v8::Exception::Error(msgStr);
  *out_object = abi::create_object(static_cast<jsi_pointer *>(
      new ObjectHandle(isolate, v8::Local<v8::Object>::Cast(error))));
  return jsi_status_ok;
}

//------------------------------------------------------------------------------
// Array Operations
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_create_array(
    jsi_runtime *rt, size_t length, jsi_array *out_array) {
  if (!out_array)
    return jsi_status_invalid_arg;
  *out_array = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Array> arr =
      v8::Array::New(state->isolate, static_cast<int>(length));
  *out_array = abi::create_array(
      static_cast<jsi_pointer *>(new ObjectHandle(state->isolate, arr)));
  return jsi_status_ok;
}

size_t JSI_ABI_CDECL jsi_get_array_length(jsi_runtime *rt, jsi_array arr) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Object> v8obj = toObjectHandle({arr.pointer})->get(state->isolate);
  if (!v8obj->IsArray())
    return 0;
  return v8::Local<v8::Array>::Cast(v8obj)->Length();
}

jsi_status JSI_ABI_CDECL jsi_get_array_element(
    jsi_runtime *rt, jsi_object arr, size_t index, jsi_value *out_value) {
  if (!out_value)
    return jsi_status_invalid_arg;
  *out_value = abi::create_undefined_value();
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(arr)->get(state->isolate);
  v8::Local<v8::Value> val;
  if (!v8obj->Get(state->getContextLocal(), static_cast<uint32_t>(index))
           .ToLocal(&val))
    return jsi_status_js_error;

  *out_value = createJsiValue(state->isolate, val);
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_set_array_element(jsi_runtime *rt,
                                                    jsi_object arr,
                                                    size_t index,
                                                    const jsi_value *value) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(arr)->get(state->isolate);
  v8::Local<v8::Value> v8val = toV8Value(state->isolate, value);

  v8::Maybe<bool> success =
      v8obj->Set(state->getContextLocal(), static_cast<uint32_t>(index), v8val);
  if (success.IsNothing())
    return jsi_status_js_error;
  return jsi_status_ok;
}

//------------------------------------------------------------------------------
// ArrayBuffer Operations
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_create_arraybuffer(
    jsi_runtime *rt, size_t byte_length, jsi_arraybuffer *out_arraybuffer) {
  if (!out_arraybuffer)
    return jsi_status_invalid_arg;
  *out_arraybuffer = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::ArrayBuffer> ab =
      v8::ArrayBuffer::New(state->isolate, byte_length);
  *out_arraybuffer = abi::create_arraybuffer(
      static_cast<jsi_pointer *>(new ObjectHandle(state->isolate, ab)));
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_create_arraybuffer_from_external_data(
    jsi_runtime *rt, jsi_mutable_buffer *buffer,
    jsi_arraybuffer *out_arraybuffer) {
  if (!out_arraybuffer)
    return jsi_status_invalid_arg;
  *out_arraybuffer = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;

#ifdef V8_ENABLE_SANDBOX
  // Under the V8 sandbox, backing stores must live inside the cage, so we cannot
  // hand V8 an external pointer (it is a fatal error). Allocate an in-cage
  // ArrayBuffer, copy the caller's bytes in, and release the source buffer
  // immediately. This mirrors the node-addon-api Buffer::NewOrCopy fallback;
  // JS<->native aliasing of the original buffer is not supported under the
  // sandbox.
  v8::Local<v8::ArrayBuffer> ab =
      v8::ArrayBuffer::New(isolate, buffer->size);
  if (buffer->size != 0)
    std::memcpy(ab->Data(), buffer->data, buffer->size);
  if (buffer->api && buffer->api->release)
    buffer->api->release(buffer);
  *out_arraybuffer = abi::create_arraybuffer(
      static_cast<jsi_pointer *>(new ObjectHandle(isolate, ab)));
#else
  struct BufRef {
    jsi_mutable_buffer *buffer;
  };
  auto *ref = new BufRef{buffer};

  std::unique_ptr<v8::BackingStore> backing_store =
      v8::ArrayBuffer::NewBackingStore(
          buffer->data, buffer->size,
          [](void * /*data*/, size_t /*length*/, void *deleter_data) {
            auto *r = static_cast<BufRef *>(deleter_data);
            if (r->buffer->api && r->buffer->api->release)
              r->buffer->api->release(r->buffer);
            delete r;
          },
          ref);

  v8::Local<v8::ArrayBuffer> ab =
      v8::ArrayBuffer::New(isolate, std::move(backing_store));
  *out_arraybuffer = abi::create_arraybuffer(
      static_cast<jsi_pointer *>(new ObjectHandle(isolate, ab)));
#endif  // V8_ENABLE_SANDBOX
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_get_arraybuffer_data(
    jsi_runtime *rt, jsi_arraybuffer ab, uint8_t **out_data) {
  if (!out_data)
    return jsi_status_invalid_arg;
  *out_data = nullptr;
  auto *state = getState(rt);
  V8Scope scope(state);

  v8::Local<v8::Object> v8obj = toObjectHandle({ab.pointer})->get(state->isolate);
  if (!v8obj->IsArrayBuffer()) {
    state->setNativeError("Object is not an ArrayBuffer");
    return jsi_status_native_error;
  }

  v8::Local<v8::ArrayBuffer> v8ab = v8::Local<v8::ArrayBuffer>::Cast(v8obj);
  *out_data = static_cast<uint8_t *>(v8ab->GetBackingStore()->Data());
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_get_arraybuffer_size(
    jsi_runtime *rt, jsi_arraybuffer ab, size_t *out_size) {
  if (!out_size)
    return jsi_status_invalid_arg;
  *out_size = 0;
  auto *state = getState(rt);
  V8Scope scope(state);

  v8::Local<v8::Object> v8obj = toObjectHandle({ab.pointer})->get(state->isolate);
  if (!v8obj->IsArrayBuffer()) {
    state->setNativeError("Object is not an ArrayBuffer");
    return jsi_status_native_error;
  }

  v8::Local<v8::ArrayBuffer> v8ab = v8::Local<v8::ArrayBuffer>::Cast(v8obj);
  *out_size = v8ab->ByteLength();
  return jsi_status_ok;
}

//------------------------------------------------------------------------------
// Function Operations
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_call(
    jsi_runtime *rt, jsi_function fn, const jsi_value *js_this,
    const jsi_value *args, size_t arg_count, jsi_value *out_value) {
  if (!out_value)
    return jsi_status_invalid_arg;
  *out_value = abi::create_undefined_value();
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);
  v8::Isolate *isolate = state->isolate;

  v8::Local<v8::Object> v8obj = toObjectHandle({fn.pointer})->get(state->isolate);
  if (!v8obj->IsFunction()) {
    state->setNativeError("Object is not a function");
    return jsi_status_native_error;
  }
  v8::Local<v8::Function> v8func = v8::Local<v8::Function>::Cast(v8obj);

  v8::Local<v8::Value> v8this =
      js_this ? toV8Value(isolate, js_this)
              : v8::Undefined(isolate).As<v8::Value>();

  std::vector<v8::Local<v8::Value>> v8args;
  v8args.reserve(arg_count);
  for (size_t i = 0; i < arg_count; ++i)
    v8args.push_back(toV8Value(isolate, &args[i]));

  v8::Local<v8::Value> callResult;
  if (!v8func
           ->Call(state->getContextLocal(), v8this,
                  static_cast<int>(arg_count), v8args.data())
           .ToLocal(&callResult))
    return jsi_status_js_error;

  *out_value = createJsiValue(isolate, callResult);
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_call_as_constructor(
    jsi_runtime *rt, jsi_function fn, const jsi_value *args,
    size_t arg_count, jsi_value *out_value) {
  if (!out_value)
    return jsi_status_invalid_arg;
  *out_value = abi::create_undefined_value();
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);
  v8::Isolate *isolate = state->isolate;

  v8::Local<v8::Object> v8obj = toObjectHandle({fn.pointer})->get(state->isolate);
  if (!v8obj->IsFunction()) {
    state->setNativeError("Object is not a function");
    return jsi_status_native_error;
  }
  v8::Local<v8::Function> v8func = v8::Local<v8::Function>::Cast(v8obj);

  std::vector<v8::Local<v8::Value>> v8args;
  v8args.reserve(arg_count);
  for (size_t i = 0; i < arg_count; ++i)
    v8args.push_back(toV8Value(isolate, &args[i]));

  v8::Local<v8::Object> constructed;
  if (!v8func
           ->NewInstance(state->getContextLocal(),
                         static_cast<int>(arg_count), v8args.data())
           .ToLocal(&constructed))
    return jsi_status_js_error;

  *out_value =
      abi::create_object_value(new ObjectHandle(isolate, constructed));
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_create_function_from_host_function(
    jsi_runtime *rt, jsi_propnameid name, uint32_t param_count,
    jsi_host_function *hf, jsi_function *out_function) {
  if (!out_function)
    return jsi_status_invalid_arg;
  *out_function = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);
  v8::Isolate *isolate = state->isolate;

  auto *ctx = new HostFunctionContext(rt, hf);
  v8::Local<v8::External> data = v8::External::New(isolate, ctx);

  v8::Local<v8::Function> func;
  if (!v8::Function::New(state->getContextLocal(),
                          HostFunctionCallbackTrampoline, data, param_count)
           .ToLocal(&func)) {
    ctx->hostFunction = nullptr; // caller still owns hf
    delete ctx;
    return jsi_status_js_error;
  }

  // Set up weak ref so ctx + hf are released when V8 GCs the function
  ctx->trackFunction(isolate, func);

  // Tag as host function so get_host_function can retrieve it
  func->SetPrivate(state->getContextLocal(), state->getHostFunctionKey(), data)
      .Check();

  if (name.pointer) {
    v8::Local<v8::Value> v8name = toPropNameIdHandle(name)->get(state->isolate);
    if (v8name->IsString())
      func->SetName(v8::Local<v8::String>::Cast(v8name));
  }

  *out_function = abi::create_function(
      static_cast<jsi_pointer *>(new ObjectHandle(isolate, func)));
  return jsi_status_ok;
}

jsi_host_function *JSI_ABI_CDECL jsi_get_host_function(jsi_runtime *rt,
                                                    jsi_function fn) {
  auto *state = getState(rt);
  V8Scope scope(state);

  v8::Local<v8::Object> v8obj = toObjectHandle({fn.pointer})->get(state->isolate);
  v8::Local<v8::Value> privateVal;
  if (v8obj->GetPrivate(state->getContextLocal(), state->getHostFunctionKey())
          .ToLocal(&privateVal) &&
      privateVal->IsExternal()) {
    auto *ctx = static_cast<HostFunctionContext *>(
        v8::Local<v8::External>::Cast(privateVal)->Value());
    return ctx->hostFunction;
  }
  return nullptr;
}

//------------------------------------------------------------------------------
// Host Object Operations
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_create_object_from_host_object(
    jsi_runtime *rt, jsi_host_object *ho, jsi_object *out_object) {
  if (!out_object)
    return jsi_status_invalid_arg;
  *out_object = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);
  v8::Isolate *isolate = state->isolate;
  v8::Local<v8::Context> context = state->getContextLocal();

  v8::Local<v8::Function> constructor = state->getHostObjectConstructor();

  v8::Local<v8::Object> newObject;
  if (!constructor->NewInstance(context).ToLocal(&newObject))
    return jsi_status_js_error;

  auto *proxy = new AbiHostObjectProxy(rt, ho, isolate, newObject);
  newObject->SetInternalField(0, v8::External::New(isolate, proxy));

  v8::Local<v8::Private> hostObjectKey = state->getHostObjectKey();
  newObject->SetPrivate(context, hostObjectKey,
                        v8::External::New(isolate, proxy));

  *out_object = abi::create_object(
      static_cast<jsi_pointer *>(new ObjectHandle(isolate, newObject)));
  return jsi_status_ok;
}

jsi_host_object *JSI_ABI_CDECL jsi_get_host_object(jsi_runtime *rt,
                                                jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Context> context = state->getContextLocal();

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Private> hostObjectKey = state->getHostObjectKey();

  v8::Local<v8::Value> privateValue;
  if (v8obj->GetPrivate(context, hostObjectKey).ToLocal(&privateValue) &&
      privateValue->IsExternal()) {
    auto *proxy = reinterpret_cast<AbiHostObjectProxy *>(
        v8::Local<v8::External>::Cast(privateValue)->Value());
    return proxy ? proxy->hostObject : nullptr;
  }
  return nullptr;
}

//------------------------------------------------------------------------------
// Native State
//------------------------------------------------------------------------------

bool JSI_ABI_CDECL jsi_has_native_state(jsi_runtime *rt, jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Context> context = state->getContextLocal();
  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);

  v8::Local<v8::Private> key = state->getNativeStateKey();
  v8::Maybe<bool> hasPrivate = v8obj->HasPrivate(context, key);
  return hasPrivate.FromMaybe(false);
}

jsi_native_state *JSI_ABI_CDECL jsi_get_native_state(jsi_runtime *rt,
                                                   jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Context> context = state->getContextLocal();
  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);

  NativeStateWrapper *wrapper = getNativeStateWrapper(state, context, v8obj);
  return wrapper ? wrapper->state : nullptr;
}

jsi_status JSI_ABI_CDECL jsi_set_native_state(jsi_runtime *rt,
                                                   jsi_object obj,
                                                   jsi_native_state *ns) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;
  v8::Local<v8::Context> context = state->getContextLocal();
  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Private> key = state->getNativeStateKey();

  NativeStateWrapper *existing = getNativeStateWrapper(state, context, v8obj);
  if (existing) {
    if (existing->state)
      existing->state->api->release(existing->state);
    existing->state = ns;
  } else {
    auto *wrapper = new NativeStateWrapper(isolate, v8obj, ns);
    v8::Local<v8::External> external = v8::External::New(isolate, wrapper);
    v8obj->SetPrivate(context, key, external).Check();
  }
  return jsi_status_ok;
}

//------------------------------------------------------------------------------
// Type Checks
//------------------------------------------------------------------------------

bool JSI_ABI_CDECL jsi_object_is_array(jsi_runtime *rt, jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toObjectHandle(obj)->get(state->isolate)->IsArray();
}

bool JSI_ABI_CDECL jsi_object_is_arraybuffer(jsi_runtime *rt, jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toObjectHandle(obj)->get(state->isolate)->IsArrayBuffer();
}

bool JSI_ABI_CDECL jsi_object_is_function(jsi_runtime *rt, jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toObjectHandle(obj)->get(state->isolate)->IsFunction();
}

//------------------------------------------------------------------------------
// Weak References
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_create_weak_object(
    jsi_runtime *rt, jsi_object obj, jsi_weak_object *out_weak_object) {
  if (!out_weak_object)
    return jsi_status_invalid_arg;
  *out_weak_object = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  *out_weak_object = abi::create_weak_object(
      static_cast<jsi_pointer *>(new WeakObjectHandle(state->isolate, v8obj)));
  return jsi_status_ok;
}

jsi_value JSI_ABI_CDECL jsi_lock_weak_object(jsi_runtime *rt,
                                          jsi_weak_object wo) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Object> v8obj = toWeakObjectHandle(wo)->get(state->isolate);
  if (v8obj.IsEmpty())
    return abi::create_undefined_value();
  return abi::create_object_value(new ObjectHandle(state->isolate, v8obj));
}

//------------------------------------------------------------------------------
// Comparison
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_instance_of(
    jsi_runtime *rt, jsi_object obj, jsi_function ctor, bool *out_result) {
  if (!out_result)
    return jsi_status_invalid_arg;
  *out_result = false;
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Object> v8ctor = toObjectHandle({ctor.pointer})->get(state->isolate);

  v8::Maybe<bool> result =
      v8obj->InstanceOf(state->getContextLocal(), v8ctor);
  if (result.IsNothing())
    return jsi_status_js_error;
  *out_result = result.FromJust();
  return jsi_status_ok;
}

bool JSI_ABI_CDECL jsi_strict_equals_symbol(jsi_runtime *rt, jsi_symbol a,
                                         jsi_symbol b) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toSymbolHandle(a)->get(state->isolate)->StrictEquals(toSymbolHandle(b)->get(state->isolate));
}

bool JSI_ABI_CDECL jsi_strict_equals_bigint(jsi_runtime *rt, jsi_bigint a,
                                         jsi_bigint b) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toBigIntHandle(a)->get(state->isolate)->StrictEquals(toBigIntHandle(b)->get(state->isolate));
}

bool JSI_ABI_CDECL jsi_strict_equals_string(jsi_runtime *rt, jsi_string a,
                                         jsi_string b) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toStringHandle(a)->get(state->isolate)->StrictEquals(toStringHandle(b)->get(state->isolate));
}

bool JSI_ABI_CDECL jsi_strict_equals_object(jsi_runtime *rt, jsi_object a,
                                         jsi_object b) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toObjectHandle(a)->get(state->isolate)->StrictEquals(toObjectHandle(b)->get(state->isolate));
}

//------------------------------------------------------------------------------
// BigInt Operations
//------------------------------------------------------------------------------

jsi_status JSI_ABI_CDECL jsi_create_bigint_from_int64(
    jsi_runtime *rt, int64_t value, jsi_bigint *out_bigint) {
  if (!out_bigint)
    return jsi_status_invalid_arg;
  *out_bigint = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::BigInt> bi = v8::BigInt::New(state->isolate, value);
  *out_bigint = abi::create_bigint(
      static_cast<jsi_pointer *>(new BigIntHandle(state->isolate, bi)));
  return jsi_status_ok;
}

jsi_status JSI_ABI_CDECL jsi_create_bigint_from_uint64(
    jsi_runtime *rt, uint64_t value, jsi_bigint *out_bigint) {
  if (!out_bigint)
    return jsi_status_invalid_arg;
  *out_bigint = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::BigInt> bi =
      v8::BigInt::NewFromUnsigned(state->isolate, value);
  *out_bigint = abi::create_bigint(
      static_cast<jsi_pointer *>(new BigIntHandle(state->isolate, bi)));
  return jsi_status_ok;
}

bool JSI_ABI_CDECL jsi_bigint_is_int64(jsi_runtime *rt, jsi_bigint bigint) {
  auto *state = getState(rt);
  V8Scope scope(state);
  bool lossless = true;
  toBigIntHandle(bigint)->get(state->isolate)->Int64Value(&lossless);
  return lossless;
}

bool JSI_ABI_CDECL jsi_bigint_is_uint64(jsi_runtime *rt, jsi_bigint bigint) {
  auto *state = getState(rt);
  V8Scope scope(state);
  bool lossless = true;
  toBigIntHandle(bigint)->get(state->isolate)->Uint64Value(&lossless);
  return lossless;
}

uint64_t JSI_ABI_CDECL jsi_bigint_truncate_to_uint64(jsi_runtime *rt,
                                                   jsi_bigint bigint) {
  auto *state = getState(rt);
  V8Scope scope(state);
  bool lossless = false;
  return toBigIntHandle(bigint)->get(state->isolate)->Uint64Value(&lossless);
}

int64_t JSI_ABI_CDECL jsi_bigint_to_int64(jsi_runtime *rt, jsi_bigint bigint,
                                       bool *out_lossless) {
  auto *state = getState(rt);
  V8Scope scope(state);
  bool is_lossless = true;
  int64_t result = toBigIntHandle(bigint)->get(state->isolate)->Int64Value(&is_lossless);
  if (out_lossless)
    *out_lossless = is_lossless;
  return result;
}

uint64_t JSI_ABI_CDECL jsi_bigint_to_uint64(jsi_runtime *rt, jsi_bigint bigint,
                                         bool *out_lossless) {
  auto *state = getState(rt);
  V8Scope scope(state);
  bool is_lossless = true;
  uint64_t result = toBigIntHandle(bigint)->get(state->isolate)->Uint64Value(&is_lossless);
  if (out_lossless)
    *out_lossless = is_lossless;
  return result;
}

jsi_status JSI_ABI_CDECL jsi_bigint_to_string(
    jsi_runtime *rt, jsi_bigint bigint, uint32_t radix,
    jsi_string *out_string) {
  if (!out_string)
    return jsi_status_invalid_arg;
  *out_string = {};
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);
  v8::Isolate *isolate = state->isolate;
  v8::Local<v8::Context> context = state->getContextLocal();

  v8::Local<v8::BigInt> bi = toBigIntHandle(bigint)->get(state->isolate);

  v8::Local<v8::Object> bigintProto;
  if (!bi->ToObject(context).ToLocal(&bigintProto))
    return jsi_status_js_error;

  v8::Local<v8::String> toStringName =
      v8::String::NewFromUtf8Literal(isolate, "toString");
  v8::Local<v8::Value> toStringFn;
  if (!bigintProto->Get(context, toStringName).ToLocal(&toStringFn) ||
      !toStringFn->IsFunction()) {
    state->setNativeError("BigInt.prototype.toString not found");
    return jsi_status_native_error;
  }

  v8::Local<v8::Value> radixArg =
      v8::Integer::New(isolate, static_cast<int32_t>(radix));
  v8::Local<v8::Value> resultValue;
  if (!v8::Local<v8::Function>::Cast(toStringFn)
           ->Call(context, bi, 1, &radixArg)
           .ToLocal(&resultValue))
    return jsi_status_js_error;

  if (!resultValue->IsString()) {
    state->setNativeError("BigInt.toString did not return a string");
    return jsi_status_native_error;
  }

  *out_string = abi::create_string(static_cast<jsi_pointer *>(
      new StringHandle(isolate, v8::Local<v8::String>::Cast(resultValue))));
  return jsi_status_ok;
}

//==============================================================================
// Runtime VTable Definition
//==============================================================================

const jsi_runtime_api g_vtable = {
    /* query_interface */ jsi_query_interface,
    /* get_abi_version */ jsi_get_abi_version,
    /* add_ref */ jsi_add_ref,
    /* release */ jsi_release,

    /* get_and_clear_js_error_value */ jsi_get_and_clear_js_error_value,
    /* get_and_clear_native_exception_message */
    jsi_get_and_clear_native_exception_message,
    /* set_js_error_value */ jsi_set_js_error_value,
    /* set_native_exception_message_utf8 */
    jsi_set_native_exception_message_utf8,

    /* clone_propnameid */ jsi_clone_propnameid,
    /* clone_string */ jsi_clone_string,
    /* clone_symbol */ jsi_clone_symbol,
    /* clone_object */ jsi_clone_object,
    /* clone_bigint */ jsi_clone_bigint,

    /* get_description */ jsi_get_description,
    /* is_inspectable */ jsi_is_inspectable,

    /* push_scope */ jsi_push_scope,
    /* pop_scope */ jsi_pop_scope,

    /* evaluate_javascript_source */ jsi_evaluate_javascript_source,
    /* prepare_javascript */ jsi_prepare_javascript,
    /* evaluate_prepared_javascript */ jsi_evaluate_prepared_javascript,

    /* drain_microtasks */ jsi_drain_microtasks,
    /* queue_microtask */ jsi_queue_microtask,

    /* get_global_object */ jsi_get_global_object,

    /* create_string_utf8 */ jsi_create_string_from_utf8,
    /* create_string_utf16 */ jsi_create_string_from_utf16,
    /* get_string_utf8 */ jsi_get_utf8_from_string,
    /* get_string_utf16 */ jsi_get_utf16_from_string,

    /* get_symbol_utf8 */ jsi_get_utf8_from_symbol,

    /* create_propnameid_utf8 */ jsi_create_propnameid_from_utf8,
    /* create_propnameid_from_string */ jsi_create_propnameid_from_string,
    /* create_propnameid_from_symbol */ jsi_create_propnameid_from_symbol,
    /* propnameid_equals */ jsi_prop_name_id_equals,
    /* get_propnameid_utf8 */ jsi_get_utf8_from_propnameid,

    /* create_object */ jsi_create_object,
    /* create_object_with_prototype */ jsi_create_object_with_prototype,
    /* get_prototype_of */ jsi_get_prototype_of,
    /* set_prototype_of */ jsi_set_prototype_of,

    /* has_object_property_from_propnameid */
    jsi_has_object_property_from_propnameid,
    /* get_object_property_from_propnameid */
    jsi_get_object_property_from_propnameid,
    /* set_object_property_from_propnameid */
    jsi_set_object_property_from_propnameid,
    /* delete_property_from_propnameid */ jsi_delete_property_from_propnameid,

    /* has_object_property_from_value */ jsi_has_object_property_from_value,
    /* get_object_property_from_value */ jsi_get_object_property_from_value,
    /* set_object_property_from_value */ jsi_set_object_property_from_value,
    /* delete_property_from_value */ jsi_delete_property_from_value,

    /* get_object_property_names */ jsi_get_object_property_names,
    /* set_object_external_memory_pressure */
    jsi_set_object_external_memory_pressure,
    /* create_error_utf8 */ jsi_create_error,

    /* create_array */ jsi_create_array,
    /* get_array_length */ jsi_get_array_length,
    /* get_array_element */ jsi_get_array_element,
    /* set_array_element */ jsi_set_array_element,

    /* create_arraybuffer */ jsi_create_arraybuffer,
    /* create_arraybuffer_from_external_data */
    jsi_create_arraybuffer_from_external_data,
    /* get_arraybuffer_data */ jsi_get_arraybuffer_data,
    /* get_arraybuffer_size */ jsi_get_arraybuffer_size,

    /* call */ jsi_call,
    /* call_as_constructor */ jsi_call_as_constructor,
    /* create_function_from_host_function */
    jsi_create_function_from_host_function,
    /* get_host_function */ jsi_get_host_function,

    /* create_object_from_host_object */ jsi_create_object_from_host_object,
    /* get_host_object */ jsi_get_host_object,

    /* has_native_state */ jsi_has_native_state,
    /* get_native_state */ jsi_get_native_state,
    /* set_native_state */ jsi_set_native_state,

    /* object_is_array */ jsi_object_is_array,
    /* object_is_arraybuffer */ jsi_object_is_arraybuffer,
    /* object_is_function */ jsi_object_is_function,

    /* create_weak_object */ jsi_create_weak_object,
    /* lock_weak_object */ jsi_lock_weak_object,

    /* instance_of */ jsi_instance_of,
    /* strict_equals_symbol */ jsi_strict_equals_symbol,
    /* strict_equals_bigint */ jsi_strict_equals_bigint,
    /* strict_equals_string */ jsi_strict_equals_string,
    /* strict_equals_object */ jsi_strict_equals_object,

    /* create_bigint_from_int64 */ jsi_create_bigint_from_int64,
    /* create_bigint_from_uint64 */ jsi_create_bigint_from_uint64,
    /* bigint_is_int64 */ jsi_bigint_is_int64,
    /* bigint_is_uint64 */ jsi_bigint_is_uint64,
    /* bigint_truncate_to_uint64 */ jsi_bigint_truncate_to_uint64,
    /* bigint_to_int64 */ jsi_bigint_to_int64,
    /* bigint_to_uint64 */ jsi_bigint_to_uint64,
    /* bigint_to_string */ jsi_bigint_to_string,
};

} // anonymous namespace

//==============================================================================
// Internal Accessors (jsi_abi_v8_internal.h)
//==============================================================================
//
// These are internal-coupling accessors exposed to other TUs compiled into
// v8jsi.dll (currently node-api/v8_api_abi.cpp). They reach into the
// anonymous-namespaced JsiRuntimeState without leaking its layout.

namespace v8rt_internal {

namespace {
inline JsiRuntimeState *toState(jsi_runtime *runtime) noexcept {
  return static_cast<JsiRuntimeState *>(runtime);
}
}  // namespace

v8::Isolate *getIsolate(jsi_runtime *runtime) noexcept {
  return toState(runtime)->isolate;
}

v8::Global<v8::Context> &getContext(jsi_runtime *runtime) noexcept {
  return toState(runtime)->context;
}

v8::Local<v8::Context> getContextLocal(jsi_runtime *runtime) noexcept {
  return toState(runtime)->getContextLocal();
}

bool getEnableMultiThread(jsi_runtime *runtime) noexcept {
  return toState(runtime)->enableMultiThread;
}

bool hasUnhandledPromiseRejection(jsi_runtime *runtime) noexcept {
  return !!toState(runtime)->last_unhandled_promise;
}

std::unique_ptr<v8rt::UnhandledPromiseRejection>
takeLastUnhandledPromiseRejection(jsi_runtime *runtime) noexcept {
  return std::exchange(toState(runtime)->last_unhandled_promise, nullptr);
}

ScriptCacheCallbacks getScriptCacheCallbacks(jsi_runtime *runtime) noexcept {
  auto *state = toState(runtime);
  return ScriptCacheCallbacks{
      state->script_cache_data,
      state->script_cache_load_cb,
      state->script_cache_store_cb};
}

void setAttachedOwner(jsi_runtime *runtime,
                      void *attached,
                      RuntimeAttachedDestroyCb destroy_cb) noexcept {
  auto *state = toState(runtime);
  state->attached_owner = attached;
  state->attached_owner_destroy = destroy_cb;
}

void clearAttachedOwner(jsi_runtime *runtime) noexcept {
  auto *state = toState(runtime);
  state->attached_owner = nullptr;
  state->attached_owner_destroy = nullptr;
}

}  // namespace v8rt_internal

//==============================================================================
// Entry Point
//==============================================================================

extern "C" {

// Flat runtime-creation export. abi_version is the consumer's compiled
// JSI_ABI_VERSION; configure (may be NULL) populates a factory-owned
// jsi_config via the v8_jsi_abi_config_set_* setters.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL v8_jsi_abi_runtime_create(
    uint32_t abi_version,
    jsi_configure_runtime_cb configure,
    void *user_data,
    jsi_runtime **out_runtime) {
  if (!out_runtime)
    return jsi_status_invalid_arg;
  *out_runtime = nullptr;

  // Model B version negotiation: refuse a version newer than we implement,
  // and version 1, which predates the status-based ABI.
  constexpr uint32_t kMinAbiVersion = 2;
  if (abi_version < kMinAbiVersion || abi_version > JSI_ABI_VERSION)
    return jsi_status_version_mismatch;

  jsi_runtime *rt = nullptr;
  if (configure) {
    // Factory owns the config: allocate it, let the consumer populate it via
    // the setters, consume it, then let it destruct. The handle never escapes
    // the callback. JsiRuntimeState::create clears the cache/task-runner
    // pointers it takes over, so ~jsi_config_s does not double-fire; on a
    // failure, ~jsi_config_s fires any registered deleters exactly once.
    jsi_config_s config;
    jsi_status status = configure(user_data, &config);
    if (status == jsi_status_ok)
      status = config.status;
    if (status != jsi_status_ok)
      return status;
    rt = JsiRuntimeState::create(&config);
  } else {
    // No configure callback → engine defaults. Passing nullptr selects the
    // historical default-init path (distinct from a zero-initialized config).
    rt = JsiRuntimeState::create(nullptr);
  }

  if (!rt)
    return jsi_status_native_error;

  // Pin the runtime to the negotiated version (reserved for future
  // behavioral branching; get_abi_version reports the impl max).
  static_cast<JsiRuntimeState *>(rt)->abi_version = abi_version;
  *out_runtime = rt;
  return jsi_status_ok;
}

// Process-global V8 engine flags. Raw CLI-style pass-through to V8 — these are
// process-global and consumed only on the first platform init, so they are NOT
// per-runtime config. Must be called before the first v8_jsi_abi_runtime_create.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL v8_jsi_abi_set_v8_flags(
    size_t *argc, char **argv, bool remove_flags) {
  if (!argc || (!argv && *argc != 0))
    return jsi_status_invalid_arg;
  if (*argc == 0)
    return jsi_status_ok;
  int n = static_cast<int>(*argc);
  v8::V8::SetFlagsFromCommandLine(&n, argv, remove_flags);
  *argc = n < 0 ? 0u : static_cast<size_t>(n);
  return jsi_status_ok;
}

// Startup-snapshot creation. See v8_jsi_abi.h for the contract. Builds a
// blob with v8::SnapshotCreator (its own isolate), running the builder script
// in a fresh context and serializing it as the default context. Exception-free.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL v8_jsi_abi_snapshot_create(
    const char *script,
    size_t script_length,
    const char *source_url,
    bool jitless,
    uint8_t **out_data,
    size_t *out_size) {
  (void)source_url;
  if (out_data)
    *out_data = nullptr;
  if (out_size)
    *out_size = 0;
  if (!script || !out_data || !out_size)
    return jsi_status_invalid_arg;
  script_length = resolveUtf8Length(script, script_length);
  if (script_length > static_cast<size_t>(v8::String::kMaxLength))
    return jsi_status_invalid_arg;

  // Ensure the V8 platform is initialized (idempotent). Set process-global
  // flags inside the one-time init so they match the eventual consumer engine.
  v8rt::V8PlatformHolder::initializePlatform(0, [jitless]() {
    if (jitless)
      v8::V8::SetFlagsFromString("--jitless");
  });

  // A non-null, null-terminated external-reference table (empty here — our
  // snapshots reach no native callbacks) must be supplied to BOTH the creator
  // and the consumer for V8 to recognize the serialized context table.
  static const intptr_t kNoExternalRefs[] = {0};

  v8::StartupData blob{nullptr, 0};
  v8::ArrayBuffer::Allocator *allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  bool ok = true;
  {
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = allocator;
    params.external_references = kNoExternalRefs;

    // SnapshotCreator allocates, owns, and enters its own isolate; its
    // destructor exits and disposes it.
    v8::SnapshotCreator creator(params);
    v8::Isolate *isolate = creator.GetIsolate();
    {
      v8::HandleScope handle_scope(isolate);

      // A SnapshotCreator needs a default context (its global proxy is NOT
      // serialized). Our builder state lives in a SEPARATE *added* context whose
      // global proxy IS serialized — so the consumer restores it (and all
      // globalThis state) verbatim via Context::FromSnapshot(isolate, 0). This
      // is the same pattern Node uses for its main context.
      v8::Local<v8::Context> default_context = v8::Context::New(isolate);
      v8::Local<v8::Context> builder_context = v8::Context::New(isolate);
      {
        v8::Context::Scope context_scope(builder_context);

        v8::Local<v8::String> source;
        if (!v8::String::NewFromUtf8(
                 isolate, script, v8::NewStringType::kNormal,
                 static_cast<int>(script_length))
                 .ToLocal(&source)) {
          ok = false;
        }

        v8::Local<v8::Script> script;
        if (ok && !v8::Script::Compile(builder_context, source).ToLocal(&script))
          ok = false;

        v8::Local<v8::Value> result;
        if (ok && !script->Run(builder_context).ToLocal(&result))
          ok = false;

        // The builder script's load is synchronous + microtasks; drain them so
        // the serialized heap reflects fully-settled state.
        if (ok)
          isolate->PerformMicrotaskCheckpoint();
      }
      if (ok) {
        creator.SetDefaultContext(default_context);
        creator.AddContext(builder_context); // index 0 (FromSnapshot target)
      }
    } // close HandleScope before CreateBlob (required)

    if (ok)
      blob = creator.CreateBlob(
          v8::SnapshotCreator::FunctionCodeHandling::kClear);
  } // creator destroyed: isolate exited + disposed

  jsi_status rc = jsi_status_ok;
  if (ok && blob.data && blob.raw_size > 0) {
    // Prefix the raw StartupData with the self-describing container header so a
    // stale/cross-engine blob is rejected at load time rather than crashing V8.
    // The version tag is final here: the SnapshotCreator isolate above was
    // initialized (flag implications enforced) before CreateBlob.
    const size_t raw = static_cast<size_t>(blob.raw_size);
    const size_t total = v8rt_snapshot::kHeaderSize + raw;
    uint8_t *bytes = static_cast<uint8_t *>(std::malloc(total));
    if (bytes) {
      v8rt_snapshot::writeHeader(bytes, static_cast<uint64_t>(raw));
      std::memcpy(bytes + v8rt_snapshot::kHeaderSize, blob.data, raw);
      *out_data = bytes;
      *out_size = total;
    } else {
      rc = jsi_status_native_error;
    }
  } else {
    rc = jsi_status_native_error;
  }

  // CreateBlob hands ownership of blob.data to us (allocated with new[]).
  delete[] blob.data;
  delete allocator;
  return rc;
}

V8_JSI_ABI_API void JSI_ABI_CDECL v8_jsi_abi_snapshot_destroy(uint8_t *data) {
  std::free(data);
}

V8_JSI_ABI_API v8_jsi_abi_snapshot_compat JSI_ABI_CDECL
v8_jsi_abi_snapshot_check(const uint8_t *data, size_t size) {
  // Initialize V8 (idempotent) so CachedDataVersionTag is read in its final,
  // flag-implication-enforced state — matching the create path. Any process-
  // global engine flags (e.g. --jitless) must already be set by the caller.
  v8rt::V8PlatformHolder::initializePlatform(0, []() {});
  return static_cast<v8_jsi_abi_snapshot_compat>(
      v8rt_snapshot::validate(data, size));
}

V8_JSI_ABI_API const char *JSI_ABI_CDECL
v8_jsi_abi_snapshot_compat_to_string(v8_jsi_abi_snapshot_compat value) {
  return v8rt_snapshot::compatString(static_cast<int>(value));
}

// inspector entry point. Starts a previously-quiet Agent (e.g. when
// the inspector was enabled on the config but break-on-start was false).
// Safe to call multiple times.
V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_runtime_open_inspector(jsi_runtime *runtime) {
  if (!runtime)
    return jsi_status_invalid_arg;
#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
  auto *state = static_cast<JsiRuntimeState *>(runtime);
  if (state->inspector_agent) {
    state->inspector_agent->start();
  }
#endif
  return jsi_status_ok;
}
// test-only hook: post a synthetic task to the runtime's foreground task
// runner. Gated behind JSI_TESTING_ONLY (gyp variable v8jsi_test_hooks) so
// release builds can drop it. Not declared in any public header; the test
// forward-declares it inline.
#ifdef JSI_TESTING_ONLY
JSI_ABI_API void JSI_ABI_CDECL v8_jsi_test_post_foreground_task(
    jsi_runtime *runtime,
    void (JSI_ABI_CDECL *task_run_cb)(void *),
    void *task_data) {
  if (!runtime || !task_run_cb)
    return;
  auto *state = static_cast<JsiRuntimeState *>(runtime);
  auto runner = v8rt::IsolateData::fromIsolate(state->isolate)->taskRunner();
  if (!runner)
    return;
  struct InlineTask final : public v8rt::TaskRunner::Task {
    void (JSI_ABI_CDECL *cb)(void *);
    void *data;
    void run() override { cb(data); }
  };
  auto task = std::make_unique<InlineTask>();
  task->cb = task_run_cb;
  task->data = task_data;
  runner->postTask(std::move(task));
}
#endif  // JSI_TESTING_ONLY

//==============================================================================
// V8 jsi_config — public C API
//==============================================================================

} // extern "C"

namespace {

// The first setter failure poisons the config; later setters return it.
jsi_status configFail(jsi_config config, jsi_status status) {
  if (config->status == jsi_status_ok)
    config->status = status;
  return config->status;
}

// Applies a plain value unless the config is NULL or already poisoned.
template <typename Fn>
jsi_status configSet(jsi_config config, Fn &&apply) {
  if (!config)
    return jsi_status_invalid_arg;
  if (config->status != jsi_status_ok)
    return config->status;
  apply(*config);
  return jsi_status_ok;
}

// An ownership-taking setter that fails still owns the incoming data, so it
// releases it before reporting the failure.
jsi_status releaseAndFail(jsi_config config,
                          jsi_status status,
                          jsi_data_delete_cb delete_cb,
                          void *delete_data,
                          void *data) {
  if (delete_cb)
    delete_cb(delete_data, data);
  return config ? configFail(config, status) : status;
}

// Status an ownership-taking setter starts from: invalid_arg for a NULL
// config, the poison status, or ok.
jsi_status configEntryStatus(jsi_config config) {
  return config ? config->status : jsi_status_invalid_arg;
}

} // namespace

extern "C" {

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_initial_heap_size(jsi_config config, size_t bytes) {
  return configSet(config, [&](jsi_config_s &c) {
    c.initial_heap_size_in_bytes = bytes;
  });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_maximum_heap_size(jsi_config config, size_t bytes) {
  return configSet(config, [&](jsi_config_s &c) {
    c.maximum_heap_size_in_bytes = bytes;
  });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_thread_pool_size(jsi_config config, uint8_t size) {
  return configSet(config, [&](jsi_config_s &c) { c.thread_pool_size = size; });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_debugger_runtime_name_utf8(jsi_config config,
                                                 const char *value,
                                                 size_t length) {
  if (!config)
    return jsi_status_invalid_arg;
  if (config->status != jsi_status_ok)
    return config->status;
  if (!value && length != 0 && length != JSI_ABI_AUTO_LENGTH)
    return configFail(config, jsi_status_invalid_arg);
  length = resolveUtf8Length(value, length);
  config->debugger_runtime_name.assign(value ? value : "", length);
  return jsi_status_ok;
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_inspector_enabled(jsi_config config, bool value) {
  return configSet(config, [&](jsi_config_s &c) { c.enable_inspector = value; });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_inspector_port(jsi_config config, uint16_t port) {
  return configSet(config, [&](jsi_config_s &c) { c.inspector_port = port; });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_inspector_break_on_start(jsi_config config, bool value) {
  return configSet(config, [&](jsi_config_s &c) {
    c.inspector_break_on_start = value;
  });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_explicit_microtask_policy(jsi_config config, bool value) {
  return configSet(config, [&](jsi_config_s &c) {
    c.explicit_microtask_policy = value;
  });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_ignore_unhandled_promises(jsi_config config, bool value) {
  return configSet(config, [&](jsi_config_s &c) {
    c.ignore_unhandled_promises = value;
  });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_gc_api_enabled(jsi_config config, bool value) {
  return configSet(config, [&](jsi_config_s &c) { c.enable_gc_api = value; });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_multi_thread_enabled(jsi_config config, bool value) {
  return configSet(config, [&](jsi_config_s &c) {
    c.enable_multi_thread = value;
  });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_jit_tracing_enabled(jsi_config config, bool value) {
  return configSet(config, [&](jsi_config_s &c) {
    c.enable_jit_tracing = value;
  });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_message_tracing_enabled(jsi_config config, bool value) {
  return configSet(config, [&](jsi_config_s &c) {
    c.enable_message_tracing = value;
  });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_gc_tracing_enabled(jsi_config config, bool value) {
  return configSet(config, [&](jsi_config_s &c) {
    c.enable_gc_tracing = value;
  });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL
v8_jsi_abi_config_set_system_instrumentation_enabled(jsi_config config,
                                                     bool value) {
  return configSet(config, [&](jsi_config_s &c) {
    c.enable_system_instrumentation = value;
  });
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL v8_jsi_abi_config_set_startup_snapshot(
    jsi_config config,
    const uint8_t *data,
    size_t size,
    jsi_data_delete_cb data_delete_cb,
    void *data_delete_data) {
  jsi_status status = configEntryStatus(config);
  if (status == jsi_status_ok && !data && size != 0)
    status = jsi_status_invalid_arg;
  if (status != jsi_status_ok)
    return releaseAndFail(config, status, data_delete_cb, data_delete_data,
                          const_cast<uint8_t *>(data));
  // Free any previously-set blob the config still owns.
  if (config->startup_snapshot_delete_cb) {
    config->startup_snapshot_delete_cb(
        config->startup_snapshot_delete_data,
        const_cast<uint8_t *>(config->startup_snapshot_blob));
  }
  config->startup_snapshot_blob = data;
  config->startup_snapshot_blob_size = size;
  config->startup_snapshot_delete_cb = data_delete_cb;
  config->startup_snapshot_delete_data = data_delete_data;
  return jsi_status_ok;
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL v8_jsi_abi_config_set_script_cache(
    jsi_config config,
    v8_jsi_abi_script_cache_load_cb load_cb,
    v8_jsi_abi_script_cache_store_cb store_cb,
    void *user_data,
    jsi_data_delete_cb user_data_delete_cb,
    void *user_data_delete_data) {
  jsi_status status = configEntryStatus(config);
  if (status != jsi_status_ok)
    return releaseAndFail(config, status, user_data_delete_cb,
                          user_data_delete_data, user_data);
  // Release any previously-set cache before overwriting.
  if (config->script_cache_data_delete_cb) {
    config->script_cache_data_delete_cb(
        config->script_cache_delete_data, config->script_cache_data);
  }
  config->script_cache_data = user_data;
  config->script_cache_load_cb = load_cb;
  config->script_cache_store_cb = store_cb;
  config->script_cache_data_delete_cb = user_data_delete_cb;
  config->script_cache_delete_data = user_data_delete_data;
  return jsi_status_ok;
}

V8_JSI_ABI_API jsi_status JSI_ABI_CDECL v8_jsi_abi_config_set_task_runner(
    jsi_config config,
    v8_jsi_abi_task_runner_post_task_cb post_task_cb,
    void *user_data,
    jsi_data_delete_cb user_data_delete_cb,
    void *user_data_delete_data) {
  jsi_status status = configEntryStatus(config);
  if (status != jsi_status_ok)
    return releaseAndFail(config, status, user_data_delete_cb,
                          user_data_delete_data, user_data);
  // Release any previously-set task runner before overwriting.
  if (config->task_runner_data_delete_cb) {
    config->task_runner_data_delete_cb(
        config->task_runner_delete_data, config->task_runner_data);
  }
  config->task_runner_data = user_data;
  config->task_runner_post_task_cb = post_task_cb;
  config->task_runner_data_delete_cb = user_data_delete_cb;
  config->task_runner_delete_data = user_data_delete_data;
  return jsi_status_ok;
}

} // extern "C"