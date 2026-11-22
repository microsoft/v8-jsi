// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <gtest/gtest.h>
#include <public/V8JsiRuntime.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
using namespace facebook::jsi;
namespace {
int freeInspectorPort() {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data)) std::abort();
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address))) std::abort();
  int size = sizeof(address);
  if (getsockname(s, reinterpret_cast<sockaddr*>(&address), &size)) std::abort();
  closesocket(s); WSACleanup();
  return ntohs(address.sin_port);
}
struct DeferredHostQueue {
  std::atomic<int> deleted{0}, runnerDeleted{0};
  struct Entry : v8runtime::JSITask {
    DeferredHostQueue &queue;
    std::unique_ptr<v8runtime::JSITask> task;
    Entry(DeferredHostQueue &q, std::unique_ptr<v8runtime::JSITask> t) : queue(q), task(std::move(t)) {}
    ~Entry() { ++queue.deleted; }
    void run() override { task->run(); }
  };
  struct Runner : v8runtime::JSITaskRunner {
    DeferredHostQueue &queue;
    explicit Runner(DeferredHostQueue &q) : queue(q) {}
    ~Runner() { ++queue.runnerDeleted; }
    void postTask(std::unique_ptr<v8runtime::JSITask> task) override {
      std::lock_guard<std::mutex> lock(queue.mutex);
      queue.tasks.push_back(std::make_unique<Entry>(queue, std::move(task))); queue.posted.notify_one();
    }
  };
  std::mutex mutex;
  std::condition_variable posted;
  std::deque<std::unique_ptr<v8runtime::JSITask>> tasks;
  bool wait() {
    std::unique_lock<std::mutex> lock(mutex);
    return posted.wait_for(lock, std::chrono::seconds(5), [&] { return !tasks.empty(); });
  }
  std::unique_ptr<v8runtime::JSITask> take() {
    std::lock_guard<std::mutex> lock(mutex);
    if (tasks.empty()) return {};
    auto task = std::move(tasks.front()); tasks.pop_front(); return task;
  }
  void drain(bool run) { while (auto task = take()) if (run) task->run(); }
  auto runtime(int port = 0, const char *name = "", bool multiThread = true) {
    v8runtime::V8RuntimeArgs args;
    args.foreground_task_runner = std::make_shared<Runner>(*this);
    args.flags.enableInspector = port != 0;
    args.flags.enableGCApi = true;
    args.flags.enableMultiThread = multiThread;
    args.debuggerRuntimeName = name;
    args.inspectorPort = static_cast<uint16_t>(port);
    return v8runtime::makeV8Runtime(std::move(args));
  }
};
template <class Future>
void waitForFuture(Future&& future, int seconds, const char* message = "Inspector operation timed out") {
  if (future.wait_for(std::chrono::seconds(seconds)) != std::future_status::ready) {
    fprintf(stderr, "%s\n", message); std::abort();
  }
}
std::string inspectorRequest(int port, const std::string &request, SOCKET *keepOpen = nullptr,
                             size_t splitAt = 0) {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data)) return {};
  SOCKET socket = INVALID_SOCKET;
  sockaddr_in address{}; address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<u_short>(port)); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  std::string response;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  int connected = SOCKET_ERROR;
  // Retry only readiness (before sending HTTP), never an accepted request.
  do {
    socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nonblocking = 1;
    ioctlsocket(socket, FIONBIO, &nonblocking);
    connected = connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    if (connected && WSAGetLastError() == WSAEWOULDBLOCK) {
      fd_set writable, errors;
      FD_ZERO(&writable); FD_SET(socket, &writable); errors = writable;
      timeval timeout{0, 50000};
      if (select(0, nullptr, &writable, &errors, &timeout) > 0) {
        int error = 0, size = sizeof(error);
        if (!getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size) && !error)
          connected = 0;
      }
    }
    if (!connected) break;
    closesocket(socket); socket = INVALID_SOCKET;
  } while (std::chrono::steady_clock::now() < deadline);
  u_long blocking = 0;
  ioctlsocket(socket, FIONBIO, &blocking);
  DWORD timeout = 5000;
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout));
  if (connected == 0) {
    if (splitAt) {
      EXPECT_EQ(send(socket, request.data(), static_cast<int>(splitAt), 0), static_cast<int>(splitAt));
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(send(socket, request.data() + splitAt, static_cast<int>(request.size() - splitAt), 0),
        static_cast<int>(request.size() - splitAt));
    char buffer[8192];
    while (response.size() < sizeof(buffer)) {
      int size = recv(socket, buffer, sizeof(buffer), 0);
      if (size <= 0) break;
      response.append(buffer, size);
      auto headers = response.find("\r\n\r\n"), length = response.find("Content-Length: ");
      if (headers != std::string::npos &&
          (response.find("101 Switching") != std::string::npos ||
           (response.find("HTTP/1.0 400 Bad Request") == 0 &&
            response.find("\r\n", headers + 4) != std::string::npos) ||
           (length != std::string::npos && response.size() >= headers + 4 +
            std::strtoul(response.c_str() + length + 16, nullptr, 10)))) break;
    }
  }
  if (keepOpen && response.find("101 Switching") != std::string::npos) {
    *keepOpen = socket; // The connected test owns the socket and WSAStartup ref.
  } else {
    closesocket(socket); WSACleanup();
  }
  return response;
}
struct InspectorClient {
  SOCKET socket{INVALID_SOCKET};
  ~InspectorClient() { if (socket != INVALID_SOCKET) { closesocket(socket); WSACleanup(); } }
};
std::string attachInspector(int port, const std::string &id, InspectorClient *client) {
  return inspectorRequest(port, "GET /" + id +
      " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
      client ? &client->socket : nullptr);
}
std::string connectInspector(int port, InspectorClient *client = nullptr) {
  auto listing = inspectorRequest(port, "GET /json/list HTTP/1.1\r\nHost: localhost\r\n\r\n");
  auto url = listing.find("ws://");
  if (url == std::string::npos) return listing;
  auto path = listing.find('/', url + 5), end = listing.find('"', path);
  if (path == std::string::npos || end == std::string::npos) return listing;
  return attachInspector(port, listing.substr(path + 1, end - path - 1), client);
}
std::string inspectorFrame(const std::string &message) {
  if (message.size() > 125) std::abort();
  std::string frame{static_cast<char>(0x81), static_cast<char>(0x80 | message.size())};
  frame.append(4, '\0'); // A fixed zero mask is sufficient for this loopback client.
  frame += message;
  return frame;
}
bool sendInspectorMessage(SOCKET socket, const std::string &message) {
  auto frame = inspectorFrame(message);
  return send(socket, frame.data(), static_cast<int>(frame.size()), 0) == static_cast<int>(frame.size());
}
std::string readInspectorMessage(SOCKET socket) {
  auto read = [socket](char *data, size_t size) {
    while (size) {
      int n = recv(socket, data, static_cast<int>(size), 0);
      if (n <= 0) return false;
      data += n; size -= n;
    }
    return true;
  };
  unsigned char header[2];
  if (!read(reinterpret_cast<char *>(header), 2) || header[0] != 0x81 || (header[1] & 0x80)) return {};
  uint64_t size = header[1];
  if (size >= 126) {
    unsigned char length[8];
    int bytes = size == 126 ? 2 : 8;
    if (!read(reinterpret_cast<char *>(length), bytes)) return {};
    size = 0;
    for (int i = 0; i < bytes; ++i) size = (size << 8) | length[i];
  }
  if (size > 4 * 1024 * 1024 + 65536) return {}; // Allow a 4 MiB value plus CDP metadata.
  std::string message(static_cast<size_t>(size), '\0');
  return read(message.data(), message.size()) ? message : std::string{};
}
void pumpInspector(Runtime &runtime, DeferredHostQueue &queue) {
  // Host callbacks run inside a runtime entry, with its isolate/context scopes.
  auto drain = Function::createFromHostFunction(runtime, PropNameID::forAscii(runtime, "drain"), 0,
      [&](Runtime &, const Value &, const Value *, size_t) {
        queue.drain(true);
        return Value::undefined();
      });
  drain.call(runtime);
}
void checkInspectorEvaluation(Runtime &runtime, DeferredHostQueue &queue, int port) {
  InspectorClient client;
  auto response = connectInspector(port, &client);
  ASSERT_NE(response.find("101 Switching"), std::string::npos) << response;
  ASSERT_TRUE(queue.wait());
  pumpInspector(runtime, queue); // Establish CONNECT before evaluating or tearing down.
  ASSERT_TRUE(sendInspectorMessage(client.socket,
      R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"6 * 7","returnByValue":true}})"));
  ASSERT_TRUE(queue.wait());
  pumpInspector(runtime, queue);
  auto result = readInspectorMessage(client.socket);
  EXPECT_NE(result.find(R"("value":42)"), std::string::npos) << result;
}

void expectInspectorListener(int port, bool enabled) {
  ULONG size = 0;
  std::vector<BYTE> storage;
  ULONG status;
  do {
    storage.resize(size);
    status = GetTcpTable(storage.empty() ? nullptr :
        reinterpret_cast<MIB_TCPTABLE *>(storage.data()), &size, FALSE);
  } while (status == ERROR_INSUFFICIENT_BUFFER);
  ASSERT_EQ(status, NO_ERROR);
  auto table = reinterpret_cast<MIB_TCPTABLE *>(storage.data());
  unsigned listeners = 0;
  for (DWORD i = 0; i < table->dwNumEntries; ++i) {
    const auto &row = table->table[i];
    if (row.dwState == MIB_TCP_STATE_LISTEN &&
        ntohs(static_cast<u_short>(row.dwLocalPort)) == port) {
      ++listeners;
      EXPECT_EQ(row.dwLocalAddr, htonl(INADDR_LOOPBACK));
    }
  }
  EXPECT_EQ(listeners, enabled ? 1u : 0u);
}
void expectInspectorConnectionRefused(ULONG address, int port) {
  WSADATA data{};
  ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
  InspectorClient client{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
  ASSERT_NE(client.socket, INVALID_SOCKET);
  sockaddr_in target{}; target.sin_family = AF_INET;
  target.sin_addr.s_addr = address; target.sin_port = htons(static_cast<u_short>(port));
  u_long nonblocking = 1;
  ASSERT_EQ(ioctlsocket(client.socket, FIONBIO, &nonblocking), 0);
  ASSERT_EQ(connect(client.socket, reinterpret_cast<sockaddr *>(&target), sizeof(target)), SOCKET_ERROR);
  auto error = WSAGetLastError();
  if (error == WSAEWOULDBLOCK) {
    fd_set writable, errors;
    FD_ZERO(&writable); FD_SET(client.socket, &writable); errors = writable;
    timeval timeout{10, 0};
    ASSERT_GT(select(0, nullptr, &writable, &errors, &timeout), 0);
    int size = sizeof(error);
    ASSERT_EQ(getsockopt(client.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size), 0);
  }
  EXPECT_EQ(error, WSAECONNREFUSED);
}
void checkInspectorOrigins(bool upgrade) {
  DeferredHostQueue queue;
  auto port = freeInspectorPort();
  auto runtime = queue.runtime(port);
  auto listing = inspectorRequest(port, "GET /json/list HTTP/1.1\r\nHost: localhost\r\n\r\n");
  auto url = listing.find("ws://");
  ASSERT_NE(url, std::string::npos) << listing;
  auto path = listing.find('/', url + 5), end = listing.find('"', path);
  ASSERT_NE(path, std::string::npos); ASSERT_NE(end, std::string::npos);
  auto targetId = listing.substr(path + 1, end - path - 1);
  ASSERT_FALSE(targetId.empty());
  struct Case { const char *headers; bool allowed; bool split = false; };
  // Every discovery route and upgrade must enforce the same allowlist and duplicate handling.
  const Case cases[] = {
      {"", true}, {"Origin: devtools://devtools\r\n", true},
      {"oRiGiN: DEVTOOLS://DevTools\r\n", true}, {"Origin-Extra: null\r\n", true},
      {"Origin: https://example.com\r\n", false}, {"Origin: http://localhost\r\n", false},
      {"Origin: null\r\n", false}, {"Origin:\r\n", false}, {"Origin: \r\n", false},
      {"Origin: file://devtools\r\n", false}, {"Origin: chrome-extension://devtools\r\n", false},
      {"Origin: unknown://devtools\r\n", false}, {"Origin: devtools:/devtools\r\n", false},
      {"Origin: devtools://devtools:9223\r\n", false}, {"Origin: devtools://devtools/\r\n", false},
      {"Origin: devtools://devtools?query\r\n", false}, {"Origin: devtools://devtools#fragment\r\n", false},
      {"Origin: devtools://devtools.evil\r\n", false}, {"Origin: devtools://devtools junk\r\n", false},
      {"Origin: devtools://devtools,devtools://devtools\r\n", false},
      {"Origin: devtools://devtools\r\n x\r\n", false},
      {"Origin: devtools://devtools\r\n", true, true},
      {"Origin: devtools://devtools\r\nOrigin: devtools://devtools\r\n", false},
      {"Origin: devtools://devtools\r\noRiGiN: devtools://devtools\r\n", false},
      {"Origin:\r\nOrigin: devtools://devtools\r\n", false},
      {"Origin: devtools://devtools\r\nOrigin:\r\n", false},
  };
  const std::vector<std::string> paths = upgrade ? std::vector<std::string>{listing.substr(path, end - path)} :
      std::vector<std::string>{"/json/list", "/json", "/json/version"};
  for (const auto &requestPath : paths) {
    SCOPED_TRACE(requestPath);
    std::string request = "GET " + requestPath + " HTTP/1.1\r\nHost: localhost\r\n";
    if (upgrade) request += "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
    for (const auto &test : cases) {
      SCOPED_TRACE(test.headers);
      {
        InspectorClient client;
        auto response = inspectorRequest(port, request + test.headers + "\r\n", &client.socket,
            test.split && upgrade ? request.size() + sizeof("Origin: devtools://") - 1 : 0);
        if (test.allowed) {
          EXPECT_NE(response.find(upgrade ? "101 Switching" : "200 OK"), std::string::npos) << response;
          if (upgrade) {
            ASSERT_TRUE(queue.wait());
            pumpInspector(*runtime, queue);
          } else {
            EXPECT_NE(response.find(requestPath == "/json/version" ? "Protocol-Version" : "webSocketDebuggerUrl"),
                std::string::npos) << response;
          }
        } else {
          EXPECT_EQ(response.find("101 Switching"), std::string::npos) << response;
          EXPECT_EQ(response.find("webSocketDebuggerUrl"), std::string::npos) << response;
          EXPECT_EQ(response.find("application/json"), std::string::npos) << response;
          EXPECT_EQ(response.find("200 OK"), std::string::npos) << response;
          auto body = response.find("\r\n\r\n");
          ASSERT_NE(body, std::string::npos) << response;
          EXPECT_GT(response.size(), body + 4); // Read the rejection body, not just its headers.
          EXPECT_EQ(response.substr(body + 4).find(targetId), std::string::npos) << response;
        }
      }
      if (test.allowed && upgrade) {
        ASSERT_TRUE(queue.wait());
        pumpInspector(*runtime, queue);
      }
    }
  }
}
} // namespace

namespace {
template<class Predicate>
bool awaitTransport(Predicate predicate, int seconds = 5) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  do {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < deadline);
  return predicate();
}
bool openInspectorTcp(InspectorClient& client, int port) {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data)) return false;
  client.socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (client.socket == INVALID_SOCKET) { WSACleanup(); return false; }
  int receiveBuffer = 1024;
  setsockopt(client.socket, SOL_SOCKET, SO_RCVBUF,
      reinterpret_cast<char*>(&receiveBuffer), sizeof(receiveBuffer));
  sockaddr_in address{}; address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<u_short>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return connect(client.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
}
void closeInspectorTcp(InspectorClient& client) {
  if (client.socket == INVALID_SOCKET) return;
  closesocket(client.socket); WSACleanup(); client.socket = INVALID_SOCKET;
}
bool drainInspectorToEof(SOCKET socket, int seconds = 5) {
  int receiveBuffer = 4 * 1024 * 1024;
  setsockopt(socket, SOL_SOCKET, SO_RCVBUF,
      reinterpret_cast<char*>(&receiveBuffer), sizeof(receiveBuffer));
  u_long nonblocking = 1;
  ioctlsocket(socket, FIONBIO, &nonblocking);
  bool closed = false, unexpected = false;
  bool done = awaitTransport([&] {
    char bytes[65536];
    int n = recv(socket, bytes, sizeof(bytes), 0);
    if (!n) return closed = true;
    if (n > 0) return false;
    auto error = WSAGetLastError();
    if (error == WSAECONNRESET || error == WSAECONNABORTED) return closed = true;
    if (error != WSAEWOULDBLOCK) return unexpected = true;
    return false;
  }, seconds);
  return done && closed && !unexpected;
}
} // namespace

#ifndef V8JSI_LEGACY_STATIC_TESTS
TEST(LegacyInspectorAccess, ListenerIsLoopbackAndRefusesNonLoopbackInterfaces) {
  DeferredHostQueue queue;
  auto port = freeInspectorPort();
  auto runtime = queue.runtime(port);
  ASSERT_NE(inspectorRequest(port, "GET /json/list HTTP/1.1\r\nHost: localhost\r\n\r\n")
      .find("200 OK"), std::string::npos);
  expectInspectorListener(port, true);
  ULONG size = 0;
  std::vector<BYTE> storage;
  ULONG status;
  do {
    storage.resize(size);
    status = GetAdaptersAddresses(AF_INET, 0, nullptr, storage.empty() ? nullptr :
        reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data()), &size);
  } while (status == ERROR_BUFFER_OVERFLOW);
  ASSERT_EQ(status, NO_ERROR);
  auto adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
  unsigned checked = 0;
  for (auto adapter = adapters; adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp) continue;
    for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
      auto address = reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr)->sin_addr.s_addr;
      if (!address || (ntohl(address) >> 24) == 127) continue;
      SCOPED_TRACE(ntohl(address));
      expectInspectorConnectionRefused(address, port);
      ++checked;
    }
  }
  EXPECT_GT(checked, 0u) << "An operational non-loopback IPv4 interface is required";
}

TEST(LegacyInspectorAccess, UpgradeAllowsOnlyNativeAndDevToolsOrigins) { checkInspectorOrigins(true); }
TEST(LegacyInspectorAccess, DiscoveryAllowsOnlyNativeAndDevToolsOrigins) { checkInspectorOrigins(false); }

TEST(LegacyInspectorAccess, LoopbackDiscoversAttachesAndEvaluates) {
  DeferredHostQueue queue;
  auto port = freeInspectorPort();
  auto runtime = queue.runtime(port);
  checkInspectorEvaluation(*runtime, queue, port);
}

TEST(LegacyInspectorAccess, HostQueueEvaluatesOutsideRuntimeEntry) {
  // Both policies must enter V8 when the host drains tasks without a JSI call.
  for (bool multiThread : {false, true}) {
    SCOPED_TRACE(multiThread);
    DeferredHostQueue queue;
    const auto port = freeInspectorPort();
    auto runtime = queue.runtime(port, "queued evaluation", multiThread);
    InspectorClient client;
    ASSERT_NE(connectInspector(port, &client).find("101 Switching"), std::string::npos);
    ASSERT_TRUE(queue.wait()); queue.drain(true);
    ASSERT_TRUE(sendInspectorMessage(client.socket,
        R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"6 * 7","returnByValue":true}})"));
    ASSERT_TRUE(queue.wait()); queue.drain(true);
    auto result = readInspectorMessage(client.socket);
    EXPECT_NE(result.find(R"("value":42)"), std::string::npos) << result;
    runtime.reset(); queue.drain(true);
  }
}

TEST(LegacyInspectorAccess, DisabledInspectorDoesNotListen) {
  auto port = freeInspectorPort();
  v8runtime::V8RuntimeArgs args;
  args.inspectorPort = static_cast<uint16_t>(port);
  args.flags.enableInspector = false;
  auto runtime = v8runtime::makeV8Runtime(std::move(args));
  EXPECT_EQ(runtime->evaluateJavaScript(std::make_shared<StringBuffer>("1+1"), "disabled.js").getNumber(), 2);
  expectInspectorListener(port, false);
  expectInspectorConnectionRefused(htonl(INADDR_LOOPBACK), port);
}



TEST(LegacyInspectorTransport, FourMiBReplyArrivesIntact) {
  DeferredHostQueue queue;
  int port = freeInspectorPort();
  auto runtime = queue.runtime(port);
  InspectorClient client;
  ASSERT_NE(connectInspector(port, &client).find("101 Switching"), std::string::npos);
  ASSERT_TRUE(queue.wait()); pumpInspector(*runtime, queue);
  ASSERT_TRUE(sendInspectorMessage(client.socket,
      R"json({"id":1,"method":"Runtime.evaluate","params":{"expression":"'x'.repeat(4*1024*1024)","returnByValue":true}})json"));
  ASSERT_TRUE(queue.wait()); pumpInspector(*runtime, queue);
  auto reply = readInspectorMessage(client.socket);
  auto value = reply.find(R"("value":")");
  ASSERT_NE(value, std::string::npos);
  value += sizeof("\"value\":\"") - 1;
  ASSERT_GE(reply.size(), value + 4 * 1024 * 1024 + 1);
  EXPECT_EQ(reply.substr(value, 4 * 1024 * 1024), std::string(4 * 1024 * 1024, 'x'));
  EXPECT_EQ(reply[value + 4 * 1024 * 1024], '"');
}

TEST(LegacyInspector, SamePortRestartAcceptsFirstDebugger) {
#ifdef _WIN32
  auto port = freeInspectorPort();
  // Keep reset + recreate on one thread to exercise immediate listener handoff.
  auto reload = std::async(std::launch::async, [port] {
    for (int i = 0; i < 10; ++i) {
      SCOPED_TRACE(i);
      DeferredHostQueue queue;
      auto runtime = queue.runtime(port);
      checkInspectorEvaluation(*runtime, queue, port);
      runtime.reset();
      queue.drain(true);
    }
  });
  waitForFuture(reload, 60, "Same-port runtime reload timed out");
  reload.get();
#endif
}



TEST(LegacyInspectorTransport, IdleConnectionCapRejectsUpgradeAndReusesReleasedSlot) {
  DeferredHostQueue queue;
  int port = freeInspectorPort();
  auto runtime = queue.runtime(port);
  auto listing = inspectorRequest(port, "GET /json/list HTTP/1.1\r\nHost: localhost\r\n\r\n");
  auto url = listing.find("ws://");
  ASSERT_NE(url, std::string::npos);
  auto path = listing.find('/', url + 5), end = listing.find('"', path);
  ASSERT_NE(path, std::string::npos); ASSERT_NE(end, std::string::npos);
  auto id = listing.substr(path + 1, end - path - 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::vector<std::unique_ptr<InspectorClient>> idle;
  for (int i = 0; i < 32; ++i) {
    auto client = std::make_unique<InspectorClient>();
    ASSERT_TRUE(openInspectorTcp(*client, port));
    idle.push_back(std::move(client));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  for (const auto &client : idle) {
    fd_set readable; FD_ZERO(&readable); FD_SET(client->socket, &readable);
    timeval timeout{0, 0};
    ASSERT_EQ(select(0, &readable, nullptr, nullptr, &timeout), 0)
        << "Every one of the first 32 connections must remain open";
  }
  InspectorClient excess;
  ASSERT_TRUE(openInspectorTcp(excess, port));
  auto request = "GET /" + id + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
      "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  send(excess.socket, request.data(), static_cast<int>(request.size()), 0);
  char response[512];
  DWORD timeout = 5000;
  setsockopt(excess.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
  int n = recv(excess.socket, response, sizeof(response), 0);
  EXPECT_TRUE(n == 0 || (n == SOCKET_ERROR &&
      (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAECONNABORTED)))
      << "The excess connection must close, not time out or upgrade";

  idle.pop_back();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  InspectorClient replacement;
  ASSERT_TRUE(awaitTransport([&] {
    return attachInspector(port, id, &replacement).find("101 Switching") != std::string::npos;
  }));
  ASSERT_TRUE(queue.wait()); pumpInspector(*runtime, queue);
  ASSERT_TRUE(sendInspectorMessage(replacement.socket,
      R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"6*7","returnByValue":true}})"));
  ASSERT_TRUE(queue.wait()); pumpInspector(*runtime, queue);
  EXPECT_NE(readInspectorMessage(replacement.socket).find(R"("value":42)"), std::string::npos);
  closeInspectorTcp(replacement); idle.clear();

}


#endif

#ifdef V8JSI_LEGACY_STATIC_TESTS
#include "V8JsiRuntime_impl.h"
#include "inspector/inspector_tcp.h"
namespace {
class InspectorRuntimeThread {
 public:
  InspectorRuntimeThread() : thread_([this] {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
        if (jobs_.empty()) return;
        job = std::move(jobs_.front()); jobs_.pop_front();
      }
      job();
    }
  }) {}
  template <class F> auto submit(F job) {
    auto task = std::make_shared<std::packaged_task<decltype(job())()>>(std::move(job));
    auto result = task->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      jobs_.push_back([task] { (*task)(); });
    }
    ready_.notify_one();
    return result;
  }
  template <class F> auto call(F job) {
    auto result = submit(std::move(job));
    waitForFuture(result, 5, "Inspector JS thread timed out");
    return result.get();
  }
  ~InspectorRuntimeThread() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    ready_.notify_one(); thread_.join();
  }
 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_{false};
  std::thread thread_;
};
struct InspectorTarget { std::string id, url, title; };
std::vector<InspectorTarget> inspectorTargets(int port) {
  auto listing = inspectorRequest(port, "GET /json/list HTTP/1.1\r\nHost: localhost:" + std::to_string(port) + "\r\n\r\n");
  std::vector<InspectorTarget> targets;
  size_t offset = listing.find("\r\n\r\n");
  while (offset != std::string::npos && (offset = listing.find('{', offset)) != std::string::npos) {
    auto object = listing.substr(offset, listing.find('}', offset) - offset);
    auto field = [&](const char *key) {
      auto start = object.find(std::string("\"") + key + "\": \"");
      if (start == std::string::npos) return std::string{};
      start = object.find(": \"", start) + 3;
      return object.substr(start, object.find('"', start) - start);
    };
    targets.push_back({field("id"), field("webSocketDebuggerUrl"), field("title")});
    ++offset;
  }
  return targets;
}
std::string inspectorUntil(SOCKET socket, const std::string &token) {
  for (int i = 0; i < 32; ++i) {
    auto message = readInspectorMessage(socket);
    if (message.empty() || message.find(token) != std::string::npos) return message;
  }
  return {};
}
void connectAndEnableDebugger(Runtime& runtime, DeferredHostQueue& queue,
    InspectorRuntimeThread& js, int port, InspectorClient& client, const std::string& target = {}) {
  auto response = target.empty() ? connectInspector(port, &client) : attachInspector(port, target, &client);
  ASSERT_NE(response.find("101 Switching"), std::string::npos) << response;
  ASSERT_TRUE(queue.wait()); js.call([&] { pumpInspector(runtime, queue); });
  ASSERT_TRUE(sendInspectorMessage(client.socket, R"({"id":10,"method":"Debugger.enable"})"));
  ASSERT_TRUE(queue.wait()); js.call([&] { pumpInspector(runtime, queue); });
  ASSERT_NE(inspectorUntil(client.socket, R"("id":10)").find("debuggerId"), std::string::npos);
}
void postTask(Runtime &runtime, void (*callback)(void *), void *data) {
  struct CallbackTask : v8runtime::JSITask {
    void (*callback)(void *); void *data;
    CallbackTask(void (*cb)(void *), void *p) : callback(cb), data(p) {}
    void run() override { callback(data); }
  };
  auto isolate = static_cast<v8runtime::V8Runtime&>(runtime).GetIsolate();
  auto dataSlot = static_cast<v8runtime::IsolateData*>(isolate->GetData(v8runtime::ISOLATE_DATA_SLOT));
  dataSlot->foreground_task_runner_->postTask(std::make_unique<CallbackTask>(callback, data));
}
void expectSurvivor(Runtime &runtime, DeferredHostQueue &queue) {
  int runs = 0;
  postTask(runtime, [](void *p) { ++*static_cast<int*>(p); }, &runs);
  queue.drain(true);
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(runtime.evaluateJavaScript(std::make_shared<StringBuffer>("40+2"), "survivor.js").getNumber(), 42);
}
} // namespace
TEST(LegacyInspectorLifetime, DestroyWaitsForNonPausedJavaScriptToComplete) {
  DeferredHostQueue queue;
  InspectorRuntimeThread js;
  auto runtime = js.call([&] { return queue.runtime(freeInspectorPort()); });
  std::promise<void> started;
  std::atomic<int> result{0};
  js.call([&] {
    runtime->global().setProperty(*runtime, "started", Function::createFromHostFunction(*runtime,
        PropNameID::forAscii(*runtime, "started"), 0, [&](Runtime &, const Value &, const Value *, size_t) {
          started.set_value(); return Value::undefined();
        }));
  });
  auto *target = runtime.get();
  std::pair<Runtime *, std::atomic<int> *> work{target, &result};
  js.call([&] {
    postTask(*target, [](void *p) {
      auto &work = *static_cast<std::pair<Runtime *, std::atomic<int> *> *>(p);
      try {
        auto value = work.first->evaluateJavaScript(std::make_shared<StringBuffer>(
            "started(); var until = Date.now() + 250; while (Date.now() < until) {} 42"), "busy.js");
        *work.second = static_cast<int>(value.getNumber());
      } catch (const JSIException &) { ADD_FAILURE() << "non-paused JS must not be terminated"; }
    }, &work);
  });
  auto task = queue.take(); ASSERT_NE(task, nullptr);
  auto running = std::async(std::launch::async, [&] { task->run(); });
  waitForFuture(started.get_future(), 5);
  auto teardown = js.submit([&] {
    runtime.reset();
    EXPECT_EQ(result.load(), 42) << "destroy must wait for normal JS completion";
  });
  waitForFuture(teardown, 10);
  teardown.get(); running.get(); task.reset();
  EXPECT_EQ(queue.runnerDeleted, 1); EXPECT_EQ(queue.deleted, 1);
  queue.drain(true);
}

TEST(LegacyInspectorLifetime, DestroyCancelsPausedHostTaskWithoutAffectingSurvivor) {
  DeferredHostQueue queue, survivorQueue;
  InspectorRuntimeThread js;
  auto port = freeInspectorPort();
  auto runtime = js.call([&] { return queue.runtime(port); });
  auto targets = inspectorTargets(port);
  ASSERT_EQ(targets.size(), 1u);
  auto survivor = survivorQueue.runtime(port);
  InspectorClient client;
  ASSERT_NO_FATAL_FAILURE(connectAndEnableDebugger(*runtime, queue, js, port, client, targets[0].id));
  int continued = 0, finished = 0;
  js.call([&] {
    runtime->global().setProperty(*runtime, "continued", Function::createFromHostFunction(*runtime,
        PropNameID::forAscii(*runtime, "continued"), 0, [&](Runtime &, const Value &, const Value *, size_t) {
          ++continued; return Value::undefined();
        }));
  });
  std::pair<Runtime *, int *> work{runtime.get(), &finished};
  js.call([&] {
    postTask(*runtime, [](void *p) {
      auto &work = *static_cast<std::pair<Runtime *, int *> *>(p);
      try { work.first->evaluateJavaScript(std::make_shared<StringBuffer>("debugger; continued()"), "pause.js"); }
      catch (const JSIException &) {} // Cancellation must unwind before teardown returns.
      ++*work.second;
    }, &work);
  });
  auto task = queue.take(); ASSERT_NE(task, nullptr);
  auto deleted = queue.deleted.load();
  auto running = std::async(std::launch::async, [&] { task->run(); task.reset(); });
  EXPECT_NE(inspectorUntil(client.socket, "Debugger.paused").find("Debugger.paused"), std::string::npos);
  auto teardown = js.submit([&] { runtime.reset(); });
  waitForFuture(teardown, 10, "Paused runtime destruction timed out");
  teardown.get();
  running.get();
  EXPECT_EQ(continued, 0); EXPECT_EQ(finished, 1);
  EXPECT_EQ(queue.deleted, deleted + 1); EXPECT_EQ(queue.runnerDeleted, 1);
  char buffer[256]; int received;
  do { received = recv(client.socket, buffer, sizeof(buffer), 0); } while (received > 0);
  EXPECT_EQ(received, 0) << "debugger must see EOF";
  queue.drain(true); expectSurvivor(*survivor, survivorQueue);
  js.call([&] { auto later = queue.runtime(); expectSurvivor(*later, queue); });
}



namespace {
void expectInspectorIdle(SOCKET socket) {
  fd_set readable; FD_ZERO(&readable); FD_SET(socket, &readable);
  timeval timeout{0, 50000};
  EXPECT_EQ(select(0, &readable, nullptr, nullptr, &timeout), 0) << "unexpected cross-target message";
}
void checkInspectorTargets(bool sharedPort, bool equalTitles) {
  SCOPED_TRACE(equalTitles ? "equal titles" : "distinct titles");
  DeferredHostQueue queueA, queueB;
  int portA = freeInspectorPort(), portB = sharedPort ? portA : freeInspectorPort();
  while (!sharedPort && portA == portB) portB = freeInspectorPort();
  InspectorRuntimeThread jsA;
  auto a = jsA.call([&] { return queueA.runtime(portA, "target"); });
  auto initial = inspectorTargets(portA);
  ASSERT_EQ(initial.size(), 1u);
  auto b = queueB.runtime(portB, equalTitles ? "target" : "other");
  auto targets = inspectorTargets(portA);
  ASSERT_EQ(targets.size(), sharedPort ? 2u : 1u);
  if (!sharedPort) {
    auto other = inspectorTargets(portB);
    ASSERT_EQ(other.size(), 1u);
    targets.push_back(other.front());
  }
  EXPECT_NE(targets[0].id, targets[1].id);
  EXPECT_NE(targets[0].url, targets[1].url);
  EXPECT_EQ(targets[0].title == targets[1].title, equalTitles);
  // Shared-port listing order is unspecified; retain A's pre-existing target ID.
  if (targets[0].id != initial[0].id) std::swap(targets[0], targets[1]);
  for (int i = 0; i < 2; ++i)
    EXPECT_EQ(targets[i].url, "ws://localhost:" + std::to_string(i ? portB : portA) + '/' + targets[i].id);
  InspectorClient clientA, clientB;
  for (int i = 0; i < 2; ++i) {
    auto &client = i ? clientB : clientA;
    ASSERT_NE(attachInspector(i ? portB : portA, targets[i].id, &client).find("101 Switching"), std::string::npos);
  }
  ASSERT_TRUE(queueA.wait()); ASSERT_TRUE(queueB.wait());
  jsA.call([&] { pumpInspector(*a, queueA); }); pumpInspector(*b, queueB);
  auto command = [&](Runtime &runtime, DeferredHostQueue &queue, InspectorClient &client,
                     const std::string &message, const std::string &token) {
    EXPECT_TRUE(sendInspectorMessage(client.socket, message));
    EXPECT_TRUE(queue.wait());
    if (&runtime == a.get()) jsA.call([&] { pumpInspector(runtime, queue); });
    else pumpInspector(runtime, queue);
    return inspectorUntil(client.socket, token);
  };
  // Per-runtime globals prove routing even when titles collide.
  jsA.call([&] { a->global().setProperty(*a, "identity", 111); });
  b->global().setProperty(*b, "identity", 222);
  EXPECT_NE(command(*a, queueA, clientA,
      R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"identity"}})", R"("id":1)").find(R"("value":111)"), std::string::npos);
  for (auto *client : {&clientA, &clientB}) {
    auto &runtime = client == &clientA ? a : b;
    auto &queue = client == &clientA ? queueA : queueB;
    EXPECT_NE(command(*runtime, queue, *client, R"({"id":2,"method":"Debugger.enable"})", R"("id":2)").find("debuggerId"), std::string::npos);
  }
  ASSERT_TRUE(sendInspectorMessage(clientA.socket,
      R"({"id":3,"method":"Runtime.evaluate","params":{"expression":"debugger; identity"}})"));
  ASSERT_TRUE(queueA.wait());
  // A runs the nested pause loop on its JS thread; the client and B must stay runnable.
  auto paused = jsA.submit([&] { pumpInspector(*a, queueA); });
  EXPECT_NE(inspectorUntil(clientA.socket, "Debugger.paused").find("Debugger.paused"), std::string::npos);
  EXPECT_NE(command(*b, queueB, clientB,
      R"({"id":4,"method":"Runtime.evaluate","params":{"expression":"identity"}})", R"("id":4)").find(R"("value":222)"), std::string::npos);
  expectInspectorIdle(clientB.socket);
  EXPECT_TRUE(sendInspectorMessage(clientA.socket, R"({"id":5,"method":"Debugger.resume"})"));
  auto resumed = inspectorUntil(clientA.socket, "Debugger.resumed");
  EXPECT_NE(resumed.find("Debugger.resumed"), std::string::npos) << resumed;
  waitForFuture(paused, 5, "Paused inspector did not resume");
  paused.get();
  EXPECT_NE(inspectorUntil(clientA.socket, R"("id":3)").find(R"("value":111)"), std::string::npos);
  expectInspectorIdle(clientB.socket);
  jsA.call([&] { a.reset(); }); queueA.drain(true);
  char eof; EXPECT_EQ(recv(clientA.socket, &eof, 1, 0), 0);
  EXPECT_NE(command(*b, queueB, clientB,
      R"({"id":6,"method":"Runtime.evaluate","params":{"expression":"identity"}})", R"("id":6)").find(R"("value":222)"), std::string::npos);
  auto remaining = inspectorTargets(portB);
  ASSERT_EQ(remaining.size(), 1u); EXPECT_EQ(remaining[0].id, targets[1].id);
  closesocket(clientB.socket); clientB.socket = INVALID_SOCKET; WSACleanup();
  ASSERT_TRUE(queueB.wait()); pumpInspector(*b, queueB); // Process disconnect before reattaching.
  checkInspectorEvaluation(*b, queueB, portB);
  a = jsA.call([&] { return queueA.runtime(portA, "replacement"); });
  remaining = inspectorTargets(portA);
  ASSERT_EQ(remaining.size(), sharedPort ? 2u : 1u);
  auto replacement = std::find_if(remaining.begin(), remaining.end(), [&](const auto &t) { return t.id != targets[1].id; });
  ASSERT_NE(replacement, remaining.end()); EXPECT_NE(replacement->id, targets[0].id);
  InspectorClient reloaded;
  ASSERT_NE(attachInspector(portA, replacement->id, &reloaded).find("101 Switching"), std::string::npos);
  ASSERT_TRUE(queueA.wait()); jsA.call([&] { pumpInspector(*a, queueA); });
  EXPECT_NE(command(*a, queueA, reloaded,
      R"({"id":7,"method":"Runtime.evaluate","params":{"expression":"7*6"}})", R"("id":7)").find(R"("value":42)"), std::string::npos);
  jsA.call([&] { a.reset(); }); b.reset();
  queueA.drain(true); queueB.drain(true);
}
} // namespace
// Equal titles must not collapse distinct targets, on shared or separate ports.
TEST(LegacyInspector, PausedTargetDoesNotBlockOtherTargetAndReloadKeepsSurvivor) {
  for (bool sharedPort : {false, true}) for (bool equalTitles : {false, true}) {
    SCOPED_TRACE(sharedPort);
    checkInspectorTargets(sharedPort, equalTitles);
  }
}

namespace {
inspector::TransportStats runtimeTransportStats(int port) {
  inspector::TransportStats stats;
  EXPECT_TRUE(inspector::tcp_server::StatsForPort(port, stats));
  return stats;
}
template<class Scenario> void runBoundedInspectorTransportScenario(Scenario scenario) {
  auto result = std::async(std::launch::async, std::move(scenario));
  waitForFuture(result, 60, "Inspector transport scenario timed out");
  result.get();
}
enum class InspectorOverflowCase { RepeatedFourMiB, SingleOversizedReply };

void checkInspectorOverflowRecovery(InspectorOverflowCase scenario) {
  runBoundedInspectorTransportScenario([scenario] {
    const bool oversized = scenario == InspectorOverflowCase::SingleOversizedReply;
    DeferredHostQueue queue;
    int port = freeInspectorPort();
    auto runtime = queue.runtime(port);
    InspectorClient client;
    ASSERT_NE(connectInspector(port, &client).find("101 Switching"), std::string::npos);
    ASSERT_TRUE(queue.wait()); pumpInspector(*runtime, queue);
    int receiveBuffer = 1024;
    setsockopt(client.socket, SOL_SOCKET, SO_RCVBUF,
        reinterpret_cast<char*>(&receiveBuffer), sizeof(receiveBuffer));
    if (oversized) {
      ASSERT_TRUE(awaitTransport([&] { return runtimeTransportStats(port).retained == 0; }));
      ASSERT_EQ(runtimeTransportStats(port).overflows, 0u);
    }
    // One 64 MiB + 1 value is rejected before allocating an outbound frame;
    // repeated 4 MiB replies instead fill the non-reading connection's budget.
    const char* message = oversized
        ? R"json({"id":1,"method":"Runtime.evaluate","params":{"expression":"'x'.repeat(67108865)","returnByValue":true}})json"
        : R"json({"id":1,"method":"Runtime.evaluate","params":{"expression":"'x'.repeat(4*1024*1024)","returnByValue":true}})json";
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(30);
    const int attempts = oversized ? 1 : 24;
    for (int i = 0; i < attempts && runtimeTransportStats(port).overflows == 0; ++i) {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline);
      ASSERT_TRUE(sendInspectorMessage(client.socket, message));
      ASSERT_TRUE(queue.wait()); pumpInspector(*runtime, queue);
    }
    const int recoverySeconds = oversized ? 10 : 5;
    EXPECT_EQ(runtimeTransportStats(port).overflows, 1u);
    EXPECT_TRUE(awaitTransport([&] { return runtimeTransportStats(port).retained == 0; }, recoverySeconds));
    EXPECT_LT(std::chrono::steady_clock::now(), deadline);
    EXPECT_TRUE(drainInspectorToEof(client.socket, recoverySeconds));
    EXPECT_TRUE(awaitTransport([&] { return runtimeTransportStats(port).admitted == 0; }, recoverySeconds));
    if (oversized)
      EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(30));
    checkInspectorEvaluation(*runtime, queue, port);
    EXPECT_EQ(runtimeTransportStats(port).overflows, 1u); // Diagnostic counted once.
  });
}
} // namespace

TEST(LegacyInspectorTransport, NonReadingDebuggerOverflowsClosesAndAllowsFreshEvaluation) {
  checkInspectorOverflowRecovery(InspectorOverflowCase::RepeatedFourMiB);
}

TEST(LegacyInspectorTransport, SingleOversizedReplyClosesAndAllowsFreshEvaluation) {
  checkInspectorOverflowRecovery(InspectorOverflowCase::SingleOversizedReply);
}

TEST(LegacyInspectorTransport, ListenerOverflowRefundsAllConnectionsAndAllowsFreshEvaluation) {
  runBoundedInspectorTransportScenario([] {
    struct Target {
      DeferredHostQueue queue;
      InspectorRuntimeThread js;
      std::unique_ptr<Runtime> runtime;
      ~Target() { js.call([&] { runtime.reset(); }); }
    };
    int port = freeInspectorPort();
    std::vector<std::unique_ptr<Target>> targets;
    std::vector<std::unique_ptr<InspectorClient>> clients;
    for (int i = 0; i < 3; ++i) {
      auto target = std::make_unique<Target>();
      auto name = "target-" + std::to_string(i);
      target->runtime = target->js.call([&] { return target->queue.runtime(port, name.c_str()); });
      std::string id;
      for (const auto &entry : inspectorTargets(port)) if (entry.title == name) id = entry.id;
      ASSERT_FALSE(id.empty());
      auto client = std::make_unique<InspectorClient>();
      ASSERT_NE(attachInspector(port, id, client.get()).find("101 Switching"), std::string::npos);
      ASSERT_TRUE(target->queue.wait());
      target->js.call([&] { pumpInspector(*target->runtime, target->queue); });
      int bytes = 1024;
      setsockopt(client->socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&bytes), sizeof(bytes));
      clients.push_back(std::move(client));
      targets.push_back(std::move(target));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    // Separate targets keep each debugger session active while the shared
    // listener reaches 128 MiB before any connection reaches 64 MiB.
    for (int i = 0; i < 48 && runtimeTransportStats(port).overflows == 0; ++i) {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline);
      ASSERT_TRUE(sendInspectorMessage(clients[i % 3]->socket,
          R"json({"id":1,"method":"Runtime.evaluate","params":{"expression":"'x'.repeat(4*1024*1024)","returnByValue":true}})json"));
      auto &target = *targets[i % 3];
      ASSERT_TRUE(target.queue.wait());
      target.js.call([&] { pumpInspector(*target.runtime, target.queue); });
    }
    auto stats = runtimeTransportStats(port);
    EXPECT_EQ(stats.overflows, 1u);
    EXPECT_GT(stats.peak, 124 * 1024 * 1024u);
    EXPECT_LE(stats.peak, 128 * 1024 * 1024u);
    EXPECT_LT(stats.connection_peak, 64 * 1024 * 1024u);
    clients.clear();
    ASSERT_TRUE(awaitTransport([&] {
      auto stats = runtimeTransportStats(port);
      return stats.retained == 0 && stats.admitted == 0;
    }));
    auto &target = *targets[0];
    std::string id;
    for (const auto &entry : inspectorTargets(port)) if (entry.title == "target-0") id = entry.id;
    ASSERT_FALSE(id.empty());
    InspectorClient recovered;
    ASSERT_NE(attachInspector(port, id, &recovered).find("101 Switching"), std::string::npos);
    ASSERT_TRUE(target.queue.wait());
    target.js.call([&] { pumpInspector(*target.runtime, target.queue); });
    ASSERT_TRUE(sendInspectorMessage(recovered.socket,
        R"({"id":2,"method":"Runtime.evaluate","params":{"expression":"6*7","returnByValue":true}})"));
    ASSERT_TRUE(target.queue.wait());
    target.js.call([&] { pumpInspector(*target.runtime, target.queue); });
    EXPECT_NE(readInspectorMessage(recovered.socket).find(R"("value":42)"), std::string::npos);
  });
}

TEST(LegacyInspectorTransport, ProductionBudgetsHaveExactInclusiveBoundaries) {
  constexpr size_t limit = 64 * 1024 * 1024;
  auto budget = std::make_shared<inspector::TransportBudget>();
  auto first = budget->TryAdmitConnection(), second = budget->TryAdmitConnection();
  ASSERT_TRUE(first->TryReserve(limit - 1));
  ASSERT_TRUE(first->TryReserve(1));
  EXPECT_EQ(budget->Stats().retained, limit);
  EXPECT_FALSE(first->TryReserve(1));
  first->Release(limit);
  EXPECT_FALSE(first->TryReserve(1)); // A refund cannot revive a failed session.
  EXPECT_EQ(budget->Stats().overflows, 1u);
  EXPECT_FALSE(second->TryReserve(limit + 1)); // No exemption on an idle account.
  first.reset(); second.reset();
  first = budget->TryAdmitConnection(); second = budget->TryAdmitConnection();
  auto third = budget->TryAdmitConnection();
  ASSERT_TRUE(first->TryReserve(limit));
  ASSERT_TRUE(second->TryReserve(limit - 1));
  ASSERT_TRUE(third->TryReserve(1)); // Aggregate just below, at, and above its limit.
  EXPECT_FALSE(third->TryReserve(1));
  EXPECT_EQ(budget->Stats().peak, 2 * limit);
  third->Release(1);
  second->Release(limit - 1); first->Release(limit);
  second.reset(); first.reset(); third.reset();
  EXPECT_EQ(budget->Stats().retained, 0u);
  EXPECT_EQ(budget->Stats().admitted, 0u);
  // Two full accounts must also reject the first byte on a third, idle account.
  first = budget->TryAdmitConnection(); second = budget->TryAdmitConnection();
  third = budget->TryAdmitConnection();
  ASSERT_TRUE(first->TryReserve(limit)); ASSERT_TRUE(second->TryReserve(limit));
  EXPECT_FALSE(third->TryReserve(1));
  EXPECT_EQ(budget->Stats().overflows, 4u);
  first->Release(limit); second->Release(limit);
  first.reset(); second.reset(); third.reset();
  std::vector<std::shared_ptr<inspector::ConnectionAccount>> accounts;
  for (int i = 0; i < 32; ++i) accounts.push_back(budget->TryAdmitConnection());
  EXPECT_EQ(budget->Stats().admitted, 32u);
  EXPECT_FALSE(budget->TryAdmitConnection());
  EXPECT_EQ(budget->Stats().rejected_connections, 1u);
  auto buffer = inspector::OutboundBuffer::Create(accounts.back(), 7);
  ASSERT_TRUE(buffer);
  accounts.pop_back();
  EXPECT_FALSE(budget->TryAdmitConnection()); // Buffer retains the admitted slot.
  buffer.reset();
  EXPECT_TRUE(budget->TryAdmitConnection());
  accounts.clear();
  EXPECT_EQ(budget->Stats().retained, 0u);
  EXPECT_EQ(budget->Stats().admitted, 0u);
}

namespace {
void checkCetBaselineStepOver(int depth) {
  const char* flags = std::getenv("V8JSI_TEST_V8_FLAGS");
  if (!flags || std::string(flags).find("--allow-natives-syntax") == std::string::npos)
    GTEST_SKIP() << "Requires --allow-natives-syntax and Sparkplug";
  DeferredHostQueue queue;
  InspectorRuntimeThread js;
  auto port = freeInspectorPort();
  auto runtime = js.call([&] { return queue.runtime(port); });
  auto eval = [&](const std::string& source) {
    return runtime->evaluateJavaScript(std::make_shared<StringBuffer>(source), "cet-inspector.js");
  };
  if (!js.call([&] { return eval("%IsSparkplugEnabled()").getBool(); })) {
    js.call([&] { runtime.reset(); }); queue.drain(true);
    GTEST_SKIP() << "Required tier Sparkplug disabled by flags/build";
  }
  InspectorClient client;
  ASSERT_NO_FATAL_FAILURE(connectAndEnableDebugger(*runtime, queue, js, port, client));
  // The tier assertion runs in every live recursive frame before the debugger
  // discards baseline code. Depth 2 uses a different call site; depths 3 and 1
  // share a return PC, covering the mark-once guard and jump idempotence.
  js.call([&] {
    eval(R"JS(function cetBaseline(depth) {
  if (!(%GetOptimizationStatus(cetBaseline) & (1 << 16))) throw Error('not baseline');
  if (depth > 0) {
    if (depth == 2) return 1 + cetBaseline(depth - 1);
    return 1 + cetBaseline(depth - 1);
  }
  debugger;
  let result = 40;
  result += 2;
  return result;
}
%CompileBaseline(cetBaseline);
)JS");
  });
  auto expression = "cetBaseline(" + std::to_string(depth) + ")";
  ASSERT_TRUE(sendInspectorMessage(client.socket,
      "{\"id\":101,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"" + expression +
      "\",\"returnByValue\":true}}"));
  ASSERT_TRUE(queue.wait());
  auto dispatch = js.submit([&] { pumpInspector(*runtime, queue); });
  auto expectPause = [&](int line) {
    auto message = inspectorUntil(client.socket, "Debugger.paused");
    EXPECT_NE(message.find("Debugger.paused"), std::string::npos) << message;
    auto location = message.find("\"location\":");
    auto locationEnd = message.find('}', location);
    auto topLocation = message.substr(location == std::string::npos ? 0 : location,
        locationEnd == std::string::npos ? 0 : locationEnd - location);
    EXPECT_NE(topLocation.find("\"lineNumber\":" + std::to_string(line) + ","), std::string::npos) << message;
    size_t frames = 0;
    for (size_t pos = 0; (pos = message.find("\"functionName\":\"cetBaseline\"", pos)) != std::string::npos; ++pos)
      ++frames;
    EXPECT_EQ(frames, static_cast<size_t>(depth + 1)) << message;
  };
  expectPause(6);
  EXPECT_TRUE(sendInspectorMessage(client.socket, R"({"id":102,"method":"Debugger.stepOver"})"));
  expectPause(7);
  EXPECT_TRUE(sendInspectorMessage(client.socket, R"({"id":103,"method":"Debugger.resume"})"));
  auto result = inspectorUntil(client.socket, R"("id":101,)");
  EXPECT_EQ(result.find("exceptionDetails"), std::string::npos) << result;
  EXPECT_NE(result.find("\"value\":" + std::to_string(42 + depth) + ","), std::string::npos) << result;
  waitForFuture(dispatch, 5, "Baseline stepOver did not finish outer frames");
  dispatch.get();
  js.call([&] { runtime.reset(); }); queue.drain(true);
}
} // namespace

TEST(LegacyInspector, CetStepOverRecursiveBaselineFrames) {
#ifdef _WIN32
  checkCetBaselineStepOver(3);
#else
  GTEST_SKIP() << "Windows inspector transport required";
#endif
}

TEST(LegacyInstrumentationFlags, EtwStackWalkingFlagsAreAccepted) {
  char program[] = "instrumentation";
  char enable[] = "--enable-etw-stack-walking";
  char unfiltered[] = "--no-enable-etw-by-custom-filter-only";
  char *argv[] = {program, enable, unfiltered};
  int argc = 3;
  v8::V8::SetFlagsFromCommandLine(&argc, argv, true);
  EXPECT_EQ(argc, 1);
}

int main(int argc, char **argv) {
  if (const char *flags = std::getenv("V8JSI_TEST_V8_FLAGS")) v8::V8::SetFlagsFromString(flags);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
