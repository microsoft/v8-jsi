#ifndef V8SBHOST_ENGINE_H_
#define V8SBHOST_ENGINE_H_

#include "sbox.h"

sbox_status SBOX_CDECL V8SbHostEngineConfigure(sbox_config cfg,
                                             const sbox_config_api* api,
                                             const void* configure_data,
                                             size_t configure_data_size);
sbox_status SBOX_CDECL V8SbHostEngineWarmup(sbox_worker worker,
                                          const sbox_worker_api* api);
sbox_status SBOX_CDECL V8SbHostEngineRun(sbox_worker worker,
                                       const sbox_worker_api* api);
void SBOX_CDECL V8SbHostEngineShutdown(sbox_worker worker);

#endif  // V8SBHOST_ENGINE_H_
