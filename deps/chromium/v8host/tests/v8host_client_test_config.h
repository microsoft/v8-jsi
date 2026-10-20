// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Test-side mirrors of the session and run inputs, applied through the public
// v8host_config / v8host_run_config builder setters. The fields keep the shape
// the tests already reason about; ApplySessionConfig and ApplyRunInputs
// translate them into setter calls and return the first failing setter's
// status. The setters come from a ConfigApi table so the same translation
// serves tests that link the client statically (LinkedConfigApi, OpenSession,
// StartRun) and tests that load v8host.dll with GetProcAddress.
#ifndef V8HOST_TESTS_V8HOST_CLIENT_TEST_CONFIG_H_
#define V8HOST_TESTS_V8HOST_CLIENT_TEST_CONFIG_H_

#include "v8host_client.h"

#include <cstddef>
#include <cstdint>

namespace v8host::test {

struct TestFileRule {
  const wchar_t* pattern = nullptr;
  bool readonly = false;
};

struct TestSessionConfig {
  v8host_broker_mode broker_mode = v8host_broker_mode_dedicated;
  v8host_tier tier = v8host_tier_untrusted;
  // Raw values so tests can drive out-of-range levels into the setters.
  int32_t integrity = 0;
  int32_t delayed_integrity = 0;
  int32_t initial_token = v8host_token_level_restricted_same_access;
  int32_t lockdown_token = 0;
  bool prohibit_dynamic_code = false;
  const TestFileRule* file_rules = nullptr;
  size_t file_rule_count = 0;
  bool use_app_container = false;
  bool low_privilege_app_container = false;
  // NULL leaves the profile name unset.
  const wchar_t* app_container_profile_name = nullptr;
  const wchar_t* const* capabilities = nullptr;
  size_t capability_count = 0;
};

struct TestRunInputs {
  // -1 leaves the run tier unset (the session tier applies).
  int32_t tier_override = -1;
  // NULL leaves the engine / snapshot unset.
  const wchar_t* engine_dll_override = nullptr;
  const wchar_t* startup_snapshot_path = nullptr;
  // Both NULL / 0 leaves the payload unset; anything else calls set_payload.
  const void* payload = nullptr;
  size_t payload_len = 0;
};

inline const v8host_char16* AsChar16(const wchar_t* text) {
  static_assert(sizeof(wchar_t) == sizeof(v8host_char16),
                "wchar_t must be UTF-16");
  return reinterpret_cast<const v8host_char16*>(text);
}

// The builder setters ApplySessionConfig / ApplyRunInputs call.
#define V8HOST_TEST_CONFIG_SETTERS(X)          \
  X(config_set_broker_mode)                    \
  X(config_set_tier)                           \
  X(config_set_integrity_level)                \
  X(config_set_delayed_integrity_level)        \
  X(config_set_initial_token_level)            \
  X(config_set_lockdown_token_level)           \
  X(config_set_prohibit_dynamic_code)          \
  X(config_add_file_rule_utf16)                \
  X(config_set_use_app_container)              \
  X(config_set_low_privilege_app_container)    \
  X(config_set_profile_name_utf16)             \
  X(config_add_capability_utf16)               \
  X(run_config_set_tier)                       \
  X(run_config_set_engine_dll_utf16)           \
  X(run_config_set_startup_snapshot_path_utf16) \
  X(run_config_set_payload)

struct ConfigApi {
#define V8HOST_TEST_CONFIG_MEMBER(name) v8host_##name##_fn name = nullptr;
  V8HOST_TEST_CONFIG_SETTERS(V8HOST_TEST_CONFIG_MEMBER)
#undef V8HOST_TEST_CONFIG_MEMBER
};

inline v8host_status ApplySessionConfig(const ConfigApi& api,
                                        v8host_config config,
                                        const TestSessionConfig& c) {
  v8host_status s = api.config_set_broker_mode(config, c.broker_mode);
  if (s == v8host_status_ok)
    s = api.config_set_tier(config, c.tier);
  if (s == v8host_status_ok) {
    s = api.config_set_integrity_level(
        config, static_cast<v8host_integrity_level>(c.integrity));
  }
  if (s == v8host_status_ok) {
    s = api.config_set_delayed_integrity_level(
        config, static_cast<v8host_integrity_level>(c.delayed_integrity));
  }
  if (s == v8host_status_ok) {
    s = api.config_set_initial_token_level(
        config, static_cast<v8host_token_level>(c.initial_token));
  }
  if (s == v8host_status_ok) {
    s = api.config_set_lockdown_token_level(
        config, static_cast<v8host_token_level>(c.lockdown_token));
  }
  if (s == v8host_status_ok)
    s = api.config_set_prohibit_dynamic_code(config, c.prohibit_dynamic_code);
  for (size_t i = 0; s == v8host_status_ok && i < c.file_rule_count; ++i) {
    s = api.config_add_file_rule_utf16(
        config, AsChar16(c.file_rules[i].pattern), V8HOST_AUTO_LENGTH,
        c.file_rules[i].readonly);
  }
  if (s == v8host_status_ok)
    s = api.config_set_use_app_container(config, c.use_app_container);
  if (s == v8host_status_ok) {
    s = api.config_set_low_privilege_app_container(
        config, c.low_privilege_app_container);
  }
  if (s == v8host_status_ok && c.app_container_profile_name != nullptr) {
    s = api.config_set_profile_name_utf16(
        config, AsChar16(c.app_container_profile_name), V8HOST_AUTO_LENGTH);
  }
  for (size_t i = 0; s == v8host_status_ok && i < c.capability_count; ++i) {
    s = api.config_add_capability_utf16(config, AsChar16(c.capabilities[i]),
                                        V8HOST_AUTO_LENGTH);
  }
  return s;
}

inline v8host_status ApplyRunInputs(const ConfigApi& api,
                                    v8host_run_config config,
                                    const TestRunInputs& r) {
  v8host_status s = v8host_status_ok;
  if (r.tier_override != -1) {
    s = api.run_config_set_tier(config,
                                static_cast<v8host_tier>(r.tier_override));
  }
  if (s == v8host_status_ok && r.engine_dll_override != nullptr) {
    s = api.run_config_set_engine_dll_utf16(
        config, AsChar16(r.engine_dll_override), V8HOST_AUTO_LENGTH);
  }
  if (s == v8host_status_ok && r.startup_snapshot_path != nullptr) {
    s = api.run_config_set_startup_snapshot_path_utf16(
        config, AsChar16(r.startup_snapshot_path), V8HOST_AUTO_LENGTH);
  }
  if (s == v8host_status_ok && (r.payload != nullptr || r.payload_len != 0))
    s = api.run_config_set_payload(config, r.payload, r.payload_len);
  return s;
}

// user_data for ConfigureSession / ConfigureRun.
struct SessionConfigure {
  const ConfigApi* api = nullptr;
  const TestSessionConfig* config = nullptr;
};
struct RunConfigure {
  const ConfigApi* api = nullptr;
  const TestRunInputs* inputs = nullptr;
};

inline v8host_status V8HOST_CDECL ConfigureSession(void* user_data,
                                                   v8host_config config) {
  const auto& a = *static_cast<const SessionConfigure*>(user_data);
  return ApplySessionConfig(*a.api, config, *a.config);
}

inline v8host_status V8HOST_CDECL ConfigureRun(void* user_data,
                                               v8host_run_config config) {
  const auto& a = *static_cast<const RunConfigure*>(user_data);
  return ApplyRunInputs(*a.api, config, *a.inputs);
}

// The setters linked into the test binary. Only tests that link the client
// implementation (not the DLL loaders) may call this or the helpers below.
inline const ConfigApi& LinkedConfigApi() {
  static const ConfigApi api = [] {
    ConfigApi a;
#define V8HOST_TEST_CONFIG_LINK(name) a.name = &v8host_##name;
    V8HOST_TEST_CONFIG_SETTERS(V8HOST_TEST_CONFIG_LINK)
#undef V8HOST_TEST_CONFIG_LINK
    return a;
  }();
  return api;
}

// v8host_session_open through ConfigureSession; a NULL `config` passes a NULL
// configure callback (all defaults).
inline v8host_status OpenSession(const TestSessionConfig* config,
                                 v8host_session* out_session) {
  SessionConfigure args{&LinkedConfigApi(), config};
  return v8host_session_open(config != nullptr ? &ConfigureSession : nullptr,
                             config != nullptr ? &args : nullptr,
                             out_session);
}

// v8host_session_start_run through ConfigureRun; a NULL `inputs` passes a NULL
// configure callback (all defaults).
inline v8host_status StartRun(v8host_session session,
                              const TestRunInputs* inputs,
                              v8host_run* out_run) {
  RunConfigure args{&LinkedConfigApi(), inputs};
  return v8host_session_start_run(
      session, inputs != nullptr ? &ConfigureRun : nullptr,
      inputs != nullptr ? &args : nullptr, out_run);
}

}  // namespace v8host::test

#endif  // V8HOST_TESTS_V8HOST_CLIENT_TEST_CONFIG_H_
