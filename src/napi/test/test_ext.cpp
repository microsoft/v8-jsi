// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#define NAPI_EXPERIMENTAL
#include "napitest.h"

using namespace napitest;

TEST_P(NapiTest, test_ext_run_script) {
  ExecuteNapi([&](NapiTestContext *testContext, napi_env env) {
    int intValue{};
    napi_value script{}, scriptResult{}, global{}, xValue{};

    THROW_IF_NOT_OK(napi_create_string_utf8(env, "1", NAPI_AUTO_LENGTH, &script));
    THROW_IF_NOT_OK(napi_run_script(env, script, &scriptResult));
    THROW_IF_NOT_OK(napi_get_value_int32(env, scriptResult, &intValue));
    EXPECT_EQ(intValue, 1);

    THROW_IF_NOT_OK(napi_create_string_utf8(env, "x = 42", NAPI_AUTO_LENGTH, &script));
    THROW_IF_NOT_OK(napi_run_script(env, script, &scriptResult));
    THROW_IF_NOT_OK(napi_get_global(env, &global));
    THROW_IF_NOT_OK(napi_get_named_property(env, global, "x", &xValue));
    THROW_IF_NOT_OK(napi_get_value_int32(env, xValue, &intValue));
    EXPECT_EQ(intValue, 42);
  });
}

TEST_P(NapiTest, test_ext_unique_string) {
  ExecuteNapi([&](NapiTestContext *testContext, napi_env env) {
    napi_ext_ref utf8Str11{}, utf8Str12{}, utf8Str21{}, utf8Str22{}, utf8Str31{};

    THROW_IF_NOT_OK(napi_ext_get_unique_string_utf8_ref(env, "Hello", NAPI_AUTO_LENGTH, &utf8Str11));
    THROW_IF_NOT_OK(napi_ext_get_unique_string_utf8_ref(env, "Hello", NAPI_AUTO_LENGTH, &utf8Str12));
    EXPECT_NE(utf8Str11, nullptr);
    EXPECT_EQ(utf8Str11, utf8Str12);

    THROW_IF_NOT_OK(napi_ext_get_unique_string_utf8_ref(env, "Hello2", NAPI_AUTO_LENGTH, &utf8Str21));
    THROW_IF_NOT_OK(napi_ext_get_unique_string_utf8_ref(env, "Hello2", NAPI_AUTO_LENGTH, &utf8Str22));
    EXPECT_NE(utf8Str21, nullptr);
    EXPECT_NE(utf8Str11, utf8Str21);
    EXPECT_EQ(utf8Str21, utf8Str22);

    napi_value str3{};
    THROW_IF_NOT_OK(napi_create_string_latin1(env, "Hello", NAPI_AUTO_LENGTH, &str3));
    THROW_IF_NOT_OK(napi_ext_get_unique_string_ref(env, str3, &utf8Str31));
    EXPECT_EQ(utf8Str11, utf8Str31);
  });
}
