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

import type { CanvasCmd, HostToRenderer, Query, RenderOp, RendererToHost, WidgetEvent } from '../shared/protocol';

/** Where the core renders: a whole document (iframe, native WebViews) or a shadow root inside the host page. */
export interface RendererEnv {
  /** Element factory. */
  doc: Document;
  /** Listener target, `elementFromPoint`, `getElementById`: the document or the shadow root. */
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
const FORBIDDEN_TAGS = new Set(['script', 'iframe', 'frame', 'frameset', 'object', 'embed', 'link', 'meta', 'base', 'portal', 'noscript', 'set', 'template', 'audio', 'video', 'source', 'track']);
const FORBIDDEN_ATTRS = new Set(['srcdoc', 'srcset', 'action', 'formaction', 'ping', 'background', 'nonce', 'target', 'http-equiv']);

const inputValue = setter(HTMLInputElement.prototype, 'value');
const inputValueGet = getter(HTMLInputElement.prototype, 'value');
const inputChecked = setter(HTMLInputElement.prototype, 'checked');
const inputIndeterminate = setter(HTMLInputElement.prototype, 'indeterminate');
const textareaValue = setter(HTMLTextAreaElement.prototype, 'value');
const textareaValueGet = getter(HTMLTextAreaElement.prototype, 'value');
const selectValue = setter(HTMLSelectElement.prototype, 'value');
const selectIndex = setter(HTMLSelectElement.prototype, 'selectedIndex');
const optionSelected = setter(HTMLOptionElement.prototype, 'selected');
const scrollTopSet = setter(Element.prototype, 'scrollTop');
const scrollLeftSet = setter(Element.prototype, 'scrollLeft');

const rectJson = (r: DOMRect, o: { x: number; y: number } | null) => {
  const dx = o?.x ?? 0, dy = o?.y ?? 0;
  return { x: r.x - dx, y: r.y - dy, width: r.width, height: r.height, top: r.top - dy, left: r.left - dx, right: r.right - dx, bottom: r.bottom - dy };
};
const matrixJson = (m: DOMMatrix | null) => (m ? { a: m.a, b: m.b, c: m.c, d: m.d, e: m.e, f: m.f } : null);

const ALWAYS_FORWARD = new Set(['click', 'input', 'change', 'submit', 'reset', 'load', 'error', 'toggle']);
const THROTTLED = new Set(['pointermove', 'mousemove', 'touchmove', 'wheel', 'scroll']);
const ALL_EVENTS = [
  'click', 'dblclick', 'contextmenu', 'input', 'change', 'submit', 'reset', 'keydown', 'keyup', 'keypress',
  'pointerdown', 'pointerup', 'pointermove', 'pointerenter', 'pointerleave', 'pointerover', 'pointerout', 'pointercancel',
  'mousedown', 'mouseup', 'mousemove', 'mouseenter', 'mouseleave', 'mouseover', 'mouseout',
  'touchstart', 'touchmove', 'touchend', 'touchcancel',
  'focus', 'blur', 'focusin', 'focusout', 'wheel', 'scroll', 'load', 'error', 'toggle',
  'animationstart', 'animationend', 'animationiteration', 'transitionend',
];

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
const getElementById = root instanceof Document ? uncurry(Document.prototype.getElementById) : uncurry(DocumentFragment.prototype.getElementById);
let send: Send = () => {};
const post = (msg: RendererToHost, transfer: Transferable[] = []) => send(msg, transfer);
const violation = (detail: string) => post({ type: 'violation', detail });

let HTML: HTMLElement;
let HEAD: HTMLElement;
let BODY: HTMLElement;

const nodes = new Map<number, Node>();
const ids = new WeakMap<Node, number>();

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
  switch (op.op) {
    case 'attr':
      return applyAttr(node as Element, op.name, op.value);
    case 'rattr':
      return removeAttribute(node as Element, op.name);
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
    case 'value':
      // Only assign when different: preserves caret / IME composition while the user types.
      if (el instanceof HTMLInputElement) {
        if (inputValueGet(el) !== value) inputValue(el, value);
      } else if (el instanceof HTMLTextAreaElement) {
        if (textareaValueGet(el) !== value) textareaValue(el, value);
      } else if (el instanceof HTMLSelectElement) selectValue(el, value);
      else if (el instanceof HTMLProgressElement || el instanceof HTMLMeterElement) setAttribute(el, 'value', String(value));
      return;
    case 'checked':
      if (el instanceof HTMLInputElement) inputChecked(el, value);
      return;
    case 'indeterminate':
      if (el instanceof HTMLInputElement) inputIndeterminate(el, value);
      return;
    case 'selectedIndex':
      if (el instanceof HTMLSelectElement) selectIndex(el, value);
      return;
    case 'selected':
      if (el instanceof HTMLOptionElement) optionSelected(el, value);
      return;
    case 'scrollTop':
      return scrollTopSet(el, value);
    case 'scrollLeft':
      return scrollLeftSet(el, value);
  }
}

const METHOD_IMPL: Record<string, (el: any, a: any[]) => void> = {
  focus: (el) => (el instanceof SVGElement ? SVGElement.prototype.focus : HTMLElement.prototype.focus).call(el, { preventScroll: true }),
  blur: (el) => (el instanceof SVGElement ? SVGElement.prototype.blur : HTMLElement.prototype.blur).call(el),
  select: (el) => {
    if (el instanceof HTMLInputElement) HTMLInputElement.prototype.select.call(el);
    else if (el instanceof HTMLTextAreaElement) HTMLTextAreaElement.prototype.select.call(el);
  },
  setSelectionRange: (el, a) => {
    try {
      if (el instanceof HTMLInputElement) HTMLInputElement.prototype.setSelectionRange.call(el, a[0], a[1]);
      else if (el instanceof HTMLTextAreaElement) HTMLTextAreaElement.prototype.setSelectionRange.call(el, a[0], a[1]);
    } catch {}
  },
  scrollIntoView: (el) => Element.prototype.scrollIntoView.call(el, { block: 'nearest' }),
  scrollTo: (el, a) => Element.prototype.scrollTo.call(el, a[0], a[1]),
  scrollBy: (el, a) => Element.prototype.scrollBy.call(el, a[0], a[1]),
  setPointerCapture: (el, a) => {
    try { Element.prototype.setPointerCapture.call(el, a[0]); } catch {}
  },
  releasePointerCapture: (el, a) => {
    try { Element.prototype.releasePointerCapture.call(el, a[0]); } catch {}
  },
  showModal: (el) => {
    if (el instanceof HTMLDialogElement) try { HTMLDialogElement.prototype.showModal.call(el); } catch {}
  },
  show: (el) => {
    if (el instanceof HTMLDialogElement) HTMLDialogElement.prototype.show.call(el);
  },
  close: (el) => {
    if (el instanceof HTMLDialogElement) HTMLDialogElement.prototype.close.call(el);
  },
  reset: (el) => {
    if (el instanceof HTMLFormElement) HTMLFormElement.prototype.reset.call(el);
  },
  showPopover: (el) => { try { HTMLElement.prototype.showPopover.call(el); } catch {} },
  hidePopover: (el) => { try { HTMLElement.prototype.hidePopover.call(el); } catch {} },
  togglePopover: (el) => { try { HTMLElement.prototype.togglePopover.call(el); } catch {} },
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
  if (e instanceof WheelEvent) {
    ev.deltaX = e.deltaX; ev.deltaY = e.deltaY; ev.deltaMode = e.deltaMode;
  }
  if (e instanceof KeyboardEvent) {
    ev.key = e.key; ev.code = e.code; ev.repeat = e.repeat;
  }
  if (typeof TouchEvent !== 'undefined' && e instanceof TouchEvent) {
    const touch = e.touches[0] ?? e.changedTouches[0];
    if (touch) {
      ev.clientX = touch.clientX; ev.clientY = touch.clientY; ev.pageX = touch.pageX; ev.pageY = touch.pageY;
      if (o) { ev.clientX -= o.x; ev.clientY -= o.y; ev.pageX = ev.clientX; ev.pageY = ev.clientY; }
    }
  }
  if (e instanceof FocusEvent && e.relatedTarget) ev.relatedId = idOf(e.relatedTarget as Node);
  if (e instanceof InputEvent) { ev.inputType = e.inputType; ev.data = e.data; }
  if (e instanceof SubmitEvent && e.submitter) ev.submitterId = idOf(e.submitter);
  if (e instanceof MouseEvent || e instanceof KeyboardEvent || (typeof TouchEvent !== 'undefined' && e instanceof TouchEvent)) {
    const m = e as MouseEvent;
    ev.modifiers = (m.altKey ? 1 : 0) | (m.ctrlKey ? 2 : 0) | (m.metaKey ? 4 : 0) | (m.shiftKey ? 8 : 0);
  }
  // Form control state travels with every event so the sandbox mirror stays current.
  if (t instanceof HTMLInputElement) { ev.value = inputValueGet(t); ev.checked = getter(HTMLInputElement.prototype, 'checked')(t); }
  else if (t instanceof HTMLTextAreaElement) ev.value = textareaValueGet(t);
  else if (t instanceof HTMLSelectElement) {
    ev.value = getter(HTMLSelectElement.prototype, 'value')(t);
    ev.selectedIndex = getter(HTMLSelectElement.prototype, 'selectedIndex')(t);
  }
  if (t instanceof HTMLImageElement && e.type === 'load') { ev.naturalWidth = t.naturalWidth; ev.naturalHeight = t.naturalHeight; }
  if (t instanceof HTMLDetailsElement && e.type === 'toggle') ev.open = t.open;
  if (e.type === 'scroll' && t instanceof Element) { ev.scrollTop = getter(Element.prototype, 'scrollTop')(t); ev.scrollLeft = getter(Element.prototype, 'scrollLeft')(t); }
  return ev;
}

/**
 * Does the widget want the touch gesture that starts on `el` for itself? True
 * when an ancestor's computed `touch-action` forbids vertical panning (the
 * platform scroll gesture) or when the sandbox has learned to prevent
 * pointerdown / touchstart on this target. Hosts whose WebView lives inside
 * a scrolling container that cannot see into it (Android) act on this.
 */
function wantsGesture(el: Element | null, targetId: number): boolean {
  if (preventKeys.has(`${targetId}:pointerdown`) || preventKeys.has(`${targetId}:touchstart`)) return true;
  for (let n: Element | null = el; n && n !== HTML; n = parentNodeOf(n) as Element | null) {
    if (!(n instanceof Element)) break;
    // Natively draggable controls: the engine moves the thumb itself, the host must not scroll instead.
    if (n instanceof HTMLInputElement && n.type === 'range') return true;
    const cs = getComputedStyle(n);
    const ta = cs.touchAction;
    if (ta === 'none') return true;
    if (ta !== 'auto' && ta !== 'manipulation' && !/\bpan-(y|up|down)\b/.test(ta)) return true;
    // The widget's own vertically scrollable area scrolls before the chat does.
    if (/^(auto|scroll)$/.test(cs.overflowY) && n.scrollHeight > n.clientHeight + 1) return true;
  }
  return false;
}

function onEvent(e: Event) {
  const type = e.type;
  const target = e.target instanceof Node ? e.target : null;
  const targetEl = target instanceof Element ? target : null;

  // Unconditional navigation cancelling. Nothing the validator emits can
  // navigate, but the renderer does not rely on that.
  if (type === 'submit') e.preventDefault();
  if (type === 'click' && targetEl) {
    const link = closest(targetEl, 'a, area') as Element | null;
    if (link) {
      e.preventDefault();
      const href = getAttribute(link, 'href');
      if (href && href.length > 1 && href[0] === '#') {
        const dest = getElementById(root, href.slice(1));
        if (dest) Element.prototype.scrollIntoView.call(dest);
      }
    }
  }

  // A frozen widget still cancels navigation (above) but forwards nothing: there is no runtime to deliver to.
  if (frozen) return;
  // Only events dispatched to (or through) widget nodes are of interest.
  const targetId = target === root || target === null ? ids.get(HTML) ?? 0 : idOf(target);
  if (!targetId) return;
  if (preventKeys.size && e.cancelable) {
    const key = `${targetId}:${type}`;
    if (preventKeys.has(key) || (e instanceof KeyboardEvent && preventKeys.has(`${key}:${e.key}`))) e.preventDefault();
  }
  if (type === 'pointerdown' && (e as PointerEvent).isPrimary) post({ type: 'gesture', wants: wantsGesture(targetEl, targetId) });
  if (!ALWAYS_FORWARD.has(type) && !interest.has(type)) return;
  // enter/leave do not bubble: with a capturing document listener they are seen once per entered element.
  const ev = serialize(e, targetId);
  if (THROTTLED.has(type)) {
    pendingMoves.set(type + ':' + (ev.pointerId ?? 0), ev);
    if (!moveFrame) {
      moveFrame = requestAnimationFrame(() => {
        moveFrame = 0;
        for (const m of pendingMoves.values()) post({ type: 'event', ev: m });
        pendingMoves.clear();
      });
    }
    return;
  }
  // Keep ordering: a queued move must not arrive after the up/click that followed it.
  if (pendingMoves.size) {
    for (const m of pendingMoves.values()) post({ type: 'event', ev: m });
    pendingMoves.clear();
  }
  post({ type: 'event', ev });
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
    case 'requestFrame':
      if (frozen || frameReq) break;
      frameReq = requestAnimationFrame((time) => { frameReq = 0; post({ type: 'frame', time }); });
      break;
    case 'frozen':
      freeze(String(msg.reason), !!msg.fatal);
      break;
  }
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

function init(s: Send) {
  send = s;
  ({ html: HTML, head: HEAD, body: BODY } = env.roots());
  for (const type of ALL_EVENTS) root.addEventListener(type, onEvent, { capture: true, passive: false });
  for (const type of ['auxclick', 'dragstart']) root.addEventListener(type, (e) => e.preventDefault(), true);
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
