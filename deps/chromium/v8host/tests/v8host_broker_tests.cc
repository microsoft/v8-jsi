#include "v8host_test_support.h"

#include "v8host_broker_rendezvous.h"
#include "v8host_client.h"
#include "v8host_client_test_config.h"
#include "v8host_broker.h"
#include "v8host_file_identity.h"
#include "v8host_payload_identity.h"
#include "v8host_peer_auth.h"
#include "v8host_router.h"
#include "../v8host_engine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

bool V8HostEngineTestBatch(const std::vector<v8host::RunEnvelope> &, bool, size_t *, size_t *, size_t *);
bool V8HostEngineTestRoute(const std::vector<v8host::RunEnvelope> &);
bool V8HostEngineTestWaitSlot(uint16_t, bool);

// v8host_client.h mirrors these sbox.h values without including it; the client
// passes them through to the wire and the coordinator hands them to sbox raw.
#define V8HOST_SAME_VALUE(a, b) \
  static_assert(static_cast<int32_t>(a) == static_cast<int32_t>(b), #a " != " #b)
V8HOST_SAME_VALUE(v8host_integrity_level_low, sbox_integrity_level_low);
V8HOST_SAME_VALUE(v8host_integrity_level_untrusted, sbox_integrity_level_untrusted);
V8HOST_SAME_VALUE(v8host_token_level_lockdown, sbox_token_level_lockdown);
V8HOST_SAME_VALUE(v8host_token_level_limited, sbox_token_level_limited);
V8HOST_SAME_VALUE(v8host_token_level_interactive, sbox_token_level_interactive);
V8HOST_SAME_VALUE(v8host_token_level_restricted_non_admin, sbox_token_level_restricted_non_admin);
V8HOST_SAME_VALUE(v8host_token_level_restricted_same_access, sbox_token_level_restricted_same_access);
V8HOST_SAME_VALUE(v8host_message_kind_string, sbox_msg_kind_string);
V8HOST_SAME_VALUE(v8host_message_kind_binary, sbox_msg_kind_binary);
V8HOST_SAME_VALUE(v8host_broker_mode_dedicated, sbox_broker_mode_dedicated);
V8HOST_SAME_VALUE(v8host_broker_mode_shared, sbox_broker_mode_shared);
#undef V8HOST_SAME_VALUE

namespace {

using v8host::BrokerConnection;
using v8host::BrokerMode;
using v8host::BrokerRendezvous;
using v8host::HelloResult;
using v8host::RendezvousStatus;
using v8host::test::ExecutableDirectory;

bool ConnectAndHandshake(BrokerMode mode,
                         uint32_t request_id,
                         BrokerConnection* connection,
                         HelloResult* result,
                         std::string* detail) {
  BrokerRendezvous rendezvous(ExecutableDirectory(), mode);
  const RendezvousStatus connected = rendezvous.ConnectOrLaunch(connection);
  if (connected != RendezvousStatus::kOk) {
    *detail = "connect status=" + std::to_string(static_cast<int>(connected));
    return false;
  }
  const RendezvousStatus handshook = connection->Handshake(request_id, result);
  // Reciprocal peer auth plus a successful HELLO_ACK already prove the broker
  // authenticated this client, so no observed-client-pid echo is checked here;
  // client-identity rejection is covered by the dedicated peer-auth suite.
  if (handshook != RendezvousStatus::kOk) {
    *detail = "handshake status=" + std::to_string(static_cast<int>(handshook));
    return false;
  }
  // End-to-end check of the negotiated endpoint mode: the broker advertises its
  // own mode in HELLO_ACK, which must match this client's requested BrokerMode
  // on the wire (Dedicated=0 / Shared=1 on both surfaces). This is the only
  // place the coordinator's mode->endpoint_mode mapping is proven end-to-end.
  if (result->endpoint_mode != static_cast<uint32_t>(mode)) {
    *detail = "endpoint_mode=" + std::to_string(result->endpoint_mode) +
              " expected=" + std::to_string(static_cast<uint32_t>(mode));
    return false;
  }
  return true;
}

bool TerminalOrderings(std::string *detail) {
  namespace router = v8host::coordinator;
  for (bool cancel_first : {false, true}) {
    router::Connection conn;
    router::Session session;
    session.conn = &conn;
    session.session_id = 1;
    session.worker = std::make_unique<router::Worker>();
    session.active_run_id = 1;
    session.pending_run_ids = {1, 2};
    auto run = std::make_unique<router::Run>();
    run->session = &session;
    run->run_id = 1;
    router::Run *contender = run.get();
    session.runs.emplace(1, std::move(run));
    bool removed_before_emit = false;
    session.terminal_removed = [&] { removed_before_emit = session.runs.empty() && conn.out_queue.empty(); };
    const auto first = cancel_first ? router::RunState::kCancelled : router::RunState::kCompleted;
    const auto second = cancel_first ? router::RunState::kCompleted : router::RunState::kCancelled;
    if (contender->TryTerminal(router::RunState::kActive) || !contender->TryTerminal(first) ||
        contender->TryTerminal(second) || !removed_before_emit || session.retired_runs.size() != 1 ||
        conn.out_queue.size() != 1 || contender->state != first || session.active_run_id != 0 ||
        session.pending_run_ids != std::deque<uint32_t>{2}) {
      *detail = "terminal winner/removal/retention invariant";
      return false;
    }
  }
  return true;
}

namespace router = v8host::coordinator;
namespace protocol = v8host::protocol;
protocol::FrameHeader ControlHeader(uint32_t request, uint32_t session = 1, uint32_t run = 0) {
  protocol::FrameHeader h;
  h.version_major = protocol::kWireVersionMajor;
  h.version_minor = protocol::kWireVersionMinor;
  h.conn_id = 1;
  h.target_id = session;
  h.run_id = run;
  h.request_id = request;
  return h;
}
protocol::CreateSessionPayload LogicalConfig() {
  protocol::CreateSessionPayload config;
  config.prohibit_dynamic_code = true;
  return config;
}
void OpenLocal(router::Connection &conn) {
  conn.conn_id = 1;
  conn.state = router::ConnState::kOpen;
}
bool LocalRoute(router::Router &service, router::Connection &conn, const std::vector<uint8_t> &frame) {
  return service.Route(conn, frame.data(), frame.size());
}
bool PopFrame(router::Connection &conn, protocol::FrameHeader *h, protocol::StatusCode *error = nullptr) {
  std::vector<uint8_t> bytes;
  if (!conn.TakeOutput(&bytes))
    return false;
  conn.CompleteOutput(bytes.size());
  const uint8_t *payload;
  size_t size;
  if (protocol::DecodeAndValidateFrame(bytes.data(), bytes.size(), h, &payload, &size) != protocol::DecodeStatus::kOk)
    return false;
  if (error) {
    protocol::ErrorPayload decoded;
    if (!protocol::DecodeErrorPayload(payload, size, &decoded))
      return false;
    *error = decoded.status_code;
  }
  return true;
}
void DrainLocal(router::Connection &conn) {
  protocol::FrameHeader h;
  while (PopFrame(conn, &h)) {
  }
}
bool ConfigFidelity(size_t count, std::string *detail) {
  router::Connection conn;
  OpenLocal(conn);
  router::Router service(BrokerMode::kDedicated);
  auto config = LogicalConfig();
  config.integrity = sbox_integrity_level_untrusted;
  config.delayed_integrity = sbox_integrity_level_low;
  config.initial_token = sbox_token_level_restricted_same_access;
  config.lockdown_token = sbox_token_level_limited;
  config.use_app_container = true;
  config.low_privilege_app_container = true;
  config.app_container_profile = "owned-profile";
  for (size_t i = 0; i < count; ++i) {
    config.file_rules.push_back({i % 2 == 0, "C:\\data\\rule" + std::to_string(i)});
    config.capabilities.push_back("cap-" + std::to_string(i));
  }
  auto frame = protocol::BuildCreateSessionFrame(ControlHeader(1), config);
  if (!LocalRoute(service, conn, frame))
    return false;
  config.app_container_profile = "mutated";
  config.file_rules.clear();
  config.capabilities.clear();
  frame.assign(frame.size(), 0);
  const auto &owned = conn.sessions.at(1)->config;
  protocol::StartRunPayload inputs;
  inputs.tier_override = -1;
  router::BoundProfile profile;
  std::vector<uint8_t> bytes;
  v8host::V8HostSpawnConfigV1 spawn;
  if (!router::PrepareProfile(owned, inputs, L"C:\\payload", &profile) ||
      !router::PrepareSpawnConfig(owned, profile, &bytes) ||
      !v8host::V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &spawn) || spawn.tier != owned.tier ||
      spawn.integrity != owned.integrity || spawn.delayed_integrity != owned.delayed_integrity ||
      spawn.initial_token != owned.initial_token || spawn.lockdown_token != owned.lockdown_token ||
      spawn.prohibit_dynamic_code != owned.prohibit_dynamic_code ||
      spawn.use_app_container != owned.use_app_container ||
      spawn.low_privilege_app_container != owned.low_privilege_app_container ||
      spawn.app_container_profile != "owned-profile" || spawn.file_rules.size() != count ||
      spawn.capabilities != owned.capabilities || spawn.effective_tier != profile.effective_tier ||
      spawn.engine_dll != profile.engine_dll || spawn.snapshot_path != profile.snapshot_path) {
    *detail = "owned config/spawn fidelity";
    return false;
  }
  for (size_t i = 0; i < count; ++i)
    if (spawn.file_rules[i].readonly != owned.file_rules[i].readonly ||
        spawn.file_rules[i].pattern != owned.file_rules[i].pattern)
      return false;
  return !conn.sessions.at(1)->worker && !conn.sessions.at(1)->profile_bound;
}
bool ConfigFidelity0(std::string *detail) {
  return ConfigFidelity(0, detail);
}
bool ConfigFidelity64(std::string *detail) {
  return ConfigFidelity(64, detail);
}
bool ProfileValidation(std::string *detail) {
  auto config = LogicalConfig();
  for (auto field :
       {&protocol::CreateSessionPayload::broker_mode,
        &protocol::CreateSessionPayload::tier,
        &protocol::CreateSessionPayload::integrity,
        &protocol::CreateSessionPayload::delayed_integrity,
        &protocol::CreateSessionPayload::initial_token,
        &protocol::CreateSessionPayload::lockdown_token}) {
    for (int bad : {-1, 99}) {
      auto invalid = config;
      invalid.*field = bad;
      if (router::ValidateSessionConfig(invalid, BrokerMode::kDedicated))
        return false;
    }
  }
  auto invalid = config;
  invalid.broker_mode = 1;
  if (router::ValidateSessionConfig(invalid, BrokerMode::kDedicated))
    return false;
  invalid = config;
  invalid.low_privilege_app_container = true;
  if (router::ValidateSessionConfig(invalid, BrokerMode::kDedicated))
    return false;
  invalid = config;
  invalid.capabilities = {"cap"};
  if (router::ValidateSessionConfig(invalid, BrokerMode::kDedicated))
    return false;
  invalid = config;
  invalid.use_app_container = true;
  if (router::ValidateSessionConfig(invalid, BrokerMode::kDedicated))
    return false;
  invalid = config;
  invalid.tier = 1;
  if (router::ValidateSessionConfig(invalid, BrokerMode::kDedicated))
    return false;
  invalid = config;
  invalid.prohibit_dynamic_code = false;
  if (router::ValidateSessionConfig(invalid, BrokerMode::kDedicated))
    return false;
  invalid = config;
  invalid.file_rules.resize(65);
  if (router::ValidateSessionConfig(invalid, BrokerMode::kDedicated))
    return false;
  invalid = config;
  invalid.use_app_container = true;
  invalid.app_container_profile = "profile";
  invalid.capabilities.resize(65);
  if (router::ValidateSessionConfig(invalid, BrokerMode::kDedicated))
    return false;
  invalid = config;
  invalid.file_rules = {{true, std::string("bad\0path", 8)}};
  if (router::ValidateSessionConfig(invalid, BrokerMode::kDedicated))
    return false;
  protocol::StartRunPayload inputs;
  inputs.tier_override = -1;
  router::BoundProfile profile;
  if (!router::PrepareProfile(config, inputs, L"C:\\payload", &profile) || profile.effective_tier != 0 ||
      profile.engine_dll != "v8jsisb.dll")
    return false;
  for (int bad : {-2, 1, 2}) {
    inputs.tier_override = bad;
    if (router::PrepareProfile(config, inputs, L"C:\\payload", &profile))
      return false;
  }
  config.tier = 1;
  config.prohibit_dynamic_code = false;
  inputs.tier_override = -1;
  if (!router::PrepareProfile(config, inputs, L"C:\\payload", &profile) || profile.engine_dll != "v8jsi.dll")
    return false;
  inputs.tier_override = 0;
  if (router::PrepareProfile(config, inputs, L"C:\\payload", &profile))
    return false;
  inputs.tier_override = -1;
  inputs.has_engine_override = true;
  for (const std::string &bad : {"", ".", "x..dll", "C:x.dll", "dir/x.dll", "dir\\x.dll", "\xc0\x80"}) {
    inputs.engine_filename = bad;
    if (router::PrepareProfile(config, inputs, L"C:\\payload", &profile))
      return false;
  }
  inputs.engine_filename = "custom.dll";
  inputs.has_snapshot = true;
  for (const std::string &bad :
       {"../escape.bin",
        "C:\\outside\\snap.bin",
        "\\\\server\\share\\a",
        "\\\\?\\C:\\payload\\a",
        "C:relative",
        "\\rooted",
        "a:stream",
        "a.",
        "NUL.bin",
        "\xc0\x80"}) {
    inputs.snapshot_path = bad;
    if (router::PrepareProfile(config, inputs, L"C:\\payload", &profile))
      return false;
  }
  inputs.snapshot_path = "sub/../snap.bin";
  if (!router::PrepareProfile(config, inputs, L"C:\\payload", &profile) ||
      profile.snapshot_path != "C:\\payload\\snap.bin") {
    *detail = "snapshot canonicalization";
    return false;
  }
  inputs.snapshot_path = "C:\\PAYLOAD\\snap.bin";
  if (!router::PrepareProfile(config, inputs, L"C:\\payload", &profile))
    return false;
  inputs.snapshot_path.assign(5000, 'x');
  if (router::PrepareProfile(config, inputs, L"C:\\payload", &profile))
    return false;
  inputs.snapshot_path = "snap.bin";
  inputs.schema_version = 2;
  if (router::PrepareProfile(config, inputs, L"C:\\payload", &profile))
    return false;
  inputs.schema_version = protocol::kMessageSchemaVersion;
  if (!router::PrepareProfile(config, inputs, L"C:\\payload", &profile))
    return false;

  inputs.snapshot_path.assign(4096 - 20 - inputs.engine_filename.size() - std::string("C:\\payload\\").size(), 'x');
  if (!router::PrepareProfile(config, inputs, L"C:\\payload", &profile)) {
    *detail = "worker-profile exact size";
    return false;
  }
  inputs.snapshot_path.push_back('x');
  if (router::PrepareProfile(config, inputs, L"C:\\payload", &profile))
    return false;
  inputs.snapshot_path = "snap.bin";
  if (!router::PrepareProfile(config, inputs, L"C:\\payload", &profile))
    return false;
  auto large_config = config;
  large_config.file_rules = {{true, std::string(32700, 'x')}, {false, std::string(32100, 'y')}};
  auto large_inputs = inputs;
  large_inputs.has_snapshot = false;
  large_inputs.snapshot_path.clear();
  large_inputs.engine_filename.assign(4000, 'e');
  router::BoundProfile large_profile;
  std::vector<uint8_t> too_large;
  if (!router::PrepareProfile(large_config, large_inputs, L"C:\\payload", &large_profile) ||
      router::PrepareSpawnConfig(large_config, large_profile, &too_large)) {
    *detail = "spawn-config encoded size ceiling";
    return false;
  }
  router::Session session;
  if (router::BindProfile(session, profile) != protocol::StatusCode::OK ||
      router::BindProfile(session, profile) != protocol::StatusCode::OK)
    return false;
  auto conflict = profile;
  conflict.engine_dll = "different.dll";
  if (router::BindProfile(session, conflict) != protocol::StatusCode::ERROR_PROFILE_ALREADY_BOUND ||
      session.profile.engine_dll != "custom.dll" || session.worker)
    return false;
  return true;
}

bool PipeIo(HANDLE pipe, bool write, std::vector<uint8_t> *bytes) {
  OVERLAPPED ov = {};
  ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent)
    return false;
  DWORD len = 0;
  BOOL started = write ? ::WriteFile(pipe, bytes->data(), static_cast<DWORD>(bytes->size()), nullptr, &ov)
                       : ::ReadFile(pipe, bytes->data(), static_cast<DWORD>(bytes->size()), nullptr, &ov);
  bool pending = !started && ::GetLastError() == ERROR_IO_PENDING;
  bool ok = false;
  if (started)
    ok = ::GetOverlappedResult(pipe, &ov, &len, FALSE) != FALSE;
  else if (pending && ::WaitForSingleObject(ov.hEvent, 3000) == WAIT_OBJECT_0) {
    ok = ::GetOverlappedResult(pipe, &ov, &len, FALSE) != FALSE;
    pending = false;
  }
  if (pending) {
    ::CancelIoEx(pipe, &ov);
    ::GetOverlappedResult(pipe, &ov, &len, TRUE);
  }
  ::CloseHandle(ov.hEvent);
  if (ok && !write)
    bytes->resize(len);
  return ok && (!write || len == bytes->size());
}
struct WireHarness {
  BrokerConnection connection;
  HelloResult hello;
  HANDLE process = nullptr;
  ~WireHarness() {
    connection.Close();
    if (process) {
      ::WaitForSingleObject(process, 10000);
      ::CloseHandle(process);
    }
  }
  bool Open(std::string *detail) {
    if (!ConnectAndHandshake(BrokerMode::kDedicated, 900, &connection, &hello, detail))
      return false;
    process = ::OpenProcess(SYNCHRONIZE, FALSE, connection.broker_pid());
    return process != nullptr;
  }
  protocol::FrameHeader Header(uint32_t request, uint32_t session = 1, uint32_t run = 0) {
    auto h = ControlHeader(request, session, run);
    h.conn_id = hello.conn_id;
    h.version_major = hello.selected_major;
    h.version_minor = hello.selected_minor;
    return h;
  }
  bool Send(std::vector<uint8_t> bytes) {
    return PipeIo(connection.pipe(), true, &bytes);
  }
  bool Receive(std::vector<uint8_t> *bytes, protocol::FrameHeader *h) {
    bytes->resize(protocol::kMaxFrameSize);
    const uint8_t *payload;
    size_t size;
    return PipeIo(connection.pipe(), false, bytes) &&
        protocol::DecodeAndValidateFrame(bytes->data(), bytes->size(), h, &payload, &size) ==
        protocol::DecodeStatus::kOk;
  }
  bool Expect(protocol::MessageType type, uint32_t request, uint32_t session, uint32_t run = 0) {
    std::vector<uint8_t> bytes;
    protocol::FrameHeader h;
    bool ok = Receive(&bytes, &h) && h.type == type && h.conn_id == hello.conn_id && h.request_id == request &&
        h.target_id == session && h.run_id == run;
    if (!ok)
      std::printf("wire expected type=%u request=%u run=%u; received type=%u request=%u run=%u bytes=%zu\n",
          static_cast<unsigned>(type), request, run, static_cast<unsigned>(h.type), h.request_id, h.run_id, bytes.size());
    return ok;
  }
  bool Closed() {
    std::vector<uint8_t> bytes(protocol::kMaxFrameSize);
    return !PipeIo(connection.pipe(), false, &bytes) && ::WaitForSingleObject(process, 3000) == WAIT_OBJECT_0;
  }
  bool Finish() {
    connection.Close();
    return ::WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
  }
};
struct EngineFixture {
  sbox_worker_api api = {};
  std::vector<uint8_t> profile;
  std::vector<v8host::RunEnvelope> output;
  bool invalid = false;
  EngineFixture(uint32_t runtimes = 0) {
    v8host::V8HostWorkerProfileV1 config;
    config.jitless = 1;
    config.engine_dll = "v8jsisb.dll";
    profile = config.Encode();
    if (runtimes) {
      v8host::V8HostWorkerProfileV2 explicit_config;
      explicit_config.jitless = 1;
      explicit_config.engine_dll = "v8jsisb.dll";
      explicit_config.max_runtimes = runtimes;
      profile = explicit_config.Encode();
    }
    api.struct_size = sizeof(api);
    api.get_plugin_data = [](sbox_worker worker, const void **data, size_t *size) -> sbox_status {
      auto &f = *reinterpret_cast<EngineFixture *>(worker);
      *data = f.profile.data(); *size = f.profile.size(); return sbox_status_ok;
    };
    api.acg_enabled = [](sbox_worker) { return true; };
    api.post_message = [](sbox_worker worker, sbox_msg_kind kind, const void *data, size_t size) -> sbox_status {
      auto &f = *reinterpret_cast<EngineFixture *>(worker);
      v8host::RunEnvelope env;
      if (kind != sbox_msg_kind_binary || !v8host::DecodeRunEnvelope(static_cast<const uint8_t *>(data), size, &env)) {
        f.invalid = true;
        return sbox_status_failed;
      }
      f.output.push_back(std::move(env));
      return sbox_status_ok;
    };
  }
  ~EngineFixture() { V8HostEngineShutdown(reinterpret_cast<sbox_worker>(this)); }
  bool Warm() { return V8HostEngineWarmup(reinterpret_cast<sbox_worker>(this), &api) == sbox_status_ok; }
  bool Batch(const std::vector<v8host::RunEnvelope> &batch, bool closing, size_t *bytes, size_t *frames) {
    size_t remaining = 99;
    return V8HostEngineTestBatch(batch, closing, bytes, frames, &remaining) && remaining == 0 && !invalid;
  }
};
v8host::RunEnvelope GuestStart(const std::string &guest, uint32_t id = 1) {
  v8host::RunEnvelope env;
  env.run_id = id;
  env.payload.assign(guest.begin(), guest.end());
  return env;
}
v8host::RunEnvelope GuestRelay(size_t size, uint32_t id = 1) {
  v8host::RunEnvelope env;
  env.type = v8host::RunEnvelopeType::kRelay;
  env.run_id = id;
  env.relay_kind = sbox_msg_kind_binary;
  env.payload.assign(size, 7);
  return env;
}
bool StartRelaySameDrain(std::string *detail) {
  EngineFixture f;
  size_t bytes, frames;
  if (!f.Warm() || !f.Batch({GuestStart("var n=0;host.onmessage=function(m){host.postMessageBinary(m);"
          "if(++n===2)host.complete();};"), GuestRelay(2), GuestRelay(3)}, false, &bytes, &frames) ||
      frames != 2 || bytes != 37 || f.output.size() != 3 ||
      f.output[0].payload != std::vector<uint8_t>(2, 7) || f.output[1].payload != std::vector<uint8_t>(3, 7) ||
      f.output[2].type != v8host::RunEnvelopeType::kResult ||
      f.output[2].disposition != v8host::RunEnvelopeDisposition::kCompleted) {
    *detail = "forced START+RELAY FIFO in production scheduler";
    return false;
  }
  return true;
}
bool StartCancelSameDrain(std::string *detail) {
  EngineFixture f;
  size_t bytes, frames;
  v8host::RunEnvelope cancel;
  cancel.type = v8host::RunEnvelopeType::kCancel;
  cancel.run_id = 1;
  if (!f.Warm() || !f.Batch({GuestStart("host.onmessage=function(m){host.postMessageBinary(m);};"),
          cancel, GuestRelay(1)}, false, &bytes, &frames) || frames != 2 || f.output.size() != 1 ||
      f.output[0].type != v8host::RunEnvelopeType::kResult ||
      f.output[0].disposition != v8host::RunEnvelopeDisposition::kCancelled) {
    *detail = "forced START+CANCEL production branch";
    return false;
  }
  return true;
}
bool StartBridgeOverflow(std::string *detail) {
  EngineFixture f;
  if (!f.Warm())
    return false;
  size_t bytes, frames;
  for (const std::string guest : {"host.complete();", "throw new Error('bridge guest');",
                                 "host.complete();throw new Error('bridge guest');"}) {
    const bool throws = guest.find("throw") != std::string::npos;
    for (bool byte_limit : {false, true}) {
      for (bool overflow : {false, true}) {
        f.output.clear();
        std::vector<v8host::RunEnvelope> batch{GuestStart(guest)};
        if (byte_limit) {
          // 16 full envelopes and one tail hit exactly 1 MiB.
          for (int i = 0; i < 16; ++i)
            batch.push_back(GuestRelay(65512));
          batch.push_back(GuestRelay(112 + (overflow ? 1 : 0)));
        } else {
          for (size_t i = 0; i < 256 + (overflow ? 1 : 0); ++i)
            batch.push_back(GuestRelay(0));
        }
        const bool error = overflow || throws;
        if (!f.Batch(batch, false, &bytes, &frames) || f.output.size() != 1 || f.output[0].run_id != 1 ||
            f.output[0].type != (error ? v8host::RunEnvelopeType::kRunError : v8host::RunEnvelopeType::kResult) ||
            (error && f.output[0].status_code != static_cast<uint32_t>(protocol::StatusCode::ERROR_INTERNAL)) ||
            (!error && f.output[0].disposition != v8host::RunEnvelopeDisposition::kCompleted) ||
            bytes > protocol::kMaxQueuedRelayBytesPerRun || frames > 256 ||
            (!overflow && byte_limit && bytes != protocol::kMaxQueuedRelayBytesPerRun) ||
            (!byte_limit && frames != 256)) {
          *detail = "activation bridge limit/+1 with completed/thrown-guest terminal priority";
          return false;
        }
      }
    }
  }
  for (bool overflow : {false, true}) {
    f.output.clear();
    std::vector<v8host::RunEnvelope> batch;
    for (uint32_t id = 1; id <= protocol::kMaxInFlightRunsPerSession + (overflow ? 1 : 0); ++id) {
      batch.push_back(GuestStart("host.complete();", id));
      batch.push_back(GuestRelay(1, id));
    }
    size_t remaining = 99;
    const bool ok = V8HostEngineTestBatch(batch, false, &bytes, &frames, &remaining);
    if (ok == overflow || remaining || f.invalid || frames != 4 || bytes != 68 ||
        f.output.size() != (overflow ? 0 : 4)) {
      *detail = "activation metadata 4/+1 did not fail closed and release held frames";
      return false;
    }
    for (size_t i = 0; i < f.output.size(); ++i)
      if (f.output[i].run_id != i + 1 || f.output[i].type != v8host::RunEnvelopeType::kResult ||
          f.output[i].disposition != v8host::RunEnvelopeDisposition::kCompleted)
        return false;
  }
  f.output.clear();
  if (!f.Batch({GuestStart("host.postMessage('not-started');"), GuestRelay(10)}, true, &bytes, &frames) ||
      !f.output.empty()) {
    *detail = "closing iteration began or replayed pending START";
    return false;
  }
  if (!f.Batch({GuestRelay(10, 99)}, false, &bytes, &frames) || !f.output.empty() || bytes || frames)
    return false;
  return true;
}
struct RouterFixture {
  router::Connection conn;
  sbox_broker_api api = {sizeof(api), &Spawn, &Post, &Close, &Wait,
                         nullptr, nullptr, nullptr, &GetProcessId};
  static sbox_status SBOX_CDECL GetProcessId(sbox_broker_worker handle, uint32_t* pid) {
    auto& f = *reinterpret_cast<Handle*>(handle)->fixture;
    ++f.pid_reads;
    *pid = f.zero_pid ? 0 : 1234;
    return f.fail_pid ? sbox_status_failed : sbox_status_ok;
  }
  std::unique_ptr<router::Router> service;
  router::Worker *worker = nullptr;
  struct Handle {
    RouterFixture *fixture;
    router::Worker *worker;
    std::atomic<bool> closed{false};
    Handle(RouterFixture *f, router::Worker *w) : fixture(f), worker(w) {}
  };
  std::vector<std::unique_ptr<Handle>> handles;
  std::vector<v8host::RunEnvelope> posts;
  std::atomic<unsigned> spawns{0}, closes{0}, waits{0}, post_after_close{0}, close_before_removal{0};
  unsigned pid_reads = 0;
  bool fail_pid = false, zero_pid = false;
  bool early = false, before_publication = false, fail_post = false, fail_spawn = false, stop_during_spawn = false;
  HANDLE wait_entered = nullptr, wait_release = nullptr;
  RouterFixture() {
    OpenLocal(conn);
    v8host::HeldFile image;
    DWORD error;
    v8host::OpenImmutableFile(ExecutableDirectory() + L"\\v8jsisb.dll", &image, &error);
    service = std::make_unique<router::Router>(BrokerMode::kDedicated, reinterpret_cast<sbox_broker>(this),
        &api, ExecutableDirectory(), image.machine());
  }
  ~RouterFixture() {
    if (wait_release)
      ::SetEvent(wait_release);
    service->Close(conn);
    if (wait_entered)
      ::CloseHandle(wait_entered);
    if (wait_release)
      ::CloseHandle(wait_release);
  }
  static sbox_status SBOX_CDECL Spawn(sbox_broker broker, const void *data, size_t size,
      sbox_message_cb callback, void *context, sbox_broker_worker *handle) {
    auto &f = *reinterpret_cast<RouterFixture *>(broker);
    ++f.spawns;
    v8host::V8HostSpawnConfigV1 config;
    if (!v8host::V8HostSpawnConfigV1::Decode(static_cast<const uint8_t *>(data), size, &config) ||
        (f.fail_spawn && !f.early))
      return sbox_status_failed;
    f.worker = static_cast<router::Worker *>(context);
    if (f.early) {
      const uint8_t startup[] = {1, 0, 0, 0}, security[] = {2, 0, 0, 0};
      callback(context, sbox_msg_kind_lifecycle, startup, 4);
      callback(context, sbox_msg_kind_lifecycle, security, 4);
      f.before_publication = !f.worker->handle && f.posts.empty() && bool(f.worker->engine_image);
    }
    if (f.fail_spawn)
      return sbox_status_failed;
    if (f.stop_during_spawn)
      f.conn.Stop();
    f.handles.push_back(std::make_unique<Handle>(&f, f.worker));
    *handle = reinterpret_cast<sbox_broker_worker>(f.handles.back().get());
    return sbox_status_ok;
  }
  static sbox_status SBOX_CDECL Post(sbox_broker_worker handle, sbox_msg_kind kind, const void *data, size_t size) {
    auto &f = *reinterpret_cast<Handle *>(handle)->fixture;
    if (reinterpret_cast<Handle *>(handle)->closed)
      ++f.post_after_close;
    v8host::RunEnvelope env;
    if (f.fail_post || kind != sbox_msg_kind_binary || !v8host::DecodeRunEnvelope(static_cast<const uint8_t *>(data), size, &env))
      return sbox_status_failed;
    f.posts.push_back(std::move(env));
    return sbox_status_ok;
  }
  static sbox_status SBOX_CDECL Close(sbox_broker_worker handle) {
    auto &f = *reinterpret_cast<Handle *>(handle)->fixture;
    ++f.closes;
    reinterpret_cast<Handle *>(handle)->closed = true;
    std::lock_guard<std::mutex> lock(f.conn.mutex);
    auto *worker = reinterpret_cast<Handle *>(handle)->worker;
    const bool live = worker && (worker->session
        ? f.conn.sessions.contains(worker->session->session_id) || !worker->session->runs.empty()
        : f.conn.workers.contains(worker->worker_id));
    if (!worker || live)
      ++f.close_before_removal;
    return sbox_status_ok;
  }
  static sbox_status SBOX_CDECL Wait(sbox_broker_worker handle, int32_t *code) {
    auto &f = *reinterpret_cast<Handle *>(handle)->fixture;
    ++f.waits;
    if (f.wait_entered)
      ::SetEvent(f.wait_entered);
    if (f.wait_release)
      ::WaitForSingleObject(f.wait_release, INFINITE);
    *code = 0;
    return sbox_status_ok;
  }
  bool Create() {
    return LocalRoute(*service, conn, protocol::BuildCreateSessionFrame(ControlHeader(1), LogicalConfig()));
  }
  bool Start(uint32_t id, protocol::StartRunPayload inputs = {}) {
    inputs.tier_override = inputs.tier_override == 0 ? -1 : inputs.tier_override;
    if (inputs.guest_payload.empty())
      inputs.guest_payload = {'x'};
    return LocalRoute(*service, conn, protocol::BuildStartRunFrame(ControlHeader(1 + id, 1, id), inputs));
  }
  bool Relay(uint32_t id, size_t size, uint8_t byte = 1) {
    auto h = ControlHeader(0, 1, id);
    h.type = protocol::MessageType::RELAY_TO_WORKER;
    std::vector<uint8_t> body(size, byte);
    return LocalRoute(*service, conn, protocol::BuildRelayFrame(h, sbox_msg_kind_binary, body.data(), body.size()));
  }
  void Phase(uint8_t phase, size_t size = 4) {
    const uint8_t bytes[] = {phase, 0, 0, 0, 0};
    router::Worker::OnMessage(worker, sbox_msg_kind_lifecycle, bytes, size);
    service->Pump(conn);
  }
  void Terminal(uint32_t id, v8host::RunEnvelopeType type = v8host::RunEnvelopeType::kResult) {
    v8host::RunEnvelope env;
    env.type = type;
    env.run_id = id;
    env.status_code = static_cast<uint32_t>(protocol::StatusCode::ERROR_INTERNAL);
    auto bytes = v8host::EncodeRunEnvelope(env);
    router::Worker::OnMessage(worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
    service->Pump(conn);
  }
};
bool QueuedCancelsFirst(std::string *detail) {
  for (bool ready : {false, true}) {
    RouterFixture f;
    f.early = ready;
    if (!f.Create() || !f.Start(1) || !f.Start(2) || !f.Relay(2, 32)) return false;
    auto *worker = f.worker;
    auto &session = *worker->session;
    auto *run = session.runs.at(2).get();
    auto removed = std::make_shared<bool>(false);
    session.terminal_removed = [&, removed, run, worker] {
      *removed = !session.runs.contains(2) && run->held_relays.empty() && !run->queued_relay_bytes &&
          std::find(worker->session->pending_run_ids.begin(), worker->session->pending_run_ids.end(), 2) == worker->session->pending_run_ids.end();
    };
    DrainLocal(f.conn);
    const auto cancel = protocol::BuildCancelRunFrame(ControlHeader(9, 1, 2));
    if (!LocalRoute(*f.service, f.conn, cancel)) return false;
    // Production reclaims the run once the strand has no contenders.
    session.terminal_removed = {};
    protocol::FrameHeader h;
    std::vector<uint8_t> terminal;
    const uint8_t *payload = nullptr;
    size_t size = 0;
    protocol::ResultPayload result;
    if (!f.conn.TakeOutput(&terminal)) return false;
    f.conn.CompleteOutput(terminal.size());
    if (!*removed || session.runs.contains(2) ||
        protocol::DecodeAndValidateFrame(terminal.data(), terminal.size(), &h, &payload, &size) != protocol::DecodeStatus::kOk ||
        h.type != protocol::MessageType::RESULT || h.run_id != 2 || !protocol::DecodeResultPayload(payload, size, &result) ||
        result.disposition != protocol::ResultDisposition::kCancelled ||
        !PopFrame(f.conn, &h) || h.type != protocol::MessageType::ACK || h.request_id != 9 ||
        !f.conn.out_queue.empty()) {
      *detail = "queued CANCEL did not remove, credit and terminalize before ACK";
      return false;
    }
    if (!LocalRoute(*f.service, f.conn, cancel) || !PopFrame(f.conn, &h) || h.type != protocol::MessageType::ACK ||
        !f.conn.out_queue.empty()) return false;
    if (!ready) { f.Phase(1); f.Phase(2); }
    f.Terminal(1);
    for (const auto &post : f.posts)
      if (post.run_id == 2) { *detail = "cancelled queued run posted to worker"; return false; }
    if (f.posts.size() != 1 || worker->session->engine_busy_run_id || !worker->session->pending_run_ids.empty()) return false;
  }
  return true;
}
bool TimeoutCloseOrder(std::string *detail) {
  for (int winner = 0; winner != 4; ++winner) {
    RouterFixture f;
    auto now = std::chrono::steady_clock::time_point{} + std::chrono::seconds(1);
    f.service->test_now = [&] { return now; };
    f.wait_entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    f.early = true;
    if (!f.wait_entered || !f.wait_release || !f.Create() || !f.Start(1) || !f.Start(2) || !f.Relay(2, 32))
      return false;
    auto *worker = f.worker;
    auto &session = *worker->session;
    std::vector<router::Run *> runs;
    for (auto &[id, run] : session.runs) runs.push_back(run.get());
    auto removed_first = std::make_shared<bool>(true);
    session.terminal_removed = [&, removed_first] {
      *removed_first = *removed_first && !f.conn.sessions.contains(1) && session.runs.empty();
    };
    DrainLocal(f.conn);
    const auto cancel = protocol::BuildCancelRunFrame(ControlHeader(9, 1, 1));
    if (!LocalRoute(*f.service, f.conn, cancel) || !LocalRoute(*f.service, f.conn, cancel)) return false;
    DrainLocal(f.conn);
    now += std::chrono::milliseconds(4999);
    f.service->Pump(f.conn);
    if (f.conn.sessions.size() != 1 || f.closes || f.waits || f.posts.size() != 2 ||
        f.posts[1].type != v8host::RunEnvelopeType::kCancel || f.service->WakeDelay(f.conn, INFINITE) != 1 ||
        !f.conn.out_queue.empty()) { *detail = "fallback fired early or duplicate CANCEL posted"; return false; }
    if (winner == 1) {
      session.terminal_removed = {};
      f.Terminal(1);
      if (worker->session->cancel_deadline != std::chrono::steady_clock::time_point{} || f.posts.size() != 4) return false;
      DrainLocal(f.conn);
    } else if (winner == 2) {
      if (!LocalRoute(*f.service, f.conn, protocol::BuildCloseSessionFrame(ControlHeader(10)))) return false;
      DrainLocal(f.conn);
    } else if (winner == 3) {
      // Connection Close joins; release the fake wait before entering it.
      ::SetEvent(f.wait_release);
      f.service->Close(f.conn);
    }
    now += std::chrono::milliseconds(1);
    f.service->Pump(f.conn);
    if (winner == 0) {
      if (::WaitForSingleObject(f.wait_entered, 5000) != WAIT_OBJECT_0 || !*removed_first ||
          !f.conn.sessions.empty() || f.conn.retired_sessions.size() != 1 || !session.runs.empty() ||
          f.posts.size() != 2 || f.closes != 1 || f.waits != 1 || f.close_before_removal || f.post_after_close ||
          worker->session->cancel_deadline != std::chrono::steady_clock::time_point{}) {
        *detail = "expiry did not remove all ids before CAS/close and retain held cleanup";
        return false;
      }
      for (auto *run : runs)
        if (run->state != router::RunState::kWorkerExited || !run->held_relays.empty() || run->queued_relay_bytes)
          return false;
      protocol::FrameHeader h;
      if (!PopFrame(f.conn, &h) || h.type != protocol::MessageType::WORKER_EXIT || !f.conn.out_queue.empty()) {
        *detail = "client terminal not delivered at timer fire while wait held"; return false;
      }
    } else if (!f.conn.out_queue.empty() || (winner == 1 && (f.closes || f.waits))) return false;
    for (int i = 0; i < 3; ++i) { now += std::chrono::seconds(5); f.service->Pump(f.conn); }
    if (!f.conn.out_queue.empty() || f.service->WakeDelay(f.conn, INFINITE) != INFINITE) return false;
    ::SetEvent(f.wait_release);
    f.service->Close(f.conn);
    if (!f.conn.sessions.empty() || !f.conn.retired_sessions.empty() || f.closes != 1 || f.waits != 1 ||
        f.close_before_removal || f.post_after_close) return false;
  }
  return true;
}
bool ReapingSessionQuota(std::string *detail) {
  RouterFixture f;
  f.early = true;
  f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!f.wait_release) return false;
  for (uint32_t id = 1; id <= protocol::kMaxSessionsPerConnection; ++id) {
    protocol::StartRunPayload start;
    start.tier_override = -1;
    start.guest_payload = {'x'};
    if (!LocalRoute(*f.service, f.conn, protocol::BuildCreateSessionFrame(ControlHeader(id * 3, id), LogicalConfig())) ||
        !LocalRoute(*f.service, f.conn, protocol::BuildStartRunFrame(ControlHeader(id * 3 + 1, id, 1), start)) ||
        !LocalRoute(*f.service, f.conn, protocol::BuildCloseSessionFrame(ControlHeader(id * 3 + 2, id)))) return false;
    DrainLocal(f.conn);
  }
  if (f.conn.retired_sessions.size() != protocol::kMaxSessionsPerConnection ||
      ::WaitForSingleObject(f.conn.stop, 0) == WAIT_OBJECT_0) {
    *detail = "bounded reaping churn disconnected healthy connection"; return false;
  }
  if (!LocalRoute(*f.service, f.conn, protocol::BuildCreateSessionFrame(ControlHeader(100, 33), LogicalConfig())))
    return false;
  protocol::FrameHeader h;
  protocol::StartRunPayload replacement;
  replacement.tier_override = -1;
  replacement.guest_payload = {'x'};
  if (!PopFrame(f.conn, &h) || h.type != protocol::MessageType::ACK ||
      !PopFrame(f.conn, &h) || h.type != protocol::MessageType::SESSION_READY ||
      !LocalRoute(*f.service, f.conn, protocol::BuildStartRunFrame(ControlHeader(101, 33, 1), replacement)))
    return false;
  DrainLocal(f.conn);
  if (!f.conn.sessions.contains(33) || f.spawns != 32 ||
      f.conn.sessions.at(33)->worker->state != router::WorkerState::kNone ||
      ::WaitForSingleObject(f.conn.stop, 0) == WAIT_OBJECT_0) return false;
  ::SetEvent(f.wait_release);
  for (const auto &session : f.conn.retired_sessions)
    if (::WaitForSingleObject(session->worker->cleanup, 5000) != WAIT_OBJECT_0) return false;
  f.service->Pump(f.conn);
  if (f.spawns != 33 || !f.conn.retired_sessions.empty() || f.closes != 32 || f.waits != 32 ||
      f.close_before_removal || f.post_after_close) {
    *detail = "joined reapers did not release admission credit"; return false;
  }
  return true;
}
bool MailboxPumpBound(std::string *detail) {
  RouterFixture f;
  f.early = true;
  if (!f.Create() || !f.Start(1)) return false;
  DrainLocal(f.conn);
  auto *worker = f.worker;
  auto bytes = v8host::EncodeRunEnvelope(GuestRelay(1));
  router::Worker::OnMessage(worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
  size_t copied = 0;
  worker->mailbox_readout = [&] {
    ++copied;
    router::Worker::OnMessage(worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
  };
  f.service->Pump(f.conn);
  worker->mailbox_readout = {};
  if (copied != worker->mailbox.size() || worker->mailbox_tail - worker->mailbox_head != 1 ||
      ::WaitForSingleObject(f.conn.worker_wake, 0) != WAIT_OBJECT_0 || f.closes || worker->mailbox_failed ||
      f.conn.out_queue.size() != worker->mailbox.size()) {
    *detail = "producer replenishment did not yield bounded Pump and re-signal"; return false;
  }
  f.service->Pump(f.conn);
  return worker->mailbox_tail == worker->mailbox_head && f.conn.out_queue.size() == worker->mailbox.size() + 1;
}
bool TimeoutPendingRead(std::string *detail) {
  RouterFixture f;
  f.early = true;
  std::atomic<int64_t> ticks{1000};
  f.service->test_now = [&] {
    return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(ticks.load());
  };
  if (!f.Create() || !f.Start(1) || !LocalRoute(*f.service, f.conn,
      protocol::BuildCancelRunFrame(ControlHeader(9, 1, 1)))) return false;
  DrainLocal(f.conn);
  struct Handles {
    HANDLE pipe = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
    HANDLE pending = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE pumped = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Handles() {
      if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(pipe);
      if (client != INVALID_HANDLE_VALUE) ::CloseHandle(client);
      if (pending) ::CloseHandle(pending);
      if (pumped) ::CloseHandle(pumped);
    }
  } h;
  const auto name = LR"(\\.\pipe\v8host-timeout-read-)" + std::to_wstring(::GetCurrentProcessId());
  h.pipe = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
  h.client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED, nullptr);
  OVERLAPPED connect = {};
  connect.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool connected = h.pipe != INVALID_HANDLE_VALUE && h.client != INVALID_HANDLE_VALUE && h.pending && h.pumped &&
      connect.hEvent && (::ConnectNamedPipe(h.pipe, &connect) || ::GetLastError() == ERROR_PIPE_CONNECTED);
  if (connect.hEvent) ::CloseHandle(connect.hEvent);
  if (!connected) return false;
  f.wait_entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!f.wait_entered || !f.wait_release) return false;
  f.conn.pipe = h.pipe;
  f.conn.read_pending = h.pending;
  f.conn.dispatcher = f.service.get();
  f.service->test_pump_complete = [&] { ::SetEvent(h.pumped); };
  bool read_ok = true;
  std::thread reader([&] { uint8_t byte; DWORD size; read_ok = f.conn.ReadFrame(&byte, 1, &size, INFINITE); });
  bool passed = ::WaitForSingleObject(h.pending, 5000) == WAIT_OBJECT_0;
  if (passed) {
    ticks = 5999;
    ::SetEvent(f.conn.worker_wake);
    passed = ::WaitForSingleObject(h.pumped, 5000) == WAIT_OBJECT_0;
  }
  if (passed) {
    passed = f.conn.sessions.size() == 1 && !f.closes && !f.waits && f.conn.out_queue.empty();
    ticks = 6000;
    ::SetEvent(f.conn.worker_wake);
    passed = passed && ::WaitForSingleObject(f.wait_entered, 5000) == WAIT_OBJECT_0;
  }
  f.conn.Stop();
  reader.join();
  f.conn.pipe = INVALID_HANDLE_VALUE;
  f.conn.read_pending = nullptr;
  f.service->test_pump_complete = {};
  f.conn.dispatcher = nullptr;
  protocol::FrameHeader frame;
  passed = passed && !read_ok && !f.conn.active_io && f.conn.sessions.empty() && f.conn.retired_sessions.size() == 1 &&
      PopFrame(f.conn, &frame) && frame.type == protocol::MessageType::WORKER_EXIT && f.conn.out_queue.empty() &&
      f.closes == 1 && f.waits == 1 && !f.close_before_removal;
  ::SetEvent(f.wait_release);
  f.service->Close(f.conn);
  if (!passed || !f.conn.retired_sessions.empty()) {
    *detail = "pending pipe read did not service explicit cancel clock and join before teardown"; return false;
  }
  return true;
}
bool PopWorkerExit(router::Connection& conn, uint32_t session_id = 1) {
  protocol::FrameHeader h;
  return PopFrame(conn, &h) && h.type == protocol::MessageType::WORKER_EXIT &&
      h.target_id == session_id && h.run_id == 0 && h.request_id == 0 && conn.out_queue.empty();
}
bool LifecycleAbi(std::string* detail) {
  bool ok = SBOX_ABI_VERSION == 1u;
  for (const auto* dll : {L"v8host.dll", L"v8host_abi4_plugin.dll"}) {
    HMODULE module = ::LoadLibraryExW((ExecutableDirectory() + L"\\" + dll).c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    if (!module)
      return false;
    auto entry = reinterpret_cast<sbox_plugin_main_fn>(::GetProcAddress(module, SBOX_PLUGIN_ENTRY));
    // The fixture plugin was built for ABI 4: an exact-version host rejects it, and it rejects the current one.
    const bool old = std::wstring(dll) == L"v8host_abi4_plugin.dll";
    const sbox_plugin_api* plugin = nullptr;
    ok = entry && entry(old ? 4u : SBOX_ABI_VERSION, &plugin) == sbox_status_ok && plugin &&
        plugin->abi_version == (old ? 4u : SBOX_ABI_VERSION) && plugin->configure && plugin->broker_run &&
        plugin->warmup && plugin->run && plugin->shutdown && ok;
    plugin = reinterpret_cast<const sbox_plugin_api*>(1);
    ok = entry && entry(old ? SBOX_ABI_VERSION : 4u, &plugin) == sbox_status_version_mismatch && !plugin && ok;
    if (!old) {
      plugin = reinterpret_cast<const sbox_plugin_api*>(1);
      ok = entry && entry(SBOX_ABI_VERSION + 1u, &plugin) == sbox_status_version_mismatch && !plugin && ok;
      plugin = reinterpret_cast<const sbox_plugin_api*>(1);
      ok = entry && entry(0u, &plugin) == sbox_status_version_mismatch && !plugin && ok;
    }
    ::FreeLibrary(module);
  }
  DWORD code = 0;
  const std::wstring command = L"\"" + ExecutableDirectory() +
      L"\\sbox.exe\" --broker --plugin v8host_abi4_plugin.dll";
  ok = v8host::test::RunProcess(command, &code) && code == 3 && ok;
  if (!ok) *detail = "matching ABI/4-8 and old7 rejection";
  return ok;
}
bool CoreLifecycleCase(const wchar_t* args, std::string* detail) {
  DWORD code = 1;
  const std::wstring command = L"\"" + ExecutableDirectory() +
      L"\\sbox_lifecycle_tests.exe\" " + args;
  if (!v8host::test::RunProcess(command, &code) || code) {
    *detail = "production core fixture failed";
    return false;
  }
  return true;
}
bool ExitFinalDrain(std::string* detail) {
  if (!CoreLifecycleCase(L"--suite=reader --case=final-drain", detail))
    return false;
  RouterFixture f;
  f.early = true;
  if (!f.Create() || !f.Start(1) || !f.Start(2))
    return false;
  DrainLocal(f.conn);
  v8host::RunEnvelope env;
  env.type = v8host::RunEnvelopeType::kResult;
  env.run_id = 1;
  const auto bytes = v8host::EncodeRunEnvelope(env);
  router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
  const uint8_t exit[] = {sbox_lifecycle_phase_exit,0,0,0};
  router::Worker::OnMessage(f.worker, sbox_msg_kind_lifecycle, exit, 4);
  f.service->Pump(f.conn);
  protocol::FrameHeader h;
  const bool ordered = PopFrame(f.conn, &h) && h.type == protocol::MessageType::RESULT && h.run_id == 1 &&
      PopWorkerExit(f.conn) && f.conn.sessions.empty() && f.posts.size() == 1;
  f.service->Close(f.conn);
  return ordered && f.closes == 1 && f.waits == 1 && !f.close_before_removal && !f.post_after_close &&
      f.conn.retired_sessions.empty();
}
bool SmokeLifecycleTwo(std::string* d) { return CoreLifecycleCase(L"--suite=smoke --case=lifecycle-two", d); }
bool WorkerKindGuard(std::string* d) { return CoreLifecycleCase(L"--suite=worker --case=post-kind-guard", d); }
bool ExitFactOrigin(std::string *detail) {
  if (!CoreLifecycleCase(L"--suite=reader --case=origin", detail))
    return false;
  RouterFixture f;
  f.wait_entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!f.Create() || !f.Start(1))
    return false;
  f.Phase(sbox_lifecycle_phase_startup);
  f.Phase(sbox_lifecycle_phase_exit);
  if (!f.conn.sessions.empty() || ::WaitForSingleObject(f.wait_entered, 5000) != WAIT_OBJECT_0 ||
      f.closes != 1 || f.close_before_removal || f.waits != 1) {
    *detail = "exit ids/cleanup handoff";
    return false;
  }
  f.Phase(sbox_lifecycle_phase_exit);
  protocol::FrameHeader h;
  unsigned exits = 0, terminals = 0;
  while (PopFrame(f.conn, &h)) {
    exits += h.type == protocol::MessageType::WORKER_EXIT;
    terminals += h.type == protocol::MessageType::RESULT || h.type == protocol::MessageType::RUN_ERROR;
  }
  return exits == 1 && terminals == 0 && f.waits == 1 && !f.post_after_close;
}
bool ExitCleanupOrderings(std::string* detail) {
  if (!CoreLifecycleCase(L"--suite=reader --case=stop-suppresses-exit", detail))
    return false;
  for (int order = 0; order != 3; ++order) {
    RouterFixture f;
    f.wait_entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    f.early = true;
    if (!f.Create() || !f.Start(1) || f.closes || f.waits || f.worker->wait_called)
      return false;
    DrainLocal(f.conn);
    auto* worker = f.worker;
    if (order == 0)
      f.Phase(sbox_lifecycle_phase_exit);
    if (!LocalRoute(*f.service, f.conn, protocol::BuildCloseSessionFrame(ControlHeader(9))))
      return false;
    if (::WaitForSingleObject(f.wait_entered, 5000) != WAIT_OBJECT_0)
      return false;
    if (order == 1)
      f.Phase(sbox_lifecycle_phase_exit);
    protocol::FrameHeader h;
    // The session or explicit-close terminal precedes the ACK.
    bool terminal = PopFrame(f.conn, &h) && h.type == (order == 0
        ? protocol::MessageType::WORKER_EXIT : protocol::MessageType::RESULT);
    if (!terminal || !PopFrame(f.conn, &h) || h.type != protocol::MessageType::ACK || !f.conn.out_queue.empty() ||
        !f.conn.sessions.empty() || !worker->session->runs.empty() || f.closes != 1 || f.waits != 1 ||
        f.close_before_removal || f.post_after_close)
      return false;
    ::SetEvent(f.wait_release);
    f.service->Close(f.conn);
    if (!f.conn.retired_sessions.empty() || f.closes != 1 || f.waits != 1)
      return false;
  }
  return true;
}
bool CoordinatorTerminalHoldsNext(std::string *detail) {
  RouterFixture f;
  if (!f.Create() || !f.Start(1))
    return false;
  f.Phase(1); f.Phase(2);
  if (f.posts.size() != 1 || !f.Start(2) || !f.Relay(2, 1, 7) || !f.Relay(1, 65513))
    return false;
  if (f.worker->session->engine_busy_run_id != 1 || f.worker->session->active_run_id != 0 || f.posts.size() != 2 ||
      f.posts.back().type != v8host::RunEnvelopeType::kCancel || f.conn.sessions.at(1)->runs.contains(1)) {
    *detail = "logical terminal retired engine busy or dispatched successor";
    return false;
  }
  f.service->Pump(f.conn); f.service->Pump(f.conn);
  if (f.posts.size() != 2 || !f.Relay(2, 1, 8))
    return false;
  unsigned terminals = 0;
  protocol::FrameHeader h;
  std::vector<uint8_t> bytes;
  while (f.conn.TakeOutput(&bytes)) {
    f.conn.CompleteOutput(bytes.size());
    const uint8_t *payload; size_t size;
    if (protocol::DecodeAndValidateFrame(bytes.data(), bytes.size(), &h, &payload, &size) != protocol::DecodeStatus::kOk)
      return false;
    if (h.type == protocol::MessageType::RUN_ERROR) {
      protocol::ErrorPayload error;
      if (!protocol::DecodeErrorPayload(payload, size, &error) || error.status_code != protocol::StatusCode::ERROR_QUOTA ||
          h.run_id != 1 || h.request_id != 0)
        return false;
      ++terminals;
    }
  }
  f.Terminal(1);
  if (terminals != 1 || f.posts.size() != 5 || f.posts[2].type != v8host::RunEnvelopeType::kStart ||
      f.posts[2].run_id != 2 || f.posts[3].payload != std::vector<uint8_t>{7} ||
      f.posts[4].payload != std::vector<uint8_t>{8} || f.worker->session->engine_busy_run_id != 2 || !f.conn.out_queue.empty()) {
    *detail = "late engine terminal/successor FIFO";
    return false;
  }
  f.Terminal(2);
  return f.worker->session->engine_busy_run_id == 0 && f.conn.out_queue.size() == 1;
}
bool CreateCloseLoop(std::string *detail) {
  router::Connection conn;
  OpenLocal(conn);
  router::Router service(BrokerMode::kDedicated);
  for (uint32_t id = 1; id <= 1000; ++id) {
    if (!LocalRoute(service, conn, protocol::BuildCreateSessionFrame(ControlHeader(id * 2, id), LogicalConfig())) ||
        !LocalRoute(service, conn, protocol::BuildCloseSessionFrame(ControlHeader(id * 2 + 1, id))) ||
        !conn.sessions.empty() || !conn.retired_sessions.empty()) {
      *detail = "empty retired session retained at iteration " + std::to_string(id);
      return false;
    }
    DrainLocal(conn);
  }
  return conn.highest_session_id == 1000 && conn.request_cache.size() <= 256;
}
bool RelayHoldBeforeReady(std::string *detail) {
  RouterFixture f;
  if (!f.Create() || !f.Start(1) || !f.Relay(1, 2, 9) || !f.Relay(1, 3, 8) || !f.posts.empty() ||
      !f.worker->engine_image)
    return false;
  f.Phase(1);
  if (!f.posts.empty() || f.worker->engine_image || !f.worker->startup || f.worker->security)
    return false;
  f.Phase(2);
  if (f.posts.size() != 3 || f.posts[0].type != v8host::RunEnvelopeType::kStart ||
      f.posts[1].payload != std::vector<uint8_t>(2, 9) || f.posts[2].payload != std::vector<uint8_t>(3, 8)) {
    *detail = "relay hold/release order before readiness";
    return false;
  }
  return true;
}
bool RelayHoldNextRun(std::string *detail) {
  RouterFixture f;
  f.early = true;
  if (!f.Create() || !f.Start(1) || !f.Start(2) || !f.Relay(2, 2, 9) || !f.Relay(2, 3, 8))
    return false;
  f.service->Pump(f.conn);
  if (f.posts.size() != 1 || f.worker->session->engine_busy_run_id != 1 || !f.before_publication)
    return false;
  f.Terminal(1);
  if (f.posts.size() != 4 || f.posts[1].type != v8host::RunEnvelopeType::kStart || f.posts[1].run_id != 2 ||
      f.posts[2].payload != std::vector<uint8_t>(2, 9) || f.posts[3].payload != std::vector<uint8_t>(3, 8)) {
    *detail = "nonselected relay FIFO";
    return false;
  }
  return true;
}
bool ExplicitRequestsFailClosed(std::string *detail) {
  RouterFixture future;
  if (!future.Create()) return false;
  DrainLocal(future.conn);
  const size_t sessions = future.conn.sessions.size();
  const uint32_t highest = future.conn.highest_session_id;
  // Requests that must fail closed, or (closes) be idempotent no-ops: existing
  // and fresh IDs, a session ID named as a worker, and an invalid trailer.
  for (unsigned i : {0u, 1u, 2u, 3u, 5u, 6u}) {
    auto h = ControlHeader(10 + i);
    h.flags = protocol::kFlagMustUnderstand;
    // Exercise both existing IDs and fresh IDs above the admission high-water mark.
    if (i >= 4) h.target_id = i == 4 ? 2 : i == 5 ? 100 : 101;
    const bool create = i < 2 || i >= 4;
    std::vector<uint8_t> frame;
    if (i == 0 || i == 4 || i == 6) {
      protocol::CreateWorkerPayload config;
      config.session = LogicalConfig();
      if (!router::ValidateSessionConfig(config.session, BrokerMode::kDedicated) ||
          !protocol::ValidateCreateWorkerPayload(config)) {
        *detail = "ExplicitRequestsFailClosed: invalid valid-request fixture";
        return false;
      }
      if (i == 6) config.max_runtimes = 0;  // structurally valid, invalid trailer value
      frame = protocol::BuildCreateWorkerFrame(h, config);
    }
    if (i == 1 || i == 5)
      frame = protocol::BuildCreateRuntimeFrame(h, protocol::CreateRuntimePayload{1, 0, 1, -1});
    if (i == 2) frame = protocol::BuildCloseWorkerFrame(h);
    if (i == 3) frame = protocol::BuildCloseRuntimeFrame(h);
    auto unchanged = [&] {
      return future.conn.sessions.size() == sessions &&
          future.conn.highest_session_id == highest && future.spawns == 0;
    };
    if (!LocalRoute(*future.service, future.conn, frame) || future.conn.out_queue.size() != 1 || !unchanged()) {
      *detail = "ExplicitRequestsFailClosed: admission or state mutation";
      return false;
    }
    const auto reply = future.conn.out_queue.front().bytes;
    DrainLocal(future.conn);
    if (!LocalRoute(*future.service, future.conn, frame) || future.conn.out_queue.size() != 1 ||
        future.conn.out_queue.front().bytes != reply || !unchanged()) {
      *detail = "ExplicitRequestsFailClosed: cached replay or state mutation";
      return false;
    }
    protocol::FrameHeader answer;
    protocol::StatusCode code;
    if (!PopFrame(future.conn, &answer, create ? &code : nullptr) ||
        (create ? answer.type != protocol::MessageType::ERROR || code != protocol::StatusCode::ERROR_BAD_STATE
                : answer.type != protocol::MessageType::ACK)) {
      *detail = "ExplicitRequestsFailClosed: wrong response";
      return false;
    }
    if (!create) {
      auto malformed = frame;
      malformed[20] = 1;
      if (LocalRoute(*future.service, future.conn, malformed)) return false;
      malformed = frame;
      malformed[28] = 1;
      malformed.push_back(0);
      if (LocalRoute(*future.service, future.conn, malformed)) return false;
    }
    frame[10] = 0;
    if (LocalRoute(*future.service, future.conn, frame)) return false;
  }
  // A valid CREATE_WORKER for a fresh ID is admitted and spawned once; its
  // duplicate gets the cached ACK.
  auto h = ControlHeader(20, 2);
  h.flags = protocol::kFlagMustUnderstand;
  protocol::CreateWorkerPayload config;
  config.session = LogicalConfig();
  const auto frame = protocol::BuildCreateWorkerFrame(h, config);
  protocol::FrameHeader answer;
  if (!LocalRoute(*future.service, future.conn, frame) || !PopFrame(future.conn, &answer) ||
      answer.type != protocol::MessageType::ACK || !future.conn.workers.contains(2) ||
      future.conn.highest_session_id != 2 || future.spawns != 1) {
    *detail = "ExplicitRequestsFailClosed: valid fresh worker not admitted once";
    return false;
  }
  DrainLocal(future.conn);
  if (!LocalRoute(*future.service, future.conn, frame) || !PopFrame(future.conn, &answer) ||
      answer.type != protocol::MessageType::ACK || future.spawns != 1) {
    *detail = "ExplicitRequestsFailClosed: duplicate worker create not cached";
    return false;
  }
  return true;
}

bool ReadinessProvenance(std::string *detail) {
  for (bool zero : {false, true}) {
    RouterFixture failed;
    failed.early = true;
    failed.fail_pid = !zero;
    failed.zero_pid = zero;
    if (!failed.Create() || !failed.Start(1)) return false;
    for (const auto& queued : failed.conn.out_queue) {
      protocol::FrameHeader h;
      if (protocol::DecodeAndValidateFrame(queued.bytes.data(), queued.bytes.size(), &h) != protocol::DecodeStatus::kOk ||
          h.type == protocol::MessageType::STARTUP_READY) return false;
    }
    failed.service->Close(failed.conn);
    if (failed.pid_reads != 1 || failed.closes != 1 || failed.waits != 1)
      return false;
  }
  if (!ExplicitRequestsFailClosed(detail)) return false;
  RouterFixture f;
  f.early = true;
  if (!f.Create() || !f.Start(1) || !f.before_publication || f.spawns != 1 || !f.worker->startup ||
      !f.worker->security || f.worker->engine_image || f.posts.size() != 1)
    return false;
  bool saw_pid = false;
  for (const auto& queued : f.conn.out_queue) {
    const auto& bytes = queued.bytes;
    protocol::FrameHeader h; const uint8_t* payload = nullptr; size_t size = 0;
    if (protocol::DecodeAndValidateFrame(bytes.data(), bytes.size(), &h, &payload, &size) != protocol::DecodeStatus::kOk)
      return false;
    if (h.type == protocol::MessageType::STARTUP_READY) {
      protocol::StartupReadyPayload ready;
      if (!protocol::DecodeStartupReadyPayload(payload, size, &ready) || ready.process_id != 1234)
        return false;
      saw_pid = true;
    }
  }
  if (!saw_pid || f.worker->process_id != 1234 || f.pid_reads != 1) return false;
  DrainLocal(f.conn);
  f.Phase(1); f.Phase(2);
  if (!f.conn.out_queue.empty() || f.posts.size() != 1)
    return false;
  v8host::RunEnvelope spoof;
  spoof.type = v8host::RunEnvelopeType::kRelay;
  spoof.run_id = 1;
  spoof.relay_kind = sbox_msg_kind_string;
  const std::string marker = "STARTUP_READY SECURITY_READY";
  spoof.payload.assign(marker.begin(), marker.end());
  auto bytes = v8host::EncodeRunEnvelope(spoof);
  router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
  f.service->Pump(f.conn);
  protocol::FrameHeader h;
  if (!PopFrame(f.conn, &h) || h.type != protocol::MessageType::RELAY_FROM_WORKER || !f.conn.out_queue.empty()) {
    *detail = "guest relay gained lifecycle authority";
    return false;
  }
  return true;
}
bool LifecycleWrongWorker(std::string *detail) {
  for (bool ready : {false, true}) {
    RouterFixture f;
    f.wait_entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!f.wait_entered || !f.wait_release || !f.Create() || !f.Start(1))
      return false;
    if (ready) {
      f.Phase(1);
      f.Phase(2);
    }
    DrainLocal(f.conn);
    auto *owner = f.worker;
    const size_t posts = f.posts.size();
    auto stale = std::make_unique<router::Worker>();
    stale->session = owner->session;
    const uint8_t phase[] = {1, 0, 0, 0};
    router::Worker::OnMessage(stale.get(), sbox_msg_kind_lifecycle, phase, sizeof(phase));
    f.service->Pump(f.conn);
    if (::WaitForSingleObject(f.wait_entered, 3000) != WAIT_OBJECT_0 ||
        !f.conn.sessions.empty() || !owner->mailbox_failed || stale->mailbox_tail.load() - stale->mailbox_head.load() ||
        f.closes != 1 || f.waits != 1 || f.posts.size() != posts || !PopWorkerExit(f.conn) ||
        owner->startup != ready || owner->security != ready || bool(owner->engine_image) == ready) {
      *detail = "wrong-worker lifecycle did not fail closed before/after readiness";
      return false;
    }
    ::SetEvent(f.wait_release);
    f.service->Close(f.conn);
    if (f.closes != 1 || f.waits != 1 || !f.conn.retired_sessions.empty())
      return false;
  }
  return true;
}
bool LifecycleOrder(std::string *detail) {
  for (auto [phase, size] : std::vector<std::pair<uint8_t, size_t>>{{2,4},{0,4},{4,4},{1,3},{1,5}}) {
    RouterFixture f;
    f.wait_entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!f.wait_entered || !f.wait_release || !f.Create() || !f.Start(1))
      return false;
    DrainLocal(f.conn);
    auto *worker = f.worker;
    f.Phase(phase, size);
    if (::WaitForSingleObject(f.wait_entered, 3000) != WAIT_OBJECT_0 || !f.conn.sessions.empty() ||
        !worker->engine_image || f.closes != 1 || !f.posts.empty() || !PopWorkerExit(f.conn)) {
      *detail = "invalid lifecycle accepted or image released before cleanup";
      return false;
    }
    ::SetEvent(f.wait_release);
    f.service->Close(f.conn);
    if (f.waits != 1 || f.post_after_close != 0 || !f.conn.retired_sessions.empty())
      return false;
  }
  return true;
}
bool WorkerOutputZeroId(std::string *detail) {
  for (bool idle : {false, true}) {
    for (auto type : {v8host::RunEnvelopeType::kRelay, v8host::RunEnvelopeType::kResult}) {
      RouterFixture f;
      if (!f.Create() || !f.Start(1)) return false;
      f.Phase(1); f.Phase(2);
      if (idle) f.Terminal(1);
      DrainLocal(f.conn);
      auto env = GuestRelay(1, 0);
      env.type = type;
      auto bytes = v8host::EncodeRunEnvelope(env);
      router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
      f.service->Pump(f.conn);
      const bool rejected = f.conn.sessions.empty();
      f.service->Close(f.conn);
      if (!rejected || !f.conn.sessions.empty() || !f.conn.retired_sessions.empty() || f.closes != 1 || f.waits != 1 ||
          f.close_before_removal || f.post_after_close || !PopWorkerExit(f.conn)) {
        *detail = "zero-id worker output was accepted while idle/busy";
        return false;
      }
    }
  }
  return true;
}
bool ProfileConflictInvalid(std::string *detail) {
  RouterFixture f;
  f.early = true;
  if (!f.Create() || !f.Start(1)) return false;
  DrainLocal(f.conn);
  for (uint32_t id : {2, 3}) {
    protocol::StartRunPayload start;
    start.tier_override = v8host::kTierTrusted;
    if (id == 2) {
      start.has_engine_override = true;
      start.engine_filename = "../engine.dll";
    } else {
      start.has_snapshot = true;
      start.snapshot_path = "../other.snapshot";
    }
    const bool accepted = f.Start(id, start);
    if (id == 2) {
      if (accepted || !f.conn.out_queue.empty()) {
        *detail = "malformed engine path bypassed the codec";
        return false;
      }
    } else {
      protocol::FrameHeader h;
      protocol::StatusCode code;
      if (!accepted || !PopFrame(f.conn, &h, &code) || h.type != protocol::MessageType::ERROR ||
          code != protocol::StatusCode::ERROR_BAD_STATE) {
        *detail = "tier conflict masked invalid snapshot inputs";
        return false;
      }
    }
    if (f.conn.sessions.at(1)->highest_run_id != 1 ||
        f.conn.sessions.at(1)->runs.size() != 1 || f.posts.size() != 1 || f.spawns != 1 || f.closes) {
      *detail = "tier conflict masked invalid profile inputs";
      return false;
    }
  }
  return true;
}
bool FirstRunBinding(std::string *detail) {
  RouterFixture f;
  f.early = true;
  if (!f.Create() || !f.Start(1))
    return false;
  DrainLocal(f.conn);
  uint32_t id = 2;
  for (int which = 0; which < 3; ++which, ++id) {
    protocol::StartRunPayload start;
    start.tier_override = -1;
    if (which == 0)
      start.tier_override = v8host::kTierTrusted;
    else if (which == 1) {
      start.has_engine_override = true;
      start.engine_filename = "other.dll";
    } else {
      start.has_snapshot = true;
      start.snapshot_path = "other.snapshot";
    }
    if (!f.Start(id, start))
      return false;
    protocol::FrameHeader h;
    protocol::StatusCode code;
    if (!PopFrame(f.conn, &h) || h.type != protocol::MessageType::ACK ||
        !PopFrame(f.conn, &h, &code) || h.type != protocol::MessageType::RUN_ERROR ||
        code != protocol::StatusCode::ERROR_PROFILE_ALREADY_BOUND || h.run_id != id || h.request_id != 0 ||
        f.spawns != 1 || f.posts.size() != 1 || f.worker->session->engine_busy_run_id != 1 || f.conn.sessions.at(1)->profile.engine_dll != "v8jsisb.dll") {
      *detail = "profile conflict disturbed worker or terminal code";
      return false;
    }
  }
  f.Terminal(1);
  if (!f.Start(id) || f.posts.size() != 2 || f.posts.back().run_id != id || f.spawns != 1)
    return false;
  f.Terminal(id);
  return true;
}
bool StartAdmissionQuota(std::string *detail) {
  RouterFixture f;
  if (!f.Create())
    return false;
  DrainLocal(f.conn);
  for (uint32_t id = 1; id <= 4; ++id) {
    if (!f.Start(id))
      return false;
    DrainLocal(f.conn);
  }
  if (!f.Start(5))
    return false;
  protocol::FrameHeader h;
  protocol::StatusCode code;
  if (!PopFrame(f.conn, &h, &code) || h.type != protocol::MessageType::ERROR ||
      code != protocol::StatusCode::ERROR_QUOTA || f.conn.sessions.at(1)->runs.size() != 4 || f.spawns != 1) {
    *detail = "in-flight 4/+1 admission";
    return false;
  }
  return true;
}
bool EnvelopeSizeBoundary(std::string *detail) {
  if (!router::EnvelopeFits(v8host::RunEnvelopeType::kStart, 65516) ||
      router::EnvelopeFits(v8host::RunEnvelopeType::kStart, 65517) ||
      !router::EnvelopeFits(v8host::RunEnvelopeType::kRelay, 65512) ||
      router::EnvelopeFits(v8host::RunEnvelopeType::kRelay, 65513) ||
      router::EnvelopeFits(v8host::RunEnvelopeType::kRelay, SIZE_MAX))
    return false;
  RouterFixture f;
  f.early = true;
  if (!f.Create() || !f.Start(1) || !f.Relay(1, 65512) || f.posts.size() != 2 ||
      f.posts.back().payload.size() != 65512)
    return false;
  DrainLocal(f.conn);
  if (!f.Relay(1, 65513))
    return false;
  protocol::FrameHeader h;
  protocol::StatusCode code;
  if (!PopFrame(f.conn, &h, &code) || h.type != protocol::MessageType::RUN_ERROR || h.request_id != 0 ||
      code != protocol::StatusCode::ERROR_QUOTA || f.posts.back().type != v8host::RunEnvelopeType::kCancel) {
    *detail = "encoded oversize did not terminalize addressed run";
    return false;
  }
  return true;
}
bool TerminalContenderOrderings(std::string *detail) {
  // Each first/second pair has a held callback-reader join, not a timing race.
  for (int first = 0; first < 5; ++first) for (int second = 0; second < 5; ++second) {
    RouterFixture f; f.early = true;
    f.wait_entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!f.wait_entered || !f.wait_release || !f.Create() || !f.Start(1) || !f.Start(2)) return false;
    DrainLocal(f.conn);
    auto *worker = f.worker;
    std::atomic<unsigned> removals{0};
    worker->session->terminal_removed = [&] { ++removals; };
    bool disconnected = false, passed = true;
    std::thread disconnect;
    auto actor = [&](int action) {
      if (action == 4 && !disconnected) {
        disconnected = true;
        disconnect = std::thread([&] { f.service->Close(f.conn); });
        passed = passed && ::WaitForSingleObject(f.wait_entered, 5000) == WAIT_OBJECT_0;
      } else if (action == 3 && !disconnected && f.conn.sessions.contains(1)) {
        passed = passed && LocalRoute(*f.service, f.conn, protocol::BuildCloseSessionFrame(ControlHeader(20)));
      } else if (action <= 2) {
        if (action == 2) {
          const uint8_t phase[] = {sbox_lifecycle_phase_exit, 0, 0, 0};
          router::Worker::OnMessage(worker, sbox_msg_kind_lifecycle, phase, 4);
        } else {
          v8host::RunEnvelope env; env.run_id = 1;
          env.type = action == 0 ? v8host::RunEnvelopeType::kResult : v8host::RunEnvelopeType::kRunError;
          auto bytes = v8host::EncodeRunEnvelope(env);
          router::Worker::OnMessage(worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
        }
        if (!disconnected) f.service->Pump(f.conn);
      }
      if (worker->close_called)
        passed = passed && ::WaitForSingleObject(f.wait_entered, 5000) == WAIT_OBJECT_0 &&
            !f.conn.sessions.contains(1) && worker->session->runs.empty();
    };
    actor(first); actor(second);
    if (!worker->close_called) actor(3);
    // Late facts cannot rebind while the callback context is still retained.
    const size_t tail = worker->mailbox_tail;
    for (int action = 0; action < 3; ++action) actor(action);
    passed = passed && worker->mailbox_tail == tail && removals == 2 && !worker->session->engine_busy_run_id &&
        worker->session->cancel_deadline == std::chrono::steady_clock::time_point{} &&
        worker->retry_deadline == std::chrono::steady_clock::time_point{};
    ::SetEvent(f.wait_release);
    if (disconnect.joinable()) disconnect.join(); else f.service->Close(f.conn);
    unsigned terminals[3] = {}, exits = 0;
    protocol::FrameHeader h;
    while (PopFrame(f.conn, &h)) {
      if (h.type == protocol::MessageType::RESULT || h.type == protocol::MessageType::RUN_ERROR) {
        if (!h.run_id || h.run_id > 2 || ++terminals[h.run_id] > 1) passed = false;
      }
      if (h.type == protocol::MessageType::WORKER_EXIT && ++exits > 1) passed = false;
    }
    if (!passed || !f.conn.CompleteZero() || f.closes != 1 || f.waits != 1 || f.close_before_removal || f.post_after_close ||
        f.posts.size() > 2) {
      *detail = "terminal contender pair " + std::to_string(first) + "/" + std::to_string(second); return false;
    }
  }
  return true;
}
bool CompleteZeroDrain(std::string *detail) {
  RouterFixture f; f.early = true;
  std::atomic<int64_t> ticks{1000}; std::atomic<unsigned> clocks{0}, posts{0};
  f.service->test_now = [&] { ++clocks; return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(ticks.load()); };
  f.wait_entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!f.wait_entered || !f.wait_release || !f.Create() || !f.Start(1)) return false;
  f.service->test_post = [&](const std::vector<uint8_t> &) { ++posts; return router::PostOutcome::kNotWritten; };
  if (!LocalRoute(*f.service, f.conn, protocol::BuildCancelRunFrame(ControlHeader(9, 1, 1)))) return false;
  DrainLocal(f.conn);
  auto *worker = f.worker;
  if (f.conn.CompleteZero() || worker->session->cancel_deadline == std::chrono::steady_clock::time_point{} ||
      worker->retry_deadline == std::chrono::steady_clock::time_point{}) return false;
  f.conn.writer_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!f.conn.writer_release || !f.conn.StartWriter()) return false;
  f.conn.dispatcher = f.service.get();
  HANDLE joined = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!joined) return false;
  std::thread closer([&] { f.service->Close(f.conn); ::SetEvent(joined); });
  const bool entered = ::WaitForSingleObject(f.wait_entered, 5000) == WAIT_OBJECT_0;
  bool passed = entered && ::WaitForSingleObject(joined, 0) == WAIT_TIMEOUT && !f.conn.CompleteZero() &&
      f.conn.sessions.empty() && f.conn.retired_sessions.size() == 1 && worker->session->runs.empty() &&
      worker->session->cancel_deadline == std::chrono::steady_clock::time_point{} &&
      worker->retry_deadline == std::chrono::steady_clock::time_point{};
  const unsigned calls = clocks;
  ticks = 100000;
  const uint8_t exit[] = {sbox_lifecycle_phase_exit, 0, 0, 0};
  router::Worker::OnMessage(worker, sbox_msg_kind_lifecycle, exit, 4);
  passed = passed && clocks == calls && posts == 1;
  ::SetEvent(f.wait_release); closer.join(); ::CloseHandle(joined);
  // A registered writer prevents zero even after worker cleanup joins.
  passed = passed && !f.conn.CompleteZero() && f.conn.retired_sessions.empty() && !f.conn.dispatcher;
  f.conn.JoinWriter(); ::CloseHandle(f.conn.writer_release); f.conn.writer_release = nullptr;
  f.conn.ClearOutput();
  if (!passed || !f.conn.CompleteZero() || f.conn.writers_started != 1 || f.conn.writers_joined != 1 ||
      f.closes != 1 || f.waits != 1 || f.post_after_close || f.close_before_removal) {
    *detail = "cleanup/timer/retry/writer/clock borrower escaped joined zero"; return false;
  }
  return true;
}
bool JoinedTeardown(std::string *detail) {
  RouterFixture f; f.early = true;
  f.wait_entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!f.wait_entered || !f.wait_release) return false;
  uint32_t request = 0;
  for (uint32_t id = 1; id <= 100; ++id) {
    ::ResetEvent(f.wait_entered); ::ResetEvent(f.wait_release);
    if (!LocalRoute(*f.service, f.conn, protocol::BuildCreateSessionFrame(ControlHeader(++request, id), LogicalConfig())))
      return false;
    protocol::StartRunPayload start; start.tier_override = -1; start.guest_payload = {'x'};
    if (!LocalRoute(*f.service, f.conn, protocol::BuildStartRunFrame(ControlHeader(++request, id, 1), start))) return false;
    auto *worker = f.worker;
    if (!LocalRoute(*f.service, f.conn, protocol::BuildCloseSessionFrame(ControlHeader(++request, id))) ||
        ::WaitForSingleObject(f.wait_entered, 5000) != WAIT_OBJECT_0 || f.conn.retired_sessions.size() != 1) return false;
    HANDLE cleanup = worker->cleanup;
    ::SetEvent(f.wait_release);
    if (::WaitForSingleObject(cleanup, 5000) != WAIT_OBJECT_0) return false;
    f.service->Pump(f.conn); DrainLocal(f.conn);
    if (!f.conn.retired_sessions.empty() || !f.conn.sessions.empty() || f.conn.in_relay_bytes || f.conn.out_relay_bytes ||
        f.service->WakeDelay(f.conn, INFINITE) != INFINITE) {
      *detail = "churn retained joined object/task or deadline"; return false;
    }
  }
  f.service->Close(f.conn);
  return f.conn.CompleteZero() && f.closes == 100 && f.waits == 100 && !f.post_after_close && !f.close_before_removal;
}
struct PressurePipe {
  HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
  HANDLE paused = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE resumed = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE pending = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool Open(bool overlapped = false) {
    const auto name = LR"(\\.\pipe\v8host-pressure-)" + std::to_wstring(::GetCurrentProcessId());
    server = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
    client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, overlapped ? FILE_FLAG_OVERLAPPED : 0, nullptr);
    OVERLAPPED connect = {}; connect.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const bool ok = server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE && paused && resumed && pending &&
        connect.hEvent && (::ConnectNamedPipe(server, &connect) || ::GetLastError() == ERROR_PIPE_CONNECTED);
    if (connect.hEvent) ::CloseHandle(connect.hEvent);
    DWORD mode = PIPE_READMODE_MESSAGE;
    return ok && ::SetNamedPipeHandleState(client, &mode, nullptr, nullptr);
  }
  ~PressurePipe() {
    if (server != INVALID_HANDLE_VALUE) ::CloseHandle(server);
    if (client != INVALID_HANDLE_VALUE) ::CloseHandle(client);
    if (paused) ::CloseHandle(paused);
    if (resumed) ::CloseHandle(resumed);
    if (pending) ::CloseHandle(pending);
  }
};
bool PressureTransfer(HANDLE pipe, bool write, void *data, DWORD capacity, DWORD *size) {
  OVERLAPPED ov = {}; ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent) return false;
  const BOOL started = write ? ::WriteFile(pipe, data, capacity, nullptr, &ov) :
      ::ReadFile(pipe, data, capacity, nullptr, &ov);
  const bool pending = !started && ::GetLastError() == ERROR_IO_PENDING;
  bool ok = started || (pending && ::WaitForSingleObject(ov.hEvent, 5000) == WAIT_OBJECT_0);
  if (ok) ok = ::GetOverlappedResult(pipe, &ov, size, FALSE) != FALSE;
  if (pending && !ok) { ::CancelIoEx(pipe, &ov); ::GetOverlappedResult(pipe, &ov, size, TRUE); }
  ::CloseHandle(ov.hEvent);
  return ok && (!write || *size == capacity);
}
bool PauseResume(std::string *detail) {
  RouterFixture f; f.early = true; PressurePipe pipe;
  if (!f.Create() || !f.Start(1)) return false;
  DrainLocal(f.conn);
  for (size_t i = 0; i < protocol::kMaxControlQueueRequests - 1; ++i)
    if (!f.conn.Enqueue(protocol::BuildAckFrame(ControlHeader(1)))) return false;
  if (!pipe.Open(true) || !f.conn.ReadsPaused()) return false;
  f.conn.pipe = pipe.server; f.conn.dispatcher = f.service.get();
  f.conn.read_paused = pipe.paused; f.conn.read_resumed = pipe.resumed; f.conn.read_pending = pipe.pending;
  HANDLE release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!release) return false;
  f.conn.writer_release = release;
  bool read = false; uint8_t byte = 0; DWORD size = 0;
  // A finite zero budget must never become an indefinite paused wait.
  if (f.conn.ReadFrame(&byte, 1, &size, 0) || f.conn.active_io || !f.conn.StartWriter()) {
    ::CloseHandle(release); return false;
  }
  ::ResetEvent(pipe.paused);
  std::thread reader([&] { read = f.conn.ReadFrame(&byte, 1, &size, INFINITE); });
  bool passed = ::WaitForSingleObject(pipe.paused, 5000) == WAIT_OBJECT_0 && f.conn.active_io == 0;
  auto encoded = v8host::EncodeRunEnvelope(GuestRelay(1));
  if (passed) router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, encoded.data(), encoded.size());
  ::SetEvent(release);
  // Real writer progress: controls followed by active-run output, while the
  // strand services mailbox wakes independently of admitting a new pipe read.
  std::array<uint8_t, protocol::kMaxFrameSize> output;
  for (size_t i = 0; i < protocol::kMaxControlQueueRequests && passed; ++i) {
    DWORD count = 0; protocol::FrameHeader h; const uint8_t *body; size_t body_size;
    passed = PressureTransfer(pipe.client, false, output.data(), static_cast<DWORD>(output.size()), &count) &&
        protocol::DecodeAndValidateFrame(output.data(), count, &h, &body, &body_size) == protocol::DecodeStatus::kOk &&
        (i < protocol::kMaxControlQueueRequests - 1 ? h.type == protocol::MessageType::ACK :
            h.type == protocol::MessageType::RELAY_FROM_WORKER && h.run_id == 1 && output[count - 1] == 7);
  }
  passed = passed && ::WaitForSingleObject(pipe.resumed, 5000) == WAIT_OBJECT_0 &&
      ::WaitForSingleObject(pipe.pending, 5000) == WAIT_OBJECT_0;
  if (passed) {
    uint8_t expected = 7; DWORD written;
    passed = PressureTransfer(pipe.client, true, &expected, 1, &written);
  }
  if (!passed) f.conn.Stop();
  reader.join(); f.conn.Stop(); f.conn.JoinWriter();
  f.conn.writer_release = nullptr; ::CloseHandle(release);
  f.conn.pipe = INVALID_HANDLE_VALUE; f.conn.read_paused = f.conn.read_resumed = f.conn.read_pending = nullptr;
  if (!passed || !read || byte != 7 || size != 1 || f.conn.active_io) {
    *detail = "connection output pause blocked active-run writer progress or read resume"; return false;
  }
  f.service->Close(f.conn); f.conn.ClearOutput();
  return f.conn.CompleteZero();
}
// A real pending pipe read proves run-local pressure does not suppress intake.
bool ReadThroughPressure(RouterFixture &f, const std::vector<uint8_t> &frame) {
  PressurePipe pipe;
  if (!pipe.Open() || f.conn.ReadsPaused()) return false;
  f.conn.pipe = pipe.server; f.conn.dispatcher = f.service.get(); f.conn.read_pending = pipe.pending;
  std::vector<uint8_t> received(protocol::kMaxFrameSize); DWORD size = 0; bool read = false;
  std::thread reader([&] { read = f.conn.ReadFrame(received.data(), static_cast<DWORD>(received.size()), &size, INFINITE); });
  bool passed = ::WaitForSingleObject(pipe.pending, 5000) == WAIT_OBJECT_0;
  DWORD written = 0;
  if (passed) passed = ::WriteFile(pipe.client, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) &&
      written == frame.size();
  if (!passed) f.conn.Stop();
  reader.join(); f.conn.pipe = INVALID_HANDLE_VALUE; f.conn.read_pending = nullptr;
  return passed && read && size == frame.size() && !f.conn.active_io &&
      LocalRoute(*f.service, f.conn, std::vector<uint8_t>(received.begin(), received.begin() + size));
}
bool RunPressureContainment(std::string *detail) {
  RouterFixture f; f.early = true;
  if (!f.Create() || !f.Start(1) || !f.Start(2)) return false;
  DrainLocal(f.conn);
  size_t bytes = protocol::kMaxQueuedRelayBytesPerRun;
  while (bytes) {
    const size_t part = (std::min)(bytes, size_t(protocol::kMaxFramePayload));
    if (part < 16 || !f.Relay(2, part - 16)) return false;
    bytes -= part;
  }
  auto h = ControlHeader(0, 1, 1); h.type = protocol::MessageType::RELAY_TO_WORKER;
  const uint8_t input = 9;
  if (!ReadThroughPressure(f, protocol::BuildRelayFrame(h, sbox_msg_kind_binary, &input, 1)) ||
      f.posts.size() != 2 || f.posts.back().payload != std::vector<uint8_t>{9} || f.conn.ReadsPaused()) {
    *detail = "queued run held active-run input behind a connection pause"; return false;
  }
  if (!ReadThroughPressure(f, protocol::BuildCreateSessionFrame(ControlHeader(4, 2), LogicalConfig())) ||
      !f.conn.sessions.contains(2) || !f.Relay(2, 1)) return false;
  protocol::FrameHeader reply;
  bool quota = false;
  std::vector<uint8_t> output;
  while (f.conn.TakeOutput(&output)) {
    f.conn.CompleteOutput(output.size());
    const uint8_t *body; size_t size;
    if (protocol::DecodeAndValidateFrame(output.data(), output.size(), &reply, &body, &size) != protocol::DecodeStatus::kOk)
      return false;
    if (reply.type == protocol::MessageType::RUN_ERROR) {
      protocol::ErrorPayload payload;
      quota = protocol::DecodeErrorPayload(body, size, &payload) && reply.target_id == 1 && reply.run_id == 2 &&
          !reply.request_id && payload.status_code == protocol::StatusCode::ERROR_QUOTA;
    }
  }
  protocol::StartRunPayload start; start.tier_override = -1; start.guest_payload = {'x'};
  if (!quota || !f.conn.sessions.at(1)->runs.contains(1) || f.conn.sessions.at(1)->runs.contains(2) ||
      f.conn.in_relay_bytes || f.conn.ReadsPaused() ||
      !ReadThroughPressure(f, protocol::BuildStartRunFrame(ControlHeader(5, 2, 11), start)) ||
      f.posts.back().run_id != 11 || f.closes) {
    *detail = "per-run input quota escaped its run or blocked another session"; return false;
  }
  return true;
}
bool RetrySessionContainment(std::string *detail) {
  RouterFixture f; f.early = true;
  f.service->test_now = [] { return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(1000); };
  f.service->test_post = [&](const std::vector<uint8_t> &bytes) {
    v8host::RunEnvelope env;
    if (!v8host::DecodeRunEnvelope(bytes.data(), bytes.size(), &env)) return router::PostOutcome::kAmbiguous;
    if (env.run_id == 1) return router::PostOutcome::kNotWritten;
    f.posts.push_back(env); return router::PostOutcome::kWritten;
  };
  if (!f.Create() || !f.Start(1)) return false;
  auto *slow = f.worker; DrainLocal(f.conn);
  protocol::StartRunPayload start; start.tier_override = -1; start.guest_payload = {'x'};
  if (!ReadThroughPressure(f, protocol::BuildCreateSessionFrame(ControlHeader(3, 2), LogicalConfig())) ||
      !ReadThroughPressure(f, protocol::BuildStartRunFrame(ControlHeader(4, 2, 11), start)) ||
      slow->retry_deadline == std::chrono::steady_clock::time_point{} || f.conn.ReadsPaused() ||
      f.posts.size() != 1 || f.posts.front().run_id != 11 || f.closes) {
    *detail = "a slow guest's pending retry blocked another session's pipe request"; return false;
  }
  return true;
}
bool PausedDisconnect(std::string *detail) {
  RouterFixture f; f.early = true; PressurePipe pipe;
  if (!f.Create() || !f.Start(1) || !pipe.Open()) return false;
  DrainLocal(f.conn);
  for (size_t i = 0; i < protocol::kMaxControlQueueRequests - 1; ++i)
    if (!f.conn.Enqueue(protocol::BuildAckFrame(ControlHeader(1)))) return false;
  f.conn.pipe = pipe.server; f.conn.dispatcher = f.service.get(); f.conn.read_paused = pipe.paused;
  HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!done) return false;
  bool read = true; uint8_t byte; DWORD size;
  std::thread reader([&] { read = f.conn.ReadFrame(&byte, 1, &size, INFINITE); ::SetEvent(done); });
  bool passed = ::WaitForSingleObject(pipe.paused, 5000) == WAIT_OBJECT_0 && !f.conn.active_io;
  ::CloseHandle(pipe.client); pipe.client = INVALID_HANDLE_VALUE;
  passed = passed && ::WaitForSingleObject(done, 5000) == WAIT_OBJECT_0;
  if (!passed) f.conn.Stop();
  reader.join(); ::CloseHandle(done); f.conn.pipe = INVALID_HANDLE_VALUE; f.conn.read_paused = nullptr;
  f.service->Close(f.conn); f.conn.ClearOutput();
  if (!passed || read || !f.conn.CompleteZero() || f.closes != 1 || f.waits != 1) {
    *detail = "disconnect during connection-level pause was not detected and reaped"; return false;
  }
  return true;
}
bool BookkeepingCallbackOverlap(std::string *detail) {
  for (bool terminal : {false, true}) {
    RouterFixture f; f.early = true;
    if (!f.Create() || !f.Start(1) || (terminal && !f.Start(2))) return false;
    DrainLocal(f.conn);
    HANDLE entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!entered || !release) return false;
    f.worker->ledger_publication = [&] { ::SetEvent(entered); ::WaitForSingleObject(release, INFINITE); };
    bool admitted = false;
    std::thread strand([&] {
      admitted = terminal ? f.conn.sessions.at(1)->runs.at(2)->TryTerminal(router::RunState::kCancelled) : f.Start(2);
    });
    bool passed = ::WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0;
    auto encoded = v8host::EncodeRunEnvelope(GuestRelay(1));
    if (passed) router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, encoded.data(), encoded.size());
    passed = passed && !f.worker->mailbox_failed && f.conn.out_relay_bytes == 37;
    ::SetEvent(release); strand.join(); f.worker->ledger_publication = nullptr;
    ::CloseHandle(entered); ::CloseHandle(release); f.service->Pump(f.conn);
    size_t relays = 0; protocol::FrameHeader h;
    while (PopFrame(f.conn, &h)) if (h.type == protocol::MessageType::RELAY_FROM_WORKER && h.run_id == 1) ++relays;
    if (!passed || !admitted || relays != 1 || f.conn.sessions.size() != 1 || f.closes ||
        f.worker->mailbox_failed || f.worker->ledger_pins.size() > 5) {
      *detail = terminal ? "terminal bookkeeping killed a healthy callback" : "START admission killed a healthy callback";
      return false;
    }
  }
  return true;
}
bool LedgerHazardChurn(std::string *detail) {
  RouterFixture f; f.early = true;
  if (!f.Create() || !f.Start(1)) return false;
  DrainLocal(f.conn);
  HANDLE entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr), release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!entered || !release) return false;
  auto weak = std::weak_ptr(f.conn.sessions.at(1)->runs.at(1)->outbound);
  f.worker->ledger_acquired = [&] { ::SetEvent(entered); ::WaitForSingleObject(release, INFINITE); };
  auto encoded = v8host::EncodeRunEnvelope(GuestRelay(1));
  std::thread callback([&] { router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, encoded.data(), encoded.size()); });
  bool passed = ::WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0;
  if (passed) {
    f.conn.sessions.at(1)->runs.at(1)->TryTerminal(router::RunState::kFailed);
    f.conn.sessions.at(1)->retired_runs.clear();
    for (uint32_t id = 2; id < 102 && passed; ++id) {
      passed = f.Start(id);
      if (passed) passed = f.conn.sessions.at(1)->runs.at(id)->TryTerminal(router::RunState::kCancelled);
      f.conn.sessions.at(1)->retired_runs.clear(); DrainLocal(f.conn);
      passed = passed && !weak.expired() && f.worker->ledger_pins.size() <= 5;
    }
  }
  ::SetEvent(release); callback.join(); f.worker->ledger_acquired = nullptr;
  ::CloseHandle(entered); ::CloseHandle(release); f.service->Pump(f.conn); DrainLocal(f.conn);
  if (passed) passed = f.Start(102) && weak.expired() && !f.worker->mailbox_failed;
  if (!passed) *detail = "reader hazard lost a retired ledger or accumulated unbounded churn pins";
  return passed;
}
bool OutputLossSticky(std::string *detail) {
  for (auto terminal : {v8host::RunEnvelopeType::kResult, v8host::RunEnvelopeType::kRunError}) {
    RouterFixture f; f.early = true;
    if (!f.Create() || !f.Start(1) || !f.Start(2)) return false;
    DrainLocal(f.conn);
    auto ledger = f.conn.sessions.at(1)->runs.at(1)->outbound;
    auto held = f.conn.ChargeOutput(protocol::kMaxQueuedRelayBytesPerRun - 36, ledger);
    auto first = v8host::EncodeRunEnvelope(GuestRelay(0));
    router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, first.data(), first.size());
    bool injected = false, rejected = false;
    f.worker->mailbox_readout = [&] {
      if (injected) return;
      injected = true;
      auto overflow = v8host::EncodeRunEnvelope(GuestRelay(1));
      const size_t tail = f.worker->mailbox_tail;
      router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, overflow.data(), overflow.size());
      held.Reset();
      router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, first.data(), first.size());
      rejected = ledger->failed && f.worker->mailbox_tail == tail;
      v8host::RunEnvelope result; result.type = terminal; result.run_id = 1;
      auto bytes = v8host::EncodeRunEnvelope(result);
      router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
    };
    f.service->Pump(f.conn); f.worker->mailbox_readout = nullptr;
    size_t errors = 0; std::vector<uint8_t> output;
    while (f.conn.TakeOutput(&output)) {
      f.conn.CompleteOutput(output.size()); protocol::FrameHeader h; const uint8_t *body; size_t size;
      if (protocol::DecodeAndValidateFrame(output.data(), output.size(), &h, &body, &size) != protocol::DecodeStatus::kOk)
        return false;
      if (h.type != protocol::MessageType::RUN_ERROR || h.run_id != 1 || h.request_id) return false;
      protocol::ErrorPayload error;
      if (!protocol::DecodeErrorPayload(body, size, &error) || error.status_code != protocol::StatusCode::ERROR_QUOTA)
        return false;
      ++errors;
    }
    if (!injected || !rejected || errors != 1 || ledger->bytes || f.worker->mailbox_failed || f.closes ||
        f.conn.sessions.at(1)->runs.contains(1) || !f.conn.sessions.at(1)->runs.contains(2) ||
        f.worker->session->engine_busy_run_id != 2) {
      *detail = "same-drain loss allowed success, post-gap delivery, or stranded the busy cursor"; return false;
    }
  }
  return true;
}
bool DiscardIntakeOrdering(std::string *detail) {
  RouterFixture f; f.early = true;
  if (!f.Create() || !f.Start(1)) return false;
  DrainLocal(f.conn);
  HANDLE entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr), release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!entered || !release) return false;
  f.worker->intake_entered = [&] { ::SetEvent(entered); ::WaitForSingleObject(release, INFINITE); };
  auto *worker = f.worker;
  auto encoded = v8host::EncodeRunEnvelope(GuestRelay(1));
  std::thread callback([&] { router::Worker::OnMessage(worker, sbox_msg_kind_binary, encoded.data(), encoded.size()); });
  bool passed = ::WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0;
  const size_t tail = worker->mailbox_tail;
  worker->DiscardMailbox();
  ::SetEvent(release); callback.join(); worker->intake_entered = nullptr;
  ::CloseHandle(entered); ::CloseHandle(release);
  if (!passed || worker->mailbox_tail != tail || f.conn.out_relay_bytes || worker->intake_active) {
    *detail = "callback published a late credit after mailbox discard"; return false;
  }
  return true;
}
bool TerminalBeforeFallback(std::string *detail) {
  RouterFixture f; f.early = true;
  int64_t ticks = 1000;
  f.service->test_now = [&] { return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(ticks); };
  if (!f.Create() || !f.Start(1) || !f.Start(2) ||
      !LocalRoute(*f.service, f.conn, protocol::BuildCancelRunFrame(ControlHeader(4, 1, 1)))) return false;
  DrainLocal(f.conn);
  ticks = 5999;
  v8host::RunEnvelope result; result.type = v8host::RunEnvelopeType::kResult; result.run_id = 1;
  result.disposition = v8host::RunEnvelopeDisposition::kCancelled;
  auto bytes = v8host::EncodeRunEnvelope(result);
  router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
  ticks = 6000; f.service->Pump(f.conn);
  protocol::FrameHeader h;
  if (f.closes || f.conn.sessions.empty() || f.worker->session->cancel_deadline != std::chrono::steady_clock::time_point{} ||
      f.worker->session->engine_busy_run_id != 2 || !PopFrame(f.conn, &h) || h.type != protocol::MessageType::RESULT ||
      h.run_id != 1 || PopFrame(f.conn, &h)) {
    *detail = "already-published worker terminal lost to a later cancel fallback pump"; return false;
  }
  return true;
}
bool FinalReplyMailbox(std::string *detail) {
  RouterFixture f; f.early = true; PressurePipe pipe;
  if (!f.Create() || !f.Start(1) || !pipe.Open()) return false;
  DrainLocal(f.conn);
  auto encoded = v8host::EncodeRunEnvelope(GuestRelay(1));
  router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, encoded.data(), encoded.size());
  // Keep both relay and control credits in unpumped worker mailboxes.
  const uint8_t startup[] = {1, 0, 0, 0};
  router::Worker::OnMessage(f.worker, sbox_msg_kind_lifecycle, startup, sizeof(startup));
  auto h = ControlHeader(19, 0, 3); h.type = static_cast<protocol::MessageType>(0xffff);
  h.flags = protocol::kFlagMustUnderstand;
  std::vector<uint8_t> unknown; protocol::EncodeHeader(h, unknown);
  if (LocalRoute(*f.service, f.conn, unknown) || f.conn.state != router::ConnState::kClosing ||
      f.conn.out_control_requests != 2 || !f.conn.out_relay_bytes) return false;
  f.conn.pipe = pipe.server;
  if (!f.conn.StartWriter()) return false;
  const bool preserved = f.conn.DrainOutput(5000);
  f.conn.Stop(); f.conn.JoinWriter(); f.service->Close(f.conn); f.conn.ClearOutput();
  if (!preserved) ::DisconnectNamedPipe(pipe.server);
  ::CloseHandle(pipe.server); pipe.server = INVALID_HANDLE_VALUE; f.conn.pipe = INVALID_HANDLE_VALUE;
  std::array<uint8_t, protocol::kMaxFrameSize> bytes; DWORD size = 0;
  protocol::FrameHeader reply; const uint8_t *body; size_t body_size; protocol::ErrorPayload error;
  if (!preserved || !::ReadFile(pipe.client, bytes.data(), static_cast<DWORD>(bytes.size()), &size, nullptr) ||
      protocol::DecodeAndValidateFrame(bytes.data(), size, &reply, &body, &body_size) != protocol::DecodeStatus::kOk ||
      reply.type != protocol::MessageType::ERROR || reply.request_id != 19 ||
      !protocol::DecodeErrorPayload(body, body_size, &error) ||
      error.status_code != protocol::StatusCode::ERROR_UNSUPPORTED_MESSAGE || !f.conn.CompleteZero()) {
    *detail = "mailbox credits withheld the mandatory error or close discarded it"; return false;
  }
  return true;
}
bool WorkerMailboxOverflow(std::string *detail) {
  for (int delta : {-1, 0, 1}) {
    RouterFixture f; f.early = true;
    if (!f.Create() || !f.Start(1) || !f.Start(2)) return false;
    DrainLocal(f.conn);
    size_t bytes = protocol::kMaxQueuedRelayBytesPerRun + delta, n = 0;
    while (bytes) {
      const size_t part = (std::min)(bytes, size_t(protocol::kMaxFramePayload));
      auto encoded = v8host::EncodeRunEnvelope(GuestRelay(part - 16, ++n % 2 ? 1 : 2));
      router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, encoded.data(), encoded.size());
      bytes -= part;
    }
    if (f.worker->mailbox_failed != (delta > 0) ||
        f.worker->byte_tail - f.worker->byte_head > protocol::kMaxQueuedRelayBytesPerRun) return false;
    f.service->Pump(f.conn);
    if (f.conn.sessions.empty() != (delta > 0)) return false;
    f.service->Close(f.conn); f.conn.ClearOutput();
    if (!f.conn.CompleteZero()) return false;
  }
  for (bool malformed : {false, true}) {
    RouterFixture f; f.early = true;
    f.wait_entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!f.wait_entered || !f.wait_release || !f.Create() || !f.Start(1)) return false;
    DrainLocal(f.conn);
    auto *worker = f.worker;
    auto encoded = v8host::EncodeRunEnvelope(GuestRelay(1));
    if (!malformed) for (size_t i = 0; i < worker->mailbox.size(); ++i)
      router::Worker::OnMessage(worker, sbox_msg_kind_binary, encoded.data(), encoded.size());
    router::Worker::OnMessage(worker, sbox_msg_kind_binary, malformed ? nullptr : encoded.data(), encoded.size());
    const bool retained_failure = worker->mailbox_failed;
    f.service->Pump(f.conn);
    if (!retained_failure || !f.conn.sessions.empty() || ::WaitForSingleObject(f.wait_entered, 5000) != WAIT_OBJECT_0)
      return false;
    const size_t tail = worker->mailbox_tail;
    router::Worker::OnMessage(worker, sbox_msg_kind_binary, encoded.data(), encoded.size());
    if (worker->mailbox_tail != tail || f.conn.out_relay_bytes || !PopWorkerExit(f.conn)) return false;
    ::SetEvent(f.wait_release); f.service->Close(f.conn);
    if (!f.conn.CompleteZero() || f.waits != 1 || f.closes != 1 || f.post_after_close || f.close_before_removal) return false;
  }
  RouterFixture f; f.early = true;
  if (!f.Create() || !f.Start(1)) return false;
  DrainLocal(f.conn);
  f.worker->byte_head = SIZE_MAX - 10; f.worker->byte_tail = SIZE_MAX - 10;
  for (uint8_t i = 0; i < 64; ++i) {
    auto encoded = v8host::EncodeRunEnvelope(GuestRelay(1)); encoded.back() = i;
    router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, encoded.data(), encoded.size());
  }
  f.service->Pump(f.conn);
  for (uint8_t i = 0; i < 64; ++i) {
    std::vector<uint8_t> frame;
    if (!f.conn.TakeOutput(&frame) || frame.back() != i) { *detail = "64-fact burst or byte-ring wrap lost FIFO"; return false; }
    f.conn.CompleteOutput(frame.size());
  }
  return !f.worker->mailbox_failed && !f.conn.out_relay_bytes && f.worker->byte_head == f.worker->byte_tail;
}
bool IdExhaustion(std::string *detail) {
  std::atomic<uint32_t> next{UINT32_MAX - 2};
  std::array<uint32_t, 4> ids{}; std::array<std::thread, 4> contenders;
  HANDLE release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!release) return false;
  for (size_t i = 0; i < contenders.size(); ++i) contenders[i] = std::thread([&, i] {
    ::WaitForSingleObject(release, INFINITE); ids[i] = router::AllocateId(next);
  });
  ::SetEvent(release); for (auto &thread : contenders) thread.join(); ::CloseHandle(release);
  std::sort(ids.begin(), ids.end());
  if (ids != std::array<uint32_t, 4>{0, 0, UINT32_MAX - 2, UINT32_MAX - 1} || next != UINT32_MAX || router::AllocateId(next)) {
    *detail = "allocator wrapped or reused exhausted id"; return false;
  }
  for (int field = 0; field < 3; ++field) {
    router::Connection conn; OpenLocal(conn); router::Router service(BrokerMode::kDedicated);
    auto h = ControlHeader(field == 0 ? UINT32_MAX : 1, field == 1 ? UINT32_MAX : 1);
    auto frame = protocol::BuildCreateSessionFrame(h, LogicalConfig());
    if (field == 2) { h = ControlHeader(1, 1, UINT32_MAX); frame = protocol::BuildCancelRunFrame(h); }
    if (LocalRoute(service, conn, frame) || ::WaitForSingleObject(conn.stop, 0) != WAIT_OBJECT_0) return false;
    service.Close(conn); if (!conn.CompleteZero()) return false;
  }
  return true;
}
bool FillInbound(RouterFixture &f, uint32_t session, size_t bytes) {
  while (bytes) {
    const size_t part = (std::min)(bytes, size_t(protocol::kMaxFramePayload));
    if (part < 16) return false;
    auto h = ControlHeader(0, session, 1); h.type = protocol::MessageType::RELAY_TO_WORKER;
    std::vector<uint8_t> payload(part - 16, 7);
    if (!LocalRoute(*f.service, f.conn, protocol::BuildRelayFrame(h, sbox_msg_kind_binary, payload.data(), payload.size())))
      return false;
    bytes -= part;
  }
  return true;
}
bool FillOutbound(RouterFixture &f, router::Worker *worker, size_t bytes) {
  while (bytes) {
    const size_t part = (std::min)(bytes, size_t(protocol::kMaxFramePayload + 20));
    if (part < 36) return false;
    auto encoded = v8host::EncodeRunEnvelope(GuestRelay(part - 36));
    router::Worker::OnMessage(worker, sbox_msg_kind_binary, encoded.data(), encoded.size());
    f.service->Pump(f.conn);
    bytes -= part;
  }
  return true;
}
bool RelayRunBoundary(std::string *detail) {
  {
    RouterFixture f; f.early = true;
    if (!f.Create() || !f.Start(1) || !f.Start(2)) return false;
    DrainLocal(f.conn);
    for (uint32_t id : {1u, 2u}) {
      auto credit = f.conn.ChargeOutput(protocol::kMaxQueuedRelayBytesPerRun + 1,
          f.conn.sessions.at(1)->runs.at(id)->outbound);
      if (credit.conn) return false;
    }
    f.service->test_post = [](const std::vector<uint8_t> &) { return router::PostOutcome::kAmbiguous; };
    f.service->Pump(f.conn);
    if (!f.conn.sessions.empty()) return false;
    f.service->Close(f.conn); f.conn.ClearOutput();
    if (!f.conn.CompleteZero() || f.closes != 1 || f.waits != 1) {
      *detail = "simultaneous run overflows followed removed session after ambiguous CANCEL"; return false;
    }
  }
  for (bool outbound : {false, true}) for (int delta : {-1, 0, 1}) {
    RouterFixture f; f.early = outbound;
    if (!f.Create() || !f.Start(1) || !f.Start(2)) return false;
    DrainLocal(f.conn);
    auto ledger = f.conn.sessions.at(1)->runs.at(1)->outbound;
    const size_t target = protocol::kMaxQueuedRelayBytesPerRun + delta;
    if (!(outbound ? FillOutbound(f, f.worker, target) : FillInbound(f, 1, target))) return false;
    const bool live = f.conn.sessions.at(1)->runs.contains(1);
    const size_t charged = outbound ? f.conn.out_relay_bytes.load() : f.conn.in_relay_bytes;
    if (live != (delta <= 0) || !f.conn.sessions.at(1)->runs.contains(2) ||
        ::WaitForSingleObject(f.conn.stop, 0) == WAIT_OBJECT_0 || charged > protocol::kMaxQueuedRelayBytesPerRun ||
        (delta <= 0 && charged != target)) {
      *detail = "run quota direction/boundary/containment"; return false;
    }
    if (delta > 0) {
      bool quota = false;
      protocol::FrameHeader h; std::vector<uint8_t> frame;
      while (f.conn.TakeOutput(&frame)) {
        f.conn.CompleteOutput(frame.size());
        const uint8_t *body; size_t size;
        if (protocol::DecodeAndValidateFrame(frame.data(), frame.size(), &h, &body, &size) != protocol::DecodeStatus::kOk)
          return false;
        if (h.type == protocol::MessageType::RUN_ERROR) {
          protocol::ErrorPayload error;
          quota = protocol::DecodeErrorPayload(body, size, &error) && h.request_id == 0 && h.run_id == 1 &&
              error.status_code == protocol::StatusCode::ERROR_QUOTA;
        }
      }
      if (!quota || (outbound && f.worker->session->engine_busy_run_id != 1)) return false;
    }
    f.service->Close(f.conn); f.conn.ClearOutput();
    if (ledger->bytes || !f.conn.CompleteZero()) return false;
  }
  return true;
}
bool RelayConnectionBoundary(std::string *detail) {
  for (bool outbound : {false, true}) for (int delta : {-1, 0, 1}) {
    RouterFixture f; f.early = outbound;
    std::vector<router::Worker *> workers;
    uint32_t request = 0;
    for (uint32_t id = 1; id <= 17; ++id) {
      if (!LocalRoute(*f.service, f.conn, protocol::BuildCreateSessionFrame(ControlHeader(++request, id), LogicalConfig())))
        return false;
      protocol::StartRunPayload start; start.tier_override = -1; start.guest_payload = {'x'};
      if (!LocalRoute(*f.service, f.conn, protocol::BuildStartRunFrame(ControlHeader(++request, id, 1), start))) return false;
      workers.push_back(f.worker);
    }
    DrainLocal(f.conn);
    for (uint32_t id = 1; id <= 16; ++id) {
      const size_t bytes = protocol::kMaxQueuedRelayBytesPerRun - (id == 16 ? 100 : 0);
      if (!(outbound ? FillOutbound(f, workers[id - 1], bytes) : FillInbound(f, id, bytes))) return false;
    }
    const bool accepted = outbound ? FillOutbound(f, workers[16], 100 + delta) : FillInbound(f, 17, 100 + delta);
    const size_t charged = outbound ? f.conn.out_relay_bytes.load() : f.conn.in_relay_bytes;
    if (!outbound && delta > 0) {
      std::vector<uint8_t> frame; protocol::FrameHeader h;
      const uint8_t *body; size_t size; protocol::ErrorPayload error;
      if (!f.conn.TakeOutput(&frame)) { *detail = "aggregate rejection missing ERROR_QUOTA"; return false; }
      f.conn.CompleteOutput(frame.size());
      if (protocol::DecodeAndValidateFrame(frame.data(), frame.size(), &h, &body, &size) != protocol::DecodeStatus::kOk ||
          h.type != protocol::MessageType::ERROR || h.conn_id != f.conn.conn_id ||
          h.target_id != 17 || h.run_id != 1 || h.request_id != 0 ||
          !protocol::DecodeErrorPayload(body, size, &error) || error.status_code != protocol::StatusCode::ERROR_QUOTA ||
          f.conn.in_relay_bytes != protocol::kMaxQueuedRelayBytesPerConnection - 100) {
        *detail = "aggregate rejection must enqueue addressed ERROR_QUOTA without input credit"; return false;
      }
    }
    if ((!outbound && accepted != (delta <= 0)) ||
        (::WaitForSingleObject(f.conn.stop, 0) == WAIT_OBJECT_0) != (outbound && delta > 0) ||
        (!outbound && (f.conn.state == router::ConnState::kClosing) != (delta > 0)) ||
        charged > protocol::kMaxQueuedRelayBytesPerConnection ||
        (delta <= 0 && charged != protocol::kMaxQueuedRelayBytesPerConnection + delta)) {
      *detail = "connection quota boundary did not contain entire connection"; return false;
    }
    f.service->Close(f.conn); f.conn.ClearOutput();
    if (!f.conn.CompleteZero() || f.closes != 17 || f.waits != 17 || f.post_after_close || f.close_before_removal)
      return false;
  }
  return true;
}
bool InboundErrorDrain(std::string *detail) {
  for (int path = 0; path < 4; ++path) {
    RouterFixture f; PressurePipe pipe;
    uint32_t request = 0;
    for (uint32_t id = 1; id <= 17; ++id) {
      if (!LocalRoute(*f.service, f.conn, protocol::BuildCreateSessionFrame(ControlHeader(++request, id), LogicalConfig()))) return false;
      protocol::StartRunPayload start; start.tier_override = -1; start.guest_payload = {'x'};
      if (!LocalRoute(*f.service, f.conn, protocol::BuildStartRunFrame(ControlHeader(++request, id, 1), start))) return false;
    }
    DrainLocal(f.conn);
    for (uint32_t id = 1; id <= 16; ++id)
      if (!FillInbound(f, id, protocol::kMaxQueuedRelayBytesPerRun - (id == 16 ? 100 : 0))) return false;
    if (!pipe.Open(true)) return false;
    HANDLE release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE pending = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!release || !pending) return false;
    f.conn.pipe = pipe.server; f.conn.writer_release = release; f.conn.write_pending = pending;
    bool passed = f.conn.StartWriter();
    // Hold the sole writer until the rejection and preceding output are queued.
    if (path == 1) passed &= f.conn.Enqueue(std::vector<uint8_t>(protocol::kMaxFrameSize, 0));
    else if (path == 3) {
      for (size_t n = 0; n < protocol::kMaxControlQueueRequests; ++n)
        passed &= f.conn.Enqueue(protocol::BuildAckFrame(ControlHeader(1)));
    } else passed &= f.conn.Enqueue(protocol::BuildAckFrame(ControlHeader(1)));
    passed &= !FillInbound(f, 17, 101) && f.conn.in_relay_bytes == protocol::kMaxQueuedRelayBytesPerConnection - 100;
    passed &= path == 3 ? ::WaitForSingleObject(f.conn.stop, 0) == WAIT_OBJECT_0 :
        f.conn.state == router::ConnState::kClosing && ::WaitForSingleObject(f.conn.stop, 0) == WAIT_TIMEOUT;
    if (path == 2) { ::CloseHandle(pipe.client); pipe.client = INVALID_HANDLE_VALUE; }
    ::SetEvent(release);
    if (path == 0) {
      std::array<uint8_t, protocol::kMaxFrameSize> frame;
      for (int n = 0; n < 2; ++n) {
        DWORD count = 0; protocol::FrameHeader h; const uint8_t *body; size_t size; protocol::ErrorPayload error;
        passed &= PressureTransfer(pipe.client, false, frame.data(), static_cast<DWORD>(frame.size()), &count) &&
            protocol::DecodeAndValidateFrame(frame.data(), count, &h, &body, &size) == protocol::DecodeStatus::kOk;
        if (n == 0) passed &= h.type == protocol::MessageType::ACK;
        else passed &= h.type == protocol::MessageType::ERROR && h.conn_id == 1 && h.target_id == 17 &&
            h.run_id == 1 && h.request_id == 0 && protocol::DecodeErrorPayload(body, size, &error) &&
            error.status_code == protocol::StatusCode::ERROR_QUOTA;
      }
      passed &= f.conn.DrainOutput(2000);
    } else if (path == 1) {
      passed &= ::WaitForSingleObject(pending, 5000) == WAIT_OBJECT_0 && !f.conn.DrainOutput(30);
    } else if (path == 2) passed &= ::WaitForSingleObject(f.conn.stop, 5000) == WAIT_OBJECT_0;
    const ULONGLONG begin = ::GetTickCount64();
    f.conn.Stop(); f.conn.JoinWriter(); f.service->Close(f.conn); f.conn.ClearOutput();
    passed &= ::GetTickCount64() - begin < 5000;
    ::CloseHandle(pipe.server); pipe.server = INVALID_HANDLE_VALUE; f.conn.pipe = INVALID_HANDLE_VALUE;
    if (path == 0) {
      uint8_t byte; DWORD count;
      passed &= !PressureTransfer(pipe.client, false, &byte, 1, &count);
      passed &= !::PeekNamedPipe(pipe.client, nullptr, 0, nullptr, nullptr, nullptr) &&
          ::GetLastError() == ERROR_BROKEN_PIPE;
    }
    f.conn.writer_release = f.conn.write_pending = nullptr;
    ::CloseHandle(release); ::CloseHandle(pending);
    if (!passed || !f.conn.CompleteZero() || f.closes != 17 || f.waits != 17 || !f.posts.empty() || f.spawns != 17 ||
        f.conn.active_io || f.conn.writers_started != 1 || f.conn.writers_joined != 1 ||
        f.post_after_close || f.close_before_removal) {
      *detail = "aggregate error drain/failed enqueue/broken or nonreading peer path=" + std::to_string(path); return false;
    }
  }
  return true;
}
bool ControlBoundary(std::string *detail) {
  for (bool bytes : {false, true}) for (int delta : {-1, 0, 1}) {
    router::Connection conn;
    const size_t target = (bytes ? protocol::kMaxControlQueueBytes : protocol::kMaxControlQueueRequests) + delta;
    size_t added = 0; bool accepted = true;
    while (added < target) {
      const size_t part = bytes ? (std::min)(size_t(protocol::kMaxFrameSize), target - added) : 1;
      accepted = conn.Enqueue(std::vector<uint8_t>(bytes ? part : protocol::kFrameHeaderSize));
      if (!accepted) break;
      added += part;
    }
    if (accepted != (delta <= 0) || (delta <= 0 && added != target) ||
        (::WaitForSingleObject(conn.stop, 0) == WAIT_OBJECT_0) != (delta > 0)) {
      *detail = "control byte/request boundary"; return false;
    }
    conn.ClearOutput();
    if (!conn.CompleteZero()) return false;
  }
  return true;
}
bool CreditAllPaths(std::string *detail) {
  for (int path = 0; path < 5; ++path) {
    RouterFixture f;
    if (!f.Create() || !f.Start(1) || !f.Relay(1, 10)) return false;
    auto ledger = f.conn.sessions.at(1)->runs.at(1)->outbound;
    if (f.conn.in_relay_bytes != 26) return false;
    if (path == 0) { f.Phase(1); f.Phase(2); }
    else if (path == 1) {
      if (!LocalRoute(*f.service, f.conn, protocol::BuildCancelRunFrame(ControlHeader(9, 1, 1)))) return false;
    } else if (path == 2) {
      f.conn.sessions.at(1)->runs.at(1)->TryTerminal(router::RunState::kFailed);
    } else if (path == 3) {
      if (!LocalRoute(*f.service, f.conn, protocol::BuildCloseSessionFrame(ControlHeader(9)))) return false;
    } else f.service->Close(f.conn);
    if (f.conn.in_relay_bytes) { *detail = "inbound credit leaked on path " + std::to_string(path); return false; }
    DrainLocal(f.conn);
    if (path == 4) continue;
    auto credit = f.conn.ChargeOutput(100, ledger);
    if (!credit.conn || !f.conn.Enqueue(std::vector<uint8_t>(100), std::move(credit)) ||
        ledger->bytes != 100 || f.conn.out_relay_bytes != 100 || f.conn.out_control_requests) return false;
    std::vector<uint8_t> frame;
    if (!f.conn.TakeOutput(&frame) || ledger->bytes != 100) return false;
    if (path % 2) f.conn.ClearOutput(); else f.conn.CompleteOutput(frame.size());
    f.conn.CompleteOutput(frame.size());
    if (ledger->bytes || f.conn.out_relay_bytes || f.conn.out_control_bytes || f.conn.out_control_requests)
      return false;
  }
  for (int path = 0; path < 5; ++path) {
    RouterFixture f; f.early = true;
    if (!f.Create() || !f.Start(1) || !f.Start(2)) return false;
    DrainLocal(f.conn);
    auto ledger = f.conn.sessions.at(1)->runs.at(1)->outbound;
    const uint32_t id = path == 1 ? 2 : 1;
    auto other = f.conn.sessions.at(1)->runs.at(id)->outbound;
    auto encoded = v8host::EncodeRunEnvelope(GuestRelay(10, id));
    router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, encoded.data(), encoded.size());
    if (other->bytes != 46 || f.conn.out_relay_bytes != 46) return false;
    if (path <= 2) {
      if (path == 2) f.Terminal(1); else f.service->Pump(f.conn);
      if ((path == 1 && (other->bytes || f.conn.out_relay_bytes)) || (path != 1 && ledger->bytes != 46)) return false;
      DrainLocal(f.conn);
    } else if (path == 3) {
      if (!LocalRoute(*f.service, f.conn, protocol::BuildCloseSessionFrame(ControlHeader(9)))) return false;
    } else f.service->Close(f.conn);
    f.service->Close(f.conn); f.conn.ClearOutput();
    if (other->bytes || ledger->bytes || !f.conn.CompleteZero()) {
      *detail = "mailbox transfer/drop/terminal/close/disconnect credit leaked"; return false;
    }
  }
  return true;
}
bool RetryFifoCredits(std::string *detail) {
  for (int terminal = 0; terminal < 5; ++terminal) {
    RouterFixture f; f.early = true;
    auto now = std::chrono::steady_clock::time_point{} + std::chrono::seconds(1);
    f.service->test_now = [&] { return now; };
    if (!f.Create() || !f.Start(1)) return false;
    DrainLocal(f.conn);
    unsigned attempts = 0; bool writable = false;
    f.service->test_post = [&](const std::vector<uint8_t> &bytes) {
      ++attempts;
      if (!writable) return router::PostOutcome::kNotWritten;
      v8host::RunEnvelope env;
      if (!v8host::DecodeRunEnvelope(bytes.data(), bytes.size(), &env)) return router::PostOutcome::kAmbiguous;
      f.posts.push_back(std::move(env)); return router::PostOutcome::kWritten;
    };
    if (!f.Relay(1, 1, 7) || !f.Relay(1, 2, 8) || attempts != 1 || f.conn.in_relay_bytes != 35) return false;
    if (terminal == 0) {
      now += std::chrono::milliseconds(9); writable = true; f.service->Pump(f.conn);
      if (attempts != 1 || f.conn.in_relay_bytes != 35) return false;
      now += std::chrono::milliseconds(1); f.service->Pump(f.conn); f.service->Pump(f.conn);
      if (attempts != 3 || f.posts.size() != 3 || f.posts[1].payload != std::vector<uint8_t>{7} ||
          f.posts[2].payload != std::vector<uint8_t>({8, 8})) return false;
    } else if (terminal == 1) f.Terminal(1);
    else if (terminal == 2) {
      if (!LocalRoute(*f.service, f.conn, protocol::BuildCancelRunFrame(ControlHeader(9, 1, 1))) ||
          attempts != 2 || f.worker->session->pending_cancel_id != 1 || f.conn.in_relay_bytes) return false;
      now += std::chrono::milliseconds(10); writable = true; f.service->Pump(f.conn); f.service->Pump(f.conn);
      if (attempts != 3 || f.posts.size() != 2 || f.posts.back().type != v8host::RunEnvelopeType::kCancel) return false;
      f.Terminal(1);
    } else if (terminal == 3) {
      if (!LocalRoute(*f.service, f.conn, protocol::BuildCloseSessionFrame(ControlHeader(9)))) return false;
    } else f.service->Close(f.conn);
    const unsigned before = attempts;
    now += std::chrono::seconds(20); f.service->Pump(f.conn);
    if (attempts != before || f.conn.in_relay_bytes || f.service->WakeDelay(f.conn, INFINITE) != INFINITE) {
      *detail = "retry head credit/FIFO/terminal cancellation"; return false;
    }
    f.service->Close(f.conn); f.conn.ClearOutput();
    if (!f.conn.CompleteZero() || f.post_after_close || f.closes != 1 || f.waits != 1) return false;
  }
  return true;
}
bool RetryExhaustion(std::string *detail) {
  for (int mode = 0; mode < 5; ++mode) {
    RouterFixture f;
    auto now = std::chrono::steady_clock::time_point{} + std::chrono::seconds(1);
    f.service->test_now = [&] { return now; };
    unsigned attempts = 0;
    f.service->test_post = [&](const std::vector<uint8_t> &bytes) {
      ++attempts;
      if (mode == 1 && attempts == 4) {
        v8host::RunEnvelope env;
        if (!v8host::DecodeRunEnvelope(bytes.data(), bytes.size(), &env)) return router::PostOutcome::kAmbiguous;
        f.posts.push_back(std::move(env));
        return router::PostOutcome::kWritten;
      }
      return mode == 2 ? router::PostOutcome::kAmbiguous : router::PostOutcome::kNotWritten;
    };
    if (!f.Create() || !f.Start(1)) return false;
    f.Phase(1); f.Phase(2);
    if (attempts != 1 || (mode != 2 && f.worker->session->engine_busy_run_id)) return false;
    if (mode == 3) {
      if (!LocalRoute(*f.service, f.conn, protocol::BuildCancelRunFrame(ControlHeader(9, 1, 1)))) return false;
    } else if (mode == 4) f.service->Close(f.conn);
    if (mode >= 2) {
      now += std::chrono::seconds(20); f.service->Pump(f.conn);
      if (attempts != 1) return false;
    } else {
      for (unsigned retry = 0; retry < (mode == 1 ? 3u : 16u); ++retry) {
        const unsigned delay = (std::min)(100u, 10u << (std::min)(retry, 4u));
        if (f.service->WakeDelay(f.conn, INFINITE) != delay) return false;
        now += std::chrono::milliseconds(delay - 1); f.service->Pump(f.conn);
        if (attempts != retry + 1) { *detail = "early retry"; return false; }
        now += std::chrono::milliseconds(1); f.service->Pump(f.conn);
        if (attempts != retry + 2) return false;
      }
      now += std::chrono::seconds(1); f.service->Pump(f.conn);
      if (mode == 1 && (f.posts.size() != 1 || f.worker->session->engine_busy_run_id != 1 || attempts != 4)) return false;
    }
    if (mode == 0 || mode == 2) {
      if (!f.conn.sessions.empty()) return false;
      f.service->Close(f.conn);
      if (f.closes != 1 || f.waits != 1 || f.post_after_close || f.close_before_removal) return false;
    }
    if (f.service->WakeDelay(f.conn, INFINITE) != INFINITE) return false;
  }
  return true;
}
bool DefinitePostFailure(std::string *detail) { return RetryExhaustion(detail) && RetryFifoCredits(detail); }
bool SpawnFailure(std::string *detail) {
  for (int mode = 0; mode < 3; ++mode) {
    const bool missing_image = mode == 0;
    RouterFixture f;
    f.fail_spawn = true;
    f.early = mode == 2;
    for (uint32_t id = 1; id <= 80; ++id) {
      if (!LocalRoute(*f.service, f.conn,
          protocol::BuildCreateSessionFrame(ControlHeader(id * 2, id), LogicalConfig()))) return false;
      DrainLocal(f.conn);
      protocol::StartRunPayload start;
      start.tier_override = -1;
      start.guest_payload = {'x'};
      if (missing_image) {
        start.has_engine_override = true;
        start.engine_filename = "missing-worker-engine.dll";
      }
      if (!LocalRoute(*f.service, f.conn,
          protocol::BuildStartRunFrame(ControlHeader(id * 2 + 1, id, 1), start))) return false;
      protocol::FrameHeader h;
      protocol::StatusCode code;
      if (!PopFrame(f.conn, &h) || h.type != protocol::MessageType::ACK || !PopFrame(f.conn, &h, &code) ||
          h.type != protocol::MessageType::RUN_ERROR || code != protocol::StatusCode::ERROR_INTERNAL ||
          !f.conn.sessions.empty() || !f.conn.retired_sessions.empty() || f.closes || f.waits || !f.posts.empty() ||
          !PopWorkerExit(f.conn, id) || ::WaitForSingleObject(f.conn.stop, 0) == WAIT_OBJECT_0 ||
          f.spawns != (missing_image ? 0 : id) || (mode == 2 && !f.before_publication)) {
        *detail = "failed spawn retained storage or exhausted connection at " + std::to_string(id);
        return false;
      }
    }
  }
  return true;
}
bool CloseDuringPublication(std::string *detail) {
  for (bool stop_first : {false, true}) {
    RouterFixture f;
    f.early = true;
    f.stop_during_spawn = stop_first;
    f.wait_entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!f.wait_entered || !f.wait_release || !f.Create() || !f.Start(1) || !f.before_publication)
      return false;
    if (!stop_first && !LocalRoute(*f.service, f.conn, protocol::BuildCloseSessionFrame(ControlHeader(9))))
      return false;
    if (::WaitForSingleObject(f.wait_entered, 3000) != WAIT_OBJECT_0 || !f.conn.sessions.empty() || f.closes != 1 ||
        (stop_first && !f.posts.empty()))
      return false;
    f.service->Pump(f.conn);
    ::SetEvent(f.wait_release);
    f.service->Close(f.conn);
    if (f.waits != 1 || f.post_after_close || f.close_before_removal || !f.conn.retired_sessions.empty()) {
      *detail = "close/publication ownership ordering";
      return false;
    }
  }
  return true;
}
bool MailboxReadout(std::string *detail) {
  RouterFixture f;
  if (!f.Create() || !f.Start(1))
    return false;
  f.Phase(1); f.Phase(2);
  DrainLocal(f.conn);
  HANDLE entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE delivered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!entered || !delivered) {
    if (entered) ::CloseHandle(entered);
    if (delivered) ::CloseHandle(delivered);
    return false;
  }
  // Put the counters at wrap to cover unsigned occupancy and slot reuse too.
  f.worker->mailbox_head = SIZE_MAX;
  f.worker->mailbox_tail = SIZE_MAX;
  auto emit = [&](uint8_t byte) {
    auto env = GuestRelay(1);
    env.payload[0] = byte;
    auto bytes = v8host::EncodeRunEnvelope(env);
    router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
  };
  emit(7);
  bool barrier_ok = false, first = true;
  f.worker->mailbox_readout = [&] {
    if (!first) return;
    first = false;
    ::SetEvent(entered);
    barrier_ok = ::WaitForSingleObject(delivered, 3000) == WAIT_OBJECT_0;
  };
  std::thread producer([&] {
    if (::WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0)
      emit(8);
    ::SetEvent(delivered);
  });
  f.service->Pump(f.conn);
  producer.join();
  ::CloseHandle(entered); ::CloseHandle(delivered);
  if (!barrier_ok || f.conn.sessions.empty() || f.closes) {
    *detail = "callback during read-out closed healthy session";
    return false;
  }
  f.worker->mailbox_readout = {};
  for (uint8_t expected : {7, 8}) {
    std::vector<uint8_t> bytes;
    protocol::FrameHeader h;
    const uint8_t *payload, *body;
    size_t size, body_size;
    int32_t kind;
    if (!f.conn.TakeOutput(&bytes)) return false;
    f.conn.CompleteOutput(bytes.size());
    if (protocol::DecodeAndValidateFrame(bytes.data(), bytes.size(), &h, &payload, &size) != protocol::DecodeStatus::kOk ||
        h.type != protocol::MessageType::RELAY_FROM_WORKER || h.run_id != 1 ||
        !protocol::DecodeRelayPayload(payload, size, &kind, &body, &body_size) ||
        kind != sbox_msg_kind_binary || body_size != 1 || body[0] != expected) {
      *detail = "callback read-out FIFO lost or duplicated";
      return false;
    }
  }
  return f.conn.out_queue.empty() && !f.worker->mailbox_failed &&
      f.worker->mailbox_head == f.worker->mailbox_tail;
}
bool MailboxFailureSticky(std::string *detail) {
  for (bool oversize : {false, true}) {
    RouterFixture f;
    f.wait_entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!f.wait_entered || !f.wait_release || !f.Create() || !f.Start(1))
      return false;
    f.Phase(1); f.Phase(2);
    if (!f.Start(2) || !f.Relay(2, 1, 9)) return false;
    DrainLocal(f.conn);
    auto *worker = f.worker;
    auto *run = worker->session->runs.at(1).get();
    auto emit = [&](uint8_t byte) {
      auto env = GuestRelay(1);
      env.payload[0] = byte;
      auto bytes = v8host::EncodeRunEnvelope(env);
      router::Worker::OnMessage(worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
    };
    auto later = [&] {
      emit(17);
      v8host::RunEnvelope env;
      env.type = v8host::RunEnvelopeType::kResult;
      env.run_id = 1;
      auto bytes = v8host::EncodeRunEnvelope(env);
      router::Worker::OnMessage(worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
    };
    for (size_t i = 0; i < (oversize ? 1 : worker->mailbox.size()); ++i) emit(static_cast<uint8_t>(i));
    const size_t published = worker->mailbox_tail.load();
    unsigned readouts = 0;
    worker->mailbox_readout = [&] {
      if (++readouts == 1) {
        if (oversize) {
          const uint8_t byte = 0;
          router::Worker::OnMessage(worker, sbox_msg_kind_binary, &byte, protocol::kMaxFramePayload + 1);
          later();
        } else {
          emit(16);  // The copied slot has not been released: this must drop.
        }
      } else if (readouts == 2) {
        later();  // A slot is free; a non-sticky producer would publish again.
      }
    };
    f.service->Pump(f.conn);
    const bool closed_in_pump = f.conn.sessions.empty() && f.worker->close_called;
    if (!closed_in_pump || ::WaitForSingleObject(f.wait_entered, 3000) != WAIT_OBJECT_0) {
      *detail = "mailbox drop did not fail closed in the same Pump";
      return false;
    }
    // Hold the join boundary so callbacks can arrive after a slot becomes free.
    const size_t tail = worker->mailbox_tail.load();
    later();
    const bool rejected = worker->mailbox_failed && worker->mailbox_tail == published && tail == published &&
        worker->mailbox_tail - worker->mailbox_head < worker->mailbox.size() && readouts == 1 &&
        run->state == router::RunState::kWorkerExited && worker->session->runs.empty() &&
        PopWorkerExit(f.conn) && f.posts.size() == 1;
    worker->mailbox_readout = {};
    ::SetEvent(f.wait_release);
    f.service->Close(f.conn);
    if (!rejected || f.closes != 1 || f.waits != 1 || f.close_before_removal || f.post_after_close ||
        !f.conn.retired_sessions.empty()) {
      *detail = "post-drop fact/RESULT delivered, successor dispatched, or cleanup ownership lost";
      return false;
    }
  }
  return true;
}
bool MailboxBoundary(std::string *detail) {
  for (int mode = 0; mode < 5; ++mode) {
    if (mode == 2) {
      if (!MailboxReadout(detail)) return false;
      continue;
    }
    RouterFixture f;
    if (!f.Create() || !f.Start(1))
      return false;
    DrainLocal(f.conn);
    const uint8_t phase[] = {1,0,0,0};
    if (mode == 3)
      router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, phase, protocol::kMaxFramePayload + 1);
    else if (mode == 4)
      router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, nullptr, 1);
    else
      for (size_t i = 0; i < (mode == 0 ? f.worker->mailbox.size() : f.worker->mailbox.size() + 1); ++i)
        router::Worker::OnMessage(f.worker, sbox_msg_kind_lifecycle, phase, 4);
    f.service->Pump(f.conn);
    if (mode == 0) {
      if (f.conn.sessions.empty() || !f.worker->startup || f.worker->mailbox_head != f.worker->mailbox_tail ||
          f.worker->mailbox_failed || f.conn.out_queue.size() != 1 || f.closes)
        return false;
    } else {
      const bool rejected = f.conn.sessions.empty();
      f.service->Close(f.conn);
      if (!rejected || !f.conn.sessions.empty() || !f.conn.retired_sessions.empty() || !PopWorkerExit(f.conn) ||
          f.closes != 1 || f.waits != 1 || f.post_after_close) {
        *detail = "mailbox full intake did not close and join";
        return false;
      }
    }
  }
  return true;
}
bool RelayMailboxFairness(std::string *detail) {
  RouterFixture f;
  if (!f.Create() || !f.Start(1)) return false;
  f.Phase(1); f.Phase(2);
  DrainLocal(f.conn);
  for (uint8_t i = 0; i < 64; ++i) {
    if (i) {
      // The engine returns one relay credit for each relay it consumed.
      v8host::RunEnvelope credit;
      credit.type = v8host::RunEnvelopeType::kCredit;
      credit.credit = 1;
      const auto encoded = v8host::EncodeRunEnvelope(credit);
      router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, encoded.data(), encoded.size());
    }
    auto bytes = v8host::EncodeRunEnvelope(GuestRelay(1));
    bytes.back() = i;
    router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
    if (!f.Relay(1, 1, i)) return false;
    std::vector<uint8_t> frame;
    if (!f.conn.TakeOutput(&frame)) {
      *detail = "back-to-back relays starved worker mailbox at " + std::to_string(i);
      return false;
    }
    f.conn.CompleteOutput(frame.size());
    protocol::FrameHeader h;
    const uint8_t *payload, *body;
    size_t size, body_size;
    int32_t kind;
    if (protocol::DecodeAndValidateFrame(frame.data(), frame.size(), &h, &payload, &size) != protocol::DecodeStatus::kOk ||
        h.type != protocol::MessageType::RELAY_FROM_WORKER || h.run_id != 1 ||
        !protocol::DecodeRelayPayload(payload, size, &kind, &body, &body_size) ||
        kind != sbox_msg_kind_binary || body_size != 1 || body[0] != i || f.conn.sessions.size() != 1 ||
        f.closes || f.worker->mailbox_failed || !f.conn.out_queue.empty()) return false;
  }
  return f.posts.size() == 65 && f.worker->mailbox_head == f.worker->mailbox_tail;
}
bool InlineReadMailbox(std::string *detail) {
  RouterFixture f;
  if (!f.Create() || !f.Start(1)) return false;
  DrainLocal(f.conn);
  struct Pipes {
    HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
    ~Pipes() {
      if (client != INVALID_HANDLE_VALUE) ::CloseHandle(client);
      if (server != INVALID_HANDLE_VALUE) ::CloseHandle(server);
    }
  } pipes;
  const std::wstring name = L"\\\\.\\pipe\\v8host-inline-" + std::to_wstring(::GetCurrentProcessId());
  pipes.server = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1, 4096, 4096, 0, nullptr);
  pipes.client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_OVERLAPPED, nullptr);
  OVERLAPPED connect = {};
  connect.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  const bool connected = pipes.server != INVALID_HANDLE_VALUE && pipes.client != INVALID_HANDLE_VALUE && connect.hEvent &&
      (::ConnectNamedPipe(pipes.server, &connect) || ::GetLastError() == ERROR_PIPE_CONNECTED);
  if (connect.hEvent) ::CloseHandle(connect.hEvent);
  if (!connected) return false;
  f.conn.pipe = pipes.server;
  f.conn.dispatcher = f.service.get();
  std::vector<uint8_t> sent{1}, received(1);
  for (int i = 0; i < 64; ++i) {
    // Completed write guarantees the read has buffered data; no pending-read wake is needed.
    if (!PipeIo(pipes.client, true, &sent)) return false;
    const uint8_t startup[] = {1, 0, 0, 0};
    router::Worker::OnMessage(f.worker, sbox_msg_kind_lifecycle, startup, sizeof(startup));
    DWORD size = 0;
    if (!f.conn.ReadFrame(received.data(), 1, &size, 3000) || size != 1 || received != sent ||
        f.conn.inline_reads != static_cast<uint32_t>(i + 1) || f.conn.sessions.empty() || f.closes ||
        f.worker->mailbox_failed || f.worker->mailbox_head != f.worker->mailbox_tail || !f.worker->startup) {
      *detail = "immediately completed read did not drain worker mailbox";
      return false;
    }
    DrainLocal(f.conn);
  }
  f.conn.pipe = INVALID_HANDLE_VALUE;
  return true;
}
bool HeldRelayBudget(std::string *detail) {
  RouterFixture f;
  if (!f.Create() || !f.Start(1))
    return false;
  DrainLocal(f.conn);
  for (int i = 0; i < 16; ++i)
    if (!f.Relay(1, 65512))
      return false;
  if (!f.Relay(1, 112) || f.conn.sessions.at(1)->runs.at(1)->queued_relay_bytes != protocol::kMaxQueuedRelayBytesPerRun ||
      !f.posts.empty() || !f.Relay(1, 0))
    return false;
  protocol::FrameHeader header;
  protocol::StatusCode code;
  if (!PopFrame(f.conn, &header, &code) || header.type != protocol::MessageType::RUN_ERROR || header.request_id != 0 ||
      code != protocol::StatusCode::ERROR_QUOTA || !f.conn.sessions.at(1)->runs.empty() || !f.posts.empty()) {
    *detail = "held relay budget exact cap/+1";
    return false;
  }
  f.Phase(1); f.Phase(2);
  return f.posts.empty();
}
struct CapturedBroker {
  HANDLE process = nullptr, output = nullptr;
  std::thread reader;
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<HANDLE> workers;
  bool failed = false;
  std::string diagnostics;
  std::string lifecycle_failure;
  unsigned lifecycle_evidence = 0;
  std::string failure_selector;
  HANDLE lifecycle_held = nullptr;
  ~CapturedBroker() {
    if (process) {
      if (::WaitForSingleObject(process, 10000) != WAIT_OBJECT_0) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          std::printf("[failure broker reap] broker=%lu workers=%zu failed=%d\n[failure diagnostics] %s\n",
              ::GetProcessId(process), workers.size(), failed, diagnostics.c_str());
          for (HANDLE worker : workers)
            std::printf("[failure worker wait] worker=%lu wait=%lu\n", ::GetProcessId(worker), ::WaitForSingleObject(worker, 0));
        }
        std::printf("[cleanup] forced owned broker termination\n");
        ::TerminateProcess(process, 1);
        ::WaitForSingleObject(process, 5000);
      }
      ::CloseHandle(process);
    }
    if (reader.joinable())
      reader.join();
    if (output)
      ::CloseHandle(output);
    if (lifecycle_held)
      ::CloseHandle(lifecycle_held);
    for (HANDLE worker : workers)
      ::CloseHandle(worker);
  }
  bool Launch(std::string *detail) {
    *detail = "captured broker identity/capture/lifecycle setup";
    v8host::PayloadIdentity payload;
    std::vector<uint8_t> sid;
    LUID session;
    DWORD error;
    std::wstring endpoint;
    std::array<uint8_t, 32> key;
    if (!v8host::ResolvePayloadIdentity(ExecutableDirectory(), L"sbox.exe", L"v8host.dll", &payload, &error) ||
        !v8host::QueryCurrentSidAndSession(&sid, &session, &error) ||
        !v8host::DeriveEndpoint(sid, payload.plugin_set_id, payload.install_root, BrokerMode::kShared, nullptr, &endpoint, &key))
      return false;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE write = nullptr;
    if (!::CreatePipe(&output, &write, &sa, 0))
      return false;
    ::SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = write;
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    std::wstring exe = ExecutableDirectory() + L"\\sbox.exe";
    std::wstring command = L"\"" + exe + L"\" --broker --mode=shared --pipe=\"" + endpoint + L"\" --plugin v8host.dll";
    PROCESS_INFORMATION pi = {};
    const DWORD previous_size = ::GetEnvironmentVariableA("SBOX_TEST_LIFECYCLE_FAILURE", nullptr, 0);
    std::string previous(previous_size, '\0');
    if (previous_size)
      ::GetEnvironmentVariableA("SBOX_TEST_LIFECYCLE_FAILURE", previous.data(), previous_size);
    const DWORD event_previous_size = ::GetEnvironmentVariableA("SBOX_TEST_LIFECYCLE_HELD_EVENT", nullptr, 0);
    std::string event_previous(event_previous_size, '\0');
    if (event_previous_size)
      ::GetEnvironmentVariableA("SBOX_TEST_LIFECYCLE_HELD_EVENT", event_previous.data(), event_previous_size);
    if (!failure_selector.empty()) {
      const std::string name = "Local\\sbox-lifecycle-held-" + std::to_string(::GetCurrentProcessId()) +
          "-" + failure_selector;
      lifecycle_held = ::CreateEventA(nullptr, TRUE, FALSE, name.c_str());
      if (!lifecycle_held) {
        ::CloseHandle(write);
        return false;
      }
      ::SetEnvironmentVariableA("SBOX_TEST_LIFECYCLE_HELD_EVENT", name.c_str());
      ::SetEnvironmentVariableA("SBOX_TEST_LIFECYCLE_FAILURE", failure_selector.c_str());
    }
    const ULONGLONG launch_started = ::GetTickCount64();
    bool ok = ::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr,
        ExecutableDirectory().c_str(), &startup, &pi) != FALSE;
    diagnostics += "[broker launch] duration_ms=" + std::to_string(::GetTickCount64() - launch_started) + "\n";
    if (!failure_selector.empty()) {
      ::SetEnvironmentVariableA("SBOX_TEST_LIFECYCLE_FAILURE", previous_size ? previous.c_str() : nullptr);
      ::SetEnvironmentVariableA("SBOX_TEST_LIFECYCLE_HELD_EVENT", event_previous_size ? event_previous.c_str() : nullptr);
    }
    ::CloseHandle(write);
    if (!ok) {
      *detail = "captured broker launch failed";
      return false;
    }
    process = pi.hProcess;
    ::CloseHandle(pi.hThread);
    reader = std::thread([this] {
      std::string line;
      bool oversized = false;
      char chunk[1024];
      DWORD size;
      while (::ReadFile(output, chunk, sizeof(chunk), &size, nullptr) && size) {
        for (DWORD i = 0; i < size; ++i) {
          if (chunk[i] == '\n') {
            if (oversized) { line.clear(); oversized = false; continue; }
            while (!line.empty() && line.back() == '\r')
              line.pop_back();
            { std::lock_guard<std::mutex> lock(mutex); if (diagnostics.size() < 65536) diagnostics += line + "\n"; }
            unsigned long pid = 0;
            if (std::sscanf(line.c_str(), "[broker] target created: pid=%lu", &pid) == 1) {
              HANDLE held = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
              std::lock_guard<std::mutex> lock(mutex);
              if (!held || workers.size() >= 32 || std::any_of(workers.begin(), workers.end(),
                  [pid](HANDLE h) { return ::GetProcessId(h) == pid && ::WaitForSingleObject(h, 0) == WAIT_TIMEOUT; })) {
                failed = true;
                if (held)
                  ::CloseHandle(held);
              } else {
                workers.push_back(held);
                changed.notify_all();
                if (lifecycle_held)
                  ::SetEvent(lifecycle_held);
                std::printf("[captured] target created: pid=%lu\n", pid);
                std::fflush(stdout);
              }
            }
            if (line.rfind("[sbox-test] failure=", 0) == 0) {
              std::lock_guard<std::mutex> lock(mutex);
              lifecycle_failure = line;
              ++lifecycle_evidence;
            }
            line.clear();
          } else if (!oversized && line.size() < 4096) {
            line += chunk[i];
          } else {
            std::lock_guard<std::mutex> lock(mutex);
            failed = true;
            oversized = true;
            changed.notify_all();
          }
        }
      }
      std::lock_guard<std::mutex> lock(mutex);
      if (!line.empty() || oversized) failed = true;
      changed.notify_all();
    });
    const ULONGLONG deadline = ::GetTickCount64() + 5000;
    while (::GetTickCount64() < deadline) {
      if (::WaitNamedPipeW(endpoint.c_str(), 100))
        return true;
      if (::WaitForSingleObject(process, 10) == WAIT_OBJECT_0)
        break;
    }
    *detail = "captured endpoint did not become ready";
    return false;
  }
  bool Observe(size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(10), [&] { return failed || workers.size() >= count; }) &&
        !failed && workers.size() == count;
  }
  HANDLE Worker(size_t index) {
    std::lock_guard<std::mutex> lock(mutex);
    return !failed && index < workers.size() ? workers[index] : nullptr;
  }
  bool WorkersExited(size_t count) {
    if (!Observe(count)) return false;
    for (size_t i = 0; i < count; ++i) {
      HANDLE h = Worker(i);
      if (!h || ::WaitForSingleObject(h, 10000) != WAIT_OBJECT_0) return false;
      std::printf("[reaped] broker=%lu worker=%lu\n", ::GetProcessId(process), ::GetProcessId(h));
    }
    return true;
  }
  bool Reaped(size_t count) {
    if (::WaitForSingleObject(process, 10000) != WAIT_OBJECT_0)
      return false;
    reader.join();
    std::printf("[reaped] broker=%lu\n", ::GetProcessId(process));
    if (failed || workers.size() != count) return false;
    for (HANDLE h : workers) {
      if (::WaitForSingleObject(h, 0) != WAIT_OBJECT_0) return false;
      std::printf("[reaped] broker=%lu worker=%lu\n", ::GetProcessId(process), ::GetProcessId(h));
    }
    return true;
  }
  bool ReapedAfterKill(size_t count) {
    if (::WaitForSingleObject(process, 10000) != WAIT_OBJECT_0) return false;
    reader.join();
    std::printf("[reaped] killed broker=%lu\n", ::GetProcessId(process));
    return !failed && WorkersExited(count);
  }
};
bool OpenCaptured(CapturedBroker &broker, WireHarness &h, std::string *detail) {
  if (!broker.Launch(detail) ||
      !ConnectAndHandshake(BrokerMode::kShared, 900, &h.connection, &h.hello, detail) ||
      h.connection.broker_pid() != ::GetProcessId(broker.process))
    return false;
  auto config = LogicalConfig();
  config.broker_mode = 1;
  config.initial_token = sbox_token_level_restricted_same_access;
  config.delayed_integrity = sbox_integrity_level_untrusted;
  return h.Send(protocol::BuildCreateSessionFrame(h.Header(1), config)) &&
      h.Expect(protocol::MessageType::ACK, 1, 1) && h.Expect(protocol::MessageType::SESSION_READY, 0, 1);
}
protocol::StartRunPayload Script(const std::string &guest) {
  protocol::StartRunPayload start;
  start.tier_override = -1;
  start.guest_payload.assign(guest.begin(), guest.end());
  return start;
}
bool SendString(WireHarness &h, uint32_t id, const std::string &message) {
  auto header = h.Header(0, 1, id);
  header.type = protocol::MessageType::RELAY_TO_WORKER;
  return h.Send(protocol::BuildRelayFrame(header, sbox_msg_kind_string,
      reinterpret_cast<const uint8_t *>(message.data()), message.size()));
}
bool ExpectString(WireHarness &h, uint32_t id, const std::string &message) {
  std::vector<uint8_t> bytes;
  protocol::FrameHeader header;
  int32_t kind;
  const uint8_t *body;
  size_t size;
  return h.Receive(&bytes, &header) && header.type == protocol::MessageType::RELAY_FROM_WORKER && header.run_id == id &&
      protocol::DecodeRelayPayload(bytes.data() + protocol::kFrameHeaderSize,
          bytes.size() - protocol::kFrameHeaderSize, &kind, &body, &size) && kind == sbox_msg_kind_string &&
      std::string(reinterpret_cast<const char *>(body), size) == message;
}
bool ExpectResult(WireHarness &h, uint32_t id, protocol::ResultDisposition disposition) {
  std::vector<uint8_t> bytes;
  protocol::FrameHeader header;
  protocol::ResultPayload result;
  return h.Receive(&bytes, &header) && header.type == protocol::MessageType::RESULT && header.run_id == id &&
      header.request_id == 0 && protocol::DecodeResultPayload(bytes.data() + protocol::kFrameHeaderSize,
          bytes.size() - protocol::kFrameHeaderSize, &result) && result.disposition == disposition;
}
// Explicit topology cases share the session fixture's fake broker and intake.
protocol::FrameHeader TopologyHeader(uint32_t request, uint32_t target = 1, uint32_t run = 0) {
  auto h = ControlHeader(request, target, run);
  h.flags = protocol::kFlagMustUnderstand;
  return h;
}
struct TopologyFixture : RouterFixture {
  uint32_t request = 100;
  bool Reply(const std::vector<uint8_t>& frame, protocol::MessageType type,
             protocol::StatusCode expected = protocol::StatusCode::OK) {
    protocol::FrameHeader h, sent;
    protocol::StatusCode error;
    return protocol::DecodeAndValidateFrame(frame.data(), frame.size(), &sent) == protocol::DecodeStatus::kOk &&
        LocalRoute(*service, conn, frame) &&
        PopFrame(conn, &h, type == protocol::MessageType::ERROR ? &error : nullptr) &&
        h.type == type && h.conn_id == sent.conn_id && h.request_id == sent.request_id &&
        h.target_id == sent.target_id && h.run_id == sent.run_id &&
        (type != protocol::MessageType::ERROR || error == expected);
  }
  bool Worker(uint32_t maximum = 2, uint32_t flags = 0) {
    protocol::CreateWorkerPayload config;
    config.session = LogicalConfig();
    config.max_runtimes = maximum;
    config.flags = flags;
    return Reply(protocol::BuildCreateWorkerFrame(TopologyHeader(++request), config), protocol::MessageType::ACK);
  }
  bool Runtime(uint32_t id, protocol::StatusCode expected = protocol::StatusCode::OK) {
    return Reply(protocol::BuildCreateRuntimeFrame(TopologyHeader(++request, id), {1, 0, 1, -1}),
        expected == protocol::StatusCode::OK ? protocol::MessageType::ACK : protocol::MessageType::ERROR, expected);
  }
  bool StartRuntime(uint32_t id, uint32_t run = 1, protocol::StartRunPayload inputs = Script("host.complete();")) {
    return Reply(protocol::BuildStartRunFrame(ControlHeader(++request, id, run), inputs), protocol::MessageType::ACK);
  }
  bool CloseRuntime(uint32_t id) {
    return Reply(protocol::BuildCloseRuntimeFrame(TopologyHeader(++request, id)), protocol::MessageType::ACK);
  }
  void Inject(v8host::RunEnvelopeType type, uint16_t slot, uint32_t run = 0, uint32_t value = 0) {
    v8host::RunEnvelope env;
    env.type = type; env.slot = slot; env.run_id = run;
    env.credit = value; env.status_code = value;
    auto bytes = v8host::EncodeRunEnvelope(env);
    router::Worker::OnMessage(worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
    service->Pump(conn);
  }
  bool Ready(uint16_t slot, uint32_t id) {
    Inject(v8host::RunEnvelopeType::kRuntimeReady, slot);
    protocol::FrameHeader h;
    return PopFrame(conn, &h) && h.type == protocol::MessageType::RUNTIME_READY &&
        h.target_id == id && h.request_id == 0;
  }
  bool OpenTwo() {
    if (!Worker() || !Runtime(2) || !Runtime(3) || !posts.empty()) return false;
    Phase(1);
    if (!posts.empty()) return false;
    DrainLocal(conn);
    Phase(2);
    DrainLocal(conn);
    return posts.size() == 2 && posts[0].type == v8host::RunEnvelopeType::kCreateRuntime &&
        posts[0].slot == 0 && posts[1].type == v8host::RunEnvelopeType::kCreateRuntime &&
        posts[1].slot == 1 && Ready(0, 2) && Ready(1, 3);
  }
};

bool WorkerCloseAtCap(std::string* detail) {
  TopologyFixture f;
  f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!f.wait_release) return false;
  protocol::CreateWorkerPayload config;
  config.session = LogicalConfig();
  for (uint32_t id = 1; id <= protocol::kMaxSessionsPerConnection; ++id)
    if (!f.Reply(protocol::BuildCreateWorkerFrame(TopologyHeader(++f.request, id), config),
                 protocol::MessageType::ACK)) return false;
  if (!f.Reply(protocol::BuildCreateWorkerFrame(TopologyHeader(++f.request, 33), config),
               protocol::MessageType::ERROR, protocol::StatusCode::ERROR_QUOTA) ||
      !f.Reply(protocol::BuildCloseWorkerFrame(TopologyHeader(++f.request, 1)), protocol::MessageType::ACK) ||
      !f.Reply(protocol::BuildCreateWorkerFrame(TopologyHeader(++f.request, 33), config),
               protocol::MessageType::ACK)) return false;
  for (unsigned i = 0; i < 3; ++i) f.service->Pump(f.conn);
  if (f.spawns != 32 || f.conn.workers.size() != 32 || f.conn.retired_workers.size() != 1 ||
      f.conn.workers.at(33)->state != router::WorkerState::kNone) {
    *detail = "replacement refused or spawned above the hard live-process cap"; return false;
  }
  if (!f.Reply(protocol::BuildCreateWorkerFrame(TopologyHeader(++f.request, 34), config),
               protocol::MessageType::ERROR, protocol::StatusCode::ERROR_QUOTA)) return false;
  ::SetEvent(f.wait_release);
  if (::WaitForSingleObject(f.conn.retired_workers.front()->cleanup, 5000) != WAIT_OBJECT_0) return false;
  f.service->Pump(f.conn);
  if (f.spawns != 33 || !f.conn.retired_workers.empty() ||
      f.conn.workers.at(33)->state != router::WorkerState::kWarming) {
    *detail = "replacement did not resume after retirement"; return false;
  }
  *detail = "immediate close/open admitted; spawn waits for reap; 32-process bound retained";
  return true;
}

bool RawTwoRuntimeJs(std::string* detail) {
  *detail = "explicit worker startup/security/ACG before runtime admission";
  CapturedBroker broker;
  WireHarness h;
  if (!broker.Launch(detail) ||
      !ConnectAndHandshake(BrokerMode::kShared, 900, &h.connection, &h.hello, detail) ||
      h.connection.broker_pid() != ::GetProcessId(broker.process)) return false;
  protocol::CreateWorkerPayload config;
  config.session = LogicalConfig();
  config.session.broker_mode = 1;
  config.session.initial_token = sbox_token_level_restricted_same_access;
  config.session.delayed_integrity = sbox_integrity_level_untrusted;
  config.max_runtimes = 2;
  auto topology_header = [&](uint32_t request, uint32_t target = 1) {
    auto header = h.Header(request, target);
    header.flags = protocol::kFlagMustUnderstand;
    return header;
  };
  if (!h.Send(protocol::BuildCreateWorkerFrame(topology_header(1), config)) ||
      !h.Expect(protocol::MessageType::ACK, 1, 1)) return false;
  std::vector<uint8_t> bytes;
  protocol::FrameHeader header;
  protocol::StartupReadyPayload startup;
  if (!h.Receive(&bytes, &header) || header.type != protocol::MessageType::STARTUP_READY ||
      header.target_id != 1 || header.request_id != 0 ||
      !protocol::DecodeStartupReadyPayload(bytes.data() + protocol::kFrameHeaderSize,
          bytes.size() - protocol::kFrameHeaderSize, &startup) ||
      !h.Expect(protocol::MessageType::SECURITY_READY, 0, 1) || !broker.Observe(1) ||
      startup.process_id != ::GetProcessId(broker.Worker(0))) return false;
  HANDLE query = ::OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, startup.process_id);
  PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy = {};
  const bool acg = query && ::GetProcessMitigationPolicy(query, ProcessDynamicCodePolicy, &policy, sizeof(policy)) &&
      policy.ProhibitDynamicCode == 1;
  const DWORD query_error = acg ? ERROR_SUCCESS : ::GetLastError();
  if (query) ::CloseHandle(query);
  if (!acg) {
    *detail = "dynamic-code policy query failed: " + std::to_string(query_error);
    return false;
  }
  for (uint32_t id : {2u, 3u})
    if (!h.Send(protocol::BuildCreateRuntimeFrame(topology_header(id, id), {1, 0, 1, -1})) ||
        !h.Expect(protocol::MessageType::ACK, id, id) ||
        !h.Expect(protocol::MessageType::RUNTIME_READY, 0, id)) return false;
  *detail = "two isolated globals, real JS, one worker PID";
  const std::array<std::pair<std::string, std::string>, 4> runs = {{
      {"globalThis.owner=42;globalThis.firstOnly=1;host.postMessage('first');host.complete();", "first"},
      {"host.postMessage(typeof firstOnly+':'+typeof owner);globalThis.owner=7;"
       "globalThis.secondOnly=1;host.complete();", "undefined:undefined"},
      {"host.postMessage(owner+':'+typeof secondOnly);host.complete();", "42:undefined"},
      {"host.postMessage(owner+':'+typeof firstOnly);host.complete();", "7:undefined"},
  }};
  for (uint32_t index = 0; index != runs.size(); ++index) {
    const uint32_t id = 2 + index % 2, run = 1 + index / 2, request = 10 + index;
    if (!h.Send(protocol::BuildStartRunFrame(h.Header(request, id, run), Script(runs[index].first))) ||
        !h.Expect(protocol::MessageType::ACK, request, id, run) ||
        !h.Receive(&bytes, &header) || header.type != protocol::MessageType::RELAY_FROM_WORKER ||
        header.target_id != id || header.run_id != run || header.request_id != 0) return false;
    int32_t kind;
    const uint8_t* body;
    size_t size;
    if (!protocol::DecodeRelayPayload(bytes.data() + protocol::kFrameHeaderSize,
            bytes.size() - protocol::kFrameHeaderSize, &kind, &body, &size) ||
        kind != sbox_msg_kind_string ||
        std::string(reinterpret_cast<const char*>(body), size) != runs[index].second ||
        !h.Receive(&bytes, &header) || header.type != protocol::MessageType::RESULT ||
        header.target_id != id || header.run_id != run || header.request_id != 0) return false;
    protocol::ResultPayload result;
    if (!protocol::DecodeResultPayload(bytes.data() + protocol::kFrameHeaderSize,
            bytes.size() - protocol::kFrameHeaderSize, &result) ||
        result.disposition != protocol::ResultDisposition::kCompleted) return false;
  }
  if (!h.Send(protocol::BuildCloseWorkerFrame(topology_header(20))) ||
      !h.Expect(protocol::MessageType::ACK, 20, 1)) return false;
  h.connection.Close();
  return broker.Reaped(1);
}

bool WorkerProfilePolicyReject(std::string* detail) {
  TopologyFixture f;
  // The codec treats an excessive snapshot count as malformed, before value
  // validation; keep that existing fail-closed oracle as well as policy errors.
  protocol::CreateWorkerPayload malformed;
  malformed.session = LogicalConfig();
  malformed.snapshot_paths.assign(9, "snapshot.bin");
  if (LocalRoute(*f.service, f.conn, protocol::BuildCreateWorkerFrame(TopologyHeader(++f.request), malformed)) ||
      f.spawns || !f.conn.out_queue.empty()) return false;
  for (unsigned variant = 0; variant != 9; ++variant) {
    protocol::CreateWorkerPayload config;
    config.session = LogicalConfig();
    auto expected = protocol::StatusCode::ERROR_BAD_STATE;
    switch (variant) {
      case 0: config.session.prohibit_dynamic_code = false; break;
      case 1: config.session.tier = 1; break;  // trusted requires dynamic code
      case 2: config.session.broker_mode = 1; break;
      case 3: config.engine_dll = "..\\v8jsisb.dll"; break;
      case 4: config.snapshot_paths = {"..\\outside.bin"}; break;
      case 5: config.flags = 2; break;
      case 6: config.max_runtimes = 0; break;
      case 7: config.max_runtimes = 17; break;
      case 8:
        config.snapshot_paths.assign(8, std::string(520, 'a') + ".bin");
        expected = protocol::StatusCode::ERROR_QUOTA;
        break;
    }
    *detail = "profile rejection variant " + std::to_string(variant);
    if (!f.Reply(protocol::BuildCreateWorkerFrame(TopologyHeader(++f.request), config),
            protocol::MessageType::ERROR, expected) ||
        f.spawns || !f.conn.workers.empty() || f.conn.highest_session_id) return false;
  }
  return true;
}

bool WorkerTenancyAndIds(std::string* detail) {
  *detail = "foreign IDs must not address owner objects; fresh IDs share one namespace";
  TopologyFixture f;
  if (!f.OpenTwo() || !f.StartRuntime(2)) return false;
  router::Connection other;
  OpenLocal(other);
  other.conn_id = 2;
  uint32_t request = 1;
  auto header = [&](uint32_t target, uint32_t run = 0) {
    auto h = TopologyHeader(request++, target, run); h.conn_id = 2; return h;
  };
  auto reply = [&](const std::vector<uint8_t>& frame, protocol::MessageType type) {
    protocol::FrameHeader h;
    protocol::StatusCode error;
    return LocalRoute(*f.service, other, frame) &&
        PopFrame(other, &h, type == protocol::MessageType::ERROR ? &error : nullptr) &&
        h.type == type && (type != protocol::MessageType::ERROR || error == protocol::StatusCode::ERROR_BAD_STATE);
  };
  if (!reply(protocol::BuildCreateRuntimeFrame(header(4), {1, 0, 1, -1}), protocol::MessageType::ERROR) ||
      !reply(protocol::BuildStartRunFrame(header(2, 1), Script("host.complete();")), protocol::MessageType::ERROR) ||
      !reply(protocol::BuildCancelRunFrame(header(2, 1)), protocol::MessageType::ACK) ||
      !reply(protocol::BuildCloseRuntimeFrame(header(2)), protocol::MessageType::ACK) ||
      !reply(protocol::BuildCloseWorkerFrame(header(1)), protocol::MessageType::ACK)) return false;
  auto relay = header(2, 1);
  relay.request_id = 0; relay.type = protocol::MessageType::RELAY_TO_WORKER;
  const size_t posts = f.posts.size();
  if (!LocalRoute(*f.service, other, protocol::BuildRelayFrame(relay, sbox_msg_kind_string, nullptr, 0)) ||
      !other.sessions.empty() || !other.workers.empty() || other.highest_session_id ||
      !other.out_queue.empty() || f.posts.size() != posts || f.conn.sessions.at(2)->cancel_deadline !=
          std::chrono::steady_clock::time_point{}) return false;
  for (uint32_t id : {1u, 2u, 3u})
    if (!f.Runtime(id, protocol::StatusCode::ERROR_BAD_STATE)) return false;
  if (!f.Reply(protocol::BuildCloseWorkerFrame(TopologyHeader(++f.request, 2)), protocol::MessageType::ACK) ||
      !f.CloseRuntime(1) ||
      !f.Reply(protocol::BuildCloseSessionFrame(ControlHeader(++f.request, 2)), protocol::MessageType::ACK) ||
      f.conn.workers.size() != 1 || f.conn.sessions.size() != 2 || f.posts.size() != posts) return false;
  f.Inject(v8host::RunEnvelopeType::kResult, 0, 1);
  protocol::FrameHeader result;
  if (!PopFrame(f.conn, &result) || result.type != protocol::MessageType::RESULT ||
      result.target_id != 2 || result.run_id != 1 || !f.conn.out_queue.empty() ||
      !f.CloseRuntime(3) || !f.Runtime(3, protocol::StatusCode::ERROR_BAD_STATE)) return false;
  return f.conn.highest_session_id == 3 && f.spawns == 1;
}

bool RuntimeQueuedSlotChurn(std::string* detail) {
  *detail = "admission N/N+1, post-security ID order, closing slots remain occupied";
  TopologyFixture f;
  if (!f.OpenTwo() || !f.Runtime(4, protocol::StatusCode::ERROR_QUOTA) ||
      !f.CloseRuntime(2) || !f.CloseRuntime(3)) return false;
  const size_t posts = f.posts.size();
  if (!f.Runtime(4) || !f.Runtime(5) || !f.Runtime(6, protocol::StatusCode::ERROR_QUOTA) ||
      f.posts.size() != posts || f.conn.sessions.at(4)->runtime_state != router::RuntimeState::kQueued ||
      f.conn.sessions.at(5)->runtime_state != router::RuntimeState::kQueued ||
      !f.worker->slots[0].occupied || !f.worker->slots[1].occupied) return false;
  f.Inject(v8host::RunEnvelopeType::kRuntimeClosed, 1);
  if (f.posts.size() != posts + 1 || f.posts.back().type != v8host::RunEnvelopeType::kCreateRuntime ||
      f.posts.back().slot != 1 || f.conn.sessions.at(4)->slot != 1 ||
      f.conn.sessions.at(5)->runtime_state != router::RuntimeState::kQueued || !f.Ready(1, 4)) return false;
  f.Inject(v8host::RunEnvelopeType::kRuntimeClosed, 0);
  if (f.posts.size() != posts + 2 || f.posts.back().slot != 0 || f.conn.sessions.at(5)->slot != 0 ||
      !f.Ready(0, 5)) return false;
  // single_use counts admissions over the worker's entire lifetime.
  TopologyFixture single;
  if (!single.Worker(1, protocol::kWorkerFlagSingleUse) || !single.Runtime(2) ||
      !single.CloseRuntime(2) || !single.posts.empty() ||
      !single.Runtime(3, protocol::StatusCode::ERROR_QUOTA)) return false;
  return f.spawns == 1 && f.worker->created_runtimes == 4 && single.worker->created_runtimes == 1;
}

bool RuntimePreReadyStart(std::string* detail) {
  *detail = "queued and creating STARTs wait for RUNTIME_READY";
  TopologyFixture f;
  if (!f.Worker() || !f.Runtime(2) || !f.StartRuntime(2) || !f.posts.empty()) return false;
  f.Phase(1);
  if (!f.posts.empty()) return false;
  DrainLocal(f.conn);
  f.Phase(2);
  DrainLocal(f.conn);
  if (f.posts.size() != 1 || f.posts[0].type != v8host::RunEnvelopeType::kCreateRuntime ||
      !f.StartRuntime(2, 2) || f.posts.size() != 1 || !f.Ready(0, 2) ||
      f.posts.size() != 2 || f.posts.back().type != v8host::RunEnvelopeType::kStart ||
      f.posts.back().run_id != 1 || f.posts.back().slot != 0) return false;
  f.Inject(v8host::RunEnvelopeType::kResult, 0, 1);
  if (f.posts.size() != 3 || f.posts.back().run_id != 2) return false;
  DrainLocal(f.conn);
  f.Inject(v8host::RunEnvelopeType::kResult, 0, 2);
  protocol::FrameHeader h;
  return PopFrame(f.conn, &h) && h.type == protocol::MessageType::RESULT && h.target_id == 2 && h.run_id == 2;
}

bool RuntimeIndependentRunsCancels(std::string* detail) {
  *detail = "run 1 intake/credit/cancel are independent per runtime slot";
  TopologyFixture f;
  if (!f.OpenTwo() || !f.StartRuntime(2) || !f.StartRuntime(3)) return false;
  auto relay = [&](uint32_t id) {
    auto h = ControlHeader(0, id, 1); h.type = protocol::MessageType::RELAY_TO_WORKER;
    const uint8_t byte = static_cast<uint8_t>(id);
    return LocalRoute(*f.service, f.conn, protocol::BuildRelayFrame(h, sbox_msg_kind_binary, &byte, 1));
  };
  for (unsigned n = 0; n != 9; ++n)
    if (!relay(2) || !relay(3)) return false;
  if (f.posts.size() != 20 || f.conn.sessions.at(2)->credit || f.conn.sessions.at(3)->credit ||
      f.conn.sessions.at(2)->runs.at(1)->held_relays.size() != 1 ||
      f.conn.sessions.at(3)->runs.at(1)->held_relays.size() != 1) return false;
  f.Inject(v8host::RunEnvelopeType::kCredit, 0, 0, 1);
  if (f.posts.size() != 21 || f.posts.back().slot != 0 ||
      !f.conn.sessions.at(2)->runs.at(1)->held_relays.empty() ||
      f.conn.sessions.at(3)->runs.at(1)->held_relays.size() != 1) return false;
  f.Inject(v8host::RunEnvelopeType::kCredit, 1, 0, 1);
  if (f.posts.size() != 22 || f.posts.back().slot != 1 || f.conn.in_relay_bytes) return false;
  // Queue both run-1 replies before one pump, exercising published[slot][run].
  for (uint16_t slot : {0, 1}) {
    v8host::RunEnvelope env;
    env.type = v8host::RunEnvelopeType::kRelay; env.slot = slot; env.run_id = 1;
    env.relay_kind = sbox_msg_kind_binary; env.payload = {static_cast<uint8_t>(slot)};
    const auto bytes = v8host::EncodeRunEnvelope(env);
    router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
  }
  f.service->Pump(f.conn);
  for (uint32_t id : {2u, 3u}) {
    std::vector<uint8_t> bytes;
    protocol::FrameHeader h;
    const uint8_t* payload; size_t size;
    if (!f.conn.TakeOutput(&bytes)) return false;
    f.conn.CompleteOutput(bytes.size());
    int32_t kind; const uint8_t* body; size_t body_size;
    if (protocol::DecodeAndValidateFrame(bytes.data(), bytes.size(), &h, &payload, &size) != protocol::DecodeStatus::kOk ||
        h.type != protocol::MessageType::RELAY_FROM_WORKER || h.target_id != id || h.run_id != 1 ||
        !protocol::DecodeRelayPayload(payload, size, &kind, &body, &body_size) ||
        body_size != 1 || body[0] != id - 2) return false;
  }
  auto now = std::chrono::steady_clock::now();
  f.service->test_now = [&] { return now; };
  f.fail_post = true;
  for (uint32_t id : {2u, 3u})
    if (!f.Reply(protocol::BuildCancelRunFrame(ControlHeader(++f.request, id, 1)), protocol::MessageType::ACK))
      return false;
  if (f.conn.sessions.at(2)->cancel_deadline != now + std::chrono::seconds(5) ||
      f.conn.sessions.at(3)->cancel_deadline != now + std::chrono::seconds(5) ||
      f.conn.sessions.at(2)->pending_cancel_id != 1 || f.conn.sessions.at(3)->pending_cancel_id != 1 ||
      f.posts.size() != 22) return false;
  f.fail_post = false;
  now += std::chrono::milliseconds(100);
  f.service->Pump(f.conn);
  if (f.conn.sessions.at(2)->pending_cancel_id || f.conn.sessions.at(3)->pending_cancel_id ||
      f.posts.size() != 24 || f.posts[22].type != v8host::RunEnvelopeType::kCancel ||
      f.posts[23].type != v8host::RunEnvelopeType::kCancel || f.posts[22].slot != 0 || f.posts[23].slot != 1)
    return false;
  f.Inject(v8host::RunEnvelopeType::kResult, 0, 1);
  if (f.conn.sessions.at(2)->cancel_deadline != std::chrono::steady_clock::time_point{} ||
      f.conn.sessions.at(3)->cancel_deadline == std::chrono::steady_clock::time_point{}) return false;
  f.Inject(v8host::RunEnvelopeType::kCredit, 0, 0, 8);
  f.Inject(v8host::RunEnvelopeType::kResult, 1, 1);
  f.Inject(v8host::RunEnvelopeType::kCredit, 1, 0, 8);
  return f.conn.sessions.at(2)->credit == 8 && f.conn.sessions.at(3)->credit == 8 && f.conn.workers.size() == 1;
}

bool RuntimeInvalidOutputState(std::string* detail) {
  for (unsigned variant = 0; variant != 6; ++variant) {
    *detail = "invalid worker output variant " + std::to_string(variant);
    TopologyFixture f;
    if (!f.OpenTwo()) return false;
    if (variant == 0) f.Inject(v8host::RunEnvelopeType::kRuntimeReady, 0);
    if (variant == 1) f.Inject(v8host::RunEnvelopeType::kRuntimeClosed, 0);
    if (variant == 2) f.Inject(v8host::RunEnvelopeType::kCredit, 2, 0, 1);
    if (variant == 3) f.Inject(v8host::RunEnvelopeType::kCredit, 0, 0, 1);
    if (variant == 4) {
      if (!f.CloseRuntime(2)) return false;
      f.Inject(v8host::RunEnvelopeType::kRuntimeClosed, 0);
      f.Inject(v8host::RunEnvelopeType::kRuntimeReady, 0);
    }
    if (variant == 5) {
      // Exhaust the bounded mailbox with control output, before the strand drains.
      v8host::RunEnvelope env;
      env.type = v8host::RunEnvelopeType::kCredit; env.credit = 1;
      const auto bytes = v8host::EncodeRunEnvelope(env);
      for (size_t n = 0; n != f.worker->mailbox.size() + 1; ++n)
        router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, bytes.data(), bytes.size());
      if (!f.worker->mailbox_failed) return false;
      f.service->Pump(f.conn);
    }
    protocol::FrameHeader h;
    if (!f.conn.workers.empty() || !f.conn.sessions.empty() || !PopFrame(f.conn, &h) ||
        h.type != protocol::MessageType::WORKER_EXIT || h.target_id != 1 || !f.conn.out_queue.empty()) return false;
    f.service->Close(f.conn);
    if (f.closes != 1 || f.waits != 1 || f.post_after_close || f.close_before_removal) return false;
  }
  *detail = "engine rejects occupied/out-of-range/empty runtime slots";
  {
    EngineFixture f;
    if (!f.Warm()) return false;
    v8host::RunEnvelope env;
    env.type = v8host::RunEnvelopeType::kCreateRuntime;
    if (V8HostEngineTestRoute({env})) return false;
    env.type = v8host::RunEnvelopeType::kStart;
    env.run_id = 1; env.slot = 1;
    if (V8HostEngineTestRoute({env})) return false;
  }
  {
    EngineFixture f(2);
    if (!f.Warm()) return false;
    for (auto type : {v8host::RunEnvelopeType::kStart, v8host::RunEnvelopeType::kRelay,
                     v8host::RunEnvelopeType::kCancel, v8host::RunEnvelopeType::kCloseRuntime}) {
      v8host::RunEnvelope env;
      env.type = type; env.slot = 1;
      env.run_id = type == v8host::RunEnvelopeType::kCloseRuntime ? 0 : 1;
      env.relay_kind = sbox_msg_kind_binary;
      if (V8HostEngineTestRoute({env})) return false;
    }
  }
  *detail = "engine drops crossed frames after RUNTIME_CLOSED and permits slot reuse";
  {
    EngineFixture f(2);
    if (!f.Warm()) return false;
    v8host::RunEnvelope create;
    create.type = v8host::RunEnvelopeType::kCreateRuntime;
    if (!V8HostEngineTestRoute({create}) || !V8HostEngineTestWaitSlot(0, false) ||
        f.output.size() != 1 || f.output[0].type != v8host::RunEnvelopeType::kRuntimeReady ||
        f.output[0].slot != 0) return false;
    v8host::RunEnvelope close;
    close.type = v8host::RunEnvelopeType::kCloseRuntime;
    if (!V8HostEngineTestRoute({close}) || !V8HostEngineTestWaitSlot(0, true) ||
        f.output.size() != 2 || f.output[1].type != v8host::RunEnvelopeType::kRuntimeClosed ||
        f.output[1].slot != 0 || f.output[1].status_code != 0) return false;
    std::vector<v8host::RunEnvelope> late;
    for (auto type : {v8host::RunEnvelopeType::kStart, v8host::RunEnvelopeType::kRelay,
                     v8host::RunEnvelopeType::kCancel, v8host::RunEnvelopeType::kCloseRuntime}) {
      v8host::RunEnvelope env;
      env.type = type;
      env.run_id = type == v8host::RunEnvelopeType::kCloseRuntime ? 0 : 1;
      env.relay_kind = sbox_msg_kind_binary;
      late.push_back(std::move(env));
    }
    if (!V8HostEngineTestRoute(late) || f.output.size() != 2 ||
        !V8HostEngineTestRoute({create}) || !V8HostEngineTestWaitSlot(0, false) ||
        f.output.size() != 3 || f.output[2].type != v8host::RunEnvelopeType::kRuntimeReady ||
        f.output[2].slot != 0 || f.invalid) return false;
    for (auto& env : late) env.slot = 1;
    for (const auto& env : late)
      if (V8HostEngineTestRoute({env})) return false;
  }
  *detail = "each closing slot has its own 5-second deadline";
  for (uint32_t first : {2u, 3u}) {
    TopologyFixture f;
    if (!f.OpenTwo()) return false;
    auto now = std::chrono::steady_clock::now();
    f.service->test_now = [&] { return now; };
    if (!f.CloseRuntime(first)) return false;
    now += std::chrono::seconds(1);
    if (!f.CloseRuntime(5 - first)) return false;
    now += std::chrono::milliseconds(3999);
    f.service->Pump(f.conn);
    if (f.conn.workers.size() != 1) return false;
    now += std::chrono::milliseconds(1);
    f.service->Pump(f.conn);
    protocol::FrameHeader h;
    if (!f.conn.workers.empty() || !PopFrame(f.conn, &h) || h.type != protocol::MessageType::WORKER_EXIT)
      return false;
  }
  return true;
}

bool RuntimeRawOverrides(std::string* detail) {
  *detail = "every runtime override ACKs before PROFILE_ALREADY_BOUND, without dispatch";
  TopologyFixture f;
  if (!f.OpenTwo()) return false;
  const size_t posts = f.posts.size();
  for (uint32_t variant = 0; variant != 5; ++variant) {
    auto inputs = Script("host.complete();");
    if (variant == 0) inputs.tier_override = 0;
    if (variant == 1) inputs.tier_override = 1;
    if (variant == 2) { inputs.has_engine_override = true; inputs.engine_filename = "v8jsisb.dll"; }
    if (variant == 3) { inputs.has_snapshot = true; inputs.snapshot_path = "snapshot.bin"; }
    if (variant == 4) {
      inputs.has_engine_override = true; inputs.engine_filename = "v8jsisb.dll";
      inputs.has_snapshot = true; inputs.snapshot_path = "snapshot.bin";
    }
    if (!f.StartRuntime(2, variant + 1, inputs)) return false;
    protocol::FrameHeader h;
    protocol::StatusCode code;
    if (!PopFrame(f.conn, &h, &code) || h.type != protocol::MessageType::RUN_ERROR ||
        h.target_id != 2 || h.run_id != variant + 1 || h.request_id != 0 ||
        code != protocol::StatusCode::ERROR_PROFILE_ALREADY_BOUND || !f.conn.out_queue.empty() ||
        f.posts.size() != posts || !f.conn.sessions.at(2)->runs.empty()) return false;
  }
  return f.StartRuntime(2, 6) && f.posts.size() == posts + 1;
}

bool LifecycleFailure(unsigned failure, std::string *detail) {
  CapturedBroker broker;
  broker.failure_selector = std::to_string(failure);
  WireHarness h;
  if (!OpenCaptured(broker, h, detail) ||
      !h.Send(protocol::BuildStartRunFrame(h.Header(2, 1, 1), Script("host.postMessage('unexpected-run');"))) ||
      !h.Expect(protocol::MessageType::ACK, 2, 1, 1) ||
      (failure != 1 && !h.Expect(protocol::MessageType::STARTUP_READY, 0, 1)) ||
      !h.Expect(protocol::MessageType::WORKER_EXIT, 0, 1)) {
    *detail = "failed child readiness/EXIT ordering";
    return false;
  }
  // A live control barrier proves there was no synthetic result/security/second EXIT.
  if (!h.Send(protocol::BuildCancelRunFrame(h.Header(3, 1, 1))) ||
      !h.Expect(protocol::MessageType::ACK, 3, 1, 1))
    return false;
  h.connection.Close();
  const std::string expected = "[sbox-test] failure=" + std::to_string(failure) +
      " lower=" + (failure == 1 ? "0" : "1") + " run=0 shutdown=1 end=1";
  if (!broker.Reaped(1) || broker.lifecycle_evidence != 1 || broker.lifecycle_failure != expected) {
    *detail = "selected worker branch/resource cleanup not observed (count=" + std::to_string(broker.lifecycle_evidence) + ",size=" + std::to_string(broker.lifecycle_failure.size()) + "): " + broker.lifecycle_failure;
    return false;
  }
  return true;
}
bool LifecycleFailStartup(std::string *d) { return LifecycleFailure(1, d); }
bool LifecycleFailSecurity(std::string *d) { return LifecycleFailure(2, d); }
bool PingPostFail(std::string *d) { return LifecycleFailure(3, d); }
bool RealCancelOrdering(bool cancel_first, std::string *detail) {
  CapturedBroker broker;
  WireHarness h;
  if (!OpenCaptured(broker, h, detail) ||
      !h.Send(protocol::BuildStartRunFrame(h.Header(2, 1, 1), Script(
          "host.onmessage=function(m){host.complete();};host.postMessage('armed');"))) ||
      !h.Expect(protocol::MessageType::ACK, 2, 1, 1) || !h.Expect(protocol::MessageType::STARTUP_READY, 0, 1) ||
      !h.Expect(protocol::MessageType::SECURITY_READY, 0, 1) || !ExpectString(h, 1, "armed"))
    return false;
  if (!cancel_first && (!SendString(h, 1, "complete") ||
      !ExpectResult(h, 1, protocol::ResultDisposition::kCompleted)))
    return false;
  if (!h.Send(protocol::BuildStartRunFrame(h.Header(3, 1, 2), Script(
          "host.onmessage=function(m){host.postMessage('next:'+m);host.complete();};"))) ||
      !h.Expect(protocol::MessageType::ACK, 3, 1, 2) || !SendString(h, 2, "held") ||
      !h.Send(protocol::BuildCancelRunFrame(h.Header(4, 1, 1))) ||
      !h.Expect(protocol::MessageType::ACK, 4, 1, 1) ||
      (cancel_first && !ExpectResult(h, 1, protocol::ResultDisposition::kCancelled)) ||
      !ExpectString(h, 2, "next:held") || !ExpectResult(h, 2, protocol::ResultDisposition::kCompleted) ||
      !SendString(h, 1, "late") || !h.Send(protocol::BuildCancelRunFrame(h.Header(5, 1, 1))) ||
      !h.Expect(protocol::MessageType::ACK, 5, 1, 1)) {
    *detail = "cancel terminal/successor FIFO/late-output barrier";
    return false;
  }
  h.connection.Close();
  return broker.Reaped(1);
}
bool CancelsFirst(std::string *d) { return RealCancelOrdering(true, d); }
bool CompletesFirst(std::string *d) { return RealCancelOrdering(false, d); }
bool RealTwoRunReuse(std::string *detail) {
  CapturedBroker broker;
  WireHarness h;
  if (!OpenCaptured(broker, h, detail) ||
      !h.Send(protocol::BuildStartRunFrame(h.Header(2, 1, 1), Script(
          "host.onmessage=function(m){host.postMessage('A:'+m);if(m==='finish')host.complete();};"))) ||
      !h.Expect(protocol::MessageType::ACK, 2, 1, 1) || !h.Expect(protocol::MessageType::STARTUP_READY, 0, 1) ||
      !h.Expect(protocol::MessageType::SECURITY_READY, 0, 1) ||
      !h.Send(protocol::BuildStartRunFrame(h.Header(3, 1, 2), Script(
          "var n=0;host.onmessage=function(m){host.postMessage('B:'+m);if(++n===2)host.complete();};"))) ||
      !SendString(h, 2, "one") || !SendString(h, 2, "two") || !h.Expect(protocol::MessageType::ACK, 3, 1, 2) ||
      !SendString(h, 1, "hold") || !ExpectString(h, 1, "A:hold") ||
      !SendString(h, 1, "finish") || !ExpectString(h, 1, "A:finish") ||
      !ExpectResult(h, 1, protocol::ResultDisposition::kCompleted) || !ExpectString(h, 2, "B:one") ||
      !ExpectString(h, 2, "B:two") || !ExpectResult(h, 2, protocol::ResultDisposition::kCompleted)) {
    *detail = "real held B dispatched before A or lost FIFO";
    return false;
  }
  for (uint32_t id = 3; id <= 5; ++id) {
    auto start = Script("host.complete();");
    if (id == 3)
      start.tier_override = v8host::kTierTrusted;
    else if (id == 4) {
      start.has_engine_override = true;
      start.engine_filename = "other.dll";
    } else {
      start.has_snapshot = true;
      start.snapshot_path = "other.snapshot";
    }
    std::vector<uint8_t> bytes;
    protocol::FrameHeader header;
    protocol::ErrorPayload error;
    if (!h.Send(protocol::BuildStartRunFrame(h.Header(id + 1, 1, id), start)) ||
        !h.Expect(protocol::MessageType::ACK, id + 1, 1, id) || !h.Receive(&bytes, &header) ||
        header.type != protocol::MessageType::RUN_ERROR || header.run_id != id || header.request_id != 0 ||
        !protocol::DecodeErrorPayload(bytes.data() + protocol::kFrameHeaderSize,
            bytes.size() - protocol::kFrameHeaderSize, &error) ||
        error.status_code != protocol::StatusCode::ERROR_PROFILE_ALREADY_BOUND) {
      *detail = "real profile conflict id=" + std::to_string(id) + " type=" +
          std::to_string(static_cast<unsigned>(header.type)) + " code=" +
          std::to_string(static_cast<unsigned>(error.status_code));
      return false;
    }
  }
  if (!h.Send(protocol::BuildStartRunFrame(h.Header(7, 1, 6), Script("host.postMessage('reused');host.complete();"))) ||
      !h.Expect(protocol::MessageType::ACK, 7, 1, 6) || !ExpectString(h, 6, "reused") ||
      !ExpectResult(h, 6, protocol::ResultDisposition::kCompleted) ||
      !h.Send(protocol::BuildCloseSessionFrame(h.Header(8))) || !h.Expect(protocol::MessageType::ACK, 8, 1)) {
    *detail = "same worker reuse/sentinel terminal barrier";
    return false;
  }
  h.connection.Close();
  return broker.Reaped(1);
}
bool LiveWorkerCleanup(bool close, std::string *detail) {
  CapturedBroker broker;
  WireHarness h;
  if (!OpenCaptured(broker, h, detail) ||
      !h.Send(protocol::BuildStartRunFrame(h.Header(2, 1, 1), Script(
          "host.onmessage=function(m){host.postMessage(m);};host.postMessage('armed');"))) ||
      !h.Expect(protocol::MessageType::ACK, 2, 1, 1) || !h.Expect(protocol::MessageType::STARTUP_READY, 0, 1) ||
      !h.Expect(protocol::MessageType::SECURITY_READY, 0, 1) || !ExpectString(h, 1, "armed")) {
    *detail = "live worker sentinel";
    return false;
  }
  if (close && (!h.Send(protocol::BuildCloseSessionFrame(h.Header(3))) ||
      !ExpectResult(h, 1, protocol::ResultDisposition::kCancelled) || !h.Expect(protocol::MessageType::ACK, 3, 1))) {
    *detail = "CLOSE live worker terminal/ACK barrier";
    return false;
  }
  h.connection.Close();
  if (!broker.Reaped(1)) {
    *detail = "held live worker did not signal or respawned";
    return false;
  }
  return true;
}
bool CloseLiveWorker(std::string *detail) { return LiveWorkerCleanup(true, detail); }
bool DisconnectLiveWorker(std::string *detail) { return LiveWorkerCleanup(false, detail); }
bool StartSpawn(std::string *detail) {
  CapturedBroker broker;
  if (!broker.Launch(detail))
    return false;
  WireHarness h;
  if (!ConnectAndHandshake(BrokerMode::kShared, 900, &h.connection, &h.hello, detail) ||
      h.connection.broker_pid() != ::GetProcessId(broker.process)) {
    *detail = "not connected to captured broker";
    return false;
  }
  auto expect = [&](protocol::MessageType type, uint32_t request, uint32_t session, uint32_t run,
                    const char *event) {
    if (!h.Expect(type, request, session, run)) return false;
    std::printf("[start-spawn] %s\n", event);
    return true;
  };
  if (!h.Send(protocol::BuildCreateSessionFrame(h.Header(1), [] {
        auto config = LogicalConfig(); config.broker_mode = 1;
        config.initial_token = sbox_token_level_restricted_same_access;
        config.delayed_integrity = sbox_integrity_level_untrusted; return config;
      }())) || !expect(protocol::MessageType::ACK, 1, 1, 0, "CREATE ACK") ||
      !expect(protocol::MessageType::SESSION_READY, 0, 1, 0, "SESSION_READY")) {
    *detail = "CREATE readiness";
    return false;
  }
  protocol::StartRunPayload start;
  start.tier_override = -1;
  const std::string guest = "var n=0;host.onmessage=function(m){if(typeof m==='string')host.postMessage('echo:'+m);"
      "else {var a=new Uint8Array(m);a[0]=66;host.postMessageBinary(m);}if(++n===2)host.complete();};";
  start.guest_payload.assign(guest.begin(), guest.end());
  auto relay = h.Header(0, 1, 1);
  relay.type = protocol::MessageType::RELAY_TO_WORKER;
  const uint8_t binary[] = {1, 2, 3};
  if (!h.Send(protocol::BuildStartRunFrame(h.Header(2, 1, 1), start)) ||
      !h.Send(protocol::BuildRelayFrame(relay, sbox_msg_kind_string, reinterpret_cast<const uint8_t *>("one"), 3)) ||
      !h.Send(protocol::BuildRelayFrame(relay, sbox_msg_kind_binary, binary, sizeof(binary))) ||
      !expect(protocol::MessageType::ACK, 2, 1, 1, "START ACK") ||
      !expect(protocol::MessageType::STARTUP_READY, 0, 1, 0, "STARTUP") ||
      !expect(protocol::MessageType::SECURITY_READY, 0, 1, 0, "SECURITY")) {
    *detail = "START/readiness";
    return false;
  }
  for (int i = 0; i < 2; ++i) {
    std::vector<uint8_t> bytes;
    protocol::FrameHeader header;
    int32_t kind;
    const uint8_t *body;
    size_t size;
    if (!h.Receive(&bytes, &header) || header.type != protocol::MessageType::RELAY_FROM_WORKER ||
        header.run_id != 1 || !protocol::DecodeRelayPayload(bytes.data() + protocol::kFrameHeaderSize,
            bytes.size() - protocol::kFrameHeaderSize, &kind, &body, &size) ||
        (i == 0 ? kind != sbox_msg_kind_string || std::string(reinterpret_cast<const char *>(body), size) != "echo:one"
                : kind != sbox_msg_kind_binary || std::vector<uint8_t>(body, body + size) != std::vector<uint8_t>{66, 2, 3})) {
      *detail = "JS relay echo " + std::to_string(i);
      return false;
    }
    std::printf("[start-spawn] RELAY echo kind=%s bytes=%zu\n", i == 0 ? "string" : "binary", size);
  }
  std::vector<uint8_t> bytes;
  protocol::FrameHeader header;
  protocol::ResultPayload result;
  if (!h.Receive(&bytes, &header) || header.type != protocol::MessageType::RESULT || header.run_id != 1 ||
      !protocol::DecodeResultPayload(bytes.data() + protocol::kFrameHeaderSize,
          bytes.size() - protocol::kFrameHeaderSize, &result) || result.disposition != protocol::ResultDisposition::kCompleted) {
    *detail = "sole RESULT";
    return false;
  }
  std::printf("[start-spawn] RESULT completed count=1\n");
  if (!h.Send(protocol::BuildCloseSessionFrame(h.Header(3))) || !h.Expect(protocol::MessageType::ACK, 3, 1)) {
    *detail = "sole RESULT/CLOSE barrier";
    return false;
  }
  h.connection.Close();
  if (!broker.Reaped(1)) {
    *detail = "spawn count/held worker exit";
    return false;
  }
  return true;
}
bool UnknownTypeIgnored(std::string *detail) {
  router::Connection conn;
  OpenLocal(conn);
  router::Router service(BrokerMode::kDedicated);
  for (uint16_t type : {0x000f, 0x0106, 0x0202, 0xffff}) {
    auto header = ControlHeader(0, 0);
    header.type = static_cast<protocol::MessageType>(type);
    header.payload_length = 3;
    std::vector<uint8_t> bytes;
    protocol::EncodeHeader(header, bytes);
    bytes.insert(bytes.end(), {0xff, 0, 0x80});
    if (!LocalRoute(service, conn, bytes) || conn.state != router::ConnState::kOpen || !conn.out_queue.empty() ||
        !conn.sessions.empty() || conn.request_cache.size() != 0) {
      *detail = "unknown optional type was not consumed without effects";
      return false;
    }
  }
  WireHarness h;
  if (!h.Open(detail))
    return false;
  auto header = h.Header(1, 0);
  header.type = static_cast<protocol::MessageType>(0xffff);
  std::vector<uint8_t> bytes;
  protocol::EncodeHeader(header, bytes);
  // Same request id must remain fresh, and no unknown-type reply may precede ACK.
  return h.Send(std::move(bytes)) && h.Send(protocol::BuildCreateSessionFrame(h.Header(1), LogicalConfig())) &&
      h.Expect(protocol::MessageType::ACK, 1, 1) && h.Expect(protocol::MessageType::SESSION_READY, 0, 1) && h.Finish();
}
bool UnknownMustUnderstand(std::string *detail) {
  WireHarness h;
  if (!h.Open(detail))
    return false;
  auto unknown = h.Header(17, 0, 3);
  unknown.type = static_cast<protocol::MessageType>(0xffff);
  unknown.flags = protocol::kFlagMustUnderstand;
  unknown.payload_length = 1;
  std::vector<uint8_t> bytes;
  protocol::EncodeHeader(unknown, bytes);
  bytes.push_back(0xff);
  // Delay the read until exit to prove the error was drained, not just enqueued.
  if (!h.Send(std::move(bytes)) || ::WaitForSingleObject(h.process, 3000) != WAIT_OBJECT_0) {
    *detail = "unsupported mandatory type did not close the broker";
    return false;
  }
  protocol::FrameHeader header;
  const uint8_t *payload;
  size_t size;
  protocol::ErrorPayload error;
  if (!h.Receive(&bytes, &header) || header.type != protocol::MessageType::ERROR ||
      header.conn_id != unknown.conn_id || header.target_id != unknown.target_id || header.run_id != unknown.run_id ||
      header.request_id != unknown.request_id || header.flags != unknown.flags ||
      header.version_major != unknown.version_major || header.version_minor != unknown.version_minor ||
      protocol::DecodeAndValidateFrame(bytes.data(), bytes.size(), &header, &payload, &size) !=
          protocol::DecodeStatus::kOk ||
      !protocol::DecodeErrorPayload(payload, size, &error) ||
      error.status_code != protocol::StatusCode::ERROR_UNSUPPORTED_MESSAGE) {
    *detail = "missing or incorrectly correlated ERROR_UNSUPPORTED_MESSAGE";
    return false;
  }
  return h.Closed();
}
bool HelloVersionReject(std::string *detail) {
  WireHarness h;
  BrokerRendezvous rendezvous(ExecutableDirectory(), BrokerMode::kDedicated);
  if (rendezvous.ConnectOrLaunch(&h.connection) != RendezvousStatus::kOk)
    return false;
  h.process = ::OpenProcess(SYNCHRONIZE, FALSE, h.connection.broker_pid());
  if (!h.process)
    return false;
  auto hello = ControlHeader(901, 0);
  hello.conn_id = 0;
  ++hello.version_major;
  // Read only after server teardown, so unread reject delivery is deterministic.
  if (!h.Send(protocol::BuildHelloFrame(hello)) || ::WaitForSingleObject(h.process, 3000) != WAIT_OBJECT_0) {
    *detail = "version reject did not finish bounded teardown";
    return false;
  }
  std::vector<uint8_t> bytes;
  protocol::FrameHeader header;
  const uint8_t *payload;
  size_t size;
  protocol::ErrorPayload error;
  if (!h.Receive(&bytes, &header) || header.type != protocol::MessageType::ERROR ||
      header.request_id != hello.request_id || header.conn_id == 0 || header.conn_id == UINT32_MAX ||
      header.version_major != protocol::kWireVersionMajor || header.version_minor != protocol::kWireVersionMinor ||
      header.target_id != 0 || header.run_id != 0 ||
      protocol::DecodeAndValidateFrame(bytes.data(), bytes.size(), &header, &payload, &size) !=
          protocol::DecodeStatus::kOk ||
      !protocol::DecodeErrorPayload(payload, size, &error) || error.status_code != protocol::StatusCode::ERROR_VERSION) {
    *detail = "client did not read ERROR_VERSION after broker exit";
    return false;
  }
  return h.Closed();
}
bool CreateReady(std::string *detail) {
  WireHarness h;
  if (!h.Open(detail))
    return false;
  if (!h.Send(protocol::BuildCreateSessionFrame(h.Header(1), LogicalConfig())) ||
      !h.Expect(protocol::MessageType::ACK, 1, 1) || !h.Expect(protocol::MessageType::SESSION_READY, 0, 1) ||
      !h.Send(protocol::BuildCloseSessionFrame(h.Header(2))) || !h.Expect(protocol::MessageType::ACK, 2, 1)) {
    *detail = "CREATE ACK/READY then CLOSE ACK";
    return false;
  }
  return h.Finish();
}

bool PersistentControl(std::string *detail) {
  WireHarness h;
  if (!h.Open(detail))
    return false;
  for (uint32_t id = 1; id <= 3; ++id) {
    if (!h.Send(protocol::BuildCreateSessionFrame(h.Header(id, id), LogicalConfig())) ||
        !h.Expect(protocol::MessageType::ACK, id, id) || !h.Expect(protocol::MessageType::SESSION_READY, 0, id))
      return false;
  }
  if (!h.Send(protocol::BuildCancelRunFrame(h.Header(4, 99, 99))) || !h.Expect(protocol::MessageType::ACK, 4, 99, 99))
    return false;
  auto relay = h.Header(0, 99, 99);
  relay.type = protocol::MessageType::RELAY_TO_WORKER;
  if (!h.Send(protocol::BuildRelayFrame(relay, sbox_msg_kind_binary, nullptr, 0)) ||
      !h.Send(protocol::BuildCloseSessionFrame(h.Header(5, 2))) || !h.Expect(protocol::MessageType::ACK, 5, 2))
    return false;
  return h.Finish();
}
bool HeaderDirectionReject(std::string *detail) {
  for (int test = 0; test < 18; ++test) {
    WireHarness h;
    if (!h.Open(detail))
      return false;
    auto header = h.Header(1);
    std::vector<uint8_t> bytes;
    switch (test) {
      case 0:
        ++header.conn_id;
        break;
      case 1:
        header.target_id = 0;
        break;
      case 2:
        header.run_id = 1;
        break;
      case 3:
        header.request_id = 0;
        break;
      case 4:
        ++header.version_major;
        break;
      case 5:
        ++header.version_minor;
        break;
      default:
        break;
    }
    bytes = protocol::BuildCreateSessionFrame(header, LogicalConfig());
    if (test == 6)
      bytes[0] ^= 1;
    if (test == 7)
      bytes.push_back(0);
    if (test == 8) {
      bytes = protocol::BuildAckFrame(header);
    }
    if (test == 9) {
      bytes = protocol::BuildSessionReadyFrame(header);
    }
    if (test == 10) {
      bytes = protocol::BuildCloseSessionFrame(header);
      bytes.push_back(0);
    }
    if (test == 11) {
      bytes = protocol::BuildCreateSessionFrame(header, LogicalConfig());
      bytes[32] = 99;
    }
    if (test == 12) {
      bytes.resize(protocol::kMaxFrameSize + 1, 0);
    }
    if (test == 13) {
      header.target_id = UINT32_MAX;
      bytes = protocol::BuildCreateSessionFrame(header, LogicalConfig());
    }
    if (test == 14) {
      header.request_id = UINT32_MAX;
      bytes = protocol::BuildCreateSessionFrame(header, LogicalConfig());
    }
    if (test == 15) {
      header.run_id = UINT32_MAX;
      bytes = protocol::BuildCancelRunFrame(header);
    }
    if (test == 16 || test == 17) {
      header.run_id = 1;
      header.request_id = 0;
      header.type = protocol::MessageType::RELAY_TO_WORKER;
      bytes = protocol::BuildRelayFrame(header, sbox_msg_kind_lifecycle, nullptr, 0);
      if (test == 17) {
        header.payload_length = 0;
        bytes.clear();
        protocol::EncodeHeader(header, bytes);
      }
    }
    if (!h.Send(std::move(bytes)) || !h.Closed()) {
      *detail = "header/direction/malformed case=" + std::to_string(test);
      return false;
    }
  }
  return true;
}
bool ControlCache(std::string *detail) {
  WireHarness h;
  if (!h.Open(detail))
    return false;
  auto create = protocol::BuildCreateSessionFrame(h.Header(1), LogicalConfig());
  if (!h.Send(create) || !h.Expect(protocol::MessageType::ACK, 1, 1) ||
      !h.Expect(protocol::MessageType::SESSION_READY, 0, 1) || !h.Send(create) ||
      !h.Expect(protocol::MessageType::ACK, 1, 1) || !h.Send(protocol::BuildCloseSessionFrame(h.Header(2))) ||
      !h.Expect(protocol::MessageType::ACK, 2, 1))
    return false;
  for (uint32_t request = 3; request <= 257; ++request) {
    if (!h.Send(protocol::BuildCancelRunFrame(h.Header(request, 99, 99))) ||
        !h.Expect(protocol::MessageType::ACK, request, 99, 99))
      return false;
  }
  if (!h.Send(create))
    return false;
  std::vector<uint8_t> bytes;
  protocol::FrameHeader header;
  const uint8_t *payload;
  size_t size;
  protocol::ErrorPayload error;
  if (!h.Receive(&bytes, &header) || header.type != protocol::MessageType::ERROR ||
      protocol::DecodeAndValidateFrame(bytes.data(), bytes.size(), &header, &payload, &size) !=
          protocol::DecodeStatus::kOk ||
      !protocol::DecodeErrorPayload(payload, size, &error) ||
      error.status_code != protocol::StatusCode::ERROR_STALE_REQUEST || !h.Finish())
    return false;
  WireHarness mismatch;
  if (!mismatch.Open(detail))
    return false;
  auto config = LogicalConfig();
  if (!mismatch.Send(protocol::BuildCreateSessionFrame(mismatch.Header(1), config)) ||
      !mismatch.Expect(protocol::MessageType::ACK, 1, 1) ||
      !mismatch.Expect(protocol::MessageType::SESSION_READY, 0, 1))
    return false;
  config.initial_token = sbox_token_level_limited;
  if (!mismatch.Send(protocol::BuildCreateSessionFrame(mismatch.Header(1), config)) || !mismatch.Closed())
    return false;
  router::Connection conn;
  OpenLocal(conn);
  router::Router service(BrokerMode::kDedicated);
  for (uint32_t request = 1; request <= 257; ++request) {
    if (!LocalRoute(service, conn, protocol::BuildCancelRunFrame(ControlHeader(request, 1, 1))))
      return false;
    DrainLocal(conn);
    if (conn.request_cache.size() != (std::min)(request, 256u))
      return false;
  }
  if (!LocalRoute(service, conn, protocol::BuildCancelRunFrame(ControlHeader(1, 1, 1))))
    return false;
  protocol::StatusCode code;
  if (!PopFrame(conn, &header, &code) || code != protocol::StatusCode::ERROR_STALE_REQUEST || !conn.sessions.empty())
    return false;
  auto changed = ControlHeader(257, 2, 1);
  if (LocalRoute(service, conn, protocol::BuildCancelRunFrame(changed)))
    return false;
  return true;
}
bool SessionQuota32(std::string *detail) {
  WireHarness h;
  if (!h.Open(detail))
    return false;
  for (uint32_t id = 1; id <= 32; ++id) {
    if (!h.Send(protocol::BuildCreateSessionFrame(h.Header(id, id), LogicalConfig())) ||
        !h.Expect(protocol::MessageType::ACK, id, id) || !h.Expect(protocol::MessageType::SESSION_READY, 0, id))
      return false;
  }
  if (!h.Send(protocol::BuildCreateSessionFrame(h.Header(33, 33), LogicalConfig())))
    return false;
  std::vector<uint8_t> bytes;
  protocol::FrameHeader header;
  const uint8_t *payload;
  size_t size;
  protocol::ErrorPayload error;
  if (!h.Receive(&bytes, &header) || header.type != protocol::MessageType::ERROR || header.request_id != 33 ||
      header.target_id != 33 ||
      protocol::DecodeAndValidateFrame(bytes.data(), bytes.size(), &header, &payload, &size) !=
          protocol::DecodeStatus::kOk ||
      !protocol::DecodeErrorPayload(payload, size, &error) || error.status_code != protocol::StatusCode::ERROR_QUOTA ||
      !h.Send(protocol::BuildCloseSessionFrame(h.Header(34))) || !h.Expect(protocol::MessageType::ACK, 34, 1) ||
      !h.Send(protocol::BuildCreateSessionFrame(h.Header(35, 34), LogicalConfig())) ||
      !h.Expect(protocol::MessageType::ACK, 35, 34) || !h.Expect(protocol::MessageType::SESSION_READY, 0, 34))
    return false;
  return h.Finish();
}
bool IdRetirement(std::string *detail) {
  router::Connection conn;
  OpenLocal(conn);
  router::Router service(BrokerMode::kDedicated);
  auto create = protocol::BuildCreateSessionFrame(ControlHeader(1), LogicalConfig());
  if (!LocalRoute(service, conn, create))
    return false;
  DrainLocal(conn);
  auto *session = conn.sessions.at(1).get();
  session->worker = std::make_unique<router::Worker>();
  session->active_run_id = 5;
  session->pending_run_ids = {5};
  auto owned_run = std::make_unique<router::Run>();
  owned_run->session = session;
  owned_run->run_id = 5;
  auto *run = owned_run.get();
  session->runs.emplace(5, std::move(owned_run));
  bool removed_before_emit = false;
  session->terminal_removed = [&] {
    removed_before_emit = !conn.sessions.contains(1) && session->runs.empty() && conn.out_queue.empty();
  };
  HANDLE grabbed = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!grabbed || !release) {
    if (grabbed)
      ::CloseHandle(grabbed);
    if (release)
      ::CloseHandle(release);
    return false;
  }
  bool contender_safe = false;
  std::thread contender([&] {
    ::SetEvent(grabbed);
    if (::WaitForSingleObject(release, 3000) == WAIT_OBJECT_0)
      contender_safe = session->state == router::SessionState::kClosed && session->session_id == 1 &&
          run->state == router::RunState::kCancelled && !run->TryTerminal(router::RunState::kCompleted);
  });
  bool retired = ::WaitForSingleObject(grabbed, 3000) == WAIT_OBJECT_0 &&
      LocalRoute(service, conn, protocol::BuildCloseSessionFrame(ControlHeader(2))) && conn.sessions.empty() &&
      conn.retired_sessions.size() == 1 && conn.retired_sessions.front().get() == session &&
      session->retired_runs.size() == 1 && session->retired_runs.front().get() == run && removed_before_emit &&
      session->active_run_id == 0 && session->pending_run_ids.empty();
  ::SetEvent(release);
  contender.join();
  ::CloseHandle(grabbed);
  ::CloseHandle(release);
  if (!retired || !contender_safe) {
    *detail = "CLOSE did not remove ids and retain Session/Run for a delayed contender";
    return false;
  }
  protocol::FrameHeader terminal;
  if (!PopFrame(conn, &terminal) || terminal.type != protocol::MessageType::RESULT || terminal.run_id != 5 ||
      !PopFrame(conn, &terminal) || terminal.type != protocol::MessageType::ACK || terminal.request_id != 2 ||
      !conn.out_queue.empty() || !LocalRoute(service, conn, protocol::BuildCloseSessionFrame(ControlHeader(2))) ||
      conn.retired_sessions.size() != 1 || !PopFrame(conn, &terminal) || terminal.type != protocol::MessageType::ACK ||
      !conn.out_queue.empty())
    return false;
  if (!LocalRoute(service, conn, protocol::BuildCreateSessionFrame(ControlHeader(3), LogicalConfig())))
    return false;
  protocol::FrameHeader header;
  protocol::StatusCode code;
  if (!PopFrame(conn, &header, &code) || code != protocol::StatusCode::ERROR_BAD_STATE || !conn.sessions.empty())
    return false;
  if (!LocalRoute(service, conn, create))
    return false; // Replay ACK only, never recreate.
  if (!PopFrame(conn, &header) || header.type != protocol::MessageType::ACK || !conn.sessions.empty())
    return false;
  {
    router::Connection exhausted;
    OpenLocal(exhausted);
    if (LocalRoute(service, exhausted, protocol::BuildCreateSessionFrame(ControlHeader(4, UINT32_MAX), LogicalConfig())) ||
        !exhausted.sessions.empty() || ::WaitForSingleObject(exhausted.stop, 0) != WAIT_OBJECT_0) {
      *detail = "exhausted id accepted or connection not stopped";
      return false;
    }
  }
  if (!LocalRoute(service, conn, protocol::BuildCreateSessionFrame(ControlHeader(5, 2), LogicalConfig())))
    return false;
  DrainLocal(conn);
  if (LocalRoute(service, conn, protocol::BuildCreateSessionFrame(ControlHeader(6, 2), LogicalConfig()))) {
    *detail = "fresh create reused live id";
    return false;
  }
  auto *second_session = conn.sessions.at(2).get();
  auto second_run = std::make_unique<router::Run>();
  second_run->session = second_session;
  second_run->run_id = 6;
  auto *disconnect_contender = second_run.get();
  second_session->runs.emplace(6, std::move(second_run));
  bool removed_on_disconnect = false;
  second_session->terminal_removed = [&] { removed_on_disconnect = !conn.sessions.contains(2); };
  service.Close(conn);
  return removed_on_disconnect && conn.sessions.empty() && conn.retired_sessions.size() == 2 &&
      second_session->state == router::SessionState::kClosed && second_session->retired_runs.size() == 1 &&
      disconnect_contender->state == router::RunState::kBrokerLost &&
      !disconnect_contender->TryTerminal(router::RunState::kCompleted) && conn.out_queue.empty();
}
std::vector<uint8_t> SizedFrame(size_t size) {
  auto h = ControlHeader(1);
  h.type = protocol::MessageType::ERROR;
  h.payload_length = static_cast<uint32_t>(size) - protocol::kFrameHeaderSize;
  std::vector<uint8_t> bytes;
  protocol::EncodeHeader(h, bytes);
  bytes.resize(size, 0);
  return bytes;
}
bool ControlBudget(std::string *detail) {
  for (bool byte_limit : {false, true}) {
    router::Connection conn;
    conn.writer_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!conn.writer_release || !conn.StartWriter())
      return false;
    bool passed = true;
    if (byte_limit) {
      size_t remaining = protocol::kMaxControlQueueBytes;
      while (remaining) {
        size_t size = (std::min)(remaining, static_cast<size_t>(protocol::kMaxFrameSize));
        passed = passed && conn.Enqueue(SizedFrame(size));
        remaining -= size;
      }
      passed =
          passed && conn.out_control_bytes == protocol::kMaxControlQueueBytes && !conn.Enqueue(std::vector<uint8_t>(1));
    } else {
      for (size_t i = 0; i < protocol::kMaxControlQueueRequests; ++i)
        passed = passed && conn.Enqueue(protocol::BuildAckFrame(ControlHeader(1)));
      passed = passed && conn.out_control_requests == protocol::kMaxControlQueueRequests &&
          !conn.Enqueue(protocol::BuildAckFrame(ControlHeader(1)));
    }
    conn.Stop();
    conn.JoinWriter();
    ::CloseHandle(conn.writer_release);
    conn.writer_release = nullptr;
    if (!passed || conn.writers_started != 1 || conn.writers_joined != 1) {
      *detail = "paused writer control budget boundary";
      return false;
    }
  }

  router::Connection bytes_conn;
  size_t remaining = protocol::kMaxControlQueueBytes - protocol::kFrameHeaderSize;
  while (remaining) {
    size_t size = (std::min)(remaining, static_cast<size_t>(protocol::kMaxFrameSize));
    if (!bytes_conn.Enqueue(SizedFrame(size)))
      return false;
    remaining -= size;
  }
  if (bytes_conn.Enqueue(SizedFrame(protocol::kFrameHeaderSize + 1)) ||
      bytes_conn.out_control_bytes != protocol::kMaxControlQueueBytes - protocol::kFrameHeaderSize) {
    *detail = "valid-frame bytes cap plus one";
    return false;
  }
  router::Connection conn;
  if (!conn.Enqueue(protocol::BuildAckFrame(ControlHeader(1))))
    return false;
  std::vector<uint8_t> frame;
  if (!conn.TakeOutput(&frame) || conn.out_control_requests != 1 || conn.DrainOutput(0))
    return false;
  conn.CompleteOutput(frame.size());
  return conn.out_control_requests == 0 && conn.out_control_bytes == 0 && conn.DrainOutput(0);
}
bool StartAdmissionValidation(std::string *detail) {
  RouterFixture f;
  if (!f.Create())
    return false;
  DrainLocal(f.conn);
  protocol::StartRunPayload start;
  start.guest_payload = {'x'};
  auto unknown = start;
  unknown.tier_override = -1;
  router::BoundProfile valid;
  if (!router::PrepareProfile(LogicalConfig(), unknown, ExecutableDirectory(), &valid)) return false;
  for (uint32_t session : {99, 1}) {
    start.tier_override = session == 99 ? -1 : 2;
    if (!LocalRoute(*f.service, f.conn, protocol::BuildStartRunFrame(ControlHeader(session == 99 ? 2 : 3, session, 1), start)))
      return false;
    protocol::FrameHeader header;
    protocol::StatusCode code;
    if (!PopFrame(f.conn, &header, &code) || header.type != protocol::MessageType::ERROR ||
        code != protocol::StatusCode::ERROR_BAD_STATE || f.spawns || f.conn.sessions.size() != 1 ||
        f.conn.sessions.contains(99) || !f.conn.retired_sessions.empty() || f.conn.sessions.at(1)->profile_bound ||
        f.conn.sessions.at(1)->worker || !f.conn.sessions.at(1)->runs.empty()) {
      *detail = "invalid/unknown START spawned or bound profile";
      return false;
    }
  }
  WireHarness h;
  if (!h.Open(detail) || !h.Send(protocol::BuildCreateSessionFrame(h.Header(1), LogicalConfig())) ||
      !h.Expect(protocol::MessageType::ACK, 1, 1) || !h.Expect(protocol::MessageType::SESSION_READY, 0, 1) ||
      !h.Send(protocol::BuildStartRunFrame(h.Header(2, 99, 1), unknown)) ||
      !h.Expect(protocol::MessageType::ERROR, 2, 99, 1) ||
      !h.Send(protocol::BuildStartRunFrame(h.Header(3, 1, 1), start)) ||
      !h.Expect(protocol::MessageType::ERROR, 3, 1, 1) || !h.Send(protocol::BuildCloseSessionFrame(h.Header(4))) ||
      !h.Expect(protocol::MessageType::ACK, 4, 1))
    return false;
  return h.Finish();
}

bool ConnectionJoined(std::string *detail) {
  router::Connection conn;
  const std::wstring name = L"\\\\.\\pipe\\v8host-join-" + std::to_wstring(::GetCurrentProcessId());
  conn.pipe = ::CreateNamedPipeW(
      name.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1,
      4096,
      4096,
      0,
      nullptr);
  if (conn.pipe == INVALID_HANDLE_VALUE)
    return false;
  HANDLE client = ::CreateFileW(
      name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  OVERLAPPED connect = {};
  connect.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  const bool connected = client != INVALID_HANDLE_VALUE &&
      (::ConnectNamedPipe(conn.pipe, &connect) || ::GetLastError() == ERROR_PIPE_CONNECTED);
  if (connect.hEvent)
    ::CloseHandle(connect.hEvent);
  conn.read_pending = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  conn.write_pending = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool passed = connected && conn.read_pending && conn.write_pending;
  std::thread reader;
  std::atomic<bool> read_done{false};
  if (passed) {
    reader = std::thread([&] {
      std::vector<uint8_t> bytes(protocol::kMaxFrameSize);
      DWORD size = 0;
      read_done = !conn.ReadFrame(bytes.data(), static_cast<DWORD>(bytes.size()), &size, INFINITE);
    });
    passed = conn.StartWriter() && conn.Enqueue(SizedFrame(protocol::kMaxFrameSize)) &&
        ::WaitForSingleObject(conn.read_pending, 3000) == WAIT_OBJECT_0 &&
        ::WaitForSingleObject(conn.write_pending, 3000) == WAIT_OBJECT_0;
  }
  conn.Stop();
  conn.JoinWriter();
  if (reader.joinable())
    reader.join();
  passed = passed && read_done && conn.active_io == 0 && conn.writers_started == 1 && conn.writers_joined == 1;
  if (conn.read_pending)
    ::CloseHandle(conn.read_pending);
  if (conn.write_pending)
    ::CloseHandle(conn.write_pending);
  conn.read_pending = conn.write_pending = nullptr;
  if (client != INVALID_HANDLE_VALUE)
    ::CloseHandle(client);
  ::DisconnectNamedPipe(conn.pipe);
  ::CloseHandle(conn.pipe);
  conn.pipe = INVALID_HANDLE_VALUE;
  if (!passed) {
    *detail = "pending read/write canceled and joined before pipe close";
    return false;
  }
  WireHarness h;
  if (!h.Open(detail))
    return false;
  for (uint32_t id = 1; id <= 4; ++id)
    if (!h.Send(protocol::BuildCreateSessionFrame(h.Header(id, id), LogicalConfig())))
      return false;
  return h.Finish(); // Unread output must not block teardown on a flush.
}
bool PendingAcceptCancellation(std::string *detail) {
  for (bool completed : {false, true}) {
    const std::wstring name = L"\\\\.\\pipe\\v8host-accept-" + std::to_wstring(::GetCurrentProcessId());
    HANDLE pipe = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, nullptr);
    OVERLAPPED pending = {};
    pending.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE client = INVALID_HANDLE_VALUE;
    bool ok = pipe != INVALID_HANDLE_VALUE && pending.hEvent &&
        !::ConnectNamedPipe(pipe, &pending) && ::GetLastError() == ERROR_IO_PENDING;
    if (ok && completed) {
      client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
          FILE_FLAG_OVERLAPPED, nullptr);
      ok = client != INVALID_HANDLE_VALUE && ::WaitForSingleObject(pending.hEvent, 3000) == WAIT_OBJECT_0;
    }
    if (ok) {
      const bool connected = V8HostBrokerFinishPendingConnect(pipe, &pending);
      ok = connected == completed;
      if (ok && connected) {
        std::vector<uint8_t> sent{1, 2, 3, 4}, received(4);
        ok = PipeIo(pipe, true, &sent) && PipeIo(client, false, &received) && received == sent;
      }
    } else if (pipe != INVALID_HANDLE_VALUE && pending.hEvent) {
      ::CancelIoEx(pipe, &pending);
      DWORD bytes;
      ::GetOverlappedResult(pipe, &pending, &bytes, TRUE);
    }
    if (client != INVALID_HANDLE_VALUE)
      ::CloseHandle(client);
    if (pending.hEvent)
      ::CloseHandle(pending.hEvent);
    if (pipe != INVALID_HANDLE_VALUE)
      ::CloseHandle(pipe);
    if (!ok) {
      *detail = completed ? "completed accept was discarded during cancellation drain" : "aborted accept was admitted";
      return false;
    }
  }
  return true;
}
bool DedicatedPreConnectExpiryPolicy(std::string* detail) {
  const std::chrono::steady_clock::time_point start{};
  constexpr auto expiry_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(kDedicatedPreConnectExpiry).count();
  for (auto ms : {decltype(expiry_ms){0}, expiry_ms - 1, expiry_ms, expiry_ms + 1}) {
    const auto now = start + std::chrono::milliseconds(ms);
    if (V8HostBrokerPreConnectExpired(true, false, start, now) != (ms >= expiry_ms) ||
        V8HostBrokerPreConnectExpired(false, false, start, now) ||
        V8HostBrokerPreConnectExpired(true, true, start, now) ||
        V8HostBrokerPreConnectExpired(false, true, start, now)) {
      *detail = "dedicated pre-connect expiry boundary or mode/accept guard";
      return false;
    }
  }
  // Pair an expired policy with both completed and uncompleted local accepts.
  return V8HostBrokerPreConnectExpired(true, false, start,
      start + kDedicatedPreConnectExpiry) && PendingAcceptCancellation(detail);
}

bool PendingAcceptPoll(std::string *detail) {
  const std::wstring name = L"\\\\.\\pipe\\v8host-accept-poll-" + std::to_wstring(::GetCurrentProcessId());
  HANDLE pipe = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1, 4096, 4096, 0, nullptr);
  OVERLAPPED pending = {};
  pending.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  bool active = pipe != INVALID_HANDLE_VALUE && pending.hEvent &&
      !::ConnectNamedPipe(pipe, &pending) && ::GetLastError() == ERROR_IO_PENDING;
  bool ok = active;
  // A client can open even after a cancelled accept completes, before pipe close.
  // Ordinary idle polls must leave the original accept pending for that client.
  for (int i = 0; ok && i < 3; ++i)
    ok = V8HostBrokerPollConnect(pipe, &pending, 0) == V8HostBrokerAcceptStatus::kPending;
  HANDLE client = INVALID_HANDLE_VALUE;
  if (ok) {
    client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr);
    ok = client != INVALID_HANDLE_VALUE &&
        V8HostBrokerPollConnect(pipe, &pending, 3000) == V8HostBrokerAcceptStatus::kConnected;
    if (ok) {
      active = false;
      std::vector<uint8_t> sent = protocol::BuildHelloFrame(protocol::FrameHeader{}), received(512);
      ok = PipeIo(client, true, &sent) && PipeIo(pipe, false, &received) && received == sent;
    }
  }
  if (active)
    V8HostBrokerFinishPendingConnect(pipe, &pending);
  if (client != INVALID_HANDLE_VALUE) ::CloseHandle(client);
  if (pending.hEvent) ::CloseHandle(pending.hEvent);
  if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(pipe);
  if (!ok) *detail = "idle poll discarded the accept before a late client HELLO";
  return ok;
}

bool SharedFanIn(std::string* detail) {
  constexpr size_t kClients = 16;
  std::array<DWORD, kClients> pids = {};
  std::array<uint32_t, kClients> conn_ids = {};
  std::array<bool, kClients> passed = {};
  std::array<std::string, kClients> details;
  std::vector<std::thread> threads;
  for (size_t i = 0; i < kClients; ++i) {
    threads.emplace_back([&, i] {
      BrokerConnection connection;
      HelloResult result;
      passed[i] = ConnectAndHandshake(BrokerMode::kShared,
                                      static_cast<uint32_t>(i + 1), &connection,
                                      &result, &details[i]);
      pids[i] = connection.broker_pid();
      conn_ids[i] = result.conn_id;
    });
  }
  for (std::thread& thread : threads)
    thread.join();
  if (!std::all_of(passed.begin(), passed.end(), [](bool value) {
        return value;
      })) {
    for (size_t i = 0; i < kClients; ++i) {
      if (!passed[i]) {
        *detail = "client " + std::to_string(i) + ": " + details[i];
        break;
      }
    }
    return false;
  }
  std::set<DWORD> unique(pids.begin(), pids.end());
  if (unique.size() != 1 || *unique.begin() == 0) {
    *detail = "clients observed multiple broker PIDs";
    return false;
  }
  // The single shared broker assigns conn_ids monotonically, so every client
  // must have received a distinct one.
  std::set<uint32_t> unique_conn_ids(conn_ids.begin(), conn_ids.end());
  if (unique_conn_ids.size() != kClients) {
    *detail = "broker reused a connection id across fan-in clients";
    return false;
  }
  printf("endpoint fan-in broker pid=%lu clients=%zu\n", *unique.begin(),
         kClients);
  return true;
}

bool ExactPayloadSeparation(std::string* detail) {
  std::vector<uint8_t> sid;
  LUID session = {};
  DWORD error = ERROR_SUCCESS;
  if (!v8host::QueryCurrentSidAndSession(&sid, &session, &error))
    return false;
  std::array<uint8_t, 32> first = {};
  std::array<uint8_t, 32> second = {};
  second[0] = 1;
  std::wstring first_endpoint;
  std::wstring second_endpoint;
  std::array<uint8_t, 32> first_key = {};
  std::array<uint8_t, 32> second_key = {};
  const bool ok =
      v8host::DeriveEndpoint(sid, first, L"C:\\payload", BrokerMode::kShared, nullptr,
                             &first_endpoint, &first_key) &&
      v8host::DeriveEndpoint(sid, second, L"C:\\payload", BrokerMode::kShared, nullptr,
                             &second_endpoint, &second_key) &&
      first_endpoint != second_endpoint;
  if (!ok)
    *detail = "alternate payload digest did not separate endpoint";
  return ok;
}

bool DedicatedUnique(std::string* detail) {
  BrokerConnection first;
  BrokerConnection second;
  HelloResult first_result;
  HelloResult second_result;
  if (!ConnectAndHandshake(BrokerMode::kDedicated, 101, &first, &first_result,
                           detail) ||
      !ConnectAndHandshake(BrokerMode::kDedicated, 102, &second, &second_result,
                           detail))
    return false;
  if (first.endpoint() == second.endpoint() ||
      first.broker_pid() == second.broker_pid()) {
    *detail = "dedicated endpoint or PID reused";
    return false;
  }

  return true;
}

bool InstallRootDerivation(std::string* detail) {
    const std::vector<uint8_t> sid{1, 2, 3};
    const std::array<uint8_t, 32> plugin = {};
    const std::array<uint8_t, 16> nonce = {};
    std::wstring endpoint, other;
    std::array<uint8_t, 32> key = {}, other_key = {};
    for (BrokerMode mode : {BrokerMode::kShared, BrokerMode::kDedicated}) {
      if (!v8host::DeriveEndpoint(sid, plugin, L"C:\\payload", mode, &nonce, &endpoint, &key) ||
          !v8host::DeriveEndpoint(sid, plugin, L"C:\\other", mode, &nonce, &other, &other_key) ||
          endpoint == other || key == other_key) {
        *detail = "install roots did not separate endpoints and keys"; return false;
      }
      // Independently computed with Node crypto, including the domain's NUL.
      if (mode == BrokerMode::kShared &&
          v8host::HexPrefix(key, 32) != "68357446964eaa6defb02bfacd8fd090dbc5f7963529212a6005bf5a3df37a75") {
        *detail = "V2 fixed vector differs"; return false;
      }
      for (const wchar_t* root : {L"c:\\PaYlOaD\\", L"\\\\?\\C:\\PAYLOAD", L"C:/payload///"}) {
        if (!v8host::DeriveEndpoint(sid, plugin, root, mode, &nonce, &other, &other_key) ||
            endpoint != other || key != other_key) {
          *detail = "root case/separator/prefix normalization differs"; return false;
        }
      }
      for (const wchar_t* root : {L"", L"\\", L"///"})
        if (v8host::DeriveEndpoint(sid, plugin, root, mode, &nonce, &other, &other_key)) {
          *detail = "empty root accepted"; return false;
        }
      for (const auto& roots : {
          std::pair{L"\\\\?\\UNC\\server\\share\\payload", L"\\\\SERVER\\SHARE\\PAYLOAD\\"},
          std::pair{L"\\\\?\\Volume{abc}\\payload", L"\\\\?\\VOLUME{ABC}\\PAYLOAD\\"}}) {
        if (!v8host::DeriveEndpoint(sid, plugin, roots.first, mode, &nonce, &endpoint, &key) ||
            !v8host::DeriveEndpoint(sid, plugin, roots.second, mode, &nonce, &other, &other_key) ||
            endpoint != other || key != other_key) {
          *detail = "UNC/volume-GUID normalization differs"; return false;
        }
      }
    }
    return true;
}

bool MixedMode(std::string* detail) {
  BrokerConnection shared;
  BrokerConnection first;
  BrokerConnection second;
  HelloResult shared_result;
  HelloResult first_result;
  HelloResult second_result;
  if (!ConnectAndHandshake(BrokerMode::kShared, 201, &shared, &shared_result,
                           detail) ||
      !ConnectAndHandshake(BrokerMode::kDedicated, 202, &first, &first_result,
                           detail) ||
      !ConnectAndHandshake(BrokerMode::kDedicated, 203, &second, &second_result,
                           detail))
    return false;
  std::set<DWORD> pids = {shared.broker_pid(), first.broker_pid(),
                          second.broker_pid()};
  if (pids.size() != 3) {
    *detail = "mixed brokers did not isolate PIDs";
    return false;
  }
  return true;
}

bool IdleExitRelaunch(std::string* detail) {
  BrokerConnection connection;
  HelloResult first;
  if (!ConnectAndHandshake(BrokerMode::kShared, 301, &connection, &first,
                           detail))
    return false;
  const DWORD first_pid = connection.broker_pid();
  connection.Close();
  const ULONGLONG start = ::GetTickCount64();
  if (!v8host::test::WaitForProcessExit(first_pid, 8000) ||
      ::GetTickCount64() - start < 4900) {
    *detail = "broker did not honor five-second idle grace";
    return false;
  }
  BrokerConnection replacement;
  HelloResult second;
  if (!ConnectAndHandshake(BrokerMode::kShared, 302, &replacement, &second,
                           detail))
    return false;
  if (first_pid == replacement.broker_pid()) {
    *detail = "broker PID did not change after relaunch";
    return false;
  }
  return true;
}

bool GraceCancel(std::string* detail) {
  BrokerConnection first_connection;
  HelloResult first;
  if (!ConnectAndHandshake(BrokerMode::kShared, 401, &first_connection, &first,
                           detail))
    return false;
  const DWORD first_pid = first_connection.broker_pid();
  first_connection.Close();
  ::Sleep(1500);
  BrokerConnection second_connection;
  HelloResult second;
  if (!ConnectAndHandshake(BrokerMode::kShared, 402, &second_connection, &second,
                           detail))
    return false;
  if (first_pid != second_connection.broker_pid()) {
    *detail = "connection during grace did not preserve broker";
    return false;
  }
  return true;
}

struct SquatterState {
  std::wstring endpoint;
  HANDLE ready = nullptr;
  std::atomic<DWORD> bytes{0};
};

void Squatter(SquatterState* state) {
  HANDLE pipe = ::CreateNamedPipeW(
      state->endpoint.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      1, 256, 256, 0, nullptr);
  ::SetEvent(state->ready);
  if (pipe == INVALID_HANDLE_VALUE)
    return;
  if (::ConnectNamedPipe(pipe, nullptr) ||
      ::GetLastError() == ERROR_PIPE_CONNECTED) {
    char byte = 0;
    DWORD read = 0;
    if (::ReadFile(pipe, &byte, 1, &read, nullptr))
      state->bytes = read;
  }
  ::CloseHandle(pipe);
}

bool EndpointSquatter(std::string* detail) {
  BrokerRendezvous rendezvous(ExecutableDirectory(), BrokerMode::kShared);
  BrokerConnection warmup;
  const RendezvousStatus init = rendezvous.ConnectOrLaunch(&warmup);
  if (init != RendezvousStatus::kOk)
    return false;
  HelloResult info;
  if (warmup.Handshake(501, &info) != RendezvousStatus::kOk)
    return false;
  const DWORD broker_pid = warmup.broker_pid();
  warmup.Close();
  if (!v8host::test::WaitForProcessExit(broker_pid, 8000))
    return false;

  SquatterState state;
  state.endpoint = rendezvous.endpoint();
  state.ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::thread squatter(Squatter, &state);
  ::WaitForSingleObject(state.ready, 2000);
  BrokerConnection connection;
  const RendezvousStatus status = rendezvous.ConnectOrLaunch(&connection);
  if (connection.pipe() != INVALID_HANDLE_VALUE)
    connection.Close();
  squatter.join();
  ::CloseHandle(state.ready);
  const bool ok = status == RendezvousStatus::kPeerAuthenticationFailed &&
                  state.bytes.load() == 0;
  if (!ok)
    *detail = "squatter was not rejected before client write";
  return ok;
}

bool ImmutableWrite(std::string* detail) {
  v8host::HeldFile held;
  DWORD error = ERROR_SUCCESS;
  const std::wstring path = ExecutableDirectory() + L"\\sbox.exe";
  if (!v8host::OpenImmutableFile(path, &held, &error))
    return false;
  HANDLE writer =
      ::CreateFileW(path.c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (writer != INVALID_HANDLE_VALUE) {
    ::CloseHandle(writer);
    *detail = "write sharing succeeded while immutable handle held";
    return false;
  }
  HANDLE deleter =
      ::CreateFileW(path.c_str(), DELETE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (deleter != INVALID_HANDLE_VALUE) {
    ::CloseHandle(deleter);
    *detail = "delete sharing succeeded while immutable handle held";
    return false;
  }
  return true;
}

bool MutablePreexisting(std::string* detail) {
  const std::wstring path = ExecutableDirectory() + L"\\sbox.exe";
  HANDLE writer =
      ::CreateFileW(path.c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (writer == INVALID_HANDLE_VALUE) {
    *detail = "could not establish mutable preexisting handle";
    return false;
  }
  v8host::HeldFile held;
  DWORD error = ERROR_SUCCESS;
  const bool rejected = !v8host::OpenImmutableFile(path, &held, &error);
  ::CloseHandle(writer);
  if (!rejected)
    *detail = "immutable open accepted mutable preexisting sharing";
  return rejected;
}

bool PathPolicy(std::string* detail) {
  const bool ok =
      v8host::IsStrictDescendant(L"\\\\?\\Volume{a}\\root",
                                 L"\\\\?\\Volume{a}\\root\\client.exe") &&
      !v8host::IsStrictDescendant(L"\\\\?\\Volume{a}\\root",
                                  L"\\\\?\\Volume{a}\\root2\\client.exe") &&
      !v8host::IsStrictDescendant(L"\\\\?\\Volume{a}\\root",
                                  L"\\\\?\\Volume{b}\\root\\client.exe");
  if (!ok)
    *detail = "component-aware canonical root policy failed";
  return ok;
}

bool DedicatedOwnerExit(std::string* detail) {
  BrokerConnection connection;
  HelloResult result;
  if (!ConnectAndHandshake(BrokerMode::kDedicated, 601, &connection, &result,
                           detail))
    return false;
  const DWORD broker_pid = connection.broker_pid();
  HANDLE broker = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, broker_pid);
  // A dedicated broker leaves its accept loop after the first accept, so the
  // pre-connect expiry can't apply here; this only checks a live owner and a
  // prompt Close.
  const bool alive = broker && ::WaitForSingleObject(broker, 500) == WAIT_TIMEOUT;
  if (broker) ::CloseHandle(broker);
  connection.Close();
  if (!alive) {
    *detail = "connected dedicated broker expired before owner Close";
    return false;
  }
  if (!v8host::test::WaitForProcessExit(broker_pid, 3000)) {
    *detail = "dedicated broker did not join and exit";
    return false;
  }
  return true;
}

bool DisconnectRaces(std::string* detail) {
  for (uint32_t i = 0; i < 64; ++i) {
    BrokerConnection connection;
    HelloResult result;
    if (!ConnectAndHandshake(BrokerMode::kShared, 700 + i, &connection, &result,
                             detail))
      return false;
    connection.Close();
  }
  return true;
}

bool NoGenericClientRole(std::string* detail) {
  DWORD exit_code = 0;
  const std::wstring command =
      L"\"" + ExecutableDirectory() + L"\\sbox.exe\" --client";
  if (!v8host::test::RunProcess(command, &exit_code) || exit_code != 2) {
    *detail = "generic host accepted removed client role";
    return false;
  }
  return true;
}

bool TrustPolicy(std::string* detail) {
  const bool ok = v8host::TrustAllowed(v8host::TrustStatus::kUnsigned) &&
                  !v8host::TrustAllowed(v8host::TrustStatus::kInvalid) &&
                  !v8host::TrustAllowed(v8host::TrustStatus::kError);
  if (!ok)
    *detail = "development trust policy accepted invalid signature";
  return ok;
}

// ===== peer authentication unit coverage =====
//
// A connected message-mode pipe pair in this process makes the current
// (same-user, trusted, installed) process the authenticated peer. Negative
// cases mutate one policy field, or inject scripted identity facts via
// PeerAuthHooks for the fail-closed paths (PID reread, per-step query failure)
// that a stable local pipe cannot reproduce.
struct SelfPeer {
  HANDLE server = INVALID_HANDLE_VALUE;
  HANDLE client = INVALID_HANDLE_VALUE;
  v8host::HeldFile image;
  std::vector<uint8_t> sid;
  LUID session = {};
  ~SelfPeer() {
    if (client != INVALID_HANDLE_VALUE)
      ::CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE)
      ::CloseHandle(server);
  }
};

bool MakeSelfPeer(SelfPeer* peer) {
  const std::wstring name = L"\\\\.\\pipe\\v8host-authtest-" +
                            std::to_wstring(::GetCurrentProcessId()) + L"-" +
                            std::to_wstring(::GetTickCount64());
  peer->server = ::CreateNamedPipeW(
      name.c_str(), PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 256, 256, 0,
      nullptr);
  if (peer->server == INVALID_HANDLE_VALUE)
    return false;
  peer->client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
  if (peer->client == INVALID_HANDLE_VALUE)
    return false;
  ::ConnectNamedPipe(peer->server, nullptr);
  std::wstring self(32768, L'\0');
  const DWORD n = ::GetModuleFileNameW(nullptr, self.data(),
                                       static_cast<DWORD>(self.size()));
  self.resize(n);
  DWORD error = 0;
  return v8host::OpenImmutableFile(self, &peer->image, &error) &&
         v8host::QueryCurrentSidAndSession(&peer->sid, &peer->session, &error);
}

v8host::PeerAuthPolicy BasePolicy(const SelfPeer& peer, v8host::BrokerMode mode) {
  v8host::PeerAuthPolicy policy = {};
  policy.peer_side = v8host::PeerSide::kPipeClient;
  policy.mode = mode;
  policy.expected_sid = peer.sid;
  policy.expected_session = peer.session;
  policy.install_root = v8host::ParentPath(peer.image.final_path());
  policy.expected_image = &peer.image;
  return policy;
}

bool Authenticates(const SelfPeer& peer,
                   const v8host::PeerAuthPolicy& policy,
                   const v8host::PeerAuthHooks* hooks = nullptr,
                   DWORD* error_out = nullptr) {
  v8host::HeldProcess out;
  DWORD error = ERROR_SUCCESS;
  const bool ok =
      v8host::AuthenticatePipePeer(peer.server, policy, &out, &error, hooks);
  if (error_out)
    *error_out = error;
  return ok;
}

// A rejection must report the code of the check that failed
// (v8host_peer_auth.h), so a broker log names it.
bool RejectedWith(const SelfPeer& peer,
                  const v8host::PeerAuthPolicy& policy,
                  DWORD expected,
                  const char* what,
                  std::string* detail,
                  const v8host::PeerAuthHooks* hooks = nullptr) {
  DWORD error = ERROR_SUCCESS;
  if (Authenticates(peer, policy, hooks, &error)) {
    *detail = std::string(what) + " was accepted";
    return false;
  }
  if (error != expected) {
    char text[96];
    std::snprintf(text, sizeof(text), " rejected with 0x%08lx, expected 0x%08lx",
                  error, expected);
    *detail = std::string(what) + text;
    return false;
  }
  return true;
}

bool AuthSameUserPositive(std::string* detail) {
  SelfPeer peer;
  if (!MakeSelfPeer(&peer)) {
    *detail = "pipe/identity setup failed";
    return false;
  }
  DWORD error = ERROR_SUCCESS;
  if (!Authenticates(peer, BasePolicy(peer, v8host::BrokerMode::kShared),
                     nullptr, &error)) {
    *detail = "correct same-user co-located policy was rejected, error " +
              std::to_string(error);
    return false;
  }
  return true;
}

bool AuthCrossUserReject(std::string* detail) {
  SelfPeer peer;
  if (!MakeSelfPeer(&peer)) {
    *detail = "setup failed";
    return false;
  }
  std::vector<uint8_t> other(SECURITY_MAX_SID_SIZE);
  DWORD size = SECURITY_MAX_SID_SIZE;
  if (!::CreateWellKnownSid(WinWorldSid, nullptr, other.data(), &size)) {
    *detail = "could not build alternate SID";
    return false;
  }
  other.resize(size);
  v8host::PeerAuthPolicy policy = BasePolicy(peer, v8host::BrokerMode::kShared);
  policy.expected_sid = other;
  return RejectedWith(peer, policy, ERROR_INVALID_OWNER, "different-user SID",
                      detail);
}

bool AuthWrongSessionReject(std::string* detail) {
  SelfPeer peer;
  if (!MakeSelfPeer(&peer)) {
    *detail = "setup failed";
    return false;
  }
  v8host::PeerAuthPolicy policy = BasePolicy(peer, v8host::BrokerMode::kShared);
  policy.expected_session.LowPart ^= 0x5a5a5a5a;
  return RejectedWith(peer, policy, ERROR_NO_SUCH_LOGON_SESSION,
                      "different logon session", detail);
}

bool AuthWrongPayloadImageReject(std::string* detail) {
  SelfPeer peer;
  if (!MakeSelfPeer(&peer)) {
    *detail = "setup failed";
    return false;
  }
  v8host::HeldFile other;
  DWORD error = 0;
  if (!v8host::OpenImmutableFile(ExecutableDirectory() + L"\\v8host.dll", &other,
                                 &error)) {
    *detail = "could not open alternate image";
    return false;
  }
  v8host::PeerAuthPolicy policy = BasePolicy(peer, v8host::BrokerMode::kShared);
  policy.expected_image = &other;
  return RejectedWith(peer, policy, ERROR_NOT_SAME_OBJECT,
                      "wrong expected payload image", detail);
}

bool AuthNonColocatedReject(std::string* detail) {
  SelfPeer peer;
  if (!MakeSelfPeer(&peer)) {
    *detail = "setup failed";
    return false;
  }
  v8host::PeerAuthPolicy policy = BasePolicy(peer, v8host::BrokerMode::kShared);
  policy.install_root =
      L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\elsewhere";
  return RejectedWith(peer, policy, ERROR_PATH_NOT_FOUND,
                      "non-co-located client in shared mode", detail);
}

bool AuthSiblingPrefixReject(std::string* detail) {
  SelfPeer peer;
  if (!MakeSelfPeer(&peer)) {
    *detail = "setup failed";
    return false;
  }
  v8host::PeerAuthPolicy policy = BasePolicy(peer, v8host::BrokerMode::kShared);
  policy.install_root = v8host::ParentPath(peer.image.final_path()) + L"2";
  return RejectedWith(peer, policy, ERROR_PATH_NOT_FOUND,
                      "sibling-prefix root as ancestor", detail);
}

bool AuthDedicatedNonColocatedPositive(std::string* detail) {
  SelfPeer peer;
  if (!MakeSelfPeer(&peer)) {
    *detail = "setup failed";
    return false;
  }
  v8host::PeerAuthPolicy policy =
      BasePolicy(peer, v8host::BrokerMode::kDedicated);
  policy.install_root =
      L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\elsewhere";
  if (!Authenticates(peer, policy)) {
    *detail = "dedicated mode rejected a same-user non-co-located peer";
    return false;
  }
  return true;
}

bool AuthLaunchMismatchReject(std::string* detail) {
  SelfPeer peer;
  if (!MakeSelfPeer(&peer)) {
    *detail = "setup failed";
    return false;
  }
  std::wstring command =
      L"\"" + ExecutableDirectory() + L"\\sbox.exe\" --client";
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process)) {
    *detail = "could not launch mismatch helper";
    return false;
  }
  ::CloseHandle(process.hThread);
  v8host::PeerAuthPolicy policy = BasePolicy(peer, v8host::BrokerMode::kShared);
  policy.launched_process = process.hProcess;
  const bool rejected = RejectedWith(peer, policy, ERROR_PROCESS_ABORTED,
                                     "launch-instance mismatch", detail);
  ::WaitForSingleObject(process.hProcess, 5000);
  ::CloseHandle(process.hProcess);
  return rejected;
}

bool AuthPidRereadMismatchReject(std::string* detail) {
  SelfPeer peer;
  if (!MakeSelfPeer(&peer)) {
    *detail = "setup failed";
    return false;
  }
  const DWORD real = ::GetCurrentProcessId();
  auto calls = std::make_shared<int>(0);
  v8host::PeerAuthHooks hooks;
  hooks.read_peer_pid = [real, calls](HANDLE, v8host::PeerSide, DWORD* pid) {
    *pid = (++*calls == 1) ? real : real + 1;
    return true;
  };
  return RejectedWith(peer, BasePolicy(peer, v8host::BrokerMode::kShared),
                      ERROR_BAD_PIPE, "peer PID reread mismatch", detail,
                      &hooks);
}

bool AuthQueryFailureReject(std::string* detail) {
  SelfPeer peer;
  if (!MakeSelfPeer(&peer)) {
    *detail = "setup failed";
    return false;
  }
  v8host::PeerAuthHooks hooks;
  // The query's own OS error is reported as is.
  hooks.query_identity = [](DWORD, v8host::HeldProcess*, DWORD* error) {
    if (error)
      *error = ERROR_INVALID_HANDLE;
    return false;
  };
  const v8host::PeerAuthPolicy policy =
      BasePolicy(peer, v8host::BrokerMode::kShared);
  if (!RejectedWith(peer, policy, ERROR_INVALID_HANDLE,
                    "identity query failure", detail, &hooks))
    return false;
  hooks.query_identity = [](DWORD, v8host::HeldProcess*, DWORD*) {
    return false;
  };
  return RejectedWith(peer, policy, ERROR_ACCESS_DENIED,
                      "identity query failure without an error", detail,
                      &hooks);
}

bool AuthUntrustedImageReject(std::string* detail) {
  SelfPeer peer;
  if (!MakeSelfPeer(&peer)) {
    *detail = "setup failed";
    return false;
  }
  const v8host::PeerAuthPolicy policy =
      BasePolicy(peer, v8host::BrokerMode::kShared);
  v8host::PeerAuthHooks hooks;
  hooks.verify_trust = [](const v8host::HeldFile&) {
    return v8host::TrustStatus::kInvalid;
  };
  if (!RejectedWith(peer, policy, static_cast<DWORD>(TRUST_E_SUBJECT_NOT_TRUSTED),
                    "invalid peer signature", detail, &hooks))
    return false;
  hooks.verify_trust = [](const v8host::HeldFile&) {
    return v8host::TrustStatus::kError;
  };
  if (!RejectedWith(peer, policy, static_cast<DWORD>(TRUST_E_SYSTEM_ERROR),
                    "unverifiable peer image", detail, &hooks))
    return false;
  // An unsigned peer: accepted only by the development trust policy; the
  // release policy (SBOX_DEV_ALLOW_UNSIGNED compiled out) rejects it.
  hooks.verify_trust = [](const v8host::HeldFile&) {
    return v8host::TrustStatus::kUnsigned;
  };
  if (v8host::TrustAllowed(v8host::TrustStatus::kUnsigned)) {
    if (!Authenticates(peer, policy, &hooks)) {
      *detail = "development trust policy rejected an unsigned peer";
      return false;
    }
    return true;
  }
  return RejectedWith(peer, policy, static_cast<DWORD>(TRUST_E_NOSIGNATURE),
                      "unsigned peer", detail, &hooks);
}

bool AuthPidReadFailureReject(std::string* detail) {
  SelfPeer peer;
  if (!MakeSelfPeer(&peer)) {
    *detail = "setup failed";
    return false;
  }
  v8host::PeerAuthHooks hooks;
  hooks.read_peer_pid = [](HANDLE, v8host::PeerSide, DWORD*) { return false; };
  return RejectedWith(peer, BasePolicy(peer, v8host::BrokerMode::kShared),
                      ERROR_PIPE_NOT_CONNECTED, "peer PID read failure", detail,
                      &hooks);
}

bool AuthInvalidPipeReject(std::string* detail) {
  v8host::PeerAuthPolicy policy = {};
  policy.peer_side = v8host::PeerSide::kPipeClient;
  policy.mode = v8host::BrokerMode::kShared;
  v8host::HeldProcess out;
  DWORD error = ERROR_SUCCESS;
  if (v8host::AuthenticatePipePeer(INVALID_HANDLE_VALUE, policy, &out, &error)) {
    *detail = "invalid pipe handle authenticated";
    return false;
  }
  if (error != ERROR_INVALID_PARAMETER) {
    *detail = "invalid pipe handle rejected with error " + std::to_string(error);
    return false;
  }
  return true;
}

// ===== generic-host sbox_broker_api ownership (driven in-broker) =====
//
// Launches the real sbox.exe broker and selects the plugin's dev-only
// sbox_broker_api self-test via an environment variable (no new export). The
// broker exercises argument rejection and the spawn/close/wait ownership cycle
// in-process; "leak" abandons a worker so the generic host's post-run cleanup
// must reclaim it and report the breach (exit 4).
bool RunBrokerApiMode(const wchar_t* mode, DWORD* exit_code) {
  ::SetEnvironmentVariableW(L"V8HOST_BROKER_API_TEST", mode);
  const std::wstring pipe =
      L"\\\\.\\pipe\\v8host-rv2-" + std::wstring(64, L'0');
  const std::wstring command = L"\"" + ExecutableDirectory() +
                               L"\\sbox.exe\" --broker --mode=shared --pipe=" +
                               pipe + L" --plugin v8host.dll";
  const bool ok = v8host::test::RunProcess(command, exit_code);
  ::SetEnvironmentVariableW(L"V8HOST_BROKER_API_TEST", nullptr);
  return ok;
}

bool BrokerApiProcessId(std::string* detail) {
  DWORD code = 0xFFFFFFFF;
  if (!RunBrokerApiMode(L"process-id", &code) || code != 0) {
    *detail = "broker process ID checks failed, exit=" + std::to_string(code);
    return false;
  }
  return true;
}

bool BrokerApiOwnership(std::string* detail) {
  DWORD code = 0xFFFFFFFF;
  if (!RunBrokerApiMode(L"ownership", &code)) {
    *detail = "ownership broker launch failed";
    return false;
  }
  if (code != 0) {
    *detail = "sbox_broker_api ownership checks failed, exit=" +
              std::to_string(code);
    return false;
  }
  return true;
}

bool BrokerApiLeakedCleanup(std::string* detail) {
  DWORD code = 0xFFFFFFFF;
  if (!RunBrokerApiMode(L"leak", &code)) {
    *detail = "leak broker launch failed";
    return false;
  }
  if (code != 4) {
    *detail = "host did not reclaim leaked worker (expected exit 4), exit=" +
              std::to_string(code);
    return false;
  }
  return true;
}

// Post-verification rename and in-place replacement of a held image must fail
// for the handle's whole lifetime (staged on a throwaway copy, not a live
// binary). Open-for-write and open-for-delete denial are covered by
// immutable-write; this adds the rename and replace mutation variants.
bool ImmutableMutation(std::string* detail) {
  wchar_t temp_dir[MAX_PATH] = {};
  if (!::GetTempPathW(MAX_PATH, temp_dir)) {
    *detail = "temp path query failed";
    return false;
  }
  const std::wstring held_path = std::wstring(temp_dir) + L"v8host-immutable-" +
                                 std::to_wstring(::GetCurrentProcessId()) +
                                 L".bin";
  const std::wstring rename_target = held_path + L".moved";
  if (!::CopyFileW((ExecutableDirectory() + L"\\sbox.exe").c_str(),
                   held_path.c_str(), FALSE)) {
    *detail = "could not stage held file copy";
    return false;
  }
  bool ok = true;
  {
    v8host::HeldFile held;
    DWORD error = 0;
    if (!v8host::OpenImmutableFile(held_path, &held, &error)) {
      ::DeleteFileW(held_path.c_str());
      *detail = "could not open staged copy immutably";
      return false;
    }
    if (::MoveFileExW(held_path.c_str(), rename_target.c_str(),
                      MOVEFILE_REPLACE_EXISTING)) {
      ::MoveFileExW(rename_target.c_str(), held_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING);
      ok = false;
      *detail = "rename succeeded while immutable handle held";
    }
    if (ok) {
      HANDLE replace = ::CreateFileW(
          held_path.c_str(), GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (replace != INVALID_HANDLE_VALUE) {
        ::CloseHandle(replace);
        ok = false;
        *detail = "in-place replace succeeded while immutable handle held";
      }
    }
  }
  ::DeleteFileW(held_path.c_str());
  ::DeleteFileW(rename_target.c_str());
  return ok;
}

// The generic host must reject a resolvable plugin whose vtable reports the
// wrong ABI version / null entries before driving any lifecycle (exit 3).
bool HostRejectsBadPluginAbi(std::string* detail) {
  const std::wstring pipe =
      L"\\\\.\\pipe\\v8host-rv2-" + std::wstring(64, L'0');
  const std::wstring command = L"\"" + ExecutableDirectory() +
                               L"\\sbox.exe\" --broker --mode=shared --pipe=" +
                               pipe + L" --plugin v8host_fake_plugin.dll";
  DWORD code = 0;
  if (!v8host::test::RunProcess(command, &code)) {
    *detail = "bad-plugin broker launch failed";
    return false;
  }
  if (code != 3) {
    *detail =
        "host did not reject invalid plugin ABI (expected exit 3), exit=" +
        std::to_string(code);
    return false;
  }
  return true;
}


#define TOPOLOGY_CLIENT_ENTRIES(X) \
  X(broker_open) X(broker_close) X(broker_config_set_mode) X(broker_config_set_callbacks) \
  X(worker_open) X(worker_close) X(worker_get_id) X(worker_get_process_id) \
  X(worker_config_set_tier) X(worker_config_set_max_runtimes) X(worker_config_set_flags) \
  X(worker_config_set_initial_token_level) X(worker_config_set_lockdown_token_level) \
  X(worker_config_set_integrity_level) X(worker_config_set_delayed_integrity_level) \
  X(worker_config_set_use_app_container) X(worker_config_set_low_privilege_app_container) \
  X(worker_config_set_profile_name_utf16) X(worker_config_set_engine_dll_utf16) \
  X(worker_config_add_startup_snapshot_path_utf16) \
  X(runtime_open) X(runtime_close) X(runtime_get_id) X(runtime_start_run) \
  X(runtime_config_set_startup_snapshot_index)
struct ContractCClient {
#define TOPOLOGY_MEMBER(name) v8host_##name##_fn name = nullptr;
  TOPOLOGY_CLIENT_ENTRIES(TOPOLOGY_MEMBER)
#undef TOPOLOGY_MEMBER
  HMODULE module = nullptr;
  decltype(&v8host_initialize) initialize = nullptr;
  decltype(&v8host_session_open) create = nullptr;
  decltype(&v8host_session_set_callbacks) callbacks = nullptr;
  decltype(&v8host_session_start_run) start = nullptr;
  decltype(&v8host_run_post_message) post = nullptr;
  decltype(&v8host_run_cancel) cancel = nullptr;
  decltype(&v8host_session_close) close = nullptr;
  v8host::test::ConfigApi config_api;
  bool Load() {
    if (module) return true;
    const std::wstring path = ExecutableDirectory() + L"/v8host.dll";
    module = ::LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) return false;
#define CLIENT_ENTRY(member, symbol) member = reinterpret_cast<decltype(member)>(::GetProcAddress(module, #symbol)); if (!member) return false
    CLIENT_ENTRY(initialize, v8host_initialize);
    CLIENT_ENTRY(create, v8host_session_open);
    CLIENT_ENTRY(callbacks, v8host_session_set_callbacks);
    CLIENT_ENTRY(start, v8host_session_start_run);
    CLIENT_ENTRY(post, v8host_run_post_message);
    CLIENT_ENTRY(cancel, v8host_run_cancel);
    CLIENT_ENTRY(close, v8host_session_close);
#define TOPOLOGY_ENTRY(name) CLIENT_ENTRY(name, v8host_##name);
    TOPOLOGY_CLIENT_ENTRIES(TOPOLOGY_ENTRY)
#undef TOPOLOGY_ENTRY
#define CLIENT_SETTER(name) CLIENT_ENTRY(config_api.name, v8host_##name);
    V8HOST_TEST_CONFIG_SETTERS(CLIENT_SETTER)
#undef CLIENT_SETTER
#undef CLIENT_ENTRY
    return initialize(V8HOST_ABI_VERSION) == v8host_status_ok;
  }
  // create / start through the DLL's own builder setters.
  v8host_status Open(const v8host::test::TestSessionConfig* config, v8host_session* out) const {
    v8host::test::SessionConfigure args{&config_api, config};
    return create(&v8host::test::ConfigureSession, &args, out);
  }
  v8host_status Start(v8host_session session, const v8host::test::TestRunInputs* inputs, v8host_run* out) const {
    v8host::test::RunConfigure args{&config_api, inputs};
    return start(session, &v8host::test::ConfigureRun, &args, out);
  }
};
#undef TOPOLOGY_CLIENT_ENTRIES
ContractCClient& PublicClient() {
  // The module owns app-thread windows and stays loaded until process exit.
  static ContractCClient client;
  return client;
}
std::vector<ULONGLONG> g_startup_ms;  // child-process samples for the timing case
struct PublicCapture {
  ContractCClient* client = nullptr;
  v8host_session_s* session = nullptr;
  v8host_run_s* run = nullptr;
  DWORD thread = ::GetCurrentThreadId();
  std::vector<int32_t> states, events, kinds;
  std::vector<v8host_status> statuses;
  std::vector<std::vector<uint8_t>> messages;
  int terminal = 0, disconnects = 0;
  ULONGLONG startup_started = 0;
  bool valid = true, close_in_terminal = true, closed = false;
  std::function<bool()> observe_worker;
  static void V8HOST_CDECL State(void* user_data, v8host_session session, v8host_session_state state, v8host_status status) {
    auto& c = *static_cast<PublicCapture*>(user_data);
    c.valid &= ::GetCurrentThreadId() == c.thread && session == c.session && status == v8host_status_ok && !c.closed;
    c.states.push_back(state);
    if (state == v8host_session_state_worker_security_ready && c.startup_started) {
      const ULONGLONG duration = ::GetTickCount64() - c.startup_started;
      g_startup_ms.push_back(duration);
      std::printf("[client startup] session=%p duration_ms=%llu\n", session, duration);
    }
  }
  static void V8HOST_CDECL Run(void* user_data, v8host_run run, v8host_run_event event, v8host_status status) {
    auto& c = *static_cast<PublicCapture*>(user_data);
    c.valid &= ::GetCurrentThreadId() == c.thread && run == c.run && !c.closed;
    c.events.push_back(event); c.statuses.push_back(status);
    if (event != v8host_run_event_started) {
      ++c.terminal;
      if (c.close_in_terminal) {
        c.client->close(c.session); c.closed = true;
      }
    }
  }
  static void V8HOST_CDECL Message(void* user_data, v8host_run run, v8host_message_kind kind, const void* data, size_t size) {
    auto& c = *static_cast<PublicCapture*>(user_data);
    c.valid &= ::GetCurrentThreadId() == c.thread && run == c.run && !c.closed &&
        c.states == std::vector<int32_t>{v8host_session_state_ready,
          v8host_session_state_worker_startup_ready, v8host_session_state_worker_security_ready};
    c.kinds.push_back(kind);
    const auto* bytes = static_cast<const uint8_t*>(data);
    c.messages.emplace_back(bytes, bytes + size);
    if (c.observe_worker) c.valid &= c.observe_worker();
  }
  static void V8HOST_CDECL Disconnect(void* user_data) {
    auto& c = *static_cast<PublicCapture*>(user_data);
    c.valid &= ::GetCurrentThreadId() == c.thread && !c.closed; ++c.disconnects;
  }
  v8host_callbacks Table() {
    v8host_callbacks t = {}; t.struct_size = sizeof(t); t.user_data = this;
    t.on_session_state = State; t.on_run_event = Run; t.on_relay_message = Message;
    t.on_broker_disconnect = Disconnect; return t;
  }
  ~PublicCapture() { if (session && !closed) client->close(session); }
};
template <class Predicate>
bool PumpPublic(Predicate done, DWORD timeout = 10000) {
  ULONGLONG deadline = ::GetTickCount64() + timeout;
  while (!done()) {
    if (::GetTickCount64() >= deadline) return false;
    MSG msg;
    if (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) ::DispatchMessageW(&msg);
    else ::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }
  return true;
}
// Observe the production-launched dedicated tree before terminal callback close.
struct PublicDedicatedProcesses {
  HANDLE broker = nullptr, worker = nullptr;
  std::wstring shared_endpoint;
  std::string observation_failure;
  bool Prepare() {
    v8host::PayloadIdentity payload;
    std::vector<uint8_t> sid;
    LUID session;
    DWORD error;
    std::array<uint8_t, 32> key;
    return v8host::ResolvePayloadIdentity(ExecutableDirectory(), L"sbox.exe", L"v8host.dll", &payload, &error) &&
        v8host::QueryCurrentSidAndSession(&sid, &session, &error) &&
        v8host::DeriveEndpoint(sid, payload.plugin_set_id, payload.install_root, BrokerMode::kShared, nullptr, &shared_endpoint, &key);
  }
  ~PublicDedicatedProcesses() {
    for (HANDLE process : {worker, broker}) {
      if (!process) continue;
      if (::WaitForSingleObject(process, 10000) != WAIT_OBJECT_0) {
        std::printf("[cleanup] forced owned dedicated process termination\n");
        ::TerminateProcess(process, 1);
        ::WaitForSingleObject(process, 5000);
      }
      ::CloseHandle(process);
    }
  }
  bool Observe() {
    auto fail = [&](const char* why) { observation_failure = why; return false; };
    // A dedicated session must not publish the deterministic shared endpoint.
    if (::WaitNamedPipeW(shared_endpoint.c_str(), 0) || ::GetLastError() != ERROR_FILE_NOT_FOUND)
      return fail("shared endpoint present or unexpected pipe error");
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return fail("process snapshot");
    std::vector<PROCESSENTRY32W> entries;
    PROCESSENTRY32W entry = {}; entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry)) {
      do {
        if (::_wcsicmp(entry.szExeFile, L"sbox.exe") == 0) entries.push_back(entry);
      } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    DWORD broker_pid = 0, worker_pid = 0;
    for (const auto& e : entries) {
      if (e.th32ParentProcessID == ::GetCurrentProcessId()) {
        if (broker_pid) return fail("multiple broker children");
        broker_pid = e.th32ProcessID;
      }
    }
    for (const auto& e : entries) {
      if (e.th32ParentProcessID == broker_pid) {
        if (worker_pid) return fail("multiple worker children");
        worker_pid = e.th32ProcessID;
      }
    }
    if (!broker_pid || !worker_pid) return fail("missing broker/worker parent mapping");
    if (broker || worker) {
      const bool alive = broker && worker && ::GetProcessId(broker) == broker_pid &&
          ::GetProcessId(worker) == worker_pid &&
          ::WaitForSingleObject(broker, 0) == WAIT_TIMEOUT &&
          ::WaitForSingleObject(worker, 0) == WAIT_TIMEOUT;
      return alive || fail("held dedicated PID changed/exited");
    }
    HANDLE held_broker = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, broker_pid);
    HANDLE held_worker = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, worker_pid);
    auto same_image = [](HANDLE h) {
      if (!h) return false;
      std::wstring path(32768, L'\0'); DWORD size = static_cast<DWORD>(path.size());
      if (!::QueryFullProcessImageNameW(h, 0, path.data(), &size)) return false;
      path.resize(size);
      return ::_wcsicmp(path.c_str(), (ExecutableDirectory() + L"\\sbox.exe").c_str()) == 0;
    };
    if (!same_image(held_broker) || !same_image(held_worker)) {
      if (held_broker) ::CloseHandle(held_broker);
      if (held_worker) ::CloseHandle(held_worker);
      return fail("held dedicated image mismatch/query");
    }
    broker = held_broker; worker = held_worker;
    std::printf("[public dedicated] broker=%lu worker=%lu\n", broker_pid, worker_pid);
    return true;
  }
  bool Reaped() {
    const bool exited = broker && worker && ::WaitForSingleObject(worker, 10000) == WAIT_OBJECT_0 &&
        ::WaitForSingleObject(broker, 10000) == WAIT_OBJECT_0;
    if (exited) std::printf("[reaped] dedicated broker=%lu worker=%lu\n", ::GetProcessId(broker), ::GetProcessId(worker));
    return exited;
  }
};
bool PublicRealJsMode(int32_t mode, std::string* detail, bool lpac = false, bool trusted = false) {
  CapturedBroker broker;
  PublicDedicatedProcesses dedicated;
  if (mode == v8host_broker_mode_shared && !broker.Launch(detail)) return false;
  auto& client = PublicClient();
  if (!client.Load()) { *detail = "production DLL client load/initialize failed"; return false; }
  PublicCapture capture; capture.client = &client; capture.close_in_terminal = false;
  if (mode == v8host_broker_mode_dedicated) {
    if (!dedicated.Prepare()) { *detail = "dedicated shared-endpoint identity"; return false; }
    capture.observe_worker = [&] { return dedicated.Observe(); };
  }
  v8host::test::TestSessionConfig config;
  config.broker_mode = static_cast<v8host_broker_mode>(mode);
  // Trusted tier: the normal v8jsi.dll engine with JIT, and ACG off.
  config.tier = trusted ? v8host_tier_trusted : v8host_tier_untrusted;
  config.prohibit_dynamic_code = trusted ? 0 : 1; config.initial_token = sbox_token_level_restricted_same_access;
  config.delayed_integrity = sbox_integrity_level_untrusted;
  if (lpac) {
    config.use_app_container = config.low_privilege_app_container = true;
    config.app_container_profile_name = L"v8host.lpac.test";
    config.integrity = config.delayed_integrity = sbox_integrity_level_low;
    config.lockdown_token = sbox_token_level_lockdown;
  }
  if (client.Open(&config, &capture.session) != v8host_status_ok || !capture.session) {
    *detail = "public create"; return false;
  }
  auto callbacks = capture.Table();
  if (client.callbacks(capture.session, &callbacks) != v8host_status_ok) {
    *detail = "public callbacks"; return false;
  }
  for (int round = 0; round != 2; ++round) {
    capture.close_in_terminal = round == 1;
    std::string guest = "var n=0;host.onmessage=function(m){if(typeof m==='string'){"
        "host.postMessage('js echo: '+m);}else{var a=new Uint8Array(m);a[0]=0x42;"
        "host.postMessageBinary(m);}if(++n===2)host.complete();};";
    v8host::test::TestRunInputs input; input.tier_override = -1;
    input.payload = guest.data(); input.payload_len = guest.size();
    char text[6] = {}; std::memcpy(text, round ? "bravo" : "alpha", 5);
    uint8_t binary[] = {0, static_cast<uint8_t>(1 + round * 2), static_cast<uint8_t>(2 + round * 2)};
    v8host_run_s* previous = capture.run;
    if (round == 0) capture.startup_started = ::GetTickCount64();
    if (client.Start(capture.session, &input, &capture.run) != v8host_status_ok || !capture.run ||
        capture.run == previous) {
      *detail = "public same-session start round=" + std::to_string(round + 1); return false;
    }
    std::fill(guest.begin(), guest.end(), ' ');
    if (client.post(capture.run, v8host_message_kind_string, text, 5) != v8host_status_ok ||
        client.post(capture.run, v8host_message_kind_binary, binary, 3) != v8host_status_ok) {
      *detail = "public relay admission"; return false;
    }
    std::memset(text, 'x', 5); std::memset(binary, 0xFF, 3);
    // First LPAC load has taken 41.1 s; allow 90 s without extending ordinary run waits.
    if (!PumpPublic([&] { return capture.terminal >= round + 1 || capture.disconnects; },
        lpac && round == 0 ? 90000 : 10000)) {
      for (size_t i = 0; i < capture.events.size(); ++i) std::printf("[failure event] %d status=%d\n", capture.events[i], capture.statuses[i]);
      for (int32_t state : capture.states) std::printf("[failure state] %d\n", state);
      std::printf("[failure callbacks] states=%zu events=%zu messages=%zu terminals=%d disconnects=%d\n", capture.states.size(), capture.events.size(), capture.messages.size(), capture.terminal, capture.disconnects);
      *detail = "public callbacks timed out round=" + std::to_string(round + 1); return false;
    }
    const std::vector<int32_t> events = round == 0
        ? std::vector<int32_t>{v8host_run_event_started, v8host_run_event_completed}
        : std::vector<int32_t>{v8host_run_event_started, v8host_run_event_completed,
                              v8host_run_event_started, v8host_run_event_completed};
    const size_t count = static_cast<size_t>(round + 1) * 2;
    if (!capture.valid || capture.terminal != round + 1 || capture.disconnects ||
        capture.states != std::vector<int32_t>{v8host_session_state_ready,
            v8host_session_state_worker_startup_ready, v8host_session_state_worker_security_ready} ||
        capture.events != events || capture.statuses != std::vector<v8host_status>(count, v8host_status_ok) ||
        capture.kinds.size() != count || capture.messages.size() != count ||
        capture.kinds[count - 2] != sbox_msg_kind_string || capture.kinds[count - 1] != sbox_msg_kind_binary ||
        std::string(capture.messages[count - 2].begin(), capture.messages[count - 2].end()) !=
            (round ? "js echo: bravo" : "js echo: alpha") ||
        capture.messages[count - 1] != std::vector<uint8_t>{0x42, static_cast<uint8_t>(1 + round * 2),
            static_cast<uint8_t>(2 + round * 2)} || capture.closed != (round == 1)) {
      for (size_t i = 0; i < capture.events.size(); ++i) std::printf("[failure event] %d status=%d\n", capture.events[i], capture.statuses[i]);
      for (int32_t state : capture.states) std::printf("[failure state] %d\n", state);
      std::printf("[failure invariant] valid=%d states=%zu events=%zu messages=%zu terminal=%d closed=%d observation=%s\n", capture.valid, capture.states.size(), capture.events.size(), capture.messages.size(), capture.terminal, capture.closed, dedicated.observation_failure.c_str());
      *detail = "public readiness/copy/FIFO/sole terminal invariant round=" + std::to_string(round + 1);
      return false;
    }
    if (trusted && round == 0) {
      // The trusted worker keeps dynamic code allowed (JIT, ACG off) after lockdown.
      HANDLE query = ::OpenProcess(0x400, FALSE, ::GetProcessId(dedicated.worker));
      PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy = {};
      const bool acg_off = query && ::GetProcessMitigationPolicy(query, ProcessDynamicCodePolicy, &policy, sizeof(policy)) &&
          policy.ProhibitDynamicCode == 0;
      if (query) ::CloseHandle(query);
      if (!acg_off) { *detail = "trusted worker has ACG on or the query failed"; return false; }
      std::printf("[public trusted] worker=%lu ACG=0 (JIT)\n", ::GetProcessId(dedicated.worker));
    }
  }
  if (!(mode == v8host_broker_mode_shared ? broker.Reaped(1) : dedicated.Reaped())) {
    { std::lock_guard<std::mutex> lock(broker.mutex); std::printf("[failure diagnostics] %s\n", broker.diagnostics.c_str()); }
    *detail = "public held broker/worker reaping"; return false;
  }
  MSG msg; while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) ::DispatchMessageW(&msg);
  *detail = "production DLL: client function + setter exports; two same-session runs; distinct copied string+binary "
      "JS transforms; readiness/FIFO; one COMPLETED/OK per run; callback close; one worker reaped";
  return capture.valid && capture.terminal == 2 && capture.disconnects == 0;
}
bool PublicRealCancelAndProfileBody(std::string* detail) {
  *detail = "public profile/cancel/admission/reap check";
  CapturedBroker broker;
  if (!broker.Launch(detail)) return false;
  auto& client = PublicClient();
  if (!client.Load()) { *detail = "public client load"; return false; }
  PublicCapture c; c.client = &client; c.close_in_terminal = false;
  v8host::test::TestSessionConfig config;
  config.broker_mode = v8host_broker_mode_shared; config.tier = v8host_tier_trusted;
  config.initial_token = sbox_token_level_restricted_same_access; config.delayed_integrity = sbox_integrity_level_untrusted;
  if (client.Open(&config, &c.session) != v8host_status_ok) return false;
  auto callbacks = c.Table(); if (client.callbacks(c.session, &callbacks) != v8host_status_ok) return false;
  DWORD worker_pid = 0;
  auto same_worker = [&] {
    std::lock_guard<std::mutex> lock(broker.mutex);
    if (broker.failed || broker.workers.size() != 1 || ::WaitForSingleObject(broker.workers[0], 0) != WAIT_TIMEOUT) return false;
    const DWORD pid = ::GetProcessId(broker.workers[0]);
    if (!worker_pid) worker_pid = pid;
    return pid && pid == worker_pid;
  };
  for (int round = 0; round < 4; ++round) {
    std::string guest = round == 0 ? "host.onmessage=function(m){};host.postMessage('armed');" :
        "host.postMessage('done');host.complete();";
    v8host::test::TestRunInputs input; input.tier_override = round == 2 ? v8host_tier_untrusted : -1;
    input.payload = guest.data(); input.payload_len = guest.size();
    input.engine_dll_override = round == 2 ? L"missing-profile-engine.dll" : L"v8jsisb.dll";
    if (round == 0) c.startup_started = ::GetTickCount64();
    if (client.Start(c.session, &input, &c.run) != v8host_status_ok || !c.run) { *detail = "public negative start"; return false; }
    if (round == 0) {
      if (!PumpPublic([&] { return !c.messages.empty() || c.disconnects; }) || c.messages.empty() ||
          std::string(c.messages[0].begin(), c.messages[0].end()) != "armed" ||
          !PumpPublic(same_worker) || client.cancel(c.run) != v8host_status_ok) {
        *detail = "public armed/cancel/worker observation messages=" + std::to_string(c.messages.size()) +
            " terminals=" + std::to_string(c.terminal) + " disconnects=" + std::to_string(c.disconnects) +
            " status=" + std::to_string(c.statuses.empty() ? -1 : c.statuses.back()); return false;
      }
    }
    if (!PumpPublic([&] { return c.terminal >= round + 1 || c.disconnects; })) { *detail = "public negative terminal timeout"; return false; }
    const int32_t expected_event = round == 0 ? v8host_run_event_cancelled : round == 2 ? v8host_run_event_failed : v8host_run_event_completed;
    const v8host_status expected_status = round == 2 ? v8host_status_profile_already_bound : v8host_status_ok;
    const std::vector<int32_t> expected_events = round == 0 ? std::vector<int32_t>{v8host_run_event_started, v8host_run_event_cancelled} :
        round == 1 ? std::vector<int32_t>{v8host_run_event_started, v8host_run_event_cancelled, v8host_run_event_started, v8host_run_event_completed} :
        round == 2 ? std::vector<int32_t>{v8host_run_event_started, v8host_run_event_cancelled, v8host_run_event_started, v8host_run_event_completed, v8host_run_event_started, v8host_run_event_failed} :
        std::vector<int32_t>{v8host_run_event_started, v8host_run_event_cancelled, v8host_run_event_started, v8host_run_event_completed, v8host_run_event_started, v8host_run_event_failed, v8host_run_event_started, v8host_run_event_completed};
    if (!c.valid || c.disconnects || c.terminal != round + 1 || c.events != expected_events ||
        c.events.back() != expected_event || c.statuses.back() != expected_status || !same_worker() ||
        client.cancel(c.run) != v8host_status_run_terminal || client.post(c.run, v8host_message_kind_string, "late", 4) != v8host_status_run_terminal) {
      *detail = "public cancel/profile/terminal/reuse round=" + std::to_string(round); return false;
    }
  }
  if (c.messages != std::vector<std::vector<uint8_t>>{{'a','r','m','e','d'}, {'d','o','n','e'}, {'d','o','n','e'}} ||
      c.statuses != std::vector<v8host_status>{v8host_status_ok,v8host_status_ok,v8host_status_ok,v8host_status_ok,v8host_status_ok,v8host_status_profile_already_bound,v8host_status_ok,v8host_status_ok}) {
    *detail = "profile failure ran guest or wrong terminal status"; return false;
  }
  client.close(c.session); c.closed = true;
  if (!broker.Reaped(1)) { *detail = "public negative worker reaping"; return false; }
  MSG msg; while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) ::DispatchMessageW(&msg);
  return c.valid && c.terminal == 4 && c.disconnects == 0;
}
struct ProcessRunRecord {
  v8host_run_s* handle = nullptr;
  std::string token;
  std::vector<int32_t> events, kinds;
  std::vector<v8host_status> statuses;
  std::vector<std::vector<uint8_t>> messages;
  unsigned terminals = 0;
};
struct ProcessSessionRecord {
  ContractCClient& client = PublicClient();
  v8host_session_s* handle = nullptr;
  DWORD thread = ::GetCurrentThreadId();
  std::deque<ProcessRunRecord> runs;
  std::vector<int32_t> states;
  std::vector<v8host_status> state_statuses;
  unsigned disconnects = 0;
  ULONGLONG startup_started = 0;
  bool valid = true, closed = false, close_on_terminal = false;
  ~ProcessSessionRecord() { Close(); }
  void Close() {
    if (handle && !closed) { client.close(handle); closed = true; }
  }
  ProcessRunRecord* Find(v8host_run_s* run) {
    valid &= ::GetCurrentThreadId() == thread && !closed;
    for (auto& r : runs) if (r.handle == run) return &r;
    valid = false; return nullptr;
  }
  static void V8HOST_CDECL State(void* user_data, v8host_session session, v8host_session_state state, v8host_status status) {
    auto& c = *static_cast<ProcessSessionRecord*>(user_data);
    c.valid &= ::GetCurrentThreadId() == c.thread && session == c.handle && !c.closed;
    if (c.states.size() >= 8) { c.valid = false; return; }
    c.states.push_back(state); c.state_statuses.push_back(status);
    if (state == v8host_session_state_worker_security_ready && c.startup_started)
      std::printf("[client startup] session=%p duration_ms=%llu\n", session, ::GetTickCount64() - c.startup_started);
  }
  static void V8HOST_CDECL Event(void* user_data, v8host_run run, v8host_run_event event, v8host_status status) {
    auto& c = *static_cast<ProcessSessionRecord*>(user_data);
    auto* r = c.Find(run); if (!r) return;
    if (r->events.size() >= 8) { c.valid = false; return; }
    r->events.push_back(event); r->statuses.push_back(status);
    if (event != v8host_run_event_started) {
      ++r->terminals; c.valid &= r->terminals == 1;
      std::printf("[terminal] session=%p run=%p token=%s event=%d status=%d count=%u\n",
          c.handle, run, r->token.c_str(), event, status, r->terminals);
      if (c.close_on_terminal) c.Close();
    }
  }
  static void V8HOST_CDECL Message(void* user_data, v8host_run run, v8host_message_kind kind, const void* data, size_t size) {
    auto& c = *static_cast<ProcessSessionRecord*>(user_data);
    auto* r = c.Find(run); if (!r) return;
    c.valid &= r->terminals == 0 && c.states == std::vector<int32_t>{v8host_session_state_ready,
        v8host_session_state_worker_startup_ready, v8host_session_state_worker_security_ready};
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (r->messages.size() >= 32 || size > 1024) { c.valid = false; return; }
    r->kinds.push_back(kind); r->messages.emplace_back(bytes, bytes + size);
  }
  static void V8HOST_CDECL Disconnect(void* user_data) {
    auto& c = *static_cast<ProcessSessionRecord*>(user_data);
    c.valid &= ::GetCurrentThreadId() == c.thread && !c.closed;
    ++c.disconnects; c.valid &= c.disconnects == 1;
  }
  bool Create() {
    if (!client.Load()) return false;
    v8host::test::TestSessionConfig config;
    config.broker_mode = v8host_broker_mode_shared; config.tier = v8host_tier_untrusted;
    config.prohibit_dynamic_code = 1; config.initial_token = sbox_token_level_restricted_same_access;
    config.delayed_integrity = sbox_integrity_level_untrusted;
    if (client.Open(&config, &handle) != v8host_status_ok || !handle) return false;
    v8host_callbacks cb = {}; cb.struct_size = sizeof(cb); cb.user_data = this;
    cb.on_session_state = State; cb.on_run_event = Event; cb.on_relay_message = Message; cb.on_broker_disconnect = Disconnect;
    return client.callbacks(handle, &cb) == v8host_status_ok;
  }
  bool Start(const std::string& token, bool finish_immediately = false) {
    std::string guest = finish_immediately ? "host.postMessage('" + token + "');host.complete();" :
        "host.onmessage=function(m){if(typeof m==='string'){if(m==='finish'){host.complete();return;}"
        "host.postMessage('" + token + ":'+m);}else{var a=new Uint8Array(m);a[0]^=0x5a;"
        "host.postMessageBinary(m);}};host.postMessage('" + token + ":armed');";
    v8host::test::TestRunInputs input; input.tier_override = -1;
    input.payload = guest.data(); input.payload_len = guest.size();
    if (runs.empty()) startup_started = ::GetTickCount64();
    runs.emplace_back(); auto& r = runs.back(); r.token = token;
    if (client.Start(handle, &input, &r.handle) != v8host_status_ok || !r.handle) { runs.pop_back(); return false; }
    if (std::any_of(runs.begin(), runs.end() - 1, [&](const auto& prior) { return prior.handle == r.handle; })) {
      valid = false; return false;
    }
    return true;
  }
  bool Armed(size_t index) {
    auto& r = runs[index];
    return PumpPublic([&] { return !r.messages.empty() || r.terminals || disconnects; }) && valid &&
        !disconnects && r.events == std::vector<int32_t>{v8host_run_event_started} && r.terminals == 0 &&
        r.statuses == std::vector<v8host_status>{v8host_status_ok} && r.kinds == std::vector<int32_t>{sbox_msg_kind_string} &&
        r.messages == std::vector<std::vector<uint8_t>>{Text(r.token + ":armed")} &&
        state_statuses == std::vector<v8host_status>(3, v8host_status_ok);
  }
  static std::vector<uint8_t> Text(const std::string& s) { return {s.begin(), s.end()}; }
  bool Echo(size_t index, const std::string& text, uint8_t value) {
    auto& r = runs[index]; size_t before = r.messages.size();
    uint8_t binary[] = {value, 0, 0xff};
    if (client.post(r.handle, v8host_message_kind_string, text.data(), text.size()) != v8host_status_ok ||
        client.post(r.handle, v8host_message_kind_binary, binary, sizeof(binary)) != v8host_status_ok ||
        !PumpPublic([&] { return r.messages.size() >= before + 2 || r.terminals || disconnects; })) return false;
    return valid && !disconnects && !r.terminals && r.messages.size() == before + 2 &&
        r.kinds[before] == sbox_msg_kind_string && r.kinds[before + 1] == sbox_msg_kind_binary &&
        r.messages[before] == Text(r.token + ":" + text) &&
        r.messages[before + 1] == std::vector<uint8_t>{static_cast<uint8_t>(value ^ 0x5a), 0, 0xff};
  }
  bool Terminal(size_t index, int32_t event, v8host_status status) {
    auto& r = runs[index];
    return PumpPublic([&] { return r.terminals != 0; }) && valid && r.terminals == 1 &&
        r.events == std::vector<int32_t>{v8host_run_event_started, event} &&
        r.statuses == std::vector<v8host_status>{v8host_status_ok, status};
  }
  bool Finish(size_t index) {
    return client.post(runs[index].handle, v8host_message_kind_string, "finish", 6) == v8host_status_ok &&
        Terminal(index, v8host_run_event_completed, v8host_status_ok);
  }
  size_t CallbackCount() const {
    size_t count = states.size() + disconnects;
    for (const auto& r : runs) count += r.events.size() + r.messages.size();
    return count;
  }
};
void DrainPublic() {
  MSG msg;
  while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) ::DispatchMessageW(&msg);
}
bool MapPublicWorker(CapturedBroker& broker, ProcessSessionRecord& session, size_t index) {
  if (!session.Armed(0) || !broker.Observe(index + 1)) return false;
  HANDLE worker = broker.Worker(index);
  if (!worker || ::WaitForSingleObject(worker, 0) != WAIT_TIMEOUT) return false;
  std::printf("[mapped] broker=%lu session=%p run=%p ordinal=%zu worker=%lu token=%s\n",
      ::GetProcessId(broker.process), session.handle, session.runs[0].handle, index + 1,
      ::GetProcessId(worker), session.runs[0].token.c_str());
  return true;
}
bool PublicNSessionBody(std::string* detail) {
  *detail = "session/PID/routing/reap check";
  CapturedBroker broker;
  if (!broker.Launch(detail)) return false;
  std::array<ProcessSessionRecord, 3> sessions;
  for (size_t i = 0; i < sessions.size(); ++i) {
    if (!sessions[i].Create() || !sessions[i].Start("tenant" + std::to_string(i)) ||
        !MapPublicWorker(broker, sessions[i], i)) { *detail = "sequential session/PID/armed mapping"; return false; }
  }
  std::set<DWORD> pids;
  for (size_t i = 0; i < sessions.size(); ++i) {
    HANDLE h = broker.Worker(i);
    if (!h || ::WaitForSingleObject(h, 0) != WAIT_TIMEOUT || !pids.insert(::GetProcessId(h)).second) return false;
  }
  for (size_t round = 0; round < 2; ++round) {
    for (size_t i = 0; i < sessions.size(); ++i) {
      auto& c = sessions[i];
      if (round && (!c.Start("reuse" + std::to_string(i)) || !c.Armed(round))) return false;
      if (!c.Echo(round, "unique" + std::to_string(i + round * 3), static_cast<uint8_t>(i + round * 16)) ||
          !c.Finish(round) || !broker.Observe(3) || ::WaitForSingleObject(broker.Worker(i), 0) != WAIT_TIMEOUT ||
          !pids.count(::GetProcessId(broker.Worker(i)))) { *detail = "text/binary routing/reuse"; return false; }
    }
  }
  for (auto& c : sessions) c.Close();
  if (!broker.Reaped(3)) return false;
  DrainPublic();
  for (auto& c : sessions) if (!c.valid || c.disconnects || c.runs.size() != 2 ||
      c.runs[0].messages.size() != 3 || c.runs[1].messages.size() != 3) return false;
  *detail = "3 live sessions/3 distinct held PIDs; per-run text+binary tokens; 2 runs/PID; all reaped";
  return true;
}
bool PublicWorkerExitBody(std::string* detail) {
  *detail = "worker-exit admission/fault/survival/reap check";
  CapturedBroker broker;
  if (!broker.Launch(detail)) return false;
  ProcessSessionRecord crashed, sibling;
  if (!crashed.Create() || !crashed.Start("completed") || !MapPublicWorker(broker, crashed, 0) || !crashed.Finish(0) ||
      !sibling.Create() || !sibling.Start("survivor") || !MapPublicWorker(broker, sibling, 1) ||
      !crashed.Start("crashed-active") || !crashed.Armed(1) || !crashed.Start("crashed-pending") ||
      !PumpPublic([&] { return !crashed.runs[2].events.empty(); })) return false;
  if (crashed.runs[2].events != std::vector<int32_t>{v8host_run_event_started} ||
      !crashed.runs[2].messages.empty() || crashed.runs[2].terminals) return false;
  HANDLE killed = broker.Worker(0), healthy = broker.Worker(1);
  if (!killed || !healthy || ::WaitForSingleObject(killed, 0) != WAIT_TIMEOUT ||
      !::TerminateProcess(killed, 0xfa17) || ::WaitForSingleObject(killed, 10000) != WAIT_OBJECT_0) return false;
  DWORD code = 0;
  if (!::GetExitCodeProcess(killed, &code) || code != 0xfa17) return false;
  std::printf("[fault] broker=%lu worker=%lu exit=%lu owned-handle\n",
      ::GetProcessId(broker.process), ::GetProcessId(killed), code);
  if (!crashed.Terminal(1, v8host_run_event_worker_exited, v8host_status_run_terminal) ||
      !crashed.Terminal(2, v8host_run_event_worker_exited, v8host_status_run_terminal) ||
      !crashed.runs[2].messages.empty() || crashed.runs[0].terminals != 1 ||
      crashed.runs[0].events.back() != v8host_run_event_completed || crashed.disconnects) {
    *detail = "completed/active/queued worker-exit arbitration"; return false;
  }
  for (size_t i : {size_t{1}, size_t{2}}) {
    if (crashed.client.post(crashed.runs[i].handle, v8host_message_kind_string, "late", 4) != v8host_status_run_terminal ||
        crashed.client.cancel(crashed.runs[i].handle) != v8host_status_run_terminal) return false;
  }
  crashed.Close(); size_t frozen = crashed.CallbackCount();
  if (::WaitForSingleObject(broker.process, 0) != WAIT_TIMEOUT ||
      ::WaitForSingleObject(healthy, 0) != WAIT_TIMEOUT || !sibling.Echo(0, "after-kill", 0x11) ||
      !sibling.Finish(0) || !sibling.Start("survivor-fresh") || !sibling.Armed(1) ||
      !sibling.Echo(1, "fresh-after-kill", 0x22) || !sibling.Finish(1) || !broker.Observe(2) ||
      ::WaitForSingleObject(healthy, 0) != WAIT_TIMEOUT) { *detail = "original sibling worker did not survive"; return false; }
  sibling.Close();
  if (!broker.Reaped(2)) return false;
  DrainPublic();
  *detail = "OS-forced recorded worker exit; completed winner retained; active+queued WORKER_EXITED once; sibling original PID survives";
  return crashed.valid && sibling.valid && crashed.CallbackCount() == frozen && !sibling.disconnects;
}
bool PublicDisconnectBody(std::string* detail) {
  *detail = "disconnect admission/order/survival/reap check";
  CapturedBroker broker;
  if (!broker.Launch(detail)) return false;
  ProcessSessionRecord sibling;
  if (!sibling.Create() || !sibling.Start("disconnect-survivor") || !MapPublicWorker(broker, sibling, 0)) return false;
  HANDLE healthy = broker.Worker(0);
  std::array<ProcessSessionRecord, 4> closed_sessions;
  for (size_t order = 0; order < closed_sessions.size(); ++order) {
    auto& c = closed_sessions[order];
    if (!c.Create() || !c.Start("close-order" + std::to_string(order)) || !MapPublicWorker(broker, c, order + 1)) return false;
    if (order == 0) {
      // Close the real transport before either active or queued guest completion.
      if (!c.Start("never-execute") || !PumpPublic([&] { return !c.runs[1].events.empty(); }) ||
          !c.runs[1].messages.empty()) return false;
      c.Close();
    } else if (order == 1) {
      if (!c.Finish(0)) return false;
      c.Close();
    } else if (order == 2) {
      c.close_on_terminal = true;
      if (!c.Finish(0) || !c.closed) return false;
    } else {
      // Completion may race EOF, but explicit close suppresses queued delivery.
      if (c.client.post(c.runs[0].handle, v8host_message_kind_string, "finish", 6) != v8host_status_ok) return false;
      c.Close();
    }
    size_t frozen = c.CallbackCount();
    HANDLE worker = broker.Worker(order + 1);
    if (!worker || ::WaitForSingleObject(worker, 10000) != WAIT_OBJECT_0 ||
        ::WaitForSingleObject(healthy, 0) != WAIT_TIMEOUT || ::WaitForSingleObject(broker.process, 0) != WAIT_TIMEOUT ||
        !sibling.Echo(0, "close-survived" + std::to_string(order), static_cast<uint8_t>(order))) return false;
    DrainPublic();
    if (!c.valid || c.CallbackCount() != frozen || c.disconnects || c.runs[0].messages.size() != 1 ||
        c.runs[0].terminals != ((order == 1 || order == 2) ? 1u : 0u) ||
        (order == 0 && (c.runs[1].terminals || !c.runs[1].messages.empty()))) {
      *detail = "close/EOF/completion/callback-close ordering=" + std::to_string(order); return false;
    }
    std::printf("[reaped] close-order=%zu broker=%lu session=%p worker=%lu callbacks=%zu\n",
        order, ::GetProcessId(broker.process), c.handle, ::GetProcessId(worker), frozen);
  }
  if (!sibling.Finish(0) || !sibling.Start("disconnect-fresh") || !sibling.Armed(1) ||
      !sibling.Echo(1, "fresh", 0x33) || !sibling.Finish(1) || !broker.Observe(5) ||
      ::WaitForSingleObject(healthy, 0) != WAIT_TIMEOUT) return false;
  sibling.Close();
  if (!broker.Reaped(5)) return false;
  DrainPublic();
  *detail = "real pipe EOF before/after completion, callback-close, completion/EOF overlap; no callback after close; original sibling PID survives";
  return sibling.valid && !sibling.disconnects;
}
bool PublicBrokerKillBody(std::string* detail) {
  *detail = "broker-loss admission/terminal/replacement check";
  CapturedBroker old_broker;
  if (!old_broker.Launch(detail)) return false;
  std::array<ProcessSessionRecord, 3> old;
  for (size_t i = 0; i < old.size(); ++i) {
    auto& c = old[i];
    if (!c.Create() || !c.Start("old-active" + std::to_string(i)) || !MapPublicWorker(old_broker, c, i) ||
        !c.Start("old-queued-side-effect" + std::to_string(i)) ||
        !PumpPublic([&] { return !c.runs[1].events.empty(); }) ||
        c.runs[1].events != std::vector<int32_t>{v8host_run_event_started} ||
        !c.runs[1].messages.empty() || c.runs[1].terminals) return false;
  }
  DWORD pid = ::GetProcessId(old_broker.process);
  if (!::TerminateProcess(old_broker.process, 0xbeef) || !old_broker.ReapedAfterKill(3)) {
    *detail = "owned broker kill did not reap every held worker"; return false;
  }
  std::printf("[fault] owned broker=%lu killed; every old worker exited before replacement\n", pid);
  for (auto& c : old) {
    for (size_t i = 0; i < c.runs.size(); ++i) {
      if (!c.Terminal(i, v8host_run_event_broker_lost, v8host_status_broker_lost) ||
          c.client.post(c.runs[i].handle, v8host_message_kind_string, "old-late", 8) != v8host_status_run_terminal ||
          c.client.cancel(c.runs[i].handle) != v8host_status_run_terminal) return false;
    }
    if (!PumpPublic([&] { return c.disconnects != 0; }) || !c.valid || c.disconnects != 1 ||
        c.states.back() != v8host_session_state_closed || c.state_statuses.back() != v8host_status_broker_lost ||
        c.runs[0].messages.size() != 1 || !c.runs[1].messages.empty()) return false;
  }
  CapturedBroker replacement;
  WireHarness observer;
  if (!replacement.Launch(detail) ||
      !ConnectAndHandshake(BrokerMode::kShared, 900, &observer.connection, &observer.hello, detail) ||
      observer.connection.broker_pid() != ::GetProcessId(replacement.process)) return false;
  auto fence = [&](uint32_t request) {
    return observer.Send(protocol::BuildCancelRunFrame(observer.Header(request, 1, 1))) &&
        observer.Expect(protocol::MessageType::ACK, request, 1, 1);
  };
  if (!fence(1)) return false;
  for (auto& c : old) {
    std::string guest = "host.postMessage('old-retry-side-effect');host.complete();";
    v8host::test::TestRunInputs input; input.tier_override = -1;
    input.payload = guest.data(); input.payload_len = guest.size();
    v8host_run_s* run = nullptr;
    if (c.client.Start(c.handle, &input, &run) != v8host_status_connect_failed || run ||
        c.client.post(c.runs[0].handle, v8host_message_kind_string, "old", 3) != v8host_status_run_terminal) {
      *detail = "old terminal session/run accepted replay"; return false;
    }
  }
  if (!fence(2)) return false;
  {
    std::lock_guard<std::mutex> lock(replacement.mutex);
    if (replacement.failed || !replacement.workers.empty()) { *detail = "replacement automatic creation"; return false; }
  }
  std::printf("[replacement] broker=%lu workers=0 authenticated observer; old retries rejected\n",
      ::GetProcessId(replacement.process));
  ProcessSessionRecord fresh;
  if (!fresh.Create() || !fresh.Start("fresh-only") || !MapPublicWorker(replacement, fresh, 0) ||
      !fresh.Echo(0, "new-token", 0x44) || !fresh.Finish(0) || !fence(3)) return false;
  for (auto& c : old) {
    if (!c.valid || c.disconnects != 1 || c.runs[0].terminals != 1 || c.runs[1].terminals != 1 ||
        c.runs[0].messages != std::vector<std::vector<uint8_t>>{ProcessSessionRecord::Text(c.runs[0].token + ":armed")} ||
        !c.runs[1].messages.empty()) return false;
    c.Close();
  }
  fresh.Close(); observer.connection.Close();
  if (!replacement.Reaped(1)) { *detail = "replacement extra creation or failed reap"; return false; }
  DrainPublic();
  *detail = "3 old workers reaped before same-set replacement; 6 BROKER_LOST once; zero automatic workers; old attempts rejected; only explicit fresh token/one worker";
  return fresh.valid && !fresh.disconnects && fresh.runs[0].terminals == 1 &&
      std::all_of(old.begin(), old.end(), [](const auto& c) { return c.valid; });
}
// Two sibling sessions share one authenticated connection; the third uses another.
bool SameConnectionWorkerExitBody(std::string* detail) {
  CapturedBroker broker;
  WireHarness siblings, other;
  *detail = "same-connection setup/admission";
  if (!OpenCaptured(broker, siblings, detail) ||
      !ConnectAndHandshake(BrokerMode::kShared, 900, &other.connection, &other.hello, detail) ||
      other.connection.broker_pid() != ::GetProcessId(broker.process) ||
      other.hello.conn_id == siblings.hello.conn_id) return false;
  auto config = LogicalConfig(); config.broker_mode = 1;
  config.initial_token = sbox_token_level_restricted_same_access; config.delayed_integrity = sbox_integrity_level_untrusted;
  if (!siblings.Send(protocol::BuildCreateSessionFrame(siblings.Header(2, 2), config)) ||
      !siblings.Expect(protocol::MessageType::ACK, 2, 2) || !siblings.Expect(protocol::MessageType::SESSION_READY, 0, 2) ||
      !other.Send(protocol::BuildCreateSessionFrame(other.Header(1), config)) ||
      !other.Expect(protocol::MessageType::ACK, 1, 1) || !other.Expect(protocol::MessageType::SESSION_READY, 0, 1)) return false;
  auto relay = [&](WireHarness& h, uint32_t session, uint32_t run, int32_t kind, const std::vector<uint8_t>& body) {
    auto header = h.Header(0, session, run); header.type = protocol::MessageType::RELAY_TO_WORKER;
    return h.Send(protocol::BuildRelayFrame(header, kind, body.data(), body.size()));
  };
  auto text = [&](WireHarness& h, uint32_t session, uint32_t run, const std::string& body) {
    return relay(h, session, run, sbox_msg_kind_string, {body.begin(), body.end()});
  };
  auto message = [&](WireHarness& h, uint32_t session, uint32_t run, int32_t kind, const std::vector<uint8_t>& expected) {
    std::vector<uint8_t> bytes; protocol::FrameHeader header; int32_t received_kind;
    const uint8_t* body; size_t size;
    return h.Receive(&bytes, &header) && header.type == protocol::MessageType::RELAY_FROM_WORKER &&
        header.conn_id == h.hello.conn_id && header.target_id == session && header.run_id == run && !header.request_id &&
        protocol::DecodeRelayPayload(bytes.data() + protocol::kFrameHeaderSize,
            bytes.size() - protocol::kFrameHeaderSize, &received_kind, &body, &size) && received_kind == kind &&
        std::vector<uint8_t>(body, body + size) == expected;
  };
  auto token = [&](WireHarness& h, uint32_t session, uint32_t run, const std::string& expected) {
    return message(h, session, run, sbox_msg_kind_string, {expected.begin(), expected.end()});
  };
  auto start = [&](WireHarness& h, uint32_t request, uint32_t session, uint32_t run, const std::string& tag, bool first) {
    const std::string guest = "host.onmessage=function(m){if(typeof m==='string'){if(m==='finish'){host.complete();return;}"
        "host.postMessage('" + tag + ":'+m);}else{var a=new Uint8Array(m);a[0]^=0x5a;host.postMessageBinary(m);}};"
        "host.postMessage('" + tag + ":armed');";
    return h.Send(protocol::BuildStartRunFrame(h.Header(request, session, run), Script(guest))) &&
        h.Expect(protocol::MessageType::ACK, request, session, run) &&
        (!first || (h.Expect(protocol::MessageType::STARTUP_READY, 0, session) &&
            h.Expect(protocol::MessageType::SECURITY_READY, 0, session))) && token(h, session, run, tag + ":armed");
  };
  auto finish = [&](WireHarness& h, uint32_t session, uint32_t run) {
    std::vector<uint8_t> bytes; protocol::FrameHeader header; protocol::ResultPayload result;
    const bool ok = text(h, session, run, "finish") && h.Receive(&bytes, &header) &&
        header.type == protocol::MessageType::RESULT && header.conn_id == h.hello.conn_id &&
        header.target_id == session && header.run_id == run && !header.request_id &&
        protocol::DecodeResultPayload(bytes.data() + protocol::kFrameHeaderSize,
            bytes.size() - protocol::kFrameHeaderSize, &result) && result.disposition == protocol::ResultDisposition::kCompleted;
    if (ok) std::printf("[terminal] wire conn=%u session=%u run=%u COMPLETED count=1\n", h.hello.conn_id, session, run);
    return ok;
  };
  std::array<WireHarness*, 3> connections{&siblings, &siblings, &other};
  std::array<uint32_t, 3> ids{1, 2, 1}, requests{3, 4, 2};
  std::set<DWORD> pids;
  for (size_t i = 0; i < 3; ++i) {
    auto& h = *connections[i];
    if (!start(h, requests[i], ids[i], 1, "wire" + std::to_string(i), true) || !broker.Observe(i + 1)) return false;
    HANDLE worker = broker.Worker(i);
    if (!worker || ::WaitForSingleObject(worker, 0) != WAIT_TIMEOUT || !pids.insert(::GetProcessId(worker)).second) return false;
    std::printf("[mapped] broker=%lu conn=%u session=%u run=1 ordinal=%zu worker=%lu\n",
        ::GetProcessId(broker.process), h.hello.conn_id, ids[i], i + 1, ::GetProcessId(worker));
  }
  *detail = "same-connection exact text/binary routing and two runs/original PID";
  for (size_t round = 0; round < 2; ++round) for (size_t i = 0; i < 3; ++i) {
    auto& h = *connections[i]; const uint32_t run = static_cast<uint32_t>(round + 1);
    const std::string tag = "wire" + std::to_string(i) + (round ? "-reuse" : "");
    if (round && !start(h, i == 2 ? 3 : static_cast<uint32_t>(5 + i), ids[i], run, tag, false)) return false;
    const std::string sent = "unique" + std::to_string(i + round * 3);
    const uint8_t value = static_cast<uint8_t>(i + round * 16);
    if (!text(h, ids[i], run, sent) || !token(h, ids[i], run, tag + ":" + sent) ||
        !relay(h, ids[i], run, sbox_msg_kind_binary, {value, 0, 0xff}) ||
        !message(h, ids[i], run, sbox_msg_kind_binary, {static_cast<uint8_t>(value ^ 0x5a), 0, 0xff}) ||
        !finish(h, ids[i], run) || !broker.Observe(3) || ::WaitForSingleObject(broker.Worker(i), 0) != WAIT_TIMEOUT) return false;
  }
  *detail = "same-connection worker fault/active+queued terminal";
  if (!start(siblings, 7, 1, 3, "crashed", false) ||
      !siblings.Send(protocol::BuildStartRunFrame(siblings.Header(8, 1, 4), Script("host.postMessage('never');host.complete();"))) ||
      !siblings.Expect(protocol::MessageType::ACK, 8, 1, 4) ||
      !start(siblings, 9, 2, 3, "sibling", false) || !start(other, 4, 1, 3, "other", false)) return false;
  HANDLE killed = broker.Worker(0); DWORD code = 0;
  if (!::TerminateProcess(killed, 0xfa17) || ::WaitForSingleObject(killed, 10000) != WAIT_OBJECT_0 ||
      !::GetExitCodeProcess(killed, &code) || code != 0xfa17 ||
      !siblings.Expect(protocol::MessageType::WORKER_EXIT, 0, 1)) return false;
  std::printf("[fault] broker=%lu worker=%lu exit=%lu; wire conn=%u session=1 runs=3,4 WORKER_EXIT count=1\n",
      ::GetProcessId(broker.process), ::GetProcessId(killed), code, siblings.hello.conn_id);
  // The received terminal fences worker retirement. Late relays are dropped;
  // CANCEL/CLOSE are idempotent ACKs, and START on the removed session is BAD_STATE.
  *detail = "late RELAY/CANCEL/START/CLOSE responses or duplicate terminal";
  if (!text(siblings, 1, 3, "late") || !text(siblings, 1, 4, "late") ||
      !siblings.Send(protocol::BuildCancelRunFrame(siblings.Header(10, 1, 3))) ||
      !siblings.Expect(protocol::MessageType::ACK, 10, 1, 3) ||
      !siblings.Send(protocol::BuildCancelRunFrame(siblings.Header(11, 1, 4))) ||
      !siblings.Expect(protocol::MessageType::ACK, 11, 1, 4) ||
      !siblings.Send(protocol::BuildStartRunFrame(siblings.Header(12, 1, 5), Script("host.complete();")))) return false;
  std::vector<uint8_t> bytes; protocol::FrameHeader header; protocol::ErrorPayload error;
  if (!siblings.Receive(&bytes, &header) || header.type != protocol::MessageType::ERROR ||
      header.conn_id != siblings.hello.conn_id || header.target_id != 1 || header.run_id != 5 || header.request_id != 12 ||
      !protocol::DecodeErrorPayload(bytes.data() + protocol::kFrameHeaderSize, bytes.size() - protocol::kFrameHeaderSize, &error) ||
      error.status_code != protocol::StatusCode::ERROR_BAD_STATE ||
      !siblings.Send(protocol::BuildCloseSessionFrame(siblings.Header(13, 1))) ||
      !siblings.Expect(protocol::MessageType::ACK, 13, 1) ||
      !siblings.Send(protocol::BuildCloseSessionFrame(siblings.Header(14, 1))) ||
      !siblings.Expect(protocol::MessageType::ACK, 14, 1)) return false;
  *detail = "original same-connection sibling and independent connection survival";
  for (size_t i : {size_t{1}, size_t{2}}) {
    auto& h = *connections[i]; const std::string tag = i == 1 ? "sibling" : "other";
    if (!text(h, ids[i], 3, "after-kill") || !token(h, ids[i], 3, tag + ":after-kill") || !finish(h, ids[i], 3) ||
        !start(h, i == 1 ? 15 : 5, ids[i], 4, "fresh" + tag, false) ||
        !text(h, ids[i], 4, "fresh") || !token(h, ids[i], 4, "fresh" + tag + ":fresh") || !finish(h, ids[i], 4) ||
        !broker.Observe(3) || ::WaitForSingleObject(broker.Worker(i), 0) != WAIT_TIMEOUT ||
        ::WaitForSingleObject(broker.process, 0) != WAIT_TIMEOUT) return false;
  }
  // Final control barriers reject any queued second terminal/late output.
  if (!siblings.Send(protocol::BuildCloseSessionFrame(siblings.Header(16, 2))) ||
      !siblings.Expect(protocol::MessageType::ACK, 16, 2) ||
      !other.Send(protocol::BuildCloseSessionFrame(other.Header(6))) || !other.Expect(protocol::MessageType::ACK, 6, 1)) return false;
  siblings.connection.Close(); other.connection.Close();
  if (!broker.Reaped(3)) { *detail = "same-connection joined reaping"; return false; }
  *detail = "3 real PIDs/2 connections including siblings; text/binary and reuse; killed session only; deterministic late-frame barriers";
  return true;
}
// Each child loads its own production client singleton and immutable payload set.
// These helpers retain the negotiated addressing and distinguish EOF from timeout.
bool ProcessToken(WireHarness& h, uint32_t session, uint32_t run, const std::string& expected) {
  std::vector<uint8_t> bytes; protocol::FrameHeader header; int32_t kind;
  const uint8_t* body; size_t size;
  return h.Receive(&bytes, &header) && header.type == protocol::MessageType::RELAY_FROM_WORKER &&
      header.conn_id == h.hello.conn_id && header.target_id == session && header.run_id == run && !header.request_id &&
      header.version_major == h.hello.selected_major && header.version_minor == h.hello.selected_minor &&
      protocol::DecodeRelayPayload(bytes.data() + protocol::kFrameHeaderSize,
          bytes.size() - protocol::kFrameHeaderSize, &kind, &body, &size) && kind == sbox_msg_kind_string &&
      std::string(reinterpret_cast<const char*>(body), size) == expected;
}
bool ProcessEof(WireHarness& h) {
  OVERLAPPED pending = {}; pending.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!pending.hEvent) return false;
  std::vector<uint8_t> bytes(protocol::kMaxFrameSize); DWORD size = 0;
  const BOOL started = ::ReadFile(h.connection.pipe(), bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &pending);
  DWORD error = started ? ERROR_SUCCESS : ::GetLastError();
  if (started || error == ERROR_IO_PENDING) {
    if (::WaitForSingleObject(pending.hEvent, 10000) == WAIT_OBJECT_0)
      error = ::GetOverlappedResult(h.connection.pipe(), &pending, &size, FALSE) ? ERROR_SUCCESS : ::GetLastError();
    else {
      ::CancelIoEx(h.connection.pipe(), &pending);
      ::GetOverlappedResult(h.connection.pipe(), &pending, &size, TRUE);
      error = WAIT_TIMEOUT;
    }
  }
  ::CloseHandle(pending.hEvent);
  return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED;
}
protocol::CreateSessionPayload ProcessConfig() {
  auto config = LogicalConfig(); config.broker_mode = 1;
  config.initial_token = sbox_token_level_restricted_same_access; config.delayed_integrity = sbox_integrity_level_untrusted;
  return config;
}
bool ProcessCreate(WireHarness& h, uint32_t request, uint32_t session) {
  return h.Send(protocol::BuildCreateSessionFrame(h.Header(request, session), ProcessConfig())) &&
      h.Expect(protocol::MessageType::ACK, request, session) && h.Expect(protocol::MessageType::SESSION_READY, 0, session);
}
bool ProcessStart(WireHarness& h, uint32_t request, uint32_t session, uint32_t run, bool first) {
  return h.Send(protocol::BuildStartRunFrame(h.Header(request, session, run), Script(
      "host.onmessage=function(m){host.postMessage(m);};host.postMessage('armed');"))) &&
      h.Expect(protocol::MessageType::ACK, request, session, run) &&
      (!first || (h.Expect(protocol::MessageType::STARTUP_READY, 0, session) &&
          h.Expect(protocol::MessageType::SECURITY_READY, 0, session))) && ProcessToken(h, session, run, "armed");
}
bool ProcessEcho(WireHarness& h, uint32_t session, uint32_t run, const std::string& text) {
  auto header = h.Header(0, session, run); header.type = protocol::MessageType::RELAY_TO_WORKER;
  return h.Send(protocol::BuildRelayFrame(header, sbox_msg_kind_string,
      reinterpret_cast<const uint8_t*>(text.data()), text.size())) && ProcessToken(h, session, run, text);
}
bool FillProcessInbound(WireHarness& h, uint32_t session, uint32_t run, size_t bytes, size_t* charged) {
  const size_t overhead = v8host::EncodeRunEnvelope(GuestRelay(0, run)).size();
  size_t frames = 0;
  if (overhead != 16 || bytes > protocol::kMaxQueuedRelayBytesPerRun) return false;
  while (bytes) {
    size_t part = (std::min)(bytes, size_t(protocol::kMaxFramePayload));
    // Avoid an unencodable tail without exceeding the legal per-frame envelope.
    if (bytes > part && bytes - part < overhead) part -= overhead;
    if (part < overhead || ++frames > 256) return false;
    std::vector<uint8_t> payload(part - overhead, 7);
    auto header = h.Header(0, session, run); header.type = protocol::MessageType::RELAY_TO_WORKER;
    auto frame = protocol::BuildRelayFrame(header, sbox_msg_kind_binary, payload.data(), payload.size());
    if (frame.size() > protocol::kMaxFrameSize || !h.Send(std::move(frame))) return false;
    *charged += v8host::EncodeRunEnvelope(GuestRelay(payload.size(), run)).size();
    bytes -= part;
  }
  return true;
}
bool RealInboundAggregateBody(std::string* detail) {
  CapturedBroker broker; WireHarness healthy;
  *detail = "aggregate healthy-connection setup";
  if (!OpenCaptured(broker, healthy, detail) || !ProcessStart(healthy, 2, 1, 1, true) || !broker.Observe(1)) return false;
  HANDLE survivor = broker.Worker(0); size_t count = 1;
  const size_t cap = protocol::kMaxQueuedRelayBytesPerConnection;
  // Separate legal histories prove cap-1, cap, and an encoded envelope exceeding cap by exactly one.
  for (unsigned variant = 0; variant < 4; ++variant) {
    WireHarness pressured;
    *detail = "aggregate authenticated admission variant=" + std::to_string(variant);
    if (!ConnectAndHandshake(BrokerMode::kShared, 900, &pressured.connection, &pressured.hello, detail) ||
        pressured.connection.broker_pid() != ::GetProcessId(broker.process) || pressured.hello.conn_id == healthy.hello.conn_id)
      return false;
    uint32_t request = 1; const size_t first = count;
    for (uint32_t session = 1; session <= 6; ++session) {
      if (!ProcessCreate(pressured, request++, session) || !ProcessStart(pressured, request++, session, 1, true) ||
          !broker.Observe(++count)) return false;
      HANDLE worker = broker.Worker(count - 1);
      if (!worker || ::WaitForSingleObject(worker, 0) != WAIT_TIMEOUT) return false;
      std::printf("[mapped] broker=%lu conn=%u session=%u run=1 worker=%lu\n",
          ::GetProcessId(broker.process), pressured.hello.conn_id, session, ::GetProcessId(worker));
      for (uint32_t run = 2; run <= 4 && !(session == 6 && run == 4); ++run) {
        const uint32_t admitted = request++;
        if (!pressured.Send(protocol::BuildStartRunFrame(pressured.Header(admitted, session, run), Script(
                "host.postMessage('pending-executed');host.complete();"))) ||
            !pressured.Expect(protocol::MessageType::ACK, admitted, session, run)) return false;
      }
    }
    const size_t cap_bytes = variant == 0 ? cap - 1 : variant == 1 ? cap : cap - 15;
    size_t charged = 0;
    for (unsigned pending = 0; pending < 17; ++pending) {
      const uint32_t session = pending / 3 + 1, run = pending % 3 + 2;
      const size_t bytes = pending < 15 ? protocol::kMaxQueuedRelayBytesPerRun :
          pending == 15 ? protocol::kMaxQueuedRelayBytesPerRun - 100 : cap_bytes - (cap - 100);
      if (!FillProcessInbound(pressured, session, run, bytes, &charged)) return false;
    }
    *detail = "aggregate boundary/control fence variant=" + std::to_string(variant);
    if (charged != cap_bytes || !pressured.Send(protocol::BuildCancelRunFrame(pressured.Header(request, 1, 99))) ||
        !pressured.Expect(protocol::MessageType::ACK, request++, 1, 99)) return false;
    std::printf("[pressure] conn=%u bytes=%zu cap=%zu pending=17 legal-workers=6 variant=%u\n",
        pressured.hello.conn_id, charged, cap, variant);
    if (variant >= 2) {
      auto header = pressured.Header(0, 6, 3); header.type = protocol::MessageType::RELAY_TO_WORKER;
      const size_t rejected = v8host::EncodeRunEnvelope(GuestRelay(0, 3)).size();
      if (charged + rejected != cap + 1 ||
          !pressured.Send(protocol::BuildRelayFrame(header, sbox_msg_kind_binary, nullptr, 0))) return false;
      if (variant == 2) {
        std::vector<uint8_t> bytes; protocol::FrameHeader error_header; protocol::ErrorPayload error;
        if (!pressured.Receive(&bytes, &error_header) || error_header.type != protocol::MessageType::ERROR ||
            error_header.conn_id != pressured.hello.conn_id || error_header.target_id != 6 || error_header.run_id != 3 ||
            error_header.request_id != 0 || error_header.version_major != pressured.hello.selected_major ||
            error_header.version_minor != pressured.hello.selected_minor ||
            !protocol::DecodeErrorPayload(bytes.data() + protocol::kFrameHeaderSize,
                bytes.size() - protocol::kFrameHeaderSize, &error) || error.status_code != protocol::StatusCode::ERROR_QUOTA ||
            !ProcessEof(pressured)) { *detail = "addressed ERROR_QUOTA before real EOF"; return false; }
        std::printf("[quota] conn=%u session=6 run=3 request=0 ERROR_QUOTA before EOF; rejected=%zu\n",
            pressured.hello.conn_id, rejected);
      }
      // The EOF variant does not promise a scheduler-specific writer-drain instant.
    }
    pressured.connection.Close();
    for (size_t index = first; index < count; ++index) {
      if (::WaitForSingleObject(broker.Worker(index), 10000) != WAIT_OBJECT_0) return false;
      std::printf("[reaped] pressure worker=%lu\n", ::GetProcessId(broker.Worker(index)));
    }
    *detail = "aggregate unaffected original PID/fresh admission";
    if (::WaitForSingleObject(survivor, 0) != WAIT_TIMEOUT || ::WaitForSingleObject(broker.process, 0) != WAIT_TIMEOUT ||
        !ProcessEcho(healthy, 1, 1, "survivor-" + std::to_string(variant)) ||
        !ProcessCreate(healthy, 3 + variant * 2, 2 + variant) ||
        !ProcessStart(healthy, 4 + variant * 2, 2 + variant, 1, true) || !broker.Observe(++count)) return false;
  }
  healthy.connection.Close();
  if (!broker.Reaped(count)) return false;
  *detail = "real inbound cap-1/cap/cap+1; addressed quota-before-EOF; six held workers/17 pending; sibling/fresh admission/reap";
  return true;
}

bool RealNoncooperativeFallbackBody(std::string* detail) {
  CapturedBroker broker; ProcessSessionRecord blocked, sibling;
  *detail = "noncooperative real-client marker/admission";
  if (!broker.Launch(detail) || !blocked.Create()) return false;
  const std::string guest = "host.postMessage('entered');for(;;){}";
  v8host::test::TestRunInputs input; input.tier_override = -1;
  input.payload = guest.data(); input.payload_len = guest.size();
  blocked.runs.emplace_back(); blocked.runs[0].token = "entered";
  if (blocked.client.Start(blocked.handle, &input, &blocked.runs[0].handle) != v8host_status_ok ||
      !PumpPublic([&] { return !blocked.runs[0].messages.empty() || blocked.runs[0].terminals || blocked.disconnects; }) ||
      !blocked.valid || blocked.disconnects || blocked.runs[0].terminals ||
      blocked.runs[0].events != std::vector<int32_t>{v8host_run_event_started} ||
      blocked.runs[0].messages != std::vector<std::vector<uint8_t>>{ProcessSessionRecord::Text("entered")} ||
      blocked.runs[0].kinds != std::vector<int32_t>{sbox_msg_kind_string} || !broker.Observe(1)) return false;
  HANDLE worker = broker.Worker(0);
  if (!worker || ::WaitForSingleObject(worker, 0) != WAIT_TIMEOUT || !blocked.Start("pending-never-execute") ||
      !PumpPublic([&] { return !blocked.runs[1].events.empty(); }) || !sibling.Create() ||
      !sibling.Start("survivor") || !MapPublicWorker(broker, sibling, 1)) return false;
  HANDLE healthy = broker.Worker(1);
  const ULONGLONG cancelled = ::GetTickCount64();
  if (blocked.client.cancel(blocked.runs[0].handle) != v8host_status_ok ||
      !blocked.Terminal(0, v8host_run_event_worker_exited, v8host_status_run_terminal) ||
      !blocked.Terminal(1, v8host_run_event_worker_exited, v8host_status_run_terminal) ||
      !blocked.runs[1].messages.empty() || blocked.disconnects ||
      ::WaitForSingleObject(worker, 0) != WAIT_TIMEOUT) { *detail = "fallback must retire once before kernel death"; return false; }
  const ULONGLONG retired = ::GetTickCount64();
  if (retired - cancelled < 4500 || !sibling.Echo(0, "during-reap", 0x31)) return false;
  std::printf("[fallback] broker=%lu worker=%lu entered-before-cancel=1 retired_ms=%llu WORKER_EXITED active+pending once\n",
      ::GetProcessId(broker.process), ::GetProcessId(worker), retired - cancelled);
  // The worker's engine joins its runtime threads for 1 s after the close and
  // then exits by itself (exit 18), long before the container's 30 s timeout.
  *detail = "noncooperative worker self-exit after the bounded runtime join (10-second budget, no harness kill)";
  const ULONGLONG deadline = retired + 10000; ULONGLONG next_echo = retired + 2000;
  while (::WaitForSingleObject(worker, 100) == WAIT_TIMEOUT && ::GetTickCount64() < deadline) {
    DrainPublic();
    if (!blocked.valid || blocked.runs[0].terminals != 1 || blocked.runs[1].terminals != 1 ||
        !blocked.runs[1].messages.empty() || blocked.disconnects || ::WaitForSingleObject(healthy, 0) != WAIT_TIMEOUT ||
        ::WaitForSingleObject(broker.process, 0) != WAIT_TIMEOUT) return false;
    if (::GetTickCount64() >= next_echo) {
      if (!sibling.Echo(0, "reap-progress", 0x32)) return false;
      next_echo += 2000;
    }
  }
  DWORD code = 0;
  if (::WaitForSingleObject(worker, 0) != WAIT_OBJECT_0 || !::GetExitCodeProcess(worker, &code) || code != 18) return false;
  std::printf("[reaped] noncooperative worker=%lu exit=%lu cancel_to_exit_ms=%llu harness_kill=0 budget_ms=10000\n",
      ::GetProcessId(worker), code, ::GetTickCount64() - cancelled);
  for (auto& run : blocked.runs)
    if (blocked.client.post(run.handle, v8host_message_kind_string, "late", 4) != v8host_status_run_terminal ||
        blocked.client.cancel(run.handle) != v8host_status_run_terminal) return false;
  blocked.Close(); const size_t frozen = blocked.CallbackCount();
  v8host_run_s* forbidden = nullptr;
  if (blocked.client.Start(blocked.handle, &input, &forbidden) == v8host_status_ok || forbidden ||
      !sibling.Echo(0, "after-reap", 0x33) || !sibling.Finish(0) || !sibling.Start("fresh-js") ||
      !sibling.Armed(1) || !sibling.Echo(1, "original-pid", 0x34) || !sibling.Finish(1) ||
      !broker.Observe(2) || ::WaitForSingleObject(healthy, 0) != WAIT_TIMEOUT) return false;
  sibling.Close();
  if (!broker.Reaped(2)) return false;
  DrainPublic();
  { std::lock_guard<std::mutex> lock(broker.mutex);
    if (broker.diagnostics.find("[broker] target timed out; terminating") != std::string::npos) return false;
  }
  *detail = "entered marker; once-only WORKER_EXITED at fallback; bounded engine join/self-exit 18; sibling/fresh JS original PID";
  return blocked.valid && sibling.valid && blocked.CallbackCount() == frozen && !sibling.disconnects;
}
bool RealIdleLifecycleBody(std::string* detail) {
  CapturedBroker broker; WireHarness owner, reuse;
  *detail = "live owner/armed worker prevents shared idle exit";
  if (!OpenCaptured(broker, owner, detail) || !ProcessStart(owner, 2, 1, 1, true) || !broker.Observe(1)) return false;
  HANDLE worker = broker.Worker(0);
  if (::WaitForSingleObject(broker.process, 5500) != WAIT_TIMEOUT || ::WaitForSingleObject(worker, 0) != WAIT_TIMEOUT ||
      !ProcessEcho(owner, 1, 1, "owner-alive") ||
      !ConnectAndHandshake(BrokerMode::kShared, 900, &reuse.connection, &reuse.hello, detail) ||
      reuse.connection.broker_pid() != ::GetProcessId(broker.process) || reuse.hello.conn_id == owner.hello.conn_id) return false;
  owner.connection.Close();
  *detail = "worker reaped while other authenticated owner preserves broker";
  if (::WaitForSingleObject(worker, 10000) != WAIT_OBJECT_0 ||
      ::WaitForSingleObject(broker.process, 5500) != WAIT_TIMEOUT || !ProcessCreate(reuse, 1, 1)) return false;
  reuse.connection.Close();
  // Broad grace cancellation, not an attempt to hit the exact cutoff instant.
  if (::WaitForSingleObject(broker.process, 1500) != WAIT_TIMEOUT) return false;
  WireHarness renewed;
  if (!ConnectAndHandshake(BrokerMode::kShared, 900, &renewed.connection, &renewed.hello, detail) ||
      renewed.connection.broker_pid() != ::GetProcessId(broker.process) || !ProcessCreate(renewed, 1, 1) ||
      ::WaitForSingleObject(broker.process, 5500) != WAIT_TIMEOUT ||
      !renewed.Send(protocol::BuildCloseSessionFrame(renewed.Header(2))) ||
      !renewed.Expect(protocol::MessageType::ACK, 2, 1)) return false;
  const ULONGLONG closed = ::GetTickCount64(); renewed.connection.Close();
  *detail = "complete-zero shared idle exit and exact-set replacement";
  if (!broker.Reaped(1) || ::GetTickCount64() - closed < 4900) return false;
  CapturedBroker replacement; WireHarness fresh;
  if (!OpenCaptured(replacement, fresh, detail) || ::GetProcessId(replacement.process) == ::GetProcessId(broker.process) ||
      !ProcessStart(fresh, 2, 1, 1, true) || !replacement.Observe(1) || !ProcessEcho(fresh, 1, 1, "replacement-js")) return false;
  fresh.connection.Close();
  if (!replacement.Reaped(1) || !DedicatedOwnerExit(detail)) return false;
  *detail = "shared reuse/live worker/live owner/grace cancellation/complete-zero exit/replacement; dedicated owner-loss drain";
  return true;
}
bool IdleAcceptBoundary(std::string* detail) {
  for (bool completed : {false, true}) {
    const std::wstring name = L"\\\\.\\pipe\\v8host-idle-accept-" + std::to_wstring(::GetCurrentProcessId());
    HANDLE pipe = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
    OVERLAPPED pending = {}; pending.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE client = INVALID_HANDLE_VALUE;
    bool active = pipe != INVALID_HANDLE_VALUE && pending.hEvent &&
        !::ConnectNamedPipe(pipe, &pending) && ::GetLastError() == ERROR_IO_PENDING;
    bool ok = active && V8HostBrokerPollConnect(pipe, &pending, 0) == V8HostBrokerAcceptStatus::kPending;
    if (ok && completed) {
      client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      ok = client != INVALID_HANDLE_VALUE && ::WaitForSingleObject(pending.hEvent, 3000) == WAIT_OBJECT_0 &&
          V8HostBrokerPollConnect(pipe, &pending, 0) == V8HostBrokerAcceptStatus::kConnected;
    }
    if (ok) {
      const bool admitted = V8HostBrokerFinishPendingConnect(pipe, &pending); active = false;
      DWORD transferred = 0;
      const bool succeeded = ::GetOverlappedResult(pipe, &pending, &transferred, FALSE) != FALSE;
      const DWORD error = succeeded ? ERROR_SUCCESS : ::GetLastError();
      ok = admitted == completed && ::WaitForSingleObject(pending.hEvent, 0) == WAIT_OBJECT_0 &&
          (completed ? succeeded : !succeeded && error == ERROR_OPERATION_ABORTED);
      if (ok && admitted) {
        std::vector<uint8_t> sent = protocol::BuildHelloFrame(protocol::FrameHeader{}), received(512);
        ok = PipeIo(client, true, &sent) && PipeIo(pipe, false, &received) && received == sent;
      }
      std::printf("[accept] completed=%d admitted=%d event_signaled=%d error=%lu real-pipe=1\n",
          completed, admitted, ::WaitForSingleObject(pending.hEvent, 0) == WAIT_OBJECT_0, error);
    }
    if (active) { ::CancelIoEx(pipe, &pending); DWORD bytes; ::GetOverlappedResult(pipe, &pending, &bytes, TRUE); }
    if (client != INVALID_HANDLE_VALUE) ::CloseHandle(client);
    if (pending.hEvent) ::CloseHandle(pending.hEvent);
    if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(pipe);
    if (!ok) { *detail = completed ? "completed accept must win with exact HELLO" : "cancelled pending accept must drain, not admit"; return false; }
  }
  *detail = "pending polls retained; completed accept wins; cancelled accept drained with ERROR_OPERATION_ABORTED";
  return true;
}

struct TopologyPublic {
  struct Worker {
    v8host_worker handle = nullptr;
    std::vector<int32_t> states;
    std::vector<v8host_status> statuses;
    DWORD pid = 0;
    HANDLE process = nullptr;
  };
  struct Runtime {
    v8host_runtime handle = nullptr;
    size_t worker = 0;
    std::vector<int32_t> states;
    std::vector<v8host_status> statuses;
  };
  struct Run {
    v8host_run handle = nullptr;
    size_t runtime = 0;
    std::vector<int32_t> events;
    std::vector<v8host_status> statuses;
    std::vector<std::string> messages;
  };
  struct Event { char kind; size_t index; int32_t value; v8host_status status; };
  ContractCClient& api = PublicClient();
  v8host_broker broker = nullptr;
  HANDLE broker_process = nullptr;
  DWORD thread = ::GetCurrentThreadId();
  std::vector<Worker> workers;
  std::vector<Runtime> runtimes;
  std::vector<Run> runs;
  std::vector<Event> order;
  bool valid = true, disconnected = false;
  int disconnects = 0;
  bool lpac = false;
  uint32_t maximum = 2, flags = 0;
  int snapshot_index = -1;
  const wchar_t* engine = nullptr;
  const wchar_t* snapshot = nullptr;

  template<class T, class H> size_t Find(const std::vector<T>& records, H handle) {
    for (size_t i = 0; i < records.size(); ++i) if (records[i].handle == handle) return i;
    valid = false; return records.size();
  }
  void CheckThread() { valid &= thread == ::GetCurrentThreadId(); }
  static void V8HOST_CDECL BrokerState(void* p, v8host_broker handle,
      v8host_broker_state state, v8host_status status) {
    auto& c = *static_cast<TopologyPublic*>(p); c.CheckThread();
    c.valid &= handle == c.broker;
    if (state == v8host_broker_state_disconnected) { c.disconnected = true; ++c.disconnects; }
    c.order.push_back({'b', 0, state, status});
  }
  static void V8HOST_CDECL WorkerState(void* p, v8host_worker handle,
      v8host_worker_state state, v8host_status status) {
    auto& c = *static_cast<TopologyPublic*>(p); c.CheckThread();
    const auto i = c.Find(c.workers, handle); if (i == c.workers.size()) return;
    auto& w = c.workers[i];
    c.valid &= w.states.empty() || w.states.back() != v8host_worker_state_closed;
    w.states.push_back(state); w.statuses.push_back(status);
    if (state == v8host_worker_state_startup_ready) {
      w.pid = c.api.worker_get_process_id(handle);
      c.valid &= w.pid != 0 && c.api.worker_get_id(handle) != 0;
    }
    if (state == v8host_worker_state_security_ready)
      c.valid &= w.states == std::vector<int32_t>{v8host_worker_state_startup_ready, v8host_worker_state_security_ready} &&
          c.api.worker_get_process_id(handle) == w.pid && status == v8host_status_ok;
    c.order.push_back({'w', i, state, status});
  }
  static void V8HOST_CDECL RuntimeState(void* p, v8host_runtime handle,
      v8host_runtime_state state, v8host_status status) {
    auto& c = *static_cast<TopologyPublic*>(p); c.CheckThread();
    const auto i = c.Find(c.runtimes, handle); if (i == c.runtimes.size()) return;
    auto& r = c.runtimes[i];
    c.valid &= r.states.empty() || r.states.back() != v8host_runtime_state_closed;
    r.states.push_back(state); r.statuses.push_back(status);
    if (state == v8host_runtime_state_ready)
      c.valid &= c.api.runtime_get_id(handle) != 0 && status == v8host_status_ok;
    c.order.push_back({'r', i, state, status});
  }
  static void V8HOST_CDECL RunEvent(void* p, v8host_run handle,
      v8host_run_event event, v8host_status status) {
    auto& c = *static_cast<TopologyPublic*>(p); c.CheckThread();
    const auto i = c.Find(c.runs, handle); if (i == c.runs.size()) return;
    auto& r = c.runs[i];
    c.valid &= r.events.empty() || r.events.back() == v8host_run_event_started;
    r.events.push_back(event); r.statuses.push_back(status);
    c.order.push_back({'e', i, event, status});
  }
  static void V8HOST_CDECL Relay(void* p, v8host_run handle,
      v8host_message_kind kind, const void* data, size_t size) {
    auto& c = *static_cast<TopologyPublic*>(p); c.CheckThread();
    const auto i = c.Find(c.runs, handle); if (i == c.runs.size()) return;
    auto& r = c.runs[i];
    c.valid &= r.events == std::vector<int32_t>{v8host_run_event_started} &&
        kind == v8host_message_kind_string;
    r.messages.emplace_back(static_cast<const char*>(data), size);
    c.order.push_back({'m', i, 0, v8host_status_ok});
  }
  static v8host_status V8HOST_CDECL ConfigureBroker(void* p, v8host_broker_config config) {
    auto& c = *static_cast<TopologyPublic*>(p);
    v8host_broker_callbacks callbacks = V8HOST_BROKER_CALLBACKS_INIT;
    callbacks.user_data = &c; callbacks.on_broker_state = BrokerState;
    callbacks.on_worker_state = WorkerState; callbacks.on_runtime_state = RuntimeState;
    callbacks.on_run_event = RunEvent; callbacks.on_relay_message = Relay;
    auto s = c.api.broker_config_set_mode(config, v8host_broker_mode_dedicated);
    return s == v8host_status_ok ? c.api.broker_config_set_callbacks(config, &callbacks) : s;
  }
  static v8host_status V8HOST_CDECL ConfigureWorker(void* p, v8host_worker_config config) {
    auto& c = *static_cast<TopologyPublic*>(p);
#define TOPOLOGY_SET(call) do { auto s = c.api.call; if (s != v8host_status_ok) return s; } while (false)
    TOPOLOGY_SET(worker_config_set_tier(config, v8host_tier_untrusted));
    TOPOLOGY_SET(worker_config_set_max_runtimes(config, c.maximum));
    TOPOLOGY_SET(worker_config_set_flags(config, static_cast<v8host_worker_flags>(c.flags)));
    TOPOLOGY_SET(worker_config_set_delayed_integrity_level(config, v8host_integrity_level_untrusted));
    if (c.lpac) {
      TOPOLOGY_SET(worker_config_set_use_app_container(config, true));
      TOPOLOGY_SET(worker_config_set_low_privilege_app_container(config, true));
      TOPOLOGY_SET(worker_config_set_profile_name_utf16(config,
          v8host::test::AsChar16(L"v8host.lpac.test"), V8HOST_AUTO_LENGTH));
      TOPOLOGY_SET(worker_config_set_integrity_level(config, v8host_integrity_level_low));
      TOPOLOGY_SET(worker_config_set_delayed_integrity_level(config, v8host_integrity_level_low));
      TOPOLOGY_SET(worker_config_set_lockdown_token_level(config, v8host_token_level_lockdown));
    }
    if (c.engine) TOPOLOGY_SET(worker_config_set_engine_dll_utf16(config,
        v8host::test::AsChar16(c.engine), V8HOST_AUTO_LENGTH));
    if (c.snapshot) TOPOLOGY_SET(worker_config_add_startup_snapshot_path_utf16(config,
        v8host::test::AsChar16(c.snapshot), V8HOST_AUTO_LENGTH));
#undef TOPOLOGY_SET
    return v8host_status_ok;
  }
  static v8host_status V8HOST_CDECL ConfigureRuntime(void* p, v8host_runtime_config config) {
    auto& c = *static_cast<TopologyPublic*>(p);
    return c.api.runtime_config_set_startup_snapshot_index(config, c.snapshot_index);
  }
  bool Open() { return api.Load() && api.broker_open(ConfigureBroker, this, &broker) == v8host_status_ok && broker; }
  bool AddWorker(uint32_t cap = 2, uint32_t lifetime = 0) {
    maximum = cap; flags = lifetime;
    Worker w;
    if (api.worker_open(broker, ConfigureWorker, this, &w.handle) != v8host_status_ok || !w.handle) return false;
    workers.push_back(std::move(w)); return true;
  }
  bool Security(size_t i) {
    return PumpPublic([&] { return (!workers[i].states.empty() &&
        workers[i].states.back() >= v8host_worker_state_security_ready) || disconnected; }, lpac ? 90000 : 15000) &&
        !disconnected && valid && workers[i].states.back() == v8host_worker_state_security_ready;
  }
  bool AddRuntime(size_t worker) {
    Runtime r; r.worker = worker;
    if (api.runtime_open(workers[worker].handle, snapshot_index == -1 ? nullptr : ConfigureRuntime,
        this, &r.handle) != v8host_status_ok || !r.handle) return false;
    runtimes.push_back(std::move(r)); return true;
  }
  bool Ready(size_t i) {
    return PumpPublic([&] { return !runtimes[i].states.empty() || disconnected; }) &&
        valid && !disconnected && runtimes[i].states == std::vector<int32_t>{v8host_runtime_state_ready};
  }
  bool Start(size_t runtime, const std::string& guest) {
    v8host::test::TestRunInputs input; input.payload = guest.data(); input.payload_len = guest.size();
    v8host::test::RunConfigure config{&api.config_api, &input};
    Run r; r.runtime = runtime;
    if (api.runtime_start_run(runtimes[runtime].handle, v8host::test::ConfigureRun, &config, &r.handle) !=
        v8host_status_ok || !r.handle) return false;
    runs.push_back(std::move(r)); return true;
  }
  bool Started(size_t i) {
    return PumpPublic([&] { return !runs[i].messages.empty() || disconnected; }) && valid && !disconnected &&
        runs[i].events == std::vector<int32_t>{v8host_run_event_started};
  }
  bool Post(size_t i, const std::string& text = "release") {
    return api.post(runs[i].handle, v8host_message_kind_string, text.data(), text.size()) == v8host_status_ok;
  }
  bool Terminal(size_t i, v8host_run_event event = v8host_run_event_completed,
      v8host_status status = v8host_status_ok) {
    return PumpPublic([&] { return runs[i].events.size() >= 2 || disconnected; }, 15000) && valid &&
        runs[i].events == std::vector<int32_t>{v8host_run_event_started, event} &&
        runs[i].statuses == std::vector<v8host_status>{v8host_status_ok, status};
  }
  bool HoldProcesses() {
    HANDLE snapshot_handle = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot_handle == INVALID_HANDLE_VALUE) return false;
    std::vector<PROCESSENTRY32W> entries;
    PROCESSENTRY32W entry = {}; entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot_handle, &entry)) do { entries.push_back(entry); }
      while (::Process32NextW(snapshot_handle, &entry));
    ::CloseHandle(snapshot_handle);
    DWORD broker_pid = 0;
    for (const auto& e : entries) if (e.th32ParentProcessID == ::GetCurrentProcessId() &&
        ::_wcsicmp(e.szExeFile, L"sbox.exe") == 0) {
      if (broker_pid) return false;
      broker_pid = e.th32ProcessID;
    }
    if (!broker_pid) return false;
    broker_process = ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, broker_pid);
    if (!broker_process || ::WaitForSingleObject(broker_process, 0) != WAIT_TIMEOUT) return false;
    for (auto& w : workers) {
      bool owned = false;
      for (const auto& e : entries) owned |= e.th32ProcessID == w.pid && e.th32ParentProcessID == broker_pid;
      if (!owned) return false;
      w.process = ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, w.pid);
      if (!w.process || ::WaitForSingleObject(w.process, 0) != WAIT_TIMEOUT) return false;
    }
    return true;
  }
  bool CloseAndReap() {
    if (broker) { api.broker_close(broker); broker = nullptr; }
    bool reaped = true;
    for (auto& w : workers) if (w.process) reaped &= ::WaitForSingleObject(w.process, 15000) == WAIT_OBJECT_0;
    if (broker_process) reaped &= ::WaitForSingleObject(broker_process, 15000) == WAIT_OBJECT_0;
    return reaped;
  }
  ~TopologyPublic() {
    CloseAndReap();
    for (auto& r : runtimes) if (r.handle) api.runtime_close(r.handle);
    for (auto& w : workers) {
      if (w.handle) api.worker_close(w.handle);
      if (w.process) ::CloseHandle(w.process);
    }
    if (broker_process) ::CloseHandle(broker_process);
  }
};

std::string TopologyBarrier(const std::string& name) {
  return "if(typeof topologyGlobal!=='undefined')throw Error('global leaked');"
      "globalThis.topologyGlobal='" + name + "';host.postMessage('waiting:" + name + "');"
      "host.onmessage=function(m){if(m!=='release')throw Error('barrier');"
      "for(var i=0;i<100000;i++){}host.postMessage('done:'+topologyGlobal);host.complete();};";
}
bool PublicTopologyJs(std::string* detail, const std::string& name) {
  TopologyPublic c; c.lpac = name == "lpac-worker-token";
  *detail = "open explicit broker/worker and await security";
  if (!c.Open() || !c.AddWorker() || !c.Security(0)) return false;
  if (name == "public-two-runtime-js") {
    // This query deliberately precedes the first runtime open.
    HANDLE process = ::OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, c.workers[0].pid);
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy = {};
    const bool acg = process && ::GetProcessMitigationPolicy(process, ProcessDynamicCodePolicy, &policy, sizeof(policy)) &&
        policy.ProhibitDynamicCode == 1;
    const DWORD error = acg ? ERROR_SUCCESS : ::GetLastError();
    if (process) ::CloseHandle(process);
    if (!acg) { *detail = "pre-runtime ACG query error=" + std::to_string(error); return false; }
    std::printf("[public topology] security_ready; query access=0x400 ACG=1 before runtime_open\n");
  }
  *detail = "open isolated runtimes";
  if (!c.AddRuntime(0) || !c.AddRuntime(0) || !c.Ready(0) || !c.Ready(1) || !c.HoldProcesses()) return false;
  if (name == "public-runtime-overrides") {
    for (int field = 0; field < 3; ++field) {
      v8host::test::TestRunInputs inputs;
      if (field == 0) inputs.tier_override = v8host_tier_untrusted;
      if (field == 1) inputs.engine_dll_override = L"v8jsisb.dll";
      if (field == 2) inputs.startup_snapshot_path = L"not-used.blob";
      v8host::test::RunConfigure config{&c.api.config_api, &inputs};
      v8host_run run = reinterpret_cast<v8host_run>(uintptr_t{1});
      if (c.api.runtime_start_run(c.runtimes[0].handle, v8host::test::ConfigureRun, &config, &run) !=
          v8host_status_profile_already_bound || run) return false;
    }
  }
  *detail = "two started runs before release, independent globals and ordered relays";
  if (!c.Start(0, TopologyBarrier("alpha")) || !c.Start(1, TopologyBarrier("bravo")) ||
      !c.Started(0) || !c.Started(1) || !c.Post(0) || !c.Post(1) ||
      !c.Terminal(0) || !c.Terminal(1) ||
      c.runs[0].messages != std::vector<std::string>{"waiting:alpha", "done:alpha"} ||
      c.runs[1].messages != std::vector<std::string>{"waiting:bravo", "done:bravo"}) return false;
  // A later run uses the same callbacks and the ordinary post/cancel entry points.
  if (!c.Start(0, "host.postMessage(topologyGlobal);host.onmessage=function(m){host.postMessage(m);};") ||
      !c.Started(2) || !c.Post(2, "echo") ||
      !PumpPublic([&] { return c.runs[2].messages.size() == 2; }) ||
      c.runs[2].messages != std::vector<std::string>{"alpha", "echo"} ||
      c.api.cancel(c.runs[2].handle) != v8host_status_ok ||
      !c.Terminal(2, v8host_run_event_cancelled) ||
      c.api.post(c.runs[2].handle, v8host_message_kind_string, "late", 4) != v8host_status_run_terminal) return false;
  return c.valid && c.CloseAndReap();
}

bool PublicTopologyShapes(std::string* detail) {
  // Each shape is fenced by guest messages, not a timing comparison.
  for (const std::vector<size_t>& shape : {std::vector<size_t>{0, 0}, {0, 1}, {0, 0, 1}}) {
    TopologyPublic c;
    *detail = "shape workers/runtimes and live PID ownership";
    if (!c.Open()) return false;
    const size_t count = *std::max_element(shape.begin(), shape.end()) + 1;
    for (size_t i = 0; i < count; ++i) if (!c.AddWorker() || !c.Security(i)) return false;
    if (!c.HoldProcesses()) return false;
    std::vector<DWORD> pids;
    for (const auto& w : c.workers) pids.push_back(w.pid);
    std::sort(pids.begin(), pids.end());
    if (std::adjacent_find(pids.begin(), pids.end()) != pids.end()) return false;
    for (size_t i = 0; i < shape.size(); ++i) {
      if (!c.AddRuntime(shape[i]) || !c.Ready(i) ||
          !c.Start(i, TopologyBarrier(std::to_string(i)))) return false;
    }
    *detail = "every run started before any RESULT, then release all";
    for (size_t i = 0; i < shape.size(); ++i)
      if (!c.Started(i) || c.api.worker_get_process_id(c.workers[c.runtimes[i].worker].handle) !=
          c.workers[shape[i]].pid) return false;
    for (const auto& r : c.runs) if (r.events != std::vector<int32_t>{v8host_run_event_started}) return false;
    for (size_t i = 0; i < shape.size(); ++i) if (!c.Post(i)) return false;
    for (size_t i = 0; i < shape.size(); ++i)
      if (!c.Terminal(i) || c.runs[i].messages != std::vector<std::string>{
          "waiting:" + std::to_string(i), "done:" + std::to_string(i)}) return false;
    if (!c.valid || !c.CloseAndReap()) return false;
  }
  return true;
}

bool PublicTopologyIdentity(std::string* detail) {
  TopologyPublic c;
  *detail = "three distinct live owned worker PIDs; both slots retain their worker PID";
  if (!c.Open()) return false;
  for (size_t i = 0; i < 3; ++i) if (!c.AddWorker() || !c.Security(i)) return false;
  if (!c.HoldProcesses()) return false;
  std::vector<DWORD> pids;
  std::vector<uint32_t> ids;
  for (const auto& w : c.workers) { pids.push_back(w.pid); ids.push_back(c.api.worker_get_id(w.handle)); }
  std::sort(pids.begin(), pids.end()); std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(pids.begin(), pids.end()) != pids.end() ||
      std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return false;
  const std::array<size_t, 4> owners{0, 0, 1, 2};
  for (size_t i = 0; i < owners.size(); ++i) {
    const auto worker = owners[i];
    if (!c.AddRuntime(worker) || !c.Ready(i) || !c.Start(i, TopologyBarrier(std::to_string(i))) ||
        !c.Started(i) || c.api.worker_get_process_id(c.workers[worker].handle) != c.workers[worker].pid ||
        ::WaitForSingleObject(c.workers[worker].process, 0) != WAIT_TIMEOUT) return false;
  }
  for (size_t i = 0; i < owners.size(); ++i) if (!c.Post(i) || !c.Terminal(i)) return false;
  for (const auto& w : c.workers) if (::WaitForSingleObject(w.process, 0) != WAIT_TIMEOUT) return false;
  return c.valid && c.CloseAndReap();
}

bool PublicTopologyLoss(std::string* detail, bool broker_kill, bool runaway = false) {
  TopologyPublic c;
  *detail = "owned broker, two workers, three live runtimes";
  if (!c.Open() || !c.AddWorker() || !c.AddWorker() || !c.Security(0) || !c.Security(1) ||
      !c.AddRuntime(0) || !c.AddRuntime(0) || !c.AddRuntime(1) ||
      !c.Ready(0) || !c.Ready(1) || !c.Ready(2) || !c.HoldProcesses()) return false;
  for (size_t i = 0; i < 3; ++i)
    if (!c.Start(i, TopologyBarrier(std::to_string(i))) || !c.Started(i)) return false;
  const size_t begin = c.order.size();
  *detail = "live-run loss fan-out once, runs before runtimes before workers";
  if (runaway) {
    if (!c.Start(0, "host.postMessage('running');while(true){}")) return false;
    // The first run still owns the slot; finish it to admit the runaway.
    if (!c.Post(0) || !c.Terminal(0) || !c.Started(3) ||
        c.api.cancel(c.runs[3].handle) != v8host_status_ok) return false;
  } else {
    HANDLE killed = broker_kill ? c.broker_process : c.workers[0].process;
    if (!::TerminateProcess(killed, broker_kill ? 0xbeef : 0xfa17) ||
        ::WaitForSingleObject(killed, 10000) != WAIT_OBJECT_0) return false;
  }
  const auto run_event = broker_kill ? v8host_run_event_failed : v8host_run_event_worker_exited;
  const auto run_status = broker_kill ? v8host_status_broker_lost : v8host_status_run_terminal;
  const auto close_status = broker_kill ? v8host_status_broker_lost : v8host_status_worker_exited;
  for (size_t i = runaway ? 1 : 0; i < (broker_kill ? 3u : 2u); ++i)
    if (!c.Terminal(i, run_event, run_status)) return false;
  if (runaway && !c.Terminal(3, run_event, run_status)) return false;
  if (!PumpPublic([&] { return broker_kill ? c.disconnected :
      c.workers[0].states.back() == v8host_worker_state_closed; }, 15000)) return false;
  int last_phase = 0;
  std::vector<size_t> ended_runs, ended_runtimes, ended_workers;
  for (size_t i = begin; i < c.order.size(); ++i) {
    const auto& e = c.order[i];
    if (runaway && ((e.kind == 'e' && (e.index == 0 || e.value == v8host_run_event_started)) || e.kind == 'm')) continue;
    int phase = e.kind == 'e' ? 1 : e.kind == 'r' ? 2 : e.kind == 'w' ? 3 : e.kind == 'b' ? 4 : 0;
    if (!phase || phase < last_phase || e.status != (phase == 1 ? run_status : close_status)) return false;
    last_phase = phase;
    (phase == 1 ? ended_runs : phase == 2 ? ended_runtimes : ended_workers).push_back(e.index);
  }
  std::sort(ended_runs.begin(), ended_runs.end());
  if (ended_runs != (broker_kill ? std::vector<size_t>{0, 1, 2} :
      runaway ? std::vector<size_t>{1, 3} : std::vector<size_t>{0, 1}) ||
      ended_runtimes.size() != (broker_kill ? 3u : 2u) ||
      ended_workers.size() != (broker_kill ? 3u : 1u) || c.disconnects != (broker_kill ? 1 : 0)) return false;
  for (size_t i = 0; i < (broker_kill ? 2u : 1u); ++i)
    if (::WaitForSingleObject(c.workers[i].process, 10000) != WAIT_OBJECT_0) return false;
  if (!broker_kill) {
    if (!c.Post(2) || !c.Terminal(2) || c.runs[2].messages !=
        std::vector<std::string>{"waiting:2", "done:2"}) return false;
  } else {
    const auto frozen = c.order.size();
    for (size_t i = 0; i < 3; ++i) {
      if (c.api.cancel(c.runs[i].handle) != v8host_status_run_terminal ||
          c.api.post(c.runs[i].handle, v8host_message_kind_string, "no replay", 9) != v8host_status_run_terminal) return false;
      v8host_run run = nullptr;
      if (c.api.runtime_start_run(c.runtimes[i].handle, nullptr, nullptr, &run) != v8host_status_invalid_state || run)
        return false;
    }
    if (!c.CloseAndReap()) return false;
    TopologyPublic replacement;
    if (!replacement.Open() || !replacement.AddWorker() || !replacement.Security(0) ||
        !replacement.AddRuntime(0) || !replacement.Ready(0) || !replacement.HoldProcesses() ||
        !replacement.Start(0, "host.postMessage('replacement');host.complete();") ||
        !replacement.Terminal(0) || replacement.runs[0].messages != std::vector<std::string>{"replacement"} ||
        c.order.size() != frozen || !replacement.CloseAndReap()) return false;
  }
  return c.valid && c.CloseAndReap();
}

bool TopologyRawQuotaComposition(std::string* detail) {
  *detail = "raw shared session/explicit admission and deferred replacement at 32 live processes";
  TopologyFixture f;
  f.early = true;
  f.wait_release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!f.wait_release) return false;
  // A private session worker and explicit workers debit the same admission pool.
  if (!f.Create() || !f.Start(1)) return false;
  DrainLocal(f.conn);
  protocol::CreateWorkerPayload config;
  config.session = LogicalConfig(); config.max_runtimes = 1;
  for (uint32_t id = 2; id <= 32; ++id) {
    if (!f.Reply(protocol::BuildCreateWorkerFrame(TopologyHeader(++f.request, id), config),
        protocol::MessageType::ACK)) return false;
    DrainLocal(f.conn);
  }
  if (f.spawns != 32 || !f.Reply(protocol::BuildCreateWorkerFrame(TopologyHeader(++f.request, 33), config),
      protocol::MessageType::ERROR, protocol::StatusCode::ERROR_QUOTA)) return false;
  if (!f.Reply(protocol::BuildCloseWorkerFrame(TopologyHeader(++f.request, 2)), protocol::MessageType::ACK) ||
      f.conn.retired_workers.size() != 1 ||
      !f.Reply(protocol::BuildCreateWorkerFrame(TopologyHeader(++f.request, 34), config),
          protocol::MessageType::ACK) || f.spawns != 32 ||
      f.conn.workers.at(34)->state != router::WorkerState::kNone ||
      !f.Reply(protocol::BuildCreateWorkerFrame(TopologyHeader(++f.request, 35), config),
          protocol::MessageType::ERROR, protocol::StatusCode::ERROR_QUOTA)) return false;
  ::SetEvent(f.wait_release);
  if (::WaitForSingleObject(f.conn.retired_workers[0]->cleanup, 5000) != WAIT_OBJECT_0) return false;
  f.service->Pump(f.conn);
  DrainLocal(f.conn);
  return f.conn.retired_workers.empty() && f.spawns == 33 && !f.post_after_close && !f.close_before_removal;
}

bool PublicDefaultToken(std::string* detail) {
  TopologyPublic c;
  *detail = "default initial token starts worker; lockdown and ACG remain enabled";
  return c.Open() && c.AddWorker() && c.Security(0) && c.AddRuntime(0) && c.Ready(0) &&
      c.HoldProcesses() && c.Start(0, "host.postMessage('default-token');host.complete();") &&
      c.Terminal(0) && c.runs[0].messages == std::vector<std::string>{"default-token"} &&
      c.CloseAndReap();
}

bool PublicTopologyQuotas(std::string* detail) {
  if (!TopologyRawQuotaComposition(detail)) return false;
  TopologyPublic c;
  *detail = "public max16 runtime quota, 4/5 in-flight runs, close-at-cap";
  if (!c.Open() || !c.AddWorker(16) || !c.Security(0)) return false;
  for (size_t i = 0; i < 16; ++i) if (!c.AddRuntime(0) || !c.Ready(i)) return false;
  if (!c.HoldProcesses()) return false;
  v8host_runtime extra = reinterpret_cast<v8host_runtime>(uintptr_t{1});
  if (c.api.runtime_open(c.workers[0].handle, nullptr, nullptr, &extra) != v8host_status_quota_exceeded || extra) return false;
  for (size_t i = 0; i < 4; ++i)
    if (!c.Start(0, "host.postMessage('active');host.onmessage=function(){host.complete();};")) return false;
  if (!c.Started(0)) return false;
  v8host_run fifth = reinterpret_cast<v8host_run>(uintptr_t{1});
  if (c.api.runtime_start_run(c.runtimes[0].handle, nullptr, nullptr, &fifth) != v8host_status_quota_exceeded || fifth)
    return false;
  // Closing the local handle admits a replacement; the coordinator must wait
  // for the occupied physical slot rather than creating a seventeenth isolate.
  c.api.runtime_close(c.runtimes[15].handle); c.runtimes[15].handle = nullptr;
  if (!c.AddRuntime(0) || !c.Ready(16) ||
      !c.Start(16, "host.postMessage('replacement-slot');host.complete();") || !c.Terminal(4) ||
      c.runs[4].messages != std::vector<std::string>{"replacement-slot"}) return false;
  for (size_t i = 0; i < 4; ++i) {
    if (!c.Started(i) || !c.Post(i) || !c.Terminal(i)) return false;
  }

  if (!c.CloseAndReap()) return false;
  // single_use counts lifetime admissions, even after local close.
  TopologyPublic once;
  *detail = "single_use lifetime cap survives close";
  if (!once.Open() || !once.AddWorker(1, v8host_worker_flags_single_use) || !once.Security(0) ||
      !once.AddRuntime(0) || !once.Ready(0) || !once.HoldProcesses()) return false;
  once.api.runtime_close(once.runtimes[0].handle); once.runtimes[0].handle = nullptr;
  extra = reinterpret_cast<v8host_runtime>(uintptr_t{1});
  return once.api.runtime_open(once.workers[0].handle, nullptr, nullptr, &extra) == v8host_status_quota_exceeded &&
      !extra && once.valid && once.CloseAndReap();
}

template<class Predicate> bool WaitPublicReader(Predicate done, DWORD timeout = 15000) {
  const auto deadline = ::GetTickCount64() + timeout;
  while (!done()) {
    if (::GetTickCount64() >= deadline) return false;
    ::Sleep(1);
  }
  return true;
}

bool PublicTopologyCallbackComposition(std::string* detail) {
  *detail = "explicit-runtime connection ledger and shared worker mailbox compositions";
  {
    TopologyFixture f;
    if (!f.Worker(16)) return false;
    f.Phase(1); DrainLocal(f.conn); f.Phase(2); DrainLocal(f.conn);
    std::vector<router::OutputCredit> credits;
    for (uint32_t id = 2; id < 18; ++id) {
      *detail = "connection-ledger runtime=" + std::to_string(id);
      if (!f.Runtime(id) || !f.Ready(static_cast<uint16_t>(id - 2), id) || !f.StartRuntime(id)) return false;
      DrainLocal(f.conn);
      auto ledger = f.conn.sessions.at(id)->runs.at(1)->outbound;
      auto credit = f.conn.ChargeOutput(protocol::kMaxQueuedRelayBytesPerRun, ledger);
      if (!credit.conn || ledger->bytes != protocol::kMaxQueuedRelayBytesPerRun) return false;
      credits.push_back(std::move(credit));
    }
    *detail = "connection-ledger exact16MiB/+1 and release";
    if (f.conn.out_relay_bytes != protocol::kMaxQueuedRelayBytesPerConnection) return false;
    auto overflow = f.conn.ChargeOutput(1, std::make_shared<router::RelayLedger>());
    if (overflow.conn || ::WaitForSingleObject(f.conn.stop, 0) != WAIT_OBJECT_0) return false;
    credits.clear(); f.service->Close(f.conn); f.conn.ClearOutput();
    if (!f.conn.CompleteZero()) return false;
  }
  for (bool bytes_boundary : {false, true}) for (bool overflow : {false, true}) {
    *detail = "explicit mailbox bytes=" + std::to_string(bytes_boundary) + " overflow=" + std::to_string(overflow);
    TopologyFixture f;
    if (!f.OpenTwo() || !f.StartRuntime(2) || !f.StartRuntime(3)) return false;
    DrainLocal(f.conn);
    size_t remaining = bytes_boundary ? protocol::kMaxQueuedRelayBytesPerRun : 256;
    size_t n = 0;
    while (remaining) {
      size_t part = bytes_boundary ? (std::min)(remaining, size_t(protocol::kMaxFramePayload)) : 1;
      auto env = GuestRelay(bytes_boundary ? part - 16 : 1);
      env.slot = static_cast<uint16_t>(n++ % 2);
      auto encoded = v8host::EncodeRunEnvelope(env);
      router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, encoded.data(), encoded.size());
      remaining -= part;
    }
    if (f.worker->mailbox_failed) { *detail += " failed at exact cap"; return false; }
    if (overflow) {
      auto env = GuestRelay(1); env.slot = 1;
      auto encoded = v8host::EncodeRunEnvelope(env);
      router::Worker::OnMessage(f.worker, sbox_msg_kind_binary, encoded.data(), encoded.size());
    }
    if (f.worker->mailbox_failed != overflow) { *detail += " sticky failure mismatch"; return false; }
    f.service->Pump(f.conn);
    if (f.conn.workers.empty() != overflow) { *detail += " worker closure mismatch"; return false; }
    f.service->Close(f.conn); f.conn.ClearOutput();
    if (!f.conn.CompleteZero() || f.close_before_removal || f.post_after_close) return false;
  }
  {
    TopologyPublic c;
    *detail = "complete-stack runtime callback overflow, sibling survives";
    if (!c.Open() || !c.AddWorker() || !c.Security(0) || !c.AddRuntime(0) ||
        !c.AddRuntime(0) || !c.Ready(0) || !c.Ready(1) || !c.HoldProcesses() ||
        !c.Start(0, "host.postMessage('waiting');host.onmessage=function(){"
            "for(var i=0;i<300;i++)host.postMessage('burst:'+i);host.complete();};") ||
        !c.Start(1, TopologyBarrier("sibling")) || !c.Started(0) || !c.Started(1)) return false;
    const size_t fence = c.order.size();
    if (!c.Post(0) || !WaitPublicReader([&] { return c.api.runtime_get_id(c.runtimes[0].handle) == 0; }) ||
        !PumpPublic([&] { return c.runtimes[0].states.back() == v8host_runtime_state_closed; }) ||
        c.runtimes[0].statuses.back() != v8host_status_callback_backpressure ||
        c.runs[0].events != std::vector<int32_t>{v8host_run_event_started} ||
        c.runs[0].messages != std::vector<std::string>{"waiting"} ||
        c.order.size() != fence + 1 || c.disconnected || !c.Post(1) || !c.Terminal(1) ||
        c.api.cancel(c.runs[0].handle) != v8host_status_run_terminal || !c.CloseAndReap()) return false;
  }
  {
    TopologyPublic c;
    *detail = "complete-stack broker callback cap across 18 runtimes, sticky terminal";
    // The broker's 256-fact worker mailbox fails closed when the router falls
    // behind; nothing paces a guest burst to it. One relay per host post keeps
    // each runtime within the 8-post credit window: at most 16 relays and 8
    // CREDIT frames, so 9 runtimes stay under the cap however the broker is
    // scheduled. 18 x 240 relays still pass the 4096-callback broker cap.
    if (!c.Open() || !c.AddWorker(9) || !c.AddWorker(9) || !c.Security(0) || !c.Security(1)) return false;
    for (size_t i = 0; i < 18; ++i)
      if (!c.AddRuntime(i < 9 ? 0 : 1) || !c.Ready(i) ||
          !c.Start(i, "host.postMessage('waiting');host.onmessage=function(){host.postMessage('b');};") ||
          !c.Started(i)) return false;
    if (!c.HoldProcesses()) return false;
    const size_t fence = c.order.size();
    bool capped = false;  // the cap may trip before the last post
    for (int post = 0; post < 240 && !capped; ++post)
      for (size_t i = 0; i < 18 && !capped; ++i)
        if (!c.Post(i)) {
          if (c.api.worker_get_id(c.workers[0].handle) != 0) return false;
          capped = true;
        }
    if (!WaitPublicReader([&] { return c.api.worker_get_id(c.workers[0].handle) == 0; }) ||
        !PumpPublic([&] { return c.disconnected; }) || c.disconnects != 1 || c.order.size() != fence + 1 ||
        c.order.back().kind != 'b' || c.order.back().status != v8host_status_callback_backpressure) return false;
    for (const auto& run : c.runs) if (run.events != std::vector<int32_t>{v8host_run_event_started} ||
        run.messages != std::vector<std::string>{"waiting"}) return false;
    if (!c.valid || !c.CloseAndReap()) return false;
  }
  return true;
}

bool PublicTopologyCloseAndRunaway(std::string* detail) {
  {
    TopologyPublic c;
    *detail = "close queued parent graph suppresses all callbacks; later child close";
    if (!c.Open() || !c.AddWorker() || !c.AddRuntime(0) || !c.Start(0, "host.complete();")) return false;
    c.api.broker_close(c.broker); c.broker = nullptr;
    if (c.api.worker_get_id(c.workers[0].handle) || c.api.runtime_get_id(c.runtimes[0].handle)) return false;
    c.api.worker_close(c.workers[0].handle); c.workers[0].handle = nullptr;
    c.api.runtime_close(c.runtimes[0].handle); c.runtimes[0].handle = nullptr;
    TopologyPublic fence;
    if (!fence.Open() || !fence.AddWorker() || !fence.Security(0) || !fence.AddRuntime(0) ||
        !fence.Ready(0) || !fence.HoldProcesses() || !fence.Start(0, "host.complete();") ||
        !fence.Terminal(0) || !c.order.empty() || !fence.CloseAndReap()) return false;
  }
  return PublicTopologyLoss(detail, false, true);
}

bool PublicTopologyRefusal(std::string* detail) {
  for (bool missing_engine : {false, true}) {
  TopologyPublic c;
  *detail = missing_engine ? "spawn ACK then WORKER_EXIT with queued children" :
      "refused profile closes queued runtimes before worker";
  if (!c.Open()) return false;
  if (missing_engine) c.engine = L"missing-topology-engine.dll";
  else c.snapshot = L"..\\outside-topology.blob";
  if (!c.AddWorker() || !c.AddRuntime(0) || !c.AddRuntime(0) ||
      !PumpPublic([&] { return !c.workers[0].states.empty(); })) return false;
  const auto status = missing_engine ? v8host_status_worker_exited : v8host_status_protocol_error;
  if (c.workers[0].states != std::vector<int32_t>{v8host_worker_state_closed} ||
      c.workers[0].statuses != std::vector<v8host_status>{status} ||
      c.runtimes[0].states != std::vector<int32_t>{v8host_runtime_state_closed} ||
      c.runtimes[1].states != std::vector<int32_t>{v8host_runtime_state_closed} ||
      c.runtimes[0].statuses != c.workers[0].statuses || c.runtimes[1].statuses != c.workers[0].statuses) return false;
  std::vector<char> fanout;
  for (const auto& e : c.order) if (e.kind != 'b') fanout.push_back(e.kind);
  if (fanout != std::vector<char>{'r', 'r', 'w'} || c.disconnected) return false;
  c.engine = nullptr; c.snapshot = nullptr;
  if (!c.AddWorker() || !c.Security(1) || !c.AddRuntime(1) || !c.Ready(2) ||
      !c.Start(2, "host.postMessage('survived refusal');host.complete();") || !c.Terminal(0) ||
      c.runs[0].messages != std::vector<std::string>{"survived refusal"}) return false;
  if (!c.valid || !c.CloseAndReap()) return false;
  }
  return true;
}

struct ChildResult {
  bool launched = false, in_owned_job = false, exited = false, empty = false, overflow = false;
  bool forced = false, drained = false;
  DWORD code = 1, error = ERROR_SUCCESS;
  std::string output;
};
ChildResult RunOwnedChild(const std::wstring& directory, const char* name,
                         DWORD watchdog = 120000, bool fail_create = false,
                         const std::wstring& desktop = std::wstring()) {
  ChildResult result;
  HANDLE job = ::CreateJobObjectW(nullptr, nullptr), read = nullptr, write = nullptr;
  LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
  struct Cleanup {
    HANDLE& job; HANDLE& read; HANDLE& write; LPPROC_THREAD_ATTRIBUTE_LIST& attributes;
    ~Cleanup() {
      if (attributes) { ::DeleteProcThreadAttributeList(attributes); ::HeapFree(::GetProcessHeap(), 0, attributes); }
      for (HANDLE h : {write, read, job}) if (h) ::CloseHandle(h);
    }
  } cleanup{job, read, write, attributes};
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
  if (!job || !::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
      !::CreatePipe(&read, &write, &sa, 0) || !::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0)) {
    result.error = ::GetLastError(); return result;
  }
  SIZE_T bytes = 0;
  ::InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
  auto* storage = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(::HeapAlloc(::GetProcessHeap(), 0, bytes));
  if (!storage) { result.error = ERROR_NOT_ENOUGH_MEMORY; return result; }
  if (!::InitializeProcThreadAttributeList(storage, 2, 0, &bytes)) {
    result.error = ::GetLastError(); ::HeapFree(::GetProcessHeap(), 0, storage); return result;
  }
  attributes = storage;
  if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job, sizeof(job), nullptr, nullptr) ||
      !::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write, sizeof(write), nullptr, nullptr)) {
    result.error = ::GetLastError(); return result;
  }
  STARTUPINFOEXW startup = {}; startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = startup.StartupInfo.hStdInput = write;
  std::wstring desktop_name = desktop;
  if (!desktop_name.empty()) startup.StartupInfo.lpDesktop = desktop_name.data();
  startup.lpAttributeList = attributes;
  const std::wstring exe = directory + L"\\v8host_broker_tests.exe";
  std::wstring command = L"\"" + exe + L"\" --public-child=" + std::wstring(name, name + std::strlen(name));
  PROCESS_INFORMATION pi = {};
  // The test-only seam fails before creating any process.
  result.launched = !fail_create && ::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
      EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(), &startup.StartupInfo, &pi);
  result.error = fail_create ? ERROR_CANCELLED : result.launched ? ERROR_SUCCESS : ::GetLastError();
  ::CloseHandle(write); write = nullptr;
  auto empty = [&] {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting = {};
    return ::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting,
        sizeof(accounting), nullptr) && accounting.ActiveProcesses == 0;
  };
  if (!result.launched) { result.empty = empty(); return result; }
  BOOL in_job = FALSE;
  result.in_owned_job = ::IsProcessInJob(pi.hProcess, job, &in_job) && in_job;
  ::CloseHandle(pi.hThread);
  std::thread reader([&] {
    char buffer[4096]; DWORD count = 0;
    while (::ReadFile(read, buffer, sizeof(buffer), &count, nullptr) && count) {
      if (result.output.size() + count <= 1024 * 1024) result.output.append(buffer, count);
      else result.overflow = true;
    }
  });
  result.exited = ::WaitForSingleObject(pi.hProcess, watchdog) == WAIT_OBJECT_0;
  // Process signaling can precede job accounting retirement. Allow natural drain
  // before deciding whether descendants leaked; forced cleanup never counts as success.
  const ULONGLONG natural_deadline = ::GetTickCount64() + 5000;
  while (result.exited && !empty() && ::GetTickCount64() < natural_deadline) ::Sleep(10);
  result.empty = empty();
  if (!result.exited || !result.empty) {
    result.forced = true;
    ::TerminateJobObject(job, 1);
    ::WaitForSingleObject(pi.hProcess, 5000);
    const ULONGLONG deadline = ::GetTickCount64() + 5000;
    while (!empty() && ::GetTickCount64() < deadline) ::Sleep(10);
  }
  result.drained = empty();
  ::GetExitCodeProcess(pi.hProcess, &result.code); ::CloseHandle(pi.hProcess);
  // Closing the owned job also closes any remaining writers before joining.
  ::CloseHandle(job); job = nullptr;
  reader.join();
  if (!result.drained) result.output += "\n[cleanup] owned job did not drain\n";
  return result;
}
bool ChildPassed(const ChildResult& result, const char* name) {
  const std::string marker = std::string("[public-child-result] ") + name + ": PASS\n";
  std::string output;
  for (size_t i = 0; i < result.output.size(); ++i) {
    if (result.output[i] == '\r' && i + 1 < result.output.size() && result.output[i + 1] == '\n') continue;
    output += result.output[i];
  }
  const auto at = output.find(marker);
  return result.launched && result.in_owned_job && result.exited && result.empty &&
      result.drained && !result.forced && !result.overflow && result.code == 0 &&
      result.output.find("FAIL") == std::string::npos && result.output.find("[cleanup]") == std::string::npos &&
      at != std::string::npos && (at == 0 || output[at - 1] == '\n') &&
      output.find(marker, at + marker.size()) == std::string::npos;
}
bool LpacReadable(const std::wstring& path) {
  PSID sid = nullptr;
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_APP_PACKAGE_AUTHORITY;
  if (!::AllocateAndInitializeSid(&authority, 2, SECURITY_APP_PACKAGE_BASE_RID,
      SECURITY_BUILTIN_PACKAGE_ANY_RESTRICTED_PACKAGE, 0, 0, 0, 0, 0, 0, &sid)) return false;
  PACL acl = nullptr; PSECURITY_DESCRIPTOR security = nullptr;
  const DWORD error = ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
      nullptr, nullptr, &acl, nullptr, &security);
  bool allowed = false, denied = false;
  if (error == ERROR_SUCCESS && acl) {
    for (DWORD i = 0; i < acl->AceCount; ++i) {
      void* ace = nullptr;
      if (!::GetAce(acl, i, &ace)) { denied = true; break; }
      const auto* header = static_cast<ACE_HEADER*>(ace);
      if (header->AceFlags & INHERIT_ONLY_ACE) continue;
      if (header->AceType != ACCESS_ALLOWED_ACE_TYPE && header->AceType != ACCESS_DENIED_ACE_TYPE) continue;
      const auto* entry = static_cast<ACCESS_ALLOWED_ACE*>(ace);
      if (::EqualSid(sid, const_cast<DWORD*>(&entry->SidStart))) {
        allowed |= header->AceType == ACCESS_ALLOWED_ACE_TYPE && (entry->Mask & FILE_GENERIC_EXECUTE) == FILE_GENERIC_EXECUTE &&
            (entry->Mask & FILE_GENERIC_READ) == FILE_GENERIC_READ;
        denied |= header->AceType == ACCESS_DENIED_ACE_TYPE && (entry->Mask & (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE));
      }
    }
  }
  if (security) ::LocalFree(security);
  ::FreeSid(sid); return allowed && !denied;
}
struct OwnedLpacProfile {
  bool owned = false;
  bool Create() {
    PSID sid = nullptr;
    const HRESULT result = ::CreateAppContainerProfile(L"v8host.lpac.test", L"LPAC test", L"LPAC test", nullptr, 0, &sid);
    if (sid) ::FreeSid(sid);
    owned = SUCCEEDED(result);
    return owned;
  }
  bool Remove() {
    if (!owned) return true;
    const bool removed = SUCCEEDED(::DeleteAppContainerProfile(L"v8host.lpac.test"));
    if (removed) owned = false;
    return removed;
  }
  ~OwnedLpacProfile() { if (owned && !Remove()) std::printf("[cleanup] owned profile removal failed\n"); }
};
bool BuildOutputPublicCase(const char* name, std::string* detail, const std::wstring& layout_directory = std::wstring(),
                           const std::wstring& desktop = std::wstring()) {
  const std::wstring directory = layout_directory.empty() ? ExecutableDirectory() : layout_directory;
  std::vector<std::filesystem::path> paths{directory};
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
    const auto extension = it->path().extension().wstring();
    if (::_wcsicmp(extension.c_str(), L".exe") == 0 || ::_wcsicmp(extension.c_str(), L".dll") == 0 ||
        ::_wcsicmp(extension.c_str(), L".bin") == 0 || ::_wcsicmp(extension.c_str(), L".blob") == 0 ||
        ::_wcsicmp(extension.c_str(), L".dat") == 0) paths.push_back(it->path());
  }
  if (ec) { *detail = "LPAC preflight enumeration error=" + std::to_string(ec.value()); return false; }
  for (const auto& path : paths) {
    if (!LpacReadable(path.wstring())) { *detail = "LPAC SID read/execute preflight: " + path.string(); return false; }
  }
  OwnedLpacProfile profile;
  if (!profile.Create()) { *detail = "LPAC profile exists or creation failed; no unowned state removed"; return false; }
  ChildResult result = RunOwnedChild(directory, name,
      std::strcmp(name, "real-lpac-session") == 0 || std::strcmp(name, "lpac-worker-token") == 0 ? 180000 :
      std::strcmp(name, "timing-samples") == 0 ? 300000 : 120000, false, desktop);
  const bool removed = profile.Remove();
  std::printf("%s", result.output.c_str());
  if (std::strcmp(name, "create-close-and-runaway") == 0 || std::strcmp(name, "public-worker-crash") == 0) {
    // These cases intentionally end one owned worker. Keep the raw diagnostic
    // above, and allow only its exact line; all other failures stay fatal.
    const std::string expected = std::strcmp(name, "create-close-and-runaway") == 0
        ? "[broker] target exit=18 (0x12) -> FAIL" : "[broker] target exit=64023 (0xfa17) -> FAIL";
    const auto at = result.output.find(expected);
    const auto end = at == std::string::npos ? at : at + expected.size();
    if (at == std::string::npos || (at && result.output[at - 1] != '\n') ||
        (result.output.compare(end, 2, "\r\n") != 0 && result.output.compare(end, 1, "\n") != 0) ||
        result.output.find(expected, end) != std::string::npos) {
      *detail = "missing, malformed or repeated owned-worker exit diagnostic"; return false;
    }
    result.output.replace(at, expected.size(), "[broker] expected owned-worker exit");
  }
  *detail = "build-output child exit=" + std::to_string(result.code) + " error=" + std::to_string(result.error) +
      " job_empty=" + std::to_string(result.empty) + " profile_removed=" + std::to_string(removed);
  return removed && ChildPassed(result, name);
}
bool ImmutableWorkerHold(std::string* detail) {
  v8host::HeldFile verified, worker, denied;
  DWORD error = 0;
  *detail = "default open";
  const auto path = ExecutableDirectory() + L"\\v8host.dll";
  if (!v8host::OpenImmutableFile(path, &verified, &error) || verified.final_path().empty()) return false;
  struct Reset { ~Reset() { v8host::FailFinalPathForTesting(false); } } reset;
  v8host::FailFinalPathForTesting(true);
  if (v8host::OpenImmutableFile(path, &denied, &error) || error != ERROR_ACCESS_DENIED || denied) {
    *detail = "default verified open did not fail closed"; return false;
  }
  *detail = "worker open/hash/machine";
  if (!v8host::OpenImmutableFile(path, &worker, &error, v8host::FinalPathMode::kOmitForWorkerHold) ||
      !worker.final_path().empty() || worker.sha256() != verified.sha256() || worker.machine() != verified.machine()) return false;
  verified = v8host::HeldFile();
  denied = v8host::HeldFile();
  *detail = "write/delete share denial";
  for (DWORD access : {GENERIC_WRITE, DELETE}) {
    HANDLE mutator = ::CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (mutator != INVALID_HANDLE_VALUE) { ::CloseHandle(mutator); return false; }
    if (::GetLastError() != ERROR_SHARING_VIOLATION) { *detail += " error=" + std::to_string(::GetLastError()); return false; }
  }
  *detail = "invalid/missing/directory rejection";
  for (auto mode : {v8host::FinalPathMode::kRequired, v8host::FinalPathMode::kOmitForWorkerHold}) {
    v8host::HeldFile invalid;
    if (v8host::OpenImmutableFile(L"", &invalid, &error, mode) || error != ERROR_INVALID_PARAMETER ||
        v8host::OpenImmutableFile(ExecutableDirectory(), &invalid, &error, mode) || invalid) return false;
    if (v8host::OpenImmutableFile(path + L".missing", &invalid, &error, mode) || invalid) return false;
  }
  *detail = "worker hold retains machine/hash/share checks; verified default fails closed on final-path denial";
  return true;
}
bool RealLpacSession(std::string* detail) { return BuildOutputPublicCase("real-lpac-session", detail); }
// The worker's process tree runs on a desktop whose DACL grants only this user
// and SYSTEM, as a service logon's Service-0x0-<luid>$\Default does (a CI agent
// running as a service, ARM64 CI). An AppContainer process that initializes
// user32 there dies in the loader with STATUS_DLL_INIT_FAILED, so the LPAC worker
// must not load user32. The desktop alone reproduces it, even under WinSta0.
bool LpacServiceDesktop(std::string* detail) {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) { *detail = "token"; return false; }
  DWORD bytes = 0;
  ::GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  std::vector<unsigned char> user(bytes);
  wchar_t* user_sid = nullptr;
  const bool have_user = bytes && ::GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes) &&
      ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &user_sid);
  ::CloseHandle(token);
  if (!have_user) { *detail = "token user"; return false; }
  const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + user_sid + L")(A;;GA;;;SY)";
  ::LocalFree(user_sid);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    *detail = "desktop descriptor"; return false;
  }
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
  const std::wstring leaf = L"v8host-lpac-service-" + std::to_wstring(::GetCurrentProcessId());
  HDESK desktop = ::CreateDesktopW(leaf.c_str(), nullptr, nullptr, 0, GENERIC_ALL, &attributes);
  const DWORD create_error = ::GetLastError();
  // Windows may add ACEs on creation; re-apply the exact DACL.
  SECURITY_INFORMATION dacl = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
  const bool exact = desktop && ::SetUserObjectSecurity(desktop, &dacl, descriptor);
  ::LocalFree(descriptor);
  if (!desktop) { *detail = "CreateDesktop error=" + std::to_string(create_error); return false; }
  struct Close { HDESK h; ~Close() { ::CloseDesktop(h); } } close{desktop};
  // Precondition: neither ALL APPLICATION PACKAGES nor ALL RESTRICTED APPLICATION
  // PACKAGES (nor any other package SID) may be granted, or the case proves nothing.
  PACL acl = nullptr; PSECURITY_DESCRIPTOR current = nullptr;
  bool package_granted = true;
  if (exact && ::GetSecurityInfo(desktop, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl,
                                 nullptr, &current) == ERROR_SUCCESS && acl) {
    package_granted = false;
    SID_IDENTIFIER_AUTHORITY package_authority = SECURITY_APP_PACKAGE_AUTHORITY;
    for (DWORD i = 0; i < acl->AceCount; ++i) {
      void* ace = nullptr;
      if (!::GetAce(acl, i, &ace)) { package_granted = true; break; }
      if (static_cast<ACE_HEADER*>(ace)->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
      PSID sid = &static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart;
      if (std::memcmp(::GetSidIdentifierAuthority(sid), &package_authority, sizeof(package_authority)) == 0)
        package_granted = true;
    }
  }
  if (current) ::LocalFree(current);
  if (package_granted) { *detail = "service-like desktop precondition: an app package is granted"; return false; }
  wchar_t station[256] = {};
  DWORD needed = 0;
  if (!::GetUserObjectInformationW(::GetProcessWindowStation(), UOI_NAME, station, sizeof(station) - sizeof(wchar_t),
                                   &needed)) {
    *detail = "window station name"; return false;
  }
  const std::wstring full = std::wstring(station) + L"\\" + leaf;
  std::printf("[lpac-service-desktop] desktop=%ls (no app package ACE)\n", full.c_str());
  return BuildOutputPublicCase("real-lpac-session", detail, std::wstring(), full);
}
bool BuildOutputChild(std::string* detail) { return BuildOutputPublicCase("build-output-child", detail); }
#define TOPOLOGY_PUBLIC_CASES(X) \
  X(PublicTwoRuntimeJs, "topology", "public-two-runtime-js") \
  X(PublicRuntimeOverrides, "topology", "public-runtime-overrides") \
  X(PublicShapesParallel, "topology", "public-shapes-parallel") \
  X(PublicProcessIdentity, "topology", "public-process-identity") \
  X(LpacWorkerToken, "security", "lpac-worker-token") \
  X(PublicWorkerCrash, "topology", "public-worker-crash") \
  X(PublicBrokerKillReaps, "topology", "public-broker-kill-reaps") \
  X(WorkerRuntimeQuotas, "boundary", "worker-runtime-quotas") \
  X(DefaultInitialToken, "topology", "default-initial-token") \
  X(RelayCallbackCredit, "boundary", "relay-callback-credit") \
  X(CreateCloseAndRunaway, "failure", "create-close-and-runaway") \
  X(RefusalRuntimeAndDisconnect, "failure", "refusal-runtime-and-disconnect")
#define TOPOLOGY_PUBLIC_WRAPPER(fn, suite, name) \
  bool fn(std::string* detail) { return BuildOutputPublicCase(name, detail); }
TOPOLOGY_PUBLIC_CASES(TOPOLOGY_PUBLIC_WRAPPER)
#undef TOPOLOGY_PUBLIC_WRAPPER
bool IsTopologyPublicCase(const std::string& name) {
#define TOPOLOGY_PUBLIC_MATCH(fn, suite, text) if (name == text) return true;
  TOPOLOGY_PUBLIC_CASES(TOPOLOGY_PUBLIC_MATCH)
#undef TOPOLOGY_PUBLIC_MATCH
  return false;
}
bool TopologyPublicBody(const std::string& name, std::string* detail) {
  if (name == "default-initial-token") return PublicDefaultToken(detail);
  if (name == "worker-runtime-quotas") return PublicTopologyQuotas(detail);
  if (name == "relay-callback-credit") return PublicTopologyCallbackComposition(detail);
  if (name == "create-close-and-runaway") return PublicTopologyCloseAndRunaway(detail);
  if (name == "refusal-runtime-and-disconnect") return PublicTopologyRefusal(detail);
  if (name == "public-shapes-parallel") return PublicTopologyShapes(detail);
  if (name == "public-process-identity") return PublicTopologyIdentity(detail);
  if (name == "public-worker-crash") return PublicTopologyLoss(detail, false);
  if (name == "public-broker-kill-reaps") return PublicTopologyLoss(detail, true);
  return PublicTopologyJs(detail, name);
}
bool ChildFailureCleanup(std::string* detail) {
  OwnedLpacProfile profile;
  if (!profile.Create()) { *detail = "LPAC profile exists or creation failed"; return false; }
  ChildResult spoof; spoof.launched = spoof.in_owned_job = spoof.exited = spoof.empty = spoof.drained = true; spoof.code = 0;
  bool ok = true;
  for (const char* text : {"prefix[public-child-result] probe: PASS\n", "child/probe: PASS\n",
      "[public-child-result] probe: PASS extra\n", "[public-child-result] pro\rbe: PASS\n"}) {
    spoof.output = text;
    if (ChildPassed(spoof, "probe")) { *detail = std::string("accepted invalid marker: ") + text; return false; }
  }
  for (const char* name : {"launch-create-failure", "launch-nonzero", "launch-absent", "launch-wrong", "launch-overflow", "launch-timeout"}) {
    const bool create_failure = std::strcmp(name, "launch-create-failure") == 0;
    const bool timeout = std::strcmp(name, "launch-timeout") == 0;
    const auto result = RunOwnedChild(ExecutableDirectory(), name, timeout ? 1000 : 120000, create_failure);
    bool passed = !ChildPassed(result, name) && result.output.find("[cleanup]") == std::string::npos &&
        (create_failure ? !result.launched && result.empty && result.error == ERROR_CANCELLED :
          result.launched && result.in_owned_job && result.drained &&
          (timeout ? !result.exited && result.forced : result.exited && result.empty && !result.forced));
    if (std::strcmp(name, "launch-overflow") == 0) passed &= result.overflow;
    if (std::strcmp(name, "launch-nonzero") == 0) passed &= result.code == 7;
    if (!passed) {
      *detail = std::string(name) + " launched=" + std::to_string(result.launched) +
          " in_owned_job=" + std::to_string(result.in_owned_job) + " exited=" + std::to_string(result.exited) +
          " empty=" + std::to_string(result.empty) + " forced=" + std::to_string(result.forced) +
          " drained=" + std::to_string(result.drained) + " overflow=" + std::to_string(result.overflow) +
          " code=" + std::to_string(result.code) + " error=" + std::to_string(result.error) +
          " output_bytes=" + std::to_string(result.output.size());
      ok = false;
      std::printf("[launch subcase] %s\n", detail->c_str());
    }
    if (result.error == ERROR_VIRUS_INFECTED) { *detail = "STOP: Defender WinError 225"; return false; }
  }
  const bool removed = profile.Remove();
  if (!removed) { *detail = "owned profile removal failed"; return false; }
  if (ok) *detail = "create failure, exit, markers, overflow, watchdog; owned job/reader/profile cleanup";
  return ok;
}
// The shared-mode cases below run from the build output, so they use its
// shared endpoint. Each child's job drains, and its brokers exit, before the
// child returns; only a shared broker started by this process's earlier cases
// can linger (5 s idle grace). Wait for it to retire so each case starts with
// no broker, as the old per-case payload copy guaranteed.
bool WaitForSharedBrokerRetired(std::string* detail) {
  v8host::PayloadIdentity identity;
  DWORD error = 0;
  std::vector<uint8_t> sid;
  LUID session;
  std::wstring endpoint;
  std::array<uint8_t, 32> key;
  if (!v8host::ResolvePayloadIdentity(ExecutableDirectory(), L"sbox.exe", L"v8host.dll", &identity, &error) ||
      !v8host::QueryCurrentSidAndSession(&sid, &session, &error) ||
      !v8host::DeriveEndpoint(sid, identity.plugin_set_id, identity.install_root, BrokerMode::kShared, nullptr, &endpoint, &key)) {
    *detail = "shared endpoint derivation failed"; return false;
  }
  const ULONGLONG deadline = ::GetTickCount64() + 20000;
  while (::WaitNamedPipeW(endpoint.c_str(), NMPWAIT_NOWAIT) || ::GetLastError() != ERROR_FILE_NOT_FOUND) {
    if (::GetTickCount64() >= deadline) { *detail = "shared broker did not retire within 20 s"; return false; }
    ::Sleep(100);
  }
  return true;
}
bool BuildOutputSharedCase(const char* name, std::string* detail) {
  return WaitForSharedBrokerRetired(detail) && BuildOutputPublicCase(name, detail);
}
bool RealNoncooperativeFallback(std::string* detail) { return BuildOutputSharedCase("real-noncooperative-fallback", detail); }
bool RealIdleLifecycle(std::string* detail) { return BuildOutputSharedCase("real-idle-lifecycle", detail); }
bool RealInboundAggregate(std::string* detail) { return BuildOutputSharedCase("real-inbound-aggregate", detail); }
bool PublicRealJs(std::string* detail) { return BuildOutputSharedCase("real-js", detail); }
bool PublicRealDedicatedJs(std::string* detail) { return BuildOutputSharedCase("real-dedicated-js", detail); }
bool PublicRealCancelAndProfile(std::string* detail) { return BuildOutputSharedCase("real-cancel-and-profile", detail); }
bool PublicBrokerKill(std::string* detail) { return BuildOutputSharedCase("broker-kill-no-replay", detail); }
bool PublicDisconnect(std::string* detail) { return BuildOutputSharedCase("disconnect-orderings", detail); }
bool PublicWorkerExit(std::string* detail) { return BuildOutputSharedCase("worker-exit-isolation", detail); }
bool SameConnectionWorkerExit(std::string* detail) { return BuildOutputSharedCase("same-connection-worker-exit", detail); }
bool PublicNSession(std::string* detail) { return BuildOutputSharedCase("n-session-n-pid", detail); }
// Hard-link layout `<root>\<native-arch>\` of build outputs (no executable is
// copied or written): the product's installed shape, where the broker derives
// its install root from the architecture directory's parent.
struct InstalledLayout {
  std::filesystem::path root, arch_dir;
  // Removal is best effort: Windows refuses to delete a hard-link name while
  // another name of the file is a running image (this test's own exe, loaded
  // DLLs), so leftovers are swept at the start of the next layout case.
  ~InstalledLayout() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  bool Create(std::string* detail, bool sweep = true) {
    static std::atomic<unsigned> sequence{0};
    std::error_code ec;
    if (sweep)
      std::filesystem::remove_all(std::filesystem::path(ExecutableDirectory()) / L"layouts", ec);
    root = std::filesystem::path(ExecutableDirectory()) / L"layouts" /
        (L"il-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence));
    arch_dir = root / (v8host::kModuleMachine == IMAGE_FILE_MACHINE_ARM64 ? L"arm64" : L"x64");
    std::filesystem::create_directories(arch_dir, ec);
    if (ec) { *detail = "layout directory: " + ec.message(); return false; }
    struct File { const wchar_t* name; bool required; };
    for (const File& file : {File{L"v8host_broker_tests.exe", true}, File{L"sbox.exe", true}, File{L"v8host.dll", true},
        File{L"v8jsisb.dll", true}, File{L"v8jsi.dll", false}, File{L"msvcp140.dll", false},
        File{L"msvcp140_atomic_wait.dll", false}, File{L"vcruntime140.dll", false}, File{L"vcruntime140_1.dll", false}}) {
      const auto source = std::filesystem::path(ExecutableDirectory()) / file.name;
      if (!std::filesystem::exists(source, ec)) {
        if (file.required) { *detail = "layout source missing: " + source.string(); return false; }
        continue;
      }
      if (!::CreateHardLinkW((arch_dir / file.name).c_str(), source.c_str(), nullptr)) {
        *detail = "layout hard link failed error=" + std::to_string(::GetLastError());
        return false;
      }
    }
    return true;
  }
};
bool InstalledLayoutCase(const char* name, std::string* detail) {
  InstalledLayout layout;
  v8host::PayloadIdentity identity;
  DWORD error = 0;
  if (!layout.Create(detail)) return false;
  // The architecture directory's parent is the install root (not the dev fallback).
  if (!v8host::ResolvePayloadIdentity(layout.arch_dir.wstring(), L"sbox.exe", L"v8host.dll", &identity, &error)) {
    *detail = "layout payload identity failed error=" + std::to_string(error);
    return false;
  }
  // Final paths use the volume GUID instead of the drive letter: compare the tail.
  const std::wstring want = layout.root.wstring().substr(2);
  std::wstring derived = identity.install_root;
  const bool root_matches = derived.size() >= want.size() &&
      ::_wcsicmp(derived.c_str() + (derived.size() - want.size()), want.c_str()) == 0;
  if (!root_matches) {
    *detail = "install root was not derived from the architecture directory: got " +
        std::string(derived.begin(), derived.end()) + " want " + layout.root.string();
    return false;
  }
  std::printf("[layout] root=%ls arch_dir=%ls\n", layout.root.c_str(), layout.arch_dir.c_str());
  return WaitForSharedBrokerRetired(detail) && BuildOutputPublicCase(name, detail, layout.arch_dir.wstring());
}
bool InstalledRealJs(std::string* detail) { return InstalledLayoutCase("real-js", detail); }
bool InstalledDedicatedJs(std::string* detail) { return InstalledLayoutCase("real-dedicated-js", detail); }
bool InstalledIdleRetirement(std::string* detail) { return InstalledLayoutCase("real-idle-lifecycle", detail); }
std::wstring LayoutEvent(const std::filesystem::path& root, const wchar_t* suffix) {
  return L"Local\\v8host-layout-" + root.filename().wstring() + suffix;
}
bool InstalledRootHold(std::string* detail) {
  const auto root = std::filesystem::path(ExecutableDirectory()).parent_path();
  HANDLE ready = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, LayoutEvent(root, L"-ready").c_str());
  HANDLE release = ::OpenEventW(SYNCHRONIZE, FALSE, LayoutEvent(root, L"-release").c_str());
  BrokerConnection connection;
  HelloResult hello;
  bool ok = ready && release &&
      ConnectAndHandshake(BrokerMode::kShared, 1, &connection, &hello, detail) &&
      ::SetEvent(ready) && ::WaitForSingleObject(release, 30000) == WAIT_OBJECT_0;
  const DWORD pid = connection.broker_pid();
  connection.Close();
  if (ok) ok = v8host::test::WaitForProcessExit(pid, 10000);
  if (ready) ::CloseHandle(ready);
  if (release) ::CloseHandle(release);
  return ok;
}
bool InstalledIdenticalRoots(std::string* detail) {
  InstalledLayout layouts[2];
  if (!layouts[0].Create(detail) || !layouts[1].Create(detail, false)) return false;
  HANDLE ready[2] = {}, release[2] = {};
  ChildResult results[2];
  std::vector<std::thread> children;
  bool ok = true;
  for (size_t i = 0; i < 2; ++i) {
    ready[i] = ::CreateEventW(nullptr, TRUE, FALSE, LayoutEvent(layouts[i].root, L"-ready").c_str());
    release[i] = ::CreateEventW(nullptr, TRUE, FALSE, LayoutEvent(layouts[i].root, L"-release").c_str());
    ok = ok && ready[i] && release[i];
  }
  if (ok) {
    for (size_t i = 0; i < 2; ++i)
      children.emplace_back([&, i] {
        results[i] = RunOwnedChild(layouts[i].arch_dir.wstring(), "installed-root-hold", 60000);
      });
    ok = ::WaitForMultipleObjects(2, ready, TRUE, 45000) == WAIT_OBJECT_0;
  }
  for (HANDLE event : release) if (event) ::SetEvent(event);
  for (auto& child : children) child.join();
  for (size_t i = 0; i < 2; ++i) {
    if (!ChildPassed(results[i], "installed-root-hold")) ok = false;
    if (ready[i]) ::CloseHandle(ready[i]);
    if (release[i]) ::CloseHandle(release[i]);
  }
  if (!ok) {
    *detail = "identical payload roots did not hold two simultaneous shared connections";
    for (const auto& result : results)
      std::printf("[layout child] launched=%d code=%lu error=%lu output=%s\n",
          result.launched, result.code, result.error, result.output.c_str());
  }
  return ok;
}
bool PublicRealTrustedJs(std::string* detail) { return BuildOutputPublicCase("real-trusted-js", detail); }
bool TimingSamples(std::string* detail) { return BuildOutputPublicCase("timing-samples", detail); }
// Worker startup (run start to security_ready) for each tier; samples only, no threshold.
bool TimingSamplesBody(std::string* detail) {
  constexpr size_t kSamples = 10;
  for (bool trusted : {false, true}) {
    g_startup_ms.clear();
    for (size_t i = 0; i < kSamples; ++i) {
      if (!PublicRealJsMode(v8host_broker_mode_dedicated, detail, false, trusted)) return false;
    }
    if (g_startup_ms.size() < kSamples) { *detail = "missing startup samples"; return false; }
    std::vector<ULONGLONG> sorted(g_startup_ms);
    std::sort(sorted.begin(), sorted.end());
    std::printf("[timing] tier=%s samples=%zu startup_ms min=%llu median=%llu p90=%llu max=%llu\n",
        trusted ? "trusted" : "untrusted", sorted.size(), sorted.front(),
        sorted[sorted.size() / 2], sorted[(sorted.size() * 9) / 10], sorted.back());
  }
  *detail = "worker startup samples for both tiers";
  return true;
}
bool IsSharedLegacyCase(const std::string& name) {
  for (const char* legacy : {"real-noncooperative-fallback", "real-idle-lifecycle", "real-inbound-aggregate",
      "real-js", "real-dedicated-js", "real-cancel-and-profile", "broker-kill-no-replay", "disconnect-orderings",
      "worker-exit-isolation", "same-connection-worker-exit", "n-session-n-pid", "real-trusted-js", "timing-samples"}) {
    if (name == legacy) return true;
  }
  return false;
}
bool SharedLegacyBody(const std::string& name, std::string* detail) {
  return name == "real-noncooperative-fallback" ? RealNoncooperativeFallbackBody(detail) :
      name == "real-idle-lifecycle" ? RealIdleLifecycleBody(detail) :
      name == "real-inbound-aggregate" ? RealInboundAggregateBody(detail) :
      name == "real-js" ? PublicRealJsMode(v8host_broker_mode_shared, detail) :
      name == "real-dedicated-js" ? PublicRealJsMode(v8host_broker_mode_dedicated, detail) :
      name == "real-trusted-js" ? PublicRealJsMode(v8host_broker_mode_dedicated, detail, false, true) :
      name == "timing-samples" ? TimingSamplesBody(detail) :
      name == "real-cancel-and-profile" ? PublicRealCancelAndProfileBody(detail) :
      name == "n-session-n-pid" ? PublicNSessionBody(detail) :
      name == "worker-exit-isolation" ? PublicWorkerExitBody(detail) :
      name == "same-connection-worker-exit" ? SameConnectionWorkerExitBody(detail) :
      name == "disconnect-orderings" ? PublicDisconnectBody(detail) :
      name == "broker-kill-no-replay" && PublicBrokerKillBody(detail);
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--public-child=", 15) != 0) continue;
    std::string name = argv[i] + 15, detail;
    if (name == "build-output-child" || name == "real-lpac-session" || IsSharedLegacyCase(name) ||
        IsTopologyPublicCase(name) || name == "installed-root-hold" || name.rfind("launch-", 0) == 0) {
      BOOL in_job = FALSE;
      if (!::IsProcessInJob(::GetCurrentProcess(), nullptr, &in_job) || !in_job) return 9;
      if (name == "launch-timeout") { ::Sleep(INFINITE); return 9; }
      if (name == "launch-absent") return 0;
      if (name == "launch-nonzero") {
        std::printf("[public-child-result] launch-nonzero: PASS\n"); return 7;
      }
      if (name == "launch-overflow") {
        const std::string bytes(1024 * 1024 + 1, 'x');
        std::fwrite(bytes.data(), 1, bytes.size(), stdout);
      }
      wchar_t image[32768] = {}, cwd[32768] = {}, plugin[32768] = {};
      const auto directory = ExecutableDirectory();
      bool ok = ::GetModuleFileNameW(nullptr, image, 32768) && ::GetCurrentDirectoryW(32768, cwd) &&
          ::_wcsicmp(image, (directory + L"\\v8host_broker_tests.exe").c_str()) == 0 && directory == cwd;
      if (name == "build-output-child" || name == "real-lpac-session") {
        ok = ok && PublicRealJsMode(v8host_broker_mode_dedicated, &detail, name == "real-lpac-session") &&
            ::GetModuleFileNameW(::GetModuleHandleW(L"v8host.dll"), plugin, 32768) &&
            ::_wcsicmp(plugin, (directory + L"\\v8host.dll").c_str()) == 0;
        std::printf("[public paths] exe=%ls plugin=%ls container=%ls\\sbox.exe engine=%ls\\v8jsisb.dll in_job=%d\n",
            image, plugin, directory.c_str(), directory.c_str(), in_job);
      }
      if (IsSharedLegacyCase(name)) {
        ok = ok && SharedLegacyBody(name, &detail);
        std::printf("child/%s: %s - %s\n", name.c_str(), ok ? "PASS" : "FAIL", detail.c_str());
      }
      if (name == "installed-root-hold") ok = ok && InstalledRootHold(&detail);
      if (IsTopologyPublicCase(name)) {
        ok = ok && TopologyPublicBody(name, &detail);
        std::printf("[public topology] %s: %s\n", name.c_str(), detail.c_str());
      }
      std::printf("[public-child-result] %s: %s\n", name == "launch-wrong" ? "other" : name.c_str(), ok ? "PASS" : "FAIL");
      return ok ? 0 : 1;
    }
    std::printf("child/%s: FAIL - unknown public child\n", name.c_str());
    return 1;
  }
  const std::vector<v8host::test::TestCase> tests = {
#define TOPOLOGY_PUBLIC_REGISTER(fn, suite, name) {suite, name, fn},
      TOPOLOGY_PUBLIC_CASES(TOPOLOGY_PUBLIC_REGISTER)
#undef TOPOLOGY_PUBLIC_REGISTER
      {"worker", "raw-two-runtime-js", RawTwoRuntimeJs},
      {"worker", "profile-policy-reject", WorkerProfilePolicyReject},
      {"worker", "tenancy-and-ids", WorkerTenancyAndIds},
      {"worker", "close-at-cap", WorkerCloseAtCap},
      {"runtime", "queued-slot-churn", RuntimeQueuedSlotChurn},
      {"runtime", "pre-ready-start", RuntimePreReadyStart},
      {"runtime", "independent-runs-cancels", RuntimeIndependentRunsCancels},
      {"runtime", "invalid-output-state", RuntimeInvalidOutputState},
      {"runtime", "raw-overrides", RuntimeRawOverrides},
      {"lpac", "immutable-worker-hold", ImmutableWorkerHold},
      {"client", "real-lpac-session", RealLpacSession},
      {"security", "lpac-service-desktop", LpacServiceDesktop},
      {"launch", "build-output-child", BuildOutputChild},
      {"launch", "child-failure-cleanup", ChildFailureCleanup},
      {"cancel", "real-noncooperative-fallback", RealNoncooperativeFallback},
      {"rendezvous", "real-idle-lifecycle", RealIdleLifecycle},
      {"rendezvous", "idle-accept-boundary", IdleAcceptBoundary},
      {"quota", "real-inbound-aggregate", RealInboundAggregate},
      {"crash", "broker-kill-no-replay", PublicBrokerKill},
      {"broker-crash", "broker-kill-no-replay", PublicBrokerKill},
      {"run", "public-js", PublicRealJs},
      {"session", "public-dedicated-js", PublicRealDedicatedJs},
      {"cancel", "public-cancel-and-profile", PublicRealCancelAndProfile},
      {"crash", "disconnect-orderings", PublicDisconnect},
      {"crash", "worker-exit-isolation", PublicWorkerExit},
      {"worker-crash", "worker-exit-isolation", PublicWorkerExit},
      {"no-crosstalk", "n-session-n-pid", PublicNSession},
      {"no-crosstalk", "same-connection-worker-exit", SameConnectionWorkerExit},
      {"worker-crash", "same-connection-worker-exit", SameConnectionWorkerExit},
      {"crash", "same-connection-worker-exit", SameConnectionWorkerExit},
      {"client", "real-js", PublicRealJs},
      {"client", "real-dedicated-js", PublicRealDedicatedJs},
      {"client", "real-trusted-js", PublicRealTrustedJs},
      {"timing", "timing-samples", TimingSamples},
      {"layout", "installed-real-js", InstalledRealJs},
      {"layout", "installed-dedicated-js", InstalledDedicatedJs},
      {"layout", "installed-idle-retirement", InstalledIdleRetirement},
      {"layout", "installed-identical-roots", InstalledIdenticalRoots},
      {"client", "real-cancel-and-profile", PublicRealCancelAndProfile},
      {"router", "discard-intake-ordering", DiscardIntakeOrdering},
      {"cancel", "terminal-before-fallback", TerminalBeforeFallback},
      {"router", "bookkeeping-callback-overlap", BookkeepingCallbackOverlap},
      {"router", "ledger-hazard-churn", LedgerHazardChurn},
      {"quota", "run-pressure-containment", RunPressureContainment},
      {"quota", "retry-session-containment", RetrySessionContainment},
      {"quota", "paused-disconnect", PausedDisconnect},
      {"quota", "output-loss-sticky", OutputLossSticky},
      {"router", "final-reply-mailbox", FinalReplyMailbox},
      {"quota", "credit-all-paths", CreditAllPaths},
      {"lifetime", "terminal-contender-orderings", TerminalContenderOrderings},
      {"lifetime", "remove-before-close", TerminalContenderOrderings},
      {"lifetime", "joined-teardown", JoinedTeardown},
      {"lifetime", "complete-zero-drain", CompleteZeroDrain},
      {"lifetime", "idle-held-cleanup", CompleteZeroDrain},
      {"quota", "pause-resume", PauseResume},
      {"router", "worker-mailbox-overflow", WorkerMailboxOverflow},
      {"router", "id-exhaustion", IdExhaustion},
      {"quota", "relay-run-boundary", RelayRunBoundary},
      {"quota", "relay-connection-boundary", RelayConnectionBoundary},
      {"quota", "inbound-error-drain", InboundErrorDrain},
      {"quota", "control-boundary", ControlBoundary},
      {"router", "worker-ring-retry-exhaustion", RetryExhaustion},
      {"router", "worker-ring-full", RetryFifoCredits},
      {"router", "terminal-orderings", TerminalOrderings},
      {"router", "connection-joined", ConnectionJoined},
      {"router", "persistent-control", PersistentControl},
      {"router", "header-direction-reject", HeaderDirectionReject},
      {"router", "unknown-type-ignored", UnknownTypeIgnored},
      {"router", "unknown-must-understand", UnknownMustUnderstand},
      {"router", "control-cache", ControlCache},
      {"router", "id-retirement", IdRetirement},
      {"router", "control-budget", ControlBudget},
      {"router", "start-admission-validation", StartAdmissionValidation},
      {"cancel", "queued-cancels-first", QueuedCancelsFirst},
      {"cancel", "timeout-close-order", TimeoutCloseOrder},
      {"cancel", "timeout-pending-read", TimeoutPendingRead},
      {"session", "reaping-admission-quota", ReapingSessionQuota},
      {"router", "worker-mailbox-pump-bound", MailboxPumpBound},
      {"cancel", "cancels-first", CancelsFirst},
      {"cancel", "completes-first", CompletesFirst},
      {"run", "lifecycle-abi", LifecycleAbi},
      {"run", "exit-fact-origin", ExitFactOrigin},
      {"run", "exit-fact-final-drain", ExitFinalDrain},
      {"run", "smoke-lifecycle-two", SmokeLifecycleTwo},
      {"run", "worker-post-kind-guard", WorkerKindGuard},
      {"run", "lifecycle-post-fail-startup", LifecycleFailStartup},
      {"run", "lifecycle-post-fail-security", LifecycleFailSecurity},
      {"run", "post-lockdown-ping-fail", PingPostFail},
      {"lifetime", "early-worker-exit", LifecycleFailSecurity},
      {"lifetime", "exit-cleanup-orderings", ExitCleanupOrderings},
      {"run", "start-spawn", StartSpawn},
      {"run", "real-two-run-reuse", RealTwoRunReuse},
      {"lifetime", "close-live-worker", CloseLiveWorker},
      {"lifetime", "disconnect-live-worker", DisconnectLiveWorker},
      {"run", "start-relay-same-drain", StartRelaySameDrain},
      {"run", "start-bridge-overflow", StartBridgeOverflow},
      {"cancel", "start-cancel-same-drain", StartCancelSameDrain},
      {"run", "coordinator-terminal-holds-next-start", CoordinatorTerminalHoldsNext},
      {"session", "create-close-loop", CreateCloseLoop},
      {"run", "relay-hold-before-ready", RelayHoldBeforeReady},
      {"run", "relay-hold-next-run", RelayHoldNextRun},
      {"run", "readiness-provenance", ReadinessProvenance},
      {"run", "lifecycle-order", LifecycleOrder},
      {"run", "lifecycle-wrong-worker", LifecycleWrongWorker},
      {"run", "first-run-binding", FirstRunBinding},
      {"run", "profile-conflict-invalid", ProfileConflictInvalid},
      {"run", "worker-output-zero-id", WorkerOutputZeroId},
      {"run", "start-admission-quota", StartAdmissionQuota},
      {"run", "envelope-size-boundary", EnvelopeSizeBoundary},
      {"run", "definite-post-failure", DefinitePostFailure},
      {"run", "spawn-failure", SpawnFailure},
      {"run", "mailbox-boundary", MailboxBoundary},
      {"run", "mailbox-readout", MailboxReadout},
      {"run", "mailbox-failure-sticky", MailboxFailureSticky},
      {"run", "relay-mailbox-fairness", RelayMailboxFairness},
      {"run", "inline-read-mailbox", InlineReadMailbox},
      {"run", "held-relay-budget", HeldRelayBudget},
      {"lifetime", "close-publication-orderings", CloseDuringPublication},
      {"session", "quota-32", SessionQuota32},
      {"router", "profile-validation", ProfileValidation},
      {"session", "hello-version-reject", HelloVersionReject},
      {"session", "create-ready", CreateReady},
      {"session", "config-fidelity-0", ConfigFidelity0},
      {"session", "config-fidelity-64", ConfigFidelity64},
      {"rendezvous", "dedicated-preconnect-expiry-policy", DedicatedPreConnectExpiryPolicy},
      {"rendezvous", "pending-accept-cancellation", PendingAcceptCancellation},
      {"rendezvous", "pending-accept-poll", PendingAcceptPoll},
      {"rendezvous", "shared-fan-in", SharedFanIn},
      {"rendezvous", "exact-payload-separation", ExactPayloadSeparation},
      {"rendezvous", "install-root-derivation", InstallRootDerivation},
      {"rendezvous", "dedicated-unique", DedicatedUnique},
      {"rendezvous", "mixed-mode", MixedMode},
      {"rendezvous", "idle-exit-relaunch", IdleExitRelaunch},
      {"rendezvous", "grace-cancel", GraceCancel},
      {"rendezvous", "no-generic-client-role", NoGenericClientRole},
      {"peer-auth", "endpoint-squatter", EndpointSquatter},
      {"peer-auth", "immutable-write", ImmutableWrite},
      {"peer-auth", "mutable-preexisting-sharing", MutablePreexisting},
      {"peer-auth", "immutable-mutation", ImmutableMutation},
      {"peer-auth", "canonical-install-root", PathPolicy},
      {"peer-auth", "compile-time-unsigned-policy", TrustPolicy},
      {"peer-auth", "same-user-positive", AuthSameUserPositive},
      {"peer-auth", "cross-user-reject", AuthCrossUserReject},
      {"peer-auth", "wrong-session-reject", AuthWrongSessionReject},
      {"peer-auth", "wrong-payload-image-reject", AuthWrongPayloadImageReject},
      {"peer-auth", "non-co-located-reject", AuthNonColocatedReject},
      {"peer-auth", "sibling-prefix-reject", AuthSiblingPrefixReject},
      {"peer-auth", "dedicated-non-co-located-positive",
       AuthDedicatedNonColocatedPositive},
      {"peer-auth", "launch-instance-mismatch-reject", AuthLaunchMismatchReject},
      {"peer-auth", "pid-reread-mismatch-reject", AuthPidRereadMismatchReject},
      {"peer-auth", "query-failure-reject", AuthQueryFailureReject},
      {"peer-auth", "untrusted-image-reject", AuthUntrustedImageReject},
      {"peer-auth", "pid-read-failure-reject", AuthPidReadFailureReject},
      {"peer-auth", "invalid-pipe-reject", AuthInvalidPipeReject},
      {"lifetime", "dedicated-owner-disconnect", DedicatedOwnerExit},
      {"lifetime", "connect-disconnect-races", DisconnectRaces},
      {"lifetime", "broker-api-ownership", BrokerApiOwnership},
      {"lifetime", "broker-api-process-id", BrokerApiProcessId},
      {"lifetime", "broker-api-leaked-cleanup", BrokerApiLeakedCleanup},
      {"lifetime", "host-rejects-bad-plugin-abi", HostRejectsBadPluginAbi},
  };
  return v8host::test::RunTests(argc, argv, tests);
}
