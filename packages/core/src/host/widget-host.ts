// WidgetHost: one widget instance = one worker (QuickJS) + one validator + one
// renderer (the sandboxed iframe unless a `rendererFactory` supplies another,
// see renderer.ts). Trusted. This is the only place where the three meet:
//
//   worker ──(untrusted ops/queries)──▶ Validator ──(RenderOps)──▶ renderer
//   worker ◀──(events, frames, query results)────────────────────── renderer
//
// Nothing from the worker is forwarded to the renderer without passing through
// the Validator.

import { finiteNumber, isTypedMessage } from './message-guards';
import sandboxBundle from 'virtual:sandbox-bundle';
import { IframeRenderer } from './iframe-renderer';
import { GestureCounter, HostActivationGate } from './user-activation';
import {
  DEFAULT_LOCALE, HapticGate, PRIVILEGED_METHODS, checkBridgeCall, checkLocale, checkStorageCommit, toWidgetTheme,
  type StorageCommit, type WidgetAction, type WidgetTheme,
} from '../shared/sdk-policy';
import { PROFILE_VERSION } from '../shared/profile-tables';
import type { Renderer, RendererFactory } from './renderer';
import type { PooledWorker, WorkerPool } from './worker-pool';
import { Validator } from './validator';
import {
  LIMIT_DEFAULTS, SAB_BIN_LEN, SAB_HEADER, SAB_JSON_LEN, SAB_SIZE, SAB_STATE,
  type HostToRenderer, type HostToWorker, type RendererToHost, type SandboxConfig, type SandboxPolicy, type Stats, type WorkerToHost, resolvePolicy,
} from '../shared/protocol';

export type { WidgetAction, WidgetTheme } from '../shared/sdk-policy';

export interface WidgetHostOptions {
  /** The chat's theme (docs/sdk-v1.md §2.3, §2.4); a plain colour scheme is still accepted. */
  theme: 'light' | 'dark' | WidgetTheme;
  /** The client's UI language, BCP 47 (`navigator.language`, the Intl default); `en-US` when absent or malformed. */
  locale?: string;
  /** Web Storage of this message as the client keeps it (docs/sdk-v1.md §5.4); over the quota it is dropped whole (`storage-rejected`). */
  storage?: StorageCommit;
  memoryLimitBytes?: number;
  cpuLimitMs?: number;
  cpuQuotaMs?: number;
  cpuQuotaWindowMs?: number;
  cpuKillAfterMs?: number;
  maxTimers?: number;
  policy?: SandboxPolicy;
  /** Initial visibility; change it later with `setVisible`. */
  visible?: boolean;
  onLog?: (level: string, text: string) => void;
  onDiag?: (kind: string, detail: string) => void;
  onStats?: (stats: Stats & { nodes: number }) => void;
  /** What the widget asked the client to do, checked (URL rules, user activation, the haptic rate limit). */
  onAction?: (action: WidgetAction) => void;
  /** The new full content of a Web Storage area, at the end of each task that changed it. */
  onStorageCommit?: (area: 'local' | 'session', values: Record<string, string>) => void;
  /** On every primary pointerdown: whether the widget takes horizontal (`x`) and vertical (`y`) drags of this touch. */
  onGestureClaim?: (claim: { x: boolean; y: boolean }) => void;
  /** @deprecated since v1: `onAction`. The raw method and arguments of each call `onAction` gets. */
  onBridge?: (method: string, args: unknown[]) => void;
  /** Fatal: the runtime is gone. The renderer keeps the last state (frozen); call `destroy()` to remove it. */
  onDead?: (reason: string) => void;
  /** Non-fatal uncaught exception in widget code (also in the log); for showing an error line under the bubble. */
  onError?: (text: string) => void;
  /** Badge text shown by the renderer over a dead widget (user-facing, no error details). */
  stoppedLabel?: string;
  onLoaded?: () => void;
  onResize?: (height: number) => void;
  /** The renderer; the sandboxed iframe when omitted. */
  rendererFactory?: RendererFactory;
  /** Take the sandbox worker from a pool of warmed ones (worker-pool.ts) instead of booting a fresh one. */
  workerPool?: WorkerPool;
  /**
   * Opt-in sandbox features (docs/protocol.md, "Features"). The web runtime honours
   * `webAnimations`, `noInPlaceText`, `noAttrFast` and `noCanvasRect` (`WEB_FEATURES` in
   * worker.ts); the native-only wire forms (`canvasWords`, `binaryOps`, `morphUpdates`) are dropped.
   */
  features?: string[];
}

const MIN_HEIGHT = 24;
const MAX_HEIGHT = 900;
const encoder = new TextEncoder();

export class WidgetHost {
  /** The renderer's element in the bubble (an iframe for the product renderer, a canvas for Blitz). */
  readonly element: HTMLElement;
  /** The product renderer's frame; null for other renderers (Playwright suites look inside it). */
  get iframe(): HTMLIFrameElement | null { return this.element instanceof HTMLIFrameElement ? this.element : null; }
  private worker: Worker | null = null;
  private renderer: Renderer;
  private validator: Validator;
  private sab: SharedArrayBuffer;
  private header: Int32Array;
  private payload: Uint8Array;
  private qid = 0;
  private pendingQuery = 0;
  private destroyed = false;
  private dead = false;
  private viewport: SandboxConfig['viewport'] | undefined;
  private theme: 'light' | 'dark';
  private widgetTheme: ReturnType<typeof toWidgetTheme>;
  private visible: boolean;
  private policy: SandboxPolicy;
  private gestures = new GestureCounter();
  private activation = new HostActivationGate();
  private blockedBridgeReported = 0;
  private rejectedBridgeReported = 0;
  private rejectedStorageReported = 0;
  private haptics = new HapticGate();

  constructor(container: HTMLElement, html: string, private opts: WidgetHostOptions) {
    const diag = (kind: string, detail: string) => opts.onDiag?.(kind, detail);
    this.widgetTheme = toWidgetTheme(opts.theme);
    this.theme = this.widgetTheme.colorScheme;
    this.visible = opts.visible ?? true;
    // ── renderer ──
    this.renderer = (opts.rendererFactory ?? IframeRenderer.factory)(container, this.theme);
    this.element = this.renderer.element;
    this.validator = new Validator(diag, { colorScheme: this.theme, scope: this.renderer.cssScope ?? 'document' });
    this.policy = resolvePolicy(opts.policy);
    this.validator.setPolicy(this.policy);
    this.renderer.onMessage = (msg) => { if (isTypedMessage(msg)) this.onRenderer(msg as RendererToHost); };
    this.renderer.onDiag = diag;
    this.renderer.load();
    this.toRenderer({ type: 'theme', theme: this.widgetTheme });

    // ── sandbox ──
    // The worker boots in parallel with the renderer: a renderer that is not ready yet queues what it gets.
    // A synchronous renderer (shadow DOM) has already reported its size by now; the iframe has not.
    this.viewport ??= { innerWidth: this.element.clientWidth || 400, innerHeight: this.element.clientHeight || 300, dpr: window.devicePixelRatio };
    const config: SandboxConfig = {
      memoryLimitBytes: opts.memoryLimitBytes ?? LIMIT_DEFAULTS.memoryLimitBytes,
      cpuLimitMs: opts.cpuLimitMs ?? LIMIT_DEFAULTS.cpuLimitMs,
      cpuQuotaMs: opts.cpuQuotaMs ?? LIMIT_DEFAULTS.cpuQuotaMs,
      cpuQuotaWindowMs: opts.cpuQuotaWindowMs ?? LIMIT_DEFAULTS.cpuQuotaWindowMs,
      cpuKillAfterMs: opts.cpuKillAfterMs ?? LIMIT_DEFAULTS.cpuKillAfterMs,
      maxTimers: opts.maxTimers ?? LIMIT_DEFAULTS.maxTimers,
      visible: opts.visible ?? true,
      policy: this.policy,
      features: opts.features ?? [],
      viewport: this.viewport,
      theme: this.theme,
      themeParams: this.widgetTheme.params,
      locale: checkLocale(opts.locale) ?? DEFAULT_LOCALE,
      profile: PROFILE_VERSION,
      storage: this.preload(opts.storage),
    };
    let pooled: PooledWorker | null = null;
    if (opts.workerPool) {
      // A warmed worker: QuickJS and the bundle are already there (or on their way), only the widget is left to load.
      pooled = opts.workerPool.take();
      this.worker = pooled.worker;
      this.sab = pooled.sab;
    } else {
      this.worker = new Worker(new URL('./worker.ts', import.meta.url), { type: 'module' });
      this.sab = new SharedArrayBuffer(SAB_SIZE);
    }
    this.header = new Int32Array(this.sab, 0, SAB_HEADER / 4);
    this.payload = new Uint8Array(this.sab, SAB_HEADER);
    this.worker.onmessage = (e) => { if (isTypedMessage(e.data)) this.onWorker(e.data as WorkerToHost); };
    this.worker.onerror = (e) => this.die('worker error: ' + e.message);
    // What the pooled worker said while idle (the shim's document skeleton) comes first, in order.
    if (pooled) for (const m of pooled.pending.splice(0)) this.onWorker(m);
    if (opts.workerPool) this.toWorker({ type: 'start', html, config });
    else this.toWorker({ type: 'init', html, sab: this.sab, config, sandboxBundle });
  }

  setTheme(theme: 'light' | 'dark' | WidgetTheme) {
    // The host owns the theme: stylesheets are re-sanitized against the scheme
    // (prefers-color-scheme, color-scheme, light-dark()), matchMedia queries
    // are answered from it, the renderer gets the variables, and the sandbox
    // gets the scheme and the palette.
    this.widgetTheme = toWidgetTheme(theme);
    this.theme = this.widgetTheme.colorScheme;
    this.renderer.setTheme(this.theme);
    this.toRenderer({ type: 'theme', theme: this.widgetTheme });
    const ops = this.validator.setTheme(this.theme);
    if (ops.length) this.toRenderer({ type: 'ops', ops });
    this.toWorker({ type: 'theme', theme: this.theme, themeParams: this.widgetTheme.params });
  }

  private preload(storage: unknown): StorageCommit {
    if (storage === undefined) return {};
    const checked = checkStorageCommit(storage);
    if (checked === null) this.opts.onDiag?.('storage-rejected', `the preload is not { local, session } objects of strings within 16384 bytes each; the widget starts with empty storage`);
    return checked ?? {};
  }

  /**
   * The host knows whether the bubble is on screen; the sandbox does not.
   * Hidden: no animation frames, timers at 1 Hz, `document.hidden` /
   * `visibilitychange` inside the widget. Visible again: deferred timers fire,
   * one frame is requested if the widget asked for one meanwhile.
   */
  setVisible(visible: boolean) {
    if (this.visible === visible) return;
    this.visible = visible;
    this.toWorker({ type: 'visibility', visible });
  }

  destroy() {
    if (this.destroyed) return;
    this.destroyed = true;
    this.worker?.terminate();
    this.renderer.destroy();
  }

  private toWorker(msg: HostToWorker) {
    // Before the worker exists (a renderer reporting its size synchronously) nothing is lost: the config carries the viewport.
    if (!this.destroyed) this.worker?.postMessage(msg);
  }
  private toRenderer(msg: HostToRenderer) {
    if (!this.destroyed) this.renderer.send(msg);
  }

  private die(reason: string) {
    if (this.dead) return;
    this.dead = true;
    this.opts.onDead?.(reason);
    // The renderer keeps showing the last state (a static snapshot); the VM is
    // gone. `frozen` tells the renderer to stop forwarding events and frames
    // and to show the badge; the owner removes the widget with destroy().
    this.worker?.terminate();
    this.answerQuery({ error: 'dead' });
    this.toRenderer({ type: 'frozen', reason: this.opts.stoppedLabel ?? 'Widget stopped', fatal: true });
  }

  // ───────────── worker → host ─────────────

  private onWorker(msg: WorkerToHost) {
    switch (msg.type) {
      case 'ops': {
        const ops = this.validator.processOps(msg.ops);
        if (ops.length) this.toRenderer({ type: 'ops', ops });
        break;
      }
      case 'query': {
        const query = this.validator.validateQuery(msg.query);
        if (!query) {
          this.answerQuery({ error: 'invalid query' });
          break;
        }
        if (query.q === 'matchMedia') query.query = this.validator.rewriteMediaQuery(query.query);
        this.pendingQuery = ++this.qid;
        this.toRenderer({ type: 'query', qid: this.pendingQuery, query });
        break;
      }
      case 'listen':
        this.toRenderer({ type: 'listen', types: this.validator.validateListen(msg.types) });
        break;
      case 'pd':
        this.toRenderer({ type: 'preventDefault', keys: this.validator.validatePreventDefault(msg.keys) });
        break;
      case 'anim': {
        const anims = this.validator.validateAnim(msg.anims);
        const now = finiteNumber(msg.now);
        if (anims.length && now !== undefined) this.toRenderer({ type: 'anim', now, anims });
        break;
      }
      case 'raf':
        this.toRenderer({ type: 'requestFrame' });
        break;
      case 'log': {
        const text = String(msg.text);
        this.opts.onLog?.(String(msg.level), text);
        if (msg.level === 'error' && text.startsWith('Uncaught ')) this.opts.onError?.(text);
        break;
      }
      case 'diag':
        this.opts.onDiag?.(String(msg.kind), String(msg.detail));
        break;
      case 'bridge': {
        // docs/sdk-v1.md §3.2: the method and its arguments, then user activation for links, then the haptic limit.
        const method = String(msg.method).slice(0, 64);
        const action = checkBridgeCall(method, msg.args);
        if (!action) {
          if (this.rejectedBridgeReported < 20) {
            this.rejectedBridgeReported++;
            this.opts.onDiag?.('bridge-rejected', `${method}: unknown method or malformed arguments`);
          }
          break;
        }
        if (this.policy === 'strict' && PRIVILEGED_METHODS.has(method) && !this.activation.accept(msg.gesture, performance.now(), this.visible)) {
          if (this.blockedBridgeReported < 20) {
            this.blockedBridgeReported++;
            this.opts.onDiag?.('activation-blocked', `${method} ignored: it needs a tap on the visible widget and runs in that tap's handler, one call per tap`);
          }
          break;
        }
        if (action.kind === 'haptic') {
          const d = this.haptics.decide(performance.now(), this.visible);
          if (d === 'report') this.opts.onDiag?.('haptic-limit', this.visible ? 'haptics over 5 per burst or 10 per second are dropped' : 'haptics of a hidden widget are dropped');
          if (d !== 'play') break;
        }
        this.opts.onAction?.(action);
        this.opts.onBridge?.(method, msg.args as unknown[]);
        break;
      }
      case 'storage': {
        const commit = checkStorageCommit(msg.commit);
        if (!commit || (!commit.local && !commit.session)) {
          if (this.rejectedStorageReported < 20) {
            this.rejectedStorageReported++;
            this.opts.onDiag?.('storage-rejected', 'a storage commit that is not { local?, session? } objects of strings within 16384 bytes each; the client keeps the previous content');
          }
          break;
        }
        if (commit.local) this.opts.onStorageCommit?.('local', commit.local);
        if (commit.session) this.opts.onStorageCommit?.('session', commit.session);
        break;
      }
      case 'warmed':
        break;
      case 'loaded':
        this.opts.onLoaded?.();
        break;
      case 'stats':
        if (typeof msg.stats !== 'object' || msg.stats === null) break;
        this.opts.onStats?.({ ...msg.stats, nodes: this.validator.nodeCount });
        break;
      case 'dead':
        this.die(msg.reason);
        break;
    }
  }

  /** Write a query answer into the shared buffer and wake the blocked worker. */
  private answerQuery(result: unknown, binary?: ArrayBuffer) {
    if (Atomics.load(this.header, SAB_STATE) !== 1) return;
    let json = encoder.encode(JSON.stringify(result ?? null));
    let bin = binary ? new Uint8Array(binary) : null;
    if (json.length + (bin?.length ?? 0) > this.payload.length) {
      json = encoder.encode('{"error":"result too large"}');
      bin = null;
    }
    this.payload.set(json, 0);
    if (bin) this.payload.set(bin, json.length);
    Atomics.store(this.header, SAB_JSON_LEN, json.length);
    Atomics.store(this.header, SAB_BIN_LEN, bin ? bin.length : 0);
    Atomics.store(this.header, SAB_STATE, 2);
    Atomics.notify(this.header, SAB_STATE);
    this.pendingQuery = 0;
  }

  // ───────────── renderer → host ─────────────

  private onRenderer(msg: RendererToHost) {
    switch (msg.type) {
      case 'event': {
        const gesture = msg.trusted === true ? this.gestures.gestureFor(msg.ev) : 0;
        if (gesture) this.activation.issue(gesture, performance.now());
        this.toWorker(gesture ? { type: 'event', ev: msg.ev, gesture } : { type: 'event', ev: msg.ev });
        break;
      }
      case 'result':
        if (msg.qid === this.pendingQuery) this.answerQuery(msg.result, msg.binary);
        break;
      case 'size': {
        const h = finiteNumber(msg.height), iw = finiteNumber(msg.innerWidth), ih = finiteNumber(msg.innerHeight), dpr = finiteNumber(msg.dpr);
        if (h === undefined || iw === undefined || ih === undefined || dpr === undefined) break;
        const height = Math.min(Math.max(h, MIN_HEIGHT), MAX_HEIGHT);
        this.element.style.height = height + 'px';
        this.opts.onResize?.(height);
        const vp = { innerWidth: iw, innerHeight: ih, dpr };
        if (JSON.stringify(vp) !== JSON.stringify(this.viewport)) {
          this.viewport = vp;
          this.toWorker({ type: 'viewport', viewport: vp });
        }
        break;
      }
      case 'resize':
        this.toWorker({ type: 'resize', entries: msg.entries });
        break;
      case 'frame': {
        const time = finiteNumber(msg.time);
        if (time !== undefined) this.toWorker({ type: 'frame', time });
        break;
      }
      case 'gesture':
        this.opts.onGestureClaim?.({ x: msg.x === true, y: msg.y === true });
        break;
      case 'media':
        // OS-level change inside the renderer: the host theme is authoritative, just re-notify.
        this.toWorker({ type: 'theme', theme: this.theme, themeParams: this.widgetTheme.params });
        break;
      case 'violation':
        this.opts.onDiag?.('renderer-violation', String(msg.detail).slice(0, 4000));
        break;
    }
  }
}
