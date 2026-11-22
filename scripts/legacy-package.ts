// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import * as fs from "node:fs";
import * as path from "node:path";
import { execFileSync } from "node:child_process";
import { repoRoot, dumpbinExportNames } from "./build-common.ts";
import { checkLegacyExports, checkLegacySources, normalizedHash } from "./legacy-checks.ts";

const configurations = ["Release", "Debug"];
const platforms = ["x64", "x86", "arm64"];
const contractDir = path.join(repoRoot, "src/legacy");
const exports = JSON.parse(fs.readFileSync(path.join(contractDir, "exports.json"), "utf8"));

export function stageLegacy(output: string, destination: string, platform: string, configuration: string): void {
  checkLegacySources(repoRoot);
  checkLegacyExports(dumpbinExportNames(path.join(output, "v8jsi.dll")), exports, platform);
  if (fs.existsSync(destination) && fs.readdirSync(destination).length) throw new Error("Stage directory must be empty");
  const cell = path.join(destination, "lib/win32", configuration, platform);
  fs.mkdirSync(cell, { recursive: true });
  for (const file of ["v8jsi.dll", "v8jsi.dll.lib"]) fs.copyFileSync(path.join(output, file), path.join(cell, file));
  const symbols = path.join(path.dirname(destination), "symbols", `${platform}-${configuration}`);
  fs.mkdirSync(symbols, { recursive: true });
  for (const file of ["v8jsi.dll", "v8jsi.dll.pdb"]) fs.copyFileSync(path.join(output, file), path.join(symbols, file));
  const include = path.join(destination, "build/native/include");
  fs.mkdirSync(include, { recursive: true });
  const manifest = JSON.parse(fs.readFileSync(path.join(contractDir, "sources.json"), "utf8"));
  for (const file of manifest.files) {
    if (!file.packagePath) continue;
    const dest = path.join(destination, file.packagePath);
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.copyFileSync(path.join(repoRoot, file.path), dest);
  }
  fs.copyFileSync(path.join(repoRoot, "ReactNative.V8Jsi.Windows.targets"), path.join(destination, "build/native/ReactNative.V8Jsi.Windows.targets"));
  fs.mkdirSync(path.join(destination, "license"), { recursive: true });
  for (const file of ["LICENSE", "LICENSE.jsi.md", "LICENSE.napi.md", "LICENSE.v8.md"]) {
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
  const files = fs.readdirSync(stage, { recursive: true, withFileTypes: true }).filter(file => file.isFile())
    .map(file => path.relative(stage, path.join(file.parentPath, file.name)).replaceAll("\\", "/"));
  const selected = legacyCells(files, diagnostic);
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
  for (const file of ["build/native/ReactNative.V8Jsi.Windows.targets", "build/native/include/V8JsiRuntime.h"]) {
    if (!files.includes(file)) throw new Error(`Missing package file: ${file}`);
  }
  const config = JSON.parse(fs.readFileSync(path.join(repoRoot, "config.json"), "utf8"));
  const version = config.version;
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
  console.log(`Packed ${pkg}${diagnostic ? " (diagnostic)" : ""}`);
}
