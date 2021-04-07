// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#include "public/V8JsiRuntime.h"

#include "libplatform/libplatform.h"
#include "v8.h"

#include "V8Platform.h"
#ifdef _WIN32
#include "inspector/inspector_agent.h"
#endif

#ifdef _WIN32
#include <windows.h>
#include "etw/tracing.h"
#endif

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <list>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <cstdlib>

namespace v8runtime {

// Note : Counter implementation based on d8
// A single counter in a counter collection.
class Counter {
 public:
  static const int kMaxNameSize = 64;
  int32_t *Bind(const char *name, bool histogram);
  int32_t *ptr() {
    return &count_;
  }
  int32_t count() {
    return count_;
  }
  int32_t sample_total() {
    return sample_total_;
  }
  bool is_histogram() {
    return is_histogram_;
  }
  void AddSample(int32_t sample);

 private:
  int32_t count_;
  int32_t sample_total_;
  bool is_histogram_;
  uint8_t name_[kMaxNameSize];
};

// A set of counters and associated information.  An instance of this
// class is stored directly in the memory-mapped counters file if
// the --map-counters options is used
class CounterCollection {
 public:
  CounterCollection();
  Counter *GetNextCounter();

 private:
  static const unsigned kMaxCounters = 512;
  uint32_t magic_number_;
  uint32_t max_counters_;
  uint32_t max_name_size_;
  uint32_t counters_in_use_;
  Counter counters_[kMaxCounters];
};

using CounterMap = std::unordered_map<std::string, Counter *>;

enum class CacheType { NoCache, CodeCache, FullCodeCache };

class V8PlatformHolder {
 public:
#ifdef USE_DEFAULT_PLATFORM
  v8::Platform &Get() {
#else
  V8Platform &Get() {
#endif

    assert(use_count_s_ > 0);
    assert(platform_s_);

    if (!platform_s_)
      std::terminate();

    return *platform_s_;
  }

  V8PlatformHolder() {}

  void addUsage() {
    std::lock_guard<std::mutex> guard(mutex_s_);

    if (use_count_s_++ == 0) {
      if (!platform_s_) {
#ifdef USE_DEFAULT_PLATFORM
        platform_s_ = v8::platform::NewDefaultPlatform();
#else
        platform_s_ = std::make_unique<V8Platform>(true);
#endif
        v8::V8::InitializePlatform(platform_s_.get());

        v8::V8::Initialize();
      }
    }
  }

  void releaseUsage() {
    std::lock_guard<std::mutex> guard(mutex_s_);

    if (--use_count_s_ == 0) {
      // We cannot shutdown the platform once created because V8 internally
      // references bits of the platform from process-globals This cannot be
      // worked around, the design of V8 is not currently embedder-friendly
      // v8::V8::Dispose();
      v8::V8::ShutdownPlatform();
      platform_s_ = nullptr;
    }
  }

 private:
  V8PlatformHolder(const V8PlatformHolder &) = delete;
  V8PlatformHolder &operator=(const V8PlatformHolder &) = delete;

#ifdef USE_DEFAULT_PLATFORM
  static std::unique_ptr<v8::Platform> platform_s_;
#else
  static std::unique_ptr<V8Platform> platform_s_;
#endif
  static std::atomic_uint32_t use_count_s_;
  static std::mutex mutex_s_;

}; // namespace v8runtime

struct UnhandledPromiseRejection {
  v8::Global<v8::Promise> promise;
  v8::Global<v8::Message> message;
  v8::Global<v8::Value> value;
};

class V8Runtime : public facebook::jsi::Runtime {
 public:
  V8Runtime(V8RuntimeArgs &&args);
  ~V8Runtime();

 public: // Used by NAPI implementation
  v8::Global<v8::Context> &GetContext() {
    return context_;
  }

  static V8Runtime *GetCurrent(v8::Local<v8::Context> context) noexcept;

  bool HasUnhandledPromiseRejection() noexcept;

  std::unique_ptr<UnhandledPromiseRejection>
  GetAndClearLastUnhandledPromiseRejection() noexcept;

  v8::Local<v8::Private> napi_type_tag() const noexcept {
    return isolate_data_->napi_type_tag();
  }

  v8::Local<v8::Private> napi_wrapper() const noexcept {
    return isolate_data_->napi_wrapper();
  }

 private: // Used by NAPI implementation
  static void PromiseRejectCallback(v8::PromiseRejectMessage data);
  void SetUnhandledPromise(
      v8::Local<v8::Promise> promise,
      v8::Local<v8::Message> message,
      v8::Local<v8::Value> exception);
  void RemoveUnhandledPromise(v8::Local<v8::Promise> promise);

 private: // Used by NAPI implementation
  static int const RuntimeContextTag;
  static void *const RuntimeContextTagPtr;

 private:
  V8Runtime() = delete;
  V8Runtime(const V8Runtime &) = delete;
  V8Runtime(V8Runtime &&) = delete;
  V8Runtime &operator=(const V8Runtime &) = delete;
  V8Runtime &operator=(V8Runtime &&) = delete;

 public:
  facebook::jsi::Value evaluateJavaScript(
      const std::shared_ptr<const facebook::jsi::Buffer> &buffer,
      const std::string &sourceURL) override;

  facebook::jsi::Object global() override;

  std::string description() override;

  bool isInspectable() override;

 private:
  struct IHostProxy {
    virtual void destroy() = 0;
  };

  class HostObjectLifetimeTracker {
   public:
    void ResetHostObject(bool isGC /*whether the call is coming from GC*/) {
      assert(!isGC || !isReset_);
      if (!isReset_) {
        isReset_ = true;
        hostProxy_->destroy();
        objectTracker_.Reset();
      }
    }

    HostObjectLifetimeTracker(
        V8Runtime &runtime,
        v8::Local<v8::Object> obj,
        IHostProxy *hostProxy)
        : hostProxy_(hostProxy) {
      objectTracker_.Reset(runtime.GetIsolate(), obj);
      objectTracker_.SetWeak(
          this,
          HostObjectLifetimeTracker::Destroyed,
          v8::WeakCallbackType::kParameter);
    }

    // Useful for debugging.
    ~HostObjectLifetimeTracker() {
      assert(isReset_);
    }

    bool IsEqual(IHostProxy *hostProxy) const noexcept {
      return hostProxy_ == hostProxy;
    }

   private:
    v8::Global<v8::Object> objectTracker_;
    std::atomic<bool> isReset_{false};
    IHostProxy *hostProxy_;

    static void Destroyed(
        const v8::WeakCallbackInfo<HostObjectLifetimeTracker> &data) {
      v8::HandleScope handle_scope(v8::Isolate::GetCurrent());
      data.GetParameter()->ResetHostObject(true /*isGC*/);
    }
  };

  class HostObjectProxy : public IHostProxy {
   private:
    static void GetInternal(
        std::string propName,
        const v8::PropertyCallbackInfo<v8::Value> &info) {
      v8::Local<v8::External> data =
          v8::Local<v8::External>::Cast(info.This()->GetInternalField(0));
      HostObjectProxy *hostObjectProxy =
          reinterpret_cast<HostObjectProxy *>(data->Value());

      if (hostObjectProxy == nullptr)
        std::abort();

      V8Runtime &runtime = hostObjectProxy->runtime_;
      std::shared_ptr<facebook::jsi::HostObject> hostObject =
          hostObjectProxy->hostObject_;

      facebook::jsi::Value result;
      try {
        result = hostObject->get(
            runtime,
            runtime.createPropNameIDFromUtf8(
                reinterpret_cast<uint8_t *>(&propName[0]), propName.length()));
      } catch (const facebook::jsi::JSError &error) {
        info.GetReturnValue().Set(v8::Undefined(info.GetIsolate()));

        // Schedule to throw the exception back to JS.
        info.GetIsolate()->ThrowException(runtime.valueRef(error.value()));
        return;
      } catch (const std::exception &ex) {
        info.GetReturnValue().Set(v8::Undefined(info.GetIsolate()));

        // Schedule to throw the exception back to JS.
        v8::Local<v8::String> message =
            v8::String::NewFromUtf8(
                info.GetIsolate(), ex.what(), v8::NewStringType::kNormal)
                .ToLocalChecked();
        info.GetIsolate()->ThrowException(v8::Exception::Error(message));
        return;
      } catch (...) {
        info.GetReturnValue().Set(v8::Undefined(info.GetIsolate()));

        // Schedule to throw the exception back to JS.
        v8::Local<v8::String> message =
            v8::String::NewFromOneByte(
                info.GetIsolate(),
                reinterpret_cast<const uint8_t *>(
                    "<Unknown exception in host function callback>"),
                v8::NewStringType::kNormal)
                .ToLocalChecked();
        info.GetIsolate()->ThrowException(v8::Exception::Error(message));
        return;
      }

      info.GetReturnValue().Set(runtime.valueRef(result));
    }

    static void SetInternal(
        std::string propName,
        v8::Local<v8::Value> value,
        const v8::PropertyCallbackInfo<v8::Value> &info) {
      v8::Local<v8::External> data =
          v8::Local<v8::External>::Cast(info.This()->GetInternalField(0));
      HostObjectProxy *hostObjectProxy =
          reinterpret_cast<HostObjectProxy *>(data->Value());

      if (hostObjectProxy == nullptr)
        std::abort();

      V8Runtime &runtime = hostObjectProxy->runtime_;
      std::shared_ptr<facebook::jsi::HostObject> hostObject =
          hostObjectProxy->hostObject_;

      try {
        hostObject->set(
            runtime,
            runtime.createPropNameIDFromUtf8(
                reinterpret_cast<uint8_t *>(&propName[0]), propName.length()),
            runtime.createValue(value));
      } catch (const facebook::jsi::JSError &error) {
        // Schedule to throw the exception back to JS.
        info.GetIsolate()->ThrowException(runtime.valueRef(error.value()));
      } catch (const std::exception &ex) {
        // Schedule to throw the exception back to JS.
        v8::Local<v8::String> message =
            v8::String::NewFromUtf8(
                info.GetIsolate(), ex.what(), v8::NewStringType::kNormal)
                .ToLocalChecked();
        info.GetIsolate()->ThrowException(v8::Exception::Error(message));
      } catch (...) {
        // Schedule to throw the exception back to JS.
        v8::Local<v8::String> message =
            v8::String::NewFromOneByte(
                info.GetIsolate(),
                reinterpret_cast<const uint8_t *>(
                    "<Unknown exception in host function callback>"),
                v8::NewStringType::kNormal)
                .ToLocalChecked();
        info.GetIsolate()->ThrowException(v8::Exception::Error(message));
      }
    }

   public:
    static void Get(
        v8::Local<v8::Name> v8PropName,
        const v8::PropertyCallbackInfo<v8::Value> &info) {
      v8::Local<v8::String> propNameStr =
          v8::Local<v8::String>::Cast(v8PropName);

      std::string propName;
      propName.resize(propNameStr->Utf8Length(info.GetIsolate()));
      propNameStr->WriteUtf8(info.GetIsolate(), &propName[0]);
      GetInternal(propName, info);
    }

    static void GetIndexed(
        uint32_t index,
        const v8::PropertyCallbackInfo<v8::Value> &info) {
      std::string propName = std::to_string(index);
      GetInternal(propName, info);
    }

    static void Set(
        v8::Local<v8::Name> v8PropName,
        v8::Local<v8::Value> value,
        const v8::PropertyCallbackInfo<v8::Value> &info) {
      v8::Local<v8::String> propNameStr =
          v8::Local<v8::String>::Cast(v8PropName);

      std::string propName;
      propName.resize(propNameStr->Utf8Length(info.GetIsolate()));
      propNameStr->WriteUtf8(info.GetIsolate(), &propName[0]);
      SetInternal(propName, value, info);
    }

    static void SetIndexed(
        uint32_t index,
        v8::Local<v8::Value> value,
        const v8::PropertyCallbackInfo<v8::Value> &info) {
      std::string propName = std::to_string(index);
      SetInternal(propName, value, info);
    }

    static void Enumerator(const v8::PropertyCallbackInfo<v8::Array> &info) {
      v8::Local<v8::External> data =
          v8::Local<v8::External>::Cast(info.This()->GetInternalField(0));
      HostObjectProxy *hostObjectProxy =
          reinterpret_cast<HostObjectProxy *>(data->Value());

      if (hostObjectProxy != nullptr) {
        V8Runtime &runtime = hostObjectProxy->runtime_;
        std::shared_ptr<facebook::jsi::HostObject> hostObject =
            hostObjectProxy->hostObject_;

        std::vector<facebook::jsi::PropNameID> propIds =
            hostObject->getPropertyNames(runtime);

        v8::Local<v8::Array> result =
            v8::Array::New(info.GetIsolate(), static_cast<int>(propIds.size()));
        v8::Local<v8::Context> context = info.GetIsolate()->GetCurrentContext();

        for (uint32_t i = 0; i < result->Length(); i++) {
          v8::Local<v8::Value> propIdValue = runtime.valueRef(propIds[i]);
          if (!result->Set(context, i, propIdValue).FromJust()) {
            std::terminate();
          };
        }

        info.GetReturnValue().Set(result);
      } else {
        info.GetReturnValue().Set(v8::Array::New(info.GetIsolate()));
      }
    }

    HostObjectProxy(
        V8Runtime &rt,
        const std::shared_ptr<facebook::jsi::HostObject> &hostObject)
        : runtime_(rt), hostObject_(hostObject) {}
    std::shared_ptr<facebook::jsi::HostObject> getHostObject() {
      return hostObject_;
    }

   private:
    friend class HostObjectLifetimeTracker;
    void destroy() override {
      hostObject_.reset();
    }

    V8Runtime &runtime_;
    std::shared_ptr<facebook::jsi::HostObject> hostObject_;
  };

  class HostFunctionProxy : public IHostProxy {
   public:
    static void call(
        HostFunctionProxy &hostFunctionProxy,
        const v8::FunctionCallbackInfo<v8::Value> &callbackInfo) {
      V8Runtime &runtime = const_cast<V8Runtime &>(hostFunctionProxy.runtime_);
      v8::Isolate *isolate = callbackInfo.GetIsolate();

      std::vector<facebook::jsi::Value> argsVector;
      for (int i = 0; i < callbackInfo.Length(); i++) {
        argsVector.push_back(
            hostFunctionProxy.runtime_.createValue(callbackInfo[i]));
      }

      const facebook::jsi::Value &thisVal =
          runtime.createValue(callbackInfo.This());

      facebook::jsi::Value result;
      try {
        result = hostFunctionProxy.func_(
            runtime, thisVal, argsVector.data(), callbackInfo.Length());
      } catch (const facebook::jsi::JSError &error) {
        callbackInfo.GetReturnValue().Set(v8::Undefined(isolate));

        // Schedule to throw the exception back to JS
        isolate->ThrowException(runtime.valueRef(error.value()));
        return;
      } catch (const std::exception &ex) {
        callbackInfo.GetReturnValue().Set(v8::Undefined(isolate));

        // Schedule to throw the exception back to JS
        std::string errMessage =
            std::string("Exception in HostFunction: ") + ex.what();
        v8::Local<v8::String> message =
            v8::String::NewFromUtf8(
                isolate, errMessage.c_str(), v8::NewStringType::kNormal)
                .ToLocalChecked();
        isolate->ThrowException(v8::Exception::Error(message));
        return;
      } catch (...) {
        callbackInfo.GetReturnValue().Set(v8::Undefined(isolate));

        // Schedule to throw the exception back to JS
        v8::Local<v8::String> message =
            v8::String::NewFromOneByte(
                isolate,
                reinterpret_cast<const uint8_t *>(
                    "Exception in HostFunction: <unknown>"),
                v8::NewStringType::kNormal)
                .ToLocalChecked();
        isolate->ThrowException(v8::Exception::Error(message));
        return;
      }

      callbackInfo.GetReturnValue().Set(runtime.valueRef(result));
    }

   public:
    static void HostFunctionCallback(
        const v8::FunctionCallbackInfo<v8::Value> &info) {

      TRACEV8RUNTIME_VERBOSE("HostFunctionCallback",
                        TraceLoggingString("start", "op"));

      v8::HandleScope handle_scope(v8::Isolate::GetCurrent());
      v8::Local<v8::External> data = v8::Local<v8::External>::Cast(info.Data());
      HostFunctionProxy *hostFunctionProxy =
          reinterpret_cast<HostFunctionProxy *>(data->Value());
      hostFunctionProxy->call(*hostFunctionProxy, info);

      TRACEV8RUNTIME_VERBOSE("HostFunctionCallback",
                        TraceLoggingString("end", "op"));
    }

    HostFunctionProxy(V8Runtime &runtime, facebook::jsi::HostFunctionType func)
        : func_(std::move(func)), runtime_(runtime) {}

   private:
    friend class HostObjectLifetimeTracker;
    void destroy() override {
      func_ = [](Runtime &rt,
                 const facebook::jsi::Value &thisVal,
                 const facebook::jsi::Value *args,
                 size_t count) { return facebook::jsi::Value::undefined(); };
    }

    facebook::jsi::HostFunctionType func_;
    V8Runtime &runtime_;
  };

  template <typename T>
  class V8PointerValue final : public PointerValue {
    static V8PointerValue<T> *make(v8::Local<T> objectRef) {
      return new V8PointerValue<T>(objectRef);
    }

    V8PointerValue(v8::Local<T> obj)
        : v8Object_(v8::Isolate::GetCurrent(), obj) {}

    ~V8PointerValue() {
      v8Object_.Reset();
    }

    void invalidate() override {
      delete this;
    }

    v8::Local<T> get(v8::Isolate *isolate) const {
      return v8Object_.Get(isolate);
    }

   private:
    v8::Persistent<T> v8Object_;

   protected:
    friend class V8Runtime;
  };

  using V8StringValue = V8PointerValue<v8::String>;
  using V8ObjectValue = V8PointerValue<v8::Object>;

  class ExternalOwningOneByteStringResource
      : public v8::String::ExternalOneByteStringResource {
   public:
    explicit ExternalOwningOneByteStringResource(
        const std::shared_ptr<const facebook::jsi::Buffer> &buffer)
        : buffer_(buffer) /*create a copy of shared_ptr*/ {}
    const char *data() const override {
      return reinterpret_cast<const char *>(buffer_->data());
    }
    size_t length() const override {
      return buffer_->size();
    }

   private:
    std::shared_ptr<const facebook::jsi::Buffer> buffer_;
  };

  std::shared_ptr<const facebook::jsi::PreparedJavaScript> prepareJavaScript(
      const std::shared_ptr<const facebook::jsi::Buffer> &,
      std::string) override;
  facebook::jsi::Value evaluatePreparedJavaScript(
      const std::shared_ptr<const facebook::jsi::PreparedJavaScript> &)
      override;

  std::string symbolToString(const facebook::jsi::Symbol &) override;

  PointerValue *cloneString(const PointerValue *pv) override;
  PointerValue *cloneObject(const PointerValue *pv) override;
  PointerValue *clonePropNameID(const PointerValue *pv) override;
  PointerValue *cloneSymbol(const PointerValue *pv) override;

  facebook::jsi::PropNameID createPropNameIDFromAscii(
      const char *str,
      size_t length) override;
  facebook::jsi::PropNameID createPropNameIDFromUtf8(
      const uint8_t *utf8,
      size_t length) override;
  facebook::jsi::PropNameID createPropNameIDFromString(
      const facebook::jsi::String &str) override;
  std::string utf8(const facebook::jsi::PropNameID &) override;
  bool compare(
      const facebook::jsi::PropNameID &,
      const facebook::jsi::PropNameID &) override;

  facebook::jsi::String createStringFromAscii(const char *str, size_t length)
      override;
  facebook::jsi::String createStringFromUtf8(const uint8_t *utf8, size_t length)
      override;
  std::string utf8(const facebook::jsi::String &) override;

  facebook::jsi::Object createObject() override;
  facebook::jsi::Object createObject(
      std::shared_ptr<facebook::jsi::HostObject> ho) override;
  virtual std::shared_ptr<facebook::jsi::HostObject> getHostObject(
      const facebook::jsi::Object &) override;
  facebook::jsi::HostFunctionType &getHostFunction(
      const facebook::jsi::Function &) override;

  facebook::jsi::Value getProperty(
      const facebook::jsi::Object &,
      const facebook::jsi::String &name) override;
  facebook::jsi::Value getProperty(
      const facebook::jsi::Object &,
      const facebook::jsi::PropNameID &name) override;
  bool hasProperty(
      const facebook::jsi::Object &,
      const facebook::jsi::String &name) override;
  bool hasProperty(
      const facebook::jsi::Object &,
      const facebook::jsi::PropNameID &name) override;
  void setPropertyValue(
      facebook::jsi::Object &,
      const facebook::jsi::String &name,
      const facebook::jsi::Value &value) override;
  void setPropertyValue(
      facebook::jsi::Object &,
      const facebook::jsi::PropNameID &name,
      const facebook::jsi::Value &value) override;
  bool isArray(const facebook::jsi::Object &) const override;
  bool isArrayBuffer(const facebook::jsi::Object &) const override;
  bool isFunction(const facebook::jsi::Object &) const override;
  bool isHostObject(const facebook::jsi::Object &) const override;
  bool isHostFunction(const facebook::jsi::Function &) const override;
  facebook::jsi::Array getPropertyNames(const facebook::jsi::Object &) override;

  facebook::jsi::WeakObject createWeakObject(
      const facebook::jsi::Object &) override;
  facebook::jsi::Value lockWeakObject(facebook::jsi::WeakObject &) override;

  facebook::jsi::Array createArray(size_t length) override;
  size_t size(const facebook::jsi::Array &) override;
  size_t size(const facebook::jsi::ArrayBuffer &) override;
  uint8_t *data(const facebook::jsi::ArrayBuffer &) override;
  facebook::jsi::Value getValueAtIndex(const facebook::jsi::Array &, size_t i)
      override;
  void setValueAtIndexImpl(
      facebook::jsi::Array &,
      size_t i,
      const facebook::jsi::Value &value) override;

  facebook::jsi::Function createFunctionFromHostFunction(
      const facebook::jsi::PropNameID &name,
      unsigned int paramCount,
      facebook::jsi::HostFunctionType func) override;
  facebook::jsi::Value call(
      const facebook::jsi::Function &,
      const facebook::jsi::Value &jsThis,
      const facebook::jsi::Value *args,
      size_t count) override;
  facebook::jsi::Value callAsConstructor(
      const facebook::jsi::Function &,
      const facebook::jsi::Value *args,
      size_t count) override;

  bool strictEquals(
      const facebook::jsi::String &a,
      const facebook::jsi::String &b) const override;
  bool strictEquals(
      const facebook::jsi::Object &a,
      const facebook::jsi::Object &b) const override;
  bool strictEquals(
      const facebook::jsi::Symbol &a,
      const facebook::jsi::Symbol &b) const override;

  bool instanceOf(
      const facebook::jsi::Object &o,
      const facebook::jsi::Function &f) override;

  void AddHostObjectLifetimeTracker(
      std::shared_ptr<HostObjectLifetimeTracker> hostObjectLifetimeTracker);

  static void OnMessage(
      v8::Local<v8::Message> message,
      v8::Local<v8::Value> error);
  static size_t NearHeapLimitCallback(
      void *raw_state,
      size_t current_heap_limit,
      size_t initial_heap_limit);

  static void GCPrologueCallback(
      v8::Isolate *isolate,
      v8::GCType type,
      v8::GCCallbackFlags flags);

  static void GCEpilogueCallback(
      v8::Isolate *isolate,
      v8::GCType type,
      v8::GCCallbackFlags flags);

  V8RuntimeArgs &runtimeArgs() {
    return args_;
  }

 private:
  v8::Local<v8::Context> CreateContext(v8::Isolate *isolate);

  // Methods to compile and execute JS script
  facebook::jsi::Value ExecuteString(
      const v8::Local<v8::String> &source,
      const std::string &sourceURL);

  void ReportException(v8::TryCatch *try_catch);

  v8::Isolate *GetIsolate() const {
    return isolate_;
  }

  void initializeTracing();
  void initializeV8();
  v8::Isolate *CreateNewIsolate();
  void createHostObjectConstructorPerContext();

  // Basically convenience casts
  template <typename T>
  static v8::Local<T> pvRef(const PointerValue *pv) {
    v8::EscapableHandleScope handle_scope(v8::Isolate::GetCurrent());
    const V8PointerValue<T> *v8PValue =
        static_cast<const V8PointerValue<T> *>(pv);
    return handle_scope.Escape(v8PValue->get(v8::Isolate::GetCurrent()));
  }

  static v8::Local<v8::String> stringRef(const facebook::jsi::String &str) {
    return pvRef<v8::String>(getPointerValue(str));
  }
  static v8::Local<v8::Value> valueRef(const facebook::jsi::PropNameID &sym) {
    return pvRef<v8::Value>(getPointerValue(sym));
  }
  static v8::Local<v8::Object> objectRef(const facebook::jsi::Object &obj) {
    return pvRef<v8::Object>(getPointerValue(obj));
  }
  static v8::Local<v8::Symbol> symbolRef(const facebook::jsi::Symbol &sym) {
    return pvRef<v8::Symbol>(getPointerValue(sym));
  }

  v8::Local<v8::Value> valueRef(const facebook::jsi::Value &value);
  facebook::jsi::Value createValue(v8::Local<v8::Value> value) const;

#ifdef _WIN32
  std::unique_ptr<inspector::Agent> inspector_agent_;
#endif

  V8RuntimeArgs args_;

  v8::Isolate *isolate_{nullptr};
  v8::Global<v8::Context> context_;
  v8runtime::IsolateData *isolate_data_{nullptr};

  v8::StartupData startup_data_;
  v8::Isolate::CreateParams create_params_;

  v8::Persistent<v8::FunctionTemplate> host_function_template_;
  v8::Persistent<v8::Function> host_object_constructor_;

  std::list<std::shared_ptr<HostObjectLifetimeTracker>>
      host_object_lifetime_tracker_list_;

  std::string desc_;

  static thread_local uint16_t tls_isolate_usage_counter_;

  V8PlatformHolder platform_holder_;
  v8::StartupData custom_snapshot_startup_data_;

  std::vector<std::unique_ptr<ExternalOwningOneByteStringResource>>
      owned_external_string_resources_;

  std::shared_ptr<v8::TaskRunner> foreground_task_runner_;

  bool ignore_unhandled_promises_{false};
  std::unique_ptr<UnhandledPromiseRejection> last_unhandled_promise_;

  static CounterMap *counter_map_;

  // We statically allocate a set of local counters to be used if we
  // don't want to store the stats in a memory-mapped file
  static CounterCollection local_counters_;
  static CounterCollection *counters_;
  static char counters_file_[sizeof(CounterCollection)];

  static void MapCounters(v8::Isolate *isolate, const char *name);
  static Counter *GetCounter(const char *name, bool is_histogram);
  static int *LookupCounter(const char *name);
  static void *
  CreateHistogram(const char *name, int min, int max, size_t buckets);
  static void AddHistogramSample(void *histogram, int sample);
  static void DumpCounters(const char *when);

  static void JitCodeEventListener(const v8::JitCodeEvent *event);
};
} // namespace v8runtime
