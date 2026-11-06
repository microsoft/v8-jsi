#include "sbox_trust_transition_test_private.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

namespace sandbox::trust_transition_test {
namespace {
std::atomic<uint32_t> g_case{SBOX_TRUST_TRANSITION_CASE_INVALID};
std::atomic<uint32_t> g_consumed{SBOX_TRUST_TRANSITION_CASE_INVALID};
std::atomic<uint32_t> g_platform{SBOX_TRUST_TRANSITION_PLATFORM_CURRENT};
std::atomic<uint32_t> g_broker_fault{SBOX_TRUST_TRANSITION_BROKER_NONE};
} // namespace

bool Consume(uint32_t case_id) {
  if (g_case.load() != case_id)
    return false;
  uint32_t expected = SBOX_TRUST_TRANSITION_CASE_INVALID;
  if (!g_consumed.compare_exchange_strong(expected, case_id))
    return false;
  ::printf("[trust-transition] consumed=%u\n", case_id);
  return true;
}

bool HookFault(int num_thunks, const std::string& function) {
  return (num_thunks == 0 &&
          Consume(SBOX_TRUST_TRANSITION_CASE_FIRST_HOOK_FAILURE)) ||
         (num_thunks == 1 &&
          Consume(SBOX_TRUST_TRANSITION_CASE_PARTIAL_HOOK_FAILURE)) ||
         (function == "NtCreateFile" &&
          Consume(SBOX_TRUST_TRANSITION_CASE_REQUIRED_FILE_HOOK_FAILURE));
}

void Disarm() {
  g_case.store(SBOX_TRUST_TRANSITION_CASE_INVALID);
}

uint32_t Consumed() {
  return g_consumed.load();
}

void Initialize(uint32_t case_id) {
  g_consumed.store(SBOX_TRUST_TRANSITION_CASE_INVALID);
  g_case.store(case_id);
}

uint32_t Platform() {
  return g_platform.load();
}

void SetPlatform(uint32_t platform) {
  g_platform.store(platform);
}

void SetBrokerFault(uint32_t fault) {
  g_broker_fault.store(fault);
}

uint32_t BrokerFault() {
  return g_broker_fault.load();
}

bool ConsumeBrokerFault(uint32_t fault) {
  return g_broker_fault.compare_exchange_strong(fault, SBOX_TRUST_TRANSITION_BROKER_NONE);
}
} // namespace sandbox::trust_transition_test
