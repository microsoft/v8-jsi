// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#include <jsi/jsi.h>
#include <memory>

namespace facebook {
namespace jsi {

struct PreparedScriptStore;
struct ScriptStore;

} // namespace jsi
} // namespace facebook

namespace v8 {
template <class T>
class Local;
class Context;
class Platform;
class Isolate;
class TaskRunner;
} // namespace v8

namespace v8runtime {

struct JSITask {
  virtual ~JSITask() = default;
  virtual void run() = 0;
};

struct JSIIdleTask {
  virtual ~JSIIdleTask() = default;
  virtual void run(double deadline_in_seconds) = 0;
};

struct JSITaskRunner {
  virtual ~JSITaskRunner() = default;
  virtual void postTask(std::unique_ptr<JSITask> task) = 0;
  virtual void postDelayedTask(
      std::unique_ptr<JSITask> task,
      double delay_in_seconds) = 0;
  virtual void postIdleTask(std::unique_ptr<JSIIdleTask> task) = 0;
  virtual bool IdleTasksEnabled() = 0;
};

class V8Platform;

enum class LogLevel {
  Trace = 0,
  Info = 1,
  Warning = 2,
  Error = 3,
  Fatal = 4,
};

using Logger = std::function<void(const char *message, LogLevel logLevel)>;

struct V8RuntimeArgs {
  std::shared_ptr<Logger> logger;

  std::unique_ptr<JSITaskRunner>
      foreground_task_runner; // foreground === js_thread => sequential

  // Sorry, currently we don't support providing custom background runner. We
  // create a default one shared by all runtimes. std::unique_ptr<TaskRunner>
  // background_task_runner; // background thread pool => non sequential

  std::unique_ptr<const facebook::jsi::Buffer> custom_snapshot_blob;

  std::unique_ptr<facebook::jsi::ScriptStore> scriptStore;
  std::unique_ptr<facebook::jsi::PreparedScriptStore> preparedScriptStore;

  bool liteMode{false};

  bool backgroundMode{false};

  // Enabling all the diagnostic and tracings by default so that we will be able to tune them and writing tools over them.
  bool trackGCObjectStats{true};
  bool enableTracing{true};
  bool enableJitTracing{true};
  bool enableMessageTracing{true};
  bool enableLog{true};
  bool enableGCTracing{true};

  // Enabling inspector by default. This will help in stabilizing inspector, and easily debug JS code when needed.
  // There shouldn't be any perf impacts until a debugger client is attached, except the overload of having a WebSocket port open, which should be very small.
  bool enableInspector{false};
  bool waitForDebugger{false};

  // To debug using vscode-node-adapter create a blank vscode workspace with the following launch.config and attach to the runtime.
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
  // To debug with edge, navigate to "edge:\\inspect", and configure network target discovery by adding "localhost:9223" to the list. Debug target should soon appear ( < 5 seconds).
  // To debug with chrome, navigate to "chrome:\\inspect", and follow the same step as above.
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
