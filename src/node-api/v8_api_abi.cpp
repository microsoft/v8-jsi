// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// ----------------------------------------------------------------------------
// Some code is copied from Node.js project to compile V8 NAPI code
// without major changes.
// ----------------------------------------------------------------------------
// Original Node.js copyright:
//
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#define NAPI_EXPERIMENTAL
#include "env-inl.h"

// Include the public Node-API headers first so NAPI_EXTERN, NAPI_VERSION_*,
// napi_key_*, napi_type_tag etc. are defined before js_native_api_v8.h pulls
// in js_native_api_v8_internals.h.
#include "js_native_api.h"
#include "v8_node_api.h"

#include "js_native_api_v8.h"

#include "MurmurHash.h"
#include "V8Instrumentation.h"
#include "jsi_abi/jsi_abi_v8_internal.h"
#include "jsi_abi/v8_jsi_abi.h"
#include "jsi_abi/v8_node_api_attach.h"
#include "v8_core.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#define CHECKED_ENV(env)                                                       \
  ((env) == nullptr)                                                           \
      ? napi_invalid_arg                                                       \
      : static_cast<v8impl::NodeApiEnv*>(reinterpret_cast<napi_env>(env))

#define CHECKED_RUNTIME(runtime)                                               \
  ((runtime) == nullptr) ? napi_generic_failure                                \
                         : reinterpret_cast<v8impl::RuntimeWrapper*>(runtime)

#define CHECKED_CONFIG(config)                                                 \
  ((config) == nullptr) ? napi_generic_failure                                 \
                        : reinterpret_cast<v8impl::ConfigWrapper*>(config)

#define V8_CHECK_ARG(arg)                                                      \
  if ((arg) == nullptr) {                                                      \
    return napi_generic_failure;                                               \
  }

namespace v8impl {

class NodeApiEnv;

//============================================================================
// V8RuntimeEnv ΓÇö Node-API surface attached to a jsi_runtime.
//
// replaces the legacy `class V8RuntimeEnv : public v8runtime::V8Runtime`.
// Composition over a jsi_runtime *. All V8-state accessors (isolate,
// context, script cache, unhandled-promise tracking) reach into the
// underlying JsiRuntimeState via the v8rt_internal:: free functions.
//
// One V8RuntimeEnv per attached napi surface. Owns a list of NodeApiEnvs
// (one root + zero or more module envs created via createNodeApi). Lifetime:
// the V8RuntimeEnv is destroyed before V8 state via the attached-owner hook
// on JsiRuntimeState; on destruction it Unrefs every env it owns.
//============================================================================
class V8RuntimeEnv {
 public:
  explicit V8RuntimeEnv(jsi_runtime *abiRuntime)
      : abiRuntime_(abiRuntime),
        instrumentation_(std::make_unique<v8runtime::V8Instrumentation>(
            v8rt_internal::getIsolate(abiRuntime))) {
    // Create the root env after the runtime fields are initialized.
    rootEnv_ = createNodeApi(NAPI_VERSION_EXPERIMENTAL);
  }

  ~V8RuntimeEnv() {
    // Drive the root env's refcount to 0. Its DeleteMe ΓåÆ unlinks itself
    // from moduleEnvList_ and frees the env. Iterate any remaining module
    // envs and Unref them too ΓÇö typical case is none, but a misbehaving
    // consumer might have leaked one.
    NodeApiEnv *root = rootEnv_;
    rootEnv_ = nullptr;
    if (root) {
      Unref(root);
    }
    // Module envs that survived (unusual). Take a snapshot since Unref ΓåÆ
    // DeleteMe mutates the list.
    std::vector<NodeApiEnv*> remaining = std::move(moduleEnvList_);
    for (NodeApiEnv *env : remaining) {
      Unref(env);
    }
  }

  V8RuntimeEnv(const V8RuntimeEnv &) = delete;
  V8RuntimeEnv &operator=(const V8RuntimeEnv &) = delete;

  jsi_runtime *abiRuntime() const noexcept { return abiRuntime_; }

  v8::Isolate *GetIsolate() const noexcept {
    return v8rt_internal::getIsolate(abiRuntime_);
  }

  v8::Global<v8::Context> &GetContext() noexcept {
    return v8rt_internal::getContext(abiRuntime_);
  }

  v8::Local<v8::Context> GetContextLocal() const noexcept {
    return v8rt_internal::getContextLocal(abiRuntime_);
  }

  bool getEnableMultiThread() const noexcept {
    return v8rt_internal::getEnableMultiThread(abiRuntime_);
  }

  bool isInspectable() const noexcept { return true; }

  bool HasUnhandledPromiseRejection() noexcept {
    return v8rt_internal::hasUnhandledPromiseRejection(abiRuntime_);
  }

  std::unique_ptr<v8rt::UnhandledPromiseRejection>
  GetAndClearLastUnhandledPromiseRejection() noexcept {
    return v8rt_internal::takeLastUnhandledPromiseRejection(abiRuntime_);
  }

  facebook::jsi::Instrumentation &instrumentation() noexcept {
    return *instrumentation_;
  }

  napi_status getRootNodeApi(napi_env *env);
  NodeApiEnv *createNodeApi(int32_t apiVersion);
  void removeModuleEnv(NodeApiEnv *env);

  void SetImmediate(std::function<void()> callback) {
    auto runner = v8rt::IsolateData::fromIsolate(GetIsolate())->taskRunner();
    if (!runner) return;
    runner->postTask(std::make_unique<ImmediateTask>(std::move(callback)));
  }

  class ImmediateTask : public v8rt::TaskRunner::Task {
   public:
    explicit ImmediateTask(std::function<void()> task)
        : task_(std::move(task)) {}
    void run() override { task_(); }

   private:
    std::function<void()> task_;
  };

  // RAII Locker for Node-API operations. Extends v8rt::IsolateLocker with
  // TLS-current tracking + AddRef/Release semantics for nested env-scope
  // opens on the same thread.
  class NodeApiIsolateLocker : public v8rt::IsolateLocker {
   public:
    explicit NodeApiIsolateLocker(V8RuntimeEnv *env)
        : v8rt::IsolateLocker(env->GetIsolate(),
                               env->GetContext(),
                               env->getEnableMultiThread()),
          env_(env),
          previous_(tls_current_) {
      tls_current_ = this;
    }

    static NodeApiIsolateLocker *Current() noexcept { return tls_current_; }

    static bool HasCurrentEnv(const V8RuntimeEnv *env) noexcept {
      return tls_current_ != nullptr && tls_current_->env_ == env;
    }

    void AddRef() noexcept { ++refCount_; }

    void Release() {
      if (--refCount_ == 0) delete this;
    }

   private:
    ~NodeApiIsolateLocker() { tls_current_ = previous_; }

    const V8RuntimeEnv *env_;
    std::atomic<int32_t> refCount_{1};
    NodeApiIsolateLocker *previous_;
    static inline thread_local NodeApiIsolateLocker *tls_current_{};
  };

  // Internal Unref helper that does not assume NodeApiEnv layout.
  static void Unref(NodeApiEnv *env);

 private:
  jsi_runtime *abiRuntime_;
  std::unique_ptr<v8runtime::V8Instrumentation> instrumentation_;
  std::vector<NodeApiEnv*> moduleEnvList_;
  NodeApiEnv *rootEnv_{nullptr};
};

//============================================================================
// NodeApiPreparedScript ΓÇö Node-API's opaque v8_node_api_prepared_script payload.
//
// was a heap-allocated shared_ptr<facebook::jsi::PreparedJavaScript> in
// the legacy code (because V8Runtime::prepareJavaScript2 returned that
// type). Now the script-cache plumbing happens directly in V8RuntimeEnv;
// the prepared-script payload is just a v8::Global<v8::UnboundScript> the
// consumer holds onto for later runs.
//============================================================================
struct NodeApiPreparedScript {
  v8::Global<v8::UnboundScript> unbound;
};

//============================================================================
// NodeApiEnv ΓÇö concrete napi_env implementation backed by V8RuntimeEnv.
//
// composition over V8RuntimeEnv (no V8Runtime inheritance). The
// destruction flow is owned by V8RuntimeEnv: its destructor Unrefs the
// root env which cascades through any module envs. NodeApiEnv::DeleteMe
// no longer deletes the runtime ΓÇö V8RuntimeEnv lifetime is managed by the
// JsiRuntimeState attached-owner hook.
//============================================================================
class NodeApiEnv : public napi_env__ {
 public:
  NodeApiEnv(V8RuntimeEnv *runtime, int32_t apiVersion)
      : napi_env__(runtime->GetIsolate(),
                    runtime->GetContext(),
                    apiVersion),
        m_runtime(runtime) {}

  ~NodeApiEnv() override {}

  V8RuntimeEnv *runtime() const noexcept { return m_runtime; }

  void DetachFromRuntime() noexcept { m_runtime = nullptr; }

  void DeleteMe() override {
    m_isDestructing = true;
    if (m_runtime) {
      m_runtime->removeModuleEnv(this);
      m_runtime = nullptr;
    }
    DrainFinalizerQueue();
    napi_env__::DeleteMe();
  }

  void EnqueueFinalizer(v8impl::RefTracker* finalizer) override {
    napi_env__::EnqueueFinalizer(finalizer);
    // Schedule a second pass only when it has not been scheduled, and not
    // destructing the env.
    if (!m_isFinalizationScheduled && !m_isDestructing && m_runtime) {
      m_isFinalizationScheduled = true;
      Ref();
      m_runtime->SetImmediate([this]() {
        m_isFinalizationScheduled = false;
        Unref();
        DrainFinalizerQueue();
      });
    }
  }

  void DrainFinalizerQueue() {
    while (!pending_finalizers.empty()) {
      v8impl::RefTracker* ref_tracker = *pending_finalizers.begin();
      pending_finalizers.erase(ref_tracker);
      ref_tracker->Finalize();
    }
  }

  bool can_call_into_js() const override { return !m_isDestructing; }

  void CallFinalizer(napi_finalize cb, void* data, void* hint) override {
    if (in_gc_finalizer) {
      cb(env, data, hint);
      return;
    }
    // Finalizers may run during runtime teardown (after the isolate has been
    // left by higher-level scopes). Context::Enter calls GetIsolate() which
    // DCHECKs heap->isolate() == Isolate::TryGetCurrent() — fires (then hangs
    // V8_Fatal's stderr writer) if no Isolate::Scope is active here.
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Context::Scope context_scope(context());
    CallIntoModule([&](napi_env env) { cb(env, data, hint); },
                   [](napi_env env, v8::Local<v8::Value> local_err) {
                     NodeApiEnv* runtimeEnv = static_cast<NodeApiEnv*>(env);
                     if (env->terminatedOrTerminating()) {
                       return;
                     }
                     runtimeEnv->TriggerFatalException(local_err);
                   });
  }

  static void PrintToStderrAndFlush(const std::string& str) {
    fprintf(stderr, "%s\n", str.c_str());
    fflush(stderr);
  }

  void TriggerFatalException(v8::Local<v8::Value> err) {
    v8::String::Utf8Value reason(
        isolate,
        err->ToDetailString(context()).FromMaybe(v8::Local<v8::String>()));
    std::string reason_str(*reason, reason.length());
    PrintToStderrAndFlush(reason_str + "\n");
    _exit(static_cast<int>(node::ExitCode::kAbort));
  }

  napi_status collectGarbage() {
    isolate->RequestGarbageCollectionForTesting(
        v8::Isolate::kFullGarbageCollection);
    return napi_status::napi_ok;
  }

  napi_status hasUnhandledPromiseRejection(bool* result) {
    CHECK_ARG(env, result);
    *result = m_runtime->HasUnhandledPromiseRejection();
    return napi_ok;
  }

  napi_status getDescription(const char** result) noexcept {
    CHECK_ARG(env, result);
    *result = "V8";
    return napi_ok;
  }

  napi_status queueMicrotask(napi_value callback) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, callback);
    v8::Local<v8::Value> value =
        v8impl::V8LocalValueFromJsValue(callback);
    if (!value->IsFunction()) {
      return napi_set_last_error(env, napi_function_expected);
    }
    isolate->EnqueueMicrotask(value.As<v8::Function>());
    return GET_RETURN_STATUS(env);
  }

  napi_status drainMicrotasks(int32_t /*maxCountHint*/, bool* result) {
    if (result) {
      *result = true;
    }
    return napi_ok;
  }

  napi_status isInspectable(bool* result) noexcept {
    CHECK_ARG(env, result);
    *result = m_runtime->isInspectable();
    return napi_ok;
  }

  napi_status openEnvScope(v8_node_api_env_scope* scope) {
    CHECK_ARG(env, scope);
    if (V8RuntimeEnv::NodeApiIsolateLocker::HasCurrentEnv(m_runtime)) {
      V8RuntimeEnv::NodeApiIsolateLocker::Current()->AddRef();
    } else {
      ::new V8RuntimeEnv::NodeApiIsolateLocker(m_runtime);
    }
    *scope = reinterpret_cast<v8_node_api_env_scope>(
        V8RuntimeEnv::NodeApiIsolateLocker::Current());
    return napi_ok;
  }

  napi_status closeEnvScope(v8_node_api_env_scope scope) {
    CHECK_ARG(env, scope);
    V8RuntimeEnv::NodeApiIsolateLocker* locker =
        reinterpret_cast<V8RuntimeEnv::NodeApiIsolateLocker*>(scope);
    if (locker != V8RuntimeEnv::NodeApiIsolateLocker::Current()) {
      return napi_generic_failure;
    }
    locker->Release();
    return napi_ok;
  }

  napi_status getAndClearLastUnhandledPromiseRejection(napi_value* result) {
    CHECK_ARG(env, result);
    auto rejectionInfo = m_runtime->GetAndClearLastUnhandledPromiseRejection();
    if (!rejectionInfo) {
      *result = v8impl::JsValueFromV8LocalValue(v8::Undefined(isolate));
      return napi_ok;
    }
    *result =
        v8impl::JsValueFromV8LocalValue(rejectionInfo->value.Get(isolate));
    return napi_ok;
  }

  napi_status runScript(napi_value source,
                        const char* source_url,
                        napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, source);
    CHECK_ARG(env, result);

    v8::Local<v8::Value> v8_source = v8impl::V8LocalValueFromJsValue(source);
    if (!v8_source->IsString()) {
      return napi_set_last_error(env, napi_string_expected);
    }

    v8::Local<v8::Context> context = env->context();

    const char* checked_source_url = source_url ? source_url : "";
    v8::Local<v8::String> urlV8String =
        v8::String::NewFromUtf8(context->GetIsolate(), checked_source_url)
            .ToLocalChecked();
    v8::ScriptOrigin origin(urlV8String);

    auto maybe_script = v8::Script::Compile(
        context, v8::Local<v8::String>::Cast(v8_source), &origin);
    CHECK_MAYBE_EMPTY(env, maybe_script, napi_generic_failure);

    auto script_result = maybe_script.ToLocalChecked()->Run(context);
    CHECK_MAYBE_EMPTY(env, script_result, napi_generic_failure);

    *result = v8impl::JsValueFromV8LocalValue(script_result.ToLocalChecked());
    return GET_RETURN_STATUS(env);
  }

  // prepare/run prepared scripts directly via V8 + the runtime's
  // script-cache callbacks. Replaces the legacy V8Runtime::prepareJavaScript2
  // / evaluatePreparedJavaScript2 round-trip through facebook::jsi types.
  napi_status createPreparedScript(const uint8_t* scriptData,
                                   size_t scriptLength,
                                   v8_node_api_data_delete_cb scriptDeleteCallback,
                                   void* deleterData,
                                   const char* sourceUrl,
                                   v8_node_api_prepared_script* result) {
    struct ScriptDataOwner {
      ~ScriptDataOwner() {
        if (callback) {
          callback(deleteData, const_cast<uint8_t*>(data));
        }
      }

      const uint8_t* data;
      v8_node_api_data_delete_cb callback;
      void* deleteData;
    } scriptDataOwner{scriptData, scriptDeleteCallback, deleterData};

    NAPI_PREAMBLE(env);
    CHECK_ARG(env, scriptData);
    CHECK_ARG(env, sourceUrl);
    CHECK_ARG(env, result);

    auto cache = v8rt_internal::getScriptCacheCallbacks(m_runtime->abiRuntime());

    uint64_t source_hash = 0;
    if (cache.load_cb || cache.store_cb) {
      murmurhash(scriptData, scriptLength, source_hash);
    }

    v8::Local<v8::String> sourceV8String =
        v8::String::NewFromUtf8(isolate,
                                 reinterpret_cast<const char*>(scriptData),
                                 v8::NewStringType::kNormal,
                                 static_cast<int>(scriptLength))
            .ToLocalChecked();

    v8::Local<v8::String> urlV8String =
        v8::String::NewFromUtf8(isolate, sourceUrl).ToLocalChecked();
    v8::ScriptOrigin origin(urlV8String);

    static constexpr const char* kCacheTag = "perf";
    static constexpr const char* kRuntimeName = "V8";
    const uint64_t runtime_version = v8::ScriptCompiler::CachedDataVersionTag();

    v8::ScriptCompiler::CompileOptions options =
        v8::ScriptCompiler::CompileOptions::kNoCompileOptions;
    v8::ScriptCompiler::CachedData* cached_data = nullptr;
    const uint8_t* cache_buffer = nullptr;
    size_t cache_buffer_size = 0;
    jsi_data_delete_cb cache_buffer_delete_cb = nullptr;
    void* cache_buffer_delete_data = nullptr;

    if (cache.load_cb) {
      cache.load_cb(cache.data,
                     sourceUrl,
                     source_hash,
                     kRuntimeName,
                     runtime_version,
                     kCacheTag,
                     &cache_buffer,
                     &cache_buffer_size,
                     &cache_buffer_delete_cb,
                     &cache_buffer_delete_data);
      if (cache_buffer && cache_buffer_size > 0) {
        cached_data = new v8::ScriptCompiler::CachedData(
            cache_buffer, static_cast<int>(cache_buffer_size));
        options = v8::ScriptCompiler::CompileOptions::kConsumeCodeCache;
      }
    }

    v8::ScriptCompiler::Source script_source(sourceV8String, origin,
                                              cached_data);

    v8::Local<v8::Script> compiled;
    bool compile_ok =
        v8::ScriptCompiler::Compile(env->context(), &script_source, options)
            .ToLocal(&compiled);

    if (cache_buffer_delete_cb) {
      cache_buffer_delete_cb(cache_buffer_delete_data,
                             const_cast<uint8_t*>(cache_buffer));
    }

    CHECK_MAYBE_EMPTY(env, v8::MaybeLocal<v8::Script>(
                              compile_ok ? compiled : v8::Local<v8::Script>()),
                       napi_generic_failure);
    if (!compile_ok) return GET_RETURN_STATUS(env);

    v8::Local<v8::UnboundScript> unbound = compiled->GetUnboundScript();

    if (cache.store_cb &&
        options != v8::ScriptCompiler::CompileOptions::kConsumeCodeCache) {
      v8::ScriptCompiler::CachedData* codeCache =
          v8::ScriptCompiler::CreateCodeCache(unbound);
      if (codeCache) {
        // Stateless lambda ΓåÆ function pointer via unary +; assign through a
        // typed function-pointer variable so the conversion is well-formed.
        jsi_data_delete_cb delete_cb = +[](void* delete_data,
                                             void* /*data*/) {
          delete static_cast<v8::ScriptCompiler::CachedData*>(delete_data);
        };
        cache.store_cb(cache.data,
                        sourceUrl,
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

    auto* prepared = new NodeApiPreparedScript();
    prepared->unbound.Reset(isolate, unbound);
    *result = reinterpret_cast<v8_node_api_prepared_script>(prepared);
    return GET_RETURN_STATUS(env);
  }

  napi_status deletePreparedScript(v8_node_api_prepared_script preparedScript) {
    CHECK_ARG(env, preparedScript);
    delete reinterpret_cast<NodeApiPreparedScript*>(preparedScript);
    return napi_clear_last_error(env);
  }

  napi_status runPreparedScript(v8_node_api_prepared_script preparedScript,
                                napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, preparedScript);
    CHECK_ARG(env, result);

    auto* prepared = reinterpret_cast<NodeApiPreparedScript*>(preparedScript);
    v8::Local<v8::UnboundScript> unbound = prepared->unbound.Get(isolate);
    v8::Local<v8::Script> script = unbound->BindToCurrentContext();

    auto script_result = script->Run(env->context());
    CHECK_MAYBE_EMPTY(env, script_result, napi_generic_failure);

    *result = v8impl::JsValueFromV8LocalValue(script_result.ToLocalChecked());
    return GET_RETURN_STATUS(env);
  }

  napi_status createNodeApi(int32_t apiVersion, napi_env* env_out) {
    *env_out = m_runtime->createNodeApi(apiVersion);
    return napi_ok;
  }

  napi_status runTask(v8_node_api_task_run_cb task_cb, void* data) {
    CallIntoModule([task_cb, data](napi_env env) { task_cb(data); });
    return napi_ok;
  }

  napi_status instrumentationGetGCStats(v8_node_api_utf8_output_cb cb, void* cb_ctx) {
    CHECK_ARG(env, cb);
    facebook::jsi::Instrumentation& instrumentation =
        m_runtime->instrumentation();
    std::string stats = instrumentation.getRecordedGCStats();
    cb(cb_ctx, stats.data(), stats.size());
    return napi_ok;
  }

  napi_status instrumentationGetHeapInfo(bool include_expensive,
                                         v8_node_api_heap_info_cb cb,
                                         void* cb_ctx) {
    CHECK_ARG(env, cb);
    facebook::jsi::Instrumentation& instrumentation =
        m_runtime->instrumentation();
    std::unordered_map<std::string, int64_t> heap_info =
        instrumentation.getHeapInfo(include_expensive);
    for (const auto& entry : heap_info) {
      cb(cb_ctx, entry.first.c_str(), entry.second);
    }
    return napi_ok;
  }

  napi_status instrumentationCollectGarbage(const char* cause) {
    facebook::jsi::Instrumentation& instrumentation =
        m_runtime->instrumentation();
    instrumentation.collectGarbage(cause);
    return napi_ok;
  }

  napi_status instrumentationStartHeapSampling(size_t sampling_interval) {
    facebook::jsi::Instrumentation& instrumentation =
        m_runtime->instrumentation();
    instrumentation.startHeapSampling(sampling_interval);
    return napi_ok;
  }

  napi_status instrumentationStopHeapSampling(v8_node_api_utf8_output_cb cb,
                                              void* cb_ctx) {
    facebook::jsi::Instrumentation& instrumentation =
        m_runtime->instrumentation();
    std::stringstream stream;
    instrumentation.stopHeapSampling(stream);
    std::string out = stream.str();
    cb(cb_ctx, out.data(), out.size());
    return napi_ok;
  }

  napi_status instrumentationCreateHeapSnapshot(bool capture_numeric_value,
                                                v8_node_api_utf8_output_cb cb,
                                                void* cb_ctx) {
    CHECK_ARG(env, cb);
    facebook::jsi::Instrumentation& instrumentation =
        m_runtime->instrumentation();
    std::stringstream stream;
#if JSI_VERSION >= 13
    facebook::jsi::Instrumentation::HeapSnapshotOptions options;
    options.captureNumericValue = capture_numeric_value;
    instrumentation.createSnapshotToStream(stream, options);
#else
    instrumentation.createSnapshotToStream(stream);
#endif
    std::string out = stream.str();
    cb(cb_ctx, out.data(), out.size());
    return napi_ok;
  }

 private:
  V8RuntimeEnv* m_runtime;
  napi_env env{this};
  bool m_isDestructing{};
  bool m_isFinalizationScheduled{};
};

// V8RuntimeEnv method bodies that need NodeApiEnv to be a complete type.

NodeApiEnv* V8RuntimeEnv::createNodeApi(int32_t apiVersion) {
  NodeApiEnv* env = new NodeApiEnv(this, apiVersion);
  moduleEnvList_.push_back(env);
  return env;
}

napi_status V8RuntimeEnv::getRootNodeApi(napi_env* env) {
  *env = rootEnv_;
  return napi_ok;
}

void V8RuntimeEnv::removeModuleEnv(NodeApiEnv* env) {
  auto it = std::find(moduleEnvList_.begin(), moduleEnvList_.end(), env);
  if (it != moduleEnvList_.end()) {
    moduleEnvList_.erase(it);
  }
  if (rootEnv_ == env) {
    rootEnv_ = nullptr;
  }
}

void V8RuntimeEnv::Unref(NodeApiEnv* env) {
  env->Unref();
}

//============================================================================
// RuntimeWrapper ΓÇö backs the public v8_node_api_runtime opaque type.
//
// Owns a jsi_runtime (created via v8_jsi_abi_runtime_create) and the V8RuntimeEnv
// attached on top via v8_attach_node_api. The V8RuntimeEnv lifetime is
// managed via JsiRuntimeState's attached-owner hook, so ~RuntimeWrapper
// just calls jsi_release on the abi runtime and the cascade tears down
// V8RuntimeEnv ΓåÆ root napi_env ΓåÆ ~JsiRuntimeState ΓåÆ V8 isolate.
//============================================================================
class RuntimeWrapper {
 public:
  RuntimeWrapper(jsi_runtime* abiRuntime, napi_env rootEnv) noexcept
      : abiRuntime_(abiRuntime), rootEnv_(rootEnv) {}

  ~RuntimeWrapper() {
    // Release the abi runtime; this triggers ~JsiRuntimeState which fires
    // the attached-owner hook and tears down V8RuntimeEnv (which Unrefs
    // rootEnv_ ΓåÆ ~NodeApiEnv).
    if (abiRuntime_ && abiRuntime_->api && abiRuntime_->api->release) {
      abiRuntime_->api->release(abiRuntime_);
    }
  }

  jsi_runtime* abiRuntime() const noexcept { return abiRuntime_; }

  napi_status getNodeApi(napi_env* env) noexcept {
    *env = rootEnv_;
    return napi_ok;
  }

  napi_status getJsiRuntime(jsi_runtime** out) noexcept {
    if (!out) return napi_invalid_arg;
    *out = abiRuntime_;
    return napi_ok;
  }

  napi_status createNodeApi(int32_t apiVersion, napi_env* env) {
    auto* nodeEnv = static_cast<NodeApiEnv*>(rootEnv_);
    *env = nodeEnv->runtime()->createNodeApi(apiVersion);
    return napi_ok;
  }

 private:
  jsi_runtime* abiRuntime_;
  napi_env rootEnv_;
};

//============================================================================
// V8TaskRunner / V8ScriptCache ΓÇö local config helpers
//
// these own the consumer-supplied task runner / script cache C
// callback bundles. ConfigWrapper holds them; on v8_node_api_runtime_create they
// are translated into v8_jsi_abi_config setters that hand a C-callback pair to
// the JSI ABI. The wrapper lifetime is governed by the ABI's deleter
// contract ΓÇö the deleter the wrapper passes to v8_jsi_abi_config_set_*
// outlives the config and is invoked exactly once on runtime destruction.
//============================================================================
class V8TaskRunner {
 public:
  V8TaskRunner(void* userData,
               v8_node_api_task_runner_post_task_cb postTaskCallback,
               v8_node_api_data_delete_cb deleteCallback,
               void* deleteData) noexcept
      : userData_(userData),
        postTaskCallback_(postTaskCallback),
        deleteCallback_(deleteCallback),
        deleteData_(deleteData) {}

  ~V8TaskRunner() {
    if (deleteCallback_ != nullptr) {
      deleteCallback_(deleteData_, userData_);
    }
  }

  V8TaskRunner(const V8TaskRunner&) = delete;
  V8TaskRunner& operator=(const V8TaskRunner&) = delete;

  void* userData() const noexcept { return userData_; }
  v8_node_api_task_runner_post_task_cb postTaskCallback() const noexcept {
    return postTaskCallback_;
  }

 private:
  void* userData_;
  v8_node_api_task_runner_post_task_cb postTaskCallback_;
  v8_node_api_data_delete_cb deleteCallback_;
  void* deleteData_;
};

class V8ScriptCache {
 public:
  V8ScriptCache(void* userData,
                v8_node_api_script_cache_load_cb scriptCacheLoadCallback,
                v8_node_api_script_cache_store_cb scriptCacheStoreCallback,
                v8_node_api_data_delete_cb scriptCacheDataDeleteCallback,
                void* deleteData) noexcept
      : userData_(userData),
        scriptCacheLoadCallback_(scriptCacheLoadCallback),
        scriptCacheStoreCallback_(scriptCacheStoreCallback),
        scriptCacheDataDeleteCallback_(scriptCacheDataDeleteCallback),
        deleteData_(deleteData) {}

  ~V8ScriptCache() {
    if (scriptCacheDataDeleteCallback_) {
      scriptCacheDataDeleteCallback_(deleteData_, userData_);
    }
  }

  V8ScriptCache(const V8ScriptCache&) = delete;
  V8ScriptCache& operator=(const V8ScriptCache&) = delete;

  void* userData() const noexcept { return userData_; }
  v8_node_api_script_cache_load_cb loadCb() const noexcept {
    return scriptCacheLoadCallback_;
  }
  v8_node_api_script_cache_store_cb storeCb() const noexcept {
    return scriptCacheStoreCallback_;
  }

 private:
  void* userData_{};
  v8_node_api_script_cache_load_cb scriptCacheLoadCallback_{};
  v8_node_api_script_cache_store_cb scriptCacheStoreCallback_{};
  v8_node_api_data_delete_cb scriptCacheDataDeleteCallback_{};
  void* deleteData_{};
};

//============================================================================
// ConfigWrapper ΓÇö backs the public v8_node_api_config opaque type.
//
// stores the consumer-facing values directly (no V8RuntimeArgs round
// trip). On v8_node_api_runtime_create the values are pushed into a jsi_config via
// v8_jsi_abi_config_set_* setters, then v8_jsi_abi_runtime_create + v8_attach_node_api
// build the final v8_node_api_runtime.
//============================================================================
class ConfigWrapper {
 public:
  napi_status enableInspector(bool value) {
    if (status_ != napi_ok) return status_;
    enableInspector_ = value;
    return napi_ok;
  }

  napi_status enableGCApi(bool value) {
    if (status_ != napi_ok) return status_;
    enableGCApi_ = value;
    return napi_ok;
  }

  napi_status enableMultithreading(bool value) {
    if (status_ != napi_ok) return status_;
    enableMultithreading_ = value;
    return napi_ok;
  }

  napi_status setInspectorRuntimeName(std::string name) {
    if (status_ != napi_ok) return status_;
    inspectorRuntimeName_ = std::move(name);
    return napi_ok;
  }

  napi_status setInspectorPort(uint16_t port) {
    if (status_ != napi_ok) return status_;
    inspectorPort_ = port;
    return napi_ok;
  }

  napi_status setInspectorBreakOnStart(bool value) {
    if (status_ != napi_ok) return status_;
    inspectorBreakOnStart_ = value;
    return napi_ok;
  }

  napi_status setTaskRunner(std::shared_ptr<V8TaskRunner> taskRunner) {
    if (status_ != napi_ok) return status_;
    taskRunner_ = std::move(taskRunner);
    return napi_ok;
  }

  napi_status setScriptCache(std::shared_ptr<V8ScriptCache> scriptCache) {
    if (status_ != napi_ok) return status_;
    scriptCache_ = std::move(scriptCache);
    return napi_ok;
  }

  napi_status poison(napi_status status) {
    if (status_ == napi_ok) status_ = status;
    return status_;
  }

  napi_status status() const noexcept { return status_; }

  // Populate a factory-owned jsi_config from this wrapper's fields. Called from
  // the v8_jsi_abi_runtime_create configure callback — the factory owns cfg's lifetime.
  void buildJsiConfigInto(jsi_config cfg) const {
    v8_jsi_abi_config_set_inspector_enabled(cfg, enableInspector_);
    v8_jsi_abi_config_set_inspector_port(cfg, inspectorPort_);
    v8_jsi_abi_config_set_inspector_break_on_start(cfg, inspectorBreakOnStart_);
    v8_jsi_abi_config_set_debugger_runtime_name_utf8(
        cfg, inspectorRuntimeName_.data(), inspectorRuntimeName_.size());
    v8_jsi_abi_config_set_gc_api_enabled(cfg, enableGCApi_);
    v8_jsi_abi_config_set_multi_thread_enabled(cfg, enableMultithreading_);

    if (taskRunner_) {
      // Forward the consumer-supplied C task runner straight through. We
      // hand a shared_ptr<V8TaskRunner>* to the ABI as runner_data; the
      // ABI's deleter releases the holder, and ~V8TaskRunner runs the
      // consumer's data-deleter callback exactly once.
      auto* runnerHolder = new std::shared_ptr<V8TaskRunner>(taskRunner_);
      v8_jsi_abi_config_set_task_runner(
          cfg,
          /*post_task_cb:*/
          [](void* runner_data, void* task_data,
             v8_jsi_abi_task_run_cb task_run_cb,
             jsi_data_delete_cb task_data_delete_cb,
             void* task_data_delete_data) {
            auto* runner =
                static_cast<std::shared_ptr<V8TaskRunner>*>(runner_data);
            (*runner)->postTaskCallback()(
                (*runner)->userData(),
                task_data,
                reinterpret_cast<v8_node_api_task_run_cb>(task_run_cb),
                reinterpret_cast<v8_node_api_data_delete_cb>(
                    task_data_delete_cb),
                task_data_delete_data);
          },
          /*user_data:*/ runnerHolder,
          /*user_data_delete_cb:*/
          [](void* /*delete_data*/, void* runner_data) {
            delete static_cast<std::shared_ptr<V8TaskRunner>*>(runner_data);
          },
          /*user_data_delete_data:*/ nullptr);
    }

    if (scriptCache_) {
      auto* cacheHolder = new std::shared_ptr<V8ScriptCache>(scriptCache_);
      v8_jsi_abi_config_set_script_cache(
          cfg,
          /*load_cb:*/
          [](void* user_data, const char* source_url, uint64_t source_hash,
             const char* runtime_name, uint64_t runtime_version,
             const char* cache_tag, const uint8_t** out_data,
             size_t* out_size, jsi_data_delete_cb* out_data_delete_cb,
             void** out_data_delete_data) {
            auto* holder =
                static_cast<std::shared_ptr<V8ScriptCache>*>(user_data);
            v8_node_api_data_delete_cb delete_cb = nullptr;
            (*holder)->loadCb()(
                (*holder)->userData(), source_url, source_hash,
                runtime_name, runtime_version, cache_tag, out_data, out_size,
                &delete_cb, out_data_delete_data);
            *out_data_delete_cb =
                reinterpret_cast<jsi_data_delete_cb>(delete_cb);
          },
          /*store_cb:*/
          [](void* user_data, const char* source_url, uint64_t source_hash,
             const char* runtime_name, uint64_t runtime_version,
             const char* cache_tag, const uint8_t* data, size_t size,
             jsi_data_delete_cb data_delete_cb, void* data_delete_data) {
            auto* holder =
                static_cast<std::shared_ptr<V8ScriptCache>*>(user_data);
            (*holder)->storeCb()(
                (*holder)->userData(), source_url, source_hash,
                runtime_name, runtime_version, cache_tag, data, size,
                reinterpret_cast<v8_node_api_data_delete_cb>(data_delete_cb),
                data_delete_data);
          },
          /*user_data:*/ cacheHolder,
          /*user_data_delete_cb:*/
          [](void* /*delete_data*/, void* user_data) {
            delete static_cast<std::shared_ptr<V8ScriptCache>*>(user_data);
          },
          /*user_data_delete_data:*/ nullptr);
    }
  }

 private:
  bool enableInspector_{};
  bool enableMultithreading_{};
  bool enableGCApi_{};
  std::string inspectorRuntimeName_;
  uint16_t inspectorPort_{9223};
  bool inspectorBreakOnStart_{};
  std::shared_ptr<V8TaskRunner> taskRunner_;
  std::shared_ptr<V8ScriptCache> scriptCache_;
  napi_status status_{napi_ok};
};

}  // namespace v8impl

//============================================================================
// v8_attach_node_api ΓÇö JSI ABI dual-API attachment seam.
//============================================================================

JSI_ABI_API napi_status JSI_ABI_CDECL v8_attach_node_api(jsi_runtime* runtime,
                                                  int32_t /*api_version*/,
                                                  napi_env* env_out) {
  if (!runtime || !env_out) return napi_invalid_arg;

  auto* v8Env = new v8impl::V8RuntimeEnv(runtime);
  napi_env root{};
  napi_status s = v8Env->getRootNodeApi(&root);
  if (s != napi_ok) {
    delete v8Env;
    return s;
  }

  // Register the V8RuntimeEnv as the runtime's attached owner. ~JsiRuntimeState
  // will invoke this destroy callback before disposing V8 state.
  v8rt_internal::setAttachedOwner(
      runtime, v8Env,
      [](void* owner) {
        delete static_cast<v8impl::V8RuntimeEnv*>(owner);
      });

  *env_out = root;
  return napi_ok;
}

namespace {

napi_status resolveUtf8(const char* value,
                        size_t length,
                        std::string& result) {
  if (!value && length != 0 && length != V8_NODE_API_AUTO_LENGTH) {
    return napi_invalid_arg;
  }
  if (length == V8_NODE_API_AUTO_LENGTH) {
    length = value ? std::strlen(value) : 0;
  }
  result.assign(value ? value : "", length);
  return napi_ok;
}

}  // namespace

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_request_garbage_collection(napi_env env) {
  return CHECKED_ENV(env)->collectGarbage();
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_has_unhandled_promise_rejection(
    napi_env env,
    bool* out_has_rejection) {
  if (out_has_rejection) *out_has_rejection = false;
  if (!out_has_rejection) return napi_invalid_arg;
  return CHECKED_ENV(env)->hasUnhandledPromiseRejection(out_has_rejection);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_get_and_clear_last_unhandled_promise_rejection(
    napi_env env,
    napi_value* out_rejection) {
  if (out_rejection) *out_rejection = nullptr;
  if (!out_rejection) return napi_invalid_arg;
  return CHECKED_ENV(env)->getAndClearLastUnhandledPromiseRejection(
      out_rejection);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_get_description_utf8(
    napi_env env,
    const char** out_description) {
  if (out_description) *out_description = nullptr;
  if (!out_description) return napi_invalid_arg;
  return CHECKED_ENV(env)->getDescription(out_description);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_queue_microtask(napi_env env, napi_value callback) {
  return CHECKED_ENV(env)->queueMicrotask(callback);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_drain_microtasks(
    napi_env env,
    int32_t max_count_hint,
    bool* out_drained) {
  if (out_drained) *out_drained = false;
  if (!out_drained) return napi_invalid_arg;
  return CHECKED_ENV(env)->drainMicrotasks(max_count_hint, out_drained);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_is_inspectable(napi_env env, bool* out_inspectable) {
  if (out_inspectable) *out_inspectable = false;
  if (!out_inspectable) return napi_invalid_arg;
  return CHECKED_ENV(env)->isInspectable(out_inspectable);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_scope_open(
    napi_env env,
    v8_node_api_env_scope* out_scope) {
  if (out_scope) *out_scope = nullptr;
  if (!out_scope) return napi_invalid_arg;
  return CHECKED_ENV(env)->openEnvScope(out_scope);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_scope_close(
    v8_node_api_env_scope scope,
    napi_env env) {
  return CHECKED_ENV(env)->closeEnvScope(scope);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_run_script_utf8(
    napi_env env,
    napi_value source,
    const char* source_url_utf8,
    size_t source_url_length,
    napi_value* out_result) {
  if (out_result) *out_result = nullptr;
  if (!out_result) return napi_invalid_arg;
  std::string sourceUrl;
  napi_status status =
      resolveUtf8(source_url_utf8, source_url_length, sourceUrl);
  if (status != napi_ok) return status;
  return CHECKED_ENV(env)->runScript(source, sourceUrl.c_str(), out_result);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_prepared_script_create_utf8(
    napi_env env,
    const uint8_t* script_data,
    size_t script_length,
    v8_node_api_data_delete_cb delete_script,
    void* delete_data,
    const char* source_url_utf8,
    size_t source_url_length,
    v8_node_api_prepared_script* out_script) {
  if (out_script) *out_script = nullptr;
  if (!env || !script_data || !source_url_utf8 || !out_script) {
    if (delete_script) {
      delete_script(delete_data, const_cast<uint8_t*>(script_data));
    }
    return napi_invalid_arg;
  }
  std::string sourceUrl;
  napi_status status =
      resolveUtf8(source_url_utf8, source_url_length, sourceUrl);
  if (status != napi_ok) {
    if (delete_script) {
      delete_script(delete_data, const_cast<uint8_t*>(script_data));
    }
    return status;
  }
  return CHECKED_ENV(env)->createPreparedScript(script_data,
                                                script_length,
                                                delete_script,
                                                delete_data,
                                                sourceUrl.c_str(),
                                                out_script);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_prepared_script_destroy(
    v8_node_api_prepared_script script,
    napi_env env) {
  return CHECKED_ENV(env)->deletePreparedScript(script);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_prepared_script_run(
    v8_node_api_prepared_script script,
    napi_env env,
    napi_value* out_result) {
  if (out_result) *out_result = nullptr;
  if (!out_result) return napi_invalid_arg;
  return CHECKED_ENV(env)->runPreparedScript(script, out_result);
}

static jsi_status JSI_ABI_CDECL configureFromWrapper(void* user_data,
                                                     jsi_config cfg) {
  static_cast<const v8impl::ConfigWrapper*>(user_data)->buildJsiConfigInto(cfg);
  return jsi_status_ok;
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_runtime_create(
    uint32_t abi_version,
    v8_node_api_configure_runtime_cb configure,
    void* user_data,
    v8_node_api_runtime* out_runtime) {
  if (!out_runtime) return napi_invalid_arg;
  *out_runtime = nullptr;
  if (abi_version != V8_NODE_API_ABI_VERSION) return napi_invalid_arg;

  v8impl::ConfigWrapper config;
  if (configure) {
    napi_status status = configure(
        user_data, reinterpret_cast<v8_node_api_config>(&config));
    if (status != napi_ok) return status;
  }
  if (config.status() != napi_ok) return config.status();

  jsi_runtime* abiRuntime = nullptr;
  if (v8_jsi_abi_runtime_create(JSI_ABI_VERSION, configureFromWrapper,
                                &config, &abiRuntime) != jsi_status_ok) {
    return napi_generic_failure;
  }

  napi_env rootEnv{};
  napi_status s = v8_attach_node_api(abiRuntime, NAPI_VERSION_EXPERIMENTAL,
                                      &rootEnv);
  if (s != napi_ok) {
    if (abiRuntime->api && abiRuntime->api->release) {
      abiRuntime->api->release(abiRuntime);
    }
    return s;
  }

  *out_runtime = reinterpret_cast<v8_node_api_runtime>(
      new v8impl::RuntimeWrapper(abiRuntime, rootEnv));
  return napi_ok;
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_runtime_destroy(v8_node_api_runtime runtime) {
  V8_CHECK_ARG(runtime);
  delete reinterpret_cast<v8impl::RuntimeWrapper*>(runtime);
  return napi_ok;
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_dispose_platform(void) {
  v8rt::V8PlatformHolder::disposePlatform();
  return napi_ok;
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_runtime_get_env(
    v8_node_api_runtime runtime,
    napi_env* out_env) {
  if (out_env) *out_env = nullptr;
  if (!out_env) return napi_invalid_arg;
  return CHECKED_RUNTIME(runtime)->getNodeApi(out_env);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_runtime_get_jsi_runtime(
    v8_node_api_runtime runtime,
    jsi_runtime** out_runtime) {
  if (out_runtime) *out_runtime = nullptr;
  if (!out_runtime) return napi_invalid_arg;
  return CHECKED_RUNTIME(runtime)->getJsiRuntime(out_runtime);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_create_module(
    napi_env root_env,
    int32_t api_version,
    napi_env* out_env) {
  if (out_env) *out_env = nullptr;
  if (!out_env) return napi_invalid_arg;
  return CHECKED_ENV(root_env)->createNodeApi(api_version, out_env);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_run_task(
    napi_env env,
    v8_node_api_task_run_cb run_task,
    void* user_data) {
  if (!run_task) return napi_invalid_arg;
  return CHECKED_ENV(env)->runTask(run_task, user_data);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_inspector_enabled(
    v8_node_api_config config,
    bool value) {
  return CHECKED_CONFIG(config)->enableInspector(value);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_gc_api_enabled(
    v8_node_api_config config,
    bool value) {
  return CHECKED_CONFIG(config)->enableGCApi(value);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_multithreading_enabled(
    v8_node_api_config config,
    bool value) {
  return CHECKED_CONFIG(config)->enableMultithreading(value);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_inspector_runtime_name_utf8(
    v8_node_api_config config,
    const char* value,
    size_t length) {
  if (!config) return napi_generic_failure;
  auto* wrapper = reinterpret_cast<v8impl::ConfigWrapper*>(config);
  if (!value) return wrapper->poison(napi_invalid_arg);
  std::string name;
  napi_status status = resolveUtf8(value, length, name);
  if (status != napi_ok) return wrapper->poison(status);
  return wrapper->setInspectorRuntimeName(std::move(name));
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_inspector_port(
    v8_node_api_config config,
    uint16_t value) {
  return CHECKED_CONFIG(config)->setInspectorPort(value);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_inspector_break_on_start(
    v8_node_api_config config,
    bool value) {
  return CHECKED_CONFIG(config)->setInspectorBreakOnStart(value);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_task_runner(
    v8_node_api_config config,
    v8_node_api_task_runner_post_task_cb post_task,
    void* user_data,
    v8_node_api_data_delete_cb delete_user_data,
    void* delete_data) {
  if (!config) {
    if (delete_user_data) delete_user_data(delete_data, user_data);
    return napi_generic_failure;
  }
  return CHECKED_CONFIG(config)->setTaskRunner(
      std::make_shared<v8impl::V8TaskRunner>(
          user_data, post_task, delete_user_data, delete_data));
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_config_set_script_cache(
    v8_node_api_config config,
    v8_node_api_script_cache_load_cb load,
    v8_node_api_script_cache_store_cb store,
    void* user_data,
    v8_node_api_data_delete_cb delete_user_data,
    void* delete_data) {
  if (!config) {
    if (delete_user_data) delete_user_data(delete_data, user_data);
    return napi_generic_failure;
  }
  return CHECKED_CONFIG(config)->setScriptCache(
      std::make_shared<v8impl::V8ScriptCache>(
          user_data, load, store, delete_user_data, delete_data));
}

namespace node {

namespace per_process {
// From node.cc ΓÇö tells whether the per-process V8::Initialize() is called and
// whether it is safe to call v8::Isolate::GetCurrent().
bool v8_initialized = false;
}  // namespace per_process

// From node_errors.cc
[[noreturn]] void Assert(const AssertionInfo& info) {
#ifdef _WIN32
  char* processName{};
  _get_pgmptr(&processName);

  fprintf(stderr,
          "%s: %s:%s%s Assertion `%s' failed.\n",
          processName,
          info.file_line,
          info.function,
          *info.function ? ":" : "",
          info.message);
  fflush(stderr);
#endif  // _WIN32

  std::terminate();
}

}  // namespace node

// The created Buffer is the Uint8Array as in Node.js with version >= 4.
napi_status NAPI_CDECL
napi_create_external_buffer(napi_env env,
                            size_t length,
                            void* data,
                            node_api_nogc_finalize finalize_cb,
                            void* finalize_hint,
                            napi_value* result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);

#ifdef V8_ENABLE_SANDBOX
  // The V8 sandbox requires every ArrayBuffer backing store to live inside the
  // cage, so external backing stores are disallowed (mirrors upstream Node.js
  // node_api.cc). Consumers using the node-addon-api C++ wrapper get an
  // automatic copy fallback (Buffer::NewOrCopy); raw C-API callers must handle
  // this status. The JSI createArrayBuffer paths copy in-cage instead.
  return napi_set_last_error(env, napi_no_external_buffers_allowed);
#else
  struct DeleterData {
    napi_env env;
    node_api_nogc_finalize finalize_cb;
    void* finalize_hint;
  };

  v8::Isolate* isolate = env->isolate;

  DeleterData* deleterData =
      finalize_cb != nullptr ? new DeleterData{env, finalize_cb, finalize_hint}
                             : nullptr;
  std::unique_ptr<v8::BackingStore> backingStore =
      v8::ArrayBuffer::NewBackingStore(
          data,
          length,
          [](void* data, size_t length, void* deleter_data) {
            DeleterData* deleterData = static_cast<DeleterData*>(deleter_data);
            if (deleterData != nullptr) {
              deleterData->finalize_cb(
                  deleterData->env, data, deleterData->finalize_hint);
              delete deleterData;
            }
          },
          deleterData);

  v8::Local<v8::ArrayBuffer> arrayBuffer = v8::ArrayBuffer::New(
      isolate, std::shared_ptr<v8::BackingStore>(std::move(backingStore)));
  if (data == nullptr) {
    arrayBuffer->Detach(v8::Local<v8::Value>()).Check();
    if (deleterData != nullptr) {
      deleterData->finalize_cb(
          deleterData->env, nullptr, deleterData->finalize_hint);
      delete deleterData;
    }
  }

  v8::Local<v8::Uint8Array> buffer =
      v8::Uint8Array::New(arrayBuffer, 0, length);

  *result = v8impl::JsValueFromV8LocalValue(buffer);
  return GET_RETURN_STATUS(env);
#endif  // V8_ENABLE_SANDBOX
}

//=============================================================================
// JSI instrumentation
//=============================================================================

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_get_recorded_gc_stats_utf8(napi_env env,
                                         v8_node_api_utf8_output_cb cb,
                                         void* cb_ctx) {
  return CHECKED_ENV(env)->instrumentationGetGCStats(cb, cb_ctx);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_get_heap_info(napi_env env,
                                          bool include_expensive,
                                          v8_node_api_heap_info_cb cb,
                                          void* cb_ctx) {
  return CHECKED_ENV(env)->instrumentationGetHeapInfo(
      include_expensive, cb, cb_ctx);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_env_collect_garbage_utf8(
    napi_env env,
    const char* cause_utf8,
    size_t cause_length) {
  if (!cause_utf8) return napi_invalid_arg;
  std::string cause;
  napi_status status = resolveUtf8(cause_utf8, cause_length, cause);
  if (status != napi_ok) return status;
  return CHECKED_ENV(env)->instrumentationCollectGarbage(cause.c_str());
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_start_heap_sampling(napi_env env,
                                                size_t sampling_interval) {
  return CHECKED_ENV(env)->instrumentationStartHeapSampling(sampling_interval);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_stop_heap_sampling_utf8(napi_env env,
                                               v8_node_api_utf8_output_cb cb,
                                               void* cb_ctx) {
  return CHECKED_ENV(env)->instrumentationStopHeapSampling(cb, cb_ctx);
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_create_heap_snapshot_utf8(napi_env env,
                                                 bool capture_numeric_value,
                                                 v8_node_api_utf8_output_cb cb,
                                                 void* cb_ctx) {
  return CHECKED_ENV(env)->instrumentationCreateHeapSnapshot(
      capture_numeric_value, cb, cb_ctx);
}
