// Stela lint (design §9): deterministic checks over stela.model/1 plus the
// configured source files needed by stela_h_current and reserved_macro.

import {
  existsSync,
  readFileSync,
  readdirSync,
  statSync,
} from "node:fs";
import path from "node:path";
import type {
  LibraryConfig,
  LintSeverity,
  StelaConfig,
} from "./config.ts";
import { StelaError } from "./errors.ts";
import type {
  Annotation,
  Declaration,
  Field,
  Model,
  ModelLibrary,
  Parameter,
  TypeNode,
} from "./model.ts";
import {
  generateLibraryHeader,
  generatedNames,
  includeGuard,
  normalizeText,
  parseTemplate,
} from "./vendor.ts";

export const LINT_RULES = [
  "prefix",
  "naming_case",
  "enumerator_name",
  "function_name",
  "table_name",
  "table_field",
  "reserved_name",
  "typedef_name",
  "handle_opaque",
  "portable_types",
  "bool_type",
  "enum_pinned",
  "macro_constants",
  "flags_range",
  "text_params",
  "struct_strings",
  "status_shape",
  "struct_size_first",
  "cdecl",
  "callback_shape",
  "param_names",
  "annotation_known",
  "annotation_redundant",
  "stela_h_current",
  "reserved_macro",
] as const;

export type LintRule = (typeof LINT_RULES)[number];
export type FindingSeverity = Exclude<LintSeverity, "off">;

export interface LintFinding {
  readonly rule: LintRule;
  readonly severity: FindingSeverity;
  readonly library: string;
  readonly file: string;
  readonly line: number;
  readonly column: number;
  readonly declaration: string;
  readonly message: string;
}

const DEFAULT_SEVERITIES: Readonly<Record<LintRule, FindingSeverity>> = Object.fromEntries(
  LINT_RULES.map((rule) => [
    rule,
    rule === "bool_type" || rule === "macro_constants"
      ? "warning"
      : rule === "annotation_redundant"
        ? "info"
        : "error",
  ]),
) as Record<LintRule, FindingSeverity>;

const LOWER_SNAKE_RE = /^[a-z][a-z0-9]*(?:_[a-z0-9]+)*$/;
const UPPER_SNAKE_RE = /^[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)*$/;
const WINDOWS_MACRO_NAMES = new Set([
  "IN",
  "OUT",
  "OPTIONAL",
  "near",
  "far",
  "min",
  "max",
  "small",
  "interface",
]);
const INTEGER_NAMES = new Set([
  "char",
  "signed char",
  "unsigned char",
  "short",
  "unsigned short",
  "int",
  "unsigned int",
  "long",
  "unsigned long",
  "long long",
  "unsigned long long",
  "int8_t",
  "uint8_t",
  "int16_t",
  "uint16_t",
  "int32_t",
  "uint32_t",
  "int64_t",
  "uint64_t",
  "size_t",
]);
const PORTABILITY_NAMES = new Set([
  "long",
  "unsigned long",
  "long double",
  "wchar_t",
  "char16_t",
  "char8_t",
]);
const KNOWN_ANNOTATIONS = new Set([
  "out",
  "inout",
  "optional",
  "nullable",
  "transfer",
  "count",
  "zero_terminated",
  "encoding",
  "scope",
  "closure",
  "destroy",
  "since",
  "deprecated",
  "experimental",
  "allow",
  "enum",
  "flags",
]);
const REFERENCE_ANNOTATIONS = new Set(["count", "closure", "destroy"]);
const NO_VALUE_ANNOTATIONS = new Set([
  "out",
  "inout",
  "optional",
  "nullable",
  "zero_terminated",
  "deprecated",
  "experimental",
]);
const DEFAULT_TEMPLATE = path.join(import.meta.dirname, "..", "include", "stela.h");

interface RuleContext {
  readonly model: Model;
  readonly library: ModelLibrary;
  readonly config?: LibraryConfig;
  readonly severities: Readonly<Record<LintRule, LintSeverity>>;
  readonly macroConstants: ReadonlySet<string>;
  readonly findings: LintFinding[];
  readonly typedefs: ReadonlyMap<string, Declaration>;
  readonly records: ReadonlyMap<string, Declaration>;
  readonly handles: ReadonlyMap<string, string>;
  readonly functionTables: ReadonlySet<string>;
}

function annotationsOf(declaration: Declaration): readonly Annotation[] {
  return declaration.annotations ?? [];
}

function isSuppressed(declaration: Declaration, rule: LintRule): boolean {
  return annotationsOf(declaration).some(
    (annotation) => annotation.key === "allow" && annotation.value === rule,
  );
}

function addFinding(
  context: RuleContext,
  rule: LintRule,
  declaration: Declaration,
  message: string,
): void {
  const severity = context.severities[rule];
  if (severity === "off" || isSuppressed(declaration, rule)) return;
  context.findings.push({
    rule,
    severity,
    library: context.library.name,
    file: declaration.location.file,
    line: declaration.location.line,
    column: declaration.location.column,
    declaration: declaration.name,
    message,
  });
}

function addFileFinding(
  context: RuleContext,
  rule: LintRule,
  file: string,
  line: number,
  column: number,
  declaration: string,
  message: string,
): void {
  const severity = context.severities[rule];
  if (severity === "off") return;
  context.findings.push({
    rule,
    severity,
    library: context.library.name,
    file,
    line,
    column,
    declaration,
    message,
  });
}

function property<T>(value: TypeNode, key: string): T | undefined {
  return value[key] as T | undefined;
}

function namedType(type: TypeNode): string | undefined {
  return property<string>(type, "name");
}

function pointedType(type: TypeNode): TypeNode | undefined {
  return property<TypeNode>(type, "pointer");
}

interface FunctionType {
  readonly result: TypeNode;
  readonly params: Parameter[];
  readonly variadic: boolean;
}

function directFunctionType(type: TypeNode): FunctionType | undefined {
  const pointed = pointedType(type);
  return pointed === undefined ? undefined : property<FunctionType>(pointed, "function");
}

function functionTypeOfTypedef(declaration: Declaration): FunctionType | undefined {
  if (declaration.type !== undefined) {
    const direct = directFunctionType(declaration.type);
    if (direct !== undefined) return direct;
  }
  for (const facts of Object.values(declaration.perTarget ?? {})) {
    if (facts.canonicalType !== undefined) {
      const canonical = directFunctionType(facts.canonicalType);
      if (canonical !== undefined) return canonical;
    }
  }
  return undefined;
}

function walkType(type: TypeNode, visit: (node: TypeNode) => void): void {
  visit(type);
  const child =
    property<TypeNode>(type, "pointer") ??
    property<TypeNode>(type, "array");
  if (child !== undefined) walkType(child, visit);
  const fn = property<FunctionType>(type, "function");
  if (fn !== undefined) {
    walkType(fn.result, visit);
    for (const parameter of fn.params) walkType(parameter.type, visit);
  }
  for (const recordKind of ["struct", "union"] as const) {
    const record = property<{ fields: Field[] }>(type, recordKind);
    if (record !== undefined) {
      for (const field of record.fields) walkType(field.type, visit);
    }
  }
  const inlineEnum = property<{ type: TypeNode }>(type, "enum");
  if (inlineEnum !== undefined) walkType(inlineEnum.type, visit);
}

function allDeclarationTypes(declaration: Declaration): TypeNode[] {
  const types: TypeNode[] = [];
  if (declaration.result !== undefined) types.push(declaration.result.type);
  if (declaration.type !== undefined) types.push(declaration.type);
  for (const parameter of declaration.params ?? []) types.push(parameter.type);
  for (const field of declaration.fields ?? []) types.push(field.type);
  return types;
}

function typeName(type: TypeNode): string | undefined {
  return namedType(type) ?? property<string>(type, "builtin");
}

function isPointer(type: TypeNode): boolean {
  return pointedType(type) !== undefined;
}

function isVoidPointer(type: TypeNode): boolean {
  const pointed = pointedType(type);
  return pointed !== undefined && property<string>(pointed, "builtin") === "void";
}

function isInteger(
  context: RuleContext,
  type: TypeNode,
  seen = new Set<string>(),
): boolean {
  const name = typeName(type);
  if (name === undefined) return false;
  if (INTEGER_NAMES.has(name)) return true;
  if (seen.has(name)) return false;
  const declaration = context.typedefs.get(name);
  if (declaration?.type === undefined) return false;
  seen.add(name);
  return isInteger(context, declaration.type, seen);
}

function isBooleanName(name: string): boolean {
  return /^(?:is_|has_|enable)/.test(name) || /_enabled$/.test(name);
}

function isTextPointer(type: TypeNode, library: ModelLibrary): boolean {
  const pointed = pointedType(type);
  if (pointed === undefined) return false;
  const name = typeName(pointed);
  return (
    name === "char" ||
    name === `${library.prefix}_char16` ||
    name?.endsWith("_char16") === true ||
    name === "char16_t" ||
    name === "char8_t" ||
    name === "wchar_t"
  );
}

function hasAnnotation(
  annotations: readonly Annotation[],
  key: string,
  value?: string,
): boolean {
  return annotations.some(
    (annotation) =>
      annotation.key === key && (value === undefined || annotation.value === value),
  );
}

function encodingFromName(name: string): string | undefined {
  return /_(utf8|utf16|latin1)(?:_|$)/.exec(name)?.[1];
}

function hasTextEncoding(
  functionName: string,
  parameter: Parameter,
  library: ModelLibrary,
): boolean {
  const pointed = pointedType(parameter.type);
  const pointedName = pointed === undefined ? undefined : typeName(pointed);
  return (
    encodingFromName(functionName) !== undefined ||
    encodingFromName(parameter.name) !== undefined ||
    pointedName === `${library.prefix}_char16` ||
    pointedName?.endsWith("_char16") === true ||
    parameter.annotations.some(
      (annotation) =>
        annotation.key === "encoding" &&
        ["utf8", "utf16", "latin1"].includes(annotation.value ?? ""),
    )
  );
}

function hasTextCount(
  context: RuleContext,
  parameters: readonly Parameter[],
  index: number,
): boolean {
  const parameter = parameters[index];
  if (hasAnnotation(parameter.annotations, "zero_terminated")) return true;
  const next = parameters[index + 1];
  return (
    next !== undefined &&
    isInteger(context, next.type) &&
    (new RegExp(`^${parameter.name}_(?:length|count|size|capacity)$`).test(next.name) ||
      /^(?:length|count|size|capacity)$/.test(next.name))
  );
}

interface FunctionSite {
  readonly name: string;
  readonly params: readonly Parameter[];
}

function functionSites(declaration: Declaration): FunctionSite[] {
  const sites: FunctionSite[] = [];
  if (declaration.kind === "function") {
    sites.push({ name: declaration.name, params: declaration.params ?? [] });
  } else if (declaration.kind === "typedef") {
    const fn = functionTypeOfTypedef(declaration);
    if (fn !== undefined) sites.push({ name: declaration.name, params: fn.params });
  }
  for (const field of declaration.fields ?? []) {
    const fn = directFunctionType(field.type);
    if (fn !== undefined) sites.push({ name: field.name, params: fn.params });
  }
  return sites;
}

function declarationNames(declaration: Declaration): string[] {
  const names = [declaration.name];
  for (const field of declaration.fields ?? []) names.push(field.name);
  for (const site of functionSites(declaration)) {
    for (const parameter of site.params) {
      if (parameter.name !== "") names.push(parameter.name);
    }
  }
  for (const enumerator of declaration.enumerators ?? []) names.push(enumerator.name);
  return names;
}

function objectFromHandleName(context: RuleContext, name: string): string | undefined {
  const own = context.handles.get(name);
  if (own !== undefined) return own;
  if (name === "napi_env") return "env";
  return undefined;
}

function handleNameFromOutput(type: TypeNode): string | undefined {
  const pointed = pointedType(type);
  return pointed === undefined ? undefined : namedType(pointed);
}

function isFunctionTable(declaration: Declaration): boolean {
  return (
    declaration.kind === "struct" &&
    (declaration.fields ?? []).some((field) => directFunctionType(field.type) !== undefined)
  );
}

function checkPrefix(context: RuleContext, declaration: Declaration): void {
  const prefix =
    declaration.kind === "macro"
      ? `${context.library.macroPrefix}_`
      : `${context.library.prefix}_`;
  const names =
    declaration.kind === "enum"
      ? [declaration.name, ...(declaration.enumerators ?? []).map((e) => e.name)]
      : [declaration.name];
  for (const name of names) {
    const typeInitializer =
      declaration.kind === "macro" &&
      name.endsWith("_INIT") &&
      context.library.declarations.some(
        (candidate) =>
          candidate.kind !== "macro" &&
          `${candidate.name.toUpperCase()}_INIT` === name,
      );
    if (!name.startsWith(prefix) && !typeInitializer) {
      addFinding(context, "prefix", declaration, `"${name}" does not start with "${prefix}"`);
    }
  }
}

function checkNamingCase(context: RuleContext, declaration: Declaration): void {
  const names = declarationNames(declaration);
  for (const name of names) {
    const includeGuard =
      declaration.kind === "macro" &&
      name === declaration.name &&
      /^[A-Z][A-Z0-9_]*_H_$/.test(name);
    const valid =
      declaration.kind === "macro" && name === declaration.name
        ? UPPER_SNAKE_RE.test(name) || includeGuard
        : LOWER_SNAKE_RE.test(name);
    if (!valid) {
      addFinding(
        context,
        "naming_case",
        declaration,
        `"${name}" is not ${declaration.kind === "macro" ? "UPPER_SNAKE" : "lowercase_snake"}`,
      );
    }
  }
}

function checkEnumeratorNames(context: RuleContext, declaration: Declaration): void {
  if (declaration.kind !== "enum") return;
  const prefix = `${declaration.name}_`;
  for (const enumerator of declaration.enumerators ?? []) {
    if (!enumerator.name.startsWith(prefix)) {
      addFinding(
        context,
        "enumerator_name",
        declaration,
        `enumerator "${enumerator.name}" does not start with "${prefix}"`,
      );
    }
  }
}

function checkFunctionName(context: RuleContext, declaration: Declaration): void {
  if (declaration.kind !== "function") return;
  const prefix = `${context.library.prefix}_`;
  if (!declaration.name.startsWith(prefix)) return;
  const parameters = declaration.params ?? [];
  const verb = /_(create|open)(?:_|$)/.exec(declaration.name)?.[1];
  if (verb !== undefined) {
    const output = parameters.findLast(
      (parameter) =>
        parameter.name.startsWith("out_") &&
        handleNameFromOutput(parameter.type) !== undefined,
    );
    const outputHandle =
      output === undefined ? undefined : handleNameFromOutput(output.type);
    const outputObject =
      outputHandle === undefined ? undefined : objectFromHandleName(context, outputHandle);
    if (outputObject !== undefined) {
      const expected = `${prefix}${outputObject}_${verb}`;
      if (
        declaration.name !== expected &&
        !declaration.name.startsWith(`${expected}_`)
      ) {
        addFinding(
          context,
          "function_name",
          declaration,
          `"${declaration.name}" returns ${outputHandle}; expected "${expected}"`,
        );
      }
      return;
    }
  }
  const firstName = parameters.length > 0 ? namedType(parameters[0].type) : undefined;
  const object = firstName === undefined ? undefined : objectFromHandleName(context, firstName);
  if (object !== undefined && !declaration.name.startsWith(`${prefix}${object}_`)) {
    addFinding(
      context,
      "function_name",
      declaration,
      `"${declaration.name}" acts on ${firstName}; expected "${prefix}${object}_<verb>"`,
    );
  }
}

function checkTableName(context: RuleContext, declaration: Declaration): void {
  if (isFunctionTable(declaration) && !declaration.name.endsWith("_api")) {
    addFinding(
      context,
      "table_name",
      declaration,
      `function table "${declaration.name}" must end in "_api"`,
    );
  }
}

function pointedName(type: TypeNode): string | undefined {
  const pointed = pointedType(type);
  return pointed === undefined ? undefined : namedType(pointed);
}

function checkTableFields(context: RuleContext, declaration: Declaration): void {
  const check = (name: string, type: TypeNode) => {
    const target = pointedName(type);
    if (
      target !== undefined &&
      (context.functionTables.has(target) || target.endsWith("_api")) &&
      name !== "api"
    ) {
      addFinding(
        context,
        "table_field",
        declaration,
        `pointer to function table ${target} is named "${name}", not "api"`,
      );
    }
  };
  for (const field of declaration.fields ?? []) check(field.name, field.type);
  for (const site of functionSites(declaration)) {
    for (const parameter of site.params) check(parameter.name, parameter.type);
  }
}

function reservedNameReason(name: string): string | undefined {
  if (name.endsWith("_t")) return 'uses the reserved "_t" suffix';
  if (name.includes("__")) return 'contains reserved "__"';
  if (/^_[A-Z]/.test(name)) return 'starts with reserved "_" plus a capital';
  if (WINDOWS_MACRO_NAMES.has(name)) return "is a Windows SDK macro name";
  return undefined;
}

function checkReservedNames(context: RuleContext, declaration: Declaration): void {
  for (const name of declarationNames(declaration)) {
    const reason = reservedNameReason(name);
    if (reason !== undefined) {
      addFinding(context, "reserved_name", declaration, `"${name}" ${reason}`);
    }
  }
}

function checkTypedefName(context: RuleContext, declaration: Declaration): void {
  if (!["struct", "union", "enum"].includes(declaration.kind)) return;
  if (
    declaration.incomplete &&
    declaration.name.endsWith("_s") &&
    [...context.typedefs.values()].some(
      (candidate) =>
        candidate.type !== undefined && pointedName(candidate.type) === declaration.name,
    )
  ) {
    return;
  }
  if (!context.typedefs.has(declaration.name)) {
    addFinding(
      context,
      "typedef_name",
      declaration,
      `${declaration.kind} "${declaration.name}" has no same-named typedef`,
    );
  }
}

function checkHandleOpaque(context: RuleContext, declaration: Declaration): void {
  if (declaration.kind !== "typedef" || declaration.type === undefined) return;
  const target = pointedName(declaration.type);
  if (target === undefined || (!target.endsWith("_s") && !context.records.has(target))) {
    return;
  }
  const expected = `${declaration.name}_s`;
  const record = context.records.get(target);
  if (target !== expected || record?.kind !== "struct" || record.incomplete !== true) {
    addFinding(
      context,
      "handle_opaque",
      declaration,
      `handle "${declaration.name}" must point to incomplete struct "${expected}"`,
    );
  }
}

function checkPortableTypes(context: RuleContext, declaration: Declaration): void {
  if (declaration.variadic) {
    addFinding(context, "portable_types", declaration, "variadic functions are not portable");
  }
  if ((declaration.fields ?? []).some((field) => field.bitWidth !== undefined)) {
    addFinding(context, "portable_types", declaration, "bitfields are not portable");
  }
  const bad = new Set<string>();
  for (const type of allDeclarationTypes(declaration)) {
    walkType(type, (node) => {
      const name = typeName(node);
      if (name !== undefined && PORTABILITY_NAMES.has(name)) bad.add(name);
      const fn = property<FunctionType>(node, "function");
      if (fn?.variadic) bad.add("varargs");
    });
  }
  for (const name of bad) {
    addFinding(context, "portable_types", declaration, `type "${name}" is not portable`);
  }
}

function checkBoolTypes(context: RuleContext, declaration: Declaration): void {
  const members: { name: string; type: TypeNode }[] = [
    ...(declaration.fields ?? []),
    ...functionSites(declaration).flatMap((site) => site.params),
  ];
  for (const member of members) {
    if (
      isBooleanName(member.name) &&
      isInteger(context, member.type) &&
      typeName(member.type) !== "bool"
    ) {
      addFinding(
        context,
        "bool_type",
        declaration,
        `"${member.name}" has integer type ${typeName(member.type)}; use bool`,
      );
    }
  }
}

function checkEnumPinned(context: RuleContext, declaration: Declaration): void {
  if (declaration.kind !== "enum" || declaration.type === undefined) return;
  const flags = hasAnnotation(annotationsOf(declaration), "flags");
  const enumMacro = hasAnnotation(
    annotationsOf(declaration),
    flags ? "flags" : "enum",
    declaration.name,
  );
  const expected = flags ? "uint32_t" : "int32_t";
  if (typeName(declaration.type) !== expected || !enumMacro) {
    addFinding(
      context,
      "enum_pinned",
      declaration,
      `${flags ? "flags" : "enum"} "${declaration.name}" must use the Stela macro with ${expected}`,
    );
  }
}

function checkMacroConstant(context: RuleContext, declaration: Declaration): void {
  if (
    declaration.kind !== "macro" ||
    declaration.functionLike ||
    declaration.perTarget === undefined ||
    !Object.values(declaration.perTarget).some((facts) => facts.value !== undefined) ||
    declaration.name.endsWith("_ABI_VERSION") ||
    declaration.name.endsWith("_AUTO_LENGTH") ||
    context.macroConstants.has(declaration.name)
  ) {
    return;
  }
  addFinding(
    context,
    "macro_constants",
    declaration,
    `integer macro "${declaration.name}" should be an enum value`,
  );
}

function checkFlagsRange(context: RuleContext, declaration: Declaration): void {
  if (
    declaration.kind !== "enum" ||
    !hasAnnotation(annotationsOf(declaration), "flags")
  ) {
    return;
  }
  for (const enumerator of declaration.enumerators ?? []) {
    if (enumerator.value < 0 || enumerator.value > 0x7fffffff) {
      addFinding(
        context,
        "flags_range",
        declaration,
        `flag "${enumerator.name}" uses a bit outside 0-30`,
      );
    }
  }
}

function checkTextParams(context: RuleContext, declaration: Declaration): void {
  for (const site of functionSites(declaration)) {
    for (let index = 0; index < site.params.length; index++) {
      const parameter = site.params[index];
      if (!isTextPointer(parameter.type, context.library)) continue;
      if (!hasTextEncoding(site.name, parameter, context.library)) {
        addFinding(
          context,
          "text_params",
          declaration,
          `text parameter "${parameter.name}" has no encoding`,
        );
      }
      const count = parameter.annotations.find((annotation) => annotation.key === "count");
      const counted =
        count !== undefined &&
        count.value !== undefined &&
        site.params.some(
          (candidate) =>
            candidate.name === count.value && isInteger(context, candidate.type),
        );
      if (!counted && !hasTextCount(context, site.params, index)) {
        addFinding(
          context,
          "text_params",
          declaration,
          `text parameter "${parameter.name}" has no count or zero-terminated annotation`,
        );
      }
    }
  }
}

function checkStructStrings(context: RuleContext, declaration: Declaration): void {
  if (!["struct", "union"].includes(declaration.kind)) return;
  for (const field of declaration.fields ?? []) {
    if (isTextPointer(field.type, context.library)) {
      addFinding(
        context,
        "struct_strings",
        declaration,
        `public ${declaration.kind} contains text pointer field "${field.name}"`,
      );
    }
  }
}

function checkStatusShape(context: RuleContext, declaration: Declaration): void {
  if (
    declaration.kind !== "function" ||
    declaration.result === undefined ||
    typeName(declaration.result.type) !== context.library.status
  ) {
    return;
  }
  const parameters = declaration.params ?? [];
  let sawOutput = false;
  for (const parameter of parameters) {
    const output = parameter.name.startsWith("out_") || hasAnnotation(parameter.annotations, "out");
    if (output) {
      sawOutput = true;
      if (!parameter.name.startsWith("out_")) {
        addFinding(
          context,
          "status_shape",
          declaration,
          `output parameter "${parameter.name}" must start with "out_"`,
        );
      }
      if (!isPointer(parameter.type)) {
        addFinding(
          context,
          "status_shape",
          declaration,
          `output parameter "${parameter.name}" is not a pointer`,
        );
      }
    } else if (sawOutput) {
      addFinding(
        context,
        "status_shape",
        declaration,
        `input parameter "${parameter.name}" follows an output parameter`,
      );
    }
  }
}

function checkStructSize(context: RuleContext, declaration: Declaration): void {
  const fields = declaration.fields ?? [];
  const index = fields.findIndex((field) => field.name === "struct_size");
  if (index < 0) return;
  if (index !== 0 || typeName(fields[index].type) !== "uint32_t") {
    addFinding(
      context,
      "struct_size_first",
      declaration,
      '"struct_size" must be the first field and have type uint32_t',
    );
  }
}

function checkCdecl(context: RuleContext, declaration: Declaration): void {
  for (const [target, facts] of Object.entries(declaration.perTarget ?? {})) {
    if (!/(?:^|-)i[3-6]86(?:-|$)/.test(target)) continue;
    if (
      (declaration.kind === "function" ||
        (declaration.kind === "typedef" &&
          functionTypeOfTypedef(declaration) !== undefined)) &&
      facts.callingConvention !== undefined &&
      facts.callingConvention !== "c"
    ) {
      addFinding(
        context,
        "cdecl",
        declaration,
        `"${declaration.name}" is ${facts.callingConvention} on ${target}, not cdecl`,
      );
    }
    facts.fieldCallingConventions?.forEach((callingConvention, index) => {
      if (callingConvention === null || callingConvention === "c") return;
      const field = declaration.fields?.[index];
      addFinding(
        context,
        "cdecl",
        declaration,
        `function-pointer field "${field?.name ?? index}" is ${callingConvention} on ${target}, not cdecl`,
      );
    });
  }
}

function checkCallbackShape(context: RuleContext, declaration: Declaration): void {
  if (declaration.kind !== "typedef" || declaration.name.endsWith("_fn")) return;
  const fn = functionTypeOfTypedef(declaration);
  if (fn === undefined) return;
  if (!declaration.name.endsWith("_cb")) {
    addFinding(
      context,
      "callback_shape",
      declaration,
      `callback typedef "${declaration.name}" must end in "_cb"`,
    );
    return;
  }
  const first = fn.params[0];
  if (first === undefined || first.name !== "user_data" || !isVoidPointer(first.type)) {
    addFinding(
      context,
      "callback_shape",
      declaration,
      `callback "${declaration.name}" must take void* user_data first`,
    );
  }
}

function checkParameterNames(context: RuleContext, declaration: Declaration): void {
  const check = (parameters: readonly Parameter[]) => {
    if (parameters.some((parameter) => parameter.name === "")) {
      addFinding(context, "param_names", declaration, "every parameter must have a name");
    }
  };
  for (const site of functionSites(declaration)) check(site.params);
}

interface AnnotationSite {
  readonly label: string;
  readonly annotations: readonly Annotation[];
  readonly names: ReadonlySet<string>;
  readonly parameters?: readonly Parameter[];
}

function annotationSites(declaration: Declaration): AnnotationSite[] {
  const fieldNames = new Set((declaration.fields ?? []).map((f) => f.name));
  const sites: AnnotationSite[] = [
    {
      label: declaration.name,
      annotations: annotationsOf(declaration),
      names: new Set(functionSites(declaration).flatMap((site) => site.params.map((p) => p.name))),
    },
    ...(declaration.fields ?? []).map((field) => ({
      label: field.name,
      annotations: field.annotations,
      names: fieldNames,
    })),
  ];
  for (const functionSite of functionSites(declaration)) {
    const names = new Set(functionSite.params.map((parameter) => parameter.name));
    sites.push(
      ...functionSite.params.map((parameter) => ({
        label: parameter.name,
        annotations: parameter.annotations,
        names,
        parameters: functionSite.params,
      })),
    );
  }
  return sites;
}

function checkKnownAnnotations(context: RuleContext, declaration: Declaration): void {
  for (const site of annotationSites(declaration)) {
    for (const annotation of site.annotations) {
      const projectKey = /^[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+$/.test(annotation.key);
      if (!KNOWN_ANNOTATIONS.has(annotation.key) && !projectKey) {
        addFinding(
          context,
          "annotation_known",
          declaration,
          `"${site.label}" has unknown annotation "${annotation.key}"`,
        );
        continue;
      }
      if (NO_VALUE_ANNOTATIONS.has(annotation.key) && annotation.value !== undefined) {
        addFinding(
          context,
          "annotation_known",
          declaration,
          `annotation "${annotation.key}" must not have a value`,
        );
      }
      const allowedValues: Readonly<Record<string, readonly string[]>> = {
        encoding: ["utf8", "utf16", "latin1"],
        scope: ["call", "async", "object", "notified", "forever"],
        transfer: ["none", "full"],
      };
      const allowed = allowedValues[annotation.key];
      if (
        allowed !== undefined &&
        (annotation.value === undefined || !allowed.includes(annotation.value))
      ) {
        addFinding(
          context,
          "annotation_known",
          declaration,
          `annotation "${annotation.key}" has invalid value "${annotation.value ?? ""}"`,
        );
      }
      if (
        REFERENCE_ANNOTATIONS.has(annotation.key) &&
        (annotation.value === undefined || !site.names.has(annotation.value))
      ) {
        addFinding(
          context,
          "annotation_known",
          declaration,
          `annotation "${annotation.key}" references missing parameter "${annotation.value ?? ""}"`,
        );
      }
      if (
        annotation.key === "allow" &&
        (annotation.value === undefined || !LINT_RULES.includes(annotation.value as LintRule))
      ) {
        addFinding(
          context,
          "annotation_known",
          declaration,
          `annotation "allow" names unknown rule "${annotation.value ?? ""}"`,
        );
      }
      if (
        (annotation.key === "enum" || annotation.key === "flags") &&
        annotation.value !== declaration.name
      ) {
        addFinding(
          context,
          "annotation_known",
          declaration,
          `annotation "${annotation.key}" must name "${declaration.name}"`,
        );
      }
      if (
        annotation.key === "count" &&
        annotation.value !== undefined &&
        site.parameters?.some(
          (parameter) =>
            parameter.name === annotation.value &&
            !isInteger(context, parameter.type),
        )
      ) {
        addFinding(
          context,
          "annotation_known",
          declaration,
          `annotation "count" references non-integer parameter "${annotation.value}"`,
        );
      }
    }
  }
}

function checkRedundantAnnotations(context: RuleContext, declaration: Declaration): void {
  for (const site of functionSites(declaration)) {
    for (let index = 0; index < site.params.length; index++) {
      const parameter = site.params[index];
      for (const annotation of parameter.annotations) {
      let redundant = false;
      if (annotation.key === "out" && parameter.name.startsWith("out_")) {
        redundant = true;
      } else if (
        annotation.key === "encoding" &&
        hasTextEncoding(site.name, { ...parameter, annotations: [] }, context.library)
      ) {
        redundant = true;
      } else if (
        annotation.key === "count" &&
        site.params[index + 1]?.name === annotation.value &&
        /^(?:length|count|size)$/.test(annotation.value)
      ) {
        redundant = true;
      } else if (
        annotation.key === "closure" &&
        annotation.value === "user_data" &&
        site.params[index + 1]?.name === "user_data"
      ) {
        redundant = true;
      }
      if (redundant) {
        addFinding(
          context,
          "annotation_redundant",
          declaration,
          `annotation "${annotation.key}" on "${parameter.name}" repeats its name or type`,
        );
      }
      }
    }
  }
}

function inspectDeclaration(context: RuleContext, declaration: Declaration): void {
  checkPrefix(context, declaration);
  checkNamingCase(context, declaration);
  checkEnumeratorNames(context, declaration);
  checkFunctionName(context, declaration);
  checkTableName(context, declaration);
  checkTableFields(context, declaration);
  checkReservedNames(context, declaration);
  checkTypedefName(context, declaration);
  checkHandleOpaque(context, declaration);
  checkPortableTypes(context, declaration);
  checkBoolTypes(context, declaration);
  checkEnumPinned(context, declaration);
  checkMacroConstant(context, declaration);
  checkFlagsRange(context, declaration);
  checkTextParams(context, declaration);
  checkStructStrings(context, declaration);
  checkStatusShape(context, declaration);
  checkStructSize(context, declaration);
  checkCdecl(context, declaration);
  checkCallbackShape(context, declaration);
  checkParameterNames(context, declaration);
  checkKnownAnnotations(context, declaration);
  checkRedundantAnnotations(context, declaration);
}

function relativeFile(config: StelaConfig, file: string): string {
  const relative = path.relative(config.root, file);
  return relative.split(path.sep).join("/");
}

function ownedFiles(config: StelaConfig, library: LibraryConfig): string[] {
  const files: string[] = [];
  const visit = (entry: string) => {
    let stat;
    try {
      stat = statSync(entry);
    } catch (error) {
      throw new StelaError(`cannot inspect ${entry}: ${(error as Error).message}`);
    }
    if (stat.isDirectory()) {
      for (const child of readdirSync(entry).sort()) visit(path.join(entry, child));
    } else if (stat.isFile()) {
      files.push(entry);
    }
  };
  for (const entry of library.owns) visit(entry);
  return files;
}

function checkStelaHeader(
  context: RuleContext,
  config: StelaConfig,
  library: LibraryConfig,
): void {
  if (library.stelaHeader === false || context.severities.stela_h_current === "off") {
    return;
  }
  const template = parseTemplate(readFileSync(DEFAULT_TEMPLATE, "utf8"));
  const expected = generateLibraryHeader(template, {
    macroPrefix: library.macroPrefix,
    fileName: path.basename(library.stelaHeader),
    stelaNames: library.stelaNames,
  });
  const file = relativeFile(config, library.stelaHeader);
  if (!existsSync(library.stelaHeader)) {
    addFileFinding(
      context,
      "stela_h_current",
      file,
      1,
      1,
      path.basename(library.stelaHeader),
      "generated Stela header is missing",
    );
    return;
  }
  if (normalizeText(readFileSync(library.stelaHeader, "utf8")) !== expected) {
    addFileFinding(
      context,
      "stela_h_current",
      file,
      1,
      1,
      path.basename(library.stelaHeader),
      "generated Stela header is stale",
    );
  }
}

function checkReservedMacros(
  context: RuleContext,
  config: StelaConfig,
  library: LibraryConfig,
): void {
  if (library.stelaHeader === false || context.severities.reserved_macro === "off") {
    return;
  }
  const template = parseTemplate(readFileSync(DEFAULT_TEMPLATE, "utf8"));
  const reserved = new Set([
    ...generatedNames(template, library.macroPrefix, library.stelaNames).values(),
    includeGuard(library.macroPrefix),
  ]);
  const directive = /^\s*#\s*(define|undef|ifdef|ifndef|if|elif)\b(.*)$/;
  const identifier = /[A-Za-z_][A-Za-z0-9_]*/g;
  for (const file of ownedFiles(config, library)) {
    if (path.resolve(file) === path.resolve(library.stelaHeader)) continue;
    const source = normalizeText(readFileSync(file, "utf8"))
      .replace(/\/\*[\s\S]*?\*\//g, (comment) => comment.replace(/[^\n]/g, " "))
      .replace(/\/\/[^\n]*/g, "");
    const lines = source.split("\n");
    lines.forEach((line, index) => {
      const match = directive.exec(line);
      if (match === null) return;
      const names =
        match[1] === "if" || match[1] === "elif"
          ? (match[2].match(identifier) ?? [])
          : [match[2].match(identifier)?.[0]].filter(
              (name): name is string => name !== undefined,
            );
      for (const name of new Set(names)) {
        if (!reserved.has(name)) continue;
        addFileFinding(
          context,
          "reserved_macro",
          relativeFile(config, file),
          index + 1,
          Math.max(1, line.indexOf(name) + 1),
          name,
          `preprocessor directive ${match[1]} uses reserved Stela macro "${name}"`,
        );
      }
    });
  }
}

function ruleSeverities(config: StelaConfig): Readonly<Record<LintRule, LintSeverity>> {
  const known = new Set<string>(LINT_RULES);
  for (const rule of Object.keys(config.lint.rules)) {
    if (!known.has(rule)) throw new StelaError(`lint.rules: unknown rule "${rule}"`);
  }
  return {
    ...DEFAULT_SEVERITIES,
    ...config.lint.rules,
  };
}

function libraryContext(
  model: Model,
  library: ModelLibrary,
  config: StelaConfig,
  findings: LintFinding[],
): RuleContext {
  const configured = config.libraries.find((candidate) => candidate.name === library.name);
  if (configured === undefined) {
    throw new StelaError(`model library "${library.name}" is not in ${config.file}`);
  }
  if (
    configured.prefix !== library.prefix ||
    configured.macroPrefix !== library.macroPrefix ||
    configured.status !== library.status
  ) {
    throw new StelaError(`model library "${library.name}" does not match ${config.file}`);
  }
  const typedefs = new Map(
    library.declarations
      .filter((declaration) => declaration.kind === "typedef")
      .map((declaration) => [declaration.name, declaration]),
  );
  const records = new Map(
    library.declarations
      .filter((declaration) => declaration.kind === "struct" || declaration.kind === "union")
      .map((declaration) => [declaration.name, declaration]),
  );
  const handles = new Map<string, string>();
  for (const declaration of typedefs.values()) {
    if (declaration.type === undefined) continue;
    const target = pointedName(declaration.type);
    if (target !== undefined && records.get(target)?.incomplete) {
      const prefix = `${library.prefix}_`;
      handles.set(
        declaration.name,
        declaration.name.startsWith(prefix)
          ? declaration.name.slice(prefix.length)
          : declaration.name,
      );
    }
  }
  return {
    model,
    library,
    config: configured,
    severities: ruleSeverities(config),
    macroConstants: new Set(config.lint.macroConstants),
    findings,
    typedefs,
    records,
    handles,
    functionTables: new Set(
      library.declarations.filter(isFunctionTable).map((declaration) => declaration.name),
    ),
  };
}

/** Runs all v1 lint rules. Findings are sorted independently of traversal order. */
export function lintModel(model: Model, config: StelaConfig): LintFinding[] {
  const findings: LintFinding[] = [];
  const modelNames = new Set(model.libraries.map((library) => library.name));
  for (const library of config.libraries) {
    if (!modelNames.has(library.name)) {
      throw new StelaError(`configured library "${library.name}" is missing from the model`);
    }
  }
  for (const library of model.libraries) {
    const context = libraryContext(model, library, config, findings);
    for (const declaration of library.declarations) inspectDeclaration(context, declaration);
    checkStelaHeader(context, config, context.config!);
    checkReservedMacros(context, config, context.config!);
  }
  const compareText = (a: string, b: string) => (a < b ? -1 : a > b ? 1 : 0);
  return findings.sort(
    (a, b) =>
      compareText(a.file, b.file) ||
      a.line - b.line ||
      a.column - b.column ||
      compareText(a.rule, b.rule) ||
      compareText(a.message, b.message),
  );
}

/** Deterministic diagnostics for humans or other tools. */
export function formatFindings(
  findings: readonly LintFinding[],
  format: "text" | "json",
): string {
  if (format === "json") return JSON.stringify(findings, null, 2);
  return findings
    .map(
      (finding) =>
        `${finding.file}:${finding.line}:${finding.column}: ${finding.severity}` +
        `[${finding.rule}]: ${finding.message}`,
    )
    .join("\n");
}
