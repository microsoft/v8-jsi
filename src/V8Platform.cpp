// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "V8Platform.h"

#include <algorithm>
#include <chrono>
#include <queue>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include "etw/tracing.h"
#endif

namespace v8runtime {

const uint8_t *ETWTracingController::GetCategoryGroupEnabled(
    const char *category_group) {
  static uint8_t value = enabled_ ? 1 : 0;
  return &value;
}

uint64_t ETWTracingController::AddTraceEvent(
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
    unsigned int flags) {
  std::vector<const char *> names(8);
  std::vector<uint8_t> types(8);
  std::vector<uint64_t> values(8);

  for (int i = 0; i < std::min(8, num_args); i++) {
    names[i] = arg_names[i];
    types[i] = arg_types[i];
    values[i] = arg_values[i];
  }

#if defined(_WIN32) && !defined(__clang__)
  EventWriteGENERIC_EVENT(
      phase,
      name,
      0,
      scope,
      id,
      bind_id,
      names[0],
      types[0],
      values[0],
      names[1],
      types[1],
      values[1],
      names[2],
      types[2],
      values[2],
      names[3],
      types[3],
      values[3],
      names[4],
      types[4],
      values[4],
      names[5],
      types[5],
      values[5],
      names[6],
      types[6],
      values[6],
      names[7],
      types[7],
      values[7],
      names[8],
      types[8],
      values[8]);
#endif

  return 0;
}

uint64_t ETWTracingController::AddTraceEventWithTimestamp(
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
    int64_t timestamp) {
  std::vector<const char *> names(8);
  std::vector<uint8_t> types(8);
  std::vector<uint64_t> values(8);

  for (int i = 0; i < std::min(8, num_args); i++) {
    names[i] = arg_names[i];
    types[i] = arg_types[i];
    values[i] = arg_values[i];
  }

#if defined(_WIN32) && !defined(__clang__)
  EventWriteGENERIC_EVENT(
      phase,
      name,
      timestamp,
      scope,
      id,
      bind_id,
      names[0],
      types[0],
      values[0],
      names[1],
      types[1],
      values[1],
      names[2],
      types[2],
      values[2],
      names[3],
      types[3],
      values[3],
      names[4],
      types[4],
      values[4],
      names[5],
      types[5],
      values[5],
      names[6],
      types[6],
      values[6],
      names[7],
      types[7],
      values[7],
      names[8],
      types[8],
      values[8]);
#endif

  return 0;
}

void ETWTracingController::UpdateTraceEventDuration(
    const uint8_t *category_enabled_flag,
    const char *name,
    uint64_t handle) {
#if defined(_WIN32) && !defined(__clang__)
  EventWriteUPDATE_TIMESTAMP(name, handle);
#endif
}

void ETWTracingController::AddTraceStateObserver(
    v8::TracingController::TraceStateObserver *observer) {}

void ETWTracingController::RemoveTraceStateObserver(
    v8::TracingController::TraceStateObserver *observer) {}

/*static*/ V8Platform &V8Platform::Get() {
  static V8Platform platform(false);
  return platform;
}

WorkerThreadsTaskRunner::~WorkerThreadsTaskRunner() {
  stop_requested_ = true;
  tasks_available_cond_.notify_all();
  delayed_tasks_available_cond_.notify_all();

  // Wait until both worker and timer threads are dranined.
  std::unique_lock<std::mutex> worker_stopped_lock(worker_stopped_mutex_);
  worker_stopped_cond_.wait(
      worker_stopped_lock, [this]() { return worker_stopped_; });

  std::unique_lock<std::mutex> timer_stopped_lock(timer_stopped_mutex_);
  timer_stopped_cond_.wait(
      timer_stopped_lock, [this]() { return timer_stopped_; });
}

WorkerThreadsTaskRunner::WorkerThreadsTaskRunner() {
  std::thread(&WorkerThreadsTaskRunner::WorkerFunc, this).detach();
  std::thread(&WorkerThreadsTaskRunner::TimerFunc, this).detach();
}

void WorkerThreadsTaskRunner::PostTask(std::unique_ptr<v8::Task> task) {
  {
    std::lock_guard<std::mutex> lock(queue_access_mutex_);
    tasks_queue_.push(std::move(task));
  }

  tasks_available_cond_.notify_all();
}

void WorkerThreadsTaskRunner::PostDelayedTask(
    std::unique_ptr<v8::Task> task,
    double delay_in_seconds) {
  {
    if (delay_in_seconds == 0) {
      PostTask(std::move(task));
      return;
    }

    double deadline =
        std::chrono::high_resolution_clock::now().time_since_epoch().count() +
        delay_in_seconds *
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::seconds(1))
                .count();

    std::lock_guard<std::mutex> lock(delayed_queue_access_mutex_);
    delayed_task_queue_.push(std::make_pair(deadline, std::move(task)));
    delayed_tasks_available_cond_.notify_all();
  }
}

void WorkerThreadsTaskRunner::WorkerFunc() {
  while (true) {
    std::unique_lock<std::mutex> lock(queue_access_mutex_);
    tasks_available_cond_.wait(
        lock, [this]() { return !tasks_queue_.empty() || stop_requested_; });

    if (stop_requested_)
      break;
    if (tasks_queue_.empty())
      continue;

    std::unique_ptr<v8::Task> nexttask = std::move(tasks_queue_.front());
    tasks_queue_.pop();

    lock.unlock();

    nexttask->Run();
  }

  worker_stopped_ = true;
  worker_stopped_cond_.notify_all();
}

void WorkerThreadsTaskRunner::TimerFunc() {
  while (true) {
    std::unique_lock<std::mutex> delayed_lock(delayed_queue_access_mutex_);
    delayed_tasks_available_cond_.wait(delayed_lock, [this]() {
      return !delayed_task_queue_.empty() || stop_requested_;
    });

    if (stop_requested_)
      break;

    if (delayed_task_queue_.empty())
      continue; // Loop back and block the thread.

    std::queue<std::unique_ptr<v8::Task>> new_ready_tasks;

    do {
      const DelayedEntry &delayed_entry = delayed_task_queue_.top();
      if (delayed_entry.first <
          std::chrono::steady_clock::now().time_since_epoch().count()) {
        new_ready_tasks.push(
            std::move(const_cast<DelayedEntry &>(delayed_entry).second));
        delayed_task_queue_.pop();
      } else {
        // The rest are not ready ..
        break;
      }

    } while (!delayed_task_queue_.empty());

    delayed_lock.unlock();

    if (!new_ready_tasks.empty()) {
      std::lock_guard<std::mutex> lock(queue_access_mutex_);

      do {
        tasks_queue_.push(std::move(new_ready_tasks.front()));
        new_ready_tasks.pop();
      } while (!new_ready_tasks.empty());

      tasks_available_cond_.notify_all();
    }

    if (!delayed_task_queue_.empty()) {
      std::this_thread::sleep_for(std::chrono::seconds(
          1)); // Wait for a second and loop back and recompute again.
    } // else loop back and block the thread.
  }

  timer_stopped_ = true;
  timer_stopped_cond_.notify_all();
}

V8Platform::V8Platform(bool enable_tracing)
    : tracing_controller_(
          std::make_unique<ETWTracingController>(enable_tracing)),
      worker_task_runner_(std::make_unique<WorkerThreadsTaskRunner>()) {}

V8Platform::~V8Platform() {}

std::shared_ptr<v8::TaskRunner> V8Platform::GetForegroundTaskRunner(
    v8::Isolate *isolate) {
  IsolateData *isolate_data =
      reinterpret_cast<IsolateData *>(isolate->GetData(ISOLATE_DATA_SLOT));
  return *reinterpret_cast<std::shared_ptr<v8::TaskRunner> *>(
      (isolate_data->foreground_task_runner_));
}

void V8Platform::CallOnWorkerThread(std::unique_ptr<v8::Task> task) {
  worker_task_runner_->PostTask(std::move(task));
}

void V8Platform::CallDelayedOnWorkerThread(
    std::unique_ptr<v8::Task> task,
    double delay_in_seconds) {
  worker_task_runner_->PostDelayedTask(std::move(task), delay_in_seconds);
}

void V8Platform::CallOnForegroundThread(v8::Isolate *isolate, v8::Task *task) {
  GetForegroundTaskRunner(isolate)->PostTask(std::unique_ptr<v8::Task>(task));
}

void V8Platform::CallDelayedOnForegroundThread(
    v8::Isolate *isolate,
    v8::Task *task,
    double delay_in_seconds) {
  // We don't need it as of now.
  std::abort();
}

void V8Platform::CallIdleOnForegroundThread(
    v8::Isolate *isolate,
    v8::IdleTask *task) {
  GetForegroundTaskRunner(isolate)->PostIdleTask(
      std::unique_ptr<v8::IdleTask>(task));
}

bool V8Platform::IdleTasksEnabled(v8::Isolate *isolate) {
  return GetForegroundTaskRunner(isolate)->IdleTasksEnabled();
}

int V8Platform::NumberOfWorkerThreads() {
  return 1;
}

double V8Platform::MonotonicallyIncreasingTime() {
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::high_resolution_clock::now().time_since_epoch())
          .count());
}

double V8Platform::CurrentClockTimeMillis() {
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::high_resolution_clock::now().time_since_epoch())
          .count());
}

v8::TracingController *V8Platform::GetTracingController() {
  return tracing_controller_.get();
}

} // namespace v8runtime
