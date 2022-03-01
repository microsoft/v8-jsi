// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"
#include <algorithm>
#include <cstdarg>
#include <fstream>
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

static std::string GetJSModuleText(std::string const &jsModuleCode) {
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
  NODE_API_CALL(env, napi_get_value_string_utf8(env, arg0, moduleName, sizeof(moduleName), &copied));

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

NapiTestException::NapiTestException(napi_env env, napi_status errorCode, const char *expr) noexcept
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
  m_errorInfo = std::make_shared<NapiErrorInfo>();
  napi_valuetype errorType{};
  napi_typeof(env, error, &errorType);
  if (errorType == napi_object) {
    m_errorInfo->Name = GetPropertyString(env, error, "name");
    m_errorInfo->Message = GetPropertyString(env, error, "message");
    m_errorInfo->Stack = GetPropertyString(env, error, "stack");
    if (m_errorInfo->Name == "AssertionError") {
      m_assertionErrorInfo = std::make_shared<NapiAssertionErrorInfo>();
      m_assertionErrorInfo->Method = GetPropertyString(env, error, "method");
      m_assertionErrorInfo->Expected = GetPropertyString(env, error, "expected");
      m_assertionErrorInfo->Actual = GetPropertyString(env, error, "actual");
      m_assertionErrorInfo->SourceFile = GetPropertyString(env, error, "sourceFile");
      m_assertionErrorInfo->SourceLine = GetPropertyInt32(env, error, "sourceLine");
      m_assertionErrorInfo->ErrorStack = GetPropertyString(env, error, "errorStack");
      if (m_assertionErrorInfo->ErrorStack.empty()) {
        m_assertionErrorInfo->ErrorStack = m_errorInfo->Stack;
      }
    }
  } else {
    m_errorInfo->Message = CoerceToString(env, error);
  }
}

/*static*/ napi_value NapiTestException::GetProperty(napi_env env, napi_value obj, char const *name) {
  napi_value result{};
  napi_get_named_property(env, obj, name, &result);
  return result;
}

/*static*/ std::string NapiTestException::GetPropertyString(napi_env env, napi_value obj, char const *name) {
  bool hasProperty{};
  napi_has_named_property(env, obj, name, &hasProperty);
  if (hasProperty) {
    napi_value napiValue = GetProperty(env, obj, name);
    size_t valueSize{};
    napi_get_value_string_utf8(env, napiValue, nullptr, 0, &valueSize);
    std::string value(valueSize, '\0');
    napi_get_value_string_utf8(env, napiValue, &value[0], valueSize + 1, nullptr);
    return value;
  } else {
    return "";
  }
}

/*static*/ int32_t NapiTestException::GetPropertyInt32(napi_env env, napi_value obj, char const *name) {
  napi_value napiValue = GetProperty(env, obj, name);
  int32_t value{};
  napi_get_value_int32(env, napiValue, &value);
  return value;
}

/*static*/ std::string NapiTestException::CoerceToString(napi_env env, napi_value value) {
  napi_value strValue;
  napi_coerce_to_string(env, value, &strValue);
  return ToString(env, strValue);
}

/*static*/ std::string NapiTestException::ToString(napi_env env, napi_value value) {
  size_t valueSize{};
  napi_get_value_string_utf8(env, value, nullptr, 0, &valueSize);
  std::string str(valueSize, '\0');
  napi_get_value_string_utf8(env, value, &str[0], valueSize + 1, nullptr);
  return str;
}

//=============================================================================
// NapiTest implementation
//=============================================================================

NapiTestErrorHandler NapiTest::ExecuteNapi(std::function<void(NapiTestContext *, napi_env)> code) noexcept {
  try {
    const NapiTestData &testData = GetParam();
    napi_env env = testData.EnvFactory();

    {
      auto context = NapiTestContext(env, testData.TestJSPath);
      code(&context, env);
    }

    THROW_IF_NOT_OK(napi_ext_env_unref(env));

    return NapiTestErrorHandler(nullptr, std::exception_ptr(), "", "", 0, 0);
  } catch (...) {
    return NapiTestErrorHandler(nullptr, std::current_exception(), "", "", 0, 0);
  }
}

//=============================================================================
// NapiTestContext implementation
//=============================================================================

NapiTestContext::NapiTestContext(napi_env env, std::string const &testJSPath)
    : env(env),
      m_testJSPath(testJSPath),
      m_envScope(env),
      m_handleScope(env),
      m_scriptModules(GetCommonScripts(testJSPath)) {
  DefineGlobalFunctions();
}

std::map<std::string, TestScriptInfo, std::less<>> NapiTestContext::GetCommonScripts(
    std::string const &testJSPath) noexcept {
  std::map<std::string, TestScriptInfo, std::less<>> moduleScripts;
  moduleScripts.try_emplace(
      "assert", TestScriptInfo{ReadScriptText(testJSPath, "common/assert.js"), "common/assert.js", 1});
  moduleScripts.try_emplace(
      "../../common", TestScriptInfo{ReadScriptText(testJSPath, "common/common.js"), "common/common.js", 1});
  return moduleScripts;
}

napi_value NapiTestContext::RunScript(std::string const &code, char const *sourceUrl) {
  napi_value script{}, scriptResult{};
  THROW_IF_NOT_OK(napi_create_string_utf8(env, code.c_str(), code.size(), &script));
  if (sourceUrl) {
    THROW_IF_NOT_OK(napi_ext_run_script(env, script, sourceUrl, &scriptResult));
  } else {
    THROW_IF_NOT_OK(napi_run_script(env, script, &scriptResult));
  }
  return scriptResult;
}

napi_value NapiTestContext::GetModule(std::string const &moduleName) {
  napi_value result{};
  auto moduleIt = m_modules.find(moduleName);
  if (moduleIt != m_modules.end()) {
    NODE_API_CALL(env, napi_get_reference_value(env, moduleIt->second.get(), &result));
  } else {
    if (moduleName.find("@babel") == 0) {
      std::string scriptFile = moduleName + ".js";
      result = RunScript(GetJSModuleText(ReadScriptText(m_testJSPath, scriptFile)), scriptFile.c_str());
    } else if (moduleName.find("./") == 0 && moduleName.find(".js") != std::string::npos) {
      std::string scriptFile = "@babel/runtime/helpers" + moduleName.substr(1);
      result = RunScript(GetJSModuleText(ReadScriptText(m_testJSPath, scriptFile)), scriptFile.c_str());
    } else {
      auto scriptIt = m_scriptModules.find(moduleName);
      if (scriptIt != m_scriptModules.end()) {
        result = RunScript(GetJSModuleText(scriptIt->second.script), moduleName.c_str());
      } else {
        auto nativeModuleIt = m_nativeModules.find(moduleName);
        if (nativeModuleIt != m_nativeModules.end()) {
          napi_value exports{};
          NODE_API_CALL(env, napi_create_object(env, &exports));
          result = nativeModuleIt->second(env, exports);
        }
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

TestScriptInfo *NapiTestContext::GetTestScriptInfo(std::string const &moduleName) {
  auto it = m_scriptModules.find(moduleName);
  return it != m_scriptModules.end() ? &it->second : nullptr;
}

void NapiTestContext::AddNativeModule(
    char const *moduleName,
    std::function<napi_value(napi_env, napi_value)> initModule) {
  m_nativeModules.try_emplace(moduleName, std::move(initModule));
}

NapiTestErrorHandler NapiTestContext::RunTestScript(char const *script, char const *file, int32_t line) {
  try {
    m_scriptModules["TestScript"] = TestScriptInfo{GetJSModuleText(script).c_str(), file, line};

    NapiHandleScope scope{env};
    {
      NapiHandleScope scope{env};
      RunScript(GetJSModuleText(script).c_str(), "TestScript");
    }
    DrainTaskQueue();
    RunCallChecks();
    HandleUnhandledPromiseRejections();
    return NapiTestErrorHandler(this, std::exception_ptr(), "", file, line, 0);
  } catch (...) {
    return NapiTestErrorHandler(this, std::current_exception(), script, file, line, ModulePrefixLineCount);
  }
}

void NapiTestContext::HandleUnhandledPromiseRejections() {
  bool hasException{false};
  THROW_IF_NOT_OK(napi_ext_has_unhandled_promise_rejection(env, &hasException));
  if (hasException) {
    napi_value error{};
    THROW_IF_NOT_OK(napi_get_and_clear_last_unhandled_promise_rejection(env, &error));
    throw NapiTestException(env, error);
  }
}

NapiTestErrorHandler NapiTestContext::RunTestScript(TestScriptInfo const &scriptInfo) {
  return RunTestScript(scriptInfo.script.c_str(), scriptInfo.file.c_str(), scriptInfo.line);
}

NapiTestErrorHandler NapiTestContext::RunTestScript(std::string const &scriptFile) {
  return RunTestScript(ReadScriptText(m_testJSPath, scriptFile).c_str(), scriptFile.c_str(), 1);
}

std::string NapiTestContext::ReadScriptText(std::string const &testJSPath, std::string const &scriptFile) {
  return ReadFileText(testJSPath + "/" + scriptFile);
}

std::string NapiTestContext::ReadFileText(std::string const &fileName) {
  std::string text;
  std::ifstream fileStream(fileName);
  if (fileStream) {
    std::ostringstream ss;
    ss << fileStream.rdbuf();
    text = ss.str();
  }
  return text;
}

void NapiTestContext::DefineGlobalFunctions() {
  NapiHandleScope scope{env};

  napi_value global{};
  THROW_IF_NOT_OK(napi_get_global(env, &global));

  // Add global
  THROW_IF_NOT_OK(napi_set_named_property(env, global, "global", global));

  // Add require
  napi_value require{};
  THROW_IF_NOT_OK(napi_create_function(env, "require", NAPI_AUTO_LENGTH, JSRequire, this, &require));
  THROW_IF_NOT_OK(napi_set_named_property(env, global, "require", require));

  // Add __NapiTestContext__
  napi_value self{};
  THROW_IF_NOT_OK(napi_create_external(env, this, nullptr, nullptr, &self));
  THROW_IF_NOT_OK(napi_set_named_property(env, global, "__NapiTestContext__", self));

  // Add global.gc()
  napi_value gc{};
  auto gcCallback = [](napi_env env, napi_callback_info /*info*/) -> napi_value {
    NODE_API_CALL(env, napi_ext_collect_garbage(env));

    napi_value undefined{};
    NODE_API_CALL(env, napi_get_undefined(env, &undefined));
    return undefined;
  };

  THROW_IF_NOT_OK(napi_create_function(env, "gc", NAPI_AUTO_LENGTH, gcCallback, nullptr, &gc));
  THROW_IF_NOT_OK(napi_set_named_property(env, global, "gc", gc));

  auto setImmediateCallback = [](napi_env env, napi_callback_info info) -> napi_value {
    size_t argc{1};
    napi_value immediateCallback{};
    NODE_API_CALL(env, napi_get_cb_info(env, info, &argc, &immediateCallback, nullptr, nullptr));

    // TODO: use a different macro that does not throw
    NODE_API_ASSERT(env, argc >= 1, "Wrong number of arguments. Expects at least one argument.");
    napi_valuetype immediateCallbackType;
    NODE_API_CALL(env, napi_typeof(env, immediateCallback, &immediateCallbackType));
    NODE_API_ASSERT(env, immediateCallbackType == napi_function, "Wrong type of arguments. Expects a function.");

    napi_value global{};
    NODE_API_CALL(env, napi_get_global(env, &global));
    napi_value selfValue{};
    NODE_API_CALL(env, napi_get_named_property(env, global, "__NapiTestContext__", &selfValue));
    NapiTestContext *self;
    NODE_API_CALL(env, napi_get_value_external(env, selfValue, (void **)&self));

    uint32_t taskId = self->AddTask(immediateCallback);

    napi_value taskIdValue{};
    NODE_API_CALL(env, napi_create_uint32(env, taskId, &taskIdValue));
    return taskIdValue;
  };

  // Add setImmediate()
  napi_value setImmediate{};
  THROW_IF_NOT_OK(
      napi_create_function(env, "setImmediate", NAPI_AUTO_LENGTH, setImmediateCallback, nullptr, &setImmediate));
  THROW_IF_NOT_OK(napi_set_named_property(env, global, "setImmediate", setImmediate));

  // Add setTimeout()
  napi_value setTimeout{};
  THROW_IF_NOT_OK(
      napi_create_function(env, "setTimeout", NAPI_AUTO_LENGTH, setImmediateCallback, nullptr, &setTimeout));
  THROW_IF_NOT_OK(napi_set_named_property(env, global, "setTimeout", setTimeout));

  auto clearTimeoutCallback = [](napi_env env, napi_callback_info info) -> napi_value {
    size_t argc{1};
    napi_value taskIdValue{};
    NODE_API_CALL(env, napi_get_cb_info(env, info, &argc, &taskIdValue, nullptr, nullptr));

    NODE_API_ASSERT(env, argc >= 1, "Wrong number of arguments. Expects at least one argument.");
    napi_valuetype taskIdType;
    NODE_API_CALL(env, napi_typeof(env, taskIdValue, &taskIdType));
    NODE_API_ASSERT(env, taskIdType == napi_number, "Wrong type of argument. Expects a number.");
    uint32_t taskId;
    NODE_API_CALL(env, napi_get_value_uint32(env, taskIdValue, &taskId));

    napi_value global{};
    NODE_API_CALL(env, napi_get_global(env, &global));
    napi_value selfValue{};
    NODE_API_CALL(env, napi_get_named_property(env, global, "__NapiTestContext__", &selfValue));
    NapiTestContext *self;
    NODE_API_CALL(env, napi_get_value_external(env, selfValue, (void **)&self));

    self->RemoveTask(taskId);

    napi_value undefined{};
    NODE_API_CALL(env, napi_get_undefined(env, &undefined));
    return undefined;
  };

  // Add clearTimeout()
  napi_value clearTimeout{};
  THROW_IF_NOT_OK(
      napi_create_function(env, "clearTimeout", NAPI_AUTO_LENGTH, clearTimeoutCallback, nullptr, &clearTimeout));
  THROW_IF_NOT_OK(napi_set_named_property(env, global, "clearTimeout", clearTimeout));
}

uint32_t NapiTestContext::AddTask(napi_value callback) noexcept {
  uint32_t taskId = m_nextTaskId++;
  m_taskQueue.emplace_back(taskId, MakeNapiRef(env, callback));
  return taskId;
}

void NapiTestContext::RemoveTask(uint32_t taskId) noexcept {
  m_taskQueue.remove_if([taskId](const std::pair<uint32_t, NapiRef> &entry) { return entry.first == taskId; });
}

void NapiTestContext::DrainTaskQueue() {
  while (!m_taskQueue.empty()) {
    std::pair<uint32_t, NapiRef> task = std::move(m_taskQueue.front());
    m_taskQueue.pop_front();
    napi_value callback{}, undefined{};
    THROW_IF_NOT_OK(napi_get_undefined(env, &undefined));
    THROW_IF_NOT_OK(napi_get_reference_value(env, task.second.get(), &callback));
    THROW_IF_NOT_OK(napi_call_function(env, undefined, callback, 0, nullptr, nullptr));
  }
}

void NapiTestContext::RunCallChecks() {
  napi_value common = GetModule("assert");
  napi_value undefined{}, runCallChecks{};
  THROW_IF_NOT_OK(napi_get_named_property(env, common, "runCallChecks", &runCallChecks));
  THROW_IF_NOT_OK(napi_get_undefined(env, &undefined));
  THROW_IF_NOT_OK(napi_call_function(env, undefined, runCallChecks, 0, nullptr, nullptr));
}

std::string NapiTestContext::ProcessStack(std::string const &stack, std::string const &assertMethod) {
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
        if (auto const *scriptInfo = GetTestScriptInfo(locationMatch[1].str())) {
          int32_t cppLine = scriptInfo->line + std::stoi(locationMatch[2].str()) - ModulePrefixLineCount - 1;
          processedFrame = locationMatch.prefix().str() + UseSrcFilePath(scriptInfo->file) + ':' +
              std::to_string(cppLine) + locationMatch.suffix().str();
        }
      }
      processedStack += (!processedFrame.empty() ? processedFrame : frame) + '\n';
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
        if (!ex.ErrorInfo() || ex.ErrorInfo()->Name == m_jsErrorName) {
          m_handler(ex);
          return;
        }
      }

      if (auto assertionError = ex.AssertionErrorInfo()) {
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

        std::string methodName = "assert." + ex.AssertionErrorInfo()->Method;
        std::stringstream errorDetails;
        if (methodName != "assert.fail") {
          errorDetails << " Expected: " << ex.AssertionErrorInfo()->Expected << '\n'
                       << "   Actual: " << ex.AssertionErrorInfo()->Actual << '\n';
        }

        std::string processedStack =
            m_testContext->ProcessStack(ex.AssertionErrorInfo()->ErrorStack, ex.AssertionErrorInfo()->Method);

        GTEST_MESSAGE_AT_(
            m_file.c_str(), sourceLine, "JavaScript assertion error", ::testing::TestPartResult::kFatalFailure)
            << "Exception: " << ex.ErrorInfo()->Name << '\n'
            << "   Method: " << methodName << '\n'
            << "  Message: " << ex.ErrorInfo()->Message << '\n'
            << errorDetails.str(/*a filler for formatting*/) << "     File: " << sourceFile << ":" << sourceLine << '\n'
            << sourceCode << '\n'
            << "Callstack: " << '\n'
            << processedStack /*   a filler for formatting    */
            << "Raw stack: " << '\n'
            << "  " << ex.AssertionErrorInfo()->ErrorStack;
      } else if (ex.ErrorInfo()) {
        GTEST_MESSAGE_AT_(m_file.c_str(), m_line, "JavaScript error", ::testing::TestPartResult::kFatalFailure)
            << "Exception: " << ex.ErrorInfo()->Name << '\n'
            << "  Message: " << ex.ErrorInfo()->Message << '\n'
            << "Callstack: " << ex.ErrorInfo()->Stack;
      } else {
        GTEST_MESSAGE_AT_(m_file.c_str(), m_line, "Test native exception", ::testing::TestPartResult::kFatalFailure)
            << "Exception: NapiTestException\n"
            << "     Code: " << ex.ErrorCode() << '\n'
            << "  Message: " << ex.what() << '\n'
            << "     Expr: " << ex.Expr();
      }
    } catch (std::exception const &ex) {
      GTEST_MESSAGE_AT_(m_file.c_str(), m_line, "C++ exception", ::testing::TestPartResult::kFatalFailure)
          << "Exception thrown: " << ex.what();
    } catch (...) {
      GTEST_MESSAGE_AT_(m_file.c_str(), m_line, "Unexpected test exception", ::testing::TestPartResult::kFatalFailure);
    }
  } else if (m_mustThrow) {
    GTEST_MESSAGE_AT_(
        m_file.c_str(),
        m_line,
        "NapiTestException was expected, but it was not thrown",
        ::testing::TestPartResult::kFatalFailure);
  }
}

void NapiTestErrorHandler::Catch(std::function<void(NapiTestException const &)> &&handler) noexcept {
  m_handler = std::move(handler);
}

void NapiTestErrorHandler::Throws(std::function<void(NapiTestException const &)> &&handler) noexcept {
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

std::string NapiTestErrorHandler::GetSourceCodeSliceForError(int32_t lineIndex, int32_t extraLineCount) noexcept {
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

INSTANTIATE_TEST_CASE_P(NapiEnvs, NapiTest, ::testing::ValuesIn(NapiEnvFactories()));
