// Dev tool: run a widget through the real sandbox bundle inside QuickJS under
// Node, with a fake host that logs everything. Layout queries get canned
// answers. Usage: pnpm tsx scripts/sandbox-node.ts corpus/compat/tabs.html [--ops] [--ops-file msgs.json] [--events]
// `--ops-file` writes the validated ops as one host → renderer `ops` message (a JSON array of
// messages) for the native renderer harnesses (renderer-litehtml/tools/run.cpp).
//
// Mirrors the worker glue (src/host/worker.ts) closely enough to debug the shim
// and to drive unit tests of the mutation protocol.

import { build } from 'esbuild';
import { newQuickJSWASMModuleFromVariant, newVariant, type QuickJSContext, type QuickJSHandle } from 'quickjs-emscripten-core';
import variant from '@jitl/quickjs-ng-wasmfile-release-sync';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { CpuBudget } from '../src/host/cpu-budget.ts';
import { Validator } from '../src/host/validator.ts';
import { LIMIT_DEFAULTS, MAX_TIMER_DELAY_MS, WASM_INITIAL_PAGES, WASM_PAGE_BYTES, type RenderOp } from '../src/shared/protocol.ts';
import { libraryPath, sandboxBuildOptions } from './sandbox-build.ts';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

export interface RunResult {
  ops: unknown[][];
  renderOps: RenderOp[];
  diags: { kind: string; detail: string }[];
  logs: { level: string; text: string }[];
  queries: unknown[];
  bridge: unknown[];
  timers: number;
  probes: string[];
  /** Tasks the CPU quota deferred (the harness drops them instead of retrying). */
  throttled: number;
  /** Final size of the wasm linear memory: the runtime's footprint, never above INITIAL + memoryLimitBytes. */
  wasmBytes: number;
}

export async function bundleSandbox(): Promise<string> {
  const out = await build(sandboxBuildOptions(root));
  return out.outputFiles![0].text;
}

let cachedBundle: string | null = null;

export interface RunOptions {
  verbose?: boolean;
  /** Events to inject after load: [{type:'click', targetId: 5}] etc. */
  events?: Record<string, unknown>[];
  /** How many rounds of pending timers to fire after load. */
  timerRounds?: number;
  /** Custom answers for sync queries. */
  answer?: (q: any) => unknown;
  /** Expressions evaluated inside the VM after load; results land in RunResult.probes. */
  probes?: string[];
  /** Live-timer cap passed to the shim (default LIMIT_DEFAULTS.maxTimers). */
  maxTimers?: number;
  /** Steps run after the timer rounds: visibility changes and probes, in order (probe results land in RunResult.probes before `probes`). */
  script?: ({ visible: boolean } | { probe: string })[];
  /** CPU quota per second (default: effectively unlimited, like the harness's 10 s cpuLimitMs; wasm under Node is slow). */
  cpuQuotaMs?: number;
  /** Validator CSS scope: 'shadow' for ops meant for the Shadow DOM renderer (css-sanitizer.ts). */
  cssScope?: 'document' | 'shadow';
  /** Runtime heap cap (default LIMIT_DEFAULTS.memoryLimitBytes), enforced like the worker does: wasm Memory maximum + setMemoryLimit. */
  memoryLimitBytes?: number;
}

export async function runWidget(html: string, opts: RunOptions = {}): Promise<RunResult> {
  cachedBundle ??= await bundleSandbox();
  // Same memory cap as src/host/worker.ts: the emscripten build cannot count its
  // heap, so the wasm Memory's maximum is the limit (see the comment there).
  const memoryLimitBytes = opts.memoryLimitBytes ?? LIMIT_DEFAULTS.memoryLimitBytes;
  const wasmMemory = new WebAssembly.Memory({ initial: WASM_INITIAL_PAGES, maximum: WASM_INITIAL_PAGES + Math.ceil(memoryLimitBytes / WASM_PAGE_BYTES) });
  const QuickJS = await newQuickJSWASMModuleFromVariant(newVariant(variant, { wasmMemory }));
  const rt = QuickJS.newRuntime();
  rt.setMemoryLimit(memoryLimitBytes);
  rt.setMaxStackSize(1 << 20);
  let deadline = Date.now() + 10_000;
  rt.setInterruptHandler(() => Date.now() > deadline);
  const vm = rt.newContext();
  const res: RunResult = { ops: [], renderOps: [], diags: [], logs: [], queries: [], bridge: [], timers: 0, probes: [], throttled: 0, wasmBytes: 0 };
  const config = { ...LIMIT_DEFAULTS, memoryLimitBytes, cpuLimitMs: 10_000, cpuQuotaMs: opts.cpuQuotaMs ?? 10_000, maxTimers: opts.maxTimers ?? LIMIT_DEFAULTS.maxTimers, visible: true, viewport: { innerWidth: 400, innerHeight: 300, dpr: 1 }, theme: 'light' };
  const budget = new CpuBudget(config);
  const validator = new Validator((kind, detail) => res.diags.push({ kind: 'validator:' + kind, detail }), { colorScheme: 'light', scope: opts.cssScope ?? 'document' });
  const timers = new Map<number, number>();
  const log = (...a: unknown[]) => opts.verbose && console.log(...a);

  const hostFn = vm.newFunction('__host', (k: QuickJSHandle, a?: QuickJSHandle) => {
    const kind = vm.getString(k);
    const arg = a ? vm.getString(a) : '';
    switch (kind) {
      case 'ops': {
        const ops = JSON.parse(arg);
        res.ops.push(ops);
        const out = validator.processOps(ops);
        res.renderOps.push(...out);
        log(`ops (${ops.length}) →`, JSON.stringify(out).slice(0, 2000));
        return vm.undefined;
      }
      case 'query': {
        const q = JSON.parse(arg);
        res.queries.push(q);
        const clean = validator.validateQuery(q);
        const answer = clean ? opts.answer?.(clean) ?? defaultAnswer(clean) : { error: 'invalid query' };
        log('query', JSON.stringify(q), '→', JSON.stringify(answer));
        return vm.newString(JSON.stringify(answer));
      }
      case 'now': return vm.newNumber(performance.now());
      case 'timer': {
        const [id, delay] = JSON.parse(arg);
        if (timers.size >= config.maxTimers) { res.diags.push({ kind: 'timer-limit-enforced', detail: `glue refused a timer beyond maxTimers=${config.maxTimers}` }); return vm.undefined; }
        timers.set(id, Math.min(Math.max(Number(delay) || 0, 0), MAX_TIMER_DELAY_MS));
        return vm.undefined;
      }
      case 'clearTimer': timers.delete(Number(arg)); return vm.undefined;
      case 'log': { const i = arg.indexOf(':'); res.logs.push({ level: arg.slice(0, i), text: arg.slice(i + 1) }); log('[' + arg.slice(0, i) + ']', arg.slice(i + 1)); return vm.undefined; }
      case 'diag': { const i = arg.indexOf(':'); res.diags.push({ kind: arg.slice(0, i), detail: arg.slice(i + 1) }); log('[diag]', arg); return vm.undefined; }
      case 'bridge': res.bridge.push(JSON.parse(arg)); log('[bridge]', arg); return vm.undefined;
      case 'intl': return vm.newString(String(intl(JSON.parse(arg))));
      case 'random': return vm.newString(JSON.stringify(Array.from({ length: Number(arg) | 0 }, () => (Math.random() * 256) | 0)));
      default: return vm.undefined;
    }
  });
  vm.setProp(vm.global, '__host', hostFn);
  hostFn.dispose();
  const binFn = vm.newFunction('__hostBinary', () => vm.null);
  vm.setProp(vm.global, '__hostBinary', binFn);
  binFn.dispose();

  const fail = (err: QuickJSHandle, where: string) => {
    const e = vm.dump(err);
    err.dispose();
    const text = e && typeof e === 'object' ? `${e.name}: ${e.message}\n${e.stack ?? ''}` : String(e);
    res.logs.push({ level: 'error', text: `${where}: ${text}` });
    log('ERROR', where, text);
  };
  const evalCode = (code: string, name: string, type: 'global' | 'module' = 'global') => {
    const r = vm.evalCode(code, name, { type });
    if (r.error) fail(r.error, name);
    else r.value.dispose();
  };
  evalCode(cachedBundle, 'sandbox-bundle.js');
  const dispatch = vm.getProp(vm.global, '__twDispatch');
  if (vm.typeof(dispatch) !== 'function') throw new Error('bundle did not define __twDispatch');
  const tw = (kind: string, arg = ''): string | null => {
    const k = vm.newString(kind);
    const a = vm.newString(arg);
    const r = vm.callFunction(dispatch, vm.undefined, k, a);
    k.dispose();
    a.dispose();
    if (r.error) { fail(r.error, kind); return null; }
    const v = vm.typeof(r.value) === 'string' ? vm.getString(r.value) : null;
    r.value.dispose();
    return v;
  };
  const drain = () => { for (let i = 0; i < 100; i++) { const r = rt.executePendingJobs(); if (r.error) { fail(r.error, 'job'); continue; } if (r.value === 0) break; } };
  const settle = () => { for (let i = 0; i < 20; i++) { drain(); const more = tw('endTick'); if (more !== '1' && !rt.hasPendingJob()) break; } };
  const pump = () => {
    for (let i = 0; i < 1000; i++) {
      const next = tw('nextScript');
      if (!next) break;
      const s = JSON.parse(next);
      let ok = true;
      if (s.src !== undefined) {
        const lib = /chart/i.test(s.src) ? 'chart.js/dist/chart.umd.min.js' : /d3/i.test(s.src) ? 'd3/dist/d3.min.js' : null;
        if (lib) evalCode(fs.readFileSync(libraryPath(lib), 'utf8'), lib);
        else { ok = false; res.diags.push({ kind: 'script-skipped', detail: s.src }); }
      } else evalCode(s.code, `inline-${s.id}.js`, s.type);
      settle();
      tw('scriptDone', JSON.stringify([s.id, ok]));
      settle();
    }
  };
  /** Same policy as the worker: decide → run with the slice → record. A deferred task is dropped and counted. */
  const task = (kind: string, fn: () => void) => {
    const now = performance.now();
    const d = budget.decide(kind, now);
    if (!d.run) { res.throttled++; res.diags.push({ kind: 'cpu-throttled', detail: `${kind} deferred until ${d.retryAt.toFixed(0)}` }); return; }
    deadline = Date.now() + d.sliceMs;
    fn(); settle(); pump();
    budget.record(kind, performance.now() - now, performance.now());
  };

  task('boot', () => tw('boot', JSON.stringify({ html, config })));
  task('boot', () => tw('loaded'));
  for (const ev of opts.events ?? []) task('event', () => tw('event', JSON.stringify(ev)));
  for (let round = 0; round < (opts.timerRounds ?? 2); round++) {
    const due = [...timers.keys()];
    for (const id of due) { timers.delete(id); res.timers++; task('timer', () => tw('timer', String(id))); }
    task('frame', () => tw('frame', String(performance.now())));
  }
  const probe = (code: string) => {
    const r = vm.evalCode(`(function(){ try { return String(${code}); } catch (e) { return 'THREW ' + e; } })()`, 'probe.js');
    if (r.error) { res.probes.push('ERROR ' + JSON.stringify(vm.dump(r.error))); r.error.dispose(); }
    else { res.probes.push(vm.getString(r.value)); r.value.dispose(); }
    settle();
  };
  for (const step of opts.script ?? []) {
    if ('visible' in step) task('visibility', () => tw('visibility', JSON.stringify({ visible: step.visible })));
    else probe(step.probe);
  }
  for (const code of opts.probes ?? []) probe(code);
  res.wasmBytes = wasmMemory.buffer.byteLength;
  dispatch.dispose();
  vm.dispose();
  rt.dispose();
  return res;
}

function defaultAnswer(q: any): unknown {
  switch (q.q) {
    case 'rect': return { x: 0, y: 0, width: 100, height: 20, top: 0, left: 0, right: 100, bottom: 20 };
    case 'rects': return [{ x: 0, y: 0, width: 100, height: 20, top: 0, left: 0, right: 100, bottom: 20 }];
    case 'box': return { offsetWidth: 100, offsetHeight: 20, offsetTop: 0, offsetLeft: 0, offsetParent: 0, clientWidth: 100, clientHeight: 20, clientTop: 0, clientLeft: 0, scrollWidth: 100, scrollHeight: 20, scrollTop: 0, scrollLeft: 0 };
    case 'cs': return q.prop ? '' : { display: 'block', width: '100px', height: '20px', 'font-size': '16px' };
    case 'svg': return q.method === 'getBBox' ? { x: 0, y: 0, width: 50, height: 12 } : q.method.startsWith('get') && /CTM/.test(q.method) ? { a: 1, b: 0, c: 0, d: 1, e: 0, f: 0 } : 42;
    case 'measureText': return { width: String(q.text).length * 6, actualBoundingBoxAscent: 8, actualBoundingBoxDescent: 2 };
    case 'matchMedia': return { matches: false };
    case 'viewport': return { innerWidth: 400, innerHeight: 300, dpr: 1 };
    case 'validity': return { valid: true, message: '' };
    case 'elementFromPoint': return 0;
    case 'canvasCall': return q.method === 'toDataURL' ? 'data:,' : false;
    default: return null;
  }
}

function intl(req: any): string {
  try {
    const d = new Date(Number(req.value));
    switch (req.kind) {
      case 'number': return new Intl.NumberFormat(req.locale, req.opts).format(Number(req.value));
      case 'date': return new Intl.DateTimeFormat(req.locale, req.opts).format(d);
      case 'dateMethod': return req.method === 'toLocaleDateString' ? d.toLocaleDateString(req.locale, req.opts) : req.method === 'toLocaleTimeString' ? d.toLocaleTimeString(req.locale, req.opts) : d.toLocaleString(req.locale, req.opts);
      case 'compare': return String(new Intl.Collator(req.locale, req.opts).compare(req.a, req.b));
      case 'plural': return new Intl.PluralRules(req.locale, req.opts).select(Number(req.value));
      default: return '';
    }
  } catch { return ''; }
}

if (process.argv[1] && path.resolve(process.argv[1]) === new URL(import.meta.url).pathname) {
  const file = process.argv[2];
  if (!file) { console.error('usage: tsx scripts/sandbox-node.ts <widget.html> [--ops] [--ops-file msgs.json] [--verbose]'); process.exit(2); }
  const verbose = process.argv.includes('--verbose');
  const probes = process.argv.flatMap((a, i) => (a === '--eval' ? [process.argv[i + 1]] : []));
  const r = await runWidget(fs.readFileSync(file, 'utf8'), { verbose, probes, cssScope: process.argv.includes('--shadow') ? 'shadow' : 'document' });
  r.probes.forEach((p, i) => console.log(`probe[${i}] ${probes[i]} → ${p}`));
  if (process.argv.includes('--ops')) console.log(JSON.stringify(r.renderOps, null, 0));
  const opsFile = process.argv.indexOf('--ops-file');
  if (opsFile > 0 && process.argv[opsFile + 1]) fs.writeFileSync(process.argv[opsFile + 1], JSON.stringify([{ type: 'ops', ops: r.renderOps }]));
  const tags = r.renderOps.filter((o) => o.op === 'el').length;
  console.log(`\nbatches ${r.ops.length}, sandbox ops ${r.ops.reduce((n, b) => n + b.length, 0)}, render ops ${r.renderOps.length} (${tags} elements), queries ${r.queries.length}, timers fired ${r.timers}`);
  for (const d of r.diags) console.log('diag ', d.kind, '—', d.detail);
  for (const l of r.logs) console.log(`[${l.level}]`, l.text);
  for (const b of r.bridge) console.log('bridge', JSON.stringify(b));
}
