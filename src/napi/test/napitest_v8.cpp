// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include <gtest/gtest.h>
#include "napitest.h"

namespace napitest {

std::vector<NapiTestData> NapiEnvFactories() {
  return {{"jsi/napi/test/js-native-api", []() {
             napi_env env{};
             napi_ext_env_settings settings{};
             settings.this_size = sizeof(napi_ext_env_settings);
             settings.flags.enable_gc_api = true;
             napi_ext_create_env(&settings, &env);
             return env;
           }}};
}

} // namespace napitest
