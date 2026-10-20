/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT license.
 *
 * Portions derived from facebook/hermes (Hermes ABI):
 *   Copyright (c) Meta Platforms, Inc. and affiliates.
 *   Licensed under the MIT license.
 *
 * JSI ABI — an ABI-stable C interface for JSI (JavaScript Interface).
 *
 * Derived from Meta's Hermes ABI: renamed to be engine-agnostic, with
 * additional changes and improvements.
 *
 * Pure C header. No engine-specific types. Can be used by any JS engine
 * (Hermes, V8, JSC).
 */

/* ===========================================================================
 * EXPERIMENTAL API - NOT YET ABI-STABLE
 *
 * This API is experimental and subject to change. Although it is C-based, it is
 * NOT ABI-safe yet: function signatures, function-table layouts, struct fields, enum
 * values, and error codes may change without notice while the API remains in
 * experimental mode. Do not depend on binary compatibility across builds. Once
 * the API leaves experimental mode it will become ABI-stable and this notice
 * will be removed.
 * =========================================================================== */

#ifndef JSI_ABI_H_
#define JSI_ABI_H_

#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "jsi_abi_stela.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==========================================================================
 * Platform Macros
 *==========================================================================*/

/// Calling convention: __cdecl on Windows (it matters on x86), empty elsewhere.
#ifndef JSI_ABI_CDECL
#ifdef _WIN32
#define JSI_ABI_CDECL __cdecl
#else
#define JSI_ABI_CDECL
#endif
#endif

/**
 * Exports only while the implementing library builds (JSI_ABI_IMPL defined).
 * Consumers get it empty, or may predefine it as __declspec(dllimport).
 */
#ifndef JSI_ABI_API
#ifdef JSI_ABI_IMPL
#ifdef _WIN32
#define JSI_ABI_API __declspec(dllexport)
#else
#define JSI_ABI_API __attribute__((visibility("default")))
#endif
#else
#define JSI_ABI_API
#endif
#endif

/*==========================================================================
 * ABI Version
 *
 * Monotonic integer (Node-API NAPI_VERSION style). Bump by 1 for every
 * additive change — there is no major/minor split because the ABI is strictly
 * append-only with behavioral pinning. The consumer's compiled-against value is
 * passed to the engine's create function (e.g. v8_jsi_abi_runtime_create), which pins
 * the runtime to it or fails if it exceeds the implementation's max (Model B).
 * Version 2 is the first status-based ABI; version 1 is no longer accepted.
 *==========================================================================*/

/// The current JSI ABI version.
#define JSI_ABI_VERSION 2u

/*==========================================================================
 * Forward Declarations
 *==========================================================================*/

typedef struct jsi_runtime jsi_runtime;
typedef struct jsi_runtime_api jsi_runtime_api;
typedef struct jsi_pointer jsi_pointer;
typedef struct jsi_pointer_api jsi_pointer_api;
typedef struct jsi_object jsi_object;
typedef struct jsi_array jsi_array;
typedef struct jsi_string jsi_string;
typedef struct jsi_bigint jsi_bigint;
typedef struct jsi_symbol jsi_symbol;
typedef struct jsi_function jsi_function;
typedef struct jsi_arraybuffer jsi_arraybuffer;
typedef struct jsi_propnameid jsi_propnameid;
typedef struct jsi_weak_object jsi_weak_object;
typedef struct jsi_value jsi_value;
typedef struct jsi_growable_buffer jsi_growable_buffer;
typedef struct jsi_growable_buffer_api jsi_growable_buffer_api;
typedef struct jsi_buffer jsi_buffer;
typedef struct jsi_buffer_api jsi_buffer_api;
typedef struct jsi_mutable_buffer jsi_mutable_buffer;
typedef struct jsi_mutable_buffer_api jsi_mutable_buffer_api;
typedef struct jsi_prepared_javascript jsi_prepared_javascript;
typedef struct jsi_prepared_javascript_api jsi_prepared_javascript_api;
typedef struct jsi_host_function jsi_host_function;
typedef struct jsi_host_function_api jsi_host_function_api;
typedef struct jsi_propnameid_list jsi_propnameid_list;
typedef struct jsi_propnameid_list_api jsi_propnameid_list_api;
typedef struct jsi_host_object jsi_host_object;
typedef struct jsi_host_object_api jsi_host_object_api;
typedef struct jsi_native_state jsi_native_state;
typedef struct jsi_native_state_api jsi_native_state_api;
typedef struct jsi_interface_id jsi_interface_id;

/// A UTF-16 code unit.
#ifdef __cplusplus
typedef char16_t jsi_char16;
#else
typedef uint16_t jsi_char16;
#endif

/// Pass as a (text, length) input's length when the text is NUL-terminated.
#define JSI_ABI_AUTO_LENGTH SIZE_MAX

/**
 * Opaque, engine-agnostic configuration object. The runtime factory (e.g.
 * v8_jsi_abi_runtime_create) allocates one and hands it to a jsi_configure_runtime_cb
 * so the consumer can populate it via engine-specific setters (e.g.
 * v8_jsi_abi_config_set_*); the handle is valid ONLY for that callback's duration.
 * Layout is owned by the engine implementation.
 */
typedef struct jsi_config_s* jsi_config;

/**
 * Standard data-deleter callback used when ownership of a buffer or other
 * pointer crosses the DLL boundary. The side that hands the pointer over also
 * supplies this callback so the receiver can free it with the same allocator
 * that allocated it. Called as delete_cb(user_data, data): user_data is the
 * deleter's own context, data is the pointer being released.
 */
typedef void(JSI_ABI_CDECL *jsi_data_delete_cb)(
    JSI_ABI_NULLABLE void *user_data,
    void *data);

/*==========================================================================
 * Status
 *
 * Every fallible call returns a jsi_status. Codes are grouped in bands
 * (code & 0xF000); test the band, not the exact code, to classify a failure.
 * On failure, a call's out_* outputs are zeroed (a jsi_value becomes
 * undefined).
 *==========================================================================*/

JSI_ABI_ENUM(int32_t, jsi_status){
    jsi_status_ok = 0,
    /* 0x1000: caller contract */
    jsi_status_invalid_arg = 0x1001,
    jsi_status_version_mismatch = 0x1002,
    /* 0x2000: state and resources (reserved) */
    /* 0x4000: a JS exception is pending; fetch it with
     * get_and_clear_js_error_value */
    jsi_status_js_error = 0x4001,
    /* 0x7000: native or internal failure; fetch the message with
     * get_and_clear_native_exception_message */
    jsi_status_native_error = 0x7001,
};

/*==========================================================================
 * Configure callback (factory-owned pull model)
 *==========================================================================*/

/**
 * The runtime factory allocates a jsi_config and invokes this callback so the
 * consumer can populate it via the engine's typed setters. The config handle is
 * valid ONLY for the duration of the call. Return jsi_status_ok to proceed, or
 * a failure status to abort creation (the factory then frees the config, and
 * the create call returns that status with *out_runtime set to NULL). May be
 * NULL at the create site, meaning "all defaults".
 */
typedef jsi_status(JSI_ABI_CDECL *jsi_configure_runtime_cb)(
    JSI_ABI_NULLABLE void *user_data,
    jsi_config config);

/*==========================================================================
 * Pointer Types (from Hermes ABI pattern)
 *
 * ManagedPointer — reference-counted GC root, released through its api table.
 * Each pointer type wraps a jsi_pointer* to indicate the JS type.
 *
 * NOTE: We intentionally spell out each type explicitly instead of using
 * X-macros (like Hermes ABI's HERMES_ABI_POINTER_TYPES macro). Explicit
 * declarations are debugger-friendly (F12/Go to Definition works), visible
 * to IntelliSense, and easier to read and maintain.
 *==========================================================================*/

/// Operations for a managed JSI pointer.
struct jsi_pointer_api {
  void(JSI_ABI_CDECL *invalidate)(jsi_pointer *self);
};
/// A reference-counted GC root.
struct jsi_pointer {
  const jsi_pointer_api *api;
};

struct jsi_object {
  jsi_pointer *pointer;
};
struct jsi_array {
  jsi_pointer *pointer;
};
struct jsi_string {
  jsi_pointer *pointer;
};
struct jsi_bigint {
  jsi_pointer *pointer;
};
struct jsi_symbol {
  jsi_pointer *pointer;
};
struct jsi_function {
  jsi_pointer *pointer;
};
struct jsi_arraybuffer {
  jsi_pointer *pointer;
};
struct jsi_propnameid {
  jsi_pointer *pointer;
};
struct jsi_weak_object {
  jsi_pointer *pointer;
};

/*==========================================================================
 * Value Representation (inline tagged union — from Hermes ABI)
 *==========================================================================*/

/**
 * Marks the value kinds whose data is a jsi_pointer*. Test with this macro,
 * never a literal.
 */
#define JSI_ABI_POINTER_MASK 0x40000000

/// The kind of value stored in a jsi_value.
JSI_ABI_ENUM(int32_t, jsi_valuekind){
    jsi_valuekind_undefined = 0,
    jsi_valuekind_null = 1,
    jsi_valuekind_boolean = 2,
    /* 3 is reserved. */
    jsi_valuekind_number = 4,
    jsi_valuekind_symbol = 5 | JSI_ABI_POINTER_MASK,
    jsi_valuekind_bigint = 6 | JSI_ABI_POINTER_MASK,
    jsi_valuekind_string = 7 | JSI_ABI_POINTER_MASK,
    jsi_valuekind_object = 9 | JSI_ABI_POINTER_MASK,
};

/// A JS value: a kind tag plus an inline payload.
struct jsi_value {
  jsi_valuekind kind; ///< The value kind.
  union {
    bool boolean;
    double number;
    jsi_pointer *pointer;
  } data; ///< The inline value payload.
};
#define JSI_ABI_VALUE_INIT {jsi_valuekind_undefined, {false}}
/*==========================================================================
 * Buffer Types (from Hermes ABI)
 *
 * Three flavors with different ownership / lifetime semantics:
 *
 *   jsi_growable_buffer  — short-lived output parameter. Caller stack-
 *                          allocates and passes &buf for the DLL to fill
 *                          in during one call. The DLL only ever calls
 *                          try_grow_to to expand it; ownership stays with
 *                          the caller, so no release callback is needed.
 *
 *   jsi_buffer           — read-only input the DLL retains past the call
 *                          (e.g., script source). Consumer heap-allocates;
 *                          the DLL calls api->release when done.
 *
 *   jsi_mutable_buffer   — writable backing the DLL retains past the call
 *                          (e.g., ArrayBuffer storage). Same release
 *                          contract as jsi_buffer.
 *==========================================================================*/

/// Operations for a growable output buffer.
struct jsi_growable_buffer_api {
  void(JSI_ABI_CDECL *try_grow_to)(jsi_growable_buffer *buf, size_t sz);
};
/// A caller-owned growable output buffer.
struct jsi_growable_buffer {
  const jsi_growable_buffer_api *api;
  JSI_ABI_NULLABLE uint8_t *data; ///< The buffer data.
  size_t size; ///< The capacity in bytes.
  size_t used; ///< The bytes written so far.
};
#define JSI_ABI_GROWABLE_BUFFER_INIT {NULL, NULL, 0, 0}

/// Operations for a retained read-only buffer.
struct jsi_buffer_api {
  void(JSI_ABI_CDECL *release)(jsi_buffer *self);
};
/// A retained read-only buffer.
struct jsi_buffer {
  const jsi_buffer_api *api;
  const uint8_t *data; ///< The buffer data.
  size_t size; ///< The buffer size in bytes.
};
#define JSI_ABI_BUFFER_INIT {NULL, NULL, 0}

/// Operations for a retained writable buffer.
struct jsi_mutable_buffer_api {
  void(JSI_ABI_CDECL *release)(jsi_mutable_buffer *self);
};
/// A retained writable buffer.
struct jsi_mutable_buffer {
  const jsi_mutable_buffer_api *api;
  uint8_t *data; ///< The buffer data.
  size_t size; ///< The buffer size in bytes.
};
#define JSI_ABI_MUTABLE_BUFFER_INIT {NULL, NULL, 0}

/*==========================================================================
 * PreparedJavaScript
 *
 * Opaque engine-managed handle to a compiled script. Returned by
 * prepare_javascript; passed (by pointer) to evaluate_prepared_javascript.
 * Always carried by pointer so the implementation can append fields beyond
 * the api field without breaking ABI for callers that only see the
 * base layout.
 *
 * Lifecycle: the consumer owns the returned pointer and calls
 * api->release(p) when done.
 *==========================================================================*/

/// Operations for a prepared JavaScript handle.
struct jsi_prepared_javascript_api {
  void(JSI_ABI_CDECL *release)(jsi_prepared_javascript *self);
};
/// An engine-managed prepared JavaScript handle.
struct jsi_prepared_javascript {
  const jsi_prepared_javascript_api *api;
  /* Implementation may append fields after this header. */
};

/*==========================================================================
 * Callback Types (embedded-struct pattern — from Hermes ABI)
 *==========================================================================*/

/// Host function: called when JS invokes a native function.
struct jsi_host_function_api {
  void(JSI_ABI_CDECL *release)(jsi_host_function *);
  jsi_status(JSI_ABI_CDECL *call)(
      jsi_host_function *self,
      jsi_runtime *rt,
      const jsi_value *this_arg,
      JSI_ABI_NULLABLE const jsi_value *args,
      size_t arg_count,
      jsi_value *out_value);
};
/// A native function callable from JavaScript.
struct jsi_host_function {
  const jsi_host_function_api *api;
};
#define JSI_ABI_HOST_FUNCTION_INIT {NULL}

/// PropNameID list: returned by host object get_own_keys.
struct jsi_propnameid_list_api {
  void(JSI_ABI_CDECL *release)(jsi_propnameid_list *);
};
/// A list of property-name identifiers.
struct jsi_propnameid_list {
  const jsi_propnameid_list_api *api;
  JSI_ABI_NULLABLE const jsi_propnameid *props; ///< The property-name identifiers.
  size_t size; ///< The number of property-name identifiers.
};
#define JSI_ABI_PROPNAMEID_LIST_INIT {NULL, NULL, 0}

/// Host object: JS object backed by native callbacks.
struct jsi_host_object_api {
  void(JSI_ABI_CDECL *release)(jsi_host_object *);
  jsi_status(JSI_ABI_CDECL *get)(
      jsi_host_object *self,
      jsi_runtime *rt,
      jsi_propnameid name,
      jsi_value *out_value);
  jsi_status(JSI_ABI_CDECL *set)(
      jsi_host_object *self,
      jsi_runtime *rt,
      jsi_propnameid name,
      const jsi_value *value);
  jsi_status(JSI_ABI_CDECL *get_own_keys)(
      jsi_host_object *self,
      jsi_runtime *rt,
      jsi_propnameid_list **out_list);
};
/// A JavaScript object backed by native callbacks.
struct jsi_host_object {
  const jsi_host_object_api *api;
};
#define JSI_ABI_HOST_OBJECT_INIT {NULL}

/// Native state: attached to JS objects, released on GC.
struct jsi_native_state_api {
  void(JSI_ABI_CDECL *release)(jsi_native_state *self);
};
/// Native state attached to a JavaScript object.
struct jsi_native_state {
  const jsi_native_state_api *api;
};
#define JSI_ABI_NATIVE_STATE_INIT {NULL}

/*==========================================================================
 * Interface Query (for optional extensions — like JSI C++ ICast)
 *==========================================================================*/

/// An identifier for an optional interface.
struct jsi_interface_id {
  uint64_t lower; ///< The lower 64 bits.
  uint64_t upper; ///< The upper 64 bits.
};

/*==========================================================================
 * Runtime function table
 *==========================================================================*/

/// The runtime function table.
struct jsi_runtime_api {
  /* Pure function table — no version/reserved data fields. Version negotiation
   * happens once at create time (see JSI_ABI_VERSION / v8_jsi_abi_runtime_create). New
   * functions are appended at the END of this struct. */

  /*----------------------------------------------------------------------
   * QueryInterface / ICast (C++ JSI: castInterface)
   * Placed first — like COM's QueryInterface at slot 0. Allows consumers to
   * discover capabilities before calling anything else.
   *----------------------------------------------------------------------*/

  /**
   * Query for an optional interface extension.
   * Returns jsi_status_native_error with empty message if not supported.
   */
  jsi_status(JSI_ABI_CDECL *query_interface)(
      jsi_runtime *rt,
      const jsi_interface_id *iid,
      const void **out_api,
      void **out_instance);

  /**
   * Report the implementation's max supported ABI version (Node-API
   * napi_get_version analog). Optional feature-probe — the create-time version
   * negotiation in v8_jsi_abi_runtime_create is what actually gates.
   */
  uint32_t(JSI_ABI_CDECL *get_abi_version)(jsi_runtime *rt);

  /*----------------------------------------------------------------------
   * Lifecycle
   *
   * The runtime is reference-counted. v8_jsi_abi_runtime_create /
   * v8_node_api_runtime_create
   * hand back a runtime with refcount 1. Each add_ref increments; each
   * release decrements and destroys the runtime when the count reaches 0.
   *----------------------------------------------------------------------*/

  void(JSI_ABI_CDECL *add_ref)(jsi_runtime *);
  void(JSI_ABI_CDECL *release)(jsi_runtime *);

  /*----------------------------------------------------------------------
   * Error retrieval (from Hermes ABI pattern)
   *----------------------------------------------------------------------*/

  /// Get and clear the JS exception value (after jsi_status_js_error).
  jsi_value(JSI_ABI_CDECL *get_and_clear_js_error_value)(
      jsi_runtime *rt);

  /// Get and clear native exception message (after jsi_status_native_error).
  void(JSI_ABI_CDECL *get_and_clear_native_exception_message)(
      jsi_runtime *rt,
      jsi_growable_buffer *buffer);

  /* Set errors before returning from host function/object callbacks. */
  void(JSI_ABI_CDECL *set_js_error_value)(
      jsi_runtime *rt,
      const jsi_value *error_value);
  void(JSI_ABI_CDECL *set_native_exception_message_utf8)(
      jsi_runtime *rt,
      JSI_ABI_NULLABLE const char *value,
      size_t length);

  /*----------------------------------------------------------------------
   * Clone operations (from Hermes ABI)
   *----------------------------------------------------------------------*/

  jsi_propnameid(JSI_ABI_CDECL *clone_propnameid)(
      jsi_runtime *rt,
      jsi_propnameid name);
  jsi_string(JSI_ABI_CDECL *clone_string)(
      jsi_runtime *rt,
      jsi_string str);
  jsi_symbol(JSI_ABI_CDECL *clone_symbol)(
      jsi_runtime *rt,
      jsi_symbol sym);
  jsi_object(JSI_ABI_CDECL *clone_object)(
      jsi_runtime *rt,
      jsi_object obj);
  jsi_bigint(JSI_ABI_CDECL *clone_bigint)(
      jsi_runtime *rt,
      jsi_bigint bigint);

  /*----------------------------------------------------------------------
   * Runtime description and capabilities
   *----------------------------------------------------------------------*/

  void(JSI_ABI_CDECL *get_description)(
      jsi_runtime *rt,
      jsi_growable_buffer *buffer);
  bool(JSI_ABI_CDECL *is_inspectable)(jsi_runtime *rt);

  /*----------------------------------------------------------------------
   * Handle scopes
   *----------------------------------------------------------------------*/

  /**
   * Push a new handle scope. All pointer types created after this call
   * are valid until pop_scope. Engines that don't need scopes (Hermes)
   * can implement these as no-ops.
   */
  jsi_status(JSI_ABI_CDECL *push_scope)(
      jsi_runtime *rt,
      void **out_scope);
  jsi_status(JSI_ABI_CDECL *pop_scope)(
      jsi_runtime *rt,
      void *scope);

  /*----------------------------------------------------------------------
   * Script evaluation
   *----------------------------------------------------------------------*/

  jsi_status(JSI_ABI_CDECL *evaluate_javascript_source)(
      jsi_runtime *rt,
      JSI_ABI_TRANSFER_FULL jsi_buffer *buffer,
      JSI_ABI_NULLABLE JSI_ABI_ENCODING(utf8) const char *source_url,
      size_t source_url_length,
      jsi_value *out_value);

  /**
   * Prepare (compile) a script for later execution. The returned
   * jsi_prepared_javascript* is owned by the caller and freed via
   * api->release.
   */
  jsi_status(JSI_ABI_CDECL *prepare_javascript)(
      jsi_runtime *rt,
      JSI_ABI_TRANSFER_FULL jsi_buffer *buffer,
      JSI_ABI_NULLABLE JSI_ABI_ENCODING(utf8) const char *source_url,
      size_t source_url_length,
      JSI_ABI_TRANSFER_FULL jsi_prepared_javascript **out_prepared);
  jsi_status(JSI_ABI_CDECL *evaluate_prepared_javascript)(
      jsi_runtime *rt,
      jsi_prepared_javascript *prepared,
      jsi_value *out_value);

  /*----------------------------------------------------------------------
   * Microtasks
   *----------------------------------------------------------------------*/

  jsi_status(JSI_ABI_CDECL *drain_microtasks)(
      jsi_runtime *rt,
      int32_t max_hint,
      bool *out_drained);
  jsi_status(JSI_ABI_CDECL *queue_microtask)(
      jsi_runtime *rt,
      jsi_function callback);

  /*----------------------------------------------------------------------
   * Global object
   *----------------------------------------------------------------------*/

  jsi_object(JSI_ABI_CDECL *get_global_object)(jsi_runtime *rt);

  /*----------------------------------------------------------------------
   * String operations
   *----------------------------------------------------------------------*/

  jsi_status(JSI_ABI_CDECL *create_string_utf8)(
      jsi_runtime *rt,
      JSI_ABI_NULLABLE const char *value,
      size_t length,
      jsi_string *out_string);
  jsi_status(JSI_ABI_CDECL *create_string_utf16)(
      jsi_runtime *rt,
      JSI_ABI_NULLABLE const jsi_char16 *value,
      size_t length,
      jsi_string *out_string);
  void(JSI_ABI_CDECL *get_string_utf8)(
      jsi_runtime *rt,
      jsi_string str,
      jsi_growable_buffer *buffer);
  void(JSI_ABI_CDECL *get_string_utf16)(
      jsi_runtime *rt,
      jsi_string str,
      jsi_growable_buffer *buffer);

  /*----------------------------------------------------------------------
   * Symbol operations
   *----------------------------------------------------------------------*/

  void(JSI_ABI_CDECL *get_symbol_utf8)(
      jsi_runtime *rt,
      jsi_symbol sym,
      jsi_growable_buffer *buffer);

  /*----------------------------------------------------------------------
   * PropNameID operations
   *----------------------------------------------------------------------*/

  jsi_status(JSI_ABI_CDECL *create_propnameid_utf8)(
      jsi_runtime *rt,
      JSI_ABI_NULLABLE const char *value,
      size_t length,
      jsi_propnameid *out_propnameid);
  jsi_status(JSI_ABI_CDECL *create_propnameid_from_string)(
      jsi_runtime *rt,
      jsi_string str,
      jsi_propnameid *out_propnameid);
  jsi_status(JSI_ABI_CDECL *create_propnameid_from_symbol)(
      jsi_runtime *rt,
      jsi_symbol sym,
      jsi_propnameid *out_propnameid);
  bool(JSI_ABI_CDECL *propnameid_equals)(
      jsi_runtime *rt,
      jsi_propnameid a,
      jsi_propnameid b);
  void(JSI_ABI_CDECL *get_propnameid_utf8)(
      jsi_runtime *rt,
      jsi_propnameid name,
      jsi_growable_buffer *buffer);

  /*----------------------------------------------------------------------
   * Object operations
   *----------------------------------------------------------------------*/

  jsi_status(JSI_ABI_CDECL *create_object)(
      jsi_runtime *rt,
      jsi_object *out_object);
  jsi_status(JSI_ABI_CDECL *create_object_with_prototype)(
      jsi_runtime *rt,
      JSI_ABI_NULLABLE const jsi_value *prototype,
      jsi_object *out_object);

  /* Prototype access */
  jsi_status(JSI_ABI_CDECL *get_prototype_of)(
      jsi_runtime *rt,
      jsi_object obj,
      jsi_value *out_value);
  jsi_status(JSI_ABI_CDECL *set_prototype_of)(
      jsi_runtime *rt,
      jsi_object obj,
      const jsi_value *prototype);

  /* Property operations (PropNameID-based) */
  jsi_status(JSI_ABI_CDECL *has_object_property_from_propnameid)(
      jsi_runtime *rt,
      jsi_object obj,
      jsi_propnameid name,
      bool *out_result);
  jsi_status(JSI_ABI_CDECL *get_object_property_from_propnameid)(
      jsi_runtime *rt,
      jsi_object obj,
      jsi_propnameid name,
      jsi_value *out_value);
  jsi_status(JSI_ABI_CDECL *set_object_property_from_propnameid)(
      jsi_runtime *rt,
      jsi_object obj,
      jsi_propnameid name,
      const jsi_value *value);
  jsi_status(JSI_ABI_CDECL *delete_property_from_propnameid)(
      jsi_runtime *rt,
      jsi_object obj,
      jsi_propnameid name);

  /* Property operations (Value-based — key can be string, symbol, number) */
  jsi_status(JSI_ABI_CDECL *has_object_property_from_value)(
      jsi_runtime *rt,
      jsi_object obj,
      const jsi_value *key,
      bool *out_result);
  jsi_status(JSI_ABI_CDECL *get_object_property_from_value)(
      jsi_runtime *rt,
      jsi_object obj,
      const jsi_value *key,
      jsi_value *out_value);
  jsi_status(JSI_ABI_CDECL *set_object_property_from_value)(
      jsi_runtime *rt,
      jsi_object obj,
      const jsi_value *key,
      const jsi_value *value);
  jsi_status(JSI_ABI_CDECL *delete_property_from_value)(
      jsi_runtime *rt,
      jsi_object obj,
      const jsi_value *key);

  /// Get enumerable string property names.
  jsi_status(JSI_ABI_CDECL *get_object_property_names)(
      jsi_runtime *rt,
      jsi_object obj,
      jsi_array *out_array);

  /// External memory pressure hint for GC.
  jsi_status(JSI_ABI_CDECL *set_object_external_memory_pressure)(
      jsi_runtime *rt,
      jsi_object obj,
      size_t amount);

  /* Error object creation */
  jsi_status(JSI_ABI_CDECL *create_error_utf8)(
      jsi_runtime *rt,
      JSI_ABI_NULLABLE const char *message,
      size_t length,
      jsi_object *out_object);

  /*----------------------------------------------------------------------
   * Array operations
   *----------------------------------------------------------------------*/

  jsi_status(JSI_ABI_CDECL *create_array)(
      jsi_runtime *rt,
      size_t length,
      jsi_array *out_array);
  size_t(JSI_ABI_CDECL *get_array_length)(
      jsi_runtime *rt,
      jsi_array arr);
  jsi_status(JSI_ABI_CDECL *get_array_element)(
      jsi_runtime *rt,
      jsi_object arr,
      size_t index,
      jsi_value *out_value);
  jsi_status(JSI_ABI_CDECL *set_array_element)(
      jsi_runtime *rt,
      jsi_object arr,
      size_t index,
      const jsi_value *value);

  /*----------------------------------------------------------------------
   * ArrayBuffer operations
   *----------------------------------------------------------------------*/

  jsi_status(JSI_ABI_CDECL *create_arraybuffer)(
      jsi_runtime *rt,
      size_t byte_length,
      jsi_arraybuffer *out_arraybuffer);
  jsi_status(JSI_ABI_CDECL *create_arraybuffer_from_external_data)(
      jsi_runtime *rt,
      JSI_ABI_TRANSFER_FULL jsi_mutable_buffer *buffer,
      jsi_arraybuffer *out_arraybuffer);
  jsi_status(JSI_ABI_CDECL *get_arraybuffer_data)(
      jsi_runtime *rt,
      jsi_arraybuffer ab,
      uint8_t **out_data);
  jsi_status(JSI_ABI_CDECL *get_arraybuffer_size)(
      jsi_runtime *rt,
      jsi_arraybuffer ab,
      size_t *out_size);

  /*----------------------------------------------------------------------
   * Function operations
   *----------------------------------------------------------------------*/

  jsi_status(JSI_ABI_CDECL *call)(
      jsi_runtime *rt,
      jsi_function fn,
      JSI_ABI_NULLABLE const jsi_value *js_this,
      JSI_ABI_NULLABLE const jsi_value *args,
      size_t arg_count,
      jsi_value *out_value);
  jsi_status(JSI_ABI_CDECL *call_as_constructor)(
      jsi_runtime *rt,
      jsi_function fn,
      JSI_ABI_NULLABLE const jsi_value *args,
      size_t arg_count,
      jsi_value *out_value);
  jsi_status(JSI_ABI_CDECL *create_function_from_host_function)(
      jsi_runtime *rt,
      jsi_propnameid name,
      uint32_t param_count,
      JSI_ABI_TRANSFER_FULL jsi_host_function *hf,
      jsi_function *out_function);
  /// Returns NULL when fn is not a host function.
  jsi_host_function *(JSI_ABI_CDECL *get_host_function)(
      jsi_runtime *rt,
      jsi_function fn);

  /*----------------------------------------------------------------------
   * Host object operations
   *----------------------------------------------------------------------*/

  jsi_status(JSI_ABI_CDECL *create_object_from_host_object)(
      jsi_runtime *rt,
      JSI_ABI_TRANSFER_FULL jsi_host_object *ho,
      jsi_object *out_object);
  /// Returns NULL when obj is not a host object.
  jsi_host_object *(JSI_ABI_CDECL *get_host_object)(
      jsi_runtime *rt,
      jsi_object obj);

  /*----------------------------------------------------------------------
   * Native state
   *----------------------------------------------------------------------*/

  bool(JSI_ABI_CDECL *has_native_state)(
      jsi_runtime *rt,
      jsi_object obj);
  /// Returns NULL when obj has no native state.
  jsi_native_state *(JSI_ABI_CDECL *get_native_state)(
      jsi_runtime *rt,
      jsi_object obj);
  jsi_status(JSI_ABI_CDECL *set_native_state)(
      jsi_runtime *rt,
      jsi_object obj,
      JSI_ABI_NULLABLE JSI_ABI_TRANSFER_FULL jsi_native_state *ns);

  /*----------------------------------------------------------------------
   * Type checks
   *----------------------------------------------------------------------*/

  bool(JSI_ABI_CDECL *object_is_array)(
      jsi_runtime *rt,
      jsi_object obj);
  bool(JSI_ABI_CDECL *object_is_arraybuffer)(
      jsi_runtime *rt,
      jsi_object obj);
  bool(JSI_ABI_CDECL *object_is_function)(
      jsi_runtime *rt,
      jsi_object obj);

  /*----------------------------------------------------------------------
   * Weak references
   *----------------------------------------------------------------------*/

  jsi_status(JSI_ABI_CDECL *create_weak_object)(
      jsi_runtime *rt,
      jsi_object obj,
      jsi_weak_object *out_weak_object);
  jsi_value(JSI_ABI_CDECL *lock_weak_object)(
      jsi_runtime *rt,
      jsi_weak_object wo);

  /*----------------------------------------------------------------------
   * Comparison
   *----------------------------------------------------------------------*/

  jsi_status(JSI_ABI_CDECL *instance_of)(
      jsi_runtime *rt,
      jsi_object obj,
      jsi_function ctor,
      bool *out_result);
  bool(JSI_ABI_CDECL *strict_equals_symbol)(
      jsi_runtime *rt,
      jsi_symbol a,
      jsi_symbol b);
  bool(JSI_ABI_CDECL *strict_equals_bigint)(
      jsi_runtime *rt,
      jsi_bigint a,
      jsi_bigint b);
  bool(JSI_ABI_CDECL *strict_equals_string)(
      jsi_runtime *rt,
      jsi_string a,
      jsi_string b);
  bool(JSI_ABI_CDECL *strict_equals_object)(
      jsi_runtime *rt,
      jsi_object a,
      jsi_object b);

  /*----------------------------------------------------------------------
   * BigInt
   *----------------------------------------------------------------------*/

  jsi_status(JSI_ABI_CDECL *create_bigint_from_int64)(
      jsi_runtime *rt,
      int64_t value,
      jsi_bigint *out_bigint);
  jsi_status(JSI_ABI_CDECL *create_bigint_from_uint64)(
      jsi_runtime *rt,
      uint64_t value,
      jsi_bigint *out_bigint);
  bool(JSI_ABI_CDECL *bigint_is_int64)(
      jsi_runtime *rt,
      jsi_bigint bigint);
  bool(JSI_ABI_CDECL *bigint_is_uint64)(
      jsi_runtime *rt,
      jsi_bigint bigint);
  uint64_t(JSI_ABI_CDECL *bigint_truncate_to_uint64)(
      jsi_runtime *rt,
      jsi_bigint bigint);
  int64_t(JSI_ABI_CDECL *bigint_to_int64)(
      jsi_runtime *rt,
      jsi_bigint bigint,
      bool *out_lossless);
  uint64_t(JSI_ABI_CDECL *bigint_to_uint64)(
      jsi_runtime *rt,
      jsi_bigint bigint,
      bool *out_lossless);
  jsi_status(JSI_ABI_CDECL *bigint_to_string)(
      jsi_runtime *rt,
      jsi_bigint bigint,
      uint32_t radix,
      jsi_string *out_string);
};

/*==========================================================================
 * Runtime Instance
 *==========================================================================*/

/// A JSI runtime instance.
struct jsi_runtime {
  const jsi_runtime_api *api;
};

#ifdef __cplusplus
}
#endif

#endif /* JSI_ABI_H_ */
