// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#define NAPI_EXPERIMENTAL
#include <V8JsiRuntime.h>
#include <node-api/js_runtime_api.h>
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>
#ifdef _DEBUG
#include <crtdbg.h>
#endif

using namespace facebook;
static_assert(JSI_VERSION == 20);
#if defined(_M_IX86)
static_assert(offsetof(v8runtime::V8RuntimeArgs, inspectorPort) == 16);
static_assert(offsetof(v8runtime::V8RuntimeArgs, debuggerRuntimeName) == 28);
static_assert(sizeof(v8runtime::V8RuntimeArgs) == (_ITERATOR_DEBUG_LEVEL == 2 ? 60 : 56));
static_assert(offsetof(v8runtime::V8RuntimeArgs, _flagspad) == (_ITERATOR_DEBUG_LEVEL == 2 ? 56 : 52));
#else
static_assert(offsetof(v8runtime::V8RuntimeArgs, inspectorPort) == 32);
static_assert(offsetof(v8runtime::V8RuntimeArgs, debuggerRuntimeName) == 56);
static_assert(sizeof(v8runtime::V8RuntimeArgs) == (_ITERATOR_DEBUG_LEVEL == 2 ? 104 : 96));
static_assert(offsetof(v8runtime::V8RuntimeArgs, _flagspad) == (_ITERATOR_DEBUG_LEVEL == 2 ? 96 : 88));
#endif

void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void ok(napi_status status) { require(status == napi_ok, "jsr/Node-API call failed"); }
jsi::Value eval(jsi::Runtime &rt, const char *source) {
  return rt.evaluateJavaScript(std::make_shared<jsi::StringBuffer>(source), "consumer.js");
}
struct Runner : v8runtime::JSITaskRunner {
  std::mutex mutex;
  std::condition_variable posted;
  std::vector<std::unique_ptr<v8runtime::JSITask>> tasks;
  void postTask(std::unique_ptr<v8runtime::JSITask> task) override {
    std::lock_guard<std::mutex> lock(mutex);
    tasks.push_back(std::move(task));
    posted.notify_all();
  }
  bool pending() { std::lock_guard<std::mutex> lock(mutex); return !tasks.empty(); }
  void waitForTask(const char *message) {
    std::unique_lock<std::mutex> lock(mutex);
    require(posted.wait_for(lock, std::chrono::seconds(5), [&] { return !tasks.empty(); }), message);
  }
  void drain() {
    std::vector<std::unique_ptr<v8runtime::JSITask>> batch;
    { std::lock_guard<std::mutex> lock(mutex); batch.swap(tasks); }
    for (auto &task : batch) task->run();
  }
};
struct Host final : jsi::HostObject {
  int value{7};
  int &destroyed;
  explicit Host(int &count) : destroyed(count) {}
  ~Host() override { ++destroyed; }
  jsi::Value get(jsi::Runtime &rt, const jsi::PropNameID &name) override {
    if (name.utf8(rt) == "fail") throw jsi::JSError(rt, "host failure");
    return value;
  }
  void set(jsi::Runtime &rt, const jsi::PropNameID &, const jsi::Value &v) override {
    if (v.getNumber() == -1) throw jsi::JSError(rt, "host setter JSError");
    if (v.getNumber() == -2) throw std::runtime_error("host setter exception");
    if (v.getNumber() == -3) throw 3;
    value = static_cast<int>(v.getNumber());
  }
  std::vector<jsi::PropNameID> getPropertyNames(jsi::Runtime &rt) override {
    std::vector<jsi::PropNameID> names; names.push_back(jsi::PropNameID::forAscii(rt, "value")); return names;
  }
};
INTERNET_PORT freeInspectorPort() {
  WSADATA data{};
  require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock startup failed");
  const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int length = sizeof(address);
  const bool success = socket != INVALID_SOCKET &&
      bind(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
      getsockname(socket, reinterpret_cast<sockaddr *>(&address), &length) == 0;
  if (socket != INVALID_SOCKET) closesocket(socket);
  WSACleanup();
  require(success, "free inspector port discovery failed");
  return ntohs(address.sin_port);
}
std::string discovery(INTERNET_PORT port) {
  auto session = WinHttpOpen(L"legacy-consumer", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
  require(session != nullptr, "WinHttpOpen failed");
  WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
  auto connection = WinHttpConnect(session, L"127.0.0.1", port, 0);
  auto request = WinHttpOpenRequest(connection, L"GET", L"/json/list", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
  bool success = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
  std::string response;
  if (success) {
    char buffer[4096]; DWORD count;
    while (WinHttpReadData(request, buffer, sizeof(buffer), &count) && count) response.append(buffer, count);
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  require(success, "inspector discovery failed");
  return response;
}
void directRuntime() {
  auto runner = std::make_shared<Runner>();
  v8runtime::V8RuntimeArgs args;
  const auto port = freeInspectorPort();
  args.inspectorPort = port;
  args.debuggerRuntimeName = "MSVC legacy consumer";
  args.flags.enableInspector = true;
  args.flags.explicitMicrotaskPolicy = true;
  args.foreground_task_runner = runner;
  auto runtime = v8runtime::makeV8Runtime(std::move(args));
  require(discovery(port).find("MSVC legacy consumer") != std::string::npos, "inspector arguments were not preserved");
  require(eval(*runtime, "6 * 7").getNumber() == 42, "evaluation failed");
  int destroyed = 0;
  {
    auto host = std::make_shared<Host>(destroyed);
    runtime->global().setProperty(*runtime, "host", jsi::Object::createFromHostObject(*runtime, host));
    require(eval(*runtime, "host.value = 17; host.value").getNumber() == 17, "host callbacks failed");
    const auto properties = eval(*runtime,
        "host.assigned = 23; host[4] = 29; JSON.stringify(["
        "Object.prototype.hasOwnProperty.call(host, 'assigned'), 'assigned' in host,"
        "Object.getOwnPropertyNames(host).includes('assigned'),"
        "Object.prototype.hasOwnProperty.call(host, '4'), 4 in host, Object.getOwnPropertyNames(host).includes('4')])").getString(*runtime).utf8(*runtime);
    require(host->value == 29, "indexed host setter callback failed");
    std::cout << "Host setter properties: " << properties << '\n';
    require(properties == "[true,true,true,true,true,true]", "host setter own-property behavior changed");
    require(eval(*runtime, "Object.keys(host).includes('value')").getBool(), "host enumeration failed");
    // Cover all exception translations for both named and indexed interceptors.
    require(eval(*runtime,
        "(() => { const messages = ['host setter JSError', 'host setter exception',"
        "'<Unknown exception in host function callback>'];"
        "for (let i = 0; i < messages.length; ++i) {"
        "for (const key of ['throw' + i, 100 + i]) {"
        "let caught = false; try { host[key] = -(i + 1); }"
        "catch (e) { caught = e.message.includes(messages[i]); }"
        "if (!caught || Object.getOwnPropertyNames(host).includes(String(key))) return false;"
        "}} return true; })()").getBool(), "throwing host setter did not preserve exception and property behavior");
    bool caught = false;
    try { eval(*runtime, "host.fail"); } catch (const jsi::JSError &error) { caught = error.getMessage().find("host failure") != std::string::npos; }
    require(caught, "host JSError was not caught by type");
    caught = false;
    try { eval(*runtime, "throw new Error('JS failure')"); } catch (const jsi::JSError &error) { caught = error.getMessage().find("JS failure") != std::string::npos; }
    require(caught, "JS JSError was not caught by type");
  }
  runtime.reset();
  require(destroyed == 1, "unique_ptr runtime destruction did not release host object");
}
struct Deferred final : v8runtime::JSITask {
  void *data; jsr_task_run_cb runCallback; jsr_data_delete_cb deleteCallback; void *hint;
  Deferred(void *d, jsr_task_run_cb r, jsr_data_delete_cb del, void *h) : data(d), runCallback(r), deleteCallback(del), hint(h) {}
  ~Deferred() override { deleteCallback(data, hint); }
  void run() override { runCallback(data); }
};
void jsrRuntime(bool lateTasks) {
  Runner runner;
  jsr_config config; jsr_runtime runtime; napi_env env;
  ok(jsr_create_config(&config));
  ok(jsr_config_set_task_runner(config, &runner, [](void *runner, void *data, jsr_task_run_cb run, jsr_data_delete_cb del, void *hint) {
    static_cast<Runner *>(runner)->postTask(std::make_unique<Deferred>(data, run, del, hint));
  }, nullptr, nullptr));
  ok(jsr_create_runtime(config, &runtime));
  ok(jsr_delete_config(config));
  ok(jsr_runtime_get_node_api_env(runtime, &env));
  jsr_napi_env_scope scope;
  ok(jsr_open_napi_env_scope(env, &scope));
  napi_value source, result; int32_t number;
  ok(napi_create_string_utf8(env, "21 * 2", NAPI_AUTO_LENGTH, &source));
  ok(jsr_run_script(env, source, "jsr-consumer.js", &result));
  ok(napi_get_value_int32(env, result, &number));
  require(number == 42, "jsr evaluation failed");
  int finalized = 0;
  if (lateTasks) {
    ok(node_api_post_finalizer(env, [](napi_env, void *data, void *) { ++*static_cast<int *>(data); }, &finalized, nullptr));
    require(runner.pending(), "finalizer was not queued");
  }
  ok(jsr_close_napi_env_scope(env, scope));
  ok(jsr_delete_runtime(runtime));
  if (lateTasks) {
    require(finalized == 1, "teardown did not drain pending finalizer");
    runner.drain();
    require(finalized == 1, "late task repeated a finalizer after teardown");
  }
}
struct InspectorConnection {
  using Handle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;
  Handle session{nullptr, WinHttpCloseHandle}, connection{nullptr, WinHttpCloseHandle}, socket{nullptr, WinHttpCloseHandle};
  explicit InspectorConnection(INTERNET_PORT port) {
    const auto listing = discovery(port);
    const auto marker = listing.find("ws://127.0.0.1:");
    require(marker != std::string::npos, "inspector URL missing");
    const auto start = listing.find('/', marker + 5), end = listing.find('"', start);
    const auto target = listing.substr(start, end - start);
    const std::wstring path(target.begin(), target.end());
    session.reset(WinHttpOpen(L"legacy-lifetime-consumer", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0));
    require(session != nullptr, "inspector session failed");
    WinHttpSetTimeouts(session.get(), 5000, 5000, 5000, 5000);
    connection.reset(WinHttpConnect(session.get(), L"127.0.0.1", port, 0));
    Handle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0), WinHttpCloseHandle);
    require(request != nullptr, "inspector request failed");
    const bool upgraded = WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
        WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request.get(), nullptr);
    if (upgraded) socket.reset(WinHttpWebSocketCompleteUpgrade(request.get(), 0));
    require(socket != nullptr, "inspector WebSocket upgrade failed");
  }
  void send(const char *message) {
    require(WinHttpWebSocketSend(socket.get(), WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char *>(message), static_cast<DWORD>(strlen(message))) == NO_ERROR, "inspector send failed");
  }
  void receiveResponse() {
    std::string message;
    do {
      char buffer[4096]; DWORD count{}; WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
      require(WinHttpWebSocketReceive(socket.get(), buffer, sizeof(buffer), &count, &type) == NO_ERROR, "inspector receive failed");
      require(type != WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE, "inspector closed before responding");
      message.append(buffer, count);
      if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
      if (message.find("\"id\":101") != std::string::npos) {
        require(message.find("\"result\"") != std::string::npos, "surviving inspector command failed");
        return;
      }
      message.clear();
    } while (true);
  }
  void close() {
    require(WinHttpWebSocketShutdown(socket.get(), WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0) == NO_ERROR, "inspector shutdown failed");
    socket.reset();
  }
};
void inspectorDispatchSurvivesContextAndAgentTeardown() {
  auto runner = std::make_shared<Runner>();
  const auto port = freeInspectorPort();
  auto create = [&] {
    v8runtime::V8RuntimeArgs args;
    args.inspectorPort = port;
    args.flags.enableInspector = true;
    args.foreground_task_runner = runner;
    return v8runtime::makeV8Runtime(std::move(args));
  };
  auto first = create(), survivor = create();
  InspectorConnection inspector(port);
  inspector.send("{\"id\":101,\"method\":\"Runtime.enable\"}");
  runner->waitForTask("inspector connect did not post dispatch work");
  first.reset();
  survivor->global().setProperty(*survivor, "pump", jsi::Function::createFromHostFunction(*survivor,
      jsi::PropNameID::forAscii(*survivor, "pump"), 0,
      [&](jsi::Runtime &, const jsi::Value &, const jsi::Value *, size_t) { runner->drain(); return jsi::Value::undefined(); }));
  require(eval(*survivor, "pump(); 40 + 2").getNumber() == 42, "surviving inspector runtime failed");
  inspector.receiveResponse();
  inspector.close();
  runner->waitForTask("inspector disconnect did not post dispatch work");
  survivor.reset();
  require(runner->pending(), "teardown did not retain a late inspector dispatch task");
  runner->drain();
  auto replacement = create();
  require(eval(*replacement, "42").getNumber() == 42, "inspector teardown affected a replacement runtime");
}
int main(int argc, char **argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef _DEBUG
  // A failed cross-module allocation check must fail, not wait on a dialog in CI.
  _CrtSetReportHook([](int, char *message, int *) { std::cerr << message; ExitProcess(1); return 0; });
#endif
  try {
    if (argc == 1) { directRuntime(); jsrRuntime(false); }
    else if (std::strcmp(argv[1], "--finalizer-drain") == 0) jsrRuntime(true);
    else if (std::strcmp(argv[1], "--late-tasks") == 0) inspectorDispatchSurvivesContextAndAgentTeardown();
    else throw std::runtime_error("unknown consumer mode");
    std::cout << "MSVC consumer passed\n"; return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
