// sbox_harden.h — header-only DLL-load hardening helpers shared by the app
// binaries (test_app.exe, v8host.exe). The sandbox engine is a swappable on-disk
// DLL, so a planted sbox.dll would replace the broker. These helpers restrict
// the loader search path and verify Authenticode signatures.
#ifndef SANDBOX_DLL_SBOX_HARDEN_H_
#define SANDBOX_DLL_SBOX_HARDEN_H_

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <softpub.h>
#include <wintrust.h>

#include <cstdio>
#include <string>

namespace sbox_harden {

// Restrict where LoadLibrary searches to System32 + the application directory +
// AddDllDirectory dirs. Neutralizes current-directory / PATH planting for any
// DLL loaded after this call. (Statically-imported DLLs are additionally
// constrained by the /DEPENDENTLOADFLAG link option.) Call first thing in main.
inline void HardenDllSearch() {
  ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 |
                             LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                             LOAD_LIBRARY_SEARCH_USER_DIRS);
}

// Directory of the current executable, with a trailing backslash.
inline std::wstring ExeDir() {
  wchar_t buf[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf);
  size_t s = p.find_last_of(L'\\');
  return s == std::wstring::npos ? std::wstring() : p.substr(0, s + 1);
}

// Full on-disk path of an already-loaded module (by name), or empty.
inline std::wstring LoadedModulePath(const wchar_t* name) {
  HMODULE m = ::GetModuleHandleW(name);
  if (!m)
    return std::wstring();
  wchar_t buf[MAX_PATH] = {};
  DWORD n = ::GetModuleFileNameW(m, buf, MAX_PATH);
  return (n == 0 || n >= MAX_PATH) ? std::wstring() : std::wstring(buf, n);
}

enum class Trust { kSignedValid, kUnsigned, kInvalid, kError };

// Authenticode verification via WinVerifyTrust (no network / revocation).
inline Trust VerifyTrust(const std::wstring& path) {
  if (path.empty())
    return Trust::kError;
  WINTRUST_FILE_INFO file = {};
  file.cbStruct = sizeof(file);
  file.pcwszFilePath = path.c_str();
  WINTRUST_DATA data = {};
  data.cbStruct = sizeof(data);
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &file;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  data.dwProvFlags = WTD_SAFER_FLAG;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  LONG st =
      ::WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  ::WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
  switch (st) {
    case ERROR_SUCCESS:
      return Trust::kSignedValid;
    case TRUST_E_NOSIGNATURE:
      return Trust::kUnsigned;
    default:
      return Trust::kInvalid;  // bad digest / untrusted root / etc.
  }
}

// Trust policy: valid signatures proceed; invalid signatures and unsigned files
// fail closed. Test harnesses may explicitly allow unsigned local binaries.
inline bool CheckTrust(const std::wstring& path, const char* tag,
                       bool allow_unsigned = false) {
  switch (VerifyTrust(path)) {
    case Trust::kSignedValid:
      printf("[harden] signature OK: %s\n", tag);
      return true;
    case Trust::kUnsigned:
      if (allow_unsigned) {
        printf("[harden] WARNING: %s is UNSIGNED (explicit test opt-in)\n", tag);
        return true;
      }
      printf("[harden] ABORT: %s is UNSIGNED\n", tag);
      return false;
    case Trust::kInvalid:
      printf("[harden] ABORT: %s has an INVALID signature (tampered?)\n", tag);
      return false;
    default:
      printf("[harden] ABORT: cannot verify %s\n", tag);
      return false;
  }
}

}  // namespace sbox_harden

#endif  // SANDBOX_DLL_SBOX_HARDEN_H_
