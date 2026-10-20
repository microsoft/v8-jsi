// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "v8host_test_support.h"  // windows.h first
#include "v8host_client_test_config.h"
#include "v8host_client_transport.h"
#include "v8host_dispatcher.h"
#include "v8host_protocol.h"
#include "v8host_protocol_messages.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace v8host::protocol;
using v8host::client::ClientTransport;
using v8host::client::ClientTransportDelegate;
using v8host::client::FailNextCallbackCopyForTesting;
using v8host::client::LastSessionDeleteThreadIdForTesting;
using v8host::client::LifecycleThreadIdForTesting;
using v8host::client::SetInitializeAfterDispatcherHookForTesting;
using v8host::client::SetInboundStatusObserverForTesting;
using v8host::client::SetTransportFactoryForTesting;
using v8host::client::TransportDisconnect;
using v8host::client::TransportParams;
using v8host::test::OpenSession;
using v8host::test::StartRun;
using v8host::test::TestCase;
using v8host::test::TestFileRule;
using v8host::test::TestRunInputs;
using v8host::test::TestSessionConfig;

bool Fail(std::string* detail, const char* text) {
  *detail = text;
  return false;
}

std::mutex g_init_mutex;
std::condition_variable g_init_cv;
bool g_dispatcher_captured = false;
bool g_racer_calling = false;
v8host_status g_main_init_status = v8host_status_internal_error;
v8host_status g_racer_init_status = v8host_status_internal_error;
DWORD g_initializing_thread = 0;
bool g_preinitialize_output_was_nulled = false;
bool g_preinitialize_broker_output_was_nulled = false;
std::atomic<int> g_inbound_ok{0};
std::atomic<int> g_inbound_invalid{0};
std::atomic<int> g_inbound_other{0};

void InitializeAfterDispatcherHook() {
  std::unique_lock<std::mutex> lock(g_init_mutex);
  g_dispatcher_captured = true;
  g_init_cv.notify_all();
  g_init_cv.wait(lock, [] { return g_racer_calling; });
}

void ObserveInboundStatus(v8host_status status) {
  if (status == v8host_status_ok)
    g_inbound_ok.fetch_add(1, std::memory_order_relaxed);
  else if (status == v8host_status_invalid_state)
    g_inbound_invalid.fetch_add(1, std::memory_order_relaxed);
  else
    g_inbound_other.fetch_add(1, std::memory_order_relaxed);
}

bool PumpUntil(const std::function<bool()>& done, DWORD timeout = 5000) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout;
  while (!done()) {
    if (::GetTickCount64() >= deadline)
      return done();
    MSG msg;
    if (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
      ::DispatchMessageW(&msg);
    else
      ::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
  }
  return true;
}

void Drain() {
  MSG msg;
  while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    ::DispatchMessageW(&msg);
}

class FakeTransport final : public ClientTransport {
 public:
  explicit FakeTransport(ClientTransportDelegate* delegate)
      : delegate_(delegate) {}

  v8host_status Start() override {
    started = true;
    if (hold_until_start) {
      std::lock_guard<std::mutex> lock(mutex);
      if (closed.load()) return v8host_status_connect_failed;
      for (auto& frame : waiting)
        outbound.push_back(std::move(frame));
      waiting.clear();
    }
    if (inline_failure) delegate_->OnDisconnect(TransportDisconnect::kConnectFailed);
    delegate_->OnConnected(1, kWireVersionMajor, kWireVersionMinor);
    return start_status;
  }
  v8host_status SendFrame(const uint8_t* bytes, size_t len) override {
    std::lock_guard<std::mutex> lock(mutex);
    FrameHeader h;
    if (DecodeAndValidateFrame(bytes, len, &h) == DecodeStatus::kOk &&
        (h.type == MessageType::CLOSE_WORKER || h.type == MessageType::CLOSE_RUNTIME) &&
        close_failures != 0) {
      --close_failures;
      return close_failure_status;  // failure admits no frame
    }
    auto& frames = hold_until_start && !started ? waiting : outbound;
    frames.emplace_back(bytes, bytes + len);
    return send_status;
  }
  uint32_t conn_id() const override { return 1; }
  void Close() override { closed.store(true); }
  void Wake() override { ++wakes; }
  void Pump() { delegate_->OnPump(); }

  std::vector<uint8_t> Frame(size_t index) {
    std::lock_guard<std::mutex> lock(mutex);
    return outbound.at(index);
  }
  size_t FrameCount() {
    std::lock_guard<std::mutex> lock(mutex);
    return outbound.size();
  }
  void Inbound(std::vector<uint8_t> frame) {
    delegate_->OnInboundFrame(frame.data(), frame.size());
  }
  void Disconnect() {
    delegate_->OnDisconnect(TransportDisconnect::kBrokerLost);
  }
  void Disconnect(TransportDisconnect reason) {
    delegate_->OnDisconnect(reason);
  }

  ClientTransportDelegate* delegate_;
  std::mutex mutex;
  std::vector<std::vector<uint8_t>> outbound;
  std::vector<std::vector<uint8_t>> waiting;
  std::atomic<bool> closed{false};
  bool started = false, inline_failure = false;
  bool hold_until_start = false;
  v8host_status start_status = v8host_status_ok;
  v8host_status send_status = v8host_status_ok;
  unsigned close_failures = 0, wakes = 0;
  v8host_status close_failure_status = v8host_status_quota_exceeded;
};

std::mutex& FakesMutex() {
  static std::mutex* const mutex = new std::mutex();
  return *mutex;
}

std::vector<FakeTransport*>& Fakes() {
  static std::vector<FakeTransport*>* const fakes =
      new std::vector<FakeTransport*>();
  return *fakes;
}

ClientTransport* MakeFake(const TransportParams&, ClientTransportDelegate* d) {
  auto* fake = new (std::nothrow) FakeTransport(d);
  if (fake != nullptr) {
    std::lock_guard<std::mutex> lock(FakesMutex());
    Fakes().push_back(fake);
  }
  return fake;
}

FakeTransport* LatestFake() {
  std::lock_guard<std::mutex> lock(FakesMutex());
  return Fakes().back();
}

TestSessionConfig BasicConfig() {
  TestSessionConfig c;
  c.broker_mode = v8host_broker_mode_shared;
  c.tier = v8host_tier_untrusted;
  c.prohibit_dynamic_code = 1;
  return c;
}

TestRunInputs BasicRun(const void* data = nullptr, size_t len = 0) {
  TestRunInputs i;
  i.tier_override = -1;
  i.payload = data;
  i.payload_len = len;
  return i;
}

bool DecodeFrame(const std::vector<uint8_t>& frame,
                 FrameHeader* header,
                 const uint8_t** payload,
                 size_t* payload_len) {
  return DecodeAndValidateFrame(frame.data(), frame.size(), header, payload,
                                payload_len) == DecodeStatus::kOk;
}

std::vector<uint8_t> EventFrame(MessageType type,
                                uint32_t session,
                                uint32_t run = 0) {
  FrameHeader h;
  h.version_major = kWireVersionMajor;
  h.version_minor = kWireVersionMinor;
  h.type = type;
  h.conn_id = 1;
  h.target_id = session;
  h.run_id = run;
  if (type == MessageType::STARTUP_READY)
    return BuildStartupReadyFrame(h, StartupReadyPayload{1, 0, 4321});
  std::vector<uint8_t> out;
  EncodeHeader(h, out);
  return out;
}

// RESULT frames carry the terminal-disposition codec, so build them through the
// protocol builder rather than EventFrame (which is header-only).
std::vector<uint8_t> ResultFrame(uint32_t session,
                                 uint32_t run,
                                 ResultDisposition disposition =
                                     ResultDisposition::kCompleted) {
  FrameHeader h;
  h.version_major = kWireVersionMajor;
  h.version_minor = kWireVersionMinor;
  h.conn_id = 1;
  h.target_id = session;
  h.run_id = run;
  return BuildResultFrame(h, disposition);
}

std::vector<uint8_t> AckFor(const std::vector<uint8_t>& request) {
  FrameHeader h;
  DecodeAndValidateFrame(request.data(), request.size(), &h);
  return BuildAckFrame(h);
}

std::vector<uint8_t> ErrorFor(const std::vector<uint8_t>& request) {
  FrameHeader h;
  DecodeAndValidateFrame(request.data(), request.size(), &h);
  ErrorPayload error;
  error.status_code = StatusCode::ERROR_BAD_STATE;
  error.message = "bad";
  return BuildErrorFrame(h, error);
}

struct Recorder {
  std::vector<int32_t> states;
  std::vector<v8host_status> state_status;
  std::vector<int32_t> events;
  std::vector<v8host_status> event_status;
  std::vector<int32_t> kinds;
  std::vector<std::string> messages;
  std::atomic<int> total{0};
  int disconnects = 0;
  DWORD callback_thread = 0;
  DWORD state_thread = 0;
  DWORD run_thread = 0;
  DWORD relay_thread = 0;
  DWORD disconnect_thread = 0;
  v8host_session_s* session = nullptr;
  v8host_run_s* run = nullptr;
  bool close_in_callback = false;
  v8host_status after_close = v8host_status_ok;
  v8host_status post_after_close = v8host_status_ok;
  v8host_status cancel_after_close = v8host_status_ok;
  v8host_status set_after_close = v8host_status_ok;
};

v8host_callbacks Callbacks(Recorder* r);

void V8HOST_CDECL OnState(void* user_data,
                         v8host_session session,
                         v8host_session_state state,
                         v8host_status status) {
  auto* r = static_cast<Recorder*>(user_data);
  r->callback_thread = ::GetCurrentThreadId();
  r->state_thread = r->callback_thread;
  r->states.push_back(state);
  r->state_status.push_back(status);
  r->total.fetch_add(1);
  if (r->close_in_callback) {
    r->close_in_callback = false;
    v8host_session_close(session);
    v8host_run_s* ignored = reinterpret_cast<v8host_run_s*>(1);
    TestRunInputs input = BasicRun();
    r->after_close = StartRun(session, &input, &ignored);
    v8host_callbacks callbacks = Callbacks(r);
    r->set_after_close = v8host_session_set_callbacks(session, &callbacks);
    if (r->run != nullptr) {
      r->post_after_close =
          v8host_run_post_message(r->run, v8host_message_kind_binary, nullptr,
                                  0);
      r->cancel_after_close = v8host_run_cancel(r->run);
    }
  }
}

void V8HOST_CDECL OnRun(void* user_data,
                       v8host_run run,
                       v8host_run_event event,
                       v8host_status status) {
  auto* r = static_cast<Recorder*>(user_data);
  r->callback_thread = ::GetCurrentThreadId();
  r->run_thread = r->callback_thread;
  r->events.push_back(event);
  r->event_status.push_back(status);
  r->total.fetch_add(1);
  r->run = run;
}

void V8HOST_CDECL OnRelay(void* user_data,
                         v8host_run,
                         v8host_message_kind kind,
                         const void* data,
                         size_t len) {
  auto* r = static_cast<Recorder*>(user_data);
  r->callback_thread = ::GetCurrentThreadId();
  r->relay_thread = r->callback_thread;
  r->kinds.push_back(kind);
  r->messages.emplace_back(len == 0 ? "" : std::string(
      static_cast<const char*>(data), len));
  r->total.fetch_add(1);
}

void V8HOST_CDECL OnDisconnect(void* user_data) {
  auto* r = static_cast<Recorder*>(user_data);
  r->callback_thread = ::GetCurrentThreadId();
  r->disconnect_thread = r->callback_thread;
  ++r->disconnects;
  r->total.fetch_add(1);
}

v8host_callbacks Callbacks(Recorder* r) {
  v8host_callbacks c = {};
  c.struct_size = sizeof(c);
  c.user_data = r;
  c.on_session_state = OnState;
  c.on_run_event = OnRun;
  c.on_relay_message = OnRelay;
  c.on_broker_disconnect = OnDisconnect;
  return c;
}

v8host_session_s* Create(FakeTransport** fake = nullptr) {
  TestSessionConfig c = BasicConfig();
  v8host_session_s* session = nullptr;
  if (OpenSession(&c, &session) != v8host_status_ok)
    return nullptr;
  if (fake)
    *fake = LatestFake();
  return session;
}

struct TopologyEvent {
  char kind;
  void* handle;
  int32_t code;
  v8host_status status;
  std::string data;
};

struct TopologyRecorder {
  std::vector<TopologyEvent> events;
  bool wrong_thread = false;
  std::function<void(char, void*)> during;

  void Record(char kind, void* handle, int32_t code, v8host_status status,
              std::string data = {}) {
    wrong_thread |= ::GetCurrentThreadId() != g_initializing_thread;
    events.push_back({kind, handle, code, status, std::move(data)});
    if (during)
      during(kind, handle);
  }
};

void V8HOST_CDECL TopologyBroker(void* p, v8host_broker b,
                               v8host_broker_state state, v8host_status status) {
  static_cast<TopologyRecorder*>(p)->Record('b', b, state, status);
}
void V8HOST_CDECL TopologyWorker(void* p, v8host_worker w,
                               v8host_worker_state state, v8host_status status) {
  static_cast<TopologyRecorder*>(p)->Record('w', w, state, status);
}
void V8HOST_CDECL TopologyRuntime(void* p, v8host_runtime r,
                                v8host_runtime_state state, v8host_status status) {
  static_cast<TopologyRecorder*>(p)->Record('r', r, state, status);
}
void V8HOST_CDECL TopologyRun(void* p, v8host_run r,
                            v8host_run_event event, v8host_status status) {
  static_cast<TopologyRecorder*>(p)->Record('u', r, event, status);
}
void V8HOST_CDECL TopologyRelay(void* p, v8host_run r, v8host_message_kind kind,
                              const void* data, size_t size) {
  static_cast<TopologyRecorder*>(p)->Record(
      'm', r, kind, v8host_status_ok,
      size ? std::string(static_cast<const char*>(data), size) : std::string());
}

template <typename Config>
v8host_status V8HOST_CDECL ConfigureTopology(void* p, Config config) {
  return (*static_cast<std::function<v8host_status(Config)>*>(p))(config);
}

using WorkerSetup = std::function<v8host_status(v8host_worker_config)>;
using RuntimeSetup = std::function<v8host_status(v8host_runtime_config)>;
using RunSetup = std::function<v8host_status(v8host_run_config)>;

// Own every host reference independently, including children of closed parents.
struct TopologyFixture {
  TopologyRecorder record;
  v8host_broker broker = nullptr;
  FakeTransport* fake = nullptr;
  std::vector<v8host_worker> workers;
  std::vector<v8host_runtime> runtimes;

  ~TopologyFixture() {
    for (auto r : runtimes)
      v8host_runtime_close(r);
    for (auto w : workers)
      v8host_worker_close(w);
    v8host_broker_close(broker);
    Drain();
  }
  bool Open(bool connect = true) {
    std::function<v8host_status(v8host_broker_config)> configure =
        [&](v8host_broker_config c) {
          v8host_broker_callbacks table = V8HOST_BROKER_CALLBACKS_INIT;
          table.user_data = &record;
          table.on_broker_state = TopologyBroker;
          table.on_worker_state = TopologyWorker;
          table.on_runtime_state = TopologyRuntime;
          table.on_run_event = TopologyRun;
          table.on_relay_message = TopologyRelay;
          auto status = v8host_broker_config_set_mode(c, v8host_broker_mode_shared);
          if (status == v8host_status_ok)
            status = v8host_broker_config_set_callbacks(c, &table);
          return status;
        };
    if (v8host_broker_open(ConfigureTopology<v8host_broker_config>, &configure,
                           &broker) != v8host_status_ok)
      return false;
    fake = LatestFake();
    fake->hold_until_start = !connect;
    if (connect)
      Drain();
    record.events.clear();
    return true;
  }
  v8host_status Worker(v8host_worker* out, WorkerSetup configure = {}) {
    auto status = v8host_worker_open(
        broker, configure ? ConfigureTopology<v8host_worker_config> : nullptr,
        &configure, out);
    if (status == v8host_status_ok)
      workers.push_back(*out);
    return status;
  }
  v8host_status Runtime(v8host_worker w, v8host_runtime* out,
                        RuntimeSetup configure = {}) {
    auto status = v8host_runtime_open(
        w, configure ? ConfigureTopology<v8host_runtime_config> : nullptr,
        &configure, out);
    if (status == v8host_status_ok)
      runtimes.push_back(*out);
    return status;
  }
  v8host_status Run(v8host_runtime r, v8host_run* out, RunSetup configure = {}) {
    return v8host_runtime_start_run(
        r, configure ? ConfigureTopology<v8host_run_config> : nullptr,
        &configure, out);
  }
  void Close(v8host_runtime r) {
    v8host_runtime_close(r);
    for (auto& entry : runtimes)
      if (entry == r) entry = nullptr;
  }
  void Close(v8host_worker w) {
    v8host_worker_close(w);
    for (auto& entry : workers)
      if (entry == w) entry = nullptr;
  }
  size_t Count(MessageType type) {
    size_t count = 0;
    for (size_t i = 0; i < fake->FrameCount(); ++i) {
      FrameHeader h;
      auto bytes = fake->Frame(i);
      if (DecodeAndValidateFrame(bytes.data(), bytes.size(), &h) == DecodeStatus::kOk &&
          h.type == type)
        ++count;
    }
    return count;
  }
  std::vector<uint8_t> Request(MessageType type, uint32_t target,
                               uint32_t run = 0) {
    for (size_t i = 0; i < fake->FrameCount(); ++i) {
      auto bytes = fake->Frame(i);
      FrameHeader h;
      if (DecodeAndValidateFrame(bytes.data(), bytes.size(), &h) == DecodeStatus::kOk &&
          h.type == type && h.target_id == target && h.run_id == run)
        return bytes;
    }
    return {};
  }
  void Refuse(MessageType type, uint32_t target, StatusCode code) {
    auto bytes = Request(type, target);
    FrameHeader h;
    if (DecodeAndValidateFrame(bytes.data(), bytes.size(), &h) != DecodeStatus::kOk)
      return;
    ErrorPayload error;
    error.status_code = code;
    fake->Inbound(BuildErrorFrame(h, error));
  }
  void Ready(v8host_worker w, v8host_runtime r = nullptr) {
    const uint32_t id = v8host_worker_get_id(w);
    fake->Inbound(EventFrame(MessageType::STARTUP_READY, id));
    fake->Inbound(EventFrame(MessageType::SECURITY_READY, id));
    if (r)
      fake->Inbound(EventFrame(MessageType::RUNTIME_READY, v8host_runtime_get_id(r)));
    Drain();
    record.events.clear();
  }
  void RuntimeFailed(v8host_runtime r) {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
    h.conn_id = 1;
    h.target_id = v8host_runtime_get_id(r);
    fake->Inbound(BuildRuntimeClosedFrame(h, RuntimeClosedPayload{}));
  }
  void Relay(v8host_runtime r, v8host_run run, const void* data, size_t size) {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
    h.conn_id = 1;
    h.target_id = v8host_runtime_get_id(r);
    h.run_id = v8host_run_get_id(run);
    h.type = MessageType::RELAY_FROM_WORKER;
    fake->Inbound(BuildRelayFrame(h, v8host_message_kind_binary,
                                  static_cast<const uint8_t*>(data), size));
  }
};

WorkerSetup WorkerOptions(uint32_t max = 1, uint32_t flags = 0) {
  return [=](v8host_worker_config c) {
    auto status = v8host_worker_config_set_max_runtimes(c, max);
    if (status == v8host_status_ok)
      status = v8host_worker_config_set_flags(c, static_cast<v8host_worker_flags>(flags));
    return status;
  };
}

bool SharedCoreSessionParity(std::string* detail) {
  FakeTransport* fake = nullptr;
  auto session = Create(&fake);
  if (!session) return Fail(detail, "session open");
  Recorder first, second;
  auto table = Callbacks(&first);
  v8host_session_set_callbacks(session, &table);
  Drain();
  fake->Inbound(EventFrame(MessageType::STARTUP_READY, 1));
  table = Callbacks(&second);
  v8host_session_set_callbacks(session, &table);
  fake->Inbound(EventFrame(MessageType::SECURITY_READY, 1));
  v8host_run run = nullptr;
  auto input = BasicRun();
  const auto status = StartRun(session, &input, &run);
  if (status == v8host_status_ok) {
    fake->Inbound(AckFor(fake->Frame(fake->FrameCount() - 1)));
    fake->Inbound(ResultFrame(1, 1));
    fake->Inbound(ResultFrame(1, 1));
  }
  Drain();
  const bool ok = status == v8host_status_ok &&
      first.states == std::vector<int32_t>{v8host_session_state_worker_startup_ready} &&
      second.states == std::vector<int32_t>{v8host_session_state_worker_security_ready} &&
      second.events == std::vector<int32_t>{v8host_run_event_started,
                                            v8host_run_event_completed} &&
      first.callback_thread == g_initializing_thread &&
      second.callback_thread == g_initializing_thread;
  v8host_session_close(session);
  Drain();
  return ok || Fail(detail, "session snapshots, FIFO, terminal CAS or callback thread changed");
}

bool SharedCoreLifetime(std::string* detail) {
  using v8host::client::SessionDispatch;
  const int baseline = SessionDispatch::live_count();
  for (char close_kind : {'w', 'r', 'b'}) {
    TopologyFixture f;
    v8host_worker w = nullptr;
    v8host_runtime r = nullptr;
    if (!f.Open() || f.Worker(&w) != v8host_status_ok ||
        f.Runtime(w, &r) != v8host_status_ok)
      return Fail(detail, "graph open");
    if (close_kind == 'w') {
      f.fake->Inbound(EventFrame(MessageType::STARTUP_READY, v8host_worker_get_id(w)));
      f.fake->Inbound(EventFrame(MessageType::SECURITY_READY, v8host_worker_get_id(w)));
    }
    if (close_kind == 'b')
      f.fake->delegate_->OnConnected(1, kWireVersionMajor, kWireVersionMinor);
    const auto runtime_id = v8host_runtime_get_id(r);
    f.fake->Inbound(EventFrame(MessageType::RUNTIME_READY, runtime_id));
    f.fake->Inbound(EventFrame(MessageType::RUNTIME_READY, runtime_id));
    f.record.during = [&](char kind, void* handle) {
      if (kind == close_kind) {
        // Reenter getters and close while the event owns the current reference.
        (void)v8host_worker_get_id(w);
        if (kind == 'w' && handle == w) f.Close(w);
        if (kind == 'r' && handle == r) f.Close(r);
        if (kind == 'b' && handle == f.broker) {
          v8host_broker_close(f.broker);
          f.broker = nullptr;
        }
      }
    };
    Drain();
    if (f.record.events.size() != 1 || f.record.wrong_thread)
      return Fail(detail, "callback close failed to suppress descendants or deadlocked");
    f.record.during = {};
    if (close_kind != 'r') {
      v8host_run out = reinterpret_cast<v8host_run>(1);
      if (f.Run(r, &out) != v8host_status_invalid_state || out)
        return Fail(detail, "retained child accepted a run after callback parent close");
    }
  }
  return SessionDispatch::live_count() == baseline ||
      Fail(detail, "broker dispatch reference survived close and drain");
}

bool PrivateIdRouting(std::string* detail) {
  TopologyFixture f;
  v8host_worker w1 = nullptr, w2 = nullptr;
  v8host_runtime r1 = nullptr, r2 = nullptr, r3 = nullptr;
  if (!f.Open(false) || f.Worker(&w1, WorkerOptions(2)) != v8host_status_ok ||
      f.Runtime(w1, &r1) != v8host_status_ok ||
      f.Runtime(w1, &r2) != v8host_status_ok ||
      f.Worker(&w2) != v8host_status_ok)
    return Fail(detail, "preconnect graph");
  if (f.fake->FrameCount() != 0 || v8host_worker_get_process_id(w1) != 0 ||
      v8host_worker_get_id(w1) != 1 || v8host_runtime_get_id(r1) != 2 ||
      v8host_runtime_get_id(r2) != 3 || v8host_worker_get_id(w2) != 4)
    return Fail(detail, "preconnect send, PID or common target namespace");
  Drain();
  if (f.Count(MessageType::CREATE_WORKER) != 2 ||
      f.Count(MessageType::CREATE_RUNTIME) != 2)
    return Fail(detail, "preconnect FIFO was not flushed once");
  for (size_t i = 0; i != 4; ++i) {
    FrameHeader item;
    const auto frame = f.fake->Frame(i);
    if (DecodeAndValidateFrame(frame.data(), frame.size(), &item) != DecodeStatus::kOk ||
        item.target_id != i + 1)
      return Fail(detail, "preconnect target FIFO changed order");
  }
  FrameHeader h;
  const uint8_t* body = nullptr;
  size_t size = 0;
  auto bytes = f.Request(MessageType::CREATE_RUNTIME, 3);
  CreateRuntimePayload payload;
  if (!DecodeFrame(bytes, &h, &body, &size) ||
      !DecodeCreateRuntimePayload(body, size, &payload) ||
      payload.worker_id != 1 || payload.snapshot_index != -1)
    return Fail(detail, "runtime parent routing/default snapshot");
  f.Ready(w1, r1);
  if (v8host_worker_get_process_id(w1) != 4321)
    return Fail(detail, "startup PID");
  f.Close(r1);
  if (f.Runtime(w1, &r3) != v8host_status_ok || v8host_runtime_get_id(r3) != 5)
    return Fail(detail, "target ID reused after close");
  f.fake->Inbound(EventFrame(MessageType::RUNTIME_READY, 2));
  Drain();
  if (!f.record.events.empty()) return Fail(detail, "late frame for retired ID was routed");
  FakeTransport* session_fake = nullptr;
  auto session = Create(&session_fake);
  Drain();
  auto session_bytes = session_fake->Frame(0);
  const bool independent = session && session_fake != f.fake &&
      DecodeFrame(session_bytes, &h, &body, &size) &&
      h.type == MessageType::CREATE_SESSION && h.target_id == 1;
  v8host_session_close(session);
  if (!independent) return Fail(detail, "session connection/target namespace changed");
  f.fake->Inbound(EventFrame(MessageType::WORKER_EXIT, 1));
  Drain();
  if (v8host_worker_get_process_id(w1) != 0)
    return Fail(detail, "worker PID remained valid after closed callback");
  return !f.record.wrong_thread || Fail(detail, "wrong callback thread");
}

bool PrivateRunRouting(std::string* detail) {
  TopologyFixture f;
  v8host_worker w = nullptr;
  v8host_runtime a = nullptr, b = nullptr;
  v8host_run ra = nullptr, rb = nullptr;
  if (!f.Open() || f.Worker(&w, WorkerOptions(2)) != v8host_status_ok ||
      f.Runtime(w, &a) != v8host_status_ok || f.Runtime(w, &b) != v8host_status_ok ||
      f.Run(a, &ra) != v8host_status_ok || f.Run(b, &rb) != v8host_status_ok)
    return Fail(detail, "two-runtime runs");
  if (v8host_run_get_id(ra) != 1 || v8host_run_get_id(rb) != 1 ||
      f.Request(MessageType::START_RUN, v8host_runtime_get_id(a), 1).empty() ||
      f.Request(MessageType::START_RUN, v8host_runtime_get_id(b), 1).empty() ||
      !f.record.events.empty())
    return Fail(detail, "pre-ready run routing or premature callback");
  // The broker holds these requests until ready; an open/start is only admission.
  f.Ready(w, a);
  f.fake->Inbound(EventFrame(MessageType::RUNTIME_READY, v8host_runtime_get_id(b)));
  f.fake->Inbound(AckFor(f.Request(MessageType::START_RUN, v8host_runtime_get_id(a), 1)));
  f.fake->Inbound(AckFor(f.Request(MessageType::START_RUN, v8host_runtime_get_id(b), 1)));
  f.Relay(b, rb, "b", 1);
  f.Relay(a, ra, "a", 1);
  if (v8host_run_cancel(ra) != v8host_status_ok ||
      v8host_run_cancel(rb) != v8host_status_ok ||
      f.Request(MessageType::CANCEL_RUN, v8host_runtime_get_id(a), 1).empty() ||
      f.Request(MessageType::CANCEL_RUN, v8host_runtime_get_id(b), 1).empty())
    return Fail(detail, "two outstanding cancels lost target correlation");
  f.fake->Inbound(ResultFrame(v8host_runtime_get_id(a), 1, ResultDisposition::kCancelled));
  f.fake->Inbound(ResultFrame(v8host_runtime_get_id(b), 1));
  f.fake->Inbound(ResultFrame(v8host_runtime_get_id(a), 1));
  f.Relay(a, ra, "late", 4);
  Drain();
  const auto& events = f.record.events;
  if (events.size() != 7 || events[1].handle != ra || events[2].handle != rb ||
      events[3].handle != rb || events[3].data != "b" ||
      events[4].handle != ra || events[4].data != "a" ||
      events[5].code != v8host_run_event_cancelled || events[5].handle != ra ||
      events[6].code != v8host_run_event_completed || events[6].handle != rb)
    return Fail(detail, "cross-runtime relay/run routing or duplicate terminal");
  const size_t sent = f.fake->FrameCount();
  f.fake->Disconnect();
  Drain();
  return (f.fake->FrameCount() == sent && !f.record.wrong_thread) ||
      Fail(detail, "disconnect replayed traffic or callback thread changed");
}

bool CloseOrderAndFlags(std::string* detail) {
  for (uint32_t flags : {0u, static_cast<uint32_t>(v8host_worker_flags_single_use),
                         static_cast<uint32_t>(v8host_worker_flags_close_when_empty)}) {
    TopologyFixture f;
    v8host_worker w = nullptr;
    v8host_runtime r = nullptr, next = reinterpret_cast<v8host_runtime>(1);
    if (!f.Open() || f.Worker(&w, WorkerOptions(1, flags)) != v8host_status_ok)
      return Fail(detail, "flag worker");
    if (f.Count(MessageType::CLOSE_WORKER) != 0)
      return Fail(detail, "initial-empty worker auto-closed");
    if (f.Runtime(w, &r) != v8host_status_ok ||
        f.Runtime(w, &next) != v8host_status_quota_exceeded || next)
      return Fail(detail, "default/max runtime admission boundary");
    const auto id = v8host_worker_get_id(w);
    f.fake->Inbound(EventFrame(MessageType::RUNTIME_READY, v8host_runtime_get_id(r)));
    f.Close(r);
    Drain();
    if (!f.record.events.empty())
      return Fail(detail, "runtime close did not suppress queued callback");
    const auto status = f.Runtime(w, &next);
    const auto expected = flags == 0 ? v8host_status_ok :
        flags == v8host_worker_flags_single_use ? v8host_status_quota_exceeded :
                                                 v8host_status_invalid_state;
    if (status != expected || (status != v8host_status_ok && next))
      return Fail(detail, "default reuse/single-use/close-when-empty admission");
    if (flags == v8host_worker_flags_close_when_empty) {
      auto close = f.Request(MessageType::CLOSE_WORKER, id);
      if (close.empty() || f.Count(MessageType::CLOSE_WORKER) != 1)
        return Fail(detail, "last close did not send exactly one CLOSE_WORKER");
      f.fake->Inbound(AckFor(close));
      Drain();
      if (f.record.events.size() != 1 ||
          f.record.events[0].code != v8host_worker_state_closed ||
          f.record.events[0].status != v8host_status_ok)
        return Fail(detail, "close-when-empty ACK callback");
    }
  }
  // Every admission-release source has the same 1 -> 0 behavior.
  for (int source = 0; source != 3; ++source) {
    TopologyFixture f;
    v8host_worker w = nullptr;
    v8host_runtime r = nullptr, out = nullptr;
    if (!f.Open() ||
        f.Worker(&w, WorkerOptions(1, v8host_worker_flags_close_when_empty)) != v8host_status_ok ||
        f.Runtime(w, &r) != v8host_status_ok)
      return Fail(detail, "admission-release graph");
    const auto id = v8host_worker_get_id(w);
    if (source == 0) f.RuntimeFailed(r);
    if (source == 1) f.Refuse(MessageType::CREATE_RUNTIME, v8host_runtime_get_id(r), StatusCode::ERROR_QUOTA);
    if (source == 2) {
      f.Ready(w, r);
      v8host_run run = nullptr;
      if (f.Run(r, &run) != v8host_status_ok) return Fail(detail, "backpressure run");
      f.fake->Inbound(AckFor(f.Request(
          MessageType::START_RUN, v8host_runtime_get_id(r), v8host_run_get_id(run))));
      for (int i = 0; i != 256; ++i)
        f.Relay(r, run, "x", 1);
    }
    f.Close(r);  // must not decrement a second time
    if (f.Count(MessageType::CLOSE_WORKER) != 1 ||
        f.Runtime(w, &out) != v8host_status_invalid_state || out)
      return Fail(detail, "failure/refusal/backpressure did not release admission exactly once");
    f.fake->Inbound(AckFor(f.Request(MessageType::CLOSE_WORKER, id)));
  }
  for (int order = 0; order != 6; ++order) {
    TopologyFixture f;
    v8host_worker w = nullptr;
    v8host_runtime r = nullptr;
    if (!f.Open() || f.Worker(&w) != v8host_status_ok ||
        f.Runtime(w, &r) != v8host_status_ok)
      return Fail(detail, "close-order graph");
    f.fake->Inbound(EventFrame(MessageType::RUNTIME_READY, v8host_runtime_get_id(r)));
    if (order == 0 || order == 3) {
      f.Close(r);
      if (order == 3) {
        v8host_broker_close(f.broker);
        f.broker = nullptr;
      }
      f.Close(w);
    }
    if (order == 1 || order == 4) {
      f.Close(w);
      const size_t sent = f.fake->FrameCount();
      if (v8host_runtime_get_id(r) != 0)
        return Fail(detail, "child ID remained valid after worker close");
      if (order == 4) {
        v8host_broker_close(f.broker);
        f.broker = nullptr;
      }
      f.Close(r);
      if (f.fake->FrameCount() != sent) return Fail(detail, "child close sent after worker close");
    }
    if (order == 2 || order == 5) {
      v8host_broker_close(f.broker);
      f.broker = nullptr;
      const size_t sent = f.fake->FrameCount();
      v8host_runtime out = reinterpret_cast<v8host_runtime>(1);
      if (v8host_worker_get_id(w) != 0 || v8host_runtime_get_id(r) != 0 ||
          f.Runtime(w, &out) != v8host_status_invalid_state || out)
        return Fail(detail, "broker-close child remained active");
      if (order == 2) { f.Close(w); f.Close(r); }
      else { f.Close(r); f.Close(w); }
      if (f.fake->FrameCount() != sent) return Fail(detail, "child close sent after broker close");
    }
    Drain();
    if (!f.record.events.empty()) return Fail(detail, "parent close leaked queued callbacks");
  }
  return true;
}

bool MandatoryCloseRetry(std::string* detail) {
  for (unsigned source = 0; source < 5; ++source) {
    TopologyFixture f;
    v8host_worker w = nullptr;
    v8host_runtime r = nullptr;
    const uint32_t flags = source == 1 ? 0 : v8host_worker_flags_close_when_empty;
    if (!f.Open() || f.Worker(&w, WorkerOptions(1, flags)) != v8host_status_ok ||
        f.Runtime(w, &r) != v8host_status_ok) return Fail(detail, "close-retry graph");
    const uint32_t worker_id = v8host_worker_get_id(w), runtime_id = v8host_runtime_get_id(r);
    v8host_run run = nullptr;
    if (source == 3) {
      f.Ready(w, r);
      if (f.Run(r, &run) != v8host_status_ok) return false;
      f.fake->Inbound(AckFor(f.Request(MessageType::START_RUN, runtime_id, v8host_run_get_id(run))));
    }
    f.fake->close_failures = 2;
    if (source == 0) f.Close(r);
    if (source == 1) f.Close(w);
    if (source == 2) f.RuntimeFailed(r);
    if (source == 3)
      for (unsigned i = 0; i < 256; ++i) f.Relay(r, run, "x", 1);
    if (source == 4) f.Refuse(MessageType::CREATE_RUNTIME, runtime_id, StatusCode::ERROR_QUOTA);
    if (f.Count(MessageType::CLOSE_WORKER) || f.Count(MessageType::CLOSE_RUNTIME) || !f.fake->wakes)
      return Fail(detail, "failed close was sent or did not wake pump");
    f.fake->Pump();
    if (f.Count(MessageType::CLOSE_WORKER) || f.Count(MessageType::CLOSE_RUNTIME) || f.fake->closed)
      return Fail(detail, "transient failure did not remain pending");
    f.fake->Pump();
    const bool runtime_close = source == 0 || source == 3;
    if (f.Count(MessageType::CLOSE_WORKER) != 1 ||
        f.Count(MessageType::CLOSE_RUNTIME) != (runtime_close ? 1u : 0u) || f.fake->closed)
      return Fail(detail, "pump did not retry each mandatory close exactly once");
    auto worker_close = f.Request(MessageType::CLOSE_WORKER, worker_id);
    if (runtime_close) {
      auto close = f.Request(MessageType::CLOSE_RUNTIME, runtime_id);
      if (close.empty()) return Fail(detail, "released runtime target lost");
      f.fake->Inbound(AckFor(close));
    }
    if (worker_close.empty()) return Fail(detail, "released worker target lost");
    f.fake->Inbound(AckFor(worker_close));
    f.fake->Pump();
    if (f.Count(MessageType::CLOSE_WORKER) != 1 ||
        f.Count(MessageType::CLOSE_RUNTIME) != (runtime_close ? 1u : 0u))
      return Fail(detail, "ACK or later pump replayed a close");
  }
  return true;
}

bool MandatoryClosePendingLimit(std::string* detail) {
  TopologyFixture f;
  if (!f.Open()) return false;
  v8host_worker w = nullptr;
  v8host_runtime first = nullptr;
  for (uint32_t i = 0; i < kMaxControlQueueRequests; ++i) {
    if (i % 16 == 0) {
      if (f.Worker(&w, WorkerOptions(16)) != v8host_status_ok) return false;
    } else {
      v8host_runtime r = nullptr;
      if (f.Runtime(w, &r) != v8host_status_ok) return false;
      if (!first) first = r;
    }
  }
  const uint32_t id = v8host_runtime_get_id(first);
  f.Close(first);
  f.fake->Pump();
  if (f.Count(MessageType::CLOSE_RUNTIME) || f.fake->closed)
    return Fail(detail, "pending-table saturation dropped close or disconnected");
  f.fake->Inbound(AckFor(f.Request(MessageType::CREATE_RUNTIME, id)));
  f.fake->Pump();
  if (f.Count(MessageType::CLOSE_RUNTIME) != 1 || f.fake->closed)
    return Fail(detail, "returned pending-table credit did not release close");
  return true;
}

bool MandatoryCloseBufferBound(std::string* detail) {
  TopologyFixture f;
  if (!f.Open()) return false;
  f.fake->close_failures = UINT32_MAX;
  for (uint32_t i = 0; i <= kMaxControlQueueRequests; ++i) {
    v8host_worker w = nullptr;
    if (f.Worker(&w) != v8host_status_ok) return false;
    f.fake->Inbound(AckFor(f.Request(MessageType::CREATE_WORKER, v8host_worker_get_id(w))));
    f.Close(w);
  }
  f.fake->Pump();
  Drain();
  if (!f.fake->closed || f.record.events.empty() ||
      f.record.events.back().code != v8host_broker_state_disconnected)
    return Fail(detail, "overflowing bounded mandatory-close storage did not tear down");
  return true;
}

bool MandatoryCloseFailure(std::string* detail) {
  for (v8host_status failure : {v8host_status_connect_failed, v8host_status_protocol_error,
                                v8host_status_no_memory, v8host_status_quota_exceeded}) {
    for (bool retry_first : {false, true}) {
      TopologyFixture f;
      v8host_worker w = nullptr;
      v8host_runtime r = nullptr;
      if (!f.Open() || f.Worker(&w) != v8host_status_ok ||
          f.Runtime(w, &r) != v8host_status_ok) return false;
      if (failure == v8host_status_quota_exceeded && !retry_first)
        v8host::client::SetBrokerRequestIdForTesting(f.broker, UINT32_MAX);
      f.fake->close_failures = 1;
      f.fake->close_failure_status = retry_first ? v8host_status_quota_exceeded : failure;
      f.Close(r);
      if (retry_first) {
        f.fake->close_failures = 1;
        f.fake->close_failure_status = failure;
        if (failure == v8host_status_quota_exceeded)
          v8host::client::SetBrokerRequestIdForTesting(f.broker, UINT32_MAX);
      }
      f.fake->Pump();
      Drain();
      if (!f.fake->closed || f.record.events.empty() ||
          f.record.events.back().kind != 'b' ||
          f.record.events.back().code != v8host_broker_state_disconnected ||
          f.record.events.back().status != v8host_status_broker_lost)
        return Fail(detail, "permanent close failure did not tear down connection");
      const auto count = f.fake->FrameCount();
      f.fake->Pump();
      if (f.fake->FrameCount() != count) return Fail(detail, "failed connection replayed close");
    }
  }
  return true;
}

bool LossFanoutOrder(std::string* detail) {
  for (int cause = 0; cause != 7; ++cause) {
    TopologyFixture f;
    v8host_worker w1 = nullptr, w2 = nullptr;
    v8host_runtime a = nullptr, b = nullptr, sibling = nullptr;
    v8host_run ra = nullptr, rb = nullptr, rs = nullptr;
    if (!f.Open(cause != 4) ||
        f.Worker(&w1, WorkerOptions(2, v8host_worker_flags_close_when_empty)) != v8host_status_ok ||
        f.Runtime(w1, &a) != v8host_status_ok || f.Runtime(w1, &b) != v8host_status_ok ||
        f.Worker(&w2) != v8host_status_ok || f.Runtime(w2, &sibling) != v8host_status_ok ||
        f.Run(a, &ra) != v8host_status_ok || f.Run(b, &rb) != v8host_status_ok ||
        f.Run(sibling, &rs) != v8host_status_ok)
      return Fail(detail, "fan-out graph");
    const uint32_t wa = v8host_worker_get_id(w1);
    const size_t sent = f.fake->FrameCount();
    auto expected = v8host_status_worker_exited;
    if (cause == 0) {
      f.fake->Inbound(EventFrame(MessageType::WORKER_EXIT, wa));
      f.fake->Inbound(EventFrame(MessageType::WORKER_EXIT, wa));
    } else if (cause == 1) {
      expected = v8host_status_runtime_failed;
      f.RuntimeFailed(a); f.RuntimeFailed(a);
    } else if (cause == 2) {
      expected = v8host_status_quota_exceeded;
      f.Refuse(MessageType::CREATE_WORKER, wa, StatusCode::ERROR_QUOTA);
    } else if (cause == 3) {
      expected = v8host_status_protocol_error;
      f.Refuse(MessageType::CREATE_RUNTIME, v8host_runtime_get_id(a), StatusCode::ERROR_BAD_STATE);
    } else {
      const auto reason = cause == 4 ? TransportDisconnect::kConnectFailed :
                          cause == 5 ? TransportDisconnect::kProtocolError :
                                       TransportDisconnect::kBrokerLost;
      expected = cause == 4 ? v8host_status_connect_failed :
                 cause == 5 ? v8host_status_protocol_error : v8host_status_broker_lost;
      f.fake->Disconnect(reason); f.fake->Disconnect(reason);
    }
    Drain();
    const auto& e = f.record.events;
    if (cause == 1 || cause == 3) {
      if (e.size() != 2 || e[0].handle != ra || e[0].kind != 'u' ||
          e[0].code != v8host_run_event_failed || e[1].handle != a || e[1].kind != 'r')
        return Fail(detail, "runtime failure/refusal order or duplicate callback");
      v8host_run next = nullptr;
      if (f.Run(b, &next) != v8host_status_ok) return Fail(detail, "runtime failure closed sibling");
    } else if (cause < 4) {
      if (e.size() != 5 || e[0].handle != ra || e[1].handle != rb ||
          e[2].handle != a || e[3].handle != b || e[4].handle != w1 ||
          e[0].code != (cause == 0 ? v8host_run_event_worker_exited : v8host_run_event_failed))
        return Fail(detail, "worker exit/refused-create fan-out order");
      v8host_run next = nullptr;
      if (f.Run(sibling, &next) != v8host_status_ok) return Fail(detail, "worker failure closed another worker");
    } else {
      if (e.size() != 9 || e[0].handle != ra || e[1].handle != rb || e[2].handle != rs ||
          e[3].handle != a || e[4].handle != b || e[5].handle != sibling ||
          e[6].handle != w1 || e[7].handle != w2 || e[8].handle != f.broker ||
          e[8].code != v8host_broker_state_disconnected)
        return Fail(detail, "disconnect run/runtime/worker/broker order");
      for (size_t i = 0; i != 3; ++i)
        if (e[i].code != v8host_run_event_failed)
          return Fail(detail, "disconnect did not report run_event_failed");
      if (f.fake->FrameCount() != sent) return Fail(detail, "disconnect replayed requests");
    }
    for (const auto& event : e)
      if (event.status != (cause == 0 && event.kind == 'u' ? v8host_status_run_terminal : expected))
        return Fail(detail, "fan-out status mapping");
    if (f.Count(MessageType::CLOSE_WORKER) != 0 || f.record.wrong_thread)
      return Fail(detail, "loss fan-out triggered close-when-empty or wrong-thread callback");
  }
  return true;
}

bool CallbackCapBoundaries(std::string* detail) {
  // Include runtime state events in the exact per-runtime limit.
  for (int count : {256, 257}) {
    TopologyFixture f;
    v8host_worker w = nullptr;
    v8host_runtime a = nullptr, b = nullptr;
    v8host_run run = nullptr;
    if (!f.Open() || f.Worker(&w, WorkerOptions(2)) != v8host_status_ok ||
        f.Runtime(w, &a) != v8host_status_ok || f.Runtime(w, &b) != v8host_status_ok ||
        f.Run(a, &run) != v8host_status_ok)
      return Fail(detail, "callback-cap graph");
    f.Ready(w);
    f.fake->Inbound(EventFrame(MessageType::RUNTIME_READY, v8host_runtime_get_id(b)));
    f.fake->Inbound(EventFrame(MessageType::RUNTIME_READY, v8host_runtime_get_id(a)));
    f.fake->Inbound(AckFor(f.Request(MessageType::START_RUN, v8host_runtime_get_id(a), 1)));
    for (int i = 2; i != count; ++i)
      f.Relay(a, run, "x", 1);
    Drain();
    if (count == 256) {
      if (f.record.events.size() != 257 || f.Count(MessageType::CLOSE_RUNTIME) != 0)
        return Fail(detail, "256 callbacks overflowed early");
      for (const auto& e : f.record.events)
        if (e.status != v8host_status_ok)
          return Fail(detail, "callback boundary delivery");
    } else {
      if (f.record.events.size() != 2 || f.record.events[0].handle != b ||
          f.record.events[1].handle != a ||
          f.record.events[1].code != v8host_runtime_state_closed ||
          f.record.events[1].status != v8host_status_callback_backpressure ||
          f.Count(MessageType::CLOSE_RUNTIME) != 1)
        return Fail(detail, "runtime overflow did not supersede locally");
      v8host_runtime next = nullptr;
      if (f.Runtime(w, &next) != v8host_status_ok)
        return Fail(detail, "overflow did not return runtime admission");
      f.Close(a);
      v8host_runtime extra = reinterpret_cast<v8host_runtime>(1);
      if (f.Runtime(w, &extra) != v8host_status_quota_exceeded || extra)
        return Fail(detail, "overflowed runtime released admission twice");
    }
  }
  // Sixteen full runtime queues exactly fill the broker cap.
  for (int count : {4096, 4097}) {
    TopologyFixture f;
    v8host_worker w = nullptr;
    if (!f.Open() || f.Worker(&w, WorkerOptions(16)) != v8host_status_ok)
      return Fail(detail, "broker-cap worker");
    f.Ready(w);
    for (int i = 0; i != 16; ++i) {
      v8host_runtime r = nullptr;
      v8host_run run = nullptr;
      if (f.Runtime(w, &r) != v8host_status_ok || f.Run(r, &run) != v8host_status_ok)
        return Fail(detail, "broker-cap runtime");
      const auto id = v8host_runtime_get_id(r);
      f.fake->Inbound(EventFrame(MessageType::RUNTIME_READY, id));
      f.fake->Inbound(AckFor(f.Request(MessageType::START_RUN, id, 1)));
      for (int j = 0; j != 254; ++j)
        f.Relay(r, run, "x", 1);
    }
    v8host_worker extra = nullptr;
    if (count == 4097) {
      if (f.Worker(&extra) != v8host_status_ok) return Fail(detail, "4097th worker event");
      f.fake->Inbound(EventFrame(MessageType::STARTUP_READY, v8host_worker_get_id(extra)));
    }
    Drain();
    if (count == 4096) {
      if (f.record.events.size() != 4096 || f.fake->closed.load())
        return Fail(detail, "4096 broker callbacks overflowed early");
    } else {
      if (f.record.events.size() != 1 || f.record.events[0].kind != 'b' ||
          f.record.events[0].code != v8host_broker_state_disconnected ||
          f.record.events[0].status != v8host_status_callback_backpressure ||
          !f.fake->closed.load())
        return Fail(detail, "broker overflow terminal/connection close");
      f.fake->Disconnect();
      f.fake->Inbound(EventFrame(MessageType::SECURITY_READY, v8host_worker_get_id(extra)));
      Drain();
      if (f.record.events.size() != 1) return Fail(detail, "broker overflow was not sticky");
    }
  }
  // Use bounded wire relays; copied bytes, not frame overhead, consume the cap.
  for (bool overflow : {false, true}) {
    TopologyFixture f;
    v8host_worker w = nullptr;
    if (!f.Open() || f.Worker(&w, WorkerOptions(16)) != v8host_status_ok)
      return Fail(detail, "copied-byte worker");
    f.Ready(w);
    const std::string bytes(32768, 'x');
    std::vector<std::pair<v8host_runtime, v8host_run>> traffic;
    for (int i = 0; i != 16; ++i) {
      v8host_runtime r = nullptr;
      v8host_run run = nullptr;
      if (f.Runtime(w, &r) != v8host_status_ok || f.Run(r, &run) != v8host_status_ok)
        return Fail(detail, "copied-byte runtime");
      const auto id = v8host_runtime_get_id(r);
      f.fake->Inbound(EventFrame(MessageType::RUNTIME_READY, id));
      f.fake->Inbound(AckFor(f.Request(MessageType::START_RUN, id, 1)));
      traffic.emplace_back(r, run);
    }
    Drain();
    f.record.events.clear();
    for (const auto& [r, run] : traffic)
      for (int j = 0; j != 32; ++j)
        f.Relay(r, run, bytes.data(), bytes.size());
    const auto [last, last_run] = traffic.back();
    if (overflow) f.Relay(last, last_run, "x", 1);
    Drain();
    if (overflow) {
      if (f.record.events.size() != 1 || f.record.events[0].kind != 'b' ||
          f.record.events[0].status != v8host_status_callback_backpressure)
        return Fail(detail, "16 MiB + 1 copied bytes did not overflow broker");
    } else {
      if (f.record.events.size() != 512 ||
          f.record.events.back().data != bytes || f.fake->closed.load())
        return Fail(detail, "exact 16 MiB copied bytes overflowed early");
      f.record.events.clear();
      f.Relay(last, last_run, bytes.data(), bytes.size());
      Drain();
      if (f.record.events.size() != 1 || f.record.events[0].kind != 'm')
        return Fail(detail, "drain did not return copied-byte credit");
    }
  }
  return true;
}

bool WorkerRuntimeBounds(std::string* detail) {
  TopologyFixture f;
  if (!f.Open()) return Fail(detail, "bounds broker");
  struct Check {
    const char* name;
    v8host_status expected;
    WorkerSetup configure;
  };
  const std::vector<Check> cases = {
      {"max zero", v8host_status_invalid_arg, WorkerOptions(0)},
      {"max one", v8host_status_ok, WorkerOptions(1)},
      {"max sixteen", v8host_status_ok, WorkerOptions(16)},
      {"max seventeen", v8host_status_quota_exceeded, WorkerOptions(17)},
      {"unknown flags", v8host_status_invalid_arg, WorkerOptions(1, 4)},
      {"tier", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_tier(c, static_cast<v8host_tier>(99)); }},
      {"integrity", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_integrity_level(c, static_cast<v8host_integrity_level>(99)); }},
      {"delayed integrity", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_delayed_integrity_level(c, static_cast<v8host_integrity_level>(99)); }},
      {"initial token", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_initial_token_level(c, static_cast<v8host_token_level>(99)); }},
      {"lockdown token", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_lockdown_token_level(c, static_cast<v8host_token_level>(99)); }},
      {"UTF8 null", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_engine_dll_utf8(c, nullptr, 1); }},
      {"UTF16 null", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_engine_dll_utf16(c, nullptr, V8HOST_AUTO_LENGTH); }},
      {"UTF8 invalid", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_engine_dll_utf8(c, "\xc0\x80", 2); }},
      {"UTF8 embedded null", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_engine_dll_utf8(c, "a\0b", 3); }},
      {"UTF16 surrogate", v8host_status_invalid_arg, [](auto c) {
        const v8host_char16 text[] = {0xd800, 0};
        return v8host_worker_config_set_engine_dll_utf16(c, text, 1); }},
      {"UTF16 embedded null", v8host_status_invalid_arg, [](auto c) {
        const v8host_char16 text[] = {'a', 0, 'b'};
        return v8host_worker_config_set_engine_dll_utf16(c, text, 3); }},
      {"UTF8 bare engine", v8host_status_ok, [](auto c) {
        return v8host_worker_config_set_engine_dll_utf8(c, "engine.dll-tail", 10); }},
      {"UTF16 bare engine", v8host_status_ok, [](auto c) {
        const v8host_char16 text[] = {'e', '.', 'd', 'l', 'l', 0};
        return v8host_worker_config_set_engine_dll_utf16(c, text, V8HOST_AUTO_LENGTH); }},
      {"engine path", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_engine_dll_utf8(c, "dir\\engine.dll", V8HOST_AUTO_LENGTH); }},
      {"engine drive", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_engine_dll_utf8(c, "C:engine.dll", V8HOST_AUTO_LENGTH); }},
      {"engine empty", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_engine_dll_utf8(c, "", 0); }},
      {"snapshot empty", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_add_startup_snapshot_path_utf8(c, nullptr, 0); }},
      {"snapshot absolute", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_add_startup_snapshot_path_utf8(c, "\\snap.bin", V8HOST_AUTO_LENGTH); }},
      {"snapshot slash", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_add_startup_snapshot_path_utf8(c, "/snap.bin", V8HOST_AUTO_LENGTH); }},
      {"snapshot drive", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_add_startup_snapshot_path_utf8(c, "C:\\snap.bin", V8HOST_AUTO_LENGTH); }},
      {"snapshot relative", v8host_status_ok, [](auto c) {
        return v8host_worker_config_add_startup_snapshot_path_utf8(c, "data\\snap.bin", V8HOST_AUTO_LENGTH); }},
      {"snapshot UTF16", v8host_status_ok, [](auto c) {
        const v8host_char16 text[] = {'s', '.', 'b', 'i', 'n', 0};
        return v8host_worker_config_add_startup_snapshot_path_utf16(c, text, V8HOST_AUTO_LENGTH); }},
      {"snapshot UTF16 null", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_add_startup_snapshot_path_utf16(c, nullptr, 1); }},
      {"snapshot UTF8 invalid", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_add_startup_snapshot_path_utf8(c, "\xed\xa0\x80", 3); }},
      {"snapshot UTF16 surrogate", v8host_status_invalid_arg, [](auto c) {
        const v8host_char16 text[] = {0xdc00};
        return v8host_worker_config_add_startup_snapshot_path_utf16(c, text, 1); }},
      {"profile UTF8 null", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_profile_name_utf8(c, nullptr, 1); }},
      {"profile UTF16 null", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_profile_name_utf16(c, nullptr, 1); }},
      {"capability UTF8 null", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_add_capability_utf8(c, nullptr, 0); }},
      {"capability UTF16 null", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_add_capability_utf16(c, nullptr, 0); }},
      {"file UTF8 null", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_add_file_rule_utf8(c, nullptr, 1, true); }},
      {"file UTF16 null", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_add_file_rule_utf16(c, nullptr, 1, true); }},
      {"profile without container", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_profile_name_utf8(c, "profile", 7); }},
      {"container without profile", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_use_app_container(c, true); }},
      {"LPAC without container", v8host_status_invalid_arg, [](auto c) {
        return v8host_worker_config_set_low_privilege_app_container(c, true); }},
  };
  for (const auto& test : cases) {
    v8host_worker out = reinterpret_cast<v8host_worker>(1);
    const size_t before = f.fake->FrameCount();
    auto status = f.Worker(&out, test.configure);
    if (status != test.expected || (status != v8host_status_ok &&
                                   (out || f.fake->FrameCount() != before))) {
      *detail = std::string("worker builder: ") + test.name;
      return false;
    }
    if (status == v8host_status_ok) f.Close(out);
  }
  // The mirrored policy setters copy both encodings into the worker's payload.
  v8host_worker copied = nullptr;
  if (f.Worker(&copied, [](auto c) {
        v8host_status s = v8host_worker_config_set_tier(c, v8host_tier_untrusted);
        if (s == v8host_status_ok) s = v8host_worker_config_set_use_app_container(c, true);
        if (s == v8host_status_ok) s = v8host_worker_config_set_low_privilege_app_container(c, true);
        if (s == v8host_status_ok) s = v8host_worker_config_set_integrity_level(
            c, static_cast<v8host_integrity_level>(0));
        if (s == v8host_status_ok) s = v8host_worker_config_set_delayed_integrity_level(
            c, static_cast<v8host_integrity_level>(0));
        if (s == v8host_status_ok) s = v8host_worker_config_set_initial_token_level(
            c, static_cast<v8host_token_level>(0));
        if (s == v8host_status_ok) s = v8host_worker_config_set_lockdown_token_level(
            c, static_cast<v8host_token_level>(0));
        if (s == v8host_status_ok) s = v8host_worker_config_set_profile_name_utf8(c, "old", 3);
        if (s == v8host_status_ok) s = v8host_worker_config_set_profile_name_utf16(
            c, v8host::test::AsChar16(L"profile"), V8HOST_AUTO_LENGTH);
        if (s == v8host_status_ok) s = v8host_worker_config_add_capability_utf8(c, "internetClient", 14);
        if (s == v8host_status_ok) s = v8host_worker_config_add_capability_utf16(
            c, v8host::test::AsChar16(L"privateNetworkClientServer"), V8HOST_AUTO_LENGTH);
        if (s == v8host_status_ok) s = v8host_worker_config_add_file_rule_utf8(c, "data\\*", 6, true);
        if (s == v8host_status_ok) s = v8host_worker_config_add_file_rule_utf16(
            c, v8host::test::AsChar16(L"other\\*"), V8HOST_AUTO_LENGTH, false);
        char engine[] = "custom.dll";
        if (s == v8host_status_ok) s = v8host_worker_config_set_engine_dll_utf8(c, engine, 10);
        std::memset(engine, 'x', 10);
        if (s == v8host_status_ok) s = v8host_worker_config_set_flags(
            c, static_cast<v8host_worker_flags>(
                v8host_worker_flags_close_when_empty | v8host_worker_flags_single_use));
        return s;
      }) != v8host_status_ok)
    return Fail(detail, "mirrored worker policy setters");
  auto encoded = f.Request(MessageType::CREATE_WORKER, v8host_worker_get_id(copied));
  FrameHeader header;
  const uint8_t* body = nullptr;
  size_t body_size = 0;
  CreateWorkerPayload copy;
  if (!DecodeFrame(encoded, &header, &body, &body_size) ||
      !DecodeCreateWorkerPayload(body, body_size, &copy) ||
      copy.engine_dll != "custom.dll" || copy.max_runtimes != 1 ||
      copy.flags != kWorkerFlagSingleUse || copy.session.broker_mode != kEndpointModeShared ||
      !copy.session.prohibit_dynamic_code || !copy.session.use_app_container ||
      !copy.session.low_privilege_app_container || copy.session.app_container_profile != "profile" ||
      copy.session.file_rules.size() != 2 || copy.session.file_rules[0].pattern != "data\\*" ||
      !copy.session.file_rules[0].readonly || copy.session.file_rules[1].pattern != "other\\*" ||
      copy.session.file_rules[1].readonly ||
      copy.session.capabilities != std::vector<std::string>{"internetClient", "privateNetworkClientServer"})
    return Fail(detail, "worker policy copy/defaults/ACG or client-only flag leaked to wire");
  f.Close(copied);
  for (unsigned count : {64u, 65u}) {
    for (bool capabilities : {false, true}) {
      v8host_worker out = reinterpret_cast<v8host_worker>(1);
      const auto status = f.Worker(&out, [=](auto c) {
        auto s = v8host_worker_config_set_use_app_container(c, true);
        if (s == v8host_status_ok) s = v8host_worker_config_set_profile_name_utf8(c, "p", 1);
        for (unsigned i = 0; s == v8host_status_ok && i != count; ++i)
          s = capabilities ? v8host_worker_config_add_capability_utf8(c, "internetClient", 14) :
                             v8host_worker_config_add_file_rule_utf8(c, "data\\*", 6, true);
        return s;
      });
      if (status != (count == 64 ? v8host_status_ok : v8host_status_quota_exceeded) ||
          (count == 65 && out))
        return Fail(detail, "worker mirrored 64/65 policy limits");
      if (out) f.Close(out);
    }
  }
  for (unsigned count : {8u, 9u}) {
    v8host_worker w = reinterpret_cast<v8host_worker>(1);
    auto status = f.Worker(&w, [=](auto c) {
      for (unsigned i = 0; i != count; ++i) {
        auto s = v8host_worker_config_add_startup_snapshot_path_utf8(c, "snap.bin", 8);
        if (s != v8host_status_ok) return s;
      }
      return v8host_status_ok;
    });
    if (status != (count == 8 ? v8host_status_ok : v8host_status_quota_exceeded) ||
        (count == 9 && w))
      return Fail(detail, "eight/nine snapshots");
    if (count == 8) {
      for (uint32_t index : {7u, 8u, UINT32_MAX}) {
        v8host_runtime r = reinterpret_cast<v8host_runtime>(1);
        auto s = f.Runtime(w, &r, [=](auto c) {
          return v8host_runtime_config_set_startup_snapshot_index(c, index);
        });
        if (s != (index == 7 ? v8host_status_ok : v8host_status_invalid_arg) ||
            (index != 7 && r))
          return Fail(detail, "snapshot index bounds");
        if (r) f.Close(r);
      }
      f.Close(w);
    }
  }
  for (size_t total : {4096u, 4097u}) {
    v8host_worker w = reinterpret_cast<v8host_worker>(1);
    const size_t sent = f.fake->FrameCount();
    auto status = f.Worker(&w, [=](auto c) {
      auto s = v8host_worker_config_set_tier(c, v8host_tier_untrusted);
      const std::string path(total - 39, 's');
      return s == v8host_status_ok
          ? v8host_worker_config_add_startup_snapshot_path_utf8(c, path.data(), path.size()) : s;
    });
    if (status != (total == 4096 ? v8host_status_ok : v8host_status_quota_exceeded) ||
        (total == 4097 && (w || f.fake->FrameCount() != sent)))
      return Fail(detail, "predictable 4096/4097 profile quota");
    if (w) f.Close(w);
  }
  v8host_worker capacity = nullptr;
  if (f.Worker(&capacity, WorkerOptions(16)) != v8host_status_ok)
    return Fail(detail, "sixteen-runtime worker");
  for (unsigned i = 0; i != 16; ++i) {
    v8host_runtime r = nullptr;
    if (f.Runtime(capacity, &r) != v8host_status_ok)
      return Fail(detail, "sixteen simultaneous runtime admissions");
  }
  v8host_runtime overflow_runtime = reinterpret_cast<v8host_runtime>(1);
  size_t capacity_sent = f.fake->FrameCount();
  if (f.Runtime(capacity, &overflow_runtime) != v8host_status_quota_exceeded ||
      overflow_runtime || f.fake->FrameCount() != capacity_sent)
    return Fail(detail, "seventeenth runtime marshalled");
  f.Close(capacity);
  // Worker and run quotas are synchronous and may not marshal the failed call.
  std::vector<v8host_worker> live;
  for (int i = 0; i != 32; ++i) {
    v8host_worker w = nullptr;
    if (f.Worker(&w) != v8host_status_ok) return Fail(detail, "32 workers");
    live.push_back(w);
  }
  v8host_worker extra = reinterpret_cast<v8host_worker>(1);
  size_t before = f.fake->FrameCount();
  if (f.Worker(&extra) != v8host_status_quota_exceeded || extra ||
      f.fake->FrameCount() != before) return Fail(detail, "33rd worker marshalled");
  v8host_runtime r = nullptr;
  if (f.Runtime(live[0], &r) != v8host_status_ok) return Fail(detail, "quota runtime");
  v8host_runtime default_extra = reinterpret_cast<v8host_runtime>(1);
  before = f.fake->FrameCount();
  if (f.Runtime(live[0], &default_extra) != v8host_status_quota_exceeded ||
      default_extra || f.fake->FrameCount() != before)
    return Fail(detail, "default max_runtimes was not one");
  v8host_run run = nullptr;
  for (int i = 0; i != 4; ++i)
    if (f.Run(r, &run) != v8host_status_ok) return Fail(detail, "four live runs");
  before = f.fake->FrameCount();
  run = reinterpret_cast<v8host_run>(1);
  if (f.Run(r, &run) != v8host_status_quota_exceeded || run ||
      f.fake->FrameCount() != before) return Fail(detail, "fifth run marshalled");
  f.fake->Inbound(ResultFrame(v8host_runtime_get_id(r), 1));
  if (f.Run(r, &run) != v8host_status_ok || v8host_run_get_id(run) != 5)
    return Fail(detail, "terminal run did not restore capacity/monotonic ID");
  const std::vector<RunSetup> overrides = {
      [](auto c) { return v8host_run_config_set_tier(c, v8host_tier_untrusted); },
      [](auto c) { return v8host_run_config_set_engine_dll_utf8(c, "e.dll", 5); },
      [](auto c) { return v8host_run_config_set_startup_snapshot_path_utf8(c, "s.bin", 5); },
  };
  for (const auto& configure : overrides) {
    before = f.fake->FrameCount();
    run = reinterpret_cast<v8host_run>(1);
    if (f.Run(r, &run, configure) != v8host_status_profile_already_bound || run ||
        f.fake->FrameCount() != before)
      return Fail(detail, "runtime profile override was not refused before send");
  }
  return true;
}

bool PoisonAndNullOutput(std::string* detail) {
  if (!g_preinitialize_broker_output_was_nulled)
    return Fail(detail, "pre-initialize broker failure did not null output");
  TopologyFixture f;
  if (!f.Open()) return Fail(detail, "poison broker");
  bool poison = true;
  std::function<v8host_status(v8host_broker_config)> bad_broker = [&](auto c) {
    poison &= v8host_broker_config_set_mode(c, static_cast<v8host_broker_mode>(99)) ==
        v8host_status_invalid_arg;
    poison &= v8host_broker_config_set_mode(c, v8host_broker_mode_shared) ==
        v8host_status_invalid_arg;
    return v8host_status_ok;
  };
  v8host_broker broker = reinterpret_cast<v8host_broker>(1);
  if (v8host_broker_open(ConfigureTopology<v8host_broker_config>, &bad_broker, &broker) !=
          v8host_status_invalid_arg || broker || !poison)
    return Fail(detail, "broker first-error poisoning/output");
  std::function<v8host_status(v8host_broker_config)> rejected_broker =
      [](auto) { return v8host_status_internal_error; };
  broker = reinterpret_cast<v8host_broker>(1);
  if (v8host_broker_open(ConfigureTopology<v8host_broker_config>, &rejected_broker, &broker) !=
          v8host_status_internal_error || broker)
    return Fail(detail, "broker callback failure/output");
  std::function<v8host_status(v8host_broker_config)> short_callbacks = [](auto c) {
    v8host_broker_callbacks table = V8HOST_BROKER_CALLBACKS_INIT;
    --table.struct_size;
    (void)v8host_broker_config_set_callbacks(c, &table);
    (void)v8host_broker_config_set_mode(c, v8host_broker_mode_shared);
    return v8host_status_ok;
  };
  broker = reinterpret_cast<v8host_broker>(1);
  if (v8host_broker_open(ConfigureTopology<v8host_broker_config>, &short_callbacks, &broker) !=
          v8host_status_invalid_struct_size || broker)
    return Fail(detail, "broker callback size poison/output");
  v8host_worker worker = reinterpret_cast<v8host_worker>(1);
  const size_t before = f.fake->FrameCount();
  if (f.Worker(&worker, [&](auto c) {
        poison &= v8host_worker_config_set_max_runtimes(c, 17) == v8host_status_quota_exceeded;
        poison &= v8host_worker_config_set_flags(c, static_cast<v8host_worker_flags>(4)) ==
            v8host_status_quota_exceeded;
        poison &= v8host_worker_config_set_max_runtimes(c, 1) == v8host_status_quota_exceeded;
        return v8host_status_ok;
      }) != v8host_status_quota_exceeded || worker || !poison ||
      f.fake->FrameCount() != before)
    return Fail(detail, "worker first-error poisoning marshalled or changed status");
  if (v8host_initialize(0) != v8host_status_version_mismatch ||
      v8host_initialize(V8HOST_ABI_VERSION + 1) != v8host_status_version_mismatch ||
      v8host_initialize(V8HOST_ABI_VERSION) != v8host_status_ok)
    return Fail(detail, "ABI version mismatch");
  if (v8host_worker_open(nullptr, nullptr, nullptr, &worker) != v8host_status_invalid_arg ||
      worker || v8host_broker_open(nullptr, nullptr, nullptr) != v8host_status_invalid_arg ||
      v8host_worker_open(f.broker, nullptr, nullptr, nullptr) != v8host_status_invalid_arg)
    return Fail(detail, "broker/worker null outputs");
  if (f.Worker(&worker) != v8host_status_ok) return Fail(detail, "valid worker");
  v8host_runtime runtime = reinterpret_cast<v8host_runtime>(1);
  if (v8host_runtime_open(nullptr, nullptr, nullptr, &runtime) != v8host_status_invalid_arg ||
      runtime || v8host_runtime_open(worker, nullptr, nullptr, nullptr) != v8host_status_invalid_arg)
    return Fail(detail, "runtime null outputs");
  if (f.Runtime(worker, &runtime, [](auto) { return v8host_status_no_memory; }) !=
          v8host_status_no_memory || runtime)
    return Fail(detail, "runtime configure error output");
  if (f.Runtime(worker, &runtime) != v8host_status_ok) return Fail(detail, "valid runtime");
  v8host_run run = reinterpret_cast<v8host_run>(1);
  if (v8host_runtime_start_run(nullptr, nullptr, nullptr, &run) != v8host_status_invalid_arg ||
      run || v8host_runtime_start_run(runtime, nullptr, nullptr, nullptr) != v8host_status_invalid_arg)
    return Fail(detail, "run null outputs");
  if (f.Run(runtime, &run, [](auto c) {
        (void)v8host_run_config_set_tier(c, static_cast<v8host_tier>(99));
        (void)v8host_run_config_set_payload(c, nullptr, 0);
        return v8host_status_ok;
      }) != v8host_status_invalid_arg || run)
    return Fail(detail, "runtime run poison/output");
  if (f.Run(runtime, &run, [](auto) { return v8host_status_no_memory; }) !=
          v8host_status_no_memory || run)
    return Fail(detail, "run configure failure/output");
  v8host_worker failed_worker = reinterpret_cast<v8host_worker>(1);
  if (f.Worker(&failed_worker, [](auto) { return v8host_status_internal_error; }) !=
          v8host_status_internal_error || failed_worker)
    return Fail(detail, "worker configure failure/output");
  if (v8host_broker_config_set_callbacks(nullptr, nullptr) != v8host_status_invalid_arg ||
      v8host_worker_config_set_flags(nullptr, v8host_worker_flags_none) != v8host_status_invalid_arg ||
      v8host_runtime_config_set_startup_snapshot_index(nullptr, 0) != v8host_status_invalid_arg)
    return Fail(detail, "NULL builders");
  f.Close(worker);
  if (f.Run(runtime, &run) != v8host_status_invalid_state || run)
    return Fail(detail, "closed-parent run output");
  f.Close(runtime);
  f.fake->Disconnect();
  if (f.Worker(&failed_worker) != v8host_status_invalid_state || failed_worker)
    return Fail(detail, "disconnected broker factory output");
  v8host_broker_close(f.broker);
  // Keep no raw broker reference after its close; child-retention cases above
  // exercise failure calls on still-owned child handles instead.
  f.broker = nullptr;
  for (int operation = 0; operation != 3; ++operation) {
    TopologyFixture rejected;
    v8host_worker w = nullptr;
    v8host_runtime r = nullptr;
    if (!rejected.Open() ||
        (operation >= 1 && rejected.Worker(&w) != v8host_status_ok) ||
        (operation >= 2 && rejected.Runtime(w, &r) != v8host_status_ok))
      return Fail(detail, "submission failure graph");
    rejected.fake->send_status = v8host_status_quota_exceeded;
    v8host_status status;
    if (operation == 0) {
      w = reinterpret_cast<v8host_worker>(1);
      status = rejected.Worker(&w);
      if (w) return Fail(detail, "worker submission failure leaked output");
    } else if (operation == 1) {
      r = reinterpret_cast<v8host_runtime>(1);
      status = rejected.Runtime(w, &r);
      if (r) return Fail(detail, "runtime submission failure leaked output");
    } else {
      v8host_run out = reinterpret_cast<v8host_run>(1);
      status = rejected.Run(r, &out);
      if (out) return Fail(detail, "run submission failure leaked output");
    }
    if (status != v8host_status_quota_exceeded)
      return Fail(detail, "submission failure status changed");
    rejected.fake->send_status = v8host_status_ok;
    if (operation == 0 && rejected.Worker(&w) != v8host_status_ok)
      return Fail(detail, "worker admission did not roll back");
    if (operation == 1 && rejected.Runtime(w, &r) != v8host_status_ok)
      return Fail(detail, "runtime admission did not roll back");
  }
  return true;
}

bool ConcurrentInitializeLifecycle(std::string* detail) {
  if (!g_preinitialize_output_was_nulled)
    return Fail(detail, "pre-initialize create did not null output");
  if (g_main_init_status != v8host_status_ok || g_racer_init_status != v8host_status_ok)
    return Fail(detail, "racing initialize did not remain idempotent");
  if (LifecycleThreadIdForTesting() != g_initializing_thread ||
      v8host::client::Dispatcher::Instance().callback_thread_id() !=
          g_initializing_thread) {
    return Fail(detail, "dispatcher/lifecycle thread binding differs");
  }
  const int baseline = v8host::client::SessionDispatch::live_count();
  v8host_session_s* session = Create();
  if (session == nullptr)
    return Fail(detail, "lifecycle create failed");
  v8host_session_close(session);
  if (!PumpUntil([&] {
        return LastSessionDeleteThreadIdForTesting() == g_initializing_thread &&
               v8host::client::SessionDispatch::live_count() == baseline;
      })) {
    return Fail(detail, "deferred destruction did not run on callback thread");
  }
  return true;
}

v8host_status V8HOST_CDECL FailSessionConfigure(void*, v8host_config) {
  return v8host_status_internal_error;
}

v8host_status V8HOST_CDECL FailRunConfigure(void*, v8host_run_config) {
  return v8host_status_no_memory;
}

// The builders replaced the versioned input structs, so v8host_callbacks is the
// only struct_size-checked input left. A configure callback's failure is
// returned unchanged, and a NULL run configure callback means all defaults.
bool ValidateStructSizes(std::string* detail) {
  // The consumer passes the ABI version it compiled against; the DLL serves
  // older consumers and rejects newer ones (and 0).
  if (v8host_initialize(0) != v8host_status_version_mismatch ||
      v8host_initialize(V8HOST_ABI_VERSION + 1) !=
          v8host_status_version_mismatch ||
      v8host_initialize(V8HOST_ABI_VERSION) != v8host_status_ok)
    return Fail(detail, "initialize ABI version");
  v8host_session out = reinterpret_cast<v8host_session>(1);
  if (v8host_session_open(&FailSessionConfigure, nullptr, &out) !=
          v8host_status_internal_error ||
      out != nullptr)
    return Fail(detail, "configure failure/output nulling");
  TestSessionConfig c = BasicConfig();
  if (OpenSession(&c, &out) != v8host_status_ok || out == nullptr)
    return Fail(detail, "create");
  v8host_run run = reinterpret_cast<v8host_run>(1);
  if (v8host_session_start_run(out, &FailRunConfigure, nullptr, &run) !=
          v8host_status_no_memory ||
      run != nullptr)
    return Fail(detail, "run configure failure/output nulling");
  if (v8host_session_start_run(out, nullptr, nullptr, &run) !=
          v8host_status_ok ||
      run == nullptr)
    return Fail(detail, "null run configure (defaults) rejected");
  v8host_callbacks cb = V8HOST_CALLBACKS_INIT;
  if (cb.struct_size != sizeof(v8host_callbacks) || cb.user_data != nullptr ||
      cb.on_session_state != nullptr || cb.on_broker_disconnect != nullptr)
    return Fail(detail, "V8HOST_CALLBACKS_INIT");
  cb.struct_size = sizeof(cb) - 1;
  if (v8host_session_set_callbacks(out, &cb) != v8host_status_invalid_struct_size)
    return Fail(detail, "callback struct_size");
  cb.struct_size = sizeof(cb) + 16;
  const bool ok = v8host_session_set_callbacks(out, &cb) == v8host_status_ok;
  v8host_session_close(out);
  Drain();
  return ok || Fail(detail, "larger callback struct rejected");
}

bool ValidatePairsAndPolicy(std::string* detail) {
  TestSessionConfig c = BasicConfig();
  if (OpenSession(&c, nullptr) != v8host_status_invalid_arg)
    return Fail(detail, "null output");
  TestFileRule rule{nullptr, true};
  c.file_rules = &rule;
  c.file_rule_count = 1;
  v8host_session out = reinterpret_cast<v8host_session>(1);
  if (OpenSession(&c, &out) != v8host_status_invalid_arg || out != nullptr)
    return Fail(detail, "null file pattern");
  const wchar_t* null_cap = nullptr;
  c = BasicConfig();
  c.use_app_container = true;
  c.app_container_profile_name = L"profile";
  c.capabilities = &null_cap;
  c.capability_count = 1;
  out = reinterpret_cast<v8host_session>(1);
  if (OpenSession(&c, &out) != v8host_status_invalid_arg || out != nullptr)
    return Fail(detail, "null capability");
  const wchar_t* empty_cap = L"";
  c.capabilities = &empty_cap;
  out = reinterpret_cast<v8host_session>(1);
  if (OpenSession(&c, &out) != v8host_status_invalid_arg || out != nullptr)
    return Fail(detail, "empty capability");
  const wchar_t* cap = L"cap";
  c = BasicConfig();
  c.app_container_profile_name = L"profile";
  out = reinterpret_cast<v8host_session>(1);
  if (OpenSession(&c, &out) != v8host_status_invalid_arg ||
      out != nullptr)
    return Fail(detail, "profile without AC");
  c = BasicConfig();
  c.capabilities = &cap;
  c.capability_count = 1;
  out = reinterpret_cast<v8host_session>(1);
  if (OpenSession(&c, &out) != v8host_status_invalid_arg ||
      out != nullptr)
    return Fail(detail, "capability without AC");
  c = BasicConfig();
  c.use_app_container = true;
  out = reinterpret_cast<v8host_session>(1);
  if (OpenSession(&c, &out) != v8host_status_invalid_arg ||
      out != nullptr)
    return Fail(detail, "AC without profile");
  c = BasicConfig();
  c.use_app_container = true;
  c.app_container_profile_name = L"";
  out = reinterpret_cast<v8host_session>(1);
  if (OpenSession(&c, &out) != v8host_status_invalid_arg || out != nullptr)
    return Fail(detail, "AC with empty profile");
  c = BasicConfig();
  c.low_privilege_app_container = true;
  if (OpenSession(&c, &out) != v8host_status_invalid_arg)
    return Fail(detail, "LPAC without AC");
  return true;
}

// One failing setter applied to a fresh session builder. The configure callback
// ignores the failure and returns ok, so open must still fail closed with the
// poison status; a later valid setter must return that same status.
struct SetterCase {
  const char* name;
  v8host_status expected;
  std::function<v8host_status(v8host_config)> apply;
  v8host_status first = v8host_status_ok;
  v8host_status after = v8host_status_ok;
};

v8host_status V8HOST_CDECL ApplySetterCase(void* user_data,
                                           v8host_config config) {
  auto* c = static_cast<SetterCase*>(user_data);
  v8host_config_set_broker_mode(config, v8host_broker_mode_shared);
  v8host_config_set_prohibit_dynamic_code(config, true);
  c->first = c->apply(config);
  c->after = v8host_config_set_tier(config, v8host_tier_untrusted);
  return v8host_status_ok;
}

struct RunSetterCase {
  const char* name;
  v8host_status expected;
  std::function<v8host_status(v8host_run_config)> apply;
  v8host_status first = v8host_status_ok;
  v8host_status after = v8host_status_ok;
};

v8host_status V8HOST_CDECL ApplyRunSetterCase(void* user_data,
                                              v8host_run_config config) {
  auto* c = static_cast<RunSetterCase*>(user_data);
  c->first = c->apply(config);
  c->after = v8host_run_config_set_tier(config, v8host_tier_untrusted);
  return v8host_status_ok;
}

v8host_status V8HOST_CDECL ConfigureUtf8Session(void*, v8host_config config) {
  v8host_status s = v8host_config_set_broker_mode(config, v8host_broker_mode_shared);
  if (s == v8host_status_ok)
    s = v8host_config_set_prohibit_dynamic_code(config, true);
  if (s == v8host_status_ok)
    s = v8host_config_set_integrity_level(config, v8host_integrity_level_low);
  if (s == v8host_status_ok) {
    s = v8host_config_set_delayed_integrity_level(
        config, v8host_integrity_level_untrusted);
  }
  if (s == v8host_status_ok) {
    s = v8host_config_add_file_rule_utf8(config, "C:\\a\\*", V8HOST_AUTO_LENGTH,
                                         true);
  }
  if (s == v8host_status_ok) {
    // An explicit length need not end at a NUL.
    s = v8host_config_add_file_rule_utf8(config, "C:\\b\\*tail", 6, false);
  }
  if (s == v8host_status_ok) {
    s = v8host_config_add_file_rule_utf8(config, "C:\\\xC3\xA9\\*",
                                         V8HOST_AUTO_LENGTH, false);
  }
  if (s == v8host_status_ok)
    s = v8host_config_set_use_app_container(config, true);
  if (s == v8host_status_ok) {
    s = v8host_config_set_profile_name_utf8(config, "profile",
                                            V8HOST_AUTO_LENGTH);
  }
  if (s == v8host_status_ok) {
    s = v8host_config_add_capability_utf8(config, "S-1-15-3-1xx", 10);
  }
  if (s == v8host_status_ok) {
    s = v8host_config_add_capability_utf16(config, u"S-1-15-3-2",
                                           V8HOST_AUTO_LENGTH);
  }
  return s;
}

v8host_status V8HOST_CDECL ConfigureUtf8Run(void*, v8host_run_config config) {
  v8host_status s =
      v8host_run_config_set_engine_dll_utf8(config, "engine.dllx", 10);
  if (s == v8host_status_ok) {
    s = v8host_run_config_set_startup_snapshot_path_utf8(config, "snap.bin",
                                                         V8HOST_AUTO_LENGTH);
  }
  return s;
}

bool ValidateBuilderSetters(std::string* detail) {
  // A NULL builder is reported but has nothing to poison.
  if (v8host_config_set_tier(nullptr, v8host_tier_untrusted) !=
          v8host_status_invalid_arg ||
      v8host_config_add_file_rule_utf8(nullptr, "x", V8HOST_AUTO_LENGTH,
                                       false) != v8host_status_invalid_arg ||
      v8host_run_config_set_tier(nullptr, v8host_tier_untrusted) !=
          v8host_status_invalid_arg ||
      v8host_run_config_set_payload(nullptr, nullptr, 0) !=
          v8host_status_invalid_arg) {
    return Fail(detail, "null builder");
  }

  const std::string too_long(kMaxStringBytes + 1, 'x');
  const std::u16string unpaired(1, static_cast<char16_t>(0xD800));
  SetterCase cases[] = {
      {"broker mode range", v8host_status_invalid_arg,
       [](v8host_config c) {
         return v8host_config_set_broker_mode(c,
                                              static_cast<v8host_broker_mode>(2));
       }},
      {"tier range", v8host_status_invalid_arg,
       [](v8host_config c) {
         return v8host_config_set_tier(c, static_cast<v8host_tier>(-1));
       }},
      {"integrity range", v8host_status_invalid_arg,
       [](v8host_config c) {
         return v8host_config_set_integrity_level(
             c, static_cast<v8host_integrity_level>(2));
       }},
      {"delayed integrity range", v8host_status_invalid_arg,
       [](v8host_config c) {
         return v8host_config_set_delayed_integrity_level(
             c, static_cast<v8host_integrity_level>(-1));
       }},
      {"initial token range", v8host_status_invalid_arg,
       [](v8host_config c) {
         return v8host_config_set_initial_token_level(
             c, static_cast<v8host_token_level>(5));
       }},
      {"lockdown token range", v8host_status_invalid_arg,
       [](v8host_config c) {
         return v8host_config_set_lockdown_token_level(
             c, static_cast<v8host_token_level>(-1));
       }},
      {"utf8 null", v8host_status_invalid_arg,
       [](v8host_config c) {
         return v8host_config_add_file_rule_utf8(c, nullptr, 0, false);
       }},
      {"utf8 embedded nul", v8host_status_invalid_arg,
       [](v8host_config c) {
         return v8host_config_add_file_rule_utf8(c, "a\0b", 3, false);
       }},
      {"utf8 invalid", v8host_status_invalid_arg,
       [](v8host_config c) {
         return v8host_config_add_file_rule_utf8(c, "\xC3\x28",
                                                 V8HOST_AUTO_LENGTH, false);
       }},
      {"utf8 encoded surrogate", v8host_status_invalid_arg,
       [](v8host_config c) {
         return v8host_config_set_profile_name_utf8(c, "\xED\xA0\x80",
                                                    V8HOST_AUTO_LENGTH);
       }},
      {"utf8 too long", v8host_status_quota_exceeded,
       [&](v8host_config c) {
         return v8host_config_add_file_rule_utf8(c, too_long.data(),
                                                 too_long.size(), false);
       }},
      {"utf16 unpaired surrogate", v8host_status_invalid_arg,
       [&](v8host_config c) {
         return v8host_config_add_file_rule_utf16(c, unpaired.data(),
                                                  unpaired.size(), false);
       }},
      {"utf16 embedded nul", v8host_status_invalid_arg,
       [](v8host_config c) {
         return v8host_config_set_profile_name_utf16(c, u"a\0b", 3);
       }},
      {"empty capability", v8host_status_invalid_arg,
       [](v8host_config c) {
         return v8host_config_add_capability_utf8(c, "", V8HOST_AUTO_LENGTH);
       }},
      {"65th file rule", v8host_status_quota_exceeded,
       [](v8host_config c) {
         for (int n = 0; n < 64; ++n) {
           if (v8host_config_add_file_rule_utf8(c, "x", 1, false) !=
               v8host_status_ok)
             return v8host_status_internal_error;
         }
         return v8host_config_add_file_rule_utf8(c, "x", 1, false);
       }},
      {"65th capability", v8host_status_quota_exceeded,
       [](v8host_config c) {
         for (int n = 0; n < 64; ++n) {
           if (v8host_config_add_capability_utf8(c, "cap", 3) !=
               v8host_status_ok)
             return v8host_status_internal_error;
         }
         return v8host_config_add_capability_utf8(c, "cap", 3);
       }},
  };
  for (SetterCase& c : cases) {
    v8host_session out = reinterpret_cast<v8host_session>(1);
    const v8host_status status =
        v8host_session_open(&ApplySetterCase, &c, &out);
    if (c.first != c.expected || c.after != c.expected ||
        status != c.expected || out != nullptr) {
      *detail = std::string("session setter: ") + c.name;
      return false;
    }
  }

  FakeTransport* fake = nullptr;
  v8host_session session = nullptr;
  if (v8host_session_open(&ConfigureUtf8Session, nullptr, &session) !=
      v8host_status_ok)
    return Fail(detail, "utf8 session");
  fake = LatestFake();
  FrameHeader h;
  const uint8_t* body = nullptr;
  size_t len = 0;
  CreateSessionPayload p;
  const auto frame = fake->Frame(0);
  if (!DecodeFrame(frame, &h, &body, &len) ||
      !DecodeCreateSessionPayload(body, len, &p) ||
      p.integrity != 0 || p.delayed_integrity != 1 ||
      p.file_rules.size() != 3 || p.file_rules[0].pattern != "C:\\a\\*" ||
      !p.file_rules[0].readonly || p.file_rules[1].pattern != "C:\\b\\*" ||
      p.file_rules[1].readonly ||
      p.file_rules[2].pattern != "C:\\\xC3\xA9\\*" ||
      p.app_container_profile != "profile" || p.capabilities.size() != 2 ||
      p.capabilities[0] != "S-1-15-3-1" || p.capabilities[1] != "S-1-15-3-2") {
    v8host_session_close(session);
    Drain();
    return Fail(detail, "utf8 session roundtrip");
  }

  v8host_run run = nullptr;
  if (v8host_session_start_run(session, &ConfigureUtf8Run, nullptr, &run) !=
      v8host_status_ok) {
    v8host_session_close(session);
    Drain();
    return Fail(detail, "utf8 run");
  }
  StartRunPayload rp;
  const auto run_frame = fake->Frame(1);
  const bool run_ok = DecodeFrame(run_frame, &h, &body, &len) &&
                      DecodeStartRunPayload(body, len, &rp) &&
                      rp.tier_override == -1 && rp.has_engine_override &&
                      rp.engine_filename == "engine.dll" && rp.has_snapshot &&
                      rp.snapshot_path == "snap.bin";

  const std::vector<uint8_t> big(kMaxFramePayload + 1, 0);
  RunSetterCase run_cases[] = {
      {"run tier range", v8host_status_invalid_arg,
       [](v8host_run_config c) {
         return v8host_run_config_set_tier(c, static_cast<v8host_tier>(2));
       }},
      {"engine utf8 path", v8host_status_invalid_arg,
       [](v8host_run_config c) {
         return v8host_run_config_set_engine_dll_utf8(c, "a/b.dll",
                                                      V8HOST_AUTO_LENGTH);
       }},
      {"engine utf8 invalid", v8host_status_invalid_arg,
       [](v8host_run_config c) {
         return v8host_run_config_set_engine_dll_utf8(c, "\xFF.dll",
                                                      V8HOST_AUTO_LENGTH);
       }},
      {"snapshot utf8 empty", v8host_status_invalid_arg,
       [](v8host_run_config c) {
         return v8host_run_config_set_startup_snapshot_path_utf8(c, "", 0);
       }},
      {"payload over frame", v8host_status_quota_exceeded,
       [&](v8host_run_config c) {
         return v8host_run_config_set_payload(c, big.data(), big.size());
       }},
  };
  bool cases_ok = true;
  const char* failed = nullptr;
  for (RunSetterCase& c : run_cases) {
    v8host_run out = reinterpret_cast<v8host_run>(1);
    const v8host_status status =
        v8host_session_start_run(session, &ApplyRunSetterCase, &c, &out);
    if (c.first != c.expected || c.after != c.expected ||
        status != c.expected || out != nullptr) {
      cases_ok = false;
      failed = c.name;
      break;
    }
  }
  v8host_session_close(session);
  Drain();
  if (!run_ok)
    return Fail(detail, "utf8 run roundtrip");
  if (!cases_ok) {
    *detail = std::string("run setter: ") + failed;
    return false;
  }
  return true;
}

bool ValidateRunPolicyAndOutputs(std::string* detail) {
  v8host_session_s* session = Create();
  if (!session)
    return Fail(detail, "create");
  v8host_run_s* out = reinterpret_cast<v8host_run_s*>(1);
  TestRunInputs i = BasicRun();
  i.tier_override = v8host_tier_trusted;
  if (StartRun(session, &i, &out) != v8host_status_invalid_arg ||
      out != nullptr)
    return Fail(detail, "tier envelope");
  for (int32_t tier : {-2, 2}) {
    i = BasicRun();
    i.tier_override = tier;
    out = reinterpret_cast<v8host_run_s*>(1);
    if (StartRun(session, &i, &out) != v8host_status_invalid_arg ||
        out != nullptr)
      return Fail(detail, "invalid tier/output");
  }
  const wchar_t* bad[] = {L"a/b.dll", L"a\\b.dll", L"C:x.dll", L".",
                          L"..",      L"a..b",     L"..x",     L"x.."};
  for (const wchar_t* value : bad) {
    i = BasicRun();
    i.engine_dll_override = value;
    out = reinterpret_cast<v8host_run_s*>(1);
    if (StartRun(session, &i, &out) != v8host_status_invalid_arg ||
        out != nullptr)
      return Fail(detail, "bad engine path/output");
  }
  char byte = 0;
  i = BasicRun(&byte, 0);
  out = reinterpret_cast<v8host_run_s*>(1);
  if (StartRun(session, &i, &out) != v8host_status_invalid_arg ||
      out != nullptr)
    return Fail(detail, "payload pair");
  i = BasicRun(nullptr, 1);
  out = reinterpret_cast<v8host_run_s*>(1);
  if (StartRun(session, &i, &out) != v8host_status_invalid_arg ||
      out != nullptr)
    return Fail(detail, "reverse payload pair");
  i = BasicRun();
  i.startup_snapshot_path = L"";
  out = reinterpret_cast<v8host_run_s*>(1);
  if (StartRun(session, &i, &out) != v8host_status_invalid_arg ||
      out != nullptr)
    return Fail(detail, "empty snapshot/output");
  v8host_session_close(session);
  Drain();
  return true;
}

bool LifecycleTokens(std::string* detail) {
  using namespace v8host::client;
  Drain();
  const auto ignored = IgnoredLifecycleTokensForTesting();
  std::atomic<int> called{0};
  for (UINT message : {WM_APP + 0x1E, WM_APP + 0x1F, WM_APP + 0x20}) {
    const auto token = PostLifecycleProbeForTesting(message, &called);
    if (!token) return Fail(detail, "legitimate lifecycle token post failed");
    Drain();
    const auto before = called.load();
    for (uint64_t forged : {uint64_t{0}, uint64_t{0x0123456789abcdef},
                            uint64_t{reinterpret_cast<uintptr_t>(&called)}, token})
      PostLifecycleTokenForTesting(message, forged);
    Drain();
    if (called != before) return Fail(detail, "forged/replayed lifecycle token invoked callback");
  }
  return called == 3 && IgnoredLifecycleTokensForTesting() == ignored + 12;
}

bool DefaultInitialToken(std::string* detail) {
  TopologyFixture f;
  v8host_worker w = nullptr;
  if (!f.Open() || f.Worker(&w) != v8host_status_ok) return false;
  const auto frame = f.Request(MessageType::CREATE_WORKER, v8host_worker_get_id(w));
  FrameHeader h;
  const uint8_t* body = nullptr;
  size_t size = 0;
  CreateWorkerPayload config;
  if (!DecodeFrame(frame, &h, &body, &size) || !DecodeCreateWorkerPayload(body, size, &config) ||
      config.session.initial_token != v8host_token_level_restricted_same_access ||
      config.session.lockdown_token != v8host_token_level_lockdown)
    return Fail(detail, "worker default token pair");
  return true;
}

bool ConfigRoundTrip(std::string* detail) {
  FakeTransport* zero_fake = nullptr;
  v8host_session_s* zero_session = Create(&zero_fake);
  if (zero_session == nullptr)
    return Fail(detail, "create zero");
  FrameHeader zero_header;
  const uint8_t* zero_body = nullptr;
  size_t zero_len = 0;
  CreateSessionPayload zero;
  const auto zero_frame = zero_fake->Frame(0);
  if (!DecodeFrame(zero_frame, &zero_header, &zero_body, &zero_len) ||
      !DecodeCreateSessionPayload(zero_body, zero_len, &zero) ||
      zero.broker_mode != v8host_broker_mode_shared ||
      zero.tier != v8host_tier_untrusted || zero.integrity != 0 ||
      zero.delayed_integrity != 0 || zero.initial_token != v8host_token_level_restricted_same_access ||
      zero.lockdown_token != 0 || !zero.prohibit_dynamic_code ||
      zero.use_app_container || zero.low_privilege_app_container ||
      !zero.app_container_profile.empty() || !zero.file_rules.empty() ||
      !zero.capabilities.empty()) {
    return Fail(detail, "zero config field fidelity");
  }
  v8host_session_close(zero_session);
  Drain();

  std::vector<std::wstring> patterns(64), caps(64);
  std::vector<TestFileRule> rules(64);
  std::vector<const wchar_t*> cap_ptrs(64);
  for (size_t n = 0; n < 64; ++n) {
    patterns[n] = L"rule" + std::to_wstring(n);
    caps[n] = L"cap" + std::to_wstring(n);
    rules[n] = {patterns[n].c_str(), (n & 1) != 0};
    cap_ptrs[n] = caps[n].c_str();
  }
  TestSessionConfig c = BasicConfig();
  c.integrity = 1; c.delayed_integrity = 0; c.initial_token = 3;
  c.lockdown_token = 4; c.file_rules = rules.data(); c.file_rule_count = 64;
  c.use_app_container = 1; c.low_privilege_app_container = 1;
  c.app_container_profile_name = L"profile";
  c.capabilities = cap_ptrs.data(); c.capability_count = 64;
  v8host_session_s* session = nullptr;
  if (OpenSession(&c, &session) != v8host_status_ok)
    return Fail(detail, "create 64");
  patterns[0] = L"mutated";
  FrameHeader h; const uint8_t* body; size_t len;
  const auto frame = LatestFake()->Frame(0);
  CreateSessionPayload p;
  bool ok = DecodeFrame(frame, &h, &body, &len) &&
            DecodeCreateSessionPayload(body, len, &p) &&
            p.broker_mode == v8host_broker_mode_shared &&
            p.tier == v8host_tier_untrusted && p.integrity == 1 &&
            p.delayed_integrity == 0 && p.initial_token == 3 &&
            p.lockdown_token == 4 && p.prohibit_dynamic_code &&
            p.use_app_container && p.low_privilege_app_container &&
            p.app_container_profile == "profile" &&
            p.file_rules.size() == 64 && p.capabilities.size() == 64 &&
            h.target_id != 0 && h.request_id != 0;
  for (size_t n = 0; ok && n < 64; ++n) {
    ok = p.file_rules[n].pattern == "rule" + std::to_string(n) &&
         p.file_rules[n].readonly == ((n & 1) != 0) &&
         p.capabilities[n] == "cap" + std::to_string(n);
  }
  v8host_session_close(session);
  Drain();
  return ok || Fail(detail, "config roundtrip/copy");
}

bool BoundaryQuotas(std::string* detail) {
  std::vector<TestFileRule> rules(65, TestFileRule{L"x", false});
  TestSessionConfig config = BasicConfig();
  config.file_rules = rules.data();
  config.file_rule_count = 65;
  v8host_session_s* session = reinterpret_cast<v8host_session_s*>(1);
  if (OpenSession(&config, &session) != v8host_status_quota_exceeded ||
      session != nullptr)
    return Fail(detail, "65 file rules not rejected/null");
  std::vector<const wchar_t*> caps(65, L"cap");
  config = BasicConfig();
  config.use_app_container = 1;
  config.app_container_profile_name = L"profile";
  config.capabilities = caps.data();
  config.capability_count = 65;
  session = reinterpret_cast<v8host_session_s*>(1);
  if (OpenSession(&config, &session) != v8host_status_quota_exceeded ||
      session != nullptr)
    return Fail(detail, "65 capabilities not rejected/null");

  std::wstring accepted(kMaxStringBytes, L'a');
  TestFileRule long_rule{accepted.c_str(), false};
  config = BasicConfig();
  config.file_rules = &long_rule;
  config.file_rule_count = 1;
  if (OpenSession(&config, &session) != v8host_status_ok)
    return Fail(detail, "32-KiB string boundary rejected");
  v8host_session_close(session);
  Drain();
  std::wstring rejected(kMaxStringBytes + 1, L'a');
  long_rule.pattern = rejected.c_str();
  session = reinterpret_cast<v8host_session_s*>(1);
  if (OpenSession(&config, &session) != v8host_status_quota_exceeded ||
      session != nullptr)
    return Fail(detail, "32-KiB+1 string not rejected/null");

  FakeTransport* fake = nullptr;
  session = Create(&fake);
  TestRunInputs input = BasicRun();
  v8host_run_s* runs[5] = {};
  for (int n = 0; n < 4; ++n) {
    if (StartRun(session, &input, &runs[n]) != v8host_status_ok)
      return Fail(detail, "four-run boundary rejected");
  }

  runs[4] = reinterpret_cast<v8host_run_s*>(1);
  if (StartRun(session, &input, &runs[4]) != v8host_status_quota_exceeded ||
      runs[4] != nullptr)
    return Fail(detail, "fifth in-flight run not rejected/null");
  v8host_session_close(session);
  Drain();

  session = Create();
  const size_t max_guest =
      kMaxFramePayload - kStartRunFixedSize - 2 * sizeof(uint32_t);
  std::vector<uint8_t> guest(max_guest, 0xA5);
  input = BasicRun(guest.data(), guest.size());
  v8host_run_s* run = nullptr;
  if (StartRun(session, &input, &run) != v8host_status_ok)
    return Fail(detail, "exact frame ceiling rejected");
  v8host_session_close(session);
  Drain();
  session = Create();
  guest.push_back(0xA5);
  input = BasicRun(guest.data(), guest.size());
  run = reinterpret_cast<v8host_run_s*>(1);
  const bool over =
      StartRun(session, &input, &run) == v8host_status_quota_exceeded &&
      run == nullptr;
  v8host_session_close(session);
  Drain();
  return over || Fail(detail, "frame ceiling+1 not rejected/null");
}

bool ExhaustiveFailingOutputNulling(std::string* detail) {
  auto create_fails_null = [&](const TestSessionConfig* config) {
    v8host_session_s* out = reinterpret_cast<v8host_session_s*>(1);
    return OpenSession(config, &out) != v8host_status_ok &&
           out == nullptr;
  };
  TestSessionConfig config = BasicConfig();
  {
    v8host_session out = reinterpret_cast<v8host_session>(1);
    if (v8host_session_open(&FailSessionConfigure, nullptr, &out) ==
            v8host_status_ok ||
        out != nullptr) {
      return Fail(detail, "failing configure output");
    }
  }
  config.broker_mode = static_cast<v8host_broker_mode>(2);
  if (!create_fails_null(&config))
    return Fail(detail, "broker enum output");
  config = BasicConfig();
  config.tier = static_cast<v8host_tier>(2);
  if (!create_fails_null(&config))
    return Fail(detail, "tier enum output");
  config = BasicConfig();
  const std::vector<TestFileRule> too_many_rules(65, TestFileRule{L"C:\\x"});
  config.file_rules = too_many_rules.data();
  config.file_rule_count = too_many_rules.size();
  if (!create_fails_null(&config))
    return Fail(detail, "count output");
  config = BasicConfig();
  config.use_app_container = 1;
  if (!create_fails_null(&config))
    return Fail(detail, "AppContainer output");

  v8host_session_s* session = Create();
  auto start_fails_null = [&](const TestRunInputs* input) {
    v8host_run_s* out = reinterpret_cast<v8host_run_s*>(1);
    return StartRun(session, input, &out) != v8host_status_ok &&
           out == nullptr;
  };
  {
    v8host_run out = reinterpret_cast<v8host_run>(1);
    if (v8host_session_start_run(session, &FailRunConfigure, nullptr, &out) ==
            v8host_status_ok ||
        out != nullptr) {
      return Fail(detail, "failing run configure output");
    }
  }
  TestRunInputs input = BasicRun();
  input.tier_override = 1;
  if (!start_fails_null(&input))
    return Fail(detail, "tier envelope output");
  input = BasicRun();
  input.engine_dll_override = L"..";
  if (!start_fails_null(&input))
    return Fail(detail, "engine path output");
  input = BasicRun();
  input.startup_snapshot_path = L"";
  if (!start_fails_null(&input))
    return Fail(detail, "snapshot output");
  input = BasicRun(nullptr, 1);
  if (!start_fails_null(&input))
    return Fail(detail, "payload pair output");
  std::vector<uint8_t> huge(kMaxFramePayload, 0);
  input = BasicRun(huge.data(), huge.size());
  if (!start_fails_null(&input))
    return Fail(detail, "frame quota output");
  v8host_session_close(session);
  Drain();
  return true;
}

bool RunAndRelayRoundTrip(std::string* detail) {
  FakeTransport* fake = nullptr;
  v8host_session_s* session = Create(&fake);
  char payload[] = "guest";
  TestRunInputs i = BasicRun(payload, 5);
  i.engine_dll_override = L"engine.dll";
  i.startup_snapshot_path = L"snap.bin";
  v8host_run_s* run = nullptr;
  if (StartRun(session, &i, &run) != v8host_status_ok)
    return Fail(detail, "start");
  std::memset(payload, 'x', 5);
  FrameHeader h; const uint8_t* body; size_t len;
  StartRunPayload p;
  auto frame = fake->Frame(1);
  if (!DecodeFrame(frame, &h, &body, &len) ||
      !DecodeStartRunPayload(body, len, &p) ||
      std::string(p.guest_payload.begin(), p.guest_payload.end()) != "guest" ||
      p.engine_filename != "engine.dll" || h.run_id == 0)
    return Fail(detail, "start roundtrip/copy");
  char relay[] = "abc";
  // A kind outside v8host_message_kind is rejected before anything is queued;
  // the next frame must still be the valid relay below.
  for (int32_t bad_kind : {-7, 2}) {
    if (v8host_run_post_message(run, static_cast<v8host_message_kind>(bad_kind),
                                relay, 3) != v8host_status_invalid_arg)
      return Fail(detail, "relay kind not rejected");
  }
  if (v8host_run_post_message(run, v8host_message_kind_binary, relay, 3) !=
      v8host_status_ok)
    return Fail(detail, "relay post");
  std::memset(relay, 'z', 3);
  frame = fake->Frame(2);
  int32_t kind; const uint8_t* relay_body; size_t relay_len;
  if (!DecodeFrame(frame, &h, &body, &len) ||
      !DecodeRelayPayload(body, len, &kind, &relay_body, &relay_len) ||
      h.request_id != 0 || kind != v8host_message_kind_binary || relay_len != 3 ||
      std::memcmp(relay_body, "abc", 3) != 0)
    return Fail(detail, "relay roundtrip/copy");
  if (v8host_run_cancel(run) != v8host_status_ok)
    return Fail(detail, "cancel");
  frame = fake->Frame(3);
  if (!DecodeFrame(frame, &h, &body, &len) ||
      h.type != MessageType::CANCEL_RUN || len != 0)
    return Fail(detail, "cancel serialization");
  if (v8host_run_post_message(run, v8host_message_kind_string, nullptr, 0) !=
      v8host_status_ok)
    return Fail(detail, "empty relay");
  FrameHeader empty_header;
  frame = fake->Frame(4);
  if (!DecodeFrame(frame, &empty_header, &body, &len) || len != 4 ||
      empty_header.request_id != 0 || h.request_id != 4)
    return Fail(detail, "empty relay/request monotonicity");
  v8host_session_close(session);
  frame = fake->Frame(5);
  if (!DecodeFrame(frame, &h, &body, &len) ||
      h.type != MessageType::CLOSE_SESSION || h.request_id != 5)
    return Fail(detail, "close control order");
  Drain();
  return true;
}

bool CallbackRoutingAndReplacement(std::string* detail) {
  FakeTransport* fake = nullptr;
  v8host_session_s* session = Create(&fake);
  Recorder a, b;
  v8host_callbacks ca = Callbacks(&a), cb = Callbacks(&b);
  v8host_session_set_callbacks(session, &ca);
  FrameHeader create_header;
  const auto create_frame = fake->Frame(0);
  DecodeAndValidateFrame(create_frame.data(), create_frame.size(),
                         &create_header);
  std::thread io([&] {
    fake->Inbound(EventFrame(MessageType::SESSION_READY,
                             create_header.target_id));
  });
  io.join();
  v8host_session_set_callbacks(session, &cb);
  if (!PumpUntil([&] { return a.total.load() == 1; }))
    return Fail(detail, "queued callback snapshot");
  v8host_callbacks clear = {};
  clear.struct_size = sizeof(clear);
  v8host_session_set_callbacks(session, &clear);
  fake->Inbound(EventFrame(MessageType::STARTUP_READY,
                           create_header.target_id));
  Drain();
  const bool ok = a.states.size() == 1 && b.total.load() == 0 &&
                  a.state_thread == ::GetCurrentThreadId();
  v8host_session_close(session);
  Drain();
  return ok || Fail(detail, "replace/clear/app thread");
}

bool CallbackAllocationFailure(std::string* detail) {
  FakeTransport* fake = nullptr;
  v8host_session_s* session = Create(&fake);
  Recorder prior, rejected;
  v8host_callbacks first = Callbacks(&prior);
  v8host_callbacks second = Callbacks(&rejected);
  if (v8host_session_set_callbacks(session, &first) != v8host_status_ok)
    return Fail(detail, "initial callback install");
  FailNextCallbackCopyForTesting();
  if (v8host_session_set_callbacks(session, &second) != v8host_status_no_memory)
    return Fail(detail, "injected callback allocation failure");
  FrameHeader h;
  const auto create = fake->Frame(0);
  DecodeAndValidateFrame(create.data(), create.size(), &h);
  fake->Inbound(EventFrame(MessageType::SESSION_READY, h.target_id));
  const bool delivered =
      PumpUntil([&] { return prior.total.load() == 1; }) &&
      rejected.total.load() == 0;
  v8host_session_close(session);
  Drain();
  return delivered || Fail(detail, "prior callback table was not retained");
}

bool InboundRunAndTerminal(std::string* detail) {
  FakeTransport* fake = nullptr;
  v8host_session_s* session = Create(&fake);
  Recorder r;
  v8host_callbacks cb = Callbacks(&r);
  v8host_session_set_callbacks(session, &cb);
  TestRunInputs input = BasicRun();
  v8host_run_s* run = nullptr;
  StartRun(session, &input, &run);
  FrameHeader start;
  const auto start_frame = fake->Frame(1);
  DecodeAndValidateFrame(start_frame.data(), start_frame.size(), &start);
  const auto ack = AckFor(fake->Frame(1));
  const uint8_t msg[] = {'h', 'i'};
  FrameHeader relay_h = start;
  relay_h.type = MessageType::RELAY_FROM_WORKER;
  relay_h.flags = 0;
  const auto relay_frame = BuildRelayFrame(relay_h, 42, msg, sizeof(msg));
  std::thread io([&] {
    fake->Inbound(ack);
    fake->Inbound(relay_frame);
    fake->Inbound(ResultFrame(start.target_id, start.run_id));
    fake->Inbound(
        EventFrame(MessageType::WORKER_EXIT, start.target_id, start.run_id));
  });
  io.join();
  if (!PumpUntil([&] { return r.total.load() >= 3; }))
    return Fail(detail, "inbound callbacks");
  const bool ok = r.events.size() == 2 &&
      r.events[0] == v8host_run_event_started &&
      r.events[1] == v8host_run_event_completed &&
      r.kinds.size() == 1 && r.kinds[0] == 42 && r.messages[0] == "hi" &&
      r.run_thread == ::GetCurrentThreadId() &&
      r.relay_thread == ::GetCurrentThreadId() &&
      v8host_run_post_message(run, v8host_message_kind_string, nullptr, 0) ==
          v8host_status_run_terminal &&
      v8host_run_cancel(run) == v8host_status_run_terminal;
  v8host_session_close(session);
  Drain();
  return ok || Fail(detail, "routing/one terminal/terminal handle");
}

bool InboundCancelledResult(std::string* detail) {
  FakeTransport* fake = nullptr;
  v8host_session_s* session = Create(&fake);
  Recorder r;
  v8host_callbacks cb = Callbacks(&r);
  v8host_session_set_callbacks(session, &cb);
  TestRunInputs input = BasicRun();
  v8host_run_s* run = nullptr;
  StartRun(session, &input, &run);
  FrameHeader start;
  const auto start_frame = fake->Frame(1);
  DecodeAndValidateFrame(start_frame.data(), start_frame.size(), &start);
  std::thread io([&] {
    fake->Inbound(AckFor(fake->Frame(1)));
    fake->Inbound(ResultFrame(start.target_id, start.run_id,
                              ResultDisposition::kCancelled));
  });
  io.join();
  if (!PumpUntil([&] { return r.total.load() >= 2; }))
    return Fail(detail, "inbound callbacks");
  const bool ok = r.events.size() == 2 &&
                  r.events[0] == v8host_run_event_started &&
                  r.events[1] == v8host_run_event_cancelled &&
                  v8host_run_cancel(run) == v8host_status_run_terminal;
  v8host_session_close(session);
  Drain();
  return ok || Fail(detail, "cancelled result -> CANCELLED terminal");
}

bool LateTrafficAfterTerminal(std::string* detail) {
  FakeTransport* fake = nullptr;
  v8host_session_s* session = Create(&fake);
  Recorder r;
  v8host_callbacks callbacks = Callbacks(&r);
  v8host_session_set_callbacks(session, &callbacks);
  TestRunInputs input = BasicRun();
  v8host_run_s* run = nullptr;
  StartRun(session, &input, &run);
  const auto start_frame = fake->Frame(1);
  FrameHeader start;
  DecodeAndValidateFrame(start_frame.data(), start_frame.size(), &start);

  fake->Inbound(ResultFrame(start.target_id, start.run_id));
  fake->Inbound(AckFor(start_frame));
  const uint8_t byte = 0x5A;
  FrameHeader relay = start;
  relay.type = MessageType::RELAY_FROM_WORKER;
  relay.flags = 0;
  fake->Inbound(BuildRelayFrame(relay, 17, &byte, 1));
  if (!PumpUntil([&] { return r.total.load() == 1; }))
    return Fail(detail, "terminal callback did not arrive");
  Drain();
  const bool ok = r.events.size() == 1 &&
                  r.events[0] == v8host_run_event_completed &&
                  r.kinds.empty() && r.total.load() == 1;
  v8host_session_close(session);
  Drain();
  return ok || Fail(detail, "late ACK/relay delivered after terminal");
}

bool CorrelationMismatchDisconnects(std::string* detail) {
  for (int variant = 0; variant < 3; ++variant) {
    FakeTransport* fake = nullptr;
    v8host_session_s* session = Create(&fake);
    Recorder r;
    v8host_callbacks callbacks = Callbacks(&r);
    v8host_session_set_callbacks(session, &callbacks);
    TestRunInputs input = BasicRun();
    v8host_run_s* run = nullptr;
    StartRun(session, &input, &run);
    const auto request = fake->Frame(1);
    FrameHeader request_header;
    DecodeAndValidateFrame(request.data(), request.size(), &request_header);
    std::vector<uint8_t> response =
        variant == 2 ? ErrorFor(request) : AckFor(request);
    FrameHeader bad;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    DecodeAndValidateFrame(response.data(), response.size(), &bad, &payload,
                           &payload_len);
    if (variant == 0)
      bad.conn_id++;
    else if (variant == 1)
      bad.target_id++;
    else
      bad.run_id++;
    std::vector<uint8_t> mutated;
    EncodeHeader(bad, mutated);
    mutated.insert(mutated.end(), payload, payload + payload_len);
    fake->Inbound(std::move(mutated));
    if (!PumpUntil([&] { return r.total.load() >= 3; }))
      return Fail(detail, "correlation mismatch did not disconnect");
    if (r.disconnects != 1 || r.states.size() != 1 ||
        r.states[0] != v8host_session_state_closed ||
        r.state_status[0] != v8host_status_protocol_error || r.events.size() != 1 ||
        r.events[0] != v8host_run_event_failed ||
        r.event_status[0] != v8host_status_protocol_error) {
      return Fail(detail, "wrong protocol-error terminalization");
    }

    v8host_session_close(session);
    Drain();
  }
  return true;
}

bool UnknownRequestIdDisconnects(std::string* detail) {
  for (bool use_error : {false, true}) {
    FakeTransport* fake = nullptr;
    v8host_session_s* session = Create(&fake);
    Recorder r;
    v8host_callbacks callbacks = Callbacks(&r);
    v8host_session_set_callbacks(session, &callbacks);
    TestRunInputs input = BasicRun();
    v8host_run_s* run = nullptr;
    StartRun(session, &input, &run);
    const auto request = fake->Frame(1);
    std::vector<uint8_t> response =
        use_error ? ErrorFor(request) : AckFor(request);
    FrameHeader bad;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    DecodeAndValidateFrame(response.data(), response.size(), &bad, &payload,
                           &payload_len);
    bad.request_id += 0x10000;
    std::vector<uint8_t> mutated;
    EncodeHeader(bad, mutated);
    mutated.insert(mutated.end(), payload, payload + payload_len);
    fake->Inbound(std::move(mutated));
    if (!PumpUntil([&] { return r.total.load() >= 3; }))
      return Fail(detail, "unknown request id did not disconnect");
    if (r.disconnects != 1 || r.events.size() != 1 ||
        r.event_status[0] != v8host_status_protocol_error || r.states.size() != 1 ||
        r.state_status[0] != v8host_status_protocol_error) {
      return Fail(detail, "unknown request id protocol outcome");
    }
    v8host_session_close(session);
    Drain();
  }
  return true;
}

bool RelayQuotaError(std::string* detail) {
  for (int path = 0; path < 12; ++path) {
    FakeTransport* fake = nullptr; v8host_session_s* session = Create(&fake);
    if (!session) return Fail(detail, "quota session");
    Recorder r; auto cb = Callbacks(&r); v8host_session_set_callbacks(session, &cb);
    auto input = BasicRun(); v8host_run_s *first = nullptr, *second = nullptr;
    if (StartRun(session, &input, &first) != v8host_status_ok ||
        StartRun(session, &input, &second) != v8host_status_ok) return Fail(detail, "quota runs");
    FrameHeader h; auto request = fake->Frame(1); DecodeAndValidateFrame(request.data(), request.size(), &h);
    fake->Inbound(AckFor(fake->Frame(0)));
    h.request_id = 0;
    if (path == 2) fake->Inbound(ResultFrame(h.target_id, h.run_id));
    if (path == 3) v8host_session_close(session);
    if (path == 4) ++h.target_id;
    if (path == 5) h.run_id += 10;
    if (path == 8) h.request_id = 10000;
    if (path == 9) ++h.conn_id;
    if (path == 11) h.run_id = 0;
    ErrorPayload error; error.status_code = path == 6 ? StatusCode::ERROR_BAD_STATE : StatusCode::ERROR_QUOTA;
    auto frame = path == 7 ? BuildAckFrame(h) : BuildErrorFrame(h, error);
    if (path == 10) frame.pop_back();
    fake->Inbound(frame);
    if (path < 3) {
      fake->Inbound(frame);
      Drain();
      if (r.events.size() != 1 || r.disconnects ||
          r.events[0] != (path == 2 ? v8host_run_event_completed : v8host_run_event_failed) ||
          r.event_status[0] != (path == 2 ? v8host_status_ok : v8host_status_quota_exceeded) ||
          v8host_run_cancel(first) != v8host_status_run_terminal ||
          v8host_run_post_message(second, v8host_message_kind_string, nullptr, 0) !=
          v8host_status_ok)
        return Fail(detail, "quota addressing/duplicate/winner/other-run containment");
      if (path == 0) fake->Inbound(ResultFrame(h.target_id, h.run_id + 1));
      else fake->Disconnect();
      fake->Inbound(frame); Drain();
      if (r.events.size() != 2 || r.event_status[1] != (path == 0 ? v8host_status_ok : v8host_status_broker_lost) ||
          r.events[1] != (path == 0 ? v8host_run_event_completed : v8host_run_event_broker_lost) ||
          r.disconnects != (path == 0 ? 0 : 1)) return Fail(detail, "quota then completion/EOF once");
    } else if (path == 3) {
      Drain();
      if (r.total.load() != 0) return Fail(detail, "callbacks after close and late quota");
    } else {
      Drain();
      if (r.events.size() != 2 || r.event_status != std::vector<v8host_status>(2, v8host_status_protocol_error) ||
          r.disconnects != 1 || r.state_status != std::vector<v8host_status>{v8host_status_protocol_error})
        return Fail(detail, "invalid unsolicited quota must fail protocol");
    }
    if (path != 3) v8host_session_close(session);
    Drain();
  }
  return true;
}

bool PendingCapacityAndDuplicateAck(std::string* detail) {
  FakeTransport* fake = nullptr;
  v8host_session_s* session = Create(&fake);
  TestRunInputs input = BasicRun();
  v8host_run_s* run = nullptr;
  if (StartRun(session, &input, &run) != v8host_status_ok)
    return Fail(detail, "capacity run setup");
  fake->Inbound(AckFor(fake->Frame(0)));
  fake->Inbound(AckFor(fake->Frame(1)));
  const size_t controls_begin = fake->FrameCount();
  for (uint32_t n = 0; n < kMaxControlQueueRequests; ++n) {
    if (v8host_run_cancel(run) != v8host_status_ok)
      return Fail(detail, "accepted capacity cancel rejected");
  }
  const size_t at_capacity = fake->FrameCount();
  if (at_capacity != controls_begin + kMaxControlQueueRequests)
    return Fail(detail, "accepted controls not all sent");
  if (v8host_run_cancel(run) != v8host_status_quota_exceeded ||
      fake->FrameCount() != at_capacity)
    return Fail(detail, "capacity+1 sent or lacked explicit quota");

  const auto first_ack = AckFor(fake->Frame(controls_begin));
  for (size_t n = controls_begin; n < at_capacity; ++n)
    fake->Inbound(AckFor(fake->Frame(n)));
  fake->Inbound(first_ack);
  if (fake->closed.load() ||
      v8host_run_post_message(run, v8host_message_kind_binary, nullptr, 0) !=
          v8host_status_ok)
    return Fail(detail, "duplicate ACK falsely terminalized connection");
  v8host_session_close(session);
  Drain();
  return true;
}

bool ReentrantClose(std::string* detail) {
  FakeTransport* fake = nullptr;
  v8host_session_s* session = Create(&fake);
  Recorder r;
  r.close_in_callback = true;
  v8host_callbacks cb = Callbacks(&r);
  v8host_session_set_callbacks(session, &cb);
  TestRunInputs input = BasicRun();
  v8host_run_s* run = nullptr;
  if (StartRun(session, &input, &run) != v8host_status_ok)
    return Fail(detail, "reentrant close run setup");
  r.run = run;
  FrameHeader h;
  auto create = fake->Frame(0);
  DecodeAndValidateFrame(create.data(), create.size(), &h);
  fake->Inbound(EventFrame(MessageType::SESSION_READY, h.target_id));
  if (!PumpUntil([&] { return r.total.load() == 1; }))
    return Fail(detail, "reentrant callback");
  const bool transport_closed = fake->closed.load();
  Drain();
  return (r.after_close == v8host_status_invalid_state &&
          r.set_after_close == v8host_status_invalid_state &&
          r.post_after_close == v8host_status_run_terminal &&
          r.cancel_after_close == v8host_status_run_terminal &&
          transport_closed) ||
         Fail(detail, "reentrant close state");
}

struct ReentrantOps {
  v8host_session_s* session = nullptr;
  v8host_run_s* run = nullptr;
  v8host_callbacks replacement = {};
  v8host_status post_status = v8host_status_internal_error;
  v8host_status set_status = v8host_status_internal_error;
  std::atomic<int> calls{0};
};

void V8HOST_CDECL OnReentrantPostSet(void* user_data,
                                    v8host_session,
                                    v8host_session_state,
                                    v8host_status) {
  auto* ops = static_cast<ReentrantOps*>(user_data);
  const char value[] = "r";
  ops->post_status = v8host_run_post_message(
      ops->run, v8host_message_kind_string, value, sizeof(value) - 1);
  ops->set_status =
      v8host_session_set_callbacks(ops->session, &ops->replacement);
  ops->calls.fetch_add(1);
}

bool ReentrantPostAndSet(std::string* detail) {
  FakeTransport* fake = nullptr;
  v8host_session_s* session = Create(&fake);
  TestRunInputs input = BasicRun();
  v8host_run_s* run = nullptr;
  StartRun(session, &input, &run);
  Recorder replacement;
  ReentrantOps ops;
  ops.session = session;
  ops.run = run;
  ops.replacement = Callbacks(&replacement);
  v8host_callbacks callbacks = {};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.user_data = &ops;
  callbacks.on_session_state = &OnReentrantPostSet;
  v8host_session_set_callbacks(session, &callbacks);
  FrameHeader h;
  const auto create = fake->Frame(0);
  DecodeAndValidateFrame(create.data(), create.size(), &h);
  fake->Inbound(EventFrame(MessageType::SESSION_READY, h.target_id));
  if (!PumpUntil([&] { return ops.calls.load() == 1; }))
    return Fail(detail, "reentrant post/set callback");
  fake->Inbound(EventFrame(MessageType::STARTUP_READY, h.target_id));
  if (!PumpUntil([&] { return replacement.total.load() == 1; }))
    return Fail(detail, "replacement callback after reentrant set");
  FrameHeader relay;
  const uint8_t* payload = nullptr;
  size_t payload_len = 0;
  const auto frame = fake->Frame(2);
  const bool ok = ops.post_status == v8host_status_ok &&
                  ops.set_status == v8host_status_ok &&
                  DecodeFrame(frame, &relay, &payload, &payload_len) &&
                  relay.type == MessageType::RELAY_TO_WORKER;
  v8host_session_close(session);
  Drain();
  return ok || Fail(detail, "reentrant post/set outcome");
}

bool BackpressureAndCloseSuppression(std::string* detail) {
  FakeTransport* fake = nullptr;
  v8host_session_s* session = Create(&fake);
  Recorder r;
  v8host_callbacks cb = Callbacks(&r);
  v8host_session_set_callbacks(session, &cb);
  FrameHeader h; auto create = fake->Frame(0);
  DecodeAndValidateFrame(create.data(), create.size(), &h);
  for (size_t n = 0; n < v8host::client::kSessionQueueCapacity + 10; ++n)
    fake->Inbound(EventFrame(MessageType::SESSION_READY, h.target_id));
  if (!PumpUntil([&] { return r.total.load() == 1; }))
    return Fail(detail, "backpressure terminal");
  const bool overflow = r.states.size() == 1 &&
      r.states[0] == v8host_session_state_closed &&
      r.state_status[0] == v8host_status_callback_backpressure;
  v8host_session_close(session);
  Drain();

  session = Create(&fake);
  Recorder suppressed;
  cb = Callbacks(&suppressed);
  v8host_session_set_callbacks(session, &cb);
  create = fake->Frame(0);
  DecodeAndValidateFrame(create.data(), create.size(), &h);
  fake->Inbound(EventFrame(MessageType::SESSION_READY, h.target_id));
  v8host_session_close(session);
  Drain();
  return (overflow && suppressed.total.load() == 0) ||
         Fail(detail, "close suppression");
}

bool CloseWhileFloodingNonblocking(std::string* detail) {
  FakeTransport* fake = nullptr;
  v8host_session_s* session = Create(&fake);
  Recorder r;
  v8host_callbacks callbacks = Callbacks(&r);
  v8host_session_set_callbacks(session, &callbacks);
  FrameHeader h;
  const auto create = fake->Frame(0);
  DecodeAndValidateFrame(create.data(), create.size(), &h);
  std::atomic<bool> started{false};
  std::atomic<bool> stop{false};
  std::thread flood([&] {
    started.store(true, std::memory_order_release);
    while (!stop.load(std::memory_order_acquire))
      fake->Inbound(EventFrame(MessageType::SESSION_READY, h.target_id));
  });
  while (!started.load(std::memory_order_acquire))
    ::SwitchToThread();
  const ULONGLONG begin = ::GetTickCount64();
  v8host_session_close(session);
  const ULONGLONG elapsed = ::GetTickCount64() - begin;
  stop.store(true, std::memory_order_release);
  flood.join();
  Drain();
  return (elapsed < 500 && r.total.load() == 0) ||
         Fail(detail, "close blocked or delivered queued flood callbacks");
}

bool DisconnectNoReplay(std::string* detail) {
  FakeTransport* fake = nullptr;
  v8host_session_s* session = Create(&fake);
  Recorder r;
  auto cb = Callbacks(&r);
  v8host_session_set_callbacks(session, &cb);
  TestRunInputs input = BasicRun();
  v8host_run_s* run1 = nullptr; v8host_run_s* run2 = nullptr;
  StartRun(session, &input, &run1);
  StartRun(session, &input, &run2);
  const size_t before = fake->FrameCount();
  std::thread io([&] { fake->Disconnect(); });
  io.join();
  if (!PumpUntil([&] { return r.total.load() >= 4; }))
    return Fail(detail, "disconnect callbacks");
  const bool ok = fake->FrameCount() == before && r.disconnects == 1 &&
      r.events.size() == 2 && r.states.size() == 1 &&
      r.states[0] == v8host_session_state_closed &&
      r.state_status[0] == v8host_status_broker_lost &&
      r.events[0] == v8host_run_event_broker_lost &&
      r.events[1] == v8host_run_event_broker_lost &&
      r.event_status[0] == v8host_status_broker_lost &&
      r.event_status[1] == v8host_status_broker_lost &&
      r.run_thread == ::GetCurrentThreadId() &&
      r.state_thread == ::GetCurrentThreadId() &&
      r.disconnect_thread == ::GetCurrentThreadId() &&
      v8host_run_cancel(run1) == v8host_status_run_terminal;
  v8host_session_close(session);
  const size_t after_close = fake->FrameCount();
  Drain();
  return (ok && after_close == before && fake->closed.load()) ||
         Fail(detail, "disconnect terminalization/replay");
}

bool MalformedFrameDisconnects(std::string* detail) {
  for (unsigned variant = 0; variant < 6; ++variant) {
    FakeTransport* fake = nullptr;
    v8host_session_s* session = Create(&fake);
    Recorder r;
    v8host_callbacks callbacks = Callbacks(&r);
    v8host_session_set_callbacks(session, &callbacks);
    FrameHeader create;
    const auto frame = fake->Frame(0);
    DecodeAndValidateFrame(frame.data(), frame.size(), &create);
    auto malformed = EventFrame(MessageType::STARTUP_READY, create.target_id);
    if (variant == 0) malformed = {1, 2, 3};
    if (variant == 1) malformed[32] = 2;
    if (variant == 2) malformed[34] = 1;
    if (variant == 3) for (size_t i = 36; i < 40; ++i) malformed[i] = 0;
    if (variant == 4) { malformed.resize(32); malformed[28] = 0; }
    if (variant == 5) { malformed.push_back(0); malformed[28] = 9; }
    fake->Inbound(malformed);
    if (!PumpUntil([&] { return r.total.load() >= 2; }))
      return Fail(detail, "malformed frame did not disconnect");
    const bool ok = r.disconnects == 1 && r.states.size() == 1 &&
                    r.states[0] == v8host_session_state_closed &&
                    r.state_status[0] == v8host_status_protocol_error;
    v8host_session_close(session);
    Drain();
    if (!ok) return Fail(detail, "malformed frame protocol outcome");
  }
  return true;
}

bool InboundVsCloseRace(std::string* detail) {
  const int baseline = v8host::client::SessionDispatch::live_count();
  g_inbound_ok.store(0);
  g_inbound_invalid.store(0);
  g_inbound_other.store(0);
  SetInboundStatusObserverForTesting(&ObserveInboundStatus);
  for (int cycle = 0; cycle < 1000; ++cycle) {
    FakeTransport* fake = nullptr;
    v8host_session_s* session = Create(&fake);
    if (session == nullptr)
      return Fail(detail, "race create");
    FrameHeader h; auto create = fake->Frame(0);
    DecodeAndValidateFrame(create.data(), create.size(), &h);
    std::atomic<bool> started{false};
    std::thread io([&] {
      started.store(true, std::memory_order_release);
      for (int n = 0; n < 16; ++n)
        fake->Inbound(EventFrame(MessageType::SESSION_READY, h.target_id));
    });
    while (!started.load(std::memory_order_acquire))
      ::SwitchToThread();
    v8host_session_close(session);
    io.join();
    Drain();
    if (v8host::client::SessionDispatch::live_count() != baseline)
      return Fail(detail, "race lifetime leak");
  }
  SetInboundStatusObserverForTesting(nullptr);
  return (g_inbound_ok.load() + g_inbound_invalid.load() == 16000 &&
          g_inbound_other.load() == 0) ||
         Fail(detail, "race producer outcome outside OK/INVALID_STATE");
}



bool g_inline_start_failure = false;
ClientTransport* MakeStartFailure(const TransportParams& p, ClientTransportDelegate* d) {
  auto* fake = static_cast<FakeTransport*>(MakeFake(p, d));
  fake->start_status = v8host_status_connect_failed;
  fake->inline_failure = g_inline_start_failure;
  return fake;
}
bool StartupRegistrationFailure(std::string* detail) {
  for (bool inline_failure : {false, true}) {
    g_inline_start_failure = inline_failure;
  SetTransportFactoryForTesting(MakeStartFailure);
  FakeTransport* fake = nullptr;
  v8host_session_s* session = Create(&fake);
  SetTransportFactoryForTesting(MakeFake);
  if (!session) return Fail(detail, "create admission");
  Recorder r; auto callbacks = Callbacks(&r);
  v8host_session_set_callbacks(session, &callbacks);
  auto input = BasicRun(); v8host_run_s* run = nullptr;
  if (StartRun(session, &input, &run) != v8host_status_ok) return Fail(detail, "prestart run");
  if (!PumpUntil([&] { return r.total.load() >= 3; }) || r.states.size() != 1 || r.events.size() != 1 ||
      r.disconnects != 1 || r.state_status[0] != v8host_status_connect_failed ||
      r.events[0] != v8host_run_event_failed || r.event_status[0] != v8host_status_connect_failed || !fake->closed)
    return Fail(detail, "startup outcome after registration");
  v8host_session_close(session); Drain();
  }
  return true;
}
bool ClientIdExhaustion(std::string* detail) {
  for (bool request : {false, true}) {
    FakeTransport* fake = nullptr; v8host_session_s* session = Create(&fake);
    Recorder r; auto callbacks = Callbacks(&r); v8host_session_set_callbacks(session, &callbacks);
    v8host::client::SetIdsForTesting(session, request ? UINT32_MAX : 2, request ? 1 : UINT32_MAX);
    auto input = BasicRun(); v8host_run_s* run = reinterpret_cast<v8host_run_s*>(1);
    size_t count = fake->FrameCount();
    if (StartRun(session, &input, &run) != v8host_status_quota_exceeded || run ||
        !PumpUntil([&] { return r.disconnects == 1; }) || r.states.size() != 1 ||
        r.state_status[0] != v8host_status_protocol_error || fake->FrameCount() != count)
      return Fail(detail, "id exhaustion must close without wrap/write");
    v8host_session_close(session); Drain();
  }
  return true;
}
bool AdmissionRollback(std::string* detail) {
  FakeTransport* fake = nullptr; v8host_session_s* session = Create(&fake);
  Recorder r; auto callbacks = Callbacks(&r); v8host_session_set_callbacks(session, &callbacks);
  auto input = BasicRun();
  fake->send_status = v8host_status_quota_exceeded;
  for (unsigned i = 0; i < 300; ++i) {
    v8host_run_s* run = reinterpret_cast<v8host_run_s*>(1);
    if (StartRun(session, &input, &run) != v8host_status_quota_exceeded || run)
      return Fail(detail, "enqueue rejection pending/run rollback");
  }
  fake->send_status = v8host_status_ok;
  v8host_run_s* run = nullptr;
  if (StartRun(session, &input, &run) != v8host_status_ok || !run)
    return Fail(detail, "admission after quota rejection");
  fake->send_status = v8host_status_quota_exceeded;
  if (v8host_run_post_message(run, v8host_message_kind_string, nullptr, 0) != v8host_status_quota_exceeded ||
      v8host_run_cancel(run) != v8host_status_quota_exceeded)
    return Fail(detail, "post/cancel admission status");
  Drain();
  if (r.disconnects || !r.events.empty()) return Fail(detail, "quota must not masquerade as connect loss");
  fake->send_status = v8host_status_ok; v8host_session_close(session); Drain();
  return true;
}

struct TransportProbe {
  HANDLE connected = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE disconnected = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE destroyed = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE inbound = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::atomic<int> connects{0}, losses{0}, frames{0};
  ULONGLONG connected_at = 0, disconnected_at = 0;
  TransportDisconnect reason = TransportDisconnect::kConnectFailed;
  uint32_t conn = 0;
  uint16_t major = 0, minor = 0;
  ~TransportProbe() {
    for (HANDLE h : {connected, disconnected, destroyed, inbound}) ::CloseHandle(h);
  }
};
struct ProbeDelegate : ClientTransportDelegate {
  explicit ProbeDelegate(TransportProbe* p) : probe(p) {}
  ~ProbeDelegate() override { ::SetEvent(probe->destroyed); }
  void OnConnected(uint32_t id, uint16_t major, uint16_t minor) override {
    probe->conn = id; probe->major = major; probe->minor = minor;
    probe->connected_at = ::GetTickCount64(); ++probe->connects;
    ::SetEvent(probe->connected);
  }
  void OnInboundFrame(const uint8_t*, size_t) override {
    ++probe->frames;
    if (close_on_frame) close_on_frame->Close();
    ::SetEvent(probe->inbound);
  }
  void OnDisconnect(TransportDisconnect reason) override {
    probe->reason = reason; probe->disconnected_at = ::GetTickCount64();
    ++probe->losses; ::SetEvent(probe->disconnected);
  }
  ClientTransport* close_on_frame = nullptr;
  TransportProbe* probe;
};
bool Signaled(HANDLE h) { return ::WaitForSingleObject(h, 5000) == WAIT_OBJECT_0; }


enum class LaunchHold { kObserve, kSlow, kExhausted, kClose };
bool g_launch_abort = false;

struct LaunchFixture {
  explicit LaunchFixture(int mode, LaunchHold hold) : mode(mode), hold(hold) {}
  TransportProbe probe;
  const int mode;
  const LaunchHold hold;
  HANDLE in_launch = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE returned = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::shared_ptr<ProbeDelegate> delegate = std::make_shared<ProbeDelegate>(&probe);
  std::unique_ptr<ClientTransport> transport;
  std::atomic<HANDLE> candidate{nullptr};
  std::atomic<DWORD> hook_error{ERROR_SUCCESS};
  std::atomic<int> before_count{0}, launching_count{0}, created_count{0}, returned_count{0};
  std::atomic<ULONGLONG> d0{0}, before_enter{0}, before_exit{0};
  std::atomic<ULONGLONG> launch_enter{0}, launch_exit{0}, shifted{0}, observed_at{0};

  void HoldUntil(ULONGLONG until) {
    ULONGLONG now = ::GetTickCount64();
    while (now < until &&
           ::WaitForSingleObject(release, static_cast<DWORD>(until - now)) == WAIT_TIMEOUT)
      now = ::GetTickCount64();
  }
  static void Hook(void* context, v8host::LaunchHookPoint point, HANDLE process,
                   ULONGLONG deadline, const wchar_t*) {
    auto& f = *static_cast<LaunchFixture*>(context);
    const ULONGLONG now = ::GetTickCount64();
    if (point == v8host::LaunchHookPoint::kBeforeCreate) {
      ++f.before_count;
      f.d0 = deadline; f.before_enter = now;
      if (f.hold == LaunchHold::kExhausted) f.HoldUntil(now + 5000);
      f.before_exit = ::GetTickCount64();
    } else if (point == v8host::LaunchHookPoint::kLaunching) {
      ++f.launching_count;
      f.launch_enter = now;
      if (f.hold == LaunchHold::kSlow)
        f.HoldUntil((std::min)(deadline + 500, now + 15000));
      else if (f.hold == LaunchHold::kExhausted)
        f.HoldUntil(now + 10000);
    } else if (point == v8host::LaunchHookPoint::kCreated) {
      if (++f.created_count == 1) {
        HANDLE duplicate = nullptr;
        const DWORD access = SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
            (f.mode == v8host_broker_mode_dedicated ? PROCESS_TERMINATE : 0);
        if (!::DuplicateHandle(::GetCurrentProcess(), process, ::GetCurrentProcess(),
                               &duplicate, access, FALSE, 0))
          f.hook_error = ::GetLastError();
        f.candidate = duplicate;
      }
      ::SetEvent(f.in_launch);
      if (f.hold == LaunchHold::kClose)
        f.HoldUntil(now + 15000);
      f.launch_exit = ::GetTickCount64();
    } else {
      ++f.returned_count;
      f.shifted = deadline; f.observed_at = now;
      ::SetEvent(f.returned);
    }
  }
  bool Init(std::string* detail) {
    if (!in_launch || !release || !returned || !probe.connected ||
        !probe.disconnected || !probe.destroyed || !probe.inbound)
      return Fail(detail, "launch fixture event creation");
    const std::wstring directory = v8host::test::ExecutableDirectory();
    if (mode == v8host_broker_mode_shared) {
      v8host::PayloadIdentity payload;
      std::vector<uint8_t> sid;
      LUID session = {};
      std::wstring endpoint;
      std::array<uint8_t, 32> key = {};
      DWORD error = ERROR_SUCCESS;
      if (!v8host::ResolvePayloadIdentity(directory, L"sbox.exe", L"v8host.dll", &payload, &error) ||
          !v8host::QueryCurrentSidAndSession(&sid, &session, &error) ||
          !v8host::DeriveEndpoint(sid, payload.plugin_set_id, payload.install_root, v8host::BrokerMode::kShared,
                                 nullptr, &endpoint, &key))
        return Fail(detail, "shared endpoint identity resolution");
      if (::WaitNamedPipeW(endpoint.c_str(), 0) || ::GetLastError() != ERROR_FILE_NOT_FOUND)
        return Fail(detail, "shared endpoint present");
    }
    TransportParams params;
    params.payload_directory = directory; params.broker_mode = mode;
    params.test_context = this; params.test_launch = Hook;
    transport.reset(v8host::client::CreateRealPipeClientTransport(params, delegate.get()));
    return transport ? true : Fail(detail, "real launch transport creation");
  }
  bool Start(std::string* detail) {
    if (transport->Start(delegate) != v8host_status_ok)
      return Fail(detail, "real launch transport Start");
    if (::WaitForSingleObject(in_launch, 30000) != WAIT_OBJECT_0)
      return Fail(detail, "launch hook did not start within 30 s");
    return candidate.load() && hook_error == ERROR_SUCCESS
        ? true : Fail(detail, "launch candidate handle duplication");
  }
  bool Hooks(std::string* detail) {
    if (before_count != 1 || launching_count != 1 || created_count != 1 || returned_count != 1)
      return Fail(detail, "launch hook counts must each be one");
    if (launch_enter < before_exit || launch_exit < launch_enter)
      return Fail(detail, "launch hook timestamp ordering");
    const ULONGLONG lower = d0 + (launch_exit - launch_enter);
    const ULONGLONG upper = d0 + (observed_at - before_exit);
    if (shifted < lower || shifted > upper)
      return Fail(detail, "launch deadline shift outside measured bracket");
    return true;
  }
  bool Cancelled() {
    DWORD code = STILL_ACTIVE;
    return ::WaitForSingleObject(candidate.load(), 0) == WAIT_OBJECT_0 &&
        ::GetExitCodeProcess(candidate.load(), &code) && code == ERROR_CANCELLED;
  }
  void ReapFailedDedicated() {
    HANDLE process = candidate.load();
    if (mode == v8host_broker_mode_dedicated && process &&
        ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
      ::TerminateProcess(process, 0xFA11);
      if (::WaitForSingleObject(process, 5000) != WAIT_OBJECT_0)
        g_launch_abort = true;
    }
  }
  bool Finish(bool passed, std::string* detail) {
    ::SetEvent(release);
    if (transport) transport->Close();
    transport.reset();
    delegate.reset();
    if (::WaitForSingleObject(probe.destroyed, 15000) != WAIT_OBJECT_0) {
      ReapFailedDedicated();
      g_launch_abort = true;
      *detail += " teardown timed out; fixture retained";
      return false;
    }
    HANDLE process = candidate.load();
    if (process) {
      if (passed && (probe.losses != (hold == LaunchHold::kExhausted ? 1 : 0) ||
                     probe.connects != (hold == LaunchHold::kClose ||
                                       hold == LaunchHold::kExhausted ? 0 : 1)))
        passed = Fail(detail, "late or duplicate launch callbacks");
      if (mode == v8host_broker_mode_dedicated) {
        if (passed && (hold == LaunchHold::kClose || hold == LaunchHold::kExhausted) && !Cancelled())
          passed = Fail(detail, "dedicated candidate not cancelled at destruction");
        if (::WaitForSingleObject(process, passed ? 8000 : 0) != WAIT_OBJECT_0) {
          passed = Fail(detail, "dedicated candidate did not exit");
          ReapFailedDedicated();
        }
      } else {
        if (passed && ::WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
          passed = Fail(detail, "shared candidate exited before idle grace");
        if (::WaitForSingleObject(process, 10000) != WAIT_OBJECT_0) {
          passed = Fail(detail, "shared candidate idle exit timed out");
          g_launch_abort = true;
        } else {
          DWORD code = STILL_ACTIVE;
          if (!::GetExitCodeProcess(process, &code) || code != 0)
            passed = Fail(detail, "shared candidate exit was not idle success");
        }
      }
      DWORD code = STILL_ACTIVE;
      ::GetExitCodeProcess(process, &code);
      *detail += " pid=" + std::to_string(::GetProcessId(process)) +
          " exit=" + std::to_string(code) + " shift=" + std::to_string(shifted - d0) +
          " bracket=[" + std::to_string(d0 + launch_exit - launch_enter) + "," +
          std::to_string(d0 + observed_at - before_exit) + "] D=" + std::to_string(shifted) +
          " hooks=" + std::to_string(before_count) + "/" + std::to_string(launching_count) +
          "/" + std::to_string(created_count) +
          "/" + std::to_string(returned_count);
    }
    delete this;
    return passed;
  }
  ~LaunchFixture() {
    if (candidate.load()) ::CloseHandle(candidate.load());
    for (HANDLE h : {in_launch, release, returned}) if (h) ::CloseHandle(h);
  }
};

bool LaunchCase(std::string* detail, int mode, LaunchHold hold) {
  auto* f = new LaunchFixture(mode, hold);
  const bool passed = [&] {
    if (!f->Init(detail) || !f->Start(detail)) return false;
    if (hold == LaunchHold::kClose) {
      const ULONGLONG close_start = ::GetTickCount64();
      f->transport->Close();
      const ULONGLONG close_end = ::GetTickCount64();
      ::SetEvent(f->release);
      if (close_end - close_start > 1000)
        return Fail(detail, "Close blocked during launch");
      const DWORD timeout = mode == v8host_broker_mode_shared ? 2000 : 5000;
      // Drop the test lease before observing background destruction.
      f->transport.reset(); f->delegate.reset();
      if (::WaitForSingleObject(f->probe.destroyed, timeout) != WAIT_OBJECT_0)
        return Fail(detail, "post-release destruction exceeded bound");
      if (!f->Hooks(detail)) return false;
      return f->probe.connects == 0 && f->probe.losses == 0
          ? true : Fail(detail, "callbacks after Close during launch");
    }
    if (::WaitForSingleObject(f->returned, 30000) != WAIT_OBJECT_0)
      return Fail(detail, "rendezvous did not return");
    if (!f->Hooks(detail)) return false;
    if (hold == LaunchHold::kExhausted) {
      if (::WaitForSingleObject(f->probe.disconnected, 5000) != WAIT_OBJECT_0 ||
          f->probe.connects != 0 || f->probe.losses != 1 ||
          f->probe.reason != TransportDisconnect::kConnectFailed ||
          f->probe.disconnected_at - f->launch_exit > 3000 || !f->Cancelled())
        return Fail(detail, "exhausted non-launch budget or joined cancellation");
    } else {
      if (::WaitForSingleObject(f->probe.connected, 5000) != WAIT_OBJECT_0 ||
          f->probe.connects != 1 || !f->probe.conn || f->probe.losses != 0 ||
          (hold == LaunchHold::kSlow && f->probe.connected_at - f->launch_exit > 5000))
        return Fail(detail, "real launch did not connect within startup budget");
    }
    return true;
  }();
  return f->Finish(passed, detail);
}

// Keep the process oracle independent of the broker policy under test.
constexpr ULONGLONG kExpectedPreConnectExpiryMs = 30000;
constexpr ULONGLONG kExpiryAliveMs = kExpectedPreConnectExpiryMs - 3000;
constexpr ULONGLONG kExpiryExitMs = kExpectedPreConnectExpiryMs + 3000;
// Cover report/readiness bounds, the exit wait, and a failure-release margin.
constexpr DWORD kExpiryReleaseMs = 30000 + 3000 + kExpiryExitMs + 5000;

struct ExpiryHandle {
  HANDLE value = nullptr;
  ~ExpiryHandle() { if (value) ::CloseHandle(value); }
  ExpiryHandle() = default;
  ExpiryHandle(const ExpiryHandle&) = delete;
  ExpiryHandle& operator=(const ExpiryHandle&) = delete;
};

bool ExpiryListenerReady(HANDLE broker, const std::wstring& endpoint,
                         ULONGLONG* ready_at, HANDLE helper = nullptr) {
  const ULONGLONG limit = ::GetTickCount64() + 3000;
  do {
    if (::WaitForSingleObject(broker, 0) != WAIT_TIMEOUT ||
        (helper && ::WaitForSingleObject(helper, 0) != WAIT_TIMEOUT))
      return false;
    // Readiness only: opening the pipe would consume the first accept.
    if (::WaitNamedPipeW(endpoint.c_str(), NMPWAIT_NOWAIT)) {
      *ready_at = ::GetTickCount64();
      return ::WaitForSingleObject(broker, 0) == WAIT_TIMEOUT;
    }
    ::WaitForSingleObject(broker, 10);
  } while (::GetTickCount64() < limit);
  return false;
}

bool ExpiryAliveUntil(HANDLE process, ULONGLONG limit) {
  const ULONGLONG now = ::GetTickCount64();
  return ::WaitForSingleObject(process, now < limit ? static_cast<DWORD>(limit - now) : 0) ==
      WAIT_TIMEOUT;
}

bool ExpiryExitZero(HANDLE process, ULONGLONG limit) {
  const ULONGLONG now = ::GetTickCount64();
  DWORD code = STILL_ACTIVE;
  return ::WaitForSingleObject(process, now < limit ? static_cast<DWORD>(limit - now) : 0) ==
             WAIT_OBJECT_0 &&
      ::GetExitCodeProcess(process, &code) && code == 0;
}

void ExpiryFailureReap(HANDLE observe, HANDLE cleanup) {
  if (observe && ::WaitForSingleObject(observe, 5000) != WAIT_OBJECT_0) {
    if (!cleanup || !::TerminateProcess(cleanup, 0xFA11) ||
        ::WaitForSingleObject(observe, 5000) != WAIT_OBJECT_0)
      g_launch_abort = true;
  }
}

struct PreConnectFixture {
  TransportProbe probe;
  ExpiryHandle broker, cleanup;
  ExpiryHandle ready, release;
  std::wstring endpoint;
  std::atomic<DWORD> error{ERROR_SUCCESS};
  std::atomic<int> creates{0};
  std::shared_ptr<ProbeDelegate> delegate = std::make_shared<ProbeDelegate>(&probe);
  std::unique_ptr<ClientTransport> transport;
  static void Hook(void* context, v8host::LaunchHookPoint point, HANDLE process,
                   ULONGLONG, const wchar_t* endpoint) {
    if (point != v8host::LaunchHookPoint::kCreated) return;
    auto& f = *static_cast<PreConnectFixture*>(context);
    ++f.creates;
    f.endpoint = endpoint;
    if (!::DuplicateHandle(::GetCurrentProcess(), process, ::GetCurrentProcess(),
            &f.broker.value, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0) ||
        !::DuplicateHandle(::GetCurrentProcess(), process, ::GetCurrentProcess(),
            &f.cleanup.value, PROCESS_TERMINATE, FALSE, 0))
      f.error = ::GetLastError();
    ::SetEvent(f.ready.value);
    if (::WaitForSingleObject(f.release.value, kExpiryReleaseMs) != WAIT_OBJECT_0)
      f.error = ERROR_TIMEOUT;
  }
  bool Start(std::string* detail) {
    ready.value = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release.value = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready.value || !release.value) return Fail(detail, "expiry fixture events");
    TransportParams params;
    params.payload_directory = v8host::test::ExecutableDirectory();
    params.broker_mode = v8host_broker_mode_dedicated;
    params.test_context = this; params.test_launch = Hook;
    transport.reset(v8host::client::CreateRealPipeClientTransport(params, delegate.get()));
    if (!transport || transport->Start(delegate) != v8host_status_ok ||
        ::WaitForSingleObject(ready.value, 30000) != WAIT_OBJECT_0 ||
        error != ERROR_SUCCESS || !broker.value || !cleanup.value)
      return Fail(detail, "expiry launch/report failed");
    return true;
  }
  bool Finish(bool passed, std::string* detail) {
    if (release.value) ::SetEvent(release.value);
    if (transport) transport->Close();
    transport.reset(); delegate.reset();
    if (::WaitForSingleObject(probe.destroyed, 15000) != WAIT_OBJECT_0) {
      ExpiryFailureReap(broker.value, cleanup.value);
      g_launch_abort = true;
      *detail += " expiry teardown timeout; fixture retained";
      return false;
    }
    if (passed && (error != ERROR_SUCCESS || creates != 1 || probe.connects != 0))
      passed = Fail(detail, "expiry hook/callback count");
    ExpiryFailureReap(broker.value, cleanup.value);
    delete this;
    return passed;
  }
};

bool DedicatedNoClientExpires(std::string* detail) {
  auto* f = new PreConnectFixture;
  const bool passed = [&] {
    if (!f->Start(detail)) return false;
    ULONGLONG ready_at = 0;
    if (!ExpiryListenerReady(f->broker.value, f->endpoint, &ready_at))
      return Fail(detail, "dedicated listener not ready/alive");
    if (!ExpiryAliveUntil(f->broker.value, ready_at + kExpiryAliveMs))
      return Fail(detail, "unconnected dedicated broker expired before readiness + 27000 ms");
    if (!ExpiryExitZero(f->broker.value, ready_at + kExpiryExitMs))
      return Fail(detail, "unconnected dedicated broker did not naturally exit0 within 33000 ms");
    *detail = "pid=" + std::to_string(::GetProcessId(f->broker.value)) +
        " exit=0 connects=" + std::to_string(f->probe.connects.load()) +
        " ready-to-exit=" + std::to_string(::GetTickCount64() - ready_at);
    return f->probe.connects == 0;
  }();
  return f->Finish(passed, detail);
}

constexpr uint32_t kOrphanReportMagic = 0x50584f52;
struct OrphanReport {
  uint32_t magic = kOrphanReportMagic;
  uint32_t size = sizeof(OrphanReport);
  uint32_t pid = 0, connects = 0, error = ERROR_SUCCESS;
  uint64_t broker = 0, cleanup = 0;
  wchar_t endpoint[512] = {};
};

struct OrphanHelper {
  HANDLE report, release, parent;
  static void Hook(void* context, v8host::LaunchHookPoint point, HANDLE process,
                   ULONGLONG, const wchar_t* endpoint) {
    if (point != v8host::LaunchHookPoint::kCreated) return;
    auto& f = *static_cast<OrphanHelper*>(context);
    OrphanReport report;
    HANDLE broker = nullptr, cleanup = nullptr;
    report.pid = ::GetProcessId(process);
    if (!::DuplicateHandle(::GetCurrentProcess(), process, f.parent, &broker,
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0) ||
        !::DuplicateHandle(::GetCurrentProcess(), process, f.parent, &cleanup,
            PROCESS_TERMINATE, FALSE, 0))
      report.error = ::GetLastError();
    report.broker = reinterpret_cast<uintptr_t>(broker);
    report.cleanup = reinterpret_cast<uintptr_t>(cleanup);
    if (wcslen(endpoint) >= std::size(report.endpoint))
      report.error = ERROR_BUFFER_OVERFLOW;
    else
      wcscpy_s(report.endpoint, endpoint);
    DWORD written = 0;
    const bool sent = ::WriteFile(f.report, &report, sizeof(report), &written, nullptr) &&
        written == sizeof(report);
    if (sent) ::WaitForSingleObject(f.release, kExpiryReleaseMs);
    // Exit without unwinding the unconnected candidate guard or calling Close.
    ::ExitProcess(sent && report.error == ERROR_SUCCESS ? 0 : 2);
  }
};

HANDLE OrphanArgument(const char* text) {
  if (!text || *text < '0' || *text > '9') return nullptr;
  char* end = nullptr;
  errno = 0;
  const unsigned long long value = std::strtoull(text, &end, 10);
  DWORD flags = 0;
  HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
  return !errno && end && !*end && value && value <= UINTPTR_MAX &&
      ::GetHandleInformation(handle, &flags) ? handle : nullptr;
}

int RunOrphanHelper(int argc, char** argv) {
  if (argc != 5) return 2;
  OrphanHelper helper{OrphanArgument(argv[2]), OrphanArgument(argv[3]), OrphanArgument(argv[4])};
  if (!helper.report || !helper.release || !helper.parent ||
      ::GetFileType(helper.report) != FILE_TYPE_PIPE) return 2;
  v8host::BrokerRendezvous rendezvous(v8host::test::ExecutableDirectory(),
                                    v8host::BrokerMode::kDedicated);
  rendezvous.SetLaunchHookForTesting(OrphanHelper::Hook, &helper);
  v8host::BrokerConnection connection;
  rendezvous.ConnectOrLaunch(&connection);
  return 2;  // A successful helper exits directly from the pre-connect hook.
}

struct OrphanFixture {
  ExpiryHandle reader, writer, release, parent, helper, broker, cleanup;
  OrphanReport report;
  bool Start(std::string* detail) {
    SECURITY_ATTRIBUTES inherited = {sizeof(inherited), nullptr, TRUE};
    if (!::CreatePipe(&reader.value, &writer.value, &inherited, 4096) ||
        !::SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0))
      return Fail(detail, "orphan report pipe creation");
    release.value = ::CreateEventW(&inherited, TRUE, FALSE, nullptr);
    parent.value = ::OpenProcess(PROCESS_DUP_HANDLE, TRUE, ::GetCurrentProcessId());
    if (!release.value || !parent.value)
      return Fail(detail, "orphan inherited capabilities");
    SIZE_T bytes = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<uint8_t> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
      return Fail(detail, "orphan handle-list initialization");
    HANDLE handles[] = {writer.value, release.value, parent.value};
    bool ok = ::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        handles, sizeof(handles), nullptr, nullptr) != FALSE;
    std::vector<wchar_t> executable(32768);
    const DWORD length = ::GetModuleFileNameW(nullptr, executable.data(),
                                             static_cast<DWORD>(executable.size()));
    ok = ok && length && length < executable.size();
    std::wstring command = L"\"" + std::wstring(executable.data()) + L"\" --px-orphan-helper";
    for (HANDLE handle : handles)
      command += L" " + std::to_wstring(reinterpret_cast<uintptr_t>(handle));
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process = {};
    const std::wstring directory = v8host::test::ExecutableDirectory();
    if (ok) ok = ::CreateProcessW(executable.data(), command.data(), nullptr, nullptr,
        TRUE, EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(),
        &startup.StartupInfo, &process) != FALSE;
    ::DeleteProcThreadAttributeList(attributes);
    if (!ok) return Fail(detail, "normal orphan helper launch");
    helper.value = process.hProcess;
    ::CloseHandle(process.hThread);
    ::CloseHandle(writer.value); writer.value = nullptr;
    const ULONGLONG limit = ::GetTickCount64() + 30000;
    DWORD available = 0;
    while (::PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr) &&
           available < sizeof(report) && ::GetTickCount64() < limit &&
           ::WaitForSingleObject(helper.value, 10) == WAIT_TIMEOUT) {}
    DWORD read = 0;
    if (available != sizeof(report) ||
        !::ReadFile(reader.value, &report, sizeof(report), &read, nullptr) || read != sizeof(report))
      return Fail(detail, "orphan helper bounded report framing");
    if (report.magic != kOrphanReportMagic || report.size != sizeof(report) ||
        report.broker > UINTPTR_MAX || report.cleanup > UINTPTR_MAX)
      return Fail(detail, "orphan helper report header");
    broker.value = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(report.broker));
    cleanup.value = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(report.cleanup));
    DWORD flags = 0;
    if (report.error || report.connects || !report.pid || !broker.value || !cleanup.value ||
        !::GetHandleInformation(broker.value, &flags) ||
        !::GetHandleInformation(cleanup.value, &flags) ||
        ::GetProcessId(broker.value) != report.pid || !report.endpoint[0] ||
        report.endpoint[std::size(report.endpoint) - 1] ||
        wcsncmp(report.endpoint, L"\\\\.\\pipe\\", 9) != 0)
      return Fail(detail, "orphan helper report identity/capability validation");
    return true;
  }
  bool Finish(bool passed, std::string* detail) {
    if (release.value) ::SetEvent(release.value);
    if (helper.value && !ExpiryExitZero(helper.value, ::GetTickCount64() + 5000)) {
      passed = Fail(detail, "orphan helper did not exit normally within bound");
      if (::WaitForSingleObject(helper.value, 0) != WAIT_OBJECT_0) g_launch_abort = true;
    }
    ExpiryFailureReap(broker.value, cleanup.value);
    return passed;
  }
};

bool DedicatedClientExitOrphan(std::string* detail) {
  OrphanFixture f;
  const bool passed = [&] {
    if (!f.Start(detail)) return false;
    ULONGLONG ready_at = 0;
    if (!ExpiryListenerReady(f.broker.value, f.report.endpoint, &ready_at, f.helper.value))
      return Fail(detail, "orphan listener/helper/broker not alive before client exit");
    if (!::SetEvent(f.release.value) ||
        !ExpiryExitZero(f.helper.value, ::GetTickCount64() + 3000))
      return Fail(detail, "orphan helper successful process death");
    if (!ExpiryAliveUntil(f.broker.value, ready_at + kExpiryAliveMs))
      return Fail(detail, "orphan broker expired before readiness + 27000 ms");
    if (::WaitForSingleObject(f.broker.value, 0) != WAIT_TIMEOUT)
      return Fail(detail, "orphan broker did not outlive helper");
    if (!ExpiryExitZero(f.broker.value, ready_at + kExpiryExitMs))
      return Fail(detail, "orphan broker did not naturally exit0 within 33000 ms");
    *detail = "helper_pid=" + std::to_string(::GetProcessId(f.helper.value)) +
        " helper_exit=0 broker_pid=" + std::to_string(f.report.pid) +
        " broker_exit=0 ready->client-exit->broker-exit=" +
        std::to_string(::GetTickCount64() - ready_at) + "ms";
    return true;
  }();
  return f.Finish(passed, detail);
}

struct PumpFixture {
  TransportProbe probe;
  HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
  HANDLE gate = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE read_pending = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE write_pending = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::shared_ptr<ProbeDelegate> delegate = std::make_shared<ProbeDelegate>(&probe);
  std::unique_ptr<ClientTransport> transport;
  std::atomic<int> attempts{0};
  std::vector<v8host::RendezvousStatus> results;
  bool started = false;
  ULONGLONG observed_deadline = 0;
  bool consistent_deadline = true;
  static v8host::RendezvousStatus Connect(void* context, v8host::BrokerConnection* conn,
                                         v8host::HelloResult* hello, HANDLE stop, ULONGLONG deadline) {
    auto& f = *static_cast<PumpFixture*>(context);
    int attempt = f.attempts.fetch_add(1);
    if (!f.observed_deadline) f.observed_deadline = deadline;
    f.consistent_deadline &= f.observed_deadline == deadline;
    ::SetEvent(f.entered);
    HANDLE events[] = {stop, f.gate};
    ULONGLONG now = ::GetTickCount64();
    DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
    if (::WaitForMultipleObjects(2, events, FALSE, remaining) != WAIT_OBJECT_0 + 1)
      return v8host::RendezvousStatus::kStartTimeout;
    auto status = attempt < static_cast<int>(f.results.size()) ? f.results[attempt] : v8host::RendezvousStatus::kOk;
    if (status != v8host::RendezvousStatus::kOk) return status;
    conn->AdoptForTesting(f.client); f.client = INVALID_HANDLE_VALUE;
    hello->conn_id = 37; hello->selected_major = 1; hello->selected_minor = 7;
    return status;
  }
  static void Pending(void* context, bool write) {
    auto& f = *static_cast<PumpFixture*>(context);
    ::SetEvent(write ? f.write_pending : f.read_pending);
  }
  bool Init(int mode = v8host_broker_mode_shared) {
    static std::atomic<unsigned> serial{0};
    std::wstring name = LR"(\\.\pipe\v8host-client-test-)" + std::to_wstring(::GetCurrentProcessId()) +
        L"-" + std::to_wstring(++serial);
    server = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    if (server == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED accept = {}; accept.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL accepted = ::ConnectNamedPipe(server, &accept);
    DWORD error = ::GetLastError();
    client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    DWORD ignored = 0;
    bool ok = client != INVALID_HANDLE_VALUE &&
        (accepted || error == ERROR_PIPE_CONNECTED || (error == ERROR_IO_PENDING &&
          Signaled(accept.hEvent) && ::GetOverlappedResult(server, &accept, &ignored, FALSE)));
    if (!ok) { ::CancelIoEx(server, &accept); ::GetOverlappedResult(server, &accept, &ignored, TRUE); }
    ::CloseHandle(accept.hEvent);
    if (!ok) return false;
    DWORD readmode = PIPE_READMODE_MESSAGE;
    if (!::SetNamedPipeHandleState(client, &readmode, nullptr, nullptr)) return false;
    TransportParams p; p.broker_mode = mode; p.test_context = this;
    p.test_connect = Connect; p.test_pending = Pending;
    transport.reset(v8host::client::CreateRealPipeClientTransport(p, delegate.get()));
    return transport != nullptr;
  }
  bool Start() {
    started = transport->Start(delegate) == v8host_status_ok;
    return started && Signaled(entered);
  }
  bool Finish() {
    transport->Close(); transport->Close();
    delegate.reset();
    return !started || Signaled(probe.destroyed);
  }
  ~PumpFixture() {
    if (transport) Finish();
    if (server != INVALID_HANDLE_VALUE) ::CloseHandle(server);
    if (client != INVALID_HANDLE_VALUE) ::CloseHandle(client);
    for (HANDLE h : {gate, entered, read_pending, write_pending}) ::CloseHandle(h);
  }
  bool Io(bool write, std::vector<uint8_t>* bytes) {
    OVERLAPPED op = {}; op.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD size = 0;
    BOOL ok = write ? ::WriteFile(server, bytes->data(), static_cast<DWORD>(bytes->size()), nullptr, &op)
                    : ::ReadFile(server, bytes->data(), static_cast<DWORD>(bytes->size()), nullptr, &op);
    if (!ok && ::GetLastError() == ERROR_IO_PENDING)
      ok = Signaled(op.hEvent) && ::GetOverlappedResult(server, &op, &size, FALSE);
    else if (ok) ok = ::GetOverlappedResult(server, &op, &size, FALSE);
    if (!ok) { ::CancelIoEx(server, &op); ::GetOverlappedResult(server, &op, &size, TRUE); }
    ::CloseHandle(op.hEvent);
    if (ok && !write) bytes->resize(size);
    return ok != FALSE;
  }
  v8host_status Send(const std::vector<uint8_t>& bytes) {
    return transport->SendFrame(bytes.data(), bytes.size());
  }
};

PumpFixture* g_client_pipe = nullptr;
ClientTransport* MakeClientPipe(const TransportParams& params, ClientTransportDelegate* delegate) {
  auto p = params; p.test_context = g_client_pipe;
  p.test_connect = PumpFixture::Connect; p.test_pending = PumpFixture::Pending;
  return v8host::client::CreateRealPipeClientTransport(p, delegate);
}
bool RealClientFailurePaths(std::string* detail) {
  for (int path = 0; path < 4; ++path) {
    PumpFixture f; if (!f.Init()) return Fail(detail, "client pipe pair");
    g_client_pipe = &f; SetTransportFactoryForTesting(&MakeClientPipe);
    struct ResetFactory { ~ResetFactory() { SetTransportFactoryForTesting(&MakeFake); g_client_pipe = nullptr; } } reset;
    if (path == 0) f.results = {v8host::RendezvousStatus::kProtocolFailed};
    v8host_session_s* session = Create();
    if (!session) return Fail(detail, "real transport client create");
    struct CloseSession { v8host_session_s* session; ~CloseSession() { if (session) v8host_session_close(session); } } cleanup{session};
    Recorder r; auto callbacks = Callbacks(&r); v8host_session_set_callbacks(session, &callbacks);
    auto input = BasicRun(); v8host_run_s* run = nullptr;
    if (StartRun(session, &input, &run) != v8host_status_ok) return Fail(detail, "real transport client start");
    ::SetEvent(f.gate);
    if (!PumpUntil([&] { return ::WaitForSingleObject(f.entered, 0) == WAIT_OBJECT_0; }))
      return Fail(detail, "real client scheduled startup");
    if (path != 0) {
      std::vector<uint8_t> frame(kMaxFrameSize);
      FrameHeader h;
      for (int n = 0; n < 2; ++n) {
        frame.resize(kMaxFrameSize);
        if (!f.Io(false, &frame) || !DecodeFrame(frame, &h, nullptr, nullptr) ||
            h.conn_id != 37 || h.version_minor != 7 || h.request_id != static_cast<uint32_t>(n + 2))
          return Fail(detail, "real client negotiated CREATE/START FIFO");
      }
      if (!Signaled(f.read_pending)) return Fail(detail, "real client pending read");
      if (path == 1) { ::CloseHandle(f.server); f.server = INVALID_HANDLE_VALUE; }
      else {
        h.request_id = 0; h.version_minor = path == 3 ? 6 : 7;
        frame = BuildResultFrame(h, ResultDisposition::kCompleted);
        if (path == 2) frame.pop_back();
        if (!f.Io(true, &frame)) return Fail(detail, "real client invalid inbound write");
      }
    }
    if (!PumpUntil([&] { return r.disconnects == 1; })) return Fail(detail, "real client failure callbacks");
    Drain();
    const v8host_status expected = path == 0 ? v8host_status_connect_failed : path == 1 ? v8host_status_broker_lost : v8host_status_protocol_error;
    if (r.events.size() != 1 || r.event_status != std::vector<v8host_status>{expected} ||
        r.events[0] != (path == 1 ? v8host_run_event_broker_lost : v8host_run_event_failed) ||
        r.state_status != std::vector<v8host_status>{expected} || r.states != std::vector<int32_t>{v8host_session_state_closed} ||
        r.run_thread != ::GetCurrentThreadId() || r.disconnect_thread != ::GetCurrentThreadId() ||
        f.attempts != 1 || v8host_run_cancel(run) != v8host_status_run_terminal)
      return Fail(detail, "real client failure once/app-thread/no replay");
    v8host_session_close(session); cleanup.session = nullptr; Drain();
  }
  return true;
}

std::vector<uint8_t> SizedTransportFrame(size_t len, bool relay, uint32_t run = 1) {
  FrameHeader h; h.version_major = kWireVersionMajor;
  h.type = relay ? MessageType::RELAY_TO_WORKER : MessageType::START_RUN;
  h.target_id = 1; h.run_id = run; h.request_id = relay ? 0 : run + 1;
  h.payload_length = static_cast<uint32_t>(len - kFrameHeaderSize);
  std::vector<uint8_t> bytes; EncodeHeader(h, bytes); bytes.resize(len, 0x61);
  return bytes;
}
bool TransportFifo(std::string* detail) {
  PumpFixture f;
  if (!f.Init()) return Fail(detail, "pipe pair");
  FrameHeader header; header.version_major = kWireVersionMajor; header.target_id = 1; header.request_id = 2;
  CreateSessionPayload config; config.broker_mode = 1; config.prohibit_dynamic_code = true;
  auto create = BuildCreateSessionFrame(header, config);
  header.request_id = 3; header.run_id = 1;
  StartRunPayload inputs; inputs.guest_payload = {'x'};
  auto start = BuildStartRunFrame(header, inputs);
  header.request_id = 0; header.type = MessageType::RELAY_TO_WORKER;
  const uint8_t message[] = {'a', 'b'};
  auto relay = BuildRelayFrame(header, -7, message, sizeof(message));
  const std::vector<std::vector<uint8_t>> expected = {create, start, relay};
  const MessageType types[] = {MessageType::CREATE_SESSION, MessageType::START_RUN, MessageType::RELAY_TO_WORKER};
  if (f.Send(create) || f.Send(start) || f.Send(relay) || !f.Start()) return Fail(detail, "preconnect admission");
  std::fill(start.begin(), start.end(), 0xFF);
  DWORD available = 1;
  if (!::PeekNamedPipe(f.server, nullptr, 0, nullptr, &available, nullptr) || available)
    return Fail(detail, "application write before ACK");
  ::SetEvent(f.gate);
  if (!Signaled(f.probe.connected)) return Fail(detail, "connected publication");
  for (unsigned i = 0; i < 3; ++i) {
    std::vector<uint8_t> bytes(kMaxFrameSize);
    FrameHeader h;
    if (!f.Io(false, &bytes) || DecodeAndValidateFrame(bytes.data(), bytes.size(), &h) != DecodeStatus::kOk ||
        h.conn_id != 37 || h.version_major != 1 || h.version_minor != 7 ||
        h.type != types[i] || h.request_id != (i == 2 ? 0 : i + 2) || bytes.size() != expected[i].size() ||
        !std::equal(bytes.begin() + kFrameHeaderSize, bytes.end(), expected[i].begin() + kFrameHeaderSize))
      return Fail(detail, "FIFO/stamping/deep copy");
  }
  if (!f.Finish() || f.transport->QueuedBytesForTesting() || f.probe.losses ||
      f.Send(create) != v8host_status_connect_failed || f.transport->Start(f.delegate) != v8host_status_connect_failed)
    return Fail(detail, "closed transport reuse/cleanup");
  return true;
}

bool TransportReadWriteStop(std::string* detail) {
  PumpFixture f;
  if (!f.Init() || !f.Start()) return Fail(detail, "start");
  ::SetEvent(f.gate);
  if (!Signaled(f.read_pending)) return Fail(detail, "genuine pending read");
  auto bytes = SizedTransportFrame(kMaxFrameSize, true);
  if (f.Send(bytes) || !Signaled(f.write_pending) || f.transport->QueuedBytesForTesting() != bytes.size())
    return Fail(detail, "pending write/inflight credit");
  // Both pending operations must be drained before the delegate lease drops.
  if (!f.Finish() || f.probe.losses || f.transport->QueuedBytesForTesting())
    return Fail(detail, "cancel/drain/lease");
  return true;
}


bool TransportWriteLoss(std::string* detail) {
  PumpFixture f;
  if (!f.Init() || !f.Start()) return Fail(detail, "pair/start");
  ::SetEvent(f.gate);
  if (!Signaled(f.read_pending)) return Fail(detail, "pending read");
  auto bytes = SizedTransportFrame(kMaxFrameSize, true);
  if (f.Send(bytes) || !Signaled(f.write_pending)) return Fail(detail, "pending write");
  ::CloseHandle(f.server); f.server = INVALID_HANDLE_VALUE;
  if (!Signaled(f.probe.disconnected) || f.probe.losses != 1 ||
      f.probe.reason != TransportDisconnect::kBrokerLost || f.attempts != 1 ||
      f.Send(bytes) != v8host_status_connect_failed || !f.Finish() || f.transport->QueuedBytesForTesting())
    return Fail(detail, "ambiguous write loss must drain, terminalize once, never replay");
  return true;
}

bool FillTransport(PumpFixture& f, size_t total, bool relay, uint32_t run) {
  while (total) {
    size_t len = (std::min)(total, size_t{kMaxFrameSize});
    if (total > len && total - len < 36) len -= 36;
    if (f.Send(SizedTransportFrame(len, relay, run)) != v8host_status_ok) return false;
    total -= len;
  }
  return true;
}
bool TransportQueueBoundaries(std::string* detail) {
  for (size_t offset : {size_t{0}, size_t{1}}) {
    PumpFixture f;
    if (!f.Init() || !FillTransport(f, kMaxControlQueueBytes - offset, false, 1) ||
        f.Send(SizedTransportFrame(32, false)) != v8host_status_quota_exceeded ||
        f.transport->QueuedBytesForTesting() != kMaxControlQueueBytes - offset)
      return Fail(detail, "control byte boundary");
  }
  {
    PumpFixture f; if (!f.Init()) return Fail(detail, "pair");
    for (unsigned i = 0; i < kMaxControlQueueRequests; ++i)
      if (f.Send(SizedTransportFrame(32, false)) != v8host_status_ok) return Fail(detail, "control count cap");
    if (f.Send(SizedTransportFrame(32, false)) != v8host_status_quota_exceeded) return Fail(detail, "control count +1");
  }
  for (size_t offset : {size_t{0}, size_t{1}}) {
    PumpFixture f; if (!f.Init()) return Fail(detail, "pair");
    if (!FillTransport(f, kMaxQueuedRelayBytesPerRun - offset, true, 1) ||
        f.Send(SizedTransportFrame(36, true, 1)) != v8host_status_quota_exceeded ||
        f.transport->QueuedBytesForTesting() != kMaxQueuedRelayBytesPerRun - offset)
      return Fail(detail, "run byte boundary");
  }
  PumpFixture f; if (!f.Init()) return Fail(detail, "pair");
  for (unsigned run = 1; run <= 16; ++run)
    if (!FillTransport(f, kMaxQueuedRelayBytesPerRun, true, run)) return Fail(detail, "connection cap");
  if (f.transport->QueuedBytesForTesting() != kMaxQueuedRelayBytesPerConnection ||
      f.Send(SizedTransportFrame(36, true, 17)) != v8host_status_quota_exceeded || !f.Start())
    return Fail(detail, "connection +1");
  if (!f.Finish() || f.transport->QueuedBytesForTesting()) return Fail(detail, "stop queue credit");
  return true;
}

bool TransportMalformed(std::string* detail) {
  for (int variant = 0; variant < 3; ++variant) {
    PumpFixture f; if (!f.Init() || !f.Start()) return Fail(detail, "pair/start");
    ::SetEvent(f.gate);
    if (!Signaled(f.read_pending)) return Fail(detail, "pending read");
    std::vector<uint8_t> bytes = variant == 0 ? std::vector<uint8_t>{1, 2, 3}
        : SizedTransportFrame(variant == 1 ? 33 : kMaxFrameSize, false);
    if (variant == 1) bytes.pop_back();
    if (variant == 2) bytes.push_back(0);
    if (!f.Io(true, &bytes) || !Signaled(f.probe.disconnected) || f.probe.losses != 1 ||
        f.probe.reason != TransportDisconnect::kProtocolError || !f.Finish() || f.attempts != 1)
      return Fail(detail, "malformed/partial/oversized disconnect");
  }
  return true;
}

bool TransportRetry(std::string* detail) {
  using Status = v8host::RendezvousStatus;
  for (auto status : {Status::kIoFailed, Status::kProtocolFailed, Status::kPeerAuthenticationFailed, Status::kStartTimeout}) {
    for (int mode : {v8host_broker_mode_shared, v8host_broker_mode_dedicated}) {
      PumpFixture f; f.results = {status};
      if (!f.Init(mode) || !f.Start()) return Fail(detail, "script start");
      ::SetEvent(f.gate);
      bool retry = status == Status::kIoFailed && mode == v8host_broker_mode_shared;
      if (!Signaled(retry ? f.probe.connected : f.probe.disconnected) ||
          f.attempts != (retry ? 2 : 1)) return Fail(detail, "preack retry policy");
      if (retry) {
        ::CloseHandle(f.server); f.server = INVALID_HANDLE_VALUE;
        if (!Signaled(f.probe.disconnected) || f.probe.reason != TransportDisconnect::kBrokerLost || f.attempts != 2)
          return Fail(detail, "postack no reconnect");
      }
      if (!f.Finish() || f.probe.losses != 1) return Fail(detail, "sole disconnect");
    }
  }
  PumpFixture expired; expired.results.assign(1000, Status::kIoFailed);
  if (!expired.Init() || !expired.Start()) return Fail(detail, "deadline start");
  ::SetEvent(expired.gate);
  if (::WaitForSingleObject(expired.probe.disconnected, 7000) != WAIT_OBJECT_0 ||
      expired.attempts < 2 || !expired.consistent_deadline || !expired.Finish() || expired.probe.losses != 1)
    return Fail(detail, "one overall preack deadline");
  PumpFixture stopped; if (!stopped.Init() || !stopped.Start() || !stopped.Finish() || stopped.probe.losses)
    return Fail(detail, "cancelled connect");
  return true;
}


HANDLE g_first_admitted = nullptr, g_second_attempted = nullptr;
std::atomic<int> g_submission_before{0}, g_submission_after{0};
bool g_hold_before_submission = false;
std::atomic<bool> g_order_barrier_ok{true};
void BeforeSubmission() {
  const int count = ++g_submission_before;
  if (g_hold_before_submission && count == 1) {
    ::SetEvent(g_first_admitted);
    if (::WaitForSingleObject(g_second_attempted, 5000) != WAIT_OBJECT_0) g_order_barrier_ok = false;
  } else if (!g_hold_before_submission && count == 2) {
    ::SetEvent(g_second_attempted);
  }
}
void AfterSubmission() {
  if (++g_submission_after == 1 && !g_hold_before_submission) {
    ::SetEvent(g_first_admitted);
    if (::WaitForSingleObject(g_second_attempted, 5000) != WAIT_OBJECT_0) g_order_barrier_ok = false;
  }
}
bool ControlIdSendOrder(std::string* detail) {
  for (bool before : {false, true}) {
  g_hold_before_submission = before;
  FakeTransport* fake = nullptr; v8host_session_s* session = Create(&fake);
  Recorder r; auto callbacks = Callbacks(&r); v8host_session_set_callbacks(session, &callbacks);
  auto input = BasicRun(); v8host_run_s* run = nullptr;
  if (StartRun(session, &input, &run) != v8host_status_ok) return Fail(detail, "start");
  g_first_admitted = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_second_attempted = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_submission_before = g_submission_after = 0;
  g_order_barrier_ok = g_first_admitted && g_second_attempted;
  v8host::client::SetSubmissionHooksForTesting(BeforeSubmission, AfterSubmission);
  v8host_status first = v8host_status_internal_error, second = v8host_status_internal_error;
  std::thread a([&] { first = v8host::client::SubmitCancelForTesting(session, 1); });
  bool entered = Signaled(g_first_admitted);
  std::thread b([&] {
    second = v8host::client::SubmitCancelForTesting(session, 1);
    if (before) ::SetEvent(g_second_attempted);
  });
  a.join(); b.join();
  v8host::client::SetSubmissionHooksForTesting(nullptr, nullptr);
  ::CloseHandle(g_first_admitted); ::CloseHandle(g_second_attempted);
  bool ok = entered && g_order_barrier_ok && first == v8host_status_ok && second == v8host_status_ok && g_submission_before == 2 && g_submission_after == 2;
  for (unsigned i = 2; i < 4; ++i) {
    FrameHeader h; auto frame = fake->Frame(i);
    ok &= DecodeAndValidateFrame(frame.data(), frame.size(), &h) == DecodeStatus::kOk &&
        h.request_id == i + 2 && h.type == MessageType::CANCEL_RUN;
    fake->Inbound(AckFor(frame));
  }
  Drain(); ok &= r.disconnects == 0;
  v8host_session_close(session); Drain();
  if (!ok) return Fail(detail, "barrier-controlled allocation/reservation/enqueue order");
  }
  return true;
}
bool TransportCallbackClose(std::string* detail) {
  PumpFixture f; if (!f.Init() || !f.Start()) return Fail(detail, "pair/start");
  f.delegate->close_on_frame = f.transport.get();
  ::SetEvent(f.gate);
  if (!Signaled(f.read_pending)) return Fail(detail, "read pending");
  auto bytes = SizedTransportFrame(32, false);
  if (!f.Io(true, &bytes) || !Signaled(f.probe.inbound) || !f.Finish() || f.probe.frames != 1 || f.probe.losses ||
      f.Send(bytes) != v8host_status_connect_failed || f.transport->QueuedBytesForTesting())
    return Fail(detail, "callback close/lease/no replay");
  return true;
}
bool HandshakeRejects(std::string* detail) {
  for (int variant = 0; variant < 5; ++variant) {
    PumpFixture f; if (!f.Init()) return Fail(detail, "pair");
    v8host::BrokerConnection conn; conn.AdoptForTesting(f.client); f.client = INVALID_HANDLE_VALUE;
    v8host::HelloResult hello;
    v8host::RendezvousStatus status = v8host::RendezvousStatus::kOk;
    std::thread handshake([&] { status = conn.Handshake(1, &hello); });
    std::vector<uint8_t> request(kMaxFrameSize);
    bool ok = f.Io(false, &request);
    FrameHeader request_header;
    ok &= DecodeAndValidateFrame(request.data(), request.size(), &request_header) == DecodeStatus::kOk &&
        request_header.type == MessageType::HELLO && request_header.request_id == 1 && request_header.conn_id == 0;
    FrameHeader h; h.version_major = 1; h.conn_id = 37; h.request_id = variant == 1 ? 2 : 1;
    HelloAckPayload ack; ack.broker_version_major = 1;
    auto bytes = BuildHelloAckFrame(h, ack);
    if (variant == 0) bytes[0] = 0;
    if (variant == 2) { h.version_minor = 1; bytes = BuildHelloAckFrame(h, ack); }
    if (variant == 3) bytes = BuildErrorFrame(h, ErrorPayload{});
    if (variant == 4) bytes.resize(513, 0);
    ok &= f.Io(true, &bytes); handshake.join();
    auto expected = variant == 3 ? v8host::RendezvousStatus::kPeerAuthenticationFailed
                                : v8host::RendezvousStatus::kProtocolFailed;
    if (!ok || status != expected || hello.conn_id != 0) return Fail(detail, "invalid/rejected ACK must not be retryable IO");
  }
  return true;
}
bool TransportExactCrossing(std::string* detail) {
  for (bool relay : {false, true}) {
    PumpFixture f; if (!f.Init()) return Fail(detail, "pair");
    const size_t cap = relay ? kMaxQueuedRelayBytesPerRun : kMaxControlQueueBytes;
    if (!FillTransport(f, cap - 35, relay, 1) || f.Send(SizedTransportFrame(36, relay)) != v8host_status_quota_exceeded ||
        f.transport->QueuedBytesForTesting() != cap - 35) return Fail(detail, "exact cap+1 rejection");
  }
  PumpFixture f; if (!f.Init()) return Fail(detail, "pair");
  for (unsigned run = 1; run < 16; ++run)
    if (!FillTransport(f, kMaxQueuedRelayBytesPerRun, true, run)) return Fail(detail, "connection fill");
  if (!FillTransport(f, kMaxQueuedRelayBytesPerRun - 35, true, 16) ||
      f.Send(SizedTransportFrame(36, true, 17)) != v8host_status_quota_exceeded ||
      f.transport->QueuedBytesForTesting() != kMaxQueuedRelayBytesPerConnection - 35)
    return Fail(detail, "connection exact cap+1 rejection");
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--px-orphan-helper") == 0)
    return RunOrphanHelper(argc, argv);
  SetTransportFactoryForTesting(&MakeFake);
  TestSessionConfig preinit_config = BasicConfig();
  v8host_session_s* preinit_session = reinterpret_cast<v8host_session_s*>(1);
  g_preinitialize_output_was_nulled =
      OpenSession(&preinit_config, &preinit_session) ==
          v8host_status_not_initialized &&
      preinit_session == nullptr;
  v8host_broker preinit_broker = reinterpret_cast<v8host_broker>(1);
  g_preinitialize_broker_output_was_nulled =
      v8host_broker_open(nullptr, nullptr, &preinit_broker) ==
          v8host_status_not_initialized && preinit_broker == nullptr;
  g_initializing_thread = ::GetCurrentThreadId();
  SetInitializeAfterDispatcherHookForTesting(&InitializeAfterDispatcherHook);
  std::thread racer([] {
    {
      std::unique_lock<std::mutex> lock(g_init_mutex);
      g_init_cv.wait(lock, [] { return g_dispatcher_captured; });
      g_racer_calling = true;
      g_init_cv.notify_all();
    }
    g_racer_init_status = v8host_initialize(V8HOST_ABI_VERSION);
  });
  g_main_init_status = v8host_initialize(V8HOST_ABI_VERSION);
  SetInitializeAfterDispatcherHookForTesting(nullptr);
  racer.join();
  std::vector<TestCase> tests = {
      {"transport", "control-id-send-order", ControlIdSendOrder},
      {"transport", "close-lifetime-no-replay", TransportCallbackClose},
      {"transport", "handshake-rejects", HandshakeRejects},
      {"transport", "real-client-failure-paths", RealClientFailurePaths},
      {"transport", "exact-quota-crossing", TransportExactCrossing},
      {"transport", "startup-registration-failure", StartupRegistrationFailure},
      {"transport", "id-exhaustion", ClientIdExhaustion},
      {"transport", "admission-rollback", AdmissionRollback},
      {"transport", "mandatory-close-retry", MandatoryCloseRetry},
      {"transport", "mandatory-close-failure", MandatoryCloseFailure},
      {"transport", "mandatory-close-pending-limit", MandatoryClosePendingLimit},
      {"transport", "mandatory-close-buffer-bound", MandatoryCloseBufferBound},
      {"transport", "preconnect-fifo-version", TransportFifo},
      {"transport", "read-write-stop", TransportReadWriteStop},
      {"transport", "write-loss-no-replay", TransportWriteLoss},
      {"transport", "queue-boundaries", TransportQueueBoundaries},
      {"transport", "malformed-message", TransportMalformed},
      {"transport", "preack-retry-policy", TransportRetry},
      {"rendezvous", "dedicated-no-client-expires", DedicatedNoClientExpires},
      {"rendezvous", "dedicated-client-exit-orphan", DedicatedClientExitOrphan},
      {"rendezvous", "real-dedicated-connects", [](std::string* d) {
        return LaunchCase(d, v8host_broker_mode_dedicated, LaunchHold::kObserve); }},
      {"rendezvous", "slow-launch-dedicated-connects", [](std::string* d) {
        return LaunchCase(d, v8host_broker_mode_dedicated, LaunchHold::kSlow); }},
      {"rendezvous", "slow-launch-deadline-shift", [](std::string* d) {
        return LaunchCase(d, v8host_broker_mode_dedicated, LaunchHold::kExhausted); }},
      {"rendezvous", "slow-launch-close-no-orphan", [](std::string* d) {
        return LaunchCase(d, v8host_broker_mode_dedicated, LaunchHold::kClose); }},
      {"rendezvous", "real-shared-connects", [](std::string* d) {
        return LaunchCase(d, v8host_broker_mode_shared, LaunchHold::kObserve); }},
      {"rendezvous", "slow-launch-close-shared-survives", [](std::string* d) {
        return LaunchCase(d, v8host_broker_mode_shared, LaunchHold::kClose); }},
      {"init", "concurrent-thread-binding-destruction",
       ConcurrentInitializeLifecycle},
      {"validate", "struct-sizes", ValidateStructSizes},
      {"validate", "count-pointer-appcontainer", ValidatePairsAndPolicy},
      {"validate", "builder-setters", ValidateBuilderSetters},
      {"validate", "tier-path-output-nulling", ValidateRunPolicyAndOutputs},
      {"config", "roundtrip-zero-and-64", ConfigRoundTrip},
      {"config", "default-initial-token", DefaultInitialToken},
      {"lifecycle", "opaque-window-tokens", LifecycleTokens},
      {"boundary", "counts-runs-strings-frame", BoundaryQuotas},
      {"output", "all-failing-paths-null", ExhaustiveFailingOutputNulling},
      {"copy", "run-and-relay-roundtrip", RunAndRelayRoundTrip},
      {"callbacks", "replace-clear-app-thread", CallbackRoutingAndReplacement},
      {"callbacks", "alloc-failure-prior-intact", CallbackAllocationFailure},
      {"delivery", "inbound-run-terminal", InboundRunAndTerminal},
      {"delivery", "inbound-cancelled-result", InboundCancelledResult},
      {"delivery", "terminal-then-late-ack-relay", LateTrafficAfterTerminal},
      {"protocol", "wrong-conn-session-run-correlation",
       CorrelationMismatchDisconnects},
      {"protocol", "wrong-request-id-ack-error", UnknownRequestIdDisconnects},
      {"protocol", "relay-quota-error", RelayQuotaError},
      {"protocol", "pending-capacity-and-duplicate-ack",
       PendingCapacityAndDuplicateAck},
      {"protocol", "malformed-frame-disconnect", MalformedFrameDisconnects},
      {"reentrancy", "post-and-set", ReentrantPostAndSet},
      {"reentrancy", "close", ReentrantClose},
      {"backpressure", "overflow-and-close", BackpressureAndCloseSuppression},
      {"close", "flooding-nonblocking", CloseWhileFloodingNonblocking},
      {"disconnect", "no-replay", DisconnectNoReplay},
      {"race", "inbound-vs-close", InboundVsCloseRace},
      {"dispatch", "shared-core-session-parity", SharedCoreSessionParity},
      {"dispatch", "shared-core-lifetime", SharedCoreLifetime},
      {"topology", "private-id-routing", PrivateIdRouting},
      {"topology", "private-run-routing", PrivateRunRouting},
      {"topology", "close-order-and-flags", CloseOrderAndFlags},
      {"topology", "loss-fanout-order", LossFanoutOrder},
      {"topology", "callback-cap-boundaries", CallbackCapBoundaries},
      {"builders", "worker-runtime-bounds", WorkerRuntimeBounds},
      {"builders", "poison-and-null-output", PoisonAndNullOutput},
  };
  for (auto& test : tests) {
    auto run = std::move(test.run);
    test.run = [run = std::move(run), suite = test.suite, name = test.name](std::string* detail) {
      if (g_launch_abort) {
        printf("FAIL - skipping %s/%s and remaining cases after launch teardown failure\n",
               suite, name);
        fflush(stdout);
        ::ExitProcess(1);
      }
      return run(detail);
    };
  }
  return v8host::test::RunTests(argc, argv, tests);
}
