// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#ifndef SANDBOX_DLL_SBOX_LIFECYCLE_TEST_H_
#define SANDBOX_DLL_SBOX_LIFECYCLE_TEST_H_
#if !defined(SBOX_LIFECYCLE_TESTING)
#error Test-only lifecycle fixture
#endif
#include <string>
bool SboxTestReader(bool final_drain, bool suppress_exit, const std::wstring& exe);
bool SboxTestSmokeLifecycle();
bool SboxTestWorkerKindGuard();
#endif
