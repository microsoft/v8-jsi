// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
import assert from "node:assert/strict";
import { test } from "node:test";
import { legacyTestEnvironment, runLegacySuite, legacySuiteFilter, requireLegacyTests, legacyTestMatches } from "./legacy-test-runner.ts";

test("ambient test filters require an explicit command-line choice", () => {
  assert.throws(() => legacyTestEnvironment({ GTEST_FILTER: "NoSuchLegacyTest.*" }), /Inherited GTEST_FILTER/);
  assert.deepEqual(legacyTestEnvironment({ GTEST_FILTER: "old", CETRUN_MODE: "strict", PATH: "tools" }, "new"), { PATH: "tools" });
});
test("successful children must report at least one executed test", () => {
  // A tiny child simulates Google Test XML without depending on a native build.
  for (const count of [0, 1]) {
    const script = `const fs=require('node:fs'); const p=process.argv.find(a=>a.startsWith('--gtest_output=xml:')).slice('--gtest_output=xml:'.length); fs.writeFileSync(p, '<testsuites tests="${count}"></testsuites>');`;
    const run = () => runLegacySuite(process.execPath, ["-e", script, "--"],
      { cwd: process.cwd(), env: process.env, stdio: "inherit", timeout: 10000 });
    if (count) run(); else assert.throws(run, /zero tests/);
  }
});

test("explicit filters allow empty suites but require an aggregate match", () => {
  const script = `const fs=require('node:fs'); const p=process.argv.find(a=>a.startsWith('--gtest_output=xml:')).slice('--gtest_output=xml:'.length); fs.writeFileSync(p, '<testsuites tests="0"></testsuites>');`;
  const count = runLegacySuite(process.execPath, ["-e", script, "--"],
    { cwd: process.cwd(), env: process.env, stdio: "inherit", timeout: 10000, allowEmpty: true });
  assert.equal(count, 0);
  assert.throws(() => requireLegacyTests(count), /across all suites/);
  requireLegacyTests(count + 1);
});
test("explicit filters retain isolated instrumentation exclusions", () => {
  assert.equal(legacySuiteFilter("*", "LegacyInstrumentationFlags.*"), "*-LegacyInstrumentationFlags.*");
  assert.equal(legacySuiteFilter("Wanted.*-Other.*", "LegacyInstrumentationFlags.*"),
    "Wanted.*-Other.*:LegacyInstrumentationFlags.*");
});
test("failed children identify the last running test", () => {
  assert.throws(() => runLegacySuite(process.execPath,
    ["-e", "console.log('[ RUN      ] Example.Hangs'); process.exit(5)", "--"],
    { cwd: process.cwd(), env: process.env, stdio: "inherit", timeout: 10000 }), /last running test: Example.Hangs/);
});

test("timed-out children identify the last running test", () => {
  assert.throws(() => runLegacySuite(process.execPath,
    ["-e", "console.log('[ RUN      ] Example.Timeout'); setTimeout(()=>{},60000)", "--"],
    { cwd: process.cwd(), env: process.env, stdio: "inherit", timeout: 1000 }), /last running test: Example.Timeout/);
});

test("filters preserve negative patterns containing another hyphen", () => {
  assert.equal(legacySuiteFilter("Wanted.*-Other.name-with-hyphens", "Isolated.*"),
    "Wanted.*-Other.name-with-hyphens:Isolated.*");
  assert.equal(legacyTestMatches("Other.name-with-hyphens", "*-Other.name-with-hyphens"), false);
});
test("isolated mandatory checks count only explicit filter matches", () => {
  const script = `const fs=require('node:fs'); const p=process.argv.find(a=>a.startsWith('--gtest_output=xml:')).slice('--gtest_output=xml:'.length); fs.writeFileSync(p, '<testsuites tests="1"></testsuites>'); console.log('[ RUN      ] LegacyInstrumentation.Flags');`;
  for (const [filter, expected] of [["LegacyInstrumentation.*", 1], ["Missing.*", 0], ["", 0], ["*-LegacyInstrumentation.*", 0]] as const) {
    assert.equal(runLegacySuite(process.execPath, ["-e", script, "--"],
      { cwd: process.cwd(), env: process.env, stdio: "inherit", timeout: 10000, countFilter: filter }), expected);
  }
});

test("negative-only filters match all tests except the exclusions", () => {
  assert.equal(legacyTestMatches("Bar.Runs", "-Foo.*"), true);
  assert.equal(legacyTestMatches("Foo.Excluded", "-Foo.*"), false);
  assert.equal(legacyTestMatches("Bar.Runs", legacySuiteFilter("-Foo.*", "Isolated.*")), true);
  assert.equal(legacyTestMatches("Isolated.Check", legacySuiteFilter("-Foo.*", "Isolated.*")), false);
});
