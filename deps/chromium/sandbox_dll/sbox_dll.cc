// sbox.dll — the Chromium sandbox engine behind a generic, V8-agnostic C ABI.
// One signed DLL, loaded by two different binaries: a broker host
// (sbox_broker_*) and an app-defined target EXE (sbox_target_*). Contains NO
// knowledge of v8jsi, JavaScript, jitless, or any guest engine — the target EXE
// owns all of that.

#define SBOX_DLL_IMPL
#include "msg_channel.h"
#include "sbox.h"

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
#define SBOX_TRUST_TRANSITION_TEST_DLL_IMPL
#include "sbox_trust_transition_test_private.h"
#endif

// Hosted (decoupled-DLL) mode hooks. We are inside sbox.dll (linked with the
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
  SboxMessageCb on_message = nullptr;
  void* on_message_ctx = nullptr;
};

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

// Broker diagnostics: mirror to stdout (console harnesses like test_app) and to
// the debugger via OutputDebugString, so broker failures are visible from GUI
// hosts that have no console. Capture with DebugView/DBWIN.
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
  ::fputs(buf, stdout);
  ::OutputDebugStringA(buf);
}

// Length-delimited variant for the relayed target output (not NUL-terminated).
void BrokerLogRaw(const char* data, size_t len) {
#if defined(SBOX_TRUST_TRANSITION_TESTING)
  {
    std::lock_guard<std::mutex> lock(g_trust_transition_test_mutex);
    g_trust_transition_last_output.append(data, len);
  }
#endif
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

// Base + SizeOfImage of our own sbox.dll in this process (diagnostics + the
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
  // our own sbox.dll image, always the build arch. (Was hardcoded ...64.)
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return 0;
  return nt->OptionalHeader.SizeOfImage;
}

// Finds the address of an exported symbol in a (suspended) child process by
// reading its PEB -> EXE base and parsing the EXE export table. We deliver the
// bootstrap into the child's EXE image (mapped at creation, unlike sbox.dll).
uint64_t FindRemoteExport(HANDLE proc, const char* name) {
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

  auto rpm = [&](uint64_t addr, void* out, size_t n) -> bool {
    SIZE_T got = 0;
    return ::ReadProcessMemory(proc, reinterpret_cast<void*>(addr), out, n,
                               &got) &&
           got == n;
  };

  // v8-jsi: PEB->ImageBaseAddress offset + the pointer width are arch-
  // specific. The broker and target are always the SAME arch (the Chromium
  // sandbox requires a uniform-ABI broker/target pair), so we read the native
  // pointer width: x64 PEB has ImageBaseAddress at 0x10, x86 PEB at 0x08.
  // (Was hardcoded to the x64 layout, which read the wrong field for a 32-bit
  // target and made FindRemoteExport return 0.)
  const uint64_t kImageBaseOff = sizeof(void*) == 8 ? 0x10 : 0x08;
  uintptr_t image_base_native = 0;
  if (!rpm(reinterpret_cast<uint64_t>(pbi.PebBaseAddress) + kImageBaseOff,
           &image_base_native, sizeof(image_base_native)))
    return 0;
  uint64_t image_base = image_base_native;

  IMAGE_DOS_HEADER dos = {};
  if (!rpm(image_base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
    return 0;
  IMAGE_NT_HEADERS nt = {};
  if (!rpm(image_base + dos.e_lfanew, &nt, sizeof(nt)) ||
      nt.Signature != IMAGE_NT_SIGNATURE)
    return 0;
  DWORD exp_rva =
      nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]
          .VirtualAddress;
  if (!exp_rva)
    return 0;
  IMAGE_EXPORT_DIRECTORY exp = {};
  if (!rpm(image_base + exp_rva, &exp, sizeof(exp)))
    return 0;

  for (DWORD i = 0; i < exp.NumberOfNames; ++i) {
    DWORD name_rva = 0;
    if (!rpm(image_base + exp.AddressOfNames + i * sizeof(DWORD), &name_rva,
             sizeof(DWORD)))
      return 0;
    char buf[64] = {};
    if (!rpm(image_base + name_rva, buf, sizeof(buf) - 1))
      continue;
    if (std::strcmp(buf, name) != 0)
      continue;
    WORD ord = 0;
    if (!rpm(image_base + exp.AddressOfNameOrdinals + i * sizeof(WORD), &ord,
             sizeof(WORD)))
      return 0;
    DWORD func_rva = 0;
    if (!rpm(image_base + exp.AddressOfFunctions + ord * sizeof(DWORD),
             &func_rva, sizeof(DWORD)))
      return 0;
    return image_base + func_rva;
  }
  return 0;
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

void DiscardFrame(void*, int, const void*, size_t) {}

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
      // Report exit once, but keep the original message/stop behavior.
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
    sbox_msg::ChannelHeader* h = sbox_msg::Header(s->msg_map);
    sbox_msg::FrameCb cb = s->on_message ? s->on_message : &DiscardFrame;
    sbox_msg::Drain(&h->t2b, sbox_msg::T2BRing(s->msg_map), cb,
                    s->on_message_ctx);
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
  // sbox.dll. Create the shared IPC section here (target->Init adopts it); after
  // the spawn we duplicate it + the message-channel handles into the child and
  // relay the values via the child's EXE bootstrap struct.
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
  BrokerLog("[broker] spawning target binary: %ls\n", target_exe);

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

  // Relay the bootstrap to the child's EXE struct while it is still suspended
  // (its EXE image is mapped; its sbox.dll is not). Duplicate the IPC section +
  // the message-channel section/events into the child, then PE-find
  // g_sbox_bootstrap and write the values.
  HANDLE proc = s->proc;
  SboxBootstrap bootstrap = {};
  bootstrap.magic = SBOX_BOOTSTRAP_MAGIC;
  if (!::DuplicateHandle(
          ::GetCurrentProcess(), s->ipc_section, proc,
          reinterpret_cast<HANDLE*>(&bootstrap.section_handle),
          FILE_MAP_READ | FILE_MAP_WRITE | SECTION_QUERY, FALSE, 0) ||
      !::DuplicateHandle(::GetCurrentProcess(), s->msg_section, proc,
                         reinterpret_cast<HANDLE*>(&bootstrap.msg_section),
                         FILE_MAP_READ | FILE_MAP_WRITE | SECTION_QUERY, FALSE,
                         0) ||
      !::DuplicateHandle(::GetCurrentProcess(), s->evt_t2b, proc,
                         reinterpret_cast<HANDLE*>(&bootstrap.msg_evt_t2b),
                         EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, 0) ||
      !::DuplicateHandle(::GetCurrentProcess(), s->evt_b2t, proc,
                         reinterpret_cast<HANDLE*>(&bootstrap.msg_evt_b2t),
                         EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, 0) ||
      !::DuplicateHandle(::GetCurrentProcess(), s->evt_close, proc,
                         reinterpret_cast<HANDLE*>(&bootstrap.msg_evt_close),
                         EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, 0)) {
    BrokerLog("[broker] DuplicateHandle(->child) failed: %lu\n", ::GetLastError());
    return nullptr;
  }
  bootstrap.ipc_size = sandbox::g_sbox_hosted_child_ipc_size;
  bootstrap.policy_size = sandbox::g_sbox_hosted_child_policy_size;
  bootstrap.integrity = MapIntegrity(policy->delayed_integrity);
  bootstrap.mitigations =
      policy->prohibit_dynamic_code ? sandbox::MITIGATION_DYNAMIC_CODE_DISABLE : 0;
  // Relay the handle-closer config (parity with SetupHandleCloser).
  static_assert(sizeof(sandbox::HandleCloserConfig) <=
                    sizeof(bootstrap.handle_closer),
                "HandleCloserConfig does not fit the bootstrap relay field");
  std::memcpy(bootstrap.handle_closer, &sandbox::g_sbox_hosted_handle_closer,
              sizeof(sandbox::HandleCloserConfig));
  uint64_t addr = FindRemoteExport(proc, "g_sbox_bootstrap");
  SIZE_T wrote = 0;
  if (!addr ||
      !::WriteProcessMemory(proc, reinterpret_cast<void*>(addr), &bootstrap,
                            sizeof(bootstrap), &wrote) ||
      wrote != sizeof(bootstrap)) {
    BrokerLog("[broker] relay bootstrap failed (addr=0x%llx err=%lu)\n",
           (unsigned long long)addr, ::GetLastError());
    return nullptr;
  }
  BrokerLog("[broker] relayed bootstrap @0x%llx: ipc=%u policy=%u + message channel\n",
         (unsigned long long)addr, bootstrap.ipc_size, bootstrap.policy_size);

#if defined(SBOX_TRUST_TRANSITION_TESTING)
  SboxTrustTransitionTestControl control = {};
  {
    std::lock_guard<std::mutex> lock(g_trust_transition_test_mutex);
    control = g_trust_transition_next_control;
    g_trust_transition_next_control = {};
  }
  if (control.magic == kSboxTrustTransitionControlMagic) {
    control.broker_dll_base = MyDllBase();
    const uint64_t control_addr =
        FindRemoteExport(proc, "g_sbox_trust_transition_test_control");
    wrote = 0;
    if (!control_addr ||
        !::WriteProcessMemory(proc, reinterpret_cast<void*>(control_addr),
                              &control, sizeof(control), &wrote) ||
        wrote != sizeof(control)) {
      BrokerLog("[broker] trust-transition control relay failed "
                "(addr=0x%llx err=%lu)\n",
                static_cast<unsigned long long>(control_addr),
                ::GetLastError());
      return nullptr;
    }
    BrokerLog("[broker] trust-transition selected case=%u serial=%llu\n",
              control.case_id,
              static_cast<unsigned long long>(control.serial));
  }
#endif

  // Base-independence probe (see security note): force the child's sbox.dll to
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
SBOX_API SboxSession* sbox_broker_spawn(const wchar_t* target_exe,
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

SBOX_API int sbox_broker_post_message(SboxSession* session, int kind,
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

SBOX_API int sbox_broker_close(SboxSession* session) {
  if (!session || !session->evt_close)
    return -1;
  ::SetEvent(session->evt_close);  // manual-reset: stays signaled
  return 0;
}

SBOX_API int sbox_broker_wait(SboxSession* session) {
  if (!session)
    return -1;

  if (::WaitForSingleObject(session->proc, 30000) == WAIT_TIMEOUT) {
    BrokerLog("[broker] target timed out; terminating\n");
    ::TerminateProcess(session->proc, 0xDEAD);
    ::WaitForSingleObject(session->proc, 5000);
  }
  DWORD ec = 0;
  ::GetExitCodeProcess(session->proc, &ec);

  // Stop the reader thread.
  if (session->reader_thread) {
    ::SetEvent(session->reader_stop);
    ::WaitForSingleObject(session->reader_thread, 5000);
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

SBOX_API int sbox_broker_run(const wchar_t* target_exe,
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

SBOX_API SboxTarget* sbox_target_begin(const SboxBootstrap* boot) {
  EnsureBase();
  setvbuf(stdout, nullptr, _IONBF, 0);

  if (!boot || boot->magic != SBOX_BOOTSTRAP_MAGIC) {
    printf("[sbox] missing/invalid bootstrap (magic=0x%llx)\n",
           boot ? static_cast<unsigned long long>(boot->magic) : 0ull);
    return nullptr;
  }
  // Populate the sandbox globals BEFORE GetTargetServices (g_shared_section is
  // the is-target gate). The broker relayed these into this EXE's exported
  // g_sbox_bootstrap while the process was suspended.
  sandbox::g_shared_section = reinterpret_cast<HANDLE>(boot->section_handle);
  sandbox::g_shared_IPC_size = boot->ipc_size;
  sandbox::g_shared_policy_size = boot->policy_size;
  sandbox::g_shared_delayed_integrity_level =
      static_cast<sandbox::IntegrityLevel>(boot->integrity);
  sandbox::g_shared_delayed_mitigations =
      static_cast<sandbox::MitigationFlags>(boot->mitigations);
  // Apply the relayed handle-closer config before LowerToken runs its
  // CloseOpenHandles step (parity with the broker's skipped SetupHandleCloser).
  std::memcpy(&sandbox::g_handle_closer_info, boot->handle_closer,
              sizeof(sandbox::HandleCloserConfig));
  printf("[sbox] target bootstrap: section=%p ipc=%u policy=%u handle_closer=%d\n",
         reinterpret_cast<void*>(sandbox::g_shared_section), boot->ipc_size,
         boot->policy_size, sandbox::g_handle_closer_info.handle_closer_enabled);
  printf("[sbox] sbox.dll base = 0x%llx (size 0x%lx)\n",
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

  // Map the duplex message channel relayed by the broker.
  if (boot->msg_section) {
    void* map = ::MapViewOfFile(reinterpret_cast<HANDLE>(boot->msg_section),
                                FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (map && sbox_msg::Header(map)->magic == sbox_msg::kMagic) {
      target->msg_map = map;
      target->evt_t2b = reinterpret_cast<HANDLE>(boot->msg_evt_t2b);
      target->evt_b2t = reinterpret_cast<HANDLE>(boot->msg_evt_b2t);
      target->evt_close = reinterpret_cast<HANDLE>(boot->msg_evt_close);
      printf("[sbox] message channel mapped\n");
    } else {
      printf("[sbox] message channel map FAILED (err=%lu)\n", ::GetLastError());
    }
  }
  return target;
}

SBOX_API int sbox_target_test_ipc(SboxTarget* target) {
  if (!target || !target->services)
    return 0;
  return static_cast<sandbox::TargetServicesBase*>(target->services)
                 ->TestIPCPing(1)
             ? 1
             : 0;
}

SBOX_API int sbox_target_lower_token(SboxTarget* target) {
  if (!target || !target->services)
    return -1;
  // Install the ntdll interceptions while still privileged: ACG (applied by
  // LowerToken) forbids patching executable ntdll pages afterward. The broker
  // can't install these remotely (our sbox.dll is a different binary, unmapped
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

SBOX_API void sbox_target_end(SboxTarget* target) {
  if (target && target->msg_map)
    ::UnmapViewOfFile(target->msg_map);
  delete target;  // does not terminate the process
}

SBOX_API int sbox_target_post_message(SboxTarget* target, int kind,
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

SBOX_API void* sbox_target_inbound_event(SboxTarget* target) {
  return target ? target->evt_b2t : nullptr;
}

SBOX_API void* sbox_target_close_event(SboxTarget* target) {
  return target ? target->evt_close : nullptr;
}

SBOX_API int sbox_target_drain_messages(SboxTarget* target, SboxMessageCb cb,
                                        void* ctx) {
  if (!target || !target->msg_map || !cb)
    return 0;
  sbox_msg::ChannelHeader* h = sbox_msg::Header(target->msg_map);
  return sbox_msg::Drain(&h->b2t, sbox_msg::B2TRing(target->msg_map), cb, ctx);
}
