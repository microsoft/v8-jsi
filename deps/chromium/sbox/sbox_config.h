// sbox_config.h — the broker's sandbox-policy BUILDER (the host side of sbox.h's
// sbox_config_api). The plugin's configure() mutates an sbox_config_s through the
// setters this exposes; MarshalSboxConfig then validates the collected builder
// and lowers it to an SboxPolicy. Factored out of sbox_main.cc so the setters +
// the fail-closed discard gate are unit-testable without the sbox.exe role
// dispatch or a spawned worker.
#ifndef SANDBOX_DLL_SBOX_CONFIG_H_
#define SANDBOX_DLL_SBOX_CONFIG_H_

#include <string>
#include <vector>

#include "sbox.h"
#include "sbox_core_internal.h"

// Count/size caps the builder enforces (mirrored by the coordinator codec).
constexpr size_t kSboxMaxFileRules = 64;
constexpr size_t kSboxMaxCapabilities = 64;
constexpr size_t kSboxMaxPluginData = 4096;

// sbox.h strings are sbox_char16; the core and Win32 use wchar_t, which is the
// same UTF-16 code unit on Windows.
static_assert(sizeof(sbox_char16) == sizeof(wchar_t),
              "sbox_char16 must match wchar_t");
inline const wchar_t* SboxToWide(const sbox_char16* text) {
  return reinterpret_cast<const wchar_t*>(text);
}
inline const sbox_char16* WideToSbox(const wchar_t* text) {
  return reinterpret_cast<const sbox_char16*>(text);
}

// sbox_config: the broker's policy builder. plugin.configure() writes into it via
// the sbox_config_api setters; MarshalSboxConfig then marshals it to an
// SboxPolicy. Any setter that rejects its input also sets `invalid` (poison), so
// a partially-built config can never spawn (fail closed). Defined at GLOBAL scope
// to match sbox.h's `struct sbox_config_s` forward declaration.
struct sbox_config_s {
  bool acg = false;
  // Token/integrity defaults mirror the ordinary restricted-token mode the
  // container applied before set_tokens existed (see MarshalSboxConfig).
  int initial_integrity = SBOX_INTEGRITY_LOW;
  int delayed_integrity = SBOX_INTEGRITY_UNTRUSTED;
  int initial_token = SBOX_TOKEN_RESTRICTED_SAME_ACCESS;
  int lockdown_token = SBOX_TOKEN_LOCKDOWN;
  std::vector<std::wstring> file_patterns;
  std::vector<int> file_readonly;
  std::vector<std::wstring> capabilities;
  bool use_app_container = false;
  bool lpac = false;
  std::wstring profile;
  std::vector<std::wstring> engine_dlls;  // verified by the broker before spawn
  std::string plugin_data;                // opaque app blob, carried to the worker
  bool invalid = false;                   // poison: a failed setter forbids spawn
};

// A plugin name / engine DLL must be a BARE app-dir filename (e.g. "v8host.dll")
// — never a path. The broker Authenticode-verifies it inside the app dir, so
// rejecting separators / ".." closes the only planting vector the name opens.
bool IsBareFilename(const std::wstring& name);

// Build the host's sbox_config_api vtable (the setters the plugin calls). Every
// setter returns sbox_status and poisons the builder on rejection.
sbox_config_api MakeConfigApi();

// The marshaled policy plus the owned storage its pointers alias. `policy`
// aliases cfg + this struct, so keep both alive and never copy after marshal.
struct SboxConfigMarshal {
  std::vector<SboxFileRule> file_rules;  // pattern ptrs into cfg.file_patterns
  std::vector<const wchar_t*> cap_ptrs;  // into cfg.capabilities
  SboxPolicy policy = {};
};

// Validate the collected builder and lower it to an SboxPolicy. THE DISCARD GATE:
// returns false (fail closed, no policy, so the caller must not spawn) if the
// builder was poisoned by a failed setter OR a cross-setter rule fails
// (capabilities present without AppContainer; LPAC without AppContainer; a count
// over quota). On success `out.policy` is ready to spawn and aliases `cfg` +
// `worker_plugin_name`, both of which must outlive it.
bool MarshalSboxConfig(const sbox_config_s& cfg,
                       const wchar_t* worker_plugin_name,
                       SboxConfigMarshal& out);

#endif  // SANDBOX_DLL_SBOX_CONFIG_H_
