// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#ifndef SRC_PUBLIC_NAPIJSIRUNTIME_H_
#define SRC_PUBLIC_NAPIJSIRUNTIME_H_

#define NAPI_EXPERIMENTAL

#include <jsi/jsi.h>
#include <memory>
#include "js_native_ext_api.h"

#ifndef V8JSI_EXPORT
#ifdef _MSC_VER
#ifdef BUILDING_V8JSI_SHARED
#define V8JSI_EXPORT __declspec(dllexport)
#else
#define V8JSI_EXPORT
#endif // BUILDING_V8JSI_SHARED
#else // _MSC_VER
#define V8JSI_EXPORT __attribute__((visibility("default")))
#endif // _MSC_VER
#endif // !defined(V8JSI_EXPORT)

namespace Microsoft::JSI {

V8JSI_EXPORT std::unique_ptr<facebook::jsi::Runtime> __cdecl MakeNapiJsiRuntime(napi_env env) noexcept;

template <typename T>
struct NativeObjectWrapper;

template <typename T>
struct NativeObjectWrapper<std::unique_ptr<T>> {
  static napi_ext_native_data Wrap(std::unique_ptr<T> &&obj) noexcept {
    napi_ext_native_data nativeData{};
    nativeData.data = obj.release();
    nativeData.finalize_cb = [](napi_env /*env*/, void *data, void * /*finalizeHint*/) {
      std::unique_ptr<T> obj{reinterpret_cast<T *>(data)};
    };
    return nativeData;
  }

  static T *Unwrap(napi_ext_native_data &nativeData) noexcept {
    return reinterpret_cast<T *>(nativeData.data);
  }
};

template <typename T>
struct NativeObjectWrapper<std::shared_ptr<T>> {
  static napi_ext_native_data Wrap(std::shared_ptr<T> &&obj) noexcept {
    static_assert(
        sizeof(SharedPtrHolder) == sizeof(std::shared_ptr<T>), "std::shared_ptr expected to have size of two pointers");
    SharedPtrHolder ptrHolder;
    new (std::addressof(ptrHolder)) std::shared_ptr(std::move(obj));
    napi_ext_native_data nativeData{};
    nativeData.data = ptrHolder.ptr1;
    nativeData.finalize_hint = ptrHolder.ptr2;
    nativeData.finalize_cb = [](napi_env /*env*/, void *data, void *finalizeHint) {
      SharedPtrHolder ptrHolder{data, finalizeHint};
      std::shared_ptr<T> obj(std::move(*reinterpret_cast<std::shared_ptr<T> *>(std::addressof(ptrHolder))));
    };
    return nativeData;
  }

  static std::shared_ptr<T> Unwrap(napi_ext_native_data &nativeData) noexcept {
    SharedPtrHolder ptrHolder{nativeData.data, nativeData.finalize_hint};
    return *reinterpret_cast<std::shared_ptr<T> *>(std::addressof(ptrHolder));
  }

 private:
  struct SharedPtrHolder {
    void *ptr1;
    void *ptr2;
  };
};

} // namespace Microsoft::JSI

#endif // SRC_PUBLIC_NAPIJSIRUNTIME_H_
