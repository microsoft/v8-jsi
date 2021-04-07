// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <thread>
#include "napitest.h"

#define Init test_exception_init
#include "js-native-api/test_exception/test.js.h"
#include "js-native-api/test_exception/testFinalizerException.js.h"
#include "js-native-api/test_exception/test_exception.c"

using namespace napitest;

TEST_P(NapiTest, test_exception) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_exception",
        [](napi_env env, napi_value exports) { return Init(env, exports); });
    testContext->RunTestScript(test_exception_test_js);
  });
}

TEST_P(NapiTest, test_exception_finalizer) {
  auto spawnSyncCallback = [](napi_env env,
                              napi_callback_info /*info*/) -> napi_value {
    std::string error;
    auto childThread = std::thread([&error]() {
      ExecuteNapi([&error](NapiTestContext *testContext, napi_env env) {
        testContext->AddNativeModule(
            "./build/x86/test_exception", [](napi_env env, napi_value exports) {
              return Init(env, exports);
            });

        testContext->RunScript(R"(
          process = { argv:['', '', 'child'] };
        )");

        testContext->RunTestScript(test_exception_testFinalizerException_js)
            .Throws("Error", [&error](NapiTestException const &ex) noexcept {
              error = ex.ErrorInfo()->Message;
            });
      });
    });
    childThread.join();

    napi_value child{}, null{}, errValue{};
    THROW_IF_NOT_OK(napi_create_object(env, &child));
    THROW_IF_NOT_OK(napi_get_null(env, &null));
    THROW_IF_NOT_OK(napi_set_named_property(env, child, "signal", null));
    THROW_IF_NOT_OK(
        napi_create_string_utf8(env, error.c_str(), error.length(), &errValue));
    THROW_IF_NOT_OK(napi_set_named_property(env, child, "stderr", errValue));
    return child;
  };

  ExecuteNapi([&](NapiTestContext *testContext, napi_env env) {
    testContext->AddNativeModule(
        "./build/x86/test_exception",
        [](napi_env env, napi_value exports) { return Init(env, exports); });

    testContext->RunScript(R"(
      process = { argv:[] };
      __filename = '';
    )");

    testContext->AddNativeModule(
        "child_process", [&](napi_env env, napi_value exports) {
          napi_value spawnSync{};
          THROW_IF_NOT_OK(napi_create_function(
              env,
              "spawnSync",
              NAPI_AUTO_LENGTH,
              spawnSyncCallback,
              nullptr,
              &spawnSync));
          THROW_IF_NOT_OK(
              napi_set_named_property(env, exports, "spawnSync", spawnSync));
          return exports;
        });

    testContext->RunTestScript(test_exception_testFinalizerException_js);
  });
}
