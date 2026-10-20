#if defined(V8HOST_ABI4_FIXTURE)
#include "sbox.h"
namespace {
sbox_status SBOX_CDECL Configure(sbox_config, const sbox_config_api*, const void*, size_t) { return sbox_status_ok; }
sbox_status SBOX_CDECL Broker(sbox_broker, const sbox_broker_api*, const sbox_broker_start*) { return sbox_status_ok; }
sbox_status SBOX_CDECL Worker(sbox_worker, const sbox_worker_api*) { return sbox_status_ok; }
void SBOX_CDECL Shutdown(sbox_worker) {}
const sbox_plugin_api kAbi4 = {sizeof(sbox_plugin_api), 4u, Configure, Broker, Worker, Worker, Shutdown};
}
extern "C" __declspec(dllexport) sbox_status SBOX_CDECL sbox_plugin_main(
    uint32_t host_abi_version, const sbox_plugin_api** out_plugin) {
  if (!out_plugin) return sbox_status_invalid_arg;
  *out_plugin = nullptr;
  if (host_abi_version != 4u) return sbox_status_version_mismatch;
  *out_plugin = &kAbi4;
  return sbox_status_ok;
}
#else
// A deliberately invalid sbox plugin for negative ABI coverage: it resolves by
// name but reports the wrong ABI version and a vtable of null entries, so the
// generic host must reject it before driving any lifecycle. Test-only.
#include "sbox.h"

extern "C" __declspec(dllexport) sbox_status SBOX_CDECL sbox_plugin_main(
    uint32_t /*host_abi_version*/,
    const sbox_plugin_api** out_plugin) {
  static const sbox_plugin_api kInvalid = {sizeof(sbox_plugin_api),
                                       1u,  // not SBOX_ABI_VERSION
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr};
  if (out_plugin)
    *out_plugin = &kInvalid;
  return sbox_status_ok;
}

#endif
