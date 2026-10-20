// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "v8host_router.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace v8host::coordinator {
namespace {
constexpr auto kCancelFallbackTimeout = std::chrono::seconds(5);
constexpr auto kPostRetryInitial = std::chrono::milliseconds(10);
constexpr auto kPostRetryCap = std::chrono::milliseconds(100);
constexpr unsigned kPostRetryAttempts = 16;
}

Connection::Connection() {
  out_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  worker_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
}
Connection::~Connection() {
  Stop();
  JoinWriter();
  ClearOutput();
  sessions.clear();
  retired_sessions.clear();
  workers.clear();
  retired_workers.clear();
  if (out_wake)
    ::CloseHandle(out_wake);
  if (stop)
    ::CloseHandle(stop);
  if (worker_wake)
    ::CloseHandle(worker_wake);
}
void Connection::Stop() {
  if (stop)
    ::SetEvent(stop);
  if (out_wake)
    ::SetEvent(out_wake);
}
namespace {
bool Reserve(std::atomic<size_t> &ledger, size_t bytes, size_t limit) {
  size_t current = ledger.load(std::memory_order_relaxed);
  do {
    if (current > limit || bytes > limit - current)
      return false;
  } while (!ledger.compare_exchange_weak(current, current + bytes));
  return true;
}
}
OutputCredit::OutputCredit(OutputCredit &&other) noexcept { *this = std::move(other); }
OutputCredit &OutputCredit::operator=(OutputCredit &&other) noexcept {
  if (this != &other) {
    Reset();
    conn = other.conn; relay = std::move(other.relay); bytes = other.bytes;
    other.conn = nullptr; other.bytes = 0;
  }
  return *this;
}
OutputCredit::~OutputCredit() { Reset(); }
void OutputCredit::Reset() {
  if (!conn) return;
  if (relay) {
    relay->bytes.fetch_sub(bytes);
    conn->out_relay_bytes.fetch_sub(bytes);
  } else {
    conn->out_control_bytes.fetch_sub(bytes);
    conn->out_control_requests.fetch_sub(1);
  }
  ::SetEvent(conn->worker_wake);
  conn = nullptr; bytes = 0; relay.reset();
}
OutputCredit Connection::ChargeOutput(size_t bytes, std::shared_ptr<RelayLedger> relay) {
  OutputCredit credit;
  if (::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return credit;
  if (relay) {
    if (relay->failed.load()) return credit;
    if (!Reserve(relay->bytes, bytes, protocol::kMaxQueuedRelayBytesPerRun)) {
      relay->failed = true;
      ::SetEvent(worker_wake);
      return credit;
    }
    if (!Reserve(out_relay_bytes, bytes, protocol::kMaxQueuedRelayBytesPerConnection)) {
      relay->bytes.fetch_sub(bytes);
      Stop();
      return credit;
    }
  } else {
    if (!Reserve(out_control_requests, 1, protocol::kMaxControlQueueRequests)) {
      Stop(); return credit;
    }
    if (!Reserve(out_control_bytes, bytes, protocol::kMaxControlQueueBytes)) {
      out_control_requests.fetch_sub(1);
      Stop(); return credit;
    }
  }
  credit.conn = this; credit.bytes = bytes; credit.relay = std::move(relay);
  return credit;
}
bool Connection::Enqueue(std::vector<uint8_t> frame, OutputCredit credit) {
  if (!credit.conn) credit = ChargeOutput(frame.size());
  if (!credit.conn) return false;
  if (credit.conn != this || credit.bytes != frame.size()) { Stop(); return false; }
  std::lock_guard<std::mutex> lock(out_mutex);
  if (!out_wake || !stop || ::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return false;
  out_queue.push_back({std::move(frame), std::move(credit)});
  ::SetEvent(out_wake);
  return true;
}
bool Connection::TakeOutput(std::vector<uint8_t> *frame) {
  std::lock_guard<std::mutex> lock(out_mutex);
  if (out_queue.empty() || in_flight_.conn) return false;
  *frame = std::move(out_queue.front().bytes);
  in_flight_ = std::move(out_queue.front().credit);
  out_queue.pop_front();
  return true;
}
void Connection::CompleteOutput(size_t bytes) {
  std::lock_guard<std::mutex> lock(out_mutex);
  if (in_flight_.conn && bytes == in_flight_.bytes) in_flight_.Reset();
  out_drained_.notify_all();
}
void Connection::ClearOutput() {
  std::lock_guard<std::mutex> lock(out_mutex);
  out_queue.clear();
  in_flight_.Reset();
  out_drained_.notify_all();
}
bool Connection::DrainOutput(DWORD timeout_ms) {
  std::unique_lock<std::mutex> lock(out_mutex);
  return out_drained_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
    return out_queue.empty() && !in_flight_.conn;
  });
}
namespace {
bool Transfer(Connection &conn, bool write, void *data, DWORD capacity, DWORD *transferred, DWORD timeout_ms) {
  OVERLAPPED ov = {};
  ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent)
    return false;
#if defined(V8HOST_ROUTER_TESTING)
  ++conn.active_io;
#endif
  BOOL started = write ? ::WriteFile(conn.pipe, data, capacity, nullptr, &ov)
                       : ::ReadFile(conn.pipe, data, capacity, nullptr, &ov);
  bool pending = !started && ::GetLastError() == ERROR_IO_PENDING;
  bool ok = false;
  if (started) {
#if defined(V8HOST_ROUTER_TESTING)
    if (!write)
      ++conn.inline_reads;
#endif
    ok = ::GetOverlappedResult(conn.pipe, &ov, transferred, FALSE) != FALSE;
  } else if (pending) {
#if defined(V8HOST_ROUTER_TESTING)
    HANDLE barrier = write ? conn.write_pending : conn.read_pending;
    if (barrier)
      ::SetEvent(barrier);
#endif
    HANDLE waits[] = {conn.stop, ov.hEvent, conn.worker_wake};
    for (;;) {
      const DWORD delay = !write && conn.dispatcher ? conn.dispatcher->WakeDelay(conn, timeout_ms) : timeout_ms;
      DWORD result = ::WaitForMultipleObjects(write ? 2 : 3, waits, FALSE, delay);
      if (result == WAIT_TIMEOUT && !write && conn.dispatcher) {
        conn.dispatcher->Pump(conn);
        if (delay != timeout_ms)
          continue;
      }
      if (result == WAIT_OBJECT_0 + 2 && conn.dispatcher) {
        conn.dispatcher->Pump(conn);
        continue;
      }
      if (result == WAIT_OBJECT_0 + 1) {
        ok = ::GetOverlappedResult(conn.pipe, &ov, transferred, FALSE) != FALSE;
        pending = false;
      }
      break;
    }
  }
  if (pending) {
    ::CancelIoEx(conn.pipe, &ov);
    ::GetOverlappedResult(conn.pipe, &ov, transferred, TRUE);
  }
  ::CloseHandle(ov.hEvent);
#if defined(V8HOST_ROUTER_TESTING)
  --conn.active_io;
#endif
  return ok && (!write || *transferred == capacity);
}
} // namespace
bool Connection::ReadsPaused() const {
  return out_control_requests >= protocol::kMaxControlQueueRequests - 1 ||
      out_control_bytes > protocol::kMaxControlQueueBytes - protocol::kMaxFrameSize ||
      out_relay_bytes > protocol::kMaxQueuedRelayBytesPerConnection - protocol::kMaxFrameSize;
}
bool Connection::CompleteZero() const {
  // Called on the strand after I/O and callback borrowers have joined.
  return sessions.empty() && retired_sessions.empty() && workers.empty() && retired_workers.empty() &&
      in_relay_bytes == 0 && out_relay_bytes == 0 && out_control_bytes == 0 && out_control_requests == 0 &&
      !writer_ && !dispatcher;
}
bool Connection::ReadFrame(uint8_t *buffer, DWORD capacity, DWORD *size, DWORD timeout_ms) {
  if (!stop || ::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
    return false;
  bool paused = false;
  const auto pause_begin = std::chrono::steady_clock::now();
  while (dispatcher && ReadsPaused()) {
#if defined(V8HOST_ROUTER_TESTING)
    if (read_paused) ::SetEvent(read_paused);
#endif
    paused = true;
    dispatcher->Pump(*this);
    if (!ReadsPaused()) break;
    // No read is outstanding here. Probe pipe loss even if the writer is idle;
    // a held client process also wakes teardown immediately on client death.
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr)) return false;
    HANDLE waits[] = {stop, worker_wake, peer.process()};
    DWORD budget = timeout_ms;
    if (budget != INFINITE) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - pause_begin).count();
      if (elapsed >= budget) return false;
      budget -= static_cast<DWORD>(elapsed);
    }
    const DWORD delay = dispatcher->WakeDelay(*this, (std::min)(budget, DWORD{100}));
    const DWORD result = ::WaitForMultipleObjects(peer.process() ? 3 : 2, waits, FALSE, delay);
    if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 2 || result == WAIT_FAILED) return false;
    if (result == WAIT_TIMEOUT && budget != INFINITE && delay == budget) return false;
  }
#if defined(V8HOST_ROUTER_TESTING)
  if (paused && read_resumed) ::SetEvent(read_resumed);
#else
  (void)paused;
#endif
  if (::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return false;
  const bool ok = Transfer(*this, false, buffer, capacity, size, timeout_ms);
  if (ok && dispatcher)
    dispatcher->Pump(*this);
  return ok;
}
bool Connection::StartWriter() {
  if (writer_ || !stop || !out_wake)
    return false;
  writer_ = ::CreateThread(nullptr, 0, &WriteTask, this, 0, nullptr);
  return writer_ != nullptr;
}
void Connection::JoinWriter() {
  if (!writer_)
    return;
  ::WaitForSingleObject(writer_, INFINITE);
  ::CloseHandle(writer_);
  writer_ = nullptr;
#if defined(V8HOST_ROUTER_TESTING)
  ++writers_joined;
#endif
}
DWORD WINAPI Connection::WriteTask(void *context) {
  auto &conn = *static_cast<Connection *>(context);
#if defined(V8HOST_ROUTER_TESTING)
  ++conn.writers_started;
  if (conn.writer_release) {
    HANDLE waits[] = {conn.stop, conn.writer_release};
    if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
      return 0;
  }
#endif
  HANDLE waits[] = {conn.stop, conn.out_wake};
  while (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
    std::vector<uint8_t> frame;
    while (::WaitForSingleObject(conn.stop, 0) != WAIT_OBJECT_0 && conn.TakeOutput(&frame)) {
      DWORD transferred = 0;
      bool ok = Transfer(conn, true, frame.data(), static_cast<DWORD>(frame.size()), &transferred, 2000);
      conn.CompleteOutput(frame.size());
      if (!ok) {
        conn.Stop();
        return 0;
      }
    }
  }
  return 0;
}
bool Run::TryTerminal(RunState desired) {
  if (!IsTerminal(desired) || !session)
    return false;
  RunState previous = state.load();
  do {
    if (IsTerminal(previous))
      return false;
  } while (!state.compare_exchange_strong(previous, desired));
  Connection *conn = session->conn;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    auto it = session->runs.find(run_id);
    if (it != session->runs.end() && it->second.get() == this) {
      session->retired_runs.push_back(std::move(it->second));
      session->runs.erase(it);
    }
    if (conn) conn->in_relay_bytes -= queued_relay_bytes;
    held_relays.clear();
    queued_relay_bytes = 0;
    if (Worker *worker = session->host) {
      worker->Publish(session->slot, run_id, {});
      if (worker->retry_slot == session->slot && worker->retry_run_id == run_id &&
          worker->retry_type != v8host::RunEnvelopeType::kCancel) {
        worker->retry_deadline = {}; worker->retry_attempts = 0; worker->retry_run_id = 0;
      }
    }
    if (session->active_run_id == run_id)
      session->active_run_id = 0;
    auto &pending = session->pending_run_ids;
    pending.erase(std::remove(pending.begin(), pending.end(), run_id), pending.end());
  }
#if defined(V8HOST_ROUTER_TESTING)
  if (session->terminal_removed)
    session->terminal_removed();
#endif
  if (conn && desired != RunState::kBrokerLost && desired != RunState::kWorkerExited) {
    protocol::FrameHeader header;
    header.version_major = protocol::kWireVersionMajor;
    header.version_minor = conn->version_minor;
    header.conn_id = conn->conn_id;
    header.target_id = session->session_id;
    header.run_id = run_id;
    if (desired == RunState::kCompleted || desired == RunState::kCancelled) {
      protocol::ResultPayload result;
      result.disposition = desired == RunState::kCompleted ? protocol::ResultDisposition::kCompleted
                                                           : protocol::ResultDisposition::kCancelled;
      conn->Enqueue(protocol::BuildResultFrame(header, result.disposition));
    } else {
      protocol::ErrorPayload failure;
      failure.status_code = error;
      auto frame = protocol::BuildErrorFrame(header, failure);
      header.type = protocol::MessageType::RUN_ERROR;
      header.payload_length = static_cast<uint32_t>(frame.size()) - protocol::kFrameHeaderSize;
      std::vector<uint8_t> encoded;
      protocol::EncodeHeader(header, encoded);
      std::copy(encoded.begin(), encoded.end(), frame.begin());
      conn->Enqueue(std::move(frame));
    }
  }
  return true;
}

namespace {
bool ValidString(const std::string &value) {
  return value.size() <= protocol::kMaxStringBytes &&
      protocol::ValidateUtf8NoNul(reinterpret_cast<const uint8_t *>(value.data()), value.size());
}
bool BareEngine(const std::string &value) {
  return !value.empty() && ValidString(value) && value != "." && value.find("..") == std::string::npos &&
      value.find_first_of("\\/:") == std::string::npos;
}
bool CanonicalSnapshot(const std::string &input, std::wstring root, std::string *out) {
  if (input.empty()) {
    out->clear();
    return true;
  }
  if (!ValidString(input))
    return false;
  int count =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
  if (!count)
    return false;
  std::wstring path(count, L'\0');
  ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), path.data(), count);
  std::replace(path.begin(), path.end(), L'/', L'\\');
  if (root.starts_with(L"\\\\?\\"))
    root.erase(0, 4);
  if (root.size() < 3 || root[1] != L':' || root[2] != L'\\')
    return false;
  if (path.front() == L'\\' || path.find_first_of(L"*?\"<>|") != std::wstring::npos)
    return false;
  const bool absolute = path.size() >= 3 && path[1] == L':' && path[2] == L'\\';
  if (path.find(L':', absolute ? 2 : 0) != std::wstring::npos)
    return false;
  // Reject Win32 aliasing and device names, but allow ordinary dot segments.
  size_t pos = absolute ? 3 : 0;
  while (pos < path.size()) {
    size_t end = path.find(L'\\', pos);
    if (end == std::wstring::npos)
      end = path.size();
    std::wstring part = path.substr(pos, end - pos);
    if (part.empty())
      return false;
    if (part != L"." && part != L"..") {
      if (part.back() == L'.' || part.back() == L' ')
        return false;
      for (wchar_t c : part)
        if (c < 32)
          return false;
      auto stem = part.substr(0, part.find(L'.'));
      for (auto &c : stem)
        if (c >= L'a' && c <= L'z')
          c -= L'a' - L'A';
      if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
          (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) && stem[3] >= L'1' &&
           stem[3] <= L'9'))
        return false;
    }
    pos = end + 1;
  }
  if (path.back() == L'\\')
    return false;
  while (root.size() > 3 && root.back() == L'\\')
    root.pop_back();
  std::wstring joined = absolute ? path : root + L"\\" + path;
  DWORD needed = ::GetFullPathNameW(joined.c_str(), 0, nullptr, nullptr);
  if (!needed)
    return false;
  std::wstring canonical(needed, L'\0');
  DWORD actual = ::GetFullPathNameW(joined.c_str(), needed, canonical.data(), nullptr);
  if (!actual || actual >= needed)
    return false;
  canonical.resize(actual);
  if (!IsStrictDescendant(root, canonical))
    return false;
  count = ::WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      canonical.data(),
      static_cast<int>(canonical.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (!count)
    return false;
  out->resize(count);
  ::WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      canonical.data(),
      static_cast<int>(canonical.size()),
      out->data(),
      count,
      nullptr,
      nullptr);
  return true;
}
protocol::FrameHeader ReplyHeader(const Connection &conn, uint32_t session, uint32_t run = 0) {
  protocol::FrameHeader h;
  h.version_major = protocol::kWireVersionMajor;
  h.version_minor = conn.version_minor;
  h.conn_id = conn.conn_id;
  h.target_id = session;
  h.run_id = run;
  return h;
}
std::vector<uint8_t> Error(protocol::FrameHeader h, protocol::StatusCode status) {
  protocol::ErrorPayload error;
  error.status_code = status;
  return protocol::BuildErrorFrame(h, error);
}
// Caller holds Connection::mutex; removed storage outlives terminal contenders.
// detach_host drops an explicit runtime's link to its worker once its runs have
// ended, so the retired record never outlives the worker it names.
void RetireSessionLocked(Connection &conn, uint32_t id, RunState terminal, bool detach_host = false) {
  auto it = conn.sessions.find(id);
  if (it == conn.sessions.end())
    return;
  auto session = std::move(it->second);
  conn.sessions.erase(it);
  std::vector<Run *> runs;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->state = SessionState::kClosing;
    for (auto &[run_id, run] : session->runs) {
      runs.push_back(run.get());
      session->retired_runs.push_back(std::move(run));
    }
    session->runs.clear();
  }
  for (Run *run : runs)
    run->TryTerminal(terminal);
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->state = SessionState::kClosed;
  }
  if (detach_host)
    session->host = nullptr;
  if (session->worker || !session->retired_runs.empty())
    conn.retired_sessions.push_back(std::move(session));
}
// Logical admissions exclude closed workers. A replacement may wait for the
// retiring process before it starts, rather than failing asynchronously.
size_t WorkerCount(const Connection &conn) {
  size_t count = conn.workers.size();
  for (const auto &[id, session] : conn.sessions)
    if (!session->explicit_runtime) ++count;
  return count;
}
// Conservative process reservations, including every unreclaimed retiree.
size_t LiveWorkerCount(const Connection &conn) {
  size_t count = conn.retired_workers.size();
  for (const auto &[id, worker] : conn.workers)
    if (worker->state != WorkerState::kNone) ++count;
  for (const auto &[id, session] : conn.sessions)
    if (session->worker && session->worker->state != WorkerState::kNone) ++count;
  for (const auto &session : conn.retired_sessions)
    if (session->worker && session->worker->state != WorkerState::kNone) ++count;
  return count;
}
// An explicit worker's admissions: its queued, creating, and ready runtimes.
uint32_t Admissions(const Connection &conn, const Worker &worker) {
  uint32_t count = 0;
  for (const auto &[id, session] : conn.sessions)
    if (session->explicit_runtime && session->host == &worker) ++count;
  return count;
}
// Canonicalizes a snapshot path under the payload directory and returns it
// relative to that directory, as an explicit worker's profile carries it.
bool RelativeSnapshot(const std::string &input, std::wstring root, std::string *out) {
  std::string canonical;
  if (input.empty() || !CanonicalSnapshot(input, root, &canonical))
    return false;
  if (root.starts_with(L"\\\\?\\"))
    root.erase(0, 4);
  while (root.size() > 3 && root.back() == L'\\')
    root.pop_back();
  const int count = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, root.data(), static_cast<int>(root.size()),
                                          nullptr, 0, nullptr, nullptr);
  if (!count)
    return false;
  std::string prefix(count, '\0');
  ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, root.data(), static_cast<int>(root.size()), prefix.data(),
                        count, nullptr, nullptr);
  if (prefix.back() != '\\')
    prefix += '\\';
  if (canonical.size() <= prefix.size() || canonical.compare(0, prefix.size(), prefix) != 0)
    return false;
  *out = canonical.substr(prefix.size());
  return IsPayloadRelativePath(*out);
}
} // namespace

uint32_t AllocateId(std::atomic<uint32_t> &next) {
  uint32_t id = next.load();
  do {
    if (!id || id == UINT32_MAX) return 0;
  } while (!next.compare_exchange_weak(id, id + 1));
  return id;
}
bool EnvelopeFits(v8host::RunEnvelopeType type, size_t size) {
  const size_t overhead =
      v8host::kRunEnvelopeHeaderSize + (type == v8host::RunEnvelopeType::kRelay ? sizeof(int32_t) : 0);
  return size <= protocol::kMaxFramePayload - overhead;
}
bool ValidateSessionConfig(const SessionConfig &config, BrokerMode mode) {
  if ((config.broker_mode != 0 && config.broker_mode != 1) || config.broker_mode != static_cast<int32_t>(mode) ||
      (config.tier != kTierUntrusted && config.tier != kTierTrusted) || config.integrity < sbox_integrity_level_low ||
      config.integrity > sbox_integrity_level_untrusted || config.delayed_integrity < sbox_integrity_level_low ||
      config.delayed_integrity > sbox_integrity_level_untrusted || config.initial_token < sbox_token_level_lockdown ||
      config.initial_token > sbox_token_level_restricted_same_access || config.lockdown_token < sbox_token_level_lockdown ||
      config.lockdown_token > sbox_token_level_restricted_same_access ||
      (config.tier == kTierUntrusted) != config.prohibit_dynamic_code)
    return false;
  if (config.file_rules.size() > protocol::kMaxFileRules || config.capabilities.size() > protocol::kMaxCapabilities ||
      !ValidString(config.app_container_profile))
    return false;
  for (const auto &rule : config.file_rules)
    if (!ValidString(rule.pattern))
      return false;
  for (const auto &cap : config.capabilities)
    if (!ValidString(cap))
      return false;
  protocol::Writer writer;
  protocol::EncodeCreateSessionPayload(config, writer);
  SessionConfig decoded;
  return protocol::DecodeCreateSessionPayload(writer.buffer().data(), writer.buffer().size(), &decoded);
}
bool PrepareProfile(
    const SessionConfig &config,
    const protocol::StartRunPayload &inputs,
    const std::wstring &payload_directory,
    BoundProfile *out) {
  if (!out || inputs.schema_version != protocol::kMessageSchemaVersion || inputs.tier_override < -1 ||
      inputs.tier_override > kTierTrusted || (!inputs.has_engine_override && !inputs.engine_filename.empty()) ||
      (!inputs.has_snapshot && !inputs.snapshot_path.empty()))
    return false;
  BoundProfile profile;
  profile.effective_tier = inputs.tier_override == -1 ? config.tier : inputs.tier_override;
  if ((profile.effective_tier != kTierUntrusted && profile.effective_tier != kTierTrusted) ||
      (profile.effective_tier == kTierUntrusted) != config.prohibit_dynamic_code)
    return false;
  profile.prohibit_dynamic_code = config.prohibit_dynamic_code;
  profile.engine_dll = inputs.has_engine_override
      ? inputs.engine_filename
      : (profile.effective_tier == kTierUntrusted ? "v8jsisb.dll" : "v8jsi.dll");
  if (!BareEngine(profile.engine_dll) ||
      !CanonicalSnapshot(inputs.has_snapshot ? inputs.snapshot_path : "", payload_directory, &profile.snapshot_path))
    return false;
  V8HostWorkerProfileV1 worker;
  worker.jitless = profile.effective_tier == kTierUntrusted;
  worker.engine_dll = profile.engine_dll;
  worker.snapshot_path = profile.snapshot_path;
  auto bytes = worker.Encode();
  V8HostWorkerProfileV1 decoded;
  if (!V8HostWorkerProfileV1::Decode(bytes.data(), bytes.size(), &decoded))
    return false;
  *out = std::move(profile);
  return true;
}
protocol::StatusCode BindProfile(Session &session, const BoundProfile &profile) {
  std::lock_guard<std::mutex> lock(session.mutex);
  if (session.profile_bound) {
    const auto &bound = session.profile;
    if (bound.effective_tier != profile.effective_tier || bound.engine_dll != profile.engine_dll ||
        bound.snapshot_path != profile.snapshot_path || bound.prohibit_dynamic_code != profile.prohibit_dynamic_code)
      return protocol::StatusCode::ERROR_PROFILE_ALREADY_BOUND;
  } else {
    session.profile = profile;
    session.profile_bound = true;
  }
  return protocol::StatusCode::OK;
}
bool PrepareSpawnConfig(const SessionConfig &config, const BoundProfile &profile, std::vector<uint8_t> *out,
                        uint32_t max_runtimes, const std::vector<std::string> *snapshots) {
  if (!out || !ValidateSessionConfig(config, static_cast<BrokerMode>(config.broker_mode)) ||
      profile.prohibit_dynamic_code != config.prohibit_dynamic_code ||
      (profile.effective_tier != kTierUntrusted && profile.effective_tier != kTierTrusted) ||
      (profile.effective_tier == kTierUntrusted) != config.prohibit_dynamic_code ||
      max_runtimes > kWorkerProfileMaxRuntimes || (max_runtimes == 0) != (snapshots == nullptr))
    return false;
  V8HostSpawnConfigV1 spawn;
  spawn.tier = config.tier;
  spawn.integrity = config.integrity;
  spawn.delayed_integrity = config.delayed_integrity;
  spawn.initial_token = config.initial_token;
  spawn.lockdown_token = config.lockdown_token;
  spawn.prohibit_dynamic_code = config.prohibit_dynamic_code;
  spawn.use_app_container = config.use_app_container;
  spawn.low_privilege_app_container = config.low_privilege_app_container;
  spawn.app_container_profile = config.app_container_profile;
  for (const auto &rule : config.file_rules)
    spawn.file_rules.push_back({rule.readonly, rule.pattern});
  spawn.capabilities = config.capabilities;
  spawn.effective_tier = profile.effective_tier;
  spawn.engine_dll = profile.engine_dll;
  spawn.snapshot_path = profile.snapshot_path;
  spawn.max_runtimes = max_runtimes;
  if (snapshots)
    spawn.snapshot_paths = *snapshots;
  auto bytes = spawn.Encode();
  V8HostSpawnConfigV1 decoded;
  if (!V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &decoded))
    return false;
  if (max_runtimes) {
    V8HostWorkerProfileV2 worker;
    worker.jitless = profile.effective_tier == kTierUntrusted;
    worker.max_runtimes = max_runtimes;
    worker.engine_dll = profile.engine_dll;
    worker.snapshot_paths = *snapshots;
    auto worker_bytes = worker.Encode();
    V8HostWorkerProfileV2 worker_decoded;
    if (worker_bytes.size() > kWorkerProfileMaxSize ||
        !V8HostWorkerProfileV2::Decode(worker_bytes.data(), worker_bytes.size(), &worker_decoded))
      return false;
    *out = std::move(bytes);
    return true;
  }
  V8HostWorkerProfileV1 worker;
  worker.jitless = profile.effective_tier == kTierUntrusted;
  worker.engine_dll = profile.engine_dll;
  worker.snapshot_path = profile.snapshot_path;
  auto worker_bytes = worker.Encode();
  V8HostWorkerProfileV1 worker_decoded;
  if (!V8HostWorkerProfileV1::Decode(worker_bytes.data(), worker_bytes.size(), &worker_decoded))
    return false;
  *out = std::move(bytes);
  return true;
}

protocol::StatusCode PrepareWorkerProfile(const protocol::CreateWorkerPayload &payload,
                                          const std::wstring &payload_directory,
                                          BoundProfile *profile,
                                          std::vector<std::string> *snapshots) {
  const auto &config = payload.session;
  BoundProfile bound;
  bound.effective_tier = config.tier;
  bound.prohibit_dynamic_code = config.prohibit_dynamic_code;
  bound.engine_dll = !payload.engine_dll.empty() ? payload.engine_dll
      : config.tier == kTierUntrusted ? "v8jsisb.dll" : "v8jsi.dll";
  if (!BareEngine(bound.engine_dll))
    return protocol::StatusCode::ERROR_BAD_STATE;
  std::vector<std::string> relative;
  for (const auto &path : payload.snapshot_paths) {
    std::string canonical;
    if (!RelativeSnapshot(path, payload_directory, &canonical))
      return protocol::StatusCode::ERROR_BAD_STATE;
    relative.push_back(std::move(canonical));
  }
  // The client bounds the profile before sending, so this is a backstop.
  V8HostWorkerProfileV2 worker;
  worker.jitless = config.tier == kTierUntrusted;
  worker.max_runtimes = payload.max_runtimes;
  worker.engine_dll = bound.engine_dll;
  worker.snapshot_paths = relative;
  if (worker.Encode().size() > kWorkerProfileMaxSize)
    return protocol::StatusCode::ERROR_QUOTA;
  *profile = std::move(bound);
  *snapshots = std::move(relative);
  return protocol::StatusCode::OK;
}

bool Router::Route(Connection &conn, const uint8_t *data, size_t size) {
  protocol::FrameHeader h;
  const uint8_t *payload = nullptr;
  size_t payload_size = 0;
  if (protocol::DecodeAndValidateFrame(data, size, &h, &payload, &payload_size) != protocol::DecodeStatus::kOk ||
      conn.state != ConnState::kOpen || h.version_major != protocol::kWireVersionMajor ||
      h.version_minor != conn.version_minor || h.conn_id != conn.conn_id)
    return false;
  switch (h.type) {
    case protocol::MessageType::HELLO:
    case protocol::MessageType::HELLO_ACK:
    case protocol::MessageType::CREATE_WORKER:
    case protocol::MessageType::CLOSE_WORKER:
    case protocol::MessageType::CREATE_RUNTIME:
    case protocol::MessageType::CLOSE_RUNTIME:
    case protocol::MessageType::RUNTIME_READY:
    case protocol::MessageType::RUNTIME_CLOSED:
    case protocol::MessageType::CREATE_SESSION:
    case protocol::MessageType::SESSION_READY:
    case protocol::MessageType::START_RUN:
    case protocol::MessageType::CANCEL_RUN:
    case protocol::MessageType::CLOSE_SESSION:
    case protocol::MessageType::ACK:
    case protocol::MessageType::ERROR:
    case protocol::MessageType::WORKER_EXIT:
    case protocol::MessageType::STARTUP_READY:
    case protocol::MessageType::SECURITY_READY:
    case protocol::MessageType::RESULT:
    case protocol::MessageType::RUN_ERROR:
    case protocol::MessageType::RELAY_TO_WORKER:
    case protocol::MessageType::RELAY_FROM_WORKER:
      break;
    default:
      if (!(h.flags & protocol::kFlagMustUnderstand))
        return true;
      if (conn.Enqueue(Error(h, protocol::StatusCode::ERROR_UNSUPPORTED_MESSAGE)))
        conn.state = ConnState::kClosing;
      return false;
  }
  if (h.target_id == UINT32_MAX || h.run_id == UINT32_MAX || h.request_id == UINT32_MAX) {
    conn.Stop(); return false;
  }
  if (!h.target_id) return false;
  const bool explicit_create = h.type == protocol::MessageType::CREATE_WORKER ||
      h.type == protocol::MessageType::CREATE_RUNTIME;
  const bool explicit_close = h.type == protocol::MessageType::CLOSE_WORKER ||
      h.type == protocol::MessageType::CLOSE_RUNTIME;
  if ((explicit_create || explicit_close) && !(h.flags & protocol::kFlagMustUnderstand)) return false;
  const bool relay = h.type == protocol::MessageType::RELAY_TO_WORKER;
  const bool create = h.type == protocol::MessageType::CREATE_SESSION;
  const bool start = h.type == protocol::MessageType::START_RUN;
  const bool cancel = h.type == protocol::MessageType::CANCEL_RUN;
  const bool close = h.type == protocol::MessageType::CLOSE_SESSION;
  if ((!relay && !create && !start && !cancel && !close && !explicit_create && !explicit_close) || ((create || close || explicit_create || explicit_close) ? h.run_id != 0 : h.run_id == 0) ||
      (relay ? h.request_id != 0 : h.request_id == 0) || ((cancel || close || explicit_close) && payload_size != 0))
    return false;
  if (relay) {
    int32_t kind = 0;
    const uint8_t *body = nullptr;
    size_t body_size = 0;
    if (!protocol::DecodeRelayPayload(payload, payload_size, &kind, &body, &body_size) ||
        (kind != sbox_msg_kind_string && kind != sbox_msg_kind_binary))
      return false;
    Pump(conn);
    auto sit = conn.sessions.find(h.target_id);
    if (sit == conn.sessions.end())
      return true;
    auto &session = *sit->second;
    auto rit = session.runs.find(h.run_id);
    if (rit == session.runs.end())
      return true;
    auto &run = *rit->second;
    v8host::RunEnvelope env;
    env.type = v8host::RunEnvelopeType::kRelay;
    env.run_id = h.run_id;
    env.relay_kind = kind;
    env.payload.assign(body, body + body_size);
    auto bytes = v8host::EncodeRunEnvelope(env);
    if (bytes.size() > protocol::kMaxFramePayload ||
        bytes.size() > protocol::kMaxQueuedRelayBytesPerRun - run.queued_relay_bytes || run.held_relays.size() >= 256) {
      run.error = protocol::StatusCode::ERROR_QUOTA;
      run.TryTerminal(RunState::kFailed);
        if (session.host && session.engine_busy_run_id == h.run_id) {
        RequestCancel(session, h.run_id);
      }
      return true;
    }
    if (bytes.size() > protocol::kMaxQueuedRelayBytesPerConnection - conn.in_relay_bytes) {
      if (conn.Enqueue(Error(h, protocol::StatusCode::ERROR_QUOTA)))
        conn.state = ConnState::kClosing;
      return false;
    }
    conn.in_relay_bytes += bytes.size();
    run.queued_relay_bytes += static_cast<uint32_t>(bytes.size());
    run.held_relays.push_back(std::move(bytes));
    Dispatch(session);
    Pump(conn);
    return true;
  }
  protocol::RequestIdentity identity{h.type, h.conn_id, h.target_id, h.run_id, h.request_id, h.flags};
  auto classified = conn.request_cache.Classify(identity, payload, payload_size);
  if (classified.classification == protocol::RequestClass::kMismatchedDuplicate)
    return false;
  if (classified.classification == protocol::RequestClass::kExactDuplicate) {
    auto response = *classified.cached_response;
    return conn.Enqueue(std::move(response));
  }
  if (classified.classification == protocol::RequestClass::kStale)
    return conn.Enqueue(Error(h, protocol::StatusCode::ERROR_STALE_REQUEST));
  std::vector<uint8_t> response;
  bool ready = false;
  Session *admitted_session = nullptr;
  Run *admitted_run = nullptr;
  Worker *admitted_worker = nullptr;
  Worker *runtime_host = nullptr;
  bool needs_spawn = false;
  if (explicit_create) {
    Reclaim(conn, false);
    protocol::CreateWorkerPayload worker_config;
    protocol::CreateRuntimePayload runtime_config;
    if (h.type == protocol::MessageType::CREATE_WORKER
            ? !protocol::DecodeCreateWorkerPayload(payload, payload_size, &worker_config)
            : !protocol::DecodeCreateRuntimePayload(payload, payload_size, &runtime_config)) return false;
    std::lock_guard<std::mutex> lock(conn.mutex);
    // A target ID never exceeds the connection's high-water mark twice, and
    // another connection's IDs simply don't exist here (design §3.4). A live
    // ID is below the mark, so it gets ERROR_BAD_STATE too.
    const bool fresh = broker_ && api_ && h.target_id > conn.highest_session_id;
    auto status = protocol::StatusCode::OK;
    if (h.type == protocol::MessageType::CREATE_WORKER) {
      BoundProfile profile;
      std::vector<std::string> snapshots;
      status = !fresh || !ValidateSessionConfig(worker_config.session, mode_) ||
              !protocol::ValidateCreateWorkerPayload(worker_config)
          ? protocol::StatusCode::ERROR_BAD_STATE
          : PrepareWorkerProfile(worker_config, payload_directory_, &profile, &snapshots);
      if (status == protocol::StatusCode::OK && WorkerCount(conn) >= protocol::kMaxSessionsPerConnection)
        status = protocol::StatusCode::ERROR_QUOTA;
      if (status == protocol::StatusCode::OK) {
        auto worker = std::make_unique<Worker>();
        worker->conn = &conn;
        worker->worker_id = h.target_id;
        worker->explicit_worker = true;
        worker->api = api_;
        worker->config = std::move(worker_config.session);
        worker->profile = std::move(profile);
        worker->max_runtimes = worker_config.max_runtimes;
        worker->single_use = (worker_config.flags & protocol::kWorkerFlagSingleUse) != 0;
        worker->snapshots = std::move(snapshots);
        admitted_worker = worker.get();
        conn.workers.emplace(h.target_id, std::move(worker));
      }
    } else {
      auto wit = conn.workers.find(runtime_config.worker_id);
      Worker *host = wit == conn.workers.end() || wit->second->close_called ? nullptr : wit->second.get();
      if (!fresh || !host ||
          !protocol::ValidateCreateRuntimePayload(runtime_config, static_cast<uint32_t>(host->snapshots.size())))
        status = protocol::StatusCode::ERROR_BAD_STATE;
      else if (Admissions(conn, *host) >= host->max_runtimes ||
               (host->single_use && host->created_runtimes >= host->max_runtimes))
        status = protocol::StatusCode::ERROR_QUOTA;
      if (status == protocol::StatusCode::OK) {
        auto runtime = std::make_unique<Session>();
        runtime->session_id = h.target_id;
        runtime->conn = &conn;
        runtime->state = SessionState::kOpen;
        runtime->profile = host->profile;
        runtime->profile_bound = true;
        runtime->host = host;
        runtime->explicit_runtime = true;
        runtime->runtime_state = RuntimeState::kQueued;
        runtime->snapshot_index = runtime_config.snapshot_index;
        ++host->created_runtimes;
        runtime_host = host;
        conn.sessions.emplace(h.target_id, std::move(runtime));
      }
    }
    if (status == protocol::StatusCode::OK) {
      conn.highest_session_id = h.target_id;
      response = protocol::BuildAckFrame(h);
    } else {
      response = Error(h, status);
    }
  } else if (explicit_close) {
    if (h.type == protocol::MessageType::CLOSE_WORKER) {
      if (conn.workers.contains(h.target_id))
        CloseWorker(conn, h.target_id, RunState::kCancelled, false);
    } else {
      auto it = conn.sessions.find(h.target_id);
      if (it != conn.sessions.end() && it->second->explicit_runtime)
        CloseRuntime(conn, h.target_id);
    }
    response = protocol::BuildAckFrame(h);
  } else if (create) {
    Reclaim(conn, false);
    SessionConfig config;
    if (!protocol::DecodeCreateSessionPayload(payload, payload_size, &config))
      return false;
    std::lock_guard<std::mutex> lock(conn.mutex);
    if (conn.sessions.contains(h.target_id) || conn.workers.contains(h.target_id))
      return false;
    if (!ValidateSessionConfig(config, mode_) || h.target_id <= conn.highest_session_id) {
      response = Error(h, protocol::StatusCode::ERROR_BAD_STATE);
    } else if (WorkerCount(conn) >= protocol::kMaxSessionsPerConnection) {
      response = Error(h, protocol::StatusCode::ERROR_QUOTA);
    } else {
      auto session = std::make_unique<Session>();
      session->session_id = h.target_id;
      session->conn = &conn;
      session->config = std::move(config);
      conn.sessions.emplace(h.target_id, std::move(session));
      conn.highest_session_id = h.target_id;
      response = protocol::BuildAckFrame(h);
      ready = true;
    }
  } else if (start) {
    protocol::StartRunPayload inputs;
    if (!protocol::DecodeStartRunPayload(payload, payload_size, &inputs))
      return false;
    auto it = conn.sessions.find(h.target_id);
    if (it != conn.sessions.end() && it->second->explicit_runtime) {
      // An explicit runtime's profile belongs to its worker (D6): a run may set
      // no tier, engine, or snapshot override, even one equal to the worker's.
      auto &runtime = *it->second;
      const bool well_formed = inputs.schema_version == protocol::kMessageSchemaVersion &&
          inputs.tier_override >= -1 && inputs.tier_override <= kTierTrusted &&
          (inputs.has_engine_override || inputs.engine_filename.empty()) &&
          (inputs.has_snapshot || inputs.snapshot_path.empty());
      const bool overrides = inputs.tier_override != -1 || inputs.has_engine_override || inputs.has_snapshot;
      if (!broker_ || !api_ || h.run_id <= runtime.highest_run_id || !well_formed) {
        response = Error(h, protocol::StatusCode::ERROR_BAD_STATE);
      } else if (runtime.runs.size() >= protocol::kMaxInFlightRunsPerSession) {
        response = Error(h, protocol::StatusCode::ERROR_QUOTA);
      } else {
        auto owned = std::make_unique<Run>();
        owned->session = &runtime;
        owned->run_id = h.run_id;
        owned->guest = std::move(inputs.guest_payload);
        owned->state = RunState::kStartPending;
        owned->error = !EnvelopeFits(v8host::RunEnvelopeType::kStart, owned->guest.size())
            ? protocol::StatusCode::ERROR_QUOTA
            : overrides ? protocol::StatusCode::ERROR_PROFILE_ALREADY_BOUND : protocol::StatusCode::OK;
        admitted_run = owned.get();
        runtime.runs.emplace(h.run_id, std::move(owned));
        runtime.highest_run_id = h.run_id;
        admitted_session = &runtime;
        if (runtime.runtime_state != RuntimeState::kQueued)
          runtime.host->slot_highest_run_id[runtime.slot] = h.run_id;
        if (admitted_run->error == protocol::StatusCode::OK) {
          if (runtime.runtime_state != RuntimeState::kQueued)
            runtime.host->Publish(runtime.slot, h.run_id, admitted_run->outbound);
          runtime.pending_run_ids.push_back(h.run_id);
        }
        response = protocol::BuildAckFrame(h);
      }
    } else {
    BoundProfile profile;
    bool prepared =
        it != conn.sessions.end() && PrepareProfile(it->second->config, inputs, payload_directory_, &profile);
    if (!prepared && it != conn.sessions.end() && it->second->profile_bound && inputs.tier_override >= 0 &&
        inputs.tier_override <= kTierTrusted && inputs.tier_override != it->second->profile.effective_tier) {
      auto normalized = inputs;
      normalized.tier_override = -1;
      prepared = PrepareProfile(it->second->config, normalized, payload_directory_, &profile);
      if (prepared) {
        profile = it->second->profile;
        profile.effective_tier = inputs.tier_override;
      }
    }
    if (it == conn.sessions.end() || !broker_ || !api_ || h.run_id <= it->second->highest_run_id || !prepared) {
      response = Error(h, protocol::StatusCode::ERROR_BAD_STATE);
    } else if (it->second->runs.size() >= protocol::kMaxInFlightRunsPerSession) {
      response = Error(h, protocol::StatusCode::ERROR_QUOTA);
    } else {
      auto &session = *it->second;
      auto owned = std::make_unique<Run>();
      owned->session = &session;
      owned->run_id = h.run_id;
      owned->guest = std::move(inputs.guest_payload);
      owned->state = RunState::kStartPending;
      owned->error = EnvelopeFits(v8host::RunEnvelopeType::kStart, owned->guest.size())
          ? BindProfile(session, profile)
          : protocol::StatusCode::ERROR_QUOTA;
      admitted_run = owned.get();
      session.runs.emplace(h.run_id, std::move(owned));
      session.highest_run_id = h.run_id;
      admitted_session = &session;
      if (admitted_run->error == protocol::StatusCode::OK) {
        if (!session.worker) {
          session.worker.reset(new Worker);
          auto &worker = *session.worker;
          worker.session = &session;
          worker.conn = &conn;
          worker.worker_id = session.session_id;
          worker.api = api_;
          worker.slots[0].runtime = &session;
          worker.slots[0].occupied = true;
          session.host = &worker;
          needs_spawn = true;
        }
        session.worker->slot_highest_run_id[0] = h.run_id;
        session.worker->Publish(0, h.run_id, admitted_run->outbound);
        session.pending_run_ids.push_back(h.run_id);
      } else if (session.worker) {
        session.worker->slot_highest_run_id[0] = h.run_id;
      }
      response = protocol::BuildAckFrame(h);
    }
    }
  } else {
    if (close)
      CloseSession(conn, h.target_id, RunState::kCancelled);
    if (cancel) {
      Pump(conn);
      auto sit = conn.sessions.find(h.target_id);
      if (sit != conn.sessions.end() && sit->second->host) {
        auto& session = *sit->second;
        auto rit = session.runs.find(h.run_id);
        if (rit != session.runs.end() && session.engine_busy_run_id != h.run_id) {
          rit->second->TryTerminal(RunState::kCancelled);
        } else if (rit != session.runs.end() && rit->second->state != RunState::kCancelRequested) {
          rit->second->state = RunState::kCancelRequested;
          RequestCancel(session, h.run_id);
        }
      }
    }
    response = protocol::BuildAckFrame(h);
  }
  conn.request_cache.Record(identity, payload, payload_size, response);
  if (!conn.Enqueue(std::move(response)))
    return false;
  if (ready) {
    if (!conn.Enqueue(protocol::BuildSessionReadyFrame(ReplyHeader(conn, h.target_id))))
      return false;
    std::lock_guard<std::mutex> lock(conn.mutex);
    auto &session = *conn.sessions.at(h.target_id);
    std::lock_guard<std::mutex> session_lock(session.mutex);
    session.state = SessionState::kLogicalReady;
  }
  if (admitted_worker) {
    // A spawn failure answers ACK, then WORKER_EXIT, as a session's first spawn.
    if (!Spawn(*admitted_worker))
      CloseWorker(conn, h.target_id, RunState::kWorkerExited, true);
    else if (::WaitForSingleObject(conn.stop, 0) == WAIT_OBJECT_0)
      CloseWorker(conn, h.target_id, RunState::kBrokerLost, false);
    else
      Pump(conn);
  }
  if (runtime_host) {
    ScheduleRuntimes(*runtime_host);
    Dispatch(*runtime_host);
  }
  if (admitted_run) {
    if (admitted_run->error != protocol::StatusCode::OK)
      admitted_run->TryTerminal(RunState::kFailed);
    else if (needs_spawn && !Spawn(*admitted_session->worker)) {
      admitted_run->error = protocol::StatusCode::ERROR_INTERNAL;
      admitted_run->TryTerminal(RunState::kFailed);
      CloseSession(conn, h.target_id, RunState::kWorkerExited);
    } else if (::WaitForSingleObject(conn.stop, 0) == WAIT_OBJECT_0) {
      if (!admitted_session->explicit_runtime)
        CloseSession(conn, h.target_id, RunState::kBrokerLost);
    } else {
      Pump(conn);
      auto it = conn.sessions.find(h.target_id);
      if (it != conn.sessions.end())
        Dispatch(*it->second);
    }
  }
  if (api_)
    for (auto &[id, session] : conn.sessions)
      session->retired_runs.clear();
  Reclaim(conn, false);
  return true;
}
Connection &Worker::Conn() const {
  return conn ? *conn : *session->conn;
}
void Worker::Publish(uint16_t slot, uint32_t id, std::shared_ptr<RelayLedger> ledger) {
  if (ledger) {
    ledger->run_id = id;
    ledger_pins.push_back(ledger);
  }
  for (auto &entry : published[slot]) {
    auto *current = entry.load();
    if ((current && current->run_id == id) || (ledger && !current)) {
      entry.store(ledger.get());
      break;
    }
  }
  // Park tests after the slot update, inside publication/withdrawal work.
#if defined(V8HOST_ROUTER_TESTING)
  if (ledger_publication) ledger_publication();
#endif
  // Sequentially consistent withdrawal/hazard/recheck prevents reclaim while
  // the reader acquires shared ownership. It never retries or waits.
  auto *borrowed = ledger_reader.load();
  std::erase_if(ledger_pins, [&](const auto &pin) {
    if (pin.get() == borrowed) return false;
    for (const auto &row : published)
      for (const auto &entry : row)
        if (entry.load() == pin.get()) return false;
    return true;
  });
}
void Worker::DiscardMailbox() {
  mailbox_failed = true;
  while (intake_active.load()) ::SwitchToThread();
  const size_t tail = mailbox_tail.load(std::memory_order_acquire);
  for (size_t head = mailbox_head.load(); head != tail; ++head)
    mailbox[head % mailbox.size()].credit.Reset();
  mailbox_head = tail; byte_head = byte_tail.load();
}
void SBOX_CDECL Worker::OnMessage(void *context, sbox_msg_kind kind, const void *data, size_t size) {
  auto &worker = *static_cast<Worker *>(context);
  auto &conn = worker.Conn();
  // One core reader produces; ordinary strand bookkeeping never excludes it.
#if defined(V8HOST_ROUTER_TESTING)
  if (worker.intake_entered) worker.intake_entered();
#endif
  worker.intake_active.store(true);
  if ((worker.session && worker.session->worker.get() != &worker) || worker.mailbox_failed.load()) {
    Worker *owner = worker.session ? worker.session->worker.get() : &worker;
    if (owner) owner->mailbox_failed = true;
    worker.intake_active.store(false);
    ::SetEvent(conn.worker_wake);
    return;
  }
  const size_t tail = worker.mailbox_tail.load(std::memory_order_relaxed);
  const size_t byte_tail = worker.byte_tail.load(std::memory_order_relaxed);
  bool valid = size <= protocol::kMaxFramePayload && (!size || data) &&
      tail - worker.mailbox_head.load(std::memory_order_acquire) < worker.mailbox.size() &&
      size <= worker.mailbox_bytes.size() - (byte_tail - worker.byte_head.load(std::memory_order_acquire));
  OutputCredit credit;
  if (valid) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    std::shared_ptr<RelayLedger> ledger;
    size_t charged = size + protocol::kFrameHeaderSize;
    bool late = false;
    uint16_t slot = 0;
    uint32_t id = 0;
    if (kind == sbox_msg_kind_binary && size >= v8host::kRunEnvelopeHeaderSize + 4 && v8host::IsRunEnvelope(bytes, size) &&
        bytes[4] == static_cast<uint8_t>(v8host::RunEnvelopeType::kRelay) && !bytes[5] &&
        bytes[6] < v8host::kMaxRuntimeSlots && !bytes[7] &&
        (bytes[12] == sbox_msg_kind_string || bytes[12] == sbox_msg_kind_binary) && !bytes[13] && !bytes[14] && !bytes[15]) {
      slot = bytes[6];
      id = bytes[8] | (uint32_t(bytes[9]) << 8) | (uint32_t(bytes[10]) << 16) | (uint32_t(bytes[11]) << 24);
      for (const auto &entry : worker.published[slot]) {
        auto *candidate = entry.load();
        worker.ledger_reader.store(candidate);
        if (candidate && entry.load() == candidate) {
#if defined(V8HOST_ROUTER_TESTING)
          if (worker.ledger_acquired) worker.ledger_acquired();
#endif
          if (candidate->run_id == id) ledger = candidate->shared_from_this();
        }
        worker.ledger_reader.store(nullptr);
        if (ledger) break;
      }
      late = !ledger;
      charged = size + protocol::kFrameHeaderSize - v8host::kRunEnvelopeHeaderSize;
    }
    // Unknown envelopes still reach validation; only admitted retired ids drop.
    if (late && id && id <= worker.slot_highest_run_id[slot].load()) {
      worker.intake_active.store(false);
      return;
    }
    if (kind == sbox_msg_kind_lifecycle && size == 4) charged = protocol::kFrameHeaderSize;
    credit = conn.ChargeOutput(charged, std::move(ledger));
    // A run overflow is owned by its ledger and does not close sibling sessions.
    if (!credit.conn && ::WaitForSingleObject(conn.stop, 0) != WAIT_OBJECT_0) {
      worker.intake_active.store(false);
      ::SetEvent(conn.worker_wake);
      return;
    }
    valid = credit.conn != nullptr;
  }
  if (!valid) worker.mailbox_failed = true;
  else {
    auto &fact = worker.mailbox[tail % worker.mailbox.size()];
    fact.kind = kind; fact.size = size; fact.offset = byte_tail;
    fact.credit = std::move(credit);
    const size_t offset = byte_tail % worker.mailbox_bytes.size();
    const size_t first = (std::min)(size, worker.mailbox_bytes.size() - offset);
    if (first) std::memcpy(worker.mailbox_bytes.data() + offset, data, first);
    if (size > first) std::memcpy(worker.mailbox_bytes.data(), static_cast<const uint8_t *>(data) + first, size - first);
    worker.byte_tail.store(byte_tail + size, std::memory_order_release);
    worker.mailbox_tail.store(tail + 1, std::memory_order_release);
  }
  worker.intake_active.store(false);
  ::SetEvent(conn.worker_wake);
}
DWORD WINAPI Worker::Reap(void *context) {
  auto &worker = *static_cast<Worker *>(context);
  int32_t exit_code = 0;
  worker.api->close(worker.handle);
  worker.api->wait(worker.handle, &exit_code);
  worker.handle = nullptr;
  worker.state = WorkerState::kReaped;
  ::SetEvent(worker.Conn().worker_wake);
  return 0;
}
bool Router::Spawn(Worker &worker) {
  // Keep the original hard process cap. Pump retries this logical admission
  // when a cleanup task signals worker_wake and its reservation is reclaimed.
  if (LiveWorkerCount(worker.Conn()) >= protocol::kMaxSessionsPerConnection)
    return true;
  Session *session = worker.session;
  if (session) session->state = SessionState::kStartingWorker;
  worker.state = WorkerState::kSpawning;
  const BoundProfile &profile = session ? session->profile : worker.profile;
  const SessionConfig &config = session ? session->config : worker.config;
  DWORD error = 0;
  int count = ::MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      profile.engine_dll.data(),
      static_cast<int>(profile.engine_dll.size()),
      nullptr,
      0);
  if (!count)
    return false;
  std::wstring engine(count, L'\0');
  ::MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      profile.engine_dll.data(),
      static_cast<int>(profile.engine_dll.size()),
      engine.data(),
      count);
  std::vector<uint8_t> spawn_config;
  // payload_directory_ is a VOLUME_NAME_DOS path; held files carry volume-GUID
  // final paths, so containment is checked against the canonical install root.
  if (!OpenImmutableFile(payload_directory_ + L"\\" + engine, &worker.engine_image, &error) ||
      !IsPayloadFileAllowed(install_root_, worker.engine_image) ||
      worker.engine_image.machine() != machine_ || !TrustAllowed(VerifyTrust(worker.engine_image)) ||
      !PrepareSpawnConfig(config, profile, &spawn_config, worker.explicit_worker ? worker.max_runtimes : 0,
                          worker.explicit_worker ? &worker.snapshots : nullptr))
    return false;
  sbox_broker_worker handle = nullptr;
  if (api_->configure_and_spawn(broker_, spawn_config.data(), spawn_config.size(), &Worker::OnMessage, &worker,
                                &handle) != sbox_status_ok ||
      !handle)
    return false;
  worker.handle = handle;
  if (api_->get_process_id(handle, &worker.process_id) != sbox_status_ok || !worker.process_id)
    return false;
  worker.state = WorkerState::kWarming;
  return true;
}
std::chrono::steady_clock::time_point Router::Now() const {
#if defined(V8HOST_ROUTER_TESTING)
  if (test_now)
    return test_now();
#endif
  return std::chrono::steady_clock::now();
}
DWORD Router::WakeDelay(const Connection &conn, DWORD maximum) const {
  const auto now = Now();
  auto consider = [&](std::chrono::steady_clock::time_point deadline) {
    if (deadline == std::chrono::steady_clock::time_point{}) return;
    const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    maximum = (std::min)(maximum, milliseconds <= 0 ? DWORD(0) : static_cast<DWORD>(milliseconds));
  };
  for (const auto &[id, session] : conn.sessions) {
    if (!session->host) continue;
    consider(session->cancel_deadline);
    if (session->worker) consider(session->worker->retry_deadline);
  }
  for (const auto &[id, worker] : conn.workers) {
    consider(worker->retry_deadline);
    for (const auto &record : worker->slots) consider(record.close_deadline);
  }
  return maximum;
}
void Router::RequestCancel(Session &session, uint32_t id) {
  if (session.cancel_deadline != std::chrono::steady_clock::time_point{}) return;
  session.cancel_deadline = Now() + kCancelFallbackTimeout;
  session.pending_cancel_id = id;
  auto run = session.runs.find(id);
  if (run != session.runs.end()) {
    session.conn->in_relay_bytes -= run->second->queued_relay_bytes;
    run->second->queued_relay_bytes = 0; run->second->held_relays.clear();
  }
  if (Worker *worker = session.host) {
    // Cancellation retires unsent relays before replacing their retry head.
    worker->retry_deadline = {}; worker->retry_attempts = 0; worker->retry_run_id = 0;
    Dispatch(*worker);
  }
}
PostOutcome Router::Post(Worker &worker, const std::vector<uint8_t> &bytes) {
  if (bytes.size() > protocol::kMaxFramePayload || !worker.handle || worker.close_called ||
      ::WaitForSingleObject(worker.Conn().stop, 0) == WAIT_OBJECT_0) return PostOutcome::kAmbiguous;
#if defined(V8HOST_ROUTER_TESTING)
  if (test_post) return test_post(bytes);
#endif
  const auto status = api_->post_message(worker.handle, sbox_msg_kind_binary, bytes.data(), bytes.size());
  // The generic host adapter maps core not-written failures to sbox_status_failed;
  // this requires a validated size and an unreaped handle.
  return status == sbox_status_ok ? PostOutcome::kWritten
      : status == sbox_status_failed ? PostOutcome::kNotWritten : PostOutcome::kAmbiguous;
}
bool Router::Send(Worker &worker, const std::vector<uint8_t> &bytes, uint16_t slot, uint32_t id,
                  v8host::RunEnvelopeType type) {
  const bool retry = worker.retry_deadline != std::chrono::steady_clock::time_point{};
  if (retry && Now() < worker.retry_deadline) return false;
  const auto outcome = Post(worker, bytes);
  if (outcome == PostOutcome::kWritten) {
    worker.retry_deadline = {}; worker.retry_attempts = 0; worker.retry_run_id = 0;
    return true;
  }
  if (retry) ++worker.retry_attempts;
  if (outcome == PostOutcome::kAmbiguous || worker.retry_attempts == kPostRetryAttempts) {
    CloseWorkerOf(worker);
    return false;
  }
  worker.retry_run_id = id; worker.retry_slot = slot; worker.retry_type = type;
  const auto delay = (std::min)(kPostRetryCap, kPostRetryInitial * (1u << (std::min)(worker.retry_attempts, 4u)));
  worker.retry_deadline = Now() + delay;
  return false;
}
void Router::Dispatch(Session &session) {
  if (session.host) Dispatch(*session.host);
}
// Runtime control first (closes, then creates, in slot order), then each ready
// runtime's runs. A blocked post holds the rest: the ring is shared.
void Router::Dispatch(Worker &worker) {
  if (!worker.security || worker.close_called || !worker.handle) return;
  for (uint16_t slot = 0; slot < worker.slots.size(); ++slot) {
    auto &record = worker.slots[slot];
    v8host::RunEnvelope env;
    env.slot = slot;
    if (record.closing && !record.close_posted) {
      env.type = v8host::RunEnvelopeType::kCloseRuntime;
      if (!Send(worker, v8host::EncodeRunEnvelope(env), slot, 0, env.type)) return;
      record.close_posted = true;
    } else if (record.runtime && record.runtime->runtime_state == RuntimeState::kCreating && !record.create_posted) {
      env.type = v8host::RunEnvelopeType::kCreateRuntime;
      env.snapshot_index = record.runtime->snapshot_index;
      if (!Send(worker, v8host::EncodeRunEnvelope(env), slot, 0, env.type)) return;
      record.create_posted = true;
    }
  }
  for (auto &record : worker.slots)
    if (record.runtime && record.runtime->runtime_state == RuntimeState::kReady && !DispatchRuntime(*record.runtime))
      return;
}
// One START at a time per runtime, its pending cancel, and its held relays
// while the runtime has relay credit. Returns false if a post was blocked.
bool Router::DispatchRuntime(Session &runtime) {
  Worker &worker = *runtime.host;
  const uint16_t slot = runtime.slot;
  if (runtime.pending_cancel_id) {
    v8host::RunEnvelope env;
    env.type = v8host::RunEnvelopeType::kCancel; env.slot = slot; env.run_id = runtime.pending_cancel_id;
    if (!Send(worker, v8host::EncodeRunEnvelope(env), slot, env.run_id, env.type)) return false;
    runtime.pending_cancel_id = 0;
  }
  if (!runtime.engine_busy_run_id && !runtime.pending_run_ids.empty()) {
    const uint32_t id = runtime.pending_run_ids.front();
    auto it = runtime.runs.find(id);
    if (it == runtime.runs.end()) { runtime.pending_run_ids.pop_front(); return true; }
    v8host::RunEnvelope env;
    env.slot = slot; env.run_id = id; env.payload = it->second->guest;
    if (!Send(worker, v8host::EncodeRunEnvelope(env), slot, id, env.type)) return false;
    runtime.pending_run_ids.pop_front();
    runtime.engine_busy_run_id = runtime.active_run_id = id;
    it->second->guest.clear();
    it->second->state = RunState::kActive;
  }
  auto it = runtime.runs.find(runtime.engine_busy_run_id);
  if (it == runtime.runs.end()) return true;
  auto &run = *it->second;
  for (size_t posted = 0; posted < 256 && !run.held_relays.empty() && runtime.credit; ++posted) {
    auto &frame = run.held_relays.front();
    frame[6] = static_cast<uint8_t>(slot);  // held before the runtime had a slot
    frame[7] = static_cast<uint8_t>(slot >> 8);
    if (!Send(worker, frame, slot, run.run_id, v8host::RunEnvelopeType::kRelay)) return false;
    runtime.conn->in_relay_bytes -= frame.size();
    run.queued_relay_bytes -= static_cast<uint32_t>(frame.size());
    run.held_relays.pop_front();
    --runtime.credit;
  }
  return true;
}
// Gives queued runtimes free slots, in ascending target ID order, once the
// worker has proven lockdown (design §2.1, §9.3).
void Router::ScheduleRuntimes(Worker &worker) {
  if (!worker.explicit_worker || !worker.security || worker.close_called) return;
  std::vector<Session *> queued;
  for (auto &[id, session] : worker.conn->sessions)
    if (session->explicit_runtime && session->host == &worker && session->runtime_state == RuntimeState::kQueued)
      queued.push_back(session.get());
  std::sort(queued.begin(), queued.end(), [](Session *a, Session *b) { return a->session_id < b->session_id; });
  for (Session *runtime : queued) {
    uint16_t slot = 0;
    while (slot < worker.max_runtimes && worker.slots[slot].occupied) ++slot;
    if (slot == worker.max_runtimes) break;
    auto &record = worker.slots[slot];
    record = Worker::SlotRecord{};
    record.runtime = runtime;
    record.occupied = true;
    runtime->slot = slot;
    runtime->runtime_state = RuntimeState::kCreating;
    runtime->credit = 0;  // granted with RUNTIME_READY
    worker.slot_highest_run_id[slot] = runtime->highest_run_id.load();
    for (auto &[run_id, run] : runtime->runs)
      worker.Publish(slot, run_id, run->outbound);
  }
}
void Router::CloseWorkerOf(Worker &worker) {
  if (worker.explicit_worker)
    CloseWorker(worker.Conn(), worker.worker_id, RunState::kWorkerExited, true);
  else if (worker.session)
    CloseSession(worker.Conn(), worker.session->session_id, RunState::kWorkerExited);
}
void Router::CloseSession(Connection &conn, uint32_t id, RunState terminal) {
  std::lock_guard<std::mutex> lock(conn.mutex);
  auto it = conn.sessions.find(id);
  if (it == conn.sessions.end() || it->second->explicit_runtime)
    return;
  Session *session = it->second.get();
  bool has_worker = session->worker != nullptr;
  if (has_worker) session->worker->DiscardMailbox();
  RetireSessionLocked(conn, id, terminal);
  if (terminal == RunState::kWorkerExited)
    conn.Enqueue(protocol::BuildWorkerExitFrame(ReplyHeader(conn, id)));
  if (!has_worker)
    return;
  auto &worker = *session->worker;
  session->cancel_deadline = {}; session->pending_cancel_id = 0; session->engine_busy_run_id = 0;
  worker.retry_deadline = {}; worker.retry_attempts = 0; worker.retry_run_id = 0;
  if (worker.handle && !worker.close_called) {
    worker.close_called = true;
    worker.state = WorkerState::kCloseRequested;
    worker.wait_called = true;
    worker.cleanup = ::CreateThread(nullptr, 0, &Worker::Reap, &worker, 0, nullptr);
    if (!worker.cleanup)
      conn.Stop();
  }
}
// Ends an explicit worker and every runtime and run in it. Only an unasked end
// reports WORKER_EXIT; the client fans it out (design §6.5).
void Router::CloseWorker(Connection &conn, uint32_t id, RunState terminal, bool report_exit) {
  std::unique_ptr<Worker> owned;
  {
    std::lock_guard<std::mutex> lock(conn.mutex);
    auto it = conn.workers.find(id);
    if (it == conn.workers.end())
      return;
    Worker &worker = *it->second;
    worker.DiscardMailbox();
    std::vector<uint32_t> runtimes;
    for (auto &[runtime_id, session] : conn.sessions)
      if (session->explicit_runtime && session->host == &worker)
        runtimes.push_back(runtime_id);
    for (uint32_t runtime_id : runtimes)
      RetireSessionLocked(conn, runtime_id, terminal, /*detach_host=*/true);
    worker.slots = {};
    worker.retry_deadline = {}; worker.retry_attempts = 0; worker.retry_run_id = 0;
    if (report_exit)
      conn.Enqueue(protocol::BuildWorkerExitFrame(ReplyHeader(conn, id)));
    owned = std::move(it->second);
    conn.workers.erase(it);
    if (owned->handle && !owned->close_called) {
      owned->close_called = true;
      owned->state = WorkerState::kCloseRequested;
      owned->wait_called = true;
      owned->cleanup = ::CreateThread(nullptr, 0, &Worker::Reap, owned.get(), 0, nullptr);
      if (!owned->cleanup)
        conn.Stop();
    }
    if (owned->handle || owned->cleanup)
      conn.retired_workers.push_back(std::move(owned));
  }
}
// A host close of an explicit runtime: its runs end now and its admission is
// released, but its slot stays occupied until the worker's RUNTIME_CLOSED.
void Router::CloseRuntime(Connection &conn, uint32_t id) {
  Worker *worker = nullptr;
  {
    std::lock_guard<std::mutex> lock(conn.mutex);
    auto it = conn.sessions.find(id);
    if (it == conn.sessions.end() || !it->second->explicit_runtime)
      return;
    worker = it->second->host;
    const RuntimeState state = it->second->runtime_state;
    const uint16_t slot = it->second->slot;
    RetireSessionLocked(conn, id, RunState::kCancelled, /*detach_host=*/true);
    if (worker && state != RuntimeState::kQueued) {
      auto &record = worker->slots[slot];
      if (state == RuntimeState::kCreating && !record.create_posted) {
        record = Worker::SlotRecord{};  // the worker never saw it
      } else {
        record.runtime = nullptr;
        record.closing = true;
        record.close_posted = false;
        record.close_deadline = Now() + kCancelFallbackTimeout;
      }
    }
  }
  if (worker) {
    ScheduleRuntimes(*worker);
    Dispatch(*worker);
  }
}
void Router::Reclaim(Connection &conn, bool join) {
  for (auto it = conn.retired_sessions.begin(); it != conn.retired_sessions.end();) {
    auto &session = **it;
    auto *worker = session.worker.get();
    if (join && worker && worker->wait_called && !worker->cleanup && worker->handle)
      Worker::Reap(worker);
    if (worker && worker->cleanup && ::WaitForSingleObject(worker->cleanup, join ? INFINITE : 0) == WAIT_OBJECT_0) {
      ::CloseHandle(worker->cleanup);
      worker->cleanup = nullptr;
      it = conn.retired_sessions.erase(it);
    } else if (api_ && (!worker || (!worker->cleanup && !worker->handle))) {
      // Failed spawn returns only after any unpublished callback reader has joined.
      it = conn.retired_sessions.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = conn.retired_workers.begin(); it != conn.retired_workers.end();) {
    auto &worker = **it;
    if (join && worker.wait_called && !worker.cleanup && worker.handle)
      Worker::Reap(&worker);
    if (worker.cleanup && ::WaitForSingleObject(worker.cleanup, join ? INFINITE : 0) == WAIT_OBJECT_0) {
      ::CloseHandle(worker.cleanup);
      worker.cleanup = nullptr;
      it = conn.retired_workers.erase(it);
    } else if (api_ && !worker.cleanup && !worker.handle) {
      it = conn.retired_workers.erase(it);
    } else {
      ++it;
    }
  }
}
// A runtime the worker ended unasked: its runs end without frames (the client
// fans RUNTIME_CLOSED out to them), and its slot is free again.
void Router::RuntimeFailed(Connection &conn, Worker &worker, Session &runtime) {
  const uint32_t id = runtime.session_id;
  {
    std::lock_guard<std::mutex> lock(conn.mutex);
    worker.slots[runtime.slot] = Worker::SlotRecord{};
    RetireSessionLocked(conn, id, RunState::kWorkerExited, /*detach_host=*/true);
  }
  protocol::RuntimeClosedPayload closed;
  conn.Enqueue(protocol::BuildRuntimeClosedFrame(ReplyHeader(conn, id), closed));
  ScheduleRuntimes(worker);
}
// One worker frame other than a lifecycle phase. Returns false for malformed or
// out-of-state output, which closes the worker.
bool Router::WorkerEnvelope(Connection &conn, Worker &worker, v8host::RunEnvelope &env, Worker::Fact &fact) {
  if (env.slot >= worker.max_runtimes)
    return false;
  auto &record = worker.slots[env.slot];
  if (!record.occupied)
    return false;  // a frame for a slot with no runtime
  Session *runtime = record.runtime;  // null while the slot is closing
  switch (env.type) {
    case v8host::RunEnvelopeType::kRuntimeReady:
      if (record.closing)
        return true;  // crossed CLOSE_RUNTIME
      if (!runtime || !runtime->explicit_runtime || runtime->runtime_state != RuntimeState::kCreating ||
          !record.create_posted)
        return false;
      runtime->runtime_state = RuntimeState::kReady;
      runtime->credit = v8host::kRelayCreditWindow;
      conn.Enqueue(protocol::BuildRuntimeReadyFrame(ReplyHeader(conn, runtime->session_id)));
      return true;
    case v8host::RunEnvelopeType::kRuntimeClosed:
      if (record.closing) {
        record = Worker::SlotRecord{};
        ScheduleRuntimes(worker);
        return true;
      }
      // The worker reports a runtime it ended itself only as failed.
      if (!runtime || !runtime->explicit_runtime || env.status_code == 0)
        return false;
      RuntimeFailed(conn, worker, *runtime);
      return true;
    case v8host::RunEnvelopeType::kCredit:
      if (record.closing || !runtime)
        return true;
      if (runtime->runtime_state != RuntimeState::kReady)
        return false;
      runtime->credit += env.credit;
      return runtime->credit <= v8host::kRelayCreditWindow;
    case v8host::RunEnvelopeType::kRelay:
    case v8host::RunEnvelopeType::kResult:
    case v8host::RunEnvelopeType::kRunError:
      break;
    default:
      return false;  // only the coordinator sends the other types
  }
  if (env.run_id == 0 ||
      (env.type == v8host::RunEnvelopeType::kRelay && env.relay_kind != sbox_msg_kind_string &&
       env.relay_kind != sbox_msg_kind_binary))
    return false;
  if (!runtime)
    return true;  // late output from a closing slot
  if (runtime->runtime_state != RuntimeState::kReady)
    return false;
  if (env.run_id != runtime->engine_busy_run_id && env.run_id <= runtime->highest_run_id)
    return true;  // late output from a retired run
  if (env.run_id != runtime->engine_busy_run_id)
    return false;
  auto rit = runtime->runs.find(env.run_id);
  if (rit != runtime->runs.end() && rit->second->outbound->failed.load()) {
    rit->second->error = protocol::StatusCode::ERROR_QUOTA;
    rit->second->TryTerminal(RunState::kFailed);
    if (env.type == v8host::RunEnvelopeType::kRelay) RequestCancel(*runtime, env.run_id);
    rit = runtime->runs.end();
  }
  if (env.type == v8host::RunEnvelopeType::kRelay) {
    if (rit != runtime->runs.end()) {
      auto header = ReplyHeader(conn, runtime->session_id, env.run_id);
      header.type = protocol::MessageType::RELAY_FROM_WORKER;
      auto frame = protocol::BuildRelayFrame(header, env.relay_kind, env.payload.data(), env.payload.size());
      auto credit = std::move(fact.credit);
      if (!credit.conn) credit = conn.ChargeOutput(frame.size(), rit->second->outbound);
      if (credit.conn) conn.Enqueue(std::move(frame), std::move(credit));
      else if (rit->second->outbound->failed) {
        rit->second->error = protocol::StatusCode::ERROR_QUOTA;
        rit->second->TryTerminal(RunState::kFailed);
        RequestCancel(*runtime, env.run_id);
      }
    }
    return true;
  }
  runtime->cancel_deadline = {}; runtime->pending_cancel_id = 0; runtime->engine_busy_run_id = 0;
  worker.retry_deadline = {}; worker.retry_attempts = 0; worker.retry_run_id = 0;
  if (rit != runtime->runs.end()) {
    if (env.type == v8host::RunEnvelopeType::kRunError) {
      rit->second->error = protocol::StatusCode::ERROR_INTERNAL;
      rit->second->TryTerminal(RunState::kFailed);
    } else {
      rit->second->TryTerminal(
          env.disposition == v8host::RunEnvelopeDisposition::kCompleted ? RunState::kCompleted
                                                                        : RunState::kCancelled);
    }
  }
  return true;
}
void Router::PumpWorker(Connection &conn, Worker &worker) {
  auto alive = [&] {
    return worker.explicit_worker ? conn.workers.contains(worker.worker_id)
                                  : conn.sessions.contains(worker.worker_id);
  };
  if (!worker.handle)
    return;
  if (worker.mailbox_failed.load()) {
    CloseWorkerOf(worker);
    return;
  }
  // A run whose output overflowed its ledger fails alone.
  for (auto &record : worker.slots) {
    Session *runtime = record.runtime;
    if (!runtime) continue;
    std::vector<uint32_t> overflowed;
    for (const auto &[run_id, run] : runtime->runs)
      if (run->outbound->failed) overflowed.push_back(run_id);
    for (uint32_t run_id : overflowed) {
      if (!alive() || record.runtime != runtime) break;
      auto rit = runtime->runs.find(run_id);
      if (rit == runtime->runs.end()) continue;
      rit->second->error = protocol::StatusCode::ERROR_QUOTA;
      rit->second->TryTerminal(RunState::kFailed);
      if (runtime->engine_busy_run_id == run_id) RequestCancel(*runtime, run_id);
    }
    if (!alive()) return;
  }
  Worker::Fact fact;
  std::array<uint8_t, protocol::kMaxFramePayload> data;
  for (size_t drained = 0; drained < worker.mailbox.size(); ++drained) {
    const size_t head = worker.mailbox_head.load(std::memory_order_relaxed);
    const size_t tail = worker.mailbox_tail.load(std::memory_order_acquire);
    if (worker.mailbox_failed.load(std::memory_order_acquire) || head == tail)
      break;
    auto &slot = worker.mailbox[head % worker.mailbox.size()];
    fact.kind = slot.kind;
    fact.size = slot.size;
    fact.offset = slot.offset;
    fact.credit = std::move(slot.credit);
    const size_t offset = slot.offset % worker.mailbox_bytes.size();
    const size_t first = (std::min)(slot.size, worker.mailbox_bytes.size() - offset);
    std::memcpy(data.data(), worker.mailbox_bytes.data() + offset, first);
    if (slot.size > first) std::memcpy(data.data() + first, worker.mailbox_bytes.data(), slot.size - first);
#if defined(V8HOST_ROUTER_TESTING)
    if (worker.mailbox_readout)
      worker.mailbox_readout();
#endif
    worker.byte_head.store(fact.offset + fact.size, std::memory_order_release);
    worker.mailbox_head.store(head + 1, std::memory_order_release);
    if (worker.mailbox_failed.load(std::memory_order_acquire))
      break;
    bool valid = true;
    if (!fact.credit.relay) fact.credit.Reset();
    if (fact.kind == sbox_msg_kind_lifecycle) {
      uint32_t phase = 0;
      if (fact.size == 4)
        phase = data[0] | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
      if (phase == sbox_lifecycle_phase_startup) {
        if (!worker.startup) {
          worker.startup = true;
          worker.engine_image = HeldFile();
          if (worker.session) worker.session->state = SessionState::kStartupReady;
          conn.Enqueue(protocol::BuildStartupReadyFrame(ReplyHeader(conn, worker.worker_id),
              protocol::StartupReadyPayload{protocol::kMessageSchemaVersion, 0, worker.process_id}));
        }
      } else if (phase == sbox_lifecycle_phase_security && worker.startup) {
        if (!worker.security) {
          worker.security = true;
          if (worker.session) worker.session->state = SessionState::kOpen;
          worker.state = WorkerState::kRunning;
          conn.Enqueue(protocol::BuildSecurityReadyFrame(ReplyHeader(conn, worker.worker_id)));
          ScheduleRuntimes(worker);
        }
      } else if (phase == sbox_lifecycle_phase_exit) {
        CloseWorkerOf(worker);
        break;
      } else {
        valid = false;
      }
    } else {
      v8host::RunEnvelope env;
      valid = fact.kind == sbox_msg_kind_binary && v8host::DecodeRunEnvelope(data.data(), fact.size, &env) &&
          worker.security && WorkerEnvelope(conn, worker, env, fact);
    }
    if (!alive())
      return;
    if (!valid) {
      CloseWorkerOf(worker);
      return;
    }
  }
  if (worker.mailbox_failed.load(std::memory_order_acquire)) {
    CloseWorkerOf(worker);
    return;
  }
  // A runaway runtime ends its whole worker after 5 s (design §7.3).
  const auto now = Now();
  for (const auto &record : worker.slots) {
    const bool cancel_expired = record.runtime &&
        record.runtime->cancel_deadline != std::chrono::steady_clock::time_point{} &&
        now >= record.runtime->cancel_deadline;
    const bool close_expired = record.closing &&
        record.close_deadline != std::chrono::steady_clock::time_point{} && now >= record.close_deadline;
    if (cancel_expired || close_expired) {
      CloseWorkerOf(worker);
      return;
    }
  }
  if (worker.mailbox_head.load(std::memory_order_relaxed) != worker.mailbox_tail.load(std::memory_order_acquire))
    ::SetEvent(conn.worker_wake);
  Dispatch(worker);
  // Only the strand borrows production run storage.
  if (alive() && worker.session)
    worker.session->retired_runs.clear();
}
void Router::Pump(Connection &conn) {
  Reclaim(conn, false);
  std::vector<uint32_t> sessions, workers;
  for (auto &[id, session] : conn.sessions)
    if (session->worker)
      sessions.push_back(id);
  for (auto &[id, worker] : conn.workers)
    workers.push_back(id);
  for (uint32_t id : sessions) {
    auto it = conn.sessions.find(id);
    if (it != conn.sessions.end() && it->second->worker) {
      if (it->second->worker->state == WorkerState::kNone && !Spawn(*it->second->worker)) {
        CloseSession(conn, id, RunState::kWorkerExited);
        continue;
      }
      PumpWorker(conn, *it->second->worker);
    }
  }
  for (uint32_t id : workers) {
    auto it = conn.workers.find(id);
    if (it != conn.workers.end()) {
      if (it->second->state == WorkerState::kNone && !Spawn(*it->second)) {
        CloseWorker(conn, id, RunState::kWorkerExited, true);
        continue;
      }
      PumpWorker(conn, *it->second);
    }
  }
  Reclaim(conn, false);
#if defined(V8HOST_ROUTER_TESTING)
  if (test_pump_complete)
    test_pump_complete();
#endif
}
void Router::Close(Connection &conn) {
  conn.Stop();
  conn.state = ConnState::kClosing;
  while (!conn.workers.empty())
    CloseWorker(conn, conn.workers.begin()->first, RunState::kBrokerLost, false);
  while (!conn.sessions.empty()) {
    const uint32_t id = conn.sessions.begin()->first;
    if (conn.sessions.begin()->second->explicit_runtime) {
      std::lock_guard<std::mutex> lock(conn.mutex);
      RetireSessionLocked(conn, id, RunState::kBrokerLost, /*detach_host=*/true);
    } else {
      CloseSession(conn, id, RunState::kBrokerLost);
    }
  }
  Reclaim(conn, true);
  conn.dispatcher = nullptr;
  conn.state = ConnState::kClosed;
}
} // namespace v8host::coordinator
