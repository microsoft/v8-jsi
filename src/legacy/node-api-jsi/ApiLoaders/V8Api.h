// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#ifndef APILOADERS_V8API_H_
#define APILOADERS_V8API_H_

#include <v8_api.h>
#include "JSRuntimeApi.h"

namespace Microsoft::NodeApiJsi {

class V8Api : public JSRuntimeApi {
 public:
  V8Api(IFuncResolver *funcResolver);

  static V8Api *current() noexcept {
    return current_;
  }

  static void setCurrent(V8Api *current) noexcept {
    JSRuntimeApi::setCurrent(current);
    current_ = current;
  }

  static V8Api *fromLib();

  class Scope : public JSRuntimeApi::Scope {
   public:
    Scope() : Scope(V8Api::fromLib()) {}

    Scope(V8Api *v8Api) : JSRuntimeApi::Scope(v8Api), prevV8Api_(V8Api::current_) {
      V8Api::current_ = v8Api;
    }

    ~Scope() {
      V8Api::current_ = prevV8Api_;
    }

   private:
    V8Api *prevV8Api_;
  };

 public:
#define V8_FUNC(func) decltype(::func) *const func;
#include "V8Api.inc"

 private:
  static thread_local V8Api *current_;
};

} // namespace Microsoft::NodeApiJsi

#endif // !APILOADERS_V8API_H_
