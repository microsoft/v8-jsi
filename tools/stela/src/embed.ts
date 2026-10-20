// Metadata embedded in binaries (design §12): the stela.embedded/1 manifest
// of one binary, its deterministic gzip form and `.rc` file (`gen embed`),
// and reading it back from an untrusted binary.

import { createHash } from "node:crypto";
import { closeSync, openSync, readFileSync, readSync } from "node:fs";
import { crc32, deflateRawSync, gunzipSync } from "node:zlib";
import type { BinaryConfig } from "./config.ts";
import { libraryModel } from "./diff.ts";
import { StelaError } from "./errors.ts";
import { type Declaration, type Model, type ModelLibrary, MODEL_SCHEMA, canonical, readModel, serializeModel } from "./model.ts";
import { MACHINE_TARGETS, isPe, parsePe } from "./pe.ts";
import { validateJson } from "./schema.ts";

export const EMBEDDED_SCHEMA = "stela.embedded/1";
/** The resource that carries the manifest: type `STELA`, ID 1. */
export const RESOURCE_TYPE = "STELA";
export const RESOURCE_ID = 1;
/** The largest manifest a reader decompresses. */
export const MAX_MANIFEST_BYTES = 16 * 1024 * 1024;

const VERSION_RE = /_ABI_VERSION$/;

export interface EmbeddedLibrary extends ModelLibrary {
  /** SHA-256 of the library's published model (`libraryModel` serialized). */
  modelSha256: string;
  /** The library's `*_ABI_VERSION` macros and their values. */
  abiVersions: Record<string, string>;
  /** The library's functions the binary exports. */
  exports: string[];
}

export interface Manifest {
  schema: string;
  producer: { stela: string; libclang: string };
  binary: string;
  targets: string[];
  otherExports: boolean;
  libraries: EmbeddedLibrary[];
}

export interface EmbeddedBinary {
  readonly file: string;
  readonly machine: number;
  /** The machine's target triple. */
  readonly target: string;
  readonly exports: readonly string[];
  readonly manifest: Manifest;
}

function libraryTargets(model: Model, library: ModelLibrary): readonly string[] {
  return library.targets ?? model.targets;
}

/** SHA-256 of one library's published model: what `stela baseline` writes and the packages ship. */
export function modelSha256(model: Model, name: string): string {
  return createHash("sha256").update(serializeModel(libraryModel(model, name)), "utf8").digest("hex");
}

function restrict(declaration: Declaration, libTargets: readonly string[], keep: readonly string[]): Declaration | undefined {
  const on = (declaration.targets ?? libTargets).filter((t) => keep.includes(t));
  if (on.length === 0) return undefined;
  const out = { ...declaration };
  if (on.length === keep.length) delete out.targets;
  else out.targets = on;
  if (out.perTarget !== undefined) {
    const facts = Object.entries(out.perTarget).filter(([t]) => keep.includes(t));
    if (facts.length === 0) delete out.perTarget;
    else out.perTarget = Object.fromEntries(facts);
  }
  return canonical(out as never, "declaration") as unknown as Declaration;
}

/**
 * The model cut down to the named libraries (those it has) and to `targets`
 * (those it covers), so it compares with a binary's manifest like for like.
 */
export function narrowModel(model: Model, names: readonly string[], targets: readonly string[]): Model {
  const modelTargets = model.targets.filter((t) => targets.includes(t));
  const libraries = model.libraries.flatMap((library): ModelLibrary[] => {
    if (!names.includes(library.name)) return [];
    const libTargets = libraryTargets(model, library);
    const keep = modelTargets.filter((t) => libTargets.includes(t));
    const { targets: _, ...rest } = library;
    return [{
      ...rest,
      ...(keep.length === modelTargets.length ? {} : { targets: keep }),
      declarations: library.declarations.flatMap((d) => (keep.length === 0 ? [] : (restrict(d, libTargets, keep) ?? []))),
    }];
  });
  return { ...model, targets: modelTargets, libraries };
}

/**
 * The manifest of `binary` built for `targets`: each of its libraries in the
 * model's shape, with only those targets' declarations and facts.
 */
export function buildManifest(model: Model, binary: BinaryConfig, targets: readonly string[]): Manifest {
  if (targets.length === 0) throw new StelaError(`${binary.name}: no targets given`);
  for (const target of targets) {
    if (!model.targets.includes(target)) throw new StelaError(`${binary.name}: the model has no target ${target}`);
  }
  if (new Set(targets).size !== targets.length) throw new StelaError(`${binary.name}: a target is given twice`);
  const binaryTargets = model.targets.filter((t) => targets.includes(t));
  const libraries = binary.libraries.map((name): EmbeddedLibrary => {
    const library = model.libraries.find((l) => l.name === name);
    if (library === undefined) throw new StelaError(`${binary.name}: library "${name}" is missing from the model`);
    const libTargets = libraryTargets(model, library);
    const keep = binaryTargets.filter((t) => libTargets.includes(t));
    if (keep.length === 0) {
      throw new StelaError(`${binary.name}: library "${name}" isn't built for ${binaryTargets.join(", ")}`);
    }
    const declarations = library.declarations.flatMap((d) => restrict(d, libTargets, keep) ?? []);
    const abiVersions: Record<string, string> = {};
    for (const d of declarations) {
      if (d.kind !== "macro" || !VERSION_RE.test(d.name)) continue;
      const value = keep.map((t) => d.perTarget?.[t]?.value).find((v) => v !== undefined);
      if (value !== undefined) abiVersions[d.name] = value;
    }
    const { declarations: _, targets: __, ...rest } = library;
    return {
      ...rest,
      ...(keep.length === binaryTargets.length ? {} : { targets: keep }),
      modelSha256: modelSha256(model, name),
      abiVersions,
      exports: binary.exports === "all" ? declarations.filter((d) => d.kind === "function").map((d) => d.name) : [],
      declarations,
    };
  });
  return {
    schema: EMBEDDED_SCHEMA,
    producer: { ...model.producer },
    binary: binary.name,
    targets: binaryTargets,
    otherExports: binary.otherExports,
    libraries,
  };
}

/**
 * The manifest as gzip with a fixed header (no name, mtime 0, OS unknown), so
 * the bytes depend only on the manifest and the zlib version.
 */
export function encodeManifest(manifest: Manifest): Buffer {
  const json = Buffer.from(JSON.stringify(manifest), "utf8");
  if (json.length > MAX_MANIFEST_BYTES) throw new StelaError(`the ${manifest.binary} manifest exceeds ${MAX_MANIFEST_BYTES} bytes`);
  const header = Buffer.from([0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 2, 0xff]);
  const trailer = Buffer.alloc(8);
  trailer.writeUInt32LE(crc32(json) >>> 0, 0);
  trailer.writeUInt32LE(json.length, 4);
  return Buffer.concat([header, deflateRawSync(json, { level: 9 }), trailer]);
}

/** The one-line resource script that links `<gzFile>` as resource STELA 1. */
export function resourceScript(gzFile: string): string {
  return `// Generated by stela gen embed; do not edit.\n${RESOURCE_ID} ${RESOURCE_TYPE} "${gzFile}"\n`;
}

/** The files `gen embed` writes for a manifest: `<binary>.stela.gz` and `<binary>.stela.rc`. */
export function embedFiles(manifest: Manifest): { path: string; bytes: Buffer }[] {
  const gz = `${manifest.binary}.stela.gz`;
  return [
    { path: gz, bytes: encodeManifest(manifest) },
    { path: `${manifest.binary}.stela.rc`, bytes: Buffer.from(resourceScript(gz), "utf8") },
  ];
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

/** Checks a parsed manifest against the schema and the rules a schema can't state. */
export function validateManifest(json: unknown, what = "manifest"): Manifest {
  if (isObject(json) && json.schema !== EMBEDDED_SCHEMA) {
    throw new StelaError(`${what}: schema ${JSON.stringify(json.schema)} isn't ${EMBEDDED_SCHEMA}`);
  }
  validateJson(json, "stela.embedded.schema.json", what);
  const manifest = json as Manifest;
  const names = new Set<string>();
  for (const library of manifest.libraries) {
    const at = `${what}: library ${library.name}`;
    if (names.has(library.name)) throw new StelaError(`${at} is listed twice`);
    names.add(library.name);
    const libTargets = library.targets ?? manifest.targets;
    for (const t of libTargets) {
      if (!manifest.targets.includes(t)) throw new StelaError(`${at}: target ${t} isn't one of the binary's`);
    }
    const functions = new Set<string>();
    const keys = new Set<string>();
    for (const d of library.declarations) {
      const key = `${d.kind} ${d.name}`;
      if (keys.has(key)) throw new StelaError(`${at}: ${key} is declared twice`);
      keys.add(key);
      if (d.kind === "function") functions.add(d.name);
      for (const t of [...(d.targets ?? []), ...Object.keys(d.perTarget ?? {})]) {
        if (!libTargets.includes(t)) throw new StelaError(`${at}: ${key} names target ${t}, which the library doesn't cover`);
      }
    }
    for (const name of library.exports) {
      if (!functions.has(name)) throw new StelaError(`${at}: export ${name} isn't one of its functions`);
    }
  }
  return manifest;
}

/** Decompresses (at most MAX_MANIFEST_BYTES) and validates a manifest. */
export function decodeManifest(bytes: Buffer, what = "manifest"): Manifest {
  if (bytes.length < 18 || bytes[0] !== 0x1f || bytes[1] !== 0x8b) throw new StelaError(`${what}: not gzip data`);
  let json: unknown;
  try {
    json = JSON.parse(gunzipSync(bytes, { maxOutputLength: MAX_MANIFEST_BYTES }).toString("utf8"));
  } catch (e) {
    const err = e as { code?: string; message: string };
    if (err.code === "ERR_BUFFER_TOO_LARGE" || e instanceof RangeError) {
      throw new StelaError(`${what}: decompresses to more than ${MAX_MANIFEST_BYTES} bytes`);
    }
    throw new StelaError(`${what}: can't be decoded: ${err.message}`);
  }
  return validateManifest(json, what);
}

/** Reads the manifest, export names, and machine type of a binary without loading it. */
export function readEmbedded(file: string): EmbeddedBinary {
  let bytes: Buffer;
  try {
    bytes = readFileSync(file);
  } catch (e) {
    throw new StelaError(`cannot read ${file}: ${(e as Error).message}`);
  }
  const pe = parsePe(bytes, file);
  const target = MACHINE_TARGETS[pe.machine];
  if (target === undefined) throw new StelaError(`${file}: unknown machine type 0x${pe.machine.toString(16)}`);
  const data = pe.resource(RESOURCE_TYPE, RESOURCE_ID);
  if (data === undefined) throw new StelaError(`${file}: no ${RESOURCE_TYPE} resource; was it built with \`stela gen embed\`?`);
  const manifest = decodeManifest(data, `${file}: manifest`);
  if (!manifest.targets.includes(target)) {
    throw new StelaError(
      `${file}: the machine type 0x${pe.machine.toString(16)} (${target}) isn't among the manifest's targets ` +
        `(${manifest.targets.join(", ")})`,
    );
  }
  return { file, machine: pe.machine, target, exports: pe.exports, manifest };
}

/** True when the file starts like a PE binary rather than a model. */
export function isBinaryFile(file: string): boolean {
  let fd: number | undefined;
  try {
    fd = openSync(file, "r");
    const head = Buffer.alloc(2);
    return readSync(fd, head, 0, 2, 0) === 2 && isPe(head);
  } catch {
    return false;
  } finally {
    if (fd !== undefined) closeSync(fd);
  }
}

/** The manifest as a stela.model/1, so `diff` and `check` can compare it. */
export function manifestModel(manifest: Manifest): Model {
  return {
    schema: MODEL_SCHEMA,
    producer: manifest.producer,
    targets: [...manifest.targets],
    libraries: manifest.libraries.map(({ modelSha256: _, abiVersions: __, exports: ___, ...library }) => library),
  };
}

/** A model read from a model file or, when it's a binary, from its manifest. */
export function readModelOrBinary(file: string): Model {
  return isBinaryFile(file) ? manifestModel(readEmbedded(file).manifest) : readModel(file);
}

export interface VerifyResult {
  /** Manifest exports the binary lacks. */
  readonly missing: string[];
  /** Binary exports the manifest doesn't allow. */
  readonly unexpected: string[];
}

/**
 * Compares a binary's export table with its manifest: every listed export
 * exists, no other name with a library's prefix is exported, and nothing else
 * is exported unless the manifest allows other exports.
 */
export function verifyExports(binary: EmbeddedBinary): VerifyResult {
  const { manifest } = binary;
  const listed = new Set(manifest.libraries.flatMap((l) => l.exports));
  const actual = new Set(binary.exports);
  const prefixes = manifest.libraries.map((l) => `${l.prefix}_`);
  return {
    missing: [...listed].filter((name) => !actual.has(name)),
    unexpected: binary.exports.filter(
      (name) => !listed.has(name) && (!manifest.otherExports || prefixes.some((p) => name.startsWith(p))),
    ),
  };
}
