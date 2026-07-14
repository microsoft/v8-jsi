// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

/// \file v8_core.cpp
/// \brief Implementation of shared V8 infrastructure.

#include "v8_core.h"

#include <cstdio>

#include "jsi_abi/v8_snapshot_container.h"

namespace v8rt {

//==============================================================================
// V8PlatformHolder Static Members
//==============================================================================

std::mutex V8PlatformHolder::mutex_;
std::unique_ptr<v8::Platform> V8PlatformHolder::platform_;
bool V8PlatformHolder::is_initialized_ = false;
bool V8PlatformHolder::is_disposed_ = false;

//==============================================================================
// Runtime Context Tag
//==============================================================================

int const RuntimeContextTag = 0x7e7f75;
void* const RuntimeContextTagPtr =
    const_cast<void*>(static_cast<const void*>(&RuntimeContextTag));

//==============================================================================
// Isolate Creation
//==============================================================================

v8::Isolate* createIsolate(const IsolateConfig& config) {
  v8::Isolate::CreateParams create_params;

  // Set up array buffer allocator
  create_params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();

  // Configure heap constraints if specified
  if (config.initial_heap_size > 0 || config.maximum_heap_size > 0) {
    v8::ResourceConstraints constraints;
    constraints.ConfigureDefaultsFromHeapSize(
        config.initial_heap_size,
        config.maximum_heap_size);
    create_params.constraints = constraints;
  }

  // Allocate the isolate up front so its IsolateData (which owns the stable
  // StartupData descriptor) can be created BEFORE Isolate::Initialize.
  v8::Isolate* isolate = v8::Isolate::Allocate();
  if (isolate == nullptr) {
    delete create_params.array_buffer_allocator;
    return nullptr;
  }

  IsolateData* isolate_data = new IsolateData(isolate, config.task_runner);

  // If a startup-snapshot blob was supplied, create the isolate from it so the
  // heap is materialized from the blob instead of the built-in startup data.
  // V8 deserializes contexts LAZILY and keeps the snapshot_blob pointer for the
  // isolate's lifetime, so it must point at storage that outlives Initialize —
  // hence IsolateData::snapshot_blob_ (the blob bytes are owned by the runtime).
  // external_references must be a non-null, null-terminated array matching the
  // one used at snapshot-creation time (v8_create_startup_snapshot); nullptr
  // makes V8 ignore the serialized context table.
  static const intptr_t kNoExternalRefs[] = {0};
  if (config.startup_snapshot_blob != nullptr &&
      config.startup_snapshot_blob_size > 0) {
    // The blob is a self-describing container (header + raw StartupData).
    // Validate it against THIS engine and strip the header before handing the
    // inner bytes to V8. On any mismatch, skip the snapshot and fall back to a
    // normal isolate — never feed V8 an incompatible blob (that crashes). This
    // is the engine's safety net; consumers that want to pick their own
    // fallback should pre-check via v8_startup_snapshot_compatible. V8 is
    // already initialized here (platform init precedes createIsolate), so the
    // version tag is read in its final state.
    const int compat = v8rt_snapshot::validate(
        config.startup_snapshot_blob, config.startup_snapshot_blob_size);
    if (compat == v8rt_snapshot::kOk) {
      isolate_data->snapshot_blob_.data = reinterpret_cast<const char*>(
          v8rt_snapshot::blobData(config.startup_snapshot_blob));
      isolate_data->snapshot_blob_.raw_size = static_cast<int>(
          v8rt_snapshot::blobSize(config.startup_snapshot_blob_size));
      create_params.snapshot_blob = &isolate_data->snapshot_blob_;
      create_params.external_references = kNoExternalRefs;
    } else {
      std::fprintf(stderr,
                   "[v8jsi] ignoring incompatible startup snapshot: %s\n",
                   v8rt_snapshot::compatString(compat));
    }
  }

  v8::Isolate::Initialize(isolate, create_params);

  isolate_data->attachToIsolate();

  // Configure microtasks policy
  isolate->SetMicrotasksPolicy(config.microtasks_policy);

  return isolate;
}

v8::Local<v8::Context> createContext(v8::Isolate* isolate, bool from_snapshot) {
  if (from_snapshot) {
    // Restore the context the snapshot serialized via AddContext at index 0,
    // which carries the global proxy and all baked-in globalThis state — so the
    // embedded script need not be re-run. (Node's main-context pattern.)
    v8::Local<v8::Context> ctx;
    if (v8::Context::FromSnapshot(isolate, 0).ToLocal(&ctx))
      return ctx;
    // Snapshot had no context at index 0 — fall back to a fresh context.
    v8::Local<v8::ObjectTemplate> fallback = v8::ObjectTemplate::New(isolate);
    return v8::Context::New(isolate, nullptr, fallback);
  }
  v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);
  return v8::Context::New(isolate, nullptr, global);
}

}  // namespace v8rt
