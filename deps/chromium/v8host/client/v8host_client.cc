// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "v8host_client.h"

#include "v8host_client_transport.h"
#include "v8host_dispatcher.h"
#include "v8host_window_tokens.h"
#include "v8host_payload_identity.h"
#include "v8host_protocol.h"
#include "v8host_protocol_messages.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace v8host::client {
class ClientConnection;
}

struct v8host_session_s;
struct v8host_runtime_s;

struct v8host_run_s {
  v8host_session_s* session = nullptr;
  v8host_runtime_s* runtime = nullptr;  // a runtime's run (session is null)
  uint32_t run_id = 0;
  v8host::client::RunTerminalArbiter arbiter;
  std::atomic<bool> terminal_local{false};
};

struct FileRuleCopy {
  std::wstring pattern;
  int32_t readonly = 0;
};

struct SessionConfigCopy {
  int32_t broker_mode = 0;
  int32_t tier = 0;
  int32_t integrity = 0;
  int32_t delayed_integrity = 0;
  int32_t initial_token = v8host_token_level_restricted_same_access;
  int32_t lockdown_token = 0;
  int32_t prohibit_dynamic_code = 0;
  std::vector<FileRuleCopy> file_rules;
  int32_t use_app_container = 0;
  int32_t low_privilege_app_container = 0;
  std::wstring app_container_profile_name;
  std::vector<std::wstring> capabilities;
};

struct v8host_config_s {
  SessionConfigCopy config;
  v8host_status status = v8host_status_ok;
};

struct v8host_run_config_s {
  bool has_tier = false;
  int32_t tier = 0;
  bool has_engine = false;
  std::wstring engine;
  bool has_snapshot = false;
  std::wstring snapshot;
  std::vector<uint8_t> payload;
  v8host_status status = v8host_status_ok;
};

// A worker builder is a session policy builder plus the worker-only fields, so
// the mirrored policy setters reuse the session setters as they are.
struct v8host_worker_config_s : v8host_config_s {
  std::wstring engine;
  std::vector<std::wstring> snapshots;
  uint32_t max_runtimes = 1;
  uint32_t flags = 0;
};

struct v8host_broker_config_s {
  int32_t mode = 0;
  v8host_broker_callbacks table{};
  v8host_status status = v8host_status_ok;
};

struct v8host_runtime_config_s {
  bool has_snapshot = false;
  uint32_t snapshot_index = 0;
  v8host_status status = v8host_status_ok;
};

// The broker graph. The broker's one mutex guards every worker, runtime, and
// run in it, so the parent-before-child lock order holds trivially. Each object
// is reference counted: the host holds one reference until it closes the
// handle, a parent's map holds one while the child is live, a child holds one
// on its parent, and each queued callback holds one on the object it names.
// The last release deletes on the app thread.
struct v8host_worker_s;
struct v8host_broker_s {
  std::atomic<int> refcount{1};
  std::shared_ptr<v8host::client::ClientConnection> conn;
  int32_t mode = 0;
  v8host::client::SessionDispatch* dispatch = nullptr;
  std::mutex mutex;
  bool released = false;  // the host closed it
  bool terminal = false;  // disconnected or closed: no more events
  uint32_t next_id = 1;
  uint32_t open_workers = 0;  // workers the host hasn't closed
  std::unordered_map<uint32_t, v8host_worker_s*> workers;
  std::unordered_map<uint32_t, v8host_runtime_s*> runtimes;

  void AddRef() { refcount.fetch_add(1, std::memory_order_relaxed); }
  void Release();
};

struct v8host_worker_s {
  std::atomic<int> refcount{1};
  v8host_broker_s* broker = nullptr;
  uint32_t id = 0;
  std::atomic<uint32_t> process_id{0};
  bool released = false;
  bool closed = false;   // closed by the host or its broker, or terminal
  bool closing = false;  // close_when_empty sent CLOSE_WORKER
  bool orphaned = false; // the host closed its broker: getters return 0
  uint32_t max_runtimes = 1;
  uint32_t flags = 0;
  uint32_t snapshot_count = 0;
  uint32_t admissions = 0;  // open runtimes, as the coordinator counts them
  uint32_t created = 0;     // runtimes ever opened (single_use)
  std::unordered_map<uint32_t, v8host_runtime_s*> runtimes;

  void AddRef() { refcount.fetch_add(1, std::memory_order_relaxed); }
  void Release();
  ~v8host_worker_s() {
    if (broker)
      broker->Release();
  }
};

struct v8host_runtime_s {
  std::atomic<int> refcount{1};
  v8host_worker_s* worker = nullptr;
  uint32_t id = 0;
  bool released = false;
  bool closed = false;    // closed by the host or a parent, or terminal
  bool silenced = false;  // its callback queue overflowed: no more events
  bool orphaned = false;  // the host closed its broker: getters return 0
  std::unordered_map<uint32_t, std::unique_ptr<v8host_run_s>> runs;
  uint32_t next_run_id = 1;

  void AddRef() { refcount.fetch_add(1, std::memory_order_relaxed); }
  void Release();
  ~v8host_runtime_s() {
    if (worker)
      worker->Release();
  }
};

struct v8host_session_s {
  std::atomic<int> refcount{1};
  std::shared_ptr<v8host::client::ClientConnection> conn;
  uint32_t session_id = 0;
  SessionConfigCopy config;
  v8host::client::SessionDispatch* dispatch = nullptr;
  uint64_t dispatch_id = 0;
  std::mutex mutex;
  std::unordered_map<uint32_t, std::unique_ptr<v8host_run_s>> runs;
  uint32_t next_run_id = 1;
  bool closed = false;

  void AddRef() { refcount.fetch_add(1, std::memory_order_relaxed); }
  void Release();
};

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace v8host::client {
namespace {

constexpr UINT kDestroySessionMessage = WM_APP + 0x1E;
constexpr UINT kStartTransportMessage = WM_APP + 0x1F;
constexpr UINT kDestroyObjectMessage = WM_APP + 0x20;
void StartConnection(ClientConnection* connection);

HINSTANCE ModuleInstance() {
  return reinterpret_cast<HINSTANCE>(&__ImageBase);
}

#ifdef V8HOST_CLIENT_TESTING
extern std::atomic<void (*)()> g_initialize_after_dispatcher_hook;
#endif

class ClientRuntime {
 public:
  static ClientRuntime& Instance() {
    static ClientRuntime* const instance = new ClientRuntime();
    return *instance;
  }

  v8host_status Initialize() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      switch (state_) {
        case State::kReady:
          return v8host_status_ok;
        case State::kFailed:
          return v8host_status_internal_error;
        case State::kInitializing:
          cv_.wait(lock, [this] { return state_ != State::kInitializing; });
          continue;
        case State::kUninitialized:
          state_ = State::kInitializing;
          break;
      }
      break;
    }
    lock.unlock();

    v8host_status status = Dispatcher::Instance().Initialize();
    if (status == v8host_status_ok &&
        Dispatcher::Instance().callback_thread_id() != ::GetCurrentThreadId()) {
      status = v8host_status_internal_error;
    }
#ifdef V8HOST_CLIENT_TESTING
    if (status == v8host_status_ok) {
      void (*hook)() =
          g_initialize_after_dispatcher_hook.load(std::memory_order_acquire);
      if (hook != nullptr)
        hook();
    }
#endif
    if (status == v8host_status_ok)
      status = CreateLifecycleWindow();

    lock.lock();
    state_ = status == v8host_status_ok ? State::kReady : State::kFailed;
    initialized_.store(status == v8host_status_ok, std::memory_order_release);
    cv_.notify_all();
    return status;
  }

  uint32_t lifecycle_thread_id() const {
    return lifecycle_thread_id_.load(std::memory_order_acquire);
  }

  uint32_t last_delete_thread_id() const {
    return last_delete_thread_id_.load(std::memory_order_acquire);
  }

 private:
  enum class State : uint8_t {
    kUninitialized,
    kInitializing,
    kReady,
    kFailed
  };

  v8host_status CreateLifecycleWindow() {
    if (!tokens_.Initialize()) return v8host_status_internal_error;
    class_name_ =
        L"V8HostClientLifecycle_" + std::to_wstring(reinterpret_cast<uintptr_t>(this));
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WindowProc;
    wc.hInstance = ModuleInstance();
    wc.lpszClassName = class_name_.c_str();
    atom_ = ::RegisterClassExW(&wc);
    if (atom_ == 0)
      return v8host_status_internal_error;
    hwnd_ = ::CreateWindowExW(0, class_name_.c_str(), L"", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, ModuleInstance(), nullptr);
    if (hwnd_ == nullptr) {
      ::UnregisterClassW(class_name_.c_str(), ModuleInstance());
      atom_ = 0;
      return v8host_status_internal_error;
    }
    lifecycle_thread_id_.store(::GetCurrentThreadId(),
                               std::memory_order_release);
    return v8host_status_ok;
  }

 public:
  bool ScheduleStart(std::shared_ptr<ClientConnection> connection) {
    auto* pending = new (std::nothrow) std::shared_ptr<ClientConnection>(std::move(connection));
    if (!pending) return false;
    if (tokens_.Post(hwnd_, kStartTransportMessage, pending, [](void* value) {
          std::unique_ptr<std::shared_ptr<ClientConnection>> owned(
              static_cast<std::shared_ptr<ClientConnection>*>(value));
          StartConnection(owned->get());
        })) return true;
    delete pending;
    return false;
  }

  bool IsInitialized() const {
    return initialized_.load(std::memory_order_acquire);
  }

  void ScheduleSessionDelete(v8host_session_s* session) {
    HWND hwnd = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      hwnd = hwnd_;
    }
    if (tokens_.Post(hwnd, kDestroySessionMessage, session, [](void* value) {
          Instance().last_delete_thread_id_.store(::GetCurrentThreadId(), std::memory_order_release);
          delete static_cast<v8host_session_s*>(value);
        })) {
      return;
    }
    // Initialization guarantees a live lifecycle window. If the OS refuses
    // the post, leaking is safer than cross-thread deletion during a callback.
  }

  // Deletes a broker-graph object on the app thread, after any callback or
  // routing call that may still use it has returned.
  void ScheduleDelete(void* object, void (*destroy)(void*)) {
    HWND hwnd = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      hwnd = hwnd_;
    }
    if (tokens_.Post(hwnd, kDestroyObjectMessage, object, destroy)) {
      return;
    }
    // As for sessions: leak rather than delete under a callback.
  }

#ifdef V8HOST_CLIENT_TESTING
  uint64_t PostProbe(UINT message, std::atomic<int>* count) {
    return tokens_.Post(hwnd_, message, count, [](void* value) {
      static_cast<std::atomic<int>*>(value)->fetch_add(1);
    });
  }
  void PostToken(UINT message, uint64_t token) {
    ::PostMessageW(hwnd_, message, static_cast<WPARAM>(static_cast<uint32_t>(token)),
                  static_cast<LPARAM>(static_cast<uint32_t>(token >> 32)));
  }
  uint64_t ignored_tokens() const { return tokens_.ignored(); }
#endif

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd,
                                     UINT message,
                                     WPARAM wparam,
                                     LPARAM lparam) {
    if (message == kStartTransportMessage || message == kDestroySessionMessage ||
        message == kDestroyObjectMessage) {
      Instance().tokens_.Dispatch(message, wparam, lparam);
      return 0;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  State state_ = State::kUninitialized;
  HWND hwnd_ = nullptr;
  ATOM atom_ = 0;
  std::wstring class_name_;
  std::atomic<bool> initialized_{false};
  std::atomic<uint32_t> lifecycle_thread_id_{0};
  std::atomic<uint32_t> last_delete_thread_id_{0};
  WindowTokens tokens_;
};

#ifdef V8HOST_CLIENT_TESTING
std::atomic<ClientTransportFactory> g_test_factory{nullptr};
std::atomic<void (*)()> g_before_submission{nullptr}, g_admitted_submission{nullptr};
std::atomic<void (*)()> g_initialize_after_dispatcher_hook{nullptr};
std::atomic<void (*)(v8host_status)> g_inbound_status_observer{nullptr};
#endif

void ObserveInboundStatusForTesting(v8host_status status) {
#ifdef V8HOST_CLIENT_TESTING
  void (*observer)(v8host_status) =
      g_inbound_status_observer.load(std::memory_order_acquire);
  if (observer != nullptr)
    observer(status);
#else
  (void)status;
#endif
}

ClientTransport* CreateTransport(const TransportParams& params,
                                 ClientTransportDelegate* delegate) {
#ifdef V8HOST_CLIENT_TESTING
  ClientTransportFactory factory =
      g_test_factory.load(std::memory_order_acquire);
  return factory != nullptr ? factory(params, delegate) : nullptr;
#else
  return CreateRealPipeClientTransport(params, delegate);
#endif
}

bool CountPairValid(const void* pointer, size_t count) {
  return (pointer == nullptr) == (count == 0);
}

v8host_status WideToUtf8(const wchar_t* text,
                        bool allow_null,
                        std::string* out) {
  out->clear();
  if (text == nullptr)
    return allow_null ? v8host_status_ok : v8host_status_invalid_arg;
  const size_t chars = std::wcslen(text);
  if (chars == 0)
    return v8host_status_ok;
  if (chars > static_cast<size_t>(INT_MAX))
    return v8host_status_quota_exceeded;
  const int bytes = ::WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(chars), nullptr, 0,
      nullptr, nullptr);
  if (bytes <= 0)
    return v8host_status_invalid_arg;
  if (static_cast<uint32_t>(bytes) > protocol::kMaxStringBytes)
    return v8host_status_quota_exceeded;
  out->resize(bytes);
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                            static_cast<int>(chars), out->data(), bytes, nullptr,
                            nullptr) != bytes) {
    out->clear();
    return v8host_status_invalid_arg;
  }
  return v8host_status_ok;
}

bool IsBareFilename(const std::wstring& value) {
  return !value.empty() && value != L"." &&
         value.find(L"..") == std::wstring::npos &&
         value.find_first_of(L"\\/:") == std::wstring::npos;
}

v8host_status MapStatus(protocol::StatusCode status) {
  switch (status) {
    case protocol::StatusCode::OK:
      return v8host_status_ok;
    case protocol::StatusCode::ERROR_PROTOCOL:
      return v8host_status_protocol_error;
    case protocol::StatusCode::ERROR_VERSION:
      return v8host_status_version_mismatch;
    case protocol::StatusCode::ERROR_QUOTA:
      return v8host_status_quota_exceeded;
    case protocol::StatusCode::ERROR_BAD_STATE:
      return v8host_status_invalid_state;
    case protocol::StatusCode::ERROR_INTERNAL:
      return v8host_status_internal_error;
    case protocol::StatusCode::ERROR_PROFILE_ALREADY_BOUND:
      return v8host_status_profile_already_bound;
    case protocol::StatusCode::ERROR_RUNTIME_FAILED:
      return v8host_status_runtime_failed;
    case protocol::StatusCode::ERROR_UNSUPPORTED_MESSAGE:
    case protocol::StatusCode::ERROR_STALE_REQUEST:
      return v8host_status_protocol_error;
  }
  return v8host_status_internal_error;
}

enum class PendingType {
  kCreate,
  kStart,
  kCancel,
  kClose,
  kCreateWorker,
  kCloseWorker,
  kCreateRuntime,
  kCloseRuntime,
};
struct PendingControl {
  PendingType type;
  uint32_t session_id;  // the target: a session, worker, or runtime
  uint32_t run_id;
};

// The broker side of a connection, defined after ClientConnection.
void BrokerConnected(v8host_broker_s* broker);
bool BrokerFrame(v8host_broker_s* broker, const protocol::FrameHeader& header,
                 const uint8_t* payload, size_t len);
void BrokerReply(v8host_broker_s* broker, const PendingControl& pending, bool ack,
                 protocol::StatusCode code);
void BrokerRelayQuota(v8host_broker_s* broker, uint32_t target, uint32_t run);
void BrokerDisconnected(v8host_broker_s* broker, TransportDisconnect reason,
                        v8host_status status);

}  // namespace

class ClientConnection final
    : public ClientTransportDelegate,
      public std::enable_shared_from_this<ClientConnection> {
 public:
  ~ClientConnection() override {
    if (transport_ != nullptr)
      transport_->Close();
  }

  v8host_status Initialize(int32_t broker_mode) {
    TransportParams params;
    params.broker_mode = broker_mode;
    std::vector<wchar_t> path(256);
    for (;;) {
      const DWORD len = ::GetModuleFileNameW(ModuleInstance(), path.data(),
                                             static_cast<DWORD>(path.size()));
      if (!len) return v8host_status_connect_failed;
      if (len < path.size()) {
        std::wstring module_dir(path.data(), len);
        const size_t slash = module_dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return v8host_status_connect_failed;
        module_dir.resize(slash);
        uint16_t native_machine = 0;
        if (!v8host::QueryNativeMachine(&native_machine) ||
            !v8host::ResolveNativePayloadDirectory(module_dir, v8host::kModuleMachine,
                                                   native_machine, &params.payload_directory))
          return v8host_status_connect_failed;
        break;
      }
      if (path.size() >= 32768) return v8host_status_connect_failed;
      path.resize((std::min)(path.size() * 2, size_t{32768}));
    }
    transport_.reset(CreateTransport(params, this));
    if (!transport_)
      return v8host_status_no_memory;
    return v8host_status_ok;
  }

  void StartTransport() {
    if (transport_->Start(shared_from_this()) != v8host_status_ok)
      OnDisconnect(TransportDisconnect::kConnectFailed);
  }

  uint32_t AllocateSessionId() { return 1; }
  void TerminalizeExhaustion() {
    if (exhausted_.load(std::memory_order_acquire))
      OnDisconnect(TransportDisconnect::kProtocolError);
  }
#ifdef V8HOST_CLIENT_TESTING
  void SetRequestId(uint32_t id) {
    std::lock_guard<std::mutex> lock(submission_mutex_);
    next_request_id_ = id;
  }
#endif

  template <typename Builder>
  v8host_status SubmitControl(PendingControl pending, Builder build) {
#ifdef V8HOST_CLIENT_TESTING
    if (auto hook = g_before_submission.load(std::memory_order_acquire)) hook();
#endif
    // Order: session -> submission -> pending; callbacks never take submission.
    std::lock_guard<std::mutex> order(submission_mutex_);
    if (disconnected_.load(std::memory_order_acquire))
      return v8host_status_connect_failed;
    if (next_request_id_ == UINT32_MAX) {
      exhausted_.store(true, std::memory_order_release);
      return v8host_status_quota_exceeded;
    }
    const uint32_t id = next_request_id_++;
    v8host_status status = ReservePending(id, pending);
    if (status != v8host_status_ok)
      return status;
    protocol::FrameHeader header;
    header.version_major = protocol::kWireVersionMajor;
    header.version_minor = protocol::kWireVersionMinor;
    header.flags = protocol::kFlagMustUnderstand;
    header.conn_id = conn_id();
    header.target_id = pending.session_id;
    header.run_id = pending.run_id;
    header.request_id = id;
#ifdef V8HOST_CLIENT_TESTING
    if (auto hook = g_admitted_submission.load(std::memory_order_acquire)) hook();
#endif
    status = Send(build(header));
    if (status != v8host_status_ok)
      RemovePending(id);
    return status;
  }

  v8host_status SendClose(PendingControl pending) {
    return SubmitControl(pending, [&](const auto& header) {
      return pending.type == PendingType::kCloseWorker
          ? protocol::BuildCloseWorkerFrame(header)
          : protocol::BuildCloseRuntimeFrame(header);
    });
  }

  // A close has no synchronous failure channel. Retain its target, not the
  // released handle; only quota is retryable. Exhaustion/failed allocation or
  // transport failure tears down the connection on its pump, outside graph locks.
  void SubmitClose(PendingControl pending) {
    {
      std::lock_guard<std::mutex> lock(close_mutex_);
      v8host_status status = close_count_ == 0 ? SendClose(pending)
                                             : v8host_status_quota_exceeded;
      if (status == v8host_status_quota_exceeded && !exhausted_.load()) {
        if (close_count_ < deferred_closes_.size()) {
          deferred_closes_[(close_head_ + close_count_) % deferred_closes_.size()] = pending;
          ++close_count_;
        } else {
          close_failed_.store(true, std::memory_order_release);
        }
      } else if (status != v8host_status_ok) {
        close_failed_.store(true, std::memory_order_release);
      }
    }
    transport_->Wake();
  }

  void OnPump() override {
    {
      std::lock_guard<std::mutex> lock(close_mutex_);
      while (close_count_ != 0 && !close_failed_.load()) {
        const v8host_status status = SendClose(deferred_closes_[close_head_]);
        if (status == v8host_status_quota_exceeded && !exhausted_.load())
          break;
        if (status != v8host_status_ok) {
          close_failed_.store(true, std::memory_order_release);
          break;
        }
        close_head_ = (close_head_ + 1) % deferred_closes_.size();
        --close_count_;
      }
    }
    if (close_failed_.load(std::memory_order_acquire))
      OnDisconnect(TransportDisconnect::kBrokerLost);
  }

  void OnConnected(uint32_t, uint16_t major, uint16_t minor) override {
    selected_major_.store(major, std::memory_order_release);
    selected_minor_.store(minor, std::memory_order_release);
    if (v8host_broker_s* broker = AcquireBroker()) {
      BrokerConnected(broker);
      broker->Release();
    }
  }

  // A broker owns this connection instead of a session; null once it closes.
  void SetBroker(v8host_broker_s* broker) {
    std::lock_guard<std::mutex> lock(mutex_);
    broker_ = broker;
  }

  v8host_broker_s* AcquireBroker() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (broker_ != nullptr)
      broker_->AddRef();
    return broker_;
  }

  uint32_t conn_id() const {
    return transport_ != nullptr ? transport_->conn_id() : 0;
  }

  void RegisterSession(v8host_session_s* session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[session->session_id] = session;
  }

  void UnregisterSession(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(id);
  }

  v8host_status ReservePending(uint32_t request_id, PendingControl pending) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.size() >= protocol::kMaxControlQueueRequests)
      return v8host_status_quota_exceeded;
    pending_[request_id] = pending;
    return v8host_status_ok;
  }

  void RemovePending(uint32_t request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(request_id);
  }

  v8host_status Send(const std::vector<uint8_t>& frame) {
    if (!transport_)
      return v8host_status_connect_failed;
    return transport_->SendFrame(frame.data(), frame.size());
  }

  void CloseTransport() {
    if (transport_)
      transport_->Close();
  }

  void OnInboundFrame(const uint8_t* bytes, size_t len) override {
    protocol::FrameHeader header;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    if (protocol::DecodeAndValidateFrame(bytes, len, &header, &payload,
                                         &payload_len) !=
            protocol::DecodeStatus::kOk ||
        header.version_major != selected_major_.load(std::memory_order_acquire) ||
        header.version_minor != selected_minor_.load(std::memory_order_acquire) ||
        header.conn_id != conn_id()) {
      OnDisconnect(TransportDisconnect::kProtocolError);
      ObserveInboundStatusForTesting(v8host_status_protocol_error);
      return;
    }

    if (header.type == protocol::MessageType::ACK ||
        header.type == protocol::MessageType::ERROR) {
      if (header.type == protocol::MessageType::ERROR && header.request_id == 0)
        RouteRelayQuota(header, payload, payload_len);
      else
        RouteControl(header, payload, payload_len);
      return;
    }

    if (v8host_broker_s* broker = AcquireBroker()) {
      const bool ok = BrokerFrame(broker, header, payload, payload_len);
      broker->Release();
      if (!ok)
        OnDisconnect(TransportDisconnect::kProtocolError);
      return;
    }

    v8host_session_s* session = AcquireSession(header.target_id);
    if (session == nullptr) {
      ObserveInboundStatusForTesting(v8host_status_invalid_state);
      return;
    }
    RouteSessionFrame(session, header, payload, payload_len);
    session->Release();
  }

  void OnDisconnect(TransportDisconnect reason) override {
    bool expected = false;
    if (!disconnected_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
      return;
    }
    CloseTransport();
    const v8host_status status =
        reason == TransportDisconnect::kProtocolError
            ? v8host_status_protocol_error
            : (reason == TransportDisconnect::kConnectFailed
                   ? v8host_status_connect_failed
                   : v8host_status_broker_lost);
    if (v8host_broker_s* broker = AcquireBroker()) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
      }
      BrokerDisconnected(broker, reason, status);
      broker->Release();
    }
    std::vector<v8host_session_s*> sessions;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sessions.reserve(sessions_.size());
      for (const auto& entry : sessions_) {
        entry.second->AddRef();
        sessions.push_back(entry.second);
      }
      pending_.clear();
    }
    for (v8host_session_s* session : sessions) {
      std::vector<v8host_run_s*> runs;
      {
        std::lock_guard<std::mutex> lock(session->mutex);
        runs.reserve(session->runs.size());
        for (const auto& entry : session->runs) {
          v8host_run_s* run = entry.second.get();
          if (!run->terminal_local.exchange(true, std::memory_order_acq_rel))
            runs.push_back(run);
        }
      }
      for (v8host_run_s* run : runs) {
        Dispatcher::Instance().PostRunEvent(
            session->dispatch_id, run, &run->arbiter, true,
            reason == TransportDisconnect::kBrokerLost
                ? v8host_run_event_broker_lost
                : v8host_run_event_failed,
            status);
      }
      Dispatcher::Instance().PostSessionState(
          session->dispatch_id, v8host_session_state_closed, status);
      Dispatcher::Instance().PostDisconnect(session->dispatch_id);
      session->Release();
    }
  }

 private:
  v8host_session_s* AcquireSession(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(id);
    if (it == sessions_.end())
      return nullptr;
    it->second->AddRef();
    return it->second;
  }

  v8host_run_s* FindRun(v8host_session_s* session, uint32_t run_id) {
    std::lock_guard<std::mutex> lock(session->mutex);
    const auto it = session->runs.find(run_id);
    return it == session->runs.end() ? nullptr : it->second.get();
  }

  void MarkAndPostTerminal(v8host_session_s* session,
                           v8host_run_s* run,
                           int32_t event,
                           v8host_status status) {
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      if (run->terminal_local.exchange(true, std::memory_order_acq_rel))
        return;
    }
    Dispatcher::Instance().PostRunEvent(session->dispatch_id, run, &run->arbiter,
                                        true, event, status);
  }

  void RouteRelayQuota(const protocol::FrameHeader& header,
                       const uint8_t* payload, size_t payload_len) {
    protocol::ErrorPayload error;
    if (!header.target_id || !header.run_id ||
        !protocol::DecodeErrorPayload(payload, payload_len, &error) ||
        error.status_code != protocol::StatusCode::ERROR_QUOTA) {
      OnDisconnect(TransportDisconnect::kProtocolError);
      return;
    }
    if (v8host_broker_s* broker = AcquireBroker()) {
      // A runtime the host already closed may still get its run's error.
      BrokerRelayQuota(broker, header.target_id, header.run_id);
      broker->Release();
      return;
    }
    v8host_session_s* session = AcquireSession(header.target_id);
    if (!session) {
      OnDisconnect(TransportDisconnect::kProtocolError);
      return;
    }
    v8host_run_s* run = FindRun(session, header.run_id);
    if (run)
      MarkAndPostTerminal(session, run, v8host_run_event_failed, v8host_status_quota_exceeded);
    session->Release();
    if (!run) OnDisconnect(TransportDisconnect::kProtocolError);
  }

  void RouteControl(const protocol::FrameHeader& header,
                    const uint8_t* payload,
                    size_t payload_len) {
    PendingControl pending{PendingType::kCreate, 0, 0};
    bool correlation_valid = false;
    bool consumed_duplicate = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = pending_.find(header.request_id);
      if (it != pending_.end()) {
        pending = it->second;
        correlation_valid =
            header.target_id == pending.session_id &&
            header.run_id == pending.run_id;
        if (correlation_valid) {
          pending_.erase(it);
          if (consumed_requests_.size() == protocol::kDuplicateCacheSize) {
            consumed_request_map_.erase(consumed_requests_.front());
            consumed_requests_.pop_front();
          }
          consumed_requests_.push_back(header.request_id);
          consumed_request_map_[header.request_id] = pending;
        }
      } else {
        const auto consumed = consumed_request_map_.find(header.request_id);
        consumed_duplicate =
            consumed != consumed_request_map_.end() &&
            header.target_id == consumed->second.session_id &&
            header.run_id == consumed->second.run_id;
      }
    }
    if (consumed_duplicate)
      return;
    if (!correlation_valid) {
      OnDisconnect(TransportDisconnect::kProtocolError);
      return;
    }
    if (v8host_broker_s* broker = AcquireBroker()) {
      bool ok = true;
      protocol::StatusCode code = protocol::StatusCode::OK;
      if (header.type == protocol::MessageType::ACK) {
        protocol::AckPayload ack;
        ok = protocol::DecodeAckPayload(payload, payload_len, &ack);
      } else {
        protocol::ErrorPayload error;
        ok = protocol::DecodeErrorPayload(payload, payload_len, &error);
        code = error.status_code;
      }
      if (ok)
        BrokerReply(broker, pending, header.type == protocol::MessageType::ACK, code);
      broker->Release();
      if (!ok)
        OnDisconnect(TransportDisconnect::kProtocolError);
      return;
    }
    v8host_session_s* session = AcquireSession(pending.session_id);
    if (session == nullptr)
      return;
    if (header.type == protocol::MessageType::ACK) {
      protocol::AckPayload ack;
      if (!protocol::DecodeAckPayload(payload, payload_len, &ack)) {
        session->Release();
        OnDisconnect(TransportDisconnect::kProtocolError);
        return;
      }
      if (pending.type == PendingType::kStart) {
        std::lock_guard<std::mutex> lock(session->mutex);
        const auto it = session->runs.find(pending.run_id);
        if (it != session->runs.end() &&
            !it->second->terminal_local.load(std::memory_order_acquire)) {
          v8host_run_s* run = it->second.get();
          Dispatcher::Instance().PostRunEvent(
              session->dispatch_id, run, &run->arbiter, false,
              v8host_run_event_started, v8host_status_ok);
        }
      }
    } else {
      protocol::ErrorPayload error;
      if (!protocol::DecodeErrorPayload(payload, payload_len, &error)) {
        session->Release();
        OnDisconnect(TransportDisconnect::kProtocolError);
        return;
      }
      const v8host_status status = MapStatus(error.status_code);
      if (pending.run_id != 0) {
        v8host_run_s* run = FindRun(session, pending.run_id);
        if (run != nullptr)
          MarkAndPostTerminal(session, run, v8host_run_event_failed, status);
      } else {
        Dispatcher::Instance().PostSessionState(
            session->dispatch_id, v8host_session_state_closed, status);
      }
    }
    session->Release();
  }

  void RouteSessionFrame(v8host_session_s* session,
                         const protocol::FrameHeader& header,
                         const uint8_t* payload,
                         size_t payload_len) {
    switch (header.type) {
      case protocol::MessageType::SESSION_READY:
        ObserveInboundStatusForTesting(
            Dispatcher::Instance().PostSessionState(
                session->dispatch_id, v8host_session_state_ready, v8host_status_ok));
        return;
      case protocol::MessageType::STARTUP_READY: {
        protocol::StartupReadyPayload ready;
        if (!protocol::DecodeStartupReadyPayload(payload, payload_len, &ready))
          break;
        Dispatcher::Instance().PostSessionState(
            session->dispatch_id,
            v8host_session_state_worker_startup_ready, v8host_status_ok);
        return;
      }
      case protocol::MessageType::SECURITY_READY:
        Dispatcher::Instance().PostSessionState(
            session->dispatch_id,
            v8host_session_state_worker_security_ready, v8host_status_ok);
        return;
      case protocol::MessageType::RELAY_FROM_WORKER: {
        int32_t kind = 0;
        const uint8_t* body = nullptr;
        size_t body_len = 0;
        if (!protocol::DecodeRelayPayload(payload, payload_len, &kind, &body,
                                          &body_len))
          break;
        std::lock_guard<std::mutex> lock(session->mutex);
        const auto it = session->runs.find(header.run_id);
        if (it == session->runs.end() ||
            it->second->terminal_local.load(std::memory_order_acquire))
          return;
        return static_cast<void>(Dispatcher::Instance().PostRelay(
            session->dispatch_id, it->second.get(), kind, body, body_len));
      }
      case protocol::MessageType::RESULT: {
        protocol::ResultPayload result;
        if (!protocol::DecodeResultPayload(payload, payload_len, &result))
          break;  // malformed -> protocol-error disconnect (fail-closed)
        v8host_run_s* run = FindRun(session, header.run_id);
        if (run != nullptr) {
          const int32_t event =
              result.disposition == protocol::ResultDisposition::kCancelled
                  ? v8host_run_event_cancelled
                  : v8host_run_event_completed;
          MarkAndPostTerminal(session, run, event, v8host_status_ok);
        }
        return;
      }
      case protocol::MessageType::RUN_ERROR: {
        v8host_run_s* run = FindRun(session, header.run_id);
        if (run == nullptr)
          return;
        protocol::ErrorPayload error;
        const v8host_status status =
            protocol::DecodeErrorPayload(payload, payload_len, &error)
                ? MapStatus(error.status_code)
                : v8host_status_protocol_error;
        MarkAndPostTerminal(session, run, v8host_run_event_failed, status);
        return;
      }
      case protocol::MessageType::WORKER_EXIT: {
        std::vector<v8host_run_s*> runs;
        {
          std::lock_guard<std::mutex> lock(session->mutex);
          for (const auto& entry : session->runs) {
            if (!entry.second->terminal_local.exchange(
                    true, std::memory_order_acq_rel)) {
              runs.push_back(entry.second.get());
            }
          }
        }
        for (v8host_run_s* run : runs) {
          Dispatcher::Instance().PostRunEvent(
              session->dispatch_id, run, &run->arbiter, true,
              v8host_run_event_worker_exited, v8host_status_run_terminal);
        }
        Dispatcher::Instance().PostSessionState(
            session->dispatch_id, v8host_session_state_closed,
            v8host_status_run_terminal);
        return;
      }
      default:
        break;
    }
    OnDisconnect(TransportDisconnect::kProtocolError);
  }

  std::mutex mutex_;
  std::unordered_map<uint32_t, v8host_session_s*> sessions_;
  v8host_broker_s* broker_ = nullptr;
  std::unordered_map<uint32_t, PendingControl> pending_;
  std::deque<uint32_t> consumed_requests_;
  std::unordered_map<uint32_t, PendingControl> consumed_request_map_;
  std::unique_ptr<ClientTransport> transport_;
  std::mutex submission_mutex_;
  uint32_t next_request_id_ = 2;
  std::atomic<uint16_t> selected_major_{protocol::kWireVersionMajor};
  std::atomic<uint16_t> selected_minor_{protocol::kWireVersionMinor};
  std::atomic<bool> disconnected_{false};
  std::atomic<bool> exhausted_{false};
  std::mutex close_mutex_;
  std::array<PendingControl, protocol::kMaxControlQueueRequests> deferred_closes_{};
  size_t close_head_ = 0, close_count_ = 0;
  std::atomic<bool> close_failed_{false};
};

namespace {
void StartConnection(ClientConnection* connection) { connection->StartTransport(); }

//==========================================================================
// The broker graph (design §2.2, §6.4, §6.5). Every function here runs with
// the broker's mutex held. Events go to the broker's one FIFO; each holds a
// reference on the object it names.
//==========================================================================
using EventKind = SessionDispatch::EventKind;

void ReleaseWorker(void* worker) { static_cast<v8host_worker_s*>(worker)->Release(); }
void ReleaseRuntime(void* runtime) { static_cast<v8host_runtime_s*>(runtime)->Release(); }

void BrokerBackpressure(v8host_broker_s* b);
void RuntimeBackpressure(v8host_broker_s* b, v8host_runtime_s* r);

void PostEvent(v8host_broker_s* b, SessionDispatch::BrokerPost&& post, v8host_runtime_s* r) {
  PostOverflow overflow = PostOverflow::kNone;
  if (b->dispatch == nullptr) {
    if (post.release != nullptr)
      post.release(post.holder);
    return;
  }
  b->dispatch->PostBroker(std::move(post), &overflow);
  if (overflow == PostOverflow::kRuntime && r != nullptr)
    RuntimeBackpressure(b, r);
  else if (overflow == PostOverflow::kBroker)
    BrokerBackpressure(b);
}

void PostBrokerState(v8host_broker_s* b, int32_t state, v8host_status status) {
  SessionDispatch::BrokerPost post;
  post.kind = EventKind::kBrokerState;
  post.code = state;
  post.status = status;
  PostEvent(b, std::move(post), nullptr);
}

void PostWorkerState(v8host_broker_s* b, v8host_worker_s* w, int32_t state,
                     v8host_status status) {
  w->AddRef();
  SessionDispatch::BrokerPost post;
  post.kind = EventKind::kWorkerState;
  post.object = w;
  post.code = state;
  post.status = status;
  post.worker_id = w->id;
  post.holder = w;
  post.release = &ReleaseWorker;
  PostEvent(b, std::move(post), nullptr);
}

void PostRuntimeEvent(v8host_broker_s* b, v8host_runtime_s* r, EventKind kind,
                      v8host_run_s* run, int32_t code, v8host_status status,
                      const void* data = nullptr, size_t size = 0) {
  if (r->silenced)
    return;
  r->AddRef();
  SessionDispatch::BrokerPost post;
  post.kind = kind;
  post.object = r;
  post.run = run;
  post.code = code;
  post.status = status;
  post.data = data;
  post.size = size;
  post.worker_id = r->worker->id;
  post.runtime_id = r->id;
  post.terminal = kind == EventKind::kRunEvent && code != v8host_run_event_started;
  post.arbiter = run != nullptr ? &run->arbiter : nullptr;
  post.holder = r;
  post.release = &ReleaseRuntime;
  PostEvent(b, std::move(post), r);
}

void UnregisterRuntime(v8host_broker_s* b, v8host_runtime_s* r) {
  if (b->runtimes.erase(r->id))
    r->Release();
  if (r->worker->runtimes.erase(r->id))
    r->Release();
}

void UnregisterWorker(v8host_broker_s* b, v8host_worker_s* w) {
  if (b->workers.erase(w->id))
    w->Release();
}

template <typename Map>
std::vector<typename Map::mapped_type> Referenced(const Map& map) {
  std::vector<typename Map::mapped_type> objects;
  for (const auto& entry : map) {
    entry.second->AddRef();
    objects.push_back(entry.second);
  }
  std::sort(objects.begin(), objects.end(),
            [](const auto* a, const auto* b) { return a->id < b->id; });
  return objects;
}

void MarkRunsTerminal(v8host_runtime_s* r) {
  for (const auto& entry : r->runs)
    entry.second->terminal_local.store(true, std::memory_order_release);
}

// Ends every live run of `r` with one terminal event, in run order.
void EndRuns(v8host_broker_s* b, v8host_runtime_s* r, int32_t event,
             v8host_status status) {
  std::vector<v8host_run_s*> runs;
  for (const auto& entry : r->runs)
    if (!entry.second->terminal_local.exchange(true, std::memory_order_acq_rel))
      runs.push_back(entry.second.get());
  std::sort(runs.begin(), runs.end(),
            [](const v8host_run_s* a, const v8host_run_s* b) { return a->run_id < b->run_id; });
  for (v8host_run_s* run : runs)
    PostRuntimeEvent(b, r, EventKind::kRunEvent, run, event, status);
}

void MarkTerminal(v8host_broker_s* b, v8host_runtime_s* r, v8host_run_s* run,
                  int32_t event, v8host_status status) {
  if (!run->terminal_local.exchange(true, std::memory_order_acq_rel))
    PostRuntimeEvent(b, r, EventKind::kRunEvent, run, event, status);
}

// close_when_empty fires when a worker's admissions drop from 1 to 0 by a host
// close, a runtime failure, a refused open, or backpressure — never during a
// worker or broker fan-out, which closes the worker first (design §6.2).
void ReleaseAdmission(v8host_broker_s* b, v8host_worker_s* w) {
  if (w->admissions == 0 || --w->admissions != 0 || w->closed || w->closing ||
      b->terminal || !(w->flags & v8host_worker_flags_close_when_empty))
    return;
  w->closing = true;
  b->conn->SubmitClose({PendingType::kCloseWorker, w->id, 0});
}

// A runtime ended unasked or refused: its runs fail, then it reports closed.
void EndRuntime(v8host_broker_s* b, v8host_runtime_s* r, v8host_status status) {
  if (r->closed)
    return;
  EndRuns(b, r, v8host_run_event_failed, status);
  if (r->closed)
    return;  // its queue overflowed during the fan-out
  r->closed = true;
  PostRuntimeEvent(b, r, EventKind::kRuntimeState, nullptr, v8host_runtime_state_closed, status);
  v8host_worker_s* w = r->worker;
  UnregisterRuntime(b, r);
  ReleaseAdmission(b, w);
}

// A worker ended: every live run, then every runtime, then the worker report
// their terminal, each once (design §6.5).
void EndWorker(v8host_broker_s* b, v8host_worker_s* w, int32_t run_event,
               v8host_status run_status, v8host_status status) {
  if (w->closed)
    return;
  w->closed = true;
  auto runtimes = Referenced(w->runtimes);
  for (v8host_runtime_s* r : runtimes)
    if (!r->closed)
      EndRuns(b, r, run_event, run_status);
  for (v8host_runtime_s* r : runtimes) {
    if (!r->closed) {
      r->closed = true;
      PostRuntimeEvent(b, r, EventKind::kRuntimeState, nullptr, v8host_runtime_state_closed, status);
    }
    UnregisterRuntime(b, r);
  }
  PostWorkerState(b, w, v8host_worker_state_closed, status);
  UnregisterWorker(b, w);
  for (v8host_runtime_s* r : runtimes)
    r->Release();
}

// Marks every child closed and unregisters it, without events. `orphan` is a
// host close of the broker: the children's getters then return 0.
void CloseGraph(v8host_broker_s* b, bool orphan) {
  auto runtimes = Referenced(b->runtimes);
  for (v8host_runtime_s* r : runtimes) {
    r->closed = r->silenced = true;
    r->orphaned = r->orphaned || orphan;
    MarkRunsTerminal(r);
    UnregisterRuntime(b, r);
    r->Release();
  }
  auto workers = Referenced(b->workers);
  for (v8host_worker_s* w : workers) {
    w->closed = true;
    w->orphaned = w->orphaned || orphan;
    UnregisterWorker(b, w);
    w->Release();
  }
}

// A runtime's callback queue overflowed: the dispatcher already queued its
// closed(callback_backpressure), which supersedes its runs' terminals.
void RuntimeBackpressure(v8host_broker_s* b, v8host_runtime_s* r) {
  r->silenced = true;
  if (r->closed)
    return;
  r->closed = true;
  MarkRunsTerminal(r);
  v8host_worker_s* w = r->worker;
  if (!w->closed && !b->terminal)
    b->conn->SubmitClose({PendingType::kCloseRuntime, r->id, 0});
  UnregisterRuntime(b, r);
  ReleaseAdmission(b, w);
}

// The broker's queue overflowed: the dispatcher queued its one sticky
// disconnected(callback_backpressure), which supersedes every child terminal.
void BrokerBackpressure(v8host_broker_s* b) {
  if (b->terminal)
    return;
  b->terminal = true;
  CloseGraph(b, false);
  b->conn->CloseTransport();
}

void BrokerConnected(v8host_broker_s* b) {
  std::lock_guard<std::mutex> lock(b->mutex);
  if (!b->terminal && !b->released)
    PostBrokerState(b, v8host_broker_state_connected, v8host_status_ok);
}

bool BrokerFrame(v8host_broker_s* b, const protocol::FrameHeader& header,
                 const uint8_t* payload, size_t len) {
  std::lock_guard<std::mutex> lock(b->mutex);
  if (b->terminal || b->released)
    return true;
  auto worker = [&]() -> v8host_worker_s* {
    const auto it = b->workers.find(header.target_id);
    return it == b->workers.end() ? nullptr : it->second;
  };
  auto runtime = [&]() -> v8host_runtime_s* {
    const auto it = b->runtimes.find(header.target_id);
    return it == b->runtimes.end() ? nullptr : it->second;
  };
  auto run_of = [&](v8host_runtime_s* r) -> v8host_run_s* {
    const auto it = r->runs.find(header.run_id);
    return it == r->runs.end() ? nullptr : it->second.get();
  };
  // Frames for objects the host or a fan-out already closed are late: drop.
  switch (header.type) {
    case protocol::MessageType::STARTUP_READY: {
      protocol::StartupReadyPayload ready;
      if (!protocol::DecodeStartupReadyPayload(payload, len, &ready))
        return false;
      if (v8host_worker_s* w = worker()) {
        w->process_id.store(ready.process_id, std::memory_order_release);
        PostWorkerState(b, w, v8host_worker_state_startup_ready, v8host_status_ok);
      }
      return true;
    }
    case protocol::MessageType::SECURITY_READY:
      if (v8host_worker_s* w = worker())
        PostWorkerState(b, w, v8host_worker_state_security_ready, v8host_status_ok);
      return true;
    case protocol::MessageType::WORKER_EXIT:
      if (v8host_worker_s* w = worker()) {
        w->AddRef();
        EndWorker(b, w, v8host_run_event_worker_exited, v8host_status_run_terminal,
                  v8host_status_worker_exited);
        w->Release();
      }
      return true;
    case protocol::MessageType::RUNTIME_READY:
      if (v8host_runtime_s* r = runtime())
        PostRuntimeEvent(b, r, EventKind::kRuntimeState, nullptr, v8host_runtime_state_ready,
                         v8host_status_ok);
      return true;
    case protocol::MessageType::RUNTIME_CLOSED: {
      protocol::RuntimeClosedPayload closed;
      if (!protocol::DecodeRuntimeClosedPayload(payload, len, &closed))
        return false;
      if (v8host_runtime_s* r = runtime()) {
        r->AddRef();
        EndRuntime(b, r, v8host_status_runtime_failed);
        r->Release();
      }
      return true;
    }
    case protocol::MessageType::RELAY_FROM_WORKER: {
      int32_t kind = 0;
      const uint8_t* body = nullptr;
      size_t body_len = 0;
      if (!protocol::DecodeRelayPayload(payload, len, &kind, &body, &body_len))
        return false;
      if (v8host_runtime_s* r = runtime())
        if (v8host_run_s* run = run_of(r); run && !run->terminal_local.load(std::memory_order_acquire))
          PostRuntimeEvent(b, r, EventKind::kRelay, run, kind, v8host_status_ok, body, body_len);
      return true;
    }
    case protocol::MessageType::RESULT: {
      protocol::ResultPayload result;
      if (!protocol::DecodeResultPayload(payload, len, &result))
        return false;
      if (v8host_runtime_s* r = runtime())
        if (v8host_run_s* run = run_of(r))
          MarkTerminal(b, r, run,
                       result.disposition == protocol::ResultDisposition::kCancelled
                           ? v8host_run_event_cancelled
                           : v8host_run_event_completed,
                       v8host_status_ok);
      return true;
    }
    case protocol::MessageType::RUN_ERROR: {
      protocol::ErrorPayload error;
      const v8host_status status = protocol::DecodeErrorPayload(payload, len, &error)
                                       ? MapStatus(error.status_code)
                                       : v8host_status_protocol_error;
      if (v8host_runtime_s* r = runtime())
        if (v8host_run_s* run = run_of(r))
          MarkTerminal(b, r, run, v8host_run_event_failed, status);
      return true;
    }
    default:
      return false;
  }
}

void BrokerReply(v8host_broker_s* b, const PendingControl& pending, bool ack,
                 protocol::StatusCode code) {
  std::lock_guard<std::mutex> lock(b->mutex);
  if (b->terminal || b->released)
    return;
  // A refused create closes the object with the mapped status; an unknown or
  // foreign target (ERROR_BAD_STATE) is a protocol error (design §3.4, §6.5).
  const v8host_status refused = code == protocol::StatusCode::ERROR_BAD_STATE
                                    ? v8host_status_protocol_error
                                    : MapStatus(code);
  const auto w_it = b->workers.find(pending.session_id);
  v8host_worker_s* w = w_it == b->workers.end() ? nullptr : w_it->second;
  const auto r_it = b->runtimes.find(pending.session_id);
  v8host_runtime_s* r = r_it == b->runtimes.end() ? nullptr : r_it->second;
  switch (pending.type) {
    case PendingType::kCreateWorker:
      if (!ack && w) {
        w->AddRef();
        EndWorker(b, w, v8host_run_event_failed, refused, refused);
        w->Release();
      }
      return;
    case PendingType::kCloseWorker:
      if (ack && w && w->closing) {
        w->AddRef();
        EndWorker(b, w, v8host_run_event_failed, v8host_status_ok, v8host_status_ok);
        w->Release();
      }
      return;
    case PendingType::kCreateRuntime:
      if (!ack && r) {
        r->AddRef();
        EndRuntime(b, r, refused);
        r->Release();
      }
      return;
    case PendingType::kStart:
    case PendingType::kCancel: {
      if (!r)
        return;
      const auto it = r->runs.find(pending.run_id);
      if (it == r->runs.end())
        return;
      v8host_run_s* run = it->second.get();
      if (!ack)
        MarkTerminal(b, r, run, v8host_run_event_failed, MapStatus(code));
      else if (pending.type == PendingType::kStart &&
               !run->terminal_local.load(std::memory_order_acquire))
        PostRuntimeEvent(b, r, EventKind::kRunEvent, run, v8host_run_event_started,
                         v8host_status_ok);
      return;
    }
    default:
      return;
  }
}

void BrokerRelayQuota(v8host_broker_s* b, uint32_t target, uint32_t run_id) {
  std::lock_guard<std::mutex> lock(b->mutex);
  if (b->terminal || b->released)
    return;
  const auto it = b->runtimes.find(target);
  if (it == b->runtimes.end())
    return;
  v8host_runtime_s* r = it->second;
  const auto run = r->runs.find(run_id);
  if (run != r->runs.end())
    MarkTerminal(b, r, run->second.get(), v8host_run_event_failed, v8host_status_quota_exceeded);
}

// The connection ended: every live run, then every runtime, then every worker,
// and finally the broker report the status once. Nothing is replayed.
void BrokerDisconnected(v8host_broker_s* b, TransportDisconnect,
                        v8host_status status) {
  std::lock_guard<std::mutex> lock(b->mutex);
  if (b->terminal || b->released)
    return;
  const int32_t run_event = v8host_run_event_failed;
  auto workers = Referenced(b->workers);
  auto runtimes = Referenced(b->runtimes);
  for (v8host_worker_s* w : workers)
    w->closed = true;  // no close_when_empty during the fan-out
  for (v8host_runtime_s* r : runtimes)
    if (!r->closed)
      EndRuns(b, r, run_event, status);
  for (v8host_runtime_s* r : runtimes) {
    if (!r->closed) {
      r->closed = true;
      PostRuntimeEvent(b, r, EventKind::kRuntimeState, nullptr, v8host_runtime_state_closed, status);
    }
    UnregisterRuntime(b, r);
  }
  for (v8host_worker_s* w : workers) {
    if (!b->terminal)
      PostWorkerState(b, w, v8host_worker_state_closed, status);
    UnregisterWorker(b, w);
  }
  if (!b->terminal)
    PostBrokerState(b, v8host_broker_state_disconnected, status);
  b->terminal = true;
  for (v8host_runtime_s* r : runtimes)
    r->Release();
  for (v8host_worker_s* w : workers)
    w->Release();
}
}
#ifdef V8HOST_CLIENT_TESTING
void SetSubmissionHooksForTesting(void (*before)(), void (*admitted)()) {
  g_before_submission.store(before, std::memory_order_release);
  g_admitted_submission.store(admitted, std::memory_order_release);
}
v8host_status SubmitCancelForTesting(v8host_session_s* session, uint32_t run) {
  return session->conn->SubmitControl({PendingType::kCancel, session->session_id, run},
      [](const auto& header) { return protocol::BuildCancelRunFrame(header); });
}
void SetIdsForTesting(v8host_session_s* session, uint32_t request, uint32_t run) {
  std::lock_guard<std::mutex> lock(session->mutex);
  session->conn->SetRequestId(request);
  session->next_run_id = run;
}
void SetBrokerRequestIdForTesting(v8host_broker broker, uint32_t request) {
  std::lock_guard<std::mutex> lock(broker->mutex);
  broker->conn->SetRequestId(request);
}
uint64_t PostLifecycleProbeForTesting(uint32_t message, std::atomic<int>* count) {
  return ClientRuntime::Instance().PostProbe(message, count);
}
void PostLifecycleTokenForTesting(uint32_t message, uint64_t token) {
  ClientRuntime::Instance().PostToken(message, token);
}
uint64_t IgnoredLifecycleTokensForTesting() {
  return ClientRuntime::Instance().ignored_tokens();
}
void SetTransportFactoryForTesting(ClientTransportFactory factory) {
  g_test_factory.store(factory, std::memory_order_release);
}

void SetInitializeAfterDispatcherHookForTesting(void (*hook)()) {
  g_initialize_after_dispatcher_hook.store(hook, std::memory_order_release);
}

uint32_t LifecycleThreadIdForTesting() {
  return ClientRuntime::Instance().lifecycle_thread_id();
}

uint32_t LastSessionDeleteThreadIdForTesting() {
  return ClientRuntime::Instance().last_delete_thread_id();
}

void SetInboundStatusObserverForTesting(void (*observer)(v8host_status)) {
  g_inbound_status_observer.store(observer, std::memory_order_release);
}
#endif

namespace {

protocol::FrameHeader MakeHeader(ClientConnection* conn,
                                 uint32_t session_id,
                                 uint32_t run_id,
                                 uint32_t request_id,
                                 bool control) {
  protocol::FrameHeader header;
  header.version_major = protocol::kWireVersionMajor;
  header.version_minor = protocol::kWireVersionMinor;
  header.flags = control ? protocol::kFlagMustUnderstand : 0;
  header.conn_id = conn->conn_id();
  header.target_id = session_id;
  header.run_id = run_id;
  header.request_id = request_id;
  return header;
}

v8host_status ValidateSessionConfig(const SessionConfigCopy& config) {
  const bool app_container = config.use_app_container != 0;
  const bool has_profile = !config.app_container_profile_name.empty();
  if ((app_container && !has_profile) ||
      (!app_container && (has_profile || !config.capabilities.empty())) ||
      (config.low_privilege_app_container != 0 && !app_container)) {
    return v8host_status_invalid_arg;
  }
  return v8host_status_ok;
}

static_assert(sizeof(wchar_t) == sizeof(v8host_char16),
              "v8host_char16 must match the Windows wchar_t representation");

// Copies consumer control-plane text into `out`. Rejects NULL, embedded NULs,
// unpaired surrogates, and text whose UTF-8 form exceeds kMaxStringBytes.
v8host_status CopyUtf16(const v8host_char16* value,
                        size_t length,
                        std::wstring* out) {
  if (value == nullptr)
    return v8host_status_invalid_arg;
  if (length == V8HOST_AUTO_LENGTH) {
    length = std::char_traits<char16_t>::length(value);
  } else if (std::char_traits<char16_t>::find(value, length, u'\0') !=
             nullptr) {
    return v8host_status_invalid_arg;
  }
  std::wstring text(reinterpret_cast<const wchar_t*>(value), length);
  std::string utf8;
  const v8host_status status = WideToUtf8(text.c_str(), false, &utf8);
  if (status != v8host_status_ok)
    return status;
  *out = std::move(text);
  return v8host_status_ok;
}

// As CopyUtf16, for UTF-8 input; invalid UTF-8 is v8host_status_invalid_arg.
v8host_status CopyUtf8(const char* value, size_t length, std::wstring* out) {
  if (value == nullptr)
    return v8host_status_invalid_arg;
  if (length == V8HOST_AUTO_LENGTH) {
    length = std::strlen(value);
  } else if (std::memchr(value, '\0', length) != nullptr) {
    return v8host_status_invalid_arg;
  }
  if (length > protocol::kMaxStringBytes)
    return v8host_status_quota_exceeded;
  std::wstring text;
  if (length != 0) {
    const int chars =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value,
                              static_cast<int>(length), nullptr, 0);
    if (chars <= 0)
      return v8host_status_invalid_arg;
    text.resize(chars);
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value,
                              static_cast<int>(length), text.data(),
                              chars) != chars) {
      return v8host_status_invalid_arg;
    }
  }
  *out = std::move(text);
  return v8host_status_ok;
}

// Runs one setter against a builder: a NULL builder is v8host_status_invalid_arg
// (nothing to poison), a poisoned builder returns its stored status unchanged,
// and the first failure poisons the builder.
template <typename Builder, typename Setter>
v8host_status ApplySetter(Builder* builder, Setter&& setter) {
  if (builder == nullptr)
    return v8host_status_invalid_arg;
  if (builder->status != v8host_status_ok)
    return builder->status;
  const v8host_status status = setter(*builder);
  if (status != v8host_status_ok)
    builder->status = status;
  return status;
}

bool IsValidTier(v8host_tier value) {
  return value == v8host_tier_untrusted || value == v8host_tier_trusted;
}

bool IsValidIntegrityLevel(v8host_integrity_level value) {
  return value == v8host_integrity_level_low ||
         value == v8host_integrity_level_untrusted;
}

bool IsValidTokenLevel(v8host_token_level value) {
  const int32_t level = static_cast<int32_t>(value);
  return level >= v8host_token_level_lockdown &&
         level <= v8host_token_level_restricted_same_access;
}

v8host_status AddFileRule(v8host_config_s& builder,
                          std::wstring pattern,
                          bool readonly) {
  if (builder.config.file_rules.size() >= protocol::kMaxFileRules)
    return v8host_status_quota_exceeded;
  builder.config.file_rules.push_back({std::move(pattern), readonly ? 1 : 0});
  return v8host_status_ok;
}

v8host_status AddCapability(v8host_config_s& builder, std::wstring sid) {
  if (sid.empty())
    return v8host_status_invalid_arg;
  if (builder.config.capabilities.size() >= protocol::kMaxCapabilities)
    return v8host_status_quota_exceeded;
  builder.config.capabilities.push_back(std::move(sid));
  return v8host_status_ok;
}

v8host_status SetEngineDll(v8host_run_config_s& builder, std::wstring name) {
  if (!IsBareFilename(name))
    return v8host_status_invalid_arg;
  builder.has_engine = true;
  builder.engine = std::move(name);
  return v8host_status_ok;
}

v8host_status SetSnapshotPath(v8host_run_config_s& builder, std::wstring path) {
  if (path.empty())
    return v8host_status_invalid_arg;
  builder.has_snapshot = true;
  builder.snapshot = std::move(path);
  return v8host_status_ok;
}

protocol::CreateSessionPayload MakeCreatePayload(
    const SessionConfigCopy& config) {
  protocol::CreateSessionPayload payload;
  payload.broker_mode = config.broker_mode;
  payload.tier = config.tier;
  payload.integrity = config.integrity;
  payload.delayed_integrity = config.delayed_integrity;
  payload.initial_token = config.initial_token;
  payload.lockdown_token = config.lockdown_token;
  payload.prohibit_dynamic_code = config.prohibit_dynamic_code != 0;
  payload.use_app_container = config.use_app_container != 0;
  payload.low_privilege_app_container =
      config.low_privilege_app_container != 0;
  WideToUtf8(config.app_container_profile_name.c_str(), true,
             &payload.app_container_profile);
  for (const FileRuleCopy& rule : config.file_rules) {
    protocol::FileRule wire_rule;
    wire_rule.readonly = rule.readonly != 0;
    WideToUtf8(rule.pattern.c_str(), false, &wire_rule.pattern);
    payload.file_rules.push_back(std::move(wire_rule));
  }
  for (const std::wstring& capability : config.capabilities) {
    std::string wire;
    WideToUtf8(capability.c_str(), false, &wire);
    payload.capabilities.push_back(std::move(wire));
  }
  return payload;
}

}  // namespace
}  // namespace v8host::client

void v8host_session_s::Release() {
  if (refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
    v8host::client::ClientRuntime::Instance().ScheduleSessionDelete(this);
}

void v8host_broker_s::Release() {
  if (refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
    v8host::client::ClientRuntime::Instance().ScheduleDelete(
        this, [](void* object) { delete static_cast<v8host_broker_s*>(object); });
}

void v8host_worker_s::Release() {
  if (refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
    v8host::client::ClientRuntime::Instance().ScheduleDelete(
        this, [](void* object) { delete static_cast<v8host_worker_s*>(object); });
}

void v8host_runtime_s::Release() {
  if (refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
    v8host::client::ClientRuntime::Instance().ScheduleDelete(
        this, [](void* object) { delete static_cast<v8host_runtime_s*>(object); });
}

namespace {

constexpr size_t kWorkerProfileMaxBytes = 4096;

v8host_status SetWorkerEngine(v8host_worker_config_s& builder, std::wstring name) {
  if (!v8host::client::IsBareFilename(name))
    return v8host_status_invalid_arg;
  builder.engine = std::move(name);
  return v8host_status_ok;
}

v8host_status AddWorkerSnapshot(v8host_worker_config_s& builder, std::wstring path) {
  if (path.empty() || path.front() == L'\\' || path.front() == L'/' ||
      path.find(L':') != std::wstring::npos)
    return v8host_status_invalid_arg;
  if (builder.snapshots.size() >= v8host::protocol::kMaxWorkerSnapshots)
    return v8host_status_quota_exceeded;
  builder.snapshots.push_back(std::move(path));
  return v8host_status_ok;
}

}  // namespace

extern "C" {

v8host_status V8HOST_CDECL v8host_initialize(uint32_t abi_version) {
  if (abi_version == 0 || abi_version > V8HOST_ABI_VERSION)
    return v8host_status_version_mismatch;
  return v8host::client::ClientRuntime::Instance().Initialize();
}

v8host_status V8HOST_CDECL v8host_config_set_broker_mode(
    v8host_config config,
    v8host_broker_mode value) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    if (value != v8host_broker_mode_dedicated &&
        value != v8host_broker_mode_shared) {
      return v8host_status_invalid_arg;
    }
    builder.config.broker_mode = value;
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_config_set_tier(v8host_config config,
                                                  v8host_tier value) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    if (!v8host::client::IsValidTier(value))
      return v8host_status_invalid_arg;
    builder.config.tier = value;
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_config_set_integrity_level(
    v8host_config config,
    v8host_integrity_level value) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    if (!v8host::client::IsValidIntegrityLevel(value))
      return v8host_status_invalid_arg;
    builder.config.integrity = value;
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_config_set_delayed_integrity_level(
    v8host_config config,
    v8host_integrity_level value) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    if (!v8host::client::IsValidIntegrityLevel(value))
      return v8host_status_invalid_arg;
    builder.config.delayed_integrity = value;
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_config_set_initial_token_level(
    v8host_config config,
    v8host_token_level value) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    if (!v8host::client::IsValidTokenLevel(value))
      return v8host_status_invalid_arg;
    builder.config.initial_token = value;
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_config_set_lockdown_token_level(
    v8host_config config,
    v8host_token_level value) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    if (!v8host::client::IsValidTokenLevel(value))
      return v8host_status_invalid_arg;
    builder.config.lockdown_token = value;
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_config_set_prohibit_dynamic_code(
    v8host_config config,
    bool value) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    builder.config.prohibit_dynamic_code = value ? 1 : 0;
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_config_add_file_rule_utf8(
    v8host_config config,
    const char* pattern,
    size_t length,
    bool readonly) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    std::wstring text;
    const v8host_status status =
        v8host::client::CopyUtf8(pattern, length, &text);
    if (status != v8host_status_ok)
      return status;
    return v8host::client::AddFileRule(builder, std::move(text), readonly);
  });
}

v8host_status V8HOST_CDECL v8host_config_add_file_rule_utf16(
    v8host_config config,
    const v8host_char16* pattern,
    size_t length,
    bool readonly) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    std::wstring text;
    const v8host_status status =
        v8host::client::CopyUtf16(pattern, length, &text);
    if (status != v8host_status_ok)
      return status;
    return v8host::client::AddFileRule(builder, std::move(text), readonly);
  });
}

v8host_status V8HOST_CDECL v8host_config_set_use_app_container(
    v8host_config config,
    bool value) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    builder.config.use_app_container = value ? 1 : 0;
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_config_set_low_privilege_app_container(
    v8host_config config,
    bool value) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    builder.config.low_privilege_app_container = value ? 1 : 0;
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_config_set_profile_name_utf8(
    v8host_config config,
    const char* value,
    size_t length) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    return v8host::client::CopyUtf8(
        value, length, &builder.config.app_container_profile_name);
  });
}

v8host_status V8HOST_CDECL v8host_config_set_profile_name_utf16(
    v8host_config config,
    const v8host_char16* value,
    size_t length) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    return v8host::client::CopyUtf16(
        value, length, &builder.config.app_container_profile_name);
  });
}

v8host_status V8HOST_CDECL v8host_config_add_capability_utf8(
    v8host_config config,
    const char* value,
    size_t length) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    std::wstring text;
    const v8host_status status = v8host::client::CopyUtf8(value, length, &text);
    if (status != v8host_status_ok)
      return status;
    return v8host::client::AddCapability(builder, std::move(text));
  });
}

v8host_status V8HOST_CDECL v8host_config_add_capability_utf16(
    v8host_config config,
    const v8host_char16* value,
    size_t length) {
  return v8host::client::ApplySetter(config, [&](v8host_config_s& builder) {
    std::wstring text;
    const v8host_status status =
        v8host::client::CopyUtf16(value, length, &text);
    if (status != v8host_status_ok)
      return status;
    return v8host::client::AddCapability(builder, std::move(text));
  });
}

v8host_status V8HOST_CDECL v8host_run_config_set_tier(v8host_run_config config,
                                                      v8host_tier value) {
  return v8host::client::ApplySetter(config, [&](v8host_run_config_s& builder) {
    if (!v8host::client::IsValidTier(value))
      return v8host_status_invalid_arg;
    builder.has_tier = true;
    builder.tier = value;
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_run_config_set_engine_dll_utf8(
    v8host_run_config config,
    const char* value,
    size_t length) {
  return v8host::client::ApplySetter(config, [&](v8host_run_config_s& builder) {
    std::wstring text;
    const v8host_status status = v8host::client::CopyUtf8(value, length, &text);
    if (status != v8host_status_ok)
      return status;
    return v8host::client::SetEngineDll(builder, std::move(text));
  });
}

v8host_status V8HOST_CDECL v8host_run_config_set_engine_dll_utf16(
    v8host_run_config config,
    const v8host_char16* value,
    size_t length) {
  return v8host::client::ApplySetter(config, [&](v8host_run_config_s& builder) {
    std::wstring text;
    const v8host_status status =
        v8host::client::CopyUtf16(value, length, &text);
    if (status != v8host_status_ok)
      return status;
    return v8host::client::SetEngineDll(builder, std::move(text));
  });
}

v8host_status V8HOST_CDECL v8host_run_config_set_startup_snapshot_path_utf8(
    v8host_run_config config,
    const char* value,
    size_t length) {
  return v8host::client::ApplySetter(config, [&](v8host_run_config_s& builder) {
    std::wstring text;
    const v8host_status status = v8host::client::CopyUtf8(value, length, &text);
    if (status != v8host_status_ok)
      return status;
    return v8host::client::SetSnapshotPath(builder, std::move(text));
  });
}

v8host_status V8HOST_CDECL v8host_run_config_set_startup_snapshot_path_utf16(
    v8host_run_config config,
    const v8host_char16* value,
    size_t length) {
  return v8host::client::ApplySetter(config, [&](v8host_run_config_s& builder) {
    std::wstring text;
    const v8host_status status =
        v8host::client::CopyUtf16(value, length, &text);
    if (status != v8host_status_ok)
      return status;
    return v8host::client::SetSnapshotPath(builder, std::move(text));
  });
}

v8host_status V8HOST_CDECL v8host_run_config_set_payload(
    v8host_run_config config,
    const void* data,
    size_t size) {
  return v8host::client::ApplySetter(config, [&](v8host_run_config_s& builder) {
    if (!v8host::client::CountPairValid(data, size))
      return v8host_status_invalid_arg;
    if (size > v8host::protocol::kMaxFramePayload)
      return v8host_status_quota_exceeded;
    const auto* bytes = static_cast<const uint8_t*>(data);
    builder.payload.assign(bytes, bytes + size);
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_session_open(
    v8host_configure_session_cb configure,
    void* user_data,
    v8host_session* out_session) {
  if (out_session == nullptr)
    return v8host_status_invalid_arg;
  *out_session = nullptr;
  if (!v8host::client::ClientRuntime::Instance().IsInitialized())
    return v8host_status_not_initialized;

  std::unique_ptr<v8host_config_s> builder(new (std::nothrow) v8host_config_s());
  if (!builder)
    return v8host_status_no_memory;
  v8host_status status = v8host_status_ok;
  if (configure != nullptr) {
    status = configure(user_data, builder.get());
    if (status != v8host_status_ok)
      return status;
  }
  if (builder->status != v8host_status_ok)
    return builder->status;
  status = v8host::client::ValidateSessionConfig(builder->config);
  if (status != v8host_status_ok)
    return status;
  SessionConfigCopy config_copy = std::move(builder->config);
  builder.reset();

  std::shared_ptr<v8host::client::ClientConnection> connection(
      new (std::nothrow) v8host::client::ClientConnection());
  if (!connection)
    return v8host_status_no_memory;
  status = connection->Initialize(config_copy.broker_mode);
  if (status != v8host_status_ok)
    return status;

  std::unique_ptr<v8host_session_s> session(new (std::nothrow) v8host_session_s());
  if (!session)
    return v8host_status_no_memory;
  session->conn = connection;
  session->session_id = connection->AllocateSessionId();
  session->config = std::move(config_copy);
  session->dispatch = v8host::client::SessionDispatch::Create(
      &v8host::client::Dispatcher::Instance(), session.get(),
      v8host_session_state_closed);
  if (session->dispatch == nullptr)
    return v8host_status_no_memory;
  session->dispatch_id = session->dispatch->id();
  connection->RegisterSession(session.get());

  status = connection->SubmitControl(
      {v8host::client::PendingType::kCreate, session->session_id, 0},
      [&](const auto& header) {
        return v8host::protocol::BuildCreateSessionFrame(
            header, v8host::client::MakeCreatePayload(session->config));
      });
  if (status != v8host_status_ok) {
    connection->UnregisterSession(session->session_id);
    session->dispatch->CloseAndRelease();
    session->dispatch = nullptr;
    return status;
  }
  if (!v8host::client::ClientRuntime::Instance().ScheduleStart(connection)) {
    connection->UnregisterSession(session->session_id);
    session->dispatch->CloseAndRelease();
    session->dispatch = nullptr;
    return v8host_status_connect_failed;
  }

  *out_session = session.release();
  return v8host_status_ok;
}

v8host_status V8HOST_CDECL v8host_session_set_callbacks(
    v8host_session session,
    const v8host_callbacks* callbacks) {
  if (session == nullptr || callbacks == nullptr)
    return v8host_status_invalid_arg;
  if (callbacks->struct_size < sizeof(v8host_callbacks))
    return v8host_status_invalid_struct_size;
  return v8host::client::Dispatcher::Instance().SetCallbacks(
      session->dispatch_id, callbacks);
}

v8host_status V8HOST_CDECL v8host_session_start_run(
    v8host_session session,
    v8host_configure_run_cb configure,
    void* user_data,
    v8host_run* out_run) {
  if (out_run == nullptr)
    return v8host_status_invalid_arg;
  *out_run = nullptr;
  if (session == nullptr)
    return v8host_status_invalid_arg;

  std::unique_ptr<v8host_run_config_s> inputs(
      new (std::nothrow) v8host_run_config_s());
  if (!inputs)
    return v8host_status_no_memory;
  v8host_status status = v8host_status_ok;
  if (configure != nullptr) {
    status = configure(user_data, inputs.get());
    if (status != v8host_status_ok)
      return status;
  }
  if (inputs->status != v8host_status_ok)
    return inputs->status;
  const int32_t tier_override = inputs->has_tier ? inputs->tier : -1;
  const int32_t effective_tier =
      tier_override == -1 ? session->config.tier : tier_override;
  if (effective_tier == v8host_tier_trusted &&
      session->config.prohibit_dynamic_code != 0) {
    return v8host_status_invalid_arg;
  }

  std::string engine_utf8;
  std::string snapshot_utf8;
  status = v8host::client::WideToUtf8(inputs->engine.c_str(), false,
                                      &engine_utf8);
  if (status != v8host_status_ok)
    return status;
  status = v8host::client::WideToUtf8(inputs->snapshot.c_str(), false,
                                      &snapshot_utf8);
  if (status != v8host_status_ok)
    return status;
  const size_t variable_size = sizeof(uint32_t) + engine_utf8.size() +
                               sizeof(uint32_t) + snapshot_utf8.size() +
                               inputs->payload.size();
  if (v8host::protocol::kStartRunFixedSize + variable_size >
      v8host::protocol::kMaxFramePayload) {
    return v8host_status_quota_exceeded;
  }

  std::unique_lock<std::mutex> lock(session->mutex);
  if (session->closed)
    return v8host_status_invalid_state;
  size_t live_runs = 0;
  for (const auto& entry : session->runs) {
    if (!entry.second->terminal_local.load(std::memory_order_acquire))
      ++live_runs;
  }
  if (live_runs >= v8host::protocol::kMaxInFlightRunsPerSession)
    return v8host_status_quota_exceeded;

  std::unique_ptr<v8host_run_s> run(new (std::nothrow) v8host_run_s());
  if (!run)
    return v8host_status_no_memory;
  run->session = session;
  if (session->next_run_id == UINT32_MAX) {
    lock.unlock();
    session->conn->OnDisconnect(v8host::client::TransportDisconnect::kProtocolError);
    return v8host_status_quota_exceeded;
  }
  run->run_id = session->next_run_id++;
  v8host_run_s* run_handle = run.get();

  v8host::protocol::StartRunPayload payload;
  payload.tier_override = tier_override;
  payload.has_engine_override = inputs->has_engine;
  payload.has_snapshot = inputs->has_snapshot;
  payload.engine_filename = std::move(engine_utf8);
  payload.snapshot_path = std::move(snapshot_utf8);
  payload.guest_payload = std::move(inputs->payload);
  session->runs.emplace(run->run_id, std::move(run));
  status = session->conn->SubmitControl(
      {v8host::client::PendingType::kStart, session->session_id, run_handle->run_id},
      [&](const auto& header) {
        return v8host::protocol::BuildStartRunFrame(header, payload);
      });
  if (status != v8host_status_ok) {
    session->runs.erase(run_handle->run_id);
    lock.unlock();
    session->conn->TerminalizeExhaustion();
    return status;
  }
  *out_run = run_handle;
  return v8host_status_ok;
}

v8host_status V8HOST_CDECL v8host_run_post_message(
    v8host_run run,
    v8host_message_kind kind,
    const void* data,
    size_t size) {
  if (run == nullptr || !v8host::client::CountPairValid(data, size) ||
      (kind != v8host_message_kind_string &&
       kind != v8host_message_kind_binary)) {
    return v8host_status_invalid_arg;
  }
  if (size > v8host::protocol::kMaxFramePayload - sizeof(uint32_t))
    return v8host_status_quota_exceeded;
  if (run->terminal_local.load(std::memory_order_acquire))
    return v8host_status_run_terminal;
  if (v8host_runtime_s* runtime = run->runtime) {
    v8host_broker_s* broker = runtime->worker->broker;
    std::lock_guard<std::mutex> lock(broker->mutex);
    if (run->terminal_local.load(std::memory_order_acquire))
      return v8host_status_run_terminal;
    if (runtime->closed || broker->terminal)
      return v8host_status_invalid_state;
    auto header = v8host::client::MakeHeader(broker->conn.get(), runtime->id, run->run_id, 0, false);
    header.type = v8host::protocol::MessageType::RELAY_TO_WORKER;
    return broker->conn->Send(v8host::protocol::BuildRelayFrame(
        header, kind, static_cast<const uint8_t*>(data), size));
  }
  v8host_session_s* session = run->session;
  std::unique_lock<std::mutex> lock(session->mutex);
  if (session->closed)
    return v8host_status_invalid_state;
  if (run->terminal_local.load(std::memory_order_acquire))
    return v8host_status_run_terminal;
  auto header = v8host::client::MakeHeader(
      session->conn.get(), session->session_id, run->run_id, 0, false);
  header.type = v8host::protocol::MessageType::RELAY_TO_WORKER;
  const auto frame = v8host::protocol::BuildRelayFrame(
      header, kind, static_cast<const uint8_t*>(data), size);
  const v8host_status send_status = session->conn->Send(frame);
  return send_status;
}

v8host_status V8HOST_CDECL v8host_run_cancel(v8host_run run) {
  if (run == nullptr)
    return v8host_status_invalid_arg;
  if (run->terminal_local.load(std::memory_order_acquire))
    return v8host_status_run_terminal;
  if (v8host_runtime_s* runtime = run->runtime) {
    v8host_broker_s* broker = runtime->worker->broker;
    v8host_status status = v8host_status_ok;
    {
      std::lock_guard<std::mutex> lock(broker->mutex);
      if (run->terminal_local.load(std::memory_order_acquire))
        return v8host_status_run_terminal;
      if (runtime->closed || broker->terminal)
        return v8host_status_invalid_state;
      status = broker->conn->SubmitControl(
          {v8host::client::PendingType::kCancel, runtime->id, run->run_id},
          [](const auto& header) { return v8host::protocol::BuildCancelRunFrame(header); });
    }
    broker->conn->TerminalizeExhaustion();
    return status;
  }
  v8host_session_s* session = run->session;
  std::unique_lock<std::mutex> lock(session->mutex);
  if (session->closed)
    return v8host_status_invalid_state;
  if (run->terminal_local.load(std::memory_order_acquire))
    return v8host_status_run_terminal;
  v8host_status status = session->conn->SubmitControl(
      {v8host::client::PendingType::kCancel, session->session_id, run->run_id},
      [](const auto& header) { return v8host::protocol::BuildCancelRunFrame(header); });
  lock.unlock();
  session->conn->TerminalizeExhaustion();
  return status;
}

void V8HOST_CDECL v8host_session_close(v8host_session session) {
  if (session == nullptr)
    return;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->closed)
      return;
    session->closed = true;
    for (const auto& entry : session->runs)
      entry.second->terminal_local.store(true, std::memory_order_release);
  }
  session->conn->UnregisterSession(session->session_id);
  session->dispatch->CloseAndRelease();
  session->conn->SubmitControl(
      {v8host::client::PendingType::kClose, session->session_id, 0},
      [](const auto& header) { return v8host::protocol::BuildCloseSessionFrame(header); });
  session->conn->CloseTransport();
  session->Release();
}

//==========================================================================
// Brokers, workers, and runtimes (Contract C, design §6).
//==========================================================================

v8host_status V8HOST_CDECL v8host_broker_config_set_mode(
    v8host_broker_config config,
    v8host_broker_mode value) {
  return v8host::client::ApplySetter(config, [&](v8host_broker_config_s& builder) {
    if (value != v8host_broker_mode_dedicated && value != v8host_broker_mode_shared)
      return v8host_status_invalid_arg;
    builder.mode = value;
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_broker_config_set_callbacks(
    v8host_broker_config config,
    const v8host_broker_callbacks* callbacks) {
  return v8host::client::ApplySetter(config, [&](v8host_broker_config_s& builder) {
    if (callbacks == nullptr)
      return v8host_status_invalid_arg;
    if (callbacks->struct_size < sizeof(v8host_broker_callbacks))
      return v8host_status_invalid_struct_size;
    builder.table = *callbacks;
    builder.table.struct_size = sizeof(v8host_broker_callbacks);
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_broker_open(
    v8host_configure_broker_cb configure,
    void* user_data,
    v8host_broker* out_broker) {
  if (out_broker == nullptr)
    return v8host_status_invalid_arg;
  *out_broker = nullptr;
  if (!v8host::client::ClientRuntime::Instance().IsInitialized())
    return v8host_status_not_initialized;
  v8host_broker_config_s builder;
  if (configure != nullptr) {
    const v8host_status status = configure(user_data, &builder);
    if (status != v8host_status_ok)
      return status;
  }
  if (builder.status != v8host_status_ok)
    return builder.status;
  std::shared_ptr<v8host::client::ClientConnection> connection(
      new (std::nothrow) v8host::client::ClientConnection());
  if (!connection)
    return v8host_status_no_memory;
  v8host_status status = connection->Initialize(builder.mode);
  if (status != v8host_status_ok)
    return status;
  std::unique_ptr<v8host_broker_s> broker(new (std::nothrow) v8host_broker_s());
  if (!broker)
    return v8host_status_no_memory;
  broker->conn = connection;
  broker->mode = builder.mode;
  broker->dispatch = v8host::client::SessionDispatch::CreateBroker(
      &v8host::client::Dispatcher::Instance(), broker.get(), builder.table);
  if (broker->dispatch == nullptr)
    return v8host_status_no_memory;
  connection->SetBroker(broker.get());
  if (!v8host::client::ClientRuntime::Instance().ScheduleStart(connection)) {
    connection->SetBroker(nullptr);
    broker->dispatch->CloseAndRelease();
    return v8host_status_connect_failed;
  }
  *out_broker = broker.release();
  return v8host_status_ok;
}

void V8HOST_CDECL v8host_broker_close(v8host_broker broker) {
  if (broker == nullptr)
    return;
  {
    std::lock_guard<std::mutex> lock(broker->mutex);
    if (broker->released)
      return;
    broker->released = true;
    broker->terminal = true;
    v8host::client::CloseGraph(broker, true);
    broker->dispatch->CloseAndRelease();
    broker->dispatch = nullptr;
  }
  broker->conn->SetBroker(nullptr);
  broker->conn->CloseTransport();
  broker->Release();
}

// The worker's policy setters reuse the session's: a worker builder is a
// session builder with the worker-only fields after it.
v8host_status V8HOST_CDECL v8host_worker_config_set_tier(
    v8host_worker_config config,
    v8host_tier value) {
  return v8host_config_set_tier(config, value);
}

v8host_status V8HOST_CDECL v8host_worker_config_set_integrity_level(
    v8host_worker_config config,
    v8host_integrity_level value) {
  return v8host_config_set_integrity_level(config, value);
}

v8host_status V8HOST_CDECL v8host_worker_config_set_delayed_integrity_level(
    v8host_worker_config config,
    v8host_integrity_level value) {
  return v8host_config_set_delayed_integrity_level(config, value);
}

v8host_status V8HOST_CDECL v8host_worker_config_set_initial_token_level(
    v8host_worker_config config,
    v8host_token_level value) {
  return v8host_config_set_initial_token_level(config, value);
}

v8host_status V8HOST_CDECL v8host_worker_config_set_lockdown_token_level(
    v8host_worker_config config,
    v8host_token_level value) {
  return v8host_config_set_lockdown_token_level(config, value);
}

v8host_status V8HOST_CDECL v8host_worker_config_add_file_rule_utf8(
    v8host_worker_config config,
    const char* pattern,
    size_t length,
    bool readonly) {
  return v8host_config_add_file_rule_utf8(config, pattern, length, readonly);
}

v8host_status V8HOST_CDECL v8host_worker_config_add_file_rule_utf16(
    v8host_worker_config config,
    const v8host_char16* pattern,
    size_t length,
    bool readonly) {
  return v8host_config_add_file_rule_utf16(config, pattern, length, readonly);
}

v8host_status V8HOST_CDECL v8host_worker_config_set_use_app_container(
    v8host_worker_config config,
    bool value) {
  return v8host_config_set_use_app_container(config, value);
}

v8host_status V8HOST_CDECL v8host_worker_config_set_low_privilege_app_container(
    v8host_worker_config config,
    bool value) {
  return v8host_config_set_low_privilege_app_container(config, value);
}

v8host_status V8HOST_CDECL v8host_worker_config_set_profile_name_utf8(
    v8host_worker_config config,
    const char* value,
    size_t length) {
  return v8host_config_set_profile_name_utf8(config, value, length);
}

v8host_status V8HOST_CDECL v8host_worker_config_set_profile_name_utf16(
    v8host_worker_config config,
    const v8host_char16* value,
    size_t length) {
  return v8host_config_set_profile_name_utf16(config, value, length);
}

v8host_status V8HOST_CDECL v8host_worker_config_add_capability_utf8(
    v8host_worker_config config,
    const char* value,
    size_t length) {
  return v8host_config_add_capability_utf8(config, value, length);
}

v8host_status V8HOST_CDECL v8host_worker_config_add_capability_utf16(
    v8host_worker_config config,
    const v8host_char16* value,
    size_t length) {
  return v8host_config_add_capability_utf16(config, value, length);
}

v8host_status V8HOST_CDECL v8host_worker_config_set_engine_dll_utf8(
    v8host_worker_config config,
    const char* value,
    size_t length) {
  return v8host::client::ApplySetter(config, [&](v8host_worker_config_s& builder) {
    std::wstring text;
    const v8host_status status = v8host::client::CopyUtf8(value, length, &text);
    return status != v8host_status_ok ? status : SetWorkerEngine(builder, std::move(text));
  });
}

v8host_status V8HOST_CDECL v8host_worker_config_set_engine_dll_utf16(
    v8host_worker_config config,
    const v8host_char16* value,
    size_t length) {
  return v8host::client::ApplySetter(config, [&](v8host_worker_config_s& builder) {
    std::wstring text;
    const v8host_status status = v8host::client::CopyUtf16(value, length, &text);
    return status != v8host_status_ok ? status : SetWorkerEngine(builder, std::move(text));
  });
}

v8host_status V8HOST_CDECL v8host_worker_config_add_startup_snapshot_path_utf8(
    v8host_worker_config config,
    const char* value,
    size_t length) {
  return v8host::client::ApplySetter(config, [&](v8host_worker_config_s& builder) {
    std::wstring text;
    const v8host_status status = v8host::client::CopyUtf8(value, length, &text);
    return status != v8host_status_ok ? status : AddWorkerSnapshot(builder, std::move(text));
  });
}

v8host_status V8HOST_CDECL v8host_worker_config_add_startup_snapshot_path_utf16(
    v8host_worker_config config,
    const v8host_char16* value,
    size_t length) {
  return v8host::client::ApplySetter(config, [&](v8host_worker_config_s& builder) {
    std::wstring text;
    const v8host_status status = v8host::client::CopyUtf16(value, length, &text);
    return status != v8host_status_ok ? status : AddWorkerSnapshot(builder, std::move(text));
  });
}

v8host_status V8HOST_CDECL v8host_worker_config_set_max_runtimes(
    v8host_worker_config config,
    uint32_t value) {
  return v8host::client::ApplySetter(config, [&](v8host_worker_config_s& builder) {
    if (value == 0)
      return v8host_status_invalid_arg;
    if (value > v8host::protocol::kMaxRuntimesPerWorker)
      return v8host_status_quota_exceeded;
    builder.max_runtimes = value;
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_worker_config_set_flags(
    v8host_worker_config config,
    v8host_worker_flags value) {
  return v8host::client::ApplySetter(config, [&](v8host_worker_config_s& builder) {
    constexpr uint32_t kKnown =
        v8host_worker_flags_close_when_empty | v8host_worker_flags_single_use;
    if (static_cast<uint32_t>(value) & ~kKnown)
      return v8host_status_invalid_arg;
    builder.flags = static_cast<uint32_t>(value);
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_worker_open(
    v8host_broker broker,
    v8host_configure_worker_cb configure,
    void* user_data,
    v8host_worker* out_worker) {
  if (out_worker == nullptr)
    return v8host_status_invalid_arg;
  *out_worker = nullptr;
  if (broker == nullptr)
    return v8host_status_invalid_arg;
  std::unique_ptr<v8host_worker_config_s> builder(new (std::nothrow) v8host_worker_config_s());
  if (!builder)
    return v8host_status_no_memory;
  v8host_status status = v8host_status_ok;
  if (configure != nullptr) {
    status = configure(user_data, builder.get());
    if (status != v8host_status_ok)
      return status;
  }
  if (builder->status != v8host_status_ok)
    return builder->status;
  // The mode is the broker's, and ACG follows the tier.
  builder->config.broker_mode = broker->mode;
  builder->config.prohibit_dynamic_code = builder->config.tier == v8host_tier_untrusted ? 1 : 0;
  status = v8host::client::ValidateSessionConfig(builder->config);
  if (status != v8host_status_ok)
    return status;
  v8host::protocol::CreateWorkerPayload payload;
  payload.session = v8host::client::MakeCreatePayload(builder->config);
  payload.max_runtimes = builder->max_runtimes;
  payload.flags = (builder->flags & v8host_worker_flags_single_use) ? v8host::protocol::kWorkerFlagSingleUse : 0;
  status = v8host::client::WideToUtf8(builder->engine.c_str(), true, &payload.engine_dll);
  if (status != v8host_status_ok)
    return status;
  // Canonicalizing never lengthens a path, so the profile's size is bounded
  // from what the host passed (design §4.3): no surprise async quota error.
  size_t profile = 16 + 4 + 4 +
      (payload.engine_dll.empty()
           ? (builder->config.tier == v8host_tier_untrusted ? 11 : 9)
           : payload.engine_dll.size());
  for (const std::wstring& path : builder->snapshots) {
    std::string utf8;
    status = v8host::client::WideToUtf8(path.c_str(), false, &utf8);
    if (status != v8host_status_ok)
      return status;
    profile += 4 + utf8.size();
    payload.snapshot_paths.push_back(std::move(utf8));
  }
  v8host::protocol::Writer encoded;
  v8host::protocol::EncodeCreateWorkerPayload(payload, encoded);
  if (profile > kWorkerProfileMaxBytes || encoded.size() > v8host::protocol::kMaxFramePayload)
    return v8host_status_quota_exceeded;

  v8host_worker_s* worker = nullptr;
  {
    std::lock_guard<std::mutex> lock(broker->mutex);
    if (broker->released || broker->terminal)
      return v8host_status_invalid_state;
    if (broker->open_workers >= v8host::protocol::kMaxSessionsPerConnection ||
        broker->next_id == UINT32_MAX)
      return v8host_status_quota_exceeded;
    worker = new (std::nothrow) v8host_worker_s();
    if (worker == nullptr)
      return v8host_status_no_memory;
    worker->broker = broker;
    broker->AddRef();
    worker->id = broker->next_id++;
    worker->max_runtimes = builder->max_runtimes;
    worker->flags = builder->flags;
    worker->snapshot_count = static_cast<uint32_t>(builder->snapshots.size());
    broker->workers[worker->id] = worker;
    worker->AddRef();
    ++broker->open_workers;
    status = broker->conn->SubmitControl(
        {v8host::client::PendingType::kCreateWorker, worker->id, 0},
        [&](const auto& header) { return v8host::protocol::BuildCreateWorkerFrame(header, payload); });
    if (status != v8host_status_ok) {
      broker->workers.erase(worker->id);
      --broker->open_workers;
      worker->Release();
      worker->Release();
      worker = nullptr;
    }
  }
  if (status != v8host_status_ok) {
    broker->conn->TerminalizeExhaustion();
    return status;
  }
  *out_worker = worker;
  return v8host_status_ok;
}

void V8HOST_CDECL v8host_worker_close(v8host_worker worker) {
  if (worker == nullptr)
    return;
  v8host_broker_s* broker = worker->broker;
  {
    std::lock_guard<std::mutex> lock(broker->mutex);
    if (worker->released)
      return;
    worker->released = true;
    --broker->open_workers;
    if (!worker->closed) {
      worker->closed = true;
      auto runtimes = v8host::client::Referenced(worker->runtimes);
      for (v8host_runtime_s* runtime : runtimes) {
        runtime->closed = runtime->silenced = true;
        v8host::client::MarkRunsTerminal(runtime);
        v8host::client::UnregisterRuntime(broker, runtime);
        runtime->Release();
      }
      v8host::client::UnregisterWorker(broker, worker);
      if (!broker->terminal && !worker->closing)
        broker->conn->SubmitClose({v8host::client::PendingType::kCloseWorker, worker->id, 0});
    }
    // Nothing more is delivered for the worker or anything in it.
    if (broker->dispatch != nullptr)
      broker->dispatch->Suppress(worker->id, 0);
  }
  worker->Release();
}

uint32_t V8HOST_CDECL v8host_worker_get_process_id(v8host_worker worker) {
  if (worker == nullptr)
    return 0;
  std::lock_guard<std::mutex> lock(worker->broker->mutex);
  return worker->released || worker->closed || worker->orphaned
             ? 0
             : worker->process_id.load(std::memory_order_acquire);
}

uint32_t V8HOST_CDECL v8host_worker_get_id(v8host_worker worker) {
  if (worker == nullptr)
    return 0;
  std::lock_guard<std::mutex> lock(worker->broker->mutex);
  return worker->released || worker->closed || worker->orphaned ? 0 : worker->id;
}

v8host_status V8HOST_CDECL v8host_runtime_config_set_startup_snapshot_index(
    v8host_runtime_config config,
    uint32_t index) {
  return v8host::client::ApplySetter(config, [&](v8host_runtime_config_s& builder) {
    builder.has_snapshot = true;
    builder.snapshot_index = index;
    return v8host_status_ok;
  });
}

v8host_status V8HOST_CDECL v8host_runtime_open(
    v8host_worker worker,
    v8host_configure_runtime_cb configure,
    void* user_data,
    v8host_runtime* out_runtime) {
  if (out_runtime == nullptr)
    return v8host_status_invalid_arg;
  *out_runtime = nullptr;
  if (worker == nullptr)
    return v8host_status_invalid_arg;
  v8host_runtime_config_s builder;
  v8host_status status = v8host_status_ok;
  if (configure != nullptr) {
    status = configure(user_data, &builder);
    if (status != v8host_status_ok)
      return status;
  }
  if (builder.status != v8host_status_ok)
    return builder.status;
  v8host_broker_s* broker = worker->broker;
  v8host_runtime_s* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(broker->mutex);
    if (worker->released || worker->closed || worker->closing || broker->released ||
        broker->terminal)
      return v8host_status_invalid_state;
    if (builder.has_snapshot && builder.snapshot_index >= worker->snapshot_count)
      return v8host_status_invalid_arg;
    // The client counts admissions exactly as the coordinator does (§2.1).
    if (worker->admissions >= worker->max_runtimes ||
        ((worker->flags & v8host_worker_flags_single_use) && worker->created >= worker->max_runtimes) ||
        broker->next_id == UINT32_MAX)
      return v8host_status_quota_exceeded;
    runtime = new (std::nothrow) v8host_runtime_s();
    if (runtime == nullptr)
      return v8host_status_no_memory;
    runtime->worker = worker;
    worker->AddRef();
    runtime->id = broker->next_id++;
    broker->runtimes[runtime->id] = runtime;
    runtime->AddRef();
    worker->runtimes[runtime->id] = runtime;
    runtime->AddRef();
    ++worker->admissions;
    ++worker->created;
    v8host::protocol::CreateRuntimePayload payload;
    payload.worker_id = worker->id;
    payload.snapshot_index = builder.has_snapshot ? static_cast<int32_t>(builder.snapshot_index) : -1;
    status = broker->conn->SubmitControl(
        {v8host::client::PendingType::kCreateRuntime, runtime->id, 0},
        [&](const auto& header) { return v8host::protocol::BuildCreateRuntimeFrame(header, payload); });
    if (status != v8host_status_ok) {
      --worker->admissions;
      --worker->created;
      v8host::client::UnregisterRuntime(broker, runtime);
      runtime->Release();
      runtime = nullptr;
    }
  }
  if (status != v8host_status_ok) {
    broker->conn->TerminalizeExhaustion();
    return status;
  }
  *out_runtime = runtime;
  return v8host_status_ok;
}

void V8HOST_CDECL v8host_runtime_close(v8host_runtime runtime) {
  if (runtime == nullptr)
    return;
  v8host_worker_s* worker = runtime->worker;
  v8host_broker_s* broker = worker->broker;
  {
    std::lock_guard<std::mutex> lock(broker->mutex);
    if (runtime->released)
      return;
    runtime->released = true;
    if (!runtime->closed) {
      runtime->closed = runtime->silenced = true;
      v8host::client::MarkRunsTerminal(runtime);
      v8host::client::UnregisterRuntime(broker, runtime);
      if (!broker->terminal && !worker->closed)
        broker->conn->SubmitClose({v8host::client::PendingType::kCloseRuntime, runtime->id, 0});
      v8host::client::ReleaseAdmission(broker, worker);
    }
    if (broker->dispatch != nullptr)
      broker->dispatch->Suppress(0, runtime->id);
  }
  runtime->Release();
}

uint32_t V8HOST_CDECL v8host_runtime_get_id(v8host_runtime runtime) {
  if (runtime == nullptr)
    return 0;
  std::lock_guard<std::mutex> lock(runtime->worker->broker->mutex);
  return runtime->released || runtime->closed || runtime->orphaned || runtime->worker->released ||
                 runtime->worker->broker->released
             ? 0
             : runtime->id;
}

v8host_status V8HOST_CDECL v8host_runtime_start_run(
    v8host_runtime runtime,
    v8host_configure_run_cb configure,
    void* user_data,
    v8host_run* out_run) {
  if (out_run == nullptr)
    return v8host_status_invalid_arg;
  *out_run = nullptr;
  if (runtime == nullptr)
    return v8host_status_invalid_arg;
  std::unique_ptr<v8host_run_config_s> inputs(new (std::nothrow) v8host_run_config_s());
  if (!inputs)
    return v8host_status_no_memory;
  v8host_status status = v8host_status_ok;
  if (configure != nullptr) {
    status = configure(user_data, inputs.get());
    if (status != v8host_status_ok)
      return status;
  }
  if (inputs->status != v8host_status_ok)
    return inputs->status;
  // The runtime's profile belongs to its worker (D6), even an equal value.
  if (inputs->has_tier || inputs->has_engine || inputs->has_snapshot)
    return v8host_status_profile_already_bound;
  if (v8host::protocol::kStartRunFixedSize + 2 * sizeof(uint32_t) + inputs->payload.size() >
      v8host::protocol::kMaxFramePayload)
    return v8host_status_quota_exceeded;
  v8host_broker_s* broker = runtime->worker->broker;
  v8host_run_s* handle = nullptr;
  {
    std::lock_guard<std::mutex> lock(broker->mutex);
    if (runtime->released || runtime->closed || broker->terminal)
      return v8host_status_invalid_state;
    size_t live = 0;
    for (const auto& entry : runtime->runs)
      if (!entry.second->terminal_local.load(std::memory_order_acquire))
        ++live;
    if (live >= v8host::protocol::kMaxInFlightRunsPerSession || runtime->next_run_id == UINT32_MAX)
      return v8host_status_quota_exceeded;
    std::unique_ptr<v8host_run_s> run(new (std::nothrow) v8host_run_s());
    if (!run)
      return v8host_status_no_memory;
    run->runtime = runtime;
    run->run_id = runtime->next_run_id++;
    handle = run.get();
    v8host::protocol::StartRunPayload payload;
    payload.tier_override = -1;
    payload.guest_payload = std::move(inputs->payload);
    runtime->runs.emplace(handle->run_id, std::move(run));
    status = broker->conn->SubmitControl(
        {v8host::client::PendingType::kStart, runtime->id, handle->run_id},
        [&](const auto& header) { return v8host::protocol::BuildStartRunFrame(header, payload); });
    if (status != v8host_status_ok) {
      runtime->runs.erase(handle->run_id);
      handle = nullptr;
    }
  }
  if (status != v8host_status_ok) {
    broker->conn->TerminalizeExhaustion();
    return status;
  }
  *out_run = handle;
  return v8host_status_ok;
}

uint32_t V8HOST_CDECL v8host_run_get_id(v8host_run run) {
  return run != nullptr ? run->run_id : 0;
}

}  // extern "C"
