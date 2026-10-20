// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// JSI tests main file - uses V8JsiRuntime without Node-API

#include <gtest/gtest.h>
#include <jsi/jsi.h>
#include <algorithm>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "jsi/test/testlib.h"
#include "public/ScriptStore.h"
#include "public/V8JsiRuntime.h"
#include "jsi_abi/JsiAbiRuntime.h"
#include "jsi_abi/jsi_abi_helpers.h"
#include "jsi_abi/v8_jsi_abi.h"
#include "v8_node_api.h"

// test-only hook (declared in jsi_abi_v8.cpp). Posts a task to the
// runtime's foreground task runner — used by JsiAbiTaskRunner.LifecycleAndPostTask
// to exercise the post-task trampoline without an inspector session.
extern "C" JSI_ABI_API void JSI_ABI_CDECL v8_jsi_test_post_foreground_task(
    jsi_runtime *runtime,
    void (JSI_ABI_CDECL *task_run_cb)(void *),
    void *task_data);

// v8_jsi_abi_snapshot_create / v8_jsi_abi_snapshot_destroy are declared in
// jsi_abi/v8_jsi_abi.h (included above).

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
        return ::jsi::abi::makeJsiAbiRuntime(&v8_jsi_abi_runtime_create);
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

// Startup-snapshot round-trip. V8's shared read-only heap requires every isolate
// in a process to use the same snapshot, so creation (a SnapshotCreator isolate)
// and consumption (a runtime isolate from a custom blob) must NOT share a
// process. These are therefore two separate tests, each run in its OWN process
// invocation, exchanging the blob via a file. They are DISABLED_ so they never
// run as part of the normal in-process suite (where the shared RO heap would
// make Consume fail); run them explicitly, each in its own process:
//   v8jsi_test.exe --gtest_also_run_disabled_tests --gtest_filter=*DISABLED_SnapshotCreate
//   v8jsi_test.exe --gtest_also_run_disabled_tests --gtest_filter=*DISABLED_SnapshotConsume
// The Consume isolate is then the only isolate in its process, so its custom
// blob defines the process read-only heap — the real (one-isolate-per-process)
// deployment shape used by v8host.
namespace {
const char *kSnapshotTestBlobPath = "e:\\tmp\\v8jsi_test_snapshot.bin";

class SnapshotBlobBuffer final : public facebook::jsi::Buffer {
 public:
  explicit SnapshotBlobBuffer(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  const uint8_t *data() const override { return bytes_.data(); }
  size_t size() const override { return bytes_.size(); }

 private:
  std::vector<uint8_t> bytes_;
};
} // namespace

TEST(SnapshotRoundtrip, DISABLED_SnapshotCreate) {
  const std::string builder = "globalThis.snapshotMarker = 41 + 1;";

  uint8_t *blob = nullptr;
  size_t blobSize = 0;
  jsi_status rc = v8_jsi_abi_snapshot_create(
      builder.data(), builder.size(), "snapshot-builder.js", /*jitless*/ false, &blob, &blobSize);
  ASSERT_EQ(rc, jsi_status_ok) << "v8_jsi_abi_snapshot_create failed";
  ASSERT_NE(blob, nullptr);
  ASSERT_GT(blobSize, 0u);
  fprintf(stderr, "[snap-test] created blob = %zu bytes -> %s\n", blobSize, kSnapshotTestBlobPath);

  FILE *f = fopen(kSnapshotTestBlobPath, "wb");
  ASSERT_NE(f, nullptr) << "cannot open blob file for write";
  ASSERT_EQ(fwrite(blob, 1, blobSize, f), blobSize);
  fclose(f);
  v8_jsi_abi_snapshot_destroy(blob);
}

TEST(SnapshotRoundtrip, DISABLED_SnapshotConsume) {
  FILE *f = fopen(kSnapshotTestBlobPath, "rb");
  ASSERT_NE(f, nullptr) << "run DISABLED_SnapshotCreate first (separate process)";
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  ASSERT_GT(sz, 0);
  std::vector<uint8_t> bytes(static_cast<size_t>(sz));
  ASSERT_EQ(fread(bytes.data(), 1, bytes.size(), f), bytes.size());
  fclose(f);
  fprintf(stderr, "[snap-test] consuming blob = %ld bytes\n", sz);

  v8runtime::V8RuntimeArgs args;
  args.flags.enableInspector = false;
  args.startupSnapshotBlob = std::make_shared<SnapshotBlobBuffer>(std::move(bytes));
  auto runtime = v8runtime::makeV8Runtime(std::move(args));

  // The global must be present from the snapshot — no script evaluated.
  facebook::jsi::Value v = runtime->global().getProperty(*runtime, "snapshotMarker");
  ASSERT_TRUE(v.isNumber()) << "snapshotMarker not restored from snapshot";
  EXPECT_EQ(v.getNumber(), 42.0);
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

// smoke test: end-to-end exercise of the v8_jsi_abi config plumbing.
// Populates a factory-owned config via the public C setters inside the
// configure callback, and confirms a basic eval works. Does not (and cannot)
// verify that V8 actually applied the requested flags — V8 platform-flag state
// is process-global and first-init-only, and these tests share a process.
TEST(JsiAbiConfig, RuntimeFromConfigEvaluates) {
  // The factory owns the jsi_config; we populate it inside the configure
  // callback (pull model). Touch a representative subset of every field group
  // to confirm linkage.
  jsi_configure_runtime_cb configure = [](void * /*user_data*/,
                                          jsi_config cfg) -> jsi_status {
    v8_jsi_abi_config_set_initial_heap_size(cfg, 0);
    v8_jsi_abi_config_set_maximum_heap_size(cfg, 0);
    v8_jsi_abi_config_set_thread_pool_size(cfg, 0);
    v8_jsi_abi_config_set_debugger_runtime_name_utf8(cfg, "config-smoke", JSI_ABI_AUTO_LENGTH);
    v8_jsi_abi_config_set_inspector_enabled(cfg, false);
    v8_jsi_abi_config_set_inspector_port(cfg, 9223);
    v8_jsi_abi_config_set_inspector_break_on_start(cfg, false);
    v8_jsi_abi_config_set_explicit_microtask_policy(cfg, true);
    v8_jsi_abi_config_set_ignore_unhandled_promises(cfg, false);
    v8_jsi_abi_config_set_gc_api_enabled(cfg, true);
    v8_jsi_abi_config_set_multi_thread_enabled(cfg, false);
    v8_jsi_abi_config_set_jit_tracing_enabled(cfg, false);
    v8_jsi_abi_config_set_message_tracing_enabled(cfg, false);
    v8_jsi_abi_config_set_gc_tracing_enabled(cfg, false);
    v8_jsi_abi_config_set_system_instrumentation_enabled(cfg, false);
    return jsi_status_ok;
  };

  std::shared_ptr<facebook::jsi::Runtime> runtime =
      ::jsi::abi::makeJsiAbiRuntime(&v8_jsi_abi_runtime_create, configure);

  facebook::jsi::Value result = runtime->evaluateJavaScript(
      std::make_shared<facebook::jsi::StringBuffer>("1 + 2"), "config-smoke.js");
  EXPECT_TRUE(result.isNumber());
  EXPECT_EQ(result.getNumber(), 3.0);
}

// round-trip test: stub PreparedScriptStore wired through V8RuntimeArgs.
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

// lifecycle + post-task test. Confirms:
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

// inspector lifecycle smoke test. Builds a runtime with
// enable_inspector=true, calls v8_jsi_abi_runtime_open_inspector, evaluates JS, and tears
// down — all without standing up a real debugger client. The point is to
// verify the inspector::Agent is created/started/torn down without crashing,
// not to exercise the protocol. Mark skipped on inspector-disabled builds so
// the test corpus stays portable during transitional rebuilds.
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

  // Calling v8_jsi_abi_runtime_open_inspector again on an already-started agent must be safe.
  EXPECT_EQ(v8_jsi_abi_runtime_open_inspector(::jsi::abi::getAbiRuntime(*runtime)), jsi_status_ok);

  // Tear down — Agent::removeContext + Agent::stop run inside the runtime
  // destructor; the test passes if nothing crashes.
#endif
}

namespace {

napi_status V8_NODE_API_CDECL configureNodeApiGc(
    void*,
    v8_node_api_config config) {
  return v8_node_api_config_set_gc_api_enabled(config, true);
}

}  // namespace

// Node-API revival smoke test. Confirms that the napi_*/v8_node_api_* surface
// compiled into v8jsi.dll works end-to-end:
//   1. Create a v8_node_api_runtime with a pull-config callback.
//   2. Obtain the napi_env and open an env scope.
//   3. Run a trivial JS script via napi_run_script and read back a number.
//   4. Tear down env scope and runtime.
// The point is to prove the path is wired, not to exhaustively cover Node-API.
// Full N-API conformance comes back later.
TEST(NodeApiSmoke, CreateRuntimeAndRunScript) {
  v8_node_api_runtime runtime{};
  ASSERT_EQ(
      v8_node_api_runtime_create(
          V8_NODE_API_ABI_VERSION, configureNodeApiGc, nullptr, &runtime),
      napi_ok);
  ASSERT_NE(runtime, nullptr);

  napi_env env{};
  ASSERT_EQ(v8_node_api_runtime_get_env(runtime, &env), napi_ok);
  ASSERT_NE(env, nullptr);

  v8_node_api_env_scope envScope{};
  ASSERT_EQ(v8_node_api_env_scope_open(env, &envScope), napi_ok);

  {
    napi_handle_scope handleScope{};
    ASSERT_EQ(napi_open_handle_scope(env, &handleScope), napi_ok);

    napi_value source{};
    ASSERT_EQ(
        napi_create_string_utf8(env, "40 + 2", NAPI_AUTO_LENGTH, &source),
        napi_ok);

    napi_value result{};
    ASSERT_EQ(
        v8_node_api_env_run_script_utf8(
            env,
            source,
            "smoke.js",
            V8_NODE_API_AUTO_LENGTH,
            &result),
        napi_ok);

    double value{};
    ASSERT_EQ(napi_get_value_double(env, result, &value), napi_ok);
    EXPECT_EQ(value, 42.0);

    ASSERT_EQ(napi_close_handle_scope(env, handleScope), napi_ok);
  }

  ASSERT_EQ(v8_node_api_env_scope_close(envScope, env), napi_ok);
  ASSERT_EQ(v8_node_api_runtime_destroy(runtime), napi_ok);
}

// dual-API smoke test. The original reason `V8RuntimeEnv : public V8Runtime`
// existed was so a consumer could create a runtime via Node-API and
// also reach into the same V8 isolate through the JSI APIs. Now the
// inheritance is gone, but the contract is preserved through:
//   1. v8_jsi_abi_runtime_create + v8_attach_node_api as the underlying primitives.
//   2. v8_node_api_runtime_create as the same convenience entry point, internally
//      calling those two.
//   3. v8_node_api_runtime_get_jsi_runtime as the accessor that lets a napi-created
//      runtime hand back a jsi_runtime * a JsiAbiRuntime can wrap.
// The test sets a global property from the napi side and reads it back from
// the JSI side over the same V8 isolate — the smallest possible exercise of
// the dual-API contract.
TEST(DualApi, NodeApiAndJsiShareIsolate) {
  v8_node_api_runtime runtime{};
  ASSERT_EQ(
      v8_node_api_runtime_create(
          V8_NODE_API_ABI_VERSION, configureNodeApiGc, nullptr, &runtime),
      napi_ok);

  // Node-API side: set globalThis.DualApiValue = 42.
  napi_env env{};
  ASSERT_EQ(v8_node_api_runtime_get_env(runtime, &env), napi_ok);

  v8_node_api_env_scope envScope{};
  ASSERT_EQ(v8_node_api_env_scope_open(env, &envScope), napi_ok);
  {
    napi_handle_scope handleScope{};
    ASSERT_EQ(napi_open_handle_scope(env, &handleScope), napi_ok);

    napi_value source{};
    ASSERT_EQ(
        napi_create_string_utf8(env,
                                 "globalThis.DualApiValue = 42;",
                                 NAPI_AUTO_LENGTH,
                                 &source),
        napi_ok);
    napi_value result{};
    ASSERT_EQ(
        v8_node_api_env_run_script_utf8(
            env,
            source,
            "dual-set.js",
            V8_NODE_API_AUTO_LENGTH,
            &result),
        napi_ok);

    ASSERT_EQ(napi_close_handle_scope(env, handleScope), napi_ok);
  }
  ASSERT_EQ(v8_node_api_env_scope_close(envScope, env), napi_ok);

  // JSI side: pull the underlying jsi_runtime out and wrap it as a
  // JsiAbiRuntime. Read back the property; assert it matches.
  jsi_runtime *abiRuntime{};
  ASSERT_EQ(v8_node_api_runtime_get_jsi_runtime(runtime, &abiRuntime), napi_ok);
  ASSERT_NE(abiRuntime, nullptr);

  // Wrap the ABI runtime as a jsi::Runtime. wrapJsiRuntime adds its own ref,
  // independent of the one held by the v8_node_api_runtime — both sides can release
  // in any order; the runtime is destroyed when the last ref drops.
  auto wrapper = ::jsi::abi::wrapJsiRuntime(abiRuntime);
  facebook::jsi::Runtime &jsiRt = *wrapper;
  facebook::jsi::Value v =
      jsiRt.global().getProperty(jsiRt, "DualApiValue");
  ASSERT_TRUE(v.isNumber());
  EXPECT_EQ(v.getNumber(), 42.0);

  ASSERT_EQ(v8_node_api_runtime_destroy(runtime), napi_ok);
}

namespace {

struct NodeApiDeleteProbe {
  void* expectedData{};
  int calls{};
  bool argumentsCorrect{true};
};

void V8_NODE_API_CDECL deleteNodeApiProbe(void* delete_data, void* data) {
  auto* probe = static_cast<NodeApiDeleteProbe*>(delete_data);
  ++probe->calls;
  probe->argumentsCorrect =
      probe->argumentsCorrect && data == probe->expectedData;
}

void V8_NODE_API_CDECL ignorePostedTask(
    void*,
    void* task_data,
    v8_node_api_task_run_cb,
    v8_node_api_data_delete_cb delete_task,
    void* delete_data) {
  if (delete_task) {
    EXPECT_NE(task_data, delete_data);
    delete_task(delete_data, task_data);
  }
}

void V8_NODE_API_CDECL emptyScriptCacheLoad(
    void*,
    const char*,
    uint64_t,
    const char*,
    uint64_t,
    const char*,
    const uint8_t** out_buffer,
    size_t* out_buffer_size,
    v8_node_api_data_delete_cb* out_delete_buffer,
    void** out_delete_data) {
  *out_buffer = nullptr;
  *out_buffer_size = 0;
  *out_delete_buffer = nullptr;
  *out_delete_data = nullptr;
}

void V8_NODE_API_CDECL discardScriptCacheStore(
    void*,
    const char*,
    uint64_t,
    const char*,
    uint64_t,
    const char*,
    const uint8_t* buffer,
    size_t,
    v8_node_api_data_delete_cb delete_buffer,
    void* delete_data) {
  if (delete_buffer) {
    delete_buffer(delete_data, const_cast<uint8_t*>(buffer));
  }
}

struct NodeApiConfigureTestState {
  NodeApiDeleteProbe* runnerProbe;
  void* runnerData;
  NodeApiDeleteProbe* cacheProbe;
  void* cacheData;
  napi_status result;
  bool poison;
};

struct NodeApiScriptCacheTestState {
  std::vector<uint8_t> cache;
  NodeApiDeleteProbe loadProbe;
  int storeCalls{};
  bool storeArgumentsCorrect{true};
};

void V8_NODE_API_CDECL deleteLoadedCache(void* delete_data, void* data) {
  auto* state = static_cast<NodeApiScriptCacheTestState*>(delete_data);
  deleteNodeApiProbe(&state->loadProbe, data);
  delete[] static_cast<uint8_t*>(data);
}

void V8_NODE_API_CDECL loadScriptCache(
    void* user_data,
    const char*,
    uint64_t,
    const char*,
    uint64_t,
    const char*,
    const uint8_t** out_buffer,
    size_t* out_buffer_size,
    v8_node_api_data_delete_cb* out_delete_buffer,
    void** out_delete_data) {
  auto* state = static_cast<NodeApiScriptCacheTestState*>(user_data);
  if (state->cache.empty()) {
    *out_buffer = nullptr;
    *out_buffer_size = 0;
    *out_delete_buffer = nullptr;
    *out_delete_data = nullptr;
    return;
  }
  auto* copy = new uint8_t[state->cache.size()];
  std::copy(state->cache.begin(), state->cache.end(), copy);
  state->loadProbe.expectedData = copy;
  *out_buffer = copy;
  *out_buffer_size = state->cache.size();
  *out_delete_buffer = deleteLoadedCache;
  *out_delete_data = state;
}

void V8_NODE_API_CDECL storeScriptCache(
    void* user_data,
    const char*,
    uint64_t,
    const char*,
    uint64_t,
    const char*,
    const uint8_t* buffer,
    size_t buffer_size,
    v8_node_api_data_delete_cb delete_buffer,
    void* delete_data) {
  auto* state = static_cast<NodeApiScriptCacheTestState*>(user_data);
  ++state->storeCalls;
  state->storeArgumentsCorrect =
      state->storeArgumentsCorrect && delete_data != buffer;
  state->cache.assign(buffer, buffer + buffer_size);
  delete_buffer(delete_data, const_cast<uint8_t*>(buffer));
}

napi_status V8_NODE_API_CDECL configureScriptCache(
    void* user_data,
    v8_node_api_config config) {
  return v8_node_api_config_set_script_cache(
      config,
      loadScriptCache,
      storeScriptCache,
      user_data,
      nullptr,
      nullptr);
}

napi_status V8_NODE_API_CDECL configureNodeApiOwnership(
    void* user_data,
    v8_node_api_config config) {
  auto* state = static_cast<NodeApiConfigureTestState*>(user_data);
  if (state->poison) {
    EXPECT_EQ(
        v8_node_api_config_set_inspector_runtime_name_utf8(
            config, nullptr, 1),
        napi_invalid_arg);
  }
  napi_status status = v8_node_api_config_set_task_runner(
      config,
      ignorePostedTask,
      state->runnerData,
      deleteNodeApiProbe,
      state->runnerProbe);
  EXPECT_EQ(status, state->poison ? napi_invalid_arg : napi_ok);
  status = v8_node_api_config_set_script_cache(
      config,
      emptyScriptCacheLoad,
      discardScriptCacheStore,
      state->cacheData,
      deleteNodeApiProbe,
      state->cacheProbe);
  EXPECT_EQ(status, state->poison ? napi_invalid_arg : napi_ok);
  return state->result;
}

}  // namespace

TEST(NodeApiConfig, PullConfigurationAndCleanup) {
  int runnerData{};
  int cacheData{};
  NodeApiDeleteProbe successRunnerProbe{&runnerData};
  NodeApiDeleteProbe successCacheProbe{&cacheData};
  NodeApiConfigureTestState success{
      &successRunnerProbe,
      &runnerData,
      &successCacheProbe,
      &cacheData,
      napi_ok,
      false};
  v8_node_api_runtime runtime{};
  ASSERT_EQ(
      v8_node_api_runtime_create(
          V8_NODE_API_ABI_VERSION,
          configureNodeApiOwnership,
          &success,
          &runtime),
      napi_ok);
  ASSERT_NE(runtime, nullptr);
  EXPECT_EQ(successRunnerProbe.calls, 0);
  EXPECT_EQ(successCacheProbe.calls, 0);
  ASSERT_EQ(v8_node_api_runtime_destroy(runtime), napi_ok);
  EXPECT_EQ(successRunnerProbe.calls, 1);
  EXPECT_EQ(successCacheProbe.calls, 1);
  EXPECT_TRUE(successRunnerProbe.argumentsCorrect);
  EXPECT_TRUE(successCacheProbe.argumentsCorrect);

  NodeApiDeleteProbe failureRunnerProbe{&runnerData};
  NodeApiDeleteProbe failureCacheProbe{&cacheData};
  NodeApiConfigureTestState failure{
      &failureRunnerProbe,
      &runnerData,
      &failureCacheProbe,
      &cacheData,
      napi_generic_failure,
      false};
  runtime = reinterpret_cast<v8_node_api_runtime>(1);
  EXPECT_EQ(
      v8_node_api_runtime_create(
          V8_NODE_API_ABI_VERSION,
          configureNodeApiOwnership,
          &failure,
          &runtime),
      napi_generic_failure);
  EXPECT_EQ(runtime, nullptr);
  EXPECT_EQ(failureRunnerProbe.calls, 1);
  EXPECT_EQ(failureCacheProbe.calls, 1);
  EXPECT_TRUE(failureRunnerProbe.argumentsCorrect);
  EXPECT_TRUE(failureCacheProbe.argumentsCorrect);

  NodeApiDeleteProbe poisonRunnerProbe{&runnerData};
  NodeApiDeleteProbe poisonCacheProbe{&cacheData};
  NodeApiConfigureTestState poison{
      &poisonRunnerProbe,
      &runnerData,
      &poisonCacheProbe,
      &cacheData,
      napi_ok,
      true};
  runtime = reinterpret_cast<v8_node_api_runtime>(1);
  EXPECT_EQ(
      v8_node_api_runtime_create(
          V8_NODE_API_ABI_VERSION,
          configureNodeApiOwnership,
          &poison,
          &runtime),
      napi_invalid_arg);
  EXPECT_EQ(runtime, nullptr);
  EXPECT_EQ(poisonRunnerProbe.calls, 1);
  EXPECT_EQ(poisonCacheProbe.calls, 1);
  EXPECT_TRUE(poisonRunnerProbe.argumentsCorrect);
  EXPECT_TRUE(poisonCacheProbe.argumentsCorrect);
}

TEST(NodeApiConfig, NullConfigureUsesDefaults) {
  v8_node_api_runtime runtime{};
  ASSERT_EQ(
      v8_node_api_runtime_create(
          V8_NODE_API_ABI_VERSION, nullptr, nullptr, &runtime),
      napi_ok);
  ASSERT_NE(runtime, nullptr);
  ASSERT_EQ(v8_node_api_runtime_destroy(runtime), napi_ok);
}

TEST(NodeApiPreparedScript, DeleterArguments) {
  NodeApiScriptCacheTestState cacheState;
  v8_node_api_runtime runtime{};
  ASSERT_EQ(
      v8_node_api_runtime_create(
          V8_NODE_API_ABI_VERSION,
          configureScriptCache,
          &cacheState,
          &runtime),
      napi_ok);
  napi_env env{};
  ASSERT_EQ(v8_node_api_runtime_get_env(runtime, &env), napi_ok);
  v8_node_api_env_scope envScope{};
  ASSERT_EQ(v8_node_api_env_scope_open(env, &envScope), napi_ok);

  uint8_t scriptData[] = "40 + 2";
  NodeApiDeleteProbe probe{scriptData};
  v8_node_api_prepared_script script{};
  ASSERT_EQ(
      v8_node_api_prepared_script_create_utf8(
          env,
          scriptData,
          sizeof(scriptData) - 1,
          deleteNodeApiProbe,
          &probe,
          "deleter-order.js",
          V8_NODE_API_AUTO_LENGTH,
          &script),
      napi_ok);
  EXPECT_EQ(probe.calls, 1);
  EXPECT_TRUE(probe.argumentsCorrect);
  ASSERT_EQ(v8_node_api_prepared_script_destroy(script, env), napi_ok);

  probe.calls = 0;
  script = nullptr;
  ASSERT_EQ(
      v8_node_api_prepared_script_create_utf8(
          env,
          scriptData,
          sizeof(scriptData) - 1,
          deleteNodeApiProbe,
          &probe,
          "deleter-order.js",
          V8_NODE_API_AUTO_LENGTH,
          &script),
      napi_ok);
  EXPECT_EQ(probe.calls, 1);
  EXPECT_TRUE(probe.argumentsCorrect);
  EXPECT_GE(cacheState.storeCalls, 1);
  EXPECT_TRUE(cacheState.storeArgumentsCorrect);
  EXPECT_EQ(cacheState.loadProbe.calls, 1);
  EXPECT_TRUE(cacheState.loadProbe.argumentsCorrect);
  ASSERT_EQ(v8_node_api_prepared_script_destroy(script, env), napi_ok);
  ASSERT_EQ(v8_node_api_env_scope_close(envScope, env), napi_ok);
  ASSERT_EQ(v8_node_api_runtime_destroy(runtime), napi_ok);
}

TEST(NodeApiPreparedScript, PendingExceptionReleasesInput) {
  v8_node_api_runtime runtime{};
  ASSERT_EQ(
      v8_node_api_runtime_create(
          V8_NODE_API_ABI_VERSION, nullptr, nullptr, &runtime),
      napi_ok);
  napi_env env{};
  ASSERT_EQ(v8_node_api_runtime_get_env(runtime, &env), napi_ok);
  v8_node_api_env_scope envScope{};
  ASSERT_EQ(v8_node_api_env_scope_open(env, &envScope), napi_ok);
  ASSERT_EQ(napi_throw_error(env, nullptr, "pending"), napi_ok);

  uint8_t scriptData[] = "40 + 2";
  NodeApiDeleteProbe probe{scriptData};
  v8_node_api_prepared_script script =
      reinterpret_cast<v8_node_api_prepared_script>(1);
  EXPECT_EQ(
      v8_node_api_prepared_script_create_utf8(
          env,
          scriptData,
          sizeof(scriptData) - 1,
          deleteNodeApiProbe,
          &probe,
          "pending-exception.js",
          V8_NODE_API_AUTO_LENGTH,
          &script),
      napi_pending_exception);
  EXPECT_EQ(script, nullptr);
  EXPECT_EQ(probe.calls, 1);
  EXPECT_TRUE(probe.argumentsCorrect);

  napi_value exception{};
  ASSERT_EQ(napi_get_and_clear_last_exception(env, &exception), napi_ok);
  ASSERT_EQ(v8_node_api_env_scope_close(envScope, env), napi_ok);
  ASSERT_EQ(v8_node_api_runtime_destroy(runtime), napi_ok);
}

namespace {

// A growable sink for the ABI's string getters.
struct AbiTestBuffer : public jsi_growable_buffer {
  std::string buf{std::string(64, '\0')};
  AbiTestBuffer() {
    static const jsi_growable_buffer_api bufApi{&tryGrowTo};
    api = &bufApi;
    data = reinterpret_cast<uint8_t *>(buf.data());
    size = buf.size();
    used = 0;
  }
  std::string str() const {
    return std::string(reinterpret_cast<const char *>(data), used);
  }
  static void JSI_ABI_CDECL tryGrowTo(jsi_growable_buffer *b, size_t sz) {
    auto *self = static_cast<AbiTestBuffer *>(b);
    if (sz > self->buf.size()) {
      self->buf.resize(sz);
      b->data = reinterpret_cast<uint8_t *>(self->buf.data());
      b->size = self->buf.size();
    }
  }
};

// Script source handed to the ABI, which releases it.
struct AbiSourceBuffer : public jsi_buffer {
  std::string text;
  explicit AbiSourceBuffer(std::string s) : text(std::move(s)) {
    static const jsi_buffer_api bufApi{&releaseBuffer};
    api = &bufApi;
    data = reinterpret_cast<const uint8_t *>(text.data());
    size = text.size();
  }
  static void JSI_ABI_CDECL releaseBuffer(jsi_buffer *b) {
    delete static_cast<AbiSourceBuffer *>(b);
  }
};

} // namespace

// query_interface negative-path test. The ABI currently supports no
// optional interfaces, so query_interface always reports not-supported.
// Confirms it returns jsi_status_native_error, zeroes both outputs, and
// surfaces "Interface not supported" via the native-exception-message slot.
// Exercises the only path through which a non-JS error message flows back to
// the consumer.
TEST(JsiAbiQueryInterface, UnknownIidReturnsError) {
  jsi_runtime *rt = nullptr;
  ASSERT_EQ(v8_jsi_abi_runtime_create(JSI_ABI_VERSION, nullptr, nullptr, &rt), jsi_status_ok);
  ASSERT_NE(rt, nullptr);

  // Bogus IID — reserved-bit-pattern that will never collide with any
  // well-known IID a future engine-specific interface might use.
  static constexpr jsi_interface_id kBogusIid = {
      0xDEADBEEFCAFEBABEull, 0x0123456789ABCDEFull};

  int sentinel = 0;
  const void *apiOut = &sentinel;
  void *instanceOut = &sentinel;
  jsi_status result =
      rt->api->query_interface(rt, &kBogusIid, &apiOut, &instanceOut);

  EXPECT_EQ(result, jsi_status_native_error);
  EXPECT_EQ(apiOut, nullptr);
  EXPECT_EQ(instanceOut, nullptr);

  // Read back the native exception message — must mention "Interface not
  // supported".
  AbiTestBuffer gb;
  rt->api->get_and_clear_native_exception_message(rt, &gb);
  std::string msg = gb.str();
  EXPECT_NE(msg.find("Interface not supported"), std::string::npos)
      << "expected 'Interface not supported' in message; got: " << msg;

  rt->api->release(rt);
}

// Status-plus-out contract: JSI_ABI_AUTO_LENGTH inputs, a NULL output is
// invalid_arg, and a failing call reports the JS band with its output zeroed.
TEST(JsiAbiStatus, OutputsAndAutoLength) {
  jsi_runtime *rt = nullptr;
  ASSERT_EQ(v8_jsi_abi_runtime_create(JSI_ABI_VERSION, nullptr, nullptr, &rt), jsi_status_ok);
  ASSERT_NE(rt, nullptr);
  const jsi_runtime_api *api = rt->api;
  void *scope = nullptr;
  ASSERT_EQ(api->push_scope(rt, &scope), jsi_status_ok);

  jsi_string str{};
  ASSERT_EQ(
      api->create_string_utf8(rt, "hello", JSI_ABI_AUTO_LENGTH, &str),
      jsi_status_ok);
  AbiTestBuffer gb;
  api->get_string_utf8(rt, str, &gb);
  EXPECT_EQ(gb.str(), "hello");
  ::jsi::abi::release_pointer(str.pointer);

  jsi_string str16{};
  ASSERT_EQ(
      api->create_string_utf16(rt, u"h\u00e9", JSI_ABI_AUTO_LENGTH, &str16),
      jsi_status_ok);
  AbiTestBuffer gb16;
  api->get_string_utf8(rt, str16, &gb16);
  EXPECT_EQ(gb16.str(), "h\xc3\xa9");
  ::jsi::abi::release_pointer(str16.pointer);

  EXPECT_EQ(
      api->create_string_utf8(rt, "x", 1, nullptr), jsi_status_invalid_arg);

  jsi_value out{};
  out.kind = jsi_valuekind_number;
  out.data.number = 1;
  jsi_status st = api->evaluate_javascript_source(
      rt,
      new AbiSourceBuffer("throw new Error('boom')"),
      "status.js",
      JSI_ABI_AUTO_LENGTH,
      &out);
  EXPECT_EQ(st, jsi_status_js_error);
  EXPECT_EQ(st & 0xF000, 0x4000);
  EXPECT_EQ(out.kind, jsi_valuekind_undefined);
  jsi_value err = api->get_and_clear_js_error_value(rt);
  EXPECT_EQ(err.kind, jsi_valuekind_object);
  ::jsi::abi::release_value(err);

  EXPECT_EQ(api->pop_scope(rt, scope), jsi_status_ok);
  api->release(rt);
}

// The cache callbacks get a NUL-terminated copy of a length-delimited URL.
TEST(JsiAbiScriptCache, CallbacksGetTerminatedUrl) {
  struct Seen {
    std::vector<std::string> urls;
  } seen;
  jsi_configure_runtime_cb configure = [](void *user_data,
                                          jsi_config cfg) -> jsi_status {
    auto load = [](void *data, const char *source_url, uint64_t, const char *,
                   uint64_t, const char *, const uint8_t **, size_t *,
                   jsi_data_delete_cb *, void **) {
      static_cast<Seen *>(data)->urls.emplace_back(source_url);
    };
    auto store = [](void *data, const char *source_url, uint64_t, const char *,
                    uint64_t, const char *, const uint8_t *bytes, size_t,
                    jsi_data_delete_cb delete_cb, void *delete_data) {
      static_cast<Seen *>(data)->urls.emplace_back(source_url);
      if (delete_cb)
        delete_cb(delete_data, const_cast<uint8_t *>(bytes));
    };
    return v8_jsi_abi_config_set_script_cache(
        cfg, load, store, user_data, nullptr, nullptr);
  };
  jsi_runtime *rt = nullptr;
  ASSERT_EQ(v8_jsi_abi_runtime_create(JSI_ABI_VERSION, configure, &seen, &rt), jsi_status_ok);
  ASSERT_NE(rt, nullptr);

  const char url[] = {'a', '.', 'j', 's', 'X', 'Y'};
  jsi_prepared_javascript *prepared = nullptr;
  ASSERT_EQ(
      rt->api->prepare_javascript(rt, new AbiSourceBuffer("1 + 1"), url, 4, &prepared),
      jsi_status_ok);
  ASSERT_NE(prepared, nullptr);
  prepared->api->release(prepared);

  ASSERT_EQ(seen.urls.size(), 2u);
  EXPECT_EQ(seen.urls[0], "a.js");
  EXPECT_EQ(seen.urls[1], "a.js");
  rt->api->release(rt);
}

// Runtime creation reports failures through jsi_status and leaves the output
// NULL.
TEST(JsiAbiCreate, StatusAndOutput) {
  EXPECT_EQ(v8_jsi_abi_runtime_create(JSI_ABI_VERSION, nullptr, nullptr, nullptr), jsi_status_invalid_arg);

  jsi_runtime *rt = reinterpret_cast<jsi_runtime *>(uintptr_t{1});
  EXPECT_EQ(
      v8_jsi_abi_runtime_create(JSI_ABI_VERSION + 1, nullptr, nullptr, &rt), jsi_status_version_mismatch);
  EXPECT_EQ(rt, nullptr);

  // Versions before 2 are rejected.
  EXPECT_EQ(JSI_ABI_VERSION, 2u);
  for (uint32_t old_version : {0u, 1u}) {
    rt = reinterpret_cast<jsi_runtime *>(uintptr_t{1});
    EXPECT_EQ(v8_jsi_abi_runtime_create(old_version, nullptr, nullptr, &rt), jsi_status_version_mismatch);
    EXPECT_EQ(rt, nullptr);
  }

  // A configure callback failure is returned as is.
  rt = reinterpret_cast<jsi_runtime *>(uintptr_t{1});
  jsi_configure_runtime_cb failing = [](void *, jsi_config) -> jsi_status { return jsi_status_native_error; };
  EXPECT_EQ(v8_jsi_abi_runtime_create(JSI_ABI_VERSION, failing, nullptr, &rt), jsi_status_native_error);
  EXPECT_EQ(rt, nullptr);
}

namespace {
struct DeleterProbe {
  int calls{0};
  void *firstArg{nullptr};
  void *secondArg{nullptr};
  jsi_status setterStatus{jsi_status_ok};
  jsi_status laterSetterStatus{jsi_status_ok};
};

void JSI_ABI_CDECL probeDelete(void *delete_data, void *data) {
  auto *probe = static_cast<DeleterProbe *>(delete_data);
  ++probe->calls;
  probe->firstArg = delete_data;
  probe->secondArg = data;
}

uint8_t kFakeSnapshot[4] = {1, 2, 3, 4};
} // namespace

// jsi_data_delete_cb receives (delete_data, data) in that order. The startup
// snapshot is released with the config when configure fails, before any
// isolate reads it.
TEST(JsiAbiConfig, DeleterArgumentOrder) {
  DeleterProbe probe;
  jsi_configure_runtime_cb configure = [](void *user_data, jsi_config cfg) -> jsi_status {
    auto *p = static_cast<DeleterProbe *>(user_data);
    p->setterStatus = v8_jsi_abi_config_set_startup_snapshot(
        cfg, kFakeSnapshot, sizeof(kFakeSnapshot), &probeDelete, p);
    return jsi_status_native_error;
  };
  jsi_runtime *rt = nullptr;
  EXPECT_EQ(v8_jsi_abi_runtime_create(JSI_ABI_VERSION, configure, &probe, &rt), jsi_status_native_error);
  EXPECT_EQ(rt, nullptr);
  EXPECT_EQ(probe.setterStatus, jsi_status_ok);
  EXPECT_EQ(probe.calls, 1);
  EXPECT_EQ(probe.firstArg, &probe);
  EXPECT_EQ(probe.secondArg, static_cast<void *>(kFakeSnapshot));
}

// A failing setter poisons the config: later setters report the same status,
// release the data they were given, and runtime creation fails with it.
TEST(JsiAbiConfig, FailingSetterPoisonsConfig) {
  DeleterProbe probe;
  jsi_configure_runtime_cb configure = [](void *user_data, jsi_config cfg) -> jsi_status {
    auto *p = static_cast<DeleterProbe *>(user_data);
    p->setterStatus = v8_jsi_abi_config_set_debugger_runtime_name_utf8(cfg, nullptr, 5);
    p->laterSetterStatus = v8_jsi_abi_config_set_startup_snapshot(
        cfg, kFakeSnapshot, sizeof(kFakeSnapshot), &probeDelete, p);
    return jsi_status_ok;
  };
  jsi_runtime *rt = reinterpret_cast<jsi_runtime *>(uintptr_t{1});
  EXPECT_EQ(v8_jsi_abi_runtime_create(JSI_ABI_VERSION, configure, &probe, &rt), jsi_status_invalid_arg);
  EXPECT_EQ(rt, nullptr);
  EXPECT_EQ(probe.setterStatus, jsi_status_invalid_arg);
  EXPECT_EQ(probe.laterSetterStatus, jsi_status_invalid_arg);
  EXPECT_EQ(probe.calls, 1);
  EXPECT_EQ(probe.firstArg, &probe);
  EXPECT_EQ(probe.secondArg, static_cast<void *>(kFakeSnapshot));
}

TEST(JsiAbiProcess, SetV8FlagsRejectsNullArgs) {
  EXPECT_EQ(v8_jsi_abi_set_v8_flags(nullptr, nullptr, false), jsi_status_invalid_arg);
  size_t argc = 1;
  EXPECT_EQ(v8_jsi_abi_set_v8_flags(&argc, nullptr, false), jsi_status_invalid_arg);
  argc = 0;
  EXPECT_EQ(v8_jsi_abi_set_v8_flags(&argc, nullptr, false), jsi_status_ok);
}

TEST(JsiAbiSnapshot, CheckAndCreateArgs) {
  EXPECT_EQ(v8_jsi_abi_snapshot_check(kFakeSnapshot, 1), v8_jsi_abi_snapshot_compat_truncated);
  EXPECT_STRNE(v8_jsi_abi_snapshot_compat_to_string(v8_jsi_abi_snapshot_compat_truncated), "");

  uint8_t *blob = reinterpret_cast<uint8_t *>(uintptr_t{1});
  size_t blobSize = 1;
  EXPECT_EQ(v8_jsi_abi_snapshot_create(nullptr, 0, "x.js", false, &blob, &blobSize), jsi_status_invalid_arg);
  EXPECT_EQ(blob, nullptr);
  EXPECT_EQ(blobSize, 0u);
  v8_jsi_abi_snapshot_destroy(nullptr);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
