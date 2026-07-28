// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

/**
 * Computes the NuGet package version for a v8-jsi package from git history, so
 * the patch component never has to be hand-maintained. Read-only: it prints
 * versions and changes no files. Run with Node.js v24+ (TypeScript strip-types).
 *
 * Each package has its own plain-text version file under versions/, named for
 * the package (e.g. versions/Microsoft.JavaScript.V8.version). The first
 * non-comment line is the version; `#` lines are comments. One file per package
 * keeps each package's history isolated, so bumping one never perturbs another,
 * and adding a package is just adding a version file.
 *
 * Model (deterministic git-height, à la Nerdbank.GitVersioning, simplified for
 * our squash-only linear history):
 * - The committed `major.minor.patch` in the file is a manually settable *floor*.
 * - Computed patch = floor + git height, where height is the number of
 *   first-parent commits *after* the commit that last set this file's version.
 *   So at a hand-set bump the computed version equals the committed value, then
 *   grows by one per commit; a manual bump resets the height. Seed the floor
 *   >= the latest published patch so versions never overlap what's already out.
 * - On a release branch (master/main) the version is clean: `major.minor.patch`.
 *   Otherwise it is a prerelease `major.minor.patch-g<8-char sha>` (the `g`
 *   marks a git commit id, per `git describe`), so test-branch builds can run
 *   (and even publish) without colliding with releases.
 *
 * The git access mirrors the `GitRepo` pattern from `@rnx-kit/fork-sync`: a thin
 * typed wrapper over a single command runner with a `fallback` for commands
 * allowed to fail. Kept local and synchronous for now; it maps onto that module
 * if this is ever promoted to rnx-kit.
 *
 * With `--write` the computed versions are materialized into config.json (a
 * surgical field replace that preserves the file's exact formatting), so every
 * existing consumer keeps reading config.json unchanged. CI does this in a
 * dedicated job and publishes the result as an artifact; the write is never
 * committed (committing would move the anchor and reset the height).
 *
 * @example
 *   node scripts/compute-version.ts                                     # all packages
 *   node scripts/compute-version.ts versions/Microsoft.JavaScript.V8.version
 *   node scripts/compute-version.ts --json                              # machine-readable
 *   node scripts/compute-version.ts --write                             # materialize into config.json
 *
 * @module compute-version
 */

import { parseArgs } from "node:util";
import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { repoRoot } from "./build-common.ts";

// Release branches. Hardcoded for now (master today, migrating to main); this
// can move to a config-driven spec later if we ever grow a richer branch model.
const releaseBranches = ["master", "main"];

// Per-package version files live here (one <PackageId>.version file each).
const versionsDir = path.join(repoRoot, "versions");

// Maps each package to the config.json field that mirrors its version. The
// version files (versions/*.version) are the source of truth; `--write`
// materializes the computed versions into these config.json fields so the
// existing consumers (build.ts, the RC generators, the NuGet aggregators, the
// tag job) keep reading config.json unchanged. This legacy-shaped adapter is
// temporary: it goes away once config.json's version fields are retired.
const configFieldByPackageId: Record<string, string> = {
  "Microsoft.JavaScript.V8": "v8jsi_version",
  "ReactNative.V8Jsi.Windows": "version",
};

// =============================================================================
// Git access (mirrors the fork-sync GitRepo pattern: typed methods over one
// command runner, with a `fallback` for commands allowed to fail)
// =============================================================================

/**
 * Typed wrapper around a git repository directory. Synchronous (execFileSync,
 * no shell, so ref syntax like `^{commit}` and `A..B` passes through intact).
 */
class GitRepo {
  readonly dir: string;

  constructor(dir: string) {
    this.dir = dir;
  }

  /**
   * Run a git command and return its trimmed stdout. On a non-zero exit, return
   * `opts.fallback` when provided, otherwise rethrow. (System errors — e.g. git
   * not found — always throw.)
   */
  run(args: string[], opts?: { fallback?: string }): string {
    const fallback = opts?.fallback;
    try {
      return execFileSync("git", args, {
        encoding: "utf-8",
        cwd: this.dir,
        // When a fallback is provided the caller tolerates failure, so silence
        // git's stderr (e.g. "path ... exists on disk, but not in HEAD").
        stdio: fallback !== undefined ? ["ignore", "pipe", "ignore"] : undefined,
      }).trim();
    } catch (error) {
      if (fallback !== undefined) return fallback;
      throw error;
    }
  }

  /** Resolve a ref to its commit hash (optionally abbreviated). */
  revParse(ref: string, opts?: { short?: number }): string {
    const args = ["rev-parse"];
    if (opts?.short) args.push(`--short=${opts.short}`);
    args.push(ref);
    return this.run(args);
  }

  /** Content of a git object (e.g. `<rev>:<path>`). */
  show(objectRef: string, opts?: { fallback?: string }): string {
    return this.run(["show", objectRef], opts);
  }

  /** Current branch name (`HEAD` when detached). */
  currentBranch(): string {
    return this.run(["rev-parse", "--abbrev-ref", "HEAD"]);
  }

  /** First-parent commit hashes that touched `pathspec`, newest first. */
  logFirstParent(pathspec: string): string[] {
    return this.run(["log", "--first-parent", "--format=%H", "--", pathspec], {
      fallback: "",
    })
      .split(/\r?\n/)
      .filter(Boolean);
  }

  /** First-parent commit count for a range like `<anchor>..HEAD`. */
  countFirstParent(range: string): number {
    return Number(this.run(["rev-list", "--count", "--first-parent", range]));
  }

  /** The repository's root (parentless) commit. */
  rootCommit(): string {
    return this.run([
      "rev-list",
      "--max-parents=0",
      "--first-parent",
      "HEAD",
    ]).split(/\r?\n/)[0];
  }
}

const repo = new GitRepo(repoRoot);

// The version from plain-text .version content: the first non-blank line that
// is not a `#` comment. Returns undefined if there is none.
function parseVersionFile(text: string): string | undefined {
  for (const line of text.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (trimmed && !trimmed.startsWith("#")) return trimmed;
  }
  return undefined;
}

// The version recorded in a version file at a given commit (undefined if the
// file did not exist there).
function versionAt(repoRelPath: string, rev: string): string | undefined {
  const text = repo.show(`${rev}:${repoRelPath}`, { fallback: "" });
  return text ? parseVersionFile(text) : undefined;
}

// A repo-relative, forward-slash path for git (accepts absolute or cwd-relative).
function toRepoRelPath(file: string): string {
  return path.relative(repoRoot, path.resolve(file)).split(path.sep).join("/");
}

// =============================================================================
// version calculation
// =============================================================================

// Split "major.minor.patch" into parts. The patch (default 0) is the manually
// settable floor that the git height is added to.
function parseVersion(version: string): {
  major: string;
  minor: string;
  patch: number;
} {
  const parts = version.split(".");
  return {
    major: parts[0] ?? "0",
    minor: parts[1] ?? "0",
    patch: Number(parts[2] ?? "0") || 0,
  };
}

function stripRefsHeads(ref: string | undefined): string | undefined {
  return ref?.replace(/^refs\/heads\//, "");
}

// The current branch. ADO checks out a detached HEAD, so prefer the branch name
// the pipeline provides; fall back to git for local runs.
function detectBranch(): string {
  const fromEnv =
    stripRefsHeads(process.env.BUILD_SOURCEBRANCH) ??
    process.env.BUILD_SOURCEBRANCHNAME;
  if (fromEnv) return fromEnv;
  return repo.currentBranch();
}

// The commit where this file's version last became its current value. Walk the
// commits that touched the file (newest -> oldest, first-parent) and keep the
// oldest contiguous one that still carries the current version; stop when it
// differs. Comparing the parsed version (not "last commit that touched the
// file") means editing a comment does not reset the height.
function findAnchor(repoRelPath: string, currentVersion: string): string {
  let anchor: string | undefined;
  for (const commit of repo.logFirstParent(repoRelPath)) {
    if (versionAt(repoRelPath, commit) !== currentVersion) break;
    anchor = commit;
  }
  // No matching commit (brand-new file just added): count from the root commit.
  return anchor ?? repo.rootCommit();
}

interface PackageVersion {
  packageId: string;
  file: string;
  base: string;
  floor: number;
  height: number;
  version: string;
  prerelease: boolean;
}

// Compute one package's version from its version file. Versions take effect when
// committed (like Nerdbank.GitVersioning): the floor is read from HEAD, so an
// uncommitted edit is ignored until committed. A file not yet committed (initial
// adoption) is treated as height 0 = the floor.
function computeForFile(
  file: string,
  isRelease: boolean,
  sha8: string,
): PackageVersion {
  const repoRelPath = toRepoRelPath(file);
  const committed = versionAt(repoRelPath, "HEAD");
  const current = committed ?? parseVersionFile(fs.readFileSync(file, "utf-8"));
  if (!current) {
    throw new Error(`${repoRelPath}: no version line found`);
  }
  const { major, minor, patch } = parseVersion(current);
  const height =
    committed === undefined
      ? 0
      : repo.countFirstParent(`${findAnchor(repoRelPath, current)}..HEAD`);
  const core = `${major}.${minor}.${patch + height}`;
  return {
    packageId: path.basename(file).replace(/\.version$/, ""),
    file: repoRelPath,
    base: `${major}.${minor}`,
    floor: patch,
    height,
    // Prerelease suffix is `-g<sha8>`: the `g` marks a git commit id (per
    // `git describe`) and keeps the identifier non-numeric, so it is always a
    // valid SemVer prerelease tag.
    version: isRelease ? core : `${core}-g${sha8}`,
    prerelease: !isRelease,
  };
}

// =============================================================================
// config.json writeback
// =============================================================================

// Materialize the computed versions into config.json. For each package with a
// mapped field, surgically replace just that field's value, leaving the rest of
// the file (including its exact formatting) untouched -- config.json is written
// by PowerShell's ConvertTo-Json, and reformatting it would create noise. The
// write is meant to stay in the working tree only (CI publishes it as an
// artifact); committing it would move the height anchor.
function writeConfig(configPath: string, results: PackageVersion[]): void {
  let text = fs.readFileSync(configPath, "utf-8");
  for (const r of results) {
    const field = configFieldByPackageId[r.packageId];
    if (!field) {
      console.error(`  (no config.json field mapped for ${r.packageId}; skipped)`);
      continue;
    }
    // Field names come from the fixed map above, so this pattern is safe.
    const fieldPattern = new RegExp(`("${field}"\\s*:\\s*")[^"]*(")`);
    if (!fieldPattern.test(text)) {
      throw new Error(`${toRepoRelPath(configPath)} has no "${field}" field to update`);
    }
    text = text.replace(fieldPattern, `$1${r.version}$2`);
  }
  fs.writeFileSync(configPath, text);
}

// =============================================================================
// CLI
// =============================================================================

function printHelp(): void {
  console.log(
    [
      "Compute v8-jsi package versions from git history.",
      "",
      "Usage: node scripts/compute-version.ts [version-file...] [options]",
      "",
      "With no version-file, computes every versions/*.version file.",
      "",
      "Options:",
      "  --write            Materialize the computed versions into config.json",
      "                     (surgical field replace; formatting preserved). Leave",
      "                     the result in the working tree -- do not commit it.",
      "  --config <path>    config.json to write with --write (default: repo-root",
      "                     config.json).",
      "  --json             Emit machine-readable JSON.",
      "  --help             Show this help.",
    ].join("\n"),
  );
}

function main(): void {
  const { values, positionals } = parseArgs({
    allowPositionals: true,
    options: {
      help: { type: "boolean", default: false },
      json: { type: "boolean", default: false },
      write: { type: "boolean", default: false },
      config: { type: "string" },
    },
  });

  if (values.help) {
    printHelp();
    return;
  }

  const files =
    positionals.length > 0
      ? positionals
      : fs
          .readdirSync(versionsDir)
          .filter((name) => name.endsWith(".version"))
          .map((name) => path.join(versionsDir, name));

  if (files.length === 0) {
    throw new Error(`no version files found in ${toRepoRelPath(versionsDir)}`);
  }

  const branch = detectBranch();
  const isRelease = releaseBranches.includes(branch);
  const commit = repo.revParse("HEAD");
  const sha8 = repo.revParse("HEAD", { short: 8 });

  const results = files.map((file) => computeForFile(file, isRelease, sha8));

  if (values.write) {
    const configPath = path.resolve(
      values.config ?? path.join(repoRoot, "config.json"),
    );
    writeConfig(configPath, results);
    // Status to stderr so `--write --json` keeps stdout clean for the JSON.
    console.error(`Wrote computed versions to ${toRepoRelPath(configPath)}:`);
    for (const r of results) {
      const field = configFieldByPackageId[r.packageId];
      if (field) console.error(`  ${field} = ${r.version}`);
    }
  }

  if (values.json) {
    console.log(
      JSON.stringify({ branch, isRelease, commit, packages: results }, null, 2),
    );
  } else {
    console.log(`branch: ${branch} (${isRelease ? "release" : "prerelease"})`);
    console.log(`commit: ${commit}`);
    for (const r of results) {
      console.log(
        `  ${r.packageId}: ${r.version}  (floor ${r.floor} + height ${r.height})`,
      );
    }
  }
}

main();
