// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include <gtest/gtest.h>
#include "napitest.h"
#include "v8_api.h"

#include <memory>
#include <vector>

namespace napitest {

class V8RuntimeHolder : public IEnvHolder {
 public:
  V8RuntimeHolder() noexcept {
    jsr_config config{};
    jsr_create_config(&config);
    jsr_config_enable_gc_api(config, true);
    jsr_create_runtime(config, &runtime_);
    jsr_delete_config(config);
    jsr_runtime_get_node_api_env(runtime_, &env_);
  }

  ~V8RuntimeHolder() {
    jsr_delete_runtime(runtime_);
  }

  V8RuntimeHolder(const V8RuntimeHolder &) = delete;
  V8RuntimeHolder &operator=(const V8RuntimeHolder &) = delete;

  napi_env getEnv() override {
    return env_;
  }

 private:
  jsr_runtime runtime_{};
  napi_env env_{};
};

std::vector<NapiTestData> NapiEnvFactories() {
  return {{"jsi/napi/test/js-native-api", [] { return std::unique_ptr<IEnvHolder>(new V8RuntimeHolder()); }}};
}

} // namespace napitest
