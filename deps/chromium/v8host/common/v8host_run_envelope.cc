// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "v8host_run_envelope.h"

#include <utility>

namespace v8host {
namespace {

using protocol::Reader;
using protocol::Writer;

bool KnownType(uint16_t raw, RunEnvelopeType* out) {
  switch (static_cast<RunEnvelopeType>(raw)) {
    case RunEnvelopeType::kStart:
    case RunEnvelopeType::kRelay:
    case RunEnvelopeType::kCancel:
    case RunEnvelopeType::kResult:
    case RunEnvelopeType::kRunError:
    case RunEnvelopeType::kCreateRuntime:
    case RunEnvelopeType::kRuntimeReady:
    case RunEnvelopeType::kCloseRuntime:
    case RunEnvelopeType::kRuntimeClosed:
    case RunEnvelopeType::kCredit:
      *out = static_cast<RunEnvelopeType>(raw);
      return true;
  }
  return false;
}

bool IsRuntimeControl(RunEnvelopeType type) {
  return type >= RunEnvelopeType::kCreateRuntime;
}

}  // namespace

bool IsRunEnvelope(const uint8_t* data, size_t size) {
  if (data == nullptr || size < sizeof(uint32_t))
    return false;
  const uint32_t magic = static_cast<uint32_t>(data[0]) |
                         (static_cast<uint32_t>(data[1]) << 8) |
                         (static_cast<uint32_t>(data[2]) << 16) |
                         (static_cast<uint32_t>(data[3]) << 24);
  return magic == kRunEnvelopeMagic;
}

std::vector<uint8_t> EncodeRunEnvelope(const RunEnvelope& env) {
  Writer writer;
  writer.PutU32(kRunEnvelopeMagic);
  writer.PutU16(static_cast<uint16_t>(env.type));
  writer.PutU16(env.slot);
  writer.PutU32(env.run_id);
  switch (env.type) {
    case RunEnvelopeType::kStart:
      writer.PutBytes(env.payload);
      break;
    case RunEnvelopeType::kRelay:
      writer.PutU32(static_cast<uint32_t>(env.relay_kind));
      writer.PutBytes(env.payload);
      break;
    case RunEnvelopeType::kCancel:
    case RunEnvelopeType::kRuntimeReady:
    case RunEnvelopeType::kCloseRuntime:
      break;
    case RunEnvelopeType::kResult:
      writer.PutU32(static_cast<uint32_t>(env.disposition));
      break;
    case RunEnvelopeType::kRunError:
      writer.PutU32(env.status_code);
      writer.PutString(env.message);
      break;
    case RunEnvelopeType::kCreateRuntime:
      writer.PutU32(static_cast<uint32_t>(env.snapshot_index));
      break;
    case RunEnvelopeType::kRuntimeClosed:
      writer.PutU32(env.status_code);
      break;
    case RunEnvelopeType::kCredit:
      writer.PutU32(env.credit);
      break;
  }
  return writer.buffer();
}

bool DecodeRunEnvelope(const uint8_t* data, size_t size, RunEnvelope* out) {
  if (data == nullptr || size > protocol::kMaxFramePayload)
    return false;
  Reader reader(data, size);
  uint32_t magic = 0;
  uint16_t type_raw = 0;
  RunEnvelope env;
  if (!reader.GetU32(&magic) || magic != kRunEnvelopeMagic)
    return false;
  if (!reader.GetU16(&type_raw) || !KnownType(type_raw, &env.type))
    return false;
  if (!reader.GetU16(&env.slot) || env.slot >= kMaxRuntimeSlots)
    return false;
  if (!reader.GetU32(&env.run_id))
    return false;
  if (IsRuntimeControl(env.type) && env.run_id != 0)
    return false;
  switch (env.type) {
    case RunEnvelopeType::kStart:
      if (!reader.GetBytes(reader.BytesRemaining(), &env.payload))
        return false;
      break;
    case RunEnvelopeType::kRelay: {
      uint32_t kind = 0;
      if (!reader.GetU32(&kind))
        return false;
      env.relay_kind = static_cast<int32_t>(kind);
      if (!reader.GetBytes(reader.BytesRemaining(), &env.payload))
        return false;
      break;
    }
    case RunEnvelopeType::kCancel:
    case RunEnvelopeType::kRuntimeReady:
    case RunEnvelopeType::kCloseRuntime:
      break;
    case RunEnvelopeType::kResult: {
      uint32_t disposition = 0;
      if (!reader.GetU32(&disposition))
        return false;
      if (disposition !=
              static_cast<uint32_t>(RunEnvelopeDisposition::kCompleted) &&
          disposition !=
              static_cast<uint32_t>(RunEnvelopeDisposition::kCancelled))
        return false;  // fail-closed: unknown disposition
      env.disposition = static_cast<RunEnvelopeDisposition>(disposition);
      break;
    }
    case RunEnvelopeType::kRunError:
      if (!reader.GetU32(&env.status_code))
        return false;
      if (!reader.GetString(&env.message))  // strict UTF-8 / no NUL / bounded
        return false;
      break;
    case RunEnvelopeType::kCreateRuntime: {
      uint32_t index = 0;
      if (!reader.GetU32(&index))
        return false;
      env.snapshot_index = static_cast<int32_t>(index);
      if (env.snapshot_index < -1 || env.snapshot_index >= kMaxRuntimeSnapshots)
        return false;
      break;
    }
    case RunEnvelopeType::kRuntimeClosed:
      if (!reader.GetU32(&env.status_code) ||
          (env.status_code != 0 && env.status_code != kRuntimeClosedFailed))
        return false;
      break;
    case RunEnvelopeType::kCredit:
      if (!reader.GetU32(&env.credit) || env.credit == 0 ||
          env.credit > kRelayCreditWindow)
        return false;
      break;
  }
  if (!reader.AtEnd())
    return false;
  *out = std::move(env);
  return true;
}

}  // namespace v8host
