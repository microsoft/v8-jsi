// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#define NAPI_EXPERIMENTAL
#include "napitest.h"

using namespace napitest;

TEST_P(NapiTest, RunScriptTest) {
  ExecuteNapi([&](NapiTestContext *testContext, napi_env env) {
    int intValue{};
    napi_value script{}, scriptResult{}, global{}, xValue{};

    THROW_IF_NOT_OK(
        napi_create_string_utf8(env, "1", NAPI_AUTO_LENGTH, &script));
    THROW_IF_NOT_OK(napi_run_script(env, script, &scriptResult));
    THROW_IF_NOT_OK(napi_get_value_int32(env, scriptResult, &intValue));
    EXPECT_EQ(intValue, 1);

    THROW_IF_NOT_OK(
        napi_create_string_utf8(env, "x = 42", NAPI_AUTO_LENGTH, &script));
    THROW_IF_NOT_OK(napi_run_script(env, script, &scriptResult));
    THROW_IF_NOT_OK(napi_get_global(env, &global));
    THROW_IF_NOT_OK(napi_get_named_property(env, global, "x", &xValue));
    THROW_IF_NOT_OK(napi_get_value_int32(env, xValue, &intValue));
    EXPECT_EQ(intValue, 42);
  });
}
