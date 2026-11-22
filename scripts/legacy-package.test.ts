// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
import assert from "node:assert/strict";
import { test } from "node:test";
import * as fs from "node:fs";
import * as path from "node:path";
import { execFileSync, spawnSync } from "node:child_process";
import { repoRoot } from "./build-common.ts";
import { tmpdir } from "node:os";
import { aggregateLegacy, expectedLegacyFiles, checkLegacyFileList, legacyCells,
  legacyPayloadFiles, checkLegacyPdbIdentity, packLegacy } from "./legacy-package.ts";

const cells = ["Debug/x86", "Debug/x64", "Debug/arm64", "Release/x86", "Release/x64", "Release/arm64"];
function fixture(action: (root: string, inputs: string[]) => void): void {
  const root = fs.mkdtempSync(path.join(tmpdir(), "legacy-package-"));
  try {
    const inputs = cells.map((cell, index) => {
      const dir = path.join(root, String(index));
      for (const file of expectedLegacyFiles([cell])) {
        const dest = path.join(dir, file);
        fs.mkdirSync(path.dirname(dest), { recursive: true });
        fs.writeFileSync(dest, file);
      }
      return dir;
    });
    action(root, inputs);
  } finally { fs.rmSync(root, { recursive: true, force: true }); }
}

test("six staged cells merge common files without changing bytes", () => fixture((root, inputs) => {
  const output = path.join(root, "merged");
  aggregateLegacy(inputs, output);
  for (const file of expectedLegacyFiles(cells)) assert.equal(fs.readFileSync(path.join(output, file), "utf8"), file);
  assert.equal(legacyCells(expectedLegacyFiles(cells), false).length, 6);
}));

test("aggregation rejects missing and duplicate cells before writing", () => fixture((root, inputs) => {
  const output = path.join(root, "merged");
  assert.throws(() => aggregateLegacy(inputs.slice(1), output), /six cells/);
  assert.throws(() => aggregateLegacy([...inputs, inputs[0]], output), /Duplicate/);
  assert.equal(fs.existsSync(output), false);
}));

test("aggregation rejects conflicting headers and unexpected payloads", () => fixture((root, inputs) => {
  const common = "build/native/include/V8JsiRuntime.h";
  fs.writeFileSync(path.join(inputs[1], common), "different");
  assert.throws(() => aggregateLegacy(inputs, path.join(root, "merged")), /Conflicting/);
  fs.writeFileSync(path.join(inputs[1], common), common);
  fs.writeFileSync(path.join(inputs[0], "unexpected.txt"), "");
  assert.throws(() => aggregateLegacy(inputs, path.join(root, "merged")), /file list mismatch/);
}));

test("package rejects forbidden payloads rather than silently excluding them", () => {
  // Each payload has a different accidental producer in the build pipeline.
  for (const file of ["probe.dll", "v8jsi.dll.pdb", "args.gn", "v8jsi2.dll", "WinRT.dll", "UWP.dll", "sandbox.dll"]) {
    assert.throws(() => legacyCells([...expectedLegacyFiles(cells), file], false), /forbidden/);
    assert.throws(() => checkLegacyFileList(legacyPayloadFiles([...expectedLegacyFiles(cells), file]), expectedLegacyFiles(cells)), /mismatch/);
  }
});
test("baseline ZIP comparison permits only symbol and build-argument removal", () => {
  const payload = expectedLegacyFiles(cells);
  const old = [...payload, "lib/win32/Debug/x64/v8jsi.dll.pdb", "lib/win32/Debug/x64/args.gn",
    "ReactNative.V8Jsi.Windows.nuspec", "_rels/.rels", "[Content_Types].xml", "package/services/metadata/core-properties/a.psmdcp", ".signature.p7s"];
  checkLegacyFileList(payload, legacyPayloadFiles(old, true));
  assert.throws(() => checkLegacyFileList(payload.slice(1), legacyPayloadFiles(old, true)), /missing/);
  assert.throws(() => checkLegacyFileList([...payload, payload[0]], payload), /mismatch/);
});
test("symbol pairing requires matching GUID and age", () => {
  const headers = "Format: RSDS, {AABB-CCDD}, 1, v8jsi.dll.pdb";
  checkLegacyPdbIdentity(headers, "GUID: {aabb-ccdd}\nAge: 1");
  for (const summary of ["GUID: {AABB-CCDD}\nAge: 2", "GUID: {FFFF-CCDD}\nAge: 1", ""]) {
    assert.throws(() => checkLegacyPdbIdentity(headers, summary), /identity mismatch/);
  }
});

test("packing checks every cell version against the package version before invoking NuGet", () => fixture((root, inputs) => {
  const stage = path.join(root, "merged");
  aggregateLegacy(inputs, stage);
  const version = JSON.parse(fs.readFileSync(path.join(repoRoot, "config.json"), "utf8")).version + ".0";
  const vswhere = path.join(process.env["ProgramFiles(x86)"]!, "Microsoft Visual Studio/Installer/vswhere.exe");
  const vs = execFileSync(vswhere, ["-latest", "-prerelease", "-products", "*", "-property", "installationPath"], { encoding: "utf8" }).trim();
  // Resource-only DLLs exercise real FileVersion reads without engine binaries.
  for (const [name, value] of [["current", version], ["stale", "0.0.0.0"]]) {
    fs.writeFileSync(path.join(root, name + ".rc"), `1 VERSIONINFO
FILEVERSION ${value.replaceAll(".", ",")}
BEGIN
  BLOCK "StringFileInfo" BEGIN
    BLOCK "040904b0" BEGIN
      VALUE "OriginalFilename", "v8jsi.dll"
      VALUE "ProductName", "React Native V8 JSI Adapter"
      VALUE "FileVersion", "${value}"
    END
  END
  BLOCK "VarFileInfo" BEGIN
    VALUE "Translation", 0x409, 1200
  END
END`);
    const result = spawnSync(process.env.ComSpec!, ["/c", `call "${vs}\\VC\\Auxiliary\\Build\\vcvarsall.bat" x64 && rc /nologo ${name}.rc && link /nologo /dll /noentry /out:${name}.dll ${name}.res`], { cwd: root, encoding: "utf8", windowsVerbatimArguments: true });
    assert.equal(result.status, 0, result.stdout + result.stderr);
  }
  for (const cell of cells) fs.copyFileSync(path.join(root, "current.dll"), path.join(stage, "lib/win32", cell, "v8jsi.dll"));
  fs.copyFileSync(path.join(root, "stale.dll"), path.join(stage, "lib/win32/Debug/arm64/v8jsi.dll"));
  const output = path.join(root, "pkg");
  assert.throws(() => packLegacy(stage, output, false), /version resource mismatch/);
  assert.equal(fs.existsSync(output), false);
}));

test("aggregation requires an explicit output directory", () => {
  const result = spawnSync(process.execPath, [path.join(repoRoot, "scripts/legacy-build.ts"), "--aggregate", "missing-cell"], { encoding: "utf8" });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /--aggregate requires an explicit --output/);
});
