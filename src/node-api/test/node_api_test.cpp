// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "node_api_test.h"
#include <js_runtime_api.h>
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <regex>
#include <sstream>
#include "child_process.h"

namespace fs = std::filesystem;

namespace node_api_tests {

// Use to override printf in tests to send output to a std::string instead of
// stdout.
int test_printf(std::string& output, const char* format, ...) {
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

std::string replaceAll(std::string&& str,
                       std::string_view from,
                       std::string_view to) {
  std::string result = std::move(str);
  if (from.empty()) return result;
  size_t start_pos = 0;
  while ((start_pos = result.find(from, start_pos)) != std::string::npos) {
    result.replace(start_pos, from.length(), to);
    start_pos += to.length();  // In case 'to' contains 'from', like replacing
                               // 'x' with 'yx'
  }
  return result;
}

static char const* ModulePrefix = R"(
  'use strict';
  (function(module) {
    let exports = module.exports;
    const __filename = module.filename;
    const __dirname = module.path;)"
                                  "\n";
static char const* ModuleSuffix = R"(
    return module.exports;
  })({exports: {}, filename: "%s", path: "%s"});)";
static int32_t const ModulePrefixLineCount = GetEndOfLineCount(ModulePrefix);

static std::string GetJSModuleText(std::string const& jsModuleCode,
                                   fs::path const& jsModulePath) {
  std::string result;
  result += ModulePrefix;
  result += jsModuleCode;
  test_printf(
      result,
      ModuleSuffix,
      replaceAll(jsModulePath.string(), "\\", "\\\\").c_str(),
      replaceAll(jsModulePath.parent_path().string(), "\\", "\\\\").c_str());
  return result;
}

static napi_value JSRequire(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value arg0;
  void* data;
  NODE_API_CALL(env, napi_get_cb_info(env, info, &argc, &arg0, nullptr, &data));

  NODE_API_ASSERT(env, argc == 1, "Wrong number of arguments");

  // Extract the name of the requested module
  char moduleName[128];
  size_t copied;
  NODE_API_CALL(env,
                napi_get_value_string_utf8(
                    env, arg0, moduleName, sizeof(moduleName), &copied));

  NodeApiTestContext* testContext = static_cast<NodeApiTestContext*>(data);
  return testContext->GetModule(moduleName);
}

static std::string UseSrcFilePath(std::string const& file) {
  std::string const toFind = "build\\v8build\\v8\\jsi";
  size_t pos = file.find(toFind);
  if (pos != std::string::npos) {
    return std::string(file).replace(pos, toFind.length(), "src");
  } else {
    return file;
  }
}

NodeApiRef MakeNodeApiRef(napi_env env, napi_value value) {
  napi_ref ref{};
  THROW_IF_NOT_OK(napi_create_reference(env, value, 1, &ref));
  return NodeApiRef(ref, NodeApiRefDeleter(env));
}

//=============================================================================
// NodeApiTestException implementation
//=============================================================================

NodeApiTestException::NodeApiTestException(napi_env env,
                                           napi_status errorCode,
                                           const char* expr) noexcept
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

NodeApiTestException::NodeApiTestException(napi_env env,
                                           napi_value error) noexcept {
  ApplyScriptErrorData(env, error);
}

void NodeApiTestException::ApplyScriptErrorData(napi_env env,
                                                napi_value error) {
  m_errorInfo = std::make_shared<NodeApiErrorInfo>();
  napi_valuetype errorType{};
  napi_typeof(env, error, &errorType);
  if (errorType == napi_object) {
    m_errorInfo->Name = GetPropertyString(env, error, "name");
    m_errorInfo->Message = GetPropertyString(env, error, "message");
    m_errorInfo->Stack = GetPropertyString(env, error, "stack");
    if (m_errorInfo->Name == "AssertionError") {
      m_assertionErrorInfo = std::make_shared<NodeApiAssertionErrorInfo>();
      m_assertionErrorInfo->Method = GetPropertyString(env, error, "method");
      m_assertionErrorInfo->Expected =
          GetPropertyString(env, error, "expected");
      m_assertionErrorInfo->Actual = GetPropertyString(env, error, "actual");
      m_assertionErrorInfo->SourceFile =
          GetPropertyString(env, error, "sourceFile");
      m_assertionErrorInfo->SourceLine =
          GetPropertyInt32(env, error, "sourceLine");
      m_assertionErrorInfo->ErrorStack =
          GetPropertyString(env, error, "errorStack");
      if (m_assertionErrorInfo->ErrorStack.empty()) {
        m_assertionErrorInfo->ErrorStack = m_errorInfo->Stack;
      }
    }
  } else {
    m_errorInfo->Message = CoerceToString(env, error);
  }
}

/*static*/ napi_value NodeApiTestException::GetProperty(napi_env env,
                                                        napi_value obj,
                                                        char const* name) {
  napi_value result{};
  napi_get_named_property(env, obj, name, &result);
  return result;
}

/*static*/ std::string NodeApiTestException::GetPropertyString(
    napi_env env, napi_value obj, char const* name) {
  bool hasProperty{};
  napi_has_named_property(env, obj, name, &hasProperty);
  if (hasProperty) {
    napi_value napiValue = GetProperty(env, obj, name);
    size_t valueSize{};
    napi_get_value_string_utf8(env, napiValue, nullptr, 0, &valueSize);
    std::string value(valueSize, '\0');
    napi_get_value_string_utf8(
        env, napiValue, &value[0], valueSize + 1, nullptr);
    return value;
  } else {
    return "";
  }
}

/*static*/ int32_t NodeApiTestException::GetPropertyInt32(napi_env env,
                                                          napi_value obj,
                                                          char const* name) {
  napi_value napiValue = GetProperty(env, obj, name);
  int32_t value{};
  napi_get_value_int32(env, napiValue, &value);
  return value;
}

/*static*/ std::string NodeApiTestException::CoerceToString(napi_env env,
                                                            napi_value value) {
  napi_value strValue;
  napi_coerce_to_string(env, value, &strValue);
  return ToString(env, strValue);
}

/*static*/ std::string NodeApiTestException::ToString(napi_env env,
                                                      napi_value value) {
  size_t valueSize{};
  napi_get_value_string_utf8(env, value, nullptr, 0, &valueSize);
  std::string str(valueSize, '\0');
  napi_get_value_string_utf8(env, value, &str[0], valueSize + 1, nullptr);
  return str;
}

//=============================================================================
// NodeApiTest implementation
//=============================================================================

std::unique_ptr<IEnvHolder> CreateEnvHolder(
    std::shared_ptr<NodeApiTaskRunner> taskRunner);

int EvaluateJSFile(int argc, char** argv) {
  // Convert arguments to vector of strings and skip all options before the JS
  // file name.
  std::vector<std::string> args;
  args.reserve(argc);
  bool skipOptions = true;
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <js_file>" << std::endl;
    return 1;
  }
  args.push_back(argv[0]);
  for (int i = 1; i < argc; i++) {
    if (skipOptions && std::string_view(argv[i]).find("--") == 0) {
      continue;
    }
    skipOptions = false;
    args.push_back(argv[i]);
  }

  try {
    std::shared_ptr<NodeApiTaskRunner> taskRunner =
        std::make_shared<NodeApiTaskRunner>();
    std::unique_ptr<IEnvHolder> envHolder = CreateEnvHolder(taskRunner);
    napi_env env = envHolder->getEnv();

    std::string jsFilePath = args[1];
    fs::path jsPath = fs::path(jsFilePath);
    fs::path jsRootDir = jsPath.parent_path().parent_path();
    {
      NodeApiTestContext context(
          env, taskRunner, jsRootDir.string(), std::move(args));
      return context.RunTestScript(jsFilePath).HandleAtProcessExit();
    }

    // return NodeApiTestErrorHandler(nullptr, std::exception_ptr(), "", "", 0,
    // 0);
  } catch (...) {
    // return NodeApiTestErrorHandler(nullptr, std::current_exception(), "", "",
    // 0, 0);
  }
  return 0;
}

//=============================================================================
// NodeApiTestContext implementation
//=============================================================================

NodeApiTestContext::NodeApiTestContext(
    napi_env env,
    std::shared_ptr<NodeApiTaskRunner> taskRunner,
    std::string const& testJSPath,
    std::vector<std::string> argv)
    : env(env),
      m_testJSPath(testJSPath),
      m_envScope(env),
      m_handleScope(env),
      m_taskRunner(std::move(taskRunner)),
      m_scriptModules(GetCommonScripts(testJSPath)),
      m_argv(std::move(argv)) {
  DefineGlobalFunctions();
  DefineChildProcessModule();
}

std::map<std::string, TestScriptInfo, std::less<>>
NodeApiTestContext::GetCommonScripts(std::string const& testJSPath) noexcept {
  std::map<std::string, TestScriptInfo, std::less<>> moduleScripts;
  moduleScripts.try_emplace(
      "assert",
      TestScriptInfo{ReadScriptText(testJSPath, "common/assert.js"),
                     "common/assert.js",
                     1});
  moduleScripts.try_emplace(
      "../../common",
      TestScriptInfo{ReadScriptText(testJSPath, "common/common.js"),
                     "common/common.js",
                     1});
  return moduleScripts;
}

napi_value NodeApiTestContext::RunScript(std::string const& code,
                                         char const* sourceUrl) {
  napi_value script{}, scriptResult{};
  THROW_IF_NOT_OK(
      napi_create_string_utf8(env, code.c_str(), code.size(), &script));
  if (sourceUrl) {
    THROW_IF_NOT_OK(jsr_run_script(env, script, sourceUrl, &scriptResult));
  } else {
    THROW_IF_NOT_OK(napi_run_script(env, script, &scriptResult));
  }
  return scriptResult;
}

using ModuleRegisterFuncCallback = napi_value(NAPI_CDECL*)(napi_env env,
                                                           napi_value exports);
using ModuleApiVersionCallback = int32_t(NAPI_CDECL*)();

napi_value NodeApiTestContext::GetModule(std::string const& moduleName) {
  napi_value result{};

  // Check if the module has already been initialized.
  auto moduleIt = m_initializedModules.find(moduleName);
  if (moduleIt != m_initializedModules.end()) {
    NODE_API_CALL(
        env, napi_get_reference_value(env, moduleIt->second.get(), &result));
    return result;
  }

  auto registerModule = [this](napi_env env,
                               std::string const& moduleName,
                               napi_value module) {
    m_initializedModules.try_emplace(moduleName, MakeNodeApiRef(env, module));
    return module;
  };

  // Check if the module is registered script module.
  auto scriptIt = m_scriptModules.find(moduleName);
  if (scriptIt != m_scriptModules.end()) {
    return registerModule(env,
                          moduleName,
                          RunScript(GetJSModuleText(scriptIt->second.script,
                                                    scriptIt->second.filePath),
                                    moduleName.c_str()));
  }

  // Check if the module is registered native module.
  auto nativeModuleIt = m_nativeModules.find(moduleName);
  if (nativeModuleIt != m_nativeModules.end()) {
    napi_value exports{};
    NODE_API_CALL(env, napi_create_object(env, &exports));
    return registerModule(
        env, moduleName, nativeModuleIt->second(env, exports));
  }

  // Check if it is a native module.
  const char nativeModulePrefix[] = "./build/x86/";
  if (moduleName.find(nativeModulePrefix) == 0) {
    std::string dllName = moduleName.substr(std::size(nativeModulePrefix) - 1);
    HMODULE dllModule = ::LoadLibraryA(dllName.c_str());
    if (dllModule != NULL) {
      ModuleRegisterFuncCallback moduleRegisterFunc =
          reinterpret_cast<ModuleRegisterFuncCallback>(
              ::GetProcAddress(dllModule, "napi_register_module_v1"));
      ModuleApiVersionCallback getModuleApiVersion =
          reinterpret_cast<ModuleApiVersionCallback>(::GetProcAddress(
              dllModule, "node_api_module_get_api_version_v1"));
      if (moduleRegisterFunc != nullptr) {
        int32_t moduleApiVersion =
            getModuleApiVersion ? getModuleApiVersion() : 8;
        napi_env moduleEnv{};
        NODE_API_CALL(
            env, jsr_create_node_api_env(env, moduleApiVersion, &moduleEnv));
        napi_value exports{};
        NODE_API_CALL(moduleEnv, napi_create_object(moduleEnv, &exports));

        auto task = [moduleEnv, moduleRegisterFunc, &exports]() {
          exports = moduleRegisterFunc(moduleEnv, exports);
        };
        using Task = decltype(task);
        NODE_API_CALL(
            moduleEnv,
            jsr_run_task(
                moduleEnv,
                [](void* data) { (*reinterpret_cast<Task*>(data))(); },
                &task));

        return registerModule(moduleEnv, moduleName, exports);
      }
    }
  }

  // Check if it is a script module.
  if (moduleName.find("@babel") == 0) {
    std::string scriptFile = moduleName + ".js";
    fs::path scriptPath = fs::path(m_testJSPath) / scriptFile;
    return registerModule(
        env,
        moduleName,
        RunScript(GetJSModuleText(ReadScriptText(m_testJSPath, scriptFile),
                                  scriptPath),
                  scriptFile.c_str()));
  } else if (moduleName.find("./") == 0 &&
             moduleName.find(".js") != std::string::npos) {
    std::string scriptFile = "@babel/runtime/helpers" + moduleName.substr(1);
    fs::path scriptPath = fs::path(m_testJSPath) / scriptFile;
    return registerModule(
        env,
        moduleName,
        RunScript(GetJSModuleText(ReadScriptText(m_testJSPath, scriptFile),
                                  scriptPath),
                  scriptFile.c_str()));
  }

  NODE_API_CALL(env, napi_get_undefined(env, &result));
  return result;
}

TestScriptInfo* NodeApiTestContext::GetTestScriptInfo(
    std::string const& moduleName) {
  auto it = m_scriptModules.find(moduleName);
  return it != m_scriptModules.end() ? &it->second : nullptr;
}

void NodeApiTestContext::AddNativeModule(
    char const* moduleName,
    std::function<napi_value(napi_env, napi_value)> initModule) {
  m_nativeModules.try_emplace(moduleName, std::move(initModule));
}

NodeApiTestErrorHandler NodeApiTestContext::RunTestScript(char const* script,
                                                          char const* file,
                                                          int32_t line) {
  try {
    std::string scriptText = GetJSModuleText(script, file);
    m_scriptModules["TestScript"] =
        TestScriptInfo{scriptText.c_str(), file, line};

    NodeApiHandleScope scope{env};
    {
      NodeApiHandleScope scope{env};
      RunScript(scriptText.c_str(), "TestScript");
    }
    DrainTaskQueue();
    RunCallChecks();
    HandleUnhandledPromiseRejections();
    return NodeApiTestErrorHandler(
        this, std::exception_ptr(), "", file, line, 0);
  } catch (...) {
    return NodeApiTestErrorHandler(this,
                                   std::current_exception(),
                                   script,
                                   file,
                                   line,
                                   ModulePrefixLineCount);
  }
}

void NodeApiTestContext::HandleUnhandledPromiseRejections() {
  bool hasException{false};
  THROW_IF_NOT_OK(jsr_has_unhandled_promise_rejection(env, &hasException));
  if (hasException) {
    napi_value error{};
    THROW_IF_NOT_OK(
        jsr_get_and_clear_last_unhandled_promise_rejection(env, &error));
    throw NodeApiTestException(env, error);
  }
}

NodeApiTestErrorHandler NodeApiTestContext::RunTestScript(
    TestScriptInfo const& scriptInfo) {
  return RunTestScript(scriptInfo.script.c_str(),
                       scriptInfo.filePath.string().c_str(),
                       scriptInfo.line);
}

NodeApiTestErrorHandler NodeApiTestContext::RunTestScript(
    std::string const& scriptFile) {
  return RunTestScript(ReadFileText(scriptFile).c_str(), scriptFile.c_str(), 1);
}

std::string NodeApiTestContext::ReadScriptText(std::string const& testJSPath,
                                               std::string const& scriptFile) {
  return ReadFileText(testJSPath + "/" + scriptFile);
}

std::string NodeApiTestContext::ReadFileText(std::string const& fileName) {
  std::string text;
  std::ifstream fileStream(fileName);
  if (fileStream) {
    std::ostringstream ss;
    ss << fileStream.rdbuf();
    text = ss.str();
  }
  return text;
}

void NodeApiTestContext::DefineObjectMethod(napi_value obj,
                                            char const* funcName,
                                            napi_callback cb) {
  napi_value func{};
  THROW_IF_NOT_OK(
      napi_create_function(env, funcName, NAPI_AUTO_LENGTH, cb, this, &func));
  THROW_IF_NOT_OK(napi_set_named_property(env, obj, funcName, func));
}

// global.require("module_name")
void NodeApiTestContext::DefineGlobalRequire(napi_value global) {
  DefineObjectMethod(global, "require", JSRequire);
}

// global.gc()
void NodeApiTestContext::DefineGlobalGC(napi_value global) {
  DefineObjectMethod(
      global,
      "gc",
      [](napi_env env, napi_callback_info /*info*/) -> napi_value {
        NODE_API_CALL(env, jsr_collect_garbage(env));
        return nullptr;
      });
}

static napi_value NAPI_CDECL SetImmediateCallback(napi_env env,
                                                  napi_callback_info info) {
  size_t argc{1};
  napi_value immediateCallback{};
  NODE_API_CALL(
      env,
      napi_get_cb_info(env, info, &argc, &immediateCallback, nullptr, nullptr));

  // TODO: use a different macro that does not throw
  NODE_API_ASSERT(env,
                  argc >= 1,
                  "Wrong number of arguments. Expects at least one argument.");
  napi_valuetype immediateCallbackType;
  NODE_API_CALL(env,
                napi_typeof(env, immediateCallback, &immediateCallbackType));
  NODE_API_ASSERT(env,
                  immediateCallbackType == napi_function,
                  "Wrong type of arguments. Expects a function.");

  napi_value global{};
  NODE_API_CALL(env, napi_get_global(env, &global));
  napi_value selfValue{};
  NODE_API_CALL(env,
                napi_get_named_property(
                    env, global, "__NodeApiTestContext__", &selfValue));
  NodeApiTestContext* self;
  NODE_API_CALL(env, napi_get_value_external(env, selfValue, (void**)&self));

  uint32_t taskId = self->AddTask(immediateCallback);

  napi_value taskIdValue{};
  NODE_API_CALL(env, napi_create_uint32(env, taskId, &taskIdValue));
  return taskIdValue;
}

// global.setImmediate()
void NodeApiTestContext::DefineGlobalSetImmediate(napi_value global) {
  DefineObjectMethod(global, "setImmediate", SetImmediateCallback);
}

// global.setTimeout()
void NodeApiTestContext::DefineGlobalSetTimeout(napi_value global) {
  DefineObjectMethod(global, "setTimeout", SetImmediateCallback);
}

// global.clearTimeout()
void NodeApiTestContext::DefineGlobalClearTimeout(napi_value global) {
  DefineObjectMethod(
      global,
      "clearTimeout",
      [](napi_env env, napi_callback_info info) -> napi_value {
        size_t argc{1};
        napi_value taskIdValue{};
        NODE_API_CALL(
            env,
            napi_get_cb_info(env, info, &argc, &taskIdValue, nullptr, nullptr));

        NODE_API_ASSERT(
            env,
            argc >= 1,
            "Wrong number of arguments. Expects at least one argument.");
        napi_valuetype taskIdType;
        NODE_API_CALL(env, napi_typeof(env, taskIdValue, &taskIdType));
        NODE_API_ASSERT(env,
                        taskIdType == napi_number,
                        "Wrong type of argument. Expects a number.");
        uint32_t taskId;
        NODE_API_CALL(env, napi_get_value_uint32(env, taskIdValue, &taskId));

        napi_value global{};
        NODE_API_CALL(env, napi_get_global(env, &global));
        napi_value selfValue{};
        NODE_API_CALL(env,
                      napi_get_named_property(
                          env, global, "__NodeApiTestContext__", &selfValue));
        NodeApiTestContext* self;
        NODE_API_CALL(env,
                      napi_get_value_external(env, selfValue, (void**)&self));

        self->RemoveTask(taskId);

        napi_value undefined{};
        NODE_API_CALL(env, napi_get_undefined(env, &undefined));
        return undefined;
      });
}

static std::string ToStdString(napi_env env, napi_value value) {
  napi_valuetype valueType;
  THROW_IF_NOT_OK(napi_typeof(env, value, &valueType));
  NODE_API_ASSERT(env,
                  valueType == napi_string,
                  "Wrong type of argument. Expects a string.");
  size_t valueSize{};
  napi_get_value_string_utf8(env, value, nullptr, 0, &valueSize);
  std::string str(valueSize, '\0');
  napi_get_value_string_utf8(env, value, &str[0], valueSize + 1, nullptr);
  return str;
}

static std::vector<std::string> ToStdStringArray(napi_env env,
                                                 napi_value value) {
  std::vector<std::string> result;
  bool isArray;
  THROW_IF_NOT_OK(napi_is_array(env, value, &isArray));
  if (isArray) {
    uint32_t length;
    THROW_IF_NOT_OK(napi_get_array_length(env, value, &length));
    result.reserve(length);
    for (uint32_t i = 0; i < length; i++) {
      napi_value element;
      THROW_IF_NOT_OK(napi_get_element(env, value, i, &element));
      result.push_back(ToStdString(env, element));
    }
  }
  return result;
}

static NodeApiTestContext* GetTestContext(napi_env env) {
  napi_value global{};
  NODE_API_CALL(env, napi_get_global(env, &global));
  napi_value contextValue{};
  NODE_API_CALL(env,
                napi_get_named_property(
                    env, global, "__NodeApiTestContext__", &contextValue));
  NodeApiTestContext* context{};
  NODE_API_CALL(env,
                napi_get_value_external(env, contextValue, (void**)&context));
  return context;
}

// global.process
void NodeApiTestContext::DefineGlobalProcess(napi_value global) {
  napi_value processObject{};
  THROW_IF_NOT_OK(napi_create_object(env, &processObject));
  THROW_IF_NOT_OK(
      napi_set_named_property(env, global, "process", processObject));

  // process.argv
  napi_value argvArray{};
  THROW_IF_NOT_OK(napi_create_array(env, &argvArray));
  THROW_IF_NOT_OK(
      napi_set_named_property(env, processObject, "argv", argvArray));

  uint32_t index = 0;
  for (const std::string& arg : m_argv) {
    napi_value argValue{};
    THROW_IF_NOT_OK(
        napi_create_string_utf8(env, arg.c_str(), arg.size(), &argValue));
    THROW_IF_NOT_OK(napi_set_element(env, argvArray, index++, argValue));
  }

  // process.execPath
  napi_value execPath{};
  THROW_IF_NOT_OK(napi_create_string_utf8(
      env, m_argv[0].c_str(), m_argv[0].size(), &execPath));
  THROW_IF_NOT_OK(
      napi_set_named_property(env, processObject, "execPath", execPath));
}

void NodeApiTestContext::DefineChildProcessModule() {
  AddNativeModule("child_process", [this](napi_env env, napi_value exports) {
    DefineObjectMethod(
        exports,
        "spawnSync",
        [](napi_env env, napi_callback_info info) -> napi_value {
          size_t argc{2};
          napi_value argv[2] = {};
          NODE_API_CALL(
              env, napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr));

          NODE_API_ASSERT(
              env,
              argc >= 1,
              "Wrong number of arguments. Expects at least one argument.");
          std::string command = ToStdString(env, argv[0]);
          std::vector<std::string> args = ToStdStringArray(env, argv[1]);

          NodeApiTestContext* self = GetTestContext(env);
          return self->SpawnSync(command, args);
        });
    return exports;
  });
}

void NodeApiTestContext::DefineGlobalFunctions() {
  NodeApiHandleScope scope{env};

  napi_value global{};
  THROW_IF_NOT_OK(napi_get_global(env, &global));

  // Add global
  THROW_IF_NOT_OK(napi_set_named_property(env, global, "global", global));

  // Add __NodeApiTestContext__
  napi_value self{};
  THROW_IF_NOT_OK(napi_create_external(env, this, nullptr, nullptr, &self));
  THROW_IF_NOT_OK(
      napi_set_named_property(env, global, "__NodeApiTestContext__", self));

  DefineGlobalRequire(global);
  DefineGlobalGC(global);
  DefineGlobalSetImmediate(global);
  DefineGlobalSetTimeout(global);
  DefineGlobalClearTimeout(global);
  DefineGlobalProcess(global);
}

static void SetUIntProperty(napi_env env,
                            napi_value obj,
                            char const* name,
                            uint32_t value) {
  napi_value valueObj{};
  THROW_IF_NOT_OK(napi_create_uint32(env, value, &valueObj));
  THROW_IF_NOT_OK(napi_set_named_property(env, obj, name, valueObj));
}

static void SetStrProperty(napi_env env,
                           napi_value obj,
                           char const* name,
                           std::string const& value) {
  napi_value valueObj{};
  THROW_IF_NOT_OK(
      napi_create_string_utf8(env, value.c_str(), value.size(), &valueObj));
  THROW_IF_NOT_OK(napi_set_named_property(env, obj, name, valueObj));
}

static void SetNullProperty(napi_env env, napi_value obj, char const* name) {
  napi_value valueObj{};
  THROW_IF_NOT_OK(napi_get_null(env, &valueObj));
  THROW_IF_NOT_OK(napi_set_named_property(env, obj, name, valueObj));
}

napi_value NodeApiTestContext::SpawnSync(std::string command,
                                         std::vector<std::string> args) {
  ProcessResult procResult = spawnSync(command, args);
  napi_value result{};
  THROW_IF_NOT_OK(napi_create_object(env, &result));
  SetUIntProperty(env, result, "status", procResult.status);
  SetStrProperty(env, result, "stderr", procResult.std_error);
  SetStrProperty(env, result, "stdout", procResult.std_output);
  SetNullProperty(env, result, "signal");
  return result;
}

uint32_t NodeApiTestContext::AddTask(napi_value callback) noexcept {
  std::shared_ptr<NodeApiRef> ref =
      std::make_shared<NodeApiRef>(MakeNodeApiRef(env, callback));
  return m_taskRunner->PostTask([env = this->env, ref = std::move(ref)]() {
    napi_value callback{}, undefined{};
    THROW_IF_NOT_OK(napi_get_undefined(env, &undefined));
    THROW_IF_NOT_OK(napi_get_reference_value(env, ref->get(), &callback));
    THROW_IF_NOT_OK(
        napi_call_function(env, undefined, callback, 0, nullptr, nullptr));
  });
}

void NodeApiTestContext::RemoveTask(uint32_t taskId) noexcept {
  m_taskRunner->RemoveTask(taskId);
}

void NodeApiTestContext::DrainTaskQueue() {
  m_taskRunner->DrainTaskQueue();
}

void NodeApiTestContext::RunCallChecks() {
  napi_value common = GetModule("assert");
  napi_value undefined{}, runCallChecks{};
  THROW_IF_NOT_OK(
      napi_get_named_property(env, common, "runCallChecks", &runCallChecks));
  THROW_IF_NOT_OK(napi_get_undefined(env, &undefined));
  THROW_IF_NOT_OK(
      napi_call_function(env, undefined, runCallChecks, 0, nullptr, nullptr));
}

std::string NodeApiTestContext::ProcessStack(std::string const& stack,
                                             std::string const& assertMethod) {
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
  for (auto const& frame : stackFrames) {
    if (assertFuncFound) {
      std::string processedFrame;
      if (std::regex_search(frame, locationMatch, locationRE)) {
        if (auto const* scriptInfo =
                GetTestScriptInfo(locationMatch[1].str())) {
          int32_t cppLine = scriptInfo->line +
                            std::stoi(locationMatch[2].str()) -
                            ModulePrefixLineCount - 1;
          processedFrame = locationMatch.prefix().str() +
                           UseSrcFilePath(scriptInfo->filePath.string()) + ':' +
                           std::to_string(cppLine) +
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
// NodeApiTestErrorHandler implementation
//=============================================================================

NodeApiTestErrorHandler::NodeApiTestErrorHandler(
    NodeApiTestContext* testContext,
    std::exception_ptr const& exception,
    std::string&& script,
    std::string&& file,
    int32_t line,
    int32_t scriptLineOffset) noexcept
    : m_testContext(testContext),
      m_exception(exception),
      m_script(std::move(script)),
      m_file(std::move(file)),
      m_line(line),
      m_scriptLineOffset(scriptLineOffset) {}

NodeApiTestErrorHandler::~NodeApiTestErrorHandler() noexcept = default;

int NodeApiTestErrorHandler::HandleAtProcessExit() noexcept {
  if (m_exception) {
    try {
      std::rethrow_exception(m_exception);
    } catch (NodeApiTestException const& ex) {
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
          errorDetails << " Expected: " << ex.AssertionErrorInfo()->Expected
                       << '\n'
                       << "   Actual: " << ex.AssertionErrorInfo()->Actual
                       << '\n';
        }

        std::string processedStack =
            m_testContext->ProcessStack(ex.AssertionErrorInfo()->ErrorStack,
                                        ex.AssertionErrorInfo()->Method);

        return FormatExitMessage(
            m_file.c_str(),
            sourceLine,
            "JavaScript assertion error",
            [&](std::ostream& os) {
              os << "Exception: " << ex.ErrorInfo()->Name << '\n'
                 << "   Method: " << methodName << '\n'
                 << "  Message: " << ex.ErrorInfo()->Message << '\n'
                 << errorDetails.str(/*a filler for formatting*/)
                 << "     File: " << sourceFile << ":" << sourceLine << '\n'
                 << sourceCode << '\n'
                 << "Callstack: " << '\n'
                 << processedStack /*   a filler for formatting    */
                 << "Raw stack: " << '\n'
                 << "  " << ex.AssertionErrorInfo()->ErrorStack;
            });
      } else if (ex.ErrorInfo()) {
        return FormatExitMessage(
            m_file.c_str(), m_line, "JavaScript error", [&](std::ostream& os) {
              os << "Exception: " << ex.ErrorInfo()->Name << '\n'
                 << "  Message: " << ex.ErrorInfo()->Message << '\n'
                 << "Callstack: " << ex.ErrorInfo()->Stack;
            });
      } else {
        return FormatExitMessage(m_file.c_str(),
                                 m_line,
                                 "Test native exception",
                                 [&](std::ostream& os) {
                                   os << "Exception: NodeApiTestException\n"
                                      << "     Code: " << ex.ErrorCode() << '\n'
                                      << "  Message: " << ex.what() << '\n'
                                      << "     Expr: " << ex.Expr();
                                 });
      }
    } catch (std::exception const& ex) {
      return FormatExitMessage(
          m_file.c_str(), m_line, "C++ exception", [&](std::ostream& os) {
            os << "Exception thrown: " << ex.what();
          });
    } catch (...) {
      return FormatExitMessage(
          m_file.c_str(), m_line, "Unexpected test exception");
    }
  }
  return 0;
}

int NodeApiTestErrorHandler::FormatExitMessage(
    const std::string& file, int line, const std::string& message) noexcept {
  return FormatExitMessage(
      file, line, message, [](std::ostream&) { return ""; });
}

int NodeApiTestErrorHandler::FormatExitMessage(
    const std::string& file,
    int line,
    const std::string& message,
    std::function<void(std::ostream&)> getDetails) noexcept {
  std::ostringstream detailsStream;
  getDetails(detailsStream);
  std::string details = detailsStream.str();
  std::cerr << "file:" << file << "\n";
  std::cerr << "line:" << line << "\n";
  std::cerr << message;
  if (!details.empty()) {
    std::cerr << "\n" << details;
  }
  std::cerr << std::endl;
  return 1;
}

std::string NodeApiTestErrorHandler::GetSourceCodeSliceForError(
    int32_t lineIndex, int32_t extraLineCount) noexcept {
  std::string sourceCode;
  auto sourceStream = std::istringstream(m_script + '\n');
  std::string sourceLine;
  int32_t currentLineIndex = 1;  // The line index is 1-based.

  while (std::getline(sourceStream, sourceLine, '\n')) {
    if (currentLineIndex > lineIndex + extraLineCount) break;
    if (currentLineIndex >= lineIndex - extraLineCount) {
      sourceCode += currentLineIndex == lineIndex ? "===> " : "     ";
      sourceCode += sourceLine;
      sourceCode += "\n";
    }
    ++currentLineIndex;
  }

  return sourceCode;
}

}  // namespace node_api_tests

int main(int argc, char** argv) {
  return node_api_tests::EvaluateJSFile(argc, argv);
}
