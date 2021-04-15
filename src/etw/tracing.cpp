#include "tracing.h"

// Define the GUID to use in TraceLoggingProviderRegister
// {85A99459-1F1D-49BD-B3DC-E5EF2AD0C2C8}
TRACELOGGING_DEFINE_PROVIDER(
    g_hV8JSIRuntimeTraceLoggingProvider,
    "Microsoft.V8JSIRuntime",
    (0x85a99459, 0x1f1d, 0x49bd, 0xb3, 0xdc, 0xe5, 0xef, 0x2a, 0xd0, 0xc2, 0xc8));

// Define the GUID to use in TraceLoggingProviderRegister
// {5509957C-25B6-4294-B2FA-8A8E41E6BC37}
TRACELOGGING_DEFINE_PROVIDER(
    g_hV8JSIInspectorTraceLoggingProvider,
    "Microsoft.V8JSIInspector",
    (0x5509957c, 0x25b6, 0x4294, 0xb2, 0xfa, 0x8a, 0x8e, 0x41, 0xe6, 0xbc, 0x37));

void globalInitializeTracing() {
#ifdef _WIN32
  TraceLoggingUnregister(g_hV8JSIRuntimeTraceLoggingProvider);
  TraceLoggingRegister(g_hV8JSIRuntimeTraceLoggingProvider);
  TraceLoggingUnregister(g_hV8JSIInspectorTraceLoggingProvider);
  TraceLoggingRegister(g_hV8JSIInspectorTraceLoggingProvider);
#endif

  TRACEV8RUNTIME_VERBOSE("Initializing providers.");
}
