// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>

#include "v8host_client.h"
#include "v8host_client_test_config.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
std::string failure;

bool Fail(const std::string& reason) {
  if (failure.empty()) failure = reason;
  return false;
}
bool WinFail(const char* operation) {
  return Fail(std::string(operation) + " Win32=" + std::to_string(::GetLastError()));
}
struct Handle {
  HANDLE value = nullptr;
  Handle() = default;
  explicit Handle(HANDLE h) : value(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
  ~Handle() { if (value) ::CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};
bool SamePath(const std::wstring& a, const std::wstring& b) {
  return ::_wcsicmp(a.c_str(), b.c_str()) == 0;
}
std::wstring Absolute(const std::wstring& value) {
  std::error_code ec;
  auto result = fs::absolute(value, ec).lexically_normal();
  if (ec) { Fail("absolute path: " + ec.message()); return {}; }
  return result.wstring();
}
struct Options {
  std::wstring scenario, client_root, payload_root, payload_v2, layout;
  std::wstring role, client_dll, mode = L"dedicated", expected_image;
  DWORD hold_ms = 0;
  USHORT native = 0;
  bool keep = false, missing = false;
};
bool Parse(int argc, wchar_t** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    std::wstring key = argv[i], value;
    const auto eq = key.find(L'=');
    if (eq != std::wstring::npos) { value = key.substr(eq + 1); key.resize(eq); }
    else if (key != L"--keep-layout" && key != L"--expect-missing") {
      if (++i == argc) return Fail("missing argument value");
      value = argv[i];
    }
    if (key == L"--scenario") o->scenario = value;
    else if (key == L"--client-root") o->client_root = Absolute(value);
    else if (key == L"--payload-root") o->payload_root = Absolute(value);
    else if (key == L"--payload-root-v2") o->payload_v2 = Absolute(value);
    else if (key == L"--layout-dir") o->layout = Absolute(value);
    else if (key == L"--role") o->role = value;
    else if (key == L"--client-dll") o->client_dll = Absolute(value);
    else if (key == L"--mode") o->mode = value;
    else if (key == L"--expected-image") o->expected_image = Absolute(value);
    else if (key == L"--keep-layout") o->keep = true;
    else if (key == L"--expect-missing") o->missing = true;
    else if (key == L"--hold-ms" || key == L"--verify-native") {
      wchar_t* end = nullptr;
      const unsigned long number = std::wcstoul(value.c_str(), &end, 10);
      if (value.empty() || *end || value[0] == L'-' || number > 60000)
        return Fail("invalid numeric argument");
      if (key == L"--hold-ms") o->hold_ms = number;
      else o->native = static_cast<USHORT>(number);
    } else return Fail("unknown argument");
  }
  return failure.empty();
}
template<class Predicate>
bool Pump(Predicate done, DWORD timeout) {
  const auto deadline = ::GetTickCount64() + timeout;
  while (!done()) {
    if (::GetTickCount64() >= deadline) return false;
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) ::DispatchMessageW(&msg);
    ::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }
  return true;
}
std::vector<PROCESSENTRY32W> Processes() {
  std::vector<PROCESSENTRY32W> result;
  Handle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (!snapshot.value) { WinFail("CreateToolhelp32Snapshot"); return result; }
  PROCESSENTRY32W entry = {}; entry.dwSize = sizeof(entry);
  if (!::Process32FirstW(snapshot.value, &entry)) { WinFail("Process32First"); return result; }
  do {
    if (::_wcsicmp(entry.szExeFile, L"sbox.exe") == 0) result.push_back(entry);
  } while (::Process32NextW(snapshot.value, &entry));
  if (::GetLastError() != ERROR_NO_MORE_FILES) WinFail("Process32Next");
  return result;
}
bool Image(HANDLE process, std::wstring* path) {
  path->resize(32768);
  DWORD size = static_cast<DWORD>(path->size());
  if (!::QueryFullProcessImageNameW(process, 0, path->data(), &size)) return false;
  path->resize(size);
  return true;
}
bool Machine(HANDLE process, USHORT* machine, USHORT* native) {
  return ::IsWow64Process2(process, machine, native) != FALSE || WinFail("IsWow64Process2");
}
bool PeMachine(const std::wstring& path, USHORT expected) {
  Handle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file.value) return WinFail("PE open");
  IMAGE_DOS_HEADER dos = {};
  DWORD read = 0;
  if (!::ReadFile(file.value, &dos, sizeof(dos), &read, nullptr) ||
      read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
    return Fail("invalid DOS header");
  DWORD signature = 0; IMAGE_FILE_HEADER header = {};
  if (::SetFilePointer(file.value, dos.e_lfanew, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER ||
      !::ReadFile(file.value, &signature, sizeof(signature), &read, nullptr) ||
      read != sizeof(signature) || signature != IMAGE_NT_SIGNATURE ||
      !::ReadFile(file.value, &header, sizeof(header), &read, nullptr) ||
      read != sizeof(header)) return Fail("invalid PE header");
  std::printf("[xa] PE machine=0x%04x expected=0x%04x\n", header.Machine, expected);
  return header.Machine == expected || Fail("PE machine mismatch");
}
struct Dedicated {
  Handle broker, worker;
  bool Observe(const Options& o) {
    const auto entries = Processes();
    DWORD broker_pid = 0, worker_pid = 0;
    for (const auto& e : entries) if (e.th32ParentProcessID == ::GetCurrentProcessId()) {
      if (broker_pid) return Fail("multiple dedicated brokers");
      broker_pid = e.th32ProcessID;
    }
    for (const auto& e : entries) if (e.th32ParentProcessID == broker_pid) {
      if (worker_pid) return Fail("multiple dedicated workers");
      worker_pid = e.th32ProcessID;
    }
    if (!broker_pid || !worker_pid) return Fail("missing dedicated broker/worker");
    broker.value = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, broker_pid);
    worker.value = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, worker_pid);
    for (HANDLE h : {broker.value, worker.value}) {
      if (!h) return WinFail("OpenProcess dedicated");
      std::wstring image;
      USHORT machine = 0, native = 0;
      if (!Image(h, &image)) return WinFail("QueryFullProcessImageName");
      if (!Machine(h, &machine, &native)) return false;
      std::printf("[xa] pid=%lu image=%ls process_machine=0x%04x native_machine=0x%04x\n",
                  ::GetProcessId(h), image.c_str(), machine, native);
      if (!SamePath(image, o.expected_image) || machine != IMAGE_FILE_MACHINE_UNKNOWN ||
          native != o.native) return Fail("dedicated native image/machine mismatch");
    }
    return PeMachine(o.expected_image, o.native);
  }
  bool Reaped() {
    for (HANDLE h : {worker.value, broker.value}) {
      if (!h) continue;
      const DWORD wait = ::WaitForSingleObject(h, 15000);
      std::printf("[xa] reap pid=%lu wait=%lu\n", ::GetProcessId(h), wait);
      if (wait != WAIT_OBJECT_0) return Fail("dedicated process did not exit");
    }
    return true;
  }
};
struct Client {
  HMODULE module = nullptr;
  decltype(&v8host_initialize) initialize = nullptr;
  decltype(&v8host_session_open) open = nullptr;
  decltype(&v8host_session_set_callbacks) callbacks = nullptr;
  decltype(&v8host_session_start_run) start = nullptr;
  decltype(&v8host_run_post_message) post = nullptr;
  decltype(&v8host_session_close) close = nullptr;
  v8host::test::ConfigApi config;
  bool Load(const std::wstring& path) {
    module = ::LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) return WinFail("LoadLibraryEx client");
#define ENTRY(member, symbol) \
    member = reinterpret_cast<decltype(member)>(::GetProcAddress(module, #symbol)); \
    if (!member) return WinFail("GetProcAddress " #symbol)
    ENTRY(initialize, v8host_initialize);
    ENTRY(open, v8host_session_open);
    ENTRY(callbacks, v8host_session_set_callbacks);
    ENTRY(start, v8host_session_start_run);
    ENTRY(post, v8host_run_post_message);
    ENTRY(close, v8host_session_close);
#define SETTER(name) ENTRY(config.name, v8host_##name);
    V8HOST_TEST_CONFIG_SETTERS(SETTER)
#undef SETTER
#undef ENTRY
    // The DLL owns app-thread windows; leave it loaded until process exit.
    return initialize(V8HOST_ABI_VERSION) == v8host_status_ok || Fail("initialize");
  }
};
struct Capture {
  Client* client;
  v8host_session session = nullptr;
  v8host_run run = nullptr;
  DWORD thread = ::GetCurrentThreadId();
  std::vector<int> states, events, kinds;
  std::vector<v8host_status> statuses;
  std::vector<std::string> messages;
  int terminals = 0, disconnects = 0;
  bool valid = true;
  explicit Capture(Client* c) : client(c) {}
  ~Capture() { Close(); }
  void Close() { if (session) { client->close(session); session = nullptr; } }
  static void V8HOST_CDECL State(void* data, v8host_session session,
                                v8host_session_state state, v8host_status status) {
    auto& c = *static_cast<Capture*>(data);
    c.valid &= c.thread == ::GetCurrentThreadId() && session == c.session &&
               status == v8host_status_ok;
    c.states.push_back(state);
  }
  static void V8HOST_CDECL Run(void* data, v8host_run run,
                              v8host_run_event event, v8host_status status) {
    auto& c = *static_cast<Capture*>(data);
    c.valid &= c.thread == ::GetCurrentThreadId() && run == c.run;
    c.events.push_back(event); c.statuses.push_back(status);
    if (event != v8host_run_event_started) ++c.terminals;
  }
  static void V8HOST_CDECL Message(void* data, v8host_run run, v8host_message_kind kind,
                                  const void* bytes, size_t size) {
    auto& c = *static_cast<Capture*>(data);
    c.valid &= c.thread == ::GetCurrentThreadId() && run == c.run &&
        c.states == std::vector<int>{v8host_session_state_ready,
            v8host_session_state_worker_startup_ready, v8host_session_state_worker_security_ready};
    c.kinds.push_back(kind);
    c.messages.emplace_back(static_cast<const char*>(bytes), size);
  }
  static void V8HOST_CDECL Disconnect(void* data) {
    auto& c = *static_cast<Capture*>(data);
    c.valid &= c.thread == ::GetCurrentThreadId();
    ++c.disconnects;
  }
  v8host_callbacks Table() {
    v8host_callbacks table = {};
    table.struct_size = sizeof(table); table.user_data = this;
    table.on_session_state = State; table.on_run_event = Run;
    table.on_relay_message = Message; table.on_broker_disconnect = Disconnect;
    return table;
  }
};
bool ClientStep(const Options& o) {
  if (o.client_dll.empty() || (o.mode != L"shared" && o.mode != L"dedicated"))
    return Fail("client requires --client-dll and shared/dedicated mode");
  Client client;
  if (!client.Load(o.client_dll)) return false;
  Capture capture(&client);
  v8host::test::TestSessionConfig config;
  config.broker_mode = o.mode == L"shared" ? v8host_broker_mode_shared : v8host_broker_mode_dedicated;
  config.tier = v8host_tier_untrusted;
  config.prohibit_dynamic_code = true;
  config.initial_token = v8host_token_level_restricted_same_access;
  config.delayed_integrity = v8host_integrity_level_untrusted;
  v8host::test::SessionConfigure session_args{&client.config, &config};
  const auto begin = ::GetTickCount64();
  auto status = client.open(&v8host::test::ConfigureSession, &session_args, &capture.session);
  std::printf("[xa] session_open status=%d duration_ms=%llu\n", status, ::GetTickCount64() - begin);
  if (status != v8host_status_ok) return o.missing || Fail("session_open");
  if (!capture.session) return Fail("null session");
  auto table = capture.Table();
  if (client.callbacks(capture.session, &table) != v8host_status_ok) return Fail("set_callbacks");
  const std::string js =
      "host.onmessage=function(m){host.postMessage('js echo: '+m);host.complete();};";
  v8host::test::TestRunInputs input;
  input.payload = js.data(); input.payload_len = js.size();
  v8host::test::RunConfigure run_args{&client.config, &input};
  status = client.start(capture.session, &v8host::test::ConfigureRun, &run_args, &capture.run);
  if (status != v8host_status_ok) return o.missing || Fail("start_run");
  if (!capture.run) return Fail("null run");
  if (client.post(capture.run, v8host_message_kind_string, "alpha", 5) != v8host_status_ok)
    return o.missing || Fail("post_message");
  if (!Pump([&] { return capture.terminals || capture.disconnects; }, o.missing ? 14000 : 15000))
    return Fail("terminal/disconnect timeout");
  if (o.missing) {
    // No worker came up: no readiness, no completed run, no message.
    const bool failed = capture.disconnects || (!capture.statuses.empty() &&
        capture.statuses.back() != v8host_status_ok);
    bool completed = false;
    for (int event : capture.events) completed |= event == v8host_run_event_completed;
    for (int state : capture.states)
      completed |= state == v8host_session_state_worker_security_ready;
    return (failed && !completed && capture.messages.empty()) || Fail("missing native payload succeeded");
  }
  if (!capture.valid || capture.disconnects || capture.terminals != 1 ||
      capture.states != std::vector<int>{v8host_session_state_ready,
          v8host_session_state_worker_startup_ready, v8host_session_state_worker_security_ready} ||
      capture.events != std::vector<int>{v8host_run_event_started, v8host_run_event_completed} ||
      capture.statuses != std::vector<v8host_status>(2, v8host_status_ok) ||
      capture.kinds != std::vector<int>{v8host_message_kind_string} ||
      capture.messages != std::vector<std::string>{"js echo: alpha"})
    return Fail("readiness/FIFO/echo/sole-terminal oracle");
  Dedicated dedicated;
  if (o.native && !dedicated.Observe(o)) return false;
  std::printf("[client] result=OK pid=%lu duration_ms=%llu\n", ::GetCurrentProcessId(),
              ::GetTickCount64() - begin);
  std::fflush(stdout);
  const auto until = ::GetTickCount64() + o.hold_ms;
  Pump([&] { return ::GetTickCount64() >= until; }, o.hold_ms + 1000);
  capture.Close();
  return dedicated.Reaped();
}
std::wstring Quote(const std::wstring& text) {
  // All quoted arguments are absolute file paths (never trailing separators).
  return L"\"" + text + L"\"";
}
std::wstring ExecutablePath() {
  wchar_t exe[32768] = {};
  const DWORD size = ::GetModuleFileNameW(nullptr, exe, 32768);
  if (!size || size == 32768) { WinFail("GetModuleFileName"); return {}; }
  return exe;
}
struct Child {
  Handle process, output;
  DWORD pid = 0;
  std::string text;
  ~Child() {
    if (process.value && ::WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT) {
      std::printf("[xa] terminating owned client pid=%lu\n", pid);
      ::TerminateProcess(process.value, 1);
      ::WaitForSingleObject(process.value, 5000);
    }
  }
  bool Launch(const std::wstring& arguments, std::wstring exe = {}) {
    if (exe.empty()) exe = ExecutablePath();
    if (exe.empty()) return false;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!::CreatePipe(&read, &write, &sa, 0)) return WinFail("CreatePipe");
    output.value = read; Handle writer(write);
    if (!::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0)) return WinFail("SetHandleInformation");
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write; si.hStdError = write;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::wstring command = Quote(exe) + L" --role=client " + arguments;
    if (!::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &si, &pi)) return WinFail("CreateProcess client");
    process.value = pi.hProcess; pid = pi.dwProcessId; ::CloseHandle(pi.hThread);
    std::printf("[xa] launched client pid=%lu\n", pid);
    return true;
  }
  bool Drain() {
    for (;;) {
      DWORD available = 0;
      if (!::PeekNamedPipe(output.value, nullptr, 0, nullptr, &available, nullptr))
        return ::GetLastError() == ERROR_BROKEN_PIPE || WinFail("PeekNamedPipe");
      if (!available) return true;
      char bytes[4096]; DWORD read = 0;
      if (!::ReadFile(output.value, bytes, (std::min)(available, DWORD(sizeof(bytes))), &read, nullptr))
        return WinFail("ReadFile client");
      text.append(bytes, read);
      std::fwrite(bytes, 1, read, stdout);
    }
  }
  bool Ready() {
    const bool done = Pump([&] {
      if (!Drain()) return true;
      return text.find("[client] result=OK") != std::string::npos ||
          ::WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0;
    }, 20000);
    return (done && text.find("[client] result=OK") != std::string::npos) ||
        Fail("client did not report result=OK");
  }
  bool Wait(DWORD timeout = 30000) {
    const auto wait = ::WaitForSingleObject(process.value, timeout);
    Drain();
    DWORD code = STILL_ACTIVE;
    if (!::GetExitCodeProcess(process.value, &code)) return WinFail("GetExitCodeProcess");
    std::printf("[xa] client pid=%lu wait=%lu exit=%lu\n", pid, wait, code);
    return (wait == WAIT_OBJECT_0 && code == 0) || Fail("client exit/wait");
  }
};
struct Layout {
  std::wstring root;
  std::vector<std::wstring> links;
  bool keep = false;
  bool Create(const Options& o) {
    std::wstring candidate = o.layout;
    keep = o.keep;
    if (candidate.empty()) candidate = (fs::path(o.payload_root).parent_path() / L"layouts" /
        (L"xa-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
         std::to_wstring(::GetTickCount64()))).wstring();
    std::error_code ec;
    if (!fs::create_directories(candidate, ec)) return Fail("layout must be fresh: " + ec.message());
    root = candidate;
    std::printf("[xa] layout=%ls keep=%d\n", root.c_str(), keep);
    return true;
  }
  bool Link(const std::wstring& dir, const std::wstring& name, const std::wstring& source) {
    const auto target_dir = fs::path(root) / dir;
    std::error_code ec;
    fs::create_directories(target_dir, ec);
    if (ec) return Fail("layout directory: " + ec.message());
    const auto target = target_dir / name;
    std::printf("[xa] hardlink %ls <- %ls\n", target.c_str(), source.c_str());
    if (!::CreateHardLinkW(target.c_str(), source.c_str(), nullptr)) return WinFail("CreateHardLinkW");
    links.push_back(target.wstring());
    return true;
  }
  bool Payload(const std::wstring& dir, const std::wstring& source) {
    for (const wchar_t* name : {L"sbox.exe", L"v8host.dll", L"v8jsisb.dll"})
      if (!Link(dir, name, (fs::path(source) / name).wstring())) return false;
    return true;
  }
  bool Cleanup() {
    if (root.empty() || keep) return true;
    std::error_code ec;
    const auto until = ::GetTickCount64() + 30000;
    do {
      for (const auto& link : links) {
        // POSIX unlink removes just this link, including an executing harness
        // link or a read-only engine link, without altering the build output.
        Handle file(::CreateFileW(link.c_str(), DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.value) continue;
        FILE_DISPOSITION_INFO_EX info = {};
        info.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
            FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
        ::SetFileInformationByHandle(file.value, FileDispositionInfoEx, &info, sizeof(info));
      }
      fs::remove_all(root, ec);
      if (!ec) { root.clear(); return true; }
      ::Sleep(100);
    } while (::GetTickCount64() < until);
    return Fail("layout cleanup: " + ec.message());
  }
  ~Layout() { Cleanup(); }
};
bool CrossArchitecture(const Options& o, Layout& layout, USHORT machine, USHORT native) {
  const bool ec = o.scenario == L"arm64ec-client-arm64-payload";
#ifdef _M_ARM64EC
  const bool compiled_ec = true;
#else
  const bool compiled_ec = false;
#endif
  if (ec ? (!compiled_ec || machine != IMAGE_FILE_MACHINE_UNKNOWN || native != IMAGE_FILE_MACHINE_ARM64)
         : (machine != IMAGE_FILE_MACHINE_I386 || native != IMAGE_FILE_MACHINE_AMD64))
    return Fail("scenario process/native architecture requirement");
  if (o.client_root.empty()) return Fail("--client-root required");
  const std::wstring client_arch = ec ? L"x64" : L"x86";
  const std::wstring native_arch = ec ? L"arm64" : L"x64";
  if (!layout.Link(client_arch, L"v8host.dll", (fs::path(o.client_root) / L"v8host.dll").wstring()) ||
      !layout.Payload(native_arch, o.payload_root)) return false;
  Child positive;
  if (!positive.Launch(L"--mode=dedicated --client-dll " +
      Quote((fs::path(layout.root) / client_arch / L"v8host.dll").wstring()) +
      L" --expected-image " + Quote((fs::path(layout.root) / native_arch / L"sbox.exe").wstring()) +
      L" --verify-native=" + std::to_wstring(native)) || !positive.Wait()) return false;
  const auto missing_dir = fs::path(L"y") / client_arch;
  if (!layout.Link(missing_dir.wstring(), L"v8host.dll",
      (fs::path(o.client_root) / L"v8host.dll").wstring())) return false;
  const auto before = Processes();
  Child negative;
  const auto begin = ::GetTickCount64();
  if (!negative.Launch(L"--mode=dedicated --expect-missing --client-dll " +
      Quote((fs::path(layout.root) / missing_dir / L"v8host.dll").wstring()))) return false;
  bool unexpected = false;
  const bool stopped = Pump([&] {
    for (const auto& e : Processes()) {
      if (std::none_of(before.begin(), before.end(), [&](const PROCESSENTRY32W& old) {
          return old.th32ProcessID == e.th32ProcessID; })) unexpected = true;
    }
    return ::WaitForSingleObject(negative.process.value, 0) == WAIT_OBJECT_0;
  }, 15000);
  if (!stopped || unexpected) return Fail("missing sibling timed out or started sbox.exe");
  if (!negative.Wait(0)) return false;
  std::printf("[xa] missing-native-sibling fail-closed; no sbox observed; duration_ms=%llu\n",
              ::GetTickCount64() - begin);
  return true;
}
struct Fingerprint {
  LONGLONG size = 0;
  std::array<unsigned char, 32> hash = {};
};
bool Hash(const fs::path& path, Fingerprint* result) {
  Handle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file.value) return WinFail("hash open");
  LARGE_INTEGER size;
  if (!::GetFileSizeEx(file.value, &size)) return WinFail("GetFileSizeEx");
  result->size = size.QuadPart;
  BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
  if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
    return Fail("BCryptOpenAlgorithmProvider");
  bool ok = ::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
  unsigned char buffer[16384]; DWORD read = 0;
  while (ok) {
    if (!::ReadFile(file.value, buffer, sizeof(buffer), &read, nullptr)) { ok = false; break; }
    if (!read) break;
    ok = ::BCryptHashData(hash, buffer, read, 0) >= 0;
  }
  if (ok) ok = ::BCryptFinishHash(hash, result->hash.data(), 32, 0) >= 0;
  if (hash) ::BCryptDestroyHash(hash);
  ::BCryptCloseAlgorithmProvider(alg, 0);
  if (!ok) return Fail("SHA-256");
  std::printf("[xa] sha256 %ls size=%lld ", path.c_str(), result->size);
  for (auto byte : result->hash) std::printf("%02x", byte);
  std::printf("\n");
  return true;
}
struct Broker {
  DWORD pid = 0;
  ULONGLONG created = 0;
};
std::vector<Broker> Brokers(const std::wstring& image) {
  const auto entries = Processes();
  std::vector<PROCESSENTRY32W> matching;
  for (const auto& entry : entries) {
    Handle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
    std::wstring actual;
    if (h.value && Image(h.value, &actual) && SamePath(image, actual)) matching.push_back(entry);
  }
  std::vector<Broker> result;
  for (const auto& entry : matching) {
    if (std::any_of(matching.begin(), matching.end(), [&](const PROCESSENTRY32W& parent) {
        return parent.th32ProcessID == entry.th32ParentProcessID; })) continue;
    Handle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
    if (!h.value) continue;
    FILETIME created, exited, kernel, user;
    if (!::GetProcessTimes(h.value, &created, &exited, &kernel, &user)) {
      WinFail("GetProcessTimes"); continue;
    }
    result.push_back({entry.th32ProcessID,
        (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime});
  }
  return result;
}
bool OneBroker(const std::wstring& image, Broker* broker) {
  const auto found = Brokers(image);
  if (found.size() != 1) return Fail("expected exactly one broker, found " + std::to_string(found.size()));
  *broker = found.front();
  std::printf("[xa] broker pid=%lu created=%llu image=%ls\n",
              broker->pid, broker->created, image.c_str());
  return true;
}
bool SameBroker(const Broker& a, const Broker& b) { return a.pid == b.pid && a.created == b.created; }
bool SideBySide(const Options& o, Layout& layout, USHORT machine, USHORT native) {
  if (machine != IMAGE_FILE_MACHINE_UNKNOWN ||
      (native != IMAGE_FILE_MACHINE_AMD64 && native != IMAGE_FILE_MACHINE_ARM64))
    return Fail("side-by-side requires native x64/ARM64 process");
  if (o.payload_v2.empty()) return Fail("--payload-root-v2 required");
  for (const wchar_t* name : {L"sbox.exe", L"v8host.dll"}) {
    Fingerprint a, b;
    if (!Hash(fs::path(o.payload_root) / name, &a) || !Hash(fs::path(o.payload_v2) / name, &b)) return false;
    if (a.size == b.size && a.hash == b.hash) return Fail("v1 and v2 payload bytes identical");
  }
  const std::wstring arch = native == IMAGE_FILE_MACHINE_AMD64 ? L"x64" : L"arm64";
  const auto dir1 = fs::path(L"v1") / arch, dir2 = fs::path(L"v2") / arch;
  if (!layout.Payload(dir1.wstring(), o.payload_root) || !layout.Payload(dir2.wstring(), o.payload_v2))
    return false;
  const auto root1 = fs::path(layout.root) / dir1, root2 = fs::path(layout.root) / dir2;
  // Shared authentication requires the client process image inside the install
  // root, not just its client DLL. These are hard links to this build output.
  const std::wstring harness = L"v8host_cross_arch_tests.exe";
  if (!layout.Link(dir1.wstring(), harness, ExecutablePath()) ||
      !layout.Link(dir2.wstring(), harness, ExecutablePath())) return false;
  const auto exe1 = (root1 / harness).wstring(), exe2 = (root2 / harness).wstring();
  const auto image1 = (root1 / L"sbox.exe").wstring(), image2 = (root2 / L"sbox.exe").wstring();
  auto args = [](const fs::path& root, DWORD hold) {
    return L"--mode=shared --client-dll " + Quote((root / L"v8host.dll").wstring()) +
        L" --hold-ms=" + std::to_wstring(hold);
  };
  Child a, b, c;
  Broker pid1, pid2, rollback;
  if (!a.Launch(args(root1, 12000), exe1) || !a.Ready() || !OneBroker(image1, &pid1) ||
      !b.Launch(args(root2, 12000), exe2) || !b.Ready() || !OneBroker(image2, &pid2)) return false;
  if (pid1.pid == pid2.pid || !OneBroker(image1, &rollback) || !SameBroker(pid1, rollback))
    return Fail("update did not preserve separate v1/v2 brokers");
  if (!c.Launch(args(root1, 0), exe1) || !c.Ready() || !OneBroker(image1, &rollback) ||
      !SameBroker(pid1, rollback)) return Fail("rollback did not reuse PID1");
  if (!c.Wait() || !a.Wait() || !b.Wait()) return false;
  const auto idle = ::GetTickCount64();
  if (!Pump([&] { return Brokers(image1).empty() && Brokers(image2).empty(); }, 30000))
    return Fail("idle brokers did not retire");
  std::printf("[xa] both brokers retired duration_ms=%llu\n", ::GetTickCount64() - idle);
  Child d; Broker restarted;
  if (!d.Launch(args(root1, 1000), exe1) || !d.Ready() || !OneBroker(image1, &restarted) ||
      SameBroker(pid1, restarted)) return Fail("retirement did not create a new broker");
  if (!d.Wait()) return false;
  if (!Pump([&] { return Brokers(image1).empty() && Brokers(image2).empty(); }, 30000))
    return Fail("final broker did not retire");
  return true;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Options options;
  bool ok = Parse(argc, argv, &options);
  if (ok && options.role == L"client") {
    ok = ClientStep(options);
    ok = ok && failure.empty();
    if (!ok) std::printf("[client] result=FAIL - %s\n", failure.c_str());
    return ok ? 0 : 1;
  }
  Layout layout;
  USHORT machine = 0, native = 0;
  if (ok) ok = Machine(::GetCurrentProcess(), &machine, &native);
  std::printf("[xa] harness pid=%lu process_machine=0x%04x native_machine=0x%04x\n",
              ::GetCurrentProcessId(), machine, native);
  if (ok && options.payload_root.empty()) ok = Fail("--payload-root required");
  if (ok) ok = layout.Create(options);
  if (ok) {
    if (options.scenario == L"x86-client-x64-payload" ||
        options.scenario == L"arm64ec-client-arm64-payload")
      ok = CrossArchitecture(options, layout, machine, native);
    else if (options.scenario == L"side-by-side-update-rollback")
      ok = SideBySide(options, layout, machine, native);
    else ok = Fail("unknown --scenario");
  }
  // Failure paths can close clients before a shared broker's idle grace expires.
  if (!ok && !layout.root.empty()) {
    Pump([&] {
      for (const auto& entry : Processes()) {
        Handle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
        std::wstring image;
        if (h.value && Image(h.value, &image) && image.find(layout.root + L"\\") == 0) return false;
      }
      return true;
    }, 30000);
  }
  if (!layout.Cleanup()) ok = false;
  ok = ok && failure.empty();
  std::printf("cross-arch/%ls: %s%s%s\n", options.scenario.c_str(),
              ok ? "PASS" : "FAIL", ok ? "" : " - ", ok ? "" : failure.c_str());
  return ok ? 0 : 1;
}
