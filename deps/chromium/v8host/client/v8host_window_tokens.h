// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#ifndef V8HOST_CLIENT_V8HOST_WINDOW_TOKENS_H_
#define V8HOST_CLIENT_V8HOST_WINDOW_TOKENS_H_

#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace v8host::client {

// Window messages carry only a 64-bit capability, split into two 32-bit halves
// on every architecture. Pointers and callbacks never cross the message queue.
// Storage is bounded and allocation-free; failure preserves the caller's
// existing post-failure ownership policy.
class WindowTokens {
 public:
  bool Initialize() {
    return BCRYPT_SUCCESS(::BCryptGenRandom(nullptr,
        reinterpret_cast<PUCHAR>(&secret_), sizeof(secret_), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
  }
  uint64_t Post(HWND window, UINT message, void* object, void (*invoke)(void*)) {
    uint64_t token = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      Entry* free = nullptr;
      for (auto& entry : entries_)
        if (!entry.token) { free = &entry; break; }
      if (!free) return 0;
      // Fresh CSPRNG entropy prevents prediction even after observing a token.
      do {
        if (counter_ == UINT64_MAX) return 0;
        uint64_t random = 0;
        if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr,
                reinterpret_cast<PUCHAR>(&random), sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
          return 0;
        token = random ^ secret_ ^ ++counter_;
      } while (!token || Contains(token));
      *free = {token, message, object, invoke};
    }
    if (window && ::PostMessageW(window, message, static_cast<WPARAM>(static_cast<uint32_t>(token)),
                                static_cast<LPARAM>(static_cast<uint32_t>(token >> 32))))
      return token;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_)
      if (entry.token == token) { entry = {}; break; }
    return 0;
  }
  void Dispatch(UINT message, WPARAM low, LPARAM high) {
    const uint64_t token = static_cast<uint32_t>(low) |
        (uint64_t{static_cast<uint32_t>(high)} << 32);
    Entry found{};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& entry : entries_)
        if (token && entry.token == token && entry.message == message) {
          found = entry;
          entry = {};  // one shot, before any reentrant callback
          break;
        }
    }
    if (found.invoke) found.invoke(found.object);
    else ignored_.fetch_add(1, std::memory_order_relaxed);
  }
  uint64_t ignored() const { return ignored_.load(std::memory_order_relaxed); }

 private:
  struct Entry {
    uint64_t token = 0;
    UINT message = 0;
    void* object = nullptr;
    void (*invoke)(void*) = nullptr;
  };
  bool Contains(uint64_t token) const {
    for (const auto& entry : entries_) if (entry.token == token) return true;
    return false;
  }
  std::mutex mutex_;
  std::array<Entry, 1024> entries_{};
  uint64_t secret_ = 0, counter_ = 0;
  std::atomic<uint64_t> ignored_{0};
};

}  // namespace v8host::client
#endif
