// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_8_passing_wrapped_init
#include "js-native-api/8_passing_wrapped/test.js.h"
namespace {
#include "js-native-api/8_passing_wrapped/binding.cc"
#include "js-native-api/8_passing_wrapped/myobject.cc"
} // namespace

using namespace napitest;

TEST_P(NapiTest, test_8_passing_wrapped) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/binding",
        [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_8_passing_wrapped_test_js);
  });
}
