#ifndef TEST_APP_SBOX_TRUST_TRANSITION_TEST_PRIVATE_H_
#define TEST_APP_SBOX_TRUST_TRANSITION_TEST_PRIVATE_H_

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Fault injection and inspection for the trusted-startup to untrusted-guest
// boundary. This define is set only on dedicated test targets, never shipping.
#if !defined(SBOX_TRUST_TRANSITION_TESTING)
#error "This private interface is test-build only."
#endif

enum SboxTrustTransitionCase : uint32_t {
  SBOX_TRUST_TRANSITION_CASE_INVALID = 0,
  SBOX_TRUST_TRANSITION_CASE_ALLOC_FAILURE = 1,
  SBOX_TRUST_TRANSITION_CASE_FIRST_HOOK_FAILURE = 2,
  SBOX_TRUST_TRANSITION_CASE_PARTIAL_HOOK_FAILURE = 3,
  SBOX_TRUST_TRANSITION_CASE_RX_FAILURE = 4,
  SBOX_TRUST_TRANSITION_CASE_FINAL_MITIGATION_FAILURE = 5,
  SBOX_TRUST_TRANSITION_CASE_ACG_QUERY_FAILURE = 6,
  SBOX_TRUST_TRANSITION_CASE_ACG_WEAK_STATE = 7,
  SBOX_TRUST_TRANSITION_CASE_STRICT_SUCCESS = 8,
  SBOX_TRUST_TRANSITION_CASE_LPAC_SUCCESS = 9,
  SBOX_TRUST_TRANSITION_CASE_ACG_OFF_SUCCESS = 10,
  SBOX_TRUST_TRANSITION_CASE_POLICY_SUCCESS = 11,
  SBOX_TRUST_TRANSITION_CASE_REQUIRED_FILE_HOOK_FAILURE = 12,
  SBOX_TRUST_TRANSITION_CASE_UNUSED_FILE_HOOK_SUCCESS = 13,
  SBOX_TRUST_TRANSITION_CASE_BASE_MISMATCH = 14,
};

enum SboxTrustTransitionPlatform : uint32_t {
  SBOX_TRUST_TRANSITION_PLATFORM_CURRENT = 0,
  SBOX_TRUST_TRANSITION_PLATFORM_PRE_RS5 = 1,
  SBOX_TRUST_TRANSITION_PLATFORM_RS5 = 2,
  SBOX_TRUST_TRANSITION_PLATFORM_UNKNOWN = 3,
};

enum SboxTrustTransitionBrokerFault : uint32_t {
  SBOX_TRUST_TRANSITION_BROKER_NONE = 0,
  SBOX_TRUST_TRANSITION_BROKER_TOKEN_FAILURE = 1,
};

enum class SboxTrustTransitionFileProbe : int32_t { Default, None, Readonly, Writable, Nonmatching, Pipe };

struct SboxTrustTransitionPolicyExpectation {
  SboxTrustTransitionFileProbe file_probe;
  int32_t ipc_lifetime;
  int32_t early_hooks;  // 1: token guard; 2: warm-up I/O compatibility.
  int32_t file_read_allowed;
  int32_t initial_integrity;
  int32_t delayed_integrity;
  int32_t app_container;
  int32_t lpac;
  wchar_t package_sid[184];
  wchar_t fixture_directory[260];
};

struct SboxTrustTransitionTestControl {
  uint64_t magic;
  uint32_t version;
  uint32_t case_id;
  uint64_t serial;
  SboxTrustTransitionPolicyExpectation policy;
};

struct SboxTrustTransitionExitObservation {
  uint32_t count;
  uint32_t exit_code;
};

constexpr uint64_t kSboxTrustTransitionControlMagic = 0x5342503154455354ull;
constexpr uint32_t kSboxTrustTransitionControlVersion = 2;
constexpr wchar_t kSboxTrustTransitionCapability[] = L"S-1-15-3-4021848294-1651122667-3873966303-2985905677";

// The control plane and both test roles link into one image, so these symbols
// resolve at link time and are never exported.
#define SBOX_TRUST_TRANSITION_API extern "C"

// The broker writes this control data at the same symbol address in the
// suspended worker after the equal-image-base check. Defined in the test main.
extern "C" SboxTrustTransitionTestControl g_sbox_trust_transition_test_control;

SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_set_next_case(uint32_t case_id, uint64_t serial);
SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_initialize(const SboxTrustTransitionTestControl *control);
SBOX_TRUST_TRANSITION_API const char *sbox_trust_transition_test_last_output();
SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_set_next_policy_case(
    const SboxTrustTransitionPolicyExpectation *policy,
    uint64_t serial);
SBOX_TRUST_TRANSITION_API void sbox_trust_transition_test_reset_broker();
SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_set_platform(uint32_t platform);
SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_set_broker_fault(uint32_t fault);
SBOX_TRUST_TRANSITION_API uint32_t sbox_trust_transition_test_pending_broker_fault();
SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_get_exit_observation(
    SboxTrustTransitionExitObservation *observation);

namespace sandbox::trust_transition_test {
struct Inspection {
  bool valid = true;
  unsigned patched = 0;
  unsigned originals = 0;
  unsigned rx = 0;
};
Inspection InspectBeforeResume(HANDLE process);
bool Consume(uint32_t case_id);
bool HookFault(int num_thunks, const std::string& function);
void Disarm();
uint32_t Consumed();
void Initialize(uint32_t case_id);
uint32_t Platform();
void SetPlatform(uint32_t platform);
void SetBrokerFault(uint32_t fault);
uint32_t BrokerFault();
bool ConsumeBrokerFault(uint32_t fault);
} // namespace sandbox::trust_transition_test

#endif // TEST_APP_SBOX_TRUST_TRANSITION_TEST_PRIVATE_H_
