// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "V8JsiRuntime_impl.h"

#include "libplatform/libplatform.h"
#include "v8.h"

#include "IsolateData.h"
#include "MurmurHash.h"
#include "node-api/js_native_api.h"
#include "node-api/js_native_api_v8.h"
#include "public/ScriptStore.h"

#include "node-api/util-inl.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <list>
#include <locale>
#include <mutex>
#include <sstream>

using namespace facebook;

namespace v8runtime {

thread_local uint16_t V8Runtime::tls_isolate_usage_counter_ = 0;

struct ContextEmbedderIndex {
  constexpr static int Runtime = 0;
  constexpr static int ContextTag = 1;
};

/*static*/ std::mutex V8PlatformHolder::mutex_s_;
/*static*/ std::unique_ptr<v8::Platform> V8PlatformHolder::platform_s_;
/*static*/ bool V8PlatformHolder::is_initialized_s_{false};
/*static*/ bool V8PlatformHolder::is_disposed_s_{false};

// String utilities
std::string JSStringToSTLString(v8::Isolate *isolate, v8::Local<v8::String> string) {
  int utfLen = string->Utf8Length(isolate);
  std::string result;
  result.resize(utfLen);
  string->WriteUtf8(isolate, &result[0], utfLen);
  return result;
}

std::u16string JSStringToStlU16String(v8::Isolate *isolate, v8::Local<v8::String> string) {
  int utf16Len = string->Length();
  std::u16string result(utf16Len, '\0');
  string->Write(isolate, reinterpret_cast<uint16_t *>(&result[0]), 0, utf16Len, v8::String::NO_NULL_TERMINATION);
  return result;
}

namespace {

// Extracts a C string from a V8 Utf8Value.
const char *ToCString(const v8::String::Utf8Value &value) {
  return *value ? *value : "<string conversion failed>";
}
} // namespace

/*static*/ int const V8Runtime::RuntimeContextTag = 0x7e7f75;
/*static*/ void *const V8Runtime::RuntimeContextTagPtr =
    const_cast<void *>(static_cast<const void *>(&V8Runtime::RuntimeContextTag));

void V8Runtime::AddHostObjectLifetimeTracker(std::shared_ptr<HostObjectLifetimeTracker> hostObjectLifetimeTracker) {
  // Note that we are letting the list grow in definitely as of now.. The list
  // gets cleaned up when the runtime is teared down.
  // TODO :: We should remove entries from the list as the objects are garbage
  // collected.
  host_object_lifetime_tracker_list_.push_back(hostObjectLifetimeTracker);
}

/*static */ void V8Runtime::OnMessage(v8::Local<v8::Message> message, v8::Local<v8::Value> error) {
  v8::Isolate *isolate = v8::Isolate::GetCurrent();

  v8::String::Utf8Value msg(isolate, message->Get());
  v8::String::Utf8Value source_line(isolate, message->GetSourceLine(isolate->GetCurrentContext()).ToLocalChecked());

#ifdef _WIN32
  TRACEV8RUNTIME_VERBOSE(
      "V8::MessageFrom",
      TraceLoggingString(*msg, "message"),
      TraceLoggingString(*source_line, "source_line"),
      TraceLoggingInt32(message->GetLineNumber(isolate->GetCurrentContext()).ToChecked(), "Line"),
      TraceLoggingInt32(message->GetStartPosition(), "StartPos"),
      TraceLoggingInt32(message->GetEndPosition(), "EndPos"),
      TraceLoggingInt32(message->ErrorLevel(), "ErrorLevel"),
      TraceLoggingInt32(message->GetStartColumn(), "StartCol"),
      TraceLoggingInt32(message->GetEndColumn(), "EndCol"));
#endif
}

size_t V8Runtime::NearHeapLimitCallback(void *raw_state, size_t current_heap_limit, size_t initial_heap_limit) {
#ifdef _WIN32
  TRACEV8RUNTIME_VERBOSE(
      "V8::NearHeapLimitCallback",
      TraceLoggingInt64(current_heap_limit, "current_heap_limit"),
      TraceLoggingInt64(initial_heap_limit, "initial_heap_limit"));
#endif
  // Add 5MB.
  return current_heap_limit + 5 * 1024 * 1024;
}

static std::string GCTypeToString(std::string &prefix, v8::GCType type, v8::GCCallbackFlags gcflags) {
  switch (type) {
    case v8::GCType::kGCTypeScavenge:
      prefix += ",Scavenge ";
      break;
    case v8::GCType::kGCTypeIncrementalMarking:
      prefix += ",IncrementalMarking";
      break;
    case v8::GCType::kGCTypeMarkSweepCompact:
      prefix += ",MarkSweepCompact";
      break;
    case v8::GCType::kGCTypeProcessWeakCallbacks:
      prefix += ",ProcessWeakCallbacks";
      break;
    default:
      prefix += ",";
      prefix += std::to_string(type);
  }

  switch (gcflags) {
    case v8::GCCallbackFlags::kGCCallbackFlagCollectAllAvailableGarbage:
      prefix += ",AllGarbage";
      break;
    case v8::GCCallbackFlags::kGCCallbackFlagCollectAllExternalMemory:
      prefix += ",AllExternalMemory";
      break;
    case v8::GCCallbackFlags::kGCCallbackFlagConstructRetainedObjectInfos:
      prefix += ",ConstructRetainedObjectInfos";
      break;
    case v8::GCCallbackFlags::kGCCallbackFlagForced:
      prefix += ",Forces";
      break;
    case v8::GCCallbackFlags::kGCCallbackFlagSynchronousPhantomCallbackProcessing:
      prefix += ",SynchronousPhantomCallbackProcessing";
      break;
    case v8::GCCallbackFlags::kGCCallbackScheduleIdleGarbageCollection:
      prefix += ",ScheduleIdleGarbageCollection";
      break;
    default:
      prefix += ",";
      prefix += std::to_string(type);
  }

  return prefix;
}

void V8Runtime::GCPrologueCallback(v8::Isolate *isolate, v8::GCType type, v8::GCCallbackFlags flags) {
  std::string prefix("GCPrologue");
  std::string gcTypeString = GCTypeToString(prefix, type, flags);
  TRACEV8RUNTIME_VERBOSE("V8::GCPrologueCallback", TraceLoggingString(gcTypeString.c_str(), "GCType"));
  DumpCounters(gcTypeString.c_str());
}

void V8Runtime::GCEpilogueCallback(v8::Isolate *isolate, v8::GCType type, v8::GCCallbackFlags flags) {
  std::string prefix("GCEpilogue");
  std::string gcTypeString = GCTypeToString(prefix, type, flags);
  TRACEV8RUNTIME_VERBOSE("V8::GCEpilogueCallback", TraceLoggingString(gcTypeString.c_str(), "GCType"));

  DumpCounters(gcTypeString.c_str());
}

CounterMap *V8Runtime::counter_map_;
char V8Runtime::counters_file_[sizeof(CounterCollection)];
CounterCollection V8Runtime::local_counters_;
CounterCollection *V8Runtime::counters_ = &local_counters_;

int32_t *Counter::Bind(const char *name, bool is_histogram) {
  int i;
  for (i = 0; i < kMaxNameSize - 1 && name[i]; i++)
    name_[i] = static_cast<char>(name[i]);
  name_[i] = '\0';
  is_histogram_ = is_histogram;
  return ptr();
}

void Counter::AddSample(int32_t sample) {
  count_++;
  sample_total_ += sample;
}

CounterCollection::CounterCollection() {
  magic_number_ = 0xDEADFACE;
  max_counters_ = kMaxCounters;
  max_name_size_ = Counter::kMaxNameSize;
  counters_in_use_ = 0;
}

Counter *CounterCollection::GetNextCounter() {
  if (counters_in_use_ == kMaxCounters)
    return nullptr;
  return &counters_[counters_in_use_++];
}

void V8Runtime::DumpCounters(const char *when) {
  static int cookie = 0;
  cookie++;
#ifdef _WIN32
  for (std::pair<std::string, Counter *> element : *counter_map_) {
    TRACEV8RUNTIME_VERBOSE(
        "V8::PerfCounters",
        TraceLoggingString(when, "when"),
        TraceLoggingInt32(cookie, "cookie"),
        TraceLoggingString(element.first.c_str(), "name"),
        TraceLoggingInt32(element.second->count(), "count"),
        TraceLoggingInt32(element.second->sample_total(), "sample_total"),
        TraceLoggingBool(element.second->is_histogram(), "is_histogram"));
  }
#endif
}

void V8Runtime::MapCounters(v8::Isolate *isolate, const char *name) {
  // counters_file_ = base::OS::MemoryMappedFile::create(
  //	name, sizeof(CounterCollection), &local_counters_);
  // void* memory =
  //	(counters_file_ == nullptr) ? nullptr : counters_file_->memory();
  // if (memory == nullptr) {
  //	printf("Could not map counters file %s\n", name);
  //	base::OS::ExitProcess(1);
  //}
  // counters_ = static_cast<CounterCollection*>(memory);
  counters_ = reinterpret_cast<CounterCollection *>(&counters_file_);
  isolate->SetCounterFunction(LookupCounter);
  isolate->SetCreateHistogramFunction(CreateHistogram);
  isolate->SetAddHistogramSampleFunction(AddHistogramSample);
}

Counter *V8Runtime::GetCounter(const char *name, bool is_histogram) {
  auto map_entry = counter_map_->find(name);
  Counter *counter = map_entry != counter_map_->end() ? map_entry->second : nullptr;

  if (counter == nullptr) {
    counter = counters_->GetNextCounter();
    if (counter != nullptr) {
      (*counter_map_)[name] = counter;
      counter->Bind(name, is_histogram);
    }
  } else {
    assert(counter->is_histogram() == is_histogram);
  }
  return counter;
}

int *V8Runtime::LookupCounter(const char *name) {
  Counter *counter = GetCounter(name, false);

  if (counter != nullptr) {
    return counter->ptr();
  } else {
    return nullptr;
  }
}

void *V8Runtime::CreateHistogram(const char *name, int min, int max, size_t buckets) {
  return GetCounter(name, true);
}

void V8Runtime::AddHistogramSample(void *histogram, int sample) {
  Counter *counter = reinterpret_cast<Counter *>(histogram);
  counter->AddSample(sample);
}

// This class is used to record the JITed code position info for JIT
// code profiling.
class JITCodeLineInfo {
 public:
  JITCodeLineInfo() {}

  void SetPosition(intptr_t pc, int pos) {
    AddCodeLineInfo(LineNumInfo(pc, pos));
  }

  struct LineNumInfo {
    LineNumInfo(intptr_t pc, int pos) : pc_(pc), pos_(pos) {}

    intptr_t pc_;
    int pos_;
  };

  std::list<LineNumInfo> *GetLineNumInfo() {
    return &line_num_info_;
  }

 private:
  void AddCodeLineInfo(const LineNumInfo &line_info) {
    line_num_info_.push_back(line_info);
  }
  std::list<LineNumInfo> line_num_info_;
};

struct SameCodeObjects {
  bool operator()(void *key1, void *key2) const {
    return key1 == key2;
  }
};

/*static */ void V8Runtime::JitCodeEventListener(const v8::JitCodeEvent *event) {
  switch (event->type) {
    case v8::JitCodeEvent::CODE_ADDED:
#ifdef _WIN32
      TRACEV8RUNTIME_VERBOSE(
          "V8::JIT",
          TraceLoggingString("CODE_ADDED", "type"),
          TraceLoggingString(
              event->code_type == v8::JitCodeEvent::CodeType::BYTE_CODE ? "BYTE_CODE" : "JIT_CODE", "cookie"),
          TraceLoggingString(std::string(event->name.str, event->name.len).c_str(), "name"));
#endif
      break;

    case v8::JitCodeEvent::CODE_ADD_LINE_POS_INFO: {
      JITCodeLineInfo *line_info = reinterpret_cast<JITCodeLineInfo *>(event->user_data);
      if (line_info != NULL) {
        line_info->SetPosition(static_cast<intptr_t>(event->line_info.offset), static_cast<int>(event->line_info.pos));
      }
      break;
    }
    case v8::JitCodeEvent::CODE_START_LINE_INFO_RECORDING: {
      v8::JitCodeEvent *temp_event = const_cast<v8::JitCodeEvent *>(event);
      temp_event->user_data = new JITCodeLineInfo();
      break;
    }
    case v8::JitCodeEvent::CODE_END_LINE_INFO_RECORDING: {
      JITCodeLineInfo *line_info = reinterpret_cast<JITCodeLineInfo *>(event->user_data);

      std::list<JITCodeLineInfo::LineNumInfo> *lineinfos = line_info->GetLineNumInfo();

      std::string code_details;

      std::list<JITCodeLineInfo::LineNumInfo>::iterator iter;
      for (iter = lineinfos->begin(); iter != lineinfos->end(); iter++) {
        code_details.append(std::to_string(iter->pc_) + ":");
        code_details.append(std::to_string(iter->pos_) + ":");
      }
#ifdef _WIN32
      TRACEV8RUNTIME_VERBOSE(
          "V8::JIT",
          TraceLoggingString("CODE_END_LINE_INFO_RECORDING", "type"),
          TraceLoggingString(
              event->code_type == v8::JitCodeEvent::CodeType::BYTE_CODE ? "BYTE_CODE" : "JIT_CODE", "cookie"),
          TraceLoggingString(code_details.c_str(), "code_details"));
#endif

      break;
    }
    default:
#ifdef _WIN32
      TRACEV8RUNTIME_VERBOSE(
          "V8::JIT",
          TraceLoggingString("DEF", "type"),
          TraceLoggingString(
              event->code_type == v8::JitCodeEvent::CodeType::BYTE_CODE ? "BYTE_CODE" : "JIT_CODE", "cookie"));
#endif
      break;
  }
}

v8::Isolate *V8Runtime::CreateNewIsolate() {
  TRACEV8RUNTIME_VERBOSE("CreateNewIsolate", TraceLoggingString("start", "op"));

  // One per each runtime.
  create_params_.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();

  if (args_.initial_heap_size_in_bytes > 0 || args_.maximum_heap_size_in_bytes > 0) {
    v8::ResourceConstraints constraints;
    constraints.ConfigureDefaultsFromHeapSize(args_.initial_heap_size_in_bytes, args_.maximum_heap_size_in_bytes);

    create_params_.constraints = constraints;
  }

  counter_map_ = new CounterMap();
  if (args_.flags.trackGCObjectStats) {
    create_params_.counter_lookup_callback = LookupCounter;
    create_params_.create_histogram_callback = CreateHistogram;
    create_params_.add_histogram_sample_callback = AddHistogramSample;
  }

  isolate_ = v8::Isolate::Allocate();
  if (isolate_ == nullptr)
    std::abort();

  isolate_data_ = new v8runtime::IsolateData(isolate_, args_.foreground_task_runner);
  isolate_->SetData(v8runtime::ISOLATE_DATA_SLOT, isolate_data_);

  v8::Isolate::Initialize(isolate_, create_params_);

  isolate_data_->CreateProperties();

  if (!args_.flags.ignoreUnhandledPromises) {
    isolate_->SetPromiseRejectCallback(PromiseRejectCallback);
  }

  if (args_.flags.trackGCObjectStats) {
    MapCounters(isolate_, "v8jsi");
  }

  if (args_.flags.enableJitTracing) {
    isolate_->SetJitCodeEventHandler(v8::kJitCodeEventDefault, JitCodeEventListener);
  }

  if (args_.flags.enableMessageTracing) {
    isolate_->AddMessageListener(OnMessage);
  }

  if (args_.flags.enableGCTracing) {
    isolate_->AddGCPrologueCallback(GCPrologueCallback);
    isolate_->AddGCEpilogueCallback(GCEpilogueCallback);
  }

  isolate_->AddNearHeapLimitCallback(NearHeapLimitCallback, nullptr);

  // TODO :: Toggle for ship builds.
  isolate_->SetAbortOnUncaughtExceptionCallback([](v8::Isolate *) { return true; });

  TRACEV8RUNTIME_VERBOSE("CreateNewIsolate", TraceLoggingString("end", "op"));
  DumpCounters("isolate_created");

  return isolate_;
}

void V8Runtime::createHostObjectConstructorPerContext() {
  // Create and keep the constructor for creating Host objects.
  v8::Local<v8::FunctionTemplate> constructorForHostObjectTemplate = v8::FunctionTemplate::New(GetIsolate());
  v8::Local<v8::ObjectTemplate> hostObjectTemplate = constructorForHostObjectTemplate->InstanceTemplate();
  hostObjectTemplate->SetHandler(v8::NamedPropertyHandlerConfiguration(
      HostObjectProxy::Get, HostObjectProxy::Set, nullptr, nullptr, HostObjectProxy::Enumerator));

  // V8 distinguishes between named properties (strings and symbols) and indexed properties (number)
  // Note that we're not passing an Enumerator here, otherwise we'd be double-counting since JSI doesn't make the
  // distinction
  hostObjectTemplate->SetIndexedPropertyHandler(HostObjectProxy::GetIndexed, HostObjectProxy::SetIndexed);
  hostObjectTemplate->SetInternalFieldCount(1);
  host_object_constructor_.Reset(
      GetIsolate(), constructorForHostObjectTemplate->GetFunction(GetContextLocal()).ToLocalChecked());
}

void V8Runtime::initializeV8() {
  V8PlatformHolder::initializePlatform(args_.flags.thread_pool_size, [this]() {
#ifdef _WIN32
    globalInitializeTracing();

    v8::V8::SetUnhandledExceptionCallback([](_EXCEPTION_POINTERS *exception_pointers) -> int {
      TRACEV8RUNTIME_CRITICAL("V8::SetUnhandledExceptionCallback");
      return 0;
    });
#endif

    // We are only allowed to set the flags the first time V8 is being initialized
    std::vector<const char *> argv;
    argv.push_back("v8jsi");

    if (args_.flags.trackGCObjectStats)
      argv.push_back("--track_gc_object_stats");

    if (args_.flags.enableGCApi)
      argv.push_back("--expose_gc");

    if (args_.flags.enableSystemInstrumentation)
      argv.push_back("--enable-system-instrumentation");

    if (args_.flags.sparkplug)
      argv.push_back("--sparkplug");

    if (args_.flags.predictable)
      argv.push_back("--predictable");

    if (args_.flags.optimize_for_size)
      argv.push_back("--optimize_for_size");

    if (args_.flags.always_compact)
      argv.push_back("--always_compact");

    if (args_.flags.jitless)
      argv.push_back("--jitless");

    if (args_.flags.lite_mode)
      argv.push_back("--lite_mode");

    int argc = static_cast<int>(argv.size());
    v8::V8::SetFlagsFromCommandLine(&argc, const_cast<char **>(&argv[0]), false);
  });
}

V8Runtime::V8Runtime(V8RuntimeArgs &&args) : args_(std::move(args)) {
  initializeV8();

  TRACEV8RUNTIME_VERBOSE("Initializing");

  // Try to reuse the already existing isolate in this thread.
  if (v8::Isolate::TryGetCurrent()) {
    TRACEV8RUNTIME_WARNING("Reusing existing V8 isolate in the current thread !");
    tls_isolate_usage_counter_++;
    isolate_ = v8::Isolate::GetCurrent();
  } else {
    CreateNewIsolate();
  }

  instrumentation_ = std::make_unique<V8Instrumentation>(isolate_);

  if (args_.flags.explicitMicrotaskPolicy) {
    isolate_->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  }

  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handleScope(isolate_);

  auto context = CreateContext(isolate_);
  context_.Reset(isolate_, context);

  // Associate the Runtime with the V8 context
  context->SetAlignedPointerInEmbedderData(ContextEmbedderIndex::Runtime, this);
  // Used by Environment::GetCurrent to know that we are on a node context.
  context->SetAlignedPointerInEmbedderData(ContextEmbedderIndex::ContextTag, V8Runtime::RuntimeContextTagPtr);

  v8::Context::Scope context_scope(context);

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
  void *inspector_agent = isolate_->GetData(ISOLATE_INSPECTOR_SLOT);
  if (inspector_agent) {
    inspector_agent_ = reinterpret_cast<inspector::Agent *>(inspector_agent)->getShared();
  } else {
    inspector_agent_ = std::make_shared<inspector::Agent>(isolate_, args_.inspectorPort);
    isolate_->SetData(ISOLATE_INSPECTOR_SLOT, inspector_agent_.get());
  }

  const char *context_name =
      args_.debuggerRuntimeName.empty() ? "JSIRuntime context" : args_.debuggerRuntimeName.c_str();
  inspector_agent_->addContext(GetContextLocal(), context_name);

  if (args_.flags.enableInspector) {
    TRACEV8RUNTIME_VERBOSE("Inspector enabled");
    inspector_agent_->start();

    if (args_.flags.waitForDebugger) {
      TRACEV8RUNTIME_VERBOSE("Waiting for inspector frontend to attach");
      inspector_agent_->waitForDebugger();
    }
  }
#endif

  createHostObjectConstructorPerContext();
}

V8Runtime::~V8Runtime() {
  // TODO: add check that destruction happens on the same thread id as
  // construction

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
  {
    if (inspector_agent_) {
      IsolateLocker isolate_locker(this);
      inspector_agent_->removeContext(GetContextLocal());
    }
  }
#endif

  host_object_constructor_.Reset();
  context_.Reset();

  for (std::shared_ptr<HostObjectLifetimeTracker> hostObjectLifetimeTracker : host_object_lifetime_tracker_list_) {
    hostObjectLifetimeTracker->ResetHostObject(false /*isGC*/);
  }

  host_object_lifetime_tracker_list_.clear();

  GetAndClearLastUnhandledPromiseRejection();

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
  if (inspector_agent_) {
    inspector_agent_.reset();
  }
#endif

  if (--tls_isolate_usage_counter_ == 0) {
    IsolateData *isolate_data = reinterpret_cast<IsolateData *>(isolate_->GetData(ISOLATE_DATA_SLOT));
    delete isolate_data;

    isolate_->SetData(v8runtime::ISOLATE_DATA_SLOT, nullptr);
    isolate_->SetData(v8runtime::ISOLATE_INSPECTOR_SLOT, nullptr);

    isolate_->Dispose();

    delete create_params_.array_buffer_allocator;
  }

  // Note :: We never dispose V8 here. Is it required ?
}

v8::Local<v8::String> V8Runtime::loadJavaScript(const std::shared_ptr<const jsi::Buffer> &buffer, std::uint64_t &hash) {
  v8::EscapableHandleScope handle_scope(GetIsolate());

  bool isAscii = murmurhash(buffer->data(), buffer->size(), hash);

  v8::Local<v8::String> sourceV8String;
  if (isAscii) {
    // This pointer is cleaned up by the platform
    ExternalOwningOneByteStringResource *external_string_resource = new ExternalOwningOneByteStringResource(buffer);
    if (!v8::String::NewExternalOneByte(GetIsolate(), external_string_resource).ToLocal(&sourceV8String)) {
      std::abort();
    }
  } else {
    if (!v8::String::NewFromUtf8(
             GetIsolate(),
             reinterpret_cast<const char *>(buffer->data()),
             v8::NewStringType::kNormal,
             static_cast<int>(buffer->size()))
             .ToLocal(&sourceV8String)) {
      std::abort();
    }
  }

  return handle_scope.Escape(sourceV8String);
}

jsi::Value V8Runtime::evaluateJavaScript(
    const std::shared_ptr<const jsi::Buffer> &buffer,
    const std::string &sourceURL) {
  TRACEV8RUNTIME_VERBOSE("evaluateJavaScript", TraceLoggingString("start", "op"));

  IsolateLocker isolate_locker(this);

  std::uint64_t hash{0};
  v8::Local<v8::String> sourceV8String = loadJavaScript(buffer, hash);
  jsi::Value result = createValue(ExecuteString(sourceV8String, sourceURL, hash));

  TRACEV8RUNTIME_VERBOSE("evaluateJavaScript", TraceLoggingString("end", "op"));
  DumpCounters("script evaluated");

  return result;
}

#if JSI_VERSION >= 12
void V8Runtime::queueMicrotask(const jsi::Function &callback) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::Function> v8Function = v8::Local<v8::Function>::Cast(objectRef(callback));
  GetIsolate()->EnqueueMicrotask(v8Function);
}
#endif

#if JSI_VERSION >= 4
bool V8Runtime::drainMicrotasks(int /*maxMicrotasksHint*/) {
  IsolateLocker isolate_locker(this);
  v8::Isolate *isolate = GetIsolate();
  if (isolate->GetMicrotasksPolicy() == v8::MicrotasksPolicy::kExplicit) {
    isolate->PerformMicrotaskCheckpoint();
  }
  return true;
}
#endif

v8::Local<v8::Context> V8Runtime::CreateContext(v8::Isolate *isolate) {
  // Create a template for the global object.

  TRACEV8RUNTIME_VERBOSE("CreateContext", TraceLoggingString("start", "op"));

  v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);

  v8::Local<v8::Context> context = v8::Context::New(isolate, NULL, global);
  context->SetAlignedPointerInEmbedderData(1, this);

  TRACEV8RUNTIME_VERBOSE("CreateContext", TraceLoggingString("end", "op"));
  DumpCounters("context_created");

  return context;
}

class ByteArrayBuffer final : public jsi::Buffer {
 public:
  size_t size() const override {
    return length_;
  }

  const uint8_t *data() const override {
    return data_;
  }

  uint8_t *data() {
    std::terminate();
  }

  ByteArrayBuffer(const uint8_t *data, int length) : data_(data), length_(length) {}

 private:
  const uint8_t *data_;
  int length_;
};

v8::Local<v8::Value>
V8Runtime::ExecuteString(const v8::Local<v8::String> &source, const std::string &sourceURL, std::uint64_t hash) {
  v8::EscapableHandleScope handle_scope(GetIsolate());

  v8::TryCatch try_catch(GetIsolate());

  v8::Local<v8::String> urlV8String =
      v8::String::NewFromUtf8(GetIsolate(), reinterpret_cast<const char *>(sourceURL.c_str())).ToLocalChecked();
  v8::ScriptOrigin origin(GetIsolate(), urlV8String);

  v8::Local<v8::Script> script;

  v8::ScriptCompiler::CompileOptions options = v8::ScriptCompiler::CompileOptions::kNoCompileOptions;
  v8::ScriptCompiler::CachedData *cached_data = nullptr;

  jsi::JSRuntimeVersion_t runtimeVersion = v8::ScriptCompiler::CachedDataVersionTag();

  std::shared_ptr<const jsi::Buffer> cache;
  if (args_.preparedScriptStore) {
    jsi::ScriptSignature scriptSignature = {sourceURL, hash};
    jsi::JSRuntimeSignature runtimeSignature = {"V8", runtimeVersion};
    cache = args_.preparedScriptStore->tryGetPreparedScript(scriptSignature, runtimeSignature, "perf");
  }

  if (cache) {
    cached_data = new v8::ScriptCompiler::CachedData(cache->data(), static_cast<int>(cache->size()));
    options = v8::ScriptCompiler::CompileOptions::kConsumeCodeCache;
  } else {
    // Eager compile so that we will write it to disk.
    // options = v8::ScriptCompiler::CompileOptions::kEagerCompile;
    options = v8::ScriptCompiler::CompileOptions::kNoCompileOptions;
  }

  v8::ScriptCompiler::Source script_source(source, origin, cached_data);

  if (!v8::ScriptCompiler::Compile(GetContextLocal(), &script_source, options).ToLocal(&script)) {
    // Print errors that happened during compilation.
    if (/*report_exceptions*/ true)
      ReportException(&try_catch);
    return handle_scope.Escape(v8::Undefined(GetIsolate()));
  } else {
    v8::Local<v8::Value> result;
    if (!script->Run(GetContextLocal()).ToLocal(&result)) {
      assert(try_catch.HasCaught());
      // Print errors that happened during execution.
      if (/*report_exceptions*/ true) {
        ReportException(&try_catch);
      }
      return handle_scope.Escape(v8::Undefined(GetIsolate()));
    } else {
      assert(!try_catch.HasCaught());

      if (args_.preparedScriptStore && options != v8::ScriptCompiler::CompileOptions::kConsumeCodeCache) {
        v8::ScriptCompiler::CachedData *codeCache = v8::ScriptCompiler::CreateCodeCache(script->GetUnboundScript());

        jsi::ScriptSignature scriptSignature = {sourceURL, hash};
        jsi::JSRuntimeSignature runtimeSignature = {"V8", runtimeVersion};

        args_.preparedScriptStore->persistPreparedScript(
            std::make_shared<ByteArrayBuffer>(codeCache->data, codeCache->length),
            scriptSignature,
            runtimeSignature,
            "perf");
      }

      return handle_scope.Escape(result);
    }
  }
}

class V8PreparedJavaScript : public facebook::jsi::PreparedJavaScript {
 public:
  jsi::ScriptSignature scriptSignature;
  jsi::JSRuntimeSignature runtimeSignature;
  std::vector<uint8_t> buffer;

  // What's the point of bytecode if we need to preserve the full source too?
  // TODO: Figure out if there's a way to use the bytecode only with V8
  std::shared_ptr<const facebook::jsi::Buffer> sourceBuffer;
  v8::Global<v8::Script> script;
};

std::shared_ptr<const facebook::jsi::PreparedJavaScript> V8Runtime::prepareJavaScript(
    const std::shared_ptr<const facebook::jsi::Buffer> &buffer,
    std::string sourceURL) {
  IsolateLocker isolate_locker(this);
  v8::TryCatch try_catch(GetIsolate());

  std::uint64_t hash{0};
  v8::Local<v8::String> sourceV8String = loadJavaScript(buffer, hash);

  v8::Local<v8::String> urlV8String =
      v8::String::NewFromUtf8(GetIsolate(), reinterpret_cast<const char *>(sourceURL.c_str())).ToLocalChecked();
  v8::ScriptOrigin origin(GetIsolate(), urlV8String);
  v8::Local<v8::Script> script;
  v8::ScriptCompiler::CompileOptions options = v8::ScriptCompiler::CompileOptions::kNoCompileOptions;
  v8::ScriptCompiler::CachedData *cached_data = nullptr;

  v8::ScriptCompiler::Source script_source(sourceV8String, origin, cached_data);

  if (!v8::ScriptCompiler::Compile(GetContextLocal(), &script_source, options).ToLocal(&script)) {
    ReportException(&try_catch);
    return nullptr;
  } else {
    v8::ScriptCompiler::CachedData *codeCache = v8::ScriptCompiler::CreateCodeCache(script->GetUnboundScript());

    auto prepared = std::make_shared<V8PreparedJavaScript>();
    prepared->scriptSignature = {sourceURL, hash};
    prepared->runtimeSignature = {"V8", v8::ScriptCompiler::CachedDataVersionTag()};
    prepared->buffer.assign(codeCache->data, codeCache->data + codeCache->length);
    prepared->sourceBuffer = buffer;
    return prepared;
  }
}

std::shared_ptr<const facebook::jsi::PreparedJavaScript> V8Runtime::prepareJavaScript2(
    const std::shared_ptr<const facebook::jsi::Buffer> &buffer,
    std::string sourceURL) {
  std::shared_ptr<V8PreparedJavaScript> prepared;

  v8::TryCatch try_catch(GetIsolate());

  std::uint64_t hash{0};
  v8::Local<v8::String> sourceV8String = loadJavaScript(buffer, hash);

  v8::Local<v8::String> urlV8String =
      v8::String::NewFromUtf8(GetIsolate(), reinterpret_cast<const char *>(sourceURL.c_str())).ToLocalChecked();
  v8::ScriptOrigin origin(GetIsolate(), urlV8String);

  v8::Local<v8::Script> script;

  v8::ScriptCompiler::CompileOptions options = v8::ScriptCompiler::CompileOptions::kNoCompileOptions;
  v8::ScriptCompiler::CachedData *cached_data = nullptr;

  jsi::JSRuntimeVersion_t runtimeVersion = v8::ScriptCompiler::CachedDataVersionTag();
  jsi::ScriptSignature scriptSignature = {sourceURL, hash};
  jsi::JSRuntimeSignature runtimeSignature = {"V8", runtimeVersion};

  std::shared_ptr<const jsi::Buffer> cache;
  if (args_.preparedScriptStore) {
    cache = args_.preparedScriptStore->tryGetPreparedScript(scriptSignature, runtimeSignature, "perf");
  }

  if (cache) {
    cached_data = new v8::ScriptCompiler::CachedData(cache->data(), static_cast<int>(cache->size()));
    options = v8::ScriptCompiler::CompileOptions::kConsumeCodeCache;
  } else if (args_.preparedScriptStore) {
    options = v8::ScriptCompiler::CompileOptions::kNoCompileOptions;
  }

  v8::ScriptCompiler::Source script_source(sourceV8String, origin, cached_data);

  if (!v8::ScriptCompiler::Compile(GetContextLocal(), &script_source, options).ToLocal(&script)) {
    // Print errors that happened during compilation.
    ReportException(&try_catch);
    if (options == v8::ScriptCompiler::CompileOptions::kConsumeCodeCache) {
      // Try to rebuild cache if it is in a bad state.
      options = v8::ScriptCompiler::CompileOptions::kEagerCompile;
      if (!v8::ScriptCompiler::Compile(GetContextLocal(), &script_source, options).ToLocal(&script)) {
        ReportException(&try_catch);
        return prepared;
      }
    }
  }

  v8::ScriptCompiler::CachedData *codeCache = v8::ScriptCompiler::CreateCodeCache(script->GetUnboundScript());

  if (args_.preparedScriptStore && options == v8::ScriptCompiler::CompileOptions::kEagerCompile) {
    args_.preparedScriptStore->persistPreparedScript(
        std::make_shared<ByteArrayBuffer>(codeCache->data, codeCache->length),
        scriptSignature,
        runtimeSignature,
        "perf");
  }

  prepared = std::make_shared<V8PreparedJavaScript>();
  prepared->scriptSignature = scriptSignature;
  prepared->runtimeSignature = runtimeSignature;
  prepared->buffer.assign(codeCache->data, codeCache->data + codeCache->length);
  prepared->sourceBuffer = buffer;
  prepared->script.Reset(isolate_, script);
  return prepared;
}

v8::Local<v8::Value> V8Runtime::evaluatePreparedJavaScript2(
    const std::shared_ptr<const facebook::jsi::PreparedJavaScript> &js) {
  const V8PreparedJavaScript *prepared = static_cast<const V8PreparedJavaScript *>(js.get());
  v8::EscapableHandleScope handle_scope(GetIsolate());

  v8::TryCatch try_catch(GetIsolate());

  v8::Local<v8::Script> script = prepared->script.Get(GetIsolate());

  v8::Local<v8::Value> result;
  if (!script->Run(GetContextLocal()).ToLocal(&result)) {
    assert(try_catch.HasCaught());
    ReportException(&try_catch);
    result = v8::Undefined(GetIsolate());
  } else {
    assert(!try_catch.HasCaught());
  }

  return handle_scope.Escape(result);
}

facebook::jsi::Value V8Runtime::evaluatePreparedJavaScript(
    const std::shared_ptr<const facebook::jsi::PreparedJavaScript> &js) {
  IsolateLocker isolate_locker(this);

  auto prepared = static_cast<const V8PreparedJavaScript *>(js.get());

  v8::TryCatch try_catch(GetIsolate());

  std::uint64_t hash{0};
  v8::Local<v8::String> sourceV8String = loadJavaScript(prepared->sourceBuffer, hash);

  if (prepared->scriptSignature.version != hash) {
    // hash mismatch, we need to recompile the source
    throw jsi::JSINativeException("Prepared JavaScript cache is invalid (Hash mismatch)");
  }

  if (prepared->runtimeSignature.version != v8::ScriptCompiler::CachedDataVersionTag()) {
    // V8 version mismatch, we need to recompile the source
    throw jsi::JSINativeException("Prepared JavaScript cache is invalid (V8 version mismatch)");
  }

  v8::Local<v8::String> urlV8String =
      v8::String::NewFromUtf8(GetIsolate(), reinterpret_cast<const char *>(prepared->scriptSignature.url.c_str()))
          .ToLocalChecked();
  v8::ScriptOrigin origin(GetIsolate(), urlV8String);
  v8::Local<v8::Script> script;

  v8::ScriptCompiler::CompileOptions options = v8::ScriptCompiler::CompileOptions::kConsumeCodeCache;
  v8::ScriptCompiler::CachedData *cached_data =
      new v8::ScriptCompiler::CachedData(prepared->buffer.data(), static_cast<int>(prepared->buffer.size()));

  v8::ScriptCompiler::Source script_source(sourceV8String, origin, cached_data);

  if (!v8::ScriptCompiler::Compile(GetContextLocal(), &script_source, options).ToLocal(&script)) {
    ReportException(&try_catch);
    return createValue(v8::Undefined(GetIsolate()));
  } else {
    v8::Local<v8::Value> result;
    if (!script->Run(GetContextLocal()).ToLocal(&result)) {
      assert(try_catch.HasCaught());
      ReportException(&try_catch);
      return createValue(v8::Undefined(GetIsolate()));
    } else {
      assert(!try_catch.HasCaught());
      return createValue(result);
    }
  }
}

void V8Runtime::ReportException(v8::TryCatch *try_catch) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::Message> message = try_catch->Message();
  if (message.IsEmpty()) {
    // V8 didn't provide any extra information about this error; just
    // throw the exception.
    v8::String::Utf8Value exception(GetIsolate(), try_catch->Exception());
    throw jsi::JSError(*this, ToCString(exception));
  } else {
    std::stringstream sstr;

    v8::Local<v8::Value> stack_trace_string;
    if (try_catch->StackTrace(GetContextLocal()).ToLocal(&stack_trace_string) && stack_trace_string->IsString() &&
        v8::Local<v8::String>::Cast(stack_trace_string)->Length() > 0) {
      v8::String::Utf8Value stack_trace(GetIsolate(), stack_trace_string);
      const char *stack_trace_string2 = ToCString(stack_trace);
      sstr << stack_trace_string2 << std::endl;
    }

    v8::String::Utf8Value ex_message(GetIsolate(), message->Get());
    std::string ex_messages = ToCString(ex_message);
    if (ex_messages.rfind("Uncaught Error:", 0) == 0) {
      // V8 adds an "Uncaught Error: " before any message string any time we read it, this strips it out.
      ex_messages.erase(0, 16);
    }

    TRACEV8RUNTIME_CRITICAL(
        "Exception",
        TraceLoggingString(ex_messages.c_str(), "ex_messages"),
        TraceLoggingString(sstr.str().c_str(), "sstr"));

    // V8 doesn't actually capture the current callstack (as we're outside of scope when this gets called)
    // See also https://v8.dev/docs/stack-trace-api
    std::string stack = sstr.str();
    if (stack.find("Maximum call stack size exceeded") == std::string::npos) {
      auto err = jsi::JSError(*this, ex_messages);

      err.value().getObject(*this).setProperty(*this, "stack", facebook::jsi::String::createFromUtf8(*this, stack));

      // The "stack" includes the message in V8, but JSI tracks the message and the callstack as 2 separate properties
      // so let's strip it out The format of stack is "%ErrorType%: %Message%\n%Callstack%" where %Message% can
      // include newline characters as well.
      auto numNewLines = std::count(ex_messages.cbegin(), ex_messages.cend(), '\n');
      auto endOfMessage = stack.find("\n");
      for (size_t j = 0; j < numNewLines; j++) {
        endOfMessage = stack.find("\n", endOfMessage + 1);
      }
      stack.erase(0, endOfMessage + 1);

      err.setStack(stack);
      throw err;
    } else {
      // If we're already in stack overflow, calling the Error constructor pushes it overboard
      throw jsi::JSError(*this, ex_messages, stack);
    }
  }
}

jsi::Object V8Runtime::global() {
  IsolateLocker isolate_locker(this);
  return make<jsi::Object>(V8ObjectValue::make(GetIsolate(), GetContextLocal()->Global()));
}

std::string V8Runtime::description() {
  if (desc_.empty()) {
    desc_ = std::string("<V8Runtime>");
  }
  return desc_;
}

bool V8Runtime::isInspectable() {
  return false;
}

// Shallow clone
jsi::Runtime::PointerValue *V8Runtime::cloneString(const jsi::Runtime::PointerValue *pv) {
  if (!pv) {
    return nullptr;
  }

  IsolateLocker isolate_locker(this);
  const V8StringValue *string = static_cast<const V8StringValue *>(pv);
  return V8StringValue::make(GetIsolate(), string->get(GetIsolate()));
}

jsi::Runtime::PointerValue *V8Runtime::cloneObject(const jsi::Runtime::PointerValue *pv) {
  if (!pv) {
    return nullptr;
  }

  IsolateLocker isolate_locker(this);
  const V8ObjectValue *object = static_cast<const V8ObjectValue *>(pv);
  return V8ObjectValue::make(GetIsolate(), object->get(GetIsolate()));
}

jsi::Runtime::PointerValue *V8Runtime::clonePropNameID(const jsi::Runtime::PointerValue *pv) {
  if (!pv) {
    return nullptr;
  }

  IsolateLocker isolate_locker(this);
  const V8StringValue *string = static_cast<const V8StringValue *>(pv);
  return V8StringValue::make(GetIsolate(), string->get(GetIsolate()));
}

jsi::Runtime::PointerValue *V8Runtime::cloneSymbol(const jsi::Runtime::PointerValue *pv) {
  if (!pv) {
    return nullptr;
  }

  IsolateLocker isolate_locker(this);
  const V8PointerValue<v8::Symbol> *symbol = static_cast<const V8PointerValue<v8::Symbol> *>(pv);
  return V8PointerValue<v8::Symbol>::make(GetIsolate(), symbol->get(GetIsolate()));
}

#if JSI_VERSION >= 6
jsi::Runtime::PointerValue *V8Runtime::cloneBigInt(const jsi::Runtime::PointerValue *pv) {
  if (!pv) {
    return nullptr;
  }

  IsolateLocker isolate_locker(this);
  const V8PointerValue<v8::BigInt> *bigInt = static_cast<const V8PointerValue<v8::BigInt> *>(pv);
  return V8PointerValue<v8::BigInt>::make(GetIsolate(), bigInt->get(GetIsolate()));
}
#endif

std::string V8Runtime::symbolToString(const jsi::Symbol &sym) {
  IsolateLocker isolate_locker(this);
  return "Symbol(" +
      JSStringToSTLString(GetIsolate(), v8::Local<v8::String>::Cast(symbolRef(sym)->Description(GetIsolate()))) + ")";
}

jsi::PropNameID V8Runtime::createPropNameIDFromAscii(const char *str, size_t length) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::String> v8String;
  if (!v8::String::NewFromOneByte(
           GetIsolate(),
           reinterpret_cast<uint8_t *>(const_cast<char *>(str)),
           v8::NewStringType::kNormal,
           static_cast<int>(length))
           .ToLocal(&v8String)) {
    std::stringstream strstream;
    strstream << "Unable to create property id: " << str;
    throw jsi::JSError(*this, strstream.str());
  }

  auto res = make<jsi::PropNameID>(V8StringValue::make(GetIsolate(), v8::Local<v8::String>::Cast(v8String)));
  return res;
}

jsi::PropNameID V8Runtime::createPropNameIDFromUtf8(const uint8_t *utf8, size_t length) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::String> v8String;
  if (!v8::String::NewFromUtf8(
           GetIsolate(),
           reinterpret_cast<const char *>(utf8),
           v8::NewStringType::kInternalized,
           static_cast<int>(length))
           .ToLocal(&v8String)) {
    std::stringstream strstream;
    strstream << "Unable to create property id: " << utf8;
    throw jsi::JSError(*this, strstream.str());
  }

  auto res = make<jsi::PropNameID>(V8StringValue::make(GetIsolate(), v8String));
  return res;
}

#if JSI_VERSION >= 19
jsi::PropNameID V8Runtime::createPropNameIDFromUtf16(const char16_t *utf16, size_t length) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::String> v8String;
  if (!v8::String::NewFromTwoByte(
           GetIsolate(),
           reinterpret_cast<const uint16_t *>(utf16),
           v8::NewStringType::kInternalized,
           static_cast<int>(length))
           .ToLocal(&v8String)) {
    throw jsi::JSError(*this, "Unable to create UTF16 property id");
  }

  auto res = make<jsi::PropNameID>(V8StringValue::make(GetIsolate(), v8String));
  return res;
}
#endif

jsi::PropNameID V8Runtime::createPropNameIDFromString(const jsi::String &str) {
  IsolateLocker isolate_locker(this);
  return make<jsi::PropNameID>(V8StringValue::make(GetIsolate(), v8::Local<v8::String>::Cast(stringRef(str))));
}

#if JSI_VERSION >= 5
jsi::PropNameID V8Runtime::createPropNameIDFromSymbol(const jsi::Symbol &sym) {
  IsolateLocker isolate_locker(this);
  return make<jsi::PropNameID>(V8SymbolValue::make(GetIsolate(), v8::Local<v8::Symbol>::Cast(symbolRef(sym))));
}
#endif

std::string V8Runtime::utf8(const jsi::PropNameID &sym) {
  IsolateLocker isolate_locker(this);
  return JSStringToSTLString(GetIsolate(), v8::Local<v8::String>::Cast(valueRef(sym)));
}

bool V8Runtime::compare(const jsi::PropNameID &a, const jsi::PropNameID &b) {
  IsolateLocker isolate_locker(this);
  return valueRef(a)->Equals(GetContextLocal(), valueRef(b)).ToChecked();
}

jsi::String V8Runtime::createStringFromAscii(const char *str, size_t length) {
  return this->createStringFromUtf8(reinterpret_cast<const uint8_t *>(str), length);
}

jsi::String V8Runtime::createStringFromUtf8(const uint8_t *str, size_t length) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::String> v8string;
  if (!v8::String::NewFromUtf8(
           GetIsolate(), reinterpret_cast<const char *>(str), v8::NewStringType::kNormal, static_cast<int>(length))
           .ToLocal(&v8string)) {
    throw jsi::JSError(*this, "V8 string creation failed.");
  }

  jsi::String jsistr = make<jsi::String>(V8StringValue::make(GetIsolate(), v8string));
  return jsistr;
}

#if JSI_VERSION >= 19
jsi::String V8Runtime::createStringFromUtf16(const char16_t *utf16, size_t length) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::String> v8string;
  if (!v8::String::NewFromTwoByte(
           GetIsolate(),
           reinterpret_cast<const uint16_t *>(utf16),
           v8::NewStringType::kNormal,
           static_cast<int>(length))
           .ToLocal(&v8string)) {
    throw jsi::JSError(*this, "V8 UTF-16 string creation failed.");
  }

  jsi::String jsistr = make<jsi::String>(V8StringValue::make(GetIsolate(), v8string));
  return jsistr;
}
#endif

std::string V8Runtime::utf8(const jsi::String &str) {
  IsolateLocker isolate_locker(this);
  return JSStringToSTLString(GetIsolate(), stringRef(str));
}

jsi::Object V8Runtime::createObject() {
  IsolateLocker isolate_locker(this);
  return make<jsi::Object>(V8ObjectValue::make(GetIsolate(), v8::Object::New(GetIsolate())));
}

jsi::Object V8Runtime::createObject(std::shared_ptr<jsi::HostObject> hostobject) {
  IsolateLocker isolate_locker(this);
  HostObjectProxy *hostObjectProxy = new HostObjectProxy(*this, hostobject);
  v8::Local<v8::Object> newObject;
  if (!host_object_constructor_.Get(GetIsolate())->NewInstance(GetContextLocal()).ToLocal(&newObject)) {
    throw jsi::JSError(*this, "HostObject construction failed!!");
  }

  newObject->SetInternalField(
      0, v8::Local<v8::External>::New(GetIsolate(), v8::External::New(GetIsolate(), hostObjectProxy)));

  AddHostObjectLifetimeTracker(std::make_shared<HostObjectLifetimeTracker>(*this, newObject, hostObjectProxy));

  return make<jsi::Object>(V8ObjectValue::make(GetIsolate(), newObject));
}

std::shared_ptr<jsi::HostObject> V8Runtime::getHostObject(const jsi::Object &obj) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::External> internalField =
      v8::Local<v8::External>::Cast(objectRef(obj)->GetInternalField(0).As<v8::Value>());
  HostObjectProxy *hostObjectProxy = reinterpret_cast<HostObjectProxy *>(internalField->Value());
  return hostObjectProxy->getHostObject();
}

jsi::Value V8Runtime::getProperty(const jsi::Object &obj, const jsi::String &name) {
  IsolateLocker isolate_locker(this);
  return createValue(objectRef(obj)->Get(GetContextLocal(), stringRef(name)).ToLocalChecked());
}

jsi::Value V8Runtime::getProperty(const jsi::Object &obj, const jsi::PropNameID &name) {
  IsolateLocker isolate_locker(this);
  v8::MaybeLocal<v8::Value> result = objectRef(obj)->Get(GetContextLocal(), valueRef(name));
  if (result.IsEmpty())
    throw jsi::JSError(*this, "V8Runtime::getProperty failed.");
  return createValue(result.ToLocalChecked());
}

bool V8Runtime::hasProperty(const jsi::Object &obj, const jsi::String &name) {
  IsolateLocker isolate_locker(this);
  v8::Maybe<bool> result = objectRef(obj)->Has(GetContextLocal(), stringRef(name));
  if (result.IsNothing())
    throw jsi::JSError(*this, "V8Runtime::hasPropertyValue failed.");
  return result.FromJust();
}

bool V8Runtime::hasProperty(const jsi::Object &obj, const jsi::PropNameID &name) {
  IsolateLocker isolate_locker(this);
  v8::Maybe<bool> result = objectRef(obj)->Has(GetContextLocal(), valueRef(name));
  if (result.IsNothing())
    throw jsi::JSError(*this, "V8Runtime::hasPropertyValue failed.");
  return result.FromJust();
}

void V8Runtime::setPropertyValue(
    JSI_CONST_10 jsi::Object &object,
    const jsi::PropNameID &name,
    const jsi::Value &value) {
  IsolateLocker isolate_locker(this);
  v8::Maybe<bool> result = objectRef(object)->Set(GetContextLocal(), valueRef(name), valueReference(value));
  if (!result.FromMaybe(false))
    throw jsi::JSError(*this, "V8Runtime::setPropertyValue failed.");
}

void V8Runtime::setPropertyValue(JSI_CONST_10 jsi::Object &object, const jsi::String &name, const jsi::Value &value) {
  IsolateLocker isolate_locker(this);
  v8::Maybe<bool> result = objectRef(object)->Set(GetContextLocal(), stringRef(name), valueReference(value));
  if (!result.FromMaybe(false))
    throw jsi::JSError(*this, "V8Runtime::setPropertyValue failed.");
}

bool V8Runtime::isArray(const jsi::Object &obj) const {
  IsolateLocker isolate_locker(this);
  return objectRef(obj)->IsArray();
}

bool V8Runtime::isArrayBuffer(const jsi::Object &obj) const {
  IsolateLocker isolate_locker(this);
  return objectRef(obj)->IsArrayBuffer();
}

uint8_t *V8Runtime::data(const jsi::ArrayBuffer &obj) {
  IsolateLocker isolate_locker(this);
  return reinterpret_cast<uint8_t *>(objectRef(obj).As<v8::ArrayBuffer>()->GetBackingStore()->Data());
}

size_t V8Runtime::size(const jsi::ArrayBuffer &obj) {
  IsolateLocker isolate_locker(this);
  return objectRef(obj).As<v8::ArrayBuffer>()->ByteLength();
}

bool V8Runtime::isFunction(const jsi::Object &obj) const {
  IsolateLocker isolate_locker(this);
  return objectRef(obj)->IsFunction();
}

bool V8Runtime::isHostObject(const jsi::Object &obj) const {
  IsolateLocker isolate_locker(this);
  if (objectRef(obj)->InternalFieldCount() < 1) {
    return false;
  }

  auto internalFieldRef = objectRef(obj)->GetInternalField(0).As<v8::Value>();
  if (internalFieldRef.IsEmpty()) {
    return false;
  }

  v8::Local<v8::External> internalField = v8::Local<v8::External>::Cast(internalFieldRef);
  if (!internalField->Value()) {
    return false;
  }

  HostObjectProxy *hostObjectProxy = reinterpret_cast<HostObjectProxy *>(internalField->Value());

  for (const std::shared_ptr<HostObjectLifetimeTracker> &hostObjectLifetimeTracker :
       host_object_lifetime_tracker_list_) {
    if (hostObjectLifetimeTracker->IsEqual(hostObjectProxy)) {
      return true;
    }
  }

  return false;
}

// Very expensive
jsi::Array V8Runtime::getPropertyNames(const jsi::Object &obj) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::Array> propNames = objectRef(obj)
                                       ->GetPropertyNames(
                                           GetContextLocal(),
                                           v8::KeyCollectionMode::kIncludePrototypes,
                                           static_cast<v8::PropertyFilter>(v8::ONLY_ENUMERABLE | v8::SKIP_SYMBOLS),
                                           v8::IndexFilter::kIncludeIndices,
                                           v8::KeyConversionMode::kConvertToString)
                                       .ToLocalChecked();
  return make<jsi::Object>(V8ObjectValue::make(GetIsolate(), propNames)).getArray(*this);
}

jsi::WeakObject V8Runtime::createWeakObject(const jsi::Object &obj) {
  IsolateLocker isolate_locker(this);
  return make<jsi::WeakObject>(V8WeakObjectValue::make(GetIsolate(), objectRef(obj)));
}

jsi::Value V8Runtime::lockWeakObject(JSI_NO_CONST_3 JSI_CONST_10 jsi::WeakObject &weakObj) {
  IsolateLocker isolate_locker(this);

  const V8WeakObjectValue *v8WeakObject = static_cast<const V8WeakObjectValue *>(getPointerValue(weakObj));
  v8::Local<v8::Object> obj = v8WeakObject->get(GetIsolate());

  if (!obj.IsEmpty())
    return createValue(obj);

  return jsi::Value();
}

jsi::Array V8Runtime::createArray(size_t length) {
  IsolateLocker isolate_locker(this);
  return make<jsi::Object>(V8ObjectValue::make(GetIsolate(), v8::Array::New(GetIsolate(), static_cast<int>(length))))
      .getArray(*this);
}

size_t V8Runtime::size(const jsi::Array &arr) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::Array> array = v8::Local<v8::Array>::Cast(objectRef(arr));
  return array->Length();
}

jsi::Value V8Runtime::getValueAtIndex(const jsi::Array &arr, size_t i) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::Array> array = v8::Local<v8::Array>::Cast(objectRef(arr));
  return createValue(array->Get(GetContextLocal(), static_cast<uint32_t>(i)).ToLocalChecked());
}

void V8Runtime::setValueAtIndexImpl(JSI_CONST_10 jsi::Array &arr, size_t i, const jsi::Value &value) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::Array> array = v8::Local<v8::Array>::Cast(objectRef(arr));
  array->Set(GetContextLocal(), static_cast<uint32_t>(i), valueReference(value));
}

jsi::Function V8Runtime::createFunctionFromHostFunction(
    const jsi::PropNameID &name,
    unsigned int paramCount,
    jsi::HostFunctionType func) {
  IsolateLocker isolate_locker(this);

  HostFunctionProxy *hostFunctionProxy = new HostFunctionProxy(*this, func);

  v8::Local<v8::Function> newFunction;
  if (!v8::Function::New(
           GetContextLocal(),
           HostFunctionProxy::HostFunctionCallback,
           v8::Local<v8::External>::New(GetIsolate(), v8::External::New(GetIsolate(), hostFunctionProxy)),
           paramCount)
           .ToLocal(&newFunction)) {
    throw jsi::JSError(*this, "Creation of HostFunction failed.");
  }

  newFunction->SetName(v8::Local<v8::String>::Cast(valueRef(name)));

  AddHostObjectLifetimeTracker(std::make_shared<HostObjectLifetimeTracker>(*this, newFunction, hostFunctionProxy));

  return make<jsi::Object>(V8ObjectValue::make(GetIsolate(), newFunction)).getFunction(*this);
}

bool V8Runtime::isHostFunction(const jsi::Function &obj) const {
  std::abort();
  return false;
}

jsi::HostFunctionType &V8Runtime::getHostFunction(const jsi::Function &obj) {
  std::abort();
}

#if JSI_VERSION >= 18
jsi::Object V8Runtime::createObjectWithPrototype(const jsi::Value &prototype) {
  IsolateLocker isolate_locker(this);
  return make<jsi::Object>(
      V8ObjectValue::make(GetIsolate(), v8::Object::New(GetIsolate(), valueReference(prototype), nullptr, nullptr, 0)));
}
#endif

namespace {
std::string getFunctionName(v8::Isolate *isolate, v8::Local<v8::Function> func) {
  std::string functionNameStr;
  v8::Local<v8::String> functionNameV8Str = v8::Local<v8::String>::Cast(func->GetName());
  int functionNameLength = functionNameV8Str->Utf8Length(isolate);
  if (functionNameLength > 0) {
    functionNameStr.resize(functionNameLength);
    functionNameV8Str->WriteUtf8(isolate, &functionNameStr[0]);
  }

  if (functionNameV8Str.IsEmpty()) {
    functionNameV8Str = v8::Local<v8::String>::Cast(func->GetInferredName());
    functionNameLength = functionNameV8Str->Utf8Length(isolate);
    if (functionNameLength > 0) {
      functionNameStr.resize(functionNameLength);
      functionNameV8Str->WriteUtf8(isolate, &functionNameStr[0]);
    }
  }

  if (functionNameV8Str.IsEmpty()) {
    functionNameStr = "<anonymous>";
  }

  return functionNameStr;
}
} // namespace

jsi::Value
V8Runtime::call(const jsi::Function &jsiFunc, const jsi::Value &jsThis, const jsi::Value *args, size_t count) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::Function> func = v8::Local<v8::Function>::Cast(objectRef(jsiFunc));

  // TODO: This may be a bit costly when there are large number of JS function
  // calls from native. Evaluate !
  std::string functionName = getFunctionName(GetIsolate(), func);

  static uint8_t callCookie = 0;
  if (callCookie > 0) {
    TRACEV8RUNTIME_WARNING(
        "CallFunctionNested",
        TraceLoggingString(functionName.c_str(), "name"),
        TraceLoggingString("Nested calls to JavaScript functions can be problematic !", "message"));
  }
  callCookie++;

  TRACEV8RUNTIME_VERBOSE(
      "CallFunction", TraceLoggingString(functionName.c_str(), "name"), TraceLoggingString("start", "op"));

  std::vector<v8::Local<v8::Value>> argv;
  for (size_t i = 0; i < count; i++) {
    argv.push_back(valueReference(args[i]));
  }

  v8::TryCatch trycatch(GetIsolate());
  v8::MaybeLocal<v8::Value> result =
      func->Call(GetContextLocal(), valueReference(jsThis), static_cast<int>(count), argv.data());

  if (trycatch.HasCaught()) {
    ReportException(&trycatch);
  }

  TRACEV8RUNTIME_VERBOSE(
      "CallFunction", TraceLoggingString(functionName.c_str(), "name"), TraceLoggingString("end", "op"));
  DumpCounters("call_completed");

  callCookie--;

  // Call can return
  if (result.IsEmpty()) {
    return createValue(v8::Undefined(GetIsolate()));
  } else {
    return createValue(result.ToLocalChecked());
  }
}

jsi::Value V8Runtime::callAsConstructor(const jsi::Function &jsiFunc, const jsi::Value *args, size_t count) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::Function> func = v8::Local<v8::Function>::Cast(objectRef(jsiFunc));

  // TODO: This may be a bit costly when there are large number of JS function
  // calls from native. Evaluate !
  std::string functionName = getFunctionName(GetIsolate(), func);
  TRACEV8RUNTIME_VERBOSE(
      "CallConstructor", TraceLoggingString(functionName.c_str(), "name"), TraceLoggingString("start", "op"));

  std::vector<v8::Local<v8::Value>> argv;
  for (size_t i = 0; i < count; i++) {
    argv.push_back(valueReference(args[i]));
  }

  v8::TryCatch trycatch(GetIsolate());
  v8::Local<v8::Object> newObject;
  if (!func->NewInstance(GetContextLocal(), static_cast<int>(count), argv.data()).ToLocal(&newObject)) {
    new jsi::JSError(*this, "Object construction failed!!");
  }

  if (trycatch.HasCaught()) {
    ReportException(&trycatch);
  }

  TRACEV8RUNTIME_VERBOSE(
      "CallConstructor", TraceLoggingString(functionName.c_str(), "name"), TraceLoggingString("end", "op"));
  DumpCounters("callAsConstructor_completed");

  return createValue(newObject);
}

bool V8Runtime::strictEquals(const jsi::String &a, const jsi::String &b) const {
  IsolateLocker isolate_locker(this);
  return stringRef(a)->StrictEquals(stringRef(b));
}

bool V8Runtime::strictEquals(const jsi::Object &a, const jsi::Object &b) const {
  IsolateLocker isolate_locker(this);
  return objectRef(a)->StrictEquals(objectRef(b));
}

bool V8Runtime::strictEquals(const jsi::Symbol &a, const jsi::Symbol &b) const {
  IsolateLocker isolate_locker(this);
  return symbolRef(a)->StrictEquals(symbolRef(b));
}

#if JSI_VERSION >= 6
bool V8Runtime::strictEquals(const jsi::BigInt &a, const jsi::BigInt &b) const {
  IsolateLocker isolate_locker(this);
  return bigIntRef(a)->StrictEquals(bigIntRef(b));
}
#endif

bool V8Runtime::instanceOf(const jsi::Object &o, const jsi::Function &f) {
  IsolateLocker isolate_locker(this);
  return objectRef(o)->InstanceOf(GetContextLocal(), objectRef(f)).ToChecked();
}

#if JSI_VERSION >= 17
void V8Runtime::setPrototypeOf(const jsi::Object &object, const jsi::Value &prototype) {
  IsolateLocker isolate_locker(this);
  objectRef(object)->SetPrototype(GetContextLocal(), valueReference(prototype));
}

jsi::Value V8Runtime::getPrototypeOf(const jsi::Object &object) {
  IsolateLocker isolate_locker(this);
  return createValue(objectRef(object)->GetPrototype());
}
#endif

#if JSI_VERSION >= 11
void V8Runtime::setExternalMemoryPressure(const jsi::Object &obj, size_t amount) {
  // TODO: implement this
}
#endif

#if JSI_VERSION >= 14
std::u16string V8Runtime::utf16(const jsi::String &str) {
  IsolateLocker isolate_locker(this);
  return JSStringToStlU16String(GetIsolate(), stringRef(str));
}

std::u16string V8Runtime::utf16(const jsi::PropNameID &sym) {
  IsolateLocker isolate_locker(this);
  return JSStringToStlU16String(GetIsolate(), v8::Local<v8::String>::Cast(valueRef(sym)));
}
#endif

#if JSI_VERSION >= 16
void V8Runtime::getStringData(
    const jsi::String &str,
    void *ctx,
    void (*cb)(void *ctx, bool ascii, const void *data, size_t num)) {
  return Runtime::getStringData(str, ctx, cb);
}

void V8Runtime::getPropNameIdData(
    const jsi::PropNameID &sym,
    void *ctx,
    void (*cb)(void *ctx, bool ascii, const void *data, size_t num)) {
  return Runtime::getPropNameIdData(sym, ctx, cb);
}
#endif

#if JSI_VERSION >= 8
facebook::jsi::BigInt V8Runtime::createBigIntFromInt64(int64_t val) {
  IsolateLocker isolate_locker(this);
  return make<facebook::jsi::BigInt>(
      V8PointerValue<v8::BigInt>::make(GetIsolate(), v8::BigInt::New(GetIsolate(), val)));
}

facebook::jsi::BigInt V8Runtime::createBigIntFromUint64(uint64_t val) {
  IsolateLocker isolate_locker(this);
  return make<facebook::jsi::BigInt>(
      V8PointerValue<v8::BigInt>::make(GetIsolate(), v8::BigInt::NewFromUnsigned(GetIsolate(), val)));
}

bool V8Runtime::bigintIsInt64(const facebook::jsi::BigInt &val) {
  IsolateLocker isolate_locker(this);
  bool lossless{true};
  uint64_t value = bigIntRef(val)->Int64Value(&lossless);
  return lossless;
}

bool V8Runtime::bigintIsUint64(const facebook::jsi::BigInt &val) {
  IsolateLocker isolate_locker(this);
  bool lossless{true};
  uint64_t value = bigIntRef(val)->Uint64Value(&lossless);
  return lossless;
}

uint64_t V8Runtime::truncate(const facebook::jsi::BigInt &val) {
  IsolateLocker isolate_locker(this);
  return bigIntRef(val)->Uint64Value(nullptr);
}

constexpr inline uint32_t maxCharsPerDigitInRadix(int32_t radix) {
  // To compute the lower bound of bits in a BigIntDigitType "covered" by a
  // char. For power of 2 radixes, it is known (exactly) that each character
  // covers log2(radix) bits. For non-power of 2 radixes, a lower bound is
  // log2(greatest power of 2 that is less than radix).
  uint32_t minNumBitsPerChar = radix < 4 ? 1 : radix < 8 ? 2 : radix < 16 ? 3 : radix < 32 ? 4 : 5;

  // With minNumBitsPerChar being the lower bound estimate of how many bits each
  // char can represent, the upper bound of how many chars "fit" in a bigint
  // digit is ceil(sizeofInBits(bigint digit) / minNumBitsPerChar).
  uint32_t numCharsPerDigits = static_cast<uint32_t>(sizeof(uint64_t)) / (1 << minNumBitsPerChar);

  return numCharsPerDigits;
}

// Return the high 32 bits of a 64 bit value.
constexpr inline uint32_t Hi_32(uint64_t Value) {
  return static_cast<uint32_t>(Value >> 32);
}

// Return the low 32 bits of a 64 bit value.
constexpr inline uint32_t Lo_32(uint64_t Value) {
  return static_cast<uint32_t>(Value);
}

// Make a 64-bit integer from a high / low pair of 32-bit integers.
constexpr inline uint64_t Make_64(uint32_t High, uint32_t Low) {
  return ((uint64_t)High << 32) | (uint64_t)Low;
}

facebook::jsi::String V8Runtime::bigintToString(const facebook::jsi::BigInt &val, int radix) {
  IsolateLocker isolate_locker(this);
  if (radix < 2 || radix > 36) {
    throw makeJSINativeException("Invalid radix ", radix, " to BigInt.toString");
  }

  v8::Local<v8::BigInt> bigint = bigIntRef(val);
  int wordCount = bigint->WordCount();
  uint64_t stackWords[8]{};
  std::unique_ptr<uint64_t[]> heapWords;
  uint64_t *words = stackWords;
  if (wordCount > static_cast<int>(std::size(stackWords))) {
    heapWords = std::unique_ptr<uint64_t[]>(new uint64_t[wordCount]);
    words = heapWords.get();
  }
  int32_t signBit{};
  bigint->ToWordsArray(&signBit, &wordCount, words);

  if (wordCount == 0) {
    return createStringFromAscii("0", 1);
  }

  // avoid trashing the heap by pre-allocating the largest possible string
  // returned by this function. The "1" below is to account for a possible "-"
  // sign.
  std::string digits;
  digits.reserve(1 + wordCount * maxCharsPerDigitInRadix(radix));

  // Use 32-bit values for calculations to get 64-bit results.
  // For the little-endian machines we just cast the words array.
  // TODO: Add support for big-endian.
  uint32_t *halfWords = reinterpret_cast<uint32_t *>(words);
  size_t count = wordCount * 2;
  for (size_t i = count; i > 0 && halfWords[i - 1] == 0; --i) {
    --count;
  }

  uint32_t divisor = static_cast<uint32_t>(radix);
  uint32_t remainder = 0;
  uint64_t word0 = words[0];

  do {
    // We rewrite the halfWords array as we divide it by radix.
    if (count <= 2) {
      remainder = word0 % divisor;
      word0 = word0 / divisor;
    } else {
      for (size_t i = count; i > 0; --i) {
        uint64_t partialDividend = Make_64(remainder, halfWords[i - 1]);
        if (partialDividend == 0) {
          halfWords[i] = 0;
          remainder = 0;
          if (i == count) {
            if (--count == 2) {
              word0 = words[0];
            }
          }
        } else if (partialDividend < divisor) {
          halfWords[i] = 0;
          remainder = Lo_32(partialDividend);
          if (i == count) {
            if (--count == 2) {
              word0 = words[0];
            }
          }
        } else if (partialDividend == divisor) {
          halfWords[i] = 1;
          remainder = 0;
        } else {
          halfWords[i] = Lo_32(partialDividend / divisor);
          remainder = Lo_32(partialDividend % divisor);
        }
      }
    }

    if (remainder < 10) {
      digits.push_back(static_cast<char>('0' + remainder));
    } else {
      digits.push_back(static_cast<char>('a' + remainder - 10));
    }
  } while (count > 2 || word0 != 0);

  if (signBit) {
    digits.push_back('-');
  }

  std::reverse(digits.begin(), digits.end());
  return createStringFromAscii(digits.data(), digits.size());
}
#endif

class NativeStateHolder final {
 public:
  NativeStateHolder(
      v8::Isolate *isolate,
      v8::Local<v8::Object> v8Object,
      std::shared_ptr<facebook::jsi::NativeState> nativeState) noexcept
      : v8WeakObject_(isolate, v8Object), nativeState_(std::move(nativeState)) {
    v8WeakObject_.SetWeak(
        this,
        [](const v8::WeakCallbackInfo<NativeStateHolder> &data) { delete data.GetParameter(); },
        v8::WeakCallbackType::kParameter);
  }

  const std::shared_ptr<facebook::jsi::NativeState> &getNativeState() const noexcept {
    return nativeState_;
  }

  void setNativeState(std::shared_ptr<facebook::jsi::NativeState> nativeState) noexcept {
    nativeState_ = std::move(nativeState);
  }

 private:
  v8::Global<v8::Object> v8WeakObject_;
  std::shared_ptr<facebook::jsi::NativeState> nativeState_;
};

#if JSI_VERSION >= 7
bool V8Runtime::hasNativeState(const facebook::jsi::Object &obj) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::Object> v8Object = objectRef(obj);
  v8::Maybe<bool> result = v8Object->HasPrivate(GetContextLocal(), nativeStateKey());
  return result.FromMaybe(false);
}

std::shared_ptr<facebook::jsi::NativeState> V8Runtime::getNativeState(const facebook::jsi::Object &obj) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::Object> v8Object = objectRef(obj);
  NativeStateHolder *holder = getNativeStateHolder(v8Object);
  return holder ? holder->getNativeState() : nullptr;
}

void V8Runtime::setNativeState(
    const facebook::jsi::Object &obj,
    std::shared_ptr<facebook::jsi::NativeState> nativeState) {
  IsolateLocker isolate_locker(this);
  v8::Local<v8::Object> v8Object = objectRef(obj);
  NativeStateHolder *holder = getNativeStateHolder(v8Object);
  if (holder) {
    holder->setNativeState(std::move(nativeState));
  } else {
    holder = new NativeStateHolder(GetIsolate(), v8Object, std::move(nativeState));
    v8::Local<v8::External> external = v8::External::New(GetIsolate(), holder);
    v8Object->SetPrivate(GetContextLocal(), nativeStateKey(), external).Check();
  }
}

NativeStateHolder *V8Runtime::getNativeStateHolder(v8::Local<v8::Object> v8Object) {
  v8::MaybeLocal<v8::Value> maybeValue = v8Object->GetPrivate(GetContextLocal(), nativeStateKey());
  if (maybeValue.IsEmpty()) {
    return nullptr;
  }
  v8::Local<v8::Value> val = maybeValue.ToLocalChecked();
  if (!val->IsExternal()) {
    return nullptr;
  }
  v8::Local<v8::External> external = val.As<v8::External>();
  return static_cast<NativeStateHolder *>(external->Value());
}

#endif

#if JSI_VERSION >= 9
facebook::jsi::ArrayBuffer V8Runtime::createArrayBuffer(std::shared_ptr<facebook::jsi::MutableBuffer> buffer) {
  IsolateLocker isolate_locker(this);

  void *data = buffer->data();
  size_t length = buffer->size();
  auto bufferPtr =
      data != nullptr ? std::make_unique<std::shared_ptr<facebook::jsi::MutableBuffer>>(std::move(buffer)) : nullptr;
  std::unique_ptr<v8::BackingStore> backingStore = v8::ArrayBuffer::NewBackingStore(
      data,
      length,
      [](void * /*data*/, size_t /*length*/, void *deleterData) {
        if (deleterData != nullptr) {
          delete static_cast<std::shared_ptr<facebook::jsi::MutableBuffer> *>(deleterData);
        }
      },
      bufferPtr.release());

  v8::Local<v8::ArrayBuffer> arrayBuffer = v8::ArrayBuffer::New(GetIsolate(), std::move(backingStore));
  if (data == nullptr) {
    arrayBuffer->Detach(v8::Local<v8::Value>()).Check();
  }

  return make<jsi::Object>(V8ObjectValue::make(GetIsolate(), arrayBuffer)).getArrayBuffer(*this);
}
#endif

jsi::Value V8Runtime::createValue(v8::Local<v8::Value> value) const {
  IsolateLocker isolate_locker(this);
  if (value->IsInt32()) {
    return jsi::Value(value->Int32Value(GetContextLocal()).ToChecked());
  }
  if (value->IsNumber()) {
    return jsi::Value(value->NumberValue(GetContextLocal()).ToChecked());
  } else if (value->IsBoolean()) {
    return jsi::Value(value->BooleanValue(GetIsolate()));
  } else if (value->IsUndefined()) {
    return jsi::Value();
  } else if (value.IsEmpty() || value->IsNull()) {
    return jsi::Value(nullptr);
  } else if (value->IsString()) {
    // Note :: Non copy create
    return make<jsi::String>(V8StringValue::make(GetIsolate(), v8::Local<v8::String>::Cast(value)));
  } else if (value->IsObject()) {
    return make<jsi::Object>(V8ObjectValue::make(GetIsolate(), v8::Local<v8::Object>::Cast(value)));
  } else if (value->IsSymbol()) {
    return make<jsi::Symbol>(V8PointerValue<v8::Symbol>::make(GetIsolate(), v8::Local<v8::Symbol>::Cast(value)));
#if JSI_VERSION >= 6
  } else if (value->IsBigInt()) {
    return make<jsi::BigInt>(V8PointerValue<v8::BigInt>::make(GetIsolate(), v8::Local<v8::BigInt>::Cast(value)));
#endif
  } else {
    // What are you?
    std::abort();
  }
}

v8::Local<v8::Value> V8Runtime::valueReference(const jsi::Value &value) {
  v8::EscapableHandleScope handle_scope(GetIsolate());

  if (value.isUndefined()) {
    return handle_scope.Escape(v8::Undefined(GetIsolate()));
  } else if (value.isNull()) {
    return handle_scope.Escape(v8::Null(GetIsolate()));
  } else if (value.isBool()) {
    return handle_scope.Escape(v8::Boolean::New(GetIsolate(), value.getBool()));
  } else if (value.isNumber()) {
    return handle_scope.Escape(v8::Number::New(GetIsolate(), value.getNumber()));
  } else if (value.isString()) {
    return handle_scope.Escape(stringRef(value.asString(*this)));
  } else if (value.isObject()) {
    return handle_scope.Escape(objectRef(value.getObject(*this)));
  } else if (value.isSymbol()) {
    return handle_scope.Escape(symbolRef(value.getSymbol(*this)));
#if JSI_VERSION >= 6
  } else if (value.isBigInt()) {
    return handle_scope.Escape(bigIntRef(value.getBigInt(*this)));
#endif
  } else {
    // What are you?
    std::abort();
  }
}

// Adopted from Node.js code
/*static*/ V8Runtime *V8Runtime::GetCurrent(v8::Local<v8::Context> context) noexcept {
  if (/*UNLIKELY*/ context.IsEmpty()) {
    return nullptr;
  }
  if (/*UNLIKELY*/
      context->GetNumberOfEmbedderDataFields() <= ContextEmbedderIndex::ContextTag) {
    return nullptr;
  }
  if (/*UNLIKELY*/ (
      context->GetAlignedPointerFromEmbedderData(ContextEmbedderIndex::ContextTag) !=
      V8Runtime::RuntimeContextTagPtr)) {
    return nullptr;
  }
  return static_cast<V8Runtime *>(context->GetAlignedPointerFromEmbedderData(ContextEmbedderIndex::Runtime));
}

bool V8Runtime::HasUnhandledPromiseRejection() noexcept {
  return !!last_unhandled_promise_;
}

std::unique_ptr<UnhandledPromiseRejection> V8Runtime::GetAndClearLastUnhandledPromiseRejection() noexcept {
  return std::exchange(last_unhandled_promise_, nullptr);
}

// Adopted from V8 d8 utility code
/*static*/ void V8Runtime::PromiseRejectCallback(v8::PromiseRejectMessage data) {
  if (data.GetEvent() == v8::kPromiseRejectAfterResolved || data.GetEvent() == v8::kPromiseResolveAfterResolved) {
    // Ignore reject/resolve after resolved.
    return;
  }

  v8::Local<v8::Promise> promise = data.GetPromise();
  v8::Isolate *isolate = promise->GetIsolate();
  v8::MaybeLocal<v8::Context> context = promise->GetCreationContext();

  if (context.IsEmpty()) {
    return;
  }

  V8Runtime *runtime = V8Runtime::GetCurrent(context.ToLocalChecked());
  if (!runtime) {
    return;
  }

  if (data.GetEvent() == v8::kPromiseHandlerAddedAfterReject) {
    runtime->RemoveUnhandledPromise(promise);
    return;
  }

  isolate->SetCaptureStackTraceForUncaughtExceptions(true);
  v8::Local<v8::Value> exception = data.GetValue();
  v8::Local<v8::Message> message;
  // Assume that all objects are stack-traces.
  if (exception->IsObject()) {
    message = v8::Exception::CreateMessage(isolate, exception);
  }
  if (!exception->IsNativeError() && (message.IsEmpty() || message->GetStackTrace().IsEmpty())) {
    // If there is no real Error object, manually throw and catch a stack trace.
    v8::TryCatch try_catch(isolate);
    try_catch.SetVerbose(true);
    isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8Literal(isolate, "Unhandled Promise.")));
    message = try_catch.Message();
    exception = try_catch.Exception();
  }

  runtime->SetUnhandledPromise(promise, message, exception);
}

// Adopted from V8 d8 utility code
void V8Runtime::SetUnhandledPromise(
    v8::Local<v8::Promise> promise,
    v8::Local<v8::Message> message,
    v8::Local<v8::Value> exception) {
  if (ignore_unhandled_promises_)
    return;
  DCHECK_EQ(promise->GetIsolate(), GetIsolate());
  last_unhandled_promise_ = std::make_unique<UnhandledPromiseRejection>(UnhandledPromiseRejection{
      v8::Global<v8::Promise>(GetIsolate(), promise),
      v8::Global<v8::Message>(GetIsolate(), message),
      v8::Global<v8::Value>(GetIsolate(), exception)});
}

// Adopted from V8 d8 utility code
void V8Runtime::RemoveUnhandledPromise(v8::Local<v8::Promise> promise) {
  if (ignore_unhandled_promises_)
    return;
  // Remove handled promises from the list
  DCHECK_EQ(promise->GetIsolate(), GetIsolate());
  if (last_unhandled_promise_ && last_unhandled_promise_->promise.Get(GetIsolate()) == promise) {
    last_unhandled_promise_.reset();
  }
}

//=============================================================================
// Make V8 JSI runtime
//=============================================================================

V8JSI_EXPORT std::unique_ptr<jsi::Runtime> __cdecl makeV8Runtime(V8RuntimeArgs &&args) {
  return std::make_unique<V8Runtime>(std::move(args));
}

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
void openInspector(jsi::Runtime &runtime) {
  V8Runtime &v8Runtime = reinterpret_cast<V8Runtime &>(runtime);
  std::shared_ptr<inspector::Agent> inspector_agent = v8Runtime.getInspectorAgent();
  if (inspector_agent) {
    inspector_agent->start();
  }
}

void openInspectors_toberemoved() {
  inspector::Agent::startAll();
}
#endif

} // namespace v8runtime
