// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_3_callbacks_init
#include "js-native-api/3_callbacks/test.js.h"
#include "js-native-api/3_callbacks/binding.c"

using namespace napitest;

TEST_P(NapiTest, test_3_callbacks) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/binding",
        [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_3_callbacks_test_js);
  });
}
