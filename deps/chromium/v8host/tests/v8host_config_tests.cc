// Deterministic, in-process unit tests for the sbox_config builder: the
// status-returning sbox_config_api setters (MakeConfigApi) and the
// MarshalSboxConfig discard gate (//sbox:sbox_config). No pipes, no
// spawned worker — the gate these exercise is EXACTLY the one PrepareSandbox
// calls before sbox_broker_spawn, so "MarshalSboxConfig returns false" is the
// decision that resumes NO worker. Covers plan §5.2's config cases: every setter
// success/failure, invalid integrity/token levels, AppContainer cross-field
// combinations, the 64/64 file-rule/capability + 4096-byte plugin-data limits,
// field round-trip fidelity, and — the key safety property — a poisoned builder
// never produces a spawnable policy.
#include "v8host_test_support.h"

#include "sbox_config.h"
#include "msg_channel.h"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

using v8host::test::TestCase;

#define CHECK(cond, msg)   \
  do {                     \
    if (!(cond)) {         \
      *detail = (msg);     \
      return false;        \
    }                      \
  } while (0)

// A minimal valid config built through the real vtable: no AppContainer and no
// capabilities, so MarshalSboxConfig accepts it. Used as the spawnable baseline
// the poison/cross-field cases perturb.
bool BuildValid(const sbox_config_api& api, sbox_config c) {
  return api.set_acg(c, 1) == sbox_status_ok &&
         api.set_integrity(c, sbox_integrity_level_low, sbox_integrity_level_untrusted) ==
             sbox_status_ok &&
         api.set_tokens(c, sbox_token_level_restricted_same_access,
                        sbox_token_level_lockdown) == sbox_status_ok &&
         api.add_file_rule(c, u"C:\\app\\data\\*", 1) == sbox_status_ok &&
         api.allow_engine_dll(c, u"v8jsi.dll") == sbox_status_ok &&
         api.set_plugin_data(c, "blob", 4) == sbox_status_ok && !c->invalid;
}

// ---- setters: success + null-arg + invalid-value ----

bool SettersSuccess(std::string* detail) {
  sbox_config_s cfg;
  const sbox_config_api api = MakeConfigApi();
  CHECK(api.set_tokens != nullptr, "set_tokens must be wired into the vtable");
  CHECK(api.set_acg(&cfg, 1) == sbox_status_ok && cfg.acg, "set_acg(1)");
  CHECK(api.set_acg(&cfg, 0) == sbox_status_ok && !cfg.acg, "set_acg(0)");
  CHECK(api.set_integrity(&cfg, sbox_integrity_level_low, sbox_integrity_level_untrusted) ==
            sbox_status_ok,
        "set_integrity valid");
  CHECK(api.set_tokens(&cfg, sbox_token_level_lockdown,
                       sbox_token_level_restricted_same_access) == sbox_status_ok,
        "set_tokens valid");
  CHECK(api.add_file_rule(&cfg, u"C:\\x\\*", 0) == sbox_status_ok, "add_file_rule");
  CHECK(api.allow_engine_dll(&cfg, u"v8jsi.dll") == sbox_status_ok, "allow_engine_dll");
  CHECK(api.set_plugin_data(&cfg, "data", 4) == sbox_status_ok, "set_plugin_data");
  CHECK(api.set_plugin_data(&cfg, nullptr, 0) == sbox_status_ok, "set_plugin_data clear");
  CHECK(!cfg.invalid, "valid setters must not poison the builder");
  return true;
}

bool SettersNullArg(std::string* detail) {
  const sbox_config_api api = MakeConfigApi();
  // A null builder is a caller bug (args), reported but un-poisonable.
  CHECK(api.set_acg(nullptr, 1) == sbox_status_invalid_arg, "set_acg(null)");
  CHECK(api.set_integrity(nullptr, sbox_integrity_level_low, sbox_integrity_level_low) == sbox_status_invalid_arg, "set_integrity(null)");
  CHECK(api.set_tokens(nullptr, sbox_token_level_lockdown, sbox_token_level_lockdown) == sbox_status_invalid_arg, "set_tokens(null)");
  CHECK(api.add_file_rule(nullptr, u"x", 0) == sbox_status_invalid_arg, "add_file_rule(null)");
  CHECK(api.add_capability(nullptr, u"S-1-1") == sbox_status_invalid_arg, "add_capability(null)");
  CHECK(api.set_app_container(nullptr, 0, 0, nullptr) == sbox_status_invalid_arg,
        "set_app_container(null)");
  CHECK(api.allow_engine_dll(nullptr, u"v8jsi.dll") == sbox_status_invalid_arg,
        "allow_engine_dll(null)");
  CHECK(api.set_plugin_data(nullptr, "x", 1) == sbox_status_invalid_arg, "set_plugin_data(null)");
  return true;
}

bool IntegrityInvalid(std::string* detail) {
  const sbox_config_api api = MakeConfigApi();
  const int bad[] = {-1, 2, 5, 100};
  for (int level : bad) {
    sbox_config_s cfg;
    CHECK(api.set_integrity(&cfg, static_cast<sbox_integrity_level>(level), sbox_integrity_level_low) == sbox_status_invalid_arg,
          "bad initial integrity must be rejected");
    CHECK(cfg.invalid, "bad integrity must poison the builder");
    sbox_config_s cfg2;
    CHECK(api.set_integrity(&cfg2, sbox_integrity_level_low, static_cast<sbox_integrity_level>(level)) == sbox_status_invalid_arg,
          "bad delayed integrity must be rejected");
    CHECK(cfg2.invalid, "bad delayed integrity must poison the builder");
  }
  return true;
}

bool TokensInvalid(std::string* detail) {
  const sbox_config_api api = MakeConfigApi();
  const int bad[] = {-1, 5, 100};
  for (int level : bad) {
    sbox_config_s cfg;
    CHECK(api.set_tokens(&cfg, static_cast<sbox_token_level>(level), sbox_token_level_lockdown) == sbox_status_invalid_arg,
          "bad initial token must be rejected");
    CHECK(cfg.invalid, "bad token must poison the builder");
    sbox_config_s cfg2;
    CHECK(api.set_tokens(&cfg2, sbox_token_level_lockdown, static_cast<sbox_token_level>(level)) == sbox_status_invalid_arg,
          "bad lockdown token must be rejected");
    CHECK(cfg2.invalid, "bad lockdown token must poison the builder");
  }
  return true;
}

bool FileRuleBadPattern(std::string* detail) {
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  CHECK(api.add_file_rule(&cfg, nullptr, 0) == sbox_status_invalid_arg, "null pattern");
  CHECK(cfg.invalid, "null pattern must poison");
  sbox_config_s cfg2;
  CHECK(api.add_file_rule(&cfg2, u"", 0) == sbox_status_invalid_arg, "empty pattern");
  CHECK(cfg2.invalid, "empty pattern must poison");
  return true;
}

bool CapabilityBadSid(std::string* detail) {
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  CHECK(api.add_capability(&cfg, nullptr) == sbox_status_invalid_arg, "null sid");
  CHECK(cfg.invalid, "null sid must poison");
  sbox_config_s cfg2;
  CHECK(api.add_capability(&cfg2, u"") == sbox_status_invalid_arg, "empty sid");
  CHECK(cfg2.invalid, "empty sid must poison");
  return true;
}

bool EngineDllBadName(std::string* detail) {
  const sbox_config_api api = MakeConfigApi();
  const sbox_char16* bad[] = {u"", u"C:\\evil.dll", u"sub\\x.dll", u"..\\x.dll",
                              u"."};
  for (const sbox_char16* name : bad) {
    sbox_config_s cfg;
    CHECK(api.allow_engine_dll(&cfg, name) == sbox_status_invalid_arg,
          "non-bare engine name must be rejected");
    CHECK(cfg.invalid, "non-bare engine name must poison");
  }
  sbox_config_s cfg;
  CHECK(api.allow_engine_dll(&cfg, nullptr) == sbox_status_invalid_arg, "null engine name");
  CHECK(cfg.invalid, "null engine name must poison");
  return true;
}

// ---- limits: 64/65 boundaries + 4096/4097 ----

bool FileRuleQuota(std::string* detail) {
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  for (size_t i = 0; i < kSboxMaxFileRules; ++i)
    CHECK(api.add_file_rule(&cfg, u"C:\\x\\*", 0) == sbox_status_ok, "64 rules must fit");
  CHECK(!cfg.invalid, "exactly 64 file rules must not poison");
  SboxConfigMarshal marshal;
  CHECK(MarshalSboxConfig(cfg, L"v8host.dll", marshal), "64 file rules marshal");
  CHECK(marshal.policy.file_rule_count == kSboxMaxFileRules, "64 rules in policy");
  // The 65th exceeds the quota: rejected, poisoned, and never spawnable.
  CHECK(api.add_file_rule(&cfg, u"C:\\x\\*", 0) == sbox_status_failed,
        "65th file rule must be rejected");
  CHECK(cfg.invalid, "65th file rule must poison");
  SboxConfigMarshal after;
  CHECK(!MarshalSboxConfig(cfg, L"v8host.dll", after), "65 file rules: no spawn");
  return true;
}

bool CapabilityQuota(std::string* detail) {
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  // Capabilities are only spawnable under AppContainer.
  CHECK(api.set_app_container(&cfg, 1, 0, u"profile") == sbox_status_ok, "enable AC");
  for (size_t i = 0; i < kSboxMaxCapabilities; ++i)
    CHECK(api.add_capability(&cfg, u"S-1-15-3-1") == sbox_status_ok, "64 caps must fit");
  CHECK(!cfg.invalid, "exactly 64 capabilities must not poison");
  SboxConfigMarshal marshal;
  CHECK(MarshalSboxConfig(cfg, L"v8host.dll", marshal), "64 caps marshal");
  CHECK(marshal.policy.capability_count == kSboxMaxCapabilities, "64 caps in policy");
  CHECK(api.add_capability(&cfg, u"S-1-15-3-1") == sbox_status_failed,
        "65th capability must be rejected");
  CHECK(cfg.invalid, "65th capability must poison");
  SboxConfigMarshal after;
  CHECK(!MarshalSboxConfig(cfg, L"v8host.dll", after), "65 caps: no spawn");
  return true;
}

bool PluginDataQuota(std::string* detail) {
  const sbox_config_api api = MakeConfigApi();
  const std::string ok_blob(kSboxMaxPluginData, 'x');
  const std::string big_blob(kSboxMaxPluginData + 1, 'x');
  sbox_config_s cfg;
  CHECK(api.set_plugin_data(&cfg, ok_blob.data(), ok_blob.size()) == sbox_status_ok,
        "4096 bytes must fit");
  CHECK(!cfg.invalid, "4096-byte plugin data must not poison");
  SboxConfigMarshal marshal;
  CHECK(MarshalSboxConfig(cfg, L"v8host.dll", marshal), "4096 bytes marshal");
  CHECK(marshal.policy.plugin_data_len == kSboxMaxPluginData,
        "4096-byte blob preserved in policy");
  CHECK(api.set_plugin_data(&cfg, big_blob.data(), big_blob.size()) == sbox_status_failed,
        "4097 bytes must be rejected");
  CHECK(cfg.invalid, "4097-byte plugin data must poison");
  SboxConfigMarshal after;
  CHECK(!MarshalSboxConfig(cfg, L"v8host.dll", after), "4097 bytes: no spawn");
  return true;
}

// ---- AppContainer cross-field combinations ----

bool AppContainerCombos(std::string* detail) {
  const sbox_config_api api = MakeConfigApi();
  {  // AppContainer on with a profile: ok.
    sbox_config_s cfg;
    CHECK(api.set_app_container(&cfg, 1, 0, u"profile") == sbox_status_ok, "AC+profile");
    CHECK(!cfg.invalid, "AC+profile must not poison");
  }
  {  // AppContainer off with an empty profile: ok (empty profile legal iff off).
    sbox_config_s cfg;
    CHECK(api.set_app_container(&cfg, 0, 0, nullptr) == sbox_status_ok, "no-AC+empty");
    CHECK(!cfg.invalid, "no-AC+empty must not poison");
  }
  {  // LPAC implies AppContainer, so LPAC+AC+profile: ok.
    sbox_config_s cfg;
    CHECK(api.set_app_container(&cfg, 1, 1, u"profile") == sbox_status_ok, "LPAC+AC");
    CHECK(cfg.lpac && cfg.use_app_container, "LPAC+AC stored");
  }
  {  // AppContainer on but empty profile: rejected + poisoned.
    sbox_config_s cfg;
    CHECK(api.set_app_container(&cfg, 1, 0, nullptr) == sbox_status_invalid_arg,
          "AC+empty-profile must be rejected");
    CHECK(cfg.invalid, "AC+empty-profile must poison");
    SboxConfigMarshal m;
    CHECK(!MarshalSboxConfig(cfg, L"v8host.dll", m), "AC+empty-profile: no spawn");
  }
  {  // LPAC without AppContainer: rejected + poisoned.
    sbox_config_s cfg;
    CHECK(api.set_app_container(&cfg, 0, 1, u"profile") == sbox_status_invalid_arg,
          "LPAC-without-AC must be rejected");
    CHECK(cfg.invalid, "LPAC-without-AC must poison");
  }
  return true;
}

// ---- marshal: valid round-trip + field fidelity + default tokens ----

bool MarshalFieldFidelity(std::string* detail) {
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  CHECK(api.set_acg(&cfg, 1) == sbox_status_ok, "acg");
  CHECK(api.set_integrity(&cfg, sbox_integrity_level_low, sbox_integrity_level_untrusted) ==
            sbox_status_ok,
        "integrity");
  CHECK(api.set_tokens(&cfg, sbox_token_level_interactive, sbox_token_level_limited) == sbox_status_ok,
        "tokens");
  CHECK(api.add_file_rule(&cfg, u"C:\\a\\*", 1) == sbox_status_ok, "rule0");
  CHECK(api.add_file_rule(&cfg, u"C:\\b\\*", 0) == sbox_status_ok, "rule1");
  CHECK(api.set_app_container(&cfg, 1, 1, u"com.example.profile") == sbox_status_ok, "ac");
  CHECK(api.add_capability(&cfg, u"S-1-15-3-1") == sbox_status_ok, "cap");
  CHECK(api.set_plugin_data(&cfg, "payload", 7) == sbox_status_ok, "plugin_data");

  SboxConfigMarshal m;
  CHECK(MarshalSboxConfig(cfg, L"v8host.dll", m), "valid config must marshal");
  const SboxPolicy& p = m.policy;
  CHECK(p.struct_size == sizeof(SboxPolicy), "struct_size");
  CHECK(p.prohibit_dynamic_code == 1, "acg -> prohibit_dynamic_code");
  CHECK(p.integrity == sbox_integrity_level_low, "integrity field");
  CHECK(p.delayed_integrity == sbox_integrity_level_untrusted, "delayed integrity field");
  CHECK(p.initial_token == SBOX_TOKEN_INTERACTIVE, "initial token field");
  CHECK(p.lockdown_token == SBOX_TOKEN_LIMITED, "lockdown token field");
  CHECK(p.file_rule_count == 2, "file rule count");
  CHECK(std::wstring(p.file_rules[0].pattern) == L"C:\\a\\*" &&
            p.file_rules[0].readonly == 1,
        "file rule 0");
  CHECK(std::wstring(p.file_rules[1].pattern) == L"C:\\b\\*" &&
            p.file_rules[1].readonly == 0,
        "file rule 1");
  CHECK(p.use_app_container == 1 && p.low_privilege_app_container == 1, "AC flags");
  CHECK(std::wstring(p.app_container_profile_name) == L"com.example.profile",
        "profile name");
  CHECK(p.capability_count == 1 &&
            std::wstring(p.capabilities[0]) == L"S-1-15-3-1",
        "capability");
  CHECK(p.plugin_data_len == 7 &&
            std::string(static_cast<const char*>(p.plugin_data), 7) == "payload",
        "plugin data");
  CHECK(std::wstring(p.worker_plugin_name) == L"v8host.dll", "worker plugin name");
  return true;
}

bool MarshalDefaultTokens(std::string* detail) {
  // A config that never calls set_tokens keeps the container's historical default
  // (restricted-same-access initial, lockdown token) — the pre-ABI behavior.
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  CHECK(BuildValid(api, &cfg), "baseline build");
  SboxConfigMarshal m;
  CHECK(MarshalSboxConfig(cfg, L"v8host.dll", m), "baseline marshal");
  CHECK(m.policy.initial_token == SBOX_TOKEN_RESTRICTED_SAME_ACCESS,
        "default initial token");
  CHECK(m.policy.lockdown_token == SBOX_TOKEN_LOCKDOWN, "default lockdown token");
  return true;
}

// ---- discard gate: a poisoned builder resumes NO worker ----

bool PoisonedBuilderNeverMarshals(std::string* detail) {
  const sbox_config_api api = MakeConfigApi();
  // Each injection drives ONE setter failure into an otherwise-valid builder;
  // MarshalSboxConfig (the pre-spawn gate) must then refuse every one of them.
  const std::vector<std::pair<const char*,
                              std::function<void(const sbox_config_api&, sbox_config)>>>
      injections = {
          {"integrity", [](const sbox_config_api& a, sbox_config c) {
             a.set_integrity(c, static_cast<sbox_integrity_level>(99), sbox_integrity_level_low);
           }},
          {"tokens", [](const sbox_config_api& a, sbox_config c) {
             a.set_tokens(c, static_cast<sbox_token_level>(99), sbox_token_level_lockdown);
           }},
          {"file_rule_empty", [](const sbox_config_api& a, sbox_config c) {
             a.add_file_rule(c, u"", 0);
           }},
          {"engine_dll_path", [](const sbox_config_api& a, sbox_config c) {
             a.allow_engine_dll(c, u"C:\\evil.dll");
           }},
          {"app_container_empty_profile",
           [](const sbox_config_api& a, sbox_config c) {
             a.set_app_container(c, 1, 0, nullptr);
           }},
          {"lpac_without_ac", [](const sbox_config_api& a, sbox_config c) {
             a.set_app_container(c, 0, 1, u"profile");
           }},
          {"plugin_data_overflow", [](const sbox_config_api& a, sbox_config c) {
             const std::string big(kSboxMaxPluginData + 1, 'x');
             a.set_plugin_data(c, big.data(), big.size());
           }},
          {"capability_bad_sid", [](const sbox_config_api& a, sbox_config c) {
             a.add_capability(c, u"");
           }},
      };
  for (const auto& injection : injections) {
    sbox_config_s cfg;
    CHECK(BuildValid(api, &cfg), "baseline build");
    injection.second(api, &cfg);
    CHECK(cfg.invalid, std::string("injection left builder un-poisoned: ") +
                           injection.first);
    SboxConfigMarshal m;
    CHECK(!MarshalSboxConfig(cfg, L"v8host.dll", m),
          std::string("poisoned builder marshaled (would spawn): ") +
              injection.first);
  }
  return true;
}

bool MarshalCrossFieldGate(std::string* detail) {
  // Defense-in-depth branches of MarshalSboxConfig, reached by constructing the
  // inconsistent state directly (the setters already block most of these).
  {  // Capability present but AppContainer off: add_capability itself succeeds,
     // so only the marshal gate catches it.
    const sbox_config_api api = MakeConfigApi();
    sbox_config_s cfg;
    CHECK(api.add_capability(&cfg, u"S-1-15-3-1") == sbox_status_ok, "cap add ok");
    CHECK(!cfg.invalid, "add_capability must not poison on its own");
    SboxConfigMarshal m;
    CHECK(!MarshalSboxConfig(cfg, L"v8host.dll", m), "cap-without-AC: no spawn");
  }
  {  // LPAC without AppContainer (belt-and-suspenders; unreachable via setters).
    sbox_config_s cfg;
    cfg.lpac = true;
    cfg.use_app_container = false;
    SboxConfigMarshal m;
    CHECK(!MarshalSboxConfig(cfg, L"v8host.dll", m), "LPAC-without-AC: no spawn");
  }
  {  // Explicit poison flag always blocks the spawn.
    sbox_config_s cfg;
    cfg.invalid = true;
    SboxConfigMarshal m;
    CHECK(!MarshalSboxConfig(cfg, L"v8host.dll", m), "poison flag: no spawn");
  }
  return true;
}

// The broker reads a ring whose producer index is worker-controlled.
bool Fail(std::string* detail, const char* message) {
  *detail = message;
  return false;
}
bool ChannelDrainBounds(std::string* detail) {
  using namespace sbox_msg;
  std::vector<uint8_t> section(kSectionSize, 0);
  InitHeader(section.data());
  ChannelHeader* h = Header(section.data());
  uint8_t* ring = T2BRing(section.data());
  struct Count { int frames = 0; } count;
  const FrameCb cb = [](void* ctx, int, const void*, size_t) { ++static_cast<Count*>(ctx)->frames; };
  const uint8_t payload[4] = {1, 2, 3, 4};
  uint32_t cursor = 0;
  for (int i = 0; i < 3; ++i)
    if (!Post(&h->t2b, ring, 1, payload, sizeof(payload))) return Fail(detail, "post");
  // A tampered read_idx doesn't move the private cursor.
  h->t2b.read_idx = 0xDEADBEEFu;
  if (Drain(&h->t2b, ring, cb, &count, &cursor) != 3 || count.frames != 3 || cursor != 3)
    return Fail(detail, "drain from the private cursor");
  // Exactly one ring ahead is the most a producer can legitimately be.
  h->t2b.write_idx = cursor + kSlots;
  count.frames = 0;
  if (Drain(&h->t2b, ring, cb, &count, &cursor) != static_cast<int>(kSlots) || cursor != 3 + kSlots)
    return Fail(detail, "a full ring was rejected");
  // Behind the consumer, or more than a ring ahead: corruption, no callback, no movement.
  for (uint32_t bad : {cursor - 1, cursor + kSlots + 1, cursor + 0x80000000u}) {
    h->t2b.write_idx = bad;
    count.frames = 0;
    const uint32_t before = cursor;
    if (Drain(&h->t2b, ring, cb, &count, &cursor) != -1 || count.frames != 0 || cursor != before)
      return Fail(detail, "an impossible producer index was accepted");
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<TestCase> tests = {
      {"channel", "drain_bounds", ChannelDrainBounds},
      {"setters", "success", SettersSuccess},
      {"setters", "null_arg", SettersNullArg},
      {"setters", "integrity_invalid", IntegrityInvalid},
      {"setters", "tokens_invalid", TokensInvalid},
      {"setters", "file_rule_bad_pattern", FileRuleBadPattern},
      {"setters", "capability_bad_sid", CapabilityBadSid},
      {"setters", "engine_dll_bad_name", EngineDllBadName},
      {"limits", "file_rule_quota", FileRuleQuota},
      {"limits", "capability_quota", CapabilityQuota},
      {"limits", "plugin_data_quota", PluginDataQuota},
      {"appcontainer", "combinations", AppContainerCombos},
      {"marshal", "field_fidelity", MarshalFieldFidelity},
      {"marshal", "default_tokens", MarshalDefaultTokens},
      {"discard", "poisoned_builder_never_marshals", PoisonedBuilderNeverMarshals},
      {"discard", "cross_field_gate", MarshalCrossFieldGate},
  };
  return v8host::test::RunTests(argc, argv, tests);
}
