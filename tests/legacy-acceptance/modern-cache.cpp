// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include <windows.h>
#include "v8_jsi_abi.h"
#include <fstream>
#include <cstdio>
#include <cstring>
#include <vector>

struct CacheProducer {
  v8_jsi_abi_config_set_script_cache_fn setCache{};
  std::vector<uint8_t> bytes;
  static void JSI_ABI_CDECL store(void *data, const char *, uint64_t, const char *, uint64_t,
      const char *, const uint8_t *bytes, size_t size, jsi_data_delete_cb release, void *hint) {
    static_cast<CacheProducer *>(data)->bytes.assign(bytes, bytes + size);
    if (release) release(hint, const_cast<uint8_t *>(bytes));
  }
  static jsi_status JSI_ABI_CDECL configure(void *data, jsi_config config) {
    auto &self = *static_cast<CacheProducer *>(data);
    return self.setCache(config, nullptr, store, &self, nullptr, nullptr);
  }
};
int main(int argc, char **argv) {
  if (argc != 4 || (std::strcmp(argv[3], "same-source") != 0 && std::strcmp(argv[3], "flag-mismatch") != 0)) {
    std::fprintf(stderr, "usage: modern-cache <v8jsi2.dll> <cache file> same-source|flag-mismatch\n"); return 1;
  }
  auto library = LoadLibraryA(argv[1]);
  if (!library) { std::fprintf(stderr, "Cannot load %s: Windows error %lu (check DLL architecture and dependencies)\n", argv[1], GetLastError()); return 2; }
  if (std::strcmp(argv[3], "flag-mismatch") == 0) {
    const auto setFlags = reinterpret_cast<v8_jsi_abi_set_v8_flags_fn>(
        GetProcAddress(library, "v8_jsi_abi_set_v8_flags"));
    char program[] = "modern-cache";
    char flag[] = "--no-lazy";
    char *flags[]{program, flag};
    size_t count = 2;
    if (!setFlags || setFlags(&count, flags, false) != jsi_status_ok) {
      std::fprintf(stderr, "Cannot set flag-mismatch cache fixture flags\n"); return 3;
    }
  }
  CacheProducer producer;
  producer.setCache = reinterpret_cast<v8_jsi_abi_config_set_script_cache_fn>(
      GetProcAddress(library, "v8_jsi_abi_config_set_script_cache"));
  auto create = reinterpret_cast<v8_jsi_abi_runtime_create_fn>(
      GetProcAddress(library, "v8_jsi_abi_runtime_create"));
  if (!producer.setCache || !create) { std::fprintf(stderr, "Modern DLL is missing required ABI exports\n"); return 3; }
  jsi_runtime *runtime{};
  const auto created = create(JSI_ABI_VERSION, CacheProducer::configure, &producer, &runtime);
  if (created != jsi_status_ok) { std::fprintf(stderr, "Modern runtime creation failed: status %d\n", static_cast<int>(created)); return 4; }
  const jsi_buffer_api api{[](jsi_buffer *) {}};
  jsi_buffer source{&api, reinterpret_cast<const uint8_t *>("17"), 2};
  jsi_value result{};
  const auto status = runtime->api->evaluate_javascript_source(runtime, &source, "retained-cache.js", 17, &result);
  runtime->api->release(runtime);
  if (status != jsi_status_ok || result.kind != jsi_valuekind_number || result.data.number != 17 || producer.bytes.size() <= 16) { std::fprintf(stderr, "Modern cache production failed: status %d, %zu bytes\n", static_cast<int>(status), producer.bytes.size()); return 5; }
  std::ofstream output(argv[2], std::ios::binary);
  output.write(reinterpret_cast<const char *>(producer.bytes.data()), static_cast<std::streamsize>(producer.bytes.size()));
  if (!output.good()) { std::fprintf(stderr, "Cannot write cache file: %s\n", argv[2]); return 6; }
  return 0;
}
