// footprint.ts — footprint orchestrator (measurement-only; not shipped).
//
// Runs the sandbox-vs-WebView2 RAM/startup matrix and emits a results JSON +
// a markdown summary. No product or build code is touched — it drives the
// measurement binaries built beside it:
//   * footprint_broker.exe (in out/sandbox-x64) spawns the unmodified v8host.exe under
//     the UNMODIFIED sbox.dll and HOLDS it at post-lockdown steady state.
//   * wv2host.exe creates N real WebView2 controls running the same JS workload.
//   * memprobe.exe samples per-process memory (WS private/shared, commit,
//     reserved-vs-committed, the V8 cage reserve).
//
// Run (PowerShell, from tools/footprint):  node footprint.ts [--quick]
//
// CI regression gate (after a measurement run produced footprint-results.json):
//   node footprint.ts --check
// compares the latest results against the checked-in footprint-baseline.json —
// per-case regression (> +15%), absolute per-instance budgets (hard floor on a
// fixed agent SKU), and the WebView2 ratio (informational). Exits non-zero on
// any regression or budget breach.
//
// Method notes are baked into the output. All sizes in bytes internally; the
// markdown prints MB. conhost.exe (a console-redirect artifact) is excluded.

import { spawn, spawnSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as os from 'node:os';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(HERE, '..', '..');
const SANDBOX_OUT = join(REPO_ROOT, 'deps', 'chromium', 'out', 'sandbox-x64');
const BROKER = join(SANDBOX_OUT, 'footprint_broker.exe');
const V8HOST = join(SANDBOX_OUT, 'v8host.exe');
const WV2HOST = join(HERE, 'wv2host.exe');
const MEMPROBE = join(HERE, 'memprobe.exe');
const TMP = process.env.TEMP || os.tmpdir();

// Engine swapping: v8host.exe always LoadLibrary's "v8jsi.dll" by name, so a
// sandbox case runs whichever DLL is staged at SANDBOX_ENGINE. We swap between the
// shipped full-JIT v8jsi.dll (33 MB; v8jsi/Trusted cases) and the sandbox
// variant v8jsisb.dll (17 MB jitless+lite; the production Untrusted engine).
//
// IMPORTANT: the two prebuilt DLLs live in DIFFERENT build-output dirs, and the
// sandbox variant's clean rebuild (rm deps/nodejs/out — the stale-snapshot trap
// when toggling the lite flags) removed v8jsi.dll from deps/nodejs/out/Release,
// leaving only v8jsisb there;
// the full-JIT copy survives in out/size. Because a build-output dir can be wiped
// out from under us mid-run (which once left the sandbox output staged with the
// wrong DLL),
// we COPY both into a local, gitignored cache at startup and stage/restore from
// there — the restore can never depend on a build dir that may have vanished.
const ENGINE_PREBUILT: Record<string, string[]> = {
    // First existing path wins (full-JIT v8jsi.dll: out/size, then engine output).
    v8jsi: [join(SANDBOX_OUT, '..', 'size', 'v8jsi.dll'),
      join(REPO_ROOT, 'deps', 'nodejs', 'out', 'Release', 'v8jsi.dll')],
    v8jsisb: [join(REPO_ROOT, 'deps', 'nodejs', 'out', 'Release', 'v8jsisb.dll')],
};
const ENGINE_CACHE = join(HERE, 'engines');
const ENGINE_SRC: Record<string, string> = {
  v8jsi: join(ENGINE_CACHE, 'v8jsi.dll'),
  v8jsisb: join(ENGINE_CACHE, 'v8jsisb.dll'),
};
const SANDBOX_ENGINE = join(SANDBOX_OUT, 'v8jsi.dll');

const QUICK = process.argv.includes('--quick');
const CHECK = process.argv.includes('--check');
const ITERS = QUICK ? 1 : 3;
const RESULTS_JSON = join(HERE, 'footprint-results.json');
const BASELINE_JSON = join(HERE, 'footprint-baseline.json');
const HOLD_MS = 9000;          // hold window; we sample inside it then kill
const SAMPLE_DELAY_MS = 2500;  // settle time after "ready" before sampling

type Proc = {
  pid: number; name: string; opened: boolean;
  ws_total: number; ws_private: number; ws_shareable: number;
  priv_commit: number; pagefile_commit: number;
  reserved: number; committed: number; largest_reserve: number; regions: number;
};
type Sum = {
  count: number; ws_total: number; ws_private: number; ws_shareable: number;
  priv_commit: number; reserved: number; committed: number; largest_reserve: number;
  procs: Proc[];
};

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

function sampleByName(name: string): Proc[] {
  const r = spawnSync(MEMPROBE, ['--name', name], { encoding: 'utf8' });
  return parseProcs(r.stdout || '');
}
function sampleTree(pid: number): Proc[] {
  const r = spawnSync(MEMPROBE, ['--tree', String(pid)], { encoding: 'utf8' });
  return parseProcs(r.stdout || '');
}
function parseProcs(stdout: string): Proc[] {
  const out: Proc[] = [];
  for (const line of stdout.split(/\r?\n/)) {
    const s = line.trim();
    if (!s.startsWith('{') || s.includes('"summary"')) continue;
    try {
      const o = JSON.parse(s);
      if (typeof o.pid === 'number') out.push(o as Proc);
    } catch { /* ignore */ }
  }
  return out;
}

// Sum a set of processes, excluding the console-redirect artifact conhost.exe
// and de-duplicating by pid.
function summarize(procs: Proc[]): Sum {
  const seen = new Set<number>();
  const sum: Sum = {
    count: 0, ws_total: 0, ws_private: 0, ws_shareable: 0, priv_commit: 0,
    reserved: 0, committed: 0, largest_reserve: 0, procs: [],
  };
  for (const p of procs) {
    if (!p.opened) continue;
    if (/conhost\.exe/i.test(p.name)) continue;
    if (seen.has(p.pid)) continue;
    seen.add(p.pid);
    sum.count++;
    sum.ws_total += p.ws_total;
    sum.ws_private += p.ws_private;
    sum.ws_shareable += p.ws_shareable;
    sum.priv_commit += p.priv_commit;
    sum.reserved += p.reserved;
    sum.committed += p.committed;
    sum.largest_reserve = Math.max(sum.largest_reserve, p.largest_reserve);
    sum.procs.push(p);
  }
  return sum;
}

function killTree(pid: number) {
  spawnSync('taskkill', ['/PID', String(pid), '/T', '/F'], { encoding: 'utf8' });
}
function killByName(name: string) {
  spawnSync('taskkill', ['/IM', name, '/T', '/F'], { encoding: 'utf8' });
}

// Populate the local engine cache from the prebuilt build outputs (once). Errors
// loudly if a needed DLL can't be found — better than silently running stale.
function ensureEngineCache() {
  mkdirSync(ENGINE_CACHE, { recursive: true });
  for (const which of Object.keys(ENGINE_PREBUILT)) {
    const dst = ENGINE_SRC[which];
    if (existsSync(dst)) continue;  // already cached this session
    const src = ENGINE_PREBUILT[which].find((p) => existsSync(p) && statSync(p).size > 1_000_000);
    if (!src) throw new Error(`no prebuilt ${which}.dll found in: ${ENGINE_PREBUILT[which].join(', ')}`);
    copyFileSync(src, dst);
    console.log(`  cached ${which}.dll <- ${src} (${statSync(dst).size} B)`);
  }
}

// Stage the requested engine DLL at SANDBOX_ENGINE (must hold no v8host open). Retry
// the copy a few times in case a just-killed v8host hasn't released the file.
function stageEngine(which: 'v8jsi' | 'v8jsisb') {
  killByName('footprint_broker.exe');
  killByName('v8host.exe');
  const src = ENGINE_SRC[which];
  if (!existsSync(src)) throw new Error(`engine DLL missing: ${src}`);
  let lastErr: unknown;
  for (let i = 0; i < 10; i++) {
    try { copyFileSync(src, SANDBOX_ENGINE); return; }
    catch (e) { lastErr = e; spawnSync('cmd', ['/c', 'ping', '-n', '1', '127.0.0.1'], { encoding: 'utf8' }); }
  }
  throw new Error(`stageEngine(${which}) failed: ${(lastErr as Error)?.message}`);
}

// Spawn a process, watch stdout lines, resolve when `readyRe` matches; records
// the timestamp (ms from spawn) of each marker in `marks`.
function launchWatch(
  exe: string, args: string[], env: NodeJS.ProcessEnv,
  readyRe: RegExp, markRes: { key: string; re: RegExp }[],
): Promise<{ pid: number; marks: Record<string, number>; kill: () => void; lines: string[] }> {
  return new Promise((resolve, reject) => {
    const t0 = Date.now();
    const child = spawn(exe, args, { env, cwd: dirname(exe) });
    const marks: Record<string, number> = {};
    const lines: string[] = [];
    let done = false;
    const onLine = (line: string) => {
      lines.push(line);
      for (const m of markRes)
        if (marks[m.key] === undefined && m.re.test(line)) marks[m.key] = Date.now() - t0;
      if (!done && readyRe.test(line)) {
        done = true;
        marks['ready'] = Date.now() - t0;
        resolve({
          pid: child.pid!, marks,
          kill: () => { try { killTree(child.pid!); } catch {} },
          lines,
        });
      }
    };
    let buf = '';
    child.stdout.on('data', (d) => {
      buf += d.toString();
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        onLine(buf.slice(0, i).replace(/\r$/, ''));
        buf = buf.slice(i + 1);
      }
    });
    child.on('error', reject);
    child.on('exit', () => { if (!done) reject(new Error(`${exe} exited before ready`)); });
    setTimeout(() => { if (!done) reject(new Error(`${exe} ready timeout`)); }, 30000);
  });
}

// ---- Sandbox cases -------------------------------------------------------

async function runSandbox(
  engine: 'v8jsi' | 'v8jsisb', tier: 'untrusted' | 'trusted', n: number,
): Promise<CaseResult> {
  killByName('footprint_broker.exe');
  killByName('v8host.exe');
  await sleep(300);
  const env = { ...process.env, SBOX_FP_HOLD_MS: String(HOLD_MS) } as NodeJS.ProcessEnv;
  if (tier === 'trusted') env.SBOX_TIER = 'trusted'; else delete env.SBOX_TIER;
  // Real production topology: ONE broker (the test_app stand-in) spawns
  // N locked-down v8host targets — not N brokers. The process set is therefore
  // exactly 1 broker + N v8hosts, and the broker's fixed cost is counted once.
  const started = await launchWatch(
    BROKER, ['--count', String(n)], env, /\[fpbroker\] HELD/,
    [
      { key: 'runtime', re: /v8jsi runtime created/ },
      { key: 'jsdone', re: /user JS evaluated = OK/ },
    ]);
  await sleep(SAMPLE_DELAY_MS);
  // Sample by name — only OUR single broker + its N v8host children exist.
  const procs = [...sampleByName('footprint_broker.exe'), ...sampleByName('v8host.exe')];
  const sum = summarize(procs);
  // marginal (engine-only) = v8host processes only.
  const engineOnly = summarize(procs.filter((p) => /v8host\.exe/i.test(p.name)));
  const marks = started.marks;
  started.kill();
  killByName('footprint_broker.exe');
  killByName('v8host.exe');
  return { label: `sandbox-${engine}-${tier}`, n, full: sum, engineOnly, marks };
}

// ---- WebView2 cases ------------------------------------------------------

async function runWv2Shared(n: number): Promise<CaseResult> {
  const udf = join(TMP, `wv2_shared_${n}_${Date.now()}`);
  rmSync(udf, { recursive: true, force: true });
  const started = await launchWatch(
    WV2HOST, ['--controls', String(n), '--udf', udf, '--hold-ms', String(HOLD_MS)],
    process.env, /\[wv2host\] READY/,
    [{ key: 'firstdone', re: /control 1\/\d+ done/ }]);
  await sleep(SAMPLE_DELAY_MS);
  const sum = summarize(sampleTree(started.pid));
  started.kill();
  rmSync(udf, { recursive: true, force: true });
  return { label: 'wv2-shared', n, full: sum, engineOnly: sum, marks: started.marks };
}

async function runWv2Separate(n: number): Promise<CaseResult> {
  const hosts = [];
  const udfs: string[] = [];
  for (let i = 0; i < n; i++) {
    const udf = join(TMP, `wv2_sep_${n}_${i}_${Date.now()}`);
    udfs.push(udf);
    rmSync(udf, { recursive: true, force: true });
    hosts.push(launchWatch(
      WV2HOST, ['--controls', '1', '--udf', udf, '--hold-ms', String(HOLD_MS)],
      process.env, /\[wv2host\] READY/,
      [{ key: 'firstdone', re: /control 1\/1 done/ }]));
  }
  const started = await Promise.all(hosts);
  await sleep(SAMPLE_DELAY_MS);
  let all: Proc[] = [];
  for (const s of started) all = all.concat(sampleTree(s.pid));
  const sum = summarize(all);
  for (const s of started) s.kill();
  for (const u of udfs) rmSync(u, { recursive: true, force: true });
  return { label: 'wv2-separate', n, full: sum, engineOnly: sum, marks: started[0].marks };
}

type CaseResult = {
  label: string; n: number; full: Sum; engineOnly: Sum;
  marks: Record<string, number>;
};

// Run a case ITERS times, keep the median by ws_private.
async function repeat(fn: () => Promise<CaseResult>): Promise<CaseResult[]> {
  const runs: CaseResult[] = [];
  for (let i = 0; i < ITERS; i++) {
    try { runs.push(await fn()); } catch (e) { console.error('  run failed:', (e as Error).message); }
    await sleep(500);
  }
  return runs;
}
function median(runs: CaseResult[]): CaseResult {
  const ok = runs.filter(Boolean).sort((a, b) => a.full.ws_private - b.full.ws_private);
  return ok[Math.floor(ok.length / 2)];
}

const MB = (b: number) => +(b / (1024 * 1024)).toFixed(1);

// ---- CI regression gate (--check) ---------------------------------------
// Validate the latest footprint-results.json against the checked-in baseline.
// Three layers, mirroring the build --check gates:
//   1. per-case regression vs baseline (full priv WS / priv commit / startup),
//      fail on > +regressionThreshold;
//   2. absolute per-instance budgets at N=1 (hard floor on a fixed agent SKU);
//   3. WebView2 priv-WS ratio (informational — Edge updates move it, never fails).
function runCheck(): void {
  if (!existsSync(RESULTS_JSON))
    throw new Error(`no ${RESULTS_JSON} — run \`node footprint.ts --quick\` first`);
  if (!existsSync(BASELINE_JSON)) throw new Error(`no ${BASELINE_JSON}`);
  const data = JSON.parse(readFileSync(RESULTS_JSON, 'utf8'));
  const base = JSON.parse(readFileSync(BASELINE_JSON, 'utf8'));
  const results: Record<string, CaseResult[]> = data.results || {};
  const med = (key: string): CaseResult | undefined => median(results[key] || []);
  const failures: string[] = [];
  const thr: number = base.regressionThreshold ?? 0.15;

  console.log('=== Footprint regression vs baseline (> +' + (thr * 100).toFixed(0) + '% fails) ===');
  for (const [key, b] of Object.entries(base.cases) as [string, Record<string, number>][]) {
    const m = med(key);
    if (!m) { failures.push(`${key}: NO DATA in results`); console.log(`  [FAIL] ${key}: NO DATA`); continue; }
    const metrics: [string, number, number][] = [
      ['fullPrivWS_MB', MB(m.full.ws_private), b.fullPrivWS_MB],
      ['fullPrivCommit_MB', MB(m.full.priv_commit), b.fullPrivCommit_MB],
      ['ready_ms', m.marks.ready ?? Infinity, b.ready_ms],
    ];
    for (const [name, cur, baseVal] of metrics) {
      if (baseVal === undefined) continue;
      const limit = baseVal * (1 + thr);
      const ok = cur <= limit;
      if (!ok) failures.push(`${key} ${name}: ${cur} > ${baseVal} +${(thr * 100).toFixed(0)}% (${limit.toFixed(1)})`);
      console.log(`  [${ok ? 'OK' : 'FAIL'}] ${key} ${name}: ${cur} (baseline ${baseVal}, limit ${limit.toFixed(1)})`);
    }
  }

  console.log('\n=== Absolute per-instance budgets (N=1, hard floor) ===');
  const one = med('sandbox-v8jsisb-untrusted-1');
  const bud = base.absoluteBudgetsMB || {};
  if (!one) {
    failures.push('sandbox-v8jsisb-untrusted-1: NO DATA (absolute budgets unchecked)');
    console.log('  [FAIL] sandbox-v8jsisb-untrusted-1: NO DATA');
  } else {
    const checks: [string, number, number][] = [
      ['engine-only private WS', MB(one.engineOnly.ws_private), bud.engineOnlyPrivWS],
      ['engine-only private commit', MB(one.engineOnly.priv_commit), bud.engineOnlyPrivCommit],
      ['full private WS', MB(one.full.ws_private), bud.fullPrivWS],
      ['startup ms', one.marks.ready ?? Infinity, bud.startupMs],
    ];
    for (const [name, cur, ceil] of checks) {
      if (ceil === undefined) continue;
      const ok = cur <= ceil;
      if (!ok) failures.push(`budget ${name}: ${cur} > ${ceil}`);
      console.log(`  [${ok ? 'OK' : 'FAIL'}] ${name}: ${cur} (ceiling ${ceil})`);
    }
  }

  const wv = med('wv2-shared-1');
  if (one && wv) {
    const ratio = wv.full.ws_private / one.full.ws_private;
    const want = bud.wv2RatioMin ?? 5;
    console.log(`\n  [INFO] WebView2 private-WS ratio: ${ratio.toFixed(1)}x (target >= ${want}x, informational — never fails CI)`);
  }

  if (failures.length) {
    console.error('\nFootprint gate FAILED:\n  ' + failures.join('\n  '));
    process.exit(1);
  }
  console.log('\nFootprint gate PASS.');
}

async function main() {
  if (CHECK) { runCheck(); return; }
  ensureEngineCache();
  const Ns = QUICK ? [1, 2] : [1, 2, 4, 8];
  const results: Record<string, CaseResult[]> = {};
  // Each step names the engine to stage (sandbox cases) or null (WebView2).
  const plan: { key: string; engine: 'v8jsi' | 'v8jsisb' | null; fn: () => Promise<CaseResult> }[] = [];

  // Engine group 1 — current shipped full-JIT v8jsi.dll (33 MB): the Untrusted
  // tier runs it with the --jitless flag; the Trusted tier uses its JIT.
  for (const n of Ns)
    plan.push({ key: `sandbox-v8jsi-untrusted-${n}`, engine: 'v8jsi', fn: () => runSandbox('v8jsi', 'untrusted', n) });
  plan.push({ key: `sandbox-v8jsi-trusted-1`, engine: 'v8jsi', fn: () => runSandbox('v8jsi', 'trusted', 1) });

  // Engine group 2 — the sandbox variant v8jsisb.dll (17 MB jitless+lite): the
  // production Untrusted engine. (Build-time jitless => no real Trusted case.)
  for (const n of Ns)
    plan.push({ key: `sandbox-v8jsisb-untrusted-${n}`, engine: 'v8jsisb', fn: () => runSandbox('v8jsisb', 'untrusted', n) });

  // WebView2 (no engine staging).
  for (const n of Ns) plan.push({ key: `wv2-shared-${n}`, engine: null, fn: () => runWv2Shared(n) });
  for (const n of Ns) plan.push({ key: `wv2-separate-${n}`, engine: null, fn: () => runWv2Separate(n) });

  let staged: string | null = null;
  try {
    for (const step of plan) {
      if (step.engine && step.engine !== staged) {
        process.stdout.write(`-- staging ${step.engine}.dll as out/sandbox-x64/v8jsi.dll --\n`);
        stageEngine(step.engine);
        staged = step.engine;
      }
      process.stdout.write(`Running ${step.key} (x${ITERS}) ... `);
      const runs = await repeat(step.fn);
      results[step.key] = runs;
      const m = median(runs);
      if (m) console.log(`priv WS=${MB(m.full.ws_private)}MB commit=${MB(m.full.priv_commit)}MB ready=${m.marks.ready}ms`);
      else console.log('NO DATA');
    }
  } finally {
    // Always restore the sandbox output to the shipped full-JIT v8jsi.dll.
    stageEngine('v8jsi');
    console.log('-- restored out/sandbox-x64/v8jsi.dll to the shipped full-JIT engine --');
  }

  const machine = {
    cpus: os.cpus()[0]?.model, ncpu: os.cpus().length,
    totalRAM_GB: +(os.totalmem() / 1024 ** 3).toFixed(1),
    platform: `${os.platform()} ${os.release()}`,
  };
  const blob = { machine, iters: ITERS, holdMs: HOLD_MS, sampleDelayMs: SAMPLE_DELAY_MS,
                 generatedUtc: new Date().toISOString(), results };
  const outJson = join(HERE, 'footprint-results.json');
  writeFileSync(outJson, JSON.stringify(blob, (k, v) => v, 2));
  console.log('\nWrote', outJson);

  // Markdown table
  const rows: string[] = [];
  rows.push('| case | N | procs | WS total MB | **WS private MB** | **priv commit MB** | committed MB | largest reserve GB | ready ms |');
  rows.push('|---|---|---|---|---|---|---|---|---|');
  const order = [
    ...Ns.map((n) => `sandbox-v8jsi-untrusted-${n}`), 'sandbox-v8jsi-trusted-1',
    ...Ns.map((n) => `sandbox-v8jsisb-untrusted-${n}`),
    ...Ns.map((n) => `wv2-shared-${n}`), ...Ns.map((n) => `wv2-separate-${n}`),
  ];
  for (const key of order) {
    const m = median(results[key] || []);
    if (!m) { rows.push(`| ${key} | | NO DATA |`); continue; }
    rows.push(`| ${m.label} | ${m.n} | ${m.full.count} | ${MB(m.full.ws_total)} | **${MB(m.full.ws_private)}** | **${MB(m.full.priv_commit)}** | ${MB(m.full.committed)} | ${(m.full.largest_reserve / 1024 ** 3).toFixed(0)} | ${m.marks.ready ?? '?'} |`);
  }
  const md = rows.join('\n');
  writeFileSync(join(HERE, 'footprint-table.md'), md + '\n');
  console.log('\n' + md);
}

main().catch((e) => { console.error(e); process.exit(1); });
