// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#ifndef NAPI_LIB_MODULES_H_
#define NAPI_LIB_MODULES_H_

#include <functional>
#include <map>
#include <string>

#define DEFINE_TEST_SCRIPT(cppId, script) \
  ::napitest::TestScriptInfo const cppId{script, __FILE__, (__LINE__ - napitest::GetEndOfLineCount(script))};

namespace napitest {

struct TestScriptInfo {
  char const *script;
  char const *file;
  int32_t line;
};

inline int32_t GetEndOfLineCount(char const *script) noexcept {
  return std::count(script, script + strlen(script), '\n');
}

} // namespace napitest

namespace napitest {
namespace module {

extern ::napitest::TestScriptInfo const assert_js;
extern ::napitest::TestScriptInfo const common_js;

inline std::map<std::string, TestScriptInfo, std::less<>>
GetModuleScripts() noexcept {
  std::map<std::string, TestScriptInfo, std::less<>> moduleScripts;
  moduleScripts.try_emplace("assert", assert_js);
  moduleScripts.try_emplace("../../common", common_js);
  return moduleScripts;
}

} // namespace module
} // namespace napitest

#endif // NAPI_LIB_MODULES_H_
