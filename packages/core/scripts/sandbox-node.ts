// Dev tool: run a widget through the real sandbox bundle inside QuickJS under
// Node, with a fake host that logs everything. Layout queries get canned
// answers. Usage: pnpm tsx scripts/sandbox-node.ts corpus/compat/tabs.html [--ops] [--ops-file msgs.json] [--events]
// `--ops-file` writes the host → renderer messages a renderer gets (a JSON array: the `theme`
// message WidgetHost sends first, then the validated ops as one `ops` message) for the native
// renderer harnesses (renderer-litehtml/tools/run.cpp).
//
// Mirrors the worker glue (src/host/worker.ts) closely enough to debug the shim
// and to drive unit tests of the mutation protocol.

import { build } from 'esbuild';
import { newQuickJSWASMModuleFromVariant, newVariant, type QuickJSContext, type QuickJSHandle } from 'quickjs-emscripten-core';
import variant from '@telegram/widget-sandbox-quickjs';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { CpuBudget, queryBudgetMs } from '../src/host/cpu-budget.ts';
import {
  DEFAULT_LOCALE, HapticGate, PRIVILEGED_METHODS, checkBridgeCall, checkLocale, checkStorageCommit, checkThemeParams, toWidgetTheme,
  type ButtonTokens, type StorageCommit, type TextTokens, type ThemeParams, type WidgetAction, type WidgetTheme,
} from '../src/shared/sdk-policy.ts';
import { PROFILE_VERSION } from '../src/shared/profile-tables.ts';
import { formatIntl } from '../src/shared/intl.ts';
import { decodeBinaryOps } from '../src/shared/binary-ops-decode.ts';
import { Validator } from '../src/host/validator.ts';
import { LIMIT_DEFAULTS, MAX_ANIM_BATCH_BYTES, MAX_TIMER_DELAY_MS, WASM_INITIAL_PAGES, WASM_PAGE_BYTES, type RenderOp } from '../src/shared/protocol.ts';
import { sandboxBuildOptions } from './sandbox-build.ts';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

export interface RunResult {
  /** The theme WidgetHost passes to the renderer at load (`{type:'theme', theme}`, docs/sdk-v1.md §3.4): scheme, checked palette, button and text tokens. */
  theme: WidgetTheme;
  /** One entry per `morph` script step: whether the shim applied it. */
  morphed: boolean[];
  ops: unknown[][];
  renderOps: RenderOp[];
  diags: { kind: string; detail: string }[];
  logs: { level: string; text: string }[];
  queries: unknown[];
  /** Bridge calls the glue let through, raw (`[method, args]`). */
  bridge: unknown[];
  /** What WidgetHost's `onAction` would get (docs/sdk-v1.md §4.5). */
  actions: WidgetAction[];
  /** Storage commits the glue accepted, in order (`onStorageCommit` gets one call per area of each). */
  storageCommits: StorageCommit[];
  timers: number;
  probes: string[];
  /** Tasks the CPU quota deferred (the harness drops them instead of retrying). */
  throttled: number;
  /** The most layout queries any one task made, and their summed answer time (ms): how close a widget comes to the layout-query budget. */
  maxTaskQueries: number;
  maxTaskQueryMs: number;
  /** Final size of the wasm linear memory: the runtime's footprint, never above INITIAL + memoryLimitBytes. */
  wasmBytes: number;
  /** Canvas ops that arrived as word payloads (`canvasWords`). */
  canvasWordBatches: number;
  /** Ops batches that arrived as binary records (`binaryOps`). */
  binaryBatches: number;
  anims: unknown[][];
  animOps: unknown[][];
  /** Ops batches, anim batches and sync queries in the order the shim sent them. */
  stream: { kind: 'ops' | 'anim' | 'query'; json: string }[];
}

/** The JSON command arrays a canvas word payload stands for (docs/protocol.md, "Canvas words"). */
export function decodeCanvasWords(all: number[], joined: string): any[][] {
  const m = all[all.length - 1];
  const lengths = all.slice(all.length - 1 - m, all.length - 1);
  const strings: string[] = [];
  let at = 0;
  for (const n of lengths) { strings.push(joined.slice(at, at + n)); at += n; }
  const f = all.slice(0, all.length - 1 - m);
  const kinds = ['c', 's', 'r', 'g', 'gs', 'pn', 'pc'];
  const num = (x: number) => (Number.isFinite(x) ? x : null);
  const out: any[][] = [];
  let j = 0;
  while (j < f.length) {
    const kind = f[j], name = f[j + 1], ident = f[j + 2], argc = f[j + 3];
    j += 4;
    const a: any[] = [kinds[kind]];
    if (kind >= 3) a.push(ident);
    if (name >= 0) a.push(strings[name]);
    for (let q = 0; q < argc; q++) {
      const tag = f[j], x = f[j + 1];
      j += 2;
      if (tag === 0) a.push(num(x));
      else if (tag === 1) a.push(strings[x]);
      else if (tag === 7) a.push(x !== 0);
      else if (tag === 6) a.push(null);
      else if (tag === 2) a.push({ g: x });
      else if (tag === 3) a.push({ p: x });
      else if (tag === 4) a.push({ n: x });
      else { a.push(f.slice(j, j + x).map(num)); j += x; }
    }
    out.push(a);
  }
  return out;
}

export async function bundleSandbox(): Promise<string> {
  const out = await build(sandboxBuildOptions(root));
  return out.outputFiles![0].text;
}

let cachedBundle: string | null = null;
/** Widget clocks tick in 100 µs steps, as in src/host/worker.ts (`now` and frame times alike). */
const coarseNowMs = (ms: number) => Math.floor(ms / 0.1) * 0.1;

export interface RunOptions {
  verbose?: boolean;
  /** Events to inject after load: [{type:'click', targetId: 5}] etc. `gesture: n` marks a trusted user gesture (the web host's number), as for user activation. */
  events?: Record<string, unknown>[];
  /** How many rounds of pending timers to fire after load. */
  timerRounds?: number;
  /** Custom answers for sync queries. */
  answer?: (q: any) => unknown;
  /** Continuous-execution slice per task (default 10 s, so a harness run never trips it); lower it to exercise the CPU and layout-query limits. */
  cpuLimitMs?: number;
  /** Expressions evaluated inside the VM after load; results land in RunResult.probes. */
  probes?: string[];
  /** false: the shim always replaces a sole text child on textContent writes (the in-place update off). */
  inPlaceText?: boolean;
  /** false: getAttribute('class') and same-value className writes take the full path. */
  attrFast?: boolean;
  /** Live-timer cap passed to the shim (default LIMIT_DEFAULTS.maxTimers). */
  maxTimers?: number;
  /** Initial visibility (default true): a hidden widget gets no frames and cannot use a gesture, as in the worker and the web host. */
  visible?: boolean;
  /** Steps run after the timer rounds: visibility and theme changes and probes, in order (probe results land in RunResult.probes before `probes`). */
  script?: ({ visible: boolean } | { probe: string } | { morph: string } | { theme: 'light' | 'dark'; themeParams?: Partial<ThemeParams> })[];
  /** The colour scheme at start (default light). */
  theme?: 'light' | 'dark';
  /** The client's palette (checked as WidgetHost does: unknown keys and non-#rrggbb values are dropped). */
  themeParams?: Record<string, unknown>;
  /** The client's button and text tokens (docs/sdk-v1.md §2.4): no effect on the sandbox, they go to the renderer in RunResult.theme. */
  button?: ButtonTokens;
  text?: TextTokens;
  /** The client's UI language (default en-US). */
  locale?: string;
  /** The client's Web Storage preload; over the quota it is dropped whole (`storage-rejected`). */
  storage?: unknown;
  /** CPU quota per second (default: effectively unlimited, like the harness's 10 s cpuLimitMs; wasm under Node is slow). */
  cpuQuotaMs?: number;
  /** Validator CSS scope: 'shadow' for ops meant for the Shadow DOM renderer (css-sanitizer.ts). */
  cssScope?: 'document' | 'shadow';
  /** Runtime heap cap (default LIMIT_DEFAULTS.memoryLimitBytes), enforced like the worker does: wasm Memory maximum + setMemoryLimit. */
  memoryLimitBytes?: number;
  /** Sandbox bundle source to run instead of the one built from packages/core/src (scripts/ops-diff.ts). */
  bundle?: string;
  /** `__host` kinds the harness does not know, with their argument. */
  unknownHost?: (kind: string, arg: string) => void;
  /** Answers for `intl` requests (the request as the shim sends it) instead of Node's own Intl; undefined falls through to it. */
  intl?: (req: any) => string | undefined;
  /** Host clock (`now`) and random bytes (`random`) instead of performance.now / Math.random, for reproducible runs. */
  now?: () => number;
  random?: (count: number) => number[];
  /** Advertise `canvasWords` and install `__hostCanvas`; payloads are decoded back into JSON commands before validation. */
  canvasWords?: boolean;
  policy?: 'compat' | 'strict';
  /** Advertise `binaryOps` and install `__hostOps`; batches are decoded back into JSON ops before validation. */
  binaryOps?: boolean;
  canvasRect?: boolean;
  webAnimations?: boolean;
  frames?: number;
  /** Advertise `morphUpdates` (in-place document updates through the `morph` dispatch). */
  morphUpdates?: boolean;
}

export async function runWidget(html: string, opts: RunOptions = {}): Promise<RunResult> {
  const bundle = opts.bundle ?? (cachedBundle ??= await bundleSandbox());
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
  // The layout-query budget of the worker and the native glue (cpu-budget.ts queryBudgetMs): the harness answers queries in-process, so the
  // time is the answer callback's, and the accounting is kept so a slow `answer` reproduces the kill.
  let taskQueryMs = 0;
  let taskQueries = 0;
  let queryLimitHit = false;
  let currentSliceMs = 10_000;
  let currentKind = 'boot';
  const vm = rt.newContext();
  const res: RunResult = { theme: toWidgetTheme({ colorScheme: opts.theme ?? 'light', params: opts.themeParams, button: opts.button, text: opts.text }), ops: [], renderOps: [], diags: [], logs: [], queries: [], bridge: [], actions: [], storageCommits: [], timers: 0, probes: [], throttled: 0, maxTaskQueries: 0, maxTaskQueryMs: 0, wasmBytes: 0, canvasWordBatches: 0, binaryBatches: 0, anims: [], animOps: [], stream: [], morphed: [] };
  const config = { ...LIMIT_DEFAULTS, features: [...(opts.canvasWords ? ['canvasWords'] : []), ...(opts.binaryOps ? ['binaryOps'] : []), ...(opts.webAnimations ? ['webAnimations'] : []), ...(opts.morphUpdates ? ['morphUpdates'] : []), ...(opts.inPlaceText === false ? ['noInPlaceText'] : []), ...(opts.attrFast === false ? ['noAttrFast'] : []), ...(opts.canvasRect === false ? ['noCanvasRect'] : [])], memoryLimitBytes, cpuLimitMs: opts.cpuLimitMs ?? 10_000, cpuQuotaMs: opts.cpuQuotaMs ?? 10_000, maxTimers: opts.maxTimers ?? LIMIT_DEFAULTS.maxTimers, visible: opts.visible !== false, viewport: { innerWidth: 400, innerHeight: 300, dpr: 1 }, theme: opts.theme ?? 'light', themeParams: checkThemeParams(opts.themeParams), locale: checkLocale(opts.locale) ?? DEFAULT_LOCALE, profile: PROFILE_VERSION, storage: {} as StorageCommit };
  if (opts.storage !== undefined) {
    const preload = checkStorageCommit(opts.storage);
    if (preload) config.storage = preload;
    else res.diags.push({ kind: 'storage-rejected', detail: 'the preload is not { local, session } objects of strings within the quota' });
  }
  const haptics = new HapticGate();
  const budget = new CpuBudget(config);
  const validator = new Validator((kind, detail) => res.diags.push({ kind: 'validator:' + kind, detail }), { colorScheme: 'light', scope: opts.cssScope ?? 'document' });
  const policy = opts.policy === 'compat' ? 'compat' : 'strict';
  validator.setPolicy(policy);
  const timers = new Map<number, number>();
  let activeGesture = 0;
  let consumedGesture = 0;
  let visible = config.visible;
  const log = (...a: unknown[]) => opts.verbose && console.log(...a);

  const hostFn = vm.newFunction('__host', (k: QuickJSHandle, a?: QuickJSHandle) => {
    const kind = vm.getString(k);
    const arg = a ? vm.getString(a) : '';
    switch (kind) {
      case 'ops': {
        acceptOps(JSON.parse(arg));
        return vm.undefined;
      }
      case 'query': {
        const q = JSON.parse(arg);
        res.queries.push(q);
        res.stream.push({ kind: 'query', json: arg });
        if (queryLimitHit) return vm.newString('{"error":"query budget exceeded"}');
        const clean = validator.validateQuery(q);
        const q0 = performance.now();
        const answer = clean ? opts.answer?.(clean) ?? defaultAnswer(clean) : { error: 'invalid query' };
        const dt = performance.now() - q0;
        taskQueries++;
        deadline += dt; // time spent in the answer is not widget CPU time (as in the worker); the query budget below bounds it instead
        taskQueryMs += dt;
        if (taskQueryMs > queryBudgetMs(currentSliceMs)) { queryLimitHit = true; deadline = -Infinity; }
        log('query', JSON.stringify(q), '→', JSON.stringify(answer));
        return vm.newString(JSON.stringify(answer));
      }
      case 'now': return vm.newNumber(opts.now ? opts.now() : coarseNowMs(performance.now()));
      case 'timer': {
        const [id, delay] = JSON.parse(arg);
        if (timers.size >= config.maxTimers) { res.diags.push({ kind: 'timer-limit-enforced', detail: `glue refused a timer beyond maxTimers=${config.maxTimers}` }); return vm.undefined; }
        const ms = Math.min(Math.max(Number(delay) || 0, 0), MAX_TIMER_DELAY_MS);
        // Rounds fire timers by delay, as a clock would; hidden ones as the other glue copies clamp them (1 Hz, in order).
        timers.set(id, visible ? ms : Math.max(ms, 1000 + ms / 10));
        return vm.undefined;
      }
      case 'clearTimer': timers.delete(Number(arg)); return vm.undefined;
      case 'anim': {
        const list = JSON.parse(arg);
        res.anims.push(list);
        res.stream.push({ kind: 'anim', json: arg });
        res.animOps.push(arg.length > MAX_ANIM_BATCH_BYTES ? [] : validator.validateAnim(list));
        return vm.undefined;
      }
      case 'log': { const i = arg.indexOf(':'); res.logs.push({ level: arg.slice(0, i), text: arg.slice(i + 1) }); log('[' + arg.slice(0, i) + ']', arg.slice(i + 1)); return vm.undefined; }
      case 'diag': { const i = arg.indexOf(':'); res.diags.push({ kind: arg.slice(0, i), detail: arg.slice(i + 1) }); log('[diag]', arg); return vm.undefined; }
      case 'bridge': {
        // As WidgetHost (docs/sdk-v1.md §3.2): the method and its arguments, user activation for links, the haptic limit.
        let call: unknown;
        try { call = JSON.parse(arg); } catch {}
        const method = Array.isArray(call) ? call[0] : undefined;
        const action = Array.isArray(call) ? checkBridgeCall(method, call[1]) : null;
        if (!action) { res.diags.push({ kind: 'bridge-rejected', detail: `${String(method).slice(0, 64)}: unknown method or malformed arguments` }); return vm.undefined; }
        if (policy === 'strict' && PRIVILEGED_METHODS.has(String(method))) {
          if (!visible || !(activeGesture > consumedGesture)) {
            res.diags.push({ kind: 'activation-blocked', detail: `${method} ignored outside a tap on the visible widget` });
            return vm.undefined;
          }
          consumedGesture = activeGesture;
          activeGesture = 0;
        }
        if (action.kind === 'haptic') {
          const d = haptics.decide(opts.now ? opts.now() : performance.now(), visible);
          if (d === 'report') res.diags.push({ kind: 'haptic-limit', detail: visible ? 'over the rate limit' : 'hidden widget' });
          if (d !== 'play') return vm.undefined;
        }
        res.bridge.push(call);
        res.actions.push(action);
        log('[bridge]', arg);
        return vm.undefined;
      }
      case 'storage': {
        let commit: StorageCommit | null = null;
        try { commit = checkStorageCommit(JSON.parse(arg)); } catch {}
        if (!commit || (!commit.local && !commit.session)) res.diags.push({ kind: 'storage-rejected', detail: arg.slice(0, 200) });
        else res.storageCommits.push(commit);
        log('[storage]', arg.slice(0, 2000));
        return vm.undefined;
      }
      case 'intl': { const req = JSON.parse(arg); let v = opts.intl?.(req); if (v === undefined) { try { v = formatIntl(req); } catch { v = ''; } } return vm.newString(String(v)); }
      case 'random': return vm.newString(JSON.stringify(opts.random ? opts.random(Number(arg) | 0) : Array.from({ length: Number(arg) | 0 }, () => (Math.random() * 256) | 0)));
      default: opts.unknownHost?.(kind, arg); return vm.undefined;
    }
  });
  vm.setProp(vm.global, '__host', hostFn);
  hostFn.dispose();
  const canvasPayloads: any[][][] = [];
  // vm.getString stops at a NUL; strings handed over by length go through encodeURIComponent instead.
  const encoder = vm.unwrapResult(vm.evalCode('(s) => encodeURIComponent(s)', 'harness-encode.js'));
  const fullString = (h: QuickJSHandle): string => {
    const r = vm.callFunction(encoder, vm.undefined, h);
    if (r.error) { r.error.dispose(); return vm.getString(h); }
    const text = decodeURIComponent(vm.getString(r.value));
    r.value.dispose();
    return text;
  };
  const acceptOps = (ops: any[]) => {
    // Each payload is taken once (CanvasWords.swift `placeholders`): a repeated key keeps its `bin` and the validator rejects it.
    for (const op of ops) if (op && op.op === 'canvas' && typeof op.bin === 'number' && canvasPayloads[op.bin]) { op.cmds = canvasPayloads[op.bin]; delete canvasPayloads[op.bin]; delete op.bin; res.canvasWordBatches++; }
    canvasPayloads.length = 0;
    res.ops.push(ops);
    res.stream.push({ kind: 'ops', json: JSON.stringify(ops) });
    const out = validator.processOps(ops);
    res.renderOps.push(...out);
    log(`ops (${ops.length}) →`, JSON.stringify(out).slice(0, 2000));
  };
  if (opts.binaryOps) {
    const internTable: string[] = [];
    const opsFn = vm.newFunction('__hostOps', (w: QuickJSHandle, s: QuickJSHandle) => {
      const num = (name: string) => { const h = vm.getProp(w, name); const v = vm.getNumber(h); h.dispose(); return v; };
      const bufferHandle = vm.getProp(w, 'buffer');
      const bytes = vm.getArrayBuffer(bufferHandle);
      const copy = bytes.value.slice();
      bytes.dispose();
      bufferHandle.dispose();
      const start = num('byteOffset') / 4;
      const words = new Int32Array(copy.buffer, copy.byteOffset, copy.byteLength / 4).subarray(start, start + num('length'));
      try {
        acceptOps(decodeBinaryOps(words, fullString(s), internTable));
        res.binaryBatches++;
      } catch (e) {
        res.diags.push({ kind: 'protocol-violation', detail: 'malformed binary ops batch (' + (e as Error).message + ')' });
      }
      return vm.undefined;
    });
    vm.setProp(vm.global, '__hostOps', opsFn);
    opsFn.dispose();
  }
  if (opts.canvasWords) {
    const canvasFn = vm.newFunction('__hostCanvas', (k: QuickJSHandle, w: QuickJSHandle, s: QuickJSHandle) => {
      const key = vm.getNumber(k);
      const num = (name: string) => { const h = vm.getProp(w, name); const v = vm.getNumber(h); h.dispose(); return v; };
      const bufferHandle = vm.getProp(w, 'buffer');
      const bytes = vm.getArrayBuffer(bufferHandle);
      const copy = bytes.value.slice();
      bytes.dispose();
      bufferHandle.dispose();
      const start = num('byteOffset') / 8;
      const words = Array.from(new Float64Array(copy.buffer, copy.byteOffset, copy.byteLength / 8).subarray(start, start + num('length')));
      canvasPayloads[key] = decodeCanvasWords(words, fullString(s));
      return vm.undefined;
    });
    vm.setProp(vm.global, '__hostCanvas', canvasFn);
    canvasFn.dispose();
  }
  const binFn = vm.newFunction('__hostBinary', () => vm.null);
  vm.setProp(vm.global, '__hostBinary', binFn);
  binFn.dispose();

  const fail = (err: QuickJSHandle, where: string) => {
    const e = vm.dump(err);
    err.dispose();
    let text = e && typeof e === 'object' ? `${e.name}: ${e.message}\n${e.stack ?? ''}` : String(e);
    if (queryLimitHit && /interrupted/.test(text)) {
      text = `Layout query limit exceeded: host queries blocked the renderer for more than ${queryBudgetMs(currentSliceMs).toFixed(0)} ms in one ${currentKind} task`;
      res.diags.push({ kind: 'query-limit', detail: text });
    }
    res.logs.push({ level: 'error', text: `${where}: ${text}` });
    log('ERROR', where, text);
  };
  const evalCode = (code: string, name: string, type: 'global' | 'module' = 'global') => {
    const r = vm.evalCode(code, name, { type });
    if (r.error) fail(r.error, name);
    else r.value.dispose();
  };
  const bundleResult = vm.evalCode(bundle, 'sandbox-bundle.js', { type: 'global' });
  if (bundleResult.error) {
    fail(bundleResult.error, 'sandbox-bundle.js');
    encoder.dispose();
    vm.dispose();
    rt.dispose();
    throw new Error('sandbox bundle failed to load');
  }
  bundleResult.value.dispose();
  const dispatch = vm.getProp(vm.global, '__twDispatch');
  if (vm.typeof(dispatch) !== 'function') throw new Error('bundle did not define __twDispatch');
  const hide = vm.evalCode("(function(g){for(const n of ['__host','__hostBinary','__hostCanvas','__hostOps','__twDispatch'])if(!delete g[n])throw new Error(n);})(globalThis);", 'hide-host.js');
  if (hide.error) {
    fail(hide.error, 'hide-host.js');
    throw new Error('sandbox bundle failed to load');
  }
  hide.value.dispose();
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
      // External scripts never run: nothing is fetched or bundled (profile 1).
      if (s.src !== undefined) { ok = false; res.diags.push({ kind: 'script-skipped', detail: s.src }); }
      else evalCode(s.code, `inline-${s.id}.js`, s.type);
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
    currentSliceMs = d.sliceMs;
    currentKind = kind;
    taskQueryMs = 0;
    taskQueries = 0;
    queryLimitHit = false;
    fn(); settle(); pump();
    res.maxTaskQueries = Math.max(res.maxTaskQueries, taskQueries);
    res.maxTaskQueryMs = Math.max(res.maxTaskQueryMs, taskQueryMs);
    budget.record(kind, performance.now() - now, performance.now()); // wall time: includes the queries, as the worker's cpuMs + taskQueryMs does
  };

  task('boot', () => tw('boot', JSON.stringify({ html, config })));
  task('boot', () => tw('loaded'));
  for (const ev of opts.events ?? []) {
    const { gesture, ...event } = ev as Record<string, unknown> & { gesture?: number };
    task('event', () => {
      activeGesture = visible && typeof gesture === 'number' && gesture > consumedGesture ? gesture : 0;
      try {
        tw('event', JSON.stringify(event));
      } finally {
        activeGesture = 0;
      }
    });
  }
  // Due timers by delay; equal delays in the order they were set.
  const dueTimers = () => [...timers.entries()].sort((a, b) => a[1] - b[1]).map(([id]) => id);
  for (let round = 0; round < (opts.timerRounds ?? 2); round++) {
    const due = dueTimers();
    for (const id of due) { timers.delete(id); res.timers++; task('timer', () => tw('timer', String(id))); }
    if (visible) task('frame', () => tw('frame', String(coarseNowMs(performance.now()))));
  }
  for (let round = 0; round < (opts.frames ?? 0); round++) {
    const due = dueTimers();
    for (const id of due) { timers.delete(id); res.timers++; task('timer', () => tw('timer', String(id))); }
    if (visible) task('frame', () => tw('frame', String(coarseNowMs(performance.now()))));
  }
  const probe = (code: string) => {
    const r = vm.evalCode(`(function(){ try { return String(${code}); } catch (e) { return 'THREW ' + e; } })()`, 'probe.js');
    if (r.error) { res.probes.push('ERROR ' + JSON.stringify(vm.dump(r.error))); r.error.dispose(); }
    else { res.probes.push(vm.getString(r.value)); r.value.dispose(); }
    settle();
  };
  for (const step of opts.script ?? []) {
    if ('visible' in step) {
      visible = step.visible;
      task('visibility', () => tw('visibility', JSON.stringify({ visible: step.visible })));
    }
    else if ('morph' in step) task('morph', () => { res.morphed.push(tw('morph', JSON.stringify({ html: step.morph })) === '1'); });
    else if ('theme' in step) task('theme', () => tw('theme', JSON.stringify({ theme: step.theme, themeParams: checkThemeParams(step.themeParams) })));
    else probe(step.probe);
  }
  for (const code of opts.probes ?? []) probe(code);
  res.wasmBytes = wasmMemory.buffer.byteLength;
  dispatch.dispose();
  encoder.dispose();
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
    case 'anims': return { list: [] };
    case 'validity': return { valid: true, message: '' };
    case 'elementFromPoint': return 0;
    case 'canvasCall': return q.method === 'toDataURL' ? 'data:,' : false;
    default: return null;
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === new URL(import.meta.url).pathname) {
  const file = process.argv[2];
  if (!file) { console.error('usage: tsx scripts/sandbox-node.ts <widget.html> [--ops] [--ops-file msgs.json] [--dark] [--font-size px] [--verbose]'); process.exit(2); }
  const verbose = process.argv.includes('--verbose');
  const probes = process.argv.flatMap((a, i) => (a === '--eval' ? [process.argv[i + 1]] : []));
  const fontSize = process.argv.indexOf('--font-size');
  const r = await runWidget(fs.readFileSync(file, 'utf8'), {
    verbose, probes, cssScope: process.argv.includes('--shadow') ? 'shadow' : 'document', theme: process.argv.includes('--dark') ? 'dark' : 'light',
    ...(fontSize > 0 ? { text: { fontSize: Number(process.argv[fontSize + 1]) } } : {}),
  });
  r.probes.forEach((p, i) => console.log(`probe[${i}] ${probes[i]} → ${p}`));
  if (process.argv.includes('--ops')) console.log(JSON.stringify(r.renderOps, null, 0));
  const opsFile = process.argv.indexOf('--ops-file');
  if (opsFile > 0 && process.argv[opsFile + 1]) fs.writeFileSync(process.argv[opsFile + 1], JSON.stringify([{ type: 'theme', theme: r.theme }, { type: 'ops', ops: r.renderOps }]));
  const tags = r.renderOps.filter((o) => o.op === 'el').length;
  console.log(`\nbatches ${r.ops.length}, sandbox ops ${r.ops.reduce((n, b) => n + b.length, 0)}, render ops ${r.renderOps.length} (${tags} elements), queries ${r.queries.length}, timers fired ${r.timers}`);
  for (const d of r.diags) console.log('diag ', d.kind, '—', d.detail);
  for (const l of r.logs) console.log(`[${l.level}]`, l.text);
  for (const a of r.actions) console.log('action', JSON.stringify(a));
  for (const c of r.storageCommits) console.log('storage', JSON.stringify(c).slice(0, 500));
}
