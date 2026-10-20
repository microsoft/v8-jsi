// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// ApplySpawnConfig — lowers a decoded V8HostSpawnConfigV1 onto the broker's
// policy builder. Shared by the engine's real configure() (the product path) and
// its unit test, so the lowering is exercised by a deterministic in-process test
// instead of only the end-to-end smoke. It links the neutral spawn/worker-
// profile codec (:spawn_config) and the sbox.h config ABI, and nothing else.

#ifndef V8HOST_V8HOST_SPAWN_APPLY_H_
#define V8HOST_V8HOST_SPAWN_APPLY_H_

#include "sbox.h"
#include "v8host_spawn_config.h"

namespace v8host {

// Drive every applicable sbox_config_api setter from `spawn`: ACG, integrity,
// tokens, each file rule, AppContainer, each capability, and the effective
// engine-DLL allow; then encode the bound worker profile (V1 for a session, V2
// for an explicit worker) and set it as the opaque plugin_data. Every setter
// status is checked.
//
// Returns false (fail closed; the caller must not spawn) if `api`/`cfg` is null,
// any setter rejects its input, the effective tier and the armed ACG are
// inconsistent, or the worker-profile plugin_data is rejected.
//
// Consistency rule (design §8.6 + the retained acg_enabled relationship): the
// jitless choice must match the armed ACG. jitless is derived from the effective
// tier — only kTierTrusted yields JIT; every other value is jitless (the most
// restrictive default). A trusted (JIT) effective tier therefore requires ACG
// off (a trusted-under-ACG config is rejected) and, symmetrically, a jitless
// tier requires ACG on, so the worker's post-lockdown ACG proof always matches
// the encoded jitless flag.
bool ApplySpawnConfig(const V8HostSpawnConfigV1& spawn,
                      const sbox_config_api* api,
                      sbox_config cfg);

}  // namespace v8host

#endif  // V8HOST_V8HOST_SPAWN_APPLY_H_
