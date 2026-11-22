// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

import { join, win32, resolve } from "node:path";
import { readFileSync, openSync, closeSync, fstatSync, readSync, writeFileSync } from "node:fs";

import { createHash } from "node:crypto";

interface ArchiveSource { length: number; read(start: number, end: number): Buffer; }

export function checkArchiveFileCfg(file: string, allowedAssembler: ReadonlySet<string>): number {
  const fd = openSync(file, "r");
  try {
    return checkArchiveCfg({ length: fstatSync(fd).size, read(start, end) {
      const data = Buffer.alloc(end - start);
      if (readSync(fd, data, 0, data.length, start) !== data.length) throw new Error("Truncated archive read");
      return data;
    } }, allowedAssembler);
  } finally { closeSync(fd); }
}

// COFF archives contain both ordinary objects and /bigobj objects. Linker
// indexes and the long-name table are metadata, not code to exempt from CFG.
export function checkArchiveCfg(archive: Buffer | ArchiveSource, allowedAssembler: ReadonlySet<string>): number {
  const read = Buffer.isBuffer(archive) ? (start: number, end: number) => archive.subarray(start, end) : archive.read;
  if (archive.length < 8 || read(0, 8).toString() !== "!<arch>\n") throw new Error("Invalid COFF archive");
  let names: Buffer = Buffer.alloc(0);
  let count = 0;
  for (let offset = 8; offset < archive.length;) {
    if (offset + 60 > archive.length) throw new Error("Truncated archive header");
    const header = read(offset, offset + 60);
    if (header.toString("ascii", 58, 60) !== "`\n") {
      throw new Error("Invalid archive member header");
    }
    const rawName = header.toString("ascii", 0, 16).trim();
    const sizeText = header.toString("ascii", 48, 58).trim();
    if (!/^\d+$/.test(sizeText)) throw new Error("Invalid archive member size");
    const size = Number(sizeText);
    const end = offset + 60 + size;
    if (end > archive.length) throw new Error("Truncated archive member");
    const object = read(offset + 60, end);
    offset = end + (size & 1);
    if (rawName === "//") { names = object; continue; }
    if (rawName === "/" || rawName === "/SYM64/") continue;
    let name = rawName.replace(/\/$/, "");
    if (/^\/\d+$/.test(rawName)) {
      const start = Number(rawName.slice(1));
      const terminator = names.indexOf(0, start);
      if (start >= names.length || terminator < 0) throw new Error("Invalid archive long name");
      name = names.toString("utf8", start, terminator);
    }
    const fail = (reason: string): never => { throw new Error(`${name}: ${reason}`); };
    if (object.length < 20) fail("Invalid COFF object");
    const big = object.readUInt16LE(0) === 0 && object.readUInt16LE(2) === 0xffff;
    if (big && (object.length < 56 || object.readUInt16LE(4) < 2)) fail("Unsupported COFF object");
    if (![0x14c, 0x8664, 0xaa64].includes(object.readUInt16LE(big ? 6 : 0))) fail("Unsupported COFF machine");
    const symbolSize = big ? 20 : 18;
    const table = object.readUInt32LE(big ? 48 : 8);
    const symbols = object.readUInt32LE(big ? 52 : 12);
    if (table + symbols * symbolSize > object.length) fail("Truncated COFF symbols");
    let feature: number | undefined;
    for (let i = 0; i < symbols;) {
      const pos = table + i * symbolSize;
      if (object.toString("ascii", pos, pos + 8) === "@feat.00") {
        const section = big ? object.readInt32LE(pos + 12) : object.readInt16LE(pos + 12);
        if (section !== -1 || feature !== undefined) fail("Invalid @feat.00 symbol");
        feature = object.readUInt32LE(pos + 8);
      }
      i += 1 + object[pos + symbolSize - 1];
      if (i > symbols) fail("Invalid COFF auxiliary symbols");
    }
    const base = name.replaceAll("\\", "/").split("/").at(-1)!;
    if (allowedAssembler.has(base)) { count++; continue; }
    if (feature === undefined || !(feature & 0x800)) fail("Missing CFG @feat.00 bit 0x800 (unexpected assembler is not allowed)");
    count++;
  }
  if (!count) throw new Error("Archive contains no COFF objects");
  return count;
}

export function checkPeFlags(pe: Buffer, highEntropy: boolean): void {
  if (pe.length < 64 || pe.toString("ascii", 0, 2) !== "MZ") throw new Error("Invalid PE header");
  const offset = pe.readUInt32LE(60);
  if (offset + 96 > pe.length || pe.toString("ascii", offset, offset + 4) !== "PE\0\0") {
    throw new Error("Invalid PE signature");
  }
  const optional = offset + 24;
  if (![0x10b, 0x20b].includes(pe.readUInt16LE(optional))) throw new Error("Invalid PE optional header");
  const flags = pe.readUInt16LE(optional + 70);
  for (const [name, bit] of [["CFG", 0x4000], ["DYNAMICBASE", 0x40], ["NX", 0x100],
                            ...(highEntropy ? [["HIGHENTROPYVA", 0x20]] : [])] as Array<[string, number]>) {
    if (!(flags & bit)) throw new Error(`PE is missing ${name}`);
  }
}

export function checkIteratorConfiguration(
  configurations: Record<string, { defines?: string[] }>, platform: string,
  projects: Iterable<readonly [string, string]>,
): void {
  const define = "_HAS_ITERATOR_DEBUGGING=0";
  for (const name of ["Debug", "Release"]) {
    if (!configurations[name] || (configurations[name].defines ?? []).includes(define) !==
        (platform === "arm64" && name === "Debug")) {
      throw new Error(`Iterator configuration mismatch: ${name}`);
    }
  }
  for (const [file, xml] of projects) {
    for (const group of xml.matchAll(/<ItemDefinitionGroup\b([^>]*)>([\s\S]*?)<\/ItemDefinitionGroup>/g)) {
      const definitions = /<PreprocessorDefinitions>([^<]*)<\/PreprocessorDefinitions>/.exec(group[2]);
      if (!group[2].includes("<ClCompile>")) continue;
      const debug = group[1].includes("Debug|");
      if ((definitions?.[1] ?? "").split(";").includes(define) !== (platform === "arm64" && debug)) {
        throw new Error(`Iterator project mismatch: ${file} (${debug ? "Debug" : "Release"})`);
      }
    }
  }
}

export function linkedArchiveNames(project: string, readProject: (reference: string) => string): string[] {
  return [...project.matchAll(/<ProjectReference Include="([^"]+)"/g)]
    .map((match) => match[1])
    .filter((reference) => readProject(reference).includes("<ConfigurationType>StaticLibrary</ConfigurationType>"))
    .map((reference) => win32.basename(reference, ".vcxproj") + ".lib");
}

export interface LegacyExports { common: string[]; platforms: Record<string, string[]>; }
export function checkLegacyExports(names: readonly string[], expected: LegacyExports, platform: string): void {
  const allowed = [...expected.common, ...expected.platforms[platform]].sort();
  if (JSON.stringify([...names].sort()) !== JSON.stringify(allowed)) {
    throw new Error(`Legacy export mismatch (${platform}): expected ${allowed.length}, got ${names.length}`);
  }
}

export function normalizedHash(data: Buffer): string {
  return createHash("sha256").update(Buffer.from(data.toString("latin1").replaceAll("\r\n", "\n"), "latin1")).digest("hex");
}

export function checkLegacySources(root: string, read: (file: string) => Buffer = readFileSync): void {
  const manifest = JSON.parse(read(join(root, "src/legacy/sources.json")).toString("utf8"));
  if (manifest.contract !== "5424838ddaf" || !Array.isArray(manifest.files) || !manifest.files.length) throw new Error("Invalid legacy source manifest");
  for (const file of manifest.files) {
    const expected = file.outputSha256 ?? file.sha256;
    if (normalizedHash(read(join(root, file.path))) !== expected) {
      throw new Error(`Legacy source drift: ${file.path}. Run node scripts/legacy-checks.ts --update-sources.`);
    }
    if (file.kind === "inspector" && normalizedHash(read(join(root, file.origin))) !== file.sha256) {
      throw new Error(`Inspector donor drift: ${file.origin}. Run node scripts/legacy-checks.ts --update-sources.`);
    }
  }
}

export function updateLegacySources(root: string): void {
  const manifestPath = join(root, "src/legacy/sources.json");
  const text = readFileSync(manifestPath, "utf8");
  const manifest = JSON.parse(text);
  if (manifest.contract !== "5424838ddaf" || !Array.isArray(manifest.files) || !manifest.files.length) {
    throw new Error("Invalid legacy source manifest");
  }
  // Replace only hash tokens so provenance, restored pins, and formatting stay intact.
  const updated = text.replace(/\{[^{}]*\}/g, entry => {
    const file = JSON.parse(entry);
    const source = file.kind === "shared" && file.path.startsWith("src/") && !file.path.startsWith("src/legacy/")
      ? file.path : file.kind === "inspector" && file.origin.startsWith("src/inspector/") && file.outputSha256
      ? file.origin : undefined;
    if (!source) return entry;
    const hash = normalizedHash(readFileSync(join(root, source)));
    const pin = file.kind === "shared" && file.outputSha256 ? "outputSha256" : "sha256";
    return entry.replace(new RegExp(`("${pin}"\\s*:\\s*")[a-f0-9]{64}(")`), (_match, before, after) => before + hash + after);
  });
  if (updated !== text) writeFileSync(manifestPath, updated);
}

export function checkLegacyProject(project: string): void {
  for (const group of project.matchAll(/<ItemDefinitionGroup\b[^>]*>([\s\S]*?)<\/ItemDefinitionGroup>/g)) {
    if (!group[1].includes("<ClCompile>")) continue;
    if (group[1].includes("_HAS_EXCEPTIONS=0") || !group[1].includes("<ExceptionHandling>Sync</ExceptionHandling>") ||
        !group[1].includes("<RuntimeTypeInfo>false</RuntimeTypeInfo>")) {
      throw new Error("Legacy C++ exception/RTTI contract mismatch");
    }
  }
}

export function checkLegacyIncludes(log: string, root: string, shared: readonly string[]): void {
  const prefix = root.replaceAll("\\", "/").toLowerCase() + "/src/";
  const allowed = new Set(shared.map(file => file.toLowerCase()));
  let count = 0;
  for (const raw of log.split(/\r?\n/)) {
    const file = raw.replace(/^\^/, "").replaceAll("\\", "/").trim().toLowerCase();
    if (!file.startsWith(prefix)) continue;
    count++;
    const relative = "src/" + file.slice(prefix.length);
    if (!relative.startsWith("src/legacy/") && !allowed.has(relative)) throw new Error(`Legacy include leakage: ${relative}`);
  }
  if (!count) throw new Error("Legacy compile dependency log is empty");
}

export function checkLegacyStartupFlags(output: string, expected: string, shadowStacks: boolean): void {
  const lines = output.split(/\r?\n/);
  const flags = lines.filter(line => /^--(?:no-)?cet-compatible$/.test(line));
  const summary = `Startup shadow-stacks=${shadowStacks ? 1 : 0} callbacks=1`;
  if (flags.length !== 1 || flags[0] !== expected || lines.filter(line => line === summary).length !== 1) {
    throw new Error("Legacy startup policy mismatch");
  }
}

export function legacyCetArtifacts(platform: string): ReadonlyArray<readonly [string, boolean]> {
  return platform === "x64" ? [["v8jsi.dll", true], ["cetrun_legacy.exe", true], ["node_lite_legacy.exe", false]] : [];
}

export function checkLegacyHardeningProject(project: string): void {
  const groups = [...project.matchAll(/<ItemDefinitionGroup\b([^>]*)>([\s\S]*?)<\/ItemDefinitionGroup>/g)]
    .filter(group => group[2].includes("<ClCompile>"));
  if (groups.length !== 2) throw new Error("Missing legacy compiler configurations");
  for (const group of groups) {
    const runtime = group[1].includes("Debug|") ? "MultiThreadedDebug" : "MultiThreaded";
    for (const requirement of ["/ZH:SHA_256", "<BufferSecurityCheck>true</BufferSecurityCheck>",
                               `<RuntimeLibrary>${runtime}</RuntimeLibrary>`]) {
      if (!group[2].includes(requirement)) throw new Error(`Legacy compiler hardening missing: ${requirement}`);
    }
  }
}

export function checkLegacyIdentity(pe: Buffer, info: { OriginalFilename: string; ProductName: string; FileVersion: string; ProductVersion: string }, version: string): void {
  if (info.OriginalFilename !== "v8jsi.dll" || info.ProductName !== "React Native V8 JSI Adapter" || info.FileVersion !== version.split("-")[0] + ".0" ||
      info.ProductVersion !== version) {
    throw new Error("Legacy version resource mismatch");
  }
  if (pe.includes(Buffer.from("v8jsi2")) || pe.includes(Buffer.from(".?AVV8Runtime@v8runtime@@"))) {
    throw new Error("Legacy module identity/RTTI mismatch");
  }
}

if (import.meta.main) {
  if (process.argv.length !== 3 || process.argv[2] !== "--update-sources") {
    throw new Error("Usage: node scripts/legacy-checks.ts --update-sources");
  }
  updateLegacySources(resolve(import.meta.dirname, ".."));
}
