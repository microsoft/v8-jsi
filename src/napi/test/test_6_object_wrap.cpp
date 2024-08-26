// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_6_object_wrap_init
namespace {
#include "js-native-api/6_object_wrap/myobject.cc"
}

using namespace napitest;

TEST_P(NapiTest, test_6_object_wrap) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/binding", [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript("6_object_wrap/test.js");
  });
}
