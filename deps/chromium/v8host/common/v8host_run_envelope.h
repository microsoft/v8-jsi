// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Run-envelope codec — the bounded, neutral little-endian framing that
// multiplexes per-run and per-runtime control over the generic worker channel's
// opaque string/binary frames:
//
//   broker -> worker : START (guest bytes), RELAY (opaque), CANCEL,
//                      CREATE_RUNTIME, CLOSE_RUNTIME
//   worker -> broker : RELAY (opaque), RESULT (disposition), RUN_ERROR,
//                      RUNTIME_READY, RUNTIME_CLOSED, CREDIT
//
// Every envelope names its worker-local runtime slot.
//
// The generic container ferries these as opaque bytes without interpreting them,
// so layering stays clean: only the coordinator (builds inbound) and the engine
// (builds outbound) understand the envelope. A magic prefix distinguishes an
// envelope from a bare dev-ambient relay (the `sbox.exe --broker` smoke posts a
// raw string with no envelope), so the engine run loop can serve both the
// coordinator-driven and dev-ambient paths.
//
// This codec is NEUTRAL (no pipe/process/token/policy/auth type, no
// client/coordinator/engine symbol) and, like v8host_spawn_config, crosses every
// field as explicit little-endian bytes via the Contract B Writer/Reader, so the
// bytes are identical regardless of host arch. The struct below is the DECODED
// form only. This surface is EXPERIMENTAL and NOT yet ABI-stable.

#ifndef V8HOST_COMMON_V8HOST_RUN_ENVELOPE_H_
#define V8HOST_COMMON_V8HOST_RUN_ENVELOPE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "v8host_protocol.h"  // Writer/Reader, kMaxFramePayload

namespace v8host {

// Envelope magic ("V8RE" little-endian): a run envelope begins with these four
// bytes; a bare dev-ambient relay frame does not, so the engine distinguishes
// them by prefix before a full decode.
inline constexpr uint32_t kRunEnvelopeMagic = 0x45523856u;  // 'V','8','R','E'

// Fixed envelope header: magic(4) | type(2) | slot(2) | run_id(4) = 12 bytes.
// `slot` is the worker-local runtime slot (0..15); a session's runtime is slot
// 0, so session envelopes keep their bytes.
inline constexpr uint32_t kRunEnvelopeHeaderSize = 12;

// Runtime slots per worker, and the relay window each runtime grants.
inline constexpr uint16_t kMaxRuntimeSlots = 16;
inline constexpr uint32_t kRelayCreditWindow = 8;
// The snapshots a worker may preload (CREATE_RUNTIME picks one by index).
inline constexpr int32_t kMaxRuntimeSnapshots = 8;
// RUNTIME_CLOSED status for a runtime the engine could not create or run
// (the Contract B ERROR_RUNTIME_FAILED value).
inline constexpr uint32_t kRuntimeClosedFailed = 9;

// env_type values (frozen; directional per design §10.2).
enum class RunEnvelopeType : uint16_t {
  kStart = 1,     // broker->worker: begin a run with guest bytes
  kRelay = 2,     // both directions: opaque guest relay (i32 kind | bytes)
  kCancel = 3,    // broker->worker: cancel the active run
  kResult = 4,    // worker->broker: run terminal disposition
  kRunError = 5,  // worker->broker: run failed (status + redacted message)
  // Runtime control; run_id is always 0.
  kCreateRuntime = 6,  // broker->worker: create the slot's runtime (i32 snapshot index, -1 = none)
  kRuntimeReady = 7,   // worker->broker: the slot's runtime is ready
  kCloseRuntime = 8,   // broker->worker: close the slot's runtime
  kRuntimeClosed = 9,  // worker->broker: the slot is free (u32 status: 0 ok, else kRuntimeClosedFailed)
  kCredit = 10,        // worker->broker: relays consumed (u32 count, 1..kRelayCreditWindow)
};

// RESULT disposition (matches Contract B RESULT / Contract C run events).
enum class RunEnvelopeDisposition : uint32_t {
  kCompleted = 0,
  kCancelled = 1,
};

// The decoded envelope. `slot` is meaningful for every type. Only the field(s)
// relevant to `type` are meaningful otherwise:
//   kStart         -> run_id, payload (guest bytes, possibly empty)
//   kRelay         -> run_id, relay_kind, payload (opaque relay bytes)
//   kCancel        -> run_id
//   kResult        -> run_id, disposition
//   kRunError      -> run_id, status_code, message
//   kCreateRuntime -> snapshot_index
//   kRuntimeReady, kCloseRuntime -> (nothing)
//   kRuntimeClosed -> status_code
//   kCredit        -> credit
struct RunEnvelope {
  RunEnvelopeType type = RunEnvelopeType::kStart;
  uint32_t run_id = 0;
  int32_t relay_kind = 0;
  RunEnvelopeDisposition disposition = RunEnvelopeDisposition::kCompleted;
  uint32_t status_code = 0;
  std::string message;           // kRunError (bounded, redacted, strict UTF-8)
  std::vector<uint8_t> payload;  // kStart guest bytes / kRelay relay bytes
  uint16_t slot = 0;
  int32_t snapshot_index = -1;
  uint32_t credit = 0;
};

// True iff `data` begins with the envelope magic (so the engine can branch
// before a full decode). A short/null buffer returns false.
bool IsRunEnvelope(const uint8_t* data, size_t size);

// Encode `env` to a complete envelope frame.
std::vector<uint8_t> EncodeRunEnvelope(const RunEnvelope& env);

// Decode a complete envelope frame. Validates the magic, a known env_type, a
// slot below kMaxRuntimeSlots, run_id == 0 for the runtime-control types, the
// per-type body and its bounds, strict UTF-8 on any message, the total size
// (<= kMaxFramePayload), and exact consumption. Returns false on any failure.
bool DecodeRunEnvelope(const uint8_t* data, size_t size, RunEnvelope* out);

}  // namespace v8host

#endif  // V8HOST_COMMON_V8HOST_RUN_ENVELOPE_H_
