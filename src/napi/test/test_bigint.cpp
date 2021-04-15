// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_bigint_init
#include "js-native-api/test_bigint/test.js.h"
#include "js-native-api/test_bigint/test_bigint.c"

using namespace napitest;

TEST_P(NapiTest, test_bigint) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_bigint", [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_bigint_test_js);
  });
}
