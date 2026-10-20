import { execFile } from "node:child_process";
import { createHash, randomUUID } from "node:crypto";
import {
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
  statSync,
} from "node:fs";
import path from "node:path";
import { promisify } from "node:util";
import {
  type MsvcArch,
  findToolchain,
  resolveOnPath,
} from "./toolchain.ts";

const execFileAsync = promisify(execFile);
const packageRoot = path.resolve(import.meta.dirname, "..");
const sourceFile = path.join(packageRoot, "extract", "stela_extract.c");
const thirdParty = path.join(packageRoot, "third_party");
const outputRoot = path.join(packageRoot, "out", "stela");
const toolchainPromise = findToolchain();
const extractorBuilds = new Map<MsvcArch, Promise<Extractor>>();
const systemIncludes = new Map<MsvcArch, Promise<readonly string[]>>();

export interface Extractor {
  readonly exe: string;
  readonly libclangBin: string;
}

export interface ExtractOptions {
  readonly target: string;
  readonly root: string;
  readonly out: string;
  readonly headers: readonly string[];
  readonly includes?: readonly string[];
  readonly defines?: readonly string[];
  readonly owns?: readonly string[];
}

export function extractorEnvironment(libclangBin: string): NodeJS.ProcessEnv {
  const env = { ...process.env };
  const inherited = env.Path ?? env.PATH ?? "";
  delete env.Path;
  delete env.PATH;
  env.Path = `${libclangBin};${inherited}`;
  return env;
}

function hostArch(): MsvcArch {
  if (process.arch === "x64" || process.arch === "arm64") return process.arch;
  throw new Error(`stela-extract needs an x64 or ARM64 Windows host, not ${process.arch}`);
}

async function toolchain() {
  const value = await toolchainPromise;
  if (typeof value === "string") throw new Error(value);
  return value;
}

function llvmRoot(vsPath: string, arch: MsvcArch): string {
  const override = process.env.STELA_LIBCLANG_DIR;
  if (override) {
    if (arch !== hostArch()) {
      throw new Error("STELA_LIBCLANG_DIR can only override the native host libclang");
    }
    return path.dirname(path.resolve(override));
  }
  return path.join(vsPath, "VC", "Tools", "Llvm", arch === "arm64" ? "ARM64" : "x64");
}

function hashTree(hash: ReturnType<typeof createHash>, directory: string): void {
  for (const entry of readdirSync(directory, { withFileTypes: true }).sort((a, b) =>
    a.name.localeCompare(b.name),
  )) {
    const file = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      hashTree(hash, file);
    } else if (entry.isFile()) {
      hash.update(path.relative(packageRoot, file).replaceAll("\\", "/"));
      hash.update(readFileSync(file));
    }
  }
}

function buildKey(arch: MsvcArch, libclangDll: string): string {
  const hash = createHash("sha256");
  hash.update(arch);
  hash.update(readFileSync(sourceFile));
  hashTree(hash, thirdParty);
  hash.update(readFileSync(libclangDll));
  return hash.digest("hex").slice(0, 16);
}

async function buildExtractorImpl(arch: MsvcArch): Promise<Extractor> {
  const tools = await toolchain();
  const llvm = llvmRoot(tools.vsPath, arch);
  const libclangBin = process.env.STELA_LIBCLANG_DIR
    ? path.resolve(process.env.STELA_LIBCLANG_DIR)
    : path.join(llvm, "bin");
  const libclangDll = path.join(libclangBin, "libclang.dll");
  const libclangLib = path.join(llvm, "lib", "libclang.lib");
  if (!existsSync(libclangDll)) throw new Error(`libclang.dll not found at ${libclangDll}`);
  if (!existsSync(libclangLib)) throw new Error(`libclang.lib not found at ${libclangLib}`);

  const directory = path.join(outputRoot, `${arch}-${buildKey(arch, libclangDll)}`);
  const exe = path.join(directory, "stela-extract.exe");
  if (!existsSync(exe)) {
    // Concurrent processes (node --test runs files in parallel) may build the
    // same key: each compiles privately, then publishes the exe into the cache.
    const scratch = path.join(outputRoot, `.build-${arch}-${process.pid}-${randomUUID()}`);
    const built = path.join(scratch, "stela-extract.exe");
    mkdirSync(scratch, { recursive: true });
    try {
      const env = await tools.msvcEnv(arch);
      const compiler = resolveOnPath("cl.exe", env);
      let output: string;
      try {
        const { stdout, stderr } = await execFileAsync(
          compiler,
          [
            "/nologo",
            "/std:c17",
            "/W4",
            "/WX",
            "/TC",
            `/external:I${thirdParty}`,
            "/external:W0",
            `/I${thirdParty}`,
            sourceFile,
            `/Fe:${built}`,
            "/link",
            libclangLib,
          ],
          { cwd: scratch, env, maxBuffer: 64 * 1024 * 1024 },
        );
        output = `${stdout}${stderr}`;
      } catch (error) {
        const { stdout = "", stderr = "" } = error as { stdout?: string; stderr?: string };
        throw new Error(`building stela-extract.exe failed\n${stdout}${stderr}`, { cause: error });
      }
      if (!existsSync(built)) {
        throw new Error(`cl.exe did not create ${built}\n${output}`);
      }
      mkdirSync(directory, { recursive: true });
      if (!existsSync(exe)) {
        try {
          renameSync(built, exe);
        } catch (error) {
          // Another process published (and may be running) the same build.
          if (!existsSync(exe)) throw error;
        }
      }
    } finally {
      rmSync(scratch, { recursive: true, force: true });
    }
  }
  return { exe, libclangBin };
}

export function buildExtractor(arch: MsvcArch = hostArch()): Promise<Extractor> {
  let build = extractorBuilds.get(arch);
  if (!build) {
    build = buildExtractorImpl(arch);
    extractorBuilds.set(arch, build);
  }
  return build;
}

function targetArch(target: string): MsvcArch {
  if (target.startsWith("i686-")) return "x86";
  if (target.startsWith("aarch64-") || target.startsWith("arm64ec-")) return "arm64";
  return "x64";
}

export async function systemIncludeDirs(target: string): Promise<readonly string[]> {
  const arch = targetArch(target);
  let includes = systemIncludes.get(arch);
  if (!includes) {
    includes = toolchain().then(async (tools) => {
      const env = await tools.msvcEnv(arch);
      return (env.INCLUDE ?? "").split(";").filter(Boolean);
    });
    systemIncludes.set(arch, includes);
  }
  return includes;
}

export async function runExtractor(options: ExtractOptions): Promise<{
  readonly stdout: string;
  readonly stderr: string;
}> {
  const extractor = await buildExtractor();
  const includes = [
    ...(await systemIncludeDirs(options.target)),
    ...(options.includes ?? []),
  ];
  const args = [
    "--target",
    options.target,
    "--root",
    options.root,
    "--out",
    options.out,
    ...includes.flatMap((include) => ["--include", include]),
    ...(options.defines ?? []).flatMap((define) => ["--define", define]),
    ...(options.owns ?? []).flatMap((owned) => ["--owns", owned]),
    "--",
    ...options.headers,
  ];
  return execFileAsync(extractor.exe, args, {
    cwd: options.root,
    env: extractorEnvironment(extractor.libclangBin),
    maxBuffer: 64 * 1024 * 1024,
  });
}
