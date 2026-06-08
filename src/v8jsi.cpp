// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Minimal v8jsi implementation for prototype testing

#include "v8.h"
#include "libplatform/libplatform.h"

#ifdef _WIN32
#define V8JSI_EXPORT __declspec(dllexport)
#else
#define V8JSI_EXPORT __attribute__((visibility("default")))
#endif

namespace v8jsi {

// Simple version info export to verify the DLL is working
extern "C" V8JSI_EXPORT const char* v8jsi_version() {
  return "0.1.0-prototype";
}

// Export V8 version to verify V8 is linked correctly
extern "C" V8JSI_EXPORT const char* v8jsi_v8_version() {
  return v8::V8::GetVersion();
}

// Minimal test function that creates a V8 isolate
extern "C" V8JSI_EXPORT int v8jsi_test_v8() {
  // Create a new Isolate and make it the current one.
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();

  v8::Isolate* isolate = v8::Isolate::New(create_params);

  if (isolate == nullptr) {
    return -1;
  }

  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);

    // Create a context
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);

    // Simple test: evaluate "1 + 2"
    v8::Local<v8::String> source =
        v8::String::NewFromUtf8Literal(isolate, "1 + 2");

    v8::Local<v8::Script> script =
        v8::Script::Compile(context, source).ToLocalChecked();

    v8::Local<v8::Value> result = script->Run(context).ToLocalChecked();

    // Should be 3
    int32_t value = result->Int32Value(context).ToChecked();
    if (value != 3) {
      isolate->Dispose();
      delete create_params.array_buffer_allocator;
      return -2;
    }
  }

  isolate->Dispose();
  delete create_params.array_buffer_allocator;

  return 0; // Success
}

}  // namespace v8jsi
