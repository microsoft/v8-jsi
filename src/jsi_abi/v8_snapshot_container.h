// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// Single source of truth for the startup-snapshot blob CONTAINER format. A
// container is a small fixed-width header followed by the raw v8::StartupData
// bytes. The header lets a stale or cross-engine blob be REJECTED instead of
// fed to V8 and crashing: a V8 startup blob is only loadable by an engine built
// with the identical V8 version and cage / pointer-compression / lite flags, and
// V8 has no graceful failure for a mismatched blob.
//
// Used on BOTH the write path (v8_create_startup_snapshot) and the read paths
// (v8_startup_snapshot_compatible + v8_core's createIsolate), so the format is
// defined in exactly one place. Engine-internal — do NOT include from a public
// header (it pulls in v8.h). Public consumers validate via the
// v8_startup_snapshot_compatible C export, never by parsing this directly.
//
// IMPORTANT: v8::ScriptCompiler::CachedDataVersionTag() folds in V8's flag hash,
// which is only final after v8::V8::Initialize() enforces flag implications
// (e.g. --jitless implies turbofan off). Every site that reads the tag must do
// so AFTER initialization, or a same-engine blob can spuriously mismatch. The
// create path reads it post-CreateBlob (the SnapshotCreator isolate has been
// initialized); the consume paths must ensure V8 is initialized first.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "v8.h"

namespace v8rt_snapshot {

// Fixed 24-byte header prefixed to the raw v8::StartupData. All v8-jsi RIDs are
// little-endian (x64 / x86 / arm64 Windows), so multi-byte fields are stored in
// host order via memcpy.
//
//   offset size field
//   0      4    magic = "V8SN"
//   4      4    format_version (uint32)
//   8      4    cached_data_version (uint32, v8 CachedDataVersionTag)
//   12     4    engine_flags (uint32 bitset of compile-time V8 build flags)
//   16     8    blob_size (uint64, bytes of StartupData that follow)
//   24     ...  raw StartupData bytes
inline constexpr size_t kHeaderSize = 24;
inline constexpr uint32_t kFormatVersion = 1;
inline constexpr uint8_t kMagic[4] = {'V', '8', 'S', 'N'};

// Compile-time identity of the V8 build. The blob is only loadable by an engine
// with the same value. These macros reach v8jsi's own sources via common.gypi
// (see src/v8jsi.gyp) and separate v8jsi.dll (cage off) from v8jsisb.dll (cage +
// shared-cage + external-code-space on). Computed identically on the write and
// validate paths, so identical builds always agree and differently-configured
// engines always differ — independent of runtime flags or init state.
inline uint32_t currentEngineFlags() {
  uint32_t f = 0;
#ifdef V8_ENABLE_SANDBOX
  f |= 1u << 0;
#endif
#ifdef V8_COMPRESS_POINTERS
  f |= 1u << 1;
#endif
#ifdef V8_COMPRESS_POINTERS_IN_SHARED_CAGE
  f |= 1u << 2;
#endif
#ifdef V8_EXTERNAL_CODE_SPACE
  f |= 1u << 3;
#endif
  return f;
}

inline uint32_t currentCachedDataVersion() {
  return v8::ScriptCompiler::CachedDataVersionTag();
}

// Reason codes — kept in sync with v8_snapshot_compat_code in v8_jsi_config.h
// (the public mirror consumers see).
enum Compat {
  kOk = 0,
  kTruncated = 1,       // smaller than the container header
  kBadMagic = 2,        // not a v8jsi snapshot container
  kBadFormat = 3,       // unknown container format version
  kVersionMismatch = 4, // CachedDataVersionTag differs
  kFlagsMismatch = 5,   // engine build flags differ (cage / ptr-compression)
  kSizeMismatch = 6,    // header blob_size != actual trailing bytes
};

inline uint32_t readU32(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline uint64_t readU64(const uint8_t *p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// Write the 24-byte header into dst (caller guarantees >= kHeaderSize bytes).
// Must be called AFTER V8 init so currentCachedDataVersion() is final.
inline void writeHeader(uint8_t *dst, uint64_t blob_size) {
  const uint32_t fmt = kFormatVersion;
  const uint32_t cdv = currentCachedDataVersion();
  const uint32_t flg = currentEngineFlags();
  std::memcpy(dst + 0, kMagic, 4);
  std::memcpy(dst + 4, &fmt, 4);
  std::memcpy(dst + 8, &cdv, 4);
  std::memcpy(dst + 12, &flg, 4);
  std::memcpy(dst + 16, &blob_size, 8);
}

// Validate a container against THIS engine. Returns kOk or a reason code. Must
// be called AFTER V8 init (see file header) so the version tag is final.
inline int validate(const uint8_t *data, size_t size) {
  if (!data || size < kHeaderSize)
    return kTruncated;
  if (std::memcmp(data, kMagic, 4) != 0)
    return kBadMagic;
  if (readU32(data + 4) != kFormatVersion)
    return kBadFormat;
  if (readU32(data + 8) != currentCachedDataVersion())
    return kVersionMismatch;
  if (readU32(data + 12) != currentEngineFlags())
    return kFlagsMismatch;
  if (readU64(data + 16) != static_cast<uint64_t>(size - kHeaderSize))
    return kSizeMismatch;
  return kOk;
}

// Inner raw-StartupData view (only valid once validate() returned kOk).
inline const uint8_t *blobData(const uint8_t *container) {
  return container + kHeaderSize;
}
inline size_t blobSize(size_t container_size) {
  return container_size - kHeaderSize;
}

inline const char *compatString(int code) {
  switch (code) {
    case kOk:
      return "compatible";
    case kTruncated:
      return "truncated (smaller than the container header)";
    case kBadMagic:
      return "bad magic (not a v8jsi snapshot container)";
    case kBadFormat:
      return "unsupported container format version";
    case kVersionMismatch:
      return "V8 CachedDataVersionTag mismatch (engine version/flags differ)";
    case kFlagsMismatch:
      return "engine build-flags mismatch (cage / pointer-compression differ)";
    case kSizeMismatch:
      return "blob size mismatch (truncated or corrupt container)";
    default:
      return "unknown";
  }
}

}  // namespace v8rt_snapshot
