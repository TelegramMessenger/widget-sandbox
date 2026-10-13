// Sandbox worker: trusted glue around one QuickJS runtime (invariants 1 and 5).
//
// Everything evaluated inside the QuickJS context — widget scripts, the DOM
// library, our shim — is untrusted. The context gets exactly ONE capability:
// the `__host(kind, payload)` function below, which takes and returns strings.
// No fetch/XHR/WebSocket or any other browser API is exposed to it.
//
// On native clients this file corresponds to the thread that owns the QuickJS
// runtime; `syncQuery` uses a semaphore instead of Atomics.

import { newQuickJSWASMModuleFromVariant, newVariant, type QuickJSContext, type QuickJSHandle, type QuickJSRuntime } from 'quickjs-emscripten-core';
import baseVariant from '@jitl/quickjs-ng-wasmfile-release-sync';
import wasmUrl from '@jitl/quickjs-ng-wasmfile-release-sync/wasm?url';
import { CpuBudget } from './cpu-budget';
import { resolveLibrary } from './loader';
import { MAX_TIMER_DELAY_MS, SAB_BIN_LEN, SAB_HEADER, SAB_JSON_LEN, SAB_STATE, WASM_INITIAL_PAGES, WASM_PAGE_BYTES, type HostToWorker, type SandboxConfig, type Stats, type WorkerToHost } from '../shared/protocol';

const post = (msg: WorkerToHost) => (self as unknown as Worker).postMessage(msg);

const QUERY_TIMEOUT_MS = 3000;
const MAX_LOG = 4000;

let rt: QuickJSRuntime;
let vm: QuickJSContext;
let config: SandboxConfig;
/** The memory limit the wasm Memory was sized for in `warm`; `start` cannot exceed it. */
let warmedMemoryLimitBytes = 0;
let header: Int32Array;
let payload: Uint8Array;
let dead = false;
let interrupted = false;
let deadline = 0;
/** Slice given to the running task (for the kill message); the boot task gets cpuLimitMs. */
let currentSliceMs = 0;
let currentKind = 'boot';
let budget: CpuBudget;
/** JS time of the current task that is ours, not the widget's (evaluating the sandbox bundle). */
let excludeMs = 0;
let lastThrottleDiag = -Infinity;
const throttledSinceDiag: Record<string, number> = {};
let lastBinary: Uint8Array | null = null;
let twDispatch: QuickJSHandle;
let inTask = false;
const queue: HostToWorker[] = [];

// ───────────────────────── visibility and the deferred-task queue ─────────────────────────
//
// Hidden widget (host said `visibility {visible: false}`): no frames are
// requested from the renderer (one is requested on return if the shim asked),
// timers fire at most once per second, batched into one wake-up, and the shim
// gets `visibilitychange`. Deferred tasks keep their order; one `fire` per
// timer id, one frame at most.

let visible = true;
let frameWanted = false;
interface Deferred { kind: string; arg: unknown }
const deferred: Deferred[] = [];
let wakeTimer: ReturnType<typeof setTimeout> | null = null;
let wakeAt = Infinity;
/** Next 1 Hz boundary at which hidden timers fire (0 = none scheduled). */
let hiddenTickAt = 0;

function defer(kind: string, arg: unknown) {
  if (kind === 'frame') {
    const f = deferred.find((d) => d.kind === 'frame');
    if (f) { f.arg = arg; return; }
  } else if (kind === 'timer' && deferred.some((d) => d.kind === 'timer' && d.arg === arg)) {
    return; // intervals do not accumulate missed ticks
  }
  deferred.push({ kind, arg });
}

function scheduleWake(at: number) {
  if (wakeTimer && at >= wakeAt) return;
  if (wakeTimer) clearTimeout(wakeTimer);
  wakeAt = at;
  wakeTimer = setTimeout(wake, Math.max(0, at - performance.now()));
}

/** Runs deferred tasks in order; stops (and re-queues the rest) if one is deferred again. */
function wake() {
  wakeTimer = null;
  wakeAt = Infinity;
  hiddenTickAt = 0;
  const items = deferred.splice(0);
  for (let i = 0; i < items.length; i++) {
    if (dead) return;
    if (!dispatch(items[i].kind, items[i].arg)) {
      deferred.unshift(...items.slice(i));
      return;
    }
  }
}

function onTimerFired(id: number) {
  timers.delete(id);
  if (!visible) {
    defer('timer', id);
    if (!hiddenTickAt) hiddenTickAt = performance.now() + 1000;
    scheduleWake(hiddenTickAt);
    return;
  }
  dispatch('timer', id);
}

function setVisible(v: boolean) {
  if (visible === v) return;
  visible = v;
  dispatch('visibility', { visible: v });
  if (visible) {
    if (frameWanted) { frameWanted = false; post({ type: 'raf' }); }
    if (deferred.length) wake();
  }
}

const stats: Stats = { jsMs: 0, tasks: 0, memoryBytes: 0, ops: 0, queries: 0, queryMs: 0, queryCacheHits: 0, missedMutations: 0, throttledTasks: 0, cpuWindowMs: 0 };
let lastStatsPost = 0;
let lastMemCheck = 0;

const timers = new Map<number, ReturnType<typeof setTimeout>>();
let timerLimitReported = false;
const libCache = new Map<string, string>();
const decoder = new TextDecoder();

// ───────────────────────── sync query bridge ─────────────────────────

/** Blocks this thread until the host has written the renderer's answer into the SAB. */
function syncQuery(queryJson: string): string {
  let query: unknown;
  try { query = JSON.parse(queryJson); } catch { return '{"error":"bad query"}'; }
  const t0 = performance.now();
  Atomics.store(header, SAB_STATE, 1);
  post({ type: 'query', query });
  const res = Atomics.wait(header, SAB_STATE, 1, QUERY_TIMEOUT_MS);
  const dt = performance.now() - t0;
  deadline += dt; // time spent blocked is not widget CPU time
  stats.queries++;
  stats.queryMs += dt;
  if (res === 'timed-out') {
    Atomics.store(header, SAB_STATE, 0);
    return '{"error":"timeout"}';
  }
  const jsonLen = Atomics.load(header, SAB_JSON_LEN);
  const binLen = Atomics.load(header, SAB_BIN_LEN);
  // TextDecoder refuses shared memory: copy out first.
  const json = decoder.decode(payload.slice(0, jsonLen));
  lastBinary = binLen > 0 ? payload.slice(jsonLen, jsonLen + binLen) : null;
  Atomics.store(header, SAB_STATE, 0);
  return json;
}

// ───────────────────────── the single host capability ─────────────────────────

function formatIntl(req: any): string {
  // Intl is delegated to the host's ICU: QuickJS has none. Pure computation, no I/O.
  const locale = typeof req.locale === 'string' || Array.isArray(req.locale) ? req.locale : undefined;
  const opts = req.opts && typeof req.opts === 'object' ? req.opts : undefined;
  const tryLocale = <T>(f: (l: any) => T): T => {
    try { return f(locale); } catch { return f(undefined); }
  };
  switch (req.kind) {
    case 'number': return tryLocale((l) => new Intl.NumberFormat(l, opts).format(Number(req.value)));
    case 'numberParts': return JSON.stringify(tryLocale((l) => new Intl.NumberFormat(l, opts).formatToParts(Number(req.value))));
    case 'numberOptions': return JSON.stringify(tryLocale((l) => new Intl.NumberFormat(l, opts).resolvedOptions()));
    case 'date': return tryLocale((l) => new Intl.DateTimeFormat(l, opts).format(new Date(Number(req.value))));
    case 'dateMethod': {
      const d = new Date(Number(req.value));
      return tryLocale((l) => (req.method === 'toLocaleDateString' ? d.toLocaleDateString(l, opts) : req.method === 'toLocaleTimeString' ? d.toLocaleTimeString(l, opts) : d.toLocaleString(l, opts)));
    }
    case 'dateParts': return JSON.stringify(tryLocale((l) => new Intl.DateTimeFormat(l, opts).formatToParts(new Date(Number(req.value)))));
    case 'dateOptions': return JSON.stringify(tryLocale((l) => new Intl.DateTimeFormat(l, opts).resolvedOptions()));
    case 'plural': return tryLocale((l) => new Intl.PluralRules(l, opts).select(Number(req.value)));
    case 'relative': return tryLocale((l) => new Intl.RelativeTimeFormat(l, opts).format(Number(req.value), req.unit));
    case 'list': return tryLocale((l) => new Intl.ListFormat(l, opts).format((req.value as unknown[]).map(String)));
    case 'compare': return String(tryLocale((l) => new Intl.Collator(l, opts).compare(String(req.a), String(req.b))));
    case 'upper': return tryLocale((l) => String(req.value).toLocaleUpperCase(l));
    case 'lower': return tryLocale((l) => String(req.value).toLocaleLowerCase(l));
    default: return '';
  }
}

function hostCall(kind: string, arg: string): string | number | undefined {
  switch (kind) {
    case 'ops': {
      let ops: unknown;
      try { ops = JSON.parse(arg); } catch { return; }
      if (Array.isArray(ops)) stats.ops += ops.length;
      post({ type: 'ops', ops });
      return;
    }
    case 'query':
      return syncQuery(arg);
    case 'now':
      return performance.now();
    case 'timer': {
      const [id, delay] = JSON.parse(arg) as [number, number];
      if (typeof id !== 'number') return;
      if (timers.size >= config.maxTimers) {
        // Backstop: the shim should have thrown RangeError before asking. Reaching this means a shim bug (or a tampered shim).
        if (!timerLimitReported) { timerLimitReported = true; post({ type: 'diag', kind: 'timer-limit-enforced', detail: `glue refused a timer beyond maxTimers=${config.maxTimers}; the shim did not stop it` }); }
        return;
      }
      let ms = Math.min(Math.max(Number(delay) || 0, 0), MAX_TIMER_DELAY_MS);
      if (!visible) ms = Math.max(ms, 1000); // hidden: 1 Hz, like a background tab
      timers.set(id, setTimeout(() => onTimerFired(id), ms));
      return;
    }
    case 'clearTimer': {
      const id = Number(arg);
      const t = timers.get(id);
      if (t !== undefined) { clearTimeout(t); timers.delete(id); }
      return;
    }
    case 'raf':
      if (!visible) { frameWanted = true; return; } // one frame is requested when the widget becomes visible again
      post({ type: 'raf' });
      return;
    case 'log': {
      const i = arg.indexOf(':');
      post({ type: 'log', level: arg.slice(0, i), text: arg.slice(i + 1, i + 1 + MAX_LOG) });
      return;
    }
    case 'diag': {
      const i = arg.indexOf(':');
      post({ type: 'diag', kind: arg.slice(0, i).slice(0, 64), detail: arg.slice(i + 1, i + 1 + MAX_LOG) });
      return;
    }
    case 'listen':
      try { post({ type: 'listen', types: JSON.parse(arg) }); } catch {}
      return;
    case 'pd':
      try { post({ type: 'pd', keys: JSON.parse(arg) }); } catch {}
      return;
    case 'bridge':
      try {
        const [method, args] = JSON.parse(arg);
        post(activeGesture ? { type: 'bridge', method: String(method).slice(0, 64), args, gesture: activeGesture } : { type: 'bridge', method: String(method).slice(0, 64), args });
      } catch {}
      return;
    case 'intl':
      try { return formatIntl(JSON.parse(arg)); } catch { return ''; }
    case 'random': {
      const n = Math.min(Math.max(Number(arg) | 0, 0), 65536);
      return JSON.stringify(Array.from(crypto.getRandomValues(new Uint8Array(n))));
    }
    case 'stat': {
      // Counters maintained by the shim (advisory, shown in diagnostics only).
      try {
        const s = JSON.parse(arg);
        stats.queryCacheHits = Number(s.cacheHits) || 0;
        stats.missedMutations = Number(s.missed) || 0;
      } catch {}
      return;
    }
  }
}

// ───────────────────────── running code in the VM ─────────────────────────

function kill(reason: string) {
  if (dead) return;
  dead = true;
  for (const t of timers.values()) clearTimeout(t);
  timers.clear();
  deferred.length = 0;
  if (wakeTimer) { clearTimeout(wakeTimer); wakeTimer = null; }
  post({ type: 'dead', reason });
}

function describeError(err: QuickJSHandle): string {
  let text: string;
  try {
    const e = vm.dump(err);
    text = e && typeof e === 'object' ? `${e.name ?? 'Error'}: ${e.message ?? ''}${e.stack ? '\n' + e.stack : ''}` : String(e);
  } catch {
    text = 'unknown error';
  }
  err.dispose();
  return text;
}

function handleError(err: QuickJSHandle, where: string) {
  const text = describeError(err);
  if (interrupted) {
    // The slice is cpuLimitMs unless the CPU quota shrank it; say so, the fix differs (infinite loop vs. a widget that was already over budget).
    const quota = currentSliceMs < config.cpuLimitMs ? `; CPU quota: ${Math.max(0, config.cpuQuotaMs - budget.used(performance.now())).toFixed(0)} ms of ${config.cpuQuotaMs} ms per ${config.cpuQuotaWindowMs} ms left, so this ${currentKind} task got ${currentSliceMs.toFixed(0)} ms` : '';
    return kill(`CPU limit exceeded: more than ${currentSliceMs.toFixed(0)} ms of continuous execution (${where}${quota})`);
  }
  if (/out of memory/i.test(text)) return kill(`Memory limit exceeded (${config.memoryLimitBytes >> 20} MB)`);
  post({ type: 'log', level: 'error', text: `Uncaught ${text}`.slice(0, MAX_LOG) });
}

function drainJobs() {
  for (let i = 0; i < 100 && !dead; i++) {
    const r = rt.executePendingJobs();
    if (r.error) {
      handleError(r.error, 'promise job');
      continue;
    }
    if (r.value === 0) break;
  }
}

function callTw(kind: string, arg: string): string | null {
  const k = vm.newString(kind);
  const a = vm.newString(arg);
  const r = vm.callFunction(twDispatch, vm.undefined, k, a);
  k.dispose();
  a.dispose();
  if (r.error) {
    handleError(r.error, kind);
    return null;
  }
  const out = vm.typeof(r.value) === 'string' ? vm.getString(r.value) : null;
  r.value.dispose();
  return out;
}

/** End of a task: microtasks, flush mutations, observers; repeat until quiescent. */
function settle() {
  for (let i = 0; i < 20 && !dead; i++) {
    drainJobs();
    if (dead) return;
    const more = callTw('endTick', '');
    if (more !== '1' && !rt.hasPendingJob()) break;
  }
}

function evalScript(code: string, filename: string, type: 'global' | 'module'): boolean {
  const r = vm.evalCode(code, filename, { type });
  if (r.error) {
    handleError(r.error, filename);
    return false;
  }
  r.value.dispose();
  return true;
}

function fetchLibrary(file: string): string | null {
  let src = libCache.get(file);
  if (src === undefined) {
    // Synchronous same-origin request for a file bundled with the app (trusted
    // code path; the URL is never derived from widget input).
    const xhr = new XMLHttpRequest();
    xhr.open('GET', `/libs/${file}`, false);
    xhr.send();
    if (xhr.status !== 200) return null;
    src = xhr.responseText;
    libCache.set(file, src);
  }
  return src;
}

/** Run scripts queued by the shim (initial inline scripts, later dynamically inserted ones). */
function pumpScripts() {
  for (let i = 0; i < 1000 && !dead; i++) {
    const next = callTw('nextScript', '');
    if (!next) break;
    const s = JSON.parse(next) as { id: number; type: 'global' | 'module'; code?: string; src?: string };
    let ok = true;
    resetDeadline();
    if (s.src !== undefined) {
      const lib = resolveLibrary(s.src);
      const source = lib ? fetchLibrary(lib.file) : null;
      if (lib && source !== null) {
        const t0 = performance.now();
        evalScript(source, lib.file, 'global');
        post({ type: 'diag', kind: 'library', detail: `${s.src} → bundled ${lib.name} (eval ${(performance.now() - t0).toFixed(0)} ms)` });
      } else {
        ok = false;
        post({ type: 'diag', kind: 'script-skipped', detail: `external script not available: ${String(s.src).slice(0, 200)}` });
      }
    } else if (s.code !== undefined) {
      evalScript(s.code, `inline-script-${s.id}.js`, s.type);
    }
    if (dead) return;
    settle();
    callTw('scriptDone', JSON.stringify([s.id, ok]));
    settle();
  }
}

function resetDeadline() {
  interrupted = false;
  deadline = performance.now() + currentSliceMs;
}

/** One task of `kind` with `sliceMs` of continuous execution; its JS time is recorded against the quota. */
function task(kind: string, sliceMs: number, fn: () => void) {
  if (dead) return;
  inTask = true;
  currentKind = kind;
  currentSliceMs = sliceMs;
  excludeMs = 0;
  const t0 = performance.now();
  const q0 = stats.queryMs;
  resetDeadline();
  try {
    fn();
    if (!dead) settle();
    if (!dead) pumpScripts();
  } catch (e) {
    kill('sandbox glue error: ' + (e as Error).message);
  }
  inTask = false;
  stats.tasks++;
  const now = performance.now();
  const cpuMs = now - t0 - (stats.queryMs - q0) - excludeMs;
  stats.jsMs += cpuMs;
  budget.record(kind, cpuMs, now);
  stats.cpuWindowMs = budget.used(now);
  if (now - lastMemCheck > 1000 && !dead) {
    lastMemCheck = now;
    try {
      const h = rt.computeMemoryUsage();
      stats.memoryBytes = (vm.dump(h) as any).memory_used_size ?? 0;
      h.dispose();
    } catch {}
  }
  if (now - lastStatsPost > 200) {
    lastStatsPost = now;
    post({ type: 'stats', stats: { ...stats } });
  }
}

/**
 * Runs one task for an input (event, timer, frame, …) if the CPU quota allows,
 * otherwise defers it (returns false). Events and visibility always run.
 */
function dispatch(kind: string, arg: unknown): boolean {
  if (dead) return true;
  const now = performance.now();
  const d = budget.decide(kind, now);
  if (!d.run) {
    if (budget.shouldKill(now)) {
      kill(`CPU quota: throttled for ${(budget.throttledFor(now) / 1000).toFixed(1)} s (${config.cpuQuotaMs} ms per ${config.cpuQuotaWindowMs} ms, cpuKillAfterMs=${config.cpuKillAfterMs})`);
      return true;
    }
    defer(kind, arg);
    stats.throttledTasks++;
    throttledSinceDiag[kind] = (throttledSinceDiag[kind] ?? 0) + 1;
    if (now - lastThrottleDiag >= 1000) {
      lastThrottleDiag = now;
      const parts = Object.entries(throttledSinceDiag).map(([k, n]) => `${n} ${k}${n === 1 ? '' : 's'}`);
      for (const k of Object.keys(throttledSinceDiag)) delete throttledSinceDiag[k];
      post({ type: 'diag', kind: 'cpu-throttled', detail: `deferred ${parts.join(', ')}: ${budget.used(now).toFixed(0)} ms of the ${config.cpuQuotaMs} ms per ${config.cpuQuotaWindowMs} ms budget used` });
    }
    scheduleWake(d.retryAt);
    return false;
  }
  task(kind, d.sliceMs, () => callTw(kind, JSON.stringify(arg)));
  return true;
}

// ───────────────────────── boot ─────────────────────────

/**
 * Phase 1 (pool `warm`, or the first half of `init`): the QuickJS runtime, the
 * host functions and the sandbox bundle. `config` is provisional here (the
 * pool's limits); the widget and its real config come with `start`.
 */
async function warm(msg: Extract<HostToWorker, { type: 'warm' | 'init' }>) {
  config = msg.config;
  visible = config.visible !== false;
  budget = new CpuBudget(config);
  header = new Int32Array(msg.sab, 0, SAB_HEADER / 4);
  payload = new Uint8Array(msg.sab, SAB_HEADER);

  // The memory limit is enforced by the wasm linear memory's hard maximum, not
  // by `rt.setMemoryLimit` alone. quickjs-ng tracks its heap as the sum of
  // malloc_usable_size() over live blocks, and the emscripten build has no
  // usable-size implementation (cutils.h knows Apple, Windows, glibc, wasi: not
  // EMSCRIPTEN), so every allocation counts as 0 bytes. `setMemoryLimit` then
  // only refuses a single allocation larger than the limit; a widget keeping
  // 1 MB typed arrays grew the runtime to >1 GB (corpus/attacks/mem-*.html).
  // One runtime owns this Memory; when dlmalloc cannot grow it any further,
  // quickjs throws the same catchable `InternalError: out of memory` the native
  // builds throw at JS_SetMemoryLimit, and the footprint of this worker is
  // bounded by INITIAL + limit. Native glue (Swift, Kotlin, C++) needs nothing
  // extra: Apple malloc_size and bionic malloc_usable_size count correctly.
  // A pooled worker is warmed with the pool's limits: the Memory's maximum is fixed
  // here, so `start` can only lower the limit, never raise it past the warmed cap.
  warmedMemoryLimitBytes = config.memoryLimitBytes;
  const wasmMemory = new WebAssembly.Memory({ initial: WASM_INITIAL_PAGES, maximum: WASM_INITIAL_PAGES + Math.ceil(config.memoryLimitBytes / WASM_PAGE_BYTES) });
  const QuickJS = await newQuickJSWASMModuleFromVariant(newVariant(baseVariant, { wasmLocation: wasmUrl, wasmMemory }));
  rt = QuickJS.newRuntime();
  rt.setMemoryLimit(config.memoryLimitBytes); // still refuses one oversized allocation before dlmalloc even tries
  rt.setMaxStackSize(1024 * 1024);
  // No module loader is installed: static and dynamic import() of anything fails.
  rt.setInterruptHandler(() => {
    if (performance.now() > deadline) {
      interrupted = true;
      return true;
    }
    return false;
  });
  vm = rt.newContext();

  const hostFn = vm.newFunction('__host', (kindH, argH) => {
    const r = hostCall(vm.getString(kindH), argH ? vm.getString(argH) : '');
    if (typeof r === 'string') return vm.newString(r);
    if (typeof r === 'number') return vm.newNumber(r);
    return vm.undefined;
  });
  vm.setProp(vm.global, '__host', hostFn);
  hostFn.dispose();
  const binFn = vm.newFunction('__hostBinary', () => {
    if (!lastBinary) return vm.null;
    const copy = lastBinary.buffer.slice(lastBinary.byteOffset, lastBinary.byteOffset + lastBinary.byteLength) as ArrayBuffer;
    lastBinary = null;
    return vm.newArrayBuffer(copy);
  });
  vm.setProp(vm.global, '__hostBinary', binFn);
  binFn.dispose();

  let bootMs = 0;
  task('boot', config.cpuLimitMs, () => {
    const t0 = performance.now();
    // Generous one-off budget for evaluating our own bundle; its time is ours, not the widget's.
    deadline = performance.now() + Math.max(config.cpuLimitMs, 5000);
    const loaded = evalScript(msg.sandboxBundle, 'sandbox-bundle.js', 'global');
    excludeMs = performance.now() - t0;
    if (dead) return;
    // Fail closed: a runtime whose shim did not load completely (its network
    // stubs and DOM capture may be missing) never runs widget code.
    if (!loaded) return kill('sandbox bundle failed to load');
    const fn = vm.getProp(vm.global, '__twDispatch');
    if (vm.typeof(fn) !== 'function') {
      fn.dispose();
      return kill('sandbox bundle failed to initialise');
    }
    twDispatch = fn;
    bootMs = performance.now() - t0;
    post({ type: 'diag', kind: 'boot', detail: `sandbox bundle eval ${bootMs.toFixed(0)} ms; wasm memory ${(wasmMemory.buffer.byteLength / 1048576).toFixed(1)} MB, cap ${((WASM_INITIAL_PAGES * WASM_PAGE_BYTES + config.memoryLimitBytes) / 1048576).toFixed(0)} MB${QuickJS.getWasmMemory() === wasmMemory ? '' : ' (module ignored the provided Memory)'}` });
  });
  if (!dead) post({ type: 'warmed', bootMs });
}

/** Phase 2 (pool `start`, or the second half of `init`): the widget's HTML and its real config into the warmed VM. */
function start(msg: Extract<HostToWorker, { type: 'start' | 'init' }>) {
  if (dead) return;
  config = { ...msg.config, memoryLimitBytes: Math.min(msg.config.memoryLimitBytes, warmedMemoryLimitBytes) };
  visible = config.visible !== false;
  budget = new CpuBudget(config);
  rt.setMemoryLimit(config.memoryLimitBytes);
  task('boot', config.cpuLimitMs, () => callTw('boot', JSON.stringify({ html: msg.html, config })));
  // `task` has run all scripts by now.
  task('boot', config.cpuLimitMs, () => callTw('loaded', ''));
  if (!dead) post({ type: 'loaded' });
  post({ type: 'stats', stats: { ...stats } });
  ready = true;
  for (const m of queue.splice(0)) handle(m);
}

let ready = false;
let activeGesture = 0;

function handle(msg: HostToWorker) {
  if (dead) return;
  switch (msg.type) {
    case 'event':
      // The host's gesture number (trusted input only) rides along with bridge
      // calls made in this event's task; the host decides whether to honour them.
      activeGesture = typeof msg.gesture === 'number' ? msg.gesture : 0;
      try {
        dispatch('event', msg.ev);
      } finally {
        activeGesture = 0;
      }
      return;
    case 'frame':
      // A frame that arrives after the widget was hidden is dropped; the shim's callbacks wait for the next visible frame.
      if (!visible) { frameWanted = true; return; }
      dispatch('frame', msg.time);
      return;
    case 'viewport': dispatch('viewport', msg.viewport); return;
    case 'resize': dispatch('resize', msg.entries); return;
    case 'theme': dispatch('theme', msg.theme); return;
    case 'bridge': dispatch('bridge', [msg.event, msg.data]); return;
    case 'visibility': setVisible(!!msg.visible); return;
  }
}

let warming: Promise<void> | null = null;
const fail = (err: unknown) => kill('failed to start sandbox: ' + (err as Error).message);

self.onmessage = (e: MessageEvent<HostToWorker>) => {
  const msg = e.data;
  if (msg.type === 'init') {
    warming = warm(msg);
    warming.then(() => start(msg)).catch(fail);
  } else if (msg.type === 'warm') {
    warming = warm(msg);
    warming.catch(fail);
  } else if (msg.type === 'start') {
    // A `start` may arrive while the pool's worker is still warming: it waits for the bundle.
    (warming ?? Promise.reject(new Error('start before warm'))).then(() => start(msg)).catch(fail);
  } else if (!ready || inTask) {
    queue.push(msg);
  } else {
    handle(msg);
  }
};
