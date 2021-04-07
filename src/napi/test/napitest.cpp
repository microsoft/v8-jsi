// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"
#include <algorithm>
#include <cstdarg>
#include <iostream>
#include <limits>
#include <regex>
#include <sstream>

extern "C" {
#include "js-native-api/common.c"
}

int test_printf(std::string &output, const char *format, ...) {
  va_list args1;
  va_start(args1, format);
  va_list args2;
  va_copy(args2, args1);
  auto buf = std::string(std::vsnprintf(nullptr, 0, format, args1), '\0');
  va_end(args1);
  std::vsnprintf(&buf[0], buf.size() + 1, format, args2);
  va_end(args2);
  output += buf;
  return buf.size();
}

namespace napitest {

static char const *ModulePrefix = R"(
  'use strict';
  (function(module) {
    const exports = module.exports;)"
                                  "\n";
static char const *ModuleSuffix = R"(
    return module.exports;
  })({exports: {}});)";
static int32_t const ModulePrefixLineCount = GetEndOfLineCount(ModulePrefix);

static std::string GetJSModuleText(const char *jsModuleCode) {
  std::string result;
  result += ModulePrefix;
  result += jsModuleCode;
  result += ModuleSuffix;
  return result;
}

static napi_value JSRequire(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg0;
  void *data;
  NODE_API_CALL(env, napi_get_cb_info(env, info, &argc, &arg0, nullptr, &data));

  NODE_API_ASSERT(env, argc == 1, "Wrong number of arguments");

  // Extract the name of the requested module
  char moduleName[128];
  size_t copied;
  NODE_API_CALL(
      env,
      napi_get_value_string_utf8(
          env, arg0, moduleName, sizeof(moduleName), &copied));

  NapiTestContext *testContext = static_cast<NapiTestContext *>(data);
  return testContext->GetModule(moduleName);
}

static std::string UseSrcFilePath(std::string const &file) {
  std::string const toFind = "build\\v8build\\v8\\jsi";
  size_t pos = file.find(toFind);
  if (pos != std::string::npos) {
    return std::string(file).replace(pos, toFind.length(), "src");
  } else {
    return file;
  }
}

NapiRef MakeNapiRef(napi_env env, napi_value value) {
  napi_ref ref{};
  THROW_IF_NOT_OK(napi_create_reference(env, value, 1, &ref));
  return NapiRef(ref, NapiRefDeleter(env));
}

//=============================================================================
// NapiTestException implementation
//=============================================================================

NapiTestException::NapiTestException(
    napi_env env,
    napi_status errorCode,
    const char *expr) noexcept
    : m_errorCode{errorCode}, m_expr{expr} {
  bool isExceptionPending;
  napi_is_exception_pending(env, &isExceptionPending);
  if (isExceptionPending) {
    napi_value error{};
    if (napi_get_and_clear_last_exception(env, &error) == napi_ok) {
      ApplyScriptErrorData(env, error);
    }
  }
}

NapiTestException::NapiTestException(napi_env env, napi_value error) noexcept {
  ApplyScriptErrorData(env, error);
}

void NapiTestException::ApplyScriptErrorData(napi_env env, napi_value error) {
  m_scriptError = std::make_shared<NapiScriptError>();
  m_scriptError->Name = GetPropertyString(env, error, "name");
  m_scriptError->Message = GetPropertyString(env, error, "message");
  m_scriptError->Stack = GetPropertyString(env, error, "stack");
  if (m_scriptError->Name == "AssertionError") {
    m_assertionError = std::make_shared<NapiAssertionError>();
    m_assertionError->Method = GetPropertyString(env, error, "method");
    m_assertionError->Expected = GetPropertyString(env, error, "expected");
    m_assertionError->Actual = GetPropertyString(env, error, "actual");
    m_assertionError->SourceFile = GetPropertyString(env, error, "sourceFile");
    m_assertionError->SourceLine = GetPropertyInt32(env, error, "sourceLine");
    m_assertionError->ErrorStack = GetPropertyString(env, error, "errorStack");
    if (m_assertionError->ErrorStack.empty()) {
      m_assertionError->ErrorStack = m_scriptError->Stack;
    }
  }
}

/*static*/ napi_value
NapiTestException::GetProperty(napi_env env, napi_value obj, char const *name) {
  napi_value result{};
  napi_get_named_property(env, obj, name, &result);
  return result;
}

/*static*/ std::string NapiTestException::GetPropertyString(
    napi_env env,
    napi_value obj,
    char const *name) {
  napi_value napiValue = GetProperty(env, obj, name);
  size_t valueSize{};
  napi_get_value_string_utf8(env, napiValue, nullptr, 0, &valueSize);
  std::string value(valueSize, '\0');
  napi_get_value_string_utf8(env, napiValue, &value[0], valueSize + 1, nullptr);
  return value;
}

/*static*/ int32_t NapiTestException::GetPropertyInt32(
    napi_env env,
    napi_value obj,
    char const *name) {
  napi_value napiValue = GetProperty(env, obj, name);
  int32_t value{};
  napi_get_value_int32(env, napiValue, &value);
  return value;
}

//=============================================================================
// NapiTest implementation
//=============================================================================

void NapiTest::ExecuteNapi(
    std::function<void(NapiTestContext *, napi_env)> code) noexcept {
  napi_env env = GetParam()();

  {
    auto context = NapiTestContext(env);
    code(&context, env);
  }

  THROW_IF_NOT_OK(napi_ext_delete_env(env));
}

//=============================================================================
// NapiTestContext implementation
//=============================================================================

NapiTestContext::NapiTestContext(napi_env env)
    : env(env), m_scriptModules(module::GetModuleScripts()) {
  THROW_IF_NOT_OK(napi_open_handle_scope(env, &m_handleScope));

  napi_value require{}, global{};
  THROW_IF_NOT_OK(napi_get_global(env, &global));
  THROW_IF_NOT_OK(napi_create_function(
      env, "require", NAPI_AUTO_LENGTH, JSRequire, this, &require));
  THROW_IF_NOT_OK(napi_set_named_property(env, global, "require", require));

  StartTest();
}

NapiTestContext::~NapiTestContext() {
  EndTest();
  napi_close_handle_scope(env, m_handleScope);
}

napi_value NapiTestContext::RunScript(char const *code, char const *sourceUrl) {
  napi_value script{}, scriptResult{};
  THROW_IF_NOT_OK(
      napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &script));
  if (sourceUrl) {
    THROW_IF_NOT_OK(napi_ext_run_script(env, script, sourceUrl, &scriptResult));
  } else {
    THROW_IF_NOT_OK(napi_run_script(env, script, &scriptResult));
  }
  return scriptResult;
}

napi_value NapiTestContext::GetModule(char const *moduleName) {
  napi_value result{};
  auto moduleIt = m_modules.find(moduleName);
  if (moduleIt != m_modules.end()) {
    NODE_API_CALL(
        env, napi_get_reference_value(env, moduleIt->second.get(), &result));
  } else {
    auto scriptIt = m_scriptModules.find(moduleName);
    if (scriptIt != m_scriptModules.end()) {
      result = RunScript(
          GetJSModuleText(scriptIt->second.script).c_str(), moduleName);
    } else {
      auto nativeModuleIt = m_nativeModules.find(moduleName);
      if (nativeModuleIt != m_nativeModules.end()) {
        napi_value exports{};
        napi_create_object(env, &exports);
        result = nativeModuleIt->second(env, exports);
      }
    }

    if (result) {
      m_modules.try_emplace(moduleName, MakeNapiRef(env, result));
    } else {
      NODE_API_CALL(env, napi_get_undefined(env, &result));
    }
  }

  return result;
}

TestScriptInfo *NapiTestContext::GetTestScriptInfo(
    std::string const &moduleName) {
  auto it = m_scriptModules.find(moduleName);
  return it != m_scriptModules.end() ? &it->second : nullptr;
}

void NapiTestContext::AddNativeModule(
    char const *moduleName,
    std::function<napi_value(napi_env, napi_value)> initModule) {
  m_nativeModules.try_emplace(moduleName, std::move(initModule));
}

NapiTestErrorHandler NapiTestContext::RunTestScript(
    char const *script,
    char const *file,
    int32_t line) {
  try {
    m_scriptModules["TestScript"] =
        TestScriptInfo{GetJSModuleText(script).c_str(), file, line};

    RunScript(GetJSModuleText(script).c_str(), "TestScript");
    while (!m_immediateQueue.empty()) {
      NapiRef callbackRef = std::move(m_immediateQueue.front());
      m_immediateQueue.pop();
      napi_value callback{}, undefined{};
      THROW_IF_NOT_OK(napi_get_undefined(env, &undefined));
      THROW_IF_NOT_OK(
          napi_get_reference_value(env, callbackRef.get(), &callback));
      THROW_IF_NOT_OK(
          napi_call_function(env, undefined, callback, 0, nullptr, nullptr));
    }

    RunCallChecks();
    HandleUnhandledPromiseRejections();
    return NapiTestErrorHandler(this, std::exception_ptr(), "", file, line, 0);
  } catch (...) {
    return NapiTestErrorHandler(
        this,
        std::current_exception(),
        script,
        file,
        line,
        ModulePrefixLineCount);
  }
}

void NapiTestContext::HandleUnhandledPromiseRejections() {
  bool hasException{false};
  THROW_IF_NOT_OK(napi_ext_has_unhandled_promise_rejection(env, &hasException));
  if (hasException) {
    napi_value error{};
    THROW_IF_NOT_OK(
        napi_get_and_clear_last_unhandled_promise_rejection(env, &error));
    throw NapiTestException(env, error);
  }
}

NapiTestErrorHandler NapiTestContext::RunTestScript(
    TestScriptInfo const &scriptInfo) {
  return RunTestScript(scriptInfo.script, scriptInfo.file, scriptInfo.line);
}

void NapiTestContext::StartTest() {
  THROW_IF_NOT_OK(napi_ext_open_env_scope(env, &m_envScope));

  // Add global
  napi_value global{};
  THROW_IF_NOT_OK(napi_get_global(env, &global));
  THROW_IF_NOT_OK(napi_set_named_property(env, global, "global", global));

  // Add __NapiTestContext__
  napi_value self{};
  THROW_IF_NOT_OK(napi_create_external(env, this, nullptr, nullptr, &self));
  THROW_IF_NOT_OK(
      napi_set_named_property(env, global, "__NapiTestContext__", self));

  // Add global.gc()
  napi_value gc{};
  auto gcCallback = [](napi_env env,
                       napi_callback_info /*info*/) -> napi_value {
    NODE_API_CALL(env, napi_ext_collect_garbage(env));

    napi_value undefined{};
    NODE_API_CALL(env, napi_get_undefined(env, &undefined));
    return undefined;
  };

  THROW_IF_NOT_OK(napi_create_function(
      env, "gc", NAPI_AUTO_LENGTH, gcCallback, nullptr, &gc));
  THROW_IF_NOT_OK(napi_set_named_property(env, global, "gc", gc));

  // Add setImmediate()
  napi_value setImmediate{};
  auto setImmediateCallback = [](napi_env env,
                                 napi_callback_info info) -> napi_value {
    size_t argc{1};
    napi_value immediateCallback{};
    NODE_API_CALL(
        env,
        napi_get_cb_info(
            env, info, &argc, &immediateCallback, nullptr, nullptr));

    NODE_API_ASSERT(
        env,
        argc >= 1,
        "Wrong number of arguments. Expects at least one argument.");
    napi_valuetype immediateCallbackType;
    NODE_API_CALL(
        env, napi_typeof(env, immediateCallback, &immediateCallbackType));
    NODE_API_ASSERT(
        env,
        immediateCallbackType == napi_function,
        "Wrong type of arguments. Expects a function.");

    napi_value global{};
    THROW_IF_NOT_OK(napi_get_global(env, &global));
    napi_value selfValue{};
    THROW_IF_NOT_OK(napi_get_named_property(
        env, global, "__NapiTestContext__", &selfValue));
    NapiTestContext *self;
    THROW_IF_NOT_OK(napi_get_value_external(env, selfValue, (void **)&self));

    self->SetImmediate(immediateCallback);

    napi_value undefined{};
    NODE_API_CALL(env, napi_get_undefined(env, &undefined));
    return undefined;
  };

  THROW_IF_NOT_OK(napi_create_function(
      env,
      "setImmediate",
      NAPI_AUTO_LENGTH,
      setImmediateCallback,
      nullptr,
      &setImmediate));
  THROW_IF_NOT_OK(
      napi_set_named_property(env, global, "setImmediate", setImmediate));
}

void NapiTestContext::EndTest() {
  THROW_IF_NOT_OK(napi_ext_close_env_scope(env, m_envScope));
}

void NapiTestContext::SetImmediate(napi_value callback) noexcept {
  m_immediateQueue.push(MakeNapiRef(env, callback));
}

void NapiTestContext::RunCallChecks() {
  napi_value common = GetModule("assert");
  napi_value undefined{}, runCallChecks{};
  THROW_IF_NOT_OK(
      napi_get_named_property(env, common, "runCallChecks", &runCallChecks));
  THROW_IF_NOT_OK(napi_get_undefined(env, &undefined));
  THROW_IF_NOT_OK(
      napi_call_function(env, undefined, runCallChecks, 0, nullptr, nullptr));
}

std::string NapiTestContext::ProcessStack(
    std::string const &stack,
    std::string const &assertMethod) {
  // Split up the stack string into an array of stack frames
  auto stackStream = std::istringstream(stack);
  std::string stackFrame;
  std::vector<std::string> stackFrames;
  while (std::getline(stackStream, stackFrame, '\n')) {
    stackFrames.push_back(std::move(stackFrame));
  }

  // Remove first and last stack frames: one is the error message
  // and another is the module root call.
  if (!stackFrames.empty()) {
    stackFrames.pop_back();
  }
  if (!stackFrames.empty()) {
    stackFrames.erase(stackFrames.begin());
  }

  std::string processedStack;
  bool assertFuncFound = false;
  std::string assertFuncPattern = assertMethod + " (";
  const std::regex locationRE("(\\w+):(\\d+)");
  std::smatch locationMatch;
  for (auto const &frame : stackFrames) {
    if (assertFuncFound) {
      std::string processedFrame;
      if (std::regex_search(frame, locationMatch, locationRE)) {
        if (auto const *scriptInfo =
                GetTestScriptInfo(locationMatch[1].str())) {
          int32_t cppLine = scriptInfo->line +
              std::stoi(locationMatch[2].str()) - ModulePrefixLineCount - 1;
          processedFrame = locationMatch.prefix().str() +
              UseSrcFilePath(scriptInfo->file) + ':' + std::to_string(cppLine) +
              locationMatch.suffix().str();
        }
      }
      processedStack +=
          (!processedFrame.empty() ? processedFrame : frame) + '\n';
    } else {
      auto pos = frame.find(assertFuncPattern);
      if (pos != std::string::npos) {
        if (frame[pos - 1] == '.' || frame[pos - 1] == ' ') {
          assertFuncFound = true;
        }
      }
    }
  }

  return processedStack;
}

//=============================================================================
// NapiTestErrorHandler implementation
//=============================================================================

NapiTestErrorHandler::NapiTestErrorHandler(
    NapiTestContext *testContext,
    std::exception_ptr const &exception,
    std::string &&script,
    std::string &&file,
    int32_t line,
    int32_t scriptLineOffset) noexcept
    : m_testContext(testContext),
      m_exception(exception),
      m_script(std::move(script)),
      m_file(std::move(file)),
      m_line(line),
      m_scriptLineOffset(scriptLineOffset) {}

NapiTestErrorHandler::~NapiTestErrorHandler() noexcept {
  if (m_exception) {
    try {
      std::rethrow_exception(m_exception);
    } catch (NapiTestException const &ex) {
      if (m_handler) {
        if (!ex.ScriptError() || ex.ScriptError()->Name == m_jsErrorName) {
          m_handler(ex);
          return;
        }
      }

      if (auto assertionError = ex.AssertionError()) {
        auto sourceFile = assertionError->SourceFile;
        auto sourceLine = assertionError->SourceLine - m_scriptLineOffset;
        auto sourceCode = std::string("<Source is unavailable>");
        if (sourceFile == "TestScript") {
          sourceFile = UseSrcFilePath(m_file);
          sourceCode = GetSourceCodeSliceForError(sourceLine, 2);
          sourceLine += m_line - 1;
        } else if (sourceFile.empty()) {
          sourceFile = "<Unknown>";
        }

        std::string methodName = "assert." + ex.AssertionError()->Method;
        std::stringstream errorDetails;
        if (methodName != "assert.fail") {
          errorDetails << " Expected: " << ex.AssertionError()->Expected << '\n'
                       << "   Actual: " << ex.AssertionError()->Actual << '\n';
        }

        std::string processedStack = m_testContext->ProcessStack(
            ex.AssertionError()->ErrorStack, ex.AssertionError()->Method);

        GTEST_MESSAGE_AT_(
            m_file.c_str(),
            sourceLine,
            "JavaScript assertion error",
            ::testing::TestPartResult::kFatalFailure)
            << "Exception: " << ex.ScriptError()->Name << '\n'
            << "   Method: " << methodName << '\n'
            << "  Message: " << ex.ScriptError()->Message << '\n'
            << errorDetails.str(/*a filler for formatting*/)
            << "     File: " << sourceFile << ":" << sourceLine << '\n'
            << sourceCode << '\n'
            << "Callstack: " << '\n'
            << processedStack /*   a filler for formatting    */
            << "Raw stack: " << '\n'
            << "  " << ex.AssertionError()->ErrorStack;
      } else if (ex.ScriptError()) {
        GTEST_MESSAGE_AT_(
            m_file.c_str(),
            m_line,
            "JavaScript error",
            ::testing::TestPartResult::kFatalFailure)
            << "Exception: " << ex.ScriptError()->Name << '\n'
            << "  Message: " << ex.ScriptError()->Message << '\n'
            << "Callstack: " << ex.ScriptError()->Stack;
      } else {
        GTEST_MESSAGE_AT_(
            m_file.c_str(),
            m_line,
            "Test native exception",
            ::testing::TestPartResult::kFatalFailure)
            << "Exception: NapiTestException\n"
            << "     Code: " << ex.ErrorCode() << '\n'
            << "  Message: " << ex.what() << '\n'
            << "     Expr: " << ex.Expr();
      }
    } catch (std::exception const &ex) {
      GTEST_MESSAGE_AT_(
          m_file.c_str(),
          m_line,
          "C++ exception",
          ::testing::TestPartResult::kFatalFailure)
          << "Exception thrown: " << ex.what();
    } catch (...) {
      GTEST_MESSAGE_AT_(
          m_file.c_str(),
          m_line,
          "Unexpected test exception",
          ::testing::TestPartResult::kFatalFailure);
    }
  } else if (m_mustThrow) {
    GTEST_MESSAGE_AT_(
        m_file.c_str(),
        m_line,
        "NapiTestException was expected, but it was not thrown",
        ::testing::TestPartResult::kFatalFailure);
  }
}

void NapiTestErrorHandler::Catch(
    std::function<void(NapiTestException const &)> &&handler) noexcept {
  m_handler = std::move(handler);
}

void NapiTestErrorHandler::Throws(
    std::function<void(NapiTestException const &)> &&handler) noexcept {
  m_handler = std::move(handler);
  m_mustThrow = true;
}

void NapiTestErrorHandler::Throws(
    char const *jsErrorName,
    std::function<void(NapiTestException const &)> &&handler) noexcept {
  m_jsErrorName = jsErrorName;
  m_handler = std::move(handler);
  m_mustThrow = true;
}

std::string NapiTestErrorHandler::GetSourceCodeSliceForError(
    int32_t lineIndex,
    int32_t extraLineCount) noexcept {
  std::string sourceCode;
  auto sourceStream = std::istringstream(m_script + '\n');
  std::string sourceLine;
  int32_t currentLineIndex = 1; // The line index is 1-based.

  while (std::getline(sourceStream, sourceLine, '\n')) {
    if (currentLineIndex > lineIndex + extraLineCount)
      break;
    if (currentLineIndex >= lineIndex - extraLineCount) {
      sourceCode += currentLineIndex == lineIndex ? "===> " : "     ";
      sourceCode += sourceLine;
      sourceCode += "\n";
    }
    ++currentLineIndex;
  }

  return sourceCode;
}

} // namespace napitest

using namespace napitest;

INSTANTIATE_TEST_SUITE_P(
    NapiEnv,
    NapiTest,
    ::testing::ValuesIn(NapiEnvFactories()));
