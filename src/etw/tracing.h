// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#include "V8Windows.h"

#include <TraceLoggingProvider.h>
#include <evntrace.h>

#ifdef __cplusplus
extern "C" {
#endif

TRACELOGGING_DECLARE_PROVIDER(g_hV8JSIRuntimeTraceLoggingProvider);
TRACELOGGING_DECLARE_PROVIDER(g_hV8JSIInspectorTraceLoggingProvider);

// Ref: https://docs.microsoft.com/en-us/windows-hardware/drivers/devtest/tracelogging-examples
#define TRACEV8RUNTIME_VERBOSE(eventName, ...) \
  TraceLoggingWrite(                           \
      g_hV8JSIRuntimeTraceLoggingProvider, eventName, TraceLoggingLevel(TRACE_LEVEL_VERBOSE), __VA_ARGS__);

#define TRACEV8RUNTIME_WARNING(eventName, ...) \
  TraceLoggingWrite(                           \
      g_hV8JSIRuntimeTraceLoggingProvider, eventName, TraceLoggingLevel(TRACE_LEVEL_WARNING), __VA_ARGS__);

#define TRACEV8RUNTIME_ERROR(eventName, ...) \
  TraceLoggingWrite(g_hV8JSIRuntimeTraceLoggingProvider, eventName, TraceLoggingLevel(TRACE_LEVEL_ERROR), __VA_ARGS__);

#define TRACEV8RUNTIME_CRITICAL(eventName, ...) \
  TraceLoggingWrite(                            \
      g_hV8JSIRuntimeTraceLoggingProvider, eventName, TraceLoggingLevel(TRACE_LEVEL_CRITICAL), __VA_ARGS__);

// Ref:
// https://docs.microsoft.com/en-us/windows-hardware/drivers/devtest/tracelogging-examples
#define TRACEV8INSPECTOR_VERBOSE(eventName, ...) \
  TraceLoggingWrite(                             \
      g_hV8JSIInspectorTraceLoggingProvider, eventName, TraceLoggingLevel(TRACE_LEVEL_VERBOSE), __VA_ARGS__);

#define TRACEV8INSPECTOR_WARNING(eventName, ...) \
  TraceLoggingWrite(                             \
      g_hV8JSIInspectorTraceLoggingProvider, eventName, TraceLoggingLevel(TRACE_LEVEL_WARNING), __VA_ARGS__);

#define TRACEV8INSPECTOR_ERROR(eventName, ...) \
  TraceLoggingWrite(                           \
      g_hV8JSIInspectorTraceLoggingProvider, eventName, TraceLoggingLevel(TRACE_LEVEL_ERROR), __VA_ARGS__);

#define TRACEV8INSPECTOR_CRITICAL(eventName, ...) \
  TraceLoggingWrite(                              \
      g_hV8JSIInspectorTraceLoggingProvider, eventName, TraceLoggingLevel(TRACE_LEVEL_CRITICAL), __VA_ARGS__);

void globalInitializeTracing();

#ifdef __cplusplus
} // extern "C"
#endif
