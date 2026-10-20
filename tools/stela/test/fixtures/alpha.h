/* alpha.h: a test header that uses every Stela v1 annotation and the
 * library's ENUM and FLAGS macros, through alpha_stela.h. */
#ifndef ALPHA_H_
#define ALPHA_H_

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "alpha_stela.h"

#ifndef ALPHA_CDECL
#ifdef _WIN32
#define ALPHA_CDECL __cdecl
#else
#define ALPHA_CDECL
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Banded status codes; 0 is success. */
ALPHA_ENUM(int32_t, alpha_status){
    alpha_status_ok = 0,
    alpha_status_invalid_arg = 0x1001, ///< 0x1000: caller contract
    alpha_status_internal = 0x7001,    ///< 0x7000: unexpected
};

ALPHA_FLAGS(uint32_t, alpha_flags){
    alpha_flags_none = 0,
    alpha_flags_first = 1u << 0,
    alpha_flags_last = 1u << 30,
};

typedef struct alpha_session_s* alpha_session;

typedef void(ALPHA_CDECL* alpha_event_cb)(void* user_data,
                                          ALPHA_NULLABLE const char* text);
typedef void(ALPHA_CDECL* alpha_destroy_cb)(void* user_data);

typedef struct alpha_record {
  uint32_t struct_size;
  ALPHA_NULLABLE ALPHA_TRANSFER_NONE void* cookie; ///< borrowed
  bool enabled;
} alpha_record;

/**
 * Opens a session.
 * \param name the session's name.
 * \param out_session receives the new session.
 */
ALPHA_SINCE(1)
alpha_status ALPHA_CDECL alpha_session_open(
    ALPHA_ZERO_TERMINATED ALPHA_ENCODING(utf8) const char* name,
    ALPHA_OUT ALPHA_TRANSFER_FULL alpha_session* out_session);

ALPHA_DEPRECATED
alpha_status ALPHA_CDECL alpha_session_read(
    alpha_session session,
    ALPHA_OUT ALPHA_COUNT(capacity) uint8_t* buffer,
    size_t capacity,
    ALPHA_OPTIONAL size_t* out_written);

ALPHA_EXPERIMENTAL
alpha_status ALPHA_CDECL alpha_session_update(alpha_session session,
                                              ALPHA_INOUT alpha_record* record);

ALPHA_ALLOW(bool_type)
ALPHA_ATTR("alpha.thread=any")
alpha_status ALPHA_CDECL alpha_session_set_flags(alpha_session session,
                                                 alpha_flags flags,
                                                 int enabled);

alpha_status ALPHA_CDECL alpha_session_set_handler(
    alpha_session session,
    ALPHA_NULLABLE ALPHA_SCOPE_OBJECT alpha_event_cb on_event,
    ALPHA_NULLABLE void* user_data);

alpha_status ALPHA_CDECL alpha_session_watch(
    alpha_session session,
    ALPHA_SCOPE_NOTIFIED ALPHA_CLOSURE(context) ALPHA_DESTROY(on_destroy)
        alpha_event_cb on_event,
    alpha_destroy_cb on_destroy,
    ALPHA_NULLABLE void* context);

alpha_status ALPHA_CDECL alpha_session_post(
    alpha_session session,
    ALPHA_SCOPE_ASYNC alpha_event_cb on_done,
    void* user_data);

alpha_status ALPHA_CDECL alpha_set_logger(
    ALPHA_SCOPE_FOREVER alpha_event_cb on_log,
    void* user_data);

alpha_status ALPHA_CDECL alpha_session_visit(
    alpha_session session,
    ALPHA_SCOPE_CALL alpha_event_cb visitor,
    void* user_data);

ALPHA_NULLABLE
const char* ALPHA_CDECL alpha_session_get_name(alpha_session session);

#ifdef __cplusplus
}
#endif

#endif /* ALPHA_H_ */
