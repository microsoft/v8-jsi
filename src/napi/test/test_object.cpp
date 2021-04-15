// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_object_init
#include "js-native-api/test_object/test.js.h"
#include "js-native-api/test_object/test_null.c"
#include "js-native-api/test_object/test_null.js.h"
#include "js-native-api/test_object/test_object.c"

using namespace napitest;

TEST_P(NapiTest, test_object) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_object", [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_object_test_js);
  });
}

TEST_P(NapiTest, test_object_null) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_object", [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_object_test_null_js);
  });
}
