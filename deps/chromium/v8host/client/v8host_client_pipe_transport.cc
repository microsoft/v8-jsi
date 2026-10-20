// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "v8host_client_transport.h"
#include "v8host_broker_rendezvous.h"
#include "v8host_protocol.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace v8host::client {
namespace {
namespace wire = protocol;

struct Outbound {
  std::vector<uint8_t> bytes;
  wire::FrameHeader header;
  uint64_t key = 0;
  size_t charge = 0;
  bool relay = false;
};

struct Pump {
  explicit Pump(TransportParams p) : params(std::move(p)) {
    stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    launched = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  ~Pump() {
    if (stop) ::CloseHandle(stop);
    if (wake) ::CloseHandle(wake);
    if (launched) ::CloseHandle(launched);
  }
  TransportParams params;
  std::shared_ptr<ClientTransportDelegate> delegate;
  HMODULE module = nullptr;
  HANDLE stop = nullptr, wake = nullptr, launched = nullptr, thread = nullptr;
  std::atomic<uint32_t> conn{0};
  std::mutex mutex;
  std::deque<Outbound> queue;
  std::unordered_map<uint64_t, size_t> runs;
  size_t controls = 0, control_bytes = 0, relay_bytes = 0;
  bool started = false, terminal = false;
  std::atomic<bool> closed{false};

  void Credit(const Outbound& item) {
    if (item.relay) {
      relay_bytes -= item.charge;
      auto it = runs.find(item.key);
      it->second -= item.charge;
      if (!it->second) runs.erase(it);
    } else {
      --controls;
      control_bytes -= item.charge;
    }
  }

  bool Stopped() const { return ::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0; }

  void Run() {
    BrokerConnection connection;
    BrokerRendezvous rendezvous(params.payload_directory,
                               static_cast<BrokerMode>(params.broker_mode));
    HelloResult hello;
#ifdef V8HOST_CLIENT_TESTING
    rendezvous.SetLaunchHookForTesting(params.test_launch, params.test_context);
#endif
    ULONGLONG deadline = ::GetTickCount64() + 5000;
    RendezvousStatus status = RendezvousStatus::kStartTimeout;
    while (!Stopped() && ::GetTickCount64() < deadline) {
#ifdef V8HOST_CLIENT_TESTING
      if (params.test_connect) {
        status = params.test_connect(params.test_context, &connection, &hello, stop, deadline);
      } else
#endif
      {
        status = rendezvous.ConnectOrLaunch(&connection, stop, &deadline);
#ifdef V8HOST_CLIENT_TESTING
        if (params.test_launch)
          params.test_launch(params.test_context, LaunchHookPoint::kReturned, nullptr, deadline, rendezvous.endpoint().c_str());
#endif
        if (status == RendezvousStatus::kOk)
          status = connection.Handshake(1, &hello, stop, deadline);
      }
      if (status == RendezvousStatus::kOk) break;
      connection.Close();
      if (status != RendezvousStatus::kIoFailed ||
          params.broker_mode != v8host_broker_mode_shared) break;
      const ULONGLONG now = ::GetTickCount64();
      if (now >= deadline) break;
      ::WaitForSingleObject(stop, static_cast<DWORD>((std::min)(deadline - now, ULONGLONG{10})));
    }
    TransportDisconnect reason = TransportDisconnect::kConnectFailed;
    if (status == RendezvousStatus::kOk && !Stopped()) {
      conn.store(hello.conn_id, std::memory_order_release);
      delegate->OnConnected(hello.conn_id, hello.selected_major, hello.selected_minor);
      reason = Duplex(connection.pipe(), hello);
    }
    connection.Close();
    {
      std::lock_guard<std::mutex> lock(mutex);
      terminal = true;
      queue.clear();
      runs.clear();
      controls = control_bytes = relay_bytes = 0;
    }
    if (!closed.load(std::memory_order_acquire)) delegate->OnDisconnect(reason);
  }

  TransportDisconnect Duplex(HANDLE pipe, const HelloResult& hello) {
    OVERLAPPED read = {}, write = {};
    read.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    write.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<uint8_t> buffer(wire::kMaxFrameSize);
    Outbound active;
    bool reading = false, writing = false;
    TransportDisconnect reason = TransportDisconnect::kBrokerLost;
    if (!read.hEvent || !write.hEvent) goto drain;
    while (!Stopped()) {
      delegate->OnPump();
      if (Stopped()) break;
      if (!reading) {
        ::ResetEvent(read.hEvent);
        BOOL ok = ::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &read);
        if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
          if (::GetLastError() == ERROR_MORE_DATA) reason = TransportDisconnect::kProtocolError;
          break;
        }
#ifdef V8HOST_CLIENT_TESTING
        if (!ok && params.test_pending) params.test_pending(params.test_context, false);
#endif
        reading = true;
        if (ok) ::SetEvent(read.hEvent);
      }
      if (!writing) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (!queue.empty()) {
            active = std::move(queue.front());
            queue.pop_front();
          }
        }
        if (!active.bytes.empty()) {
          active.header.conn_id = hello.conn_id;
          active.header.version_major = hello.selected_major;
          active.header.version_minor = hello.selected_minor;
          std::vector<uint8_t> header;
          wire::EncodeHeader(active.header, header);
          std::copy(header.begin(), header.end(), active.bytes.begin());
          ::ResetEvent(write.hEvent);
          BOOL ok = ::WriteFile(pipe, active.bytes.data(), static_cast<DWORD>(active.bytes.size()), nullptr, &write);
          if (!ok && ::GetLastError() != ERROR_IO_PENDING) break;
#ifdef V8HOST_CLIENT_TESTING
          if (!ok && params.test_pending) params.test_pending(params.test_context, true);
#endif
          writing = true;
          if (ok) ::SetEvent(write.hEvent);
        }
      }
      HANDLE events[] = {stop, wake, read.hEvent, write.hEvent};
      DWORD wait = ::WaitForMultipleObjects(writing ? 4 : 3, events, FALSE, INFINITE);
      if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
      // Service both completions, even under a continuous producer wakeup.
      if (reading && ::WaitForSingleObject(read.hEvent, 0) == WAIT_OBJECT_0) {
        DWORD size = 0;
        BOOL ok = ::GetOverlappedResult(pipe, &read, &size, FALSE);
        reading = false;
        if (!ok || !size) {
          if (!ok && ::GetLastError() == ERROR_MORE_DATA) reason = TransportDisconnect::kProtocolError;
          break;
        }
        wire::FrameHeader header;
        const uint8_t* payload = nullptr;
        size_t len = 0;
        if (wire::DecodeAndValidateFrame(buffer.data(), size, &header, &payload, &len) != wire::DecodeStatus::kOk) {
          reason = TransportDisconnect::kProtocolError;
          break;
        }
        if (!Stopped()) delegate->OnInboundFrame(buffer.data(), size);
      }
      if (writing && ::WaitForSingleObject(write.hEvent, 0) == WAIT_OBJECT_0) {
        DWORD size = 0;
        BOOL ok = ::GetOverlappedResult(pipe, &write, &size, FALSE);
        writing = false;
        if (!ok || size != active.bytes.size()) break;
        std::lock_guard<std::mutex> lock(mutex);
        Credit(active);
        active = {};
      }
    }
  drain:
    // Pending operations own their buffers until cancellation completes.
    ::CancelIoEx(pipe, nullptr);
    DWORD ignored = 0;
    if (reading) ::GetOverlappedResult(pipe, &read, &ignored, TRUE);
    if (writing) ::GetOverlappedResult(pipe, &write, &ignored, TRUE);
    if (read.hEvent) ::CloseHandle(read.hEvent);
    if (write.hEvent) ::CloseHandle(write.hEvent);
    return reason;
  }
};

DWORD WINAPI RunPump(void* context) {
  static_cast<Pump*>(context)->Run();
  return 0;
}
DWORD WINAPI CleanupPump(void* context) {
  auto* holder = static_cast<std::shared_ptr<Pump>*>(context);
  std::shared_ptr<Pump> state = std::move(*holder);
  delete holder;
  ::WaitForSingleObject(state->launched, INFINITE);
  if (state->thread) {
    ::WaitForSingleObject(state->thread, INFINITE);
    ::CloseHandle(state->thread);
  }
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->terminal = true;
    state->queue.clear(); state->runs.clear();
    state->controls = state->control_bytes = state->relay_bytes = 0;
  }
  HMODULE module = state->module;
  state->delegate.reset();
  state.reset();
  ::FreeLibraryAndExitThread(module, 0);
}

class PipeClientTransport final : public ClientTransport {
 public:
  explicit PipeClientTransport(TransportParams params)
      : state_(std::make_shared<Pump>(std::move(params))) {}
  ~PipeClientTransport() override { Close(); }
  v8host_status Start() override { return v8host_status_connect_failed; }
  v8host_status Start(std::shared_ptr<ClientTransportDelegate> delegate) override {
    auto& state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.started || state.terminal || state.closed || !state.stop || !state.wake || !state.launched)
      return v8host_status_connect_failed;
    if (!delegate) return v8host_status_invalid_arg;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
         reinterpret_cast<LPCWSTR>(&RunPump), &state.module)) return v8host_status_connect_failed;
    auto* holder = new (std::nothrow) std::shared_ptr<Pump>(state_);
    HANDLE cleanup = holder ? ::CreateThread(nullptr, 0, CleanupPump, holder, 0, nullptr) : nullptr;
    if (!cleanup) {
      delete holder;
      ::FreeLibrary(state.module);
      state.module = nullptr;
      return v8host_status_connect_failed;
    }
    ::CloseHandle(cleanup);
    state.delegate = std::move(delegate);
    state.started = true;
    state.thread = ::CreateThread(nullptr, 0, RunPump, &state, 0, nullptr);
    ::SetEvent(state.launched);
    return state.thread ? v8host_status_ok : v8host_status_connect_failed;
  }
  v8host_status SendFrame(const uint8_t* bytes, size_t len) override {
    wire::FrameHeader header;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    if (wire::DecodeAndValidateFrame(bytes, len, &header, &payload, &payload_len) != wire::DecodeStatus::kOk)
      return v8host_status_protocol_error;
    auto& state = *state_;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.closed || state.terminal) return v8host_status_connect_failed;
    const bool relay = header.type == wire::MessageType::RELAY_TO_WORKER;
    const uint64_t key = (uint64_t{header.target_id} << 32) | header.run_id;
    const size_t charge = len;
    auto it = state.runs.find(key);
    const size_t run_bytes = it == state.runs.end() ? 0 : it->second;
    if (relay) {
      if (charge > wire::kMaxQueuedRelayBytesPerRun - run_bytes ||
          charge > wire::kMaxQueuedRelayBytesPerConnection - state.relay_bytes)
        return v8host_status_quota_exceeded;
    } else if (state.controls >= wire::kMaxControlQueueRequests ||
               charge > wire::kMaxControlQueueBytes - state.control_bytes) {
      return v8host_status_quota_exceeded;
    }
    Outbound item;
    item.bytes.assign(bytes, bytes + len);
    item.header = header;
    item.relay = relay;
    item.charge = charge;
    item.key = key;
    state.queue.push_back(std::move(item));
    if (relay) {
      state.runs[key] += charge;
      state.relay_bytes += charge;
    } else {
      ++state.controls;
      state.control_bytes += charge;
    }
    ::SetEvent(state.wake);
    return v8host_status_ok;
  }
#ifdef V8HOST_CLIENT_TESTING
  size_t QueuedBytesForTesting() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->control_bytes + state_->relay_bytes;
  }
#endif
  uint32_t conn_id() const override { return state_->conn.load(std::memory_order_acquire); }
  void Wake() override { ::SetEvent(state_->wake); }
  void Close() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->closed.store(true, std::memory_order_release);
    if (!state_->started) {
      state_->terminal = true;
      state_->queue.clear(); state_->runs.clear();
      state_->controls = state_->control_bytes = state_->relay_bytes = 0;
    }
    if (state_->stop) ::SetEvent(state_->stop);
  }
 private:
  std::shared_ptr<Pump> state_;
};
}  // namespace

ClientTransport* CreateRealPipeClientTransport(const TransportParams& params,
                                               ClientTransportDelegate*) {
  return new (std::nothrow) PipeClientTransport(params);
}
}  // namespace v8host::client
