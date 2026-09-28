#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <sddl.h>

#include <cstdio>
#include <cstring>
#include <string>

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
  ::ConvertStringSidToSidW(L"S-1-15-3-4021848294-1651122667-3873966303-2985905677", &expected_capability);
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
