// Release-flavor installed-layout negatives. This executable compiles the
// identity and payload-path code WITHOUT SBOX_DEV_ALLOW_UNSIGNED, so it runs the
// policy a shipped binary has: unsigned payloads and non-architecture parents
// fail closed. Positive installed-layout runs are in v8host_broker_tests.
#ifdef SBOX_DEV_ALLOW_UNSIGNED
#error "the release-flavor layout tests must build without SBOX_DEV_ALLOW_UNSIGNED"
#endif

#include "v8host_test_support.h"

#include "v8host_file_identity.h"
#include "v8host_payload_identity.h"
#include "v8host_peer_auth.h"

#include <winioctl.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

using v8host::test::ExecutableDirectory;

bool Fail(std::string* detail, const char* message) {
  *detail = message;
  return false;
}

bool ReleaseTrustPolicy(std::string* detail) {
  if (v8host::TrustAllowed(v8host::TrustStatus::kUnsigned) ||
      v8host::TrustAllowed(v8host::TrustStatus::kInvalid) ||
      v8host::TrustAllowed(v8host::TrustStatus::kError) ||
      !v8host::TrustAllowed(v8host::TrustStatus::kSignedValid))
    return Fail(detail, "release trust policy accepted an unsigned or invalid payload");
  return true;
}

// The dev build outputs next to this exe are unsigned: no identity may form.
bool UnsignedPayloadRejected(std::string* detail) {
  v8host::PayloadIdentity identity;
  DWORD error = ERROR_SUCCESS;
  const std::wstring dir = ExecutableDirectory();
  if (v8host::ResolvePayloadIdentity(dir, L"v8host_layout_release_tests.exe",
                                     L"v8host_layout_release_tests.exe", &identity, &error))
    return Fail(detail, "an unsigned payload resolved to an identity");
  if (error == ERROR_SUCCESS) return Fail(detail, "rejection did not set an error");
  if (identity.container || identity.plugin) return Fail(detail, "rejected identity kept held files");
  return true;
}

bool InstallRootDerivation(std::string* detail) {
  constexpr uint16_t kI386 = 0x014C, kAmd64 = 0x8664, kArm64 = 0xAA64;
  struct Row {
    const wchar_t* image;
    uint16_t machine;
    const wchar_t* root;  // nullptr: must be rejected
  };
  const Row rows[] = {
      {L"C:\\app\\x64\\sbox.exe", kAmd64, L"C:\\app"},
      {L"C:\\app\\X64\\sbox.exe", kAmd64, L"C:\\app"},
      {L"C:\\app\\arm64\\sbox.exe", kArm64, L"C:\\app"},
      {L"\\\\?\\Volume{a}\\app\\x64\\sbox.exe", kAmd64, L"\\\\?\\Volume{a}\\app"},
      {L"C:\\app\\arm64\\sbox.exe", kAmd64, nullptr},          // wrong architecture directory
      {L"C:\\app\\x64\\sbox.exe", kArm64, nullptr},
      {L"C:\\app\\x86\\sbox.exe", kAmd64, nullptr},
      {L"C:\\app\\x86\\sbox.exe", kI386, nullptr},             // no 32-bit payload
      {L"C:\\app\\x64-evil\\sbox.exe", kAmd64, nullptr},       // sibling prefix
      {L"C:\\app\\ax64\\sbox.exe", kAmd64, nullptr},
      {L"C:\\app\\sandbox-x64\\sbox.exe", kAmd64, nullptr},    // developer layout
      {L"C:\\app\\sbox.exe", kAmd64, nullptr},                 // no architecture directory
      {L"sbox.exe", kAmd64, nullptr},
  };
  for (const Row& row : rows) {
    std::wstring root = L"unchanged";
    const bool ok = v8host::DeriveInstallRoot(row.image, row.machine, &root);
    if (row.root ? (!ok || root != row.root) : (ok || root != L"unchanged"))
      return Fail(detail, "install root derivation");
  }
  return true;
}

// A junction named for the architecture can't make another directory look like
// an installed layout: the held file's final path names the real directory.
bool CreateJunction(const std::filesystem::path& link, const std::filesystem::path& target) {
  std::error_code ec;
  if (!std::filesystem::create_directory(link, ec) || ec) return false;
  const HANDLE dir = ::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (dir == INVALID_HANDLE_VALUE) return false;
  // Mount-point reparse buffer: tag, data length, reserved, then the
  // substitute/print name offsets and lengths, then the NUL-terminated names.
  const std::wstring substitute = L"\\??\\" + target.wstring();
  const USHORT name_bytes = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
  std::vector<uint8_t> buffer(8 + 8 + name_bytes + 4, 0);
  const auto put16 = [&](size_t at, uint16_t value) { std::memcpy(&buffer[at], &value, 2); };
  const uint32_t tag = IO_REPARSE_TAG_MOUNT_POINT;
  std::memcpy(&buffer[0], &tag, 4);
  put16(4, static_cast<uint16_t>(8 + name_bytes + 4));  // data length
  put16(8, 0);                                          // substitute offset
  put16(10, name_bytes);                                // substitute length
  put16(12, static_cast<uint16_t>(name_bytes + 2));     // print offset (empty name)
  put16(14, 0);                                         // print length
  std::memcpy(&buffer[16], substitute.data(), name_bytes);
  DWORD returned = 0;
  const bool ok = ::DeviceIoControl(dir, FSCTL_SET_REPARSE_POINT, buffer.data(),
                                    static_cast<DWORD>(buffer.size()), nullptr, 0, &returned,
                                    nullptr) != FALSE;
  ::CloseHandle(dir);
  return ok;
}

bool ReparseOutsideRoot(std::string* detail) {
  namespace fs = std::filesystem;
  const fs::path base = fs::path(ExecutableDirectory()) / L"layouts-release" /
                        (L"rp-" + std::to_wstring(::GetCurrentProcessId()));
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base / L"elsewhere", ec);
  fs::create_directories(base / L"root", ec);
  if (ec) return Fail(detail, "fixture directories");
  wchar_t self[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, self, MAX_PATH);
  // A hard link of this exe stands in for a payload image: no bytes are written.
  if (!::CreateHardLinkW((base / L"elsewhere" / L"sbox.exe").c_str(), self, nullptr))
    return Fail(detail, "fixture hard link");
  if (!CreateJunction(base / L"root" / L"x64", base / L"elsewhere"))
    return Fail(detail, "fixture junction");
  v8host::HeldFile held;
  DWORD error = 0;
  bool ok = v8host::OpenImmutableFile((base / L"root" / L"x64" / L"sbox.exe").wstring(), &held, &error);
  std::wstring root = L"unchanged";
  if (!ok) {
    *detail = "image behind the junction did not open";
  } else if (v8host::DeriveInstallRoot(held.final_path(), held.machine(), &root)) {
    *detail = "a junction named for the architecture produced an install root";
    ok = false;
  } else if (root != L"unchanged") {
    *detail = "rejection changed the output";
    ok = false;
  }
  held = v8host::HeldFile();
  // Best effort: a hard link of a running image can't be deleted while it runs.
  fs::remove_all(base / L"root", ec);
  fs::remove_all(base, ec);
  return ok;
}

bool PayloadFinalPathPolicy(std::string* detail) {
  namespace fs = std::filesystem;
  const fs::path base = fs::path(ExecutableDirectory()) / L"layouts-release" /
                        (L"contain-" + std::to_wstring(::GetCurrentProcessId()));
  const fs::path root = base / L"root";
  const fs::path outside = base / L"root-evil";
  std::error_code ec;
  fs::create_directories(root / L"x64", ec);
  fs::create_directories(outside, ec);
  if (ec) return Fail(detail, "containment fixture directories");
  wchar_t self[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, self, MAX_PATH);
  if (!::CreateHardLinkW((root / L"x64" / L"image.dll").c_str(), self, nullptr) ||
      !::CreateHardLinkW((outside / L"image.dll").c_str(), self, nullptr) ||
      !CreateJunction(root / L"x64" / L"redirect", outside))
    return Fail(detail, "containment hard links/junction");
  bool ok = false;
  {
    v8host::HeldFile contained, escaped, redirected, leaf;
    DWORD error = 0;
    ok = v8host::OpenImmutableFile((root / L"x64" / L"image.dll").wstring(), &contained, &error) &&
         v8host::OpenImmutableFile((outside / L"image.dll").wstring(), &escaped, &error) &&
         v8host::OpenImmutableFile((root / L"x64" / L"redirect" / L"image.dll").wstring(),
                                   &redirected, &error);
    const std::wstring canonical_root =
        v8host::ParentPath(v8host::ParentPath(contained.final_path()));
    ok = ok && v8host::IsPayloadFileAllowed(canonical_root, contained) &&
         !v8host::IsPayloadFileAllowed(canonical_root, escaped) &&
         !v8host::IsPayloadFileAllowed(canonical_root, redirected) &&
         !v8host::IsPayloadFileAllowed(contained.final_path(), contained) &&
         !v8host::OpenImmutableFile((root / L"x64" / L"redirect").wstring(), &leaf, &error);
  }
  fs::remove_all(root / L"x64" / L"redirect", ec);
  fs::remove_all(base, ec);
  return ok || Fail(detail, "contained hard link / sibling prefix / parent junction / final reparse policy");
}

// The release broker admits an unsigned application client (an Office developer
// build, for one): this unsigned exe connects to itself through a pipe and the
// real WinVerifyTrust runs. Without the allowance the release policy rejects it.
bool UnsignedClientAccepted(std::string* detail) {
  const std::wstring name = L"\\\\.\\pipe\\v8host-release-authtest-" +
                            std::to_wstring(::GetCurrentProcessId()) + L"-" +
                            std::to_wstring(::GetTickCount64());
  const HANDLE server = ::CreateNamedPipeW(
      name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
      1, 256, 256, 0, nullptr);
  if (server == INVALID_HANDLE_VALUE) return Fail(detail, "pipe server");
  const HANDLE client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
  bool ok = false;
  if (client == INVALID_HANDLE_VALUE) {
    *detail = "pipe client";
  } else {
    ::ConnectNamedPipe(server, nullptr);
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    v8host::HeldFile image;
    v8host::PeerAuthPolicy policy = {};
    policy.peer_side = v8host::PeerSide::kPipeClient;
    policy.mode = v8host::BrokerMode::kDedicated;
    DWORD error = ERROR_SUCCESS;
    if (!v8host::OpenImmutableFile(self, &image, &error) ||
        !v8host::QueryCurrentSidAndSession(&policy.expected_sid, &policy.expected_session,
                                           &error)) {
      *detail = "self identity";
    } else if (v8host::VerifyTrust(image) != v8host::TrustStatus::kUnsigned) {
      *detail = "the test exe must be unsigned for this case";
    } else {
      v8host::HeldProcess peer;
      if (v8host::AuthenticatePipePeer(server, policy, &peer, &error) ||
          error != static_cast<DWORD>(TRUST_E_NOSIGNATURE)) {
        *detail = "an unsigned peer without the allowance was not rejected with 0x800b0100";
      } else {
        policy.allow_unsigned_image = true;
        ok = v8host::AuthenticatePipePeer(server, policy, &peer, &error);
        if (!ok) {
          char text[96];
          std::snprintf(text, sizeof(text), "unsigned client rejected with 0x%08lx", error);
          *detail = text;
        }
      }
    }
    ::CloseHandle(client);
  }
  ::CloseHandle(server);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<v8host::test::TestCase> tests = {
      {"layout-release", "release-trust-policy", ReleaseTrustPolicy},
      {"layout-release", "unsigned-payload-rejected", UnsignedPayloadRejected},
      {"layout-release", "install-root-derivation", InstallRootDerivation},
      {"layout-release", "reparse-outside-root", ReparseOutsideRoot},
      {"layout-release", "payload-final-path-policy", PayloadFinalPathPolicy},
      {"layout-release", "unsigned-client-accepted", UnsignedClientAccepted},
  };
  return v8host::test::RunTests(argc, argv, tests);
}
