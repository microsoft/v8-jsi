#include <windows.h>
#include <TraceLoggingProvider.h>
#include <evntrace.h>

#ifdef __cplusplus
extern "C" {
#endif

TRACELOGGING_DECLARE_PROVIDER(g_hV8JSIRuntimeTraceLoggingProvider);
TRACELOGGING_DECLARE_PROVIDER(g_hV8JSIInspectorTraceLoggingProvider);

// Ref:
// https://docs.microsoft.com/en-us/windows-hardware/drivers/devtest/tracelogging-examples
#define TRACEV8RUNTIME_VERBOSE(eventName, ...)                      \
  TraceLoggingWrite(g_hV8JSIRuntimeTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_VERBOSE),         \
                    TraceLoggingInt32(instance_id_, "instanceId"),  \
                    __VA_ARGS__);

#define TRACEV8RUNTIME_WARNING(eventName, ...)                      \
  TraceLoggingWrite(g_hV8JSIRuntimeTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_WARNING),         \
                    TraceLoggingInt32(instance_id_, "instanceId"),  \
                    __VA_ARGS__);

#define TRACEV8RUNTIME_ERROR(eventName, ...)                        \
  TraceLoggingWrite(g_hV8JSIRuntimeTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_ERROR),           \
                    TraceLoggingInt32(instance_id_, "instanceId"),  \
                    __VA_ARGS__);

#define TRACEV8RUNTIME_CRITICAL(eventName, ...)                     \
  TraceLoggingWrite(g_hV8JSIRuntimeTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_CRITICAL),        \
                    TraceLoggingInt32(instance_id_, "instanceId"),  \
                    __VA_ARGS__);

// Ref:
// https://docs.microsoft.com/en-us/windows-hardware/drivers/devtest/tracelogging-examples
#define TRACEV8RUNTIME_VERBOSE_0(eventName, ...)                               \
  TraceLoggingWrite(g_hV8JSIRuntimeTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_VERBOSE),         \
                    TraceLoggingInt32(0, "instanceId"), __VA_ARGS__);

#define TRACEV8RUNTIME_WARNING_0(eventName, ...)                               \
  TraceLoggingWrite(g_hV8JSIRuntimeTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_WARNING),         \
                    TraceLoggingInt32(0, "instanceId"), __VA_ARGS__);

#define TRACEV8RUNTIME_ERROR_0(eventName, ...)                                 \
  TraceLoggingWrite(g_hV8JSIRuntimeTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_ERROR),           \
                    TraceLoggingInt32(0, "instanceId"), __VA_ARGS__);

#define TRACEV8RUNTIME_CRITICAL_0(eventName, ...)                              \
  TraceLoggingWrite(g_hV8JSIRuntimeTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_CRITICAL),        \
                    TraceLoggingInt32(0, "instanceId"), __VA_ARGS__);

// Ref:
// https://docs.microsoft.com/en-us/windows-hardware/drivers/devtest/tracelogging-examples
#define TRACEV8INSPECTOR_VERBOSE(eventName, instanceId, ...)          \
  TraceLoggingWrite(g_hV8JSIInspectorTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_VERBOSE),           \
                    TraceLoggingInt32(instanceId_, "instanceId"), __VA_ARGS__);

#define TRACEV8INSPECTOR_WARNING(eventName, ...)                      \
  TraceLoggingWrite(g_hV8JSIInspectorTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_WARNING),           \
                    TraceLoggingInt32(instanceId_, "instanceId"),     \
                    __VA_ARGS__);

#define TRACEV8INSPECTOR_ERROR(eventName, ...)                        \
  TraceLoggingWrite(g_hV8JSIInspectorTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_ERROR),             \
                    TraceLoggingInt32(instanceId_, "instanceId"),     \
                    __VA_ARGS__);

#define TRACEV8INSPECTOR_CRITICAL(eventName, ...)                     \
  TraceLoggingWrite(g_hV8JSIInspectorTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_CRITICAL),          \
                    TraceLoggingInt32(instanceId_, "instanceId"),     \
                    __VA_ARGS__);

#define TRACEV8INSPECTOR_VERBOSE_0(eventName, ...)          \
  TraceLoggingWrite(g_hV8JSIInspectorTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_VERBOSE),           \
                    TraceLoggingInt32(0, "instanceId"), __VA_ARGS__);

#define TRACEV8INSPECTOR_WARNING_0(eventName, ...)                      \
  TraceLoggingWrite(g_hV8JSIInspectorTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_WARNING),           \
                    TraceLoggingInt32(0, "instanceId"), __VA_ARGS__);

#define TRACEV8INSPECTOR_ERROR_0(eventName, ...)                        \
  TraceLoggingWrite(g_hV8JSIInspectorTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_ERROR),             \
                    TraceLoggingInt32(0, "instanceId"), __VA_ARGS__);

#define TRACEV8INSPECTOR_CRITICAL_0(eventName, ...)                     \
  TraceLoggingWrite(g_hV8JSIInspectorTraceLoggingProvider, eventName, \
                    TraceLoggingLevel(TRACE_LEVEL_CRITICAL),          \
                    TraceLoggingInt32(0, "instanceId"), __VA_ARGS__);


void globalInitializeTracing();

#ifdef __cplusplus
}  // extern "C"
#endif
