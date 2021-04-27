// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#ifndef SRC_V8WINDOWS_H
#define SRC_V8WINDOWS_H

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif // NOMINMAX

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif // WIN32_LEAN_AND_MEAN

#include <windows.h>
#include "etw/tracing.h"

#endif

#endif // SRC_V8WINDOWS_H
