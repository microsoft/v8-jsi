// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// ----------------------------------------------------------------------------
// This file is referenced from js_native_api_v8.cc when compiled into the
// new v8jsi.dll (JSI_ABI_BUILDING). It replaces V8Runtime-based private-key
// lookup with the per-isolate IsolateData lookup the W8 architecture
// exposes. The legacy js_native_api_v8_internals.h (master state) stays
// unchanged through PR 1+2; the rename-on-delete collapse happens in PR 3.
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

#pragma once
#ifndef SRC_JS_NATIVE_API_V8_INTERNALS_ABI_H_
#define SRC_JS_NATIVE_API_V8_INTERNALS_ABI_H_

#define NODE_API_DEFAULT_MODULE_API_VERSION 8
#define NODE_API_SUPPORTED_VERSION_MAX 9
#define NODE_API_SUPPORTED_VERSION_MIN 1

// The V8 implementation of N-API, including `js_native_api_v8.h` uses certain
// idioms which require definition here. For example, it uses a variant of
// persistent references which need not be reset in the constructor. It is the
// responsibility of this file to define these idioms. Optionally, this file
// may also define `NAPI_VERSION` and set it to the version of N-API to be
// exposed.

// In the case of the Node.js implementation of N-API some of the idioms are
// imported directly from Node.js by including `node_internals.h` below. Others
// are bridged to remove references to the `node` namespace. `node_version.h`,
// included below, defines `NAPI_VERSION`.

#include "v8_core.h"
#include "js_native_api.h"
#include "util-inl.h"
#include "v8.h"

#define NAPI_ARRAYSIZE(array) node::arraysize((array))

// W8: napi_type_tag/napi_wrapper private keys live on v8rt::IsolateData
// (set up by createIsolate via v8_core). The legacy lookup through
// V8Runtime::GetCurrent(context) is gone — the keys are per-isolate, not
// per-context, so we read them from the shared struct directly.
#define NAPI_PRIVATE_KEY(context, suffix)                                      \
  (v8rt::IsolateData::fromIsolate((context)->GetIsolate())->napi_##suffix())

namespace v8impl {

template <typename T>
using Persistent = v8::Global<T>;

[[noreturn]] inline void OnFatalError(const char* location,
                                      const char* message) {
  if (location) {
    fprintf(stderr, "FATAL ERROR: %s %s\n", location, message);
  } else {
    fprintf(stderr, "FATAL ERROR: %s\n", message);
  }

  fflush(stderr);
  _exit(static_cast<int>(node::ExitCode::kAbort));
}

// Convert a v8::PersistentBase, e.g. v8::Global, to a Local, with an extra
// optimization for strong persistent handles.
class PersistentToLocal {
 public:
  // If persistent.IsWeak() == false, then do not call persistent.Reset()
  // while the returned Local<T> is still in scope, it will destroy the
  // reference to the object.
  template <class TypeName>
  static inline v8::Local<TypeName> Default(
      v8::Isolate* isolate, const v8::PersistentBase<TypeName>& persistent) {
    if (persistent.IsWeak()) {
      return PersistentToLocal::Weak(isolate, persistent);
    } else {
      return PersistentToLocal::Strong(persistent);
    }
  }

  // Unchecked conversion from a non-weak Persistent<T> to Local<T>,
  // use with care!
  //
  // Do not call persistent.Reset() while the returned Local<T> is still in
  // scope, it will destroy the reference to the object.
  template <class TypeName>
  static inline v8::Local<TypeName> Strong(
      const v8::PersistentBase<TypeName>& persistent) {
    return *reinterpret_cast<v8::Local<TypeName>*>(
        const_cast<v8::PersistentBase<TypeName>*>(&persistent));
  }

  template <class TypeName>
  static inline v8::Local<TypeName> Weak(
      v8::Isolate* isolate, const v8::PersistentBase<TypeName>& persistent) {
    return v8::Local<TypeName>::New(isolate, persistent);
  }
};

}  // end of namespace v8impl

// It is called from the napi_create_external_arraybuffer implementation
NAPI_EXTERN napi_status NAPI_CDECL
napi_create_external_buffer(napi_env env,
                            size_t length,
                            void* data,
                            node_api_nogc_finalize finalize_cb,
                            void* finalize_hint,
                            napi_value* result);

#endif  // SRC_JS_NATIVE_API_V8_INTERNALS_ABI_H_
