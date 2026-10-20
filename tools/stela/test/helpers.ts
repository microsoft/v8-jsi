import { spawnSync } from "node:child_process";
import { cpSync, mkdtempSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";

/** A v2 template: v1 plus one annotation, as a future Stela release would be. */
export function templateV2(v1: string): string {
  return v1
    .replace("(STELA_VERSION < 1)", "(STELA_VERSION < 2)")
    .replace("#define STELA_VERSION 1", "#define STELA_VERSION 2")
    .replace(
      "/* Tooling */",
      '/* Threads */\n#undef STELA_THREAD_ANY\n#define STELA_THREAD_ANY STELA_ANNOTATE_("thread=any")\n\n/* Tooling */',
    );
}

export function tempDir(t: { after(fn: () => void): void }, name: string): string {
  const dir = mkdtempSync(path.join(os.tmpdir(), `stela-${name}-`));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  return dir;
}

export const cli = path.join(import.meta.dirname, "..", "src", "stela.ts");

export function runCli(cwd: string, ...args: string[]) {
  const r = spawnSync(process.execPath, [cli, ...args], { cwd, encoding: "utf8" });
  return { code: r.status, out: r.stdout, err: r.stderr };
}

/** A temporary copy of test/fixtures/model, with its demo_stela.h vendored. */
export function modelFixture(t: { after(fn: () => void): void }, name: string): string {
  const dir = tempDir(t, name);
  cpSync(path.join(import.meta.dirname, "fixtures", "model"), dir, { recursive: true });
  const r = runCli(dir, "vendor");
  if (r.code !== 0) throw new Error(`vendor failed: ${r.err}`);
  return dir;
}
