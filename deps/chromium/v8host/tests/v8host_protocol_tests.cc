// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Deterministic, in-process conformance tests for the Contract B framing core
// (v8host_protocol). No processes, pipes, or Windows calls — only byte vectors.
// The `vectors` suite asserts exact bytes for canonical encodings, which is the
// real cross-arch guarantee; `--vectors-only` additionally prints a SHA-256 over
// all canonical vectors so CI can compare x86/x64/arm64 digests.

#include "v8host_test_support.h"  // brings in <windows.h>; must precede the header
#include "v8host_protocol.h"      // its #undef ERROR neutralizes the wingdi macro
#include "v8host_protocol_messages.h"  // message codecs, negotiation, request cache

#include <cstddef>
#include <cstdint>
#include "../common/v8host_payload_identity.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using namespace v8host::protocol;
using v8host::test::TestCase;

// ---------------------------------------------------------------------------
// Self-contained SHA-256 (FIPS 180-4). Lives only in this test TU so the
// protocol static_library stays dependency-free. Operates on raw bytes.
// ---------------------------------------------------------------------------
class Sha256 {
 public:
  Sha256() { Reset(); }

  void Reset() {
    state_[0] = 0x6a09e667; state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372; state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f; state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab; state_[7] = 0x5be0cd19;
    bitlen_ = 0;
    buflen_ = 0;
  }

  void Update(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      buffer_[buflen_++] = data[i];
      if (buflen_ == 64) {
        Transform();
        bitlen_ += 512;
        buflen_ = 0;
      }
    }
  }

  void Final(uint8_t out[32]) {
    const uint64_t total_bits = bitlen_ + static_cast<uint64_t>(buflen_) * 8;
    buffer_[buflen_++] = 0x80;
    if (buflen_ > 56) {
      while (buflen_ < 64)
        buffer_[buflen_++] = 0x00;
      Transform();
      buflen_ = 0;
    }
    while (buflen_ < 56)
      buffer_[buflen_++] = 0x00;
    for (int i = 7; i >= 0; --i)
      buffer_[buflen_++] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF);
    Transform();
    for (int i = 0; i < 8; ++i) {
      out[i * 4 + 0] = static_cast<uint8_t>((state_[i] >> 24) & 0xFF);
      out[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xFF);
      out[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xFF);
      out[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xFF);
    }
  }

 private:
  static uint32_t Rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
  }

  void Transform() {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t m[64];
    for (int i = 0; i < 16; ++i) {
      m[i] = (static_cast<uint32_t>(buffer_[i * 4 + 0]) << 24) |
             (static_cast<uint32_t>(buffer_[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(buffer_[i * 4 + 2]) << 8) |
             (static_cast<uint32_t>(buffer_[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 =
          Rotr(m[i - 15], 7) ^ Rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
      const uint32_t s1 =
          Rotr(m[i - 2], 17) ^ Rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
      m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
      const uint32_t ch = (e & f) ^ ((~e) & g);
      const uint32_t t1 = h + s1 + ch + K[i] + m[i];
      const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      h = g; g = f; f = e; e = d + t1;
      d = c; c = b; b = a; a = t1 + t2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
  }

  uint32_t state_[8];
  uint8_t buffer_[64];
  uint64_t bitlen_;
  size_t buflen_;
};

std::string Sha256Hex(const uint8_t* data, size_t size) {
  Sha256 sha;
  sha.Update(data, size);
  uint8_t digest[32];
  sha.Final(digest);
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (int i = 0; i < 32; ++i) {
    out.push_back(kHex[(digest[i] >> 4) & 0xF]);
    out.push_back(kHex[digest[i] & 0xF]);
  }
  return out;
}

std::string Sha256Hex(const std::vector<uint8_t>& v) {
  return Sha256Hex(v.data(), v.size());
}

std::string Sha256HexStr(const char* s) {
  return Sha256Hex(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
}

// ---------------------------------------------------------------------------
// Small test helpers.
// ---------------------------------------------------------------------------
bool Fail(std::string* detail, const std::string& message) {
  *detail = message;
  return false;
}

bool CheckBytes(const char* label,
                const std::vector<uint8_t>& got,
                const uint8_t* want,
                size_t want_len,
                std::string* detail) {
  if (got.size() != want_len) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s: size %zu != %zu", label, got.size(),
                  want_len);
    return Fail(detail, buf);
  }
  for (size_t i = 0; i < want_len; ++i) {
    if (got[i] != want[i]) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s: byte[%zu] = 0x%02X != 0x%02X", label,
                    i, got[i], want[i]);
      return Fail(detail, buf);
    }
  }
  return true;
}

FrameHeader Sentinel() {
  FrameHeader h;
  h.version_major = 0xAAAA;
  h.version_minor = 0xBBBB;
  h.type = static_cast<MessageType>(0xCCCC);
  h.flags = 0xDDDD;
  h.conn_id = 0x11111111;
  h.target_id = 0x22222222;
  h.run_id = 0x33333333;
  h.request_id = 0x44444444;
  h.payload_length = 0x55555555;
  return h;
}

bool SameHeader(const FrameHeader& a, const FrameHeader& b) {
  return a.version_major == b.version_major &&
         a.version_minor == b.version_minor && a.type == b.type &&
         a.flags == b.flags && a.conn_id == b.conn_id &&
         a.target_id == b.target_id && a.run_id == b.run_id &&
         a.request_id == b.request_id && a.payload_length == b.payload_length;
}

// A framing-valid frame with the given payload length (flags within the mask).
std::vector<uint8_t> SampleFrame(uint32_t payload_len) {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.type = MessageType::START_RUN;
  h.flags = kFlagMustUnderstand;
  h.conn_id = 0x01020304;
  h.target_id = 0x05060708;
  h.run_id = 0x090A0B0C;
  h.request_id = 0x0D0E0F10;
  h.payload_length = payload_len;
  std::vector<uint8_t> out;
  EncodeHeader(h, out);
  for (uint32_t i = 0; i < payload_len; ++i)
    out.push_back(static_cast<uint8_t>(i & 0xFF));
  return out;
}

// A string field: u32 length prefix (`len_field`, which may intentionally
// disagree with `bytes` to exercise the length checks) followed by `bytes`.
std::vector<uint8_t> StrBuf(uint32_t len_field,
                            std::initializer_list<uint8_t> bytes) {
  Writer w;
  w.PutU32(len_field);
  std::vector<uint8_t> out = w.buffer();
  out.insert(out.end(), bytes.begin(), bytes.end());
  return out;
}

// ---------------------------------------------------------------------------
// Canonical vectors (fixed order; concatenated for the SHA-256 digest).
// ---------------------------------------------------------------------------

// V1: every header field a distinct, byte-identifiable value (layout probe).
std::vector<uint8_t> VectorAllDistinctHeader() {
  FrameHeader h;
  h.version_major = 0x0102;
  h.version_minor = 0x0304;
  h.type = MessageType::RELAY_FROM_WORKER;  // 0x0201
  h.flags = kFlagMustUnderstand;            // 0x0001
  h.conn_id = 0x11223344;
  h.target_id = 0x55667788;
  h.run_id = 0x99AABBCC;
  h.request_id = 0xDDEEFF00;
  h.payload_length = 0x0000000A;
  std::vector<uint8_t> out;
  EncodeHeader(h, out);
  return out;
}

// V2: a HELLO-shaped header (zero conn/session/run, nonzero request id).
std::vector<uint8_t> VectorHelloHeader() {
  FrameHeader h;
  h.version_major = kWireVersionMajor;  // 2
  h.version_minor = kWireVersionMinor;  // 0
  h.type = MessageType::HELLO;          // 0x0001
  h.flags = 0;
  h.conn_id = 0;
  h.target_id = 0;
  h.run_id = 0;
  h.request_id = 1;
  h.payload_length = 0;
  std::vector<uint8_t> out;
  EncodeHeader(h, out);
  return out;
}

// V3: a Writer payload exercising PutU16/PutU32/PutString (string "A" + U+20AC).
std::vector<uint8_t> VectorWriterPayload() {
  Writer w;
  w.PutU16(0xABCD);
  w.PutU32(0x12345678);
  w.PutString("A\xE2\x82\xAC");
  return w.buffer();
}

// V4: a full CREATE_SESSION frame — header (payload_length set) + the V3 payload.
std::vector<uint8_t> VectorFullFrame() {
  const std::vector<uint8_t> payload = VectorWriterPayload();
  FrameHeader h;
  h.version_major = kWireVersionMajor;
  h.version_minor = kWireVersionMinor;
  h.type = MessageType::CREATE_SESSION;  // 0x0003
  h.flags = 0;
  h.conn_id = 7;
  h.target_id = 0;
  h.run_id = 0;
  h.request_id = 0x0000002A;
  h.payload_length = static_cast<uint32_t>(payload.size());
  std::vector<uint8_t> out;
  EncodeHeader(h, out);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// V5: a HELLO frame (client -> broker); schema-only payload, nonzero request id.
std::vector<uint8_t> VectorHelloFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.request_id = 1;  // conn/session/run stay 0 by contract
  return BuildHelloFrame(h);
}

// V6: a HELLO_ACK frame (broker -> client) carrying the frozen limits.
std::vector<uint8_t> VectorHelloAckFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.conn_id = 9;     // the assigned connection id
  h.request_id = 1;  // echoes the HELLO
  HelloAckPayload p;
  p.broker_version_major = 2;
  p.broker_version_minor = 0;
  p.endpoint_mode = kEndpointModeShared;
  p.max_frame_payload = kMaxFramePayload;
  p.max_sessions_per_connection = kMaxSessionsPerConnection;
  p.max_in_flight_runs_per_session = kMaxInFlightRunsPerSession;
  p.max_file_rules = kMaxFileRules;
  p.max_capabilities = kMaxCapabilities;
  p.max_queued_relay_bytes_per_run = kMaxQueuedRelayBytesPerRun;
  p.max_queued_relay_bytes_per_connection = kMaxQueuedRelayBytesPerConnection;
  return BuildHelloAckFrame(h, p);
}

// V7: an ACK frame (broker -> client) echoing session/run/request correlation.
std::vector<uint8_t> VectorAckFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.conn_id = 9;
  h.target_id = 2;
  h.run_id = 3;
  h.request_id = 0x2A;
  return BuildAckFrame(h);
}

// V8: an ERROR frame (broker -> client); status ERROR_QUOTA + a short message.
std::vector<uint8_t> VectorErrorFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.conn_id = 9;
  h.target_id = 2;
  h.run_id = 3;
  h.request_id = 0x2B;
  ErrorPayload p;
  p.status_code = StatusCode::ERROR_QUOTA;
  p.message = "quota";
  return BuildErrorFrame(h, p);
}

// V9: a CREATE_SESSION frame (client -> broker) with AppContainer + LPAC armed,
// a non-empty profile, two file rules (one read-only), and two capabilities.
std::vector<uint8_t> VectorCreateSessionFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.conn_id = 9;
  h.request_id = 0x30;
  CreateSessionPayload p;
  p.broker_mode = 1;
  p.tier = 2;
  p.integrity = 3;
  p.delayed_integrity = 4;
  p.initial_token = 5;
  p.lockdown_token = 6;
  p.prohibit_dynamic_code = true;
  p.use_app_container = true;
  p.low_privilege_app_container = true;
  p.app_container_profile = "profile";
  p.file_rules = {FileRule{true, "r1"}, FileRule{false, "r2"}};
  p.capabilities = {"cap1", "cap2"};
  return BuildCreateSessionFrame(h, p);
}

// V10: a START_RUN frame (client -> broker) with an engine override, a snapshot
// path, and a short opaque guest payload.
std::vector<uint8_t> VectorStartRunFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.conn_id = 9;
  h.target_id = 2;
  h.run_id = 3;
  h.request_id = 0x31;
  StartRunPayload p;
  p.tier_override = 1;
  p.has_engine_override = true;
  p.has_snapshot = true;
  p.engine_filename = "engine.dll";
  p.snapshot_path = "snap.bin";
  p.guest_payload = {0xDE, 0xAD, 0xBE, 0xEF};
  return BuildStartRunFrame(h, p);
}

// V11/V12: header-only run/session controls.
std::vector<uint8_t> VectorCancelRunFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.flags = kFlagMustUnderstand;
  h.conn_id = 9;
  h.target_id = 2;
  h.run_id = 3;
  h.request_id = 0x32;
  return BuildCancelRunFrame(h);
}

std::vector<uint8_t> VectorCloseSessionFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.flags = kFlagMustUnderstand;
  h.conn_id = 9;
  h.target_id = 2;
  h.request_id = 0x33;
  return BuildCloseSessionFrame(h);
}

// V13: relay payload = [little-endian i32 kind | opaque bytes].
std::vector<uint8_t> VectorRelayFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.type = MessageType::RELAY_TO_WORKER;
  h.conn_id = 9;
  h.target_id = 2;
  h.run_id = 3;
  h.request_id = 0x34;
  const uint8_t body[] = {0xAA, 0x00, 0xBB};
  return BuildRelayFrame(h, -2, body, sizeof(body));
}

// V14: header-only event frames + the RESULT disposition
// codec. All addressed by the header; RESULT carries the 8-byte codec.
std::vector<uint8_t> VectorSessionReadyFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.conn_id = 9;
  h.target_id = 2;
  h.request_id = 0x35;
  return BuildSessionReadyFrame(h);
}

std::vector<uint8_t> VectorStartupReadyFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.conn_id = 9;
  h.target_id = 2;
  h.request_id = 0x36;
  return BuildStartupReadyFrame(h, StartupReadyPayload{1, 0, 1234});
}

std::vector<uint8_t> VectorSecurityReadyFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.conn_id = 9;
  h.target_id = 2;
  h.request_id = 0x37;
  return BuildSecurityReadyFrame(h);
}

std::vector<uint8_t> VectorWorkerExitFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.conn_id = 9;
  h.target_id = 2;
  h.request_id = 0x38;
  return BuildWorkerExitFrame(h);
}

std::vector<uint8_t> VectorResultFrame() {
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.conn_id = 9;
  h.target_id = 2;
  h.run_id = 3;
  h.request_id = 0x39;
  return BuildResultFrame(h, ResultDisposition::kCancelled);
}

std::vector<std::vector<uint8_t>> WorkerRuntimeFrames() {
  FrameHeader h;
  h.version_major = kWireVersionMajor;
  h.flags = kFlagMustUnderstand;
  h.conn_id = 9;
  h.target_id = 4;
  h.request_id = 42;
  std::vector<std::vector<uint8_t>> frames = {
      BuildCreateWorkerFrame(h, CreateWorkerPayload{}), BuildCloseWorkerFrame(h),
      BuildCreateRuntimeFrame(h, CreateRuntimePayload{1, 0, 2, -1}), BuildCloseRuntimeFrame(h)};
  h.flags = 0;
  h.request_id = 0;
  frames.push_back(BuildRuntimeReadyFrame(h));
  frames.push_back(BuildRuntimeClosedFrame(h, RuntimeClosedPayload{}));
  return frames;
}

const uint8_t kExpectTopology0[] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x01, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2A, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x01, 0x00, 0x34, 0x00,
    0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
const uint8_t kExpectTopology1[] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x01, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
const uint8_t kExpectTopology2[] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x0D, 0x00, 0x01, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2A, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
};
const uint8_t kExpectTopology3[] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x01, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
const uint8_t kExpectTopology4[] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
const uint8_t kExpectTopology5[] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x05, 0x01, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00,
};

bool CanonicalWorkerRuntimeFrames(std::string* detail) {
  const auto frames = WorkerRuntimeFrames();
  const std::vector<std::vector<uint8_t>> expected = {
      {std::begin(kExpectTopology0), std::end(kExpectTopology0)},
      {std::begin(kExpectTopology1), std::end(kExpectTopology1)},
      {std::begin(kExpectTopology2), std::end(kExpectTopology2)},
      {std::begin(kExpectTopology3), std::end(kExpectTopology3)},
      {std::begin(kExpectTopology4), std::end(kExpectTopology4)},
      {std::begin(kExpectTopology5), std::end(kExpectTopology5)}};
  if (frames != expected) return Fail(detail, "worker/runtime literal frames");
  CreateWorkerPayload worker;
  CreateRuntimePayload runtime;
  RuntimeClosedPayload closed;
  return DecodeCreateWorkerPayload(expected[0].data() + 32, expected[0].size() - 32, &worker) &&
      worker.max_runtimes == 1 && worker.snapshot_paths.empty() &&
      DecodeCreateRuntimePayload(expected[2].data() + 32, expected[2].size() - 32, &runtime) &&
      runtime.worker_id == 2 && runtime.snapshot_index == -1 &&
      DecodeRuntimeClosedPayload(expected[5].data() + 32, 8, &closed) &&
      closed.status_code == StatusCode::ERROR_RUNTIME_FAILED;
}

void PatchU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value);

bool WorkerRuntimeRejects(std::string* detail) {
  CreateWorkerPayload worker;
  CreateRuntimePayload runtime;
  RuntimeClosedPayload closed;
  auto reject_worker = [&](const std::vector<uint8_t>& b) {
    worker.max_runtimes = 123;
    return !DecodeCreateWorkerPayload(b.data(), b.size(), &worker) && worker.max_runtimes == 123;
  };
  const auto frames = WorkerRuntimeFrames();
  {
    std::vector<uint8_t> body(frames[0].begin() + 32, frames[0].end());
    Reader prefix(body.data(), body.size());
    uint16_t schema = 0, fixed = 0;
    uint32_t session_size = 0;
    if (!prefix.GetU16(&schema) || !prefix.GetU16(&fixed) ||
        !prefix.GetU32(&session_size))
      return Fail(detail, "worker session header");
    auto overlap = body;
    PatchU32(overlap, 4, session_size + 4);
    if (!reject_worker(overlap))
      return Fail(detail, "worker session overlaps trailer");

    CreateWorkerPayload expected;
    if (!DecodeCreateWorkerPayload(body.data(), body.size(), &expected))
      return Fail(detail, "worker forward-compat baseline");
    // CREATE_SESSION permits additive fixed fields. The worker trailer starts
    // at the enlarged session total_size, not at its old offset.
    body.insert(body.begin() + fixed, {0xAA, 0xBB, 0xCC, 0xDD});
    body[2] = static_cast<uint8_t>(fixed + 4);
    body[3] = static_cast<uint8_t>((fixed + 4) >> 8);
    PatchU32(body, 4, session_size + 4);
    if (!DecodeCreateWorkerPayload(body.data(), body.size(), &worker) ||
        worker.max_runtimes != expected.max_runtimes ||
        worker.flags != expected.flags || worker.engine_dll != expected.engine_dll ||
        worker.snapshot_paths != expected.snapshot_paths ||
        worker.session.app_container_profile != expected.session.app_container_profile)
      return Fail(detail, "worker forward-compat session/trailer offset");
  }
  for (size_t which : {0u, 2u, 5u}) {
    const auto& f = frames[which];
    std::vector<uint8_t> body(f.begin() + 32, f.end());
    auto accepts = [&](const std::vector<uint8_t>& b) {
      if (which == 0) return DecodeCreateWorkerPayload(b.data(), b.size(), &worker);
      if (which == 2) return DecodeCreateRuntimePayload(b.data(), b.size(), &runtime);
      return DecodeRuntimeClosedPayload(b.data(), b.size(), &closed);
    };
    if (!accepts(body)) return Fail(detail, "valid topology payload");
    for (size_t n = 0; n < body.size(); ++n)
      if (accepts({body.begin(), body.begin() + n})) return Fail(detail, "topology truncation");
    auto bad = body; bad.push_back(0);
    if (accepts(bad)) return Fail(detail, "topology trailing");
    bad = body; bad[0] = 2;
    if (accepts(bad)) return Fail(detail, "topology schema");
    bad = body; bad[2] = 1;
    if (accepts(bad)) return Fail(detail, "topology reserved/fixed size");
  }
  CreateWorkerPayload w;
  for (uint32_t count : {0u, 8u}) for (uint32_t max : {1u, 16u}) {
    w.max_runtimes = max;
    w.flags = kWorkerFlagSingleUse;
    w.engine_dll = "engine.dll";
    w.snapshot_paths.assign(count, "data/snapshot.bin");
    Writer bytes; EncodeCreateWorkerPayload(w, bytes);
    if (!DecodeCreateWorkerPayload(bytes.buffer().data(), bytes.size(), &worker) ||
        worker.max_runtimes != max || worker.snapshot_paths != w.snapshot_paths ||
        !ValidateCreateWorkerPayload(worker))
      return Fail(detail, "worker bounds roundtrip");
  }
  auto malformed = [&](const CreateWorkerPayload& value) {
    Writer bytes; EncodeCreateWorkerPayload(value, bytes); return reject_worker(bytes.buffer());
  };
  auto rejects = [&](const CreateWorkerPayload& value) {
    Writer bytes; EncodeCreateWorkerPayload(value, bytes);
    return DecodeCreateWorkerPayload(bytes.buffer().data(), bytes.size(), &worker) &&
        !ValidateCreateWorkerPayload(worker);
  };
  for (uint32_t max : {0u, 17u, UINT32_MAX}) {
    auto bad = w; bad.max_runtimes = max;
    if (!rejects(bad)) return Fail(detail, "worker runtime bound");
  }
  auto bad = w; bad.flags = 2;
  if (!rejects(bad)) return Fail(detail, "worker flags");
  bad = w; bad.snapshot_paths.assign(9, "s.bin");
  if (!malformed(bad) || ValidateCreateWorkerPayload(bad)) return Fail(detail, "snapshot count");
  for (const auto& name : {"../engine.dll", "C:engine.dll", "a/b.dll", "a\\b.dll", ".", "a..dll"}) {
    bad = w; bad.engine_dll = name;
    if (!rejects(bad)) return Fail(detail, "engine name");
  }
  for (const auto& path : {"", "../s", "a/../s", "a\\..\\s", "/s", "\\s", "C:s", "s:stream", "a/.. /s"}) {
    bad = w; bad.snapshot_paths = {path};
    if (!rejects(bad)) return Fail(detail, "snapshot relative path");
  }
  for (const std::string text : {std::string(1, char(0xFF)), std::string("a\0b", 3), std::string(kMaxStringBytes + 1, 'a')}) {
    bad = w; bad.engine_dll = text;
    if (!malformed(bad)) return Fail(detail, "worker UTF8/NUL/limit");
    bad = w; bad.snapshot_paths = {text};
    if (!malformed(bad)) return Fail(detail, "snapshot UTF8/NUL/limit");
  }
  bad = w; bad.snapshot_paths.assign(8, std::string(kMaxStringBytes, 's'));
  if (!malformed(bad)) return Fail(detail, "worker payload ceiling");
  for (uint32_t offset : {4u, 8u}) {
    std::vector<uint8_t> b(frames[2].begin() + 32, frames[2].end());
    PatchU32(b, offset, offset == 4 ? 0 : 8);
    if (!DecodeCreateRuntimePayload(b.data(), b.size(), &runtime) ||
        ValidateCreateRuntimePayload(runtime, kMaxWorkerSnapshots)) return Fail(detail, "runtime fields");
  }
  for (int32_t index : {-2, -1, 0, 7, 8, INT32_MAX}) {
    CreateRuntimePayload value{1, 0, 1, index};
    Writer bytes; EncodeCreateRuntimePayload(value, bytes);
    if (!DecodeCreateRuntimePayload(bytes.buffer().data(), bytes.size(), &runtime) ||
        ValidateCreateRuntimePayload(runtime, 8) != (index >= -1 && index < 8) ||
        ValidateCreateRuntimePayload(runtime, 0) != (index == -1) ||
        ValidateCreateRuntimePayload(runtime, 1) != (index == -1 || index == 0))
      return Fail(detail, "runtime snapshot bounds");
  }
  std::vector<uint8_t> b(frames[5].begin() + 32, frames[5].end());
  PatchU32(b, 4, 0);
  if (DecodeRuntimeClosedPayload(b.data(), b.size(), &closed)) return Fail(detail, "runtime status");
  for (size_t i = 0; i < 4; ++i) {
    RequestCache cache;
    FrameHeader h; const uint8_t* payload = nullptr; size_t size = 0;
    if (DecodeAndValidateFrame(frames[i].data(), frames[i].size(), &h, &payload, &size) != DecodeStatus::kOk)
      return false;
    RequestIdentity id{h.type, h.conn_id, h.target_id, h.run_id, h.request_id, h.flags};
    const auto reply = BuildAckFrame(h);
    cache.Record(id, payload, size, reply);
    const auto duplicate = cache.Classify(id, payload, size);
    if (duplicate.classification != RequestClass::kExactDuplicate || *duplicate.cached_response != reply)
      return Fail(detail, "topology duplicate replay");
    std::vector<uint8_t> changed(payload, payload + size); changed.push_back(0);
    if (cache.Classify(id, changed.data(), changed.size()).classification != RequestClass::kMismatchedDuplicate)
      return Fail(detail, "topology fingerprint");
    ++id.target_id;
    if (cache.Classify(id, payload, size).classification != RequestClass::kMismatchedDuplicate)
      return Fail(detail, "topology identity");
  }
  return true;
}

std::vector<std::vector<uint8_t>> CanonicalVectors() {
  return {VectorAllDistinctHeader(),  VectorHelloHeader(),
          VectorWriterPayload(),      VectorFullFrame(),
          VectorHelloFrame(),         VectorHelloAckFrame(),
          VectorAckFrame(),           VectorErrorFrame(),
          VectorCreateSessionFrame(), VectorStartRunFrame(),
          VectorCancelRunFrame(),     VectorCloseSessionFrame(),
          VectorRelayFrame(),         VectorSessionReadyFrame(),
          VectorStartupReadyFrame(),  VectorSecurityReadyFrame(),
          VectorWorkerExitFrame(),    VectorResultFrame(),
          WorkerRuntimeFrames()[0], WorkerRuntimeFrames()[1], WorkerRuntimeFrames()[2],
          WorkerRuntimeFrames()[3], WorkerRuntimeFrames()[4], WorkerRuntimeFrames()[5]};
}

std::vector<uint8_t> CanonicalConcat() {
  std::vector<uint8_t> all;
  for (const std::vector<uint8_t>& v : CanonicalVectors())
    all.insert(all.end(), v.begin(), v.end());
  return all;
}

// The frozen cross-arch canonical-vector contract: the SHA-256 over
// CanonicalConcat() (every canonical vector, fixed order). `--vectors-only`
// asserts the live digest equals this on whatever arch it runs, so an identical
// value on x86/x64/arm64 IS the cross-arch equality gate and any wire/byte
// change trips it. Update this ONLY together with a deliberate wire change.
constexpr const char kExpectedVectorsSha256[] =
    "716a302585704f14a300a70eeaa38290017adb1b29f98b02d719d8b7e67e394c";

// Hard-coded expected bytes (computed independently). These exact-byte asserts
// are the cross-arch contract: any arch must produce precisely these bytes.
const uint8_t kExpectAllDistinct[32] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x01, 0x04, 0x03, 0x01, 0x02, 0x01,
    0x00, 0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55, 0xCC, 0xBB,
    0xAA, 0x99, 0x00, 0xFF, 0xEE, 0xDD, 0x0A, 0x00, 0x00, 0x00};

const uint8_t kExpectHello[32] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t kExpectWriterPayload[14] = {0xCD, 0xAB, 0x78, 0x56, 0x34, 0x12,
                                          0x04, 0x00, 0x00, 0x00, 0x41, 0xE2,
                                          0x82, 0xAC};

// Handshake/control frames. Exact bytes computed independently; any arch must
// produce precisely these for the HELLO / HELLO_ACK / ACK / ERROR layers.
const uint8_t kExpectHelloFrame[36] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};

const uint8_t kExpectHelloAckFrame[72] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xF8, 0xFF, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x01};

const uint8_t kExpectAckFrame[36] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x2A, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};

const uint8_t kExpectErrorFrame[49] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x2B, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x71, 0x75, 0x6F, 0x74,
    0x61};

// Session/run frames. Exact bytes computed independently; any arch must
// produce precisely these for the CREATE_SESSION / START_RUN payloads.
const uint8_t kExpectCreateSessionFrame[131] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x00, 0x00, 0x00, 0x63, 0x00, 0x00, 0x00, 0x01, 0x00, 0x34, 0x00,
    0x63, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x00, 0x00, 0x70, 0x72, 0x6F, 0x66, 0x69, 0x6C, 0x65, 0x01,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x72, 0x31, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x72, 0x32, 0x04, 0x00, 0x00, 0x00, 0x63,
    0x61, 0x70, 0x31, 0x04, 0x00, 0x00, 0x00, 0x63, 0x61, 0x70, 0x32};

const uint8_t kExpectStartRunFrame[86] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x31, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x01, 0x00, 0x18, 0x00,
    0x36, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00,
    0x65, 0x6E, 0x67, 0x69, 0x6E, 0x65, 0x2E, 0x64, 0x6C, 0x6C, 0x08, 0x00,
    0x00, 0x00, 0x73, 0x6E, 0x61, 0x70, 0x2E, 0x62, 0x69, 0x6E, 0xDE, 0xAD,
    0xBE, 0xEF};

const uint8_t kExpectCancelRunFrame[32] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01,
    0x00, 0x09, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t kExpectCloseSessionFrame[32] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x07, 0x00, 0x01,
    0x00, 0x09, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t kExpectRelayFrame[39] = {
    0x56, 0x38, 0x48, 0x57, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x09, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0xFE,
    0xFF, 0xFF, 0xFF, 0xAA, 0x00, 0xBB};

// ---------------------------------------------------------------------------
// vectors suite.
// ---------------------------------------------------------------------------
bool CanonicalAllDistinctHeader(std::string* detail) {
  return CheckBytes("all-distinct", VectorAllDistinctHeader(), kExpectAllDistinct,
                    sizeof(kExpectAllDistinct), detail);
}

bool CanonicalHelloHeader(std::string* detail) {
  return CheckBytes("hello", VectorHelloHeader(), kExpectHello,
                    sizeof(kExpectHello), detail);
}

bool CanonicalWriterPayload(std::string* detail) {
  return CheckBytes("writer-payload", VectorWriterPayload(), kExpectWriterPayload,
                    sizeof(kExpectWriterPayload), detail);
}

bool FullFrameRoundTrip(std::string* detail) {
  const std::vector<uint8_t> frame = VectorFullFrame();
  FrameHeader out;
  const uint8_t* payload = nullptr;
  size_t payload_size = 0;
  const DecodeStatus st = DecodeAndValidateFrame(frame.data(), frame.size(),
                                                 &out, &payload, &payload_size);
  if (st != DecodeStatus::kOk)
    return Fail(detail, "decode failed");
  if (out.version_major != 2 || out.version_minor != 0)
    return Fail(detail, "version mismatch");
  if (out.type != MessageType::CREATE_SESSION)
    return Fail(detail, "type mismatch");
  if (out.conn_id != 7 || out.target_id != 0 || out.run_id != 0 ||
      out.request_id != 0x0000002A)
    return Fail(detail, "id mismatch");
  const std::vector<uint8_t> expected = VectorWriterPayload();
  if (payload_size != expected.size())
    return Fail(detail, "payload size mismatch");
  if (payload == nullptr || std::memcmp(payload, expected.data(), payload_size) != 0)
    return Fail(detail, "payload bytes mismatch");
  return true;
}

bool CanonicalHelloFrame(std::string* detail) {
  return CheckBytes("hello-frame", VectorHelloFrame(), kExpectHelloFrame,
                    sizeof(kExpectHelloFrame), detail);
}

bool CanonicalHelloAckFrame(std::string* detail) {
  return CheckBytes("hello-ack-frame", VectorHelloAckFrame(),
                    kExpectHelloAckFrame, sizeof(kExpectHelloAckFrame), detail);
}

bool CanonicalAckFrame(std::string* detail) {
  return CheckBytes("ack-frame", VectorAckFrame(), kExpectAckFrame,
                    sizeof(kExpectAckFrame), detail);
}

bool CanonicalErrorFrame(std::string* detail) {
  return CheckBytes("error-frame", VectorErrorFrame(), kExpectErrorFrame,
                    sizeof(kExpectErrorFrame), detail);
}

bool CanonicalCreateSessionFrame(std::string* detail) {
  return CheckBytes("create-session-frame", VectorCreateSessionFrame(),
                    kExpectCreateSessionFrame,
                    sizeof(kExpectCreateSessionFrame), detail);
}

bool CanonicalStartRunFrame(std::string* detail) {
  return CheckBytes("start-run-frame", VectorStartRunFrame(),
                    kExpectStartRunFrame, sizeof(kExpectStartRunFrame), detail);
}

bool CanonicalCancelRunFrame(std::string* detail) {
  return CheckBytes("cancel-run-frame", VectorCancelRunFrame(),
                    kExpectCancelRunFrame, sizeof(kExpectCancelRunFrame), detail);
}

bool CanonicalCloseSessionFrame(std::string* detail) {
  return CheckBytes("close-session-frame", VectorCloseSessionFrame(),
                    kExpectCloseSessionFrame, sizeof(kExpectCloseSessionFrame),
                    detail);
}

bool CanonicalRelayFrame(std::string* detail) {
  return CheckBytes("relay-frame", VectorRelayFrame(), kExpectRelayFrame,
                    sizeof(kExpectRelayFrame), detail);
}

bool RelayPayloadCodec(std::string* detail) {
  const std::vector<uint8_t> frame = VectorRelayFrame();
  FrameHeader header;
  const uint8_t* payload = nullptr;
  size_t payload_len = 0;
  if (DecodeAndValidateFrame(frame.data(), frame.size(), &header, &payload,
                             &payload_len) != DecodeStatus::kOk) {
    return Fail(detail, "relay frame rejected");
  }
  int32_t kind = 0;
  const uint8_t* body = nullptr;
  size_t body_len = 0;
  if (!DecodeRelayPayload(payload, payload_len, &kind, &body, &body_len) ||
      kind != -2 || body_len != 3 || body[0] != 0xAA || body[1] != 0 ||
      body[2] != 0xBB) {
    return Fail(detail, "relay payload mismatch");
  }
  if (DecodeRelayPayload(payload, 3, &kind, &body, &body_len) ||
      DecodeRelayPayload(nullptr, 4, &kind, &body, &body_len) ||
      DecodeRelayPayload(payload, payload_len, nullptr, &body, &body_len)) {
    return Fail(detail, "malformed relay payload accepted");
  }
  return true;
}

bool StartupReadyPid(std::string* detail) {
  const uint8_t valid[] = {1, 0, 0, 0, 0xD2, 4, 0, 0};
  StartupReadyPayload p;
  if (!DecodeStartupReadyPayload(valid, sizeof(valid), &p) || p.process_id != 1234)
    return Fail(detail, "startup PID literal");
  Writer w;
  EncodeStartupReadyPayload(p, w);
  if (!CheckBytes("startup PID", w.buffer(), valid, sizeof(valid), detail)) return false;
  for (size_t n = 0; n < sizeof(valid); ++n)
    if (DecodeStartupReadyPayload(valid, n, &p)) return Fail(detail, "startup truncation");
  for (size_t offset : {0u, 2u, 4u}) {
    std::vector<uint8_t> bad(valid, valid + sizeof(valid));
    bad[offset] = offset == 4 ? 0 : 2;
    if (offset == 4) bad[5] = 0;
    if (DecodeStartupReadyPayload(bad.data(), bad.size(), &p)) return Fail(detail, "startup malformed");
  }
  std::vector<uint8_t> trailing(valid, valid + sizeof(valid));
  trailing.push_back(0);
  return !DecodeStartupReadyPayload(trailing.data(), trailing.size(), &p);
}

bool CanonicalEventFrames(std::string* detail) {
  struct Case {
    const char* name;
    std::vector<uint8_t> frame;
    MessageType type;
  };
  const Case cases[] = {
      {"session-ready", VectorSessionReadyFrame(), MessageType::SESSION_READY},
      {"startup-ready", VectorStartupReadyFrame(), MessageType::STARTUP_READY},
      {"security-ready", VectorSecurityReadyFrame(),
       MessageType::SECURITY_READY},
      {"worker-exit", VectorWorkerExitFrame(), MessageType::WORKER_EXIT},
  };
  for (const Case& c : cases) {
    FrameHeader header;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    if (DecodeAndValidateFrame(c.frame.data(), c.frame.size(), &header,
                               &payload, &payload_len) != DecodeStatus::kOk)
      return Fail(detail, std::string(c.name) + " frame rejected");
    if (header.type != c.type || payload_len != (c.type == MessageType::STARTUP_READY ? 8u : 0u))
      return Fail(detail, std::string(c.name) + " wrong type/payload");
  }
  // RESULT carries the disposition codec (here, cancelled).
  const std::vector<uint8_t> result = VectorResultFrame();
  FrameHeader header;
  const uint8_t* payload = nullptr;
  size_t payload_len = 0;
  ResultPayload p;
  if (DecodeAndValidateFrame(result.data(), result.size(), &header, &payload,
                             &payload_len) != DecodeStatus::kOk ||
      header.type != MessageType::RESULT ||
      !DecodeResultPayload(payload, payload_len, &p) ||
      p.disposition != ResultDisposition::kCancelled)
    return Fail(detail, "result frame decode mismatch");
  return true;
}

bool ResultPayloadCodec(std::string* detail) {
  const uint8_t completed[] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const uint8_t cancelled[] = {0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
  ResultPayload out;
  if (!DecodeResultPayload(completed, sizeof(completed), &out) ||
      out.disposition != ResultDisposition::kCompleted ||
      !DecodeResultPayload(cancelled, sizeof(cancelled), &out) ||
      out.disposition != ResultDisposition::kCancelled)
    return Fail(detail, "result disposition round-trip");
  // Fail-closed: unknown disposition(2), trailing byte, short, bad schema.
  const uint8_t bad_disposition[] = {0x01, 0x00, 0x00, 0x00,
                                     0x02, 0x00, 0x00, 0x00};
  const uint8_t trailing[] = {0x01, 0x00, 0x00, 0x00, 0x00,
                              0x00, 0x00, 0x00, 0xFF};
  const uint8_t short_payload[] = {0x01, 0x00, 0x00, 0x00, 0x00};
  const uint8_t bad_schema[] = {0x02, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00};
  if (DecodeResultPayload(bad_disposition, sizeof(bad_disposition), &out) ||
      DecodeResultPayload(trailing, sizeof(trailing), &out) ||
      DecodeResultPayload(short_payload, sizeof(short_payload), &out) ||
      DecodeResultPayload(bad_schema, sizeof(bad_schema), &out))
    return Fail(detail, "malformed result payload accepted");
  return true;
}

// ---------------------------------------------------------------------------
// protocol suite.
// ---------------------------------------------------------------------------
bool Sha256SelfTest(std::string* detail) {
  if (Sha256HexStr("") !=
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
    return Fail(detail, "sha256(empty) mismatch");
  if (Sha256HexStr("abc") !=
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    return Fail(detail, "sha256(abc) mismatch");
  // 56-byte input forces the two-block padding path.
  if (Sha256HexStr(
          "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") !=
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")
    return Fail(detail, "sha256(56-byte) mismatch");
  return true;
}

bool HeaderRoundTrip(std::string* detail) {
  const std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x7F};
  FrameHeader in;
  in.version_major = 1;
  in.version_minor = 0;
  in.type = MessageType::RESULT;
  in.flags = kFlagMustUnderstand;
  in.conn_id = 0x11223344;
  in.target_id = 0x55667788;
  in.run_id = 0x99AABBCC;
  in.request_id = 0xDDEEFF00;
  in.payload_length = static_cast<uint32_t>(payload.size());
  std::vector<uint8_t> frame;
  EncodeHeader(in, frame);
  frame.insert(frame.end(), payload.begin(), payload.end());

  FrameHeader out = Sentinel();
  const uint8_t* view = nullptr;
  size_t view_size = 0;
  if (DecodeAndValidateFrame(frame.data(), frame.size(), &out, &view,
                             &view_size) != DecodeStatus::kOk)
    return Fail(detail, "decode failed");
  if (!SameHeader(in, out))
    return Fail(detail, "header fields differ");
  if (view_size != payload.size() || view == nullptr ||
      std::memcmp(view, payload.data(), view_size) != 0)
    return Fail(detail, "payload view mismatch");
  return true;
}

bool MagicReject(std::string* detail) {
  std::vector<uint8_t> frame = SampleFrame(0);
  frame[0] ^= 0xFF;  // corrupt the magic
  FrameHeader out = Sentinel();
  if (DecodeAndValidateFrame(frame.data(), frame.size(), &out) !=
      DecodeStatus::kBadMagic)
    return Fail(detail, "bad magic not rejected");
  if (!SameHeader(out, Sentinel()))
    return Fail(detail, "output mutated on failure");
  return true;
}

bool TruncationEveryPrefix(std::string* detail) {
  const std::vector<uint8_t> frames[] = {SampleFrame(0), SampleFrame(6)};
  for (const std::vector<uint8_t>& frame : frames) {
    for (size_t prefix = 0; prefix < frame.size(); ++prefix) {
      FrameHeader out = Sentinel();
      const DecodeStatus st =
          DecodeAndValidateFrame(frame.data(), prefix, &out);
      // Below a full header the decode stops at the header-size check; at or
      // above it the header decodes but the declared size no longer matches.
      const DecodeStatus expected = prefix < kFrameHeaderSize
                                        ? DecodeStatus::kIncompleteHeader
                                        : DecodeStatus::kLengthMismatch;
      if (st != expected) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "prefix %zu/%zu status %u != %u", prefix,
                      frame.size(), static_cast<unsigned>(st),
                      static_cast<unsigned>(expected));
        return Fail(detail, buf);
      }
      if (!SameHeader(out, Sentinel())) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "prefix %zu mutated output", prefix);
        return Fail(detail, buf);
      }
    }
  }
  return true;
}

bool PayloadLengthBounds(std::string* detail) {
  // payload_length == kMaxFramePayload, size == 32 + 65528 -> ok.
  {
    FrameHeader h;
    h.version_major = 2;
    h.type = MessageType::RELAY_TO_WORKER;
    h.payload_length = kMaxFramePayload;
    std::vector<uint8_t> frame;
    EncodeHeader(h, frame);
    frame.resize(kFrameHeaderSize + kMaxFramePayload, 0xAB);
    FrameHeader out;
    size_t payload_size = 0;
    if (DecodeAndValidateFrame(frame.data(), frame.size(), &out, nullptr,
                               &payload_size) != DecodeStatus::kOk)
      return Fail(detail, "max payload rejected");
    if (payload_size != kMaxFramePayload)
      return Fail(detail, "max payload size wrong");
  }
  // payload_length == kMaxFramePayload + 1 (65529) -> reject (oversize).
  {
    FrameHeader h;
    h.version_major = 2;
    h.type = MessageType::RELAY_TO_WORKER;
    h.payload_length = kMaxFramePayload + 1;
    std::vector<uint8_t> frame;
    EncodeHeader(h, frame);
    frame.resize(kFrameHeaderSize + kMaxFramePayload + 1, 0xAB);
    FrameHeader out = Sentinel();
    if (DecodeAndValidateFrame(frame.data(), frame.size(), &out) !=
        DecodeStatus::kOversizePayload)
      return Fail(detail, "max+1 payload not rejected");
    if (!SameHeader(out, Sentinel()))
      return Fail(detail, "max+1 mutated output");
  }
  // payload_length that would overflow 32 + len is caught by the oversize gate
  // before any addition — a 32-byte buffer claiming a 4 GiB payload.
  {
    FrameHeader h;
    h.version_major = 2;
    h.type = MessageType::RELAY_TO_WORKER;
    h.payload_length = 0xFFFFFFFFu;
    std::vector<uint8_t> frame;
    EncodeHeader(h, frame);
    FrameHeader out = Sentinel();
    if (DecodeAndValidateFrame(frame.data(), frame.size(), &out) !=
        DecodeStatus::kOversizePayload)
      return Fail(detail, "overflow-length not rejected");
    if (!SameHeader(out, Sentinel()))
      return Fail(detail, "overflow-length mutated output");
  }
  return true;
}

bool ExactFrame(std::string* detail) {
  std::vector<uint8_t> frame = SampleFrame(8);  // 40 bytes, payload_length == 8
  frame.push_back(0x99);                         // 41 bytes with an extra tail
  FrameHeader out = Sentinel();
  if (DecodeAndValidateFrame(frame.data(), 39, &out) !=
      DecodeStatus::kLengthMismatch)
    return Fail(detail, "size-1 not rejected");
  if (!SameHeader(out, Sentinel()))
    return Fail(detail, "size-1 mutated output");
  FrameHeader ok;
  if (DecodeAndValidateFrame(frame.data(), 40, &ok) != DecodeStatus::kOk)
    return Fail(detail, "exact size rejected");
  out = Sentinel();
  if (DecodeAndValidateFrame(frame.data(), 41, &out) !=
      DecodeStatus::kLengthMismatch)
    return Fail(detail, "size+1 not rejected");
  if (!SameHeader(out, Sentinel()))
    return Fail(detail, "size+1 mutated output");
  // A zero-payload frame (exactly 32 bytes) with one trailing byte is likewise
  // not one declared frame.
  {
    std::vector<uint8_t> empty = SampleFrame(0);  // 32 bytes, payload_length 0
    empty.push_back(0x5A);                         // 33 bytes
    FrameHeader bad = Sentinel();
    if (DecodeAndValidateFrame(empty.data(), 33, &bad) !=
        DecodeStatus::kLengthMismatch)
      return Fail(detail, "zero-payload size+1 not rejected");
    if (!SameHeader(bad, Sentinel()))
      return Fail(detail, "zero-payload size+1 mutated output");
    FrameHeader good;
    if (DecodeAndValidateFrame(empty.data(), 32, &good) != DecodeStatus::kOk)
      return Fail(detail, "zero-payload exact size rejected");
  }
  return true;
}

bool ReservedFlags(std::string* detail) {
  const uint16_t reserved_values[] = {0x0002, 0x8000, 0xFFFE};
  for (uint16_t flags : reserved_values) {
    FrameHeader h;
    h.version_major = 2;
    h.type = MessageType::HELLO;
    h.flags = flags;
    h.payload_length = 0;
    std::vector<uint8_t> frame;
    EncodeHeader(h, frame);
    FrameHeader out = Sentinel();
    if (DecodeAndValidateFrame(frame.data(), frame.size(), &out) !=
        DecodeStatus::kReservedFlags)
      return Fail(detail, "reserved flag not rejected");
    if (!SameHeader(out, Sentinel()))
      return Fail(detail, "reserved flag mutated output");
  }
  // MUST_UNDERSTAND alone is framing-valid.
  {
    FrameHeader h;
    h.version_major = 2;
    h.type = MessageType::HELLO;
    h.flags = kFlagMustUnderstand;
    h.payload_length = 0;
    std::vector<uint8_t> frame;
    EncodeHeader(h, frame);
    FrameHeader out;
    if (DecodeAndValidateFrame(frame.data(), frame.size(), &out) !=
        DecodeStatus::kOk)
      return Fail(detail, "must-understand rejected");
    if (out.flags != kFlagMustUnderstand)
      return Fail(detail, "flags not preserved");
  }
  return true;
}

bool ReaderBounds(std::string* detail) {
  const uint8_t buf[3] = {0x01, 0x02, 0x03};
  {
    Reader r(buf, sizeof(buf));
    uint16_t v = 0;
    if (!r.GetU16(&v) || v != 0x0201)
      return Fail(detail, "GetU16 LE wrong");
    uint16_t v2 = 0;
    if (r.GetU16(&v2))
      return Fail(detail, "GetU16 past end accepted");
    if (r.offset() != 2 || r.BytesRemaining() != 1)
      return Fail(detail, "GetU16 mutated on failure");
  }
  {
    Reader r(buf, sizeof(buf));
    uint32_t v = 0;
    if (r.GetU32(&v))
      return Fail(detail, "GetU32 past end accepted");
    if (r.offset() != 0)
      return Fail(detail, "GetU32 mutated on failure");
  }
  {
    Reader r(buf, sizeof(buf));
    std::vector<uint8_t> out;
    if (r.GetBytes(4, &out))
      return Fail(detail, "GetBytes past end accepted");
    if (r.offset() != 0)
      return Fail(detail, "GetBytes mutated on failure");
    if (!r.GetBytes(3, &out) || out.size() != 3 || out[0] != 1 || out[2] != 3)
      return Fail(detail, "GetBytes exact failed");
    if (!r.AtEnd())
      return Fail(detail, "reader not at end");
  }
  return true;
}

bool WriterReaderRoundTrip(std::string* detail) {
  Writer w;
  w.PutU16(0xBEEF);
  w.PutU32(0x01234567);
  const std::vector<uint8_t> raw = {0xAA, 0xBB, 0xCC};
  w.PutBytes(raw);
  const std::string text = "hello \xE2\x82\xAC world";
  w.PutString(text);

  Reader r(w.buffer());
  uint16_t a = 0;
  uint32_t b = 0;
  std::vector<uint8_t> c;
  std::string s;
  if (!r.GetU16(&a) || a != 0xBEEF)
    return Fail(detail, "u16 round-trip");
  if (!r.GetU32(&b) || b != 0x01234567)
    return Fail(detail, "u32 round-trip");
  if (!r.GetBytes(raw.size(), &c) || c != raw)
    return Fail(detail, "bytes round-trip");
  if (!r.GetString(&s) || s != text)
    return Fail(detail, "string round-trip");
  if (!r.AtEnd())
    return Fail(detail, "reader not fully consumed");
  return true;
}

bool StringReaderValid(std::string* detail) {
  Writer w;
  w.PutString("A\xE2\x82\xAC");
  Reader r(w.buffer());
  std::string s;
  if (!r.GetString(&s))
    return Fail(detail, "valid string rejected");
  if (s.size() != 4 || static_cast<uint8_t>(s[0]) != 0x41 ||
      static_cast<uint8_t>(s[1]) != 0xE2 ||
      static_cast<uint8_t>(s[2]) != 0x82 ||
      static_cast<uint8_t>(s[3]) != 0xAC)
    return Fail(detail, "string bytes wrong");
  if (!r.AtEnd())
    return Fail(detail, "reader not at end");
  // Round-trip each UTF-8 length class at its boundary code points: U+0080 and
  // U+07FF (2-byte), U+0800 and U+FFFF (3-byte), U+10000 and U+10FFFF (4-byte).
  {
    const std::vector<uint8_t> bytes = {
        0xC2, 0x80,              // U+0080
        0xDF, 0xBF,              // U+07FF
        0xE0, 0xA0, 0x80,        // U+0800
        0xEF, 0xBF, 0xBF,        // U+FFFF
        0xF0, 0x90, 0x80, 0x80,  // U+10000
        0xF4, 0x8F, 0xBF, 0xBF,  // U+10FFFF
    };
    const std::string boundaries(bytes.begin(), bytes.end());
    Writer bw;
    bw.PutString(boundaries);
    Reader br(bw.buffer());
    std::string got;
    if (!br.GetString(&got) || got != boundaries)
      return Fail(detail, "boundary code points round-trip failed");
    if (!br.AtEnd())
      return Fail(detail, "boundary reader not at end");
  }
  return true;
}

bool StringReaderReject(std::string* detail) {
  struct Case {
    const char* name;
    std::vector<uint8_t> buf;
  };
  const Case cases[] = {
      {"len>remaining", StrBuf(100, {0x41, 0x42, 0x43})},
      {"len>max", StrBuf(kMaxStringBytes + 1, {})},
      {"embedded-nul", StrBuf(3, {0x41, 0x00, 0x42})},
      {"overlong-c0-80", StrBuf(2, {0xC0, 0x80})},
      {"overlong-e0-80-80", StrBuf(3, {0xE0, 0x80, 0x80})},
      {"overlong-f0-80-80-80", StrBuf(4, {0xF0, 0x80, 0x80, 0x80})},
      {"lone-80", StrBuf(1, {0x80})},
      {"truncated-c2", StrBuf(1, {0xC2})},
      {"truncated-e2-82", StrBuf(2, {0xE2, 0x82})},
      {"surrogate-ed-a0-80", StrBuf(3, {0xED, 0xA0, 0x80})},
      {"surrogate-ed-bf-bf", StrBuf(3, {0xED, 0xBF, 0xBF})},
      {"above-10ffff-f4-90", StrBuf(4, {0xF4, 0x90, 0x80, 0x80})},
      {"above-10ffff-f5", StrBuf(4, {0xF5, 0x80, 0x80, 0x80})},
  };
  for (const Case& c : cases) {
    Reader r(c.buf);
    std::string s = "UNTOUCHED";
    if (r.GetString(&s))
      return Fail(detail, std::string("accepted invalid: ") + c.name);
    if (r.offset() != 0)
      return Fail(detail, std::string("mutated on reject: ") + c.name);
    if (s != "UNTOUCHED")
      return Fail(detail, std::string("out param written on reject: ") + c.name);
  }
  // A valid non-empty string still passes next to these rejects.
  {
    Writer w;
    w.PutString("ok");
    Reader r(w.buffer());
    std::string s;
    if (!r.GetString(&s) || s != "ok")
      return Fail(detail, "valid string after rejects failed");
  }
  return true;
}

// ---------------------------------------------------------------------------
// messages suite: payload codec round-trips, reject paths, builders.
// ---------------------------------------------------------------------------
std::vector<uint8_t> ValidHelloBytes() {
  Writer w;
  EncodeHelloPayload(HelloPayload{}, w);
  return w.buffer();
}

std::vector<uint8_t> ValidHelloAckBytes() {
  Writer w;
  EncodeHelloAckPayload(HelloAckPayload{}, w);
  return w.buffer();
}

std::vector<uint8_t> ValidAckBytes() {
  Writer w;
  EncodeAckPayload(AckPayload{}, w);
  return w.buffer();
}

std::vector<uint8_t> ValidErrorBytes() {
  ErrorPayload p;
  p.status_code = StatusCode::ERROR_PROTOCOL;
  p.message = "x";
  Writer w;
  EncodeErrorPayload(p, w);
  return w.buffer();
}

bool MessagesHelloRoundTrip(std::string* detail) {
  const std::vector<uint8_t> bytes = ValidHelloBytes();
  HelloPayload out;
  out.schema_version = 0xFFFF;  // sentinels; decode must overwrite
  out.reserved = 0xFFFF;
  if (!DecodeHelloPayload(bytes.data(), bytes.size(), &out))
    return Fail(detail, "hello decode failed");
  if (out.schema_version != kMessageSchemaVersion || out.reserved != 0)
    return Fail(detail, "hello fields wrong");
  return true;
}

bool MessagesHelloAckRoundTrip(std::string* detail) {
  HelloAckPayload in;
  in.broker_version_major = 0x1234;
  in.broker_version_minor = 0x5678;
  in.endpoint_mode = kEndpointModeDedicated;
  in.max_frame_payload = 0x00010203;
  in.max_sessions_per_connection = 0x04050607;
  in.max_in_flight_runs_per_session = 0x08090A0B;
  in.max_file_rules = 0x0C0D0E0F;
  in.max_capabilities = 0x10111213;
  in.max_queued_relay_bytes_per_run = 0x14151617;
  in.max_queued_relay_bytes_per_connection = 0x18191A1B;
  Writer w;
  EncodeHelloAckPayload(in, w);
  HelloAckPayload out;
  if (!DecodeHelloAckPayload(w.buffer().data(), w.buffer().size(), &out))
    return Fail(detail, "hello-ack decode failed");
  if (out.schema_version != kMessageSchemaVersion || out.reserved != 0 ||
      out.broker_version_major != in.broker_version_major ||
      out.broker_version_minor != in.broker_version_minor ||
      out.endpoint_mode != in.endpoint_mode ||
      out.max_frame_payload != in.max_frame_payload ||
      out.max_sessions_per_connection != in.max_sessions_per_connection ||
      out.max_in_flight_runs_per_session != in.max_in_flight_runs_per_session ||
      out.max_file_rules != in.max_file_rules ||
      out.max_capabilities != in.max_capabilities ||
      out.max_queued_relay_bytes_per_run != in.max_queued_relay_bytes_per_run ||
      out.max_queued_relay_bytes_per_connection !=
          in.max_queued_relay_bytes_per_connection)
    return Fail(detail, "hello-ack fields differ");
  return true;
}

bool MessagesAckRoundTrip(std::string* detail) {
  const std::vector<uint8_t> bytes = ValidAckBytes();
  AckPayload out;
  out.schema_version = 0xFFFF;
  out.reserved = 0xFFFF;
  if (!DecodeAckPayload(bytes.data(), bytes.size(), &out))
    return Fail(detail, "ack decode failed");
  if (out.schema_version != kMessageSchemaVersion || out.reserved != 0)
    return Fail(detail, "ack fields wrong");
  return true;
}

bool MessagesErrorRoundTrip(std::string* detail) {
  // Non-empty message carrying a multi-byte code point.
  {
    ErrorPayload in;
    in.status_code = StatusCode::ERROR_STALE_REQUEST;
    in.message = "stale \xE2\x82\xAC";
    Writer w;
    EncodeErrorPayload(in, w);
    ErrorPayload out;
    if (!DecodeErrorPayload(w.buffer().data(), w.buffer().size(), &out))
      return Fail(detail, "error decode failed");
    if (out.schema_version != kMessageSchemaVersion || out.reserved != 0 ||
        out.status_code != StatusCode::ERROR_STALE_REQUEST ||
        out.message != in.message)
      return Fail(detail, "error fields differ");
  }
  // Empty message.
  {
    ErrorPayload in;
    in.status_code = StatusCode::ERROR_QUOTA;
    in.message = "";
    Writer w;
    EncodeErrorPayload(in, w);
    ErrorPayload out;
    out.message = "UNTOUCHED";
    if (!DecodeErrorPayload(w.buffer().data(), w.buffer().size(), &out))
      return Fail(detail, "empty-message error decode failed");
    if (out.status_code != StatusCode::ERROR_QUOTA || !out.message.empty())
      return Fail(detail, "empty-message error fields differ");
  }
  return true;
}

bool MessagesRejectBadPrologue(std::string* detail) {
  // A bad schema_version or reserved value is rejected before any other field;
  // a 4-byte prologue alone is enough to prove the check fires for every type.
  const std::vector<uint8_t> bad_schema = {0x02, 0x00, 0x00, 0x00};
  const std::vector<uint8_t> bad_reserved = {0x01, 0x00, 0x01, 0x00};
  struct Decoder {
    const char* name;
    std::function<bool(const uint8_t*, size_t)> fn;
  };
  const Decoder decoders[] = {
      {"hello",
       [](const uint8_t* d, size_t n) {
         HelloPayload o;
         return DecodeHelloPayload(d, n, &o);
       }},
      {"hello-ack",
       [](const uint8_t* d, size_t n) {
         HelloAckPayload o;
         return DecodeHelloAckPayload(d, n, &o);
       }},
      {"ack",
       [](const uint8_t* d, size_t n) {
         AckPayload o;
         return DecodeAckPayload(d, n, &o);
       }},
      {"error",
       [](const uint8_t* d, size_t n) {
         ErrorPayload o;
         return DecodeErrorPayload(d, n, &o);
       }},
  };
  for (const Decoder& dec : decoders) {
    if (dec.fn(bad_schema.data(), bad_schema.size()))
      return Fail(detail, std::string("bad schema accepted: ") + dec.name);
    if (dec.fn(bad_reserved.data(), bad_reserved.size()))
      return Fail(detail, std::string("bad reserved accepted: ") + dec.name);
  }
  return true;
}

bool MessagesRejectFraming(std::string* detail) {
  // Trailing bytes (payload not fully consumed) and truncation at every prefix
  // are rejected for each payload type.
  struct Case {
    const char* name;
    std::vector<uint8_t> valid;
    std::function<bool(const uint8_t*, size_t)> fn;
  };
  const Case cases[] = {
      {"hello", ValidHelloBytes(),
       [](const uint8_t* d, size_t n) {
         HelloPayload o;
         return DecodeHelloPayload(d, n, &o);
       }},
      {"hello-ack", ValidHelloAckBytes(),
       [](const uint8_t* d, size_t n) {
         HelloAckPayload o;
         return DecodeHelloAckPayload(d, n, &o);
       }},
      {"ack", ValidAckBytes(),
       [](const uint8_t* d, size_t n) {
         AckPayload o;
         return DecodeAckPayload(d, n, &o);
       }},
      {"error", ValidErrorBytes(),
       [](const uint8_t* d, size_t n) {
         ErrorPayload o;
         return DecodeErrorPayload(d, n, &o);
       }},
  };
  for (const Case& c : cases) {
    if (!c.fn(c.valid.data(), c.valid.size()))
      return Fail(detail, std::string("valid rejected: ") + c.name);
    std::vector<uint8_t> extra = c.valid;
    extra.push_back(0x00);
    if (c.fn(extra.data(), extra.size()))
      return Fail(detail, std::string("trailing accepted: ") + c.name);
    for (size_t n = 0; n < c.valid.size(); ++n) {
      if (c.fn(c.valid.data(), n))
        return Fail(detail, std::string("truncation accepted: ") + c.name);
    }
  }
  return true;
}

bool MessagesRejectErrorString(std::string* detail) {
  // Prologue (schema=1, reserved=0) + status_code, then a malformed string.
  auto build = [](const std::vector<uint8_t>& str_field) {
    Writer w;
    w.PutU16(1);
    w.PutU16(0);
    w.PutU32(1);
    std::vector<uint8_t> out = w.buffer();
    out.insert(out.end(), str_field.begin(), str_field.end());
    return out;
  };
  // Embedded NUL in the message bytes.
  {
    const std::vector<uint8_t> field = {0x03, 0x00, 0x00, 0x00,
                                        0x41, 0x00, 0x42};
    const std::vector<uint8_t> bytes = build(field);
    ErrorPayload out;
    if (DecodeErrorPayload(bytes.data(), bytes.size(), &out))
      return Fail(detail, "embedded NUL accepted");
  }
  // Invalid UTF-8 (lone continuation byte).
  {
    const std::vector<uint8_t> field = {0x01, 0x00, 0x00, 0x00, 0x80};
    const std::vector<uint8_t> bytes = build(field);
    ErrorPayload out;
    if (DecodeErrorPayload(bytes.data(), bytes.size(), &out))
      return Fail(detail, "invalid UTF-8 accepted");
  }
  return true;
}

bool MessagesBuildersStampType(std::string* detail) {
  // Each builder owns the message type and produces a frame that decodes back
  // to that type, regardless of a caller-set header.type.
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.request_id = 1;
  h.type = MessageType::RESULT;  // deliberately wrong; must be overwritten
  HelloAckPayload ack_payload;
  ErrorPayload err_payload;
  err_payload.message = "x";
  struct Case {
    MessageType want;
    std::vector<uint8_t> frame;
  };
  const Case cases[] = {
      {MessageType::HELLO, BuildHelloFrame(h)},
      {MessageType::HELLO_ACK, BuildHelloAckFrame(h, ack_payload)},
      {MessageType::ACK, BuildAckFrame(h)},
      {MessageType::ERROR, BuildErrorFrame(h, err_payload)},
  };
  for (const Case& c : cases) {
    FrameHeader out;
    if (DecodeAndValidateFrame(c.frame.data(), c.frame.size(), &out) !=
        DecodeStatus::kOk)
      return Fail(detail, "builder frame decode failed");
    if (out.type != c.want)
      return Fail(detail, "builder did not stamp the type");
    if (out.request_id != 1)
      return Fail(detail, "builder dropped correlation");
  }
  return true;
}

bool MessagesCorrelation(std::string* detail) {
  // ACK/ERROR builders echo conn/session/run/request in the frame header.
  FrameHeader h;
  h.version_major = 2;
  h.version_minor = 0;
  h.conn_id = 0x0A0B0C0D;
  h.target_id = 0x11223344;
  h.run_id = 0x55667788;
  h.request_id = 0x99AABBCC;
  auto check = [&](const std::vector<uint8_t>& frame, MessageType want) {
    FrameHeader out;
    if (DecodeAndValidateFrame(frame.data(), frame.size(), &out) !=
        DecodeStatus::kOk)
      return false;
    return out.type == want && out.conn_id == h.conn_id &&
           out.target_id == h.target_id && out.run_id == h.run_id &&
           out.request_id == h.request_id;
  };
  if (!check(BuildAckFrame(h), MessageType::ACK))
    return Fail(detail, "ack correlation not echoed");
  ErrorPayload p;
  p.status_code = StatusCode::ERROR_BAD_STATE;
  p.message = "x";
  if (!check(BuildErrorFrame(h, p), MessageType::ERROR))
    return Fail(detail, "error correlation not echoed");
  return true;
}

// ---------------------------------------------------------------------------
// session suite: CREATE_SESSION codec (§8.5) + per-message structural quotas.
// ---------------------------------------------------------------------------

// Overwrites a little-endian u16/u32 already present in an encoded payload, used
// to inject malformed field values the struct encoder cannot express.
void PatchU16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
  b[off] = static_cast<uint8_t>(v & 0xFF);
  b[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void PatchU32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  b[off] = static_cast<uint8_t>(v & 0xFF);
  b[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  b[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  b[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

std::vector<uint8_t> EncodeSession(const CreateSessionPayload& p) {
  Writer w;
  EncodeCreateSessionPayload(p, w);
  return w.buffer();
}

bool DecodeSessionOk(const std::vector<uint8_t>& b, CreateSessionPayload* out) {
  return DecodeCreateSessionPayload(b.data(), b.size(), out);
}

bool DecodeSessionRejects(const std::vector<uint8_t>& b) {
  CreateSessionPayload out;
  return !DecodeCreateSessionPayload(b.data(), b.size(), &out);
}

bool SessionEq(const CreateSessionPayload& a,
               const CreateSessionPayload& b,
               std::string* detail) {
  if (a.broker_mode != b.broker_mode || a.tier != b.tier ||
      a.integrity != b.integrity || a.delayed_integrity != b.delayed_integrity ||
      a.initial_token != b.initial_token ||
      a.lockdown_token != b.lockdown_token ||
      a.prohibit_dynamic_code != b.prohibit_dynamic_code ||
      a.use_app_container != b.use_app_container ||
      a.low_privilege_app_container != b.low_privilege_app_container ||
      a.app_container_profile != b.app_container_profile)
    return Fail(detail, "session scalar/profile mismatch");
  if (a.file_rules.size() != b.file_rules.size())
    return Fail(detail, "file-rule count mismatch");
  for (size_t i = 0; i < a.file_rules.size(); ++i) {
    if (a.file_rules[i].readonly != b.file_rules[i].readonly ||
        a.file_rules[i].pattern != b.file_rules[i].pattern)
      return Fail(detail, "file-rule mismatch");
  }
  if (a.capabilities != b.capabilities)
    return Fail(detail, "capabilities mismatch");
  return true;
}

// A fully-populated, cross-field-valid session (AppContainer + LPAC armed).
CreateSessionPayload ValidSession() {
  CreateSessionPayload p;
  p.broker_mode = 1;
  p.tier = 2;
  p.integrity = 3;
  p.delayed_integrity = 4;
  p.initial_token = 5;
  p.lockdown_token = 6;
  p.prohibit_dynamic_code = true;
  p.use_app_container = true;
  p.low_privilege_app_container = true;
  p.app_container_profile = "profile";
  p.file_rules = {FileRule{true, "ro"}, FileRule{false, "rw"}};
  p.capabilities = {"capA", "capB"};
  return p;
}

bool SessionRoundTrip(std::string* detail) {
  // Empty: AppContainer off, empty profile, no rules/caps; extreme i32 values
  // (incl. negative) prove the signed fields transport verbatim.
  {
    CreateSessionPayload in;
    in.broker_mode = -5;
    in.tier = 0x7FFFFFFF;
    in.lockdown_token = static_cast<int32_t>(0x80000000);  // INT32_MIN
    in.use_app_container = false;
    CreateSessionPayload out;
    if (!DecodeSessionOk(EncodeSession(in), &out))
      return Fail(detail, "empty session decode failed");
    if (!SessionEq(in, out, detail))
      return false;
    if (!out.file_rules.empty() || !out.capabilities.empty())
      return Fail(detail, "empty session had rules/caps");
  }
  // Boundary: exactly 64 file rules + 64 capabilities with AppContainer armed.
  {
    CreateSessionPayload in = ValidSession();
    in.file_rules.clear();
    in.capabilities.clear();
    for (uint32_t i = 0; i < kMaxFileRules; ++i)
      in.file_rules.push_back(FileRule{(i & 1) == 0, "rule" + std::to_string(i)});
    for (uint32_t i = 0; i < kMaxCapabilities; ++i)
      in.capabilities.push_back("cap" + std::to_string(i));
    CreateSessionPayload out;
    if (!DecodeSessionOk(EncodeSession(in), &out))
      return Fail(detail, "boundary session decode failed");
    if (!SessionEq(in, out, detail))
      return false;
    if (out.file_rules.size() != kMaxFileRules ||
        out.capabilities.size() != kMaxCapabilities)
      return Fail(detail, "boundary counts wrong");
  }
  // Frame round-trip exercises BuildCreateSessionFrame + the frame validator.
  {
    FrameHeader h;
    h.version_major = 2;
    h.conn_id = 9;
    h.request_id = 0x30;
    const std::vector<uint8_t> frame =
        BuildCreateSessionFrame(h, ValidSession());
    FrameHeader out_h;
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;
    if (DecodeAndValidateFrame(frame.data(), frame.size(), &out_h, &payload,
                               &payload_size) != DecodeStatus::kOk)
      return Fail(detail, "session frame failed validation");
    if (out_h.type != MessageType::CREATE_SESSION)
      return Fail(detail, "session frame wrong type");
    CreateSessionPayload out;
    if (!DecodeCreateSessionPayload(payload, payload_size, &out))
      return Fail(detail, "session frame payload decode failed");
    if (!SessionEq(ValidSession(), out, detail))
      return false;
  }
  return true;
}

bool SessionRejectCounts(std::string* detail) {
  // 65 file rules → reject (AppContainer off, so the count is the only fault).
  {
    CreateSessionPayload in;
    in.use_app_container = false;
    for (uint32_t i = 0; i <= kMaxFileRules; ++i)
      in.file_rules.push_back(FileRule{false, "r"});
    if (!DecodeSessionRejects(EncodeSession(in)))
      return Fail(detail, "65 file rules accepted");
  }
  // 65 capabilities → reject (AppContainer on + profile, so only the count).
  {
    CreateSessionPayload in;
    in.use_app_container = true;
    in.app_container_profile = "p";
    for (uint32_t i = 0; i <= kMaxCapabilities; ++i)
      in.capabilities.push_back("c");
    if (!DecodeSessionRejects(EncodeSession(in)))
      return Fail(detail, "65 capabilities accepted");
  }
  return true;
}

// A count that lies BEYOND a valid-total_size body: the element reads run off
// the real body and must fail-closed (no over-read, no fabricated elements) —
// distinct from reject-counts, which uses a consistent oversized body.
bool SessionRejectLyingCount(std::string* detail) {
  {
    std::vector<uint8_t> b = EncodeSession(ValidSession());  // 2 rules, 2 caps
    PatchU32(b, 44, 4);  // file_rule_count 2 -> 4, total_size left valid
    if (!DecodeSessionRejects(b))
      return Fail(detail, "lying file_rule_count accepted");
  }
  {
    std::vector<uint8_t> b = EncodeSession(ValidSession());
    PatchU32(b, 48, 8);  // capability_count 2 -> 8, total_size left valid
    if (!DecodeSessionRejects(b))
      return Fail(detail, "lying capability_count accepted");
  }
  return true;
}

bool SessionRejectBadBool(std::string* detail) {
  // The three fixed booleans live at payload offsets 32/36/40.
  struct Case {
    const char* name;
    size_t off;
  };
  const Case fixed_bools[] = {{"prohibit_dynamic_code", 32},
                              {"use_app_container", 36},
                              {"low_privilege_app_container", 40}};
  for (const Case& c : fixed_bools) {
    std::vector<uint8_t> bytes = EncodeSession(ValidSession());
    PatchU32(bytes, c.off, 2);  // neither 0 nor 1
    if (!DecodeSessionRejects(bytes))
      return Fail(detail, std::string("bad bool accepted: ") + c.name);
  }
  // A file_rule.readonly of 2 is equally malformed. With AppContainer off and an
  // empty profile, the readonly u32 sits at a known offset:
  //   52 fixed + profile string(4 bytes, empty) => readonly at offset 56.
  {
    CreateSessionPayload in;
    in.use_app_container = false;
    in.file_rules = {FileRule{false, "r"}};
    std::vector<uint8_t> bytes = EncodeSession(in);
    PatchU32(bytes, 56, 2);
    if (!DecodeSessionRejects(bytes))
      return Fail(detail, "bad file_rule.readonly accepted");
  }
  return true;
}

bool SessionRejectStringLimit(std::string* detail) {
  // A profile of exactly 32 KiB decodes.
  {
    CreateSessionPayload in;
    in.use_app_container = false;
    in.app_container_profile = std::string(kMaxStringBytes, 'a');
    CreateSessionPayload out;
    if (!DecodeSessionOk(EncodeSession(in), &out))
      return Fail(detail, "32 KiB profile rejected");
    if (out.app_container_profile.size() != kMaxStringBytes)
      return Fail(detail, "32 KiB profile truncated");
  }
  // 32 KiB + 1 is rejected by GetString.
  {
    CreateSessionPayload in;
    in.use_app_container = false;
    in.app_container_profile = std::string(kMaxStringBytes + 1, 'a');
    if (!DecodeSessionRejects(EncodeSession(in)))
      return Fail(detail, "oversize profile accepted");
  }
  return true;
}

bool SessionRejectBadUtf8(std::string* detail) {
  // Invalid UTF-8 in the profile (lone continuation byte).
  {
    CreateSessionPayload in;
    in.use_app_container = false;
    in.app_container_profile = std::string(1, static_cast<char>(0x80));
    if (!DecodeSessionRejects(EncodeSession(in)))
      return Fail(detail, "invalid-UTF-8 profile accepted");
  }
  // Invalid UTF-8 in a file-rule pattern (overlong C0 80).
  {
    CreateSessionPayload in;
    in.use_app_container = false;
    in.file_rules = {FileRule{
        false, std::string({static_cast<char>(0xC0), static_cast<char>(0x80)})}};
    if (!DecodeSessionRejects(EncodeSession(in)))
      return Fail(detail, "invalid-UTF-8 pattern accepted");
  }
  // Embedded NUL in a capability (AppContainer on so the cap is otherwise legal).
  {
    CreateSessionPayload in;
    in.use_app_container = true;
    in.app_container_profile = "p";
    in.capabilities = {std::string({'a', '\0', 'b'})};
    if (!DecodeSessionRejects(EncodeSession(in)))
      return Fail(detail, "embedded-NUL capability accepted");
  }
  return true;
}

bool SessionRejectTotalSize(std::string* detail) {
  const std::vector<uint8_t> valid = EncodeSession(ValidSession());
  {
    std::vector<uint8_t> b = valid;
    PatchU32(b, 4, static_cast<uint32_t>(valid.size()) + 1);  // too big
    if (!DecodeSessionRejects(b))
      return Fail(detail, "total_size too big accepted");
  }
  {
    std::vector<uint8_t> b = valid;
    PatchU32(b, 4, static_cast<uint32_t>(valid.size()) - 1);  // too small
    if (!DecodeSessionRejects(b))
      return Fail(detail, "total_size too small accepted");
  }
  return true;
}

bool SessionRejectFixedSize(std::string* detail) {
  const std::vector<uint8_t> valid = EncodeSession(ValidSession());
  // fixed_size below the v1 known section.
  {
    std::vector<uint8_t> b = valid;
    PatchU16(b, 2, kCreateSessionFixedSize - 1);  // 51
    if (!DecodeSessionRejects(b))
      return Fail(detail, "fixed_size < 52 accepted");
  }
  // fixed_size beyond total_size.
  {
    std::vector<uint8_t> b = valid;
    PatchU16(b, 2, static_cast<uint16_t>(valid.size() + 1));
    if (!DecodeSessionRejects(b))
      return Fail(detail, "fixed_size > total_size accepted");
  }
  return true;
}

bool SessionForwardCompat(std::string* detail) {
  // A future minor version appends `extra` fixed bytes after the 52-byte v1
  // section; a v1 reader skips them (variable section starts at fixed_size).
  const CreateSessionPayload in = ValidSession();
  std::vector<uint8_t> b = EncodeSession(in);
  const uint16_t extra = 4;
  b.insert(b.begin() + kCreateSessionFixedSize, extra, 0x7E);
  PatchU16(b, 2, kCreateSessionFixedSize + extra);  // fixed_size = 56
  PatchU32(b, 4, static_cast<uint32_t>(b.size()));   // new total_size
  CreateSessionPayload out;
  if (!DecodeSessionOk(b, &out))
    return Fail(detail, "forward-compat payload rejected");
  if (!SessionEq(in, out, detail))
    return false;
  return true;
}

bool SessionRejectTrailingShort(std::string* detail) {
  const std::vector<uint8_t> valid = EncodeSession(ValidSession());
  // Trailing byte beyond total_size: size grows, stale total_size → mismatch.
  {
    std::vector<uint8_t> b = valid;
    b.push_back(0x00);
    if (!DecodeSessionRejects(b))
      return Fail(detail, "trailing byte accepted");
  }
  // Short buffer: size shrinks, stale total_size → mismatch.
  {
    std::vector<uint8_t> b = valid;
    b.pop_back();
    if (!DecodeSessionRejects(b))
      return Fail(detail, "short buffer accepted");
  }
  // Declared-but-unconsumed: total_size counts an extra byte the structure never
  // reads, so AtEnd() must still reject (exact consumption).
  {
    std::vector<uint8_t> b = valid;
    b.push_back(0x00);
    PatchU32(b, 4, static_cast<uint32_t>(b.size()));
    if (!DecodeSessionRejects(b))
      return Fail(detail, "unconsumed trailing byte accepted");
  }
  return true;
}

bool SessionRejectCrossField(std::string* detail) {
  // Empty profile with AppContainer enabled → reject.
  {
    CreateSessionPayload in;
    in.use_app_container = true;
    in.app_container_profile = "";
    if (!DecodeSessionRejects(EncodeSession(in)))
      return Fail(detail, "empty profile + AppContainer accepted");
  }
  // A capability with AppContainer disabled → reject.
  {
    CreateSessionPayload in;
    in.use_app_container = false;
    in.capabilities = {"cap"};
    if (!DecodeSessionRejects(EncodeSession(in)))
      return Fail(detail, "capability without AppContainer accepted");
  }
  // LPAC without AppContainer → reject.
  {
    CreateSessionPayload in;
    in.use_app_container = false;
    in.low_privilege_app_container = true;
    if (!DecodeSessionRejects(EncodeSession(in)))
      return Fail(detail, "LPAC without AppContainer accepted");
  }
  return true;
}

bool SessionNearMaxFrame(std::string* detail) {
  // Two file-rule patterns bring the payload to exactly kMaxFramePayload:
  //   52 fixed + profile(4, empty) + 2*(4 readonly + 4 len + pat) == 65528
  //   => pat == 32728 (below the 32 KiB per-string cap).
  const size_t pat = (kMaxFramePayload - (kCreateSessionFixedSize + 4)) / 2 - 8;
  {
    CreateSessionPayload in;
    in.use_app_container = false;
    in.file_rules = {FileRule{false, std::string(pat, 'a')},
                     FileRule{true, std::string(pat, 'b')}};
    const std::vector<uint8_t> bytes = EncodeSession(in);
    if (bytes.size() != kMaxFramePayload)
      return Fail(detail, "near-max payload not at the ceiling");
    CreateSessionPayload out;
    if (!DecodeSessionOk(bytes, &out))
      return Fail(detail, "max-size payload rejected");
    if (out.file_rules.size() != 2 ||
        out.file_rules[0].pattern.size() != pat ||
        out.file_rules[1].pattern.size() != pat)
      return Fail(detail, "max-size patterns wrong");
  }
  // One byte over the ceiling → reject (total_size > kMaxFramePayload).
  {
    CreateSessionPayload in;
    in.use_app_container = false;
    in.file_rules = {FileRule{false, std::string(pat, 'a')},
                     FileRule{true, std::string(pat + 1, 'b')}};
    const std::vector<uint8_t> bytes = EncodeSession(in);
    if (bytes.size() != kMaxFramePayload + 1)
      return Fail(detail, "over-ceiling payload not 65529");
    if (!DecodeSessionRejects(bytes))
      return Fail(detail, "over-ceiling payload accepted");
  }
  return true;
}

// ---------------------------------------------------------------------------
// run suite: START_RUN codec (§8.6) + field-format rules.
// ---------------------------------------------------------------------------
std::vector<uint8_t> EncodeRun(const StartRunPayload& p) {
  Writer w;
  EncodeStartRunPayload(p, w);
  return w.buffer();
}

bool DecodeRunOk(const std::vector<uint8_t>& b, StartRunPayload* out) {
  return DecodeStartRunPayload(b.data(), b.size(), out);
}

bool DecodeRunRejects(const std::vector<uint8_t>& b) {
  StartRunPayload out;
  return !DecodeStartRunPayload(b.data(), b.size(), &out);
}

bool RunEq(const StartRunPayload& a,
           const StartRunPayload& b,
           std::string* detail) {
  if (a.tier_override != b.tier_override ||
      a.has_engine_override != b.has_engine_override ||
      a.has_snapshot != b.has_snapshot ||
      a.engine_filename != b.engine_filename ||
      a.snapshot_path != b.snapshot_path || a.guest_payload != b.guest_payload)
    return Fail(detail, "start-run field mismatch");
  return true;
}

bool RunRoundTrip(std::string* detail) {
  // Full: a bare engine override, a snapshot path (separators allowed for the
  // snapshot, unlike the engine filename), an opaque guest payload, negative
  // tier override.
  {
    StartRunPayload in;
    in.tier_override = -1;
    in.has_engine_override = true;
    in.engine_filename = "v8jsi.dll";
    in.has_snapshot = true;
    in.snapshot_path = "cache/app.snapshot";
    in.guest_payload = {0x00, 0x01, 0x02, 0xFE, 0xFF};
    StartRunPayload out;
    if (!DecodeRunOk(EncodeRun(in), &out))
      return Fail(detail, "full run decode failed");
    if (!RunEq(in, out, detail))
      return false;
  }
  // Minimal: no override, no snapshot, empty guest payload.
  {
    StartRunPayload in;
    in.tier_override = 7;
    StartRunPayload out;
    if (!DecodeRunOk(EncodeRun(in), &out))
      return Fail(detail, "minimal run decode failed");
    if (!RunEq(in, out, detail))
      return false;
    if (!out.engine_filename.empty() || !out.snapshot_path.empty() ||
        !out.guest_payload.empty())
      return Fail(detail, "minimal run not empty");
  }
  // Frame round-trip exercises BuildStartRunFrame + the frame validator.
  {
    FrameHeader h;
    h.version_major = 2;
    h.conn_id = 9;
    h.target_id = 2;
    h.run_id = 3;
    h.request_id = 0x31;
    StartRunPayload in;
    in.has_engine_override = true;
    in.engine_filename = "engine.dll";
    in.guest_payload = {0xAA, 0xBB};
    const std::vector<uint8_t> frame = BuildStartRunFrame(h, in);
    FrameHeader out_h;
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;
    if (DecodeAndValidateFrame(frame.data(), frame.size(), &out_h, &payload,
                               &payload_size) != DecodeStatus::kOk)
      return Fail(detail, "run frame failed validation");
    if (out_h.type != MessageType::START_RUN)
      return Fail(detail, "run frame wrong type");
    StartRunPayload out;
    if (!DecodeStartRunPayload(payload, payload_size, &out))
      return Fail(detail, "run frame payload decode failed");
    if (!RunEq(in, out, detail))
      return false;
  }
  return true;
}

bool RunRejectEngineFilename(std::string* detail) {
  // has_engine_override == 1 but the filename is not a bare payload-dir name.
  const char* bad[] = {"sub\\engine.dll", "sub/engine.dll", "c:engine.dll",
                       "..", ".", "", "a..b", "..x", "x.."};
  for (const char* name : bad) {
    StartRunPayload in;
    in.has_engine_override = true;
    in.engine_filename = name;
    if (!DecodeRunRejects(EncodeRun(in)))
      return Fail(detail, std::string("non-bare engine filename accepted: '") +
                              name + "'");
  }
  // A plain bare filename still decodes.
  {
    StartRunPayload in;
    in.has_engine_override = true;
    in.engine_filename = "engine.dll";
    StartRunPayload out;
    if (!DecodeRunOk(EncodeRun(in), &out))
      return Fail(detail, "bare engine filename rejected");
  }
  return true;
}

bool RunRejectEngineRequiredEmpty(std::string* detail) {
  // has_engine_override == 0 but a non-empty filename → reject.
  StartRunPayload in;
  in.has_engine_override = false;
  in.engine_filename = "engine.dll";
  if (!DecodeRunRejects(EncodeRun(in)))
    return Fail(detail, "no-override + non-empty filename accepted");
  return true;
}

bool RunRejectSnapshot(std::string* detail) {
  // has_snapshot == 0 but a non-empty path → reject.
  {
    StartRunPayload in;
    in.has_snapshot = false;
    in.snapshot_path = "snap.bin";
    if (!DecodeRunRejects(EncodeRun(in)))
      return Fail(detail, "no-snapshot + non-empty path accepted");
  }
  // has_snapshot == 1 but an empty path → reject.
  {
    StartRunPayload in;
    in.has_snapshot = true;
    in.snapshot_path = "";
    if (!DecodeRunRejects(EncodeRun(in)))
      return Fail(detail, "snapshot + empty path accepted");
  }
  return true;
}

bool RunRejectStrings(std::string* detail) {
  // Oversize snapshot path (> 32 KiB) → GetString rejects.
  {
    StartRunPayload in;
    in.has_snapshot = true;
    in.snapshot_path = std::string(kMaxStringBytes + 1, 'a');
    if (!DecodeRunRejects(EncodeRun(in)))
      return Fail(detail, "oversize snapshot path accepted");
  }
  // Invalid UTF-8 in the engine filename.
  {
    StartRunPayload in;
    in.has_engine_override = true;
    in.engine_filename = std::string(1, static_cast<char>(0x80));
    if (!DecodeRunRejects(EncodeRun(in)))
      return Fail(detail, "invalid-UTF-8 engine filename accepted");
  }
  // Embedded NUL in the snapshot path.
  {
    StartRunPayload in;
    in.has_snapshot = true;
    in.snapshot_path = std::string({'a', '\0', 'b'});
    if (!DecodeRunRejects(EncodeRun(in)))
      return Fail(detail, "embedded-NUL snapshot path accepted");
  }
  return true;
}

bool RunRejectFraming(std::string* detail) {
  StartRunPayload base;
  base.has_engine_override = true;
  base.engine_filename = "engine.dll";
  base.has_snapshot = true;
  base.snapshot_path = "snap.bin";
  base.guest_payload = {0xDE, 0xAD};
  const std::vector<uint8_t> valid = EncodeRun(base);
  {
    std::vector<uint8_t> b = valid;
    PatchU32(b, 4, static_cast<uint32_t>(valid.size()) + 1);  // total too big
    if (!DecodeRunRejects(b))
      return Fail(detail, "total_size too big accepted");
  }
  {
    std::vector<uint8_t> b = valid;
    PatchU32(b, 4, static_cast<uint32_t>(valid.size()) - 1);  // total too small
    if (!DecodeRunRejects(b))
      return Fail(detail, "total_size too small accepted");
  }
  {
    std::vector<uint8_t> b = valid;
    PatchU16(b, 2, kStartRunFixedSize - 1);  // fixed_size < 24
    if (!DecodeRunRejects(b))
      return Fail(detail, "fixed_size < 24 accepted");
  }
  {
    std::vector<uint8_t> b = valid;
    b.push_back(0x00);  // trailing byte beyond total_size
    if (!DecodeRunRejects(b))
      return Fail(detail, "trailing byte accepted");
  }
  {
    std::vector<uint8_t> b = valid;
    b.pop_back();  // short buffer
    if (!DecodeRunRejects(b))
      return Fail(detail, "short buffer accepted");
  }
  {
    std::vector<uint8_t> b = valid;
    b.push_back(0x00);
    PatchU32(b, 4, static_cast<uint32_t>(b.size()));  // declared but unconsumed
    if (!DecodeRunRejects(b))
      return Fail(detail, "unconsumed trailing byte accepted");
  }
  return true;
}

bool RunForwardCompat(std::string* detail) {
  StartRunPayload in;
  in.tier_override = 3;
  in.has_engine_override = true;
  in.engine_filename = "engine.dll";
  in.has_snapshot = true;
  in.snapshot_path = "snap.bin";
  in.guest_payload = {0x11, 0x22, 0x33};
  std::vector<uint8_t> b = EncodeRun(in);
  const uint16_t extra = 4;
  b.insert(b.begin() + kStartRunFixedSize, extra, 0x7E);
  PatchU16(b, 2, kStartRunFixedSize + extra);  // fixed_size = 28
  PatchU32(b, 4, static_cast<uint32_t>(b.size()));
  StartRunPayload out;
  if (!DecodeRunOk(b, &out))
    return Fail(detail, "run forward-compat payload rejected");
  if (!RunEq(in, out, detail))
    return false;
  return true;
}

// guest_payload_length lying BEYOND a valid-total_size body: GetBytes must
// bound the claim against the remaining bytes and fail-closed (no over-read).
bool RunRejectLyingGuestLen(std::string* detail) {
  StartRunPayload in;
  in.tier_override = 1;
  in.guest_payload = {1, 2, 3, 4, 5};
  std::vector<uint8_t> b = EncodeRun(in);
  PatchU32(b, 20, 99);  // guest_payload_length 5 -> 99, total_size left valid
  if (!DecodeRunRejects(b))
    return Fail(detail, "lying guest_payload_length accepted");
  return true;
}

bool RunGuestCeiling(std::string* detail) {
  // Guest payload sized so the whole payload is exactly kMaxFramePayload:
  //   24 fixed + engine(4, empty) + snapshot(4, empty) + guest == 65528.
  const size_t guest_max = kMaxFramePayload - (kStartRunFixedSize + 4 + 4);
  {
    StartRunPayload in;
    in.guest_payload = std::vector<uint8_t>(guest_max, 0x5A);
    const std::vector<uint8_t> bytes = EncodeRun(in);
    if (bytes.size() != kMaxFramePayload)
      return Fail(detail, "ceiling guest payload not at 65528");
    StartRunPayload out;
    if (!DecodeRunOk(bytes, &out))
      return Fail(detail, "max guest payload rejected");
    if (out.guest_payload.size() != guest_max)
      return Fail(detail, "max guest payload truncated");
  }
  // One byte over → reject (total_size > kMaxFramePayload).
  {
    StartRunPayload in;
    in.guest_payload = std::vector<uint8_t>(guest_max + 1, 0x5A);
    const std::vector<uint8_t> bytes = EncodeRun(in);
    if (bytes.size() != kMaxFramePayload + 1)
      return Fail(detail, "over-ceiling guest payload not 65529");
    if (!DecodeRunRejects(bytes))
      return Fail(detail, "over-ceiling guest payload accepted");
  }
  return true;
}

// ---------------------------------------------------------------------------
// negotiate suite: version matrix (E15).
// ---------------------------------------------------------------------------
NegotiationResult RunNegotiate(uint16_t client_major,
                               uint16_t client_minor,
                               uint16_t broker_major,
                               uint16_t broker_minor,
                               uint32_t conn_id,
                               uint32_t request_id) {
  FrameHeader hello;
  hello.version_major = client_major;
  hello.version_minor = client_minor;
  hello.type = MessageType::HELLO;
  hello.request_id = request_id;
  BrokerCapabilities caps;
  caps.broker_version_major = broker_major;
  caps.broker_version_minor = broker_minor;
  return NegotiateHello(hello, HelloPayload{}, caps, conn_id);
}

bool CheckHelloAck(const NegotiationResult& r,
                   uint16_t expect_selected_minor,
                   uint16_t expect_broker_minor,
                   uint32_t expect_conn,
                   uint32_t expect_request,
                   std::string* detail) {
  if (r.decision != NegotiationDecision::kAck)
    return Fail(detail, "decision not kAck");
  if (r.selected_minor != expect_selected_minor)
    return Fail(detail, "selected_minor wrong");
  FrameHeader out;
  const uint8_t* pl = nullptr;
  size_t pls = 0;
  if (DecodeAndValidateFrame(r.frame.data(), r.frame.size(), &out, &pl, &pls) !=
      DecodeStatus::kOk)
    return Fail(detail, "hello-ack frame decode failed");
  if (out.type != MessageType::HELLO_ACK)
    return Fail(detail, "reply not HELLO_ACK");
  if (out.conn_id != expect_conn)
    return Fail(detail, "assigned conn id not in header");
  if (out.request_id != expect_request)
    return Fail(detail, "request id not echoed");
  if (out.version_major != 2 || out.version_minor != expect_selected_minor)
    return Fail(detail, "header wire version wrong");
  HelloAckPayload p;
  if (!DecodeHelloAckPayload(pl, pls, &p))
    return Fail(detail, "hello-ack payload decode failed");
  if (p.broker_version_major != 2 ||
      p.broker_version_minor != expect_broker_minor)
    return Fail(detail, "broker max version wrong");
  if (p.endpoint_mode != kEndpointModeShared ||
      p.max_frame_payload != kMaxFramePayload ||
      p.max_sessions_per_connection != kMaxSessionsPerConnection ||
      p.max_in_flight_runs_per_session != kMaxInFlightRunsPerSession ||
      p.max_file_rules != kMaxFileRules ||
      p.max_capabilities != kMaxCapabilities ||
      p.max_queued_relay_bytes_per_run != kMaxQueuedRelayBytesPerRun ||
      p.max_queued_relay_bytes_per_connection !=
          kMaxQueuedRelayBytesPerConnection)
    return Fail(detail, "advertised limits/endpoint wrong");
  return true;
}

bool Wire2VersionSet(std::string* detail) {
  static_assert(v8host::kV8HostWireMajor == kWireVersionMajor);
  static_assert(v8host::kV8HostWireMinor == kWireVersionMinor);
  if (kWireVersionMajor != 2 || kWireVersionMinor != 0) return Fail(detail, "wire version");
  FrameHeader h;
  h.request_id = 1;
  BrokerCapabilities caps;
  for (uint16_t major : {1, 3}) {
    h.version_major = major;
    auto rejected = NegotiateHello(h, HelloPayload{}, caps, 9);
    FrameHeader reply; const uint8_t* bytes = nullptr; size_t size = 0;
    ErrorPayload error;
    if (rejected.decision != NegotiationDecision::kReject ||
        DecodeAndValidateFrame(rejected.frame.data(), rejected.frame.size(), &reply, &bytes, &size) != DecodeStatus::kOk ||
        reply.version_major != kWireVersionMajor || !DecodeErrorPayload(bytes, size, &error) ||
        error.status_code != StatusCode::ERROR_VERSION) return Fail(detail, "incompatible major accepted");
  }
  h.version_major = 2;
  auto accepted = NegotiateHello(h, HelloPayload{}, caps, 9);
  if (accepted.decision != NegotiationDecision::kAck || accepted.selected_minor != 0)
    return Fail(detail, "wire2 rejected");
  const std::array<uint8_t, 32> zero = {};
  std::array<uint8_t, 32> actual = {};
  const std::array<uint8_t, 32> expected = {0xFD, 0x11, 0xCA, 0x68, 0x4D, 0xD7, 0x2E, 0x52, 0x77, 0x3F, 0x86, 0x9E, 0xB4, 0x5A, 0x9F, 0xC1, 0x68, 0x60, 0x56, 0x0D, 0x59, 0x79, 0x58, 0x83, 0x76, 0x91, 0xAD, 0xC4, 0xA2, 0x83, 0x84, 0x0E};
  if (!v8host::ComputePluginSetId(0x8664, zero, zero, &actual) || actual != expected)
    return Fail(detail, "wire2 plugin hash literal");
  std::vector<uint8_t> input;
  const char domain[] = "V8HOST-PLUGIN-SET-V1";
  input.insert(input.end(), domain, domain + sizeof(domain));
  input.insert(input.end(), {0x64, 0x86, 1, 0});
  input.resize(input.size() + 64, 0);
  const auto old_hash = Sha256Hex(input);
  return old_hash == "368ca3ef621dfebe7542a3442a18b9036d8a008b5f5fc1691a8f0b723087f04b" && actual != zero && actual == expected;
}

bool NegotiateEqualMinor(std::string* detail) {
  return CheckHelloAck(RunNegotiate(2, 0, 2, 0, 9, 1), 0, 0, 9, 1, detail);
}

bool NegotiateNewerBroker(std::string* detail) {
  // broker_minor (7) > client_minor (3) -> selected == client_minor (3).
  return CheckHelloAck(RunNegotiate(2, 3, 2, 7, 9, 1), 3, 7, 9, 1, detail);
}

bool NegotiateNewerClient(std::string* detail) {
  // client_minor (7) > broker_minor (3) -> selected == broker_minor (3).
  return CheckHelloAck(RunNegotiate(2, 7, 2, 3, 9, 1), 3, 3, 9, 1, detail);
}

bool NegotiateMajorMismatch(std::string* detail) {
  NegotiationResult r = RunNegotiate(1, 0, 2, 0, 9, 1);
  if (r.decision != NegotiationDecision::kReject)
    return Fail(detail, "decision not kReject");
  FrameHeader out;
  const uint8_t* pl = nullptr;
  size_t pls = 0;
  if (DecodeAndValidateFrame(r.frame.data(), r.frame.size(), &out, &pl, &pls) !=
      DecodeStatus::kOk)
    return Fail(detail, "error frame decode failed");
  if (out.type != MessageType::ERROR)
    return Fail(detail, "reply not ERROR");
  if (out.request_id != 1)
    return Fail(detail, "request id not echoed");
  if (out.conn_id != 9)
    return Fail(detail, "assigned conn id not in reject header");
  if (out.version_major != 2)
    return Fail(detail, "broker major not spoken in reject");
  ErrorPayload p;
  if (!DecodeErrorPayload(pl, pls, &p))
    return Fail(detail, "error payload decode failed");
  if (p.status_code != StatusCode::ERROR_VERSION)
    return Fail(detail, "status not ERROR_VERSION");
  return true;
}

// ---------------------------------------------------------------------------
// cache suite: correlation + duplicate horizon (D18.9).
// ---------------------------------------------------------------------------
RequestIdentity MakeRequestId(uint32_t request_id, uint32_t session_id = 1) {
  RequestIdentity id;
  id.type = MessageType::CREATE_SESSION;
  id.conn_id = 1;
  id.target_id = session_id;
  id.run_id = 0;
  id.request_id = request_id;
  id.flags = 0;
  return id;
}

std::vector<uint8_t> PayloadSeed(uint32_t seed) {
  return {static_cast<uint8_t>(seed), static_cast<uint8_t>(seed >> 8),
          static_cast<uint8_t>(seed >> 16), static_cast<uint8_t>(seed >> 24)};
}

bool CacheFreshRecordReplay(std::string* detail) {
  RequestCache cache;
  const RequestIdentity id = MakeRequestId(1);
  const std::vector<uint8_t> payload = PayloadSeed(1);
  ClassifyResult first = cache.Classify(id, payload.data(), payload.size());
  if (first.classification != RequestClass::kFresh)
    return Fail(detail, "first classify not fresh");
  if (first.cached_response != nullptr)
    return Fail(detail, "fresh carried a response");
  const std::vector<uint8_t> response = {0xDE, 0xAD, 0xBE, 0xEF};
  cache.Record(id, payload.data(), payload.size(), response);
  if (cache.size() != 1 || cache.watermark() != 1)
    return Fail(detail, "record bookkeeping wrong");
  ClassifyResult replay = cache.Classify(id, payload.data(), payload.size());
  if (replay.classification != RequestClass::kExactDuplicate)
    return Fail(detail, "replay not exact duplicate");
  if (replay.cached_response == nullptr || *replay.cached_response != response)
    return Fail(detail, "cached response bytes mismatch");
  return true;
}

bool CacheMismatchedPayload(std::string* detail) {
  RequestCache cache;
  const RequestIdentity id = MakeRequestId(5);
  const std::vector<uint8_t> p1 = PayloadSeed(5);
  const std::vector<uint8_t> resp = {0x01};
  cache.Record(id, p1.data(), p1.size(), resp);
  const std::vector<uint8_t> p2 = PayloadSeed(6);  // same id, different payload
  ClassifyResult c = cache.Classify(id, p2.data(), p2.size());
  if (c.classification != RequestClass::kMismatchedDuplicate)
    return Fail(detail, "different payload not mismatched");
  if (c.cached_response != nullptr)
    return Fail(detail, "mismatch carried a response");
  return true;
}

bool CacheMismatchedIdentity(std::string* detail) {
  RequestCache cache;
  const RequestIdentity id = MakeRequestId(5, /*session_id=*/1);
  const std::vector<uint8_t> p = PayloadSeed(5);
  const std::vector<uint8_t> resp = {0x01};
  cache.Record(id, p.data(), p.size(), resp);
  const RequestIdentity id2 = MakeRequestId(5, /*session_id=*/2);
  ClassifyResult c = cache.Classify(id2, p.data(), p.size());
  if (c.classification != RequestClass::kMismatchedDuplicate)
    return Fail(detail, "different identity not mismatched");
  return true;
}

bool CacheFingerprintSensitive(std::string* detail) {
  RequestCache cache;
  const RequestIdentity id = MakeRequestId(1);
  const std::vector<uint8_t> base = {0x01, 0x02, 0x03};
  const std::vector<uint8_t> resp = {0xAA};
  cache.Record(id, base.data(), base.size(), resp);
  const std::vector<uint8_t> flipped = {0x01, 0x02, 0x04};  // one byte differs
  if (cache.Classify(id, flipped.data(), flipped.size()).classification !=
      RequestClass::kMismatchedDuplicate)
    return Fail(detail, "one-byte difference not detected");
  if (cache.Classify(id, base.data(), base.size()).classification !=
      RequestClass::kExactDuplicate)
    return Fail(detail, "identical payload not exact");
  return true;
}

bool CacheEvictionStale(std::string* detail) {
  RequestCache cache;
  const uint32_t total = kDuplicateCacheSize + 44;  // drive past the horizon
  for (uint32_t i = 1; i <= total; ++i) {
    const RequestIdentity id = MakeRequestId(i);
    const std::vector<uint8_t> payload = PayloadSeed(i);
    if (cache.Classify(id, payload.data(), payload.size()).classification !=
        RequestClass::kFresh)
      return Fail(detail, "monotonic request not fresh");
    cache.Record(id, payload.data(), payload.size(),
                 PayloadSeed(0xF00000 + i));
  }
  if (cache.size() != kDuplicateCacheSize)
    return Fail(detail, "cache exceeded the horizon");
  if (cache.watermark() != total)
    return Fail(detail, "watermark wrong");
  const uint32_t lowest = total - kDuplicateCacheSize + 1;  // oldest survivor
  // An evicted id is permanently stale, not re-executed.
  {
    const RequestIdentity id = MakeRequestId(1);
    const std::vector<uint8_t> payload = PayloadSeed(1);
    if (cache.Classify(id, payload.data(), payload.size()).classification !=
        RequestClass::kStale)
      return Fail(detail, "evicted id not stale");
  }
  // The boundary survivor still replays its recorded response exactly.
  {
    const RequestIdentity id = MakeRequestId(lowest);
    const std::vector<uint8_t> payload = PayloadSeed(lowest);
    ClassifyResult c = cache.Classify(id, payload.data(), payload.size());
    if (c.classification != RequestClass::kExactDuplicate)
      return Fail(detail, "surviving id not exact duplicate");
    const std::vector<uint8_t> expected = PayloadSeed(0xF00000 + lowest);
    if (c.cached_response == nullptr || *c.cached_response != expected)
      return Fail(detail, "survivor response wrong");
  }
  // A brand-new id above the watermark is fresh again.
  {
    const RequestIdentity id = MakeRequestId(total + 1);
    const std::vector<uint8_t> payload = PayloadSeed(total + 1);
    if (cache.Classify(id, payload.data(), payload.size()).classification !=
        RequestClass::kFresh)
      return Fail(detail, "above-watermark id not fresh");
  }
  return true;
}

bool NativePayloadDirectory(std::string* detail) {
  constexpr uint16_t kI386 = 0x014C, kAmd64 = 0x8664, kArm64 = 0xAA64;
  struct Row {
    uint16_t module, native;
    const wchar_t* dir;
    const wchar_t* expected;
  };
  const Row rows[] = {
      {kAmd64, kAmd64, L"C:\\app\\x64", L"C:\\app\\x64"},
      {kArm64, kArm64, L"C:\\app\\arm64", L"C:\\app\\arm64"},
      {kI386, kAmd64, L"C:\\app\\x86", L"C:\\app\\x64"},
      {kI386, kAmd64, L"C:\\app\\x86\\", L"C:\\app\\x64"},
      {kAmd64, kArm64, L"C:\\app\\arm64ec", L"C:\\app\\arm64"},
      {kAmd64, kArm64, L"C:\\app\\x64", L"C:\\app\\arm64"},
      {kI386, kArm64, L"C:\\app\\x86", L"C:\\app\\arm64"},
      {kI386, kAmd64, L"\\\\?\\C:\\app\\x86", L"\\\\?\\C:\\app\\x64"},
      {kI386, kAmd64, L"\\\\server\\share\\app\\x86", L"\\\\server\\share\\app\\x64"},
      {kI386, kAmd64, L"C:\\x86", L"C:\\x64"},
  };
  for (const Row& row : rows) {
    std::wstring out = L"unchanged";
    if (!v8host::ResolveNativePayloadDirectory(row.dir, row.module, row.native, &out) ||
        out != row.expected)
      return Fail(detail, "native payload directory mapping");
  }
  return true;
}

bool NativePayloadNoFallback(std::string* detail) {
  constexpr uint16_t kI386 = 0x014C, kAmd64 = 0x8664, kArm64 = 0xAA64;
  std::wstring out = L"unchanged";
  // 32-bit Windows (native I386) and unknown machines have no payload: no fallback.
  for (uint16_t native : {kI386, uint16_t{0}, uint16_t{0x01C4}}) {
    for (uint16_t module : {kI386, kAmd64, kArm64}) {
      if (v8host::ResolveNativePayloadDirectory(L"C:\\app\\x86", module, native, &out) ||
          out != L"unchanged")
        return Fail(detail, "non-x64/arm64 native machine resolved a payload");
    }
  }
  // A cross-architecture client needs a parent directory; a null output or empty input fails.
  if (v8host::ResolveNativePayloadDirectory(L"x86", kI386, kAmd64, &out) ||
      v8host::ResolveNativePayloadDirectory(L"", kI386, kAmd64, &out) ||
      v8host::ResolveNativePayloadDirectory(L"C:\\app\\x86", kI386, kAmd64, nullptr) ||
      out != L"unchanged")
    return Fail(detail, "missing parent, empty input, or null output accepted");
  // The same-architecture case needs no parent.
  return v8host::ResolveNativePayloadDirectory(L"x64", kAmd64, kAmd64, &out) && out == L"x64"
      ? true : Fail(detail, "same-architecture directory changed");
}

bool NativeMachineMatchesBuild(std::string* detail) {
  uint16_t native = 0;
  if (!v8host::QueryNativeMachine(&native) || (native != 0x8664 && native != 0xAA64))
    return Fail(detail, "native machine is not x64 or ARM64");
  if (sizeof(void*) == 4 ? v8host::kModuleMachine != 0x014C
                         : v8host::kModuleMachine != 0x8664 && v8host::kModuleMachine != 0xAA64)
    return Fail(detail, "module machine does not match the pointer size");
  std::wstring out;
  if (!v8host::ResolveNativePayloadDirectory(L"C:\\app\\here", v8host::kModuleMachine, native, &out) ||
      (v8host::kModuleMachine == native) != (out == L"C:\\app\\here"))
    return Fail(detail, "resolution disagrees with the live machine pair");
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<TestCase> tests = {
      {"layout", "native-payload-directory", NativePayloadDirectory},
      {"layout", "native-payload-no-fallback", NativePayloadNoFallback},
      {"layout", "native-machine-matches-build", NativeMachineMatchesBuild},
      {"vectors", "canonical-all-distinct-header", CanonicalAllDistinctHeader},
      {"vectors", "canonical-hello-header", CanonicalHelloHeader},
      {"vectors", "canonical-writer-payload", CanonicalWriterPayload},
      {"vectors", "full-frame-round-trip", FullFrameRoundTrip},
      {"vectors", "canonical-hello-frame", CanonicalHelloFrame},
      {"vectors", "canonical-hello-ack-frame", CanonicalHelloAckFrame},
      {"vectors", "canonical-ack-frame", CanonicalAckFrame},
      {"vectors", "canonical-error-frame", CanonicalErrorFrame},
      {"vectors", "canonical-create-session-frame", CanonicalCreateSessionFrame},
      {"vectors", "canonical-start-run-frame", CanonicalStartRunFrame},
      {"vectors", "canonical-cancel-run-frame", CanonicalCancelRunFrame},
      {"vectors", "canonical-close-session-frame", CanonicalCloseSessionFrame},
      {"vectors", "canonical-relay-frame", CanonicalRelayFrame},
      {"vectors", "canonical-event-frames", CanonicalEventFrames},
      {"messages", "startup-ready-pid", StartupReadyPid},
      {"vectors", "canonical-worker-runtime-frames", CanonicalWorkerRuntimeFrames},
      {"messages", "worker-runtime-rejects", WorkerRuntimeRejects},
      {"protocol", "sha256-self-test", Sha256SelfTest},
      {"protocol", "header-round-trip", HeaderRoundTrip},
      {"protocol", "magic-reject", MagicReject},
      {"protocol", "truncation-every-prefix", TruncationEveryPrefix},
      {"protocol", "payload-length-bounds", PayloadLengthBounds},
      {"protocol", "exact-frame", ExactFrame},
      {"protocol", "reserved-flags", ReservedFlags},
      {"protocol", "reader-bounds", ReaderBounds},
      {"protocol", "writer-reader-round-trip", WriterReaderRoundTrip},
      {"protocol", "string-reader-valid", StringReaderValid},
      {"protocol", "string-reader-reject", StringReaderReject},
      {"messages", "hello-round-trip", MessagesHelloRoundTrip},
      {"messages", "hello-ack-round-trip", MessagesHelloAckRoundTrip},
      {"messages", "ack-round-trip", MessagesAckRoundTrip},
      {"messages", "error-round-trip", MessagesErrorRoundTrip},
      {"messages", "reject-bad-prologue", MessagesRejectBadPrologue},
      {"messages", "reject-framing", MessagesRejectFraming},
      {"messages", "reject-error-string", MessagesRejectErrorString},
      {"messages", "builders-stamp-type", MessagesBuildersStampType},
      {"messages", "correlation", MessagesCorrelation},
      {"messages", "relay-payload", RelayPayloadCodec},
      {"messages", "result-payload", ResultPayloadCodec},
      {"session", "round-trip", SessionRoundTrip},
      {"session", "reject-counts", SessionRejectCounts},
      {"session", "reject-lying-count", SessionRejectLyingCount},
      {"session", "reject-bad-bool", SessionRejectBadBool},
      {"session", "reject-string-limit", SessionRejectStringLimit},
      {"session", "reject-bad-utf8", SessionRejectBadUtf8},
      {"session", "reject-total-size", SessionRejectTotalSize},
      {"session", "reject-fixed-size", SessionRejectFixedSize},
      {"session", "forward-compat", SessionForwardCompat},
      {"session", "reject-trailing-short", SessionRejectTrailingShort},
      {"session", "reject-cross-field", SessionRejectCrossField},
      {"session", "near-max-frame", SessionNearMaxFrame},
      {"run", "round-trip", RunRoundTrip},
      {"run", "reject-engine-filename", RunRejectEngineFilename},
      {"run", "reject-engine-required-empty", RunRejectEngineRequiredEmpty},
      {"run", "reject-snapshot", RunRejectSnapshot},
      {"run", "reject-strings", RunRejectStrings},
      {"run", "reject-framing", RunRejectFraming},
      {"run", "reject-lying-guest-len", RunRejectLyingGuestLen},
      {"run", "forward-compat", RunForwardCompat},
      {"run", "guest-ceiling", RunGuestCeiling},
      {"negotiate", "wire2-version-set", Wire2VersionSet},
      {"negotiate", "equal-minor", NegotiateEqualMinor},
      {"negotiate", "newer-broker", NegotiateNewerBroker},
      {"negotiate", "newer-client", NegotiateNewerClient},
      {"negotiate", "major-mismatch", NegotiateMajorMismatch},
      {"cache", "fresh-record-replay", CacheFreshRecordReplay},
      {"cache", "mismatched-payload", CacheMismatchedPayload},
      {"cache", "mismatched-identity", CacheMismatchedIdentity},
      {"cache", "fingerprint-sensitive", CacheFingerprintSensitive},
      {"cache", "eviction-stale", CacheEvictionStale},
  };

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--vectors-only") == 0) {
      // One deterministic digest line over all canonical vectors (fixed order),
      // then the self-check against the frozen cross-arch contract, then the
      // vectors suite so a byte regression fails here too. The digest doubles as
      // the cross-arch gate (same bytes on x86/x64/arm64 -> same digest), so a
      // mismatch fails the run on whatever arch it executes.
      const std::string digest = Sha256Hex(CanonicalConcat());
      std::printf("vectors-sha256=%s\n", digest.c_str());
      const bool digest_ok = digest == kExpectedVectorsSha256;
      if (!digest_ok)
        std::printf(
            "vectors-sha256 FAIL: expected %s (frozen cross-arch canonical-"
            "vector contract); a differing digest means a wire/byte regression "
            "or a cross-arch divergence\n",
            kExpectedVectorsSha256);
      std::fflush(stdout);
      char suite_arg[] = "--suite=vectors";
      char* synthetic[] = {argv[0], suite_arg};
      const int suite_result = v8host::test::RunTests(2, synthetic, tests);
      return (digest_ok && suite_result == 0) ? 0 : 1;
    }
  }

  // `--all` (or no filter) runs everything; RunTests also honors --suite/--case.
  return v8host::test::RunTests(argc, argv, tests);
}
