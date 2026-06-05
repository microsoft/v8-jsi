// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// Tests for v8jsi.dll - loads the DLL and tests V8 functionality

#include "gtest/gtest.h"
#include "v8.h"
#include "libplatform/libplatform.h"

#include <memory>

#ifdef _WIN32
#include <windows.h>
#define V8JSI_IMPORT __declspec(dllimport)
#else
#define V8JSI_IMPORT
#endif

// Import functions from v8jsi.dll
extern "C" {
  V8JSI_IMPORT const char* v8jsi_version();
  V8JSI_IMPORT const char* v8jsi_v8_version();
  V8JSI_IMPORT int v8jsi_test_v8();
}

class V8JsiTest : public ::testing::Test {
 protected:
  static std::unique_ptr<v8::Platform> platform_;

  static void SetUpTestSuite() {
    // Initialize V8 platform once for all tests
    platform_ = v8::platform::NewDefaultPlatform();
    v8::V8::InitializePlatform(platform_.get());
    v8::V8::Initialize();
  }

  static void TearDownTestSuite() {
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
    platform_.reset();
  }
};

std::unique_ptr<v8::Platform> V8JsiTest::platform_;

// Test that we can get the v8jsi version
TEST_F(V8JsiTest, GetVersion) {
  const char* version = v8jsi_version();
  ASSERT_NE(version, nullptr);
  EXPECT_STRNE(version, "");
  std::cout << "v8jsi version: " << version << std::endl;
}

// Test that we can get the V8 version from the DLL
TEST_F(V8JsiTest, GetV8Version) {
  const char* v8_version = v8jsi_v8_version();
  ASSERT_NE(v8_version, nullptr);
  EXPECT_STRNE(v8_version, "");
  std::cout << "V8 version: " << v8_version << std::endl;
}

// Test basic V8 functionality through the DLL
TEST_F(V8JsiTest, BasicV8Test) {
  int result = v8jsi_test_v8();
  EXPECT_EQ(result, 0) << "v8jsi_test_v8() failed with code: " << result;
}

// Test creating an isolate directly
TEST_F(V8JsiTest, CreateIsolate) {
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();

  v8::Isolate* isolate = v8::Isolate::New(create_params);
  ASSERT_NE(isolate, nullptr);

  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    ASSERT_FALSE(context.IsEmpty());
  }

  isolate->Dispose();
  delete create_params.array_buffer_allocator;
}

// Test running JavaScript code
TEST_F(V8JsiTest, RunJavaScript) {
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();

  v8::Isolate* isolate = v8::Isolate::New(create_params);
  ASSERT_NE(isolate, nullptr);

  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);

    // Test simple expression
    v8::Local<v8::String> source =
        v8::String::NewFromUtf8Literal(isolate, "40 + 2");
    v8::Local<v8::Script> script =
        v8::Script::Compile(context, source).ToLocalChecked();
    v8::Local<v8::Value> result = script->Run(context).ToLocalChecked();

    EXPECT_TRUE(result->IsNumber());
    EXPECT_EQ(42, result->Int32Value(context).ToChecked());
  }

  isolate->Dispose();
  delete create_params.array_buffer_allocator;
}

// Test creating and calling a JavaScript function
TEST_F(V8JsiTest, CallJavaScriptFunction) {
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();

  v8::Isolate* isolate = v8::Isolate::New(create_params);
  ASSERT_NE(isolate, nullptr);

  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);

    // Create a function
    v8::Local<v8::String> source = v8::String::NewFromUtf8Literal(
        isolate, "(function(a, b) { return a + b; })");
    v8::Local<v8::Script> script =
        v8::Script::Compile(context, source).ToLocalChecked();
    v8::Local<v8::Value> func_value = script->Run(context).ToLocalChecked();

    EXPECT_TRUE(func_value->IsFunction());
    v8::Local<v8::Function> func = v8::Local<v8::Function>::Cast(func_value);

    // Call the function with arguments
    v8::Local<v8::Value> args[2] = {
        v8::Number::New(isolate, 10),
        v8::Number::New(isolate, 32)
    };
    v8::Local<v8::Value> result =
        func->Call(context, context->Global(), 2, args).ToLocalChecked();

    EXPECT_TRUE(result->IsNumber());
    EXPECT_EQ(42, result->Int32Value(context).ToChecked());
  }

  isolate->Dispose();
  delete create_params.array_buffer_allocator;
}

// Test string handling
TEST_F(V8JsiTest, StringHandling) {
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();

  v8::Isolate* isolate = v8::Isolate::New(create_params);
  ASSERT_NE(isolate, nullptr);

  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);

    // Test string concatenation
    v8::Local<v8::String> source = v8::String::NewFromUtf8Literal(
        isolate, "'Hello, ' + 'World!'");
    v8::Local<v8::Script> script =
        v8::Script::Compile(context, source).ToLocalChecked();
    v8::Local<v8::Value> result = script->Run(context).ToLocalChecked();

    EXPECT_TRUE(result->IsString());
    v8::String::Utf8Value utf8(isolate, result);
    EXPECT_STREQ(*utf8, "Hello, World!");
  }

  isolate->Dispose();
  delete create_params.array_buffer_allocator;
}
