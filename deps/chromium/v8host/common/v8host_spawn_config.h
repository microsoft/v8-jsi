// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Spawn/worker-profile codec — the two bounded little-endian encodings that the
// coordinator and the engine/configure code agree on when a session is spawned:
//
//   * V8HostWorkerProfileV1 — the worker-facing blob carried broker->worker in
//     the opaque sbox plugin_data (<= 4 KiB). It replaces the old native,
//     fixed-size RunProfile struct. warmup() decodes it to pick jitless, the
//     engine DLL, and the optional startup snapshot.
//   * V8HostSpawnConfigV1 — the configure_data the coordinator hands to the
//     plugin's configure() (<= 64 KiB). It carries the validated session
//     security envelope plus the bound effective worker profile; configure()
//     drives every applicable sbox_config_api setter from it.
//
// This codec is NEUTRAL: it owns no pipe, process, token, or policy authority
// and references no client/coordinator/engine symbol. Like the Contract B wire
// (v8host_protocol.h), every field crosses as explicit little-endian fixed-width
// bytes via that header's Writer/Reader, so the bytes are identical regardless
// of host arch and no raw bytes are ever cast onto a native struct. The structs
// below are the DECODED form only. This surface is EXPERIMENTAL and NOT yet
// ABI-stable; it may be reshaped between revisions.

#ifndef V8HOST_COMMON_V8HOST_SPAWN_CONFIG_H_
#define V8HOST_COMMON_V8HOST_SPAWN_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "v8host_protocol.h"  // Writer/Reader, kMaxFileRules/kMaxCapabilities

namespace v8host {

// Security tier values. These match the Contract C V8HOST_TIER_* numbering
// (src/public/v8host_client.h) and the dev tier the engine derives from the
// SBOX_TIER env var, kept here so the neutral codec and the engine share one
// source without the engine depending on the client header. The codec only
// transports `tier`/`effective_tier` as-is; the engine maps `effective_tier` to
// the jitless/ACG posture (Untrusted => jitless + ACG; Trusted => JIT, ACG off).
inline constexpr int32_t kTierUntrusted = 0;  // jitless + ACG (default)
inline constexpr int32_t kTierTrusted = 1;    // JIT, ACG off

// ---------------------------------------------------------------------------
// V8HostWorkerProfileV1 — the broker->worker plugin_data blob (<= 4 KiB).
//
//   offset size field
//   0      2    schema_version (u16 LE, == 1)
//   2      2    fixed_size     (u16 LE, == 12 for v1)
//   4      4    total_size     (u32 LE, == whole blob size)
//   8      4    jitless        (u32 LE, 0 or 1)
//   12     ..   engine_dll     (string: u32 byte_length | strict UTF-8 bytes)
//   ..     ..   snapshot_path  (string: u32 byte_length | strict UTF-8 bytes)
//
// `fixed_size` lets a future minor version append bounded trailing fixed fields
// that a v1 reader skips; `total_size` must equal the whole blob and is consumed
// exactly. engine_dll is a bare app-dir filename (non-empty, no separator/colon,
// not "."/".."). snapshot_path may be empty (no snapshot).
// ---------------------------------------------------------------------------
inline constexpr uint16_t kWorkerProfileSchemaVersion = 1;
inline constexpr uint16_t kWorkerProfileFixedSize = 12;
inline constexpr uint32_t kWorkerProfileMaxSize = 4096;  // == kSboxMaxPluginData

struct V8HostWorkerProfileV1 {
  uint32_t jitless = 0;       // 1 = jitless (+ ACG); 0 = JIT (Trusted)
  std::string engine_dll;     // bare app-dir filename (UTF-8), non-empty
  std::string snapshot_path;  // optional startup snapshot (UTF-8), empty = none

  // Encode to the bounded LE blob above. Trusts the caller's fields; the decoder
  // is the enforcement point. The result is <= kWorkerProfileMaxSize for any
  // reasonable filename/path (the caller/setter enforces that ceiling).
  std::vector<uint8_t> Encode() const;

  // Decode [data, data+size). Returns false (leaving *out untouched) on any
  // malformed, oversized (> kWorkerProfileMaxSize), short, or trailing input, a
  // bad schema_version/fixed_size/total_size, invalid UTF-8/embedded NUL, or an
  // engine_dll that is not a bare filename.
  static bool Decode(const uint8_t* data, size_t size, V8HostWorkerProfileV1* out);
};

// ---------------------------------------------------------------------------
// V8HostWorkerProfileV2 — an explicit worker's plugin_data blob (<= 4 KiB).
//
//   offset size field
//   0      2    schema_version (u16 LE, == 2)
//   2      2    fixed_size     (u16 LE, == 16 for v2)
//   4      4    total_size     (u32 LE, == whole blob size)
//   8      4    jitless        (u32 LE, 0 or 1)
//   12     4    max_runtimes   (u32 LE, 1..16)
//   16     ..   engine_dll     (string: bare filename)
//   ..     4    snapshot_count (u32 LE, 0..8)
//   ..     ..   snapshot_path[snapshot_count] (string: payload-relative)
//
// Snapshot paths stay relative to the payload directory; the engine joins them
// to it, as it does engine_dll. Bounds and framing match V1.
// ---------------------------------------------------------------------------
inline constexpr uint16_t kWorkerProfileV2SchemaVersion = 2;
inline constexpr uint16_t kWorkerProfileV2FixedSize = 16;
inline constexpr uint32_t kWorkerProfileMaxRuntimes = 16;
inline constexpr uint32_t kWorkerProfileMaxSnapshots = 8;

struct V8HostWorkerProfileV2 {
  uint32_t jitless = 0;
  uint32_t max_runtimes = 1;
  std::string engine_dll;
  std::vector<std::string> snapshot_paths;

  std::vector<uint8_t> Encode() const;
  static bool Decode(const uint8_t* data, size_t size, V8HostWorkerProfileV2* out);
};

// The engine's single view of either profile schema. V1 decodes to one runtime
// that warmup precreates, with zero or one absolute snapshot path; V2 to its
// own runtime cap and payload-relative snapshots, with no runtime at lockdown.
struct V8HostWorkerProfile {
  bool jitless = true;
  uint32_t max_runtimes = 1;
  std::string engine_dll;
  std::vector<std::string> snapshot_paths;
  bool snapshots_relative = false;
  bool precreate = true;
};

// Decode a V1 or V2 blob (selected by schema_version) into the common profile.
bool DecodeWorkerProfile(const uint8_t* data, size_t size, V8HostWorkerProfile* out);

// True iff `path` is a non-empty payload-relative path: no drive, colon, or
// leading separator, and no empty, ".", "..", or dot/space-terminated part.
bool IsPayloadRelativePath(const std::string& path);

// One file-access rule in the spawn config: a read-only flag plus an opaque
// path/pattern string (patterns may contain separators/colons; the config
// builder normalizes them, so the codec only enforces strict UTF-8 / no NUL).
struct V8HostSpawnFileRule {
  bool readonly = false;
  std::string pattern;
};

// ---------------------------------------------------------------------------
// V8HostSpawnConfigV1 — the configure_data (<= 64 KiB): the validated session
// security envelope plus the bound effective worker profile.
//
//   offset size field
//   0      2    schema_version              (u16 LE, == 1)
//   2      2    fixed_size                  (u16 LE, == 52 for v1)
//   4      4    total_size                  (u32 LE, == whole payload size)
//   8      4    tier                        (i32 LE)
//   12     4    integrity                   (i32 LE)
//   16     4    delayed_integrity           (i32 LE)
//   20     4    initial_token               (i32 LE)
//   24     4    lockdown_token              (i32 LE)
//   28     4    prohibit_dynamic_code       (u32 LE, 0 or 1 — ACG)
//   32     4    use_app_container           (u32 LE, 0 or 1)
//   36     4    low_privilege_app_container (u32 LE, 0 or 1)
//   40     4    effective_tier              (i32 LE)
//   44     4    file_rule_count             (u32 LE, <= 64)
//   48     4    capability_count            (u32 LE, <= 64)
//   52     ..   app_container_profile       (string)
//   ..     ..   file_rule[file_rule_count]  (u32 readonly | string pattern)
//   ..     ..   capability[capability_count](string)
//   ..     ..   engine_dll                  (string: effective engine, bare)
//   ..     ..   snapshot_path               (string: effective snapshot, may be empty)
//
//   string := u32 byte_length | strict UTF-8 bytes (no terminator, no NUL)
//
// Enum/token fields are transported as-is (not range-validated here); the config
// builder validates them against its allowlists. Booleans are exactly 0 or 1.
// Cross-field rules (fail-closed on decode): an empty AppContainer profile is
// legal only when AppContainer is off; capabilities are legal only when it is
// on; LPAC implies AppContainer. engine_dll is a bare payload-dir filename;
// snapshot_path may be empty. `fixed_size`/`total_size` discipline mirrors
// V8HostWorkerProfileV1.
//
// Schema 2 (an explicit worker) is the same layout with fixed_size 56: a u32
// max_runtimes (1..16) at offset 52, an empty snapshot_path, and then
// snapshot_count (u32, <= 8) payload-relative snapshot paths after it. A
// session always encodes schema 1, so its bytes don't change.
// ---------------------------------------------------------------------------
inline constexpr uint16_t kSpawnConfigSchemaVersion = 1;
inline constexpr uint16_t kSpawnConfigFixedSize = 52;
inline constexpr uint16_t kSpawnConfigV2SchemaVersion = 2;
inline constexpr uint16_t kSpawnConfigV2FixedSize = 56;
inline constexpr uint32_t kSpawnConfigMaxSize = 64 * 1024;  // configure_data cap

struct V8HostSpawnConfigV1 {
  // Session security envelope.
  int32_t tier = 0;
  int32_t integrity = 0;
  int32_t delayed_integrity = 0;
  int32_t initial_token = 0;
  int32_t lockdown_token = 0;
  bool prohibit_dynamic_code = false;  // ACG
  bool use_app_container = false;
  bool low_privilege_app_container = false;
  std::string app_container_profile;
  std::vector<V8HostSpawnFileRule> file_rules;
  std::vector<std::string> capabilities;
  // Bound effective worker profile.
  int32_t effective_tier = 0;
  std::string engine_dll;     // bare app-dir filename (UTF-8)
  std::string snapshot_path;  // optional (UTF-8), empty = none
  // Explicit workers only (schema 2): 1..16 selects V8HostWorkerProfileV2; 0
  // is a session (schema 1, V8HostWorkerProfileV1).
  uint32_t max_runtimes = 0;
  std::vector<std::string> snapshot_paths;  // payload-relative

  // Encode to the bounded LE payload above. Trusts the caller's fields.
  std::vector<uint8_t> Encode() const;

  // Decode [data, data+size). Returns false (leaving *out untouched) on any
  // malformed, oversized (> kSpawnConfigMaxSize), short, or trailing input, a
  // bad schema/fixed_size/total_size, a count above 64, invalid UTF-8/NUL, a
  // non-bare engine_dll, or a violated cross-field rule.
  static bool Decode(const uint8_t* data, size_t size, V8HostSpawnConfigV1* out);
};

}  // namespace v8host

#endif  // V8HOST_COMMON_V8HOST_SPAWN_CONFIG_H_
