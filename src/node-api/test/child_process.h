// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once
#ifndef CHILD_PROCESS_H_
#define CHILD_PROCESS_H_

#include <string>
#include <string_view>
#include <vector>

namespace node_api_tests {

struct ProcessResult {
  uint32_t status;
  std::string std_output;
  std::string std_error;
};

ProcessResult spawnSync(std::string_view command,
                        std::vector<std::string> args);

}  // namespace node_api_tests

#endif  // !CHILD_PROCESS_H_