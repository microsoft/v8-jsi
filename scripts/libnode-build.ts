// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Build script for the Microsoft.JavaScript.LibNode packages: Node.js from
// deps/nodejs built as a shared library (libnode) with the embedding API
// (deps/nodejs/src/node_embedding_api.h). Run with Node.js v24+ (TypeScript
// strip-types).
//
// Each platform builds, tests, checks, and stages on a machine of its own
// architecture: cross-compiling would turn off Node's startup snapshot. --pack
// then makes the packages from the staged platforms, and --smoke builds a C
// consumer from a platform package and runs tests/libnode/features.js with it.
//
//   node scripts/libnode-build.ts [--platform x64|arm64] [--configuration release|debug]
//        [--no-build] [--test] [--check] [--stage <dir>]
//   node scripts/libnode-build.ts --pack <stage-root> --output <dir> [--semantic-version <v>]
//   node scripts/libnode-build.ts --smoke <package-dir> [--platform x64|arm64]

import { parseArgs } from "node:util";
import { execSync, spawn } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";

import {
  repoRoot,
  analyzeBinSkim,
  deleteDir,
  dumpbinExportNames,
  ensureBinSkimExe,
  ensureDir,
  ensureNasm,
  ensureSupportedPython,
  findDumpbin,
  prependToPath,
  run,
  vswhereExe,
} from "./build-common.ts";

// =============================================================================
// Constants
// =============================================================================

const nodejsDir = path.join(repoRoot, "deps", "nodejs");
const nodejsSrcDir = path.join(nodejsDir, "src");
const templatesDir = path.join(repoRoot, "scripts", "nuget-libnode");
const smokeSourceDir = path.join(repoRoot, "tests", "libnode");
const stelaCli = path.join(repoRoot, "tools", "stela", "src", "stela.ts");
const stelaConfig = path.join(repoRoot, "stela.libnode.config.json");
const embeddingBaseline = path.join(repoRoot, "api", "node_embedding.model.json");

const packageId = "Microsoft.JavaScript.LibNode";
const allRids = ["win-x64", "win-arm64", "linux-x64", "linux-arm64", "osx-x64", "osx-arm64"];

// The public headers, by package include folder. The package keeps the 20.x
// layout: build/native/include/embedding and build/native/include/node-api.
const packageHeaders: Record<string, string[]> = {
  embedding: ["node_embedding_api.h", "node_embedding_api_cpp.h"],
  "node-api": ["js_native_api.h", "js_native_api_types.h", "node_api.h", "node_api_types.h"],
};

// BinSkim rules accepted as residuals on libnode.dll. The gate fails on any
// other finding.
//   BA2014 - functions without stack protection: Rust's precompiled standard
//            library, which the Temporal library links in (stable Rust has no
//            /GS equivalent), and compiler-generated V8 constructor closures,
//            the same false positive as on v8jsi.dll.
//   BA2025 - CET shadow stack off: V8's heap snapshot code fails under it, as
//            on v8jsi.dll.
const acceptedBinSkimRules = new Set(["BA2014", "BA2025"]);

// =============================================================================
// CLI
// =============================================================================

const options = {
  help: { type: "boolean" as const, default: false },
  platform: { type: "string" as const, default: process.arch },
  configuration: { type: "string" as const, default: "release" },
  build: { type: "boolean" as const, default: true },
  test: { type: "boolean" as const, default: false },
  check: { type: "boolean" as const, default: false },
  stage: { type: "string" as const },
  pack: { type: "string" as const },
  output: { type: "string" as const },
  smoke: { type: "string" as const },
  "semantic-version": { type: "string" as const },
  "tools-path": { type: "string" as const, default: path.join(repoRoot, "out", "tools") },
};

const { values: args } = parseArgs({ options, allowNegative: true });

function showHelp(): void {
  console.log(`Usage: node scripts/libnode-build.ts [options]

Builds libnode from deps/nodejs and makes the Microsoft.JavaScript.LibNode packages.

Per-platform operations (run on a machine of that architecture):
  --platform <x64|arm64>     Target architecture (default: this machine's)
  --configuration <release|debug>
                             Build configuration (default: release)
  --no-build                 Skip the build; use the existing output
  --test                     Run Node.js's embedding tests (tools/test.py embedding)
  --check                    Check the library: imports, exports, Stela manifest, BinSkim
  --stage <dir>              Copy the library, its import library and symbols, and
                             the public headers into <dir> for --pack

Package operations:
  --pack <stage-root>        Pack every <stage-root>/<rid> folder; the meta package
                             only when all of ${allRids.join(", ")} are staged
  --output <dir>             Package output folder (required with --pack)
  --semantic-version <v>     Package version (default: config.json v8jsi_version)
  --smoke <package-dir>      Build the C consumer in tests/libnode from the
                             --platform package in <package-dir> and run
                             tests/libnode/features.js with it

  --tools-path <dir>         Tool cache for NASM and BinSkim (default: out\\tools)
  --help                     Show this help`);
}

function fail(message: string): never {
  console.error(`ERROR: ${message}`);
  process.exit(1);
}

// =============================================================================
// Platform helpers
// =============================================================================

function hostOs(): "win" | "linux" | "osx" {
  switch (process.platform) {
    case "win32":
      return "win";
    case "linux":
      return "linux";
    case "darwin":
      return "osx";
    default:
      return fail(`Unsupported host platform: ${process.platform}`);
  }
}

function ridFor(platform: string): string {
  return `${hostOs()}-${platform}`;
}

function requireWindows(operation: string): void {
  if (hostOs() !== "win") {
    fail(`${operation} on ${process.platform} is not implemented yet; only Windows is supported.`);
  }
}

function configDir(configuration: string): string {
  return configuration === "debug" ? "Debug" : "Release";
}

function buildOutputDir(configuration: string): string {
  return path.join(nodejsDir, "out", configDir(configuration));
}

// The library file name a package ships for a RID. Linux and macOS builds name
// the library after NODE_MODULE_VERSION; the packages drop it, as before.
function packageLibraryName(rid: string): string {
  if (rid.startsWith("win-")) return "libnode.dll";
  if (rid.startsWith("linux-")) return "libnode.so";
  return "libnode.dylib";
}

function readJson<T>(file: string): T {
  return JSON.parse(fs.readFileSync(file, "utf-8")) as T;
}

function nodejsVersion(): string {
  const header = fs.readFileSync(path.join(nodejsSrcDir, "node_version.h"), "utf-8");
  const part = (name: string): string => {
    const match = new RegExp(`#define ${name} (\\d+)`).exec(header);
    if (!match) fail(`${name} not found in node_version.h`);
    return match[1];
  };
  return `${part("NODE_MAJOR_VERSION")}.${part("NODE_MINOR_VERSION")}.${part("NODE_PATCH_VERSION")}`;
}

function rootCertificateCount(): number {
  const header = fs.readFileSync(path.join(nodejsSrcDir, "node_root_certs.h"), "utf-8");
  return header.split("-----BEGIN CERTIFICATE-----").length - 1;
}

// The functions of embedding API version 1, from the committed Stela baseline.
function embeddingFunctions(): string[] {
  interface Model {
    libraries: { declarations: { kind: string; name: string }[] }[];
  }
  return readJson<Model>(embeddingBaseline)
    .libraries.flatMap((library) => library.declarations)
    .filter((declaration) => declaration.kind === "function")
    .map((declaration) => declaration.name);
}

function runStela(stelaArgs: string[]): void {
  run(`"${process.execPath}"`, [stelaCli, ...stelaArgs].map((a) => `"${a}"`), { cwd: repoRoot });
}

// Runs a command line in a Visual Studio developer environment for this host.
function runInVsEnvironment(commandLine: string, cwd: string): void {
  const vswhere = vswhereExe();
  if (!vswhere) fail("vswhere.exe not found; install Visual Studio with the C++ tools.");
  const installation = execSync(`"${vswhere}" -latest -products * -property installationPath`, {
    encoding: "utf-8",
  }).trim().split(/\r?\n/)[0];
  const vcvarsall = path.join(installation, "VC", "Auxiliary", "Build", "vcvarsall.bat");
  if (!fs.existsSync(vcvarsall)) fail(`vcvarsall.bat not found: ${vcvarsall}`);
  const hostArch = process.arch === "arm64" ? "arm64" : "x64";
  run(`call "${vcvarsall}" ${hostArch} >nul &&`, [commandLine], { cwd });
}

// =============================================================================
// Build
// =============================================================================

// Runs a command, echoing its output, and returns the lines that match pattern.
function runAndCollect(
  command: string,
  commandArgs: string[],
  cwd: string,
  env: NodeJS.ProcessEnv,
  pattern: RegExp,
): Promise<string[]> {
  console.log(`> ${[command, ...commandArgs].join(" ")}`);
  return new Promise((resolve) => {
    const child = spawn(command, commandArgs, { cwd, env, stdio: ["inherit", "pipe", "pipe"] });
    const matches: string[] = [];
    const scan = (stream: NodeJS.ReadableStream, sink: NodeJS.WriteStream) => {
      let pending = "";
      stream.setEncoding("utf-8");
      stream.on("data", (chunk: string) => {
        sink.write(chunk);
        const lines = (pending + chunk).split(/\r?\n/);
        pending = lines.pop() ?? "";
        for (const line of lines) if (pattern.test(line)) matches.push(line.trim());
      });
      stream.on("end", () => {
        if (pattern.test(pending)) matches.push(pending.trim());
      });
    };
    scan(child.stdout, process.stdout);
    scan(child.stderr, process.stderr);
    child.on("close", (code) => {
      if (code !== 0) fail(`${command} failed with exit code ${code}`);
      resolve(matches);
    });
  });
}

async function runBuild(platform: string, configuration: string, toolsPath: string): Promise<void> {
  requireWindows("Building libnode");
  console.log(`\n=== Building libnode (${ridFor(platform)} ${configuration}) ===\n`);

  // libnode with everything a full Node.js build has (full ICU, Temporal, and
  // Node's defaults), plus node.exe and embedtest.exe, which only the tests
  // use. The no-* flags skip the binaries nothing here needs.
  const vcbuildArgs = [
    ".\\vcbuild.bat",
    configuration,
    platform,
    "dll",
    "cfg",
    "full-icu",
    "v8temporal",
    "no-cctest",
    "no-fuzzers",
    "no-openssl-cli",
    "no-nop",
    "no-overlapped-checker",
  ];

  const env = { ...process.env };
  ensureSupportedPython(env);
  if (platform !== "arm64") {
    prependToPath(env, await ensureNasm(toolsPath));
  }
  // The gyp action that embeds the Stela manifest runs `node`; use this one.
  prependToPath(env, path.dirname(process.execPath));

  // clang-cl warns and goes on when it ignores an option it does not support
  // (it did for /Qspectre), so such a warning fails the build.
  const ignoredOptions = await runAndCollect(
    process.env.ComSpec ?? "cmd.exe",
    ["/d", "/c", ...vcbuildArgs],
    nodejsDir,
    env,
    /argument unused during compilation/,
  );
  if (ignoredOptions.length > 0) {
    for (const line of new Set(ignoredOptions)) console.error(`  ${line}`);
    fail("The compiler ignored the options above.");
  }

  const outDir = buildOutputDir(configuration);
  for (const file of ["libnode.dll", "libnode.lib", "libnode.pdb", "node.exe", "embedtest.exe"]) {
    if (!fs.existsSync(path.join(outDir, file))) fail(`Build output not found: ${path.join(outDir, file)}`);
  }
}

// =============================================================================
// Test
// =============================================================================

function runTests(configuration: string): void {
  requireWindows("Testing libnode");
  console.log(`\n=== Embedding tests (${configuration}) ===\n`);
  const env = { ...process.env };
  ensureSupportedPython(env);
  run("python", ["tools\\test.py", "-J", "-p", "tap", `--mode=${configuration}`, "embedding"], {
    cwd: nodejsDir,
    env,
  });
}

// =============================================================================
// Check
// =============================================================================

// The DLLs a PE file imports, from `dumpbin /DEPENDENTS`.
function dependentDlls(file: string): string[] {
  const out = execSync(`"${findDumpbin()}" /DEPENDENTS "${file}"`, { encoding: "utf-8" });
  const names: string[] = [];
  let inList = false;
  for (const line of out.split(/\r?\n/)) {
    if (/Image has the following dependencies:/.test(line)) {
      inList = true;
      continue;
    }
    if (!inList) continue;
    const name = line.trim();
    if (/\.dll$/i.test(name)) names.push(name);
    else if (names.length > 0 && name === "") break;
  }
  return names;
}

async function runChecks(platform: string, configuration: string, toolsPath: string): Promise<void> {
  requireWindows("Checking libnode");
  const rid = ridFor(platform);
  const library = path.join(buildOutputDir(configuration), "libnode.dll");
  if (!fs.existsSync(library)) fail(`Not found: ${library}`);
  let failed = false;

  console.log(`\n=== Imports (${rid} ${configuration}) ===\n`);
  // The Hybrid CRT: only Windows itself, no Visual C++ runtime DLLs.
  const system32 = path.join(process.env.SystemRoot ?? "C:\\Windows", "System32");
  for (const dll of dependentDlls(library)) {
    const isSystem =
      /^api-ms-win-/i.test(dll) || fs.existsSync(path.join(system32, dll));
    const isVcRuntime = /^(vcruntime|msvcp|concrt|vccorlib)/i.test(dll);
    const ok = isSystem && !isVcRuntime;
    console.log(`  ${ok ? "ok  " : "FAIL"} ${dll}`);
    if (!ok) failed = true;
  }

  console.log(`\n=== Exports (${rid} ${configuration}) ===\n`);
  const exported = new Set(dumpbinExportNames(library));
  const functions = embeddingFunctions();
  const missing = functions.filter((name) => !exported.has(name));
  const count = (prefix: string) => [...exported].filter((name) => name.startsWith(prefix)).length;
  console.log(`  embedding API: ${functions.length - missing.length} of ${functions.length} functions exported`);
  console.log(`  napi_*: ${count("napi_")}, node_api_*: ${count("node_api_")}, total: ${exported.size}`);
  for (const name of missing) console.error(`  missing: ${name}`);
  if (missing.length > 0) failed = true;

  console.log(`\n=== Stela manifest (${rid} ${configuration}) ===\n`);
  runStela(["verify", library]);
  runStela(["check", library, "--config", stelaConfig]);

  console.log(`\n=== BinSkim (${rid} ${configuration}) ===\n`);
  const binskim = await ensureBinSkimExe(toolsPath);
  const binskimOk = analyzeBinSkim(
    binskim,
    [library],
    path.join(buildOutputDir(configuration), "libnode.binskim.sarif"),
    `libnode ${rid} ${configuration}`,
    acceptedBinSkimRules,
  );
  if (!binskimOk) failed = true;

  if (failed) fail(`libnode ${rid} ${configuration} failed its checks.`);
  console.log(`\nlibnode ${rid} ${configuration} passed its checks.`);
}

// =============================================================================
// Stage
// =============================================================================

function stage(platform: string, configuration: string, stageDir: string): void {
  requireWindows("Staging libnode");
  const rid = ridFor(platform);
  console.log(`\n=== Staging ${rid} into ${stageDir} ===\n`);
  const outDir = buildOutputDir(configuration);
  deleteDir(stageDir);
  ensureDir(stageDir);
  for (const file of ["libnode.dll", "libnode.lib", "libnode.pdb"]) {
    fs.copyFileSync(path.join(outDir, file), path.join(stageDir, file));
  }
  for (const [folder, headers] of Object.entries(packageHeaders)) {
    const includeDir = path.join(stageDir, "include", folder);
    ensureDir(includeDir);
    for (const header of headers) {
      fs.copyFileSync(path.join(nodejsSrcDir, header), path.join(includeDir, header));
    }
  }
  console.log(`Staged ${rid}: ${fs.readdirSync(stageDir).join(", ")}`);
}

// =============================================================================
// Pack
// =============================================================================

function gitValue(gitArgs: string): string {
  try {
    return execSync(`git ${gitArgs}`, { encoding: "utf-8", cwd: repoRoot }).trim();
  } catch {
    console.log(`Warning: git ${gitArgs} failed`);
    return "unknown";
  }
}

function textWithLf(file: string): string {
  return fs.readFileSync(file, "utf-8").replaceAll("\r\n", "\n");
}

function pack(stageRoot: string, outputDir: string): void {
  requireWindows("Packing");
  const rids = allRids.filter((rid) => fs.existsSync(path.join(stageRoot, rid)));
  if (rids.length === 0) fail(`No staged platforms in ${stageRoot} (expected ${allRids.join(", ")})`);
  console.log(`\n=== Packing ${rids.join(", ")} ===\n`);

  interface ConfigJson {
    v8jsi_version?: string;
  }
  const version =
    args["semantic-version"] ?? readJson<ConfigJson>(path.join(repoRoot, "config.json")).v8jsi_version;
  if (!version) fail("No package version: pass --semantic-version or set config.json v8jsi_version.");

  const staging = path.join(outputDir, "staging");
  deleteDir(staging);
  ensureDir(staging);

  // Templates, the license and notices, and the per-RID props and targets.
  for (const file of fs.readdirSync(templatesDir)) {
    if (!file.includes(".rid.") || file.endsWith(".nuspec")) {
      fs.copyFileSync(path.join(templatesDir, file), path.join(staging, file));
    }
  }
  fs.copyFileSync(path.join(nodejsDir, "LICENSE"), path.join(staging, "LICENSE"));
  fs.copyFileSync(path.join(repoRoot, "scripts", "nuget", "NOTICE.txt"), path.join(staging, "NOTICE.txt"));
  for (const rid of rids) {
    for (const file of fs.readdirSync(templatesDir)) {
      if (!file.includes(".rid.") || file.endsWith(".nuspec")) continue;
      const text = fs
        .readFileSync(path.join(templatesDir, file), "utf-8")
        .replaceAll("$rid$", rid)
        .replaceAll("$libnodeName$", packageLibraryName(rid));
      fs.writeFileSync(path.join(staging, file.replace(".rid.", `.${rid}.`)), text);
    }
  }

  // The headers are the same for every platform; a difference means the
  // platforms were built from different sources. Windows checkouts stage them
  // with CRLF line endings, so they are compared, and packed, with LF.
  const firstInclude = path.join(stageRoot, rids[0], "include");
  for (const [folder, headers] of Object.entries(packageHeaders)) {
    const includeDir = path.join(staging, "build", "native", "include", folder);
    ensureDir(includeDir);
    for (const header of headers) {
      const text = textWithLf(path.join(firstInclude, folder, header));
      for (const rid of rids.slice(1)) {
        if (textWithLf(path.join(stageRoot, rid, "include", folder, header)) !== text) {
          fail(`${folder}/${header} differs between ${rids[0]} and ${rid}`);
        }
      }
      fs.writeFileSync(path.join(includeDir, header), text);
    }
  }

  for (const rid of rids) {
    const ridDir = path.join(staging, "build", "native", rid);
    ensureDir(ridDir);
    const stageDir = path.join(stageRoot, rid);
    const libraryName = packageLibraryName(rid);
    fs.copyFileSync(path.join(stageDir, libraryName), path.join(ridDir, libraryName));
    if (rid.startsWith("win-")) {
      fs.copyFileSync(path.join(stageDir, "libnode.lib"), path.join(ridDir, "libnode.lib"));
    }
  }

  const nuget = execSync("where nuget.exe", { encoding: "utf-8" }).trim().split(/\r?\n/)[0].trim();
  const baseProperties = [
    `nugetroot=${staging}`,
    `version=${version}`,
    `repoUrl=https://github.com/microsoft/v8-jsi`,
    `repoBranch=${gitValue("rev-parse --abbrev-ref HEAD")}`,
    `repoCommit=${gitValue("rev-parse HEAD")}`,
  ];
  const packOne = (nuspec: string, extraProperties: string[] = []) => {
    run(`"${nuget}"`, [
      "pack",
      `"${path.join(staging, nuspec)}"`,
      "-Properties",
      `"${[...baseProperties, ...extraProperties].join(";")}"`,
      "-OutputDirectory",
      `"${outputDir}"`,
      "-NonInteractive",
    ]);
  };

  for (const rid of rids) {
    packOne(`${packageId}.rid.nuspec`, [`rid=${rid}`, `libnodeName=${packageLibraryName(rid)}`]);
  }
  // The meta package depends on all platform packages; without all of them,
  // restoring it would fail.
  const missing = allRids.filter((rid) => !rids.includes(rid));
  if (missing.length === 0) {
    packOne(`${packageId}.nuspec`);
  } else {
    console.log(`\nSkipping the ${packageId} meta package: not staged: ${missing.join(", ")}.`);
  }
  console.log(`\nPackages ${version} are in ${outputDir}`);
}

// =============================================================================
// Smoke test
// =============================================================================

function runSmoke(platform: string, packageDir: string): void {
  requireWindows("The smoke test");
  const rid = ridFor(platform);
  const pattern = new RegExp(`^${packageId.replaceAll(".", "\\.")}\\.${rid}\\.\\d.*\\.nupkg$`);
  const packages = fs.readdirSync(packageDir).filter((file) => pattern.test(file));
  if (packages.length !== 1) {
    fail(`Expected one ${packageId}.${rid} package in ${packageDir}, found ${packages.length}`);
  }
  console.log(`\n=== Smoke test of ${packages[0]} ===\n`);

  const workDir = path.join(packageDir, "smoke", rid);
  const packageRoot = path.join(workDir, "package");
  deleteDir(workDir);
  ensureDir(packageRoot);
  run("tar", ["-xf", `"${path.join(packageDir, packages[0])}"`, "-C", `"${packageRoot}"`]);

  const include = path.join(packageRoot, "build", "native", "include");
  const libDir = path.join(packageRoot, "build", "native", rid);
  const exe = path.join(workDir, "libnode_smoke.exe");
  runInVsEnvironment(
    [
      "cl /nologo /W4 /WX /MT /EHsc /std:c++17",
      `/I "${path.join(include, "embedding")}"`,
      `/I "${path.join(include, "node-api")}"`,
      `"${path.join(smokeSourceDir, "smoke.c")}"`,
      `"${path.join(smokeSourceDir, "smoke_cpp.cc")}"`,
      `/Fe"${exe}"`,
      `/link /LIBPATH:"${libDir}" libnode.lib`,
    ].join(" "),
    workDir,
  );
  fs.copyFileSync(
    path.join(packageRoot, "runtimes", rid, "native", packageLibraryName(rid)),
    path.join(workDir, packageLibraryName(rid)),
  );
  run(`"${exe}"`, [
    `"${path.join(smokeSourceDir, "features.js")}"`,
    "--expect-version",
    nodejsVersion(),
    "--expect-roots",
    String(rootCertificateCount()),
  ], { cwd: workDir });
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

  const platform = (args.platform ?? process.arch).toLowerCase();
  if (!["x64", "arm64"].includes(platform)) fail(`Invalid --platform ${platform}: use x64 or arm64.`);
  const buildsHere = !args.pack && !args.smoke && (args.build || args.test);
  if (platform !== process.arch && (buildsHere || args.smoke)) {
    fail(`Build and test ${platform} on an ${platform} machine; this one is ${process.arch}.`);
  }
  const configuration = (args.configuration ?? "release").toLowerCase();
  if (!["release", "debug"].includes(configuration)) {
    fail(`Invalid --configuration ${configuration}: use release or debug.`);
  }
  const toolsPath = path.resolve(args["tools-path"]);

  if (args.pack || args.smoke) {
    if (args.pack) {
      if (!args.output) fail("--pack needs --output <dir>.");
      pack(path.resolve(args.pack), path.resolve(args.output));
    }
    if (args.smoke) runSmoke(platform, path.resolve(args.smoke));
  } else {
    if (args.build) await runBuild(platform, configuration, toolsPath);
    if (args.test) runTests(configuration);
    if (args.check) await runChecks(platform, configuration, toolsPath);
    if (args.stage) stage(platform, configuration, path.resolve(args.stage));
  }

  const seconds = Math.floor((Date.now() - startTime) / 1000);
  console.log(`\nlibnode-build took ${Math.floor(seconds / 60)}m ${seconds % 60}s`);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
