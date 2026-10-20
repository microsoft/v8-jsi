#ifndef V8HOST_CLIENT_V8HOST_BROKER_RENDEZVOUS_H_
#define V8HOST_CLIENT_V8HOST_BROKER_RENDEZVOUS_H_

#include "v8host_payload_identity.h"

#include <memory>

namespace v8host {

enum class RendezvousStatus {
  kOk,
  kInvalidArgument,
  kPayloadInvalid,
  kEndpointInvalid,
  kLaunchFailed,
  kStartTimeout,
  kPeerAuthenticationFailed,
  kIoFailed,
  kProtocolFailed,
};

struct HelloResult {
  uint32_t conn_id = 0;          // broker-assigned, nonzero
  uint16_t selected_major = 0;   // negotiated wire version (HELLO_ACK header)
  uint16_t selected_minor = 0;
  uint16_t broker_max_major = 0; // broker's advertised maximum
  uint16_t broker_max_minor = 0;
  uint32_t endpoint_mode = 0;    // kEndpointMode* value from HELLO_ACK
  uint32_t request_id = 0;       // echoed HELLO request_id
};

class BrokerConnection {
 public:
  BrokerConnection();
  ~BrokerConnection();
  BrokerConnection(BrokerConnection&&) noexcept;
  BrokerConnection& operator=(BrokerConnection&&) noexcept;
  BrokerConnection(const BrokerConnection&) = delete;
  BrokerConnection& operator=(const BrokerConnection&) = delete;

  HANDLE pipe() const;
  DWORD broker_pid() const;
  const std::wstring& endpoint() const;
  RendezvousStatus Handshake(uint32_t request_id, HelloResult* result,
                             HANDLE stop = nullptr, ULONGLONG deadline = 0);
  void Close();
#ifdef V8HOST_CLIENT_TESTING
  void AdoptForTesting(HANDLE pipe);
#endif

 private:
  friend class BrokerRendezvous;
  struct State;
  std::unique_ptr<State> state_;
};

#ifdef V8HOST_CLIENT_TESTING
enum class LaunchHookPoint { kBeforeCreate, kLaunching, kCreated, kReturned };
using LaunchHook = void (*)(void*, LaunchHookPoint, HANDLE, ULONGLONG, const wchar_t*);
#endif

class BrokerRendezvous {
 public:
  BrokerRendezvous(std::wstring payload_directory, BrokerMode mode);
  // Process creation time is excluded from the startup budget; *deadline is
  // extended by it. Signal stop to cancel; transport Close signals it.
  RendezvousStatus ConnectOrLaunch(BrokerConnection* connection,
                                   HANDLE stop = nullptr, ULONGLONG* deadline = nullptr);
#ifdef V8HOST_CLIENT_TESTING
  void SetLaunchHookForTesting(LaunchHook hook, void* context);
#endif

  const std::wstring& endpoint() const { return endpoint_; }
  const std::array<uint8_t, 32>& endpoint_key() const { return endpoint_key_; }

 private:
  RendezvousStatus Initialize();
  RendezvousStatus TryConnect(HANDLE launched_process,
                              BrokerConnection* connection);
  RendezvousStatus LaunchCandidate(PROCESS_INFORMATION* process);

  std::wstring payload_directory_;
  BrokerMode mode_;
  PayloadIdentity payload_;
  std::vector<uint8_t> sid_;
  LUID session_ = {};
  std::array<uint8_t, 16> nonce_ = {};
  std::array<uint8_t, 32> endpoint_key_ = {};
  std::wstring endpoint_;
  bool initialized_ = false;
#ifdef V8HOST_CLIENT_TESTING
  LaunchHook test_launch_ = nullptr;
  void* test_context_ = nullptr;
#endif
};

}  // namespace v8host

#endif  // V8HOST_CLIENT_V8HOST_BROKER_RENDEZVOUS_H_
