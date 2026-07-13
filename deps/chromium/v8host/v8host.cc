// v8host.exe — an app-defined sandbox TARGET binary. It loads the generic
// sbox.dll for the sandbox target role, and (its "prime use") loads the JSI
// engine DLL to run untrusted JavaScript under lockdown: the jitless sandbox
// engine v8jsisb.dll for the Untrusted tier (default) or the full-JIT v8jsi.dll
// for Trusted (overridable via V8HOST_ENGINE_DLL). All V8
// knowledge lives HERE, not in the sandbox DLL.
//
// g_sbox_bootstrap is EXPORTED so the broker can find it (by parsing this EXE's
// export table) and write the bootstrap values into it while this process is
// still suspended — the EXE image is mapped at creation, unlike sbox.dll.
//
// Stage 2 of the WebView2-style message channel lives here: a `host` JS object
// (host.postMessage / host.postMessageBinary / host.onmessage) bridged to the
// sbox message channel, driven by a long-running event loop that OWNS the JS
// thread.
//
// IMPORTANT: v8host drives v8jsi through the **JSI C++ API** (facebook::jsi via
// JsiAbiRuntime), NOT the raw ABI-safe C interface. The C ABI is the binary-
// stable boundary INSIDE the DLL; C++ consumers use jsi.h + JsiAbiRuntime.h
// (both shipped in the NuGet package), which give RAII handle lifetimes,
// exceptions, and the familiar jsi::Value/Object/Function types. Only the config
// seam (the configure callback used to wire the task runner) stays on the C ABI,
// because that IS the ABI boundary.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sbox.h"
#include "sbox_harden.h"

// JSI C++ API (consumer-compiled, via jsi_cpp) + the v8jsi config header (its
// setters are resolved from v8jsi.dll by name and used inside the configure
// callback — the legitimate C ABI seam).
#include <jsi/jsi.h>

#include "jsi_abi/JsiAbiRuntime.h"
#include "jsi_abi/v8_jsi_config.h"

extern "C" __declspec(dllexport) SboxBootstrap g_sbox_bootstrap = {};

namespace {

using namespace facebook::jsi;  // Runtime, Value, Object, Function, String, ...

// Diagnostics sink. As a sandbox TARGET, this process has no console of its own
// (and a GUI broker can't reliably give it one), so plain stdout is often
// invisible. Mirror every diagnostic line to the debugger output as well, so a
// debugger / DebugView captures it regardless of how the process was launched.
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
  ::OutputDebugStringA(buf);
}
#define printf HostLog

// Resolve a v8jsi.dll export by name (decltype(&name) + GetProcAddress).
#define RESOLVE(mod, name)                                               \
  auto p_##name =                                                        \
      reinterpret_cast<decltype(&name)>(::GetProcAddress((mod), #name)); \
  if (!p_##name) {                                                       \
    printf("[v8host] missing v8jsi export: %s\n", #name);                \
    return 21;                                                           \
  }

void PrintAcgStatus() {
  PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dc = {};
  if (::GetProcessMitigationPolicy(::GetCurrentProcess(),
                                   ProcessDynamicCodePolicy, &dc, sizeof(dc))) {
    printf("[v8host] ACG (ProhibitDynamicCode) = %s\n",
           dc.ProhibitDynamicCode ? "ON" : "off");
  }
}

DWORD TryReadOpen(const std::wstring& path) {
  if (path.empty())
    return ERROR_FILE_NOT_FOUND;
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return ::GetLastError();
  ::CloseHandle(h);
  return ERROR_SUCCESS;
}

// Read an entire file into a string of UTF-8 bytes. Returns false (leaving
// `out` unspecified) if the file can't be opened or fully read. Used to load a
// host-supplied guest script PRE-lockdown — after the token is lowered the
// target can no longer open arbitrary files.
bool ReadFileBytes(const std::wstring& path, std::string& out) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  LARGE_INTEGER size = {};
  bool ok = (::GetFileSizeEx(h, &size) != 0) && size.QuadPart >= 0;
  if (ok) {
    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    size_t total = 0;
    while (total < out.size()) {
      DWORD want =
          static_cast<DWORD>(std::min<size_t>(out.size() - total, 1u << 28));
      DWORD got = 0;
      if (!::ReadFile(h, &out[total], want, &got, nullptr) || got == 0) {
        ok = false;
        break;
      }
      total += got;
    }
  }
  ::CloseHandle(h);
  return ok;
}

std::wstring EnvW(const wchar_t* name) {
  wchar_t buf[MAX_PATH] = {};
  DWORD n = ::GetEnvironmentVariableW(name, buf, MAX_PATH);
  return (n == 0 || n >= MAX_PATH) ? std::wstring() : std::wstring(buf, n);
}

//==========================================================================
// QPC timing split. Capture a timestamp at each startup boundary and print the
// delta from the previous one as `[v8host][perf] <segment> = N.N ms`. These
// lines go through HostLog, so they reach BOTH stdout (captured by the broker)
// and OutputDebugString (DebugView) — no new sink. The split breaks open the
// time between process start and the first JS line that the broker can't see,
// and isolates the snapshot deserialize tax (folded into runtime-create, since
// the blob is deserialized while the isolate/context is built) from the
// parse+execute it removes. Defaulting on for measurements; V8HOST_PERF=0 silences.
//==========================================================================
struct Perf {
  LARGE_INTEGER freq{};
  bool on = true;
  void init() {
    ::QueryPerformanceFrequency(&freq);
    on = EnvW(L"V8HOST_PERF") != L"0";
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

// Perf state shared with the host-object lambdas so the kickoff's first
// outbound host.postMessage can be timestamped. 0 until the guest posts.
Perf g_perf;
LONGLONG g_first_post_qpc = 0;
inline void MarkFirstPost() {
  if (g_first_post_qpc == 0)
    g_first_post_qpc = g_perf.now();
}

//==========================================================================
// Foreground task runner (wired into the JSI config via the C ABI seam).
//
// V8 may post foreground tasks (from background workers, the inspector, etc.).
// We own the JS thread, so the task runner just enqueues onto a thread-safe
// queue and wakes the loop; the loop runs them on the JS thread. The CTask
// shape mirrors the public V8TaskRunner/CTask consumer in V8JsiRuntime.cpp.
//==========================================================================

struct CTask {
  void* data;
  v8_jsi_task_run_cb run;
  jsi_data_delete_cb del;
  void* deleter_data;
};

struct TaskQueue {
  CRITICAL_SECTION cs;
  std::vector<CTask> tasks;
  HANDLE wake = nullptr;  // auto-reset; signaled when a task is posted
};

// Pointer (not an object) so there is no exit-time destructor; the TaskQueue
// itself lives on main's stack for the whole runtime lifetime.
TaskQueue* g_tasks = nullptr;

// Called by the DLL (possibly on a background thread) to post a foreground task.
void JSI_CDECL PostTaskCb(void* runner_data, void* task_data,
                          v8_jsi_task_run_cb task_run_cb,
                          jsi_data_delete_cb task_data_delete_cb,
                          void* deleter_data) {
  auto* q = static_cast<TaskQueue*>(runner_data);
  ::EnterCriticalSection(&q->cs);
  q->tasks.push_back({task_data, task_run_cb, task_data_delete_cb, deleter_data});
  ::LeaveCriticalSection(&q->cs);
  ::SetEvent(q->wake);
}

// Called when the runtime is destroyed. runner_data points at g_tasks (process
// lifetime) so we don't free it — but fire the delete cb of any task still
// queued so nothing leaks.
void JSI_CDECL TaskRunnerDeleteCb(void* runner_data, void* /*deleter_data*/) {
  auto* q = static_cast<TaskQueue*>(runner_data);
  ::EnterCriticalSection(&q->cs);
  for (auto& t : q->tasks)
    if (t.del)
      t.del(t.data, t.deleter_data);
  q->tasks.clear();
  ::LeaveCriticalSection(&q->cs);
}

// Run (and free) all queued foreground tasks on the JS thread.
void RunQueuedTasks(TaskQueue* q) {
  std::vector<CTask> local;
  ::EnterCriticalSection(&q->cs);
  local.swap(q->tasks);
  ::LeaveCriticalSection(&q->cs);
  for (auto& t : local) {
    if (t.run)
      t.run(t.data);
    if (t.del)
      t.del(t.data, t.deleter_data);
  }
}

// Resolved v8jsi config setters (called inside the configure callback, which
// runs synchronously during v8_create_runtime). Set before create.
decltype(&v8_jsi_config_set_task_runner) g_set_task_runner = nullptr;
decltype(&v8_jsi_config_set_explicit_microtask_policy) g_set_microtask = nullptr;
decltype(&v8_jsi_config_set_startup_snapshot) g_set_snapshot = nullptr;

// Optional startup-snapshot blob (V8HOST_SNAPSHOT), read pre-lockdown. Held for
// the process lifetime so the bytes outlive the runtime; passed to the config
// with a NULL deleter (v8host owns them, not the runtime).
std::string g_snapshot_blob;
bool g_have_snapshot = false;

jsi_error_code JSI_CDECL ConfigureRuntime(void* /*cb_data*/, jsi_config cfg) {
  if (g_set_task_runner)
    g_set_task_runner(cfg, g_tasks, &PostTaskCb, &TaskRunnerDeleteCb, nullptr);
  // Explicit microtask policy: promise jobs run only when we drain them on the
  // JS thread (after each loop turn), giving the host deterministic control.
  if (g_set_microtask)
    g_set_microtask(cfg, true);
  // If a startup snapshot was supplied, hand the engine the blob so the isolate
  // is created from it — the embedded [shim + bundle] heap is pre-materialized,
  // and only the kickoff script is evaluated below. NULL deleter: g_snapshot_blob
  // (process-lifetime) owns the bytes.
  if (g_have_snapshot && g_set_snapshot)
    g_set_snapshot(cfg, reinterpret_cast<const uint8_t*>(g_snapshot_blob.data()),
                   g_snapshot_blob.size(), nullptr, nullptr);
  return jsi_no_error;
}

//==========================================================================
// The `host` JS object and the inbound-message bridge (JSI C++ API).
//==========================================================================

// Install host.postMessage(string) / host.postMessageBinary(ArrayBuffer) and
// attach `host` to the global. The capturing lambdas hold the target handle;
// RAII manages every jsi pointer's lifetime — nothing leaks.
void InstallHostObject(Runtime& rt, SboxTarget* target) {
  Object host(rt);

  host.setProperty(
      rt, "postMessage",
      Function::createFromHostFunction(
          rt, PropNameID::forAscii(rt, "postMessage"), 1,
          [target](Runtime& rt, const Value&, const Value* args,
                   size_t count) -> Value {
            MarkFirstPost();
            if (count >= 1 && args[0].isString()) {
              std::string s = args[0].getString(rt).utf8(rt);
              sbox_target_post_message(target, SBOX_MSG_STRING, s.data(),
                                       s.size());
            }
            return Value::undefined();
          }));

  host.setProperty(
      rt, "postMessageBinary",
      Function::createFromHostFunction(
          rt, PropNameID::forAscii(rt, "postMessageBinary"), 1,
          [target](Runtime& rt, const Value&, const Value* args,
                   size_t count) -> Value {
            MarkFirstPost();
            if (count >= 1 && args[0].isObject()) {
              Object o = args[0].getObject(rt);
              if (o.isArrayBuffer(rt)) {
                ArrayBuffer ab = o.getArrayBuffer(rt);
                sbox_target_post_message(target, SBOX_MSG_BINARY, ab.data(rt),
                                         ab.size(rt));
              }
            }
            return Value::undefined();
          }));

  rt.global().setProperty(rt, "host", host);
}

// Deliver one inbound frame to JS host.onmessage (string -> JS string,
// binary -> JS ArrayBuffer). Runs on the JS thread (the loop IS the JS thread).
void DeliverToJs(Runtime& rt, int kind, const void* data, size_t len) {
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

  if (kind == SBOX_MSG_STRING) {
    cb.call(rt, String::createFromUtf8(rt, static_cast<const uint8_t*>(data),
                                       len));
  } else {
    // Allocate the ArrayBuffer IN-CAGE via JS `new ArrayBuffer(n)` and copy the
    // frame in. Do NOT hand V8 an external backing store (jsi ArrayBuffer over a
    // MutableBuffer): when the V8 in-process sandbox is enabled, external backing
    // stores are a FATAL error ("backing stores must be allocated inside the
    // sandbox"). A JS-allocated buffer uses V8's in-cage allocator and works in
    // both sandbox and non-sandbox builds.
    Function ab_ctor = rt.global().getPropertyAsFunction(rt, "ArrayBuffer");
    ArrayBuffer ab = ab_ctor.callAsConstructor(rt, Value(static_cast<double>(len)))
                         .getObject(rt)
                         .getArrayBuffer(rt);
    if (len)
      std::memcpy(ab.data(rt), data, len);
    cb.call(rt, std::move(ab));
  }
}

// Drain context: the runtime + counts of what JS received (for the scorecard).
struct DrainCtx {
  Runtime* rt;
  int strings = 0;
  int binaries = 0;
};

// Invoked by sbox_target_drain_messages (C code) — exceptions must NOT cross
// back into C, so catch everything here.
void OnInbound(void* ctx, int kind, const void* data, size_t len) {
  auto* c = static_cast<DrainCtx*>(ctx);
  if (kind == SBOX_MSG_STRING)
    ++c->strings;
  else
    ++c->binaries;
  try {
    DeliverToJs(*c->rt, kind, data, len);
  } catch (const std::exception& e) {
    printf("[v8host] onmessage threw: %s\n", e.what());
  } catch (...) {
    printf("[v8host] onmessage threw (unknown)\n");
  }
}

// User (untrusted) JS: wire onmessage to echo, then announce readiness. The
// binary handler tags byte 0 to prove JS actually read/wrote the ArrayBuffer.
const char* kUserJs =
    // Hot loop so the Trusted tier actually exercises the JIT (harmless under
    // jitless — runs interpreted). Also confirms compute works under lockdown.
    "(function(){var s=0;for(var i=0;i<3000000;i++){s+=i;}return s;})();"
    "host.onmessage = function(m){"
    "  if (typeof m === 'string') { host.postMessage('js echo: ' + m); }"
    "  else { var a = new Uint8Array(m); a[0] = 0x42; host.postMessageBinary(m); }"
    "};"
    "host.postMessage('ready');";

}  // namespace

int main() {
  g_perf.init();
  const LONGLONG t_entry = g_perf.now();
  setvbuf(stdout, nullptr, _IONBF, 0);

  TaskQueue task_queue;
  g_tasks = &task_queue;
  ::InitializeCriticalSection(&task_queue.cs);
  task_queue.wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset

  // Hardening: restrict the loader search path. We do NOT call WinVerifyTrust
  // here — the sandboxed target (restricted token + low integrity, even
  // pre-LowerToken) cannot reliably reach the crypto/catalog services it needs,
  // and the target is untrusted anyway. Signature verification of sbox.dll,
  // v8host.exe, and v8jsi.dll is the BROKER's job (it runs unrestricted and does
  // it before spawning us — see test_app.cc).
  sbox_harden::HardenDllSearch();

  SboxTarget* target = sbox_target_begin(&g_sbox_bootstrap);
  if (!target) {
    printf("[v8host] sbox_target_begin failed\n");
    return 10;
  }

  printf("[v8host] IPC test (pre-lockdown)  = %s\n",
         sbox_target_test_ipc(target) ? "OK" : "FAIL");
  const bool ping_pre = sbox_target_test_ipc(target) != 0;

  // Tier selection (host-chosen; the broker sets SBOX_TIER, inherited here):
  //   Untrusted (default) = jitless + ACG (V8 emits no executable code).
  //   Trusted            = JIT allowed; the broker also leaves ACG off.
  const bool trusted_tier = EnvW(L"SBOX_TIER") == L"trusted";
  printf("[v8host] tier = %s\n",
         trusted_tier ? "Trusted (JIT, ACG off)" : "Untrusted (jitless + ACG)");

  // --- warmup: load the guest engine + create the V8 runtime (BEFORE LowerToken) ---
  // Engine selection: the Untrusted tier runs the jitless
  // sandbox engine v8jsisb.dll (the product default); the Trusted tier runs the
  // full-JIT v8jsi.dll. A broker may override the filename via V8HOST_ENGINE_DLL
  // (the engines share the JSI ABI, so the host just LoadLibrary's the chosen
  // one by name — no link-time bind, no new consumer header). If the tier-derived
  // default is absent (e.g. a minimal harness that staged only v8jsi.dll), fall
  // back to v8jsi.dll so a single-engine layout still runs.
  std::wstring engine_dll = EnvW(L"V8HOST_ENGINE_DLL");
  const bool engine_overridden = !engine_dll.empty();
  if (engine_dll.empty())
    engine_dll = trusted_tier ? L"v8jsi.dll" : L"v8jsisb.dll";
  // Host-supplied startup snapshot: if V8HOST_SNAPSHOT names a readable file,
  // read the blob NOW (pre-lockdown) and create the runtime from it below, so
  // the embedded [env shim + bundle] heap is pre-materialized instead of parsed
  // and executed. A set-but-unreadable path is a warning, not fatal (falls back
  // to a normal runtime). The blob is held for the process lifetime.
  const std::wstring snapshot_path = EnvW(L"V8HOST_SNAPSHOT");
  if (!snapshot_path.empty()) {
    if (ReadFileBytes(snapshot_path, g_snapshot_blob)) {
      g_have_snapshot = true;
      printf("[v8host] startup snapshot from %ls (%zu bytes)\n",
             snapshot_path.c_str(), g_snapshot_blob.size());
    } else {
      printf("[v8host] WARNING: V8HOST_SNAPSHOT=%ls could not be read; "
             "creating a normal runtime\n", snapshot_path.c_str());
    }
  }

  // Host-supplied guest script: if V8HOST_GUEST_JS names a readable file,
  // evaluate its contents as the guest instead of the built-in kUserJs demo.
  // Read it NOW (pre-lockdown) — once LowerToken() drops privileges the target
  // can no longer open arbitrary files. v8host stays a neutral JS host: it
  // installs the `host` object and evaluates whatever guest it is handed; the
  // broker composes whatever the guest contains. A set-but-unreadable path is a
  // warning, not a fatal error, so the demo guest keeps the binary self-testable.
  std::string guest_js;
  bool use_guest_file = false;
  const std::wstring guest_js_path = EnvW(L"V8HOST_GUEST_JS");
  if (!guest_js_path.empty()) {
    if (ReadFileBytes(guest_js_path, guest_js)) {
      use_guest_file = true;
      printf("[v8host] guest JS from %ls (%zu bytes)\n", guest_js_path.c_str(),
             guest_js.size());
    } else {
      printf("[v8host] WARNING: V8HOST_GUEST_JS=%ls could not be read; "
             "falling back to built-in demo guest\n",
             guest_js_path.c_str());
    }
  }

  // Load the engine by FULL PATH (our own application directory) — never a bare
  // name (which would honor the search path / allow planting). The broker
  // already verified this file's signature before spawning us.
  auto load_engine = [](const std::wstring& name) -> HMODULE {
    const std::wstring p = sbox_harden::ExeDir() + name;
    return ::LoadLibraryExW(p.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
  };
  HMODULE guest = load_engine(engine_dll);
  if (!guest && !engine_overridden && engine_dll != L"v8jsi.dll") {
    printf("[v8host] %ls not found, falling back to v8jsi.dll\n",
           engine_dll.c_str());
    engine_dll = L"v8jsi.dll";
    guest = load_engine(engine_dll);
  }
  if (!guest) {
    printf("[v8host] LoadLibrary(%ls) failed: %lu\n", engine_dll.c_str(),
           ::GetLastError());
    return 20;
  }
  const LONGLONG t_engine_loaded = g_perf.now();
  g_perf.emit("engine-load", t_entry, t_engine_loaded);
  printf("[v8host] engine = %ls\n", engine_dll.c_str());
  if (!trusted_tier) {
    // Process-global flag — note the dummy argv[0] (V8 skips it).
    RESOLVE(guest, v8_jsi_set_v8_flags);
    char arg0[] = "v8host";
    char flag[] = "--jitless";
    char* flag_argv[] = {arg0, flag};
    size_t flag_argc = 2;
    p_v8_jsi_set_v8_flags(&flag_argc, flag_argv, /*remove_flags=*/true);
    printf("[v8host] --jitless %s\n",
           flag_argc == 1 ? "recognized" : "NOT recognized");
  }
  // If a startup snapshot was supplied, pre-check it against THIS engine now —
  // engine build flags are fixed and the process-global --jitless (if any) is
  // set, so the version tag matches what the blob was built with. A stale or
  // cross-engine blob is dropped here and we continue WITHOUT a snapshot rather
  // than feed V8 an incompatible blob (which would crash). The engine also
  // validates internally as a safety net; this just surfaces a clear reason and
  // lets the host choose its fallback. (An engine too old to export the check
  // simply skips it and relies on the internal validation.)
  if (g_have_snapshot) {
    auto p_compat = reinterpret_cast<decltype(&v8_startup_snapshot_compatible)>(
        ::GetProcAddress(guest, "v8_startup_snapshot_compatible"));
    auto p_compat_str =
        reinterpret_cast<decltype(&v8_startup_snapshot_compat_string)>(
            ::GetProcAddress(guest, "v8_startup_snapshot_compat_string"));
    if (p_compat) {
      const int code =
          p_compat(reinterpret_cast<const uint8_t*>(g_snapshot_blob.data()),
                   g_snapshot_blob.size());
      if (code != 0) {
        const char* why = p_compat_str ? p_compat_str(code) : "incompatible";
        printf("[v8host] WARNING: startup snapshot rejected (%s); continuing "
               "without it\n", why);
        g_have_snapshot = false;
        g_snapshot_blob.clear();
        g_snapshot_blob.shrink_to_fit();
      } else {
        printf("[v8host] startup snapshot compatible with engine\n");
      }
    }
  }
  // Resolve the config setters the configure callback uses, then build the
  // runtime through the JSI C++ API (JsiAbiRuntime injects our JSI_ABI_VERSION).
  {
    RESOLVE(guest, v8_jsi_config_set_task_runner);
    RESOLVE(guest, v8_jsi_config_set_explicit_microtask_policy);
    g_set_task_runner = p_v8_jsi_config_set_task_runner;
    g_set_microtask = p_v8_jsi_config_set_explicit_microtask_policy;
    if (g_have_snapshot) {
      RESOLVE(guest, v8_jsi_config_set_startup_snapshot);
      g_set_snapshot = p_v8_jsi_config_set_startup_snapshot;
    }
  }
  RESOLVE(guest, v8_create_runtime);
  std::unique_ptr<Runtime> rt_owner =
      jsi::abi::makeJsiAbiRuntime(p_v8_create_runtime, &ConfigureRuntime, nullptr);
  if (!rt_owner) {
    printf("[v8host] makeJsiAbiRuntime failed\n");
    return 22;
  }
  Runtime& rt = *rt_owner;
  const LONGLONG t_runtime_created = g_perf.now();
  // In snapshot mode this delta folds in the blob deserialize (the embedded
  // [shim + bundle] heap is materialized here) — that is the deserialize tax.
  g_perf.emit("runtime-create", t_engine_loaded, t_runtime_created);
  printf("[v8host] v8jsi runtime created (warmup, pre-lockdown)\n");

  // --- drop privileges (installs file interceptions + applies ACG) ---
  sbox_target_lower_token(target);
  const LONGLONG t_lockdown = g_perf.now();
  g_perf.emit("lockdown", t_runtime_created, t_lockdown);
  printf("[v8host] LowerToken() survived\n");
  PrintAcgStatus();

  const bool ping_post = sbox_target_test_ipc(target) != 0;
  printf("[v8host] IPC test (post-lockdown) = %s\n", ping_post ? "OK" : "FAIL");

  // --- brokered file proxy: an UNMODIFIED CreateFileW. The restricted token
  // denies it; the self-installed NtCreateFile interception routes the denied
  // open to the broker, which evaluates the policy. ---
  const std::wstring allowed = EnvW(L"SBOX_DEMO_ALLOWED");
  const std::wstring denied = EnvW(L"SBOX_DEMO_DENIED");
  DWORD allowed_rc = TryReadOpen(allowed);
  DWORD denied_rc = TryReadOpen(denied);
  const bool nonallowed_denied = (denied_rc != ERROR_SUCCESS);
#if defined(_WIN64)
  const bool proxied_ok = (allowed_rc == ERROR_SUCCESS);
  printf("[v8host] policy-allowed file open (broker-proxied) = %-7s (err=%lu)\n",
         proxied_ok ? "OPENED" : "FAILED", allowed_rc);
#else
  // v8-jsi: on the 32-bit (x86) target SelfInstallInterceptions() is a no-op
  // (the Chromium sandbox interception thunks are 64-bit-only — see
  // interception.cc), so there is NO brokered file re-allow channel. Under the
  // deny-all Untrusted model that channel is unused, so the policy-allowed open
  // is expected to be denied and is NOT a pass criterion on x86. (The hard
  // kernel-level denial of the non-allowed open below still holds.)
  const bool proxied_ok = true;  // not applicable on x86 (no interceptions)
  printf("[v8host] policy-allowed file open (broker-proxied) = N/A     "
         "(no interceptions on x86; deny-all needs none, err=%lu)\n", allowed_rc);
#endif
  printf("[v8host] non-allowed file open                     = %-7s (err=%lu)\n",
         nonallowed_denied ? "DENIED" : "OPENED!", denied_rc);

  // --- Stage 2: WebView2-style JS messaging, all POST-LOCKDOWN (jitless+ACG) ---
  // Install the `host` object, run the untrusted JS (sets onmessage, posts
  // "ready"), then own the JS thread in an event loop until the host closes the
  // channel. All via the JSI C++ API — RAII, exceptions, no manual ABI calls.
  bool js_ok = false;
  DrainCtx dctx{&rt};
  try {
    InstallHostObject(rt, target);
    std::string guest_src =
        use_guest_file ? std::move(guest_js) : std::string(kUserJs);
    // Split parse+compile from execute so each is timed separately. In text
    // mode (no snapshot) the guest is parse-heavy and this shows the real
    // parse vs execute cost; in snapshot mode the guest is only the small
    // kickoff, so both collapse toward ~0 (the bundle's parse/execute moved
    // into the snapshot, surfacing as a larger runtime-create above).
    auto guest_buf = std::make_shared<StringBuffer>(std::move(guest_src));
    const char* guest_url = use_guest_file ? "guest.js" : "user.js";
    auto prepared = rt.prepareJavaScript(guest_buf, guest_url);
    const LONGLONG t_parsed = g_perf.now();
    g_perf.emit("parse", t_lockdown, t_parsed);
    rt.evaluatePreparedJavaScript(prepared);
    const LONGLONG t_executed = g_perf.now();
    g_perf.emit("execute", t_parsed, t_executed);
    js_ok = true;
    printf("[v8host] %s JS evaluated = OK\n", use_guest_file ? "guest" : "user");
    rt.drainMicrotasks();
    // kickoff: time to the guest's first outbound host.postMessage. In a real
    // async kickoff (e.g. a host API call that resolves a promise) the first
    // frame is posted during this post-execute microtask drain, so the segment
    // captures it. For a synchronous kickoff the post already happened inside
    // execute (counted there), so we note that rather than print a spurious or
    // negative delta.
    if (g_perf.on) {
      if (g_first_post_qpc >= t_executed)
        g_perf.emit("kickoff", t_executed, g_first_post_qpc);
      else if (g_first_post_qpc != 0)
        printf("[v8host][perf] %-14s = (during execute, synchronous)\n",
               "kickoff");
    }

    // Event loop — owns the JS thread. Wait on inbound messages, engine-posted
    // foreground tasks, and the host close signal.
    HANDLE inbound = static_cast<HANDLE>(sbox_target_inbound_event(target));
    HANDLE close_evt = static_cast<HANDLE>(sbox_target_close_event(target));
    HANDLE waits[3] = {inbound, task_queue.wake, close_evt};
    printf("[v8host] entering JS message loop (until host closes channel)\n");
    for (;;) {
      DWORD w = ::WaitForMultipleObjects(3, waits, FALSE, INFINITE);
      if (w == WAIT_OBJECT_0 + 2) {  // host closed the channel
        sbox_target_drain_messages(target, OnInbound, &dctx);  // final drain
        break;
      } else if (w == WAIT_OBJECT_0) {  // inbound message
        sbox_target_drain_messages(target, OnInbound, &dctx);
      } else if (w == WAIT_OBJECT_0 + 1) {  // engine-posted foreground task(s)
        RunQueuedTasks(&task_queue);
      } else {
        break;  // WAIT_FAILED / unexpected
      }
      rt.drainMicrotasks();
    }
    printf("[v8host] JS loop exited: onmessage delivered %d string + %d binary\n",
           dctx.strings, dctx.binaries);
  } catch (const JSError& e) {
    printf("[v8host] JS error: %s\n", e.getMessage().c_str());
  } catch (const std::exception& e) {
    printf("[v8host] exception: %s\n", e.what());
  }
  const bool msg_ok = (dctx.strings >= 1 && dctx.binaries >= 1);

  // Destroy the runtime BEFORE tearing down the task queue it references (the
  // runtime's destruction fires TaskRunnerDeleteCb, which touches task_queue).
  rt_owner.reset();

  const bool pass = ping_pre && ping_post && proxied_ok && nonallowed_denied &&
                    js_ok && msg_ok;
  printf("[v8host] RESULT: %s\n",
         pass ? "PASS - untrusted JS exchanges WebView2-style messages "
                "(postMessage/onmessage, string+binary) under lockdown via the "
                "JSI C++ API over a generic, V8-agnostic sbox.dll"
              : "FAIL");
  sbox_target_end(target);
  ::CloseHandle(task_queue.wake);
  ::DeleteCriticalSection(&task_queue.cs);
  return pass ? 0 : 13;
}
