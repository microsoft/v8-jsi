// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "v8host_protocol.h"

// Contract B framing core. Every multi-byte integer is read and written
// byte-by-byte in little-endian order so the bytes are identical on any host
// arch; no multi-byte integer is memcpy'd out of the buffer (that would be
// endianness-dependent and could alias). Exception-free: no throw/try — each
// accessor reports success through its return value.

namespace v8host::protocol {

namespace {

uint16_t ReadU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                               static_cast<uint16_t>(p[1] << 8));
}

uint32_t ReadU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

void Writer::PutU16(uint16_t value) {
  buffer_.push_back(static_cast<uint8_t>(value & 0xFF));
  buffer_.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void Writer::PutU32(uint32_t value) {
  buffer_.push_back(static_cast<uint8_t>(value & 0xFF));
  buffer_.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  buffer_.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  buffer_.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void Writer::PutBytes(const uint8_t* data, size_t size) {
  buffer_.insert(buffer_.end(), data, data + size);
}

void Writer::PutBytes(const std::vector<uint8_t>& bytes) {
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

void Writer::PutString(const std::string& value) {
  PutU32(static_cast<uint32_t>(value.size()));
  PutBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

bool Reader::GetU16(uint16_t* out) {
  if (size_ - offset_ < 2)
    return false;
  *out = ReadU16LE(data_ + offset_);
  offset_ += 2;
  return true;
}

bool Reader::GetU32(uint32_t* out) {
  if (size_ - offset_ < 4)
    return false;
  *out = ReadU32LE(data_ + offset_);
  offset_ += 4;
  return true;
}

bool Reader::GetBytes(size_t count, std::vector<uint8_t>* out) {
  if (size_ - offset_ < count)
    return false;
  out->assign(data_ + offset_, data_ + offset_ + count);
  offset_ += count;
  return true;
}

bool Reader::GetString(std::string* out) {
  // Read the length prefix without committing the offset until every check
  // passes, so a failed GetString mutates no reader state.
  if (size_ - offset_ < 4)
    return false;
  const uint32_t length = ReadU32LE(data_ + offset_);
  if (length > kMaxStringBytes)
    return false;
  if (length > size_ - offset_ - 4)
    return false;
  const uint8_t* bytes = data_ + offset_ + 4;
  if (!ValidateUtf8NoNul(bytes, length))
    return false;
  out->assign(reinterpret_cast<const char*>(bytes), length);
  offset_ += 4 + static_cast<size_t>(length);
  return true;
}

bool Reader::Skip(size_t n) {
  if (size_ - offset_ < n)
    return false;
  offset_ += n;
  return true;
}

bool ValidateUtf8NoNul(const uint8_t* data, size_t size) {
  size_t i = 0;
  while (i < size) {
    const uint8_t b0 = data[i];
    if (b0 == 0x00)
      return false;  // embedded NUL rejected even though it is valid UTF-8
    if (b0 < 0x80) {
      i += 1;
      continue;
    }

    // Classify the lead byte and the valid range of the FIRST continuation
    // byte (Unicode Table 3-7). The narrowed lower/upper bounds reject overlong
    // forms, UTF-16 surrogates, and code points above U+10FFFF.
    size_t extra = 0;
    uint8_t lower = 0x80;
    uint8_t upper = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
      extra = 1;
    } else if (b0 == 0xE0) {
      extra = 2;
      lower = 0xA0;  // reject overlong 3-byte (U+0000..U+07FF)
    } else if (b0 >= 0xE1 && b0 <= 0xEC) {
      extra = 2;
    } else if (b0 == 0xED) {
      extra = 2;
      upper = 0x9F;  // reject surrogates (U+D800..U+DFFF)
    } else if (b0 >= 0xEE && b0 <= 0xEF) {
      extra = 2;
    } else if (b0 == 0xF0) {
      extra = 3;
      lower = 0x90;  // reject overlong 4-byte (U+0000..U+FFFF)
    } else if (b0 >= 0xF1 && b0 <= 0xF3) {
      extra = 3;
    } else if (b0 == 0xF4) {
      extra = 3;
      upper = 0x8F;  // reject code points above U+10FFFF
    } else {
      return false;  // 0xC0/0xC1, a bare continuation byte, or 0xF5..0xFF
    }

    if (i + extra >= size)
      return false;  // truncated multi-byte sequence
    if (data[i + 1] < lower || data[i + 1] > upper)
      return false;
    for (size_t k = 2; k <= extra; ++k) {
      if (data[i + k] < 0x80 || data[i + k] > 0xBF)
        return false;
    }
    i += extra + 1;
  }
  return true;
}

void EncodeHeader(const FrameHeader& header, std::vector<uint8_t>& out) {
  Writer writer;
  writer.PutU32(kWireMagic);
  writer.PutU16(header.version_major);
  writer.PutU16(header.version_minor);
  writer.PutU16(static_cast<uint16_t>(header.type));
  writer.PutU16(header.flags);
  writer.PutU32(header.conn_id);
  writer.PutU32(header.target_id);
  writer.PutU32(header.run_id);
  writer.PutU32(header.request_id);
  writer.PutU32(header.payload_length);
  out.insert(out.end(), writer.buffer().begin(), writer.buffer().end());
}

DecodeStatus DecodeAndValidateFrame(const uint8_t* data,
                                    size_t size,
                                    FrameHeader* out_header,
                                    const uint8_t** out_payload,
                                    size_t* out_payload_size) {
  // 1. A full fixed header must be present before any field is touched.
  if (size < kFrameHeaderSize)
    return DecodeStatus::kIncompleteHeader;

  // 2. Magic (little-endian), read before trusting any other field.
  if (ReadU32LE(data) != kWireMagic)
    return DecodeStatus::kBadMagic;

  // 3. Decode the typed fields (all little-endian). The `type` is cast through
  //    without validation; unknown types are a message-layer concern.
  FrameHeader header;
  header.version_major = ReadU16LE(data + 4);
  header.version_minor = ReadU16LE(data + 6);
  header.type = static_cast<MessageType>(ReadU16LE(data + 8));
  header.flags = ReadU16LE(data + 10);
  header.conn_id = ReadU32LE(data + 12);
  header.target_id = ReadU32LE(data + 16);
  header.run_id = ReadU32LE(data + 20);
  header.request_id = ReadU32LE(data + 24);
  header.payload_length = ReadU32LE(data + 28);

  // 4. Bound the declared payload before any length-keyed arithmetic.
  if (header.payload_length > kMaxFramePayload)
    return DecodeStatus::kOversizePayload;

  // 5. Prove kFrameHeaderSize + payload_length does not overflow and is exactly
  //    the message size. A pipe message carrying fewer or more bytes than one
  //    declared frame is rejected, never resynchronized. (The 64-bit sum cannot
  //    overflow given step 4, but it is computed widely to make that explicit.)
  const uint64_t total =
      static_cast<uint64_t>(kFrameHeaderSize) + header.payload_length;
  if (total != static_cast<uint64_t>(size))
    return DecodeStatus::kLengthMismatch;

  // 6. Reject reserved flag bits for version 2.
  const uint16_t reserved = static_cast<uint16_t>(~kFlagsMask);
  if ((header.flags & reserved) != 0)
    return DecodeStatus::kReservedFlags;

  // 7. Commit outputs only now, after full validation.
  *out_header = header;
  if (out_payload != nullptr)
    *out_payload = header.payload_length ? data + kFrameHeaderSize : nullptr;
  if (out_payload_size != nullptr)
    *out_payload_size = header.payload_length;
  return DecodeStatus::kOk;
}

}  // namespace v8host::protocol
