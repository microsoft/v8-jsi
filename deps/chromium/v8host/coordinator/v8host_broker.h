#ifndef V8HOST_COORDINATOR_V8HOST_BROKER_H_
#define V8HOST_COORDINATOR_V8HOST_BROKER_H_

#include "sbox.h"
#include <windows.h>
#include <chrono>

inline constexpr auto kDedicatedPreConnectExpiry = std::chrono::seconds(30);
static_assert(kDedicatedPreConnectExpiry == std::chrono::seconds(30),
              "the dedicated pre-connect expiry is a tested 30 s contract");

inline bool V8HostBrokerPreConnectExpired(
    bool dedicated, bool first_accept,
    std::chrono::steady_clock::time_point listen_start,
    std::chrono::steady_clock::time_point now) {
  return dedicated && !first_accept && now - listen_start >= kDedicatedPreConnectExpiry;
}

inline bool V8HostBrokerFinishPendingConnect(HANDLE pipe, OVERLAPPED *pending) {
  ::CancelIoEx(pipe, pending);
  DWORD transferred = 0;
  // Completion may win the race with cancellation; preserve that connection.
  return ::GetOverlappedResult(pipe, pending, &transferred, TRUE) != FALSE;
}

enum class V8HostBrokerAcceptStatus { kPending, kConnected, kFailed };

inline V8HostBrokerAcceptStatus V8HostBrokerPollConnect(HANDLE pipe,
                                                      OVERLAPPED *pending,
                                                      DWORD timeout_ms) {
  const DWORD waited = ::WaitForSingleObject(pending->hEvent, timeout_ms);
  // Cancelling a poll leaves a connectable instance until its handle is closed.
  if (waited == WAIT_TIMEOUT)
    return V8HostBrokerAcceptStatus::kPending;
  if (waited != WAIT_OBJECT_0)
    return V8HostBrokerFinishPendingConnect(pipe, pending)
        ? V8HostBrokerAcceptStatus::kConnected : V8HostBrokerAcceptStatus::kFailed;
  DWORD transferred = 0;
  return ::GetOverlappedResult(pipe, pending, &transferred, FALSE)
      ? V8HostBrokerAcceptStatus::kConnected : V8HostBrokerAcceptStatus::kFailed;
}

sbox_status V8HostBrokerRun(sbox_broker broker,
                            const sbox_broker_api* api,
                            const sbox_broker_start* start);

#endif  // V8HOST_COORDINATOR_V8HOST_BROKER_H_
