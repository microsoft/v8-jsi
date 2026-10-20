#ifndef DEMO_EXT_H_
#define DEMO_EXT_H_

#include "demo.h"

#define DEMO_EXT_VERSION 1u

typedef struct demo_ext_info {
  uint32_t struct_size;
  demo_status last_status;
  demo_session session;
} demo_ext_info;

demo_status DEMO_CDECL demo_ext_query(demo_session session, DEMO_OUT demo_ext_info* out_info);

#endif /* DEMO_EXT_H_ */
