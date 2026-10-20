#ifndef V8HOST_TESTS_V8HOST_TEST_SUPPORT_H_
#define V8HOST_TESTS_V8HOST_TEST_SUPPORT_H_

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace v8host::test {

struct TestCase {
  const char* suite;
  const char* name;
  std::function<bool(std::string*)> run;
};

std::wstring ExecutableDirectory();
bool WaitForProcessExit(DWORD pid, DWORD timeout_ms);
bool RunProcess(const std::wstring& command, DWORD* exit_code);
int RunTests(int argc, char** argv, const std::vector<TestCase>& tests);

}  // namespace v8host::test

#endif  // V8HOST_TESTS_V8HOST_TEST_SUPPORT_H_
