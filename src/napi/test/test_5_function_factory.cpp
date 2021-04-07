// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_5_function_factory_init
#include "js-native-api/5_function_factory/test.js.h"
#include "js-native-api/5_function_factory/binding.c"

using namespace napitest;

TEST_P(NapiTest, test_5_function_factory) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/binding",
        [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_5_function_factory_test_js);
  });
}
