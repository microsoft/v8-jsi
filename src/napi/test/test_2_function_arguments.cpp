// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_2_function_arguments_init
#include "js-native-api/2_function_arguments/binding.c"
#include "js-native-api/2_function_arguments/test.js.h"

using namespace napitest;

TEST_P(NapiTest, test_2_function_arguments) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/binding", [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_2_function_arguments_test_js);
  });
}
