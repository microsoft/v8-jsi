// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// TODO(crbug.com/351564777): Remove this and convert code to safer constructs.
#pragma allow_unsafe_buffers
#endif

#include "sandbox/win/src/target_process.h"

#include <windows.h>

#include <processenv.h>
#include <stddef.h>
#include <stdint.h>

#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/memory/free_deleter.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/string_util.h"
#include "base/win/access_token.h"
#include "base/win/current_module.h"
#include "base/win/scoped_handle.h"
#include "base/win/security_util.h"
#include "base/win/startup_information.h"
#include "sandbox/win/src/crosscall_client.h"
#include "sandbox/win/src/crosscall_server.h"
#include "sandbox/win/src/policy_low_level.h"
#include "sandbox/win/src/restricted_token_utils.h"
#include "sandbox/win/src/sandbox_nt_util.h"
#include "sandbox/win/src/sandbox_types.h"
#include "sandbox/win/src/sharedmem_ipc_server.h"
#include "sandbox/win/src/startup_information_helper.h"
#include "sandbox/win/src/win_utils.h"

namespace sandbox {

namespace {

void CopyPolicyToTarget(base::span<const uint8_t> source, void* dest) {
  if (!source.size()) {
    return;
  }
  memcpy(dest, source.data(), source.size());
  sandbox::PolicyGlobal* policy =
      reinterpret_cast<sandbox::PolicyGlobal*>(dest);

  size_t offset = reinterpret_cast<size_t>(source.data());

  for (size_t i = 0; i < sandbox::kSandboxIpcCount; i++) {
    size_t buffer = reinterpret_cast<size_t>(policy->entry[i]);
    if (buffer) {
      buffer -= offset;
      policy->entry[i] = reinterpret_cast<sandbox::PolicyBuffer*>(buffer);
    }
  }
}

}  // namespace

// 'SAND'
SANDBOX_INTERCEPT DWORD g_sentinel_value_start = 0x53414E44;
SANDBOX_INTERCEPT HANDLE g_shared_section;
SANDBOX_INTERCEPT size_t g_shared_IPC_size;
// The following may be zero if not needed in the child.
SANDBOX_INTERCEPT size_t g_shared_policy_size;
SANDBOX_INTERCEPT size_t g_delegate_data_size;
// 'BOXY'
SANDBOX_INTERCEPT DWORD g_sentinel_value_end = 0x424F5859;

// Hosted (decoupled-DLL) mode: the sandbox engine is a shared sandbox.dll loaded
// by a broker host that is a DIFFERENT binary from the target. The broker can't
// WriteProcessMemory into the suspended child's unmapped sandbox.dll, so in this
// mode TargetProcess::Init adopts a broker-provided shared section, keeps the IPC
// server, and skips the sentinel + every fixed-&g_global TransferVariable. The
// broker relays the child-side section handle + sizes to the child via its EXE
// bootstrap struct instead. Engine-generic; no app/guest knowledge.
extern bool g_sbox_hosted_mode;                    // sandbox_policy_base.cc
HANDLE g_sbox_hosted_section = nullptr;            // broker-created section to adopt
uint32_t g_sbox_hosted_child_ipc_size = 0;         // captured for the broker to relay
uint32_t g_sbox_hosted_child_policy_size = 0;      // captured for the broker to relay

TargetProcess::TargetProcess()
    : process_handle_(::GetCurrentProcess()),
      process_id_(::GetCurrentProcessId()) {}

TargetProcess::TargetProcess(HANDLE process_handle) {
  CHECK(::DuplicateHandle(::GetCurrentProcess(), process_handle,
                          ::GetCurrentProcess(), &process_handle, 0, FALSE,
                          DUPLICATE_SAME_ACCESS));
  process_handle_.Set(process_handle);
  process_id_ = ::GetProcessId(process_handle_.get());
}

TargetProcess::~TargetProcess() {
  // Give a chance to the process to die. In most cases the JOB_KILL_ON_CLOSE
  // will take effect only when the context changes. As far as the testing went,
  // this wait was enough to switch context and kill the processes in the job.
  // If this process is already dead, the function will return without waiting.
  // For now, this wait is there only to do a best effort to prevent some leaks
  // from showing up in purify.
  if (process_handle_.is_valid()) {
    ::WaitForSingleObject(process_handle_.get(), 50);
    // Terminate the process if it's still alive, as its IPC server is going
    // away. 1 is RESULT_CODE_KILLED.
    ::TerminateProcess(process_handle_.get(), 1);
  }

  // ipc_server_ references our process handle, so make sure the former is shut
  // down before the latter is closed (by ScopedProcessInformation).
  ipc_server_.reset();
}

ResultCode TargetProcess::TransferVariable(const void* local_address,
                                           void* target_address,
                                           size_t size) {
  if (!process_handle_.is_valid()) {
    return SBOX_ERROR_UNEXPECTED_CALL;
  }
  SIZE_T written;
  if (!::WriteProcessMemory(process_handle_.get(), target_address,
                            local_address, size, &written)) {
    return SBOX_ERROR_CANNOT_WRITE_VARIABLE_VALUE;
  }
  if (written != size) {
    return SBOX_ERROR_INVALID_WRITE_VARIABLE_SIZE;
  }

  return SBOX_ALL_OK;
}

// Construct the IPC server and the IPC dispatcher. When the target does
// an IPC it will eventually call the dispatcher.
ResultCode TargetProcess::Init(
    Dispatcher* ipc_dispatcher,
    std::optional<base::span<const uint8_t>> policy,
    std::optional<base::span<const uint8_t>> delegate_data,
    uint32_t shared_IPC_size,
    ThreadPool* thread_pool,
    DWORD* win_error) {
  ResultCode ret = SBOX_ALL_OK;
  if (!g_sbox_hosted_mode) {
    // Sentinel check reads the child's sandbox image — skip in hosted mode where
    // the child's sandbox.dll is not mapped while suspended.
    ret = VerifySentinels();
    if (ret != SBOX_ALL_OK)
      return ret;
  }
  // We need to map the shared memory on the target. This is necessary for
  // any IPC that needs to take place, even if the target has not yet hit
  // the main( ) function or even has initialized the CRT. So here we set
  // the handle to the shared section. The target on the first IPC must do
  // the rest, which boils down to calling MapViewofFile()

  // We use this single memory pool for IPC and for policy.
  size_t shared_mem_size = shared_IPC_size;
  if (policy.has_value()) {
    shared_mem_size += policy->size();
  }
  if (delegate_data.has_value()) {
    shared_mem_size += delegate_data->size();
  }

  // This region should be small, so we only pass dwMaximumSizeLow below.
  CHECK(shared_mem_size <= std::numeric_limits<DWORD>::max());

  HANDLE section_to_map;
  if (g_sbox_hosted_mode) {
    // Adopt the broker-provided section. It may be oversized; the offset CHECK
    // below validates the write math (computed from policy sizes), not the
    // section size. The broker owns the handle and relays it to the child via
    // its EXE bootstrap struct, so we do not take ownership here.
    section_to_map = g_sbox_hosted_section;
    g_sbox_hosted_child_ipc_size = static_cast<uint32_t>(shared_IPC_size);
    // The policy blob is copied into the section right after the IPC region
    // (CopyPolicyToTarget below). Relay its size to the child so the child's
    // MapGlobalMemory can locate the policy and its file interceptors can
    // evaluate rules locally before brokering.
    g_sbox_hosted_child_policy_size =
        policy.has_value() ? static_cast<uint32_t>(policy->size()) : 0;
  } else {
    shared_section_.Set(::CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0,
        static_cast<DWORD>(shared_mem_size), nullptr));
    if (!shared_section_.is_valid()) {
      *win_error = ::GetLastError();
      return SBOX_ERROR_CREATE_FILE_MAPPING;
    }
    section_to_map = shared_section_.get();
  }

  void* shared_memory = ::MapViewOfFile(
      section_to_map, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 0);
  if (!shared_memory) {
    *win_error = ::GetLastError();
    return SBOX_ERROR_MAP_VIEW_OF_SHARED_SECTION;
  }

  // The IPC area is just zeros so we skip over it.
  size_t current_offset = shared_IPC_size;
  // PolicyGlobal region.
  if (policy.has_value()) {
    CopyPolicyToTarget(policy.value(),
                       reinterpret_cast<char*>(shared_memory) + current_offset);
    current_offset += policy->size();
  }

  // Delegate Data region.
  if (delegate_data.has_value()) {
    memcpy(reinterpret_cast<char*>(shared_memory) + current_offset,
           delegate_data->data(), delegate_data->size());
    current_offset += delegate_data->size();
  }

  // After all regions are written we should be at the end of the allocation.
  CHECK_EQ(current_offset, shared_mem_size);

  // Set the global variables in the target. These are not used on the broker.
  // In hosted mode the broker can't write the child's unmapped sandbox.dll
  // globals; it relays these sizes to the child via the EXE bootstrap struct.
  if (!g_sbox_hosted_mode) {
    size_t transfer_shared_IPC_size = shared_IPC_size;
    static_assert(sizeof(g_shared_IPC_size) == sizeof(transfer_shared_IPC_size));
    ret = TransferVariable(&transfer_shared_IPC_size, &g_shared_IPC_size,
                           sizeof(g_shared_IPC_size));
    if (SBOX_ALL_OK != ret) {
      *win_error = ::GetLastError();
      return ret;
    }
    if (policy.has_value()) {
      size_t transfer_shared_policy_size = policy->size();
      static_assert(sizeof(g_shared_policy_size) ==
                    sizeof(transfer_shared_policy_size));
      ret = TransferVariable(&transfer_shared_policy_size, &g_shared_policy_size,
                             sizeof(g_shared_policy_size));
      if (SBOX_ALL_OK != ret) {
        *win_error = ::GetLastError();
        return ret;
      }
    }
    if (delegate_data.has_value()) {
      size_t transfer_delegate_data_size = delegate_data->size();
      static_assert(sizeof(g_delegate_data_size) ==
                    sizeof(transfer_delegate_data_size));
      ret = TransferVariable(&transfer_delegate_data_size, &g_delegate_data_size,
                             sizeof(g_delegate_data_size));
      if (SBOX_ALL_OK != ret) {
        *win_error = ::GetLastError();
        return ret;
      }
    }
  }

  ipc_server_ = std::make_unique<SharedMemIPCServer>(
      process_handle_.get(), process_id_, thread_pool, ipc_dispatcher);

  if (!ipc_server_->Init(shared_memory, shared_IPC_size, kIPCChannelSize))
    return SBOX_ERROR_NO_SPACE;

  // In hosted mode the broker duplicates the section into the child and relays the
  // handle value via the EXE bootstrap struct (the child's sandbox.dll global
  // can't be written while suspended/unmapped).
  if (!g_sbox_hosted_mode) {
    DWORD access = FILE_MAP_READ | FILE_MAP_WRITE | SECTION_QUERY;
    HANDLE target_shared_section;
    if (!::DuplicateHandle(::GetCurrentProcess(), shared_section_.get(),
                           process_handle_.get(), &target_shared_section, access,
                           false, 0)) {
      *win_error = ::GetLastError();
      return SBOX_ERROR_DUPLICATE_SHARED_SECTION;
    }

    static_assert(sizeof(g_shared_section) == sizeof(target_shared_section));
    ret = TransferVariable(&target_shared_section, &g_shared_section,
                           sizeof(g_shared_section));
    if (SBOX_ALL_OK != ret) {
      *win_error = ::GetLastError();
      return ret;
    }
  }

  return SBOX_ALL_OK;
}

void TargetProcess::Terminate() {
  if (process_handle_.is_valid()) {
    ::TerminateProcess(process_handle_.get(), 0);
  }
}

ResultCode TargetProcess::VerifySentinels() {
  if (!process_handle_.is_valid()) {
    return SBOX_ERROR_UNEXPECTED_CALL;
  }
  DWORD value = 0;
  SIZE_T read;

  if (!::ReadProcessMemory(process_handle_.get(), &g_sentinel_value_start,
                           &value, sizeof(DWORD), &read)) {
    return SBOX_ERROR_CANNOT_READ_SENTINEL_VALUE;
  }
  if (read != sizeof(DWORD))
    return SBOX_ERROR_INVALID_READ_SENTINEL_SIZE;
  if (value != g_sentinel_value_start)
    return SBOX_ERROR_MISMATCH_SENTINEL_VALUE;
  if (!::ReadProcessMemory(process_handle_.get(), &g_sentinel_value_end, &value,
                           sizeof(DWORD), &read)) {
    return SBOX_ERROR_CANNOT_READ_SENTINEL_VALUE;
  }
  if (read != sizeof(DWORD))
    return SBOX_ERROR_INVALID_READ_SENTINEL_SIZE;
  if (value != g_sentinel_value_end)
    return SBOX_ERROR_MISMATCH_SENTINEL_VALUE;

  return SBOX_ALL_OK;
}

}  // namespace sandbox
