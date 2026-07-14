// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

/// \file v8_core.h
/// \brief Shared V8 infrastructure for JSI ABI and Node-API implementations.
///
/// This file provides common V8 utilities used by:
/// - jsi_abi_v8.cpp (JSI ABI implementation)
/// - v8_api_abi.cpp (Node-API implementation)
///
/// Design principles:
/// - No JSI dependencies (pure V8)
/// - No C++ exceptions (matches Node.js/V8 style)
/// - Minimal abstractions

#pragma once

#include "v8.h"
#include "libplatform/libplatform.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <functional>
#include <optional>

namespace v8rt {

//==============================================================================
// Forward Declarations
//==============================================================================

struct IsolateData;

//==============================================================================
// Task Runner Interface
//==============================================================================

/// Abstract interface for posting tasks to the JavaScript thread.
/// Used for foreground task execution (microtasks, immediate callbacks, etc.)
class TaskRunner {
 public:
  virtual ~TaskRunner() = default;

  /// A task that can be run on the JS thread.
  class Task {
   public:
    virtual ~Task() = default;
    virtual void run() = 0;
  };

  /// Post a task to be executed on the JS thread.
  virtual void postTask(std::unique_ptr<Task> task) = 0;
};

//==============================================================================
// V8 Platform Management
//==============================================================================

/// Manages the V8 platform singleton.
/// Ensures the platform is initialized and disposed only once.
/// Thread-safe.
///
/// The platform must be initialized before any V8 isolate is created.
/// The platform must be disposed before process exit to avoid random
/// failures from tasks still in the delayed task queue.
class V8PlatformHolder {
 public:
  /// Initialize the V8 platform.
  /// \param thread_pool_size 0 for default (V8 uses min(N-1, 16) where N = cores)
  /// \param init_callback Called once during first initialization for custom setup
  template <typename InitCallback>
  static void initializePlatform(int thread_pool_size, InitCallback&& init_callback) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (is_initialized_) {
      return;
    }
    is_initialized_ = true;
    init_callback();
    platform_ = v8::platform::NewDefaultPlatform(thread_pool_size);
    v8::V8::InitializePlatform(platform_.get());
    v8::V8::Initialize();
  }

  /// Dispose the V8 platform.
  /// Should be called before process exit.
  static void disposePlatform() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!is_initialized_ || is_disposed_) {
      return;
    }
    is_disposed_ = true;
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
    platform_ = nullptr;
  }

  /// Check if the platform is initialized.
  static bool isInitialized() {
    std::lock_guard<std::mutex> guard(mutex_);
    return is_initialized_ && !is_disposed_;
  }

  V8PlatformHolder() = delete;
  V8PlatformHolder(const V8PlatformHolder&) = delete;
  V8PlatformHolder& operator=(const V8PlatformHolder&) = delete;

 private:
  static std::mutex mutex_;
  static std::unique_ptr<v8::Platform> platform_;
  static bool is_initialized_;
  static bool is_disposed_;
};

//==============================================================================
// Isolate Data Slots
//==============================================================================

/// Well-known slots for data associated with a V8 isolate.
constexpr int kIsolateDataSlot = 0;
constexpr int kIsolateInspectorSlot = 1;

//==============================================================================
// Per-Isolate Data
//==============================================================================

/// Data associated with each V8 isolate.
/// Stored in the isolate's embedder slot.
struct IsolateData {
  /// Create IsolateData for an isolate.
  /// \param isolate The V8 isolate (must not be null)
  /// \param task_runner Optional task runner for foreground tasks
  IsolateData(v8::Isolate* isolate, std::shared_ptr<TaskRunner> task_runner = nullptr) noexcept
      : foreground_task_runner_(std::move(task_runner)), isolate_(isolate) {}

  /// Get the isolate this data is associated with.
  v8::Isolate* isolate() const noexcept { return isolate_; }

  /// Get the foreground task runner.
  std::shared_ptr<TaskRunner> taskRunner() const noexcept { return foreground_task_runner_; }

  //----------------------------------------------------------------------------
  // Private Property Keys
  //----------------------------------------------------------------------------

  /// Get or create the private key for Node-API type tags.
  v8::Local<v8::Private> napi_type_tag() {
    return getOrCreatePrivateKey(napi_type_tag_, "node:napi:type_tag");
  }

  /// Get or create the private key for Node-API wrappers.
  v8::Local<v8::Private> napi_wrapper() {
    return getOrCreatePrivateKey(napi_wrapper_, "node:napi:wrapper");
  }

  /// Get or create the private key for native state.
  v8::Local<v8::Private> nativeStateKey() {
    return getOrCreatePrivateKey(native_state_key_, "v8rt:nativeState");
  }

  /// Get or create the private key for host object proxies.
  v8::Local<v8::Private> hostObjectKey() {
    return getOrCreatePrivateKey(host_object_key_, "v8rt:hostObject");
  }

  //----------------------------------------------------------------------------
  // Isolate Data Accessors
  //----------------------------------------------------------------------------

  /// Get IsolateData from an isolate.
  /// Returns nullptr if no IsolateData is associated.
  static IsolateData* fromIsolate(v8::Isolate* isolate) noexcept {
    return static_cast<IsolateData*>(isolate->GetData(kIsolateDataSlot));
  }

  /// Associate this IsolateData with its isolate.
  void attachToIsolate() noexcept {
    isolate_->SetData(kIsolateDataSlot, this);
  }

  /// Public access to foreground task runner (for backwards compatibility)
  std::shared_ptr<TaskRunner> foreground_task_runner_;

  /// Descriptor for an optional startup-snapshot blob. V8 retains a pointer to
  /// this struct (via Isolate::CreateParams::snapshot_blob) and deserializes
  /// contexts from it LAZILY — so it must outlive Isolate::Initialize and live
  /// for the isolate's lifetime. Holding it here (created before Initialize)
  /// gives it exactly that lifetime. The .data bytes are owned elsewhere
  /// (the runtime), kept alive at least as long as this IsolateData.
  v8::StartupData snapshot_blob_{nullptr, 0};

 private:
  v8::Local<v8::Private> getOrCreatePrivateKey(
      v8::Eternal<v8::Private>& key,
      const char* name) {
    if (key.IsEmpty()) {
      v8::Local<v8::String> key_name = v8::String::NewFromUtf8(
          isolate_, name, v8::NewStringType::kInternalized).ToLocalChecked();
      key.Set(isolate_, v8::Private::New(isolate_, key_name));
    }
    return key.Get(isolate_);
  }

  // Note: isolate_ is declared after foreground_task_runner_ to match init order
  v8::Isolate* isolate_;
  v8::Eternal<v8::Private> napi_type_tag_;
  v8::Eternal<v8::Private> napi_wrapper_;
  v8::Eternal<v8::Private> native_state_key_;
  v8::Eternal<v8::Private> host_object_key_;
};

//==============================================================================
// RAII Scope Helpers
//==============================================================================

/// RAII helper for V8 isolate scope.
/// Does NOT throw exceptions.
class IsolateScope {
 public:
  explicit IsolateScope(v8::Isolate* isolate) noexcept
      : isolate_scope_(isolate) {}

 private:
  v8::Isolate::Scope isolate_scope_;
};

/// RAII helper for V8 handle scope.
/// Does NOT throw exceptions.
class HandleScope {
 public:
  explicit HandleScope(v8::Isolate* isolate) noexcept
      : handle_scope_(isolate) {}

 private:
  v8::HandleScope handle_scope_;
};

/// RAII helper for V8 context scope.
/// Does NOT throw exceptions.
class ContextScope {
 public:
  explicit ContextScope(v8::Local<v8::Context> context) noexcept
      : context_scope_(context) {}

 private:
  v8::Context::Scope context_scope_;
};

/// Combined RAII helper for isolate + handle + context scopes.
/// Useful for most V8 API calls.
class V8Scope {
 public:
  V8Scope(v8::Isolate* isolate, v8::Local<v8::Context> context) noexcept
      : isolate_scope_(isolate),
        handle_scope_(isolate),
        context_scope_(context) {}

 private:
  v8::Isolate::Scope isolate_scope_;
  v8::HandleScope handle_scope_;
  v8::Context::Scope context_scope_;
};

/// RAII wrapper for multi-threaded V8 isolate access. Acquires v8::Locker
/// (when enabled), then enters Isolate::Scope, HandleScope, Context::Scope.
/// Order matters: Locker must be acquired first when the isolate is shared
/// across threads, then the HandleScope must be live before any Local is
/// derived from a Global<Context> for the Context::Scope.
///
/// The Global<Context> overload exists because callers that derive a Local
/// at the call site would do so without a HandleScope of their own (the
/// one inside the locker is not yet built). Passing the Global lets the
/// locker derive the Local under its own HandleScope.
///
/// Migrated from V8Runtime::IsolateLocker. Used by Node-API's
/// NodeApiIsolateLocker which extends it with TLS-current tracking.
class IsolateLocker {
 public:
  IsolateLocker(v8::Isolate* isolate,
                const v8::Global<v8::Context>& context,
                bool enable_multi_thread) noexcept
      : locker_(makeOptionalLocker(isolate, enable_multi_thread)),
        isolate_scope_(isolate),
        handle_scope_(isolate),
        context_scope_(context.Get(isolate)) {}

 protected:
  static std::optional<v8::Locker> makeOptionalLocker(
      v8::Isolate* isolate, bool enabled) noexcept {
    if (enabled) {
      return std::make_optional<v8::Locker>(isolate);
    }
    return std::nullopt;
  }

  std::optional<v8::Locker> locker_;
  v8::Isolate::Scope isolate_scope_;
  v8::HandleScope handle_scope_;
  v8::Context::Scope context_scope_;
};

//==============================================================================
// TryCatch Wrapper
//==============================================================================

/// Wrapper around v8::TryCatch that stores caught exceptions for later retrieval.
/// Similar to Node-API's v8impl::TryCatch.
/// Does NOT use C++ exceptions.
class TryCatch {
 public:
  explicit TryCatch(v8::Isolate* isolate) noexcept
      : try_catch_(isolate) {}

  /// Check if a JavaScript exception was caught.
  bool hasCaught() const noexcept { return try_catch_.HasCaught(); }

  /// Get the caught exception value.
  v8::Local<v8::Value> exception() const { return try_catch_.Exception(); }

  /// Get the exception message.
  v8::Local<v8::Message> message() const { return try_catch_.Message(); }

  /// Reset the TryCatch state.
  void reset() { try_catch_.Reset(); }

  /// Rethrow the caught exception (if any).
  void rethrow() { try_catch_.ReThrow(); }

 private:
  v8::TryCatch try_catch_;
};

//==============================================================================
// Isolate Creation
//==============================================================================

/// Configuration for creating a new V8 isolate.
struct IsolateConfig {
  /// Initial heap size in bytes (0 for default).
  size_t initial_heap_size = 0;

  /// Maximum heap size in bytes (0 for default).
  size_t maximum_heap_size = 0;

  /// Task runner for foreground tasks.
  std::shared_ptr<TaskRunner> task_runner;

  /// Whether to track GC object statistics.
  bool track_gc_object_stats = false;

  /// Whether to expose the gc() function.
  bool enable_gc_api = false;

  /// Microtask policy.
  v8::MicrotasksPolicy microtasks_policy = v8::MicrotasksPolicy::kAuto;

  /// Optional V8 startup-snapshot blob (borrowed; caller keeps it alive for the
  /// isolate's lifetime). When non-null, the isolate is created from it instead
  /// of the built-in startup data, pre-materializing the embedded script's heap.
  const uint8_t* startup_snapshot_blob = nullptr;
  size_t startup_snapshot_blob_size = 0;
};

/// Create a new V8 isolate with the given configuration.
/// The caller owns the returned isolate and must dispose it.
/// Returns nullptr on failure.
v8::Isolate* createIsolate(const IsolateConfig& config);

/// Create a new V8 context in the given isolate.
/// The isolate must be entered (have an active Isolate::Scope).
/// When \p from_snapshot is true the isolate was created from a custom startup
/// snapshot; the context is cloned from the snapshot's default context (no
/// global template) so the baked-in globalThis state is restored.
v8::Local<v8::Context> createContext(v8::Isolate* isolate, bool from_snapshot = false);

//==============================================================================
// Context Embedder Data Indices
//==============================================================================

/// Well-known embedder data indices for V8 contexts.
struct ContextEmbedderIndex {
  /// Slot 0 — back-pointer from a context to the JSI ABI runtime that owns it.
  /// Set by JsiRuntimeState::create. Read by static callbacks (e.g.
  /// PromiseRejectCallback) that only have a context handle and need to find
  /// the owning runtime.
  static constexpr int kRuntime = 0;

  /// Slot 1 — RuntimeContextTagPtr (a stable per-process address). Used as a
  /// "this is a v8jsi context" marker so upstream Node.js Environment::GetCurrent
  /// patterns recognize it.
  static constexpr int kContextTag = 1;
};

/// A stable per-process address used to mark v8jsi-created contexts. Set into
/// ContextEmbedderIndex::kContextTag of every context created by
/// JsiRuntimeState::create. Has no in-tree readers today; preserved for
/// upstream Node.js Environment::GetCurrent compatibility.
extern int const RuntimeContextTag;
extern void* const RuntimeContextTagPtr;

//==============================================================================
// Unhandled Promise Tracking
//==============================================================================

/// Captured by the V8 PromiseRejectCallback when a promise is rejected with no
/// handler. Stored on JsiRuntimeState; read by Node-API's
/// hasUnhandledPromiseRejection / getAndClearLastUnhandledPromiseRejection.
/// Pure V8 types — no JSI dependency.
struct UnhandledPromiseRejection {
  v8::Global<v8::Promise> promise;
  v8::Global<v8::Message> message;
  v8::Global<v8::Value> value;
};

}  // namespace v8rt
