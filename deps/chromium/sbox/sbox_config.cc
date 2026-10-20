// sbox_config.cc — host side of sbox.h's sbox_config_api: the policy-builder
// setters + the fail-closed marshal/discard gate. See sbox_config.h.
#include "sbox_config.h"

// The plugin passes sbox.h's integrity constants straight into the builder; pin
// them to the core's SboxIntegrityLevel enum so the SboxPolicy values are 1:1.
static_assert(static_cast<int>(sbox_integrity_level_low) ==
                      static_cast<int>(SBOX_INTEGRITY_LOW) &&
                  static_cast<int>(sbox_integrity_level_untrusted) ==
                      static_cast<int>(SBOX_INTEGRITY_UNTRUSTED),
              "sbox.h integrity levels must match SboxIntegrityLevel");

// Pin sbox.h's public token levels to the core's SboxTokenLevel, so set_tokens'
// ints lower 1:1 to the builder (mirrors the integrity pin above).
static_assert(static_cast<int>(sbox_token_level_lockdown) ==
                      static_cast<int>(SBOX_TOKEN_LOCKDOWN) &&
                  static_cast<int>(sbox_token_level_restricted_same_access) ==
                      static_cast<int>(SBOX_TOKEN_RESTRICTED_SAME_ACCESS),
              "sbox.h token levels must match SboxTokenLevel");

namespace {

bool IsKnownIntegrity(int level) {
  return level == SBOX_INTEGRITY_LOW || level == SBOX_INTEGRITY_UNTRUSTED;
}
bool IsKnownToken(int level) {
  return level >= SBOX_TOKEN_LOCKDOWN &&
         level <= SBOX_TOKEN_RESTRICTED_SAME_ACCESS;
}

// Poison the builder and return `err`. A poisoned builder can never spawn: the
// MarshalSboxConfig gate rejects it, so no partial policy is ever applied.
sbox_status Poison(sbox_config c, sbox_status err) {
  c->invalid = true;
  return err;
}

}  // namespace

bool IsBareFilename(const std::wstring& name) {
  return !name.empty() && name != L"." &&
         name.find_first_of(L"\\/:") == std::wstring::npos &&
         name.find(L"..") == std::wstring::npos;
}

// --- sbox_config_api setters (contract in sbox.h). Each returns sbox_status and,
// on rejection, poisons the builder so a later MarshalSboxConfig refuses to
// spawn. A null builder is a caller bug (args) and does not poison anything. ---
sbox_status SBOX_CDECL CfgSetAcg(sbox_config c, bool enabled) {
  if (!c)
    return sbox_status_invalid_arg;
  c->acg = enabled;
  return sbox_status_ok;
}
sbox_status SBOX_CDECL CfgSetIntegrity(sbox_config c,
                                       sbox_integrity_level initial,
                                       sbox_integrity_level delayed) {
  if (!c)
    return sbox_status_invalid_arg;
  if (!IsKnownIntegrity(initial) || !IsKnownIntegrity(delayed))
    return Poison(c, sbox_status_invalid_arg);
  c->initial_integrity = initial;
  c->delayed_integrity = delayed;
  return sbox_status_ok;
}
sbox_status SBOX_CDECL CfgSetTokens(sbox_config c,
                                    sbox_token_level initial_token,
                                    sbox_token_level lockdown_token) {
  if (!c)
    return sbox_status_invalid_arg;
  if (!IsKnownToken(initial_token) || !IsKnownToken(lockdown_token))
    return Poison(c, sbox_status_invalid_arg);
  c->initial_token = initial_token;
  c->lockdown_token = lockdown_token;
  return sbox_status_ok;
}
sbox_status SBOX_CDECL CfgAddFileRule(sbox_config c,
                                      const sbox_char16* pattern,
                                      bool readonly) {
  if (!c)
    return sbox_status_invalid_arg;
  if (!pattern || !pattern[0])
    return Poison(c, sbox_status_invalid_arg);
  if (c->file_patterns.size() >= kSboxMaxFileRules)
    return Poison(c, sbox_status_failed);  // quota
  c->file_patterns.emplace_back(SboxToWide(pattern));
  c->file_readonly.push_back(readonly ? 1 : 0);
  return sbox_status_ok;
}
sbox_status SBOX_CDECL CfgAddCapability(sbox_config c,
                                        const sbox_char16* sid) {
  if (!c)
    return sbox_status_invalid_arg;
  if (!sid || !sid[0])
    return Poison(c, sbox_status_invalid_arg);
  if (c->capabilities.size() >= kSboxMaxCapabilities)
    return Poison(c, sbox_status_failed);  // quota
  c->capabilities.emplace_back(SboxToWide(sid));
  return sbox_status_ok;
}
sbox_status SBOX_CDECL CfgSetAppContainer(sbox_config c,
                                          bool enabled,
                                          bool lpac,
                                          const sbox_char16* profile) {
  if (!c)
    return sbox_status_invalid_arg;
  const bool have_profile = profile && profile[0];
  // An empty profile is legal only when AppContainer is off; LPAC implies
  // AppContainer. Reject (and poison) the illegal single-call combinations.
  if (enabled && !have_profile)
    return Poison(c, sbox_status_invalid_arg);
  if (lpac && !enabled)
    return Poison(c, sbox_status_invalid_arg);
  c->use_app_container = enabled;
  c->lpac = lpac;
  c->profile = have_profile ? SboxToWide(profile) : L"";
  return sbox_status_ok;
}
sbox_status SBOX_CDECL CfgAllowEngineDll(sbox_config c,
                                         const sbox_char16* filename) {
  if (!c)
    return sbox_status_invalid_arg;
  if (!filename || !IsBareFilename(SboxToWide(filename)))
    return Poison(c, sbox_status_invalid_arg);
  c->engine_dlls.emplace_back(SboxToWide(filename));
  return sbox_status_ok;
}
sbox_status SBOX_CDECL CfgSetPluginData(sbox_config c, const void* data,
                                        size_t size) {
  if (!c)
    return sbox_status_invalid_arg;
  if (size > kSboxMaxPluginData)
    return Poison(c, sbox_status_failed);  // quota
  if (data && size)
    c->plugin_data.assign(static_cast<const char*>(data), size);
  else
    c->plugin_data.clear();
  return sbox_status_ok;
}

sbox_config_api MakeConfigApi() {
  sbox_config_api api = SBOX_CONFIG_API_INIT;
  api.set_acg = &CfgSetAcg;
  api.set_integrity = &CfgSetIntegrity;
  api.add_file_rule = &CfgAddFileRule;
  api.add_capability = &CfgAddCapability;
  api.set_app_container = &CfgSetAppContainer;
  api.allow_engine_dll = &CfgAllowEngineDll;
  api.set_plugin_data = &CfgSetPluginData;
  api.set_tokens = &CfgSetTokens;
  return api;
}

bool MarshalSboxConfig(const sbox_config_s& cfg,
                       const wchar_t* worker_plugin_name,
                       SboxConfigMarshal& out) {
  // The discard gate: a builder poisoned by any failed setter never spawns.
  if (cfg.invalid)
    return false;
  // Cross-setter rules the single-call setters cannot see (belt-and-suspenders
  // with the setters that can). Capabilities are meaningful only under
  // AppContainer; LPAC implies AppContainer.
  if (!cfg.use_app_container && !cfg.capabilities.empty())
    return false;
  if (cfg.lpac && !cfg.use_app_container)
    return false;
  // Never trust the collected counts, even though the setters cap them.
  if (cfg.file_patterns.size() > kSboxMaxFileRules ||
      cfg.file_patterns.size() != cfg.file_readonly.size() ||
      cfg.capabilities.size() > kSboxMaxCapabilities ||
      cfg.plugin_data.size() > kSboxMaxPluginData)
    return false;

  out.file_rules.clear();
  out.cap_ptrs.clear();
  SboxPolicy& p = out.policy;
  p = {};
  p.struct_size = sizeof(p);
  p.initial_token = cfg.initial_token;
  p.lockdown_token = cfg.lockdown_token;
  p.integrity = cfg.initial_integrity;
  p.delayed_integrity = cfg.delayed_integrity;
  p.prohibit_dynamic_code = cfg.acg ? 1 : 0;
  for (size_t i = 0; i < cfg.file_patterns.size(); ++i) {
    SboxFileRule r = {};
    r.pattern = cfg.file_patterns[i].c_str();
    r.readonly = cfg.file_readonly[i];
    out.file_rules.push_back(r);
  }
  p.file_rules = out.file_rules.empty() ? nullptr : out.file_rules.data();
  p.file_rule_count = out.file_rules.size();
  p.use_app_container = cfg.use_app_container ? 1 : 0;
  p.low_privilege_app_container = cfg.lpac ? 1 : 0;
  p.app_container_profile_name =
      cfg.profile.empty() ? nullptr : cfg.profile.c_str();
  for (const std::wstring& cap : cfg.capabilities)
    out.cap_ptrs.push_back(cap.c_str());
  p.capabilities = out.cap_ptrs.empty() ? nullptr : out.cap_ptrs.data();
  p.capability_count = out.cap_ptrs.size();
  p.worker_plugin_name = worker_plugin_name;
  p.plugin_data = cfg.plugin_data.empty() ? nullptr : cfg.plugin_data.data();
  p.plugin_data_len = cfg.plugin_data.size();
  return true;
}
