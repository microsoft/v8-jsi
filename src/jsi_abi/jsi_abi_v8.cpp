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
/// jsi_pointer-based handles, *_or_error return types, and separate JS/native
/// error storage.
///
/// Error Handling Design
/// =====================
/// Two error types following the Hermes ABI pattern:
/// - jsi_error_js: JavaScript exception (value stored in pendingJSError)
/// - jsi_error_native: Native exception (message in nativeExceptionMessage)
///
/// The TryCatch class inherits from v8::TryCatch and automatically captures
/// JavaScript exceptions in its destructor.
///
/// This file is designed to work without C++ exceptions.

#include "jsi_abi/jsi_abi.h"
#include "jsi_abi/jsi_abi_helpers.h"
#include "jsi_abi/v8_jsi_config.h"
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
// Mirrors v8runtime::V8RuntimeArgs field-for-field so a future W2
// applyArgs() helper can simply call the setters one-to-one.
struct jsi_config_s {
  // Heap / threading
  size_t initial_heap_size_in_bytes{0};
  size_t maximum_heap_size_in_bytes{0};
  uint8_t thread_pool_size{0};

  // Inspector / debugger (W5 — wired into JsiRuntimeState::create).
  std::string debugger_runtime_name;
  uint16_t inspector_port{9223};
  bool enable_inspector{false};
  bool inspector_break_on_start{false};

  // Microtasks / promises
  bool explicit_microtask_policy{false};
  bool ignore_unhandled_promises{false};

  // GC
  bool enable_gc_api{false};

  // Concurrency (stored for future W6; currently inert)
  bool enable_multi_thread{false};

  // Tracing (stored for future W6; currently inert except for system instr)
  bool enable_jit_tracing{false};
  bool enable_message_tracing{false};
  bool enable_gc_tracing{false};
  bool enable_system_instrumentation{false};

  // Note: process-global V8 engine flags (sparkplug, predictable,
  // optimize_for_size, always_compact, jitless, lite_mode) are NOT per-runtime
  // config — they are set via the process-level v8_jsi_set_v8_flags before the
  // first runtime. See v8_jsi_config.h.

  // Script cache (W3) — null callbacks mean "no cache".
  // The config takes ownership of script_cache_data when the setter is called.
  // On v8_create_runtime the runtime takes over (we clear these pointers so
  // ~jsi_config_s does not double-delete). If the config is destroyed before
  // any runtime claims the data, ~jsi_config_s runs the deleter once.
  void *script_cache_data{nullptr};
  v8_jsi_script_cache_load_cb script_cache_load_cb{nullptr};
  v8_jsi_script_cache_store_cb script_cache_store_cb{nullptr};
  jsi_data_delete_cb script_cache_data_delete_cb{nullptr};
  void *script_cache_deleter_data{nullptr};

  // Task runner (W4) — null callbacks mean "no task runner".
  // Same lifetime contract as the script-cache fields above: config takes
  // ownership at setter time; on v8_create_runtime the runtime takes over
  // (these pointers are cleared); the deleter runs exactly once via the
  // internal adapter's destructor (held inside IsolateData by shared_ptr).
  // If the config is destroyed before any runtime claims the data,
  // ~jsi_config_s runs the deleter once.
  void *task_runner_data{nullptr};
  v8_jsi_task_runner_post_task_cb task_runner_post_task_cb{nullptr};
  jsi_data_delete_cb task_runner_data_delete_cb{nullptr};
  void *task_runner_deleter_data{nullptr};

  ~jsi_config_s() {
    if (script_cache_data_delete_cb) {
      script_cache_data_delete_cb(script_cache_data, script_cache_deleter_data);
    }
    if (task_runner_data_delete_cb) {
      task_runner_data_delete_cb(task_runner_data, task_runner_deleter_data);
    }
  }
};

namespace {

//==============================================================================
// Forward Declarations
//==============================================================================

struct JsiRuntimeState;
struct AbiHostObjectProxy;
struct HostFunctionContext;

//==============================================================================
// W4: TaskRunner adapter (C-callbacks → v8rt::TaskRunner)
//==============================================================================
//
// Internal-only adapter held inside v8rt::IsolateData via shared_ptr. The
// engine-internal v8rt::TaskRunner interface is what V8's inspector and other
// platform plumbing speak; this adapter forwards postTask() calls into the
// consumer-supplied C callback pair.
//
// The adapter owns task_runner_data and runs task_runner_data_delete_cb
// exactly once in its destructor — that's the single point where the
// consumer's runner is released.
class CTaskRunner final : public v8rt::TaskRunner {
 public:
  CTaskRunner(void *task_runner_data,
              v8_jsi_task_runner_post_task_cb post_task_cb,
              jsi_data_delete_cb task_runner_data_delete_cb,
              void *deleter_data) noexcept
      : task_runner_data_(task_runner_data),
        post_task_cb_(post_task_cb),
        task_runner_data_delete_cb_(task_runner_data_delete_cb),
        deleter_data_(deleter_data) {}

  ~CTaskRunner() override {
    if (task_runner_data_delete_cb_) {
      task_runner_data_delete_cb_(task_runner_data_, deleter_data_);
    }
  }

  CTaskRunner(const CTaskRunner &) = delete;
  CTaskRunner &operator=(const CTaskRunner &) = delete;

  void postTask(std::unique_ptr<Task> task) override {
    post_task_cb_(
        task_runner_data_,
        static_cast<void *>(task.release()),
        [](void *task_data) {
          static_cast<Task *>(task_data)->run();
        },
        [](void *task_data, void * /*deleter_data*/) {
          delete static_cast<Task *>(task_data);
        },
        /*deleter_data:*/ nullptr);
  }

 private:
  void *task_runner_data_;
  v8_jsi_task_runner_post_task_cb post_task_cb_;
  jsi_data_delete_cb task_runner_data_delete_cb_;
  void *deleter_data_;
};

//==============================================================================
// Handle Wrapper Types (inherit from jsi_pointer)
//==============================================================================

/// Base wrapper for ABI handles that wrap V8 persistent handles.
/// Inherits from jsi_pointer so the wrapper IS a jsi_pointer with an
/// invalidate callback in its vtable.
template <typename V8Type>
struct HandleWrapper : public jsi_pointer {
  static void JSI_CDECL invalidate(jsi_pointer *self) {
    delete static_cast<HandleWrapper *>(self);
  }
  static constexpr jsi_pointer_vtable pointerVt{invalidate};

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
/// vtable in the ABI.
struct PreparedScriptImpl : public jsi_prepared_javascript {
  static void JSI_CDECL release(jsi_prepared_javascript *self) {
    delete static_cast<PreparedScriptImpl *>(self);
  }
  static constexpr jsi_prepared_javascript_vtable preparedVt{release};

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
  static void JSI_CDECL invalidate(jsi_pointer *self) {
    delete static_cast<PropNameIdHandle *>(self);
  }
  static constexpr jsi_pointer_vtable pointerVt{invalidate};

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
  static void JSI_CDECL invalidate(jsi_pointer *self) {
    delete static_cast<WeakObjectHandle *>(self);
  }
  static constexpr jsi_pointer_vtable pointerVt{invalidate};

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
    buf->vtable->try_grow_to(buf, len);
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
      state->vtable->release(state);
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

  // ABI version negotiated in v8_create_runtime (the consumer's compiled
  // JSI_ABI_VERSION). Reserved for future behavioral pinning; get_abi_version
  // reports the implementation's max, not this value.
  uint32_t abi_version{JSI_ABI_VERSION};

  v8::Isolate *isolate;
  v8::Global<v8::Context> context;
  v8rt::IsolateData *isolateData;

  // True if the consumer requested v8::Locker-protected isolate access.
  // Read by V8Scope to construct an optional Locker before entering scope.
  bool enableMultiThread{false};

  // Script cache (W3) — copied from the config in create(). Runtime owns
  // script_cache_data after handoff: the deleter runs in ~JsiRuntimeState.
  void *script_cache_data{nullptr};
  v8_jsi_script_cache_load_cb script_cache_load_cb{nullptr};
  v8_jsi_script_cache_store_cb script_cache_store_cb{nullptr};
  jsi_data_delete_cb script_cache_data_delete_cb{nullptr};
  void *script_cache_deleter_data{nullptr};

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

  // W8: unhandled-promise tracking. Migrated from the legacy V8Runtime so the
  // Node-API path (jsr_has_unhandled_promise_rejection /
  // jsr_get_and_clear_last_unhandled_promise_rejection) keeps working.
  // The PromiseRejectCallback finds the runtime via context embedder slot 0.
  bool ignore_unhandled_promises{false};
  std::unique_ptr<v8rt::UnhandledPromiseRejection> last_unhandled_promise;

  // W8: optional attached-owner hook. v8_attach_node_api stores the
  // V8RuntimeEnv pointer and a destroy callback here so the Node-API surface
  // is torn down before V8 state is disposed in ~JsiRuntimeState.
  void *attached_owner{nullptr};
  v8rt_internal::RuntimeAttachedDestroyCb attached_owner_destroy{nullptr};

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
  // W5: per-runtime inspector agent. nullptr when enable_inspector is false.
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
      hostObject->vtable->release(hostObject);
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

    jsi_value_or_error result = proxy->hostObject->vtable->get(
        proxy->hostObject, proxy->runtime, propNameId);

    propNameId.pointer->vtable->invalidate(propNameId.pointer);

    if (abi::is_error(result)) {
      auto *state = getState(proxy->runtime);
      if (abi::get_error(result) == jsi_error_js) {
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

    jsi_value val = abi::get_value(result);
    info.GetReturnValue().Set(toV8Value(isolate, &val));
    abi::release_value(val);
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

    jsi_error_code result = proxy->hostObject->vtable->set(
        proxy->hostObject, proxy->runtime, propNameId, &jsiValue);

    propNameId.pointer->vtable->invalidate(propNameId.pointer);
    abi::release_value(jsiValue);

    if (abi::is_error(result)) {
      auto *state = getState(proxy->runtime);
      if (abi::get_error(result) == jsi_error_js &&
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

    jsi_propnameid_list_or_error listResult =
        proxy->hostObject->vtable->get_own_keys(proxy->hostObject,
                                                 proxy->runtime);

    if (abi::is_error(listResult)) {
      info.GetReturnValue().Set(v8::Array::New(isolate));
      return;
    }

    jsi_propnameid_list *list = abi::get_propnameid_list(listResult);
    v8::Local<v8::Array> result =
        v8::Array::New(isolate, static_cast<int>(list->size));
    for (size_t i = 0; i < list->size; i++) {
      v8::Local<v8::Value> propValue =
          toPropNameIdHandle(list->props[i])->get(isolate);
      result->Set(context, static_cast<uint32_t>(i), propValue)
          .FromMaybe(false);
    }
    list->vtable->release(list);

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
  // shipped before W1 — --expose_gc, kExplicit microtasks, enable_gc_api=true.
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

    // W4: take ownership of the task runner from the config. The shared_ptr
    // is handed to IsolateData by createIsolate; once IsolateData is freed in
    // ~JsiRuntimeState, the shared_ptr drops and the CTaskRunner destructor
    // runs the consumer's deleter exactly once. We clear the config fields
    // so ~jsi_config_s does not double-fire the deleter.
    if (config->task_runner_post_task_cb) {
      auto *mutableConfig = const_cast<jsi_config_s *>(config);
      isolateConfig.task_runner = std::make_shared<CTaskRunner>(
          mutableConfig->task_runner_data,
          mutableConfig->task_runner_post_task_cb,
          mutableConfig->task_runner_data_delete_cb,
          mutableConfig->task_runner_deleter_data);
      mutableConfig->task_runner_data = nullptr;
      mutableConfig->task_runner_post_task_cb = nullptr;
      mutableConfig->task_runner_data_delete_cb = nullptr;
      mutableConfig->task_runner_deleter_data = nullptr;
    }
  }

  v8::Isolate *isolate = v8rt::createIsolate(isolateConfig);
  if (!isolate)
    return nullptr;

  v8::Isolate::Scope isolate_scope(isolate);
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = v8rt::createContext(isolate);

  // Forward-declare the vtable (defined below)
  extern const jsi_runtime_vtable g_vtable;

  auto *state = new JsiRuntimeState();
  state->vt = &g_vtable;
  state->isolate = isolate;
  state->context.Reset(isolate, context);
  state->isolateData = v8rt::IsolateData::fromIsolate(isolate);
  state->enableMultiThread = !useDefaults && config->enable_multi_thread;
  state->ignore_unhandled_promises =
      !useDefaults && config->ignore_unhandled_promises;

  // W8: stamp the context with a back-pointer to this runtime + the v8jsi
  // marker tag. The PromiseRejectCallback (and any future static V8 callback
  // that only has a context handle) walks slot 0 to recover the runtime.
  context->SetAlignedPointerInEmbedderData(
      v8rt::ContextEmbedderIndex::kRuntime, state);
  context->SetAlignedPointerInEmbedderData(
      v8rt::ContextEmbedderIndex::kContextTag, v8rt::RuntimeContextTagPtr);

  // W8: install the unhandled-promise-rejection tracker unless the consumer
  // explicitly opted out. Mirrors legacy V8Runtime behavior — Node-API's
  // jsr_has_unhandled_promise_rejection depends on this.
  if (!state->ignore_unhandled_promises) {
    isolate->SetPromiseRejectCallback(JsiRuntimeState::PromiseRejectCallback);
  }

  // W3: take ownership of the script cache from the config. After this
  // handoff the config no longer owns script_cache_data, so freeing the
  // config does not invoke the deleter — only ~JsiRuntimeState does.
  if (!useDefaults) {
    auto *mutableConfig = const_cast<jsi_config_s *>(config);
    state->script_cache_data = mutableConfig->script_cache_data;
    state->script_cache_load_cb = mutableConfig->script_cache_load_cb;
    state->script_cache_store_cb = mutableConfig->script_cache_store_cb;
    state->script_cache_data_delete_cb =
        mutableConfig->script_cache_data_delete_cb;
    state->script_cache_deleter_data = mutableConfig->script_cache_deleter_data;
    mutableConfig->script_cache_data = nullptr;
    mutableConfig->script_cache_load_cb = nullptr;
    mutableConfig->script_cache_store_cb = nullptr;
    mutableConfig->script_cache_data_delete_cb = nullptr;
    mutableConfig->script_cache_deleter_data = nullptr;
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
      hostFunction->vtable->release(hostFunction);
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
  // W8: tear down the attached Node-API surface (if any) before disposing
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

  // W3: release the consumer-supplied script cache after the isolate is gone
  // (no more cache calls can be in flight). Runs in the consumer's CRT
  // because the consumer also supplied the deleter callback.
  if (script_cache_data_delete_cb) {
    script_cache_data_delete_cb(script_cache_data, script_cache_deleter_data);
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

void JSI_CDECL HostFunctionCallbackTrampoline(
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

  jsi_value_or_error result = ctx->hostFunction->vtable->call(
      ctx->hostFunction, ctx->runtime, &thisVal, args.data(), args.size());

  for (auto &arg : args)
    abi::release_value(arg);
  abi::release_value(thisVal);

  if (abi::is_error(result)) {
    auto *state = getState(ctx->runtime);
    jsi_error_code err = abi::get_error(result);
    if (err == jsi_error_js && abi::is_pointer_value(state->pendingJSError)) {
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

  jsi_value val = abi::get_value(result);
  info.GetReturnValue().Set(toV8Value(isolate, &val));
  abi::release_value(val);
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

jsi_error_code JSI_CDECL jsi_query_interface(
    jsi_runtime *rt, const jsi_interface_id * /*iid*/,
    const void ** /*vtable_out*/, void ** /*instance_out*/) {
  // No optional interfaces are currently supported. query_interface is
  // retained as the COM-style extension seam for future engine-specific
  // interfaces; callers must handle the not-supported result.
  getState(rt)->setNativeError("Interface not supported");
  return jsi_error_native;
}

//------------------------------------------------------------------------------
// ABI version
//------------------------------------------------------------------------------

// Report the implementation's max supported ABI version (Node-API
// napi_get_version analog). The create-time negotiation in v8_create_runtime is
// what actually gates; this is a feature-probe.
uint32_t JSI_CDECL jsi_get_abi_version(jsi_runtime * /*rt*/) {
  return JSI_ABI_VERSION;
}

//------------------------------------------------------------------------------
// Lifecycle
//------------------------------------------------------------------------------

void JSI_CDECL jsi_add_ref(jsi_runtime *rt) {
  getState(rt)->refcount.fetch_add(1, std::memory_order_relaxed);
}

void JSI_CDECL jsi_release(jsi_runtime *rt) {
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

jsi_value JSI_CDECL jsi_get_and_clear_js_error_value(jsi_runtime *rt) {
  auto *state = getState(rt);
  jsi_value result = state->pendingJSError;
  state->pendingJSError = abi::create_undefined_value();
  return result;
}

void JSI_CDECL jsi_get_and_clear_native_exception_message(
    jsi_runtime *rt, jsi_growable_buffer *buf) {
  auto *state = getState(rt);
  writeToBuf(buf, state->nativeExceptionMessage);
  state->nativeExceptionMessage.clear();
  state->nativeExceptionMessage.shrink_to_fit();
}

void JSI_CDECL jsi_set_js_error_value(jsi_runtime *rt,
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

void JSI_CDECL jsi_set_native_exception_message(jsi_runtime *rt,
                                                  const uint8_t *utf8,
                                                  size_t length) {
  getState(rt)->nativeExceptionMessage.assign(
      reinterpret_cast<const char *>(utf8), length);
}

//------------------------------------------------------------------------------
// Clone Operations
//------------------------------------------------------------------------------

jsi_propnameid JSI_CDECL jsi_clone_propnameid(jsi_runtime *rt,
                                               jsi_propnameid name) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Value> v8val = toPropNameIdHandle(name)->get(state->isolate);
  return {new PropNameIdHandle(state->isolate, v8val)};
}

jsi_string JSI_CDECL jsi_clone_string(jsi_runtime *rt, jsi_string str) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::String> v8str = toStringHandle(str)->get(state->isolate);
  return {new StringHandle(state->isolate, v8str)};
}

jsi_symbol JSI_CDECL jsi_clone_symbol(jsi_runtime *rt, jsi_symbol sym) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Symbol> v8sym = toSymbolHandle(sym)->get(state->isolate);
  return {new SymbolHandle(state->isolate, v8sym)};
}

jsi_object JSI_CDECL jsi_clone_object(jsi_runtime *rt, jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  return {new ObjectHandle(state->isolate, v8obj)};
}

jsi_bigint JSI_CDECL jsi_clone_bigint(jsi_runtime *rt, jsi_bigint bigint) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::BigInt> v8bi = toBigIntHandle(bigint)->get(state->isolate);
  return {new BigIntHandle(state->isolate, v8bi)};
}

//------------------------------------------------------------------------------
// Description & Capabilities
//------------------------------------------------------------------------------

void JSI_CDECL jsi_get_description(jsi_runtime * /*rt*/,
                                    jsi_growable_buffer *buf) {
  const char *v8_version = v8::V8::GetVersion();
  std::string desc = std::string("V8Runtime [") + v8_version + "]";
  writeToBuf(buf, desc);
}

bool JSI_CDECL jsi_is_inspectable(jsi_runtime * /*rt*/) { return false; }

//------------------------------------------------------------------------------
// Handle Scopes
//------------------------------------------------------------------------------

jsi_error_code JSI_CDECL jsi_push_scope(jsi_runtime * /*rt*/,
                                            void **scope) {
  if (scope)
    *scope = reinterpret_cast<void *>(1);
  return jsi_no_error;
}

jsi_error_code JSI_CDECL jsi_pop_scope(jsi_runtime * /*rt*/,
                                           void * /*scope*/) {
  return jsi_no_error;
}

//------------------------------------------------------------------------------
// Script Evaluation
//------------------------------------------------------------------------------

jsi_value_or_error JSI_CDECL jsi_evaluate_javascript_source(
    jsi_runtime *rt, jsi_buffer *buf, const char *source_url,
    size_t source_url_len) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;
  TryCatch try_catch(state);

  v8::Local<v8::String> sourceStr;
  if (!v8::String::NewFromUtf8(isolate,
                                reinterpret_cast<const char *>(buf->data),
                                v8::NewStringType::kNormal,
                                static_cast<int>(buf->size))
           .ToLocal(&sourceStr)) {
    if (buf->vtable && buf->vtable->release)
      buf->vtable->release(buf);
    state->setNativeError("Failed to create source string");
    return abi::create_value_or_error(jsi_error_native);
  }

  v8::Local<v8::String> urlV8Str;
  if (!v8::String::NewFromUtf8(isolate, source_url, v8::NewStringType::kNormal,
                                static_cast<int>(source_url_len))
           .ToLocal(&urlV8Str)) {
    if (buf->vtable && buf->vtable->release)
      buf->vtable->release(buf);
    state->setNativeError("Failed to create source URL string");
    return abi::create_value_or_error(jsi_error_native);
  }

  if (buf->vtable && buf->vtable->release)
    buf->vtable->release(buf);

  v8::ScriptOrigin origin(urlV8Str);

  v8::Local<v8::Script> compiled;
  if (!v8::Script::Compile(state->getContextLocal(), sourceStr, &origin)
           .ToLocal(&compiled))
    return abi::create_value_or_error(jsi_error_js);

  v8::Local<v8::Value> resultValue;
  if (!compiled->Run(state->getContextLocal()).ToLocal(&resultValue))
    return abi::create_value_or_error(jsi_error_js);

  return abi::create_value_or_error(createJsiValue(isolate, resultValue));
}

jsi_prepared_javascript_or_error JSI_CDECL jsi_prepare_javascript(
    jsi_runtime *rt, jsi_buffer *buf, const char *source_url,
    size_t source_url_len) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;
  TryCatch try_catch(state);

  // W3: compute MurmurHash3 over the source bytes for the cache key. Must
  // happen before buf->release() since the buffer pointer becomes invalid.
  uint64_t source_hash = 0;
  if (state->script_cache_load_cb || state->script_cache_store_cb) {
    murmurhash(buf->data, buf->size, source_hash);
  }

  v8::Local<v8::String> sourceStr;
  if (!v8::String::NewFromUtf8(isolate,
                                reinterpret_cast<const char *>(buf->data),
                                v8::NewStringType::kNormal,
                                static_cast<int>(buf->size))
           .ToLocal(&sourceStr)) {
    if (buf->vtable && buf->vtable->release)
      buf->vtable->release(buf);
    state->setNativeError("Failed to create source string");
    return abi::create_prepared_javascript_or_error(jsi_error_native);
  }

  v8::Local<v8::String> urlV8Str;
  if (!v8::String::NewFromUtf8(isolate, source_url, v8::NewStringType::kNormal,
                                static_cast<int>(source_url_len))
           .ToLocal(&urlV8Str)) {
    if (buf->vtable && buf->vtable->release)
      buf->vtable->release(buf);
    state->setNativeError("Failed to create source URL string");
    return abi::create_prepared_javascript_or_error(jsi_error_native);
  }

  if (buf->vtable && buf->vtable->release)
    buf->vtable->release(buf);

  v8::ScriptOrigin origin(urlV8Str);

  // W3: cache_tag = "perf" matches the legacy V8Runtime convention.
  // runtime_name = "V8" likewise — preserves cache keys for deployed
  // consumers whose stores already contain entries under that name.
  static constexpr const char *kCacheTag = "perf";
  static constexpr const char *kRuntimeName = "V8";
  const uint64_t runtime_version = v8::ScriptCompiler::CachedDataVersionTag();

  // W3: try cache load.
  v8::ScriptCompiler::CompileOptions options =
      v8::ScriptCompiler::CompileOptions::kNoCompileOptions;
  v8::ScriptCompiler::CachedData *cached_data = nullptr;
  const uint8_t *cache_buffer = nullptr;
  size_t cache_buffer_size = 0;
  jsi_data_delete_cb cache_buffer_delete_cb = nullptr;
  void *cache_buffer_deleter_data = nullptr;

  if (state->script_cache_load_cb) {
    state->script_cache_load_cb(
        state->script_cache_data,
        source_url,
        source_hash,
        kRuntimeName,
        runtime_version,
        kCacheTag,
        &cache_buffer,
        &cache_buffer_size,
        &cache_buffer_delete_cb,
        &cache_buffer_deleter_data);
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
    cache_buffer_delete_cb(const_cast<uint8_t *>(cache_buffer),
                            cache_buffer_deleter_data);
  }

  if (!compile_ok)
    return abi::create_prepared_javascript_or_error(jsi_error_js);

  v8::Local<v8::UnboundScript> unbound = compiled->GetUnboundScript();

  // W3: cache miss → produce code cache and hand it to the consumer.
  if (state->script_cache_store_cb &&
      options != v8::ScriptCompiler::CompileOptions::kConsumeCodeCache) {
    v8::ScriptCompiler::CachedData *codeCache =
        v8::ScriptCompiler::CreateCodeCache(unbound);
    if (codeCache) {
      // The consumer reads codeCache->data and then calls our delete_cb,
      // which deletes the V8 CachedData object (and its owned buffer) in the
      // DLL's CRT — the same CRT that allocated it.
      auto delete_cb = [](void * /*data*/, void *deleter_data) {
        delete static_cast<v8::ScriptCompiler::CachedData *>(deleter_data);
      };
      state->script_cache_store_cb(
          state->script_cache_data,
          source_url,
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

  return abi::create_prepared_javascript_or_error(
      new PreparedScriptImpl(isolate, unbound));
}

jsi_value_or_error JSI_CDECL
jsi_evaluate_prepared_javascript(jsi_runtime *rt,
                                  jsi_prepared_javascript *prepared) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;
  TryCatch try_catch(state);

  auto *impl = static_cast<PreparedScriptImpl *>(prepared);
  v8::Local<v8::UnboundScript> unbound = impl->get(state->isolate);
  v8::Local<v8::Script> script = unbound->BindToCurrentContext();

  v8::Local<v8::Value> resultValue;
  if (!script->Run(state->getContextLocal()).ToLocal(&resultValue))
    return abi::create_value_or_error(jsi_error_js);

  return abi::create_value_or_error(createJsiValue(isolate, resultValue));
}

//------------------------------------------------------------------------------
// Microtasks
//------------------------------------------------------------------------------

jsi_bool_or_error JSI_CDECL jsi_drain_microtasks(jsi_runtime *rt,
                                                   int32_t /*max_hint*/) {
  auto *state = getState(rt);
  V8Scope scope(state);

  if (state->isolate->GetMicrotasksPolicy() == v8::MicrotasksPolicy::kExplicit)
    state->isolate->PerformMicrotaskCheckpoint();

  return abi::create_bool_or_error(false);
}

jsi_error_code JSI_CDECL jsi_queue_microtask(jsi_runtime *rt,
                                                  jsi_function callback) {
  auto *state = getState(rt);
  V8Scope scope(state);

  v8::Local<v8::Object> obj = toObjectHandle({callback.pointer})->get(state->isolate);
  if (!obj->IsFunction()) {
    state->setNativeError("callback must be a function");
    return jsi_error_native;
  }

  state->isolate->EnqueueMicrotask(v8::Local<v8::Function>::Cast(obj));
  return jsi_no_error;
}

//------------------------------------------------------------------------------
// Global Object
//------------------------------------------------------------------------------

jsi_object JSI_CDECL jsi_get_global_object(jsi_runtime *rt) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Object> global = state->getContextLocal()->Global();
  return {new ObjectHandle(state->isolate, global)};
}

//------------------------------------------------------------------------------
// String Operations
//------------------------------------------------------------------------------

jsi_string_or_error JSI_CDECL jsi_create_string_from_utf8(jsi_runtime *rt,
                                                           const uint8_t *utf8,
                                                           size_t len) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::String> v8str;
  if (!v8::String::NewFromUtf8(state->isolate,
                                reinterpret_cast<const char *>(utf8),
                                v8::NewStringType::kNormal,
                                static_cast<int>(len))
           .ToLocal(&v8str)) {
    state->setNativeError("Failed to create string");
    return abi::create_string_or_error(jsi_error_native);
  }
  return abi::create_string_or_error(
      static_cast<jsi_pointer *>(new StringHandle(state->isolate, v8str)));
}

jsi_string_or_error JSI_CDECL jsi_create_string_from_utf16(
    jsi_runtime *rt, const uint16_t *utf16, size_t len) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::String> v8str;
  if (!v8::String::NewFromTwoByte(state->isolate, utf16,
                                   v8::NewStringType::kNormal,
                                   static_cast<int>(len))
           .ToLocal(&v8str)) {
    state->setNativeError("Failed to create string");
    return abi::create_string_or_error(jsi_error_native);
  }
  return abi::create_string_or_error(
      static_cast<jsi_pointer *>(new StringHandle(state->isolate, v8str)));
}

void JSI_CDECL jsi_get_utf8_from_string(jsi_runtime *rt, jsi_string str,
                                         jsi_growable_buffer *buf) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;
  v8::Local<v8::String> v8str = toStringHandle(str)->get(state->isolate);

  size_t utf8_len = v8str->Utf8LengthV2(isolate);
  if (buf->size < utf8_len)
    buf->vtable->try_grow_to(buf, utf8_len);
  if (buf->size >= utf8_len) {
    v8str->WriteUtf8V2(isolate, reinterpret_cast<char *>(buf->data),
                        static_cast<int>(utf8_len));
    buf->used = utf8_len;
  }
}

void JSI_CDECL jsi_get_utf16_from_string(jsi_runtime *rt, jsi_string str,
                                          jsi_growable_buffer *buf) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;
  v8::Local<v8::String> v8str = toStringHandle(str)->get(state->isolate);

  int len = v8str->Length();
  size_t bytes = static_cast<size_t>(len) * sizeof(uint16_t);
  if (buf->size < bytes)
    buf->vtable->try_grow_to(buf, bytes);
  if (buf->size >= bytes) {
    v8str->WriteV2(isolate, 0, len,
                    reinterpret_cast<uint16_t *>(buf->data));
    buf->used = bytes;
  }
}

//------------------------------------------------------------------------------
// Symbol Operations
//------------------------------------------------------------------------------

void JSI_CDECL jsi_get_utf8_from_symbol(jsi_runtime *rt, jsi_symbol sym,
                                         jsi_growable_buffer *buf) {
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
  writeToBuf(buf, result_str);
}

//------------------------------------------------------------------------------
// PropNameID Operations
//------------------------------------------------------------------------------

jsi_propnameid_or_error JSI_CDECL jsi_create_propnameid_from_utf8(
    jsi_runtime *rt, const uint8_t *utf8, size_t len) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::String> v8str;
  if (!v8::String::NewFromUtf8(state->isolate,
                                reinterpret_cast<const char *>(utf8),
                                v8::NewStringType::kInternalized,
                                static_cast<int>(len))
           .ToLocal(&v8str)) {
    state->setNativeError("Failed to create property name");
    return abi::create_propnameid_or_error(jsi_error_native);
  }
  return abi::create_propnameid_or_error(
      static_cast<jsi_pointer *>(new PropNameIdHandle(state->isolate, v8str)));
}

jsi_propnameid_or_error JSI_CDECL
jsi_create_propnameid_from_string(jsi_runtime *rt, jsi_string str) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::String> v8str = toStringHandle(str)->get(state->isolate);
  return abi::create_propnameid_or_error(
      static_cast<jsi_pointer *>(new PropNameIdHandle(state->isolate, v8str)));
}

jsi_propnameid_or_error JSI_CDECL
jsi_create_propnameid_from_symbol(jsi_runtime *rt, jsi_symbol sym) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Symbol> v8sym = toSymbolHandle(sym)->get(state->isolate);
  return abi::create_propnameid_or_error(
      static_cast<jsi_pointer *>(new PropNameIdHandle(state->isolate, v8sym)));
}

bool JSI_CDECL jsi_prop_name_id_equals(jsi_runtime *rt, jsi_propnameid a,
                                        jsi_propnameid b) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Value> va = toPropNameIdHandle(a)->get(state->isolate);
  v8::Local<v8::Value> vb = toPropNameIdHandle(b)->get(state->isolate);
  return va->StrictEquals(vb);
}

void JSI_CDECL jsi_get_utf8_from_propnameid(jsi_runtime *rt,
                                             jsi_propnameid name,
                                             jsi_growable_buffer *buf) {
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
    buf->used = 0;
    return;
  }

  size_t utf8_len = v8str->Utf8LengthV2(isolate);
  if (buf->size < utf8_len)
    buf->vtable->try_grow_to(buf, utf8_len);
  if (buf->size >= utf8_len) {
    v8str->WriteUtf8V2(isolate, reinterpret_cast<char *>(buf->data),
                        static_cast<int>(utf8_len));
    buf->used = utf8_len;
  }
}

//------------------------------------------------------------------------------
// Object Operations
//------------------------------------------------------------------------------

jsi_object_or_error JSI_CDECL jsi_create_object(jsi_runtime *rt) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Object> obj = v8::Object::New(state->isolate);
  return abi::create_object_or_error(
      static_cast<jsi_pointer *>(new ObjectHandle(state->isolate, obj)));
}

jsi_object_or_error JSI_CDECL
jsi_create_object_with_prototype(jsi_runtime *rt,
                                  const jsi_value *prototype) {
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
  return abi::create_object_or_error(
      static_cast<jsi_pointer *>(new ObjectHandle(isolate, obj)));
}

jsi_value_or_error JSI_CDECL jsi_get_prototype_of(jsi_runtime *rt,
                                                    jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> proto = v8obj->GetPrototypeV2();
  return abi::create_value_or_error(createJsiValue(state->isolate, proto));
}

jsi_error_code JSI_CDECL jsi_set_prototype_of(jsi_runtime *rt,
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
    return jsi_error_js;
  if (!success.FromJust()) {
    state->setNativeError("Failed to set prototype");
    return jsi_error_native;
  }
  return jsi_no_error;
}

jsi_bool_or_error JSI_CDECL jsi_has_object_property_from_propnameid(
    jsi_runtime *rt, jsi_object obj, jsi_propnameid name) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8name = toPropNameIdHandle(name)->get(state->isolate);

  v8::Maybe<bool> has = v8obj->Has(state->getContextLocal(), v8name);
  if (has.IsNothing())
    return abi::create_bool_or_error(jsi_error_js);
  return abi::create_bool_or_error(has.FromJust());
}

jsi_value_or_error JSI_CDECL jsi_get_object_property_from_propnameid(
    jsi_runtime *rt, jsi_object obj, jsi_propnameid name) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8name = toPropNameIdHandle(name)->get(state->isolate);

  v8::Local<v8::Value> val;
  if (!v8obj->Get(state->getContextLocal(), v8name).ToLocal(&val))
    return abi::create_value_or_error(jsi_error_js);

  return abi::create_value_or_error(createJsiValue(state->isolate, val));
}

jsi_error_code JSI_CDECL jsi_set_object_property_from_propnameid(
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
    return jsi_error_js;
  return jsi_no_error;
}

jsi_error_code JSI_CDECL jsi_delete_property_from_propnameid(
    jsi_runtime *rt, jsi_object obj, jsi_propnameid name) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8name = toPropNameIdHandle(name)->get(state->isolate);

  v8::Maybe<bool> deleted = v8obj->Delete(state->getContextLocal(), v8name);
  if (deleted.IsNothing())
    return jsi_error_js;
  return jsi_no_error;
}

jsi_bool_or_error JSI_CDECL jsi_has_object_property_from_value(
    jsi_runtime *rt, jsi_object obj, const jsi_value *key) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8key = toV8Value(state->isolate, key);

  v8::Maybe<bool> has = v8obj->Has(state->getContextLocal(), v8key);
  if (has.IsNothing())
    return abi::create_bool_or_error(jsi_error_js);
  return abi::create_bool_or_error(has.FromJust());
}

jsi_value_or_error JSI_CDECL jsi_get_object_property_from_value(
    jsi_runtime *rt, jsi_object obj, const jsi_value *key) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8key = toV8Value(state->isolate, key);

  v8::Local<v8::Value> val;
  if (!v8obj->Get(state->getContextLocal(), v8key).ToLocal(&val))
    return abi::create_value_or_error(jsi_error_js);

  return abi::create_value_or_error(createJsiValue(state->isolate, val));
}

jsi_error_code JSI_CDECL jsi_set_object_property_from_value(
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
    return jsi_error_js;
  return jsi_no_error;
}

jsi_error_code JSI_CDECL jsi_delete_property_from_value(
    jsi_runtime *rt, jsi_object obj, const jsi_value *key) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Value> v8key = toV8Value(state->isolate, key);

  v8::Maybe<bool> deleted = v8obj->Delete(state->getContextLocal(), v8key);
  if (deleted.IsNothing())
    return jsi_error_js;
  return jsi_no_error;
}

jsi_array_or_error JSI_CDECL jsi_get_object_property_names(jsi_runtime *rt,
                                                            jsi_object obj) {
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
    return abi::create_array_or_error(jsi_error_js);

  return abi::create_array_or_error(
      static_cast<jsi_pointer *>(new ObjectHandle(state->isolate, names)));
}

jsi_error_code JSI_CDECL jsi_set_object_external_memory_pressure(
    jsi_runtime *rt, jsi_object /*obj*/, size_t amount) {
  auto *state = getState(rt);
  V8Scope scope(state);
  state->isolate->AdjustAmountOfExternalAllocatedMemory(
      static_cast<int64_t>(amount));
  return jsi_no_error;
}

jsi_object_or_error JSI_CDECL jsi_create_error(jsi_runtime *rt,
                                                const uint8_t *utf8_msg,
                                                size_t len) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;

  v8::Local<v8::String> msgStr;
  if (utf8_msg && len > 0) {
    if (!v8::String::NewFromUtf8(isolate,
                                  reinterpret_cast<const char *>(utf8_msg),
                                  v8::NewStringType::kNormal,
                                  static_cast<int>(len))
             .ToLocal(&msgStr)) {
      state->setNativeError("Failed to create message string");
      return abi::create_object_or_error(jsi_error_native);
    }
  } else {
    msgStr = v8::String::Empty(isolate);
  }

  v8::Local<v8::Value> error = v8::Exception::Error(msgStr);
  return abi::create_object_or_error(static_cast<jsi_pointer *>(
      new ObjectHandle(isolate, v8::Local<v8::Object>::Cast(error))));
}

//------------------------------------------------------------------------------
// Array Operations
//------------------------------------------------------------------------------

jsi_array_or_error JSI_CDECL jsi_create_array(jsi_runtime *rt, size_t length) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Array> arr =
      v8::Array::New(state->isolate, static_cast<int>(length));
  return abi::create_array_or_error(
      static_cast<jsi_pointer *>(new ObjectHandle(state->isolate, arr)));
}

size_t JSI_CDECL jsi_get_array_length(jsi_runtime *rt, jsi_array arr) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Object> v8obj = toObjectHandle({arr.pointer})->get(state->isolate);
  if (!v8obj->IsArray())
    return 0;
  return v8::Local<v8::Array>::Cast(v8obj)->Length();
}

jsi_value_or_error JSI_CDECL jsi_get_array_element(jsi_runtime *rt,
                                                    jsi_object arr,
                                                    size_t index) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(arr)->get(state->isolate);
  v8::Local<v8::Value> val;
  if (!v8obj->Get(state->getContextLocal(), static_cast<uint32_t>(index))
           .ToLocal(&val))
    return abi::create_value_or_error(jsi_error_js);

  return abi::create_value_or_error(createJsiValue(state->isolate, val));
}

jsi_error_code JSI_CDECL jsi_set_array_element(jsi_runtime *rt,
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
    return jsi_error_js;
  return jsi_no_error;
}

//------------------------------------------------------------------------------
// ArrayBuffer Operations
//------------------------------------------------------------------------------

jsi_arraybuffer_or_error JSI_CDECL jsi_create_arraybuffer(jsi_runtime *rt,
                                                           size_t byte_length) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::ArrayBuffer> ab =
      v8::ArrayBuffer::New(state->isolate, byte_length);
  return abi::create_arraybuffer_or_error(
      static_cast<jsi_pointer *>(new ObjectHandle(state->isolate, ab)));
}

jsi_arraybuffer_or_error JSI_CDECL jsi_create_arraybuffer_from_external_data(
    jsi_runtime *rt, jsi_mutable_buffer *buf) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Isolate *isolate = state->isolate;

  struct BufRef {
    jsi_mutable_buffer *buf;
  };
  auto *ref = new BufRef{buf};

  std::unique_ptr<v8::BackingStore> backing_store =
      v8::ArrayBuffer::NewBackingStore(
          buf->data, buf->size,
          [](void * /*data*/, size_t /*length*/, void *deleter_data) {
            auto *r = static_cast<BufRef *>(deleter_data);
            if (r->buf->vtable && r->buf->vtable->release)
              r->buf->vtable->release(r->buf);
            delete r;
          },
          ref);

  v8::Local<v8::ArrayBuffer> ab =
      v8::ArrayBuffer::New(isolate, std::move(backing_store));
  return abi::create_arraybuffer_or_error(
      static_cast<jsi_pointer *>(new ObjectHandle(isolate, ab)));
}

jsi_uint8_ptr_or_error JSI_CDECL jsi_get_arraybuffer_data(jsi_runtime *rt,
                                                           jsi_arraybuffer ab) {
  auto *state = getState(rt);
  V8Scope scope(state);

  v8::Local<v8::Object> v8obj = toObjectHandle({ab.pointer})->get(state->isolate);
  if (!v8obj->IsArrayBuffer()) {
    state->setNativeError("Object is not an ArrayBuffer");
    return abi::create_uint8_ptr_or_error(jsi_error_native);
  }

  v8::Local<v8::ArrayBuffer> v8ab = v8::Local<v8::ArrayBuffer>::Cast(v8obj);
  return abi::create_uint8_ptr_or_error(
      static_cast<uint8_t *>(v8ab->GetBackingStore()->Data()));
}

jsi_size_or_error JSI_CDECL jsi_get_arraybuffer_size(jsi_runtime *rt,
                                                      jsi_arraybuffer ab) {
  auto *state = getState(rt);
  V8Scope scope(state);

  v8::Local<v8::Object> v8obj = toObjectHandle({ab.pointer})->get(state->isolate);
  if (!v8obj->IsArrayBuffer()) {
    state->setNativeError("Object is not an ArrayBuffer");
    return abi::create_size_or_error(jsi_error_native);
  }

  v8::Local<v8::ArrayBuffer> v8ab = v8::Local<v8::ArrayBuffer>::Cast(v8obj);
  return abi::create_size_or_error(v8ab->ByteLength());
}

//------------------------------------------------------------------------------
// Function Operations
//------------------------------------------------------------------------------

jsi_value_or_error JSI_CDECL jsi_call(jsi_runtime *rt, jsi_function fn,
                                       const jsi_value *js_this,
                                       const jsi_value *args,
                                       size_t arg_count) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);
  v8::Isolate *isolate = state->isolate;

  v8::Local<v8::Object> v8obj = toObjectHandle({fn.pointer})->get(state->isolate);
  if (!v8obj->IsFunction()) {
    state->setNativeError("Object is not a function");
    return abi::create_value_or_error(jsi_error_native);
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
    return abi::create_value_or_error(jsi_error_js);

  return abi::create_value_or_error(createJsiValue(isolate, callResult));
}

jsi_value_or_error JSI_CDECL jsi_call_as_constructor(jsi_runtime *rt,
                                                      jsi_function fn,
                                                      const jsi_value *args,
                                                      size_t arg_count) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);
  v8::Isolate *isolate = state->isolate;

  v8::Local<v8::Object> v8obj = toObjectHandle({fn.pointer})->get(state->isolate);
  if (!v8obj->IsFunction()) {
    state->setNativeError("Object is not a function");
    return abi::create_value_or_error(jsi_error_native);
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
    return abi::create_value_or_error(jsi_error_js);

  return abi::create_value_or_error(
      abi::create_object_value(new ObjectHandle(isolate, constructed)));
}

jsi_function_or_error JSI_CDECL jsi_create_function_from_host_function(
    jsi_runtime *rt, jsi_propnameid name, uint32_t param_count,
    jsi_host_function *hf) {
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
    return abi::create_function_or_error(jsi_error_js);
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

  return abi::create_function_or_error(
      static_cast<jsi_pointer *>(new ObjectHandle(isolate, func)));
}

jsi_host_function *JSI_CDECL jsi_get_host_function(jsi_runtime *rt,
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

jsi_object_or_error JSI_CDECL
jsi_create_object_from_host_object(jsi_runtime *rt, jsi_host_object *ho) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);
  v8::Isolate *isolate = state->isolate;
  v8::Local<v8::Context> context = state->getContextLocal();

  v8::Local<v8::Function> constructor = state->getHostObjectConstructor();

  v8::Local<v8::Object> newObject;
  if (!constructor->NewInstance(context).ToLocal(&newObject))
    return abi::create_object_or_error(jsi_error_js);

  auto *proxy = new AbiHostObjectProxy(rt, ho, isolate, newObject);
  newObject->SetInternalField(0, v8::External::New(isolate, proxy));

  v8::Local<v8::Private> hostObjectKey = state->getHostObjectKey();
  newObject->SetPrivate(context, hostObjectKey,
                        v8::External::New(isolate, proxy));

  return abi::create_object_or_error(
      static_cast<jsi_pointer *>(new ObjectHandle(isolate, newObject)));
}

jsi_host_object *JSI_CDECL jsi_get_host_object(jsi_runtime *rt,
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

bool JSI_CDECL jsi_has_native_state(jsi_runtime *rt, jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Context> context = state->getContextLocal();
  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);

  v8::Local<v8::Private> key = state->getNativeStateKey();
  v8::Maybe<bool> hasPrivate = v8obj->HasPrivate(context, key);
  return hasPrivate.FromMaybe(false);
}

jsi_native_state *JSI_CDECL jsi_get_native_state(jsi_runtime *rt,
                                                   jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Context> context = state->getContextLocal();
  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);

  NativeStateWrapper *wrapper = getNativeStateWrapper(state, context, v8obj);
  return wrapper ? wrapper->state : nullptr;
}

jsi_error_code JSI_CDECL jsi_set_native_state(jsi_runtime *rt,
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
      existing->state->vtable->release(existing->state);
    existing->state = ns;
  } else {
    auto *wrapper = new NativeStateWrapper(isolate, v8obj, ns);
    v8::Local<v8::External> external = v8::External::New(isolate, wrapper);
    v8obj->SetPrivate(context, key, external).Check();
  }
  return jsi_no_error;
}

//------------------------------------------------------------------------------
// Type Checks
//------------------------------------------------------------------------------

bool JSI_CDECL jsi_object_is_array(jsi_runtime *rt, jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toObjectHandle(obj)->get(state->isolate)->IsArray();
}

bool JSI_CDECL jsi_object_is_arraybuffer(jsi_runtime *rt, jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toObjectHandle(obj)->get(state->isolate)->IsArrayBuffer();
}

bool JSI_CDECL jsi_object_is_function(jsi_runtime *rt, jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toObjectHandle(obj)->get(state->isolate)->IsFunction();
}

//------------------------------------------------------------------------------
// Weak References
//------------------------------------------------------------------------------

jsi_weak_object_or_error JSI_CDECL jsi_create_weak_object(jsi_runtime *rt,
                                                           jsi_object obj) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  return abi::create_weak_object_or_error(
      static_cast<jsi_pointer *>(new WeakObjectHandle(state->isolate, v8obj)));
}

jsi_value JSI_CDECL jsi_lock_weak_object(jsi_runtime *rt,
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

jsi_bool_or_error JSI_CDECL jsi_instance_of(jsi_runtime *rt, jsi_object obj,
                                             jsi_function ctor) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);

  v8::Local<v8::Object> v8obj = toObjectHandle(obj)->get(state->isolate);
  v8::Local<v8::Object> v8ctor = toObjectHandle({ctor.pointer})->get(state->isolate);

  v8::Maybe<bool> result =
      v8obj->InstanceOf(state->getContextLocal(), v8ctor);
  if (result.IsNothing())
    return abi::create_bool_or_error(jsi_error_js);
  return abi::create_bool_or_error(result.FromJust());
}

bool JSI_CDECL jsi_strict_equals_symbol(jsi_runtime *rt, jsi_symbol a,
                                         jsi_symbol b) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toSymbolHandle(a)->get(state->isolate)->StrictEquals(toSymbolHandle(b)->get(state->isolate));
}

bool JSI_CDECL jsi_strict_equals_bigint(jsi_runtime *rt, jsi_bigint a,
                                         jsi_bigint b) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toBigIntHandle(a)->get(state->isolate)->StrictEquals(toBigIntHandle(b)->get(state->isolate));
}

bool JSI_CDECL jsi_strict_equals_string(jsi_runtime *rt, jsi_string a,
                                         jsi_string b) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toStringHandle(a)->get(state->isolate)->StrictEquals(toStringHandle(b)->get(state->isolate));
}

bool JSI_CDECL jsi_strict_equals_object(jsi_runtime *rt, jsi_object a,
                                         jsi_object b) {
  auto *state = getState(rt);
  V8Scope scope(state);
  return toObjectHandle(a)->get(state->isolate)->StrictEquals(toObjectHandle(b)->get(state->isolate));
}

//------------------------------------------------------------------------------
// BigInt Operations
//------------------------------------------------------------------------------

jsi_bigint_or_error JSI_CDECL jsi_create_bigint_from_int64(jsi_runtime *rt,
                                                            int64_t value) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::BigInt> bi = v8::BigInt::New(state->isolate, value);
  return abi::create_bigint_or_error(
      static_cast<jsi_pointer *>(new BigIntHandle(state->isolate, bi)));
}

jsi_bigint_or_error JSI_CDECL jsi_create_bigint_from_uint64(jsi_runtime *rt,
                                                             uint64_t value) {
  auto *state = getState(rt);
  V8Scope scope(state);
  v8::Local<v8::BigInt> bi =
      v8::BigInt::NewFromUnsigned(state->isolate, value);
  return abi::create_bigint_or_error(
      static_cast<jsi_pointer *>(new BigIntHandle(state->isolate, bi)));
}

bool JSI_CDECL jsi_bigint_is_int64(jsi_runtime *rt, jsi_bigint bigint) {
  auto *state = getState(rt);
  V8Scope scope(state);
  bool lossless = true;
  toBigIntHandle(bigint)->get(state->isolate)->Int64Value(&lossless);
  return lossless;
}

bool JSI_CDECL jsi_bigint_is_uint64(jsi_runtime *rt, jsi_bigint bigint) {
  auto *state = getState(rt);
  V8Scope scope(state);
  bool lossless = true;
  toBigIntHandle(bigint)->get(state->isolate)->Uint64Value(&lossless);
  return lossless;
}

uint64_t JSI_CDECL jsi_bigint_truncate_to_uint64(jsi_runtime *rt,
                                                   jsi_bigint bigint) {
  auto *state = getState(rt);
  V8Scope scope(state);
  bool lossless = false;
  return toBigIntHandle(bigint)->get(state->isolate)->Uint64Value(&lossless);
}

int64_t JSI_CDECL jsi_bigint_to_int64(jsi_runtime *rt, jsi_bigint bigint,
                                       bool *lossless) {
  auto *state = getState(rt);
  V8Scope scope(state);
  bool is_lossless = true;
  int64_t result = toBigIntHandle(bigint)->get(state->isolate)->Int64Value(&is_lossless);
  if (lossless)
    *lossless = is_lossless;
  return result;
}

uint64_t JSI_CDECL jsi_bigint_to_uint64(jsi_runtime *rt, jsi_bigint bigint,
                                         bool *lossless) {
  auto *state = getState(rt);
  V8Scope scope(state);
  bool is_lossless = true;
  uint64_t result = toBigIntHandle(bigint)->get(state->isolate)->Uint64Value(&is_lossless);
  if (lossless)
    *lossless = is_lossless;
  return result;
}

jsi_string_or_error JSI_CDECL jsi_bigint_to_string(jsi_runtime *rt,
                                                    jsi_bigint bigint,
                                                    uint32_t radix) {
  auto *state = getState(rt);
  V8Scope scope(state);
  TryCatch try_catch(state);
  v8::Isolate *isolate = state->isolate;
  v8::Local<v8::Context> context = state->getContextLocal();

  v8::Local<v8::BigInt> bi = toBigIntHandle(bigint)->get(state->isolate);

  v8::Local<v8::Object> bigintProto;
  if (!bi->ToObject(context).ToLocal(&bigintProto))
    return abi::create_string_or_error(jsi_error_js);

  v8::Local<v8::String> toStringName =
      v8::String::NewFromUtf8Literal(isolate, "toString");
  v8::Local<v8::Value> toStringFn;
  if (!bigintProto->Get(context, toStringName).ToLocal(&toStringFn) ||
      !toStringFn->IsFunction()) {
    state->setNativeError("BigInt.prototype.toString not found");
    return abi::create_string_or_error(jsi_error_native);
  }

  v8::Local<v8::Value> radixArg =
      v8::Integer::New(isolate, static_cast<int32_t>(radix));
  v8::Local<v8::Value> resultValue;
  if (!v8::Local<v8::Function>::Cast(toStringFn)
           ->Call(context, bi, 1, &radixArg)
           .ToLocal(&resultValue))
    return abi::create_string_or_error(jsi_error_js);

  if (!resultValue->IsString()) {
    state->setNativeError("BigInt.toString did not return a string");
    return abi::create_string_or_error(jsi_error_native);
  }

  return abi::create_string_or_error(static_cast<jsi_pointer *>(
      new StringHandle(isolate, v8::Local<v8::String>::Cast(resultValue))));
}

//==============================================================================
// Runtime VTable Definition
//==============================================================================

const jsi_runtime_vtable g_vtable = {
    /* query_interface */ jsi_query_interface,
    /* get_abi_version */ jsi_get_abi_version,
    /* add_ref */ jsi_add_ref,
    /* release */ jsi_release,

    /* get_and_clear_js_error_value */ jsi_get_and_clear_js_error_value,
    /* get_and_clear_native_exception_message */
    jsi_get_and_clear_native_exception_message,
    /* set_js_error_value */ jsi_set_js_error_value,
    /* set_native_exception_message */ jsi_set_native_exception_message,

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

    /* create_string_from_utf8 */ jsi_create_string_from_utf8,
    /* create_string_from_utf16 */ jsi_create_string_from_utf16,
    /* get_utf8_from_string */ jsi_get_utf8_from_string,
    /* get_utf16_from_string */ jsi_get_utf16_from_string,

    /* get_utf8_from_symbol */ jsi_get_utf8_from_symbol,

    /* create_propnameid_from_utf8 */ jsi_create_propnameid_from_utf8,
    /* create_propnameid_from_string */ jsi_create_propnameid_from_string,
    /* create_propnameid_from_symbol */ jsi_create_propnameid_from_symbol,
    /* prop_name_id_equals */ jsi_prop_name_id_equals,
    /* get_utf8_from_propnameid */ jsi_get_utf8_from_propnameid,

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
    /* create_error */ jsi_create_error,

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

// Flat runtime-creation export (replaces the dropped jsi_vtable factory).
// requested_abi_version is the consumer's compiled JSI_ABI_VERSION; configure
// (may be NULL) populates a factory-owned jsi_config via the v8_jsi_config_set_*
// setters. Returns NULL on version mismatch or configure failure.
JSI_API jsi_runtime *JSI_CDECL v8_create_runtime(
    uint32_t requested_abi_version,
    jsi_configure_runtime_cb configure,
    void *configure_data) {
  // Model B version negotiation: refuse a version newer than we implement.
  if (requested_abi_version > JSI_ABI_VERSION)
    return nullptr;

  jsi_runtime *rt = nullptr;
  if (configure) {
    // Factory owns the config: allocate it, let the consumer populate it via
    // the setters, consume it, then let it destruct. The handle never escapes
    // the callback. JsiRuntimeState::create clears the cache/task-runner
    // pointers it takes over, so ~jsi_config_s does not double-fire; on a
    // configure abort, ~jsi_config_s fires any registered deleters exactly once.
    jsi_config_s config;
    if (configure(configure_data, &config) != jsi_no_error)
      return nullptr;
    rt = JsiRuntimeState::create(&config);
  } else {
    // No configure callback → engine defaults. Passing nullptr selects the
    // historical default-init path (distinct from a zero-initialized config).
    rt = JsiRuntimeState::create(nullptr);
  }

  if (rt) {
    // Pin the runtime to the negotiated version (reserved for future
    // behavioral branching; get_abi_version reports the impl max).
    static_cast<JsiRuntimeState *>(rt)->abi_version = requested_abi_version;
  }
  return rt;
}

// Process-global V8 engine flags. Raw CLI-style pass-through to V8 — these are
// process-global and consumed only on the first platform init, so they are NOT
// per-runtime config. Must be called before the first v8_create_runtime.
JSI_API void JSI_CDECL v8_jsi_set_v8_flags(
    size_t *argc, char **argv, bool remove_flags) {
  if (!argc || !argv)
    return;
  int n = static_cast<int>(*argc);
  v8::V8::SetFlagsFromCommandLine(&n, argv, remove_flags);
  *argc = n < 0 ? 0u : static_cast<size_t>(n);
}

// W5 inspector entry point. Pure-C replacement for the legacy
// openInspector(jsi::Runtime &) and openInspectors_toberemoved() exports.
// Starts a previously-quiet Agent (e.g. when enable_inspector was set on the
// config but inspector_break_on_start was false). Safe to call multiple times.
JSI_API void JSI_CDECL v8_open_inspector(jsi_runtime *runtime) {
#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
  if (!runtime) return;
  auto *state = static_cast<JsiRuntimeState *>(runtime);
  if (state->inspector_agent) {
    state->inspector_agent->start();
  }
#else
  (void)runtime;
#endif
}

// W4 test-only hook: post a synthetic task to the runtime's foreground task
// runner. Gated behind JSI_TESTING_ONLY (gyp variable v8jsi_test_hooks) so
// release builds can drop it. Not declared in any public header; the test
// forward-declares it inline.
#ifdef JSI_TESTING_ONLY
JSI_API void JSI_CDECL v8_jsi_test_post_foreground_task(
    jsi_runtime *runtime,
    void (JSI_CDECL *task_run_cb)(void *),
    void *task_data) {
  if (!runtime || !task_run_cb)
    return;
  auto *state = static_cast<JsiRuntimeState *>(runtime);
  auto runner = v8rt::IsolateData::fromIsolate(state->isolate)->taskRunner();
  if (!runner)
    return;
  struct InlineTask final : public v8rt::TaskRunner::Task {
    void (JSI_CDECL *cb)(void *);
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

JSI_API void JSI_CDECL
v8_jsi_config_set_initial_heap_size(jsi_config config, size_t bytes) {
  if (config) config->initial_heap_size_in_bytes = bytes;
}

JSI_API void JSI_CDECL
v8_jsi_config_set_maximum_heap_size(jsi_config config, size_t bytes) {
  if (config) config->maximum_heap_size_in_bytes = bytes;
}

JSI_API void JSI_CDECL
v8_jsi_config_set_thread_pool_size(jsi_config config, uint8_t size) {
  if (config) config->thread_pool_size = size;
}

JSI_API void JSI_CDECL
v8_jsi_config_set_debugger_runtime_name(jsi_config config, const char *name) {
  if (config) config->debugger_runtime_name = (name ? name : "");
}

JSI_API void JSI_CDECL
v8_jsi_config_enable_inspector(jsi_config config, bool value) {
  if (config) config->enable_inspector = value;
}

JSI_API void JSI_CDECL
v8_jsi_config_set_inspector_port(jsi_config config, uint16_t port) {
  if (config) config->inspector_port = port;
}

JSI_API void JSI_CDECL
v8_jsi_config_set_inspector_break_on_start(jsi_config config, bool value) {
  if (config) config->inspector_break_on_start = value;
}

JSI_API void JSI_CDECL
v8_jsi_config_set_explicit_microtask_policy(jsi_config config, bool value) {
  if (config) config->explicit_microtask_policy = value;
}

JSI_API void JSI_CDECL
v8_jsi_config_set_ignore_unhandled_promises(jsi_config config, bool value) {
  if (config) config->ignore_unhandled_promises = value;
}

JSI_API void JSI_CDECL
v8_jsi_config_enable_gc_api(jsi_config config, bool value) {
  if (config) config->enable_gc_api = value;
}

JSI_API void JSI_CDECL
v8_jsi_config_enable_multi_thread(jsi_config config, bool value) {
  if (config) config->enable_multi_thread = value;
}

JSI_API void JSI_CDECL
v8_jsi_config_enable_jit_tracing(jsi_config config, bool value) {
  if (config) config->enable_jit_tracing = value;
}

JSI_API void JSI_CDECL
v8_jsi_config_enable_message_tracing(jsi_config config, bool value) {
  if (config) config->enable_message_tracing = value;
}

JSI_API void JSI_CDECL
v8_jsi_config_enable_gc_tracing(jsi_config config, bool value) {
  if (config) config->enable_gc_tracing = value;
}

JSI_API void JSI_CDECL
v8_jsi_config_enable_system_instrumentation(jsi_config config, bool value) {
  if (config) config->enable_system_instrumentation = value;
}

JSI_API void JSI_CDECL v8_jsi_config_set_script_cache(
    jsi_config config,
    void *script_cache_data,
    v8_jsi_script_cache_load_cb load_cb,
    v8_jsi_script_cache_store_cb store_cb,
    jsi_data_delete_cb script_cache_data_delete_cb,
    void *deleter_data) {
  if (!config) {
    // Nothing owns script_cache_data; release it immediately.
    if (script_cache_data_delete_cb)
      script_cache_data_delete_cb(script_cache_data, deleter_data);
    return;
  }
  // Release any previously-set cache before overwriting.
  if (config->script_cache_data_delete_cb) {
    config->script_cache_data_delete_cb(
        config->script_cache_data, config->script_cache_deleter_data);
  }
  config->script_cache_data = script_cache_data;
  config->script_cache_load_cb = load_cb;
  config->script_cache_store_cb = store_cb;
  config->script_cache_data_delete_cb = script_cache_data_delete_cb;
  config->script_cache_deleter_data = deleter_data;
}

JSI_API void JSI_CDECL v8_jsi_config_set_task_runner(
    jsi_config config,
    void *task_runner_data,
    v8_jsi_task_runner_post_task_cb post_task_cb,
    jsi_data_delete_cb task_runner_data_delete_cb,
    void *deleter_data) {
  if (!config) {
    // Nothing owns task_runner_data; release it immediately.
    if (task_runner_data_delete_cb)
      task_runner_data_delete_cb(task_runner_data, deleter_data);
    return;
  }
  // Release any previously-set task runner before overwriting.
  if (config->task_runner_data_delete_cb) {
    config->task_runner_data_delete_cb(
        config->task_runner_data, config->task_runner_deleter_data);
  }
  config->task_runner_data = task_runner_data;
  config->task_runner_post_task_cb = post_task_cb;
  config->task_runner_data_delete_cb = task_runner_data_delete_cb;
  config->task_runner_deleter_data = deleter_data;
}

} // extern "C"
