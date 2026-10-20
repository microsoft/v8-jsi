// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "v8host_protocol_messages.h"

#include <algorithm>
#include <cstring>

// Contract B message layer. Every multi-byte integer is read and written
// through the framing core's Writer/Reader, so the bytes are identical on any
// host arch. Exception-free: no throw/try — codecs report success through their
// return value and the cache never throws.

namespace v8host::protocol {

namespace {

// ---------------------------------------------------------------------------
// Self-contained SHA-256 (FIPS 180-4), file-local so //v8host:protocol pulls in
// no bcrypt/Windows dependency. This is an independent implementation from the
// test's SHA-256 (which stays a separate oracle); both compute the standard
// digest, so the cache fingerprint is a plain SHA-256 of the request payload.
// ---------------------------------------------------------------------------
class Sha256 {
 public:
  Sha256() {
    h_[0] = 0x6a09e667u;
    h_[1] = 0xbb67ae85u;
    h_[2] = 0x3c6ef372u;
    h_[3] = 0xa54ff53au;
    h_[4] = 0x510e527fu;
    h_[5] = 0x9b05688cu;
    h_[6] = 0x1f83d9abu;
    h_[7] = 0x5be0cd19u;
  }

  void Update(const uint8_t* data, size_t len) {
    total_len_ += len;
    for (size_t i = 0; i < len; ++i) {
      block_[fill_++] = data[i];
      if (fill_ == 64) {
        Compress();
        fill_ = 0;
      }
    }
  }

  void Finish(uint8_t out[32]) {
    const uint64_t bit_len = static_cast<uint64_t>(total_len_) * 8;
    // Append 0x80 then zero-pad to a 56-byte residue, then the 64-bit length.
    block_[fill_++] = 0x80;
    if (fill_ > 56) {
      while (fill_ < 64)
        block_[fill_++] = 0;
      Compress();
      fill_ = 0;
    }
    while (fill_ < 56)
      block_[fill_++] = 0;
    for (int i = 7; i >= 0; --i)
      block_[fill_++] = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF);
    Compress();
    for (int i = 0; i < 8; ++i) {
      out[i * 4 + 0] = static_cast<uint8_t>((h_[i] >> 24) & 0xFF);
      out[i * 4 + 1] = static_cast<uint8_t>((h_[i] >> 16) & 0xFF);
      out[i * 4 + 2] = static_cast<uint8_t>((h_[i] >> 8) & 0xFF);
      out[i * 4 + 3] = static_cast<uint8_t>(h_[i] & 0xFF);
    }
  }

 private:
  static uint32_t Rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
  }

  void Compress() {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
        0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
        0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
        0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
        0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
        0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
        0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
        0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(block_[i * 4 + 0]) << 24) |
             (static_cast<uint32_t>(block_[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block_[i * 4 + 2]) << 8) |
             (static_cast<uint32_t>(block_[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 =
          Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 =
          Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      const uint32_t ch = (e & f) ^ ((~e) & g);
      const uint32_t t1 = h + s1 + ch + k[i] + w[i];
      const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
  }

  uint32_t h_[8];
  uint8_t block_[64] = {};
  size_t fill_ = 0;
  uint64_t total_len_ = 0;
};

void Sha256Fingerprint(const uint8_t* data, size_t len, uint8_t out[32]) {
  Sha256 sha;
  if (len != 0)
    sha.Update(data, len);
  sha.Finish(out);
}

// Reads and validates the common {schema_version==1, reserved==0} prologue that
// opens every version-1 control payload. Returns false (reader unchanged on the
// failing accessor) on a short read or a disallowed value.
bool ReadSchemaPrologue(Reader& reader, uint16_t* schema, uint16_t* reserved) {
  if (!reader.GetU16(schema) || *schema != kMessageSchemaVersion)
    return false;
  if (!reader.GetU16(reserved) || *reserved != 0)
    return false;
  return true;
}

// Decodes a wire boolean: exactly 0 or 1, anything else is malformed.
bool DecodeBool(uint32_t value, bool* out) {
  if (value > 1)
    return false;
  *out = (value != 0);
  return true;
}

// Reads the {schema_version==1, fixed_size, total_size} header shared by the
// CREATE_SESSION / START_RUN variable-length payloads and enforces the framing
// invariants: total_size must equal the whole handed payload and stay within the
// frame ceiling, and fixed_size must cover the v1 known section yet fit inside
// total_size. On success the reader is positioned just past total_size.
bool ReadVariablePayloadHeader(Reader& reader,
                               size_t size,
                               uint16_t known_fixed_size,
                               uint16_t* fixed_size) {
  uint16_t schema = 0;
  uint32_t total_size = 0;
  if (!reader.GetU16(&schema) || schema != kMessageSchemaVersion)
    return false;
  if (!reader.GetU16(fixed_size))
    return false;
  if (!reader.GetU32(&total_size))
    return false;
  if (total_size != size)
    return false;  // declared length must match the actual payload exactly
  if (total_size > kMaxFramePayload)
    return false;  // per-message ceiling (also the frame payload ceiling)
  if (*fixed_size < known_fixed_size || *fixed_size > total_size)
    return false;
  return true;
}

// True iff `name` is a bare payload-directory filename: non-empty, no path
// separator or drive colon, not ".", and with no ".." substring. Full
// canonicalization under the payload dir is the coordinator's job; this
// is only the wire-format guard that an override can never be an arbitrary path
// or traverse out of the payload dir. The predicate is uniform with the client
// and the spawn/config layers.
bool IsBareFilename(const std::string& name) {
  if (name.empty() || name == "." || name.find("..") != std::string::npos)
    return false;
  for (const char c : name) {
    if (c == '\\' || c == '/' || c == ':')
      return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Payload codecs.
// ---------------------------------------------------------------------------
void EncodeHelloPayload(const HelloPayload& payload, Writer& writer) {
  writer.PutU16(payload.schema_version);
  writer.PutU16(payload.reserved);
}

bool DecodeHelloPayload(const uint8_t* data, size_t size, HelloPayload* out) {
  Reader reader(data, size);
  HelloPayload p;
  if (!ReadSchemaPrologue(reader, &p.schema_version, &p.reserved))
    return false;
  if (!reader.AtEnd())
    return false;  // trailing bytes
  *out = p;
  return true;
}

void EncodeHelloAckPayload(const HelloAckPayload& payload, Writer& writer) {
  writer.PutU16(payload.schema_version);
  writer.PutU16(payload.reserved);
  writer.PutU16(payload.broker_version_major);
  writer.PutU16(payload.broker_version_minor);
  writer.PutU32(payload.endpoint_mode);
  writer.PutU32(payload.max_frame_payload);
  writer.PutU32(payload.max_sessions_per_connection);
  writer.PutU32(payload.max_in_flight_runs_per_session);
  writer.PutU32(payload.max_file_rules);
  writer.PutU32(payload.max_capabilities);
  writer.PutU32(payload.max_queued_relay_bytes_per_run);
  writer.PutU32(payload.max_queued_relay_bytes_per_connection);
}

bool DecodeHelloAckPayload(const uint8_t* data,
                           size_t size,
                           HelloAckPayload* out) {
  Reader reader(data, size);
  HelloAckPayload p;
  if (!ReadSchemaPrologue(reader, &p.schema_version, &p.reserved))
    return false;
  if (!reader.GetU16(&p.broker_version_major))
    return false;
  if (!reader.GetU16(&p.broker_version_minor))
    return false;
  if (!reader.GetU32(&p.endpoint_mode))
    return false;
  if (!reader.GetU32(&p.max_frame_payload))
    return false;
  if (!reader.GetU32(&p.max_sessions_per_connection))
    return false;
  if (!reader.GetU32(&p.max_in_flight_runs_per_session))
    return false;
  if (!reader.GetU32(&p.max_file_rules))
    return false;
  if (!reader.GetU32(&p.max_capabilities))
    return false;
  if (!reader.GetU32(&p.max_queued_relay_bytes_per_run))
    return false;
  if (!reader.GetU32(&p.max_queued_relay_bytes_per_connection))
    return false;
  if (!reader.AtEnd())
    return false;
  *out = p;
  return true;
}

void EncodeAckPayload(const AckPayload& payload, Writer& writer) {
  writer.PutU16(payload.schema_version);
  writer.PutU16(payload.reserved);
}

bool DecodeAckPayload(const uint8_t* data, size_t size, AckPayload* out) {
  Reader reader(data, size);
  AckPayload p;
  if (!ReadSchemaPrologue(reader, &p.schema_version, &p.reserved))
    return false;
  if (!reader.AtEnd())
    return false;
  *out = p;
  return true;
}

void EncodeErrorPayload(const ErrorPayload& payload, Writer& writer) {
  writer.PutU16(payload.schema_version);
  writer.PutU16(payload.reserved);
  writer.PutU32(static_cast<uint32_t>(payload.status_code));
  writer.PutString(payload.message);
}

bool DecodeErrorPayload(const uint8_t* data, size_t size, ErrorPayload* out) {
  Reader reader(data, size);
  ErrorPayload p;
  if (!ReadSchemaPrologue(reader, &p.schema_version, &p.reserved))
    return false;
  uint32_t status = 0;
  if (!reader.GetU32(&status))
    return false;
  p.status_code = static_cast<StatusCode>(status);  // tolerate unknown codes
  if (!reader.GetString(&p.message))  // strict UTF-8 / no NUL / bounded
    return false;
  if (!reader.AtEnd())
    return false;
  *out = p;
  return true;
}

void EncodeResultPayload(const ResultPayload& payload, Writer& writer) {
  writer.PutU16(payload.schema_version);
  writer.PutU16(payload.reserved);
  writer.PutU32(static_cast<uint32_t>(payload.disposition));
}

bool DecodeResultPayload(const uint8_t* data, size_t size, ResultPayload* out) {
  Reader reader(data, size);
  ResultPayload p;
  if (!ReadSchemaPrologue(reader, &p.schema_version, &p.reserved))
    return false;
  uint32_t disposition = 0;
  if (!reader.GetU32(&disposition))
    return false;
  if (disposition != static_cast<uint32_t>(ResultDisposition::kCompleted) &&
      disposition != static_cast<uint32_t>(ResultDisposition::kCancelled))
    return false;  // fail-closed: unknown terminal disposition
  p.disposition = static_cast<ResultDisposition>(disposition);
  if (!reader.AtEnd())
    return false;
  *out = p;
  return true;
}

void EncodeCreateSessionPayload(const CreateSessionPayload& payload,
                                Writer& writer) {
  // Encode the variable section first so total_size is known before the fixed
  // header is stamped. (Encode trusts the caller's struct; the decoder is the
  // enforcement point for counts, booleans, strings, and cross-field rules.)
  Writer var;
  var.PutString(payload.app_container_profile);
  for (const FileRule& rule : payload.file_rules) {
    var.PutU32(rule.readonly ? 1u : 0u);
    var.PutString(rule.pattern);
  }
  for (const std::string& capability : payload.capabilities)
    var.PutString(capability);

  const uint32_t total_size =
      kCreateSessionFixedSize + static_cast<uint32_t>(var.size());

  writer.PutU16(payload.schema_version);
  writer.PutU16(kCreateSessionFixedSize);
  writer.PutU32(total_size);
  writer.PutU32(static_cast<uint32_t>(payload.broker_mode));
  writer.PutU32(static_cast<uint32_t>(payload.tier));
  writer.PutU32(static_cast<uint32_t>(payload.integrity));
  writer.PutU32(static_cast<uint32_t>(payload.delayed_integrity));
  writer.PutU32(static_cast<uint32_t>(payload.initial_token));
  writer.PutU32(static_cast<uint32_t>(payload.lockdown_token));
  writer.PutU32(payload.prohibit_dynamic_code ? 1u : 0u);
  writer.PutU32(payload.use_app_container ? 1u : 0u);
  writer.PutU32(payload.low_privilege_app_container ? 1u : 0u);
  writer.PutU32(static_cast<uint32_t>(payload.file_rules.size()));
  writer.PutU32(static_cast<uint32_t>(payload.capabilities.size()));
  writer.PutBytes(var.buffer());
}

bool DecodeCreateSessionPayload(const uint8_t* data,
                                size_t size,
                                CreateSessionPayload* out) {
  Reader reader(data, size);
  CreateSessionPayload p;

  uint16_t fixed_size = 0;
  if (!ReadVariablePayloadHeader(reader, size, kCreateSessionFixedSize,
                                 &fixed_size))
    return false;

  // The i32 enum/token fields are transported as-is; the coordinator validates
  // them against allowlists at bind time.
  uint32_t v[6] = {};
  for (uint32_t& field : v) {
    if (!reader.GetU32(&field))
      return false;
  }
  p.broker_mode = static_cast<int32_t>(v[0]);
  p.tier = static_cast<int32_t>(v[1]);
  p.integrity = static_cast<int32_t>(v[2]);
  p.delayed_integrity = static_cast<int32_t>(v[3]);
  p.initial_token = static_cast<int32_t>(v[4]);
  p.lockdown_token = static_cast<int32_t>(v[5]);

  uint32_t prohibit = 0, use_ac = 0, lpac = 0, file_rule_count = 0,
           capability_count = 0;
  if (!reader.GetU32(&prohibit) || !reader.GetU32(&use_ac) ||
      !reader.GetU32(&lpac) || !reader.GetU32(&file_rule_count) ||
      !reader.GetU32(&capability_count))
    return false;
  if (!DecodeBool(prohibit, &p.prohibit_dynamic_code) ||
      !DecodeBool(use_ac, &p.use_app_container) ||
      !DecodeBool(lpac, &p.low_privilege_app_container))
    return false;
  // Bound the counts before any length-keyed allocation.
  if (file_rule_count > kMaxFileRules || capability_count > kMaxCapabilities)
    return false;

  // Forward-compat: skip any additive fixed fields a future minor version
  // appended, so the variable section starts exactly at fixed_size. (The 52
  // v1 bytes have been consumed, so this skips fixed_size - 52 bytes.)
  if (!reader.Skip(fixed_size - kCreateSessionFixedSize))
    return false;

  if (!reader.GetString(&p.app_container_profile))
    return false;
  p.file_rules.resize(file_rule_count);
  for (FileRule& rule : p.file_rules) {
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

  // Cross-field policy rules (design §8.5), fail-closed.
  if (p.app_container_profile.empty() && p.use_app_container)
    return false;  // an empty profile is legal only when AppContainer is off
  if (!p.capabilities.empty() && !p.use_app_container)
    return false;  // capabilities are legal only when AppContainer is on
  if (p.low_privilege_app_container && !p.use_app_container)
    return false;  // LPAC implies AppContainer

  if (!reader.AtEnd())
    return false;  // the body must be consumed exactly (ends at total_size)
  *out = std::move(p);
  return true;
}

void EncodeCreateWorkerPayload(const CreateWorkerPayload& p, Writer& writer) {
  EncodeCreateSessionPayload(p.session, writer);
  writer.PutU32(p.max_runtimes);
  writer.PutU32(p.flags);
  writer.PutString(p.engine_dll);
  writer.PutU32(static_cast<uint32_t>(p.snapshot_paths.size()));
  for (const auto& path : p.snapshot_paths) writer.PutString(path);
}
bool DecodeCreateWorkerPayload(const uint8_t* data, size_t size, CreateWorkerPayload* out) {
  if (size > kMaxFramePayload) return false;
  Reader prefix(data, size);
  uint16_t schema = 0, fixed = 0;
  uint32_t session_size = 0;
  if (!prefix.GetU16(&schema) || !prefix.GetU16(&fixed) ||
      !prefix.GetU32(&session_size) || session_size > size) return false;
  CreateWorkerPayload p;
  if (!DecodeCreateSessionPayload(data, session_size, &p.session)) return false;
  Reader reader(data + session_size, size - session_size);
  uint32_t count = 0;
  if (!reader.GetU32(&p.max_runtimes) || !reader.GetU32(&p.flags) ||
      !reader.GetString(&p.engine_dll) || !reader.GetU32(&count) ||
      count > kMaxWorkerSnapshots) return false;
  for (uint32_t i = 0; i < count; ++i) {
    std::string path;
    if (!reader.GetString(&path)) return false;
    p.snapshot_paths.push_back(std::move(path));
  }
  if (!reader.AtEnd()) return false;
  *out = std::move(p);
  return true;
}
void EncodeCreateRuntimePayload(const CreateRuntimePayload& p, Writer& writer) {
  writer.PutU16(p.schema_version);
  writer.PutU16(p.reserved);
  writer.PutU32(p.worker_id);
  writer.PutU32(static_cast<uint32_t>(p.snapshot_index));
}
bool DecodeCreateRuntimePayload(const uint8_t* data, size_t size, CreateRuntimePayload* out) {
  Reader reader(data, size);
  CreateRuntimePayload p;
  uint32_t index = 0;
  if (!ReadSchemaPrologue(reader, &p.schema_version, &p.reserved) ||
      !reader.GetU32(&p.worker_id) || !reader.GetU32(&index) ||
      !reader.AtEnd()) return false;
  p.snapshot_index = static_cast<int32_t>(index);
  *out = p;
  return true;
}
bool ValidateCreateWorkerPayload(const CreateWorkerPayload& p) {
  if (!p.max_runtimes || p.max_runtimes > kMaxRuntimesPerWorker ||
      (p.flags & ~kWorkerFlagSingleUse) ||
      (!p.engine_dll.empty() && !IsBareFilename(p.engine_dll)) ||
      p.snapshot_paths.size() > kMaxWorkerSnapshots) return false;
  for (const auto& path : p.snapshot_paths) {
    if (path.empty() || path.front() == '/' || path.front() == '\\' ||
        path.find(':') != std::string::npos) return false;
    // Canonicalization at bind time also checks the resolved directory.
    size_t start = 0;
    while (start < path.size()) {
      const size_t end = path.find_first_of("/\\", start);
      const auto part = path.substr(start, end == std::string::npos ? end : end - start);
      if (part == ".." || part == "." || part.empty() || part.back() == '.' ||
          part.back() == ' ') return false;
      if (end == std::string::npos) break;
      start = end + 1;
      if (start == path.size()) return false;
    }
  }
  return true;
}
bool ValidateCreateRuntimePayload(const CreateRuntimePayload& p, uint32_t snapshot_count) {
  return p.worker_id != 0 && snapshot_count <= kMaxWorkerSnapshots &&
      (p.snapshot_index == -1 || (p.snapshot_index >= 0 &&
       static_cast<uint32_t>(p.snapshot_index) < snapshot_count));
}
void EncodeRuntimeClosedPayload(const RuntimeClosedPayload& p, Writer& writer) {
  writer.PutU16(p.schema_version);
  writer.PutU16(p.reserved);
  writer.PutU32(static_cast<uint32_t>(p.status_code));
}
bool DecodeRuntimeClosedPayload(const uint8_t* data, size_t size, RuntimeClosedPayload* out) {
  Reader reader(data, size);
  RuntimeClosedPayload p;
  uint32_t status = 0;
  if (!ReadSchemaPrologue(reader, &p.schema_version, &p.reserved) ||
      !reader.GetU32(&status) || status != static_cast<uint32_t>(StatusCode::ERROR_RUNTIME_FAILED) ||
      !reader.AtEnd()) return false;
  p.status_code = static_cast<StatusCode>(status);
  *out = p;
  return true;
}

void EncodeStartRunPayload(const StartRunPayload& payload, Writer& writer) {
  Writer var;
  var.PutString(payload.engine_filename);
  var.PutString(payload.snapshot_path);
  var.PutBytes(payload.guest_payload);

  const uint32_t total_size =
      kStartRunFixedSize + static_cast<uint32_t>(var.size());

  writer.PutU16(payload.schema_version);
  writer.PutU16(kStartRunFixedSize);
  writer.PutU32(total_size);
  writer.PutU32(static_cast<uint32_t>(payload.tier_override));
  writer.PutU32(payload.has_engine_override ? 1u : 0u);
  writer.PutU32(payload.has_snapshot ? 1u : 0u);
  writer.PutU32(static_cast<uint32_t>(payload.guest_payload.size()));
  writer.PutBytes(var.buffer());
}

bool DecodeStartRunPayload(const uint8_t* data,
                           size_t size,
                           StartRunPayload* out) {
  Reader reader(data, size);
  StartRunPayload p;

  uint16_t fixed_size = 0;
  if (!ReadVariablePayloadHeader(reader, size, kStartRunFixedSize, &fixed_size))
    return false;

  uint32_t tier_override = 0, has_engine = 0, has_snapshot = 0, guest_len = 0;
  if (!reader.GetU32(&tier_override) || !reader.GetU32(&has_engine) ||
      !reader.GetU32(&has_snapshot) || !reader.GetU32(&guest_len))
    return false;
  p.tier_override = static_cast<int32_t>(tier_override);
  if (!DecodeBool(has_engine, &p.has_engine_override) ||
      !DecodeBool(has_snapshot, &p.has_snapshot))
    return false;

  // Forward-compat: skip any additive fixed fields to reach the variable
  // section, which starts at fixed_size.
  if (!reader.Skip(fixed_size - kStartRunFixedSize))
    return false;

  if (!reader.GetString(&p.engine_filename))
    return false;
  if (!reader.GetString(&p.snapshot_path))
    return false;
  // guest_payload is opaque bytes; GetBytes bounds guest_len <= remaining, so
  // it always stays within total_size (and thus the frame ceiling).
  if (!reader.GetBytes(guest_len, &p.guest_payload))
    return false;

  // Structural field-format rules (design §8.6), fail-closed. Full
  // canonicalization under the payload directory, held-file signature
  // verification, and opening the snapshot are the coordinator's job —
  // this codec never touches the filesystem.
  if (p.has_engine_override) {
    if (!IsBareFilename(p.engine_filename))
      return false;  // an override must be a bare payload-dir filename
  } else if (!p.engine_filename.empty()) {
    return false;  // no override => the filename must be empty
  }
  if (p.has_snapshot) {
    if (p.snapshot_path.empty())
      return false;  // a declared snapshot must name a payload-relative path
  } else if (!p.snapshot_path.empty()) {
    return false;  // no snapshot => the path must be empty
  }

  if (!reader.AtEnd())
    return false;  // the body must be consumed exactly (ends at total_size)
  *out = std::move(p);
  return true;
}

// ---------------------------------------------------------------------------
// Full-frame builders.
// ---------------------------------------------------------------------------
namespace {

// Assembles header + payload: stamps the length, encodes the 32-byte header,
// then appends the already-encoded payload bytes.
std::vector<uint8_t> AssembleFrame(FrameHeader header,
                                   const std::vector<uint8_t>& payload) {
  header.payload_length = static_cast<uint32_t>(payload.size());
  std::vector<uint8_t> frame;
  EncodeHeader(header, frame);
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

}  // namespace

std::vector<uint8_t> BuildCreateWorkerFrame(FrameHeader h, const CreateWorkerPayload& p) {
  Writer writer;
  EncodeCreateWorkerPayload(p, writer);
  h.type = MessageType::CREATE_WORKER;
  return AssembleFrame(h, writer.buffer());
}
std::vector<uint8_t> BuildCreateRuntimeFrame(FrameHeader h, const CreateRuntimePayload& p) {
  Writer writer;
  EncodeCreateRuntimePayload(p, writer);
  h.type = MessageType::CREATE_RUNTIME;
  return AssembleFrame(h, writer.buffer());
}
std::vector<uint8_t> BuildRuntimeClosedFrame(FrameHeader h, const RuntimeClosedPayload& p) {
  Writer writer;
  EncodeRuntimeClosedPayload(p, writer);
  h.type = MessageType::RUNTIME_CLOSED;
  return AssembleFrame(h, writer.buffer());
}
std::vector<uint8_t> BuildCloseWorkerFrame(FrameHeader h) {
  h.type = MessageType::CLOSE_WORKER;
  return AssembleFrame(h, {});
}
std::vector<uint8_t> BuildCloseRuntimeFrame(FrameHeader h) {
  h.type = MessageType::CLOSE_RUNTIME;
  return AssembleFrame(h, {});
}
std::vector<uint8_t> BuildRuntimeReadyFrame(FrameHeader h) {
  h.type = MessageType::RUNTIME_READY;
  return AssembleFrame(h, {});
}

std::vector<uint8_t> BuildHelloFrame(FrameHeader header) {
  Writer writer;
  EncodeHelloPayload(HelloPayload{}, writer);
  header.type = MessageType::HELLO;
  return AssembleFrame(header, writer.buffer());
}

std::vector<uint8_t> BuildHelloAckFrame(FrameHeader header,
                                        const HelloAckPayload& payload) {
  Writer writer;
  EncodeHelloAckPayload(payload, writer);
  header.type = MessageType::HELLO_ACK;
  return AssembleFrame(header, writer.buffer());
}

std::vector<uint8_t> BuildAckFrame(FrameHeader header) {
  Writer writer;
  EncodeAckPayload(AckPayload{}, writer);
  header.type = MessageType::ACK;
  return AssembleFrame(header, writer.buffer());
}

std::vector<uint8_t> BuildErrorFrame(FrameHeader header,
                                     const ErrorPayload& payload) {
  Writer writer;
  EncodeErrorPayload(payload, writer);
  header.type = MessageType::ERROR;
  return AssembleFrame(header, writer.buffer());
}

std::vector<uint8_t> BuildCreateSessionFrame(
    FrameHeader header,
    const CreateSessionPayload& payload) {
  Writer writer;
  EncodeCreateSessionPayload(payload, writer);
  header.type = MessageType::CREATE_SESSION;
  return AssembleFrame(header, writer.buffer());
}

std::vector<uint8_t> BuildStartRunFrame(FrameHeader header,
                                        const StartRunPayload& payload) {
  Writer writer;
  EncodeStartRunPayload(payload, writer);
  header.type = MessageType::START_RUN;
  return AssembleFrame(header, writer.buffer());
}

std::vector<uint8_t> BuildCancelRunFrame(FrameHeader header) {
  header.type = MessageType::CANCEL_RUN;
  return AssembleFrame(header, {});
}

std::vector<uint8_t> BuildCloseSessionFrame(FrameHeader header) {
  header.type = MessageType::CLOSE_SESSION;
  return AssembleFrame(header, {});
}

std::vector<uint8_t> BuildSessionReadyFrame(FrameHeader header) {
  header.type = MessageType::SESSION_READY;
  return AssembleFrame(header, {});
}

void EncodeStartupReadyPayload(const StartupReadyPayload& p, Writer& writer) {
  writer.PutU16(p.schema_version);
  writer.PutU16(p.reserved);
  writer.PutU32(p.process_id);
}

bool DecodeStartupReadyPayload(const uint8_t* data, size_t size, StartupReadyPayload* out) {
  Reader reader(data, size);
  StartupReadyPayload p;
  if (!ReadSchemaPrologue(reader, &p.schema_version, &p.reserved) ||
      !reader.GetU32(&p.process_id) || !p.process_id || !reader.AtEnd())
    return false;
  *out = p;
  return true;
}

std::vector<uint8_t> BuildStartupReadyFrame(FrameHeader header, const StartupReadyPayload& p) {
  Writer writer;
  EncodeStartupReadyPayload(p, writer);
  header.type = MessageType::STARTUP_READY;
  return AssembleFrame(header, writer.buffer());
}

std::vector<uint8_t> BuildSecurityReadyFrame(FrameHeader header) {
  header.type = MessageType::SECURITY_READY;
  return AssembleFrame(header, {});
}

std::vector<uint8_t> BuildWorkerExitFrame(FrameHeader header) {
  header.type = MessageType::WORKER_EXIT;
  return AssembleFrame(header, {});
}

std::vector<uint8_t> BuildResultFrame(FrameHeader header,
                                      ResultDisposition disposition) {
  Writer writer;
  ResultPayload payload;
  payload.disposition = disposition;
  EncodeResultPayload(payload, writer);
  header.type = MessageType::RESULT;
  return AssembleFrame(header, writer.buffer());
}

std::vector<uint8_t> BuildRelayFrame(FrameHeader header,
                                     int32_t kind,
                                     const uint8_t* data,
                                     size_t len) {
  Writer writer;
  writer.PutU32(static_cast<uint32_t>(kind));
  if (len != 0)
    writer.PutBytes(data, len);
  return AssembleFrame(header, writer.buffer());
}

bool DecodeRelayPayload(const uint8_t* data,
                        size_t size,
                        int32_t* out_kind,
                        const uint8_t** out_body,
                        size_t* out_body_len) {
  if (data == nullptr || size < sizeof(uint32_t) || out_kind == nullptr ||
      out_body == nullptr || out_body_len == nullptr) {
    return false;
  }
  Reader reader(data, size);
  uint32_t kind = 0;
  if (!reader.GetU32(&kind))
    return false;
  *out_kind = static_cast<int32_t>(kind);
  *out_body = data + sizeof(uint32_t);
  *out_body_len = size - sizeof(uint32_t);
  return true;
}

// ---------------------------------------------------------------------------
// Version negotiation.
// ---------------------------------------------------------------------------
NegotiationResult NegotiateHello(const FrameHeader& hello_header,
                                 const HelloPayload& hello,
                                 const BrokerCapabilities& caps,
                                 uint32_t assigned_conn_id) {
  (void)hello;  // reserved for future HELLO capability bits; unused in v1

  NegotiationResult result;

  // The reply always addresses the assigned connection and echoes the HELLO
  // request_id; the broker speaks its own major in the header either way.
  FrameHeader reply;
  reply.version_major = caps.broker_version_major;
  reply.conn_id = assigned_conn_id;
  reply.request_id = hello_header.request_id;

  if (hello_header.version_major != caps.broker_version_major) {
    // Major mismatch: the broker cannot serve this client. Emit a bounded
    // ERROR(ERROR_VERSION); the caller closes the connection afterward.
    result.decision = NegotiationDecision::kReject;
    result.selected_minor = 0;
    reply.version_minor = caps.broker_version_minor;
    ErrorPayload error;
    error.status_code = StatusCode::ERROR_VERSION;
    error.message = "unsupported wire protocol major version";
    result.frame = BuildErrorFrame(reply, error);
    return result;
  }

  // Equal major: the lower minor governs. The selected minor is <= the client's
  // offer, so the client can always operate at it.
  result.decision = NegotiationDecision::kAck;
  result.selected_minor =
      std::min(hello_header.version_minor, caps.broker_version_minor);
  reply.version_minor = result.selected_minor;

  HelloAckPayload ack;
  ack.broker_version_major = caps.broker_version_major;
  ack.broker_version_minor = caps.broker_version_minor;
  ack.endpoint_mode = caps.endpoint_mode;
  ack.max_frame_payload = caps.max_frame_payload;
  ack.max_sessions_per_connection = caps.max_sessions_per_connection;
  ack.max_in_flight_runs_per_session = caps.max_in_flight_runs_per_session;
  ack.max_file_rules = caps.max_file_rules;
  ack.max_capabilities = caps.max_capabilities;
  ack.max_queued_relay_bytes_per_run = caps.max_queued_relay_bytes_per_run;
  ack.max_queued_relay_bytes_per_connection =
      caps.max_queued_relay_bytes_per_connection;
  result.frame = BuildHelloAckFrame(reply, ack);
  return result;
}

// ---------------------------------------------------------------------------
// Correlation + duplicate-request cache.
// ---------------------------------------------------------------------------
bool operator==(const RequestIdentity& a, const RequestIdentity& b) {
  return a.type == b.type && a.conn_id == b.conn_id &&
         a.target_id == b.target_id && a.run_id == b.run_id &&
         a.request_id == b.request_id && a.flags == b.flags;
}

ClassifyResult RequestCache::Classify(const RequestIdentity& identity,
                                      const uint8_t* payload,
                                      size_t payload_len) const {
  ClassifyResult result;
  const auto it = entries_.find(identity.request_id);
  if (it != entries_.end()) {
    // The request_id is cached. An exact duplicate must match both the full
    // identity and the payload fingerprint; anything else is malformed.
    uint8_t fingerprint[32];
    Sha256Fingerprint(payload, payload_len, fingerprint);
    if (it->second.identity == identity &&
        std::memcmp(it->second.fingerprint, fingerprint, 32) == 0) {
      result.classification = RequestClass::kExactDuplicate;
      result.cached_response = &it->second.response;
    } else {
      result.classification = RequestClass::kMismatchedDuplicate;
    }
    return result;
  }

  // Not cached: fresh above the watermark, stale at or below it (evicted or an
  // old/reused id). A cached id always has request_id <= watermark, so reaching
  // here with request_id > watermark is unambiguously fresh.
  result.classification = identity.request_id > watermark_
                              ? RequestClass::kFresh
                              : RequestClass::kStale;
  return result;
}

void RequestCache::Record(const RequestIdentity& identity,
                          const uint8_t* payload,
                          size_t payload_len,
                          std::vector<uint8_t> response_frame) {
  Entry entry;
  entry.identity = identity;
  Sha256Fingerprint(payload, payload_len, entry.fingerprint);
  entry.response = std::move(response_frame);
  entries_[identity.request_id] = std::move(entry);

  // Evict the oldest (lowest request_id) once over the 256-entry horizon.
  if (entries_.size() > kDuplicateCacheSize)
    entries_.erase(entries_.begin());

  watermark_ = std::max(watermark_, identity.request_id);
}

}  // namespace v8host::protocol
