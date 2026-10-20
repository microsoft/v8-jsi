// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Contract B wire protocol — framing core: the checked little-endian byte
// reader/writer, the fixed 32-byte frame header, strict UTF-8 string decoding,
// and one-frame-per-message validation. This wire surface is EXPERIMENTAL and
// NOT yet ABI-stable; it may be reshaped between revisions and must not be
// relied on as a stable contract yet.
//
// The protocol crosses processes built for different CPU targets (the client
// may be x86/x64/arm64/arm64ec, the broker the native arch), so every field is
// encoded and decoded as explicit little-endian fixed-width bytes. No Windows
// types, pointers, handles, size_t, or wchar_t ever cross the wire, and the
// codec never casts raw bytes onto a native struct.

#ifndef V8HOST_PROTOCOL_V8HOST_PROTOCOL_H_
#define V8HOST_PROTOCOL_V8HOST_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A translation unit that includes <windows.h> before this header pulls in
// wingdi.h's object-like `ERROR` macro (value 0), which would otherwise corrupt
// the scoped MessageType::ERROR enumerator below. This header owns no Windows
// types and the GDI ERROR constant is unused in this codebase, so neutralize
// just that macro here (the vendored abseil and protobuf do the same). The
// winerror.h `ERROR_INVALID_STATE` code is deliberately left intact — the
// matching status code is named ERROR_BAD_STATE to avoid shadowing it.
#ifdef ERROR
#undef ERROR
#endif

namespace v8host::protocol {

// Frame magic: the bytes 'V','8','H','W' read little-endian.
inline constexpr uint32_t kWireMagic = 0x57483856u;

inline constexpr uint16_t kWireVersionMajor = 2;
inline constexpr uint16_t kWireVersionMinor = 0;

// Fixed 32-byte header; payload follows inline. The max payload equals the
// broker<->worker channel slot (msg_channel.h kMaxPayload), so every frame
// shares this ceiling and a relay frame always fits a worker slot.
inline constexpr uint32_t kFrameHeaderSize = 32;
inline constexpr uint32_t kMaxFramePayload = 65528;
inline constexpr uint32_t kMaxFrameSize = kFrameHeaderSize + kMaxFramePayload;  // 65560

// Frame flags (16-bit field). bit0 = MUST_UNDERSTAND; every other bit is
// reserved and must be zero in version 2. kFlagsMask makes reserved-bit
// rejection explicit (any bit outside the mask is rejected by the validator).
inline constexpr uint16_t kFlagMustUnderstand = 0x0001;
inline constexpr uint16_t kFlagsMask = kFlagMustUnderstand;

// Frozen Contract B quotas. Enforcement lives in later message/coordinator
// slices; the constants are defined now so every layer shares one source.
inline constexpr uint32_t kMaxFileRules = 64;
inline constexpr uint32_t kMaxCapabilities = 64;
inline constexpr uint32_t kMaxSessionsPerConnection = 32;
inline constexpr uint32_t kMaxInFlightRunsPerSession = 4;
inline constexpr uint32_t kMaxQueuedRelayBytesPerRun = 1u << 20;         // 1 MiB
inline constexpr uint32_t kMaxQueuedRelayBytesPerConnection = 16u << 20;  // 16 MiB
inline constexpr uint32_t kMaxControlQueueRequests = 256;
inline constexpr uint32_t kMaxControlQueueBytes = 1u << 20;              // 1 MiB
inline constexpr uint32_t kDuplicateCacheSize = 256;
inline constexpr uint32_t kMaxStringBytes = 32u * 1024;                  // 32 KiB

// Contract B message types. Numbering is stable and banded by role so the kind
// is obvious on the wire: control = 0x0001.., events = 0x0100.., relay =
// 0x0200... Values are frozen; append new types to the end of a band.
enum class MessageType : uint16_t {
  // Control (broker-interpreted).
  HELLO = 0x0001,
  HELLO_ACK = 0x0002,
  CREATE_SESSION = 0x0003,
  SESSION_READY = 0x0004,
  START_RUN = 0x0005,
  CANCEL_RUN = 0x0006,
  CLOSE_SESSION = 0x0007,
  ACK = 0x0008,
  ERROR = 0x0009,
  WORKER_EXIT = 0x000A,
  CREATE_WORKER = 0x000B,
  CLOSE_WORKER = 0x000C,
  CREATE_RUNTIME = 0x000D,
  CLOSE_RUNTIME = 0x000E,

  // Events (broker -> client).
  STARTUP_READY = 0x0100,
  SECURITY_READY = 0x0101,
  RESULT = 0x0102,
  RUN_ERROR = 0x0103,
  RUNTIME_READY = 0x0104,
  RUNTIME_CLOSED = 0x0105,

  // Relay (opaque passthrough).
  RELAY_TO_WORKER = 0x0200,
  RELAY_FROM_WORKER = 0x0201,
};

// Status/error codes carried by ACK/ERROR payloads. Message-layer slices use
// these; the framing validator here reports DecodeStatus instead. Values are
// explicit and stable. ERROR_BAD_STATE is the design's "invalid state" code:
// the natural name ERROR_INVALID_STATE is a <winerror.h> macro, so a distinct
// name avoids shadowing that GetLastError value in Windows consumers.
enum class StatusCode : uint32_t {
  OK = 0,
  ERROR_PROTOCOL = 1,             // malformed framing/encoding
  ERROR_VERSION = 2,              // unsupported wire major version
  ERROR_UNSUPPORTED_MESSAGE = 3,  // unknown type with MUST_UNDERSTAND
  ERROR_STALE_REQUEST = 4,        // request id evicted from the duplicate cache
  ERROR_QUOTA = 5,                // a frozen quota was exceeded
  ERROR_BAD_STATE = 6,            // impossible state or object reference
  ERROR_INTERNAL = 7,             // unexpected internal failure
  ERROR_PROFILE_ALREADY_BOUND = 8,  // a later run's profile conflicts
  ERROR_RUNTIME_FAILED = 9,       // runtime or thread creation failed; RUNTIME_CLOSED only
};

// Decoded form of the 32-byte frame header. This is NOT a wire-layout struct;
// bytes are never cast onto it. EncodeHeader / DecodeAndValidateFrame translate
// between these typed fields and the explicit little-endian layout:
//
//   offset size field
//   0      4    magic = 0x57483856   (little-endian)
//   4      2    version_major
//   6      2    version_minor
//   8      2    type
//   10     2    flags (bit0 = MUST_UNDERSTAND)
//   12     4    conn_id
//   16     4    target_id
//   20     4    run_id
//   24     4    request_id
//   28     4    payload_length
//   32     N    payload
struct FrameHeader {
  uint16_t version_major = 0;
  uint16_t version_minor = 0;
  MessageType type = MessageType::HELLO;
  uint16_t flags = 0;
  uint32_t conn_id = 0;
  uint32_t target_id = 0;
  uint32_t run_id = 0;
  uint32_t request_id = 0;
  uint32_t payload_length = 0;
};

// Result of DecodeAndValidateFrame. Any non-kOk result means the caller closes
// the connection fail-closed (never resynchronize by scanning for magic); on
// failure no output is written and no state is mutated.
enum class DecodeStatus : uint32_t {
  kOk = 0,
  kIncompleteHeader = 1,  // fewer than kFrameHeaderSize bytes
  kBadMagic = 2,          // magic != kWireMagic
  kOversizePayload = 3,   // payload_length > kMaxFramePayload
  kLengthMismatch = 4,    // kFrameHeaderSize + payload_length != size
  kReservedFlags = 5,     // a flag bit outside kFlagsMask is set
};

// Append-only little-endian byte writer over an owned buffer. Multi-byte
// integers are written byte-by-byte so the output is identical regardless of
// host endianness.
class Writer {
 public:
  void PutU16(uint16_t value);
  void PutU32(uint32_t value);
  void PutBytes(const uint8_t* data, size_t size);
  void PutBytes(const std::vector<uint8_t>& bytes);
  // Writes a u32 byte length followed by the raw bytes (no terminator). The
  // caller supplies valid UTF-8 without NUL; the Reader enforces that on the
  // way back in.
  void PutString(const std::string& value);

  const std::vector<uint8_t>& buffer() const { return buffer_; }
  std::vector<uint8_t>& buffer() { return buffer_; }
  size_t size() const { return buffer_.size(); }

 private:
  std::vector<uint8_t> buffer_;
};

// Bounds-checked little-endian byte reader. Every accessor validates the
// remaining length and returns false (leaving the offset unchanged) rather than
// reading out of bounds; nothing throws.
class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  explicit Reader(const std::vector<uint8_t>& bytes)
      : data_(bytes.data()), size_(bytes.size()) {}

  bool GetU16(uint16_t* out);
  bool GetU32(uint32_t* out);
  bool GetBytes(size_t count, std::vector<uint8_t>* out);
  // Reads a u32 byte length then that many bytes, enforcing length <=
  // kMaxStringBytes and <= remaining, strict UTF-8, and no embedded NUL. On any
  // failure the offset is unchanged.
  bool GetString(std::string* out);
  // Advances the offset by `n` bytes without reading them, used to skip the
  // bounded forward-compat fields a future minor version may append to a fixed
  // section. Returns false (offset unchanged) if n > BytesRemaining().
  bool Skip(size_t n);

  size_t offset() const { return offset_; }
  size_t size() const { return size_; }
  size_t BytesRemaining() const { return size_ - offset_; }
  bool AtEnd() const { return offset_ == size_; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t offset_ = 0;
};

// Strict UTF-8 well-formedness check (Unicode Table 3-7): rejects overlong
// encodings, UTF-16 surrogates, code points above U+10FFFF, truncated
// sequences, and (per Contract B) any embedded NUL.
bool ValidateUtf8NoNul(const uint8_t* data, size_t size);

// Appends exactly kFrameHeaderSize little-endian bytes for `header`. The caller
// appends the payload after this and must set header.payload_length to its size.
void EncodeHeader(const FrameHeader& header, std::vector<uint8_t>& out);

// Decodes and frames exactly one message. In order, and before any allocation
// keyed on payload_length: require size >= kFrameHeaderSize; check magic; decode
// fields; require payload_length <= kMaxFramePayload; prove (checked)
// kFrameHeaderSize + payload_length does not overflow and equals `size` exactly;
// reject reserved flag bits. On kOk, *out_header is filled and (when non-null)
// *out_payload / *out_payload_size describe the payload view into `data`
// (out_payload is null for an empty payload). The message `type` is NOT
// validated here, and version / MUST_UNDERSTAND policy is a message-layer
// concern; this validator only decodes and frames. Writes no output on failure.
DecodeStatus DecodeAndValidateFrame(const uint8_t* data,
                                    size_t size,
                                    FrameHeader* out_header,
                                    const uint8_t** out_payload = nullptr,
                                    size_t* out_payload_size = nullptr);

}  // namespace v8host::protocol

#endif  // V8HOST_PROTOCOL_V8HOST_PROTOCOL_H_
