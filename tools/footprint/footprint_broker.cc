// footprint_broker.exe — measurement broker (measurement-only; not shipped).
//
// A drop-in stand-in for test_app.exe that spawns the UNMODIFIED v8host.exe
// through the UNMODIFIED sbox.dll, drives the exact same WebView2-style demo
// handshake so the untrusted JS runs to its steady state... and then, instead of
// closing the channel immediately (which is what test_app does, causing v8host
// to exit within ~1 s), it HOLDS the channel open. That parks v8host in its
// post-lockdown message loop — the defined "ready, after a representative JS
// workload ran" point — for as long as the harness needs to sample memory and to
// stage N concurrent instances.
//
// No product code is modified: this is a new broker host that consumes the same
// public sbox.dll C ABI (sbox.h) as test_app.
//
// Env knobs (read by this broker; SBOX_TIER is also inherited by the target):
//   SBOX_TIER               = "trusted" -> JIT + ACG off; else Untrusted jitless+ACG
//   SBOX_FP_HOLD_MS         = max hold in ms after the round-trip (default 10000)
//   SBOX_FP_RELEASE_EVENT   = name of a manual-reset event; when the harness sets
//                             it, all holding brokers release early (optional)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sbox.h"
#include "sbox_harden.h"

namespace {

std::wstring TempFilePath(const wchar_t* name) {
  wchar_t dir[MAX_PATH] = {};
  DWORD n = ::GetTempPathW(MAX_PATH, dir);
  std::wstring path =
      (n == 0 || n > MAX_PATH) ? std::wstring(L"C:\\") : std::wstring(dir, n);
  return path + name;
}

bool CreateProbeFile(const std::wstring& path) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  const char kBytes[] = "sbox-fp-probe\n";
  DWORD wrote = 0;
  ::WriteFile(h, kBytes, sizeof(kBytes) - 1, &wrote, nullptr);
  ::CloseHandle(h);
  return true;
}

struct HostCtx {
  SboxSession* session = nullptr;
  HANDLE done = nullptr;
  bool driven = false;
  int js_string_echoes = 0;
  int js_binary_echoes = 0;
};

// Same handshake as test_app: on JS "ready", push a string + a binary; signal
// done once JS has echoed both back. That guarantees the target reached and is
// sitting in its steady-state message loop.
void OnTargetMessage(void* ctx, int kind, const void* data, size_t len) {
  HostCtx* hc = static_cast<HostCtx*>(ctx);
  if (kind == SBOX_MSG_STRING) {
    std::string s(static_cast<const char*>(data), len);
    if (s == "ready") {
      if (!hc->driven) {
        hc->driven = true;
        const char* ping = "ping from host";
        sbox_broker_post_message(hc->session, SBOX_MSG_STRING, ping,
                                 std::strlen(ping));
        const unsigned char bin[] = {0xDE, 0xAD, 0xBE, 0xEF};
        sbox_broker_post_message(hc->session, SBOX_MSG_BINARY, bin, sizeof(bin));
      }
    } else {
      ++hc->js_string_echoes;
    }
  } else {
    ++hc->js_binary_echoes;
  }
  if (hc->js_string_echoes >= 1 && hc->js_binary_echoes >= 1 && hc->done)
    ::SetEvent(hc->done);
}

std::wstring TargetExeBesideUs(const wchar_t* exe) {
  wchar_t self[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring path(self);
  size_t slash = path.find_last_of(L'\\');
  if (slash != std::wstring::npos)
    path.resize(slash + 1);
  return path + exe;
}

DWORD EnvDword(const wchar_t* name, DWORD def) {
  wchar_t buf[32] = {};
  DWORD n = ::GetEnvironmentVariableW(name, buf, 32);
  if (n == 0 || n >= 32)
    return def;
  return (DWORD)_wtoi(buf);
}

// How many sandboxed targets this one broker should spawn. Primary: `--count N`
// argv (what footprint.ts passes); fallback: the SBOX_FP_COUNT env knob; default
// 1 (the legacy single-target behavior, e.g. for a manual run).
int InstanceCount(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
      int n = std::atoi(argv[i + 1]);
      return n > 0 ? n : 1;
    }
  }
  DWORD env_n = EnvDword(L"SBOX_FP_COUNT", 0);
  return env_n > 0 ? (int)env_n : 1;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  sbox_harden::HardenDllSearch();

  const int count = InstanceCount(argc, argv);

  const std::wstring allowed = TempFilePath(L"sbox_fp_allowed.txt");
  const std::wstring denied = TempFilePath(L"sbox_fp_denied.txt");
  CreateProbeFile(allowed);
  CreateProbeFile(denied);
  ::SetEnvironmentVariableW(L"SBOX_DEMO_ALLOWED", allowed.c_str());
  ::SetEnvironmentVariableW(L"SBOX_DEMO_DENIED", denied.c_str());

  wchar_t tier_buf[32] = {};
  ::GetEnvironmentVariableW(L"SBOX_TIER", tier_buf, 32);
  const bool trusted = (std::wstring(tier_buf) == L"trusted");

  SboxFileRule rules[1] = {{allowed.c_str(), /*readonly=*/1}};
  SboxPolicy policy = {};
  policy.initial_token = SBOX_TOKEN_RESTRICTED_SAME_ACCESS;
  policy.lockdown_token = SBOX_TOKEN_LOCKDOWN;
  policy.integrity = SBOX_INTEGRITY_LOW;
  policy.delayed_integrity = SBOX_INTEGRITY_UNTRUSTED;
  policy.prohibit_dynamic_code = trusted ? 0 : 1;
  policy.file_rules = rules;
  policy.file_rule_count = 1;

  const std::wstring target = TargetExeBesideUs(L"v8host.exe");
  printf("[fpbroker] pid=%lu tier=%s SPAWN %d x %ls\n", ::GetCurrentProcessId(),
         trusted ? "trusted" : "untrusted", count, target.c_str());

  // One broker (this process, a test_app stand-in) spawns N locked-down
  // v8host targets through the same sbox.dll BrokerServices singleton — the real
  // production topology (one app -> several sandboxed instances), not N brokers.
  // unique_ptr keeps each HostCtx at a stable address for its spawn callback as
  // the vector grows; each session's callback only ever touches its own ctx.
  std::vector<std::unique_ptr<HostCtx>> ctxs;
  std::vector<HANDLE> dones;
  ctxs.reserve(count);
  dones.reserve(count);
  for (int i = 0; i < count; ++i) {
    auto hc = std::make_unique<HostCtx>();
    hc->done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // No trust checks here — measurement tool; binaries are the dev build beside us.
    SboxSession* session =
        sbox_broker_spawn(target.c_str(), &policy, OnTargetMessage, hc.get());
    if (!session) {
      printf("[fpbroker] sbox_broker_spawn failed (instance %d/%d)\n", i + 1,
             count);
      for (auto& c : ctxs) {
        sbox_broker_close(c->session);
        sbox_broker_wait(c->session);
        ::CloseHandle(c->done);
      }
      ::CloseHandle(hc->done);
      return 32;
    }
    hc->session = session;
    dones.push_back(hc->done);
    ctxs.push_back(std::move(hc));
  }

  // Wait until every target has run the demo round-trip and parked in its
  // post-lockdown JS message loop.
  DWORD ww = ::WaitForMultipleObjects((DWORD)dones.size(), dones.data(),
                                      /*bWaitAll=*/TRUE, 30000);
  if (ww != WAIT_OBJECT_0) {
    printf("[fpbroker] demo TIMED OUT before steady state -> closing\n");
    int rc = 40;
    for (auto& c : ctxs) {
      sbox_broker_close(c->session);
      int r = sbox_broker_wait(c->session);
      if (r) rc = r;
      ::CloseHandle(c->done);
    }
    return rc;
  }

  // Steady state reached for all N targets: HOLD so the harness can sample.
  // Release on the named event or on the hold timeout, whichever comes first.
  const DWORD hold_ms = EnvDword(L"SBOX_FP_HOLD_MS", 10000);
  HANDLE release = nullptr;
  wchar_t evname[128] = {};
  if (::GetEnvironmentVariableW(L"SBOX_FP_RELEASE_EVENT", evname, 128) > 0)
    release = ::OpenEventW(SYNCHRONIZE, FALSE, evname);
  printf("[fpbroker] HELD pid=%lu (%d targets in steady state) hold_ms=%lu\n",
         ::GetCurrentProcessId(), count, hold_ms);
  if (release) {
    ::WaitForSingleObject(release, hold_ms);
    ::CloseHandle(release);
  } else {
    ::Sleep(hold_ms);
  }

  printf("[fpbroker] RELEASE pid=%lu -> closing %d channel(s)\n",
         ::GetCurrentProcessId(), count);
  int rc = 0, total_str = 0, total_bin = 0;
  for (auto& c : ctxs) {
    sbox_broker_close(c->session);
    int r = sbox_broker_wait(c->session);
    if (r) rc = r;
    total_str += c->js_string_echoes;
    total_bin += c->js_binary_echoes;
    ::CloseHandle(c->done);
  }
  printf("[fpbroker] %d target(s) finished rc=%d (js echoes: %d str, %d bin)\n",
         count, rc, total_str, total_bin);
  return rc;
}
