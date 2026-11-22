// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include <cstdio>

#ifdef V8JSI_PROBE_RUNNER
#include <windows.h>

int main() {
  auto dll = LoadLibraryExW(L"v8jsi_legacy_probe.dll", nullptr,
                            LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll) return 1;
  using Probe = int (*)();
  auto probe = reinterpret_cast<Probe>(GetProcAddress(dll, "v8jsi_legacy_engine_probe"));
  int result = probe ? probe() : 1;
  FreeLibrary(dll);
  return result;
}
#else
#include <memory>
#include <libplatform/libplatform.h>
#include <v8.h>

extern "C" __declspec(dllexport) int v8jsi_legacy_engine_probe() {
  auto platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  if (!v8::V8::Initialize()) return 1;
  auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = allocator.get();
  auto isolate = v8::Isolate::New(params);
  int status = 1;
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handles(isolate);
    auto context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    auto source = v8::String::NewFromUtf8Literal(isolate, "1+2");
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> result;
    if (v8::Script::Compile(context, source).ToLocal(&script) &&
        script->Run(context).ToLocal(&result) && result->IsInt32() &&
        result.As<v8::Int32>()->Value() == 3) {
      std::printf("V8 %s: 1+2 = 3\n", v8::V8::GetVersion());
      status = 0;
    }
  }
  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  return status;
}
#endif
