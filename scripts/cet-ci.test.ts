// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import assert from "node:assert/strict";
import { copyFileSync, mkdtempSync, mkdirSync, rmSync, existsSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import * as path from "node:path";
import { spawnSync } from "node:child_process";
import { test } from "node:test";
import { repoRoot, shouldDeferCetTests } from "./build-common.ts";

const quote = (s: string) => "'" + s.replaceAll("'", "''") + "'";
const helper = `. ${quote(path.join(repoRoot, ".ado/run-cet-tests.ps1"))} -RepoRoot . -Archive unused; `;
const powershell = (s: string) => spawnSync("pwsh", ["-NoProfile", "-NonInteractive", "-Command",
  "$ErrorActionPreference = 'Stop'; $PSNativeCommandUseErrorActionPreference = $false; " + helper + s],
  { encoding: "utf8", timeout: 30_000 });

test("CET deferral requires explicit control, unavailable CET, and a build invocation", () => {
  // Representative controls and both vetoes protect local behavior and required-mode precedence.
  const cases: [string | undefined, boolean, boolean, boolean][] = [
    [undefined, false, false, false],
    ["0", false, false, false],
    ["true", false, false, false],
    ["1", false, false, true],
    ["1", false, true, false],
    ["1", true, false, false],
  ];
  for (const [control, available, runtime, expected] of cases) {
    assert.equal(shouldDeferCetTests(control, available, runtime), expected);
  }
});

test("runtime-only entry rejects missing and failing launchers without build inputs or static gates", () => {
  const root = mkdtempSync(path.join(tmpdir(), "cet-entry-"));
  try {
    mkdirSync(path.join(root, "scripts"));
    writeFileSync(path.join(root, "scripts/package.json"), '{"type":"module"}');
    // Copy product sources (not fixtures) to exercise a real sparse checkout without build inputs.
    for (const file of ["build.ts", "build-common.ts", "source-link-check.ts", "cetrun.test.ts"]) {
      copyFileSync(path.join(repoRoot, "scripts", file), path.join(root, "scripts", file));
    }
    const payload = path.join(root, "payload");
    // Let the nested contract runner execute rather than inherit node:test's recursion guard.
    const env: NodeJS.ProcessEnv = { ...process.env, V8JSI_CET_TESTS_DEFERRED: "1" };
    delete env.NODE_TEST_CONTEXT;
    for (const failingLauncher of [false, true]) {
      if (failingLauncher) { mkdirSync(payload); copyFileSync(process.execPath, path.join(payload, "cetrun.exe")); }
      const result = spawnSync(process.execPath, [path.join(root, "scripts/build.ts"), "--test-cet-runtime", payload],
        { cwd: root, encoding: "utf8", timeout: 20_000, env });
      assert.notEqual(result.status, 0, result.stdout);
      assert.match(result.stdout, /=== CET launcher contract tests ===/);
      assert.match(result.stderr, /CET launcher tests failed/);
      assert.match(result.stdout + result.stderr, failingLauncher ? /Cannot find module .*off/ : /cetrun\.exe ENOENT/);
      assert.doesNotMatch(result.stdout + result.stderr, /header gate|Stela|config\.json|v8jsi_names|CET availability probe failed/);
      assert.equal(existsSync(path.join(root, "out")), false);
    }
  } finally { rmSync(root, { recursive: true, force: true }); }
});

test("CET command helper rejects native nonzero exit", () => {
  const result = powershell(`Invoke-CetCommand ${quote(process.execPath)} @('-e', 'process.exit(23)')`);
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /exit 23/);
});

test("broker zero exit cannot hide unavailable CET but normal output passes", () => {
  // The broker alone has a success-shaped unavailable marker that must override its exit code.
  for (const output of ["client/cet-catch-stress: PASS", "client/cet-catch-stress: PASS - SKIP: user CET not available"]) {
    const result = powershell(`Invoke-CetCommand ${quote(process.execPath)} @('-e', ${quote(`console.log(${JSON.stringify(output)})`)}) -Broker`);
    assert.equal(result.status === 0, !output.includes("SKIP"), result.stderr);
    if (output.includes("SKIP")) assert.match(result.stderr, /Broker skipped/);
  }
});

test("legacy startup requires exactly one expected flag, policy, and callback summary", () => {
  const valid = "--cet-compatible\nStartup shadow-stacks=1 callbacks=1";
  // One valid result and independent flag, duplicate, policy, and callback faults exercise the exact summary check.
  for (const output of [valid, valid.replace("--cet", "--no-cet"), valid.replace("--cet", "--CET"), valid + "\n--cet-compatible",
    valid.replace("stacks=1", "stacks=0"), valid.replace("callbacks=1", "callbacks=2")]) {
    const result = powershell(`Assert-LegacyStartup ${quote(output)} '--cet-compatible' 1`);
    assert.equal(result.status === 0, output === valid, result.stderr);
    if (output !== valid) assert.match(result.stderr, /Legacy startup policy mismatch/);
  }
});
