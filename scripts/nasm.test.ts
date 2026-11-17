// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import assert from "node:assert/strict";
import path from "node:path";
import { test } from "node:test";
import { ensureNasm, nasmRequiredForTarget, prependToPath } from "./build-common.ts";

const env = {
  Path: "C:\\first;C:\\second",
  ProgramFiles: "C:\\Program Files",
  "ProgramFiles(x86)": "C:\\Program Files (x86)",
  LOCALAPPDATA: "C:\\User",
};
const folders = ["C:\\first", "C:\\second", "C:\\Program Files\\NASM",
  "C:\\Program Files (x86)\\NASM", "C:\\User\\bin\\NASM"];
const exe = (folder: string) => path.join(folder, "nasm.exe");
const version = () => "NASM version 3.02";

test("developer discovery follows the vendored search order", () => {
  // Each suffix proves the next fallback without any filesystem or process access.
  for (let i = 0; i < folders.length; i++) {
    const available = folders.slice(i).map(exe);
    const calls: string[] = [];
    assert.equal(ensureNasm(env, (p) => available.includes(p), (p) => {
      calls.push(p);
      return version();
    }), folders[i]);
    assert.deepEqual(calls, [available[0]]);
  }
});

test("CI ignores PATH and per-user installations", () => {
  for (const TF_BUILD of ["true", "TrUe"]) {
    assert.throws(() => ensureNasm({ ...env, TF_BUILD },
      (p) => [exe(folders[0]), exe(folders[4])].includes(p), version),
    /\.ado\/image\/windows-2025-1espt-x64\.json/);
    for (const folder of folders.slice(2, 4)) {
      assert.equal(ensureNasm({ ...env, TF_BUILD },
        (p) => [exe(folders[0]), exe(folder)].includes(p), version), folder);
    }
    assert.equal(ensureNasm({ ...env, TF_BUILD }, () => true, version), folders[2]);
  }
  assert.equal(ensureNasm({ ...env, TF_BUILD: "false" }, () => true, version), folders[0]);
});

test("all NASM environment lookups ignore key casing", () => {
  const mixedEnv = {
    pAtH: env.Path,
    PROGRAMFILES: env.ProgramFiles,
    "programfiles(X86)": env["ProgramFiles(x86)"],
    localappdata: env.LOCALAPPDATA,
  };
  // Every search location and CI restriction must survive differently-cased keys.
  for (const folder of folders) {
    assert.equal(ensureNasm(mixedEnv, (p) => p === exe(folder), version), folder);
  }
  assert.throws(() => ensureNasm({ ...mixedEnv, tf_build: "TrUe" },
    (p) => [exe(folders[0]), exe(folders[4])].includes(p), version), /CI image/);
  for (const folder of folders.slice(2, 4)) {
    assert.equal(ensureNasm({ ...mixedEnv, tf_build: "true" },
      (p) => p === exe(folder), version), folder);
  }
});

test("missing developer NASM names the setup script and WinGet command", () => {
  assert.throws(() => ensureNasm(env, () => false, version),
    /scripts\\install-prerequisites\.ps1.*winget install --id NASM\.NASM --exact --source winget/);
  assert.throws(() => ensureNasm({}, () => false, version), /NASM is missing/);
});

test("a failing or empty version probe is an explicit error", () => {
  for (const probe of [() => "", () => " \r\n", () => { throw new Error("exit 1"); }]) {
    assert.throws(() => ensureNasm(env, () => true, probe), /NASM version probe failed.*nasm\.exe -v/);
  }
});

test("the selected path and version are printed and its folder can lead PATH", (t) => {
  const messages: string[] = [];
  t.mock.method(console, "log", (s: string) => messages.push(s));
  const childEnv = { ...env };
  prependToPath(childEnv, ensureNasm(childEnv, () => true, version));
  assert.equal(childEnv.Path, `${folders[0]};${env.Path}`);
  assert.deepEqual(messages, [`Using NASM at ${exe(folders[0])}: ${version()}`]);
});

test("NASM requirements depend on the target, not the host", () => {
  // Cross-target cases exercise the shared build decision on each supported host.
  const original = process.env.PROCESSOR_ARCHITECTURE;
  try {
    for (const host of ["AMD64", "ARM64"]) {
      process.env.PROCESSOR_ARCHITECTURE = host;
      assert.equal(nasmRequiredForTarget("x64"), true);
      assert.equal(nasmRequiredForTarget("x86"), true);
      assert.equal(nasmRequiredForTarget("arm64"), false);
    }
  } finally {
    if (original === undefined) delete process.env.PROCESSOR_ARCHITECTURE;
    else process.env.PROCESSOR_ARCHITECTURE = original;
  }
});
