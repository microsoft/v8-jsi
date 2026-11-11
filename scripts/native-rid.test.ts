// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import { repoRoot, vswhereExe } from "./build-common.ts";

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
        writeFileSync(path.join(native(rid), `${rid}.${ext}`), template.replaceAll("$rid$", rid).replaceAll("$v8jsiName$", "v8jsi.dll"));
      }
      for (const file of [`build/native/${rid}/v8jsi.dll.lib`, `runtimes/${rid}/native/v8jsi.dll`, `runtimes/${rid}/native/v8jsi.dll.pdb`]) {
        mkdirSync(path.dirname(path.join(root(rid), file)), { recursive: true });
        writeFileSync(path.join(root(rid), file), "");
      }
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
        assert.match(Items.Link[0].AdditionalDependencies, /^v8jsi\.dll\.lib;delayimp\.lib;/);
        assert.equal(Items.Link[0].AdditionalLibraryDirectories.split(";")[0], path.join(native(selected), selected));
        assert.deepEqual(Items.ReferenceCopyLocalPaths.map((item: { FullPath: string }) => item.FullPath),
          ["v8jsi.dll", "v8jsi.dll.pdb"].map((file) => path.join(root(selected), "runtimes", selected, "native", file)));
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
        assert.match(Items.Link[0].AdditionalDependencies, /v8host\.dll\.lib;/);
        assert.ok(Items.ClCompile.every((item: { AdditionalIncludeDirectories: string }) =>
          item.AdditionalIncludeDirectories.includes(path.join(native(selected), "include", "v8host"))));
        assert.deepEqual(Items.ReferenceCopyLocalPaths.slice(2).map((item: { FullPath: string }) => item.FullPath),
          ["v8jsisb.dll", "sbox.exe", "v8host.dll"].map((file) => path.join(root(selected), "runtimes", selected, "native", file)));
        assert.deepEqual(Items.None.slice(1).map((item: { FullPath: string }) => item.FullPath),
          Items.ReferenceCopyLocalPaths.slice(2).map((item: { FullPath: string }) => item.FullPath));
        const invalid = evaluate(platform, [...args, "-p:OutDir=bin/wrong"]);
        assert.notEqual(invalid.status, 0);
        assert.match(invalid.stdout + invalid.stderr, /V8HOST001/);
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
    for (const [file, rid] of ["build/native/win-x64/v8jsi.dll.lib", "runtimes/win-x64/native/v8jsi.dll"]
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
