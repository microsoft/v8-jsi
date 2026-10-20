import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { test } from "node:test";
import { tempDir } from "./helpers.ts";

const cli = path.join(import.meta.dirname, "..", "src", "stela.ts");

function run(cwd: string, ...args: string[]) {
  const r = spawnSync(process.execPath, [cli, ...args], { cwd, encoding: "utf8" });
  return { code: r.status, out: r.stdout, err: r.stderr };
}

function writeConfig(dir: string, libraries: unknown[]) {
  writeFileSync(
    path.join(dir, "stela.config.json"),
    JSON.stringify({ schema: "stela.config/1", libraries }, null, 2),
  );
}

const libraries = [
  { name: "alpha", prefix: "alpha", macroPrefix: "ALPHA", headers: ["a/alpha.h"] },
  { name: "beta", prefix: "beta", macroPrefix: "BETA", stelaHeader: "b/beta_stela.h" },
  { name: "beta_ext", prefix: "beta_ext", macroPrefix: "BETA_EXT", headers: ["b/ext.h"], stelaHeader: false, status: "beta_status" },
];

test("vendor writes, then leaves byte-identical files alone; --check agrees", (t) => {
  const dir = tempDir(t, "cli");
  writeConfig(dir, libraries);

  let r = run(dir, "vendor");
  assert.equal(r.code, 0, r.err);
  assert.equal(r.out, "wrote:     a/alpha_stela.h\nwrote:     b/beta_stela.h\n");
  const alpha = readFileSync(path.join(dir, "a", "alpha_stela.h"));
  const beta = readFileSync(path.join(dir, "b", "beta_stela.h"));
  assert.match(alpha.toString(), /^#define ALPHA_NULLABLE /m);
  assert.match(beta.toString(), /^#define BETA_NULLABLE /m);

  r = run(dir, "vendor");
  assert.equal(r.code, 0, r.err);
  assert.equal(r.out, "unchanged: a/alpha_stela.h\nunchanged: b/beta_stela.h\n");
  assert.ok(readFileSync(path.join(dir, "a", "alpha_stela.h")).equals(alpha));
  assert.ok(readFileSync(path.join(dir, "b", "beta_stela.h")).equals(beta));

  r = run(dir, "vendor", "--check");
  assert.equal(r.code, 0, r.err);
  assert.equal(r.out, "current:   a/alpha_stela.h\ncurrent:   b/beta_stela.h\n");

  // A CRLF checkout of a current file still passes the check.
  writeFileSync(path.join(dir, "a", "alpha_stela.h"), alpha.toString().replace(/\n/g, "\r\n"));
  assert.equal(run(dir, "vendor", "--check").code, 0);
});

test("--check reports stale and missing files with exit code 1", (t) => {
  const dir = tempDir(t, "cli");
  writeConfig(dir, libraries);
  assert.equal(run(dir, "vendor").code, 0);
  const file = path.join(dir, "a", "alpha_stela.h");
  writeFileSync(file, readFileSync(file, "utf8").replace("ALPHA_NULLABLE", "ALPHA_NULLABLE2"));
  let r = run(dir, "vendor", "--check");
  assert.equal(r.code, 1);
  assert.match(r.err, /^stale: {5}a\/alpha_stela\.h$/m);

  writeConfig(dir, [...libraries, { name: "gamma", prefix: "gamma", macroPrefix: "GAMMA", stelaHeader: "g/gamma_stela.h" }]);
  r = run(dir, "vendor", "--check");
  assert.equal(r.code, 1);
  assert.match(r.err, /^missing: {3}g\/gamma_stela\.h$/m);
  assert.match(r.err, /2 generated file\(s\) are out of date/);
});

test("usage and configuration errors exit with code 2", (t) => {
  const dir = tempDir(t, "cli");
  assert.equal(run(dir).code, 2);
  assert.equal(run(dir, "frobnicate").code, 2);
  assert.equal(run(dir, "vendor").code, 2); // no stela.config.json
  writeConfig(dir, libraries);
  assert.equal(run(dir, "vendor", "--bogus").code, 2);
  assert.equal(run(dir, "vendor", "--template", path.join(dir, "missing.h")).code, 2);
  writeConfig(dir, [{ name: "alpha" }]);
  const r = run(dir, "vendor");
  assert.equal(r.code, 2);
  assert.match(r.err, /^stela: .*"prefix" must be lowercase_snake/m);
  assert.equal(run(dir, "help").code, 0);
});

test("--template and --config select their inputs", (t) => {
  const dir = tempDir(t, "cli");
  const cwd = path.join(dir, "elsewhere");
  mkdirSync(cwd);
  writeConfig(dir, libraries.slice(0, 1));
  const template = readFileSync(path.join(import.meta.dirname, "..", "include", "stela.h"), "utf8");
  writeFileSync(path.join(dir, "t.h"), template.replace("/* Lifecycle */", "/* Lifecycle, edited */"));
  // Paths in the config resolve against the config's directory, not the cwd.
  const r = run(cwd, "vendor", "--config", path.join(dir, "stela.config.json"), "--template", path.join(dir, "t.h"));
  assert.equal(r.code, 0, r.err);
  assert.match(readFileSync(path.join(dir, "a", "alpha_stela.h"), "utf8"), /Lifecycle, edited/);
});
