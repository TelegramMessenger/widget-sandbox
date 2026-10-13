// Web Animations inside the sandbox: Element.animate(), Animation, KeyframeEffect,
// DocumentTimeline and getAnimations(). The timing model lives here, so currentTime and
// playState never leave the sandbox; renderers are sent each animation's effect state as a
// record (`host('anim', …)`, see AnimRecord in shared/protocol.ts) and sample it themselves.
// CSS animations and transitions come from the renderer through the `anims` query and can be
// paused, seeked, re-rated or cancelled with `css` records.

import * as capture from './capture';
import * as ev from './events';
import { host, query, unsupported } from './host';

const G: any = globalThis;
const now = (): number => G.performance.now();

const MAX_KEYFRAMES = 64;
let nextAid = 1;
/** Animations whose record changed since the last flush; removals are kept by aid. */
const dirty = new Set<Animation | CSSAnimationProxy>();
const removed = new Set<number>();
/** Web animations the renderer holds a record for, by target. */
const byTarget = new Map<any, Set<Animation>>();

function mark(a: Animation | CSSAnimationProxy) {
  dirty.add(a);
}

/** Sends the records that changed in this task. Called after the task's ops went out. */
export function flushAnimations(): void {
  if (!dirty.size && !removed.size) return;
  const anims: any[] = [];
  for (const aid of removed) anims.push({ aid, remove: true });
  removed.clear();
  for (const a of [...dirty]) {
    const record = a._record();
    // Not rendered yet: the record goes with a later flush, after the element's ops.
    if (record === null) continue;
    dirty.delete(a);
    if (record) anims.push(record);
  }
  if (anims.length) host('anim', JSON.stringify({ now: now(), anims }));
}

// ───────────────────────── keyframes and timing ─────────────────────────

const kebab = (name: string) => (name === 'cssFloat' ? 'float' : name.startsWith('--') ? name : name.replace(/[A-Z]/g, (m) => '-' + m.toLowerCase()));
const RESERVED = new Set(['offset', 'easing', 'composite']);

interface Frame { offset: number | null; easing: string; composite?: string; values: Record<string, string> }

function normalizeKeyframes(input: any): Frame[] {
  if (input === null || input === undefined) return [];
  let frames: Frame[] = [];
  if (Array.isArray(input) || typeof input[Symbol.iterator] === 'function') {
    for (const k of input) {
      if (!k || typeof k !== 'object') throw new TypeError('Failed to execute animate: keyframes must be objects');
      const values: Record<string, string> = {};
      for (const [name, value] of Object.entries(k)) if (!RESERVED.has(name) && value !== undefined && value !== null) values[kebab(name)] = String(value);
      const offset = k.offset === undefined || k.offset === null ? null : Number(k.offset);
      if (offset !== null && !(offset >= 0 && offset <= 1)) throw new TypeError('Offsets must be null or in the range [0,1]');
      frames.push({ offset, easing: k.easing ? String(k.easing) : 'linear', composite: k.composite ?? undefined, values });
    }
  } else if (typeof input === 'object') {
    // Property-indexed: each property's values spread evenly; frames at equal offsets merge.
    const byOffset = new Map<number, Frame>();
    const list = (v: any) => (Array.isArray(v) ? v : [v]);
    for (const [name, raw] of Object.entries(input)) {
      if (RESERVED.has(name)) continue;
      const values = list(raw);
      values.forEach((value: any, i: number) => {
        const offset = values.length === 1 ? 1 : i / (values.length - 1);
        let f = byOffset.get(offset);
        if (!f) byOffset.set(offset, (f = { offset, easing: 'linear', values: {} }));
        f.values[kebab(name)] = String(value);
      });
    }
    frames = [...byOffset.values()].sort((a, b) => (a.offset as number) - (b.offset as number));
    const easings = input.easing === undefined ? [] : list(input.easing);
    frames.forEach((f, i) => { if (easings.length) f.easing = String(easings[i % easings.length]); });
    if (input.composite !== undefined) {
      const composites = list(input.composite);
      frames.forEach((f, i) => { f.composite = composites[i % composites.length]; });
    }
  } else {
    throw new TypeError('Failed to execute animate: keyframes must be an object or an array');
  }
  if (frames.length > MAX_KEYFRAMES) frames = frames.slice(0, MAX_KEYFRAMES);
  // Missing offsets: a lone frame is the end state; otherwise first 0, last 1, the rest evenly between known ones.
  if (frames.length === 1 && frames[0].offset === null) frames[0].offset = 1;
  if (frames.length > 1) {
    if (frames[0].offset === null) frames[0].offset = 0;
    if (frames[frames.length - 1].offset === null) frames[frames.length - 1].offset = 1;
  }
  for (let i = 0; i < frames.length; i++) {
    if (frames[i].offset !== null) continue;
    let j = i;
    while (frames[j].offset === null) j++;
    const a = frames[i - 1].offset as number, b = frames[j].offset as number;
    for (let k = i; k < j; k++) frames[k].offset = a + ((b - a) * (k - i + 1)) / (j - i + 1);
    i = j;
  }
  for (let i = 1; i < frames.length; i++) if ((frames[i].offset as number) < (frames[i - 1].offset as number)) throw new TypeError('Offsets must be monotonically non-decreasing');
  return frames;
}

interface Timing { duration: number; delay: number; endDelay: number; iterations: number; iterationStart: number; direction: string; fill: string; easing: string }

function normalizeTiming(options: any, base?: Timing): Timing {
  const t: Timing = base ? { ...base } : { duration: 0, delay: 0, endDelay: 0, iterations: 1, iterationStart: 0, direction: 'normal', fill: 'auto', easing: 'linear' };
  if (typeof options === 'number' || (typeof options === 'string' && options !== '')) {
    t.duration = Number(options);
  } else if (options && typeof options === 'object') {
    if (options.duration !== undefined) t.duration = options.duration === 'auto' ? 0 : Number(options.duration);
    if (options.delay !== undefined) t.delay = Number(options.delay);
    if (options.endDelay !== undefined) t.endDelay = Number(options.endDelay);
    if (options.iterations !== undefined) t.iterations = Number(options.iterations);
    if (options.iterationStart !== undefined) t.iterationStart = Number(options.iterationStart);
    if (options.direction !== undefined) t.direction = String(options.direction);
    if (options.fill !== undefined) t.fill = String(options.fill);
    if (options.easing !== undefined) t.easing = String(options.easing);
  }
  if (!(t.duration >= 0) || Number.isNaN(t.duration)) throw new TypeError('duration must be a non-negative number');
  if (!(t.iterations >= 0)) throw new TypeError('iterations must be non-negative');
  const bezier = /^\s*cubic-bezier\(([^,]+),[^,]+,([^,]+),/i.exec(t.easing);
  if (bezier && ![bezier[1], bezier[2]].every((x) => Number(x) >= 0 && Number(x) <= 1)) throw new TypeError(`'${t.easing}' is not a valid value for easing`);
  if (!Number.isFinite(t.delay)) t.delay = 0;
  if (!Number.isFinite(t.endDelay)) t.endDelay = 0;
  return t;
}

const activeDuration = (t: Timing) => (t.duration === 0 || t.iterations === 0 ? 0 : t.duration * t.iterations);
const endTime = (t: Timing) => Math.max(t.delay + activeDuration(t) + t.endDelay, 0);

/** The Web Animations progress of a local time, without the easing: enough for getComputedTiming. */
function computed(t: Timing, local: number | null) {
  const active = activeDuration(t);
  const end = endTime(t);
  const out: any = { ...t, fill: t.fill === 'auto' ? 'none' : t.fill, activeDuration: active, endTime: end, localTime: local, progress: null, currentIteration: null };
  if (local === null) return out;
  const before = local < Math.max(Math.min(t.delay, end), 0);
  const after = local > Math.max(Math.min(t.delay + active, end), 0);
  const fill = out.fill;
  let activeTime: number | null;
  if (before) activeTime = fill === 'backwards' || fill === 'both' ? 0 : null;
  else if (after) activeTime = fill === 'forwards' || fill === 'both' ? Math.max(Math.min(local - t.delay, active), 0) : null;
  else activeTime = local - t.delay;
  if (activeTime === null) return out;
  let overall = t.duration === 0 ? (before ? 0 : t.iterations) : activeTime / t.duration;
  overall += t.iterationStart;
  let simple = Number.isFinite(overall) ? overall % 1 : t.iterationStart % 1;
  if (simple === 0 && !before && activeTime === active && t.iterations !== 0) simple = 1;
  out.currentIteration = after && !Number.isFinite(t.iterations) ? Infinity : simple === 1 ? Math.floor(overall) - 1 : Math.floor(overall);
  out.progress = simple;
  return out;
}

export class KeyframeEffect {
  target: any;
  pseudoElement: string | null = null;
  composite: string;
  iterationComposite: string;
  _frames: Frame[];
  _timing: Timing;
  _animation: Animation | null = null;

  constructor(target: any, keyframes: any, options?: any) {
    if (target instanceof KeyframeEffect) {
      this.target = target.target;
      this._frames = target._frames.map((f) => ({ ...f, values: { ...f.values } }));
      this._timing = { ...target._timing };
      this.composite = target.composite;
      this.iterationComposite = target.iterationComposite;
      return;
    }
    this.target = target ?? null;
    this._frames = normalizeKeyframes(keyframes);
    this._timing = normalizeTiming(options);
    this.composite = (options && typeof options === 'object' && options.composite) || 'replace';
    this.iterationComposite = (options && typeof options === 'object' && options.iterationComposite) || 'replace';
  }

  getKeyframes() {
    return this._frames.map((f) => {
      const out: any = { offset: f.offset, computedOffset: f.offset, easing: f.easing, composite: f.composite ?? 'auto' };
      for (const [k, v] of Object.entries(f.values)) out[k.replace(/-([a-z])/g, (_, c) => c.toUpperCase())] = v;
      return out;
    });
  }
  setKeyframes(keyframes: any) {
    this._frames = normalizeKeyframes(keyframes);
    if (this._animation) mark(this._animation);
  }
  getTiming() {
    return { ...this._timing };
  }
  getComputedTiming() {
    return computed(this._timing, this._animation ? this._animation.currentTime : null);
  }
  updateTiming(options?: any) {
    this._timing = normalizeTiming(options ?? {}, this._timing);
    if (this._animation) {
      mark(this._animation);
      this._animation._schedule();
    }
  }
}

export class AnimationPlaybackEvent extends ev.Event {
  currentTime: number | null;
  timelineTime: number | null;
  constructor(type: string, init: any = {}) {
    super(type, init);
    this.currentTime = init.currentTime ?? null;
    this.timelineTime = init.timelineTime ?? null;
  }
}

export class DocumentTimeline {
  get currentTime() { return now(); }
  get duration() { return null; }
}
export const timeline = new DocumentTimeline();

function deferred() {
  let resolve!: (v: any) => void, reject!: (e: any) => void;
  const promise = new Promise<any>((res, rej) => { resolve = res; reject = rej; });
  // A cancel rejects `finished`; widgets that never await it must not see an unhandled rejection.
  promise.catch(() => {});
  return { promise, resolve, reject };
}

const abortError = () => {
  const e: any = new Error('The user aborted a request.');
  e.name = 'AbortError';
  return e;
};

export class Animation extends ev.EventTarget {
  id = '';
  timeline: DocumentTimeline | null;
  onfinish: ((e: any) => void) | null = null;
  oncancel: ((e: any) => void) | null = null;
  onremove: ((e: any) => void) | null = null;
  replaceState = 'active';
  _aid = nextAid++;
  _effect: KeyframeEffect | null = null;
  _start: number | null = null;
  _hold: number | null = null;
  _rate = 1;
  _finished = deferred();
  _ready = deferred();
  _notified = false;
  _timer: any = 0;
  _sent = false;

  constructor(effect?: KeyframeEffect | null, tl?: DocumentTimeline | null) {
    super();
    this.timeline = tl === undefined ? timeline : tl;
    this.effect = effect ?? null;
    this._ready.resolve(this);
  }

  get effect() { return this._effect; }
  set effect(e: KeyframeEffect | null) {
    if (this._effect) this._effect._animation = null;
    this._effect = e;
    if (e) e._animation = this;
    this._track();
    mark(this);
  }

  get finished() { return this._finished.promise; }
  get ready() { return this._ready.promise; }
  get pending() { return false; }

  get currentTime(): number | null {
    if (this._hold !== null) return this._hold;
    if (this._start === null) return null;
    return (now() - this._start) * this._rate;
  }
  set currentTime(t: number | null) {
    if (t === null) throw new TypeError('currentTime cannot be set to null');
    const time = Number(t);
    if (this._hold !== null || this._start === null) this._hold = time;
    else this._start = now() - time / this._rate;
    this._changed();
  }

  get startTime(): number | null { return this._start; }
  set startTime(t: number | null) {
    if (t === null) {
      this._hold = this.currentTime;
      this._start = null;
    } else {
      this._start = Number(t);
      this._hold = null;
    }
    this._changed();
  }

  get playbackRate() { return this._rate; }
  set playbackRate(r: number) { this.updatePlaybackRate(r); }
  updatePlaybackRate(r: number) {
    const rate = Number(r);
    if (!Number.isFinite(rate)) throw new TypeError('playbackRate must be finite');
    const ct = this.currentTime;
    this._rate = rate;
    if (this._hold === null && ct !== null && rate !== 0) this._start = now() - ct / rate;
    this._changed();
  }

  get _end() { return this._effect ? endTime(this._effect._timing) : 0; }

  get playState(): string {
    const ct = this.currentTime;
    if (ct === null) return 'idle';
    if (this._hold !== null && this._start === null && !this._finishedAt(ct)) return 'paused';
    if (this._finishedAt(ct)) return 'finished';
    return 'running';
  }
  _finishedAt(ct: number) {
    return (this._rate > 0 && ct >= this._end) || (this._rate < 0 && ct <= 0);
  }

  play() {
    const ct = this.currentTime;
    const end = this._end;
    let time = ct ?? 0;
    if (this._rate > 0 && (ct === null || ct < 0 || ct >= end)) time = 0;
    else if (this._rate < 0 && (ct === null || ct <= 0 || ct > end)) {
      if (end === Infinity) throw new Error('InvalidStateError: cannot play an infinite animation in reverse');
      time = end;
    }
    this._hold = null;
    this._start = this._rate === 0 ? now() : now() - time / this._rate;
    if (this._rate === 0) this._hold = time;
    this._restartFinished();
    this._changed();
  }
  pause() {
    const ct = this.currentTime;
    this._hold = ct === null ? (this._rate >= 0 ? 0 : this._end) : ct;
    this._start = null;
    this._changed();
  }
  reverse() {
    this._rate = -this._rate;
    this.play();
  }
  finish() {
    if (this._rate === 0 || (this._rate > 0 && this._end === Infinity)) throw new Error('InvalidStateError: cannot finish this animation');
    const limit = this._rate > 0 ? this._end : 0;
    if (this._hold !== null && this._start === null) this._hold = limit;
    else this._start = now() - limit / this._rate;
    this._changed();
    this._check();
  }
  cancel() {
    if (this.playState === 'idle') return;
    this._start = null;
    this._hold = null;
    clearTimeout(this._timer);
    this._finished.reject(abortError());
    this._finished = deferred();
    this._notified = false;
    this._changed();
    this._fire('cancel', null);
  }
  persist() {}
  commitStyles() { unsupported('Animation.commitStyles'); }

  _restartFinished() {
    if (this._notified) {
      this._notified = false;
      this._finished = deferred();
    }
  }
  _track() {
    for (const set of byTarget.values()) set.delete(this);
    const target = this._effect?.target;
    if (!target) return;
    let set = byTarget.get(target);
    if (!set) byTarget.set(target, (set = new Set()));
    set.add(this);
  }
  _changed() {
    mark(this);
    this._schedule();
  }
  /** Wakes once at the finish time instead of polling. */
  _schedule() {
    clearTimeout(this._timer);
    this._timer = 0;
    const ct = this.currentTime;
    if (ct === null || this._start === null || this._rate === 0) return;
    const remaining = this._rate > 0 ? (this._end - ct) / this._rate : ct / -this._rate;
    if (!Number.isFinite(remaining)) return;
    this._timer = setTimeout(() => this._check(), Math.max(0, remaining) + 1);
  }
  _check() {
    if (this.playState !== 'finished' || this._notified) return;
    this._notified = true;
    this._finished.resolve(this);
    // An animation that no longer fills has nothing left for the renderer.
    const fill = this._effect?._timing.fill ?? 'auto';
    if (!(fill === 'forwards' || fill === 'both')) mark(this);
    this._fire('finish', this.currentTime);
  }
  _fire(type: string, currentTime: number | null) {
    const e = new AnimationPlaybackEvent(type, { currentTime, timelineTime: now() });
    const handler = (this as any)['on' + type];
    try { if (typeof handler === 'function') handler.call(this, e); } catch (err) { G.reportError?.(err); }
    this.dispatchEvent(e);
  }

  /** The renderer's record: null while the target is not rendered yet, undefined when nothing is to be sent. */
  _record(): any {
    const effect = this._effect;
    const target = effect?.target;
    const id = target ? capture.mirroredId(target) : 0;
    const fill = effect?._timing.fill ?? 'auto';
    const idle = this.playState === 'idle';
    const spent = this.playState === 'finished' && !(fill === 'forwards' || fill === 'both');
    if (!effect || !target || idle || spent) {
      if (this._sent) {
        this._sent = false;
        return { aid: this._aid, remove: true };
      }
      return undefined;
    }
    if (!id) return target.isConnected ? null : undefined;
    this._sent = true;
    const t = effect._timing;
    return {
      aid: this._aid,
      target: id,
      keyframes: effect._frames.map((f) => ({ offset: f.offset, easing: f.easing, ...(f.composite && f.composite !== 'auto' ? { composite: f.composite } : {}), values: f.values })),
      timing: { duration: t.duration, delay: t.delay, endDelay: t.endDelay, iterations: Number.isFinite(t.iterations) ? t.iterations : null, iterationStart: t.iterationStart, direction: t.direction, fill: t.fill === 'auto' ? 'none' : t.fill, easing: t.easing },
      composite: effect.composite,
      iterationComposite: effect.iterationComposite,
      ...(this._hold !== null ? { holdTime: this._hold } : { startTime: this._start }),
      playbackRate: this._rate,
    };
  }
}

// ───────────────────────── CSS animations and transitions ─────────────────────────

/** A running CSS animation or transition as the renderer reported it; control goes back as a `css` record. */
class CSSAnimationProxy extends ev.EventTarget {
  _aid = nextAid++;
  _target: any;
  _kind: 'animation' | 'transition';
  _name: string;
  _origin: number;
  _base: number;
  _at: number;
  _rate: number;
  _paused: boolean;
  _cancelled = false;
  _timing: Timing;
  _finished = deferred();
  timeline = timeline;
  id = '';
  onfinish = null;
  oncancel = null;
  onremove = null;
  replaceState = 'active';

  constructor(target: any, e: any) {
    super();
    this._target = target;
    this._kind = e.kind;
    this._name = String(e.name);
    this._origin = Number(e.origin);
    this._base = Number(e.currentTime) || 0;
    this._at = now();
    this._rate = Number(e.playbackRate) || 1;
    this._paused = !!e.paused;
    this._timing = { duration: Number(e.duration) || 0, delay: Number(e.delay) || 0, endDelay: 0, iterations: e.iterations === null ? Infinity : Number(e.iterations) || 1, iterationStart: 0, direction: e.direction ?? 'normal', fill: e.fill ?? 'none', easing: e.easing ?? 'ease' };
  }
  get animationName() { return this._kind === 'animation' ? this._name : undefined; }
  get transitionProperty() { return this._kind === 'transition' ? this._name : undefined; }
  get effect() {
    const self = this;
    return { target: this._target, getTiming: () => ({ ...self._timing }), getComputedTiming: () => computed(self._timing, self.currentTime), getKeyframes: () => [] };
  }
  get currentTime(): number | null {
    if (this._cancelled) return null;
    return this._paused ? this._base : this._base + (now() - this._at) * this._rate;
  }
  set currentTime(t: number | null) {
    if (t === null) return;
    this._base = Number(t);
    this._at = now();
    mark(this);
  }
  get startTime() { return this._paused || this._cancelled ? null : this._at - this._base / this._rate; }
  get playbackRate() { return this._rate; }
  set playbackRate(r: number) { this.updatePlaybackRate(r); }
  updatePlaybackRate(r: number) {
    this._base = this.currentTime ?? 0;
    this._at = now();
    this._rate = Number(r) || 0;
    mark(this);
  }
  get playState() {
    if (this._cancelled) return 'idle';
    if (this._paused) return 'paused';
    const end = endTime(this._timing);
    const ct = this.currentTime ?? 0;
    return (this._rate > 0 && ct >= end) || (this._rate < 0 && ct <= 0) ? 'finished' : 'running';
  }
  get finished() { return this._finished.promise; }
  get ready() { return Promise.resolve(this); }
  get pending() { return false; }
  play() {
    this._base = this.currentTime ?? 0;
    this._at = now();
    this._paused = false;
    this._cancelled = false;
    mark(this);
  }
  pause() {
    this._base = this.currentTime ?? 0;
    this._paused = true;
    mark(this);
  }
  reverse() { this.updatePlaybackRate(-this._rate); this.play(); }
  finish() { this.currentTime = this._rate > 0 ? endTime(this._timing) : 0; }
  cancel() {
    this._cancelled = true;
    this._finished.reject(abortError());
    mark(this);
  }
  persist() {}
  commitStyles() { unsupported('Animation.commitStyles'); }
  _record(): any {
    const id = capture.mirroredId(this._target);
    if (!id) return undefined;
    const base = { aid: this._aid, target: id, css: { kind: this._kind, name: this._name, origin: this._origin }, playbackRate: this._rate };
    if (this._cancelled) return { ...base, cancelled: true };
    return this._paused ? { ...base, holdTime: this._base } : { ...base, startTime: this._at - this._base / this._rate };
  }
}

/** Proxies stay the same objects between getAnimations() calls while their identity matches. */
const proxies = new Map<string, CSSAnimationProxy>();

function cssAnimations(target: any, subtree: boolean): CSSAnimationProxy[] {
  const id = capture.mirroredId(target);
  if (!id) return [];
  const answer = query({ q: 'anims', id, subtree }, false);
  const out: CSSAnimationProxy[] = [];
  for (const e of Array.isArray(answer?.list) ? answer.list : []) {
    const node = capture.nodeById(Number(e.id));
    if (!node) continue;
    const key = `${e.id}:${e.kind}:${e.name}:${e.origin}`;
    let p = proxies.get(key);
    if (!p || p._cancelled) {
      p = new CSSAnimationProxy(node, e);
      proxies.set(key, p);
    }
    out.push(p);
  }
  if (proxies.size > 4096) proxies.clear();
  return out;
}

function webAnimations(root: any, subtree: boolean): Animation[] {
  const out: Animation[] = [];
  for (const [target, set] of byTarget) {
    if (!(target === root || (subtree && root.contains?.(target)))) continue;
    for (const a of set) {
      const state = a.playState;
      const fill = a._effect?._timing.fill ?? 'auto';
      if (state === 'running' || state === 'paused' || (state === 'finished' && (fill === 'forwards' || fill === 'both'))) out.push(a);
    }
  }
  return out.sort((a, b) => a._aid - b._aid);
}

// ───────────────────────── install ─────────────────────────

export function installAnimations(elementProto: any, document: any) {
  const define = (obj: any, name: string, value: any) => Object.defineProperty(obj, name, { value, writable: true, configurable: true });
  define(elementProto, 'animate', function (this: any, keyframes: any, options?: any) {
    const effect = new KeyframeEffect(this, keyframes, options);
    const animation = new Animation(effect, timeline);
    if (options && typeof options === 'object' && options.id !== undefined) animation.id = String(options.id);
    animation.play();
    return animation;
  });
  define(elementProto, 'getAnimations', function (this: any, options?: any) {
    const subtree = !!(options && options.subtree);
    return [...cssAnimations(this, subtree), ...webAnimations(this, subtree)];
  });
  define(document, 'getAnimations', function () {
    const root = document.documentElement;
    return root ? [...cssAnimations(root, true), ...webAnimations(root, true)] : [];
  });
  Object.defineProperty(document, 'timeline', { get: () => timeline, configurable: true });
  Object.assign(G, { Animation, KeyframeEffect, DocumentTimeline, AnimationPlaybackEvent, AnimationTimeline: DocumentTimeline });
}
