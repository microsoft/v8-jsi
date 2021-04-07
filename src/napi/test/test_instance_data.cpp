// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

#define Init test_instance_data_init

// Redirect printf output to s_output.
// Define it before the includes.
#define printf(...) test_printf(s_output, __VA_ARGS__)
static std::string s_output;

#include "js-native-api/test_instance_data/test.js.h"
#include "js-native-api/test_instance_data/test_instance_data.c"

using namespace napitest;

TEST_P(NapiTest, test_instance_data) {
  s_output = "";
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_instance_data",
        [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_instance_data_test_js);
  });
  EXPECT_EQ(s_output, "deleting addon data\n");
}
