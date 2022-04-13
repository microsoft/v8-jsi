// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include <gtest/gtest.h>
#include "jsi/test/testlib.h"
#ifdef _WIN32
#include "public/NapiJsiRuntime.h"
#endif
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
#ifdef _WIN32
      []() -> std::unique_ptr<facebook::jsi::Runtime> {
        napi_ext_env_settings settings{};
        settings.this_size = sizeof(napi_ext_env_settings);
        settings.flags.enable_gc_api = true;
        napi_env env{};
         napi_ext_create_env(&settings, &env);
        return Microsoft::JSI::MakeNapiJsiRuntime(env);
      }
#endif
    };
}

} // namespace jsi
} // namespace facebook

TEST(Basic, CreateManyRuntimes) {
  for(size_t i = 0; i < 100; i++) {
    v8runtime::V8RuntimeArgs args;
    args.flags.enableInspector = true;
    auto runtime = v8runtime::makeV8Runtime(std::move(args));

    runtime->evaluateJavaScript(std::make_unique<facebook::jsi::StringBuffer>("x = 1"), "");
    EXPECT_EQ(runtime->global().getProperty(*runtime, "x").getNumber(), 1);
  }
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
