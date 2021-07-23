// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#ifndef SRC_PUBLIC_NAPIJSIRUNTIME_H_
#define SRC_PUBLIC_NAPIJSIRUNTIME_H_

#define NAPI_EXPERIMENTAL

#include <jsi/jsi.h>
#include <memory>
#include "js_native_ext_api.h"

namespace Microsoft { namespace JSI {

__declspec(dllexport) std::unique_ptr<facebook::jsi::Runtime> __cdecl MakeNapiJsiRuntime(napi_env env) noexcept;

}} // namespace Microsoft::JSI

#endif // SRC_PUBLIC_NAPIJSIRUNTIME_H_
