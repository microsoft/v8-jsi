// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "V8JsiRuntime_impl.h"

#include "libplatform/libplatform.h"
#include "v8.h"

#include "V8Platform.h"
#include "public/ScriptStore.h"

#include <atomic>
#include <cstdlib>
#include <list>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include "etw/tracing.h"
#endif

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
  IdleTaskAdapter(std::unique_ptr<v8::IdleTask> &&task)
      : task_(std::move(task)) {}

  void run(double deadline_in_seconds) override {
    task_->Run(deadline_in_seconds);
  }

 private:
  std::unique_ptr<v8::IdleTask> task_;
};

class TaskRunnerAdapter : public v8::TaskRunner {
 public:
  TaskRunnerAdapter(std::unique_ptr<v8runtime::JSITaskRunner> &&taskRunner)
      : taskRunner_(std::move(taskRunner)) {}

  void PostTask(std::unique_ptr<v8::Task> task) override {
    taskRunner_->postTask(std::make_unique<TaskAdapter>(std::move(task)));
  }

  void PostDelayedTask(std::unique_ptr<v8::Task> task, double delay_in_seconds)
      override {
    taskRunner_->postDelayedTask(
        std::make_unique<TaskAdapter>(std::move(task)), delay_in_seconds);
  }

  bool IdleTasksEnabled() override {
    return taskRunner_->IdleTasksEnabled();
  }

  void PostIdleTask(std::unique_ptr<v8::IdleTask> task) override {
    taskRunner_->postIdleTask(
        std::make_unique<IdleTaskAdapter>(std::move(task)));
  }

  void PostNonNestableTask(std::unique_ptr<v8::Task> task) override {
    //TODO: non-nestable
    taskRunner_->postTask(std::make_unique<TaskAdapter>(std::move(task)));
  }

  bool NonNestableTasksEnabled() const override {
    return true;
  }

 private:
  std::unique_ptr<v8runtime::JSITaskRunner> taskRunner_;
};

// String utilities
namespace {
std::string JSStringToSTLString(
    v8::Isolate *isolate,
    v8::Local<v8::String> string) {
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

void V8Runtime::AddHostObjectLifetimeTracker(
    std::shared_ptr<HostObjectLifetimeTracker> hostObjectLifetimeTracker) {
  // Note that we are letting the list grow in definitely as of now.. The list
  // gets cleaned up when the runtime is teared down.
  // TODO :: We should remove entries from the list as the objects are garbage
  // collected.
  host_object_lifetime_tracker_list_.push_back(hostObjectLifetimeTracker);
}

/*static */ void V8Runtime::OnMessage(
    v8::Local<v8::Message> message,
    v8::Local<v8::Value> error) {
  v8::Isolate *isolate = v8::Isolate::GetCurrent();

  v8::String::Utf8Value msg(isolate, message->Get());
  v8::String::Utf8Value source_line(
      isolate,
      message->GetSourceLine(isolate->GetCurrentContext()).ToLocalChecked());

#ifdef _WIN32
  EventWriteMESSAGE(
      *msg,
      *source_line,
      "",
      message->GetLineNumber(isolate->GetCurrentContext()).ToChecked(),
      message->GetStartPosition(),
      message->GetEndPosition(),
      message->ErrorLevel(),
      message->GetStartColumn(),
      message->GetEndColumn());
#endif
}

size_t V8Runtime::NearHeapLimitCallback(
    void *raw_state,
    size_t current_heap_limit,
    size_t initial_heap_limit) {
#ifdef _WIN32
  EventWriteV8JSI_LOG(
      "NearHeapLimitCallback",
      std::to_string(current_heap_limit).c_str(),
      std::to_string(initial_heap_limit).c_str(),
      "");
#endif
  // Add 5MB.
  return current_heap_limit + 5 * 1024 * 1024;
}

static std::string GCTypeToString(
    std::string &prefix,
    v8::GCType type,
    v8::GCCallbackFlags gcflags) {
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
    case v8::GCCallbackFlags::
        kGCCallbackFlagSynchronousPhantomCallbackProcessing:
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

void V8Runtime::GCPrologueCallback(
    v8::Isolate *isolate,
    v8::GCType type,
    v8::GCCallbackFlags flags) {
  std::string prefix("GCPrologue");
  DumpCounters(GCTypeToString(prefix, type, flags).c_str());
}

void V8Runtime::GCEpilogueCallback(
    v8::Isolate *isolate,
    v8::GCType type,
    v8::GCCallbackFlags flags) {
  std::string prefix("GCEpilogue");
  DumpCounters(GCTypeToString(prefix, type, flags).c_str());
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
    if (element.second->count() > 0)
      EventWriteDUMPT_COUNTERS(
          when,
          cookie,
          element.first.c_str(),
          element.second->count(),
          element.second->sample_total(),
          element.second->is_histogram());
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
  Counter *counter =
      map_entry != counter_map_->end() ? map_entry->second : nullptr;

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

void *
V8Runtime::CreateHistogram(const char *name, int min, int max, size_t buckets) {
  return GetCounter(name, true);
}

void V8Runtime::AddHistogramSample(void *histogram, int sample) {
  Counter *counter = reinterpret_cast<Counter *>(histogram);
  counter->AddSample(sample);
}

// This class is used to record the JITted code position info for JIT
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
      EventWriteJIT_CODE_EVENT(
          event->type,
          event->code_type,
          std::string(event->name.str, event->name.len).c_str(),
          "");
#endif
      break;

    case v8::JitCodeEvent::CODE_ADD_LINE_POS_INFO: {
      JITCodeLineInfo *line_info =
          reinterpret_cast<JITCodeLineInfo *>(event->user_data);
      if (line_info != NULL) {
        line_info->SetPosition(
            static_cast<intptr_t>(event->line_info.offset),
            static_cast<int>(event->line_info.pos));
      }
      break;
    }
    case v8::JitCodeEvent::CODE_START_LINE_INFO_RECORDING: {
      v8::JitCodeEvent *temp_event = const_cast<v8::JitCodeEvent *>(event);
      temp_event->user_data = new JITCodeLineInfo();
      break;
    }
    case v8::JitCodeEvent::CODE_END_LINE_INFO_RECORDING: {
      JITCodeLineInfo *line_info =
          reinterpret_cast<JITCodeLineInfo *>(event->user_data);

      std::list<JITCodeLineInfo::LineNumInfo> *lineinfos =
          line_info->GetLineNumInfo();

      std::string code_details;

      std::list<JITCodeLineInfo::LineNumInfo>::iterator iter;
      for (iter = lineinfos->begin(); iter != lineinfos->end(); iter++) {
        code_details.append(std::to_string(iter->pc_) + ":");
        code_details.append(std::to_string(iter->pos_) + ":");
      }
#ifdef _WIN32
      EventWriteJIT_CODE_EVENT(
          event->type, event->code_type, "###", code_details.c_str());
#endif

      break;
    }
    default:
#ifdef _WIN32
      EventWriteJIT_CODE_EVENT(event->type, event->code_type, "~~~", "");
#endif
      break;
  }
}

v8::Isolate *V8Runtime::CreateNewIsolate() {
  if (args_.custom_snapshot_blob) {
    custom_snapshot_startup_data_ = {
        reinterpret_cast<const char *>(args_.custom_snapshot_blob->data()),
        static_cast<int>(args_.custom_snapshot_blob->size())};
    create_params_.snapshot_blob = &custom_snapshot_startup_data_;
  }

  // One per each runtime.
  create_params_.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();

  if (args_.initial_heap_size_in_bytes > 0 ||
      args_.maximum_heap_size_in_bytes > 0) {
    v8::ResourceConstraints constraints;
    constraints.ConfigureDefaultsFromHeapSize(
        args_.initial_heap_size_in_bytes, args_.maximum_heap_size_in_bytes);

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

  foreground_task_runner_ = std::make_shared<TaskRunnerAdapter>(
      std::move(args_.foreground_task_runner));
  isolate_->SetData(
      v8runtime::ISOLATE_DATA_SLOT,
      new v8runtime::IsolateData({foreground_task_runner_}));

  v8::Isolate::Initialize(isolate_, create_params_);

  if (args_.trackGCObjectStats) {
    MapCounters(isolate_, "v8jsi");
  }

  if (args_.enableJitTracing) {
    isolate_->SetJitCodeEventHandler(
        v8::kJitCodeEventDefault, JitCodeEventListener);
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
  isolate_->SetAbortOnUncaughtExceptionCallback(
      [](v8::Isolate *) { return true; });

  isolate_->Enter();

  return isolate_;
}

void V8Runtime::createHostObjectConstructorPerContext() {
  // Create and keep the constuctor for creating Host objects.
  v8::Local<v8::FunctionTemplate> constructorForHostObjectTemplate =
      v8::FunctionTemplate::New(isolate_);
  v8::Local<v8::ObjectTemplate> hostObjectTemplate =
      constructorForHostObjectTemplate->InstanceTemplate();
  hostObjectTemplate->SetHandler(v8::NamedPropertyHandlerConfiguration(
      HostObjectProxy::Get,
      HostObjectProxy::Set,
      nullptr,
      nullptr,
      HostObjectProxy::Enumerator));
  hostObjectTemplate->SetInternalFieldCount(1);
  host_object_constructor_.Reset(
      isolate_,
      constructorForHostObjectTemplate->GetFunction(context_.Get(isolate_))
          .ToLocalChecked());
}

void V8Runtime::initializeTracing() {
#ifdef _WIN32
  EventRegisterv8jsi_Provider();
#endif
}

void V8Runtime::initializeV8() {
  std::vector<const char *> argv;
  argv.push_back("v8jsi");

  if (args_.liteMode)
    argv.push_back("--lite-mode");

  if (args_.trackGCObjectStats)
    argv.push_back("--track_gc_object_stats");

  int argc = static_cast<int>(argv.size());
  v8::V8::SetFlagsFromCommandLine(&argc, const_cast<char **>(&argv[0]), false);

  // Assuming Initialize can be called multiple times in process.
  v8::V8::Initialize();
}

V8Runtime::V8Runtime(V8RuntimeArgs &&args) : args_(std::move(args)) {
  initializeTracing();
  initializeV8();

  // Try to reuse the already existing isolate in this thread.
  if (tls_isolate_usage_counter_++ > 0) {
    isolate_ = v8::Isolate::GetCurrent();
  } else {
    CreateNewIsolate();
  }

  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handleScope(isolate_);
  context_.Reset(GetIsolate(), CreateContext(isolate_));

  v8::Context::Scope context_scope(context_.Get(GetIsolate()));

#ifdef _WIN32
  if (args_.enableInspector) {
    inspector_agent_ = std::make_unique<inspector::Agent>(
        platform_holder_.Get(),
        isolate_,
        context_.Get(GetIsolate()),
        "JSIRuntime context",
        args_.inspectorPort);
    inspector_agent_->start();

    if (args_.waitForDebugger) {
      inspector_agent_->waitForDebugger();
    }
  }
#endif

  createHostObjectConstructorPerContext();
}

V8Runtime::~V8Runtime() {
#ifdef _WIN32
  if (inspector_agent_ && inspector_agent_->IsStarted()) {
    inspector_agent_->stop();
  }
#endif

  host_object_constructor_.Reset();
  context_.Reset();

  for (std::shared_ptr<HostObjectLifetimeTracker> hostObjectLifetimeTracker :
       host_object_lifetime_tracker_list_) {
    hostObjectLifetimeTracker->ResetHostObject(false /*isGC*/);
  }

  if (--tls_isolate_usage_counter_ == 0) {
    isolate_->SetData(v8runtime::ISOLATE_DATA_SLOT, nullptr);

    isolate_->Exit();
    isolate_->Dispose();

    delete create_params_.array_buffer_allocator;
  }

  // Note :: We never dispose V8 here. Is it required ?
}

jsi::Value V8Runtime::evaluateJavaScript(
    const std::shared_ptr<const jsi::Buffer> &buffer,
    const std::string &sourceURL) {
  _ISOLATE_CONTEXT_ENTER

  v8::Local<v8::String> sourceV8String;
  // If we'd somehow know the buffer includes only ASCII characters, we could use External strings to avoid the copy
  /* ExternalOwningOneByteStringResource *external_string_resource = new ExternalOwningOneByteStringResource(buffer);
  if (!v8::String::NewExternalOneByte(isolate, external_string_resource).ToLocal(&sourceV8String)) {
    std::abort();
  }
  delete external_string_resource; */

  if (!v8::String::NewFromUtf8(isolate, reinterpret_cast<const char *>(buffer->data()),
      v8::NewStringType::kNormal, static_cast<int>(buffer->size())).ToLocal(&sourceV8String)) {
    std::abort();
  }

  jsi::Value result = ExecuteString(sourceV8String, sourceURL);
  return result;
}

v8::Local<v8::Script> V8Runtime::GetCompiledScript(
    const v8::Local<v8::String> &source,
    const std::string &sourceURL) {
  v8::Isolate *isolate = GetIsolate();
  v8::TryCatch try_catch(isolate);
  v8::MaybeLocal<v8::String> name = v8::String::NewFromUtf8(
      isolate, reinterpret_cast<const char *>(sourceURL.c_str()));
  v8::ScriptOrigin origin(name.ToLocalChecked());
  v8::Local<v8::Context> context(isolate->GetCurrentContext());

  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(context, source, &origin).ToLocal(&script)) {
    ReportException(&try_catch);
  }
  return script;
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
  v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);

  global->Set(
      v8::String::NewFromUtf8(isolate, "print", v8::NewStringType::kNormal)
          .ToLocalChecked(),
      v8::FunctionTemplate::New(isolate, Print));

  v8::Local<v8::Context> context = v8::Context::New(isolate, NULL, global);
  context->SetAlignedPointerInEmbedderData(1, this);
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

  ByteArrayBuffer(const uint8_t *data, int length)
      : data_(data), length_(length) {}

 private:
  const uint8_t *data_;
  int length_;
};

jsi::Value V8Runtime::ExecuteString(
    const v8::Local<v8::String> &source,
    const std::string &sourceURL) {
  // jsi::Value V8Runtime::ExecuteString(v8::Local<v8::String> source, const
  // jsi::Buffer* cache, v8::Local<v8::Value> name, bool report_exceptions) {
  _ISOLATE_CONTEXT_ENTER
  v8::TryCatch try_catch(isolate);

  v8::Local<v8::String> urlV8String =
      v8::String::NewFromUtf8(
          isolate, reinterpret_cast<const char *>(sourceURL.c_str()))
          .ToLocalChecked();
  v8::ScriptOrigin origin(urlV8String);
  v8::Local<v8::Context> context(isolate->GetCurrentContext());
  v8::Local<v8::Script> script;

  v8::ScriptCompiler::CompileOptions options =
      v8::ScriptCompiler::CompileOptions::kNoCompileOptions;
  v8::ScriptCompiler::CachedData *cached_data = nullptr;

  std::shared_ptr<const jsi::Buffer> cache;
  if (args_.preparedScriptStore) {
    jsi::ScriptSignature scriptSignature = {sourceURL, 1};
    jsi::JSRuntimeSignature runtimeSignature = {"V8", 76};
    cache = args_.preparedScriptStore->tryGetPreparedScript(
        scriptSignature, runtimeSignature, "perf");
  }

  if (cache) {
    cached_data = new v8::ScriptCompiler::CachedData(
        cache->data(), static_cast<int>(cache->size()));
    options = v8::ScriptCompiler::CompileOptions::kConsumeCodeCache;
  } else {
    // Eager compile so that we will write it to disk.
    // options = v8::ScriptCompiler::CompileOptions::kEagerCompile;
    options = v8::ScriptCompiler::CompileOptions::kNoCompileOptions;
  }

  v8::ScriptCompiler::Source script_source(source, origin, cached_data);

  if (!v8::ScriptCompiler::Compile(context, &script_source, options)
           .ToLocal(&script)) {
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

      if (args_.preparedScriptStore &&
          options != v8::ScriptCompiler::CompileOptions::kConsumeCodeCache) {
        v8::ScriptCompiler::CachedData *codeCache =
            v8::ScriptCompiler::CreateCodeCache(script->GetUnboundScript());

        jsi::ScriptSignature scriptSignature = {sourceURL, 1};
        jsi::JSRuntimeSignature runtimeSignature = {"V8", 76};

        args_.preparedScriptStore->persistPreparedScript(
            std::make_shared<ByteArrayBuffer>(
                codeCache->data, codeCache->length),
            scriptSignature,
            runtimeSignature,
            "perf");
      }

      return createValue(result);
    }
  }
}

std::shared_ptr<const facebook::jsi::PreparedJavaScript>
V8Runtime::prepareJavaScript(
    const std::shared_ptr<const facebook::jsi::Buffer> &,
    std::string) {
  throw jsi::JSINativeException(
      "V8Runtime::prepareJavaScript is not implemented!");
}

facebook::jsi::Value V8Runtime::evaluatePreparedJavaScript(
    const std::shared_ptr<const facebook::jsi::PreparedJavaScript> &) {
  throw jsi::JSINativeException(
      "V8Runtime::evaluatePreparedJavaScript is not implemented!");
}

void V8Runtime::ReportException(v8::TryCatch *try_catch) {
  _ISOLATE_CONTEXT_ENTER
  v8::String::Utf8Value exception(isolate, try_catch->Exception());
  const char *exception_string = ToCString(exception);
  v8::Local<v8::Message> message = try_catch->Message();
  if (message.IsEmpty()) {
    // V8 didn't provide any extra information about this error; just
    // throw the exception.
    throw jsi::JSError(*this, "<Unknown exception>");
  } else {
    // Print (filename):(line number): (message).

    std::stringstream sstr;

    v8::String::Utf8Value filename(
        isolate, message->GetScriptOrigin().ResourceName());
    v8::Local<v8::Context> context(isolate->GetCurrentContext());
    const char *filename_string = ToCString(filename);
    int linenum = message->GetLineNumber(context).FromJust();
    sstr << filename_string << ":" << linenum << ": " << exception_string
         << std::endl;

    // Print line of source code.
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
    sstr << std::endl;

    v8::Local<v8::Value> stack_trace_string;
    if (try_catch->StackTrace(context).ToLocal(&stack_trace_string) &&
        stack_trace_string->IsString() &&
        v8::Local<v8::String>::Cast(stack_trace_string)->Length() > 0) {
      v8::String::Utf8Value stack_trace(isolate, stack_trace_string);
      const char *stack_trace_string2 = ToCString(stack_trace);
      sstr << stack_trace_string2 << std::endl;
    }

    throw jsi::JSError(*this, sstr.str());
  }
}

jsi::Object V8Runtime::global() {
  _ISOLATE_CONTEXT_ENTER
  return createObject(context_.Get(isolate)->Global());
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

V8Runtime::V8StringValue::V8StringValue(v8::Local<v8::String> str)
    : v8String_(v8::Isolate::GetCurrent(), str) {}

void V8Runtime::V8StringValue::invalidate() {
  delete this;
}

V8Runtime::V8StringValue::~V8StringValue() {
  v8String_.Reset();
}

V8Runtime::V8ObjectValue::V8ObjectValue(v8::Local<v8::Object> obj)
    : v8Object_(v8::Isolate::GetCurrent(), obj) {}

void V8Runtime::V8ObjectValue::invalidate() {
  delete this;
}

V8Runtime::V8ObjectValue::~V8ObjectValue() {
  v8Object_.Reset();
}

// Shallow clone
jsi::Runtime::PointerValue *V8Runtime::cloneString(
    const jsi::Runtime::PointerValue *pv) {
  if (!pv) {
    return nullptr;
  }

  _ISOLATE_CONTEXT_ENTER
  const V8StringValue *string = static_cast<const V8StringValue *>(pv);
  return makeStringValue(string->v8String_.Get(GetIsolate()));
}

jsi::Runtime::PointerValue *V8Runtime::cloneObject(
    const jsi::Runtime::PointerValue *pv) {
  if (!pv) {
    return nullptr;
  }

  _ISOLATE_CONTEXT_ENTER
  const V8ObjectValue *object = static_cast<const V8ObjectValue *>(pv);
  return makeObjectValue(object->v8Object_.Get(GetIsolate()));
}

jsi::Runtime::PointerValue *V8Runtime::clonePropNameID(
    const jsi::Runtime::PointerValue *pv) {
  if (!pv) {
    return nullptr;
  }

  _ISOLATE_CONTEXT_ENTER
  const V8StringValue *string = static_cast<const V8StringValue *>(pv);
  return makeStringValue(string->v8String_.Get(GetIsolate()));
}

jsi::Runtime::PointerValue *V8Runtime::cloneSymbol(
    const jsi::Runtime::PointerValue *) {
  throw jsi::JSINativeException("V8Runtime::cloneSymbol is not implemented!");
}

std::string V8Runtime::symbolToString(const jsi::Symbol &) {
  throw jsi::JSINativeException(
      "V8Runtime::symbolToString is not implemented!");
}

jsi::PropNameID V8Runtime::createPropNameIDFromAscii(
    const char *str,
    size_t length) {
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

  auto res = createPropNameID(v8String);
  return res;
}

jsi::PropNameID V8Runtime::createPropNameIDFromUtf8(
    const uint8_t *utf8,
    size_t length) {
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::String> v8String;
  if (!v8::String::NewFromUtf8(
           GetIsolate(),
           reinterpret_cast<const char *>(utf8),
           v8::NewStringType::kNormal,
           static_cast<int>(length))
           .ToLocal(&v8String)) {
    std::stringstream strstream;
    strstream << "Unable to create property id: " << utf8;
    throw jsi::JSError(*this, strstream.str());
  }

  auto res = createPropNameID(v8String);
  return res;
}

jsi::PropNameID V8Runtime::createPropNameIDFromString(const jsi::String &str) {
  _ISOLATE_CONTEXT_ENTER
  return createPropNameID(stringRef(str));
}

std::string V8Runtime::utf8(const jsi::PropNameID &sym) {
  _ISOLATE_CONTEXT_ENTER
  return JSStringToSTLString(
      GetIsolate(), v8::Local<v8::String>::Cast(valueRef(sym)));
}

bool V8Runtime::compare(const jsi::PropNameID &a, const jsi::PropNameID &b) {
  _ISOLATE_CONTEXT_ENTER
  return valueRef(a)
      ->Equals(GetIsolate()->GetCurrentContext(), valueRef(b))
      .ToChecked();
}

jsi::String V8Runtime::createStringFromAscii(const char *str, size_t length) {
  return this->createStringFromUtf8(
      reinterpret_cast<const uint8_t *>(str), length);
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

  jsi::String jsistr = createString(v8string);
  return jsistr;
}

std::string V8Runtime::utf8(const jsi::String &str) {
  _ISOLATE_CONTEXT_ENTER
  return JSStringToSTLString(GetIsolate(), stringRef(str));
}

jsi::Object V8Runtime::createObject() {
  _ISOLATE_CONTEXT_ENTER
  return createObject(v8::Object::New(GetIsolate()));
}

jsi::Object V8Runtime::createObject(
    std::shared_ptr<jsi::HostObject> hostobject) {
  _ISOLATE_CONTEXT_ENTER
  HostObjectProxy *hostObjectProxy = new HostObjectProxy(*this, hostobject);
  v8::Local<v8::Object> newObject;
  if (!host_object_constructor_.Get(isolate_)
           ->NewInstance(isolate_->GetCurrentContext())
           .ToLocal(&newObject)) {
    throw jsi::JSError(*this, "HostObject construction failed!!");
  }

  newObject->SetInternalField(
      0,
      v8::Local<v8::External>::New(
          GetIsolate(), v8::External::New(GetIsolate(), hostObjectProxy)));

  AddHostObjectLifetimeTracker(std::make_shared<HostObjectLifetimeTracker>(
      *this, newObject, hostObjectProxy));

  return createObject(newObject);
}

std::shared_ptr<jsi::HostObject> V8Runtime::getHostObject(
    const jsi::Object &obj) {
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::External> internalField =
      v8::Local<v8::External>::Cast(objectRef(obj)->GetInternalField(0));
  HostObjectProxy *hostObjectProxy =
      reinterpret_cast<HostObjectProxy *>(internalField->Value());
  return hostObjectProxy->getHostObject();
}

jsi::Value V8Runtime::getProperty(
    const jsi::Object &obj,
    const jsi::String &name) {
  _ISOLATE_CONTEXT_ENTER
  return createValue(objectRef(obj)
                         ->Get(context_.Get(isolate), stringRef(name))
                         .ToLocalChecked());
}

jsi::Value V8Runtime::getProperty(
    const jsi::Object &obj,
    const jsi::PropNameID &name) {
  _ISOLATE_CONTEXT_ENTER
  v8::MaybeLocal<v8::Value> result =
      objectRef(obj)->Get(isolate_->GetCurrentContext(), valueRef(name));
  if (result.IsEmpty())
    throw jsi::JSError(*this, "V8Runtime::getProperty failed.");
  return createValue(result.ToLocalChecked());
}

bool V8Runtime::hasProperty(const jsi::Object &obj, const jsi::String &name) {
  _ISOLATE_CONTEXT_ENTER
  v8::Maybe<bool> result =
      objectRef(obj)->Has(isolate_->GetCurrentContext(), stringRef(name));
  if (result.IsNothing())
    throw jsi::JSError(*this, "V8Runtime::setPropertyValue failed.");
  return result.FromJust();
}

bool V8Runtime::hasProperty(
    const jsi::Object &obj,
    const jsi::PropNameID &name) {
  _ISOLATE_CONTEXT_ENTER
  v8::Maybe<bool> result =
      objectRef(obj)->Has(isolate_->GetCurrentContext(), valueRef(name));
  if (result.IsNothing())
    throw jsi::JSError(*this, "V8Runtime::setPropertyValue failed.");
  return result.FromJust();
}

void V8Runtime::setPropertyValue(
    jsi::Object &object,
    const jsi::PropNameID &name,
    const jsi::Value &value) {
  _ISOLATE_CONTEXT_ENTER
  v8::Maybe<bool> result = objectRef(object)->Set(
      isolate_->GetCurrentContext(), valueRef(name), valueRef(value));
  if (!result.FromMaybe(false))
    throw jsi::JSError(*this, "V8Runtime::setPropertyValue failed.");
}

void V8Runtime::setPropertyValue(
    jsi::Object &object,
    const jsi::String &name,
    const jsi::Value &value) {
  _ISOLATE_CONTEXT_ENTER
  v8::Maybe<bool> result = objectRef(object)->Set(
      isolate_->GetCurrentContext(), stringRef(name), valueRef(value));
  if (!result.FromMaybe(false))
    throw jsi::JSError(*this, "V8Runtime::setPropertyValue failed.");
}

bool V8Runtime::isArray(const jsi::Object &obj) const {
  _ISOLATE_CONTEXT_ENTER
  return objectRef(obj)->IsArray();
}

bool V8Runtime::isArrayBuffer(const jsi::Object & /*obj*/) const {
  throw std::runtime_error("Unsupported");
}

uint8_t *V8Runtime::data(const jsi::ArrayBuffer &obj) {
  throw std::runtime_error("Unsupported");
}

size_t V8Runtime::size(const jsi::ArrayBuffer & /*obj*/) {
  throw std::runtime_error("Unsupported");
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

  for (const std::shared_ptr<HostObjectLifetimeTracker>& hostObjectLifetimeTracker :
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
  v8::Local<v8::Array> propNames =
      objectRef(obj)->GetPropertyNames(context_.Get(isolate_)).ToLocalChecked();
  return createObject(propNames).getArray(*this);
}

jsi::WeakObject V8Runtime::createWeakObject(const jsi::Object &) {
  throw std::logic_error("Not implemented");
}

jsi::Value V8Runtime::lockWeakObject(const jsi::WeakObject &) {
  throw std::logic_error("Not implemented");
}

jsi::Array V8Runtime::createArray(size_t length) {
  _ISOLATE_CONTEXT_ENTER
  return createObject(v8::Array::New(GetIsolate(), static_cast<int>(length)))
      .getArray(*this);
}

size_t V8Runtime::size(const jsi::Array &arr) {
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::Array> array = v8::Local<v8::Array>::Cast(objectRef(arr));
  return array->Length();
}

jsi::Value V8Runtime::getValueAtIndex(const jsi::Array &arr, size_t i) {
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::Array> array = v8::Local<v8::Array>::Cast(objectRef(arr));
  return createValue(array->Get(context_.Get(isolate), static_cast<uint32_t>(i))
                         .ToLocalChecked());
}

void V8Runtime::setValueAtIndexImpl(
    jsi::Array &arr,
    size_t i,
    const jsi::Value &value) {
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
           v8::Local<v8::External>::New(
               GetIsolate(),
               v8::External::New(GetIsolate(), hostFunctionProxy)))
           .ToLocal(&newFunction)) {
    throw jsi::JSError(*this, "Creation of HostFunction failed.");
  }

  AddHostObjectLifetimeTracker(std::make_shared<HostObjectLifetimeTracker>(
      *this, newFunction, hostFunctionProxy));

  return createObject(newFunction).getFunction(*this);
}

bool V8Runtime::isHostFunction(const jsi::Function &obj) const {
  std::abort();
  return false;
}

jsi::HostFunctionType &V8Runtime::getHostFunction(const jsi::Function &obj) {
  std::abort();
}

jsi::Value V8Runtime::call(
    const jsi::Function &jsiFunc,
    const jsi::Value &jsThis,
    const jsi::Value *args,
    size_t count) {
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::Function> func =
      v8::Local<v8::Function>::Cast(objectRef(jsiFunc));
  std::vector<v8::Local<v8::Value>> argv;
  for (size_t i = 0; i < count; i++) {
    argv.push_back(valueRef(args[i]));
  }

  v8::TryCatch trycatch(isolate_);
  v8::MaybeLocal<v8::Value> result = func->Call(
      isolate_->GetCurrentContext(),
      valueRef(jsThis),
      static_cast<int>(count),
      argv.data());

  if (trycatch.HasCaught()) {
    ReportException(&trycatch);
  }

  // Call can return
  if (result.IsEmpty()) {
    return createValue(v8::Undefined(GetIsolate()));
  } else {
    return createValue(result.ToLocalChecked());
  }
}

jsi::Value V8Runtime::callAsConstructor(
    const jsi::Function &jsiFunc,
    const jsi::Value *args,
    size_t count) {
  _ISOLATE_CONTEXT_ENTER
  v8::Local<v8::Function> func =
      v8::Local<v8::Function>::Cast(objectRef(jsiFunc));
  std::vector<v8::Local<v8::Value>> argv;
  for (size_t i = 0; i < count; i++) {
    argv.push_back(valueRef(args[i]));
  }

  v8::TryCatch trycatch(isolate_);
  v8::Local<v8::Object> newObject;
  if (!func->NewInstance(
               GetIsolate()->GetCurrentContext(),
               static_cast<int>(count),
               argv.data())
           .ToLocal(&newObject)) {
    new jsi::JSError(*this, "Object construction failed!!");
  }

  if (trycatch.HasCaught()) {
    ReportException(&trycatch);
  }

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

bool V8Runtime::strictEquals(const jsi::Symbol &, const jsi::Symbol &) const {
  throw jsi::JSINativeException("Not implemented!");
}

bool V8Runtime::instanceOf(const jsi::Object &o, const jsi::Function &f) {
  _ISOLATE_CONTEXT_ENTER
  return objectRef(o)
      ->InstanceOf(GetIsolate()->GetCurrentContext(), objectRef(f))
      .ToChecked();
}

jsi::Runtime::PointerValue *V8Runtime::makeStringValue(
    v8::Local<v8::String> string) const {
  return new V8StringValue(string);
}

jsi::String V8Runtime::createString(v8::Local<v8::String> str) const {
  return make<jsi::String>(makeStringValue(str));
}

jsi::PropNameID V8Runtime::createPropNameID(v8::Local<v8::Value> str) {
  _ISOLATE_CONTEXT_ENTER
  return make<jsi::PropNameID>(
      makeStringValue(v8::Local<v8::String>::Cast(str)));
}

jsi::Runtime::PointerValue *V8Runtime::makeObjectValue(
    v8::Local<v8::Object> objectRef) const {
  _ISOLATE_CONTEXT_ENTER
  return new V8ObjectValue(objectRef);
}

jsi::Object V8Runtime::createObject(v8::Local<v8::Object> obj) const {
  _ISOLATE_CONTEXT_ENTER
  return make<jsi::Object>(makeObjectValue(obj));
}

jsi::Value V8Runtime::createValue(v8::Local<v8::Value> value) const {
  _ISOLATE_CONTEXT_ENTER
  if (value->IsInt32()) {
    return jsi::Value(
        value->Int32Value(GetIsolate()->GetCurrentContext()).ToChecked());
  }
  if (value->IsNumber()) {
    return jsi::Value(
        value->NumberValue(GetIsolate()->GetCurrentContext()).ToChecked());
  } else if (value->IsBoolean()) {
    return jsi::Value(value->BooleanValue(GetIsolate()));
  } else if (value->IsUndefined()) {
    return jsi::Value();
  } else if (value.IsEmpty() || value->IsNull()) {
    return jsi::Value(nullptr);
  }

  else if (value->IsString()) {
    // Note :: Non copy create
    return createString(v8::Local<v8::String>::Cast(value));
  } else if (value->IsObject()) {
    return createObject(v8::Local<v8::Object>::Cast(value));
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
    return handle_scope.Escape(
        v8::Number::New(GetIsolate(), value.getNumber()));
  } else if (value.isString()) {
    return handle_scope.Escape(stringRef(value.asString(*this)));
  } else if (value.isObject()) {
    return handle_scope.Escape(objectRef(value.getObject(*this)));
  } else {
    // What are you?
    std::abort();
  }
}

v8::Local<v8::String> V8Runtime::stringRef(const jsi::String &str) {
  v8::EscapableHandleScope handle_scope(v8::Isolate::GetCurrent());
  const V8StringValue *v8StringValue =
      static_cast<const V8StringValue *>(getPointerValue(str));
  return handle_scope.Escape(
      v8StringValue->v8String_.Get(v8::Isolate::GetCurrent()));
}

v8::Local<v8::Value> V8Runtime::valueRef(const jsi::PropNameID &sym) {
  v8::EscapableHandleScope handle_scope(v8::Isolate::GetCurrent());
  const V8StringValue *v8StringValue =
      static_cast<const V8StringValue *>(getPointerValue(sym));
  return handle_scope.Escape(
      v8StringValue->v8String_.Get(v8::Isolate::GetCurrent()));
}

v8::Local<v8::Object> V8Runtime::objectRef(const jsi::Object &obj) {
  v8::EscapableHandleScope handle_scope(v8::Isolate::GetCurrent());
  const V8ObjectValue *v8ObjectValue =
      static_cast<const V8ObjectValue *>(getPointerValue(obj));
  return handle_scope.Escape(
      v8ObjectValue->v8Object_.Get(v8::Isolate::GetCurrent()));
}

std::unique_ptr<jsi::Runtime> makeV8Runtime(V8RuntimeArgs &&args) {
  return std::make_unique<V8Runtime>(std::move(args));
}

std::unique_ptr<jsi::Runtime> makeV8Runtime() {
  return std::make_unique<V8Runtime>(V8RuntimeArgs());
}

} // namespace v8runtime
