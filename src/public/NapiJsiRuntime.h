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

namespace Microsoft { namespace JSI {

V8JSI_EXPORT std::unique_ptr<facebook::jsi::Runtime> __cdecl MakeNapiJsiRuntime(napi_env env) noexcept;

}} // namespace Microsoft::JSI

#endif // SRC_PUBLIC_NAPIJSIRUNTIME_H_
