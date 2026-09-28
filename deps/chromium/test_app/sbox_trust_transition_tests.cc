#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <userenv.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "sbox.h"
#include "sbox_trust_transition_test_private.h"

namespace {

constexpr wchar_t kLpacProfile[] = L"V8Jsi.Sbox.TrustTransition.Test";

struct CaseSpec {
  uint32_t id;
  const char *name;
  const char *stage;
  DWORD exit_code;
  bool success;
  bool lpac;
  bool acg;
};

constexpr CaseSpec kCases[] = {
    {SBOX_TRUST_TRANSITION_CASE_ALLOC_FAILURE, "allocation-failure", "thunk-allocation", 23, false, false, true},
    {SBOX_TRUST_TRANSITION_CASE_FIRST_HOOK_FAILURE, "first-hook-failure", "hook-install", 23, false, false, true},
    {SBOX_TRUST_TRANSITION_CASE_PARTIAL_HOOK_FAILURE, "partial-hook-failure", "hook-install", 23, false, false, true},
    {SBOX_TRUST_TRANSITION_CASE_RX_FAILURE, "rx-failure", "thunk-rx", 23, false, false, true},
    {SBOX_TRUST_TRANSITION_CASE_FINAL_MITIGATION_FAILURE,
     "final-mitigation-failure",
     "final-mitigation",
     7011,
     false,
     false,
     true},
    {SBOX_TRUST_TRANSITION_CASE_ACG_QUERY_FAILURE, "acg-query-failure", "acg-query", 23, false, false, true},
    {SBOX_TRUST_TRANSITION_CASE_ACG_WEAK_STATE, "acg-weak-state", "acg-postcondition", 23, false, false, true},
    {SBOX_TRUST_TRANSITION_CASE_STRICT_SUCCESS, "ordinary-strict-success", nullptr, 0, true, false, true},
    {SBOX_TRUST_TRANSITION_CASE_LPAC_SUCCESS, "lpac-low-strict-success", nullptr, 0, true, true, true},
    {SBOX_TRUST_TRANSITION_CASE_ACG_OFF_SUCCESS, "ordinary-acg-off-success", nullptr, 0, true, false, false},
};

struct Messages {
  std::mutex mutex;
  std::vector<std::string> values;
};

void OnMessage(void *context, int kind, const void *data, size_t length) {
  if (kind != SBOX_MSG_STRING)
    return;
  auto *messages = static_cast<Messages *>(context);
  std::lock_guard<std::mutex> lock(messages->mutex);
  messages->values.emplace_back(static_cast<const char *>(data), length);
}

std::wstring TargetPath() {
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring result(path);
  result.resize(result.find_last_of(L'\\') + 1);
  return result + L"sbox_trust_transition_target.exe";
}

SboxPolicy MakePolicy(const CaseSpec &spec) {
  static const wchar_t *const capabilities[] = {
      L"S-1-15-3-4021848294-1651122667-3873966303-2985905677",
  };
  SboxPolicy policy = {};
  policy.struct_size = sizeof(policy);
  policy.initial_token = SBOX_TOKEN_RESTRICTED_SAME_ACCESS;
  policy.lockdown_token = SBOX_TOKEN_LOCKDOWN;
  policy.integrity = SBOX_INTEGRITY_LOW;
  policy.delayed_integrity = spec.lpac ? SBOX_INTEGRITY_LOW : SBOX_INTEGRITY_UNTRUSTED;
  policy.prohibit_dynamic_code = spec.acg ? 1 : 0;
  policy.use_app_container = spec.lpac ? 1 : 0;
  policy.low_privilege_app_container = spec.lpac ? 1 : 0;
  policy.app_container_profile_name = kLpacProfile;
  policy.capabilities = spec.lpac ? capabilities : nullptr;
  policy.capability_count = spec.lpac ? std::size(capabilities) : 0;
  return policy;
}

size_t Count(const std::vector<std::string> &messages, const char *prefix, uint32_t case_id) {
  char expected[80] = {};
  std::snprintf(expected, sizeof(expected), "%s case=%u", prefix, case_id);
  return static_cast<size_t>(std::count(messages.begin(), messages.end(), expected));
}

bool Contains(const std::string &output, const char *text) {
  return output.find(text) != std::string::npos;
}

bool DeleteTestProfile(bool require_existing) {
  const HRESULT result = ::DeleteAppContainerProfile(kLpacProfile);
  if (SUCCEEDED(result))
    return true;
  if (!require_existing && result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
    return true;
  }
  printf("[trust-transition-tests] fixture profile cleanup failed: hr=0x%08lx\n", static_cast<unsigned long>(result));
  return false;
}

bool RunWithoutControl(const char *label) {
  const CaseSpec &base = kCases[7];
  SboxPolicy policy = MakePolicy(base);
  SboxSession *session = sbox_broker_spawn(TargetPath().c_str(), &policy, nullptr, nullptr);
  if (!session) {
    printf("[trust-transition-tests] %s: spawn failed\n", label);
    return false;
  }
  const int exit_code = sbox_broker_wait(session);
  const bool ok = exit_code == 40;
  printf("[trust-transition-tests] %s: exit=%d expected=40 -> %s\n", label, exit_code, ok ? "PASS" : "FAIL");
  return ok;
}

bool RunCase(const CaseSpec &spec, uint64_t serial) {
  printf(
      "[trust-transition-tests] selected=%u name=%s serial=%llu\n",
      spec.id,
      spec.name,
      static_cast<unsigned long long>(serial));
  if (!sbox_trust_transition_test_set_next_case(spec.id, serial)) {
    printf("[trust-transition-tests] set-next failed\n");
    return false;
  }
  if (sbox_trust_transition_test_set_next_case(spec.id, serial + 10000)) {
    printf("[trust-transition-tests] duplicate control was accepted\n");
    return false;
  }

  if (spec.lpac && !DeleteTestProfile(false))
    return false;
  SboxPolicy policy = MakePolicy(spec);
  Messages messages;
  SboxSession *session = sbox_broker_spawn(TargetPath().c_str(), &policy, &OnMessage, &messages);
  if (!session) {
    printf("[trust-transition-tests] spawn failed\n");
    return false;
  }
  const char request[] = "REQUEST";
  sbox_broker_post_message(session, SBOX_MSG_STRING, request, sizeof(request) - 1);
  const int exit_code = sbox_broker_wait(session);
  const bool fixture_cleanup_ok = !spec.lpac || DeleteTestProfile(true);

  const char *captured = sbox_trust_transition_test_last_output();
  const std::string output = captured ? captured : "";
  std::vector<std::string> snapshot;
  {
    std::lock_guard<std::mutex> lock(messages.mutex);
    snapshot = messages.values;
  }
  const size_t ready = Count(snapshot, "SECURITY_READY", spec.id);
  const size_t guest = Count(snapshot, "GUEST_ENTRY", spec.id);
  const size_t host = Count(snapshot, "HOST_OPERATION", spec.id);
  const size_t result = Count(snapshot, "RESULT", spec.id);

  char selected[64] = {};
  std::snprintf(
      selected, sizeof(selected), "selected=%u serial=%llu", spec.id, static_cast<unsigned long long>(serial));
  char consumed_control[80] = {};
  std::snprintf(
      consumed_control,
      sizeof(consumed_control),
      "consumed-control=%u serial=%llu",
      spec.id,
      static_cast<unsigned long long>(serial));
  bool ok = exit_code == static_cast<int>(spec.exit_code) && Contains(output, selected) &&
      Contains(output, consumed_control) && fixture_cleanup_ok;
  if (spec.success) {
    ok = ok && ready == 1 && guest == 1 && host == 1 && result == 1 && Contains(output, "pre-lockdown-ipc=ok") &&
        Contains(output, "final-lockdown=ok") && Contains(output, "post-lockdown-ipc=ok") &&
        Contains(output, "rx=1 protect=0x20");
    ok = ok && Contains(output, spec.acg ? "strict_acg_expected=true" : "strict_acg_expected=false");
    if (spec.lpac)
      ok = ok && Contains(output, "lpac=1 app_container=1") && Contains(output, "capability=present");
  } else {
    char consumed[32] = {};
    std::snprintf(consumed, sizeof(consumed), "consumed=%u", spec.id);
    ok = ok && spec.stage && Contains(output, spec.stage) && Contains(output, consumed) && ready == 0 && guest == 0 &&
        host == 0 && result == 0;
    if (spec.id == SBOX_TRUST_TRANSITION_CASE_FIRST_HOOK_FAILURE)
      ok = ok && Contains(output, "installed=0");
    if (spec.id == SBOX_TRUST_TRANSITION_CASE_PARTIAL_HOOK_FAILURE)
      ok = ok && Contains(output, "installed=1");
  }
  printf(
      "[trust-transition-tests] case=%s exit=%d markers=%zu/%zu/%zu/%zu -> %s\n",
      spec.name,
      exit_code,
      ready,
      guest,
      host,
      result,
      ok ? "PASS" : "FAIL");
  return ok;
}

const CaseSpec *FindCase(const char *name) {
  for (const auto &spec : kCases) {
    if (std::strcmp(name, spec.name) == 0)
      return &spec;
  }
  return nullptr;
}

void PrintHelp() {
  printf("Usage: sbox_trust_transition_tests.exe --all | --case <name>\nCases:\n");
  for (const auto &spec : kCases)
    printf("  %s\n", spec.name);
}

} // namespace

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    PrintHelp();
    return 0;
  }
  if (argc == 3 && std::strcmp(argv[1], "--case") == 0) {
    const CaseSpec *spec = FindCase(argv[2]);
    if (!spec) {
      PrintHelp();
      return 2;
    }
    return RunCase(*spec, 1) ? 0 : 1;
  }
  if (argc != 2 || std::strcmp(argv[1], "--all") != 0) {
    PrintHelp();
    return 2;
  }

  bool ok = RunWithoutControl("missing-control");
  ok = !sbox_trust_transition_test_set_next_case(999, 1) && ok;
  printf("[trust-transition-tests] unknown-control -> %s\n", ok ? "PASS" : "FAIL");
  uint64_t serial = 1;
  for (const auto &spec : kCases)
    ok = RunCase(spec, serial++) && ok;
  ok = RunWithoutControl("stale-control") && ok;
  printf("[trust-transition-tests] all -> %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
