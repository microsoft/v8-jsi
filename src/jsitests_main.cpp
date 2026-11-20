// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// JSI tests main file - uses V8JsiRuntime without Node-API

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif
#include <gtest/gtest.h>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <jsi/jsi.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
#include "jsi/test/testlib.h"
#include "public/ScriptStore.h"
#include "public/V8JsiRuntime.h"
#include "jsi_abi/JsiAbiRuntime.h"
#include "jsi_abi/jsi_abi_helpers.h"
#include "jsi_abi/v8_jsi_abi.h"
#define NAPI_EXPERIMENTAL
#include "v8_node_api.h"
#include "jsi_abi/v8_node_api_attach.h"
#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
#include "inspector/inspector_tcp.h"
extern "C" JSI_ABI_API bool JSI_ABI_CDECL v8_jsi_test_inspector_transport_stats(
    int port, inspector::TransportStats* stats);
extern "C" JSI_ABI_API void JSI_ABI_CDECL v8_jsi_test_inspector_send_dispatch(
    inspector::SendDispatchObservations* observations);
#endif

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
// deployment shape used by v8sbhost.
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
    if (loadCorruptCache)
      return std::make_shared<StubPreparedScript>(std::vector<uint8_t>(16, 0));
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

  bool loadCorruptCache{false};
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

// Hosts such as React Native load their bundles with evaluateJavaScript, so it
// must use the script cache as well.
TEST(JsiAbiScriptCache, EvaluateUsesCache) {
  auto store = std::make_shared<StubPreparedScriptStore>();
  const std::string source = "var evaluateResult = 40 + 2; evaluateResult";
  const std::string url = "evaluate-cache.js";
  auto makeRuntime = [&store]() {
    v8runtime::V8RuntimeArgs args;
    args.flags.explicitMicrotaskPolicy = true;
    args.flags.enableGCApi = true;
    args.preparedScriptStore = store;
    return v8runtime::makeV8Runtime(std::move(args));
  };

  // The first runtime misses the cache and stores; the second one hits.
  for (int run = 0; run < 2; ++run) {
    auto runtime = makeRuntime();
    auto result = runtime->evaluateJavaScript(
        std::make_shared<facebook::jsi::StringBuffer>(source), url);
    EXPECT_EQ(result.getNumber(), 42.0);
  }
  EXPECT_EQ(store->loadCount, 2);
  EXPECT_EQ(store->storeCount, 1);

  // Neither misses nor rejected caches may persist after a failed evaluation.
  for (bool rejected : {false, true}) {
    SCOPED_TRACE(rejected ? "rejected cache" : "cache miss");
    store->loadCorruptCache = rejected;
    auto runtime = makeRuntime();
    EXPECT_THROW(
        runtime->evaluateJavaScript(
            std::make_shared<facebook::jsi::StringBuffer>("1 +"), "syntax-error.js"),
        facebook::jsi::JSError);
    EXPECT_THROW(
        runtime->evaluateJavaScript(
            std::make_shared<facebook::jsi::StringBuffer>("throw new Error('boom')"),
            "throws.js"),
        facebook::jsi::JSError);
    EXPECT_EQ(store->storeCount, 1) << "failed scripts must not be cached";
  }
}

TEST(JsiAbiScriptCache, RejectedCacheIsReplacedAndReused) {
  // Both entry points share compilation and must replace a rejected cache.
  for (bool prepare : {false, true}) {
    SCOPED_TRACE(prepare ? "prepare" : "evaluate");
    auto store = std::make_shared<StubPreparedScriptStore>();
    store->loadCorruptCache = true;
    for (int run = 0; run < 2; ++run) {
      v8runtime::V8RuntimeArgs args;
      args.flags.enableGCApi = true;
      args.preparedScriptStore = store;
      auto runtime = v8runtime::makeV8Runtime(std::move(args));
      auto source = std::make_shared<facebook::jsi::StringBuffer>(
          "function answer() { return 42; } answer();");
      auto result = prepare
          ? runtime->evaluatePreparedJavaScript(
                runtime->prepareJavaScript(source, "rejected-cache.js"))
          : runtime->evaluateJavaScript(source, "rejected-cache.js");
      EXPECT_EQ(result.getNumber(), 42.0);
      EXPECT_EQ(store->storeCount, 1);
      EXPECT_GT(store->lastPersistedSize, 16u);
      store->loadCorruptCache = false;
    }
    EXPECT_EQ(store->loadCount, 2);
  }
}

// Evaluate stores the code cache after the run, so it also holds the
// functions that the run compiled; prepare can only store what it compiled.
TEST(JsiAbiScriptCache, EvaluateCachesFunctionsCompiledByTheRun) {
  const std::string source =
      "function f() { return [1, 2, 3].map((x) => x * 2).join(); } f();";
  auto makeRuntime = [](std::shared_ptr<StubPreparedScriptStore> store) {
    v8runtime::V8RuntimeArgs args;
    args.flags.explicitMicrotaskPolicy = true;
    args.flags.enableGCApi = true;
    args.preparedScriptStore = std::move(store);
    return v8runtime::makeV8Runtime(std::move(args));
  };

  // Misses and rejected caches must both include functions compiled by the run.
  for (bool rejected : {false, true}) {
    SCOPED_TRACE(rejected ? "rejected cache" : "cache miss");
    auto evaluated = std::make_shared<StubPreparedScriptStore>();
    evaluated->loadCorruptCache = rejected;
    {
      auto runtime = makeRuntime(evaluated);
      runtime->evaluateJavaScript(
          std::make_shared<facebook::jsi::StringBuffer>(source), "cache-size.js");
    }
    auto prepared = std::make_shared<StubPreparedScriptStore>();
    prepared->loadCorruptCache = rejected;
    {
      auto runtime = makeRuntime(prepared);
      runtime->prepareJavaScript(
          std::make_shared<facebook::jsi::StringBuffer>(source), "cache-size.js");
    }

    ASSERT_EQ(evaluated->storeCount, 1);
    ASSERT_EQ(prepared->storeCount, 1);
    EXPECT_GT(evaluated->lastPersistedSize, prepared->lastPersistedSize);
  }
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

namespace {
#ifdef _WIN32
int freeInspectorPort() {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data)) std::abort();
  SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{}; address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int size = sizeof(address);
  if (bind(socket, reinterpret_cast<sockaddr *>(&address), size) ||
      getsockname(socket, reinterpret_cast<sockaddr *>(&address), &size)) std::abort();
  int port = ntohs(address.sin_port);
  // Another process may take this port before the inspector binds (low risk).
  closesocket(socket); WSACleanup();
  return port;
}
#endif
} // namespace

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
  args.inspectorPort = freeInspectorPort();
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
// Shared host queue for task-lifetime and inspector tests; every entry owns one
// DLL delete callback, whether the host runs it or drops it at shutdown.
struct DeferredHostQueue {
  struct Entry {
    void *data; v8_jsi_abi_task_run_cb run; jsi_data_delete_cb dispose;
    void *deleteData; DeferredHostQueue *queue;
    ~Entry() { dispose(deleteData, data); ++queue->deleted; }
  };
  std::mutex mutex;
  std::condition_variable posted;
  std::deque<std::unique_ptr<Entry>> tasks;
  std::atomic<int> deleted{0}, runnerDeleted{0};
  static void Post(void *data, void *task, v8_jsi_abi_task_run_cb run,
                   jsi_data_delete_cb dispose, void *deleteData) {
    auto &q = *static_cast<DeferredHostQueue *>(data);
    std::lock_guard<std::mutex> lock(q.mutex);
    q.tasks.emplace_back(new Entry{task, run, dispose, deleteData, &q});
    q.posted.notify_all();
  }
  bool wait() {
    std::unique_lock<std::mutex> lock(mutex);
    return posted.wait_for(lock, std::chrono::seconds(5), [&] { return !tasks.empty(); });
  }
  std::unique_ptr<Entry> take() {
    std::lock_guard<std::mutex> lock(mutex);
    if (tasks.empty()) return {};
    auto task = std::move(tasks.front()); tasks.pop_front(); return task;
  }
  void drain(bool run) {
    while (auto task = take()) { if (run) task->run(task->data); }
  }
  ~DeferredHostQueue() { drain(false); }
  auto runtime(int inspectorPort = 0, const char *name = "") {
    struct Config { DeferredHostQueue *queue; int port; const char *name; } config{this, inspectorPort, name};
    return ::jsi::abi::makeJsiAbiRuntime(&v8_jsi_abi_runtime_create,
        [](void *data, jsi_config cfg) -> jsi_status {
          auto *pair = static_cast<Config *>(data);
          v8_jsi_abi_config_set_debugger_runtime_name_utf8(cfg, pair->name, JSI_ABI_AUTO_LENGTH);
          if (pair->port) {
            v8_jsi_abi_config_set_inspector_enabled(cfg, true);
            v8_jsi_abi_config_set_inspector_port(cfg, static_cast<uint16_t>(pair->port));
          }
          return v8_jsi_abi_config_set_task_runner(cfg, Post, pair->queue,
              [](void *, void *q) { ++static_cast<DeferredHostQueue *>(q)->runnerDeleted; }, nullptr);
        }, &config);
  }
};
void postMarker(Runtime &runtime, int &runs) {
  v8_jsi_test_post_foreground_task(::jsi::abi::getAbiRuntime(runtime),
      [](void *p) { ++*static_cast<int *>(p); }, &runs);
}
void expectSurvivor(Runtime &runtime, DeferredHostQueue &queue) {
  int runs = 0;
  postMarker(runtime, runs); queue.drain(true);
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(runtime.evaluateJavaScript(std::make_shared<StringBuffer>("40 + 2"), "survivor.js").getNumber(), 42);
}
template <class Future>
void waitForFuture(Future&& future, int seconds, const char* message = "Inspector operation timed out") {
  if (future.wait_for(std::chrono::seconds(seconds)) != std::future_status::ready) {
    fprintf(stderr, "%s\n", message);
    std::abort();
  }
}
#ifdef _WIN32
// Minimal bounded loopback client: discover a target, upgrade, then disconnect.
// Connect/disconnect themselves enqueue real inspector backend dispatch tasks.
std::string inspectorRequest(int port, const std::string &request, SOCKET *keepOpen = nullptr,
                             size_t splitAt = 0) {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data)) return {};
  SOCKET socket = INVALID_SOCKET;
  sockaddr_in address{}; address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<u_short>(port)); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  std::string response;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  int connected = SOCKET_ERROR;
  // Retry only readiness (before sending HTTP), never an accepted request.
  do {
    socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nonblocking = 1;
    ioctlsocket(socket, FIONBIO, &nonblocking);
    connected = connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    if (connected && WSAGetLastError() == WSAEWOULDBLOCK) {
      fd_set writable, errors;
      FD_ZERO(&writable); FD_SET(socket, &writable); errors = writable;
      timeval timeout{0, 50000};
      if (select(0, nullptr, &writable, &errors, &timeout) > 0) {
        int error = 0, size = sizeof(error);
        if (!getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size) && !error)
          connected = 0;
      }
    }
    if (!connected) break;
    closesocket(socket); socket = INVALID_SOCKET;
  } while (std::chrono::steady_clock::now() < deadline);
  u_long blocking = 0;
  ioctlsocket(socket, FIONBIO, &blocking);
  DWORD timeout = 5000;
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout));
  if (connected == 0) {
    if (splitAt) {
      EXPECT_EQ(send(socket, request.data(), static_cast<int>(splitAt), 0), static_cast<int>(splitAt));
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(send(socket, request.data() + splitAt, static_cast<int>(request.size() - splitAt), 0),
        static_cast<int>(request.size() - splitAt));
    char buffer[8192];
    while (response.size() < sizeof(buffer)) {
      int size = recv(socket, buffer, sizeof(buffer), 0);
      if (size <= 0) break;
      response.append(buffer, size);
      auto headers = response.find("\r\n\r\n"), length = response.find("Content-Length: ");
      if (headers != std::string::npos &&
          (response.find("101 Switching") != std::string::npos ||
           (response.find("HTTP/1.0 400 Bad Request") == 0 &&
            response.find("\r\n", headers + 4) != std::string::npos) ||
           (length != std::string::npos && response.size() >= headers + 4 +
            std::strtoul(response.c_str() + length + 16, nullptr, 10)))) break;
    }
  }
  if (keepOpen && response.find("101 Switching") != std::string::npos) {
    *keepOpen = socket; // The connected test owns the socket and WSAStartup ref.
  } else {
    closesocket(socket); WSACleanup();
  }
  return response;
}
struct InspectorClient {
  SOCKET socket{INVALID_SOCKET};
  ~InspectorClient() { if (socket != INVALID_SOCKET) { closesocket(socket); WSACleanup(); } }
};
std::string attachInspector(int port, const std::string &id, InspectorClient *client) {
  return inspectorRequest(port, "GET /" + id +
      " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
      client ? &client->socket : nullptr);
}
std::string connectInspector(int port, InspectorClient *client = nullptr) {
  auto listing = inspectorRequest(port, "GET /json/list HTTP/1.1\r\nHost: localhost\r\n\r\n");
  auto url = listing.find("ws://");
  if (url == std::string::npos) return listing;
  auto path = listing.find('/', url + 5), end = listing.find('"', path);
  if (path == std::string::npos || end == std::string::npos) return listing;
  return attachInspector(port, listing.substr(path + 1, end - path - 1), client);
}
std::string inspectorFrame(const std::string &message) {
  if (message.size() > 125) std::abort();
  std::string frame{static_cast<char>(0x81), static_cast<char>(0x80 | message.size())};
  frame.append(4, '\0'); // A fixed zero mask is sufficient for this loopback client.
  frame += message;
  return frame;
}
bool sendInspectorMessage(SOCKET socket, const std::string &message) {
  auto frame = inspectorFrame(message);
  return send(socket, frame.data(), static_cast<int>(frame.size()), 0) == static_cast<int>(frame.size());
}
std::string readInspectorMessage(SOCKET socket) {
  auto read = [socket](char *data, size_t size) {
    while (size) {
      int n = recv(socket, data, static_cast<int>(size), 0);
      if (n <= 0) return false;
      data += n; size -= n;
    }
    return true;
  };
  unsigned char header[2];
  if (!read(reinterpret_cast<char *>(header), 2) || header[0] != 0x81 || (header[1] & 0x80)) return {};
  uint64_t size = header[1];
  if (size >= 126) {
    unsigned char length[8];
    int bytes = size == 126 ? 2 : 8;
    if (!read(reinterpret_cast<char *>(length), bytes)) return {};
    size = 0;
    for (int i = 0; i < bytes; ++i) size = (size << 8) | length[i];
  }
  if (size > 4 * 1024 * 1024 + 65536) return {}; // Allow a 4 MiB value plus CDP metadata.
  std::string message(static_cast<size_t>(size), '\0');
  return read(message.data(), message.size()) ? message : std::string{};
}
void destroyInspectorRuntime(std::unique_ptr<Runtime> &runtime) {
  auto teardown = std::async(std::launch::async, [&] { runtime.reset(); });
  waitForFuture(teardown, 10, "Inspector runtime teardown timed out");
  teardown.get();
}
void pumpInspector(Runtime &runtime, DeferredHostQueue &queue) {
  // Host callbacks run inside a runtime entry, with its isolate/context scopes.
  auto drain = Function::createFromHostFunction(runtime, PropNameID::forAscii(runtime, "drain"), 0,
      [&](Runtime &, const Value &, const Value *, size_t) {
        queue.drain(true);
        return Value::undefined();
      });
  drain.call(runtime);
}
void checkInspectorEvaluation(Runtime &runtime, DeferredHostQueue &queue, int port) {
  InspectorClient client;
  auto response = connectInspector(port, &client);
  ASSERT_NE(response.find("101 Switching"), std::string::npos) << response;
  ASSERT_TRUE(queue.wait());
  pumpInspector(runtime, queue); // Establish CONNECT before evaluating or tearing down.
  ASSERT_TRUE(sendInspectorMessage(client.socket,
      R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"6 * 7","returnByValue":true}})"));
  ASSERT_TRUE(queue.wait());
  pumpInspector(runtime, queue);
  auto result = readInspectorMessage(client.socket);
  EXPECT_NE(result.find(R"("value":42)"), std::string::npos) << result;
}

#endif
} // namespace

// Both host shutdown choices must preserve the survivor and delete once.
// The delete-only branch is a regression guard that passes on BASE.
TEST(JsiAbiTaskRunner, QueuedTasksAfterDestroyAreSkippedOrDeleted) {
  for (bool run : {false, true}) { // Same queue ownership, two host shutdown choices.
    SCOPED_TRACE(run ? "run after destroy" : "delete after destroy");
    DeferredHostQueue queue;
    auto destroyed = queue.runtime(), survivor = queue.runtime();
    int runs = 0;
    postMarker(*destroyed, runs);
    destroyed.reset();
    EXPECT_EQ(queue.runnerDeleted, 1);
    queue.drain(run);
    EXPECT_EQ(runs, 0);
    EXPECT_EQ(queue.deleted, 1);
    expectSurvivor(*survivor, queue);
    survivor.reset();
    EXPECT_EQ(queue.runnerDeleted, 2);
    EXPECT_EQ(queue.deleted, 2);
  }
}

TEST(JsiAbiTaskRunner, DestroyWaitsForInFlightTask) {
  DeferredHostQueue queue;
  auto destroyed = queue.runtime(), survivor = queue.runtime();
  struct InFlight {
    std::promise<void> started;
    std::atomic<bool> finished{false};
  } flight;
  v8_jsi_test_post_foreground_task(::jsi::abi::getAbiRuntime(*destroyed), [](void *p) {
    auto &f = *static_cast<InFlight *>(p);
    f.started.set_value();
    // Bias BASE toward returning from destroy before the callback completes.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    f.finished = true;
  }, &flight);
  auto task = queue.take();
  ASSERT_NE(task, nullptr);
  auto running = std::async(std::launch::async, [&] { task->run(task->data); });
  ASSERT_EQ(flight.started.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
  auto teardown = std::async(std::launch::async, [&] {
    destroyed.reset();
    EXPECT_TRUE(flight.finished.load()) << "destroy must wait for the in-flight callback";
  });
  waitForFuture(teardown, 5, "DestroyWaitsForInFlightTask timed out");
  running.get(); teardown.get(); task.reset();
  EXPECT_EQ(queue.deleted, 1);
  EXPECT_EQ(queue.runnerDeleted, 1);
  expectSurvivor(*survivor, queue);
}

// Regression guard: own-thread teardown already works on BASE.
TEST(JsiAbiTaskRunner, DestroyFromInsideOwnTask) {
  DeferredHostQueue queue;
  auto destroyed = queue.runtime(), survivor = queue.runtime();
  v8_jsi_test_post_foreground_task(::jsi::abi::getAbiRuntime(*destroyed), [](void *p) {
    static_cast<decltype(destroyed) *>(p)->reset();
  }, &destroyed);
  auto task = queue.take();
  ASSERT_NE(task, nullptr);
  auto running = std::async(std::launch::async, [&] { task->run(task->data); });
  waitForFuture(running, 5, "DestroyFromInsideOwnTask timed out");
  running.get(); task.reset();
  EXPECT_EQ(queue.runnerDeleted, 1);
  EXPECT_EQ(queue.deleted, 1);
  expectSurvivor(*survivor, queue);
}

TEST(NodeApiTaskRunner, ScheduledFinalizerAtDestroy) {
  // Exercise cancellation with and without a host runner (both retain a ref).
  for (bool runner : {true, false}) {
    SCOPED_TRACE(runner ? "deferred runner" : "no runner");
    DeferredHostQueue queue;
    v8_node_api_runtime runtime{};
    ASSERT_EQ(v8_node_api_runtime_create(V8_NODE_API_ABI_VERSION,
        [](void *p, v8_node_api_config cfg) {
          if (!p) return napi_ok;
          return v8_node_api_config_set_task_runner(cfg, DeferredHostQueue::Post, p,
              [](void *, void *q) { ++static_cast<DeferredHostQueue *>(q)->runnerDeleted; }, nullptr);
        }, runner ? &queue : nullptr, &runtime), napi_ok);
    napi_env root{}, module{};
    ASSERT_EQ(v8_node_api_runtime_get_env(runtime, &root), napi_ok);
    ASSERT_EQ(v8_node_api_env_create_module(root, NAPI_VERSION_EXPERIMENTAL, &module), napi_ok);
    int finalized[2]{};
    for (int i = 0; i < 2; ++i) {
      auto env = i ? module : root;
      v8_node_api_env_scope scope{};
      ASSERT_EQ(v8_node_api_env_scope_open(env, &scope), napi_ok);
      ASSERT_EQ(node_api_post_finalizer(env,
          [](napi_env, void *p, void *) { ++*static_cast<int *>(p); }, &finalized[i], nullptr), napi_ok);
      ASSERT_EQ(v8_node_api_env_scope_close(scope, env), napi_ok);
    }
    ASSERT_EQ(v8_node_api_runtime_destroy(runtime), napi_ok);
    EXPECT_EQ(finalized[0], 1);
    EXPECT_EQ(finalized[1], 1);
    queue.drain(true);
    EXPECT_EQ(finalized[0], 1);
    EXPECT_EQ(finalized[1], 1);
    EXPECT_EQ(queue.deleted, runner ? 2 : 0);
    EXPECT_EQ(queue.runnerDeleted, runner ? 1 : 0);
  }
}

TEST(NodeApiTaskRunner, DrainDuringTeardownFinalizerCannotRunJs) {
  v8_node_api_runtime runtime{};
  ASSERT_EQ(v8_node_api_runtime_create(V8_NODE_API_ABI_VERSION, nullptr, nullptr, &runtime), napi_ok);
  napi_env root{}, module{};
  ASSERT_EQ(v8_node_api_runtime_get_env(runtime, &root), napi_ok);
  ASSERT_EQ(v8_node_api_env_create_module(root, NAPI_VERSION_EXPERIMENTAL, &module), napi_ok);
  int finalized = 0;
  int calls = 0;
  {
    v8_node_api_env_scope scope{};
    ASSERT_EQ(v8_node_api_env_scope_open(module, &scope), napi_ok);
    napi_value callback{};
    ASSERT_EQ(napi_create_function(module, "microtask", NAPI_AUTO_LENGTH,
        [](napi_env env, napi_callback_info info) -> napi_value {
          void* data{};
          EXPECT_EQ(napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data), napi_ok);
          ++*static_cast<int*>(data);
          return nullptr;
        }, &calls, &callback), napi_ok);
    ASSERT_EQ(v8_node_api_env_queue_microtask(module, callback), napi_ok);
    ASSERT_EQ(v8_node_api_env_scope_close(scope, module), napi_ok);
  }
  // The root finalizer also drains a module that is still attached and live.
  for (auto env : {root, module}) {
    v8_node_api_env_scope scope{};
    ASSERT_EQ(v8_node_api_env_scope_open(env, &scope), napi_ok);
    ASSERT_EQ(node_api_post_finalizer(env,
        [](napi_env env, void* data, void* hint) {
          bool drained = true;
          EXPECT_EQ(v8_node_api_env_drain_microtasks(env, -1, &drained), napi_cannot_run_js);
          EXPECT_FALSE(drained);
          const napi_extended_error_info* error{};
          EXPECT_EQ(napi_get_last_error_info(env, &error), napi_ok);
          ASSERT_NE(error, nullptr);
          EXPECT_EQ(error->error_code, napi_cannot_run_js);
          if (hint) {
            auto module = static_cast<napi_env>(hint);
            drained = true;
            EXPECT_EQ(v8_node_api_env_drain_microtasks(module, -1, &drained), napi_cannot_run_js);
            EXPECT_FALSE(drained);
            ASSERT_EQ(napi_get_last_error_info(module, &error), napi_ok);
            ASSERT_NE(error, nullptr);
            EXPECT_EQ(error->error_code, napi_cannot_run_js);
          }
          ++*static_cast<int*>(data);
        }, &finalized, env == root ? module : nullptr), napi_ok);
    ASSERT_EQ(v8_node_api_env_scope_close(scope, env), napi_ok);
  }
  ASSERT_EQ(v8_node_api_runtime_destroy(runtime), napi_ok);
  EXPECT_EQ(finalized, 2);
  EXPECT_EQ(calls, 0);
}

void checkQueuedInspectorDispatch(bool run) {
#ifdef _WIN32
  {
    DeferredHostQueue queue;
    auto port = freeInspectorPort();
    auto destroyed = queue.runtime(port), survivor = queue.runtime();
    auto response = connectInspector(port);
    ASSERT_NE(response.find("101 Switching"), std::string::npos) << response;
    ASSERT_TRUE(queue.wait());
    // Socket close queues disconnect; teardown drains the inspector's own
    // messages, but the host still owns its deferred dispatch task.
    destroyInspectorRuntime(destroyed);
    EXPECT_EQ(queue.runnerDeleted, 1);
    queue.drain(run);
    EXPECT_GE(queue.deleted.load(), 1);
    auto deleted = queue.deleted.load();
    expectSurvivor(*survivor, queue);
    EXPECT_EQ(queue.deleted, deleted + 1);
  }
#endif
}

TEST(JsiAbiInspector, DeletesQueuedDispatchAfterDestroy) { checkQueuedInspectorDispatch(false); }
TEST(JsiAbiInspector, SkipsQueuedDispatchAfterDestroy) { checkQueuedInspectorDispatch(true); }

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
namespace {
void expectInspectorListener(int port, bool enabled) {
  ULONG size = 0;
  std::vector<BYTE> storage;
  ULONG status;
  do {
    storage.resize(size);
    status = GetTcpTable(storage.empty() ? nullptr :
        reinterpret_cast<MIB_TCPTABLE *>(storage.data()), &size, FALSE);
  } while (status == ERROR_INSUFFICIENT_BUFFER);
  ASSERT_EQ(status, NO_ERROR);
  auto table = reinterpret_cast<MIB_TCPTABLE *>(storage.data());
  unsigned listeners = 0;
  for (DWORD i = 0; i < table->dwNumEntries; ++i) {
    const auto &row = table->table[i];
    if (row.dwState == MIB_TCP_STATE_LISTEN &&
        ntohs(static_cast<u_short>(row.dwLocalPort)) == port) {
      ++listeners;
      EXPECT_EQ(row.dwLocalAddr, htonl(INADDR_LOOPBACK));
    }
  }
  EXPECT_EQ(listeners, enabled ? 1u : 0u);
}
void expectInspectorConnectionRefused(ULONG address, int port) {
  WSADATA data{};
  ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
  InspectorClient client{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
  ASSERT_NE(client.socket, INVALID_SOCKET);
  sockaddr_in target{}; target.sin_family = AF_INET;
  target.sin_addr.s_addr = address; target.sin_port = htons(static_cast<u_short>(port));
  u_long nonblocking = 1;
  ASSERT_EQ(ioctlsocket(client.socket, FIONBIO, &nonblocking), 0);
  ASSERT_EQ(connect(client.socket, reinterpret_cast<sockaddr *>(&target), sizeof(target)), SOCKET_ERROR);
  auto error = WSAGetLastError();
  if (error == WSAEWOULDBLOCK) {
    fd_set writable, errors;
    FD_ZERO(&writable); FD_SET(client.socket, &writable); errors = writable;
    timeval timeout{10, 0};
    ASSERT_GT(select(0, nullptr, &writable, &errors, &timeout), 0);
    int size = sizeof(error);
    ASSERT_EQ(getsockopt(client.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size), 0);
  }
  EXPECT_EQ(error, WSAECONNREFUSED);
}
void checkInspectorOrigins(bool upgrade) {
  DeferredHostQueue queue;
  auto port = freeInspectorPort();
  auto runtime = queue.runtime(port);
  auto listing = inspectorRequest(port, "GET /json/list HTTP/1.1\r\nHost: localhost\r\n\r\n");
  auto url = listing.find("ws://");
  ASSERT_NE(url, std::string::npos) << listing;
  auto path = listing.find('/', url + 5), end = listing.find('"', path);
  ASSERT_NE(path, std::string::npos); ASSERT_NE(end, std::string::npos);
  auto targetId = listing.substr(path + 1, end - path - 1);
  ASSERT_FALSE(targetId.empty());
  struct Case { const char *headers; bool allowed; bool split = false; };
  // Every discovery route and upgrade must enforce the same allowlist and duplicate handling.
  const Case cases[] = {
      {"", true}, {"Origin: devtools://devtools\r\n", true},
      {"oRiGiN: DEVTOOLS://DevTools\r\n", true}, {"Origin-Extra: null\r\n", true},
      {"Origin: https://example.com\r\n", false}, {"Origin: http://localhost\r\n", false},
      {"Origin: null\r\n", false}, {"Origin:\r\n", false}, {"Origin: \r\n", false},
      {"Origin: file://devtools\r\n", false}, {"Origin: chrome-extension://devtools\r\n", false},
      {"Origin: unknown://devtools\r\n", false}, {"Origin: devtools:/devtools\r\n", false},
      {"Origin: devtools://devtools:9223\r\n", false}, {"Origin: devtools://devtools/\r\n", false},
      {"Origin: devtools://devtools?query\r\n", false}, {"Origin: devtools://devtools#fragment\r\n", false},
      {"Origin: devtools://devtools.evil\r\n", false}, {"Origin: devtools://devtools junk\r\n", false},
      {"Origin: devtools://devtools,devtools://devtools\r\n", false},
      {"Origin: devtools://devtools\r\n x\r\n", false},
      {"Origin: devtools://devtools\r\n", true, true},
      {"Origin: devtools://devtools\r\nOrigin: devtools://devtools\r\n", false},
      {"Origin: devtools://devtools\r\noRiGiN: devtools://devtools\r\n", false},
      {"Origin:\r\nOrigin: devtools://devtools\r\n", false},
      {"Origin: devtools://devtools\r\nOrigin:\r\n", false},
  };
  const std::vector<std::string> paths = upgrade ? std::vector<std::string>{listing.substr(path, end - path)} :
      std::vector<std::string>{"/json/list", "/json", "/json/version"};
  for (const auto &requestPath : paths) {
    SCOPED_TRACE(requestPath);
    std::string request = "GET " + requestPath + " HTTP/1.1\r\nHost: localhost\r\n";
    if (upgrade) request += "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
    for (const auto &test : cases) {
      SCOPED_TRACE(test.headers);
      {
        InspectorClient client;
        auto response = inspectorRequest(port, request + test.headers + "\r\n", &client.socket,
            test.split && upgrade ? request.size() + sizeof("Origin: devtools://") - 1 : 0);
        if (test.allowed) {
          EXPECT_NE(response.find(upgrade ? "101 Switching" : "200 OK"), std::string::npos) << response;
          if (upgrade) {
            ASSERT_TRUE(queue.wait());
            pumpInspector(*runtime, queue);
          } else {
            EXPECT_NE(response.find(requestPath == "/json/version" ? "Protocol-Version" : "webSocketDebuggerUrl"),
                std::string::npos) << response;
          }
        } else {
          EXPECT_EQ(response.find("101 Switching"), std::string::npos) << response;
          EXPECT_EQ(response.find("webSocketDebuggerUrl"), std::string::npos) << response;
          EXPECT_EQ(response.find("application/json"), std::string::npos) << response;
          EXPECT_EQ(response.find("200 OK"), std::string::npos) << response;
          auto body = response.find("\r\n\r\n");
          ASSERT_NE(body, std::string::npos) << response;
          EXPECT_GT(response.size(), body + 4); // Read the rejection body, not just its headers.
          EXPECT_EQ(response.substr(body + 4).find(targetId), std::string::npos) << response;
        }
      }
      if (test.allowed && upgrade) {
        ASSERT_TRUE(queue.wait());
        pumpInspector(*runtime, queue);
      }
    }
  }
}
} // namespace

TEST(JsiAbiInspectorAccess, ListenerIsLoopbackAndRefusesNonLoopbackInterfaces) {
  DeferredHostQueue queue;
  auto port = freeInspectorPort();
  auto runtime = queue.runtime(port);
  ASSERT_NE(inspectorRequest(port, "GET /json/list HTTP/1.1\r\nHost: localhost\r\n\r\n")
      .find("200 OK"), std::string::npos);
  expectInspectorListener(port, true);
  ULONG size = 0;
  std::vector<BYTE> storage;
  ULONG status;
  do {
    storage.resize(size);
    status = GetAdaptersAddresses(AF_INET, 0, nullptr, storage.empty() ? nullptr :
        reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data()), &size);
  } while (status == ERROR_BUFFER_OVERFLOW);
  ASSERT_EQ(status, NO_ERROR);
  auto adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
  unsigned checked = 0;
  for (auto adapter = adapters; adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp) continue;
    for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
      auto address = reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr)->sin_addr.s_addr;
      if (!address || (ntohl(address) >> 24) == 127) continue;
      SCOPED_TRACE(ntohl(address));
      expectInspectorConnectionRefused(address, port);
      ++checked;
    }
  }
  EXPECT_GT(checked, 0u) << "An operational non-loopback IPv4 interface is required";
}

TEST(JsiAbiInspectorAccess, UpgradeAllowsOnlyNativeAndDevToolsOrigins) { checkInspectorOrigins(true); }
TEST(JsiAbiInspectorAccess, DiscoveryAllowsOnlyNativeAndDevToolsOrigins) { checkInspectorOrigins(false); }

TEST(JsiAbiInspectorAccess, LoopbackDiscoversAttachesAndEvaluates) {
  DeferredHostQueue queue;
  auto port = freeInspectorPort();
  auto runtime = queue.runtime(port);
  checkInspectorEvaluation(*runtime, queue, port);
}

TEST(JsiAbiInspectorAccess, DisabledInspectorDoesNotListen) {
  auto port = freeInspectorPort();
  v8runtime::V8RuntimeArgs args;
  args.inspectorPort = static_cast<uint16_t>(port);
  args.flags.enableInspector = false;
  auto runtime = v8runtime::makeV8Runtime(std::move(args));
  EXPECT_EQ(runtime->evaluateJavaScript(std::make_shared<StringBuffer>("1+1"), "disabled.js").getNumber(), 2);
  expectInspectorListener(port, false);
  expectInspectorConnectionRefused(htonl(INADDR_LOOPBACK), port);
}
#endif

TEST(JsiAbiInspector, ConnectedTeardown) {
#ifdef _WIN32
  DeferredHostQueue queue;
  auto port = freeInspectorPort();
  auto destroyed = queue.runtime(port), survivor = queue.runtime();
  InspectorClient client;
  auto response = connectInspector(port, &client);
  ASSERT_NE(response.find("101 Switching"), std::string::npos) << response;
  // Wait on the queue predicate: the HTTP upgrade response can precede CONNECT.
  ASSERT_TRUE(queue.wait());
  auto connectTask = queue.take();
  ASSERT_NE(connectTask, nullptr);
  connectTask->run(connectTask->data); // Exactly one drain establishes kConnected.
  connectTask.reset();
  int runs = 0;
  postMarker(*destroyed, runs);
  auto teardown = std::async(std::launch::async, [&] { destroyed.reset(); });
  waitForFuture(teardown, 10, "ConnectedTeardown timed out");
  teardown.get();
  char buffer[256];
  int received;
  do { received = recv(client.socket, buffer, sizeof(buffer), 0); } while (received > 0);
  EXPECT_EQ(received, 0) << "debugger must see EOF: " << WSAGetLastError();
  EXPECT_EQ(queue.runnerDeleted, 1);
  queue.drain(true);
  EXPECT_EQ(runs, 0);
  EXPECT_GE(queue.deleted.load(), 2);
  expectSurvivor(*survivor, queue);
#endif
}

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
TEST(JsiAbiInspector, ListenerSurvivesHandlerException) {
  auto port = freeInspectorPort();
  std::atomic<int> accepted{0};
  auto server = std::make_shared<inspector::tcp_server>(port,
      [](auto, void *data) {
        ++*static_cast<std::atomic<int> *>(data);
        throw std::runtime_error("inspector connection failure");
      }, &accepted);
  std::promise<void> recovered;
  server->post([] { throw std::runtime_error("inspector handler failure"); });
  server->post([&] { recovered.set_value(); });
  auto io = std::async(std::launch::async, [server] { server->run(); });
  EXPECT_EQ(recovered.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
  for (int i = 1; i <= 2; ++i) {
    inspectorRequest(port, "GET /json/list HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_EQ(accepted.load(), i); // Callback closes the socket before the client returns.
  }
  server->stop();
  waitForFuture(io, 5);
  io.get();
}

TEST(JsiAbiInspector, RepeatedHandlerFailuresStopWithoutSpinning) {
  auto port = freeInspectorPort();
  std::promise<std::shared_ptr<inspector::tcp_connection>> accepted;
  auto server = std::make_shared<inspector::tcp_server>(port,
      [](auto connection, void *p) {
        static_cast<decltype(accepted) *>(p)->set_value(std::move(connection));
      }, &accepted);
  int completed = 0;
  // Successful handlers reset the failure budget; only consecutive errors stop IO.
  for (int i = 0; i < 16; ++i) {
    server->post([] { throw std::runtime_error("recoverable handler failure"); });
    server->post([&] { ++completed; });
  }
  auto io = std::async(std::launch::async, [server] { server->run(); });
  asio::io_context context;
  asio::ip::tcp::socket client(context);
  client.connect({asio::ip::address_v4::loopback(), static_cast<unsigned short>(port)});
  auto connection = accepted.get_future();
  waitForFuture(connection, 5);
  auto retained = connection.get(); // Keep an accepted socket alive across the cap.
  std::atomic<int> eofCalls{0};
  retained->registerReadCallback([](auto &, bool eof, void *p) {
    if (eof) {
      ++*static_cast<std::atomic<int> *>(p);
      throw std::runtime_error("EOF handler failure");
    }
  }, &eofCalls);
  for (int i = 0; i < 8; ++i)
    server->post([] { throw std::runtime_error("persistent handler failure"); });
  server->post([&] { ADD_FAILURE() << "failure budget did not stop IO"; });
  waitForFuture(io, 5);
  io.get();
  EXPECT_EQ(completed, 16);
  EXPECT_EQ(eofCalls, 1);
  char eof;
  asio::error_code ec;
  client.non_blocking(true); // A broken cap must fail, not block the regression test.
  EXPECT_EQ(client.read_some(asio::buffer(&eof, 1), ec), 0u);
  EXPECT_EQ(ec, asio::error::eof);
  WSADATA data{};
  ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
  SOCKET probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{}; address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<u_short>(port)); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  EXPECT_EQ(connect(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address)), SOCKET_ERROR);
  EXPECT_EQ(WSAGetLastError(), WSAECONNREFUSED);
  closesocket(probe); WSACleanup();
  auto start = std::chrono::steady_clock::now();
  server->stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}

TEST(JsiAbiInspector, StopWithUnresponsiveIoThreadIsBounded) {
  auto server = std::make_shared<inspector::tcp_server>(freeInspectorPort(),
      [](auto, void *) {}, nullptr);
  std::promise<void> entered, resume;
  auto ready = resume.get_future();
  server->post([&] { entered.set_value(); ready.wait(); });
  auto io = std::async(std::launch::async, [server] { server->run(); });
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
  auto stop = std::async(std::launch::async, [server] { server->stop(); });
  waitForFuture(stop, 7, "Inspector stop exceeded its deadline");
  stop.get();
  std::weak_ptr<inspector::tcp_server> retained = server;
  server.reset();
  EXPECT_FALSE(retained.expired()); // The paused io thread still owns its state.
  resume.set_value();
  waitForFuture(io, 5);
  io.get();
  EXPECT_TRUE(retained.expired());
}

TEST(JsiAbiInspector, QueuedMessagesFlushBeforeClose) {
  asio::io_context context;
  asio::ip::tcp::acceptor listener(context, {asio::ip::tcp::v4(), 0});
  asio::ip::tcp::socket reader(context);
  reader.connect({asio::ip::address_v4::loopback(), listener.local_endpoint().port()});
  auto budget = std::make_shared<inspector::TransportBudget>();
  auto writer = std::make_shared<inspector::tcp_connection>(listener.accept(), budget->TryAdmitConnection());
  std::string expected;
  // One handler queues every send and the close before any write can complete.
  asio::post(context, [writer, &expected] {
    for (int i = 0; i < 16; ++i) {
      auto message = std::to_string(i) + " queued inspector message\n";
      expected += message;
      auto buffer = inspector::OutboundBuffer::Create(writer->account(), message.size());
      ASSERT_TRUE(buffer);
      std::copy(message.begin(), message.end(), buffer->data.begin());
      writer->write_async(std::move(buffer));
    }
    writer->write_async(inspector::OutboundBuffer::Create(writer->account(), 0));
  });
  auto io = std::async(std::launch::async, [&] { context.run(); });
  DWORD timeout = 5000;
  setsockopt(reader.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
      reinterpret_cast<char *>(&timeout), sizeof(timeout));
  std::string received;
  asio::error_code ec;
  char buffer[1024];
  while (auto n = reader.read_some(asio::buffer(buffer), ec)) received.append(buffer, n);
  EXPECT_EQ(ec, asio::error::eof);
  waitForFuture(io, 5);
  io.get();
  EXPECT_EQ(received, expected);
}
#endif

#if defined(_WIN32) && defined(V8JSI_ENABLE_INSPECTOR)
namespace {
template<class Scenario>
void runBoundedInspectorTransportScenario(Scenario scenario) {
  // Create, use and destroy the runtime on one thread. Bound the whole scenario,
  // including synchronous pumps, socket reads and assertion-failure cleanup.
  auto completion = std::async(std::launch::async, std::move(scenario));
  waitForFuture(completion, 30, "Inspector transport scenario exceeded 30 seconds");
  completion.get();
}
template<class Predicate>
bool awaitTransport(Predicate predicate, int seconds = 5) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  do {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < deadline);
  return predicate();
}
bool openInspectorTcp(InspectorClient& client, int port) {
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data)) return false;
  client.socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (client.socket == INVALID_SOCKET) { WSACleanup(); return false; }
  int receiveBuffer = 1024;
  setsockopt(client.socket, SOL_SOCKET, SO_RCVBUF,
      reinterpret_cast<char*>(&receiveBuffer), sizeof(receiveBuffer));
  sockaddr_in address{}; address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<u_short>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return connect(client.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
}
void closeInspectorTcp(InspectorClient& client) {
  if (client.socket == INVALID_SOCKET) return;
  closesocket(client.socket); WSACleanup(); client.socket = INVALID_SOCKET;
}
bool drainInspectorToEof(SOCKET socket, int seconds = 5) {
  int receiveBuffer = 4 * 1024 * 1024;
  setsockopt(socket, SOL_SOCKET, SO_RCVBUF,
      reinterpret_cast<char*>(&receiveBuffer), sizeof(receiveBuffer));
  u_long nonblocking = 1;
  ioctlsocket(socket, FIONBIO, &nonblocking);
  bool closed = false, unexpected = false;
  bool done = awaitTransport([&] {
    char bytes[65536];
    int n = recv(socket, bytes, sizeof(bytes), 0);
    if (!n) return closed = true;
    if (n > 0) return false;
    auto error = WSAGetLastError();
    if (error == WSAECONNRESET || error == WSAECONNABORTED) return closed = true;
    if (error != WSAEWOULDBLOCK) return unexpected = true;
    return false;
  }, seconds);
  return done && closed && !unexpected;
}
struct LimitedInspectorTransport {
  const int port{freeInspectorPort()};
  std::mutex mutex;
  std::vector<std::weak_ptr<inspector::tcp_connection>> accepted;
  std::shared_ptr<inspector::tcp_server> server;
  std::future<void> io;
  explicit LimitedInspectorTransport(inspector::TransportLimits limits) {
    server = std::make_shared<inspector::tcp_server>(port, [](auto connection, void* data) {
      auto& self = *static_cast<LimitedInspectorTransport*>(data);
      connection->socket().set_option(asio::socket_base::send_buffer_size(1024));
      connection->registerReadCallback([](auto&, bool, void*) {}, nullptr);
      connection->read_loop_async();
      std::lock_guard<std::mutex> lock(self.mutex);
      self.accepted.push_back(connection);
    }, this, limits);
    io = std::async(std::launch::async, [s = server] { s->run(); });
  }
  std::shared_ptr<inspector::tcp_connection> connection(size_t index = 0) {
    std::shared_ptr<inspector::tcp_connection> result;
    EXPECT_TRUE(awaitTransport([&] {
      std::lock_guard<std::mutex> lock(mutex);
      if (accepted.size() > index) result = accepted[index].lock();
      return result != nullptr;
    }));
    return result;
  }
  void destroy() {
    if (!server) return;
    server->stop(); waitForFuture(io, 5); io.get(); server.reset();
  }
  ~LimitedInspectorTransport() { destroy(); }
};
inspector::TransportStats runtimeTransportStats(int port) {
  inspector::TransportStats stats;
  EXPECT_TRUE(v8_jsi_test_inspector_transport_stats(port, &stats));
  return stats;
}
} // namespace

TEST(JsiAbiInspectorTransport, ProductionBudgetsHaveExactInclusiveBoundaries) {
  constexpr size_t limit = 64 * 1024 * 1024;
  auto budget = std::make_shared<inspector::TransportBudget>();
  auto first = budget->TryAdmitConnection(), second = budget->TryAdmitConnection();
  ASSERT_TRUE(first->TryReserve(limit - 1));
  ASSERT_TRUE(first->TryReserve(1));
  EXPECT_EQ(budget->Stats().retained, limit);
  EXPECT_FALSE(first->TryReserve(1));
  first->Release(limit);
  EXPECT_FALSE(first->TryReserve(1)); // A refund cannot revive a failed session.
  EXPECT_EQ(budget->Stats().overflows, 1u);
  EXPECT_FALSE(second->TryReserve(limit + 1)); // No exemption on an idle account.
  first.reset(); second.reset();
  first = budget->TryAdmitConnection(); second = budget->TryAdmitConnection();
  auto third = budget->TryAdmitConnection();
  ASSERT_TRUE(first->TryReserve(limit));
  ASSERT_TRUE(second->TryReserve(limit - 1));
  ASSERT_TRUE(third->TryReserve(1)); // Aggregate just below, at, and above its limit.
  EXPECT_FALSE(third->TryReserve(1));
  EXPECT_EQ(budget->Stats().peak, 2 * limit);
  third->Release(1);
  second->Release(limit - 1); first->Release(limit);
  second.reset(); first.reset(); third.reset();
  EXPECT_EQ(budget->Stats().retained, 0u);
  EXPECT_EQ(budget->Stats().admitted, 0u);
  // Two full accounts must also reject the first byte on a third, idle account.
  first = budget->TryAdmitConnection(); second = budget->TryAdmitConnection();
  third = budget->TryAdmitConnection();
  ASSERT_TRUE(first->TryReserve(limit)); ASSERT_TRUE(second->TryReserve(limit));
  EXPECT_FALSE(third->TryReserve(1));
  EXPECT_EQ(budget->Stats().overflows, 4u);
  first->Release(limit); second->Release(limit);
  first.reset(); second.reset(); third.reset();
  std::vector<std::shared_ptr<inspector::ConnectionAccount>> accounts;
  for (int i = 0; i < 32; ++i) accounts.push_back(budget->TryAdmitConnection());
  EXPECT_EQ(budget->Stats().admitted, 32u);
  EXPECT_FALSE(budget->TryAdmitConnection());
  EXPECT_EQ(budget->Stats().rejected_connections, 1u);
  auto buffer = inspector::OutboundBuffer::Create(accounts.back(), 7);
  ASSERT_TRUE(buffer);
  accounts.pop_back();
  EXPECT_FALSE(budget->TryAdmitConnection()); // Buffer retains the admitted slot.
  buffer.reset();
  EXPECT_TRUE(budget->TryAdmitConnection());
  accounts.clear();
  EXPECT_EQ(budget->Stats().retained, 0u);
  EXPECT_EQ(budget->Stats().admitted, 0u);
}

TEST(JsiAbiInspectorTransport, BufferAdmissionIncludesMetadataAndStillFitsFourMiB) {
  constexpr size_t limit = 1024, overhead = inspector::OutboundBuffer::kOverhead;
  auto budget = std::make_shared<inspector::TransportBudget>(inspector::TransportLimits{limit, 2 * limit, 2});
  // Fresh accounts isolate inclusive payload+metadata boundaries and size_t overflow.
  for (size_t bytes : {limit - overhead - 1, limit - overhead, limit - overhead + 1, SIZE_MAX}) {
    auto account = budget->TryAdmitConnection();
    auto buffer = inspector::OutboundBuffer::Create(account, bytes);
    EXPECT_EQ(buffer != nullptr, bytes <= limit - overhead);
    EXPECT_EQ(budget->Stats().retained, buffer ? bytes + overhead : 0u);
    buffer.reset();
    EXPECT_EQ(budget->Stats().retained, 0u);
  }
  budget = std::make_shared<inspector::TransportBudget>();
  auto buffer = inspector::OutboundBuffer::Create(budget->TryAdmitConnection(), 4 * 1024 * 1024);
  ASSERT_TRUE(buffer);
  EXPECT_EQ(budget->Stats().retained, 4 * 1024 * 1024 + overhead);
  buffer.reset();
  EXPECT_EQ(budget->Stats().retained, 0u);
}

TEST(JsiAbiInspectorTransport, TinyBuffersConsumeBothBudgetsIncludingMetadata) {
  constexpr size_t charge = 1 + inspector::OutboundBuffer::kOverhead;
  auto budget = std::make_shared<inspector::TransportBudget>(inspector::TransportLimits{2 * charge, 3 * charge, 2});
  auto first = budget->TryAdmitConnection(), second = budget->TryAdmitConnection();
  auto one = inspector::OutboundBuffer::Create(first, 1);
  auto two = inspector::OutboundBuffer::Create(first, 1);
  auto three = inspector::OutboundBuffer::Create(second, 1);
  ASSERT_TRUE(one && two && three);
  EXPECT_EQ(budget->Stats().retained, 3 * charge);
  EXPECT_FALSE(inspector::OutboundBuffer::Create(first, 0)); // Metadata alone exceeds the connection budget.
  EXPECT_FALSE(inspector::OutboundBuffer::Create(second, 1)); // Listener budget is also full.
  EXPECT_EQ(budget->Stats().overflows, 2u);
  one.reset(); two.reset(); three.reset();
  EXPECT_EQ(budget->Stats().retained, 0u);
}

TEST(JsiAbiInspectorTransport, InFlightAndQueuedBuffersRemainChargedUntilCancellation) {
  constexpr size_t chunk = 512 * 1024;
  constexpr size_t charge = chunk + inspector::OutboundBuffer::kOverhead;
  LimitedInspectorTransport transport({2 * charge, 4 * charge, 2});
  InspectorClient client;
  ASSERT_TRUE(openInspectorTcp(client, transport.port));
  auto connection = transport.connection();
  ASSERT_TRUE(connection);
  auto budget = transport.server->budget();
  std::promise<void> queued;
  transport.server->post([connection, budget, &queued, charge] {
    auto buffer = inspector::OutboundBuffer::Create(connection->account(), chunk);
    std::weak_ptr<inspector::OutboundBuffer> first = buffer;
    connection->write_async(std::move(buffer));
    connection->write_async(inspector::OutboundBuffer::Create(connection->account(), chunk));
    EXPECT_FALSE(first.expired()); // The active async_write owns the first buffer.
    EXPECT_EQ(budget->Stats().retained, 2 * charge);
    connection->write_raw("x", 1);
    connection->write_raw("x", 1); // Sticky failure logs/counts only once.
    EXPECT_EQ(budget->Stats().overflows, 1u);
    EXPECT_EQ(budget->Stats().retained, charge); // Queue refunded, active send still owned.
    queued.set_value();
  });
  auto ready = queued.get_future(); waitForFuture(ready, 5);
  EXPECT_TRUE(drainInspectorToEof(client.socket));
  EXPECT_TRUE(awaitTransport([&] { return budget->Stats().retained == 0; }));
  EXPECT_EQ(budget->Stats().peak, 2 * charge);
  connection.reset();
  transport.destroy();
  EXPECT_EQ(budget->Stats().admitted, 0u);
}

TEST(JsiAbiInspectorTransport, PeerEofRefundsQueuedBuffersBeforeConnectionDestruction) {
  runBoundedInspectorTransportScenario([] {
    constexpr size_t chunk = 512 * 1024;
    constexpr size_t charge = chunk + inspector::OutboundBuffer::kOverhead;
    struct EofObservation {
      std::shared_ptr<inspector::TransportBudget> budget;
      std::promise<size_t> retained;
    } eof; // Outlives transport teardown, including assertion-failure cleanup.
    LimitedInspectorTransport transport({2 * charge, 4 * charge, 2});
    InspectorClient client;
    ASSERT_TRUE(openInspectorTcp(client, transport.port));
    auto connection = transport.connection();
    ASSERT_TRUE(connection);
    auto budget = transport.server->budget();
    eof.budget = budget;
    std::promise<void> queued;
    transport.server->post([connection, budget, &eof, &queued, charge] {
      connection->registerReadCallback([](auto&, bool isEof, void* data) {
        auto& observation = *static_cast<EofObservation*>(data);
        if (isEof) observation.retained.set_value(observation.budget->Stats().retained);
      }, &eof);
      auto buffer = inspector::OutboundBuffer::Create(connection->account(), chunk);
      std::weak_ptr<inspector::OutboundBuffer> active = buffer;
      connection->write_async(std::move(buffer));
      connection->write_async(inspector::OutboundBuffer::Create(connection->account(), chunk));
      EXPECT_FALSE(active.expired());
      EXPECT_EQ(budget->Stats().retained, 2 * charge);
      queued.set_value();
    });
    auto ready = queued.get_future(); waitForFuture(ready, 5);
    // Half-close only the peer's send side; it never reads the queued output.
    ASSERT_EQ(shutdown(client.socket, SD_SEND), 0);
    auto closed = eof.retained.get_future(); waitForFuture(closed, 5);
    EXPECT_LE(closed.get(), charge); // EOF refunds the queue, not the active write.
    EXPECT_TRUE(awaitTransport([&] { return budget->Stats().retained == 0; }));
    EXPECT_EQ(budget->Stats().overflows, 0u); // No overflow/abort_async cleanup.
    EXPECT_EQ(budget->Stats().admitted, 1u); // Owner is still alive at both assertions.
    EXPECT_EQ(budget->Stats().peak, 2 * charge);
    connection.reset(); transport.destroy();
    EXPECT_EQ(budget->Stats().admitted, 0u);
  });
}

TEST(JsiAbiInspectorTransport, PostedBufferKeepsItsChargeThroughSuccessfulWrite) {
  constexpr size_t charge = 5 + inspector::OutboundBuffer::kOverhead;
  LimitedInspectorTransport transport({charge, 2 * charge, 2});
  InspectorClient client;
  ASSERT_TRUE(openInspectorTcp(client, transport.port));
  auto connection = transport.connection();
  ASSERT_TRUE(connection);
  auto budget = transport.server->budget();
  std::promise<void> entered, resume;
  auto resumed = resume.get_future();
  transport.server->post([&] { entered.set_value(); resumed.wait(); });
  auto blocked = entered.get_future(); waitForFuture(blocked, 5);
  auto buffer = inspector::OutboundBuffer::Create(connection->account(), 5);
  ASSERT_TRUE(buffer);
  std::fill(buffer->data.begin(), buffer->data.end(), 'x');
  std::weak_ptr<inspector::OutboundBuffer> retained = buffer;
  transport.server->post([connection, buffer = std::move(buffer), budget, charge] {
    EXPECT_EQ(budget->Stats().retained, charge);
    connection->write_async(buffer);
    EXPECT_EQ(budget->Stats().retained, charge);
  });
  EXPECT_EQ(budget->Stats().retained, charge);
  resume.set_value();
  char received[5]{};
  DWORD timeout = 5000;
  setsockopt(client.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
  ASSERT_EQ(recv(client.socket, received, sizeof(received), MSG_WAITALL), sizeof(received));
  EXPECT_EQ(std::string(received, sizeof(received)), "xxxxx");
  EXPECT_TRUE(awaitTransport([&] { return budget->Stats().retained == 0; }));
  EXPECT_TRUE(retained.expired());
  EXPECT_EQ(budget->Stats().peak, charge);
  connection.reset(); transport.destroy();
  EXPECT_EQ(budget->Stats().admitted, 0u);
}

TEST(JsiAbiInspectorTransport, SendReservesBeforeDispatchAndRejectsBeforePosting) {
  inspector::SendDispatchObservations observed;
  v8_jsi_test_inspector_send_dispatch(&observed);
  ASSERT_TRUE(observed.strand_blocked);
  EXPECT_EQ(observed.before_send.retained, 0u);
  EXPECT_EQ(observed.after_send.retained, 42 + inspector::OutboundBuffer::kOverhead);
  EXPECT_EQ(observed.posted_after_send, observed.posted_before + 1);
  EXPECT_EQ(observed.after_overflow.retained, observed.after_send.retained);
  EXPECT_EQ(observed.after_overflow.overflows, 1u);
  EXPECT_EQ(observed.posted_after_overflow, observed.posted_after_send + 1); // Abort only, no data dispatch.
  EXPECT_EQ(observed.posted_after_failure, observed.posted_after_overflow);
  EXPECT_EQ(observed.completed, 1u); // Only the blocked handler ran, not either posted Send action.
  EXPECT_EQ(observed.after_release.retained, 0u);
  EXPECT_EQ(observed.after_release.admitted, 0u);
}

TEST(JsiAbiInspectorTransport, ConnectionCapIncludesHandshakeAndClosingTransports) {
  LimitedInspectorTransport transport({1024 * 1024, 2 * 1024 * 1024, 2});
  InspectorClient pending, closing, excess;
  ASSERT_TRUE(openInspectorTcp(pending, transport.port)); // No HTTP handshake yet.
  ASSERT_TRUE(openInspectorTcp(closing, transport.port));
  auto connection = transport.connection(1);
  ASSERT_TRUE(connection);
  auto budget = transport.server->budget();
  std::promise<void> queued;
  transport.server->post([connection, &queued] {
    connection->write_async(inspector::OutboundBuffer::Create(connection->account(), 512 * 1024));
    connection->write_async(inspector::OutboundBuffer::Create(connection->account(), 0));
    queued.set_value();
  });
  auto ready = queued.get_future(); waitForFuture(ready, 5);
  connection.reset();
  EXPECT_EQ(budget->Stats().admitted, 2u);
  ASSERT_TRUE(openInspectorTcp(excess, transport.port));
  EXPECT_TRUE(drainInspectorToEof(excess.socket));
  EXPECT_EQ(budget->Stats().rejected_connections, 1u);
  EXPECT_TRUE(awaitTransport([&] { return budget->Stats().admitted == 1; })); // 2 s closing deadline.
  InspectorClient replacement;
  ASSERT_TRUE(openInspectorTcp(replacement, transport.port));
  EXPECT_TRUE(awaitTransport([&] { return budget->Stats().admitted == 2; }));
  transport.destroy();
  EXPECT_EQ(budget->Stats().retained, 0u);
  EXPECT_EQ(budget->Stats().admitted, 0u);
}

namespace {
enum class InspectorOverflowCase { RepeatedFourMiB, SingleOversizedReply };

void checkInspectorOverflowRecovery(InspectorOverflowCase scenario) {
  runBoundedInspectorTransportScenario([scenario] {
    const bool oversized = scenario == InspectorOverflowCase::SingleOversizedReply;
    DeferredHostQueue queue;
    int port = freeInspectorPort();
    auto runtime = queue.runtime(port);
    InspectorClient client;
    ASSERT_NE(connectInspector(port, &client).find("101 Switching"), std::string::npos);
    ASSERT_TRUE(queue.wait()); pumpInspector(*runtime, queue);
    int receiveBuffer = 1024;
    setsockopt(client.socket, SOL_SOCKET, SO_RCVBUF,
        reinterpret_cast<char*>(&receiveBuffer), sizeof(receiveBuffer));
    if (oversized) {
      ASSERT_TRUE(awaitTransport([&] { return runtimeTransportStats(port).retained == 0; }));
      ASSERT_EQ(runtimeTransportStats(port).overflows, 0u);
    }
    // One 64 MiB + 1 value is rejected before allocating an outbound frame;
    // repeated 4 MiB replies instead fill the non-reading connection's budget.
    const char* message = oversized
        ? R"json({"id":1,"method":"Runtime.evaluate","params":{"expression":"'x'.repeat(67108865)","returnByValue":true}})json"
        : R"json({"id":1,"method":"Runtime.evaluate","params":{"expression":"'x'.repeat(4*1024*1024)","returnByValue":true}})json";
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(30);
    const int attempts = oversized ? 1 : 24;
    for (int i = 0; i < attempts && runtimeTransportStats(port).overflows == 0; ++i) {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline);
      ASSERT_TRUE(sendInspectorMessage(client.socket, message));
      ASSERT_TRUE(queue.wait()); pumpInspector(*runtime, queue);
    }
    const int recoverySeconds = oversized ? 10 : 5;
    EXPECT_EQ(runtimeTransportStats(port).overflows, 1u);
    EXPECT_TRUE(awaitTransport([&] { return runtimeTransportStats(port).retained == 0; }, recoverySeconds));
    EXPECT_LT(std::chrono::steady_clock::now(), deadline);
    EXPECT_TRUE(drainInspectorToEof(client.socket, recoverySeconds));
    EXPECT_TRUE(awaitTransport([&] { return runtimeTransportStats(port).admitted == 0; }, recoverySeconds));
    if (oversized)
      EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(30));
    checkInspectorEvaluation(*runtime, queue, port);
    EXPECT_EQ(runtimeTransportStats(port).overflows, 1u); // Diagnostic counted once.
  });
}
} // namespace

TEST(JsiAbiInspectorTransport, NonReadingDebuggerOverflowsClosesAndAllowsFreshEvaluation) {
  checkInspectorOverflowRecovery(InspectorOverflowCase::RepeatedFourMiB);
}

TEST(JsiAbiInspectorTransport, SingleOversizedReplyClosesAndAllowsFreshEvaluation) {
  checkInspectorOverflowRecovery(InspectorOverflowCase::SingleOversizedReply);
}

namespace {
enum class InspectorRejectionCase {
  DiscoveryOrigin, UpgradeOrigin, UnknownTargetUpgrade, UnknownDiscoveryPath, MalformedRequest
};

void checkInspectorRejectionRecovery(InspectorRejectionCase scenario) {
  runBoundedInspectorTransportScenario([scenario] {
    DeferredHostQueue queue;
    int port = freeInspectorPort();
    auto runtime = queue.runtime(port);
    auto listing = inspectorRequest(port, "GET /json/list HTTP/1.1\r\nHost: localhost\r\n\r\n");
    auto url = listing.find("ws://");
    ASSERT_NE(url, std::string::npos) << listing;
    auto path = listing.find('/', url + 5), end = listing.find('"', path);
    ASSERT_NE(path, std::string::npos); ASSERT_NE(end, std::string::npos);
    const bool origin = scenario == InspectorRejectionCase::DiscoveryOrigin ||
        scenario == InspectorRejectionCase::UpgradeOrigin;
    const bool upgrade = scenario == InspectorRejectionCase::UpgradeOrigin ||
        scenario == InspectorRejectionCase::UnknownTargetUpgrade;
    // Target ids are UUIDs; this cannot match the runtime's target.
    std::string requestPath = scenario == InspectorRejectionCase::UnknownTargetUpgrade
        ? "/nonexistent-target" : upgrade ? listing.substr(path, end - path) :
        scenario == InspectorRejectionCase::UnknownDiscoveryPath ? "/json/unknown" : "/json/list";
    std::string request = "GET " + requestPath + " HTTP/1.1\r\nHost: localhost\r\n";
    if (origin) request += "Origin: http://example.com\r\n";
    if (upgrade) request += "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";
    request += "\r\n";
    // Invalid at the first byte, with no trailing bytes left unread at close.
    if (scenario == InspectorRejectionCase::MalformedRequest) request = "!";
    const std::string expected = "HTTP/1.0 400 Bad Request\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n\r\n"
        "WebSockets request was expected\r\n";
    ASSERT_TRUE(awaitTransport([&] { return runtimeTransportStats(port).admitted == 0; }));
    std::vector<std::unique_ptr<InspectorClient>> clients;
    // Origin cases exceed the 32-slot cap; other paths repeat three times for speed.
    // Never close a client socket, even after server EOF.
    const int attempts = origin ? 33 : 3;
    for (int i = 0; i < attempts; ++i) {
      SCOPED_TRACE(i);
      auto client = std::make_unique<InspectorClient>();
      ASSERT_TRUE(openInspectorTcp(*client, port)); // The scenario watchdog bounds connect.
      SOCKET socket = client->socket;
      clients.push_back(std::move(client));
      ASSERT_EQ(send(socket, request.data(), static_cast<int>(request.size()), 0),
          static_cast<int>(request.size()));
      u_long nonblocking = 1;
      ASSERT_EQ(ioctlsocket(socket, FIONBIO, &nonblocking), 0);
      std::string response;
      bool closed = false;
      int error = 0;
      ASSERT_TRUE(awaitTransport([&] {
        char bytes[512];
        int n = recv(socket, bytes, sizeof(bytes), 0);
        if (n > 0) {
          response.append(bytes, n);
          return response.size() > expected.size();
        }
        if (!n) return closed = true;
        error = WSAGetLastError();
        if (error == WSAECONNRESET || error == WSAECONNABORTED) {
          closed = response == expected; // Reset counts only after the full body.
          return true;
        }
        return error != WSAEWOULDBLOCK;
      })) << "Server did not close the rejected connection within 5 seconds";
      ASSERT_EQ(response, expected);
      ASSERT_TRUE(closed) << "Unexpected receive error: " << error;
    }
    ASSERT_TRUE(awaitTransport([&] {
      auto stats = runtimeTransportStats(port);
      return stats.admitted == 0 && stats.retained == 0;
    }));
    EXPECT_EQ(runtimeTransportStats(port).rejected_connections, 0u);
    checkInspectorEvaluation(*runtime, queue, port); // All rejected client sockets remain open.
  });
}
} // namespace

TEST(JsiAbiInspectorTransport, RejectedDiscoveryOriginFlushesResponseClosesAndReleasesSlots) {
  checkInspectorRejectionRecovery(InspectorRejectionCase::DiscoveryOrigin);
}

TEST(JsiAbiInspectorTransport, RejectedUpgradeOriginFlushesResponseClosesAndReleasesSlots) {
  checkInspectorRejectionRecovery(InspectorRejectionCase::UpgradeOrigin);
}

TEST(JsiAbiInspectorTransport, UnknownTargetUpgradeFlushesResponseClosesAndReleasesSlots) {
  checkInspectorRejectionRecovery(InspectorRejectionCase::UnknownTargetUpgrade);
}

TEST(JsiAbiInspectorTransport, UnknownDiscoveryPathFlushesResponseClosesAndReleasesSlots) {
  checkInspectorRejectionRecovery(InspectorRejectionCase::UnknownDiscoveryPath);
}

TEST(JsiAbiInspectorTransport, MalformedRequestFlushesResponseClosesAndReleasesSlots) {
  checkInspectorRejectionRecovery(InspectorRejectionCase::MalformedRequest);
}

TEST(JsiAbiInspectorTransport, IdleConnectionCapRejectsUpgradeAndReusesReleasedSlot) {
  DeferredHostQueue queue;
  int port = freeInspectorPort();
  auto runtime = queue.runtime(port);
  auto listing = inspectorRequest(port, "GET /json/list HTTP/1.1\r\nHost: localhost\r\n\r\n");
  auto url = listing.find("ws://");
  ASSERT_NE(url, std::string::npos);
  auto path = listing.find('/', url + 5), end = listing.find('"', path);
  ASSERT_NE(path, std::string::npos); ASSERT_NE(end, std::string::npos);
  auto id = listing.substr(path + 1, end - path - 1);
  ASSERT_TRUE(awaitTransport([&] { return runtimeTransportStats(port).admitted == 0; }));
  std::vector<std::unique_ptr<InspectorClient>> idle;
  for (int i = 0; i < 32; ++i) {
    auto client = std::make_unique<InspectorClient>();
    ASSERT_TRUE(openInspectorTcp(*client, port));
    idle.push_back(std::move(client));
  }
  ASSERT_TRUE(awaitTransport([&] { return runtimeTransportStats(port).admitted == 32; }));
  InspectorClient excess;
  ASSERT_TRUE(openInspectorTcp(excess, port));
  auto request = "GET /" + id + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
      "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  send(excess.socket, request.data(), static_cast<int>(request.size()), 0);
  char response[512];
  DWORD timeout = 5000;
  setsockopt(excess.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
  int n = recv(excess.socket, response, sizeof(response), 0);
  EXPECT_LE(n, 0); // In particular, no 101 or debugger session.
  EXPECT_TRUE(awaitTransport([&] { return runtimeTransportStats(port).rejected_connections == 1; }));
  idle.pop_back();
  ASSERT_TRUE(awaitTransport([&] { return runtimeTransportStats(port).admitted == 31; }));
  InspectorClient replacement;
  ASSERT_TRUE(awaitTransport([&] {
    return attachInspector(port, id, &replacement).find("101 Switching") != std::string::npos;
  }));
  ASSERT_TRUE(queue.wait()); pumpInspector(*runtime, queue);
  ASSERT_TRUE(sendInspectorMessage(replacement.socket,
      R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"6*7","returnByValue":true}})"));
  ASSERT_TRUE(queue.wait()); pumpInspector(*runtime, queue);
  EXPECT_NE(readInspectorMessage(replacement.socket).find(R"("value":42)"), std::string::npos);
  closeInspectorTcp(replacement); idle.clear();
  EXPECT_TRUE(awaitTransport([&] { return runtimeTransportStats(port).admitted == 0; }));
}

TEST(JsiAbiInspectorTransport, FourMiBReplyArrivesIntact) {
  DeferredHostQueue queue;
  int port = freeInspectorPort();
  auto runtime = queue.runtime(port);
  InspectorClient client;
  ASSERT_NE(connectInspector(port, &client).find("101 Switching"), std::string::npos);
  ASSERT_TRUE(queue.wait()); pumpInspector(*runtime, queue);
  ASSERT_TRUE(sendInspectorMessage(client.socket,
      R"json({"id":1,"method":"Runtime.evaluate","params":{"expression":"'x'.repeat(4*1024*1024)","returnByValue":true}})json"));
  ASSERT_TRUE(queue.wait()); pumpInspector(*runtime, queue);
  auto reply = readInspectorMessage(client.socket);
  auto value = reply.find(R"("value":")");
  ASSERT_NE(value, std::string::npos);
  value += sizeof("\"value\":\"") - 1;
  ASSERT_GE(reply.size(), value + 4 * 1024 * 1024 + 1);
  EXPECT_EQ(reply.substr(value, 4 * 1024 * 1024), std::string(4 * 1024 * 1024, 'x'));
  EXPECT_EQ(reply[value + 4 * 1024 * 1024], '"');
  EXPECT_EQ(runtimeTransportStats(port).overflows, 0u);
}
#endif

TEST(JsiAbiInspector, SamePortRestartAcceptsFirstDebugger) {
#ifdef _WIN32
  auto port = freeInspectorPort();
  // Keep reset + recreate on one thread to exercise immediate listener handoff.
  auto reload = std::async(std::launch::async, [port] {
    for (int i = 0; i < 10; ++i) {
      SCOPED_TRACE(i);
      DeferredHostQueue queue;
      auto runtime = queue.runtime(port);
      checkInspectorEvaluation(*runtime, queue, port);
      runtime.reset();
      EXPECT_EQ(queue.runnerDeleted, 1);
      queue.drain(true);
    }
  });
  waitForFuture(reload, 60, "Same-port runtime reload timed out");
  reload.get();
#endif
}

TEST(JsiAbiInspector, ConcurrentTargetReplacementKeepsListener) {
#ifdef _WIN32
  auto port = freeInspectorPort();
  DeferredHostQueue queue;
  auto runtime = queue.runtime(port);
  for (int i = 0; i < 10; ++i) { // Race the last removal against a new target on the same port.
    SCOPED_TRACE(i);
    std::promise<void> start;
    auto ready = start.get_future().share();
    auto old = std::move(runtime);
    auto removal = std::async(std::launch::async, [&] { ready.wait(); old.reset(); });
    start.set_value();
    runtime = queue.runtime(port);
    waitForFuture(removal, 10);
    removal.get();
    pumpInspector(*runtime, queue);
    checkInspectorEvaluation(*runtime, queue, port);
  }
  destroyInspectorRuntime(runtime);
  queue.drain(true);
  EXPECT_EQ(queue.runnerDeleted, 11);
#endif
}

#ifdef _WIN32
namespace {
// Owns A's JS thread, including creation (and hence V8's stack limit).
class InspectorRuntimeThread {
 public:
  InspectorRuntimeThread() : thread_([this] {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
        if (jobs_.empty()) return;
        job = std::move(jobs_.front()); jobs_.pop_front();
      }
      job();
    }
  }) {}
  template <class F> auto submit(F job) {
    auto task = std::make_shared<std::packaged_task<decltype(job())()>>(std::move(job));
    auto result = task->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      jobs_.push_back([task] { (*task)(); });
    }
    ready_.notify_one();
    return result;
  }
  template <class F> auto call(F job) {
    auto result = submit(std::move(job));
    waitForFuture(result, 5, "Inspector JS thread timed out");
    return result.get();
  }
  ~InspectorRuntimeThread() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    ready_.notify_one(); thread_.join();
  }
 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_{false};
  std::thread thread_;
};
struct InspectorTarget { std::string id, url, title; };
std::vector<InspectorTarget> inspectorTargets(int port) {
  auto listing = inspectorRequest(port, "GET /json/list HTTP/1.1\r\nHost: localhost:" + std::to_string(port) + "\r\n\r\n");
  std::vector<InspectorTarget> targets;
  size_t offset = listing.find("\r\n\r\n");
  while (offset != std::string::npos && (offset = listing.find('{', offset)) != std::string::npos) {
    auto object = listing.substr(offset, listing.find('}', offset) - offset);
    auto field = [&](const char *key) {
      auto start = object.find(std::string("\"") + key + "\": \"");
      if (start == std::string::npos) return std::string{};
      start = object.find(": \"", start) + 3;
      return object.substr(start, object.find('"', start) - start);
    };
    targets.push_back({field("id"), field("webSocketDebuggerUrl"), field("title")});
    ++offset;
  }
  return targets;
}
std::string inspectorUntil(SOCKET socket, const std::string &token) {
  for (int i = 0; i < 32; ++i) {
    auto message = readInspectorMessage(socket);
    if (message.empty() || message.find(token) != std::string::npos) return message;
  }
  return {};
}
void connectAndEnableDebugger(Runtime& runtime, DeferredHostQueue& queue,
    InspectorRuntimeThread& js, int port, InspectorClient& client, const std::string& target = {}) {
  auto response = target.empty() ? connectInspector(port, &client) : attachInspector(port, target, &client);
  ASSERT_NE(response.find("101 Switching"), std::string::npos) << response;
  ASSERT_TRUE(queue.wait()); js.call([&] { pumpInspector(runtime, queue); });
  ASSERT_TRUE(sendInspectorMessage(client.socket, R"({"id":10,"method":"Debugger.enable"})"));
  ASSERT_TRUE(queue.wait()); js.call([&] { pumpInspector(runtime, queue); });
  ASSERT_NE(inspectorUntil(client.socket, R"("id":10)").find("debuggerId"), std::string::npos);
}
void expectInspectorIdle(SOCKET socket) {
  fd_set readable; FD_ZERO(&readable); FD_SET(socket, &readable);
  timeval timeout{0, 50000};
  EXPECT_EQ(select(0, &readable, nullptr, nullptr, &timeout), 0) << "unexpected cross-target message";
}
void checkInspectorTargets(bool sharedPort, bool equalTitles) {
  SCOPED_TRACE(equalTitles ? "equal titles" : "distinct titles");
  DeferredHostQueue queueA, queueB;
  int portA = freeInspectorPort(), portB = sharedPort ? portA : freeInspectorPort();
  while (!sharedPort && portA == portB) portB = freeInspectorPort();
  InspectorRuntimeThread jsA;
  auto a = jsA.call([&] { return queueA.runtime(portA, "target"); });
  auto initial = inspectorTargets(portA);
  ASSERT_EQ(initial.size(), 1u);
  auto b = queueB.runtime(portB, equalTitles ? "target" : "other");
  auto targets = inspectorTargets(portA);
  ASSERT_EQ(targets.size(), sharedPort ? 2u : 1u);
  if (!sharedPort) {
    auto other = inspectorTargets(portB);
    ASSERT_EQ(other.size(), 1u);
    targets.push_back(other.front());
  }
  EXPECT_NE(targets[0].id, targets[1].id);
  EXPECT_NE(targets[0].url, targets[1].url);
  EXPECT_EQ(targets[0].title == targets[1].title, equalTitles);
  // Shared-port listing order is unspecified; retain A's pre-existing target ID.
  if (targets[0].id != initial[0].id) std::swap(targets[0], targets[1]);
  for (int i = 0; i < 2; ++i)
    EXPECT_EQ(targets[i].url, "ws://localhost:" + std::to_string(i ? portB : portA) + '/' + targets[i].id);
  InspectorClient clientA, clientB;
  for (int i = 0; i < 2; ++i) {
    auto &client = i ? clientB : clientA;
    ASSERT_NE(attachInspector(i ? portB : portA, targets[i].id, &client).find("101 Switching"), std::string::npos);
  }
  ASSERT_TRUE(queueA.wait()); ASSERT_TRUE(queueB.wait());
  jsA.call([&] { pumpInspector(*a, queueA); }); pumpInspector(*b, queueB);
  auto command = [&](Runtime &runtime, DeferredHostQueue &queue, InspectorClient &client,
                     const std::string &message, const std::string &token) {
    EXPECT_TRUE(sendInspectorMessage(client.socket, message));
    EXPECT_TRUE(queue.wait());
    if (&runtime == a.get()) jsA.call([&] { pumpInspector(runtime, queue); });
    else pumpInspector(runtime, queue);
    return inspectorUntil(client.socket, token);
  };
  // Per-runtime globals prove routing even when titles collide.
  jsA.call([&] { a->global().setProperty(*a, "identity", 111); });
  b->global().setProperty(*b, "identity", 222);
  EXPECT_NE(command(*a, queueA, clientA,
      R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"identity"}})", R"("id":1)").find(R"("value":111)"), std::string::npos);
  for (auto *client : {&clientA, &clientB}) {
    auto &runtime = client == &clientA ? a : b;
    auto &queue = client == &clientA ? queueA : queueB;
    EXPECT_NE(command(*runtime, queue, *client, R"({"id":2,"method":"Debugger.enable"})", R"("id":2)").find("debuggerId"), std::string::npos);
  }
  ASSERT_TRUE(sendInspectorMessage(clientA.socket,
      R"({"id":3,"method":"Runtime.evaluate","params":{"expression":"debugger; identity"}})"));
  ASSERT_TRUE(queueA.wait());
  // A runs the nested pause loop on its JS thread; the client and B must stay runnable.
  auto paused = jsA.submit([&] { pumpInspector(*a, queueA); });
  EXPECT_NE(inspectorUntil(clientA.socket, "Debugger.paused").find("Debugger.paused"), std::string::npos);
  EXPECT_NE(command(*b, queueB, clientB,
      R"({"id":4,"method":"Runtime.evaluate","params":{"expression":"identity"}})", R"("id":4)").find(R"("value":222)"), std::string::npos);
  expectInspectorIdle(clientB.socket);
  EXPECT_TRUE(sendInspectorMessage(clientA.socket, R"({"id":5,"method":"Debugger.resume"})"));
  auto resumed = inspectorUntil(clientA.socket, "Debugger.resumed");
  EXPECT_NE(resumed.find("Debugger.resumed"), std::string::npos) << resumed;
  waitForFuture(paused, 5, "Paused inspector did not resume");
  paused.get();
  EXPECT_NE(inspectorUntil(clientA.socket, R"("id":3)").find(R"("value":111)"), std::string::npos);
  expectInspectorIdle(clientB.socket);
  destroyInspectorRuntime(a); queueA.drain(true);
  char eof; EXPECT_EQ(recv(clientA.socket, &eof, 1, 0), 0);
  EXPECT_NE(command(*b, queueB, clientB,
      R"({"id":6,"method":"Runtime.evaluate","params":{"expression":"identity"}})", R"("id":6)").find(R"("value":222)"), std::string::npos);
  auto remaining = inspectorTargets(portB);
  ASSERT_EQ(remaining.size(), 1u); EXPECT_EQ(remaining[0].id, targets[1].id);
  closesocket(clientB.socket); clientB.socket = INVALID_SOCKET; WSACleanup();
  ASSERT_TRUE(queueB.wait()); pumpInspector(*b, queueB); // Process disconnect before reattaching.
  checkInspectorEvaluation(*b, queueB, portB);
  a = jsA.call([&] { return queueA.runtime(portA, "replacement"); });
  remaining = inspectorTargets(portA);
  ASSERT_EQ(remaining.size(), sharedPort ? 2u : 1u);
  auto replacement = std::find_if(remaining.begin(), remaining.end(), [&](const auto &t) { return t.id != targets[1].id; });
  ASSERT_NE(replacement, remaining.end()); EXPECT_NE(replacement->id, targets[0].id);
  InspectorClient reloaded;
  ASSERT_NE(attachInspector(portA, replacement->id, &reloaded).find("101 Switching"), std::string::npos);
  ASSERT_TRUE(queueA.wait()); jsA.call([&] { pumpInspector(*a, queueA); });
  EXPECT_NE(command(*a, queueA, reloaded,
      R"({"id":7,"method":"Runtime.evaluate","params":{"expression":"7*6"}})", R"("id":7)").find(R"("value":42)"), std::string::npos);
  destroyInspectorRuntime(a); destroyInspectorRuntime(b);
  queueA.drain(true); queueB.drain(true);
}
} // namespace
TEST(JsiAbiInspector, RemovingTargetClosesPendingConnectionsAndPreservesSharedPortSession) {
  DeferredHostQueue queueA, queueB;
  auto port = freeInspectorPort();
  auto a = queueA.runtime(port, "pending");
  auto initial = inspectorTargets(port);
  ASSERT_EQ(initial.size(), 1u);
  auto b = queueB.runtime(port, "survivor");
  auto targets = inspectorTargets(port);
  ASSERT_EQ(targets.size(), 2u);
  auto other = std::find_if(targets.begin(), targets.end(),
      [&](const auto &target) { return target.id != initial[0].id; });
  ASSERT_NE(other, targets.end());

  InspectorClient clientB;
  ASSERT_NE(attachInspector(port, other->id, &clientB).find("101 Switching"), std::string::npos);
  ASSERT_TRUE(queueB.wait());
  pumpInspector(*b, queueB);

  // Leave both CONNECT messages queued: no runtime entry may dispatch A's tasks.
  InspectorClient clientsA[2];
  for (auto &client : clientsA) {
    ASSERT_NE(attachInspector(port, initial[0].id, &client).find("101 Switching"), std::string::npos);
  }
  ASSERT_TRUE(queueA.wait());
  destroyInspectorRuntime(a);
  for (auto &client : clientsA) {
    // A receive timeout bounds the unfixed socket leak; drain any bytes until EOF.
    DWORD timeout = 2000;
    ASSERT_EQ(setsockopt(client.socket, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<char *>(&timeout), sizeof(timeout)), 0);
    char buffer[256];
    int received = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
      received = recv(client.socket, buffer, sizeof(buffer), 0);
    } while (received > 0 && std::chrono::steady_clock::now() < deadline);
    EXPECT_EQ(received, 0) << "removed target must close every debugger socket: " << WSAGetLastError();
  }
  queueA.drain(true); // Stale host callbacks must remain harmless after teardown.
  EXPECT_EQ(queueA.runnerDeleted, 1);

  ASSERT_TRUE(sendInspectorMessage(clientB.socket,
      R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"6 * 7","returnByValue":true}})"));
  ASSERT_TRUE(queueB.wait());
  pumpInspector(*b, queueB);
  auto result = inspectorUntil(clientB.socket, R"("id":1)");
  EXPECT_NE(result.find(R"("value":42)"), std::string::npos) << result;
  auto remaining = inspectorTargets(port);
  ASSERT_EQ(remaining.size(), 1u);
  EXPECT_EQ(remaining[0].id, other->id);
  destroyInspectorRuntime(b);
  queueB.drain(true);
}

TEST(JsiAbiInspector, BatchedCommandsReplyWhilePausedAndResume) {
  DeferredHostQueue queue;
  InspectorRuntimeThread js;
  auto port = freeInspectorPort();
  auto runtime = js.call([&] { return queue.runtime(port); });
  InspectorClient client;
  ASSERT_NO_FATAL_FAILURE(connectAndEnableDebugger(*runtime, queue, js, port, client));
  std::string batch = inspectorFrame(R"({"id":1,"method":"Runtime.evaluate","params":{"expression":"debugger; 111"}})");
  // Five commands expose work hidden in the outer dispatch batch during pause.
  for (int id = 2; id <= 6; ++id)
    batch += inspectorFrame("{\"id\":" + std::to_string(id) + R"(,"method":"Runtime.evaluate","params":{"expression":"222"}})");
  batch += inspectorFrame(R"({"id":7,"method":"Debugger.resume"})");
  ASSERT_EQ(send(client.socket, batch.data(), static_cast<int>(batch.size()), 0), batch.size());
  ASSERT_TRUE(queue.wait());
  auto dispatch = js.submit([&] { pumpInspector(*runtime, queue); });
  EXPECT_NE(inspectorUntil(client.socket, "Debugger.paused").find("Debugger.paused"), std::string::npos);
  for (int id = 2; id <= 6; ++id) {
    std::string reply;
    do {
      reply = readInspectorMessage(client.socket);
      EXPECT_EQ(reply.find("Debugger.resumed"), std::string::npos) << "reply must precede resume";
    } while (!reply.empty() && reply.find(R"("id":)") == std::string::npos);
    EXPECT_NE(reply.find("\"id\":" + std::to_string(id)), std::string::npos) << reply;
    EXPECT_NE(reply.find(R"("value":222)"), std::string::npos) << reply;
  }
  EXPECT_NE(inspectorUntil(client.socket, "Debugger.resumed").find("Debugger.resumed"), std::string::npos);
  waitForFuture(dispatch, 5);
  dispatch.get();
  EXPECT_NE(inspectorUntil(client.socket, R"("id":1,)").find(R"("value":111)"), std::string::npos);
  destroyInspectorRuntime(runtime); queue.drain(true);
}

TEST(JsiAbiInspector, DisconnectDuringBatchedPauseDoesNotReachReplacementSession) {
  DeferredHostQueue queue;
  InspectorRuntimeThread js;
  auto port = freeInspectorPort();
  auto runtime = js.call([&] { return queue.runtime(port); });
  InspectorClient oldClient, replacement;
  ASSERT_NO_FATAL_FAILURE(connectAndEnableDebugger(*runtime, queue, js, port, oldClient));
  auto batch = inspectorFrame(R"({"id":101,"method":"Runtime.evaluate","params":{"expression":"debugger; 111"}})");
  batch += inspectorFrame(R"({"id":102,"method":"Runtime.evaluate","params":{"expression":"222"}})");
  ASSERT_EQ(send(oldClient.socket, batch.data(), static_cast<int>(batch.size()), 0), batch.size());
  ASSERT_TRUE(queue.wait());
  auto dispatch = js.submit([&] { pumpInspector(*runtime, queue); });
  EXPECT_NE(inspectorUntil(oldClient.socket, "Debugger.paused").find("Debugger.paused"), std::string::npos);
  closesocket(oldClient.socket); oldClient.socket = INVALID_SOCKET; WSACleanup();
  // EOF must finish the old dispatch before reattachment, rather than race a
  // second frontend against a still-connected target.
  waitForFuture(dispatch, 5);
  dispatch.get();
  ASSERT_NE(connectInspector(port, &replacement).find("101 Switching"), std::string::npos);
  ASSERT_TRUE(queue.wait()); js.call([&] { pumpInspector(*runtime, queue); });
  ASSERT_TRUE(sendInspectorMessage(replacement.socket,
      R"({"id":201,"method":"Runtime.evaluate","params":{"expression":"333"}})"));
  ASSERT_TRUE(queue.wait()); js.call([&] { pumpInspector(*runtime, queue); });
  // The replacement must not receive the old evaluate's response or commands.
  auto reply = readInspectorMessage(replacement.socket);
  EXPECT_NE(reply.find(R"("id":201)"), std::string::npos) << reply;
  EXPECT_NE(reply.find(R"("value":333)"), std::string::npos) << reply;
  expectInspectorIdle(replacement.socket);
  destroyInspectorRuntime(runtime); queue.drain(true);
}

TEST(JsiAbiInspector, DisconnectDuringEvaluateDisablesLaterPauses) {
  DeferredHostQueue queue;
  InspectorRuntimeThread js;
  auto port = freeInspectorPort();
  auto runtime = js.call([&] { return queue.runtime(port); });
  InspectorClient client;
  ASSERT_NO_FATAL_FAILURE(connectAndEnableDebugger(*runtime, queue, js, port, client));
  ASSERT_TRUE(sendInspectorMessage(client.socket,
      R"({"id":101,"method":"Runtime.evaluate","params":{"expression":"debugger;finished=21;debugger;finished*2"}})"));
  ASSERT_TRUE(queue.wait());
  auto dispatch = js.submit([&] { pumpInspector(*runtime, queue); });
  EXPECT_NE(inspectorUntil(client.socket, "Debugger.paused").find("Debugger.paused"), std::string::npos);
  closesocket(client.socket); client.socket = INVALID_SOCKET; WSACleanup();
  waitForFuture(dispatch, 5, "Disconnected evaluation paused again without a frontend");
  dispatch.get();
  js.call([&] {
    EXPECT_EQ(runtime->evaluateJavaScript(
        std::make_shared<StringBuffer>("finished * 2"), "after-disconnect.js").getNumber(), 42);
  });
  js.call([&] { checkInspectorEvaluation(*runtime, queue, port); });
  destroyInspectorRuntime(runtime); queue.drain(true);
}

TEST(JsiAbiInspector, ReconnectDuringEvaluatePauseKeepsInFlightSessionAlive) {
  DeferredHostQueue queue;
  InspectorRuntimeThread js;
  auto port = freeInspectorPort();
  auto runtime = js.call([&] { return queue.runtime(port); });
  std::promise<void> entered, release;
  auto enteredFuture = entered.get_future(), releaseFuture = release.get_future();
  js.call([&] {
    auto hold = Function::createFromHostFunction(*runtime, PropNameID::forAscii(*runtime, "hold"), 0,
        [&](Runtime&, const Value&, const Value*, size_t) -> Value {
          queue.drain(false);
          entered.set_value();
          waitForFuture(releaseFuture, 5, "Reconnect did not release paused dispatch");
          return Value::undefined();
        });
    runtime->global().setProperty(*runtime, "hold", hold);
  });
  InspectorClient oldClient, replacement;
  ASSERT_NO_FATAL_FAILURE(connectAndEnableDebugger(*runtime, queue, js, port, oldClient));
  ASSERT_TRUE(sendInspectorMessage(oldClient.socket,
      R"({"id":101,"method":"Runtime.evaluate","params":{"expression":"finished=false;debugger;finished=true;42"}})"));
  ASSERT_TRUE(queue.wait());
  auto dispatch = js.submit([&] { pumpInspector(*runtime, queue); });
  EXPECT_NE(inspectorUntil(oldClient.socket, "Debugger.paused").find("Debugger.paused"), std::string::npos);
  EXPECT_TRUE(sendInspectorMessage(oldClient.socket,
      R"CDP({"id":102,"method":"Runtime.evaluate","params":{"expression":"hold()"}})CDP"));
  waitForFuture(enteredFuture, 5);
  // Hold a nested command so EOF and the replacement CONNECT reach the same pause.
  closesocket(oldClient.socket); oldClient.socket = INVALID_SOCKET; WSACleanup();
  EXPECT_TRUE(queue.wait());
  auto response = connectInspector(port, &replacement);
  EXPECT_NE(response.find("101 Switching"), std::string::npos) << response;
  EXPECT_TRUE(sendInspectorMessage(replacement.socket,
      R"({"id":201,"method":"Runtime.evaluate","params":{"expression":"333"}})"));
  release.set_value();
  waitForFuture(dispatch, 5);
  dispatch.get();
  // The replacement command may arrive before or after the old evaluation resumes.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool readable = false;
  do {
    js.call([&] { pumpInspector(*runtime, queue); });
    fd_set readers; FD_ZERO(&readers); FD_SET(replacement.socket, &readers);
    timeval timeout{0, 1000};
    readable = select(0, &readers, nullptr, nullptr, &timeout) == 1;
  } while (!readable && std::chrono::steady_clock::now() < deadline);
  ASSERT_TRUE(readable) << "Replacement evaluation did not reply before deadline";
  auto reply = readInspectorMessage(replacement.socket);
  EXPECT_NE(reply.find(R"("id":201,)"), std::string::npos) << reply;
  EXPECT_NE(reply.find(R"("value":333)"), std::string::npos) << reply;
  expectInspectorIdle(replacement.socket);
  EXPECT_TRUE(sendInspectorMessage(replacement.socket,
      R"({"id":202,"method":"Runtime.evaluate","params":{"expression":"40 + 2"}})"));
  EXPECT_TRUE(queue.wait()); js.call([&] { pumpInspector(*runtime, queue); });
  reply = readInspectorMessage(replacement.socket);
  EXPECT_NE(reply.find(R"("id":202,)"), std::string::npos) << reply;
  EXPECT_NE(reply.find(R"("value":42)"), std::string::npos) << reply;
  expectInspectorIdle(replacement.socket);
  destroyInspectorRuntime(runtime); queue.drain(true);
}

TEST(JsiAbiInspector, DestroyWaitsForNonPausedJavaScriptToComplete) {
  DeferredHostQueue queue;
  InspectorRuntimeThread js;
  auto runtime = js.call([&] { return queue.runtime(freeInspectorPort()); });
  std::promise<void> started;
  std::atomic<int> result{0};
  js.call([&] {
    runtime->global().setProperty(*runtime, "started", Function::createFromHostFunction(*runtime,
        PropNameID::forAscii(*runtime, "started"), 0, [&](Runtime &, const Value &, const Value *, size_t) {
          started.set_value(); return Value::undefined();
        }));
  });
  auto *target = runtime.get();
  std::pair<Runtime *, std::atomic<int> *> work{target, &result};
  js.call([&] {
    v8_jsi_test_post_foreground_task(::jsi::abi::getAbiRuntime(*target), [](void *p) {
      auto &work = *static_cast<std::pair<Runtime *, std::atomic<int> *> *>(p);
      try {
        auto value = work.first->evaluateJavaScript(std::make_shared<StringBuffer>(
            "started(); var until = Date.now() + 250; while (Date.now() < until) {} 42"), "busy.js");
        *work.second = static_cast<int>(value.getNumber());
      } catch (const JSIException &) { ADD_FAILURE() << "non-paused JS must not be terminated"; }
    }, &work);
  });
  auto task = queue.take(); ASSERT_NE(task, nullptr);
  auto running = js.submit([&] { task->run(task->data); });
  waitForFuture(started.get_future(), 5);
  auto teardown = std::async(std::launch::async, [&] {
    runtime.reset();
    EXPECT_EQ(result.load(), 42) << "destroy must wait for normal JS completion";
  });
  waitForFuture(teardown, 10);
  teardown.get(); running.get(); task.reset();
  EXPECT_EQ(queue.runnerDeleted, 1); EXPECT_EQ(queue.deleted, 1);
  queue.drain(true);
}

TEST(JsiAbiInspector, DestroyCancelsPausedHostTaskWithoutAffectingSurvivor) {
  DeferredHostQueue queue, survivorQueue;
  InspectorRuntimeThread js;
  auto port = freeInspectorPort();
  auto runtime = js.call([&] { return queue.runtime(port); });
  auto targets = inspectorTargets(port);
  ASSERT_EQ(targets.size(), 1u);
  auto survivor = survivorQueue.runtime(port);
  InspectorClient client;
  ASSERT_NO_FATAL_FAILURE(connectAndEnableDebugger(*runtime, queue, js, port, client, targets[0].id));
  int continued = 0, finished = 0;
  js.call([&] {
    runtime->global().setProperty(*runtime, "continued", Function::createFromHostFunction(*runtime,
        PropNameID::forAscii(*runtime, "continued"), 0, [&](Runtime &, const Value &, const Value *, size_t) {
          ++continued; return Value::undefined();
        }));
  });
  std::pair<Runtime *, int *> work{runtime.get(), &finished};
  js.call([&] {
    v8_jsi_test_post_foreground_task(::jsi::abi::getAbiRuntime(*runtime), [](void *p) {
      auto &work = *static_cast<std::pair<Runtime *, int *> *>(p);
      try { work.first->evaluateJavaScript(std::make_shared<StringBuffer>("debugger; continued()"), "pause.js"); }
      catch (const JSIException &) {} // Cancellation must unwind before teardown returns.
      ++*work.second;
    }, &work);
  });
  auto task = queue.take(); ASSERT_NE(task, nullptr);
  auto deleted = queue.deleted.load();
  auto running = js.submit([&] { task->run(task->data); task.reset(); });
  EXPECT_NE(inspectorUntil(client.socket, "Debugger.paused").find("Debugger.paused"), std::string::npos);
  auto teardown = std::async(std::launch::async, [&] { runtime.reset(); });
  waitForFuture(teardown, 10, "Paused runtime destruction timed out");
  teardown.get();
  running.get();
  EXPECT_EQ(continued, 0); EXPECT_EQ(finished, 1);
  EXPECT_EQ(queue.deleted, deleted + 1); EXPECT_EQ(queue.runnerDeleted, 1);
  char buffer[256]; int received;
  do { received = recv(client.socket, buffer, sizeof(buffer), 0); } while (received > 0);
  EXPECT_EQ(received, 0) << "debugger must see EOF";
  queue.drain(true); expectSurvivor(*survivor, survivorQueue);
  js.call([&] { auto later = queue.runtime(); expectSurvivor(*later, queue); });
}

// Title collisions must not affect routing with either listener topology.
TEST(JsiAbiInspector, SharedPortSessionsStayIsolatedAcrossPauseDestroyAndReload) {
  for (bool equalTitles : {false, true}) checkInspectorTargets(true, equalTitles);
}
TEST(JsiAbiInspector, SeparatePortSessionsStayIsolatedAcrossPauseDestroyAndReload) {
  for (bool equalTitles : {false, true}) checkInspectorTargets(false, equalTitles);
}
#endif

TEST(JsiAbiInspector, TeardownCancelsSendToNonReadingDebugger) {
#ifdef _WIN32
  DeferredHostQueue queue;
  auto port = freeInspectorPort();
  auto runtime = queue.runtime(port);
  InspectorClient client;
  auto response = connectInspector(port, &client);
  ASSERT_NE(response.find("101 Switching"), std::string::npos) << response;
  ASSERT_TRUE(queue.wait());
  pumpInspector(*runtime, queue);
  int receiveBuffer = 1024;
  setsockopt(client.socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char *>(&receiveBuffer), sizeof(receiveBuffer));
  // Multiple large replies exceed both socket buffers even if async_send completes a partial write.
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(sendInspectorMessage(client.socket,
        R"json({"id":1,"method":"Runtime.evaluate","params":{"expression":"'x'.repeat(4*1024*1024)","returnByValue":true}})json"));
    ASSERT_TRUE(queue.wait());
    pumpInspector(*runtime, queue);
  }
  fd_set readable; FD_ZERO(&readable); FD_SET(client.socket, &readable);
  timeval timeout{5, 0};
  ASSERT_EQ(select(0, &readable, nullptr, nullptr, &timeout), 1) << "reply must begin before teardown";
  // Deliberately never read a reply: teardown must cancel the pending send.
  destroyInspectorRuntime(runtime);
  EXPECT_EQ(queue.runnerDeleted, 1);
  queue.drain(true);
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
// compiled into v8jsi2.dll works end-to-end:
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

// A late flag change must fail; isolate a regression that could crash V8.
TEST(JsiAbiFlagsDeathTest, SetV8FlagsFailsAfterInit) {
  EXPECT_EXIT(
      {
        auto runtime = ::jsi::abi::makeJsiAbiRuntime(&v8_jsi_abi_runtime_create);
        char program[] = "v8jsi";
        char jitless[] = "--jitless";
        char *argv[2] = {program};
        argv[1] = jitless;
        size_t argc = 2;
        auto status = v8_jsi_abi_set_v8_flags(&argc, argv, false);
        runtime.reset();
        std::_Exit(status == jsi_status_invalid_arg && argc == 2 ? 0 : 1);
      },
      ::testing::ExitedWithCode(0),
      "");
}

namespace {

#ifdef V8JSI_TEST_WEBASSEMBLY
// Jitless V8 leaves WebAssembly off the global object.
bool exposesWebAssembly(v8runtime::V8RuntimeArgs args) {
  std::unique_ptr<facebook::jsi::Runtime> runtime =
      v8runtime::makeV8Runtime(std::move(args));
  facebook::jsi::Value type = runtime->evaluateJavaScript(
      std::make_shared<facebook::jsi::StringBuffer>("typeof WebAssembly"), "flags.js");
  return type.asString(*runtime).utf8(*runtime) == "object";
}
#endif

} // namespace

// makeV8Runtime must apply a lone flag, although V8 does not parse argv[0].
// V8 flags are process-global and fixed by the first runtime, so each check
// runs in a child process and this test never initializes V8 here.
TEST(V8RuntimeFlagsDeathTest, LoneFlagIsApplied) {
#ifndef V8JSI_TEST_WEBASSEMBLY
  GTEST_SKIP() << "The check needs WebAssembly, which this engine leaves out";
#else
  EXPECT_EXIT(
      std::_Exit(exposesWebAssembly(v8runtime::V8RuntimeArgs{}) ? 0 : 1),
      ::testing::ExitedWithCode(0),
      "");
  v8runtime::V8RuntimeArgs jitlessArgs;
  jitlessArgs.flags.jitless = true;
  EXPECT_EXIT(
      std::_Exit(exposesWebAssembly(std::move(jitlessArgs)) ? 1 : 0),
      ::testing::ExitedWithCode(0),
      "");
#endif
}

// Snapshot creation precedes runtime creation; main may already have applied
// V8JSI_TEST_V8_FLAGS. CetStartupFlags separately covers no-prior-flag startup.
TEST(JsiAbiFlagsDeathTest, SnapshotCanInitializeBeforeRuntime) {
  EXPECT_EXIT(
      {
        uint8_t* blob = nullptr;
        size_t size = 0;
        auto created = v8_jsi_abi_snapshot_create("", 0, "empty.js", false, &blob, &size);
        v8_jsi_abi_snapshot_destroy(blob);
        auto runtime = ::jsi::abi::makeJsiAbiRuntime(&v8_jsi_abi_runtime_create);
        char program[] = "v8jsi";
        char flag[] = "--no-cet-compatible";
        char* argv[2] = {program};
        argv[1] = flag;
        size_t argc = 2;
        auto late = v8_jsi_abi_set_v8_flags(&argc, argv, false);
        runtime.reset();
        std::_Exit(created == jsi_status_ok && size > 0 &&
                           late == jsi_status_invalid_arg && argc == 2 ? 0 : 1);
      },
      ::testing::ExitedWithCode(0),
      "");
}

// Disposal is process-global and irreversible, so isolate it from other tests.
// V8::Dispose forbids reinitialization: only late flag rejection is supported,
// not runtime creation after platform disposal.
TEST(JsiAbiFlagsDeathTest, SetV8FlagsFailsAfterDispose) {
  EXPECT_EXIT(
      {
        {
          auto runtime = ::jsi::abi::makeJsiAbiRuntime(&v8_jsi_abi_runtime_create);
        }
        auto disposed = v8_node_api_dispose_platform();
        char program[] = "v8jsi";
        char jitless[] = "--jitless";
        char *argv[2] = {program};
        argv[1] = jitless;
        size_t argc = 2;
        auto status = v8_jsi_abi_set_v8_flags(&argc, argv, true);
        std::_Exit(disposed == napi_ok && status == jsi_status_invalid_arg &&
                           argc == 2 && argv[1] == jitless
                       ? 0 : 1);
      },
      ::testing::ExitedWithCode(0),
      "");
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

namespace {
std::vector<uint8_t> emptySnapshotHeader() {
  uint8_t *blob = nullptr;
  size_t size = 0;
  EXPECT_EQ(v8_jsi_abi_snapshot_create("", 0, "header.js", false, &blob, &size), jsi_status_ok);
  std::unique_ptr<uint8_t, decltype(&v8_jsi_abi_snapshot_destroy)> owner(blob, v8_jsi_abi_snapshot_destroy);
  if (!blob || size < 24) return {};
  std::vector<uint8_t> header(blob, blob + 24);
  std::fill(header.begin() + 16, header.end(), 0); // Declare an empty payload.
  return header;
}
} // namespace

TEST(JsiAbiSnapshot, HeaderFieldsAndBoundaries) {
  auto header = emptySnapshotHeader();
  ASSERT_EQ(header.size(), 24u);
  EXPECT_EQ(v8_jsi_abi_snapshot_check(nullptr, header.size()), v8_jsi_abi_snapshot_compat_truncated);
  struct Case { size_t size; int changedByte; v8_jsi_abi_snapshot_compat code; };
  // Isolate each header field and the boundaries without passing payload bytes to V8.
  const Case cases[] = {
      {0, -1, v8_jsi_abi_snapshot_compat_truncated},
      {23, -1, v8_jsi_abi_snapshot_compat_truncated},
      {24, -1, v8_jsi_abi_snapshot_compat_ok},
      {25, -1, v8_jsi_abi_snapshot_compat_size_mismatch},
      {24, 0, v8_jsi_abi_snapshot_compat_bad_magic},
      {24, 4, v8_jsi_abi_snapshot_compat_bad_format},
      {24, 8, v8_jsi_abi_snapshot_compat_version_mismatch},
      {24, 12, v8_jsi_abi_snapshot_compat_flags_mismatch},
      {24, 16, v8_jsi_abi_snapshot_compat_size_mismatch},
  };
  for (const auto &test : cases) {
    SCOPED_TRACE(::testing::Message() << "changedByte=" << test.changedByte << ", size=" << test.size);
    auto bytes = header;
    bytes.resize(25);
    if (test.changedByte >= 0) bytes[test.changedByte] ^= 1;
    EXPECT_EQ(v8_jsi_abi_snapshot_check(bytes.data(), test.size), test.code);
  }
}

TEST(JsiAbiSnapshot, OversizedPayloadFallsBackAndDeletesOnce) {
#if !defined(_WIN32) || !(defined(_M_X64) || defined(_M_ARM64))
  GTEST_SKIP() << "Reserving an INT_MAX+1 payload requires Windows x64 or ARM64";
#else
  auto header = emptySnapshotHeader();
  ASSERT_EQ(header.size(), 24u);
  const size_t size = header.size() + static_cast<size_t>(INT_MAX) + 1;
  auto bytes = static_cast<uint8_t *>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS));
  auto release = [](uint8_t *p) { if (p) VirtualFree(p, 0, MEM_RELEASE); };
  std::unique_ptr<uint8_t, decltype(release)> reservation(bytes, release);
  ASSERT_NE(bytes, nullptr);
  ASSERT_EQ(VirtualAlloc(bytes, header.size(), MEM_COMMIT, PAGE_READWRITE), bytes);
  std::memcpy(bytes, header.data(), header.size());
  uint64_t payload = INT_MAX;
  std::memcpy(bytes + 16, &payload, sizeof(payload));
  EXPECT_EQ(v8_jsi_abi_snapshot_check(bytes, size - 1), v8_jsi_abi_snapshot_compat_ok);
  ++payload;
  std::memcpy(bytes + 16, &payload, sizeof(payload));
  ASSERT_EQ(v8_jsi_abi_snapshot_check(bytes, size), v8_jsi_abi_snapshot_compat_size_mismatch);
  struct Blob { uint8_t *bytes; size_t size; int deleted = 0; } blob{bytes, size};
  {
    auto runtime = ::jsi::abi::makeJsiAbiRuntime(&v8_jsi_abi_runtime_create,
        [](void *data, jsi_config cfg) -> jsi_status {
          auto *b = static_cast<Blob *>(data);
          return v8_jsi_abi_config_set_startup_snapshot(cfg, b->bytes, b->size,
              [](void *p, void *bytes) {
                auto *b = static_cast<Blob *>(p);
                EXPECT_EQ(bytes, b->bytes); ++b->deleted;
              }, b);
        }, &blob);
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->evaluateJavaScript(std::make_shared<StringBuffer>("6 * 7"), "fallback.js").getNumber(), 42);
    EXPECT_EQ(blob.deleted, 0);
  }
  EXPECT_EQ(blob.deleted, 1);
#endif
}

namespace {
// Five supported paths: JSI auto/explicit, attached auto/explicit, standalone
// auto. Standalone hosting has no public explicit-policy or rejection setter.
enum class MicrotaskMode { JsiAuto, JsiExplicit, AttachedAuto, AttachedExplicit, StandaloneAuto };
constexpr MicrotaskMode kMicrotaskModes[] = {
    MicrotaskMode::JsiAuto, MicrotaskMode::JsiExplicit, MicrotaskMode::AttachedAuto,
    MicrotaskMode::AttachedExplicit, MicrotaskMode::StandaloneAuto};

struct MicrotaskHost {
  MicrotaskMode mode;
  bool explicitPolicy;
  DeferredHostQueue queue;
  std::shared_ptr<facebook::jsi::Runtime> runtime;
  v8_node_api_runtime standalone{};
  napi_env env{};
  v8_node_api_env_scope scope{};
  napi_handle_scope handles{};

  MicrotaskHost(MicrotaskMode path, bool ignore = false)
      : mode(path), explicitPolicy(path == MicrotaskMode::JsiExplicit || path == MicrotaskMode::AttachedExplicit) {
    if (mode == MicrotaskMode::StandaloneAuto) {
      EXPECT_EQ(v8_node_api_runtime_create(V8_NODE_API_ABI_VERSION,
          [](void* data, v8_node_api_config cfg) -> napi_status {
            v8_node_api_config_set_gc_api_enabled(cfg, true);
            return v8_node_api_config_set_task_runner(cfg, DeferredHostQueue::Post, data, nullptr, nullptr);
          }, &queue, &standalone), napi_ok);
      EXPECT_EQ(v8_node_api_runtime_get_env(standalone, &env), napi_ok);
    } else {
      struct Options { bool explicitPolicy; bool ignore; DeferredHostQueue* queue; } options{explicitPolicy, ignore, &queue};
      runtime = ::jsi::abi::makeJsiAbiRuntime(&v8_jsi_abi_runtime_create,
          [](void* data, jsi_config cfg) -> jsi_status {
            auto& options = *static_cast<Options*>(data);
            v8_jsi_abi_config_set_gc_api_enabled(cfg, true);
            v8_jsi_abi_config_set_ignore_unhandled_promises(cfg, options.ignore);
            v8_jsi_abi_config_set_task_runner(cfg, DeferredHostQueue::Post, options.queue, nullptr, nullptr);
            return v8_jsi_abi_config_set_explicit_microtask_policy(cfg, options.explicitPolicy);
          }, &options);
      EXPECT_EQ(v8_attach_node_api(::jsi::abi::getAbiRuntime(*runtime), NAPI_VERSION, &env), napi_ok);
    }
    EXPECT_EQ(v8_node_api_env_scope_open(env, &scope), napi_ok);
    EXPECT_EQ(napi_open_handle_scope(env, &handles), napi_ok);
  }
  ~MicrotaskHost() {
    if (handles) EXPECT_EQ(napi_close_handle_scope(env, handles), napi_ok);
    if (scope) EXPECT_EQ(v8_node_api_env_scope_close(scope, env), napi_ok);
    if (standalone) EXPECT_EQ(v8_node_api_runtime_destroy(standalone), napi_ok);
  }
  napi_value script(const char* text) {
    napi_value source{}, result{};
    EXPECT_EQ(napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &source), napi_ok);
    EXPECT_EQ(napi_run_script(env, source, &result), napi_ok);
    return result;
  }
  bool usesJsi() const { return mode == MicrotaskMode::JsiAuto || mode == MicrotaskMode::JsiExplicit; }
  void evaluate(const char* text) {
    if (usesJsi()) runtime->evaluateJavaScript(std::make_shared<facebook::jsi::StringBuffer>(text), "policy.js");
    else script(text);
  }
  int count() {
    napi_value global{}, value{};
    int32_t result = -1;
    EXPECT_EQ(napi_get_global(env, &global), napi_ok);
    EXPECT_EQ(napi_get_named_property(env, global, "count", &value), napi_ok);
    EXPECT_EQ(napi_get_value_int32(env, value, &result), napi_ok);
    return result;
  }
  void drain(int hint = -1) {
    bool drained = false;
    if (usesJsi()) drained = runtime->drainMicrotasks(hint);
    else EXPECT_EQ(v8_node_api_env_drain_microtasks(env, hint, &drained), napi_ok);
    EXPECT_TRUE(drained);
  }
  bool rejected() {
    bool rejected = false;
    EXPECT_EQ(v8_node_api_env_has_unhandled_promise_rejection(env, &rejected), napi_ok);
    return rejected;
  }
};
} // namespace

TEST(JsiAbiMicrotaskPolicy, PromiseReactionsFollowPolicyAndDrainOnce) {
  for (auto mode : kMicrotaskModes) {
    SCOPED_TRACE(static_cast<int>(mode));
    MicrotaskHost host(mode);
    // Raw host-queued callbacks must drain too, under both JSI policies.
    if (host.usesJsi()) {
      host.drain();
      host.evaluate("globalThis.count = 0;");
      auto& rt = *host.runtime;
      auto callback = rt.evaluateJavaScript(std::make_shared<facebook::jsi::StringBuffer>(
          "(function () { count++; })"), "microtask.js").asObject(rt).asFunction(rt);
      rt.queueMicrotask(callback);
      host.drain();
      EXPECT_EQ(host.count(), 1);
      host.drain();
      EXPECT_EQ(host.count(), 1);
    }
    host.evaluate("globalThis.count = 0; Promise.resolve().then(() => ++count);");
    EXPECT_EQ(host.count(), host.explicitPolicy ? 0 : 1);
    host.drain(0); // V8 ignores the count hint, including zero.
    EXPECT_EQ(host.count(), 1);
    host.drain(1);
    EXPECT_EQ(host.count(), 1);
    // Calling a resolver from a later host turn does not itself pump explicit queues.
    host.evaluate("globalThis.resolveTask = null; new Promise(r => resolveTask = r).then(() => ++count);");
    jsi_runtime* abi{};
    if (host.standalone) ASSERT_EQ(v8_node_api_runtime_get_jsi_runtime(host.standalone, &abi), napi_ok);
    else abi = ::jsi::abi::getAbiRuntime(*host.runtime);
    v8_jsi_test_post_foreground_task(abi, [](void* data) {
      auto& host = *static_cast<MicrotaskHost*>(data);
      if (host.usesJsi()) {
        host.runtime->global().getPropertyAsFunction(*host.runtime, "resolveTask").call(*host.runtime);
        return;
      }
      napi_value global{}, resolve{}, result{};
      EXPECT_EQ(napi_get_global(host.env, &global), napi_ok);
      EXPECT_EQ(napi_get_named_property(host.env, global, "resolveTask", &resolve), napi_ok);
      EXPECT_EQ(napi_call_function(host.env, global, resolve, 0, nullptr, &result), napi_ok);
    }, &host);
    EXPECT_EQ(host.count(), 1);
    host.queue.drain(true);
    EXPECT_EQ(host.count(), host.explicitPolicy ? 1 : 2);
    host.drain();
    EXPECT_EQ(host.count(), 2);
  }
}

TEST(JsiAbiMicrotaskPolicy, AutoPolicyDrainInsideHostFunctionRunsPendingReactionsImmediately) {
  MicrotaskHost host(MicrotaskMode::JsiAuto);
  auto callback = facebook::jsi::Function::createFromHostFunction(*host.runtime,
      facebook::jsi::PropNameID::forAscii(*host.runtime, "drain"), 0,
      [&host](facebook::jsi::Runtime&, const facebook::jsi::Value&,
              const facebook::jsi::Value*, size_t) -> facebook::jsi::Value {
        EXPECT_EQ(host.count(), 0);
        host.drain();
        EXPECT_EQ(host.count(), 1);
        return facebook::jsi::Value::undefined();
      });
  host.runtime->global().setProperty(*host.runtime, "drain", callback);
  host.evaluate("globalThis.count = 0; Promise.resolve().then(() => ++count); drain(); globalThis.afterDrain = count;");
  EXPECT_EQ(host.runtime->global().getProperty(*host.runtime, "afterDrain").getNumber(), 1);
  EXPECT_EQ(host.count(), 1);
}

TEST(JsiAbiMicrotaskPolicy, RejectionsAreTrackedUnlessIgnoredAndLateCatchClearsThem) {
  for (auto mode : kMicrotaskModes) {
    for (bool ignore : {false, true}) { // Exercise tracking and opt-out on each configurable surface.
      if (mode == MicrotaskMode::StandaloneAuto && ignore) continue; // Standalone hosting exposes no opt-out setter.
      SCOPED_TRACE(static_cast<int>(mode));
      SCOPED_TRACE(ignore);
      MicrotaskHost host(mode, ignore);
      host.evaluate("globalThis.count = 0; globalThis.rejected = Promise.resolve().then(() => { throw 42; });");
      EXPECT_EQ(host.rejected(), !host.explicitPolicy && !ignore);
      host.drain();
      EXPECT_EQ(host.rejected(), !ignore);
      host.evaluate("rejected.catch(() => ++count);");
      EXPECT_FALSE(host.rejected());
      host.drain();
      EXPECT_EQ(host.count(), 1);
      host.evaluate("Promise.reject(17);");
      EXPECT_EQ(host.rejected(), !ignore);
      napi_value rejection{};
      ASSERT_EQ(v8_node_api_env_get_and_clear_last_unhandled_promise_rejection(host.env, &rejection), napi_ok);
      EXPECT_FALSE(host.rejected());
    }
  }
}

TEST(JsiAbiMicrotaskPolicy, NestedDrainDefersToOuterCheckpoint) {
  for (auto mode : kMicrotaskModes) {
    SCOPED_TRACE(static_cast<int>(mode));
    MicrotaskHost host(mode);
    napi_value callback{}, global{};
    ASSERT_EQ(napi_create_function(host.env, "drain", NAPI_AUTO_LENGTH,
        [](napi_env env, napi_callback_info info) -> napi_value {
          void* data{};
          EXPECT_EQ(napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data), napi_ok);
          auto& host = *static_cast<MicrotaskHost*>(data);
          host.drain();
          EXPECT_EQ(host.count(), 0); // The next reaction is still owned by the outer checkpoint.
          return nullptr;
        }, &host, &callback), napi_ok);
    ASSERT_EQ(napi_get_global(host.env, &global), napi_ok);
    ASSERT_EQ(napi_set_named_property(host.env, global, "nestedDrain", callback), napi_ok);
    host.evaluate("globalThis.count = 0; Promise.resolve().then(nestedDrain); Promise.resolve().then(() => ++count);");
    host.drain();
    EXPECT_EQ(host.count(), 1);
  }
}

TEST(JsiAbiMicrotaskPolicy, NodeApiCheckpointPreservesRawMicrotaskException) {
  for (auto mode : {MicrotaskMode::AttachedAuto, MicrotaskMode::AttachedExplicit, MicrotaskMode::StandaloneAuto}) { // Raw callbacks must propagate on both supported policies and hosting paths.
    SCOPED_TRACE(static_cast<int>(mode));
    MicrotaskHost host(mode);
    napi_value nested{}, global{};
    ASSERT_EQ(napi_create_function(host.env, "drain", NAPI_AUTO_LENGTH,
        [](napi_env env, napi_callback_info) -> napi_value {
          bool drained = false;
          EXPECT_EQ(v8_node_api_env_drain_microtasks(env, -1, &drained), napi_ok);
          EXPECT_TRUE(drained);
          return nullptr;
        }, nullptr, &nested), napi_ok);
    ASSERT_EQ(napi_get_global(host.env, &global), napi_ok);
    ASSERT_EQ(napi_set_named_property(host.env, global, "nestedDrain", nested), napi_ok);
    napi_value callback = host.script("(() => { nestedDrain(); throw 42; })");
    ASSERT_EQ(v8_node_api_env_queue_microtask(host.env, callback), napi_ok);
    bool drained = true;
    EXPECT_EQ(v8_node_api_env_drain_microtasks(host.env, -1, &drained), napi_pending_exception);
    EXPECT_FALSE(drained);
    const napi_extended_error_info* error{};
    ASSERT_EQ(napi_get_last_error_info(host.env, &error), napi_ok);
    EXPECT_EQ(error->error_code, napi_pending_exception);
    bool pending = false;
    ASSERT_EQ(napi_is_exception_pending(host.env, &pending), napi_ok);
    EXPECT_TRUE(pending);
    EXPECT_EQ(v8_node_api_env_drain_microtasks(host.env, -1, &drained), napi_pending_exception);
    napi_value exception{};
    ASSERT_EQ(napi_get_and_clear_last_exception(host.env, &exception), napi_ok);
    int32_t value{};
    ASSERT_EQ(napi_get_value_int32(host.env, exception, &value), napi_ok);
    EXPECT_EQ(value, 42);
    host.drain();
    EXPECT_EQ(v8_node_api_env_drain_microtasks(host.env, -1, nullptr), napi_invalid_arg);
  }
}

TEST(JsiAbiMicrotaskPolicy, RawExceptionSurvivesNodeApiPromiseReaction) {
  // Both attached policies and standalone hosting must isolate drain exceptions from callbacks.
  for (auto mode : {MicrotaskMode::AttachedAuto, MicrotaskMode::AttachedExplicit, MicrotaskMode::StandaloneAuto}) {
    SCOPED_TRACE(static_cast<int>(mode));
    MicrotaskHost host(mode);
    bool called = false;
    napi_value callback{}, global{};
    ASSERT_EQ(napi_create_function(host.env, "reaction", NAPI_AUTO_LENGTH,
        [](napi_env env, napi_callback_info info) -> napi_value {
          void* data{};
          EXPECT_EQ(napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data), napi_ok);
          *static_cast<bool*>(data) = true;
          bool pending = true;
          EXPECT_EQ(napi_is_exception_pending(env, &pending), napi_ok);
          EXPECT_FALSE(pending);
          napi_value value{};
          EXPECT_EQ(napi_create_int32(env, 123, &value), napi_ok);
          return value;
        }, &called, &callback), napi_ok);
    ASSERT_EQ(napi_get_global(host.env, &global), napi_ok);
    ASSERT_EQ(napi_set_named_property(host.env, global, "reaction", callback), napi_ok);
    auto throwing = host.script("(() => { throw 42; })");
    auto schedule = host.script("(() => { globalThis.reactionResult = Promise.resolve().then(reaction); })");
    ASSERT_EQ(v8_node_api_env_queue_microtask(host.env, throwing), napi_ok);
    ASSERT_EQ(v8_node_api_env_queue_microtask(host.env, schedule), napi_ok);
    bool drained = true;
    EXPECT_EQ(v8_node_api_env_drain_microtasks(host.env, -1, &drained), napi_pending_exception);
    EXPECT_FALSE(drained);
    EXPECT_TRUE(called);
    napi_value exception{};
    ASSERT_EQ(napi_get_and_clear_last_exception(host.env, &exception), napi_ok);
    int32_t value{};
    ASSERT_EQ(napi_get_value_int32(host.env, exception, &value), napi_ok);
    EXPECT_EQ(value, 42);
    EXPECT_FALSE(host.rejected());
    host.script("reactionResult.then(value => globalThis.count = value);");
    host.drain();
    EXPECT_EQ(host.count(), 123);
  }
}

TEST(JsiAbiMicrotaskPolicy, NodeApiDrainEntersEnvironmentWithoutAnOpenScope) {
  for (auto mode : {MicrotaskMode::AttachedAuto, MicrotaskMode::AttachedExplicit, MicrotaskMode::StandaloneAuto}) { // Check the real public drain boundary without a caller-owned locker.
    SCOPED_TRACE(static_cast<int>(mode));
    MicrotaskHost host(mode);
    host.script("globalThis.count = 0;");
    napi_value callback = host.script("(() => ++count)");
    ASSERT_EQ(v8_node_api_env_queue_microtask(host.env, callback), napi_ok);
    ASSERT_EQ(napi_close_handle_scope(host.env, host.handles), napi_ok);
    host.handles = nullptr;
    ASSERT_EQ(v8_node_api_env_scope_close(host.scope, host.env), napi_ok);
    host.scope = nullptr;
    host.drain();
    ASSERT_EQ(v8_node_api_env_scope_open(host.env, &host.scope), napi_ok);
    ASSERT_EQ(napi_open_handle_scope(host.env, &host.handles), napi_ok);
    EXPECT_EQ(host.count(), 1);
  }
}

#if defined(_WIN32) && defined(_M_X64)
// A real child for the launcher contract tests; no runtime or gtest is started.
static int CetRunChild(int argc, char** argv) {
  if (argc < 3) return 2;
  if (!strcmp(argv[2], "echo")) {
    for (int i = 3; i < argc; ++i) printf("%zu:%s\n", strlen(argv[i]), argv[i]);
    fprintf(stderr, "child stderr\n");
  } else if (!strcmp(argv[2], "exit") && argc == 4) {
    ExitProcess(static_cast<DWORD>(strtoul(argv[3], nullptr, 0)));
  } else if (!strcmp(argv[2], "sleep")) {
    printf("pid=%lu\n", GetCurrentProcessId());
    fflush(stdout);
    Sleep(60000);
  } else if (!strcmp(argv[2], "tree")) {
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return 3;
    std::string command = "\"" + std::string(exe) + "\" --cetrun-child sleep";
    STARTUPINFOA si{sizeof(si)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                        nullptr, nullptr, &si, &child)) return 3;
    printf("parent=%lu\ndescendant=%lu\n", GetCurrentProcessId(), child.dwProcessId);
    fflush(stdout);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    Sleep(60000);
  } else if (!strcmp(argv[2], "policy")) {
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy{};
    using QueryPolicy = BOOL(WINAPI*)(HANDLE, PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
    auto query = reinterpret_cast<QueryPolicy>(GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "GetProcessMitigationPolicy"));
    if (!query || !query(GetCurrentProcess(), ProcessUserShadowStackPolicy,
                         &policy, sizeof(policy))) return 3;
    printf("child: shadowstack=%lu strict=%lu\n",
           static_cast<DWORD>(policy.EnableUserShadowStack),
           static_cast<DWORD>(policy.EnableUserShadowStackStrictMode));
  } else return 2;
  return 0;
}
#endif

int main(int argc, char **argv) {
#if defined(_WIN32) && defined(_M_X64)
  if (argc >= 2 && !strcmp(argv[1], "--cetrun-child")) return CetRunChild(argc, argv);
#endif
  // This child must reach snapshot creation before ANY engine flag call.
  const bool snapshotFirst = argc >= 2 && !strcmp(argv[1], "--cet-snapshot-first");
  if (snapshotFirst) {
    for (int i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc;
  }
  if (const char* flags = snapshotFirst ? nullptr : std::getenv("V8JSI_TEST_V8_FLAGS")) {
    std::istringstream input(flags);
    std::vector<std::string> storage{argv[0]};
    for (std::string flag; input >> flag;) storage.push_back(std::move(flag));
    std::vector<char*> flagArgs;
    for (auto& flag : storage) flagArgs.push_back(flag.data());
    size_t count = flagArgs.size();
    jsi_status status = v8_jsi_abi_set_v8_flags(&count, flagArgs.data(), false);
    if (status != jsi_status_ok) {
      fprintf(stderr, "V8JSI_TEST_V8_FLAGS rejected (status=%d): %s\n", static_cast<int>(status), flags);
      return 1;
    }
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
