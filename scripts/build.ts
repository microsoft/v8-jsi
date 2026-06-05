// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Build script for v8-jsi. Run with Node.js v24+ (TypeScript strip-types).
// Usage: node scripts/build.ts [options]

import { parseArgs } from "node:util";
import { execSync, type ExecSyncOptions } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import * as https from "node:https";

// =============================================================================
// Constants
// =============================================================================

const repoRoot = path.resolve(import.meta.dirname, "..");
const nodejsDir = path.join(repoRoot, "deps", "nodejs");
const srcDir = path.join(repoRoot, "src");

const binskimPackageName = "Microsoft.CodeAnalysis.BinSkim";
const binskimVersion = "4.4.9";
const binskimNuGetSource = "https://api.nuget.org/v3/index.json";
const nugetDownloadUrl =
  "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe";

// =============================================================================
// CLI Parsing
// =============================================================================

const options = {
  help: { type: "boolean" as const, default: false },
  build: { type: "boolean" as const, default: true },
  test: { type: "boolean" as const, default: false },
  pack: { type: "boolean" as const, default: false },
  binskim: { type: "boolean" as const, default: false },
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

function ensureDir(dirPath: string): void {
  if (!fs.existsSync(dirPath)) {
    fs.mkdirSync(dirPath, { recursive: true });
  }
}

function deleteDir(dirPath: string): void {
  if (fs.existsSync(dirPath)) {
    fs.rmSync(dirPath, { recursive: true, force: true });
  }
}

function run(
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

function copyFile(
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

function downloadFile(url: string, destPath: string): Promise<void> {
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

async function ensureNuGet(toolsPath: string): Promise<string> {
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
  --binskim           Run BinSkim security validation
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
`);
}

// =============================================================================
// Build Operation
// =============================================================================

function runBuild(platform: string, configuration: string): void {
  console.log(`\n=== Building v8jsi (${platform} ${configuration}) ===\n`);

  if (platform !== "x64") {
    console.log(
      `WARNING: vcbuild.bat currently only supports x64 builds. Skipping ${platform}.`,
    );
    return;
  }

  const vcbuildArgs = [
    ".\\vcbuild.bat",
    configuration,
    "v8jsi",
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

  run("cmd.exe", ["/c", ...vcbuildArgs], { cwd: nodejsDir });
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

  const testExe = path.join(buildOutputDir(configuration), "v8jsi_test.exe");
  if (!fs.existsSync(testExe)) {
    console.error(`Test executable not found: ${testExe}`);
    process.exit(1);
  }

  run(testExe, []);
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
    ]);
  }

  // Find BinSkim.exe
  let binskimExe = "";
  const toolsDir = path.join(binskimPkgDir, "tools");
  if (fs.existsSync(toolsDir)) {
    for (const runtime of fs.readdirSync(toolsDir)) {
      const candidate = path.join(
        toolsDir,
        runtime,
        "win-x64",
        "BinSkim.exe",
      );
      if (fs.existsSync(candidate)) {
        binskimExe = candidate;
        break;
      }
    }
  }

  if (!binskimExe) {
    console.error("BinSkim.exe not found in installed package");
    process.exit(1);
  }

  // Collect files to scan
  const outDir = buildOutputDir(configuration);
  const filesToScan: string[] = [];
  const dllPath = path.join(outDir, "v8jsi.dll");
  if (fs.existsSync(dllPath)) {
    filesToScan.push(dllPath);
  }

  if (filesToScan.length === 0) {
    console.log("No binaries found to scan");
    return;
  }

  const fileArgs = filesToScan.map((f) => `"${f}"`).join(" ");
  run(binskimExe, [
    "analyze",
    "--config",
    "default",
    "--ignorePdbLoadError",
    "--ignorePELoadErrors",
    "True",
    "--hashes",
    "--statistics",
    "--disable-telemetry",
    "True",
    fileArgs,
  ]);
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
  // Windows-only today; .so/.dylib substitutions land with Phase-2/3 multi-OS.
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
      // Stage B2: Debug v8jsi.dll variants are dropped from the shipped
      // NuGet. Hybrid CRT (B1) makes a Debug-CRT consumer safe to link
      // against the Release v8jsi.dll: App-CRT is statically private to
      // the DLL, UCRT is the only shared runtime, and ucrtbase.dll /
      // ucrtbased.dll are separate DLLs with separate heaps - so the
      // Stage-A "no STL across the boundary" rule keeps the cross-CRT
      // allocations sound. Debug builds may still run in CI for
      // validation; they just don't get packaged.
      // See: ../kb/v8-jsi/new-v8jsi-dll-phase1/impl-b2-results.md.
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
      // PDB is optional (Q2 decision: ship in per-RID .nupkg when present).
      copyFile("v8jsi.dll.pdb", outDir, ridDir, true);
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

  // Public consumer-side TU. Holds makeV8Runtime + the V8ScriptCache (W3) and
  // V8TaskRunner (W4) adapters. Compiled by the consumer via the .targets
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
  const version = args["semantic-version"] || configJson.version;
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
        runBuild(platform, configuration);
      }
      if (args.test) {
        runTests(platform, configuration);
      }
      if (args.binskim) {
        await runBinSkim(platform, configuration, toolsPath);
      }
    }
  }

  // Package operation (after all builds)
  if (args.pack) {
    packNuGet(outputPath, platforms, configurations);
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
