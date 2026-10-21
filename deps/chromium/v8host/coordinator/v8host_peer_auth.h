#ifndef V8HOST_COORDINATOR_V8HOST_PEER_AUTH_H_
#define V8HOST_COORDINATOR_V8HOST_PEER_AUTH_H_

#include "v8host_file_identity.h"
#include "v8host_payload_identity.h"

#include <functional>

namespace v8host {

enum class PeerSide { kPipeClient, kPipeServer };

struct PeerAuthPolicy {
  PeerSide peer_side;
  BrokerMode mode;
  std::vector<uint8_t> expected_sid;
  LUID expected_session = {};
  std::wstring install_root;
  const HeldFile* expected_image = nullptr;
  HANDLE launched_process = nullptr;
  // Accept an unsigned peer image under every trust policy. The broker sets it
  // for application clients: a same-user, same-session client's signature is
  // not a security boundary, and host apps (e.g. developer builds) are often
  // unsigned. An invalid (tampered) signature is still rejected. Never set it
  // when the peer is a payload image (the broker itself).
  bool allow_unsigned_image = false;
};

// Test-only injection seam. Production callers pass nullptr (the default) and
// run entirely on the real OS-backed operations; a test may script where the
// peer PID, the resolved identity, the creation-instance check, or the image
// trust verdict come from in order to exercise fail-closed paths (PID reread
// mismatch, per-step query failure, an untrusted image) that cannot be
// reproduced against a real local pipe. This never relaxes a check — it only
// substitutes the source of identity facts, and the same decision logic runs
// regardless.
struct PeerAuthHooks {
  std::function<bool(HANDLE, PeerSide, DWORD*)> read_peer_pid;
  std::function<bool(DWORD, HeldProcess*, DWORD*)> query_identity;
  std::function<bool(HANDLE, const HeldProcess&)> same_creation_instance;
  std::function<TrustStatus(const HeldFile&)> verify_trust;
};

// On failure *error names the failing check (the broker logs it):
//   ERROR_INVALID_PARAMETER       no pipe or no output;
//   ERROR_PIPE_NOT_CONNECTED      the peer PID could not be read;
//   (the query's own OS error)    the peer's identity could not be read;
//   ERROR_BAD_PIPE                the peer PID changed between reads;
//   ERROR_INVALID_OWNER           the peer runs as a different user;
//   ERROR_NO_SUCH_LOGON_SESSION   the peer is in a different logon session;
//   TRUST_E_NOSIGNATURE           the peer image is unsigned (release policy,
//                                 without allow_unsigned_image);
//   TRUST_E_SUBJECT_NOT_TRUSTED   the peer image's signature is not valid;
//   TRUST_E_SYSTEM_ERROR          the peer image's trust could not be checked;
//   ERROR_NOT_SAME_OBJECT         the peer image is not the expected payload;
//   ERROR_PATH_NOT_FOUND          shared mode: the peer image is outside the
//                                 install root (not co-located);
//   ERROR_PROCESS_ABORTED         the peer is not the process instance launched.
bool AuthenticatePipePeer(HANDLE pipe,
                          const PeerAuthPolicy& policy,
                          HeldProcess* peer,
                          DWORD* error,
                          const PeerAuthHooks* hooks = nullptr);

}  // namespace v8host

#endif  // V8HOST_COORDINATOR_V8HOST_PEER_AUTH_H_
