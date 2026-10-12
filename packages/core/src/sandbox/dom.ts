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
