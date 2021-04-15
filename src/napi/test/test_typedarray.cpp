// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_typedarray_init
#include "js-native-api/test_typedarray/test.js.h"
#include "js-native-api/test_typedarray/test_typedarray.c"

using namespace napitest;

TEST_P(NapiTest, test_typedarray) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_typedarray", [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_typedarray_test_js);
  });
}
