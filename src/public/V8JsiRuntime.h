// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#include <jsi/jsi.h>
#include <memory>

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

  // Padded to allow adding boolean flags without breaking the ABI
  union {
    struct {
      bool trackGCObjectStats:1;
      bool enableJitTracing:1;
      bool enableMessageTracing:1;
      bool enableGCTracing:1;
      bool enableInspector:1;
      bool waitForDebugger:1;
      bool enableGCApi:1;
      bool ignoreUnhandledPromises:1;

      // Experimental flags (for memory-constrained optimization testing)
      bool sparkplug:1; // https://v8.dev/blog/sparkplug
      bool predictable:1; // take a big CPU hit to reduce the number of threads
      bool optimize_for_size:1; // enables optimizations which favor memory size over execution speed
      bool always_compact:1; // perform compaction on every full GC
      bool jitless:1; // disable JIT entirely
    } flags;
    uint32_t _flagspad {0};
  };
};

#ifdef BUILDING_V8_SHARED
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
#endif
    std::unique_ptr<facebook::jsi::Runtime> __cdecl makeV8Runtime(V8RuntimeArgs &&args);

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
  __declspec(dllexport) void openInspector(facebook::jsi::Runtime& runtime);

  // This API will be removed once we have a proper entry point for users to enable inspector on an active runtime.
  // This function should be callable from any thread, including the synthetic WinDbg break thread.
  // For example, break to the host app and issue this to open the inspector servers : .call v8jsi!v8runtime::openInspectors_toberemoved()
  __declspec(dllexport) void openInspectors_toberemoved();
#endif
} // namespace v8runtime
