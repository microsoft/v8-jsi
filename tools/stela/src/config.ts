// Loads and validates stela.config.json (Stela design §8.2).

import { readFileSync } from "node:fs";
import path from "node:path";
import { StelaError } from "./errors.ts";

export const CONFIG_SCHEMA = "stela.config/1";

export interface LibraryConfig {
  readonly name: string;
  readonly prefix: string;
  readonly macroPrefix: string;
  /** Entry headers, absolute paths. */
  readonly headers: readonly string[];
  /** Absolute path of the generated <lib>_stela.h, or false for an extension library. */
  readonly stelaHeader: string | false;
  readonly stelaNames: Readonly<Record<string, string>>;
  /** The library's status type (§8.2 `status`). */
  readonly status: string;
  /** Include directories a consumer puts on its path, absolute paths. */
  readonly includeDirs: readonly string[];
  /** Files or directories whose declarations belong to the library, absolute paths. */
  readonly owns: readonly string[];
  /** Preprocessor definitions, `NAME` or `NAME=VALUE`. */
  readonly defines: readonly string[];
  /** The targets this library is extracted for: its own list, or the global one. */
  readonly targets: readonly string[];
}

export type LintSeverity = "error" | "warning" | "info" | "off";

export interface LintConfig {
  readonly rules: Readonly<Record<string, LintSeverity>>;
  /** Integer macros that the macro_constants rule permits. */
  readonly macroConstants: readonly string[];
}

export interface DiffConfig {
  /** Library name → absolute path of its baseline model, in library order. */
  readonly baselines: Readonly<Record<string, string>>;
  /** Absolute paths of headers whose breaks `diff` reports as notes. */
  readonly experimental: readonly string[];
}

/** A binary that implements libraries and embeds their manifest (design §12). */
export interface BinaryConfig {
  /** The file name, such as `v8jsi.dll`. */
  readonly name: string;
  /** The libraries it implements, in config library order. */
  readonly libraries: readonly string[];
  /** `all`: it exports every function of its libraries; `none`: it exports nothing of them. */
  readonly exports: "all" | "none";
  /** Whether it may export names outside its libraries. */
  readonly otherExports: boolean;
}

export interface StelaConfig {
  /** Absolute path of the config file. */
  readonly file: string;
  /** The directory that config paths are relative to. */
  readonly root: string;
  /** The global target list, in order. Empty when the config sets none. */
  readonly targets: readonly string[];
  readonly libraries: readonly LibraryConfig[];
  readonly lint: LintConfig;
  readonly diff: DiffConfig;
  readonly binaries: readonly BinaryConfig[];
}

const TOP_LEVEL_KEYS = new Set(["$schema", "schema", "targets", "libraries", "lint", "diff", "binaries"]);
const BINARY_KEYS = new Set(["name", "libraries", "exports", "otherExports"]);
const BINARY_NAME_RE = /^[A-Za-z0-9_][A-Za-z0-9_.-]*\.(?:dll|exe)$/i;
const LIBRARY_KEYS = new Set([
  "name",
  "prefix",
  "macroPrefix",
  "headers",
  "includeDirs",
  "owns",
  "defines",
  "targets",
  "status",
  "stelaHeader",
  "stelaNames",
]);
const LOWER_SNAKE_RE = /^[a-z][a-z0-9]*(?:_[a-z0-9]+)*$/;
const UPPER_SNAKE_RE = /^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)*$/;
const TARGET_RE = /^[a-z0-9_]+(?:-[a-z0-9_.]+)+$/;
const DEFINE_RE = /^[A-Za-z_][A-Za-z0-9_]*(?:=.*)?$/;
const LINT_KEYS = new Set(["rules", "macroConstants"]);
const DIFF_KEYS = new Set(["baselines", "experimental"]);
const LINT_SEVERITIES = new Set<LintSeverity>(["error", "warning", "info", "off"]);

function targetList(value: unknown, where: string): string[] {
  const list = stringList(value, where);
  for (const target of list) {
    if (!TARGET_RE.test(target)) throw new StelaError(`${where}: "${target}" is not a target triple`);
  }
  if (new Set(list).size !== list.length) throw new StelaError(`${where} lists a target twice`);
  return list;
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function stringList(value: unknown, where: string): string[] {
  if (value === undefined) return [];
  if (!Array.isArray(value) || !value.every((v) => typeof v === "string" && v !== "")) {
    throw new StelaError(`${where} must be an array of non-empty strings`);
  }
  return value as string[];
}

function lintConfig(value: unknown, where: string): LintConfig {
  if (value === undefined) return { rules: {}, macroConstants: [] };
  if (!isObject(value)) throw new StelaError(`${where} must be an object`);
  for (const key of Object.keys(value)) {
    if (!LINT_KEYS.has(key)) throw new StelaError(`${where}: unknown key "${key}"`);
  }
  const rules: Record<string, LintSeverity> = {};
  if (value.rules !== undefined) {
    if (!isObject(value.rules)) throw new StelaError(`${where}.rules must be an object`);
    for (const [rule, severity] of Object.entries(value.rules)) {
      if (!/^[a-z][a-z0-9_]*$/.test(rule) || !LINT_SEVERITIES.has(severity as LintSeverity)) {
        throw new StelaError(
          `${where}.rules.${rule} must be "error", "warning", "info", or "off"`,
        );
      }
      rules[rule] = severity as LintSeverity;
    }
  }
  const macroConstants = stringList(value.macroConstants, `${where}.macroConstants`);
  for (const name of macroConstants) {
    if (!/^[A-Z][A-Z0-9_]*$/.test(name)) {
      throw new StelaError(`${where}.macroConstants has a bad macro name "${name}"`);
    }
  }
  return { rules, macroConstants };
}

function diffConfig(
  value: unknown,
  where: string,
  root: string,
  libraries: readonly LibraryConfig[],
): DiffConfig {
  if (value === undefined) return { baselines: {}, experimental: [] };
  if (!isObject(value)) throw new StelaError(`${where} must be an object`);
  for (const key of Object.keys(value)) {
    if (!DIFF_KEYS.has(key)) throw new StelaError(`${where}: unknown key "${key}"`);
  }
  const baselines: Record<string, string> = {};
  if (value.baselines !== undefined) {
    if (!isObject(value.baselines)) throw new StelaError(`${where}.baselines must be an object`);
    for (const name of Object.keys(value.baselines)) {
      if (!libraries.some((l) => l.name === name)) {
        throw new StelaError(`${where}.baselines names unknown library "${name}"`);
      }
    }
    // Library order, so commands that walk baselines are deterministic.
    for (const library of libraries) {
      const file = value.baselines[library.name];
      if (file === undefined) continue;
      if (typeof file !== "string" || !file.endsWith(".json")) {
        throw new StelaError(`${where}.baselines.${library.name} must be a .json path`);
      }
      baselines[library.name] = path.resolve(root, file);
    }
  }
  const experimental = stringList(value.experimental, `${where}.experimental`).map((p) =>
    path.resolve(root, p),
  );
  return { baselines, experimental };
}

function binariesConfig(value: unknown, where: string, libraries: readonly LibraryConfig[]): BinaryConfig[] {
  if (value === undefined) return [];
  if (!Array.isArray(value)) throw new StelaError(`${where} must be an array`);
  const names = new Set<string>();
  return value.map((raw: unknown, index: number) => {
    const at = `${where}[${index}]`;
    if (!isObject(raw)) throw new StelaError(`${at} must be an object`);
    for (const key of Object.keys(raw)) {
      if (!BINARY_KEYS.has(key)) throw new StelaError(`${at}: unknown key "${key}"`);
    }
    const name = raw.name;
    if (typeof name !== "string" || !BINARY_NAME_RE.test(name)) {
      throw new StelaError(`${at}: "name" must be a .dll or .exe file name`);
    }
    if (names.has(name.toLowerCase())) throw new StelaError(`${at}: duplicate binary "${name}"`);
    names.add(name.toLowerCase());
    const listed = stringList(raw.libraries, `${at}: "libraries"`);
    if (listed.length === 0) throw new StelaError(`${at}: "libraries" must not be empty`);
    for (const lib of listed) {
      if (!libraries.some((l) => l.name === lib)) throw new StelaError(`${at}: unknown library "${lib}"`);
    }
    if (new Set(listed).size !== listed.length) throw new StelaError(`${at}: "libraries" lists a library twice`);
    const exports = raw.exports ?? "all";
    if (exports !== "all" && exports !== "none") throw new StelaError(`${at}: "exports" must be "all" or "none"`);
    const otherExports = raw.otherExports ?? true;
    if (typeof otherExports !== "boolean") throw new StelaError(`${at}: "otherExports" must be a boolean`);
    return {
      name,
      libraries: libraries.filter((l) => listed.includes(l.name)).map((l) => l.name),
      exports,
      otherExports,
    };
  });
}

export function parseConfig(json: unknown, file: string): StelaConfig {
  const root = path.dirname(path.resolve(file));
  if (!isObject(json)) throw new StelaError(`${file}: the config must be a JSON object`);
  for (const key of Object.keys(json)) {
    if (!TOP_LEVEL_KEYS.has(key)) throw new StelaError(`${file}: unknown key "${key}"`);
  }
  if (json.schema !== CONFIG_SCHEMA) {
    throw new StelaError(`${file}: "schema" must be "${CONFIG_SCHEMA}"`);
  }
  if (!Array.isArray(json.libraries) || json.libraries.length === 0) {
    throw new StelaError(`${file}: "libraries" must be a non-empty array`);
  }
  const globalTargets = targetList(json.targets, `${file}: "targets"`);

  const libraries: LibraryConfig[] = [];
  const names = new Set<string>();
  const macroPrefixes = new Map<string, string>();
  const outputs = new Map<string, string>();
  json.libraries.forEach((raw: unknown, index: number) => {
    const where = `${file}: libraries[${index}]`;
    if (!isObject(raw)) throw new StelaError(`${where} must be an object`);
    for (const key of Object.keys(raw)) {
      if (!LIBRARY_KEYS.has(key)) throw new StelaError(`${where}: unknown key "${key}"`);
    }
    const { name, prefix, macroPrefix } = raw;
    if (typeof name !== "string" || !LOWER_SNAKE_RE.test(name)) {
      throw new StelaError(`${where}: "name" must be lowercase_snake`);
    }
    if (names.has(name)) throw new StelaError(`${where}: duplicate library name "${name}"`);
    names.add(name);
    if (typeof prefix !== "string" || !LOWER_SNAKE_RE.test(prefix)) {
      throw new StelaError(`${where}: "prefix" must be lowercase_snake`);
    }
    if (typeof macroPrefix !== "string" || !UPPER_SNAKE_RE.test(macroPrefix)) {
      throw new StelaError(`${where}: "macroPrefix" must be UPPER_SNAKE`);
    }
    const headers = stringList(raw.headers, `${where}: "headers"`).map((h) =>
      path.resolve(root, h),
    );

    let stelaHeader: string | false;
    if (raw.stelaHeader === false) {
      stelaHeader = false;
    } else if (raw.stelaHeader === undefined) {
      if (headers.length === 0) {
        throw new StelaError(`${where}: set "stelaHeader" or list at least one header`);
      }
      stelaHeader = path.join(path.dirname(headers[0]), `${macroPrefix.toLowerCase()}_stela.h`);
    } else if (typeof raw.stelaHeader === "string" && raw.stelaHeader.endsWith(".h")) {
      stelaHeader = path.resolve(root, raw.stelaHeader);
    } else {
      throw new StelaError(`${where}: "stelaHeader" must be a .h path or false`);
    }
    if (stelaHeader !== false) {
      // Two libraries may share a status type, but never a generated file or
      // its macro namespace.
      const otherFile = outputs.get(stelaHeader.toLowerCase());
      if (otherFile) {
        throw new StelaError(`${where}: "stelaHeader" is also written by "${otherFile}"`);
      }
      outputs.set(stelaHeader.toLowerCase(), name);
      const otherPrefix = macroPrefixes.get(macroPrefix);
      if (otherPrefix) {
        throw new StelaError(`${where}: macroPrefix "${macroPrefix}" is also used by "${otherPrefix}"`);
      }
      macroPrefixes.set(macroPrefix, name);
    }

    let stelaNames: Record<string, string> = {};
    if (raw.stelaNames !== undefined) {
      if (!isObject(raw.stelaNames) || !Object.values(raw.stelaNames).every((v) => typeof v === "string")) {
        throw new StelaError(`${where}: "stelaNames" must map names to strings`);
      }
      if (stelaHeader === false) {
        throw new StelaError(`${where}: "stelaNames" needs a generated "stelaHeader"`);
      }
      stelaNames = raw.stelaNames as Record<string, string>;
    }

    const status = raw.status ?? `${prefix}_status`;
    if (typeof status !== "string" || !LOWER_SNAKE_RE.test(status)) {
      throw new StelaError(`${where}: "status" must be a lowercase_snake type name`);
    }
    const resolve = (list: string[]) => list.map((p) => path.resolve(root, p));
    const includeDirs = resolve(stringList(raw.includeDirs, `${where}: "includeDirs"`));
    const owns = raw.owns === undefined ? headers : resolve(stringList(raw.owns, `${where}: "owns"`));
    const defines = stringList(raw.defines, `${where}: "defines"`);
    for (const define of defines) {
      if (!DEFINE_RE.test(define)) throw new StelaError(`${where}: "defines" has a bad entry "${define}"`);
    }
    let targets = globalTargets;
    if (raw.targets !== undefined) {
      targets = targetList(raw.targets, `${where}: "targets"`);
      if (targets.length === 0) throw new StelaError(`${where}: "targets" must not be empty`);
      if (globalTargets.length > 0) {
        const extra = targets.filter((t) => !globalTargets.includes(t));
        if (extra.length > 0) {
          throw new StelaError(`${where}: "targets" must be a subset of the global "targets"; extra: ${extra.join(", ")}`);
        }
        // Keep the global order, so models don't depend on how a library lists them.
        targets = globalTargets.filter((t) => targets.includes(t));
      }
    }
    libraries.push({
      name,
      prefix,
      macroPrefix,
      headers,
      stelaHeader,
      stelaNames,
      status,
      includeDirs,
      owns,
      defines,
      targets,
    });
  });
  // Without a global list, the model's targets are the libraries' own, in order.
  const targets = globalTargets.length > 0 ? globalTargets : [...new Set(libraries.flatMap((l) => l.targets))];
  return {
    file: path.resolve(file),
    root,
    targets,
    libraries,
    lint: lintConfig(json.lint, `${file}: "lint"`),
    diff: diffConfig(json.diff, `${file}: "diff"`, root, libraries),
    binaries: binariesConfig(json.binaries, `${file}: "binaries"`, libraries),
  };
}

export function loadConfig(file: string): StelaConfig {
  let text: string;
  try {
    text = readFileSync(file, "utf8");
  } catch (e) {
    throw new StelaError(`cannot read ${file}: ${(e as Error).message}`);
  }
  let json: unknown;
  try {
    json = JSON.parse(text.replace(/^\uFEFF/, ""));
  } catch (e) {
    throw new StelaError(`${file}: invalid JSON: ${(e as Error).message}`);
  }
  return parseConfig(json, file);
}
