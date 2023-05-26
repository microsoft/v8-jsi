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

#include "env-inl.h"

#include "V8JsiRuntime_impl.h"
#include "js_native_api_v8.h"
#include "public/ScriptStore.h"
#include "public/v8_api.h"

#define CHECKED_ENV(env) \
  ((env) == nullptr) ? napi_invalid_arg : static_cast<v8impl::V8RuntimeEnv *>(reinterpret_cast<napi_env>(env))

#define CHECKED_RUNTIME(runtime) \
  ((runtime) == nullptr) ? napi_generic_failure : reinterpret_cast<v8impl::RuntimeWrapper *>(runtime)

#define CHECKED_CONFIG(config) \
  ((config) == nullptr) ? napi_generic_failure : reinterpret_cast<v8impl::ConfigWrapper *>(config)

#define V8_CHECK_ARG(arg)        \
  if ((arg) == nullptr) {        \
    return napi_generic_failure; \
  }

namespace v8impl {

class NodeApiJsiBuffer : public facebook::jsi::Buffer {
 public:
  NodeApiJsiBuffer(
      const uint8_t *data,
      size_t byteCount,
      jsr_data_delete_cb deleteDataCallback,
      void *deleterData) noexcept
      : data_(data), byteCount_(byteCount), deleteDataCallback_(deleteDataCallback), deleterData_(deleterData) {}

  ~NodeApiJsiBuffer() override {
    if (deleteDataCallback_ != nullptr) {
      deleteDataCallback_(const_cast<uint8_t *>(data_), deleterData_);
    }
  }

  NodeApiJsiBuffer(const NodeApiJsiBuffer &) = delete;
  NodeApiJsiBuffer &operator=(const NodeApiJsiBuffer &) = delete;

  const uint8_t *data() const override {
    return data_;
  }

  size_t size() const override {
    return byteCount_;
  }

 private:
  const uint8_t *data_{};
  size_t byteCount_{};
  jsr_data_delete_cb deleteDataCallback_{};
  void *deleterData_{};
};

class V8RuntimeEnv : public v8runtime::V8Runtime, public napi_env__ {
 public:
  V8RuntimeEnv(v8runtime::V8RuntimeArgs &&args)
      : v8runtime::V8Runtime(std::move(args)), napi_env__(GetIsolate(), GetContext()) {}

  ~V8RuntimeEnv() override {}

  napi_status collectGarbage() {
    isolate->RequestGarbageCollectionForTesting(v8::Isolate::kFullGarbageCollection);
    return napi_status::napi_ok;
  }

  napi_status hasUnhandledPromiseRejection(bool *result) {
    CHECK_ARG(env, result);
    *result = HasUnhandledPromiseRejection();
    return napi_ok;
  }

  napi_status getDescription(const char **result) noexcept {
    CHECK_ARG(env, result);
    *result = "V8";
    return napi_ok;
  }

  napi_status drainMicrotasks(int32_t /*maxCountHint*/, bool * /*result*/) {
    // V8 drains microtasks automatically after each call.
    return napi_ok;
  }

  napi_status isInspectable(bool *result) noexcept {
    CHECK_ARG(env, result);
    *result = v8runtime::V8Runtime::isInspectable();
    return napi_ok;
  }

  class NodeApiIsolateLocker : public IsolateLocker {
   public:
    NodeApiIsolateLocker(const V8Runtime *runtime)
        : IsolateLocker(runtime), runtime_(runtime), previous_(tls_current_) {
      tls_current_ = this;
    }

    static NodeApiIsolateLocker *Current() {
      return tls_current_;
    }

    static bool HasCurrentRuntime(const V8Runtime *runtime) {
      return tls_current_ != nullptr && tls_current_->runtime_ == runtime;
    }

    void AddRef() {
      ++refCount_;
    }

    void Release() {
      if (--refCount_ == 0)
        delete this;
    }

   private:
    ~NodeApiIsolateLocker() {
      tls_current_ = previous_;
    }

   private:
    const V8Runtime *runtime_;
    std::atomic<int32_t> refCount_{1};
    NodeApiIsolateLocker *previous_;
    static inline thread_local NodeApiIsolateLocker *tls_current_{};
  };

  napi_status openEnvScope(jsr_napi_env_scope *scope) {
    CHECK_ARG(env, scope);
    if (NodeApiIsolateLocker::HasCurrentRuntime(this)) {
      NodeApiIsolateLocker::Current()->AddRef();
    } else {
      ::new NodeApiIsolateLocker(this);
    }
    *scope = reinterpret_cast<jsr_napi_env_scope>(NodeApiIsolateLocker::Current());
    return napi_ok;
  }

  napi_status closeEnvScope(jsr_napi_env_scope scope) {
    CHECK_ARG(env, scope);
    NodeApiIsolateLocker *locker = reinterpret_cast<NodeApiIsolateLocker *>(scope);
    if (locker != NodeApiIsolateLocker::Current()) {
      return napi_generic_failure;
    }
    locker->Release();
    return napi_ok;
  }

  napi_status getAndClearLastUnhandledPromiseRejection(napi_value *result) {
    CHECK_ARG(env, result);
    auto rejectionInfo = GetAndClearLastUnhandledPromiseRejection();
    *result = v8impl::JsValueFromV8LocalValue(rejectionInfo->value.Get(isolate));
    return napi_ok;
  }

  napi_status runScript(napi_value source, const char *source_url, napi_value *result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, source);
    CHECK_ARG(env, result);

    v8::Local<v8::Value> v8_source = v8impl::V8LocalValueFromJsValue(source);

    if (!v8_source->IsString()) {
      return napi_set_last_error(env, napi_string_expected);
    }

    v8::Local<v8::Context> context = env->context();

    const char *checked_source_url = source_url ? source_url : "";
    v8::Local<v8::String> urlV8String =
        v8::String::NewFromUtf8(context->GetIsolate(), checked_source_url).ToLocalChecked();
    v8::ScriptOrigin origin(context->GetIsolate(), urlV8String);

    auto maybe_script = v8::Script::Compile(context, v8::Local<v8::String>::Cast(v8_source), &origin);
    CHECK_MAYBE_EMPTY(env, maybe_script, napi_generic_failure);

    auto script_result = maybe_script.ToLocalChecked()->Run(context);
    CHECK_MAYBE_EMPTY(env, script_result, napi_generic_failure);

    *result = v8impl::JsValueFromV8LocalValue(script_result.ToLocalChecked());
    return GET_RETURN_STATUS(env);
  }

  napi_status createPreparedScript(
      const uint8_t *scriptData,
      size_t scriptLength,
      jsr_data_delete_cb scriptDeleteCallback,
      void *deleterData,
      const char *sourceUrl,
      jsr_prepared_script *result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, scriptData);
    CHECK_ARG(env, sourceUrl);
    CHECK_ARG(env, result);
    std::shared_ptr<facebook::jsi::Buffer> scriptBuffer = std::shared_ptr<facebook::jsi::Buffer>(
        new NodeApiJsiBuffer(scriptData, scriptLength, scriptDeleteCallback, deleterData));
    std::shared_ptr<const facebook::jsi::PreparedJavaScript> preparedScript =
        prepareJavaScript2(scriptBuffer, sourceUrl);
    *result = reinterpret_cast<jsr_prepared_script>(
        new std::shared_ptr<const facebook::jsi::PreparedJavaScript>(std::move(preparedScript)));
  }

  napi_status deletePreparedScript(jsr_prepared_script preparedScript) {
    CHECK_ARG(env, preparedScript);
    std::shared_ptr<const facebook::jsi::PreparedJavaScript> *script =
        reinterpret_cast<std::shared_ptr<const facebook::jsi::PreparedJavaScript> *>(preparedScript);
    delete script;
    return napi_clear_last_error(env);
  }

  napi_status runPreparedScript(jsr_prepared_script preparedScript, napi_value *result) {
    NAPI_PREAMBLE(env);
    CHECK_ARG(env, preparedScript);
    CHECK_ARG(env, result);

    std::shared_ptr<const facebook::jsi::PreparedJavaScript> *script =
        reinterpret_cast<std::shared_ptr<const facebook::jsi::PreparedJavaScript> *>(preparedScript);
    v8::Local<v8::Value> scriptResult = evaluatePreparedJavaScript2(*script);

    *result = v8impl::JsValueFromV8LocalValue(scriptResult);
    return GET_RETURN_STATUS(env);
  }

 private:
  napi_env env{this};
};

class V8TaskRunner : public v8runtime::JSITaskRunner {
 public:
  V8TaskRunner(
      void *taskRunnerData,
      jsr_task_runner_post_task_cb postTaskCallback,
      jsr_data_delete_cb deleteCallback,
      void *deleterData)
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
        static_cast<void *>(task.release()),
        [](void *taskData) { static_cast<v8runtime::JSITask *>(taskData)->run(); },
        [](void *taskData, void * /*deleterData*/) { delete static_cast<v8runtime::JSITask *>(taskData); },
        /*deleterData:*/ nullptr);
  }

 private:
  void *taskRunnerData_; // a pointer to the task runner implementation
  jsr_task_runner_post_task_cb postTaskCallback_;
  jsr_data_delete_cb deleteCallback_;
  void *deleterData_;
};

class V8JsiBuffer : public facebook::jsi::Buffer {
 public:
  V8JsiBuffer(const uint8_t *data, size_t size, jsr_data_delete_cb deleteCallback, void *deleterData)
      : data_(data), size_(size), deleteCallback_(deleteCallback), deleterData_(deleterData) {}

  ~V8JsiBuffer() override {
    if (deleteCallback_ != nullptr) {
      deleteCallback_(const_cast<uint8_t *>(data_), deleterData_);
    }
  }

  const uint8_t *data() const override {
    return data_;
  }

  size_t size() const override {
    return size_;
  }

 private:
  const uint8_t *data_{};
  size_t size_{};
  jsr_data_delete_cb deleteCallback_{};
  void *deleterData_{};
};

class V8ScriptCache : public facebook::jsi::PreparedScriptStore {
 public:
  V8ScriptCache(
      void *scriptCacheData,
      jsr_script_cache_load_cb scriptCacheLoadCallback,
      jsr_script_cache_store_cb scriptCacheStoreCallback,
      jsr_data_delete_cb scriptCacheDataDeleteCallback,
      void *deleterData) noexcept
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
      const facebook::jsi::ScriptSignature &scriptSignature,
      const facebook::jsi::JSRuntimeSignature &runtimeMetadata,
      const char *prepareTag) noexcept override {
    const uint8_t *buffer{};
    size_t bufferSize{};
    jsr_data_delete_cb bufferDeleteCallback{};
    void *bufferDeleterData{};
    scriptCacheLoadCallback_(
        scriptCacheData_,
        scriptSignature.url.c_str(),
        scriptSignature.version,
        runtimeMetadata.runtimeName.c_str(),
        runtimeMetadata.version,
        prepareTag,
        &buffer,
        &bufferSize,
        &bufferDeleteCallback,
        &bufferDeleterData);
    return std::make_shared<V8JsiBuffer>(buffer, bufferSize, bufferDeleteCallback, bufferDeleterData);
  }

  void persistPreparedScript(
      std::shared_ptr<const facebook::jsi::Buffer> preparedScript,
      const facebook::jsi::ScriptSignature &scriptSignature,
      const facebook::jsi::JSRuntimeSignature &runtimeMetadata,
      const char *prepareTag) noexcept override {
    scriptCacheStoreCallback_(
        scriptCacheData_,
        scriptSignature.url.c_str(),
        scriptSignature.version,
        runtimeMetadata.runtimeName.c_str(),
        runtimeMetadata.version,
        prepareTag,
        preparedScript->data(),
        preparedScript->size(),
        [](void * /*data*/, void *deleterData) {
          delete reinterpret_cast<std::shared_ptr<const facebook::jsi::Buffer> *>(deleterData);
        },
        new std::shared_ptr<const facebook::jsi::Buffer>(preparedScript));
  }

 private:
  void *scriptCacheData_{};
  jsr_script_cache_load_cb scriptCacheLoadCallback_{};
  jsr_script_cache_store_cb scriptCacheStoreCallback_{};
  jsr_data_delete_cb scriptCacheDataDeleteCallback_{};
  void *deleterData_{};
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
  explicit RuntimeWrapper(const ConfigWrapper &config) {
    v8runtime::V8RuntimeArgs args = config.getV8RuntimeArgs();
    env_ = new V8RuntimeEnv(std::move(args));
  }

  ~RuntimeWrapper() {
    env_->Unref();
  }

  napi_status getNodeApi(napi_env *env) {
    *env = env_;
    return napi_ok;
  }

 private:
  V8RuntimeEnv *env_;
};

} // namespace v8impl

// Provides a hint to run garbage collection.
// It is typically used for unit tests.
JSR_API jsr_collect_garbage(napi_env env) {
  return CHECKED_ENV(env)->collectGarbage();
}

// Checks if the environment has an unhandled promise rejection.
JSR_API jsr_has_unhandled_promise_rejection(napi_env env, bool *result) {
  return CHECKED_ENV(env)->hasUnhandledPromiseRejection(result);
}

// Gets and clears the last unhandled promise rejection.
JSR_API jsr_get_and_clear_last_unhandled_promise_rejection(napi_env env, napi_value *result) {
  return CHECKED_ENV(env)->getAndClearLastUnhandledPromiseRejection(result);
}

// To implement JSI description()
JSR_API jsr_get_description(napi_env env, const char **result) {
  return CHECKED_ENV(env)->getDescription(result);
}

// To implement JSI drainMicrotasks()
JSR_API jsr_drain_microtasks(napi_env env, int32_t max_count_hint, bool *result) {
  return CHECKED_ENV(env)->drainMicrotasks(max_count_hint, result);
}

// To implement JSI isInspectable()
JSR_API jsr_is_inspectable(napi_env env, bool *result) {
  return CHECKED_ENV(env)->isInspectable(result);
}

JSR_API jsr_open_napi_env_scope(napi_env env, jsr_napi_env_scope *scope) {
  return CHECKED_ENV(env)->openEnvScope(scope);
}

JSR_API jsr_close_napi_env_scope(napi_env env, jsr_napi_env_scope scope) {
  return CHECKED_ENV(env)->closeEnvScope(scope);
}

// Run script with source URL.
JSR_API jsr_run_script(napi_env env, napi_value source, const char *source_url, napi_value *result) {
  return CHECKED_ENV(env)->runScript(source, source_url, result);
}

// Prepare the script for running.
JSR_API jsr_create_prepared_script(
    napi_env env,
    const uint8_t *script_data,
    size_t script_length,
    jsr_data_delete_cb script_delete_cb,
    void *deleter_data,
    const char *source_url,
    jsr_prepared_script *result) {
  return CHECKED_ENV(env)->createPreparedScript(
      script_data, script_length, script_delete_cb, deleter_data, source_url, result);
}

// Delete the prepared script.
JSR_API jsr_delete_prepared_script(napi_env env, jsr_prepared_script prepared_script) {
  return CHECKED_ENV(env)->deletePreparedScript(prepared_script);
}

// Run the prepared script.
JSR_API jsr_prepared_script_run(napi_env env, jsr_prepared_script prepared_script, napi_value *result) {
  return CHECKED_ENV(env)->runPreparedScript(prepared_script, result);
}

JSR_API jsr_create_runtime(jsr_config config, jsr_runtime *runtime) {
  V8_CHECK_ARG(config);
  V8_CHECK_ARG(runtime);
  *runtime =
      reinterpret_cast<jsr_runtime>(new v8impl::RuntimeWrapper(*reinterpret_cast<v8impl::ConfigWrapper *>(config)));
  return napi_ok;
}

JSR_API jsr_delete_runtime(jsr_runtime runtime) {
  V8_CHECK_ARG(runtime);
  delete reinterpret_cast<v8impl::RuntimeWrapper *>(runtime);
  return napi_ok;
}

JSR_API jsr_runtime_get_node_api_env(jsr_runtime runtime, napi_env *env) {
  return CHECKED_RUNTIME(runtime)->getNodeApi(env);
}

JSR_API jsr_create_config(jsr_config *config) {
  V8_CHECK_ARG(config);
  *config = reinterpret_cast<jsr_config>(new v8impl::ConfigWrapper());
  return napi_ok;
}

JSR_API jsr_delete_config(jsr_config config) {
  V8_CHECK_ARG(config);
  delete reinterpret_cast<v8impl::ConfigWrapper *>(config);
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

JSR_API jsr_config_set_inspector_runtime_name(jsr_config config, const char *name) {
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
    void *task_runner_data,
    jsr_task_runner_post_task_cb task_runner_post_task_cb,
    jsr_data_delete_cb task_runner_data_delete_cb,
    void *deleter_data) {
  return CHECKED_CONFIG(config)->setTaskRunner(std::make_shared<v8impl::V8TaskRunner>(
      task_runner_data, task_runner_post_task_cb, task_runner_data_delete_cb, deleter_data));
}

JSR_API jsr_config_set_script_cache(
    jsr_config config,
    void *script_cache_data,
    jsr_script_cache_load_cb script_cache_load_cb,
    jsr_script_cache_store_cb script_cache_store_cb,
    jsr_data_delete_cb script_cache_data_delete_cb,
    void *deleter_data) {
  return CHECKED_CONFIG(config)->setScriptCache(std::make_shared<v8impl::V8ScriptCache>(
      script_cache_data, script_cache_load_cb, script_cache_store_cb, script_cache_data_delete_cb, deleter_data));
}

namespace node {

namespace per_process {
// From node.cc
// Tells whether the per-process V8::Initialize() is called and
// if it is safe to call v8::Isolate::GetCurrent().
bool v8_initialized = false;
} // namespace per_process

// From node_errors.cc
[[noreturn]] void Assert(const AssertionInfo &info) {
#ifdef _WIN32
  char *processName{};
  _get_pgmptr(&processName);

  fprintf(
      stderr,
      "%s: %s:%s%s Assertion `%s' failed.\n",
      processName,
      info.file_line,
      info.function,
      *info.function ? ":" : "",
      info.message);
  fflush(stderr);
#endif // _WIN32

  TRACEV8RUNTIME_CRITICAL("Assertion failed");

  std::terminate();
}

} // namespace node

// TODO: [vmoroz] verify that finalize_cb runs in JS thread
// The created Buffer is the Uint8Array as in Node.js with version >= 4.
napi_status napi_create_external_buffer(
    napi_env env,
    size_t length,
    void *data,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);

  struct DeleterData {
    napi_env env;
    napi_finalize finalize_cb;
    void *finalize_hint;
  };

  v8::Isolate *isolate = env->isolate;

  DeleterData *deleterData = finalize_cb != nullptr ? new DeleterData{env, finalize_cb, finalize_hint} : nullptr;
  auto backingStore = v8::ArrayBuffer::NewBackingStore(
      data,
      length,
      [](void *data, size_t length, void *deleter_data) {
        DeleterData *deleterData = static_cast<DeleterData *>(deleter_data);
        if (deleterData != nullptr) {
          deleterData->finalize_cb(deleterData->env, data, deleterData->finalize_hint);
          delete deleterData;
        }
      },
      deleterData);

  v8::Local<v8::ArrayBuffer> arrayBuffer =
      v8::ArrayBuffer::New(isolate, std::shared_ptr<v8::BackingStore>(std::move(backingStore)));

  v8::Local<v8::Uint8Array> buffer = v8::Uint8Array::New(arrayBuffer, 0, length);

  *result = v8impl::JsValueFromV8LocalValue(buffer);
  return GET_RETURN_STATUS(env);
}
