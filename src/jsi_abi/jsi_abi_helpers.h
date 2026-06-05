/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT license.
 *
 * Portions derived from facebook/hermes (Hermes ABI):
 *   Copyright (c) Meta Platforms, Inc. and affiliates.
 *   Licensed under the MIT license.
 *
 * JSI ABI Helpers — C++ inline helpers for encoding/decoding OrError types,
 * creating values, and checking value kinds.
 *
 * Same role as Hermes' HermesABIHelpers.h but with jsi_* naming.
 * No X-macros — every type and helper spelled out explicitly.
 */

/* ===========================================================================
 * EXPERIMENTAL API - NOT YET ABI-STABLE
 *
 * This API is experimental and subject to change. Although it is C-based, it is
 * NOT ABI-safe yet: function signatures, vtable layouts, struct fields, enum
 * values, and error codes may change without notice while the API remains in
 * experimental mode. Do not depend on binary compatibility across builds. Once
 * the API leaves experimental mode it will become ABI-stable and this notice
 * will be removed.
 * =========================================================================== */

#ifndef JSI_ABI_HELPERS_H
#define JSI_ABI_HELPERS_H

#include "jsi_abi.h"

#include <cassert>

namespace jsi::abi {

/* --- jsi_error_code helpers (for void-returning operations) --- */

inline bool is_error(jsi_error_code err) {
  return err != jsi_no_error;
}
inline jsi_error_code get_error(jsi_error_code err) {
  assert(is_error(err));
  return err;
}
/* Identity for jsi_error_code. Same signature shape as the create_*_or_error
 * helpers, so template code that wraps an error into a result type via a
 * function-pointer hook can use it as a no-op wrap when the result type
 * already IS jsi_error_code. */
inline jsi_error_code identity_error(jsi_error_code err) {
  return err;
}

/*==========================================================================
 * OrError helpers (bit-packing)
 *
 * Error encoding for pointer types:
 *   low bit = 1 means error, error code = bits >> 2
 *   low bit = 0 means success, pointer value in remaining bits
 *
 * Each pointer type gets explicit helper functions (no X-macros).
 *==========================================================================*/

/* --- Object --- */

inline jsi_object create_object(jsi_pointer *ptr) {
  return {ptr};
}
inline jsi_object_or_error create_object_or_error(jsi_pointer *ptr) {
  return {reinterpret_cast<uintptr_t>(ptr)};
}
inline jsi_object_or_error create_object_or_error(jsi_error_code err) {
  return {static_cast<uintptr_t>(err)};
}
inline bool is_error(const jsi_object_or_error &p) {
  return p.ptr_or_error & 1;
}
inline jsi_error_code get_error(const jsi_object_or_error &p) {
  assert(is_error(p));
  return static_cast<jsi_error_code>(p.ptr_or_error);
}
inline jsi_object get_object(jsi_object_or_error p) {
  assert(!is_error(p));
  return create_object(reinterpret_cast<jsi_pointer *>(p.ptr_or_error));
}

/* --- Array --- */

inline jsi_array create_array(jsi_pointer *ptr) {
  return {ptr};
}
inline jsi_array_or_error create_array_or_error(jsi_pointer *ptr) {
  return {reinterpret_cast<uintptr_t>(ptr)};
}
inline jsi_array_or_error create_array_or_error(jsi_error_code err) {
  return {static_cast<uintptr_t>(err)};
}
inline bool is_error(const jsi_array_or_error &p) {
  return p.ptr_or_error & 1;
}
inline jsi_error_code get_error(const jsi_array_or_error &p) {
  assert(is_error(p));
  return static_cast<jsi_error_code>(p.ptr_or_error);
}
inline jsi_array get_array(jsi_array_or_error p) {
  assert(!is_error(p));
  return create_array(reinterpret_cast<jsi_pointer *>(p.ptr_or_error));
}

/* --- String --- */

inline jsi_string create_string(jsi_pointer *ptr) {
  return {ptr};
}
inline jsi_string_or_error create_string_or_error(jsi_pointer *ptr) {
  return {reinterpret_cast<uintptr_t>(ptr)};
}
inline jsi_string_or_error create_string_or_error(jsi_error_code err) {
  return {static_cast<uintptr_t>(err)};
}
inline bool is_error(const jsi_string_or_error &p) {
  return p.ptr_or_error & 1;
}
inline jsi_error_code get_error(const jsi_string_or_error &p) {
  assert(is_error(p));
  return static_cast<jsi_error_code>(p.ptr_or_error);
}
inline jsi_string get_string(jsi_string_or_error p) {
  assert(!is_error(p));
  return create_string(reinterpret_cast<jsi_pointer *>(p.ptr_or_error));
}

/* --- BigInt --- */

inline jsi_bigint create_bigint(jsi_pointer *ptr) {
  return {ptr};
}
inline jsi_bigint_or_error create_bigint_or_error(jsi_pointer *ptr) {
  return {reinterpret_cast<uintptr_t>(ptr)};
}
inline jsi_bigint_or_error create_bigint_or_error(jsi_error_code err) {
  return {static_cast<uintptr_t>(err)};
}
inline bool is_error(const jsi_bigint_or_error &p) {
  return p.ptr_or_error & 1;
}
inline jsi_error_code get_error(const jsi_bigint_or_error &p) {
  assert(is_error(p));
  return static_cast<jsi_error_code>(p.ptr_or_error);
}
inline jsi_bigint get_bigint(jsi_bigint_or_error p) {
  assert(!is_error(p));
  return create_bigint(reinterpret_cast<jsi_pointer *>(p.ptr_or_error));
}

/* --- Symbol --- */

inline jsi_symbol create_symbol(jsi_pointer *ptr) {
  return {ptr};
}
inline jsi_symbol_or_error create_symbol_or_error(jsi_pointer *ptr) {
  return {reinterpret_cast<uintptr_t>(ptr)};
}
inline jsi_symbol_or_error create_symbol_or_error(jsi_error_code err) {
  return {static_cast<uintptr_t>(err)};
}
inline bool is_error(const jsi_symbol_or_error &p) {
  return p.ptr_or_error & 1;
}
inline jsi_error_code get_error(const jsi_symbol_or_error &p) {
  assert(is_error(p));
  return static_cast<jsi_error_code>(p.ptr_or_error);
}
inline jsi_symbol get_symbol(jsi_symbol_or_error p) {
  assert(!is_error(p));
  return create_symbol(reinterpret_cast<jsi_pointer *>(p.ptr_or_error));
}

/* --- Function --- */

inline jsi_function create_function(jsi_pointer *ptr) {
  return {ptr};
}
inline jsi_function_or_error create_function_or_error(jsi_pointer *ptr) {
  return {reinterpret_cast<uintptr_t>(ptr)};
}
inline jsi_function_or_error create_function_or_error(jsi_error_code err) {
  return {static_cast<uintptr_t>(err)};
}
inline bool is_error(const jsi_function_or_error &p) {
  return p.ptr_or_error & 1;
}
inline jsi_error_code get_error(const jsi_function_or_error &p) {
  assert(is_error(p));
  return static_cast<jsi_error_code>(p.ptr_or_error);
}
inline jsi_function get_function(jsi_function_or_error p) {
  assert(!is_error(p));
  return create_function(reinterpret_cast<jsi_pointer *>(p.ptr_or_error));
}

/* --- ArrayBuffer --- */

inline jsi_arraybuffer create_arraybuffer(jsi_pointer *ptr) {
  return {ptr};
}
inline jsi_arraybuffer_or_error create_arraybuffer_or_error(jsi_pointer *ptr) {
  return {reinterpret_cast<uintptr_t>(ptr)};
}
inline jsi_arraybuffer_or_error create_arraybuffer_or_error(
    jsi_error_code err) {
  return {static_cast<uintptr_t>(err)};
}
inline bool is_error(const jsi_arraybuffer_or_error &p) {
  return p.ptr_or_error & 1;
}
inline jsi_error_code get_error(const jsi_arraybuffer_or_error &p) {
  assert(is_error(p));
  return static_cast<jsi_error_code>(p.ptr_or_error);
}
inline jsi_arraybuffer get_arraybuffer(jsi_arraybuffer_or_error p) {
  assert(!is_error(p));
  return create_arraybuffer(reinterpret_cast<jsi_pointer *>(p.ptr_or_error));
}

/* --- PropNameID --- */

inline jsi_propnameid create_propnameid(jsi_pointer *ptr) {
  return {ptr};
}
inline jsi_propnameid_or_error create_propnameid_or_error(jsi_pointer *ptr) {
  return {reinterpret_cast<uintptr_t>(ptr)};
}
inline jsi_propnameid_or_error create_propnameid_or_error(jsi_error_code err) {
  return {static_cast<uintptr_t>(err)};
}
inline bool is_error(const jsi_propnameid_or_error &p) {
  return p.ptr_or_error & 1;
}
inline jsi_error_code get_error(const jsi_propnameid_or_error &p) {
  assert(is_error(p));
  return static_cast<jsi_error_code>(p.ptr_or_error);
}
inline jsi_propnameid get_propnameid(jsi_propnameid_or_error p) {
  assert(!is_error(p));
  return create_propnameid(reinterpret_cast<jsi_pointer *>(p.ptr_or_error));
}

/* --- WeakObject --- */

inline jsi_weak_object create_weak_object(jsi_pointer *ptr) {
  return {ptr};
}
inline jsi_weak_object_or_error create_weak_object_or_error(jsi_pointer *ptr) {
  return {reinterpret_cast<uintptr_t>(ptr)};
}
inline jsi_weak_object_or_error create_weak_object_or_error(
    jsi_error_code err) {
  return {static_cast<uintptr_t>(err)};
}
inline bool is_error(const jsi_weak_object_or_error &p) {
  return p.ptr_or_error & 1;
}
inline jsi_error_code get_error(const jsi_weak_object_or_error &p) {
  assert(is_error(p));
  return static_cast<jsi_error_code>(p.ptr_or_error);
}
inline jsi_weak_object get_weak_object(jsi_weak_object_or_error p) {
  assert(!is_error(p));
  return create_weak_object(reinterpret_cast<jsi_pointer *>(p.ptr_or_error));
}

/* --- PreparedJavaScript ---
 * Pointer-based handle: get_prepared_javascript returns the raw pointer
 * (not a wrapper struct) because jsi_prepared_javascript is itself the
 * handle type. */

inline jsi_prepared_javascript_or_error create_prepared_javascript_or_error(
    jsi_prepared_javascript *ptr) {
  return {reinterpret_cast<uintptr_t>(ptr)};
}
inline jsi_prepared_javascript_or_error create_prepared_javascript_or_error(
    jsi_error_code err) {
  return {static_cast<uintptr_t>(err)};
}
inline bool is_error(const jsi_prepared_javascript_or_error &p) {
  return p.ptr_or_error & 1;
}
inline jsi_error_code get_error(const jsi_prepared_javascript_or_error &p) {
  assert(is_error(p));
  return static_cast<jsi_error_code>(p.ptr_or_error);
}
inline jsi_prepared_javascript *get_prepared_javascript(
    jsi_prepared_javascript_or_error p) {
  assert(!is_error(p));
  return reinterpret_cast<jsi_prepared_javascript *>(p.ptr_or_error);
}

/*==========================================================================
 * BoolOrError / SizeTOrError / Uint8PtrOrError helpers
 *==========================================================================*/

/* --- BoolOrError --- */

inline jsi_bool_or_error create_bool_or_error(bool val) {
  return {val ? 2u : 0u};
}
inline jsi_bool_or_error create_bool_or_error(jsi_error_code err) {
  return {static_cast<uintptr_t>(err)};
}
inline bool is_error(const jsi_bool_or_error &p) {
  return p.bool_or_error & 1;
}
inline jsi_error_code get_error(const jsi_bool_or_error &p) {
  assert(is_error(p));
  return static_cast<jsi_error_code>(p.bool_or_error);
}
inline bool get_bool(const jsi_bool_or_error &p) {
  assert(!is_error(p));
  return p.bool_or_error != 0;
}

/* --- SizeTOrError --- */

inline jsi_size_or_error create_size_or_error(size_t val) {
  jsi_size_or_error r;
  r.is_error = false;
  r.data.val = val;
  return r;
}
inline jsi_size_or_error create_size_or_error(jsi_error_code err) {
  jsi_size_or_error r;
  r.is_error = true;
  r.data.error = err;
  return r;
}
inline bool is_error(const jsi_size_or_error &r) {
  return r.is_error;
}
inline jsi_error_code get_error(const jsi_size_or_error &r) {
  assert(is_error(r));
  return r.data.error;
}
inline size_t get_size(const jsi_size_or_error &r) {
  assert(!is_error(r));
  return r.data.val;
}

/* --- Uint8PtrOrError --- */

inline jsi_uint8_ptr_or_error create_uint8_ptr_or_error(uint8_t *val) {
  jsi_uint8_ptr_or_error r;
  r.is_error = false;
  r.data.val = val;
  return r;
}
inline jsi_uint8_ptr_or_error create_uint8_ptr_or_error(jsi_error_code err) {
  jsi_uint8_ptr_or_error r;
  r.is_error = true;
  r.data.error = err;
  return r;
}
inline bool is_error(const jsi_uint8_ptr_or_error &r) {
  return r.is_error;
}
inline jsi_error_code get_error(const jsi_uint8_ptr_or_error &r) {
  assert(is_error(r));
  return r.data.error;
}
inline uint8_t *get_uint8_ptr(const jsi_uint8_ptr_or_error &r) {
  assert(!is_error(r));
  return r.data.val;
}

/* --- PropNameIDListPtrOrError --- */

inline jsi_propnameid_list_or_error create_propnameid_list_or_error(
    jsi_propnameid_list *ptr) {
  return {reinterpret_cast<uintptr_t>(ptr)};
}
inline jsi_propnameid_list_or_error create_propnameid_list_or_error(
    jsi_error_code err) {
  return {static_cast<uintptr_t>(err)};
}
inline bool is_error(const jsi_propnameid_list_or_error &p) {
  return p.ptr_or_error & 1;
}
inline jsi_error_code get_error(const jsi_propnameid_list_or_error &p) {
  assert(is_error(p));
  return static_cast<jsi_error_code>(p.ptr_or_error);
}
inline jsi_propnameid_list *get_propnameid_list(
    jsi_propnameid_list_or_error p) {
  assert(!is_error(p));
  return reinterpret_cast<jsi_propnameid_list *>(p.ptr_or_error);
}

/*==========================================================================
 * Value helpers
 *==========================================================================*/

/* --- Value constructors --- */

inline jsi_value create_undefined_value() {
  jsi_value v;
  v.kind = jsi_valuekind_undefined;
  return v;
}
inline jsi_value create_null_value() {
  jsi_value v;
  v.kind = jsi_valuekind_null;
  return v;
}
inline jsi_value create_bool_value(bool b) {
  jsi_value v;
  v.kind = jsi_valuekind_boolean;
  v.data.boolean = b;
  return v;
}
inline jsi_value create_number_value(double d) {
  jsi_value v;
  v.kind = jsi_valuekind_number;
  v.data.number = d;
  return v;
}
inline jsi_value create_object_value(jsi_pointer *ptr) {
  jsi_value v;
  v.kind = jsi_valuekind_object;
  v.data.pointer = ptr;
  return v;
}
inline jsi_value create_object_value(const jsi_object &obj) {
  return create_object_value(obj.pointer);
}
inline jsi_value create_string_value(jsi_pointer *ptr) {
  jsi_value v;
  v.kind = jsi_valuekind_string;
  v.data.pointer = ptr;
  return v;
}
inline jsi_value create_string_value(const jsi_string &str) {
  return create_string_value(str.pointer);
}
inline jsi_value create_symbol_value(jsi_pointer *ptr) {
  jsi_value v;
  v.kind = jsi_valuekind_symbol;
  v.data.pointer = ptr;
  return v;
}
inline jsi_value create_symbol_value(const jsi_symbol &sym) {
  return create_symbol_value(sym.pointer);
}
inline jsi_value create_bigint_value(jsi_pointer *ptr) {
  jsi_value v;
  v.kind = jsi_valuekind_bigint;
  v.data.pointer = ptr;
  return v;
}
inline jsi_value create_bigint_value(const jsi_bigint &bigint) {
  return create_bigint_value(bigint.pointer);
}

/* --- Value type checks --- */

inline jsi_valuekind get_value_kind(const jsi_value &v) {
  return v.kind;
}
inline bool is_pointer_value(const jsi_value &v) {
  return v.kind & JSI_POINTER_MASK;
}
inline bool is_undefined_value(const jsi_value &v) {
  return v.kind == jsi_valuekind_undefined;
}
inline bool is_null_value(const jsi_value &v) {
  return v.kind == jsi_valuekind_null;
}
inline bool is_bool_value(const jsi_value &v) {
  return v.kind == jsi_valuekind_boolean;
}
inline bool is_number_value(const jsi_value &v) {
  return v.kind == jsi_valuekind_number;
}
inline bool is_object_value(const jsi_value &v) {
  return v.kind == jsi_valuekind_object;
}
inline bool is_string_value(const jsi_value &v) {
  return v.kind == jsi_valuekind_string;
}
inline bool is_symbol_value(const jsi_value &v) {
  return v.kind == jsi_valuekind_symbol;
}
inline bool is_bigint_value(const jsi_value &v) {
  return v.kind == jsi_valuekind_bigint;
}

/* --- Value extraction --- */

inline bool get_bool_value(const jsi_value &v) {
  assert(is_bool_value(v));
  return v.data.boolean;
}
inline double get_number_value(const jsi_value &v) {
  assert(is_number_value(v));
  return v.data.number;
}
inline jsi_object get_object_value(const jsi_value &v) {
  assert(is_object_value(v));
  return {v.data.pointer};
}
inline jsi_string get_string_value(const jsi_value &v) {
  assert(is_string_value(v));
  return {v.data.pointer};
}
inline jsi_symbol get_symbol_value(const jsi_value &v) {
  assert(is_symbol_value(v));
  return {v.data.pointer};
}
inline jsi_bigint get_bigint_value(const jsi_value &v) {
  assert(is_bigint_value(v));
  return {v.data.pointer};
}
inline jsi_pointer *get_pointer_value(const jsi_value &v) {
  assert(is_pointer_value(v));
  return v.data.pointer;
}

/* --- Cleanup --- */

inline void release_pointer(jsi_pointer *mp) {
  mp->vtable->invalidate(mp);
}
inline void release_value(const jsi_value &v) {
  if (is_pointer_value(v))
    v.data.pointer->vtable->invalidate(v.data.pointer);
}

/*==========================================================================
 * ValueOrError helpers
 *==========================================================================*/

inline jsi_value_or_error create_value_or_error(jsi_value val) {
  return {val};
}
inline jsi_value_or_error create_value_or_error(jsi_error_code err) {
  jsi_value_or_error r;
  r.value.kind = jsi_valuekind_error;
  r.value.data.error = err;
  return r;
}
inline bool is_error(const jsi_value_or_error &v) {
  return v.value.kind == jsi_valuekind_error;
}
inline jsi_error_code get_error(const jsi_value_or_error &v) {
  assert(is_error(v));
  return v.value.data.error;
}
inline jsi_value get_value(const jsi_value_or_error &v) {
  assert(!is_error(v));
  return v.value;
}

} // namespace jsi::abi

#endif /* JSI_ABI_HELPERS_H */
