// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <queue>
#include "libplatform/libplatform.h"
#include "v8.h"

#include "public/V8JsiRuntime.h"

namespace v8runtime {

class ETWTracingController : public v8::TracingController {
 public:
  ETWTracingController(bool enabled) : enabled_(enabled) {}
  ~ETWTracingController() = default;

  const uint8_t *GetCategoryGroupEnabled(const char *category_group) override;

  uint64_t AddTraceEvent(
      char phase,
      const uint8_t *category_enabled_flag,
      const char *name,
      const char *scope,
      uint64_t id,
      uint64_t bind_id,
      int32_t num_args,
      const char **arg_names,
      const uint8_t *arg_types,
      const uint64_t *arg_values,
      std::unique_ptr<v8::ConvertableToTraceFormat> *arg_convertables,
      unsigned int flags) override;

  uint64_t AddTraceEventWithTimestamp(
      char phase,
      const uint8_t *category_enabled_flag,
      const char *name,
      const char *scope,
      uint64_t id,
      uint64_t bind_id,
      int32_t num_args,
      const char **arg_names,
      const uint8_t *arg_types,
      const uint64_t *arg_values,
      std::unique_ptr<v8::ConvertableToTraceFormat> *arg_convertables,
      unsigned int flags,
      int64_t timestamp) override;

  void UpdateTraceEventDuration(
      const uint8_t *category_enabled_flag,
      const char *name,
      uint64_t handle) override;

  void AddTraceStateObserver(
      v8::TracingController::TraceStateObserver *observer) override;

  void RemoveTraceStateObserver(
      v8::TracingController::TraceStateObserver *observer) override;

 private:
  // Disallow copy and assign
  ETWTracingController(const ETWTracingController &) = delete;
  void operator=(const ETWTracingController &) = delete;

  bool enabled_;
};

class WorkerThreadsTaskRunner : public v8::TaskRunner {
 public:
  WorkerThreadsTaskRunner();
  ~WorkerThreadsTaskRunner();

  void PostTask(std::unique_ptr<v8::Task> task) override;

  void PostDelayedTask(std::unique_ptr<v8::Task> task, double delay_in_seconds)
      override;

  void PostIdleTask(std::unique_ptr<v8::IdleTask> task) override {
    std::abort();
  }

  bool IdleTasksEnabled() override {
    return false;
  }

 private:
  void WorkerFunc();
  void TimerFunc();

 private:
  using DelayedEntry = std::pair<double, std::unique_ptr<v8::Task>>;

  // Define a comparison operator for the delayed_task_queue_ to make sure
  // that the unique_ptr in the DelayedEntry is not accessed in the priority
  // queue. This is necessary because we have to reset the unique_ptr when we
  // remove a DelayedEntry from the priority queue.
  struct DelayedEntryCompare {
    bool operator()(DelayedEntry &left, DelayedEntry &right) {
      return left.first > right.first;
    }
  };

  std::priority_queue<
      DelayedEntry,
      std::vector<DelayedEntry>,
      DelayedEntryCompare>
      delayed_task_queue_;
  std::queue<std::unique_ptr<v8::Task>> tasks_queue_;

  std::mutex queue_access_mutex_;
  std::condition_variable tasks_available_cond_;

  std::mutex delayed_queue_access_mutex_;
  std::condition_variable delayed_tasks_available_cond_;

  std::atomic<bool> stop_requested_{false};

  // TODO -- This should become a semaphore when we support more than one worker
  // thread.
  std::mutex worker_stopped_mutex_;
  std::condition_variable worker_stopped_cond_;
  bool worker_stopped_{false};

  std::mutex timer_stopped_mutex_;
  std::condition_variable timer_stopped_cond_;
  bool timer_stopped_{false};
};

class V8Platform : public v8::Platform {
 public:
  explicit V8Platform(bool enable_tracing);
  ~V8Platform() override;

  int NumberOfWorkerThreads() override;

  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(
      v8::Isolate *isolate) override;

  void CallOnWorkerThread(std::unique_ptr<v8::Task> task) override;
  void CallDelayedOnWorkerThread(
      std::unique_ptr<v8::Task> task,
      double delay_in_seconds) override;

  bool IdleTasksEnabled(v8::Isolate *isolate) override;

  double MonotonicallyIncreasingTime() override;
  double CurrentClockTimeMillis() override;
  v8::TracingController *GetTracingController() override;

 private:
  V8Platform(const V8Platform &) = delete;
  void operator=(const V8Platform &) = delete;

  std::unique_ptr<ETWTracingController> tracing_controller_;

  std::mutex foreground_task_runner_map_access_mutex;
  std::unique_ptr<WorkerThreadsTaskRunner> worker_task_runner_;

 public:
  static V8Platform &Get();
};

} // namespace v8runtime
