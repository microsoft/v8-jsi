// The compile matrix for generated <lib>_stela.h files (Stela design §5.1,
// §14): each library alone and two libraries (with different Stela template
// versions) in one translation unit in both orders; MSVC and clang; C and C++;
// every shipped architecture; before and after <windows.h>; and extractor
// mode, where every annotation must reach the AST.

import assert from "node:assert/strict";
import { copyFileSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { after, before, describe, test } from "node:test";
import { DEFAULT_TEMPLATE } from "../src/stela.ts";
import { generateLibraryHeader, parseTemplate } from "../src/vendor.ts";
import { templateV2 } from "./helpers.ts";
import {
  type CompileJob,
  type MsvcArch,
  type Toolchain,
  findToolchain,
  resolveOnPath,
  runJobs,
} from "../src/toolchain.ts";

const fixtures = path.join(import.meta.dirname, "fixtures");
const templateText = readFileSync(DEFAULT_TEMPLATE, "utf8");

const ASSERT_MACRO = "#define STELA_TEST_ASSERT(name, cond) typedef char stela_test_##name[(cond) ? 1 : -1]\n";

const ALPHA_CHECKS = `
STELA_TEST_ASSERT(alpha_status_size, sizeof(alpha_status) == sizeof(int32_t));
STELA_TEST_ASSERT(alpha_flags_size, sizeof(alpha_flags) == sizeof(uint32_t));
STELA_TEST_ASSERT(alpha_status_value, alpha_status_internal == 0x7001);
STELA_TEST_ASSERT(alpha_flags_value, (uint32_t)(alpha_flags_first | alpha_flags_last) == 0x40000001u);
int alpha_use(alpha_session session);
int alpha_use(alpha_session session) {
  alpha_record record = {sizeof(alpha_record), NULL, true};
  alpha_status status = alpha_session_set_flags(session, alpha_flags_first | alpha_flags_last, 1);
  if (status == alpha_status_ok) status = alpha_session_update(session, &record);
  return status == alpha_status_ok && record.enabled ? 0 : 1;
}
`;

const BETA_CHECKS = `
STELA_TEST_ASSERT(beta_status_size, sizeof(beta_status) == sizeof(int32_t));
STELA_TEST_ASSERT(beta_flags_size, sizeof(beta_open_flags) == sizeof(uint32_t));
int beta_use(void);
int beta_use(void) {
  beta_thing thing = NULL;
  beta_status status = beta_thing_create(beta_open_flags_shared, true, &thing);
  return status == beta_status_ok && beta_thing_get_label(thing) != NULL ? 0 : 1;
}
`;

// The three generated files v8-jsi ships, together in one translation unit.
const PRODUCTS = `#include <stdint.h>
#include "sbox_stela.h"
#include "v8host_stela.h"
#include "jsi_abi_stela.h"
${ASSERT_MACRO}
SBOX_ENUM(int32_t, sbox_probe){sbox_probe_a = 0};
V8HOST_FLAGS(uint32_t, v8host_probe){v8host_probe_a = 1u << 0};
JSI_ABI_ENUM(int32_t, jsi_probe){jsi_probe_a = 0};
STELA_TEST_ASSERT(sizes, sizeof(sbox_probe) == 4 && sizeof(v8host_probe) == 4 && sizeof(jsi_probe) == 4);
`;

const TUS: Record<string, string> = {
  "alpha_only.c": `#include "alpha.h"\n${ASSERT_MACRO}${ALPHA_CHECKS}`,
  "alpha_beta.c": `#include "alpha.h"\n#include "beta.h"\n${ASSERT_MACRO}${ALPHA_CHECKS}${BETA_CHECKS}`,
  "beta_alpha.c": `#include "beta.h"\n#include "alpha.h"\n${ASSERT_MACRO}${ALPHA_CHECKS}${BETA_CHECKS}`,
  "products.c": PRODUCTS,
};
const WINDOWS_TUS: Record<string, string> = {
  "windows_first.c": `#include <windows.h>\n#include "alpha.h"\n#include "beta.h"\n${ASSERT_MACRO}${ALPHA_CHECKS}${BETA_CHECKS}`,
  "windows_last.c": `#include "alpha.h"\n#include "beta.h"\n#include <windows.h>\n${ASSERT_MACRO}${ALPHA_CHECKS}${BETA_CHECKS}`,
};

const CLANG_WARNINGS = [
  "-Wall",
  "-Wextra",
  "-Wpedantic",
  "-Wconversion",
  "-Wsign-conversion",
  "-Wdocumentation",
  "-Werror",
];
const CLANG_MODES = [
  ["c", "c99"],
  ["c", "c11"],
  ["c", "c17"],
  ["c", "c23"],
  ["c++", "c++11"],
  ["c++", "c++14"],
  ["c++", "c++17"],
  ["c++", "c++20"],
  ["c++", "c++23"],
] as const;
const CLANG_TARGETS = [
  "x86_64-pc-windows-msvc",
  "i686-pc-windows-msvc",
  "aarch64-pc-windows-msvc",
  "arm64ec-pc-windows-msvc",
  "x86_64-unknown-linux-gnu",
  "aarch64-unknown-linux-gnu",
];
const MSVC_MODES = [
  ["c", undefined],
  ["c", "/std:c11"],
  ["c", "/std:c17"],
  ["c", "/std:clatest"],
  ["c++", "/std:c++14"],
  ["c++", "/std:c++17"],
  ["c++", "/std:c++20"],
  ["c++", "/std:c++latest"],
] as const;
const MSVC_ARCHS: MsvcArch[] = ["x64", "x86", "arm64"];

// What extractor mode must see in the AST, per fixture.
const ALPHA_ANNOTATIONS: Record<string, number> = {
  "stela:enum=alpha_status": 1,
  "stela:flags=alpha_flags": 1,
  "stela:nullable": 5,
  "stela:transfer=none": 1,
  "stela:transfer=full": 1,
  "stela:since=1": 1,
  "stela:zero_terminated": 1,
  "stela:encoding=utf8": 1,
  "stela:out": 2,
  "stela:count=capacity": 1,
  "stela:optional": 1,
  "stela:deprecated": 1,
  "stela:experimental": 1,
  "stela:inout": 1,
  "stela:allow=bool_type": 1,
  "stela:alpha.thread=any": 1,
  "stela:scope=object": 1,
  "stela:scope=notified": 1,
  "stela:closure=context": 1,
  "stela:destroy=on_destroy": 1,
  "stela:scope=async": 1,
  "stela:scope=forever": 1,
  "stela:scope=call": 1,
};
const BETA_ANNOTATIONS: Record<string, number> = {
  "stela:enum=beta_status": 1,
  "stela:flags=beta_open_flags": 1,
  "stela:since=2": 1,
  "stela:thread=any": 1,
  "stela:nullable": 2,
};

function sum(...maps: Record<string, number>[]): Record<string, number> {
  const out: Record<string, number> = {};
  for (const m of maps) for (const [k, v] of Object.entries(m)) out[k] = (out[k] ?? 0) + v;
  return Object.fromEntries(Object.entries(out).sort());
}

function annotationCounts(astDump: string): Record<string, number> {
  const counts: Record<string, number> = {};
  for (const m of astDump.matchAll(/AnnotateAttr\b[^\n]*?"(stela:[^"]*)"/g)) {
    counts[m[1]] = (counts[m[1]] ?? 0) + 1;
  }
  return Object.fromEntries(Object.entries(counts).sort());
}

function failures(results: { job: CompileJob; ok: boolean; output: string }[]): string {
  return results
    .filter((r) => !r.ok)
    .map((r) => `--- ${r.job.label}\n${r.output.trim().split("\n").slice(0, 12).join("\n")}`)
    .join("\n");
}

describe("generated headers compile", () => {
  let toolchain: Toolchain | string;
  let dir: string;

  before(async () => {
    toolchain = await findToolchain();
    dir = mkdtempSync(path.join(os.tmpdir(), "stela-compile-"));
    const v1 = parseTemplate(templateText);
    const v2 = parseTemplate(templateV2(templateText));
    const gen = (template: typeof v1, macroPrefix: string, fileName: string) =>
      writeFileSync(path.join(dir, fileName), generateLibraryHeader(template, { macroPrefix, fileName }));
    gen(v1, "ALPHA", "alpha_stela.h");
    gen(v2, "BETA", "beta_stela.h");
    gen(v1, "SBOX", "sbox_stela.h");
    gen(v1, "V8HOST", "v8host_stela.h");
    gen(v1, "JSI_ABI", "jsi_abi_stela.h");
    copyFileSync(path.join(fixtures, "alpha.h"), path.join(dir, "alpha.h"));
    copyFileSync(path.join(fixtures, "beta.h"), path.join(dir, "beta.h"));
    for (const [name, text] of Object.entries({ ...TUS, ...WINDOWS_TUS })) {
      writeFileSync(path.join(dir, name), text);
    }
  });

  after(() => rmSync(dir, { recursive: true, force: true }));

  test("alpha.h uses every v1 annotation", () => {
    const alpha = readFileSync(path.join(fixtures, "alpha.h"), "utf8");
    for (const name of parseTemplate(templateText).vocabulary) {
      assert.match(alpha, new RegExp(`\\bALPHA_${name}\\b`), `ALPHA_${name}`);
    }
  });

  test("clang: C99–C23 and C++11–C++23, Windows and Linux targets", async (t) => {
    if (typeof toolchain === "string") return t.skip(toolchain);
    const tc = toolchain;
    const jobs: CompileJob[] = [];
    for (const target of CLANG_TARGETS) {
      for (const [lang, std] of CLANG_MODES) {
        for (const tu of Object.keys(TUS)) {
          jobs.push({
            label: `clang ${target} ${std} ${tu}`,
            exe: tc.clang,
            args: ["-fsyntax-only", "-ffreestanding", `--target=${target}`, "-x", lang, `-std=${std}`, ...CLANG_WARNINGS, "-I", dir, tu],
            cwd: dir,
          });
        }
      }
    }
    const env = await tc.msvcEnv("x64");
    for (const [lang, std] of CLANG_MODES) {
      for (const tu of Object.keys(WINDOWS_TUS)) {
        jobs.push({
          label: `clang x86_64-pc-windows-msvc ${std} ${tu}`,
          exe: tc.clang,
          args: ["-fsyntax-only", "--target=x86_64-pc-windows-msvc", "-x", lang, `-std=${std}`, ...CLANG_WARNINGS, "-I", dir, tu],
          cwd: dir,
          env,
        });
      }
    }
    const results = await runJobs(jobs);
    t.diagnostic(`${results.length} clang compiles`);
    assert.equal(failures(results), "");
  });

  test("MSVC: C (default, C11, C17, latest) and C++14–latest, x64/x86/arm64", async (t) => {
    if (typeof toolchain === "string") return t.skip(toolchain);
    const tc = toolchain;
    const jobs: CompileJob[] = [];
    for (const arch of MSVC_ARCHS) {
      const env = await tc.msvcEnv(arch);
      const tus = arch === "x64" ? { ...TUS, ...WINDOWS_TUS } : TUS;
      for (const [lang, std] of MSVC_MODES) {
        for (const tu of Object.keys(tus)) {
          const args = ["/nologo", "/Zs", "/W4", "/WX", lang === "c" ? "/TC" : "/TP"];
          if (std) args.push(std);
          if (lang === "c++") args.push("/permissive-");
          if (tu in WINDOWS_TUS) args.push("/external:anglebrackets", "/external:W0");
          args.push("/I", dir, tu);
          jobs.push({ label: `cl ${arch} ${std ?? "default"} ${tu}`, exe: "cl.exe", args, cwd: dir, env });
        }
      }
    }
    // cl.exe resolves through the vcvars PATH, so run it via that environment.
    const resolved = jobs.map((j) => ({ ...j, exe: resolveOnPath("cl.exe", j.env ?? {}) }));
    const results = await runJobs(resolved);
    t.diagnostic(`${results.length} MSVC compiles`);
    assert.equal(failures(results), "");
  });

  test("extractor mode keeps every annotation; default mode has none", async (t) => {
    if (typeof toolchain === "string") return t.skip(toolchain);
    const tc = toolchain;
    const expected: Record<string, Record<string, number>> = {
      "alpha_only.c": sum(ALPHA_ANNOTATIONS),
      "alpha_beta.c": sum(ALPHA_ANNOTATIONS, BETA_ANNOTATIONS),
      "beta_alpha.c": sum(ALPHA_ANNOTATIONS, BETA_ANNOTATIONS),
    };
    const jobs: CompileJob[] = [];
    for (const [lang, std] of [["c++", "c++17"], ["c", "c23"], ["c", "c11"]] as const) {
      for (const tu of Object.keys(expected)) {
        for (const extract of [true, false]) {
          jobs.push({
            label: `${extract ? "extract" : "default"} ${std} ${tu}`,
            exe: tc.clang,
            args: [
              "-fsyntax-only", "-ffreestanding", "--target=x86_64-pc-windows-msvc", "-x", lang, `-std=${std}`,
              ...CLANG_WARNINGS, "-fno-color-diagnostics", ...(extract ? ["-DSTELA_EXTRACT"] : []),
              "-Xclang", "-ast-dump", "-I", dir, tu,
            ],
            cwd: dir,
          });
        }
      }
    }
    const results = await runJobs(jobs);
    assert.equal(failures(results), "");
    for (const r of results) {
      const [mode, std, tu] = r.job.label.split(" ");
      const counts = annotationCounts(r.output);
      if (mode === "default") {
        assert.deepEqual(counts, {}, r.job.label);
        continue;
      }
      // The C fallback (C11) has no fixed enums, so enum provenance
      // annotations and enum names exist only with fixed enums (C++ and C23).
      const want = { ...expected[tu] };
      if (std === "c11") {
        for (const k of Object.keys(want)) {
          if (k.startsWith("stela:enum=") || k.startsWith("stela:flags=")) delete want[k];
        }
      }
      assert.deepEqual(counts, want, r.job.label);
      if (std !== "c11") {
        assert.match(r.output, /EnumDecl\b[^\n]* alpha_status 'int32_t'/, r.job.label);
        if (tu !== "alpha_only.c") assert.match(r.output, /EnumDecl\b[^\n]* beta_status 'int32_t'/, r.job.label);
      }
    }
  });
});
