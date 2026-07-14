// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Build script for the Chromium-sandbox binaries (sbox.dll / v8host.exe /
// test_app.exe). Run with Node.js v24+ (TypeScript strip-types).
// Usage: node scripts/sbox-build.ts [options]
//
// These binaries are built by the Chromium gn/ninja toolchain, NOT vcbuild —
// that is the v8jsi/v8jsisb engine build in build.ts. This script:
//   1. resolves the toolchain: clang + ninja from the installed Visual Studio
//      (via vswhere; requires the "C++ Clang Compiler for Windows" component),
//      and the vendored gn (deps/gn/gn.exe, Git LFS),
//   2. runs `gn gen` (writing args.gn) then `ninja` into a single out dir, and
//   3. enforces the same hygiene gates as v8jsi.dll (export allowlist + Hybrid
//      CRT + BinSkim) over the produced binaries.
//
// Why the VS toolchain: the sandbox sources compile with a stock stable clang
// (plugins off, rust off), so we use the clang+ninja that Visual Studio already
// ships instead of a ~225 MB hand-placed Chromium toolchain. Only gn is vendored
// (VS never ships it).

import { parseArgs } from "node:util";
import { execSync, execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";

import {
  repoRoot,
  ensureDir,
  deleteDir,
  run,
  vswhereExe,
  dumpbinExportNames,
  crtDependencyLines,
  ensureBinSkimExe,
  analyzeBinSkim,
} from "./build-common.ts";

// =============================================================================
// Constants
// =============================================================================

const chromiumDir = path.join(repoRoot, "deps", "chromium");

// Vendored gn (VS never ships it). 2.5 MB, tracked via Git LFS. Override with
// SBOX_GN for a local experiment.
const gnExe =
  process.env.SBOX_GN ?? path.join(repoRoot, "deps", "gn", "gn.exe");

// The space-free junction that stands in for the VS LLVM dir as clang_base_path.
// gn writes the compiler-rt lib path into the ninja link line unquoted, so a
// clang_base_path with spaces (C:\Program Files\Microsoft Visual Studio\...)
// breaks lld-link. build/ is .gitignored, so this generated link is not tracked.
const clangJunction = path.join(repoRoot, "build", ".tools", "vsllvm");

// Supported gn target_cpu values (win). The host clang (VC\Tools\Llvm\x64)
// cross-compiles to all three; target_cpu selects the target ISA.
const supportedTargetCpus = ["x64", "x86", "arm64"] as const;
type TargetCpu = (typeof supportedTargetCpus)[number];

function defaultOutDirFor(cpu: TargetCpu): string {
  return path.join(chromiumDir, "out", `sandbox-${cpu}`);
}

// Toolchain floor. The sandbox sources are proven to build with stock clang 22.
// A clang older than this is rejected with an actionable message.
const clangMajorFloor = 22;

// The VS Installer component id for the C++ Clang compiler.
const clangComponentId = "Microsoft.VisualStudio.Component.VC.Llvm.Clang";

// BinSkim rules accepted as residuals on the sandbox binaries (sbox.dll,
// v8host.exe, test_app.exe). These follow from the gn/ninja (clang-cl +
// lld-link) build configuration for these binaries. The mitigations that this
// toolchain CAN satisfy are enabled at the source (build/config/compiler/BUILD.gn
// + build/toolchain/win/setup_toolchain.py) and are NO LONGER accepted:
//   BA2024 (Spectre)        - CLEARED: /Qspectre + linking the Spectre-mitigated
//                             MSVC CRT (vcvars -vcvars_spectre_libs=spectre).
//                             clang-cl, unlike cl/link, does not auto-link the
//                             Spectre CRT, so both pieces are required.
//   BA2004 error (MD5 hash) - CLEARED on compiled objects via /ZH:SHA_256.
// What remains accepted, with why it is not productizable on this toolchain:
//   BA2004 (warning) - residual SHA/no-hash finding on objects NOT compiled by
//                      clang-cl: the NASM/prebuilt asm objects (e.g. boringssl)
//                      carry no source hash and cannot be given one. The
//                      error-level MD5 finding is cleared (above).
//   BA6004 / BA6006  - Link-Time Code Generation (/LTCG). LTCG is MSVC
//                      link.exe metadata; lld-link does not emit it and clang
//                      ThinLTO is not recognized by BinSkim's check. Clearing
//                      it would require switching the sandbox link to MSVC
//                      link.exe -- a large toolchain change, tracked separately.
//   BA2027           - PDBs lack SourceLink (a debugging-ergonomics item, not a
//                      runtime mitigation; orthogonal to this build).
//   BA2025           - CET Shadow Stack. Chromium enables /CETCOMPAT on the x64
//                      link (so x64 clears this), but not for the x86 target, and
//                      32-bit CET shadow stack is rarely deployed. Accepted on x86
//                      -- the same residual v8jsi.dll accepts -- since the OS
//                      lockdown, not CET, is the sandbox's primary defense; x64 is
//                      unaffected. Revisit if x86 /CETCOMPAT is productized.
const acceptedSandboxBinSkimRules = new Set([
  "BA2004",
  "BA2025",
  "BA2027",
  "BA6004",
  "BA6006",
]);

// =============================================================================
// CLI Parsing
// =============================================================================

const options = {
  help: { type: "boolean" as const, default: false },
  gen: { type: "boolean" as const, default: true },
  build: { type: "boolean" as const, default: true },
  check: { type: "boolean" as const, default: false },
  clean: { type: "boolean" as const, default: false },
  target: { type: "string" as const, default: "generic" },
  "target-cpu": { type: "string" as const, default: "x64" },
  "out-dir": { type: "string" as const },
  "output-path": { type: "string" as const, default: "./out" },
};

const { values: args } = parseArgs({ options, allowNegative: true });

function showHelp(): void {
  console.log(`
v8-jsi sandbox build script (Chromium gn/ninja toolchain)

Usage: node scripts/sbox-build.ts [options]

Boolean flags (all support --no- prefix):
  --help          Show this help message
  --gen           Run 'gn gen' (writes args.gn) before building (default: true)
  --build         Run 'ninja' on the target (default: true)
  --check         Enforce export-allowlist + Hybrid-CRT + BinSkim gates on the
                  produced binaries (sbox.dll, v8host.exe, test_app.exe)
  --clean         Delete the out dir before generating

String arguments:
  --target <t>    ninja target to build (default: generic)
  --target-cpu <cpu>
                  gn target_cpu: x64 | x86 | arm64 (default: x64). The host
                  clang cross-compiles all three targets.
  --out-dir <dir> gn/ninja output dir (default: deps/chromium/out/sandbox-<cpu>)
  --output-path <dir>
                  Tools dir parent for BinSkim/nuget (default: ./out)

Toolchain:
  clang + ninja come from the installed Visual Studio (resolved via vswhere;
  requires the "C++ Clang Compiler for Windows" component, clang >= ${clangMajorFloor}).
  gn is vendored at deps/gn/gn.exe (Git LFS); override with SBOX_GN.

Examples:
  node scripts/sbox-build.ts                  # gen + build into out/sandbox-x64
  node scripts/sbox-build.ts --no-gen         # rebuild without re-running gn gen
  node scripts/sbox-build.ts --no-gen --no-build --check   # gates only
  node scripts/sbox-build.ts --clean          # clean, then gen + build
`);
}

// =============================================================================
// Toolchain resolution
// =============================================================================

interface VsToolchain {
  vsPath: string;
  clangBasePath: string; // <vs>\VC\Tools\Llvm\x64
  clangMajor: number;
  ninja: string;
}

// Print an actionable remediation message and exit. Used for every "the dev's
// machine is missing a required piece" case so the failure is self-explanatory.
function floor(message: string): never {
  console.error(`\nsbox-build: ${message}\n`);
  process.exit(1);
}

function firstLine(s: string): string {
  return s.split(/\r?\n/).find((l) => l.trim().length > 0)?.trim() ?? "";
}

// Resolve clang + ninja from the installed Visual Studio. Enforces the toolchain
// floor (VS present, Clang component installed, clang >= clangMajorFloor, the
// matching compiler-rt libs and VS ninja present), failing with a clear message.
function resolveVsToolchain(targetCpu: TargetCpu): VsToolchain {
  const vswhere = vswhereExe();
  if (!vswhere) {
    floor(
      "vswhere.exe not found. Install Visual Studio 2022 or newer (the C++ " +
        "workload), which provides vswhere under Program Files (x86).",
    );
  }

  let vsPath = "";
  try {
    vsPath = firstLine(
      execSync(
        `"${vswhere}" -prerelease -latest -products * ` +
          `-requires ${clangComponentId} -property installationPath`,
        { encoding: "utf-8" },
      ),
    );
  } catch {
    // fall through to the not-found message below
  }
  if (!vsPath || !fs.existsSync(vsPath)) {
    floor(
      "No Visual Studio install with the C++ Clang component was found. In the " +
        'Visual Studio Installer, add "C++ Clang Compiler for Windows" and ' +
        '"MSBuild support for the LLVM (clang-cl) toolset" (components ' +
        `${clangComponentId} + ...VC.Llvm.ClangToolset).`,
    );
  }

  // VS ships a SEPARATE LLVM toolchain per host arch (VC\Tools\Llvm\x64 and
  // \ARM64). clang runs as the host binary, so the base path follows the HOST
  // arch; target_cpu selects what it emits (the host clang cross-compiles all
  // targets — see the compiler-rt note below).
  const hostArchDir = process.arch === "arm64" ? "ARM64" : "x64";
  const clangBasePath = path.join(vsPath, "VC", "Tools", "Llvm", hostArchDir);
  const clangCl = path.join(clangBasePath, "bin", "clang-cl.exe");
  const clangExe = path.join(clangBasePath, "bin", "clang.exe");
  if (!fs.existsSync(clangCl) || !fs.existsSync(clangExe)) {
    floor(
      `clang not found under ${clangBasePath}\\bin. Repair the VS Clang ` +
        "component (C++ Clang Compiler for Windows).",
    );
  }

  let clangMajor = 0;
  try {
    const verOut = execSync(`"${clangExe}" --version`, { encoding: "utf-8" });
    const m = /clang version (\d+)\./.exec(verOut);
    if (m) clangMajor = Number(m[1]);
  } catch {
    // handled below
  }
  if (!clangMajor) {
    floor(`Could not determine the clang version from ${clangExe}.`);
  }
  if (clangMajor < clangMajorFloor) {
    floor(
      `clang ${clangMajor} is older than the required minimum ${clangMajorFloor}. ` +
        "Update Visual Studio (the C++ Clang component).",
    );
  }

  // NOTE: we deliberately do NOT require clang's compiler-rt builtins
  // (clang_rt.builtins-<arch>.lib). A fork edit (build/config/compiler/BUILD.gn,
  // tagged "v8-jsi:") drops the compiler-rt linkage on Windows so the build uses
  // the MSVC vcruntime intrinsics (like the Node/v8jsi clang-cl build) instead.
  // VS bundles compiler-rt per host arch only (x64 -> x86_64, ARM64 -> aarch64;
  // no i386 anywhere), so requiring it would block cross builds (x86's i386 lib
  // is not shipped by VS; arm64's aarch64 lib lives only in the ARM64 toolchain).
  // Dropping it means target_cpu just selects what clang-cl emits; no extra lib.

  const ninja = path.join(
    vsPath,
    "Common7",
    "IDE",
    "CommonExtensions",
    "Microsoft",
    "CMake",
    "Ninja",
    "ninja.exe",
  );
  if (!fs.existsSync(ninja)) {
    floor(
      `VS ninja not found at ${ninja}. Install the "C++ CMake tools for Windows" ` +
        "component, which provides ninja.",
    );
  }

  // Spectre-mitigated MSVC CRT libs. The Windows build links the Spectre-hardened
  // CRT (build/toolchain/win/setup_toolchain.py, tagged "v8-jsi:") so the binaries
  // clear BinSkim BA2024. Those libs ship ONLY with the "C++ Spectre-mitigated
  // libs" VS component; without it lld-link silently falls back to the unmitigated
  // CRT — the build still succeeds but BinSkim later fails on BA2024 with a
  // cryptic message. Fail fast here instead. (gn target_cpu x86|x64|arm64 maps
  // directly to the lib\spectre\<arch> subdir.)
  const msvcRoot = path.join(vsPath, "VC", "Tools", "MSVC");
  const hasSpectreLibs =
    fs.existsSync(msvcRoot) &&
    fs
      .readdirSync(msvcRoot)
      .some((ver) =>
        fs.existsSync(
          path.join(msvcRoot, ver, "lib", "spectre", targetCpu, "libcmt.lib"),
        ),
      );
  if (!hasSpectreLibs) {
    const component =
      targetCpu === "arm64"
        ? "Microsoft.VisualStudio.Component.VC.Runtimes.ARM64.Spectre"
        : "Microsoft.VisualStudio.Component.VC.Runtimes.x86.x64.Spectre";
    floor(
      `Spectre-mitigated CRT libs for ${targetCpu} not found under ` +
        `${msvcRoot}\\*\\lib\\spectre\\${targetCpu}. Install the ` +
        `"C++ Spectre-mitigated libs (Latest)" VS component (${component}). ` +
        "Without it the build links the unmitigated CRT and BinSkim fails BA2024.",
    );
  }

  return { vsPath, clangBasePath, clangMajor, ninja };
}

// Create (or refresh) the space-free junction that stands in for the VS LLVM dir
// as clang_base_path, and return its path. A junction (not a symlink) needs no
// admin rights. If a stale junction points elsewhere, it is replaced.
function ensureClangJunction(clangBasePath: string): string {
  ensureDir(path.dirname(clangJunction));
  const wantReal = fs.realpathSync(clangBasePath);
  if (fs.existsSync(clangJunction)) {
    let current = "";
    try {
      current = fs.realpathSync(clangJunction);
    } catch {
      current = "";
    }
    if (current === wantReal) return clangJunction;
    // Stale — remove just the reparse point. rmdir on a junction removes the
    // link without touching the target's contents.
    try {
      fs.rmdirSync(clangJunction);
    } catch {
      fs.unlinkSync(clangJunction);
    }
  }
  fs.symlinkSync(clangBasePath, clangJunction, "junction");
  return clangJunction;
}

// gn wants forward slashes in path-valued args, even on Windows.
function toGnPath(p: string): string {
  return p.replace(/\\/g, "/");
}

// =============================================================================
// gn gen / ninja
// =============================================================================

// The gn args for the sandbox build: stock clang (plugins off), no rust, no
// sysroot, no remote exec, the produced binaries V8-agnostic. clang_base_path is
// the space-free junction; clang_version pins the compiler-rt dir to the
// resolved clang major.
function gnArgsContent(
  clangBasePathForGn: string,
  clangMajor: number,
  targetCpu: TargetCpu,
): string {
  return (
    [
      'target_os = "win"',
      `target_cpu = "${targetCpu}"`,
      "is_debug = false",
      "is_component_build = false",
      "is_clang = true",
      `clang_base_path = "${toGnPath(clangBasePathForGn)}"`,
      `clang_version = "${clangMajor}"`,
      "clang_use_chrome_plugins = false",
      "use_sysroot = false",
      "use_remoteexec = false",
      "symbol_level = 1",
      "treat_warnings_as_errors = false",
      "enable_rust = false",
      // Drop the JS protobuf generator (protoc-gen-js). base's tracing protos
      // only need C++ generation, but Chromium's protoc target pulls in the JS
      // plugin whose sources live in a separate DEPS checkout we don't vendor.
      // Disabling it cuts protoc -> protoc-gen-js so the build needs no JS gen.
      "enable_js_protobuf = false",
    ].join("\n") + "\n"
  );
}

function sandboxEnv(): NodeJS.ProcessEnv {
  // The Chromium build keys off this to use the locally-resolved MSVC/SDK rather
  // than trying to download a Google-internal toolchain package.
  return { ...process.env, DEPOT_TOOLS_WIN_TOOLCHAIN: "0" };
}

// gclient's `runhooks` normally generates these build inputs. This vendored tree
// builds WITHOUT gclient, so they are not committed (gitignored upstream) and we
// synthesize them here at gen time. Each is written only if absent, so a real
// gclient checkout's copies are left untouched. The gclient_args flags below are
// the ones gn imports for this Windows configuration; if a future Chromium bump
// needs a new flag, gn gen will error and this list is where to add it.
const GCLIENT_ARGS_GNI =
  [
    'android_ndk_version = "2@30.0.14608247"',
    "build_with_chromium = true",
    "checkout_android = false",
    "checkout_android_prebuilts_build_tools = false",
    "checkout_clang_coverage_tools = false",
    "checkout_copybara = false",
    "checkout_glic_e2e_tests = false",
    "checkout_ios_webkit = false",
    "checkout_mutter = false",
    "checkout_openxr = false",
    "checkout_src_internal = false",
    "checkout_src_internal_infra = false",
    "checkout_clusterfuzz_data = false",
    'cros_boards = ""',
    'cros_boards_with_qemu_images = ""',
    "generate_location_tags = true",
  ].join("\n") + "\n";

function ensureGeneratedBuildFiles(): void {
  const writeIfAbsent = (p: string, content: string): void => {
    if (fs.existsSync(p)) return;
    ensureDir(path.dirname(p));
    fs.writeFileSync(p, content);
    console.log(`  generated ${path.relative(repoRoot, p).replace(/\\/g, "/")}`);
  };

  writeIfAbsent(
    path.join(chromiumDir, "build", "config", "gclient_args.gni"),
    GCLIENT_ARGS_GNI,
  );

  // LASTCHANGE / committime: version-stamp inputs the build reads. Derive them
  // from the repo HEAD so the stamp is meaningful; fall back to a deterministic
  // placeholder when git isn't available.
  const utilDir = path.join(chromiumDir, "build", "util");
  const lastchange = path.join(utilDir, "LASTCHANGE");
  const committime = path.join(utilDir, "LASTCHANGE.committime");
  if (!fs.existsSync(lastchange) || !fs.existsSync(committime)) {
    let hash = "0".repeat(40);
    let ctime = "0";
    try {
      hash = execSync("git rev-parse HEAD", {
        cwd: repoRoot,
        encoding: "utf-8",
      }).trim();
      ctime = execSync("git show -s --format=%ct HEAD", {
        cwd: repoRoot,
        encoding: "utf-8",
      }).trim();
    } catch {
      /* not a git checkout — keep the placeholder */
    }
    writeIfAbsent(lastchange, `LASTCHANGE=${hash}\n`);
    writeIfAbsent(committime, `${ctime}\n`);
  }
}

function gnGen(
  outDir: string,
  clangBasePathForGn: string,
  clangMajor: number,
  targetCpu: TargetCpu,
): void {
  console.log(`\n=== gn gen (${outDir}, target_cpu=${targetCpu}) ===\n`);
  ensureGeneratedBuildFiles();
  ensureDir(outDir);
  // Write args.gn directly instead of passing --args= through the shell: it
  // avoids quoting the embedded double-quotes, and the file is inspectable.
  fs.writeFileSync(
    path.join(outDir, "args.gn"),
    gnArgsContent(clangBasePathForGn, clangMajor, targetCpu),
  );
  run(
    `"${gnExe}"`,
    ["gen", `"${outDir}"`, `--root="${chromiumDir}"`],
    { env: sandboxEnv() },
  );
}

function ninjaBuild(ninja: string, outDir: string, target: string): void {
  console.log(`\n=== ninja ${target} (${outDir}) ===\n`);
  run(`"${ninja}"`, ["-C", `"${outDir}"`, target], { env: sandboxEnv() });
}

// Generate src/version_info_gen.h (the version numbers src/version_info.rc
// #includes for sbox.dll / v8host.exe). The gyp engine build regenerates the
// same header via its v8jsi_version_gen action; both keep it fresh from
// config.json.v8jsi_version. The header is git-ignored.
function generateVersionInfo(): void {
  const script = path.join(repoRoot, "src", "generate_version_info.ts");
  console.log("\n=== version info (src/version_info_gen.h) ===\n");
  execSync(`node "${script}"`, { stdio: "inherit", cwd: repoRoot });
}

// Compile src/version_info.rc -> <outDir>/version_info_<name>.res for sbox.dll
// and v8host.exe, which link the prebuilt .res via ldflags. GN's own rc tool
// (rc.py) needs the hermetic rc binary + hermetic clang, neither of which the
// pruned VS-clang sandbox tree carries — so we use the Windows SDK rc.exe
// directly, recovered from the toolchain env block that gn gen writes
// (environment.<cpu>: NUL-separated KEY=VALUE, holds INCLUDE for verrsrc.h and
// the SDK bin dir with rc.exe). VER_BINARY selects each binary's strings.
function compileVersionResources(outDir: string, targetCpu: TargetCpu): void {
  console.log("\n=== version resources (sbox.dll / v8host.exe) ===\n");
  const envFile = path.join(outDir, `environment.${targetCpu}`);
  if (!fs.existsSync(envFile)) {
    floor(`toolchain env block not found at ${envFile} (run 'gn gen' first).`);
  }
  const raw = fs.readFileSync(envFile, "latin1");
  const env: NodeJS.ProcessEnv = {};
  for (const entry of raw.split("\0")) {
    const eq = entry.indexOf("=");
    if (eq > 0) env[entry.slice(0, eq)] = entry.slice(eq + 1);
  }
  const pathVar = env.PATH ?? env.Path ?? "";
  let rcExe = "";
  for (const dir of pathVar.split(";")) {
    const cand = dir && path.join(dir, "rc.exe");
    if (cand && fs.existsSync(cand)) {
      rcExe = cand;
      break;
    }
  }
  if (!rcExe) {
    floor("rc.exe not found in the toolchain PATH (Windows SDK bin).");
  }

  const rcSource = path.join(repoRoot, "src", "version_info.rc");
  for (const b of [
    { name: "sbox", verBinary: 4 },
    { name: "v8host", verBinary: 5 },
  ]) {
    const res = path.join(outDir, `version_info_${b.name}.res`);
    console.log(`  rc ${b.name} (VER_BINARY=${b.verBinary}) -> ${path.basename(res)}`);
    execFileSync(
      rcExe,
      ["/nologo", `/dVER_BINARY=${b.verBinary}`, `/fo${res}`, rcSource],
      { env, stdio: "inherit" },
    );
  }
}

// =============================================================================
// Sandbox binary hygiene gates
// =============================================================================
//
// The same hygiene bar as v8jsi.dll: an export allowlist, the Hybrid CRT
// contract, and BinSkim. Parameterized over a binary table so further binaries
// can be added with one entry.

interface SandboxBinary {
  file: string;
  // Every exported symbol name must start with one of these prefixes. An empty
  // array means the binary must export nothing.
  allowedExportPrefixes: string[];
  // Optional size ceiling (bytes). Footprint budget — guards against silent
  // bloat. Only binaries with a budget are size-checked; omit to skip.
  maxBytes?: number;
}

// The sandbox binaries and their allowed export surfaces.
//   sbox.dll      — the sandbox C ABI: only sbox_*.
//   v8host.exe    — exports only g_sbox_bootstrap, the struct the broker locates
//                   and populates in the suspended child.
//   test_app.exe  — exports nothing.
const sandboxBinaries: SandboxBinary[] = [
  // sbox.dll budget: <= 2 MiB (measured 1.74 MiB; floor at the Hybrid CRT).
  { file: "sbox.dll", allowedExportPrefixes: ["sbox_"], maxBytes: 2 * 1024 * 1024 },
  { file: "v8host.exe", allowedExportPrefixes: ["g_sbox_bootstrap"] },
  { file: "test_app.exe", allowedExportPrefixes: [] },
];

// Gate 1: export allowlist. Every export of every sandbox binary must match its
// allowed prefixes. Returns true if all binaries pass.
function checkSandboxExports(outDir: string): boolean {
  console.log("\n=== Sandbox export allowlist ===\n");
  let ok = true;
  for (const bin of sandboxBinaries) {
    const filePath = path.join(outDir, bin.file);
    if (!fs.existsSync(filePath)) {
      console.error(`  MISSING: ${filePath}`);
      ok = false;
      continue;
    }
    const names = dumpbinExportNames(filePath);
    const stray = names.filter(
      (n) => !bin.allowedExportPrefixes.some((p) => n.startsWith(p)),
    );
    const allowed =
      bin.allowedExportPrefixes.length === 0
        ? "(none)"
        : bin.allowedExportPrefixes.map((p) => `${p}*`).join(", ");
    if (stray.length === 0) {
      console.log(
        `  OK   ${bin.file}: ${names.length} export(s), all match [${allowed}]`,
      );
    } else {
      ok = false;
      console.error(
        `  FAIL ${bin.file}: ${stray.length} disallowed export(s) ` +
          `(allowed: [${allowed}]): ${stray.join(", ")}`,
      );
    }
  }
  if (!ok) {
    console.error(
      "\nSandbox export allowlist FAILED. If a C++ symbol leaked from the " +
        "statically linked base/sandbox libraries, drop its __declspec(dllexport) " +
        "at the source so the export table is exactly the intended C ABI.",
    );
  }
  return ok;
}

// Gate 2: Hybrid CRT. No sandbox binary may import vcruntime140 / msvcp140;
// only the UCRT may be shared. Returns true if all binaries pass.
function checkSandboxCrt(outDir: string): boolean {
  console.log("\n=== Sandbox CRT dependencies (Hybrid CRT) ===\n");
  let ok = true;
  for (const bin of sandboxBinaries) {
    const filePath = path.join(outDir, bin.file);
    if (!fs.existsSync(filePath)) {
      console.error(`  MISSING: ${filePath}`);
      ok = false;
      continue;
    }
    const crtLines = crtDependencyLines(filePath);
    const violations = crtLines.filter((l) => /vcruntime|msvcp/i.test(l));
    if (violations.length === 0) {
      console.log(`  OK   ${bin.file}: only the UCRT is shared`);
    } else {
      ok = false;
      console.error(
        `  FAIL ${bin.file}: non-UCRT shared runtime: ${violations.join(", ")}`,
      );
    }
  }
  if (!ok) {
    console.error("\nSandbox Hybrid CRT contract FAILED.");
  }
  return ok;
}

// Gate 4: binary-size ceiling (footprint budget). Only binaries with a
// declared maxBytes are checked. Returns true if all pass.
function checkSandboxSize(outDir: string): boolean {
  console.log("\n=== Sandbox binary size budgets ===\n");
  let ok = true;
  for (const bin of sandboxBinaries) {
    if (bin.maxBytes === undefined) continue;
    const filePath = path.join(outDir, bin.file);
    if (!fs.existsSync(filePath)) {
      console.error(`  MISSING: ${filePath}`);
      ok = false;
      continue;
    }
    const size = fs.statSync(filePath).size;
    const mib = (n: number) => (n / (1024 * 1024)).toFixed(2);
    if (size <= bin.maxBytes) {
      console.log(
        `  OK   ${bin.file}: ${mib(size)} MiB (ceiling ${mib(bin.maxBytes)} MiB)`,
      );
    } else {
      ok = false;
      console.error(
        `  FAIL ${bin.file}: ${mib(size)} MiB exceeds ceiling ${mib(bin.maxBytes)} MiB`,
      );
    }
  }
  if (!ok) {
    console.error(
      "\nSandbox size budget FAILED. Investigate the growth before raising the " +
        "ceiling (re-baseline only on a deliberate, justified increase).",
    );
  }
  return ok;
}

// Gate 3: BinSkim over the sandbox binaries (their own accepted-residuals set).
async function runSandboxBinSkim(
  outDir: string,
  toolsPath: string,
): Promise<boolean> {
  console.log("\n=== Sandbox BinSkim ===\n");
  const files = sandboxBinaries
    .map((b) => path.join(outDir, b.file))
    .filter((f) => fs.existsSync(f));
  if (files.length === 0) {
    console.error("No sandbox binaries found to scan");
    return false;
  }
  const binskimExe = await ensureBinSkimExe(toolsPath);
  return analyzeBinSkim(
    binskimExe,
    files,
    path.join(outDir, "sandbox.binskim.sarif"),
    "sandbox",
    acceptedSandboxBinSkimRules,
  );
}

// Orchestrate all three sandbox gates and exit non-zero if any fail. BinSkim
// needs its package feed reachable to install (set BINSKIM_NUGET_SOURCE to point
// at an available source); a reachable run that finds unaccepted issues fails
// the gate.
async function runSandboxGates(outDir: string, toolsPath: string): Promise<void> {
  console.log(`\n=== Sandbox hygiene gates (${outDir}) ===`);
  const exportsOk = checkSandboxExports(outDir);
  const crtOk = checkSandboxCrt(outDir);
  const sizeOk = checkSandboxSize(outDir);
  const binskimOk = await runSandboxBinSkim(outDir, toolsPath);

  console.log("\n=== Sandbox gate summary ===");
  console.log(`  export allowlist: ${exportsOk ? "PASS" : "FAIL"}`);
  console.log(`  hybrid CRT:       ${crtOk ? "PASS" : "FAIL"}`);
  console.log(`  size budget:      ${sizeOk ? "PASS" : "FAIL"}`);
  console.log(`  binskim:          ${binskimOk ? "PASS" : "FAIL"}`);
  if (!exportsOk || !crtOk || !sizeOk || !binskimOk) {
    process.exit(1);
  }
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

  const targetCpu = (args["target-cpu"] ?? "x64") as TargetCpu;
  if (!supportedTargetCpus.includes(targetCpu)) {
    floor(
      `--target-cpu must be one of ${supportedTargetCpus.join(" | ")} ` +
        `(got "${targetCpu}").`,
    );
  }
  const outDir = path.resolve(args["out-dir"] ?? defaultOutDirFor(targetCpu));
  const outputPath = path.resolve(args["output-path"] ?? "./out");
  const toolsPath = path.join(outputPath, "tools");

  if (args.clean) {
    console.log(`Cleaning sandbox out dir: ${outDir}`);
    deleteDir(outDir);
  }

  // Resolve the toolchain only when we need it (gen or build); the gates run on
  // already-built binaries and use only dumpbin/BinSkim.
  if (args.gen || args.build) {
    const tc = resolveVsToolchain(targetCpu);
    console.log(
      `\nToolchain: clang ${tc.clangMajor} (${tc.clangBasePath})\n` +
        `           ninja  (${tc.ninja})\n` +
        `           gn     (${gnExe})\n` +
        `           target_cpu=${targetCpu} -> ${outDir}`,
    );
    if (!fs.existsSync(gnExe)) {
      floor(
        `vendored gn not found at ${gnExe}. It is tracked via Git LFS — run ` +
          "`git lfs pull` (or set SBOX_GN to a gn.exe).",
      );
    }

    // version_info.rc #includes this generated header; produce it before the
    // SDK rc.exe compiles the .res below.
    generateVersionInfo();
    if (args.gen) {
      const junction = ensureClangJunction(tc.clangBasePath);
      gnGen(outDir, junction, tc.clangMajor, targetCpu);
    }
    if (args.build) {
      // Compile the VERSIONINFO .res (sbox.dll / v8host.exe link it) after gn
      // gen has written the toolchain env block, before ninja links.
      compileVersionResources(outDir, targetCpu);
      ninjaBuild(tc.ninja, outDir, args.target ?? "generic");
    }
  }

  if (args.check) {
    await runSandboxGates(outDir, toolsPath);
  }

  const elapsed = Date.now() - startTime;
  const seconds = Math.floor(elapsed / 1000);
  const minutes = Math.floor(seconds / 60);
  console.log(`\nsbox-build took ${minutes}m ${seconds % 60}s`);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
