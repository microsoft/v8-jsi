# Runtime hosting and migration notes

These are observations of the current implementation, backed by
`JsiAbiMicrotaskPolicy.*` and the hosting tests.
They do not extend the experimental ABI's compatibility promises.
See `src/jsi_abi/v8_jsi_abi.h` and `src/node-api/v8_node_api.h` for signatures.

## Microtasks and promises

| Creation path | Observed policy |
| --- | --- |
| JSI ABI with no configure callback | Explicit |
| JSI ABI with a configure callback and untouched policy setting | Auto |
| JSI ABI with `v8_jsi_abi_config_set_explicit_microtask_policy(..., true)` | Explicit |
| Standalone `v8_node_api_runtime_create`, including a null configure callback | Auto |
| Node-API attached with `v8_attach_node_api` | Inherits the JSI runtime's policy |

A configure callback starts with `explicit_microtask_policy = false`; this is
not the same as omitting the JSI configure callback. Standalone Node-API hosting
always supplies an internal configure callback. It exposes no policy setter;
use a configured JSI runtime plus attachment when explicit policy is needed.

With auto policy, promise reactions run as top-level evaluation or function
calls return. With explicit policy they remain pending until a checkpoint.
A host foreground task that calls a promise resolver follows the same rule:
posting the task is not a checkpoint, and explicit reactions need a drain
after the task runs. The host must provide and pump its own task runner.

Both drain entry points return a true drained flag after a successful drain;
repeated empty drains succeed without rerunning reactions. The implementation
ignores the maximum-count hint (including zero), running a checkpoint to completion.
Both entry points explicitly perform a checkpoint under every supported policy,
including for callbacks enqueued directly by the host with `jsi_queue_microtask`
or `v8_node_api_env_queue_microtask`.
With auto policy, a JSI ABI drain called from a host function during a running
JavaScript call now runs pending reactions immediately, just as under explicit
policy, consistent with the JSI checkpoint contract; it does not wait for the
outer JavaScript call to return.
A recursive drain from inside a microtask returns true without running another
checkpoint: the outer checkpoint still owns the remaining queue. This is not a
bounded-work or recursive-queue-emptiness guarantee.

Promise reaction exceptions reject the resulting promise; they are not a
synchronous drain error. Tracking is enabled by default. Node-API's
`v8_node_api_env_has_unhandled_promise_rejection` exposes the recorded rejection;
`v8_node_api_env_get_and_clear_last_unhandled_promise_rejection` consumes it.
Adding a late catch clears the matching recorded rejection before its catch
reaction runs. The implementation retains the last rejection, not a history.
`v8_jsi_abi_config_set_ignore_unhandled_promises(..., true)` disables tracking;
standalone Node-API hosting has no corresponding setter. This embedding is not
Node.js's process-level unhandled-rejection reporting policy.

For a raw, non-promise microtask throwing during a Node-API drain, the first
reported exception is retained in the environment. Node-API callbacks running
in the drain do not see an earlier raw microtask exception as pending; the first
such exception is reported after the drain. Drain returns
`napi_pending_exception`, leaves the drained flag false, and sets last error.
Use `napi_get_and_clear_last_exception` before retrying. V8 may already have run
later queued callbacks; false on error does not mean no work ran. The Node-API
drain enters its isolate/context and handle scope even without an open env
scope. During environment teardown it instead returns `napi_cannot_run_js`,
with the drained flag false. Other Node-API calls still need the host's normal
env/handle scopes.

## Host task-runner and teardown rules

1. Do not hold a lock acquired by your post callback when releasing a JSI runtime
   (`runtime->api->release(runtime)`) or calling `v8_node_api_runtime_destroy`.
   Destruction may wait for an in-flight inspector post or task.
2. A post callback must not block waiting on the runtime's own thread, nor
   synchronously reenter the runtime. Enqueue work for that thread instead.
3. Do not destroy a runtime while holding a Node-API env scope (isolate locker)
   or a lock needed by a task on another thread.
4. A task must not block waiting on the thread destroying its runtime.
   Destruction waits for running JavaScript. A debugger-paused task is cancelled:
   execution is terminated, so the remainder of that script does not run.
5. Posts can arrive from the inspector socket thread or during destruction.
   A queued payload can become a no-op; its delete callback is still required
   exactly once, whether the host runs or discards the task.

## Porting legacy hosting calls

This is a source-port checklist, not an alias layer or port-acceptance result.
The old `js_runtime_api.h` hosting names are not current exports. The two
`v8_*` legacy names below came from the old `v8_api.cpp` implementation, not
that header. Bind current `*_fn` signatures, not casts of legacy function
pointers. Hosting ABI version
`V8_NODE_API_ABI_VERSION` (1), JSI ABI version `JSI_ABI_VERSION` (2), and the
Node-API module version passed to attachment/module creation are distinct.

| Legacy entry point | Current entry point / action |
| --- | --- |
| `jsr_create_config`, `jsr_delete_config` | No allocated config: use the factory-owned config only inside the configure callback. |
| `jsr_create_runtime` | `v8_node_api_runtime_create(version, configure, data, out_runtime)` |
| `jsr_delete_runtime` | `v8_node_api_runtime_destroy` |
| `jsr_runtime_get_node_api_env` | `v8_node_api_runtime_get_env` |
| `jsr_runtime_get_jsi_runtime` | `v8_node_api_runtime_get_jsi_runtime` (borrowed; do not release) |
| `jsr_config_enable_inspector` | `v8_node_api_config_set_inspector_enabled` |
| `jsr_config_enable_gc_api` | `v8_node_api_config_set_gc_api_enabled` |
| `v8_config_enable_multithreading` | `v8_node_api_config_set_multithreading_enabled` |
| `jsr_config_set_inspector_runtime_name` | `v8_node_api_config_set_inspector_runtime_name_utf8` (explicit length) |
| `jsr_config_set_inspector_port`, `jsr_config_set_inspector_break_on_start` | `v8_node_api_config_set_inspector_port`, `v8_node_api_config_set_inspector_break_on_start` |
| `jsr_config_set_task_runner`, `jsr_config_set_script_cache` | `v8_node_api_config_set_task_runner`, `v8_node_api_config_set_script_cache` (adapt argument order and callback ownership) |
| `jsr_open_napi_env_scope`, `jsr_close_napi_env_scope` | `v8_node_api_env_scope_open(env, out)`, `v8_node_api_env_scope_close(scope, env)` (close arguments reversed) |
| `jsr_get_description`, `jsr_is_inspectable` | `v8_node_api_env_get_description_utf8`, `v8_node_api_env_is_inspectable` |
| `jsr_queue_microtask`, `jsr_drain_microtasks` | `v8_node_api_env_queue_microtask`, `v8_node_api_env_drain_microtasks` |
| `jsr_run_script` | `v8_node_api_env_run_script_utf8` (URL length added) or standard `napi_run_script` |
| `jsr_create_prepared_script`, `jsr_delete_prepared_script`, `jsr_prepared_script_run` | `v8_node_api_prepared_script_create_utf8`, `v8_node_api_prepared_script_destroy`, `v8_node_api_prepared_script_run` |
| `jsr_collect_garbage` | `v8_node_api_env_request_garbage_collection` |
| `jsr_has_unhandled_promise_rejection`, `jsr_get_and_clear_last_unhandled_promise_rejection` | `v8_node_api_env_has_unhandled_promise_rejection`, `v8_node_api_env_get_and_clear_last_unhandled_promise_rejection` |
| `jsr_create_node_api_env`, `jsr_run_task` | `v8_node_api_env_create_module`, `v8_node_api_env_run_task` |
| `jsr_instrumentation_get_gc_stats`, `jsr_instrumentation_get_heap_info` | `v8_node_api_env_get_recorded_gc_stats_utf8`, `v8_node_api_env_get_heap_info` |
| `jsr_instrumentation_collect_garbage`, `jsr_instrumentation_start_heap_sampling` | `v8_node_api_env_collect_garbage_utf8`, `v8_node_api_env_start_heap_sampling` |
| `jsr_instrumentation_stop_heap_sampling`, `jsr_instrumentation_create_heap_snapshot` | `v8_node_api_env_stop_heap_sampling_utf8`, `v8_node_api_env_create_heap_snapshot_utf8` |
| `v8_platform_dispose` | `v8_node_api_dispose_platform` (process-wide and irreversible) |

Legacy delete callbacks took `(data, deleter_data)`; current callbacks take
`(delete_data, data)`. Adapt task/cache callbacks and explicit UTF-8 lengths;
do not mechanically rename or reuse old callback types. A failing config setter
poisons creation. Standard `napi_*` calls remain separate from hosting exports;
check each extension and module's required Node-API version when porting.

## Initialization and behavior changes for hosts

- `v8_jsi_abi_config_set_thread_pool_size` takes effect only at the first
  platform initialization. Later runtime configurations cannot resize that
  process-wide pool. There is no new standalone Node-API pool-size setter.
- A lone V8 flag now applies. Previously ignored flags such as `predictable` or
  `always_compact` can therefore incur a performance cost. Set flags before
  initializing V8; `v8_jsi_abi_set_v8_flags` now rejects calls after initialization.
- `jsi_evaluate_javascript_source` (and `makeV8Runtime`'s `evaluateJavaScript`)
  now loads the script cache and stores it after a successful run, never after
  a compile error or a throw. `jsi_prepare_javascript` stores at prepare time.
  Both replace a rejected cache entry. Node-API prepared scripts are unchanged
  and do not rewrite a rejected entry.
- Node-API drain is no longer a no-op, and JSI ABI drain checkpoints under
  every supported policy; see [Microtasks and promises](#microtasks-and-promises)
  for drain results, errors, and host-queued callbacks.
  The public C++ `makeV8Runtime` wrapper is ABI-backed, so its
  `jsi::Runtime::drainMicrotasks` follows the JSI ABI behavior above under both
  auto and explicit policy. Default `V8RuntimeArgs` use auto policy.
- For engine RID defaults, explicit overrides, and package availability checks,
  see [NuGet engine RID selection](v8host-sandbox.md#nuget-engine-rid-selection).
- For queued-task ownership and destruction (including debugger-paused tasks),
  see [Host task-runner and teardown rules](#host-task-runner-and-teardown-rules).
  Keep host queues and callback data alive through teardown.
