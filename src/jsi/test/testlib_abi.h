// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

/// \file testlib_abi.h
/// \brief Helper utilities for ABI runtime testing.

#ifndef TESTLIB_ABI_H
#define TESTLIB_ABI_H

namespace facebook::jsi {

/// Check if the current test is running with the ABI runtime.
/// Use in tests to skip tests not yet supported:
///   if (isAbiRuntime()) GTEST_SKIP() << "Not yet supported in ABI runtime";
bool isAbiRuntime();

} // namespace facebook::jsi

#endif // TESTLIB_ABI_H
