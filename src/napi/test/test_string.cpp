// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_string_init
#include "js-native-api/test_string/test.js.h"
#include "js-native-api/test_string/test_string.c"

using namespace napitest;

TEST_P(NapiTest, test_string) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_string",
        [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_string_test_js);
  });
}
