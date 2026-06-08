// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// B1/B2 real-consumer smoke build. Validates the W9 .targets-driven consumer
// path end-to-end: this TU and the consumer-side V8JsiRuntime.cpp /
// JsiAbiRuntime.cpp / jsi.cpp (pulled in by Microsoft.JavaScript.V8.<rid>.targets)
// all compile against the shipped header layout, link against
// v8jsi.dll.lib, and run against v8jsi.dll at load time.
//
// B2 extends the smoke to exercise the Hybrid CRT cross-CRT boundary: when
// built /MDd (Debug-CRT) and linked against a Release Hybrid-CRT v8jsi.dll,
// every allocation/free shown below stays on the consumer-CRT side. The DLL
// sees only the C ABI vtable; STL types (std::string, jsi::JSError, etc.)
// never cross the boundary.
//
// Exit code:
//   0 = all checks passed.
//   1 = any failure (runtime construction, evaluation, type or value
//       mismatch, exception not propagated, etc.).

#include <V8JsiRuntime.h>
#include <jsi/jsi.h>

#include <cstdio>
#include <exception>
#include <memory>
#include <string>

namespace jsi = facebook::jsi;

#define FAIL(msg)                                       \
  do {                                                  \
    std::fprintf(stderr, "test-harness: %s\n", (msg));  \
    return 1;                                           \
  } while (0)

int main() {
  try {
    v8runtime::V8RuntimeArgs args;
    auto runtime = v8runtime::makeV8Runtime(std::move(args));
    if (!runtime) FAIL("makeV8Runtime returned null");

    // 1) Baseline number evaluation (B1).
    {
      auto src = std::make_shared<jsi::StringBuffer>("1 + 2");
      auto r = runtime->evaluateJavaScript(src, "smoke-num.js");
      if (!r.isNumber() || r.getNumber() != 3.0) FAIL("1 + 2 != 3");
    }

    // 2) utf8 round-trip. JSI String allocated DLL-side via the C ABI is
    //    surfaced to the consumer through Runtime::utf8 (defined in jsi.cpp,
    //    consumer-compiled). The returned std::string lives entirely in the
    //    consumer's CRT heap; the DLL never sees the std::string object.
    {
      auto src = std::make_shared<jsi::StringBuffer>("'hello, ' + 'world'");
      auto r = runtime->evaluateJavaScript(src, "smoke-str.js");
      if (!r.isString()) FAIL("expected string result");
      std::string utf8 = r.asString(*runtime).utf8(*runtime);
      if (utf8 != "hello, world") FAIL("utf8 round-trip mismatch");
    }

    // 3) Global property set/get. createPropNameIDFromAscii inside jsi.cpp
    //    allocates a PropNameID handle through the C ABI; setProperty /
    //    getProperty round-trip a number value.
    {
      runtime->global().setProperty(*runtime, "x", 42);
      auto v = runtime->global().getProperty(*runtime, "x");
      if (!v.isNumber() || v.getNumber() != 42.0) FAIL("global.x != 42");
    }

    // 4) HostFunction create + call. The std::function lambda captures
    //    consumer-side state (the call counter); v8jsi.dll invokes it
    //    through the C ABI host-function callback.
    {
      int calls = 0;
      auto fn = jsi::Function::createFromHostFunction(
          *runtime,
          jsi::PropNameID::forAscii(*runtime, "add"),
          2,
          [&calls](jsi::Runtime &rt, const jsi::Value &, const jsi::Value *a,
                   size_t n) -> jsi::Value {
            ++calls;
            if (n != 2 || !a[0].isNumber() || !a[1].isNumber())
              throw jsi::JSError(rt, "bad args");
            return jsi::Value(a[0].getNumber() + a[1].getNumber());
          });
      auto r = fn.call(*runtime, jsi::Value(7.0), jsi::Value(35.0));
      if (!r.isNumber() || r.getNumber() != 42.0) FAIL("host fn result != 42");
      if (calls != 1) FAIL("host fn call counter != 1");
    }

    // 5) JS exception caught consumer-side. JSError is constructed inside
    //    jsi.cpp (consumer-compiled) when the C ABI signals a pending
    //    exception via jsi_status; thrown and caught entirely in the
    //    consumer's CRT. v8jsi.dll never participates in the exception
    //    unwinding.
    {
      auto src = std::make_shared<jsi::StringBuffer>("throw new Error('boom')");
      bool caught = false;
      try {
        runtime->evaluateJavaScript(src, "smoke-throw.js");
      } catch (const jsi::JSError &) {
        caught = true;
      }
      if (!caught) FAIL("JSError was not propagated to consumer");
    }

    std::printf("test-harness: B2 cross-CRT smoke OK\n");
    return 0;
  } catch (const std::exception &ex) {
    std::fprintf(stderr, "test-harness: unexpected exception: %s\n", ex.what());
    return 1;
  } catch (...) {
    std::fprintf(stderr, "test-harness: unknown exception\n");
    return 1;
  }
}
