// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import { parseArgs } from "node:util";
import { execFileSync, spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { repoRoot, run, ensureSupportedPython, ensureNasm, nasmRequiredForTarget,
         prependToPath, findDumpbin, checkCetHeaders, dumpbinExportNames, ensureBinSkimExe, analyzeBinSkim, crtDependencyLines } from "./build-common.ts";
import { checkArchiveFileCfg, checkPeFlags, checkIteratorConfiguration, linkedArchiveNames, legacyBinaryChecks, checkLegacyStartupFlags, legacyCetArtifacts, checkLegacyHardeningProject, checkLegacyIdentity } from "./legacy-checks.ts";

import { stageLegacy, packLegacy } from "./legacy-package.ts";
import { checkLegacyExports, checkLegacySources, checkLegacyProject, checkLegacyIncludes } from "./legacy-checks.ts";

const { values: args } = parseArgs({ allowNegative: true, options: {
  platform: { type: "string", default: "x64" },
  configuration: { type: "string", default: "release" },
  build: { type: "boolean", default: true },
  test: { type: "boolean", default: false },
  check: { type: "boolean", default: false },
  stage: { type: "string" },
  pack: { type: "string" },
  output: { type: "string", default: "out/legacy/pkg" },
  "diagnostic-pack": { type: "boolean", default: false },
  help: { type: "boolean", default: false },
} });
if (args.help) {
  console.log("Usage: node scripts/legacy-build.ts --platform x64|x86|arm64 --configuration release|debug [--no-build] [--test] [--check] [--stage <dir>] | --pack <dir> [--output <dir>] [--diagnostic-pack]");
  process.exit(0);
}
if (process.platform !== "win32" || !["x64", "x86", "arm64"].includes(args.platform) ||
    !["release", "debug"].includes(args.configuration)) throw new Error("Invalid Windows build configuration");
if (args.pack) {
  if (args.stage || args.test || args.check) throw new Error("Pack cannot be combined with build operations");
  packLegacy(path.resolve(args.pack), path.resolve(args.output), args["diagnostic-pack"]);
  process.exit(0);
}
if (args["diagnostic-pack"]) throw new Error("--diagnostic-pack requires --pack");
if (args.stage && args.build) throw new Error("Staging requires --no-build");
const nodeDir = path.join(repoRoot, "deps/nodejs");
const output = path.join(nodeDir, "out", args.configuration === "release" ? "Release" : "Debug");
if (args.build) {
  const started = Date.now();
  fs.mkdirSync(path.join(repoRoot, "out", "legacy"), { recursive: true });
  execFileSync(process.execPath, [path.join(repoRoot, "src/generate_source_link.ts"),
    "--output", path.join(repoRoot, "out/legacy/source_link_gen.json")], { stdio: "inherit" });
  const env = { ...process.env };
  ensureSupportedPython(env);
  if (nasmRequiredForTarget(args.platform)) prependToPath(env, ensureNasm(env));
  run("cmd.exe", ["/c", ".\\vcbuild.bat", args.configuration, args.platform, "v8jsilegacy", "cfg", "without-intl",
                 "no-node", "no-cctest", "no-embedtest", "no-openssl-cli", "no-fuzzers", "no-nop", "no-overlapped-checker"],
      { cwd: nodeDir, env });
  const seconds = Math.floor((Date.now() - started) / 1000);
  console.log(`Build took ${Math.floor(seconds / 60)}m ${seconds % 60}s`);
}
if (args.test || args.check || args.stage) {
  const config = JSON.parse(fs.readFileSync(path.join(nodeDir, "config.gypi"), "utf8").replace(/^#.*$/mg, ""));
  if (config.target_defaults.default_configuration.toLowerCase() !== args.configuration) {
    throw new Error("Legacy configuration mismatch: default_configuration");
  }
  const v = config.variables;
  const compressed = args.platform === "x86" ? 0 : 1;
  const expected = { build_v8jsi_legacy: 1, control_flow_guard: "true", v8_enable_sandbox: 0,
    v8_enable_pointer_compression: compressed, v8_enable_pointer_compression_shared_cage: compressed,
    v8_enable_external_code_space: compressed, v8_enable_31bit_smis_on_64bit_arch: compressed,
    v8_enable_cet_shadow_stack: args.platform === "x64" ? 1 : 0,
    target_arch: args.platform === "x86" ? "ia32" : args.platform };
  for (const [key, value] of Object.entries(expected)) {
    if (v[key] !== value) throw new Error(`Legacy configuration mismatch: ${key}`);
  }
}
if (args.test) {
  if (args.platform !== process.arch && !(args.platform === "x86" && process.arch === "x64")) {
    throw new Error("Legacy tests must run on a native host (x86 also supports x64 WOW64)");
  }
  run(`"${path.join(output, "v8jsi_legacy_probe_runner.exe")}"`, [], { cwd: output });
  execFileSync(path.join(output, "v8jsi_legacy_task_tests.exe"), [], { cwd: output, stdio: "inherit", timeout: 60000 });
  const suiteEnv = Object.fromEntries(Object.entries(process.env).filter(([key]) =>
    !/^(CETRUN_|V8JSI_TEST_)/i.test(key)));
  // ETW's process-global initialization must see the requested instrumentation flags first.
  execFileSync(path.join(output, "jsitests_legacy.exe"),
    ["--gtest_filter=LegacyInstrumentation.*", "--gtest_brief=1"],
    { cwd: output, env: suiteEnv, stdio: "inherit", timeout: 60000 });
  for (const name of ["jsitests_legacy", "node_api_tests_legacy", "inspector_tests_legacy", "layout_tests_legacy"]) {
    execFileSync(path.join(output, name + ".exe"), ["--gtest_brief=1"],
      { cwd: output, env: suiteEnv, stdio: "inherit", timeout: 300000 });
  }
  if (args.platform === "x64") {
    const env = Object.fromEntries(Object.entries(process.env).filter(([key]) =>
      !/^(CETRUN_|V8JSI_TEST_)/i.test(key)));
    env.CETRUN_TIMEOUT_MS = "60000";
    let available = false;
    // Fresh policy children keep V8's frozen flags and startup defaults independent.
    for (const [mode, flag, expected] of [
      ["off", "", "--no-cet-compatible"],
      ["off", "--cet-compatible", "--cet-compatible"],
      ["strict", "", "--cet-compatible"],
      ["strict", "--no-cet-compatible", "--no-cet-compatible"],
    ]) {
      if (mode === "strict" && available) env.CETRUN_REQUIRE_CET = "1";
      const child = spawnSync(path.join(output, "cetrun_legacy.exe"),
        [mode, path.join(output, "v8jsi_legacy_startup_tests.exe"), ...(flag ? [flag] : [])],
        { cwd: output, env, encoding: "utf8", timeout: 90000 });
      if (child.error) throw child.error;
      if (child.status !== 0) throw new Error(`Legacy startup child failed: ${child.status} ${child.stdout} ${child.stderr}`);
      console.error(child.stderr.trim());
      if (child.stderr.includes("user_cet=yes")) available = true;
      const result = child.stdout;
      if (child.stderr.includes("SKIP: user CET not available")) {
        console.log("CET startup coverage pending: user CET not available");
        continue;
      }
      checkLegacyStartupFlags(result, expected, mode === "strict");
      console.log(`CET startup ${mode} ${flag || "default"}: ${expected}; initialized once`);
    }
    for (const mode of ["off", "strict"]) {
      const childEnv = { ...suiteEnv, V8JSI_TEST_CET: mode, CETRUN_TIMEOUT_MS: "300000",
        ...(available ? { CETRUN_REQUIRE_CET: "1" } : {}) };
      execFileSync(path.join(output, "cetrun_legacy.exe"),
        [mode, path.join(output, "jsitests_legacy.exe"), "--gtest_filter=LegacyInstrumentation.*", "--gtest_brief=1"],
        { cwd: output, env: childEnv, stdio: "inherit", timeout: 90000 });
      for (const name of ["jsitests_legacy", "node_api_tests_legacy", "inspector_tests_legacy"]) {
        execFileSync(path.join(output, "cetrun_legacy.exe"), [mode, path.join(output, name + ".exe"), "--gtest_brief=1"],
          { cwd: output, env: childEnv, stdio: "inherit", timeout: 360000 });
      }
    }
    if (available) {
      execFileSync(path.join(output, "cetrun_legacy.exe"),
        ["strict", path.join(output, "inspector_tests_legacy.exe"), "--gtest_brief=1", "--gtest_filter=*CetStepOver*"],
        { cwd: output, env: { ...suiteEnv, CETRUN_REQUIRE_CET: "1", CETRUN_TIMEOUT_MS: "60000",
          V8JSI_TEST_V8_FLAGS: "--allow-natives-syntax --sparkplug" }, stdio: "inherit", timeout: 90000 });
      for (const tier of ["", "--no-sparkplug", "--jitless"]) {
        const env = { ...suiteEnv, CETRUN_REQUIRE_CET: "1", CETRUN_TIMEOUT_MS: "300000",
          V8JSI_TEST_V8_FLAGS: "--allow-natives-syntax " + tier };
        const filter = tier ? ["--gtest_filter=*Interpreted*:*AsyncCatch*:*GeneratorCatch*"] : [];
        execFileSync(path.join(output, "cetrun_legacy.exe"),
          ["strict", path.join(output, "cet_tests_legacy.exe"), "--gtest_brief=1", ...filter],
          { cwd: output, env, stdio: "inherit", timeout: 360000 });
      }
    }
  }
}
if (args.check) {
  // ml64 has no /guard:cf switch. All other missing feature bits fail closed.
  const assembler = new Set(args.platform === "x64" ? ["push_registers_masm.obj"] : []);
  const config = JSON.parse(fs.readFileSync(path.join(nodeDir, "config.gypi"), "utf8").replace(/^#.*$/mg, ""));
  const projects = new Map<string, string>();
  function readProject(file: string): void {
    if (projects.has(file)) return;
    const xml = fs.readFileSync(file, "utf8");
    projects.set(file, xml);
    for (const ref of xml.matchAll(/<ProjectReference Include="([^"]+)"/g)) {
      readProject(path.resolve(path.dirname(file), ref[1]));
    }
  }
  readProject(path.join(nodeDir, "v8jsi_legacy.vcxproj"));
  checkIteratorConfiguration(config.target_defaults.configurations, args.platform, projects);
  const project = fs.readFileSync(path.join(nodeDir, "v8jsi_legacy.vcxproj"), "utf8");
  checkLegacyProject(project);
  checkLegacyHardeningProject(project);
  const manifest = JSON.parse(fs.readFileSync(path.join(repoRoot, "src/legacy/sources.json"), "utf8"));
  checkLegacyIncludes(fs.readFileSync(path.join(output, "obj/v8jsi_legacy/v8jsi_legacy.tlog/Microsoft.Build.CPPTasks.CL.read.1.tlog"), "utf16le"), repoRoot,
    manifest.files.filter((file: { kind: string }) => file.kind === "shared").map((file: { path: string }) => file.path));
  const libraries = linkedArchiveNames(project, (ref) => fs.readFileSync(path.join(nodeDir, ref), "utf8"));
  for (const required of ["v8_snapshot.lib", "v8_libplatform.lib"]) {
    if (!libraries.includes(required)) throw new Error(`Missing linked archive: ${required}`);
  }
  for (const lib of new Set(libraries)) {
    console.log(`CFG ${lib}: ${checkArchiveFileCfg(path.join(output, "lib", lib), assembler)} objects`);
  }
  checkLegacySources(repoRoot);
  const dll = path.join(output, "v8jsi.dll");
  const expectedExports = JSON.parse(fs.readFileSync(path.join(repoRoot, "src/legacy/exports.json"), "utf8"));
  for (const check of legacyBinaryChecks(args.platform)) {
    switch (check) {
      case "exports": checkLegacyExports(dumpbinExportNames(dll), expectedExports, args.platform); break;
      case "pe": checkPeFlags(fs.readFileSync(dll), args.platform !== "x86"); break;
      case "cet": {
        const dumpbin = findDumpbin();
        checkCetHeaders(execFileSync(dumpbin, ["/headers", dll], { encoding: "utf8" }), true, dll, dumpbin);
        break;
      }
    }
  }
  const dumpbin = findDumpbin();
  for (const [name, compatible] of legacyCetArtifacts(args.platform)) {
    const binary = path.join(output, name);
    checkCetHeaders(execFileSync(dumpbin, ["/headers", binary], { encoding: "utf8" }), compatible, binary, dumpbin);
  }
  const escapedDll = dll.replaceAll("'", "''");
  const info = JSON.parse(execFileSync("pwsh", ["-NoProfile", "-Command",
    `(Get-Item '${escapedDll}').VersionInfo | Select-Object OriginalFilename,ProductName,FileVersion | ConvertTo-Json -Compress`], { encoding: "utf8" }));
  const version = JSON.parse(fs.readFileSync(path.join(repoRoot, "config.json"), "utf8")).version;
  checkLegacyIdentity(fs.readFileSync(dll), info, version);
  const imports = crtDependencyLines(dll);
  if (!imports.some(line => /api-ms-win-crt|ucrtbase/i.test(line)) || imports.some(line => /(?:msvcp|vcruntime)\d/i.test(line))) {
    throw new Error("Legacy Hybrid CRT import mismatch");
  }
  // V8 archives retain MD5 records, buffer-less thunks lack GS metadata, and
  // clang-cl cannot emit x86 SafeSEH. Non-x64 has no CET shadow stacks.
  const residuals = new Set(["BA2004", "BA2014", "BA2018"]);
  if (args.platform !== "x64") residuals.add("BA2025");
  const binskim = process.env.BINSKIM_EXE ?? await ensureBinSkimExe(path.join(repoRoot, "out/tools"));
  if (!analyzeBinSkim(binskim, [dll], path.join(output, "v8jsi.binskim.sarif"), "legacy engine", residuals)) {
    throw new Error("Legacy BinSkim gate failed");
  }
  console.log("Legacy engine hardening checks passed");
}

if (args.stage) stageLegacy(output, path.resolve(args.stage), args.platform, args.configuration === "release" ? "Release" : "Debug");
