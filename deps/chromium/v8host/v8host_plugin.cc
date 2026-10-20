// The final-DLL composition seam. Persona implementations remain in separate
// source sets; only this translation unit assembles their plugin callbacks.

#include "sbox.h"
#include "v8host_broker.h"
#include "v8host_engine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <delayimp.h>

#include <cstdio>

namespace {

// user32 is delay-loaded for the client persona's dispatcher only. The worker
// loads this DLL too and must never initialize user32 (an AppContainer worker
// dies doing so on a service logon's desktop; see //sbox:sbox). Only the worker
// calls warmup; from then on any delay load ends the worker with sbox.exe's
// kDelayLoadRefused exit code (19), on any desktop.
bool g_worker = false;
constexpr UINT kDelayLoadRefused = 19;

FARPROC WINAPI WorkerDelayLoadGuard(unsigned notify, PDelayLoadInfo info) {
  if (g_worker && notify == dliStartProcessing) {
    std::printf("[v8host] worker: refused delay load of %s\n",
                info && info->szDll ? info->szDll : "?");
    ::TerminateProcess(::GetCurrentProcess(), kDelayLoadRefused);
  }
  return nullptr;
}

sbox_status SBOX_CDECL Warmup(sbox_worker worker, const sbox_worker_api* api) {
  g_worker = true;
  return V8HostEngineWarmup(worker, api);
}

sbox_status SBOX_CDECL BrokerRun(sbox_broker broker,
                                const sbox_broker_api* api,
                                const sbox_broker_start* start) {
  return V8HostBrokerRun(broker, api, start);
}

const sbox_plugin_api kPlugin = {
    sizeof(sbox_plugin_api),       SBOX_ABI_VERSION,      &V8HostEngineConfigure,
    &BrokerRun,                &Warmup,               &V8HostEngineRun,
    &V8HostEngineShutdown,
};

}  // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = WorkerDelayLoadGuard;

extern "C" __declspec(dllexport) sbox_status SBOX_CDECL sbox_plugin_main(
    uint32_t host_abi_version, const sbox_plugin_api** out_plugin) {
  if (!out_plugin)
    return sbox_status_invalid_arg;
  *out_plugin = nullptr;
  if (host_abi_version != SBOX_ABI_VERSION)
    return sbox_status_version_mismatch;
  *out_plugin = &kPlugin;
  return sbox_status_ok;
}
