// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

const read = (file: string) => readFileSync(new URL("../" + file, import.meta.url), "utf8").replaceAll("\r\n", "\n");
const jobs = read(".ado/windows-jobs.yml");
const build = read(".ado/windows-build-legacy.yml");
const acceptance = read(".ado/windows-acceptance-legacy.yml");
const job = (name: string) => jobs.split(`- job: ${name}\n`)[1]?.split(/\n\s*- job:/)[0] ?? "";

test("legacy builds cover six cells on every run with native ARM64", () => {
  const matrix = jobs.split("- name: SandboxMatrix")[0];
  assert.equal((matrix.match(/Name: Desktop_/g) ?? []).length, 6);
  const cell = jobs.split("- job: V8JsiBuildLegacy_${{ matrix.Name }}")[1].split("- job: V8JsiBuildNew_")[0];
  assert.match(cell, /hostArchitecture: arm64/);
  assert.match(cell, /ensure-node10-shim/);
  assert.match(cell, /artifactName: \$\{\{ matrix.Name \}\}-legacy/);
  assert.match(build, /overlay-version-config/);
  assert.match(build, /--no-build --test --check/);
  assert.match(build, /--no-build --stage/);
  assert.ok(build.indexOf("--test --check") < build.indexOf("--stage"));
  assert.match(build, /if parameters.isPublish[\s\S]*esrp-codesign-binaries/);
  assert.match(build, /keyCode: CP-230012/);
  assert.match(build, /Copy-Item \$dll .*out\\symbols/);
});

test("legacy pack consumes all cells and verifies the final signed package", () => {
  const pack = jobs.split("- job: V8JsiCreateNugetLegacy")[1].split("- job: V8JsiAcceptanceLegacy_x64host")[0];
  assert.match(pack, /each matrix in parameters.BuildMatrix/);
  assert.match(pack, /V8JsiBuildLegacy_\$\{\{ matrix.Name/);
  assert.match(pack, /overlay-version-config/);
  assert.match(pack, /Packaging.EnableSBOMSigning/);
  assert.match(pack, /--aggregate/);
  assert.match(pack, /--pack/);
  assert.match(pack, /published-packages-legacy/);
  assert.match(pack, /if parameters.isPublish[\s\S]*esrp-codesign-nuget/);
  assert.match(pack, /Length -ge 500MB/);
  assert.match(pack, /PDB in final legacy package/);
});

test("legacy acceptance runs unsigned PRs and signed publishing packages", () => {
  assert.match(job("V8JsiAcceptanceLegacy_x64host"), /platforms: \[x64, x86\]/);
  assert.match(job("V8JsiAcceptanceLegacy_ARM64"), /hostArchitecture: arm64/);
  assert.match(job("V8JsiAcceptanceLegacy_ARM64"), /platforms: \[arm64\]/);
  assert.match(acceptance, /artifact: published-packages-legacy/);
  assert.match(acceptance, /artifact: Desktop_ARM64_Release-new/);
  assert.match(acceptance, /artifact: Desktop_\$\{\{ platform \}\}_Release-new/);
  assert.match(acceptance, /foreach \(\$configuration in 'Debug', 'Release'\)/);
  assert.match(acceptance, /nuget install ReactNative.V8Jsi.Windows -Version 0.79.5 -Source 'https:\/\/pkgs.dev.azure.com\/ms\/react-native/);
  assert.doesNotMatch(acceptance, /nuget.org|NuGetAuthenticate/);
  assert.match(acceptance, /nuget verify -Signatures/);
  assert.match(acceptance, /-ModernDll \$modern/);
});

test("legacy failures gate modern tags and release completion", () => {
  for (const name of ["V8JsiTagRelease", "V8JsiTagReleaseLegacy"]) {
    assert.match(job(name), /V8JsiAcceptanceLegacy_x64host/);
    assert.match(job(name), /V8JsiAcceptanceLegacy_ARM64/);
  }
  const tag = job("V8JsiTagReleaseLegacy");
  assert.match(tag, /condition: and\(succeeded\(\), eq\(variables\['Build.SourceBranch'\], 'refs\/heads\/main'\)\)/);
  assert.match(tag, /ReactNative.V8Jsi.Windows/);
  assert.match(tag, /\$config.version/);
  assert.match(tag, /git ls-remote --tags origin/);
  assert.match(tag, /if \(\$existing\)/);
  const release = read(".ado/windows-release.yml").split("  repositories:")[0];
  assert.match(release, /source: microsoft.v8-jsi.ci/);
  assert.doesNotMatch(release, /stages:|succeededOrFailed|always\(/);
});
