// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Consumer-side definition of v8runtime::makeV8Runtime.
//
// This translation unit ships as source in the NuGet alongside V8JsiRuntime.h
// (see also include/node-api-jsi/NodeApiJsiRuntime.cpp for the same pattern).
// The consumer compiles it locally; only the C-stable v8_jsi_config_* setters,
// the v8_create_runtime entry point, and the JsiAbiRuntime C++ wrapper (also
// compiled in the consumer) cross into v8jsi.dll.

#include "V8JsiRuntime.h"

#include "jsi_abi/JsiAbiRuntime.h"
#include "jsi_abi/v8_jsi_config.h"
#include "ScriptStore.h"

namespace v8runtime {

namespace {

// Wraps a buffer that crosses the DLL boundary back into a facebook::jsi::Buffer
// so the consumer's PreparedScriptStore::persistPreparedScript can hold on to
// the bytes for later retrieval. The supplied delete callback frees the bytes
// in whichever CRT allocated them — ours, when invoked from inside V8jsi.dll's
// store path; theirs, when invoked from a recursive load.
class V8JsiBuffer final : public facebook::jsi::Buffer {
 public:
  V8JsiBuffer(
      const uint8_t *buffer,
      size_t bufferSize,
      jsi_data_delete_cb bufferDeleteCallback,
      void *deleterData) noexcept
      : buffer_(buffer),
        bufferSize_(bufferSize),
        bufferDeleteCallback_(bufferDeleteCallback),
        deleterData_(deleterData) {}

  ~V8JsiBuffer() override {
    if (bufferDeleteCallback_) {
      bufferDeleteCallback_(
          const_cast<uint8_t *>(buffer_), deleterData_);
    }
  }

  V8JsiBuffer(const V8JsiBuffer &) = delete;
  V8JsiBuffer &operator=(const V8JsiBuffer &) = delete;

  const uint8_t *data() const override { return buffer_; }
  size_t size() const override { return bufferSize_; }

 private:
  const uint8_t *buffer_;
  size_t bufferSize_;
  jsi_data_delete_cb bufferDeleteCallback_;
  void *deleterData_;
};

// Adapter from the consumer's std::shared_ptr<facebook::jsi::PreparedScriptStore>
// to the C-callback shape expected by v8_jsi_config_set_script_cache. Modeled
// on react-native-windows/vnext/Shared/JSI/V8RuntimeHolder.cpp's V8ScriptCache.
//
// Allocated and freed entirely in the consumer's CRT — the DLL only sees an
// opaque void* and three function pointers.
class V8ScriptCache {
 public:
  static void Create(
      jsi_config config,
      std::shared_ptr<facebook::jsi::PreparedScriptStore> scriptStore) {
    v8_jsi_config_set_script_cache(
        config,
        new V8ScriptCache(std::move(scriptStore)),
        &LoadScript,
        &StoreScript,
        &Delete,
        nullptr);
  }

 private:
  explicit V8ScriptCache(
      std::shared_ptr<facebook::jsi::PreparedScriptStore> scriptStore)
      : scriptStore_(std::move(scriptStore)) {}

  static void __cdecl LoadScript(
      void *scriptCache,
      const char *sourceUrl,
      uint64_t sourceHash,
      const char *runtimeName,
      uint64_t runtimeVersion,
      const char *cacheTag,
      const uint8_t **buffer,
      size_t *bufferSize,
      jsi_data_delete_cb *bufferDeleteCallback,
      void **deleterData) {
    auto &scriptStore =
        reinterpret_cast<V8ScriptCache *>(scriptCache)->scriptStore_;
    std::shared_ptr<const facebook::jsi::Buffer> preparedScript =
        scriptStore->tryGetPreparedScript(
            facebook::jsi::ScriptSignature{sourceUrl, sourceHash},
            facebook::jsi::JSRuntimeSignature{runtimeName, runtimeVersion},
            cacheTag);
    if (preparedScript) {
      *buffer = preparedScript->data();
      *bufferSize = preparedScript->size();
      *bufferDeleteCallback = [](void * /*data*/, void *deleterData) {
        delete reinterpret_cast<
            std::shared_ptr<const facebook::jsi::Buffer> *>(deleterData);
      };
      *deleterData = new std::shared_ptr<const facebook::jsi::Buffer>(
          std::move(preparedScript));
    } else {
      *buffer = nullptr;
      *bufferSize = 0;
      *bufferDeleteCallback = nullptr;
      *deleterData = nullptr;
    }
  }

  static void __cdecl StoreScript(
      void *scriptCache,
      const char *sourceUrl,
      uint64_t sourceHash,
      const char *runtimeName,
      uint64_t runtimeVersion,
      const char *cacheTag,
      const uint8_t *buffer,
      size_t bufferSize,
      jsi_data_delete_cb bufferDeleteCallback,
      void *deleterData) {
    auto &scriptStore =
        reinterpret_cast<V8ScriptCache *>(scriptCache)->scriptStore_;
    scriptStore->persistPreparedScript(
        std::make_shared<V8JsiBuffer>(
            buffer, bufferSize, bufferDeleteCallback, deleterData),
        facebook::jsi::ScriptSignature{sourceUrl, sourceHash},
        facebook::jsi::JSRuntimeSignature{runtimeName, runtimeVersion},
        cacheTag);
  }

  static void __cdecl Delete(void *scriptCache, void * /*deleterData*/) {
    delete reinterpret_cast<V8ScriptCache *>(scriptCache);
  }

  std::shared_ptr<facebook::jsi::PreparedScriptStore> scriptStore_;
};

// Adapter from the consumer's std::shared_ptr<JSITaskRunner> to the C-callback
// shape expected by v8_jsi_config_set_task_runner. Modeled on
// react-native-windows/vnext/Shared/JSI/V8RuntimeHolder.cpp's V8TaskRunner.
//
// Allocated and freed entirely in the consumer's CRT — the DLL only sees an
// opaque void* and two function pointers.
class V8TaskRunner {
 public:
  static void Create(jsi_config config,
                     std::shared_ptr<JSITaskRunner> taskRunner) {
    v8_jsi_config_set_task_runner(
        config,
        new V8TaskRunner(std::move(taskRunner)),
        &PostTask,
        &Delete,
        nullptr);
  }

 private:
  explicit V8TaskRunner(std::shared_ptr<JSITaskRunner> taskRunner)
      : taskRunner_(std::move(taskRunner)) {}

  // Wraps the C-callback pair as a JSITask the consumer's runner can post.
  // Destructor invokes the DLL-supplied delete callback so the DLL frees
  // the task in its own CRT.
  class CTask final : public JSITask {
   public:
    CTask(void *taskData,
          v8_jsi_task_run_cb taskRunCb,
          jsi_data_delete_cb taskDataDeleteCb,
          void *deleterData) noexcept
        : taskData_(taskData),
          taskRunCb_(taskRunCb),
          taskDataDeleteCb_(taskDataDeleteCb),
          deleterData_(deleterData) {}

    ~CTask() override {
      if (taskDataDeleteCb_) {
        taskDataDeleteCb_(taskData_, deleterData_);
      }
    }

    CTask(const CTask &) = delete;
    CTask &operator=(const CTask &) = delete;

    void run() override {
      if (taskRunCb_) {
        taskRunCb_(taskData_);
      }
    }

   private:
    void *taskData_;
    v8_jsi_task_run_cb taskRunCb_;
    jsi_data_delete_cb taskDataDeleteCb_;
    void *deleterData_;
  };

  static void __cdecl PostTask(
      void *taskRunnerData,
      void *taskData,
      v8_jsi_task_run_cb taskRunCb,
      jsi_data_delete_cb taskDataDeleteCb,
      void *deleterData) {
    auto &taskRunner =
        reinterpret_cast<V8TaskRunner *>(taskRunnerData)->taskRunner_;
    taskRunner->postTask(std::make_unique<CTask>(
        taskData, taskRunCb, taskDataDeleteCb, deleterData));
  }

  static void __cdecl Delete(void *taskRunnerData, void * /*deleterData*/) {
    delete reinterpret_cast<V8TaskRunner *>(taskRunnerData);
  }

  std::shared_ptr<JSITaskRunner> taskRunner_;
};

void applyArgs(jsi_config cfg, const V8RuntimeArgs &args) {
  v8_jsi_config_set_initial_heap_size(cfg, args.initial_heap_size_in_bytes);
  v8_jsi_config_set_maximum_heap_size(cfg, args.maximum_heap_size_in_bytes);
  v8_jsi_config_set_thread_pool_size(cfg, args.flags.thread_pool_size);
  v8_jsi_config_set_debugger_runtime_name(
      cfg, args.debuggerRuntimeName.c_str());
  v8_jsi_config_enable_inspector(cfg, args.flags.enableInspector);
  v8_jsi_config_set_inspector_port(cfg, args.inspectorPort);
  v8_jsi_config_set_inspector_break_on_start(cfg, args.flags.waitForDebugger);
  v8_jsi_config_set_explicit_microtask_policy(
      cfg, args.flags.explicitMicrotaskPolicy);
  v8_jsi_config_set_ignore_unhandled_promises(
      cfg, args.flags.ignoreUnhandledPromises);
  v8_jsi_config_enable_gc_api(cfg, args.flags.enableGCApi);
  v8_jsi_config_enable_multi_thread(cfg, args.flags.enableMultiThread);
  v8_jsi_config_enable_jit_tracing(cfg, args.flags.enableJitTracing);
  v8_jsi_config_enable_message_tracing(cfg, args.flags.enableMessageTracing);
  v8_jsi_config_enable_gc_tracing(cfg, args.flags.enableGCTracing);
  v8_jsi_config_enable_system_instrumentation(
      cfg, args.flags.enableSystemInstrumentation);
  // Process-global V8 engine flags (sparkplug, predictable, optimize_for_size,
  // always_compact, jitless, lite_mode) are NOT set here — they go through the
  // process-level v8_jsi_set_v8_flags in makeV8Runtime (see applyV8Flags).
  if (args.preparedScriptStore) {
    V8ScriptCache::Create(cfg, args.preparedScriptStore);
  }
  if (args.foreground_task_runner) {
    V8TaskRunner::Create(cfg, args.foreground_task_runner);
  }
  if (args.startupSnapshotBlob && args.startupSnapshotBlob->size() > 0) {
    // The runtime needs the blob bytes for its whole lifetime, but `args` (and
    // this shared_ptr) only lives for the synchronous create call. Heap-hold a
    // copy of the shared_ptr and free it via the deleter the runtime fires on
    // destroy — mirrors the V8ScriptCache buffer-ownership pattern above.
    auto *held =
        new std::shared_ptr<const facebook::jsi::Buffer>(args.startupSnapshotBlob);
    v8_jsi_config_set_startup_snapshot(
        cfg,
        (*held)->data(),
        (*held)->size(),
        [](void * /*data*/, void *deleterData) {
          delete reinterpret_cast<
              std::shared_ptr<const facebook::jsi::Buffer> *>(deleterData);
        },
        held);
  }
}

// Configure trampoline: v8_create_runtime hands us a factory-owned config and
// we populate it from the V8RuntimeArgs carried in cb_data. The args outlive
// the synchronous create call, so the pointer stays valid. (Pull in the ABI
// does not mean pull in C++ — V8RuntimeArgs stays a plain struct.)
jsi_error_code JSI_CDECL configureFromArgs(void *cb_data, jsi_config cfg) {
  applyArgs(cfg, *static_cast<const V8RuntimeArgs *>(cb_data));
  return jsi_no_error;
}

// Forward the process-global V8 engine flags carried on V8RuntimeArgs to the
// process-level setter before the first runtime triggers V8 init. They are
// process-global and first-init-only; on later runtimes this is effectively a
// no-op (matching the historical per-config behavior). Static mutable storage
// because V8::SetFlagsFromCommandLine takes char** and may split "name=value"
// in place (these boolean flags have no '=', but static arrays keep us safe).
void applyV8Flags(const V8RuntimeArgs &args) {
  static char kSparkplug[] = "--sparkplug";
  static char kPredictable[] = "--predictable";
  static char kOptimizeForSize[] = "--optimize_for_size";
  static char kAlwaysCompact[] = "--always_compact";
  static char kJitless[] = "--jitless";
  static char kLiteMode[] = "--lite_mode";

  char *argv[6];
  size_t argc = 0;
  const auto &f = args.flags;
  if (f.sparkplug) argv[argc++] = kSparkplug;
  if (f.predictable) argv[argc++] = kPredictable;
  if (f.optimize_for_size) argv[argc++] = kOptimizeForSize;
  if (f.always_compact) argv[argc++] = kAlwaysCompact;
  if (f.jitless) argv[argc++] = kJitless;
  if (f.lite_mode) argv[argc++] = kLiteMode;
  if (argc == 0) return;

  // remove_flags=false: we pass only known flags, nothing to report back.
  v8_jsi_set_v8_flags(&argc, argv, /*remove_flags:*/ false);
}

} // namespace

std::unique_ptr<facebook::jsi::Runtime> __cdecl makeV8Runtime(
    V8RuntimeArgs &&args) {
  // Process-global engine flags must be set before the first runtime triggers
  // V8 init; per-runtime config flows through the configure callback.
  applyV8Flags(args);
  return ::jsi::abi::makeJsiAbiRuntime(
      &v8_create_runtime, &configureFromArgs, &args);
}

} // namespace v8runtime
