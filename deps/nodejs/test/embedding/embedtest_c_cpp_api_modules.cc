#include "embedtest_c_cpp_api_common.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace node::embedding {

namespace {

class GreeterModule {
 public:
  explicit GreeterModule(std::atomic<int32_t>* counter_ptr)
      : counter_ptr_(counter_ptr) {}

  napi_value operator()(const NodeRuntime& runtime,
                        napi_env env,
                        std::string_view module_name,
                        napi_value exports) {
    NodeApiErrorHandler<napi_value> error_handler(env);
    counter_ptr_->fetch_add(1);

    napi_value greet_func{};
    NODE_API_CALL(napi_create_function(
        env,
        "greet",
        NAPI_AUTO_LENGTH,
        [](napi_env env, napi_callback_info info) -> napi_value {
          NodeApiErrorHandler<napi_value> error_handler(env);
          std::string greeting = "Hello, ";
          napi_value arg{};
          size_t arg_count = 1;
          NODE_API_CALL(
              napi_get_cb_info(env, info, &arg_count, &arg, nullptr, nullptr));
          NODE_API_CALL(AddUtf8String(greeting, env, arg));
          napi_value result;
          NODE_API_CALL(napi_create_string_utf8(
              env, greeting.c_str(), greeting.size(), &result));
          return result;
        },
        nullptr,
        &greet_func));
    NODE_API_CALL(napi_set_named_property(env, exports, "greet", greet_func));
    return exports;
  }

 private:
  std::atomic<int32_t>* counter_ptr_;
};

class ReplicatorModule {
 public:
  explicit ReplicatorModule(std::atomic<int32_t>* counter_ptr)
      : counter_ptr_(counter_ptr) {}

  napi_value operator()(const NodeRuntime& runtime,
                        napi_env env,
                        std::string_view module_name,
                        napi_value exports) {
    NodeApiErrorHandler<napi_value> error_handler(env);
    counter_ptr_->fetch_add(1);

    napi_value greet_func{};
    NODE_API_CALL(napi_create_function(
        env,
        "replicate",
        NAPI_AUTO_LENGTH,
        [](napi_env env, napi_callback_info info) -> napi_value {
          NodeApiErrorHandler<napi_value> error_handler(env);
          std::string str;
          napi_value arg{};
          size_t arg_count = 1;
          NODE_API_CALL(
              napi_get_cb_info(env, info, &arg_count, &arg, nullptr, nullptr));
          NODE_API_CALL(AddUtf8String(str, env, arg));
          str += " " + str;
          napi_value result;
          NODE_API_CALL(
              napi_create_string_utf8(env, str.c_str(), str.size(), &result));
          return result;
        },
        nullptr,
        &greet_func));
    NODE_API_CALL(
        napi_set_named_property(env, exports, "replicate", greet_func));
    return exports;
  }

 private:
  std::atomic<int32_t>* counter_ptr_;
};

// Above NODE_API_SUPPORTED_VERSION_MAX, so Node.js rejects the module.
constexpr int32_t kUnsupportedNodeApiVersion = 9999;

// Throws from its initializer after creating new exports:
// process._linkedBinding() must throw the error instead of returning them.
class FailingModule {
 public:
  explicit FailingModule(std::atomic<int32_t>* counter_ptr)
      : counter_ptr_(counter_ptr) {}

  napi_value operator()(const NodeRuntime& runtime,
                        napi_env env,
                        std::string_view module_name,
                        napi_value exports) {
    NodeApiErrorHandler<napi_value> error_handler(env);
    counter_ptr_->fetch_add(1);

    napi_value new_exports{};
    NODE_API_CALL(napi_create_object(env, &new_exports));
    NODE_API_CALL(napi_throw_error(env, nullptr, "failing_module init failed"));
    return new_exports;
  }

 private:
  std::atomic<int32_t>* counter_ptr_;
};

// Registered with an unsupported Node-API version: Node.js throws before the
// initializer can run.
class UnsupportedModule {
 public:
  explicit UnsupportedModule(std::atomic<int32_t>* counter_ptr)
      : counter_ptr_(counter_ptr) {}

  napi_value operator()(const NodeRuntime& runtime,
                        napi_env env,
                        std::string_view module_name,
                        napi_value exports) {
    counter_ptr_->fetch_add(1);
    return exports;
  }

 private:
  std::atomic<int32_t>* counter_ptr_;
};

}  // namespace

int32_t test_main_c_cpp_api_linked_modules(int32_t argc, const char* argv[]) {
  TestExitCodeHandler error_handler(argv[0]);
  NODE_ASSERT(argc == 4);
  int32_t expectedGreeterModuleInitCallCount = atoi(argv[2]);
  int32_t expectedReplicatorModuleInitCallCount = atoi(argv[2]);

  std::atomic<int32_t> greeterModuleInitCallCount{0};
  std::atomic<int32_t> replicatorModuleInitCallCount{0};
  std::atomic<int32_t> failingModuleInitCallCount{0};
  std::atomic<int32_t> unsupportedModuleInitCallCount{0};

  NODE_EMBEDDING_CALL(NodePlatform::RunMain(
      NodeArgs(argc, argv),
      nullptr,
      NodeConfigureRuntimeCallback(
          [&](const NodePlatform& platform,
              const NodeRuntimeConfig& runtime_config) {
            NodeEmbeddingErrorHandler error_handler;
            NODE_EMBEDDING_CALL(
                runtime_config.OnPreload([](const NodeRuntime& runtime,
                                            napi_env env,
                                            napi_value process,
                                            napi_value /*require*/
                                         ) {
                  napi_value global;
                  napi_get_global(env, &global);
                  napi_set_named_property(env, global, "process", process);
                }));

            NODE_EMBEDDING_CALL(runtime_config.AddModule(
                "greeter_module",
                GreeterModule(&greeterModuleInitCallCount),
                NAPI_VERSION));

            NODE_EMBEDDING_CALL(runtime_config.AddModule(
                "replicator_module",
                ReplicatorModule(&replicatorModuleInitCallCount),
                NAPI_VERSION));

            NODE_EMBEDDING_CALL(runtime_config.AddModule(
                "failing_module",
                FailingModule(&failingModuleInitCallCount),
                NAPI_VERSION));

            NODE_EMBEDDING_CALL(runtime_config.AddModule(
                "unsupported_module",
                UnsupportedModule(&unsupportedModuleInitCallCount),
                kUnsupportedNodeApiVersion));

            NODE_EMBEDDING_CALL(LoadUtf8Script(runtime_config, main_script));

            return error_handler.ReportResult();
          })));

  NODE_ASSERT(greeterModuleInitCallCount == expectedGreeterModuleInitCallCount);
  NODE_ASSERT(replicatorModuleInitCallCount ==
              expectedReplicatorModuleInitCallCount);
  // use-linked-modules.js loads each failing module once, in the main thread.
  NODE_ASSERT(failingModuleInitCallCount == 1);
  NODE_ASSERT(unsupportedModuleInitCallCount == 0);

  return error_handler.ReportResult();
}

}  // namespace node::embedding
