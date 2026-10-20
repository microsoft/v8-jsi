#ifndef DEMO_H_
#define DEMO_H_

#include <stddef.h>
#include <stdint.h>

#include "demo_stela.h"

#ifdef _WIN32
#define DEMO_CDECL __cdecl
#else
#define DEMO_CDECL
#endif

#define DEMO_ABI_VERSION 3u

/** Status codes. */
DEMO_ENUM(int32_t, demo_status) {
  demo_status_ok = 0,
  demo_status_invalid_arg = 0x1001,
  demo_status_failed = 0x7001,
};

DEMO_FLAGS(uint32_t, demo_flags) {
  demo_flags_none = 0,
  demo_flags_fast = 1u << 0,
  demo_flags_last = 1u << 30,
};

/** An open session; the struct stays incomplete. */
typedef struct demo_session_s* demo_session;

typedef struct demo_options {
  uint32_t struct_size;
  demo_flags flags;
  uint64_t budget;
  DEMO_NULLABLE const char* name;
} demo_options;

typedef union demo_value {
  uint64_t bits;
  double number;
  void* pointer;
} demo_value;

typedef struct {
  int32_t x;
  int32_t y;
} demo_point;

typedef struct demo_bits {
  uint32_t struct_size;
  unsigned ready : 1;
  unsigned mode : 3;
  uint16_t tail;
} demo_bits;

#define DEMO_TAG(name) name##_s

/** Its tag comes from a macro. */
typedef struct DEMO_TAG(demo_tagged) {
  uint32_t struct_size;
} demo_tagged;

typedef struct demo_slot {
  uint32_t struct_size;
  union {
    int32_t number;
    const char* text;
  } data;
  enum { demo_slot_small, demo_slot_large } size_class;
} demo_slot;

#ifdef _WIN64
/** Only 64-bit targets have it. */
typedef struct demo_wide {
  uint64_t first;
  uint64_t second;
} demo_wide;
#endif

typedef void(DEMO_CDECL* demo_event_cb)(DEMO_NULLABLE void* user_data, demo_status status);

demo_status DEMO_CDECL demo_open(
    const demo_options* options,
    DEMO_OUT demo_session* out_session);

#endif /* DEMO_H_ */
