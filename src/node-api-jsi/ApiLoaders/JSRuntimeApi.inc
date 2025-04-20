// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#ifndef JSR_FUNC
#define JSR_FUNC(func)
#endif

#ifndef JSR_JSI_FUNC
#define JSR_JSI_FUNC(func)
#endif

#ifndef JSR_PREPARED_SCRIPT
#define JSR_PREPARED_SCRIPT(func)
#endif

// The JS runtime functions sorted alphabetically.
JSR_FUNC(jsr_collect_garbage)
JSR_FUNC(jsr_config_enable_gc_api)
JSR_FUNC(jsr_config_enable_inspector)
JSR_FUNC(jsr_config_set_inspector_break_on_start)
JSR_FUNC(jsr_config_set_inspector_port)
JSR_FUNC(jsr_config_set_inspector_runtime_name)
JSR_FUNC(jsr_config_set_script_cache)
JSR_FUNC(jsr_config_set_task_runner)
JSR_FUNC(jsr_create_config)
JSR_FUNC(jsr_create_runtime)
JSR_FUNC(jsr_create_heap_snapshot_to_file)
JSR_FUNC(jsr_create_heap_snapshot_to_string)
JSR_FUNC(jsr_delete_config)
JSR_FUNC(jsr_delete_runtime)
JSR_FUNC(jsr_dump_profiler_symbols)
JSR_FUNC(jsr_flush_and_disable_bridge_traffic_trace)
JSR_FUNC(jsr_free_string)
JSR_FUNC(jsr_get_and_clear_last_unhandled_promise_rejection)
JSR_FUNC(jsr_get_heap_info)
JSR_FUNC(jsr_get_recorded_gc_stats)
JSR_FUNC(jsr_has_unhandled_promise_rejection)
JSR_FUNC(jsr_run_script)
JSR_FUNC(jsr_runtime_get_node_api_env)
JSR_FUNC(jsr_start_heap_sampling)
JSR_FUNC(jsr_start_tracking_heap_object_stack_traces)
JSR_FUNC(jsr_stop_heap_sampling)
JSR_FUNC(jsr_stop_tracking_heap_object_stack_traces)
JSR_FUNC(jsr_write_basic_block_profile_trace)

// The JS runtime functions needed for JSI.
JSR_JSI_FUNC(jsr_close_napi_env_scope)
JSR_JSI_FUNC(jsr_queue_microtask)
JSR_JSI_FUNC(jsr_drain_microtasks)
JSR_JSI_FUNC(jsr_get_description)
JSR_JSI_FUNC(jsr_is_inspectable)
JSR_JSI_FUNC(jsr_open_napi_env_scope)

// The JS runtime functions needed for prepared script.
JSR_PREPARED_SCRIPT(jsr_create_prepared_script)
JSR_PREPARED_SCRIPT(jsr_delete_prepared_script)
JSR_PREPARED_SCRIPT(jsr_prepared_script_run)

#undef JSR_FUNC
#undef JSR_JSI_FUNC
#undef JSR_PREPARED_SCRIPT