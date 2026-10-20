#include "v8host_peer_auth.h"

namespace v8host {
namespace {

bool SameCreationInstance(HANDLE expected, const HeldProcess& actual) {
  if (!expected)
    return true;
  if (::GetProcessId(expected) != actual.pid())
    return false;
  FILETIME creation = {}, exit = {}, kernel = {}, user = {};
  return ::GetProcessTimes(expected, &creation, &exit, &kernel, &user) &&
         creation.dwLowDateTime == actual.creation_time().dwLowDateTime &&
         creation.dwHighDateTime == actual.creation_time().dwHighDateTime;
}

bool ReadPeerPid(HANDLE pipe, PeerSide side, DWORD* pid) {
  ULONG value = 0;
  const BOOL ok =
      side == PeerSide::kPipeClient
          ? ::GetNamedPipeClientProcessId(pipe, &value)
          : ::GetNamedPipeServerProcessId(pipe, &value);
  if (ok)
    *pid = value;
  return ok != FALSE;
}

}  // namespace

bool AuthenticatePipePeer(HANDLE pipe,
                          const PeerAuthPolicy& policy,
                          HeldProcess* peer,
                          DWORD* error,
                          const PeerAuthHooks* hooks) {
  using ReadPidFn = std::function<bool(HANDLE, PeerSide, DWORD*)>;
  using QueryFn = std::function<bool(DWORD, HeldProcess*, DWORD*)>;
  using SameInstFn = std::function<bool(HANDLE, const HeldProcess&)>;
  using TrustFn = std::function<TrustStatus(const HeldFile&)>;
  const ReadPidFn read_pid = hooks && hooks->read_peer_pid
                                 ? hooks->read_peer_pid
                                 : ReadPidFn(ReadPeerPid);
  const QueryFn query = hooks && hooks->query_identity
                            ? hooks->query_identity
                            : QueryFn(QueryProcessIdentity);
  const SameInstFn same_inst = hooks && hooks->same_creation_instance
                                   ? hooks->same_creation_instance
                                   : SameInstFn(SameCreationInstance);
  const TrustFn verify_trust = hooks && hooks->verify_trust
                                   ? hooks->verify_trust
                                   : TrustFn(VerifyTrust);
  // Each check reports its own code (see the header) so a log names it.
  const auto fail = [error](DWORD reason) {
    if (error)
      *error = reason;
    return false;
  };
  if (pipe == INVALID_HANDLE_VALUE || !peer)
    return fail(ERROR_INVALID_PARAMETER);
  DWORD first_pid = 0;
  if (!read_pid(pipe, policy.peer_side, &first_pid))
    return fail(ERROR_PIPE_NOT_CONNECTED);
  HeldProcess identity;
  DWORD query_error = ERROR_SUCCESS;
  if (!query(first_pid, &identity, &query_error))
    return fail(query_error != ERROR_SUCCESS ? query_error : ERROR_ACCESS_DENIED);
  if (identity.pid() != first_pid)
    return fail(ERROR_BAD_PIPE);
  DWORD second_pid = 0;
  if (!read_pid(pipe, policy.peer_side, &second_pid))
    return fail(ERROR_PIPE_NOT_CONNECTED);
  if (second_pid != first_pid)
    return fail(ERROR_BAD_PIPE);
  if (!EqualSidBytes(identity.sid(), policy.expected_sid))
    return fail(ERROR_INVALID_OWNER);
  if (!EqualLuid(identity.logon_session(), policy.expected_session))
    return fail(ERROR_NO_SUCH_LOGON_SESSION);
  const TrustStatus trust = verify_trust(identity.image());
  if (!TrustAllowed(trust)) {
    return fail(static_cast<DWORD>(
        trust == TrustStatus::kUnsigned  ? TRUST_E_NOSIGNATURE
        : trust == TrustStatus::kInvalid ? TRUST_E_SUBJECT_NOT_TRUSTED
                                         : TRUST_E_SYSTEM_ERROR));
  }
  if (policy.expected_image &&
      !IsSameFileIdentity(identity.image(), *policy.expected_image))
    return fail(ERROR_NOT_SAME_OBJECT);
  if (policy.mode == BrokerMode::kShared &&
      !IsStrictDescendant(policy.install_root, identity.image().final_path()))
    return fail(ERROR_PATH_NOT_FOUND);
  if (!same_inst(policy.launched_process, identity))
    return fail(ERROR_PROCESS_ABORTED);
  if (!read_pid(pipe, policy.peer_side, &second_pid))
    return fail(ERROR_PIPE_NOT_CONNECTED);
  if (second_pid != first_pid)
    return fail(ERROR_BAD_PIPE);
  *peer = std::move(identity);
  if (error)
    *error = ERROR_SUCCESS;
  return true;
}

}  // namespace v8host
