// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "sbox_lifecycle_test.h"
#include "v8host_test_support.h"
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
  if (argc == 2 && std::strncmp(argv[1], "--reader-child=", 15) == 0) {
    HANDLE exit = reinterpret_cast<HANDLE>(std::strtoull(argv[1] + 15, nullptr, 10));
    return ::WaitForSingleObject(exit, 10000) == WAIT_OBJECT_0 ? 0 : 1;
  }
  using namespace v8host::test;
  const auto exe = ExecutableDirectory() + L"\\sbox_lifecycle_tests.exe";
  return RunTests(argc, argv, {
      {"reader", "origin", [&](std::string*) { return SboxTestReader(false, false, exe); }},
      {"reader", "final-drain", [&](std::string*) { return SboxTestReader(true, false, exe); }},
      {"reader", "stop-suppresses-exit", [&](std::string*) { return SboxTestReader(true, true, exe); }},
      {"smoke", "lifecycle-two", [](std::string*) { return SboxTestSmokeLifecycle(); }},
      {"worker", "post-kind-guard", [](std::string*) { return SboxTestWorkerKindGuard(); }},
  });
}
