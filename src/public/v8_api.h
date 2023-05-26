// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#ifndef V8_API_H_
#define V8_API_H_

#include "js_runtime_api.h"

EXTERN_C_START

//=============================================================================
// jsr_config
//=============================================================================

JSR_API v8_config_enable_multithreading(jsr_config config, bool value);

EXTERN_C_END

#endif // V8_API_H_
