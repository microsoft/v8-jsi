// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Build only: ARM64EC requires ARM64 Windows to run. The client DLL under test
// is the x64 build loaded by this EC executable, not an EC build of the client.
import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { repoRoot, ensureDir, vswhereExe } from "./build-common.ts";

function newestDirectory(root: string, required: string[]): string {
  const candidates = fs.readdirSync(root).sort((a, b) =>
    b.localeCompare(a, undefined, { numeric: true }));
  const found = candidates.find((name) =>
    required.every((file) => fs.existsSync(path.join(root, name, file))));
  if (!found) throw new Error(`Missing ARM64EC tools/libraries under ${root}: ${required.join(", ")}`);
  return path.join(root, found);
}

function main(): void {
  if (process.argv.includes("--help")) {
    console.log("Usage: node scripts\\cross-arch-build.ts\n" +
      "Builds the client-only ARM64EC qualification harness with MSVC /arm64EC.\n" +
      "Output: deps\\chromium\\out\\cross-arch-arm64ec\\v8host_cross_arch_tests.exe");
    return;
  }
  if (process.argv.length !== 2) throw new Error("Unknown argument (use --help)");
  const vswhere = vswhereExe();
  if (!vswhere) throw new Error("Visual Studio Installer vswhere.exe is missing");
  const vs = execFileSync(vswhere, [
    "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.ARM64",
    "-property", "installationPath",
  ], { encoding: "utf8" }).trim();
  if (!vs) throw new Error("Visual Studio MSVC ARM64/ARM64EC build tools are missing");
  const msvc = newestDirectory(path.join(vs, "VC", "Tools", "MSVC"), [
    "bin\\Hostx64\\arm64\\cl.exe", "include\\vector",
  ]);
  const dumpbin = path.join(msvc, "bin", "Hostx64", "x64", "dumpbin.exe");
  function ecLibrary(root: string, dirs: string[], name: string): string {
    for (const dir of dirs) {
      const file = path.join(root, dir, name);
      if (!fs.existsSync(file)) continue;
      // Recent toolsets put both ARM64 and EC members in the ARM64 archives.
      // Do not silently substitute native ARM64 libraries for missing EC ones.
      const headers = execFileSync(dumpbin, ["/headers", file], {
        encoding: "utf8", maxBuffer: 64 * 1024 * 1024,
      });
      if (/A641 machine \(ARM64EC\)|Machine\s*:\s*A641 \(ARM64EC\)/i.test(headers))
        return path.join(root, dir);
    }
    throw new Error(`Missing ARM64EC library ${name}: checked ${dirs.map((dir) => path.join(root, dir)).join(", ")}`);
  }
  const crt = ecLibrary(path.join(msvc, "lib"), ["arm64ec", "arm64"], "libcmt.lib");
  const kits = path.join(process.env["ProgramFiles(x86)"] ?? "C:\\Program Files (x86)", "Windows Kits", "10");
  const sdkLib = newestDirectory(path.join(kits, "Lib"), [
    "um\\arm64\\kernel32.lib", "um\\arm64\\user32.lib", "um\\arm64\\bcrypt.lib",
    "ucrt\\arm64\\libucrt.lib",
  ]);
  const um = ecLibrary(path.join(sdkLib, "um"), ["arm64ec", "arm64"], "kernel32.lib");
  const ucrt = ecLibrary(path.join(sdkLib, "ucrt"), ["arm64ec", "arm64"], "libucrt.lib");
  const sdkVersion = path.basename(sdkLib);
  const includes = [
    path.join(msvc, "include"),
    ...["ucrt", "shared", "um", "winrt"].map((dir) => path.join(kits, "Include", sdkVersion, dir)),
  ];
  for (const include of includes) {
    if (!fs.existsSync(include)) throw new Error(`Missing SDK include directory: ${include}`);
  }
  const out = path.join(repoRoot, "deps", "chromium", "out", "cross-arch-arm64ec");
  ensureDir(out);
  const tools = path.join(msvc, "bin", "Hostx64", "arm64");
  const env = {
    ...process.env,
    INCLUDE: includes.join(";"),
    LIB: [path.join(msvc, "lib", "arm64ec"), crt, um, ucrt].join(";"),
    PATH: `${tools};${process.env.PATH ?? ""}`,
  };
  const exe = path.join(out, "v8host_cross_arch_tests.exe");
  console.log(`ARM64EC: MSVC=${msvc}\nSDK=${sdkLib}\nOutput=${exe}`);
  execFileSync(path.join(tools, "cl.exe"), [
    "/nologo", "/arm64EC", "/std:c++17", "/MT", "/guard:cf", "/W4", "/WX",
    "/DUNICODE", "/D_UNICODE", "/D_WIN32_WINNT=0x0A00", "/O2",
    `/I${path.join(repoRoot, "src", "public")}`,
    `/I${path.join(repoRoot, "deps", "chromium", "v8host", "tests")}`,
    `/Fo${path.join(out, "v8host_cross_arch_tests.obj")}`, `/Fe${exe}`,
    path.join(repoRoot, "deps", "chromium", "v8host", "tests", "v8host_cross_arch_tests.cc"),
    "/link", "/MACHINE:ARM64EC", "/guard:cf", "user32.lib", "bcrypt.lib",
  ], { env, cwd: repoRoot, stdio: "inherit" });
  if (fs.existsSync(dumpbin)) {
    const headers = execFileSync(dumpbin, ["/headers", "/loadconfig", exe], { encoding: "utf8" });
    console.log(headers.split(/\r?\n/).filter((line) =>
      /machine|ARM64EC|CHPE|Hybrid metadata pointer/i.test(line)).join("\n"));
    if (!/8664 machine/i.test(headers) ||
        !/[0-9A-F]*[1-9A-F][0-9A-F]*\s+(?:CHPE|Hybrid) metadata pointer/i.test(headers))
      throw new Error("ARM64EC output lacks AMD64 machine or nonzero CHPE metadata");
  } else {
    console.log("dumpbin unavailable: PE/CHPE verification not performed");
  }
  console.log("cross-arch-build: PASS (compile/link only; ARM64 hardware required to run)");
}

try { main(); } catch (error) { console.error(error); process.exitCode = 1; }
