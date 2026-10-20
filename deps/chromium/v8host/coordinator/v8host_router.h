// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#ifndef V8HOST_COORDINATOR_V8HOST_ROUTER_H_
#define V8HOST_COORDINATOR_V8HOST_ROUTER_H_

#include "sbox.h"
#include "v8host_file_identity.h"
#include "v8host_peer_auth.h"
#include "v8host_protocol_messages.h"
#include "v8host_run_envelope.h"
#include "v8host_spawn_config.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#if defined(V8HOST_ROUTER_TESTING)
#include <functional>
#endif

namespace v8host::coordinator {
namespace protocol = v8host::protocol;
enum class ConnState { kAuthenticating, kNegotiating, kOpen, kClosing, kClosed };
enum class SessionState {
  kCreating,
  kLogicalReady,
  kBindingProfile,
  kStartingWorker,
  kStartupReady,
  kSecurityReady,
  kOpen,
  kClosing,
  kClosed
};
enum class WorkerState { kNone, kSpawning, kWarming, kLockedDown, kRunning, kCloseRequested, kWaiting, kReaped };
enum class RunState {
  kCreated,
  kStartPending,
  kActive,
  kCancelRequested,
  kCompleted,
  kFailed,
  kCancelled,
  kBrokerLost,
  kWorkerExited
};
inline bool IsTerminal(RunState s) {
  return s >= RunState::kCompleted;
}
// An explicit runtime's lifecycle (design §2.1). A session's runtime is always
// kReady in slot 0.
enum class RuntimeState { kQueued, kCreating, kReady };
struct Connection;
struct Session;
struct Worker;
class Router;
using SessionConfig = protocol::CreateSessionPayload;
struct BoundProfile {
  int32_t effective_tier = 0;
  std::string engine_dll;
  std::string snapshot_path;
  bool prohibit_dynamic_code = false;
};
struct RelayLedger : std::enable_shared_from_this<RelayLedger> {
  uint32_t run_id = 0;
  std::atomic<size_t> bytes{0};
  std::atomic<bool> failed{false};
};
// A charge moves with the bytes, including the in-flight writer frame.
struct OutputCredit {
  Connection *conn = nullptr;
  std::shared_ptr<RelayLedger> relay;
  size_t bytes = 0;
  OutputCredit() = default;
  OutputCredit(const OutputCredit &) = delete;
  OutputCredit &operator=(const OutputCredit &) = delete;
  OutputCredit(OutputCredit &&) noexcept;
  OutputCredit &operator=(OutputCredit &&) noexcept;
  ~OutputCredit();
  void Reset();
};
struct OutputFrame {
  std::vector<uint8_t> bytes;
  OutputCredit credit;
};
struct Run {
  uint32_t run_id = 0;
  std::atomic<RunState> state{RunState::kCreated};
  uint32_t queued_relay_bytes = 0;
  std::shared_ptr<RelayLedger> outbound = std::make_shared<RelayLedger>();
  Session *session = nullptr;
  protocol::StatusCode error = protocol::StatusCode::ERROR_INTERNAL;
  std::vector<uint8_t> guest;
  std::deque<std::vector<uint8_t>> held_relays;
  bool TryTerminal(RunState desired);
};
enum class PostOutcome { kWritten, kNotWritten, kAmbiguous };
// One sandboxed worker process: a session's private worker, or an explicit
// worker that hosts up to max_runtimes runtimes, each in a slot.
struct Worker {
  struct Fact {
    sbox_msg_kind kind = sbox_msg_kind_binary;
    size_t size = 0;
    size_t offset = 0;
    OutputCredit credit;
  };
  // A worker-local runtime slot. A closing slot keeps its isolate until the
  // worker reports RUNTIME_CLOSED, so it stays occupied without a runtime.
  struct SlotRecord {
    Session *runtime = nullptr;
    bool occupied = false;
    bool create_posted = false;
    bool closing = false;
    bool close_posted = false;
    std::chrono::steady_clock::time_point close_deadline{};
  };
  Session *session = nullptr;  // a session's private worker: that session
  Connection *conn = nullptr;
  // The owning connection; a session's worker reaches it through its session.
  Connection &Conn() const;
  uint32_t worker_id = 0;      // its target ID (the session's for a private worker)
  bool explicit_worker = false;
  const sbox_broker_api *api = nullptr;
  HANDLE cleanup = nullptr;
  // One core reader produces; the connection strand consumes. Counters wrap unsigned.
  static_assert(std::atomic<size_t>::is_always_lock_free);
  std::atomic<bool> mailbox_failed{false};
  std::array<Fact, 256> mailbox;
  std::array<uint8_t, protocol::kMaxQueuedRelayBytesPerRun> mailbox_bytes;
  std::atomic<size_t> byte_head{0}, byte_tail{0};
  std::atomic<bool> intake_active{false};
  static_assert(std::atomic<RelayLedger *>::is_always_lock_free);
  // The intake slots, per runtime slot, matched on run_id.
  std::array<std::array<std::atomic<RelayLedger *>, protocol::kMaxInFlightRunsPerSession>,
             v8host::kMaxRuntimeSlots> published{};
  // Each slot's highest admitted run ID, for the reader's late-frame check.
  std::array<std::atomic<uint32_t>, v8host::kMaxRuntimeSlots> slot_highest_run_id{};
  std::atomic<RelayLedger *> ledger_reader{nullptr};
  // At most the admitted ledgers plus the single reader's hazard are retained.
  std::vector<std::shared_ptr<RelayLedger>> ledger_pins;
  void Publish(uint16_t slot, uint32_t id, std::shared_ptr<RelayLedger> ledger);
  void DiscardMailbox();
  std::atomic<size_t> mailbox_head{0}, mailbox_tail{0};
#if defined(V8HOST_ROUTER_TESTING)
  std::function<void()> mailbox_readout, ledger_publication, intake_entered, ledger_acquired;
#endif
  // The ring is shared, so a blocked post holds every slot's dispatch.
  std::chrono::steady_clock::time_point retry_deadline{};
  unsigned retry_attempts = 0;
  uint32_t retry_run_id = 0;
  uint16_t retry_slot = 0;
  v8host::RunEnvelopeType retry_type = v8host::RunEnvelopeType::kStart;
  uint32_t process_id = 0;
  bool startup = false, security = false;
  static void SBOX_CDECL OnMessage(void *, sbox_msg_kind, const void *, size_t);
  static DWORD WINAPI Reap(void *);
  sbox_broker_worker handle = nullptr;
  std::atomic<WorkerState> state{WorkerState::kNone};
  // The strand reserves one cleanup task; that task closes and waits off-reader.
  bool close_called = false;
  bool wait_called = false;
  HeldFile engine_image;
  // Explicit workers: the profile bound at CREATE_WORKER.
  SessionConfig config;
  BoundProfile profile;
  uint32_t max_runtimes = 1;
  bool single_use = false;
  uint32_t created_runtimes = 0;
  std::vector<std::string> snapshots;  // payload-relative, canonical
  std::array<SlotRecord, v8host::kMaxRuntimeSlots> slots{};
};
// A run target: a session (which owns its private worker and is its one
// runtime, in slot 0) or an explicit runtime hosted by an explicit worker.
struct Session {
  uint32_t session_id = 0;  // the target ID
  Connection *conn = nullptr;
  // State and worker publication are strand-owned after admission.
  SessionState state = SessionState::kCreating;
  SessionConfig config;
  std::atomic<uint32_t> highest_run_id{0};
  bool profile_bound = false;
  BoundProfile profile;
  std::unique_ptr<Worker> worker;  // a session's private worker
  Worker *host = nullptr;          // the worker this runtime lives in
  bool explicit_runtime = false;
  RuntimeState runtime_state = RuntimeState::kReady;
  uint16_t slot = 0;
  int32_t snapshot_index = -1;
  // Per-runtime run serialization: one START at a time, in FIFO order.
  uint32_t credit = v8host::kRelayCreditWindow;
  uint32_t engine_busy_run_id = 0;
  uint32_t active_run_id = 0;
  uint32_t pending_cancel_id = 0;
  std::deque<uint32_t> pending_run_ids;
  std::chrono::steady_clock::time_point cancel_deadline{};
  std::mutex mutex;
  std::unordered_map<uint32_t, std::unique_ptr<Run>> runs;
  // Retain removed objects until their contenders have joined.
  std::vector<std::unique_ptr<Run>> retired_runs;
#if defined(V8HOST_ROUTER_TESTING)
  std::function<void()> terminal_removed;
#endif
};
struct Connection {
  Connection();
  ~Connection();
  Connection(const Connection &) = delete;
  Connection &operator=(const Connection &) = delete;
  HANDLE pipe = INVALID_HANDLE_VALUE;
  HeldProcess peer;
  uint32_t conn_id = 0;
  uint16_t version_minor = protocol::kWireVersionMinor;
  ConnState state = ConnState::kAuthenticating;
  protocol::RequestCache request_cache;
  // Lock order: connection, session, output. Callbacks never take these locks.
  std::mutex mutex;
  // Run targets: sessions and explicit runtimes, keyed by target ID.
  std::unordered_map<uint32_t, std::unique_ptr<Session>> sessions;
  // The highest target ID this connection has created (one namespace).
  uint32_t highest_session_id = 0;
  std::vector<std::unique_ptr<Session>> retired_sessions;
  std::unordered_map<uint32_t, std::unique_ptr<Worker>> workers;  // explicit workers
  std::vector<std::unique_ptr<Worker>> retired_workers;
  std::mutex out_mutex;
  std::deque<OutputFrame> out_queue;
  std::atomic<size_t> out_control_bytes{0}, out_control_requests{0}, out_relay_bytes{0};
  size_t in_relay_bytes = 0;
  HANDLE out_wake = nullptr;
  HANDLE stop = nullptr;
  HANDLE worker_wake = nullptr;
  Router *dispatcher = nullptr;
  OutputCredit ChargeOutput(size_t bytes, std::shared_ptr<RelayLedger> relay = {});
  bool Enqueue(std::vector<uint8_t> frame, OutputCredit credit = {});
  void ClearOutput();
  bool TakeOutput(std::vector<uint8_t> *frame);
  void CompleteOutput(size_t bytes);
  bool DrainOutput(DWORD timeout_ms);
  void Stop();
  bool StartWriter();
  void JoinWriter();
  bool ReadsPaused() const;
  bool CompleteZero() const;
  bool ReadFrame(uint8_t *buffer, DWORD capacity, DWORD *size, DWORD timeout_ms);
#if defined(V8HOST_ROUTER_TESTING)
  HANDLE read_pending = nullptr;
  HANDLE read_paused = nullptr, read_resumed = nullptr;
  HANDLE write_pending = nullptr;
  HANDLE writer_release = nullptr;
  std::atomic<uint32_t> active_io{0};
  std::atomic<uint32_t> inline_reads{0};
  std::atomic<uint32_t> writers_started{0};
  std::atomic<uint32_t> writers_joined{0};
#endif
 private:
  static DWORD WINAPI WriteTask(void *context);
  std::condition_variable out_drained_;
  HANDLE writer_ = nullptr;
  OutputCredit in_flight_;
};

uint32_t AllocateId(std::atomic<uint32_t> &next);
bool EnvelopeFits(v8host::RunEnvelopeType type, size_t payload_size);
bool ValidateSessionConfig(const SessionConfig &config, BrokerMode mode);
bool PrepareProfile(
    const SessionConfig &config,
    const protocol::StartRunPayload &inputs,
    const std::wstring &payload_directory,
    BoundProfile *out);
protocol::StatusCode BindProfile(Session &session, const BoundProfile &profile);
// An explicit worker's spawn config passes max_runtimes (1..16) and its
// payload-relative snapshots; a session passes 0 and none.
bool PrepareSpawnConfig(const SessionConfig &config, const BoundProfile &profile, std::vector<uint8_t> *out,
                        uint32_t max_runtimes = 0, const std::vector<std::string> *snapshots = nullptr);
// Validates and canonicalizes an explicit worker's profile (tier default
// engine, payload-relative snapshots, the 4 KiB profile bound).
protocol::StatusCode PrepareWorkerProfile(const protocol::CreateWorkerPayload &payload,
                                          const std::wstring &payload_directory,
                                          BoundProfile *profile,
                                          std::vector<std::string> *snapshots);
class Router {
 public:
  // install_root is the broker's canonical install root (the same final-path
  // form as HeldFile::final_path); engine images must lie beneath it.
  explicit Router(
      BrokerMode mode,
      sbox_broker broker = nullptr,
      const sbox_broker_api *api = nullptr,
      std::wstring payload_directory = L"",
      uint16_t machine = 0,
      std::wstring install_root = L"")
      : mode_(mode), broker_(broker), api_(api), payload_directory_(std::move(payload_directory)), machine_(machine),
        install_root_(std::move(install_root)) {}
  void Pump(Connection &conn);
  DWORD WakeDelay(const Connection &conn, DWORD maximum) const;
#if defined(V8HOST_ROUTER_TESTING)
  std::function<std::chrono::steady_clock::time_point()> test_now;
  std::function<void()> test_pump_complete;
  std::function<PostOutcome(const std::vector<uint8_t> &)> test_post;
#endif
  bool Route(Connection &conn, const uint8_t *data, size_t size);
  void Close(Connection &conn);

 private:
  std::chrono::steady_clock::time_point Now() const;
  void RequestCancel(Session &session, uint32_t id);
  bool Spawn(Worker &worker);
  PostOutcome Post(Worker &worker, const std::vector<uint8_t> &bytes);
  bool Send(Worker &worker, const std::vector<uint8_t> &bytes, uint16_t slot, uint32_t id, v8host::RunEnvelopeType type);
  void Dispatch(Session &session);
  void Dispatch(Worker &worker);
  bool DispatchRuntime(Session &runtime);
  void ScheduleRuntimes(Worker &worker);
  void PumpWorker(Connection &conn, Worker &worker);
  bool WorkerEnvelope(Connection &conn, Worker &worker, v8host::RunEnvelope &env, Worker::Fact &fact);
  void RuntimeFailed(Connection &conn, Worker &worker, Session &runtime);
  void CloseWorkerOf(Worker &worker);
  void CloseSession(Connection &conn, uint32_t id, RunState terminal);
  void CloseWorker(Connection &conn, uint32_t id, RunState terminal, bool report_exit);
  void CloseRuntime(Connection &conn, uint32_t id);
  void Reclaim(Connection &conn, bool join);
  BrokerMode mode_;
  sbox_broker broker_;
  const sbox_broker_api *api_;
  std::wstring payload_directory_;
  uint16_t machine_;
  std::wstring install_root_;
};
} // namespace v8host::coordinator
#endif // V8HOST_COORDINATOR_V8HOST_ROUTER_H_
