#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <sddl.h>
#include <userenv.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
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

std::wstring TargetPath() {
  wchar_t path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring result(path);
  result.resize(result.find_last_of(L'\\') + 1);
  return result + L"sbox_trust_transition_target.exe";
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
  bool relocation = false;
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
  expected.force_relocation = spec.relocation;
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

  std::wstring old_relocation(32768, L'\0');
  const DWORD old_length = ::GetEnvironmentVariableW(
      L"SBOX_FORCE_RELOCATE", old_relocation.data(), static_cast<DWORD>(old_relocation.size()));
  if (old_length >= old_relocation.size()) {
    printf("[policy-tests] inherited relocation selector is too long\n");
    fixture.Cleanup();
    sbox_trust_transition_test_reset_broker();
    return false;
  } else {
    old_relocation.resize(old_length);
  }
  if (spec.relocation && !::SetEnvironmentVariableW(L"SBOX_FORCE_RELOCATE", L"1")) {
    printf("[policy-tests] setting relocation probe failed: error=%lu\n", ::GetLastError());
    ok = false;
  }
  Messages messages;
  SboxSession *session = ok ? sbox_broker_spawn(TargetPath().c_str(), &policy, &OnMessage, &messages) : nullptr;
  if (spec.relocation &&
      !::SetEnvironmentVariableW(L"SBOX_FORCE_RELOCATE", old_length ? old_relocation.c_str() : nullptr)) {
    printf("[policy-tests] restoring relocation selector failed: error=%lu\n", ::GetLastError());
    ok = false;
  }
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
  if (spec.relocation)
    ok = ok && Contains(output, "hosted-relocation=ok");
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
  ok = ExpectPolicyRejected(
           "non-hosted-dll-linkage",
           target.c_str(),
           &ordinary,
           "rc=64",
           SBOX_TRUST_TRANSITION_PLATFORM_CURRENT,
           SBOX_TRUST_TRANSITION_BROKER_NON_HOSTED) &&
      ok;

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
       false,
       SBOX_TRUST_TRANSITION_PLATFORM_RS5},
      {"appcontainer-low", true, false, SBOX_INTEGRITY_LOW},
      {"lpac-low", true, true, SBOX_INTEGRITY_LOW},
      {"lpac-untrusted", true, true, SBOX_INTEGRITY_UNTRUSTED},
      {"appcontainer-unused-token-fields", true, false, SBOX_INTEGRITY_LOW, true},
      {"lpac-unused-token-fields", true, true, SBOX_INTEGRITY_LOW, true},
      {"hosted-relocation", false, false, SBOX_INTEGRITY_UNTRUSTED, false, false, true},
      {"lpac-hosted-relocation", true, true, SBOX_INTEGRITY_LOW, false, false, true},
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
  printf("Usage: sbox_trust_transition_tests.exe --all | --policy | --exit-diagnostics | --case <name>\nCases:\n");
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
