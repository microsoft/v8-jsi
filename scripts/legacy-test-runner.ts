// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

export function legacyTestEnvironment(env: NodeJS.ProcessEnv, filter?: string): NodeJS.ProcessEnv {
  if (Object.keys(env).some(key => /^GTEST_FILTER$/i.test(key)) && filter === undefined) {
    throw new Error("Inherited GTEST_FILTER is not allowed; pass --gtest-filter explicitly");
  }
  return Object.fromEntries(Object.entries(env).filter(([key]) =>
    !/^(CETRUN_|V8JSI_TEST_|GTEST_FILTER$|GTEST_OUTPUT$)/i.test(key)));
}

export function legacySuiteFilter(filter: string, exclusion: string): string {
  const separator = filter.indexOf("-");
  const positive = separator < 0 ? filter : filter.slice(0, separator);
  const negative = separator < 0 ? "" : filter.slice(separator + 1);
  return `${positive}-${negative ? negative + ":" : ""}${exclusion}`;
}

export function requireLegacyTests(count: number): void {
  if (!count) throw new Error("Legacy test run matched zero tests across all suites");
}

export function runLegacySuite(exe: string, args: string[],
    options: { cwd: string; env: NodeJS.ProcessEnv; stdio: "inherit"; timeout: number; allowEmpty?: boolean; countFilter?: string }): number {
  const directory = mkdtempSync(join(tmpdir(), "legacy-gtest-"));
  const report = join(directory, "result.xml");
  try {
    const child = spawnSync(exe, [...args, "--gtest_brief=0", "--gtest_color=no", "--gtest_output=xml:" + report],
      { ...options, stdio: "pipe", encoding: "utf8", maxBuffer: 32 * 1024 * 1024 });
    process.stdout.write(child.stdout ?? "");
    process.stderr.write(child.stderr ?? "");
    const running = [...(child.stdout ?? "").matchAll(/\[ RUN      \] (.+)/g)].at(-1)?.[1] ?? "before first test";
    if (child.error || child.status !== 0) {
      throw new Error(`Legacy suite failed: ${exe} (exit ${child.status}; last running test: ${running})`,
        { cause: child.error });
    }
    if (child.stderr.includes("SKIP: user CET not available")) return 0;
    const xml = readFileSync(report, "utf8");
    const count = Number(/<testsuites\s[^>]*tests="(\d+)"/.exec(xml)?.[1] ?? 0);
    if (!count && !options.allowEmpty) {
      throw new Error(`Legacy suite ran zero tests: ${exe} ${args.join(" ")}`);
    }
    if (options.countFilter === undefined) return count;
    return [...child.stdout.matchAll(/\[ RUN      \] (.+)/g)].filter(match => legacyTestMatches(match[1].trim(), options.countFilter!)).length;
  } finally { rmSync(directory, { recursive: true, force: true }); }
}

export function legacyTestMatches(name: string, filter: string): boolean {
  const separator = filter.indexOf("-");
  const positive = separator < 0 ? filter : filter.slice(0, separator);
  const negative = separator < 0 ? "" : filter.slice(separator + 1);
  const matches = (patterns: string) => patterns.split(":").some(pattern =>
    new RegExp("^" + pattern.replace(/[.+^${}()|[\]\\]/g, "\\$&").replaceAll("*", ".*").replaceAll("?", ".") + "$", "u").test(name));
  return matches(positive) && (!negative || !matches(negative));
}
