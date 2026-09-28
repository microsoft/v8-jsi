#ifndef TEST_APP_SBOX_TRUST_TRANSITION_TEST_PRIVATE_H_
#define TEST_APP_SBOX_TRUST_TRANSITION_TEST_PRIVATE_H_

#include <cstddef>
#include <cstdint>

// Fault injection and telemetry for the trusted-startup to untrusted-guest
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
};

struct SboxTrustTransitionTestControl {
  uint64_t magic;
  uint32_t version;
  uint32_t case_id;
  uint64_t serial;
};

struct SboxTrustTransitionThunkTelemetry {
  uintptr_t base;
  size_t allocated_bytes;
  size_t used_bytes;
  size_t installed_hooks;
  int rx_complete;
};

constexpr uint64_t kSboxTrustTransitionControlMagic = 0x5342503154455354ull;
constexpr uint32_t kSboxTrustTransitionControlVersion = 1;

#if defined(SBOX_TRUST_TRANSITION_TEST_DLL_IMPL)
#define SBOX_TRUST_TRANSITION_API extern "C" __declspec(dllexport)
#else
#define SBOX_TRUST_TRANSITION_API extern "C" __declspec(dllimport)
#endif

SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_set_next_case(uint32_t case_id, uint64_t serial);
SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_initialize(const SboxTrustTransitionTestControl *control);
SBOX_TRUST_TRANSITION_API int sbox_trust_transition_test_get_thunk_telemetry(
    SboxTrustTransitionThunkTelemetry *telemetry);
SBOX_TRUST_TRANSITION_API const char *sbox_trust_transition_test_last_output();

namespace sandbox::trust_transition_test {
bool Consume(uint32_t case_id);
uint32_t CurrentCase();
void Initialize(uint32_t case_id);
void SetThunkTelemetry(const SboxTrustTransitionThunkTelemetry &telemetry);
SboxTrustTransitionThunkTelemetry GetThunkTelemetry();
} // namespace sandbox::trust_transition_test

#endif // TEST_APP_SBOX_TRUST_TRANSITION_TEST_PRIVATE_H_
