// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "TaskRunner.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace v8runtime;
using namespace std::chrono_literals;
void require(bool value) { if (!value) throw std::runtime_error("task lifetime check failed"); }
struct Queue : JSITaskRunner {
  std::vector<std::unique_ptr<JSITask>> tasks;
  void postTask(std::unique_ptr<JSITask> task) override { tasks.push_back(std::move(task)); }
  void drain() { auto batch = std::move(tasks); for (auto &task : batch) task->run(); }
};
struct Callback : JSITask {
  std::function<void()> action;
  int &deleted;
  Callback(std::function<void()> callback, int &count) : action(std::move(callback)), deleted(count) {}
  ~Callback() override { ++deleted; }
  void run() override { action(); }
};
void queuedTasksAreCanceledAndDeleted() {
  auto queue = std::make_shared<Queue>();
  int ran = 0, deleted = 0;
  {
    GatedTaskRunner runner(queue);
    runner.postTask(std::make_unique<Callback>([&] { ++ran; }, deleted));
    queue->drain();
    require(ran == 1 && deleted == 1);
    runner.postTask(std::make_unique<Callback>([&] { ++ran; }, deleted));
    runner.close([] {});
  }
  queue->drain();
  require(ran == 1 && deleted == 2);
}
void teardownWaitsForOtherThreads() {
  auto queue = std::make_shared<Queue>();
  GatedTaskRunner runner(queue);
  int deleted = 0;
  std::promise<void> entered, resume, canceled;
  auto resumed = resume.get_future().share();
  runner.postTask(std::make_unique<Callback>([&] { entered.set_value(); resumed.wait(); }, deleted));
  auto run = std::async(std::launch::async, [&] { queue->drain(); });
  require(entered.get_future().wait_for(5s) == std::future_status::ready);
  auto close = std::async(std::launch::async, [&] { runner.close([&] { canceled.set_value(); }); });
  require(canceled.get_future().wait_for(5s) == std::future_status::ready);
  bool waited = close.wait_for(50ms) == std::future_status::timeout;
  resume.set_value();
  run.get(); close.get();
  require(waited && deleted == 1);
}
void ownTaskCanCloseAndSurvivorRemainsLive() {
  auto queue = std::make_shared<Queue>();
  GatedTaskRunner closing(queue), survivor(queue);
  int ran = 0, deleted = 0;
  closing.postTask(std::make_unique<Callback>([&] { closing.close([] {}); }, deleted));
  queue->drain();
  closing.postTask(std::make_unique<Callback>([&] { ran += 100; }, deleted));
  survivor.postTask(std::make_unique<Callback>([&] { ++ran; }, deleted));
  queue->drain();
  require(ran == 1 && deleted == 3);
}
void throwingTaskReleasesAdmission() {
  auto queue = std::make_shared<Queue>();
  GatedTaskRunner runner(queue);
  int deleted = 0;
  runner.postTask(std::make_unique<Callback>([] { throw std::runtime_error("callback"); }, deleted));
  bool caught = false;
  try { queue->drain(); } catch (const std::runtime_error &) { caught = true; }
  runner.close([] {});
  require(caught && deleted == 1);
}
int main() {
  try {
    queuedTasksAreCanceledAndDeleted(); teardownWaitsForOtherThreads();
    ownTaskCanCloseAndSurvivorRemainsLive(); throwingTaskReleasesAdmission();
    std::cout << "Task lifetime checks passed\n"; return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
