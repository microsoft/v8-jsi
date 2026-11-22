// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#include "public/V8JsiRuntime.h"
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace v8runtime {

// v8-jsi security: queued tasks may outlive the runtime; teardown waits for
// other threads, but never waits for the task that is performing teardown.
class TaskGate {
 public:
  bool enter() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!alive_) return false;
    running_.push_back(std::this_thread::get_id());
    return true;
  }
  void leave() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_.erase(std::find(running_.begin(), running_.end(), std::this_thread::get_id()));
    cv_.notify_all();
  }
  template <class Cancel>
  void close(Cancel cancel) {
    std::unique_lock<std::mutex> lock(mutex_);
    alive_ = false;
    lock.unlock();
    cancel();
    lock.lock();
    cv_.wait(lock, [this] {
      return std::all_of(running_.begin(), running_.end(), [](auto id) {
        return id == std::this_thread::get_id();
      });
    });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool alive_{true};
  std::vector<std::thread::id> running_;
};

class GatedTask final : public JSITask {
 public:
  GatedTask(std::unique_ptr<JSITask> task, std::shared_ptr<TaskGate> gate)
      : task_(std::move(task)), gate_(std::move(gate)) {}
  void run() override {
    if (!gate_->enter()) return;
    struct Leave {
      TaskGate &gate;
      ~Leave() { gate.leave(); }
    } leave{*gate_};
    task_->run();
  }

 private:
  std::unique_ptr<JSITask> task_;
  std::shared_ptr<TaskGate> gate_;
};

class GatedTaskRunner final : public JSITaskRunner {
 public:
  explicit GatedTaskRunner(std::shared_ptr<JSITaskRunner> runner)
      : runner_(std::move(runner)) {}
  void postTask(std::unique_ptr<JSITask> task) override {
    if (runner_) runner_->postTask(std::make_unique<GatedTask>(std::move(task), gate_));
  }
  template <class Cancel>
  void close(Cancel cancel) { gate_->close(std::move(cancel)); }

 private:
  std::shared_ptr<JSITaskRunner> runner_;
  std::shared_ptr<TaskGate> gate_{std::make_shared<TaskGate>()};
};
} // namespace v8runtime
