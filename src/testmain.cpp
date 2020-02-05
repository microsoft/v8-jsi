// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include <gtest/gtest.h>
#include "public/V8JsiRuntime.h"
#include "public/ScriptStore.h"
#include "jsi/test/testlib.h"

namespace facebook {
namespace jsi {

std::vector<facebook::jsi::RuntimeFactory> runtimeGenerators() {
  return {[]() -> std::unique_ptr<facebook::jsi::Runtime> {
    v8runtime::V8RuntimeArgs args;

    return v8runtime::makeV8Runtime(std::move(args));
  }};
}

}}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
