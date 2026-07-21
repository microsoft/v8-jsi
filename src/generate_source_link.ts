// Generate src/source_link_gen_gyp.json for the **new** Microsoft.JavaScript.V8
// build (v8jsi.dll / v8jsisb.dll). SourceLink embeds, inside the PDB, a mapping
// from the local build-time source paths to their raw GitHub URLs at the built
// commit, so a debugger (WinDbg / Visual Studio) can fetch the exact sources
// without them on disk.
//
// This must be embedded at LINK time: the PublishSymbols@2 task cannot srcsrv-
// index GitHub sources ("Unsupported source provider 'GitHub'"), so the mapping
// is fed to the linker via /SOURCELINK: (wired in src/v8jsi.gyp). This mirrors
// what scripts/fetch_code.ps1 does for the legacy Google-V8 build
// (src/source_link.json -> src/source_link_gen.json), but for the new build the
// whole repo root maps to microsoft/v8-jsi/<commit> — that single rule covers
// both the v8-jsi sources (src/) and the vendored V8/Node sources (deps/nodejs/).
// A distinct filename (…_gyp) avoids clobbering the legacy build's map.
//
// Usage: node generate_source_link.ts [--output <path>] [--repo-root <path>] [--commit <sha>]

import { execSync } from "node:child_process";
import { writeFileSync } from "node:fs";
import { join, dirname, resolve } from "node:path";
import { parseArgs } from "node:util";

const scriptDir = dirname(
  new URL(import.meta.url).pathname.replace(/^\/([A-Z]:)/i, "$1"),
);
const defaultRepoRoot = resolve(scriptDir, "..");

const { values } = parseArgs({
  options: {
    output: {
      type: "string",
      default: join(scriptDir, "source_link_gen_gyp.json"),
    },
    "repo-root": { type: "string", default: defaultRepoRoot },
    commit: { type: "string" },
  },
});

const repoRoot = resolve(values["repo-root"]!);

let commit = values.commit;
if (!commit) {
  try {
    commit = execSync("git rev-parse HEAD", {
      cwd: repoRoot,
      encoding: "utf8",
    }).trim();
  } catch {
    commit = "HEAD";
    console.log("Warning: could not resolve git commit; using 'HEAD'.");
  }
}

// SourceLink document map: a local path glob -> a raw GitHub URL glob. The local
// side uses the OS path separator (backslashes on Windows) with a trailing `\*`;
// the URL side uses `/` with a trailing `/*`. JSON.stringify escapes the
// backslashes. Anything under the repo root that also exists on GitHub at the
// built commit resolves; generated/out-of-repo files simply won't (harmless).
const document: Record<string, string> = {
  [`${repoRoot}\\*`]: `https://raw.githubusercontent.com/microsoft/v8-jsi/${commit}/*`,
};

writeFileSync(values.output!, JSON.stringify({ documents: document }, null, 2) + "\n");
console.log(`Generated ${values.output} (commit ${commit}, root ${repoRoot}).`);
