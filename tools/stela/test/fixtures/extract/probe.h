#ifndef PROBE_H_
#define PROBE_H_

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "probe_stela.h"

#ifdef _WIN32
#define PROBE_CDECL __cdecl
#else
#define PROBE_CDECL
#endif

#define PROBE_ABI_VERSION 7u
#define PROBE_BIG_VALUE (1ull << 40)
#define PROBE_ALIAS PROBE_ABI_VERSION
#define PROBE_TEXT "not an integer"
#define PROBE_EMPTY
#define PROBE_ADD(a, b) ((a) + (b))

/** A fixed-width status. */
PROBE_ENUM(int32_t, probe_status) {
  probe_status_ok = 0,
  probe_status_failed = 0x7001, ///< An unexpected failure.
};

PROBE_FLAGS(uint32_t, probe_flags) {
  probe_flags_none = 0,
  probe_flags_first = 1u << 0,
  probe_flags_last = 1u << 30,
};

typedef struct probe_record {
  uint32_t struct_size;
  PROBE_NULLABLE const char* name; ///< A borrowed UTF-8 name.
  uint8_t bytes[4];
  unsigned ready : 1;
} probe_record;

/** A union in the public layout. */
typedef union probe_value {
  uint64_t bits;
  double number;
} probe_value;

typedef void(PROBE_CDECL* probe_event_cb)(
    PROBE_NULLABLE const char* text,
    PROBE_SCOPE_CALL void* user_data);

typedef void(*probe_default_cb)(void);
typedef probe_event_cb probe_event_alias_cb;
typedef probe_default_cb probe_default_alias_cb;

// Plain comments must not attach.
typedef uint32_t probe_plain_comment;

/**
 * Runs a probe.
 * \param record the record to update.
 * \param out_value receives the result.
 */
PROBE_SINCE(2)
probe_status PROBE_CDECL probe_run(
    PROBE_INOUT probe_record* record,
    probe_flags flags,
    PROBE_OPTIONAL PROBE_OUT probe_value* out_value,
    PROBE_SCOPE_ASYNC probe_event_cb on_event,
    PROBE_NULLABLE void* user_data);

/// A variadic library-wide function.
probe_status PROBE_CDECL probe_log(PROBE_ZERO_TERMINATED const char* format, ...);

probe_status probe_default_call(probe_default_cb callback);

#endif /* PROBE_H_ */
