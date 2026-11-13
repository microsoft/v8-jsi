// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// A consumer of the Microsoft.JavaScript.LibNode package, built only from the
// package's headers and libraries. It runs Node.js through the embedding API,
// like the node executable: libnode_smoke <script> [args...]

#include <stdio.h>

#include "node_embedding_api.h"

// Implemented in smoke_cpp.cc with the package's C++ wrapper header.
const char* smoke_last_error_message(void);

int main(int argc, const char* argv[]) {
  node_embedding_status status = node_embedding_main_run(
      NODE_EMBEDDING_VERSION, argc, argv, NULL, NULL, NULL, NULL);
  if (status == node_embedding_status_ok) {
    return 0;
  }

  fprintf(stderr, "libnode_smoke: %s\n", smoke_last_error_message());
  if ((status & node_embedding_status_error_exit_code) != 0) {
    return status & ~node_embedding_status_error_exit_code;
  }
  return 1;
}
