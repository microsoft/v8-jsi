// Compile plans for generated tests (design §11): every generated file, for
// every target its library ships, as C or C++ by extension, with clang and
// MSVC, warnings as errors. clang runs freestanding with no system include
// paths, like the header gate; MSVC runs in each target's vcvars environment.

import path from "node:path";
import type { GeneratedFile } from "./generators.ts";
import type { Model } from "./model.ts";
import { StelaError } from "./errors.ts";
import { type CompileJob, type MsvcArch, type Toolchain, resolveOnPath } from "./toolchain.ts";

/** The MSVC host-tools architecture that builds for each target. */
export const TARGET_ARCH: Readonly<Record<string, MsvcArch>> = {
  "x86_64-pc-windows-msvc": "x64",
  "i686-pc-windows-msvc": "x86",
  "aarch64-pc-windows-msvc": "arm64",
};

const CLANG_WARNINGS = ["-Wall", "-Wextra", "-Wpedantic", "-Wdocumentation", "-Werror"];

// Header tests use the conventions' floors (C99, C++11); layout tests need
// C11 for _Static_assert. Both also use the build's default (C17, C++17).
// MSVC's oldest modes are C11 and C++14.
const CLANG_MODES = {
  header: { c: ["c99", "c17"], cpp: ["c++11", "c++17"] },
  layout: { c: ["c11", "c17"], cpp: ["c++11", "c++17"] },
} as const;
const MSVC_MODES = { c: ["/std:c11", "/std:c17"], cpp: ["/std:c++14", "/std:c++17"] } as const;

export interface CompileUnit {
  /** Absolute path of the generated file. */
  readonly file: string;
  /** Absolute include directories of its library. */
  readonly includeDirs: readonly string[];
  /** Target triples of its library. */
  readonly targets: readonly string[];
}

/** Pairs each generated file with its library's include directories and targets. */
export function compileUnits(
  model: Model,
  root: string,
  outDir: string,
  files: readonly GeneratedFile[],
): CompileUnit[] {
  return files.map((f) => {
    const name = f.path.split("/")[0];
    const library = model.libraries.find((l) => l.name === name);
    if (library === undefined) throw new StelaError(`${f.path}: no library ${name} in the model`);
    return {
      file: path.join(outDir, ...f.path.split("/")),
      includeDirs: library.includeDirs.map((d) => path.join(root, ...d.split("/"))),
      targets: library.targets ?? model.targets,
    };
  });
}

export function archOf(target: string): MsvcArch {
  const arch = TARGET_ARCH[target];
  if (arch === undefined) throw new StelaError(`no MSVC architecture is known for target ${target}`);
  return arch;
}

/** Every clang and MSVC job for `units`; `envs` holds each needed architecture's vcvars environment. */
export function compileJobs(
  tc: Toolchain,
  envs: Readonly<Partial<Record<MsvcArch, NodeJS.ProcessEnv>>>,
  units: readonly CompileUnit[],
): CompileJob[] {
  const jobs: CompileJob[] = [];
  for (const unit of units) {
    const lang = unit.file.endsWith(".cpp") ? "cpp" : "c";
    const kind = /\.header\.(c|cpp)$/.test(unit.file) ? "header" : "layout";
    const name = path.basename(unit.file);
    const cwd = path.dirname(unit.file);
    const includes = unit.includeDirs.flatMap((d) => ["-I", d]);
    for (const target of unit.targets) {
      const arch = archOf(target);
      for (const std of CLANG_MODES[kind][lang]) {
        jobs.push({
          label: `${name} clang ${arch} ${std}`,
          exe: tc.clang,
          args: [
            "-fsyntax-only",
            "-ffreestanding",
            "-nostdlibinc",
            `--target=${target}`,
            "-x",
            lang === "c" ? "c" : "c++",
            `-std=${std}`,
            ...CLANG_WARNINGS,
            "-fno-color-diagnostics",
            ...includes,
            unit.file,
          ],
          cwd,
        });
      }
      const env = envs[arch];
      if (env === undefined) throw new StelaError(`no MSVC environment for ${arch}`);
      const cl = resolveOnPath("cl.exe", env);
      for (const std of MSVC_MODES[lang]) {
        const args = ["/nologo", "/Zs", "/W4", "/WX", lang === "c" ? "/TC" : "/TP", std];
        if (lang === "cpp") args.push("/permissive-");
        for (const d of unit.includeDirs) args.push("/I", d);
        args.push(unit.file);
        jobs.push({ label: `${name} cl ${arch} ${std}`, exe: cl, args, cwd, env });
      }
    }
  }
  return jobs;
}

/** The vcvars environment of every architecture `units` need. */
export async function msvcEnvs(
  tc: Toolchain,
  units: readonly CompileUnit[],
): Promise<Partial<Record<MsvcArch, NodeJS.ProcessEnv>>> {
  const envs: Partial<Record<MsvcArch, NodeJS.ProcessEnv>> = {};
  for (const unit of units) {
    for (const target of unit.targets) {
      const arch = archOf(target);
      envs[arch] ??= await tc.msvcEnv(arch);
    }
  }
  return envs;
}
