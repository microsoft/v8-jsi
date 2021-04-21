// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "V8JsiRuntime_impl.h"

#include "libplatform/libplatform.h"
#include "v8.h"

#include "V8Platform.h"
#include "napi/js_native_api_v8.h"
#include "public/ScriptStore.h"
#include "public/js_native_api.h"

#include "napi/util-inl.h"

#include <atomic>
#include <cstdlib>
#include <list>
#include <locale>
#include <mutex>
#include <sstream>

using namespace facebook;

#ifndef _ISOLATE_CONTEXT_ENTER
#define _ISOLATE_CONTEXT_ENTER                      \
  v8::Isolate *isolate = v8::Isolate::GetCurrent(); \
  v8::Isolate::Scope isolate_scope(isolate);        \
  v8::HandleScope handle_scope(isolate);            \
  v8::Context::Scope context_scope(context_.Get(isolate));
#endif

using namespace facebook;

namespace v8runtime {

thread_local uint16_t V8Runtime::tls_isolate_usage_counter_ = 0;

struct ContextEmbedderIndex {
  constexpr static int Runtime = 0;
  constexpr static int ContextTag = 1;
};

#ifdef USE_DEFAULT_PLATFORM
std::unique_ptr<v8::Platform> V8PlatformHolder::platform_s_;
#else
/*static */ std::unique_ptr<V8Platform> V8PlatformHolder::platform_s_;
#endif
/*static */ std::atomic_uint32_t V8PlatformHolder::use_count_s_{0};
/*static */ std::mutex V8PlatformHolder::mutex_s_;

class TaskAdapter : public v8runtime::JSITask {
 public:
  TaskAdapter(std::unique_ptr<v8::Task> &&task) : task_(std::move(task)) {}

  void run() override {
    task_->Run();
  }

 private:
  std::unique_ptr<v8::Task> task_;
};

class IdleTaskAdapter : public v8runtime::JSIIdleTask {
 public:
  IdleTaskAdapter(std::unique_ptr<v8::IdleTask> &&task) : task_(std::move(task)) {}

  void run(double deadline_in_seconds) override {
    task_->Run(deadline_in_seconds);
  }

 private:
  std::unique_ptr<v8::IdleTask> task_;
};

class TaskRunnerAdapter : public v8::TaskRunner {
 public:
  TaskRunnerAdapter(std::unique_ptr<v8runtime::JSITaskRunner> &&taskRunner) : taskRunner_(std::move(taskRunner)) {}

  void PostTask(std::unique_ptr<v8::Task> task) override {
    taskRunner_->postTask(node::static_unique_pointer_cast<JSITask>(std::make_unique<TaskAdapter>(std::move(task))));
  }

  void PostDelayedTask(std::unique_ptr<v8::Task> task, double delay_in_seconds) override {
    taskRunner_->postDelayedTask(
        node::static_unique_pointer_cast<JSITask>(std::make_unique<TaskAdapter>(std::move(task))), delay_in_seconds);
  }

  bool IdleTasksEnabled() override {
    return taskRunner_->IdleTasksEnabled();
  }

  void PostIdleTask(std::unique_ptr<v8::IdleTask> task) override {
    taskRunner_->postIdleTask(
        node::static_unique_pointer_cast<JSIIdleTask>(std::make_unique<IdleTaskAdapter>(std::move(task))));
  }

  void PostNonNestableTask(std::unique_ptr<v8::Task> task) override {
    // TODO: non-nestable
    taskRunner_->postTask(node::static_unique_pointer_cast<JSITask>(std::make_unique<TaskAdapter>(std::move(task))));
  }

  bool NonNestableTasksEnabled() const override {
    return true;
  }

 private:
  std::unique_ptr<v8runtime::JSITaskRunner> taskRunner_;
};

// String utilities
namespace {
std::string JSStringToSTLString(v8::Isolate *isolate, v8::Local<v8::String> string) {
  int utfLen = string->Utf8Length(isolate);
  std::string result;
  result.resize(utfLen);
  string->WriteUtf8(isolate, &result[0], utfLen);
  return result;
}

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

  if (args_.custom_snapshot_blob) {
    custom_snapshot_startup_data_ = {
        reinterpret_cast<const char *>(args_.custom_snapshot_blob->data()),
        static_cast<int>(args_.custom_snapshot_blob->size())};
    create_params_.snapshot_blob = &custom_snapshot_startup_data_;
  }

  // One per each runtime.
  create_params_.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();

  if (args_.initial_heap_size_in_bytes > 0 || args_.maximum_heap_size_in_bytes > 0) {
    v8::ResourceConstraints constraints;
    constraints.ConfigureDefaultsFromHeapSize(args_.initial_heap_size_in_bytes, args_.maximum_heap_size_in_bytes);

    create_params_.constraints = constraints;
  }

  counter_map_ = new CounterMap();
  if (args_.trackGCObjectStats) {
    create_params_.counter_lookup_callback = LookupCounter;
    create_params_.create_histogram_callback = CreateHistogram;
    create_params_.add_histogram_sample_callback = AddHistogramSample;
  }

  isolate_ = v8::Isolate::Allocate();
  if (isolate_ == nullptr)
    std::abort();

  foreground_task_runner_ = std::make_shared<TaskRunnerAdapter>(std::move(args_.foreground_task_runner));
  isolate_data_ = new v8runtime::IsolateData(isolate_, foreground_task_runner_);
  isolate_->SetData(v8runtime::ISOLATE_DATA_SLOT, isolate_data_);

  v8::Isolate::Initialize(isolate_, create_params_);

  isolate_data_->CreateProperties();

  if (!args_.ignoreUnhandledPromises) {
    isolate_->SetPromiseRejectCallback(PromiseRejectCallback);
  }

  if (args_.trackGCObjectStats) {
    MapCounters(isolate_, "v8jsi");
  }

  if (args_.enableJitTracing) {
    isolate_->SetJitCodeEventHandler(v8::kJitCodeEventDefault, JitCodeEventListener);
  }

  if (args_.backgroundMode) {
    isolate_->IsolateInBackgroundNotification();
  }

  if (args_.enableMessageTracing) {
    isolate_->AddMessageListener(OnMessage);
  }

  if (args_.enableGCTracing) {
    isolate_->AddGCPrologueCallback(GCPrologueCallback);
    isolate_->AddGCEpilogueCallback(GCEpilogueCallback);
  }

  isolate_->AddNearHeapLimitCallback(NearHeapLimitCallback, nullptr);

  // TODO :: Toggle for ship builds.
  isolate_->SetAbortOnUncaughtExceptionCallback([](v8::Isolate *) { return true; });

  isolate_->Enter();

  TRACEV8RUNTIME_VERBOSE("CreateNewIsolate", TraceLoggingString("end", "op"));
  DumpCounters("isolate_created");

  return isolate_;
}

void V8Runtime::createHostObjectConstructorPerContext() {
  // Create and keep the constructor for creating Host objects.
  v8::Local<v8::FunctionTemplate> constructorForHostObjectTemplate = v8::FunctionTemplate::New(isolate_);
  v8::Local<v8::ObjectTemplate> hostObjectTemplate = constructorForHostObjectTemplate->InstanceTemplate();
  hostObjectTemplate->SetHandler(v8::NamedPropertyHandlerConfiguration(
      HostObjectProxy::Get, HostObjectProxy::Set, nullptr, nullptr, HostObjectProxy::Enumerator));

  // V8 distinguishes between named properties (strings and symbols) and indexed properties (number)
  // Note that we're not passing an Enumerator here, otherwise we'd be double-counting since JSI doesn't make the
  // distinction
  hostObjectTemplate->SetIndexedPropertyHandler(HostObjectProxy::GetIndexed, HostObjectProxy::SetIndexed);
  hostObjectTemplate->SetInternalFieldCount(1);
  host_object_constructor_.Reset(
      isolate_, constructorForHostObjectTemplate->GetFunction(context_.Get(isolate_)).ToLocalChecked());
}

void V8Runtime::initializeTracing() {
#ifdef _WIN32
  globalInitializeTracing();
#endif

  TRACEV8RUNTIME_VERBOSE("Initializing");
}

void V8Runtime::initializeV8() {
  std::vector<const char *> argv;
  argv.push_back("v8jsi");

  if (args_.liteMode)
    argv.push_back("--lite-mode");

  if (args_.trackGCObjectStats)
    argv.push_back("--track_gc_object_stats");

  if (args_.enableGCApi)
    argv.push_back("--expose_gc");

  int argc = static_cast<int>(argv.size());
  v8::V8::SetFlagsFromCommandLine(&argc, const_cast<char **>(&argv[0]), false);
}

V8Runtime::V8Runtime(V8RuntimeArgs &&args) : args_(std::move(args)) {
  initializeTracing();
  initializeV8();

  v8::V8::SetUnhandledExceptionCallback([](_EXCEPTION_POINTERS *exception_pointers) -> int {
    TRACEV8RUNTIME_CRITICAL("V8::SetUnhandledExceptionCallback");
    return 0;
  });

  // Try to reuse the already existing isolate in this thread.
  if (tls_isolate_usage_counter_++ > 0) {
    TRACEV8RUNTIME_WARNING("Reusing existing V8 isolate in the current thread !");
    isolate_ = v8::Isolate::GetCurrent();
  } else {
    platform_holder_.addUsage();
    CreateNewIsolate();
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

#ifdef _WIN32
  if (args_.enableInspector) {
    TRACEV8RUNTIME_VERBOSE("Inspector enabled");
    inspector_agent_ = std::make_unique<inspector::Agent>(
        platform_holder_.Get(), isolate_, context_.Get(GetIsolate()), "JSIRuntime context", args_.inspectorPort);
    inspector_agent_->start();

    if (args_.waitForDebugger) {
      TRACEV8RUNTIME_VERBOSE("Waiting for inspector frontend to attach");
      inspector_agent_->waitForDebugger();
    }
  }
#endif

  createHostObjectConstructorPerContext();
}

V8Runtime::~V8Runtime() {
  // TODO: add check that destruction happens on the same thread id as construction
#ifdef _WIN32
  if (inspector_agent_ && inspector_agent_->IsStarted()) {
    inspector_agent_->stop();
  }
  inspector_agent_.reset();
#endif

  host_object_constructor_.Reset();
  context_.Reset();

  for (std::shared_ptr<HostObjectLifetimeTracker> hostObjectLifetimeTracker : host_object_lifetime_tracker_list_) {
    hostObjectLifetimeTracker->ResetHostObject(false /*isGC*/);
  }

  if (--tls_isolate_usage_counter_ == 0) {
    IsolateData *isolate_data = reinterpret_cast<IsolateData *>(isolate_->GetData(ISOLATE_DATA_SLOT));
    delete isolate_data;

    isolate_->SetData(v8runtime::ISOLATE_DATA_SLOT, nullptr);

    isolate_->Exit();
    isolate_->Dispose();

    delete create_params_.array_buffer_allocator;

    platform_holder_.releaseUsage();
  }

  // Note :: We never dispose V8 here. Is it required ?
}

jsi::Value V8Runtime::evaluateJavaScript(
    const std::shared_ptr<const jsi::Buffer> &buffer,
    const std::string &sourceURL) {
  TRACEV8RUNTIME_VERBOSE("evaluateJavaScript", TraceLoggingString("start", "op"));

  _ISOLATE_CONTEXT_ENTER

  v8::Local<v8::String> sourceV8String;
  // If we'd somehow know the buffer includes only ASCII characters, we could use External strings to avoid the copy
  /* ExternalOwningOneByteStringResource *external_string_resource = new ExternalOwningOneByteStringResource(buffer);
  if (!v8::String::NewExternalOneByte(isolate, external_string_resource).ToLocal(&sourceV8String)) {
    std::abort();
  }
  delete external_string_resource; */

  if (!v8::String::NewFromUtf8(
           isolate,
           reinterpret_cast<const char *>(buffer->data()),
           v8::NewStringType::kNormal,
           static_cast<int>(buffer->size()))
           .ToLocal(&sourceV8String)) {
    std::abort();
  }

  jsi::Value result = ExecuteString(sourceV8String, sourceURL);

  TRACEV8RUNTIME_VERBOSE("evaluateJavaScript", TraceLoggingString("end", "op"));
  DumpCounters("script evaluated");

  return result;
}

// The callback that is invoked by v8 whenever the JavaScript 'print'
// function is called.  Prints its arguments on stdout separated by
// spaces and ending with a newline.
void Print(const v8::FunctionCallbackInfo<v8::Value> &args) {
  bool first = true;
  for (int i = 0; i < args.Length(); i++) {
    v8::HandleScope handle_scope(args.GetIsolate());
    if (first) {
      first = false;
    } else {
      printf(" ");
    }
    v8::String::Utf8Value str(args.GetIsolate(), args[i]);
    const char *cstr = ToCString(str);
    printf("%s", cstr);
  }
  printf("\n");
  fflush(stdout);
}

v8::Local<v8::Context> V8Runtime::CreateContext(v8::Isolate *isolate) {
  // Create a template for the global object.

  TRACEV8RUNTIME_VERBOSE("CreateContext", TraceLoggingString("start", "op"));

  v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);

  global->Set(
      v8::String::NewFromUtf8(isolate, "print", v8::NewStringType::kNormal).ToLocalChecked(),
      v8::FunctionTemplate::New(isolate, Print));

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

jsi::Value V8Runtime::ExecuteString(const v8::Local<v8::String> &source, const std::string &sourceURL) {
  _ISOLATE_CONTEXT_ENTER
  v8::TryCatch try_catch(isolate);

  v8::Local<v8::String> urlV8String =
      v8::String::NewFromUtf8(isolate, reinterpret_cast<const char *>(sourceURL.c_str())).ToLocalChecked();
  v8::ScriptOrigin origin(urlV8String);

  v8::Local<v8::Context> context(isolate->GetCurrentContext());
  v8::Local<v8::Script> script;

  v8::ScriptCompiler::CompileOptions options = v8::ScriptCompiler::CompileOptions::kNoCompileOptions;
  v8::ScriptCompiler::CachedData *cached_data = nullptr;

  std::shared_ptr<const jsi::Buffer> cache;
  if (args_.preparedScriptStore) {
    jsi::ScriptSignature scriptSignature = {sourceURL, 1};
    jsi::JSRuntimeSignature runtimeSignature = {"V8", 76};
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

  if (!v8::ScriptCompiler::Compile(context, &script_source, options).ToLocal(&script)) {
    // Print errors that happened during compilation.
    if (/*report_exceptions*/ true)
      ReportException(&try_catch);
    return createValue(v8::Undefined(GetIsolate()));
  } else {
    v8::Local<v8::Value> result;
    if (!script->Run(context).ToLocal(&result)) {
      assert(try_catch.HasCaught());
      // Print errors that happened during execution.
      if (/*report_exceptions*/ true) {
        ReportException(&try_catch);
      }
      return createValue(v8::Undefined(GetIsolate()));
    } else {
      assert(!try_catch.HasCaught());

      if (args_.preparedScriptStore && options != v8::ScriptCompiler::CompileOptions::kConsumeCodeCache) {
        v8::ScriptCompiler::CachedData *codeCache = v8::ScriptCompiler::CreateCodeCache(script->GetUnboundScript());

        jsi::ScriptSignature scriptSignature = {sourceURL, 1};
        jsi::JSRuntimeSignature runtimeSignature = {"V8", 76};

        args_.preparedScriptStore->persistPreparedScript(
            std::make_shared<ByteArrayBuffer>(codeCache->data, codeCache->length),
            scriptSignature,
            runtimeSignature,
            "perf");
      }

      return createValue(result);
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
};

std::shared_ptr<const facebook::jsi::PreparedJavaScript> V8Runtime::prepareJavaScript(
    const std::shared_ptr<const facebook::jsi::Buffer> &buffer,
    std::string sourceURL) {
  _ISOLATE_CONTEXT_ENTER
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> source;
  if (!v8::String::NewFromUtf8(
           isolate,
           reinterpret_cast<const char *>(buffer->data()),
           v8::NewStringType::kNormal,
           static_cast<int>(buffer->size()))
           .ToLocal(&source)) {
    std::abort();
  }

  v8::Local<v8::String> urlV8String =
      v8::String::NewFromUtf8(isolate, reinterpret_cast<const char *>(sourceURL.c_str())).ToLocalChecked();
  v8::ScriptOrigin origin(urlV8String);
  v8::Local<v8::Context> context(isolate->GetCurrentContext());
  v8::Local<v8::Script> script;
  v8::ScriptCompiler::CompileOptions options = v8::ScriptCompiler::CompileOptions::kNoCompileOptions;
  v8::ScriptCompiler::CachedData *cached_data = nullptr;

  v8::ScriptCompiler::Source script_source(source, origin, cached_data);

  if (!v8::ScriptCompiler::Compile(context, &script_source, options).ToLocal(&script)) {
    ReportException(&try_catch);
    return nullptr;
  } else {
    v8::ScriptCompiler::CachedData *codeCache = v8::ScriptCompiler::CreateCodeCache(script->GetUnboundScript());

    auto prepared = std::make_shared<V8PreparedJavaScript>();
    prepared->scriptSignature = {sourceURL, 1};
    prepared->runtimeSignature = {"V8", 76};
    prepared->buffer.assign(codeCache->data, codeCache->data + codeCache->length);
    prepared->sourceBuffer = buffer;
    return prepared;
  }
}

facebook::jsi::Value V8Runtime::evaluatePreparedJavaScript(
    const std::shared_ptr<const facebook::jsi::PreparedJavaScript> &js) {
  _ISOLATE_CONTEXT_ENTER

  auto prepared = static_cast<const V8PreparedJavaScript *>(js.get());

  v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> source;
  if (!v8::String::NewFromUtf8(
           isolate,
           reinterpret_cast<const char *>(prepared->sourceBuffer->data()),
           v8::NewStringType::kNormal,
           static_cast<int>(prepared->sourceBuffer->size()))
           .ToLocal(&source)) {
    std::abort();
  }
  v8::Local<v8::String> urlV8String =
      v8::String::NewFromUtf8(isolate, reinterpret_cast<const char *>(prepared->scriptSignature.url.c_str()))
          .ToLocalChecked();
  v8::ScriptOrigin origin(urlV8String);
  v8::Local<v8::Context> context(isolate->GetCurrentContext());
  v8::Local<v8::Script> script;

  v8::ScriptCompiler::CompileOptions options = v8::ScriptCompiler::CompileOptions::kConsumeCodeCache;
  v8::ScriptCompiler::CachedData *cached_data =
      new v8::ScriptCompiler::CachedData(prepared->buffer.data(), static_cast<int>(prepared->buffer.size()));

  v8::ScriptCompiler::Source script_source(source, origin, cached_data);

  if (!v8::ScriptCompiler::Compile(context, &script_source, options).ToLocal(&script)) {
    ReportException(&try_catch);
    return createValue(v8::Undefined(GetIsolate()));
  } else {
    v8::Local<v8::Value> result;
    if (!script->Run(context).ToLocal(&result)) {
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
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::Message> message = try_catch->Message();
  if (message.IsEmpty()) {
    // V8 didn't provide any extra information about this error; just
    // throw the exception.
    v8::String::Utf8Value exception(isolate, try_catch->Exception());
    throw jsi::JSError(*this, ToCString(exception));
  } else {
    std::stringstream sstr;

    // Print (filename):(line number): (message) - this would differ from what JSI expects (it wants the plain
    // stacktrace)
    /*v8::String::Utf8Value filename(
        isolate, message->GetScriptOrigin().ResourceName());
    v8::Local<v8::Context> context(isolate->GetCurrentContext());
    const char *filename_string = ToCString(filename);
    int linenum = message->GetLineNumber(context).FromJust();
    sstr << filename_string << ":" << linenum << ": " << exception_string
         << std::endl;

    // Print line of source code
    v8::String::Utf8Value sourceline(
        isolate, message->GetSourceLine(context).ToLocalChecked());
    const char *sourceline_string = ToCString(sourceline);
    sstr << sourceline_string << std::endl;

    // Print wavy underline (GetUnderline is deprecated).
    int start = message->GetStartColumn(context).FromJust();
    for (int i = 0; i < start; i++) {
      sstr << " ";
    }
    int end = message->GetEndColumn(context).FromJust();
    for (int i = start; i < end; i++) {
      sstr << "^";
    }
    sstr << std::endl;*/

    v8::Local<v8::Value> stack_trace_string;
    if (try_catch->StackTrace(context_.Get(isolate)).ToLocal(&stack_trace_string) && stack_trace_string->IsString() &&
        v8::Local<v8::String>::Cast(stack_trace_string)->Length() > 0) {
      v8::String::Utf8Value stack_trace(isolate, stack_trace_string);
      const char *stack_trace_string2 = ToCString(stack_trace);
      sstr << stack_trace_string2 << std::endl;
    }

    v8::String::Utf8Value ex_message(isolate, message->Get());
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
    if (ex_messages.find("Maximum call stack size exceeded") == std::string::npos) {
      auto err = jsi::JSError(*this, ex_messages);
      err.value().getObject(*this).setProperty(
          *this, "stack", facebook::jsi::String::createFromUtf8(*this, sstr.str()));
      err.setStack(sstr.str());
      throw err;
    } else {
      // If we're already in stack overflow, calling the Error constructor pushes it overboard
      throw jsi::JSError(*this, ex_messages, sstr.str());
    }
  }
}

jsi::Object V8Runtime::global() {
  _ISOLATE_CONTEXT_ENTER
  return make<jsi::Object>(V8ObjectValue::make(context_.Get(isolate)->Global()));
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

  _ISOLATE_CONTEXT_ENTER
  const V8StringValue *string = static_cast<const V8StringValue *>(pv);
  return V8StringValue::make(string->get(GetIsolate()));
}

jsi::Runtime::PointerValue *V8Runtime::cloneObject(const jsi::Runtime::PointerValue *pv) {
  if (!pv) {
    return nullptr;
  }

  _ISOLATE_CONTEXT_ENTER
  const V8ObjectValue *object = static_cast<const V8ObjectValue *>(pv);
  return V8ObjectValue::make(object->get(GetIsolate()));
}

jsi::Runtime::PointerValue *V8Runtime::clonePropNameID(const jsi::Runtime::PointerValue *pv) {
  if (!pv) {
    return nullptr;
  }

  _ISOLATE_CONTEXT_ENTER
  const V8StringValue *string = static_cast<const V8StringValue *>(pv);
  return V8StringValue::make(string->get(GetIsolate()));
}

jsi::Runtime::PointerValue *V8Runtime::cloneSymbol(const jsi::Runtime::PointerValue *pv) {
  if (!pv) {
    return nullptr;
  }

  _ISOLATE_CONTEXT_ENTER
  const V8PointerValue<v8::Symbol> *symbol = static_cast<const V8PointerValue<v8::Symbol> *>(pv);
  return V8PointerValue<v8::Symbol>::make(symbol->get(GetIsolate()));
}

std::string V8Runtime::symbolToString(const jsi::Symbol &sym) {
  _ISOLATE_CONTEXT_ENTER
  return "Symbol(" + JSStringToSTLString(GetIsolate(), v8::Local<v8::String>::Cast(symbolRef(sym)->Description())) +
      ")";
}

jsi::PropNameID V8Runtime::createPropNameIDFromAscii(const char *str, size_t length) {
  _ISOLATE_CONTEXT_ENTER
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

  auto res = make<jsi::PropNameID>(V8StringValue::make(v8::Local<v8::String>::Cast(v8String)));
  return res;
}

jsi::PropNameID V8Runtime::createPropNameIDFromUtf8(const uint8_t *utf8, size_t length) {
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::String> v8String;
  if (!v8::String::NewFromUtf8(
           GetIsolate(), reinterpret_cast<const char *>(utf8), v8::NewStringType::kNormal, static_cast<int>(length))
           .ToLocal(&v8String)) {
    std::stringstream strstream;
    strstream << "Unable to create property id: " << utf8;
    throw jsi::JSError(*this, strstream.str());
  }

  auto res = make<jsi::PropNameID>(V8StringValue::make(v8::Local<v8::String>::Cast(v8String)));
  return res;
}

jsi::PropNameID V8Runtime::createPropNameIDFromString(const jsi::String &str) {
  _ISOLATE_CONTEXT_ENTER
  return make<jsi::PropNameID>(V8StringValue::make(v8::Local<v8::String>::Cast(stringRef(str))));
}

std::string V8Runtime::utf8(const jsi::PropNameID &sym) {
  _ISOLATE_CONTEXT_ENTER
  return JSStringToSTLString(GetIsolate(), v8::Local<v8::String>::Cast(valueRef(sym)));
}

bool V8Runtime::compare(const jsi::PropNameID &a, const jsi::PropNameID &b) {
  _ISOLATE_CONTEXT_ENTER
  return valueRef(a)->Equals(GetIsolate()->GetCurrentContext(), valueRef(b)).ToChecked();
}

jsi::String V8Runtime::createStringFromAscii(const char *str, size_t length) {
  return this->createStringFromUtf8(reinterpret_cast<const uint8_t *>(str), length);
}

jsi::String V8Runtime::createStringFromUtf8(const uint8_t *str, size_t length) {
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::String> v8string;
  if (!v8::String::NewFromUtf8(
           v8::Isolate::GetCurrent(),
           reinterpret_cast<const char *>(str),
           v8::NewStringType::kNormal,
           static_cast<int>(length))
           .ToLocal(&v8string)) {
    throw jsi::JSError(*this, "V8 string creation failed.");
  }

  jsi::String jsistr = make<jsi::String>(V8StringValue::make(v8string));
  return jsistr;
}

std::string V8Runtime::utf8(const jsi::String &str) {
  _ISOLATE_CONTEXT_ENTER
  return JSStringToSTLString(GetIsolate(), stringRef(str));
}

jsi::Object V8Runtime::createObject() {
  _ISOLATE_CONTEXT_ENTER
  return make<jsi::Object>(V8ObjectValue::make(v8::Object::New(GetIsolate())));
}

jsi::Object V8Runtime::createObject(std::shared_ptr<jsi::HostObject> hostobject) {
  _ISOLATE_CONTEXT_ENTER
  HostObjectProxy *hostObjectProxy = new HostObjectProxy(*this, hostobject);
  v8::Local<v8::Object> newObject;
  if (!host_object_constructor_.Get(isolate_)->NewInstance(isolate_->GetCurrentContext()).ToLocal(&newObject)) {
    throw jsi::JSError(*this, "HostObject construction failed!!");
  }

  newObject->SetInternalField(
      0, v8::Local<v8::External>::New(GetIsolate(), v8::External::New(GetIsolate(), hostObjectProxy)));

  AddHostObjectLifetimeTracker(std::make_shared<HostObjectLifetimeTracker>(*this, newObject, hostObjectProxy));

  return make<jsi::Object>(V8ObjectValue::make(newObject));
}

std::shared_ptr<jsi::HostObject> V8Runtime::getHostObject(const jsi::Object &obj) {
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::External> internalField = v8::Local<v8::External>::Cast(objectRef(obj)->GetInternalField(0));
  HostObjectProxy *hostObjectProxy = reinterpret_cast<HostObjectProxy *>(internalField->Value());
  return hostObjectProxy->getHostObject();
}

jsi::Value V8Runtime::getProperty(const jsi::Object &obj, const jsi::String &name) {
  _ISOLATE_CONTEXT_ENTER
  return createValue(objectRef(obj)->Get(context_.Get(isolate), stringRef(name)).ToLocalChecked());
}

jsi::Value V8Runtime::getProperty(const jsi::Object &obj, const jsi::PropNameID &name) {
  _ISOLATE_CONTEXT_ENTER
  v8::MaybeLocal<v8::Value> result = objectRef(obj)->Get(isolate_->GetCurrentContext(), valueRef(name));
  if (result.IsEmpty())
    throw jsi::JSError(*this, "V8Runtime::getProperty failed.");
  return createValue(result.ToLocalChecked());
}

bool V8Runtime::hasProperty(const jsi::Object &obj, const jsi::String &name) {
  _ISOLATE_CONTEXT_ENTER
  v8::Maybe<bool> result = objectRef(obj)->Has(isolate_->GetCurrentContext(), stringRef(name));
  if (result.IsNothing())
    throw jsi::JSError(*this, "V8Runtime::setPropertyValue failed.");
  return result.FromJust();
}

bool V8Runtime::hasProperty(const jsi::Object &obj, const jsi::PropNameID &name) {
  _ISOLATE_CONTEXT_ENTER
  v8::Maybe<bool> result = objectRef(obj)->Has(isolate_->GetCurrentContext(), valueRef(name));
  if (result.IsNothing())
    throw jsi::JSError(*this, "V8Runtime::setPropertyValue failed.");
  return result.FromJust();
}

void V8Runtime::setPropertyValue(jsi::Object &object, const jsi::PropNameID &name, const jsi::Value &value) {
  _ISOLATE_CONTEXT_ENTER
  v8::Maybe<bool> result = objectRef(object)->Set(isolate_->GetCurrentContext(), valueRef(name), valueRef(value));
  if (!result.FromMaybe(false))
    throw jsi::JSError(*this, "V8Runtime::setPropertyValue failed.");
}

void V8Runtime::setPropertyValue(jsi::Object &object, const jsi::String &name, const jsi::Value &value) {
  _ISOLATE_CONTEXT_ENTER
  v8::Maybe<bool> result = objectRef(object)->Set(isolate_->GetCurrentContext(), stringRef(name), valueRef(value));
  if (!result.FromMaybe(false))
    throw jsi::JSError(*this, "V8Runtime::setPropertyValue failed.");
}

bool V8Runtime::isArray(const jsi::Object &obj) const {
  _ISOLATE_CONTEXT_ENTER
  return objectRef(obj)->IsArray();
}

bool V8Runtime::isArrayBuffer(const jsi::Object &obj) const {
  _ISOLATE_CONTEXT_ENTER
  return objectRef(obj)->IsArrayBuffer();
}

uint8_t *V8Runtime::data(const jsi::ArrayBuffer &obj) {
  _ISOLATE_CONTEXT_ENTER
  return reinterpret_cast<uint8_t *>(objectRef(obj).As<v8::ArrayBuffer>()->GetContents().Data());
}

size_t V8Runtime::size(const jsi::ArrayBuffer &obj) {
  _ISOLATE_CONTEXT_ENTER
  return objectRef(obj).As<v8::ArrayBuffer>()->ByteLength();
}

bool V8Runtime::isFunction(const jsi::Object &obj) const {
  _ISOLATE_CONTEXT_ENTER
  return objectRef(obj)->IsFunction();
}

bool V8Runtime::isHostObject(const jsi::Object &obj) const {
  _ISOLATE_CONTEXT_ENTER
  if (objectRef(obj)->InternalFieldCount() < 1) {
    return false;
  }

  auto internalFieldRef = objectRef(obj)->GetInternalField(0);
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
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::Array> propNames = objectRef(obj)
                                       ->GetPropertyNames(
                                           context_.Get(isolate_),
                                           v8::KeyCollectionMode::kIncludePrototypes,
                                           static_cast<v8::PropertyFilter>(v8::ONLY_ENUMERABLE | v8::SKIP_SYMBOLS),
                                           v8::IndexFilter::kIncludeIndices,
                                           v8::KeyConversionMode::kConvertToString)
                                       .ToLocalChecked();
  return make<jsi::Object>(V8ObjectValue::make(propNames)).getArray(*this);
}

jsi::WeakObject V8Runtime::createWeakObject(const jsi::Object &) {
  throw std::logic_error("Not implemented");
}

jsi::Value V8Runtime::lockWeakObject(jsi::WeakObject &) {
  throw std::logic_error("Not implemented");
}

jsi::Array V8Runtime::createArray(size_t length) {
  _ISOLATE_CONTEXT_ENTER
  return make<jsi::Object>(V8ObjectValue::make(v8::Array::New(GetIsolate(), static_cast<int>(length)))).getArray(*this);
}

size_t V8Runtime::size(const jsi::Array &arr) {
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::Array> array = v8::Local<v8::Array>::Cast(objectRef(arr));
  return array->Length();
}

jsi::Value V8Runtime::getValueAtIndex(const jsi::Array &arr, size_t i) {
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::Array> array = v8::Local<v8::Array>::Cast(objectRef(arr));
  return createValue(array->Get(context_.Get(isolate), static_cast<uint32_t>(i)).ToLocalChecked());
}

void V8Runtime::setValueAtIndexImpl(jsi::Array &arr, size_t i, const jsi::Value &value) {
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::Array> array = v8::Local<v8::Array>::Cast(objectRef(arr));
  array->Set(context_.Get(isolate), static_cast<uint32_t>(i), valueRef(value));
}

jsi::Function V8Runtime::createFunctionFromHostFunction(
    const jsi::PropNameID &name,
    unsigned int paramCount,
    jsi::HostFunctionType func) {
  _ISOLATE_CONTEXT_ENTER

  HostFunctionProxy *hostFunctionProxy = new HostFunctionProxy(*this, func);

  v8::Local<v8::Function> newFunction;
  if (!v8::Function::New(
           isolate_->GetCurrentContext(),
           HostFunctionProxy::HostFunctionCallback,
           v8::Local<v8::External>::New(GetIsolate(), v8::External::New(GetIsolate(), hostFunctionProxy)),
           paramCount)
           .ToLocal(&newFunction)) {
    throw jsi::JSError(*this, "Creation of HostFunction failed.");
  }

  newFunction->SetName(v8::Local<v8::String>::Cast(valueRef(name)));

  AddHostObjectLifetimeTracker(std::make_shared<HostObjectLifetimeTracker>(*this, newFunction, hostFunctionProxy));

  return make<jsi::Object>(V8ObjectValue::make(newFunction)).getFunction(*this);
}

bool V8Runtime::isHostFunction(const jsi::Function &obj) const {
  std::abort();
  return false;
}

jsi::HostFunctionType &V8Runtime::getHostFunction(const jsi::Function &obj) {
  std::abort();
}

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
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::Function> func = v8::Local<v8::Function>::Cast(objectRef(jsiFunc));

  // TODO: This may be a bit costly when there are large number of JS function
  // calls from native. Evaluate !
  std::string functionName = getFunctionName(isolate_, func);

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
    argv.push_back(valueRef(args[i]));
  }

  v8::TryCatch trycatch(isolate_);
  v8::MaybeLocal<v8::Value> result =
      func->Call(isolate_->GetCurrentContext(), valueRef(jsThis), static_cast<int>(count), argv.data());

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
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::Function> func = v8::Local<v8::Function>::Cast(objectRef(jsiFunc));

  // TODO: This may be a bit costly when there are large number of JS function
  // calls from native. Evaluate !
  std::string functionName = getFunctionName(isolate_, func);
  TRACEV8RUNTIME_VERBOSE(
      "CallConstructor", TraceLoggingString(functionName.c_str(), "name"), TraceLoggingString("start", "op"));

  std::vector<v8::Local<v8::Value>> argv;
  for (size_t i = 0; i < count; i++) {
    argv.push_back(valueRef(args[i]));
  }

  v8::TryCatch trycatch(isolate_);
  v8::Local<v8::Object> newObject;
  if (!func->NewInstance(GetIsolate()->GetCurrentContext(), static_cast<int>(count), argv.data()).ToLocal(&newObject)) {
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
  _ISOLATE_CONTEXT_ENTER
  return stringRef(a)->StrictEquals(stringRef(b));
}

bool V8Runtime::strictEquals(const jsi::Object &a, const jsi::Object &b) const {
  _ISOLATE_CONTEXT_ENTER
  return objectRef(a)->StrictEquals(objectRef(b));
}

bool V8Runtime::strictEquals(const jsi::Symbol &a, const jsi::Symbol &b) const {
  _ISOLATE_CONTEXT_ENTER
  return symbolRef(a)->StrictEquals(symbolRef(b));
}

bool V8Runtime::instanceOf(const jsi::Object &o, const jsi::Function &f) {
  _ISOLATE_CONTEXT_ENTER
  return objectRef(o)->InstanceOf(GetIsolate()->GetCurrentContext(), objectRef(f)).ToChecked();
}

jsi::Value V8Runtime::createValue(v8::Local<v8::Value> value) const {
  _ISOLATE_CONTEXT_ENTER
  if (value->IsInt32()) {
    return jsi::Value(value->Int32Value(GetIsolate()->GetCurrentContext()).ToChecked());
  }
  if (value->IsNumber()) {
    return jsi::Value(value->NumberValue(GetIsolate()->GetCurrentContext()).ToChecked());
  } else if (value->IsBoolean()) {
    return jsi::Value(value->BooleanValue(GetIsolate()));
  } else if (value->IsUndefined()) {
    return jsi::Value();
  } else if (value.IsEmpty() || value->IsNull()) {
    return jsi::Value(nullptr);
  } else if (value->IsString()) {
    // Note :: Non copy create
    return make<jsi::String>(V8StringValue::make(v8::Local<v8::String>::Cast(value)));
  } else if (value->IsObject()) {
    return make<jsi::Object>(V8ObjectValue::make(v8::Local<v8::Object>::Cast(value)));
  } else if (value->IsSymbol()) {
    return make<jsi::Symbol>(V8PointerValue<v8::Symbol>::make(v8::Local<v8::Symbol>::Cast(value)));
  } else {
    // What are you?
    std::abort();
  }
}

v8::Local<v8::Value> V8Runtime::valueRef(const jsi::Value &value) {
  v8::EscapableHandleScope handle_scope(v8::Isolate::GetCurrent());

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
  } else {
    // What are you?
    std::abort();
  }
}

// Adoped from Node.js code
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

// Adoped from V8 d8 utility code
/*static*/ void V8Runtime::PromiseRejectCallback(v8::PromiseRejectMessage data) {
  if (data.GetEvent() == v8::kPromiseRejectAfterResolved || data.GetEvent() == v8::kPromiseResolveAfterResolved) {
    // Ignore reject/resolve after resolved.
    return;
  }

  v8::Local<v8::Promise> promise = data.GetPromise();
  v8::Isolate *isolate = promise->GetIsolate();
  v8::Local<v8::Context> context = promise->CreationContext();
  V8Runtime *runtime = V8Runtime::GetCurrent(context);
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

// Adoped from V8 d8 utility code
void V8Runtime::SetUnhandledPromise(
    v8::Local<v8::Promise> promise,
    v8::Local<v8::Message> message,
    v8::Local<v8::Value> exception) {
  if (ignore_unhandled_promises_)
    return;
  DCHECK_EQ(promise->GetIsolate(), isolate_);
  last_unhandled_promise_ = std::make_unique<UnhandledPromiseRejection>(UnhandledPromiseRejection{
      v8::Global<v8::Promise>(isolate_, promise),
      v8::Global<v8::Message>(isolate_, message),
      v8::Global<v8::Value>(isolate_, exception)});
}

// Adoped from V8 d8 utility code
void V8Runtime::RemoveUnhandledPromise(v8::Local<v8::Promise> promise) {
  if (ignore_unhandled_promises_)
    return;
  // Remove handled promises from the list
  DCHECK_EQ(promise->GetIsolate(), isolate_);
  if (last_unhandled_promise_ && last_unhandled_promise_->promise.Get(isolate_) == promise) {
    last_unhandled_promise_.reset();
  }
}

napi_status V8Runtime::NapiGetUniqueUtf8StringRef(napi_env env, const char *str, size_t length, napi_ext_ref *result) {
  if (length == NAPI_AUTO_LENGTH) {
    length = std::char_traits<char>::length(str);
  }

  napi_ext_ref ref{};
  auto it = unique_strings_.find({str, length});
  if (it != unique_strings_.end()) {
    ref = it->second->GetRef();
    STATUS_CALL(napi_ext_reference_ref(env, ref));
  }

  if (!ref) {
    auto uniqueString = std::make_unique<NapiUniqueString>(env, std::string(str, length));
    auto isolate = env->isolate;
    auto str_maybe = v8::String::NewFromUtf8(isolate, str, v8::NewStringType::kInternalized, static_cast<int>(length));
    CHECK_MAYBE_EMPTY(env, str_maybe, napi_generic_failure);

    napi_value nstr = v8impl::JsValueFromV8LocalValue(str_maybe.ToLocalChecked());
    auto finalize = [](napi_env env, void *finalize_data, void *finalize_hint) {
      NapiUniqueString *uniqueString = static_cast<NapiUniqueString *>(finalize_data);
      V8Runtime *runtime = static_cast<V8Runtime *>(finalize_hint);
      auto it = runtime->unique_strings_.find(uniqueString->GetView());
      if (it != runtime->unique_strings_.end()) {
        runtime->unique_strings_.erase(it);
      }
    };
    STATUS_CALL(napi_ext_create_reference_with_data(env, nstr, uniqueString.get(), finalize, this, &ref));
    uniqueString->SetRef(ref);
    unique_strings_[uniqueString->GetView()] = std::move(uniqueString);
  }

  *result = ref;
  return napi_clear_last_error(env);
}

bool V8Runtime::IsEnvDeleted() noexcept {
  return is_env_deleted_;
}

void V8Runtime::SetIsEnvDeleted() noexcept {
  is_env_deleted_ = true;
}

//=============================================================================
// StringView implementation
//=============================================================================

constexpr StringView::StringView(const char *data, size_t size) noexcept : m_data{data}, m_size{size} {}

StringView::StringView(const std::string &str) noexcept : m_data{str.data()}, m_size{str.size()} {}

constexpr const char *StringView::begin() const noexcept {
  return m_data;
}

constexpr const char *StringView::end() const noexcept {
  return m_data + m_size;
}

constexpr const char &StringView::operator[](size_t pos) const noexcept {
  return *(m_data + pos);
}

constexpr const char *StringView::data() const noexcept {
  return m_data;
}

constexpr size_t StringView::size() const noexcept {
  return m_size;
}

constexpr bool StringView::empty() const noexcept {
  return m_size == 0;
}

void StringView::swap(StringView &other) noexcept {
  using std::swap;
  swap(m_data, other.m_data);
  swap(m_size, other.m_size);
}

int StringView::compare(StringView other) const noexcept {
  size_t minCommonSize = (std::min)(m_size, other.m_size);
  int result = std::char_traits<char>::compare(m_data, other.m_data, minCommonSize);
  if (result == 0) {
    if (m_size < other.m_size) {
      result = -1;
    } else if (m_size > other.m_size) {
      result = 1;
    }
  }
  return result;
}

void swap(StringView &left, StringView &right) noexcept {
  left.swap(right);
}

bool operator==(StringView left, StringView right) noexcept {
  return left.compare(right) == 0;
}

bool operator!=(StringView left, StringView right) noexcept {
  return left.compare(right) != 0;
}

bool operator<(StringView left, StringView right) noexcept {
  return left.compare(right) < 0;
}

bool operator<=(StringView left, StringView right) noexcept {
  return left.compare(right) <= 0;
}

bool operator>(StringView left, StringView right) noexcept {
  return left.compare(right) > 0;
}

bool operator>=(StringView left, StringView right) noexcept {
  return left.compare(right) >= 0;
}

constexpr StringView operator"" _sv(const char *str, std::size_t len) noexcept {
  return StringView(str, len);
}

//=============================================================================
// StringViewHash implementation
//=============================================================================

size_t StringViewHash::operator()(StringView view) const noexcept {
  return s_classic_collate.hash(view.begin(), view.end());
}

/*static*/ const std::collate<char> &StringViewHash::s_classic_collate =
    std::use_facet<std::collate<char>>(std::locale::classic());

//=============================================================================
// NapiUniqueString implementation
//=============================================================================

NapiUniqueString::NapiUniqueString(napi_env env, std::string value) noexcept : env_{env}, value_{std::move(value)} {}

NapiUniqueString::~NapiUniqueString() noexcept {}

StringView NapiUniqueString::GetView() const noexcept {
  return StringView{value_};
}

napi_ext_ref NapiUniqueString::GetRef() const noexcept {
  return string_ref_;
}

void NapiUniqueString::SetRef(napi_ext_ref ref) noexcept {
  string_ref_ = ref;
}

//=============================================================================
// Make V8 JSI runtime
//=============================================================================

std::unique_ptr<jsi::Runtime> makeV8Runtime(V8RuntimeArgs &&args) {
  return std::make_unique<V8Runtime>(std::move(args));
}

std::unique_ptr<jsi::Runtime> makeV8Runtime() {
  return std::make_unique<V8Runtime>(V8RuntimeArgs());
}

} // namespace v8runtime
