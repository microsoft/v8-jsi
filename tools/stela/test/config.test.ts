import assert from "node:assert/strict";
import path from "node:path";
import { test } from "node:test";
import { parseConfig } from "../src/config.ts";
import { StelaError } from "../src/errors.ts";

const file = path.resolve("/repo/stela.config.json");
const root = path.dirname(file);
const lib = (extra: Record<string, unknown> = {}) => ({
  name: "v8host",
  prefix: "v8host",
  macroPrefix: "V8HOST",
  headers: ["src/public/v8host_client.h"],
  ...extra,
});
const parse = (libraries: unknown[], top: Record<string, unknown> = {}) =>
  parseConfig({ schema: "stela.config/1", libraries, ...top }, file);

test("defaults: stelaHeader next to the first header, status from the prefix", () => {
  const [l] = parse([lib()]).libraries;
  assert.equal(l.stelaHeader, path.join(root, "src", "public", "v8host_stela.h"));
  assert.equal(l.status, "v8host_status");
  assert.deepEqual(l.stelaNames, {});
  assert.deepEqual(l.headers, [path.join(root, "src", "public", "v8host_client.h")]);
});

test("explicit stelaHeader, extension libraries, and other §8.2 keys", () => {
  const config = parse(
    [
      lib({ stelaHeader: "include/v8host_stela.h", includeDirs: ["src/public"], targets: ["x86_64-pc-windows-msvc"] }),
      { name: "jsi_abi", prefix: "jsi", macroPrefix: "JSI_ABI", stelaHeader: "src/jsi_abi/jsi_abi_stela.h" },
      { name: "v8_jsi_abi", prefix: "v8_jsi_abi", macroPrefix: "V8_JSI_ABI", headers: ["a.h"], stelaHeader: false, status: "jsi_status" },
    ],
    { targets: [], lint: {}, diff: {} },
  );
  assert.equal(config.libraries[0].stelaHeader, path.join(root, "include", "v8host_stela.h"));
  assert.equal(config.libraries[1].stelaHeader, path.join(root, "src", "jsi_abi", "jsi_abi_stela.h"));
  assert.equal(config.libraries[2].stelaHeader, false);
  assert.equal(config.libraries[2].status, "jsi_status");
});

test("extraction keys: includeDirs, owns, defines, and targets", () => {
  const x64 = "x86_64-pc-windows-msvc";
  const x86 = "i686-pc-windows-msvc";
  const arm64 = "aarch64-pc-windows-msvc";
  const config = parse(
    [
      lib({ includeDirs: ["src/public"], defines: ["V8HOST_EXPERIMENTAL", "V8HOST_LEVEL=2"] }),
      { ...lib({ name: "ext", prefix: "ext", macroPrefix: "EXT" }), stelaHeader: false, owns: ["a.h"], targets: [arm64, x64] },
    ],
    { targets: [x64, x86, arm64] },
  );
  const [host, ext] = config.libraries;
  assert.deepEqual(config.targets, [x64, x86, arm64]);
  assert.deepEqual(host.includeDirs, [path.join(root, "src", "public")]);
  assert.deepEqual(host.owns, host.headers);
  assert.deepEqual(host.defines, ["V8HOST_EXPERIMENTAL", "V8HOST_LEVEL=2"]);
  assert.deepEqual(host.targets, [x64, x86, arm64]);
  assert.deepEqual(ext.owns, [path.join(root, "a.h")]);
  // A library's targets keep the global order.
  assert.deepEqual(ext.targets, [x64, arm64]);

  // Without a global list, the model's targets are the libraries' own, in order.
  const local = parse([lib({ targets: [x86] }), { ...lib({ name: "b", prefix: "b", macroPrefix: "B" }), targets: [x64, x86] }]);
  assert.deepEqual(local.targets, [x86, x64]);

  const bad = (extra: Record<string, unknown>, top: Record<string, unknown> = {}) =>
    assert.throws(() => parse([lib(extra)], top), StelaError);
  bad({ includeDirs: "src" });
  bad({ defines: ["1BAD"] });
  bad({ defines: ["A B"] });
  bad({ targets: [] });
  bad({ targets: ["x64"] });
  bad({ targets: [x64, x64] });
  bad({ targets: [arm64] }, { targets: [x64] });
  bad({}, { targets: [x64, x64] });
  bad({}, { targets: "x86_64-pc-windows-msvc" });
});

test("binaries: defaults, and the libraries they implement", () => {
  const libraries = [lib(), { name: "jsi_abi", prefix: "jsi", macroPrefix: "JSI_ABI", headers: ["j.h"] }];
  const config = parse(libraries, {
    binaries: [
      { name: "v8host.dll", libraries: ["jsi_abi", "v8host"] },
      { name: "sbox.exe", libraries: ["v8host"], exports: "none", otherExports: false },
    ],
  });
  assert.deepEqual(config.binaries, [
    { name: "v8host.dll", libraries: ["v8host", "jsi_abi"], exports: "all", otherExports: true },
    { name: "sbox.exe", libraries: ["v8host"], exports: "none", otherExports: false },
  ]);
  assert.deepEqual(parse([lib()]).binaries, []);

  const bad = (binaries: unknown) => assert.throws(() => parse(libraries, { binaries }), StelaError);
  bad({});
  bad([{ name: "v8host.dll" }]);
  bad([{ name: "v8host.dll", libraries: [] }]);
  bad([{ name: "v8host.dll", libraries: ["nope"] }]);
  bad([{ name: "v8host.dll", libraries: ["v8host", "v8host"] }]);
  bad([{ name: "v8host.so", libraries: ["v8host"] }]);
  bad([{ name: "../v8host.dll", libraries: ["v8host"] }]);
  bad([{ name: "v8host.dll", libraries: ["v8host"], exports: "some" }]);
  bad([{ name: "v8host.dll", libraries: ["v8host"], otherExports: "no" }]);
  bad([{ name: "v8host.dll", libraries: ["v8host"], typo: 1 }]);
  bad([
    { name: "v8host.dll", libraries: ["v8host"] },
    { name: "V8Host.DLL", libraries: ["v8host"] },
  ]);
});

test("invalid configs are rejected", () => {
  const bad = (libraries: unknown[], top: Record<string, unknown> = {}) =>
    assert.throws(() => parse(libraries, top), StelaError);
  assert.throws(() => parseConfig({ libraries: [lib()] }, file), StelaError);
  assert.throws(() => parseConfig([], file), StelaError);
  bad([]);
  bad([lib()], { typo: 1 });
  bad([lib({ typo: 1 })]);
  bad([lib({ name: "V8Host" })]);
  bad([lib({ prefix: "V8HOST" })]);
  bad([lib({ macroPrefix: "v8host" })]);
  bad([lib(), lib({ macroPrefix: "OTHER" })]);
  bad([lib(), lib({ name: "other" })]);
  bad([lib(), lib({ name: "other", macroPrefix: "OTHER", stelaHeader: "src/public/v8host_stela.h" })]);
  bad([lib({ headers: undefined })]);
  bad([lib({ headers: [""] })]);
  bad([lib({ stelaHeader: "x.txt" })]);
  bad([lib({ stelaHeader: true })]);
  bad([lib({ stelaHeader: false, stelaNames: { ENUM: "V8HOST_STELA_ENUM" } })]);
  bad([lib({ stelaNames: { ENUM: 1 } })]);
  bad([lib({ status: "Status" })]);
});
