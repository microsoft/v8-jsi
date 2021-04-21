// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// ----------------------------------------------------------------------------
// Some code is copied from Node.js project to compile V8 NAPI code
// without major changes.
// ----------------------------------------------------------------------------
// Original Node.js copyright:
//
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "env-inl.h"

#include "V8JsiRuntime_impl.h"
#include "js_native_api_v8.h"
#include "js_native_ext_api.h"
#include "public/ScriptStore.h"

namespace v8impl {

// Reference counter implementation.
struct ExtRefCounter : protected RefTracker {
  ~ExtRefCounter() override {
    Unlink();
  }

  void Ref() {
    ++ref_count_;
  }

  void Unref() {
    if (--ref_count_ == 0) {
      Finalize(false);
    }
  }

  virtual v8::Local<v8::Value> Get(napi_env env) = 0;

 protected:
  ExtRefCounter(napi_env env) {
    Link(&env->reflist);
  }

  void Finalize(bool is_env_teardown) override {
    delete this;
  }

 private:
  uint32_t ref_count_{1};
};

// Wrapper around v8impl::Persistent that implements reference counting.
struct ExtReference : protected ExtRefCounter {
  static ExtReference *New(napi_env env, v8::Local<v8::Value> value) {
    return new ExtReference(env, value);
  }

  v8::Local<v8::Value> Get(napi_env env) override {
    return persistent_.Get(env->isolate);
  }

 protected:
  ExtReference(napi_env env, v8::Local<v8::Value> value) : ExtRefCounter(env), persistent_(env->isolate, value) {}

 private:
  v8impl::Persistent<v8::Value> persistent_;
};

// Associates data with ExtReference.
struct ExtReferenceWithData : protected ExtReference {
  static ExtReferenceWithData *
  New(napi_env env, v8::Local<v8::Value> value, void *native_object, napi_finalize finalize_cb, void *finalize_hint) {
    return new ExtReferenceWithData(env, value, native_object, finalize_cb, finalize_hint);
  }

 protected:
  ExtReferenceWithData(
      napi_env env,
      v8::Local<v8::Value> value,
      void *native_object,
      napi_finalize finalize_cb,
      void *finalize_hint)
      : ExtReference(env, value),
        env_{env},
        native_object_{native_object},
        finalize_cb_{finalize_cb},
        finalize_hint_{finalize_hint} {}

  void Finalize(bool is_env_teardown) override {
    if (finalize_cb_) {
      finalize_cb_(env_, native_object_, finalize_hint_);
      finalize_cb_ = nullptr;
    }
    ExtReference::Finalize(is_env_teardown);
  }

 private:
  napi_env env_{nullptr};
  void *native_object_{nullptr};
  napi_finalize finalize_cb_{nullptr};
  void *finalize_hint_{nullptr};
};

// Wrapper around v8impl::Persistent that implements reference counting.
struct ExtWeakReference : protected ExtRefCounter {
  static ExtWeakReference *New(napi_env env, v8::Local<v8::Value> value) {
    return new ExtWeakReference(env, value);
  }

  ~ExtWeakReference() override {
    napi_delete_reference(env_, weak_ref_);
  }

  v8::Local<v8::Value> Get(napi_env env) override {
    napi_value result{};
    napi_get_reference_value(env, weak_ref_, &result);
    return result ? v8impl::V8LocalValueFromJsValue(result) : v8::Local<v8::Value>();
  }

 protected:
  ExtWeakReference(napi_env env, v8::Local<v8::Value> value) : ExtRefCounter(env), env_{env} {
    napi_create_reference(env, v8impl::JsValueFromV8LocalValue(value), 0, &weak_ref_);
  }

 private:
  napi_env env_{nullptr};
  napi_ref weak_ref_{nullptr};
};

// Responsible for notifying V8Runtime that the NAPI env is destroyed.
struct V8RuntimeHolder : protected v8impl::RefTracker {
  V8RuntimeHolder(napi_env env, v8runtime::V8Runtime *runtime) : runtime_{std::move(runtime)} {
    Link(&env->finalizing_reflist);
  }

  ~V8RuntimeHolder() override {
    Unlink();
  }

  void Finalize(bool is_env_teardown) override {
    runtime_->SetIsEnvDeleted();
    delete this;
  }

 private:
  v8runtime::V8Runtime *runtime_;
};

} // namespace v8impl

static struct EnvScope {
  EnvScope(napi_env env)
      : env_{env},
        isolate_scope_{new v8::Isolate::Scope(env->isolate)},
        context_scope_{new v8::Context::Scope(env->context())} {
    napi_open_handle_scope(env, &handle_scope_);
  }

  ~EnvScope() {
    napi_close_handle_scope(env_, handle_scope_);
  }

  // This type is not copyable
  EnvScope(const EnvScope &) = delete;
  EnvScope &operator=(const EnvScope &) = delete;

  // This type is movable
  EnvScope(EnvScope &&other)
      : env_{std::exchange(other.env_, nullptr)},
        isolate_scope_{std::exchange(other.isolate_scope_, nullptr)},
        context_scope_{std::exchange(other.context_scope_, nullptr)},
        handle_scope_{std::exchange(other.handle_scope_, nullptr)} {}

  EnvScope &operator=(EnvScope &&other) {
    if (this != &other) {
      EnvScope temp{std::move(*this)};
      Swap(other);
    }
    return *this;
  }

  void Swap(EnvScope &other) {
    using std::swap;
    swap(env_, other.env_);
    swap(isolate_scope_, other.isolate_scope_);
    swap(context_scope_, other.context_scope_);
    swap(handle_scope_, other.handle_scope_);
  }

 private:
  napi_env env_;
  std::unique_ptr<v8::Isolate::Scope> isolate_scope_{};
  std::unique_ptr<v8::Context::Scope> context_scope_{};
  napi_handle_scope handle_scope_{};
};

napi_status napi_ext_create_env(napi_ext_env_attributes attributes, napi_env *env) {
  v8runtime::V8RuntimeArgs args;
  args.trackGCObjectStats = false;
  args.enableTracing = false;
  args.enableJitTracing = false;
  args.enableMessageTracing = false;
  args.enableLog = false;
  args.enableGCTracing = false;

  if ((attributes & napi_ext_env_attribute_enable_gc_api) != 0) {
    args.enableGCApi = true;
  }

  if ((attributes & napi_ext_env_attribute_ignore_unhandled_promises) != 0) {
    args.ignoreUnhandledPromises = true;
  }

  auto runtime = std::make_unique<v8runtime::V8Runtime>(std::move(args));

  auto context = v8impl::PersistentToLocal::Strong(runtime->GetContext());
  *env = new napi_env__(context);

  // Let the runtime exists. It can be accessed from the Context.
  new v8impl::V8RuntimeHolder(*env, runtime.release());

  return napi_status::napi_ok;
}

napi_status napi_ext_env_ref(napi_env env) {
  CHECK_ENV(env);
  env->Ref();
  return napi_status::napi_ok;
}

napi_status napi_ext_env_unref(napi_env env) {
  CHECK_ENV(env);
  auto runtime = v8runtime::V8Runtime::GetCurrent(env->context());
  env->Unref();
  if (runtime->IsEnvDeleted()) {
    delete runtime;
  }
  return napi_status::napi_ok;
}

napi_status napi_ext_open_env_scope(napi_env env, napi_ext_env_scope *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  *result = reinterpret_cast<napi_ext_env_scope>(new EnvScope(env));
  return napi_ok;
}

napi_status napi_ext_close_env_scope(napi_env env, napi_ext_env_scope scope) {
  CHECK_ENV(env);
  CHECK_ARG(env, scope);

  delete reinterpret_cast<EnvScope*>(scope);
  return napi_ok;
}

napi_status napi_ext_has_unhandled_promise_rejection(napi_env env, bool *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  auto runtime = v8runtime::V8Runtime::GetCurrent(env->context());
  CHECK_ARG(env, runtime);

  *result = runtime->HasUnhandledPromiseRejection();
  return napi_ok;
}

napi_status napi_get_and_clear_last_unhandled_promise_rejection(napi_env env, napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  auto runtime = v8runtime::V8Runtime::GetCurrent(env->context());
  CHECK_ARG(env, runtime);

  auto rejectionInfo = runtime->GetAndClearLastUnhandledPromiseRejection();
  *result = v8impl::JsValueFromV8LocalValue(rejectionInfo->value.Get(env->isolate));
  return napi_ok;
}

napi_status napi_ext_run_script(napi_env env, napi_value source, const char *source_url, napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, source);
  CHECK_ARG(env, source_url);
  CHECK_ARG(env, result);

  v8::Local<v8::Value> v8_source = v8impl::V8LocalValueFromJsValue(source);

  if (!v8_source->IsString()) {
    return napi_set_last_error(env, napi_string_expected);
  }

  v8::Local<v8::Context> context = env->context();

  v8::Local<v8::String> urlV8String = v8::String::NewFromUtf8(context->GetIsolate(), source_url).ToLocalChecked();
  v8::ScriptOrigin origin(urlV8String);

  auto maybe_script = v8::Script::Compile(context, v8::Local<v8::String>::Cast(v8_source), &origin);
  CHECK_MAYBE_EMPTY(env, maybe_script, napi_generic_failure);

  auto script_result = maybe_script.ToLocalChecked()->Run(context);
  CHECK_MAYBE_EMPTY(env, script_result, napi_generic_failure);

  *result = v8impl::JsValueFromV8LocalValue(script_result.ToLocalChecked());
  return GET_RETURN_STATUS(env);
}

napi_status napi_ext_run_serialized_script(
    napi_env env,
    uint8_t const *buffer,
    size_t buffer_length,
    napi_value source,
    char const *source_url,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  if (!buffer || !buffer_length) {
    return napi_ext_run_script(env, source, source_url, result);
  }
  CHECK_ARG(env, source);
  CHECK_ARG(env, source_url);
  CHECK_ARG(env, result);

  v8::Local<v8::Value> v8_source = v8impl::V8LocalValueFromJsValue(source);

  if (!v8_source->IsString()) {
    return napi_set_last_error(env, napi_string_expected);
  }

  v8::Local<v8::Context> context = env->context();

  v8::Local<v8::String> urlV8String = v8::String::NewFromUtf8(context->GetIsolate(), source_url).ToLocalChecked();
  v8::ScriptOrigin origin(urlV8String);

  auto cached_data = new v8::ScriptCompiler::CachedData(buffer, static_cast<int>(buffer_length));
  v8::ScriptCompiler::Source script_source(v8::Local<v8::String>::Cast(v8_source), origin, cached_data);
  auto options = v8::ScriptCompiler::CompileOptions::kConsumeCodeCache;

  auto maybe_script = v8::ScriptCompiler::Compile(context, &script_source, options);
  CHECK_MAYBE_EMPTY(env, maybe_script, napi_generic_failure);

  auto script_result = maybe_script.ToLocalChecked()->Run(context);
  CHECK_MAYBE_EMPTY(env, script_result, napi_generic_failure);

  *result = v8impl::JsValueFromV8LocalValue(script_result.ToLocalChecked());
  return GET_RETURN_STATUS(env);
}

napi_status napi_ext_serialize_script(
    napi_env env,
    napi_value source,
    char const *source_url,
    napi_ext_buffer_callback buffer_cb,
    void *buffer_hint) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, source);
  CHECK_ARG(env, buffer_cb);

  v8::Local<v8::Value> v8_source = v8impl::V8LocalValueFromJsValue(source);

  if (!v8_source->IsString()) {
    return napi_set_last_error(env, napi_string_expected);
  }

  v8::Local<v8::Context> context = env->context();

  v8::Local<v8::String> urlV8String = v8::String::NewFromUtf8(context->GetIsolate(), source_url).ToLocalChecked();
  v8::ScriptOrigin origin(urlV8String);

  v8::Local<v8::UnboundScript> script;
  v8::ScriptCompiler::Source script_source(v8::Local<v8::String>::Cast(v8_source), origin);

  if (v8::ScriptCompiler::CompileUnboundScript(context->GetIsolate(), &script_source).ToLocal(&script)) {
    v8::ScriptCompiler::CachedData *code_cache = v8::ScriptCompiler::CreateCodeCache(script);

    buffer_cb(env, code_cache->data, code_cache->length, buffer_hint);
  }

  return GET_RETURN_STATUS(env);
}

napi_status napi_ext_collect_garbage(napi_env env) {
  env->isolate->RequestGarbageCollectionForTesting(v8::Isolate::kFullGarbageCollection);
  return napi_status::napi_ok;
}

NAPI_EXTERN napi_status
napi_ext_get_unique_string_utf8_ref(napi_env env, const char *str, size_t length, napi_ext_ref *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, str);
  CHECK_ARG(env, result);

  auto runtime = v8runtime::V8Runtime::GetCurrent(env->context());
  CHECK_ARG(env, runtime);
  STATUS_CALL(runtime->NapiGetUniqueUtf8StringRef(env, str, length, result));

  return GET_RETURN_STATUS(env);
}

NAPI_EXTERN napi_status
napi_ext_get_unique_string_ref(napi_env env, napi_value str_value, napi_ext_ref *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, str_value);
  CHECK_ARG(env, result);

  v8::Local<v8::Value> v8value = v8impl::V8LocalValueFromJsValue(str_value);
  RETURN_STATUS_IF_FALSE(env, v8value->IsString(), napi_string_expected);
  v8::String::Utf8Value utf8Value{env->isolate, v8value};

  auto runtime = v8runtime::V8Runtime::GetCurrent(env->context());
  CHECK_ARG(env, runtime);
  STATUS_CALL(runtime->NapiGetUniqueUtf8StringRef(env, *utf8Value, utf8Value.length(), result));

  return GET_RETURN_STATUS(env);
}

namespace node {

namespace per_process {
// From node.cc
// Tells whether the per-process V8::Initialize() is called and
// if it is safe to call v8::Isolate::GetCurrent().
bool v8_initialized = false;
} // namespace per_process

// From node_errors.cc
[[noreturn]] void Assert(const AssertionInfo &info) {
  char *processName{};
  _get_pgmptr(&processName);

  fprintf(
      stderr,
      "%s: %s:%s%s Assertion `%s' failed.\n",
      processName,
      info.file_line,
      info.function,
      *info.function ? ":" : "",
      info.message);
  fflush(stderr);

  TRACEV8RUNTIME_CRITICAL("Assertion failed");

  std::terminate();
}

} // namespace node

// TODO: [vmoroz] verify that finalize_cb runs in JS thread
// The created Buffer is the Uint8Array as in Node.js with version >= 4.
napi_status napi_create_external_buffer(
    napi_env env,
    size_t length,
    void *data,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, result);

  struct DeleterData {
    napi_env env;
    napi_finalize finalize_cb;
    void *finalize_hint;
  };

  v8::Isolate *isolate = env->isolate;

  DeleterData *deleterData = finalize_cb != nullptr ? new DeleterData{env, finalize_cb, finalize_hint} : nullptr;
  auto backingStore = v8::ArrayBuffer::NewBackingStore(
      data,
      length,
      [](void *data, size_t length, void *deleter_data) {
        DeleterData *deleterData = static_cast<DeleterData *>(deleter_data);
        if (deleterData != nullptr) {
          deleterData->finalize_cb(deleterData->env, data, deleterData->finalize_hint);
          delete deleterData;
        }
      },
      deleterData);

  v8::Local<v8::ArrayBuffer> arrayBuffer =
      v8::ArrayBuffer::New(isolate, std::shared_ptr<v8::BackingStore>(std::move(backingStore)));

  v8::Local<v8::Uint8Array> buffer = v8::Uint8Array::New(arrayBuffer, 0, length);

  *result = v8impl::JsValueFromV8LocalValue(buffer);
  return GET_RETURN_STATUS(env);
}

// Creates new napi_ext_ref with ref counter set to 1.
NAPI_EXTERN napi_status napi_ext_create_reference(napi_env env, napi_value value, napi_ext_ref *result) {
  // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot throw
  // JS exceptions.
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  v8::Local<v8::Value> v8_value = v8impl::V8LocalValueFromJsValue(value);

  v8impl::ExtReference *reference = v8impl::ExtReference::New(env, v8_value);

  *result = reinterpret_cast<napi_ext_ref>(reference);
  return napi_clear_last_error(env);
}

// Creates new napi_ext_ref and associates native data with the reference.
// The ref counter is set to 1.
NAPI_EXTERN napi_status napi_ext_create_reference_with_data(
    napi_env env,
    napi_value value,
    void *native_object,
    napi_finalize finalize_cb,
    void *finalize_hint,
    napi_ext_ref *result) {
  // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot throw
  // JS exceptions.
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, native_object);
  CHECK_ARG(env, result);

  v8::Local<v8::Value> v8_value = v8impl::V8LocalValueFromJsValue(value);

  v8impl::ExtReferenceWithData *reference =
      v8impl::ExtReferenceWithData::New(env, v8_value, native_object, finalize_cb, finalize_hint);

  *result = reinterpret_cast<napi_ext_ref>(reference);
  return napi_clear_last_error(env);
}

NAPI_EXTERN napi_status napi_ext_create_weak_reference(napi_env env, napi_value value, napi_ext_ref *result) {
  // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot throw
  // JS exceptions.
  CHECK_ENV(env);
  CHECK_ARG(env, value);
  CHECK_ARG(env, result);

  v8::Local<v8::Value> v8_value = v8impl::V8LocalValueFromJsValue(value);

  v8impl::ExtWeakReference *reference = v8impl::ExtWeakReference::New(env, v8_value);

  *result = reinterpret_cast<napi_ext_ref>(reference);
  return napi_clear_last_error(env);
}

// Increments the reference count.
NAPI_EXTERN napi_status napi_ext_reference_ref(napi_env env, napi_ext_ref ref) {
  // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot throw
  // JS exceptions.
  CHECK_ENV(env);
  CHECK_ARG(env, ref);

  v8impl::ExtRefCounter *reference = reinterpret_cast<v8impl::ExtRefCounter *>(ref);
  reference->Ref();

  return napi_clear_last_error(env);
}

// Decrements the reference count.
// The provided ref must not be used after this call because it could be deleted
// if the internal ref counter became zero.
NAPI_EXTERN napi_status napi_ext_reference_unref(napi_env env, napi_ext_ref ref) {
  // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot throw
  // JS exceptions.
  CHECK_ENV(env);
  CHECK_ARG(env, ref);

  v8impl::ExtRefCounter *reference = reinterpret_cast<v8impl::ExtRefCounter *>(ref);

  reference->Unref();

  return napi_clear_last_error(env);
}

// Gets the referenced value.
NAPI_EXTERN napi_status napi_ext_get_reference_value(napi_env env, napi_ext_ref ref, napi_value *result) {
  // Omit NAPI_PREAMBLE and GET_RETURN_STATUS because V8 calls here cannot throw
  // JS exceptions.
  CHECK_ENV(env);
  CHECK_ARG(env, ref);
  CHECK_ARG(env, result);

  v8impl::ExtRefCounter *reference = reinterpret_cast<v8impl::ExtRefCounter *>(ref);
  *result = v8impl::JsValueFromV8LocalValue(reference->Get(env));

  return napi_clear_last_error(env);
}
