// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include <gtest/gtest.h>
#include "napitest.h"

namespace napitest {

std::vector<NapiEnvFactory> NapiEnvFactories() {
  return {[]() {
    napi_env env{};
    napi_ext_create_env(napi_ext_env_attribute_enable_gc_api, &env);
    return env;
  }};
}

} // namespace napitest
