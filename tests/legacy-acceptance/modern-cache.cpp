// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include <windows.h>
#include "v8_jsi_abi.h"
#include <fstream>
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
  if (argc != 3) return 1;
  auto library = LoadLibraryA(argv[1]);
  if (!library) return 2;
  CacheProducer producer;
  producer.setCache = reinterpret_cast<v8_jsi_abi_config_set_script_cache_fn>(
      GetProcAddress(library, "v8_jsi_abi_config_set_script_cache"));
  auto create = reinterpret_cast<v8_jsi_abi_runtime_create_fn>(
      GetProcAddress(library, "v8_jsi_abi_runtime_create"));
  if (!producer.setCache || !create) return 3;
  jsi_runtime *runtime{};
  if (create(JSI_ABI_VERSION, CacheProducer::configure, &producer, &runtime) != jsi_status_ok) return 4;
  const jsi_buffer_api api{[](jsi_buffer *) {}};
  jsi_buffer source{&api, reinterpret_cast<const uint8_t *>("17"), 2};
  jsi_value result{};
  const auto status = runtime->api->evaluate_javascript_source(runtime, &source, "retained-cache.js", 17, &result);
  runtime->api->release(runtime);
  if (status != jsi_status_ok || producer.bytes.size() <= 16) return 5;
  std::ofstream output(argv[2], std::ios::binary);
  output.write(reinterpret_cast<const char *>(producer.bytes.data()), static_cast<std::streamsize>(producer.bytes.size()));
  return output.good() ? 0 : 6;
}
