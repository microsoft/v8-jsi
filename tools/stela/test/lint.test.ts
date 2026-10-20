import assert from "node:assert/strict";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { test } from "node:test";
import { parseConfig, type StelaConfig } from "../src/config.ts";
import { StelaError } from "../src/errors.ts";
import {
  formatFindings,
  lintModel,
  type LintRule,
} from "../src/lint.ts";
import type {
  Declaration,
  Field,
  Model,
  Parameter,
  TypeNode,
} from "../src/model.ts";
import {
  generateLibraryHeader,
  parseTemplate,
} from "../src/vendor.ts";
import { runCli, tempDir } from "./helpers.ts";

const location = { file: "include/alpha.h", line: 10, column: 1 };
const named = (name: string): TypeNode => ({ name });
const builtin = (name: string): TypeNode => ({ builtin: name });
const pointer = (type: TypeNode): TypeNode => ({ pointer: type });
const parameter = (
  name: string,
  type: TypeNode,
  annotations: { key: string; value?: string }[] = [],
): Parameter => ({ name, type, annotations });
const field = (
  name: string,
  type: TypeNode,
  annotations: { key: string; value?: string }[] = [],
): Field => ({ name, type, annotations, doc: null });
const base = (
  kind: Declaration["kind"],
  name: string,
  extra: Partial<Declaration> = {},
): Declaration => ({
  kind,
  name,
  location,
  doc: null,
  annotations: [],
  ...extra,
});
const fn = (
  name: string,
  params: Parameter[] = [],
  extra: Partial<Declaration> = {},
): Declaration =>
  base("function", name, {
    result: { type: named("alpha_status") },
    params,
    variadic: false,
    perTarget: {
      "i686-pc-windows-msvc": { callingConvention: "c" },
    },
    ...extra,
  });
const fnPointer = (params: Parameter[] = []): TypeNode => ({
  pointer: {
    function: {
      result: builtin("void"),
      params,
      variadic: false,
    },
  },
}) as unknown as TypeNode;

function model(declarations: Declaration[]): Model {
  return {
    schema: "stela.model/1",
    producer: { stela: "0.1.0", libclang: "test" },
    targets: ["i686-pc-windows-msvc"],
    libraries: [
      {
        name: "alpha",
        prefix: "alpha",
        macroPrefix: "ALPHA",
        status: "alpha_status",
        headers: ["include/alpha.h"],
        includeDirs: ["include"],
        defines: [],
        declarations,
      },
    ],
  };
}

function config(
  root: string,
  options: {
    stelaHeader?: string | false;
    rules?: Record<string, string>;
    macroConstants?: string[];
    owns?: string[];
  } = {},
): StelaConfig {
  return parseConfig(
    {
      schema: "stela.config/1",
      targets: ["i686-pc-windows-msvc"],
      lint: {
        rules: {
          stela_h_current: "off",
          reserved_macro: "off",
          ...options.rules,
        },
        macroConstants: options.macroConstants ?? [],
      },
      libraries: [
        {
          name: "alpha",
          prefix: "alpha",
          macroPrefix: "ALPHA",
          headers: ["include/alpha.h"],
          owns: options.owns,
          status: "alpha_status",
          stelaHeader: options.stelaHeader ?? false,
        },
      ],
    },
    path.join(root, "stela.config.json"),
  );
}

function rulesFor(root: string, declarations: Declaration[], cfg = config(root)): Set<LintRule> {
  return new Set(lintModel(model(declarations), cfg).map((finding) => finding.rule));
}

const handleDeclarations = (): Declaration[] => [
  base("struct", "alpha_session_s", { fields: [], incomplete: true }),
  base("typedef", "alpha_session", {
    type: pointer(named("alpha_session_s")),
    perTarget: {
      "i686-pc-windows-msvc": { size: 4, align: 4 },
    },
  }),
];

type Scenario = (root: string) => {
  declarations: Declaration[];
  config?: StelaConfig;
};

const scenarios: Record<LintRule, Scenario> = {
  prefix: () => ({
    declarations: [base("typedef", "foreign_value", { type: named("uint32_t") })],
  }),
  naming_case: () => ({
    declarations: [fn("alpha_run", [parameter("badName", named("uint32_t"))])],
  }),
  enumerator_name: () => ({
    declarations: [
      base("typedef", "alpha_mode", { type: named("alpha_mode") }),
      base("enum", "alpha_mode", {
        type: named("int32_t"),
        enumerators: [{ name: "alpha_wrong", value: 0, doc: null }],
      }),
    ],
  }),
  function_name: () => ({
    declarations: [
      ...handleDeclarations(),
      fn("alpha_close_session", [parameter("session", named("alpha_session"))]),
    ],
  }),
  table_name: () => ({
    declarations: [
      base("struct", "alpha_session_vtable", {
        fields: [field("run", fnPointer([parameter("self", pointer(builtin("void"))) ]))],
      }),
    ],
  }),
  table_field: () => ({
    declarations: [
      base("struct", "alpha_session_api", {
        fields: [field("run", fnPointer())],
      }),
      base("typedef", "alpha_session_api", { type: named("alpha_session_api") }),
      fn("alpha_set_api", [parameter("table", pointer(named("alpha_session_api")))]),
    ],
  }),
  reserved_name: () => ({
    declarations: [
      base("struct", "alpha_record", { fields: [field("interface", named("uint32_t"))] }),
      base("typedef", "alpha_record", { type: named("alpha_record") }),
    ],
  }),
  typedef_name: () => ({
    declarations: [base("struct", "alpha_record", { fields: [] })],
  }),
  handle_opaque: () => ({
    declarations: [
      base("struct", "alpha_wrong_s", { fields: [], incomplete: true }),
      base("typedef", "alpha_session", { type: pointer(named("alpha_wrong_s")) }),
    ],
  }),
  portable_types: () => ({
    declarations: [
      base("struct", "alpha_record", { fields: [field("value", builtin("long"))] }),
      base("typedef", "alpha_record", { type: named("alpha_record") }),
    ],
  }),
  bool_type: () => ({
    declarations: [fn("alpha_set_enabled", [parameter("enabled", named("int32_t"))])],
  }),
  enum_pinned: () => ({
    declarations: [
      base("typedef", "alpha_mode", { type: named("alpha_mode") }),
      base("enum", "alpha_mode", {
        type: named("uint16_t"),
        enumerators: [{ name: "alpha_mode_default", value: 0, doc: null }],
      }),
    ],
  }),
  macro_constants: () => ({
    declarations: [
      base("macro", "ALPHA_LIMIT", {
        functionLike: false,
        replacement: ["4"],
        perTarget: { "i686-pc-windows-msvc": { value: "4" } },
      }),
    ],
  }),
  flags_range: () => ({
    declarations: [
      base("typedef", "alpha_flags", { type: named("uint32_t") }),
      base("enum", "alpha_flags", {
        annotations: [{ key: "flags", value: "alpha_flags" }],
        type: named("uint32_t"),
        enumerators: [{ name: "alpha_flags_bad", value: 0x80000000, doc: null }],
      }),
    ],
  }),
  text_params: () => ({
    declarations: [
      fn("alpha_set_name", [parameter("name", pointer({ builtin: "char", const: true }))]),
    ],
  }),
  struct_strings: () => ({
    declarations: [
      base("struct", "alpha_record", {
        fields: [field("name", pointer({ builtin: "char", const: true }))],
      }),
      base("typedef", "alpha_record", { type: named("alpha_record") }),
    ],
  }),
  status_shape: () => ({
    declarations: [
      fn("alpha_query", [
        parameter("out_value", pointer(named("uint32_t"))),
        parameter("flags", named("uint32_t")),
      ]),
    ],
  }),
  struct_size_first: () => ({
    declarations: [
      base("struct", "alpha_record", {
        fields: [field("value", named("uint32_t")), field("struct_size", named("uint16_t"))],
      }),
      base("typedef", "alpha_record", { type: named("alpha_record") }),
    ],
  }),
  cdecl: () => ({
    declarations: [
      fn("alpha_run", [], {
        perTarget: {
          "i686-pc-windows-msvc": { callingConvention: "stdcall" },
        },
      }),
    ],
  }),
  callback_shape: () => ({
    declarations: [
      base("typedef", "alpha_event_cb", {
        type: fnPointer([
          parameter("value", named("uint32_t")),
          parameter("user_data", pointer(builtin("void"))),
        ]),
      }),
    ],
  }),
  param_names: () => ({
    declarations: [fn("alpha_run", [parameter("", named("uint32_t"))])],
  }),
  annotation_known: () => ({
    declarations: [
      fn("alpha_run", [], {
        annotations: [{ key: "mystery" }],
      }),
    ],
  }),
  annotation_redundant: () => ({
    declarations: [
      fn("alpha_query", [
        parameter("out_value", pointer(named("uint32_t")), [{ key: "out" }]),
      ]),
    ],
  }),
  stela_h_current: (root) => ({
    declarations: [],
    config: config(root, {
      stelaHeader: "include/alpha_stela.h",
      rules: { stela_h_current: "error" },
    }),
  }),
  reserved_macro: (root) => {
    const include = path.join(root, "include");
    mkdirSync(include, { recursive: true });
    writeFileSync(
      path.join(include, "alpha.h"),
      "#ifdef ALPHA_OUT\n#endif\n",
    );
    const template = parseTemplate(
      readFileSync(path.join(import.meta.dirname, "..", "include", "stela.h"), "utf8"),
    );
    writeFileSync(
      path.join(include, "alpha_stela.h"),
      generateLibraryHeader(template, {
        macroPrefix: "ALPHA",
        fileName: "alpha_stela.h",
      }),
    );
    return {
      declarations: [],
      config: config(root, {
        stelaHeader: "include/alpha_stela.h",
        owns: ["include/alpha.h"],
        rules: { stela_h_current: "error", reserved_macro: "error" },
      }),
    };
  },
};

for (const [rule, makeScenario] of Object.entries(scenarios) as [
  LintRule,
  Scenario,
][]) {
  test(`lint rule ${rule}`, (t) => {
    const root = tempDir(t, `lint-${rule}`);
    const scenario = makeScenario(root);
    assert.ok(
      rulesFor(root, scenario.declarations, scenario.config ?? config(root)).has(rule),
      `${rule} did not report its violation`,
    );
  });
}

test("severity overrides, off, configured constants, and declaration suppression", (t) => {
  const root = tempDir(t, "lint-config");
  const boolDeclaration = fn("alpha_set_enabled", [
    parameter("enabled", named("int32_t")),
  ]);
  let findings = lintModel(
    model([boolDeclaration]),
    config(root, { rules: { bool_type: "error" } }),
  );
  assert.equal(findings.find((finding) => finding.rule === "bool_type")?.severity, "error");

  findings = lintModel(
    model([
      { ...boolDeclaration, annotations: [{ key: "allow", value: "bool_type" }] },
      scenarios.macro_constants(root).declarations[0],
    ]),
    config(root, {
      rules: { bool_type: "error" },
      macroConstants: ["ALPHA_LIMIT"],
    }),
  );
  assert.ok(!findings.some((finding) => finding.rule === "bool_type"));
  assert.ok(!findings.some((finding) => finding.rule === "macro_constants"));

  findings = lintModel(
    model([scenarios.prefix(root).declarations[0]]),
    config(root, { rules: { prefix: "off" } }),
  );
  assert.ok(!findings.some((finding) => finding.rule === "prefix"));
});

test("mandated guards, type initializers, counted text, and returned-object creates pass", (t) => {
  const root = tempDir(t, "lint-convention-shapes");
  const declarations = [
    base("macro", "ALPHA_H_", { functionLike: false, replacement: [] }),
    ...handleDeclarations(),
    base("macro", "ALPHA_SESSION_INIT", {
      functionLike: false,
      replacement: ["{", "NULL", "}"],
    }),
    fn("alpha_session_create_utf8", [
      parameter("name", pointer({ builtin: "char", const: true })),
      parameter("name_length", named("size_t")),
      parameter("out_session", pointer(named("alpha_session"))),
    ]),
  ];
  const findings = lintModel(model(declarations), config(root));
  for (const rule of ["naming_case", "prefix", "text_params", "function_name"] as const) {
    assert.ok(!findings.some((finding) => finding.rule === rule), rule);
  }
});

test("function-table slot parameters and calling conventions are linted", (t) => {
  const root = tempDir(t, "lint-function-table");
  const declarations = [
    base("struct", "alpha_session_api", {
      fields: [
        field(
          "run",
          fnPointer([
            parameter(
              "badName",
              pointer({ builtin: "char", const: true }),
              [{ key: "mystery" }],
            ),
            parameter("is_enabled", named("int32_t")),
          ]),
        ),
      ],
      perTarget: {
        "i686-pc-windows-msvc": {
          size: 4,
          align: 4,
          offsets: [0],
          fieldCallingConventions: ["stdcall"],
        },
      },
    }),
    base("typedef", "alpha_session_api", { type: named("alpha_session_api") }),
  ];
  const reported = rulesFor(root, declarations);
  for (const rule of [
    "naming_case",
    "text_params",
    "annotation_known",
    "bool_type",
    "cdecl",
  ] as const) {
    assert.ok(reported.has(rule), rule);
  }
});

test("factory naming uses the last output handle", (t) => {
  const root = tempDir(t, "lint-factory-output");
  const declarations = [
    ...handleDeclarations(),
    fn("alpha_wrong_create", [
      parameter("out_count", pointer(named("uint32_t"))),
      parameter("out_session", pointer(named("alpha_session"))),
    ]),
  ];
  assert.ok(rulesFor(root, declarations).has("function_name"));
});

test("annotation values and count types are validated", (t) => {
  const root = tempDir(t, "lint-annotation-values");
  const declarations = [
    fn("alpha_set_name", [
      parameter(
        "name",
        pointer({ builtin: "char", const: true }),
        [
          { key: "encoding", value: "banana" },
          { key: "count", value: "user_data" },
        ],
      ),
      parameter("user_data", pointer(builtin("void"))),
    ]),
  ];
  const reported = rulesFor(root, declarations);
  assert.ok(reported.has("annotation_known"));
  assert.ok(reported.has("text_params"));
});

test("enum provenance, union handles, and integer aliases are checked", (t) => {
  const root = tempDir(t, "lint-model-provenance");
  const declarations = [
    base("typedef", "alpha_mode", { type: named("alpha_mode") }),
    base("enum", "alpha_mode", {
      type: named("int32_t"),
      enumerators: [{ name: "alpha_mode_default", value: 0, doc: null }],
    }),
    base("union", "alpha_session_s", { fields: [], incomplete: true }),
    base("typedef", "alpha_session", {
      type: pointer(named("alpha_session_s")),
    }),
    base("typedef", "alpha_switch", { type: named("uint32_t") }),
    fn("alpha_set_enabled", [
      parameter("is_enabled", named("alpha_switch")),
    ]),
  ];
  const reported = rulesFor(root, declarations);
  assert.ok(reported.has("enum_pinned"));
  assert.ok(reported.has("handle_opaque"));
  assert.ok(reported.has("bool_type"));
});

test("reserved macro scan ignores replacements and comments", (t) => {
  const root = tempDir(t, "lint-reserved-operands");
  const include = path.join(root, "include");
  mkdirSync(include, { recursive: true });
  writeFileSync(
    path.join(include, "alpha.h"),
    "#define ALPHA_OPTIONAL_VALUE ALPHA_OPTIONAL\n#define ALPHA_VALUE 1 /* ALPHA_OUT */\n",
  );
  const template = parseTemplate(
    readFileSync(path.join(import.meta.dirname, "..", "include", "stela.h"), "utf8"),
  );
  writeFileSync(
    path.join(include, "alpha_stela.h"),
    generateLibraryHeader(template, {
      macroPrefix: "ALPHA",
      fileName: "alpha_stela.h",
    }),
  );
  const cfg = config(root, {
    stelaHeader: "include/alpha_stela.h",
    owns: ["include/alpha.h"],
    rules: { stela_h_current: "error", reserved_macro: "error" },
  });
  assert.ok(!rulesFor(root, [], cfg).has("reserved_macro"));
});

test("finding order uses locale-independent code-unit order", (t) => {
  const root = tempDir(t, "lint-order");
  const first = {
    ...scenarios.prefix(root).declarations[0],
    location: { file: "z.h", line: 1, column: 1 },
  };
  const second = {
    ...scenarios.prefix(root).declarations[0],
    name: "other_value",
    location: { file: "ä.h", line: 1, column: 1 },
  };
  const files = lintModel(model([second, first]), config(root))
    .filter((finding) => finding.rule === "prefix")
    .map((finding) => finding.file);
  assert.deepEqual(files, ["z.h", "ä.h"]);
});

test("unknown configured rules are tool errors", (t) => {
  const root = tempDir(t, "lint-unknown-rule");
  assert.throws(
    () =>
      lintModel(
        model([]),
        config(root, { rules: { made_up: "error" } }),
      ),
    StelaError,
  );
});

test("text and JSON finding formats are deterministic", (t) => {
  const root = tempDir(t, "lint-format");
  const findings = lintModel(model(scenarios.prefix(root).declarations), config(root));
  assert.match(
    formatFindings(findings, "text"),
    /^include\/alpha\.h:10:1: error\[prefix\]: /,
  );
  assert.deepEqual(JSON.parse(formatFindings(findings, "json")), findings);
});

test("lint CLI fails on errors unless --report-only is set", (t) => {
  const root = tempDir(t, "lint-cli");
  mkdirSync(path.join(root, "include"));
  writeFileSync(
    path.join(root, "stela.config.json"),
    JSON.stringify({
      schema: "stela.config/1",
      lint: {
        rules: {
          stela_h_current: "off",
          reserved_macro: "off",
        },
      },
      libraries: [
        {
          name: "alpha",
          prefix: "alpha",
          macroPrefix: "ALPHA",
          headers: ["include/alpha.h"],
          stelaHeader: false,
        },
      ],
    }),
  );
  writeFileSync(
    path.join(root, "model.json"),
    JSON.stringify(model(scenarios.prefix(root).declarations)),
  );

  let result = runCli(root, "lint", "--model", "model.json");
  assert.equal(result.code, 1, result.err);
  assert.match(result.out, /error\[prefix\]/);

  result = runCli(root, "lint", "--model", "model.json", "--report-only");
  assert.equal(result.code, 0, result.err);
  assert.match(result.out, /1 error\(s\)/);

  result = runCli(root, "lint", "--model", "model.json", "--format", "json");
  assert.equal(result.code, 1, result.err);
  assert.ok(Array.isArray(JSON.parse(result.out)));
});
