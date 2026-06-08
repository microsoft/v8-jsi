// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

/// \file v8_core.cpp
/// \brief Implementation of shared V8 infrastructure.

#include "v8_core.h"

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

  // Allocate and initialize the isolate
  v8::Isolate* isolate = v8::Isolate::Allocate();
  if (isolate == nullptr) {
    delete create_params.array_buffer_allocator;
    return nullptr;
  }

  v8::Isolate::Initialize(isolate, create_params);

  // Create and attach IsolateData
  IsolateData* isolate_data = new IsolateData(isolate, config.task_runner);
  isolate_data->attachToIsolate();

  // Configure microtasks policy
  isolate->SetMicrotasksPolicy(config.microtasks_policy);

  return isolate;
}

v8::Local<v8::Context> createContext(v8::Isolate* isolate) {
  v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);
  return v8::Context::New(isolate, nullptr, global);
}

}  // namespace v8rt
