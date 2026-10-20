#!/usr/bin/env node
// The Stela CLI. Run with Node.js 24+: node tools/stela/src/stela.ts <command>.
// Exit codes: 0 success; 1 check failures; 2 usage or tool errors.

import { existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import path from "node:path";
import { parseArgs } from "node:util";
import { type LibraryConfig, type StelaConfig, loadConfig } from "./config.ts";
import {
  diffModels,
  experimentalHeaders,
  formatDiff,
  libraryModel,
  loadBaselines,
  renderType,
  summarizeDiff,
} from "./diff.ts";
import {
  type EmbeddedBinary,
  type Manifest,
  buildManifest,
  embedFiles,
  isBinaryFile,
  manifestModel,
  narrowModel,
  readEmbedded,
  readModelOrBinary,
  verifyExports,
} from "./embed.ts";
import { StelaError } from "./errors.ts";
import { extractModel } from "./extract.ts";
import { GENERATORS, generate } from "./generators.ts";
import { formatFindings, lintModel } from "./lint.ts";
import { type Declaration, type Model, defaultModelPath, formatMismatch, readModel, serializeModel } from "./model.ts";
import { generateLibraryHeader, normalizeText, parseTemplate } from "./vendor.ts";

export const DEFAULT_TEMPLATE = path.join(import.meta.dirname, "..", "include", "stela.h");

const USAGE = `usage: node stela.ts <command> [options]

commands:
  vendor   generate each library's <lib>_stela.h from the stela.h template
             --config <file>    default: stela.config.json
             --template <file>  default: tools/stela/include/stela.h
             --check            report out-of-date files instead of writing
  extract  extract every library for every target and merge stela.model/1
             --config <file>    default: stela.config.json
             --out <file>       default: out/stela/<config name>.model.json
  lint     check a stela.model/1 against the C ABI conventions
             --config <file>    default: stela.config.json
             --model <file>     default: the extract default for --config
             --format <format>  text (default) or json
             --report-only      report errors without failing
  gen      <generator> --out <dir>   generators: ${GENERATORS.join(", ")}
             --model <file>     default: the extract default for --config
             --config <file>    default: stela.config.json
           embed --binary <name> --target <triple>... --out <dir>
                                write <name>.stela.gz, the binary's manifest,
                                and <name>.stela.rc, which links it
             --extract          extract the binary's libraries instead of
                                reading --model
             --stamp <file>     also write <file> on every run, for builds
                                that rerun until an output is new
  diff     classify the changes between two models as breaks, additions, notes
             --config <file>    default: stela.config.json; not read when
                                both --base and --head are given
             --base <file>      default: the config's diff.baselines
             --head <file>      default: the extract default for --config
                                --base and --head also take binaries; the
                                other side is cut to a binary's libraries
                                and targets
             --format <format>  text (default) or json
  baseline write each configured diff baseline from the current model
             [<library>...]     default: every library in diff.baselines
             --config <file>    default: stela.config.json
             --model <file>     default: the extract default for --config
             --out <dir>        write <dir>/<library>.model.json instead, for
                                any library (default: every library)
  show     <binary>             print the API a binary's manifest describes
             --json             print the manifest itself
  verify   <binary>...          check each binary's exports against its manifest
  check    <binary>             diff headers (base) against a binary (head)
             --config <file>    extract the binary's libraries from a config
             --headers <header>...  or from these headers, matched by name
             --format <format>  text (default) or json

exit codes: 0 success; 1 check failures (stale files, target mismatches,
            ABI breaks, export mismatches); 2 usage or tool errors
`;

interface Io {
  out(line: string): void;
  err(line: string): void;
}

function display(root: string, file: string): string {
  return path.relative(root, file).split(path.sep).join("/");
}

function vendor(argv: string[], io: Io): number {
  const { values } = parseArgs({
    args: argv,
    options: {
      config: { type: "string", default: "stela.config.json" },
      template: { type: "string", default: DEFAULT_TEMPLATE },
      check: { type: "boolean", default: false },
    },
    strict: true,
    allowPositionals: false,
  });
  const config = loadConfig(values.config as string);
  let templateText: string;
  try {
    templateText = readFileSync(values.template as string, "utf8");
  } catch (e) {
    throw new StelaError(`cannot read template ${values.template}: ${(e as Error).message}`);
  }
  const template = parseTemplate(templateText);

  let stale = 0;
  for (const lib of config.libraries) {
    if (lib.stelaHeader === false) continue;
    const file = lib.stelaHeader;
    const text = generateLibraryHeader(template, {
      macroPrefix: lib.macroPrefix,
      fileName: path.basename(file),
      stelaNames: lib.stelaNames,
    });
    const existing = existsSync(file) ? readFileSync(file) : undefined;
    const shown = display(config.root, file);
    if (values.check) {
      // Compare text, so a CRLF checkout of an up-to-date file still passes.
      if (existing === undefined) {
        io.err(`missing:   ${shown}`);
        stale++;
      } else if (normalizeText(existing.toString("utf8")) !== text) {
        io.err(`stale:     ${shown}`);
        stale++;
      } else {
        io.out(`current:   ${shown}`);
      }
    } else if (existing !== undefined && existing.equals(Buffer.from(text, "utf8"))) {
      io.out(`unchanged: ${shown}`);
    } else {
      mkdirSync(path.dirname(file), { recursive: true });
      writeFileSync(file, text, "utf8");
      io.out(`wrote:     ${shown}`);
    }
  }
  if (stale > 0) {
    io.err(`${stale} generated file(s) are out of date; run \`stela vendor\``);
    return 1;
  }
  return 0;
}

/** Writes `content` only when the bytes differ; replaces an existing file atomically. */
function writeIfChanged(file: string, content: string | Buffer): "wrote" | "unchanged" {
  const bytes = typeof content === "string" ? Buffer.from(content, "utf8") : content;
  if (existsSync(file) && readFileSync(file).equals(bytes)) return "unchanged";
  mkdirSync(path.dirname(file), { recursive: true });
  const temp = `${file}.${process.pid}.tmp`;
  writeFileSync(temp, bytes);
  renameSync(temp, file);
  return "wrote";
}

async function extract(argv: string[], io: Io): Promise<number> {
  const { values } = parseArgs({
    args: argv,
    options: {
      config: { type: "string", default: "stela.config.json" },
      out: { type: "string" },
    },
    strict: true,
    allowPositionals: false,
  });
  const config = loadConfig(values.config as string);
  const out = path.resolve(values.out ?? defaultModelPath(config.file));
  const { model, mismatches } = await extractModel(config);
  if (mismatches.length > 0) {
    for (const m of mismatches) io.err(formatMismatch(m));
    io.err(`${mismatches.length} declaration(s) differ between targets; no model written`);
    return 1;
  }
  const status = writeIfChanged(out, serializeModel(model));
  const count = model.libraries.reduce((n, l) => n + l.declarations.length, 0);
  io.out(
    `${`${status}:`.padEnd(11)}${display(process.cwd(), out)} ` +
      `(${model.libraries.length} libraries, ${model.targets.length} targets, ${count} declarations)`,
  );
  return 0;
}

/** The model of `libraries` (default: all), extracted in memory; mismatches fail. */
async function extractInMemory(config: StelaConfig, io: Io, libraries?: readonly string[]): Promise<Model | undefined> {
  const selected =
    libraries === undefined ? config : { ...config, libraries: config.libraries.filter((l) => libraries.includes(l.name)) };
  const { model, mismatches } = await extractModel(selected);
  if (mismatches.length === 0) return model;
  for (const m of mismatches) io.err(formatMismatch(m));
  io.err(`${mismatches.length} declaration(s) differ between targets`);
  return undefined;
}

async function gen(argv: string[], io: Io): Promise<number> {
  const { values, positionals } = parseArgs({
    args: argv,
    options: {
      model: { type: "string" },
      config: { type: "string", default: "stela.config.json" },
      out: { type: "string" },
      binary: { type: "string" },
      target: { type: "string", multiple: true },
      extract: { type: "boolean", default: false },
      stamp: { type: "string" },
    },
    strict: true,
    allowPositionals: true,
  });
  const generators = [...GENERATORS, "embed"];
  if (positionals.length !== 1) throw new StelaError(`gen takes one generator: ${generators.join(", ")}`);
  if (values.out === undefined) throw new StelaError("gen needs --out <dir>");
  const outDir = path.resolve(values.out);
  const write = (relative: string, content: string | Buffer) => {
    const target = path.join(outDir, ...relative.split("/"));
    const status = writeIfChanged(target, content);
    io.out(`${`${status}:`.padEnd(11)}${display(process.cwd(), target)}`);
  };

  if (positionals[0] !== "embed") {
    if (values.binary !== undefined || values.target !== undefined || values.extract || values.stamp !== undefined) {
      throw new StelaError("--binary, --target, --extract, and --stamp are options of `gen embed`");
    }
    const modelFile = values.model ?? defaultModelPath(values.config as string);
    for (const file of generate(positionals[0], readModel(modelFile))) write(file.path, file.text);
    return 0;
  }

  if (values.binary === undefined) throw new StelaError("gen embed needs --binary <name>");
  if (values.target === undefined) throw new StelaError("gen embed needs --target <triple>");
  if (values.extract && values.model !== undefined) throw new StelaError("give --model or --extract, not both");
  const config = loadConfig(values.config as string);
  const name = values.binary;
  const binary = config.binaries.find((b) => b.name.toLowerCase() === name.toLowerCase());
  if (binary === undefined) throw new StelaError(`${config.file}: no binary "${name}" in "binaries"`);
  // The model hash covers every target, so even one binary target extracts them all.
  const model = values.extract
    ? await extractInMemory(config, io, binary.libraries)
    : readModel(path.resolve(values.model ?? defaultModelPath(config.file)));
  if (model === undefined) return 1;
  for (const file of embedFiles(buildManifest(model, binary, values.target))) write(file.path, file.bytes);
  if (values.stamp !== undefined) {
    // Unchanged outputs keep their times, so a build that would rerun the
    // command until they're newer than its inputs names this file instead.
    const stamp = path.resolve(values.stamp);
    mkdirSync(path.dirname(stamp), { recursive: true });
    writeFileSync(stamp, "");
    io.out(`${"stamped:".padEnd(11)}${display(process.cwd(), stamp)}`);
  }
  return 0;
}

function lint(argv: string[], io: Io): number {
  const { values } = parseArgs({
    args: argv,
    options: {
      model: { type: "string" },
      config: { type: "string", default: "stela.config.json" },
      format: { type: "string", default: "text" },
      "report-only": { type: "boolean", default: false },
    },
    strict: true,
    allowPositionals: false,
  });
  if (values.format !== "text" && values.format !== "json") {
    throw new StelaError('lint --format must be "text" or "json"');
  }
  const config = loadConfig(values.config as string);
  const modelFile = path.resolve(values.model ?? defaultModelPath(config.file));
  const findings = lintModel(readModel(modelFile), config);
  const text = formatFindings(findings, values.format);
  if (text !== "") io.out(text);
  if (values.format === "text") {
    const counts = { error: 0, warning: 0, info: 0 };
    for (const finding of findings) counts[finding.severity]++;
    io.out(
      `${findings.length} finding(s): ${counts.error} error(s), ` +
        `${counts.warning} warning(s), ${counts.info} info`,
    );
  }
  return values["report-only"] || !findings.some((f) => f.severity === "error") ? 0 : 1;
}

function diff(argv: string[], io: Io): number {
  const { values } = parseArgs({
    args: argv,
    options: {
      config: { type: "string" },
      base: { type: "string" },
      head: { type: "string" },
      format: { type: "string", default: "text" },
    },
    strict: true,
    allowPositionals: false,
  });
  if (values.format !== "text" && values.format !== "json") {
    throw new StelaError('diff --format must be "text" or "json"');
  }
  const needsConfig = values.config !== undefined || values.base === undefined || values.head === undefined;
  const config: StelaConfig | undefined = needsConfig ? loadConfig(values.config ?? "stela.config.json") : undefined;
  const headFile = path.resolve(values.head ?? defaultModelPath(config!.file));
  let base = values.base === undefined ? loadBaselines(config!) : readModelOrBinary(values.base);
  let head = readModelOrBinary(headFile);
  // A manifest covers only its binary's libraries and targets, so the other
  // side is narrowed to match; two binaries compare what they share.
  const baseBinary = values.base !== undefined && isBinaryFile(values.base);
  const headBinary = isBinaryFile(headFile);
  const names = (m: Model) => m.libraries.map((l) => l.name);
  if (baseBinary && headBinary) {
    const shared = names(base).filter((n) => names(head).includes(n));
    const targets = base.targets.filter((t) => head.targets.includes(t));
    if (shared.length === 0) throw new StelaError("the two binaries share no library");
    if (targets.length === 0) throw new StelaError("the two binaries share no target");
    base = narrowModel(base, shared, targets);
    head = narrowModel(head, shared, targets);
  } else if (headBinary) {
    base = narrowModel(base, names(head), head.targets);
  } else if (baseBinary) {
    head = narrowModel(head, names(base), base.targets);
  }
  const findings = diffModels(base, head, {
    experimentalHeaders: config === undefined ? [] : experimentalHeaders(config),
    // Against configured baselines, libraries without one aren't compared.
    libraries: values.base === undefined ? Object.keys(config!.diff.baselines) : undefined,
  });
  const text = formatDiff(findings, values.format);
  if (text !== "") io.out(text);
  if (values.format === "text") io.out(summarizeDiff(findings));
  return findings.some((f) => f.class === "break") ? 1 : 0;
}

function baseline(argv: string[], io: Io): number {
  const { values, positionals } = parseArgs({
    args: argv,
    options: {
      config: { type: "string", default: "stela.config.json" },
      model: { type: "string" },
      out: { type: "string" },
    },
    strict: true,
    allowPositionals: true,
  });
  const config = loadConfig(values.config as string);
  // With --out, any configured library is written to <dir>/<library>.model.json
  // (e.g. to ship with the headers); without it, the diff baselines are.
  const configured = values.out === undefined ? Object.keys(config.diff.baselines) : config.libraries.map((l) => l.name);
  if (configured.length === 0) throw new StelaError(`${config.file}: no "diff.baselines" are configured`);
  for (const name of positionals) {
    if (!configured.includes(name)) {
      throw new StelaError(values.out === undefined ? `library "${name}" has no entry in "diff.baselines"` : `unknown library "${name}"`);
    }
  }
  const model = readModel(path.resolve(values.model ?? defaultModelPath(config.file)));
  const names = positionals.length === 0 ? configured : configured.filter((n) => positionals.includes(n));
  for (const name of names) {
    const file = values.out === undefined ? config.diff.baselines[name] : path.join(values.out, `${name}.model.json`);
    const status = writeIfChanged(file, serializeModel(libraryModel(model, name)));
    io.out(`${`${status}:`.padEnd(11)}${display(process.cwd(), file)}`);
  }
  return 0;
}

function renderDeclaration(d: Declaration, target: string): string {
  const facts = d.perTarget?.[target];
  switch (d.kind) {
    case "function": {
      const params = (d.params ?? []).map((p) => `${renderType(p.type)} ${p.name}`.trimEnd());
      if (d.variadic) params.push("...");
      return `${renderType(d.result?.type)} ${d.name}(${params.length === 0 ? "void" : params.join(", ")})`;
    }
    case "typedef":
      return `typedef ${renderType(d.type)} ${d.name}`;
    case "struct":
    case "union": {
      if (d.incomplete) return `${d.kind} ${d.name} (incomplete)`;
      const size = facts?.size === undefined ? "" : `, ${facts.size} bytes`;
      return `${d.kind} ${d.name} {${d.fields?.length ?? 0} field(s)${size}}`;
    }
    case "enum":
      return `enum ${d.name} {${d.enumerators?.length ?? 0} enumerator(s)}`;
    case "macro":
      return `#define ${d.name}${d.functionLike ? "(...)" : ""} ${facts?.value ?? (d.replacement ?? []).join(" ")}`.trimEnd();
  }
}

function showManifest(binary: EmbeddedBinary, io: Io): void {
  const { manifest } = binary;
  io.out(`${path.basename(binary.file)}: ${manifest.binary} for ${manifest.targets.join(", ")} (machine 0x${binary.machine.toString(16)})`);
  io.out(`  ${manifest.schema} from stela ${manifest.producer.stela}, libclang ${manifest.producer.libclang}`);
  io.out(`  other exports: ${manifest.otherExports ? "allowed" : "none"}`);
  for (const library of manifest.libraries) {
    const exported = new Set(library.exports);
    const functions = library.declarations.filter((d) => d.kind === "function").length;
    io.out("");
    io.out(`library ${library.name}: prefix ${library.prefix}_, status ${library.status}, ${exported.size} of ${functions} function(s) exported`);
    io.out(`  headers: ${library.headers.join(", ")}`);
    if (library.targets !== undefined) io.out(`  targets: ${library.targets.join(", ")}`);
    const versions = Object.entries(library.abiVersions).map(([name, value]) => `${name} = ${value}`);
    io.out(`  ABI versions: ${versions.length === 0 ? "(none)" : versions.join(", ")}`);
    io.out(`  model sha256: ${library.modelSha256}`);
    for (const d of library.declarations) {
      const marks = [exported.has(d.name) && d.kind === "function" ? "exported" : "", d.targets ? `only ${d.targets.join(", ")}` : ""];
      const suffix = marks.filter(Boolean).join("; ");
      io.out(`  ${renderDeclaration(d, binary.target)}${suffix ? `  [${suffix}]` : ""}`);
    }
  }
}

function show(argv: string[], io: Io): number {
  const { values, positionals } = parseArgs({
    args: argv,
    options: { json: { type: "boolean", default: false } },
    strict: true,
    allowPositionals: true,
  });
  if (positionals.length !== 1) throw new StelaError("show takes one binary");
  const binary = readEmbedded(path.resolve(positionals[0]));
  if (values.json) io.out(JSON.stringify(binary.manifest, null, 2));
  else showManifest(binary, io);
  return 0;
}

function verify(argv: string[], io: Io): number {
  const { positionals } = parseArgs({ args: argv, options: {}, strict: true, allowPositionals: true });
  if (positionals.length === 0) throw new StelaError("verify takes one or more binaries");
  let failed = 0;
  for (const file of positionals) {
    const binary = readEmbedded(path.resolve(file));
    const { missing, unexpected } = verifyExports(binary);
    const name = path.basename(file);
    const libraries = binary.manifest.libraries.map((l) => l.name).join(", ");
    if (missing.length === 0 && unexpected.length === 0) {
      const listed = binary.manifest.libraries.reduce((n, l) => n + l.exports.length, 0);
      const others = binary.exports.length - listed;
      io.out(
        `OK   ${name}: ${listed} export(s) match the manifest of ${libraries}` +
          (others > 0 ? `; ${others} other export(s) allowed` : ""),
      );
    } else {
      failed++;
      io.err(`FAIL ${name}: exports differ from the manifest of ${libraries}`);
      if (missing.length > 0) io.err(`  missing: ${missing.join(", ")}`);
      if (unexpected.length > 0) io.err(`  unexpected: ${unexpected.join(", ")}`);
    }
  }
  return failed === 0 ? 0 : 1;
}

/** `config` narrowed to a binary's libraries and targets. */
function configForManifest(config: StelaConfig, manifest: Manifest): StelaConfig {
  for (const target of manifest.targets) {
    if (!config.targets.includes(target)) throw new StelaError(`${config.file}: no target ${target}, which ${manifest.binary} is built for`);
  }
  const libraries = manifest.libraries.map((embedded): LibraryConfig => {
    const library = config.libraries.find((l) => l.name === embedded.name);
    if (library === undefined) throw new StelaError(`${config.file}: no library "${embedded.name}", which ${manifest.binary} implements`);
    const targets = library.targets.filter((t) => manifest.targets.includes(t));
    if (targets.length === 0) {
      throw new StelaError(`${config.file}: library "${library.name}" isn't built for ${manifest.targets.join(", ")}`);
    }
    return { ...library, targets };
  });
  return { ...config, targets: config.targets.filter((t) => manifest.targets.includes(t)), libraries };
}

function commonDirectory(paths: readonly string[]): string {
  const split = paths.map((p) => path.resolve(p).split(path.sep));
  const first = split[0];
  let length = first.length;
  for (const parts of split.slice(1)) {
    let i = 0;
    while (i < Math.min(length, parts.length) && parts[i].toLowerCase() === first[i].toLowerCase()) i++;
    length = i;
  }
  if (length === 0) throw new StelaError("the headers share no common directory");
  return first.slice(0, length).join(path.sep) || path.sep;
}

/**
 * A config for headers given on the command line: each header is matched to
 * the binary's library by its include spelling (the path below the first
 * include directory that holds it), else by file name; the directory above
 * that spelling becomes an include directory.
 */
function configFromHeaders(headers: readonly string[], manifest: Manifest): StelaConfig {
  const found = new Map<string, { headers: string[]; includeDirs: string[] }>();
  for (const header of headers) {
    const file = path.resolve(header);
    if (!existsSync(file)) throw new StelaError(`cannot read ${header}`);
    const slashed = file.split(path.sep).join("/").toLowerCase();
    let match: { library: string; includeDir: string } | undefined;
    for (const library of manifest.libraries) {
      for (const own of library.headers) {
        const dir = library.includeDirs.find((d) => own.startsWith(`${d}/`));
        const spelling = (dir === undefined ? path.posix.basename(own) : own.slice(dir.length + 1)).toLowerCase();
        if (slashed.endsWith(`/${spelling}`)) {
          match ??= { library: library.name, includeDir: file.slice(0, file.length - spelling.length - 1) };
        } else if (path.posix.basename(own).toLowerCase() === path.basename(file).toLowerCase()) {
          match ??= { library: library.name, includeDir: path.dirname(file) };
        }
      }
    }
    if (match === undefined) throw new StelaError(`${header} isn't a header of any library ${manifest.binary} implements`);
    let entry = found.get(match.library);
    if (entry === undefined) found.set(match.library, (entry = { headers: [], includeDirs: [] }));
    entry.headers.push(file);
    for (const dir of [match.includeDir, path.dirname(file)]) if (!entry.includeDirs.includes(dir)) entry.includeDirs.push(dir);
  }
  const all = [...found.values()].flatMap((e) => [...e.headers, ...e.includeDirs]);
  // The root holds every include directory, so it sits above their common one.
  const root = path.dirname(commonDirectory(all));
  // A library's headers may include another's, so each sees every directory.
  const includeDirs = [...new Set([...found.values()].flatMap((e) => e.includeDirs))];
  const libraries = manifest.libraries.flatMap((library): LibraryConfig[] => {
    const entry = found.get(library.name);
    if (entry === undefined) return [];
    return [
      {
        name: library.name,
        prefix: library.prefix,
        macroPrefix: library.macroPrefix,
        headers: entry.headers,
        stelaHeader: false,
        stelaNames: {},
        status: library.status,
        includeDirs,
        owns: entry.headers,
        defines: library.defines,
        targets: library.targets ?? manifest.targets,
      },
    ];
  });
  return {
    file: path.join(root, "(headers)"),
    root,
    targets: manifest.targets,
    libraries,
    lint: { rules: {}, macroConstants: [] },
    diff: { baselines: {}, experimental: [] },
    binaries: [],
  };
}

async function check(argv: string[], io: Io): Promise<number> {
  const { values, positionals } = parseArgs({
    args: argv,
    options: {
      config: { type: "string" },
      headers: { type: "boolean", default: false },
      format: { type: "string", default: "text" },
    },
    strict: true,
    allowPositionals: true,
  });
  if (values.format !== "text" && values.format !== "json") throw new StelaError('check --format must be "text" or "json"');
  if ((values.config === undefined) === !values.headers) {
    throw new StelaError("check needs either --config <file> or --headers <header>...");
  }
  if (values.headers ? positionals.length < 2 : positionals.length !== 1) {
    throw new StelaError(values.headers ? "check --headers takes a binary and its headers" : "check takes one binary");
  }
  const binary = readEmbedded(path.resolve(positionals[0]));
  const loaded = values.config === undefined ? undefined : loadConfig(values.config);
  const config = loaded ? configForManifest(loaded, binary.manifest) : configFromHeaders(positionals.slice(1), binary.manifest);
  const base = await extractInMemory(config, io);
  if (base === undefined) return 1;
  const findings = diffModels(base, manifestModel(binary.manifest), {
    experimentalHeaders: loaded ? experimentalHeaders(loaded) : [],
    libraries: config.libraries.map((l) => l.name),
  });
  const text = formatDiff(findings, values.format);
  if (text !== "") io.out(text);
  if (values.format === "text") io.out(summarizeDiff(findings));
  return findings.some((f) => f.class === "break") ? 1 : 0;
}

export async function main(argv: string[], io: Io): Promise<number> {
  const [command, ...rest] = argv;
  try {
    switch (command) {
      case "vendor":
        return vendor(rest, io);
      case "extract":
        return await extract(rest, io);
      case "lint":
        return lint(rest, io);
      case "gen":
        return await gen(rest, io);
      case "diff":
        return diff(rest, io);
      case "baseline":
        return baseline(rest, io);
      case "show":
        return show(rest, io);
      case "verify":
        return verify(rest, io);
      case "check":
        return await check(rest, io);
      case undefined:
      case "help":
      case "--help":
      case "-h":
        io.out(USAGE);
        return command === undefined ? 2 : 0;
      default:
        io.err(`unknown command "${command}"\n${USAGE}`);
        return 2;
    }
  } catch (e) {
    if (e instanceof StelaError || (e as { code?: string }).code?.startsWith("ERR_PARSE_ARGS")) {
      io.err(`stela: ${(e as Error).message}`);
      return 2;
    }
    throw e;
  }
}

if (import.meta.main) {
  process.exitCode = await main(process.argv.slice(2), {
    out: (line) => process.stdout.write(line + "\n"),
    err: (line) => process.stderr.write(line + "\n"),
  });
}
