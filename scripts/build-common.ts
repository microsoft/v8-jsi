// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Shared helpers for the v8-jsi build scripts. Imported by both build.ts (the
// v8jsi/v8jsisb engine build over vcbuild.bat) and sbox-build.ts (the Chromium
// sandbox binaries over gn/ninja). Run with Node.js v24+ (TypeScript
// strip-types); import with the explicit ".ts" extension.
//
// This module is side-effect-free: no CLI parsing and no top-level execution, so
// either script can import it without triggering the other's behavior.

import { execSync, type ExecSyncOptions } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import * as https from "node:https";

// Repo root (this file lives in scripts/).
export const repoRoot = path.resolve(import.meta.dirname, "..");

// =============================================================================
// BinSkim / NuGet constants
// =============================================================================

export const binskimPackageName = "Microsoft.CodeAnalysis.BinSkim";
export const binskimVersion = "4.4.9";
// Install BinSkim from the ms/react-native-public ADO feed, not nuget.org, which
// the locked-down CI pools blackhole. The feed proxies nuget.org via an
// authenticated upstream, so reaching it needs NuGetAuthenticate + the
// 'Nuget - ms/react-native-public' service connection. Override for local runs.
export const binskimNuGetSource =
  process.env.BINSKIM_NUGET_SOURCE ??
  "https://pkgs.dev.azure.com/ms/react-native/_packaging/react-native-public/nuget/v3/index.json";

export const nugetDownloadUrl =
  "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe";

// =============================================================================
// Generic fs / process utilities
// =============================================================================

export function ensureDir(dirPath: string): void {
  if (!fs.existsSync(dirPath)) {
    fs.mkdirSync(dirPath, { recursive: true });
  }
}

export function deleteDir(dirPath: string): void {
  if (fs.existsSync(dirPath)) {
    fs.rmSync(dirPath, { recursive: true, force: true });
  }
}

export function run(
  command: string,
  args: string[],
  runOptions?: ExecSyncOptions,
): void {
  const cmdLine = [command, ...args].join(" ");
  console.log(`> ${cmdLine}`);
  try {
    execSync(cmdLine, { stdio: "inherit", ...runOptions });
  } catch (error: unknown) {
    const exitCode =
      error && typeof error === "object" && "status" in error
        ? (error as { status: number }).status
        : 1;
    console.error(`Command failed with exit code: ${exitCode}`);
    process.exit(exitCode || 1);
  }
}

export function copyFile(
  fileName: string,
  sourcePath: string,
  targetPath: string,
  optional = false,
): void {
  ensureDir(targetPath);
  const sourceFile = path.join(sourcePath, fileName);
  const targetFile = path.join(targetPath, fileName);

  if (optional && !fs.existsSync(sourceFile)) {
    console.log(`Skipping copy of ${fileName} (file not found, optional)`);
    return;
  }

  fs.copyFileSync(sourceFile, targetFile);
}

export function downloadFile(url: string, destPath: string): Promise<void> {
  return new Promise((resolve, reject) => {
    ensureDir(path.dirname(destPath));
    const file = fs.createWriteStream(destPath);

    const request = (requestUrl: string) => {
      https
        .get(requestUrl, (response) => {
          // Follow redirects
          if (
            response.statusCode &&
            response.statusCode >= 300 &&
            response.statusCode < 400 &&
            response.headers.location
          ) {
            file.close();
            fs.unlinkSync(destPath);
            request(response.headers.location);
            return;
          }

          if (response.statusCode !== 200) {
            file.close();
            fs.unlinkSync(destPath);
            reject(new Error(`Download failed with status ${response.statusCode}`));
            return;
          }

          response.pipe(file);
          file.on("finish", () => {
            file.close();
            resolve();
          });
        })
        .on("error", (err) => {
          file.close();
          fs.unlinkSync(destPath);
          reject(err);
        });
    };

    request(url);
  });
}

export async function ensureNuGet(toolsPath: string): Promise<string> {
  // Check tools directory first
  const localNuget = path.join(toolsPath, "nuget.exe");
  if (fs.existsSync(localNuget)) {
    return localNuget;
  }

  // Check PATH
  try {
    const result = execSync("where nuget.exe", { encoding: "utf-8" }).trim();
    if (result) {
      return result.split("\n")[0].trim();
    }
  } catch {
    // Not on PATH
  }

  // Download
  console.log("Downloading nuget.exe...");
  await downloadFile(nugetDownloadUrl, localNuget);
  console.log(`Downloaded nuget.exe to ${localNuget}`);
  return localNuget;
}

// =============================================================================
// Visual Studio toolchain discovery (vswhere / dumpbin)
// =============================================================================

// Locate vswhere.exe (ships under Program Files (x86) regardless of VS edition).
// Returns undefined if it is not installed.
export function vswhereExe(): string | undefined {
  const programFilesX86 =
    process.env["ProgramFiles(x86)"] ??
    process.env["ProgramFiles"] ??
    "C:\\Program Files (x86)";
  const vswhere = path.join(
    programFilesX86,
    "Microsoft Visual Studio",
    "Installer",
    "vswhere.exe",
  );
  return fs.existsSync(vswhere) ? vswhere : undefined;
}

// Locate dumpbin.exe via vswhere (works across VS editions/versions), falling
// back to PATH. Cached after the first lookup.
let cachedDumpbin: string | undefined;
export function findDumpbin(): string {
  if (cachedDumpbin) return cachedDumpbin;

  const vswhere = vswhereExe();
  if (vswhere) {
    try {
      const out = execSync(
        `"${vswhere}" -latest -products * -find "**\\Hostx64\\x64\\dumpbin.exe"`,
        { encoding: "utf-8" },
      ).trim();
      const first = out.split(/\r?\n/).find((l) => l.trim().length > 0)?.trim();
      if (first && fs.existsSync(first)) {
        cachedDumpbin = first;
        return first;
      }
    } catch {
      // Fall through to the PATH lookup.
    }
  }

  try {
    const onPath = execSync("where dumpbin.exe", { encoding: "utf-8" })
      .split(/\r?\n/)
      .find((l: string) => l.trim().length > 0)
      ?.trim();
    if (onPath && fs.existsSync(onPath)) {
      cachedDumpbin = onPath;
      return onPath;
    }
  } catch {
    // Not on PATH either.
  }

  throw new Error(
    "dumpbin.exe not found (install the Visual Studio C++ tools or run from a VS developer prompt).",
  );
}

// Parse a PE's exported symbol names via `dumpbin /EXPORTS`. The export table is
// the region between the "ordinal hint RVA name" header and the "Summary"
// section; rows look like "    1    0 00001000 napi_foo = napi_foo" (ordinal,
// hex hint, 8-hex-digit RVA, name [, " = " forwarder/demangled]). Bounding the
// scan + requiring an 8-hex RVA avoids matching dumpbin's own summary lines
// ("... date stamp", "number of functions/names").
export function dumpbinExportNames(filePath: string): string[] {
  const dumpbin = findDumpbin();
  const out = execSync(`"${dumpbin}" /EXPORTS "${filePath}"`, {
    encoding: "utf-8",
  });
  const rowRe = /^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)/;
  const names: string[] = [];
  let inTable = false;
  for (const line of out.split(/\r?\n/)) {
    if (/^\s*ordinal\s+hint\s+RVA\s+name/.test(line)) {
      inTable = true;
      continue;
    }
    if (/^\s*Summary\b/.test(line)) break;
    if (!inTable) continue;
    const m = rowRe.exec(line);
    if (m) names.push(m[1]);
  }
  return names;
}

// The CRT-related lines from a PE's `dumpbin /DEPENDENTS`. Hybrid CRT means the
// only shared runtime is the UCRT (api-ms-win-crt-* / ucrtbase.dll); a violation
// is any vcruntime140 / msvcp140 import (App-CRT must be statically private).
export function crtDependencyLines(filePath: string): string[] {
  const dumpbin = findDumpbin();
  const out = execSync(`"${dumpbin}" /DEPENDENTS "${filePath}"`, {
    encoding: "utf-8",
  });
  const crtRe = /vcruntime|msvcp|ucrtbase|api-ms-win-crt|ucrt/i;
  return out
    .split(/\r?\n/)
    .map((l: string) => l.trim())
    .filter((l: string) => crtRe.test(l));
}

// =============================================================================
// BinSkim
// =============================================================================

// Install (if needed) and locate BinSkim.exe. Shared by the v8jsi and sandbox
// BinSkim gates.
export async function ensureBinSkimExe(toolsPath: string): Promise<string> {
  const nugetExe = await ensureNuGet(toolsPath);
  const binskimDir = path.join(toolsPath, "binskim");

  // Install BinSkim if not present
  const binskimPkgDir = path.join(
    binskimDir,
    `${binskimPackageName}.${binskimVersion}`,
  );
  if (!fs.existsSync(binskimPkgDir)) {
    run(nugetExe, [
      "install",
      binskimPackageName,
      "-Version",
      binskimVersion,
      "-Source",
      `"${binskimNuGetSource}"`,
      "-OutputDirectory",
      `"${binskimDir}"`,
      "-NonInteractive",
    ]);
  }

  // Find BinSkim.exe
  const toolsDir = path.join(binskimPkgDir, "tools");
  if (fs.existsSync(toolsDir)) {
    for (const runtime of fs.readdirSync(toolsDir)) {
      const candidate = path.join(toolsDir, runtime, "win-x64", "BinSkim.exe");
      if (fs.existsSync(candidate)) {
        return candidate;
      }
    }
  }

  console.error("BinSkim.exe not found in installed package");
  process.exit(1);
}

// Run BinSkim over `files`, evaluate the SARIF against `acceptedRules`, and
// return whether the scan is clean (no unaccepted findings). Does not exit; the
// caller decides how to aggregate pass/fail across binaries.
export function analyzeBinSkim(
  binskimExe: string,
  files: string[],
  sarifPath: string,
  label: string,
  acceptedRules: Set<string>,
): boolean {
  const fileArgs = files.map((f) => `"${f}"`).join(" ");
  if (fs.existsSync(sarifPath)) {
    fs.rmSync(sarifPath);
  }

  // BinSkim exits non-zero when it reports any failing result, so capture the
  // SARIF and decide pass/fail ourselves against the accepted-residuals list.
  const cmdLine = [
    `"${binskimExe}"`,
    "analyze",
    "--config",
    "default",
    "--output",
    `"${sarifPath}"`,
    "--ignorePdbLoadError",
    "--ignorePELoadErrors",
    "True",
    "--hashes",
    "--disable-telemetry",
    "True",
    fileArgs,
  ].join(" ");
  console.log(`> ${cmdLine}`);
  try {
    execSync(cmdLine, { stdio: "inherit" });
  } catch {
    // Non-zero exit means findings were reported; the SARIF below is the source
    // of truth for whether any are unaccepted.
  }

  if (!fs.existsSync(sarifPath)) {
    console.error(`BinSkim produced no SARIF output at ${sarifPath}`);
    return false;
  }

  const sarif = JSON.parse(fs.readFileSync(sarifPath, "utf-8"));
  const breaking: string[] = [];
  const accepted: string[] = [];
  for (const sarifRun of sarif.runs ?? []) {
    // Rule default levels, used when a result omits an explicit level.
    const ruleLevels = new Map<string, string>();
    for (const rule of sarifRun.tool?.driver?.rules ?? []) {
      if (rule.id && rule.defaultConfiguration?.level) {
        ruleLevels.set(rule.id, rule.defaultConfiguration.level);
      }
    }
    for (const result of sarifRun.results ?? []) {
      const ruleId: string = result.ruleId ?? "<unknown>";
      // BinSkim only emits failing results by default; treat absent level via
      // the rule default, then SARIF's "warning" default.
      const level: string =
        result.level ?? ruleLevels.get(ruleId) ?? "warning";
      const where =
        result.locations?.[0]?.physicalLocation?.artifactLocation?.uri ??
        "<binary>";
      const line = `${ruleId} (${level}) - ${where}`;
      if (acceptedRules.has(ruleId)) {
        accepted.push(line);
      } else if (level === "error" || level === "warning") {
        breaking.push(line);
      }
    }
  }

  if (accepted.length > 0) {
    console.log(
      `\nBinSkim ${label}: ${accepted.length} accepted residual finding(s) ` +
        `(rationale in the accepted-rules set passed by the caller):`,
    );
    for (const a of accepted) {
      console.log(`  - ${a}`);
    }
  }
  if (breaking.length > 0) {
    console.error(`\nBinSkim FAILED (${label}): ${breaking.length} unaccepted finding(s):`);
    for (const b of breaking) {
      console.error(`  - ${b}`);
    }
    console.error(
      "\nIf a finding is a genuine fix, address it at the source; if it is a " +
        "vetted residual, add it to .ado/guardian/sdl/.gdnsuppress and to the " +
        "accepted-rules set for this binary.",
    );
    return false;
  }
  console.log(`\nBinSkim OK (${label}): no unaccepted findings.`);
  return true;
}
