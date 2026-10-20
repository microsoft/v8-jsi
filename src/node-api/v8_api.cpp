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

#include "V8JsiRuntime_impl.h"
#include "js_native_api_v8.h"
#include "public/ScriptStore.h"
#include "v8_node_api.h"

#include <cstring>

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

class NodeApiJsiBuffer : public facebook::jsi::Buffer {
 public:
  NodeApiJsiBuffer(const uint8_t* data,
                   size_t byteCount,
                   v8_node_api_data_delete_cb deleteDataCallback,
                   void* deleterData) noexcept
      : data_(data),
        byteCount_(byteCount),
        deleteDataCallback_(deleteDataCallback),
        deleterData_(deleterData) {}

  ~NodeApiJsiBuffer() override {
    if (deleteDataCallback_ != nullptr) {
      deleteDataCallback_(deleterData_, const_cast<uint8_t*>(data_));
    }
  }

  NodeApiJsiBuffer(const NodeApiJsiBuffer&) = delete;
  NodeApiJsiBuffer& operator=(const NodeApiJsiBuffer&) = delete;

  const uint8_t* data() const override { return data_; }

  size_t size() const override { return byteCount_; }

 private:
  const uint8_t* data_{};
  size_t byteCount_{};
  v8_node_api_data_delete_cb deleteDataCallback_{};
  void* deleterData_{};
};

class NodeApiEnv;

class V8RuntimeEnv : public v8runtime::V8Runtime {
 public:
  V8RuntimeEnv(v8runtime::V8RuntimeArgs&& args)
      : v8runtime::V8Runtime(std::move(args)),
        m_rootEnv(createNodeApi(NAPI_VERSION_EXPERIMENTAL)) {}
  ~V8RuntimeEnv() override {}

  napi_status getRootNodeApi(napi_env* env);
  NodeApiEnv* createNodeApi(int32_t apiVersion);
  bool removeModuleEnv(NodeApiEnv* env);

  void SetImmediate(std::function<void()> callback) {
    isolate_data_->foreground_task_runner_->postTask(
        std::make_unique<ImmediateTask>(std::move(callback)));
  }

  class ImmediateTask : public v8runtime::JSITask {
   public:
    explicit ImmediateTask(std::function<void()> task)
        : task_(std::move(task)) {}

    void run() override { task_(); }

   private:
    std::function<void()> task_;
  };

  class NodeApiIsolateLocker : public IsolateLocker {
   public:
    NodeApiIsolateLocker(const V8Runtime* runtime)
        : IsolateLocker(runtime), runtime_(runtime), previous_(tls_current_) {
      tls_current_ = this;
    }

    static NodeApiIsolateLocker* Current() { return tls_current_; }

    static bool HasCurrentRuntime(const V8Runtime* runtime) {
      return tls_current_ != nullptr && tls_current_->runtime_ == runtime;
    }

    void AddRef() { ++refCount_; }

    void Release() {
      if (--refCount_ == 0) delete this;
    }

   private:
    ~NodeApiIsolateLocker() { tls_current_ = previous_; }

   private:
    const V8Runtime* runtime_;
    std::atomic<int32_t> refCount_{1};
    NodeApiIsolateLocker* previous_;
    static inline thread_local NodeApiIsolateLocker* tls_current_{};
  };

 private:
  std::vector<NodeApiEnv*> m_moduleEnvList;
  NodeApiEnv* m_rootEnv;  // Must the last field so we can call createNodeApi.
};

class NodeApiEnv : public napi_env__ {
 public:
  NodeApiEnv(V8RuntimeEnv* runtime, int32_t apiVersion)
      : m_runtime(runtime),
        napi_env__(runtime->GetIsolate(), runtime->GetContext(), apiVersion) {}

  ~NodeApiEnv() override {}

  void DeleteMe() override {
    m_isDestructing = true;
    // Get the field before this instance is deleted.
    V8RuntimeEnv* runtime = m_runtime;
    bool shouldDeleteRuntime = runtime->removeModuleEnv(this);
    DrainFinalizerQueue();
    napi_env__::DeleteMe();
    if (shouldDeleteRuntime) {
      delete runtime;
    }
  }

  void EnqueueFinalizer(v8impl::RefTracker* finalizer) override {
    napi_env__::EnqueueFinalizer(finalizer);
    // Schedule a second pass only when it has not been scheduled, and not
    // destructing the env.
    // When the env is being destructed, queued finalizers are drained in the
    // loop of `node_napi_env__::DrainFinalizerQueue`.
    if (!m_isFinalizationScheduled && !m_isDestructing) {
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
    // As userland code can delete additional references in one finalizer,
    // the list of pending finalizers may be mutated as we execute them, so
    // we keep iterating it until it is empty.
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

    v8::HandleScope handle_scope(isolate);
    v8::Context::Scope context_scope(context());

    CallIntoModule([&](napi_env env) { cb(env, data, hint); },
                   [](napi_env env, v8::Local<v8::Value> local_err) {
                     NodeApiEnv* runtimeEnv = static_cast<NodeApiEnv*>(env);
                     if (env->terminatedOrTerminating()) {
                       return;
                     }
                     // If there was an unhandled exception in the complete
                     // callback, report it as a fatal exception. (There is no
                     // JavaScript on the call stack that can possibly handle
                     // it.)
                     runtimeEnv->TriggerFatalException(local_err);
                   });
  }

  static void PrintToStderrAndFlush(const std::string& str) {
    fprintf(stderr, "%s\n", str.c_str());
    fflush(stderr);
  }

  void TriggerFatalException(v8::Local<v8::Value> err) {
    v8::Local<v8::Message> msg = v8::Exception::CreateMessage(isolate, err);

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
    // V8 drains microtasks automatically after each call.
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
    if (V8RuntimeEnv::NodeApiIsolateLocker::HasCurrentRuntime(m_runtime)) {
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
    v8::ScriptOrigin origin(context->GetIsolate(), urlV8String);

    auto maybe_script = v8::Script::Compile(
        context, v8::Local<v8::String>::Cast(v8_source), &origin);
    CHECK_MAYBE_EMPTY(env, maybe_script, napi_generic_failure);

    auto script_result = maybe_script.ToLocalChecked()->Run(context);
    CHECK_MAYBE_EMPTY(env, script_result, napi_generic_failure);

    *result = v8impl::JsValueFromV8LocalValue(script_result.ToLocalChecked());
    return GET_RETURN_STATUS(env);
  }

  napi_status createPreparedScript(const uint8_t* scriptData,
                                   size_t scriptLength,
                                   v8_node_api_data_delete_cb scriptDeleteCallback,
                                   void* deleterData,
                                   const char* sourceUrl,
                                   v8_node_api_prepared_script* result) {
    std::shared_ptr<facebook::jsi::Buffer> scriptBuffer =
        std::make_shared<NodeApiJsiBuffer>(
            scriptData, scriptLength, scriptDeleteCallback, deleterData);
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, scriptData);
    CHECK_ARG(env, sourceUrl);
    CHECK_ARG(env, result);
    std::shared_ptr<const facebook::jsi::PreparedJavaScript> preparedScript =
        m_runtime->prepareJavaScript2(scriptBuffer, sourceUrl);
    *result = reinterpret_cast<v8_node_api_prepared_script>(
        new std::shared_ptr<const facebook::jsi::PreparedJavaScript>(
            std::move(preparedScript)));
    return GET_RETURN_STATUS(env);
  }

  napi_status deletePreparedScript(v8_node_api_prepared_script preparedScript) {
    CHECK_ARG(env, preparedScript);
    std::shared_ptr<const facebook::jsi::PreparedJavaScript>* script =
        reinterpret_cast<
            std::shared_ptr<const facebook::jsi::PreparedJavaScript>*>(
            preparedScript);
    delete script;
    return napi_clear_last_error(env);
  }

  napi_status runPreparedScript(v8_node_api_prepared_script preparedScript,
                                napi_value* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, preparedScript);
    CHECK_ARG(env, result);

    std::shared_ptr<const facebook::jsi::PreparedJavaScript>* script =
        reinterpret_cast<
            std::shared_ptr<const facebook::jsi::PreparedJavaScript>*>(
            preparedScript);
    v8::Local<v8::Value> scriptResult =
        m_runtime->evaluatePreparedJavaScript2(*script);

    *result = v8impl::JsValueFromV8LocalValue(scriptResult);
    return GET_RETURN_STATUS(env);
  }

  napi_status createNodeApi(int32_t apiVersion, napi_env* env) {
    *env = m_runtime->createNodeApi(apiVersion);
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

NodeApiEnv* V8RuntimeEnv::createNodeApi(int32_t apiVersion) {
  NodeApiEnv* env = new NodeApiEnv(this, apiVersion);
  m_moduleEnvList.push_back(env);
  return env;
}

napi_status V8RuntimeEnv::getRootNodeApi(napi_env* env) {
  *env = m_rootEnv;
  return napi_ok;
}

bool V8RuntimeEnv::removeModuleEnv(NodeApiEnv* env) {
  auto it = std::find(m_moduleEnvList.begin(), m_moduleEnvList.end(), env);
  if (it == m_moduleEnvList.end()) {
    return false;
  }
  m_moduleEnvList.erase(it);
  bool isRootEnv = m_rootEnv == env;
  if (isRootEnv) {
    m_rootEnv = nullptr;
    // Do a copy to iterate over a snapshot of the list.
    std::vector<NodeApiEnv*> moduleEnvList = m_moduleEnvList;
    for (NodeApiEnv* moduleEnv : moduleEnvList) {
      moduleEnv->Unref();
    }
  }

  return isRootEnv;
}

class V8TaskRunner : public v8runtime::JSITaskRunner {
 public:
  V8TaskRunner(void* userData,
               v8_node_api_task_runner_post_task_cb postTaskCallback,
               v8_node_api_data_delete_cb deleteCallback,
               void* deleteData)
      : userData_(userData),
        postTaskCallback_(postTaskCallback),
        deleteCallback_(deleteCallback),
        deleteData_(deleteData) {}

  ~V8TaskRunner() {
    if (deleteCallback_ != nullptr) {
      deleteCallback_(deleteData_, userData_);
    }
  }

  void postTask(std::unique_ptr<v8runtime::JSITask> task) override {
    postTaskCallback_(
        userData_,
        static_cast<void*>(task.release()),
        [](void* taskData) {
          static_cast<v8runtime::JSITask*>(taskData)->run();
        },
        [](void* /*deleteData*/, void* taskData) {
          delete static_cast<v8runtime::JSITask*>(taskData);
        },
        /*deleteData:*/ nullptr);
  }

 private:
  void* userData_;  // a pointer to the task runner implementation
  v8_node_api_task_runner_post_task_cb postTaskCallback_;
  v8_node_api_data_delete_cb deleteCallback_;
  void* deleteData_;
};

class V8JsiBuffer : public facebook::jsi::Buffer {
 public:
  V8JsiBuffer(const uint8_t* data,
              size_t size,
              v8_node_api_data_delete_cb deleteCallback,
              void* deleteData)
      : data_(data),
        size_(size),
        deleteCallback_(deleteCallback),
        deleteData_(deleteData) {}

  ~V8JsiBuffer() override {
    if (deleteCallback_ != nullptr) {
      deleteCallback_(deleteData_, const_cast<uint8_t*>(data_));
    }
  }

  const uint8_t* data() const override { return data_; }

  size_t size() const override { return size_; }

 private:
  const uint8_t* data_{};
  size_t size_{};
  v8_node_api_data_delete_cb deleteCallback_{};
  void* deleteData_{};
};

class V8ScriptCache : public facebook::jsi::PreparedScriptStore {
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

  ~V8ScriptCache() override {
    if (scriptCacheDataDeleteCallback_) {
      scriptCacheDataDeleteCallback_(deleteData_, userData_);
    }
  }

  std::shared_ptr<const facebook::jsi::Buffer> tryGetPreparedScript(
      const facebook::jsi::ScriptSignature& scriptSignature,
      const facebook::jsi::JSRuntimeSignature& runtimeMetadata,
      const char* prepareTag) noexcept override {
    const uint8_t* buffer{};
    size_t bufferSize{};
    v8_node_api_data_delete_cb bufferDeleteCallback{};
    void* bufferDeleterData{};
    scriptCacheLoadCallback_(userData_,
                             scriptSignature.url.c_str(),
                             scriptSignature.version,
                             runtimeMetadata.runtimeName.c_str(),
                             runtimeMetadata.version,
                             prepareTag,
                             &buffer,
                             &bufferSize,
                             &bufferDeleteCallback,
                             &bufferDeleterData);
    return (buffer && bufferSize)
               ? std::make_shared<V8JsiBuffer>(buffer,
                                               bufferSize,
                                               bufferDeleteCallback,
                                               bufferDeleterData)
               : nullptr;
  }

  void persistPreparedScript(
      std::shared_ptr<const facebook::jsi::Buffer> preparedScript,
      const facebook::jsi::ScriptSignature& scriptSignature,
      const facebook::jsi::JSRuntimeSignature& runtimeMetadata,
      const char* prepareTag) noexcept override {
    scriptCacheStoreCallback_(
        userData_,
        scriptSignature.url.c_str(),
        scriptSignature.version,
        runtimeMetadata.runtimeName.c_str(),
        runtimeMetadata.version,
        prepareTag,
        preparedScript->data(),
        preparedScript->size(),
        [](void* deleteData, void* /*data*/) {
          delete reinterpret_cast<
              std::shared_ptr<const facebook::jsi::Buffer>*>(deleteData);
        },
        new std::shared_ptr<const facebook::jsi::Buffer>(preparedScript));
  }

 private:
  void* userData_{};
  v8_node_api_script_cache_load_cb scriptCacheLoadCallback_{};
  v8_node_api_script_cache_store_cb scriptCacheStoreCallback_{};
  v8_node_api_data_delete_cb scriptCacheDataDeleteCallback_{};
  void* deleteData_{};
};

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

  v8runtime::V8RuntimeArgs getV8RuntimeArgs() const {
    v8runtime::V8RuntimeArgs args{};
    args.flags.trackGCObjectStats = false;
    args.flags.enableJitTracing = false;
    args.flags.enableMessageTracing = false;
    args.flags.enableGCTracing = false;
    args.flags.enableInspector = enableInspector_;
    args.flags.waitForDebugger = inspectorBreakOnStart_;
    args.flags.enableGCApi = enableGCApi_;
    args.flags.ignoreUnhandledPromises = false;
    args.flags.enableSystemInstrumentation = false;
    args.flags.sparkplug = false;
    args.flags.predictable = false;
    args.flags.optimize_for_size = false;
    args.flags.always_compact = false;
    args.flags.jitless = false;
    args.flags.lite_mode = false;
    args.flags.thread_pool_size = 0;
    args.flags.enableMultiThread = enableMultithreading_;

    args.inspectorPort = inspectorPort_;
    args.debuggerRuntimeName = inspectorRuntimeName_;

    args.foreground_task_runner = taskRunner_;

    if (scriptCache_) {
      args.preparedScriptStore = scriptCache_;
    }

    return args;
  }

 private:
  bool enableInspector_{};
  bool enableMultithreading_{};
  bool enableGCApi_{};
  std::string inspectorRuntimeName_;
  uint16_t inspectorPort_{};
  bool inspectorBreakOnStart_{};
  std::shared_ptr<V8TaskRunner> taskRunner_;
  std::shared_ptr<V8ScriptCache> scriptCache_;
  napi_status status_{napi_ok};
};

class RuntimeWrapper {
 public:
  explicit RuntimeWrapper(const ConfigWrapper& config) {
    v8runtime::V8RuntimeArgs args = config.getV8RuntimeArgs();
    runtime_ = new V8RuntimeEnv(std::move(args));
  }

  ~RuntimeWrapper() {
    napi_env env;
    runtime_->getRootNodeApi(&env);
    env->Unref();
  }

  napi_status getNodeApi(napi_env* env) {
    return runtime_->getRootNodeApi(env);
  }

  napi_status createNodeApi(int32_t apiVersion, napi_env* env) {
    *env = runtime_->createNodeApi(apiVersion);
    return napi_ok;
  }

 private:
  V8RuntimeEnv* runtime_;
};

}  // namespace v8impl

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

  *out_runtime =
      reinterpret_cast<v8_node_api_runtime>(new v8impl::RuntimeWrapper(config));
  return napi_ok;
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_runtime_destroy(v8_node_api_runtime runtime) {
  V8_CHECK_ARG(runtime);
  delete reinterpret_cast<v8impl::RuntimeWrapper*>(runtime);
  return napi_ok;
}

V8_NODE_API_API napi_status V8_NODE_API_CDECL
v8_node_api_dispose_platform(void) {
  v8runtime::V8PlatformHolder::disposePlatform();
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
    v8_node_api_runtime /*runtime*/,
    jsi_runtime** out_runtime) {
  if (out_runtime) *out_runtime = nullptr;
  return out_runtime ? napi_generic_failure : napi_invalid_arg;
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
// From node.cc
// Tells whether the per-process V8::Initialize() is called and
// if it is safe to call v8::Isolate::GetCurrent().
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

  TRACEV8RUNTIME_CRITICAL("Assertion failed");

  std::terminate();
}

}  // namespace node

// TODO: [vmoroz] verify that finalize_cb runs in JS thread
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
  // See v8_api_abi.cpp: external backing stores are disallowed under the V8
  // sandbox. (NOTE: this file is not currently compiled into v8jsi.dll --
  // v8jsi.gyp uses v8_api_abi.cpp -- but the guard is kept in sync.)
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

// Gets garbage collection statistics as a JSON-encoded string
V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_get_recorded_gc_stats_utf8(napi_env env,
                                         v8_node_api_utf8_output_cb cb,
                                         void* cb_ctx) {
  return CHECKED_ENV(env)->instrumentationGetGCStats(cb, cb_ctx);
}

// Gets current heap information as a struct
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

// Starts heap sampling profiler
V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_start_heap_sampling(napi_env env,
                                                size_t sampling_interval) {
  return CHECKED_ENV(env)->instrumentationStartHeapSampling(sampling_interval);
}

// Stops heap sampling profiler and returns the result as JSON
V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_stop_heap_sampling_utf8(napi_env env,
                                               v8_node_api_utf8_output_cb cb,
                                               void* cb_ctx) {
  return CHECKED_ENV(env)->instrumentationStopHeapSampling(cb, cb_ctx);
}

// Creates a heap snapshot and saves it to a file
V8_NODE_API_API napi_status V8_NODE_API_CDECL v8_node_api_env_create_heap_snapshot_utf8(napi_env env,
                                                 bool capture_numeric_value,
                                                 v8_node_api_utf8_output_cb cb,
                                                 void* cb_ctx) {
  return CHECKED_ENV(env)->instrumentationCreateHeapSnapshot(
      capture_numeric_value, cb, cb_ctx);
}
