/*
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT license.
 *
 * Portions derived from facebook/hermes (Hermes ABI):
 *   Copyright (c) Meta Platforms, Inc. and affiliates.
 *   Licensed under the MIT license.
 *
 * JsiAbiRuntime — a facebook::jsi::Runtime implementation backed by the
 * JSI ABI C interface. Engine-agnostic; compiled on the consumer side.
 *
 * The implementation class lives entirely in JsiAbiRuntime.cpp. Consumers
 * see only the factory + accessor functions declared here.
 */

/* ===========================================================================
 * EXPERIMENTAL API - NOT YET ABI-STABLE
 *
 * This is the consumer-compiled C++ wrapper over the experimental JSI ABI. It
 * tracks the ABI's stability: while the ABI is in experimental mode this header
 * is subject to change and is NOT ABI-safe. Do not depend on binary
 * compatibility across builds. The notice will be removed once the underlying
 * ABI leaves experimental mode.
 * =========================================================================== */

#ifndef JSI_ABI_RUNTIME_H
#define JSI_ABI_RUNTIME_H

#include "jsi_abi/jsi_abi.h"

#include <jsi/jsi.h>

#include <memory>

namespace jsi::abi {

/// The engine's flat runtime-creation export, as a function-pointer type.
/// Each engine provides one (e.g. v8_create_runtime). Takes the consumer's
/// compiled JSI_ABI_VERSION + a configure callback/context; returns a runtime
/// with refcount 1, or nullptr on version mismatch / configure failure.
using jsi_create_runtime_fn = jsi_runtime *(JSI_CDECL *)(
    uint32_t requested_abi_version,
    jsi_configure_runtime_cb configure,
    void *configure_data);

/// Create a new JSI runtime via the engine's flat create function. This wrapper
/// is compiled into the consumer, so it injects the consumer's JSI_ABI_VERSION
/// for free (Node-API Model B).
/// \param create_runtime The engine's create export (e.g. v8_create_runtime).
/// \param configure Optional callback that populates the factory-owned config
///   via engine-specific setters (e.g. v8_jsi_config_set_*); nullptr = defaults.
/// \param configure_data Opaque context passed through to configure.
/// \return A facebook::jsi::Runtime that owns one ref on the new runtime.
std::unique_ptr<facebook::jsi::Runtime> makeJsiAbiRuntime(
    jsi_create_runtime_fn create_runtime,
    jsi_configure_runtime_cb configure = nullptr,
    void *configure_data = nullptr);

/// Wrap an existing jsi_runtime. The returned wrapper holds its own ref
/// (add_ref is called on entry), so the caller's ref is independent.
///
/// Used when a runtime created via jsr_create_runtime (the Node-API entry
/// point) also needs to be driven through facebook::jsi::Runtime — the same
/// V8 isolate exposed through both surfaces. Retrieve the jsi_runtime with
/// jsr_runtime_get_jsi_runtime, then pass it here.
std::unique_ptr<facebook::jsi::Runtime> wrapJsiRuntime(jsi_runtime *abiRuntime);

/// Retrieve the underlying jsi_runtime pointer from a facebook::jsi::Runtime
/// that was created by one of the factories above. The returned pointer is
/// borrowed (no add_ref); it stays alive as long as the wrapping Runtime does.
///
/// Intended for test hooks and advanced consumers that need to call
/// ABI-specific extensions (e.g., v8_open_inspector). Returns nullptr if
/// the supplied Runtime was not created by this factory.
jsi_runtime *getAbiRuntime(facebook::jsi::Runtime &runtime) noexcept;

} // namespace jsi::abi

#endif /* JSI_ABI_RUNTIME_H */
