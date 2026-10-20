// Generates a library's <lib>_stela.h from the stela.h template (Stela design
// §5.7). The transform is purely textual:
// - the newest-wins guard becomes a plain <LIB>_STELA_H_ include guard;
// - every `#undef STELA_*` line goes;
// - helper macros (STELA_VERSION and names ending in `_`) become
//   <LIB>_STELA_<NAME>;
// - every other STELA_<NAME> becomes <LIB>_<NAME>, or its `stelaNames`
//   override;
// - STELA_EXTRACT keeps its name, and the annotate("stela:...") strings don't
//   change.
// Output is LF-only and depends only on the template text (line endings
// normalized) and the options, so regenerating is byte-identical.

import { createHash } from "node:crypto";
import { StelaError } from "./errors.ts";

export interface StelaTemplate {
  /** The template text with LF line endings and no BOM. */
  readonly text: string;
  /** The template's STELA_VERSION. */
  readonly version: number;
  /** SHA-256 of `text` (UTF-8), lowercase hex. */
  readonly sha256: string;
  /** Vocabulary names without the `STELA_` prefix, in template order. */
  readonly vocabulary: readonly string[];
  /** Helper names without the `STELA_` prefix, in template order. */
  readonly helpers: readonly string[];
  /** Template lines inside the version guard. */
  readonly body: readonly string[];
}

export interface VendorOptions {
  /** The library's macro prefix, such as `V8HOST`. */
  readonly macroPrefix: string;
  /** The generated file's base name, such as `v8host_stela.h`. */
  readonly fileName: string;
  /** Per-name overrides: vocabulary name (without `STELA_`) → full macro name. */
  readonly stelaNames?: Readonly<Record<string, string>>;
}

/** The one STELA_ name that generated files keep: the extractor's switch. */
export const EXTRACT_MACRO = "STELA_EXTRACT";

const MACRO_PREFIX_RE = /^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)*$/;
const MACRO_NAME_RE = /^[A-Za-z_][A-Za-z0-9_]*$/;
const STELA_TOKEN_RE = /\bSTELA_([A-Za-z0-9_]*)\b/g;
const GUARD_RE = /^#if !defined\(STELA_VERSION\) \|\| \(STELA_VERSION < (\d+)\)$/;
const GUARD_END = "#endif /* STELA_VERSION */";
const VERSION_DEFINE_RE = /^#define STELA_VERSION (\d+)$/;
const DEFINE_RE = /^#define STELA_([A-Za-z0-9_]+)\b/;

export function normalizeText(raw: string): string {
  return raw.replace(/^\uFEFF/, "").replace(/\r\n?/g, "\n");
}

function isHelper(name: string): boolean {
  return name === "VERSION" || name.endsWith("_");
}

/** Parses and validates the stela.h template. */
export function parseTemplate(raw: string): StelaTemplate {
  const text = normalizeText(raw);
  const lines = text.split("\n");
  if (lines.length > 0 && lines[lines.length - 1] === "") lines.pop();

  const guardIndex = lines.findIndex((l) => l.startsWith("#"));
  if (guardIndex < 0) throw new StelaError("template: no preprocessor lines");
  const guard = GUARD_RE.exec(lines[guardIndex]);
  if (!guard) {
    throw new StelaError(
      `template: the first directive must be "#if !defined(STELA_VERSION) || (STELA_VERSION < N)", found "${lines[guardIndex]}"`,
    );
  }
  let endIndex = lines.length - 1;
  while (endIndex > guardIndex && lines[endIndex].trim() === "") endIndex--;
  if (lines[endIndex] !== GUARD_END) {
    throw new StelaError(`template: the last line must be "${GUARD_END}"`);
  }
  // The banner is replaced, so only comments may precede the guard.
  const preamble = lines.slice(0, guardIndex).join("\n");
  if (preamble.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "").trim() !== "") {
    throw new StelaError("template: only comments may precede the version guard");
  }

  const version = Number(guard[1]);
  const body = lines.slice(guardIndex + 1, endIndex);
  const versionDefine = body.map((l) => VERSION_DEFINE_RE.exec(l)).find((m) => m);
  if (!versionDefine) throw new StelaError("template: no #define STELA_VERSION");
  if (Number(versionDefine[1]) !== version) {
    throw new StelaError(
      `template: the guard says version ${version}, but STELA_VERSION is ${versionDefine[1]}`,
    );
  }

  const vocabulary: string[] = [];
  const helpers: string[] = [];
  const seen = new Set<string>();
  for (const line of body) {
    const m = DEFINE_RE.exec(line);
    if (!m || seen.has(m[1])) continue;
    seen.add(m[1]);
    if (`STELA_${m[1]}` === EXTRACT_MACRO) {
      throw new StelaError(`template: must not define ${EXTRACT_MACRO}`);
    }
    (isHelper(m[1]) ? helpers : vocabulary).push(m[1]);
  }
  for (const line of body) {
    for (const t of line.matchAll(STELA_TOKEN_RE)) {
      if (`STELA_${t[1]}` !== EXTRACT_MACRO && !seen.has(t[1])) {
        throw new StelaError(`template: STELA_${t[1]} is used but never defined`);
      }
    }
  }

  const sha256 = createHash("sha256").update(text, "utf8").digest("hex");
  return { text, version, sha256, vocabulary, helpers, body };
}

/** The generated macro name for each template name (without `STELA_`). */
export function generatedNames(
  template: StelaTemplate,
  macroPrefix: string,
  stelaNames: Readonly<Record<string, string>> = {},
): Map<string, string> {
  if (!MACRO_PREFIX_RE.test(macroPrefix)) {
    throw new StelaError(
      `macroPrefix "${macroPrefix}" must be UPPER_SNAKE without a trailing underscore`,
    );
  }
  if (macroPrefix === "STELA") {
    throw new StelaError('macroPrefix "STELA" is reserved for the template');
  }
  for (const [key, value] of Object.entries(stelaNames)) {
    if (!template.vocabulary.includes(key)) {
      throw new StelaError(
        `stelaNames: "${key}" is not a vocabulary name in stela.h version ${template.version}`,
      );
    }
    if (!MACRO_NAME_RE.test(value) || value.startsWith("STELA_")) {
      throw new StelaError(`stelaNames: "${value}" is not a valid macro name`);
    }
  }

  const names = new Map<string, string>();
  for (const h of template.helpers) names.set(h, `${macroPrefix}_STELA_${h}`);
  for (const v of template.vocabulary) {
    names.set(v, stelaNames[v] ?? `${macroPrefix}_${v}`);
  }
  const owners = new Map<string, string>();
  for (const [from, to] of names) {
    const other = owners.get(to);
    if (other !== undefined) {
      throw new StelaError(`STELA_${from} and STELA_${other} would both become ${to}`);
    }
    owners.set(to, from);
  }
  const guard = includeGuard(macroPrefix);
  if (owners.has(guard)) {
    throw new StelaError(`STELA_${owners.get(guard)} would become the include guard ${guard}`);
  }
  return names;
}

export function includeGuard(macroPrefix: string): string {
  return `${macroPrefix}_STELA_H_`;
}

/** Re-aligns the backslashes of each multi-line macro to one column. */
function alignContinuations(lines: string[]): string[] {
  const out = lines.slice();
  for (let i = 0; i < out.length; ) {
    if (!out[i].endsWith("\\")) {
      i++;
      continue;
    }
    let j = i;
    while (j < out.length && out[j].endsWith("\\")) j++;
    const contents = out.slice(i, j).map((l) => l.slice(0, -1).trimEnd());
    const width = Math.max(...contents.map((c) => c.length));
    for (let k = i; k < j; k++) out[k] = contents[k - i].padEnd(width) + " \\";
    i = j;
  }
  return out;
}

/** Generates the text of one library's <lib>_stela.h. */
export function generateLibraryHeader(template: StelaTemplate, options: VendorOptions): string {
  const { macroPrefix, fileName } = options;
  if (!/^[A-Za-z0-9_.-]+\.h$/.test(fileName)) {
    throw new StelaError(`"${fileName}" is not a valid header file name`);
  }
  const names = generatedNames(template, macroPrefix, options.stelaNames);

  const body = template.body
    .filter((l) => !/^#undef STELA_[A-Za-z0-9_]+$/.test(l))
    .map((l) =>
      l.replace(STELA_TOKEN_RE, (token, name: string) =>
        token === EXTRACT_MACRO ? token : (names.get(name) as string),
      ),
    );
  while (body.length > 0 && body[0].trim() === "") body.shift();
  while (body.length > 0 && body[body.length - 1].trim() === "") body.pop();

  const guard = includeGuard(macroPrefix);
  const lines = [
    `/* ${fileName}: GENERATED by Stela. Do not edit; run \`stela vendor\` to regenerate.`,
    ` * Prefix: ${macroPrefix}. Source: stela.h version ${template.version}`,
    ` * (sha256 ${template.sha256}).`,
    " * Annotations compile to nothing outside the Stela extractor.",
    " *",
    " * Copyright (c) Microsoft Corporation.",
    " * Licensed under the MIT License. */",
    `#ifndef ${guard}`,
    `#define ${guard}`,
    "",
    ...alignContinuations(body),
    "",
    `#endif /* ${guard} */`,
  ];
  const text = lines.join("\n") + "\n";

  const leaked = [...text.matchAll(STELA_TOKEN_RE)].find((m) => m[0] !== EXTRACT_MACRO);
  if (leaked) throw new StelaError(`internal: ${leaked[0]} survived generation`);
  return text;
}
