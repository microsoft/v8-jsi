// The instrumented trust-transition test links the same sandbox core as
// sbox.exe into one image with broker and worker roles selected by argv.
// The broker writes control data into the suspended worker at symbol addresses
// validated by the equal-image-base guard.
//
//   * --worker: reads the control data, drives the trust transition, and injects
//     selected final-mitigation or ACG faults using sbox_target_*.
//   * --all / --case <name> / --policy / --exit-diagnostics / --help: runs cases
//     using sbox_broker_* and the trust-transition control API, asserting exit
//     codes and markers.
//
// Role dispatch is FIRST in main() so neither persona can fall through into the
// other's init. testonly; SBOX_TRUST_TRANSITION_TESTING-gated; never shipped.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <sddl.h>
#include <tlhelp32.h>
#include <userenv.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "sandbox/win/src/handle_closer.h"
#include "sandbox/win/src/interception.h"
#include "sandbox/win/src/interceptors.h"
#include "sandbox/win/src/target_interceptions.h"
#include "sbox_core_internal.h"
#include "sbox_trust_transition_test_private.h"

// Defined here and declared in the private header so both roles reference the
// same unexported symbol; the broker writes it before resuming the worker.
extern "C" SboxTrustTransitionTestControl g_sbox_trust_transition_test_control = {};

namespace sandbox {
extern "C" OriginalFunctions g_originals;
}

namespace {
using EntryBytes = std::array<unsigned char, 16>;
enum class EntryArch { X64, Arm64 };
enum class EntryKind { Unpatched, Patched, Foreign };

EntryKind ClassifyEntry(const EntryBytes &entry, const EntryBytes &broker,
                        uint64_t interceptor) {
  if (entry == broker)
    return EntryKind::Unpatched;
  // Both supported resolver layouts embed the broker's 64-bit address.
  for (size_t offset = 0; offset + sizeof(interceptor) <= entry.size(); ++offset) {
    uint64_t address = 0;
    std::memcpy(&address, entry.data() + offset, sizeof(address));
    if (interceptor && address == interceptor)
      return EntryKind::Patched;
  }
  return EntryKind::Foreign;
}

bool RunEntryClassifierSynthetic() {
  bool ok = true;
  // Both resolver layouts are checked on every host without executing the bytes.
  for (EntryArch arch : {EntryArch::X64, EntryArch::Arm64}) {
    const EntryBytes broker{0x4c, 0x8b, 0xd1, 0xb8};
    const uint64_t interceptor = 0x123456789abcdef0ull;
    EntryBytes patched{};
    if (arch == EntryArch::X64) {
      patched[0] = 0x48;
      patched[1] = 0xb8;
      patched[10] = 0xff;
      patched[11] = 0xe0;
    } else {
      const uint32_t instructions[] = {0x58000050, 0xd61f0200};
      std::memcpy(patched.data(), instructions, sizeof(instructions));
    }
    const size_t offset = arch == EntryArch::X64 ? 2 : 8;
    std::memcpy(patched.data() + offset, &interceptor, sizeof(interceptor));
    EntryBytes foreign = broker;
    foreign[0] = 0xe9;
    const bool passed = ClassifyEntry(patched, broker, interceptor) == EntryKind::Patched &&
        ClassifyEntry(broker, broker, interceptor) == EntryKind::Unpatched &&
        ClassifyEntry(foreign, broker, interceptor) == EntryKind::Foreign &&
        ClassifyEntry(patched, broker, interceptor + 1) == EntryKind::Foreign;
    printf("[trust-transition-tests] classifier arch=%s patched/unpatched/foreign/wrong-address -> %s\n",
           arch == EntryArch::X64 ? "x64" : "ARM64", passed ? "PASS" : "FAIL");
    ok = passed && ok;
  }
  printf("[trust-transition-tests] case=entry-classifier-synthetic -> %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool ReadRemote(HANDLE process, const void *address, void *buffer, size_t size) {
  SIZE_T read = 0;
  return ::ReadProcessMemory(process, address, buffer, size, &read) && read == size;
}
} // namespace

namespace sandbox::trust_transition_test {
Inspection InspectBeforeResume(HANDLE process) {
  struct Hook { const char *name; const void *interceptor; InterceptorId id; };
  const Hook hooks[] = {
#define MONITOR(name, id) {#name, reinterpret_cast<const void *>(MAKE_SERVICE_NAME(name)), id}
      MONITOR(NtOpenThread, OPEN_THREAD_ID),
      MONITOR(NtOpenProcess, OPEN_PROCESS_ID),
      MONITOR(NtOpenProcessToken, OPEN_PROCESS_TOKEN_ID),
      MONITOR(NtOpenProcessTokenEx, OPEN_PROCESS_TOKEN_EX_ID),
      MONITOR(NtSetInformationThread, SET_INFORMATION_THREAD_ID),
      MONITOR(NtOpenThreadToken, OPEN_THREAD_TOKEN_ID),
      MONITOR(NtOpenThreadTokenEx, OPEN_THREAD_TOKEN_EX_ID),
      MONITOR(NtCreateFile, CREATE_FILE_ID),
      MONITOR(NtOpenFile, OPEN_FILE_ID),
      MONITOR(NtQueryAttributesFile, QUERY_ATTRIB_FILE_ID),
      MONITOR(NtQueryFullAttributesFile, QUERY_FULL_ATTRIB_FILE_ID),
      MONITOR(NtSetInformationFile, SET_INFO_FILE_ID),
#undef MONITOR
  };
  Inspection result;
  OriginalFunctions originals{};
  HandleCloserConfig closer{};
  result.valid = ReadRemote(process, &g_originals, &originals, sizeof(originals)) &&
      ReadRemote(process, &g_handle_closer_info, &closer, sizeof(closer)) && !closer.handle_closer_enabled;
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  unsigned expected_originals = 0;
  for (size_t i = 0; i < std::size(hooks); ++i) {
    const Hook &hook = hooks[i];
    const void *entry = ntdll ? reinterpret_cast<const void *>(::GetProcAddress(ntdll, hook.name)) : nullptr;
    EntryBytes remote{}, broker{};
    const bool read = entry && ReadRemote(process, entry, remote.data(), remote.size()) &&
        ReadRemote(::GetCurrentProcess(), entry, broker.data(), broker.size());
    const EntryKind kind = read ? ClassifyEntry(remote, broker,
        reinterpret_cast<uintptr_t>(hook.interceptor)) : EntryKind::Foreign;
    if (kind == EntryKind::Foreign) {
      printf("[trust-transition] export=%s invalid entry: possible third-party (endpoint-security) hooking\n",
             hook.name);
      result.valid = false;
    }
    const bool patched = kind == EntryKind::Patched;
    if (patched) {
      result.patched |= 1u << i;
      ++expected_originals;
    }
    void *original = originals.functions[hook.id];
    if (patched != (original != nullptr))
      result.valid = false;
  }
  for (void *original : originals.functions) {
    if (!original)
      continue;
    ++result.originals;
    MEMORY_BASIC_INFORMATION region{};
    if (::VirtualQueryEx(process, original, &region, sizeof(region)) == sizeof(region) &&
        region.State == MEM_COMMIT && region.Type == MEM_PRIVATE && region.Protect == PAGE_EXECUTE_READ)
      ++result.rx;
  }
  result.valid = result.valid && result.originals == expected_originals && result.rx == expected_originals;
  return result;
}
} // namespace sandbox::trust_transition_test

namespace {

// ===========================================================================
// Worker role (fault-injection harness).
// ===========================================================================

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

  for (BOOL open_as_self : {FALSE, TRUE}) {
    HANDLE thread_token = nullptr;
    const bool impersonating = ::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, open_as_self, &thread_token) != FALSE;
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
        "open_as_self=%d thread_token=%d thread_error=%lu -> %s\n",
        final ? "final" : "initial",
        app_container,
        expected.lpac,
        integrity,
        open_as_self,
        impersonating,
        thread_error,
        ok ? "PASS" : "FAIL");
  }
  return ok;
}

bool CheckFixtureRead(const wchar_t *directory, const wchar_t *name, bool expected) {
  const std::wstring path = std::wstring(directory) + L"\\" + name;
  HANDLE file = ::CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  const bool opened = file != INVALID_HANDLE_VALUE;
  const DWORD error = opened ? ERROR_SUCCESS : ::GetLastError();
  char data[64] = {};
  DWORD read = 0;
  const bool read_ok = opened && ::ReadFile(file, data, sizeof(data), &read, nullptr) && read == 23 && std::memcmp(data, "sandbox policy fixture\n", 23) == 0;
  if (opened)
    ::CloseHandle(file);
  const bool ok = opened == expected && (!opened || read_ok) && (opened || error == ERROR_ACCESS_DENIED);
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
  const bool allowed = CheckFixtureRead(expected.fixture_directory, L"allowed.txt", expected.file_read_allowed != 0);
  const bool denied = CheckFixtureRead(expected.fixture_directory, L"denied.txt", false);
  const bool package = CheckFixtureRead(expected.fixture_directory, L"package.txt", expected.app_container != 0);
  // ALL APPLICATION PACKAGES can be implicit rather than listed in TokenGroups.
  // Test its actual access effect to distinguish AppContainer from LPAC.
  const bool all_apps =
      CheckFixtureRead(expected.fixture_directory, L"all-apps.txt", expected.app_container && !expected.lpac);
  const std::wstring path = std::wstring(expected.fixture_directory) + L"\\allowed.txt";
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  const bool write_denied = file == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_ACCESS_DENIED;
  if (file != INVALID_HANDLE_VALUE)
    ::CloseHandle(file);
  printf("[policy-target] resource=allowed.txt write expected=denied -> %s\n", write_denied ? "PASS" : "FAIL");
  return allowed && denied && package && all_apps && write_denied;
}

DWORD WINAPI WarmupThread(void *) { return 73; }

bool VerifyEarlyWarmup(const SboxTrustTransitionPolicyExpectation &expected) {
  wchar_t executable[MAX_PATH] = {};
  if (!::GetModuleFileNameW(nullptr, executable, MAX_PATH))
    return false;
  const std::wstring path(executable);
  const std::wstring directory = path.substr(0, path.find_last_of(L"\\") + 1);
  bool ok = true;
  for (const wchar_t *name : {L"v8host.dll", L"v8jsisb.dll"}) {
    HMODULE module = ::LoadLibraryExW((directory + name).c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    if (!module)
      printf("[policy-target] stage=warmup-load module=%ls error=%lu -> FAIL\n", name, ::GetLastError());
    ok = module != nullptr && ok;
    // Keep modules loaded through lockdown, as the real warm-up path does.
  }
  // Tiny opaque snapshot/guest inputs exercise warm-up I/O, not deserialization.
  for (const wchar_t *name : {L"snapshot.bin", L"guest.js"})
    ok = CheckFixtureRead(expected.fixture_directory, name, true) && ok;
  HANDLE thread = ::CreateThread(nullptr, 0, &WarmupThread, nullptr, 0, nullptr);
  DWORD code = 0;
  ok = thread && ::WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0 &&
      ::GetExitCodeThread(thread, &code) && code == 73 && ok;
  if (thread)
    ::CloseHandle(thread);
  printf("[policy-target] early warmup loader/read/thread -> %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool VerifyBrokerResources(const SboxTrustTransitionPolicyExpectation &expected) {
  const bool pipe = expected.file_probe == SboxTrustTransitionFileProbe::Pipe;
  const bool readable = expected.file_probe == SboxTrustTransitionFileProbe::Readonly ||
      expected.file_probe == SboxTrustTransitionFileProbe::Writable;
  const bool writable = expected.file_probe == SboxTrustTransitionFileProbe::Writable;
  const std::wstring directory(expected.fixture_directory);
  if (pipe) {
    bool ok = true;
    for (bool allowed : {true, false}) {
      const std::wstring path = L"\\\\.\\pipe\\v8jsi-sbox-" + directory.substr(directory.find_last_of(L"\\") + 1) +
          (allowed ? L"-allowed" : L"-denied");
      HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      DWORD wrote = 0;
      const DWORD error = ::GetLastError();
      const bool passed = allowed ? handle != INVALID_HANDLE_VALUE &&
          ::WriteFile(handle, "ping", 4, &wrote, nullptr) && wrote == 4 :
          handle == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED;
      if (handle != INVALID_HANDLE_VALUE)
        ::CloseHandle(handle);
      printf("[policy-target] pipe connect-send allowed=%d -> %s\n", allowed, passed ? "PASS" : "FAIL");
      ok = passed && ok;
    }
    return ok;
  }
  bool ok = CheckFixtureRead(expected.fixture_directory, L"broker.txt", readable);
  const std::wstring path = directory + L"\\broker.txt";
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD wrote = 0;
  const DWORD error = ::GetLastError();
  const bool modified = file != INVALID_HANDLE_VALUE &&
      ::WriteFile(file, "edit", 4, &wrote, nullptr) && wrote == 4;
  ok = (writable ? modified : file == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED) && ok;
  if (file != INVALID_HANDLE_VALUE)
    ::CloseHandle(file);
  // Probe rename and delete independently of the write-only open.
  const bool renamed = ::MoveFileExW(path.c_str(), (directory + L"\\broker-renamed.txt").c_str(), 0) != FALSE;
  ok = (writable ? renamed : !renamed && ::GetLastError() == ERROR_ACCESS_DENIED) && ok;
  if (!writable)
    ok = !::DeleteFileW(path.c_str()) && ::GetLastError() == ERROR_ACCESS_DENIED && ok;
  printf("[policy-target] broker file read=%d write/rename=%d -> %s\n", readable, writable, ok ? "PASS" : "FAIL");
  return ok;
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

struct RequestState {
  SboxTarget *target;
  uint32_t case_id;
  bool complete;
};

void OnRequest(void *context, int kind, const void *data, size_t length) {
  auto *state = static_cast<RequestState *>(context);
  if (kind == SBOX_MSG_STRING && length == 4 && std::memcmp(data, "PING", 4) == 0) {
    if (sbox_target_test_ipc(state->target))
      Emit(state->target, "PONG");
    return;
  }
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
  if (policy_case && control.policy.early_hooks == 1) {
    if (!::RevertToSelf()) {
      printf("[policy-target] stage=early-revert-to-self error=%lu -> FAIL\n", ::GetLastError());
      return 53;
    }
    printf("[policy-target] pre-lockdown RevertToSelf=TRUE\n");
  }
  if (policy_case && control.policy.early_hooks == 2 && !VerifyEarlyWarmup(control.policy)) {
    printf("[policy-target] stage=early-warmup result=failed\n");
    return 54;
  }
  if (policy_case && !VerifyPolicyTokens(control.policy, false)) {
    printf("[policy-target] stage=initial-token-contract result=failed\n");
    return 50;
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
  if (policy_case && (!VerifyPolicyTokens(control.policy, true) || !VerifyPolicyResources(control.policy) ||
      (control.policy.file_probe != SboxTrustTransitionFileProbe::Default && !VerifyBrokerResources(control.policy)))) {
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
  do {
    const DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, control.policy.ipc_lifetime ? 25000 : 10000);
    if (wait == WAIT_OBJECT_0) {
      sbox_target_drain_messages(target, &OnRequest, &state);
      if (!state.complete && !control.policy.ipc_lifetime)
        return 49;
    }
    else if (wait == WAIT_OBJECT_0 + 1 && control.policy.ipc_lifetime)
      break;
    else {
      printf("[trust-transition-target] stage=synthetic-protocol result=failed wait=%lu\n", wait);
      return 49;
    }
  } while (!state.complete);
  sbox_target_end(target);
  return 0;
}

// ===========================================================================
// Broker / case-runner role.
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
  bool readonly = true;
  int spawn_rc = 0;
};

constexpr CaseSpec kCases[] = {
    {SBOX_TRUST_TRANSITION_CASE_ALLOC_FAILURE, "allocation-failure", nullptr, 0, false, false, true, false, true, 40},
    {SBOX_TRUST_TRANSITION_CASE_FIRST_HOOK_FAILURE, "first-hook-failure", nullptr, 0, false, false, true, false, true, 40},
    {SBOX_TRUST_TRANSITION_CASE_PARTIAL_HOOK_FAILURE, "partial-hook-failure", nullptr, 0, false, false, true, false, true, 40},
    {SBOX_TRUST_TRANSITION_CASE_RX_FAILURE, "rx-failure", nullptr, 0, false, false, true, false, true, 40},
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
     nullptr, 0, false, false, true, true, false, 40},
    {SBOX_TRUST_TRANSITION_CASE_UNUSED_FILE_HOOK_SUCCESS, "unused-file-hook-success", nullptr, 0, true, false, true},
    {SBOX_TRUST_TRANSITION_CASE_BASE_MISMATCH, "base-mismatch", nullptr, 0, false, false, true, false, true, 65},
};

const CaseSpec *FindCase(const char *name) {
  for (const auto &spec : kCases) {
    if (std::strcmp(name, spec.name) == 0)
      return &spec;
  }
  printf("[trust-transition-tests] unknown case=%s -> FAIL\n", name);
  std::exit(2);
}

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

bool HasInspection(const std::string &output, unsigned mask) {
  unsigned count = 0;
  for (unsigned bits = mask; bits; bits >>= 1)
    count += bits & 1;
  char expected[128] = {};
  std::snprintf(expected, sizeof(expected),
      "[trust-transition] pre-resume valid=1 patched=0x%x originals=%u rx=%u", mask, count, count);
  const bool ok = Contains(output, expected);
  printf("%s -> %s\n", expected, ok ? "PASS" : "FAIL");
  return ok;
}

bool RunWithoutControl(const char *label) {
  const CaseSpec &base = *FindCase("ordinary-strict-success");
  SboxPolicy policy = MakePolicy(base);
  SboxSession *session = sbox_broker_spawn(TargetPath().c_str(), &policy, nullptr, nullptr);
  if (!session) {
    printf("[trust-transition-tests] %s: spawn failed\n", label);
    return false;
  }
  const int exit_code = sbox_broker_wait(session);
  const bool ok = exit_code == 40 && HasInspection(sbox_trust_transition_test_last_output(), 0x7f);
  sbox_trust_transition_test_reset_broker();
  printf("[trust-transition-tests] %s: exit=%d expected=40 -> %s\n", label, exit_code, ok ? "PASS" : "FAIL");
  return ok;
}

bool WaitForNoChild() {
  const ULONGLONG deadline = ::GetTickCount64() + 5000;
  do {
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
      return false;
    PROCESSENTRY32W entry = {sizeof(entry)};
    bool child = false;
    BOOL found = ::Process32FirstW(snapshot, &entry);
    while (found) {
      if (entry.th32ParentProcessID == ::GetCurrentProcessId())
        child = true;
      found = ::Process32NextW(snapshot, &entry);
    }
    const DWORD error = ::GetLastError();
    ::CloseHandle(snapshot);
    if (error != ERROR_NO_MORE_FILES)
      return false;
    if (!child)
      return true;
    ::Sleep(10);
  } while (::GetTickCount64() < deadline);
  return false;
}

// Compare exact names, not counts: old logs must not hide a newly leaked log.
bool SnapshotWorkerLogs(std::vector<std::wstring> &names) {
  wchar_t dir[MAX_PATH] = {};
  const DWORD length = ::GetTempPathW(MAX_PATH, dir);
  if (!length || length >= MAX_PATH)
    return false;
  WIN32_FIND_DATAW data = {};
  HANDLE search = ::FindFirstFileW((std::wstring(dir) + L"sbx*.tmp").c_str(), &data);
  if (search == INVALID_HANDLE_VALUE)
    return ::GetLastError() == ERROR_FILE_NOT_FOUND;
  do {
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      names.emplace_back(data.cFileName);
  } while (::FindNextFileW(search, &data));
  const DWORD error = ::GetLastError();
  ::FindClose(search);
  std::sort(names.begin(), names.end());
  return error == ERROR_NO_MORE_FILES;
}

bool WorkerLogsCleanedUp(const std::vector<std::wstring> &before) {
  std::vector<std::wstring> after;
  if (!SnapshotWorkerLogs(after))
    return false;
  bool ok = true;
  for (const auto &name : after) {
    if (!std::binary_search(before.begin(), before.end(), name)) {
      printf("[trust-transition-tests] leaked worker log=%ls\n", name.c_str());
      ok = false;
    }
  }
  return ok;
}

bool RunCase(const CaseSpec &spec, uint64_t serial, bool recover = true) {
  sbox_trust_transition_test_reset_broker();
  printf(
      "[trust-transition-tests] selected=%u name=%s serial=%llu\n",
      spec.id,
      spec.name,
      static_cast<unsigned long long>(serial));
  if (!sbox_trust_transition_test_set_next_case(spec.id, serial)) {
    printf("[trust-transition-tests] set-next failed\n");
    sbox_trust_transition_test_reset_broker();
    return false;
  }
  if (sbox_trust_transition_test_set_next_case(spec.id, serial + 10000)) {
    printf("[trust-transition-tests] duplicate control was accepted\n");
    sbox_trust_transition_test_reset_broker();
    return false;
  }

  if (spec.lpac && !DeleteTestProfile(false)) {
    sbox_trust_transition_test_reset_broker();
    return false;
  }
  SboxPolicy policy = MakePolicy(spec);
  const std::wstring file_probe = TargetPath();
  const SboxFileRule file_rule{file_probe.c_str(), spec.readonly ? 1 : 0};
  if (spec.file_brokering) {
    policy.file_rules = &file_rule;
    policy.file_rule_count = 1;
  }
  std::vector<std::wstring> logs_before;
  if (spec.spawn_rc && !SnapshotWorkerLogs(logs_before)) {
    sbox_trust_transition_test_reset_broker();
    return false;
  }
  Messages messages;
  SboxSession *session = sbox_broker_spawn(TargetPath().c_str(), &policy, &OnMessage, &messages);
  int exit_code = -1;
  if (session) {
    const char request[] = "REQUEST";
    sbox_broker_post_message(session, SBOX_MSG_STRING, request, sizeof(request) - 1);
    exit_code = sbox_broker_wait(session);
  }
  const uint32_t consumed = sandbox::trust_transition_test::Consumed();
  // Already-consumed faults cannot distinguish disarming; worker/success cases can.
  const bool broker_disarmed = !sandbox::trust_transition_test::Consume(spec.id) &&
      sandbox::trust_transition_test::Consumed() == consumed;
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

  if (spec.spawn_rc) {
    char failure[80] = {};
    std::snprintf(failure, sizeof(failure), "SpawnTargetAsync failed: rc=%d ", spec.spawn_rc);
    const bool no_child = WaitForNoChild();
    const bool logs_cleaned_up = WorkerLogsCleanedUp(logs_before);
    bool ok = logs_cleaned_up && broker_disarmed && !session && consumed == spec.id && Contains(output, failure) &&
        !Contains(output, "target resume:") && !Contains(output, "pre-resume") &&
        ready == 0 && guest == 0 && host == 0 && result == 0 && no_child && fixture_cleanup_ok &&
        !Contains(output, "SECURITY_READY") && !Contains(output, "GUEST_ENTRY") &&
        !Contains(output, "HOST_OPERATION") && !Contains(output, "RESULT");
    printf("[trust-transition-tests] case=%s spawn=null:%d rc=%d consumed=%u markers=%zu/%zu/%zu/%zu no-child=%d logs-cleaned-up=%d -> %s\n",
           spec.name, !session, spec.spawn_rc, consumed, ready, guest, host, result, no_child, logs_cleaned_up, ok ? "PASS" : "FAIL");
    sbox_trust_transition_test_reset_broker();
    ok = sandbox::trust_transition_test::Consumed() == SBOX_TRUST_TRANSITION_CASE_INVALID && ok;
    if (recover) {
      const bool recovered = RunCase(*FindCase("ordinary-strict-success"), serial + 100000);
      printf("[trust-transition-tests] recovery after=%s -> %s\n", spec.name, recovered ? "PASS" : "FAIL");
      ok = recovered && ok;
    }
    return ok;
  }

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
  bool ok = broker_disarmed && exit_code == static_cast<int>(spec.exit_code) && Contains(output, selected) &&
      Contains(output, consumed_control) && fixture_cleanup_ok;
  ok = HasInspection(output, !spec.file_brokering ? 0x7f : spec.readonly ? 0x7ff : 0xfff) && ok;
  if (spec.success) {
    ok = ok && consumed == SBOX_TRUST_TRANSITION_CASE_INVALID && ready == 1 && guest == 1 && host == 1 &&
        result == 1 && Contains(output, "pre-lockdown-ipc=ok") && Contains(output, "final-lockdown=ok") &&
        Contains(output, "post-lockdown-ipc=ok");
    ok = ok && Contains(output, spec.acg ? "strict_acg_expected=true" : "strict_acg_expected=false");
    if (spec.lpac)
      ok = ok && Contains(output, "lpac=1 app_container=1") && Contains(output, "capability=present");
  } else {
    char consumed_marker[32] = {};
    std::snprintf(consumed_marker, sizeof(consumed_marker), "consumed=%u", spec.id);
    ok = ok && spec.stage && Contains(output, spec.stage) && Contains(output, consumed_marker) && ready == 0 && guest == 0 &&
        host == 0 && result == 0;
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
  sbox_trust_transition_test_reset_broker();
  return ok;
}

struct PolicyFixture {
  std::wstring directory;
  std::wstring profile;
  std::wstring package_sid;
  std::wstring user_sid;
  std::vector<std::wstring> files;
  HANDLE pipes[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
  std::wstring pipe_path;
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

  bool MakePipes() {
    pipe_path = L"\\\\.\\pipe\\v8jsi-sbox-" + directory.substr(directory.find_last_of(L"\\") + 1);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(Descriptor(nullptr, false).c_str(),
        SDDL_REVISION_1, &descriptor, nullptr))
      return false;
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    for (int i = 0; i < 2; ++i)
      pipes[i] = ::CreateNamedPipeW((pipe_path + (i ? L"-denied" : L"-allowed")).c_str(),
          PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 64, 64, 0, &attributes);
    ::LocalFree(descriptor);
    return pipes[0] != INVALID_HANDLE_VALUE && pipes[1] != INVALID_HANDLE_VALUE;
  }

  bool Cleanup() {
    bool ok = true;
    for (HANDLE pipe : pipes) {
      if (pipe != INVALID_HANDLE_VALUE)
        ok = ::CloseHandle(pipe) && ok;
    }
    for (const auto &file : files) {
      if (!::DeleteFileW(file.c_str())) {
        printf("[policy-tests] fixture file cleanup failed: error=%lu\n", ::GetLastError());
        ok = false;
      }
    }
    files.clear();
    const std::wstring renamed = directory + L"\\broker-renamed.txt";
    if (directory_created && ::GetFileAttributesW(renamed.c_str()) != INVALID_FILE_ATTRIBUTES) {
      printf("[policy-tests] unexpected renamed fixture left behind -> FAIL\n");
      ::DeleteFileW(renamed.c_str());
      ok = false;
    }
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

bool SetFixtureExpectation(const PolicyFixture &fixture, const SboxPolicy &policy,
                           SboxTrustTransitionPolicyExpectation &expected) {
  expected.initial_integrity = policy.integrity;
  expected.delayed_integrity = policy.delayed_integrity;
  expected.app_container = policy.use_app_container;
  expected.lpac = policy.low_privilege_app_container;
  if (fixture.directory.size() >= std::size(expected.fixture_directory) ||
      fixture.package_sid.size() >= std::size(expected.package_sid)) {
    printf("[policy-tests] fixture path or SID exceeds control capacity\n");
    return false;
  }
  std::wcscpy(expected.fixture_directory, fixture.directory.c_str());
  std::wcscpy(expected.package_sid, fixture.package_sid.c_str());
  return true;
}

struct PolicySpec {
  const char *name;
  bool app_container;
  bool lpac;
  int32_t delayed_integrity;
  bool inactive_tokens = false;
  bool extended_size = false;
  uint32_t platform = SBOX_TRUST_TRANSITION_PLATFORM_CURRENT;
  bool unknown_initial_token = false;
  bool no_file_rule = false;
  int32_t early_hooks = 0;
  SboxTrustTransitionFileProbe file_probe = SboxTrustTransitionFileProbe::Default;
};

bool RunPolicyCase(const PolicySpec &spec, uint64_t serial) {
  sbox_trust_transition_test_reset_broker();
  // Resolve the case before creating fixtures: FindCase fails fast on an unknown name.
  const SboxPolicy base_policy = MakePolicy(*FindCase("ordinary-strict-success"));
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
  policy = base_policy;
  policy.struct_size = spec.extended_size ? sizeof(extended) : sizeof(policy);
  policy.use_app_container = spec.app_container;
  policy.low_privilege_app_container = spec.lpac;
  policy.delayed_integrity = spec.delayed_integrity;
  const wchar_t *capabilities[] = {kSboxTrustTransitionCapability};
  policy.app_container_profile_name = fixture.profile.c_str();
  policy.capabilities = spec.app_container ? capabilities : nullptr;
  policy.capability_count = spec.app_container ? 1 : 0;
  const std::wstring allowed_path = fixture.directory + L"\\allowed.txt";
  std::wstring probe_path;
  if (spec.file_probe != SboxTrustTransitionFileProbe::Default) {
    if (!(spec.file_probe == SboxTrustTransitionFileProbe::Pipe ? fixture.MakePipes() :
          fixture.MakeFile(L"broker.txt", nullptr))) {
      fixture.Cleanup();
      return false;
    }
    probe_path = spec.file_probe == SboxTrustTransitionFileProbe::Pipe ? L"\\??\\pipe\\" + fixture.pipe_path.substr(9) + L"-allowed" :
        fixture.directory + (spec.file_probe == SboxTrustTransitionFileProbe::Nonmatching ? L"\\missing*" : L"\\broker*");
  }
  const SboxFileRule probe_rule{probe_path.c_str(), spec.file_probe == SboxTrustTransitionFileProbe::Writable ||
      spec.file_probe == SboxTrustTransitionFileProbe::Pipe ? 0 : 1};
  const SboxFileRule allowed_rule{allowed_path.c_str(), 1};
  policy.file_rules = (spec.app_container && spec.early_hooks != 2) || spec.no_file_rule ? nullptr : &allowed_rule;
  policy.file_rule_count = policy.file_rules ? 1 : 0;
  if (spec.file_probe != SboxTrustTransitionFileProbe::Default) {
    policy.file_rules = spec.file_probe == SboxTrustTransitionFileProbe::None ? nullptr : &probe_rule;
    policy.file_rule_count = policy.file_rules ? 1 : 0;
  }
  if (spec.inactive_tokens) {
    policy.initial_token = SBOX_TOKEN_LOCKDOWN;
    policy.lockdown_token = SBOX_TOKEN_RESTRICTED_SAME_ACCESS;
  }
  if (spec.unknown_initial_token)
    policy.initial_token = -1;

  SboxTrustTransitionPolicyExpectation expected = {};
  expected.file_probe = spec.file_probe;
  expected.early_hooks = spec.early_hooks;
  expected.file_read_allowed =
      (spec.file_probe == SboxTrustTransitionFileProbe::Default && policy.file_rule_count) || spec.app_container;
  if (!SetFixtureExpectation(fixture, policy, expected)) {
    fixture.Cleanup();
    return false;
  }
  bool ok = sbox_trust_transition_test_set_next_policy_case(&expected, serial) &&
      sbox_trust_transition_test_set_platform(spec.platform);
  if (spec.inactive_tokens)
    ok = sbox_trust_transition_test_set_broker_fault(SBOX_TRUST_TRANSITION_BROKER_TOKEN_FAILURE) && ok;

  if (spec.early_hooks == 2)
    ok = fixture.MakeFile(L"snapshot.bin", spec.app_container ? kSboxTrustTransitionCapability : nullptr) &&
        fixture.MakeFile(L"guest.js", spec.app_container ? kSboxTrustTransitionCapability : nullptr) && ok;
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
  ok = HasInspection(output, !policy.file_rule_count ? 0x7f :
      spec.file_probe == SboxTrustTransitionFileProbe::Writable ? 0xfff : 0x7ff) && ok;
  if (spec.file_probe == SboxTrustTransitionFileProbe::Pipe) {
    char data[4] = {};
    DWORD read = 0;
    // The worker has exited: read only the bytes already buffered, without blocking.
    DWORD available = 0;
    const bool received = ::PeekNamedPipe(fixture.pipes[0], nullptr, 0, nullptr, &available, nullptr) && available == 4 &&
        ::ReadFile(fixture.pipes[0], data, sizeof(data), &read, nullptr);
    ok = received && read == 4 && std::memcmp(data, "ping", 4) == 0 && ok;
  }
  if (spec.file_probe == SboxTrustTransitionFileProbe::Writable) {
    const std::wstring renamed = fixture.directory + L"\\broker-renamed.txt";
    const bool exists = ::GetFileAttributesW(renamed.c_str()) != INVALID_FILE_ATTRIBUTES;
    ok = exists && ok;
    if (exists)
      std::replace(fixture.files.begin(), fixture.files.end(), fixture.directory + L"\\broker.txt", renamed);
  }
  if (spec.inactive_tokens) {
    ok = ok && sbox_trust_transition_test_pending_broker_fault() == SBOX_TRUST_TRANSITION_BROKER_TOKEN_FAILURE &&
        Contains(output, "do not apply; no restricted-default-DACL guarantee");
  }
  if (!ok)
    printf("[policy-tests] failure output:\n%s\n", output.c_str());
  ok = fixture.Cleanup() && ok;
  sbox_trust_transition_test_reset_broker();
  printf(
      "[policy-tests] case=%s profile=%s exit=%d markers=%zu/%zu/%zu/%zu -> %s\n",
      spec.name,
      spec.lpac ? "lpac" : spec.app_container ? "appcontainer" : "ordinary",
      exit_code,
      ready,
      guest,
      host,
      result,
      ok ? "PASS" : "FAIL");
  return ok;
}

bool WaitForMessage(Messages &messages, const char *value) {
  const ULONGLONG deadline = ::GetTickCount64() + 10000;
  do {
    {
      std::lock_guard<std::mutex> lock(messages.mutex);
      if (std::find(messages.values.begin(), messages.values.end(), value) != messages.values.end())
        return true;
    }
    ::Sleep(10);
  } while (::GetTickCount64() < deadline);
  return false;
}

bool RunIpcLifetime() {
  sbox_trust_transition_test_reset_broker();
  const SboxPolicy base_policy = MakePolicy(*FindCase("ordinary-strict-success"));
  PolicyFixture fixture;
  if (!fixture.Initialize(false, 300)) {
    fixture.Cleanup();
    return false;
  }
  SboxPolicy policy = base_policy;
  SboxTrustTransitionPolicyExpectation expected = {};
  if (!SetFixtureExpectation(fixture, policy, expected)) {
    fixture.Cleanup();
    return false;
  }
  expected.ipc_lifetime = 1;
  Messages messages[2];
  SboxSession *sessions[2] = {};
  bool ok = true;
  for (int i = 0; i < 2; ++i) {
    const bool armed = sbox_trust_transition_test_set_next_policy_case(&expected, 300 + i) != 0;
    sessions[i] = armed ? sbox_broker_spawn(TargetPath().c_str(), &policy, &OnMessage, &messages[i]) : nullptr;
    ok = sessions[i] && HasInspection(sbox_trust_transition_test_last_output(), 0x7f) &&
        WaitForMessage(messages[i], "SECURITY_READY case=11") && ok;
  }
  if (sessions[0]) {
    ok = sbox_broker_close(sessions[0]) == 0 && ok;
    ok = sbox_broker_wait(sessions[0]) == 0 && ok;
  }
  if (sessions[1]) {
    ok = sbox_broker_post_message(sessions[1], SBOX_MSG_STRING, "PING", 4) == 0 && ok;
    ok = WaitForMessage(messages[1], "PONG") && ok;
    ok = sbox_broker_close(sessions[1]) == 0 && ok;
    ok = sbox_broker_wait(sessions[1]) == 0 && ok;
  }
  ok = WaitForNoChild() && ok;
  ok = fixture.Cleanup() && ok;
  sbox_trust_transition_test_reset_broker();
  printf("[trust-transition-tests] case=ipc-session-survives-peer-close -> %s\n", ok ? "PASS" : "FAIL");
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
  ok = ok && Contains(output, reason) && !Contains(output, "target resume:") &&
      !Contains(output, "pre-resume");
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
  const SboxPolicy ordinary = MakePolicy(*FindCase("ordinary-strict-success"));
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

  // A handle increase was observed only on first use, not across repeats.
  // Warm up before measuring steady-state failed-spawn cleanup.
  ok = RunCase(*FindCase("allocation-failure"), 999, false) && ok;

  // Each case exercises a different post-creation failure and its cleanup.
  for (const auto &spec : kCases) {
    if (!spec.spawn_rc)
      continue;
    handles_before = handles_after = 0;
    cleanup_ok = ::GetProcessHandleCount(::GetCurrentProcess(), &handles_before) != FALSE;
    for (int attempt = 0; attempt < 16; ++attempt)
      cleanup_ok = RunCase(spec, 1000 + attempt, false) && cleanup_ok;
    cleanup_ok = ::GetProcessHandleCount(::GetCurrentProcess(), &handles_after) &&
        handles_before == handles_after && cleanup_ok;
    printf("[policy-tests] failed-spawn case=%s attempts=16 handles before=%lu after=%lu -> %s\n",
           spec.name, handles_before, handles_after, cleanup_ok ? "PASS" : "FAIL");
    ok = cleanup_ok && ok;
  }

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
  };
  uint64_t serial = 100;
  for (const auto &spec : cases)
    ok = RunPolicyCase(spec, serial++) && ok;
  // Cross profiles with rule selection: each cell proves native denial or broker access.
  for (int profile = 0; profile < 3; ++profile) {
    for (SboxTrustTransitionFileProbe probe : {SboxTrustTransitionFileProbe::None, SboxTrustTransitionFileProbe::Readonly,
             SboxTrustTransitionFileProbe::Writable, SboxTrustTransitionFileProbe::Nonmatching,
             SboxTrustTransitionFileProbe::Pipe}) {
      constexpr const char *profiles[] = {"ordinary", "appcontainer", "lpac"};
      constexpr const char *probes[] = {"default", "none", "readonly", "writable", "nonmatching", "pipe"};
      const std::string name = std::string(profiles[profile]) + "-" + probes[static_cast<int>(probe)];
      PolicySpec spec{name.c_str(), profile != 0, profile == 2, SBOX_INTEGRITY_LOW};
      spec.file_probe = probe;
      ok = RunPolicyCase(spec, serial++) && ok;
    }
  }
  PolicySpec unknown_token{"ordinary-unknown-initial-token", false, false, SBOX_INTEGRITY_UNTRUSTED};
  unknown_token.unknown_initial_token = true;
  ok = RunPolicyCase(unknown_token, serial++) && ok;
  ok = RunIpcLifetime() && ok;
  printf("[policy-tests] all -> %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool RunExitDiagnostics() {
  sbox_trust_transition_test_reset_broker();
  const SboxPolicy policy = MakePolicy(*FindCase("ordinary-strict-success"));
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
  ok = HasInspection(output, 0x7f) && ok;
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

bool RunStandardInstaller() {
  const CaseSpec *base = FindCase("ordinary-strict-success");
  CaseSpec spec = *base;
  spec.file_brokering = true;
  bool ok = RunCase(spec, 2);
  spec.readonly = false;
  ok = RunCase(spec, 3) && ok;
  printf("[trust-transition-tests] case=standard-installer-patches-before-resume -> %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool RunFilePolicyAllowDeny() {
  PolicySpec spec{"file-policy-allow-deny", false, false, SBOX_INTEGRITY_UNTRUSTED};
  bool ok = RunPolicyCase(spec, 1);
  spec.name = "file-policy-without-rule-denied";
  spec.no_file_rule = true;
  ok = RunPolicyCase(spec, 2) && ok;
  printf("[trust-transition-tests] case=file-policy-allow-deny -> %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

bool RunEarlyHooks(bool warmup) {
  bool ok = true;
  // Profiles differ in whether the initial thread has an impersonation token.
  for (int profile = 0; profile < (warmup ? 3 : 1); ++profile) {
    PolicySpec spec{warmup ? "early-hooks-warmup-compatible" : "early-hooks-active-before-lockdown",
                    profile != 0, profile == 2, SBOX_INTEGRITY_LOW};
    spec.early_hooks = warmup ? 2 : 1;
    ok = RunPolicyCase(spec, 200 + profile) && ok;
  }
  printf("[trust-transition-tests] case=%s -> %s\n",
      warmup ? "early-hooks-warmup-compatible" : "early-hooks-active-before-lockdown", ok ? "PASS" : "FAIL");
  return ok;
}

void PrintHelp() {
  printf("Usage: sbox_trust_transition_test.exe --all | --policy | --exit-diagnostics | --case <name>\n"
         "Cases:\n  entry-classifier-synthetic\n  standard-installer-patches-before-resume\n"
         "  file-policy-allow-deny\n  early-hooks-active-before-lockdown\n  early-hooks-warmup-compatible\n  ipc-session-survives-peer-close\n");
  for (const auto &spec : kCases)
    printf("  %s\n", spec.name);
}

int RunCaseRunner(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    PrintHelp();
    return 0;
  }
  if (argc == 3 && std::strcmp(argv[1], "--case") == 0) {
    if (std::strcmp(argv[2], "entry-classifier-synthetic") == 0)
      return RunEntryClassifierSynthetic() ? 0 : 1;
    if (std::strcmp(argv[2], "standard-installer-patches-before-resume") == 0)
      return RunStandardInstaller() ? 0 : 1;
    if (std::strcmp(argv[2], "file-policy-allow-deny") == 0)
      return RunFilePolicyAllowDeny() ? 0 : 1;
    if (std::strcmp(argv[2], "ipc-session-survives-peer-close") == 0)
      return RunIpcLifetime() ? 0 : 1;
    if (std::strcmp(argv[2], "early-hooks-active-before-lockdown") == 0)
      return RunEarlyHooks(false) ? 0 : 1;
    if (std::strcmp(argv[2], "early-hooks-warmup-compatible") == 0)
      return RunEarlyHooks(true) ? 0 : 1;
    const CaseSpec *spec = FindCase(argv[2]);
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
  ok = RunEarlyHooks(false) && ok;
  ok = RunEarlyHooks(true) && ok;
  ok = RunEntryClassifierSynthetic() && ok;
  ok = RunStandardInstaller() && ok;
  ok = RunFilePolicyAllowDeny() && ok;
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
