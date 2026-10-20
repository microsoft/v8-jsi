#include "v8host_file_identity.h"

#include <bcrypt.h>
#include <softpub.h>
#include <wintrust.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace v8host {
#ifdef V8HOST_FILE_IDENTITY_TESTING
thread_local bool fail_final_path = false;
void FailFinalPathForTesting(bool fail) { fail_final_path = fail; }
#endif
namespace {

void CloseHandleIfValid(HANDLE handle) {
  if (handle && handle != INVALID_HANDLE_VALUE)
    ::CloseHandle(handle);
}

bool ReadAt(HANDLE file, uint64_t offset, void* data, DWORD size) {
  LARGE_INTEGER position = {};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!::SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
    return false;
  DWORD read = 0;
  return ::ReadFile(file, data, size, &read, nullptr) && read == size;
}

bool ReadFinalPath(HANDLE file, std::wstring* path) {
#ifdef V8HOST_FILE_IDENTITY_TESTING
  if (fail_final_path) { ::SetLastError(ERROR_ACCESS_DENIED); return false; }
#endif
  const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_GUID;
  DWORD size = ::GetFinalPathNameByHandleW(file, nullptr, 0, flags);
  if (!size)
    return false;
  std::wstring value(size, L'\0');
  DWORD written =
      ::GetFinalPathNameByHandleW(file, value.data(), size, flags);
  if (!written || written >= size)
    return false;
  value.resize(written);
  *path = std::move(value);
  return true;
}

bool ReadMachine(HANDLE file, uint16_t* machine) {
  IMAGE_DOS_HEADER dos = {};
  if (!ReadAt(file, 0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew <= 0)
    return false;
  DWORD signature = 0;
  IMAGE_FILE_HEADER header = {};
  const uint64_t offset = static_cast<uint64_t>(dos.e_lfanew);
  if (!ReadAt(file, offset, &signature, sizeof(signature)) ||
      signature != IMAGE_NT_SIGNATURE ||
      !ReadAt(file, offset + sizeof(signature), &header, sizeof(header)))
    return false;
  *machine = header.Machine;
  return true;
}

bool HashFile(HANDLE file, std::array<uint8_t, 32>* digest) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_size = 0;
  DWORD result_size = 0;
  std::vector<uint8_t> object;
  bool ok =
      BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
          &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
      BCRYPT_SUCCESS(::BCryptGetProperty(
          algorithm, BCRYPT_OBJECT_LENGTH,
          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
          &result_size, 0));
  if (ok) {
    object.resize(object_size);
    ok = BCRYPT_SUCCESS(::BCryptCreateHash(
        algorithm, &hash, object.data(), object_size, nullptr, 0, 0));
  }
  LARGE_INTEGER original = {};
  LARGE_INTEGER zero = {};
  if (ok)
    ok = ::SetFilePointerEx(file, zero, &original, FILE_CURRENT) != FALSE &&
         ::SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != FALSE;
  std::array<uint8_t, 64 * 1024> buffer = {};
  while (ok) {
    DWORD read = 0;
    if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                    &read, nullptr)) {
      ok = false;
      break;
    }
    if (!read)
      break;
    ok = BCRYPT_SUCCESS(
        ::BCryptHashData(hash, buffer.data(), read, 0));
  }
  if (ok)
    ok = BCRYPT_SUCCESS(
        ::BCryptFinishHash(hash, digest->data(), digest->size(), 0));
  if (original.QuadPart >= 0)
    ::SetFilePointerEx(file, original, nullptr, FILE_BEGIN);
  if (hash)
    ::BCryptDestroyHash(hash);
  if (algorithm)
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
  return ok;
}

bool QueryTokenIdentity(HANDLE token,
                        std::vector<uint8_t>* sid,
                        LUID* session) {
  DWORD size = 0;
  ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size)
    return false;
  std::vector<uint8_t> user_data(size);
  if (!::GetTokenInformation(token, TokenUser, user_data.data(), size, &size))
    return false;
  auto* user = reinterpret_cast<TOKEN_USER*>(user_data.data());
  if (!::IsValidSid(user->User.Sid))
    return false;
  const DWORD sid_size = ::GetLengthSid(user->User.Sid);
  sid->resize(sid_size);
  if (!::CopySid(sid_size, sid->data(), user->User.Sid))
    return false;
  TOKEN_STATISTICS statistics = {};
  size = sizeof(statistics);
  if (!::GetTokenInformation(token, TokenStatistics, &statistics,
                             sizeof(statistics), &size))
    return false;
  *session = statistics.AuthenticationId;
  return true;
}

}  // namespace

HeldFile::~HeldFile() {
  CloseHandleIfValid(handle_);
}

HeldFile::HeldFile(HeldFile&& other) noexcept {
  *this = std::move(other);
}

HeldFile& HeldFile::operator=(HeldFile&& other) noexcept {
  if (this != &other) {
    CloseHandleIfValid(handle_);
    handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
    final_path_ = std::move(other.final_path_);
    machine_ = other.machine_;
    sha256_ = other.sha256_;
  }
  return *this;
}

HeldProcess::~HeldProcess() {
  CloseHandleIfValid(token_);
  CloseHandleIfValid(process_);
}

HeldProcess::HeldProcess(HeldProcess&& other) noexcept {
  *this = std::move(other);
}

HeldProcess& HeldProcess::operator=(HeldProcess&& other) noexcept {
  if (this != &other) {
    CloseHandleIfValid(token_);
    CloseHandleIfValid(process_);
    process_ = std::exchange(other.process_, nullptr);
    token_ = std::exchange(other.token_, nullptr);
    pid_ = other.pid_;
    sid_ = std::move(other.sid_);
    logon_session_ = other.logon_session_;
    creation_time_ = other.creation_time_;
    image_ = std::move(other.image_);
  }
  return *this;
}

bool OpenImmutableFile(const std::wstring& path, HeldFile* out, DWORD* error,
                       FinalPathMode final_path) {
  if (!out || path.empty()) {
    if (error)
      *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  HeldFile result;
  result.handle_ =
      ::CreateFileW(path.c_str(),
                    FILE_READ_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL,
                    FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL
#if !defined(SBOX_DEV_ALLOW_UNSIGNED)
                        | FILE_FLAG_OPEN_REPARSE_POINT
#endif
                    , nullptr);
  if (result.handle_ == INVALID_HANDLE_VALUE) {
    if (error)
      *error = ::GetLastError();
    return false;
  }
  BY_HANDLE_FILE_INFORMATION info = {};
  FILE_STANDARD_INFO standard = {};
  if (::GetFileType(result.handle_) != FILE_TYPE_DISK ||
      !::GetFileInformationByHandle(result.handle_, &info) ||
      !::GetFileInformationByHandleEx(result.handle_, FileStandardInfo,
                                      &standard, sizeof(standard)) ||
      standard.Directory || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
#if !defined(SBOX_DEV_ALLOW_UNSIGNED)
      (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
#endif
      (final_path != FinalPathMode::kOmitForWorkerHold &&
       !ReadFinalPath(result.handle_, &result.final_path_)) ||
      !ReadMachine(result.handle_, &result.machine_) ||
      !HashFile(result.handle_, &result.sha256_)) {
    if (error)
      *error = ::GetLastError() ? ::GetLastError() : ERROR_BAD_EXE_FORMAT;
    return false;
  }
  *out = std::move(result);
  if (error)
    *error = ERROR_SUCCESS;
  return true;
}

TrustStatus VerifyTrust(const HeldFile& file) {
  if (!file)
    return TrustStatus::kError;
  WINTRUST_FILE_INFO info = {};
  info.cbStruct = sizeof(info);
  info.pcwszFilePath = file.final_path().c_str();
  info.hFile = file.get();
  WINTRUST_DATA data = {};
  data.cbStruct = sizeof(data);
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &info;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  data.dwProvFlags = WTD_SAFER_FLAG;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG status =
      ::WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action,
                       &data);
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  ::WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
  if (status == ERROR_SUCCESS)
    return TrustStatus::kSignedValid;
  if (status == TRUST_E_NOSIGNATURE)
    return TrustStatus::kUnsigned;
  return TrustStatus::kInvalid;
}

bool TrustAllowed(TrustStatus trust) {
  if (trust == TrustStatus::kSignedValid)
    return true;
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
  return trust == TrustStatus::kUnsigned;
#else
  return false;
#endif
}

bool QueryProcessIdentity(DWORD pid, HeldProcess* out, DWORD* error) {
  if (!out || !pid) {
    if (error)
      *error = ERROR_INVALID_PARAMETER;
    return false;
  }
  HeldProcess result;
  result.process_ = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                  FALSE, pid);
  if (!result.process_ || ::GetProcessId(result.process_) != pid) {
    if (error)
      *error = ::GetLastError();
    return false;
  }
  result.pid_ = pid;
  if (!::OpenProcessToken(result.process_, TOKEN_QUERY, &result.token_) ||
      !QueryTokenIdentity(result.token_, &result.sid_, &result.logon_session_)) {
    if (error)
      *error = ::GetLastError();
    return false;
  }
  FILETIME exit_time = {}, kernel_time = {}, user_time = {};
  if (!::GetProcessTimes(result.process_, &result.creation_time_, &exit_time,
                         &kernel_time, &user_time)) {
    if (error)
      *error = ::GetLastError();
    return false;
  }
  std::wstring path(32768, L'\0');
  DWORD path_size = static_cast<DWORD>(path.size());
  if (!::QueryFullProcessImageNameW(result.process_, 0, path.data(),
                                    &path_size)) {
    if (error)
      *error = ::GetLastError();
    return false;
  }
  path.resize(path_size);
  if (!OpenImmutableFile(path, &result.image_, error))
    return false;
  *out = std::move(result);
  if (error)
    *error = ERROR_SUCCESS;
  return true;
}

bool QueryCurrentSidAndSession(std::vector<uint8_t>* sid,
                               LUID* logon_session,
                               DWORD* error) {
  HANDLE token = nullptr;
  if (!sid || !logon_session ||
      !::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
    if (error)
      *error = ::GetLastError();
    return false;
  }
  const bool ok = QueryTokenIdentity(token, sid, logon_session);
  const DWORD last_error = ok ? ERROR_SUCCESS : ::GetLastError();
  ::CloseHandle(token);
  if (error)
    *error = last_error;
  return ok;
}

bool EqualSidBytes(const std::vector<uint8_t>& left,
                   const std::vector<uint8_t>& right) {
  PSID left_sid = const_cast<uint8_t*>(left.data());
  PSID right_sid = const_cast<uint8_t*>(right.data());
  return !left.empty() && !right.empty() && ::IsValidSid(left_sid) &&
         ::IsValidSid(right_sid) && ::EqualSid(left_sid, right_sid) != FALSE;
}

bool EqualLuid(const LUID& left, const LUID& right) {
  return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

bool IsStrictDescendant(const std::wstring& root,
                        const std::wstring& candidate) {
  if (root.empty() || candidate.size() <= root.size())
    return false;
  if (::CompareStringOrdinal(root.data(), static_cast<int>(root.size()),
                             candidate.data(), static_cast<int>(root.size()),
                             TRUE) != CSTR_EQUAL)
    return false;
  const wchar_t boundary = candidate[root.size()];
  return (root.back() == L'\\' || root.back() == L'/') ||
         boundary == L'\\' || boundary == L'/';
}

bool IsPayloadFileAllowed(const std::wstring& install_root, const HeldFile& file) {
#if defined(SBOX_DEV_ALLOW_UNSIGNED)
  return true;
#else
  return file && IsStrictDescendant(install_root, file.final_path());
#endif
}

bool IsSameFileIdentity(const HeldFile& left, const HeldFile& right) {
  return left && right && left.machine() == right.machine() &&
         left.sha256() == right.sha256() &&
         ::CompareStringOrdinal(left.final_path().c_str(), -1,
                                right.final_path().c_str(), -1,
                                TRUE) == CSTR_EQUAL;
}

std::wstring ParentPath(const std::wstring& path) {
  const size_t separator = path.find_last_of(L"\\/");
  return separator == std::wstring::npos ? std::wstring()
                                         : path.substr(0, separator);
}

}  // namespace v8host
