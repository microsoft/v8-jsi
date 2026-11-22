// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include <V8JsiRuntime.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
using Args = v8runtime::V8RuntimeArgs;
static_assert(JSI_VERSION == 20);
#if defined(_M_IX86)
constexpr size_t port = 16, name = 28, alignment = 4;
constexpr size_t size = _ITERATOR_DEBUG_LEVEL == 2 ? 60 : 56;
#else
constexpr size_t port = 32, name = 56, alignment = 8;
constexpr size_t size = _ITERATOR_DEBUG_LEVEL == 2 ? 104 : 96;
#endif
static_assert(sizeof(Args) == size);
static_assert(alignof(Args) == alignment);
static_assert(offsetof(Args, inspectorPort) == port);
static_assert(offsetof(Args, debuggerRuntimeName) == name);
static_assert(offsetof(Args, flags) == size - 8 || (sizeof(void*) == 4 && offsetof(Args, flags) == size - 4));
int main() {
  using Setter = void (*)(Args&);
  Setter setters[] = {
    [](Args &a) { a.flags.trackGCObjectStats = true; },
    [](Args &a) { a.flags.enableJitTracing = true; },
    [](Args &a) { a.flags.enableMessageTracing = true; },
    [](Args &a) { a.flags.enableGCTracing = true; },
    [](Args &a) { a.flags.enableInspector = true; },
    [](Args &a) { a.flags.waitForDebugger = true; },
    [](Args &a) { a.flags.enableGCApi = true; },
    [](Args &a) { a.flags.ignoreUnhandledPromises = true; },
    [](Args &a) { a.flags.enableSystemInstrumentation = true; },
    [](Args &a) { a.flags.sparkplug = true; },
    [](Args &a) { a.flags.predictable = true; },
    [](Args &a) { a.flags.optimize_for_size = true; },
    [](Args &a) { a.flags.always_compact = true; },
    [](Args &a) { a.flags.jitless = true; },
    [](Args &a) { a.flags.lite_mode = true; },
    [](Args &a) { a.flags.enableMultiThread = true; },
    [](Args &a) { a.flags.explicitMicrotaskPolicy = true; },
  };
  for (size_t i = 0; i < sizeof(setters) / sizeof(setters[0]); ++i) {
    Args args; setters[i](args);
    uint32_t bits = 0; std::memcpy(&bits, &args.flags, sizeof(bits));
    if (bits != (uint32_t{1} << i)) return 1;
  }
  Args args; args.flags.thread_pool_size = 0xa5;
  uint32_t bits = 0; std::memcpy(&bits, &args.flags, sizeof(bits));
  if (bits != 0xa5000000) return 2;
  std::printf("RuntimeArgs size=%zu align=%zu port=%zu name=%zu flags=%zu\n", sizeof(Args), alignof(Args),
              offsetof(Args, inspectorPort), offsetof(Args, debuggerRuntimeName), offsetof(Args, flags));
}
