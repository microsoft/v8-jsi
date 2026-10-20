// stela.model/1 merging and serialization (design §7), from synthetic raw
// models: no toolchain needed.

import assert from "node:assert/strict";
import path from "node:path";
import { test } from "node:test";
import { parseConfig } from "../src/config.ts";
import { StelaError } from "../src/errors.ts";
import {
  type RawModel,
  buildModel,
  canonical,
  defaultModelPath,
  firstDifference,
  formatMismatch,
  normalizeDeclaration,
  serializeModel,
} from "../src/model.ts";

const X64 = "x86_64-pc-windows-msvc";
const X86 = "i686-pc-windows-msvc";
const ARM64 = "aarch64-pc-windows-msvc";
const file = path.resolve("/repo/stela.config.json");
const root = path.dirname(file);

const config = (library: Record<string, unknown> = {}, targets = [X64, X86, ARM64]) =>
  parseConfig(
    {
      schema: "stela.config/1",
      targets,
      libraries: [{ name: "demo", prefix: "demo", macroPrefix: "DEMO", headers: ["include/demo.h"], ...library }],
    },
    file,
  );

const at = (line: number, column = 1, header = "include/demo.h") => ({ file: header, line, column });

function decl(kind: string, name: string, line: number, extra: Record<string, unknown> = {}) {
  return { kind, name, location: at(line), doc: null, annotations: [], ...extra };
}

function record(name: string, line: number, size: number, offsets: number[], extra: Record<string, unknown> = {}) {
  return (target: string) =>
    decl("struct", name, line, {
      fields: offsets.map((_, i) => ({ name: `f${i}`, type: { name: "uint32_t" }, annotations: [], doc: null })),
      perTarget: { [target]: { size, align: 4, offsets } },
      ...extra,
    });
}

type Make = (target: string) => Record<string, unknown>;

function raws(byTarget: Record<string, Make[]>): Map<string, Map<string, RawModel>> {
  const map = new Map<string, RawModel>();
  for (const [target, makers] of Object.entries(byTarget)) {
    map.set(target, {
      schema: "stela.raw/1",
      producer: { stela: "0.1.0", libclang: "clang 22" },
      target,
      headers: ["include/demo.h"],
      declarations: makers.map((m) => m(target)) as RawModel["declarations"],
    });
  }
  return new Map([["demo", map]]);
}

const macro = (name: string, line: number, value?: string): Make => (target) =>
  decl("macro", name, line, {
    functionLike: false,
    replacement: value === undefined ? [] : [`${value}u`],
    ...(value === undefined ? {} : { perTarget: { [target]: { value } } }),
  });

test("identical targets merge into one declaration with per-target facts", () => {
  const all = [macro("DEMO_ABI_VERSION", 3, "3"), record("demo_point", 5, 8, [0, 4])];
  const { model, mismatches } = buildModel(config(), raws({ [X64]: all, [X86]: all, [ARM64]: all }));
  assert.deepEqual(mismatches, []);
  assert.equal(model.schema, "stela.model/1");
  assert.deepEqual(model.targets, [X64, X86, ARM64]);
  const [library] = model.libraries;
  assert.deepEqual(Object.keys(library), [
    "name",
    "prefix",
    "macroPrefix",
    "status",
    "headers",
    "includeDirs",
    "defines",
    "stelaHeader",
    "declarations",
  ]);
  assert.equal(library.status, "demo_status");
  assert.deepEqual(library.headers, ["include/demo.h"]);
  // An entry header's own directory is an include directory when none holds it.
  assert.deepEqual(library.includeDirs, ["include"]);
  assert.equal(library.stelaHeader, "include/demo_stela.h");
  // A stela header elsewhere adds its directory; an extension library records none.
  const elsewhere = buildModel(config({ stelaHeader: "abi/demo_stela.h" }), raws({ [X64]: all, [X86]: all, [ARM64]: all }));
  assert.equal(elsewhere.model.libraries[0].stelaHeader, "abi/demo_stela.h");
  assert.deepEqual(elsewhere.model.libraries[0].includeDirs, ["include", "abi"]);
  const extension = buildModel(config({ stelaHeader: false }), raws({ [X64]: all, [X86]: all, [ARM64]: all }));
  assert.equal("stelaHeader" in extension.model.libraries[0], false);
  assert.deepEqual(extension.model.libraries[0].includeDirs, ["include"]);
  const [version, point] = library.declarations;
  assert.equal(version.targets, undefined);
  assert.deepEqual(Object.keys(version.perTarget!), [X64, X86, ARM64]);
  assert.equal(version.perTarget![X86].value, "3");
  assert.deepEqual(point.perTarget![ARM64], { size: 8, align: 4, offsets: [0, 4] });
});

test("per-target facts may differ; the declaration itself may not", () => {
  const ok = buildModel(
    config(),
    raws({ [X64]: [record("demo_s", 5, 16, [0, 8])], [X86]: [record("demo_s", 5, 8, [0, 4])], [ARM64]: [record("demo_s", 5, 16, [0, 8])] }),
  );
  assert.deepEqual(ok.mismatches, []);
  assert.equal(ok.model.libraries[0].declarations[0].perTarget![X86].size, 8);

  const renamed: Make = (target) => {
    const d = record("demo_s", 5, 8, [0, 4])(target) as unknown as { fields: { name: string }[] };
    d.fields[1].name = "other";
    return d;
  };
  const { mismatches } = buildModel(
    config(),
    raws({ [X64]: [record("demo_s", 5, 8, [0, 4])], [X86]: [renamed], [ARM64]: [record("demo_s", 5, 8, [0, 4])] }),
  );
  assert.equal(mismatches.length, 1);
  assert.deepEqual(mismatches[0], {
    library: "demo",
    kind: "struct",
    name: "demo_s",
    targets: [X64, X86],
    at: "fields[1].name",
  });
  assert.equal(
    formatMismatch(mismatches[0]),
    `target_mismatch: demo: struct demo_s differs between ${X64} and ${X86} at fields[1].name`,
  );
});

test("a declaration on some targets lists them, in the library's order", () => {
  const wide = record("demo_wide", 9, 16, [0, 8]);
  const { model } = buildModel(config(), raws({ [ARM64]: [wide], [X86]: [], [X64]: [wide] }));
  const [d] = model.libraries[0].declarations;
  assert.deepEqual(d.targets, [X64, ARM64]);
  assert.deepEqual(Object.keys(d.perTarget!), [X64, ARM64]);
});

test("a library with fewer targets than the model lists its own", () => {
  const c = config({ targets: [ARM64, X64] });
  const { model } = buildModel(c, raws({ [X64]: [], [ARM64]: [] }));
  assert.deepEqual(model.libraries[0].targets, [X64, ARM64]);
  assert.deepEqual(Object.keys(model.libraries[0]).slice(-2), ["targets", "declarations"]);
  assert.throws(() => buildModel(c, raws({ [X64]: [] })), /no extraction for aarch64/);
});

test("negative sizes: incomplete records, and typedefs without a layout", () => {
  const opaque = normalizeDeclaration(
    decl("struct", "demo_session_s", 4, { fields: [], perTarget: { [X64]: { size: -2, align: -2, offsets: [] } } }),
    X64,
  );
  assert.deepEqual(opaque, { kind: "struct", name: "demo_session_s", location: at(4), doc: null, annotations: [], fields: [], incomplete: true });
  const alias = normalizeDeclaration(
    decl("typedef", "demo_session_t", 5, {
      type: { name: "demo_session_s" },
      perTarget: { [X64]: { canonicalType: { name: "demo_session_s" }, size: -2, align: -2 } },
    }),
    X64,
  );
  assert.deepEqual(alias.perTarget, { [X64]: { canonicalType: { name: "demo_session_s" } } });
  assert.throws(() => normalizeDeclaration(decl("struct", "s", 1, { incomplete: true }), X64), StelaError);
  assert.throws(
    () => normalizeDeclaration(decl("macro", "M", 1, { functionLike: false, perTarget: { [X86]: { value: "1" } } }), X64),
    /exactly the target/,
  );
});

test("declarations sort by header, line, and column; ties keep extractor order", () => {
  const ext = (name: string, line: number): Make => () => ({ ...decl("macro", name, line, { functionLike: false }), location: at(line, 1, "include/other.h") });
  const c = config({ headers: ["include/demo.h", "include/other.h"] });
  const order: Make[] = [
    ext("OTHER", 1),
    macro("LATE", 9),
    () => decl("typedef", "demo_status", 7, { type: { name: "demo_status" } }),
    () => decl("enum", "demo_status", 7, { type: { name: "int32_t" }, enumerators: [] }),
    () => ({ ...decl("macro", "B", 7, { functionLike: false }), location: at(7, 12) }),
  ];
  const { model } = buildModel(c, raws({ [X64]: order, [X86]: order, [ARM64]: order }));
  assert.deepEqual(
    model.libraries[0].declarations.map((d) => `${d.kind} ${d.name}`),
    ["typedef demo_status", "enum demo_status", "macro B", "macro LATE", "macro OTHER"],
  );
});

test("canonical key order doesn't depend on the input's", () => {
  const shuffled = {
    perTarget: { [X64]: { offsets: [0], align: 4, size: 4 } },
    fields: [{ doc: null, annotations: [], type: { name: "uint32_t" }, name: "x" }],
    annotations: [],
    doc: null,
    location: { column: 1, line: 2, file: "include/demo.h" },
    name: "demo_x",
    kind: "struct",
  };
  const ordered = canonical(shuffled, "declaration");
  assert.equal(
    JSON.stringify(ordered),
    JSON.stringify(record("demo_x", 2, 4, [0])(X64)).replace('"f0"', '"x"'),
  );
  assert.throws(() => canonical({ ...shuffled, extra: 1 }, "declaration"), /unexpected key "extra"/);
  assert.throws(() => canonical({ ...shuffled, location: { ...shuffled.location, path: "x" } }, "declaration"), /location/);
});

test("duplicates within one target and libclang disagreements are errors", () => {
  const point = record("demo_point", 5, 8, [0, 4]);
  assert.throws(() => buildModel(config(), raws({ [X64]: [point, point], [X86]: [point], [ARM64]: [point] })), /declared twice|twice/);
  const mixed = raws({ [X64]: [], [X86]: [], [ARM64]: [] });
  mixed.get("demo")!.get(X86)!.producer.libclang = "clang 21";
  assert.throws(() => buildModel(config(), mixed), /libclang/);
  const wrong = raws({ [X64]: [], [X86]: [], [ARM64]: [] });
  wrong.get("demo")!.get(X86)!.target = X64;
  assert.throws(() => buildModel(config(), wrong), /raw model is for/);
});

test("firstDifference names the first differing path", () => {
  assert.equal(firstDifference({ a: [1, { b: 2 }] }, { a: [1, { b: 2 }] }), undefined);
  assert.equal(firstDifference({ a: [1, { b: 2 }] }, { a: [1, { b: 3 }] }), "a[1].b");
  assert.equal(firstDifference({ a: [1] }, { a: [1, 2] }), "a[1]");
  assert.equal(firstDifference({ a: 1 }, { b: 1 }), "a");
});

test("serialization: compact short containers, a final newline, deterministic", () => {
  const all = [record("demo_point", 5, 8, [0, 4])];
  const { model } = buildModel(config(), raws({ [X64]: all, [X86]: all, [ARM64]: all }));
  const text = serializeModel(model);
  assert.ok(text.endsWith("}\n"));
  assert.ok(!text.includes("\r"));
  assert.match(text, /^ {10}"location": \{"file": "include\/demo.h", "line": 5, "column": 1\},$/m);
  assert.deepEqual(JSON.parse(text), model);
  const again = buildModel(config(), raws({ [X64]: all, [X86]: all, [ARM64]: all })).model;
  assert.equal(serializeModel(again), text);
});

test("the default model path is out/stela/<config name>.model.json", () => {
  assert.equal(defaultModelPath(file), path.join(root, "out", "stela", "stela.model.json"));
  assert.equal(defaultModelPath(path.join(root, "demo.config.json")), path.join(root, "out", "stela", "demo.model.json"));
});
