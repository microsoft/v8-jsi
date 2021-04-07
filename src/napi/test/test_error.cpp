// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_error_init
#include "js-native-api/test_error/test.js.h"
#include "js-native-api/test_error/test_error.c"

using namespace napitest;

TEST_P(NapiTest, test_error) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_error",
        [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_error_test_js);
  });
}
