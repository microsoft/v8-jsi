// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include <gtest/gtest.h>
#include "napitest.h"

namespace napitest {

std::vector<NapiEnvFactory> NapiEnvFactories() {
  return {[]() {
    napi_env env{};
    napi_ext_env_settings settings{};
    settings.this_size = sizeof(napi_ext_env_settings);
    settings.attributes = napi_ext_env_attribute_enable_gc_api;
    napi_ext_create_env(&settings, &env);
    return env;
  }};
}

} // namespace napitest
