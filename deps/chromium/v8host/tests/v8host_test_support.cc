#include "v8host_test_support.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <set>

namespace v8host::test {

std::wstring ExecutableDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD size =
      ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(size);
  const size_t separator = path.find_last_of(L"\\/");
  return path.substr(0, separator);
}

bool WaitForProcessExit(DWORD pid, DWORD timeout_ms) {
  HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!process)
    return ::GetLastError() == ERROR_INVALID_PARAMETER;
  const bool exited =
      ::WaitForSingleObject(process, timeout_ms) == WAIT_OBJECT_0;
  ::CloseHandle(process);
  return exited;
}

bool RunProcess(const std::wstring& command, DWORD* exit_code) {
  std::wstring mutable_command = command;
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, ExecutableDirectory().c_str(),
                        &startup, &process))
    return false;
  ::CloseHandle(process.hThread);
  const bool waited =
      ::WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0;
  DWORD code = 0;
  const bool queried = waited && ::GetExitCodeProcess(process.hProcess, &code);
  ::CloseHandle(process.hProcess);
  if (exit_code)
    *exit_code = code;
  return queried;
}

int RunTests(int argc, char** argv, const std::vector<TestCase>& tests) {
  std::set<std::string> suites;
  std::string selected_case;
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--suite=", 8) == 0) {
      std::string value = argv[i] + 8;
      size_t start = 0;
      while (start <= value.size()) {
        size_t comma = value.find(',', start);
        suites.insert(value.substr(start, comma - start));
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
    } else if (std::strncmp(argv[i], "--case=", 7) == 0) {
      selected_case = argv[i] + 7;
    }
  }
  size_t selected = 0;
  size_t failed = 0;
  std::set<std::string> seen_suites;
  for (const TestCase& test : tests) {
    if ((!selected_case.empty() && selected_case != test.name) ||
        (!suites.empty() && !suites.count(test.suite)))
      continue;
    ++selected;
    seen_suites.insert(test.suite);
    std::string detail;
    const bool passed = test.run(&detail);
    printf("%s/%s: %s%s%s\n", test.suite, test.name,
           passed ? "PASS" : "FAIL", detail.empty() ? "" : " - ",
           detail.c_str());
    if (!passed)
      ++failed;
  }
  for (const std::string& suite : seen_suites)
    printf("%s suite: %s\n", suite.c_str(), failed ? "FAIL" : "PASS");
  if (!selected) {
    printf("No tests selected\n");
    return 2;
  }
  return failed ? 1 : 0;
}

}  // namespace v8host::test
