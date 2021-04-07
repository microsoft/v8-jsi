// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_symbol_init
#include "js-native-api/test_symbol/test1.js.h"
#include "js-native-api/test_symbol/test2.js.h"
#include "js-native-api/test_symbol/test3.js.h"
#include "js-native-api/test_symbol/test_symbol.c"

using namespace napitest;

TEST_P(NapiTest, test_symbol1) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_symbol",
        [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_symbol_test1_js);
  });
}

TEST_P(NapiTest, test_symbol2) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_symbol",
        [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_symbol_test2_js);
  });
}

TEST_P(NapiTest, test_symbol3) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_symbol",
        [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_symbol_test3_js);
  });
}
