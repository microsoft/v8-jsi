// sbox_main.cc — the product single-image sbox.exe: one binary that is BOTH the
// sandbox broker and the sandbox worker, selected by argv. It statically links
// the same Chromium sandbox core as the DLL did (via sbox_broker.cc) so the broker
// and worker share one image and one set of sandbox globals.
//
// Personas (role is chosen first; neither falls through into the other's init).
// The app-specific behavior lives in a plugin DLL named by --plugin (resolved by
// name via the sbox.h plugin ABI); sbox.exe itself stays generic + app-agnostic:
//   sbox.exe --broker --plugin <dll>  : load+verify the plugin, plugin.configure
//                         supplies the sandbox policy, spawn "<self> --worker",
//                         round-trip. Drives sbox_broker_* + the plugin's configure.
//   sbox.exe --worker --plugin <dll>  : sbox_target_begin -> plugin.warmup ->
//                         lower_token -> plugin.run -> plugin.shutdown.
//   sbox.exe --broker --mode=<mode> --pipe=<name> --plugin <dll>
//                       : invoke the plugin-owned coordinator service.
//   sbox.exe --pipe-bench : developer transport-latency probe.
//
// SBOX_CORE_API is plain extern "C" here (the target defines SBOX_STATIC), so the
// sbox_* core compiled into this EXE stays out of its export table.
#include "sbox_core_internal.h"
#include "sbox_harden.h"
#include "sbox.h"
#include "sbox_config.h"
#include "v8host_file_identity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <delayimp.h>

#include <cstdio>
#include <cstring>
#include <array>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Same-image seed: the broker writes this worker's sandbox globals + message
// channel directly at their shared RVAs while the worker is suspended, so there
// is no exported bootstrap struct and no export-table walk. sbox_target_begin
// adopts that seed (and fails closed if its magic is absent).

// The host's private definitions of sbox.h's opaque handles. Defined at GLOBAL
// scope (matching sbox.h's forward declarations), not in the anonymous namespace
// below, so they are the SAME ::sbox_worker_s / ::sbox_broker_s the typedefs
// name. (::sbox_config_s + its setters live in sbox_config.h/.cc, shared with the
// config unit test.)

// sbox_worker: the worker's run context — just the sandbox target the worker_api
// services bridge to.
struct sbox_worker_s {
  SboxTarget* target = nullptr;
};

struct sbox_broker_s;
struct sbox_broker_worker_s {
  sbox_broker_s* broker = nullptr;
  SboxSession* session = nullptr;
  sbox_message_cb callback = nullptr;
  void* callback_context = nullptr;
  bool close_called = false;
  bool wait_called = false;
};

struct sbox_broker_s {
  std::wstring self_path;
  std::wstring plugin_name;
  std::wstring endpoint_name;
  std::wstring container_final_path;
  std::wstring plugin_final_path;
  const sbox_plugin_api* plugin = nullptr;
  HMODULE plugin_module = nullptr;
  std::mutex mutex;
  std::set<sbox_broker_worker_s*> workers;
  bool stopping = false;
};

namespace {

// Worker exit codes (the broker asserts 0). Distinct values make a failed run
// self-describing in the captured worker log.
enum WorkerExit {
  kOk = 0,
  kBeginFailed = 10,
  kPingPreFailed = 11,
  kDllLoadFailed = 12,
  kDllResolveFailed = 13,
  kWarmupFailed = 14,
  kLowerTokenFailed = 15,
  kPingPostFailed = 16,
  kRunFailed = 17,  // plugin.run returned non-sbox_status_ok (see worker log; e.g. ACG not enforced)
  kDelayLoadRefused = 18,  // the worker reached a delay-loaded import (see below)
};

// user32/shell32/ole32 are delay-loaded for the broker role only. An
// AppContainer worker that initializes user32 on a desktop granting no app
// package (a service logon's window station) dies in the loader, so the
// worker must never load them. Refuse every delay load in the worker role, on
// any desktop, so a regression fails fast and visibly everywhere.
bool g_worker_role = false;

FARPROC WINAPI WorkerDelayLoadGuard(unsigned notify, PDelayLoadInfo info) {
  if (g_worker_role && notify == dliStartProcessing) {
    printf("[sbox] worker: refused delay load of %s\n",
           info && info->szDll ? info->szDll : "?");
    ::TerminateProcess(::GetCurrentProcess(), kDelayLoadRefused);
  }
  return nullptr;
}

}  // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = WorkerDelayLoadGuard;

namespace {

std::wstring SelfPath() {
  wchar_t buf[MAX_PATH] = {};
  const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::wstring(buf, n);
}

std::wstring Widen(const char* s);

std::wstring ParseValue(int argc, char** argv, const char* name) {
  const size_t name_length = std::strlen(name);
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], name, name_length) == 0 &&
        argv[i][name_length] == '=')
      return Widen(argv[i] + name_length + 1);
  }
  return std::wstring();
}

bool ParseBrokerMode(int argc, char** argv, sbox_broker_mode* mode) {
  const std::wstring value = ParseValue(argc, argv, "--mode");
  if (value == L"shared") {
    *mode = sbox_broker_mode_shared;
    return true;
  }
  if (value == L"dedicated") {
    *mode = sbox_broker_mode_dedicated;
    return true;
  }
  return false;
}

bool ParsePipeName(int argc, char** argv, std::wstring* pipe) {
  const std::wstring value = ParseValue(argc, argv, "--pipe");
  constexpr wchar_t kPrefix[] = L"\\\\.\\pipe\\v8host-rv2-";
  if (value.size() != (sizeof(kPrefix) / sizeof(kPrefix[0]) - 1) + 64 ||
      value.compare(0, sizeof(kPrefix) / sizeof(kPrefix[0]) - 1, kPrefix) != 0)
    return false;
  for (size_t i = sizeof(kPrefix) / sizeof(kPrefix[0]) - 1; i < value.size();
       ++i) {
    if (!((value[i] >= L'0' && value[i] <= L'9') ||
          (value[i] >= L'a' && value[i] <= L'f')))
      return false;
  }
  *pipe = value;
  return true;
}

bool ParseNonce(int argc, char** argv, std::array<uint8_t, 16>* nonce) {
  const std::wstring value = ParseValue(argc, argv, "--nonce");
  if (value.size() != nonce->size() * 2)
    return false;
  for (size_t i = 0; i < nonce->size(); ++i) {
    auto hex = [](wchar_t c) -> int {
      if (c >= L'0' && c <= L'9')
        return c - L'0';
      if (c >= L'a' && c <= L'f')
        return c - L'a' + 10;
      return -1;
    };
    const int high = hex(value[i * 2]);
    const int low = hex(value[i * 2 + 1]);
    if (high < 0 || low < 0)
      return false;
    (*nonce)[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

// Widen a narrow argv token (plugin names are ASCII; UTF-8 is a safe superset).
std::wstring Widen(const char* s) {
  if (!s || !*s)
    return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  if (n <= 1)
    return std::wstring();
  std::wstring w(static_cast<size_t>(n - 1), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
  return w;
}

// Parse the plugin DLL name. Accepts both "--plugin <name>" (manual CLI) and the
// "--plugin=<name>" spelling base::CommandLine renders when the broker propagates
// it to the worker. Returns "" if absent.
std::wstring ParsePluginName(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--plugin") == 0)
      return (i + 1 < argc) ? Widen(argv[i + 1]) : std::wstring();
    if (std::strncmp(argv[i], "--plugin=", 9) == 0)
      return Widen(argv[i] + 9);
  }
  return std::wstring();
}

// ===== the sbox.h plugin ABI, host side =====
// sbox.exe is a generic container; the plugin DLL (resolved by --plugin) carries
// all app knowledge. The broker calls plugin.configure to collect the sandbox
// policy (the sbox_config_api setters + the MarshalSboxConfig gate live in
// sbox_config.cc); the worker drives warmup -> lower_token -> run -> shutdown.

// WkDrainMessages hands the plugin's sbox_message_cb to the core's drain, which
// calls it as SboxMessageCb; the two differ only in the kind parameter type
// (sbox_msg_kind vs int), so that must stay ABI-identical.
static_assert(sizeof(sbox_msg_kind) == sizeof(int) &&
                  static_cast<int>(sbox_msg_kind_string) ==
                      static_cast<int>(SBOX_MSG_STRING) &&
                  static_cast<int>(sbox_msg_kind_binary) ==
                      static_cast<int>(SBOX_MSG_BINARY) &&
                  static_cast<int>(sbox_msg_kind_lifecycle) ==
                      static_cast<int>(SBOX_MSG_LIFECYCLE),
              "sbox_msg_kind must match SboxMsgKind for the drain cast");

// --- sbox_worker_api: worker services the plugin's warmup()/run() call, bridged
// to the sandbox core's sbox_target_* message channel ---
sbox_status SBOX_CDECL WkGetPluginData(sbox_worker w, const void** out_data,
                                       size_t* out_size) {
  if (!w || !w->target || !out_data || !out_size)
    return sbox_status_invalid_arg;
  return sbox_target_plugin_data(w->target, out_data, out_size)
             ? sbox_status_ok
             : sbox_status_failed;
}
bool SBOX_CDECL WkAcgEnabled(sbox_worker w) {
  return (w && w->target) ? sbox_target_acg_enabled(w->target) != 0 : false;
}
sbox_status SBOX_CDECL WkPostMessage(sbox_worker w, sbox_msg_kind kind,
                                     const void* data, size_t size) {
  if (!w || !w->target)
    return sbox_status_invalid_arg;
  // The engine/guest may post only opaque string/binary frames. sbox_msg_kind_lifecycle
  // (and any other kind) is container-only, so reject it here — a readiness marker
  // cannot be submitted through this API. Code executing inside the worker can
  // still forge the underlying channel; readiness is not proof of worker integrity.
  if (kind != sbox_msg_kind_string && kind != sbox_msg_kind_binary)
    return sbox_status_invalid_arg;
  return sbox_target_post_message(w->target, static_cast<int>(kind), data,
                                  size) == 0
             ? sbox_status_ok
             : sbox_status_failed;
}
void* SBOX_CDECL WkInboundEvent(sbox_worker w) {
  return (w && w->target) ? sbox_target_inbound_event(w->target) : nullptr;
}
sbox_status SBOX_CDECL WkDrainMessages(sbox_worker w,
                                       sbox_message_cb on_message,
                                       void* user_data) {
  if (!w || !w->target || !on_message)
    return sbox_status_invalid_arg;
  // sbox_message_cb and SboxMessageCb differ only in the kind parameter's type
  // (sbox_msg_kind vs int) — ABI-identical, so the reinterpret_cast is safe.
  sbox_target_drain_messages(
      w->target, reinterpret_cast<SboxMessageCb>(on_message), user_data);
  return sbox_status_ok;
}
void* SBOX_CDECL WkCloseEvent(sbox_worker w) {
  return (w && w->target) ? sbox_target_close_event(w->target) : nullptr;
}

sbox_worker_api MakeWorkerApi() {
  sbox_worker_api api = SBOX_WORKER_API_INIT;
  api.get_plugin_data = &WkGetPluginData;
  api.acg_enabled = &WkAcgEnabled;
  api.post_message = &WkPostMessage;
  api.inbound_event = &WkInboundEvent;
  api.drain_messages = &WkDrainMessages;
  api.close_event = &WkCloseEvent;
  return api;
}

// Load a --plugin DLL and hand back its validated sbox_plugin_api vtable. The broker
// runs UNRESTRICTED, so IT (not the sandbox worker, which cannot reliably reach
// the crypto/catalog services once locked down) Authenticode-verifies the DLL
// BEFORE load; the worker re-resolves the already-verified DLL with verify=false.
// Dev builds tolerate UNSIGNED only via SBOX_DEV_ALLOW_UNSIGNED (no runtime
// bypass).
bool LoadPlugin(const std::wstring& plugin_name, bool verify, const char* tag,
                HMODULE* out_mod, const sbox_plugin_api** out_vtable,
                v8host::HeldFile* out_file) {
  if (!IsBareFilename(plugin_name)) {
    printf("[sbox] %s: invalid --plugin name (bare app-dir filename only)\n",
           tag);
    return false;
  }
  const std::wstring full = sbox_harden::ExeDir() + plugin_name;
  v8host::HeldFile held;
  DWORD error = ERROR_SUCCESS;
  if (!v8host::OpenImmutableFile(full, &held, &error,
      verify ? v8host::FinalPathMode::kRequired : v8host::FinalPathMode::kOmitForWorkerHold)) {
    printf("[sbox] %s: immutable open failed: %lu\n", tag, error);
    return false;
  }
  if (verify && !v8host::TrustAllowed(v8host::VerifyTrust(held))) {
    printf("[sbox] %s: signature verification failed\n", tag);
    return false;
  }
#if !defined(SBOX_DEV_ALLOW_UNSIGNED)
  if (verify) {
    v8host::HeldFile container;
    if (!v8host::OpenImmutableFile(SelfPath(), &container, &error) ||
        !v8host::IsPayloadFileAllowed(
            v8host::ParentPath(v8host::ParentPath(container.final_path())), held)) {
      printf("[sbox] %s: plugin escaped install root\n", tag);
      return false;
    }
  }
#endif
  HMODULE mod = ::LoadLibraryExW(full.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                     LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
  if (!mod) {
    printf("[sbox] %s: LoadLibrary(%ls) failed: %lu\n", tag,
           plugin_name.c_str(), ::GetLastError());
    return false;
  }
  auto entry = reinterpret_cast<sbox_plugin_main_fn>(
      ::GetProcAddress(mod, SBOX_PLUGIN_ENTRY));
  const sbox_plugin_api* vtable = nullptr;
  if (!entry || entry(SBOX_ABI_VERSION, &vtable) != sbox_status_ok || !vtable) {
    printf("[sbox] %s: %s did not return a plugin\n", tag, SBOX_PLUGIN_ENTRY);
    ::FreeLibrary(mod);
    return false;
  }
  if (vtable->abi_version != SBOX_ABI_VERSION ||
      vtable->struct_size < sizeof(sbox_plugin_api) || !vtable->configure ||
      !vtable->broker_run || !vtable->warmup || !vtable->run ||
      !vtable->shutdown) {
    printf("[sbox] %s: plugin ABI mismatch (abi=%u host=%u) or incomplete "
           "vtable\n",
           tag, vtable->abi_version, SBOX_ABI_VERSION);
    ::FreeLibrary(mod);
    return false;
  }
  *out_mod = mod;
  *out_vtable = vtable;
  if (out_file)
    *out_file = std::move(held);
  return true;
}

// The broker's prepared sandbox: the plugin vtable + the SboxPolicy it described,
// with all pointer-referenced storage owned here. Must outlive every spawn that
// uses `marshal.policy` (its pointers alias cfg + marshal), so never copy it
// after PrepareSandbox fills it.
struct PreparedSandbox {
  HMODULE plugin_mod = nullptr;
  const sbox_plugin_api* plugin = nullptr;
  std::wstring plugin_name;
  v8host::HeldFile plugin_file;
#if !defined(SBOX_DEV_ALLOW_UNSIGNED)
  std::vector<v8host::HeldFile> engine_files;
#endif
  sbox_config_s cfg;          // collected by configure()
  SboxConfigMarshal marshal;  // the SboxPolicy + the storage its pointers alias

  PreparedSandbox() = default;
  // marshal.policy aliases cfg + marshal, so a copy/move would leave it dangling.
  PreparedSandbox(const PreparedSandbox&) = delete;
  PreparedSandbox& operator=(const PreparedSandbox&) = delete;
  ~PreparedSandbox() {
    if (plugin_mod)
      ::FreeLibrary(plugin_mod);
  }
};

// Resolve+verify the plugin, run configure() to collect the policy, verify every
// declared engine DLL, and build the SboxPolicy. Returns false (fail closed) on
// any error.
bool PrepareSandbox(const std::wstring& plugin_name,
                    const void* configure_data,
                    size_t configure_data_size,
                    PreparedSandbox& out) {
  if (!LoadPlugin(plugin_name, /*verify=*/true, "plugin", &out.plugin_mod,
                  &out.plugin, &out.plugin_file))
    return false;
  out.plugin_name = plugin_name;

  // BROKER role: the plugin describes the sandbox it needs.
  sbox_config_api api = MakeConfigApi();
  const sbox_status cs = out.plugin->configure(
      &out.cfg, &api, configure_data, configure_data_size);
  if (cs != sbox_status_ok) {
    printf("[sbox] plugin.configure failed: status=%d\n", cs);
    return false;
  }

  // Verify each engine DLL the worker will LoadLibrary pre-lockdown.
  const std::wstring dir = sbox_harden::ExeDir();
  for (const std::wstring& dll : out.cfg.engine_dlls) {
#if !defined(SBOX_DEV_ALLOW_UNSIGNED)
    v8host::HeldFile engine;
    DWORD error = ERROR_SUCCESS;
    if (!IsBareFilename(dll) ||
        !v8host::OpenImmutableFile(dir + dll, &engine, &error) ||
        !v8host::IsPayloadFileAllowed(
            v8host::ParentPath(v8host::ParentPath(out.plugin_file.final_path())), engine) ||
        !v8host::TrustAllowed(v8host::VerifyTrust(engine))) {
      printf("[sbox] engine DLL verification failed: %ls\n", dll.c_str());
      return false;
    }
    out.engine_files.push_back(std::move(engine));
#else
    if (!IsBareFilename(dll) || !sbox_harden::CheckTrust(dir + dll, "engine")) {
      printf("[sbox] engine DLL verification failed: %ls\n", dll.c_str());
      return false;
    }
#endif
  }

  // Marshal the collected config into an SboxPolicy. THE DISCARD GATE: if any
  // setter poisoned the builder, or a cross-setter rule fails, this returns false
  // and we fail closed here — before sbox_broker_spawn, so NO worker is resumed
  // and no partial policy is applied.
  if (!MarshalSboxConfig(out.cfg, out.plugin_name.c_str(), out.marshal)) {
    printf("[sbox] plugin.configure produced an invalid policy; no spawn\n");
    return false;
  }
  return true;
}

// Worker persona: sbox.exe owns the lifecycle and calls into the plugin at the
// ACG-correct points (warmup PRE-lockdown, run POST-lockdown). This is the
// control inversion from a payload that owns main().
int RunWorker(const std::wstring& plugin_name) {
  g_worker_role = true;
  printf("[sbox] role=worker pid=%lu\n", ::GetCurrentProcessId());
  sbox_harden::HardenDllSearch();

  SboxTarget* target = sbox_target_begin();
  if (!target) {
    printf("[sbox] worker: sbox_target_begin failed\n");
    return kBeginFailed;
  }
  const bool ping_pre = sbox_target_test_ipc(target) != 0;
  printf("[sbox] worker: IPC pre-lockdown = %s\n", ping_pre ? "OK" : "FAIL");

  // Load the broker-verified plugin by FULL PATH from our own app dir (PRE-
  // lockdown: the lowered token can no longer open it) and resolve its vtable.
  // The broker already Authenticode-verified it, so the worker just re-resolves.
  HMODULE plugin_mod = nullptr;
  const sbox_plugin_api* plugin = nullptr;
  if (!LoadPlugin(plugin_name, /*verify=*/false, "worker-plugin", &plugin_mod,
                  &plugin, nullptr)) {
    sbox_target_end(target);
    return kDllResolveFailed;
  }

  sbox_worker_s wk;
  wk.target = target;
  const sbox_worker_api wapi = MakeWorkerApi();

  // warmup (PRE-lockdown) -> lower_token -> run (POST-lockdown) -> shutdown.
  const sbox_status warmup_rc = plugin->warmup(&wk, &wapi);
  printf("[sbox] worker: plugin.warmup -> %d\n", warmup_rc);
  if (warmup_rc != sbox_status_ok) {
    plugin->shutdown(&wk);
    sbox_target_end(target);
    return kWarmupFailed;
  }
  // Readiness posts must succeed before advancing the lifecycle.
  unsigned lower_calls = 0;
  auto finish = [&](int result) {
    plugin->shutdown(&wk);
    sbox_target_end(target);
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
    if (g_sbox_lifecycle_test_failure)
      printf("[sbox-test] failure=%u lower=%u run=0 shutdown=1 end=1\n",
             g_sbox_lifecycle_test_failure,
             lower_calls);
#endif
    return result;
  };
  auto post_lifecycle = [&](uint32_t phase) {
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
    if (g_sbox_lifecycle_test_failure == phase)
      return -2;
#endif
    return sbox_target_post_lifecycle(target, phase);
  };
  if (post_lifecycle(sbox_lifecycle_phase_startup) != 0)
    return finish(kWarmupFailed);

  ++lower_calls;
  const int lowered = sbox_target_lower_token(target);
  if (lowered != 0) {
    printf("[sbox] worker: sbox_target_lower_token failed (%d)\n", lowered);
    return finish(kLowerTokenFailed);
  }
  printf("[sbox] worker: LowerToken survived\n");

  bool ping_post = sbox_target_test_ipc(target) != 0;
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
  if (g_sbox_lifecycle_test_failure == 3)
    ping_post = false;
#endif
  printf("[sbox] worker: IPC post-lockdown = %s\n", ping_post ? "OK" : "FAIL");

  // SECURITY_READY provenance: the container observed the lowered token (above)
  // and a working post-lockdown IPC channel. Emit before handing the guest the
  // runtime so the coordinator can gate the first run on observed lockdown.
  if (!ping_post)
    return finish(kPingPostFailed);
  if (post_lifecycle(sbox_lifecycle_phase_security) != 0)
    return finish(kRunFailed);

  const sbox_status run_rc = plugin->run(&wk, &wapi);
  printf("[sbox] worker: plugin.run -> %d\n", run_rc);

  plugin->shutdown(&wk);
  sbox_target_end(target);

  if (!ping_pre)
    return kPingPreFailed;
  if (run_rc != sbox_status_ok)
    return kRunFailed;
  return kOk;
}

// --- broker ---

struct BrokerMsgCtx {
  HANDLE reply_event = nullptr;
  int replies = 0;
  bool unexpected = false;
  std::string last_reply;  // worker reply text, forwarded to the pipe client
  int lifecycle_markers = 0;  // container readiness markers observed (expect 2)
};

void OnBrokerReply(void* ctx, int kind, const void* data, size_t len) {
  auto* c = static_cast<BrokerMsgCtx*>(ctx);
  // Count only readiness; lifecycle facts must not wake a reply wait.
  if (kind == SBOX_MSG_LIFECYCLE) {
    if (len == 4 && data) {
      const auto* bytes = static_cast<const uint8_t*>(data);
      const uint32_t phase = bytes[0] | (uint32_t(bytes[1]) << 8) |
          (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
      if (phase == sbox_lifecycle_phase_startup || phase == sbox_lifecycle_phase_security)
        ++c->lifecycle_markers;
    }
    return;
  }
  if (kind == SBOX_MSG_STRING) {
    const std::string text(static_cast<const char*>(data), len);
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
    printf("[sbox] broker: worker reply = \"%s\"\n", text.c_str());
#endif
    // The worker runs real guest JS under lockdown; the built-in demo guest
    // echoes each request as "js echo: <request>" via host.postMessage.
    if (text.rfind("js echo: ", 0) == 0) {
      ++c->replies;
      c->last_reply = text;
    } else {
      c->unexpected = true;
    }
  } else {
    c->unexpected = true;
  }
  if (c->reply_event) ::SetEvent(c->reply_event);
}

// High-resolution timer for the latency measurement.
struct Perf {
  LARGE_INTEGER freq;
  Perf() { ::QueryPerformanceFrequency(&freq); }
  LONGLONG now() const {
    LARGE_INTEGER t;
    ::QueryPerformanceCounter(&t);
    return t.QuadPart;
  }
  double ms(LONGLONG a, LONGLONG b) const {
    return static_cast<double>(b - a) * 1000.0 / freq.QuadPart;
  }
};

// One timed request->reply cycle against the running worker. Returns ms, or <0.
double RoundTrip(SboxSession* session, BrokerMsgCtx* mctx, const Perf& perf,
                 const char* msg, size_t len) {
  ::ResetEvent(mctx->reply_event);
  mctx->replies = 0;
  const LONGLONG t0 = perf.now();
  if (sbox_broker_post_message(session, SBOX_MSG_STRING, msg, len) != 0)
    return -1.0;
  if (::WaitForSingleObject(mctx->reply_event, 15000) != WAIT_OBJECT_0 ||
      mctx->replies == 0)
    return -1.0;
  return perf.ms(t0, perf.now());
}

int RunBroker(const std::wstring& plugin_name) {
  printf("[sbox] role=broker pid=%lu\n", ::GetCurrentProcessId());
  sbox_harden::HardenDllSearch();

  const std::wstring self = SelfPath();
  // The plugin (not sbox.exe) supplies the sandbox policy: load+verify it, run
  // configure(), and apply what it set. This keeps the container app-agnostic.
  PreparedSandbox sb;
  if (!PrepareSandbox(plugin_name, nullptr, 0, sb)) {
    printf("[sbox] broker: sandbox preparation failed\n");
    return 3;
  }

  BrokerMsgCtx mctx;
  mctx.reply_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

  // Time the one-time spawn (process creation + sandbox lockdown setup to
  // resume). Spawns THIS image with --worker --plugin=<name> (appended inside
  // sbox_broker.cc from the policy's passthrough fields).
  Perf perf;
  const LONGLONG t_spawn0 = perf.now();
  SboxSession* session =
      sbox_broker_spawn(self.c_str(), &sb.marshal.policy, &OnBrokerReply, &mctx);
  const LONGLONG t_spawn1 = perf.now();
  if (!session) {
    printf("[sbox] broker: spawn failed\n");
    ::CloseHandle(mctx.reply_event);
    return 2;
  }

  const char msg[] = "ping from broker";
  // First (cold) round-trip also covers the worker's warmup + LowerToken +
  // reaching its run loop; the warm ones are steady-state broker<->worker RTT.
  const double first_rtt = RoundTrip(session, &mctx, perf, msg, sizeof(msg) - 1);
  const bool got_reply = first_rtt >= 0 && !mctx.unexpected;
  double warm_sum = 0;
  double warm_min = 0;
  int warm_n = 0;
  bool warm_ok = got_reply;
  for (int i = 0; i < 20 && warm_ok; ++i) {
    const double rtt = RoundTrip(session, &mctx, perf, msg, sizeof(msg) - 1);
    if (rtt < 0 || mctx.unexpected) {
      warm_ok = false;
      break;
    }
    warm_sum += rtt;
    if (warm_n == 0 || rtt < warm_min) warm_min = rtt;
    ++warm_n;
  }

  const int rc_close = sbox_broker_close(session);
  const int worker_exit = sbox_broker_wait(session);
  ::CloseHandle(mctx.reply_event);

  const double spawn_ms = perf.ms(t_spawn0, t_spawn1);
  const double warm_avg = warm_n ? warm_sum / warm_n : -1.0;
  printf("[sbox] spawn+lockdown=%.2f ms  first-roundtrip(cold)=%.3f ms"
         "  warm-roundtrip avg=%.3f min=%.3f ms (n=%d)\n",
         spawn_ms, first_rtt, warm_avg, warm_min, warm_n);

  // Positive readiness-emission evidence: the container posts exactly two markers
  // (STARTUP then SECURITY) per worker before the guest runs (design §9).
  const bool ok = got_reply && warm_ok && !mctx.unexpected && rc_close == 0 &&
                  worker_exit == kOk && mctx.lifecycle_markers == 2;
  printf("[sbox] broker: worker_exit=%d warm_rtts=%d lifecycle=%d -> %s\n",
         worker_exit, warm_n, mctx.lifecycle_markers, ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

// Named-pipe round-trip latency — models the new app->broker hop (the transport
// that used to be in-process on x64). Single process: a server thread echoes;
// the main thread times N round-trips. Isolates the pipe transport cost
// (cross-process scheduling is already in the warm broker<->worker number). No
// protocol, no auth — just RTT.
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
DWORD WINAPI PipeServerThread(void* arg) {
  const wchar_t* name = static_cast<const wchar_t*>(arg);
  HANDLE pipe = ::CreateNamedPipeW(
      name, PIPE_ACCESS_DUPLEX,
      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0,
      nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return 1;
  if (!::ConnectNamedPipe(pipe, nullptr) &&
      ::GetLastError() != ERROR_PIPE_CONNECTED) {
    ::CloseHandle(pipe);
    return 2;
  }
  char buf[256];
  DWORD n = 0;
  while (::ReadFile(pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
    DWORD wrote = 0;
    if (!::WriteFile(pipe, buf, n, &wrote, nullptr)) break;
    if (n == 4 && std::memcmp(buf, "quit", 4) == 0) break;
  }
  ::FlushFileBuffers(pipe);
  ::DisconnectNamedPipe(pipe);
  ::CloseHandle(pipe);
  return 0;
}

int RunPipeBench() {
  printf("[sbox] role=pipe-bench pid=%lu\n", ::GetCurrentProcessId());
  const wchar_t* name = L"\\\\.\\pipe\\sbox_bench";
  HANDLE th = ::CreateThread(nullptr, 0, &PipeServerThread,
                             const_cast<wchar_t*>(name), 0, nullptr);
  if (!th) return 2;
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (int i = 0; i < 200 && pipe == INVALID_HANDLE_VALUE; ++i) {
    pipe = ::CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                         OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) ::Sleep(1);
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    ::WaitForSingleObject(th, 2000);
    ::CloseHandle(th);
    return 3;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  ::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

  Perf perf;
  const char msg[] = "ping";
  char buf[256];
  DWORD w = 0, r = 0;
  ::WriteFile(pipe, msg, sizeof(msg) - 1, &w, nullptr);  // warm the pipe once
  ::ReadFile(pipe, buf, sizeof(buf), &r, nullptr);
  double sum = 0, mn = 0;
  int n = 0;
  for (int i = 0; i < 1000; ++i) {
    const LONGLONG t0 = perf.now();
    if (!::WriteFile(pipe, msg, sizeof(msg) - 1, &w, nullptr)) break;
    if (!::ReadFile(pipe, buf, sizeof(buf), &r, nullptr)) break;
    const double rtt = perf.ms(t0, perf.now());
    sum += rtt;
    if (n == 0 || rtt < mn) mn = rtt;
    ++n;
  }
  ::WriteFile(pipe, "quit", 4, &w, nullptr);
  ::ReadFile(pipe, buf, sizeof(buf), &r, nullptr);
  ::CloseHandle(pipe);
  ::WaitForSingleObject(th, 2000);
  ::CloseHandle(th);
  printf("[sbox] named-pipe RTT avg=%.4f min=%.4f ms (n=%d)\n",
         n ? sum / n : -1.0, mn, n);
  return n > 0 ? 0 : 1;
}

#endif

// ===== generic plugin coordinator adapter =====

void OnBrokerWorkerMessage(void* context, int kind, const void* data, size_t len) {
  auto* worker = static_cast<sbox_broker_worker_s*>(context);
  if (worker && worker->callback)
    worker->callback(worker->callback_context,
                     static_cast<sbox_msg_kind>(kind), data, len);
}

sbox_status SBOX_CDECL BrokerConfigureAndSpawn(
    sbox_broker broker,
    const void* configure_data,
    size_t configure_data_size,
    sbox_message_cb on_message,
    void* user_data,
    sbox_broker_worker* out_worker) {
  if (!broker || !out_worker || (configure_data_size && !configure_data) ||
      configure_data_size > 64 * 1024) {
    return sbox_status_invalid_arg;
  }
  *out_worker = nullptr;
  std::vector<uint8_t> copied;
  if (configure_data_size) {
    const auto* begin = static_cast<const uint8_t*>(configure_data);
    copied.assign(begin, begin + configure_data_size);
  }
  {
    std::lock_guard<std::mutex> lock(broker->mutex);
    if (broker->stopping)
      return sbox_status_failed;
  }
  PreparedSandbox sandbox;
  if (!PrepareSandbox(broker->plugin_name,
                      copied.empty() ? nullptr : copied.data(), copied.size(),
                      sandbox))
    return sbox_status_failed;
  auto worker = std::make_unique<sbox_broker_worker_s>();
  worker->broker = broker;
  worker->callback = on_message;
  worker->callback_context = user_data;
  worker->session =
      sbox_broker_spawn(broker->self_path.c_str(), &sandbox.marshal.policy,
                        &OnBrokerWorkerMessage, worker.get());
  if (!worker->session)
    return sbox_status_failed;
  {
    std::lock_guard<std::mutex> lock(broker->mutex);
    if (broker->stopping) {
      sbox_broker_close(worker->session);
      sbox_broker_wait(worker->session);
      return sbox_status_failed;
    }
    broker->workers.insert(worker.get());
  }
  *out_worker = worker.release();
  return sbox_status_ok;
}

sbox_status SBOX_CDECL BrokerPostMessage(sbox_broker_worker worker,
                                        sbox_msg_kind kind,
                                        const void* data,
                                        size_t size) {
  if (!worker || !worker->session || (size && !data))
    return sbox_status_invalid_arg;
  return sbox_broker_post_message(worker->session, static_cast<int>(kind), data,
                                  size) == 0
             ? sbox_status_ok
             : sbox_status_failed;
}

sbox_status SBOX_CDECL BrokerClose(sbox_broker_worker worker) {
  if (!worker || !worker->session)
    return sbox_status_invalid_arg;
  if (worker->close_called)
    return sbox_status_ok;
  worker->close_called = true;
  return sbox_broker_close(worker->session) == 0 ? sbox_status_ok : sbox_status_failed;
}

sbox_status SBOX_CDECL BrokerWait(sbox_broker_worker worker,
                                 int32_t* out_exit_code) {
  if (!worker || !worker->session || worker->wait_called)
    return sbox_status_invalid_arg;
  worker->wait_called = true;
  sbox_broker_s* broker = worker->broker;
  const int exit_code = sbox_broker_wait(worker->session);
  {
    std::lock_guard<std::mutex> lock(broker->mutex);
    broker->workers.erase(worker);
  }
  if (out_exit_code)
    *out_exit_code = exit_code;
  delete worker;
  return sbox_status_ok;
}

sbox_status CopyBrokerText(const std::wstring& text,
                           sbox_char16* buffer,
                           size_t buffer_length,
                           size_t* out_length) {
  if (!out_length || (!buffer && buffer_length != 0))
    return sbox_status_invalid_arg;
  *out_length = text.size();
  if (!buffer)
    return sbox_status_ok;
  if (buffer_length <= text.size())
    return sbox_status_invalid_arg;
  std::memcpy(buffer, text.c_str(), (text.size() + 1) * sizeof(sbox_char16));
  return sbox_status_ok;
}

sbox_status SBOX_CDECL BrokerGetEndpointName(sbox_broker broker,
                                             sbox_char16* buffer,
                                             size_t buffer_length,
                                             size_t* out_length) {
  if (!broker)
    return sbox_status_invalid_arg;
  return CopyBrokerText(broker->endpoint_name, buffer, buffer_length,
                        out_length);
}

sbox_status SBOX_CDECL BrokerGetContainerFinalPath(sbox_broker broker,
                                                   sbox_char16* buffer,
                                                   size_t buffer_length,
                                                   size_t* out_length) {
  if (!broker)
    return sbox_status_invalid_arg;
  return CopyBrokerText(broker->container_final_path, buffer, buffer_length,
                        out_length);
}

sbox_status SBOX_CDECL BrokerGetPluginFinalPath(sbox_broker broker,
                                                sbox_char16* buffer,
                                                size_t buffer_length,
                                                size_t* out_length) {
  if (!broker)
    return sbox_status_invalid_arg;
  return CopyBrokerText(broker->plugin_final_path, buffer, buffer_length,
                        out_length);
}

sbox_status SBOX_CDECL BrokerGetProcessId(sbox_broker_worker worker,
                                          uint32_t* out_process_id) {
  if (!out_process_id)
    return sbox_status_invalid_arg;
  *out_process_id = 0;
  if (!worker || !worker->session)
    return sbox_status_invalid_arg;
  *out_process_id = sbox_broker_get_process_id(worker->session);
  return *out_process_id ? sbox_status_ok : sbox_status_failed;
}

sbox_broker_api MakeBrokerApi() {
  sbox_broker_api api = SBOX_BROKER_API_INIT;
  api.configure_and_spawn = &BrokerConfigureAndSpawn;
  api.post_message = &BrokerPostMessage;
  api.close = &BrokerClose;
  api.wait = &BrokerWait;
  api.get_endpoint_name = &BrokerGetEndpointName;
  api.get_container_final_path = &BrokerGetContainerFinalPath;
  api.get_plugin_final_path = &BrokerGetPluginFinalPath;
  api.get_process_id = &BrokerGetProcessId;
  return api;
}

int RunBrokerPlugin(int argc,
                    char** argv,
                    const std::wstring& plugin_name) {
  sbox_broker_mode mode = sbox_broker_mode_shared;
  std::wstring endpoint;
  std::array<uint8_t, 16> nonce = {};
  if (!ParseBrokerMode(argc, argv, &mode) ||
      !ParsePipeName(argc, argv, &endpoint) ||
      (mode == sbox_broker_mode_dedicated &&
       !ParseNonce(argc, argv, &nonce))) {
    printf("[sbox] broker: invalid coordinator arguments\n");
    return 2;
  }
  sbox_harden::HardenDllSearch();
  sbox_broker_s broker;
  broker.self_path = SelfPath();
  broker.plugin_name = plugin_name;
  v8host::HeldFile container_file;
  v8host::HeldFile plugin_file;
  DWORD error = ERROR_SUCCESS;
  if (!v8host::OpenImmutableFile(broker.self_path, &container_file, &error) ||
      !LoadPlugin(plugin_name, true, "broker-plugin", &broker.plugin_module,
                  &broker.plugin, &plugin_file)) {
    printf("[sbox] broker: immutable payload verification failed\n");
    return 3;
  }
  broker.endpoint_name = endpoint;
  broker.container_final_path = container_file.final_path();
  broker.plugin_final_path = plugin_file.final_path();
  sbox_broker_start start = SBOX_BROKER_START_INIT;
  start.mode = mode;
  start.native_machine = container_file.machine();
  start.container_sha256 = container_file.sha256().data();
  start.plugin_sha256 = plugin_file.sha256().data();
  start.container_file_token = &container_file;
  start.plugin_file_token = &plugin_file;
  if (mode == sbox_broker_mode_dedicated) {
    start.dedicated_nonce = nonce.data();
    start.dedicated_nonce_size = nonce.size();
  }
  const sbox_broker_api api = MakeBrokerApi();
  const sbox_status status = broker.plugin->broker_run(&broker, &api, &start);
  std::vector<sbox_broker_worker_s*> leaked;
  {
    std::lock_guard<std::mutex> lock(broker.mutex);
    broker.stopping = true;
    leaked.assign(broker.workers.begin(), broker.workers.end());
  }
  for (sbox_broker_worker_s* worker : leaked) {
    BrokerClose(worker);
    BrokerWait(worker, nullptr);
  }
  ::FreeLibrary(broker.plugin_module);
  broker.plugin_module = nullptr;
  if (!leaked.empty()) {
    printf("[sbox] broker: plugin returned with live workers\n");
    return 4;
  }
  return status == sbox_status_ok ? 0 : 1;
}

// Role dispatch. Kept deliberately dumb and FIRST so neither persona can fall
// through into the other's init.
enum class Role {
  kNone,
  kBroker,
  kWorker,
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
  kPipeBench
#endif
};

Role ParseRole(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--worker") == 0) return Role::kWorker;
    if (std::strcmp(argv[i], "--broker") == 0) return Role::kBroker;
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
    if (std::strcmp(argv[i], "--pipe-bench") == 0) return Role::kPipeBench;
#endif
  }
  return Role::kNone;
}

}  // namespace

#if !defined(SBOX_LIFECYCLE_TESTING)
int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const Role role = ParseRole(argc, argv);
  const std::wstring plugin = ParsePluginName(argc, argv);

  // The broker/worker roles are driven by a plugin; require its
  // name up front (the container is app-agnostic and has no built-in default).
  const bool needs_plugin = role == Role::kBroker || role == Role::kWorker;
  if (needs_plugin && plugin.empty()) {
    printf("[sbox] this role requires --plugin <filename>\n");
    return 2;
  }

  switch (role) {
    case Role::kBroker:
      return ParseValue(argc, argv, "--pipe").empty()
                 ? RunBroker(plugin)
                 : RunBrokerPlugin(argc, argv, plugin);
    case Role::kWorker:
      return RunWorker(plugin);
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
    case Role::kPipeBench:
      return RunPipeBench();
#endif
    case Role::kNone:
    default:
      printf("Usage: sbox.exe --broker|--worker --plugin <filename>"
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
             " | --pipe-bench"
#endif
             "\n");
      return 2;
  }
}

#endif

#if defined(SBOX_LIFECYCLE_TESTING)
bool SboxTestSmokeLifecycle() {
  BrokerMsgCtx context;
  context.reply_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!context.reply_event)
    return false;
  for (uint8_t phase : {uint8_t(1), uint8_t(2), uint8_t(3), uint8_t(4)}) {
    const uint8_t bytes[] = {phase,0,0,0};
    OnBrokerReply(&context, SBOX_MSG_LIFECYCLE, bytes, 4);
  }
  const uint8_t bytes[] = {1,0,0,0,0};
  OnBrokerReply(&context, SBOX_MSG_LIFECYCLE, bytes, 3);
  OnBrokerReply(&context, SBOX_MSG_LIFECYCLE, bytes, 5);
  bool ok = context.lifecycle_markers == 2 && context.replies == 0 &&
      ::WaitForSingleObject(context.reply_event, 0) == WAIT_TIMEOUT;
  ::CloseHandle(context.reply_event);
  return ok;
}
bool SboxTestWorkerKindGuard() {
  sbox_worker_s worker;
  worker.target = reinterpret_cast<SboxTarget*>(1);
  const auto api = MakeWorkerApi();
  const uint8_t bytes[] = {1,0,0,0};
  return api.post_message(&worker, sbox_msg_kind_lifecycle, bytes, 4) == sbox_status_invalid_arg &&
      api.post_message(&worker, static_cast<sbox_msg_kind>(99), bytes, 4) == sbox_status_invalid_arg;
}
#endif
