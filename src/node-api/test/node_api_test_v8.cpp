// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "node_api_test.h"
#include "v8_node_api.h"

namespace node_api_tests {

class V8RuntimeHolder : public IEnvHolder {
 public:
  V8RuntimeHolder(std::shared_ptr<NodeApiTaskRunner> taskRunner) noexcept {
    std::shared_ptr<NodeApiTaskRunner>* taskRunnerPtr =
        new std::shared_ptr<NodeApiTaskRunner>(std::move(taskRunner));
    v8_node_api_runtime_create(
        V8_NODE_API_ABI_VERSION, configure, taskRunnerPtr, &runtime_);
    v8_node_api_runtime_get_env(runtime_, &env_);
  }

  ~V8RuntimeHolder() { v8_node_api_runtime_destroy(runtime_); }

  V8RuntimeHolder(const V8RuntimeHolder&) = delete;
  V8RuntimeHolder& operator=(const V8RuntimeHolder&) = delete;

  napi_env getEnv() override { return env_; }

 private:
  static napi_status V8_NODE_API_CDECL configure(
      void* user_data,
      v8_node_api_config config) {
    napi_status status =
        v8_node_api_config_set_gc_api_enabled(config, true);
    return status == napi_ok
        ? v8_node_api_config_set_task_runner(
              config,
              NodeApiTaskRunner::PostTaskCallback,
              user_data,
              NodeApiTaskRunner::DeleteCallback,
              nullptr)
        : status;
  }

  v8_node_api_runtime runtime_{};
  napi_env env_{};
};

std::unique_ptr<IEnvHolder> CreateEnvHolder(
    std::shared_ptr<NodeApiTaskRunner> taskRunner) {
  return std::unique_ptr<IEnvHolder>(
      new V8RuntimeHolder(std::move(taskRunner)));
}

}  // namespace node_api_tests
