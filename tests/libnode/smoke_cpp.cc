// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Compiles the package's header-only C++ wrapper in a consumer.

#include "node_embedding_api_cpp.h"

extern "C" const char* smoke_last_error_message(void) {
  return node::embedding::NodeErrorInfo::GetLastErrorMessage();
}
