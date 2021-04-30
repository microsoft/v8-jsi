// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
// ----------------------------------------------------------------------------
// This file is referenced from js_native_api_v8.cc.
// Most of code is removed or copied from other Node.js files
// to compile V8 NAPI code without major changes.
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
#ifndef SRC_UTIL_INL_H_
#define SRC_UTIL_INL_H_

#include <memory>

// Allow use of std::min in js_native_api_v8.cc
#undef min

// From util.h
namespace node {

namespace per_process {
// Tells whether the per-process V8::Initialize() is called and
// if it is safe to call v8::Isolate::GetCurrent().
extern bool v8_initialized;
} // namespace per_process

// The reason that Assert() takes a struct argument instead of individual
// const char*s is to ease instruction cache pressure in calls from CHECK.
struct AssertionInfo {
  const char *file_line; // filename:line
  const char *message;
  const char *function;
};
[[noreturn]] void Assert(const AssertionInfo &info);

#define ERROR_AND_ABORT(expr)                                                                                \
  do {                                                                                                       \
    /* Make sure that this struct does not end up in inline code, but      */                                \
    /* rather in a read-only data section when modifying this code.        */                                \
    static const node::AssertionInfo args = {__FILE__ ":" STRINGIFY(__LINE__), #expr, PRETTY_FUNCTION_NAME}; \
    node::Assert(args);                                                                                      \
  } while (0)

#ifdef __GNUC__
#define UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#define PRETTY_FUNCTION_NAME __PRETTY_FUNCTION__
#else
#define UNLIKELY(expr) expr
#define PRETTY_FUNCTION_NAME ""
#endif

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

#define CHECK(expr)          \
  do {                       \
    if (UNLIKELY(!(expr))) { \
      ERROR_AND_ABORT(expr); \
    }                        \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_GE(a, b) CHECK((a) >= (b))
#define CHECK_GT(a, b) CHECK((a) > (b))
#define CHECK_LE(a, b) CHECK((a) <= (b))
#define CHECK_LT(a, b) CHECK((a) < (b))
#define CHECK_NE(a, b) CHECK((a) != (b))
#define CHECK_NULL(val) CHECK((val) == nullptr)
#define CHECK_NOT_NULL(val) CHECK((val) != nullptr)
#define CHECK_IMPLIES(a, b) CHECK(!(a) || (b))

#ifdef DEBUG
#define DCHECK(expr) CHECK(expr)
#define DCHECK_EQ(a, b) CHECK((a) == (b))
#define DCHECK_GE(a, b) CHECK((a) >= (b))
#define DCHECK_GT(a, b) CHECK((a) > (b))
#define DCHECK_LE(a, b) CHECK((a) <= (b))
#define DCHECK_LT(a, b) CHECK((a) < (b))
#define DCHECK_NE(a, b) CHECK((a) != (b))
#define DCHECK_NULL(val) CHECK((val) == nullptr)
#define DCHECK_NOT_NULL(val) CHECK((val) != nullptr)
#define DCHECK_IMPLIES(a, b) CHECK(!(a) || (b))
#else
#define DCHECK(expr)
#define DCHECK_EQ(a, b)
#define DCHECK_GE(a, b)
#define DCHECK_GT(a, b)
#define DCHECK_LE(a, b)
#define DCHECK_LT(a, b)
#define DCHECK_NE(a, b)
#define DCHECK_NULL(val)
#define DCHECK_NOT_NULL(val)
#define DCHECK_IMPLIES(a, b)
#endif

template <typename T, size_t N>
constexpr size_t arraysize(const T (&)[N]) {
  return N;
}

template <typename T, size_t N>
constexpr size_t strsize(const T (&)[N]) {
  return N - 1;
}

// Like std::static_pointer_cast but for unique_ptr with the default deleter.
template <typename T, typename U>
std::unique_ptr<T> static_unique_pointer_cast(std::unique_ptr<U> &&ptr) {
  return std::unique_ptr<T>(static_cast<T *>(ptr.release()));
}

} // namespace node

#endif // SRC_UTIL_INL_H_
