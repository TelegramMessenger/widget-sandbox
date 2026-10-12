// Sandbox entry point. Runs INSIDE QuickJS (untrusted from the host's point of
// view). Installs the shim over linkedom, builds the widget document and exposes
// `__twDispatch(kind, arg)`, the single function the worker glue calls.

import './prelude';
import * as linkedom from 'linkedom';
import { flushCanvases, ImageData, installCanvas, Context2D, Path2D } from './canvas';
import * as capture from './capture';
import { document, loadDocument, parseInto, SCRIPT_INERT, SVG_NS } from './dom';
import * as ev from './events';
import { pseudoHooks } from './css-select-shim';
import { applyEventState, emitProps, FormData, installForms, isChecked, syntheticToggle } from './forms';
import * as globals from './globals';
import { counters, diag, host, setFlushHook, unsupported } from './host';
import { DOMMatrix, DOMPoint, DOMRect, elementFromPoint, getComputedStyle, installLayout } from './layout';
import { styleOf } from './style';

const G: any = globalThis;
const LD: any = linkedom;
const protoOf = (tag: string) => Object.getPrototypeOf(document.createElement(tag));
const ElementProto = LD.Element.prototype;
const HTMLElementProto = LD.HTMLElement.prototype;
const SVGElementProto = LD.SVGElement.prototype;

function define(proto: any, props: Record<string, PropertyDescriptor | Function>) {
  for (const [name, d] of Object.entries(props)) {
    Object.defineProperty(proto, name, typeof d === 'function' ? { value: d, writable: true, configurable: true } : { configurable: true, ...d });
  }
}

const reportError = (e: unknown) => globals.reportError(e);
ev.setErrorReporter(reportError);

// ───────────────────────── globals from linkedom + our overrides ─────────────────────────

for (const [k, v] of Object.entries(LD)) if (/^[A-Z]/.test(k) && typeof v === 'function') G[k] = v;
const constructible = (name: string, make: (...a: any[]) => any) => {
  const C: any = function (this: any, ...args: any[]) { return make(...args); };
  C.prototype = LD[name].prototype;
  Object.defineProperty(C, 'name', { value: name });
  G[name] = C;
};
constructible('Text', (data = '') => document.createTextNode(String(data)));
constructible('Comment', (data = '') => document.createComment(String(data)));
constructible('DocumentFragment', () => document.createDocumentFragment());
G.Image = function Image(w?: number, h?: number) {
  const img = document.createElement('img');
  if (w !== undefined) img.setAttribute('width', String(w));
  if (h !== undefined) img.setAttribute('height', String(h));
  return img;
};
G.Image.prototype = protoOf('img');
G.Option = function Option(text?: string, value?: string, defaultSelected?: boolean, selected?: boolean) {
  const o = document.createElement('option');
  if (text !== undefined) o.textContent = String(text);
  if (value !== undefined) o.setAttribute('value', String(value));
  if (defaultSelected) o.setAttribute('selected', '');
  if (selected) o.selected = true;
  return o;
};
G.Option.prototype = protoOf('option');
G.Audio = function Audio() { unsupported('Audio'); return document.createElement('audio'); };

Object.assign(G, {
  Event: ev.Event, CustomEvent: ev.CustomEvent, UIEvent: ev.UIEvent, MouseEvent: ev.MouseEvent, PointerEvent: ev.PointerEvent,
  WheelEvent: ev.WheelEvent, KeyboardEvent: ev.KeyboardEvent, InputEvent: ev.InputEvent, FocusEvent: ev.FocusEvent,
  TouchEvent: ev.TouchEvent, SubmitEvent: ev.SubmitEvent, ErrorEvent: ev.ErrorEvent, AnimationEvent: ev.AnimationEvent,
  TransitionEvent: ev.TransitionEvent, EventTarget: ev.EventTarget, MutationObserver: capture.MutationObserver,
  DOMRect, DOMRectReadOnly: DOMRect, DOMPoint, DOMPointReadOnly: DOMPoint, DOMMatrix, DOMMatrixReadOnly: DOMMatrix, SVGMatrix: DOMMatrix,
  CanvasRenderingContext2D: Context2D, Path2D, ImageData, FormData, NetworkDisabledError: globals.NetworkDisabledError,
  OffscreenCanvas: class { constructor() { unsupported('OffscreenCanvas'); throw new Error('NotSupportedError: OffscreenCanvas is not available'); } },
  AudioContext: class { constructor() { unsupported('Web Audio'); throw new Error('NotSupportedError: Web Audio is not available'); } },
  customElements: {
    define() { unsupported('customElements.define'); }, get() { return undefined; }, whenDefined() { return new Promise(() => {}); }, upgrade() {},
  },
});
G.webkitAudioContext = G.AudioContext;

// ───────────────────────── EventTarget / window ─────────────────────────

ev.installEventTarget(LD.EventTarget.prototype);
ev.setEventRoots(G, document);
globals.installWindow({ document, getComputedStyle, viewport: { innerWidth: 400, innerHeight: 300, dpr: 1 } });
ev.installEventTarget(G);
setFlushHook(() => capture.flush());

// ───────────────────────── element.style / innerHTML ─────────────────────────

define(ElementProto, {
  style: { get(this: any) { return styleOf(this); }, set(this: any, v: any) { styleOf(this).cssText = String(v ?? ''); } },
});

const origInnerHTML = Object.getOwnPropertyDescriptor(ElementProto, 'innerHTML')!;
const clearChildren = (n: any) => { while (n.firstChild) n.removeChild(n.firstChild); };
define(ElementProto, {
  innerHTML: {
    get(this: any) { return origInnerHTML.get!.call(this); },
    set(this: any, v: any) {
      if (this.localName === 'template' && this.content) { clearChildren(this.content); parseInto(this, v, this.content); return; }
      clearChildren(this);
      parseInto(this, String(v ?? ''));
    },
  },
  outerHTML: {
    get(this: any) { return this.toString(); },
    set(this: any, v: any) {
      const p = this.parentNode;
      if (!p) return;
      const frag = document.createDocumentFragment();
      parseInto(p.nodeType === 1 ? p : document.body, String(v ?? ''), frag);
      this.replaceWith(...frag.childNodes);
    },
  },
  insertAdjacentHTML(this: any, position: string, html: string) {
    const pos = String(position).toLowerCase();
    const outside = pos === 'beforebegin' || pos === 'afterend';
    const p = this.parentNode;
    if (outside && !p) return;
    const frag = document.createDocumentFragment();
    parseInto(outside && p.nodeType === 1 ? p : this, String(html ?? ''), frag);
    if (pos === 'beforebegin') p.insertBefore(frag, this);
    else if (pos === 'afterbegin') this.insertBefore(frag, this.firstChild);
    else if (pos === 'beforeend') this.appendChild(frag);
    else if (pos === 'afterend') p.insertBefore(frag, this.nextSibling);
  },
});
for (const t of ['style', 'script', 'textarea', 'title']) {
  const p = protoOf(t);
  if (Object.getOwnPropertyDescriptor(p, 'innerHTML')) continue;
  define(p, { innerHTML: { get(this: any) { return this.textContent ?? ''; }, set(this: any, v: any) { this.textContent = String(v ?? ''); } } });
}
if (!('toggleAttribute' in ElementProto)) {
  define(ElementProto, {
    toggleAttribute(this: any, name: string, force?: boolean) {
      const has = this.hasAttribute(name);
      const want = force === undefined ? !has : !!force;
      if (want && !has) this.setAttribute(name, '');
      else if (!want && has) this.removeAttribute(name);
      return want;
    },
  });
}

// ───────────────────────── forms / layout / canvas ─────────────────────────

installForms(protoOf, HTMLElementProto, ElementProto);
// State-backed selector pseudo-classes (css-select only knows attributes).
pseudoHooks.checked = (el) => (el.localName === 'input' ? isChecked(el) : el.localName === 'option' ? !!el.selected : false);
pseudoHooks.indeterminate = (el) => el.localName === 'input' && !!el.indeterminate;
pseudoHooks.focus = (el) => activeElement === el;
pseudoHooks['focus-visible'] = pseudoHooks.focus;
pseudoHooks['focus-within'] = (el) => !!activeElement && el.contains(activeElement);
pseudoHooks['placeholder-shown'] = (el) => (el.localName === 'input' || el.localName === 'textarea') && el.hasAttribute('placeholder') && !el.value;
pseudoHooks.default = (el) => el.localName === 'input' ? el.hasAttribute('checked') : el.localName === 'option' ? el.hasAttribute('selected') : false;
installLayout(ElementProto, HTMLElementProto, SVGElementProto, (el, name, value) => capture.pushProp(el, name, value));
installCanvas(protoOf('canvas'));

/** The click flow shared by real and synthetic clicks: external links go to openLink. */
function afterClick(target: any, e: ev.Event) {
  if (e.defaultPrevented) return;
  const el = target?.nodeType === 1 ? target : target?.parentNode;
  const a = el?.closest?.('a[href]');
  if (!a) return;
  const href = String(a.getAttribute('href') ?? '').trim();
  if (!href || href.startsWith('#')) return;
  if (/^\s*javascript:/i.test(href)) return diag('navigation-blocked', 'javascript: link ignored');
  globals.openLink(href);
}

define(HTMLElementProto, {
  click(this: any) {
    syntheticToggle(this);
    const e = new ev.PointerEvent('click', { bubbles: true, cancelable: true, composed: true, button: 0 });
    ev.dispatchEvent.call(this, e);
    afterClick(this, e);
  },
});
define(protoOf('script'), {
  text: { get(this: any) { return this.textContent ?? ''; }, set(this: any, v: any) { this.textContent = String(v ?? ''); } },
  async: { get() { return false; }, set() {} }, defer: { get() { return false; }, set() {} },
});

// ───────────────────────── stylesheets ─────────────────────────

const styleProto = protoOf('style');
const origSheet = Object.getOwnPropertyDescriptor(styleProto, 'sheet')?.get;
const sheets = new WeakMap<object, any>();
class CSSStyleSheet {
  _el: any;
  disabled = false; href = null; media = { mediaText: '' }; type = 'text/css';
  constructor(el?: any) { this._el = el ?? document.createElement('style'); }
  get ownerNode() { return this._el; }
  get cssRules() { try { return origSheet ? origSheet.call(this._el).cssRules ?? [] : []; } catch { return []; } }
  get rules() { return this.cssRules; }
  insertRule(rule: string, index = 0) {
    const t = this._el.textContent ?? '';
    this._el.textContent = index === 0 ? `${rule}\n${t}` : `${t}\n${rule}`;
    return index;
  }
  addRule(selector: string, style: string, index?: number) { this.insertRule(`${selector}{${style}}`, index ?? this.cssRules.length); return -1; }
  deleteRule(index: number) {
    const rules = [...this.cssRules];
    if (!rules[index]) return;
    this._el.textContent = rules.filter((_, i) => i !== index).map((r: any) => r.cssText).join('\n');
  }
  removeRule(index: number) { this.deleteRule(index); }
  replaceSync(text: string) { this._el.textContent = String(text ?? ''); }
  replace(text: string) { this.replaceSync(text); return Promise.resolve(this); }
}
G.CSSStyleSheet = CSSStyleSheet;
define(styleProto, {
  sheet: { get(this: any) { let s = sheets.get(this); if (!s) sheets.set(this, (s = new CSSStyleSheet(this))); return s; } },
});
let adopted: any[] = [];

// ───────────────────────── document extras ─────────────────────────

let activeElement: any = null;
let currentScript: any = null;
const docProps: Record<string, PropertyDescriptor> = {
  defaultView: { get: () => G },
  readyState: { value: 'loading', writable: true },
  currentScript: { get: () => currentScript },
  activeElement: { get: () => (activeElement && activeElement.isConnected ? activeElement : document.body) },
  hidden: { get: () => globals.isHidden() }, visibilityState: { get: () => (globals.isHidden() ? 'hidden' : 'visible') }, hasFocus: { value: () => !globals.isHidden() },
  referrer: { value: '' }, domain: { value: '' }, URL: { value: 'about:srcdoc' }, documentURI: { value: 'about:srcdoc' },
  characterSet: { value: 'UTF-8' }, charset: { value: 'UTF-8' }, compatMode: { value: 'CSS1Compat' }, contentType: { value: 'text/html' },
  cookie: { get: () => '', set: () => unsupported('document.cookie') },
  scrollingElement: { get: () => document.documentElement },
  fonts: { value: { ready: Promise.resolve(), load: () => Promise.resolve([]), check: () => true, addEventListener() {}, removeEventListener() {}, status: 'loaded', size: 0, forEach() {} } },
  styleSheets: { get: () => [...document.querySelectorAll('style')].map((s: any) => s.sheet).concat(adopted) },
  adoptedStyleSheets: {
    get: () => adopted,
    set: (list: any[]) => {
      for (const s of adopted) if (!list.includes(s)) s._el.remove();
      adopted = [...list];
      for (const s of adopted) if (s instanceof CSSStyleSheet && !s._el.isConnected) document.head.appendChild(s._el);
    },
  },
  forms: { get: () => [...document.querySelectorAll('form')] },
  images: { get: () => [...document.querySelectorAll('img')] },
  links: { get: () => [...document.querySelectorAll('a[href], area[href]')] },
  scripts: { get: () => [...document.querySelectorAll('script')] },
  elementFromPoint: { value: elementFromPoint },
  elementsFromPoint: { value: (x: number, y: number) => { const el = elementFromPoint(x, y); const out = []; for (let n = el; n; n = n.parentNode) if (n.nodeType === 1) out.push(n); return out; } },
  getSelection: { value: () => G.getSelection() },
  execCommand: { value: (cmd: string) => { unsupported('document.execCommand(' + cmd + ')'); return false; } },
  write: { value: () => unsupported('document.write') }, writeln: { value: () => unsupported('document.write') }, open: { value: () => document }, close: { value: () => {} },
  createEvent: {
    value: (type: string) => {
      const t = String(type).toLowerCase();
      const C = t.startsWith('mouse') ? ev.MouseEvent : t.startsWith('key') ? ev.KeyboardEvent : t === 'customevent' ? ev.CustomEvent : t.startsWith('ui') ? ev.UIEvent : ev.Event;
      return new C('');
    },
  },
  timeline: { value: { currentTime: 0 } },
  dir: { value: 'ltr', writable: true },
};
for (const [name, d] of Object.entries(docProps)) Object.defineProperty(document, name, { configurable: true, ...d });
if (typeof document.importNode !== 'function') define(Object.getPrototypeOf(document), { importNode(this: any, n: any, deep = false) { return n.cloneNode(deep); } });
if (typeof document.adoptNode !== 'function') define(Object.getPrototypeOf(document), { adoptNode(this: any, n: any) { return n; } });

// ───────────────────────── scripts ─────────────────────────

const EXECUTED = Symbol('tw.executed');
interface QueuedScript { id: number; el: any; type: 'global' | 'module'; code?: string; src?: string }
const scriptQueue: QueuedScript[] = [];
const running = new Map<number, any>();
let scriptSeq = 1;
const SCRIPT_TYPES = new Set(['', 'text/javascript', 'application/javascript', 'text/ecmascript', 'application/ecmascript', 'module', 'text/jscript']);

function queueScript(el: any) {
  if (el[EXECUTED] || el[SCRIPT_INERT] || !el.isConnected) return;
  const type = String(el.getAttribute('type') ?? '').trim().toLowerCase();
  const src = el.getAttribute('src');
  if (!SCRIPT_TYPES.has(type)) {
    el[EXECUTED] = true;
    if (!/json|importmap|template|x-/.test(type)) diag('script-skipped', `unsupported script type "${type}"`);
    return;
  }
  if (src === null && !(el.textContent ?? '').trim()) return; // nothing to run (yet)
  if (el.hasAttribute('nomodule')) { el[EXECUTED] = true; return; }
  el[EXECUTED] = true;
  const q: QueuedScript = { id: scriptSeq++, el, type: type === 'module' ? 'module' : 'global' };
  if (src !== null) q.src = String(src);
  else q.code = String(el.textContent);
  scriptQueue.push(q);
}

function collectScripts(node: any) {
  for (let c = node.firstChild; c; c = c.nextSibling) {
    if (c.nodeType !== 1) continue;
    if (c.localName === 'script') queueScript(c);
    else collectScripts(c);
  }
}

// ───────────────────────── capture hooks & dirty tracking ─────────────────────────

capture.setHooks({
  elementCreated(el) { if (el.localName === 'script' && el.namespaceURI !== SVG_NS) queueScript(el); },
  attributeChanged(el, name, value) { if (name === 'src' && value !== null && el.localName === 'script') queueScript(el); },
  elementSynced: emitProps,
  flushExtra: flushCanvases,
});
{
  const samples = [
    document, document.createElement('div'), document.createElement('input'), document.createElement('select'), document.createElement('option'),
    document.createElement('textarea'), document.createElement('canvas'), document.createElement('template'), document.createElementNS(SVG_NS, 'svg'),
    document.createTextNode(''), document.createComment(''), document.createDocumentFragment(), document.createAttribute('x'),
  ];
  capture.installDirtyTracking(capture.collectPrototypes(samples), Object.getPrototypeOf(document.createElement('div').classList));
}

// ───────────────────────── preventDefault learning ─────────────────────────

const pdKeys = new Set<string>();
let pdDirty = false;

function dispatchHostEvent(raw: any) {
  const target = capture.nodeById(raw.targetId);
  if (!target) return;
  applyEventState(target, raw);
  if (raw.type === 'focus' || raw.type === 'focusin') activeElement = target;
  else if (raw.type === 'blur' || raw.type === 'focusout') activeElement = null;
  const e = ev.eventFromHost(raw, capture.nodeById);
  ev.dispatchEvent.call(target, e);
  if (raw.type === 'click') afterClick(target, e);
  if (e.defaultPrevented) {
    const key = `${raw.targetId}:${raw.type}` + (raw.type.startsWith('key') && raw.key ? `:${raw.key}` : '');
    if (!pdKeys.has(key)) { pdKeys.add(key); pdDirty = true; }
  }
}

// ───────────────────────── dispatcher ─────────────────────────

const handlers: Record<string, (arg: string) => string | void> = {
  boot(arg) {
    const { html, config } = JSON.parse(arg);
    globals.configureLimits({ maxTimers: Number(config.maxTimers) });
    globals.setHidden(config.visible === false);
    globals.setTheme(config.theme, false);
    globals.setViewport(config.viewport);
    ev.noteInterest('focus');
    ev.noteInterest('blur');
    loadDocument(String(html));
    capture.markDirty(document);
    collectScripts(document.documentElement);
  },
  nextScript() {
    const s = scriptQueue.shift();
    if (!s) return '';
    running.set(s.id, s.el);
    currentScript = s.el;
    return JSON.stringify({ id: s.id, type: s.type, code: s.code, src: s.src });
  },
  scriptDone(arg) {
    const [id, ok] = JSON.parse(arg) as [number, boolean];
    const el = running.get(id);
    running.delete(id);
    currentScript = null;
    if (el && el.hasAttribute('src')) ev.dispatchEvent.call(el, new ev.Event(ok ? 'load' : 'error'));
  },
  loaded() {
    document.readyState = 'interactive';
    ev.dispatchEvent.call(document, new ev.Event('DOMContentLoaded', { bubbles: true }));
    document.readyState = 'complete';
    ev.dispatchEvent.call(document, new ev.Event('readystatechange'));
    ev.dispatchEvent.call(G, new ev.Event('load'));
    ev.dispatchEvent.call(G, new ev.Event('pageshow'));
  },
  endTick() {
    const ran = capture.deliverMutationRecords(reportError);
    ev.flushInterest();
    capture.flushTick();
    if (pdDirty) { pdDirty = false; host('pd', JSON.stringify([...pdKeys])); }
    host('stat', JSON.stringify({ cacheHits: counters.cacheHits, missed: counters.missed }));
    return ran || capture.hasPendingRecords() ? '1' : '';
  },
  event(arg) { dispatchHostEvent(JSON.parse(arg)); },
  timer(arg) { globals.fireTimer(JSON.parse(arg)); },
  frame() { globals.fireFrame(); },
  viewport(arg) {
    globals.setViewport(JSON.parse(arg));
    globals.mediaChanged();
    ev.dispatchEvent.call(G, new ev.Event('resize'));
  },
  resize(arg) { globals.fireResize(JSON.parse(arg), capture.nodeById); },
  theme(arg) { globals.setTheme(JSON.parse(arg), true); globals.mediaChanged(); },
  bridge(arg) { const [event, data] = JSON.parse(arg); globals.fireBridge(String(event), data); },
  visibility(arg) {
    // Same signal a background tab gets: document.hidden / visibilityState flip and `visibilitychange` fires (no pagehide/pageshow).
    const { visible } = JSON.parse(arg);
    if (globals.setHidden(!visible)) ev.dispatchEvent.call(document, new ev.Event('visibilitychange'));
  },
};

Object.defineProperty(G, '__twDispatch', {
  value: (kind: string, arg: string) => {
    const h = Object.prototype.hasOwnProperty.call(handlers, kind) ? handlers[kind] : null;
    if (!h) return;
    try {
      return h(arg);
    } catch (e) {
      reportError(e);
    }
  },
  enumerable: false,
  configurable: false,
  writable: false,
});
