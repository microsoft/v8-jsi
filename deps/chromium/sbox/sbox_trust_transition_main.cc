// sbox_trust_transition_main.cc — the instrumented single-image trust-transition
// test. ONE binary that is BOTH the sandbox broker/case-runner and the sandbox
// worker/fault-harness, selected by argv, statically linking the same sandbox
// core as sbox.exe (via sbox_broker.cc). It folds the former two-image suite (a
// separate case-runner EXE + a fault-injection target EXE joined by the retired
// export-table relay) onto the same-image seed:
//
//   * --worker : the fault-injection worker. Adopts the broker's same-image
//                seed, reads the RVA-seeded control struct, drives the trust
//                transition, and injects the selected fault. Calls ONLY
//                sbox_target_*. (Was sbox_trust_transition_target.cc.)
//   * --all / --case <name> / --policy / --exit-diagnostics / --help : the case
//                runner. For each case it sets the next case, spawns itself
//                --worker (the broker appends the switch), and seeds the control
//                struct by RVA, then asserts the per-case exit code / markers.
//                Calls ONLY sbox_broker_* + the trust-transition control API.
//                (Was sbox_trust_transition_tests.cc.)
//
// Role dispatch is FIRST in main() so neither persona can fall through into the
// other's init. testonly; SBOX_TRUST_TRANSITION_TESTING-gated; never shipped.

#define SBOX_DLL_IMPL  // sbox_* are defined in THIS image (sbox_broker.cc).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <sddl.h>
#include <userenv.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "sbox_core_internal.h"
#include "sbox_trust_transition_test_private.h"

// The same-image control struct: the broker role seeds it by RVA into the
// suspended worker (sbox_broker.cc), the worker role reads it. Defined once here;
// declared extern in the private header so both TUs reference the one symbol.
// Not exported — addressed by same-image RVA, never by an export lookup.
extern "C" SboxTrustTransitionTestControl g_sbox_trust_transition_test_control = {};

namespace {

// ===========================================================================
// Worker role (fault-injection harness) — was sbox_trust_transition_target.cc.
// ===========================================================================

constexpr DWORD kTransitionExit = 23;

using FileHookCode = std::array<std::array<unsigned char, 32>, 5>;

bool ReadFileHookCode(FileHookCode &code) {
  constexpr const char *names[] = {
      "NtCreateFile", "NtOpenFile", "NtQueryAttributesFile", "NtQueryFullAttributesFile", "NtSetInformationFile"};
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) {
    printf("[trust-transition-target] file-hook module query failed: error=%lu\n", ::GetLastError());
    return false;
  }
  for (size_t i = 0; i < code.size(); ++i) {
    const auto entry = ::GetProcAddress(ntdll, names[i]);
    SIZE_T read = 0;
    if (!entry ||
        !::ReadProcessMemory(
            ::GetCurrentProcess(), reinterpret_cast<const void *>(entry), code[i].data(), code[i].size(), &read) ||
        read != code[i].size()) {
      printf("[trust-transition-target] file-hook code read failed: hook=%s error=%lu\n", names[i], ::GetLastError());
      return false;
    }
  }
  return true;
}

bool Emit(SboxTarget *target, const char *marker) {
  printf("[trust-transition-target] %s\n", marker);
  return sbox_target_post_message(target, SBOX_MSG_STRING, marker, std::strlen(marker)) == 0;
}

bool QueryStrictAcg() {
  PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy = {};
  return ::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessDynamicCodePolicy, &policy, sizeof(policy)) &&
      policy.ProhibitDynamicCode && !policy.AllowThreadOptOut && !policy.AllowRemoteDowngrade;
}

bool QueryTokenBuffer(HANDLE token, TOKEN_INFORMATION_CLASS kind, std::vector<BYTE> &buffer) {
  DWORD bytes = 0;
  if (::GetTokenInformation(token, kind, nullptr, 0, &bytes) || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
      !bytes) {
    printf("[policy-target] token size query failed: class=%d error=%lu\n", kind, ::GetLastError());
    return false;
  }
  buffer.resize(bytes);
  if (!::GetTokenInformation(token, kind, buffer.data(), bytes, &bytes)) {
    printf("[policy-target] token query failed: class=%d error=%lu\n", kind, ::GetLastError());
    return false;
  }
  return true;
}

bool HasGroup(const TOKEN_GROUPS &groups, PSID sid) {
  for (DWORD i = 0; i < groups.GroupCount; ++i) {
    if (::EqualSid(groups.Groups[i].Sid, sid))
      return true;
  }
  return false;
}

bool TokenIntegrity(HANDLE token, DWORD &integrity) {
  std::vector<BYTE> buffer;
  if (!QueryTokenBuffer(token, TokenIntegrityLevel, buffer))
    return false;
  const auto *label = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(buffer.data());
  integrity =
      *::GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(*::GetSidSubAuthorityCount(label->Label.Sid) - 1));
  return true;
}

bool VerifyPolicyTokens(const SboxTrustTransitionPolicyExpectation &expected, bool final) {
  HANDLE process_token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &process_token)) {
    printf("[policy-target] OpenProcessToken failed: error=%lu\n", ::GetLastError());
    return false;
  }
  DWORD app_container = 0;
  DWORD bytes = 0;
  DWORD integrity = 0;
  std::vector<BYTE> restricted;
  bool ok = ::GetTokenInformation(process_token, TokenIsAppContainer, &app_container, sizeof(app_container), &bytes) &&
      TokenIntegrity(process_token, integrity) && QueryTokenBuffer(process_token, TokenRestrictedSids, restricted);
  const int32_t wanted_integrity = final ? expected.delayed_integrity : expected.initial_integrity;
  const DWORD wanted_rid =
      wanted_integrity == SBOX_INTEGRITY_LOW ? SECURITY_MANDATORY_LOW_RID : SECURITY_MANDATORY_UNTRUSTED_RID;
  ok = ok && app_container == static_cast<DWORD>(expected.app_container) && integrity == wanted_rid;

  if (ok && !expected.app_container) {
    const auto *groups = reinterpret_cast<const TOKEN_GROUPS *>(restricted.data());
    printf(
        "[policy-target] restricted_sids=%lu null_sid=%d\n",
        groups->GroupCount,
        groups->GroupCount == 1 && ::IsWellKnownSid(groups->Groups[0].Sid, WinNullSid));
    ok = groups->GroupCount == 1 && ::IsWellKnownSid(groups->Groups[0].Sid, WinNullSid);
  }

  if (ok && expected.app_container) {
    std::vector<BYTE> package;
    std::vector<BYTE> capabilities;
    std::vector<BYTE> groups;
    PSID package_sid = nullptr;
    PSID capability_sid = nullptr;
    PSID all_apps_sid = nullptr;
    ok = ::ConvertStringSidToSidW(expected.package_sid, &package_sid) &&
        ::ConvertStringSidToSidW(kSboxTrustTransitionCapability, &capability_sid) &&
        ::ConvertStringSidToSidW(L"S-1-15-2-1", &all_apps_sid) &&
        QueryTokenBuffer(process_token, TokenAppContainerSid, package) &&
        QueryTokenBuffer(process_token, TokenCapabilities, capabilities) &&
        QueryTokenBuffer(process_token, TokenGroups, groups);
    if (ok) {
      const auto *actual_package = reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION *>(package.data());
      const auto *actual_capabilities = reinterpret_cast<const TOKEN_GROUPS *>(capabilities.data());
      const auto *actual_groups = reinterpret_cast<const TOKEN_GROUPS *>(groups.data());
      printf(
          "[policy-target] package_match=%d capabilities=%lu capability_match=%d "
          "all_apps=%d\n",
          actual_package->TokenAppContainer && ::EqualSid(actual_package->TokenAppContainer, package_sid),
          actual_capabilities->GroupCount,
          HasGroup(*actual_capabilities, capability_sid),
          HasGroup(*actual_groups, all_apps_sid));
      ok = actual_package->TokenAppContainer && ::EqualSid(actual_package->TokenAppContainer, package_sid) &&
          actual_capabilities->GroupCount == 1 && HasGroup(*actual_capabilities, capability_sid);
    } else {
      printf("[policy-target] profile token query failed: error=%lu\n", ::GetLastError());
    }
    if (package_sid)
      ::LocalFree(package_sid);
    if (capability_sid)
      ::LocalFree(capability_sid);
    if (all_apps_sid)
      ::LocalFree(all_apps_sid);
  }
  ::CloseHandle(process_token);

  HANDLE thread_token = nullptr;
  const bool impersonating = ::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, FALSE, &thread_token) != FALSE;
  const DWORD thread_error = impersonating ? ERROR_SUCCESS : ::GetLastError();
  if (final || expected.app_container) {
    ok = ok && !impersonating && thread_error == ERROR_NO_TOKEN;
  } else {
    DWORD thread_integrity = 0;
    ok = ok && impersonating && TokenIntegrity(thread_token, thread_integrity) && thread_integrity == wanted_rid;
  }
  if (thread_token)
    ::CloseHandle(thread_token);
  printf(
      "[policy-target] tokens phase=%s appcontainer=%lu lpac=%d integrity=0x%lx "
      "thread_token=%d thread_error=%lu -> %s\n",
      final ? "final" : "initial",
      app_container,
      expected.lpac,
      integrity,
      impersonating,
      thread_error,
      ok ? "PASS" : "FAIL");
  return ok;
}

bool CheckFixtureRead(const wchar_t *directory, const wchar_t *name, bool expected) {
  const std::wstring path = std::wstring(directory) + L"\\" + name;
  HANDLE file = ::CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  const bool opened = file != INVALID_HANDLE_VALUE;
  const DWORD error = opened ? ERROR_SUCCESS : ::GetLastError();
  if (opened)
    ::CloseHandle(file);
  const bool ok = opened == expected && (opened || error == ERROR_ACCESS_DENIED);
  printf(
      "[policy-target] resource=%ls expected=%s actual=%s error=%lu -> %s\n",
      name,
      expected ? "allowed" : "denied",
      opened ? "allowed" : "denied",
      error,
      ok ? "PASS" : "FAIL");
  return ok;
}

bool VerifyPolicyResources(const SboxTrustTransitionPolicyExpectation &expected) {
  const bool allowed = CheckFixtureRead(expected.fixture_directory, L"allowed.txt", true);
  const bool denied = CheckFixtureRead(expected.fixture_directory, L"denied.txt", false);
  const bool package = CheckFixtureRead(expected.fixture_directory, L"package.txt", expected.app_container != 0);
  // ALL APPLICATION PACKAGES can be implicit rather than listed in TokenGroups.
  // Test its actual access effect to distinguish AppContainer from LPAC.
  const bool all_apps =
      CheckFixtureRead(expected.fixture_directory, L"all-apps.txt", expected.app_container && !expected.lpac);
  return allowed && denied && package && all_apps;
}

bool VerifyLpacLow() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
    return false;
  DWORD app_container = 0;
  DWORD bytes = 0;
  TOKEN_MANDATORY_LABEL label = {};
  const bool ok = ::GetTokenInformation(token, TokenIsAppContainer, &app_container, sizeof(app_container), &bytes) &&
      ::GetTokenInformation(token, TokenIntegrityLevel, &label, sizeof(label), &bytes) == FALSE &&
      ::GetLastError() == ERROR_INSUFFICIENT_BUFFER;
  DWORD label_bytes = bytes;
  auto *label_buffer = new BYTE[label_bytes];
  const bool got_label = ::GetTokenInformation(token, TokenIntegrityLevel, label_buffer, label_bytes, &bytes) != FALSE;
  DWORD integrity = SECURITY_MANDATORY_HIGH_RID;
  wchar_t *integrity_sid = nullptr;
  if (got_label) {
    auto *actual = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(label_buffer);
    integrity =
        *::GetSidSubAuthority(actual->Label.Sid, static_cast<DWORD>(*::GetSidSubAuthorityCount(actual->Label.Sid) - 1));
    ::ConvertSidToStringSidW(actual->Label.Sid, &integrity_sid);
  }

  DWORD groups_bytes = 0;
  ::GetTokenInformation(token, TokenGroups, nullptr, 0, &groups_bytes);
  auto *groups_buffer = new BYTE[groups_bytes];
  const bool got_groups = ::GetTokenInformation(token, TokenGroups, groups_buffer, groups_bytes, &bytes) != FALSE;
  PSID all_apps = nullptr;
  PSID expected_capability = nullptr;
  ::ConvertStringSidToSidW(L"S-1-15-2-1", &all_apps);
  ::ConvertStringSidToSidW(kSboxTrustTransitionCapability, &expected_capability);
  bool has_all_apps = false;
  if (got_groups && all_apps) {
    auto *groups = reinterpret_cast<TOKEN_GROUPS *>(groups_buffer);
    for (DWORD i = 0; i < groups->GroupCount; ++i)
      has_all_apps |= ::EqualSid(groups->Groups[i].Sid, all_apps) != FALSE;
  }

  DWORD capabilities_bytes = 0;
  ::GetTokenInformation(token, TokenCapabilities, nullptr, 0, &capabilities_bytes);
  auto *capabilities_buffer = new BYTE[capabilities_bytes];
  const bool got_capabilities =
      ::GetTokenInformation(token, TokenCapabilities, capabilities_buffer, capabilities_bytes, &bytes) != FALSE;
  bool has_capability = false;
  if (got_capabilities && expected_capability) {
    auto *capabilities = reinterpret_cast<TOKEN_GROUPS *>(capabilities_buffer);
    for (DWORD i = 0; i < capabilities->GroupCount; ++i) {
      has_capability |= ::EqualSid(capabilities->Groups[i].Sid, expected_capability) != FALSE;
    }
  }
  delete[] label_buffer;
  delete[] groups_buffer;
  delete[] capabilities_buffer;
  ::CloseHandle(token);
  printf(
      "[trust-transition-target] lpac=%d app_container=%lu integrity=0x%lx sid=%ls "
      "all_apps=%s capability=%s\n",
      !has_all_apps,
      app_container,
      integrity,
      integrity_sid ? integrity_sid : L"(query-failed)",
      has_all_apps ? "present" : "absent",
      has_capability ? "present" : "missing");
  if (integrity_sid)
    ::LocalFree(integrity_sid);
  if (all_apps)
    ::LocalFree(all_apps);
  if (expected_capability)
    ::LocalFree(expected_capability);
  return ok && got_label && got_groups && got_capabilities && app_container == 1 && !has_all_apps && has_capability &&
      integrity <= SECURITY_MANDATORY_LOW_RID;
}

bool VerifyThunkRx() {
  SboxTrustTransitionThunkTelemetry telemetry = {};
  if (!sbox_trust_transition_test_get_thunk_telemetry(&telemetry) || !telemetry.rx_complete || !telemetry.base ||
      !telemetry.used_bytes || telemetry.used_bytes > telemetry.allocated_bytes) {
    return false;
  }
  MEMORY_BASIC_INFORMATION mbi = {};
  if (!::VirtualQuery(reinterpret_cast<void *>(telemetry.base), &mbi, sizeof(mbi))) {
    return false;
  }
  const uintptr_t end = telemetry.base + telemetry.used_bytes;
  const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  const bool ok = mbi.State == MEM_COMMIT && mbi.Protect == PAGE_EXECUTE_READ && end <= region_end;
  printf(
      "[trust-transition-target] thunk base=0x%llx allocated=%zu used=%zu installed=%zu "
      "rx=%d protect=0x%lx region=%zu\n",
      static_cast<unsigned long long>(telemetry.base),
      telemetry.allocated_bytes,
      telemetry.used_bytes,
      telemetry.installed_hooks,
      telemetry.rx_complete,
      mbi.Protect,
      mbi.RegionSize);
  return ok;
}

struct RequestState {
  SboxTarget *target;
  uint32_t case_id;
  bool complete;
};

void OnRequest(void *context, int kind, const void *data, size_t length) {
  auto *state = static_cast<RequestState *>(context);
  if (state->complete || kind != SBOX_MSG_STRING || length != 7 || std::memcmp(data, "REQUEST", 7) != 0) {
    return;
  }
  char marker[80] = {};
  std::snprintf(marker, sizeof(marker), "HOST_OPERATION case=%u", state->case_id);
  if (!Emit(state->target, marker))
    return;
  std::snprintf(marker, sizeof(marker), "RESULT case=%u", state->case_id);
  state->complete = Emit(state->target, marker);
}

int RunWorker() {
  const SboxTrustTransitionTestControl control = g_sbox_trust_transition_test_control;
  printf(
      "[trust-transition-target] selected=%u serial=%llu\n",
      control.case_id,
      static_cast<unsigned long long>(control.serial));
  if (!sbox_trust_transition_test_initialize(&control)) {
    printf("[trust-transition-target] stage=control-validation result=invalid\n");
    return 40;
  }
  printf(
      "[trust-transition-target] consumed-control=%u serial=%llu\n",
      control.case_id,
      static_cast<unsigned long long>(control.serial));

  SboxTarget *target = sbox_target_begin();
  if (!target) {
    printf("[trust-transition-target] stage=target-begin result=null\n");
    return 41;
  }
  if (!sbox_target_test_ipc(target)) {
    printf("[trust-transition-target] stage=pre-lockdown-ipc result=failed\n");
    return 42;
  }
  printf("[trust-transition-target] pre-lockdown-ipc=ok\n");

  const bool policy_case = control.case_id == SBOX_TRUST_TRANSITION_CASE_POLICY_SUCCESS;
  if (policy_case && !VerifyPolicyTokens(control.policy, false)) {
    printf("[policy-target] stage=initial-token-contract result=failed\n");
    return 50;
  }

  const bool verify_unused_file_hooks = control.case_id == SBOX_TRUST_TRANSITION_CASE_UNUSED_FILE_HOOK_SUCCESS;
  FileHookCode file_code_before = {};
  if (verify_unused_file_hooks && !ReadFileHookCode(file_code_before))
    return 53;

  const int transition = sbox_target_lower_token(target);
  if (transition != 0) {
    printf("[trust-transition-target] stage=target-transition result=%d\n", transition);
    ::TerminateProcess(::GetCurrentProcess(), kTransitionExit);
  }
  printf("[trust-transition-target] final-lockdown=ok\n");

  if (!sbox_target_test_ipc(target)) {
    printf("[trust-transition-target] stage=post-lockdown-ipc result=failed\n");
    return 43;
  }
  printf("[trust-transition-target] post-lockdown-ipc=ok\n");

  const bool strict_expected = control.case_id != SBOX_TRUST_TRANSITION_CASE_ACG_OFF_SUCCESS;
  printf("[trust-transition-target] strict_acg_expected=%s\n", strict_expected ? "true" : "false");
  if (strict_expected && !QueryStrictAcg()) {
    printf("[trust-transition-target] stage=target-acg-validation result=failed\n");
    return 44;
  }
  if (control.case_id == SBOX_TRUST_TRANSITION_CASE_LPAC_SUCCESS && !VerifyLpacLow()) {
    printf("[trust-transition-target] stage=lpac-low-validation result=failed\n");
    return 45;
  }
  if (!VerifyThunkRx()) {
    printf("[trust-transition-target] stage=thunk-telemetry result=failed\n");
    return 46;
  }
  if (verify_unused_file_hooks) {
    FileHookCode file_code_after = {};
    if (!ReadFileHookCode(file_code_after) || file_code_before != file_code_after) {
      printf("[trust-transition-target] stage=unused-file-hooks result=changed\n");
      return 54;
    }
    printf("[trust-transition-target] unused-file-hooks=unchanged\n");
  }
  if (policy_case && (!VerifyPolicyTokens(control.policy, true) || !VerifyPolicyResources(control.policy))) {
    printf("[policy-target] stage=final-policy-contract result=failed\n");
    return 52;
  }

  char marker[80] = {};
  std::snprintf(marker, sizeof(marker), "SECURITY_READY case=%u", control.case_id);
  if (!Emit(target, marker))
    return 47;
  std::snprintf(marker, sizeof(marker), "GUEST_ENTRY case=%u", control.case_id);
  if (!Emit(target, marker))
    return 48;

  RequestState state{target, control.case_id, false};
  HANDLE waits[] = {
      reinterpret_cast<HANDLE>(sbox_target_inbound_event(target)),
      reinterpret_cast<HANDLE>(sbox_target_close_event(target)),
  };
  const DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, 10000);
  if (wait == WAIT_OBJECT_0)
    sbox_target_drain_messages(target, &OnRequest, &state);
  if (!state.complete) {
    printf("[trust-transition-target] stage=synthetic-protocol result=failed wait=%lu\n", wait);
    return 49;
  }
  sbox_target_end(target);
  return 0;
}

// ===========================================================================
// Broker / case-runner role — was sbox_trust_transition_tests.cc.
// ===========================================================================

constexpr wchar_t kLpacProfile[] = L"V8Jsi.Sbox.TrustTransition.Test";

struct CaseSpec {
  uint32_t id;
  const char *name;
  const char *stage;
  DWORD exit_code;
  bool success;
  bool lpac;
  bool acg;
  bool file_brokering = false;
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
    {SBOX_TRUST_TRANSITION_CASE_REQUIRED_FILE_HOOK_FAILURE,
     "required-file-hook-failure",
     "hook-install",
     23,
     false,
     false,
     true,
     true},
    {SBOX_TRUST_TRANSITION_CASE_UNUSED_FILE_HOOK_SUCCESS, "unused-file-hook-success", nullptr, 0, true, false, true},
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

// Single-image topology: the worker IS this exe. Return our own full path; the
// broker appends --worker to select the worker persona.
std::wstring TargetPath() {
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::wstring(path);
}

SboxPolicy MakePolicy(const CaseSpec &spec) {
  static const wchar_t *const capabilities[] = {
      kSboxTrustTransitionCapability,
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
  const std::wstring file_probe = TargetPath();
  const SboxFileRule file_rule{file_probe.c_str(), 1};
  if (spec.file_brokering) {
    policy.file_rules = &file_rule;
    policy.file_rule_count = 1;
  }
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
  ok = ok &&
      Contains(
           output,
           spec.file_brokering ? "interception plan: file_brokering=1 hooks=12"
                               : "interception plan: file_brokering=0 hooks=7");
  if (spec.success) {
    ok = ok && ready == 1 && guest == 1 && host == 1 && result == 1 && Contains(output, "pre-lockdown-ipc=ok") &&
        Contains(output, "final-lockdown=ok") && Contains(output, "post-lockdown-ipc=ok") &&
        Contains(output, spec.file_brokering ? "installed=12 rx=1 protect=0x20" : "installed=7 rx=1 protect=0x20");
    ok = ok && Contains(output, spec.acg ? "strict_acg_expected=true" : "strict_acg_expected=false");
    if (spec.lpac)
      ok = ok && Contains(output, "lpac=1 app_container=1") && Contains(output, "capability=present");
    if (spec.id == SBOX_TRUST_TRANSITION_CASE_UNUSED_FILE_HOOK_SUCCESS)
      ok = ok && Contains(output, "unused-file-hooks=unchanged") && !Contains(output, "[trust-transition] consumed=13");
  } else {
    char consumed[32] = {};
    std::snprintf(consumed, sizeof(consumed), "consumed=%u", spec.id);
    ok = ok && spec.stage && Contains(output, spec.stage) && Contains(output, consumed) && ready == 0 && guest == 0 &&
        host == 0 && result == 0;
    if (spec.id == SBOX_TRUST_TRANSITION_CASE_FIRST_HOOK_FAILURE)
      ok = ok && Contains(output, "installed=0");
    if (spec.id == SBOX_TRUST_TRANSITION_CASE_PARTIAL_HOOK_FAILURE)
      ok = ok && Contains(output, "installed=1");
    if (spec.id == SBOX_TRUST_TRANSITION_CASE_REQUIRED_FILE_HOOK_FAILURE)
      ok = ok && Contains(output, "hook=NtCreateFile index=7 installed=7") && Contains(output, "ntstatus=0xc0000035");
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

struct PolicyFixture {
  std::wstring directory;
  std::wstring profile;
  std::wstring package_sid;
  std::wstring user_sid;
  std::vector<std::wstring> files;
  bool directory_created = false;
  bool profile_created = false;

  std::wstring Descriptor(const wchar_t *principal, bool directory_access) const {
    std::wstring result = L"D:P(A;;FA;;;SY)(A;;FA;;;" + user_sid + L")";
    if (principal && principal[0]) {
      result += directory_access ? L"(A;;GRGX;;;" : L"(A;;GR;;;";
      result += principal;
      result += L")";
    }
    return result;
  }

  bool MakeFile(const wchar_t *name, const wchar_t *principal) {
    const std::wstring path = directory + L"\\" + name;
    const std::wstring sddl = Descriptor(principal, false);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
      printf("[policy-tests] file descriptor failed: error=%lu\n", ::GetLastError());
      return false;
    }
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    HANDLE file = ::CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD error = ::GetLastError();
    ::LocalFree(descriptor);
    if (file == INVALID_HANDLE_VALUE) {
      printf("[policy-tests] fixture file creation failed: error=%lu\n", error);
      return false;
    }
    files.push_back(path);
    const char data[] = "sandbox policy fixture\n";
    DWORD wrote = 0;
    const bool ok = ::WriteFile(file, data, sizeof(data) - 1, &wrote, nullptr) && wrote == sizeof(data) - 1;
    if (!ok)
      printf("[policy-tests] fixture write failed: error=%lu\n", ::GetLastError());
    ::CloseHandle(file);
    return ok;
  }

  bool Initialize(bool app_container, uint64_t serial) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
      printf("[policy-tests] host token query failed: error=%lu\n", ::GetLastError());
      return false;
    }
    DWORD bytes = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> user(bytes);
    const bool got_user = bytes && ::GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes);
    ::CloseHandle(token);
    wchar_t *text = nullptr;
    if (!got_user || !::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, &text)) {
      printf("[policy-tests] host SID query failed: error=%lu\n", ::GetLastError());
      return false;
    }
    user_sid = text;
    ::LocalFree(text);

    const std::wstring suffix = std::to_wstring(::GetCurrentProcessId()) + L"." + std::to_wstring(serial);
    if (app_container) {
      profile = L"V8Jsi.Sbox.Policy.Test." + suffix;
      PSID sid = nullptr;
      const HRESULT result = ::CreateAppContainerProfile(
          profile.c_str(), L"Sandbox policy test", L"Temporary policy test fixture", nullptr, 0, &sid);
      if (FAILED(result)) {
        printf("[policy-tests] fixture profile creation failed: hr=0x%08lx\n", static_cast<unsigned long>(result));
        return false;
      }
      profile_created = true;
      const bool converted = ::ConvertSidToStringSidW(sid, &text) != FALSE;
      ::FreeSid(sid);
      if (!converted) {
        printf("[policy-tests] package SID conversion failed: error=%lu\n", ::GetLastError());
        return false;
      }
      package_sid = text;
      ::LocalFree(text);
    }

    wchar_t temp[MAX_PATH] = {};
    const DWORD length = ::GetTempPathW(std::size(temp), temp);
    if (!length || length >= std::size(temp)) {
      printf("[policy-tests] temporary path query failed: error=%lu\n", ::GetLastError());
      return false;
    }
    directory = std::wstring(temp, length) + L"v8jsi-sbox-policy-" + suffix;
    const std::wstring sddl = Descriptor(app_container ? kSboxTrustTransitionCapability : nullptr, true);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
      printf("[policy-tests] directory descriptor failed: error=%lu\n", ::GetLastError());
      return false;
    }
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    directory_created = ::CreateDirectoryW(directory.c_str(), &attributes) != FALSE;
    const DWORD error = ::GetLastError();
    ::LocalFree(descriptor);
    if (!directory_created) {
      printf("[policy-tests] fixture directory creation failed: error=%lu\n", error);
      return false;
    }
    return MakeFile(L"allowed.txt", app_container ? kSboxTrustTransitionCapability : nullptr) &&
        MakeFile(L"denied.txt", nullptr) && MakeFile(L"package.txt", app_container ? package_sid.c_str() : nullptr) &&
        MakeFile(L"all-apps.txt", L"S-1-15-2-1");
  }

  bool Cleanup() {
    bool ok = true;
    for (const auto &file : files) {
      if (!::DeleteFileW(file.c_str())) {
        printf("[policy-tests] fixture file cleanup failed: error=%lu\n", ::GetLastError());
        ok = false;
      }
    }
    files.clear();
    if (directory_created && !::RemoveDirectoryW(directory.c_str())) {
      printf("[policy-tests] fixture directory cleanup failed: error=%lu\n", ::GetLastError());
      ok = false;
    }
    directory_created = false;
    if (profile_created) {
      const HRESULT result = ::DeleteAppContainerProfile(profile.c_str());
      if (FAILED(result)) {
        printf("[policy-tests] fixture profile cleanup failed: hr=0x%08lx\n", static_cast<unsigned long>(result));
        ok = false;
      }
    }
    profile_created = false;
    return ok;
  }
};

struct PolicySpec {
  const char *name;
  bool app_container;
  bool lpac;
  int32_t delayed_integrity;
  bool inactive_tokens = false;
  bool extended_size = false;
  uint32_t platform = SBOX_TRUST_TRANSITION_PLATFORM_CURRENT;
  bool unknown_initial_token = false;
};

bool RunPolicyCase(const PolicySpec &spec, uint64_t serial) {
  sbox_trust_transition_test_reset_broker();
  PolicyFixture fixture;
  if (!fixture.Initialize(spec.app_container, serial)) {
    fixture.Cleanup();
    return false;
  }
  struct ExtendedPolicy {
    SboxPolicy policy;
    uint64_t future_fields[4] = {};
  } extended = {};
  SboxPolicy &policy = extended.policy;
  policy = MakePolicy(kCases[7]);
  policy.struct_size = spec.extended_size ? sizeof(extended) : sizeof(policy);
  policy.use_app_container = spec.app_container;
  policy.low_privilege_app_container = spec.lpac;
  policy.delayed_integrity = spec.delayed_integrity;
  const wchar_t *capabilities[] = {kSboxTrustTransitionCapability};
  policy.app_container_profile_name = fixture.profile.c_str();
  policy.capabilities = spec.app_container ? capabilities : nullptr;
  policy.capability_count = spec.app_container ? 1 : 0;
  const std::wstring allowed_path = fixture.directory + L"\\allowed.txt";
  const SboxFileRule allowed_rule{allowed_path.c_str(), 1};
  policy.file_rules = spec.app_container ? nullptr : &allowed_rule;
  policy.file_rule_count = spec.app_container ? 0 : 1;
  if (spec.inactive_tokens) {
    policy.initial_token = SBOX_TOKEN_LOCKDOWN;
    policy.lockdown_token = SBOX_TOKEN_RESTRICTED_SAME_ACCESS;
  }
  if (spec.unknown_initial_token)
    policy.initial_token = -1;

  SboxTrustTransitionPolicyExpectation expected = {};
  expected.initial_integrity = policy.integrity;
  expected.delayed_integrity = policy.delayed_integrity;
  expected.app_container = spec.app_container;
  expected.lpac = spec.lpac;
  if (fixture.directory.size() >= std::size(expected.fixture_directory) ||
      fixture.package_sid.size() >= std::size(expected.package_sid)) {
    printf("[policy-tests] fixture path or SID exceeds control capacity\n");
    fixture.Cleanup();
    return false;
  }
  std::wcscpy(expected.fixture_directory, fixture.directory.c_str());
  std::wcscpy(expected.package_sid, fixture.package_sid.c_str());
  bool ok = sbox_trust_transition_test_set_next_policy_case(&expected, serial) &&
      sbox_trust_transition_test_set_platform(spec.platform);
  if (spec.inactive_tokens)
    ok = sbox_trust_transition_test_set_broker_fault(SBOX_TRUST_TRANSITION_BROKER_TOKEN_FAILURE) && ok;

  Messages messages;
  SboxSession *session = ok ? sbox_broker_spawn(TargetPath().c_str(), &policy, &OnMessage, &messages) : nullptr;
  if (!session) {
    printf("[policy-tests] case=%s spawn failed\n", spec.name);
    fixture.Cleanup();
    sbox_trust_transition_test_reset_broker();
    return false;
  }
  const char request[] = "REQUEST";
  ok = sbox_broker_post_message(session, SBOX_MSG_STRING, request, sizeof(request) - 1) == 0 && ok;
  const int exit_code = sbox_broker_wait(session);
  const std::string output = sbox_trust_transition_test_last_output();
  std::vector<std::string> snapshot;
  {
    std::lock_guard<std::mutex> lock(messages.mutex);
    snapshot = messages.values;
  }
  const uint32_t case_id = SBOX_TRUST_TRANSITION_CASE_POLICY_SUCCESS;
  const size_t ready = Count(snapshot, "SECURITY_READY", case_id);
  const size_t guest = Count(snapshot, "GUEST_ENTRY", case_id);
  const size_t host = Count(snapshot, "HOST_OPERATION", case_id);
  const size_t result = Count(snapshot, "RESULT", case_id);
  ok = ok && exit_code == 0 && ready == 1 && guest == 1 && host == 1 && result == 1 &&
      Contains(output, "tokens phase=initial") && Contains(output, "tokens phase=final") &&
      Contains(output, "post-lockdown-ipc=ok") && Contains(output, "resource=denied.txt") &&
      Contains(output, "resource=all-apps.txt");
  ok = ok &&
      Contains(
           output,
           policy.file_rule_count ? "interception plan: file_brokering=1 hooks=12"
                                  : "interception plan: file_brokering=0 hooks=7") &&
      Contains(output, policy.file_rule_count ? "installed=12 rx=1 protect=0x20" : "installed=7 rx=1 protect=0x20");
  if (spec.inactive_tokens) {
    ok = ok && sbox_trust_transition_test_pending_broker_fault() == SBOX_TRUST_TRANSITION_BROKER_TOKEN_FAILURE &&
        Contains(output, "do not apply; no restricted-default-DACL guarantee");
  }
  ok = fixture.Cleanup() && ok;
  sbox_trust_transition_test_reset_broker();
  printf(
      "[policy-tests] case=%s exit=%d markers=%zu/%zu/%zu/%zu -> %s\n",
      spec.name,
      exit_code,
      ready,
      guest,
      host,
      result,
      ok ? "PASS" : "FAIL");
  return ok;
}

bool ExpectPolicyRejected(
    const char *name,
    const wchar_t *target,
    const SboxPolicy *policy,
    const char *reason,
    uint32_t platform = SBOX_TRUST_TRANSITION_PLATFORM_CURRENT,
    uint32_t fault = SBOX_TRUST_TRANSITION_BROKER_NONE) {
  sbox_trust_transition_test_reset_broker();
  if (!sbox_trust_transition_test_set_platform(platform) || !sbox_trust_transition_test_set_broker_fault(fault)) {
    printf("[policy-tests] case=%s invalid test setup\n", name);
    return false;
  }
  Messages messages;
  SboxSession *session = sbox_broker_spawn(target, policy, &OnMessage, &messages);
  const DWORD error = ::GetLastError();
  bool ok = !session;
  if (session) {
    sbox_broker_close(session);
    sbox_broker_wait(session);
  }
  const std::string output = sbox_trust_transition_test_last_output();
  ok = ok && Contains(output, reason);
  if (platform == SBOX_TRUST_TRANSITION_PLATFORM_PRE_RS5 || platform == SBOX_TRUST_TRANSITION_PLATFORM_UNKNOWN) {
    ok = ok && error == ERROR_OLD_WIN_VERSION;
  } else if (fault == SBOX_TRUST_TRANSITION_BROKER_NONE) {
    ok = ok && error == ERROR_INVALID_PARAMETER;
  } else {
    ok = ok && sbox_trust_transition_test_pending_broker_fault() == SBOX_TRUST_TRANSITION_BROKER_NONE;
  }
  {
    std::lock_guard<std::mutex> lock(messages.mutex);
    ok = ok && messages.values.empty();
  }
  sbox_trust_transition_test_reset_broker();
  printf("[policy-tests] case=%s rejected=%d messages=0 -> %s\n", name, !session, ok ? "PASS" : "FAIL");
  return ok;
}

bool RunPolicyTests() {
  const std::wstring target = TargetPath();
  const SboxPolicy ordinary = MakePolicy(kCases[7]);
  bool ok = true;
  constexpr const char *platform_cases[] = {"pre-rs5-restricted", "pre-rs5-appcontainer", "pre-rs5-lpac"};
  for (int mode = 0; mode < 3; ++mode) {
    SboxPolicy policy = ordinary;
    policy.use_app_container = mode != 0;
    policy.low_privilege_app_container = mode == 2;
    ok = ExpectPolicyRejected(
             platform_cases[mode], target.c_str(), &policy, "stage=platform", SBOX_TRUST_TRANSITION_PLATFORM_PRE_RS5) &&
        ok;
  }
  ok = ExpectPolicyRejected(
           "unknown-platform", target.c_str(), &ordinary, "stage=platform", SBOX_TRUST_TRANSITION_PLATFORM_UNKNOWN) &&
      ok;
  ok = ExpectPolicyRejected("null-policy", target.c_str(), nullptr, "missing target path or policy") && ok;
  ok = ExpectPolicyRejected("null-target", nullptr, &ordinary, "missing target path or policy") && ok;
  ok = ExpectPolicyRejected("empty-target", L"", &ordinary, "missing target path or policy") && ok;

  for (uint32_t size :
       {0u,
        static_cast<uint32_t>(offsetof(SboxPolicy, use_app_container)),
        static_cast<uint32_t>(sizeof(SboxPolicy) - 1)}) {
    SboxPolicy policy = ordinary;
    policy.struct_size = size;
    ok = ExpectPolicyRejected("undersized-policy", target.c_str(), &policy, "incompatible SboxPolicy size") && ok;
  }
  SboxPolicy policy = ordinary;
  policy.low_privilege_app_container = 1;
  ok = ExpectPolicyRejected("lpac-without-appcontainer", target.c_str(), &policy, "LPAC requires AppContainer") && ok;
  policy = ordinary;
  policy.initial_token = SBOX_TOKEN_LOCKDOWN;
  policy.lockdown_token = SBOX_TOKEN_RESTRICTED_SAME_ACCESS;
  ok = ExpectPolicyRejected("invalid-ordinary-token-pair", target.c_str(), &policy, "initial token") && ok;
  policy = ordinary;
  policy.integrity = -1;
  ok = ExpectPolicyRejected("unknown-initial-integrity", target.c_str(), &policy, "invalid integrity") && ok;
  policy = ordinary;
  policy.delayed_integrity = -1;
  ok = ExpectPolicyRejected("unknown-final-integrity", target.c_str(), &policy, "invalid integrity") && ok;
  policy = ordinary;
  policy.use_app_container = 1;
  policy.integrity = SBOX_INTEGRITY_UNTRUSTED;
  ok = ExpectPolicyRejected("profile-initial-untrusted", target.c_str(), &policy, "profile initial integrity") && ok;
  policy = ordinary;
  policy.use_app_container = 1;
  policy.app_container_profile_name = nullptr;
  ok = ExpectPolicyRejected("missing-profile", target.c_str(), &policy, "missing AppContainer profile") && ok;
  policy.app_container_profile_name = L"";
  ok = ExpectPolicyRejected("empty-profile", target.c_str(), &policy, "missing AppContainer profile") && ok;
  policy.app_container_profile_name = kLpacProfile;
  policy.capability_count = 1;
  policy.capabilities = nullptr;
  ok = ExpectPolicyRejected("missing-capability-array", target.c_str(), &policy, "capability array") && ok;
  const wchar_t *empty_capabilities[] = {nullptr};
  policy.capabilities = empty_capabilities;
  ok = ExpectPolicyRejected("null-capability", target.c_str(), &policy, "empty capability") && ok;
  empty_capabilities[0] = L"";
  ok = ExpectPolicyRejected("empty-capability", target.c_str(), &policy, "empty capability") && ok;
  policy = ordinary;
  policy.file_rule_count = 1;
  policy.file_rules = nullptr;
  ok = ExpectPolicyRejected("missing-file-array", target.c_str(), &policy, "missing file-rule array") && ok;
  SboxFileRule empty_rule{nullptr, 1};
  policy.file_rules = &empty_rule;
  ok = ExpectPolicyRejected("null-file-rule", target.c_str(), &policy, "empty file rule") && ok;
  empty_rule.pattern = L"";
  ok = ExpectPolicyRejected("empty-file-rule", target.c_str(), &policy, "empty file rule") && ok;
  ok = ExpectPolicyRejected(
           "ordinary-token-creation-failure",
           target.c_str(),
           &ordinary,
           "SpawnTargetAsync failed",
           SBOX_TRUST_TRANSITION_PLATFORM_CURRENT,
           SBOX_TRUST_TRANSITION_BROKER_TOKEN_FAILURE) &&
      ok;
  // Retired with the two-image topology: the former
  // "non-hosted-dll-linkage" case drove SBOX_TRUST_TRANSITION_BROKER_NON_HOSTED
  // to prove the broker rejects a target whose sandbox core is a SEPARATE,
  // non-hosted DLL (SBOX_ERROR_INVALID_LINK_STATE / rc=64). That guard
  // (broker_services.cc: !g_sbox_hosted_mode && CURRENT_MODULE() !=
  // GetModuleHandleW(nullptr)) fires only when the core is in its own module; in
  // the single statically-linked image the core IS the EXE, so the guard
  // correctly treats same-image as the valid state and the case is inapplicable.
  // The guard itself is unchanged and still fails closed for a non-hosted DLL.

  DWORD handles_before = 0;
  DWORD handles_after = 0;
  bool cleanup_ok = ::GetProcessHandleCount(::GetCurrentProcess(), &handles_before) != FALSE;
  for (int attempt = 0; attempt < 16; ++attempt) {
    cleanup_ok = ExpectPolicyRejected(
                     "failed-spawn-cleanup",
                     target.c_str(),
                     &ordinary,
                     "SpawnTargetAsync failed",
                     SBOX_TRUST_TRANSITION_PLATFORM_CURRENT,
                     SBOX_TRUST_TRANSITION_BROKER_TOKEN_FAILURE) &&
        cleanup_ok;
  }
  cleanup_ok =
      ::GetProcessHandleCount(::GetCurrentProcess(), &handles_after) && cleanup_ok && handles_before == handles_after;
  printf(
      "[policy-tests] failed-spawn handles before=%lu after=%lu -> %s\n",
      handles_before,
      handles_after,
      cleanup_ok ? "PASS" : "FAIL");
  ok = cleanup_ok && ok;

  SYSTEM_INFO system = {};
  ::GetSystemInfo(&system);
  auto *guarded =
      static_cast<BYTE *>(::VirtualAlloc(nullptr, 2 * system.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!guarded) {
    printf("[policy-tests] short-policy guard allocation failed: error=%lu\n", ::GetLastError());
    ok = false;
  } else {
    DWORD old_protection = 0;
    if (!::VirtualProtect(guarded + system.dwPageSize, system.dwPageSize, PAGE_NOACCESS, &old_protection)) {
      printf("[policy-tests] short-policy guard protection failed: error=%lu\n", ::GetLastError());
      ok = false;
    } else {
      const uint32_t size = sizeof(uint32_t);
      void *short_policy = guarded + system.dwPageSize - sizeof(size);
      std::memcpy(short_policy, &size, sizeof(size));
      ok = ExpectPolicyRejected(
               "guarded-short-policy",
               target.c_str(),
               static_cast<const SboxPolicy *>(short_policy),
               "incompatible SboxPolicy size") &&
          ok;
    }
    if (!::VirtualFree(guarded, 0, MEM_RELEASE)) {
      printf("[policy-tests] short-policy guard cleanup failed: error=%lu\n", ::GetLastError());
      ok = false;
    }
  }

  constexpr PolicySpec cases[] = {
      {"ordinary-untrusted", false, false, SBOX_INTEGRITY_UNTRUSTED},
      {"ordinary-low", false, false, SBOX_INTEGRITY_LOW},
      {"extended-policy-size", false, false, SBOX_INTEGRITY_UNTRUSTED, false, true},
      {"rs5-gate-ordinary",
       false,
       false,
       SBOX_INTEGRITY_UNTRUSTED,
       false,
       false,
       SBOX_TRUST_TRANSITION_PLATFORM_RS5},
      {"appcontainer-low", true, false, SBOX_INTEGRITY_LOW},
      {"lpac-low", true, true, SBOX_INTEGRITY_LOW},
      {"lpac-untrusted", true, true, SBOX_INTEGRITY_UNTRUSTED},
      {"appcontainer-unused-token-fields", true, false, SBOX_INTEGRITY_LOW, true},
      {"lpac-unused-token-fields", true, true, SBOX_INTEGRITY_LOW, true},
      // Retired with the two-image topology: the former
      // "hosted-relocation" / "lpac-hosted-relocation" cases set SBOX_FORCE_RELOCATE
      // to force the target's SEPARATE sandbox-core DLL to load at a different base
      // and prove the RVA seed survived. In the single statically-linked image the
      // core is mapped with the EXE at creation and same-boot ASLR pins the broker
      // and worker to the same base, so the core image cannot be post-spawn
      // relocated; the forced-relocation assertion is architecturally inapplicable.
      // (The SBOX_FORCE_RELOCATE broker probe remains for a manual seed-resolves check.)
  };
  uint64_t serial = 100;
  for (const auto &spec : cases)
    ok = RunPolicyCase(spec, serial++) && ok;
  PolicySpec unknown_token{"ordinary-unknown-initial-token", false, false, SBOX_INTEGRITY_UNTRUSTED};
  unknown_token.unknown_initial_token = true;
  ok = RunPolicyCase(unknown_token, serial++) && ok;
  printf("[policy-tests] all -> %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool RunExitDiagnostics() {
  sbox_trust_transition_test_reset_broker();
  const SboxPolicy policy = MakePolicy(kCases[7]);
  SboxSession *session = sbox_broker_spawn(TargetPath().c_str(), &policy, nullptr, nullptr);
  if (!session) {
    printf("[trust-transition-tests] exit-diagnostics spawn failed\n");
    return false;
  }
  SboxTrustTransitionExitObservation observed = {};
  const ULONGLONG deadline = ::GetTickCount64() + 10000;
  bool queried = true;
  do {
    queried = sbox_trust_transition_test_get_exit_observation(&observed) != 0;
    if (!queried || observed.count)
      break;
    ::Sleep(10);
  } while (::GetTickCount64() < deadline);

  // The reader must report exit even when the host has not called wait yet.
  bool ok = queried && observed.count == 1 && observed.exit_code == 40;
  const int exit_code = sbox_broker_wait(session);
  const std::string output = sbox_trust_transition_test_last_output();
  ok = ok && exit_code == 40 && Contains(output, "target created: pid=") && Contains(output, "target resume: pid=") &&
      Contains(output, "target exit observed: pid=") && Contains(output, "exit=40 (0x00000028) output=");
  printf(
      "[trust-transition-tests] exit-diagnostics observed_before_wait=%u exit=%u -> %s\n",
      observed.count,
      observed.exit_code,
      ok ? "PASS" : "FAIL");
  sbox_trust_transition_test_reset_broker();
  return ok;
}

void PrintHelp() {
  printf("Usage: sbox_trust_transition_test.exe --all | --policy | --exit-diagnostics | --case <name>\nCases:\n");
  for (const auto &spec : kCases)
    printf("  %s\n", spec.name);
}

int RunCaseRunner(int argc, char **argv) {
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
  if (argc == 2 && std::strcmp(argv[1], "--policy") == 0)
    return RunPolicyTests() ? 0 : 1;
  if (argc == 2 && std::strcmp(argv[1], "--exit-diagnostics") == 0)
    return RunExitDiagnostics() ? 0 : 1;
  if (argc != 2 || std::strcmp(argv[1], "--all") != 0) {
    PrintHelp();
    return 2;
  }

  // Measure failed-spawn cleanup before successful children can leave pending
  // job notifications that change the broker's handle count.
  bool ok = RunPolicyTests();
  ok = RunExitDiagnostics() && ok;
  ok = RunWithoutControl("missing-control") && ok;
  ok = !sbox_trust_transition_test_set_next_case(999, 1) && ok;
  printf("[trust-transition-tests] unknown-control -> %s\n", ok ? "PASS" : "FAIL");
  uint64_t serial = 1;
  for (const auto &spec : kCases)
    ok = RunCase(spec, serial++) && ok;
  ok = RunWithoutControl("stale-control") && ok;
  printf("[trust-transition-tests] all -> %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

// Role dispatch. Kept deliberately dumb and FIRST so neither persona can fall
// through into the other's init: a directly-launched --worker with no broker
// seed fails closed in sbox_target_begin.
bool IsWorkerRole(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--worker") == 0)
      return true;
  }
  return false;
}

}  // namespace

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (IsWorkerRole(argc, argv))
    return RunWorker();
  return RunCaseRunner(argc, argv);
}
