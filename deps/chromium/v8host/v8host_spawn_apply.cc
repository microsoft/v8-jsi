// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "v8host_spawn_apply.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace v8host {

namespace {

// UTF-8 -> UTF-16 for the wide config setters. The codec guarantees strict
// UTF-8 / no NUL, so this is lossless; an empty input yields an empty string and
// a conversion failure yields an empty string (which the setters reject -> fail
// closed for the path/SID/engine fields).
std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty())
    return std::wstring();
  const int needed =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0)
    return std::wstring();
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                        static_cast<int>(utf8.size()), wide.data(), needed);
  return wide;
}

// sbox.h strings are sbox_char16, the same UTF-16 code unit as wchar_t here.
static_assert(sizeof(sbox_char16) == sizeof(wchar_t),
              "sbox_char16 must match wchar_t");
const sbox_char16* AsSboxChar16(const std::wstring& text) {
  return reinterpret_cast<const sbox_char16*>(text.c_str());
}

}  // namespace

bool ApplySpawnConfig(const V8HostSpawnConfigV1& spawn,
                      const sbox_config_api* api,
                      sbox_config cfg) {
  if (!api || !cfg)
    return false;

  // Derive the jitless posture from the effective tier (only an explicit Trusted
  // tier gets JIT; every other value stays jitless, the most restrictive
  // default), then require the armed ACG to match it. This rejects a trusted/JIT
  // tier under ACG and a jitless tier without ACG, so the worker's post-lockdown
  // ACG proof always agrees with the encoded jitless flag. Fail closed.
  const bool jitless = spawn.effective_tier != kTierTrusted;
  if (jitless != spawn.prohibit_dynamic_code)
    return false;

  if (api->set_acg(cfg, spawn.prohibit_dynamic_code) != sbox_status_ok)
    return false;
  if (api->set_integrity(
          cfg, static_cast<sbox_integrity_level>(spawn.integrity),
          static_cast<sbox_integrity_level>(spawn.delayed_integrity)) !=
      sbox_status_ok)
    return false;
  if (api->set_tokens(cfg,
                      static_cast<sbox_token_level>(spawn.initial_token),
                      static_cast<sbox_token_level>(spawn.lockdown_token)) !=
      sbox_status_ok)
    return false;
  for (const V8HostSpawnFileRule& rule : spawn.file_rules) {
    const std::wstring pattern = Utf8ToWide(rule.pattern);
    if (api->add_file_rule(cfg, AsSboxChar16(pattern), rule.readonly) !=
        sbox_status_ok)
      return false;
  }
  {
    const std::wstring profile = Utf8ToWide(spawn.app_container_profile);
    if (api->set_app_container(cfg, spawn.use_app_container,
                               spawn.low_privilege_app_container,
                               profile.empty() ? nullptr
                                               : AsSboxChar16(profile)) !=
        sbox_status_ok)
      return false;
  }
  for (const std::string& capability : spawn.capabilities) {
    const std::wstring sid = Utf8ToWide(capability);
    if (api->add_capability(cfg, AsSboxChar16(sid)) != sbox_status_ok)
      return false;
  }
  const std::wstring engine = Utf8ToWide(spawn.engine_dll);
  if (api->allow_engine_dll(cfg, AsSboxChar16(engine)) != sbox_status_ok)
    return false;

  // Encode the bound worker profile into the opaque plugin_data the worker
  // decodes in warmup: V1 (jitless + engine + one snapshot) for a session, V2
  // (plus the runtime cap and the snapshot list) for an explicit worker.
  // set_plugin_data enforces the 4 KiB ceiling and poisons the builder if
  // exceeded.
  std::vector<uint8_t> blob;
  if (spawn.max_runtimes == 0) {
    V8HostWorkerProfileV1 profile;
    profile.jitless = jitless ? 1u : 0u;
    profile.engine_dll = spawn.engine_dll;
    profile.snapshot_path = spawn.snapshot_path;
    blob = profile.Encode();
  } else {
    V8HostWorkerProfileV2 profile;
    profile.jitless = jitless ? 1u : 0u;
    profile.max_runtimes = spawn.max_runtimes;
    profile.engine_dll = spawn.engine_dll;
    profile.snapshot_paths = spawn.snapshot_paths;
    blob = profile.Encode();
  }
  if (api->set_plugin_data(cfg, blob.data(), blob.size()) != sbox_status_ok)
    return false;
  return true;
}

}  // namespace v8host
