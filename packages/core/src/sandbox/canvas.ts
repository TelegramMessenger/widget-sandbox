// Canvas 2D proxy. getContext('2d') returns a recording context: state setters
// and draw calls are buffered as commands and replayed by the renderer on a real
// canvas. Anything that must return a value synchronously (measureText,
// getImageData, isPointInPath, …) goes through the sync query bridge.

import { ensureCreated, flush, markDirty, mirroredId } from './capture';
import { diag, hostBinary, query, unsupported } from './host';

const CALLS = [
  'save', 'restore', 'scale', 'rotate', 'translate', 'transform', 'setTransform', 'resetTransform', 'clearRect', 'fillRect',
  'strokeRect', 'beginPath', 'closePath', 'moveTo', 'lineTo', 'bezierCurveTo', 'quadraticCurveTo', 'arc', 'arcTo',
  'ellipse', 'rect', 'roundRect', 'fill', 'stroke', 'clip', 'fillText', 'strokeText', 'drawImage', 'reset',
];
const DEFAULTS: Record<string, any> = {
  fillStyle: '#000000', strokeStyle: '#000000', lineWidth: 1, lineCap: 'butt', lineJoin: 'miter', miterLimit: 10,
  lineDashOffset: 0, font: '10px sans-serif', textAlign: 'start', textBaseline: 'alphabetic', direction: 'ltr',
  globalAlpha: 1, globalCompositeOperation: 'source-over', shadowBlur: 0, shadowColor: 'rgba(0, 0, 0, 0)',
  shadowOffsetX: 0, shadowOffsetY: 0, imageSmoothingEnabled: true, imageSmoothingQuality: 'low', filter: 'none',
  letterSpacing: '0px', wordSpacing: '0px', fontKerning: 'auto', fontStretch: 'normal', fontVariantCaps: 'normal',
  textRendering: 'auto',
};
const MAX_BUFFERED = 400_000;

const pending = new Map<any, any[]>(); // canvas element → commands not yet flushed
const contexts = new WeakMap<object, Context2D>();
const measureCache = new Map<string, any>();
let objectSeq = 1;

function buffer(canvas: any): any[] {
  let cmds = pending.get(canvas);
  if (!cmds) pending.set(canvas, (cmds = []));
  return cmds;
}

/** capture hook: append one `canvas` op per canvas with buffered commands. */
export function flushCanvases(push: (op: any) => void) {
  if (!pending.size) return;
  for (const [canvas, cmds] of pending) {
    if (!cmds.length) continue;
    // Detached (offscreen) canvases are announced to the host on first use.
    const id = mirroredId(canvas) || ensureCreated(canvas);
    if (id) push({ op: 'canvas', id, cmds });
  }
  pending.clear();
}

class CanvasGradient {
  _id = objectSeq++;
  _canvas: any;
  constructor(canvas: any) { this._canvas = canvas; }
  addColorStop(offset: number, color: string) { buffer(this._canvas).push(['gs', this._id, Number(offset), String(color)]); }
}
class CanvasPattern {
  _id = objectSeq++;
  setTransform() {}
}

export class Path2D {
  _id = objectSeq++;
  _d: string | null;
  _cmds: any[][] = [];
  _sent = new WeakMap<object, number>();
  constructor(init?: any) {
    this._d = typeof init === 'string' ? init : null;
    if (init instanceof Path2D) {
      this._d = init._d;
      this._cmds = init._cmds.map((c) => [...c]);
    }
  }
  addPath(p: Path2D) { this._cmds.push(...p._cmds.map((c) => [...c])); }
  /** Make sure the renderer-side Path2D for `canvas` is up to date. */
  _materialize(canvas: any) {
    const cmds = buffer(canvas);
    const sent = this._sent.get(canvas);
    if (sent === undefined) cmds.push(['pn', this._id, this._d]);
    for (let i = sent ?? 0; i < this._cmds.length; i++) cmds.push(['pc', this._id, ...this._cmds[i]]);
    this._sent.set(canvas, this._cmds.length);
  }
}
for (const m of ['moveTo', 'lineTo', 'bezierCurveTo', 'quadraticCurveTo', 'arc', 'arcTo', 'ellipse', 'rect', 'roundRect', 'closePath']) {
  (Path2D.prototype as any)[m] = function (this: Path2D, ...args: any[]) { this._cmds.push([m, ...args.map(Number)]); };
}

export class ImageData {
  width: number; height: number; data: Uint8ClampedArray; colorSpace = 'srgb';
  constructor(a: any, b: number, c?: number) {
    if (typeof a === 'number') { this.width = a; this.height = b; this.data = new Uint8ClampedArray(a * b * 4); }
    else { this.data = a; this.width = b; this.height = c ?? a.length / 4 / b; }
  }
}

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function toBase64(bytes: Uint8ClampedArray): string {
  let out = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const a = bytes[i], b = bytes[i + 1] ?? 0, c = bytes[i + 2] ?? 0;
    out += B64[a >> 2] + B64[((a & 3) << 4) | (b >> 4)] + (i + 1 < bytes.length ? B64[((b & 15) << 2) | (c >> 6)] : '=') + (i + 2 < bytes.length ? B64[c & 63] : '=');
  }
  return out;
}

export class Context2D {
  canvas: any;
  _s: Record<string, any> = { ...DEFAULTS };
  _stack: any[] = [];
  _dash: number[] = [];
  [k: string]: any;
  constructor(canvas: any) { this.canvas = canvas; }

  _enc(a: any): any {
    if (a === undefined) return undefined;
    if (a === null || typeof a === 'number' || typeof a === 'string' || typeof a === 'boolean') return a;
    if (a instanceof CanvasGradient || a instanceof CanvasPattern) return { g: a._id };
    if (a instanceof Path2D) { a._materialize(this.canvas); return { p: a._id }; }
    if (a instanceof ImageData) return { w: a.width, h: a.height, b64: toBase64(a.data) };
    if (Array.isArray(a)) return a.map(Number);
    if (a && a.nodeType === 1) return { n: ensureCreated(a) };
    if (a && typeof a === 'object' && 'x' in a && 'y' in a) return [Number(a.x), Number(a.y)]; // DOMPoint radii
    return null;
  }
  _push(cmd: any[]) {
    const cmds = buffer(this.canvas);
    if (cmds.length > MAX_BUFFERED) {
      if (cmds.length === MAX_BUFFERED + 1) diag('canvas', 'too many buffered canvas commands; dropping');
      return;
    }
    while (cmd.length && cmd[cmd.length - 1] === undefined) cmd.pop();
    cmds.push(cmd);
    markDirty(this.canvas);
  }

  setLineDash(d: number[]) { this._dash = [...(d ?? [])].map(Number); this._push(['c', 'setLineDash', this._dash]); }
  getLineDash() { return [...this._dash]; }
  createLinearGradient(...a: number[]) { const g = new CanvasGradient(this.canvas); this._push(['g', g._id, 'linear', ...a.map(Number)]); return g; }
  createRadialGradient(...a: number[]) { const g = new CanvasGradient(this.canvas); this._push(['g', g._id, 'radial', ...a.map(Number)]); return g; }
  createConicGradient(...a: number[]) { const g = new CanvasGradient(this.canvas); this._push(['g', g._id, 'conic', ...a.map(Number)]); return g; }
  createPattern(image: any, repetition: string) {
    const p = new CanvasPattern();
    this._push(['g', p._id, 'pattern', this._enc(image), String(repetition ?? 'repeat')]);
    return p;
  }
  measureText(text: string) {
    const key = this._s.font + '\n' + text;
    let m = measureCache.get(key);
    if (!m) {
      m = query({ q: 'measureText', font: String(this._s.font), text: String(text) }, false);
      if (!m || m.error) m = { width: 0 };
      if (measureCache.size > 5000) measureCache.clear();
      measureCache.set(key, m);
    }
    return { ...m };
  }
  getImageData(x: number, y: number, w: number, h: number) {
    const id = ensureCreated(this.canvas);
    flush();
    const r = query({ q: 'imageData', id, x: Number(x), y: Number(y), w: Number(w), h: Number(h) }, false);
    const bin = hostBinary();
    if (!r || r.error || !bin) return new ImageData(Math.max(1, Math.abs(w) | 0), Math.max(1, Math.abs(h) | 0));
    return new ImageData(new Uint8ClampedArray(bin), r.width, r.height);
  }
  createImageData(w: any, h?: number) { return typeof w === 'object' ? new ImageData(w.width, w.height) : new ImageData(Math.abs(w) | 0, Math.abs(h ?? 0) | 0); }
  putImageData(img: ImageData, x: number, y: number, ...rest: number[]) { this._push(['c', 'putImageData', this._enc(img), Number(x), Number(y), ...rest.map(Number)]); }
  _call(method: string, args: any[]) {
    const id = ensureCreated(this.canvas);
    return query({ q: 'canvasCall', id, method, args: args.map((a) => this._enc(a)) });
  }
  isPointInPath(...a: any[]) { return this._call('isPointInPath', a) === true; }
  isPointInStroke(...a: any[]) { return this._call('isPointInStroke', a) === true; }
  getTransform() { const m = this._call('getTransform', []); return new (globalThis as any).DOMMatrix(m && !m.error ? m : undefined); }
  getContextAttributes() { return { alpha: true, colorSpace: 'srgb', desynchronized: false, willReadFrequently: false }; }
  drawFocusIfNeeded() {}
}

for (const name of CALLS) {
  Context2D.prototype[name] = function (this: Context2D, ...args: any[]) {
    if (name === 'save') this._stack.push([{ ...this._s }, [...this._dash]]);
    else if (name === 'restore') {
      const top = this._stack.pop();
      if (top) [this._s, this._dash] = top;
    } else if (name === 'reset') { this._s = { ...DEFAULTS }; this._stack = []; this._dash = []; }
    this._push(['c', name, ...args.map((a) => this._enc(a))]);
  };
}
for (const name of Object.keys(DEFAULTS)) {
  Object.defineProperty(Context2D.prototype, name, {
    configurable: true,
    get(this: Context2D) { return this._s[name]; },
    set(this: Context2D, v: any) {
      const dflt = DEFAULTS[name];
      // Like a real context, ignore values of the wrong type.
      if (name === 'fillStyle' || name === 'strokeStyle') {
        if (typeof v !== 'string' && !(v instanceof CanvasGradient) && !(v instanceof CanvasPattern)) return;
      } else if (typeof dflt === 'number') {
        v = Number(v);
        if (!Number.isFinite(v)) return;
      } else if (typeof dflt === 'boolean') v = !!v;
      else if (typeof v !== 'string') return;
      this._s[name] = v;
      this._push(['s', name, this._enc(v)]);
    },
  });
}

const WEBGL = new Set(['webgl', 'webgl2', 'experimental-webgl', 'webgpu', 'bitmaprenderer']);

export function installCanvas(canvasProto: any) {
  const dim = (attr: string, dflt: number): PropertyDescriptor => ({
    configurable: true,
    get(this: any) { const n = parseInt(this.getAttribute(attr) ?? '', 10); return Number.isFinite(n) && n >= 0 ? n : dflt; },
    set(this: any, v: any) {
      const n = Math.max(0, Math.floor(Number(v) || 0));
      this.setAttribute(attr, String(n));
      // Assigning either dimension resets the bitmap and the context state.
      const ctx = contexts.get(this);
      if (ctx) { ctx._s = { ...DEFAULTS }; ctx._stack = []; ctx._dash = []; }
      buffer(this).push(['r', this.width, this.height]);
    },
  });
  Object.defineProperties(canvasProto, {
    width: dim('width', 300),
    height: dim('height', 150),
    getContext: {
      configurable: true, writable: true,
      value(this: any, type: string) {
        const t = String(type).toLowerCase();
        if (t === '2d') {
          let ctx = contexts.get(this);
          if (!ctx) contexts.set(this, (ctx = new Context2D(this)));
          return ctx;
        }
        if (WEBGL.has(t)) unsupported(t.includes('webgl') ? 'WebGL' : `canvas.getContext('${t}')`);
        return null;
      },
    },
    toDataURL: {
      configurable: true, writable: true,
      value(this: any, type?: string, quality?: number) {
        const id = ensureCreated(this);
        const r = query({ q: 'canvasCall', id, method: 'toDataURL', args: [type ?? 'image/png', quality ?? 0.92] }, false);
        return typeof r === 'string' ? r : 'data:,';
      },
    },
    toBlob: { configurable: true, writable: true, value(cb: any) { unsupported('canvas.toBlob'); if (typeof cb === 'function') cb(null); } },
    captureStream: { configurable: true, writable: true, value() { unsupported('canvas.captureStream'); return null; } },
    transferControlToOffscreen: { configurable: true, writable: true, value() { unsupported('OffscreenCanvas'); throw new Error('NotSupportedError: OffscreenCanvas is not available'); } },
  });
}
