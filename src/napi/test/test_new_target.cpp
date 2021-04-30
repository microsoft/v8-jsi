// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_new_target_init
#include "js-native-api/test_new_target/binding.c"
#include "js-native-api/test_new_target/test.js.h"

using namespace napitest;

TEST_P(NapiTest, test_new_target) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/binding", [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_new_target_test_js);
  });
}
