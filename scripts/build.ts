// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Build script for v8-jsi. Run with Node.js v24+ (TypeScript strip-types).
// Usage: node scripts/build.ts [options]

import { parseArgs } from "node:util";
import { execSync, spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";

import {
  repoRoot,
  ensureDir,
  deleteDir,
  run,
  copyFile,
  ensureBinSkimExe,
  ensureNasm,
  nasmRequiredForTarget,
  analyzeBinSkim,
  dumpbinExportNames,
  findDumpbin,
  checkCetHeaders,
  crtDependencyLines,
  ensureSupportedPython,
  prependToPath,
  readV8JsiNames,
  checkV8JsiPayload,
  shouldDeferCetTests,
} from "./build-common.ts";
import { checkSourceLink, pdbOf } from "./source-link-check.ts";

// =============================================================================
// Constants
// =============================================================================

const nodejsDir = path.join(repoRoot, "deps", "nodejs");
const srcDir = path.join(repoRoot, "src");
const stelaCli = path.join(repoRoot, "tools", "stela", "src", "stela.ts");
const stelaConfig = path.join(repoRoot, "stela.config.json");

// Libraries whose headers ship in the package; each ships its Stela model
// (build/native/stela/<library>.model.json), extracted from the same headers.
const shippedStelaLibraries = ["jsi_abi", "v8_jsi_abi", "v8_node_api", "sbox", "v8sbhost"];

// Run the Stela CLI; `run` joins a shell command line, so quote every path.
function runStela(stelaArgs: string[]): void {
  run(`"${process.execPath}"`, [stelaCli, ...stelaArgs].map((a) => `"${a}"`), { cwd: repoRoot });
}

// BinSkim rules accepted as residuals on the engine binaries (rationale for these
// rules is in .ado/guardian/sdl/.gdnsuppress's per-RID suppressions). The
// --binskim gate fails on any finding whose rule is NOT listed, so a real
// regression (CFG/ASLR/Spectre dropping off) still breaks the build.
//   BA2004 - MD5 source hashing in vendored V8 libs (our objs are SHA-256).
//   BA2014 - false-positive stack-protector flag on buffer-less V8 thunks.
//   BA2018 - x86 SafeSEH unsupported by clang-cl.
// The x64 engine needs V8's CET switch for deopt/unwind shadow-stack adjustments;
// BA2025 acceptance for non-x64 engines is per call below, not in Guardian.
// The x64 snapshot-tool BA2025 suppression used by CI is in
// .ado/guardian/sdl/.gdnsuppress; the x64 engine is not suppressed.
const acceptedBinSkimRules = new Set(["BA2004", "BA2014", "BA2018"]);

// =============================================================================
// CLI Parsing
// =============================================================================

const options = {
  help: { type: "boolean" as const, default: false },
  build: { type: "boolean" as const, default: true },
  test: { type: "boolean" as const, default: false },
  pack: { type: "boolean" as const, default: false },
  smoke: { type: "boolean" as const, default: false },
  "test-cet-runtime": { type: "string" as const },
  "test-e2e": { type: "boolean" as const, default: false },
  "test-acceptance": { type: "boolean" as const, default: false },
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
// artifact. v8jsi2sb.dll comes from the vcbuild out dir (engineOutDir, alongside
// v8jsi2.dll); sbox.exe (the sandbox container) and v8sbhost.dll (the engine
// payload a consuming app loads) + v8sbhost.dll.lib from the gn out dir. The gn
// test binaries are deliberately excluded.
function stageSandboxBinaries(
  platform: string,
  engineOutDir: string,
  ridDir: string,
): void {
  const sbxOut = sandboxOutDir(platform);
  const { V8JSI_SB_DLL_NAME: sbDllName, V8SBHOST_DLL_NAME: hostDllName } = readV8JsiNames();
  // The jitless sandbox engine (vcbuild output, same dir as v8jsi2.dll). On x86
  // this is the degraded no-cage variant (still product-shipped); on x64/arm64
  // it carries the V8 in-process cage. Runtime payload only — no import lib
  // (v8sbhost LoadLibrary's it by name; the JSI C++ consumer links v8jsi2.dll.lib).
  copyFile(sbDllName, engineOutDir, ridDir, /*optional*/ true);
  // Optional: release/no-symbols builds skip the DLL-qualified PDB.
  copyFile(`${sbDllName}.pdb`, engineOutDir, ridDir, /*optional*/ true);
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
  // The Chromium-sandbox binaries (gn output) + v8sbhost's import lib.
  for (const f of [
    "sbox.exe",
    "sbox.exe.pdb",
    hostDllName,
    `${hostDllName}.pdb`,
    `${hostDllName}.lib`,
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
  --build             Build v8jsi2.dll (default: true)
  --test              Run v8jsi_test.exe + node_api_tests.exe, check the DLL's
                      embedded Stela manifest (stela verify + check), and check
                      Source Link in its and mkv8snapshot.exe's PDBs
  --pack              Create NuGet package
  --smoke             Run the real-consumer smoke (tests/run-smoke.ps1) against
                      the packed .nupkg (x64/x86/arm64; Release + Debug)
  --test-e2e          Run the full tests/ integration suite against the packed
                      .nupkg (x64/x86/arm64): the consumer smoke + the startup-snapshot
                      e2e (build a snapshot with packaged mkv8snapshot.exe and
                      consume it via V8RuntimeArgs::startupSnapshotBlob, both
                      engines, incl. cross-engine rejection)
  --test-acceptance   Like --test-e2e but against an already-built pack (e.g. the
                      downloaded signed NuGet in CI); forces the sandbox-engine
                      leg on (the final multi-engine pack always carries v8jsi2sb.dll).
                      The pack's sandbox binaries must be the shipped release-trust
                      build: the x64/ARM64 sandbox smoke requires the unsigned
                      consumer to succeed; sign it, then run
                      tests/run-smoke.ps1 -UseV8Sandbox -RunSigned
  --binskim           Run BinSkim security validation
  --check-crt         Report v8jsi2.dll's C/C++ runtime imports (Hybrid CRT check)
  --check-size        Enforce the binary-size budget (v8jsi2sb.dll <= 18 MiB)
  --count-exports     Report v8jsi2.dll's exported symbol counts by API family
  --build-v8jsisb     Build the sandbox variant v8jsi2sb.dll (V8 in-process sandbox
                      + pointer-compression shared cage + build-time jitless/lite)
                      instead of the mainline full-JIT v8jsi2.dll.
  --clean-all         Delete entire output folder
  --clean-build       Delete build outputs for targeted configs
  --clean-pkg         Delete pkg and pkg-staging folders

String arguments:
  --test-cet-runtime <dir> Run only CET runtime rows from a payload; requires CET,
                           no build or static tools. --build-v8jsisb selects sandbox.
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

The Chromium sandbox binaries (sbox.exe / v8sbhost.dll and their tests) are built
by a separate gn/ninja toolchain — see: node scripts/sbox-build.ts --help
`);
}

// =============================================================================
// Build Operation
// =============================================================================

async function runBuild(
  platform: string,
  configuration: string,
): Promise<void> {
  console.log(`\n=== Building v8jsi (${platform} ${configuration}) ===\n`);

  // vcbuild.bat accepts x86 / x64 / arm64 as order-independent target-arch
  // tokens and writes the binaries to out\<Config>\ regardless of arch, so
  // we forward the platform straight through. x64 is vcbuild's default, but
  // passing it explicitly is harmless and keeps the call uniform.
  // The sandbox variant uses the `v8jsisb` vcbuild token (which implies the
  // `v8jsi` target) so configure.py turns on the V8 sandbox + lite variables and
  // emits v8jsi2sb.dll. Mainline builds keep the plain `v8jsi` token unchanged.
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

  // vcbuild.bat runs the first python.exe on PATH; make it one that Node.js
  // configure accepts. The x86/x64 OpenSSL build also needs NASM on PATH;
  // arm64 skips the asm path.
  const env = { ...process.env };
  ensureSupportedPython(env);
  if (nasmRequiredForTarget(platform)) {
    prependToPath(env, ensureNasm(env));
  }

  run("cmd.exe", ["/c", ...vcbuildArgs], { cwd: nodejsDir, env });
}

// =============================================================================
// Test Operation
// =============================================================================

// No shell: preserve Windows argv and the full unsigned child exit code. Keep
// launcher stderr (including the host diagnostic and SKIP) visible in CI logs.
function runCetSuite(exe: string, args: string[], env = process.env): boolean {
  console.log(`> ${exe} ${args.join(" ")}`);
  const result = spawnSync(exe, args, { env, stdio: "inherit" });
  if (result.error) throw result.error;
  if (result.status !== 0) {
    console.error(`CET suite FAILED: exit=${result.status} signal=${result.signal}`);
    return false;
  }
  return true;
}

let loggedDumpbin = false;
function checkBinaryHeaders(platform: string, configuration: string): void {
  const dumpbin = findDumpbin();
  if (!loggedDumpbin) {
    console.log(`CET header gate: using ${dumpbin}`);
    loggedDumpbin = true;
  }
  const binaries: [string, boolean][] = [
    [v8jsiDllName(), platform === "x64"],
    ["mkv8snapshot.exe", false],
  ];
  if (platform === "x64" && !isCrossPlatformBuild(platform)) {
    binaries.push(["v8jsi_test.exe", true], ["cetrun.exe", true]);
  }
  for (const [name, expected] of binaries) {
    const binary = path.join(buildOutputDir(configuration), name);
    // vcbuild's v8jsi target list always includes mkv8snapshot, even for cross builds.
    if (!fs.existsSync(binary)) {
      throw new Error(`CET header gate: required binary not found: ${binary}. Build ${platform} ${configuration} first.`);
    }
    const headers = execSync(`"${dumpbin}" /headers "${binary}"`, { encoding: "utf8" });
    checkCetHeaders(headers, expected, binary, dumpbin);
    console.log(`CET header gate passed: ${name} (${platform}, CET compatible=${expected})`);
  }
}

function runTests(platform: string, configuration: string): void {
  checkBinaryHeaders(platform, configuration);
  const engineName = v8jsiDllName();
  console.log(`\n=== Testing ${engineName} (${platform} ${configuration}) ===\n`);

  // The embedded Stela manifest gate is a static check, so it runs for
  // cross-platform builds too: the DLL's exports match its manifest, and the
  // manifest matches the current headers.
  const engine = path.join(buildOutputDir(configuration), engineName);
  runStela(["verify", engine]);
  runStela(["check", engine, "--config", stelaConfig]);

  // Static too: the engine's and the snapshot tool's PDBs resolve our sources
  // on GitHub through Source Link.
  const pdbs = [pdbOf(engine)];
  const snapshotTool = path.join(buildOutputDir(configuration), "mkv8snapshot.exe");
  pdbs.push(pdbOf(snapshotTool)); // Required by the header gate above.
  if (!checkSourceLink(pdbs)) {
    console.error("Source Link check FAILED.");
    process.exit(1);
  }

  if (isCrossPlatformBuild(platform)) {
    console.log(
      `Skipping tests for cross-platform build (${platform} on ${process.arch} host)`,
    );
    return;
  }

  // Run both gtest binaries: the JSI suite and the Node-API conformance suite.
  const outDir = buildOutputDir(configuration);
  // Explicit child environments leave the caller's environment untouched.
  const ordinaryEnv = { ...process.env };
  for (const name of Object.keys(ordinaryEnv)) {
    if (["V8JSI_TEST_CET", "V8JSI_TEST_V8_FLAGS"].includes(name.toUpperCase())) delete ordinaryEnv[name];
  }
  for (const exeName of ["v8jsi_test.exe", "node_api_tests.exe"]) {
    const testExe = path.join(outDir, exeName);
    if (!fs.existsSync(testExe)) {
      console.error(`Test executable not found: ${testExe}`);
      process.exit(1);
    }
    run(testExe, ["--gtest_brief=1"], { env: ordinaryEnv });
  }
  if (platform !== "x64") return;

  runCetTests(outDir, args["build-v8jsisb"], false);
}

function runCetTests(outDir: string, sandbox: boolean, requireCet: boolean): void {
  const cetEnv = { ...process.env };
  // Never re-add ambient CETRUN_* passthrough to strict CI evidence rows
  // (including nested node_lite launches); set their controls explicitly.
  for (const name of Object.keys(cetEnv)) {
    if (name.toUpperCase().startsWith("CETRUN_") ||
        ["V8JSI_TEST_CET", "V8JSI_TEST_V8_FLAGS"].includes(name.toUpperCase())) delete cetEnv[name];
  }
  // Probe once with a bounded, tiny child; the full suites stream uncapped.
  const probe = spawnSync(path.join(outDir, "cetrun.exe"),
    ["off", path.join(outDir, "v8jsi_test.exe"), "--cetrun-child", "echo"],
    { env: cetEnv, encoding: "utf8", timeout: 15000 });
  if (probe.error) throw probe.error;
  if (probe.status !== 0 || !/user_cet=(yes|no) forced_unavailable=0/.test(probe.stderr)) {
    throw new Error(`CET availability probe failed: ${probe.status} ${probe.stderr}`);
  }
  const cetAvailable = /user_cet=yes/.test(probe.stderr);
  if (shouldDeferCetTests(process.env.V8JSI_CET_TESTS_DEFERRED, cetAvailable, requireCet)) {
    console.log("CET rows run in the Windows Server CET jobs");
    return;
  }
  if (requireCet && !cetAvailable) throw new Error("User CET is required on this test host");
  if (cetAvailable) cetEnv.CETRUN_REQUIRE_CET = "1";
  console.log("\n=== CET launcher contract tests ===");
  if (!runCetSuite(process.execPath, ["--test", path.join(repoRoot, "scripts", "cetrun.test.ts")],
    { ...cetEnv, CETRUN_TEST_OUT: outDir })) throw new Error("CET launcher tests failed");
  cetEnv.CETRUN_TIMEOUT_MS = String(30 * 60 * 1000);
  // Death-test children do not inherit policy: the CETCOMPAT test exe runs
  // them in compat mode on CET hosts, which is intentional.
  let passed = true;
  console.log("\n=== CET off: full JSI suite (no extra flags) ===");
  passed = runCetSuite(path.join(outDir, "cetrun.exe"),
    ["off", path.join(outDir, "v8jsi_test.exe"), "--gtest_brief=1"], cetEnv);
  console.log("\n=== Strict CET: full JSI suite (flags: --allow-natives-syntax) ===");
  passed = runCetSuite(path.join(outDir, "cetrun.exe"),
    ["strict", path.join(outDir, "v8jsi_test.exe"), "--gtest_brief=1"],
    { ...cetEnv, V8JSI_TEST_V8_FLAGS: "--allow-natives-syntax" }) && passed;
  // The sandbox engine is jitless; run its full suites, not the flag matrix.
  if (!sandbox) {
    const interpreterFlags = "--allow-natives-syntax --no-sparkplug --no-maglev --no-turbofan";
    // Explicit policies cover both process behavior and host-flag overrides.
    // Only the JSPI row opts out of compatible code in a shadow-stack process.
    const rows = [
      ["interpreter", "strict", interpreterFlags, "*CetShadowStack.*"],
      ["jitless", "strict", "--allow-natives-syntax --jitless", "*CetShadowStack.*"],
      ["native-stack", "strict", `${interpreterFlags} --interpreted-frames-native-stack`, "*CetShadowStack.*"],
      ["Wasm", "off", "--allow-natives-syntax", "*CetShadowStack.Jspi*:*CetShadowStack.Wasm*"],
      ["Wasm", "compat", "--allow-natives-syntax", "*CetShadowStack.Jspi*:*CetShadowStack.Wasm*"],
      ["compatible code", "off", "--allow-natives-syntax --cet-compatible", "*CetShadowStack.*:*JsiAbiInspector.Cet*"],
      ["JSPI override", "strict", "--allow-natives-syntax --no-cet-compatible", "*CetShadowStack.Jspi*"],
    ];
    for (const [name, mode, flags, filter] of rows) {
      console.log(`\n=== CET ${name}: ${mode} (flags: ${flags}) ===`);
      passed = runCetSuite(path.join(outDir, "cetrun.exe"),
        [mode, path.join(outDir, "v8jsi_test.exe"), `--gtest_filter=${filter}`,
          "--gtest_fail_if_no_test_selected=1", "--gtest_brief=1"],
        { ...cetEnv, V8JSI_TEST_V8_FLAGS: flags }) && passed;
    }
  }
  console.log("\n=== Strict CET: per-child Node-API suite ===");
  passed = runCetSuite(path.join(outDir, "node_api_tests.exe"), ["--gtest_brief=1"],
    { ...cetEnv, V8JSI_TEST_CET: "strict" }) && passed;
  if (!passed) throw new Error("CET tests failed");
  const summary = cetAvailable ? "Strict CET summary: JSI and Node-API rows passed" :
    "Strict CET summary: JSI and Node-API rows SKIPPED (SKIP: user CET not available)";
  console.log(summary);
  if (!cetAvailable) console.log(`##vso[task.logissue type=warning]${summary}`);
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
  // Local copies prevent one architecture's residuals leaking into another.
  const engineRules = new Set(acceptedBinSkimRules);
  if (platform !== "x64") engineRules.add("BA2025");
  // The tool links no V8/assembly and enables x86 SafeSEH (no BA2018 residual).
  // Non-x64 lacks CET; x64 intentionally opts out to make ordinary-host blobs.
  const snapshotToolRules = new Set(["BA2025"]);
  const engineOk = analyzeBinSkim(
    binskimExe,
    [dllPath],
    path.join(outDir, `${path.parse(dllName).name}.binskim.sarif`),
    `${path.parse(dllName).name} ${platform} ${configuration}`,
    engineRules,
  );

  const snapshotTool = path.join(outDir, "mkv8snapshot.exe");
  let snapshotToolOk = true;
  if (fs.existsSync(snapshotTool)) {
    snapshotToolOk = analyzeBinSkim(
      binskimExe,
      [snapshotTool],
      path.join(outDir, "mkv8snapshot.binskim.sarif"),
      `mkv8snapshot ${platform} ${configuration}`,
      snapshotToolRules,
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
// (--build-v8jsisb) emits v8jsi2sb.dll; the mainline build emits v8jsi2.dll. The
// hygiene gates (--binskim, --check-crt, --count-exports) follow the variant so
// they validate whichever DLL was just built.
function v8jsiDllName(): string {
  const names = readV8JsiNames();
  return args["build-v8jsisb"] ? names.V8JSI_SB_DLL_NAME : names.V8JSI_DLL_NAME;
}

function builtDllPath(configuration: string): string {
  const dll = path.join(buildOutputDir(configuration), v8jsiDllName());
  if (!fs.existsSync(dll)) {
    console.error(`${v8jsiDllName()} not found: ${dll}`);
    process.exit(1);
  }
  return dll;
}

// Report v8jsi2.dll's C/C++ runtime imports. The Hybrid CRT contract means the
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

// Report v8jsi2.dll's exported symbol counts by API family. Handy for spotting
// an unexpected change in the export surface.
function reportExports(platform: string, configuration: string): void {
  console.log(`\n=== Export counts (${platform} ${configuration}) ===\n`);
  const dll = builtDllPath(configuration);
  const names = dumpbinExportNames(dll);
  const count = (prefix: string): number =>
    names.filter((n: string) => n.startsWith(prefix)).length;

  console.log(`napi_*:     ${count("napi_")}`);
  console.log(`v8_node_api_*: ${count("v8_node_api_")}`);
  console.log(`node_api_*: ${count("node_api_")}`);
  console.log(`v8_*:       ${count("v8_")}`);
  console.log(`total:      ${names.length}`);
}

// Enforce the binary-size budget on the built engine DLL. Only the sandbox
// variant v8jsi2sb.dll has a ceiling (17.06 MB jitless+lite, budget <= 18 MiB);
// the full-JIT v8jsi2.dll is reported but not gated. Exits non-zero on breach.
// Re-baseline only on a deliberate, justified increase (e.g. a V8 bump).
function checkDllSize(platform: string, configuration: string): void {
  console.log(`\n=== Binary size budget (${platform} ${configuration}) ===\n`);
  const dll = builtDllPath(configuration);
  const name = path.basename(dll);
  const size = fs.statSync(dll).size;
  const mib = (n: number) => (n / (1024 * 1024)).toFixed(2);
  const budget = args["build-v8jsisb"] ? 18 * 1024 * 1024 : undefined;
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
  const names = readV8JsiNames();
  const v8jsiName = names.V8JSI_DLL_NAME;

  // ---------------------------------------------------------------------
  // Stage RID-specific content for each requested platform: DLL + import
  // lib + (optional) PDB. The build host is assumed to have built each
  // requested platform last (vcbuild.bat outputs to a single
  // deps/nodejs/out/<Config>/ directory regardless of architecture, so
  // multi-arch packing assumes prior matrix runs staged each arch).
  // ---------------------------------------------------------------------
  for (const platform of platforms) {
    for (const configuration of configurations) {
      // Debug v8jsi2.dll variants are dropped from the shipped NuGet. The
      // Hybrid CRT makes a Debug-CRT consumer safe to link against the
      // Release v8jsi2.dll: the App-CRT is statically private to the DLL,
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
      // build dir has no v8jsi2.dll, fall back to whatever is already
      // staged for this RID (idempotent re-run is fine).
      if (!fs.existsSync(path.join(outDir, v8jsiName))) {
        if (fs.existsSync(path.join(ridDir, v8jsiName))) {
          console.log(
            `Using pre-staged ${v8jsiName} for ${rid} (no local build at ${outDir}).`,
          );
          continue;
        }
        throw new Error(
          `Cannot stage ${rid}: no ${v8jsiName} at ${outDir} or ${ridDir}.`,
        );
      }

      copyFile(v8jsiName, outDir, ridDir);
      copyFile(`${v8jsiName}.lib`, outDir, ridDir);
      copyFile(`${v8jsiName}.pdb`, outDir, ridDir, /*optional*/ true);

      // Sandbox binaries: the jitless sandbox engine v8jsi2sb.dll (vcbuild
      // output, same out dir as v8jsi2.dll) plus the Chromium-sandbox binaries
      // sbox.exe / v8sbhost.dll + v8sbhost.dll.lib (gn output, per-arch out dir).
      // All optional: a non-sandbox build won't have produced them, and the CI
      // aggregator pre-stages them from the `sandbox-binaries` artifact, so a
      // missing local file just means "already staged / not built here".
      // The gn test binaries are deliberately NOT staged.
      stageSandboxBinaries(platform, outDir, ridDir);
    }
  }

  for (const rid of ALL_WINDOWS_RIDS) {
    const ridDir = path.join(pkgStagingPath, "build", "native", rid);
    if (platforms.map(platformToRid).includes(rid) || fs.existsSync(ridDir)) {
      checkV8JsiPayload(ridDir, names);
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
    "v8jsi_names.h",
    "compat.h",
    "Readme.md",
    "ScriptStore.h",
    "V8JsiRuntime.h",
  ]) {
    copyFile(file, path.join(srcDir, "public"), includeDir);
  }

  // JSI headers + base-class impl + inline impl. jsi.cpp ships the JSI
  // base-class implementations (Buffer/Value/JSError/Runtime destructors
  // and virtual default impls) since v8jsi2.dll does not export them
  // (JSI_EXPORT is a no-op unless CREATE_SHARED_LIBRARY is set, which
  // v8jsi.gyp doesn't define), so every consumer compiles jsi.cpp locally.
  for (const file of ["jsi.h", "jsi-inl.h", "jsi.cpp", "instrumentation.h"]) {
    copyFile(file, path.join(srcDir, "jsi"), jsiHeaderDir);
  }

  // JSI ABI headers + sources. Consumers compile JsiAbiRuntime.cpp locally;
  // v8_jsi_abi.h and v8_node_api_attach.h are the public C-stable surfaces
  // for runtime configuration and the dual-API attach seam respectively.
  for (const file of [
    "jsi_abi.h",
    "jsi_abi_stela.h",
    "jsi_abi_helpers.h",
    "JsiAbiRuntime.h",
    "JsiAbiRuntime.cpp",
    "v8_jsi_abi.h",
    "v8_node_api_attach.h",
  ]) {
    copyFile(file, path.join(srcDir, "jsi_abi"), jsiAbiDir);
  }

  // Public consumer-side TU. Holds makeV8Runtime + the V8ScriptCache and
  // V8TaskRunner adapters. Compiled by the consumer via the .targets
  // file's <ClCompile> include.
  copyFile("V8JsiRuntime.cpp", path.join(srcDir, "public"), jsiPublicDir);

  // V8's Node-API hosting header and its generated annotations.
  for (const file of [
    "js_native_api_types.h",
    "js_native_api.h",
    "v8_node_api.h",
    "v8_node_api_stela.h",
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

  // Sandbox public C-ABI headers, staged RID-independently under
  // build/native/include/sbox and build/native/include/v8sbhost.
  //   sbox.h            — the sbox.exe plugin ABI (self-contained: <stddef.h>/<stdint.h>)
  //   sbox_stela.h      — the Stela annotation macros sbox.h includes
  //   sbox_harden.h     — header-only DLL-search hardening + WinVerifyTrust helpers
  //   v8sbhost_client.h — the client ABI an app compiles against to load
  //                       v8sbhost.dll and run sandboxed JavaScript
  //   v8sbhost_stela.h  — the Stela annotation macros v8sbhost_client.h includes
  // msg_channel.h and the sbox_*internal headers are NOT shipped — they're
  // internal to the sandbox binaries.
  const sboxIncludeDir = path.join(includeDir, "sbox");
  const sboxHeaderSrc = path.join(repoRoot, "deps", "chromium", "sbox");
  for (const file of ["sbox.h", "sbox_stela.h", "sbox_harden.h"]) {
    copyFile(file, sboxHeaderSrc, sboxIncludeDir);
  }
  for (const file of ["v8sbhost_client.h", "v8sbhost_stela.h"]) {
    copyFile(file, path.join(srcDir, "public"), path.join(includeDir, "v8sbhost"));
  }

  // Stela models of the shipped C ABI headers, extracted from the same source
  // tree so they describe exactly the headers above. Each binary's embedded
  // manifest names its libraries' model by SHA-256 (`stela show`).
  const stelaModel = path.join(outputPath, "stela", "stela.model.json");
  runStela(["extract", "--config", stelaConfig, "--out", stelaModel]);
  runStela(["baseline", "--config", stelaConfig, "--model", stelaModel, "--out", path.join(pkgStagingPath, "build", "native", "stela"), ...shippedStelaLibraries]);

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
  // Node.js-aligned 26.x.y scheme carried in config.json.v8jsi_version, which
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
      path.join(pkgStagingPath, "build", "native", r, v8jsiName),
    ),
  );
  if (allRidsPresent) {
    packOne("Microsoft.JavaScript.V8.nuspec");
  } else {
    const missing = ALL_WINDOWS_RIDS.filter(
      (r) =>
        !fs.existsSync(
          path.join(pkgStagingPath, "build", "native", r, v8jsiName),
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
// per-RID .nupkg. The harness supports x64, x86, and arm64. Each scenario runs
// against the default engine (v8jsi) in the given consumer configs, then once
// more on the sandbox engine (v8jsisb, -UseV8Sandbox). requireSandbox forces the
// sandbox leg on (acceptance against a sandbox-complete pack); otherwise it runs
// only when the pack actually staged a v8jsi2sb.dll for this RID. The package
// supports UseV8Sandbox for x64 and ARM64 consumers only, so on x86 the leg
// asserts that the package rejects it. sandboxArgs are extra script arguments
// for the sandbox leg only.
function runHarnessScenario(
  scenario: string,
  outputPath: string,
  platforms: string[],
  consumerConfigs: string[],
  requireSandbox: boolean = false,
  sandboxArgs: string[] = [],
): void {
  const harnessPlatforms = platforms.filter(
    (platform) =>
      platform === "x64" || platform === "x86" || platform === "arm64",
  );
  if (harnessPlatforms.length === 0) {
    console.log(
      `Skipping ${scenario}: the harness supports x64, x86, and arm64, but none was targeted.`,
    );
    return;
  }

  const script = path.join(repoRoot, "tests", `run-${scenario}.ps1`);
  const pkgDir = path.join(outputPath, "pkg");
  for (const platform of harnessPlatforms) {
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
        platform,
        "-PkgDir",
        `"${pkgDir}"`,
      ];
      if (sandbox) psArgs.push("-UseV8Sandbox", ...sandboxArgs);
      run("pwsh", psArgs);
    };

    for (const config of consumerConfigs) {
      invoke(config, /*sandbox*/ false);
    }

    // Sandbox-engine leg: only when the pack actually carries the sandbox
    // binaries (a non-sandbox build doesn't). Drives v8jsi2sb.dll.
    const stagedV8jsisb = path.join(
      outputPath,
      "pkg-staging",
      "build",
      "native",
      platformToRid(platform),
      readV8JsiNames().V8JSI_SB_DLL_NAME,
    );
    if (requireSandbox || fs.existsSync(stagedV8jsisb)) {
      const leg =
        platform === "x86"
          ? "UseV8Sandbox must be rejected for x86 consumers"
          : "sandbox engine";
      console.log(`\n=== ${scenario} (${platform}, ${leg}) ===\n`);
      invoke("Release", /*sandbox*/ true);
    } else {
      console.log(
        `Skipping ${scenario} sandbox leg for ${platform}: no ${readV8JsiNames().V8JSI_SB_DLL_NAME} staged (non-sandbox pack).`,
      );
    }
  }
}

// Real-consumer smoke: builds the consumer in Release + Debug against the
// Release v8jsi2.dll (the Debug/Release leg exercises the Hybrid CRT cross-CRT
// boundary) and runs the JSI ABI checks. Under acceptance the pack carries the
// shipped release-trust sandbox binaries, which require signed payloads but
// accept an unsigned client: the sandbox leg requires the unsigned consumer to
// succeed, and CI signs it and runs `tests/run-smoke.ps1 -UseV8Sandbox -RunSigned`.
function runSmoke(
  outputPath: string,
  platforms: string[],
  requireSandbox: boolean = false,
): void {
  console.log("\n=== Smoke (real-consumer) ===\n");
  runHarnessScenario(
    "smoke",
    outputPath,
    platforms,
    ["Release", "Debug"],
    requireSandbox,
    requireSandbox ? ["-ReleaseTrustPayload"] : [],
  );
}

// Startup-snapshot e2e: builds a snapshot with the packaged mkv8snapshot.exe and
// consumes it via the public V8RuntimeArgs::startupSnapshotBlob surface, for both
// engines, including cross-engine rejection. Release consumer is sufficient (the
// scenario exercises the snapshot path, not the cross-CRT matrix).
function runSnapshotE2E(
  outputPath: string,
  platforms: string[],
  requireSandbox: boolean = false,
): void {
  console.log("\n=== Snapshot e2e ===\n");
  runHarnessScenario(
    "snapshot",
    outputPath,
    platforms,
    ["Release"],
    requireSandbox,
  );
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

  if (args["test-cet-runtime"] !== undefined) {
    runCetTests(path.resolve(args["test-cet-runtime"]), args["build-v8jsisb"], true);
    return;
  }

  // Validate arguments. platform/configuration are parseArgs multiple:true
  // options (always arrays), so narrow the string | string[] union.
  const platforms = ((args.platform ?? ["x64"]) as string[]).map((p) =>
    p.toLowerCase(),
  );
  const configurations = ((args.configuration ?? ["release"]) as string[]).map(
    (c) => c.toLowerCase(),
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
        await runBuild(platform, configuration);
        if (!args.test) checkBinaryHeaders(platform, configuration);
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
  // suite (smoke + snapshot); --smoke is the focused subset. --test-acceptance
  // runs the same suite against an already-built pack (e.g. the downloaded
  // signed NuGet) and forces the sandbox-engine leg on. --smoke/--test-e2e
  // imply the smoke; --test-e2e/--test-acceptance add the snapshot e2e.
  const requireSandbox = args["test-acceptance"];
  if (args.smoke || args["test-e2e"] || args["test-acceptance"]) {
    runSmoke(outputPath, platforms, requireSandbox);
  }
  if (args["test-e2e"] || args["test-acceptance"]) {
    runSnapshotE2E(outputPath, platforms, requireSandbox);
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
