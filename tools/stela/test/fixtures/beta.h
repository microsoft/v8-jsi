/* beta.h: a test header for a second library whose beta_stela.h comes from
 * a newer (v2) template, to show that libraries and Stela versions mix. */
#ifndef BETA_H_
#define BETA_H_

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "beta_stela.h"

#ifndef BETA_CDECL
#ifdef _WIN32
#define BETA_CDECL __cdecl
#else
#define BETA_CDECL
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

BETA_ENUM(int32_t, beta_status){
    beta_status_ok = 0,
    beta_status_failed = 0x7001, ///< 0x7000: failed
};

BETA_FLAGS(uint32_t, beta_open_flags){
    beta_open_flags_none = 0,
    beta_open_flags_shared = 1u << 0,
};

typedef struct beta_thing_s* beta_thing;

/// Creates a thing. Callable from any thread.
BETA_SINCE(2)
BETA_THREAD_ANY
beta_status BETA_CDECL beta_thing_create(beta_open_flags flags,
                                         bool shared,
                                         beta_thing* out_thing);

BETA_NULLABLE
const char* BETA_CDECL beta_thing_get_label(BETA_NULLABLE beta_thing thing);

#ifdef __cplusplus
}
#endif

#endif /* BETA_H_ */
