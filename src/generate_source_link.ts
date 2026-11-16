// Generate the GitHub Source Link map for the Windows PDBs of v8jsi.dll and
// v8jsisb.dll (src/v8jsi.gyp), mkv8snapshot.exe, libnode.dll (src/libnode.gyp),
// and sbox.exe and v8host.dll (scripts/sbox-build.ts). The linker embeds it in
// each PDB as the "sourcelink" stream (lld-link /pdbstream:sourcelink=), and a
// debugger (Visual Studio, WinDbg) uses it to fetch each source from
// raw.githubusercontent.com/microsoft/v8-jsi/<commit>/ at the built commit.
//
// One rule maps the repository root, which covers the v8-jsi sources and the
// vendored Node.js and Chromium trees. Files generated under out\ have no
// GitHub copy. The debugger accepts a fetched file only if its checksum matches
// the one in the PDB, so the build must compile the bytes Git stores (LF line
// endings, .gitattributes). scripts/source-link-check.ts checks the result.
//
// PublishSymbols@2 can't source-index GitHub ("Unsupported source provider
// 'GitHub'"), so the map must be embedded at link time. The default output name
// (..._gyp) avoids clobbering the legacy Google-V8 build's source_link_gen.json.
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
