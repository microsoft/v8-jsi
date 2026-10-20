// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Deterministic, in-process conformance tests for spawn configuration:
//   * the neutral spawn/worker-profile codec (:spawn_config) —
//     V8HostWorkerProfileV1 and V8HostSpawnConfigV1 round-trips, canonical
//     exact-byte vectors (so the encodings are pinned cross-arch), and the
//     fail-closed decode rejections; and
//   * the engine's real configure() lowering (ApplySpawnConfig, compiled in) —
//     driven onto the real MakeConfigApi builder + MarshalSboxConfig gate
//     so a spawn config lowers 1:1 to the policy and a
//     trusted-under-ACG / jitless<->ACG mismatch is rejected.
// No pipes, no spawned worker: the `codec` suites are pure byte vectors and the
// `apply` suite drives the builder directly.

#include "v8host_test_support.h"  // brings in <windows.h> first

#include "v8host_spawn_config.h"  // its transitive v8host_protocol.h #undefs ERROR
#include "v8host_spawn_apply.h"
#include "sbox_config.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

using v8host::test::TestCase;
using v8host::V8HostSpawnConfigV1;
using v8host::V8HostSpawnFileRule;
using v8host::V8HostWorkerProfileV1;
using v8host::V8HostWorkerProfileV2;

#define CHECK(cond, msg) \
  do {                   \
    if (!(cond)) {       \
      *detail = (msg);   \
      return false;      \
    }                    \
  } while (0)

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------
bool CheckBytes(const char* label,
                const std::vector<uint8_t>& got,
                const uint8_t* want,
                size_t want_len,
                std::string* detail) {
  if (got.size() != want_len) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s: size %zu != %zu", label, got.size(),
                  want_len);
    *detail = buf;
    return false;
  }
  for (size_t i = 0; i < want_len; ++i) {
    if (got[i] != want[i]) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s: byte[%zu] = 0x%02X != 0x%02X", label,
                    i, got[i], want[i]);
      *detail = buf;
      return false;
    }
  }
  return true;
}

// A valid baseline worker profile (jitless + an engine + a snapshot).
V8HostWorkerProfileV1 SampleWorkerProfile() {
  V8HostWorkerProfileV1 p;
  p.jitless = 1;
  p.engine_dll = "v8jsisb.dll";
  p.snapshot_path = "snap.bin";
  return p;
}

// A fully-populated valid spawn config (AppContainer on, file rules + a
// capability, a snapshot). effective_tier/ACG are kept consistent (jitless).
V8HostSpawnConfigV1 SampleSpawnConfig() {
  V8HostSpawnConfigV1 s;
  s.tier = v8host::kTierUntrusted;
  s.integrity = sbox_integrity_level_low;
  s.delayed_integrity = sbox_integrity_level_untrusted;
  s.initial_token = sbox_token_level_restricted_same_access;
  s.lockdown_token = sbox_token_level_lockdown;
  s.prohibit_dynamic_code = true;  // ACG on (matches jitless)
  s.use_app_container = true;
  s.low_privilege_app_container = true;
  s.app_container_profile = "com.example.profile";
  s.file_rules = {{true, "C:\\app\\data\\*"}, {false, "C:\\app\\tmp\\*"}};
  s.capabilities = {"S-1-15-3-1"};
  s.effective_tier = v8host::kTierUntrusted;
  s.engine_dll = "v8jsisb.dll";
  s.snapshot_path = "snap.bin";
  return s;
}

bool SameWorkerProfile(const V8HostWorkerProfileV1& a,
                       const V8HostWorkerProfileV1& b) {
  return a.jitless == b.jitless && a.engine_dll == b.engine_dll &&
         a.snapshot_path == b.snapshot_path;
}

bool SameSpawnConfig(const V8HostSpawnConfigV1& a,
                     const V8HostSpawnConfigV1& b) {
  if (a.tier != b.tier || a.integrity != b.integrity ||
      a.delayed_integrity != b.delayed_integrity ||
      a.initial_token != b.initial_token ||
      a.lockdown_token != b.lockdown_token ||
      a.prohibit_dynamic_code != b.prohibit_dynamic_code ||
      a.use_app_container != b.use_app_container ||
      a.low_privilege_app_container != b.low_privilege_app_container ||
      a.app_container_profile != b.app_container_profile ||
      a.effective_tier != b.effective_tier || a.engine_dll != b.engine_dll ||
      a.snapshot_path != b.snapshot_path || a.max_runtimes != b.max_runtimes ||
      a.snapshot_paths != b.snapshot_paths)
    return false;
  if (a.file_rules.size() != b.file_rules.size() ||
      a.capabilities != b.capabilities)
    return false;
  for (size_t i = 0; i < a.file_rules.size(); ++i) {
    if (a.file_rules[i].readonly != b.file_rules[i].readonly ||
        a.file_rules[i].pattern != b.file_rules[i].pattern)
      return false;
  }
  return true;
}

std::wstring Widen(const std::string& s) {
  return std::wstring(s.begin(), s.end());  // ASCII test data only
}

// ---------------------------------------------------------------------------
// Canonical exact-byte vectors (pin the two encodings byte-for-byte). Any
// change to the layout fails these before any consumer breaks.
// ---------------------------------------------------------------------------

// V8HostWorkerProfileV1 { jitless=1, engine="v8jsisb.dll", snapshot="snap.bin" }.
const uint8_t kWorkerProfileVector[] = {
    0x01, 0x00,              // schema_version = 1
    0x0C, 0x00,              // fixed_size = 12
    0x27, 0x00, 0x00, 0x00,  // total_size = 39
    0x01, 0x00, 0x00, 0x00,  // jitless = 1
    0x0B, 0x00, 0x00, 0x00,  // engine_dll length = 11
    0x76, 0x38, 0x6A, 0x73, 0x69, 0x73, 0x62, 0x2E, 0x64, 0x6C, 0x6C,  // v8jsisb.dll
    0x08, 0x00, 0x00, 0x00,  // snapshot_path length = 8
    0x73, 0x6E, 0x61, 0x70, 0x2E, 0x62, 0x69, 0x6E,  // snap.bin
};

// V8HostSpawnConfigV1: untrusted + ACG, AppContainer + LPAC on, profile
// "ac.profile", one read-only file rule "docs", one capability "S-1-15-3-1",
// effective engine "v8jsisb.dll", no snapshot.
const uint8_t kSpawnConfigVector[] = {
    0x01, 0x00,              // schema_version = 1
    0x34, 0x00,              // fixed_size = 52
    0x6F, 0x00, 0x00, 0x00,  // total_size = 111
    0x00, 0x00, 0x00, 0x00,  // tier = 0
    0x00, 0x00, 0x00, 0x00,  // integrity = 0
    0x01, 0x00, 0x00, 0x00,  // delayed_integrity = 1
    0x04, 0x00, 0x00, 0x00,  // initial_token = 4
    0x00, 0x00, 0x00, 0x00,  // lockdown_token = 0
    0x01, 0x00, 0x00, 0x00,  // prohibit_dynamic_code = 1
    0x01, 0x00, 0x00, 0x00,  // use_app_container = 1
    0x01, 0x00, 0x00, 0x00,  // low_privilege_app_container = 1
    0x00, 0x00, 0x00, 0x00,  // effective_tier = 0
    0x01, 0x00, 0x00, 0x00,  // file_rule_count = 1
    0x01, 0x00, 0x00, 0x00,  // capability_count = 1
    0x0A, 0x00, 0x00, 0x00,  // app_container_profile length = 10
    0x61, 0x63, 0x2E, 0x70, 0x72, 0x6F, 0x66, 0x69, 0x6C, 0x65,  // ac.profile
    0x01, 0x00, 0x00, 0x00,  // file_rule[0].readonly = 1
    0x04, 0x00, 0x00, 0x00,  // file_rule[0].pattern length = 4
    0x64, 0x6F, 0x63, 0x73,  // docs
    0x0A, 0x00, 0x00, 0x00,  // capability[0] length = 10
    0x53, 0x2D, 0x31, 0x2D, 0x31, 0x35, 0x2D, 0x33, 0x2D, 0x31,  // S-1-15-3-1
    0x0B, 0x00, 0x00, 0x00,  // engine_dll length = 11
    0x76, 0x38, 0x6A, 0x73, 0x69, 0x73, 0x62, 0x2E, 0x64, 0x6C, 0x6C,  // v8jsisb.dll
    0x00, 0x00, 0x00, 0x00,  // snapshot_path length = 0 (no snapshot)
};

V8HostSpawnConfigV1 CanonicalSpawnConfig() {
  V8HostSpawnConfigV1 s;
  s.tier = 0;
  s.integrity = 0;
  s.delayed_integrity = 1;
  s.initial_token = 4;
  s.lockdown_token = 0;
  s.prohibit_dynamic_code = true;
  s.use_app_container = true;
  s.low_privilege_app_container = true;
  s.app_container_profile = "ac.profile";
  s.file_rules = {{true, "docs"}};
  s.capabilities = {"S-1-15-3-1"};
  s.effective_tier = 0;
  s.engine_dll = "v8jsisb.dll";
  s.snapshot_path = "";
  return s;
}

// ===========================================================================
// codec-worker suite — V8HostWorkerProfileV1
// ===========================================================================
bool WorkerRoundTrip(std::string* detail) {
  const std::vector<std::function<V8HostWorkerProfileV1()>> cases = {
      [] { return SampleWorkerProfile(); },
      [] {
        V8HostWorkerProfileV1 p;
        p.jitless = 0;  // JIT (trusted), engine present, no snapshot
        p.engine_dll = "v8jsi.dll";
        return p;
      },
      [] {
        V8HostWorkerProfileV1 p;  // jitless, empty snapshot
        p.jitless = 1;
        p.engine_dll = "e.dll";
        return p;
      },
      [] {
        V8HostWorkerProfileV1 p;  // long (but bounded) engine + snapshot
        p.jitless = 1;
        p.engine_dll = std::string(200, 'a') + ".dll";
        p.snapshot_path = std::string(3000, 'p');
        return p;
      },
  };
  for (size_t i = 0; i < cases.size(); ++i) {
    const V8HostWorkerProfileV1 in = cases[i]();
    const std::vector<uint8_t> bytes = in.Encode();
    CHECK(bytes.size() <= v8host::kWorkerProfileMaxSize, "profile over ceiling");
    V8HostWorkerProfileV1 out;
    CHECK(V8HostWorkerProfileV1::Decode(bytes.data(), bytes.size(), &out),
          "worker decode failed");
    CHECK(SameWorkerProfile(in, out), "worker round-trip mismatch");
  }
  return true;
}

bool WorkerCanonicalBytes(std::string* detail) {
  const V8HostWorkerProfileV1 p = SampleWorkerProfile();
  CHECK(CheckBytes("worker-profile", p.Encode(), kWorkerProfileVector,
                   sizeof(kWorkerProfileVector), detail),
        detail->c_str());
  // And the frozen bytes decode back to the same profile.
  V8HostWorkerProfileV1 out;
  CHECK(V8HostWorkerProfileV1::Decode(kWorkerProfileVector,
                                      sizeof(kWorkerProfileVector), &out),
        "canonical worker bytes must decode");
  CHECK(SameWorkerProfile(p, out), "canonical worker decode mismatch");
  return true;
}

bool WorkerRejectBadSchema(std::string* detail) {
  std::vector<uint8_t> bytes = SampleWorkerProfile().Encode();
  bytes[0] = 2;  // schema_version = 2
  V8HostWorkerProfileV1 out;
  CHECK(!V8HostWorkerProfileV1::Decode(bytes.data(), bytes.size(), &out),
        "bad schema must be rejected");
  return true;
}

bool WorkerRejectTruncation(std::string* detail) {
  const std::vector<uint8_t> full = SampleWorkerProfile().Encode();
  for (size_t n = 0; n < full.size(); ++n) {
    V8HostWorkerProfileV1 out;
    CHECK(!V8HostWorkerProfileV1::Decode(full.data(), n, &out),
          "every truncated prefix must be rejected");
  }
  return true;
}

bool WorkerRejectOverCeiling(std::string* detail) {
  // A blob whose declared total_size exceeds the 4 KiB ceiling (content is
  // irrelevant: the header check rejects before any field is read).
  std::vector<uint8_t> bytes(v8host::kWorkerProfileMaxSize + 1, 0);
  bytes[0] = 1;   // schema_version = 1
  bytes[2] = 12;  // fixed_size = 12
  const uint32_t total = static_cast<uint32_t>(bytes.size());
  bytes[4] = static_cast<uint8_t>(total & 0xFF);
  bytes[5] = static_cast<uint8_t>((total >> 8) & 0xFF);
  bytes[6] = static_cast<uint8_t>((total >> 16) & 0xFF);
  bytes[7] = static_cast<uint8_t>((total >> 24) & 0xFF);
  V8HostWorkerProfileV1 out;
  CHECK(!V8HostWorkerProfileV1::Decode(bytes.data(), bytes.size(), &out),
        "over-4096 profile must be rejected");
  return true;
}

bool WorkerRejectTrailing(std::string* detail) {
  // total_size claims four extra bytes that the body never consumes -> the exact-
  // consumption (AtEnd) check rejects it.
  std::vector<uint8_t> bytes = SampleWorkerProfile().Encode();
  bytes.insert(bytes.end(), {0, 0, 0, 0});
  const uint32_t total = static_cast<uint32_t>(bytes.size());
  bytes[4] = static_cast<uint8_t>(total & 0xFF);
  bytes[5] = static_cast<uint8_t>((total >> 8) & 0xFF);
  bytes[6] = static_cast<uint8_t>((total >> 16) & 0xFF);
  bytes[7] = static_cast<uint8_t>((total >> 24) & 0xFF);
  V8HostWorkerProfileV1 out;
  CHECK(!V8HostWorkerProfileV1::Decode(bytes.data(), bytes.size(), &out),
        "trailing bytes must be rejected");
  return true;
}

bool WorkerRejectBadJitless(std::string* detail) {
  std::vector<uint8_t> bytes = SampleWorkerProfile().Encode();
  bytes[8] = 2;  // jitless = 2 (not a boolean)
  V8HostWorkerProfileV1 out;
  CHECK(!V8HostWorkerProfileV1::Decode(bytes.data(), bytes.size(), &out),
        "non-boolean jitless must be rejected");
  return true;
}

bool WorkerRejectBadUtf8(std::string* detail) {
  // Replace the first engine byte with a bare continuation byte (invalid UTF-8).
  std::vector<uint8_t> bytes = SampleWorkerProfile().Encode();
  bytes[16] = 0x80;  // first engine_dll byte
  V8HostWorkerProfileV1 out;
  CHECK(!V8HostWorkerProfileV1::Decode(bytes.data(), bytes.size(), &out),
        "invalid UTF-8 must be rejected");
  return true;
}

bool WorkerRejectNonBareEngine(std::string* detail) {
  V8HostWorkerProfileV1 p = SampleWorkerProfile();
  p.engine_dll = "sub\\evil.dll";  // a path, not a bare filename
  const std::vector<uint8_t> bytes = p.Encode();
  V8HostWorkerProfileV1 out;
  CHECK(!V8HostWorkerProfileV1::Decode(bytes.data(), bytes.size(), &out),
        "non-bare engine must be rejected");
  // An empty engine is likewise rejected.
  V8HostWorkerProfileV1 empty;
  empty.jitless = 1;
  empty.engine_dll = "";
  const std::vector<uint8_t> eb = empty.Encode();
  CHECK(!V8HostWorkerProfileV1::Decode(eb.data(), eb.size(), &out),
        "empty engine must be rejected");
  // A lone "." is rejected too (uniform bare-filename predicate).
  V8HostWorkerProfileV1 dot = SampleWorkerProfile();
  dot.engine_dll = ".";
  const std::vector<uint8_t> db = dot.Encode();
  CHECK(!V8HostWorkerProfileV1::Decode(db.data(), db.size(), &out),
        "lone-dot engine must be rejected");
  return true;
}

bool WorkerForwardCompat(std::string* detail) {
  // A future minor version appends four trailing fixed bytes (fixed_size 16); a
  // v1 reader skips them to reach the strings.
  const V8HostWorkerProfileV1 p = SampleWorkerProfile();
  std::vector<uint8_t> bytes;
  bytes.insert(bytes.end(), {0x01, 0x00});  // schema
  bytes.insert(bytes.end(), {0x10, 0x00});  // fixed_size = 16
  std::vector<uint8_t> tail;
  tail.insert(tail.end(), {0x01, 0x00, 0x00, 0x00});              // jitless
  tail.insert(tail.end(), {0xAA, 0xBB, 0xCC, 0xDD});              // extra fixed
  tail.insert(tail.end(), {0x0B, 0x00, 0x00, 0x00});              // engine len 11
  const std::string eng = "v8jsisb.dll";
  tail.insert(tail.end(), eng.begin(), eng.end());
  tail.insert(tail.end(), {0x08, 0x00, 0x00, 0x00});  // snapshot len 8
  const std::string snap = "snap.bin";
  tail.insert(tail.end(), snap.begin(), snap.end());
  const uint32_t total = 8 + static_cast<uint32_t>(tail.size());
  bytes.insert(bytes.end(), {static_cast<uint8_t>(total & 0xFF),
                             static_cast<uint8_t>((total >> 8) & 0xFF),
                             static_cast<uint8_t>((total >> 16) & 0xFF),
                             static_cast<uint8_t>((total >> 24) & 0xFF)});
  bytes.insert(bytes.end(), tail.begin(), tail.end());
  V8HostWorkerProfileV1 out;
  CHECK(V8HostWorkerProfileV1::Decode(bytes.data(), bytes.size(), &out),
        "forward-compat profile must decode");
  CHECK(SameWorkerProfile(p, out), "forward-compat profile mismatch");
  return true;
}

// ===========================================================================
// codec-spawn suite — V8HostSpawnConfigV1
// ===========================================================================
bool SpawnRoundTrip(std::string* detail) {
  std::vector<V8HostSpawnConfigV1> cases;
  cases.push_back(SampleSpawnConfig());
  {  // ordinary mode: no AppContainer, no capabilities, no file rules
    V8HostSpawnConfigV1 s;
    s.tier = v8host::kTierTrusted;
    s.integrity = 0;
    s.delayed_integrity = 1;
    s.initial_token = 2;
    s.lockdown_token = 1;
    s.prohibit_dynamic_code = false;
    s.effective_tier = v8host::kTierTrusted;
    s.engine_dll = "v8jsi.dll";
    cases.push_back(s);
  }
  {  // boundary: 64 file rules + 64 capabilities (AppContainer on)
    V8HostSpawnConfigV1 s;
    s.use_app_container = true;
    s.app_container_profile = "p";
    s.prohibit_dynamic_code = true;
    s.effective_tier = v8host::kTierUntrusted;
    s.engine_dll = "e.dll";
    for (int i = 0; i < 64; ++i) {
      s.file_rules.push_back({i % 2 == 0, "rule" + std::to_string(i)});
      s.capabilities.push_back("S-1-15-3-" + std::to_string(i));
    }
    cases.push_back(s);
  }
  {  // negative tiers/tokens round-trip verbatim (transported as-is)
    V8HostSpawnConfigV1 s;
    s.tier = -7;
    s.integrity = -1;
    s.delayed_integrity = -2;
    s.initial_token = -3;
    s.lockdown_token = -4;
    s.effective_tier = -9;  // not kTierTrusted -> jitless semantics downstream
    s.prohibit_dynamic_code = true;
    s.engine_dll = "e.dll";
    cases.push_back(s);
  }
  for (size_t i = 0; i < cases.size(); ++i) {
    const std::vector<uint8_t> bytes = cases[i].Encode();
    CHECK(bytes.size() <= v8host::kSpawnConfigMaxSize, "spawn over ceiling");
    V8HostSpawnConfigV1 out;
    CHECK(V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
          "spawn decode failed");
    CHECK(SameSpawnConfig(cases[i], out), "spawn round-trip mismatch");
  }
  return true;
}

bool SpawnCanonicalBytes(std::string* detail) {
  const V8HostSpawnConfigV1 s = CanonicalSpawnConfig();
  CHECK(CheckBytes("spawn-config", s.Encode(), kSpawnConfigVector,
                   sizeof(kSpawnConfigVector), detail),
        detail->c_str());
  V8HostSpawnConfigV1 out;
  CHECK(V8HostSpawnConfigV1::Decode(kSpawnConfigVector,
                                    sizeof(kSpawnConfigVector), &out),
        "canonical spawn bytes must decode");
  CHECK(SameSpawnConfig(s, out), "canonical spawn decode mismatch");
  return true;
}

bool SpawnRejectBadSchema(std::string* detail) {
  std::vector<uint8_t> bytes = SampleSpawnConfig().Encode();
  bytes[0] = 2;
  V8HostSpawnConfigV1 out;
  CHECK(!V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
        "bad schema must be rejected");
  return true;
}

bool SpawnRejectCount65(std::string* detail) {
  {  // 65 file rules
    V8HostSpawnConfigV1 s;
    s.engine_dll = "e.dll";
    s.prohibit_dynamic_code = true;
    s.effective_tier = v8host::kTierUntrusted;
    for (int i = 0; i < 65; ++i)
      s.file_rules.push_back({false, "r"});
    const std::vector<uint8_t> bytes = s.Encode();
    V8HostSpawnConfigV1 out;
    CHECK(!V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
          "65 file rules must be rejected");
  }
  {  // 65 capabilities (AppContainer on)
    V8HostSpawnConfigV1 s;
    s.use_app_container = true;
    s.app_container_profile = "p";
    s.engine_dll = "e.dll";
    s.prohibit_dynamic_code = true;
    s.effective_tier = v8host::kTierUntrusted;
    for (int i = 0; i < 65; ++i)
      s.capabilities.push_back("S-1-15-3-1");
    const std::vector<uint8_t> bytes = s.Encode();
    V8HostSpawnConfigV1 out;
    CHECK(!V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
          "65 capabilities must be rejected");
  }
  return true;
}

bool SpawnRejectOversizeString(std::string* detail) {
  // A single string whose length prefix exceeds kMaxStringBytes (32 KiB) is
  // rejected by the Reader even though the whole payload fits under 64 KiB.
  V8HostSpawnConfigV1 s;
  s.use_app_container = true;
  s.app_container_profile = std::string(33 * 1024, 'x');  // > 32 KiB
  s.engine_dll = "e.dll";
  s.prohibit_dynamic_code = true;
  s.effective_tier = v8host::kTierUntrusted;
  const std::vector<uint8_t> bytes = s.Encode();
  CHECK(bytes.size() <= v8host::kSpawnConfigMaxSize, "fixture must fit 64 KiB");
  V8HostSpawnConfigV1 out;
  CHECK(!V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
        "oversize string must be rejected");
  return true;
}

bool SpawnRejectTotalSizeMismatch(std::string* detail) {
  std::vector<uint8_t> bytes = SampleSpawnConfig().Encode();
  bytes[4] ^= 0x01;  // perturb total_size so it no longer equals the size
  V8HostSpawnConfigV1 out;
  CHECK(!V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
        "total_size mismatch must be rejected");
  return true;
}

bool SpawnRejectTrailing(std::string* detail) {
  std::vector<uint8_t> bytes = SampleSpawnConfig().Encode();
  bytes.insert(bytes.end(), {0, 0, 0, 0});
  const uint32_t total = static_cast<uint32_t>(bytes.size());
  bytes[4] = static_cast<uint8_t>(total & 0xFF);
  bytes[5] = static_cast<uint8_t>((total >> 8) & 0xFF);
  bytes[6] = static_cast<uint8_t>((total >> 16) & 0xFF);
  bytes[7] = static_cast<uint8_t>((total >> 24) & 0xFF);
  V8HostSpawnConfigV1 out;
  CHECK(!V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
        "trailing bytes must be rejected");
  return true;
}

bool SpawnRejectOver64KiB(std::string* detail) {
  // Declared total_size above the 64 KiB ceiling (content irrelevant).
  std::vector<uint8_t> bytes(v8host::kSpawnConfigMaxSize + 1, 0);
  bytes[0] = 1;   // schema_version = 1
  bytes[2] = 52;  // fixed_size = 52
  const uint32_t total = static_cast<uint32_t>(bytes.size());
  bytes[4] = static_cast<uint8_t>(total & 0xFF);
  bytes[5] = static_cast<uint8_t>((total >> 8) & 0xFF);
  bytes[6] = static_cast<uint8_t>((total >> 16) & 0xFF);
  bytes[7] = static_cast<uint8_t>((total >> 24) & 0xFF);
  V8HostSpawnConfigV1 out;
  CHECK(!V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
        "over-64KiB spawn config must be rejected");
  return true;
}

bool SpawnRejectBadBool(std::string* detail) {
  // prohibit_dynamic_code lives at offset 28; set it to 2 (not a boolean).
  std::vector<uint8_t> bytes = SampleSpawnConfig().Encode();
  bytes[28] = 2;
  V8HostSpawnConfigV1 out;
  CHECK(!V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
        "non-boolean flag must be rejected");
  return true;
}

bool SpawnRejectCrossField(std::string* detail) {
  {  // empty AppContainer profile while AppContainer is on
    V8HostSpawnConfigV1 s;
    s.use_app_container = true;
    s.app_container_profile = "";
    s.engine_dll = "e.dll";
    s.prohibit_dynamic_code = true;
    s.effective_tier = v8host::kTierUntrusted;
    const std::vector<uint8_t> bytes = s.Encode();
    V8HostSpawnConfigV1 out;
    CHECK(!V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
          "AC on with empty profile must be rejected");
  }
  {  // capabilities present while AppContainer is off
    V8HostSpawnConfigV1 s;
    s.capabilities = {"S-1-15-3-1"};
    s.engine_dll = "e.dll";
    s.prohibit_dynamic_code = true;
    s.effective_tier = v8host::kTierUntrusted;
    const std::vector<uint8_t> bytes = s.Encode();
    V8HostSpawnConfigV1 out;
    CHECK(!V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
          "caps without AC must be rejected");
  }
  {  // LPAC without AppContainer
    V8HostSpawnConfigV1 s;
    s.low_privilege_app_container = true;
    s.engine_dll = "e.dll";
    s.prohibit_dynamic_code = true;
    s.effective_tier = v8host::kTierUntrusted;
    const std::vector<uint8_t> bytes = s.Encode();
    V8HostSpawnConfigV1 out;
    CHECK(!V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
          "LPAC without AC must be rejected");
  }
  return true;
}

bool SpawnRejectNonBareEngine(std::string* detail) {
  V8HostSpawnConfigV1 s = SampleSpawnConfig();
  s.engine_dll = "C:\\evil.dll";
  const std::vector<uint8_t> bytes = s.Encode();
  V8HostSpawnConfigV1 out;
  CHECK(!V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
        "non-bare effective engine must be rejected");
  // A lone "." is rejected too (uniform bare-filename predicate).
  V8HostSpawnConfigV1 dot = SampleSpawnConfig();
  dot.engine_dll = ".";
  const std::vector<uint8_t> db = dot.Encode();
  CHECK(!V8HostSpawnConfigV1::Decode(db.data(), db.size(), &out),
        "lone-dot effective engine must be rejected");
  return true;
}

bool SpawnRejectBadUtf8(std::string* detail) {
  // Corrupt the first app_container_profile byte (offset 56 = 52-byte fixed
  // section + 4-byte length prefix): invalid UTF-8 and an embedded NUL must both
  // be rejected by the shared string validator.
  std::vector<uint8_t> bad = SampleSpawnConfig().Encode();
  bad[56] = 0x80;  // bare continuation byte
  V8HostSpawnConfigV1 out;
  CHECK(!V8HostSpawnConfigV1::Decode(bad.data(), bad.size(), &out),
        "invalid UTF-8 in a spawn-config string must be rejected");
  std::vector<uint8_t> nul = SampleSpawnConfig().Encode();
  nul[56] = 0x00;
  V8HostSpawnConfigV1 out2;
  CHECK(!V8HostSpawnConfigV1::Decode(nul.data(), nul.size(), &out2),
        "embedded NUL in a spawn-config string must be rejected");
  return true;
}

bool SpawnForwardCompat(std::string* detail) {
  // Append four trailing fixed bytes (fixed_size 56); a v1 reader skips them.
  V8HostSpawnConfigV1 s = SampleSpawnConfig();
  std::vector<uint8_t> bytes = s.Encode();
  // Splice 4 extra bytes right after the 52-byte fixed section, bump fixed_size
  // (offset 2) and total_size (offset 4).
  bytes.insert(bytes.begin() + 52, {0xAA, 0xBB, 0xCC, 0xDD});
  bytes[2] = 56;  // fixed_size = 56
  const uint32_t total = static_cast<uint32_t>(bytes.size());
  bytes[4] = static_cast<uint8_t>(total & 0xFF);
  bytes[5] = static_cast<uint8_t>((total >> 8) & 0xFF);
  bytes[6] = static_cast<uint8_t>((total >> 16) & 0xFF);
  bytes[7] = static_cast<uint8_t>((total >> 24) & 0xFF);
  V8HostSpawnConfigV1 out;
  CHECK(V8HostSpawnConfigV1::Decode(bytes.data(), bytes.size(), &out),
        "forward-compat spawn config must decode");
  CHECK(SameSpawnConfig(s, out), "forward-compat spawn mismatch");
  return true;
}

// ===========================================================================
// apply suite — the real configure() lowering (ApplySpawnConfig) onto the real
// MakeConfigApi builder + MarshalSboxConfig gate.
// ===========================================================================
bool ApplyLowersEveryField(std::string* detail) {
  const V8HostSpawnConfigV1 s = SampleSpawnConfig();
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  CHECK(v8host::ApplySpawnConfig(s, &api, &cfg), "ApplySpawnConfig must succeed");
  CHECK(!cfg.invalid, "a valid spawn config must not poison the builder");

  // Builder fields lowered 1:1 from the spawn config.
  CHECK(cfg.acg == s.prohibit_dynamic_code, "acg lowered");
  CHECK(cfg.initial_integrity == s.integrity, "integrity lowered");
  CHECK(cfg.delayed_integrity == s.delayed_integrity, "delayed integrity lowered");
  CHECK(cfg.initial_token == s.initial_token, "initial token lowered");
  CHECK(cfg.lockdown_token == s.lockdown_token, "lockdown token lowered");
  CHECK(cfg.use_app_container == s.use_app_container, "AC lowered");
  CHECK(cfg.lpac == s.low_privilege_app_container, "LPAC lowered");
  CHECK(cfg.profile == Widen(s.app_container_profile), "profile lowered");
  CHECK(cfg.file_patterns.size() == s.file_rules.size(), "file rule count");
  for (size_t i = 0; i < s.file_rules.size(); ++i) {
    CHECK(cfg.file_patterns[i] == Widen(s.file_rules[i].pattern), "rule pattern");
    CHECK(cfg.file_readonly[i] == (s.file_rules[i].readonly ? 1 : 0), "rule ro");
  }
  CHECK(cfg.capabilities.size() == s.capabilities.size(), "capability count");
  for (size_t i = 0; i < s.capabilities.size(); ++i)
    CHECK(cfg.capabilities[i] == Widen(s.capabilities[i]), "capability sid");
  CHECK(cfg.engine_dlls.size() == 1 && cfg.engine_dlls[0] == Widen(s.engine_dll),
        "effective engine allowed");

  // plugin_data is the encoded worker profile (jitless derived from the tier).
  V8HostWorkerProfileV1 prof;
  CHECK(V8HostWorkerProfileV1::Decode(
            reinterpret_cast<const uint8_t*>(cfg.plugin_data.data()),
            cfg.plugin_data.size(), &prof),
        "plugin_data must decode as a worker profile");
  CHECK(prof.jitless == 1, "untrusted effective tier -> jitless");
  CHECK(prof.engine_dll == s.engine_dll, "worker profile engine");
  CHECK(prof.snapshot_path == s.snapshot_path, "worker profile snapshot");

  // The marshaled policy (the pre-spawn gate) carries the same values.
  SboxConfigMarshal m;
  CHECK(MarshalSboxConfig(cfg, L"v8host.dll", m), "marshal must succeed");
  const SboxPolicy& p = m.policy;
  CHECK(p.prohibit_dynamic_code == (s.prohibit_dynamic_code ? 1 : 0), "policy acg");
  CHECK(p.integrity == s.integrity && p.delayed_integrity == s.delayed_integrity,
        "policy integrity");
  CHECK(p.initial_token == s.initial_token &&
            p.lockdown_token == s.lockdown_token,
        "policy tokens");
  CHECK(p.file_rule_count == s.file_rules.size(), "policy file rule count");
  CHECK(p.use_app_container == 1 && p.low_privilege_app_container == 1,
        "policy AC flags");
  CHECK(p.capability_count == s.capabilities.size(), "policy capability count");
  CHECK(p.plugin_data_len == cfg.plugin_data.size(), "policy plugin_data len");
  return true;
}

bool ApplyTrustedConsistent(std::string* detail) {
  // Trusted effective tier + ACG off -> JIT (jitless=0), accepted.
  V8HostSpawnConfigV1 s;
  s.tier = v8host::kTierTrusted;
  s.effective_tier = v8host::kTierTrusted;
  s.prohibit_dynamic_code = false;
  s.integrity = sbox_integrity_level_low;
  s.delayed_integrity = sbox_integrity_level_untrusted;
  s.initial_token = sbox_token_level_restricted_same_access;
  s.lockdown_token = sbox_token_level_lockdown;
  s.engine_dll = "v8jsi.dll";
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  CHECK(v8host::ApplySpawnConfig(s, &api, &cfg), "trusted+no-ACG must succeed");
  CHECK(cfg.acg == false, "trusted leaves ACG off");
  V8HostWorkerProfileV1 prof;
  CHECK(V8HostWorkerProfileV1::Decode(
            reinterpret_cast<const uint8_t*>(cfg.plugin_data.data()),
            cfg.plugin_data.size(), &prof),
        "worker profile decodes");
  CHECK(prof.jitless == 0, "trusted effective tier -> JIT (jitless=0)");
  return true;
}

bool ApplyRejectsTrustedUnderAcg(std::string* detail) {
  // Trusted/JIT effective tier under ACG is rejected (design §8.6).
  V8HostSpawnConfigV1 s;
  s.tier = v8host::kTierTrusted;
  s.effective_tier = v8host::kTierTrusted;
  s.prohibit_dynamic_code = true;  // ACG on -> inconsistent with JIT
  s.integrity = sbox_integrity_level_low;
  s.delayed_integrity = sbox_integrity_level_untrusted;
  s.initial_token = sbox_token_level_restricted_same_access;
  s.lockdown_token = sbox_token_level_lockdown;
  s.engine_dll = "v8jsi.dll";
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  CHECK(!v8host::ApplySpawnConfig(s, &api, &cfg),
        "trusted-under-ACG must be rejected");
  return true;
}

bool ApplyRejectsJitlessWithoutAcg(std::string* detail) {
  // A jitless (untrusted) effective tier without ACG is the symmetric mismatch.
  V8HostSpawnConfigV1 s;
  s.tier = v8host::kTierUntrusted;
  s.effective_tier = v8host::kTierUntrusted;
  s.prohibit_dynamic_code = false;  // ACG off -> inconsistent with jitless
  s.integrity = sbox_integrity_level_low;
  s.delayed_integrity = sbox_integrity_level_untrusted;
  s.initial_token = sbox_token_level_restricted_same_access;
  s.lockdown_token = sbox_token_level_lockdown;
  s.engine_dll = "v8jsisb.dll";
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  CHECK(!v8host::ApplySpawnConfig(s, &api, &cfg),
        "jitless-without-ACG must be rejected");
  return true;
}

bool ApplyFailsClosedOnBadSetter(std::string* detail) {
  // An out-of-range integrity makes set_integrity poison the builder; the lowering
  // must fail closed (and never reach a spawnable policy).
  V8HostSpawnConfigV1 s = SampleSpawnConfig();
  s.integrity = 99;  // not a known SboxIntegrityLevel
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  CHECK(!v8host::ApplySpawnConfig(s, &api, &cfg),
        "a rejected setter must fail ApplySpawnConfig closed");
  CHECK(cfg.invalid, "the builder must be poisoned");
  SboxConfigMarshal m;
  CHECK(!MarshalSboxConfig(cfg, L"v8host.dll", m),
        "a poisoned builder must not marshal a spawnable policy");
  return true;
}

bool ApplyNullArgs(std::string* detail) {
  const V8HostSpawnConfigV1 s = SampleSpawnConfig();
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  CHECK(!v8host::ApplySpawnConfig(s, nullptr, &cfg), "null api rejected");
  CHECK(!v8host::ApplySpawnConfig(s, &api, nullptr), "null cfg rejected");
  return true;
}

bool ApplyLowersNonDefaultTokens(std::string* detail) {
  // The sample config's tokens equal the sbox_config_s builder defaults
  // (RESTRICTED_SAME_ACCESS / LOCKDOWN), so a dropped set_tokens would go
  // unnoticed there. Use tokens that DIFFER from the defaults so the lowering is
  // actually observed end-to-end (builder + marshaled policy).
  V8HostSpawnConfigV1 s = SampleSpawnConfig();
  s.initial_token = sbox_token_level_restricted_non_admin;
  s.lockdown_token = sbox_token_level_limited;
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  CHECK(v8host::ApplySpawnConfig(s, &api, &cfg), "apply must succeed");
  CHECK(cfg.initial_token == sbox_token_level_restricted_non_admin,
        "non-default initial token lowered");
  CHECK(cfg.lockdown_token == sbox_token_level_limited,
        "non-default lockdown token lowered");
  SboxConfigMarshal m;
  CHECK(MarshalSboxConfig(cfg, L"v8host.dll", m), "marshal must succeed");
  CHECK(m.policy.initial_token == sbox_token_level_restricted_non_admin &&
            m.policy.lockdown_token == sbox_token_level_limited,
        "policy carries the non-default tokens");
  return true;
}

bool SameWorkerProfileV2(const V8HostWorkerProfileV2& a,
                         const V8HostWorkerProfileV2& b) {
  return a.jitless == b.jitless && a.max_runtimes == b.max_runtimes &&
         a.engine_dll == b.engine_dll && a.snapshot_paths == b.snapshot_paths;
}

void PatchTotalSize(std::vector<uint8_t>& bytes) {
  const uint32_t total = static_cast<uint32_t>(bytes.size());
  for (size_t i = 0; i < 4; ++i)
    bytes[4 + i] = static_cast<uint8_t>(total >> (8 * i));
}

bool WorkerV2CanonicalAndBounds(std::string* detail) {
  V8HostWorkerProfileV2 p;
  p.jitless = 1;
  p.max_runtimes = 2;
  p.engine_dll = "v8jsisb.dll";
  p.snapshot_paths = {"snap\\a.bin"};
  // 16 fixed + (4 + 11) engine + 4 count + (4 + 10) snapshot = 49.
  const uint8_t canonical[] = {
      0x02, 0x00, 0x10, 0x00, 0x31, 0x00, 0x00, 0x00,
      0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
      0x0B, 0x00, 0x00, 0x00,
      0x76, 0x38, 0x6A, 0x73, 0x69, 0x73, 0x62, 0x2E, 0x64, 0x6C, 0x6C,
      0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
      0x73, 0x6E, 0x61, 0x70, 0x5C, 0x61, 0x2E, 0x62, 0x69, 0x6E};
  CHECK(CheckBytes("worker-v2", p.Encode(), canonical, sizeof(canonical), detail),
        detail->c_str());
  V8HostWorkerProfileV2 out;
  CHECK(V8HostWorkerProfileV2::Decode(canonical, sizeof(canonical), &out) &&
            SameWorkerProfileV2(p, out), "canonical V2 decode mismatch");
  auto decode = [&](const std::vector<uint8_t>& bytes) {
    return V8HostWorkerProfileV2::Decode(bytes.data(), bytes.size(), &out);
  };
  auto accepts = [&](const V8HostWorkerProfileV2& value) {
    return decode(value.Encode());
  };
  for (uint32_t count : {0u, 8u, 9u}) {
    auto value = p;
    value.snapshot_paths.assign(count, "snap\\a.bin");
    CHECK(accepts(value) == (count <= 8), "V2 snapshot count bounds");
    if (count <= 8)
      CHECK(SameWorkerProfileV2(value, out), "V2 snapshot count round-trip");
  }
  for (uint32_t max : {0u, 1u, 16u, 17u}) {
    auto value = p;
    value.max_runtimes = max;
    CHECK(accepts(value) == (max == 1 || max == 16), "V2 runtime count bounds");
    if (max == 1 || max == 16)
      CHECK(SameWorkerProfileV2(value, out), "V2 runtime count round-trip");
  }
  auto bytes = p.Encode();
  bytes[0] = 3;
  CHECK(!decode(bytes), "V2 bad schema accepted");
  bytes = p.Encode();
  bytes[2] = 15;
  CHECK(!decode(bytes), "V2 short fixed section accepted");
  bytes = p.Encode();
  bytes[4] ^= 1;
  CHECK(!decode(bytes), "V2 total mismatch accepted");
  bytes = p.Encode();
  bytes[8] = 2;
  CHECK(!decode(bytes), "V2 non-boolean jitless accepted");
  bytes = p.Encode();
  for (size_t n = 0; n < bytes.size(); ++n)
    CHECK(!V8HostWorkerProfileV2::Decode(bytes.data(), n, &out),
          "V2 truncated prefix accepted");
  bytes.push_back(0);
  PatchTotalSize(bytes);
  CHECK(!decode(bytes), "V2 trailing byte accepted");

  auto large = p;
  large.snapshot_paths = {""};
  const size_t overhead = large.Encode().size();
  large.snapshot_paths[0].assign(4096 - overhead, 's');
  CHECK(large.snapshot_paths[0].size() <= v8host::protocol::kMaxStringBytes,
        "boundary fixture exceeds string limit");
  bytes = large.Encode();
  CHECK(bytes.size() == 4096 && decode(bytes) && SameWorkerProfileV2(large, out),
        "V2 exactly 4096 rejected");
  large.snapshot_paths[0].push_back('s');
  bytes = large.Encode();
  CHECK(bytes.size() == 4097 && !decode(bytes), "V2 4097 accepted");

  for (const std::string path :
       {std::string(1, char(0x80)), std::string("a\0b", 3)}) {
    auto value = p;
    value.snapshot_paths = {path};
    CHECK(!accepts(value), "V2 invalid UTF8/NUL snapshot accepted");
  }
  for (const char* engine : {"sub\\v8.dll", "C:v8.dll", "", "."}) {
    auto value = p;
    value.engine_dll = engine;
    CHECK(!accepts(value), "V2 non-bare engine accepted");
  }
  for (const char* path :
       {"C:\\x", "\\x", "/x", "a\\..\\b", "a\\.\\b", "a.", "a ", "a\\\\b"}) {
    auto value = p;
    value.snapshot_paths = {path};
    CHECK(!accepts(value), std::string("V2 non-relative snapshot accepted: ") + path);
  }
  bytes = p.Encode();
  bytes.insert(bytes.begin() + 16, {0xAA, 0xBB, 0xCC, 0xDD});
  bytes[2] = 20;
  PatchTotalSize(bytes);
  CHECK(decode(bytes) && SameWorkerProfileV2(p, out), "V2 forward-compat mismatch");
  return true;
}

bool WorkerDecodeCommonProfile(std::string* detail) {
  v8host::V8HostWorkerProfile out;
  for (bool snapshot : {false, true}) {
    auto p = SampleWorkerProfile();
    if (!snapshot)
      p.snapshot_path.clear();
    const auto bytes = p.Encode();
    CHECK(v8host::DecodeWorkerProfile(bytes.data(), bytes.size(), &out),
          "common V1 decode");
    const std::vector<std::string> snapshots =
        snapshot ? std::vector<std::string>{p.snapshot_path} : std::vector<std::string>{};
    CHECK(out.jitless == (p.jitless != 0) && out.engine_dll == p.engine_dll &&
              out.max_runtimes == 1 && out.precreate && !out.snapshots_relative &&
              out.snapshot_paths == snapshots, "common V1 fields");
  }
  V8HostWorkerProfileV2 p;
  p.jitless = 0;
  p.max_runtimes = 4;
  p.engine_dll = "v8jsi.dll";
  p.snapshot_paths = {"snap\\a.bin", "snap\\b.bin"};
  auto bytes = p.Encode();
  CHECK(v8host::DecodeWorkerProfile(bytes.data(), bytes.size(), &out) &&
            !out.jitless && out.engine_dll == p.engine_dll &&
            out.max_runtimes == p.max_runtimes && !out.precreate &&
            out.snapshots_relative && out.snapshot_paths == p.snapshot_paths,
        "common V2 fields");
  bytes[0] = 3;
  CHECK(!v8host::DecodeWorkerProfile(bytes.data(), bytes.size(), &out),
        "common unknown schema accepted");
  return true;
}

bool SpawnExplicitWorkerSchema2(std::string* detail) {
  auto s = CanonicalSpawnConfig();
  s.max_runtimes = 4;
  s.snapshot_paths = {"snap\\a.bin", "snap\\b.bin"};
  auto bytes = s.Encode();
  CHECK(bytes[0] == 2 && bytes[1] == 0 && bytes[2] == 56 && bytes[3] == 0 &&
            bytes[52] == 4 && bytes[53] == 0 && bytes[54] == 0 && bytes[55] == 0,
        "spawn schema2 layout");
  V8HostSpawnConfigV1 out;
  auto decode = [&](const std::vector<uint8_t>& blob) {
    return V8HostSpawnConfigV1::Decode(blob.data(), blob.size(), &out);
  };
  CHECK(decode(bytes) && SameSpawnConfig(s, out), "spawn schema2 round-trip");
  auto session = s;
  session.max_runtimes = 0;
  CHECK(CheckBytes("session unchanged", session.Encode(), kSpawnConfigVector,
                   sizeof(kSpawnConfigVector), detail), detail->c_str());
  for (uint8_t max : {uint8_t{0}, uint8_t{17}}) {
    auto bad = bytes;
    bad[52] = max;
    CHECK(!decode(bad), "spawn schema2 invalid max accepted");
  }
  auto bad = s;
  bad.snapshot_path = "session.bin";
  CHECK(!decode(bad.Encode()), "spawn schema2 session snapshot accepted");
  bad = s;
  bad.snapshot_paths.assign(9, "snap.bin");
  CHECK(!decode(bad.Encode()), "spawn schema2 nine snapshots accepted");
  bad = s;
  bad.snapshot_paths = {"a\\..\\b"};
  CHECK(!decode(bad.Encode()), "spawn schema2 non-relative snapshot accepted");
  return true;
}

bool ApplyExplicitWorkerProfileV2(std::string* detail) {
  auto s = SampleSpawnConfig();
  s.max_runtimes = 3;
  s.snapshot_path.clear();
  s.snapshot_paths = {"snap\\a.bin"};
  const sbox_config_api api = MakeConfigApi();
  sbox_config_s cfg;
  CHECK(v8host::ApplySpawnConfig(s, &api, &cfg) && !cfg.invalid,
        "apply explicit worker failed");
  const auto* data = reinterpret_cast<const uint8_t*>(cfg.plugin_data.data());
  V8HostWorkerProfileV2 out;
  CHECK(V8HostWorkerProfileV2::Decode(data, cfg.plugin_data.size(), &out) &&
            out.jitless == 1 && out.max_runtimes == s.max_runtimes &&
            out.engine_dll == s.engine_dll && out.snapshot_paths == s.snapshot_paths,
        "apply explicit V2 profile mismatch");
  v8host::V8HostWorkerProfile common;
  CHECK(v8host::DecodeWorkerProfile(data, cfg.plugin_data.size(), &common) &&
            common.jitless && common.max_runtimes == s.max_runtimes &&
            common.engine_dll == s.engine_dll &&
            common.snapshot_paths == s.snapshot_paths &&
            common.snapshots_relative && !common.precreate,
        "apply common profile mismatch");
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<TestCase> tests = {
      {"codec-worker", "round-trip", WorkerRoundTrip},
      {"codec-worker", "canonical-bytes", WorkerCanonicalBytes},
      {"codec-worker", "reject-bad-schema", WorkerRejectBadSchema},
      {"codec-worker", "reject-truncation", WorkerRejectTruncation},
      {"codec-worker", "reject-over-ceiling", WorkerRejectOverCeiling},
      {"codec-worker", "reject-trailing", WorkerRejectTrailing},
      {"codec-worker", "reject-bad-jitless", WorkerRejectBadJitless},
      {"codec-worker", "reject-bad-utf8", WorkerRejectBadUtf8},
      {"codec-worker", "reject-non-bare-engine", WorkerRejectNonBareEngine},
      {"codec-worker", "forward-compat", WorkerForwardCompat},
      {"codec-spawn", "round-trip", SpawnRoundTrip},
      {"codec-spawn", "canonical-bytes", SpawnCanonicalBytes},
      {"codec-spawn", "reject-bad-schema", SpawnRejectBadSchema},
      {"codec-spawn", "reject-count-65", SpawnRejectCount65},
      {"codec-spawn", "reject-oversize-string", SpawnRejectOversizeString},
      {"codec-spawn", "reject-total-size", SpawnRejectTotalSizeMismatch},
      {"codec-spawn", "reject-trailing", SpawnRejectTrailing},
      {"codec-spawn", "reject-over-64kib", SpawnRejectOver64KiB},
      {"codec-spawn", "reject-bad-bool", SpawnRejectBadBool},
      {"codec-spawn", "reject-cross-field", SpawnRejectCrossField},
      {"codec-spawn", "reject-non-bare-engine", SpawnRejectNonBareEngine},
      {"codec-spawn", "reject-bad-utf8", SpawnRejectBadUtf8},
      {"codec-spawn", "forward-compat", SpawnForwardCompat},
      {"apply", "lowers-every-field", ApplyLowersEveryField},
      {"apply", "trusted-consistent", ApplyTrustedConsistent},
      {"apply", "rejects-trusted-under-acg", ApplyRejectsTrustedUnderAcg},
      {"apply", "rejects-jitless-without-acg", ApplyRejectsJitlessWithoutAcg},
      {"apply", "fails-closed-on-bad-setter", ApplyFailsClosedOnBadSetter},
      {"apply", "null-args", ApplyNullArgs},
      {"apply", "lowers-non-default-tokens", ApplyLowersNonDefaultTokens},
      {"codec-worker-v2", "canonical-and-bounds", WorkerV2CanonicalAndBounds},
      {"codec-worker-v2", "decode-common-profile", WorkerDecodeCommonProfile},
      {"codec-spawn", "explicit-worker-schema2", SpawnExplicitWorkerSchema2},
      {"apply", "explicit-worker-profile-v2", ApplyExplicitWorkerProfileV2},
  };
  return v8host::test::RunTests(argc, argv, tests);
}
