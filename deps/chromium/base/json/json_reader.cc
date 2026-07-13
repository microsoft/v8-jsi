// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// [v8-jsi sandbox: Rust dropped] Upstream routes base::JSONReader through the
// Rust serde_json_lenient parser, which forces a Rust toolchain into the build.
// The sandbox engine (sbox.dll) never reaches JSONReader (verified: 0 bytes in
// the linked image), so this file is reduced to a C++-only no-op that keeps the
// JSONReader API linkable without any Rust dependency. If a shared-base consumer
// ever needs JSON parsing, restore a real C++ parser here instead of the stub.

#include "base/json/json_reader.h"

#include <string_view>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"

namespace base {

std::string JSONReader::Error::ToString() const {
  return base::StrCat({"line ", base::NumberToString(line), ", column ",
                       base::NumberToString(column), ": ", message});
}

// static
std::optional<Value> JSONReader::Read(std::string_view json,
                                      int options,
                                      size_t max_depth) {
  return std::nullopt;
}

// static
std::optional<DictValue> JSONReader::ReadDict(std::string_view json,
                                              int options,
                                              size_t max_depth) {
  return std::nullopt;
}

// static
std::optional<ListValue> JSONReader::ReadList(std::string_view json,
                                              int options,
                                              size_t max_depth) {
  return std::nullopt;
}

// static
JSONReader::Result JSONReader::ReadAndReturnValueWithError(
    std::string_view json,
    int options) {
  return base::unexpected(JSONReader::Error{
      .message = "JSON parsing disabled (v8-jsi sandbox: Rust dropped)",
      .line = 0,
      .column = 0,
  });
}

}  // namespace base
