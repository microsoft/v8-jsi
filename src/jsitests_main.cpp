// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// JSI tests main file - uses V8JsiRuntime without Node-API

#include <gtest/gtest.h>
#include <jsi/jsi.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "jsi/test/testlib.h"
#include "public/ScriptStore.h"
#include "public/V8JsiRuntime.h"
#include "jsi_abi/JsiAbiRuntime.h"
#include "jsi_abi/jsi_abi_helpers.h"
#include "jsi_abi/v8_jsi_config.h"
#include "js_runtime_api.h"

// W4 test-only hook (declared in jsi_abi_v8.cpp). Posts a task to the
// runtime's foreground task runner — used by JsiAbiTaskRunner.LifecycleAndPostTask
// to exercise the post-task trampoline without an inspector session.
extern "C" JSI_API void JSI_CDECL v8_jsi_test_post_foreground_task(
    jsi_runtime *runtime,
    void (JSI_CDECL *task_run_cb)(void *),
    void *task_data);

namespace facebook::jsi {

// Runtime index constants for identifying which runtime is being tested
constexpr size_t kDirectRuntimeIndex = 0;
constexpr size_t kAbiRuntimeIndex = 1;

std::vector<facebook::jsi::RuntimeFactory> runtimeGenerators() {
  return std::vector<facebook::jsi::RuntimeFactory>{
      // Index 0: Direct V8JsiRuntime (C++ interface)
      []() -> std::shared_ptr<facebook::jsi::Runtime> {
        v8runtime::V8RuntimeArgs args;
        args.flags.explicitMicrotaskPolicy = true;
        args.flags.enableGCApi = true;
        return v8runtime::makeV8Runtime(std::move(args));
      },
      // Index 1: V8JsiRuntime via ABI-stable C interface
      []() -> std::shared_ptr<facebook::jsi::Runtime> {
        return ::jsi::abi::makeJsiAbiRuntime(&v8_create_runtime);
      },
  };
}

// Helper to check if current test parameter is the ABI runtime
// Use in tests: if (isAbiRuntime()) GTEST_SKIP() << "Not yet supported in ABI runtime";
bool isAbiRuntime() {
  auto* testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
  if (!testInfo) return false;

  // Parameter index is in the test name suffix: TestName/0 or TestName/1
  std::string name = testInfo->name();
  size_t slashPos = name.rfind('/');
  if (slashPos != std::string::npos && slashPos + 1 < name.size()) {
    return name.substr(slashPos + 1) == std::to_string(kAbiRuntimeIndex);
  }
  return false;
}

} // namespace facebook::jsi

using namespace facebook::jsi;

TEST(Basic, CreateOneRuntimes) {
  v8runtime::V8RuntimeArgs args;
  args.flags.enableInspector = false;  // Inspector disabled for now
  args.flags.enableGCApi = true;
  auto runtime = v8runtime::makeV8Runtime(std::move(args));

  runtime->evaluateJavaScript(std::make_unique<facebook::jsi::StringBuffer>("x = 1"), "");
  EXPECT_EQ(runtime->global().getProperty(*runtime, "x").getNumber(), 1);
}

TEST(Basic, CreateManyRuntimes) {
  for (size_t i = 0; i < 100; i++) {
    v8runtime::V8RuntimeArgs args;
    args.flags.enableInspector = false;
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

// W1 smoke test: end-to-end exercise of the v8_jsi_config plumbing.
// Populates a factory-owned config via the public C setters inside the
// configure callback, and confirms a basic eval works. Does not (and cannot)
// verify that V8 actually applied the requested flags — V8 platform-flag state
// is process-global and first-init-only, and these tests share a process.
TEST(JsiAbiConfig, RuntimeFromConfigEvaluates) {
  // The factory owns the jsi_config; we populate it inside the configure
  // callback (pull model). Touch a representative subset of every field group
  // to confirm linkage.
  jsi_configure_runtime_cb configure = [](void * /*cb_data*/,
                                          jsi_config cfg) -> jsi_error_code {
    v8_jsi_config_set_initial_heap_size(cfg, 0);
    v8_jsi_config_set_maximum_heap_size(cfg, 0);
    v8_jsi_config_set_thread_pool_size(cfg, 0);
    v8_jsi_config_set_debugger_runtime_name(cfg, "w1-smoke");
    v8_jsi_config_enable_inspector(cfg, false);
    v8_jsi_config_set_inspector_port(cfg, 9223);
    v8_jsi_config_set_inspector_break_on_start(cfg, false);
    v8_jsi_config_set_explicit_microtask_policy(cfg, true);
    v8_jsi_config_set_ignore_unhandled_promises(cfg, false);
    v8_jsi_config_enable_gc_api(cfg, true);
    v8_jsi_config_enable_multi_thread(cfg, false);
    v8_jsi_config_enable_jit_tracing(cfg, false);
    v8_jsi_config_enable_message_tracing(cfg, false);
    v8_jsi_config_enable_gc_tracing(cfg, false);
    v8_jsi_config_enable_system_instrumentation(cfg, false);
    return jsi_no_error;
  };

  std::shared_ptr<facebook::jsi::Runtime> runtime =
      ::jsi::abi::makeJsiAbiRuntime(&v8_create_runtime, configure);

  facebook::jsi::Value result = runtime->evaluateJavaScript(
      std::make_shared<facebook::jsi::StringBuffer>("1 + 2"), "w1-smoke.js");
  EXPECT_TRUE(result.isNumber());
  EXPECT_EQ(result.getNumber(), 3.0);
}

// W3 round-trip test: stub PreparedScriptStore wired through V8RuntimeArgs.
// Confirms the full chain — consumer's PreparedScriptStore → V8ScriptCache
// adapter → C callbacks → DLL → V8 code cache → C callbacks → adapter →
// consumer's persistPreparedScript / tryGetPreparedScript.
namespace {

class StubPreparedScript final : public facebook::jsi::Buffer {
 public:
  explicit StubPreparedScript(std::vector<uint8_t> bytes)
      : bytes_(std::move(bytes)) {}
  const uint8_t *data() const override { return bytes_.data(); }
  size_t size() const override { return bytes_.size(); }

 private:
  std::vector<uint8_t> bytes_;
};

class StubPreparedScriptStore final
    : public facebook::jsi::PreparedScriptStore {
 public:
  std::shared_ptr<const facebook::jsi::Buffer> tryGetPreparedScript(
      const facebook::jsi::ScriptSignature &scriptSignature,
      const facebook::jsi::JSRuntimeSignature & /*runtimeSignature*/,
      const char * /*prepareTag*/) noexcept override {
    ++loadCount;
    auto it = entries.find(makeKey(scriptSignature));
    if (it == entries.end())
      return nullptr;
    return std::make_shared<StubPreparedScript>(it->second);
  }

  void persistPreparedScript(
      std::shared_ptr<const facebook::jsi::Buffer> preparedScript,
      const facebook::jsi::ScriptSignature &scriptMetadata,
      const facebook::jsi::JSRuntimeSignature & /*runtimeMetadata*/,
      const char * /*prepareTag*/) noexcept override {
    ++storeCount;
    std::vector<uint8_t> copy(
        preparedScript->data(),
        preparedScript->data() + preparedScript->size());
    lastPersistedSize = copy.size();
    entries[makeKey(scriptMetadata)] = std::move(copy);
  }

  int loadCount{0};
  int storeCount{0};
  size_t lastPersistedSize{0};

 private:
  static std::string makeKey(
      const facebook::jsi::ScriptSignature &sig) {
    return sig.url + "#" + std::to_string(sig.version);
  }

  std::unordered_map<std::string, std::vector<uint8_t>> entries;
};

} // namespace

TEST(JsiAbiScriptCache, LoadStoreRoundtrip) {
  auto store = std::make_shared<StubPreparedScriptStore>();

  const std::string source = "var roundtripResult = 40 + 2; roundtripResult";
  const std::string url = "w3-roundtrip.js";

  // First run: cache miss → V8 compiles, stores cached data via the adapter.
  {
    v8runtime::V8RuntimeArgs args;
    args.flags.explicitMicrotaskPolicy = true;
    args.flags.enableGCApi = true;
    args.preparedScriptStore = store;
    auto runtime = v8runtime::makeV8Runtime(std::move(args));

    auto prepared = runtime->prepareJavaScript(
        std::make_shared<facebook::jsi::StringBuffer>(source), url);
    auto result = runtime->evaluatePreparedJavaScript(prepared);
    EXPECT_TRUE(result.isNumber());
    EXPECT_EQ(result.getNumber(), 42.0);
  }

  EXPECT_EQ(store->loadCount, 1)
      << "tryGetPreparedScript should be called exactly once on first run";
  EXPECT_EQ(store->storeCount, 1)
      << "persistPreparedScript should be called once after compile-from-source";
  EXPECT_GT(store->lastPersistedSize, 0u)
      << "stored cache should be non-empty";

  // Second run: cache hit → V8 consumes cached data, no new persist call.
  {
    v8runtime::V8RuntimeArgs args;
    args.flags.explicitMicrotaskPolicy = true;
    args.flags.enableGCApi = true;
    args.preparedScriptStore = store;
    auto runtime = v8runtime::makeV8Runtime(std::move(args));

    auto prepared = runtime->prepareJavaScript(
        std::make_shared<facebook::jsi::StringBuffer>(source), url);
    auto result = runtime->evaluatePreparedJavaScript(prepared);
    EXPECT_TRUE(result.isNumber());
    EXPECT_EQ(result.getNumber(), 42.0);
  }

  EXPECT_EQ(store->loadCount, 2)
      << "tryGetPreparedScript should be queried again on second run";
  EXPECT_EQ(store->storeCount, 1)
      << "persistPreparedScript must NOT be called on a cache hit";
}

// W4 lifecycle + post-task test. Confirms:
//   1. A foreground_task_runner supplied via V8RuntimeArgs reaches the runtime.
//   2. Posting a task through the foreground runner round-trips through both
//      the consumer-side V8TaskRunner adapter and the DLL-side CTaskRunner
//      adapter, and the supplied callback fires.
//   3. The deleter chain runs exactly once on runtime destruction (the
//      consumer-side adapter is freed, which drops the shared_ptr to the
//      stub task runner, which destroys the stub).
namespace {

class StubTaskRunner final : public v8runtime::JSITaskRunner {
 public:
  std::atomic<int> postTaskCount{0};

  void postTask(std::unique_ptr<v8runtime::JSITask> task) override {
    ++postTaskCount;
    task->run();
    // unique_ptr destruction fires the C task_data_delete_cb via ~CTask.
  }
};

} // namespace

TEST(JsiAbiTaskRunner, LifecycleAndPostTask) {
  auto stub = std::make_shared<StubTaskRunner>();
  std::weak_ptr<StubTaskRunner> weakStub = stub;

  int taskFiredCount = 0;

  {
    v8runtime::V8RuntimeArgs args;
    args.flags.explicitMicrotaskPolicy = true;
    args.flags.enableGCApi = true;
    args.foreground_task_runner = stub;
    auto runtime = v8runtime::makeV8Runtime(std::move(args));

    // Sanity: runtime evaluates JS — confirms construction succeeded with
    // the task runner wired in.
    auto evalResult = runtime->evaluateJavaScript(
        std::make_shared<facebook::jsi::StringBuffer>("1 + 2"),
        "w4-construct.js");
    EXPECT_TRUE(evalResult.isNumber());
    EXPECT_EQ(evalResult.getNumber(), 3.0);

    jsi_runtime *abiRuntime = ::jsi::abi::getAbiRuntime(*runtime);

    // Post a task: runtime → DLL CTaskRunner → consumer V8TaskRunner →
    // stub::postTask → CTask::run → DLL trampoline → user callback.
    v8_jsi_test_post_foreground_task(
        abiRuntime,
        [](void *data) { ++(*static_cast<int *>(data)); },
        &taskFiredCount);

    EXPECT_EQ(taskFiredCount, 1)
        << "user callback must fire exactly once via the post-task chain";
    EXPECT_EQ(stub->postTaskCount.load(), 1)
        << "stub task runner must observe the postTask call";

    // Drop our local strong ref. Only the runtime should be holding the
    // stub alive at this point — verify that with the weak_ptr.
    stub.reset();
    EXPECT_FALSE(weakStub.expired())
        << "runtime must keep the task runner alive while it lives";
  }

  // Runtime is destroyed; consumer's Delete callback freed the V8TaskRunner
  // adapter, which dropped its shared_ptr<JSITaskRunner>, which destroyed
  // the stub — verify the deleter fired exactly once via weak_ptr expiry.
  EXPECT_TRUE(weakStub.expired())
      << "runtime destruction must release the task runner exactly once";
}

// W5 inspector lifecycle smoke test. Builds a runtime with
// enable_inspector=true, calls v8_open_inspector, evaluates JS, and tears
// down — all without standing up a real debugger client. The point is to
// verify the inspector::Agent is created/started/torn down without crashing,
// not to exercise the protocol. Mark skipped on inspector-disabled builds so
// the test corpus stays portable during transitional rebuilds.
extern "C" JSI_API void JSI_CDECL v8_open_inspector(jsi_runtime *runtime);

TEST(JsiAbiInspector, LifecycleAndOpen) {
#if !defined(_WIN32) || !defined(V8JSI_ENABLE_INSPECTOR)
  GTEST_SKIP() << "Inspector not enabled in this build";
#else
  // Stub task runner — inspector dispatch posts onto the foreground runner.
  // Run-immediately to keep the test synchronous.
  auto stub = std::make_shared<StubTaskRunner>();

  v8runtime::V8RuntimeArgs args;
  args.flags.explicitMicrotaskPolicy = true;
  args.flags.enableGCApi = true;
  args.flags.enableInspector = true;
  args.flags.waitForDebugger = false;  // Don't block waiting on a client.
  // Use a non-default port to avoid collisions with anything else listening.
  args.inspectorPort = 19223;
  args.debuggerRuntimeName = "w5-smoke";
  args.foreground_task_runner = stub;

  auto runtime = v8runtime::makeV8Runtime(std::move(args));

  // Trivial evaluation must succeed with the inspector wired in.
  auto result = runtime->evaluateJavaScript(
      std::make_shared<facebook::jsi::StringBuffer>("40 + 2"),
      "w5-smoke.js");
  EXPECT_TRUE(result.isNumber());
  EXPECT_EQ(result.getNumber(), 42.0);

  // Calling v8_open_inspector again on an already-started agent must be safe.
  v8_open_inspector(::jsi::abi::getAbiRuntime(*runtime));

  // Tear down — Agent::removeContext + Agent::stop run inside the runtime
  // destructor; the test passes if nothing crashes.
#endif
}

// W6 Node-API revival smoke test. Confirms that the napi_*/jsr_* surface
// compiled into v8jsi.dll works end-to-end:
//   1. Build a jsr_config and create a jsr_runtime.
//   2. Obtain the napi_env and open an env scope.
//   3. Run a trivial JS script via napi_run_script and read back a number.
//   4. Tear down env scope and runtime.
// The point is to prove the path is wired, not to exhaustively cover Node-API.
// Full N-API conformance comes back in W7.
TEST(NodeApiSmoke, CreateRuntimeAndRunScript) {
  jsr_config config{};
  ASSERT_EQ(jsr_create_config(&config), napi_ok);
  ASSERT_NE(config, nullptr);
  jsr_config_enable_gc_api(config, true);

  jsr_runtime runtime{};
  ASSERT_EQ(jsr_create_runtime(config, &runtime), napi_ok);
  ASSERT_NE(runtime, nullptr);
  // jsr_create_runtime takes ownership of config-owned state; safe to free now.
  ASSERT_EQ(jsr_delete_config(config), napi_ok);

  napi_env env{};
  ASSERT_EQ(jsr_runtime_get_node_api_env(runtime, &env), napi_ok);
  ASSERT_NE(env, nullptr);

  jsr_napi_env_scope envScope{};
  ASSERT_EQ(jsr_open_napi_env_scope(env, &envScope), napi_ok);

  {
    napi_handle_scope handleScope{};
    ASSERT_EQ(napi_open_handle_scope(env, &handleScope), napi_ok);

    napi_value source{};
    ASSERT_EQ(
        napi_create_string_utf8(env, "40 + 2", NAPI_AUTO_LENGTH, &source),
        napi_ok);

    napi_value result{};
    ASSERT_EQ(jsr_run_script(env, source, "w6-smoke.js", &result), napi_ok);

    double value{};
    ASSERT_EQ(napi_get_value_double(env, result, &value), napi_ok);
    EXPECT_EQ(value, 42.0);

    ASSERT_EQ(napi_close_handle_scope(env, handleScope), napi_ok);
  }

  ASSERT_EQ(jsr_close_napi_env_scope(env, envScope), napi_ok);
  ASSERT_EQ(jsr_delete_runtime(runtime), napi_ok);
}

// W8 dual-API smoke test. The original reason `V8RuntimeEnv : public V8Runtime`
// existed was so a consumer could create a runtime via Node-API (jsr_*) and
// also reach into the same V8 isolate through the JSI APIs. After W8 the
// inheritance is gone, but the contract is preserved through:
//   1. v8_create_runtime + v8_attach_node_api as the underlying primitives.
//   2. jsr_create_runtime as the same convenience entry point, internally
//      calling those two.
//   3. jsr_runtime_get_jsi_runtime as the accessor that lets a napi-created
//      runtime hand back a jsi_runtime * a JsiAbiRuntime can wrap.
// The test sets a global property from the napi side and reads it back from
// the JSI side over the same V8 isolate — the smallest possible exercise of
// the dual-API contract.
TEST(DualApi, NodeApiAndJsiShareIsolate) {
  jsr_config config{};
  ASSERT_EQ(jsr_create_config(&config), napi_ok);
  jsr_config_enable_gc_api(config, true);

  jsr_runtime runtime{};
  ASSERT_EQ(jsr_create_runtime(config, &runtime), napi_ok);
  ASSERT_EQ(jsr_delete_config(config), napi_ok);

  // Node-API side: set globalThis.W8DualValue = 42.
  napi_env env{};
  ASSERT_EQ(jsr_runtime_get_node_api_env(runtime, &env), napi_ok);

  jsr_napi_env_scope envScope{};
  ASSERT_EQ(jsr_open_napi_env_scope(env, &envScope), napi_ok);
  {
    napi_handle_scope handleScope{};
    ASSERT_EQ(napi_open_handle_scope(env, &handleScope), napi_ok);

    napi_value source{};
    ASSERT_EQ(
        napi_create_string_utf8(env,
                                 "globalThis.W8DualValue = 42;",
                                 NAPI_AUTO_LENGTH,
                                 &source),
        napi_ok);
    napi_value result{};
    ASSERT_EQ(jsr_run_script(env, source, "w8-dual-set.js", &result), napi_ok);

    ASSERT_EQ(napi_close_handle_scope(env, handleScope), napi_ok);
  }
  ASSERT_EQ(jsr_close_napi_env_scope(env, envScope), napi_ok);

  // JSI side: pull the underlying jsi_runtime out and wrap it as a
  // JsiAbiRuntime. Read back the property; assert it matches.
  jsi_runtime *abiRuntime{};
  ASSERT_EQ(jsr_runtime_get_jsi_runtime(runtime, &abiRuntime), napi_ok);
  ASSERT_NE(abiRuntime, nullptr);

  // Wrap the ABI runtime as a jsi::Runtime. wrapJsiRuntime adds its own ref,
  // independent of the one held by the jsr_runtime — both sides can release
  // in any order; the runtime is destroyed when the last ref drops.
  auto wrapper = ::jsi::abi::wrapJsiRuntime(abiRuntime);
  facebook::jsi::Runtime &jsiRt = *wrapper;
  facebook::jsi::Value v =
      jsiRt.global().getProperty(jsiRt, "W8DualValue");
  ASSERT_TRUE(v.isNumber());
  EXPECT_EQ(v.getNumber(), 42.0);

  ASSERT_EQ(jsr_delete_runtime(runtime), napi_ok);
}

// W9 query_interface negative-path test. The ABI currently supports no
// optional interfaces, so query_interface always reports not-supported.
// Confirms it returns jsi_error_native and surfaces "Interface not supported"
// via the native-exception-message slot. Exercises the only path through which
// a non-JS error message flows back to the consumer.
TEST(JsiAbiQueryInterface, UnknownIidReturnsError) {
  jsi_runtime *rt = v8_create_runtime(JSI_ABI_VERSION, nullptr, nullptr);
  ASSERT_NE(rt, nullptr);

  // Bogus IID — reserved-bit-pattern that will never collide with any
  // well-known IID a future engine-specific interface might use.
  static constexpr jsi_interface_id kBogusIid = {
      0xDEADBEEFCAFEBABEull, 0x0123456789ABCDEFull};

  const void *vtableOut = nullptr;
  void *instanceOut = nullptr;
  jsi_error_code result =
      rt->vt->query_interface(rt, &kBogusIid, &vtableOut, &instanceOut);

  ASSERT_TRUE(::jsi::abi::is_error(result));
  EXPECT_EQ(::jsi::abi::get_error(result), jsi_error_native);

  // Read back the native exception message — must mention "Interface not
  // supported".
  struct LocalBuf : public jsi_growable_buffer {
    std::string buf{std::string(64, '\0')};
    LocalBuf() {
      static const jsi_growable_buffer_vtable bufVt{&tryGrowTo};
      vtable = const_cast<jsi_growable_buffer_vtable *>(&bufVt);
      data = reinterpret_cast<uint8_t *>(buf.data());
      size = buf.size();
      used = 0;
    }
    static void JSI_CDECL tryGrowTo(jsi_growable_buffer *b, size_t sz) {
      auto *self = static_cast<LocalBuf *>(b);
      if (sz > self->buf.size()) {
        self->buf.resize(sz);
        b->data = reinterpret_cast<uint8_t *>(self->buf.data());
        b->size = self->buf.size();
      }
    }
  } gb;

  rt->vt->get_and_clear_native_exception_message(rt, &gb);
  std::string msg(reinterpret_cast<const char *>(gb.data), gb.used);
  EXPECT_NE(msg.find("Interface not supported"), std::string::npos)
      << "expected 'Interface not supported' in message; got: " << msg;

  rt->vt->release(rt);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
