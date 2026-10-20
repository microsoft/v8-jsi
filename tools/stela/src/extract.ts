// `stela extract` (design §8.1): runs the extractor once per library and
// target, then merges the raw models into one stela.model/1.

import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import type { StelaConfig } from "./config.ts";
import { StelaError } from "./errors.ts";
import { runExtractor } from "./extractor.ts";
import { type Mismatch, type Model, type RawModel, buildModel } from "./model.ts";

export async function extractModel(
  config: StelaConfig,
): Promise<{ model: Model; mismatches: Mismatch[] }> {
  if (config.targets.length === 0) {
    throw new StelaError(`${config.file}: "targets" is required to extract a model`);
  }
  const jobs = config.libraries.flatMap((library) =>
    library.targets.map((target) => ({ library, target })),
  );
  const dir = mkdtempSync(path.join(os.tmpdir(), "stela-extract-"));
  try {
    const raws = new Map<string, Map<string, RawModel>>();
    const errors: StelaError[] = [];
    let next = 0;
    const worker = async () => {
      while (next < jobs.length && errors.length === 0) {
        const { library, target } = jobs[next++];
        const out = path.join(dir, `${library.name}.${target}.json`);
        try {
          await runExtractor({
            target,
            root: config.root,
            out,
            headers: library.headers,
            includes: library.includeDirs,
            defines: library.defines,
            owns: library.owns,
          });
        } catch (e) {
          const err = e as { stderr?: string; message: string };
          const detail = err.stderr?.trim() || err.message;
          errors.push(new StelaError(`extracting ${library.name} for ${target} failed:\n${detail}`));
          return;
        }
        const raw = JSON.parse(readFileSync(out, "utf8")) as RawModel;
        let byTarget = raws.get(library.name);
        if (!byTarget) raws.set(library.name, (byTarget = new Map()));
        byTarget.set(target, raw);
      }
    };
    const width = Math.max(1, Math.min(jobs.length, os.availableParallelism()));
    // Every worker settles before the temporary directory is removed.
    await Promise.all(Array.from({ length: width }, worker));
    if (errors.length > 0) throw errors[0];
    return buildModel(config, raws);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}
