// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_number_init
#include "js-native-api/test_number/test.js.h"
#include "js-native-api/test_number/test_number.c"

using namespace napitest;

TEST_P(NapiTest, test_number) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_number", [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_number_test_js);
  });
}
