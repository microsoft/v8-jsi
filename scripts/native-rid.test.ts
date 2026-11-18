// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import { checkV8JsiPayload, readV8JsiNames, repoRoot, vswhereExe } from "./build-common.ts";

const namesHeader = readFileSync(path.join(repoRoot, "src/public/v8jsi_names.h"), "utf8");

test("shared DLL names are narrow and wide literals", () => {
  assert.deepEqual(readV8JsiNames(), {
    V8JSI_DLL_NAME: "v8jsi2.dll", V8JSI_SB_DLL_NAME: "v8jsi2sb.dll", V8SBHOST_DLL_NAME: "v8sbhost.dll",
  });
});

// Each macro must independently reject missing, computed, duplicate, or mismatched definitions.
for (const key of ["V8JSI_DLL_NAME", "V8JSI_SB_DLL_NAME", "V8SBHOST_DLL_NAME"]) {
  for (const suffix of ["", "_W"]) {
    const macro = `${key}${suffix}`;
    const line = namesHeader.split(/\r?\n/).find((line) => line.startsWith(`#define ${macro} `))!;
    test(`${macro} rejects missing, nonliteral, and duplicate definitions`, () => {
      for (const replacement of ["", `#define ${macro} OTHER_NAME`, `${line} + "suffix"`, `${line}\n${line}`]) {
        assert.throws(() => readV8JsiNames(namesHeader.replace(line, replacement)), /must be a single/);
      }
    });
  }
  test(`${key} rejects a different wide name`, () => {
    const changed = namesHeader.replace(new RegExp(`(#define ${key}_W L")[^"]+`), '$1different.dll');
    assert.throws(() => readV8JsiNames(changed), /does not match/);
  });
}

test("pre-staged payload matches shared mainline and optional sandbox names", () => {
  const dir = mkdtempSync(path.join(os.tmpdir(), "v8jsi-payload-"));
  const names = readV8JsiNames();
  try {
    assert.throws(() => checkV8JsiPayload(dir, names), /Missing v8jsi2\.dll/);
    writeFileSync(path.join(dir, names.V8JSI_DLL_NAME), "");
    checkV8JsiPayload(dir, names); // No local build or optional PDB/import lib is needed.
    assert.throws(() => checkV8JsiPayload(dir, { ...names, V8JSI_DLL_NAME: "v8jsi3.dll" }), /Stale engine payload/);
    // Reject obsolete DLLs and sidecars even alongside a correctly named engine.
    for (const file of ["v8jsi.dll", "v8jsi.dll.lib", "v8jsi.dll.pdb", "v8jsi.lib", "v8jsi.pdb"]) {
      writeFileSync(path.join(dir, file), "");
      assert.throws(() => checkV8JsiPayload(dir, names), /Stale engine payload/);
      rmSync(path.join(dir, file));
    }
    for (const name of Object.values(names)) {
      for (const suffix of ["", ".lib", ".pdb"]) writeFileSync(path.join(dir, name + suffix), "");
    }
    checkV8JsiPayload(dir, names);
    // Each optional producer must independently reject header/output drift.
    for (const key of ["V8JSI_SB_DLL_NAME", "V8SBHOST_DLL_NAME"] as const) {
      assert.throws(() => checkV8JsiPayload(dir, { ...names, [key]: "different.dll" }), /Stale engine payload/);
    }
    for (const file of ["v8jsisb.dll", "v8jsisb.dll.lib", "v8jsisb.dll.pdb", "v8jsisb.pdb",
      "v8host.dll", "v8host.dll.lib", "v8host.dll.pdb"]) {
      writeFileSync(path.join(dir, file), "");
      assert.throws(() => checkV8JsiPayload(dir, names), /Stale engine payload/);
      rmSync(path.join(dir, file));
    }
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

// The CLI must fail closed even when only pre-staged aggregator inputs exist.
test("pack CLI rejects stale pre-staged aggregator payload before packaging", () => {
  const stale = "v8jsi.dll";
  const dir = mkdtempSync(path.join(os.tmpdir(), "v8jsi-pack-"));
  try {
    // Relocate the real CLI and its imports so local build outputs cannot mask the aggregator path.
    for (const file of ["scripts/build.ts", "scripts/build-common.ts", "scripts/source-link-check.ts",
      "config.json", "src/public/v8jsi_names.h"]) {
      const dest = path.join(dir, file);
      mkdirSync(path.dirname(dest), { recursive: true });
      copyFileSync(path.join(repoRoot, file), dest);
    }
    const output = path.join(dir, "out");
    const ridDir = path.join(output, "pkg-staging/build/native/win-x64");
    mkdirSync(ridDir, { recursive: true });
    writeFileSync(path.join(ridDir, readV8JsiNames().V8JSI_DLL_NAME), "");
    writeFileSync(path.join(ridDir, stale), "");
    const result = spawnSync(process.execPath, [path.join(dir, "scripts/build.ts"), "--no-build", "--pack",
      "--platform", "x64", "--configuration", "release", "--output-path", output], { encoding: "utf8" });
    assert.equal(result.error, undefined);
    assert.equal(result.status, 1, result.stdout + result.stderr);
    assert.match(result.stdout, /Using pre-staged v8jsi2\.dll for win-x64/);
    assert.match(result.stderr, new RegExp(`Stale engine payload ${stale.replaceAll(".", "\\.")}`));
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

// Missing and malformed headers must not break unrelated CLI commands, but size checks fail closed.
for (const header of [undefined, "#define V8JSI_SB_DLL_NAME OTHER_NAME"]) {
  test(`help works but size checks reject a ${header === undefined ? "missing" : "malformed"} names header`, () => {
    const dir = mkdtempSync(path.join(os.tmpdir(), "v8jsi-help-"));
    try {
      for (const file of ["build.ts", "build-common.ts", "source-link-check.ts"]) {
        mkdirSync(path.join(dir, "scripts"), { recursive: true });
        copyFileSync(path.join(repoRoot, "scripts", file), path.join(dir, "scripts", file));
      }
      if (header !== undefined) {
        mkdirSync(path.join(dir, "src/public"), { recursive: true });
        writeFileSync(path.join(dir, "src/public/v8jsi_names.h"), header);
      }
      const invoke = (...args: string[]) => spawnSync(process.execPath,
        [path.join(dir, "scripts/build.ts"), ...args], { encoding: "utf8", cwd: dir });
      const help = invoke("--help");
      assert.equal(help.status, 0, help.stdout + help.stderr);
      assert.match(help.stdout, /Usage: node scripts\/build\.ts/);
      assert.equal(help.stderr, "");
      const size = invoke("--no-build", "--check-size", "--build-v8jsisb");
      assert.equal(size.status, 1, size.stdout + size.stderr);
      assert.match(size.stderr, header === undefined ? /ENOENT.*v8jsi_names\.h/ : /must be a single/);
    } finally { rmSync(dir, { recursive: true, force: true }); }
  });
}

test("native package RID selection", { skip: process.platform !== "win32" ? "Native RID imports require Windows MSBuild" : false }, async (t) => {
  const vswhere = vswhereExe();
  assert.ok(vswhere, "Visual Studio Installer vswhere.exe is required on Windows");
  const msbuild = execFileSync(vswhere, ["-latest", "-products", "*", "-find", "MSBuild\\**\\Bin\\MSBuild.exe"],
    { encoding: "utf8" }).trim().split(/\r?\n/)[0];
  assert.ok(msbuild, "Visual Studio MSBuild.exe is required on Windows");
  // MSBuild expands short directory names when reporting consumer paths.
  const dir = realpathSync.native(mkdtempSync(path.join(os.tmpdir(), "v8jsi-rid-")));
  const rids = ["win-x64", "win-arm64", "win-x86"];
  const root = (rid: string) => path.join(dir, rid);
  const native = (rid: string) => path.join(root(rid), "build", "native");
  try {
    for (const rid of rids) {
      mkdirSync(native(rid), { recursive: true });
      for (const ext of ["props", "targets"]) {
        const template = readFileSync(path.join(repoRoot, "scripts", "nuget", `Microsoft.JavaScript.V8.native.rid.${ext}`), "utf8");
        writeFileSync(path.join(native(rid), `${rid}.${ext}`), template.replaceAll("$rid$", rid).replaceAll("$v8jsiName$", "alternate.dll"));
      }
      for (const file of [`build/native/${rid}/alternate.dll.lib`, `runtimes/${rid}/native/alternate.dll`, `runtimes/${rid}/native/alternate.dll.pdb`]) {
        mkdirSync(path.dirname(path.join(root(rid), file)), { recursive: true });
        writeFileSync(path.join(root(rid), file), "");
      }
    }
    // A substituted name must reach both the source path and net461 TargetPath.
    for (const dll of [readV8JsiNames().V8JSI_DLL_NAME, "alternate.dll"]) {
      await t.test(`net461 copies ${dll} with its RID TargetPath`, () => {
        const rid = "win-x64";
        const netDir = path.join(root(rid), "buildTransitive", "net461");
        mkdirSync(netDir, { recursive: true });
        for (const ext of ["props", "targets"]) {
          const template = readFileSync(path.join(repoRoot, "scripts/nuget", `Microsoft.JavaScript.V8.net461.rid.${ext}`), "utf8");
          writeFileSync(path.join(netDir, `runtime.${ext}`), template.replaceAll("$rid$", rid)
            .replaceAll("$v8jsiName$", dll));
        }
        const project = path.join(dir, "net461.proj");
        writeFileSync(project, `<Project><Import Project="${netDir}/runtime.props"/><Import Project="${netDir}/runtime.targets"/></Project>`);
        const result = spawnSync(msbuild, [project, "-nologo", "-getItem:None"], { encoding: "utf8" });
        assert.equal(result.status, 0, result.stdout + result.stderr);
        const [item] = JSON.parse(result.stdout).Items.None;
        assert.equal(item.FullPath, path.join(root(rid), "runtimes", rid, "native", dll));
        assert.equal(item.TargetPath, `runtimes\\${rid}\\native\\${dll}`);
        assert.equal(item.CopyToOutputDirectory, "PreserveNewest");
        assert.equal(item.Pack, "false");
        const disabled = spawnSync(msbuild, [project, "-nologo", "-getItem:None", "-p:V8JsiCopyDLL=false"], { encoding: "utf8" });
        assert.equal(disabled.status, 0, disabled.stdout + disabled.stderr);
        assert.deepEqual(JSON.parse(disabled.stdout).Items.None, []);
      });
    }
    const project = path.join(dir, "consumer.proj");
    function evaluate(platform: string, extra: string[] = [], imports = rids, projectRid = "") {
      writeFileSync(project, `<Project><PropertyGroup><V8JsiRuntimeIdentifier>${projectRid}</V8JsiRuntimeIdentifier></PropertyGroup>
        ${imports.map((rid) => `<Import Project="${rid}/build/native/${rid}.props"/><Import Project="${rid}/build/native/${rid}.targets"/>`).join("")}
        <ItemGroup><Link Include="consumer.obj"/><ClCompile Include="consumer.cpp"/></ItemGroup><Target Name="PrepareForBuild"/></Project>`);
      return spawnSync(msbuild, [project, "-nologo", "-v:q", "-t:PrepareForBuild", `-p:Platform=${platform}`,
        "-getProperty:V8JsiRuntimeIdentifier", "-getItem:Link,ClCompile,ReferenceCopyLocalPaths,None", ...extra], { encoding: "utf8" });
    }
    // Cover platform aliases and both global and project-level explicit overrides.
    for (const [platform, rid, override] of [["ARM64EC", "win-x64"], ["X64", "win-x64"], ["Arm64", "win-arm64"],
      ["X86", "win-x86"], ["Win32", "win-x86"], ["ARM64EC", "win-arm64", "global"], ["ARM64EC", "win-arm64", "project"],
      ...["Win-X64", "WIN-Arm64", "Win-X86"].flatMap((rid) => [
        ["ARM64EC", rid, "global"], ["ARM64EC", rid, "project"]])]) {
      await t.test(`${platform} selects only ${rid}${override ? ` with ${override} override` : ""}`, () => {
        const result = evaluate(platform, override === "global" ? [`-p:V8JsiRuntimeIdentifier=${rid}`] : [], rids, override === "project" ? rid : "");
        assert.equal(result.status, 0, result.stdout + result.stderr);
        const { Properties, Items } = JSON.parse(result.stdout);
        assert.equal(Properties.V8JsiRuntimeIdentifier, rid);
        const selected = rid.toLowerCase();
        assert.match(Items.Link[0].AdditionalDependencies, /^alternate\.dll\.lib;delayimp\.lib;/);
        assert.equal(Items.Link[0].DelayLoadDLLs.split(";")[0], "alternate.dll");
        assert.equal(Items.Link[0].AdditionalLibraryDirectories.split(";")[0], path.join(native(selected), selected));
        assert.deepEqual(Items.ReferenceCopyLocalPaths.map((item: { FullPath: string }) => item.FullPath),
          ["alternate.dll", "alternate.dll.pdb"].map((file) => path.join(root(selected), "runtimes", selected, "native", file)));
        assert.equal(Items.None.length, 1);
        assert.equal(Items.None[0].FullPath, Items.ReferenceCopyLocalPaths[0].FullPath);
        assert.equal(Items.None[0].CopyToOutputDirectory, "PreserveNewest");
        assert.equal(Items.ClCompile.length, 4);
        assert.ok(Items.ClCompile.every((item: { AdditionalIncludeDirectories: string }) =>
          item.AdditionalIncludeDirectories.includes(path.join(native(selected), "include"))));
        // Item metadata covers includes, library dirs, dependencies, sources, and both copy item types.
        for (const other of rids.filter((candidate) => candidate !== selected)) {
          assert.ok(!JSON.stringify(Items).toLowerCase().includes(JSON.stringify(root(other)).slice(1, -1).toLowerCase()),
            `unexpected ${other} package root in consumer inputs`);
        }
      });
    }
    // Each C-only surface needs the names root (and Node-API its C ABI types) with JSI disabled.
    for (const sandbox of [false, true]) {
      await t.test(`${sandbox ? "sandbox-only" : "Node-API-only"} includes the names root with JSI public headers disabled`, () => {
        const result = evaluate("X64", ["-p:V8JsiIncludePublicHeaders=false", "-p:V8JsiAddConsumerSourceTUs=false",
          `-p:V8JsiIncludeNodeApiHeaders=${!sandbox}`, `-p:UseV8Sandbox=${sandbox}`, "-p:OutDir=bin/x64"]);
        assert.equal(result.status, 0, result.stdout + result.stderr);
        const { Items } = JSON.parse(result.stdout);
        assert.equal(Items.ClCompile.length, 1);
        const includes = Items.ClCompile[0].AdditionalIncludeDirectories.split(";").filter(Boolean);
        assert.deepEqual(includes, ["include", ...(sandbox ? ["include/v8sbhost"] : ["include/node-api", "include/node-api-jsi", "jsi"])]
          .map((dir) => path.join(native("win-x64"), dir)));
      });
    }
    // Link and copy opt-outs are independent; turning either off must not disable the other.
    for (const property of ["V8JsiAddLibDependency", "V8JsiCopyDLL"]) {
      await t.test(`${property} opt-out preserves the other engine wiring`, () => {
        const result = evaluate("X64", [`-p:${property}=false`]);
        assert.equal(result.status, 0, result.stdout + result.stderr);
        const { Items } = JSON.parse(result.stdout);
        if (property === "V8JsiAddLibDependency") {
          assert.ok(!Items.Link[0].AdditionalDependencies);
          assert.ok(!Items.Link[0].DelayLoadDLLs);
          assert.equal(Items.ReferenceCopyLocalPaths.length, 2);
          assert.equal(Items.None.length, 1);
        } else {
          assert.match(Items.Link[0].AdditionalDependencies, /^alternate\.dll\.lib;delayimp\.lib;/);
          assert.equal(Items.Link[0].DelayLoadDLLs.split(";")[0], "alternate.dll");
          assert.deepEqual(Items.ReferenceCopyLocalPaths, []);
          assert.deepEqual(Items.None, []);
        }
      });
    }
    // A nonempty list without a leading separator makes the empty-RID guard observable.
    for (const platform of ["ARM", "Custom"]) {
      await t.test(`${platform} leaves engine inputs unwired`, () => {
        const result = evaluate(platform, ["-p:V8JsiAvailableRuntimeIdentifiers=win-x64"]);
        assert.equal(result.status, 0, result.stdout + result.stderr);
        const { Properties, Items } = JSON.parse(result.stdout);
        assert.equal(Properties.V8JsiRuntimeIdentifier, "");
        assert.deepEqual(Items.Link.map((item: { Identity: string }) => item.Identity), ["consumer.obj"]);
        assert.ok(!Items.Link[0].AdditionalDependencies);
        assert.ok(!Items.Link[0].AdditionalLibraryDirectories);
        assert.equal(Items.ClCompile.length, 1);
        assert.ok(!Items.ClCompile[0].AdditionalIncludeDirectories);
        assert.deepEqual(Items.ReferenceCopyLocalPaths, []);
        assert.deepEqual(Items.None, []);
      });
    }
    await t.test("rejects a partial RID rather than matching an available RID substring", () => {
      const result = evaluate("X64", ["-p:V8JsiRuntimeIdentifier=Win-X6"]);
      assert.notEqual(result.status, 0);
      assert.match(result.stdout + result.stderr, /V8JSI001.*not available/);
    });
    // Exercise sandbox-only RID conditions as well as the normal engine inputs above.
    for (const [platform, rid] of [["X64", "Win-X64"], ["ARM64", "WIN-Arm64"]]) {
      await t.test(`${rid} wires sandbox inputs and validates the output directory`, () => {
        const selected = rid.toLowerCase();
        const args = ["-p:UseV8Sandbox=true", `-p:V8JsiRuntimeIdentifier=${rid}`];
        const result = evaluate(platform, [...args, `-p:OutDir=bin/${platform}`]);
        assert.equal(result.status, 0, result.stdout + result.stderr);
        const { Items } = JSON.parse(result.stdout);
        assert.match(Items.Link[0].AdditionalDependencies, /v8sbhost\.dll\.lib;/);
        assert.ok(Items.ClCompile.every((item: { AdditionalIncludeDirectories: string }) =>
          item.AdditionalIncludeDirectories.includes(path.join(native(selected), "include", "v8sbhost"))));
        assert.deepEqual(Items.ReferenceCopyLocalPaths.slice(2).map((item: { FullPath: string }) => item.FullPath),
          ["v8jsi2sb.dll", "sbox.exe", "v8sbhost.dll"].map((file) => path.join(root(selected), "runtimes", selected, "native", file)));
        assert.deepEqual(Items.None.slice(1).map((item: { FullPath: string }) => item.FullPath),
          Items.ReferenceCopyLocalPaths.slice(2).map((item: { FullPath: string }) => item.FullPath));
        const invalid = evaluate(platform, [...args, "-p:OutDir=bin/wrong"]);
        assert.notEqual(invalid.status, 0);
        assert.match(invalid.stdout + invalid.stderr, /V8SBHOST001/);
      });
    }
    for (const [platform, rid] of [["X86", "Win-X86"], ["ARM64", "Win-X64"]]) {
      await t.test(`${platform} rejects unsupported sandbox selection ${rid}`, () => {
        const result = evaluate(platform, ["-p:UseV8Sandbox=true", `-p:V8JsiRuntimeIdentifier=${rid}`]);
        assert.notEqual(result.status, 0);
        assert.match(result.stdout + result.stderr, /clients need the cross-architecture install layout/);
      });
    }
    // Only x64 is imported: the selected package is wholly absent, not merely incomplete.
    for (const platform of ["Win32", "ARM64"]) {
      await t.test(`${platform} rejects an unimported RID package`, () => {
        const result = evaluate(platform, [], ["win-x64"]);
        assert.notEqual(result.status, 0);
        assert.match(result.stdout + result.stderr, /V8JSI001.*not available.*no architecture fallback/);
      });
    }
    // Sandbox rejection must precede missing-RID validation, even with no matching import.
    for (const override of ["", "win-arm64", "Win-X64"]) {
      await t.test(`ARM64EC rejects sandbox${override ? ` with RID override ${override}` : ""}`, () => {
        const result = evaluate("ARM64EC", ["-p:UseV8Sandbox=true"], ["win-x64"], override);
        assert.notEqual(result.status, 0);
        assert.match(result.stdout + result.stderr, /ARM64EC clients need the cross-architecture install layout/);
      });
    }
    // Leave other architectures present: a missing half of the selected pair must never fall back.
    for (const [file, rid] of ["build/native/win-x64/alternate.dll.lib", "runtimes/win-x64/native/alternate.dll"]
      .flatMap((file) => ["win-x64", "Win-X64"].map((rid) => [file, rid]))) {
      await t.test(`${rid} rejects incomplete imported package missing ${path.basename(file)}`, () => {
        const fullPath = path.join(root("win-x64"), file);
        rmSync(fullPath);
        try {
          const result = evaluate("ARM64EC", [`-p:V8JsiRuntimeIdentifier=${rid}`]);
          assert.notEqual(result.status, 0);
          const output = result.stdout + result.stderr;
          assert.match(output, /V8JSI001.*win-x64.*imported RID package is incomplete.*no architecture fallback/);
          assert.ok(output.includes(fullPath), "diagnostic must print a normalized missing path");
        } finally {
          writeFileSync(fullPath, "");
        }
      });
    }
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
