// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import assert from "node:assert/strict";
import { test } from "node:test";
import { checkArchiveCfg, checkPeFlags, checkIteratorConfiguration, linkedArchiveNames } from "./legacy-checks.ts";

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


import { checkLegacyExports, checkLegacySources } from "./legacy-checks.ts";
import { repoRoot } from "./build-common.ts";

test("legacy exports require the complete contract, without duplicates or hooks", () => {
  const expected = { common: ["napi_call_function"], platforms: { x64: ["makeV8Runtime"] } };
  checkLegacyExports(["makeV8Runtime", "napi_call_function"], expected, "x64");
  for (const names of [[], ["napi_call_function"], ["napi_call_function", "makeV8Runtime", "test_hook"],
                       ["napi_call_function", "makeV8Runtime", "makeV8Runtime"]]) {
    assert.throws(() => checkLegacyExports(names, expected, "x64"), /export mismatch/);
  }
});
test("frozen legacy sources and inspector donors match their pinned hashes", () => {
  checkLegacySources(repoRoot);
});

import { checkLegacyProject, checkLegacyIncludes } from "./legacy-checks.ts";
test("legacy exception support must change the STL layout as well as compiler unwinding", () => {
  const valid = "<ItemDefinitionGroup><ClCompile><ExceptionHandling>Sync</ExceptionHandling><RuntimeTypeInfo>false</RuntimeTypeInfo></ClCompile></ItemDefinitionGroup>";
  checkLegacyProject(valid);
  assert.throws(() => checkLegacyProject(valid.replace("<ClCompile>", "<ClCompile>_HAS_EXCEPTIONS=0")), /contract mismatch/);
  assert.throws(() => checkLegacyProject(valid.replace("Sync", "false")), /contract mismatch/);
});
test("legacy compile dependencies cannot resolve modern public headers", () => {
  checkLegacyIncludes("C:/repo/src/legacy/jsi/jsi.h\nC:/repo/src/MurmurHash.h", "C:/repo", ["src/MurmurHash.h"]);
  assert.throws(() => checkLegacyIncludes("C:/repo/src/jsi/jsi.h", "C:/repo", []), /include leakage/);
  assert.throws(() => checkLegacyIncludes("", "C:/repo", []), /empty/);
});

import { checkLegacyStartupFlags } from "./legacy-checks.ts";
test("startup flags distinguish defaults, overrides and duplicate initialization", () => {
  const output = "--no-cet-compatible\r\nStartup shadow-stacks=1 callbacks=1\r\n";
  checkLegacyStartupFlags(output, "--no-cet-compatible", true);
  checkLegacyStartupFlags("--cet-compatible\nStartup shadow-stacks=1 callbacks=1\n", "--cet-compatible", true);
  checkLegacyStartupFlags("--no-cet-compatible\nStartup shadow-stacks=0 callbacks=1\n", "--no-cet-compatible", false);
  for (const bad of ["", output.replace("--no-cet-compatible", "--cet-compatible"),
                    output.replace("shadow-stacks=1", "shadow-stacks=0"),
                    output.replace("callbacks=1", "callbacks=2"),
                    `--no-cet-compatible\n${output}`, `${output}Startup shadow-stacks=1 callbacks=1\n`]) {
    assert.throws(() => checkLegacyStartupFlags(bad, "--no-cet-compatible", true), /startup policy mismatch/);
  }
});

import { normalizedHash, legacyCetArtifacts, checkLegacyHardeningProject, checkLegacyIdentity } from "./legacy-checks.ts";
import { join } from "node:path";
test("legacy CET artifact wiring checks the launcher and the ordinary Node-API child", () => {
  assert.deepEqual(legacyCetArtifacts("x64"), [["v8jsi.dll", true], ["cetrun_legacy.exe", true], ["node_lite_legacy.exe", false]]);
  assert.deepEqual(legacyCetArtifacts("x86"), []);
  assert.deepEqual(legacyCetArtifacts("arm64"), []);
});
test("source hashes normalize checkout EOLs but not content tampering", () => {
  assert.equal(normalizedHash(Buffer.from("a\nb\n")), normalizedHash(Buffer.from("a\r\nb\r\n")));
  assert.notEqual(normalizedHash(Buffer.from("a\nb\n")), normalizedHash(Buffer.from("a\nc\n")));
});
test("source manifests independently reject copy and inspector origin drift", () => {
  const data = Buffer.from("source\n"), hash = normalizedHash(data);
  const manifest = { contract: "5424838ddaf", files: [{ kind: "inspector", path: "copy", origin: "origin", sha256: hash }] };
  const files = new Map([[join("repo", "src/legacy/sources.json"), Buffer.from(JSON.stringify(manifest))],
                         [join("repo", "copy"), data], [join("repo", "origin"), data]]);
  const read = (file: string) => files.get(file)!;
  checkLegacySources("repo", read);
  for (const name of ["copy", "origin"]) {
    files.set(join("repo", name), Buffer.from("tampered"));
    assert.throws(() => checkLegacySources("repo", read), /drift/);
    files.set(join("repo", name), data);
  }
  for (const invalid of ["{}", "{", JSON.stringify({ contract: "other", files: manifest.files })]) {
    files.set(join("repo", "src/legacy/sources.json"), Buffer.from(invalid));
    assert.throws(() => checkLegacySources("repo", read));
  }
});
test("module resources reject modern identity, wrong versions, and runtime RTTI", () => {
  const info = { OriginalFilename: "v8jsi.dll", ProductName: "React Native V8 JSI Adapter", FileVersion: "0.79.8.0", ProductVersion: "0.79.8" };
  checkLegacyIdentity(Buffer.alloc(0), info, "0.79.8");
  for (const field of ["OriginalFilename", "ProductName", "FileVersion", "ProductVersion"]) {
    assert.throws(() => checkLegacyIdentity(Buffer.alloc(0), { ...info, [field]: "wrong" }, "0.79.8"), /resource mismatch/);
  }
  for (const data of ["v8jsi2", ".?AVV8Runtime@v8runtime@@"]) {
    assert.throws(() => checkLegacyIdentity(Buffer.from(data), info, "0.79.8"), /identity\/RTTI/);
  }
});
test("CI package versions require numeric FileVersion and the exact prerelease ProductVersion", () => {
  const version = "0.79.44-g8c3c4ccc";
  const info = { OriginalFilename: "v8jsi.dll", ProductName: "React Native V8 JSI Adapter",
    FileVersion: "0.79.44.0", ProductVersion: version };
  checkLegacyIdentity(Buffer.alloc(0), info, version);
  // Numeric drift and same-core builds from different commits are distinct mismatches.
  for (const changed of [{ FileVersion: "0.79.43.0" }, { FileVersion: version + ".0" },
                         { ProductVersion: "0.79.44-g344c6be7" }, { ProductVersion: "0.79.44" }]) {
    assert.throws(() => checkLegacyIdentity(Buffer.alloc(0), { ...info, ...changed }, version), /resource mismatch/);
  }
});
test("compiler checks require SHA-256, GS and the static CRT in both configurations", () => {
  const group = (config: string, runtime: string) => `<ItemDefinitionGroup Condition="${config}|x64"><ClCompile>/ZH:SHA_256<BufferSecurityCheck>true</BufferSecurityCheck><RuntimeLibrary>${runtime}</RuntimeLibrary></ClCompile></ItemDefinitionGroup>`;
  const valid = group("Debug", "MultiThreadedDebug") + group("Release", "MultiThreaded");
  checkLegacyHardeningProject(valid);
  for (const bad of ["", valid.replace("/ZH:SHA_256", ""), valid.replace("true", "false"), valid.replace("MultiThreadedDebug", "MultiThreadedDebugDLL")]) {
    assert.throws(() => checkLegacyHardeningProject(bad), /Missing|missing/);
  }
});
