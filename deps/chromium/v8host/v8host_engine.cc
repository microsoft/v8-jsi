// v8host_engine.cc — the REAL V8/JSI engine persona of v8host.dll, driven by the
// product sbox.exe container over the sbox.h plugin ABI. It implements the
// configure/warmup/run/shutdown callbacks; v8host_plugin.cc composes them with
// the coordinator callback into the exported plugin vtable.
//
// Control inversion: the container (sbox.exe) owns main() and the sandbox
// lifecycle; this DLL links ONLY sbox.h (never the sandbox core) and reaches the
// message channel through the sbox_worker_api function pointers.
//
//   configure  BROKER        : decode the V8HostSpawnConfigV1 (session envelope +
//                              effective worker profile), drive every sbox config
//                              setter, and encode the worker profile (V1 for a
//                              session, V2 for an explicit worker) into opaque
//                              plugin_data.
//   warmup     PRE-lockdown  : decode plugin_data, load the engine DLL, set
//                              --jitless, read the startup snapshots and the
//                              guest JS, and set V8 up with a throwaway runtime
//                              on this thread. A V1 (session) worker also
//                              creates its one runtime here, on its own thread.
//                              All codegen/patching happens HERE — ACG forbids it
//                              afterward.
//   (sbox.exe lowers the token here: ACG armed)
//   run        POST-lockdown : prove ACG, then own the channel: this thread is the
//                              only one that drains or posts. It routes each
//                              envelope to its runtime slot, creates and closes
//                              runtimes on demand (each on its own thread), and
//                              posts their output in order until the host closes
//                              the channel. Then it joins the runtime threads
//                              (1 s in total) and disposes the V8 platform.
//   shutdown                 : tear down the runtimes + engine handle.
//
// IMPORTANT: this drives v8jsi through the **JSI C++ API** (facebook::jsi via
// JsiAbiRuntime), NOT the raw ABI-safe C interface. The C ABI is the binary-
// stable boundary INSIDE the DLL; C++ consumers use jsi.h + JsiAbiRuntime.h
// (both shipped in the NuGet package). Only the config seam (the configure
// callback that wires the task runner) stays on the C ABI, because that IS the
// ABI boundary.

#include "sbox.h"
#include "v8host_engine.h"
#include "v8host_run_envelope.h"  // neutral per-run START/RELAY/CANCEL/RESULT codec
#include "v8host_spawn_apply.h"  // ApplySpawnConfig + the spawn/worker-profile codec

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// Header-only loader hardening (ExeDir): resolves the engine DLL by full path
// from the app dir. Inline + kernel32-only here, so it pulls in NO sandbox core.
#include "sbox_harden.h"

// JSI C++ API (consumer-compiled, via jsi_cpp) + the v8jsi config header (its
// setters are resolved from the engine DLL by name and used inside the configure
// callback — the legitimate C ABI seam).
#include <jsi/jsi.h>

#include "jsi_abi/JsiAbiRuntime.h"
#include "jsi_abi/v8_jsi_abi.h"

namespace {

using namespace facebook::jsi;  // Runtime, Value, Object, Function, String, ...

//==========================================================================
// Persona state — created in warmup (pre-lockdown), used in run
// (post-lockdown), torn down in shutdown. Kept in DLL file-scope statics
// because the container, not this DLL, owns main() and the call sequence.
//==========================================================================
sbox_worker g_worker = nullptr;          // the worker run context (opaque)
const sbox_worker_api* g_api = nullptr;  // worker services (message channel)

HMODULE g_engine = nullptr;                // the loaded engine DLL
std::string g_guest_src;                   // guest source, read pre-lockdown
bool g_use_guest_file = false;             // custom guest vs. built-in demo

// Diagnostics sink. As a sandbox payload this process has no console of its own,
// so plain stdout is often invisible. Developer builds also mirror diagnostic
// lines to OutputDebugString. Release brokers never echo the worker's output.
// Used via the `printf` redefine below, so all existing call sites are covered.
inline void HostLog(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  const int n = ::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  ::fputs(buf, stdout);
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
  ::OutputDebugStringA(buf);
#endif
}
#define printf HostLog

// Resolve an engine-DLL export by name (decltype(&name) + GetProcAddress).
#define RESOLVE(mod, name)                                               \
  auto p_##name =                                                        \
      reinterpret_cast<decltype(&name)>(::GetProcAddress((mod), #name)); \
  if (!p_##name) {                                                       \
    printf("[v8host] missing v8jsi export: %s\n", #name);                \
    return sbox_status_failed;                                                   \
  }

void PrintAcgStatus() {
  PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dc = {};
  if (::GetProcessMitigationPolicy(::GetCurrentProcess(),
                                   ProcessDynamicCodePolicy, &dc, sizeof(dc))) {
    printf("[v8host] ACG (ProhibitDynamicCode) = %s\n",
           dc.ProhibitDynamicCode ? "ON" : "off");
  }
}

// Direct ACG probe: under MITIGATION_DYNAMIC_CODE_DISABLE a PAGE_EXECUTE_READWRITE
// allocation is refused with ERROR_DYNAMIC_CODE_BLOCKED (1655). Reads kernel
// state directly — the strongest post-lockdown proof that ACG is in force.
bool CanAllocExecutable(DWORD* last_error) {
  void* p = ::VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
  if (p) {
    ::VirtualFree(p, 0, MEM_RELEASE);
    if (last_error) *last_error = ERROR_SUCCESS;
    return true;
  }
  if (last_error) *last_error = ::GetLastError();
  return false;
}

// Read an entire file into a string of UTF-8 bytes. Returns false (leaving
// `out` unspecified) if the file can't be opened or fully read. Used to load a
// host-supplied guest script PRE-lockdown — after the token is lowered the
// payload can no longer open arbitrary files.
bool ReadFileBytes(const std::wstring& path, std::string& out) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  LARGE_INTEGER size = {};
  bool ok = ::GetFileSizeEx(h, &size) != 0;
  DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
  if (ok && (size.QuadPart < 0 ||
             static_cast<unsigned long long>(size.QuadPart) >
                 std::numeric_limits<size_t>::max())) {
    ok = false;
    error = ERROR_FILE_TOO_LARGE;
  }
  if (ok) {
    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    size_t total = 0;
    while (total < out.size()) {
      DWORD want =
          static_cast<DWORD>(std::min<size_t>(out.size() - total, 1u << 28));
      DWORD got = 0;
      const BOOL read_ok = ::ReadFile(h, &out[total], want, &got, nullptr);
      if (!read_ok || got == 0) {
        error = read_ok ? ERROR_HANDLE_EOF : ::GetLastError();
        ok = false;
        break;
      }
      total += got;
    }
  }
  ::CloseHandle(h);
  if (!ok)
    ::SetLastError(error);
  return ok;
}

#if defined(SBOX_DEV_ALLOW_UNSIGNED)
std::wstring EnvW(const wchar_t* name) {
  wchar_t buf[MAX_PATH] = {};
  DWORD n = ::GetEnvironmentVariableW(name, buf, MAX_PATH);
  return (n == 0 || n >= MAX_PATH) ? std::wstring() : std::wstring(buf, n);
}
#endif

// UTF-8 <-> UTF-16 bridges between the neutral spawn/worker-profile codec (UTF-8)
// and the Win32 wide APIs (engine DLL / snapshot paths). Lossless for valid
// Unicode (the codec guarantees strict UTF-8 / no NUL); empty maps to empty.
std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty())
    return std::wstring();
  const int needed =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0)
    return std::wstring();
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                        static_cast<int>(utf8.size()), wide.data(), needed);
  return wide;
}
std::string WideToUtf8(const std::wstring& wide) {
  if (wide.empty())
    return std::string();
  const int needed =
      ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                            nullptr, 0, nullptr, nullptr);
  if (needed <= 0)
    return std::string();
  std::string utf8(static_cast<size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), needed, nullptr, nullptr);
  return utf8;
}

bool ReadGuestPath(std::wstring& path) {
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
  constexpr wchar_t name[] = L"V8HOST_GUEST_JS";
  ::SetLastError(ERROR_SUCCESS);
  const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0);
  if (!required) {
    const DWORD error = ::GetLastError();
    return error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND;
  }
  path.resize(required);
  ::SetLastError(ERROR_SUCCESS);
  const DWORD length = ::GetEnvironmentVariableW(name, path.data(), required);
  if ((!length && ::GetLastError() != ERROR_SUCCESS) || length >= required) {
    if (length >= required)
      ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return false;
  }
  path.resize(length);
#else
  path.clear();
#endif
  return true;
}

//==========================================================================
// QPC timing split. Capture a timestamp at each startup boundary and print the
// delta as `[v8host][perf] <segment> = N.N ms`. These lines go through HostLog,
// so developer builds reach BOTH stdout and OutputDebugString. Developer-only,
// defaulting on; V8HOST_PERF=0 silences. (The `lockdown` segment is not emitted here — LowerToken now runs in
// the container between warmup and run, not inside this DLL.)
//==========================================================================
struct Perf {
  LARGE_INTEGER freq{};
  bool on = true;
  void init() {
    ::QueryPerformanceFrequency(&freq);
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
    on = EnvW(L"V8HOST_PERF") != L"0";
#else
    on = false;
#endif
  }
  LONGLONG now() const {
    LARGE_INTEGER t;
    ::QueryPerformanceCounter(&t);
    return t.QuadPart;
  }
  double ms(LONGLONG from, LONGLONG to) const {
    return freq.QuadPart ? static_cast<double>(to - from) * 1000.0 /
                               static_cast<double>(freq.QuadPart)
                         : 0.0;
  }
  void emit(const char* segment, LONGLONG from, LONGLONG to) const {
    if (on)
      printf("[v8host][perf] %-14s = %.1f ms\n", segment, ms(from, to));
  }
};

Perf g_perf;
std::atomic<LONGLONG> g_first_post_qpc{0};
inline void MarkFirstPost() {
  LONGLONG expected = 0;
  g_first_post_qpc.compare_exchange_strong(expected, g_perf.now());
}

//==========================================================================
// Foreground task runner (wired into the JSI config via the C ABI seam).
//
// V8 may post foreground tasks (from background workers, the inspector, etc.).
// Each runtime owns its thread, so its task runner just enqueues onto a
// thread-safe queue and wakes that thread; the thread runs them.
//==========================================================================

struct CTask {
  void* data;
  v8_jsi_abi_task_run_cb run;
  jsi_data_delete_cb del;
  void* delete_data;
};

struct TaskQueue {
  CRITICAL_SECTION cs;
  std::vector<CTask> tasks;
  HANDLE wake = nullptr;  // auto-reset, not owned; signaled when a task is posted
};

// Called by the DLL (possibly on a background thread) to post a foreground task.
void JSI_ABI_CDECL PostTaskCb(void* runner_data, void* task_data,
                          v8_jsi_abi_task_run_cb task_run_cb,
                          jsi_data_delete_cb task_data_delete_cb,
                          void* task_data_delete_data) {
  auto* q = static_cast<TaskQueue*>(runner_data);
  ::EnterCriticalSection(&q->cs);
  q->tasks.push_back({task_data, task_run_cb, task_data_delete_cb, task_data_delete_data});
  ::LeaveCriticalSection(&q->cs);
  ::SetEvent(q->wake);
}

// Called when the runtime is destroyed. runner_data points at a queue that
// outlives the runtime, so we don't free it — but fire the delete cb of any
// task still queued so nothing leaks.
void JSI_ABI_CDECL TaskRunnerDeleteCb(void* /*delete_data*/, void* runner_data) {
  auto* q = static_cast<TaskQueue*>(runner_data);
  ::EnterCriticalSection(&q->cs);
  for (auto& t : q->tasks)
    if (t.del)
      t.del(t.delete_data, t.data);
  q->tasks.clear();
  ::LeaveCriticalSection(&q->cs);
}

// Run (and free) all queued foreground tasks on the runtime's thread.
void RunQueuedTasks(TaskQueue* q) {
  std::vector<CTask> local;
  ::EnterCriticalSection(&q->cs);
  local.swap(q->tasks);
  ::LeaveCriticalSection(&q->cs);
  for (auto& t : local) {
    if (t.run)
      t.run(t.data);
    if (t.del)
      t.del(t.delete_data, t.data);
  }
}

// Resolved v8jsi entry points, set in warmup before the first runtime.
v8_jsi_abi_config_set_task_runner_fn g_set_task_runner = nullptr;
v8_jsi_abi_config_set_explicit_microtask_policy_fn g_set_microtask = nullptr;
v8_jsi_abi_config_set_startup_snapshot_fn g_set_snapshot = nullptr;
v8_jsi_abi_runtime_create_fn g_runtime_create = nullptr;
// Optional: stops V8's platform threads once every runtime is gone.
using DisposePlatformFn = int(__cdecl*)(void);
DisposePlatformFn g_dispose_platform = nullptr;

// The worker profile and its startup snapshots, read pre-lockdown. Each blob is
// kept for the process lifetime so it outlives every runtime created from it; a
// runtime borrows it through the config with a NULL deleter.
v8host::V8HostWorkerProfile g_profile;
std::vector<std::string> g_snapshots;

// Whether ACG must be in force post-lockdown. Stashed in warmup from the bound
// worker profile's jitless tier: untrusted (jitless) => ACG enforced; trusted
// (JIT) => ACG off. The post-lockdown probe is gated by this so a trusted worker
// is not failed for (correctly) being allowed to allocate executable memory.
bool g_expect_acg = true;

struct RuntimeConfig {
  TaskQueue* tasks = nullptr;
  const std::string* snapshot = nullptr;
};

jsi_status JSI_ABI_CDECL ConfigureRuntime(void* user_data, jsi_config cfg) {
  const auto* config = static_cast<const RuntimeConfig*>(user_data);
  // A failing setter poisons the config, so runtime creation reports it; the
  // individual statuses need no separate check here.
  if (g_set_task_runner)
    g_set_task_runner(cfg, &PostTaskCb, config->tasks, &TaskRunnerDeleteCb, nullptr);
  // Explicit microtask policy: promise jobs run only when the runtime's thread
  // drains them (after each turn), giving the host deterministic control.
  if (g_set_microtask)
    g_set_microtask(cfg, true);
  // A chosen snapshot creates the isolate from the worker's preloaded blob.
  if (config->snapshot && g_set_snapshot)
    g_set_snapshot(cfg, reinterpret_cast<const uint8_t*>(config->snapshot->data()),
                   config->snapshot->size(), nullptr, nullptr);
  return jsi_status_ok;
}

// Throws (the JSI C++ API does); callers catch.
std::unique_ptr<Runtime> CreateRuntime(RuntimeConfig* config) {
  return jsi::abi::makeJsiAbiRuntime(g_runtime_create, &ConfigureRuntime, config);
}

//==========================================================================
// Runtime slots. A worker hosts up to max_runtimes runtimes, each in a fixed
// slot (the envelope's slot field) on its own thread. The main thread owns the
// table and is the only thread that touches the channel: it routes each inbound
// frame into its slot's inbox and posts each slot's outbox in order. A runtime
// thread touches only its own slot, under that slot's lock.
//==========================================================================

// Runtime threads get the container main thread's stack reserve.
constexpr SIZE_T kRuntimeStackReserve = 1u << 20;
// Guest output frames a runtime may have waiting for the main thread before
// host.postMessage blocks.
constexpr uint32_t kGuestOutboxWindow = v8host::kRelayCreditWindow;
// One deadline for joining every runtime thread at shutdown.
constexpr DWORD kRuntimeJoinBudgetMs = 1000;
// The worker's exit code when a runtime thread outlives that deadline (it
// follows the container's worker exit codes, 10..17).
constexpr UINT kRunawayExitCode = 18;

struct OutFrame {
  sbox_msg_kind kind = sbox_msg_kind_binary;
  std::vector<uint8_t> bytes;
  bool guest = false;  // guest output counts toward kGuestOutboxWindow
};

struct InboxItem {
  enum class Kind { kFrame, kEvalGuest, kTestBatch };
  Kind kind = Kind::kFrame;
  sbox_msg_kind frame_kind = sbox_msg_kind_binary;
  std::vector<uint8_t> bytes;
  void* test = nullptr;
};

enum class SlotState { kEmpty, kRunning, kExited };

struct Slot {
  uint16_t index = 0;
  // Main-thread state.
  SlotState state = SlotState::kEmpty;
  HANDLE thread = nullptr;
  bool host_closed = false;  // CLOSE_RUNTIME routed: RUNTIME_CLOSED reports ok
  bool used = false;  // has held a runtime: late frames may cross RUNTIME_CLOSED
  // Set before the thread starts; read-only while it runs.
  int32_t snapshot_index = -1;
  bool precreated = false;
  // Shared with the runtime thread, under cs.
  CRITICAL_SECTION cs;
  HANDLE wake = nullptr;   // auto-reset: inbox item, task, or close
  HANDLE space = nullptr;  // auto-reset: outbox space, a cancel, or close
  HANDLE ready = nullptr;  // manual-reset: the runtime exists
  TaskQueue tasks;
  std::deque<InboxItem> inbox;
  std::deque<OutFrame> outbox;
  uint32_t outbox_guest = 0;
  uint32_t credit_owed = 0;
  uint32_t cancel_run_id = 0;  // the latest CANCEL routed to this slot
  bool closing = false;
  bool failed = false;       // creation failed, or the thread failed
  bool test_direct = false;  // testing: no main thread drains the outbox
  // Runtime-thread counters, read by the main thread after the join.
  int ambient_strings = 0, ambient_binaries = 0;
};

std::array<Slot*, v8host::kMaxRuntimeSlots> g_slots{};
size_t g_slot_count = 0;
HANDLE g_main_wake = nullptr;  // auto-reset: some slot has output, credit, or ended

void Wake(HANDLE event) {
  if (event)
    ::SetEvent(event);
}

void PushInbox(Slot& s, InboxItem&& item) {
  ::EnterCriticalSection(&s.cs);
  s.inbox.push_back(std::move(item));
  ::LeaveCriticalSection(&s.cs);
  Wake(s.wake);
}

void EnqueueOutput(Slot& s, OutFrame&& frame) {
  ::EnterCriticalSection(&s.cs);
  s.outbox.push_back(std::move(frame));
  ::LeaveCriticalSection(&s.cs);
  Wake(g_main_wake);
}

// Queue guest output. While the runtime already has a full window waiting for
// the main thread this blocks; a cancel of `run_id` or a close drops the frame.
void EnqueueGuestOutput(Slot& s, OutFrame&& frame, uint32_t run_id) {
  frame.guest = true;
  ::EnterCriticalSection(&s.cs);
  while (!s.test_direct && s.outbox_guest >= kGuestOutboxWindow) {
    if (s.closing || (run_id != 0 && s.cancel_run_id == run_id)) {
      ::LeaveCriticalSection(&s.cs);
      return;
    }
    ::LeaveCriticalSection(&s.cs);
    ::WaitForSingleObject(s.space, INFINITE);
    ::EnterCriticalSection(&s.cs);
  }
  s.outbox.push_back(std::move(frame));
  ++s.outbox_guest;
  ::LeaveCriticalSection(&s.cs);
  Wake(g_main_wake);
}

// One inbound RELAY was delivered or dropped: the main thread owes its credit.
void ReturnCredit(Slot& s) {
  ::EnterCriticalSection(&s.cs);
  ++s.credit_owed;
  ::LeaveCriticalSection(&s.cs);
  Wake(g_main_wake);
}

OutFrame EnvelopeFrame(const v8host::RunEnvelope& env) {
  OutFrame frame;
  frame.bytes = v8host::EncodeRunEnvelope(env);
  return frame;
}

bool IsRelayFrame(const std::vector<uint8_t>& frame) {
  return frame.size() > 5 &&
         frame[4] == static_cast<uint8_t>(v8host::RunEnvelopeType::kRelay) &&
         frame[5] == 0;
}

//==========================================================================
// Per-runtime run state, owned by the runtime's thread (design §10.3-§10.4).
// A coordinator START envelope makes a run active; the dev-ambient path (the
// `sbox.exe --broker` smoke) keeps active_run_id 0 so guest output posts bare
// and inbound bare frames reach host.onmessage exactly as before.
// complete_requested is set by the neutral host.complete() and consumed by the
// loop to emit a single RESULT(kCompleted). pending_starts is the admitted-but-
// not-yet-begun START FIFO (the activation bridge); cancel/error are set while
// servicing the active run and drive its terminal.
//==========================================================================
constexpr size_t kMaxActivationBridgeFrames = 256;
struct PendingStart {
  v8host::RunEnvelope start;
  std::deque<std::vector<uint8_t>> frames;
  size_t bytes = 0;
  bool overflow = false;
};
struct RunCtx {
  Slot* slot = nullptr;
  Runtime* rt = nullptr;
  uint32_t active_run_id = 0;
  bool complete_requested = false;
  bool failed = false;  // fatal: the runtime can't continue
  std::vector<PendingStart> pending_starts;
  size_t bridge_bytes = 0, bridge_frames = 0;
  bool replaying = false;
  bool cancel_active = false;  // CANCEL seen for the active run
  bool run_error = false;      // uncaught JS error in the active run
};

// Route guest output (host.postMessage / postMessageBinary / a host.complete
// value) into the slot's outbox. During an active run it is wrapped as a RELAY
// envelope scoped to that run and slot; dev-ambient it is posted bare,
// preserving the generic smoke. Returns false only for a frame the channel can
// never carry.
bool PostGuestOutput(RunCtx* c, sbox_msg_kind kind, const void* data, size_t len) {
  OutFrame frame;
  const auto* p = static_cast<const uint8_t*>(data);
  if (c->active_run_id == 0) {
    frame.kind = kind;
    frame.bytes.assign(p, p + len);
  } else {
    v8host::RunEnvelope env;
    env.type = v8host::RunEnvelopeType::kRelay;
    env.slot = c->slot->index;
    env.run_id = c->active_run_id;
    env.relay_kind = static_cast<int32_t>(kind);
    env.payload.assign(p, p + len);
    frame = EnvelopeFrame(env);
  }
  if (frame.bytes.size() > v8host::protocol::kMaxFramePayload)
    return false;
  EnqueueGuestOutput(*c->slot, std::move(frame), c->active_run_id);
  return true;
}

// Terminal emitters (worker -> broker). RESULT carries only the disposition (no
// guest bytes, design §4.2); RUN_ERROR carries a status plus a bounded, redacted
// diagnostic — the specific JS error text is logged host-side only, never on the
// wire.
void EmitRunResult(RunCtx* c, uint32_t run_id,
                   v8host::RunEnvelopeDisposition disposition) {
  v8host::RunEnvelope env;
  env.type = v8host::RunEnvelopeType::kResult;
  env.slot = c->slot->index;
  env.run_id = run_id;
  env.disposition = disposition;
  EnqueueOutput(*c->slot, EnvelopeFrame(env));
}

void EmitRunError(RunCtx* c, uint32_t run_id) {
  v8host::RunEnvelope env;
  env.type = v8host::RunEnvelopeType::kRunError;
  env.slot = c->slot->index;
  env.run_id = run_id;
  env.status_code =
      static_cast<uint32_t>(v8host::protocol::StatusCode::ERROR_INTERNAL);
  env.message = "guest run failed";  // redacted; detail stays in the host log
  EnqueueOutput(*c->slot, EnvelopeFrame(env));
}

//==========================================================================
// The `host` JS object and the inbound-message bridge (JSI C++ API).
//==========================================================================

// Install host.postMessage(string) / host.postMessageBinary(ArrayBuffer) /
// host.complete() and attach `host` to the global. Output goes to the runtime's
// slot outbox; only the main thread touches the channel. RAII manages every jsi
// pointer's lifetime — nothing leaks.
void InstallHostObject(Runtime& rt, RunCtx* c) {
  Object host(rt);

  host.setProperty(
      rt, "postMessage",
      Function::createFromHostFunction(
          rt, PropNameID::forAscii(rt, "postMessage"), 1,
          [c](Runtime& rt, const Value&, const Value* args,
              size_t count) -> Value {
            MarkFirstPost();
            if (count >= 1 && args[0].isString()) {
              std::string s = args[0].getString(rt).utf8(rt);
              if (!PostGuestOutput(c, sbox_msg_kind_string, s.data(), s.size())) {
                throw JSError(rt, "host.postMessage failed: result=" +
                                     std::to_string(static_cast<int>(sbox_status_failed)));
              }
            }
            return Value::undefined();
          }));

  host.setProperty(
      rt, "postMessageBinary",
      Function::createFromHostFunction(
          rt, PropNameID::forAscii(rt, "postMessageBinary"), 1,
          [c](Runtime& rt, const Value&, const Value* args,
              size_t count) -> Value {
            MarkFirstPost();
            if (count >= 1 && args[0].isObject()) {
              Object o = args[0].getObject(rt);
              if (o.isArrayBuffer(rt)) {
                ArrayBuffer ab = o.getArrayBuffer(rt);
                if (!PostGuestOutput(c, sbox_msg_kind_binary, ab.data(rt), ab.size(rt))) {
                  throw JSError(rt, "host.postMessageBinary failed: result=" +
                                       std::to_string(static_cast<int>(sbox_status_failed)));
                }
              }
            }
            return Value::undefined();
          }));

  // host.complete(optionalValue): the neutral run-completion signal (design
  // §10.3). Any provided string/ArrayBuffer rides the ordinary RELAY path first
  // (RESULT carries no guest bytes); then the run loop emits RESULT(kCompleted).
  // Dev-ambient (no active run) it just posts any value bare and is otherwise a
  // no-op, since only a coordinator run has a RESULT channel.
  host.setProperty(
      rt, "complete",
      Function::createFromHostFunction(
          rt, PropNameID::forAscii(rt, "complete"), 1,
          [c](Runtime& rt, const Value&, const Value* args,
              size_t count) -> Value {
            if (count >= 1) {
              if (args[0].isString()) {
                std::string s = args[0].getString(rt).utf8(rt);
                PostGuestOutput(c, sbox_msg_kind_string, s.data(), s.size());
              } else if (args[0].isObject()) {
                Object o = args[0].getObject(rt);
                if (o.isArrayBuffer(rt)) {
                  ArrayBuffer ab = o.getArrayBuffer(rt);
                  PostGuestOutput(c, sbox_msg_kind_binary, ab.data(rt), ab.size(rt));
                }
              }
            }
            c->complete_requested = true;
            return Value::undefined();
          }));

  rt.global().setProperty(rt, "host", host);
}

// Deliver one inbound frame to JS host.onmessage (string -> JS string,
// binary -> JS ArrayBuffer). Runs on the runtime's thread.
void DeliverToJs(Runtime& rt, sbox_msg_kind kind, const void* data,
                 size_t len) {
  Value host_v = rt.global().getProperty(rt, "host");
  if (!host_v.isObject())
    return;
  Object host = host_v.getObject(rt);
  Value cb_v = host.getProperty(rt, "onmessage");
  if (!cb_v.isObject())
    return;  // not set yet
  Object cb_obj = cb_v.getObject(rt);
  if (!cb_obj.isFunction(rt))
    return;
  Function cb = cb_obj.getFunction(rt);

  if (kind == sbox_msg_kind_string) {
    cb.call(rt, String::createFromUtf8(rt, static_cast<const uint8_t*>(data),
                                       len));
  } else {
    // Allocate the ArrayBuffer IN-CAGE via JS `new ArrayBuffer(n)` and copy the
    // frame in. Do NOT hand V8 an external backing store: when the V8 in-process
    // sandbox is enabled, external backing stores are a FATAL error. A
    // JS-allocated buffer uses V8's in-cage allocator and works in both sandbox
    // and non-sandbox builds.
    Function ab_ctor = rt.global().getPropertyAsFunction(rt, "ArrayBuffer");
    ArrayBuffer ab = ab_ctor.callAsConstructor(rt, Value(static_cast<double>(len)))
                         .getObject(rt)
                         .getArrayBuffer(rt);
    if (len)
      std::memcpy(ab.data(rt), data, len);
    cb.call(rt, std::move(ab));
  }
}

// True iff host.onmessage is currently a callable function. Used to detect a run
// that completed by top-level return without installing a handler.
bool HasOnMessageHandler(Runtime& rt) {
  Value host_v = rt.global().getProperty(rt, "host");
  if (!host_v.isObject())
    return false;
  Object host = host_v.getObject(rt);
  Value cb_v = host.getProperty(rt, "onmessage");
  return cb_v.isObject() && cb_v.getObject(rt).isFunction(rt);
}

// Clear host.onmessage so a prior run's handler cannot leak into the next run
// started on the same warmed runtime (design §10.3 "reset host handlers").
void ResetHostHandlers(Runtime& rt) {
  Value host_v = rt.global().getProperty(rt, "host");
  if (host_v.isObject())
    host_v.getObject(rt).setProperty(rt, "onmessage", Value::undefined());
}

// Deliver a bare dev-ambient frame to host.onmessage (string/binary), counting
// it for the scorecard. Fatal on an invalid kind or a thrown handler, exactly as
// before the run-dispatch rework.
void DeliverAmbient(RunCtx* c, sbox_msg_kind kind, const void* data,
                    size_t len) {
  if (kind == sbox_msg_kind_string)
    ++c->slot->ambient_strings;
  else if (kind == sbox_msg_kind_binary)
    ++c->slot->ambient_binaries;
  else {
    printf("[v8host] invalid inbound message kind=%d\n", kind);
    c->failed = true;
    return;
  }
  try {
    DeliverToJs(*c->rt, kind, data, len);
  } catch (const std::exception& e) {
    printf("[v8host] onmessage threw: %s\n", e.what());
    c->failed = true;
  } catch (...) {
    printf("[v8host] onmessage threw (unknown)\n");
    c->failed = true;
  }
}

// Handle one frame the main thread routed to this runtime: a bare frame (no
// envelope magic) is the dev-ambient path; an envelope is a coordinator-driven
// START/RELAY/CANCEL. Every RELAY envelope returns exactly one credit when it is
// delivered or dropped.
void HandleFrame(RunCtx* c, sbox_msg_kind kind, const void* data, size_t len) {
  if (c->failed)
    return;
  const auto* bytes = static_cast<const uint8_t*>(data);

  if (!v8host::IsRunEnvelope(bytes, len)) {
    DeliverAmbient(c, kind, data, len);
    return;
  }

  v8host::RunEnvelope env;
  if (!v8host::DecodeRunEnvelope(bytes, len, &env)) {
    printf("[v8host] dropping malformed run envelope (%zu bytes)\n", len);
    return;  // fail-closed: never act on an undecodable control frame
  }
  const bool relay = env.type == v8host::RunEnvelopeType::kRelay;
  if (!c->replaying && env.run_id != c->active_run_id &&
      (relay || env.type == v8host::RunEnvelopeType::kCancel)) {
    auto pending = std::find_if(c->pending_starts.begin(), c->pending_starts.end(),
        [&](const PendingStart &entry) { return entry.start.run_id == env.run_id; });
    if (pending != c->pending_starts.end()) {
      if (!pending->overflow) {
        if (len > v8host::protocol::kMaxQueuedRelayBytesPerRun - c->bridge_bytes ||
            c->bridge_frames == kMaxActivationBridgeFrames) {
          pending->overflow = true;
          c->bridge_bytes -= pending->bytes;
          c->bridge_frames -= pending->frames.size();
          for (const auto& held : pending->frames)
            if (IsRelayFrame(held))
              ReturnCredit(*c->slot);
          pending->bytes = 0;
          pending->frames.clear();
          if (relay)
            ReturnCredit(*c->slot);
        } else {
          pending->frames.emplace_back(bytes, bytes + len);
          pending->bytes += len;
          c->bridge_bytes += len;
          ++c->bridge_frames;
        }
      } else if (relay) {
        ReturnCredit(*c->slot);
      }
      return;
    }
  }
  switch (env.type) {
    case v8host::RunEnvelopeType::kStart:
      if (c->pending_starts.size() >= v8host::protocol::kMaxInFlightRunsPerSession) {
        c->failed = true;
      } else if (env.run_id != c->active_run_id && std::none_of(c->pending_starts.begin(), c->pending_starts.end(),
                 [&](const PendingStart &pending) { return pending.start.run_id == env.run_id; })) {
        c->pending_starts.push_back({std::move(env)});
      }
      break;
    case v8host::RunEnvelopeType::kRelay: {
      // Late and unknown relays have no dispatch authority.
      const bool servicing = c->active_run_id != 0 &&
                             env.run_id == c->active_run_id && !c->cancel_active &&
                             !c->run_error && !c->complete_requested;
      if (!servicing) {
        printf("[v8host] dropping relay for inactive run id=%u\n", env.run_id);
        ReturnCredit(*c->slot);
        break;
      }
      const sbox_msg_kind rk =
          env.relay_kind == static_cast<int32_t>(sbox_msg_kind_binary)
              ? sbox_msg_kind_binary
              : sbox_msg_kind_string;
      try {
        DeliverToJs(*c->rt, rk, env.payload.data(), env.payload.size());
      } catch (const std::exception& e) {
        printf("[v8host] run onmessage threw: %s\n", e.what());
        c->run_error = true;
      } catch (...) {
        printf("[v8host] run onmessage threw (unknown)\n");
        c->run_error = true;
      }
      ReturnCredit(*c->slot);
      break;
    }
    case v8host::RunEnvelopeType::kCancel:
      if (c->active_run_id != 0 && env.run_id == c->active_run_id)
        c->cancel_active = true;
      break;
    default:
      // The main thread routes only START, RELAY, and CANCEL here.
      printf("[v8host] dropping unexpected inbound envelope type=%u\n",
             static_cast<unsigned>(env.type));
      break;
  }
}

// Reset the per-run flags + active id once a run reaches a terminal, so the
// runtime can begin the next pending START.
void FinishRun(RunCtx* c) {
  c->active_run_id = 0;
  c->complete_requested = false;
  c->cancel_active = false;
  c->run_error = false;
}

// Emit the active run's terminal if one is due (priority: uncaught error >
// explicit complete > cancel). The active-run state is cleared first, so the
// next START never sees a stale run. No-op in the dev-ambient path.
void ServiceActiveRunTerminal(RunCtx* c) {
  const uint32_t id = c->active_run_id;
  if (id == 0)
    return;
  if (c->run_error) {
    FinishRun(c);
    EmitRunError(c, id);
  } else if (c->complete_requested) {
    FinishRun(c);
    EmitRunResult(c, id, v8host::RunEnvelopeDisposition::kCompleted);
  } else if (c->cancel_active) {
    FinishRun(c);
    EmitRunResult(c, id, v8host::RunEnvelopeDisposition::kCancelled);
  }
}

// Begin a coordinator-driven run: reset handlers, evaluate the START guest under
// the active run id (so its output wraps as RELAY), drain microtasks, then
// detect immediate completion — host.complete() during eval, a top-level return
// with no onmessage handler, or an uncaught error (RUN_ERROR). A run that
// installs a handler stays active for subsequent RELAY/CANCEL.
void BeginRun(const v8host::RunEnvelope& start, RunCtx* c) {
  Runtime& rt = *c->rt;
  c->complete_requested = false;
  c->cancel_active = false;
  c->run_error = false;
  c->active_run_id = start.run_id;

  bool completed = false;
  try {
    ResetHostHandlers(rt);
    std::string src(start.payload.begin(), start.payload.end());
    auto buf = std::make_shared<StringBuffer>(std::move(src));
    auto prepared = rt.prepareJavaScript(buf, "run.js");
    rt.evaluatePreparedJavaScript(prepared);
    rt.drainMicrotasks();
    completed = c->complete_requested || !HasOnMessageHandler(rt);
  } catch (const JSError& e) {
    printf("[v8host] run JS error: %s\n", e.getMessage().c_str());
    c->run_error = true;
  } catch (const std::exception& e) {
    printf("[v8host] run exception: %s\n", e.what());
    c->run_error = true;
  } catch (...) {
    printf("[v8host] run exception (unknown)\n");
    c->run_error = true;
  }

  if (completed)
    c->complete_requested = true;
}

void ServicePendingStarts(RunCtx *c, bool closing) {
  if (closing || c->failed) {
    c->pending_starts.clear();
    c->bridge_bytes = c->bridge_frames = 0;
    return;
  }
  while (c->active_run_id == 0 && !c->pending_starts.empty()) {
    PendingStart pending = std::move(c->pending_starts.front());
    c->pending_starts.erase(c->pending_starts.begin());
    c->bridge_bytes -= pending.bytes;
    c->bridge_frames -= pending.frames.size();
    BeginRun(pending.start, c);
    if (pending.overflow)
      c->run_error = true;
    c->replaying = true;
    for (auto &frame : pending.frames)
      HandleFrame(c, sbox_msg_kind_binary, frame.data(), frame.size());
    c->replaying = false;
    c->rt->drainMicrotasks();
    ServiceActiveRunTerminal(c);
  }
}

// Built-in demo guest (used when no V8HOST_GUEST_JS is supplied): wire onmessage
// to echo each inbound message. A string becomes `js echo: <m>`; a binary tags
// byte 0 to prove JS actually read/wrote the in-cage ArrayBuffer. It does NOT
// post unprompted, so every broker request maps to exactly one reply.
const char* kDemoJs =
    "host.onmessage = function(m){"
    "  if (typeof m === 'string') { host.postMessage('js echo: ' + m); }"
    "  else { var a = new Uint8Array(m); a[0] = 0x42; host.postMessageBinary(m); }"
    "};";

// Evaluate a V1 worker's guest (the built-in demo or V8HOST_GUEST_JS) on runtime
// 1, before any envelope, as the single JS thread did after lockdown.
void EvalGuest(RunCtx* c) {
  Runtime& rt = *c->rt;
  const LONGLONG t_start = g_perf.now();
  try {
    auto guest_buf = std::make_shared<StringBuffer>(g_guest_src);
    const char* guest_url = g_use_guest_file ? "guest.js" : "user.js";
    auto prepared = rt.prepareJavaScript(guest_buf, guest_url);
    const LONGLONG t_parsed = g_perf.now();
    g_perf.emit("parse", t_start, t_parsed);
    rt.evaluatePreparedJavaScript(prepared);
    const LONGLONG t_executed = g_perf.now();
    g_perf.emit("execute", t_parsed, t_executed);
    printf("[v8host] %s JS evaluated = OK\n", g_use_guest_file ? "guest" : "user");
    rt.drainMicrotasks();
    const LONGLONG first_post = g_first_post_qpc.load();
    if (g_perf.on) {
      if (first_post >= t_executed)
        g_perf.emit("kickoff", t_executed, first_post);
      else if (first_post != 0)
        printf("[v8host][perf] %-14s = (during execute, synchronous)\n",
               "kickoff");
    }
  } catch (const JSError& e) {
    printf("[v8host] JS error: %s\n", e.getMessage().c_str());
    c->failed = true;
  } catch (const std::exception& e) {
    printf("[v8host] exception: %s\n", e.what());
    c->failed = true;
  } catch (...) {
    printf("[v8host] exception (unknown)\n");
    c->failed = true;
  }
}

#if defined(V8HOST_ENGINE_TESTING)
// A batch the test thread hands to runtime 1, run as one fresh run context.
struct TestBatch {
  const std::vector<v8host::RunEnvelope>* batch = nullptr;
  bool closing = false;
  size_t peak_bytes = 0, peak_frames = 0, remaining = 0;
  bool ok = false;
  HANDLE done = nullptr;
};

void ResetRunState(RunCtx* c) {
  FinishRun(c);
  c->pending_starts.clear();
  c->bridge_bytes = c->bridge_frames = 0;
  c->replaying = false;
  c->failed = false;
}

void RunTestBatch(RunCtx* c, TestBatch* t) {
  ResetRunState(c);
  for (const auto& env : *t->batch) {
    auto bytes = v8host::EncodeRunEnvelope(env);
    HandleFrame(c, sbox_msg_kind_binary, bytes.data(), bytes.size());
    t->peak_bytes = (std::max)(t->peak_bytes, c->bridge_bytes);
    t->peak_frames = (std::max)(t->peak_frames, c->bridge_frames);
  }
  ServiceActiveRunTerminal(c);
  ServicePendingStarts(c, t->closing);
  t->remaining = c->bridge_bytes + c->bridge_frames;
  t->ok = !c->failed;
  ResetRunState(c);
}
#endif

// A runtime's thread: create the runtime (precreated runtime 1 in warmup, or a
// CREATE_RUNTIME after lockdown), then serve the slot's inbox — one START at a
// time, its relays and cancels, and V8's foreground tasks — until the slot
// closes or the runtime fails. The runtime never outlives this thread.
DWORD WINAPI RuntimeThread(void* param) {
  Slot& s = *static_cast<Slot*>(param);
  RunCtx ctx;
  ctx.slot = &s;
  RuntimeConfig config;
  config.tasks = &s.tasks;
  if (s.snapshot_index >= 0)
    config.snapshot = &g_snapshots[static_cast<size_t>(s.snapshot_index)];
  std::unique_ptr<Runtime> rt;
  const LONGLONG t_create = g_perf.now();
  try {
    rt = CreateRuntime(&config);
    if (rt)
      InstallHostObject(*rt, &ctx);
  } catch (const JSError& e) {
    printf("[v8host] slot %u: runtime setup JS error: %s\n", s.index,
           e.getMessage().c_str());
    rt.reset();
  } catch (const std::exception& e) {
    printf("[v8host] slot %u: runtime setup exception: %s\n", s.index, e.what());
    rt.reset();
  } catch (...) {
    rt.reset();
  }
  if (!rt) {
    printf("[v8host] slot %u: runtime creation failed\n", s.index);
    ::EnterCriticalSection(&s.cs);
    s.failed = true;
    ::LeaveCriticalSection(&s.cs);
    return 1;
  }
  ctx.rt = rt.get();
  if (s.precreated) {
    g_perf.emit("runtime-create", t_create, g_perf.now());
    printf("[v8host] v8jsi runtime created (warmup, pre-lockdown)\n");
  } else {
    printf("[v8host] slot %u: runtime created (snapshot %d)\n", s.index,
           s.snapshot_index);
    v8host::RunEnvelope ready;
    ready.type = v8host::RunEnvelopeType::kRuntimeReady;
    ready.slot = s.index;
    EnqueueOutput(s, EnvelopeFrame(ready));
  }
  ::SetEvent(s.ready);

  for (;;) {
    ::WaitForSingleObject(s.wake, INFINITE);
    RunQueuedTasks(&s.tasks);
    std::deque<InboxItem> items;
    ::EnterCriticalSection(&s.cs);
    items.swap(s.inbox);
    const bool closing = s.closing;
    ::LeaveCriticalSection(&s.cs);
    for (auto& item : items) {
      if (ctx.failed)
        break;
      switch (item.kind) {
        case InboxItem::Kind::kFrame:
          HandleFrame(&ctx, item.frame_kind, item.bytes.data(), item.bytes.size());
          break;
        case InboxItem::Kind::kEvalGuest:
          EvalGuest(&ctx);
          break;
        case InboxItem::Kind::kTestBatch:
#if defined(V8HOST_ENGINE_TESTING)
          RunTestBatch(&ctx, static_cast<TestBatch*>(item.test));
          ::SetEvent(static_cast<TestBatch*>(item.test)->done);
#endif
          break;
      }
    }
    if (!ctx.failed) {
      try {
        rt->drainMicrotasks();
      } catch (const std::exception& e) {
        printf("[v8host] slot %u: microtask failure: %s\n", s.index, e.what());
        ctx.failed = true;
      }
      ServiceActiveRunTerminal(&ctx);
      ServicePendingStarts(&ctx, closing);
    }
    if (closing || ctx.failed)
      break;
  }
  // A close ends the active run as cancelled; a failure reports through
  // RUNTIME_CLOSED instead.
  if (!ctx.failed && ctx.active_run_id != 0) {
    const uint32_t id = ctx.active_run_id;
    FinishRun(&ctx);
    EmitRunResult(&ctx, id, v8host::RunEnvelopeDisposition::kCancelled);
  }
  rt.reset();
  ::EnterCriticalSection(&s.cs);
  s.failed = s.failed || ctx.failed;
  ::LeaveCriticalSection(&s.cs);
  Wake(g_main_wake);
  return 0;
}

// Starts the runtime thread for an empty slot (main thread only).
bool StartSlot(Slot& s, int32_t snapshot_index, bool precreated) {
  s.snapshot_index = snapshot_index;
  s.precreated = precreated;
  s.host_closed = false;
  s.used = true;
  ::EnterCriticalSection(&s.cs);
  s.inbox.clear();
  s.outbox.clear();
  s.outbox_guest = 0;
  s.credit_owed = 0;
  s.cancel_run_id = 0;
  s.closing = false;
  s.failed = false;
  s.ambient_strings = s.ambient_binaries = 0;
  ::LeaveCriticalSection(&s.cs);
  ::ResetEvent(s.ready);
  s.thread = ::CreateThread(nullptr, kRuntimeStackReserve, &RuntimeThread, &s,
                            STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (!s.thread) {
    printf("[v8host] slot %u: runtime thread failed to start: %lu\n", s.index,
           ::GetLastError());
    s.failed = true;
    s.state = SlotState::kExited;
    return false;
  }
  s.state = SlotState::kRunning;
  return true;
}

// Closes every live runtime and joins its thread, with one deadline for all.
// Returns false if a thread is still running (JS that never yields).
bool QuiesceSlots(DWORD budget_ms) {
  for (size_t i = 0; i < g_slot_count; ++i) {
    Slot* s = g_slots[i];
    if (!s || !s->thread)
      continue;
    ::EnterCriticalSection(&s->cs);
    s->closing = true;
    ::LeaveCriticalSection(&s->cs);
    Wake(s->wake);
    Wake(s->space);
  }
  const ULONGLONG deadline = ::GetTickCount64() + budget_ms;
  bool joined = true;
  for (size_t i = 0; i < g_slot_count; ++i) {
    Slot* s = g_slots[i];
    if (!s || !s->thread)
      continue;
    const ULONGLONG now = ::GetTickCount64();
    const DWORD left = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
    if (::WaitForSingleObject(s->thread, left) != WAIT_OBJECT_0) {
      joined = false;
      continue;
    }
    ::CloseHandle(s->thread);
    s->thread = nullptr;
    s->state = SlotState::kExited;
  }
  return joined;
}

struct MainCtx {
  bool fatal = false;
};

void Violation(MainCtx* m, const char* why, unsigned slot) {
  printf("[v8host] protocol violation: %s (slot %u)\n", why, slot);
  m->fatal = true;
}

// The main thread's drain callback: route each inbound frame to its runtime
// slot. Called by the container's drain_messages (C code) — nothing here
// throws. A CREATE_RUNTIME for an occupied slot or any slot past max_runtimes
// is a protocol violation that ends the worker, as is a non-create frame for
// a slot that never held a runtime. Frames for a slot whose runtime has closed
// are dropped: the coordinator may send them before it sees RUNTIME_CLOSED.
void SBOX_CDECL RouteInbound(void* ctx, sbox_msg_kind kind, const void* data,
                             size_t len) {
  auto* m = static_cast<MainCtx*>(ctx);
  if (m->fatal)
    return;
  const auto* bytes = static_cast<const uint8_t*>(data);
  InboxItem item;
  item.frame_kind = kind;

  if (!v8host::IsRunEnvelope(bytes, len)) {
    // Dev-ambient bare frames go to runtime 1, which only V1 workers have.
    Slot* s = g_slot_count ? g_slots[0] : nullptr;
    if (!g_profile.precreate || !s || s->state != SlotState::kRunning) {
      printf("[v8host] dropping bare frame (%zu bytes): no runtime 1\n", len);
      return;
    }
    item.bytes.assign(bytes, bytes + len);
    PushInbox(*s, std::move(item));
    return;
  }

  v8host::RunEnvelope env;
  if (!v8host::DecodeRunEnvelope(bytes, len, &env)) {
    printf("[v8host] dropping malformed run envelope (%zu bytes)\n", len);
    return;  // fail-closed: never act on an undecodable control frame
  }
  if (env.slot >= g_slot_count)
    return Violation(m, "slot past max_runtimes", env.slot);
  Slot& s = *g_slots[env.slot];
  if (s.state == SlotState::kEmpty && env.type != v8host::RunEnvelopeType::kCreateRuntime) {
    if (!s.used)
      return Violation(m, "frame for an empty slot", env.slot);
    return;
  }
  switch (env.type) {
    case v8host::RunEnvelopeType::kCreateRuntime:
      if (s.state != SlotState::kEmpty)
        return Violation(m, "create on an occupied slot", env.slot);
      if (env.snapshot_index >= static_cast<int32_t>(g_snapshots.size()))
        return Violation(m, "create with an unknown snapshot", env.slot);
      StartSlot(s, env.snapshot_index, /*precreated=*/false);
      return;
    case v8host::RunEnvelopeType::kCloseRuntime:
      if (s.host_closed)
        return;
      s.host_closed = true;
      ::EnterCriticalSection(&s.cs);
      s.closing = true;
      ::LeaveCriticalSection(&s.cs);
      Wake(s.wake);
      Wake(s.space);
      return;
    case v8host::RunEnvelopeType::kStart:
    case v8host::RunEnvelopeType::kRelay:
    case v8host::RunEnvelopeType::kCancel:
      if (s.state != SlotState::kRunning || s.host_closed)
        return;
      if (env.type == v8host::RunEnvelopeType::kCancel) {
        ::EnterCriticalSection(&s.cs);
        s.cancel_run_id = env.run_id;
        ::LeaveCriticalSection(&s.cs);
        Wake(s.space);
      }
      item.bytes.assign(bytes, bytes + len);
      PushInbox(s, std::move(item));
      return;
    default:
      // Outbound-only (engine -> broker); a worker never receives these.
      printf("[v8host] dropping unexpected inbound envelope type=%u\n",
             static_cast<unsigned>(env.type));
      return;
  }
}

bool Post(sbox_msg_kind kind, const std::vector<uint8_t>& bytes) {
  return g_api->post_message(g_worker, kind, bytes.data(), bytes.size()) ==
         sbox_status_ok;
}

void ResetSlot(Slot& s) {
  ::EnterCriticalSection(&s.cs);
  s.inbox.clear();
  s.outbox.clear();
  s.outbox_guest = 0;
  s.credit_owed = 0;
  ::LeaveCriticalSection(&s.cs);
  s.state = SlotState::kEmpty;
  s.host_closed = false;
}

// Posts each slot's output in order, round-robin across slots, plus any owed
// CREDIT and, once an ended runtime's output has drained, its RUNTIME_CLOSED
// (always the slot's last frame). A full ring keeps the head for the next turn.
// Returns true if anything is still waiting.
bool PumpOutbound(size_t* cursor) {
  bool ring_full = false;
  for (bool progress = true; progress && !ring_full;) {
    progress = false;
    for (size_t n = 0; n < g_slot_count && !ring_full; ++n) {
      Slot& s = *g_slots[(*cursor + n) % g_slot_count];
      if (s.state == SlotState::kEmpty)
        continue;
      ::EnterCriticalSection(&s.cs);
      if (s.credit_owed) {
        v8host::RunEnvelope credit;
        credit.type = v8host::RunEnvelopeType::kCredit;
        credit.slot = s.index;
        credit.credit = (std::min)(s.credit_owed, v8host::kRelayCreditWindow);
        if (Post(sbox_msg_kind_binary, v8host::EncodeRunEnvelope(credit))) {
          s.credit_owed -= credit.credit;
          progress = true;
        } else {
          ring_full = true;
        }
      }
      if (!ring_full && !s.outbox.empty()) {
        const OutFrame& frame = s.outbox.front();
        if (Post(frame.kind, frame.bytes)) {
          if (frame.guest) {
            --s.outbox_guest;
            ::SetEvent(s.space);
          }
          s.outbox.pop_front();
          progress = true;
        } else {
          ring_full = true;
        }
      }
      const bool drained = s.outbox.empty() && s.credit_owed == 0;
      ::LeaveCriticalSection(&s.cs);
      if (!ring_full && drained && s.state == SlotState::kExited &&
          !g_profile.precreate) {
        v8host::RunEnvelope closed;
        closed.type = v8host::RunEnvelopeType::kRuntimeClosed;
        closed.slot = s.index;
        closed.status_code = s.host_closed ? 0 : v8host::kRuntimeClosedFailed;
        if (Post(sbox_msg_kind_binary, v8host::EncodeRunEnvelope(closed))) {
          printf("[v8host] slot %u: runtime closed (%s)\n", s.index,
                 closed.status_code ? "failed" : "ok");
          ResetSlot(s);
          progress = true;
        } else {
          ring_full = true;
        }
      }
    }
    if (g_slot_count)
      *cursor = (*cursor + 1) % g_slot_count;
  }
  for (size_t i = 0; i < g_slot_count; ++i) {
    Slot& s = *g_slots[i];
    if (s.state == SlotState::kEmpty)
      continue;
    ::EnterCriticalSection(&s.cs);
    const bool waiting = !s.outbox.empty() || s.credit_owed != 0;
    ::LeaveCriticalSection(&s.cs);
    if (waiting || (s.state == SlotState::kExited && !g_profile.precreate))
      return true;
  }
  return false;
}

void FreeSlots() {
  for (size_t i = 0; i < g_slots.size(); ++i) {
    Slot* s = g_slots[i];
    if (!s)
      continue;
    for (HANDLE h : {s->wake, s->space, s->ready})
      if (h)
        ::CloseHandle(h);
    ::DeleteCriticalSection(&s->tasks.cs);
    ::DeleteCriticalSection(&s->cs);
    delete s;
    g_slots[i] = nullptr;
  }
  g_slot_count = 0;
}

}  // namespace

#if defined(V8HOST_ENGINE_TESTING)
// Exercise the production main-thread codec/router without changing this
// process's mitigation policy or entering the channel wait loop.
bool V8HostEngineTestRoute(const std::vector<v8host::RunEnvelope>& batch) {
  MainCtx m;
  for (const auto& env : batch) {
    const auto bytes = v8host::EncodeRunEnvelope(env);
    RouteInbound(&m, sbox_msg_kind_binary, bytes.data(), bytes.size());
  }
  return !m.fatal;
}

// Wait for one runtime transition, then perform the main thread's outbound
// drain (including RUNTIME_CLOSED and ResetSlot after a thread has exited).
bool V8HostEngineTestWaitSlot(uint16_t index, bool closed) {
  if (index >= g_slot_count)
    return false;
  Slot& s = *g_slots[index];
  if (!s.thread || ::WaitForSingleObject(closed ? s.thread : s.ready, 5000) != WAIT_OBJECT_0)
    return false;
  if (closed) {
    ::CloseHandle(s.thread);
    s.thread = nullptr;
    s.state = SlotState::kExited;
  }
  size_t cursor = index;
  PumpOutbound(&cursor);
  return s.state == (closed ? SlotState::kEmpty : SlotState::kRunning);
}

// Runs one batch on runtime 1's thread as a fresh run context, then stands in
// for the main thread and posts the slot's output in order (no CREDIT).
bool V8HostEngineTestBatch(const std::vector<v8host::RunEnvelope> &batch, bool closing,
                           size_t *peak_bytes, size_t *peak_frames, size_t *remaining) {
  Slot* s = g_slot_count ? g_slots[0] : nullptr;
  if (!s || s->state != SlotState::kRunning)
    return false;
  TestBatch t;
  t.batch = &batch;
  t.closing = closing;
  t.done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!t.done)
    return false;
  ::EnterCriticalSection(&s->cs);
  s->test_direct = true;
  ::LeaveCriticalSection(&s->cs);
  InboxItem item;
  item.kind = InboxItem::Kind::kTestBatch;
  item.test = &t;
  PushInbox(*s, std::move(item));
  ::WaitForSingleObject(t.done, INFINITE);
  ::CloseHandle(t.done);
  std::deque<OutFrame> output;
  ::EnterCriticalSection(&s->cs);
  output.swap(s->outbox);
  s->outbox_guest = 0;
  s->credit_owed = 0;
  ::LeaveCriticalSection(&s->cs);
  for (const auto& frame : output)
    g_api->post_message(g_worker, frame.kind, frame.bytes.data(), frame.bytes.size());
  *peak_bytes = t.peak_bytes;
  *peak_frames = t.peak_frames;
  *remaining = t.remaining;
  return t.ok;
}
#endif

//==========================================================================
// PRE-lockdown. Load the engine, read the snapshots, and set V8 up with a
// throwaway runtime on this thread. A V1 worker also creates its one runtime,
// on its own thread. ACG forbids codegen/patching after this returns, so ALL
// engine setup (DLL load, --jitless, snapshot reads, V8 setup) happens here.
// Returns sbox_status_ok on success, non-zero (fail-closed) otherwise.
//==========================================================================
sbox_status SBOX_CDECL V8HostEngineWarmup(sbox_worker w,
                                         const sbox_worker_api* api) {
  if (!w || !api || api->struct_size < sizeof(sbox_worker_api))
    return sbox_status_invalid_arg;
  g_worker = w;
  g_api = api;
  g_perf.init();
  const LONGLONG t_entry = g_perf.now();
  g_main_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
  if (!g_main_wake)
    return sbox_status_failed;

  // The broker's configure() encoded the bound worker profile (engine DLL +
  // jitless + snapshots, and for an explicit worker its runtime cap) into the
  // opaque plugin_data; decode it here. The SAME configure() armed ACG to match
  // the jitless choice.
  {
    const void* pd = nullptr;
    size_t pd_len = 0;
    if (api->get_plugin_data(w, &pd, &pd_len) != sbox_status_ok || !pd || pd_len == 0 ||
        !v8host::DecodeWorkerProfile(static_cast<const uint8_t*>(pd), pd_len,
                                     &g_profile)) {
      printf("[v8host] warmup: missing/invalid worker profile plugin_data\n");
      return sbox_status_failed;
    }
  }
  const bool jitless = g_profile.jitless;
  g_expect_acg = jitless;  // untrusted (jitless) => ACG enforced post-lockdown
  std::wstring engine_dll = Utf8ToWide(g_profile.engine_dll);
  printf("[v8host] tier = %s\n",
         jitless ? "Untrusted (jitless + ACG)" : "Trusted (JIT, ACG off)");
  if (engine_dll.empty()) {
    printf("[v8host] warmup: empty engine DLL in run profile\n");
    return sbox_status_failed;
  }
  if (!g_profile.precreate)
    printf("[v8host] worker: up to %u runtimes, %zu snapshots\n",
           g_profile.max_runtimes, g_profile.snapshot_paths.size());

  // Startup snapshots from the profile (read NOW, pre-lockdown). Every bound
  // snapshot is required: an unreadable one is fatal (fail-closed, design
  // §8.6), never a silent normal-runtime fallback. An explicit worker's paths
  // are relative to the payload directory.
  for (const std::string& bound : g_profile.snapshot_paths) {
    std::wstring path = Utf8ToWide(bound);
    if (g_profile.snapshots_relative && !path.empty()) {
      std::replace(path.begin(), path.end(), L'/', L'\\');
      path = sbox_harden::ExeDir() + path;
    }
    std::string blob;
    if (path.empty() || !ReadFileBytes(path, blob)) {
      printf("[v8host] warmup: bound startup snapshot %ls could not be read; "
             "failing closed\n", path.c_str());
      return sbox_status_failed;
    }
    printf("[v8host] startup snapshot from %ls (%zu bytes)\n", path.c_str(),
           blob.size());
    g_snapshots.push_back(std::move(blob));
  }

  // A supplied guest selects custom-guest mode, not the built-in demo. Read it
  // NOW (pre-lockdown) — once the token is lowered the payload can no longer open
  // arbitrary files. Only runtime 1 of a V1 worker evaluates it. This stays a
  // neutral JS host: it installs the `host` object and evaluates whatever guest
  // it is handed. A requested but unreadable guest is an input error; never
  // replace it with the demo.
  if (g_profile.precreate) {
    std::wstring guest_js_path;
    if (!ReadGuestPath(guest_js_path)) {
      printf("[v8host] stage=guest-input environment query failed: error=%lu\n",
             ::GetLastError());
      return sbox_status_failed;
    }
    g_use_guest_file = !guest_js_path.empty();
    if (g_use_guest_file) {
      if (!ReadFileBytes(guest_js_path, g_guest_src)) {
        printf("[v8host] stage=guest-input cannot read %ls: error=%lu\n",
               guest_js_path.c_str(), ::GetLastError());
        return sbox_status_failed;
      }
      printf("[v8host] guest JS from %ls (%zu bytes)\n", guest_js_path.c_str(),
             g_guest_src.size());
    } else {
      g_guest_src = kDemoJs;
    }
    printf("[v8host] mode = %s\n",
           g_use_guest_file ? "custom guest" : "demo self-test");
  }

  // Load the engine by FULL PATH (our own application directory) — never a bare
  // name (which would honor the search path / allow planting). The container
  // (broker) hardened the loader search path and verified the payload chain
  // (v8host.dll + this engine) before spawning us — the token-restricted worker
  // cannot reliably reach the crypto/catalog services to verify itself.
  auto load_engine = [](const std::wstring& name) -> HMODULE {
    const std::wstring p = sbox_harden::ExeDir() + name;
    return ::LoadLibraryExW(p.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
  };
  g_engine = load_engine(engine_dll);
  if (!g_engine) {
    printf("[v8host] LoadLibrary(%ls) failed: %lu\n", engine_dll.c_str(),
           ::GetLastError());
    return sbox_status_failed;
  }
  const LONGLONG t_engine_loaded = g_perf.now();
  g_perf.emit("engine-load", t_entry, t_engine_loaded);
  printf("[v8host] engine = %ls\n", engine_dll.c_str());

  HMODULE guest = g_engine;  // RESOLVE() expects the module in `guest`
  if (jitless) {
    // Process-global flag — note the dummy argv[0] (V8 skips it).
    RESOLVE(guest, v8_jsi_abi_set_v8_flags);
    char arg0[] = "v8host";
    char flag[] = "--jitless";
    char* flag_argv[] = {arg0, flag};
    size_t flag_argc = 2;
    if (p_v8_jsi_abi_set_v8_flags(&flag_argc, flag_argv,
                                  /*remove_flags=*/true) != jsi_status_ok) {
      printf("[v8host] v8_jsi_abi_set_v8_flags failed\n");
      return sbox_status_failed;
    }
    printf("[v8host] --jitless %s\n",
           flag_argc == 1 ? "recognized" : "NOT recognized");
  }

  // Pre-check every snapshot against THIS engine now — the process-global
  // --jitless (if any) is set, so the version tag matches what the blob was
  // built with. A stale or cross-engine blob fails warmup rather than feeding V8
  // an incompatible blob, and so does an engine that cannot check at all.
  if (!g_snapshots.empty()) {
    auto p_compat = reinterpret_cast<v8_jsi_abi_snapshot_check_fn>(
        ::GetProcAddress(guest, "v8_jsi_abi_snapshot_check"));
    auto p_compat_str = reinterpret_cast<v8_jsi_abi_snapshot_compat_to_string_fn>(
        ::GetProcAddress(guest, "v8_jsi_abi_snapshot_compat_to_string"));
    if (!p_compat) {
      printf("[v8host] warmup: engine cannot check startup snapshots; failing "
             "closed\n");
      return sbox_status_failed;
    }
    for (const std::string& blob : g_snapshots) {
      const v8_jsi_abi_snapshot_compat code = p_compat(
          reinterpret_cast<const uint8_t*>(blob.data()), blob.size());
      if (code != v8_jsi_abi_snapshot_compat_ok) {
        const char* why = p_compat_str ? p_compat_str(code) : "incompatible";
        printf("[v8host] warmup: bound startup snapshot rejected (%s); failing "
               "closed\n", why);
        return sbox_status_failed;
      }
    }
    printf("[v8host] startup snapshot compatible with engine\n");
  }

  // Resolve the config setters the configure callback uses and the runtime
  // factory (JsiAbiRuntime injects our JSI_ABI_VERSION).
  {
    RESOLVE(guest, v8_jsi_abi_config_set_task_runner);
    RESOLVE(guest, v8_jsi_abi_config_set_explicit_microtask_policy);
    g_set_task_runner = p_v8_jsi_abi_config_set_task_runner;
    g_set_microtask = p_v8_jsi_abi_config_set_explicit_microtask_policy;
    if (!g_snapshots.empty()) {
      RESOLVE(guest, v8_jsi_abi_config_set_startup_snapshot);
      g_set_snapshot = p_v8_jsi_abi_config_set_startup_snapshot;
    }
    RESOLVE(guest, v8_jsi_abi_runtime_create);
    g_runtime_create = p_v8_jsi_abi_runtime_create;
    g_dispose_platform = reinterpret_cast<DisposePlatformFn>(
        ::GetProcAddress(guest, "v8_node_api_dispose_platform"));
  }

  // Set V8 up on THIS thread, pre-lockdown: a throwaway runtime, created with
  // the runtime config and destroyed, runs V8's process-wide setup (platform,
  // flags, sandbox reservation) once under this thread's initial token. Every
  // later runtime, on its own thread, finds it done.
  {
    const LONGLONG t_setup = g_perf.now();
    HANDLE scratch_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    TaskQueue scratch;
    ::InitializeCriticalSection(&scratch.cs);
    scratch.wake = scratch_wake;
    RuntimeConfig config;
    config.tasks = &scratch;
    bool created = false;
    try {
      created = scratch_wake && CreateRuntime(&config) != nullptr;
    } catch (const JSError& e) {
      printf("[v8host] warmup JS error: %s\n", e.getMessage().c_str());
    } catch (const std::exception& e) {
      printf("[v8host] warmup exception: %s\n", e.what());
    } catch (...) {
    }
    ::DeleteCriticalSection(&scratch.cs);
    if (scratch_wake)
      ::CloseHandle(scratch_wake);
    if (!created) {
      printf("[v8host] makeJsiAbiRuntime failed\n");
      return sbox_status_failed;
    }
    g_perf.emit("v8-setup", t_setup, g_perf.now());
  }

  // The runtime table: one slot per runtime the worker may host.
  g_slot_count = g_profile.max_runtimes;
  for (size_t i = 0; i < g_slot_count; ++i) {
    Slot* s = new (std::nothrow) Slot();
    if (!s)
      return sbox_status_failed;
    s->index = static_cast<uint16_t>(i);
    ::InitializeCriticalSection(&s->cs);
    ::InitializeCriticalSection(&s->tasks.cs);
    s->wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s->space = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s->ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s->tasks.wake = s->wake;
    g_slots[i] = s;
    if (!s->wake || !s->space || !s->ready)
      return sbox_status_failed;
  }

  // A V1 (session or dev) worker creates its one runtime now, before lockdown,
  // on its own thread, so a session keeps today's ordering. An explicit worker
  // has no runtime until CREATE_RUNTIME, after lockdown.
  if (g_profile.precreate) {
    Slot& s = *g_slots[0];
    if (!StartSlot(s, g_snapshots.empty() ? -1 : 0, /*precreated=*/true))
      return sbox_status_failed;
    HANDLE waits[2] = {s.ready, s.thread};
    if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
      printf("[v8host] makeJsiAbiRuntime failed\n");
      return sbox_status_failed;
    }
  }
  return sbox_status_ok;
}

//==========================================================================
// POST-lockdown. ACG MUST now be in force. Prove it with a FAILED executable
// allocation (observable kernel state) before servicing any envelope, then own
// the channel until the host closes it: route inbound frames to their runtime
// slots, create and close runtimes, and post each runtime's output. Returns
// sbox_status_ok on clean completion.
//==========================================================================
sbox_status SBOX_CDECL V8HostEngineRun(sbox_worker worker,
                                      const sbox_worker_api* api) {
  if (!worker || !api || api->struct_size < sizeof(sbox_worker_api))
    return sbox_status_invalid_arg;
  g_worker = worker;
  g_api = api;

  DWORD err = ERROR_SUCCESS;
  const bool exec_post = CanAllocExecutable(&err);
  printf("[v8host] exec-alloc(post)=%s (err=%lu)\n",
         exec_post ? "ALLOWED -> ACG NOT ENFORCED"
                   : "BLOCKED (ACG in force)",
         err);
  PrintAcgStatus();
  // Gate by the bound tier (stashed in warmup). Untrusted (jitless) MUST have ACG
  // in force post-lockdown (executable alloc blocked -> error 1655); trusted (JIT)
  // MUST be able to allocate executable memory. Either inconsistency fails closed.
  if (g_expect_acg && exec_post)
    return sbox_status_failed;  // untrusted but dynamic code allowed -> ACG failure
  if (!g_expect_acg && !exec_post)
    return sbox_status_failed;  // trusted but executable alloc blocked -> inconsistent

  if (!g_slot_count ||
      (g_profile.precreate && g_slots[0]->state != SlotState::kRunning)) {
    printf("[v8host] run: no runtime (warmup did not complete)\n");
    return sbox_status_failed;
  }

  // The message loop — this thread is the only one that drains or posts. It
  // waits on inbound messages, the host close signal, runtime output, and the
  // runtime threads. Messaging goes through the container's sbox_worker_api,
  // never the sandbox core.
  HANDLE inbound = static_cast<HANDLE>(g_api->inbound_event(g_worker));
  HANDLE close_evt = static_cast<HANDLE>(g_api->close_event(g_worker));
  if (!inbound || !close_evt) {
    printf("[v8host] run: channel handles missing\n");
    return sbox_status_failed;
  }
  // Runtime 1 of a V1 worker evaluates the guest before any envelope.
  if (g_profile.precreate) {
    InboxItem eval;
    eval.kind = InboxItem::Kind::kEvalGuest;
    PushInbox(*g_slots[0], std::move(eval));
  }

  MainCtx m;
  bool host_closed = false;
  bool pending = false;
  size_t cursor = 0;
  printf("[v8host] entering JS message loop (until host closes channel)\n");
  for (;;) {
    HANDLE waits[3 + v8host::kMaxRuntimeSlots] = {inbound, close_evt, g_main_wake};
    DWORD count = 3;
    for (size_t i = 0; i < g_slot_count; ++i)
      if (g_slots[i]->thread)
        waits[count++] = g_slots[i]->thread;
    const DWORD w = ::WaitForMultipleObjects(count, waits, FALSE, pending ? 1 : INFINITE);
    if (w == WAIT_FAILED) {
      printf("[v8host] message loop wait failed: wait=%lu error=%lu\n", w,
             ::GetLastError());
      break;
    }
    const bool closing = w == WAIT_OBJECT_0 + 1;
    if (closing || w == WAIT_OBJECT_0)
      g_api->drain_messages(g_worker, &RouteInbound, &m);
    // Join every runtime thread that ended. Runtime 1 of a V1 worker ends only
    // on failure, which ends the worker.
    for (size_t i = 0; i < g_slot_count; ++i) {
      Slot& s = *g_slots[i];
      if (!s.thread || ::WaitForSingleObject(s.thread, 0) != WAIT_OBJECT_0)
        continue;
      ::CloseHandle(s.thread);
      s.thread = nullptr;
      s.state = SlotState::kExited;
      if (g_profile.precreate)
        m.fatal = true;
    }
    if (m.fatal)
      break;
    pending = PumpOutbound(&cursor);
    if (closing) {
      host_closed = true;
      break;
    }
  }

  // Close every runtime and join the threads, with one deadline for all.
  const bool joined = QuiesceSlots(kRuntimeJoinBudgetMs);
  const Slot* first = g_slots[0];
  const bool runtime_failed = g_profile.precreate && first->failed;
  const bool js_ok = host_closed && !m.fatal && !runtime_failed;
  printf("[v8host] JS loop exited: onmessage delivered %d string + %d binary\n",
         first->ambient_strings, first->ambient_binaries);
  printf("[v8host] RESULT: %s\n",
         js_ok ? "PASS - untrusted JS exchanges WebView2-style messages "
                 "(postMessage/onmessage) under lockdown (jitless + ACG) via "
                 "the JSI C++ API over a generic, V8-agnostic container"
               : "FAIL");
  if (!joined) {
    // JS that never yields still runs inside the engine: unloading it or
    // running its process-exit teardown under that thread is unsafe.
    printf("[v8host] runtime threads did not stop within %lu ms; exiting\n",
           kRuntimeJoinBudgetMs);
    ::fflush(stdout);
    ::TerminateProcess(::GetCurrentProcess(), kRunawayExitCode);
  }
  // Every runtime is gone: stop V8's platform threads now, outside the loader
  // lock, so process exit never tears them down during DLL detach.
  if (g_dispose_platform)
    g_dispose_platform();
  return js_ok ? sbox_status_ok : sbox_status_failed;
}

//==========================================================================
// Tear down the runtimes (each runtime's destruction fires TaskRunnerDeleteCb on
// its slot's queue, so the slots outlive them), then free the engine handle.
//==========================================================================
void SBOX_CDECL V8HostEngineShutdown(sbox_worker /*w*/) {
  if (QuiesceSlots(kRuntimeJoinBudgetMs)) {
    FreeSlots();
    if (g_engine) {
      ::FreeLibrary(g_engine);
      g_engine = nullptr;
    }
  } else {
    // Never unload the engine under a thread that still runs JS in it.
    printf("[v8host] shutdown: runtime threads still running\n");
  }
  if (g_main_wake) {
    ::CloseHandle(g_main_wake);
    g_main_wake = nullptr;
  }
  g_snapshots.clear();
  g_profile = v8host::V8HostWorkerProfile();
  g_set_task_runner = nullptr;
  g_set_microtask = nullptr;
  g_set_snapshot = nullptr;
  g_runtime_create = nullptr;
  g_dispose_platform = nullptr;
  g_expect_acg = true;
  printf("[v8host] shutdown\n");
}

//==========================================================================
// BROKER role. Runs in the broker process (which loads this DLL only to call
// configure). On the coordinator path it decodes the V8HostSpawnConfigV1
// (session security envelope + bound effective worker profile) from
// configure_data; the standalone broker smoke passes none, so a dev fallback
// derives the tier/engine/snapshot from env + container defaults. Either way it
// drives every sbox config setter from the spawn config and encodes the
// V8HostWorkerProfileV1 into the opaque plugin_data the worker reads in warmup.
//==========================================================================

// Build the dev-fallback spawn config used when configure() is handed no
// configure_data (the standalone `sbox.exe --broker` smoke). ACG forces jitless
// for Untrusted; Trusted leaves ACG off so V8's JIT can run (ApplySpawnConfig
// enforces that jitless<->ACG match). The SBOX_TIER / V8HOST_ENGINE_DLL /
// V8HOST_SNAPSHOT overrides stay dev-test-only.
static void BuildDevSpawnConfig(v8host::V8HostSpawnConfigV1& spawn) {
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
  const bool trusted = EnvW(L"SBOX_TIER") == L"trusted";
  std::wstring engine = EnvW(L"V8HOST_ENGINE_DLL");
#else
  const bool trusted = false;
  std::wstring engine;
#endif
  if (engine.empty())
    engine = trusted ? L"v8jsi.dll" : L"v8jsisb.dll";
  spawn.tier = trusted ? v8host::kTierTrusted : v8host::kTierUntrusted;
  spawn.effective_tier = spawn.tier;
  spawn.prohibit_dynamic_code = !trusted;
  spawn.integrity = sbox_integrity_level_low;
  spawn.delayed_integrity = sbox_integrity_level_untrusted;
  spawn.initial_token = sbox_token_level_restricted_same_access;
  spawn.lockdown_token = sbox_token_level_lockdown;
  spawn.use_app_container = false;
  spawn.low_privilege_app_container = false;
  spawn.engine_dll = WideToUtf8(engine);
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
  spawn.snapshot_path = WideToUtf8(EnvW(L"V8HOST_SNAPSHOT"));
#endif
}

sbox_status SBOX_CDECL V8HostEngineConfigure(sbox_config cfg,
                                            const sbox_config_api* api,
                                            const void* configure_data,
                                            size_t configure_data_size) {
  if (!cfg || !api || api->struct_size < sizeof(sbox_config_api))
    return sbox_status_invalid_arg;
  if ((configure_data_size && !configure_data) ||
      configure_data_size > 64 * 1024)
    return sbox_status_invalid_arg;

  v8host::V8HostSpawnConfigV1 spawn;
  if (configure_data && configure_data_size) {
    // Coordinator path: a malformed spawn config fails closed (no half-described
    // sandbox is ever spawned).
    if (!v8host::V8HostSpawnConfigV1::Decode(
            static_cast<const uint8_t*>(configure_data), configure_data_size,
            &spawn)) {
      printf("[v8host] configure: bad V8HostSpawnConfigV1\n");
      return sbox_status_failed;
    }
  } else {
    BuildDevSpawnConfig(spawn);
  }

  // Drive every applicable config setter from the spawn config, enforce the
  // effective-tier/ACG consistency rule, and encode the worker profile into the
  // opaque plugin_data. Any rejected setter or inconsistency fails closed.
  if (!v8host::ApplySpawnConfig(spawn, api, cfg)) {
    printf("[v8host] configure: ApplySpawnConfig failed (fail closed)\n");
    return sbox_status_failed;
  }
  return sbox_status_ok;
}
