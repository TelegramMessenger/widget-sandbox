// Document construction. linkedom provides the DOM object model; parse5 does
// the HTML parsing so the tree has the same shape a browser would build
// (implied <tbody>, <head>/<body> for bare fragments, SVG name casing, …).

import { parseHTML } from 'linkedom';
import { parse, parseFragment } from 'parse5';

export const SVG_NS = 'http://www.w3.org/2000/svg';
const HTML_NS = 'http://www.w3.org/1999/xhtml';

/**
 * Scripts created by fragment parsing (innerHTML, outerHTML,
 * insertAdjacentHTML) never execute, exactly like in browsers. Without this a
 * script doing `document.body.innerHTML += …` would re-create and re-run
 * itself forever.
 */
export const SCRIPT_INERT = Symbol('tw.scriptInert');

const lw: any = parseHTML('<!DOCTYPE html><html><head></head><body></body></html>');
export const document: any = lw.document;
export const linkedomWindow: any = lw;

function ownerOf(proto: any, name: string): any {
  for (let p = proto; p; p = Object.getPrototypeOf(p)) if (Object.prototype.hasOwnProperty.call(p, name)) return p;
  return null;
}

// Fast setters for the two writes tickers make every frame. Both keep linkedom's
// semantics; capture.ts's dirty tracking wraps them like any other setter.
{
  const sample = document.createElement('div');
  const tokens = sample.classList;
  const classListKey = Object.getOwnPropertySymbols(sample).find((k) => sample[k] === tokens);
  const addToken = Set.prototype.add;
  const classProto = ownerOf(sample, 'className');
  const className = classProto && Object.getOwnPropertyDescriptor(classProto, 'className');
  if (className?.set && classListKey) {
    const set = className.set;
    const SINGLE = /^\S+$/;
    // One class token on an element that already has a class attribute: set
    // the attribute and the classList cache directly instead of clear + split + add.
    Object.defineProperty(classProto, 'className', {
      ...className,
      set(this: any, v: any) {
        const s = String(v);
        const a = s.length <= 256 && SINGLE.test(s) ? this.getAttributeNode('class') : null;
        if (!a) return set.call(this, v);
        const list = this[classListKey];
        if (list && a.value !== s) {
          list.clear();
          addToken.call(list, s);
        }
        a.value = s;
      },
    });
  }
  const textProto = ownerOf(sample, 'textContent');
  const textContent = textProto && Object.getOwnPropertyDescriptor(textProto, 'textContent');
  if (textContent?.set) {
    const set = textContent.set;
    // Replacing an element's sole text child: one replaceChild instead of
    // replaceChildren + appendChild (capture.ts then keeps the node id).
    Object.defineProperty(textProto, 'textContent', {
      ...textContent,
      set(this: any, v: any) {
        const first = this.nodeType === 1 ? this.firstChild : null;
        if (typeof v === 'string' && v !== '' && first && first.nodeType === 3 && !first.nextSibling) {
          this.replaceChild(document.createTextNode(v), first);
          return;
        }
        set.call(this, v);
      },
    });
  }
}

function setAttrs(el: any, attrs: { name: string; value: string; prefix?: string }[]) {
  for (const a of attrs) {
    try {
      el.setAttribute(a.prefix ? `${a.prefix}:${a.name}` : a.name, a.value);
    } catch {
      /* invalid attribute name: skip */
    }
  }
}

/** Append linkedom nodes for the children of a parse5 node. */
export function build(p5: any, parent: any, inertScripts = false): void {
  for (const c of p5.childNodes ?? []) {
    if (c.nodeName === '#text') parent.appendChild(document.createTextNode(c.value));
    else if (c.nodeName === '#comment') parent.appendChild(document.createComment(c.data));
    else if (c.tagName) {
      let el: any;
      try {
        el = c.namespaceURI === SVG_NS ? document.createElementNS(SVG_NS, c.tagName) : document.createElement(c.tagName);
      } catch {
        continue;
      }
      setAttrs(el, c.attrs);
      if (inertScripts && c.tagName === 'script') el[SCRIPT_INERT] = true;
      parent.appendChild(el);
      if (c.tagName === 'template' && c.content && el.content) build(c.content, el.content, inertScripts);
      else build(c, el, inertScripts);
    }
  }
}

/** Parse a full document or a bare fragment into the (single) widget document. */
export function loadDocument(html: string): void {
  const p5: any = parse(html);
  const root = p5.childNodes.find((n: any) => n.tagName === 'html');
  if (!root) return;
  setAttrs(document.documentElement, root.attrs);
  for (const c of root.childNodes) {
    if (c.tagName === 'head') {
      setAttrs(document.head, c.attrs);
      build(c, document.head);
    } else if (c.tagName === 'body') {
      setAttrs(document.body, c.attrs);
      build(c, document.body);
    }
  }
}

/** Parse `html` as the children of `context` (innerHTML semantics). */
export function parseInto(context: any, html: string, target: any = context): void {
  const svg = context.namespaceURI === SVG_NS && context.localName !== 'foreignObject';
  const mock = { nodeName: context.localName, tagName: context.localName, attrs: [], namespaceURI: svg ? SVG_NS : HTML_NS, childNodes: [], parentNode: null };
  const frag = (parseFragment as (ctx: unknown, html: string) => any)(mock, String(html));
  build(frag, target, true);
}
