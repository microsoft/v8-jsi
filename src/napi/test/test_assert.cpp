// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "napitest.h"

using namespace napitest;

TEST_P(NapiTest, test_assert) {
  ExecuteNapi([](NapiTestContext *testContext, napi_env env) {
    RUN_TEST_SCRIPT("require('assert').fail();")
        .Throws("AssertionError", [](NapiTestException const &ex) noexcept {
          EXPECT_EQ(ex.AssertionError()->Method, "fail");
          EXPECT_EQ(ex.ScriptError()->Message, "Failed");
        });

    RUN_TEST_SCRIPT("require('assert').fail('assert failed');")
        .Throws("AssertionError", [](NapiTestException const &ex) noexcept {
          EXPECT_EQ(ex.ScriptError()->Message, "assert failed");
        });

    RUN_TEST_SCRIPT("require('assert').ok(true);");

    RUN_TEST_SCRIPT("require('assert').ok(false);")
        .Throws("AssertionError", [](NapiTestException const &ex) noexcept {
          EXPECT_EQ(ex.AssertionError()->Method, "ok");
          EXPECT_EQ(
              ex.ScriptError()->Message,
              "The expression evaluated to a falsy value");
          EXPECT_EQ(ex.AssertionError()->Expected, "<boolean> true");
          EXPECT_EQ(ex.AssertionError()->Actual, "<boolean> false");
        });

    RUN_TEST_SCRIPT("require('assert').ok();")
        .Throws("AssertionError", [](NapiTestException const &ex) noexcept {
          EXPECT_EQ(ex.AssertionError()->Method, "ok");
          EXPECT_EQ(
              ex.ScriptError()->Message,
              "No value argument passed to `assert.ok()`");
          EXPECT_EQ(ex.AssertionError()->Expected, "<boolean> true");
          EXPECT_EQ(ex.AssertionError()->Actual, "<undefined> undefined");
        });

    RUN_TEST_SCRIPT("require('assert').strictEqual(true, 1);")
        .Throws("AssertionError", [](NapiTestException const &ex) noexcept {
          EXPECT_EQ(ex.AssertionError()->Method, "strictEqual");
          EXPECT_EQ(ex.ScriptError()->Message, "Values are not strict equal");
          EXPECT_EQ(ex.AssertionError()->Actual, "<boolean> true");
          EXPECT_EQ(ex.AssertionError()->Expected, "<number> 1");
        });

    RUN_TEST_SCRIPT("require('assert').strictEqual({}, []);")
        .Throws("AssertionError", [](NapiTestException const &ex) noexcept {
          EXPECT_EQ(ex.AssertionError()->Method, "strictEqual");
          EXPECT_EQ(ex.ScriptError()->Message, "Values are not strict equal");
          EXPECT_EQ(ex.AssertionError()->Actual, "<object> {}");
          EXPECT_EQ(ex.AssertionError()->Expected, "<array> []");
        });

    RUN_TEST_SCRIPT("require('assert').strictEqual(Number.NaN, Number.NaN);");

    RUN_TEST_SCRIPT("require('assert').mustCall();")
        .Throws("AssertionError", [](NapiTestException const &ex) noexcept {
          EXPECT_EQ(ex.AssertionError()->Method, "mustCall");
          EXPECT_EQ(ex.AssertionError()->Expected, "exactly 1 calls");
          EXPECT_EQ(ex.AssertionError()->Actual, "0 calls");
        });

    RUN_TEST_SCRIPT(R"(
      const assert = require('assert');
      const fn = assert.mustCall();
      fn(1, 2, 3);
      )");

    RUN_TEST_SCRIPT(R"(
      const assert = require('assert');
      const fn = assert.mustCall((x, y) => x + y);
      assert.strictEqual(fn(1, 2), 3);
      )");

    RUN_TEST_SCRIPT(R"(
      const assert = require('assert');
      const fn = assert.mustNotCall();
      fn(1, 2, 3); // must cause an AssertionError
      )")
        .Throws("AssertionError", [](NapiTestException const &ex) noexcept {
          EXPECT_EQ(ex.AssertionError()->Method, "mustNotCall");
        });

    RUN_TEST_SCRIPT("require('assert').mustNotCall();");

    RUN_TEST_SCRIPT(R"(
      const assert = require('assert');
      let resolvePromise;
      const promise = new Promise((resolve) => {resolvePromise = resolve;});
      promise.then(() => {
        assert.fail('Continuation must fail');
      });
      resolvePromise();
      )")
        .Throws("AssertionError", [](NapiTestException const &ex) noexcept {
          EXPECT_EQ(ex.AssertionError()->Method, "fail");
        });

    RUN_TEST_SCRIPT(
        "require('assert').throws(function() { throw new Error(); });");
  });
}
