// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_7_factory_wrap_init
#include "js-native-api/7_factory_wrap/test.js.h"
namespace {
#include "js-native-api/7_factory_wrap/binding.cc"
#include "js-native-api/7_factory_wrap/myobject.cc"
} // namespace

using namespace napitest;

TEST_P(NapiTest, test_7_factory_wrap) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/binding", [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_7_factory_wrap_test_js);
  });
}
