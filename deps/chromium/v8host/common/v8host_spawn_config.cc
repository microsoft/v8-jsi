// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "v8host_spawn_config.h"

// The two spawn-time encodings. Both reuse the Contract B framing primitives
// (v8host::protocol::Writer/Reader) so every multi-byte integer is little-endian
// byte-by-byte and no raw bytes are cast onto a native struct. Exception-free:
// each decoder reports success through its return value and mutates no output on
// failure. The discipline mirrors the CREATE_SESSION / START_RUN codecs:
// {schema_version==1, fixed_size, total_size} header, forward-compat Skip to the
// variable section, strict UTF-8 / no-NUL strings, bounded counts, and exact
// consumption.

namespace v8host {

using protocol::Reader;
using protocol::Writer;

namespace {

// Decodes a wire boolean: exactly 0 or 1, anything else is malformed.
bool DecodeBool(uint32_t value, bool* out) {
  if (value > 1)
    return false;
  *out = (value != 0);
  return true;
}

// Reads the {schema_version==1, fixed_size, total_size} header shared by both
// encodings and enforces the framing invariants: total_size must equal the whole
// handed blob and stay within `max_size`, and fixed_size must cover the v1 known
// section yet fit inside total_size. On success the reader sits just past
// total_size.
bool ReadHeader(Reader& reader,
                size_t size,
                uint16_t schema_version,
                uint16_t known_fixed_size,
                uint32_t max_size,
                uint16_t* fixed_size) {
  uint16_t schema = 0;
  uint32_t total_size = 0;
  if (!reader.GetU16(&schema) || schema != schema_version)
    return false;
  if (!reader.GetU16(fixed_size))
    return false;
  if (!reader.GetU32(&total_size))
    return false;
  if (total_size != size)
    return false;  // declared length must match the actual blob exactly
  if (total_size > max_size)
    return false;  // per-encoding ceiling
  if (*fixed_size < known_fixed_size || *fixed_size > total_size)
    return false;
  return true;
}

// True iff `name` is a bare payload-directory filename: non-empty, not ".", no
// path separator or drive colon, and no ".." substring. Matches the
// sbox_config.cc allow_engine_dll setter exactly, so the codec is never the
// weaker validator of a directly-crafted worker/spawn blob.
bool IsBareFilename(const std::string& name) {
  if (name.empty() || name == "." || name.find("..") != std::string::npos)
    return false;
  for (const char c : name) {
    if (c == '\\' || c == '/' || c == ':')
      return false;
  }
  return true;
}

// Reads a u32 count (<= max) and that many payload-relative path strings.
bool ReadRelativePaths(Reader& reader, uint32_t max, std::vector<std::string>* out) {
  uint32_t count = 0;
  if (!reader.GetU32(&count) || count > max)
    return false;
  out->resize(count);
  for (std::string& path : *out) {
    if (!reader.GetString(&path) || !IsPayloadRelativePath(path))
      return false;
  }
  return true;
}

}  // namespace

bool IsPayloadRelativePath(const std::string& path) {
  if (path.empty() || path.front() == '/' || path.front() == '\\' ||
      path.find(':') != std::string::npos)
    return false;
  size_t start = 0;
  for (;;) {
    const size_t end = path.find_first_of("/\\", start);
    const std::string part =
        path.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (part.empty() || part == "." || part == ".." || part.back() == '.' ||
        part.back() == ' ')
      return false;
    if (end == std::string::npos)
      return true;
    start = end + 1;
  }
}

// ---------------------------------------------------------------------------
// V8HostWorkerProfileV1
// ---------------------------------------------------------------------------
std::vector<uint8_t> V8HostWorkerProfileV1::Encode() const {
  Writer var;
  var.PutString(engine_dll);
  var.PutString(snapshot_path);

  const uint32_t total_size =
      kWorkerProfileFixedSize + static_cast<uint32_t>(var.size());

  Writer writer;
  writer.PutU16(kWorkerProfileSchemaVersion);
  writer.PutU16(kWorkerProfileFixedSize);
  writer.PutU32(total_size);
  writer.PutU32(jitless ? 1u : 0u);
  writer.PutBytes(var.buffer());
  return writer.buffer();
}

bool V8HostWorkerProfileV1::Decode(const uint8_t* data,
                                   size_t size,
                                   V8HostWorkerProfileV1* out) {
  Reader reader(data, size);
  V8HostWorkerProfileV1 p;

  uint16_t fixed_size = 0;
  if (!ReadHeader(reader, size, kWorkerProfileSchemaVersion,
                  kWorkerProfileFixedSize, kWorkerProfileMaxSize, &fixed_size))
    return false;

  uint32_t jitless_raw = 0;
  if (!reader.GetU32(&jitless_raw))
    return false;
  bool jitless_bool = false;
  if (!DecodeBool(jitless_raw, &jitless_bool))
    return false;
  p.jitless = jitless_bool ? 1u : 0u;

  // Forward-compat: skip any additive fixed fields a future minor version
  // appended, so the variable section starts exactly at fixed_size.
  if (!reader.Skip(fixed_size - kWorkerProfileFixedSize))
    return false;

  if (!reader.GetString(&p.engine_dll))
    return false;
  if (!reader.GetString(&p.snapshot_path))
    return false;

  // The engine DLL is a bare app-dir filename (never a path); the snapshot path
  // may be empty (no snapshot) or a payload-relative path the worker opens.
  if (!IsBareFilename(p.engine_dll))
    return false;

  if (!reader.AtEnd())
    return false;  // the body must be consumed exactly (ends at total_size)
  *out = std::move(p);
  return true;
}

// ---------------------------------------------------------------------------
// V8HostWorkerProfileV2
// ---------------------------------------------------------------------------
std::vector<uint8_t> V8HostWorkerProfileV2::Encode() const {
  Writer var;
  var.PutString(engine_dll);
  var.PutU32(static_cast<uint32_t>(snapshot_paths.size()));
  for (const std::string& path : snapshot_paths)
    var.PutString(path);

  Writer writer;
  writer.PutU16(kWorkerProfileV2SchemaVersion);
  writer.PutU16(kWorkerProfileV2FixedSize);
  writer.PutU32(kWorkerProfileV2FixedSize + static_cast<uint32_t>(var.size()));
  writer.PutU32(jitless ? 1u : 0u);
  writer.PutU32(max_runtimes);
  writer.PutBytes(var.buffer());
  return writer.buffer();
}

bool V8HostWorkerProfileV2::Decode(const uint8_t* data,
                                   size_t size,
                                   V8HostWorkerProfileV2* out) {
  Reader reader(data, size);
  V8HostWorkerProfileV2 p;
  uint16_t fixed_size = 0;
  if (!ReadHeader(reader, size, kWorkerProfileV2SchemaVersion,
                  kWorkerProfileV2FixedSize, kWorkerProfileMaxSize, &fixed_size))
    return false;
  uint32_t jitless_raw = 0;
  bool jitless_bool = false;
  if (!reader.GetU32(&jitless_raw) || !DecodeBool(jitless_raw, &jitless_bool))
    return false;
  p.jitless = jitless_bool ? 1u : 0u;
  if (!reader.GetU32(&p.max_runtimes) || p.max_runtimes == 0 ||
      p.max_runtimes > kWorkerProfileMaxRuntimes)
    return false;
  if (!reader.Skip(fixed_size - kWorkerProfileV2FixedSize))
    return false;
  if (!reader.GetString(&p.engine_dll) || !IsBareFilename(p.engine_dll))
    return false;
  if (!ReadRelativePaths(reader, kWorkerProfileMaxSnapshots, &p.snapshot_paths))
    return false;
  if (!reader.AtEnd())
    return false;
  *out = std::move(p);
  return true;
}

bool DecodeWorkerProfile(const uint8_t* data, size_t size, V8HostWorkerProfile* out) {
  if (data == nullptr || size < sizeof(uint16_t))
    return false;
  const uint16_t schema = static_cast<uint16_t>(data[0] | (data[1] << 8));
  V8HostWorkerProfile profile;
  if (schema == kWorkerProfileSchemaVersion) {
    V8HostWorkerProfileV1 v1;
    if (!V8HostWorkerProfileV1::Decode(data, size, &v1))
      return false;
    profile.jitless = v1.jitless != 0;
    profile.engine_dll = std::move(v1.engine_dll);
    if (!v1.snapshot_path.empty())
      profile.snapshot_paths.push_back(std::move(v1.snapshot_path));
  } else if (schema == kWorkerProfileV2SchemaVersion) {
    V8HostWorkerProfileV2 v2;
    if (!V8HostWorkerProfileV2::Decode(data, size, &v2))
      return false;
    profile.jitless = v2.jitless != 0;
    profile.max_runtimes = v2.max_runtimes;
    profile.engine_dll = std::move(v2.engine_dll);
    profile.snapshot_paths = std::move(v2.snapshot_paths);
    profile.snapshots_relative = true;
    profile.precreate = false;
  } else {
    return false;
  }
  *out = std::move(profile);
  return true;
}

// ---------------------------------------------------------------------------
// V8HostSpawnConfigV1 (schema 1 for sessions, schema 2 for explicit workers)
// ---------------------------------------------------------------------------
std::vector<uint8_t> V8HostSpawnConfigV1::Encode() const {
  // Encode the variable section first so total_size is known before the fixed
  // header is stamped. Encode trusts the caller's struct; the decoder is the
  // enforcement point for counts, booleans, strings, and cross-field rules.
  const bool explicit_worker = max_runtimes != 0;
  Writer var;
  var.PutString(app_container_profile);
  for (const V8HostSpawnFileRule& rule : file_rules) {
    var.PutU32(rule.readonly ? 1u : 0u);
    var.PutString(rule.pattern);
  }
  for (const std::string& capability : capabilities)
    var.PutString(capability);
  var.PutString(engine_dll);
  var.PutString(snapshot_path);
  if (explicit_worker) {
    var.PutU32(static_cast<uint32_t>(snapshot_paths.size()));
    for (const std::string& path : snapshot_paths)
      var.PutString(path);
  }

  const uint16_t fixed_size =
      explicit_worker ? kSpawnConfigV2FixedSize : kSpawnConfigFixedSize;
  const uint32_t total_size = fixed_size + static_cast<uint32_t>(var.size());

  Writer writer;
  writer.PutU16(explicit_worker ? kSpawnConfigV2SchemaVersion
                                : kSpawnConfigSchemaVersion);
  writer.PutU16(fixed_size);
  writer.PutU32(total_size);
  writer.PutU32(static_cast<uint32_t>(tier));
  writer.PutU32(static_cast<uint32_t>(integrity));
  writer.PutU32(static_cast<uint32_t>(delayed_integrity));
  writer.PutU32(static_cast<uint32_t>(initial_token));
  writer.PutU32(static_cast<uint32_t>(lockdown_token));
  writer.PutU32(prohibit_dynamic_code ? 1u : 0u);
  writer.PutU32(use_app_container ? 1u : 0u);
  writer.PutU32(low_privilege_app_container ? 1u : 0u);
  writer.PutU32(static_cast<uint32_t>(effective_tier));
  writer.PutU32(static_cast<uint32_t>(file_rules.size()));
  writer.PutU32(static_cast<uint32_t>(capabilities.size()));
  if (explicit_worker)
    writer.PutU32(max_runtimes);
  writer.PutBytes(var.buffer());
  return writer.buffer();
}

bool V8HostSpawnConfigV1::Decode(const uint8_t* data,
                                 size_t size,
                                 V8HostSpawnConfigV1* out) {
  Reader reader(data, size);
  V8HostSpawnConfigV1 p;

  if (data == nullptr || size < sizeof(uint16_t))
    return false;
  const bool explicit_worker =
      static_cast<uint16_t>(data[0] | (data[1] << 8)) == kSpawnConfigV2SchemaVersion;
  const uint16_t known_fixed_size =
      explicit_worker ? kSpawnConfigV2FixedSize : kSpawnConfigFixedSize;
  uint16_t fixed_size = 0;
  if (!ReadHeader(reader, size,
                  explicit_worker ? kSpawnConfigV2SchemaVersion
                                  : kSpawnConfigSchemaVersion,
                  known_fixed_size, kSpawnConfigMaxSize, &fixed_size))
    return false;

  // Enum/token fields are transported as-is; the config builder validates them
  // against its allowlists (and poisons itself on a bad value).
  uint32_t scalars[5] = {};
  for (uint32_t& field : scalars) {
    if (!reader.GetU32(&field))
      return false;
  }
  p.tier = static_cast<int32_t>(scalars[0]);
  p.integrity = static_cast<int32_t>(scalars[1]);
  p.delayed_integrity = static_cast<int32_t>(scalars[2]);
  p.initial_token = static_cast<int32_t>(scalars[3]);
  p.lockdown_token = static_cast<int32_t>(scalars[4]);

  uint32_t prohibit = 0, use_ac = 0, lpac = 0, effective_tier = 0,
           file_rule_count = 0, capability_count = 0;
  if (!reader.GetU32(&prohibit) || !reader.GetU32(&use_ac) ||
      !reader.GetU32(&lpac) || !reader.GetU32(&effective_tier) ||
      !reader.GetU32(&file_rule_count) || !reader.GetU32(&capability_count))
    return false;
  if (!DecodeBool(prohibit, &p.prohibit_dynamic_code) ||
      !DecodeBool(use_ac, &p.use_app_container) ||
      !DecodeBool(lpac, &p.low_privilege_app_container))
    return false;
  p.effective_tier = static_cast<int32_t>(effective_tier);
  // Bound the counts before any length-keyed allocation.
  if (file_rule_count > protocol::kMaxFileRules ||
      capability_count > protocol::kMaxCapabilities)
    return false;
  if (explicit_worker &&
      (!reader.GetU32(&p.max_runtimes) || p.max_runtimes == 0 ||
       p.max_runtimes > kWorkerProfileMaxRuntimes))
    return false;

  // Forward-compat: skip any additive fixed fields to reach the variable
  // section, which starts at fixed_size.
  if (!reader.Skip(fixed_size - known_fixed_size))
    return false;

  if (!reader.GetString(&p.app_container_profile))
    return false;
  p.file_rules.resize(file_rule_count);
  for (V8HostSpawnFileRule& rule : p.file_rules) {
    uint32_t readonly = 0;
    if (!reader.GetU32(&readonly) || !DecodeBool(readonly, &rule.readonly))
      return false;
    if (!reader.GetString(&rule.pattern))
      return false;
  }
  p.capabilities.resize(capability_count);
  for (std::string& capability : p.capabilities) {
    if (!reader.GetString(&capability))
      return false;
  }
  if (!reader.GetString(&p.engine_dll))
    return false;
  if (!reader.GetString(&p.snapshot_path))
    return false;
  // An explicit worker names its snapshots in its own list, never in the
  // session's single snapshot_path.
  if (explicit_worker &&
      (!p.snapshot_path.empty() ||
       !ReadRelativePaths(reader, kWorkerProfileMaxSnapshots, &p.snapshot_paths)))
    return false;

  // Cross-field policy rules (design §8.5), fail-closed.
  if (p.app_container_profile.empty() && p.use_app_container)
    return false;  // an empty profile is legal only when AppContainer is off
  if (!p.capabilities.empty() && !p.use_app_container)
    return false;  // capabilities are legal only when AppContainer is on
  if (p.low_privilege_app_container && !p.use_app_container)
    return false;  // LPAC implies AppContainer
  // The effective engine is a bare app-dir filename (never a path); the snapshot
  // may be empty (no snapshot) or a payload-relative path the worker opens.
  if (!IsBareFilename(p.engine_dll))
    return false;

  if (!reader.AtEnd())
    return false;  // the body must be consumed exactly (ends at total_size)
  *out = std::move(p);
  return true;
}

}  // namespace v8host
