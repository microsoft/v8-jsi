// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Checks that a Windows PDB lets a debugger fetch our sources from GitHub
// through Source Link (src/generate_source_link.ts). For each PDB:
//
//   - its "sourcelink" stream maps the build's repository root to
//     https://raw.githubusercontent.com/microsoft/v8-jsi/<commit>/, and <commit>
//     is the one checked out;
//   - every source under the repository root is a file tracked at <commit>,
//     with the same case (GitHub URLs are case-sensitive);
//   - each such source's checksum in the PDB matches the file in Git, which is
//     what GitHub serves: a debugger rejects a file whose checksum differs.
//
// Generated files under deps\nodejs\out\ and deps\chromium\out\ have no GitHub
// copy and are skipped.
// Sources outside the repository root (Visual Studio, the Windows SDK, the
// Microsoft C runtime, the Rust standard library) aren't ours and are skipped.
// So is the "no source file" placeholder that Microsoft Rust's prebuilt standard
// library objects record without a checksum: the linker makes it absolute
// against the build's working directory, so it lands under the repository root.
// A relative Chromium path (o:\...; gn's symbol_level below 2) fails: no map
// entry can resolve it.
//
// A stale map commit and checksum mismatches fail only on CI (TF_BUILD) or with
// --strict. Locally they are warnings: an incremental build after a commit
// keeps the previous map, and uncommitted edits differ from Git.
//
//   node scripts/source-link-check.ts [--commit <sha>] [--strict] <pdb>...

import { execFileSync } from "node:child_process";
import * as crypto from "node:crypto";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs } from "node:util";

import { repoRoot, vswhereExe } from "./build-common.ts";

const githubPrefix = "https://raw.githubusercontent.com/microsoft/v8-jsi/";

export interface SourceLinkOptions {
  // The commit the map must name; defaults to the checked-out HEAD.
  commit?: string;
  // Fail, rather than warn, on a stale map commit or a checksum mismatch.
  strict?: boolean;
}

interface PdbSource {
  path: string;
  kind: string;
  checksum: string;
}

let cachedPdbUtil: string | undefined;

// llvm-pdbutil from the Visual Studio LLVM tools (the clang-cl component the
// builds already need), for the host architecture first.
function findPdbUtil(): string {
  if (cachedPdbUtil) return cachedPdbUtil;
  const vswhere = vswhereExe();
  if (vswhere) {
    const hosts = process.arch === "arm64" ? ["ARM64", "x64"] : ["x64", "ARM64"];
    for (const host of hosts) {
      try {
        const found = execFileSync(
          vswhere,
          ["-latest", "-prerelease", "-products", "*", "-find", `VC\\Tools\\Llvm\\${host}\\bin\\llvm-pdbutil.exe`],
          { encoding: "utf-8" },
        )
          .split(/\r?\n/)
          .map((line) => line.trim())
          .find((line) => line.length > 0 && fs.existsSync(line));
        if (found) return (cachedPdbUtil = found);
      } catch {
        // Try the next host architecture, then PATH.
      }
    }
  }
  try {
    const onPath = execFileSync("where.exe", ["llvm-pdbutil.exe"], { encoding: "utf-8" })
      .split(/\r?\n/)
      .map((line) => line.trim())
      .find((line) => line.length > 0);
    if (onPath) return (cachedPdbUtil = onPath);
  } catch {
    // Not on PATH either.
  }
  throw new Error(
    "llvm-pdbutil.exe not found (install the Visual Studio \"C++ Clang Compiler for Windows\" component).",
  );
}

function git(args: string[], input?: string): Buffer {
  return execFileSync("git", ["-C", repoRoot, ...args], {
    input,
    maxBuffer: 1 << 30,
    stdio: ["pipe", "pipe", "pipe"],
  });
}

function readSourceLinkMap(pdbUtil: string, pdb: string): Record<string, string> | undefined {
  const out = path.join(os.tmpdir(), `sourcelink-${process.pid}-${path.basename(pdb)}.json`);
  try {
    execFileSync(pdbUtil, ["export", "--stream=sourcelink", `--out=${out}`, pdb], { stdio: "pipe" });
    const documents = JSON.parse(fs.readFileSync(out, "utf-8")).documents;
    return documents && typeof documents === "object" ? documents : undefined;
  } catch {
    return undefined;
  } finally {
    fs.rmSync(out, { force: true });
  }
}

// The source files of every module, from `llvm-pdbutil dump --files`: lines
// such as "- (SHA-256: 0A1B...) D:\a\_work\1\s\src\v8_core.cpp" or "- (None: ) path".
// Modules compiled at different times can record different checksums for the
// same file, so each distinct (path, kind, checksum) record is kept.
function readSources(pdbUtil: string, pdb: string): PdbSource[] {
  const out = execFileSync(pdbUtil, ["dump", "--files", pdb], { encoding: "utf-8", maxBuffer: 1 << 30 });
  const sources = new Map<string, PdbSource>();
  for (const line of out.split(/\r?\n/)) {
    const match = /^\s*- \(([^:)]*):\s*([0-9A-Fa-f]*)\) (.+)$/.exec(line);
    if (!match) continue;
    const source = { path: match[3].trim(), kind: match[1], checksum: match[2].toLowerCase() };
    const key = `${source.path}\0${source.kind}\0${source.checksum}`;
    if (!sources.has(key)) sources.set(key, source);
  }
  return [...sources.values()];
}

// Blob contents at a commit, through one `git cat-file --batch`.
function readBlobs(commit: string, files: string[]): Map<string, Buffer> {
  const blobs = new Map<string, Buffer>();
  if (files.length === 0) return blobs;
  const out = git(["cat-file", "--batch"], files.map((file) => `${commit}:${file}\n`).join(""));
  let offset = 0;
  for (const file of files) {
    const end = out.indexOf(0x0a, offset);
    const header = out.subarray(offset, end).toString("utf-8").split(" ");
    offset = end + 1;
    if (header[1] !== "blob") continue;
    const size = Number(header[2]);
    blobs.set(file, out.subarray(offset, offset + size));
    offset += size + 1;
  }
  return blobs;
}

const hashAlgorithms: Record<string, string> = { MD5: "md5", "SHA-1": "sha1", "SHA1": "sha1", "SHA-256": "sha256" };

function hash(algorithm: string, data: Buffer): string {
  return crypto.createHash(algorithm).update(data).digest("hex");
}

function toCrlf(data: Buffer): Buffer {
  return Buffer.from(data.toString("latin1").replace(/\r?\n/g, "\r\n"), "latin1");
}

const trackedFilesCache = new Map<string, Map<string, string>>();

// Tracked paths at a commit, keyed by their lowercase form.
function trackedFiles(commit: string): Map<string, string> {
  let files = trackedFilesCache.get(commit);
  if (!files) {
    files = new Map();
    for (const file of git(["ls-tree", "-r", "-z", "--name-only", commit]).toString("utf-8").split("\0")) {
      if (file) files.set(file.toLowerCase(), file);
    }
    trackedFilesCache.set(commit, files);
  }
  return files;
}

function commitExists(commit: string): boolean {
  try {
    git(["cat-file", "-e", `${commit}^{commit}`]);
    return true;
  } catch {
    return false;
  }
}

// Reports up to a few examples of each problem.
function report(kind: "error" | "warning", message: string, examples: string[]): void {
  if (examples.length === 0) return;
  const label = kind === "error" ? "FAIL" : "warn";
  console.log(`  ${label} ${message}: ${examples.length}`);
  for (const example of examples.slice(0, 5)) console.log(`         ${example}`);
  if (examples.length > 5) console.log(`         ... and ${examples.length - 5} more`);
}

function checkPdb(pdbUtil: string, pdb: string, commit: string, strict: boolean): boolean {
  console.log(`\n${pdb}`);
  if (!fs.existsSync(pdb)) {
    console.log("  FAIL the PDB doesn't exist");
    return false;
  }
  const map = readSourceLinkMap(pdbUtil, pdb);
  if (!map) {
    console.log("  FAIL no Source Link map (\"sourcelink\" stream)");
    return false;
  }
  const rules = Object.entries(map)
    .filter(([local]) => local.endsWith("*"))
    .map(([local, url]) => ({ local: local.slice(0, -1).toLowerCase(), url }))
    .sort((a, b) => b.local.length - a.local.length);
  const rootRule = rules.find((rule) => new RegExp(`^${githubPrefix.replace(/[./]/g, "\\$&")}[0-9a-f]{40}/\\*$`).test(rule.url));
  if (!rootRule) {
    console.log(`  FAIL the map has no repository-root entry pointing at ${githubPrefix}<commit>/: ${JSON.stringify(map)}`);
    return false;
  }
  const mapCommit = rootRule.url.slice(githubPrefix.length, githubPrefix.length + 40);
  console.log(`  map: ${rootRule.local}* -> ${rootRule.url}`);

  let failed = false;
  const fail = (message: string, examples: string[]) => {
    report("error", message, examples);
    if (examples.length > 0) failed = true;
  };
  const strictOrWarn = (message: string, examples: string[]) => {
    if (strict) fail(message, examples);
    else report("warning", message, examples);
  };

  if (mapCommit !== commit) {
    strictOrWarn(`the map names commit ${mapCommit}, not ${commit} (rebuild to refresh it)`, [pdb]);
  }
  const blobCommit = commitExists(mapCommit) ? mapCommit : commit;
  const tracked = trackedFiles(blobCommit);

  const own: { source: PdbSource; file: string }[] = [];
  const notTracked = new Set<string>();
  const wrongCase = new Set<string>();
  const relativeChromium = new Set<string>();
  const generated = new Set<string>();
  const foreign = new Set<string>();
  for (const source of readSources(pdbUtil, pdb)) {
    const lower = source.path.toLowerCase();
    if (lower.startsWith("o:\\")) {
      relativeChromium.add(source.path);
      continue;
    }
    if (!lower.startsWith(rootRule.local) || /\\<?no source file>?$/.test(lower)) {
      foreign.add(lower);
      continue;
    }
    const relative = source.path.slice(rootRule.local.length);
    if (/^deps\\(nodejs|chromium)\\out\\/i.test(relative)) {
      generated.add(lower);
      continue;
    }
    const file = relative.replace(/\\/g, "/");
    const trackedFile = tracked.get(file.toLowerCase());
    if (!trackedFile) notTracked.add(file);
    else if (trackedFile !== file) wrongCase.add(`${file} (Git: ${trackedFile})`);
    else own.push({ source, file });
  }

  const ownFiles = new Set(own.map((o) => o.file));
  const blobs = readBlobs(blobCommit, [...new Set(own.filter((o) => hashAlgorithms[o.source.kind]).map((o) => o.file))]);
  const crlf = new Set<string>();
  const differs = new Set<string>();
  let verified = 0;
  let unverifiable = 0;
  for (const { source, file } of own) {
    const algorithm = hashAlgorithms[source.kind];
    const blob = blobs.get(file);
    if (!algorithm || !blob) {
      unverifiable++;
      continue;
    }
    if (hash(algorithm, blob) === source.checksum) verified++;
    else if (hash(algorithm, toCrlf(blob)) === source.checksum) crlf.add(file);
    else differs.add(file);
  }

  console.log(
    `  sources: ${ownFiles.size + notTracked.size + wrongCase.size} ours (${verified} checksums match, ` +
      `${unverifiable} without a checksum), ${generated.size} generated, ${foreign.size} not ours`,
  );
  fail("relative Chromium source paths (o:\\...; build with gn symbol_level = 2)", [...relativeChromium]);
  fail("sources not tracked in Git", [...notTracked]);
  fail("sources whose case differs from Git", [...wrongCase]);
  strictOrWarn("sources built with CRLF line endings (Git has LF; check .gitattributes and core.autocrlf)", [...crlf]);
  strictOrWarn("sources that differ from Git", [...differs]);
  if (own.length === 0) fail("no sources under the repository root", [pdb]);
  if (!failed) console.log("  ok");
  return !failed;
}

// Checks each PDB and returns true when all of them pass.
export function checkSourceLink(pdbs: string[], options: SourceLinkOptions = {}): boolean {
  const pdbUtil = findPdbUtil();
  const commit = options.commit ?? git(["rev-parse", "HEAD"]).toString("utf-8").trim();
  const strict = options.strict ?? /^true$/i.test(process.env.TF_BUILD ?? "");
  console.log(`Source Link: commit ${commit}${strict ? " (strict)" : ""}`);
  let ok = true;
  for (const pdb of pdbs) {
    if (!checkPdb(pdbUtil, pdb, commit, strict)) ok = false;
  }
  return ok;
}

// The PDB of a binary: lld-link names it <binary>.pdb (sbox.exe.pdb) or, from
// gyp, <name>.pdb (v8jsi.pdb).
export function pdbOf(binary: string): string {
  const full = `${binary}.pdb`;
  if (fs.existsSync(full)) return full;
  const parsed = path.parse(binary);
  return path.join(parsed.dir, `${parsed.name}.pdb`);
}

if (import.meta.filename === path.resolve(process.argv[1] ?? "")) {
  const { values, positionals } = parseArgs({
    options: {
      commit: { type: "string" },
      strict: { type: "boolean" },
    },
    allowPositionals: true,
  });
  if (positionals.length === 0) {
    console.error("Usage: node scripts/source-link-check.ts [--commit <sha>] [--strict] <pdb>...");
    process.exit(2);
  }
  process.exit(checkSourceLink(positionals, { commit: values.commit, strict: values.strict }) ? 0 : 1);
}
