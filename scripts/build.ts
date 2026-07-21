// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Build script for v8-jsi. Run with Node.js v24+ (TypeScript strip-types).
// Usage: node scripts/build.ts [options]

import { parseArgs } from "node:util";
import { execSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";

import {
  repoRoot,
  ensureDir,
  deleteDir,
  run,
  copyFile,
  downloadFile,
  ensureBinSkimExe,
  analyzeBinSkim,
  dumpbinExportNames,
  crtDependencyLines,
} from "./build-common.ts";

// =============================================================================
// Constants
// =============================================================================

const nodejsDir = path.join(repoRoot, "deps", "nodejs");
const srcDir = path.join(repoRoot, "src");

// BinSkim rules accepted as residuals on the engine binaries (full rationale in the
// per-RID suppressions in .ado/guardian/sdl/.gdnsuppress -- keep in sync). The
// --binskim gate fails on any finding whose rule is NOT listed, so a real
// regression (CFG/ASLR/Spectre dropping off) still breaks the build.
//   BA2004 - MD5 source hashing in vendored V8 libs (our objs are SHA-256).
//   BA2014 - false-positive stack-protector flag on buffer-less V8 thunks.
//   BA2018 - x86 SafeSEH unsupported by clang-cl.
//   BA2025 - CET shadow stack off (reverted; crashed V8 snapshot).
const acceptedBinSkimRules = new Set(["BA2004", "BA2014", "BA2018", "BA2025"]);

// mkv8snapshot.exe links no V8 code or hand-written assembly. Its x86 target
// enables SafeSEH, so BA2018 is not an accepted residual for this binary.
const acceptedSnapshotToolBinSkimRules = new Set(["BA2025"]);

const nasmVersion = "2.16.03";
const nasmDownloadUrl = `https://www.nasm.us/pub/nasm/releasebuilds/${nasmVersion}/win64/nasm-${nasmVersion}-win64.zip`;

// =============================================================================
// CLI Parsing
// =============================================================================

const options = {
  help: { type: "boolean" as const, default: false },
  build: { type: "boolean" as const, default: true },
  test: { type: "boolean" as const, default: false },
  pack: { type: "boolean" as const, default: false },
  smoke: { type: "boolean" as const, default: false },
  "test-e2e": { type: "boolean" as const, default: false },
  binskim: { type: "boolean" as const, default: false },
  "check-crt": { type: "boolean" as const, default: false },
  "check-size": { type: "boolean" as const, default: false },
  "count-exports": { type: "boolean" as const, default: false },
  "build-v8jsisb": { type: "boolean" as const, default: false },
  "clean-all": { type: "boolean" as const, default: false },
  "clean-build": { type: "boolean" as const, default: false },
  "clean-pkg": { type: "boolean" as const, default: false },
  platform: { type: "string" as const, multiple: true, default: ["x64"] },
  configuration: {
    type: "string" as const,
    multiple: true,
    default: ["release"],
  },
  "output-path": { type: "string" as const, default: "./out" },
  "semantic-version": { type: "string" as const },
  "file-version": { type: "string" as const },
};

const { values: args } = parseArgs({ options, allowNegative: true });

const validPlatforms = ["x64", "x86", "arm64"];
const validConfigurations = ["debug", "release"];

// =============================================================================
// Utility Functions
// =============================================================================

// Ensure NASM is available and return the directory containing nasm.exe.
// The Node.js OpenSSL build assembles its crypto with NASM on x86/x64 (the
// arm64 build skips that asm path). Mirrors ensureNuGet: prefer PATH, else a
// cached copy under toolsPath, else download + extract the official zip.
async function ensureNasm(toolsPath: string): Promise<string> {
  try {
    const onPath = execSync("where nasm.exe", { encoding: "utf-8" })
      .split(/\r?\n/)
      .find((l: string) => l.trim().length > 0)
      ?.trim();
    if (onPath) return path.dirname(onPath);
  } catch {
    // Not on PATH; fall through to the cached/download path.
  }

  const nasmDir = path.join(toolsPath, "nasm", `nasm-${nasmVersion}`);
  if (fs.existsSync(path.join(nasmDir, "nasm.exe"))) {
    return nasmDir;
  }

  console.log(`Downloading NASM ${nasmVersion}...`);
  const zipPath = path.join(toolsPath, `nasm-${nasmVersion}-win64.zip`);
  await downloadFile(nasmDownloadUrl, zipPath);
  const extractDir = path.join(toolsPath, "nasm");
  ensureDir(extractDir);
  run("powershell.exe", [
    "-NoProfile",
    "-Command",
    `"Expand-Archive -Path '${zipPath}' -DestinationPath '${extractDir}' -Force"`,
  ]);
  if (!fs.existsSync(path.join(nasmDir, "nasm.exe"))) {
    throw new Error(`NASM not found after extraction at ${nasmDir}`);
  }
  console.log(`NASM ready at ${nasmDir}`);
  return nasmDir;
}

function configDir(configuration: string): string {
  return configuration.charAt(0).toUpperCase() + configuration.slice(1);
}

function buildOutputDir(configuration: string): string {
  return path.join(nodejsDir, "out", configDir(configuration));
}

function isCrossPlatformBuild(platform: string): boolean {
  const hostArch = process.arch; // "x64" or "arm64"
  if (hostArch === "x64" && platform === "arm64") return true;
  if (hostArch === "arm64" && platform !== "arm64") return true;
  return false;
}

function readConfigJson(): {
  version: string;
  v8jsi_version?: string;
  nodejs_version?: string;
} {
  const configPath = path.join(repoRoot, "config.json");
  return JSON.parse(fs.readFileSync(configPath, "utf-8"));
}

// Read NODE_MAJOR/MINOR/PATCH_VERSION from the vendored
// deps/nodejs/src/node_version.h and return them as "MAJOR.MINOR.PATCH".
function readVendoredNodejsVersion(): string {
  const headerPath = path.join(
    nodejsDir,
    "src",
    "node_version.h",
  );
  const header = fs.readFileSync(headerPath, "utf-8");
  const parts: Record<string, string> = {};
  for (const key of ["MAJOR", "MINOR", "PATCH"]) {
    const re = new RegExp(`#define\\s+NODE_${key}_VERSION\\s+(\\d+)`);
    const m = re.exec(header);
    if (!m) {
      throw new Error(
        `Failed to parse NODE_${key}_VERSION from ${headerPath}`,
      );
    }
    parts[key] = m[1];
  }
  return `${parts["MAJOR"]}.${parts["MINOR"]}.${parts["PATCH"]}`;
}

// Verifies config.json.nodejs_version matches the actual vendored
// Node.js version under deps/nodejs/. Hard-fails on mismatch.
function checkNodejsVersionSync(): void {
  const cfg = readConfigJson();
  const declared = cfg.nodejs_version;
  if (!declared) {
    console.error(
      "config.json is missing the 'nodejs_version' field. See docs/versioning.md.",
    );
    process.exit(1);
  }
  const actual = readVendoredNodejsVersion();
  if (declared !== actual) {
    console.error(
      `Version mismatch: config.json.nodejs_version is "${declared}" but ` +
        `deps/nodejs/src/node_version.h declares "${actual}". ` +
        `Update config.json.nodejs_version to match the vendored Node.js, ` +
        `or re-run the fork-sync if you intended a different version. ` +
        `See docs/versioning.md.`,
    );
    process.exit(1);
  }
  console.log(
    `Version sync OK: nodejs_version=${declared}, vendored=${actual}`,
  );
}

// Maps a build platform (x64 / x86 / arm64) to the .NET RID convention
// used in the Microsoft.JavaScript.V8 NuGet layout (LibNode pattern).
function platformToRid(platform: string): string {
  switch (platform.toLowerCase()) {
    case "x64":
      return "win-x64";
    case "x86":
      return "win-x86";
    case "arm64":
      return "win-arm64";
    default:
      throw new Error(`Unsupported platform for RID mapping: ${platform}`);
  }
}

const ALL_WINDOWS_RIDS = ["win-x86", "win-x64", "win-arm64"] as const;

// The Chromium-sandbox gn/ninja output dir for a build platform.
function sandboxOutDir(platform: string): string {
  const cpu = platform.toLowerCase();
  return path.join(repoRoot, "deps", "chromium", "out", `sandbox-${cpu}`);
}

// Stage the sandbox binaries for one platform into build/native/<rid>/.
// Everything is optional: a mainline (non-sandbox) build won't have produced
// them, and the CI aggregator pre-stages them from the `sandbox-binaries`
// artifact. v8jsisb.dll comes from the vcbuild out dir (engineOutDir, alongside
// v8jsi.dll); sbox.dll / v8host.exe / sbox.dll.lib from the gn out dir.
// test_app.exe (the test harness) is deliberately excluded.
function stageSandboxBinaries(
  platform: string,
  engineOutDir: string,
  ridDir: string,
): void {
  const sbxOut = sandboxOutDir(platform);
  // The jitless sandbox engine (vcbuild output, same dir as v8jsi.dll). On x86
  // this is the degraded no-cage variant (still product-shipped); on x64/arm64
  // it carries the V8 in-process cage. Runtime payload only — no import lib
  // (v8host LoadLibrary's it by name; the JSI C++ consumer links v8jsi.dll.lib).
  copyFile("v8jsisb.dll", engineOutDir, ridDir, /*optional*/ true);
  // PDB: the GYP build emits v8jsisb.pdb; stage it as v8jsisb.dll.pdb so the
  // *.pdb nuspec glob + the symbol-publish staging resolve (mirrors v8jsi.pdb ->
  // v8jsi.dll.pdb above). Optional — release/no-symbols builds skip it.
  if (fs.existsSync(path.join(engineOutDir, "v8jsisb.dll.pdb"))) {
    copyFile("v8jsisb.dll.pdb", engineOutDir, ridDir);
  } else if (fs.existsSync(path.join(engineOutDir, "v8jsisb.pdb"))) {
    ensureDir(ridDir);
    fs.copyFileSync(
      path.join(engineOutDir, "v8jsisb.pdb"),
      path.join(ridDir, "v8jsisb.dll.pdb"),
    );
  }
  // The offline startup-snapshot builder (vcbuild output, same dir as the
  // engine — it links no V8, so it's an engine-build artifact, not a gn one).
  // Ships per-RID beside the engine it LoadLibrary's. Optional: only the
  // build_v8jsi/v8jsisb passes produce it.
  copyFile("mkv8snapshot.exe", engineOutDir, ridDir, /*optional*/ true);
  if (fs.existsSync(path.join(engineOutDir, "mkv8snapshot.exe.pdb"))) {
    copyFile("mkv8snapshot.exe.pdb", engineOutDir, ridDir);
  } else if (fs.existsSync(path.join(engineOutDir, "mkv8snapshot.pdb"))) {
    ensureDir(ridDir);
    fs.copyFileSync(
      path.join(engineOutDir, "mkv8snapshot.pdb"),
      path.join(ridDir, "mkv8snapshot.exe.pdb"),
    );
  }
  // The Chromium-sandbox binaries (gn output) + sbox's import lib.
  for (const f of [
    "sbox.dll",
    "sbox.dll.pdb",
    "sbox.dll.lib",
    "v8host.exe",
    "v8host.exe.pdb",
  ]) {
    copyFile(f, sbxOut, ridDir, /*optional*/ true);
  }
}

// Substitute $rid$ and $v8jsiName$ placeholders in a .rid.{props,targets}
// template. nuget pack does NOT run files through -Properties expansion
// (only the .nuspec metadata), so .props/.targets must be pre-templated
// at staging time. Mirrors prepareNugetFiles.js:copyPatchedRootRIDFile
// from the Office libnode pipeline.
function templateFile(
  templatePath: string,
  destPath: string,
  rid: string,
  v8jsiName: string,
): void {
  const template = fs.readFileSync(templatePath, "utf-8");
  const rendered = template
    .replaceAll("$rid$", rid)
    .replaceAll("$v8jsiName$", v8jsiName);
  fs.writeFileSync(destPath, rendered);
}

// =============================================================================
// Help
// =============================================================================

function showHelp(): void {
  console.log(`
v8-jsi build script

Usage: node scripts/build.ts [options]

Boolean flags (all support --no- prefix):
  --help              Show this help message
  --build             Build v8jsi.dll (default: true)
  --test              Run v8jsi_test.exe
  --pack              Create NuGet package
  --smoke             Run the real-consumer smoke (tests/run-smoke.ps1) against
                      the packed .nupkg (x64; Release + Debug)
  --test-e2e          Run the full tests/ integration suite against the packed
                      .nupkg (x64): the consumer smoke + the startup-snapshot
                      e2e (build a snapshot with packaged mkv8snapshot.exe and
                      consume it via V8RuntimeArgs::startupSnapshotBlob, both
                      engines, incl. cross-engine rejection)
  --binskim           Run BinSkim security validation
  --check-crt         Report v8jsi.dll's C/C++ runtime imports (Hybrid CRT check)
  --check-size        Enforce the binary-size budget (v8jsisb.dll <= 18 MiB)
  --count-exports     Report v8jsi.dll's exported symbol counts by API family
  --build-v8jsisb     Build the sandbox variant v8jsisb.dll (V8 in-process sandbox
                      + pointer-compression shared cage + build-time jitless/lite)
                      instead of the mainline full-JIT v8jsi.dll.
  --clean-all         Delete entire output folder
  --clean-build       Delete build outputs for targeted configs
  --clean-pkg         Delete pkg and pkg-staging folders

String arguments:
  --platform <p>      Target platform: x64, x86, arm64 (default: x64, multiple allowed)
  --configuration <c> Build config: debug, release (default: release, multiple allowed)
  --output-path <dir> Output directory (default: ./out)
  --semantic-version  NuGet package version (default: from config.json)
  --file-version      Version for binary files (X.Y.Z.W format)

Examples:
  node scripts/build.ts                          # Build release x64
  node scripts/build.ts --configuration debug    # Build debug x64
  node scripts/build.ts --no-build --test        # Run tests only
  node scripts/build.ts --no-build --pack        # Package only
  node scripts/build.ts --clean-all              # Clean everything

The Chromium sandbox binaries (sbox.dll / v8host.exe / test_app.exe) are built
by a separate gn/ninja toolchain — see: node scripts/sbox-build.ts --help
`);
}

// =============================================================================
// Build Operation
// =============================================================================

async function runBuild(
  platform: string,
  configuration: string,
  toolsPath: string,
): Promise<void> {
  console.log(`\n=== Building v8jsi (${platform} ${configuration}) ===\n`);

  // vcbuild.bat accepts x86 / x64 / arm64 as order-independent target-arch
  // tokens and writes the binaries to out\<Config>\ regardless of arch, so
  // we forward the platform straight through. x64 is vcbuild's default, but
  // passing it explicitly is harmless and keeps the call uniform.
  // The sandbox variant uses the `v8jsisb` vcbuild token (which implies the
  // `v8jsi` target) so configure.py turns on the V8 sandbox + lite variables and
  // emits v8jsisb.dll. Mainline builds keep the plain `v8jsi` token unchanged.
  const v8jsiToken = args["build-v8jsisb"] ? "v8jsisb" : "v8jsi";
  const vcbuildArgs = [
    ".\\vcbuild.bat",
    configuration,
    platform,
    v8jsiToken,
    "without-intl",
    "no-node",
    "no-cctest",
    "no-embedtest",
    "no-openssl-cli",
    "no-fuzzers",
    "no-nop",
    "no-overlapped-checker",
  ];

  if (args["file-version"]) {
    // Pass file version as environment variable for the GYP version_gen action
    process.env.V8JSI_FILE_VERSION = args["file-version"];
  }

  // The x86/x64 OpenSSL build needs NASM on PATH; arm64 skips the asm path.
  const env = { ...process.env };
  if (!platform.startsWith("arm")) {
    const nasmDir = await ensureNasm(toolsPath);
    // Update the EXISTING path variable in place. Windows stores it as "Path";
    // assigning env.PATH would add a second, conflicting key that shadows the
    // real one (dropping System32, so cmd.exe/vcbuild can't be found).
    const pathKey =
      Object.keys(env).find((k) => k.toUpperCase() === "PATH") ?? "Path";
    env[pathKey] = `${nasmDir};${env[pathKey] ?? ""}`;
  }

  run("cmd.exe", ["/c", ...vcbuildArgs], { cwd: nodejsDir, env });
}

// =============================================================================
// Test Operation
// =============================================================================

function runTests(platform: string, configuration: string): void {
  console.log(`\n=== Testing v8jsi (${platform} ${configuration}) ===\n`);

  if (isCrossPlatformBuild(platform)) {
    console.log(
      `Skipping tests for cross-platform build (${platform} on ${process.arch} host)`,
    );
    return;
  }

  // Run both gtest binaries: the JSI suite and the Node-API conformance suite.
  const outDir = buildOutputDir(configuration);
  for (const exeName of ["v8jsi_test.exe", "node_api_tests.exe"]) {
    const testExe = path.join(outDir, exeName);
    if (!fs.existsSync(testExe)) {
      console.error(`Test executable not found: ${testExe}`);
      process.exit(1);
    }
    run(testExe, ["--gtest_brief=1"]);
  }
}

// =============================================================================
// BinSkim Operation
// =============================================================================

async function runBinSkim(
  platform: string,
  configuration: string,
  toolsPath: string,
): Promise<void> {
  console.log(`\n=== BinSkim (${platform} ${configuration}) ===\n`);

  const binskimExe = await ensureBinSkimExe(toolsPath);
  const outDir = buildOutputDir(configuration);
  const dllName = v8jsiDllName();
  const dllPath = path.join(outDir, dllName);
  if (!fs.existsSync(dllPath)) {
    console.log("No binaries found to scan");
    return;
  }
  const engineOk = analyzeBinSkim(
    binskimExe,
    [dllPath],
    path.join(outDir, `${path.parse(dllName).name}.binskim.sarif`),
    `${path.parse(dllName).name} ${platform} ${configuration}`,
    acceptedBinSkimRules,
  );

  const snapshotTool = path.join(outDir, "mkv8snapshot.exe");
  let snapshotToolOk = true;
  if (fs.existsSync(snapshotTool)) {
    snapshotToolOk = analyzeBinSkim(
      binskimExe,
      [snapshotTool],
      path.join(outDir, "mkv8snapshot.binskim.sarif"),
      `mkv8snapshot ${platform} ${configuration}`,
      acceptedSnapshotToolBinSkimRules,
    );
  }
  if (!engineOk || !snapshotToolOk) {
    process.exit(1);
  }
}

// =============================================================================
// DLL Reports (dumpbin)
// =============================================================================

// The produced DLL name depends on the build variant: the sandbox build
// (--build-v8jsisb) emits v8jsisb.dll; the mainline build emits v8jsi.dll. The
// hygiene gates (--binskim, --check-crt, --count-exports) follow the variant so
// they validate whichever DLL was just built.
function v8jsiDllName(): string {
  return args["build-v8jsisb"] ? "v8jsisb.dll" : "v8jsi.dll";
}

function builtDllPath(configuration: string): string {
  const dll = path.join(buildOutputDir(configuration), v8jsiDllName());
  if (!fs.existsSync(dll)) {
    console.error(`${v8jsiDllName()} not found: ${dll}`);
    process.exit(1);
  }
  return dll;
}

// Report v8jsi.dll's C/C++ runtime imports. The Hybrid CRT contract means the
// only shared runtime should be the UCRT (api-ms-win-crt-* / ucrtbase.dll) —
// there must be no vcruntime140 / msvcp140 (App-CRT is statically private).
function reportCrtDependencies(platform: string, configuration: string): void {
  console.log(`\n=== CRT dependencies (${platform} ${configuration}) ===\n`);
  const dll = builtDllPath(configuration);
  const crtLines = crtDependencyLines(dll);

  console.log(`==== ${dll} ====`);
  for (const l of crtLines) console.log(`  ${l}`);

  const violations = crtLines.filter((l: string) => /vcruntime|msvcp/i.test(l));
  if (violations.length) {
    console.log(
      `\nWARNING: Hybrid CRT contract broken — non-UCRT shared runtime present: ${violations.join(", ")}`,
    );
  } else {
    console.log(
      "\nHybrid CRT OK: only the UCRT (api-ms-win-crt-* / ucrtbase) is shared.",
    );
  }
}

// Report v8jsi.dll's exported symbol counts by API family. Handy for spotting
// an unexpected change in the export surface.
function reportExports(platform: string, configuration: string): void {
  console.log(`\n=== Export counts (${platform} ${configuration}) ===\n`);
  const dll = builtDllPath(configuration);
  const names = dumpbinExportNames(dll);
  const count = (prefix: string): number =>
    names.filter((n: string) => n.startsWith(prefix)).length;

  console.log(`napi_*:     ${count("napi_")}`);
  console.log(`jsr_*:      ${count("jsr_")}`);
  console.log(`node_api_*: ${count("node_api_")}`);
  console.log(`v8_*:       ${count("v8_")}`);
  console.log(`total:      ${names.length}`);
}

// Enforce the binary-size budget on the built engine DLL. Only the sandbox
// variant v8jsisb.dll has a ceiling (17.06 MB jitless+lite, budget <= 18 MiB);
// the full-JIT v8jsi.dll is reported but not gated. Exits non-zero on breach.
// Re-baseline only on a deliberate, justified increase (e.g. a V8 bump).
const DLL_SIZE_BUDGET_BYTES: Record<string, number> = {
  "v8jsisb.dll": 18 * 1024 * 1024,
};
function checkDllSize(platform: string, configuration: string): void {
  console.log(`\n=== Binary size budget (${platform} ${configuration}) ===\n`);
  const dll = builtDllPath(configuration);
  const name = path.basename(dll);
  const size = fs.statSync(dll).size;
  const mib = (n: number) => (n / (1024 * 1024)).toFixed(2);
  const budget = DLL_SIZE_BUDGET_BYTES[name];
  if (budget === undefined) {
    console.log(`  ${name}: ${mib(size)} MiB (no size budget — reported only)`);
    return;
  }
  if (size <= budget) {
    console.log(`  OK   ${name}: ${mib(size)} MiB (ceiling ${mib(budget)} MiB)`);
  } else {
    console.error(
      `  FAIL ${name}: ${mib(size)} MiB exceeds ceiling ${mib(budget)} MiB`,
    );
    process.exit(1);
  }
}

// =============================================================================
// Pack Operation
// =============================================================================

function packNuGet(outputPath: string, platforms: string[], configurations: string[]): void {
  console.log("\n=== Packing NuGet ===\n");

  const pkgStagingPath = path.join(outputPath, "pkg-staging");
  const pkgPath = path.join(outputPath, "pkg");
  const nugetSourceDir = path.join(repoRoot, "scripts", "nuget");
  const configJson = readConfigJson();
  // Windows-only today; .so/.dylib substitutions land with future multi-OS support.
  const v8jsiName = "v8jsi.dll";

  // ---------------------------------------------------------------------
  // Stage RID-specific content for each requested platform: DLL + import
  // lib + (optional) PDB. The build host is assumed to have built each
  // requested platform last (vcbuild.bat outputs to a single
  // deps/nodejs/out/<Config>/ directory regardless of architecture, so
  // multi-arch packing assumes prior matrix runs staged each arch).
  // ---------------------------------------------------------------------
  for (const platform of platforms) {
    for (const configuration of configurations) {
      // Debug v8jsi.dll variants are dropped from the shipped NuGet. The
      // Hybrid CRT makes a Debug-CRT consumer safe to link against the
      // Release v8jsi.dll: the App-CRT is statically private to the DLL,
      // UCRT is the only shared runtime, and ucrtbase.dll / ucrtbased.dll
      // are separate DLLs with separate heaps - so as long as no STL types
      // cross the DLL boundary, the cross-CRT allocations stay sound. Debug
      // builds may still run in CI for validation; they just don't get
      // packaged.
      if (configuration === "debug") continue;
      const rid = platformToRid(platform);
      const outDir = buildOutputDir(configuration);
      const ridDir = path.join(pkgStagingPath, "build", "native", rid);

      // CI aggregator scenario: matrix jobs uploaded their per-RID
      // staging; aggregator has no local build output. If the source
      // build dir has no v8jsi.dll, fall back to whatever is already
      // staged for this RID (idempotent re-run is fine).
      if (!fs.existsSync(path.join(outDir, "v8jsi.dll"))) {
        if (fs.existsSync(path.join(ridDir, "v8jsi.dll"))) {
          console.log(
            `Using pre-staged v8jsi.dll for ${rid} (no local build at ${outDir}).`,
          );
          continue;
        }
        throw new Error(
          `Cannot stage ${rid}: no v8jsi.dll at ${outDir} or ${ridDir}.`,
        );
      }

      copyFile("v8jsi.dll", outDir, ridDir);
      // GYP build produces v8jsi.lib (MSBuild legacy emitted v8jsi.dll.lib);
      // rename on copy so .targets resolves either way.
      if (fs.existsSync(path.join(outDir, "v8jsi.dll.lib"))) {
        copyFile("v8jsi.dll.lib", outDir, ridDir);
      } else {
        ensureDir(ridDir);
        fs.copyFileSync(
          path.join(outDir, "v8jsi.lib"),
          path.join(ridDir, "v8jsi.dll.lib"),
        );
      }
      // PDB is optional: ship it in the per-RID .nupkg when present.
      // Like the import lib, the GYP build names it v8jsi.pdb while the
      // MSBuild legacy emitted v8jsi.dll.pdb; stage it as v8jsi.dll.pdb either
      // way so the .targets copy rule and the symbol-publish staging resolve.
      if (fs.existsSync(path.join(outDir, "v8jsi.dll.pdb"))) {
        copyFile("v8jsi.dll.pdb", outDir, ridDir);
      } else if (fs.existsSync(path.join(outDir, "v8jsi.pdb"))) {
        ensureDir(ridDir);
        fs.copyFileSync(
          path.join(outDir, "v8jsi.pdb"),
          path.join(ridDir, "v8jsi.dll.pdb"),
        );
      } else {
        console.log("Skipping copy of v8jsi.dll.pdb (file not found, optional)");
      }

      // Sandbox binaries: the jitless sandbox engine v8jsisb.dll (vcbuild
      // output, same out dir as v8jsi.dll) plus the Chromium-sandbox binaries
      // sbox.dll / v8host.exe + sbox.dll.lib (gn output, per-arch out dir). All
      // optional: a non-sandbox build won't have produced them, and the CI
      // aggregator pre-stages them from the `sandbox-binaries` artifact, so a
      // missing local file just means "already staged / not built here".
      // test_app.exe is the TEST harness and is deliberately NOT staged.
      stageSandboxBinaries(platform, outDir, ridDir);
    }
  }

  // ---------------------------------------------------------------------
  // Stage RID-INDEPENDENT shared content: headers, consumer-side source
  // TUs, root assets, and the .nuspec templates. Idempotent — multiple
  // matrix runs overwrite with identical content.
  // ---------------------------------------------------------------------
  const includeDir = path.join(pkgStagingPath, "build", "native", "include");
  const jsiBaseDir = path.join(pkgStagingPath, "build", "native", "jsi");
  const jsiHeaderDir = path.join(jsiBaseDir, "jsi");
  const jsiAbiDir = path.join(jsiBaseDir, "jsi_abi");
  const jsiPublicDir = path.join(jsiBaseDir, "public");
  const nodeApiDir = path.join(includeDir, "node-api");
  const nodeApiJsiDir = path.join(includeDir, "node-api-jsi");
  const apiLoadersDir = path.join(nodeApiJsiDir, "ApiLoaders");

  // Public headers
  for (const file of [
    "compat.h",
    "Readme.md",
    "ScriptStore.h",
    "v8_api.h",
    "V8JsiRuntime.h",
  ]) {
    copyFile(file, path.join(srcDir, "public"), includeDir);
  }

  // JSI headers + base-class impl + inline impl. jsi.cpp ships the JSI
  // base-class implementations (Buffer/Value/JSError/Runtime destructors
  // and virtual default impls) since v8jsi.dll does not export them
  // (JSI_EXPORT is a no-op unless CREATE_SHARED_LIBRARY is set, which
  // v8jsi.gyp doesn't define), so every consumer compiles jsi.cpp locally.
  for (const file of ["jsi.h", "jsi-inl.h", "jsi.cpp", "instrumentation.h"]) {
    copyFile(file, path.join(srcDir, "jsi"), jsiHeaderDir);
  }

  // JSI ABI headers + sources. Consumers compile JsiAbiRuntime.cpp locally;
  // v8_jsi_config.h and v8_node_api_attach.h are the public C-stable surfaces
  // for runtime configuration and the dual-API attach seam respectively.
  for (const file of [
    "jsi_abi.h",
    "jsi_abi_helpers.h",
    "JsiAbiRuntime.h",
    "JsiAbiRuntime.cpp",
    "v8_jsi_config.h",
    "v8_node_api_attach.h",
  ]) {
    copyFile(file, path.join(srcDir, "jsi_abi"), jsiAbiDir);
  }

  // Public consumer-side TU. Holds makeV8Runtime + the V8ScriptCache and
  // V8TaskRunner adapters. Compiled by the consumer via the .targets
  // file's <ClCompile> include.
  copyFile("V8JsiRuntime.cpp", path.join(srcDir, "public"), jsiPublicDir);

  // Node-API headers
  for (const file of [
    "js_native_api_types.h",
    "js_native_api.h",
    "js_runtime_api.h",
  ]) {
    copyFile(file, path.join(srcDir, "node-api"), nodeApiDir, true);
  }

  // Node-API-JSI headers + consumer-side TU
  for (const file of ["NodeApiJsiRuntime.h", "NodeApiJsiRuntime.cpp"]) {
    copyFile(file, path.join(srcDir, "node-api-jsi"), nodeApiJsiDir, true);
  }

  // ApiLoaders headers + consumer-side TUs
  for (const file of [
    "JSRuntimeApi.cpp",
    "JSRuntimeApi.h",
    "JSRuntimeApi.inc",
    "NodeApi_win.cpp",
    "NodeApi.cpp",
    "NodeApi.h",
    "NodeApi.inc",
    "V8Api.cpp",
    "V8Api.h",
    "V8Api.inc",
  ]) {
    copyFile(
      file,
      path.join(srcDir, "node-api-jsi", "ApiLoaders"),
      apiLoadersDir,
      true,
    );
  }

  // sbox public C-ABI headers. sbox.dll has its own small C ABI (it is NOT
  // a JSI surface): a broker host compiles against these to spawn + lock down
  // v8host.exe. Staged RID-independently under build/native/include/sbox.
  //   sbox.h        — broker + target C ABI (self-contained: <cstddef>/<cstdint>)
  //   sbox_harden.h — header-only DLL-search hardening + WinVerifyTrust helpers
  // msg_channel.h is intentionally NOT shipped — it is internal to sbox.dll (the
  // broker uses the channel via sbox_broker_post_message in sbox.h).
  const sboxIncludeDir = path.join(includeDir, "sbox");
  const sboxHeaderSrc = path.join(repoRoot, "deps", "chromium", "sandbox_dll");
  for (const file of ["sbox.h", "sbox_harden.h"]) {
    copyFile(file, sboxHeaderSrc, sboxIncludeDir);
  }

  // Root assets (LICENSE, NOTICE.txt, README.md, _._ placeholder)
  for (const file of ["LICENSE", "NOTICE.txt", "README.md", "_._"]) {
    copyFile(file, nugetSourceDir, pkgStagingPath);
  }

  // Nuspec templates: meta + per-RID. No pre-templating — nuget pack
  // expands $version$ / $rid$ / $v8jsiName$ / etc. from -Properties at
  // pack time.
  copyFile(
    "Microsoft.JavaScript.V8.nuspec",
    nugetSourceDir,
    pkgStagingPath,
  );
  copyFile(
    "Microsoft.JavaScript.V8.rid.nuspec",
    nugetSourceDir,
    pkgStagingPath,
  );

  // ---------------------------------------------------------------------
  // Pre-template the 4 .rid.{props,targets} files for each RID staged
  // under build/native/<rid>/. Unlike .nuspec, nuget pack does NOT expand
  // $-placeholders in .props/.targets — they ship verbatim. So $rid$ and
  // $v8jsiName$ must be substituted at staging time. Mirrors the Office
  // libnode pipeline's prepareNugetFiles.js:copyPatchedRootRIDFile.
  // ---------------------------------------------------------------------
  const ridTemplateNames = [
    "Microsoft.JavaScript.V8.native.rid.props",
    "Microsoft.JavaScript.V8.native.rid.targets",
    "Microsoft.JavaScript.V8.net461.rid.props",
    "Microsoft.JavaScript.V8.net461.rid.targets",
  ];
  const stagedRids = new Set(platforms.map(platformToRid));
  for (const rid of stagedRids) {
    for (const templateName of ridTemplateNames) {
      const ridFileName = templateName.replace(".rid.", `.${rid}.`);
      templateFile(
        path.join(nugetSourceDir, templateName),
        path.join(pkgStagingPath, ridFileName),
        rid,
        v8jsiName,
      );
    }
  }

  // ---------------------------------------------------------------------
  // Run nuget pack: once per RID present, plus the meta if all 3 Windows
  // RIDs are present (aggregator-pack scenario). Local single-arch builds
  // produce 1 per-RID .nupkg; the aggregator job produces 4 (meta + 3).
  // ---------------------------------------------------------------------
  // The Node.js-source v8jsi NuGet (Microsoft.JavaScript.V8) versions on the
  // Node.js-aligned 24.x.y scheme carried in config.json.v8jsi_version, which
  // is distinct from config.json.version (the legacy ReactNative.V8Jsi.Windows
  // scheme that the old NuGet still ships on). Prefer v8jsi_version; an
  // explicit --semantic-version always wins, and config.json.version remains
  // the last-resort fallback.
  const version =
    args["semantic-version"] || configJson.v8jsi_version || configJson.version;
  const repoUrl = "https://github.com/microsoft/v8-jsi";

  let repoCommit = "unknown";
  try {
    repoCommit = execSync("git rev-parse HEAD", {
      encoding: "utf-8",
      cwd: repoRoot,
    }).trim();
  } catch {
    console.log("Warning: Could not determine git commit ID");
  }

  let repoBranch = "unknown";
  try {
    repoBranch = execSync("git rev-parse --abbrev-ref HEAD", {
      encoding: "utf-8",
      cwd: repoRoot,
    }).trim();
  } catch {
    console.log("Warning: Could not determine git branch");
  }

  ensureDir(pkgPath);

  const nugetExePath = execSync("where nuget.exe", { encoding: "utf-8" })
    .trim()
    .split("\n")[0]
    .trim();

  function packOne(nuspecFileName: string, extraProperties: string = ""): void {
    const props =
      [
        `nugetroot=${pkgStagingPath}`,
        `version=${version}`,
        `repoUrl=${repoUrl}`,
        `repoBranch=${repoBranch}`,
        `repoCommit=${repoCommit}`,
      ].join(";") + extraProperties;
    run(nugetExePath, [
      "pack",
      `"${path.join(pkgStagingPath, nuspecFileName)}"`,
      "-Properties",
      `"${props}"`,
      "-OutputDirectory",
      `"${pkgPath}"`,
    ]);
  }

  // Per-RID packages.
  for (const rid of stagedRids) {
    packOne(
      "Microsoft.JavaScript.V8.rid.nuspec",
      `;rid=${rid};v8jsiName=${v8jsiName}`,
    );
  }

  // Meta package — only if all 3 Windows RIDs are present in staging.
  // Otherwise its <dependencies> would resolve to packages that the local
  // feed doesn't have, breaking consumer installs.
  const allRidsPresent = ALL_WINDOWS_RIDS.every((r) =>
    fs.existsSync(
      path.join(pkgStagingPath, "build", "native", r, "v8jsi.dll"),
    ),
  );
  if (allRidsPresent) {
    packOne("Microsoft.JavaScript.V8.nuspec");
  } else {
    const missing = ALL_WINDOWS_RIDS.filter(
      (r) =>
        !fs.existsSync(
          path.join(pkgStagingPath, "build", "native", r, "v8jsi.dll"),
        ),
    );
    console.log(
      `\nSkipping meta package: per-RID staging missing for ${missing.join(", ")}.`,
    );
  }

  console.log(`\nNuGet packages created in ${pkgPath}`);
}

// =============================================================================
// Integration suite (tests/) — real-consumer e2e against the packed .nupkg
// =============================================================================

// Run one tests/ scenario (tests/run-<scenario>.ps1) against the freshly-packed
// per-RID .nupkg. The harness is x64-only. Each scenario runs against the
// default engine (v8jsi) in the given consumer configs, then once more on the
// sandbox engine (v8jsisb, -UseV8Sandbox) when the pack actually carries it.
function runHarnessScenario(
  scenario: string,
  outputPath: string,
  platforms: string[],
  consumerConfigs: string[],
): void {
  if (!platforms.includes("x64")) {
    console.log(
      `Skipping ${scenario}: the harness is x64-only and x64 was not targeted.`,
    );
    return;
  }

  const script = path.join(repoRoot, "tests", `run-${scenario}.ps1`);
  const pkgDir = path.join(outputPath, "pkg");
  const invoke = (config: string, sandbox: boolean): void => {
    const psArgs = [
      "-NoProfile",
      "-ExecutionPolicy",
      "Bypass",
      "-File",
      `"${script}"`,
      "-Configuration",
      config,
      "-Platform",
      "x64",
      "-PkgDir",
      `"${pkgDir}"`,
    ];
    if (sandbox) psArgs.push("-UseV8Sandbox");
    run("powershell.exe", psArgs);
  };

  for (const config of consumerConfigs) {
    invoke(config, /*sandbox*/ false);
  }

  // Sandbox-engine leg: only when the pack actually carries the sandbox
  // binaries (a non-sandbox build doesn't). Drives v8jsisb.dll.
  const stagedV8jsisb = path.join(
    outputPath,
    "pkg-staging",
    "build",
    "native",
    "win-x64",
    "v8jsisb.dll",
  );
  if (fs.existsSync(stagedV8jsisb)) {
    console.log(`\n=== ${scenario} (sandbox engine) ===\n`);
    invoke("Release", /*sandbox*/ true);
  } else {
    console.log(
      `Skipping ${scenario} sandbox leg: no v8jsisb.dll staged (non-sandbox pack).`,
    );
  }
}

// Real-consumer smoke: builds the consumer in Release + Debug against the
// Release v8jsi.dll (the Debug/Release leg exercises the Hybrid CRT cross-CRT
// boundary) and runs the JSI ABI checks.
function runSmoke(outputPath: string, platforms: string[]): void {
  console.log("\n=== Smoke (real-consumer) ===\n");
  runHarnessScenario("smoke", outputPath, platforms, ["Release", "Debug"]);
}

// Startup-snapshot e2e: builds a snapshot with the packaged mkv8snapshot.exe and
// consumes it via the public V8RuntimeArgs::startupSnapshotBlob surface, for both
// engines, including cross-engine rejection. Release consumer is sufficient (the
// scenario exercises the snapshot path, not the cross-CRT matrix).
function runSnapshotE2E(outputPath: string, platforms: string[]): void {
  console.log("\n=== Snapshot e2e ===\n");
  runHarnessScenario("snapshot", outputPath, platforms, ["Release"]);
}

// =============================================================================
// Clean Operations
// =============================================================================

function cleanAll(outputPath: string): void {
  console.log(`Cleaning all: ${outputPath}`);
  deleteDir(outputPath);
}

function cleanBuild(configuration: string): void {
  const outDir = buildOutputDir(configuration);
  console.log(`Cleaning build: ${outDir}`);
  deleteDir(outDir);
}

function cleanPkg(outputPath: string): void {
  console.log("Cleaning pkg and pkg-staging");
  deleteDir(path.join(outputPath, "pkg-staging"));
  deleteDir(path.join(outputPath, "pkg"));
}

// =============================================================================
// Main
// =============================================================================

async function main(): Promise<void> {
  const startTime = Date.now();

  if (args.help) {
    showHelp();
    return;
  }

  // Validate arguments
  const platforms = (args.platform ?? ["x64"]).map((p) => p.toLowerCase());
  const configurations = (args.configuration ?? ["release"]).map((c) =>
    c.toLowerCase(),
  );

  for (const p of platforms) {
    if (!validPlatforms.includes(p)) {
      console.error(
        `Invalid platform: ${p}. Valid values: ${validPlatforms.join(", ")}`,
      );
      process.exit(1);
    }
  }
  for (const c of configurations) {
    if (!validConfigurations.includes(c)) {
      console.error(
        `Invalid configuration: ${c}. Valid values: ${validConfigurations.join(", ")}`,
      );
      process.exit(1);
    }
  }

  const outputPath = path.resolve(args["output-path"] ?? "./out");
  ensureDir(outputPath);

  const toolsPath = path.join(outputPath, "tools");

  // Clean operations
  if (args["clean-all"]) {
    cleanAll(outputPath);
  }
  if (args["clean-pkg"]) {
    cleanPkg(outputPath);
  }

  if (args.build) {
    checkNodejsVersionSync();
  }

  // Per-platform × configuration operations
  for (const platform of platforms) {
    for (const configuration of configurations) {
      if (args["clean-build"]) {
        cleanBuild(configuration);
      }
      if (args.build) {
        await runBuild(platform, configuration, toolsPath);
      }
      if (args.test) {
        runTests(platform, configuration);
      }
      if (args.binskim) {
        await runBinSkim(platform, configuration, toolsPath);
      }
      if (args["check-crt"]) {
        reportCrtDependencies(platform, configuration);
      }
      if (args["check-size"]) {
        checkDllSize(platform, configuration);
      }
      if (args["count-exports"]) {
        reportExports(platform, configuration);
      }
    }
  }

  // Package operation (after all builds)
  if (args.pack) {
    packNuGet(outputPath, platforms, configurations);
  }

  // Real-consumer integration tests (after packing). --test-e2e runs the full
  // suite (smoke + snapshot); --smoke is the focused subset. Either implies the
  // smoke; only --test-e2e adds the snapshot e2e.
  if (args.smoke || args["test-e2e"]) {
    runSmoke(outputPath, platforms);
  }
  if (args["test-e2e"]) {
    runSnapshotE2E(outputPath, platforms);
  }

  // Report elapsed time
  const elapsed = Date.now() - startTime;
  const seconds = Math.floor(elapsed / 1000);
  const minutes = Math.floor(seconds / 60);
  const remainingSeconds = seconds % 60;
  console.log(
    `\nBuild took ${minutes}m ${remainingSeconds}s`,
  );
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
