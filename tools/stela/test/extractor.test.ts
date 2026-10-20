import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import {
  copyFileSync,
  cpSync,
  mkdirSync,
  readFileSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";
import { before, test } from "node:test";
import { promisify } from "node:util";
import {
  buildExtractor,
  extractorEnvironment,
  runExtractor,
} from "../src/extractor.ts";
import { DEFAULT_TEMPLATE } from "../src/stela.ts";
import { findToolchain } from "../src/toolchain.ts";
import { generateLibraryHeader, parseTemplate } from "../src/vendor.ts";
import { tempDir } from "./helpers.ts";

const execFileAsync = promisify(execFile);
const fixtureRoot = path.join(import.meta.dirname, "fixtures", "extract");
const goldenRoot = path.join(import.meta.dirname, "golden");
const targets = [
  ["x64", "x86_64-pc-windows-msvc"],
  ["x86", "i686-pc-windows-msvc"],
  ["arm64", "aarch64-pc-windows-msvc"],
] as const;
let unavailable: string | undefined;

before(async () => {
  const toolchain = await findToolchain();
  if (typeof toolchain === "string") unavailable = toolchain;
});

function prepareFixture(t: { after(fn: () => void): void }, name: string): string {
  const root = tempDir(t, name);
  cpSync(fixtureRoot, root, { recursive: true });
  const template = parseTemplate(readFileSync(DEFAULT_TEMPLATE, "utf8"));
  writeFileSync(
    path.join(root, "probe_stela.h"),
    generateLibraryHeader(template, {
      macroPrefix: "PROBE",
      fileName: "probe_stela.h",
    }),
  );
  return root;
}

async function extract(root: string, target: string, outputName = "actual.json") {
  const out = path.join(root, outputName);
  await runExtractor({
    target,
    root,
    out,
    headers: [path.join(root, "probe.h"), path.join(root, "probe_more.h")],
    includes: [root],
  });
  return readFileSync(out, "utf8");
}

test("fixtures match stela.raw/1 goldens for x64, x86, and ARM64 targets", async (t) => {
  if (unavailable) return t.skip(unavailable);
  const root = prepareFixture(t, "extract-golden");
  for (const [name, target] of targets) {
    const actual = await extract(root, target, `${name}.json`);
    const expected = readFileSync(path.join(goldenRoot, `extractor-${name}.json`), "utf8");
    assert.equal(actual, expected, target);
    const raw = JSON.parse(actual);
    const status = raw.declarations.find(
      (declaration: { kind: string; name: string }) =>
        declaration.kind === "enum" && declaration.name === "probe_status",
    );
    assert.deepEqual(
      status.enumerators.map((enumerator: { name: string; value: number }) => [
        enumerator.name,
        enumerator.value,
      ]),
      [
        ["probe_status_ok", 0],
        ["probe_status_failed", 0x7001],
      ],
    );
    const facts = (typedefName: string) => {
      const declaration = raw.declarations.find(
        (candidate: { kind: string; name: string }) =>
          candidate.kind === "typedef" && candidate.name === typedefName,
      );
      return declaration.perTarget[target];
    };
    assert.equal(facts("probe_event_alias_cb").callingConvention, "c");
    assert.equal(
      facts("probe_default_alias_cb").callingConvention,
      name === "x86" ? "stdcall" : "c",
    );
  }
});

test("different absolute roots produce byte-identical output", async (t) => {
  if (unavailable) return t.skip(unavailable);
  const first = prepareFixture(t, "extract-root-a");
  const second = prepareFixture(t, "extract-root-b");
  assert.equal(await extract(first, targets[0][1]), await extract(second, targets[0][1]));
});

test("record facts include function-pointer field calling conventions", async (t) => {
  if (unavailable) return t.skip(unavailable);
  const root = tempDir(t, "extract-field-cdecl");
  const header = path.join(root, "slots.h");
  const out = path.join(root, "slots.json");
  writeFileSync(
    header,
    "typedef struct probe_api {\n" +
      "  void (__cdecl *run)(void *user_data);\n" +
      "  void *state;\n" +
      "} probe_api;\n",
  );
  await runExtractor({
    target: "i686-pc-windows-msvc",
    root,
    out,
    headers: [header],
  });
  const raw = JSON.parse(readFileSync(out, "utf8"));
  const record = raw.declarations.find(
    (declaration: { kind: string; name: string }) =>
      declaration.kind === "struct" && declaration.name === "probe_api",
  );
  assert.deepEqual(
    record.perTarget["i686-pc-windows-msvc"].fieldCallingConventions,
    ["c", null],
  );
  assert.equal(
    record.fields[0].type.pointer.function.params[0].name,
    "user_data",
  );
});

test("parse errors return 2 and do not replace an existing model", async (t) => {
  if (unavailable) return t.skip(unavailable);
  const root = prepareFixture(t, "extract-error");
  const bad = path.join(root, "bad.h");
  const out = path.join(root, "model.json");
  writeFileSync(bad, "typedef struct broken {\n");
  writeFileSync(out, "old model\n");
  const extractor = await buildExtractor();
  const good = await runExtractor({
    target: targets[0][1],
    root,
    out: path.join(root, "good.json"),
    headers: [path.join(root, "probe.h")],
    includes: [root],
  });
  assert.equal(good.stderr, "");

  await assert.rejects(
    execFileAsync(
      extractor.exe,
      [
        "--target",
        targets[0][1],
        "--root",
        root,
        "--out",
        out,
        "--include",
        root,
        "--",
        bad,
      ],
      {
        cwd: root,
        env: extractorEnvironment(extractor.libclangBin),
      },
    ),
    (error: NodeJS.ErrnoException & { code?: number; stderr?: string }) => {
      assert.equal(error.code, 2);
      assert.match(error.stderr ?? "", /error:/);
      return true;
    },
  );
  assert.equal(readFileSync(out, "utf8"), "old model\n");
});

test("owned paths outside the root are rejected", async (t) => {
  if (unavailable) return t.skip(unavailable);
  const root = tempDir(t, "extract-owned-root");
  const external = tempDir(t, "extract-owned-external");
  const header = path.join(root, "entry.h");
  const owned = path.join(external, "external.h");
  const out = path.join(root, "model.json");
  writeFileSync(owned, "typedef int external_type;\n");
  writeFileSync(header, `#include "${owned.replaceAll("\\", "/")}"\n`);
  await assert.rejects(
    runExtractor({
      target: targets[0][1],
      root,
      out,
      headers: [header],
      owns: [owned],
    }),
    (error: NodeJS.ErrnoException & { code?: number; stderr?: string }) => {
      assert.equal(error.code, 2);
      assert.match(error.stderr ?? "", /path is outside --root/);
      return true;
    },
  );
});

test("a failed replacement preserves the existing destination", async (t) => {
  if (unavailable) return t.skip(unavailable);
  const root = prepareFixture(t, "extract-replace");
  const out = path.join(root, "model.json");
  mkdirSync(out);
  writeFileSync(path.join(out, "sentinel"), "preserved\n");
  await assert.rejects(
    runExtractor({
      target: targets[0][1],
      root,
      out,
      headers: [path.join(root, "probe.h")],
      includes: [root],
    }),
    (error: NodeJS.ErrnoException & { code?: number; stderr?: string }) => {
      assert.equal(error.code, 2);
      assert.match(error.stderr ?? "", /cannot replace/);
      return true;
    },
  );
  assert.equal(readFileSync(path.join(out, "sentinel"), "utf8"), "preserved\n");
});

test("the ARM64 host extractor links when the VS ARM64 libclang is installed", async (t) => {
  if (unavailable) return t.skip(unavailable);
  if (process.platform !== "win32") return t.skip("the extractor is a Windows tool");
  try {
    const extractor = await buildExtractor("arm64");
    assert.match(extractor.exe, /arm64-/);
  } catch (error) {
    if (/not found|vcvarsall/.test((error as Error).message)) {
      return t.skip((error as Error).message);
    }
    throw error;
  }
});
