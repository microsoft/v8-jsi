// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import assert from "node:assert/strict";
import { test } from "node:test";
import { checkArchiveCfg, checkPeFlags, checkProbeExports, checkIteratorConfiguration, linkedArchiveNames, legacyBinaryChecks } from "./legacy-checks.ts";

function object(feature: number | undefined, big = false): Buffer {
  const header = big ? 56 : 20;
  const data = Buffer.alloc(header + (feature === undefined ? 0 : big ? 20 : 18));
  if (big) { data.writeUInt16LE(0xffff, 2); data.writeUInt16LE(2, 4); }
  data.writeUInt16LE(0x8664, big ? 6 : 0);
  data.writeUInt32LE(header, big ? 48 : 8);
  data.writeUInt32LE(feature === undefined ? 0 : 1, big ? 52 : 12);
  if (feature !== undefined) {
    data.write("@feat.00", header);
    data.writeUInt32LE(feature, header + 8);
    if (big) data.writeInt32LE(-1, header + 12);
    else data.writeInt16LE(-1, header + 12);
  }
  return data;
}
function archive(name: string, data: Buffer): Buffer {
  const member = (n: string, b: Buffer): Buffer => Buffer.concat([
    Buffer.from(n.padEnd(16) + "0".padEnd(12) + "0".padEnd(6) + "0".padEnd(6) +
                "0".padEnd(8) + String(b.length).padEnd(10) + "`\n"), b,
    b.length & 1 ? Buffer.from("\n") : Buffer.alloc(0),
  ]);
  return Buffer.concat([Buffer.from("!<arch>\n"), member("//", Buffer.from(name + "\0")), member("/0", data)]);
}
const assembler = new Set(["push_registers_masm.obj"]);
// Ordinary and bigobj have different symbol-table layouts; both must fail closed.
for (const big of [false, true]) {
  test(`${big ? "bigobj" : "ordinary COFF"} requires CFG on every C++ member`, () => {
    assert.equal(checkArchiveCfg(archive("engine.obj", object(0x800, big)), assembler), 1);
    for (const feature of [undefined, 0, 0x10]) {
      assert.throws(() => checkArchiveCfg(archive("engine.obj", object(feature, big)), assembler), /Missing CFG/);
    }
  });
}
test("only named assembler objects are exempt from CFG", () => {
  assert.equal(checkArchiveCfg(archive("obj\\push_registers_masm.obj", object(0x10)), assembler), 1);
  assert.throws(() => checkArchiveCfg(archive("unexpected_masm.obj", object(undefined)), assembler), /unexpected assembler/);
});
test("CFG cannot be satisfied by another archive member", () => {
  const good = archive("good.obj", object(0x800));
  const bad = archive("bad.obj", object(0));
  assert.throws(() => checkArchiveCfg(Buffer.concat([good, bad.subarray(8)]), assembler), /Missing CFG/);
});
test("malformed or empty COFF archives fail closed", () => {
  for (const data of [Buffer.alloc(0), Buffer.from("!<arch>\n"), archive("engine.obj", object(0x800)).subarray(0, 80)]) {
    assert.throws(() => checkArchiveCfg(data, assembler), /Invalid|Truncated|no COFF/);
  }
});
function pe(flags: number): Buffer {
  const data = Buffer.alloc(160);
  data.write("MZ"); data.writeUInt32LE(64, 60); data.write("PE\0\0", 64);
  data.writeUInt16LE(0x20b, 88); data.writeUInt16LE(flags, 158);
  return data;
}
test("PE hardening requires each independent mitigation", () => {
  assert.doesNotThrow(() => checkPeFlags(pe(0x4160), true));
  for (const bit of [0x4000, 0x40, 0x100, 0x20]) {
    assert.throws(() => checkPeFlags(pe(0x4160 & ~bit), true), /missing/);
  }
  assert.doesNotThrow(() => checkPeFlags(pe(0x4140), false));
  assert.throws(() => checkPeFlags(Buffer.alloc(64), true), /Invalid PE/);
});

test("probe exports exclude the V8 C++ API", () => {
  const probe = "v8jsi_legacy_engine_probe";
  checkProbeExports([probe], "x64");
  for (const names of [[], [probe, "V8_Initialize"], [probe, probe]]) {
    assert.throws(() => checkProbeExports(names, "x64"), /Unexpected probe exports/);
  }
});

test("CET is required only by the x64 binary check set", () => {
  assert.deepEqual(legacyBinaryChecks("x64"), ["exports", "pe", "cet"]);
  assert.deepEqual(legacyBinaryChecks("x86"), ["exports", "pe"]);
  assert.deepEqual(legacyBinaryChecks("arm64"), ["exports", "pe"]);
});

test("exception range export is allowed only on x64 and ARM64", () => {
  const names = ["v8jsi_legacy_engine_probe", "CrashForExceptionInNonABICompliantCodeRange"];
  checkProbeExports(names, "x64");
  checkProbeExports(names, "arm64");
  checkProbeExports(names.slice(0, 1), "x86");
  assert.throws(() => checkProbeExports(names, "x86"), /Unexpected probe exports/);
});

// ARM64 Debug differs from every non-ARM64 cell; x64 represents the latter.
for (const platform of ["arm64", "x64"]) {
  test(`iterator override reaches only ${platform} Debug when required`, () => {
    const define = "_HAS_ITERATOR_DEBUGGING=0";
    const configurations = { Debug: { defines: platform === "arm64" ? [define] : [] }, Release: { defines: [] as string[] } };
    const project = (debug: string, release = "") =>
      `<ItemDefinitionGroup Condition="Debug|${platform}"><ClCompile><PreprocessorDefinitions>${debug}</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup>` +
      `<ItemDefinitionGroup Condition="Release|${platform}"><ClCompile><PreprocessorDefinitions>${release}</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup>`;
    checkIteratorConfiguration(configurations, platform, [["engine", project(configurations.Debug.defines.join(";"))]]);
    assert.throws(() => checkIteratorConfiguration(configurations, platform,
      [["engine", project(platform === "arm64" ? "" : define)]]), /Iterator project/);
    assert.throws(() => checkIteratorConfiguration(configurations, platform,
      [["engine", project(configurations.Debug.defines.join(";"), define)]]), /Iterator project/);
    if (platform === "arm64") {
      assert.throws(() => checkIteratorConfiguration(configurations, platform,
        [["engine", project(define).replace(/<PreprocessorDefinitions>[^<]*<\/PreprocessorDefinitions>/g, "")]]), /Iterator project/);
    }
    configurations.Release.defines.push(define);
    assert.throws(() => checkIteratorConfiguration(configurations, platform, []), /Iterator configuration/);
  });
}

test("all linked static libraries require CFG regardless of their names", () => {
  const libraries = ["v8_snapshot", "abseil", "simdutf", "highway", "torque_generated_definitions", "llhttp"];
  const project = [...libraries, "generator"].map((name) => `<ProjectReference Include="tools\\${name}.vcxproj" />`).join("");
  assert.deepEqual(linkedArchiveNames(project, (ref) => ref.includes("generator")
    ? "<ConfigurationType>Application</ConfigurationType>"
    : "<ConfigurationType>StaticLibrary</ConfigurationType>"), libraries.map((name) => name + ".lib"));
});
