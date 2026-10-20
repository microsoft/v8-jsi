// Embedded manifests without a toolchain (design §12): building one from a
// model, its deterministic encoding, decoding untrusted bytes, the PE reader
// on synthetic and corrupted images, and the export rules of `verify`.

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import path from "node:path";
import { test } from "node:test";
import { gunzipSync, gzipSync } from "node:zlib";
import type { BinaryConfig } from "../src/config.ts";
import { libraryModel } from "../src/diff.ts";
import {
  type EmbeddedBinary,
  type Manifest,
  MAX_MANIFEST_BYTES,
  buildManifest,
  decodeManifest,
  embedFiles,
  encodeManifest,
  manifestModel,
  modelSha256,
  validateManifest,
  verifyExports,
} from "../src/embed.ts";
import { StelaError } from "../src/errors.ts";
import { type Model, readModel, serializeModel } from "../src/model.ts";
import { MAX_RESOURCE_BYTES, parsePe } from "../src/pe.ts";
import { validateJson } from "../src/schema.ts";

const X64 = "x86_64-pc-windows-msvc";
const X86 = "i686-pc-windows-msvc";
const ARM64 = "aarch64-pc-windows-msvc";
const golden = path.join(import.meta.dirname, "golden", "model-demo.json");
const model: Model = readModel(golden);

const binary = (extra: Partial<BinaryConfig> = {}): BinaryConfig => ({
  name: "demo.dll",
  libraries: ["demo", "demo_ext"],
  exports: "all",
  otherExports: true,
  ...extra,
});

const declarationNames = (m: Manifest, library: string) =>
  m.libraries.find((l) => l.name === library)!.declarations.map((d) => `${d.kind} ${d.name}`);

test("a manifest keeps the binary's targets and records hashes, versions, and exports", () => {
  const m = buildManifest(model, binary(), [X64]);
  assert.equal(m.schema, "stela.embedded/1");
  assert.deepEqual(m.targets, [X64]);
  assert.equal(m.binary, "demo.dll");
  assert.equal(m.otherExports, true);
  assert.deepEqual(m.producer, model.producer);
  const [demo, ext] = m.libraries;
  assert.equal(demo.name, "demo");
  assert.equal(demo.targets, undefined);
  assert.deepEqual(demo.exports, ["demo_open"]);
  assert.deepEqual(demo.abiVersions, { DEMO_ABI_VERSION: "3" });
  assert.deepEqual(ext.exports, ["demo_ext_query"]);
  assert.deepEqual(ext.abiVersions, {}, "DEMO_EXT_VERSION isn't an *_ABI_VERSION macro");
  // The hash is the one of the library's published model file.
  const published = serializeModel(libraryModel(model, "demo"));
  assert.equal(demo.modelSha256, createHash("sha256").update(published, "utf8").digest("hex"));
  assert.equal(demo.modelSha256, modelSha256(model, "demo"));
  assert.notEqual(ext.modelSha256, demo.modelSha256);
  // Only the binary's target's facts remain.
  for (const library of m.libraries) {
    for (const d of library.declarations) {
      assert.equal(d.targets, undefined, `${d.name} has targets`);
      for (const t of Object.keys(d.perTarget ?? {})) assert.equal(t, X64, `${d.name} keeps ${t}`);
    }
  }
  validateManifest(JSON.parse(JSON.stringify(m)));
});

test("a 32-bit manifest drops 64-bit-only declarations; the hash still covers every target", () => {
  const m = buildManifest(model, binary({ libraries: ["demo"] }), [X86]);
  assert.ok(declarationNames(buildManifest(model, binary({ libraries: ["demo"] }), [X64]), "demo").includes("struct demo_wide"));
  assert.ok(!declarationNames(m, "demo").includes("struct demo_wide"));
  assert.equal(m.libraries[0].modelSha256, modelSha256(model, "demo"));

  // A two-target binary keeps the per-declaration targets that differ.
  const both = buildManifest(model, binary({ libraries: ["demo"] }), [ARM64, X86]);
  assert.deepEqual(both.targets, [X86, ARM64], "targets keep the model's order");
  const wide = both.libraries[0].declarations.find((d) => d.name === "demo_wide")!;
  assert.deepEqual(wide.targets, [ARM64]);
  assert.deepEqual(Object.keys(wide.perTarget ?? {}), [ARM64]);

  // demo_ext isn't built for x86, so it gets its own targets on a multi-target binary...
  const multi = buildManifest(model, binary(), [X64, X86]);
  assert.deepEqual(multi.libraries[1].targets, [X64]);
  validateManifest(JSON.parse(JSON.stringify(multi)));
  // ...and a binary for x86 alone can't implement it.
  assert.throws(() => buildManifest(model, binary(), [X86]), /demo_ext.*isn't built for/);
});

test("exports: none, and bad requests", () => {
  const m = buildManifest(model, binary({ name: "demo.exe", libraries: ["demo"], exports: "none" }), [ARM64]);
  assert.deepEqual(m.libraries[0].exports, []);
  assert.throws(() => buildManifest(model, binary(), []), StelaError);
  assert.throws(() => buildManifest(model, binary(), ["riscv64-unknown-linux"]), /no target/);
  assert.throws(() => buildManifest(model, binary(), [X64, X64]), /twice/);
  assert.throws(() => buildManifest(model, binary({ libraries: ["nope"] }), [X64]), /missing from the model/);
});

test("the encoding is deterministic gzip that standard tools read", () => {
  const m = buildManifest(model, binary(), [X64]);
  const a = encodeManifest(m);
  const b = encodeManifest(JSON.parse(JSON.stringify(m)));
  assert.ok(a.equals(b));
  assert.deepEqual([...a.subarray(0, 10)], [0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 2, 0xff]);
  assert.equal(gunzipSync(a).toString("utf8"), JSON.stringify(m));
  assert.deepEqual(decodeManifest(a), m);

  const [gz, rc] = embedFiles(m);
  assert.equal(gz.path, "demo.dll.stela.gz");
  assert.ok(gz.bytes.equals(a));
  assert.equal(rc.path, "demo.dll.stela.rc");
  assert.equal(rc.bytes.toString("utf8"), '// Generated by stela gen embed; do not edit.\n1 STELA "demo.dll.stela.gz"\n');
});

test("a manifest model compares equal to the model it came from", () => {
  const m = buildManifest(model, binary({ libraries: ["demo"] }), [X64, X86, ARM64]);
  const back = manifestModel(m);
  assert.equal(back.schema, model.schema);
  assert.deepEqual(back.libraries[0], model.libraries.find((l) => l.name === "demo"));
});

test("decoding rejects bytes that aren't a valid manifest", () => {
  const m = buildManifest(model, binary(), [X64]);
  const gz = (value: unknown) => gzipSync(Buffer.from(typeof value === "string" ? value : JSON.stringify(value)));
  const rejects = (bytes: Buffer, pattern: RegExp) => assert.throws(() => decodeManifest(bytes), (e: unknown) => {
    assert.ok(e instanceof StelaError, String(e));
    assert.match(e.message, pattern);
    return true;
  });

  rejects(Buffer.from("not gzip at all, but long enough"), /not gzip/);
  rejects(encodeManifest(m).subarray(0, 40), /can't be decoded/);
  rejects(gz("{"), /can't be decoded/);
  rejects(gz({ ...m, schema: "stela.embedded/2" }), /isn't stela\.embedded\/1/);
  rejects(gz({ ...m, extra: 1 }), /extra/);
  rejects(gz({ ...m, targets: ["x64"] }), /target x86_64-pc-windows-msvc, which the library doesn't cover/);
  const lib = (patch: Record<string, unknown>) => gz({ ...m, libraries: [{ ...m.libraries[0], ...patch }] });
  rejects(lib({ modelSha256: "ABC" }), /modelSha256/);
  rejects(lib({ abiVersions: { DEMO_ABI_VERSION: 3 } }), /abiVersions/);
  rejects(lib({ exports: ["demo_missing"] }), /export demo_missing isn't one of its functions/);
  rejects(lib({ exports: ["demo_open", "demo_open"] }), /exports/);
  rejects(lib({ targets: [ARM64] }), /target aarch64-pc-windows-msvc isn't one of the binary's/);
  rejects(gz({ ...m, libraries: [m.libraries[0], m.libraries[0]] }), /listed twice/);
  const declarations = m.libraries[0].declarations;
  rejects(lib({ declarations: [...declarations, declarations[0]] }), /declared twice/);
  rejects(lib({ declarations: [{ ...declarations[0], targets: [ARM64] }] }), /doesn't cover/);
  rejects(lib({ declarations: [{ ...declarations[0], kind: "class" }] }), /kind/);

  // Deep nesting is an error, not a stack overflow.
  rejects(gz(`{"schema":"stela.embedded/1","producer":${"[".repeat(100000)}${"]".repeat(100000)}}`), /./);
});

test("decoding stops at the size limit (gzip bombs)", () => {
  const bomb = gzipSync(Buffer.alloc(MAX_MANIFEST_BYTES + 1, 0x20), { level: 9 });
  assert.ok(bomb.length < 64 * 1024);
  assert.throws(() => decodeManifest(bomb), /decompresses to more than/);
});

test("the validator agrees with the model schema on the golden model", () => {
  validateJson(JSON.parse(readFileSync(golden, "utf8")), "stela.model.schema.json", "golden");
  assert.throws(
    () => validateJson({ schema: "stela.model/1", producer: {}, targets: [], libraries: [] }, "stela.model.schema.json", "x"),
    /x: .*producer/,
  );
});

// --- A synthetic PE image ---------------------------------------------------

interface PeSpec {
  machine?: number;
  pe32?: boolean;
  exports?: string[];
  /** Repeat the name table's pointers this many times (names sharing bytes). */
  exportRepeat?: number;
  resource?: { type: string; id: number; data: Buffer };
}

/**
 * A PE image with one section at RVA 0x1000 holding an export directory and a
 * three-level resource tree, laid out the way link.exe does.
 */
function buildPe(spec: PeSpec): Buffer {
  const pe32 = spec.pe32 ?? false;
  const optionalSize = (pe32 ? 96 : 112) + 16 * 8;
  const sectionVa = 0x1000;
  const fileAlign = 0x200;
  const data: Buffer[] = [];
  let size = 0;
  const put = (bytes: Buffer): number => {
    const at = size;
    data.push(bytes);
    size += bytes.length;
    const pad = (4 - (size % 4)) % 4;
    if (pad) {
      data.push(Buffer.alloc(pad));
      size += pad;
    }
    return at;
  };
  const u32s = (...values: number[]) => {
    const b = Buffer.alloc(values.length * 4);
    values.forEach((v, i) => b.writeUInt32LE(v >>> 0, i * 4));
    return b;
  };

  let resourceRva = 0;
  let resourceSize = 0;
  if (spec.resource) {
    // Directory offsets are relative to the resource root; it comes first.
    const name = Buffer.alloc(2 + spec.resource.type.length * 2);
    name.writeUInt16LE(spec.resource.type.length, 0);
    name.write(spec.resource.type, 2, "utf16le");
    const dir = (named: number, ids: number) => {
      const b = Buffer.alloc(16);
      b.writeUInt16LE(named, 12);
      b.writeUInt16LE(ids, 14);
      return b;
    };
    // root(16+8) type-dir(16+8) lang-dir(16+8) data-entry(16) name data
    const root = 0;
    const typeDir = 24;
    const langDir = 48;
    const entry = 72;
    const nameAt = 88;
    const dataAt = nameAt + ((name.length + 3) & ~3);
    const tree = Buffer.concat([
      dir(1, 0),
      u32s(0x80000000 | nameAt, 0x80000000 | typeDir),
      dir(0, 1),
      u32s(spec.resource.id, 0x80000000 | langDir),
      dir(0, 1),
      u32s(0x409, entry),
      u32s(sectionVa + dataAt, spec.resource.data.length, 0, 0),
      name,
      Buffer.alloc(dataAt - nameAt - name.length),
      spec.resource.data,
    ]);
    assert.equal(root, put(tree));
    resourceRva = sectionVa;
    resourceSize = tree.length;
  }

  let exportRva = 0;
  if (spec.exports) {
    const unique = spec.exports.map((n) => put(Buffer.from(`${n}\0`, "latin1")) + sectionVa);
    const names = Array.from({ length: spec.exportRepeat ?? 1 }, () => unique).flat();
    const table = put(u32s(...names)) + sectionVa;
    const directory = Buffer.alloc(40);
    directory.writeUInt32LE(names.length, 24);
    directory.writeUInt32LE(table, 32);
    exportRva = put(directory) + sectionVa;
  }

  const raw = Buffer.concat([...data, Buffer.alloc((fileAlign - (size % fileAlign)) % fileAlign)]);
  const headers = Buffer.alloc(fileAlign);
  headers.write("MZ", 0, "latin1");
  headers.writeUInt32LE(0x40, 0x3c);
  headers.write("PE\0\0", 0x40, "latin1");
  const coff = 0x44;
  headers.writeUInt16LE(spec.machine ?? 0x8664, coff);
  headers.writeUInt16LE(1, coff + 2);
  headers.writeUInt16LE(optionalSize, coff + 16);
  const optional = coff + 20;
  headers.writeUInt16LE(pe32 ? 0x10b : 0x20b, optional);
  const directories = optional + (pe32 ? 96 : 112);
  headers.writeUInt32LE(16, directories - 4);
  headers.writeUInt32LE(exportRva, directories);
  headers.writeUInt32LE(exportRva ? 40 : 0, directories + 4);
  headers.writeUInt32LE(resourceRva, directories + 16);
  headers.writeUInt32LE(resourceSize, directories + 20);
  const section = optional + optionalSize;
  headers.write(".rdata", section, "latin1");
  headers.writeUInt32LE(size, section + 8);
  headers.writeUInt32LE(sectionVa, section + 12);
  headers.writeUInt32LE(raw.length, section + 16);
  headers.writeUInt32LE(fileAlign, section + 20);
  return Buffer.concat([headers, raw]);
}

const payload = Buffer.from("payload bytes");

test("the PE reader finds the machine, exports, and a named resource", () => {
  for (const pe32 of [false, true]) {
    const pe = parsePe(buildPe({ pe32, machine: pe32 ? 0x14c : 0xaa64, exports: ["a_one", "a_two"], resource: { type: "STELA", id: 1, data: payload } }));
    assert.equal(pe.machine, pe32 ? 0x14c : 0xaa64);
    assert.deepEqual(pe.exports, ["a_one", "a_two"]);
    assert.ok(pe.resource("STELA", 1)?.equals(payload));
    assert.ok(pe.resource("stela", 1)?.equals(payload), "resource types match case-insensitively");
    assert.equal(pe.resource("STELA", 2), undefined);
    assert.equal(pe.resource("OTHER", 1), undefined);
  }
  const bare = parsePe(buildPe({}));
  assert.deepEqual(bare.exports, []);
  assert.equal(bare.resource("STELA", 1), undefined);
});

test("the PE reader rejects malformed images with a StelaError", () => {
  const good = buildPe({ exports: ["a_one"], resource: { type: "STELA", id: 1, data: payload } });
  const rejects = (bytes: Buffer, pattern: RegExp) =>
    assert.throws(() => {
      const pe = parsePe(bytes, "x.dll");
      pe.resource("STELA", 1);
    }, (e: unknown) => {
      assert.ok(e instanceof StelaError, String(e));
      assert.match(e.message, pattern);
      return true;
    });
  rejects(Buffer.from("ZM"), /no MZ signature/);
  const patched = (offset: number, value: number) => {
    const b = Buffer.from(good);
    b.writeUInt32LE(value >>> 0, offset);
    return b;
  };
  rejects(patched(0x3c, 0xfffffff0), /PE signature is out of bounds/);
  rejects(patched(0x40, 0), /no PE signature/);
  rejects(patched(0x58, 0x30b), /optional header magic/);
  // The export directory points outside every section.
  rejects(patched(0x58 + 112, 0x900000), /in no section/);
  // A resource larger than the limit is refused before it's copied.
  const dataEntry = 0x200 + 72 + 4;
  rejects(patched(dataEntry, MAX_RESOURCE_BYTES + 1), /larger than/);
  rejects(patched(dataEntry, 0x10000), /runs past its section's data/);
  // Every pointer names one long string: the decoded total is capped.
  rejects(buildPe({ exports: ["x".repeat(4000)], exportRepeat: 5000 }), /export names exceed/);
  rejects(buildPe({ exports: ["x".repeat(4097)] }), /export name isn't terminated/);
});

test("the PE reader survives truncation and corruption", () => {
  const good = buildPe({ exports: ["a_one", "a_two"], resource: { type: "STELA", id: 1, data: payload } });
  const tolerated = (bytes: Buffer) => {
    try {
      const pe = parsePe(bytes);
      pe.resource("STELA", 1);
    } catch (e) {
      if (!(e instanceof StelaError)) throw e;
    }
  };
  for (let length = 0; length < good.length; length += 7) tolerated(good.subarray(0, length));
  let seed = 0x5eed;
  const random = () => ((seed = (seed * 1103515245 + 12345) >>> 0) >>> 8) / 0x1000000;
  for (let i = 0; i < 3000; i++) {
    const b = Buffer.from(good);
    for (let k = 0; k < 4; k++) b[Math.floor(random() * b.length)] = Math.floor(random() * 256);
    tolerated(b);
  }
});

// --- verify -------------------------------------------------------------------

const embedded = (extra: Partial<BinaryConfig>, exports: string[]): EmbeddedBinary => ({
  file: "demo.dll",
  machine: 0x8664,
  target: X64,
  exports,
  manifest: buildManifest(model, binary(extra), [X64]),
});

test("verify: listed exports must exist; library-prefixed extras never pass", () => {
  assert.deepEqual(verifyExports(embedded({}, ["demo_open", "demo_ext_query", "DllMain", "other"])), {
    missing: [],
    unexpected: [],
  });
  assert.deepEqual(verifyExports(embedded({}, ["demo_open"])), { missing: ["demo_ext_query"], unexpected: [] });
  assert.deepEqual(verifyExports(embedded({}, ["demo_open", "demo_ext_query", "demo_secret", "demo_ext_x"])), {
    missing: [],
    unexpected: ["demo_secret", "demo_ext_x"],
  });
  assert.deepEqual(verifyExports(embedded({ otherExports: false }, ["demo_open", "demo_ext_query", "other"])), {
    missing: [],
    unexpected: ["other"],
  });
  // A binary that implements the API without exporting it.
  assert.deepEqual(verifyExports(embedded({ exports: "none", otherExports: false }, [])), { missing: [], unexpected: [] });
  assert.deepEqual(verifyExports(embedded({ exports: "none", otherExports: false }, ["demo_open"])), {
    missing: [],
    unexpected: ["demo_open"],
  });
});
