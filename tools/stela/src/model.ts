// The Stela model (design §7): merges the per-target stela.raw/1 extractions
// of each library into one stela.model/1, and serializes it deterministically.

import { readFileSync } from "node:fs";
import path from "node:path";
import type { LibraryConfig, StelaConfig } from "./config.ts";
import { StelaError } from "./errors.ts";

export const RAW_SCHEMA = "stela.raw/1";
export const MODEL_SCHEMA = "stela.model/1";
export const STELA_VERSION: string = JSON.parse(
  readFileSync(path.join(import.meta.dirname, "..", "package.json"), "utf8"),
).version;

export type Json = null | boolean | number | string | Json[] | { [key: string]: Json };
export type JsonObject = { [key: string]: Json };

export interface TypeNode extends JsonObject {}

export interface Annotation {
  key: string;
  value?: string;
}

export interface Parameter {
  name: string;
  type: TypeNode;
  annotations: Annotation[];
}

export interface TargetFacts {
  canonicalType?: TypeNode;
  size?: number;
  align?: number;
  offsets?: number[];
  fieldCallingConventions?: (string | null)[];
  callingConvention?: string;
  value?: string;
}

export interface Field {
  name: string;
  type: TypeNode;
  annotations: Annotation[];
  doc: string | null;
  bitWidth?: number;
}

export interface Enumerator {
  name: string;
  value: number;
  doc: string | null;
}

export interface Declaration {
  kind: "function" | "typedef" | "struct" | "union" | "enum" | "macro";
  name: string;
  location: { file: string; line: number; column: number };
  doc: string | null;
  annotations: Annotation[];
  result?: { type: TypeNode };
  params?: Parameter[];
  variadic?: boolean;
  type?: TypeNode;
  fields?: Field[];
  enumerators?: Enumerator[];
  /** A struct or union defined without a tag; only a typedef names it. */
  tagless?: true;
  /** A struct or union declared without a definition (an opaque handle's target). */
  incomplete?: true;
  functionLike?: boolean;
  replacement?: string[];
  /** Present only when the declaration exists on some of the library's targets. */
  targets?: string[];
  perTarget?: Record<string, TargetFacts>;
}

export interface RawModel {
  schema: string;
  producer: { stela: string; libclang: string };
  target: string;
  headers: string[];
  declarations: JsonObject[];
}

export interface ModelLibrary {
  name: string;
  prefix: string;
  macroPrefix: string;
  status: string;
  headers: string[];
  includeDirs: string[];
  defines: string[];
  /** The generated `<lib>_stela.h`; absent for a library that uses another's. */
  stelaHeader?: string;
  /** Present only when the library covers some of the model's targets. */
  targets?: string[];
  declarations: Declaration[];
}

export interface Model {
  schema: string;
  producer: { stela: string; libclang: string };
  targets: string[];
  libraries: ModelLibrary[];
}

export interface Mismatch {
  readonly library: string;
  readonly kind: string;
  readonly name: string;
  readonly targets: readonly [string, string];
  /** The first path where the two targets' declarations differ. */
  readonly at: string;
}

export function formatMismatch(m: Mismatch): string {
  return (
    `target_mismatch: ${m.library}: ${m.kind} ${m.name} differs between ` +
    `${m.targets[0]} and ${m.targets[1]} at ${m.at || "(root)"}`
  );
}

// --- Canonical form ---------------------------------------------------------
//
// Every object in a model has a fixed key order (the schema's), so a
// declaration serializes the same way however it was produced. Unknown keys
// are rejected, which also keeps raw input honest.

type Shape =
  | "declaration"
  | "location"
  | "annotation"
  | "type"
  | "record"
  | "inlineEnum"
  | "function"
  | "parameter"
  | "field"
  | "enumerator"
  | "result"
  | "facts";
type Child = Shape | `${Shape}[]` | "scalar" | "scalar[]" | "facts{}";

const SHAPES: Record<Shape, readonly (readonly [string, Child])[]> = {
  declaration: [
    ["kind", "scalar"],
    ["name", "scalar"],
    ["location", "location"],
    ["doc", "scalar"],
    ["annotations", "annotation[]"],
    ["result", "result"],
    ["params", "parameter[]"],
    ["variadic", "scalar"],
    ["type", "type"],
    ["tagless", "scalar"],
    ["fields", "field[]"],
    ["enumerators", "enumerator[]"],
    ["incomplete", "scalar"],
    ["functionLike", "scalar"],
    ["replacement", "scalar[]"],
    ["targets", "scalar[]"],
    ["perTarget", "facts{}"],
  ],
  location: [
    ["file", "scalar"],
    ["line", "scalar"],
    ["column", "scalar"],
  ],
  annotation: [
    ["key", "scalar"],
    ["value", "scalar"],
  ],
  type: [
    ["builtin", "scalar"],
    ["name", "scalar"],
    ["pointer", "type"],
    ["array", "type"],
    ["length", "scalar"],
    ["function", "function"],
    ["struct", "record"],
    ["union", "record"],
    ["enum", "inlineEnum"],
    ["const", "scalar"],
  ],
  record: [["fields", "field[]"]],
  inlineEnum: [
    ["type", "type"],
    ["enumerators", "enumerator[]"],
  ],
  function: [
    ["result", "type"],
    ["params", "parameter[]"],
    ["variadic", "scalar"],
  ],
  parameter: [
    ["name", "scalar"],
    ["type", "type"],
    ["annotations", "annotation[]"],
  ],
  field: [
    ["name", "scalar"],
    ["type", "type"],
    ["annotations", "annotation[]"],
    ["doc", "scalar"],
    ["bitWidth", "scalar"],
  ],
  enumerator: [
    ["name", "scalar"],
    ["value", "scalar"],
    ["doc", "scalar"],
  ],
  result: [["type", "type"]],
  facts: [
    ["canonicalType", "type"],
    ["size", "scalar"],
    ["align", "scalar"],
    ["offsets", "scalar[]"],
    ["fieldCallingConventions", "scalar[]"],
    ["callingConvention", "scalar"],
    ["value", "scalar"],
  ],
};

function isObject(value: unknown): value is JsonObject {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function canonChild(value: Json, child: Child, where: string): Json {
  if (child === "scalar") {
    if (isObject(value) || Array.isArray(value)) throw new StelaError(`${where}: expected a scalar`);
    return value;
  }
  if (child === "facts{}") {
    if (!isObject(value)) throw new StelaError(`${where}: expected an object`);
    const out: JsonObject = {};
    for (const [target, facts] of Object.entries(value)) {
      out[target] = canonical(facts, "facts", `${where}.${target}`);
    }
    return out;
  }
  if (child.endsWith("[]")) {
    if (!Array.isArray(value)) throw new StelaError(`${where}: expected an array`);
    const item = child.slice(0, -2) as Shape | "scalar";
    return value.map((v, i) => canonChild(v, item, `${where}[${i}]`));
  }
  return canonical(value, child as Shape, where);
}

/** Returns a copy of `value` with the shape's key order; rejects unknown keys. */
export function canonical(value: Json, shape: Shape, where: string = shape): JsonObject {
  if (!isObject(value)) throw new StelaError(`${where}: expected an object`);
  const keys = SHAPES[shape];
  for (const key of Object.keys(value)) {
    if (!keys.some(([k]) => k === key)) throw new StelaError(`${where}: unexpected key "${key}"`);
  }
  const out: JsonObject = {};
  for (const [key, child] of keys) {
    if (value[key] !== undefined) out[key] = canonChild(value[key], child, `${where}.${key}`);
  }
  return out;
}

/** The first path where two JSON values differ, or undefined if they're equal. */
export function firstDifference(a: Json, b: Json, at = ""): string | undefined {
  if (Array.isArray(a) && Array.isArray(b)) {
    for (let i = 0; i < Math.max(a.length, b.length); i++) {
      if (i >= a.length || i >= b.length) return `${at}[${i}]`;
      const d = firstDifference(a[i], b[i], `${at}[${i}]`);
      if (d !== undefined) return d;
    }
    return undefined;
  }
  if (isObject(a) && isObject(b)) {
    for (const key of new Set([...Object.keys(a), ...Object.keys(b)])) {
      const sub = at ? `${at}.${key}` : key;
      if (!(key in a) || !(key in b)) return sub;
      const d = firstDifference(a[key], b[key], sub);
      if (d !== undefined) return d;
    }
    return undefined;
  }
  return a === b ? undefined : at;
}

// --- Normalization ----------------------------------------------------------

/**
 * One raw declaration in model form. libclang reports a layout it can't
 * compute as a negative size: a record without a definition becomes
 * `incomplete`, and a typedef of an incomplete type loses its size and align.
 */
export function normalizeDeclaration(raw: JsonObject, target: string): JsonObject {
  const where = `${target}: ${String(raw.kind)} ${String(raw.name)}`;
  const decl = canonical(raw, "declaration", where);
  if (decl.targets !== undefined || decl.incomplete !== undefined) {
    throw new StelaError(`${where}: raw declarations can't carry "targets" or "incomplete"`);
  }
  const perTarget = decl.perTarget as Record<string, JsonObject> | undefined;
  if (perTarget !== undefined) {
    const keys = Object.keys(perTarget);
    if (keys.length !== 1 || keys[0] !== target) {
      throw new StelaError(`${where}: "perTarget" must hold exactly the target ${target}`);
    }
    const facts = perTarget[target];
    if (typeof facts.size === "number" && facts.size < 0) {
      if (decl.kind === "struct" || decl.kind === "union") {
        delete decl.perTarget;
        decl.incomplete = true;
        return canonical(decl, "declaration", where);
      }
      delete facts.size;
      delete facts.align;
      if (Object.keys(facts).length === 0) delete decl.perTarget;
    }
  }
  return decl;
}

// --- Merging ----------------------------------------------------------------

export function relativePath(root: string, file: string): string {
  const relative = path.relative(root, file);
  if (relative === "" || relative.startsWith("..") || path.isAbsolute(relative)) {
    throw new StelaError(`${file} is outside the configuration root ${root}`);
  }
  return relative.split(path.sep).join("/");
}

/**
 * The include directories a consumer needs: the configured ones, plus the
 * directory of each entry header, and of the stela header, that none of them
 * contains.
 */
export function effectiveIncludeDirs(root: string, library: LibraryConfig): string[] {
  const dirs = library.includeDirs.map((d) => relativePath(root, d));
  const headers = library.stelaHeader === false ? library.headers : [...library.headers, library.stelaHeader];
  for (const header of headers) {
    const relative = relativePath(root, header);
    if (!dirs.some((d) => relative.startsWith(`${d}/`))) {
      const dir = path.posix.dirname(relative);
      if (dir === ".") throw new StelaError(`${header}: a library header needs a directory below the root`);
      dirs.push(dir);
    }
  }
  return [...new Set(dirs)];
}

interface Entry {
  readonly key: string;
  readonly common: JsonObject;
  readonly json: string;
  readonly firstTarget: string;
  readonly order: readonly [number, number];
  readonly present: Set<string>;
  readonly facts: Map<string, JsonObject>;
}

/**
 * Merges one library's raw extractions (design §7.3). Declarations are keyed
 * by kind and name; their target-independent parts must match on every target
 * that has them.
 */
export function mergeLibrary(
  root: string,
  library: LibraryConfig,
  raws: ReadonlyMap<string, RawModel>,
): { library: ModelLibrary; mismatches: Mismatch[]; libclang: string } {
  const headers = library.headers.map((h) => relativePath(root, h));
  const fileRank = new Map<string, number>(headers.map((h, i) => [h, i]));
  const entries = new Map<string, Entry>();
  const mismatches: Mismatch[] = [];
  let libclang: string | undefined;

  library.targets.forEach((target, targetIndex) => {
    const raw = raws.get(target);
    if (raw === undefined) throw new StelaError(`${library.name}: no extraction for ${target}`);
    if (raw.schema !== RAW_SCHEMA) throw new StelaError(`${library.name} ${target}: schema is not ${RAW_SCHEMA}`);
    if (raw.target !== target) throw new StelaError(`${library.name} ${target}: the raw model is for ${raw.target}`);
    if (libclang === undefined) libclang = raw.producer.libclang;
    else if (raw.producer.libclang !== libclang) {
      throw new StelaError(`${library.name}: extractions disagree on the libclang version`);
    }
    raw.declarations.forEach((rawDecl, index) => {
      const decl = normalizeDeclaration(rawDecl, target);
      const { perTarget, ...common } = decl;
      const key = `${common.kind as string} ${common.name as string}`;
      const file = (common.location as { file: string }).file;
      if (!fileRank.has(file)) fileRank.set(file, fileRank.size);
      const json = JSON.stringify(common);
      const entry = entries.get(key);
      if (entry === undefined) {
        const facts = new Map<string, JsonObject>();
        if (perTarget !== undefined) facts.set(target, (perTarget as Record<string, JsonObject>)[target]);
        entries.set(key, {
          key,
          common,
          json,
          firstTarget: target,
          order: [targetIndex, index],
          present: new Set([target]),
          facts,
        });
        return;
      }
      if (entry.present.has(target)) {
        throw new StelaError(`${library.name} ${target}: ${key} is declared twice`);
      }
      entry.present.add(target);
      if (json !== entry.json) {
        mismatches.push({
          library: library.name,
          kind: common.kind as string,
          name: common.name as string,
          targets: [entry.firstTarget, target],
          at: firstDifference(entry.common, common) ?? "",
        });
        return;
      }
      if (perTarget !== undefined) entry.facts.set(target, (perTarget as Record<string, JsonObject>)[target]);
    });
  });

  // Source order: header include order, then line and column. Ties (a typedef
  // and its tag at one macro expansion) keep the extractor's order.
  const sortKey = (e: Entry) => {
    const { file, line, column } = e.common.location as { file: string; line: number; column: number };
    return [fileRank.get(file)!, line, column, e.order[0], e.order[1]];
  };
  const sorted = [...entries.values()].sort((a, b) => {
    const ka = sortKey(a);
    const kb = sortKey(b);
    for (let i = 0; i < ka.length; i++) if (ka[i] !== kb[i]) return ka[i] - kb[i];
    return 0;
  });

  const declarations = sorted.map((entry) => {
    const decl: JsonObject = { ...entry.common };
    if (entry.present.size < library.targets.length) {
      decl.targets = library.targets.filter((t) => entry.present.has(t));
    }
    if (entry.facts.size > 0) {
      const perTarget: JsonObject = {};
      for (const target of library.targets) {
        const facts = entry.facts.get(target);
        if (facts !== undefined) perTarget[target] = facts;
      }
      decl.perTarget = perTarget;
    }
    return canonical(decl, "declaration") as unknown as Declaration;
  });

  return {
    library: {
      name: library.name,
      prefix: library.prefix,
      macroPrefix: library.macroPrefix,
      status: library.status,
      headers,
      includeDirs: effectiveIncludeDirs(root, library),
      defines: [...library.defines],
      ...(library.stelaHeader === false ? {} : { stelaHeader: relativePath(root, library.stelaHeader) }),
      declarations,
    },
    mismatches,
    libclang: libclang ?? "",
  };
}

/** Builds the model from every library's raw extractions, keyed by target. */
export function buildModel(
  config: StelaConfig,
  raws: ReadonlyMap<string, ReadonlyMap<string, RawModel>>,
): { model: Model; mismatches: Mismatch[] } {
  const libraries: ModelLibrary[] = [];
  const mismatches: Mismatch[] = [];
  let libclang: string | undefined;
  for (const library of config.libraries) {
    if (library.targets.length === 0) throw new StelaError(`${library.name}: no targets to extract`);
    const merged = mergeLibrary(config.root, library, raws.get(library.name) ?? new Map());
    if (libclang === undefined) libclang = merged.libclang;
    else if (merged.libclang !== libclang) throw new StelaError("extractions disagree on the libclang version");
    const { declarations, ...rest } = merged.library;
    const sameTargets =
      library.targets.length === config.targets.length &&
      library.targets.every((t, i) => t === config.targets[i]);
    libraries.push({ ...rest, ...(sameTargets ? {} : { targets: [...library.targets] }), declarations });
    mismatches.push(...merged.mismatches);
  }
  return {
    model: {
      schema: MODEL_SCHEMA,
      producer: { stela: STELA_VERSION, libclang: libclang ?? "" },
      targets: [...config.targets],
      libraries,
    },
    mismatches,
  };
}

const LINE_WIDTH = 100;

function compact(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(compact).join(", ")}]`;
  if (isObject(value)) {
    return `{${Object.entries(value)
      .map(([k, v]) => `${JSON.stringify(k)}: ${compact(v)}`)
      .join(", ")}}`;
  }
  return JSON.stringify(value);
}

function format(value: unknown, indent: string, prefix: string): string {
  const flat = compact(value);
  const container = Array.isArray(value) || isObject(value);
  if (!container || indent.length + prefix.length + flat.length <= LINE_WIDTH) return flat;
  const inner = `${indent}  `;
  const items = Array.isArray(value)
    ? value.map((v) => `${inner}${format(v, inner, "")}`)
    : Object.entries(value as JsonObject).map(([k, v]) => {
        const key = `${JSON.stringify(k)}: `;
        return `${inner}${key}${format(v, inner, key)}`;
      });
  if (items.length === 0) return flat;
  const [open, close] = Array.isArray(value) ? ["[", "]"] : ["{", "}"];
  return `${open}\n${items.join(",\n")}\n${indent}${close}`;
}

/**
 * Deterministic text (design §7.4): two-space indentation, LF line endings,
 * and a final newline. An object or array that fits on one line within 100
 * columns is written on one line, with a space after each `,` and `:`.
 */
export function serializeModel(model: Model): string {
  return `${format(model, "", "")}\n`;
}

export function readModel(file: string): Model {
  let json: unknown;
  try {
    json = JSON.parse(readFileSync(file, "utf8").replace(/^\uFEFF/, ""));
  } catch (e) {
    throw new StelaError(`cannot read model ${file}: ${(e as Error).message}`);
  }
  if (!isObject(json) || json.schema !== MODEL_SCHEMA || !Array.isArray(json.libraries)) {
    throw new StelaError(`${file}: not a ${MODEL_SCHEMA} model`);
  }
  return json as unknown as Model;
}

/** `out/stela/<config name>.model.json` below the config's directory (design §8.1). */
export function defaultModelPath(configFile: string): string {
  const base = path.basename(configFile).replace(/\.json$/i, "").replace(/\.config$/i, "");
  return path.join(path.dirname(path.resolve(configFile)), "out", "stela", `${base || "stela"}.model.json`);
}
