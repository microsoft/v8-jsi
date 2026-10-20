// Embedded manifests end to end (design §12, §14): `gen embed` writes the
// resource, rc and link put it in real DLLs and EXEs for x64, x86, and ARM64,
// and `show`, `verify`, `check`, and `diff` read it back without loading the
// binary.

import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { copyFileSync, cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, utimesSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { after, before, test } from "node:test";
import { promisify } from "node:util";
import { buildManifest } from "../src/embed.ts";
import { loadConfig } from "../src/config.ts";
import { readModel } from "../src/model.ts";
import { type MsvcArch, type Toolchain, findToolchain, resolveOnPath } from "../src/toolchain.ts";
import { runCli } from "./helpers.ts";

const execFileAsync = promisify(execFile);
const TRIPLES: Record<MsvcArch, string> = {
  x64: "x86_64-pc-windows-msvc",
  x86: "i686-pc-windows-msvc",
  arm64: "aarch64-pc-windows-msvc",
};
const HOST: MsvcArch | undefined = process.arch === "x64" ? "x64" : process.arch === "arm64" ? "arm64" : undefined;

const DLL_SOURCE = `#include <windows.h>
#define EXPORT __declspec(dllexport)
EXPORT int __cdecl demo_open(void* options, void* out_session) { return options == out_session; }
#ifndef NO_EXT
EXPORT int __cdecl demo_ext_query(void* session, void* out_info) { return session == out_info; }
#endif
#ifdef WITH_SECRET
EXPORT int __cdecl demo_secret(void) { return 0; }
#endif
/* Leaves <dll>.ran behind if anything ever loads the DLL. */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    char path[MAX_PATH + 8];
    DWORD n = GetModuleFileNameA(instance, path, MAX_PATH);
    HANDLE file;
    lstrcpyA(path + n, ".ran");
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  }
  return TRUE;
}
`;
const EXE_SOURCE = "int main(void) { return 0; }\n";

const BINARIES = [
  { name: "demo.dll", libraries: ["demo", "demo_ext"], otherExports: false },
  { name: "demo_core.dll", libraries: ["demo"] },
  { name: "demo.exe", libraries: ["demo"], exports: "none", otherExports: false },
];

let unavailable: string | undefined;
let toolchain: Toolchain;
let work: string;
let root: string;
const built = new Map<string, string>();

function fixture(name: string): string {
  const dir = path.join(work, name);
  cpSync(path.join(import.meta.dirname, "fixtures", "model"), dir, { recursive: true });
  const configFile = path.join(dir, "stela.config.json");
  const config = JSON.parse(readFileSync(configFile, "utf8"));
  writeFileSync(configFile, JSON.stringify({ ...config, binaries: BINARIES }, null, 2));
  for (const command of [["vendor"], ["extract"]]) {
    const r = runCli(dir, ...command);
    if (r.code !== 0) throw new Error(`${command} failed: ${r.err}`);
  }
  return dir;
}

/** Compiles `source` into `<dir>/<name>`, linking `<rc>` when given. */
async function build(arch: MsvcArch, dir: string, name: string, source: string, rc?: string, defines: string[] = []) {
  const env = await toolchain.msvcEnv(arch);
  mkdirSync(dir, { recursive: true });
  const src = path.join(dir, name.endsWith(".exe") ? "main.c" : "dll.c");
  writeFileSync(src, source);
  const inputs = [src];
  if (rc !== undefined) {
    const res = path.join(dir, `${name}.res`);
    // rc runs from elsewhere: the script names the .gz relative to itself.
    await execFileAsync(resolveOnPath("rc.exe", env), ["/nologo", "/fo", res, rc], { cwd: work, env });
    inputs.push(res);
  }
  const out = path.join(dir, name);
  const kind = name.endsWith(".exe") ? [] : ["/LD"];
  const args = ["/nologo", "/Brepro", "/O1", ...kind, ...defines.map((d) => `/D${d}`), ...inputs, `/Fe${out}`, `/Fo${dir}\\`, "/link", "/Brepro"];
  try {
    await execFileAsync(resolveOnPath("cl.exe", env), args, { cwd: dir, env });
  } catch (e) {
    throw new Error(`cl ${name} (${arch}) failed: ${(e as { stdout?: string }).stdout ?? e}`);
  }
  return out;
}

/** `gen embed` for `name` on `arch`, then the binary built with it; returns its path. */
async function embedAndBuild(fixtureRoot: string, arch: MsvcArch, name: string, variant = "", defines: string[] = []) {
  const out = path.join(fixtureRoot, "out", `${arch}${variant}`);
  const r = runCli(fixtureRoot, "gen", "embed", "--binary", name, "--target", TRIPLES[arch], "--out", out);
  assert.equal(r.code, 0, r.err);
  const source = name.endsWith(".exe") ? EXE_SOURCE : DLL_SOURCE;
  return build(arch, path.join(out, name), name, source, path.join(out, `${name}.stela.rc`), defines);
}

const binaryOf = (arch: MsvcArch, name: string) => built.get(`${arch}/${name}`)!;

before(async () => {
  const found = await findToolchain();
  if (typeof found === "string") {
    unavailable = found;
    return;
  }
  toolchain = found;
  work = mkdtempSync(path.join(os.tmpdir(), "stela-embed-"));
  root = fixture("demo");
  const jobs: [MsvcArch, string][] = [
    ["x64", "demo.dll"],
    ["arm64", "demo.dll"],
    ["x86", "demo_core.dll"],
    ["x64", "demo.exe"],
    ["x86", "demo.exe"],
    ["arm64", "demo.exe"],
  ];
  await Promise.all(
    jobs.map(async ([arch, name]) => {
      const defines = name === "demo_core.dll" ? ["NO_EXT"] : [];
      built.set(`${arch}/${name}`, await embedAndBuild(root, arch, name, "", defines));
    }),
  );
});

after(() => {
  if (work) rmSync(work, { recursive: true, force: true });
});

test("show prints each binary's API from its manifest without loading it", (t) => {
  if (unavailable) return t.skip(unavailable);
  const dll = binaryOf("x64", "demo.dll");
  let r = runCli(root, "show", dll);
  assert.equal(r.code, 0, r.err);
  assert.match(r.out, /^demo\.dll: demo\.dll for x86_64-pc-windows-msvc \(machine 0x8664\)$/m);
  assert.match(r.out, /^  other exports: none$/m);
  assert.match(r.out, /^library demo: prefix demo_, status demo_status, 1 of 1 function\(s\) exported$/m);
  assert.match(r.out, /^  ABI versions: DEMO_ABI_VERSION = 3$/m);
  assert.match(r.out, /^  demo_status demo_open\(.*demo_options.*options, .*demo_session.*out_session\)  \[exported\]$/m);
  assert.match(r.out, /^library demo_ext: prefix demo_ext_, status demo_status, 1 of 1 function\(s\) exported$/m);
  assert.match(r.out, /^  struct demo_wide \{2 field\(s\), 16 bytes\}$/m);

  r = runCli(root, "show", "--json", dll);
  assert.equal(r.code, 0, r.err);
  const config = loadConfig(path.join(root, "stela.config.json"));
  const model = readModel(path.join(root, "out", "stela", "stela.model.json"));
  const binary = config.binaries.find((b) => b.name === "demo.dll")!;
  assert.deepEqual(JSON.parse(r.out), buildManifest(model, binary, [TRIPLES.x64]));

  // 32-bit: no 64-bit-only declarations; ARM64 and an EXE read the same way.
  r = runCli(root, "show", binaryOf("x86", "demo_core.dll"));
  assert.equal(r.code, 0, r.err);
  assert.match(r.out, /for i686-pc-windows-msvc \(machine 0x14c\)/);
  assert.doesNotMatch(r.out, /demo_wide/);
  r = runCli(root, "show", binaryOf("arm64", "demo.dll"));
  assert.match(r.out, /for aarch64-pc-windows-msvc \(machine 0xaa64\)/);
  r = runCli(root, "show", binaryOf("arm64", "demo.exe"));
  assert.match(r.out, /^library demo: .*, 0 of 1 function\(s\) exported$/m);

  for (const file of built.values()) assert.ok(!existsSync(`${file}.ran`), `reading ${file} loaded it`);
  // The marker works: loading a copy leaves one.
  if (HOST !== undefined) {
    const copy = path.join(path.dirname(binaryOf(HOST, "demo.dll")), "loaded.dll");
    copyFileSync(binaryOf(HOST, "demo.dll"), copy);
    assert.throws(() => process.dlopen({ exports: {} } as never, copy));
    assert.ok(existsSync(`${copy}.ran`));
  }
});

test("verify passes every binary built from its manifest", (t) => {
  if (unavailable) return t.skip(unavailable);
  const r = runCli(root, "verify", ...built.values());
  assert.equal(r.code, 0, r.err);
  assert.equal(r.out.split("\n").filter((l) => l.startsWith("OK   ")).length, built.size);
  assert.match(r.out, /^OK   demo\.dll: 2 export\(s\) match the manifest of demo, demo_ext$/m);
  assert.match(r.out, /^OK   demo\.exe: 0 export\(s\) match the manifest of demo$/m);
});

test("verify fails on missing and unexpected exports", async (t) => {
  if (unavailable) return t.skip(unavailable);
  const missing = await embedAndBuild(root, "x64", "demo.dll", "-missing", ["NO_EXT"]);
  const extra = await embedAndBuild(root, "x64", "demo.dll", "-extra", ["WITH_SECRET"]);
  const r = runCli(root, "verify", binaryOf("x64", "demo.dll"), missing, extra);
  assert.equal(r.code, 1);
  assert.match(r.out, /^OK   demo\.dll/m);
  assert.equal(
    r.err,
    "FAIL demo.dll: exports differ from the manifest of demo, demo_ext\n  missing: demo_ext_query\n" +
      "FAIL demo.dll: exports differ from the manifest of demo, demo_ext\n  unexpected: demo_secret\n",
  );
});

test("builds with /Brepro are byte-identical", async (t) => {
  if (unavailable) return t.skip(unavailable);
  const again = await embedAndBuild(root, "x64", "demo.dll", "-again");
  assert.ok(readFileSync(again).equals(readFileSync(binaryOf("x64", "demo.dll"))));
  const gz = (variant: string) => readFileSync(path.join(root, "out", `x64${variant}`, "demo.dll.stela.gz"));
  assert.ok(gz("-again").equals(gz("")));
});

test("check compares headers with a binary; diff takes binaries", async (t) => {
  if (unavailable) return t.skip(unavailable);
  const dll = binaryOf("x64", "demo.dll");
  const headers = ["include/demo.h", "include/demo_ext.h"];
  let r = runCli(root, "check", dll, "--config", "stela.config.json");
  assert.equal(r.code, 0, r.err);
  assert.equal(r.out, "0 change(s): 0 break(s), 0 addition(s), 0 note(s)\n");
  r = runCli(root, "check", dll, "--headers", ...headers);
  assert.equal(r.code, 0, r.err);
  assert.equal(r.out, "0 change(s): 0 break(s), 0 addition(s), 0 note(s)\n");
  r = runCli(root, "check", binaryOf("x86", "demo_core.dll"), "--headers", "include/demo.h");
  assert.equal(r.code, 0, r.err);
  r = runCli(root, "check", binaryOf("arm64", "demo.exe"), "--config", "stela.config.json");
  assert.equal(r.code, 0, r.err);

  // Headers that changed since the binary was built.
  const changed = fixture("changed");
  const demoH = path.join(changed, "include", "demo.h");
  const original = readFileSync(demoH, "utf8");
  const edited = original.replace(/demo_open\((\r?\n)/, "demo_open($1    uint32_t version,$1");
  assert.notEqual(edited, original);
  writeFileSync(demoH, edited);
  assert.equal(runCli(changed, "extract").code, 0);
  r = runCli(changed, "check", dll, "--headers", ...headers);
  assert.equal(r.code, 1, r.out);
  assert.match(r.out, /break\[.*\]: .*demo_open/);
  r = runCli(changed, "check", dll, "--config", "stela.config.json", "--format", "json");
  assert.equal(r.code, 1, r.out);
  assert.ok(JSON.parse(r.out).some((f: { class: string }) => f.class === "break"));

  // diff reads binaries as models.
  r = runCli(root, "diff", "--base", dll, "--head", dll);
  assert.equal(r.code, 0, r.err);
  assert.equal(r.out, "0 change(s): 0 break(s), 0 addition(s), 0 note(s)\n");
  const newer = await embedAndBuild(changed, "x64", "demo.dll");
  r = runCli(root, "diff", "--base", dll, "--head", newer);
  assert.equal(r.code, 1, r.out);
  assert.match(r.out, /break\[.*\]: .*demo_open/);

  // A model on one side is narrowed to the binary's libraries and targets.
  const model = path.join(root, "out", "stela", "stela.model.json");
  for (const binary of [dll, binaryOf("x86", "demo_core.dll"), binaryOf("arm64", "demo.exe")]) {
    for (const [base, head] of [[model, binary], [binary, model]]) {
      r = runCli(root, "diff", "--base", base, "--head", head);
      assert.equal(r.code, 0, `${path.basename(base)} -> ${path.basename(head)}: ${r.out}${r.err}`);
      assert.equal(r.out, "0 change(s): 0 break(s), 0 addition(s), 0 note(s)\n");
    }
  }
  r = runCli(changed, "diff", "--base", dll, "--head", path.join(changed, "out", "stela", "stela.model.json"));
  assert.equal(r.code, 1, r.out);
  assert.match(r.out, /break\[.*\]: .*demo_open/);
  // Two binaries compare the libraries and targets they share.
  r = runCli(root, "diff", "--base", dll, "--head", binaryOf("x64", "demo.exe"));
  assert.equal(r.code, 0, r.out);
  assert.equal(r.out, "0 change(s): 0 break(s), 0 addition(s), 0 note(s)\n");
  r = runCli(root, "diff", "--base", dll, "--head", binaryOf("arm64", "demo.dll"));
  assert.equal(r.code, 2, r.out);
  assert.match(r.err, /share no target/);
});

test("binaries without a manifest, or with another machine's, are refused", async (t) => {
  if (unavailable) return t.skip(unavailable);
  const bare = await build("x64", path.join(root, "out", "bare"), "demo.dll", DLL_SOURCE);
  let r = runCli(root, "show", bare);
  assert.equal(r.code, 2);
  assert.match(r.err, /no STELA resource/);

  // An x64 manifest linked into an ARM64 DLL.
  const rc = path.join(root, "out", "x64", "demo.dll.stela.rc");
  const wrong = await build("arm64", path.join(root, "out", "wrong"), "demo.dll", DLL_SOURCE, rc);
  r = runCli(root, "verify", wrong);
  assert.equal(r.code, 2);
  assert.match(r.err, /machine type 0xaa64 \(aarch64-pc-windows-msvc\) isn't among the manifest's targets/);

  r = runCli(root, "show", path.join(root, "stela.config.json"));
  assert.equal(r.code, 2);
  assert.match(r.err, /not a valid PE file/);
});

test("gen embed rejects bad requests", (t) => {
  if (unavailable) return t.skip(unavailable);
  const out = path.join(root, "out", "bad");
  const fails = (pattern: RegExp, ...args: string[]) => {
    const r = runCli(root, "gen", ...args);
    assert.equal(r.code, 2, r.out);
    assert.match(r.err, pattern);
  };
  fails(/no binary "nope.dll"/, "embed", "--binary", "nope.dll", "--target", TRIPLES.x64, "--out", out);
  fails(/needs --target/, "embed", "--binary", "demo.dll", "--out", out);
  fails(/needs --binary/, "embed", "--target", TRIPLES.x64, "--out", out);
  fails(/isn't built for i686/, "embed", "--binary", "demo.dll", "--target", TRIPLES.x86, "--out", out);
  fails(/options of `gen embed`/, "layout-tests", "--binary", "demo.dll", "--out", out);
  fails(/--model or --extract/, "embed", "--binary", "demo.dll", "--target", TRIPLES.x64, "--extract", "--model", "m.json", "--out", out);
  assert.ok(!existsSync(out));

  // --extract gives the bytes the model does.
  const r = runCli(root, "gen", "embed", "--binary", "demo.dll", "--target", TRIPLES.x64, "--extract", "--out", out);
  assert.equal(r.code, 0, r.err);
  assert.ok(readFileSync(path.join(out, "demo.dll.stela.gz")).equals(readFileSync(path.join(root, "out", "x64", "demo.dll.stela.gz"))));

  // --stamp is written on every run; unchanged outputs keep their times.
  fails(/options of `gen embed`/, "layout-tests", "--stamp", "s", "--out", out);
  const gz = path.join(out, "demo.dll.stela.gz");
  const stamp = path.join(out, "stamps", "demo.dll.stela.stamp");
  const old = new Date(Date.now() - 3_600_000);
  utimesSync(gz, old, old);
  const again = runCli(root, "gen", "embed", "--binary", "demo.dll", "--target", TRIPLES.x64, "--out", out, "--stamp", stamp);
  assert.equal(again.code, 0, again.err);
  assert.match(again.out, /unchanged: .*demo\.dll\.stela\.gz/);
  assert.equal(statSync(gz).mtimeMs, old.getTime());
  assert.ok(statSync(stamp).mtimeMs > old.getTime());
});
