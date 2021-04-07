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

static struct napi_env_scope__ {
  napi_env_scope__(v8::Isolate *isolate, v8::Local<v8::Context> context)
      : isolate_scope(isolate ? new v8::Isolate::Scope(isolate) : nullptr),
        context_scope(
            !context.IsEmpty() ? new v8::Context::Scope(context) : nullptr) {}

  napi_env_scope__(napi_env_scope__ const &) = delete;
  napi_env_scope__ &operator=(napi_env_scope__ const &) = delete;

  napi_env_scope__(napi_env_scope__ &&other)
      : isolate_scope(other.isolate_scope), context_scope(other.context_scope) {
    other.isolate_scope = nullptr;
    other.context_scope = nullptr;
  }

  napi_env_scope__ &operator=(napi_env_scope__ &&other) {
    if (this != &other) {
      napi_env_scope__ temp{std::move(*this)};
      Swap(other);
    }
    return *this;
  }

  void Swap(napi_env_scope__ &other) {
    std::swap(isolate_scope, other.isolate_scope);
    std::swap(context_scope, other.context_scope);
  }

  ~napi_env_scope__() {
    if (context_scope) {
      delete context_scope;
    }

    if (isolate_scope) {
      delete isolate_scope;
    }
  }

 private:
  v8::Isolate::Scope *isolate_scope{nullptr};
  v8::Context::Scope *context_scope{nullptr};
};

napi_status napi_ext_create_env(
    napi_ext_env_attributes attributes,
    napi_env *env) {
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
  runtime.release();

  return napi_status::napi_ok;
}

napi_status napi_ext_delete_env(napi_env env) {
  CHECK_ENV(env);
  auto runtime = std::unique_ptr<v8runtime::V8Runtime>(
      v8runtime::V8Runtime::GetCurrent(env->context()));
  auto env_ptr = std::unique_ptr<napi_env__>(env);
  return napi_status::napi_ok;
}

napi_status napi_ext_open_env_scope(napi_env env, napi_env_scope *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  *result = new napi_env_scope__(env->isolate, env->context());
  return napi_ok;
}

napi_status napi_ext_close_env_scope(napi_env env, napi_env_scope scope) {
  CHECK_ENV(env);
  CHECK_ARG(env, scope);

  delete scope;
  return napi_ok;
}

napi_status napi_ext_has_unhandled_promise_rejection(
    napi_env env,
    bool *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  auto runtime = v8runtime::V8Runtime::GetCurrent(env->context());
  CHECK_ARG(env, runtime);

  *result = runtime->HasUnhandledPromiseRejection();
  return napi_ok;
}

napi_status napi_get_and_clear_last_unhandled_promise_rejection(
    napi_env env,
    napi_value *result) {
  CHECK_ENV(env);
  CHECK_ARG(env, result);

  auto runtime = v8runtime::V8Runtime::GetCurrent(env->context());
  CHECK_ARG(env, runtime);

  auto rejectionInfo = runtime->GetAndClearLastUnhandledPromiseRejection();
  *result =
      v8impl::JsValueFromV8LocalValue(rejectionInfo->value.Get(env->isolate));
  return napi_ok;
}

napi_status napi_ext_run_script(
    napi_env env,
    napi_value script,
    const char *source_url,
    napi_value *result) {
  NAPI_PREAMBLE(env);
  CHECK_ARG(env, script);
  CHECK_ARG(env, result);

  v8::Local<v8::Value> v8_script = v8impl::V8LocalValueFromJsValue(script);

  if (!v8_script->IsString()) {
    return napi_set_last_error(env, napi_string_expected);
  }

  v8::Local<v8::Context> context = env->context();

  v8::Local<v8::String> urlV8String =
      v8::String::NewFromUtf8(context->GetIsolate(), source_url)
          .ToLocalChecked();
  v8::ScriptOrigin origin(urlV8String);

  auto maybe_script = v8::Script::Compile(
      context, v8::Local<v8::String>::Cast(v8_script), &origin);
  CHECK_MAYBE_EMPTY(env, maybe_script, napi_generic_failure);

  auto script_result = maybe_script.ToLocalChecked()->Run(context);
  CHECK_MAYBE_EMPTY(env, script_result, napi_generic_failure);

  *result = v8impl::JsValueFromV8LocalValue(script_result.ToLocalChecked());
  return GET_RETURN_STATUS(env);
}

napi_status napi_ext_collect_garbage(napi_env env) {
  env->isolate->RequestGarbageCollectionForTesting(
      v8::Isolate::kFullGarbageCollection);
  return napi_status::napi_ok;
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
  std::string name = "aaa"; // [vmoroz] GetHumanReadableProcessName();

  fprintf(
      stderr,
      "%s: %s:%s%s Assertion `%s' failed.\n",
      name.c_str(),
      info.file_line,
      info.function,
      *info.function ? ":" : "",
      info.message);
  fflush(stderr);

  // [vmoroz] Abort();
}

} // namespace node

// TODO: [vmoroz] verify that finalize_cb runs in JS thread
// The created Buffer is the Uint8Array.
extern napi_status napi_create_external_buffer(
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

  DeleterData *deleterData = finalize_cb != nullptr
      ? new DeleterData{env, finalize_cb, finalize_hint}
      : nullptr;
  auto backingStore = v8::ArrayBuffer::NewBackingStore(
      data,
      length,
      [](void *data, size_t length, void *deleter_data) {
        DeleterData *deleterData = static_cast<DeleterData *>(deleter_data);
        if (deleterData != nullptr) {
          deleterData->finalize_cb(
              deleterData->env, data, deleterData->finalize_hint);
          delete deleterData;
        }
      },
      deleterData);

  v8::Local<v8::ArrayBuffer> arrayBuffer = v8::ArrayBuffer::New(
      isolate, std::shared_ptr<v8::BackingStore>(std::move(backingStore)));

  v8::Local<v8::Uint8Array> buffer =
      v8::Uint8Array::New(arrayBuffer, 0, length);

  *result = v8impl::JsValueFromV8LocalValue(buffer);
  return GET_RETURN_STATUS(env);
}
