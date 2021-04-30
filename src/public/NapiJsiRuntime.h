// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#ifndef SRC_PUBLIC_NAPIJSIRUNTIME_H_
#define SRC_PUBLIC_NAPIJSIRUNTIME_H_

#define NAPI_EXPERIMENTAL

#include <jsi/jsi.h>
#include <memory>
#include "js_native_ext_api.h"

namespace napijsi {

std::unique_ptr<facebook::jsi::Runtime> MakeNapiJsiRuntime(napi_env env) noexcept;

} // namespace napijsi

#endif // SRC_PUBLIC_NAPIJSIRUNTIME_H_
