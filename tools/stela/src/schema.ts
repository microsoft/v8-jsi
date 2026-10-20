// Validates JSON against Stela's own JSON Schemas (schema/*.schema.json), so
// input from an untrusted binary is checked against the published contract.
// Supports the draft 2020-12 keywords those schemas use; any other keyword is
// an error, so a schema can't silently outgrow the validator.

import { readFileSync } from "node:fs";
import path from "node:path";
import { StelaError } from "./errors.ts";

export const SCHEMA_DIR = path.join(import.meta.dirname, "..", "schema");

type Schema = boolean | { readonly [key: string]: unknown };

const ANNOTATIONS = new Set(["$schema", "$id", "$defs", "title", "description"]);
const KEYWORDS = new Set([
  "$ref",
  "type",
  "const",
  "enum",
  "minimum",
  "minLength",
  "pattern",
  "properties",
  "additionalProperties",
  "required",
  "minProperties",
  "items",
  "minItems",
  "uniqueItems",
  "allOf",
  "anyOf",
  "oneOf",
  "not",
  "if",
  "then",
  "else",
]);
/** Deeper input than any model needs is rejected rather than risking the stack. */
const MAX_DEPTH = 128;

const documents = new Map<string, Schema>();
const patterns = new Map<string, RegExp>();

function load(file: string): Schema {
  let schema = documents.get(file);
  if (schema === undefined) {
    schema = JSON.parse(readFileSync(file, "utf8")) as Schema;
    documents.set(file, schema);
  }
  return schema;
}

function resolve(ref: string, base: string): { schema: Schema; base: string } {
  const hash = ref.indexOf("#");
  const filePart = hash < 0 ? ref : ref.slice(0, hash);
  const pointer = hash < 0 ? "" : ref.slice(hash + 1);
  const file = filePart === "" ? base : path.join(path.dirname(base), filePart);
  let node: unknown = load(file);
  for (const segment of pointer.split("/").slice(1)) {
    const key = segment.replaceAll("~1", "/").replaceAll("~0", "~");
    node = typeof node === "object" && node !== null ? (node as Record<string, unknown>)[key] : undefined;
    if (node === undefined) throw new Error(`schema reference ${ref} from ${base} doesn't resolve`);
  }
  return { schema: node as Schema, base: file };
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function typeOf(value: unknown): string {
  if (value === null) return "null";
  if (Array.isArray(value)) return "array";
  return typeof value;
}

function hasType(value: unknown, type: string): boolean {
  switch (type) {
    case "integer":
      return Number.isInteger(value);
    case "number":
      return typeof value === "number" && Number.isFinite(value);
    case "object":
      return isObject(value);
    default:
      return typeOf(value) === type;
  }
}

function key(value: unknown): string {
  return JSON.stringify(value);
}

function regex(pattern: string): RegExp {
  let re = patterns.get(pattern);
  if (re === undefined) {
    re = new RegExp(pattern, "u");
    patterns.set(pattern, re);
  }
  return re;
}

/** The first error of `value` against `schema`, or undefined when it's valid. */
function check(value: unknown, schema: Schema, base: string, at: string, depth: number): string | undefined {
  if (schema === true) return undefined;
  if (schema === false) return `${at}: is not allowed`;
  if (depth > MAX_DEPTH) return `${at}: nested too deeply`;
  const s = schema as Record<string, unknown>;
  for (const k of Object.keys(s)) {
    if (!KEYWORDS.has(k) && !ANNOTATIONS.has(k)) throw new Error(`${base}: unsupported schema keyword "${k}"`);
  }
  const sub = (v: unknown, sch: unknown, where: string) => check(v, sch as Schema, base, where, depth + 1);

  if (typeof s.$ref === "string") {
    const target = resolve(s.$ref, base);
    const error = check(value, target.schema, target.base, at, depth + 1);
    if (error !== undefined) return error;
  }
  if (s.type !== undefined) {
    const types = Array.isArray(s.type) ? (s.type as string[]) : [s.type as string];
    if (!types.some((t) => hasType(value, t))) return `${at}: expected ${types.join(" or ")}, found ${typeOf(value)}`;
  }
  if ("const" in s && key(value) !== key(s.const)) return `${at}: must be ${key(s.const)}`;
  if (Array.isArray(s.enum) && !s.enum.some((e) => key(e) === key(value))) {
    return `${at}: must be one of ${s.enum.map(key).join(", ")}`;
  }
  if (typeof value === "number" && typeof s.minimum === "number" && value < s.minimum) {
    return `${at}: must be at least ${s.minimum}`;
  }
  if (typeof value === "string") {
    if (typeof s.minLength === "number" && [...value].length < s.minLength) return `${at}: is too short`;
    if (typeof s.pattern === "string" && !regex(s.pattern).test(value)) return `${at}: doesn't match ${s.pattern}`;
  }
  if (isObject(value)) {
    const properties = (s.properties ?? {}) as Record<string, unknown>;
    if (Array.isArray(s.required)) {
      for (const name of s.required as string[]) {
        if (!Object.hasOwn(value, name)) return `${at}: "${name}" is required`;
      }
    }
    if (typeof s.minProperties === "number" && Object.keys(value).length < s.minProperties) {
      return `${at}: needs at least ${s.minProperties} propert${s.minProperties === 1 ? "y" : "ies"}`;
    }
    for (const [name, v] of Object.entries(value)) {
      const where = `${at}.${name}`;
      if (Object.hasOwn(properties, name)) {
        const error = sub(v, properties[name], where);
        if (error !== undefined) return error;
      } else if (s.additionalProperties !== undefined) {
        if (s.additionalProperties === false) return `${at}: unexpected key "${name}"`;
        const error = sub(v, s.additionalProperties, where);
        if (error !== undefined) return error;
      }
    }
  }
  if (Array.isArray(value)) {
    if (typeof s.minItems === "number" && value.length < s.minItems) return `${at}: needs at least ${s.minItems} item(s)`;
    if (s.uniqueItems === true && new Set(value.map(key)).size !== value.length) return `${at}: has duplicate items`;
    if (s.items !== undefined) {
      for (let i = 0; i < value.length; i++) {
        const error = sub(value[i], s.items, `${at}[${i}]`);
        if (error !== undefined) return error;
      }
    }
  }
  if (Array.isArray(s.allOf)) {
    for (const part of s.allOf) {
      const error = sub(value, part, at);
      if (error !== undefined) return error;
    }
  }
  if (Array.isArray(s.anyOf) && !s.anyOf.some((part) => sub(value, part, at) === undefined)) {
    return `${at}: matches none of the allowed forms`;
  }
  if (Array.isArray(s.oneOf)) {
    const errors = s.oneOf.map((part) => sub(value, part, at));
    const matches = errors.filter((e) => e === undefined).length;
    if (matches === 0) {
      // A single alternative's error is the useful one; several aren't.
      return errors.length === 1 ? errors[0] : `${at}: matches none of the allowed forms`;
    }
    if (matches > 1) return `${at}: matches more than one of the allowed forms`;
  }
  if (s.not !== undefined && sub(value, s.not, at) === undefined) return `${at}: is not allowed here`;
  if (s.if !== undefined) {
    const branch = sub(value, s.if, at) === undefined ? s.then : s.else;
    if (branch !== undefined) {
      const error = sub(value, branch, at);
      if (error !== undefined) return error;
    }
  }
  return undefined;
}

/** Throws a StelaError naming the first place where `value` breaks the schema in `schemaFile`. */
export function validateJson(value: unknown, schemaFile: string, what: string): void {
  const file = path.resolve(SCHEMA_DIR, schemaFile);
  const error = check(value, load(file), file, "$", 0);
  if (error !== undefined) throw new StelaError(`${what}: ${error}`);
}
