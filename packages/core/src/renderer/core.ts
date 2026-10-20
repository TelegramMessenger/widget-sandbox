// Renderer core (security-critical, invariants 3 and 6). One instance per
// widget, created by `createRenderer(env)`: inside the sandboxed srcdoc iframe
// (renderer.ts, the only script that ever executes there) `env` is that frame's
// document; the ShadowDomRenderer prototype (src/host/shadow-renderer.ts) runs
// the same core in the chat page itself, on a shadow root.
//
// It applies already-validated RenderOps to the real DOM using DOM APIs only
// (never innerHTML), answers layout queries, forwards user events and cancels
// every navigation.
//
// DOM clobbering: widget markup may contain elements whose id/name shadow
// properties of `document` and of <form> elements (e.g. <input name="setAttribute">).
// Therefore every DOM method/accessor used below is captured from its prototype
// and invoked explicitly ("primordials"); nothing is looked up on `document` or
// on a widget element after start-up.

import { EVENT_TYPES, type CanvasCmd, type HostToRenderer, type Query, type RenderOp, type RendererToHost, type WidgetEvent } from '../shared/protocol';
import { PROFILE_UA_STYLESHEET } from '../shared/profile-tables';
import { THEME_VARIABLES, themeVariables } from '../shared/sdk-policy';
import { AnimationState, applyAnims, cssAnimationList } from './web-animations';

/** Where the core renders: a whole document (iframe, native WebViews) or a shadow root inside the host page. */
export interface RendererEnv {
  /** Element factory. */
  doc: Document;
  /** Listener target and `elementFromPoint`: the document or the shadow root. */
  root: Document | ShadowRoot;
  /** The widget's <html>, <head>, <body>: the document's own, or three elements the shadow renderer created. Read at `init()` (a document's body does not exist while its <head> script runs). */
  roots(): { html: HTMLElement; head: HTMLElement; body: HTMLElement };
  /** Origin of the widget's coordinate space in the root's viewport, or null when they coincide (a document). */
  origin(): { x: number; y: number } | null;
  viewport(): { innerWidth: number; innerHeight: number; dpr: number };
  /** Height of the laid-out widget, used to auto-size the bubble. */
  docHeight(): number;
  matchMedia(query: string): boolean;
  /** Fires when the widget's viewport changes size; returns the unsubscribe. */
  onViewportResize(cb: () => void): () => void;
  /** Fires when the OS colour scheme changes under the renderer (the host re-asserts its theme); returns the unsubscribe. */
  onColorSchemeChange(cb: () => void): () => void;
  /**
   * The element that stands for the canvas when the widget's <html> is not a document root (the shadow renderer's
   * host): the core paints on it what CSS paints on a document's canvas, the root's background or else the body's
   * (root background propagation). A document has a canvas of its own: undefined.
   */
  canvas?: HTMLElement;
}
export type Send = (msg: RendererToHost, transfer: Transferable[]) => void;
export interface RendererCore {
  /** Starts listening and reporting; `send` carries renderer → host messages. */
  init(send: Send): void;
  onMessage(msg: HostToRenderer): void;
  destroy(): void;
}

const SVG_NS = 'http://www.w3.org/2000/svg';

type AnyFn = (...a: any[]) => any;
const uncurry = <F extends AnyFn>(f: F) => (self: unknown, ...args: Parameters<F>): ReturnType<F> => f.apply(self, args);
const getter = (proto: object, name: string): ((self: unknown) => any) => {
  const g = Object.getOwnPropertyDescriptor(proto, name)!.get!;
  return (self) => g.call(self);
};
const setter = (proto: object, name: string): ((self: unknown, v: any) => void) => {
  const s = Object.getOwnPropertyDescriptor(proto, name)!.set!;
  return (self, v) => s.call(self, v);
};

const createElement = uncurry(Document.prototype.createElement);
const createElementNS = uncurry(Document.prototype.createElementNS);
const createTextNode = uncurry(Document.prototype.createTextNode);
const setAttribute = uncurry(Element.prototype.setAttribute);
const getAttribute = uncurry(Element.prototype.getAttribute);
const removeAttribute = uncurry(Element.prototype.removeAttribute);
const closest = uncurry(Element.prototype.closest);
const getBoundingClientRect = uncurry(Element.prototype.getBoundingClientRect);
const getClientRects = uncurry(Element.prototype.getClientRects);
const insertBefore = uncurry(Node.prototype.insertBefore);
const removeChild = uncurry(Node.prototype.removeChild);
const parentNodeOf = getter(Node.prototype, 'parentNode');
const firstChildOf = getter(Node.prototype, 'firstChild');
const nextSiblingOf = getter(Node.prototype, 'nextSibling');
const localNameOf = getter(Element.prototype, 'localName');
const setTextContent = setter(Node.prototype, 'textContent');
const setCharData = setter(CharacterData.prototype, 'data');
const htmlStyleOf = getter(HTMLElement.prototype, 'style');
const svgStyleOf = getter(SVGElement.prototype, 'style');
const styleOf = (el: Element): CSSStyleDeclaration => (el instanceof SVGElement ? svgStyleOf(el) : htmlStyleOf(el));

/** Defense in depth: the validator never emits these, but the renderer refuses them regardless. */
const FORBIDDEN_TAGS = new Set(['script', 'iframe', 'frame', 'frameset', 'object', 'embed', 'link', 'meta', 'base', 'portal', 'noscript', 'set', 'template', 'audio', 'video', 'source', 'track',
  // Outside the profile (form controls, dialogs): the validator drops or replaces them.
  'input', 'select', 'option', 'optgroup', 'textarea', 'datalist', 'fieldset', 'progress', 'meter', 'form', 'dialog']);
const FORBIDDEN_ATTRS = new Set(['srcdoc', 'srcset', 'action', 'formaction', 'ping', 'background', 'nonce', 'target', 'http-equiv']);

const scrollTopSet = setter(Element.prototype, 'scrollTop');
const scrollLeftSet = setter(Element.prototype, 'scrollLeft');

const rectJson = (r: DOMRect, o: { x: number; y: number } | null) => {
  const dx = o?.x ?? 0, dy = o?.y ?? 0;
  return { x: r.x - dx, y: r.y - dy, width: r.width, height: r.height, top: r.top - dy, left: r.left - dx, right: r.right - dx, bottom: r.bottom - dy };
};
const matrixJson = (m: DOMMatrix | null) => (m ? { a: m.a, b: m.b, c: m.c, d: m.d, e: m.e, f: m.f } : null);

const ALWAYS_FORWARD = new Set(['click', 'load', 'error', 'toggle']);
const THROTTLED = new Set(['pointermove', 'mousemove']);
/** What the widget can get: the profile's events (no keyboard, focus, dblclick, contextmenu, touch, wheel or scroll). */
const ALL_EVENTS = [...EVENT_TYPES];
const ENTER_LEAVE = ['pointerenter', 'pointerleave', 'mouseenter', 'mouseleave'];
const noop = () => {};
/**
 * Input that belongs to the chat, never to the widget (D4, docs/profile-v1.md):
 * keys and editing are swallowed (no Enter or Space activation), the platform's
 * context menu is cancelled (the bubble's own menu wins), a mouse press neither
 * moves the focus into the widget nor starts a text selection unless it lands on
 * text the widget made selectable (`user-select: text`, D18), and the focus
 * never stays in it.
 */
const SWALLOWED = ['keydown', 'keyup', 'keypress', 'beforeinput', 'input', 'contextmenu', 'mousedown', 'focusin', 'submit', 'selectstart'];

function decodeBase64(b64: string): Uint8ClampedArray<ArrayBuffer> {
  const bin = atob(b64);
  const out = new Uint8ClampedArray(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

export function toBase64(buf: ArrayBuffer): string {
  const bytes = new Uint8Array(buf);
  let bin = '';
  for (let i = 0; i < bytes.length; i += 0x8000) bin += String.fromCharCode.apply(null, Array.from(bytes.subarray(i, i + 0x8000)));
  return btoa(bin);
}

export function createRenderer(env: RendererEnv): RendererCore {
const doc = env.doc;
const root = env.root;
// `DocumentOrShadowRoot` members are separate functions on each prototype: the document's copy throws "Illegal invocation" on a shadow root.
const elementFromPoint = root instanceof Document ? uncurry(Document.prototype.elementFromPoint) : uncurry(ShadowRoot.prototype.elementFromPoint);
let send: Send = () => {};
const post = (msg: RendererToHost, transfer: Transferable[] = []) => send(msg, transfer);
const violation = (detail: string) => post({ type: 'violation', detail });

let HTML: HTMLElement;
let HEAD: HTMLElement;
let BODY: HTMLElement;

const nodes = new Map<number, Node>();
const ids = new WeakMap<Node, number>();
const animations = new AnimationState();

function register(id: number, node: Node) {
  nodes.set(id, node);
  ids.set(node, id);
}

function forget(node: Node) {
  const id = ids.get(node);
  if (id !== undefined) {
    nodes.delete(id);
    ids.delete(node);
    canvases.delete(id);
    if (node instanceof Element) resizeObserver.unobserve(node);
  }
  for (let c = firstChildOf(node); c; c = nextSiblingOf(c)) forget(c);
}

function idOf(node: Node | null): number {
  for (let n = node; n; n = parentNodeOf(n)) {
    const id = ids.get(n);
    if (id !== undefined) return id;
  }
  return 0;
}

// ───────────────────────── ops ─────────────────────────

function applyAttr(el: Element, name: string, value: string) {
  const lower = name.toLowerCase();
  if (lower.startsWith('on') || FORBIDDEN_ATTRS.has(lower)) return violation(`attribute ${name}`);
  // Assigning canvas width/height clears the bitmap even if unchanged; the
  // canvas command stream ('r') is authoritative for resets.
  if (el instanceof HTMLCanvasElement && (lower === 'width' || lower === 'height') && getAttribute(el, lower) === value) return;
  setAttribute(el, name, value);
}

function applyOp(op: RenderOp) {
  switch (op.op) {
    case 'el': {
      const tag = op.tag.toLowerCase();
      if (FORBIDDEN_TAGS.has(tag)) return violation(`element <${op.tag}>`);
      let el: Element;
      if (op.adopt) canvasDirty = true;
      if (op.adopt === 'html') el = HTML;
      else if (op.adopt === 'head') el = HEAD;
      else if (op.adopt === 'body') el = BODY;
      else el = op.ns === 'svg' ? createElementNS(doc, SVG_NS, op.tag) : createElement(doc, op.tag);
      register(op.id, el);
      for (const [name, value] of op.attrs) applyAttr(el, name, value);
      return;
    }
    case 'text':
      return register(op.id, createTextNode(doc, op.data));
    case 'comment':
      return;
  }
  const node = nodes.get(op.id);
  if (!node) return;
  // What can change the root's or the body's background: their own attributes and styles, any stylesheet.
  if (node === HTML || node === BODY || op.op === 'css' || ((op.op === 'ins' || op.op === 'rm') && node instanceof HTMLStyleElement)) canvasDirty = true;
  switch (op.op) {
    case 'attr':
      applyAttr(node as Element, op.name, op.value);
      if (node === HTML && op.name.toLowerCase() === 'style') applyThemeVariables();
      return;
    case 'rattr':
      removeAttribute(node as Element, op.name);
      if (node === HTML && op.name.toLowerCase() === 'style') applyThemeVariables();
      return;
    case 'data':
      return setCharData(node, op.data);
    case 'ins': {
      if (node === HTML || node === HEAD || node === BODY) return;
      if (op.parent === 0) {
        const p = parentNodeOf(node);
        if (p) removeChild(p, node);
        return;
      }
      const parent = nodes.get(op.parent);
      if (!parent) return;
      const ref = op.before === null ? null : nodes.get(op.before) ?? null;
      insertBefore(parent, node, ref && parentNodeOf(ref) === parent ? ref : null);
      return;
    }
    case 'rm': {
      if (node === HTML || node === HEAD || node === BODY) return;
      const p = parentNodeOf(node);
      if (p) removeChild(p, node);
      forget(node);
      return;
    }
    case 'style': {
      // The host's theme variables on the root are not the widget's to change (it can shadow them on its own elements).
      if (node === HTML && THEME_VARIABLES.includes(op.name)) return;
      const style = styleOf(node as Element);
      if (op.value === '') style.removeProperty(op.name);
      else style.setProperty(op.name, op.value, op.important ? 'important' : '');
      return;
    }
    case 'css':
      // Sanitized stylesheet text for a <style> element.
      if (localNameOf(node) === 'style') setTextContent(node, op.text);
      return;
    case 'prop':
      return applyProp(node as any, op.name, op.value);
    case 'call':
      return applyCall(node as any, op.method, op.args);
    case 'canvas':
      if (node instanceof HTMLCanvasElement) replay(op.id, node, op.cmds);
      return;
  }
}

function applyProp(el: Element, name: string, value: string | number | boolean) {
  switch (name) {
    case 'scrollTop':
      return scrollTopSet(el, value);
    case 'scrollLeft':
      return scrollLeftSet(el, value);
  }
}

/** The profile's element methods (no focus, selection, dialogs, popovers or scrollIntoView: docs/profile-v1.md). */
const METHOD_IMPL: Record<string, (el: any, a: any[]) => void> = {
  scrollTo: (el, a) => Element.prototype.scrollTo.call(el, a[0], a[1]),
  scrollBy: (el, a) => Element.prototype.scrollBy.call(el, a[0], a[1]),
  setPointerCapture: (el, a) => {
    try { Element.prototype.setPointerCapture.call(el, a[0]); } catch {}
  },
  releasePointerCapture: (el, a) => {
    try { Element.prototype.releasePointerCapture.call(el, a[0]); } catch {}
  },
  observeResize: (el) => resizeObserver.observe(el),
  unobserveResize: (el) => resizeObserver.unobserve(el),
};

function applyCall(el: Element, method: string, args: unknown[]) {
  const impl = Object.prototype.hasOwnProperty.call(METHOD_IMPL, method) ? METHOD_IMPL[method] : null;
  if (impl) impl(el, args);
}

// ───────────────────────── canvas replay ─────────────────────────

interface CanvasState {
  ctx: CanvasRenderingContext2D;
  objects: Map<number, CanvasGradient | CanvasPattern | null>;
  paths: Map<number, Path2D>;
}
const canvases = new Map<number, CanvasState>();

function canvasState(id: number, el: HTMLCanvasElement): CanvasState | null {
  let st = canvases.get(id);
  if (!st) {
    const ctx = HTMLCanvasElement.prototype.getContext.call(el, '2d') as CanvasRenderingContext2D | null;
    if (!ctx) return null;
    st = { ctx, objects: new Map(), paths: new Map() };
    canvases.set(id, st);
  }
  return st;
}

function resolveArg(st: CanvasState, a: any): any {
  if (a && typeof a === 'object' && !Array.isArray(a)) {
    if ('g' in a) return st.objects.get(a.g) ?? undefined;
    if ('p' in a) return st.paths.get(a.p) ?? undefined;
    if ('n' in a) return nodes.get(a.n);
    if ('b64' in a) {
      try { return new ImageData(decodeBase64(a.b64), a.w, a.h); } catch { return undefined; }
    }
  }
  return a;
}

function replay(id: number, el: HTMLCanvasElement, cmds: CanvasCmd[]) {
  const st = canvasState(id, el);
  if (!st) return;
  const ctx = st.ctx as any;
  for (const cmd of cmds) {
    try {
      const [kind, name, ...rest] = cmd as [string, any, ...any[]];
      switch (kind) {
        case 'c': {
          const args = rest.map((a) => resolveArg(st, a));
          if (args.includes(undefined)) break;
          ctx[name](...args);
          break;
        }
        case 's':
          ctx[name] = resolveArg(st, rest[0]);
          break;
        case 'r':
          el.width = name;
          el.height = rest[0];
          break;
        case 'g': {
          const [type, ...args] = rest;
          let obj: CanvasGradient | CanvasPattern | null = null;
          if (type === 'linear') obj = ctx.createLinearGradient(...args);
          else if (type === 'radial') obj = ctx.createRadialGradient(...args);
          else if (type === 'conic') obj = ctx.createConicGradient(...args);
          else if (type === 'pattern') {
            const src = resolveArg(st, args[0]);
            if (src) obj = ctx.createPattern(src, args[1] ?? 'repeat');
          }
          st.objects.set(name, obj);
          break;
        }
        case 'gs':
          (st.objects.get(name) as CanvasGradient | undefined)?.addColorStop(rest[0], rest[1]);
          break;
        case 'pn':
          st.paths.set(name, typeof rest[0] === 'string' ? new Path2D(rest[0]) : new Path2D());
          break;
        case 'pc': {
          const path = st.paths.get(name) as any;
          const [method, ...args] = rest;
          if (path) path[method](...args.map((a: any) => resolveArg(st, a)));
          break;
        }
      }
    } catch {
      // A failing draw call (bad argument) must not abort the rest of the frame.
    }
  }
}

// ───────────────────────── queries ─────────────────────────

let scratch: CanvasRenderingContext2D | null = null;

function answer(q: Query): { result: unknown; binary?: ArrayBuffer } {
  const el = 'id' in q ? (nodes.get(q.id) as any) : null;
  if ('id' in q && !(el instanceof Element)) return { result: { error: 'no such element' } };
  switch (q.q) {
    case 'rect':
      return { result: rectJson(getBoundingClientRect(el), env.origin()) };
    case 'rects': {
      const o = env.origin();
      return { result: Array.from(getClientRects(el), (r) => rectJson(r, o)) };
    }
    case 'box': {
      const g = (name: string) => getter(Element.prototype, name)(el);
      const html = el instanceof HTMLElement;
      const h = (name: string) => (html ? getter(HTMLElement.prototype, name)(el) : 0);
      return {
        result: {
          offsetWidth: h('offsetWidth'), offsetHeight: h('offsetHeight'), offsetTop: h('offsetTop'), offsetLeft: h('offsetLeft'),
          offsetParent: html ? idOf(h('offsetParent')) : 0,
          clientWidth: g('clientWidth'), clientHeight: g('clientHeight'), clientTop: g('clientTop'), clientLeft: g('clientLeft'),
          scrollWidth: g('scrollWidth'), scrollHeight: g('scrollHeight'), scrollTop: g('scrollTop'), scrollLeft: g('scrollLeft'),
        },
      };
    }
    case 'cs': {
      const cs = getComputedStyle(el, q.pseudo ?? null);
      if (q.prop) return { result: cs.getPropertyValue(q.prop) };
      const out: Record<string, string> = {};
      for (let i = 0; i < cs.length; i++) out[cs[i]] = cs.getPropertyValue(cs[i]);
      return { result: out };
    }
    case 'svg': {
      try {
        const args = q.args ?? [];
        switch (q.method) {
          case 'getBBox': {
            const b = (el as SVGGraphicsElement).getBBox();
            return { result: { x: b.x, y: b.y, width: b.width, height: b.height } };
          }
          case 'getCTM':
            return { result: matrixJson((el as any)[q.method]()) };
          case 'getScreenCTM': {
            const m = matrixJson((el as SVGGraphicsElement).getScreenCTM());
            const o = env.origin();
            if (m && o) { m.e -= o.x; m.f -= o.y; }
            return { result: m };
          }
          case 'getPointAtLength': {
            const p = (el as SVGGeometryElement).getPointAtLength(args[0] ?? 0);
            return { result: { x: p.x, y: p.y } };
          }
          case 'getComputedTextLength': case 'getTotalLength': case 'getNumberOfChars':
            return { result: (el as any)[q.method]() };
          case 'getSubStringLength':
            return { result: (el as SVGTextContentElement).getSubStringLength(args[0] ?? 0, args[1] ?? 0) };
        }
      } catch (e) {
        return { result: { error: String(e) } };
      }
      return { result: { error: 'unsupported' } };
    }
    case 'measureText': {
      scratch ??= HTMLCanvasElement.prototype.getContext.call(createElement(doc, 'canvas'), '2d') as CanvasRenderingContext2D;
      scratch.font = '10px sans-serif';
      scratch.font = q.font;
      const m = scratch.measureText(q.text);
      return {
        result: {
          width: m.width, actualBoundingBoxLeft: m.actualBoundingBoxLeft, actualBoundingBoxRight: m.actualBoundingBoxRight,
          actualBoundingBoxAscent: m.actualBoundingBoxAscent, actualBoundingBoxDescent: m.actualBoundingBoxDescent,
          fontBoundingBoxAscent: m.fontBoundingBoxAscent, fontBoundingBoxDescent: m.fontBoundingBoxDescent,
        },
      };
    }
    case 'imageData': {
      const st = canvasState(q.id, el);
      if (!st) return { result: { error: 'no context' } };
      const data = st.ctx.getImageData(q.x, q.y, q.w, q.h);
      const buf = data.data.buffer.slice(0);
      return { result: { width: data.width, height: data.height }, binary: buf };
    }
    case 'canvasCall': {
      const st = canvasState(q.id, el);
      if (!st) return { result: { error: 'no context' } };
      const args = (q.args ?? []).map((a) => resolveArg(st, a));
      try {
        switch (q.method) {
          case 'isPointInPath': case 'isPointInStroke':
            return { result: (st.ctx as any)[q.method](...args) };
          case 'getTransform':
            return { result: matrixJson(st.ctx.getTransform()) };
          case 'getLineDash':
            return { result: st.ctx.getLineDash() };
          case 'toDataURL':
            return { result: HTMLCanvasElement.prototype.toDataURL.call(el, ...(args as [string?, number?])) };
        }
      } catch (e) {
        return { result: { error: String(e) } };
      }
      return { result: { error: 'unsupported' } };
    }
    case 'elementFromPoint': {
      const o = env.origin();
      return { result: idOf(elementFromPoint(root, q.x + (o?.x ?? 0), q.y + (o?.y ?? 0))) };
    }
    case 'validity': {
      const v = (el as HTMLInputElement).validity;
      if (!v) return { result: { valid: true, message: '' } };
      const out: Record<string, unknown> = { message: (el as HTMLInputElement).validationMessage };
      for (const k of ['valid', 'valueMissing', 'typeMismatch', 'patternMismatch', 'tooLong', 'tooShort', 'rangeUnderflow', 'rangeOverflow', 'stepMismatch', 'badInput', 'customError'] as const) out[k] = v[k];
      return { result: out };
    }
    case 'matchMedia':
      return { result: { matches: env.matchMedia(q.query) } };
    case 'viewport':
      return { result: env.viewport() };
    case 'anims':
      return { result: { list: cssAnimationList(animations, el, q.subtree === true, idOf) } };
  }
}

// ───────────────────────── events ─────────────────────────

let interest = new Set<string>();
/** "Learned preventDefault": keys reported by the sandbox for which the default is cancelled synchronously. */
let preventKeys = new Set<string>();
const pendingMoves = new Map<string, WidgetEvent>();
let moveFrame = 0;
/** Pending `requestFrame` from the host (one at a time: the shim coalesces its callbacks into one request). */
let frameReq = 0;
/** Set by the `frozen` op: the runtime is gone, the last render stays as a static snapshot. */
let frozen = false;

function serialize(e: Event, targetId: number): WidgetEvent {
  const ev: WidgetEvent = { type: e.type, targetId };
  const t = e.target as any;
  const o = env.origin();
  if (e instanceof MouseEvent) {
    ev.clientX = e.clientX; ev.clientY = e.clientY; ev.offsetX = e.offsetX; ev.offsetY = e.offsetY;
    ev.pageX = e.pageX; ev.pageY = e.pageY; ev.movementX = e.movementX; ev.movementY = e.movementY;
    // Shadow root in the chat page: client coordinates relative to the widget, and its "page" never scrolls.
    if (o) { ev.clientX -= o.x; ev.clientY -= o.y; ev.pageX = ev.clientX; ev.pageY = ev.clientY; }
    ev.button = e.button; ev.buttons = e.buttons; ev.detail = e.detail;
    if (e.relatedTarget) ev.relatedId = idOf(e.relatedTarget as Node);
  }
  if (e instanceof PointerEvent) {
    ev.pointerId = e.pointerId; ev.pointerType = e.pointerType; ev.isPrimary = e.isPrimary; ev.pressure = e.pressure;
  }
  if (e instanceof MouseEvent) {
    const m = e as MouseEvent;
    ev.modifiers = (m.altKey ? 1 : 0) | (m.ctrlKey ? 2 : 0) | (m.metaKey ? 4 : 0) | (m.shiftKey ? 8 : 0);
  }
  if (t instanceof HTMLImageElement && e.type === 'load') { ev.naturalWidth = t.naturalWidth; ev.naturalHeight = t.naturalHeight; }
  if (t instanceof HTMLDetailsElement && e.type === 'toggle') ev.open = t.open;
  return ev;
}

/**
 * Which drags of the touch that starts on `el` the widget takes for itself
 * (docs/sdk-v1.md §5.5): `x` when an ancestor's computed `touch-action`
 * forbids horizontal panning (the chat's swipe-to-reply), `y` when one forbids
 * vertical panning (the list scroll); both when the sandbox has learned to
 * prevent pointerdown / touchstart on this target. The host tells the client
 * (`onGestureClaim`); hosts whose WebView lives inside a scrolling container
 * that cannot see into it (Android) also act on `y` themselves.
 */
function gestureClaim(el: Element | null, targetId: number): { x: boolean; y: boolean } {
  if (preventKeys.has(`${targetId}:pointerdown`) || preventKeys.has(`${targetId}:touchstart`)) return { x: true, y: true };
  let x = false, y = false;
  for (let n: Element | null = el; n && n !== HTML; n = parentNodeOf(n) as Element | null) {
    if (!(n instanceof Element)) break;
    const ta = getComputedStyle(n).touchAction;
    if (ta === 'auto' || ta === 'manipulation') continue;
    if (ta === 'none') return { x: true, y: true };
    if (!/\bpan-(x|left|right)\b/.test(ta)) x = true;
    if (!/\bpan-(y|up|down)\b/.test(ta)) y = true;
  }
  return { x, y };
}

function onEvent(e: Event) {
  const type = e.type;
  const target = e.target instanceof Node ? e.target : null;
  const targetEl = target instanceof Element ? target : null;

  // Unconditional navigation cancelling. Nothing the validator emits can
  // navigate, but the renderer does not rely on that. A `#fragment` link does
  // not scroll either: the widget does not scroll, and on the Shadow DOM
  // renderer the page that would scroll is the chat.
  if (type === 'click' && targetEl && closest(targetEl, 'a, area')) e.preventDefault();
  // A click without a pointer press (detail 0: Enter or Space on a focused button, assistive technology) is not input the widget gets.
  if (type === 'click' && (e as MouseEvent).detail === 0 && e.isTrusted) {
    e.preventDefault();
    return;
  }

  // A frozen widget still cancels navigation (above) but forwards nothing: there is no runtime to deliver to.
  if (frozen) return;
  // Only events dispatched to (or through) widget nodes are of interest.
  const targetId = target === root || target === null ? ids.get(HTML) ?? 0 : idOf(target);
  if (!targetId) return;
  if (preventKeys.size && e.cancelable && preventKeys.has(`${targetId}:${type}`)) e.preventDefault();
  if (type === 'pointerdown' && (e as PointerEvent).isPrimary) { const c = gestureClaim(targetEl, targetId); post({ type: 'gesture', wants: c.y, x: c.x, y: c.y }); }
  if (!ALWAYS_FORWARD.has(type) && !interest.has(type)) return;
  // enter/leave do not bubble: with a capturing document listener they are seen once per entered element.
  const ev = serialize(e, targetId);
  if (THROTTLED.has(type)) {
    pendingMoves.set(type + ':' + (ev.pointerId ?? 0), ev);
    if (!moveFrame) {
      moveFrame = requestAnimationFrame(() => {
        moveFrame = 0;
        for (const m of pendingMoves.values()) post({ type: 'event', ev: m, trusted: false });
        pendingMoves.clear();
      });
    }
    return;
  }
  // Keep ordering: a queued move must not arrive after the up/click that followed it.
  if (pendingMoves.size) {
    for (const m of pendingMoves.values()) post({ type: 'event', ev: m, trusted: false });
    pendingMoves.clear();
  }
  post({ type: 'event', ev, trusted: e.isTrusted });
}

// ───────────────────────── size reporting ─────────────────────────

let lastSize = '';
function reportSize() {
  const msg: RendererToHost = { type: 'size', width: HTML.clientWidth, height: env.docHeight(), ...env.viewport() };
  const key = JSON.stringify(msg);
  if (key !== lastSize) {
    lastSize = key;
    post(msg);
  }
}

const resizeObserver = new ResizeObserver((entries) => {
  const out: { id: number; width: number; height: number }[] = [];
  for (const entry of entries) {
    const id = ids.get(entry.target);
    if (id !== undefined) out.push({ id, width: entry.contentRect.width, height: entry.contentRect.height });
  }
  if (out.length) post({ type: 'resize', entries: out });
});

// ───────────────────────── wiring ─────────────────────────

function onMessage(msg: HostToRenderer) {
  switch (msg.type) {
    case 'ops':
      for (const op of msg.ops) {
        try { applyOp(op); } catch (e) { violation('op failed: ' + String(e)); }
      }
      syncCanvas();
      reportSize();
      break;
    case 'query': {
      let res: { result: unknown; binary?: ArrayBuffer };
      try { res = answer(msg.query); } catch (e) { res = { result: { error: String(e) } }; }
      post({ type: 'result', qid: msg.qid, result: res.result, binary: res.binary }, res.binary ? [res.binary] : []);
      break;
    }
    case 'listen':
      interest = new Set(msg.types);
      break;
    case 'preventDefault':
      preventKeys = new Set(msg.keys);
      break;
    case 'anim':
      try { applyAnims(animations, msg.now, msg.anims, (id) => nodes.get(id)); } catch (e) { violation('anim failed: ' + String(e)); }
      break;
    case 'requestFrame':
      if (frozen || frameReq) break;
      frameReq = requestAnimationFrame((time) => { frameReq = 0; post({ type: 'frame', time }); });
      break;
    case 'frozen':
      freeze(String(msg.reason), !!msg.fatal);
      break;
    case 'theme':
      themeVars = themeVariables(msg.theme);
      applyThemeVariables();
      canvasDirty = true;
      syncCanvas();
      break;
  }
}

// ───────────────────────── theme variables (docs/sdk-v1.md §2.3, §2.4, §3.4) ─────────────────────────

let themeVars: Record<string, string> = {};

/** The host's variables, inline and important on the widget's root: no widget stylesheet or root style op overrides them. */
function applyThemeVariables() {
  if (!HTML) return;
  const style = styleOf(HTML);
  for (const name of THEME_VARIABLES) {
    const v = themeVars[name];
    if (v === undefined) style.removeProperty(name);
    else style.setProperty(name, v, 'important');
  }
}

// ───────────────────────── the canvas of a widget that is not a document (shadow renderer) ─────────────────────────

let canvasDirty = true;
/** Painted on the canvas element; `background-attachment` stays `scroll` (fixed would follow the chat page's viewport). */
const CANVAS_PROPS = ['background-color', 'background-image', 'background-position', 'background-size', 'background-repeat', 'background-origin', 'background-clip'];
const transparentColor = (c: string) => c === 'transparent' || /^rgba\(\s*0,\s*0,\s*0,\s*0\s*\)$/.test(c);

/**
 * CSS paints a document's canvas with the root element's background, or with the body's when the root has none
 * (and a body background then reaches the whole bubble, its margins included). A shadow root has no canvas, so the
 * core does it on `env.canvas` after every change that can move either background. Widget content only: the values
 * are the widget's own computed, validated ones; the bubble's own colour is behind the renderer, never painted here.
 */
function syncCanvas() {
  const canvas = env.canvas;
  if (!canvas || !canvasDirty || !HTML || !BODY) return;
  canvasDirty = false;
  const root = getComputedStyle(HTML);
  const src = root.backgroundImage !== 'none' || !transparentColor(root.backgroundColor) ? root : getComputedStyle(BODY);
  const style = styleOf(canvas);
  for (const p of CANVAS_PROPS) style.setProperty(p, src.getPropertyValue(p));
}

/** Whether a press on `t` may start a text selection: the nearest `user-select` that is not `auto`, from the target up to the root, where the UA layer says none (D18). */
function selectable(t: EventTarget | null): boolean {
  let el: Element | null = t instanceof Element ? t : t instanceof Node ? t.parentElement : null;
  while (el) {
    const cs = getComputedStyle(el);
    const v = cs.userSelect || (cs as unknown as { webkitUserSelect?: string }).webkitUserSelect;
    if (v && v !== 'auto') return v !== 'none';
    if (el === HTML) return false;
    el = el.parentElement;
  }
  return false;
}

// ───────────────────────── frozen state (last render as fallback) ─────────────────────────

/**
 * The host reports that the widget's runtime died (fatal) or was stopped. The
 * DOM stays exactly as it was (the shim flushes ops only at the end of a task,
 * so a kill mid-task leaves the previous consistent state), events and frames
 * stop, and a small badge — a renderer element, never part of the widget's
 * node table — tells the user. Both the stylesheet and the badge are trusted
 * renderer content; nothing here comes from the widget.
 */
function freeze(reason: string, fatal: boolean) {
  if (frozen) return;
  frozen = true;
  if (frameReq) { cancelAnimationFrame(frameReq); frameReq = 0; }
  if (moveFrame) { cancelAnimationFrame(moveFrame); moveFrame = 0; }
  pendingMoves.clear();
  setAttribute(HTML, 'data-tw-frozen', fatal ? 'fatal' : 'paused');
  const style = createElement(doc, 'style');
  setTextContent(style, '[data-tw-frozen]{pointer-events:none}[data-tw-frozen=fatal]{filter:grayscale(.3)}' +
    '[data-tw-badge]{position:fixed;right:6px;bottom:6px;z-index:2147483647;font:11px/1.3 system-ui,sans-serif;color:#fff;background:rgba(60,60,60,.75);padding:2px 7px;border-radius:9px;pointer-events:none;user-select:none}');
  insertBefore(HEAD, style, null);
  const badge = createElement(doc, 'div');
  setAttribute(badge, 'data-tw-badge', fatal ? 'fatal' : 'paused');
  setTextContent(badge, reason);
  insertBefore(BODY, badge, null);
}

let sizeObservers: ResizeObserver[] = [];
let offViewport: (() => void) | null = null;

/** Input the chat keeps (SWALLOWED): never forwarded to the widget, and without its default action inside it. */
function swallow(e: Event) {
  if (e.type === 'mousedown' || e.type === 'selectstart') {
    // No focus move and no selection from a press, unless it lands on text the widget made selectable (user-select: text,
    // D18); the press itself still reaches the widget (pointerdown, mousedown, :active).
    if (e.cancelable && !selectable(e.target)) e.preventDefault();
    return;
  }
  if (e.type === 'focusin') {
    const t = e.target;
    if (t instanceof HTMLElement || t instanceof SVGElement) (t instanceof SVGElement ? SVGElement.prototype.blur : HTMLElement.prototype.blur).call(t);
    return;
  }
  // Default actions only: on the Shadow DOM renderer the chat page's own handlers (its message menu) still see the event.
  if (e.cancelable) e.preventDefault();
}

function init(s: Send) {
  send = s;
  ({ html: HTML, head: HEAD, body: BODY } = env.roots());
  // The renderer's own stylesheet (trusted, from the profile): the root and text defaults, text not selectable unless the
  // widget says user-select: text, the client-styled <button>.
  const ua = createElement(doc, 'style');
  setTextContent(ua, PROFILE_UA_STYLESHEET);
  insertBefore(HEAD, ua, firstChildOf(HEAD));
  for (const type of SWALLOWED) root.addEventListener(type, swallow, { capture: true, passive: false });
  for (const type of ALL_EVENTS) root.addEventListener(type, onEvent, { capture: true, passive: false });
  // Chromium dispatches enter/leave events only to nodes that listen for them or have an ancestor element with a
  // capturing listener; a shadow root is no element, so without this the widget never gets pointerenter / mouseenter.
  if (root instanceof ShadowRoot) for (const type of ENTER_LEAVE) root.host.addEventListener(type, noop, true);
  for (const type of ['auxclick', 'dragstart', 'dblclick']) root.addEventListener(type, (e) => e.preventDefault(), true);
  sizeObservers = [new ResizeObserver(reportSize), new ResizeObserver(reportSize)];
  sizeObservers[0].observe(HTML);
  sizeObservers[1].observe(BODY);
  const offResize = env.onViewportResize(reportSize);
  const offScheme = env.onColorSchemeChange(() => post({ type: 'media' }));
  offViewport = () => { offResize(); offScheme(); };
  reportSize();
}

function destroy() {
  for (const type of ALL_EVENTS) root.removeEventListener(type, onEvent, { capture: true });
  for (const type of SWALLOWED) root.removeEventListener(type, swallow, { capture: true });
  if (root instanceof ShadowRoot) for (const type of ENTER_LEAVE) root.host.removeEventListener(type, noop, true);
  for (const o of sizeObservers) o.disconnect();
  sizeObservers = [];
  offViewport?.();
  offViewport = null;
  resizeObserver.disconnect();
  if (frameReq) { cancelAnimationFrame(frameReq); frameReq = 0; }
  if (moveFrame) { cancelAnimationFrame(moveFrame); moveFrame = 0; }
  send = () => {};
}

return { init, onMessage, destroy };
}
