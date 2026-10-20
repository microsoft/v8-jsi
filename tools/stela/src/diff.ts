// Stela ABI diff (design §10): compares two stela.model/1 models of the same
// configuration, typically a release baseline and the current tree, and
// classifies each change as a break, an addition, or a note.

import type { StelaConfig } from "./config.ts";
import { StelaError } from "./errors.ts";
import {
  type Annotation,
  type Declaration,
  type Field,
  type Model,
  type ModelLibrary,
  type Parameter,
  type TargetFacts,
  type TypeNode,
  MODEL_SCHEMA,
  readModel,
  relativePath,
} from "./model.ts";

export type ChangeClass = "break" | "addition" | "note";

export interface DiffFinding {
  readonly class: ChangeClass;
  /** The change kind, such as `removed` or `signature_changed`. */
  readonly change: string;
  readonly library: string;
  readonly file: string;
  /** 0 for a library-level change. */
  readonly line: number;
  readonly column: number;
  /** `<kind> <name>`, or "" for a library-level change. */
  readonly declaration: string;
  readonly message: string;
  /**
   * The declaration is experimental (annotated, or in an experimental
   * header): a break is reported as a note, and an addition needs no
   * version bump.
   */
  readonly experimental?: true;
}

export interface DiffOptions {
  /** Headers, relative to the configuration root with `/`, whose breaks are notes. */
  readonly experimentalHeaders?: readonly string[];
  /** Compare only these libraries. Default: every library in either model. */
  readonly libraries?: readonly string[];
}

interface FunctionShape {
  readonly result: TypeNode;
  readonly params: readonly Parameter[];
  readonly variadic: boolean;
}

/** An annotation key mapped to its value ("" for keys without one). */
type AnnotationMap = Map<string, string>;

/**
 * Where a value flows. Only plain functions have a known direction: a callback
 * or a function-table slot may be implemented by either side.
 */
type Position = "input" | "output" | "inout" | "result" | "unknown";

/** Keys whose change alters the contract. */
const CONTRACT_KEYS = new Set([
  "out",
  "inout",
  "transfer",
  "scope",
  "count",
  "closure",
  "destroy",
  "encoding",
  "zero_terminated",
  "enum",
  "flags",
]);
const REFERENCE_KEYS = new Set(["count", "closure", "destroy"]);
const VERSION_RE = /_ABI_VERSION$/;
const CREATE_RE = /_(?:create|open|copy)$/;

interface LibraryContext {
  readonly library: string;
  readonly experimental: ReadonlySet<string>;
  /** Typedefs of both models, by name, to recognize callback types. */
  readonly typedefs: ReadonlyMap<string, Declaration>;
  readonly declarations: readonly [readonly Declaration[], readonly Declaration[]];
  readonly findings: DiffFinding[];
}

interface Pair {
  readonly base?: Declaration;
  readonly head?: Declaration;
}

// --- Types ------------------------------------------------------------------

function prop<T>(node: TypeNode | undefined, key: string): T | undefined {
  return node === undefined ? undefined : (node[key] as T | undefined);
}

function pointerFunction(type: TypeNode | undefined): FunctionShape | undefined {
  return prop<FunctionShape>(prop<TypeNode>(type, "pointer"), "function");
}

/** A type without parameter names, annotations, or docs: what the ABI sees. */
function shape(type: TypeNode): unknown {
  const out: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(type)) {
    if (key === "pointer" || key === "array") {
      out[key] = shape(value as TypeNode);
    } else if (key === "function") {
      const fn = value as unknown as FunctionShape;
      out[key] = { result: shape(fn.result), params: fn.params.map((p) => shape(p.type)), variadic: fn.variadic };
    } else if (key === "struct" || key === "union") {
      const fields = (value as unknown as { fields: Field[] }).fields;
      out[key] = fields.map((f) => [f.name, shape(f.type), f.bitWidth ?? null]);
    } else if (key === "enum") {
      const e = value as unknown as { type: TypeNode; enumerators: { name: string; value: number }[] };
      out[key] = { type: shape(e.type), enumerators: e.enumerators.map((x) => [x.name, x.value]) };
    } else {
      out[key] = value;
    }
  }
  return out;
}

function typeKey(type: TypeNode | undefined): string {
  return type === undefined ? "" : JSON.stringify(shape(type));
}

function renderParams(fn: FunctionShape): string {
  const params = fn.params.map((p) => renderType(p.type));
  if (fn.variadic) params.push("...");
  return params.length === 0 ? "void" : params.join(", ");
}

/** A short C-like spelling of a type node, for messages. */
export function renderType(type: TypeNode | undefined): string {
  if (type === undefined) return "?";
  const isConst = type.const === true;
  const pointer = prop<TypeNode>(type, "pointer");
  if (pointer !== undefined) {
    const fn = prop<FunctionShape>(pointer, "function");
    if (fn !== undefined) return `${renderType(fn.result)} (*${isConst ? "const" : ""})(${renderParams(fn)})`;
    return `${renderType(pointer)}*${isConst ? " const" : ""}`;
  }
  const fn = prop<FunctionShape>(type, "function");
  if (fn !== undefined) return `${renderType(fn.result)}(${renderParams(fn)})`;
  const array = prop<TypeNode>(type, "array");
  if (array !== undefined) return `${renderType(array)}[${String(type.length ?? "")}]`;
  const prefix = isConst ? "const " : "";
  for (const kind of ["struct", "union", "enum"]) {
    if (type[kind] !== undefined) return `${prefix}${kind} {…}`;
  }
  return `${prefix}${String(type.name ?? type.builtin ?? "?")}`;
}

function isCallbackType(context: LibraryContext, type: TypeNode): boolean {
  if (pointerFunction(type) !== undefined) return true;
  const name = prop<string>(type, "name");
  if (name === undefined) return false;
  return name.endsWith("_cb") || pointerFunction(context.typedefs.get(name)?.type) !== undefined;
}

function textUnitName(type: TypeNode): string | undefined {
  const pointed = prop<TypeNode>(type, "pointer");
  if (pointed === undefined) return undefined;
  const name = prop<string>(pointed, "name") ?? prop<string>(pointed, "builtin");
  if (name === undefined) return undefined;
  return ["char", "char8_t", "char16_t", "wchar_t"].includes(name) || name.endsWith("_char16") ? name : undefined;
}

function encodingFromName(name: string): string | undefined {
  return /_(utf8|utf16|latin1)(?:_|$)/.exec(name)?.[1];
}

// --- Annotations ------------------------------------------------------------
//
// Annotations are compared in effect: an annotation that only states what a
// name or position already implies (design §5.5) is the same as no
// annotation, and references (`count=p`) resolve to positions, so renaming
// the referenced parameter isn't a contract change.

function explicitAnnotations(annotations: readonly Annotation[], names: readonly string[]): AnnotationMap {
  const map: AnnotationMap = new Map();
  for (const annotation of annotations) {
    if (annotation.key === "allow") continue;
    const value = annotation.value ?? "";
    if (REFERENCE_KEYS.has(annotation.key)) {
      const index = names.indexOf(value);
      map.set(annotation.key, index < 0 ? `?${value}` : `#${index}`);
    } else {
      map.set(annotation.key, value);
    }
  }
  return map;
}

function parameterAnnotations(
  context: LibraryContext,
  siteName: string,
  creates: boolean,
  params: readonly Parameter[],
  index: number,
): AnnotationMap {
  const parameter = params[index];
  const next = params[index + 1];
  const map = explicitAnnotations(
    parameter.annotations,
    params.map((p) => p.name),
  );
  if (parameter.name.startsWith("out_")) map.set("out", "");
  const unit = textUnitName(parameter.type);
  if (!map.has("encoding") && unit !== undefined) {
    const encoding =
      encodingFromName(parameter.name) ??
      encodingFromName(siteName) ??
      (unit.endsWith("_char16") ? "utf16" : undefined);
    if (encoding !== undefined) map.set("encoding", encoding);
  }
  if (
    !map.has("count") &&
    next !== undefined &&
    prop(parameter.type, "pointer") !== undefined &&
    (/^(?:length|count|size)$/.test(next.name) || next.name.startsWith(`${parameter.name}_`) &&
      /^(?:length|count|size)$/.test(next.name.slice(parameter.name.length + 1)))
  ) {
    map.set("count", `#${index + 1}`);
  }
  if (isCallbackType(context, parameter.type)) {
    if (!map.has("scope")) map.set("scope", "call");
    if (!map.has("closure") && next?.name === "user_data") map.set("closure", `#${index + 1}`);
  }
  if (!map.has("transfer")) map.set("transfer", creates && map.has("out") ? "full" : "none");
  return map;
}

function fieldAnnotations(fields: readonly Field[], index: number): AnnotationMap {
  const field = fields[index];
  const map = explicitAnnotations(
    field.annotations,
    fields.map((f) => f.name),
  );
  const unit = textUnitName(field.type);
  if (!map.has("encoding") && unit !== undefined) {
    const encoding = encodingFromName(field.name) ?? (unit.endsWith("_char16") ? "utf16" : undefined);
    if (encoding !== undefined) map.set("encoding", encoding);
  }
  if (!map.has("transfer")) map.set("transfer", "none");
  return map;
}

function declarationAnnotations(declaration: Declaration): AnnotationMap {
  const map = explicitAnnotations(declaration.annotations, []);
  if (declaration.kind === "function" && !map.has("transfer")) map.set("transfer", "none");
  return map;
}

function displayValue(value: string, names: readonly string[]): string {
  if (value.startsWith("#")) return names[Number(value.slice(1))] ?? value;
  if (value.startsWith("?")) return value.slice(1);
  return value;
}

function classify(key: string, added: boolean, position: Position): [ChangeClass, string] {
  switch (key) {
    case "nullable":
      if (position === "input") return [added ? "addition" : "break", "nullable_changed"];
      if (position === "output" || position === "result") return [added ? "break" : "note", "nullable_changed"];
      return ["break", "nullable_changed"];
    case "optional":
      return [position === "output" && added ? "addition" : "break", "optional_changed"];
    case "deprecated":
      return ["note", "deprecated_changed"];
    default:
      if (CONTRACT_KEYS.has(key)) return ["break", "annotation_changed"];
      // `since`, `experimental`, and project keys describe; they don't bind.
      return ["note", "metadata_changed"];
  }
}

function compareAnnotations(
  context: LibraryContext,
  pair: Pair,
  label: string,
  before: AnnotationMap,
  after: AnnotationMap,
  position: Position,
  beforeNames: readonly string[] = [],
  afterNames: readonly string[] = [],
): void {
  const keys = [...new Set([...before.keys(), ...after.keys()])].sort();
  for (const key of keys) {
    const b = before.get(key);
    const a = after.get(key);
    if (b === a) continue;
    const added = b === undefined;
    const removed = a === undefined;
    const [cls, change] = classify(key, added, position);
    const show = (value: string, names: readonly string[]) => {
      const v = displayValue(value, names);
      return v === "" ? key : `${key}=${v}`;
    };
    const message = added
      ? `"${show(a!, afterNames)}" added to ${label}`
      : removed
        ? `"${show(b, beforeNames)}" removed from ${label}`
        : `${label}: "${show(b, beforeNames)}" changed to "${show(a!, afterNames)}"`;
    report(context, cls, change, pair, message);
  }
}

function positionOf(kind: "function" | "callback", annotations: AnnotationMap): Position {
  if (kind !== "function") return "unknown";
  if (annotations.has("inout")) return "inout";
  if (annotations.has("out")) return "output";
  return "input";
}

/** Parameter names and annotations of two signatures with the same shape. */
function compareParameters(
  context: LibraryContext,
  pair: Pair,
  kind: "function" | "callback",
  names: readonly [string, string],
  params: readonly [readonly Parameter[], readonly Parameter[]],
  where: string,
): void {
  const [beforeParams, afterParams] = params;
  const beforeNames = beforeParams.map((p) => p.name);
  const afterNames = afterParams.map((p) => p.name);
  for (let i = 0; i < beforeParams.length && i < afterParams.length; i++) {
    const before = beforeParams[i];
    const after = afterParams[i];
    if (before.name !== after.name) {
      report(
        context,
        "note",
        "parameter_renamed",
        pair,
        `${where}parameter ${i + 1} renamed from "${before.name}" to "${after.name}"`,
      );
    }
    const b = parameterAnnotations(context, names[0], kind === "function" && CREATE_RE.test(names[0]), beforeParams, i);
    const a = parameterAnnotations(context, names[1], kind === "function" && CREATE_RE.test(names[1]), afterParams, i);
    compareAnnotations(
      context,
      pair,
      `${where}parameter "${after.name}"`,
      b,
      a,
      positionOf(kind, a),
      beforeNames,
      afterNames,
    );
  }
}

// --- Reporting --------------------------------------------------------------

function isExperimental(context: LibraryContext, declaration: Declaration | undefined): boolean {
  return (
    declaration !== undefined &&
    (declaration.annotations.some((a) => a.key === "experimental") ||
      context.experimental.has(declaration.location.file))
  );
}

function report(context: LibraryContext, cls: ChangeClass, change: string, pair: Pair, detail: string): void {
  const declaration = pair.head ?? pair.base!;
  // The released contract decides: marking a stable declaration experimental,
  // or moving it into an experimental header, doesn't excuse breaking it.
  const experimental = isExperimental(context, pair.base ?? pair.head);
  const downgraded = experimental && cls === "break";
  context.findings.push({
    class: downgraded ? "note" : cls,
    change,
    library: context.library,
    file: declaration.location.file,
    line: declaration.location.line,
    column: declaration.location.column,
    declaration: `${declaration.kind} ${declaration.name}`,
    message: `${declaration.kind} ${declaration.name}: ${detail}${downgraded ? " (experimental)" : ""}`,
    ...(experimental ? { experimental: true as const } : {}),
  });
}

/** Groups targets by a per-target value change: one report per distinct change. */
function perTarget(
  pair: Pair,
  targets: readonly string[],
  value: (facts: TargetFacts | undefined) => string | undefined,
  emit: (from: string | undefined, to: string | undefined, where: string) => void,
): void {
  const groups = new Map<string, { from?: string; to?: string; targets: string[] }>();
  for (const target of targets) {
    const from = value(pair.base!.perTarget?.[target]);
    const to = value(pair.head!.perTarget?.[target]);
    if (from === to) continue;
    const key = JSON.stringify([from ?? null, to ?? null]);
    const group = groups.get(key) ?? { from, to, targets: [] };
    group.targets.push(target);
    groups.set(key, group);
  }
  for (const group of groups.values()) {
    const where = group.targets.length === targets.length ? "" : ` on ${group.targets.join(", ")}`;
    emit(group.from, group.to, where);
  }
}

const show = (value: string | undefined) => value ?? "none";

// --- Declarations -----------------------------------------------------------

function diffFunction(context: LibraryContext, pair: Pair, targets: readonly string[]): void {
  const signature = (d: Declaration): FunctionShape => ({
    result: d.result?.type ?? { builtin: "void" },
    params: d.params ?? [],
    variadic: d.variadic === true,
  });
  const before = signature(pair.base!);
  const after = signature(pair.head!);
  const fnType = (fn: FunctionShape) => ({ function: fn }) as unknown as TypeNode;
  if (typeKey(fnType(before)) !== typeKey(fnType(after))) {
    report(
      context,
      "break",
      "signature_changed",
      pair,
      `signature changed from ${renderType(fnType(before))} to ${renderType(fnType(after))}`,
    );
  } else {
    compareParameters(context, pair, "function", [pair.base!.name, pair.head!.name], [before.params, after.params], "");
  }
  perTarget(pair, targets, (f) => f?.callingConvention, (from, to, where) =>
    report(context, "break", "calling_convention_changed", pair, `calling convention${where} changed from ${show(from)} to ${show(to)}`),
  );
}

function diffLayout(
  context: LibraryContext,
  pair: Pair,
  targets: readonly string[],
  allow: { size?: boolean; align?: boolean } = {},
): void {
  perTarget(pair, targets, (f) => f?.size?.toString(), (from, to, where) => {
    const grew = from !== undefined && to !== undefined && Number(to) > Number(from);
    if (!(allow.size && grew)) {
      report(context, "break", "layout_changed", pair, `size${where} changed from ${show(from)} to ${show(to)}`);
    }
  });
  perTarget(pair, targets, (f) => f?.align?.toString(), (from, to, where) => {
    if (!allow.align) {
      report(context, "break", "layout_changed", pair, `alignment${where} changed from ${show(from)} to ${show(to)}`);
    }
  });
}

function diffCanonical(context: LibraryContext, pair: Pair, targets: readonly string[]): void {
  perTarget(pair, targets, (f) => (f?.canonicalType === undefined ? undefined : typeKey(f.canonicalType)), (_from, _to, where) => {
    report(context, "break", "type_changed", pair, `canonical type${where} changed`);
  });
}

function diffTypedef(context: LibraryContext, pair: Pair, targets: readonly string[]): void {
  const { base, head } = pair as Required<Pair>;
  if (typeKey(base.type) !== typeKey(head.type)) {
    report(context, "break", "type_changed", pair, `type changed from ${renderType(base.type)} to ${renderType(head.type)}`);
    return;
  }
  const before = pointerFunction(base.type);
  const after = pointerFunction(head.type);
  if (before !== undefined && after !== undefined) {
    compareParameters(context, pair, "callback", [base.name, head.name], [before.params, after.params], "");
  }
  diffCanonical(context, pair, targets);
  const recordBase = resolveRecord(base.type, context.declarations[0]);
  const recordHead = resolveRecord(head.type, context.declarations[1]);
  const grows = recordBase !== undefined && recordHead !== undefined && isEndGrowth(context, recordBase, recordHead);
  diffLayout(context, pair, targets, { size: grows, align: grows });
  perTarget(pair, targets, (f) => f?.callingConvention, (from, to, where) =>
    report(context, "break", "calling_convention_changed", pair, `calling convention${where} changed from ${show(from)} to ${show(to)}`),
  );
}

function isFunctionField(context: LibraryContext, declaration: Declaration, index: number): boolean {
  const field = declaration.fields![index];
  if (pointerFunction(field.type) !== undefined) return true;
  const name = prop<string>(field.type, "name");
  if (name !== undefined && pointerFunction(context.typedefs.get(name)?.type) !== undefined) return true;
  return Object.values(declaration.perTarget ?? {}).some((f) => (f.fieldCallingConventions?.[index] ?? null) !== null);
}

/** A struct whose fields, after an optional leading `struct_size`, are all function pointers (design §10). */
function isFunctionTable(context: LibraryContext, declaration: Declaration): boolean {
  const fields = declaration.fields ?? [];
  if (declaration.kind !== "struct") return false;
  const start = fields[0]?.name === "struct_size" ? 1 : 0;
  if (fields.length <= start) return false;
  for (let i = start; i < fields.length; i++) if (!isFunctionField(context, declaration, i)) return false;
  return true;
}

/** Resolve only by-value record aliases, never pointers or arrays. */
function resolveRecord(type: TypeNode | undefined, declarations: readonly Declaration[]): Declaration | undefined {
  const seen = new Set<string>();
  let name = prop<string>(type, "name");
  while (name !== undefined && !seen.has(name)) {
    seen.add(name);
    const record = declarations.find((d) => (d.kind === "struct" || d.kind === "union") && d.name === name);
    if (record !== undefined) return record;
    name = prop<string>(declarations.find((d) => d.kind === "typedef" && d.name === name)?.type, "name");
  }
  return undefined;
}

function isGrowableRecord(context: LibraryContext, declaration: Declaration): boolean {
  return declaration.kind === "struct" &&
    (declaration.fields?.[0]?.name === "struct_size" || isFunctionTable(context, declaration));
}

/** Only an appended tail can explain a growable record's larger layout. */
function isEndGrowth(context: LibraryContext, base: Declaration, head: Declaration): boolean {
  const before = base.fields ?? [];
  const after = head.fields ?? [];
  return !base.incomplete && !head.incomplete && base.kind === head.kind && base.tagless === head.tagless &&
    isGrowableRecord(context, base) && after.length > before.length && before.every((b, i) => {
      const a = after[i];
      return b.name === a.name && typeKey(b.type) === typeKey(a.type) && b.bitWidth === a.bitWidth;
    });
}

function diffRecord(context: LibraryContext, pair: Pair, targets: readonly string[]): void {
  const { base, head } = pair as Required<Pair>;
  if (base.incomplete !== head.incomplete) {
    if (base.incomplete) report(context, "addition", "definition_changed", pair, "the opaque record is now defined");
    else report(context, "break", "definition_changed", pair, "the record's definition was removed");
    return;
  }
  if (base.tagless !== head.tagless) {
    report(context, "break", "definition_changed", pair, head.tagless ? "the record lost its tag" : "the record gained a tag");
  }
  if (base.incomplete) return;

  const before = base.fields ?? [];
  const after = head.fields ?? [];
  const table = isFunctionTable(context, base);
  const growable = isGrowableRecord(context, base);
  const common = Math.min(before.length, after.length);
  let structural = false;
  for (let i = 0; i < common; i++) {
    const b = before[i];
    const a = after[i];
    if (b.name !== a.name || typeKey(b.type) !== typeKey(a.type) || b.bitWidth !== a.bitWidth) {
      const spell = (f: Field) => `"${f.name}" (${renderType(f.type)}${f.bitWidth === undefined ? "" : ` : ${f.bitWidth}`})`;
      const tail = table ? "; table entries can only be appended" : "";
      report(context, "break", "field_changed", pair, `field ${i + 1} changed from ${spell(b)} to ${spell(a)}${tail}`);
      structural = true;
      continue;
    }
    if (b.doc !== a.doc) report(context, "note", "doc_changed", pair, `documentation of field "${a.name}" changed`);
    compareAnnotations(
      context,
      pair,
      `field "${a.name}"`,
      fieldAnnotations(before, i),
      fieldAnnotations(after, i),
      "unknown",
      before.map((f) => f.name),
      after.map((f) => f.name),
    );
    const fb = pointerFunction(b.type);
    const fa = pointerFunction(a.type);
    if (fb !== undefined && fa !== undefined) {
      compareParameters(context, pair, "callback", [b.name, a.name], [fb.params, fa.params], `field "${a.name}" `);
    }
  }
  for (const field of before.slice(common)) {
    report(context, "break", "field_removed", pair, `field "${field.name}" was removed`);
    structural = true;
  }
  const appended = after.slice(common);
  for (const field of appended) {
    if (growable) {
      report(context, "addition", "field_added", pair, `field "${field.name}" was appended`);
    } else {
      report(
        context,
        "break",
        "field_added",
        pair,
        `field "${field.name}" was added; only a struct that starts with struct_size, or a function table, can grow`,
      );
      structural = true;
    }
  }
  if (structural) return;

  const grows = isEndGrowth(context, base, head);
  diffLayout(context, pair, targets, { size: grows, align: grows });
  for (let i = 0; i < common; i++) {
    const name = after[i].name;
    perTarget(pair, targets, (f) => f?.offsets?.[i]?.toString(), (from, to, where) =>
      report(context, "break", "layout_changed", pair, `offset of "${name}"${where} changed from ${show(from)} to ${show(to)}`),
    );
    perTarget(pair, targets, (f) => f?.fieldCallingConventions?.[i] ?? undefined, (from, to, where) =>
      report(context, "break", "calling_convention_changed", pair, `calling convention of "${name}"${where} changed from ${show(from)} to ${show(to)}`),
    );
  }
}

function diffEnum(context: LibraryContext, pair: Pair, targets: readonly string[]): void {
  const { base, head } = pair as Required<Pair>;
  if (typeKey(base.type) !== typeKey(head.type)) {
    report(context, "break", "type_changed", pair, `underlying type changed from ${renderType(base.type)} to ${renderType(head.type)}`);
  } else {
    diffCanonical(context, pair, targets);
  }
  const after = new Map((head.enumerators ?? []).map((e) => [e.name, e]));
  const beforeNames = new Set<string>();
  for (const b of base.enumerators ?? []) {
    beforeNames.add(b.name);
    const a = after.get(b.name);
    if (a === undefined) {
      report(context, "break", "enumerator_removed", pair, `enumerator ${b.name} was removed`);
    } else if (a.value !== b.value) {
      report(context, "break", "enumerator_value_changed", pair, `enumerator ${b.name} changed from ${b.value} to ${a.value}`);
    } else if (a.doc !== b.doc) {
      report(context, "note", "doc_changed", pair, `documentation of enumerator ${b.name} changed`);
    }
  }
  for (const a of head.enumerators ?? []) {
    if (!beforeNames.has(a.name)) report(context, "addition", "enumerator_added", pair, `enumerator ${a.name} = ${a.value} was added`);
  }
  diffLayout(context, pair, targets);
}

function diffMacro(context: LibraryContext, pair: Pair, targets: readonly string[]): void {
  const { base, head } = pair as Required<Pair>;
  if ((base.functionLike === true) !== (head.functionLike === true)) {
    report(context, "break", "definition_changed", pair, head.functionLike ? "became function-like" : "is no longer function-like");
    return;
  }
  let valued = false;
  perTarget(pair, targets, (f) => {
    if (f?.value !== undefined) valued = true;
    return f?.value;
  }, (from, to, where) => {
    if (from !== undefined && to !== undefined && VERSION_RE.test(head.name) && BigInt(to) > BigInt(from)) {
      report(context, "note", "abi_version_increased", pair, `increased${where} from ${from} to ${to}`);
    } else {
      report(context, "break", "macro_value_changed", pair, `value${where} changed from ${show(from)} to ${show(to)}`);
    }
  });
  for (const target of targets) {
    if (base.perTarget?.[target]?.value !== undefined) valued = true;
  }
  const tokens = (d: Declaration) => (d.replacement ?? []).join(" ");
  if (!valued && tokens(base) !== tokens(head)) {
    report(context, "note", "macro_changed", pair, `replacement changed from "${tokens(base)}" to "${tokens(head)}"`);
  }
}

function diffDeclaration(context: LibraryContext, pair: Pair, targets: readonly string[]): void {
  const { base, head } = pair as Required<Pair>;
  if (base.doc !== head.doc) report(context, "note", "doc_changed", pair, "documentation changed");
  compareAnnotations(
    context,
    pair,
    base.kind === "function" ? "the result" : "the declaration",
    declarationAnnotations(base),
    declarationAnnotations(head),
    base.kind === "function" ? "result" : "unknown",
  );
  switch (base.kind) {
    case "function":
      return diffFunction(context, pair, targets);
    case "typedef":
      return diffTypedef(context, pair, targets);
    case "struct":
    case "union":
      return diffRecord(context, pair, targets);
    case "enum":
      return diffEnum(context, pair, targets);
    case "macro":
      return diffMacro(context, pair, targets);
  }
}

// --- Libraries ----------------------------------------------------------------

function libraryTargets(model: Model, library: ModelLibrary): readonly string[] {
  return library.targets ?? model.targets;
}

function versionsOf(library: ModelLibrary): Map<string, bigint> {
  const versions = new Map<string, bigint>();
  for (const d of library.declarations) {
    if (d.kind !== "macro" || !VERSION_RE.test(d.name)) continue;
    const value = Object.values(d.perTarget ?? {}).find((f) => f.value !== undefined)?.value;
    if (value !== undefined) versions.set(d.name, BigInt(value));
  }
  return versions;
}

/**
 * The library's `*_ABI_VERSION` macros. An extension library without its own
 * uses those of the library that declares its status type (design §8.2).
 */
function versionMacros(model: Model, library: ModelLibrary): Map<string, bigint> {
  const own = versionsOf(library);
  if (own.size > 0) return own;
  const owner = model.libraries.find(
    (l) =>
      l !== library &&
      l.declarations.some((d) => (d.kind === "enum" || d.kind === "typedef") && d.name === library.status),
  );
  return owner === undefined ? own : versionsOf(owner);
}

function libraryFinding(
  cls: ChangeClass,
  change: string,
  library: ModelLibrary,
  message: string,
  experimental: boolean,
): DiffFinding {
  const downgraded = experimental && cls === "break";
  return {
    class: downgraded ? "note" : cls,
    change,
    library: library.name,
    file: library.headers[0] ?? "",
    line: 0,
    column: 0,
    declaration: "",
    message: `library ${library.name}: ${message}${downgraded ? " (experimental)" : ""}`,
    ...(experimental ? { experimental: true as const } : {}),
  };
}

function diffLibrary(
  context: LibraryContext,
  models: readonly [Model, Model],
  libraries: readonly [ModelLibrary, ModelLibrary],
): void {
  const [baseModel, headModel] = models;
  const [baseLib, headLib] = libraries;
  const baseTargets = libraryTargets(baseModel, baseLib);
  const headTargets = libraryTargets(headModel, headLib);
  const experimental = baseLib.headers.length > 0 && baseLib.headers.every((h) => context.experimental.has(h));
  for (const target of baseTargets) {
    if (!headTargets.includes(target)) {
      context.findings.push(libraryFinding("break", "target_removed", headLib, `target ${target} was removed`, experimental));
    }
  }
  for (const target of headTargets) {
    if (!baseTargets.includes(target)) {
      context.findings.push(libraryFinding("addition", "target_added", headLib, `target ${target} was added`, experimental));
    }
  }
  const common = baseTargets.filter((t) => headTargets.includes(t));
  const key = (d: Declaration) => `${d.kind} ${d.name}`;
  const heads = new Map(headLib.declarations.map((d) => [key(d), d]));
  const bases = new Set(baseLib.declarations.map(key));

  for (const base of baseLib.declarations) {
    const head = heads.get(key(base));
    if (head === undefined) {
      report(context, "break", "removed", { base }, "was removed");
      continue;
    }
    const pair = { base, head };
    const beforeOn = base.targets ?? baseTargets;
    const afterOn = head.targets ?? headTargets;
    const lost = common.filter((t) => beforeOn.includes(t) && !afterOn.includes(t));
    const gained = common.filter((t) => !beforeOn.includes(t) && afterOn.includes(t));
    if (lost.length > 0) report(context, "break", "removed", pair, `was removed on ${lost.join(", ")}`);
    if (gained.length > 0) report(context, "addition", "added", pair, `was added on ${gained.join(", ")}`);
    diffDeclaration(context, pair, common.filter((t) => beforeOn.includes(t) && afterOn.includes(t)));
  }
  for (const head of headLib.declarations) {
    if (!bases.has(key(head))) report(context, "addition", "added", { head }, "was added");
  }

  const additions = context.findings.filter((f) => f.class === "addition" && f.experimental !== true);
  if (additions.length === 0) return;
  const before = versionMacros(baseModel, baseLib);
  const after = versionMacros(headModel, headLib);
  const bumped = [...after].some(([name, value]) => {
    const old = before.get(name);
    return old === undefined || value > old;
  });
  if (bumped) return;
  const names = [...after.keys()];
  context.findings.push(
    libraryFinding(
      "break",
      "version_not_bumped",
      headLib,
      `${additions.length} addition(s), but ` +
        (names.length === 0 ? "no *_ABI_VERSION macro covers the library" : `${names.join(", ")} didn't increase`),
      false,
    ),
  );
}

/** Compares two models (design §10). Findings keep library order, then source position. */
export function diffModels(base: Model, head: Model, options: DiffOptions = {}): DiffFinding[] {
  const only = options.libraries === undefined ? undefined : new Set(options.libraries);
  const selected = (library: ModelLibrary) => only === undefined || only.has(library.name);
  const experimental = new Set(options.experimentalHeaders ?? []);
  const typedefs = new Map<string, Declaration>();
  for (const model of [base, head]) {
    for (const library of model.libraries) {
      for (const d of library.declarations) if (d.kind === "typedef") typedefs.set(d.name, d);
    }
  }
  const heads = new Map(head.libraries.map((l) => [l.name, l]));
  const bases = new Set(base.libraries.map((l) => l.name));
  const groups: DiffFinding[][] = [];
  for (const baseLib of base.libraries.filter(selected)) {
    const headLib = heads.get(baseLib.name);
    const libExperimental = baseLib.headers.length > 0 && baseLib.headers.every((h) => experimental.has(h));
    if (headLib === undefined) {
      groups.push([libraryFinding("break", "library_removed", baseLib, "was removed", libExperimental)]);
      continue;
    }
    const context: LibraryContext = {
      library: baseLib.name, experimental, typedefs, declarations: [baseLib.declarations, headLib.declarations], findings: [],
    };
    diffLibrary(context, [base, head], [baseLib, headLib]);
    groups.push(context.findings);
  }
  for (const headLib of head.libraries.filter(selected)) {
    if (!bases.has(headLib.name)) {
      groups.push([libraryFinding("addition", "library_added", headLib, "was added", false)]);
    }
  }
  const compareText = (a: string, b: string) => (a < b ? -1 : a > b ? 1 : 0);
  // Target changes lead a library's findings; the version check sums them up.
  const rank = (f: DiffFinding) => (f.declaration !== "" ? 1 : f.change === "version_not_bumped" ? 2 : 0);
  return groups.flatMap((group) =>
    [...group].sort(
      (a, b) =>
        rank(a) - rank(b) ||
        compareText(a.file, b.file) ||
        a.line - b.line ||
        a.column - b.column ||
        compareText(a.change, b.change) ||
        compareText(a.message, b.message),
    ),
  );
}

export function formatDiff(findings: readonly DiffFinding[], format: "text" | "json"): string {
  if (format === "json") return JSON.stringify(findings, null, 2);
  return findings
    .map((f) => `${f.line > 0 ? `${f.file}:${f.line}:${f.column}` : f.file}: ${f.class}[${f.change}]: ${f.message}`)
    .join("\n");
}

export function summarizeDiff(findings: readonly DiffFinding[]): string {
  const counts = { break: 0, addition: 0, note: 0 };
  for (const f of findings) counts[f.class]++;
  return (
    `${findings.length} change(s): ${counts.break} break(s), ` +
    `${counts.addition} addition(s), ${counts.note} note(s)`
  );
}

// --- Baselines --------------------------------------------------------------

/** One library of `model` as a model of its own, as baselines are stored. */
export function libraryModel(model: Model, name: string): Model {
  const library = model.libraries.find((l) => l.name === name);
  if (library === undefined) throw new StelaError(`library "${name}" is missing from the model`);
  return { schema: model.schema, producer: model.producer, targets: model.targets, libraries: [library] };
}

/** The configured baselines (design §8.2 `diff.baselines`) merged into one model. */
export function loadBaselines(config: StelaConfig): Model {
  const names = Object.keys(config.diff.baselines);
  if (names.length === 0) throw new StelaError(`${config.file}: no "diff.baselines" are configured`);
  const libraries: ModelLibrary[] = [];
  const targets: string[] = [];
  let producer: Model["producer"] | undefined;
  for (const name of names) {
    const file = config.diff.baselines[name];
    let model: Model;
    try {
      model = readModel(file);
    } catch (e) {
      throw new StelaError(`${(e as Error).message}; run \`stela baseline ${name}\` to create it`);
    }
    const library = model.libraries.find((l) => l.name === name);
    if (library === undefined) throw new StelaError(`${relativePath(config.root, file)} has no library "${name}"`);
    producer ??= model.producer;
    const own = libraryTargets(model, library);
    for (const t of own) if (!targets.includes(t)) targets.push(t);
    libraries.push({ ...library, targets: [...own] });
  }
  return { schema: MODEL_SCHEMA, producer: producer!, targets, libraries };
}

/** The experimental headers of a configuration, as model paths. */
export function experimentalHeaders(config: StelaConfig): string[] {
  return config.diff.experimental.map((file) => relativePath(config.root, file));
}
