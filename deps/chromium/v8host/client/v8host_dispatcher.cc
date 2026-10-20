// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "v8host_dispatcher.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <new>
#include <string>

// The current module (DLL or test exe); used as the window-class hInstance so
// registration/creation stay inside this module.
extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace v8host::client {
namespace {

// Private drain capability. The table, never a message parameter, holds the
// SessionDispatch reference released by DrainFromMessage.
constexpr UINT kDrainMessage = WM_APP + 0x1D;

HINSTANCE ModuleInstance() {
  return reinterpret_cast<HINSTANCE>(&__ImageBase);
}

LRESULT CALLBACK DispatchWndProc(HWND hwnd,
                                 UINT message,
                                 WPARAM wparam,
                                 LPARAM lparam) {
  if (message == kDrainMessage) {
    auto* owner = reinterpret_cast<Dispatcher*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (owner) owner->DispatchToken(message, wparam, lparam);
    return 0;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

std::atomic<int>& LiveCounter() {
  static std::atomic<int> counter{0};
  return counter;
}

#ifdef V8HOST_DISPATCHER_TESTING
std::atomic<bool>& FailNextCallbackCopy() {
  static std::atomic<bool> fail{false};
  return fail;
}
#endif

// RAII holder for a counted SessionDispatch reference. The id-keyed producer API
// AddRef's the target under `sessions_mutex_` (so the lookup cannot race the
// object's destruction), then dispatches through this holder, which Releases the
// reference when it leaves scope.
class SessionRef {
 public:
  explicit SessionRef(SessionDispatch* session) : session_(session) {}
  ~SessionRef() {
    if (session_ != nullptr)
      session_->Release();
  }
  SessionRef(const SessionRef&) = delete;
  SessionRef& operator=(const SessionRef&) = delete;
  explicit operator bool() const { return session_ != nullptr; }
  SessionDispatch* operator->() const { return session_; }

 private:
  SessionDispatch* session_;
};

}  // namespace

#ifdef V8HOST_DISPATCHER_TESTING
void FailNextCallbackCopyForTesting() {
  FailNextCallbackCopy().store(true, std::memory_order_release);
}
#endif

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

Dispatcher& Dispatcher::Instance() {
  // Intentionally leaked: the message-only window lives for the whole process,
  // so there is no safe time (and no correct thread) to run a destructor.
  static Dispatcher* const instance = new Dispatcher();
  return *instance;
}

Dispatcher::Dispatcher() = default;

Dispatcher::~Dispatcher() = default;

v8host_status Dispatcher::Initialize() {
  std::unique_lock<std::mutex> lock(init_mutex_);
  for (;;) {
    switch (state_) {
      case State::kReady:
        return v8host_status_ok;
      case State::kFailed:
        return v8host_status_internal_error;
      case State::kInitializing:
        // Another thread is creating the window. Wait without pumping: a
        // condition variable parks this thread until the first initializer
        // publishes a terminal state.
        init_cv_.wait(lock,
                      [this] { return state_ != State::kInitializing; });
        continue;
      case State::kUninitialized:
        state_ = State::kInitializing;
        // Release the lock across the Win32 work so racing callers can observe
        // kInitializing and wait on the condition variable.
        lock.unlock();
        const bool ok = CreateDispatchWindow();
        lock.lock();
        state_ = ok ? State::kReady : State::kFailed;
        init_cv_.notify_all();
        return ok ? v8host_status_ok : v8host_status_internal_error;
    }
  }
}

bool Dispatcher::CreateDispatchWindow() {
  if (!tokens_.Initialize()) return false;
  // Captures the calling thread as the callback thread (design 9.1).
  thread_id_.store(::GetCurrentThreadId(), std::memory_order_release);

  // Unique per-instance class name so independent Dispatcher instances (e.g.
  // the process-global one plus any under test) never collide on registration.
  class_name_ = L"V8HostDispatch_" +
                std::to_wstring(reinterpret_cast<uintptr_t>(this));

  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = &DispatchWndProc;
  window_class.hInstance = ModuleInstance();
  window_class.lpszClassName = class_name_.c_str();
  class_atom_ = ::RegisterClassExW(&window_class);
  if (class_atom_ == 0)
    return false;

  HWND hwnd = ::CreateWindowExW(0, class_name_.c_str(), L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, ModuleInstance(),
                                nullptr);
  if (hwnd == nullptr) {
    ::UnregisterClassW(class_name_.c_str(), ModuleInstance());
    class_atom_ = 0;
    return false;
  }

  ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  hwnd_.store(hwnd, std::memory_order_release);
  ready_.store(true, std::memory_order_release);
  return true;
}

void Dispatcher::ShutdownForTesting() {
  HWND hwnd =
      reinterpret_cast<HWND>(hwnd_.exchange(nullptr, std::memory_order_acq_rel));
  ready_.store(false, std::memory_order_release);
  if (hwnd != nullptr)
    ::DestroyWindow(hwnd);  // must run on the creating (callback) thread
  if (class_atom_ != 0) {
    ::UnregisterClassW(class_name_.c_str(), ModuleInstance());
    class_atom_ = 0;
  }
}

bool Dispatcher::PostDrain(SessionDispatch* session) {
  HWND hwnd = reinterpret_cast<HWND>(hwnd_.load(std::memory_order_acquire));
  if (hwnd == nullptr)
    return false;
  return tokens_.Post(hwnd, kDrainMessage, session, [](void* value) {
    SessionDispatch::DrainFromMessage(static_cast<SessionDispatch*>(value));
  }) != 0;
}

#ifdef V8HOST_DISPATCHER_TESTING
uint64_t Dispatcher::PostTokenProbeForTesting(std::atomic<int>* count) {
  return tokens_.Post(reinterpret_cast<HWND>(hwnd_.load()), kDrainMessage, count, [](void* value) {
    static_cast<std::atomic<int>*>(value)->fetch_add(1);
  });
}
void Dispatcher::PostTokenForTesting(uint64_t token) {
  ::PostMessageW(reinterpret_cast<HWND>(hwnd_.load()), kDrainMessage,
      static_cast<WPARAM>(static_cast<uint32_t>(token)),
      static_cast<LPARAM>(static_cast<uint32_t>(token >> 32)));
}
#endif

// ---------------------------------------------------------------------------
// Dispatcher -- session registry + id-keyed producer API
// ---------------------------------------------------------------------------

uint64_t Dispatcher::AllocateSessionId() {
  return next_session_id_.fetch_add(1, std::memory_order_relaxed);
}

void Dispatcher::RegisterSession(uint64_t id, SessionDispatch* session) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  sessions_[id] = session;
}

void Dispatcher::UnregisterSession(uint64_t id) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  sessions_.erase(id);
}

SessionDispatch* Dispatcher::AcquireSession(uint64_t id) {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  auto it = sessions_.find(id);
  if (it == sessions_.end())
    return nullptr;
  // AddRef before releasing sessions_mutex_: UnregisterSession erases under the
  // same lock, so we either AddRef before the owner's erase (the object stays
  // alive for this post) or never get here (find fails after the erase). The
  // returned reference is owned by the caller.
  it->second->AddRef();
  return it->second;
}

// Id-keyed producer API: resolve the id to a counted reference (AddRef'd under
// sessions_mutex_), release that lock, dispatch to the per-session method, then
// Release on scope exit. An unknown id (never created or already closed) drops
// with v8host_status_invalid_state -- no freed object is ever dereferenced.
v8host_status Dispatcher::PostSessionState(uint64_t id,
                                          int32_t state,
                                          v8host_status status) {
  SessionRef session(AcquireSession(id));
  if (!session)
    return v8host_status_invalid_state;
  return session->PostSessionState(state, status);
}

v8host_status Dispatcher::PostRunEvent(uint64_t id,
                                      v8host_run_s* run,
                                      RunTerminalArbiter* arbiter,
                                      bool terminal,
                                      int32_t event,
                                      v8host_status status) {
  SessionRef session(AcquireSession(id));
  if (!session)
    return v8host_status_invalid_state;
  return session->PostRunEvent(run, arbiter, terminal, event, status);
}

v8host_status Dispatcher::PostRelay(uint64_t id,
                                   v8host_run_s* run,
                                   int32_t kind,
                                   const void* data,
                                   size_t len) {
  SessionRef session(AcquireSession(id));
  if (!session)
    return v8host_status_invalid_state;
  return session->PostRelay(run, kind, data, len);
}

v8host_status Dispatcher::PostDisconnect(uint64_t id) {
  SessionRef session(AcquireSession(id));
  if (!session)
    return v8host_status_invalid_state;
  return session->PostDisconnect();
}

v8host_status Dispatcher::SetCallbacks(uint64_t id,
                                      const v8host_callbacks* callbacks) {
  SessionRef session(AcquireSession(id));
  if (!session)
    return v8host_status_invalid_state;
  return session->SetCallbacks(callbacks);
}

// ---------------------------------------------------------------------------
// SessionDispatch
// ---------------------------------------------------------------------------

SessionDispatch* SessionDispatch::Create(Dispatcher* dispatcher,
                                         v8host_session_s* handle,
                                         int32_t failed_state) {
  if (dispatcher == nullptr || !dispatcher->IsInitialized())
    return nullptr;
  const uint64_t id = dispatcher->AllocateSessionId();
  auto* session =
      new (std::nothrow) SessionDispatch(dispatcher, id, handle, failed_state);
  if (session == nullptr)
    return nullptr;
  // Register under sessions_mutex_ so the id-keyed producer API can resolve this
  // session. The creator reference (refcount 1) is returned to and owned by the
  // caller; CloseAndRelease unregisters before dropping it.
  dispatcher->RegisterSession(id, session);
  return session;
}

SessionDispatch* SessionDispatch::CreateBroker(Dispatcher* dispatcher,
                                               v8host_broker_s* broker,
                                               const v8host_broker_callbacks& table) {
  SessionDispatch* dispatch = Create(dispatcher, nullptr, 0);
  if (dispatch == nullptr)
    return nullptr;
  dispatch->broker_ = broker;
  dispatch->broker_table_ = table;
  return dispatch;
}

SessionDispatch::Event& SessionDispatch::Event::operator=(Event&& other) noexcept {
  if (this != &other) {
    Drop();
    kind = other.kind;
    table = other.table;
    run = other.run;
    object = other.object;
    code = other.code;
    status = other.status;
    data = std::move(other.data);
    worker_id = other.worker_id;
    runtime_id = other.runtime_id;
    holder = other.holder;
    release = other.release;
    other.holder = nullptr;
    other.release = nullptr;
  }
  return *this;
}

void SessionDispatch::Event::Drop() {
  if (release != nullptr && holder != nullptr)
    release(holder);
  holder = nullptr;
  release = nullptr;
}

void SessionDispatch::Account(const Event& event, int delta) {
  if (broker_ == nullptr)
    return;
  if (event.runtime_id != 0) {
    size_t& count = runtime_counts_[event.runtime_id];
    count += delta;
    if (count == 0)
      runtime_counts_.erase(event.runtime_id);
  }
  queued_bytes_ += delta > 0 ? event.data.size() : 0;
  queued_bytes_ -= delta < 0 ? event.data.size() : 0;
}

template <typename Predicate>
void SessionDispatch::EraseLocked(Predicate&& matches) {
  for (auto it = queue_.begin(); it != queue_.end();) {
    if (matches(*it)) {
      Account(*it, -1);
      it = queue_.erase(it);
    } else {
      ++it;
    }
  }
}

void SessionDispatch::ClearLocked() {
  queue_.clear();
  runtime_counts_.clear();
  queued_bytes_ = 0;
}

v8host_status SessionDispatch::PostBroker(BrokerPost&& post, PostOverflow* overflow) {
  *overflow = PostOverflow::kNone;
  Event event;
  event.kind = post.kind;
  event.run = post.run;
  event.object = post.object;
  event.code = post.code;
  event.status = post.status;
  if (post.size > 0)
    event.data.assign(static_cast<const uint8_t*>(post.data),
                      static_cast<const uint8_t*>(post.data) + post.size);
  event.worker_id = post.worker_id;
  event.runtime_id = post.runtime_id;
  event.holder = post.holder;
  event.release = post.release;
  post.holder = nullptr;
  v8host_status result = v8host_status_ok;
  bool need_post = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_)
      return v8host_status_invalid_state;
    if (backpressure_failed_)
      return v8host_status_callback_backpressure;
    if (queue_.size() >= kBrokerQueueCapacity ||
        event.data.size() > kBrokerQueueBytes - queued_bytes_) {
      // The app thread isn't draining: fail the broker once and stickily. Its
      // disconnected terminal supersedes every pending child terminal.
      backpressure_failed_ = true;
      ClearLocked();
      Event terminal;
      terminal.kind = EventKind::kBrokerState;
      terminal.code = v8host_broker_state_disconnected;
      terminal.status = v8host_status_callback_backpressure;
      queue_.push_back(std::move(terminal));
      *overflow = PostOverflow::kBroker;
      result = v8host_status_callback_backpressure;
    } else if (event.runtime_id != 0 &&
               runtime_counts_[event.runtime_id] >= kRuntimeQueueCapacity) {
      // The runtime's closed terminal supersedes its pending events, and keeps
      // the overflowing event's reference to the runtime.
      const uint32_t runtime_id = event.runtime_id;
      EraseLocked([&](const Event& queued) { return queued.runtime_id == runtime_id; });
      Event terminal;
      terminal.kind = EventKind::kRuntimeState;
      terminal.object = event.object;
      terminal.code = v8host_runtime_state_closed;
      terminal.status = v8host_status_callback_backpressure;
      terminal.worker_id = event.worker_id;
      terminal.runtime_id = runtime_id;
      terminal.holder = event.holder;
      terminal.release = event.release;
      event.holder = nullptr;
      event.release = nullptr;
      Account(terminal, +1);
      queue_.push_back(std::move(terminal));
      *overflow = PostOverflow::kRuntime;
      result = v8host_status_callback_backpressure;
    } else {
      if (post.terminal && post.arbiter != nullptr && !post.arbiter->TryClaimTerminal())
        return v8host_status_run_terminal;
      Account(event, +1);
      queue_.push_back(std::move(event));
    }
    need_post = EnsureScheduledLocked();
  }
  if (need_post && !dispatcher_->PostDrain(this)) {
    OnPostFailed();
    return v8host_status_internal_error;
  }
  return result;
}

void SessionDispatch::Suppress(uint32_t worker_id, uint32_t runtime_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  EraseLocked([&](const Event& queued) {
    return runtime_id != 0 ? queued.runtime_id == runtime_id
                           : queued.worker_id == worker_id;
  });
}

SessionDispatch::SessionDispatch(Dispatcher* dispatcher,
                                 uint64_t id,
                                 v8host_session_s* handle,
                                 int32_t failed_state)
    : dispatcher_(dispatcher),
      id_(id),
      handle_(handle),
      failed_state_(failed_state) {
  LiveCounter().fetch_add(1, std::memory_order_relaxed);
}

SessionDispatch::~SessionDispatch() {
  LiveCounter().fetch_sub(1, std::memory_order_relaxed);
}

int SessionDispatch::live_count() {
  return LiveCounter().load(std::memory_order_relaxed);
}

void SessionDispatch::AddRef() {
  refcount_.fetch_add(1, std::memory_order_relaxed);
}

void SessionDispatch::Release() {
  if (refcount_.fetch_sub(1, std::memory_order_acq_rel) == 1)
    delete this;
}

v8host_status SessionDispatch::SetCallbacks(const v8host_callbacks* callbacks) {
  if (callbacks == nullptr)
    return v8host_status_invalid_arg;
  // For version 1 every member is required, so the required size equals the
  // struct size. A larger struct_size (a future version) is accepted and only
  // the known fields are read.
  if (callbacks->struct_size < sizeof(v8host_callbacks))
    return v8host_status_invalid_struct_size;

#ifdef V8HOST_DISPATCHER_TESTING
  if (FailNextCallbackCopy().exchange(false, std::memory_order_acq_rel))
    return v8host_status_no_memory;
#endif

  v8host_callbacks copy = {};
  copy.struct_size = sizeof(v8host_callbacks);
  copy.user_data = callbacks->user_data;
  copy.on_session_state = callbacks->on_session_state;
  copy.on_run_event = callbacks->on_run_event;
  copy.on_relay_message = callbacks->on_relay_message;
  copy.on_broker_disconnect = callbacks->on_broker_disconnect;

  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_)
    return v8host_status_invalid_state;
  current_table_ = copy;
  return v8host_status_ok;
}

v8host_status SessionDispatch::PostSessionState(int32_t state,
                                               v8host_status status) {
  Event event;
  event.kind = EventKind::kSessionState;
  event.code = state;
  event.status = status;
  return Enqueue(std::move(event), /*terminal_run=*/false, /*arbiter=*/nullptr);
}

v8host_status SessionDispatch::PostRunEvent(v8host_run_s* run,
                                           RunTerminalArbiter* arbiter,
                                           bool terminal,
                                           int32_t event,
                                           v8host_status status) {
  Event entry;
  entry.kind = EventKind::kRunEvent;
  entry.run = run;
  entry.code = event;
  entry.status = status;
  return Enqueue(std::move(entry), terminal, arbiter);
}

v8host_status SessionDispatch::PostRelay(v8host_run_s* run,
                                        int32_t kind,
                                        const void* data,
                                        size_t len) {
  if (len > 0 && data == nullptr)
    return v8host_status_invalid_arg;
  Event event;
  event.kind = EventKind::kRelay;
  event.run = run;
  event.code = kind;
  if (len > 0) {
    // Copy the borrowed payload so it outlives the producer's buffer; it is
    // owned until the callback delivers and the event is destroyed.
    event.data.resize(len);
    std::memcpy(event.data.data(), data, len);
  }
  return Enqueue(std::move(event), /*terminal_run=*/false, /*arbiter=*/nullptr);
}

v8host_status SessionDispatch::PostDisconnect() {
  Event event;
  event.kind = EventKind::kDisconnect;
  return Enqueue(std::move(event), /*terminal_run=*/false, /*arbiter=*/nullptr);
}

v8host_status SessionDispatch::Enqueue(Event&& event,
                                      bool terminal_run,
                                      RunTerminalArbiter* arbiter) {
  v8host_status result = v8host_status_ok;
  bool need_post = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_)
      return v8host_status_invalid_state;
    if (backpressure_failed_)
      return v8host_status_callback_backpressure;

    if (queue_.size() >= kSessionQueueCapacity) {
      // The app thread is not draining. Fail the session: drop everything
      // pending and deliver exactly one terminal backpressure notification.
      // Backpressure SUPERSEDES run-terminal arbitration: a terminal
      // PostRunEvent that overflows returns v8host_status_callback_backpressure and
      // does NOT claim the run arbiter (the arbiter check below is skipped on
      // this path) -- the session failure subsumes the run's terminal, and
      // backpressure_failed_ is sticky so no second terminal is ever delivered.
      backpressure_failed_ = true;
      ClearLocked();
      Event terminal;
      terminal.kind = EventKind::kSessionState;
      terminal.table = current_table_;
      terminal.code = failed_state_;
      terminal.status = v8host_status_callback_backpressure;
      queue_.push_back(std::move(terminal));
      result = v8host_status_callback_backpressure;
    } else {
      // Claim run-terminal arbitration only when the event is actually
      // enqueued, so a lost arbitration leaves the queue untouched.
      if (terminal_run && arbiter != nullptr && !arbiter->TryClaimTerminal())
        return v8host_status_run_terminal;
      event.table = current_table_;  // snapshot at post time
      queue_.push_back(std::move(event));
      result = v8host_status_ok;
    }
    need_post = EnsureScheduledLocked();
  }

  // PostMessage is outside the lock but after AddRef (in EnsureScheduledLocked),
  // which is safe: no drain for this session can start before its message
  // exists. On failure revert and fail closed.
  if (need_post && !dispatcher_->PostDrain(this)) {
    OnPostFailed();
    return v8host_status_internal_error;
  }
  return result;
}

bool SessionDispatch::EnsureScheduledLocked() {
  if (scheduled_)
    return false;
  scheduled_ = true;
  AddRef();  // balanced by DrainFromMessage's Release (or OnPostFailed)
  return true;
}

void SessionDispatch::OnPostFailed() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    scheduled_ = false;
    backpressure_failed_ = true;
    ClearLocked();
  }
  Release();  // balance the AddRef taken in EnsureScheduledLocked
}

void SessionDispatch::DrainFromMessage(SessionDispatch* session) {
  session->DrainLoop();
  // Consumes the reference held by the posted message; may delete `session`.
  // Touch nothing on `session` after this.
  session->Release();
}

void SessionDispatch::DrainLoop() {
  for (;;) {
    Event event;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // closed_ suppresses any remaining events (close cleared them already);
      // an empty queue ends the batch. Clearing scheduled_ under the lock lets
      // a future post schedule a fresh drain.
      if (closed_ || queue_.empty()) {
        scheduled_ = false;
        return;
      }
      event = std::move(queue_.front());
      queue_.pop_front();
      Account(event, -1);
    }
    // No lock held: the callback may reenter (post/set_callbacks/close).
    Invoke(event);
  }
}

void SessionDispatch::Invoke(const Event& event) {
  if (broker_ != nullptr) {
    const v8host_broker_callbacks& t = broker_table_;
    switch (event.kind) {
      case EventKind::kBrokerState:
        if (t.on_broker_state)
          t.on_broker_state(t.user_data, broker_,
                            static_cast<v8host_broker_state>(event.code), event.status);
        break;
      case EventKind::kWorkerState:
        if (t.on_worker_state)
          t.on_worker_state(t.user_data, static_cast<v8host_worker>(event.object),
                            static_cast<v8host_worker_state>(event.code), event.status);
        break;
      case EventKind::kRuntimeState:
        if (t.on_runtime_state)
          t.on_runtime_state(t.user_data, static_cast<v8host_runtime>(event.object),
                             static_cast<v8host_runtime_state>(event.code), event.status);
        break;
      case EventKind::kRunEvent:
        if (t.on_run_event)
          t.on_run_event(t.user_data, event.run,
                         static_cast<v8host_run_event>(event.code), event.status);
        break;
      case EventKind::kRelay:
        if (t.on_relay_message)
          t.on_relay_message(t.user_data, event.run,
                             static_cast<v8host_message_kind>(event.code),
                             event.data.empty() ? nullptr : event.data.data(),
                             event.data.size());
        break;
      default:
        break;
    }
    return;
  }
  switch (event.kind) {
    case EventKind::kSessionState:
      if (event.table.on_session_state) {
        event.table.on_session_state(
            event.table.user_data, handle_,
            static_cast<v8host_session_state>(event.code), event.status);
      }
      break;
    case EventKind::kRunEvent:
      if (event.table.on_run_event) {
        event.table.on_run_event(event.table.user_data, event.run,
                                 static_cast<v8host_run_event>(event.code),
                                 event.status);
      }
      break;
    case EventKind::kRelay:
      if (event.table.on_relay_message) {
        event.table.on_relay_message(
            event.table.user_data, event.run,
            static_cast<v8host_message_kind>(event.code),
            event.data.empty() ? nullptr : event.data.data(),
            event.data.size());
      }
      break;
    case EventKind::kDisconnect:
      if (event.table.on_broker_disconnect)
        event.table.on_broker_disconnect(event.table.user_data);
      break;
    default:
      break;
  }
}

void SessionDispatch::CloseAndRelease() {
  // Unregister FIRST, under sessions_mutex_, so no new producer can resolve this
  // id to us; a producer that already AddRef'd under that lock keeps us alive for
  // its in-flight post. Only then mark closed + suppress pending (session mutex_)
  // and drop the creator reference. sessions_mutex_ is not held across mutex_, so
  // there is no lock-order inversion with the id-keyed producer path.
  dispatcher_->UnregisterSession(id_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    ClearLocked();  // suppress pending, not-yet-delivered callbacks
  }
  // Drop the creator's reference. If a drain is in progress it holds its own
  // reference, so destruction is deferred until that callback unwinds.
  Release();
}

bool SessionDispatch::closed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return closed_;
}

}  // namespace v8host::client
