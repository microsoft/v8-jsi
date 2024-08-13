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
    jsr_config_set_task_runner(config,
                               taskRunnerPtr,
                               NodeApiTaskRunner::PostTaskCallback,
                               NodeApiTaskRunner::DeleteCallback,
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
