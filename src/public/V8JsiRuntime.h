// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#include <jsi/jsi.h>
#include <memory>

#ifndef V8JSI_EXPORT
#ifdef _MSC_VER
#ifdef BUILDING_V8JSI_SHARED
#define V8JSI_EXPORT __declspec(dllexport)
#else
#define V8JSI_EXPORT
#endif // BUILDING_V8JSI_SHARED
#else // _MSC_VER
#define V8JSI_EXPORT __attribute__((visibility("default")))
#endif // _MSC_VER
#endif // !defined(V8JSI_EXPORT)

namespace facebook {
namespace jsi {

struct PreparedScriptStore;

} // namespace jsi
} // namespace facebook

namespace v8runtime {

struct JSITask {
  virtual ~JSITask() = default;
  virtual void run() = 0;
};

struct JSITaskRunner {
  virtual ~JSITaskRunner() = default;
  virtual void postTask(std::unique_ptr<JSITask> task) = 0;
};

struct V8RuntimeArgs {
  // This is only used by the debugger: inspector needs to wake up the js thread for message dispatching
  // It cannot be done in an asynchronous lambda because V8Inspector internally uses "main thread" handles
  std::shared_ptr<JSITaskRunner> foreground_task_runner; // foreground === js_thread => sequential
  std::unique_ptr<facebook::jsi::PreparedScriptStore> preparedScriptStore;

  // To debug using vscode-node-adapter create a blank vscode workspace with the following launch.config and attach to
  // the runtime.
  // {
  // "version" : "0.2.0",
  // "configurations" : [
  //    {
  //      "name" : "Attach",
  //      "port" : 9223,
  //      "request" : "attach",
  //      "type" : "node",
  //      "protocol" : "inspector"
  //    },
  // ]
  //
  //
  // To debug with edge, navigate to "edge://inspect", and configure network target discovery by adding "localhost:9223"
  // to the list. Debug target should soon appear ( < 5 seconds). To debug with chrome, navigate to "chrome://inspect",
  // and follow the same step as above.
  uint16_t inspectorPort{9223};

  size_t initial_heap_size_in_bytes{0};
  size_t maximum_heap_size_in_bytes{0};

  // Set this to override the target name displayed in the debugger (to distinguish multiple parallel runtimes)
  std::string debuggerRuntimeName;

  // Padded to allow adding boolean flags without breaking the ABI
  union {
    struct {
      bool trackGCObjectStats : 1;
      bool enableJitTracing : 1;
      bool enableMessageTracing : 1;
      bool enableGCTracing : 1;
      bool enableInspector : 1;
      bool waitForDebugger : 1;
      bool enableGCApi : 1;
      bool ignoreUnhandledPromises : 1;
      bool enableSystemInstrumentation : 1; // Provider GUID "57277741-3638-4A4B-BDBA-0AC6E45DA56C"

      // Experimental flags (for memory-constrained optimization testing)
      bool sparkplug : 1; // https://v8.dev/blog/sparkplug
      bool predictable : 1; // take a big CPU hit to reduce the number of threads
      bool optimize_for_size : 1; // enables optimizations which favor memory size over execution speed
      bool always_compact : 1; // perform compaction on every full GC
      bool jitless : 1; // disable JIT entirely
      bool lite_mode : 1; // enables trade-off of performance for memory savings

      // unused padding to get better alignment at byte boundary
      bool padding : 1;

      // caps the number of worker threads (trade fewer threads for time)
      std::uint8_t thread_pool_size; // by default (0) V8 uses min(N-1,16) where N = number of cores
    } flags;
    uint32_t _flagspad{0};
  };
};

V8JSI_EXPORT std::unique_ptr<facebook::jsi::Runtime> __cdecl makeV8Runtime(V8RuntimeArgs &&args);

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
V8JSI_EXPORT void openInspector(facebook::jsi::Runtime &runtime);

// This API will be removed once we have a proper entry point for users to enable inspector on an active runtime.
// This function should be callable from any thread, including the synthetic WinDbg break thread.
// For example, break to the host app and issue this to open the inspector servers : .call
// v8jsi!v8runtime::openInspectors_toberemoved()
V8JSI_EXPORT void openInspectors_toberemoved();
#endif
} // namespace v8runtime
