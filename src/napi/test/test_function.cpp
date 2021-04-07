// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_function_init
#include "js-native-api/test_function/test.js.h"
#include "js-native-api/test_function/test_function.c"

using namespace napitest;

TEST_P(NapiTest, test_function) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_function",
        [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_function_test_js);
  });
}
