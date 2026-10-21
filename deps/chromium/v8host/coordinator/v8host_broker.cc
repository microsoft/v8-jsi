#include "v8host_broker.h"

#include "v8host_file_identity.h"
#include "v8host_payload_identity.h"
#include "v8host_peer_auth.h"
#include "v8host_protocol.h"
#include "v8host_protocol_messages.h"
#include "v8host_router.h"

#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

namespace protocol = v8host::protocol;
namespace router = v8host::coordinator;

// sbox.h strings are sbox_char16, the same UTF-16 code unit as wchar_t here.
static_assert(sizeof(sbox_char16) == sizeof(wchar_t),
              "sbox_char16 must match wchar_t");

using BrokerTextGetter = sbox_status(SBOX_CDECL*)(sbox_broker broker,
                                                  sbox_char16* buffer,
                                                  size_t buffer_length,
                                                  size_t* out_length);

// Copies one host string through the sbox_broker_api length-query protocol.
bool ReadBrokerText(sbox_broker broker,
                    BrokerTextGetter get,
                    std::wstring* out) {
  size_t length = 0;
  if (get(broker, nullptr, 0, &length) != sbox_status_ok ||
      length == SIZE_MAX)
    return false;
  std::wstring text(length + 1, L'\0');
  size_t written = 0;
  if (get(broker, reinterpret_cast<sbox_char16*>(text.data()), text.size(),
          &written) != sbox_status_ok ||
      written != length)
    return false;
  text.resize(length);
  *out = std::move(text);
  return true;
}

bool IoWithDeadline(HANDLE pipe,
                    bool write,
                    void* buffer,
                    DWORD size,
                    DWORD timeout_ms) {
  OVERLAPPED overlapped = {};
  overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!overlapped.hEvent)
    return false;
  DWORD transferred = 0;
  BOOL started =
      write ? ::WriteFile(pipe, buffer, size, nullptr, &overlapped)
            : ::ReadFile(pipe, buffer, size, nullptr, &overlapped);
  bool pending = !started && ::GetLastError() == ERROR_IO_PENDING;
  bool ok = false;
  if (started) {
    ok = ::GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
  } else if (pending &&
             ::WaitForSingleObject(overlapped.hEvent, timeout_ms) ==
                 WAIT_OBJECT_0) {
    ok = ::GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
    pending = false;
  }
  if (pending) {
    ::CancelIoEx(pipe, &overlapped);
    ::GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
  }
  ::CloseHandle(overlapped.hEvent);
  return ok && transferred == size;
}

// Reads exactly one pipe message (a full Contract B frame) into a bounded
// buffer. The transferred size is variable, so success does not require filling
// the buffer. Handshake control frames are small (HELLO is 36 bytes); a message
// larger than the buffer surfaces as ERROR_MORE_DATA and is fatal
// (fail-closed), never reassembled.
bool ReadFrameMessage(HANDLE pipe,
                      uint8_t* buf,
                      DWORD buf_size,
                      DWORD* out_len,
                      DWORD timeout_ms) {
  OVERLAPPED overlapped = {};
  overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!overlapped.hEvent)
    return false;
  DWORD transferred = 0;
  BOOL started = ::ReadFile(pipe, buf, buf_size, nullptr, &overlapped);
  bool pending = !started && ::GetLastError() == ERROR_IO_PENDING;
  bool ok = false;
  if (started) {
    ok = ::GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
  } else if (pending &&
             ::WaitForSingleObject(overlapped.hEvent, timeout_ms) ==
                 WAIT_OBJECT_0) {
    ok = ::GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
    pending = false;
  }
  if (pending) {
    ::CancelIoEx(pipe, &overlapped);
    ::GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
  }
  ::CloseHandle(overlapped.hEvent);
  if (ok)
    *out_len = transferred;
  return ok;
}

class PipeSecurity {
 public:
  ~PipeSecurity() {
    if (descriptor_)
      ::LocalFree(descriptor_);
  }
  bool Initialize(const std::vector<uint8_t>& sid) {
    LPWSTR sid_string = nullptr;
    if (!::ConvertSidToStringSidW(
            const_cast<uint8_t*>(sid.data()), &sid_string))
      return false;
    std::wstring sddl = L"D:P(D;;GA;;;AN)(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;";
    sddl += sid_string;
    sddl += L")";
    ::LocalFree(sid_string);
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr))
      return false;
    attributes_.nLength = sizeof(attributes_);
    attributes_.lpSecurityDescriptor = descriptor_;
    return true;
  }
  SECURITY_ATTRIBUTES* get() { return &attributes_; }

 private:
  PSECURITY_DESCRIPTOR descriptor_ = nullptr;
  SECURITY_ATTRIBUTES attributes_ = {};
};

class BrokerService {
 public:
  BrokerService(sbox_broker broker, const sbox_broker_api* api, const sbox_broker_start* start,
                std::wstring endpoint_name,
                v8host::PayloadIdentity payload,
                std::vector<uint8_t> sid,
                LUID session)
      : broker_(broker), api_(api), start_(start),
        endpoint_name_(std::move(endpoint_name)),
        payload_(std::move(payload)),
        sid_(std::move(sid)),
        session_(session),
        mode_(start->mode == sbox_broker_mode_shared
                  ? v8host::BrokerMode::kShared
                  : v8host::BrokerMode::kDedicated) {}

  sbox_status Run() {
    if (!security_.Initialize(sid_))
      return sbox_status_failed;
    bool first = true;
    bool first_accept = false;
    sbox_status result = sbox_status_ok;
    auto zero_since = std::chrono::steady_clock::now();
    bool zero_timing = false;
    std::chrono::steady_clock::time_point listen_start;
    while (!draining_) {
      ReapConnectionTasks();
      if (mode_ == v8host::BrokerMode::kDedicated && owner_seen_)
        break;
      HANDLE pipe = ::CreateNamedPipeW(
          endpoint_name_.c_str(),
          PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
              (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
          PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
              PIPE_REJECT_REMOTE_CLIENTS,
          mode_ == v8host::BrokerMode::kShared ? PIPE_UNLIMITED_INSTANCES : 1,
          4096, 4096, 0, security_.get());
      if (pipe == INVALID_HANDLE_VALUE) {
        const DWORD pipe_error = ::GetLastError();
        if (first && pipe_error == ERROR_ACCESS_DENIED &&
            mode_ == v8host::BrokerMode::kShared) {
          const bool winner_ok = AuthenticateWinningBroker();
          if (!winner_ok)
            printf("[v8host] shared race: winner authentication failed\n");
          return winner_ok ? sbox_status_ok : sbox_status_failed;
        }
        printf("[v8host] pipe creation failed category=%lu first=%d\n",
               pipe_error, first ? 1 : 0);
        CancelConnections();
        result = sbox_status_failed;
        break;
      }
      if (first)
        listen_start = std::chrono::steady_clock::now();
      first = false;
      OVERLAPPED overlapped = {};
      overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!overlapped.hEvent) {
        ::CloseHandle(pipe);
        CancelConnections();
        result = sbox_status_failed;
        break;
      }
      bool connected = false;
      bool pending = false;
      if (::ConnectNamedPipe(pipe, &overlapped)) {
        connected = true;
      } else if (::GetLastError() == ERROR_PIPE_CONNECTED) {
        connected = true;
      } else if (::GetLastError() == ERROR_IO_PENDING) {
        pending = true;
      }
      while (pending && !draining_) {
        const auto status = V8HostBrokerPollConnect(pipe, &overlapped, 100);
        if (status != V8HostBrokerAcceptStatus::kPending) {
          connected = status == V8HostBrokerAcceptStatus::kConnected;
          pending = false;
        } else {
          ReapConnectionTasks();
          // An unconnected dedicated broker exits after 30 s to reclaim a broker
          // whose launcher died before connecting. A starved client can lose this race.
          if (IdleExpired(&zero_since, &zero_timing) ||
              V8HostBrokerPreConnectExpired(mode_ == v8host::BrokerMode::kDedicated,
                  first_accept, listen_start, std::chrono::steady_clock::now())) {
            connected = V8HostBrokerFinishPendingConnect(pipe, &overlapped);
            pending = false;
            // A completed accept wins over expiry and resets the idle grace.
            if (!connected)
              draining_ = true;
          }
        }
      }
      if (pending)
        connected = V8HostBrokerFinishPendingConnect(pipe, &overlapped);
      ::CloseHandle(overlapped.hEvent);
      if (connected) {
        first_accept = true;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          ++setup_count_;
          zero_timing = false;
        }
        auto context = std::make_unique<ConnectionContext>();
        context->service = this;
        context->pipe = pipe;
        HANDLE task = ::CreateThread(nullptr, 0, &ConnectionTask,
                                     context.get(), 0, nullptr);
        if (task) {
          context.release();
          tasks_.push_back(task);
        } else {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            --setup_count_;
          }
          ::CloseHandle(pipe);
        }
        if (mode_ == v8host::BrokerMode::kDedicated)
          break;
      } else {
        ::CloseHandle(pipe);
      }
    }
    for (HANDLE task : tasks_) {
      ::WaitForSingleObject(task, INFINITE);
      ::CloseHandle(task);
    }
    tasks_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (setup_count_ || connection_count_ || !connections_.empty()) drain_failed_ = true;
    return drain_failed_ ? sbox_status_failed : result;
  }

 private:
  void ReapConnectionTasks() {
    for (auto it = tasks_.begin(); it != tasks_.end();) {
      if (::WaitForSingleObject(*it, 0) == WAIT_OBJECT_0) {
        ::CloseHandle(*it);
        it = tasks_.erase(it);
      } else {
        ++it;
      }
    }
  }

  bool IdleExpired(std::chrono::steady_clock::time_point *zero_since,
                   bool *zero_timing) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Connection refs cover sessions through cleanup; tasks cover final release.
    if (setup_count_ || connection_count_ || !connections_.empty() || !tasks_.empty()) {
      *zero_timing = false;
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!*zero_timing) {
      *zero_since = now;
      *zero_timing = true;
    }
    return mode_ == v8host::BrokerMode::kShared && now - *zero_since >= std::chrono::seconds(5);
  }

  void CancelConnections() {
    std::lock_guard<std::mutex> lock(mutex_);
    draining_ = true;
    for (auto* conn : connections_) conn->Stop();
  }

  struct ConnectionContext {
    BrokerService* service = nullptr;
    HANDLE pipe = INVALID_HANDLE_VALUE;
  };

  static DWORD WINAPI ConnectionTask(void* raw_context) {
    std::unique_ptr<ConnectionContext> context(
        static_cast<ConnectionContext*>(raw_context));
    context->service->ServeConnection(context->pipe);
    return 0;
  }

  bool AuthenticateWinningBroker() {
    const ULONGLONG deadline = ::GetTickCount64() + 5000;
    DWORD delay = 10;
    while (::GetTickCount64() < deadline) {
      HANDLE pipe =
          ::CreateFileW(endpoint_name_.c_str(),
                        GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (pipe != INVALID_HANDLE_VALUE) {
        v8host::PeerAuthPolicy policy = {};
        policy.peer_side = v8host::PeerSide::kPipeServer;
        policy.mode = mode_;
        policy.expected_sid = sid_;
        policy.expected_session = session_;
        policy.install_root = payload_.install_root;
        policy.expected_image = &payload_.container;
        v8host::HeldProcess peer;
        DWORD error = ERROR_SUCCESS;
        const bool ok =
            v8host::AuthenticatePipePeer(pipe, policy, &peer, &error);
        ::CloseHandle(pipe);
        return ok;
      }
      if (::GetLastError() == ERROR_PIPE_BUSY)
        ::WaitNamedPipeW(endpoint_name_.c_str(), delay);
      else
        ::Sleep(delay);
      delay = (std::min)(delay * 2, static_cast<DWORD>(250));
    }
    return false;
  }

  void ServeConnection(HANDLE pipe) {
    v8host::PeerAuthPolicy policy = {};
    policy.peer_side = v8host::PeerSide::kPipeClient;
    policy.mode = mode_;
    policy.expected_sid = sid_;
    policy.expected_session = session_;
    policy.install_root = payload_.install_root;
    // Applications may be unsigned (see PeerAuthPolicy); payload trust is separate.
    policy.allow_unsigned_image = true;
    v8host::HeldProcess peer;
    DWORD error = ERROR_SUCCESS;
    const bool authenticated = v8host::AuthenticatePipePeer(pipe, policy, &peer, &error);
    if (!authenticated)
      printf("[v8host] client authentication failed category=%lu (0x%08lx)\n", error, error);
    router::Connection conn;
    conn.pipe = pipe;
    conn.peer = std::move(peer);
    conn.state = authenticated ? router::ConnState::kNegotiating : router::ConnState::kClosing;
    bool referenced = false;
    bool negotiated = false;
    bool preserve_output = false;
    if (authenticated) {
      uint8_t buffer[512] = {};
      DWORD len = 0;
      protocol::FrameHeader hello_header;
      const uint8_t *payload = nullptr;
      size_t payload_size = 0;
      protocol::HelloPayload hello_payload;
      if (ReadFrameMessage(pipe, buffer, sizeof(buffer), &len, 2000) &&
          protocol::DecodeAndValidateFrame(buffer, len, &hello_header, &payload, &payload_size) ==
              protocol::DecodeStatus::kOk &&
          hello_header.type == protocol::MessageType::HELLO && hello_header.conn_id == 0 &&
          hello_header.target_id == 0 && hello_header.run_id == 0 && hello_header.request_id != 0 &&
          protocol::DecodeHelloPayload(payload, payload_size, &hello_payload)) {
        const uint32_t conn_id = router::AllocateId(next_conn_id_);
        if (conn_id != 0 && conn_id != UINT32_MAX) {
          protocol::BrokerCapabilities caps;
          caps.endpoint_mode = static_cast<uint32_t>(mode_);
          auto negotiation = protocol::NegotiateHello(hello_header, hello_payload, caps, conn_id);
          if (negotiation.decision == protocol::NegotiationDecision::kAck) {
            conn.conn_id = conn_id;
            conn.version_minor = negotiation.selected_minor;
            negotiated = conn.Enqueue(std::move(negotiation.frame));
          } else {
            preserve_output =
                IoWithDeadline(pipe, true, negotiation.frame.data(), static_cast<DWORD>(negotiation.frame.size()), 2000);
          }
        } else {
          CancelConnections();
        }
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --setup_count_;
      if (negotiated) {
        ++connection_count_;
        connections_.insert(&conn);
        if (draining_)
          conn.Stop();
        referenced = true;
        owner_seen_ = true;
        conn.state = router::ConnState::kOpen;
      }
    }
    DWORD needed = ::GetFinalPathNameByHandleW(payload_.container.get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    std::wstring container_path(needed, L'\0');
    DWORD actual = needed ? ::GetFinalPathNameByHandleW(payload_.container.get(), container_path.data(), needed,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
    if (!actual || actual >= needed)
      conn.Stop();
    container_path.resize(actual);
    router::Router service(mode_, broker_, api_, v8host::ParentPath(container_path), payload_.native_machine,
                           payload_.install_root);
    conn.dispatcher = &service;
    if (negotiated && conn.StartWriter()) {
      std::vector<uint8_t> frame(protocol::kMaxFrameSize);
      DWORD len = 0;
      while (conn.ReadFrame(frame.data(), static_cast<DWORD>(frame.size()), &len, INFINITE)) {
        if (!service.Route(conn, frame.data(), len))
          break;
      }
    }
    if (conn.state == router::ConnState::kClosing && negotiated)
      preserve_output = conn.DrainOutput(2000);
    conn.Stop();
    conn.JoinWriter();
    service.Close(conn);
    conn.ClearOutput();
    if (!conn.CompleteZero()) drain_failed_ = true;
    // DisconnectNamedPipe discards unread replies; CloseHandle preserves them.
    if (!preserve_output)
      ::DisconnectNamedPipe(pipe);
    ::CloseHandle(pipe);
    conn.pipe = INVALID_HANDLE_VALUE;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (referenced) {
        connections_.erase(&conn);
        --connection_count_;
      }
      if (mode_ == v8host::BrokerMode::kDedicated)
        draining_ = true;
    }
  }

  sbox_broker broker_;
  const sbox_broker_api* api_;
  const sbox_broker_start* start_;
  std::wstring endpoint_name_;
  v8host::PayloadIdentity payload_;
  std::vector<uint8_t> sid_;
  LUID session_;
  v8host::BrokerMode mode_;
  PipeSecurity security_;
  std::mutex mutex_;
  std::vector<HANDLE> tasks_;
  std::unordered_set<router::Connection*> connections_;
  size_t setup_count_ = 0;
  size_t connection_count_ = 0;
  std::atomic<bool> drain_failed_{false};
  std::atomic<bool> owner_seen_{false};
  std::atomic<bool> draining_{false};
  // Process-unique, nonzero, monotonic connection id (design §8.3: never reused
  // within this broker process).
  std::atomic<uint32_t> next_conn_id_{1};
};

#if defined(SBOX_DEV_ALLOW_UNSIGNED)
// Dev-only PID checks for live and naturally exited workers, retained until wait.
sbox_status RunBrokerApiProcessIdTest(sbox_broker broker,
                                      const sbox_broker_api* api) {
  uint32_t pid = 123;
  if (api->get_process_id(nullptr, &pid) != sbox_status_invalid_arg || pid ||
      api->get_process_id(nullptr, nullptr) != sbox_status_invalid_arg)
    return sbox_status_failed;
  for (bool exited : {false, true}) {
    // A missing engine makes warmup exit without a host close or termination.
    ::SetLastError(ERROR_SUCCESS);
    const DWORD saved_length = ::GetEnvironmentVariableW(L"V8HOST_ENGINE_DLL", nullptr, 0);
    const DWORD saved_error = ::GetLastError();
    if (!saved_length && saved_error != ERROR_SUCCESS &&
        saved_error != ERROR_ENVVAR_NOT_FOUND)
      return sbox_status_failed;
    const bool had_engine = saved_length || saved_error != ERROR_ENVVAR_NOT_FOUND;
    std::vector<wchar_t> saved_engine(saved_length ? saved_length : 1, L'\0');
    if (saved_length && ::GetEnvironmentVariableW(L"V8HOST_ENGINE_DLL",
            saved_engine.data(), saved_length) != saved_length - 1)
      return sbox_status_failed;
    if (exited && !::SetEnvironmentVariableW(
            L"V8HOST_ENGINE_DLL", L"missing-pid-test-engine.dll"))
      return sbox_status_failed;
    sbox_broker_worker worker = nullptr;
    const auto spawned = api->configure_and_spawn(broker, nullptr, 0, nullptr, nullptr, &worker);
    const bool restored = !exited || ::SetEnvironmentVariableW(
        L"V8HOST_ENGINE_DLL", had_engine ? saved_engine.data() : nullptr);
    if (spawned != sbox_status_ok || !worker)
      return sbox_status_failed;
    pid = 123;
    bool ok = restored && api->get_process_id(worker, nullptr) == sbox_status_invalid_arg &&
        api->get_process_id(worker, &pid) == sbox_status_ok && pid &&
        pid != ::GetCurrentProcessId();
    HANDLE process = ok ? ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    wchar_t image[MAX_PATH] = {};
    DWORD image_size = MAX_PATH;
    ok = process && ::GetProcessId(process) == pid &&
        ::QueryFullProcessImageNameW(process, 0, image, &image_size) &&
        std::wstring(image).ends_with(L"\\sbox.exe") && ok;
    if (ok)
      ok = ::WaitForSingleObject(process, exited ? 10000 : 0) ==
          (exited ? WAIT_OBJECT_0 : WAIT_TIMEOUT);
    uint32_t retained = 0;
    ok = api->get_process_id(worker, &retained) == sbox_status_ok && retained == pid && ok;
    if (process) ::CloseHandle(process);
    const auto closed = api->close(worker);
    int32_t exit_code = 0;
    const auto waited = api->wait(worker, &exit_code);
    // wait invalidates worker, even when a preceding assertion failed.
    if (!ok || closed != sbox_status_ok || waited != sbox_status_ok ||
        (exited && exit_code == 0))
      return sbox_status_failed;
  }
  return sbox_status_ok;
}

// Dev-only sbox_broker_api tests selected by V8HOST_BROKER_API_TEST;
// compiled out of release builds. "process-id" checks worker PID lifetime.
// Other modes first prove argument rejection (no spawn). "ownership" runs a
// clean spawn / post / idempotent-close / single-wait cycle and returns ok
// (exit 0). "leak" abandons a worker so the generic host's post-run cleanup
// must close/wait it and report the breach.
sbox_status RunBrokerApiOwnershipTest(sbox_broker broker,
                                      const sbox_broker_api* api,
                                      const wchar_t* mode) {
  if (std::wcscmp(mode, L"process-id") == 0)
    return RunBrokerApiProcessIdTest(broker, api);
  sbox_broker_worker worker = nullptr;
  int32_t exit_code = 0;
  const std::vector<uint8_t> oversize(64 * 1024 + 1, 0);
  size_t text_length = 0;
  sbox_char16 short_buffer[1] = {};
  if (api->configure_and_spawn(nullptr, nullptr, 0, nullptr, nullptr,
                               &worker) != sbox_status_invalid_arg ||
      api->configure_and_spawn(broker, nullptr, 0, nullptr, nullptr, nullptr) !=
          sbox_status_invalid_arg ||
      api->configure_and_spawn(broker, oversize.data(), oversize.size(),
                               nullptr, nullptr, &worker) != sbox_status_invalid_arg ||
      api->post_message(nullptr, sbox_msg_kind_string, "x", 1) != sbox_status_invalid_arg ||
      api->close(nullptr) != sbox_status_invalid_arg ||
      api->wait(nullptr, &exit_code) != sbox_status_invalid_arg ||
      api->get_endpoint_name(nullptr, nullptr, 0, &text_length) !=
          sbox_status_invalid_arg ||
      api->get_endpoint_name(broker, nullptr, 0, nullptr) !=
          sbox_status_invalid_arg ||
      api->get_endpoint_name(broker, nullptr, 1, &text_length) !=
          sbox_status_invalid_arg ||
      api->get_endpoint_name(broker, nullptr, 0, &text_length) !=
          sbox_status_ok ||
      text_length == 0 ||
      api->get_endpoint_name(broker, short_buffer, 1, &text_length) !=
          sbox_status_invalid_arg ||
      short_buffer[0] != 0 || text_length == 0)
    return sbox_status_failed;
  std::wstring endpoint_name;
  if (!ReadBrokerText(broker, api->get_endpoint_name, &endpoint_name) ||
      endpoint_name.size() != text_length ||
      endpoint_name.find(L'\0') != std::wstring::npos)
    return sbox_status_failed;

  if (mode[0] == L'l') {  // "leak": abandon a spawned worker for host cleanup.
    worker = nullptr;
    if (api->configure_and_spawn(broker, nullptr, 0, nullptr, nullptr,
                                 &worker) != sbox_status_ok ||
        !worker)
      return sbox_status_failed;
    return sbox_status_ok;
  }

  worker = nullptr;
  if (api->configure_and_spawn(broker, nullptr, 0, nullptr, nullptr, &worker) !=
          sbox_status_ok ||
      !worker)
    return sbox_status_failed;
  api->post_message(worker, sbox_msg_kind_string, "ping", 4);
  if (api->close(worker) != sbox_status_ok || api->close(worker) != sbox_status_ok)
    return sbox_status_failed;  // close is idempotent
  if (api->wait(worker, &exit_code) != sbox_status_ok)
    return sbox_status_failed;  // single destructive wait
  return sbox_status_ok;
}
#endif  // SBOX_DEV_ALLOW_UNSIGNED

}  // namespace

sbox_status V8HostBrokerRun(sbox_broker broker,
                            const sbox_broker_api* api,
                            const sbox_broker_start* start) {
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
  wchar_t test_mode[16] = {};
  if (::GetEnvironmentVariableW(L"V8HOST_BROKER_API_TEST", test_mode, 16) > 0 &&
      broker && api && api->struct_size >= sizeof(*api) &&
      api->configure_and_spawn && api->post_message && api->close &&
      api->wait && api->get_endpoint_name && api->get_process_id)
    return RunBrokerApiOwnershipTest(broker, api, test_mode);
#endif
  if (!broker || !api || api->struct_size < sizeof(*api) ||
      !api->configure_and_spawn || !api->post_message || !api->close ||
      !api->wait || !api->get_endpoint_name ||
      !api->get_container_final_path || !api->get_plugin_final_path ||
      !api->get_process_id ||
      !start || start->struct_size < sizeof(*start) ||
      !start->container_sha256 || !start->plugin_sha256 ||
      (start->mode != sbox_broker_mode_shared &&
       start->mode != sbox_broker_mode_dedicated))
    return sbox_status_invalid_arg;
  std::wstring endpoint_name;
  std::wstring container_final_path;
  std::wstring plugin_final_path;
  if (!ReadBrokerText(broker, api->get_endpoint_name, &endpoint_name) ||
      !ReadBrokerText(broker, api->get_container_final_path,
                      &container_final_path) ||
      !ReadBrokerText(broker, api->get_plugin_final_path, &plugin_final_path))
    return sbox_status_invalid_arg;

  DWORD error = ERROR_SUCCESS;
  v8host::PayloadIdentity payload;
  if (!v8host::OpenImmutableFile(container_final_path, &payload.container,
                                 &error) ||
      !v8host::OpenImmutableFile(plugin_final_path, &payload.plugin, &error) ||
      payload.container.machine() != start->native_machine ||
      payload.plugin.machine() != start->native_machine ||
      std::memcmp(payload.container.sha256().data(), start->container_sha256,
                  32) != 0 ||
      std::memcmp(payload.plugin.sha256().data(), start->plugin_sha256, 32) !=
          0 ||
      !v8host::TrustAllowed(v8host::VerifyTrust(payload.container)) ||
      !v8host::TrustAllowed(v8host::VerifyTrust(payload.plugin)) ||
      !v8host::ComputePluginSetId(
          start->native_machine, payload.container.sha256(),
          payload.plugin.sha256(), &payload.plugin_set_id))
    return sbox_status_failed;
  payload.native_machine = start->native_machine;
  if (!v8host::DeriveInstallRoot(payload.container.final_path(),
                                 payload.native_machine, &payload.install_root) ||
      !v8host::IsPayloadFileAllowed(payload.install_root, payload.container) ||
      !v8host::IsPayloadFileAllowed(payload.install_root, payload.plugin))
    return sbox_status_failed;
  std::vector<uint8_t> sid;
  LUID session = {};
  if (!v8host::QueryCurrentSidAndSession(&sid, &session, &error))
    return sbox_status_failed;
  std::array<uint8_t, 16> nonce = {};
  const std::array<uint8_t, 16>* nonce_ptr = nullptr;
  if (start->mode == sbox_broker_mode_dedicated) {
    if (!start->dedicated_nonce || start->dedicated_nonce_size != nonce.size())
      return sbox_status_invalid_arg;
    std::memcpy(nonce.data(), start->dedicated_nonce, nonce.size());
    nonce_ptr = &nonce;
  }
  std::wstring expected_endpoint;
  std::array<uint8_t, 32> endpoint_key = {};
  if (!v8host::DeriveEndpoint(
          sid, payload.plugin_set_id, payload.install_root,
          start->mode == sbox_broker_mode_shared
              ? v8host::BrokerMode::kShared
              : v8host::BrokerMode::kDedicated,
          nonce_ptr, &expected_endpoint, &endpoint_key) ||
      expected_endpoint != endpoint_name)
    return sbox_status_failed;
  printf("[v8host] broker endpoint=%s mode=%s pid=%lu\n",
         v8host::HexPrefix(endpoint_key, 6).c_str(),
         start->mode == sbox_broker_mode_shared ? "shared" : "dedicated",
         ::GetCurrentProcessId());
  return BrokerService(broker, api, start, std::move(endpoint_name),
                       std::move(payload), std::move(sid), session)
      .Run();
}
