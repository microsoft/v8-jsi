// Locates the Visual Studio C/C++ toolchain for compile checks (Stela's
// compile matrix and the repo's public-header gate): MSVC through vcvarsall
// (one cached environment per target arch) and the clang that ships with VS
// (component Microsoft.VisualStudio.Component.VC.Llvm.Clang).

import { execFile } from "node:child_process";
import { existsSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { promisify } from "node:util";

const execFileAsync = promisify(execFile);

export type MsvcArch = "x64" | "x86" | "arm64";

export interface Toolchain {
  readonly vsPath: string;
  readonly clang: string;
  msvcEnv(arch: MsvcArch): Promise<NodeJS.ProcessEnv>;
}

/** Returns the toolchain, or a reason why it isn't available. */
export async function findToolchain(): Promise<Toolchain | string> {
  if (process.platform !== "win32") return "the compile matrix runs on Windows hosts";
  const vswhere = path.join(
    process.env["ProgramFiles(x86)"] ?? "C:\\Program Files (x86)",
    "Microsoft Visual Studio",
    "Installer",
    "vswhere.exe",
  );
  if (!existsSync(vswhere)) return `vswhere not found at ${vswhere}`;
  const { stdout } = await execFileAsync(vswhere, [
    "-latest",
    "-products",
    "*",
    "-requires",
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "-property",
    "installationPath",
  ]);
  const vsPath = stdout.trim().split(/\r?\n/)[0];
  if (!vsPath) return "no Visual Studio with the C++ tools found";
  const hostDir = process.arch === "arm64" ? "ARM64" : "x64";
  const clang = path.join(vsPath, "VC", "Tools", "Llvm", hostDir, "bin", "clang.exe");
  if (!existsSync(clang)) return `VS clang not found at ${clang}`;
  const vcvarsall = path.join(vsPath, "VC", "Auxiliary", "Build", "vcvarsall.bat");
  if (!existsSync(vcvarsall)) return `vcvarsall.bat not found at ${vcvarsall}`;

  const hostPrefix = process.arch === "arm64" ? "arm64" : "x64";
  const vcvarsArg: Record<MsvcArch, string> = {
    x64: hostPrefix === "x64" ? "x64" : "arm64_x64",
    x86: `${hostPrefix}_x86`,
    arm64: hostPrefix === "arm64" ? "arm64" : "x64_arm64",
  };
  const envs = new Map<MsvcArch, Promise<NodeJS.ProcessEnv>>();
  const loadEnv = async (arch: MsvcArch): Promise<NodeJS.ProcessEnv> => {
    const { stdout: set } = await execFileAsync(
      process.env.ComSpec ?? "cmd.exe",
      ["/d", "/s", "/c", `""${vcvarsall}" ${vcvarsArg[arch]} >nul && set"`],
      { windowsVerbatimArguments: true, maxBuffer: 16 * 1024 * 1024 },
    );
    const env: NodeJS.ProcessEnv = {};
    for (const line of set.split(/\r?\n/)) {
      const eq = line.indexOf("=", 1);
      if (eq > 0) env[line.slice(0, eq)] = line.slice(eq + 1);
    }
    if (!env.INCLUDE) throw new Error(`vcvarsall ${vcvarsArg[arch]} did not set INCLUDE`);
    return env;
  };
  return {
    vsPath,
    clang,
    msvcEnv(arch) {
      let env = envs.get(arch);
      if (!env) {
        env = loadEnv(arch);
        envs.set(arch, env);
      }
      return env;
    },
  };
}

export interface CompileJob {
  readonly label: string;
  readonly exe: string;
  readonly args: readonly string[];
  readonly cwd: string;
  readonly env?: NodeJS.ProcessEnv;
}

export interface CompileResult {
  readonly job: CompileJob;
  readonly ok: boolean;
  readonly output: string;
}

/** Resolves an executable such as `cl.exe` on a vcvars environment's PATH. */
export function resolveOnPath(exe: string, env: NodeJS.ProcessEnv): string {
  const pathVar = env.Path ?? env.PATH ?? "";
  for (const d of pathVar.split(";")) {
    if (d && existsSync(path.join(d, exe))) return path.join(d, exe);
  }
  throw new Error(`${exe} not found on the vcvars PATH`);
}

/** Runs jobs in parallel; a job passes when the compiler exits with 0. */
export async function runJobs(jobs: readonly CompileJob[]): Promise<CompileResult[]> {
  const results: CompileResult[] = new Array(jobs.length);
  let next = 0;
  const worker = async () => {
    while (next < jobs.length) {
      const index = next++;
      const job = jobs[index];
      try {
        const { stdout, stderr } = await execFileAsync(job.exe, job.args as string[], {
          cwd: job.cwd,
          env: job.env,
          maxBuffer: 64 * 1024 * 1024,
        });
        results[index] = { job, ok: true, output: stdout + stderr };
      } catch (e) {
        const err = e as { stdout?: string; stderr?: string; message: string };
        results[index] = { job, ok: false, output: `${err.stdout ?? ""}${err.stderr ?? ""}` || err.message };
      }
    }
  };
  await Promise.all(Array.from({ length: Math.max(1, os.availableParallelism()) }, worker));
  return results;
}
