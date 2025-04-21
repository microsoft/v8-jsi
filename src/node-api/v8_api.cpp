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
#include "public/v8_api.h"

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
                   jsr_data_delete_cb deleteDataCallback,
                   void* deleterData) noexcept
      : data_(data),
        byteCount_(byteCount),
        deleteDataCallback_(deleteDataCallback),
        deleterData_(deleterData) {}

  ~NodeApiJsiBuffer() override {
    if (deleteDataCallback_ != nullptr) {
      deleteDataCallback_(const_cast<uint8_t*>(data_), deleterData_);
    }
  }

  NodeApiJsiBuffer(const NodeApiJsiBuffer&) = delete;
  NodeApiJsiBuffer& operator=(const NodeApiJsiBuffer&) = delete;

  const uint8_t* data() const override { return data_; }

  size_t size() const override { return byteCount_; }

 private:
  const uint8_t* data_{};
  size_t byteCount_{};
  jsr_data_delete_cb deleteDataCallback_{};
  void* deleterData_{};
};

napi_status CopyStringToOutput(const std::string& str, char** result, size_t* result_length) {
  if (!result || !result_length) {
    return napi_invalid_arg;
  }
  
  size_t len = str.length();
  char* buffer = new char[len + 1];
  memcpy(buffer, str.c_str(), len);
  buffer[len] = '\0';
  
  *result = buffer;
  *result_length = len;
  
  return napi_ok;
}

class StringOutputStream : public std::stringstream {
public:
  ~StringOutputStream() = default;
};

// Custom buffer stream that writes directly to a provided buffer
class DirectBufferStream : public std::ostream {
public:
  class DirectBufferStreamBuf : public std::streambuf {
  public:
    DirectBufferStreamBuf(char* buffer, size_t size) : buffer_(buffer), size_(size), current_size_(0) {
      if (buffer_) {
        setp(buffer_, buffer_ + size_ - 1); // Reserve space for null terminator
      } else {
        // No actual buffer available, we'll just track size
        setp(nullptr, nullptr);
      }
    }

    size_t size() const { return current_size_; }

    int_type overflow(int_type ch) override {
      current_size_++;
      
      if (!buffer_ || current_size_ > size_) {
        // Just tracking the required size or already overflowed buffer
        return ch;
      }

      // We have space in the buffer
      *pptr() = static_cast<char>(ch);
      pbump(1);
      
      return ch;
    }
    
  private:
    char* buffer_;
    size_t size_;
    size_t current_size_;
  };

  DirectBufferStream(char* buffer, size_t size) 
    : std::ostream(&buffer_), buffer_(buffer, size) {}

  size_t size() const { return buffer_.size(); }

private:
  DirectBufferStreamBuf buffer_;
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
    isolate->LowMemoryNotification();
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

  napi_status openEnvScope(jsr_napi_env_scope* scope) {
    CHECK_ARG(env, scope);
    if (V8RuntimeEnv::NodeApiIsolateLocker::HasCurrentRuntime(m_runtime)) {
      V8RuntimeEnv::NodeApiIsolateLocker::Current()->AddRef();
    } else {
      ::new V8RuntimeEnv::NodeApiIsolateLocker(m_runtime);
    }
    *scope = reinterpret_cast<jsr_napi_env_scope>(
        V8RuntimeEnv::NodeApiIsolateLocker::Current());
    return napi_ok;
  }

  napi_status closeEnvScope(jsr_napi_env_scope scope) {
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
                                   jsr_data_delete_cb scriptDeleteCallback,
                                   void* deleterData,
                                   const char* sourceUrl,
                                   jsr_prepared_script* result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, scriptData);
    CHECK_ARG(env, sourceUrl);
    CHECK_ARG(env, result);
    std::shared_ptr<facebook::jsi::Buffer> scriptBuffer =
        std::shared_ptr<facebook::jsi::Buffer>(new NodeApiJsiBuffer(
            scriptData, scriptLength, scriptDeleteCallback, deleterData));
    std::shared_ptr<const facebook::jsi::PreparedJavaScript> preparedScript =
        m_runtime->prepareJavaScript2(scriptBuffer, sourceUrl);
    *result = reinterpret_cast<jsr_prepared_script>(
        new std::shared_ptr<const facebook::jsi::PreparedJavaScript>(
            std::move(preparedScript)));
    return GET_RETURN_STATUS(env);
  }

  napi_status deletePreparedScript(jsr_prepared_script preparedScript) {
    CHECK_ARG(env, preparedScript);
    std::shared_ptr<const facebook::jsi::PreparedJavaScript>* script =
        reinterpret_cast<
            std::shared_ptr<const facebook::jsi::PreparedJavaScript>*>(
            preparedScript);
    delete script;
    return napi_clear_last_error(env);
  }

  napi_status runPreparedScript(jsr_prepared_script preparedScript,
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

  napi_status runTask(jsr_task_run_cb task_cb, void* data) {
    CallIntoModule([task_cb, data](napi_env env) { task_cb(data); });
    return napi_ok;
  }

  // Instrumentation related methods
  napi_status getRecordedGcStates(char** result, size_t* result_length) {
    CHECK_ARG(env, result);
    CHECK_ARG(env, result_length);

    auto& instrumentation = m_runtime->instrumentation();
    
    std::string stats = instrumentation.getRecordedGCStats();
    return CopyStringToOutput(stats, result, result_length);
  }
  
  napi_status getHeapInfo(bool include_expensive, char** result, size_t* result_length) {
    CHECK_ARG(env, result);
    CHECK_ARG(env, result_length);

    auto& instrumentation = m_runtime->instrumentation();
    
    auto heapInfo = instrumentation.getHeapInfo(include_expensive);
    
    // Convert the map to JSON
    std::stringstream json;
    json << "{";
    bool first = true;
    for (const auto& item : heapInfo) {
      if (!first) {
        json << ",";
      }
      json << "\"" << item.first << "\":" << item.second;
      first = false;
    }
    json << "}";
    
    return CopyStringToOutput(json.str(), result, result_length);
  }
  
  napi_status startTrackingHeapObjectStackTraces() {
    auto& instrumentation = m_runtime->instrumentation();
    
    instrumentation.startTrackingHeapObjectStackTraces(nullptr);
    return napi_ok;
  }
  
  napi_status stopTrackingHeapObjectStackTraces() {
    auto& instrumentation = m_runtime->instrumentation();
    
    instrumentation.stopTrackingHeapObjectStackTraces();
    return napi_ok;
  }
  
  napi_status startHeapSampling(size_t sampling_interval) {
    auto& instrumentation = m_runtime->instrumentation();
    
    instrumentation.startHeapSampling(sampling_interval);
    return napi_ok;
  }
  
  napi_status stopHeapSampling(char** result, size_t* result_length) {
    CHECK_ARG(env, result);
    CHECK_ARG(env, result_length);

    auto& instrumentation = m_runtime->instrumentation();
    
    StringOutputStream stream;
    instrumentation.stopHeapSampling(stream);
    
    return CopyStringToOutput(stream.str(), result, result_length);
  }
  
  napi_status createHeapSnapshotToFile(const char* path, const jsr_heap_snapshot_options* options) {
    CHECK_ARG(env, path);

    auto& instrumentation = m_runtime->instrumentation();
    
    facebook::jsi::Instrumentation::HeapSnapshotOptions jsiOptions;
    if (options) {
      jsiOptions.captureNumericValue = options->capture_numeric_value;
    }
    
    #if JSI_VERSION >= 13
    instrumentation.createSnapshotToFile(path, jsiOptions);
    #else
    instrumentation.createSnapshotToFile(path);
    #endif
    return napi_ok;
  }
  
  napi_status createHeapSnapshotToString(
      char* buffer,
      size_t buffer_length,
      const jsr_heap_snapshot_options* options,
      size_t* required_length) {
    CHECK_ARG(env, required_length);

    auto& instrumentation = m_runtime->instrumentation();
  
    facebook::jsi::Instrumentation::HeapSnapshotOptions jsiOptions;
    if (options) {
      jsiOptions.captureNumericValue = options->capture_numeric_value;
    }
  
    // Create a custom stream that writes directly to the provided buffer
    DirectBufferStream stream(buffer, buffer_length);
  
  #if JSI_VERSION >= 13
    instrumentation.createSnapshotToStream(stream, jsiOptions);
  #else
    instrumentation.createSnapshotToStream(stream);
  #endif
  
    // Update the required length based on what the stream determined
    *required_length = stream.size();
  
    // Add null terminator if we have space
    if (buffer && buffer_length > 0 && stream.size() < buffer_length) {
      buffer[stream.size()] = '\0';
    }
  
    // Return an appropriate error code if the buffer was too small
    if (buffer && buffer_length > 0 && stream.size() >= buffer_length) {
      return napi_invalid_arg;  // Buffer overflow - too small for the data
    }
  
    return napi_ok;
  }
  
  napi_status flushAndDisableBridgeTrafficTrace(char** result, size_t* result_length) {
    CHECK_ARG(env, result);
    CHECK_ARG(env, result_length);

    auto& instrumentation = m_runtime->instrumentation();
    
    std::string trace = instrumentation.flushAndDisableBridgeTrafficTrace();
    return CopyStringToOutput(trace, result, result_length);
  }
  
  napi_status writeBasicBlockProfileTrace(const char* file_name) {
    CHECK_ARG(env, file_name);

    auto& instrumentation = m_runtime->instrumentation();
    
    instrumentation.writeBasicBlockProfileTraceToFile(file_name);
    return napi_ok;
  }
  
  napi_status dumpProfilerSymbols(const char* file_name) {
    CHECK_ARG(env, file_name);

    auto& instrumentation = m_runtime->instrumentation();
    
    instrumentation.dumpProfilerSymbolsToFile(file_name);
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
  V8TaskRunner(void* taskRunnerData,
               jsr_task_runner_post_task_cb postTaskCallback,
               jsr_data_delete_cb deleteCallback,
               void* deleterData)
      : taskRunnerData_(taskRunnerData),
        postTaskCallback_(postTaskCallback),
        deleteCallback_(deleteCallback),
        deleterData_(deleterData) {}

  ~V8TaskRunner() {
    if (deleteCallback_ != nullptr) {
      deleteCallback_(taskRunnerData_, deleterData_);
    }
  }

  void postTask(std::unique_ptr<v8runtime::JSITask> task) override {
    postTaskCallback_(
        taskRunnerData_,
        static_cast<void*>(task.release()),
        [](void* taskData) {
          static_cast<v8runtime::JSITask*>(taskData)->run();
        },
        [](void* taskData, void* /*deleterData*/) {
          delete static_cast<v8runtime::JSITask*>(taskData);
        },
        /*deleterData:*/ nullptr);
  }

 private:
  void* taskRunnerData_;  // a pointer to the task runner implementation
  jsr_task_runner_post_task_cb postTaskCallback_;
  jsr_data_delete_cb deleteCallback_;
  void* deleterData_;
};

class V8JsiBuffer : public facebook::jsi::Buffer {
 public:
  V8JsiBuffer(const uint8_t* data,
              size_t size,
              jsr_data_delete_cb deleteCallback,
              void* deleterData)
      : data_(data),
        size_(size),
        deleteCallback_(deleteCallback),
        deleterData_(deleterData) {}

  ~V8JsiBuffer() override {
    if (deleteCallback_ != nullptr) {
      deleteCallback_(const_cast<uint8_t*>(data_), deleterData_);
    }
  }

  const uint8_t* data() const override { return data_; }

  size_t size() const override { return size_; }

 private:
  const uint8_t* data_{};
  size_t size_{};
  jsr_data_delete_cb deleteCallback_{};
  void* deleterData_{};
};

class V8ScriptCache : public facebook::jsi::PreparedScriptStore {
 public:
  V8ScriptCache(void* scriptCacheData,
                jsr_script_cache_load_cb scriptCacheLoadCallback,
                jsr_script_cache_store_cb scriptCacheStoreCallback,
                jsr_data_delete_cb scriptCacheDataDeleteCallback,
                void* deleterData) noexcept
      : scriptCacheData_(scriptCacheData),
        scriptCacheLoadCallback_(scriptCacheLoadCallback),
        scriptCacheStoreCallback_(scriptCacheStoreCallback),
        scriptCacheDataDeleteCallback_(scriptCacheDataDeleteCallback),
        deleterData_(deleterData) {}

  ~V8ScriptCache() override {
    if (scriptCacheDataDeleteCallback_) {
      scriptCacheDataDeleteCallback_(scriptCacheData_, deleterData_);
    }
  }

  std::shared_ptr<const facebook::jsi::Buffer> tryGetPreparedScript(
      const facebook::jsi::ScriptSignature& scriptSignature,
      const facebook::jsi::JSRuntimeSignature& runtimeMetadata,
      const char* prepareTag) noexcept override {
    const uint8_t* buffer{};
    size_t bufferSize{};
    jsr_data_delete_cb bufferDeleteCallback{};
    void* bufferDeleterData{};
    scriptCacheLoadCallback_(scriptCacheData_,
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
        scriptCacheData_,
        scriptSignature.url.c_str(),
        scriptSignature.version,
        runtimeMetadata.runtimeName.c_str(),
        runtimeMetadata.version,
        prepareTag,
        preparedScript->data(),
        preparedScript->size(),
        [](void* /*data*/, void* deleterData) {
          delete reinterpret_cast<
              std::shared_ptr<const facebook::jsi::Buffer>*>(deleterData);
        },
        new std::shared_ptr<const facebook::jsi::Buffer>(preparedScript));
  }

 private:
  void* scriptCacheData_{};
  jsr_script_cache_load_cb scriptCacheLoadCallback_{};
  jsr_script_cache_store_cb scriptCacheStoreCallback_{};
  jsr_data_delete_cb scriptCacheDataDeleteCallback_{};
  void* deleterData_{};
};

class ConfigWrapper {
 public:
  napi_status enableInspector(bool value) {
    enableInspector_ = value;
    return napi_ok;
  }

  napi_status enableGCApi(bool value) {
    enableGCApi_ = value;
    return napi_ok;
  }

  napi_status enableMultithreading(bool value) {
    enableMultithreading_ = value;
    return napi_ok;
  }

  napi_status setInspectorRuntimeName(std::string name) {
    inspectorRuntimeName_ = std::move(name);
    return napi_ok;
  }

  napi_status setInspectorPort(uint16_t port) {
    inspectorPort_ = port;
    return napi_ok;
  }

  napi_status setInspectorBreakOnStart(bool value) {
    inspectorBreakOnStart_ = value;
    return napi_ok;
  }

  napi_status setTaskRunner(std::shared_ptr<V8TaskRunner> taskRunner) {
    taskRunner_ = std::move(taskRunner);
    return napi_ok;
  }

  napi_status setScriptCache(std::shared_ptr<V8ScriptCache> scriptCache) {
    scriptCache_ = std::move(scriptCache);
    return napi_ok;
  }

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

// Provides a hint to run garbage collection.
// It is typically used for unit tests.
JSR_API jsr_collect_garbage(napi_env env) {
  return CHECKED_ENV(env)->collectGarbage();
}

// Checks if the environment has an unhandled promise rejection.
JSR_API jsr_has_unhandled_promise_rejection(napi_env env, bool* result) {
  return CHECKED_ENV(env)->hasUnhandledPromiseRejection(result);
}

// Gets and clears the last unhandled promise rejection.
JSR_API jsr_get_and_clear_last_unhandled_promise_rejection(napi_env env,
                                                           napi_value* result) {
  return CHECKED_ENV(env)->getAndClearLastUnhandledPromiseRejection(result);
}

// To implement JSI description()
JSR_API jsr_get_description(napi_env env, const char** result) {
  return CHECKED_ENV(env)->getDescription(result);
}

// To implement JSI drainMicrotasks()
JSR_API jsr_drain_microtasks(napi_env env,
                             int32_t max_count_hint,
                             bool* result) {
  return CHECKED_ENV(env)->drainMicrotasks(max_count_hint, result);
}

// To implement JSI isInspectable()
JSR_API jsr_is_inspectable(napi_env env, bool* result) {
  return CHECKED_ENV(env)->isInspectable(result);
}

JSR_API jsr_open_napi_env_scope(napi_env env, jsr_napi_env_scope* scope) {
  return CHECKED_ENV(env)->openEnvScope(scope);
}

JSR_API jsr_close_napi_env_scope(napi_env env, jsr_napi_env_scope scope) {
  return CHECKED_ENV(env)->closeEnvScope(scope);
}

// Run script with source URL.
JSR_API jsr_run_script(napi_env env,
                       napi_value source,
                       const char* source_url,
                       napi_value* result) {
  return CHECKED_ENV(env)->runScript(source, source_url, result);
}

// Prepare the script for running.
JSR_API jsr_create_prepared_script(napi_env env,
                                   const uint8_t* script_data,
                                   size_t script_length,
                                   jsr_data_delete_cb script_delete_cb,
                                   void* deleter_data,
                                   const char* source_url,
                                   jsr_prepared_script* result) {
  return CHECKED_ENV(env)->createPreparedScript(script_data,
                                                script_length,
                                                script_delete_cb,
                                                deleter_data,
                                                source_url,
                                                result);
}

// Delete the prepared script.
JSR_API jsr_delete_prepared_script(napi_env env,
                                   jsr_prepared_script prepared_script) {
  return CHECKED_ENV(env)->deletePreparedScript(prepared_script);
}

// Run the prepared script.
JSR_API jsr_prepared_script_run(napi_env env,
                                jsr_prepared_script prepared_script,
                                napi_value* result) {
  return CHECKED_ENV(env)->runPreparedScript(prepared_script, result);
}

JSR_API jsr_create_runtime(jsr_config config, jsr_runtime* runtime) {
  V8_CHECK_ARG(config);
  V8_CHECK_ARG(runtime);
  *runtime = reinterpret_cast<jsr_runtime>(new v8impl::RuntimeWrapper(
      *reinterpret_cast<v8impl::ConfigWrapper*>(config)));
  return napi_ok;
}

JSR_API jsr_delete_runtime(jsr_runtime runtime) {
  V8_CHECK_ARG(runtime);
  delete reinterpret_cast<v8impl::RuntimeWrapper*>(runtime);
  return napi_ok;
}

JSR_API v8_platform_dispose() {
  v8runtime::V8PlatformHolder::disposePlatform();
  return napi_ok;
}

JSR_API jsr_runtime_get_node_api_env(jsr_runtime runtime, napi_env* env) {
  return CHECKED_RUNTIME(runtime)->getNodeApi(env);
}

JSR_API jsr_create_node_api_env(napi_env root_env,
                                int32_t api_version,
                                napi_env* env) {
  return CHECKED_ENV(root_env)->createNodeApi(api_version, env);
}

JSR_API jsr_run_task(napi_env env, jsr_task_run_cb task_cb, void* data) {
  return CHECKED_ENV(env)->runTask(task_cb, data);
}

JSR_API jsr_create_config(jsr_config* config) {
  V8_CHECK_ARG(config);
  *config = reinterpret_cast<jsr_config>(new v8impl::ConfigWrapper());
  return napi_ok;
}

JSR_API jsr_delete_config(jsr_config config) {
  V8_CHECK_ARG(config);
  delete reinterpret_cast<v8impl::ConfigWrapper*>(config);
  return napi_ok;
}

JSR_API jsr_config_enable_inspector(jsr_config config, bool value) {
  return CHECKED_CONFIG(config)->enableInspector(value);
}

JSR_API jsr_config_enable_gc_api(jsr_config config, bool value) {
  return CHECKED_CONFIG(config)->enableGCApi(value);
}

JSR_API v8_config_enable_multithreading(jsr_config config, bool value) {
  return CHECKED_CONFIG(config)->enableMultithreading(value);
}

JSR_API jsr_config_set_inspector_runtime_name(jsr_config config,
                                              const char* name) {
  return CHECKED_CONFIG(config)->setInspectorRuntimeName(name);
}

JSR_API jsr_config_set_inspector_port(jsr_config config, uint16_t port) {
  return CHECKED_CONFIG(config)->setInspectorPort(port);
}

JSR_API jsr_config_set_inspector_break_on_start(jsr_config config, bool value) {
  return CHECKED_CONFIG(config)->setInspectorBreakOnStart(value);
}

JSR_API jsr_config_set_task_runner(
    jsr_config config,
    void* task_runner_data,
    jsr_task_runner_post_task_cb task_runner_post_task_cb,
    jsr_data_delete_cb task_runner_data_delete_cb,
    void* deleter_data) {
  return CHECKED_CONFIG(config)->setTaskRunner(
      std::make_shared<v8impl::V8TaskRunner>(task_runner_data,
                                             task_runner_post_task_cb,
                                             task_runner_data_delete_cb,
                                             deleter_data));
}

JSR_API jsr_config_set_script_cache(
    jsr_config config,
    void* script_cache_data,
    jsr_script_cache_load_cb script_cache_load_cb,
    jsr_script_cache_store_cb script_cache_store_cb,
    jsr_data_delete_cb script_cache_data_delete_cb,
    void* deleter_data) {
  return CHECKED_CONFIG(config)->setScriptCache(
      std::make_shared<v8impl::V8ScriptCache>(script_cache_data,
                                              script_cache_load_cb,
                                              script_cache_store_cb,
                                              script_cache_data_delete_cb,
                                              deleter_data));
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
}

// Free a string allocated by JSR APIs
JSR_API jsr_free_string(napi_env env, char* string) {
  if (string) {
    delete[] string;
  }
  
  return napi_ok;
}

JSR_API jsr_get_recorded_gc_stats(napi_env env, char** result, size_t* result_length) {
  return CHECKED_ENV(env)->getRecordedGcStates(result, result_length);
}

JSR_API jsr_get_heap_info(napi_env env, bool include_expensive, char** result, size_t* result_length) {
  return CHECKED_ENV(env)->getHeapInfo(include_expensive, result, result_length);
}

JSR_API jsr_start_tracking_heap_object_stack_traces(napi_env env) {
  return CHECKED_ENV(env)->startTrackingHeapObjectStackTraces();
}

JSR_API jsr_stop_tracking_heap_object_stack_traces(napi_env env) {
  return CHECKED_ENV(env)->stopTrackingHeapObjectStackTraces();
}

JSR_API jsr_start_heap_sampling(napi_env env, size_t sampling_interval) {
  return CHECKED_ENV(env)->startHeapSampling(sampling_interval);
}

JSR_API jsr_stop_heap_sampling(napi_env env, char** result, size_t* result_length) {
  return CHECKED_ENV(env)->stopHeapSampling(result, result_length);
}

JSR_API jsr_create_heap_snapshot_to_file(napi_env env, const char* path, const jsr_heap_snapshot_options* options) {
  return CHECKED_ENV(env)->createHeapSnapshotToFile(path, options);
}

JSR_API jsr_create_heap_snapshot_to_string(napi_env env, char* buffer, size_t buffer_length, const jsr_heap_snapshot_options* options, size_t* required_length) {
  return CHECKED_ENV(env)->createHeapSnapshotToString(buffer, buffer_length, options, required_length);
}

JSR_API jsr_flush_and_disable_bridge_traffic_trace(napi_env env, char** result, size_t* result_length) {
  return CHECKED_ENV(env)->flushAndDisableBridgeTrafficTrace(result, result_length);
}

JSR_API jsr_write_basic_block_profile_trace(napi_env env, const char* file_name) {
  return CHECKED_ENV(env)->writeBasicBlockProfileTrace(file_name);
}

JSR_API jsr_dump_profiler_symbols(napi_env env, const char* file_name) {
  return CHECKED_ENV(env)->dumpProfilerSymbols(file_name);
}
