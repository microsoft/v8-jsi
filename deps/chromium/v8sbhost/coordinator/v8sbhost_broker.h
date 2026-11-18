#ifndef V8SBHOST_COORDINATOR_V8SBHOST_BROKER_H_
#define V8SBHOST_COORDINATOR_V8SBHOST_BROKER_H_

#include "sbox.h"
#include <windows.h>
#include <chrono>

inline constexpr auto kDedicatedPreConnectExpiry = std::chrono::seconds(30);
static_assert(kDedicatedPreConnectExpiry == std::chrono::seconds(30),
              "the dedicated pre-connect expiry is a tested 30 s contract");

inline bool V8SbHostBrokerPreConnectExpired(
    bool dedicated, bool first_accept,
    std::chrono::steady_clock::time_point listen_start,
    std::chrono::steady_clock::time_point now) {
  return dedicated && !first_accept && now - listen_start >= kDedicatedPreConnectExpiry;
}

inline bool V8SbHostBrokerFinishPendingConnect(HANDLE pipe, OVERLAPPED *pending) {
  ::CancelIoEx(pipe, pending);
  DWORD transferred = 0;
  // Completion may win the race with cancellation; preserve that connection.
  return ::GetOverlappedResult(pipe, pending, &transferred, TRUE) != FALSE;
}

enum class V8SbHostBrokerAcceptStatus { kPending, kConnected, kFailed };

inline V8SbHostBrokerAcceptStatus V8SbHostBrokerPollConnect(HANDLE pipe,
                                                      OVERLAPPED *pending,
                                                      DWORD timeout_ms) {
  const DWORD waited = ::WaitForSingleObject(pending->hEvent, timeout_ms);
  // Cancelling a poll leaves a connectable instance until its handle is closed.
  if (waited == WAIT_TIMEOUT)
    return V8SbHostBrokerAcceptStatus::kPending;
  if (waited != WAIT_OBJECT_0)
    return V8SbHostBrokerFinishPendingConnect(pipe, pending)
        ? V8SbHostBrokerAcceptStatus::kConnected : V8SbHostBrokerAcceptStatus::kFailed;
  DWORD transferred = 0;
  return ::GetOverlappedResult(pipe, pending, &transferred, FALSE)
      ? V8SbHostBrokerAcceptStatus::kConnected : V8SbHostBrokerAcceptStatus::kFailed;
}

sbox_status V8SbHostBrokerRun(sbox_broker broker,
                            const sbox_broker_api* api,
                            const sbox_broker_start* start);

#endif  // V8SBHOST_COORDINATOR_V8SBHOST_BROKER_H_
