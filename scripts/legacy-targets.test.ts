// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
import assert from "node:assert/strict";
import { test } from "node:test";
import { execFileSync, spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { tmpdir } from "node:os";
import { repoRoot } from "./build-common.ts";

function evaluate(properties: string[]): { status: number | null; stdout: string; stderr: string } {
  const root = fs.mkdtempSync(path.join(tmpdir(), "legacy-targets-"));
  try {
    const native = path.join(root, "build/native");
    fs.mkdirSync(native, { recursive: true });
    fs.copyFileSync(path.join(repoRoot, "ReactNative.V8Jsi.Windows.targets"), path.join(native, "ReactNative.V8Jsi.Windows.targets"));
    for (const config of ["Debug", "Release"]) for (const arch of ["x86", "x64", "arm64"]) {
      const cell = path.join(root, "lib/win32", config, arch);
      fs.mkdirSync(cell, { recursive: true });
      fs.writeFileSync(path.join(cell, "v8jsi.dll"), `${config}/${arch}`);
    }
    const project = path.join(root, "consumer.proj");
    fs.writeFileSync(project, `<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
      <Import Project="build/native/ReactNative.V8Jsi.Windows.targets"/>
      <ItemGroup><Link Include="link"/><ClCompile Include="source"/></ItemGroup>
      <Target Name="PrepareForBuild"/>
      <Target Name="Check" DependsOnTargets="PrepareForBuild">
        <Message Importance="high" Text="LIB=%(Link.AdditionalLibraryDirectories)"/>
        <Message Importance="high" Text="DEP=%(Link.AdditionalDependencies)"/>
        <Message Importance="high" Text="DELAY=%(Link.DelayLoadDLLs)"/>
        <Message Importance="high" Text="INCLUDE=%(ClCompile.AdditionalIncludeDirectories)"/>
        <Message Importance="high" Text="COPY=@(ReferenceCopyLocalPaths)"/>
        <Copy SourceFiles="@(ReferenceCopyLocalPaths)" DestinationFolder="copied"/>
      </Target></Project>`);
    const vswhere = path.join(process.env["ProgramFiles(x86)"]!, "Microsoft Visual Studio/Installer/vswhere.exe");
    const msbuild = execFileSync(vswhere, ["-latest", "-prerelease", "-products", "*", "-find", "MSBuild\\**\\Bin\\MSBuild.exe"],
      { encoding: "utf8" }).trim().split(/\r?\n/)[0];
    const result = spawnSync(msbuild, [project, "/t:Check", "/v:minimal", ...properties.map(value => "/p:" + value)], { encoding: "utf8" });
    if (result.error) throw result.error;
    const copy = /COPY=(.*)/.exec(result.stdout)?.[1].trim();
    if (copy && result.status === 0) assert.equal(fs.readFileSync(path.join(root, "copied/v8jsi.dll"), "utf8"),
      fs.readFileSync(copy, "utf8"));
    return result;
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
}

// Platform and configuration select distinct package cells, including the Win32 alias.
for (const config of ["Debug", "Release"]) for (const [platform, arch] of [["Win32", "x86"], ["x64", "x64"], ["ARM64", "ARM64"]]) {
  test(`${platform} ${config} links, delay-loads and copies only the selected legacy DLL`, () => {
    const result = evaluate([`Platform=${platform}`, `Configuration=${config}`]);
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.match(result.stdout, new RegExp(`LIB=.*lib\\\\win32\\\\${config}\\\\${arch};`));
    assert.match(result.stdout, /DEP=v8jsi\.dll\.lib;/);
    assert.match(result.stdout, /DELAY=v8jsi\.dll;/);
    assert.match(result.stdout, /INCLUDE=.*include;/);
    assert.match(result.stdout, /COPY=.*v8jsi\.dll\s/);
    assert.doesNotMatch(result.stdout, /\.pdb|MSB3030/);
  });
}
for (const value of ["true", "false", "0"]) {
  test(`nonempty V8JsiNoDLLCopy=${value} retains the original no-copy semantics`, () => {
    const result = evaluate(["Platform=x64", "Configuration=Release", `V8JsiNoDLLCopy=${value}`]);
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.match(result.stdout, /COPY=\s*$/);
    assert.match(result.stdout, /DEP=v8jsi\.dll\.lib;/);
  });
}
test("explicit V8Platform takes precedence over Platform", () => {
  const result = evaluate(["Platform=Custom", "V8Platform=x86", "Configuration=Release"]);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(result.stdout, /LIB=.*Release\\x86;/);
});
test("sandbox requests fail before copying any payload", () => {
  const result = evaluate(["Platform=x64", "Configuration=Release", "UseV8Sandbox=True"]);
  assert.notEqual(result.status, 0);
  assert.match(result.stdout, /UseV8Sandbox is not supported/);
  assert.doesNotMatch(result.stdout, /COPY=/);
});
