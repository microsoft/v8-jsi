// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "V8JsiRuntime_impl.h"
#include <thread>
#include <stdexcept>

using namespace v8runtime;
using namespace facebook;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char **argv) {
  try {
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy{};
    require(GetProcessMitigationPolicy(GetCurrentProcess(), ProcessUserShadowStackPolicy, &policy, sizeof(policy)),
        "process shadow-stack policy query failed");
    int callbacks = 0;
    V8PlatformHolder::initializePlatform(1, [&] {
      ++callbacks;
      v8::V8::SetFlagsFromString("--print-flag-values");
      if (argc == 2) {
        const std::string flag(argv[1]);
        require(flag == "--cet-compatible" || flag == "--no-cet-compatible", "unexpected host flag");
        v8::V8::SetFlagsFromString(argv[1]);
      }
    });
    // Concurrent and later callers must not reapply defaults or initialization flags.
    std::vector<std::thread> callers;
    for (int i = 0; i < 4; ++i) callers.emplace_back([&] {
      V8PlatformHolder::initializePlatform(1, [&] { ++callbacks; });
    });
    for (auto &caller : callers) caller.join();
    auto first = makeV8Runtime({}), second = makeV8Runtime({});
    require(callbacks == 1, "platform initialization was repeated");
    require(second->evaluateJavaScript(std::make_shared<jsi::StringBuffer>("40 + 2"), "startup.js").getNumber() == 42,
        "runtime failed after process flag initialization");
    std::cout << "Startup shadow-stacks=" << policy.EnableUserShadowStack << " callbacks=" << callbacks << '\n';
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
