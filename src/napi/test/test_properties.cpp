// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_properties_init
#include "js-native-api/test_properties/test.js.h"
#include "js-native-api/test_properties/test_properties.c"

using namespace napitest;

TEST_P(NapiTest, test_properties) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_properties",
        [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_properties_test_js);
  });
}
