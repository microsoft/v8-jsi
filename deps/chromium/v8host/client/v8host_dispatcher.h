// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// App-thread callback dispatcher for the v8host client persona (Contract C,
// design sections 9.1/9.3). This is an IN-DLL implementation detail, not the
// public consumer ABI: it marshals session/run/relay/disconnect callbacks from
// internal I/O threads onto the single app/callback thread captured by the
// first initialize, through a message-only window and bounded per-session FIFO
// queues. The public v8host_client_* surface drives this engine;
// the externally observable semantics are fixed by design section 9, but this
// internal API shape is ours.
//
// The threading contract realized here:
//   - Process-idempotent, thread-safe initialization captures the CALLING
//     thread as the callback thread and creates a message-only window on it;
//     racing initializers wait on a condition variable (never pumping).
//   - Producers POST work (never block) to the window; callbacks run ONLY on
//     the app thread, drained by the window procedure, never under a lock.
//   - Each session has a bounded FIFO; overflow fails the session with one
//     terminal v8host_status_callback_backpressure and drops later events.
//   - Each queued event carries the callback-table snapshot captured when it
//     was queued; set_callbacks atomically replaces the table for FUTURE posts.
//   - close marks the session closed immediately, suppresses pending callbacks,
//     and defers destruction (refcount) until any in-progress callback unwinds;
//     reentrant close/post/set_callbacks from inside a callback are supported.
//   - A per-run arbiter enforces exactly one app-visible terminal run event.

#ifndef V8HOST_CLIENT_V8HOST_DISPATCHER_H_
#define V8HOST_CLIENT_V8HOST_DISPATCHER_H_

#include "v8host_client.h"  // public Contract C types (handles, callbacks, status)
#include "v8host_window_tokens.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace v8host::client {

#ifdef V8HOST_DISPATCHER_TESTING
// Compiled only into test executables that separately compile the dispatcher.
// Fails the next callback-table copy at the actual SessionDispatch boundary.
void FailNextCallbackCopyForTesting();
#endif

// Fixed, documented cap on the number of callback events buffered per session
// before the app thread must drain. Reaching it means the consumer stopped
// pumping its message loop; the session is failed with one terminal
// v8host_status_callback_backpressure rather than growing unbounded or blocking an
// I/O thread. 256 is large enough to absorb normal bursts yet bounds a stalled
// consumer to a small, fixed footprint.
inline constexpr size_t kSessionQueueCapacity = 256;

// Broker-mode caps (design §6.4): each runtime, counting its runs and its own
// state events, may have kRuntimeQueueCapacity queued callbacks; the whole
// broker kBrokerQueueCapacity events and kBrokerQueueBytes of copied relay
// bytes.
inline constexpr size_t kRuntimeQueueCapacity = 256;
inline constexpr size_t kBrokerQueueCapacity = 4096;
inline constexpr size_t kBrokerQueueBytes = 16u << 20;

// What a broker-mode post did besides queuing its event.
enum class PostOverflow : uint8_t { kNone, kRuntime, kBroker };

class SessionDispatch;
class RunTerminalArbiter;

// Process-global owner of the app/callback thread and its message-only window.
// A single instance (Instance()) backs the whole process; it is intentionally
// never destroyed (process-lifetime window). Separate instances exist only for
// unit tests of the initialization arbiter.
class Dispatcher {
 public:
  // The process-global dispatcher the client persona uses.
  static Dispatcher& Instance();

  Dispatcher();
  ~Dispatcher();
  Dispatcher(const Dispatcher&) = delete;
  Dispatcher& operator=(const Dispatcher&) = delete;

  // Process-idempotent, thread-safe. The FIRST successful call captures the
  // CALLING thread as the callback thread and creates the message-only window
  // on it (the caller must own a Windows message queue). Later calls succeed
  // without changing the captured thread. Calls racing the first initialize
  // from another thread wait on a condition variable until it finishes; they do
  // not pump messages. Returns v8host_status_ok on success, v8host_status_internal_error if the
  // window could not be created.
  v8host_status Initialize();

  bool IsInitialized() const { return ready_.load(std::memory_order_acquire); }

  // Thread id of the captured callback thread (0 before initialization). Used
  // by tests to assert callbacks arrive on the app thread.
  uint32_t callback_thread_id() const {
    return thread_id_.load(std::memory_order_acquire);
  }

  // Destroys the message-only window. MUST be called on the callback thread.
  // Only for test instances; the process-global instance is never shut down.
  void ShutdownForTesting();

  // Posts a drain request for `session` to the message-only window. Returns
  // false if the post failed (not initialized, or the OS message queue is
  // full). Internal; called by SessionDispatch when it schedules a drain.
  bool PostDrain(SessionDispatch* session);
  void DispatchToken(uint32_t message, uintptr_t low, intptr_t high) {
    tokens_.Dispatch(message, low, high);
  }
#ifdef V8HOST_DISPATCHER_TESTING
  uint64_t PostTokenProbeForTesting(std::atomic<int>* count);
  void PostTokenForTesting(uint64_t token);
  uint64_t IgnoredTokensForTesting() const { return tokens_.ignored(); }
#endif

  // ---- Id-keyed producer API: the ONLY cross-thread producer surface --------
  //
  // Producers (internal I/O threads) post session/run/relay/disconnect events
  // and replace the callback table by SESSION ID, never through a raw
  // SessionDispatch*. Memory-safe from any thread: each call resolves the id
  // under `sessions_mutex_`, AddRef's the target into a scoped reference while
  // the lookup is still locked, releases the lock, THEN dispatches, and finally
  // Releases the scoped reference. A session whose owner called CloseAndRelease
  // is unregistered first (also under `sessions_mutex_`), so a racing producer
  // either acquires a live reference before the removal or finds nothing after
  // it and drops with v8host_status_invalid_state -- it never dereferences a freed
  // object. `sessions_mutex_` is always released before the per-session `mutex_`
  // is taken, so there is no lock-order inversion with UnregisterSession.
  //
  // Each returns v8host_status_invalid_state if `id` is unknown (never created or
  // already closed); otherwise the status of the underlying per-session call.
  v8host_status PostSessionState(uint64_t id, int32_t state, v8host_status status);
  v8host_status PostRunEvent(uint64_t id,
                            v8host_run_s* run,
                            RunTerminalArbiter* arbiter,
                            bool terminal,
                            int32_t event,
                            v8host_status status);
  v8host_status PostRelay(uint64_t id,
                         v8host_run_s* run,
                         int32_t kind,
                         const void* data,
                         size_t len);
  v8host_status PostDisconnect(uint64_t id);
  v8host_status SetCallbacks(uint64_t id, const v8host_callbacks* callbacks);

  // Session registry management (internal). AllocateSessionId + RegisterSession
  // are called by SessionDispatch::Create; UnregisterSession by CloseAndRelease.
  // RegisterSession/UnregisterSession take `sessions_mutex_`; the erase in
  // UnregisterSession serializes with the find+AddRef in the id-keyed producer
  // API above to defeat the cross-thread use-after-free.
  uint64_t AllocateSessionId();
  void RegisterSession(uint64_t id, SessionDispatch* session);
  void UnregisterSession(uint64_t id);

 private:
  bool CreateDispatchWindow();
  WindowTokens tokens_;

  // Resolves `id` to a live session under `sessions_mutex_`, AddRef'ing it
  // before the lock is released (nullptr if unknown). The caller owns the
  // returned reference and must Release it.
  SessionDispatch* AcquireSession(uint64_t id);

  enum class State : uint8_t { kUninitialized, kInitializing, kReady, kFailed };

  mutable std::mutex init_mutex_;
  std::condition_variable init_cv_;
  State state_ = State::kUninitialized;

  // Hot-path fields read by producer threads without the init lock. Published
  // (release) once the window exists; consumed (acquire) by IsInitialized /
  // PostDrain / callback_thread_id.
  std::atomic<bool> ready_{false};
  std::atomic<uint32_t> thread_id_{0};
  std::atomic<void*> hwnd_{nullptr};  // HWND; kept Win32-type-free in the header

  uint16_t class_atom_ = 0;  // ATOM
  std::wstring class_name_;

  // Live, registered sessions keyed by id. The map holds a raw pointer (no
  // reference): the invariant is that an entry is erased under `sessions_mutex_`
  // (UnregisterSession, from CloseAndRelease) BEFORE the creator reference is
  // dropped, so a session found under the lock is guaranteed alive long enough
  // to AddRef. `sessions_mutex_` is never held across a per-session `mutex_`
  // (no lock-order inversion).
  std::mutex sessions_mutex_;
  std::unordered_map<uint64_t, SessionDispatch*> sessions_;
  std::atomic<uint64_t> next_session_id_{1};
};

// Enforces exactly one app-visible terminal run event. The client's Run owns
// one of these; it is passed to PostRunEvent for terminal events. The first
// terminal attempt wins (compare/exchange); later terminal attempts are
// dropped. Non-terminal events do not touch the arbiter.
class RunTerminalArbiter {
 public:
  RunTerminalArbiter() = default;
  RunTerminalArbiter(const RunTerminalArbiter&) = delete;
  RunTerminalArbiter& operator=(const RunTerminalArbiter&) = delete;

  // Returns true exactly once (for the first caller); false thereafter.
  bool TryClaimTerminal() {
    bool expected = false;
    return claimed_.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel);
  }

  bool terminal_claimed() const {
    return claimed_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<bool> claimed_{false};
};

// Per-session callback dispatch: a bounded FIFO of pending events delivered on
// the app thread, atomic callback-table snapshots, backpressure, close
// suppression, and deferred (refcounted) destruction.
//
// Lifetime: Create() returns an object with one reference owned by the caller
// (the client's session handle) and registers it with the Dispatcher under an
// integer id. CloseAndRelease() (owner-only) unregisters the id, then
// relinquishes that reference; the object survives any in-progress drain and is
// destroyed once the last reference (the drain's) is released. Producers never
// hold a raw SessionDispatch pointer across threads: they post by id through the
// Dispatcher, which resolves the id to a counted reference under a lock (see the
// Dispatcher id-keyed producer API), so a post can never touch a freed object.
class SessionDispatch {
 public:
  // Creates a session dispatch bound to `dispatcher` (which must be
  // initialized). `handle` is the opaque v8host_session_s* echoed to on_session_
  // state (a cookie to this layer; the public client binds it to the real handle).
  // `failed_state` is the session-state code delivered with the synthetic
  // backpressure terminal (the client's "session failed/closed" state value).
  // Returns nullptr on bad arguments or allocation failure (fail closed).
  static SessionDispatch* Create(Dispatcher* dispatcher,
                                 v8host_session_s* handle,
                                 int32_t failed_state);

  // Creates a broker's dispatch: one FIFO for every event of the broker, its
  // workers, runtimes, and runs, in a single global order, delivered through
  // the broker's table (copied here, fixed for the broker's life).
  static SessionDispatch* CreateBroker(Dispatcher* dispatcher,
                                       v8host_broker_s* broker,
                                       const v8host_broker_callbacks& table);

  enum class EventKind : uint8_t {
    kSessionState,
    kRunEvent,
    kRelay,
    kDisconnect,
    kBrokerState,
    kWorkerState,
    kRuntimeState,
  };

  // A broker-mode event. `object` is the worker or runtime a state event names
  // (and, for run and relay events, the run's runtime). worker_id/runtime_id
  // key the per-runtime cap and close suppression. The event keeps `holder`
  // alive until it is delivered or dropped, then calls release(holder).
  struct BrokerPost {
    EventKind kind = EventKind::kBrokerState;
    void* object = nullptr;
    v8host_run_s* run = nullptr;
    int32_t code = 0;
    v8host_status status = v8host_status_ok;
    const void* data = nullptr;
    size_t size = 0;
    uint32_t worker_id = 0, runtime_id = 0;
    bool terminal = false;
    RunTerminalArbiter* arbiter = nullptr;
    void* holder = nullptr;
    void (*release)(void*) = nullptr;
  };

  // Queues a broker-mode event; the caller holds the broker's lock. Returns as
  // the session posts do. On a runtime's overflow its pending events are
  // dropped and one runtime closed(callback_backpressure) is queued; on the
  // broker's overflow everything is dropped and one
  // disconnected(callback_backpressure) is queued, and later posts fail.
  // *overflow says which happened.
  v8host_status PostBroker(BrokerPost&& post, PostOverflow* overflow);

  // Drops every queued event of a closed runtime (runtime_id != 0) or of a
  // closed worker and all its runtimes and runs.
  void Suppress(uint32_t worker_id, uint32_t runtime_id);

  SessionDispatch(const SessionDispatch&) = delete;
  SessionDispatch& operator=(const SessionDispatch&) = delete;

  void AddRef();
  void Release();

  // Stable integer id assigned at Create() and registered with the owning
  // Dispatcher. Producers use this id (never a raw pointer) to post through the
  // Dispatcher's id-keyed API from any thread; an integer is safe to hold or go
  // stale across threads where a freed pointer is not.
  uint64_t id() const { return id_; }

  // Marks the session closed immediately (suppressing all pending, not-yet-
  // delivered callbacks) and relinquishes the creator's reference. Called by the
  // OWNER, which holds the creator reference, so the raw `this` is alive for the
  // call. It first unregisters the id from the Dispatcher (under
  // `sessions_mutex_`) so no further producer can resolve it, then marks closed
  // and suppresses the queue (session `mutex_`), then drops the creator
  // reference. Non-blocking; safe from inside a callback. Destruction is deferred
  // until any in-progress drain unwinds. The caller must not touch the object
  // after this returns.
  void CloseAndRelease();

  // Owner-only accessors: call only while holding the creator reference (the
  // client's session handle). They are not part of the cross-thread producer
  // surface — producers post by id through the Dispatcher.
  bool closed() const;

  v8host_session_s* handle() const { return handle_; }

  // Diagnostic: number of live SessionDispatch objects, for leak/use-after-free
  // checks in tests.
  static int live_count();

  // Invoked by the dispatch window procedure on the app thread. Consumes one
  // reference (the one taken when the drain was scheduled).
  static void DrainFromMessage(SessionDispatch* session);

 private:
  friend class Dispatcher;

  // Per-session producers and table replacement. NOT a cross-thread surface:
  // these are reached ONLY through the Dispatcher's id-keyed API, which resolves
  // a session id to a counted reference under `sessions_mutex_` before calling
  // them (Dispatcher::Post*/SetCallbacks). A raw SessionDispatch* can therefore
  // never be posted to from another thread.

  // Atomically replaces the callback table for FUTURE dispatch; already-queued
  // events keep the snapshot captured when they were queued. The table (and its
  // user_data) are copied; no consumer pointer is retained. NULL members decline
  // their callback. Rejects a null pointer (INVALID_ARG), an undersized struct
  // (STRUCT_SIZE), or a closed session (INVALID_STATE).
  v8host_status SetCallbacks(const v8host_callbacks* callbacks);

  // Each snapshots the current table, enqueues one event, and schedules an
  // app-thread drain. Return v8host_status_ok when accepted,
  // v8host_status_callback_backpressure if the queue overflowed (the session is now
  // failed), v8host_status_run_terminal if a terminal run event lost arbitration,
  // v8host_status_invalid_state if the session is closed, or v8host_status_internal_error if the
  // drain could not be scheduled.
  v8host_status PostSessionState(int32_t state, v8host_status status);
  v8host_status PostRunEvent(v8host_run_s* run,
                            RunTerminalArbiter* arbiter,
                            bool terminal,
                            int32_t event,
                            v8host_status status);
  v8host_status PostRelay(v8host_run_s* run,
                         int32_t kind,
                         const void* data,
                         size_t len);
  v8host_status PostDisconnect();

  struct Event {
    EventKind kind = EventKind::kSessionState;
    v8host_callbacks table{};  // snapshot captured at post time (zero = decline)
    v8host_run_s* run = nullptr;
    void* object = nullptr;  // broker mode: the worker or runtime named
    int32_t code = 0;  // session state / run event / relay kind
    v8host_status status = v8host_status_ok;
    std::vector<uint8_t> data;  // owned copy of a relay payload
    uint32_t worker_id = 0, runtime_id = 0;
    void* holder = nullptr;
    void (*release)(void*) = nullptr;

    Event() = default;
    Event(Event&& other) noexcept { *this = std::move(other); }
    Event& operator=(Event&& other) noexcept;
    ~Event() { Drop(); }
    void Drop();
  };

  SessionDispatch(Dispatcher* dispatcher,
                  uint64_t id,
                  v8host_session_s* handle,
                  int32_t failed_state);
  ~SessionDispatch();

  v8host_status Enqueue(Event&& event,
                       bool terminal_run,
                       RunTerminalArbiter* arbiter);
  bool EnsureScheduledLocked();
  void OnPostFailed();
  void DrainLoop();
  void Invoke(const Event& event);
  // Broker-mode accounting for an event entering (+1) or leaving (-1) queue_.
  void Account(const Event& event, int delta);
  template <typename Predicate>
  void EraseLocked(Predicate&& matches);
  void ClearLocked();

  Dispatcher* const dispatcher_;
  const uint64_t id_;
  v8host_session_s* const handle_;
  const int32_t failed_state_;
  // Broker mode (null broker_ for a session).
  v8host_broker_s* broker_ = nullptr;
  v8host_broker_callbacks broker_table_{};
  std::unordered_map<uint32_t, size_t> runtime_counts_;
  size_t queued_bytes_ = 0;

  std::atomic<int> refcount_{1};

  mutable std::mutex mutex_;
  v8host_callbacks current_table_{};  // live table; copied into each event
  std::deque<Event> queue_;
  bool closed_ = false;
  bool backpressure_failed_ = false;
  // True while a drain message is in flight for this session. Coalesces many
  // posts into one outstanding WM_DRAIN; cleared only when a drain observes an
  // empty queue (or the session closed) under the lock.
  bool scheduled_ = false;
};

}  // namespace v8host::client

#endif  // V8HOST_CLIENT_V8HOST_DISPATCHER_H_
