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
#include <string>
#include "v8.h"

// Allow use of std::min in js_native_api_v8.cc
#undef min

#ifdef __GNUC__
#define MUST_USE_RESULT __attribute__((warn_unused_result))
#else
#define MUST_USE_RESULT [[nodiscard]]
#endif

// From util.h
namespace node {

namespace per_process {
// Tells whether the per-process V8::Initialize() is called and
// if it is safe to call v8::Isolate::GetCurrent().
extern bool v8_initialized;
}  // namespace per_process

// The reason that Assert() takes a struct argument instead of individual
// const char*s is to ease instruction cache pressure in calls from CHECK.
struct AssertionInfo {
  const char* file_line;  // filename:line
  const char* message;
  const char* function;
};
[[noreturn]] void Assert(const AssertionInfo& info);

#define ERROR_AND_ABORT(expr)                                                  \
  do {                                                                         \
    /* Make sure that this struct does not end up in inline code, but      */  \
    /* rather in a read-only data section when modifying this code.        */  \
    static const node::AssertionInfo args = {                                  \
        __FILE__ ":" STRINGIFY(__LINE__), #expr, PRETTY_FUNCTION_NAME};        \
    node::Assert(args);                                                        \
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

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (UNLIKELY(!(expr))) {                                                   \
      ERROR_AND_ABORT(expr);                                                   \
    }                                                                          \
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

#define EXIT_CODE_LIST(V)                                                      \
  V(NoFailure, 0)                                                              \
  /* 1 was intended for uncaught JS exceptions from the user land but we */    \
  /* actually use this for all kinds of generic errors. */                     \
  V(GenericUserError, 1)                                                       \
  /* 2 is unused */                                                            \
  /* 3 is actually unused because we pre-compile all builtins during */        \
  /* snapshot building, when we exit with 1 if there's any error.  */          \
  V(InternalJSParseError, 3)                                                   \
  /* 4 is actually unused. We exit with 1 in this case. */                     \
  V(InternalJSEvaluationFailure, 4)                                            \
  /* 5 is actually unused. We exit with 133 (128+SIGTRAP) or 134 */            \
  /* (128+SIGABRT) in this case. */                                            \
  V(V8FatalError, 5)                                                           \
  V(InvalidFatalExceptionMonkeyPatching, 6)                                    \
  V(ExceptionInFatalExceptionHandler, 7)                                       \
  /* 8 is unused */                                                            \
  V(InvalidCommandLineArgument, 9)                                             \
  V(BootstrapFailure, 10)                                                      \
  /* 11 is unused */                                                           \
  /* This was intended for invalid inspector arguments but is actually now */  \
  /* just a duplicate of InvalidCommandLineArgument */                         \
  V(InvalidCommandLineArgument2, 12)                                           \
  V(UnsettledTopLevelAwait, 13)                                                \
  V(StartupSnapshotFailure, 14)                                                \
  /* If the process exits from unhandled signals e.g. SIGABRT, SIGTRAP, */     \
  /* typically the exit codes are 128 + signal number. We also exit with */    \
  /* certain error codes directly for legacy reasons. Here we define those */  \
  /* that are used to normalize the exit code on Windows. */                   \
  V(Abort, 134)

// TODO(joyeecheung): expose this to user land when the codes are stable.
// The underlying type should be an int, or we can get undefined behavior when
// casting error codes into exit codes (technically we shouldn't do that,
// but that's how things have been).
enum class ExitCode : int {
#define V(Name, Code) k##Name = Code,
  EXIT_CODE_LIST(V)
#undef V
};

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
std::unique_ptr<T> static_unique_pointer_cast(std::unique_ptr<U>&& ptr) {
  return std::unique_ptr<T>(static_cast<T*>(ptr.release()));
}

template <typename Fn>
struct OnScopeLeaveImpl {
  Fn fn_;
  bool active_;

  explicit OnScopeLeaveImpl(Fn&& fn) : fn_(std::move(fn)), active_(true) {}
  ~OnScopeLeaveImpl() {
    if (active_) fn_();
  }

  OnScopeLeaveImpl(const OnScopeLeaveImpl& other) = delete;
  OnScopeLeaveImpl& operator=(const OnScopeLeaveImpl& other) = delete;
  OnScopeLeaveImpl(OnScopeLeaveImpl&& other)
      : fn_(std::move(other.fn_)), active_(other.active_) {
    other.active_ = false;
  }
};

// Run a function when exiting the current scope. Used like this:
// auto on_scope_leave = OnScopeLeave([&] {
//   // ... run some code ...
// });
template <typename Fn>
inline MUST_USE_RESULT OnScopeLeaveImpl<Fn> OnScopeLeave(Fn&& fn) {
  return OnScopeLeaveImpl<Fn>{std::move(fn)};
}

}  // namespace node

#endif  // SRC_UTIL_INL_H_
