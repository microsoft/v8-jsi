#ifndef V8HOST_ENGINE_H_
#define V8HOST_ENGINE_H_

#include "sbox.h"

sbox_status SBOX_CDECL V8HostEngineConfigure(sbox_config cfg,
                                             const sbox_config_api* api,
                                             const void* configure_data,
                                             size_t configure_data_size);
sbox_status SBOX_CDECL V8HostEngineWarmup(sbox_worker worker,
                                          const sbox_worker_api* api);
sbox_status SBOX_CDECL V8HostEngineRun(sbox_worker worker,
                                       const sbox_worker_api* api);
void SBOX_CDECL V8HostEngineShutdown(sbox_worker worker);

#endif  // V8HOST_ENGINE_H_
