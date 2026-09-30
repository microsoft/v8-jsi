#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "sbox.h"
#include "sbox_harden.h"

namespace {

enum class GuestSource { kDemo, kEmptySelector, kFile, kMissing, kUnreadable, kLongPath };
enum class Input { kNone, kString, kBinary, kBoth, kInvalid };

struct TestCase {
  const char *name;
  GuestSource source;
  const char *script;
  Input input;
  int expected_exit;
  int expected_strings = 0;
  int expected_binaries = 0;
  bool demo_files = false;
};

constexpr char kStringGuest[] =
    "host.onmessage = function(m) {"
    "  if (m !== 'ping from host') throw new Error('unexpected input');"
    "  host.postMessage('js echo: ' + m);"
    "};";
constexpr char kBinaryGuest[] =
    "host.onmessage = function(m) {"
    "  var bytes = new Uint8Array(m);"
    "  if (bytes.length !== 4 || bytes[0] !== 0xde)"
    "    throw new Error('unexpected binary input');"
    "  bytes[0] = 0x42;"
    "  host.postMessageBinary(m);"
    "};";
constexpr char kThrowingCallback[] = "host.onmessage = function() { throw new Error('callback failure'); };";

constexpr TestCase kCases[] = {
    {"custom-strings-only", GuestSource::kFile, kStringGuest, Input::kString, 0, 1},
    {"custom-binary-only", GuestSource::kFile, kBinaryGuest, Input::kBinary, 0, 0, 1},
    {"custom-no-messages", GuestSource::kFile, "void 0;", Input::kNone, 0},
    {"custom-empty-file", GuestSource::kFile, "", Input::kNone, 0},
    {"custom-no-handler", GuestSource::kFile, "void 0;", Input::kString, 0},
    {"custom-syntax-error", GuestSource::kFile, "function {", Input::kNone, 13},
    {"custom-runtime-error", GuestSource::kFile, "throw new Error('evaluation failure');", Input::kNone, 13},
    {"custom-callback-error", GuestSource::kFile, kThrowingCallback, Input::kString, 13},
    {"custom-invalid-message", GuestSource::kFile, "void 0;", Input::kInvalid, 13},
    {"custom-post-error", GuestSource::kFile, "host.postMessage('x'.repeat(65529));", Input::kNone, 13},
    {"custom-binary-post-error",
     GuestSource::kFile,
     "host.postMessageBinary(new ArrayBuffer(65529));",
     Input::kNone,
     13},
    {"custom-missing-file", GuestSource::kMissing, "", Input::kNone, 24},
    {"custom-unreadable-file", GuestSource::kUnreadable, "", Input::kNone, 24},
    {"custom-long-path", GuestSource::kLongPath, "", Input::kNone, 24},
    {"demo-success", GuestSource::kDemo, "", Input::kBoth, 0, 1, 1, true},
    {"demo-empty-guest-selector", GuestSource::kEmptySelector, "", Input::kBoth, 0, 1, 1, true},
#if defined(_WIN64)
    {"demo-missing-file-probes", GuestSource::kDemo, "", Input::kBoth, 13, 1, 1},
#endif
    {"demo-missing-binary", GuestSource::kDemo, "", Input::kString, 13, 1, 0, true},
};

bool SetEnvironment(const wchar_t *name, const wchar_t *value) {
  if (::SetEnvironmentVariableW(name, value))
    return true;
  printf("[v8host-tests] setting %ls failed: error=%lu\n", name, ::GetLastError());
  return false;
}

struct Fixture {
  std::wstring directory;
  std::vector<std::wstring> files;

  bool Initialize() {
    wchar_t temp[MAX_PATH] = {};
    const DWORD length = ::GetTempPathW(MAX_PATH, temp);
    if (!length || length >= MAX_PATH) {
      printf("[v8host-tests] temporary directory query failed: error=%lu\n", ::GetLastError());
      return false;
    }
    const std::wstring path = std::wstring(temp, length) + L"v8host-results-" +
        std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
    if (!::CreateDirectoryW(path.c_str(), nullptr)) {
      printf("[v8host-tests] fixture directory creation failed: error=%lu\n", ::GetLastError());
      return false;
    }
    directory = path;
    return true;
  }

  bool Write(const std::wstring &path, const char *content) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      printf("[v8host-tests] fixture file creation failed: error=%lu\n", ::GetLastError());
      return false;
    }
    files.push_back(path);
    const DWORD size = static_cast<DWORD>(std::strlen(content));
    DWORD written = 0;
    const bool ok = ::WriteFile(file, content, size, &written, nullptr) && written == size;
    if (!ok)
      printf("[v8host-tests] fixture write failed: error=%lu\n", ::GetLastError());
    ::CloseHandle(file);
    return ok;
  }

  bool Cleanup() {
    bool ok = true;
    for (const auto &file : files) {
      if (!::DeleteFileW(file.c_str())) {
        printf("[v8host-tests] fixture file cleanup failed: error=%lu\n", ::GetLastError());
        ok = false;
      }
    }
    if (!directory.empty() && !::RemoveDirectoryW(directory.c_str())) {
      printf("[v8host-tests] fixture directory cleanup failed: error=%lu\n", ::GetLastError());
      ok = false;
    }
    return ok;
  }
};

struct Messages {
  std::mutex mutex;
  HANDLE done = nullptr;
  int expected_strings = 0;
  int expected_binaries = 0;
  int ready = 0;
  int strings = 0;
  int binaries = 0;
  bool unexpected = false;
};

void OnMessage(void *context, int kind, const void *data, size_t length) {
  auto *messages = static_cast<Messages *>(context);
  std::lock_guard<std::mutex> lock(messages->mutex);
  if (kind == SBOX_MSG_STRING) {
    const std::string text(static_cast<const char *>(data), length);
    if (text == "ready")
      ++messages->ready;
    else if (text == "js echo: ping from host")
      ++messages->strings;
    else
      messages->unexpected = true;
  } else if (kind == SBOX_MSG_BINARY && length == 4 && static_cast<const unsigned char *>(data)[0] == 0x42) {
    ++messages->binaries;
  } else {
    messages->unexpected = true;
  }
  if (messages->unexpected ||
      (messages->strings >= messages->expected_strings && messages->binaries >= messages->expected_binaries)) {
    ::SetEvent(messages->done);
  }
}

bool RunCase(const TestCase &test, Fixture &fixture, const std::wstring &target, size_t serial) {
  const std::wstring allowed = fixture.directory + L"\\allowed.txt";
  const std::wstring denied = fixture.directory + L"\\denied.txt";
  std::wstring guest = fixture.directory + L"\\guest-" + std::to_wstring(serial) + L".js";
  if (test.source == GuestSource::kLongPath)
    guest += std::wstring(300, L'x');
  if ((test.source == GuestSource::kFile || test.source == GuestSource::kUnreadable) &&
      !fixture.Write(guest, test.script)) {
    return false;
  }

  // A custom guest must not depend on the demo's file fixtures, even when
  // inherited selectors are set to missing files.
  const std::wstring missing = fixture.directory + L"\\missing.txt";
  const bool demo = test.source == GuestSource::kDemo || test.source == GuestSource::kEmptySelector;
  const wchar_t *guest_path = test.source == GuestSource::kDemo ? nullptr
      : test.source == GuestSource::kEmptySelector              ? L""
                                                                : guest.c_str();
  if (!SetEnvironment(L"V8HOST_GUEST_JS", guest_path) ||
      !SetEnvironment(L"SBOX_DEMO_ALLOWED", test.demo_files ? allowed.c_str() : missing.c_str()) ||
      !SetEnvironment(L"SBOX_DEMO_DENIED", test.demo_files ? denied.c_str() : missing.c_str())) {
    return false;
  }

  HANDLE locked = INVALID_HANDLE_VALUE;
  if (test.source == GuestSource::kUnreadable) {
    locked = ::CreateFileW(guest.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (locked == INVALID_HANDLE_VALUE) {
      printf("[v8host-tests] locking unreadable fixture failed: error=%lu\n", ::GetLastError());
      return false;
    }
  }
  Messages messages;
  messages.expected_strings = test.expected_strings;
  messages.expected_binaries = test.expected_binaries;
  messages.done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!messages.done) {
    printf("[v8host-tests] message event creation failed: error=%lu\n", ::GetLastError());
    if (locked != INVALID_HANDLE_VALUE)
      ::CloseHandle(locked);
    return false;
  }

  SboxPolicy policy = {};
  policy.struct_size = sizeof(policy);
  policy.initial_token = SBOX_TOKEN_RESTRICTED_SAME_ACCESS;
  policy.lockdown_token = SBOX_TOKEN_LOCKDOWN;
  policy.integrity = SBOX_INTEGRITY_LOW;
  policy.delayed_integrity = SBOX_INTEGRITY_UNTRUSTED;
  policy.prohibit_dynamic_code = 1;
  const SboxFileRule file_rule{allowed.c_str(), 1};
  if (test.demo_files) {
    policy.file_rules = &file_rule;
    policy.file_rule_count = 1;
  }

  SboxSession *session = sbox_broker_spawn(target.c_str(), &policy, &OnMessage, &messages);
  bool ok = session != nullptr;
  int exit_code = -1;
  if (session) {
    if (test.input == Input::kString || test.input == Input::kBoth) {
      constexpr char input[] = "ping from host";
      ok = sbox_broker_post_message(session, SBOX_MSG_STRING, input, sizeof(input) - 1) == 0 && ok;
    }
    if (test.input == Input::kBinary || test.input == Input::kBoth) {
      constexpr unsigned char input[] = {0xde, 0xad, 0xbe, 0xef};
      ok = sbox_broker_post_message(session, SBOX_MSG_BINARY, input, sizeof(input)) == 0 && ok;
    }
    if (test.input == Input::kInvalid) {
      constexpr char input[] = "invalid kind";
      ok = sbox_broker_post_message(session, 99, input, sizeof(input) - 1) == 0 && ok;
    }
    if (test.expected_strings || test.expected_binaries) {
      const DWORD wait = ::WaitForSingleObject(messages.done, 10000);
      if (wait != WAIT_OBJECT_0) {
        printf("[v8host-tests] case=%s reply wait failed: wait=%lu\n", test.name, wait);
        ok = false;
      }
    }
    ok = sbox_broker_close(session) == 0 && ok;
    exit_code = sbox_broker_wait(session);
  } else {
    printf("[v8host-tests] case=%s spawn failed\n", test.name);
  }
  if (locked != INVALID_HANDLE_VALUE)
    ::CloseHandle(locked);
  ::CloseHandle(messages.done);
  {
    std::lock_guard<std::mutex> lock(messages.mutex);
    ok = ok && exit_code == test.expected_exit && !messages.unexpected && messages.strings == test.expected_strings &&
        messages.binaries == test.expected_binaries && messages.ready == (demo ? 1 : 0);
  }
  printf(
      "[v8host-tests] case=%s exit=%d expected=%d -> %s\n",
      test.name,
      exit_code,
      test.expected_exit,
      ok ? "PASS" : "FAIL");
  return ok;
}

} // namespace

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char *selected = nullptr;
  if (argc == 3 && std::strcmp(argv[1], "--case") == 0)
    selected = argv[2];
  else if (argc != 2 || std::strcmp(argv[1], "--all") != 0) {
    printf("Usage: v8host_tests.exe --all | --case <name>\n");
    for (const auto &test : kCases)
      printf("  %s\n", test.name);
    return 2;
  }
  sbox_harden::HardenDllSearch();
  const std::wstring directory = sbox_harden::ExeDir();
  if (!sbox_harden::CheckTrust(sbox_harden::LoadedModulePath(L"sbox.dll"), "sbox.dll") ||
      !sbox_harden::CheckTrust(directory + L"v8host.exe", "v8host.exe") ||
      !sbox_harden::CheckTrust(directory + L"v8jsisb.dll", "v8jsisb.dll")) {
    return 3;
  }
  for (const wchar_t *name :
       {L"SBOX_USE_LPAC",
        L"SBOX_LPAC_SMOKE",
        L"SBOX_TIER",
        L"SBOX_FORCE_RELOCATE",
        L"V8HOST_ENGINE_DLL",
        L"V8HOST_SNAPSHOT"}) {
    if (!SetEnvironment(name, nullptr))
      return 4;
  }
  Fixture fixture;
  bool ok = fixture.Initialize() && fixture.Write(fixture.directory + L"\\allowed.txt", "allowed") &&
      fixture.Write(fixture.directory + L"\\denied.txt", "denied");
  size_t ran = 0;
  if (ok) {
    for (const auto &test : kCases) {
      if (selected && std::strcmp(selected, test.name) != 0)
        continue;
      ok = RunCase(test, fixture, directory + L"v8host.exe", ++ran) && ok;
    }
  }
  if (!ran) {
    printf("[v8host-tests] no test ran\n");
    ok = false;
  }
  ok = fixture.Cleanup() && ok;
  printf("[v8host-tests] %zu cases -> %s\n", ran, ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
