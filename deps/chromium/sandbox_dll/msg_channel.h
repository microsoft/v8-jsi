// msg_channel.h — the duplex message channel laid over a shared section. Used
// only inside sbox.dll (both broker and target run the same DLL, so the layout
// is identical in both). Two single-producer/single-consumer slot rings:
//   t2b: target -> broker     b2t: broker -> target
// Each direction has an auto-reset event the PRODUCER signals after a post; the
// CONSUMER waits on it and drains. Fire-and-forget; frames carry a kind tag
// (string vs binary) so the channel itself is payload-agnostic.
#ifndef SANDBOX_DLL_MSG_CHANNEL_H_
#define SANDBOX_DLL_MSG_CHANNEL_H_

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace sbox_msg {

constexpr uint32_t kMagic = 0x534D5347u;          // 'SMSG'
constexpr uint32_t kSlots = 16;                   // slots per ring
constexpr uint32_t kSlotSize = 64u * 1024u;       // bytes per slot
constexpr uint32_t kMaxPayload = kSlotSize - 8u;  // minus {len,kind}
constexpr uint32_t kRingBytes = kSlots * kSlotSize;

struct RingControl {
  volatile uint32_t write_idx;  // owned by the producer
  volatile uint32_t read_idx;   // owned by the consumer
  uint32_t pad[14];             // keep the two indices on separate cache lines
};

struct ChannelHeader {
  uint32_t magic;
  uint32_t slots;
  uint32_t slot_size;
  uint32_t reserved;
  RingControl t2b;  // target -> broker
  RingControl b2t;  // broker -> target
};

constexpr size_t kSectionSize =
    sizeof(ChannelHeader) + 2u * static_cast<size_t>(kRingBytes);

inline ChannelHeader* Header(void* base) {
  return reinterpret_cast<ChannelHeader*>(base);
}
inline uint8_t* T2BRing(void* base) {
  return reinterpret_cast<uint8_t*>(base) + sizeof(ChannelHeader);
}
inline uint8_t* B2TRing(void* base) {
  return T2BRing(base) + kRingBytes;
}

inline void InitHeader(void* base) {
  ChannelHeader* h = Header(base);
  std::memset(h, 0, sizeof(*h));
  h->magic = kMagic;
  h->slots = kSlots;
  h->slot_size = kSlotSize;
}

// SPSC post. Returns true on success, false if oversized or the ring is full.
inline bool Post(RingControl* ctl, uint8_t* ring, int kind, const void* data,
                 size_t len) {
  if (len > kMaxPayload)
    return false;
  uint32_t w = ctl->write_idx;
  uint32_t r = ctl->read_idx;  // cross-read; a stale (small) value only over-
  if (w - r >= kSlots)         // estimates fullness, which is safe.
    return false;
  uint8_t* slot = ring + (w % kSlots) * kSlotSize;
  uint32_t l = static_cast<uint32_t>(len);
  uint32_t k = static_cast<uint32_t>(kind);
  std::memcpy(slot + 0, &l, 4);
  std::memcpy(slot + 4, &k, 4);
  std::memcpy(slot + 8, data, len);
  MemoryBarrier();        // publish the slot before the index
  ctl->write_idx = w + 1;
  return true;
}

typedef void (*FrameCb)(void* ctx, int kind, const void* data, size_t len);

// SPSC drain: invoke cb for every available frame. Returns the count drained.
inline int Drain(RingControl* ctl, uint8_t* ring, FrameCb cb, void* ctx) {
  int n = 0;
  uint32_t r = ctl->read_idx;
  while (r != ctl->write_idx) {
    MemoryBarrier();  // observe the slot after the index
    uint8_t* slot = ring + (r % kSlots) * kSlotSize;
    uint32_t len = 0, kind = 0;
    std::memcpy(&len, slot + 0, 4);
    std::memcpy(&kind, slot + 4, 4);
    if (len > kMaxPayload)
      len = kMaxPayload;  // defensive clamp on untrusted input
    cb(ctx, static_cast<int>(kind), slot + 8, len);
    MemoryBarrier();
    ctl->read_idx = r + 1;
    r = ctl->read_idx;
    ++n;
  }
  return n;
}

}  // namespace sbox_msg

#endif  // SANDBOX_DLL_MSG_CHANNEL_H_
