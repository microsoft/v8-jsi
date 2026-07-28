// Generate version_gen.rc from version.rc template and config.json
// Usage: node generate_version_rc.ts [--config <path>] [--template <path>] [--output <path>]

import { readFileSync, writeFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { parseArgs } from "node:util";

const scriptDir = dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Z]:)/i, "$1"));

const { values } = parseArgs({
  options: {
    config: { type: "string", default: join(scriptDir, "..", "config.json") },
    template: { type: "string", default: join(scriptDir, "version.rc") },
    output: { type: "string", default: join(scriptDir, "version_gen.rc") },
  },
});

const config = JSON.parse(readFileSync(values.config!, "utf8"));
// A Windows binary version is numeric major.minor.build. Drop any prerelease
// suffix (e.g. 0.79.8-g0564e49f) before splitting.
const [major, minor, build] = (config.version as string).split("-")[0].split(".");

const template = readFileSync(values.template!, "utf8");
const result = template
  .replaceAll("V8JSIVER_MAJOR", major ?? "0")
  .replaceAll("V8JSIVER_MINOR", minor ?? "0")
  .replaceAll("V8JSIVER_BUILD", build ?? "0")
  .replaceAll("V8JSIVER_V8REF", "0");

writeFileSync(values.output!, result);
console.log(`Generated ${values.output} (version ${major}.${minor}.${build})`);
