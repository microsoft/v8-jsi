// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

using namespace napitest;

TEST_P(NapiTest, test_basics_jsr_config) {
  // Just to see that jsr_config related functions compile
  jsr_config config{};
  jsr_create_config(&config);
  jsr_config_enable_inspector(config, true);
  jsr_config_set_inspector_runtime_name(config, "Test");
  jsr_config_set_inspector_port(config, 9229);
  jsr_config_set_inspector_break_on_start(config, true);
  jsr_config_enable_gc_api(config, true);
  jsr_config_set_task_runner(
      config,
      nullptr,
      [](void * /*task_runner_data*/,
         void * /*task_data*/,
         jsr_task_run_cb /*task_run_cb*/,
         jsr_data_delete_cb /*task_data_delete_cb*/,
         void * /*deleter_data*/
      ) {},
      [](void * /*data*/, void * /*deleter_data*/) {},
      nullptr);
  jsr_config_set_script_cache(
      config,
      nullptr,
      [](void * /*script_cache_data*/,
         const char * /*source_url*/,
         uint64_t /*source_hash*/,
         const char * /*runtime_name*/,
         uint64_t /*runtime_version*/,
         const char * /*cache_tag*/,
         const uint8_t ** /*buffer*/,
         size_t * /*buffer_size*/,
         jsr_data_delete_cb * /*buffer_delete_cb*/,
         void ** /*deleter_data*/) {},
      [](void * /*script_cache_data*/,
         const char * /*source_url*/,
         uint64_t /*source_hash*/,
         const char * /*runtime_name*/,
         uint64_t /*runtime_version*/,
         const char * /*cache_tag*/,
         const uint8_t * /*buffer*/,
         size_t /*buffer_size*/,
         jsr_data_delete_cb /*buffer_delete_cb*/,
         void * /*deleter_data*/) {},
      [](void * /*data*/, void * /*deleter_data*/) {},
      nullptr);
  jsr_delete_config(config);
}

TEST_P(NapiTest, test_basics_CreateStringLatin1) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value str;
    napi_create_string_latin1(env, "Hello", NAPI_AUTO_LENGTH, &str);

    size_t strSize;
    napi_get_value_string_latin1(env, str, nullptr, 0, &strSize);
    ASSERT_EQ(strSize, 5);

    char strBuf[6];
    napi_get_value_string_latin1(env, str, strBuf, 6, nullptr);
    ASSERT_STREQ(strBuf, "Hello");
  });
}

TEST_P(NapiTest, test_basics_CreateStringLatin1_2) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value str;
    napi_create_string_latin1(env, "Hello\xC0", NAPI_AUTO_LENGTH, &str);

    size_t strSize;
    napi_get_value_string_latin1(env, str, nullptr, 0, &strSize);
    ASSERT_EQ(strSize, 6);

    char strBuf[7];
    napi_get_value_string_latin1(env, str, strBuf, 7, nullptr);
    ASSERT_STREQ(strBuf, "Hello\xC0");
  });
}

TEST_P(NapiTest, test_basics_CreateStringUTF8) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value str;
    napi_create_string_utf8(env, "Hello\xd0\x81", NAPI_AUTO_LENGTH, &str);

    size_t strSize;
    napi_get_value_string_utf8(env, str, nullptr, 0, &strSize);
    ASSERT_EQ(strSize, 7);

    char strBuf[8];
    napi_get_value_string_utf8(env, str, strBuf, 8, nullptr);
    ASSERT_STREQ(strBuf, "Hello\xd0\x81");
  });
}

TEST_P(NapiTest, test_basics_CreateStringUTF16) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value str;
    napi_create_string_utf16(env, u"Hello\x0401", NAPI_AUTO_LENGTH, &str);

    size_t strSize;
    napi_get_value_string_utf16(env, str, nullptr, 0, &strSize);
    ASSERT_EQ(strSize, 6);

    char16_t strBuf[7];
    napi_get_value_string_utf16(env, str, strBuf, 7, nullptr);
    ASSERT_STREQ(reinterpret_cast<wchar_t *>(strBuf), L"Hello\x0401");
  });
}

TEST_P(NapiTest, test_basics_RunScript) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value script;
    ASSERT_EQ(napi_ok, napi_create_string_utf8(env, "myVar = \"Hello\"", NAPI_AUTO_LENGTH, &script));

    napi_value scriptResult;
    ASSERT_EQ(napi_ok, jsr_run_script(env, script, nullptr, &scriptResult));

    napi_value global;
    ASSERT_EQ(napi_ok, napi_get_global(env, &global));

    napi_value value;
    ASSERT_EQ(napi_ok, napi_get_named_property(env, global, "myVar", &value));

    napi_valuetype valueType;
    ASSERT_EQ(napi_ok, napi_typeof(env, value, &valueType));

    ASSERT_EQ(napi_string, valueType);

    size_t strSize;
    ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env, value, nullptr, 0, &strSize));
    ASSERT_EQ(strSize, 5);

    char strBuf[6];
    ASSERT_EQ(napi_ok, napi_get_value_string_utf8(env, value, strBuf, 6, nullptr));
    ASSERT_STREQ(strBuf, "Hello");
  });
}

TEST_P(NapiTest, test_basics_HostFunctionTest) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    bool isCalled{false};
    napi_value func;
    ASSERT_EQ(
        napi_ok,
        napi_create_function(
            env,
            "Foo",
            NAPI_AUTO_LENGTH,
            [](napi_env env, napi_callback_info cbInfo) -> napi_value {
              napi_value result{};
              napi_create_int32(env, 5, &result);
              return result;
            },
            &isCalled,
            &func));

    napi_value global{};
    ASSERT_EQ(napi_ok, napi_get_global(env, &global));

    ASSERT_EQ(napi_ok, napi_set_named_property(env, global, "Foo", func));

    napi_value script;
    ASSERT_EQ(napi_ok, napi_create_string_utf8(env, "Foo()", NAPI_AUTO_LENGTH, &script));

    napi_value scriptResult;
    ASSERT_EQ(napi_ok, napi_run_script(env, script, &scriptResult));

    napi_valuetype scriptResultType;
    ASSERT_EQ(napi_ok, napi_typeof(env, scriptResult, &scriptResultType));

    ASSERT_EQ(scriptResultType, napi_number);

    int32_t intResult;
    ASSERT_EQ(napi_ok, napi_get_value_int32(env, scriptResult, &intResult));
    ASSERT_EQ(intResult, 5);
  });
}

TEST_P(NapiTest, test_basics_PropertyNamedGetSetTest) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value obj{};
    ASSERT_EQ(napi_ok, napi_create_object(env, &obj));

    napi_value intValue{};
    ASSERT_EQ(napi_ok, napi_create_int32(env, 5, &intValue));

    ASSERT_EQ(napi_ok, napi_set_named_property(env, obj, "test", intValue));

    napi_value intResult{};
    ASSERT_EQ(napi_ok, napi_get_named_property(env, obj, "test", &intResult));

    napi_valuetype valueType{};
    ASSERT_EQ(napi_ok, napi_typeof(env, intResult, &valueType));
    ASSERT_EQ(valueType, napi_number);

    int32_t i32Value{};
    ASSERT_EQ(napi_ok, napi_get_value_int32(env, intResult, &i32Value));
    ASSERT_EQ(i32Value, 5);
  });
}

TEST_P(NapiTest, test_basics_PropertyStringGetSetTest) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value obj{};
    ASSERT_EQ(napi_ok, napi_create_object(env, &obj));

    napi_value propName{};
    ASSERT_EQ(napi_ok, napi_create_string_latin1(env, "test", NAPI_AUTO_LENGTH, &propName));

    napi_valuetype propType{};
    ASSERT_EQ(napi_ok, napi_typeof(env, propName, &propType));
    ASSERT_EQ(propType, napi_string);

    napi_value intValue{};
    ASSERT_EQ(napi_ok, napi_create_int32(env, 5, &intValue));

    ASSERT_EQ(napi_ok, napi_set_property(env, obj, propName, intValue));

    napi_value intResult{};
    ASSERT_EQ(napi_ok, napi_get_property(env, obj, propName, &intResult));

    napi_valuetype valueType{};
    ASSERT_EQ(napi_ok, napi_typeof(env, intResult, &valueType));
    ASSERT_EQ(valueType, napi_number);

    int32_t i32Value{};
    ASSERT_EQ(napi_ok, napi_get_value_int32(env, intResult, &i32Value));
    ASSERT_EQ(i32Value, 5);
  });
}

TEST_P(NapiTest, test_basics_CreateStringType) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value str{};
    ASSERT_EQ(napi_ok, napi_create_string_latin1(env, "test", NAPI_AUTO_LENGTH, &str));

    napi_valuetype strType{};
    ASSERT_EQ(napi_ok, napi_typeof(env, str, &strType));
    ASSERT_EQ(strType, napi_string);
  });
}

TEST_P(NapiTest, test_basics_CreateSymbolByScript) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value script;
    ASSERT_EQ(napi_ok, napi_create_string_utf8(env, "sym1 = Symbol('Hello')", NAPI_AUTO_LENGTH, &script));

    napi_value scriptResult;
    ASSERT_EQ(napi_ok, jsr_run_script(env, script, nullptr, &scriptResult));

    napi_value global;
    ASSERT_EQ(napi_ok, napi_get_global(env, &global));

    napi_value value;
    ASSERT_EQ(napi_ok, napi_get_named_property(env, global, "sym1", &value));

    napi_valuetype valueType;
    ASSERT_EQ(napi_ok, napi_typeof(env, value, &valueType));
    ASSERT_EQ(valueType, napi_symbol);
  });
}

TEST_P(NapiTest, test_basics_CreateSymbol) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value symDesc{};
    ASSERT_EQ(napi_ok, napi_create_string_latin1(env, "test", NAPI_AUTO_LENGTH, &symDesc));

    napi_value sym{};
    ASSERT_EQ(napi_ok, napi_create_symbol(env, symDesc, &sym));

    napi_valuetype symType{};
    ASSERT_EQ(napi_ok, napi_typeof(env, sym, &symType));
    ASSERT_EQ(symType, napi_symbol);
  });
}

TEST_P(NapiTest, test_basics_PropertySymbolGetSetTest) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value obj{};
    ASSERT_EQ(napi_ok, napi_create_object(env, &obj));

    napi_value symDesc{};
    ASSERT_EQ(napi_ok, napi_create_string_latin1(env, "test", NAPI_AUTO_LENGTH, &symDesc));

    napi_value propSym{};
    ASSERT_EQ(napi_ok, napi_create_symbol(env, symDesc, &propSym));

    napi_valuetype propType{};
    ASSERT_EQ(napi_ok, napi_typeof(env, propSym, &propType));
    ASSERT_EQ(propType, napi_symbol);

    napi_value intValue{};
    ASSERT_EQ(napi_ok, napi_create_int32(env, 5, &intValue));

    ASSERT_EQ(napi_ok, napi_set_property(env, obj, propSym, intValue));

    napi_value intResult{};
    ASSERT_EQ(napi_ok, napi_get_property(env, obj, propSym, &intResult));

    napi_valuetype valueType{};
    ASSERT_EQ(napi_ok, napi_typeof(env, intResult, &valueType));
    ASSERT_EQ(valueType, napi_number);

    int32_t i32Value{};
    ASSERT_EQ(napi_ok, napi_get_value_int32(env, intResult, &i32Value));
    ASSERT_EQ(i32Value, 5);

    // All newly created symbols are unique even if they have the same name.
    napi_value propSym2{};
    ASSERT_EQ(napi_ok, napi_create_symbol(env, symDesc, &propSym2));

    napi_value undefResult{};
    ASSERT_EQ(napi_ok, napi_get_property(env, obj, propSym2, &undefResult));

    napi_valuetype undefType{};
    ASSERT_EQ(napi_ok, napi_typeof(env, undefResult, &undefType));
    ASSERT_EQ(undefType, napi_undefined);
  });
}

TEST_P(NapiTest, test_basics_ExternalValueTest) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value external{};
    std::unique_ptr<int> data = std::make_unique<int>(5);
    ASSERT_EQ(napi_ok, napi_create_external(env, data.get(), nullptr, nullptr, &external));

    napi_valuetype valueType{};
    ASSERT_EQ(napi_ok, napi_typeof(env, external, &valueType));
    ASSERT_EQ(valueType, napi_external);
  });
}

TEST_P(NapiTest, test_basics_ExternalValue2Test) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    auto native_ptr = std::make_unique<int>(5);

    napi_value external{};
    ASSERT_EQ(napi_ok, napi_create_external(env, native_ptr.get(), nullptr, nullptr, &external));

    napi_valuetype valueType{};
    ASSERT_EQ(napi_ok, napi_typeof(env, external, &valueType));
    ASSERT_EQ(valueType, napi_external);

    void *ext;
    ASSERT_EQ(napi_ok, napi_get_value_external(env, external, &ext));
    ASSERT_EQ(ext, native_ptr.get());
  });
}

TEST_P(NapiTest, test_basics_ExternalValue3Test) {
  bool finalizeRan{};

  ExecuteNapi([&](NapiTestContext * /*testContext*/, napi_env env) {
    auto native_ptr = std::make_unique<int>(5);

    napi_value external{};
    ASSERT_EQ(
        napi_ok,
        napi_create_external(
            env,
            native_ptr.get(),
            [](napi_env env, void *nativeData, void *finalizeHint) { *static_cast<bool *>(finalizeHint) = true; },
            &finalizeRan,
            &external));

    napi_valuetype valueType{};
    ASSERT_EQ(napi_ok, napi_typeof(env, external, &valueType));
    ASSERT_EQ(valueType, napi_external);

    void *ext;
    ASSERT_EQ(napi_ok, napi_get_value_external(env, external, &ext));
    ASSERT_EQ(ext, native_ptr.get());
  });

  ASSERT_TRUE(finalizeRan);
}

TEST_P(NapiTest, test_basics_ExternalValue4Test) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    auto native_ptr = std::make_unique<int>(5);
    bool finalizeRan{};

    napi_handle_scope scope{};
    ASSERT_EQ(napi_ok, napi_open_handle_scope(env, &scope));

    napi_value external{};
    ASSERT_EQ(
        napi_ok,
        napi_create_external(
            env,
            native_ptr.get(),
            [](napi_env env, void *nativeData, void *finalizeHint) { *static_cast<bool *>(finalizeHint) = true; },
            &finalizeRan,
            &external));

    ASSERT_FALSE(finalizeRan);
    ASSERT_EQ(napi_ok, napi_close_handle_scope(env, scope));
    ASSERT_EQ(napi_ok, jsr_collect_garbage(env));
    ASSERT_TRUE(finalizeRan);
  });
}

#if 0
TEST_P(NapiTest, test_basics_ExternalValue5Test) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    auto native_ptr = std::make_unique<int>(5);
    bool finalizeRan{};

    {
      napi_handle_scope scope{};
      ASSERT_EQ(napi_ok, napi_open_handle_scope(env, &scope));

      napi_value external{};
      ASSERT_EQ(
          napi_ok,
          napi_create_external(
              env,
              native_ptr.get(),
              [](napi_env env, void *nativeData, void *finalizeHint) { *static_cast<bool *>(finalizeHint) = true; },
              &finalizeRan,
              &external));

      napi_ref ref;
      ASSERT_EQ(napi_ok, napi_create_reference(env, external, 1, &ref));

      ASSERT_FALSE(finalizeRan);
      ASSERT_EQ(napi_ok, napi_close_handle_scope(env, scope));
    }
    ASSERT_EQ(napi_ok, jsr_collect_garbage(env));
    ASSERT_FALSE(finalizeRan);
  });
}
#endif

TEST_P(NapiTest, test_basics_DateValueTest) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value date{};
    ASSERT_EQ(napi_ok, napi_create_date(env, 123, &date));

    bool isDate{};
    ASSERT_EQ(napi_ok, napi_is_date(env, date, &isDate));
    ASSERT_EQ(isDate, true);

    double innerValue{};
    ASSERT_EQ(napi_ok, napi_get_date_value(env, date, &innerValue));
    ASSERT_EQ(innerValue, 123);
  });
}

TEST_P(NapiTest, test_basics_DateValueNaNTest) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value date{};
    ASSERT_EQ(napi_ok, napi_create_date(env, std::numeric_limits<double>::quiet_NaN(), &date));

    bool isDate{};
    ASSERT_EQ(napi_ok, napi_is_date(env, date, &isDate));
    ASSERT_EQ(isDate, true);

    double innerValue{};
    ASSERT_EQ(napi_ok, napi_get_date_value(env, date, &innerValue));
    ASSERT_TRUE(std::isnan(innerValue));
  });
}

TEST_P(NapiTest, test_basics_PromisePropertyTest) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value global{};
    ASSERT_EQ(napi_ok, napi_get_global(env, &global));

    napi_value promiseCtor{};
    ASSERT_EQ(napi_ok, napi_get_named_property(env, global, "Promise", &promiseCtor));

    napi_valuetype promiseValueType;
    ASSERT_EQ(napi_ok, napi_typeof(env, promiseCtor, &promiseValueType));

    ASSERT_EQ(promiseValueType, napi_function);
  });
}

TEST_P(NapiTest, test_basics_NewInstanceTest) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value script;
    ASSERT_EQ(napi_ok, napi_create_string_utf8(env, "function MyClass() { this.x = 42; }", NAPI_AUTO_LENGTH, &script));

    napi_value scriptResult;
    ASSERT_EQ(napi_ok, jsr_run_script(env, script, nullptr, &scriptResult));

    napi_value global;
    ASSERT_EQ(napi_ok, napi_get_global(env, &global));

    napi_value myClassCtor;
    ASSERT_EQ(napi_ok, napi_get_named_property(env, global, "MyClass", &myClassCtor));

    napi_valuetype classType;
    ASSERT_EQ(napi_ok, napi_typeof(env, myClassCtor, &classType));
    ASSERT_EQ(classType, napi_function);

    napi_value myClassInstance;
    ASSERT_EQ(napi_ok, napi_new_instance(env, myClassCtor, 0, nullptr, &myClassInstance));

    napi_valuetype instType;
    ASSERT_EQ(napi_ok, napi_typeof(env, myClassInstance, &instType));
    ASSERT_EQ(napi_object, instType);

    bool isInstOf;
    ASSERT_EQ(napi_ok, napi_instanceof(env, myClassInstance, myClassCtor, &isInstOf));
    ASSERT_TRUE(isInstOf);

    napi_value xValue;
    ASSERT_EQ(napi_ok, napi_get_named_property(env, myClassInstance, "x", &xValue));

    napi_valuetype valueType;
    ASSERT_EQ(napi_ok, napi_typeof(env, xValue, &valueType));
    ASSERT_EQ(napi_number, valueType);

    int32_t value;
    ASSERT_EQ(napi_ok, napi_get_value_int32(env, xValue, &value));
    ASSERT_EQ(value, 42);
  });
}

TEST_P(NapiTest, test_basics_ArrayTest) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value arr;
    ASSERT_EQ(napi_ok, napi_create_array(env, &arr));

    bool hasElement;
    ASSERT_EQ(napi_ok, napi_has_element(env, arr, 1, &hasElement));
    ASSERT_EQ(hasElement, false);

    napi_value elementValue;
    ASSERT_EQ(napi_ok, napi_get_element(env, arr, 1, &elementValue));

    napi_valuetype elementType;
    ASSERT_EQ(napi_ok, napi_typeof(env, elementValue, &elementType));
    ASSERT_EQ(elementType, napi_undefined);

    napi_value value42;
    ASSERT_EQ(napi_ok, napi_create_int32(env, 42, &value42));

    ASSERT_EQ(napi_ok, napi_set_element(env, arr, 1, value42));

    ASSERT_EQ(napi_ok, napi_has_element(env, arr, 1, &hasElement));
    ASSERT_EQ(hasElement, true);

    ASSERT_EQ(napi_ok, napi_get_element(env, arr, 1, &elementValue));
    ASSERT_EQ(napi_ok, napi_typeof(env, elementValue, &elementType));
    ASSERT_EQ(elementType, napi_number);

    uint32_t uintValue;
    ASSERT_EQ(napi_ok, napi_get_value_uint32(env, elementValue, &uintValue));
    ASSERT_EQ(uintValue, 42);

    ASSERT_EQ(napi_ok, napi_delete_element(env, arr, 1, nullptr));

    ASSERT_EQ(napi_ok, napi_has_element(env, arr, 1, &hasElement));
    ASSERT_EQ(hasElement, false);

    ASSERT_EQ(napi_ok, napi_get_element(env, arr, 1, &elementValue));

    ASSERT_EQ(napi_ok, napi_typeof(env, elementValue, &elementType));
    ASSERT_EQ(elementType, napi_undefined);
  });
}

TEST_P(NapiTest, test_basics_Finalizer) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    auto native_ptr = std::make_unique<int>(5);
    bool finalizeRan{};
    {
      napi_handle_scope scope{};
      ASSERT_EQ(napi_ok, napi_open_handle_scope(env, &scope));

      napi_value object{};
      ASSERT_EQ(napi_ok, napi_create_object(env, &object));
      ASSERT_EQ(
          napi_ok,
          napi_add_finalizer(
              env,
              object,
              native_ptr.get(),
              [](napi_env env, void *nativeData, void *finalizeHint) { *static_cast<bool *>(finalizeHint) = true; },
              &finalizeRan,
              nullptr));

      ASSERT_FALSE(finalizeRan);
      ASSERT_EQ(napi_ok, napi_close_handle_scope(env, scope));
    }
    ASSERT_EQ(napi_ok, jsr_collect_garbage(env));
    ASSERT_TRUE(finalizeRan);
  });
}

TEST_P(NapiTest, test_basics_Wrap) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    auto native_ptr = std::make_unique<int>(5);
    bool finalizeRan{};
    {
      napi_handle_scope scope{};
      ASSERT_EQ(napi_ok, napi_open_handle_scope(env, &scope));

      napi_value object{};
      ASSERT_EQ(napi_ok, napi_create_object(env, &object));
      ASSERT_EQ(
          napi_ok,
          napi_wrap(
              env,
              object,
              native_ptr.get(),
              [](napi_env env, void *nativeData, void *finalizeHint) { *static_cast<bool *>(finalizeHint) = true; },
              &finalizeRan,
              nullptr));

      ASSERT_FALSE(finalizeRan);
      ASSERT_EQ(napi_ok, napi_close_handle_scope(env, scope));
    }
    ASSERT_EQ(napi_ok, jsr_collect_garbage(env));
    ASSERT_TRUE(finalizeRan);
  });
}

TEST_P(NapiTest, test_basics_Wrap2) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    bool finalizeRan{};
    {
      napi_handle_scope scope{};
      ASSERT_EQ(napi_ok, napi_open_handle_scope(env, &scope));

      napi_value object{};
      ASSERT_EQ(napi_ok, napi_create_object(env, &object));

      struct MyClass {
        MyClass(napi_env env) : env(env) {}
        ~MyClass() {
          napi_delete_reference(env, wrapper);
        }
        napi_env env{};
        napi_ref wrapper{};
      };

      MyClass *myc = new MyClass(env);

      ASSERT_EQ(
          napi_ok,
          napi_wrap(
              env,
              object,
              myc,
              [](napi_env env, void *nativeData, void *finalizeHint) {
                *static_cast<bool *>(finalizeHint) = true;
                delete reinterpret_cast<MyClass *>(nativeData);
              },
              &finalizeRan,
              &myc->wrapper));

      ASSERT_FALSE(finalizeRan);

      // The object is kept alive by the active handle scope.
      ASSERT_EQ(napi_ok, jsr_collect_garbage(env));
      ASSERT_FALSE(finalizeRan);

      ASSERT_EQ(napi_ok, napi_close_handle_scope(env, scope));
    }
    ASSERT_EQ(napi_ok, jsr_collect_garbage(env));
    ASSERT_TRUE(finalizeRan);
  });
}

TEST_P(NapiTest, test_basics_BigIntWords) {
  ExecuteNapi([](NapiTestContext * /*testContext*/, napi_env env) {
    napi_value script;
    ASSERT_EQ(
        napi_ok,
        napi_create_string_utf8(
            env,
            R"JS(
              v01 = 0n;
              v02 = -0n;
              v03 = 1n;
              v04 = -1n;
              v05 = 986583n;
              v06 = -976675n;
              v07 = 98765432213456789876546896323445679887645323232436587988766545658n;
              v08 = -4350987086545760976737453646576078997096876957864353245245769809n;
            )JS",
            NAPI_AUTO_LENGTH,
            &script));

    napi_value scriptResult;
    ASSERT_EQ(napi_ok, napi_run_script(env, script, &scriptResult));

    napi_value global;
    ASSERT_EQ(napi_ok, napi_get_global(env, &global));

    napi_value v01, v02, v03, v04, v05, v06, v07, v08;
    ASSERT_EQ(napi_ok, napi_get_named_property(env, global, "v01", &v01));
    ASSERT_EQ(napi_ok, napi_get_named_property(env, global, "v02", &v02));
    ASSERT_EQ(napi_ok, napi_get_named_property(env, global, "v03", &v03));
    ASSERT_EQ(napi_ok, napi_get_named_property(env, global, "v04", &v04));
    ASSERT_EQ(napi_ok, napi_get_named_property(env, global, "v05", &v05));
    ASSERT_EQ(napi_ok, napi_get_named_property(env, global, "v06", &v06));
    ASSERT_EQ(napi_ok, napi_get_named_property(env, global, "v07", &v07));
    ASSERT_EQ(napi_ok, napi_get_named_property(env, global, "v08", &v08));

    int32_t sign_bit{};
    uint64_t words[8] = {};
    size_t word_count = std::size(words);
    ASSERT_EQ(napi_ok, napi_get_value_bigint_words(env, v01, &sign_bit, &word_count, words));
    EXPECT_EQ(0, sign_bit);
    EXPECT_EQ(0, word_count);

    word_count = std::size(words);
    ASSERT_EQ(napi_ok, napi_get_value_bigint_words(env, v02, &sign_bit, &word_count, words));
    EXPECT_EQ(0, sign_bit);
    EXPECT_EQ(0, word_count);

    word_count = std::size(words);
    ASSERT_EQ(napi_ok, napi_get_value_bigint_words(env, v03, &sign_bit, &word_count, words));
    EXPECT_EQ(0, sign_bit);
    EXPECT_EQ(1, word_count);
    EXPECT_EQ(1, words[0]);

    word_count = std::size(words);
    ASSERT_EQ(napi_ok, napi_get_value_bigint_words(env, v04, &sign_bit, &word_count, words));
    EXPECT_EQ(1, sign_bit);
    EXPECT_EQ(1, word_count);
    EXPECT_EQ(1, words[0]);

    word_count = std::size(words);
    ASSERT_EQ(napi_ok, napi_get_value_bigint_words(env, v05, &sign_bit, &word_count, words));
    EXPECT_EQ(0, sign_bit);
    EXPECT_EQ(1, word_count);
    EXPECT_EQ(986583, words[0]);

    word_count = std::size(words);
    ASSERT_EQ(napi_ok, napi_get_value_bigint_words(env, v06, &sign_bit, &word_count, words));
    ASSERT_EQ(1, sign_bit);
    EXPECT_EQ(1, word_count);
    EXPECT_EQ(976675, words[0]);

    word_count = std::size(words);
    ASSERT_EQ(napi_ok, napi_get_value_bigint_words(env, v07, &sign_bit, &word_count, words));
    ASSERT_EQ(0, sign_bit);
    EXPECT_EQ(4, word_count);
    EXPECT_EQ(1081160290208438010, words[0]);
    EXPECT_EQ(6353857413966178407, words[1]);
    EXPECT_EQ(2137532229865108125, words[2]);
    EXPECT_EQ(15734241, words[3]);

    word_count = std::size(words);
    ASSERT_EQ(napi_ok, napi_get_value_bigint_words(env, v08, &sign_bit, &word_count, words));
    ASSERT_EQ(1, sign_bit);
    EXPECT_EQ(4, word_count);
    EXPECT_EQ(2249227439184578641, words[0]);
    EXPECT_EQ(8929859034951574840, words[1]);
    EXPECT_EQ(4303658240311279431, words[2]);
    EXPECT_EQ(693152, words[3]);
  });
}
