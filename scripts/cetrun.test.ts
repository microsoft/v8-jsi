// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Real Windows x64 process tests; no mocked policy, process, or handle APIs.
import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { copyFileSync, linkSync, mkdtempSync, rmSync } from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { after, test, type TestContext } from "node:test";

const out = process.env.CETRUN_TEST_OUT ?? path.resolve(
  path.dirname(fileURLToPath(import.meta.url)), "../deps/nodejs/out/Release");
const launcher = path.join(out, "cetrun.exe");
const child = path.join(out, "v8jsi_test.exe");
const nodeApi = path.join(out, "node_api_tests.exe");
const cleanEnv = { ...process.env };
for (const name of Object.keys(cleanEnv)) {
  if (name.toUpperCase().startsWith("CETRUN_") || name.toUpperCase().startsWith("V8JSI_TEST_")) delete cleanEnv[name];
}
function invoke(mode: string, args: string[], env: NodeJS.ProcessEnv = {}, exe = child) {
  const r = spawnSync(launcher, [mode, exe, ...args], {
    env: { ...cleanEnv, ...env }, encoding: "utf8", timeout: 15000,
  });
  assert.ifError(r.error);
  assert.equal(r.signal, null);
  return r;
}
function isSkip(stderr: string) {
  return stderr.split(/\r?\n/).includes("SKIP: user CET not available");
}

// Probe independently in off mode; a later launcher SKIP must never hide a
// policy/creation failure on a capable host.
const availability = invoke("off", ["--cetrun-child", "echo"]);
assert.equal(availability.status, 0, availability.stderr);
assert.match(availability.stderr, /user_cet=(yes|no) forced_unavailable=0/);
const hostAvailable = /user_cet=yes/.test(availability.stderr);
function skipUnavailable(t: TestContext, diagnostic: string) {
  assert.equal(hostAvailable, false, `capable host cannot skip: ${diagnostic}`);
  assert.match(diagnostic, /user_cet=no forced_unavailable=0/);
  t.skip("SKIP: user CET not available");
}

const spacedDir = mkdtempSync(path.join(out, "launcher child "));
const spacedChild = path.join(spacedDir, "child with spaces.exe");
copyFileSync(child, spacedChild);
after(() => rmSync(spacedDir, { recursive: true, force: true }));

test("Windows argv quoting and inherited stdout/stderr round-trip", () => {
  // Runs around quotes/end-of-argument distinguish all CRT backslash rules.
  const args = ["", "two words", 'a"b', "tail" + "\\", "two " + "\\".repeat(2), "tab\targ"];
  for (let n = 0; n < 6; n++) args.push("x" + "\\".repeat(n) + '"end', "\\".repeat(n));
  const r = invoke("off", ["--cetrun-child", "echo", ...args], {
    PATH: `${out};${cleanEnv.PATH ?? cleanEnv.Path ?? ""}`,
  }, spacedChild);
  assert.equal(r.status, 0, r.stderr);
  assert.equal(r.stdout.replaceAll("\r\n", "\n"), args.map(a => `${a.length}:${a}\n`).join(""));
  assert.match(r.stderr, /child stderr/);
  assert.match(r.stderr, /cetrun: mode=off user_cet=(yes|no) forced_unavailable=0 os=\d+\.\d+\.\d+ cpu=.+/);
});

// Boundary exit values catch signed/truncated DWORD propagation.
for (const code of [0, 1, 0xc0000409]) {
  test(`child exit code ${code} propagates exactly`, () => {
    const r = invoke("off", ["--cetrun-child", "exit", `${code}`]);
    assert.equal(r.status! >>> 0, code, r.stderr);
  });
}

for (const [mode, enabled, strict] of [["off", 0, 0], ["compat", 1, 0], ["strict", 1, 1]] as const) {
  // Each child queries itself, independently of the launcher's suspended query.
  test(`${mode} policy is observed by the running child`, t => {
    const r = invoke(mode, ["--cetrun-child", "policy"]);
    // Older OSes may lack both CET availability and the policy-query enum.
    // Off still runs plainly; only its unavailable policy observation skips.
    if (isSkip(r.stderr) || (mode === "off" && /user_cet=no/.test(r.stderr) && r.status === 3)) {
      assert.doesNotMatch(r.stderr, /Windows operation failed/);
      skipUnavailable(t, r.stderr); return;
    }
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, new RegExp(`child: shadowstack=${enabled} strict=${strict}`));
    console.log(r.stderr.trim());
  });
}

function assertDead(pid: number) {
  // Job termination is asynchronous for descendants: allow bounded reaping.
  for (let attempt = 0; attempt < 100; ++attempt) {
    try { process.kill(pid, 0); } catch (e) {
      assert.equal((e as NodeJS.ErrnoException).code, "ESRCH"); return;
    }
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 10);
  }
  process.kill(pid); // Don't leave a process behind even if the assertion fails.
  assert.fail(`process ${pid} survived job close`);
}

test("timeout kills child and descendants", () => {
  const r = invoke("off", ["--cetrun-child", "tree"], { CETRUN_TIMEOUT_MS: "5000" });
  assert.equal(r.status, 5, r.stderr);
  assert.match(r.stderr, /timeout; terminating child job/);
  const descendant = /descendant=(\d+)/.exec(r.stdout);
  const parent = /parent=(\d+)/.exec(r.stdout);
  assert.ok(descendant, r.stdout);
  assert.ok(parent, r.stdout);
  assertDead(Number(descendant[1]));
  assertDead(Number(parent[1]));
});

test("closing launcher kills its job without waiting for its timeout", async () => {
  const p = spawn(launcher, ["off", child, "--cetrun-child", "sleep"], {
    env: cleanEnv, stdio: ["ignore", "pipe", "pipe"],
  });
  const closed = new Promise<void>((resolve, reject) => {
    p.once("close", () => resolve()); p.once("error", reject);
  });
  let output = "";
  const timer = setTimeout(() => p.kill(), 10000);
  try {
    for await (const chunk of p.stdout) {
      output += chunk.toString();
      if (/pid=\d+/.test(output)) { p.kill(); break; }
    }
    await closed;
    const pid = /pid=(\d+)/.exec(output);
    assert.ok(pid, output);
    assertDead(Number(pid[1]));
  } finally { clearTimeout(timer); if (p.exitCode === null) p.kill(); }
});

for (const [variable, message] of [["CETRUN_TEST_MISMATCH", /policy mismatch/],
                                  ["CETRUN_TEST_QUERY_FAILURE", /policy query failed/]] as const) {
  test(`${variable.includes("MISMATCH") ? "mismatched" : "unqueryable"} policy fails before child resumes`, t => {
    const r = invoke("strict", ["--cetrun-child", "echo", "must not run"], { [variable]: "1" });
    if (isSkip(r.stderr)) { skipUnavailable(t, r.stderr); return; }
    assert.equal(r.status, 4, r.stderr);
    assert.match(r.stderr, message);
    assert.match(r.stderr, /child not resumed/);
    assert.equal(r.stdout, "");
  });
}

test("unavailable CET skips on/strict without starting child; off still runs", () => {
  // This capable host cannot naturally exercise a missing API: force only the
  // launcher's availability result, leaving real process/skip handling intact.
  for (const mode of ["compat", "strict"]) {
    const r = invoke(mode, ["--cetrun-child", "echo", "must not run"], { CETRUN_TEST_NO_CET: "1" });
    assert.equal(r.status, 0, r.stderr);
    assert.ok(isSkip(r.stderr));
    assert.match(r.stderr, new RegExp(`user_cet=${hostAvailable ? "yes" : "no"} forced_unavailable=1`));
    assert.equal(r.stdout, "");
  }
  const off = invoke("off", ["--cetrun-child", "echo", "plain"], { CETRUN_TEST_NO_CET: "1" });
  assert.equal(off.status, 0, off.stderr);
  assert.match(off.stdout, /5:plain/);
  assert.ok(!isSkip(off.stderr));
});

test("required CET fails instead of skipping when unavailable", () => {
  // Both CET-enabled modes must fail closed, independently of host capability.
  for (const mode of ["compat", "strict"]) {
    const r = invoke(mode, ["--cetrun-child", "echo", "must not run"], {
      CETRUN_REQUIRE_CET: "1", CETRUN_TEST_NO_CET: "1",
    });
    assert.equal(r.status, 6, r.stderr);
    assert.match(r.stderr, /user CET not available but CETRUN_REQUIRE_CET=1/);
    assert.ok(!isSkip(r.stderr));
    assert.equal(r.stdout, "");
  }
});

// All spellings resolve to the same executable identity, including a hard link
// with a different basename that no string-only recursion guard can recognize.
const linkedLauncher = path.join(spacedDir, "launcher alias.exe");
linkSync(launcher, linkedLauncher);
for (const alias of [launcher + ".", linkedLauncher]) {
  test(`recursive executable alias is rejected: ${path.basename(alias)}`, () => {
    const r = invoke("off", ["off", child, "--cetrun-child", "policy"], {}, alias);
    assert.equal(r.status, 2, r.stderr);
    assert.match(r.stderr, /recursive launch is not allowed/);
    assert.equal(r.stdout, "");
    assert.equal(r.stderr.split(/\r?\n/).filter(Boolean).length, 1);
  });
}

// Attribute and process-creation errors must fail closed, never become SKIP.
for (const [hook, error] of [["CETRUN_TEST_ATTRIBUTE_FAILURE", 87], ["CETRUN_TEST_CREATE_FAILURE", 50]] as const) {
  test(`${hook.includes("ATTRIBUTE") ? "attribute" : "creation"} rejection fails without running child`, t => {
    const r = invoke("strict", ["--cetrun-child", "echo", "must not run"], { [hook]: "1" });
    if (!hostAvailable) { skipUnavailable(t, r.stderr); return; }
    assert.equal(r.status, 3, r.stderr);
    assert.match(r.stderr, new RegExp(`Windows operation failed: ${error}`));
    assert.ok(!isSkip(r.stderr));
    assert.equal(r.stdout, "");
  });
}

test("Node-API per-child strict skip is counted, never passed", () => {
  const r = spawnSync(nodeApi, ["--gtest_filter=js_native_api.2_function_arguments/test.js"], {
    env: { ...cleanEnv, V8JSI_TEST_CET: "strict", CETRUN_TEST_NO_CET: "1" },
    encoding: "utf8", timeout: 15000,
  });
  assert.ifError(r.error);
  assert.equal(r.status, 0, r.stderr);
  assert.match(r.stdout, /1 test from 1 test suite/);
  assert.match(r.stdout, /\[  SKIPPED \] 1 test/);
  assert.match(r.stdout, /\[  PASSED  \] 0 tests/);
  assert.match(r.stdout, /SKIP: user CET not available/);
});

test("Node-API required CET turns a per-child skip into a failed test", () => {
  const r = spawnSync(nodeApi, ["--gtest_filter=js_native_api.2_function_arguments/test.js"], {
    env: { ...cleanEnv, V8JSI_TEST_CET: "strict", CETRUN_REQUIRE_CET: "1", CETRUN_TEST_NO_CET: "1" },
    encoding: "utf8", timeout: 15000,
  });
  assert.ifError(r.error);
  assert.equal(r.status, 1, r.stdout + r.stderr);
  assert.match(r.stdout, /\[  FAILED  \] 1 test/);
  assert.doesNotMatch(r.stdout, /\[  SKIPPED \]/);
  assert.match(r.stderr, /user CET not available but CETRUN_REQUIRE_CET=1/);
});

test("Node-API rejects invalid CET modes before launching a child", () => {
  // Empty, case-mismatched, and injected-argument values are not enum members.
  for (const mode of ["", "STRICT", `strict "${child}" --cetrun-child echo`]) {
    const r = spawnSync(nodeApi, ["--gtest_filter=js_native_api.2_function_arguments/test.js"], {
      env: { ...cleanEnv, V8JSI_TEST_CET: mode }, encoding: "utf8", timeout: 15000,
    });
    assert.ifError(r.error);
    assert.equal(r.status, 1, r.stdout + r.stderr);
    assert.match(r.stdout, /Invalid V8JSI_TEST_CET: expected off\|compat\|strict/);
    assert.match(r.stdout, /\[  FAILED  \] 1 test/);
    assert.doesNotMatch(r.stderr, /cetrun:/);
  }
});

// Four fixtures each spawn one nested JS child, including intentional fatal
// finalizers; asserting eight policies proves both generations are wrapped.
test("Node-API nested JavaScript children use strict policy", t => {
  const r = spawnSync(nodeApi, ["--gtest_filter=*testEnvCleanup*:*testFinalizerException*:*test_fatal_finalize*:*test_finalizer.js"], {
    env: { ...cleanEnv, V8JSI_TEST_CET: "strict" }, encoding: "utf8", timeout: 30000,
  });
  assert.ifError(r.error);
  assert.equal(r.status, 0, r.stdout + r.stderr);
  if (!hostAvailable) { skipUnavailable(t, r.stderr); return; }
  assert.match(r.stdout, /\[  PASSED  \] 4 tests/);
  const policies = r.stderr.match(/child policy shadowstack=\d strict=\d/g) ?? [];
  assert.equal(policies.length, 8, r.stderr);
  assert.ok(policies.every(p => p === "child policy shadowstack=1 strict=1"), r.stderr);
  assert.ok(!isSkip(r.stdout + r.stderr));
});

test("JSI main applies environment V8 flags before first runtime", () => {
  const r = spawnSync(child, ["--gtest_filter=Basic.CreateOneRuntimes"], {
    env: { ...cleanEnv, V8JSI_TEST_V8_FLAGS: "--allow-natives-syntax --print-flag-values" }, encoding: "utf8",
  });
  assert.ifError(r.error);
  assert.equal(r.status, 0, r.stderr);
  assert.match(r.stdout, /^--allow-natives-syntax\r?$/m);
  assert.match(r.stdout, /\[  PASSED  \] 1 test/);
});
