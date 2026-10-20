import assert from "node:assert/strict";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { test } from "node:test";
import { parseConfig } from "../src/config.ts";
import { type DiffFinding, diffModels, formatDiff, summarizeDiff } from "../src/diff.ts";
import { StelaError } from "../src/errors.ts";
import {
  type Annotation,
  type Declaration,
  type Field,
  type Model,
  type ModelLibrary,
  type Parameter,
  type TargetFacts,
  type TypeNode,
  serializeModel,
} from "../src/model.ts";
import { runCli, tempDir } from "./helpers.ts";

const X64 = "x86_64-pc-windows-msvc";
const X86 = "i686-pc-windows-msvc";
const TARGETS = [X64, X86];
const location = { file: "include/alpha.h", line: 10, column: 1 };

const named = (name: string): TypeNode => ({ name });
const builtin = (name: string): TypeNode => ({ builtin: name });
const pointer = (type: TypeNode): TypeNode => ({ pointer: type });
const constChar = (): TypeNode => pointer({ builtin: "char", const: true });
const fnPointer = (params: Parameter[] = [], result: TypeNode = builtin("void")): TypeNode =>
  ({ pointer: { function: { result, params, variadic: false } } }) as unknown as TypeNode;
const note = (key: string, value?: string): Annotation => (value === undefined ? { key } : { key, value });
const param = (name: string, type: TypeNode, annotations: Annotation[] = []): Parameter => ({ name, type, annotations });
const field = (name: string, type: TypeNode, annotations: Annotation[] = []): Field => ({ name, type, annotations, doc: null });

/** The same facts on every target. */
const facts = (value: TargetFacts): Record<string, TargetFacts> =>
  Object.fromEntries(TARGETS.map((t) => [t, value]));

const decl = (kind: Declaration["kind"], name: string, extra: Partial<Declaration> = {}): Declaration => ({
  kind,
  name,
  location,
  doc: null,
  annotations: [],
  ...extra,
});

const fn = (name: string, params: Parameter[] = [], extra: Partial<Declaration> = {}): Declaration =>
  decl("function", name, {
    result: { type: named("alpha_status") },
    params,
    variadic: false,
    perTarget: facts({ callingConvention: "c" }),
    ...extra,
  });

/** A struct whose fields are all pointer-sized, with matching layout facts. */
const struct = (name: string, fields: Field[], extra: Partial<Declaration> = {}): Declaration =>
  decl("struct", name, {
    fields,
    perTarget: {
      [X64]: { size: 8 * fields.length, align: 8, offsets: fields.map((_, i) => 8 * i), fieldCallingConventions: fields.map((f) => (f.type.pointer !== undefined && (f.type.pointer as TypeNode).function !== undefined ? "c" : null)) },
      [X86]: { size: 4 * fields.length, align: 4, offsets: fields.map((_, i) => 4 * i), fieldCallingConventions: fields.map((f) => (f.type.pointer !== undefined && (f.type.pointer as TypeNode).function !== undefined ? "c" : null)) },
    },
    ...extra,
  });

const enumDecl = (name: string, enumerators: [string, number][], extra: Partial<Declaration> = {}): Declaration =>
  decl("enum", name, {
    type: named("int32_t"),
    enumerators: enumerators.map(([n, value]) => ({ name: n, value, doc: null })),
    perTarget: facts({ canonicalType: builtin("int") }),
    ...extra,
  });

const macro = (name: string, value: string | undefined, replacement: string[] = [value ?? "x"]): Declaration =>
  decl("macro", name, {
    functionLike: false,
    replacement,
    ...(value === undefined ? {} : { perTarget: facts({ value }) }),
  });

function library(name: string, declarations: Declaration[], extra: Partial<ModelLibrary> = {}): ModelLibrary {
  return {
    name,
    prefix: name,
    macroPrefix: name.toUpperCase(),
    status: `${name}_status`,
    headers: [`include/${name}.h`],
    includeDirs: ["include"],
    defines: [],
    declarations,
    ...extra,
  };
}

function model(...libraries: ModelLibrary[]): Model {
  return { schema: "stela.model/1", producer: { stela: "0.1.0", libclang: "test" }, targets: [...TARGETS], libraries };
}

/**
 * Diffs two declaration lists of library "alpha". Each side gets an
 * ALPHA_ABI_VERSION, 1 before and 2 after unless `bump` is false; findings on
 * that macro are dropped.
 */
function changes(before: Declaration[], after: Declaration[], options: { bump?: boolean; experimentalHeaders?: string[] } = {}): DiffFinding[] {
  const version = (v: string) => macro("ALPHA_ABI_VERSION", v);
  const findings = diffModels(
    model(library("alpha", [version("1"), ...before])),
    model(library("alpha", [version(options.bump === false ? "1" : "2"), ...after])),
    { experimentalHeaders: options.experimentalHeaders },
  );
  return findings.filter((f) => f.declaration !== "macro ALPHA_ABI_VERSION");
}

/** `class change` per finding, for compact assertions. */
const kinds = (findings: readonly DiffFinding[]) => findings.map((f) => `${f.class} ${f.change}`);

// --- Declarations -----------------------------------------------------------

test("identical models have no changes", () => {
  const decls = [fn("alpha_run", [param("value", builtin("int"))]), struct("alpha_point", [field("x", builtin("int"))])];
  assert.deepEqual(changes(decls, decls, { bump: false }), []);
});

test("a removed or renamed declaration is a break", () => {
  const findings = changes([fn("alpha_run"), fn("alpha_stop")], [fn("alpha_start"), fn("alpha_stop")]);
  assert.deepEqual(kinds(findings), ["addition added", "break removed"]);
  const removed = findings.find((f) => f.change === "removed")!;
  assert.equal(removed.declaration, "function alpha_run");
  assert.match(removed.message, /^function alpha_run: was removed$/);
});

test("a declaration or enumerator added is an addition", () => {
  assert.deepEqual(kinds(changes([], [fn("alpha_run")])), ["addition added"]);
  assert.deepEqual(
    kinds(changes([enumDecl("alpha_mode", [["alpha_mode_a", 0]])], [enumDecl("alpha_mode", [["alpha_mode_a", 0], ["alpha_mode_b", 1]])])),
    ["addition enumerator_added"],
  );
});

test("a function's signature change is a break", () => {
  const before = fn("alpha_run", [param("value", builtin("int"))]);
  for (const after of [
    fn("alpha_run", [param("value", builtin("long"))]),
    fn("alpha_run", [param("value", builtin("int")), param("extra", builtin("int"))]),
    fn("alpha_run", [param("value", builtin("int"))], { result: { type: builtin("void") } }),
    fn("alpha_run", [param("value", builtin("int"))], { variadic: true }),
  ]) {
    const findings = changes([before], [after]);
    assert.deepEqual(kinds(findings), ["break signature_changed"]);
  }
  assert.match(
    changes([before], [fn("alpha_run", [param("value", builtin("long"))])])[0].message,
    /from alpha_status\(int\) to alpha_status\(long\)/,
  );
});

test("a calling convention change is a break, naming the targets", () => {
  const after = fn("alpha_run", [], { perTarget: { [X64]: { callingConvention: "c" }, [X86]: { callingConvention: "stdcall" } } });
  const findings = changes([fn("alpha_run")], [after]);
  assert.deepEqual(kinds(findings), ["break calling_convention_changed"]);
  assert.match(findings[0].message, /calling convention on i686-pc-windows-msvc changed from c to stdcall/);
});

test("a parameter rename is a note, and references follow positions", () => {
  const before = fn("alpha_read", [param("buffer", pointer(builtin("uint8_t")), [note("count", "n")]), param("n", named("size_t"))]);
  const after = fn("alpha_read", [param("buffer", pointer(builtin("uint8_t")), [note("count", "byte_count")]), param("byte_count", named("size_t"))]);
  const findings = changes([before], [after]);
  assert.deepEqual(kinds(findings), ["note parameter_renamed"]);
  assert.match(findings[0].message, /parameter 2 renamed from "n" to "byte_count"/);
});

test("struct size, alignment, and offset changes are breaks", () => {
  const before = struct("alpha_point", [field("x", builtin("int")), field("y", builtin("int"))]);
  const resized = structuredClone(before);
  resized.perTarget![X86].size = 12;
  assert.deepEqual(kinds(changes([before], [resized])), ["break layout_changed"]);
  assert.match(changes([before], [resized])[0].message, /size on i686-pc-windows-msvc changed from 8 to 12/);
  const realigned = structuredClone(before);
  realigned.perTarget![X64].align = 16;
  assert.deepEqual(kinds(changes([before], [realigned])), ["break layout_changed"]);
  const moved = structuredClone(before);
  moved.perTarget![X64].offsets = [0, 16];
  const findings = changes([before], [moved]);
  assert.deepEqual(kinds(findings), ["break layout_changed"]);
  assert.match(findings[0].message, /offset of "y" on x86_64-pc-windows-msvc changed from 8 to 16$/);
});

test("a field appended to a struct_size struct or a function table is an addition", () => {
  const sized = [field("struct_size", named("uint32_t")), field("flags", named("uint32_t"))];
  assert.deepEqual(
    kinds(changes([struct("alpha_options", sized)], [struct("alpha_options", [...sized, field("extra", named("uint32_t"))])])),
    ["addition field_added"],
  );
  const slots = [field("run", fnPointer()), field("stop", fnPointer())];
  assert.deepEqual(
    kinds(changes([struct("alpha_api", slots)], [struct("alpha_api", [...slots, field("pause", fnPointer())])])),
    ["addition field_added"],
  );
});

/** Typedef layout facts omit the record's field-specific facts, as extraction does. */
function recordTypedef(record: Declaration, name = record.name): Declaration {
  return decl("typedef", name, {
    type: named(record.name),
    perTarget: Object.fromEntries(Object.entries(record.perTarget ?? {}).map(([target, f]) =>
      [target, { size: f.size, align: f.align, canonicalType: named(record.name) }],
    )),
  });
}

test("a field appended to a typedef'd struct_size struct or function table is an addition", () => {
  for (const fields of [
    [field("struct_size", named("uint32_t")), field("flags", named("uint32_t"))],
    [field("run", fnPointer()), field("stop", fnPointer())],
    [field("struct_size", named("uint32_t")), field("run", fnPointer())],
  ]) {
    const before = struct("alpha_api", fields);
    const after = struct("alpha_api", [...fields, field("extra", fnPointer())]);
    const alias = (record: Declaration) => recordTypedef(record, "alpha_alias");
    const chain = (record: Declaration) => ({ ...recordTypedef(record, "alpha_chain"), type: named("alpha_alias") });
    const declarations = (record: Declaration) => [record, recordTypedef(record), alias(record), chain(record)];
    assert.deepEqual(
      kinds(changes(declarations(before), declarations(after))),
      ["addition field_added"],
    );
  }
});

test("a typedef of a non-growable struct still breaks when it grows", () => {
  const before = struct("alpha_point", [field("x", builtin("int"))]);
  const after = struct("alpha_point", [...before.fields!, field("y", builtin("int"))]);
  const findings = changes([before, recordTypedef(before)], [after, recordTypedef(after)]);
  assert.ok(findings.some((f) => f.declaration === "struct alpha_point" && f.class === "break" && f.change === "field_added"));
  assert.ok(findings.some((f) => f.declaration === "typedef alpha_point" && f.class === "break" && f.change === "layout_changed"));
});

test("typedef record shrink, non-end changes, and unexplained growth remain breaks", () => {
  const fields = [field("run", fnPointer()), field("stop", fnPointer())];
  const before = struct("alpha_api", fields);
  const resized = structuredClone(before);
  resized.perTarget![X64].size! += 8;
  for (const after of [
    struct("alpha_api", fields.slice(0, 1)),
    struct("alpha_api", [fields[1], fields[0]]),
    struct("alpha_api", [fields[0], field("pause", fnPointer()), fields[1]]),
    resized,
  ]) {
    const findings = changes([before, recordTypedef(before)], [after, recordTypedef(after)]);
    assert.ok(findings.some((f) => f.class === "break"));
    if (after.perTarget![X64].size !== before.perTarget![X64].size) {
      assert.ok(findings.some((f) => f.declaration === "typedef alpha_api" && f.class === "break" && f.change === "layout_changed"));
    }
  }
});

test("a field appended to any other struct is a break", () => {
  const fields = [field("x", builtin("int"))];
  const findings = changes([struct("alpha_point", fields)], [struct("alpha_point", [...fields, field("y", builtin("int"))])]);
  assert.deepEqual(kinds(findings), ["break field_added"]);
  assert.match(findings[0].message, /only a struct that starts with struct_size, or a function table, can grow/);
});

test("function-table entries reordered, removed, or inserted are breaks", () => {
  const run = field("run", fnPointer());
  const stop = field("stop", fnPointer());
  const pause = field("pause", fnPointer());
  const table = (...fields: Field[]) => struct("alpha_api", fields);
  const reordered = changes([table(run, stop)], [table(stop, run)]);
  assert.deepEqual(kinds(reordered), ["break field_changed", "break field_changed"]);
  assert.match(reordered[0].message, /table entries can only be appended/);
  assert.deepEqual(kinds(changes([table(run, stop)], [table(run)])), ["break field_removed"]);
  assert.ok(kinds(changes([table(run, stop)], [table(run, pause, stop)])).includes("break field_changed"));
});

test("a field's type change is a break, and its layout isn't reported again", () => {
  const before = struct("alpha_point", [field("x", builtin("int"))]);
  const after = struct("alpha_point", [field("x", builtin("long"))], { perTarget: { [X64]: { size: 16, align: 8, offsets: [0] }, [X86]: { size: 4, align: 4, offsets: [0] } } });
  assert.deepEqual(kinds(changes([before], [after])), ["break field_changed"]);
});

test("a table slot's calling convention change is a break", () => {
  const before = struct("alpha_api", [field("run", fnPointer())]);
  const after = structuredClone(before);
  after.perTarget![X86].fieldCallingConventions = ["stdcall"];
  assert.deepEqual(kinds(changes([before], [after])), ["break calling_convention_changed"]);
});

test("an opaque record becoming defined is an addition; the reverse is a break", () => {
  const opaque = decl("struct", "alpha_runtime_s", { incomplete: true });
  const defined = struct("alpha_runtime_s", [field("x", builtin("int"))]);
  assert.deepEqual(kinds(changes([opaque], [defined])), ["addition definition_changed"]);
  assert.deepEqual(kinds(changes([defined], [opaque])), ["break definition_changed"]);
});

test("an enumerator removed or renumbered is a break", () => {
  const before = enumDecl("alpha_mode", [["alpha_mode_a", 0], ["alpha_mode_b", 1]]);
  assert.deepEqual(kinds(changes([before], [enumDecl("alpha_mode", [["alpha_mode_a", 0]])])), ["break enumerator_removed"]);
  const findings = changes([before], [enumDecl("alpha_mode", [["alpha_mode_a", 0], ["alpha_mode_b", 2]])]);
  assert.deepEqual(kinds(findings), ["break enumerator_value_changed"]);
  assert.match(findings[0].message, /alpha_mode_b changed from 1 to 2/);
});

test("an enum's underlying type change is a break", () => {
  const before = enumDecl("alpha_mode", [["alpha_mode_a", 0]]);
  assert.deepEqual(kinds(changes([before], [enumDecl("alpha_mode", [["alpha_mode_a", 0]], { type: named("uint8_t") })])), ["break type_changed"]);
  const canonical = enumDecl("alpha_mode", [["alpha_mode_a", 0]], { perTarget: facts({ canonicalType: builtin("unsigned int") }) });
  assert.deepEqual(kinds(changes([before], [canonical])), ["break type_changed"]);
});

test("a typedef's underlying type change is a break", () => {
  const typedef = (type: TypeNode, size: number) => decl("typedef", "alpha_id", { type, perTarget: facts({ canonicalType: type, size, align: size }) });
  const findings = changes([typedef(named("uint32_t"), 4)], [typedef(named("uint64_t"), 8)]);
  assert.deepEqual(kinds(findings), ["break type_changed"]);
  assert.match(findings[0].message, /from uint32_t to uint64_t/);
  const before = decl("typedef", "alpha_id", { type: named("alpha_int"), perTarget: facts({ canonicalType: builtin("int"), size: 4, align: 4 }) });
  const after = decl("typedef", "alpha_id", { type: named("alpha_int"), perTarget: facts({ canonicalType: builtin("long long"), size: 8, align: 8 }) });
  assert.deepEqual(kinds(changes([before], [after])), ["break layout_changed", "break layout_changed", "break type_changed"]);
});

test("a macro value change is a break; an *_ABI_VERSION increase is a note", () => {
  const findings = changes([macro("ALPHA_LIMIT", "4")], [macro("ALPHA_LIMIT", "8")]);
  assert.deepEqual(kinds(findings), ["break macro_value_changed"]);
  assert.match(findings[0].message, /value changed from 4 to 8/);
  const all = diffModels(model(library("alpha", [macro("ALPHA_ABI_VERSION", "1")])), model(library("alpha", [macro("ALPHA_ABI_VERSION", "2")])));
  assert.deepEqual(kinds(all), ["note abi_version_increased"]);
  const lowered = diffModels(model(library("alpha", [macro("ALPHA_ABI_VERSION", "2")])), model(library("alpha", [macro("ALPHA_ABI_VERSION", "1")])));
  assert.deepEqual(kinds(lowered), ["break macro_value_changed"]);
});

test("a non-integer macro's replacement change is a note", () => {
  const before = macro("ALPHA_OPTIONS_INIT", undefined, ["{", "0", "}"]);
  const after = macro("ALPHA_OPTIONS_INIT", undefined, ["{", "0", ",", "0", "}"]);
  assert.deepEqual(kinds(changes([before], [after])), ["note macro_changed"]);
  const functionLike = { ...after, functionLike: true };
  assert.deepEqual(kinds(changes([before], [functionLike])), ["break definition_changed"]);
});

// --- Annotations ------------------------------------------------------------

test("nullable on an input: added is an addition, removed is a break", () => {
  const plain = fn("alpha_set", [param("name", constChar(), [note("encoding", "utf8")])]);
  const nullable = fn("alpha_set", [param("name", constChar(), [note("encoding", "utf8"), note("nullable")])]);
  assert.deepEqual(kinds(changes([plain], [nullable])), ["addition nullable_changed"]);
  assert.deepEqual(kinds(changes([nullable], [plain])), ["break nullable_changed"]);
});

test("nullable on an output or result: added is a break, removed is a note", () => {
  const out = (annotations: Annotation[]) => fn("alpha_get", [param("out_value", pointer(named("alpha_value")), annotations)]);
  assert.deepEqual(kinds(changes([out([])], [out([note("nullable")])])), ["break nullable_changed"]);
  assert.deepEqual(kinds(changes([out([note("nullable")])], [out([])])), ["note nullable_changed"]);
  const result = (annotations: Annotation[]) => fn("alpha_name", [], { result: { type: constChar() }, annotations });
  assert.deepEqual(kinds(changes([result([])], [result([note("nullable")])])), ["break nullable_changed"]);
  assert.deepEqual(kinds(changes([result([note("nullable")])], [result([])])), ["note nullable_changed"]);
});

test("nullable on a callback parameter or struct field changes is a break either way", () => {
  const callback = (annotations: Annotation[]) =>
    decl("typedef", "alpha_event_cb", { type: fnPointer([param("data", pointer(builtin("void")), annotations)]) });
  assert.deepEqual(kinds(changes([callback([])], [callback([note("nullable")])])), ["break nullable_changed"]);
  assert.deepEqual(kinds(changes([callback([note("nullable")])], [callback([])])), ["break nullable_changed"]);
  const options = (annotations: Annotation[]) =>
    struct("alpha_options", [field("struct_size", named("uint32_t")), field("name", constChar(), [note("encoding", "utf8"), ...annotations])]);
  assert.deepEqual(kinds(changes([options([])], [options([note("nullable")])])), ["break nullable_changed"]);
});

test("transfer, scope, count, closure, and destroy changes are breaks", () => {
  const cb = named("alpha_event_cb");
  const callback = decl("typedef", "alpha_event_cb", { type: fnPointer([param("user_data", pointer(builtin("void")))]) });
  const site = (cbNotes: Annotation[], dataNotes: Annotation[] = [], countNotes: Annotation[] = []) =>
    fn("alpha_listen", [
      param("on_event", cb, cbNotes),
      param("user_data", pointer(builtin("void")), dataNotes),
      param("release", named("alpha_release_cb")),
      param("items", pointer(named("alpha_item")), countNotes),
      param("n", named("size_t")),
    ]);
  const before = site([note("scope", "object"), note("destroy", "release")], [], [note("count", "n")]);
  for (const after of [
    site([note("scope", "async"), note("destroy", "release")], [], [note("count", "n")]),
    site([note("scope", "object")], [], [note("count", "n")]),
    site([note("scope", "object"), note("destroy", "release")], [], []),
    site([note("scope", "object"), note("destroy", "release"), note("closure", "release")], [], [note("count", "n")]),
    site([note("scope", "object"), note("destroy", "release")], [note("transfer", "full")], [note("count", "n")]),
  ]) {
    assert.deepEqual(kinds(changes([callback, before], [callback, after])), ["break annotation_changed"]);
  }
});

test("annotations a name or position already implies are no change", () => {
  const cb = decl("typedef", "alpha_event_cb", { type: fnPointer([param("user_data", pointer(builtin("void")))]) });
  const site = (outNotes: Annotation[], cbNotes: Annotation[], countNotes: Annotation[], textNotes: Annotation[]) =>
    fn("alpha_create_utf8", [
      param("items", pointer(named("alpha_item")), countNotes),
      param("length", named("size_t")),
      param("on_event", named("alpha_event_cb"), cbNotes),
      param("user_data", pointer(builtin("void"))),
      param("text", constChar(), textNotes),
      param("out_value", pointer(named("alpha_value")), outNotes),
    ]);
  const implicit = site([], [], [], []);
  const explicit = site([note("out"), note("transfer", "none")], [note("scope", "call"), note("closure", "user_data")], [note("count", "length")], [note("encoding", "utf8")]);
  assert.deepEqual(changes([cb, implicit], [cb, explicit], { bump: false }), []);
  const create = (outNotes: Annotation[]) => fn("alpha_value_create", [param("out_value", pointer(named("alpha_value")), outNotes)]);
  assert.deepEqual(changes([create([])], [create([note("transfer", "full")])], { bump: false }), []);
  assert.deepEqual(kinds(changes([create([])], [create([note("transfer", "none")])])), ["break annotation_changed"]);
});

test("optional added to an output is an addition; any other optional change is a break", () => {
  const out = (annotations: Annotation[]) => fn("alpha_get", [param("out_value", pointer(named("alpha_value")), annotations)]);
  assert.deepEqual(kinds(changes([out([])], [out([note("optional")])])), ["addition optional_changed"]);
  assert.deepEqual(kinds(changes([out([note("optional")])], [out([])])), ["break optional_changed"]);
});

test("doc, deprecated, and metadata changes are notes", () => {
  assert.deepEqual(kinds(changes([fn("alpha_run")], [fn("alpha_run", [], { doc: "Runs." })])), ["note doc_changed"]);
  assert.deepEqual(kinds(changes([fn("alpha_run")], [fn("alpha_run", [], { annotations: [note("deprecated", "use alpha_start")] })])), ["note deprecated_changed"]);
  assert.deepEqual(kinds(changes([fn("alpha_run")], [fn("alpha_run", [], { annotations: [note("since", "2")] })])), ["note metadata_changed"]);
  const documented = enumDecl("alpha_mode", [["alpha_mode_a", 0]]);
  const redocumented = structuredClone(documented);
  redocumented.enumerators![0].doc = "The first mode.";
  assert.deepEqual(kinds(changes([documented], [redocumented])), ["note doc_changed"]);
});

// --- Versions, experimental code, targets, libraries -------------------------

test("an addition without an *_ABI_VERSION increase is a break", () => {
  const findings = changes([], [fn("alpha_run")], { bump: false });
  assert.deepEqual(kinds(findings), ["addition added", "break version_not_bumped"]);
  assert.match(findings[1].message, /library alpha: 1 addition\(s\), but ALPHA_ABI_VERSION didn't increase/);
  assert.deepEqual(kinds(changes([fn("alpha_run")], [fn("alpha_run", [], { doc: "x" })], { bump: false })), ["note doc_changed"]);
});

test("an extension library uses the version of the library that owns its status type", () => {
  const status = enumDecl("alpha_status", [["alpha_status_ok", 0]]);
  const ext = (decls: Declaration[]) => library("beta", decls, { status: "alpha_status" });
  const before = model(library("alpha", [macro("ALPHA_ABI_VERSION", "1"), status]), ext([]));
  const unbumped = model(library("alpha", [macro("ALPHA_ABI_VERSION", "1"), status]), ext([fn("beta_run")]));
  assert.deepEqual(kinds(diffModels(before, unbumped)), ["addition added", "break version_not_bumped"]);
  const bumped = model(library("alpha", [macro("ALPHA_ABI_VERSION", "2"), status]), ext([fn("beta_run")]));
  assert.deepEqual(kinds(diffModels(before, bumped)), ["note abi_version_increased", "addition added"]);
});

test("breaks in experimental declarations or headers are notes", () => {
  const experimental = fn("alpha_try", [], { annotations: [note("experimental")] });
  const findings = changes([experimental], [], { bump: false });
  assert.deepEqual(kinds(findings), ["note removed"]);
  assert.equal(findings[0].experimental, true);
  assert.match(findings[0].message, /\(experimental\)$/);
  assert.deepEqual(kinds(changes([fn("alpha_try")], [], { experimentalHeaders: ["include/alpha.h"] })), ["note removed"]);
  // Experimental additions need no version bump.
  assert.deepEqual(kinds(changes([], [experimental], { bump: false })), ["addition added"]);
});

test("marking a stable declaration experimental doesn't excuse breaking it", () => {
  const stable = fn("alpha_run", [param("value", builtin("int"))]);
  const marked = fn("alpha_run", [param("value", builtin("long"))], { annotations: [note("experimental")] });
  assert.deepEqual(kinds(changes([stable], [marked])), ["note metadata_changed", "break signature_changed"]);
  const moved = fn("alpha_run", [param("value", builtin("long"))], { location: { ...location, file: "include/alpha_exp.h" } });
  assert.deepEqual(kinds(changes([stable], [moved], { experimentalHeaders: ["include/alpha_exp.h"] })), ["break signature_changed"]);
});

test("a declaration or library losing a target is a break; gaining one is an addition", () => {
  const findings = changes([fn("alpha_run")], [fn("alpha_run", [], { targets: [X64] })]);
  assert.deepEqual(kinds(findings), ["break removed"]);
  assert.match(findings[0].message, /was removed on i686-pc-windows-msvc/);
  assert.deepEqual(kinds(changes([fn("alpha_run", [], { targets: [X64] })], [fn("alpha_run")])), ["addition added"]);
  const narrowed = diffModels(model(library("alpha", [])), model(library("alpha", [], { targets: [X64] })));
  assert.deepEqual(kinds(narrowed), ["break target_removed"]);
});

test("a library removed is a break; a library added is an addition", () => {
  const two = model(library("alpha", []), library("beta", []));
  assert.deepEqual(kinds(diffModels(two, model(library("alpha", [])))), ["break library_removed"]);
  assert.deepEqual(kinds(diffModels(model(library("alpha", [])), two)), ["addition library_added"]);
  assert.deepEqual(diffModels(two, model(library("alpha", [])), { libraries: ["alpha"] }), []);
});

test("findings are formatted as text and JSON", () => {
  const findings = changes([fn("alpha_run"), macro("ALPHA_LIMIT", "1")], [macro("ALPHA_LIMIT", "1", ["1", "/*", "*/"])], { bump: false });
  assert.equal(formatDiff(findings, "text"), "include/alpha.h:10:1: break[removed]: function alpha_run: was removed");
  assert.equal(summarizeDiff(findings), "1 change(s): 1 break(s), 0 addition(s), 0 note(s)");
  assert.deepEqual(JSON.parse(formatDiff(findings, "json")), findings);
});

// --- Config and CLI ---------------------------------------------------------

const libraryConfig = { name: "alpha", prefix: "alpha", macroPrefix: "ALPHA", headers: ["include/alpha.h"] };

test("the config parses diff baselines and experimental headers", () => {
  const root = path.resolve("/repo");
  const config = parseConfig(
    { schema: "stela.config/1", libraries: [libraryConfig], diff: { baselines: { alpha: "api/alpha.model.json" }, experimental: ["include/alpha.h"] } },
    path.join(root, "stela.config.json"),
  );
  assert.deepEqual(config.diff, { baselines: { alpha: path.join(root, "api", "alpha.model.json") }, experimental: [path.join(root, "include", "alpha.h")] });
  const bad = (diff: unknown) => () => parseConfig({ schema: "stela.config/1", libraries: [libraryConfig], diff }, path.join(root, "stela.config.json"));
  assert.throws(bad({ baselines: { beta: "api/beta.model.json" } }), (e) => e instanceof StelaError && /unknown library "beta"/.test(e.message));
  assert.throws(bad({ baselines: { alpha: "api/alpha.txt" } }), (e) => e instanceof StelaError && /must be a \.json path/.test(e.message));
  assert.throws(bad({ baseline: {} }), (e) => e instanceof StelaError && /unknown key "baseline"/.test(e.message));
});

function writeJson(file: string, value: unknown): void {
  mkdirSync(path.dirname(file), { recursive: true });
  writeFileSync(file, typeof value === "string" ? value : JSON.stringify(value));
}

test("CLI: diff of two model files needs no config and exits 1 on a break", (t) => {
  const dir = tempDir(t, "diff-files");
  writeJson(path.join(dir, "base.json"), model(library("alpha", [fn("alpha_run")])));
  writeJson(path.join(dir, "head.json"), model(library("alpha", [])));
  const r = runCli(dir, "diff", "--base", "base.json", "--head", "head.json");
  assert.equal(r.code, 1, r.err);
  assert.match(r.out, /break\[removed\]: function alpha_run: was removed\n1 change\(s\): 1 break\(s\)/);
  const json = runCli(dir, "diff", "--base", "head.json", "--head", "head.json", "--format", "json");
  assert.equal(json.code, 0, json.err);
  assert.deepEqual(JSON.parse(json.out), []);
});

test("CLI: baseline writes the configured baselines, and diff compares against them", (t) => {
  const dir = tempDir(t, "diff-config");
  const configJson = { schema: "stela.config/1", targets: TARGETS, libraries: [libraryConfig, { ...libraryConfig, name: "beta", prefix: "beta", macroPrefix: "BETA", headers: ["include/beta.h"] }], diff: { baselines: { alpha: "api/alpha.model.json" } } };
  writeJson(path.join(dir, "stela.config.json"), configJson);
  const missing = runCli(dir, "diff");
  assert.equal(missing.code, 2);
  assert.match(missing.err, /run `stela baseline alpha` to create it/);

  const modelFile = path.join(dir, "out", "stela", "stela.model.json");
  writeJson(modelFile, serializeModel(model(library("alpha", [macro("ALPHA_ABI_VERSION", "1"), fn("alpha_run")]), library("beta", [fn("beta_run")]))));
  const written = runCli(dir, "baseline");
  assert.equal(written.code, 0, written.err);
  assert.match(written.out, /wrote: {5}api\/alpha\.model\.json/);
  const stored = JSON.parse(readFileSync(path.join(dir, "api", "alpha.model.json"), "utf8")) as Model;
  assert.deepEqual(stored.libraries.map((l) => l.name), ["alpha"]);
  assert.match(runCli(dir, "baseline").out, /unchanged: api\/alpha\.model\.json/);
  assert.match(runCli(dir, "baseline", "beta").err, /library "beta" has no entry in "diff.baselines"/);

  // --out writes any library to <dir>/<library>.model.json.
  const shipped = runCli(dir, "baseline", "--out", "ship", "beta");
  assert.equal(shipped.code, 0, shipped.err);
  assert.match(shipped.out, /wrote: {5}ship\/beta\.model\.json/);
  assert.deepEqual((JSON.parse(readFileSync(path.join(dir, "ship", "beta.model.json"), "utf8")) as Model).libraries.map((l) => l.name), ["beta"]);
  assert.match(runCli(dir, "baseline", "--out", "ship").out, /wrote: {5}ship\/alpha\.model\.json\nunchanged: ship\/beta\.model\.json/);
  assert.match(runCli(dir, "baseline", "--out", "ship", "gamma").err, /unknown library "gamma"/);

  // Libraries without a baseline aren't compared.
  writeJson(modelFile, serializeModel(model(library("alpha", [macro("ALPHA_ABI_VERSION", "1"), fn("alpha_run")]), library("beta", []))));
  const clean = runCli(dir, "diff");
  assert.equal(clean.code, 0, clean.err);
  assert.match(clean.out, /^0 change\(s\)/);

  writeJson(modelFile, serializeModel(model(library("alpha", [macro("ALPHA_ABI_VERSION", "1"), fn("alpha_run"), fn("alpha_stop")]))));
  const unbumped = runCli(dir, "diff");
  assert.equal(unbumped.code, 1);
  assert.match(unbumped.out, /addition\[added\]: function alpha_stop: was added/);
  assert.match(unbumped.out, /break\[version_not_bumped\]/);
});
