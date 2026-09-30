#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <sddl.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "sbox.h"
#include "sbox_trust_transition_test_private.h"

extern "C" __declspec(dllexport) SboxBootstrap g_sbox_bootstrap = {};
extern "C" __declspec(dllexport) SboxTrustTransitionTestControl g_sbox_trust_transition_test_control = {};

namespace {

constexpr DWORD kTransitionExit = 23;

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

} // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
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

  SboxTarget *target = sbox_target_begin(&g_sbox_bootstrap);
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
  if (policy_case && control.policy.force_relocation) {
    const uintptr_t target_base = reinterpret_cast<uintptr_t>(::GetModuleHandleW(L"sbox_trust_transition_test.dll"));
    if (!target_base || !control.broker_dll_base || target_base == control.broker_dll_base) {
      printf("[policy-target] stage=hosted-relocation result=failed\n");
      return 51;
    }
    printf(
        "[policy-target] hosted-relocation=ok broker=0x%llx target=0x%llx\n",
        static_cast<unsigned long long>(control.broker_dll_base),
        static_cast<unsigned long long>(target_base));
  }

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
