// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import assert from "node:assert/strict";
import { readFileSync, mkdtempSync, mkdirSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import * as path from "node:path";
import { spawnSync } from "node:child_process";
import { test } from "node:test";

const read = (file: string) => readFileSync(new URL("../" + file, import.meta.url), "utf8").replaceAll("\r\n", "\n");
const jobs = read(".ado/windows-jobs.yml");
const job = (name: string) => jobs.split(`- job: ${name}\n`)[1]?.split(/\n\s*- job:/)[0] ?? "";

test("legacy Guardian receives adjacent PDBs only after packaging", () => {
  const pack = job("V8JsiCreateNugetLegacy");
  const copy = pack.match(/Copy-Item .*v8jsi\.dll\.pdb.*-ErrorAction Stop/)![0];
  assert.ok(pack.indexOf(copy) > pack.indexOf("if ($LASTEXITCODE -ne 0) { throw 'Legacy pack failed' }"));
  const root = mkdtempSync(path.join(tmpdir(), "legacy-guardian-"));
  try {
    // One-byte symbols in all six cells verify the expanded pipeline paths without binary fixtures.
    for (const platform of ["x64", "x86", "arm64"]) for (const configuration of ["Debug", "Release"]) {
      const name = `Desktop_${platform === "arm64" ? "ARM64" : platform}_${configuration}`;
      const source = path.join(root, "legacy-cells", name, "symbols", `${platform}-${configuration}`, "v8jsi.dll.pdb");
      const destination = path.join(root, "out/pkg-staging/lib/win32", configuration, platform, "v8jsi.dll.pdb");
      mkdirSync(path.dirname(source), { recursive: true });
      mkdirSync(path.dirname(destination), { recursive: true });
      writeFileSync(source, platform[0]);
      const script = copy.replaceAll("$(Build.StagingDirectory)", root)
        .replaceAll("${{ matrix.Name }}", name).replaceAll("${{ matrix.BuildPlatform }}", platform)
        .replaceAll("${{ matrix.BuildConfiguration }}", configuration);
      const result = runPowerShell(script);
      assert.equal(result.status, 0, result.stderr);
      assert.equal(readFileSync(destination, "utf8"), platform[0]);
      rmSync(source);
      assert.notEqual(runPowerShell(script).status, 0, "Missing symbols must fail the job");
    }
  } finally { rmSync(root, { recursive: true, force: true }); }
});

const release = read(".ado/windows-release.yml");
const releaseJob = (name: string) => release.split(`- job: ${name}\n`)[1]?.split(/\n\s*- job:/)[0] ?? "";
const runPowerShell = (script: string, env = {}) => spawnSync("pwsh", ["-NoProfile", "-NonInteractive", "-Command", script],
  { encoding: "utf8", timeout: 30_000, env: { ...process.env, ...env } });

test("release gate rejects non-main and unresolved branches", () => {
  const gate = release.match(/- pwsh: \|\n([\s\S]*?)\n          name: gate/)![1];
  // Exercise resolved, non-main, missing, and unresolved resource branches against the real gate script.
  for (const branch of ["refs/heads/main", "refs/heads/legacy/test", "", "$(resources.pipeline.microsoft.v8-jsi.ci.sourceBranch)"]) {
    const result = runPowerShell(gate, { CI_SOURCE_BRANCH: branch, CI_RUN_NAME: "123 - 0.79.44", BUILD_BUILDNUMBER: "v8jsi_release_0.0.1" });
    assert.equal(result.status, 0, result.stderr);
    assert.ok(result.stdout.includes(`variable=shouldRelease;isOutput=true]${branch === "refs/heads/main" ? "True" : "False"}`));
  }
});

test("legacy packages publish only to internal feeds and nuget.org stays LibNode-only", () => {
  const glob = "$(Pipeline.Workspace)/published-packages-legacy/ReactNative.V8Jsi.Windows.*.nupkg";
  for (const prefix of ["ms_react_native_nuget", "office_nuget"]) {
    const body = releaseJob(`${prefix}_job_legacy`);
    const pushes = [...body.matchAll(/packagesToPush: ['"]?([^'"\n]+)/g)].map(match => match[1].trim());
    assert.deepEqual(pushes, [glob]);
    assert.deepEqual([...body.matchAll(/- template: ([^\n]+)/g)].map(match => match[1]),
      [`.ado/templates/publish-nuget-to-${prefix === "office_nuget" ? "office" : "ado"}-feed.yml@self`]);
    assert.deepEqual([...body.matchAll(/nugetFeedUrl: ['"]?([^'"\n]+)/g)].map(match => match[1].trim()),
      prefix === "office_nuget" ? [] : ["https://pkgs.dev.azure.com/ms/react-native/_packaging/react-native-public/nuget/v3/index.json"]);
    assert.doesNotMatch(body, /nuget\.org|EsrpRelease|libnode-packages/);
  }
  const publicJobs = release.split(/\n      - job: /).slice(1).filter(body => /nuget\.org|EsrpRelease@/.test(body));
  assert.equal(publicJobs.length, 1);
  assert.match(publicJobs[0], /Contents: 'Microsoft\.JavaScript\.LibNode\.\*\.nupkg'/);
  assert.doesNotMatch(publicJobs[0], /ReactNative\.V8Jsi\.Windows|published-packages-legacy/);
});

test("legacy symbols compare every cell with the packed package and reject absent or differing images", () => {
  const script = releaseJob("publish_symbols_job_legacy").match(/- pwsh: \|\n([\s\S]*?)\n          displayName:/)![1]
    .replace(/\$vswhere = [\s\S]*?if \(-not \$dumpbin[^\n]+/, "$dumpbin = 'ReadHeaders'; $pdbutil = 'ReadPdb'");
  // Six one-byte cells and textual PDB-tool outputs exercise the real ZIP, path selection, and hashes.
  for (const mode of ["valid", "mismatch", "missing-entry", "missing-package", "multiple-packages"]) {
    const root = mkdtempSync(path.join(tmpdir(), "legacy-symbol-package-"));
    try {
      for (const platform of ["x64", "x86", "arm64"]) for (const configuration of ["Debug", "Release"]) {
        const name = `Desktop_${platform === "arm64" ? "ARM64" : platform}_${configuration}-legacy`;
        const source = path.join(root, "legacy-symbol-inputs", name, "symbols", `${platform}-${configuration}`);
        mkdirSync(source, { recursive: true });
        writeFileSync(path.join(source, "v8jsi.dll"), platform[0]);
        writeFileSync(path.join(source, "v8jsi.dll.pdb"), "p");
        const payload = path.join(root, "payload/lib/win32", configuration, platform);
        mkdirSync(payload, { recursive: true });
        if (mode !== "missing-entry" || platform !== "x86" || configuration !== "Release") {
          writeFileSync(path.join(payload, "v8jsi.dll"), mode === "mismatch" && platform === "arm64" && configuration === "Debug" ? "!" : platform[0]);
        }
      }
      const packages = path.join(root, "published-packages-legacy");
      mkdirSync(packages);
      const setup = `
        $ErrorActionPreference = 'Stop'
        Compress-Archive -Path '${root}/payload/*' -DestinationPath '${root}/package.zip'
        if ('${mode}' -ne 'missing-package') {
          Copy-Item '${root}/package.zip' '${packages}/ReactNative.V8Jsi.Windows.0.79.8.nupkg'
        }
        if ('${mode}' -eq 'multiple-packages') {
          Copy-Item '${root}/package.zip' '${packages}/ReactNative.V8Jsi.Windows.0.79.9.nupkg'
        }
        function Get-AuthenticodeSignature { [pscustomobject]@{ Status = 'Valid' } }
        function ReadHeaders { $global:LASTEXITCODE = 0; 'Format: RSDS, {AABB-CCDD}, 1' }
        function ReadPdb {
          $global:LASTEXITCODE = 0
          'GUID: {AABB-CCDD}'; 'Age: 1'; 'Is stripped: false'; 'Has Types: true'; 'Has Debug Info: true'
        }
      `;
      const result = runPowerShell(setup + script.replaceAll("$(Pipeline.Workspace)", root));
      assert.equal(result.status === 0, mode === "valid", `${mode}: ${result.stderr}`);
      if (mode === "valid") assert.equal((result.stdout.match(/Verified /g) ?? []).length, 6);
      if (mode === "mismatch") assert.match(result.stderr, /Symbol image differs from package/);
      if (mode === "missing-entry") assert.match(result.stderr, /Cannot find path/);
      if (mode.endsWith("packages") || mode === "missing-package") assert.match(result.stderr, /Exactly one legacy package is required/);
    } finally { rmSync(root, { recursive: true, force: true }); }
  }
});

test("legacy symbol validation rejects missing unreadable mismatched stripped or unsigned pairs", () => {
  const helper = releaseJob("publish_symbols_job_legacy").match(/function Assert-LegacySymbolPair[\s\S]*?\n            }/)![0];
  // Small textual tool outputs isolate every publication rejection without storing large PDB fixtures.
  const failures: Record<string, string> = { missing: "Missing artifact", unsigned: "Invalid signature", image: "Symbol image differs from package",
    unreadable: "Cannot read PDB", guid: "DLL/PDB identity mismatch", age: "DLL/PDB identity mismatch",
    stripped: "Full PDB required", "no-types": "Full PDB required", "no-debug": "Full PDB required" };
  for (const mode of ["valid", ...Object.keys(failures)]) {
    const script = `
      $ErrorActionPreference = 'Stop'; $global:LASTEXITCODE = 0; $mode = '${mode}'
      $dumpbin = 'ReadHeaders'; $pdbutil = 'ReadPdb'
      function Get-Item { if ($mode -eq 'missing') { throw 'Missing artifact' }; [pscustomobject]@{ Length = 1024 } }
      function Get-AuthenticodeSignature { [pscustomobject]@{ Status = $(if ($mode -eq 'unsigned') { 'NotSigned' } else { 'Valid' }) } }
      function Get-FileHash($file) { [pscustomobject]@{ Hash = $(if ($mode -eq 'image' -and $file -eq 'package') { 'different' } else { 'same' }) } }
      function ReadHeaders { 'Format: RSDS, {AABB-CCDD}, 1' }
      function ReadPdb {
        if ($mode -eq 'unreadable') { $global:LASTEXITCODE = 1 }
        "GUID: {$(if ($mode -eq 'guid') { 'EEEE-FFFF' } else { 'aabb-ccdd' })}"
        "Age: $(if ($mode -eq 'age') { 2 } else { 1 })"
        "Is stripped: $($mode -eq 'stripped')".ToLower()
        "Has Types: $($mode -ne 'no-types')".Replace('True', 'true').Replace('False', 'false')
        "Has Debug Info: $($mode -ne 'no-debug')".Replace('True', 'true').Replace('False', 'false')
      }
      ${helper}
      Assert-LegacySymbolPair dll pdb package`;
    const result = runPowerShell(script);
    assert.equal(result.status === 0, mode === "valid", `${mode}: ${result.stderr}`);
    if (mode !== "valid") assert.ok(result.stderr.includes(failures[mode]), result.stderr);
  }
});
