// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import { parseArgs } from "node:util";
import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { repoRoot, run, ensureSupportedPython, ensureNasm, nasmRequiredForTarget,
         prependToPath, findDumpbin, checkCetHeaders, dumpbinExportNames } from "./build-common.ts";
import { checkArchiveFileCfg, checkPeFlags, checkProbeExports, checkIteratorConfiguration, linkedArchiveNames, legacyBinaryChecks } from "./legacy-checks.ts";

const { values: args } = parseArgs({ allowNegative: true, options: {
  platform: { type: "string", default: "x64" },
  configuration: { type: "string", default: "release" },
  build: { type: "boolean", default: true },
  test: { type: "boolean", default: false },
  check: { type: "boolean", default: false },
  help: { type: "boolean", default: false },
} });
if (args.help) {
  console.log("Usage: node scripts/legacy-build.ts --platform x64|x86|arm64 --configuration release|debug [--no-build] [--test] [--check]");
  process.exit(0);
}
if (process.platform !== "win32" || !["x64", "x86", "arm64"].includes(args.platform) ||
    !["release", "debug"].includes(args.configuration)) throw new Error("Invalid Windows build configuration");
const nodeDir = path.join(repoRoot, "deps/nodejs");
const output = path.join(nodeDir, "out", args.configuration === "release" ? "Release" : "Debug");
if (args.build) {
  const started = Date.now();
  const env = { ...process.env };
  ensureSupportedPython(env);
  if (nasmRequiredForTarget(args.platform)) prependToPath(env, ensureNasm(env));
  fs.mkdirSync(path.join(repoRoot, "out", "legacy"), { recursive: true });
  run("cmd.exe", ["/c", ".\\vcbuild.bat", args.configuration, args.platform, "v8jsilegacy", "cfg", "without-intl",
                 "no-node", "no-cctest", "no-embedtest", "no-openssl-cli", "no-fuzzers", "no-nop", "no-overlapped-checker"],
      { cwd: nodeDir, env });
  const seconds = Math.floor((Date.now() - started) / 1000);
  console.log(`Build took ${Math.floor(seconds / 60)}m ${seconds % 60}s`);
}
if (args.test || args.check) {
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
    throw new Error("Probe must run on a native host (x86 also supports x64 WOW64)");
  }
  run(`"${path.join(output, "v8jsi_legacy_probe_runner.exe")}"`, [], { cwd: output });
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
  readProject(path.join(nodeDir, "v8jsi_legacy_probe_runner.vcxproj"));
  checkIteratorConfiguration(config.target_defaults.configurations, args.platform, projects);
  const project = fs.readFileSync(path.join(nodeDir, "v8jsi_legacy_probe.vcxproj"), "utf8");
  const libraries = linkedArchiveNames(project, (ref) => fs.readFileSync(path.join(nodeDir, ref), "utf8"));
  for (const required of ["v8_snapshot.lib", "v8_libplatform.lib"]) {
    if (!libraries.includes(required)) throw new Error(`Missing linked archive: ${required}`);
  }
  for (const lib of new Set(libraries)) {
    console.log(`CFG ${lib}: ${checkArchiveFileCfg(path.join(output, "lib", lib), assembler)} objects`);
  }
  const dll = path.join(output, "v8jsi_legacy_probe.dll");
  for (const check of legacyBinaryChecks(args.platform)) {
    switch (check) {
      case "exports": checkProbeExports(dumpbinExportNames(dll), args.platform); break;
      case "pe": checkPeFlags(fs.readFileSync(dll), args.platform !== "x86"); break;
      case "cet": {
        const dumpbin = findDumpbin();
        checkCetHeaders(execFileSync(dumpbin, ["/headers", dll], { encoding: "utf8" }), true, dll, dumpbin);
        break;
      }
    }
  }
  console.log("Legacy engine hardening checks passed");
}
