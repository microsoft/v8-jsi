// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_4_object_factory_init
#include "js-native-api/4_object_factory/binding.c"
#include "js-native-api/4_object_factory/test.js.h"

using namespace napitest;

TEST_P(NapiTest, test_4_object_factory) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/binding", [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_4_object_factory_test_js);
  });
}
