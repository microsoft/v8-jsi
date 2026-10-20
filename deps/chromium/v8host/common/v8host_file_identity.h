#ifndef V8HOST_COMMON_V8HOST_FILE_IDENTITY_H_
#define V8HOST_COMMON_V8HOST_FILE_IDENTITY_H_

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace v8host {

enum class TrustStatus { kSignedValid, kUnsigned, kInvalid, kError };

enum class FinalPathMode { kRequired, kOmitForWorkerHold };

#ifdef V8HOST_FILE_IDENTITY_TESTING
void FailFinalPathForTesting(bool fail);
#endif

class HeldFile {
 public:
  HeldFile() = default;
  ~HeldFile();
  HeldFile(HeldFile&& other) noexcept;
  HeldFile& operator=(HeldFile&& other) noexcept;
  HeldFile(const HeldFile&) = delete;
  HeldFile& operator=(const HeldFile&) = delete;

  HANDLE get() const { return handle_; }
  const std::wstring& final_path() const { return final_path_; }
  uint16_t machine() const { return machine_; }
  const std::array<uint8_t, 32>& sha256() const { return sha256_; }
  explicit operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }

 private:
  friend bool OpenImmutableFile(const std::wstring&, HeldFile*, DWORD*, FinalPathMode);
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  std::wstring final_path_;
  uint16_t machine_ = 0;
  std::array<uint8_t, 32> sha256_ = {};
};

class HeldProcess {
 public:
  HeldProcess() = default;
  ~HeldProcess();
  HeldProcess(HeldProcess&& other) noexcept;
  HeldProcess& operator=(HeldProcess&& other) noexcept;
  HeldProcess(const HeldProcess&) = delete;
  HeldProcess& operator=(const HeldProcess&) = delete;

  HANDLE process() const { return process_; }
  HANDLE token() const { return token_; }
  DWORD pid() const { return pid_; }
  const std::vector<uint8_t>& sid() const { return sid_; }
  const LUID& logon_session() const { return logon_session_; }
  const FILETIME& creation_time() const { return creation_time_; }
  const HeldFile& image() const { return image_; }
  HeldFile& image() { return image_; }
  explicit operator bool() const { return process_ != nullptr; }

 private:
  friend bool QueryProcessIdentity(DWORD, HeldProcess*, DWORD*);
  HANDLE process_ = nullptr;
  HANDLE token_ = nullptr;
  DWORD pid_ = 0;
  std::vector<uint8_t> sid_;
  LUID logon_session_ = {};
  FILETIME creation_time_ = {};
  HeldFile image_;
};

bool OpenImmutableFile(const std::wstring& path, HeldFile* out, DWORD* error,
                       FinalPathMode final_path = FinalPathMode::kRequired);
TrustStatus VerifyTrust(const HeldFile& file);
bool TrustAllowed(TrustStatus trust);
bool QueryProcessIdentity(DWORD pid, HeldProcess* out, DWORD* error);
bool QueryCurrentSidAndSession(std::vector<uint8_t>* sid,
                               LUID* logon_session,
                               DWORD* error);
bool EqualSidBytes(const std::vector<uint8_t>& left,
                   const std::vector<uint8_t>& right);
bool EqualLuid(const LUID& left, const LUID& right);
bool IsStrictDescendant(const std::wstring& root,
                        const std::wstring& candidate);
// Release payloads must resolve strictly below the container-derived root.
// Both arguments use held-file final paths (including volume-GUID prefixes).
bool IsPayloadFileAllowed(const std::wstring& install_root, const HeldFile& file);
bool IsSameFileIdentity(const HeldFile& left, const HeldFile& right);
std::wstring ParentPath(const std::wstring& path);

}  // namespace v8host

#endif  // V8HOST_COMMON_V8HOST_FILE_IDENTITY_H_
