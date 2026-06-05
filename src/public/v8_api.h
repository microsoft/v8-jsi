// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

/* ===========================================================================
 * EXPERIMENTAL API - NOT YET ABI-STABLE
 *
 * This API is experimental and subject to change. Although it is C-based, it is
 * NOT ABI-safe yet: function signatures, struct fields, and enum values may
 * change without notice while the API remains in experimental mode. Do not
 * depend on binary compatibility across builds. Once the API leaves
 * experimental mode it will become ABI-stable and this notice will be removed.
 * =========================================================================== */

#ifndef V8_API_H_
#define V8_API_H_

#include "js_runtime_api.h"

EXTERN_C_START

JSR_API v8_config_enable_multithreading(jsr_config config, bool value);

JSR_API v8_platform_dispose();

EXTERN_C_END

#endif // V8_API_H_
