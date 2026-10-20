// Public C ABI header gate (C ABI conventions §2 and §14; Stela design §13).
// It extracts every library in stela.config.json once, for every target, then:
//
// - compiles Stela's generated header tests: each entry header, and each
//   generated <lib>_stela.h, on its own, included twice, as C and as C++,
//   with clang and MSVC, warnings as errors, for every target (x64, x86,
//   arm64);
// - compiles Stela's generated layout tests: static asserts of each record's
//   size, alignment, and field offsets and each enumerator's value;
// - requires `stela lint` to report no findings at all;
// - requires `stela diff` against the committed baselines (api/*.model.json)
//   to report no breaks.
//
//   node --test scripts/header-check.ts
//   node --test --test-name-pattern=sbox scripts/header-check.ts
//
// clang runs freestanding with no system include paths, so a header can reach
// only <stddef.h>, <stdint.h>, <stdbool.h>, and its own library's headers;
// <wchar.h>, <windows.h>, and C++ headers fail. MSVC runs with the vcvars
// environment of each target architecture.

import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { after, before, describe, test } from "node:test";
import { compileJobs, compileUnits, msvcEnvs } from "../tools/stela/src/compile.ts";
import { loadConfig } from "../tools/stela/src/config.ts";
import { diffModels, experimentalHeaders, formatDiff, loadBaselines } from "../tools/stela/src/diff.ts";
import { extractModel } from "../tools/stela/src/extract.ts";
import { generate } from "../tools/stela/src/generators.ts";
import { formatFindings, lintModel } from "../tools/stela/src/lint.ts";
import { type Model, formatMismatch } from "../tools/stela/src/model.ts";
import { type CompileResult, findToolchain, runJobs } from "../tools/stela/src/toolchain.ts";

const repoRoot = path.resolve(import.meta.dirname, "..");

/** Groups failures by their first diagnostic so one broken line reads once. */
function report(results: readonly CompileResult[]): string {
  const failed = results.filter((r) => !r.ok);
  if (failed.length === 0) return "";
  const groups = new Map<string, CompileResult[]>();
  for (const r of failed) {
    const lines = r.output.trim().split(/\r?\n/);
    const key = lines.find((l) => /\berror\b/.test(l)) ?? lines[0] ?? "";
    const group = groups.get(key) ?? [];
    group.push(r);
    groups.set(key, group);
  }
  const parts = [`${failed.length} of ${results.length} compiles failed`];
  for (const group of groups.values()) {
    parts.push(`--- ${group.map((r) => r.job.label).join(", ")}`);
    parts.push(...group[0].output.trim().split(/\r?\n/).slice(0, 8));
  }
  return parts.join("\n");
}

describe("public C ABI headers", () => {
  const config = loadConfig(path.join(repoRoot, "stela.config.json"));
  let dir = "";
  let setupError: string | undefined;
  let extracted: Model | undefined;
  /** Compile results by generated file, as `<library>/<file name>`. */
  const results = new Map<string, CompileResult[]>();

  before(async () => {
    const tc = await findToolchain();
    if (typeof tc === "string") {
      setupError = `toolchain: ${tc}`;
      return;
    }
    try {
      const { model, mismatches } = await extractModel(config);
      if (mismatches.length > 0) {
        setupError = mismatches.map(formatMismatch).join("\n");
        return;
      }
      extracted = model;
      dir = mkdtempSync(path.join(os.tmpdir(), "header-check-"));
      const files = [...generate("header-tests", model), ...generate("layout-tests", model)];
      for (const file of files) {
        const target = path.join(dir, ...file.path.split("/"));
        mkdirSync(path.dirname(target), { recursive: true });
        writeFileSync(target, file.text);
      }
      const units = compileUnits(model, config.root, dir, files);
      const done = await runJobs(compileJobs(tc, await msvcEnvs(tc, units), units));
      for (const result of done) {
        // Job labels start with the generated file's name; its directory is the library.
        const key = `${path.basename(result.job.cwd)}/${result.job.label.split(" ")[0]}`;
        const list = results.get(key) ?? [];
        list.push(result);
        results.set(key, list);
      }
    } catch (e) {
      setupError = (e as Error).message;
    }
  });

  after(() => {
    if (dir) rmSync(dir, { recursive: true, force: true });
  });

  function check(keys: (key: string) => boolean): void {
    assert.equal(setupError, undefined, setupError ?? "setup failed");
    const list = [...results].filter(([key]) => keys(key)).flatMap(([, r]) => r);
    assert.ok(list.length > 0, "no compile jobs ran");
    const failures = report(list);
    if (failures) assert.fail(failures);
  }

  // Header tests (Stela design §11): `<library>/<header stem>.header.{c,cpp}`,
  // for each entry header and the library's generated stela header.
  for (const library of config.libraries) {
    const headers = library.stelaHeader === false ? library.headers : [...library.headers, library.stelaHeader];
    for (const header of headers) {
      const stem = path.basename(header).replace(/\.[^.]*$/, "");
      for (const ext of ["c", "cpp"]) {
        const file = `${library.name}/${stem}.header.${ext}`;
        test(`${file} compiles`, () => check((key) => key === file));
      }
    }
  }

  for (const library of config.libraries) {
    test(`${library.name} layout`, () =>
      check((key) => key.startsWith(`${library.name}/`) && !/\.header\.(c|cpp)$/.test(key)));
  }

  test("stela lint is clean", () => {
    assert.ok(extracted, setupError ?? "model extraction failed");
    const findings = lintModel(extracted, config);
    assert.equal(findings.length, 0, formatFindings(findings, "text"));
  });

  // A break needs a deliberate baseline refresh (`stela baseline`) in the same
  // change; an addition needs the library's *_ABI_VERSION bumped.
  test("stela diff against the baselines has no breaks", () => {
    assert.ok(extracted, setupError ?? "model extraction failed");
    const findings = diffModels(loadBaselines(config), extracted, {
      experimentalHeaders: experimentalHeaders(config),
      libraries: Object.keys(config.diff.baselines),
    });
    const breaks = findings.filter((f) => f.class === "break");
    assert.equal(breaks.length, 0, formatDiff(breaks, "text"));
  });
});
