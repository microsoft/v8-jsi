// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_date_init
#include "js-native-api/test_date/test.js.h"
#include "js-native-api/test_date/test_date.c"

using namespace napitest;

TEST_P(NapiTest, test_date) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_date",
        [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_date_test_js);
  });
}
