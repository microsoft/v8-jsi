// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "node_api_test.h"
#include "v8_api.h"

namespace node_api_tests {

class V8RuntimeHolder : public IEnvHolder {
 public:
  V8RuntimeHolder(std::shared_ptr<NodeApiTaskRunner> taskRunner) noexcept {
    jsr_config config{};
    jsr_create_config(&config);
    jsr_config_enable_gc_api(config, true);
    std::shared_ptr<NodeApiTaskRunner>* taskRunnerPtr =
        new std::shared_ptr<NodeApiTaskRunner>(std::move(taskRunner));
    jsr_config_set_task_runner(
        config,
        taskRunnerPtr,
        [](void* task_runner_data,
           void* task_data,
           jsr_task_run_cb task_run_cb,
           jsr_data_delete_cb task_data_delete_cb,
           void* deleter_data) {
          NodeApiTaskRunner* taskRunnerPtr =
              static_cast<std::shared_ptr<NodeApiTaskRunner>*>(task_runner_data)
                  ->get();
          taskRunnerPtr->PostTask(
              [task_run_cb, task_data, task_data_delete_cb, deleter_data]() {
                if (task_run_cb != nullptr) {
                  task_run_cb(task_data);
                }
                if (task_data_delete_cb != nullptr) {
                  task_data_delete_cb(task_data, deleter_data);
                }
              });
        },
        [](void* data, void* /*deleter_data*/) {
          delete static_cast<std::shared_ptr<NodeApiTaskRunner>*>(data);
        },
        nullptr);
    jsr_create_runtime(config, &runtime_);
    jsr_delete_config(config);
    jsr_runtime_get_node_api_env(runtime_, &env_);
  }

  ~V8RuntimeHolder() { jsr_delete_runtime(runtime_); }

  V8RuntimeHolder(const V8RuntimeHolder&) = delete;
  V8RuntimeHolder& operator=(const V8RuntimeHolder&) = delete;

  napi_env getEnv() override { return env_; }

 private:
  jsr_runtime runtime_{};
  napi_env env_{};
};

std::unique_ptr<IEnvHolder> CreateEnvHolder(
    std::shared_ptr<NodeApiTaskRunner> taskRunner) {
  return std::unique_ptr<IEnvHolder>(
      new V8RuntimeHolder(std::move(taskRunner)));
}

}  // namespace node_api_tests
