// The sbox core — the Chromium sandbox behind a generic, V8-agnostic C ABI.
// One signed DLL, loaded by two different binaries: a broker host
// (sbox_broker_*) and an app-defined target EXE (sbox_target_*). Contains NO
// knowledge of v8jsi, JavaScript, jitless, or any guest engine — the target EXE
// owns all of that.

#define SBOX_DLL_IMPL
#include "msg_channel.h"
#include "sbox_core_internal.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#if defined(SBOX_LIFECYCLE_TESTING)
#include "sbox_lifecycle_test.h"
#include <vector>
#endif

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/win/current_module.h"
#include "base/win/scoped_process_information.h"
#include "base/win/windows_version.h"
#include "sandbox/win/src/app_container.h"
#include "sandbox/win/src/handle_closer.h"  // HandleCloserConfig, g_handle_closer_info
#include "sandbox/win/src/interception.h"   // SelfInstallInterceptions
#include "sandbox/win/src/sandbox.h"
#include "sandbox/win/src/sandbox_factory.h"
#include "sandbox/win/src/sandbox_policy.h"
#include "sandbox/win/src/security_level.h"
#include "sandbox/win/src/target_services.h"  // TargetServicesBase::TestIPCPing
#if defined(SBOX_TRUST_TRANSITION_TESTING)
#include "sbox_trust_transition_test_private.h"
#endif

// Hosted (decoupled-DLL) mode hooks. We are inside the sbox core (linked with the
// sandbox engine), so we reference these vendored globals directly.
namespace sandbox {
extern bool g_sbox_hosted_mode;                  // sandbox_policy_base.cc
extern HANDLE g_sbox_hosted_section;             // target_process.cc (broker-provided)
extern uint32_t g_sbox_hosted_child_ipc_size;    // target_process.cc (captured)
extern uint32_t g_sbox_hosted_child_policy_size; // target_process.cc (captured)
extern HandleCloserConfig g_sbox_hosted_handle_closer;  // sandbox_policy_base.cc (captured)
extern "C" HANDLE g_shared_section;              // sandbox.cc
extern "C" size_t g_shared_IPC_size;             // target_process.cc
extern "C" size_t g_shared_policy_size;          // sandbox_nt_util.cc
extern "C" IntegrityLevel g_shared_delayed_integrity_level;  // target_services.cc
extern "C" MitigationFlags g_shared_delayed_mitigations;     // target_services.cc
}  // namespace sandbox

struct SboxTarget {
  sandbox::TargetServices* services = nullptr;
  // Message channel (target side).
  void* msg_map = nullptr;   // mapped message section
  HANDLE evt_t2b = nullptr;  // target signals after posting (target->broker)
  HANDLE evt_b2t = nullptr;  // target waits on for inbound (broker->target)
  HANDLE evt_close = nullptr;  // broker signals to ask the target to close (manual-reset)
};

// Broker-side session: a running target + its message channel. Returned by
// sbox_broker_spawn so the host can message the target while it runs.
struct SboxSession {
  HANDLE proc = nullptr;
  HANDLE thread = nullptr;  // target main thread (suspended until ResumeThread)
  HANDLE logh = INVALID_HANDLE_VALUE;
  std::wstring log_path;
  HANDLE ipc_section = nullptr;  // sandbox IPC section (g_sbox_hosted_section)
  // Message channel (broker side).
  HANDLE msg_section = nullptr;
  void* msg_map = nullptr;
  HANDLE evt_t2b = nullptr;  // broker waits on for inbound (target->broker)
  HANDLE evt_b2t = nullptr;  // broker signals after posting (broker->target)
  HANDLE evt_close = nullptr;  // broker signals to ask the target to close (manual-reset)
  HANDLE reader_thread = nullptr;
  HANDLE reader_stop = nullptr;  // manual-reset; set to stop the reader
  uint32_t t2b_cursor = 0;  // private consumer position: the shared read_idx is untrusted
  bool channel_corrupt = false;
  SboxMessageCb on_message = nullptr;
  void* on_message_ctx = nullptr;
};

#if defined(SBOX_DEV_ALLOW_UNSIGNED)
uint32_t g_sbox_lifecycle_test_failure = 0;
#endif

namespace {

constexpr int kAcgQueryFailure = -3;
constexpr int kAcgPostconditionFailure = -4;

#if defined(SBOX_TRUST_TRANSITION_TESTING)
std::mutex g_trust_transition_test_mutex;
SboxTrustTransitionTestControl g_trust_transition_next_control = {};
std::string g_trust_transition_last_output;
SboxTrustTransitionExitObservation g_trust_transition_exit_observation = {};

bool ValidPolicyExpectation(const SboxTrustTransitionPolicyExpectation& policy) {
  return (policy.initial_integrity == SBOX_INTEGRITY_LOW ||
          policy.initial_integrity == SBOX_INTEGRITY_UNTRUSTED) &&
         (policy.delayed_integrity == SBOX_INTEGRITY_LOW ||
          policy.delayed_integrity == SBOX_INTEGRITY_UNTRUSTED) &&
         (policy.app_container == 0 || policy.app_container == 1) &&
         (policy.lpac == 0 || policy.lpac == 1) &&
         (policy.force_relocation == 0 || policy.force_relocation == 1) &&
         (!policy.lpac || policy.app_container) &&
         (!policy.app_container || policy.package_sid[0]) &&
         policy.fixture_directory[0] &&
         policy.package_sid[std::size(policy.package_sid) - 1] == L'\0' &&
         policy.fixture_directory[std::size(policy.fixture_directory) - 1] == L'\0';
}
#endif

// Developer diagnostics only. Test-only capture remains independent of whether
// stdout / OutputDebugString echo is enabled.
void BrokerLog(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = ::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
#if defined(SBOX_TRUST_TRANSITION_TESTING)
  {
    std::lock_guard<std::mutex> lock(g_trust_transition_test_mutex);
    g_trust_transition_last_output.append(buf);
  }
#endif
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
  ::fputs(buf, stdout);
  ::OutputDebugStringA(buf);
#endif
}

// Length-delimited variant for the relayed target output (not NUL-terminated).
void BrokerLogRaw(const char* data, size_t len) {
#if defined(SBOX_TRUST_TRANSITION_TESTING)
  {
    std::lock_guard<std::mutex> lock(g_trust_transition_test_mutex);
    g_trust_transition_last_output.append(data, len);
  }
#endif
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
  ::fwrite(data, 1, len, stdout);
  char buf[4097];
  while (len) {
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    ::memcpy(buf, data, n);
    buf[n] = '\0';
    ::OutputDebugStringA(buf);
    data += n;
    len -= n;
  }
#endif
}

// Synchronous launch delegate (run task + reply inline; no thread pool).
class SyncBrokerDelegate : public sandbox::BrokerServicesDelegate {
 public:
  void ParallelLaunchPostTaskAndReplyWithResult(
      const base::Location&,
      base::OnceCallback<sandbox::CreateTargetResult()> task,
      base::OnceCallback<void(sandbox::CreateTargetResult)> reply) override {
    sandbox::CreateTargetResult result = std::move(task).Run();
    std::move(reply).Run(std::move(result));
  }
  void BeforeTargetProcessCreateOnCreationThread(const void*) override {}
  void AfterTargetProcessCreateOnCreationThread(const void*, DWORD) override {}
  void OnCreateThreadActionCreateFailure(DWORD) override {}
  void OnCreateThreadActionDuplicateFailure(DWORD) override {}
};

// base needs an AtExitManager + an initialized CommandLine. The DLL owns them
// (the host EXEs don't link base). One per process; leaked for process lifetime.
struct BaseBootstrap {
  base::AtExitManager at_exit;
  BaseBootstrap() { base::CommandLine::Init(0, nullptr); }
};
void EnsureBase() {
  static BaseBootstrap* base = new BaseBootstrap();  // leak intentionally
  (void)base;
}

// BrokerServices::Init() is a strictly once-per-process call (Chrome inits it in
// the browser process exactly once, then SpawnTarget()s many times). A second
// Init() returns non-OK, which is what broke one-broker -> N-targets. Guard it
// so the first sbox_broker_spawn initializes the broker and every later spawn
// reuses the same initialized instance. The std::once_flag makes init itself
// thread-safe. Returns the broker on success, nullptr if init failed (and stays
// failed for the process lifetime).
sandbox::BrokerServices* EnsureBrokerInitialized() {
  static sandbox::BrokerServices* g_broker = nullptr;
  static bool g_init_ok = false;
  static std::once_flag g_once;
  std::call_once(g_once, [] {
    sandbox::BrokerServices* b = sandbox::SandboxFactory::GetBrokerServices();
    if (!b) {
      BrokerLog("[broker] GetBrokerServices() returned null\n");
      return;
    }
    sandbox::ResultCode rc = b->Init(std::make_unique<SyncBrokerDelegate>());
    if (rc != sandbox::SBOX_ALL_OK) {
      BrokerLog("[broker] BrokerServices::Init() failed: rc=%d\n", rc);
      return;
    }
    g_broker = b;
    g_init_ok = true;
  });
  return g_init_ok ? g_broker : nullptr;
}

// Chromium's BrokerServices::SpawnTargetAsync has hard THREAD AFFINITY: it must
// always run on the one thread that first called it (the "launcher thread"). The
// vendored code enforces this with a DCHECK on the captured launcher thread id,
// and relies on it -- the launcher thread is opted out of ACG exactly once, on
// the first spawn, and the spawn-time globals assume a single launcher. A plain
// mutex is therefore NOT enough to let callers spawn from arbitrary threads:
// serializing the calls still leaves them on DIFFERENT threads, which trips the
// affinity guard (STATUS_BREAKPOINT) and mis-applies the launcher-thread
// mitigation.
//
// So the public sbox_broker_spawn (callable from ANY thread) marshals the real
// spawn onto a single dedicated launcher thread via this queue. The launcher
// runs spawns one at a time, which also serializes the process-global hosted-
// mode state for free; the spawned targets then run fully in parallel.
SboxSession* SpawnOnLauncherThread(const wchar_t* target_exe,
                                   const SboxPolicy* policy,
                                   SboxMessageCb on_message, void* ctx);

struct SpawnRequest {
  const wchar_t* target_exe = nullptr;
  const SboxPolicy* policy = nullptr;
  SboxMessageCb on_message = nullptr;
  void* ctx = nullptr;
  SboxSession* result = nullptr;
  HANDLE done = nullptr;  // manual-reset; set by the launcher when result ready
};

std::mutex g_launcher_mtx;             // guards g_launcher_queue
base::NoDestructor<std::deque<SpawnRequest*>> g_launcher_queue;
HANDLE g_launcher_wake = nullptr;      // auto-reset; signaled on enqueue

DWORD WINAPI LauncherThreadMain(void*) {
  for (;;) {
    ::WaitForSingleObject(g_launcher_wake, INFINITE);
    for (;;) {
      SpawnRequest* req = nullptr;
      {
        std::lock_guard<std::mutex> lk(g_launcher_mtx);
        if (g_launcher_queue->empty())
          break;
        req = g_launcher_queue->front();
        g_launcher_queue->pop_front();
      }
      req->result = SpawnOnLauncherThread(req->target_exe, req->policy,
                                          req->on_message, req->ctx);
      ::SetEvent(req->done);
    }
  }
}

// Lazily start the single launcher thread + its wake event. Thread-safe (magic
// static); returns false if the thread/event could not be created.
bool EnsureLauncherThread() {
  static bool ok = [] {
    g_launcher_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_launcher_wake)
      return false;
    HANDLE t =
        ::CreateThread(nullptr, 0, &LauncherThreadMain, nullptr, 0, nullptr);
    if (!t)
      return false;
    ::CloseHandle(t);  // detached; runs for the process lifetime
    return true;
  }();
  return ok;
}

sandbox::TokenLevel MapToken(int32_t level) {
  // SboxTokenLevel mirrors Chromium's TokenLevel 1:1; clamp unknown -> same-access.
  if (level < sandbox::USER_LOCKDOWN || level > sandbox::USER_RESTRICTED_SAME_ACCESS)
    return sandbox::USER_RESTRICTED_SAME_ACCESS;
  return static_cast<sandbox::TokenLevel>(level);
}
sandbox::IntegrityLevel MapIntegrity(int32_t level) {
  return level == SBOX_INTEGRITY_UNTRUSTED ? sandbox::INTEGRITY_LEVEL_UNTRUSTED
                                           : sandbox::INTEGRITY_LEVEL_LOW;
}

bool ValidatePolicy(const wchar_t* target_exe, const SboxPolicy* policy) {
  auto version = base::win::GetVersion();
#if defined(SBOX_TRUST_TRANSITION_TESTING)
  switch (sandbox::trust_transition_test::Platform()) {
    case SBOX_TRUST_TRANSITION_PLATFORM_PRE_RS5:
      version = base::win::Version::WIN10_RS4;
      break;
    case SBOX_TRUST_TRANSITION_PLATFORM_RS5:
      version = base::win::Version::WIN10_RS5;
      break;
    case SBOX_TRUST_TRANSITION_PLATFORM_UNKNOWN:
      version = base::win::Version::WIN_LAST;
      break;
  }
#endif
  if (version < base::win::Version::WIN10_RS5 ||
      version >= base::win::Version::WIN_LAST) {
    BrokerLog("[broker] stage=platform unsupported Windows version=%d; "
              "Windows 10 RS5/build 17763 or later is required\n",
              static_cast<int>(version));
    ::SetLastError(ERROR_OLD_WIN_VERSION);
    return false;
  }
  if (!target_exe || !target_exe[0] || !policy) {
    BrokerLog("[broker] stage=policy missing target path or policy\n");
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  if (policy->struct_size < sizeof(SboxPolicy)) {
    BrokerLog("[broker] stage=policy incompatible SboxPolicy size: "
              "got %u, need at least %zu\n",
              policy->struct_size, sizeof(SboxPolicy));
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  const bool app_container = policy->use_app_container != 0;
  if (policy->low_privilege_app_container && !app_container) {
    BrokerLog("[broker] stage=policy LPAC requires AppContainer\n");
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  const auto valid_integrity = [](int32_t value) {
    return value == SBOX_INTEGRITY_LOW || value == SBOX_INTEGRITY_UNTRUSTED;
  };
  if (!valid_integrity(policy->integrity) ||
      !valid_integrity(policy->delayed_integrity) ||
      (app_container && policy->integrity != SBOX_INTEGRITY_LOW)) {
    BrokerLog("[broker] stage=policy invalid integrity: initial=%d delayed=%d "
              "appcontainer=%d (profile initial integrity must be LOW)\n",
              policy->integrity, policy->delayed_integrity, app_container);
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  if (!app_container &&
      MapToken(policy->initial_token) < MapToken(policy->lockdown_token)) {
    BrokerLog("[broker] stage=policy initial token cannot be more restricted "
              "than lockdown token: initial=%d lockdown=%d\n",
              policy->initial_token, policy->lockdown_token);
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  if (app_container) {
    if (!policy->app_container_profile_name ||
        !policy->app_container_profile_name[0] ||
        (policy->capability_count && !policy->capabilities)) {
      BrokerLog("[broker] stage=policy missing AppContainer profile or "
                "capability array\n");
      ::SetLastError(ERROR_INVALID_PARAMETER);
      return false;
    }
    for (size_t i = 0; i < policy->capability_count; ++i) {
      if (!policy->capabilities[i] || !policy->capabilities[i][0]) {
        BrokerLog("[broker] stage=policy empty capability at index %zu\n", i);
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return false;
      }
    }
  }
  if (policy->file_rule_count && !policy->file_rules) {
    BrokerLog("[broker] stage=policy missing file-rule array\n");
    ::SetLastError(ERROR_INVALID_PARAMETER);
    return false;
  }
  for (size_t i = 0; i < policy->file_rule_count; ++i) {
    if (!policy->file_rules[i].pattern || !policy->file_rules[i].pattern[0]) {
      BrokerLog("[broker] stage=policy empty file rule at index %zu\n", i);
      ::SetLastError(ERROR_INVALID_PARAMETER);
      return false;
    }
  }
  return true;
}

// Base + SizeOfImage of our own sbox.exe in this process (diagnostics + the
// optional forced-relocation base-independence probe).
uintptr_t MyDllBase() {
  return reinterpret_cast<uintptr_t>(CURRENT_MODULE());
}
DWORD MyDllSizeOfImage() {
  HMODULE m = CURRENT_MODULE();
  if (!m)
    return 0;
  auto base = reinterpret_cast<const BYTE*>(m);
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return 0;
  // v8-jsi: native IMAGE_NT_HEADERS (PE32 on x86, PE32+ on x64) — this reads
  // our own sbox.exe image, always the build arch. (Was hardcoded ...64.)
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return 0;
  return nt->OptionalHeader.SizeOfImage;
}

// Same-image handoff — replaces the retired FindRemoteExport export-table walk.
//
// The old relay PE-parsed the suspended child's EXE export table to locate an
// exported g_sbox_bootstrap, then WriteProcessMemory'd a bootstrap struct the
// child copied into the sandbox globals. That existed ONLY because the broker
// and target were DIFFERENT images (the target's sbox core was not yet mapped),
// so the broker could not name the target's sandbox globals.
//
// In the single-image topology the broker and worker are the SAME image with
// the sandbox core statically linked, so every sandbox global sits at the SAME
// RVA in both. The broker writes the worker's globals directly — no export-table
// walk, no bootstrap struct. The ONLY per-process unknown is the worker's ASLR
// base, read from one documented PEB field (not the export table).
uint64_t WorkerImageBase(HANDLE proc) {
  struct PBI {
    PVOID r1;
    PVOID PebBaseAddress;
    PVOID r2[2];
    ULONG_PTR pid;
    PVOID r3;
  };
  using NtQIP = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  static auto nt_qip = reinterpret_cast<NtQIP>(::GetProcAddress(
      ::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
  if (!nt_qip)
    return 0;
  PBI pbi = {};
  if (nt_qip(proc, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), nullptr) < 0)
    return 0;
  // PEB->ImageBaseAddress is at 0x10 on 64-bit (x64/arm64) and 0x08 on x86. The
  // broker and target are the same arch by construction (the Chromium sandbox
  // requires a uniform-ABI broker/target pair), so the native pointer width
  // selects the correct offset.
  static_assert(sizeof(void*) == 8 || sizeof(void*) == 4,
                "unexpected pointer width for the PEB ImageBaseAddress offset");
  const uint64_t kImageBaseOff = sizeof(void*) == 8 ? 0x10 : 0x08;
  uintptr_t base = 0;  // pointer-width read: exactly ImageBaseAddress (x86-safe)
  SIZE_T got = 0;
  if (!::ReadProcessMemory(
          proc, reinterpret_cast<BYTE*>(pbi.PebBaseAddress) + kImageBaseOff,
          &base, sizeof(base), &got) ||
      got != sizeof(base))
    return 0;
  return base;
}

// The duplex message channel has no sandbox global, so the broker seeds this
// file-scope struct (by RVA) with the 4 child-side channel handles + a magic;
// the worker reads its own copy. Not exported — addressed by same-image RVA,
// never by an export lookup. plugin_data is an opaque app blob the broker copies
// in and the worker reads back (sbox_target_plugin_data); the core never looks
// inside it.
constexpr size_t kSboxPluginDataMax = 4096;  // fixed cap; a larger blob fails spawn
struct SboxWorkerSeed {
  uint64_t magic;
  uint64_t msg_section;
  uint64_t msg_evt_t2b;
  uint64_t msg_evt_b2t;
  uint64_t msg_evt_close;
  uint32_t plugin_data_len;                 // bytes actually set in plugin_data
  uint8_t plugin_data[kSboxPluginDataMax];  // opaque app knowledge; never read here
};
SboxWorkerSeed g_sbox_worker_seed = {};
constexpr uint64_t kSboxWorkerSeedMagic = 0x5342584F53454544ull;  // 'SBXOSEED'

// Write `n` bytes of `value` into the SAME variable in the same-image worker:
// worker_addr = &local - my_base + worker_base.
bool SeedWorkerVar(HANDLE proc, uint64_t worker_base, uintptr_t my_base,
                   const void* local, const void* value, size_t n) {
  const uint64_t remote =
      worker_base + (reinterpret_cast<uintptr_t>(local) - my_base);
  SIZE_T wrote = 0;
  return ::WriteProcessMemory(proc, reinterpret_cast<void*>(remote), value, n,
                              &wrote) &&
         wrote == n;
}

// --- message channel (broker side) ---

// Used only before the target is resumed and the broker reader is started.
void CleanupFailedSpawn(SboxSession* session) {
  if (session->proc) {
    if (::WaitForSingleObject(session->proc, 0) == WAIT_TIMEOUT &&
        !::TerminateProcess(session->proc, 1)) {
      BrokerLog("[broker] failed to terminate suspended target: error=%lu\n",
                ::GetLastError());
    }
    if (::WaitForSingleObject(session->proc, 5000) != WAIT_OBJECT_0) {
      BrokerLog("[broker] failed target did not exit during cleanup\n");
    }
  }
  if (session->msg_map)
    ::UnmapViewOfFile(session->msg_map);
  for (HANDLE handle : {session->msg_section, session->evt_t2b,
                        session->evt_b2t, session->evt_close,
                        session->reader_stop, session->ipc_section,
                        session->thread, session->proc}) {
    if (handle)
      ::CloseHandle(handle);
  }
  if (session->logh != INVALID_HANDLE_VALUE)
    ::CloseHandle(session->logh);
  if (!session->log_path.empty() && !::DeleteFileW(session->log_path.c_str())) {
    BrokerLog("[broker] failed log cleanup: error=%lu\n", ::GetLastError());
  }
  delete session;
}

// Create the duplex message section + the two auto-reset events + a manual-reset
// stop event for the reader thread.
bool CreateMsgChannel(SboxSession* s) {
  s->msg_section = ::CreateFileMappingW(
      INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0,
      static_cast<DWORD>(sbox_msg::kSectionSize), nullptr);
  if (!s->msg_section)
    return false;
  s->msg_map =
      ::MapViewOfFile(s->msg_section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
  if (!s->msg_map)
    return false;
  sbox_msg::InitHeader(s->msg_map);
  s->evt_t2b = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);    // auto-reset
  s->evt_b2t = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);    // auto-reset
  s->evt_close = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual-reset
  s->reader_stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual-reset
  return s->evt_t2b && s->evt_b2t && s->evt_close && s->reader_stop;
}

void ForwardWorkerFrame(void* context, int kind, const void* data, size_t size) {
  auto* session = static_cast<SboxSession*>(context);
  if (kind == SBOX_MSG_LIFECYCLE) {
    if (size != 4 || !data)
      return;
    const auto* bytes = static_cast<const uint8_t*>(data);
    const uint32_t phase = bytes[0] | (uint32_t(bytes[1]) << 8) |
        (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
    if (phase != sbox_lifecycle_phase_startup && phase != sbox_lifecycle_phase_security)
      return;
  }
  if (session->on_message)
    session->on_message(session->on_message_ctx, kind, data, size);
}
void DrainWorkerFrames(SboxSession* session) {
  auto* header = sbox_msg::Header(session->msg_map);
  if (session->channel_corrupt)
    return;
  if (sbox_msg::Drain(&header->t2b, sbox_msg::T2BRing(session->msg_map),
                      &ForwardWorkerFrame, session, &session->t2b_cursor) < 0) {
    // The worker wrote an impossible producer index. Stop reading its frames
    // and end it; the exit is reported as for any worker exit.
    session->channel_corrupt = true;
    BrokerLog("[broker] target message channel corrupt; terminating\n");
    ::TerminateProcess(session->proc, 0xBAD1);
  }
}

// Deliver incoming frames and report process exit. The host still owns wait
// and session cleanup.
DWORD WINAPI BrokerReaderThread(void* param) {
  SboxSession* s = static_cast<SboxSession*>(param);
  HANDLE waits[3] = {s->evt_t2b, s->reader_stop, s->proc};
  DWORD wait_count = 3;
  for (;;) {
    DWORD w = ::WaitForMultipleObjects(wait_count, waits, FALSE, INFINITE);
    if (w == WAIT_OBJECT_0 + 2) {
      DWORD exit_code = 0;
      const DWORD pid = ::GetProcessId(s->proc);
      if (::GetExitCodeProcess(s->proc, &exit_code)) {
        BrokerLog("[broker] target exit observed: pid=%lu exit=%lu (0x%08lx) "
                  "output=%ls\n",
                  pid, exit_code, exit_code,
                  s->log_path.empty() ? L"(unavailable)" : s->log_path.c_str());
#if defined(SBOX_TRUST_TRANSITION_TESTING)
        {
          std::lock_guard<std::mutex> lock(g_trust_transition_test_mutex);
          ++g_trust_transition_exit_observation.count;
          g_trust_transition_exit_observation.exit_code = exit_code;
        }
#endif
      } else {
        BrokerLog("[broker] target exit query failed: pid=%lu error=%lu\n",
                  pid, ::GetLastError());
      }
      // Preserve queued output before the broker-origin exit fact.
      DrainWorkerFrames(s);
      const uint8_t phase[] = {sbox_lifecycle_phase_exit, 0, 0, 0};
      if (s->on_message)
        s->on_message(s->on_message_ctx, SBOX_MSG_LIFECYCLE, phase, sizeof(phase));
      wait_count = 2;
      continue;
    }
    if (w == WAIT_FAILED) {
      const DWORD error = ::GetLastError();
      BrokerLog("[broker] target reader wait failed: pid=%lu error=%lu\n",
                ::GetProcessId(s->proc), error);
    }
    if (w != WAIT_OBJECT_0)
      break;  // stop event or error
    DrainWorkerFrames(s);
  }
  return 0;
}

}  // namespace

namespace {

// The real spawn. Runs ONLY on the launcher thread (Chromium's SpawnTargetAsync
// has hard thread affinity -- see EnsureLauncherThread / the note above). Never
// call this directly from a caller thread; go through sbox_broker_spawn.
SboxSession* SpawnOnLauncherThread(const wchar_t* target_exe,
                                   const SboxPolicy* policy,
                                   SboxMessageCb on_message, void* ctx) {
  // base + the broker were already initialized by sbox_broker_spawn on the
  // CALLER thread (deliberately -- see the note there: doing that possibly
  // DLL-loading init on this freshly-created launcher thread can stall if the
  // host hardened its DLL search path). EnsureBrokerInitialized() here just
  // returns the already-created broker (its std::call_once is already satisfied).
  BrokerLog("[broker] sbox.dll base = 0x%llx (size 0x%lx)\n",
         (unsigned long long)MyDllBase(), MyDllSizeOfImage());

  sandbox::BrokerServices* broker = EnsureBrokerInitialized();
  if (!broker) {
    BrokerLog("[broker] broker not initialized\n");
    return nullptr;
  }

  std::unique_ptr<sandbox::TargetPolicy> sb_policy = broker->CreatePolicy();
  sandbox::TargetConfig* config = sb_policy->GetConfig();
  if (config->SetJobLevel(sandbox::JobLevel::kLockdown, 0) !=
      sandbox::SBOX_ALL_OK) {
    BrokerLog("[broker] job policy configuration failed\n");
    return nullptr;
  }

  if (!policy->use_app_container) {
    if (config->SetTokenLevel(MapToken(policy->initial_token),
                              MapToken(policy->lockdown_token)) !=
            sandbox::SBOX_ALL_OK ||
        config->SetIntegrityLevel(MapIntegrity(policy->integrity)) !=
            sandbox::SBOX_ALL_OK) {
      BrokerLog("[broker] restricted-token policy configuration failed\n");
      return nullptr;
    }
    config->SetLockdownDefaultDacl();
  } else {
    BrokerLog("[broker] profile token contract: initial_token=%d and "
              "lockdown_token=%d do not apply; no restricted-default-DACL "
              "guarantee\n",
              policy->initial_token, policy->lockdown_token);
  }
  config->SetDelayedIntegrityLevel(MapIntegrity(policy->delayed_integrity));

  if (policy->use_app_container) {
    const sandbox::ResultCode app_container_rc =
        config->AddAppContainerProfile(
            base::wcstring_view(policy->app_container_profile_name));
    if (app_container_rc != sandbox::SBOX_ALL_OK) {
      BrokerLog("[broker] AddAppContainerProfile failed: rc=%d\n",
             app_container_rc);
      return nullptr;
    }
    sandbox::AppContainer* app_container = config->GetAppContainer();
    if (!app_container) {
      BrokerLog("[broker] AppContainer profile unavailable\n");
      return nullptr;
    }
    app_container->SetEnableLowPrivilegeAppContainer(
        policy->low_privilege_app_container != 0);
    for (size_t i = 0; i < policy->capability_count; ++i) {
      const wchar_t* capability = policy->capabilities[i];
      if (!capability || !capability[0] ||
          !app_container->AddCapabilitySddl(base::wcstring_view(capability))) {
        BrokerLog("[broker] invalid AppContainer capability at index %zu\n", i);
        return nullptr;
      }
    }
  }

  for (size_t i = 0; i < policy->file_rule_count; ++i) {
    const SboxFileRule& rule = policy->file_rules[i];
    sandbox::FileSemantics sem = rule.readonly
                                     ? sandbox::FileSemantics::kAllowReadonly
                                     : sandbox::FileSemantics::kAllowAny;
    if (config->AllowFileAccess(sem, rule.pattern) != sandbox::SBOX_ALL_OK) {
      BrokerLog("[broker] AllowFileAccess(%ls) failed\n", rule.pattern);
      return nullptr;
    }
    BrokerLog("[broker] allow file (%s): %ls\n",
           rule.readonly ? "readonly" : "any", rule.pattern);
  }

  std::unique_ptr<SboxSession, decltype(&CleanupFailedSpawn)> pending(
      new SboxSession(), &CleanupFailedSpawn);
  SboxSession* s = pending.get();
  s->on_message = on_message;
  s->on_message_ctx = ctx;

  // The duplex message channel (independent of the sandbox IPC).
  if (!CreateMsgChannel(s)) {
    BrokerLog("[broker] CreateMsgChannel failed: %lu\n", ::GetLastError());
    return nullptr;
  }

  // Hosted mode: the broker can't push into the suspended child's unmapped
  // sbox.exe. Create the shared IPC section here (target->Init adopts it); after
  // the spawn we duplicate it + the message-channel handles into the child and
  // seed the values directly at their shared RVAs (same-image handoff).
  sandbox::g_sbox_hosted_mode = true;
#if defined(SBOX_TRUST_TRANSITION_TESTING)
  if (sandbox::trust_transition_test::ConsumeBrokerFault(
          SBOX_TRUST_TRANSITION_BROKER_NON_HOSTED)) {
    sandbox::g_sbox_hosted_mode = false;
  }
#endif
  s->ipc_section =
      ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                           PAGE_READWRITE | SEC_COMMIT, 0, 256 * 1024, nullptr);
  if (!s->ipc_section) {
    BrokerLog("[broker] CreateFileMapping(ipc section) failed: %lu\n",
           ::GetLastError());
    return nullptr;
  }
  sandbox::g_sbox_hosted_section = s->ipc_section;

  base::CommandLine target_cmd((base::FilePath(target_exe)));
  // Single-image topology — the broker spawns THIS SAME image in the worker
  // role. The role is carried by argv so main() can branch before any role-only
  // init; the sandbox state is delivered by the same-image seed below. This
  // switch only selects the persona.
  target_cmd.AppendSwitch("worker");
  BrokerLog("[broker] spawning target binary: %ls --worker\n", target_exe);

  // Propagate the plugin identity to the worker persona (opaque to the core — a
  // renameable host stays in sync with its plugin). Rendered as --plugin=<name>.
  if (policy->worker_plugin_name && policy->worker_plugin_name[0]) {
    target_cmd.AppendSwitchNative("plugin", policy->worker_plugin_name);
    BrokerLog("[broker] worker plugin = %ls\n", policy->worker_plugin_name);
  }

  // Capture the sandboxed target's stdout (it can't reach the console).
  wchar_t dir[MAX_PATH] = {};
  DWORD tn = ::GetTempPathW(MAX_PATH, dir);
  wchar_t log_path[MAX_PATH] = {};
  if (tn && tn < MAX_PATH &&
      ::GetTempFileNameW(dir, L"sbx", 0, log_path)) {
    s->log_path = log_path;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    s->logh = ::CreateFileW(s->log_path.c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  if (s->logh != INVALID_HANDLE_VALUE) {
    sb_policy->SetStdoutHandle(s->logh);
    sb_policy->SetStderrHandle(s->logh);
  } else {
    BrokerLog("[broker] target output capture unavailable: error=%lu\n",
              ::GetLastError());
    if (!s->log_path.empty()) {
      if (!::DeleteFileW(s->log_path.c_str())) {
        BrokerLog("[broker] unused log cleanup failed: error=%lu\n",
                  ::GetLastError());
      }
      s->log_path.clear();
    }
  }

  DWORD last_error = ERROR_SUCCESS;
  sandbox::ResultCode rc = sandbox::SBOX_ALL_OK;
  broker->SpawnTargetAsync(
      target_cmd, std::move(sb_policy),
      base::BindOnce(
          [](HANDLE* op, HANDLE* ot, DWORD* oe, sandbox::ResultCode* orc,
             base::win::ScopedProcessInformation pi, DWORD le,
             sandbox::ResultCode r) {
            *oe = le;
            *orc = r;
            if (r == sandbox::SBOX_ALL_OK) {
              *op = pi.TakeProcessHandle();
              *ot = pi.TakeThreadHandle();
            }
          },
          &s->proc, &s->thread, &last_error, &rc));

  if (rc != sandbox::SBOX_ALL_OK || !s->proc) {
    BrokerLog("[broker] SpawnTargetAsync failed: rc=%d last_error=%lu\n", rc,
           last_error);
    return nullptr;
  }
  const DWORD target_pid = ::GetProcessId(s->proc);
  BrokerLog("[broker] target created: pid=%lu output=%ls\n", target_pid,
            s->log_path.empty() ? L"(unavailable)" : s->log_path.c_str());

  // Same-image seed. Duplicate the IPC + message-channel handles into the child
  // (unchanged), then write the worker's sandbox globals + the channel seed
  // DIRECTLY at their shared RVAs while it is still suspended — no export-table
  // walk, no bootstrap struct.
  HANDLE proc = s->proc;
  HANDLE child_ipc = nullptr, child_msg = nullptr;
  HANDLE child_t2b = nullptr, child_b2t = nullptr, child_close = nullptr;
  if (!::DuplicateHandle(::GetCurrentProcess(), s->ipc_section, proc, &child_ipc,
                         FILE_MAP_READ | FILE_MAP_WRITE | SECTION_QUERY, FALSE,
                         0) ||
      !::DuplicateHandle(::GetCurrentProcess(), s->msg_section, proc, &child_msg,
                         FILE_MAP_READ | FILE_MAP_WRITE | SECTION_QUERY, FALSE,
                         0) ||
      !::DuplicateHandle(::GetCurrentProcess(), s->evt_t2b, proc, &child_t2b,
                         EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, 0) ||
      !::DuplicateHandle(::GetCurrentProcess(), s->evt_b2t, proc, &child_b2t,
                         EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, 0) ||
      !::DuplicateHandle(::GetCurrentProcess(), s->evt_close, proc, &child_close,
                         EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, 0)) {
    BrokerLog("[broker] DuplicateHandle(->child) failed: %lu\n", ::GetLastError());
    return nullptr;
  }

  const uint64_t worker_base = WorkerImageBase(proc);
  // my_base is the base of THIS image (the one holding the sandbox globals we
  // address below). sbox_broker.cc links into sbox.exe and the single-image
  // test exe; CURRENT_MODULE() anchors the globals in either executable.
  const uintptr_t my_base = MyDllBase();
  if (!worker_base || !my_base) {
    BrokerLog("[broker] same-image base read failed (worker=0x%llx my=0x%llx)\n",
              (unsigned long long)worker_base, (unsigned long long)my_base);
    return nullptr;
  }
  BrokerLog("[broker] same-image seed: my_base=0x%llx worker_base=0x%llx%s\n",
            (unsigned long long)my_base, (unsigned long long)worker_base,
            worker_base == my_base ? " (identical)" : " (relocated)");

  // Values for the worker's copies of the sandbox globals (the same values the
  // retired bootstrap carried, written directly instead of through a struct).
  const HANDLE v_section = child_ipc;
  const size_t v_ipc = sandbox::g_sbox_hosted_child_ipc_size;
  const size_t v_policy = sandbox::g_sbox_hosted_child_policy_size;
  const sandbox::IntegrityLevel v_integ = MapIntegrity(policy->delayed_integrity);
  const sandbox::MitigationFlags v_mit =
      policy->prohibit_dynamic_code ? sandbox::MITIGATION_DYNAMIC_CODE_DISABLE : 0;
  SboxWorkerSeed seed = {};
  seed.magic = kSboxWorkerSeedMagic;
  seed.msg_section = reinterpret_cast<uint64_t>(child_msg);
  seed.msg_evt_t2b = reinterpret_cast<uint64_t>(child_t2b);
  seed.msg_evt_b2t = reinterpret_cast<uint64_t>(child_b2t);
  seed.msg_evt_close = reinterpret_cast<uint64_t>(child_close);

  // Carry the opaque app blob broker->worker inline in the seed (design: "lean
  // seed"). Bounded; a blob that doesn't fit fails closed.
  const size_t plugin_data_len =
      (policy->plugin_data && policy->plugin_data_len) ? policy->plugin_data_len
                                                       : 0;
  if (plugin_data_len > kSboxPluginDataMax) {
    BrokerLog("[broker] plugin_data too large: %zu > %zu\n", plugin_data_len,
              kSboxPluginDataMax);
    return nullptr;
  }
  if (plugin_data_len)
    ::memcpy(seed.plugin_data, policy->plugin_data, plugin_data_len);
  seed.plugin_data_len = static_cast<uint32_t>(plugin_data_len);

#if defined(SBOX_DEV_ALLOW_UNSIGNED)
  wchar_t failure_text[2] = {};
  uint32_t failure = 0;
  if (::GetEnvironmentVariableW(L"SBOX_TEST_LIFECYCLE_FAILURE", failure_text, 2) == 1 &&
      failure_text[0] >= L'1' && failure_text[0] <= L'3')
    failure = failure_text[0] - L'0';
  if (!SeedWorkerVar(proc, worker_base, my_base, &g_sbox_lifecycle_test_failure,
                     &failure, sizeof(failure)))
    return nullptr;
#endif

  // All seed writes land while the worker is suspended, before ResumeThread.
  const bool seeded =
      SeedWorkerVar(proc, worker_base, my_base, &sandbox::g_shared_section,
                    &v_section, sizeof(HANDLE)) &&
      SeedWorkerVar(proc, worker_base, my_base, &sandbox::g_shared_IPC_size,
                    &v_ipc, sizeof(size_t)) &&
      SeedWorkerVar(proc, worker_base, my_base, &sandbox::g_shared_policy_size,
                    &v_policy, sizeof(size_t)) &&
      SeedWorkerVar(proc, worker_base, my_base,
                    &sandbox::g_shared_delayed_integrity_level, &v_integ,
                    sizeof(v_integ)) &&
      SeedWorkerVar(proc, worker_base, my_base,
                    &sandbox::g_shared_delayed_mitigations, &v_mit,
                    sizeof(v_mit)) &&
      SeedWorkerVar(proc, worker_base, my_base, &sandbox::g_handle_closer_info,
                    &sandbox::g_sbox_hosted_handle_closer,
                    sizeof(sandbox::HandleCloserConfig)) &&
      SeedWorkerVar(proc, worker_base, my_base, &g_sbox_worker_seed, &seed,
                    sizeof(seed));
  if (!seeded) {
    BrokerLog("[broker] same-image seed write failed: %lu\n", ::GetLastError());
    return nullptr;
  }
  BrokerLog("[broker] same-image seed OK: ipc=%zu policy=%zu + message channel\n",
            v_ipc, v_policy);

#if defined(SBOX_TRUST_TRANSITION_TESTING)
  SboxTrustTransitionTestControl control = {};
  {
    std::lock_guard<std::mutex> lock(g_trust_transition_test_mutex);
    control = g_trust_transition_next_control;
    g_trust_transition_next_control = {};
  }
  if (control.magic == kSboxTrustTransitionControlMagic) {
    control.broker_dll_base = MyDllBase();
    // Same-image seed: write the control struct directly at its shared RVA in
    // the suspended worker (was a FindRemoteExport of an exported symbol).
    if (!SeedWorkerVar(proc, worker_base, my_base,
                       &g_sbox_trust_transition_test_control, &control,
                       sizeof(control))) {
      BrokerLog("[broker] trust-transition control seed failed (err=%lu)\n",
                ::GetLastError());
      return nullptr;
    }
    BrokerLog("[broker] trust-transition selected case=%u serial=%llu\n",
              control.case_id,
              static_cast<unsigned long long>(control.serial));
  }
#endif

#if defined(SBOX_DEV_ALLOW_UNSIGNED)
  // Base-independence probe (see security note): force the child's sbox.exe to
  // relocate when SBOX_FORCE_RELOCATE is set.
  if (::GetEnvironmentVariableW(L"SBOX_FORCE_RELOCATE", nullptr, 0) > 0) {
    uintptr_t host_base = MyDllBase();
    DWORD img_size = MyDllSizeOfImage();
    void* reserved = ::VirtualAllocEx(proc, reinterpret_cast<void*>(host_base),
                                      img_size, MEM_RESERVE, PAGE_NOACCESS);
    BrokerLog("[broker] FORCE_RELOCATE: reserved child range [0x%llx,+0x%lx) -> %s "
           "(err=%lu); child must relocate sbox.dll\n",
           (unsigned long long)host_base, img_size, reserved ? "OK" : "FAILED",
           reserved ? 0 : ::GetLastError());
  }

  if (failure) {
    wchar_t event_name[256] = {};
    if (::GetEnvironmentVariableW(L"SBOX_TEST_LIFECYCLE_HELD_EVENT", event_name, 256)) {
      HANDLE held = ::OpenEventW(SYNCHRONIZE, FALSE, event_name);
      const bool ready = held && ::WaitForSingleObject(held, 5000) == WAIT_OBJECT_0;
      if (held) ::CloseHandle(held);
      if (!ready) return nullptr;
    }
  }
#endif
  // Start the broker reader thread, then let the target run.
  s->reader_thread = ::CreateThread(nullptr, 0, &BrokerReaderThread, s, 0, nullptr);
  if (!s->reader_thread) {
    BrokerLog("[broker] target reader start failed: pid=%lu error=%lu\n",
              target_pid, ::GetLastError());
  }
  const DWORD previous_suspend_count = ::ResumeThread(s->thread);
  const DWORD resume_error =
      previous_suspend_count == static_cast<DWORD>(-1)
          ? ::GetLastError()
          : ERROR_SUCCESS;
  BrokerLog("[broker] target resume: pid=%lu previous_suspend_count=%lu error=%lu\n",
            target_pid, previous_suspend_count, resume_error);
  return pending.release();
}

}  // namespace

// Public entry: callable from ANY thread, including concurrently. Marshals the
// real spawn onto the single launcher thread (Chromium requires every spawn on
// one thread), blocks until it finishes, and returns the session (or nullptr).
SBOX_CORE_API SboxSession* sbox_broker_spawn(const wchar_t* target_exe,
                                        const SboxPolicy* policy,
                                        SboxMessageCb on_message, void* ctx) {
  if (!ValidatePolicy(target_exe, policy))
    return nullptr;
  // Do the heavy, once-per-process init (base + BrokerServices::Init) HERE, on
  // the caller thread, before handing off to the launcher thread. The host may
  // have hardened its DLL search path (SetDefaultDllDirectories) before calling
  // us; running this possibly DLL-loading init on the freshly-created launcher
  // thread can stall under that restricted search, whereas the caller thread
  // (which the host has already been running on) does it cleanly. Only the
  // affinity-bound spawn itself is marshaled to the launcher thread.
  EnsureBase();
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (!EnsureBrokerInitialized()) {
    BrokerLog("[broker] broker not initialized\n");
    return nullptr;
  }
  if (!EnsureLauncherThread()) {
    BrokerLog("[broker] launcher thread unavailable\n");
    return nullptr;
  }
  SpawnRequest req;
  req.target_exe = target_exe;
  req.policy = policy;
  req.on_message = on_message;
  req.ctx = ctx;
  req.done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset
  if (!req.done)
    return nullptr;
  {
    std::lock_guard<std::mutex> lk(g_launcher_mtx);
    g_launcher_queue->push_back(&req);
  }
  ::SetEvent(g_launcher_wake);
  ::WaitForSingleObject(req.done, INFINITE);
  ::CloseHandle(req.done);
  return req.result;
}

SBOX_CORE_API int sbox_broker_post_message(SboxSession* session, int kind,
                                      const void* data, size_t len) {
  if (!session || !session->msg_map)
    return -1;
  sbox_msg::ChannelHeader* h = sbox_msg::Header(session->msg_map);
  if (!sbox_msg::Post(&h->b2t, sbox_msg::B2TRing(session->msg_map), kind, data,
                      len))
    return -2;  // full or oversized
  ::SetEvent(session->evt_b2t);
  return 0;
}

SBOX_CORE_API int sbox_broker_close(SboxSession* session) {
  if (!session || !session->evt_close)
    return -1;
  ::SetEvent(session->evt_close);  // manual-reset: stays signaled
  return 0;
}

SBOX_CORE_API uint32_t sbox_broker_get_process_id(SboxSession* session) {
  return session && session->proc ? ::GetProcessId(session->proc) : 0;
}

SBOX_CORE_API int sbox_broker_wait(SboxSession* session) {
  if (!session)
    return -1;

  if (::WaitForSingleObject(session->proc, 30000) == WAIT_TIMEOUT) {
    BrokerLog("[broker] target timed out; terminating\n");
    ::TerminateProcess(session->proc, 0xDEAD);
    ::WaitForSingleObject(session->proc, 5000);
  }
  DWORD ec = 0;
  ::GetExitCodeProcess(session->proc, &ec);

  // Stop the reader thread. If it is still inside a callback after the wait,
  // keep the session and its mapping alive (a deliberate leak): freeing them
  // under a running reader is a use-after-free.
  if (session->reader_thread) {
    ::SetEvent(session->reader_stop);
    if (::WaitForSingleObject(session->reader_thread, 5000) != WAIT_OBJECT_0) {
      BrokerLog("[broker] reader thread still running; session retained\n");
      return static_cast<int>(ec);
    }
    ::CloseHandle(session->reader_thread);
  }

  // Echo the captured target output.
  if (session->logh != INVALID_HANDLE_VALUE) {
    ::CloseHandle(session->logh);
    HANDLE rd = ::CreateFileW(session->log_path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rd != INVALID_HANDLE_VALUE) {
      BrokerLog("[broker] ----- captured target output -----\n");
      char buf[4096];
      DWORD got = 0;
      BOOL read_ok = TRUE;
      while ((read_ok = ::ReadFile(rd, buf, sizeof(buf), &got, nullptr)) && got)
        BrokerLogRaw(buf, got);
      if (!read_ok) {
        BrokerLog("[broker] target output read failed: path=%ls error=%lu\n",
                  session->log_path.c_str(), ::GetLastError());
      }
      BrokerLog("[broker] -----------------------------------\n");
      ::CloseHandle(rd);
    } else {
      BrokerLog("[broker] target output open failed: path=%ls error=%lu\n",
                session->log_path.c_str(), ::GetLastError());
    }
    if (!::DeleteFileW(session->log_path.c_str())) {
      BrokerLog("[broker] target output cleanup failed: path=%ls error=%lu\n",
                session->log_path.c_str(), ::GetLastError());
    }
  }

  BrokerLog("[broker] target exit=%lu (0x%lx) -> %s\n", ec, ec,
         ec == 0 ? "PASS" : "FAIL");

  if (session->msg_map)
    ::UnmapViewOfFile(session->msg_map);
  for (HANDLE h : {session->msg_section, session->evt_t2b, session->evt_b2t,
                   session->evt_close, session->reader_stop, session->ipc_section,
                   session->thread, session->proc}) {
    if (h)
      ::CloseHandle(h);
  }
  delete session;
  return static_cast<int>(ec);
}

SBOX_CORE_API int sbox_broker_run(const wchar_t* target_exe,
                             const SboxPolicy* policy) {
  SboxSession* s = sbox_broker_spawn(target_exe, policy, nullptr, nullptr);
  if (!s)
    return 1;
  return sbox_broker_wait(s);
}

#if defined(SBOX_TRUST_TRANSITION_TESTING)
SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_set_next_case(
    uint32_t case_id, uint64_t serial) {
  if (case_id == SBOX_TRUST_TRANSITION_CASE_INVALID ||
      case_id == SBOX_TRUST_TRANSITION_CASE_POLICY_SUCCESS ||
      case_id > SBOX_TRUST_TRANSITION_CASE_UNUSED_FILE_HOOK_SUCCESS ||
      serial == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_trust_transition_test_mutex);
  if (g_trust_transition_next_control.magic != 0)
    return 0;
  g_trust_transition_next_control = {
      kSboxTrustTransitionControlMagic, kSboxTrustTransitionControlVersion,
      case_id, serial};
  g_trust_transition_last_output.clear();
  return 1;
}

SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_initialize(
    const SboxTrustTransitionTestControl* control) {
  if (!control || control->magic != kSboxTrustTransitionControlMagic ||
      control->version != kSboxTrustTransitionControlVersion ||
      control->case_id == SBOX_TRUST_TRANSITION_CASE_INVALID ||
      control->case_id > SBOX_TRUST_TRANSITION_CASE_UNUSED_FILE_HOOK_SUCCESS ||
      control->serial == 0) {
    return 0;
  }
  if (control->case_id == SBOX_TRUST_TRANSITION_CASE_POLICY_SUCCESS &&
      !ValidPolicyExpectation(control->policy)) {
    return 0;
  }
  sandbox::trust_transition_test::Initialize(control->case_id);
  return 1;
}

SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_get_thunk_telemetry(
    SboxTrustTransitionThunkTelemetry* telemetry) {
  if (!telemetry)
    return 0;
  *telemetry = sandbox::trust_transition_test::GetThunkTelemetry();
  return 1;
}

SBOX_TRUST_TRANSITION_API const char* sbox_trust_transition_test_last_output() {
  std::lock_guard<std::mutex> lock(g_trust_transition_test_mutex);
  return g_trust_transition_last_output.c_str();
}

SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_set_next_policy_case(
    const SboxTrustTransitionPolicyExpectation* policy, uint64_t serial) {
  if (!policy || !ValidPolicyExpectation(*policy) || !serial)
    return 0;
  std::lock_guard<std::mutex> lock(g_trust_transition_test_mutex);
  if (g_trust_transition_next_control.magic != 0)
    return 0;
  g_trust_transition_next_control = {};
  g_trust_transition_next_control.magic = kSboxTrustTransitionControlMagic;
  g_trust_transition_next_control.version = kSboxTrustTransitionControlVersion;
  g_trust_transition_next_control.case_id =
      SBOX_TRUST_TRANSITION_CASE_POLICY_SUCCESS;
  g_trust_transition_next_control.serial = serial;
  g_trust_transition_next_control.policy = *policy;
  g_trust_transition_last_output.clear();
  return 1;
}

SBOX_TRUST_TRANSITION_API void sbox_trust_transition_test_reset_broker() {
  std::lock_guard<std::mutex> lock(g_trust_transition_test_mutex);
  g_trust_transition_next_control = {};
  g_trust_transition_last_output.clear();
  g_trust_transition_exit_observation = {};
  sandbox::trust_transition_test::SetPlatform(
      SBOX_TRUST_TRANSITION_PLATFORM_CURRENT);
  sandbox::trust_transition_test::SetBrokerFault(SBOX_TRUST_TRANSITION_BROKER_NONE);
}

SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_set_platform(
    uint32_t platform) {
  if (platform > SBOX_TRUST_TRANSITION_PLATFORM_UNKNOWN)
    return 0;
  sandbox::trust_transition_test::SetPlatform(platform);
  return 1;
}

SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_set_broker_fault(
    uint32_t fault) {
  if (fault > SBOX_TRUST_TRANSITION_BROKER_NON_HOSTED)
    return 0;
  sandbox::trust_transition_test::SetBrokerFault(fault);
  return 1;
}

SBOX_TRUST_TRANSITION_API uint32_t
sbox_trust_transition_test_pending_broker_fault() {
  return sandbox::trust_transition_test::BrokerFault();
}

SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_get_exit_observation(
    SboxTrustTransitionExitObservation* observation) {
  if (!observation)
    return 0;
  std::lock_guard<std::mutex> lock(g_trust_transition_test_mutex);
  *observation = g_trust_transition_exit_observation;
  return 1;
}
#endif

SBOX_CORE_API SboxTarget* sbox_target_begin(void) {
  EnsureBase();
  setvbuf(stdout, nullptr, _IONBF, 0);

  // The broker already wrote this worker's sandbox globals (g_shared_section is
  // the is-target gate) and the message-channel seed directly at their shared
  // RVAs while we were suspended — there is no bootstrap struct to copy. The
  // seed magic confirms the broker ran; its absence fails closed.
  if (g_sbox_worker_seed.magic != kSboxWorkerSeedMagic) {
    printf("[sbox] missing same-image seed (magic=0x%llx)\n",
           static_cast<unsigned long long>(g_sbox_worker_seed.magic));
    return nullptr;
  }
  printf("[sbox] same-image seed: section=%p ipc=%zu policy=%zu handle_closer=%d\n",
         reinterpret_cast<void*>(sandbox::g_shared_section),
         static_cast<size_t>(sandbox::g_shared_IPC_size),
         static_cast<size_t>(sandbox::g_shared_policy_size),
         sandbox::g_handle_closer_info.handle_closer_enabled);
  printf("[sbox] image base = 0x%llx (size 0x%lx)\n",
         (unsigned long long)MyDllBase(), MyDllSizeOfImage());

  sandbox::TargetServices* services =
      sandbox::SandboxFactory::GetTargetServices();
  if (!services) {
    printf("[sbox] GetTargetServices() returned null (not sandboxed?)\n");
    return nullptr;
  }
  if (services->Init() != sandbox::SBOX_ALL_OK) {
    printf("[sbox] TargetServices::Init() failed\n");
    return nullptr;
  }
  printf("[sbox] TargetServices::Init() ok\n");

  SboxTarget* target = new SboxTarget();
  target->services = services;

  // Map the duplex message channel from the same-image seed.
  if (g_sbox_worker_seed.msg_section) {
    void* map = ::MapViewOfFile(
        reinterpret_cast<HANDLE>(g_sbox_worker_seed.msg_section),
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (map && sbox_msg::Header(map)->magic == sbox_msg::kMagic) {
      target->msg_map = map;
      target->evt_t2b = reinterpret_cast<HANDLE>(g_sbox_worker_seed.msg_evt_t2b);
      target->evt_b2t = reinterpret_cast<HANDLE>(g_sbox_worker_seed.msg_evt_b2t);
      target->evt_close =
          reinterpret_cast<HANDLE>(g_sbox_worker_seed.msg_evt_close);
      printf("[sbox] message channel mapped\n");
    } else {
      printf("[sbox] message channel map FAILED (err=%lu)\n", ::GetLastError());
    }
  }
  return target;
}

SBOX_CORE_API int sbox_target_test_ipc(SboxTarget* target) {
  if (!target || !target->services)
    return 0;
  return static_cast<sandbox::TargetServicesBase*>(target->services)
                 ->TestIPCPing(1)
             ? 1
             : 0;
}

SBOX_CORE_API int sbox_target_lower_token(SboxTarget* target) {
  if (!target || !target->services)
    return -1;
  // Install the ntdll interceptions while still privileged: ACG (applied by
  // LowerToken) forbids patching executable ntdll pages afterward. The broker
  // can't install these remotely (our sbox core is a different binary, unmapped
  // while we were suspended), so we patch our own.
  // The hosted public policy serializes file rules. An empty policy section
  // means the broker requested no file interceptions.
  const bool file_brokering = sandbox::g_shared_policy_size != 0;
  const sandbox::ResultCode interception =
      sandbox::SelfInstallInterceptions(file_brokering);
  if (interception != sandbox::SBOX_ALL_OK) {
    printf("[sbox] SelfInstallInterceptions() failed: result=%d\n",
           interception);
    return -2;
  }
  target->services->LowerToken();

  const bool strict_acg_requested =
      (sandbox::g_shared_delayed_mitigations &
       sandbox::MITIGATION_DYNAMIC_CODE_DISABLE) != 0;
  if (!strict_acg_requested)
    return 0;

  PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamic_code = {};
#if defined(SBOX_TRUST_TRANSITION_TESTING)
  const bool inject_query =
      sandbox::trust_transition_test::Consume(
          SBOX_TRUST_TRANSITION_CASE_ACG_QUERY_FAILURE);
#else
  const bool inject_query = false;
#endif
  if (inject_query ||
      !::GetProcessMitigationPolicy(::GetCurrentProcess(),
                                    ProcessDynamicCodePolicy, &dynamic_code,
                                    sizeof(dynamic_code))) {
    const DWORD error = inject_query ? ERROR_INVALID_FUNCTION : ::GetLastError();
    printf("[sbox] stage=acg-query expected_strict=1 win32=%lu\n", error);
    return kAcgQueryFailure;
  }
#if defined(SBOX_TRUST_TRANSITION_TESTING)
  if (sandbox::trust_transition_test::Consume(
          SBOX_TRUST_TRANSITION_CASE_ACG_WEAK_STATE)) {
    dynamic_code.ProhibitDynamicCode = 0;
    dynamic_code.AllowThreadOptOut = 1;
    dynamic_code.AllowRemoteDowngrade = 1;
  }
#endif
  if (!dynamic_code.ProhibitDynamicCode || dynamic_code.AllowThreadOptOut ||
      dynamic_code.AllowRemoteDowngrade) {
    printf("[sbox] stage=acg-postcondition expected_strict=1 prohibit=%u "
           "thread_opt_out=%u remote_downgrade=%u\n",
           dynamic_code.ProhibitDynamicCode, dynamic_code.AllowThreadOptOut,
           dynamic_code.AllowRemoteDowngrade);
    return kAcgPostconditionFailure;
  }
  return 0;
}

SBOX_CORE_API void sbox_target_end(SboxTarget* target) {
  if (target && target->msg_map)
    ::UnmapViewOfFile(target->msg_map);
  delete target;  // does not terminate the process
}

SBOX_CORE_API int sbox_target_plugin_data(SboxTarget* target, const void** data,
                                     size_t* len) {
  if (!target || !data || !len)
    return 0;
  *data = g_sbox_worker_seed.plugin_data_len ? g_sbox_worker_seed.plugin_data
                                             : nullptr;
  *len = g_sbox_worker_seed.plugin_data_len;
  return 1;
}

SBOX_CORE_API int sbox_target_acg_enabled(SboxTarget* target) {
  if (!target)
    return 0;
  return (sandbox::g_shared_delayed_mitigations &
          sandbox::MITIGATION_DYNAMIC_CODE_DISABLE) != 0
             ? 1
             : 0;
}

SBOX_CORE_API int sbox_target_post_message(SboxTarget* target, int kind,
                                      const void* data, size_t len) {
  if (!target || !target->msg_map)
    return -1;
  sbox_msg::ChannelHeader* h = sbox_msg::Header(target->msg_map);
  if (!sbox_msg::Post(&h->t2b, sbox_msg::T2BRing(target->msg_map), kind, data,
                      len))
    return -2;  // full or oversized
  ::SetEvent(target->evt_t2b);
  return 0;
}

SBOX_CORE_API int sbox_target_post_lifecycle(SboxTarget* target, uint32_t phase) {
  if (phase != sbox_lifecycle_phase_startup && phase != sbox_lifecycle_phase_security)
    return -1;
  const uint8_t bytes[] = {static_cast<uint8_t>(phase), 0, 0, 0};
  return sbox_target_post_message(target, SBOX_MSG_LIFECYCLE, bytes, sizeof(bytes));
}

SBOX_CORE_API void* sbox_target_inbound_event(SboxTarget* target) {
  return target ? target->evt_b2t : nullptr;
}

SBOX_CORE_API void* sbox_target_close_event(SboxTarget* target) {
  return target ? target->evt_close : nullptr;
}

SBOX_CORE_API int sbox_target_drain_messages(SboxTarget* target, SboxMessageCb cb,
                                        void* ctx) {
  if (!target || !target->msg_map || !cb)
    return 0;
  sbox_msg::ChannelHeader* h = sbox_msg::Header(target->msg_map);
  const int drained = sbox_msg::Drain(&h->b2t, sbox_msg::B2TRing(target->msg_map), cb, ctx);
  return drained < 0 ? 0 : drained;
}

#if defined(SBOX_LIFECYCLE_TESTING)
bool SboxTestReader(bool final_drain, bool suppress_exit, const std::wstring& exe) {
  struct Observation {
    HANDLE result_seen;
    HANDLE exit_seen;
    HANDLE barrier_seen;
    std::vector<std::pair<int, std::vector<uint8_t>>> frames;
    static void Message(void* context, int kind, const void* data, size_t size) {
      auto& o = *static_cast<Observation*>(context);
      const auto* bytes = static_cast<const uint8_t*>(data);
      o.frames.emplace_back(kind, std::vector<uint8_t>(bytes, bytes + size));
      if (kind == SBOX_MSG_LIFECYCLE && size == 4 && bytes[0] == sbox_lifecycle_phase_exit)
        ::SetEvent(o.exit_seen);
      if (kind == SBOX_MSG_BINARY && size == 1 && bytes[0] == 9)
        ::SetEvent(o.result_seen);
      if (kind == SBOX_MSG_BINARY && size == 1 && bytes[0] == 10)
        ::SetEvent(o.barrier_seen);
    }
  };
  SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
  HANDLE child_exit = ::CreateEventW(&sa, TRUE, FALSE, nullptr);
  Observation observation{::CreateEventW(nullptr, FALSE, FALSE, nullptr),
                          ::CreateEventW(nullptr, FALSE, FALSE, nullptr),
                          ::CreateEventW(nullptr, FALSE, FALSE, nullptr), {}};
  std::wstring command = L"\"" + exe + L"\" --reader-child=" +
      std::to_wstring(reinterpret_cast<uintptr_t>(child_exit));
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child = {};
  if (!child_exit || !observation.result_seen || !observation.exit_seen ||
      !observation.barrier_seen || !::CreateProcessW(exe.c_str(), command.data(),
      nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &child)) {
    if (child_exit) ::CloseHandle(child_exit);
    for (HANDLE event : {observation.result_seen, observation.exit_seen,
                         observation.barrier_seen})
      if (event) ::CloseHandle(event);
    return false;
  }
  SboxSession session;
  session.proc = child.hProcess;
  session.evt_t2b = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  session.reader_stop = ::CreateEventW(nullptr, TRUE, suppress_exit, nullptr);
  std::vector<uint8_t> mapping(sbox_msg::kSectionSize);
  session.msg_map = mapping.data();
  session.on_message = &Observation::Message;
  session.on_message_ctx = &observation;
  sbox_msg::InitHeader(session.msg_map);
  auto* h = sbox_msg::Header(session.msg_map);
  auto post = [&](int kind, const uint8_t* data, size_t size) {
    return sbox_msg::Post(&h->t2b, sbox_msg::T2BRing(session.msg_map), kind, data, size);
  };
  bool ok = session.evt_t2b && session.reader_stop;
  for (uint8_t phase : {uint8_t(3), uint8_t(0), uint8_t(4), uint8_t(255)}) {
    const uint8_t bytes[] = {phase, 0, 0, 0};
    ok = post(SBOX_MSG_LIFECYCLE, bytes, 4) && ok;
  }
  const uint8_t high_byte[] = {1, 1, 0, 0};
  ok = post(SBOX_MSG_LIFECYCLE, high_byte, 4) && ok;
  const uint8_t malformed[] = {1, 0, 0, 0, 0};
  ok = post(SBOX_MSG_LIFECYCLE, malformed, 3) && post(SBOX_MSG_LIFECYCLE, malformed, 5) && ok;
  for (uint8_t phase : {uint8_t(1), uint8_t(2)}) {
    const uint8_t bytes[] = {phase, 0, 0, 0};
    ok = post(SBOX_MSG_LIFECYCLE, bytes, 4) && ok;
  }
  const uint8_t result[] = {9};
  ok = post(SBOX_MSG_BINARY, result, 1) && ok;
  if (final_drain || suppress_exit) {
    ::SetEvent(child_exit);
    ok = ::WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0 && ok;
  }
  HANDLE reader = ::CreateThread(nullptr, 0, &BrokerReaderThread, &session, 0, nullptr);
  ok = reader && ok;
  if (!suppress_exit && reader) {
    if (!final_drain) {
      ::SetEvent(session.evt_t2b);
      ok = ::WaitForSingleObject(observation.result_seen, 5000) == WAIT_OBJECT_0 && ok;
      ::SetEvent(child_exit);
    }
    ok = ::WaitForSingleObject(observation.exit_seen, 5000) == WAIT_OBJECT_0 && ok;
    // The still-signaled process cannot displace a subsequent ring barrier.
    const uint8_t barrier[] = {10};
    ok = post(SBOX_MSG_BINARY, barrier, 1) && ok;
    ::SetEvent(session.evt_t2b);
    ok = ::WaitForSingleObject(observation.barrier_seen, 5000) == WAIT_OBJECT_0 && ok;
  }
  ::SetEvent(session.reader_stop);
  if (reader) {
    ok = ::WaitForSingleObject(reader, 5000) == WAIT_OBJECT_0 && ok;
    ::CloseHandle(reader);
  }
  ::SetEvent(child_exit);
  ok = ::WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0 && ok;
  const std::vector<std::pair<int, std::vector<uint8_t>>> expected = {
      {SBOX_MSG_LIFECYCLE, {1,0,0,0}}, {SBOX_MSG_LIFECYCLE, {2,0,0,0}},
      {SBOX_MSG_BINARY, {9}}, {SBOX_MSG_LIFECYCLE, {3,0,0,0}}, {SBOX_MSG_BINARY, {10}}};
  ok = (suppress_exit ? observation.frames.empty() : observation.frames == expected) && ok;
  for (HANDLE handle : {child_exit, observation.result_seen, observation.exit_seen,
                       observation.barrier_seen, child.hProcess, child.hThread,
                       session.evt_t2b, session.reader_stop})
    if (handle) ::CloseHandle(handle);
  return ok;
}
#endif
