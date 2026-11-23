// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "V8JsiRuntime.h"
#include <cstdio>

static std::unique_ptr<facebook::jsi::Runtime> runtime;
extern "C" bool legacy_create(unsigned short port) {
  try {
    v8runtime::V8RuntimeArgs args;
    args.flags.enableInspector = true;
    args.inspectorPort = port;
    runtime = v8runtime::makeV8Runtime(std::move(args));
    return true;
  } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return false; }
}
extern "C" bool legacy_eval() {
  try {
    return runtime->evaluateJavaScript(std::make_shared<facebook::jsi::StringBuffer>("21 * 2"), "legacy.js").getNumber() == 42;
  } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return false; }
}
extern "C" void legacy_destroy() { runtime.reset(); }
