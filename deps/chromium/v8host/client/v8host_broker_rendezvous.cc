#include "v8host_broker_rendezvous.h"

#include "v8host_peer_auth.h"
#include "v8host_protocol.h"
#include "v8host_protocol_messages.h"

#include <algorithm>
#include <utility>

namespace v8host {
namespace {

DWORD Remaining(ULONGLONG deadline, DWORD cap) {
  const ULONGLONG now = ::GetTickCount64();
  return now >= deadline ? 0 : static_cast<DWORD>((std::min)(deadline - now, ULONGLONG{cap}));
}
bool WaitIo(HANDLE event, HANDLE stop, DWORD timeout) {
  HANDLE events[] = {stop, event};
  return stop ? ::WaitForMultipleObjects(2, events, FALSE, timeout) == WAIT_OBJECT_0 + 1
              : ::WaitForSingleObject(event, timeout) == WAIT_OBJECT_0;
}
bool Stopped(HANDLE stop) {
  return stop && ::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0;
}

constexpr DWORD kCandidateReapMs = 2000;

// Terminate only our unconnected dedicated candidate (pre-connect); never shared
// candidates, which may be the canonical broker.
struct LaunchedCandidate {
  LaunchedCandidate(HANDLE process, BrokerMode mode) : process(process), mode(mode) {}
  LaunchedCandidate(const LaunchedCandidate&) = delete;
  LaunchedCandidate& operator=(const LaunchedCandidate&) = delete;
  LaunchedCandidate(LaunchedCandidate&&) = delete;
  LaunchedCandidate& operator=(LaunchedCandidate&&) = delete;
  ~LaunchedCandidate() {
    if (!connected && mode == BrokerMode::kDedicated) {
      ::TerminateProcess(process, ERROR_CANCELLED);
      ::WaitForSingleObject(process, kCandidateReapMs);
    }
    ::CloseHandle(process);
  }
  HANDLE process;
  BrokerMode mode;
  bool connected = false;
};

bool IoWithDeadline(HANDLE pipe,
                    bool write,
                    void* buffer,
                    DWORD size,
                    DWORD timeout_ms, HANDLE stop = nullptr) {
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
             WaitIo(overlapped.hEvent, stop, timeout_ms)) {
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
// buffer. Unlike IoWithDeadline, the transferred size is variable, so success
// does not require filling the buffer. Handshake control frames are small
// (HELLO_ACK is 72 bytes, ERROR is tiny): a message larger than the buffer
// surfaces as ERROR_MORE_DATA and is treated as fatal (fail-closed), never
// reassembled.
bool ReadFrameMessage(HANDLE pipe,
                      uint8_t* buf,
                      DWORD buf_size,
                      DWORD* out_len,
                      DWORD timeout_ms, HANDLE stop = nullptr) {
  OVERLAPPED overlapped = {};
  overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!overlapped.hEvent)
    return false;
  DWORD transferred = 0;
  BOOL started = ::ReadFile(pipe, buf, buf_size, nullptr, &overlapped);
  bool pending = !started && ::GetLastError() == ERROR_IO_PENDING;
  DWORD failure = started ? ERROR_SUCCESS : ::GetLastError();
  bool ok = false;
  if (started) {
    ok = ::GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
  } else if (pending &&
             WaitIo(overlapped.hEvent, stop, timeout_ms)) {
    ok = ::GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
    if (!ok) failure = ::GetLastError();
    pending = false;
  }
  if (!ok && !pending && started) failure = ::GetLastError();
  if (pending) {
    ::CancelIoEx(pipe, &overlapped);
    ::GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
  }
  ::CloseHandle(overlapped.hEvent);
  if (ok)
    *out_len = transferred;
  if (!ok) ::SetLastError(failure);
  return ok;
}

std::wstring Quote(const std::wstring& value) {
  return L"\"" + value + L"\"";
}

std::wstring NonceHex(const std::array<uint8_t, 16>& nonce) {
  static constexpr wchar_t kHex[] = L"0123456789abcdef";
  std::wstring result;
  result.reserve(32);
  for (uint8_t byte : nonce) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0xf]);
  }
  return result;
}

}  // namespace

struct BrokerConnection::State {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  HeldProcess server;
  HeldFile plugin;
  std::wstring endpoint;
  BrokerMode mode = BrokerMode::kShared;
};

BrokerConnection::BrokerConnection() : state_(std::make_unique<State>()) {}
BrokerConnection::~BrokerConnection() {
  Close();
}
BrokerConnection::BrokerConnection(BrokerConnection&&) noexcept = default;
BrokerConnection& BrokerConnection::operator=(BrokerConnection&&) noexcept =
    default;

HANDLE BrokerConnection::pipe() const {
  return state_ ? state_->pipe : INVALID_HANDLE_VALUE;
}

DWORD BrokerConnection::broker_pid() const {
  return state_ && state_->server ? state_->server.pid() : 0;
}

const std::wstring& BrokerConnection::endpoint() const {
  static const std::wstring empty;
  return state_ ? state_->endpoint : empty;
}

void BrokerConnection::Close() {
  if (state_ && state_->pipe != INVALID_HANDLE_VALUE) {
    ::CancelIoEx(state_->pipe, nullptr);
    ::CloseHandle(state_->pipe);
    state_->pipe = INVALID_HANDLE_VALUE;
  }
}

#ifdef V8HOST_CLIENT_TESTING
void BrokerConnection::AdoptForTesting(HANDLE pipe) {
  Close();
  state_->pipe = pipe;
}
#endif

RendezvousStatus BrokerConnection::Handshake(uint32_t request_id,
                                             HelloResult* result, HANDLE stop, ULONGLONG deadline) {
  if (!deadline) deadline = ::GetTickCount64() + 4000;
  if (Stopped(stop) || !Remaining(deadline, 1)) return RendezvousStatus::kStartTimeout;
  if (!result || pipe() == INVALID_HANDLE_VALUE || request_id == 0)
    return RendezvousStatus::kInvalidArgument;

  // Contract B §8.2: HELLO is the first frame after server auth, with zero
  // addressing ids and a nonzero request id. The builder stamps type + length.
  protocol::FrameHeader request_header;
  request_header.version_major = protocol::kWireVersionMajor;
  request_header.version_minor = protocol::kWireVersionMinor;
  request_header.conn_id = 0;
  request_header.target_id = 0;
  request_header.run_id = 0;
  request_header.request_id = request_id;
  std::vector<uint8_t> frame = protocol::BuildHelloFrame(request_header);
  if (!IoWithDeadline(pipe(), /*write=*/true, frame.data(),
                      static_cast<DWORD>(frame.size()), Remaining(deadline, 2000), stop)) {
    return RendezvousStatus::kIoFailed;
  }

  uint8_t buffer[512] = {};
  DWORD len = 0;
  if (!ReadFrameMessage(pipe(), buffer, sizeof(buffer), &len, Remaining(deadline, 2000), stop))
    return ::GetLastError() == ERROR_MORE_DATA ? RendezvousStatus::kProtocolFailed
                                             : RendezvousStatus::kIoFailed;

  protocol::FrameHeader ack_header;
  const uint8_t* payload = nullptr;
  size_t payload_size = 0;
  if (protocol::DecodeAndValidateFrame(buffer, len, &ack_header, &payload,
                                       &payload_size) !=
      protocol::DecodeStatus::kOk) {
    return RendezvousStatus::kProtocolFailed;
  }
  if (ack_header.type == protocol::MessageType::ERROR) {
    // A broker reject during negotiation (e.g. a major-version mismatch).
    // Unreachable at wire v1.0 since both peers speak the same major; treated
    // as a failed rendezvous.
    return RendezvousStatus::kPeerAuthenticationFailed;
  }
  if (ack_header.type != protocol::MessageType::HELLO_ACK)
    return RendezvousStatus::kProtocolFailed;

  protocol::HelloAckPayload ack_payload;
  if (!protocol::DecodeHelloAckPayload(payload, payload_size, &ack_payload))
    return RendezvousStatus::kProtocolFailed;
  if (ack_header.request_id != request_id || ack_header.conn_id == 0 ||
      ack_header.version_major != protocol::kWireVersionMajor ||
      ack_header.version_minor > protocol::kWireVersionMinor ||
      ack_header.target_id != 0 || ack_header.run_id != 0 ||
      ack_payload.endpoint_mode != static_cast<uint32_t>(state_->mode)) {
    return RendezvousStatus::kProtocolFailed;
  }

  result->conn_id = ack_header.conn_id;
  result->selected_major = ack_header.version_major;
  result->selected_minor = ack_header.version_minor;
  result->broker_max_major = ack_payload.broker_version_major;
  result->broker_max_minor = ack_payload.broker_version_minor;
  result->endpoint_mode = ack_payload.endpoint_mode;
  result->request_id = request_id;
  return RendezvousStatus::kOk;
}

BrokerRendezvous::BrokerRendezvous(std::wstring payload_directory,
                                   BrokerMode mode)
    : payload_directory_(std::move(payload_directory)), mode_(mode) {}

RendezvousStatus BrokerRendezvous::Initialize() {
  DWORD error = ERROR_SUCCESS;
  if (!ResolvePayloadIdentity(payload_directory_, L"sbox.exe", L"v8host.dll",
                              &payload_, &error))
    return RendezvousStatus::kPayloadInvalid;
  if (!QueryCurrentSidAndSession(&sid_, &session_, &error))
    return RendezvousStatus::kPeerAuthenticationFailed;
  const std::array<uint8_t, 16>* nonce = nullptr;
  if (mode_ == BrokerMode::kDedicated) {
    if (!GenerateNonce(&nonce_))
      return RendezvousStatus::kEndpointInvalid;
    nonce = &nonce_;
  }
  if (!DeriveEndpoint(sid_, payload_.plugin_set_id, payload_.install_root, mode_, nonce, &endpoint_,
                      &endpoint_key_))
    return RendezvousStatus::kEndpointInvalid;
  initialized_ = true;
  return RendezvousStatus::kOk;
}

RendezvousStatus BrokerRendezvous::TryConnect(
    HANDLE launched_process,
    BrokerConnection* connection) {
  HANDLE pipe =
      ::CreateFileW(endpoint_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (pipe == INVALID_HANDLE_VALUE)
    return RendezvousStatus::kStartTimeout;
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
    ::CloseHandle(pipe);
    return RendezvousStatus::kIoFailed;
  }
  PeerAuthPolicy policy = {};
  policy.peer_side = PeerSide::kPipeServer;
  policy.mode = mode_;
  policy.expected_sid = sid_;
  policy.expected_session = session_;
  policy.install_root = payload_.install_root;
  policy.expected_image = &payload_.container;
  policy.launched_process = launched_process;
  HeldProcess server;
  DWORD error = ERROR_SUCCESS;
  if (!AuthenticatePipePeer(pipe, policy, &server, &error)) {
    ::CloseHandle(pipe);
    return RendezvousStatus::kPeerAuthenticationFailed;
  }
  HeldFile plugin;
  if (!OpenImmutableFile(payload_.plugin.final_path(), &plugin, &error) ||
      !IsSameFileIdentity(plugin, payload_.plugin) ||
      !TrustAllowed(VerifyTrust(plugin))) {
    ::CloseHandle(pipe);
    return RendezvousStatus::kPeerAuthenticationFailed;
  }
  connection->Close();
  connection->state_->pipe = pipe;
  connection->state_->server = std::move(server);
  connection->state_->plugin = std::move(plugin);
  connection->state_->endpoint = endpoint_;
  connection->state_->mode = mode_;
  return RendezvousStatus::kOk;
}

RendezvousStatus BrokerRendezvous::LaunchCandidate(
    PROCESS_INFORMATION* process) {
  const std::wstring executable =
      payload_directory_ +
      (payload_directory_.back() == L'\\' ? L"" : L"\\") + L"sbox.exe";
  std::wstring command =
      Quote(executable) + L" --broker --mode=" +
      (mode_ == BrokerMode::kShared ? L"shared" : L"dedicated") +
      L" --pipe=" + Quote(endpoint_) + L" --plugin v8host.dll";
  if (mode_ == BrokerMode::kDedicated)
    command += L" --nonce=" + NonceHex(nonce_);
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  *process = {};
  if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                        FALSE, 0, nullptr,
                        payload_directory_.c_str(), &startup, process))
    return RendezvousStatus::kLaunchFailed;
  return RendezvousStatus::kOk;
}

#ifdef V8HOST_CLIENT_TESTING
void BrokerRendezvous::SetLaunchHookForTesting(LaunchHook hook, void* context) {
  test_launch_ = hook;
  test_context_ = context;
}
#endif

RendezvousStatus BrokerRendezvous::ConnectOrLaunch(
    BrokerConnection* connection, HANDLE stop, ULONGLONG* deadline) {
  ULONGLONG local = 0;
  ULONGLONG& limit = deadline ? *deadline : local;
  if (!limit) limit = ::GetTickCount64() + 5000;
  if (Stopped(stop) || !Remaining(limit, 1)) return RendezvousStatus::kStartTimeout;
  if (!connection)
    return RendezvousStatus::kInvalidArgument;
  if (!initialized_) {
    RendezvousStatus status = Initialize();
    if (status != RendezvousStatus::kOk)
      return status;
  }
  if (Stopped(stop) || !Remaining(limit, 1)) return RendezvousStatus::kStartTimeout;
  if (mode_ == BrokerMode::kShared) {
    RendezvousStatus existing = TryConnect(nullptr, connection);
    if (existing == RendezvousStatus::kOk)
      return existing;
    if (existing == RendezvousStatus::kPeerAuthenticationFailed)
      return existing;
  }
  if (Stopped(stop) || !Remaining(limit, 1)) return RendezvousStatus::kStartTimeout;
  PROCESS_INFORMATION candidate = {};
#ifdef V8HOST_CLIENT_TESTING
  if (test_launch_) test_launch_(test_context_, LaunchHookPoint::kBeforeCreate, nullptr, limit, endpoint_.c_str());
#endif
  const ULONGLONG launch_start = ::GetTickCount64();
#ifdef V8HOST_CLIENT_TESTING
  if (test_launch_) test_launch_(test_context_, LaunchHookPoint::kLaunching, nullptr, limit, endpoint_.c_str());
#endif
  RendezvousStatus status = LaunchCandidate(&candidate);
#ifdef V8HOST_CLIENT_TESTING
  if (status == RendezvousStatus::kOk && test_launch_)
    test_launch_(test_context_, LaunchHookPoint::kCreated, candidate.hProcess, limit, endpoint_.c_str());
#endif
  limit += ::GetTickCount64() - launch_start;
  if (status != RendezvousStatus::kOk)
    return status;
  ::CloseHandle(candidate.hThread);
  LaunchedCandidate owned(candidate.hProcess, mode_);
  DWORD delay = 10;
  while (!Stopped(stop) && ::GetTickCount64() < limit) {
    HANDLE binding = candidate.hProcess;
    DWORD exit_code = STILL_ACTIVE;
    if (::GetExitCodeProcess(candidate.hProcess, &exit_code) &&
        exit_code != STILL_ACTIVE) {
      if (mode_ == BrokerMode::kDedicated || exit_code != 0) {
        return RendezvousStatus::kLaunchFailed;
      }
      binding = nullptr;
    }
    status = TryConnect(binding, connection);
    if (status == RendezvousStatus::kOk) {
      owned.connected = true;
      return status;
    }
    if (status == RendezvousStatus::kPeerAuthenticationFailed) {
      if (mode_ == BrokerMode::kShared &&
          WaitIo(candidate.hProcess, stop, Remaining(limit, 500))) {
        DWORD loser_exit = 1;
        if (::GetExitCodeProcess(candidate.hProcess, &loser_exit) &&
            loser_exit == 0) {
          if (stop) ::WaitForSingleObject(stop, Remaining(limit, delay));
          else ::Sleep(Remaining(limit, delay));
          continue;
        }
      }
      return status;
    }
    if (stop) ::WaitForSingleObject(stop, Remaining(limit, delay));
    else ::Sleep(Remaining(limit, delay));
    delay = (std::min)(delay * 2, static_cast<DWORD>(250));
  }
  return RendezvousStatus::kStartTimeout;
}

}  // namespace v8host
