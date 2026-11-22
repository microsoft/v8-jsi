// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#ifndef V8_API_H_
#define V8_API_H_

#include "js_runtime_api.h"

EXTERN_C_START

JSR_API v8_config_enable_multithreading(jsr_config config, bool value);

JSR_API v8_platform_dispose();

EXTERN_C_END

#endif // V8_API_H_
