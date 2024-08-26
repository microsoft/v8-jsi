/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// These tests are adopted from the Hermes API tests

#include <jsi/test/testlib.h>

#include <gtest/gtest.h>
#include <jsi/decorator.h>
#include <jsi/jsi.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace facebook::jsi;

class JSITestExt : public JSITestBase {};

// TODO: figure out how to fix it for V8
#if 0
TEST_P(JSITestExt, StrictHostFunctionBindTest) {
  Function coolify = Function::createFromHostFunction(
      rt,
      PropNameID::forAscii(rt, "coolify"),
      0,
      [](Runtime&, const Value& thisVal, const Value* args, size_t count) {
        EXPECT_TRUE(thisVal.isUndefined());
        return thisVal.isUndefined();
      });
  rt.global().setProperty(rt, "coolify", coolify);
  EXPECT_TRUE(eval("(function() {"
                   "  \"use strict\";"
                   "  return coolify.bind(undefined)();"
                   "})()")
                  .getBool());
}
#endif

TEST_P(JSITestExt, DescriptionTest) {
  // Description is not empty
  EXPECT_NE(rt.description().size(), 0);
}

TEST_P(JSITestExt, ArrayBufferTest) {
  eval(
      "var buffer = new ArrayBuffer(16);\
        var int32View = new Int32Array(buffer);\
        int32View[0] = 1234;\
        int32View[1] = 5678;");

  Object object = rt.global().getPropertyAsObject(rt, "buffer");
  EXPECT_TRUE(object.isArrayBuffer(rt));

  auto arrayBuffer = object.getArrayBuffer(rt);
  EXPECT_EQ(arrayBuffer.size(rt), 16);

  int32_t* buffer = reinterpret_cast<int32_t*>(arrayBuffer.data(rt));
  EXPECT_EQ(buffer[0], 1234);
  EXPECT_EQ(buffer[1], 5678);
}

#if JSI_VERSION >= 9
#ifndef JSI_V8_IMPL
TEST_P(JSITestExt, ExternalArrayBufferTest) {
  struct FixedBuffer : MutableBuffer {
    size_t size() const override {
      return sizeof(arr);
    }
    uint8_t* data() override {
      return reinterpret_cast<uint8_t*>(arr.data());
    }

    std::array<uint32_t, 256> arr;
  };

  {
    auto buf = std::make_shared<FixedBuffer>();
    for (uint32_t i = 0; i < buf->arr.size(); i++)
      buf->arr[i] = i;
    auto arrayBuffer = ArrayBuffer(rt, buf);
    auto square = eval(
        R"#(
(function (buf) {
  var view = new Uint32Array(buf);
  for(var i = 0; i < view.length; i++) view[i] = view[i] * view[i];
})
)#");
    square.asObject(rt).asFunction(rt).call(rt, arrayBuffer);
    for (uint32_t i = 0; i < 256; i++)
      EXPECT_EQ(buf->arr[i], i * i);
  }
}
#endif

TEST_P(JSITestExt, NoCorruptionOnJSError) {
  // If the test crashes or infinite loops, the likely cause is that
  // Hermes API library is not built with proper compiler flags
  // (-fexception in GCC/CLANG, /EHsc in MSVC)
  try {
    rt.evaluateJavaScript(std::make_unique<StringBuffer>("foo.bar = 1"), "");
    FAIL() << "Expected JSIException";
  } catch (const facebook::jsi::JSIException&) {
    // expected exception, ignore
  }
  try {
    rt.evaluateJavaScript(std::make_unique<StringBuffer>("foo.baz = 1"), "");
    FAIL() << "Expected JSIException";
  } catch (const facebook::jsi::JSIException&) {
    // expected exception, ignore
  }
  rt.evaluateJavaScript(std::make_unique<StringBuffer>("gc()"), "");
}

#if !defined(JSI_V8_IMPL)
TEST_P(JSITestExt, SpreadHostObjectWithOwnProperties) {
  class HostObjectWithPropertyNames : public HostObject {
    std::vector<PropNameID> getPropertyNames(Runtime& rt) override {
      return PropNameID::names(rt, "prop1", "1", "2", "prop2", "3");
    }
    Value get(Runtime& runtime, const PropNameID& name) override {
      return Value();
    }
  };

  Object ho = Object::createFromHostObject(
      rt, std::make_shared<HostObjectWithPropertyNames>());
  rt.global().setProperty(rt, "ho", ho);

  auto res = eval(R"###(
var spreaded = {...ho};
var props = Object.getOwnPropertyNames(spreaded);
props.toString();
)###")
                 .getString(rt)
                 .utf8(rt);
  EXPECT_EQ(res, "1,2,3,prop1,prop2");
}

TEST_P(JSITestExt, HostObjectWithOwnProperties) {
  class HostObjectWithPropertyNames : public HostObject {
    std::vector<PropNameID> getPropertyNames(Runtime& rt) override {
      return PropNameID::names(rt, "prop1", "1", "2", "prop2", "3");
    }
    Value get(Runtime& runtime, const PropNameID& name) override {
      if (PropNameID::compare(
              runtime, name, PropNameID::forAscii(runtime, "prop1")))
        return 10;
      return Value();
    }
  };

  Object ho = Object::createFromHostObject(
      rt, std::make_shared<HostObjectWithPropertyNames>());
  rt.global().setProperty(rt, "ho", ho);

  EXPECT_TRUE(eval("\"prop1\" in ho").getBool());
  EXPECT_TRUE(eval("1 in ho").getBool());
  EXPECT_TRUE(eval("2 in ho").getBool());
  EXPECT_TRUE(eval("\"prop2\" in ho").getBool());
  EXPECT_TRUE(eval("3 in ho").getBool());
  // HostObjects say they own any property, even if it's not in their property
  // names list.
  // This is an explicit design choice, to avoid the runtime and API costs of
  // handling checking for property existence.
  EXPECT_TRUE(eval("\"foo\" in ho").getBool());

  EXPECT_TRUE(eval("var properties = Object.getOwnPropertyNames(ho);"
                   "properties[0] === '1' && "
                   "properties[1] === '2' && "
                   "properties[2] === '3' && "
                   "properties[3] === 'prop1' && "
                   "properties[4] === 'prop2' && "
                   "properties.length === 5")
                  .getBool());
  EXPECT_TRUE(eval("ho[2] === undefined").getBool());
  EXPECT_TRUE(eval("ho.prop2 === undefined").getBool());

  eval("Object.defineProperty(ho, '0', {value: 'hi there'})");
  eval("Object.defineProperty(ho, '2', {value: 'hi there'})");
  eval("Object.defineProperty(ho, '4', {value: 'hi there'})");
  eval("Object.defineProperty(ho, 'prop2', {value: 'hi there'})");

  EXPECT_TRUE(eval("var properties = Object.getOwnPropertyNames(ho);"
                   "properties[0] === '0' && "
                   "properties[1] === '1' && "
                   "properties[2] === '2' && "
                   "properties[3] === '3' && "
                   "properties[4] === '4' && "
                   "properties[5] === 'prop2' && "
                   "properties[6] === 'prop1' && "
                   "properties.length === 7")
                  .getBool());
  EXPECT_TRUE(eval("ho[2] === 'hi there'").getBool());
  EXPECT_TRUE(eval("ho.prop2 === 'hi there'").getBool());

  // hasOwnProperty() always succeeds on HostObject
  EXPECT_TRUE(
      eval("Object.prototype.hasOwnProperty.call(ho, 'prop1')").getBool());
  EXPECT_TRUE(
      eval("Object.prototype.hasOwnProperty.call(ho, 'any-string')").getBool());

  // getOwnPropertyDescriptor() always succeeds on HostObject
  EXPECT_TRUE(eval("var d = Object.getOwnPropertyDescriptor(ho, 'prop1');"
                   "d != undefined && "
                   "d.value == 10 && "
                   "d.enumerable && "
                   "d.writable ")
                  .getBool());
  EXPECT_TRUE(eval("var d = Object.getOwnPropertyDescriptor(ho, 'any-string');"
                   "d != undefined && "
                   "d.value == undefined && "
                   "d.enumerable && "
                   "d.writable")
                  .getBool());
}
#endif

TEST_P(JSITestExt, HostObjectAsParentTest) {
  class HostObjectWithProp : public HostObject {
    Value get(Runtime& runtime, const PropNameID& name) override {
      if (PropNameID::compare(
              runtime, name, PropNameID::forAscii(runtime, "prop1")))
        return 10;
      return Value();
    }
  };

  Object ho =
      Object::createFromHostObject(rt, std::make_shared<HostObjectWithProp>());
  rt.global().setProperty(rt, "ho", ho);

  EXPECT_TRUE(
      eval("var subClass = {__proto__: ho}; subClass.prop1 == 10;").getBool());
}

#if JSI_VERSION >= 5
TEST_P(JSITestExt, PropNameIDFromSymbol) {
  auto strProp = PropNameID::forAscii(rt, "a");
  auto secretProp = PropNameID::forSymbol(
      rt, eval("var secret = Symbol('a'); secret;").getSymbol(rt));
  auto globalProp =
      PropNameID::forSymbol(rt, eval("Symbol.for('a');").getSymbol(rt));
  auto x =
      eval("({a : 'str', [secret] : 'secret', [Symbol.for('a')] : 'global'});")
          .getObject(rt);

  EXPECT_EQ(x.getProperty(rt, strProp).getString(rt).utf8(rt), "str");
  EXPECT_EQ(x.getProperty(rt, secretProp).getString(rt).utf8(rt), "secret");
  EXPECT_EQ(x.getProperty(rt, globalProp).getString(rt).utf8(rt), "global");
}
#endif

TEST_P(JSITestExt, HasComputedTest) {
  // The only use of JSObject::hasComputed() is in HermesRuntimeImpl,
  // so we test its Proxy support here, instead of from JS.

  EXPECT_FALSE(eval("'prop' in new Proxy({}, {})").getBool());
  EXPECT_TRUE(eval("'prop' in new Proxy({prop:1}, {})").getBool());
  EXPECT_FALSE(
      eval("'prop' in new Proxy({}, {has() { return false; }})").getBool());
  EXPECT_TRUE(
      eval("'prop' in new Proxy({}, {has() { return true; }})").getBool());

  // While we're here, test that a HostFunction can be used as a proxy
  // trap.  This could be very powerful in the right hands.
  Function returnTrue = Function::createFromHostFunction(
      rt,
      PropNameID::forAscii(rt, "returnTrue"),
      0,
      [](Runtime& rt, const Value&, const Value* args, size_t count) {
        EXPECT_EQ(count, 2);
        EXPECT_EQ(args[1].toString(rt).utf8(rt), "prop");
        return true;
      });
  rt.global().setProperty(rt, "returnTrue", returnTrue);
  EXPECT_TRUE(eval("'prop' in new Proxy({}, {has: returnTrue})").getBool());
}

TEST_P(JSITestExt, GlobalObjectTest) {
  rt.global().setProperty(rt, "a", 5);
  eval("f = function(b) { return a + b; }");
  eval("gc()");
  EXPECT_EQ(eval("f(10)").getNumber(), 15);
}
#endif

#if JSI_VERSION >= 8
#if !defined(JSI_V8_IMPL)
TEST_P(JSITestExt, BigIntJSI) {
  Function bigintCtor = rt.global().getPropertyAsFunction(rt, "BigInt");
  auto BigInt = [&](const char* v) { return bigintCtor.call(rt, eval(v)); };

  auto v0 = BigInt("0");
  auto b0 = v0.asBigInt(rt);
  EXPECT_EQ(v0.toString(rt).utf8(rt), "0");
  EXPECT_EQ(b0.toString(rt).utf8(rt), "0");

  auto vffffffffffffffff = BigInt("0xffffffffffffffffn");
  auto bffffffffffffffff = vffffffffffffffff.asBigInt(rt);
  EXPECT_EQ(vffffffffffffffff.toString(rt).utf8(rt), "18446744073709551615");
  EXPECT_EQ(bffffffffffffffff.toString(rt, 16).utf8(rt), "ffffffffffffffff");
  EXPECT_EQ(bffffffffffffffff.toString(rt, 36).utf8(rt), "3w5e11264sgsf");

  auto vNeg1 = BigInt("-1");
  auto bNeg1 = vNeg1.asBigInt(rt);
  EXPECT_EQ(vNeg1.toString(rt).utf8(rt), "-1");
  EXPECT_EQ(bNeg1.toString(rt, 16).utf8(rt), "-1");
  EXPECT_EQ(bNeg1.toString(rt, 36).utf8(rt), "-1");

  EXPECT_TRUE(BigInt::strictEquals(rt, b0, b0));
  EXPECT_TRUE(BigInt::strictEquals(rt, bffffffffffffffff, bffffffffffffffff));
  EXPECT_FALSE(BigInt::strictEquals(rt, bNeg1, bffffffffffffffff));
}

TEST_P(JSITestExt, BigIntJSIFromScalar) {
  Function bigintCtor = rt.global().getPropertyAsFunction(rt, "BigInt");
  auto BigInt = [&](const char* v) {
    return bigintCtor.call(rt, eval(v)).asBigInt(rt);
  };

  EXPECT_TRUE(BigInt::strictEquals(rt, BigInt("0"), BigInt::fromUint64(rt, 0)));
  EXPECT_TRUE(BigInt::strictEquals(rt, BigInt("0"), BigInt::fromInt64(rt, 0)));
  EXPECT_TRUE(BigInt::strictEquals(
      rt, BigInt("0xdeadbeef"), BigInt::fromUint64(rt, 0xdeadbeef)));
  EXPECT_TRUE(BigInt::strictEquals(
      rt, BigInt("0xc0ffee"), BigInt::fromInt64(rt, 0xc0ffee)));
  EXPECT_TRUE(BigInt::strictEquals(
      rt, BigInt("0xffffffffffffffffn"), BigInt::fromUint64(rt, ~0ull)));
  EXPECT_TRUE(
      BigInt::strictEquals(rt, BigInt("-1"), BigInt::fromInt64(rt, ~0ull)));
}

TEST_P(JSITestExt, BigIntJSIToString) {
  auto b = BigInt::fromUint64(rt, 1);
  // Test all possible radixes.
  for (int radix = 2; radix <= 36; ++radix) {
    EXPECT_EQ(b.toString(rt, radix).utf8(rt), "1") << radix;
  }

  // Test some invaild radixes.
  EXPECT_THROW(b.toString(rt, -1), JSIException);
  EXPECT_THROW(b.toString(rt, 0), JSIException);
  EXPECT_THROW(b.toString(rt, 1), JSIException);
  EXPECT_THROW(b.toString(rt, 37), JSIException);
  EXPECT_THROW(b.toString(rt, 100), JSIException);

  Function bigintCtor = rt.global().getPropertyAsFunction(rt, "BigInt");
  auto BigInt = [&](int value) {
    return bigintCtor.call(rt, value).asBigInt(rt);
  };

  // Now test that the radix is being passed to the VM.
  for (int radix = 2; radix <= 36; ++radix) {
    EXPECT_EQ(BigInt(radix + 1).toString(rt, radix).utf8(rt), "11") << radix;
    EXPECT_EQ(BigInt(-(radix + 1)).toString(rt, radix).utf8(rt), "-11")
        << radix;
  }
}

TEST_P(JSITestExt, BigIntJSITruncation) {
  auto lossless = [](uint64_t value) { return std::make_tuple(value, true); };
  auto lossy = [](uint64_t value) { return std::make_tuple(value, false); };

  auto toInt64 = [this](const BigInt& b) {
    return std::make_tuple(b.getInt64(rt), b.isInt64(rt));
  };

  auto toUint64 = [this](const BigInt& b) {
    return std::make_tuple(b.getUint64(rt), b.isUint64(rt));
  };

  Function bigintCtor = rt.global().getPropertyAsFunction(rt, "BigInt");
  auto BigInt = [&](const char* v) {
    return bigintCtor.call(rt, eval(v)).asBigInt(rt);
  };

  // 0n can be truncated losslessly to either int64_t and uint64_t
  auto b = BigInt::fromUint64(rt, 0);
  EXPECT_EQ(toUint64(b), lossless(0));
  EXPECT_TRUE(
      BigInt::strictEquals(rt, BigInt::fromUint64(rt, b.getUint64(rt)), b));
  EXPECT_EQ(toInt64(b), lossless(0));
  EXPECT_TRUE(
      BigInt::strictEquals(rt, BigInt::fromInt64(rt, b.getInt64(rt)), b));

  // Creating BigInt from an ~0ull. This value can't be truncated losslessly to
  // int64_t.
  b = BigInt::fromUint64(rt, ~0ull);
  EXPECT_EQ(toUint64(b), lossless(~0ull));
  EXPECT_TRUE(
      BigInt::strictEquals(rt, BigInt::fromUint64(rt, b.getUint64(rt)), b));
  EXPECT_EQ(toInt64(b), lossy(~0ull));

  // Creating BigInt from an -1ull. This value can't be truncated losslessly to
  // int64_t.
  b = BigInt::fromInt64(rt, -1ull);
  EXPECT_EQ(toUint64(b), lossy(-1ull));
  EXPECT_EQ(toInt64(b), lossless(-1ull));
  EXPECT_TRUE(
      BigInt::strictEquals(rt, BigInt::fromInt64(rt, b.getInt64(rt)), b));

  // 0x10000000000000000n can't be truncated to int64_t nor uint64_t.
  b = BigInt("0x10000000000000000n");
  EXPECT_EQ(toUint64(b), lossy(0));
  EXPECT_EQ(toInt64(b), lossy(0));

  // -0x10000000000000000n can't be truncated to int64_t nor uint64_t.
  b = BigInt("-0x10000000000000000n");
  EXPECT_EQ(toUint64(b), lossy(0));
  EXPECT_EQ(toInt64(b), lossy(0));

  // (1n << 65n) - 1n can't be truncated to int64_t nor uint64_t.
  b = BigInt("(1n << 65n) - 1n");
  EXPECT_EQ(toUint64(b), lossy(~0ull));
  EXPECT_EQ(toInt64(b), lossy(~0ull));
}
#endif
#endif

TEST_P(JSITestExt, NativeExceptionDoesNotUseGlobalError) {
  Function alwaysThrows = Function::createFromHostFunction(
      rt,
      PropNameID::forAscii(rt, "alwaysThrows"),
      0,
      [](Runtime&, const Value&, const Value*, size_t) -> Value {
        throw std::logic_error(
            "Native std::logic_error C++ exception in Host Function");
      });
  rt.global().setProperty(rt, "alwaysThrows", alwaysThrows);
  rt.global().setProperty(rt, "Error", 10);

  auto test = eval(
                  R"#((function(val) {
                          'use strict';
                          try {
                            alwaysThrows(val);
                          } catch(e) {
                            return 'typeof Error is ' + typeof(Error) + '; ' + e.message;
                          }
                          throw new Error('Unreachable statement');
                       }))#")
                  .getObject(rt)
                  .getFunction(rt);
  EXPECT_EQ(
      "typeof Error is number; Exception in HostFunction: Native "
      "std::logic_error C++ exception in Host Function",
      test.call(rt).getString(rt).utf8(rt));
}

INSTANTIATE_TEST_SUITE_P(
    Runtimes,
    JSITestExt,
    ::testing::ValuesIn(runtimeGenerators()));
