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

  bool trackGCObjectStats{false};

  bool backgroundMode{false};

  bool enableTracing{false};
  bool enableJitTracing{false};
  bool enableMessageTracing{false};
  bool enableLog{false};
  bool enableGCTracing{false};

  bool enableInspector{false};
  bool waitForDebugger{false};

  // chrome-devtools://devtools/bundled/inspector.html?experiments=true&v8only=true&ws=localhost:8888
  uint16_t inspectorPort{9229};

  size_t initial_heap_size_in_bytes{0};
  size_t maximum_heap_size_in_bytes{0};
};

#ifdef BUILDING_V8_SHARED
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
#endif
std::unique_ptr<facebook::jsi::Runtime> __cdecl makeV8Runtime(V8RuntimeArgs &&args);

// TODO :: Following should go to a more private header

constexpr int ISOLATE_DATA_SLOT = 0;

// Platform needs to map every isolate to this data.
struct IsolateData {
  void *foreground_task_runner_;

  // Weak reference.
  void *runtime_;
};

} // namespace v8runtime