// Message schema shared by sandbox shim, host and renderer. See docs/protocol.md.
// Types only (plus a few constants): no code here runs with any privilege.

export type NodeId = number;
/** The widget's Document node. Never rendered; parent of the <html> element. */
export const DOCUMENT_ID = 1;

// ───────────── sandbox → host: mutation ops (UNTRUSTED, validated by host) ─────────────

export type SandboxOp =
  | { op: 'el'; id: NodeId; tag: string; ns?: 'svg'; attrs?: Record<string, string> }
  | { op: 'text'; id: NodeId; data: string }
  | { op: 'comment'; id: NodeId; data: string }
  | { op: 'attr'; id: NodeId; name: string; value: string }
  | { op: 'rattr'; id: NodeId; name: string }
  | { op: 'data'; id: NodeId; data: string }
  | { op: 'ins'; parent: NodeId; id: NodeId; before: NodeId | null }
  | { op: 'rm'; id: NodeId }
  | { op: 'style'; id: NodeId; name: string; value: string; important?: boolean }
  | { op: 'prop'; id: NodeId; name: string; value: string | number | boolean }
  | { op: 'call'; id: NodeId; method: string; args?: unknown[] }
  | { op: 'canvas'; id: NodeId; cmds: CanvasCmd[] };

/** [kind, name, ...args]. kind: 'c' call, 's' set property, 'r' resize, 'g' gradient/pattern op. */
export type CanvasCmd = unknown[];

// ───────────── host → renderer: render ops (TRUSTED, already sanitized) ─────────────

export type RenderOp =
  | { op: 'el'; id: NodeId; tag: string; ns?: 'svg'; attrs: [string, string][]; adopt?: 'html' | 'head' | 'body' }
  | { op: 'text'; id: NodeId; data: string }
  | { op: 'comment'; id: NodeId; data: string }
  | { op: 'attr'; id: NodeId; name: string; value: string }
  | { op: 'rattr'; id: NodeId; name: string }
  | { op: 'data'; id: NodeId; data: string }
  | { op: 'ins'; parent: NodeId; id: NodeId; before: NodeId | null }
  | { op: 'rm'; id: NodeId }
  | { op: 'style'; id: NodeId; name: string; value: string; important: boolean }
  | { op: 'css'; id: NodeId; text: string }
  | { op: 'prop'; id: NodeId; name: string; value: string | number | boolean }
  | { op: 'call'; id: NodeId; method: string; args: (string | number | boolean | null)[] }
  | { op: 'canvas'; id: NodeId; cmds: CanvasCmd[] };

// ───────────── sync queries (sandbox → host → renderer → host → sandbox) ─────────────

export type Query =
  | { q: 'rect'; id: NodeId }
  | { q: 'rects'; id: NodeId }
  | { q: 'box'; id: NodeId }
  | { q: 'cs'; id: NodeId; pseudo?: string; prop?: string }
  | { q: 'svg'; id: NodeId; method: string; args?: number[] }
  | { q: 'measureText'; font: string; text: string }
  | { q: 'imageData'; id: NodeId; x: number; y: number; w: number; h: number }
  | { q: 'canvasCall'; id: NodeId; method: string; args?: unknown[] }
  | { q: 'elementFromPoint'; x: number; y: number }
  | { q: 'validity'; id: NodeId }
  | { q: 'matchMedia'; query: string }
  | { q: 'viewport' }
  | { q: 'anims'; id: NodeId; subtree: boolean };

export interface AnimKeyframe {
  offset: number;
  easing: string;
  composite: 'replace' | 'add' | 'accumulate' | null;
  values: Record<string, string>;
}

export interface AnimTiming {
  duration: number;
  delay: number;
  endDelay: number;
  iterations: number | null;
  iterationStart: number;
  direction: 'normal' | 'reverse' | 'alternate' | 'alternate-reverse';
  fill: 'none' | 'forwards' | 'backwards' | 'both';
  easing: string;
}

export type AnimRecord =
  | { aid: number; remove: true }
  | {
    aid: number; target: NodeId; css: { kind: 'animation' | 'transition'; name: string; origin: number };
    startTime: number | null; holdTime: number | null; playbackRate: number; cancelled: boolean;
  }
  | {
    aid: number; target: NodeId; keyframes: AnimKeyframe[]; timing: AnimTiming;
    composite: 'replace' | 'add' | 'accumulate'; iterationComposite: 'replace' | 'accumulate';
    startTime: number | null; holdTime: number | null; playbackRate: number;
  };

export const MAX_ANIM_BATCH_BYTES = 4 * 1024 * 1024;

// ───────────── renderer → host → sandbox: events ─────────────

export interface WidgetEvent {
  type: string;
  targetId: NodeId;
  relatedId?: NodeId;
  clientX?: number;
  clientY?: number;
  offsetX?: number;
  offsetY?: number;
  pageX?: number;
  pageY?: number;
  movementX?: number;
  movementY?: number;
  button?: number;
  buttons?: number;
  detail?: number;
  pointerId?: number;
  pointerType?: string;
  isPrimary?: boolean;
  pressure?: number;
  deltaX?: number;
  deltaY?: number;
  deltaMode?: number;
  key?: string;
  code?: string;
  repeat?: boolean;
  modifiers?: number; // bit 0 alt, 1 ctrl, 2 meta, 3 shift
  value?: string;
  checked?: boolean;
  selectedIndex?: number;
  inputType?: string;
  data?: string | null;
  submitterId?: NodeId;
  defaultActions?: boolean;
  editorGeneration?: number;
  editorCommand?: number;
  naturalWidth?: number;
  naturalHeight?: number;
  open?: boolean;
  scrollTop?: number;
  scrollLeft?: number;
}

/** Event types the renderer may forward. Everything else is ignored. */
export const EVENT_TYPES = [
  'click', 'dblclick', 'contextmenu', 'input', 'change', 'submit', 'reset', 'keydown', 'keyup', 'keypress',
  'pointerdown', 'pointerup', 'pointermove', 'pointerenter', 'pointerleave', 'pointerover', 'pointerout', 'pointercancel',
  'mousedown', 'mouseup', 'mousemove', 'mouseenter', 'mouseleave', 'mouseover', 'mouseout',
  'touchstart', 'touchmove', 'touchend', 'touchcancel',
  'focus', 'blur', 'focusin', 'focusout', 'wheel', 'scroll', 'load', 'error', 'toggle',
  'animationstart', 'animationend', 'animationiteration', 'transitionend',
] as const;

// ───────────── host ⇄ renderer envelope ─────────────

export type HostToRenderer =
  | { type: 'ops'; ops: RenderOp[] }
  | { type: 'query'; qid: number; query: Query }
  | { type: 'listen'; types: string[] }
  | { type: 'preventDefault'; keys: string[] }
  | { type: 'anim'; now: number; anims: AnimRecord[] }
  | { type: 'requestFrame' }
  /** The runtime is gone (fatal) or paused: keep the last render, stop forwarding events and frames, show a badge with `reason`. */
  | { type: 'frozen'; reason: string; fatal: boolean };

export type RendererToHost =
  | { type: 'ready' }
  | { type: 'event'; ev: WidgetEvent; trusted?: boolean }
  | { type: 'result'; qid: number; result: unknown; binary?: ArrayBuffer; binaryBase64?: string }
  | { type: 'size'; width: number; height: number; innerWidth: number; innerHeight: number; dpr: number }
  | { type: 'resize'; entries: { id: NodeId; width: number; height: number }[] }
  | { type: 'frame'; time: number }
  | { type: 'media' }
  /**
   * Sent on every primary pointerdown: whether the widget wants the touch
   * gesture for itself (an ancestor's `touch-action` forbids vertical panning,
   * or the sandbox has prevented pointerdown/touchstart there before). Hosts
   * whose renderer sits inside a scrolling container that cannot see into the
   * WebView (Android) use it to stop the container from intercepting; the
   * web and iOS hosts ignore it.
   */
  | { type: 'gesture'; wants: boolean }
  | { type: 'violation'; detail: string };

// ───────────── worker ⇄ host envelope ─────────────

export interface SandboxConfig {
  memoryLimitBytes: number;
  /** Longest continuous run of widget JS in one task before the runtime is killed. */
  cpuLimitMs: number;
  /** JS time the widget may use per `cpuQuotaWindowMs` (sliding window); beyond it timers and frames are deferred, never killed. */
  cpuQuotaMs: number;
  cpuQuotaWindowMs: number;
  /** Kill after this long of uninterrupted throttling; 0 disables (default). */
  cpuKillAfterMs: number;
  /** Live timers (setTimeout + setInterval) a widget may hold; the shim throws RangeError beyond it, the glue refuses as a backstop. */
  maxTimers: number;
  /** Initial visibility (see `setVisible`): hidden widgets get no frames and timers at 1 Hz. */
  visible: boolean;
  viewport: { innerWidth: number; innerHeight: number; dpr: number };
  theme: 'light' | 'dark';
  /**
   * Optional wire forms this host understands, opt-in per host (docs/protocol.md,
   * "Features"). Absent or empty: plain JSON only, which every host and renderer speaks.
   * `canvasWords`: canvas commands as a word stream through `__hostCanvas`.
   */
  features?: string[];
  /** Extra bundled libraries the host serves under /libs/<file> (loader.ts `provideLibrary`); glue only, never sent to the sandbox. */
  libraries?: { name: string; file: string; patterns: string[] }[];
  /**
   * `compat` (default): the behaviour hosts had before the hardening below. `strict`: privileged
   * bridge calls need a user gesture, text values are capped at 64 KB and complex-script text is budgeted.
   */
  policy?: SandboxPolicy;
}

export type SandboxPolicy = 'compat' | 'strict';

/** Defaults shared by every host (the native configs repeat them). */
export const LIMIT_DEFAULTS = {
  memoryLimitBytes: 64 * 1024 * 1024,
  cpuLimitMs: 500,
  cpuQuotaMs: 300,
  cpuQuotaWindowMs: 1000,
  cpuKillAfterMs: 0,
  maxTimers: 256,
} as const;

/** Timer delays are clamped to this on every platform (24 h, like Telegram). */
export const MAX_TIMER_DELAY_MS = 86_400_000;

/**
 * Web only: the QuickJS wasm module imports its memory, and the glue hands it one
 * whose `maximum` is INITIAL + memoryLimitBytes. INITIAL mirrors the variant's
 * INITIAL_MEMORY (16 MB: static data, C stack and dlmalloc's first arena); the
 * runtime's heap may grow memoryLimitBytes beyond it. See src/host/worker.ts.
 */
export const WASM_PAGE_BYTES = 65_536;
export const WASM_INITIAL_PAGES = (16 * 1024 * 1024) / WASM_PAGE_BYTES;

export type HostToWorker =
  | { type: 'init'; html: string; sab: SharedArrayBuffer; config: SandboxConfig; sandboxBundle: string }
  /** Pool: boot QuickJS and evaluate the sandbox bundle now (`config` is provisional: limits only), the widget comes later with `start`. */
  | { type: 'warm'; sab: SharedArrayBuffer; config: SandboxConfig; sandboxBundle: string }
  /** Pool: load the widget into a warmed worker (`config` is the real one). */
  | { type: 'start'; html: string; config: SandboxConfig }
  | { type: 'event'; ev: WidgetEvent; gesture?: number }
  | { type: 'frame'; time: number }
  | { type: 'viewport'; viewport: SandboxConfig['viewport'] }
  | { type: 'resize'; entries: { id: NodeId; width: number; height: number }[] }
  | { type: 'theme'; theme: 'light' | 'dark' }
  | { type: 'bridge'; event: string; data?: unknown }
  | { type: 'visibility'; visible: boolean };

export interface Stats {
  jsMs: number;
  tasks: number;
  memoryBytes: number;
  ops: number;
  queries: number;
  queryMs: number;
  queryCacheHits: number;
  missedMutations: number;
  /** Tasks deferred by the CPU quota (frames collapse into one, so this undercounts frames). */
  throttledTasks: number;
  /** JS time used in the current quota window at the time of the report. */
  cpuWindowMs: number;
}

export type WorkerToHost =
  // Messages whose payload originates in the sandbox (UNTRUSTED):
  | { type: 'ops'; ops: unknown }
  | { type: 'query'; query: unknown }
  | { type: 'listen'; types: unknown }
  | { type: 'pd'; keys: unknown }
  | { type: 'anim'; anims: unknown; now: number }
  | { type: 'raf' }
  | { type: 'log'; level: string; text: string }
  | { type: 'diag'; kind: string; detail: string }
  | { type: 'bridge'; method: string; args: unknown; gesture?: number }
  // Messages produced by the trusted worker glue:
  /** The sandbox bundle is evaluated; a `start` can follow at no boot cost (pool). */
  | { type: 'warmed'; bootMs: number }
  | { type: 'loaded' }
  | { type: 'stats'; stats: Stats }
  | { type: 'dead'; reason: string };

// Sync bridge SharedArrayBuffer layout (Int32 header, then payload bytes).
export const SAB_SIZE = 16 * 1024 * 1024;
export const SAB_HEADER = 16; // bytes
export const SAB_STATE = 0; // 0 idle, 1 waiting, 2 done
export const SAB_JSON_LEN = 1;
export const SAB_BIN_LEN = 2;
