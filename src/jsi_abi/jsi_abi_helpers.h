/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT license.
 *
 * Portions derived from facebook/hermes (Hermes ABI):
 *   Copyright (c) Meta Platforms, Inc. and affiliates.
 *   Licensed under the MIT license.
 *
 * JSI ABI Helpers — C++ inline helpers for creating handles and values and
 * checking value kinds.
 *
 * Same role as Hermes' HermesABIHelpers.h but with jsi_* naming.
 * No X-macros — every type and helper spelled out explicitly.
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

#ifndef JSI_ABI_HELPERS_H
#define JSI_ABI_HELPERS_H

#include "jsi_abi.h"

#include <cassert>

namespace jsi::abi {

/*==========================================================================
 * Handle constructors
 *
 * Each pointer type gets an explicit helper (no X-macros).
 *==========================================================================*/

/* --- Object --- */

inline jsi_object create_object(jsi_pointer *ptr) {
  return {ptr};
}

/* --- Array --- */

inline jsi_array create_array(jsi_pointer *ptr) {
  return {ptr};
}

/* --- String --- */

inline jsi_string create_string(jsi_pointer *ptr) {
  return {ptr};
}

/* --- BigInt --- */

inline jsi_bigint create_bigint(jsi_pointer *ptr) {
  return {ptr};
}

/* --- Symbol --- */

inline jsi_symbol create_symbol(jsi_pointer *ptr) {
  return {ptr};
}

/* --- Function --- */

inline jsi_function create_function(jsi_pointer *ptr) {
  return {ptr};
}

/* --- ArrayBuffer --- */

inline jsi_arraybuffer create_arraybuffer(jsi_pointer *ptr) {
  return {ptr};
}

/* --- PropNameID --- */

inline jsi_propnameid create_propnameid(jsi_pointer *ptr) {
  return {ptr};
}

/* --- WeakObject --- */

inline jsi_weak_object create_weak_object(jsi_pointer *ptr) {
  return {ptr};
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
  return v.kind & JSI_ABI_POINTER_MASK;
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
  mp->api->invalidate(mp);
}
inline void release_value(const jsi_value &v) {
  if (is_pointer_value(v))
    v.data.pointer->api->invalidate(v.data.pointer);
}

} // namespace jsi::abi

#endif /* JSI_ABI_HELPERS_H */
