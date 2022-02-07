// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_constructor_init
#include "js-native-api/test_constructor/test_constructor.c"

using namespace napitest;

TEST_P(NapiTest, test_constructor) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_constructor", [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript("test_constructor/test.js");
  });
}

TEST_P(NapiTest, test_constructor2) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_constructor", [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript("test_constructor/test2.js");
  });
}
