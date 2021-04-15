// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_promise_init
#include "js-native-api/test_promise/test.js.h"
#include "js-native-api/test_promise/test_promise.c"

using namespace napitest;

TEST_P(NapiTest, test_promise) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_promise", [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_promise_test_js);
  });
}
