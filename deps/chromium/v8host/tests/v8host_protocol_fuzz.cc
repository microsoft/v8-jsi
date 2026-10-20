// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Deterministic, in-process fuzz harness for the Contract B protocol codec.
// Deliberately NOT libFuzzer: this GN build defaults use_libfuzzer off and
// gates it to x64+ASan only, so a self-contained deterministic harness is the
// portable, flake-free way to meet the §4.3 acceptance ("the fuzz corpus must
// complete with no crash/leak/unbounded allocation") across x86/x64/arm64.
//
// A seeded PRNG mutates copies of a built-in + on-disk corpus and feeds the
// bytes to every decoder: DecodeAndValidateFrame (then a type-dispatched
// Decode* over the validated payload view), each Decode* directly on the raw
// buffer (so the message codecs are fuzzed even when framing rejects), and the
// bounded Reader. It asserts only robustness — no crash/hang/unbounded
// allocation and a clean bool from every decoder — never a specific
// accept/reject (that is the unit tests' job). Runs are reproducible from
// -seed/-runs so a failure is replayable. Links only :protocol; the tiny bit of
// Win32 here (corpus directory enumeration) keeps the codec library itself
// dependency-free.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // FindFirstFileA for corpus enumeration; must precede the
                      // protocol header, whose #undef ERROR clears wingdi's macro

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "v8host_protocol.h"
#include "v8host_protocol_messages.h"

namespace {

using namespace v8host::protocol;

// Hard cap on a mutated buffer so "no unbounded allocation" is provable: every
// decoder allocates at most O(input), and the input itself is bounded here.
constexpr size_t kMaxFuzzInput = 128 * 1024;

// Self-contained 64-bit PRNG (SplitMix64) — no external dependency, fully
// deterministic from the seed so every run is reproducible.
class Prng {
 public:
  explicit Prng(uint64_t seed) : state_(seed) {}

  uint64_t Next() {
    uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  uint32_t NextU32() { return static_cast<uint32_t>(Next()); }
  uint8_t Byte() { return static_cast<uint8_t>(Next()); }
  // Uniform-ish in [0, n); returns 0 when n == 0.
  size_t Below(size_t n) { return n == 0 ? 0 : static_cast<size_t>(Next() % n); }

 private:
  uint64_t state_;
};

// A canonical frame of every message type plus a few degenerate buffers. These
// mirror the committed corpus files, so the fuzzer still has rich seeds when run
// with no corpus directory argument.
std::vector<std::vector<uint8_t>> BuiltinSeeds() {
  std::vector<std::vector<uint8_t>> seeds;

  {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
    h.request_id = 1;
    seeds.push_back(BuildHelloFrame(h));
  }
  {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
    h.conn_id = 9;
    h.request_id = 1;
    HelloAckPayload p;
    p.broker_version_major = kWireVersionMajor;
    p.broker_version_minor = kWireVersionMinor;
    p.endpoint_mode = kEndpointModeShared;
    p.max_frame_payload = kMaxFramePayload;
    p.max_sessions_per_connection = kMaxSessionsPerConnection;
    p.max_in_flight_runs_per_session = kMaxInFlightRunsPerSession;
    p.max_file_rules = kMaxFileRules;
    p.max_capabilities = kMaxCapabilities;
    p.max_queued_relay_bytes_per_run = kMaxQueuedRelayBytesPerRun;
    p.max_queued_relay_bytes_per_connection = kMaxQueuedRelayBytesPerConnection;
    seeds.push_back(BuildHelloAckFrame(h, p));
  }
  {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
    h.conn_id = 9;
    h.target_id = 2;
    h.run_id = 3;
    h.request_id = 0x2A;
    seeds.push_back(BuildAckFrame(h));
  }
  {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
    h.conn_id = 9;
    h.request_id = 0x2B;
    ErrorPayload p;
    p.status_code = StatusCode::ERROR_QUOTA;
    p.message = "quota";
    seeds.push_back(BuildErrorFrame(h, p));
  }
  {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
    h.conn_id = 9;
    h.request_id = 0x30;
    CreateSessionPayload p;
    p.broker_mode = 1;
    p.tier = 2;
    p.use_app_container = true;
    p.app_container_profile = "profile";
    p.file_rules = {FileRule{true, "r1"}, FileRule{false, "r2"}};
    p.capabilities = {"cap1", "cap2"};
    seeds.push_back(BuildCreateSessionFrame(h, p));
  }
  {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
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
    seeds.push_back(BuildStartRunFrame(h, p));
  }
  {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
    h.flags = kFlagMustUnderstand;
    h.conn_id = 9;
    h.target_id = 2;
    h.run_id = 3;
    h.request_id = 0x32;
    seeds.push_back(BuildCancelRunFrame(h));
    h.run_id = 0;
    h.request_id = 0x33;
    seeds.push_back(BuildCloseSessionFrame(h));
  }
  {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
    h.type = MessageType::RELAY_TO_WORKER;
    h.conn_id = 9;
    h.target_id = 2;
    h.run_id = 3;
    h.request_id = 0x34;
    const uint8_t body[] = {0xAA, 0x00, 0xBB};
    seeds.push_back(BuildRelayFrame(h, -2, body, sizeof(body)));
  }

  {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.flags = kFlagMustUnderstand;
    h.conn_id = 9;
    h.target_id = 4;
    h.request_id = 42;
    seeds.push_back(BuildCreateWorkerFrame(h, CreateWorkerPayload{}));
    seeds.push_back(BuildCloseWorkerFrame(h));
    seeds.push_back(BuildCreateRuntimeFrame(h, CreateRuntimePayload{1, 0, 2, -1}));
    seeds.push_back(BuildCloseRuntimeFrame(h));
    h.flags = 0;
    h.request_id = 0;
    seeds.push_back(BuildRuntimeReadyFrame(h));
    seeds.push_back(BuildRuntimeClosedFrame(h, RuntimeClosedPayload{}));
    seeds.push_back(BuildStartupReadyFrame(h, StartupReadyPayload{1, 0, 1234}));
  }

  // A max-payload frame: a valid header whose payload_length is the ceiling,
  // followed by that many opaque bytes (stresses the length path at the limit).
  {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
    h.type = MessageType::RELAY_TO_WORKER;
    h.conn_id = 9;
    h.target_id = 2;
    h.run_id = 3;
    h.request_id = 1;
    h.payload_length = kMaxFramePayload;
    std::vector<uint8_t> frame;
    EncodeHeader(h, frame);
    frame.resize(frame.size() + kMaxFramePayload, 0xAB);
    seeds.push_back(std::move(frame));
  }

  // A truncated HELLO frame (header cut short -> kIncompleteHeader).
  {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
    h.request_id = 1;
    std::vector<uint8_t> frame = BuildHelloFrame(h);
    frame.resize(frame.size() / 2);
    seeds.push_back(std::move(frame));
  }

  // A reserved-flags frame: a valid HELLO with a reserved flag bit set.
  {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
    h.request_id = 1;
    std::vector<uint8_t> frame = BuildHelloFrame(h);
    frame[10] = 0x02;  // flags field (offset 10), bit1 reserved in v1
    seeds.push_back(std::move(frame));
  }

  // An oversized-length frame: a 32-byte header declaring payload_length one
  // past the ceiling (-> kOversizePayload before any body allocation).
  {
    FrameHeader h;
    h.version_major = kWireVersionMajor;
    h.version_minor = kWireVersionMinor;
    h.request_id = 1;
    h.payload_length = kMaxFramePayload + 1;
    std::vector<uint8_t> frame;
    EncodeHeader(h, frame);
    seeds.push_back(std::move(frame));
  }

  seeds.push_back(std::vector<uint8_t>());                       // empty
  seeds.push_back(std::vector<uint8_t>(64, 0x00));               // all-zero

  return seeds;
}

// One mutation step applied to a copy of a seed. Each strategy keeps the buffer
// within kMaxFuzzInput so allocation stays bounded.
void Mutate(std::vector<uint8_t>& buf,
            const std::vector<std::vector<uint8_t>>& seeds,
            Prng& rng) {
  switch (rng.Below(7)) {
    case 0:  // flip a few random bits
      if (!buf.empty()) {
        const size_t flips = 1 + rng.Below(8);
        for (size_t i = 0; i < flips; ++i)
          buf[rng.Below(buf.size())] ^= static_cast<uint8_t>(1u << rng.Below(8));
      }
      break;
    case 1:  // overwrite a random byte
      if (!buf.empty())
        buf[rng.Below(buf.size())] = rng.Byte();
      break;
    case 2:  // truncate to a random shorter length
      if (!buf.empty())
        buf.resize(rng.Below(buf.size()));
      break;
    case 3: {  // extend with random bytes (bounded)
      const size_t room = buf.size() < kMaxFuzzInput ? kMaxFuzzInput - buf.size() : 0;
      const size_t cap = room < 64 ? room : 64;
      const size_t add = cap == 0 ? 0 : 1 + rng.Below(cap);
      for (size_t i = 0; i < add; ++i)
        buf.push_back(rng.Byte());
      break;
    }
    case 4: {  // splice a prefix of another seed onto this buffer
      const std::vector<uint8_t>& other = seeds[rng.Below(seeds.size())];
      const size_t take = rng.Below(other.size() + 1);
      for (size_t i = 0; i < take && buf.size() < kMaxFuzzInput; ++i)
        buf.push_back(other[i]);
      break;
    }
    case 5:  // perturb the 32-bit payload_length field (header offset 28)
      if (buf.size() >= kFrameHeaderSize) {
        const uint32_t v = rng.NextU32();
        buf[28] = static_cast<uint8_t>(v & 0xFF);
        buf[29] = static_cast<uint8_t>((v >> 8) & 0xFF);
        buf[30] = static_cast<uint8_t>((v >> 16) & 0xFF);
        buf[31] = static_cast<uint8_t>((v >> 24) & 0xFF);
      }
      break;
    case 6:  // perturb the type + flags fields (header offsets 8..11)
      if (buf.size() >= kFrameHeaderSize)
        for (size_t off = 8; off < 12; ++off)
          buf[off] = rng.Byte();
      break;
  }
  if (buf.size() > kMaxFuzzInput)
    buf.resize(kMaxFuzzInput);
}

// Feed one buffer to every decoder. Asserts only that each returns cleanly (no
// crash/UB under the build's bounds/UB checks); accept/reject is not asserted.
void ExerciseDecoders(const uint8_t* data, size_t size) {
  // 1) Frame, then dispatch on type over the validated payload view (mirrors
  //    exactly how the coordinator consumes a frame).
  FrameHeader header;
  const uint8_t* payload = nullptr;
  size_t payload_size = 0;
  if (DecodeAndValidateFrame(data, size, &header, &payload, &payload_size) ==
      DecodeStatus::kOk) {
    switch (header.type) {
      case MessageType::HELLO: {
        HelloPayload p;
        DecodeHelloPayload(payload, payload_size, &p);
        break;
      }
      case MessageType::HELLO_ACK: {
        HelloAckPayload p;
        DecodeHelloAckPayload(payload, payload_size, &p);
        break;
      }
      case MessageType::ACK: {
        AckPayload p;
        DecodeAckPayload(payload, payload_size, &p);
        break;
      }
      case MessageType::ERROR: {
        ErrorPayload p;
        DecodeErrorPayload(payload, payload_size, &p);
        break;
      }
      case MessageType::CREATE_SESSION: {
        CreateSessionPayload p;
        DecodeCreateSessionPayload(payload, payload_size, &p);
        break;
      }
      case MessageType::CREATE_WORKER: {
        CreateWorkerPayload p;
        if (DecodeCreateWorkerPayload(payload, payload_size, &p))
          ValidateCreateWorkerPayload(p);
        break;
      }
      case MessageType::CREATE_RUNTIME: {
        CreateRuntimePayload p;
        if (DecodeCreateRuntimePayload(payload, payload_size, &p))
          ValidateCreateRuntimePayload(p, kMaxWorkerSnapshots);
        break;
      }
      case MessageType::RUNTIME_CLOSED: {
        RuntimeClosedPayload p;
        DecodeRuntimeClosedPayload(payload, payload_size, &p);
        break;
      }
      case MessageType::STARTUP_READY: {
        StartupReadyPayload p;
        DecodeStartupReadyPayload(payload, payload_size, &p);
        break;
      }
      case MessageType::RESULT: {
        ResultPayload p;
        DecodeResultPayload(payload, payload_size, &p);
        break;
      }
      case MessageType::START_RUN: {
        StartRunPayload p;
        DecodeStartRunPayload(payload, payload_size, &p);
        break;
      }
      default:
        break;
    }
  }

  // 2) Independently fuzz every message decoder on the RAW buffer, so the
  //    payload codecs are exercised even when framing rejects these bytes.
  HelloPayload hp;
  DecodeHelloPayload(data, size, &hp);
  HelloAckPayload hap;
  DecodeHelloAckPayload(data, size, &hap);
  AckPayload ap;
  DecodeAckPayload(data, size, &ap);
  ErrorPayload ep;
  DecodeErrorPayload(data, size, &ep);
  CreateSessionPayload csp;
  DecodeCreateSessionPayload(data, size, &csp);
  StartRunPayload srp;
  DecodeStartRunPayload(data, size, &srp);
  CreateWorkerPayload added0;
  if (DecodeCreateWorkerPayload(data, size, &added0))
    ValidateCreateWorkerPayload(added0);
  CreateRuntimePayload added1;
  if (DecodeCreateRuntimePayload(data, size, &added1))
    ValidateCreateRuntimePayload(added1, kMaxWorkerSnapshots);
  RuntimeClosedPayload added2;
  DecodeRuntimeClosedPayload(data, size, &added2);
  StartupReadyPayload added3;
  DecodeStartupReadyPayload(data, size, &added3);
  ResultPayload added4;
  DecodeResultPayload(data, size, &added4);
  int32_t kind = 0;
  const uint8_t* relay_body = nullptr;
  size_t relay_body_len = 0;
  DecodeRelayPayload(data, size, &kind, &relay_body, &relay_body_len);
}

// Drive the bounded Reader over the raw bytes with a pseudo-random op sequence,
// proving every accessor stays in bounds on arbitrary input. The op count is
// capped and every iteration advances at least one byte (a refused op falls
// back to Skip(1)), so a stream of refusals can never loop forever.
void ExerciseReader(const uint8_t* data, size_t size, Prng& rng) {
  Reader reader(data, size);
  for (int i = 0; i < 512 && !reader.AtEnd(); ++i) {
    switch (rng.Below(5)) {
      case 0: {
        uint16_t v = 0;
        if (!reader.GetU16(&v))
          reader.Skip(1);
        break;
      }
      case 1: {
        uint32_t v = 0;
        if (!reader.GetU32(&v))
          reader.Skip(1);
        break;
      }
      case 2: {
        std::vector<uint8_t> bytes;
        if (!reader.GetBytes(rng.Below(size + 1), &bytes))
          reader.Skip(1);
        break;
      }
      case 3: {
        std::string s;
        if (!reader.GetString(&s))
          reader.Skip(1);
        break;
      }
      case 4:
        if (!reader.Skip(1 + rng.Below(4)))
          reader.Skip(1);
        break;
    }
  }
}

// Loads every file in `dir` as a seed. A tiny bit of Win32 (FindFirstFileA)
// keeps the protocol library dependency-free; the README in the corpus is read
// too, which is harmless (just more arbitrary seed bytes).
std::vector<std::vector<uint8_t>> LoadCorpusDir(const std::string& dir) {
  std::vector<std::vector<uint8_t>> files;
  std::string base = dir;
  if (!base.empty() && base.back() != '\\' && base.back() != '/')
    base.push_back('\\');
  WIN32_FIND_DATAA fd;
  HANDLE h = ::FindFirstFileA((base + "*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE)
    return files;
  // Collect names first and sort them, so the seed order (and thus a replay
  // from the printed -seed) is deterministic regardless of the directory
  // enumeration order.
  std::vector<std::string> names;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      names.emplace_back(fd.cFileName);
  } while (::FindNextFileA(h, &fd));
  ::FindClose(h);
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    std::ifstream in(base + name, std::ios::binary);
    if (!in)
      continue;
    files.emplace_back(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  }
  return files;
}

// Parses a complete unsigned value (decimal or 0x-prefixed); rejects an empty
// string or trailing garbage so a typo'd -runs=/-seed= fails loudly instead of
// silently becoming 0 (which would run the fuzzer for zero iterations).
bool ParseU64(const char* s, uint64_t* out) {
  if (s == nullptr || *s == '\0')
    return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long long value = std::strtoull(s, &end, 0);
  if (errno != 0 || end == s || *end != '\0')
    return false;
  *out = value;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t runs = 100000;
  uint64_t seed = 0x5bD2C0DEFACEF00Dull;
  std::string corpus_dir;
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (std::strncmp(arg, "-runs=", 6) == 0) {
      if (!ParseU64(arg + 6, &runs)) {
        std::fprintf(stderr, "fuzz: invalid -runs value '%s'\n", arg + 6);
        return 2;
      }
    } else if (std::strncmp(arg, "-seed=", 6) == 0) {
      if (!ParseU64(arg + 6, &seed)) {
        std::fprintf(stderr, "fuzz: invalid -seed value '%s'\n", arg + 6);
        return 2;
      }
    } else if (arg[0] != '-') {
      corpus_dir = arg;
    }
    // Unknown -flags are ignored (libFuzzer-style invocation tolerance).
  }

  std::vector<std::vector<uint8_t>> seeds = BuiltinSeeds();
  const size_t builtin_count = seeds.size();
  size_t corpus_count = 0;
  if (!corpus_dir.empty()) {
    for (std::vector<uint8_t>& f : LoadCorpusDir(corpus_dir)) {
      seeds.push_back(std::move(f));
      ++corpus_count;
    }
  }
  if (seeds.empty())
    seeds.push_back(std::vector<uint8_t>());  // never index into an empty set

  Prng rng(seed);
  for (uint64_t i = 0; i < runs; ++i) {
    std::vector<uint8_t> buf = seeds[rng.Below(seeds.size())];
    const int rounds = 1 + static_cast<int>(rng.Below(4));
    for (int r = 0; r < rounds; ++r)
      Mutate(buf, seeds, rng);
    ExerciseDecoders(buf.data(), buf.size());
    ExerciseReader(buf.data(), buf.size(), rng);
  }

  std::printf("fuzz: runs=%llu seeds=%llu (builtin=%llu corpus=%llu) seed=0x%llx ok\n",
              static_cast<unsigned long long>(runs),
              static_cast<unsigned long long>(seeds.size()),
              static_cast<unsigned long long>(builtin_count),
              static_cast<unsigned long long>(corpus_count),
              static_cast<unsigned long long>(seed));
  std::fflush(stdout);
  return 0;
}
