// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#ifndef V8HOST_CLIENT_V8HOST_CLIENT_TRANSPORT_H_
#define V8HOST_CLIENT_V8HOST_CLIENT_TRANSPORT_H_

#include "v8host_client.h"

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <string>
#include <memory>
#ifdef V8HOST_CLIENT_TESTING
#include "v8host_broker_rendezvous.h"
#endif

namespace v8host::client {

enum class TransportDisconnect { kBrokerLost, kConnectFailed, kProtocolError };

// Production callbacks run on the I/O pump, without queue locks held.
// OnConnected publishes the assigned connection/negotiated versions before frames.
// Callbacks may race with or arrive after Close; delegates must tolerate them.
class ClientTransportDelegate {
 public:
  virtual ~ClientTransportDelegate() = default;
  virtual void OnConnected(uint32_t conn_id, uint16_t major, uint16_t minor) = 0;
  virtual void OnInboundFrame(const uint8_t* bytes, size_t len) = 0;
  virtual void OnDisconnect(TransportDisconnect reason) = 0;
  // Pump turn, outside transport locks, also after send credit is returned.
  virtual void OnPump() {}
};

class ClientTransport {
 public:
  virtual ~ClientTransport() = default;
  // Start(delegate) retains the delegate through background cleanup; production
  // Start() without a lease fails. The caller publishes a returned startup error;
  // asynchronous failure is reported via OnDisconnect.
  // SendFrame copies and enqueues: OK is not delivery; QUOTA admits nothing,
  // CONNECT means closed/terminal, and PROTOCOL means an invalid frame.
  // Close is idempotent, nonblocking and callable from a callback.
  virtual v8host_status Start() = 0;
  virtual v8host_status Start(std::shared_ptr<ClientTransportDelegate> delegate) {
    return Start();
  }
  virtual v8host_status SendFrame(const uint8_t* bytes, size_t len) = 0;
  virtual uint32_t conn_id() const = 0;
  virtual void Close() = 0;
  virtual void Wake() {}
#ifdef V8HOST_CLIENT_TESTING
  virtual size_t QueuedBytesForTesting() { return 0; }
#endif
};

struct TransportParams {
  std::wstring payload_directory;
  int32_t broker_mode = 0;
#ifdef V8HOST_CLIENT_TESTING
  void* test_context = nullptr;
  LaunchHook test_launch = nullptr;
  void (*test_pending)(void*, bool) = nullptr;
  RendezvousStatus (*test_connect)(void*, BrokerConnection*, HelloResult*, HANDLE,
                                   ULONGLONG) = nullptr;
#endif
};

using ClientTransportFactory =
    ClientTransport* (*)(const TransportParams&, ClientTransportDelegate*);

ClientTransport* CreateRealPipeClientTransport(const TransportParams& params,
                                               ClientTransportDelegate* delegate);

#ifdef V8HOST_CLIENT_TESTING
void SetSubmissionHooksForTesting(void (*before)(), void (*admitted)());
v8host_status SubmitCancelForTesting(v8host_session_s* session, uint32_t run);
void SetIdsForTesting(v8host_session_s* session, uint32_t request, uint32_t run);
void SetBrokerRequestIdForTesting(v8host_broker broker, uint32_t request);
uint64_t PostLifecycleProbeForTesting(uint32_t message, std::atomic<int>* count);
void PostLifecycleTokenForTesting(uint32_t message, uint64_t token);
uint64_t IgnoredLifecycleTokensForTesting();
void SetTransportFactoryForTesting(ClientTransportFactory factory);
void SetInitializeAfterDispatcherHookForTesting(void (*hook)());
uint32_t LifecycleThreadIdForTesting();
uint32_t LastSessionDeleteThreadIdForTesting();
void SetInboundStatusObserverForTesting(void (*observer)(v8host_status));
#endif

}  // namespace v8host::client

#endif  // V8HOST_CLIENT_V8HOST_CLIENT_TRANSPORT_H_
