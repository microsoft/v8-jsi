// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import * as fs from "node:fs";
import * as path from "node:path";
import { execFileSync } from "node:child_process";
import { repoRoot, dumpbinExportNames, findDumpbin } from "./build-common.ts";
import { checkSourceLink } from "./source-link-check.ts";
import { checkLegacyExports, checkLegacySources, normalizedHash, checkLegacyIdentity } from "./legacy-checks.ts";

const configurations = ["Release", "Debug"];
const platforms = ["x64", "x86", "arm64"];
const contractDir = path.join(repoRoot, "src/legacy");
const exports = JSON.parse(fs.readFileSync(path.join(contractDir, "exports.json"), "utf8"));

export function stageLegacy(output: string, destination: string, platform: string, configuration: string): void {
  checkLegacySources(repoRoot);
  const dll = path.join(output, "v8jsi.dll");
  const bytes = fs.readFileSync(dll);
  const machine = bytes.readUInt16LE(bytes.readUInt32LE(60) + 4);
  if (machine !== ({ x86: 0x14c, x64: 0x8664, arm64: 0xaa64 } as Record<string, number>)[platform]) {
    throw new Error("Staged DLL architecture mismatch");
  }
  const version = JSON.parse(fs.readFileSync(path.join(repoRoot, "config.json"), "utf8")).version;
  checkLegacyDllIdentity(dll, version);
  checkLegacySymbols(output);
  checkLegacyExports(dumpbinExportNames(path.join(output, "v8jsi.dll")), exports, platform);
  if (fs.existsSync(destination) && fs.readdirSync(destination).length) throw new Error("Stage directory must be empty");
  const cell = path.join(destination, "lib/win32", configuration, platform);
  fs.mkdirSync(cell, { recursive: true });
  for (const file of ["v8jsi.dll", "v8jsi.dll.lib"]) fs.copyFileSync(path.join(output, file), path.join(cell, file));
  const symbols = path.join(path.dirname(destination), "symbols", `${platform}-${configuration}`);
  fs.mkdirSync(symbols, { recursive: true });
  for (const file of ["v8jsi.dll", "v8jsi.dll.pdb"]) fs.copyFileSync(path.join(output, file), path.join(symbols, file));

  const manifest = JSON.parse(fs.readFileSync(path.join(contractDir, "sources.json"), "utf8"));
  for (const file of manifest.files) {
    if (!file.packagePath) continue;
    const dest = path.join(destination, file.packagePath);
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.copyFileSync(path.join(repoRoot, file.path), dest);
  }
  fs.copyFileSync(path.join(repoRoot, "ReactNative.V8Jsi.Windows.targets"), path.join(destination, "build/native/ReactNative.V8Jsi.Windows.targets"));
  fs.mkdirSync(path.join(destination, "license"), { recursive: true });
  for (const file of ["LICENSE.jsi.md", "LICENSE.napi.md", "LICENSE.v8.md"]) {
    fs.copyFileSync(path.join(repoRoot, file), path.join(destination, "license", file));
  }
  console.log(`Staged ${platform} ${configuration}; symbols are separate from the package payload`);
}

export function legacyCells(files: readonly string[], diagnostic: boolean): string[] {
  if (files.some(file => /(?:\.pdb$|args\.gn$|probe|sandbox|v8jsi2|winrt|uwp)/i.test(file))) {
    throw new Error("Legacy package contains a forbidden payload");
  }
  const cells: string[] = [];
  for (const config of configurations) for (const platform of platforms) {
    const prefix = `lib/win32/${config}/${platform}/`;
    const entries = files.filter(file => file.startsWith(prefix));
    if (!entries.length) continue;
    if (entries.length !== 2 || !entries.includes(prefix + "v8jsi.dll") || !entries.includes(prefix + "v8jsi.dll.lib")) {
      throw new Error(`Incomplete legacy cell: ${config}/${platform}`);
    }
    cells.push(`${config}/${platform}`);
  }
  if ((!diagnostic && cells.length !== 6) || (diagnostic && cells.length !== 1 && cells.length !== 6)) {
    throw new Error("Production packaging requires six cells; a single cell requires --diagnostic-pack");
  }
  const unexpected = files.filter(file => file.startsWith("lib/") && !cells.some(cell => file.startsWith(`lib/win32/${cell}/`)));
  if (unexpected.length) throw new Error(`Unexpected legacy cells: ${unexpected.join(", ")}`);
  return cells;
}

export function packLegacy(stage: string, output: string, diagnostic: boolean): void {
  checkLegacySources(repoRoot);
  const files = stagedFiles(stage);
  const selected = legacyCells(files, diagnostic);
  checkLegacyFileList(files, expectedLegacyFiles(selected));
  const version = JSON.parse(fs.readFileSync(path.join(repoRoot, "config.json"), "utf8")).version;
  for (const cell of selected) {
    const dll = path.join(stage, "lib/win32", cell, "v8jsi.dll");
    checkLegacyDllIdentity(dll, version);
  }
  for (const cell of selected) {
    checkLegacyExports(dumpbinExportNames(path.join(stage, "lib/win32", cell, "v8jsi.dll")), exports, cell.split("/")[1]);
  }
  const manifest = JSON.parse(fs.readFileSync(path.join(contractDir, "sources.json"), "utf8"));
  for (const file of manifest.files) {
    if (!file.packagePath) continue;
    if (normalizedHash(fs.readFileSync(path.join(stage, file.packagePath))) !== file.sha256) {
      throw new Error(`Staged contract drift: ${file.packagePath}`);
    }
  }
  if (normalizedHash(fs.readFileSync(path.join(stage, "build/native/ReactNative.V8Jsi.Windows.targets"))) !==
      normalizedHash(fs.readFileSync(path.join(repoRoot, "ReactNative.V8Jsi.Windows.targets")))) throw new Error("Staged targets drift");
  const revision = execFileSync("git", ["rev-parse", "HEAD"], { cwd: repoRoot, encoding: "utf8" }).trim();
  const nuspec = path.join(repoRoot, "ReactNative.V8Jsi.Windows.nuspec");
  const repository = /<projectUrl>([^<]+)<\/projectUrl>/.exec(fs.readFileSync(nuspec, "utf8"))![1];
  const nuget = execFileSync("where.exe", ["nuget.exe"], { encoding: "utf8" }).trim().split(/\r?\n/)[0];
  fs.mkdirSync(output, { recursive: true });
  const properties = `nugetroot=${path.resolve(stage)};Version=${version};VersionDetails=V8 14.6;RepoUri=${repository};CommitId=${revision}`;
  execFileSync(nuget, ["pack", nuspec, "-Properties", properties, "-OutputDirectory", output,
    "-NoPackageAnalysis", "-NonInteractive"], { stdio: "inherit" });
  const pkg = path.join(output, `ReactNative.V8Jsi.Windows.${version}.nupkg`);
  if (fs.statSync(pkg).size >= 500 * 1024 * 1024) throw new Error("Legacy package exceeds 500 MiB");
  const packedFiles = zipFiles(pkg);
  legacyCells(packedFiles, diagnostic);
  checkLegacyFileList(legacyPayloadFiles(packedFiles), files);
  console.log(`Packed ${pkg}${diagnostic ? " (diagnostic)" : ""}`);
}

function stagedFiles(root: string): string[] {
  return fs.readdirSync(root, { recursive: true, withFileTypes: true }).filter(file => file.isFile())
    .map(file => path.relative(root, path.join(file.parentPath, file.name)).replaceAll("\\", "/"));
}

export function expectedLegacyFiles(cells: readonly string[]): string[] {
  const manifest = JSON.parse(fs.readFileSync(path.join(contractDir, "sources.json"), "utf8"));
  return [...manifest.files.filter((file: { packagePath?: string }) => file.packagePath)
    .map((file: { packagePath: string }) => file.packagePath),
    "build/native/ReactNative.V8Jsi.Windows.targets",
    ...["LICENSE.jsi.md", "LICENSE.napi.md", "LICENSE.v8.md"].map(file => `license/${file}`),
    ...cells.flatMap(cell => ["v8jsi.dll", "v8jsi.dll.lib"].map(file => `lib/win32/${cell}/${file}`))];
}

export function checkLegacyFileList(actual: readonly string[], expected: readonly string[]): void {
  const missing = expected.filter(file => !actual.includes(file));
  const extra = actual.filter(file => !expected.includes(file));
  if (missing.length || extra.length || new Set(actual).size !== actual.length) {
    throw new Error(`Legacy file list mismatch: missing [${missing.join(", ")}]; extra [${extra.join(", ")}]`);
  }
}

export function legacyPayloadFiles(files: readonly string[], baseline = false): string[] {
  return files.filter(file => !/^(?:_rels\/|package\/|\[Content_Types\]\.xml$|\.signature\.p7s$)/.test(file) &&
    !/^[^/]+\.nuspec$/.test(file) && (!baseline || !/(?:\.pdb|args\.gn)$/.test(file)));
}

function zipFiles(file: string): string[] {
  const escaped = path.resolve(file).replaceAll("'", "''");
  return JSON.parse(execFileSync("pwsh", ["-NoProfile", "-Command",
    `Add-Type -AssemblyName System.IO.Compression; $z = [IO.Compression.ZipFile]::OpenRead('${escaped}'); try { ConvertTo-Json -InputObject @($z.Entries | Where-Object { !$_.FullName.EndsWith('/') } | ForEach-Object { $_.FullName }) -Compress } finally { $z.Dispose() }`], { encoding: "utf8" }));
}

export function aggregateLegacy(inputs: readonly string[], destination: string): void {
  const cells: string[] = [];
  const contents = new Map<string, Buffer>();
  for (const input of inputs) {
    const files = stagedFiles(input);
    const selected = legacyCells(files, true);
    checkLegacyFileList(files, expectedLegacyFiles(selected));
    for (const cell of selected) {
      if (cells.includes(cell)) throw new Error(`Duplicate legacy cell: ${cell}`);
      cells.push(cell);
    }
    for (const file of files) {
      const data = fs.readFileSync(path.join(input, file));
      if (contents.has(file) && !contents.get(file)!.equals(data)) throw new Error(`Conflicting staged file: ${file}`);
      contents.set(file, data);
    }
  }
  legacyCells([...contents.keys()], false);
  if (fs.existsSync(destination) && fs.readdirSync(destination).length) throw new Error("Aggregate directory must be empty");
  for (const [file, data] of contents) {
    const output = path.join(destination, file);
    fs.mkdirSync(path.dirname(output), { recursive: true });
    fs.writeFileSync(output, data);
  }
}

export function checkLegacyPdbIdentity(headers: string, summary: string): void {
  const dll = /Format:\s*RSDS,\s*\{([^}]+)\},\s*(\d+)/i.exec(headers);
  const guid = /GUID:\s*\{([^}]+)\}/i.exec(summary);
  const age = /Age:\s*(\d+)/i.exec(summary);
  if (!dll || !guid || !age || dll[1].toLowerCase() !== guid[1].toLowerCase() || dll[2] !== age[1]) {
    throw new Error("Legacy DLL/PDB identity mismatch");
  }
}

function checkLegacySymbols(output: string): void {
  const pdb = path.join(output, "v8jsi.dll.pdb");
  const vswhere = path.join(process.env["ProgramFiles(x86)"]!, "Microsoft Visual Studio/Installer/vswhere.exe");
  const util = execFileSync(vswhere, ["-latest", "-prerelease", "-products", "*", "-find",
    `VC\\Tools\\Llvm\\${process.arch === "arm64" ? "ARM64" : "x64"}\\bin\\llvm-pdbutil.exe`], { encoding: "utf8" }).trim().split(/\r?\n/)[0];
  if (!util) throw new Error("Visual Studio llvm-pdbutil is required to check legacy symbols");
  checkLegacyPdbIdentity(execFileSync(findDumpbin(), ["/headers", path.join(output, "v8jsi.dll")], { encoding: "utf8" }),
    execFileSync(util, ["dump", "--summary", pdb], { encoding: "utf8" }));
  if (!checkSourceLink([pdb], { strict: true })) throw new Error("Legacy strict Source Link failed");
}

function checkLegacyDllIdentity(dll: string, version: string): void {
  const escaped = dll.replaceAll("'", "''");
  const info = JSON.parse(execFileSync("pwsh", ["-NoProfile", "-Command",
    `(Get-Item '${escaped}').VersionInfo | Select-Object OriginalFilename,ProductName,FileVersion | ConvertTo-Json -Compress`], { encoding: "utf8" }));
  checkLegacyIdentity(fs.readFileSync(dll), info, version);
}
