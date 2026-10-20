// `stela extract` end to end (design §7, §8.1): the fixture library and its
// extension extract for x64, x86, and ARM64 and merge into a golden
// stela.model/1.

import assert from "node:assert/strict";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { before, test } from "node:test";
import { findToolchain } from "../src/toolchain.ts";
import { modelFixture, runCli } from "./helpers.ts";

const golden = path.join(import.meta.dirname, "golden", "model-demo.json");
const modelPath = (root: string) => path.join(root, "out", "stela", "stela.model.json");
let unavailable: string | undefined;

before(async () => {
  const toolchain = await findToolchain();
  if (typeof toolchain === "string") unavailable = toolchain;
});

test("extract writes the golden model, then leaves it alone", (t) => {
  if (unavailable) return t.skip(unavailable);
  const root = modelFixture(t, "extract-model");
  let r = runCli(root, "extract");
  assert.equal(r.code, 0, r.err);
  assert.equal(r.out, "wrote:     out/stela/stela.model.json (2 libraries, 3 targets, 31 declarations)\n");
  assert.equal(readFileSync(modelPath(root), "utf8"), readFileSync(golden, "utf8"));

  r = runCli(root, "extract");
  assert.equal(r.code, 0, r.err);
  assert.match(r.out, /^unchanged: out\/stela\/stela\.model\.json /);

  r = runCli(root, "extract", "--out", "other/demo.json");
  assert.equal(r.code, 0, r.err);
  assert.equal(readFileSync(path.join(root, "other", "demo.json"), "utf8"), readFileSync(golden, "utf8"));
});

test("the model doesn't depend on where the tree is", (t) => {
  if (unavailable) return t.skip(unavailable);
  const a = modelFixture(t, "extract-root-a");
  const b = modelFixture(t, "extract-root-b-with-a-longer-name");
  assert.equal(runCli(a, "extract").code, 0);
  assert.equal(runCli(b, "extract").code, 0);
  assert.equal(readFileSync(modelPath(a), "utf8"), readFileSync(modelPath(b), "utf8"));
});

test("the model records what the generators and later stages need", (t) => {
  if (unavailable) return t.skip(unavailable);
  const model = JSON.parse(readFileSync(golden, "utf8"));
  const [demo, ext] = model.libraries;
  const find = (lib: { declarations: { kind: string; name: string }[] }, kind: string, name: string) =>
    lib.declarations.find((d) => d.kind === kind && d.name === name) as Record<string, unknown> | undefined;

  assert.deepEqual(demo.includeDirs, ["include"]);
  assert.equal(demo.targets, undefined);
  assert.deepEqual(ext.targets, ["x86_64-pc-windows-msvc", "aarch64-pc-windows-msvc"]);
  assert.deepEqual(ext.defines, ["DEMO_EXT_EXPERIMENTAL"]);
  assert.equal(ext.status, "demo_status");
  // An extension owns only its own header.
  assert.ok(ext.declarations.every((d: { location: { file: string } }) => d.location.file === "include/demo_ext.h"));

  assert.equal(find(demo, "struct", "demo_session_s")?.incomplete, true);
  assert.equal(find(demo, "struct", "demo_session_s")?.perTarget, undefined);
  assert.equal(find(demo, "struct", "demo_point")?.tagless, true);
  assert.equal(find(demo, "struct", "demo_options")?.tagless, undefined);
  // A tag a macro spells is still a tag.
  assert.equal(find(demo, "struct", "demo_tagged_s")?.tagless, undefined);
  // Anonymous field types are written inline, not as Clang's "(unnamed at <path>)".
  const slot = find(demo, "struct", "demo_slot")?.fields as { name: string; type: Record<string, unknown> }[];
  assert.deepEqual(
    slot.map((f) => Object.keys(f.type)[0]),
    ["name", "union", "enum"],
  );
  assert.doesNotMatch(readFileSync(golden, "utf8"), /unnamed|anonymous at/);
  assert.equal(find(demo, "macro", "DEMO_TAG")?.functionLike, true);
  assert.deepEqual(find(demo, "struct", "demo_wide")?.targets, ["x86_64-pc-windows-msvc", "aarch64-pc-windows-msvc"]);
  const options = find(demo, "struct", "demo_options")?.perTarget as Record<string, { size: number; offsets: number[] }>;
  assert.deepEqual(options["i686-pc-windows-msvc"], { size: 24, align: 8, offsets: [0, 4, 8, 16] });
  const bits = find(demo, "struct", "demo_bits")?.fields as { name: string; bitWidth?: number }[];
  assert.deepEqual(bits.map((f) => f.bitWidth), [undefined, 1, 3, undefined]);
});

test("a declaration that differs between targets fails with target_mismatch", (t) => {
  if (unavailable) return t.skip(unavailable);
  const root = modelFixture(t, "extract-mismatch");
  const header = path.join(root, "include", "demo.h");
  writeFileSync(
    header,
    readFileSync(header, "utf8").replace(
      /  uint64_t budget;\r?\n/,
      "  uint64_t budget;\n#ifdef _M_IX86\n  uint32_t x86_only;\n#endif\n",
    ),
  );
  const r = runCli(root, "extract");
  assert.equal(r.code, 1, r.err);
  assert.match(
    r.err,
    /^target_mismatch: demo: struct demo_options differs between x86_64-pc-windows-msvc and i686-pc-windows-msvc at fields\[3\]\.name$/m,
  );
  assert.match(r.err, /no model written/);
  assert.equal(existsSync(modelPath(root)), false);
});

test("extraction and usage errors exit with 2", (t) => {
  if (unavailable) return t.skip(unavailable);
  const root = modelFixture(t, "extract-errors");
  writeFileSync(path.join(root, "include", "demo_ext.h"), "#include \"demo.h\"\nint broken(;\n");
  let r = runCli(root, "extract");
  assert.equal(r.code, 2);
  assert.match(r.err, /extracting demo_ext for \S+ failed/);
  assert.equal(existsSync(modelPath(root)), false);

  r = runCli(root, "extract", "--bogus");
  assert.equal(r.code, 2);
  const config = JSON.parse(readFileSync(path.join(root, "stela.config.json"), "utf8"));
  delete config.targets;
  delete config.libraries[1].targets;
  writeFileSync(path.join(root, "stela.config.json"), JSON.stringify(config));
  r = runCli(root, "extract");
  assert.equal(r.code, 2);
  assert.match(r.err, /"targets" is required/);
});
