// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include <windows.h>
#include "v8_jsi_abi.h"

static jsi_runtime *runtime;
static v8_jsi_abi_config_set_inspector_enabled_fn enable;
static v8_jsi_abi_config_set_inspector_port_fn setPort;
static jsi_status JSI_ABI_CDECL configure(void *data, jsi_config config) {
  const auto status = enable(config, true);
  return status == jsi_status_ok ? setPort(config, *static_cast<unsigned short *>(data)) : status;
}
extern "C" bool modern_create(unsigned short port) {
  const auto module = GetModuleHandleW(L"v8jsi2.dll");
  const auto create = reinterpret_cast<v8_jsi_abi_runtime_create_fn>(GetProcAddress(module, "v8_jsi_abi_runtime_create"));
  enable = reinterpret_cast<v8_jsi_abi_config_set_inspector_enabled_fn>(GetProcAddress(module, "v8_jsi_abi_config_set_inspector_enabled"));
  setPort = reinterpret_cast<v8_jsi_abi_config_set_inspector_port_fn>(GetProcAddress(module, "v8_jsi_abi_config_set_inspector_port"));
  return create && enable && setPort && create(JSI_ABI_VERSION, configure, &port, &runtime) == jsi_status_ok;
}
extern "C" bool modern_eval() {
  const jsi_buffer_api api{[](jsi_buffer *) {}};
  jsi_buffer source{&api, reinterpret_cast<const uint8_t *>("6 * 7"), 5};
  jsi_value value{};
  const auto status = runtime->api->evaluate_javascript_source(runtime, &source, "modern.js", 9, &value);
  return status == jsi_status_ok && value.kind == jsi_valuekind_number && value.data.number == 42;
}
extern "C" void modern_destroy() { if (runtime) { runtime->api->release(runtime); runtime = nullptr; } }
