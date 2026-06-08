// Copy Node-API conformance test JS files into the build output directory,
// preserving the source-target relative path under <dest-root>/.
// Mirrors the BUILD.gn copy("node_api_test_js_files") rule but driven from
// GYP via an action.
//
// Usage:
//   node copy_test_js_files.ts --source-base <dir> --dest-root <dir> [--stamp <file>]
//
// All *.js files under <source-base> (recursive) are copied to
// <dest-root>/<rel-path>. No manifest — the file list is derived from the
// directory tree at action-execution time.
//
// Change-detection caveat: gyp's `inputs:` for the action only lists this
// script. Adding a new .js file does NOT change the script's mtime, so an
// incremental build won't re-run the copy on its own. To pick up newly-added
// .js files, do a clean build (or touch this script).

import {
  copyFileSync,
  mkdirSync,
  readdirSync,
  writeFileSync,
} from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { parseArgs } from "node:util";

// On MSVS, `<(PRODUCT_DIR)` expands to `$(OutDir)` which ends with a backslash;
// when emitted as `--dest-root "$(OutDir)"` in the project file, the trailing
// backslash escapes the closing quote and the argument arrives with a trailing
// `"` baked in. Strip it before resolving.
function stripStrayQuote(s: string): string {
  return s.endsWith('"') ? s.slice(0, -1) : s;
}

const { values } = parseArgs({
  options: {
    "source-base": { type: "string" },
    "dest-root": { type: "string" },
    stamp: { type: "string" },
  },
});

if (!values["source-base"] || !values["dest-root"]) {
  console.error(
    "Usage: node copy_test_js_files.ts --source-base <dir> --dest-root <dir> [--stamp <file>]"
  );
  process.exit(1);
}

const sourceBase = resolve(stripStrayQuote(values["source-base"]));
const destRoot = resolve(stripStrayQuote(values["dest-root"]));
const stampPath = values.stamp ? resolve(stripStrayQuote(values.stamp)) : null;

function* walkJsFiles(dir: string): Generator<string> {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const fullPath = join(dir, entry.name);
    if (entry.isDirectory()) {
      yield* walkJsFiles(fullPath);
    } else if (entry.isFile() && entry.name.endsWith(".js")) {
      yield fullPath;
    }
  }
}

mkdirSync(destRoot, { recursive: true });

let copied = 0;
for (const src of walkJsFiles(sourceBase)) {
  const rel = relative(sourceBase, src);
  const dest = join(destRoot, rel);
  mkdirSync(dirname(dest), { recursive: true });
  copyFileSync(src, dest);
  copied++;
}

if (stampPath) {
  mkdirSync(dirname(stampPath), { recursive: true });
  writeFileSync(stampPath, `${copied} files copied at ${new Date().toISOString()}\n`);
}

console.log(`Copied ${copied} test JS files to ${destRoot}`);
