// test_app.exe — generic broker test harness for the public sbox.dll C API. It
// knows nothing about the sandbox internals or V8; it builds a policy and asks
// sbox.dll to broker a sandboxed target EXE.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "sbox.h"
#include "sbox_harden.h"

namespace {

bool EnvFlagEnabled(const wchar_t* name) {
  wchar_t value[2] = {};
  return ::GetEnvironmentVariableW(name, value, 2) == 1 && value[0] == L'1';
}

std::wstring TempFilePath(const wchar_t* name) {
  wchar_t dir[MAX_PATH] = {};
  DWORD n = ::GetTempPathW(MAX_PATH, dir);
  std::wstring path =
      (n == 0 || n > MAX_PATH) ? std::wstring(L"C:\\") : std::wstring(dir, n);
  return path + name;
}

// Create a small file the broker can later open on the target's behalf.
bool CreateProbeFile(const std::wstring& path) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  const char kBytes[] = "sbox-generic-proxy-probe\n";
  DWORD wrote = 0;
  ::WriteFile(h, kBytes, sizeof(kBytes) - 1, &wrote, nullptr);
  ::CloseHandle(h);
  return true;
}

// Host-side message context: drives the WebView2-style demo from the reader
// thread and signals `done` once the JS has echoed both a string and a binary.
struct HostCtx {
  SboxSession* session = nullptr;
  HANDLE done = nullptr;  // manual-reset; set when the demo round-trip completes
  bool driven = false;    // pushed the initial string+binary yet?
  int js_string_echoes = 0;
  int js_binary_echoes = 0;
};

// Invoked on the broker's reader thread for each message the target posts.
// Treats target->host messages as UNTRUSTED input (bounded, no assumptions).
void OnTargetMessage(void* ctx, int kind, const void* data, size_t len) {
  HostCtx* hc = static_cast<HostCtx*>(ctx);
  if (kind == SBOX_MSG_STRING) {
    std::string s(static_cast<const char*>(data), len);
    printf("[test_app] <- target (string): \"%s\"\n", s.c_str());
    if (s == "ready") {
      // The JS is set up and listening — host-initiated push (host -> target),
      // proving the reverse direction reaches host.onmessage.
      if (!hc->driven) {
        hc->driven = true;
        const char* ping = "ping from host";
        sbox_broker_post_message(hc->session, SBOX_MSG_STRING, ping,
                                 std::strlen(ping));
        const unsigned char bin[] = {0xDE, 0xAD, 0xBE, 0xEF};
        sbox_broker_post_message(hc->session, SBOX_MSG_BINARY, bin, sizeof(bin));
        printf("[test_app] -> target: pushed string + binary (host-initiated)\n");
      }
    } else {
      ++hc->js_string_echoes;  // a JS echo reply (host.postMessage from JS)
    }
  } else {
    unsigned first = len ? static_cast<const unsigned char*>(data)[0] : 0;
        printf("[test_app] <- target (binary, %zu bytes, first=0x%02x)\n", len,
          first);
    ++hc->js_binary_echoes;  // JS tagged byte 0 -> expect 0x42
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

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);

  // Hardening: restrict the loader search path, then verify the sandbox engine
  // DLL we just loaded is genuine. A planted sbox.dll would replace the broker.
  // Unsigned local and PR-validation binaries require an explicit test opt-in;
  // production callers fail closed by default.
  const bool allow_unsigned = EnvFlagEnabled(L"SBOX_ALLOW_UNSIGNED");
  sbox_harden::HardenDllSearch();
  if (!sbox_harden::CheckTrust(sbox_harden::LoadedModulePath(L"sbox.dll"),
                               "sbox.dll", allow_unsigned)) {
    return 30;
  }

  // Two probe files; only one is allowed by policy. Create both so the denial of
  // the other is observably from the policy, not a missing file. The paths flow
  // to the target via the environment (app-level glue — not the sandbox API).
  const std::wstring allowed = TempFilePath(L"sbox_generic_allowed.txt");
  const std::wstring denied = TempFilePath(L"sbox_generic_denied.txt");
  CreateProbeFile(allowed);
  CreateProbeFile(denied);
  ::SetEnvironmentVariableW(L"SBOX_DEMO_ALLOWED", allowed.c_str());
  ::SetEnvironmentVariableW(L"SBOX_DEMO_DENIED", denied.c_str());

  // Tier selection (the host picks the policy). Untrusted (default) arms ACG and
  // the target runs jitless; Trusted leaves ACG off so V8 can JIT. SBOX_TIER is
  // in our environment and is inherited by the spawned target, which reads it to
  // decide jitless. Both tiers keep the restricted lockdown token + low/untrusted
  // integrity — only ACG (and the target's jitless) differ.
  wchar_t tier_buf[32] = {};
  ::GetEnvironmentVariableW(L"SBOX_TIER", tier_buf, 32);
  const bool trusted = (std::wstring(tier_buf) == L"trusted");
  const wchar_t* engine_name = trusted ? L"v8jsi.dll" : L"v8jsisb.dll";
  const char* engine_tag = trusted ? "v8jsi.dll" : "v8jsisb.dll";

  SboxFileRule rules[1] = {{allowed.c_str(), /*readonly=*/1}};
  SboxPolicy policy = {};
  policy.initial_token = SBOX_TOKEN_RESTRICTED_SAME_ACCESS;
  policy.lockdown_token = SBOX_TOKEN_LOCKDOWN;
  policy.integrity = SBOX_INTEGRITY_LOW;
  policy.delayed_integrity = SBOX_INTEGRITY_UNTRUSTED;
  policy.prohibit_dynamic_code = trusted ? 0 : 1;  // ACG off only for Trusted
  policy.file_rules = rules;
  policy.file_rule_count = 1;
  printf("[test_app] tier = %s\n",
         trusted ? "Trusted (JIT, ACG off)" : "Untrusted (jitless + ACG)");

  const std::wstring target = TargetExeBesideUs(L"v8host.exe");
  printf("[test_app] test_app.exe -> sbox_broker_run\n");
  printf("[test_app] target = %ls\n", target.c_str());
  printf("[test_app] allow (readonly) = %ls\n", allowed.c_str());

  // Verify every artifact the target will use BEFORE spawning it, here in the
  // unrestricted broker — the sandboxed target can't reliably call
  // WinVerifyTrust (restricted token / low integrity). In the same-EXE model the
  // target is the same signed image by construction; here we check explicitly:
  // the target EXE and the guest DLL it will load (both in our application dir).
  const std::wstring guest = sbox_harden::ExeDir() + engine_name;
  if (!sbox_harden::CheckTrust(target, "v8host.exe", allow_unsigned) ||
      !sbox_harden::CheckTrust(guest, engine_tag, allow_unsigned)) {
    return 31;
  }

  // Spawn (non-blocking) with a message handler so we can exchange messages with
  // the target while it runs. The handler drives the WebView2-style demo: on the
  // JS-posted "ready", it pushes a string + a binary and signals `done` once the
  // JS has echoed both back.
  HostCtx hc;
  hc.done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset
  SboxSession* session =
      sbox_broker_spawn(target.c_str(), &policy, OnTargetMessage, &hc);
  if (!session) {
    printf("[test_app] sbox_broker_spawn failed\n");
    return 32;
  }
  hc.session = session;

  // Wait for the round-trip to complete (or a safety timeout), then close the
  // channel so the target's event loop exits cleanly.
  DWORD ww = ::WaitForSingleObject(hc.done, 15000);
  printf("[test_app] demo %s (js echoes: %d string, %d binary) -> closing channel\n",
         ww == WAIT_OBJECT_0 ? "complete" : "TIMED OUT", hc.js_string_echoes,
         hc.js_binary_echoes);
  sbox_broker_close(session);

  int rc = sbox_broker_wait(session);
  ::CloseHandle(hc.done);
  printf("[test_app] target finished rc=%d\n", rc);
  return rc;
}
