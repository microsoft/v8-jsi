// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include <gtest/gtest.h>
#include <jsi/jsi.h>
#include <thread>
#include "jsi/test/testlib.h"
#include "node-api-jsi/ApiLoaders/V8Api.h"
#include "node-api-jsi/NodeApiJsiRuntime.h"
#include "public/ScriptStore.h"
#include "public/V8JsiRuntime.h"
#include "public/v8_api.h"

using namespace Microsoft::NodeApiJsi;

namespace facebook::jsi {

bool isAbiRuntime() { return false; }

std::vector<facebook::jsi::RuntimeFactory> runtimeGenerators() {
  return std::vector<facebook::jsi::RuntimeFactory>{
#if defined(JSI_V8_IMPL)
      []() -> std::unique_ptr<facebook::jsi::Runtime> {
        v8runtime::V8RuntimeArgs args;
        args.flags.explicitMicrotaskPolicy = true;
        args.flags.enableGCApi = true;
        return v8runtime::makeV8Runtime(std::move(args));
      },
#endif
#if defined(_WIN32) // Node-API not supported on POSIX (LibLoader not implemented)
      []() -> std::unique_ptr<facebook::jsi::Runtime> {
        V8Api *v8Api = V8Api::fromLib();
        V8Api::setCurrent(v8Api);

        jsr_config config{};
        jsr_runtime runtime{};
        napi_env env{};
        v8Api->jsr_create_config(&config);
        v8Api->jsr_config_enable_gc_api(config, true);
        v8Api->jsr_create_runtime(config, &runtime);
        v8Api->jsr_delete_config(config);
        v8Api->jsr_runtime_get_node_api_env(runtime, &env);

        NodeApiEnvScope envScope{env};

        return makeNodeApiJsiRuntime(env, v8Api, [runtime]() { V8Api::current()->jsr_delete_runtime(runtime); });
      }
#endif
  };
};

} // namespace facebook::jsi

using namespace facebook::jsi;

TEST(Basic, CreateOneRuntimes) {
  v8runtime::V8RuntimeArgs args;
  args.flags.enableInspector = true;
  args.flags.enableGCApi = true; // It can be enabled only once per process and we need it later for Node-API tests.
  auto runtime = v8runtime::makeV8Runtime(std::move(args));

  runtime->evaluateJavaScript(std::make_unique<facebook::jsi::StringBuffer>("x = 1"), "");
  EXPECT_EQ(runtime->global().getProperty(*runtime, "x").getNumber(), 1);
}

TEST(Basic, CreateManyRuntimes) {
  for (size_t i = 0; i < 100; i++) {
    v8runtime::V8RuntimeArgs args;
    args.flags.enableInspector = true;
    auto runtime = v8runtime::makeV8Runtime(std::move(args));

    runtime->evaluateJavaScript(std::make_unique<facebook::jsi::StringBuffer>("x = 1"), "");
    EXPECT_EQ(runtime->global().getProperty(*runtime, "x").getNumber(), 1);
  }
}

TEST(Basic, MultiThreadIsolate) {
  v8runtime::V8RuntimeArgs args;
  args.flags.enableMultiThread = true;
  auto runtime = v8runtime::makeV8Runtime(std::move(args));

  runtime->evaluateJavaScript(
      std::make_unique<facebook::jsi::StringBuffer>("x = {1:2, '3':4, 5:'six', 'seven':['eight', 'nine']}"), "");

  Runtime &rt = *runtime;

  Object x = rt.global().getPropertyAsObject(rt, "x");
  EXPECT_EQ(x.getProperty(rt, "1").getNumber(), 2);

  std::vector<std::thread> vec_thr;
  for (size_t i = 0; i < 10; ++i) {
    vec_thr.push_back(std::thread([&]() {
      EXPECT_EQ(x.getProperty(rt, PropNameID::forAscii(rt, "1")).getNumber(), 2);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      EXPECT_EQ(x.getProperty(rt, "3").getNumber(), 4);
    }));
  }

  for (size_t i = 0; i < vec_thr.size(); ++i) {
    vec_thr.at(i).join();
  }
}

TEST(Basic, DestroyOnWorkerPreservesSiblingAndCreatingThreadReuse) {
  v8runtime::V8RuntimeArgs args;
  args.flags.enableMultiThread = true;
  auto first = v8runtime::makeV8Runtime(std::move(args));
  auto sibling = v8runtime::makeV8Runtime({});
  std::thread worker([runtime = std::move(first)]() mutable {
    EXPECT_EQ(runtime->evaluateJavaScript(std::make_shared<StringBuffer>("42"), "").getNumber(), 42);
    runtime.reset();
  });
  worker.join();
  EXPECT_EQ(sibling->evaluateJavaScript(std::make_shared<StringBuffer>("17"), "").getNumber(), 17);
  auto reused = v8runtime::makeV8Runtime({});
  sibling.reset();
  EXPECT_EQ(reused->evaluateJavaScript(std::make_shared<StringBuffer>("23"), "").getNumber(), 23);
}

TEST(Basic, DestroyLastRuntimeOnWorkerAllowsFreshRuntimeOnCreatingThread) {
  v8runtime::V8RuntimeArgs args;
  args.flags.enableMultiThread = true;
  auto runtime = v8runtime::makeV8Runtime(std::move(args));
  std::thread worker([runtime = std::move(runtime)]() mutable {
    EXPECT_EQ(runtime->evaluateJavaScript(std::make_shared<StringBuffer>("42"), "").getNumber(), 42);
    runtime.reset();
  });
  worker.join();
  auto fresh = v8runtime::makeV8Runtime({});
  EXPECT_EQ(fresh->evaluateJavaScript(std::make_shared<StringBuffer>("17"), "").getNumber(), 17);
}

#ifdef _WIN32 // Node-API not supported on POSIX (LibLoader not implemented)
TEST(Basic, MultiThreadIsolateNodeApi) {
  V8Api *v8Api = V8Api::fromLib();
  V8Api::setCurrent(v8Api);

  jsr_config config{};
  jsr_runtime runtime{};
  napi_env env{};
  v8Api->jsr_create_config(&config);
  v8Api->jsr_config_enable_gc_api(config, true);
  v8Api->v8_config_enable_multithreading(config, true);
  v8Api->jsr_create_runtime(config, &runtime);
  v8Api->jsr_delete_config(config);
  v8Api->jsr_runtime_get_node_api_env(runtime, &env);

  std::unique_ptr<facebook::jsi::Runtime> jsiRuntime;
  {
    NodeApiEnvScope envScope{env};
    jsiRuntime = makeNodeApiJsiRuntime(env, v8Api, [runtime]() { V8Api::current()->jsr_delete_runtime(runtime); });
  }

  jsiRuntime->evaluateJavaScript(
      std::make_unique<facebook::jsi::StringBuffer>("x = {1:2, '3':4, 5:'six', 'seven':['eight', 'nine']}"), "");

  Runtime &rt = *jsiRuntime;

  Object x = rt.global().getPropertyAsObject(rt, "x");
  EXPECT_EQ(x.getProperty(rt, "1").getNumber(), 2);

  std::vector<std::thread> vec_thr;
  for (size_t i = 0; i < 10; ++i) {
    vec_thr.push_back(std::thread([&]() {
      V8Api::setCurrent(v8Api);
      EXPECT_EQ(x.getProperty(rt, PropNameID::forAscii(rt, "1")).getNumber(), 2);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      EXPECT_EQ(x.getProperty(rt, "3").getNumber(), 4);
    }));
  }

  for (size_t i = 0; i < vec_thr.size(); ++i) {
    vec_thr.at(i).join();
  }
}
#endif

namespace {
class CacheBuffer final : public Buffer {
 public:
  explicit CacheBuffer(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  const uint8_t *data() const override { return bytes_.data(); }
  size_t size() const override { return bytes_.size(); }
 private:
  std::vector<uint8_t> bytes_;
};
class CacheStore final : public PreparedScriptStore {
 public:
  std::vector<uint8_t> bytes;
  int loads{0}, stores{0};
  std::shared_ptr<const Buffer> tryGetPreparedScript(
      const ScriptSignature &, const JSRuntimeSignature &, const char *) noexcept override {
    ++loads;
    return bytes.empty() ? nullptr : std::make_shared<CacheBuffer>(bytes);
  }
  void persistPreparedScript(std::shared_ptr<const Buffer> buffer,
      const ScriptSignature &, const JSRuntimeSignature &, const char *) noexcept override {
    ++stores;
    bytes.assign(buffer->data(), buffer->data() + buffer->size());
  }
};
}
TEST(LegacyCodeCache, MismatchedVersionAndFlagsRecompileFromSource) {
  // SerializedCodeData stores version and flag hashes at byte offsets 4 and 12.
  for (size_t offset : {4u, 12u}) {
    SCOPED_TRACE(offset);
    auto store = std::make_shared<CacheStore>();
    auto evaluate = [&](const char *source) {
      v8runtime::V8RuntimeArgs args;
      args.flags.enableGCApi = true;
      args.preparedScriptStore = store;
      auto runtime = v8runtime::makeV8Runtime(std::move(args));
      return runtime->evaluateJavaScript(std::make_shared<StringBuffer>(source), "cache.js").getNumber();
    };
    EXPECT_EQ(evaluate("17"), 17);
    ASSERT_EQ(store->stores, 1);
    ASSERT_GT(store->bytes.size(), 16u);
    // V8's source hash records length, not contents. A same-length source
    // makes cache acceptance observable independently of the rejection path.
    EXPECT_EQ(evaluate("42"), 17);
    store->bytes[offset] ^= 0xff;
    EXPECT_EQ(evaluate("42"), 42);
    EXPECT_EQ(store->loads, 3);
  }
}

TEST(LegacyInstrumentation, EtwEnabledRuntimePreservesMicrotasksAndExceptions) {
  v8runtime::V8RuntimeArgs args;
  args.flags.enableSystemInstrumentation = true;
  args.flags.enableJitTracing = true;
  args.flags.enableGCTracing = true;
  args.flags.enableMessageTracing = true;
  args.flags.enableGCApi = true;
  args.flags.explicitMicrotaskPolicy = true;
  auto runtime = v8runtime::makeV8Runtime(std::move(args));
  EXPECT_EQ(runtime->evaluateJavaScript(std::make_shared<StringBuffer>(
      "var result=0; Promise.resolve(42).then(x=>result=x); result"), "etw.js").getNumber(), 0);
  runtime->drainMicrotasks();
  EXPECT_EQ(runtime->global().getProperty(*runtime, "result").getNumber(), 42);
  EXPECT_THROW(runtime->evaluateJavaScript(std::make_shared<StringBuffer>(
      "throw new Error('instrumented exception')"), "etw-error.js"), JSError);
  EXPECT_EQ(runtime->evaluateJavaScript(std::make_shared<StringBuffer>("gc(); 42"), "etw-gc.js").getNumber(), 42);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
