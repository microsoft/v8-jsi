// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#include "v8.h"

#include "public/V8JsiRuntime.h"

namespace v8runtime {

constexpr int ISOLATE_DATA_SLOT = 0;
constexpr int ISOLATE_INSPECTOR_SLOT = 1;

// Custom data associated with each V8 isolate.
struct IsolateData {
  IsolateData(v8::Isolate *isolate, std::shared_ptr<JSITaskRunner> foreground_task_runner) noexcept
      : isolate_{isolate}, foreground_task_runner_{std::move(foreground_task_runner)} {}

  std::shared_ptr<JSITaskRunner> foreground_task_runner_;

  v8::Local<v8::Private> napi_type_tag() const {
    return napi_type_tag_.Get(isolate_);
  }

  v8::Local<v8::Private> napi_wrapper() const {
    return napi_wrapper_.Get(isolate_);
  }

  v8::Local<v8::Private> nativeStateKey() const {
    return nativeStateKey_.Get(isolate_);
  }

  // Creates property names often used by the NAPI implementation.
  void CreateProperties() {
    v8::HandleScope handle_scope(isolate_);
    CreateProperty(napi_type_tag_, "node:napi:type_tag");
    CreateProperty(napi_wrapper_, "node:napi:wrapper");
    CreateProperty(nativeStateKey_, "v8:jsi:nativeStateKey");
  }

 private:
  template <size_t N>
  void CreateProperty(v8::Eternal<v8::Private> &property, const char (&name)[N]) {
    property.Set(
        isolate_,
        v8::Private::New(
            isolate_,
            v8::String::NewFromOneByte(
                isolate_, reinterpret_cast<const uint8_t *>(name), v8::NewStringType::kInternalized, N - 1)
                .ToLocalChecked()));
  }

 private:
  v8::Isolate *isolate_;
  v8::Eternal<v8::Private> napi_type_tag_;
  v8::Eternal<v8::Private> napi_wrapper_;
  v8::Eternal<v8::Private> nativeStateKey_;
};

} // namespace v8runtime
