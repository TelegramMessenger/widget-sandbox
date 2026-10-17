// Dev UI (trusted host page). Loads corpus files or dropped files into a
// WidgetHost, shows diagnostics, and optionally the native reference render
// side by side. Exposes `window.__tw` for the Playwright suites.

import './style.css';
import { BlitzRenderer } from './renderers/blitz-renderer';
import { LitehtmlRenderer } from './renderers/litehtml-renderer';
import { ShadowDomRenderer } from '@telegram/widget-sandbox/host/shadow-renderer';
import { WidgetHost } from '@telegram/widget-sandbox/host/widget-host';
import { WorkerPool } from '@telegram/widget-sandbox/host/worker-pool';
import { WIDGET_FEATURES, WIDGET_POLICY } from './policy';
import type { FeedApi } from './feed';
import type { Stats } from '@telegram/widget-sandbox/shared/protocol';
import type { StorageCommit, WidgetAction } from '@telegram/widget-sandbox/shared/sdk-policy';

const $ = <T extends HTMLElement = HTMLElement>(sel: string) => document.querySelector(sel) as T;
const el = {
  corpus: $('#corpus'), drop: $('#drop'), file: $<HTMLInputElement>('#file'), title: $('#title'),
  dark: $<HTMLInputElement>('#dark'), reference: $<HTMLInputElement>('#reference'), blitz: $<HTMLInputElement>('#blitz'), litehtml: $<HTMLInputElement>('#litehtml'), shadow: $<HTMLInputElement>('#shadow'), compare: $<HTMLInputElement>('#compare'), feed: $<HTMLInputElement>('#feed'), reload: $('#reload'),
  app: $('#app'), feedView: $('#feed-view'),
  sandboxWrap: $('#sandbox-wrap'), referenceWrap: $('#reference-wrap'), chatReference: $('#chat-reference'), compareChats: $('#compare-chats'), stage: $('.stage'),
  status: $('#status'), stats: $('#stats'), diags: $('#diags'), bridge: $('#bridge'), console: $('#console'),
  widgetError: $('#widget-error'),
};

export interface TwState {
  file: string | null;
  html: string;
  theme: 'light' | 'dark';
  loaded: boolean;
  dead: string | null;
  diags: Record<string, { count: number; details: string[] }>;
  logs: { level: string; text: string }[];
  bridge: { method: string; args: unknown[] }[];
  /** What the widget asked the client to do (SDK v1 `onAction`), in order. */
  actions: WidgetAction[];
  /** Gesture claims of the primary pointerdowns (`onGestureClaim`), in order. */
  gestures: { x: boolean; y: boolean }[];
  stats: (Stats & { nodes: number }) | null;
  height: number;
  /** Last uncaught widget error, shown under the bubble until a later task completes without one. */
  lastError: string | null;
}

const state: TwState = { file: null, html: '', theme: 'light', loaded: false, dead: null, diags: {}, logs: [], bridge: [], actions: [], gestures: [], stats: null, height: 0, lastError: null };
/** `?locale=ru`: the client's UI language passed to the widget (navigator.language, the Intl default); en-US otherwise. */
const LOCALE = new URLSearchParams(location.search).get('locale') ?? undefined;

// Web Storage as a client keeps it (docs/sdk-v1.md §5.4), per file: `local` in this page's localStorage (it survives a
// reload, as it survives an app restart), `session` in memory. `__tw.restart()` and reopening the file get it back.
const sessionAreas = new Map<string, Record<string, string>>();
const storageKey = (file: string) => 'tw-widget-storage:' + file;
function storageFor(file: string | null): StorageCommit | undefined {
  if (!file) return undefined;
  let local: Record<string, string> = {};
  try { local = JSON.parse(localStorage.getItem(storageKey(file)) ?? '{}'); } catch {}
  return { local, session: sessionAreas.get(file) ?? {} };
}
function commitStorage(file: string | null, area: 'local' | 'session', values: Record<string, string>) {
  if (!file) return;
  if (area === 'session') sessionAreas.set(file, values);
  else try { localStorage.setItem(storageKey(file), JSON.stringify(values)); } catch {}
}
function clearStorage(file: string | null) {
  if (!file) return;
  sessionAreas.delete(file);
  try { localStorage.removeItem(storageKey(file)); } catch {}
}
let tasksAtError = 0;
/** Runtime limits overridable from the URL for the Playwright suites (?cpuQuotaMs=…&cpuKillAfterMs=…&maxTimers=…). */
const LIMIT_PARAMS = ['cpuLimitMs', 'cpuQuotaMs', 'cpuQuotaWindowMs', 'cpuKillAfterMs', 'maxTimers'] as const;
const limits: Partial<Record<(typeof LIMIT_PARAMS)[number], number>> = {};
let host: WidgetHost | null = null;
/** `?pool=N`: sandbox workers from a pool of N warmed ones (worker-pool.ts), as a feed would do; prewarmed at init. */
let workerPool: WorkerPool | null = null;
/** `?feed=1`: the chat feed demo (src/feed.ts) takes over the centre column; the single-widget panels are hidden and `host` stays null. */
const FEED_MODE = new URLSearchParams(location.search).get('feed') === '1';
let feed: FeedApi | null = null;
/** Compare mode: the renderers the main bubble is not using, one WidgetHost each (same HTML, same limits). */
let compareHosts: WidgetHost[] = [];
let referenceFrame: HTMLIFrameElement | null = null;
/** Off-screen pause: the bubble's intersection with the viewport, combined with the page's own visibility. `forcedVisible` (tests) overrides both. */
let intersecting = true;
let forcedVisible: boolean | null = null;
const visibilityObserver = new IntersectionObserver((entries) => { intersecting = entries[entries.length - 1].isIntersecting; applyVisibility(); });
function applyVisibility() {
  host?.setVisible(forcedVisible ?? (intersecting && !document.hidden));
}
document.addEventListener('visibilitychange', applyVisibility);
let referenceTimer = 0;
let loadWaiters: (() => void)[] = [];

// ───────────── rendering of the panels ─────────────

// Panels are re-rendered at most once per frame: a misbehaving widget can emit
// thousands of diagnostics per second, and per-event DOM rebuilds froze the page.
const pending = new Set<() => void>();
let frame = 0;
function coalesce(fn: () => void) {
  pending.add(fn);
  if (!frame) frame = requestAnimationFrame(() => { frame = 0; const fns = [...pending]; pending.clear(); for (const f of fns) f(); });
}

function renderStatus() {
  el.status.textContent = state.dead ? `✖ sandbox terminated: ${state.dead}` : state.loaded ? '● loaded' : host ? '… loading' : '—';
  el.status.className = state.dead ? 'dead' : state.loaded ? 'ok' : '';
  // What a product would show under the bubble: the widget stays on screen (frozen) with a one-line reason.
  const line = state.dead ? `Widget stopped: ${state.dead.split('\n')[0]}` : state.lastError ? state.lastError.split('\n')[0] : '';
  el.widgetError.textContent = line;
  el.widgetError.classList.toggle('hidden', !line);
}
function renderStats() {
  const s = state.stats;
  if (!s) { el.stats.textContent = ''; return; }
  const rows: [string, string][] = [
    ['JS time', `${s.jsMs.toFixed(0)} ms in ${s.tasks} tasks`],
    ['memory', `${(s.memoryBytes / 1024 / 1024).toFixed(1)} MB`],
    ['ops sent', String(s.ops)],
    ['nodes', String(s.nodes)],
    ['sync queries', `${s.queries} (${s.queryMs.toFixed(0)} ms, avg ${s.queries ? (s.queryMs / s.queries).toFixed(2) : '0'} ms), cache hits ${s.queryCacheHits}`],
    ['missed mutations', String(s.missedMutations)],
    ['CPU quota', `${s.cpuWindowMs.toFixed(0)} ms used in the current window, ${s.throttledTasks} tasks deferred`],
    ['bubble height', `${state.height} px`],
  ];
  el.stats.replaceChildren(...rows.map(([k, v]) => {
    const tr = document.createElement('tr');
    const th = document.createElement('th'); th.textContent = k;
    const td = document.createElement('td'); td.textContent = v;
    tr.append(th, td);
    return tr;
  }));
}
function renderDiags() {
  el.diags.replaceChildren(...Object.entries(state.diags).map(([kind, d]) => {
    const li = document.createElement('li');
    li.className = 'diag diag-' + kind;
    const b = document.createElement('b'); b.textContent = `${kind} ×${d.count}`;
    const span = document.createElement('span'); span.textContent = d.details.slice(-3).join(' · ');
    li.append(b, ' ', span);
    li.title = d.details.join('\n');
    return li;
  }));
}
function renderBridge() {
  el.bridge.replaceChildren(...state.bridge.slice(-20).map((b) => {
    const li = document.createElement('li');
    li.textContent = `${b.method}(${b.args.map((a) => JSON.stringify(a)).join(', ')})`;
    return li;
  }));
}
function renderConsole() {
  el.console.replaceChildren(...state.logs.slice(-300).map((l) => {
    const div = document.createElement('div');
    div.className = 'log log-' + l.level;
    div.textContent = `[${l.level}] ${l.text}`;
    return div;
  }));
  el.console.scrollTop = el.console.scrollHeight;
}

// ───────────── loading widgets ─────────────

function load(html: string, name: string | null) {
  host?.destroy();
  host = null;
  Object.assign(state, { file: name, html, loaded: false, dead: null, diags: {}, logs: [], bridge: [], actions: [], gestures: [], stats: null, height: 0, lastError: null });
  tasksAtError = 0;
  // Showcase widgets own the whole bubble (edge to edge, the bubble clips); the older corpora are cards in a padded bubble (style.css .bubble.edge).
  for (const b of document.querySelectorAll('.stage .bubble')) b.classList.toggle('edge', isEdge(name));
  el.title.textContent = name ?? 'dropped file';
  el.sandboxWrap.replaceChildren();
  renderStatus(); renderStats(); renderDiags(); renderBridge(); renderConsole();
  host = new WidgetHost(el.sandboxWrap, html, {
    policy: WIDGET_POLICY,
    features: WIDGET_FEATURES,
    theme: state.theme,
    locale: LOCALE,
    storage: storageFor(name),
    ...limits,
    // Blitz / litehtml (dev tooling): a native renderer compiled to wasm instead of the sandboxed iframe; sandbox and validator unchanged.
    // Shadow DOM (prototype): the same renderer core on a shadow root in this page, no iframe.
    rendererFactory: el.litehtml.checked ? LitehtmlRenderer.factory : el.blitz.checked ? BlitzRenderer.factory : el.shadow.checked ? ShadowDomRenderer.factory : undefined,
    workerPool: workerPool ?? undefined,
    onLog: (level, text) => { if (state.logs.length < 5000) state.logs.push({ level, text }); coalesce(renderConsole); },
    onError: (text) => { state.lastError = text; tasksAtError = state.stats?.tasks ?? 0; coalesce(renderStatus); },
    onDiag: (kind, detail) => {
      const d = (state.diags[kind] ??= { count: 0, details: [] });
      d.count++;
      if (d.details.length < 50) d.details.push(detail);
      coalesce(renderDiags);
    },
    onStats: (s) => {
      state.stats = s;
      if (state.lastError && s.tasks > tasksAtError + 1) { state.lastError = null; coalesce(renderStatus); }
      coalesce(renderStats);
    },
    onBridge: (method, args) => { state.bridge.push({ method, args }); coalesce(renderBridge); },
    onAction: (action) => { state.actions.push(action); },
    onStorageCommit: (area, values) => commitStorage(name, area, values),
    onGestureClaim: (claim) => { if (state.gestures.length < 1000) state.gestures.push(claim); },
    onDead: (reason) => { state.dead = reason; renderStatus(); settle(); },
    onLoaded: () => { state.loaded = true; renderStatus(); settle(); },
    onResize: (h) => { state.height = h; coalesce(renderStats); },
  });
  visibilityObserver.disconnect();
  intersecting = true;
  visibilityObserver.observe(host.element);
  applyVisibility();
  updateCompare();
  updateReference();
  syncUrl();
}

type RendererKind = 'iframe' | 'shadow' | 'blitz' | 'litehtml';
const RENDERERS: { kind: RendererKind; title: string; factory?: typeof BlitzRenderer.factory }[] = [
  { kind: 'iframe', title: 'sandbox · iframe renderer (product)' },
  { kind: 'shadow', title: 'sandbox · Shadow DOM renderer (no iframe, prototype)', factory: ShadowDomRenderer.factory },
  { kind: 'blitz', title: 'sandbox · Blitz renderer (wasm, dev tooling)', factory: BlitzRenderer.factory },
  { kind: 'litehtml', title: 'sandbox · litehtml renderer (wasm, dev tooling)', factory: LitehtmlRenderer.factory },
];
function mainRenderer(): RendererKind { return el.litehtml.checked ? 'litehtml' : el.blitz.checked ? 'blitz' : el.shadow.checked ? 'shadow' : 'iframe'; }

/** Compare mode (dev tooling): the other two renderers next to the main bubble, each on its own sandbox from the same
 * HTML, plus the native reference. The right-hand panels keep describing the main bubble; each extra column shows its
 * own one-line status (loaded / dead, the renderer's paint line). The sandboxes boot one after another — three QuickJS
 * workers starting at once contend for the CPU and the boot slice (cpuLimitMs) kills them. */
let compareGeneration = 0;
function updateCompare() {
  compareGeneration++;
  for (const h of compareHosts) h.destroy();
  compareHosts = [];
  el.compareChats.replaceChildren();
  const on = el.compare.checked && !!state.html;
  el.stage.classList.toggle('compare', on);
  if (!on) return;
  const generation = compareGeneration;
  const html = state.html;
  const columns = RENDERERS.filter((r) => r.kind !== mainRenderer()).map((r) => {
    const chat = document.createElement('div');
    chat.className = 'chat';
    chat.dataset.renderer = r.kind;
    const head = document.createElement('div');
    head.className = 'bubble-head';
    head.textContent = r.title;
    const bubble = document.createElement('div');
    bubble.className = 'bubble' + (isEdge(state.file) ? ' edge' : '');
    const wrap = document.createElement('div');
    wrap.className = 'frame-wrap';
    bubble.appendChild(wrap);
    const status = document.createElement('div');
    status.className = 'compare-status';
    status.textContent = '… waiting for the previous sandbox to boot';
    chat.append(head, bubble, status);
    el.compareChats.appendChild(chat);
    return { r, wrap, status };
  });
  let i = 0;
  const startNext = () => {
    if (generation !== compareGeneration || i >= columns.length) return;
    const { r, wrap, status } = columns[i++];
    const t0 = performance.now();
    const lines: Record<string, string> = { status: '… loading' };
    const show = () => { status.textContent = Object.entries(lines).map(([k, v]) => `${k}: ${v}`).join('\n'); };
    show();
    let settled = false;
    const settle = () => { if (!settled) { settled = true; startNext(); } };
    // `current` (what __tw.tap / __tw.blitz / __tw.litehtml talk to) stays the main bubble's renderer.
    const mainBlitz = BlitzRenderer.current, mainLitehtml = LitehtmlRenderer.current;
    const h = new WidgetHost(wrap, html, {
      policy: WIDGET_POLICY,
      features: WIDGET_FEATURES,
      theme: state.theme,
      ...limits,
      rendererFactory: r.factory,
      onLoaded: () => { lines.status = `loaded in ${(performance.now() - t0).toFixed(0)} ms`; coalesce(show); settle(); },
      onDead: (reason) => { lines.status = `dead: ${reason.split('\n')[0]}`; coalesce(show); settle(); },
      onError: (text) => { lines.error = text.split('\n')[0]; coalesce(show); },
      onDiag: (kind, detail) => { if (kind === 'renderer' || /-(init|paint|build)$/.test(kind)) { lines[kind] = detail; coalesce(show); } },
    });
    BlitzRenderer.current = mainBlitz;
    LitehtmlRenderer.current = mainLitehtml;
    compareHosts.push(h);
    setTimeout(settle, 15_000);
  };
  ready().then(startNext);
}

function settle() {
  const w = loadWaiters;
  loadWaiters = [];
  for (const f of w) f();
}

/** Resolves once the current widget has finished loading (or died). */
function ready(): Promise<void> {
  if (state.loaded || state.dead) return Promise.resolve();
  return new Promise((r) => loadWaiters.push(r));
}

/** corpus/showcase widgets are edge-to-edge bubbles (no padding, clipped by the bubble's radius); the other groups are cards inside a padded bubble. */
export const isEdge = (file: string | null) => !!file && file.startsWith('showcase/');

async function openFile(rel: string) {
  const res = await fetch('/corpus/' + rel);
  if (!res.ok) throw new Error('no such corpus file: ' + rel);
  load(await res.text(), rel);
  for (const li of el.corpus.children) li.classList.toggle('active', (li as HTMLElement).dataset.file === rel);
}

// Reference mode (dev tooling): native render of a repo corpus file in a
// sandboxed iframe served with connect-src 'none'. Never used for attacks.
function updateReference() {
  // compare mode shows the reference too (never for attacks: reference mode has no sandbox)
  const on = (el.reference.checked || el.compare.checked) && state.file && !state.file.startsWith('attacks/');
  el.chatReference.classList.toggle('hidden', !on);
  referenceFrame?.remove();
  referenceFrame = null;
  clearInterval(referenceTimer);
  if (!on) return;
  const f = document.createElement('iframe');
  f.className = 'widget-frame';
  // allow-same-origin only so that the dev UI can read the document height;
  // allow-forms so that `submit` events fire, like in the product renderer.
  f.setAttribute('sandbox', 'allow-scripts allow-same-origin allow-forms');
  f.style.colorScheme = state.theme;
  f.src = '/reference/' + state.file;
  el.referenceWrap.replaceChildren(f);
  referenceFrame = f;
  const size = () => {
    try {
      const d = f.contentDocument;
      if (d?.documentElement) f.style.height = Math.min(Math.max(Math.ceil(d.documentElement.getBoundingClientRect().height), 24), 900) + 'px';
    } catch {}
  };
  // Same-origin frame: size it from inside (ResizeObserver) plus a slow fallback poll.
  f.addEventListener('load', () => {
    size();
    try {
      const d = f.contentDocument!;
      new (f.contentWindow as any).ResizeObserver(size).observe(d.documentElement);
      new (f.contentWindow as any).ResizeObserver(size).observe(d.body);
    } catch {}
  });
  referenceTimer = window.setInterval(size, 1000);
}

function setTheme(theme: 'light' | 'dark') {
  state.theme = theme;
  el.dark.checked = theme === 'dark';
  document.documentElement.dataset.theme = theme;
  host?.setTheme(theme);
  for (const h of compareHosts) h.setTheme(theme);
  if (referenceFrame) referenceFrame.style.colorScheme = theme;
  feed?.setTheme(theme);
  syncUrl();
}

function syncUrl() {
  if (FEED_MODE) return; // the feed owns the URL (its own switches, plus the theme)
  const p = new URLSearchParams();
  if (state.file) p.set('file', state.file);
  if (state.theme === 'dark') p.set('theme', 'dark');
  if (el.reference.checked) p.set('reference', '1');
  if (el.blitz.checked) p.set('blitz', '1');
  if (el.litehtml.checked) p.set('litehtml', '1');
  if (el.shadow.checked) p.set('shadow', '1');
  if (el.compare.checked) p.set('compare', '1');
  for (const k of LIMIT_PARAMS) if (limits[k] !== undefined) p.set(k, String(limits[k]));
  if (workerPool) p.set('pool', String(workerPool.size));
  history.replaceState(null, '', '?' + p.toString());
}

// ───────────── wiring ─────────────

async function init() {
  const files: string[] = await (await fetch('/api/corpus')).json();
  el.corpus.replaceChildren(...files.map((f) => {
    const li = document.createElement('li');
    li.textContent = f;
    li.dataset.file = f;
    li.addEventListener('click', () => openFile(f));
    return li;
  }));
  const params = new URLSearchParams(location.search);
  for (const k of LIMIT_PARAMS) if (params.has(k) && Number.isFinite(Number(params.get(k)))) limits[k] = Number(params.get(k));
  if (FEED_MODE) {
    // Feed demo: the theme toggle and the feed checkbox are the only toolbar controls left; everything else is per bubble inside the feed.
    el.feed.checked = true;
    el.app.classList.add('feed-mode');
    el.feedView.classList.remove('hidden');
    setTheme(params.get('theme') === 'dark' ? 'dark' : 'light');
    const { startFeed } = await import('./feed');
    feed = startFeed(el.feedView, { theme: state.theme, params });
    (window as any).__tw.feed = feed;
    return;
  }
  const poolSize = Number(params.get('pool') ?? 0);
  if (poolSize > 0) {
    workerPool = new WorkerPool(poolSize);
    await workerPool.prewarm();
  }
  el.reference.checked = params.get('reference') === '1';
  el.blitz.checked = params.get('blitz') === '1';
  el.litehtml.checked = params.get('litehtml') === '1';
  el.shadow.checked = params.get('shadow') === '1';
  el.compare.checked = params.get('compare') === '1';
  setTheme(params.get('theme') === 'dark' ? 'dark' : 'light');
  const file = params.get('file');
  if (file && files.includes(file)) await openFile(file);
}

el.dark.addEventListener('change', () => setTheme(el.dark.checked ? 'dark' : 'light'));
el.reference.addEventListener('change', () => { updateReference(); syncUrl(); });
el.compare.addEventListener('change', () => { updateCompare(); updateReference(); syncUrl(); });
// The two native renderers are exclusive: ticking one unticks the other.
el.blitz.addEventListener('change', () => { if (el.blitz.checked) { el.litehtml.checked = false; el.shadow.checked = false; } if (state.html) load(state.html, state.file); else syncUrl(); });
el.litehtml.addEventListener('change', () => { if (el.litehtml.checked) { el.blitz.checked = false; el.shadow.checked = false; } if (state.html) load(state.html, state.file); else syncUrl(); });
el.shadow.addEventListener('change', () => { if (el.shadow.checked) { el.blitz.checked = false; el.litehtml.checked = false; } if (state.html) load(state.html, state.file); else syncUrl(); });
el.reload.addEventListener('click', () => { if (state.html) load(state.html, state.file); });
// The feed is a different app mode: a reload with `?feed=1` (keeping the theme), or back to the single-widget UI.
el.feed.addEventListener('change', () => { location.search = el.feed.checked ? `?feed=1${state.theme === 'dark' ? '&theme=dark' : ''}` : state.theme === 'dark' ? '?theme=dark' : ''; });
el.drop.addEventListener('click', () => el.file.click());
el.file.addEventListener('change', async () => {
  const f = el.file.files?.[0];
  if (f) load(await f.text(), null);
});
el.drop.addEventListener('dragover', (e) => { e.preventDefault(); el.drop.classList.add('over'); });
el.drop.addEventListener('dragleave', () => el.drop.classList.remove('over'));
el.drop.addEventListener('drop', async (e) => {
  e.preventDefault();
  el.drop.classList.remove('over');
  const f = e.dataTransfer?.files[0];
  if (f) load(await f.text(), null);
});

(window as any).__tw = {
  state,
  load: (html: string, name: string | null = null) => load(html, name),
  openFile,
  ready,
  setTheme,
  /** Re-create the WidgetHost from the same HTML (what a "Restart" action in a product would do). */
  restart: () => { if (state.html) load(state.html, state.file); },
  /** Force the widget's visibility (null = back to the IntersectionObserver / page visibility). */
  setVisible: (v: boolean | null) => { forcedVisible = v; applyVisibility(); },
  /** The Web Storage the dev app keeps for a file (`local` survives a page reload), and forgetting it. */
  storage: (file: string | null = state.file) => storageFor(file),
  clearStorage: (file: string | null = state.file) => clearStorage(file),
  get host() { return host; },
  /** Native renderers only: a tap at CSS px inside the bubble, hit-tested by the native document (the runners cannot click into a canvas). */
  tap: (x: number, y: number) => { const r = BlitzRenderer.current ?? LitehtmlRenderer.current; if (r) { r.touch(0, x, y); r.touch(2, x, y); } },
  /** Blitz renderer only: paint statistics. */
  get blitz() { return BlitzRenderer.current?.stats ?? null; },
  /** litehtml renderer only: paint statistics. */
  get litehtml() { return LitehtmlRenderer.current?.stats ?? null; },
  /** Compare mode: the extra hosts (the renderers the main bubble is not using). */
  get compareHosts() { return compareHosts; },
  /** `?pool=N`: the worker pool's counters. */
  get pool() { return workerPool ? { size: workerPool.size, warm: workerPool.warm, ...workerPool.stats } : null; },
  /** `?feed=1`: the chat feed demo's API (src/feed.ts); null otherwise. */
  feed: null as FeedApi | null,
};

init().catch((e) => { el.status.textContent = String(e); });
