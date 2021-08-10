// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include <gtest/gtest.h>
#include "jsi/test/testlib.h"
#include "public/NapiJsiRuntime.h"
#include "public/ScriptStore.h"
#include "public/V8JsiRuntime.h"

namespace facebook {
namespace jsi {

std::vector<facebook::jsi::RuntimeFactory> runtimeGenerators() {
  return {
      []() -> std::unique_ptr<facebook::jsi::Runtime> {
        v8runtime::V8RuntimeArgs args;
        return v8runtime::makeV8Runtime(std::move(args));
      },
      []() -> std::unique_ptr<facebook::jsi::Runtime> {
        napi_ext_env_settings settings{};
        settings.this_size = sizeof(napi_ext_env_settings);
        settings.flags.enable_gc_api = true;
        napi_env env{};
         napi_ext_create_env(&settings, &env);
        return Microsoft::JSI::MakeNapiJsiRuntime(env);
      }};
}

} // namespace jsi
} // namespace facebook

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
