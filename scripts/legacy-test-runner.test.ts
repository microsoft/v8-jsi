// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
import assert from "node:assert/strict";
import { test } from "node:test";
import { legacyTestEnvironment, runLegacySuite } from "./legacy-test-runner.ts";

test("ambient test filters require an explicit command-line choice", () => {
  assert.throws(() => legacyTestEnvironment({ GTEST_FILTER: "NoSuchLegacyTest.*" }), /Inherited GTEST_FILTER/);
  assert.deepEqual(legacyTestEnvironment({ GTEST_FILTER: "old", CETRUN_MODE: "strict", PATH: "tools" }, "new"), { PATH: "tools" });
});
test("successful children must report at least one executed test", () => {
  // A tiny child simulates Google Test XML without depending on a native build.
  for (const count of [0, 1]) {
    const script = `const fs=require('node:fs'); const p=process.argv[1].slice('--gtest_output=xml:'.length); fs.writeFileSync(p, '<testsuites tests="${count}"></testsuites>');`;
    const run = () => runLegacySuite(process.execPath, ["-e", script, "--"],
      { cwd: process.cwd(), env: process.env, stdio: "inherit", timeout: 10000 });
    if (count) run(); else assert.throws(run, /zero tests/);
  }
});
