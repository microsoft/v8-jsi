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
  std::shared_ptr<JSITaskRunner> foreground_task_runner; // foreground === js_thread => sequential
  std::unique_ptr<facebook::jsi::PreparedScriptStore> preparedScriptStore;

  // Enabling all the diagnostic and tracings by default so that we will be able to tune them and writing tools over
  // them.
  bool trackGCObjectStats{true};
  bool enableJitTracing{true};
  bool enableMessageTracing{true};
  bool enableGCTracing{true};
  bool enableInspector{false};
  bool waitForDebugger{false};

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

  bool enableGCApi{false};
  bool ignoreUnhandledPromises{false};
};

#ifdef BUILDING_V8_SHARED
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
#endif
    std::unique_ptr<facebook::jsi::Runtime> __cdecl makeV8Runtime(V8RuntimeArgs &&args);

} // namespace v8runtime
