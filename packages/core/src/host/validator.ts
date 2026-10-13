// Validator / sanitizer (security-critical, invariants 2–4).
//
// Input:  untrusted JSON produced inside QuickJS (mutation ops, queries, …).
// Output: RenderOps that are safe to apply to the renderer DOM as-is.
//
// The validator keeps its own model of the widget tree (ids, tags, parents) so
// that decisions never depend on anything the sandbox claims beyond the op
// itself. It is pure TypeScript with no DOM dependency: it can be unit-tested in
// Node and ported 1:1 to native hosts.

import {
  ALLOWED_ATTRS, ANIMATION_VALUE_ATTRS, CANVAS_METHODS, CANVAS_PATH_METHODS, CANVAS_PROPS, CANVAS_QUERY_METHODS,
  CSS_VALUE_ATTRS, DROPPED_TAGS, HTML_TAGS, METHODS, PROPS, SILENT_DROP_ATTRS, SVG_ATTR_CASE, SVG_QUERY_METHODS,
  SVG_TAGS, URL_ATTRS,
} from './allowlist';
import { DEFAULT_ENV, rewriteMediaQuery, sanitizeCssValue, sanitizeStyleAttribute, sanitizeStyleProperty, sanitizeStylesheet, type CssEnv, type Diag } from './css-sanitizer';
import { checkUrl, IMAGE_MIME, isFragment, PLACEHOLDER_IMAGE } from './url-policy';
import { DOCUMENT_ID, EVENT_TYPES, type AnimKeyframe, type AnimRecord, type CanvasCmd, type NodeId, type Query, type RenderOp } from '../shared/protocol';

const MAX_NODES = 50_000;
/** Text nodes, string properties and Path2D data; a chat widget has no use for more, and every byte is copied through the renderer. */
const MAX_TEXT = 64 * 1024;
const COMPAT_MAX_TEXT = 2 * 1024 * 1024;
/**
 * Per-document budget of complex-script text (UTF-16 units at or above U+0590:
 * Hebrew, Arabic, Indic, Thai, CJK, emoji, …) in text nodes. Shaping and line
 * breaking such text is what makes a renderer slow, so a widget gets this much
 * of it in its whole document, counted on create, data change and removal.
 */
const COMPLEX_TEXT_BUDGET = 8192;
const COMPLEX_CHURN_PER_SECOND = 1024;
const TEXT_BUDGET = 256 * 1024;

export function textWork(text: string): number {
  let count = 0;
  let run = false;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    if ((c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7ff) || c >= 0xf900 || (c >= 0xd800 && c <= 0xdbff)) {
      if (run) count++;
      run = false;
      count++;
      if (c >= 0xd800 && c <= 0xdbff && i + 1 < text.length && text.charCodeAt(i + 1) >= 0xdc00 && text.charCodeAt(i + 1) <= 0xdfff) i++;
    } else if (c <= 0x20 || c === 0x2d || c === 0x3f || c === 0x2f || c === 0x200b) {
      if (run) count++;
      run = false;
    } else run = true;
  }
  if (run) count++;
  return Math.max(count, Math.ceil(text.length / 1024));
}

function complexUnits(text: string): number {
  let n = 0;
  for (let i = 0; i < text.length; i++) if (text.charCodeAt(i) >= 0x0590) n++;
  return n;
}
const MAX_ATTR = 4 * 1024 * 1024; // data: URLs can be large
const MAX_OPS_PER_BATCH = 200_000;
const MAX_CANVAS_CMDS = 500_000;
const MAX_ID = 2 ** 31;
/** Renderer-side parent id meaning "detached, but keep the node". */
const LIMBO = 0;

const ATTR_NAME = /^[a-zA-Z_][\w.:\-]*$/;
const TAG_NAME = /^[a-zA-Z][\w.\-]*$/;
const EVENT_TYPE_SET = new Set<string>(EVENT_TYPES);
const SVG_ANIMATION_TAGS = new Set(['animate', 'animateTransform', 'animateMotion']);
const SVG_HREF_FRAGMENT_TAGS = new Set(['use', 'linearGradient', 'radialGradient', 'pattern', 'filter', 'textPath', 'mpath', 'feImage']);

interface NodeInfo {
  id: NodeId;
  kind: 'el' | 'text' | 'comment' | 'doc';
  /** Canonical tag as rendered ('' for non-elements). */
  tag: string;
  ns: 'html' | 'svg';
  /** True if this node (not counting ancestors) has a counterpart in the renderer. */
  rendered: boolean;
  /** <style> element: its text children are sanitized as a stylesheet instead of being rendered. */
  isStyle: boolean;
  adopted: boolean;
  parent: NodeId | null;
  children: NodeId[];
  data: string;
  /** Complex-script units this text node counts against the document budget. */
  complex?: number;
  /** Renderer currently has this node attached under its parent. */
  attached: boolean;
}

/**
 * The allowlists, limits and placeholder this validator enforces, as data, so a
 * native host's own node model (Swift `NativeValidator`) applies the same rules
 * from one source of truth instead of a copy.
 */
export type ValidatorPolicy = 'compat' | 'strict';

export function policyLimits(policy: ValidatorPolicy) {
  return policy === 'strict'
    ? { maxText: MAX_TEXT, maxCanvasString: MAX_TEXT, complexTextBudget: COMPLEX_TEXT_BUDGET, textBudget: TEXT_BUDGET }
    : { maxText: COMPAT_MAX_TEXT, maxCanvasString: MAX_ATTR, complexTextBudget: 0, textBudget: 0 };
}

export function validatorTables(policy: ValidatorPolicy = 'compat'): unknown {
  const limits = policyLimits(policy);
  return {
    html: [...HTML_TAGS], svg: [...SVG_TAGS.entries()], dropped: [...DROPPED_TAGS], attrs: [...ALLOWED_ATTRS],
    silent: [...SILENT_DROP_ATTRS], url: [...URL_ATTRS], css: [...CSS_VALUE_ATTRS], anim: [...ANIMATION_VALUE_ATTRS],
    animTags: [...SVG_ANIMATION_TAGS], fragTags: [...SVG_HREF_FRAGMENT_TAGS], svgCase: [...SVG_ATTR_CASE.entries()],
    props: Object.entries(PROPS).map(([k, v]) => [k, v.type, v.tags ? [...v.tags] : null]), methods: Object.entries(METHODS),
    events: [...EVENT_TYPE_SET], mime: [...IMAGE_MIME], placeholder: PLACEHOLDER_IMAGE,
    limits: [MAX_NODES, limits.maxText, MAX_ATTR, MAX_OPS_PER_BATCH, MAX_CANVAS_CMDS, LIMBO],
    complexTextBudget: limits.complexTextBudget,
    textBudget: limits.textBudget,
  };
}

function isObj(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}
function isId(v: unknown): v is NodeId {
  return typeof v === 'number' && Number.isInteger(v) && v > DOCUMENT_ID && v < MAX_ID;
}
function isStr(v: unknown, max: number): v is string {
  return typeof v === 'string' && v.length <= max;
}
function isNum(v: unknown): v is number {
  return typeof v === 'number' && Number.isFinite(v);
}

export class Validator {
  private nodes = new Map<NodeId, NodeInfo>();
  private adopted = { html: false, head: false, body: false };
  private dirtyStyles = new Set<NodeId>();
  private complexText = 0;
  private totalText = 0;
  private churnTokens = COMPLEX_TEXT_BUDGET;
  private churnAt: number | null = null;
  private clock: () => number = () => Date.now() / 1000;

  setClock(clock: () => number) {
    this.clock = clock;
  }

  private takeChurn(words: number): boolean {
    const now = this.clock();
    if (this.churnAt !== null) this.churnTokens = Math.min(COMPLEX_TEXT_BUDGET, this.churnTokens + Math.max(0, now - this.churnAt) * COMPLEX_CHURN_PER_SECOND);
    this.churnAt = now;
    if (this.churnTokens < words) return false;
    this.churnTokens -= words;
    return true;
  }

  private overChurn(): boolean {
    this.diag('text-churn', `text changes over ${COMPLEX_CHURN_PER_SECOND} units per second`);
    return false;
  }

  private policyName: ValidatorPolicy = 'compat';
  private limits = policyLimits('compat');

  get policy(): ValidatorPolicy { return this.policyName; }

  setPolicy(policy: ValidatorPolicy) {
    this.policyName = policy === 'strict' ? 'strict' : 'compat';
    this.limits = policyLimits(this.policyName);
  }
  private out: RenderOp[] = [];
  private diag: Diag;
  private env: CssEnv;

  constructor(diag: Diag, env: CssEnv = DEFAULT_ENV) {
    this.diag = diag;
    this.env = { ...env };
    this.nodes.set(DOCUMENT_ID, {
      id: DOCUMENT_ID, kind: 'doc', tag: '', ns: 'html', rendered: false, isStyle: false, adopted: false,
      parent: null, children: [], data: '', attached: false,
    });
  }

  get nodeCount(): number {
    return this.nodes.size;
  }

  /** The host theme changed: re-evaluate every stylesheet against it. */
  setTheme(colorScheme: CssEnv['colorScheme']): RenderOp[] {
    this.env = { ...this.env, colorScheme };
    const out: RenderOp[] = [];
    for (const n of this.nodes.values()) {
      if (n.kind === 'el' && n.isStyle && n.rendered) out.push({ op: 'css', id: n.id, text: this.styleText(n) });
    }
    return out;
  }

  private styleText(style: NodeInfo): string {
    const css = style.children.map((c) => this.nodes.get(c)?.data ?? '').join('');
    return sanitizeStylesheet(css, this.diag, this.env);
  }

  /** Media query strings from matchMedia(): prefers-color-scheme is answered from the host theme. */
  rewriteMediaQuery(query: string): string {
    return rewriteMediaQuery(query, this.env);
  }

  /** Would a `canvas` op for `id` be accepted (a known `<canvas>` element)? Native hosts check this before validating canvas words themselves. */
  isCanvas(id: unknown): boolean {
    if (!isId(id)) return false;
    const n = this.nodes.get(id);
    return !!n && n.kind === 'el' && n.tag === 'canvas';
  }

  /**
   * A native host that keeps the node model itself (Swift `NativeValidator`) answers
   * "which rendered element is `id`" for the checks still done here (queries,
   * Web Animations): the tag, or null.
   */
  private nodeLookup: ((id: number) => string | null) | null = null;
  setNodeLookup(lookup: ((id: number) => string | null) | null) {
    this.nodeLookup = lookup;
  }

  // Stateless pieces for a native node model: the same sanitizers with this
  // validator's diagnostics and colour scheme.
  get cssEnv(): CssEnv { return this.env; }
  get diagnostics(): Diag { return this.diag; }

  /** Validated canvas commands, with the same diagnostics as a `canvas` op. */
  canvasCommands(cmds: unknown): CanvasCmd[] {
    if (!Array.isArray(cmds)) return [];
    const out: CanvasCmd[] = [];
    for (const cmd of cmds) {
      const clean = this.canvasCmd(cmd);
      if (clean) out.push(clean);
      else this.diag('protocol-violation', 'rejected canvas cmd ' + safeJson(cmd));
    }
    return out;
  }

  /** Does `id` name an element the renderer knows about? (Used to validate queries.) */
  private renderedElement(id: unknown): NodeInfo | null {
    if (!isId(id)) return null;
    if (this.nodeLookup) {
      const tag = this.nodeLookup(id);
      return tag ? { id, kind: 'el', tag, ns: 'html', rendered: true, isStyle: false, adopted: false, parent: null, children: [], data: '', attached: true } : null;
    }
    const n = this.nodes.get(id);
    return n && n.kind === 'el' && n.rendered ? n : null;
  }

  // ───────────────────────── ops ─────────────────────────

  processOps(batch: unknown): RenderOp[] {
    this.out = [];
    if (!Array.isArray(batch) || batch.length > MAX_OPS_PER_BATCH) {
      this.diag('protocol-violation', 'ops batch is not an array or too large');
      return [];
    }
    for (const op of batch) {
      try {
        if (!isObj(op) || !this.applyOp(op)) this.diag('protocol-violation', 'rejected op ' + safeJson(op));
      } catch (e) {
        this.diag('protocol-violation', 'validator error: ' + (e as Error).message);
      }
    }
    for (const id of this.dirtyStyles) {
      const style = this.nodes.get(id);
      if (style) this.out.push({ op: 'css', id, text: this.styleText(style) });
    }
    this.dirtyStyles.clear();
    const out = this.out;
    this.out = [];
    return out;
  }

  private applyOp(op: Record<string, unknown>): boolean {
    switch (op.op) {
      case 'el': return this.opEl(op);
      case 'text': case 'comment': return this.opCharData(op);
      case 'attr': return this.opAttr(op);
      case 'rattr': return this.opRemoveAttr(op);
      case 'data': return this.opData(op);
      case 'ins': return this.opInsert(op);
      case 'rm': return this.opRemove(op);
      case 'style': return this.opStyle(op);
      case 'prop': return this.opProp(op);
      case 'call': return this.opCall(op);
      case 'canvas': return this.opCanvas(op);
      default: return false;
    }
  }

  private newNode(id: NodeId, partial: Partial<NodeInfo>): NodeInfo | null {
    if (this.nodes.has(id) || this.nodes.size >= MAX_NODES) return null;
    const n: NodeInfo = {
      id, kind: 'el', tag: '', ns: 'html', rendered: true, isStyle: false, adopted: false, parent: null,
      children: [], data: '', attached: false, ...partial,
    };
    this.nodes.set(id, n);
    return n;
  }

  private opEl(op: Record<string, unknown>): boolean {
    const { id, tag, ns, attrs } = op;
    if (!isId(id) || !isStr(tag, 64) || !TAG_NAME.test(tag)) return false;
    if (ns !== undefined && ns !== 'svg') return false;
    if (attrs !== undefined && !isObj(attrs)) return false;
    const lower = tag.toLowerCase();

    // Decide what (if anything) this element becomes in the renderer.
    let outTag: string | null;
    let extra: [string, string][] = [];
    const isStyle = lower === 'style';
    if (ns === 'svg') {
      if (isStyle) outTag = 'style';
      else if (DROPPED_TAGS.has(lower) && lower !== 'title') outTag = null;
      else outTag = SVG_TAGS.get(lower) ?? null;
      if (outTag === null && !DROPPED_TAGS.has(lower)) this.diag('tag-dropped', `<svg:${tag}>`);
    } else if (DROPPED_TAGS.has(lower)) {
      outTag = null;
      if (lower !== 'script' && lower !== 'title' && lower !== 'meta') this.diag('tag-dropped', `<${lower}>`);
    } else if (isStyle || HTML_TAGS.has(lower)) {
      outTag = lower;
    } else {
      // Unknown element (custom element, typo, MathML…): render as an inert span.
      outTag = 'span';
      extra = [['data-tg-tag', lower]];
      this.diag('tag-replaced', `<${lower}> rendered as <span>`);
    }

    let adopt: 'html' | 'head' | 'body' | undefined;
    if (ns !== 'svg' && (lower === 'html' || lower === 'head' || lower === 'body')) {
      if (this.adopted[lower]) {
        outTag = 'div';
        extra = [['data-tg-tag', lower]];
      } else {
        this.adopted[lower] = true;
        adopt = lower;
      }
    }

    const node = this.newNode(id, {
      kind: 'el', tag: outTag ?? '', ns: ns === 'svg' ? 'svg' : 'html', rendered: outTag !== null, isStyle,
      adopted: adopt !== undefined,
    });
    if (!node) return false;
    if (outTag === null) return true; // tracked, never rendered

    const outAttrs: [string, string][] = [...extra];
    if (attrs) {
      for (const [name, value] of Object.entries(attrs)) {
        if (!isStr(value, MAX_ATTR)) continue;
        for (const pair of this.sanitizeAttr(node, name, value)) outAttrs.push(pair);
      }
    }
    this.out.push({ op: 'el', id, tag: outTag, ...(node.ns === 'svg' ? { ns: 'svg' as const } : {}), attrs: outAttrs, ...(adopt ? { adopt } : {}) });
    return true;
  }

  private opCharData(op: Record<string, unknown>): boolean {
    const { id, data } = op;
    if (!isId(id) || !isStr(data, this.limits.maxText)) return false;
    if (this.nodes.has(id) || this.nodes.size >= MAX_NODES) return false;
    if (this.limits.textBudget && this.totalText + data.length > this.limits.textBudget) {
      this.diag('text-budget', `document text over ${this.limits.textBudget} units`);
      return false;
    }
    const kind = op.op === 'text' ? 'text' : 'comment';
    const complex = kind === 'text' ? complexUnits(data) : 0;
    if (complex && this.limits.complexTextBudget && this.complexText + complex > this.limits.complexTextBudget) {
      this.diag('text-budget', `document complex-script text over ${this.limits.complexTextBudget} units`);
      return false;
    }
    if (this.limits.textBudget && !this.takeChurn(textWork(data))) return this.overChurn();
    // Comments are tracked for tree consistency but never rendered.
    const node = this.newNode(id, { kind, data, rendered: kind === 'text', complex });
    if (!node) return false;
    this.complexText += complex;
    this.totalText += data.length;
    if (kind === 'text') this.out.push({ op: 'text', id, data });
    return true;
  }

  private opData(op: Record<string, unknown>): boolean {
    const { id, data } = op;
    if (!isId(id) || !isStr(data, this.limits.maxText)) return false;
    const node = this.nodes.get(id);
    if (!node || node.kind === 'el' || node.kind === 'doc') return false;
    const textGrowth = data.length - node.data.length;
    if (textGrowth > 0 && this.limits.textBudget && this.totalText + textGrowth > this.limits.textBudget) {
      this.diag('text-budget', `document text over ${this.limits.textBudget} units`);
      return false;
    }
    const complex = node.kind === 'text' ? complexUnits(data) : 0;
    const growth = complex - (node.complex ?? 0);
    if (growth > 0 && this.limits.complexTextBudget && this.complexText + growth > this.limits.complexTextBudget) {
      this.diag('text-budget', `document complex-script text over ${this.limits.complexTextBudget} units`);
      return false;
    }
    if (this.limits.textBudget && !this.takeChurn(textWork(data))) return this.overChurn();
    this.complexText += growth;
    this.totalText += textGrowth;
    node.complex = complex;
    node.data = data;
    const parent = node.parent !== null ? this.nodes.get(node.parent) : null;
    if (parent?.isStyle) this.dirtyStyles.add(parent.id);
    else if (node.rendered) this.out.push({ op: 'data', id, data });
    return true;
  }

  private opInsert(op: Record<string, unknown>): boolean {
    const { parent, id, before } = op;
    if (!isId(id) || !(isId(parent) || parent === DOCUMENT_ID)) return false;
    if (before !== null && !isId(before)) return false;
    const node = this.nodes.get(id);
    const p = this.nodes.get(parent as NodeId);
    if (!node || !p || p.kind === 'text' || p.kind === 'comment') return false;
    // Reject cycles: parent must not be the node itself or one of its descendants.
    for (let a: NodeInfo | undefined = p; a; a = a.parent !== null ? this.nodes.get(a.parent) : undefined) {
      if (a === node) return false;
    }
    let ref: NodeInfo | null = null;
    if (before !== null) {
      ref = this.nodes.get(before as NodeId) ?? null;
      if (!ref || ref.parent !== p.id || ref === node) return false;
    }

    // Update the model.
    this.unlink(node);
    node.parent = p.id;
    if (ref) p.children.splice(p.children.indexOf(ref.id), 0, id);
    else p.children.push(id);
    if (p.isStyle) this.dirtyStyles.add(p.id);

    // Decide whether the renderer should attach it.
    const renderHere = node.rendered && !node.adopted && p.rendered && !p.isStyle && p.kind === 'el';
    if (!renderHere) {
      if (p.isStyle && node.rendered && node.kind !== 'el') {
        // Stylesheet text only ever reaches the renderer sanitized (`css` op):
        // forget the raw text node there. The model keeps its data.
        this.out.push({ op: 'rm', id });
        node.rendered = false;
      } else if (node.attached) {
        this.out.push({ op: 'ins', parent: LIMBO, id, before: null });
      }
      node.attached = false;
      return true;
    }
    // The reference node must itself be attached in the renderer; otherwise use
    // the next sibling that is.
    let refId: NodeId | null = null;
    const idx = p.children.indexOf(id);
    for (let i = idx + 1; i < p.children.length; i++) {
      const sib = this.nodes.get(p.children[i]);
      if (sib && sib.attached) {
        refId = sib.id;
        break;
      }
    }
    this.out.push({ op: 'ins', parent: p.id, id, before: refId });
    node.attached = true;
    return true;
  }

  private unlink(node: NodeInfo): void {
    if (node.parent === null) return;
    const old = this.nodes.get(node.parent);
    if (old) {
      const i = old.children.indexOf(node.id);
      if (i >= 0) old.children.splice(i, 1);
      if (old.isStyle) this.dirtyStyles.add(old.id);
    }
    node.parent = null;
  }

  private opRemove(op: Record<string, unknown>): boolean {
    const { id } = op;
    if (!isId(id)) return false;
    const node = this.nodes.get(id);
    if (!node) return false;
    if (node.adopted) {
      // <html>/<head>/<body> cannot be removed from the renderer; empty them instead.
      for (const c of [...node.children]) this.opRemove({ id: c });
      return true;
    }
    this.unlink(node);
    if (node.rendered) this.out.push({ op: 'rm', id });
    // Forget the whole subtree (the renderer does the same).
    const stack = [node];
    while (stack.length) {
      const n = stack.pop()!;
      this.complexText -= n.complex ?? 0;
      this.totalText -= n.data.length;
      this.nodes.delete(n.id);
      this.dirtyStyles.delete(n.id);
      for (const c of n.children) {
        const child = this.nodes.get(c);
        if (child) {
          // Children that were never attached under this parent still exist in the renderer map.
          if (child.rendered && !child.attached) this.out.push({ op: 'rm', id: child.id });
          stack.push(child);
        }
      }
    }
    return true;
  }

  private opAttr(op: Record<string, unknown>): boolean {
    const { id, name, value } = op;
    if (!isId(id) || !isStr(name, 128) || !isStr(value, MAX_ATTR)) return false;
    const node = this.nodes.get(id);
    if (!node || node.kind !== 'el') return false;
    if (!node.rendered) return true;
    const pairs = this.sanitizeAttr(node, name, value);
    if (pairs.length === 0) {
      // The previous (possibly valid) value must not survive a rejected update.
      const canon = this.canonAttrName(node, name);
      if (canon) this.out.push({ op: 'rattr', id, name: canon });
      return true;
    }
    for (const [n, v] of pairs) this.out.push({ op: 'attr', id, name: n, value: v });
    return true;
  }

  private opRemoveAttr(op: Record<string, unknown>): boolean {
    const { id, name } = op;
    if (!isId(id) || !isStr(name, 128)) return false;
    const node = this.nodes.get(id);
    if (!node || node.kind !== 'el') return false;
    if (!node.rendered) return true;
    const canon = this.canonAttrName(node, name);
    if (!canon) return true;
    this.out.push({ op: 'rattr', id, name: canon });
    if (node.tag === 'a' && (canon === 'href' || canon === 'xlink:href')) this.out.push({ op: 'rattr', id, name: 'data-tg-link' });
    return true;
  }

  private canonAttrName(node: NodeInfo, name: string): string | null {
    if (!ATTR_NAME.test(name)) return null;
    const lower = name.toLowerCase();
    if (lower.startsWith('on')) return null;
    // SVG in an HTML document: plain `href` is the namespace-free equivalent of xlink:href.
    if (node.ns === 'svg') return lower === 'xlink:href' ? 'href' : (SVG_ATTR_CASE.get(lower) ?? lower);
    return lower;
  }

  /**
   * The attribute gate. Returns the attribute(s) to set in the renderer:
   * usually one, none if the attribute is dropped.
   */
  private sanitizeAttr(node: NodeInfo, rawName: string, value: string): [string, string][] {
    const name = this.canonAttrName(node, rawName);
    if (name === null) return []; // malformed name or on* handler
    const lower = name.toLowerCase();
    const tag = node.tag;

    if (lower === 'style') return [['style', sanitizeStyleAttribute(value, this.diag, this.env)]];

    if (URL_ATTRS.has(lower)) return this.sanitizeUrlAttr(node, name, lower, value);

    if (lower.startsWith('data-tg-')) return []; // reserved for the host
    if (lower.startsWith('data-') || lower.startsWith('aria-')) return [[lower, value]];
    if (SILENT_DROP_ATTRS.has(lower)) return [];
    if (!ALLOWED_ATTRS.has(lower)) {
      this.diag('attr-dropped', `${tag}[${lower}]`);
      return [];
    }

    if (node.ns === 'svg') {
      if (SVG_ANIMATION_TAGS.has(tag)) {
        if (lower === 'attributename' && /href|src/i.test(value)) return []; // never animate URL attributes
        if (ANIMATION_VALUE_ATTRS.has(lower)) {
          const parts: string[] = [];
          for (const part of value.split(';')) {
            const clean = sanitizeCssValue(part, this.diag, `${tag}[${lower}]`, this.env);
            if (clean === null) return [];
            parts.push(clean);
          }
          return [[name, parts.join(';')]];
        }
      }
      if (CSS_VALUE_ATTRS.has(lower)) {
        const clean = sanitizeCssValue(value, this.diag, `${tag}[${lower}]`, this.env);
        return clean === null ? [] : [[name, clean]];
      }
    }
    return [[name, value]];
  }

  private sanitizeUrlAttr(node: NodeInfo, name: string, lower: string, value: string): [string, string][] {
    const tag = node.tag;
    const block = (): [string, string][] => {
      this.diag('url-blocked', `${tag}[${lower}]=${value.length > 80 ? value.slice(0, 80) + '…' : value}`);
      return [];
    };
    const isHref = lower === 'href' || lower === 'xlink:href';

    if (tag === 'a' && isHref) {
      // Links never navigate. Fragments are kept; anything else renders as an
      // inert "#" link and the real URL stays in the sandbox (openLink flow).
      if (isFragment(value)) {
        const frag = checkUrl(value, 'fragment-only');
        return frag ? [[name, frag]] : [[name, '#']];
      }
      return [[name, '#'], ['data-tg-link', '']];
    }
    if (node.ns === 'svg') {
      if (!isHref) return block();
      if (tag === 'image') {
        const url = checkUrl(value, 'image');
        return url && !url.startsWith('#') ? [[name, url]] : block();
      }
      if (SVG_HREF_FRAGMENT_TAGS.has(tag)) {
        const url = checkUrl(value, tag === 'feImage' ? 'image' : 'fragment-only');
        return url ? [[name, url]] : block();
      }
      return block();
    }
    if (lower === 'src') {
      if (tag === 'img' || tag === 'input') {
        const url = checkUrl(value, 'image');
        if (url && !url.startsWith('#')) return [['src', url]];
        block();
        return tag === 'img' ? [['src', PLACEHOLDER_IMAGE], ['data-tg-placeholder', '']] : [];
      }
      // audio/video/source never get here: the elements are dropped by the allowlist.
      return block();
    }
    if (lower === 'usemap') return [];
    // srcset, poster, action, formaction, background, ping, data, srcdoc, …: never allowed.
    return block();
  }

  private opStyle(op: Record<string, unknown>): boolean {
    const { id, name, value, important } = op;
    if (!isId(id) || !isStr(name, 128) || !isStr(value, MAX_ATTR)) return false;
    const node = this.nodes.get(id);
    if (!node || node.kind !== 'el') return false;
    if (!node.rendered) return true;
    if (!/^-{0,2}[a-zA-Z_][\w-]*$/.test(name)) return false;
    const clean = sanitizeStyleProperty(name, value, this.diag, this.env);
    // A rejected value clears the property rather than leaving a stale one.
    this.out.push({ op: 'style', id, name, value: clean ? clean.value : '', important: important === true });
    return true;
  }

  private opProp(op: Record<string, unknown>): boolean {
    const { id, name, value } = op;
    if (!isId(id) || typeof name !== 'string') return false;
    const spec = Object.prototype.hasOwnProperty.call(PROPS, name) ? PROPS[name] : null;
    const node = this.nodes.get(id);
    if (!spec || !node || node.kind !== 'el') return false;
    if (typeof value !== spec.type) return false;
    if (typeof value === 'string' && value.length > this.limits.maxText) return false;
    if (typeof value === 'number' && !Number.isFinite(value)) return false;
    if (!node.rendered || (spec.tags && !spec.tags.has(node.tag))) return true;
    this.out.push({ op: 'prop', id, name, value: value as string | number | boolean });
    return true;
  }

  private opCall(op: Record<string, unknown>): boolean {
    const { id, method, args } = op;
    if (!isId(id) || typeof method !== 'string') return false;
    if (!Object.prototype.hasOwnProperty.call(METHODS, method)) return false;
    const node = this.nodes.get(id);
    if (!node || node.kind !== 'el') return false;
    if (!node.rendered) return true;
    const n = METHODS[method];
    const out: number[] = [];
    if (n > 0) {
      if (!Array.isArray(args)) return false;
      for (let i = 0; i < n; i++) {
        if (!isNum(args[i])) return false;
        out.push(args[i] as number);
      }
    }
    this.out.push({ op: 'call', id, method, args: out });
    return true;
  }

  // ───────────────────────── canvas ─────────────────────────

  private opCanvas(op: Record<string, unknown>): boolean {
    const { id, cmds } = op;
    if (!isId(id) || !Array.isArray(cmds) || cmds.length > MAX_CANVAS_CMDS) return false;
    const node = this.nodes.get(id);
    if (!node || node.kind !== 'el' || node.tag !== 'canvas') return false;
    const out: CanvasCmd[] = [];
    for (const cmd of cmds) {
      const clean = this.canvasCmd(cmd);
      if (clean) out.push(clean);
      else this.diag('protocol-violation', 'rejected canvas cmd ' + safeJson(cmd));
    }
    if (out.length) this.out.push({ op: 'canvas', id, cmds: out });
    return true;
  }

  /** A canvas argument: finite number, short string, boolean, null, flat number array, or object ref. */
  private canvasArg(v: unknown): unknown {
    if (v === null || typeof v === 'boolean') return v;
    if (typeof v === 'number') return Number.isFinite(v) ? v : undefined;
    if (typeof v === 'string') return v.length <= this.limits.maxCanvasString ? v : undefined;
    if (Array.isArray(v)) return v.length <= 4096 && v.every(isNum) ? v : undefined;
    if (isObj(v)) {
      if (isNum(v.g) && Number.isInteger(v.g)) return { g: v.g }; // gradient / pattern
      if (isNum(v.p) && Number.isInteger(v.p)) return { p: v.p }; // Path2D
      if (isId(v.n)) {
        // image source: must be a rendered canvas / img / SVG image (no <video>: never rendered)
        const n = this.renderedElement(v.n);
        return n && (n.tag === 'canvas' || n.tag === 'img' || n.tag === 'image') ? { n: n.id } : undefined;
      }
      if (isNum(v.w) && isNum(v.h) && typeof v.b64 === 'string') return { w: v.w, h: v.h, b64: v.b64 }; // ImageData
    }
    return undefined;
  }

  private canvasArgs(list: unknown[]): unknown[] | null {
    const out: unknown[] = [];
    for (const a of list) {
      const c = this.canvasArg(a);
      if (c === undefined) return null;
      out.push(c);
    }
    return out;
  }

  private canvasCmd(cmd: unknown): CanvasCmd | null {
    if (!Array.isArray(cmd) || cmd.length < 2 || cmd.length > 16) return null;
    const [kind, name, ...rest] = cmd;
    switch (kind) {
      case 'c': {
        if (typeof name !== 'string' || !CANVAS_METHODS.has(name)) return null;
        const args = this.canvasArgs(rest);
        return args ? ['c', name, ...args] : null;
      }
      case 's': {
        if (typeof name !== 'string' || !CANVAS_PROPS.has(name) || rest.length !== 1) return null;
        let v = this.canvasArg(rest[0]);
        if (v === undefined || Array.isArray(v)) return null;
        if (name === 'filter') {
          // The only canvas property that can reference a URL.
          if (typeof v !== 'string') return null;
          v = sanitizeCssValue(v, this.diag, 'canvas filter', this.env);
          if (v === null || /url/i.test(v as string)) return null;
        }
        return ['s', name, v];
      }
      case 'r':
        return isNum(name) && isNum(rest[0]) && name >= 0 && rest[0] >= 0 && name <= 16384 && rest[0] <= 16384 ? ['r', name, rest[0]] : null;
      case 'g': {
        // ['g', gid, 'linear'|'radial'|'conic'|'pattern', ...args]
        if (!isNum(name) || typeof rest[0] !== 'string' || !['linear', 'radial', 'conic', 'pattern'].includes(rest[0])) return null;
        const args = this.canvasArgs(rest.slice(1));
        return args ? ['g', name, rest[0], ...args] : null;
      }
      case 'gs':
        return isNum(name) && isNum(rest[0]) && typeof rest[1] === 'string' && rest[1].length < 256 ? ['gs', name, rest[0], rest[1]] : null;
      case 'pn':
        // ['pn', pid, svgPathData | null]
        return isNum(name) && (rest[0] === null || isStr(rest[0], this.limits.maxText)) ? ['pn', name, rest[0]] : null;
      case 'pc': {
        // ['pc', pid, method, ...args]
        if (!isNum(name) || typeof rest[0] !== 'string' || !CANVAS_PATH_METHODS.has(rest[0])) return null;
        const args = this.canvasArgs(rest.slice(1));
        return args ? ['pc', name, rest[0], ...args] : null;
      }
      default:
        return null;
    }
  }

  // ───────────────────────── queries & misc ─────────────────────────

  /** Validate a sync query. Returns a clean copy, or null to answer with an error. */
  validateQuery(q: unknown): Query | null {
    if (!isObj(q)) return null;
    const el = () => this.renderedElement(q.id);
    switch (q.q) {
      case 'rect': case 'rects': case 'box': case 'validity':
        return el() ? { q: q.q, id: q.id as NodeId } : null;
      case 'cs': {
        if (!el()) return null;
        const pseudo = typeof q.pseudo === 'string' && /^::?[a-z-]{1,32}$/.test(q.pseudo) ? q.pseudo : undefined;
        const prop = typeof q.prop === 'string' && /^--[\w-]{1,128}$/.test(q.prop) ? q.prop : undefined;
        return { q: 'cs', id: q.id as NodeId, ...(pseudo ? { pseudo } : {}), ...(prop ? { prop } : {}) };
      }
      case 'svg': {
        if (!el() || typeof q.method !== 'string' || !SVG_QUERY_METHODS.has(q.method)) return null;
        const args = Array.isArray(q.args) && q.args.length <= 4 && q.args.every(isNum) ? (q.args as number[]) : [];
        return { q: 'svg', id: q.id as NodeId, method: q.method, args };
      }
      case 'measureText':
        return isStr(q.font, 512) && isStr(q.text, 100_000) ? { q: 'measureText', font: q.font, text: q.text } : null;
      case 'imageData': {
        const n = el();
        if (!n || n.tag !== 'canvas') return null;
        const { x, y, w, h } = q;
        if (![x, y, w, h].every(isNum) || (w as number) <= 0 || (h as number) <= 0 || (w as number) * (h as number) > 3_500_000) return null;
        return { q: 'imageData', id: n.id, x: x as number, y: y as number, w: w as number, h: h as number };
      }
      case 'canvasCall': {
        const n = el();
        if (!n || n.tag !== 'canvas' || typeof q.method !== 'string' || !CANVAS_QUERY_METHODS.has(q.method)) return null;
        const args = Array.isArray(q.args) ? this.canvasArgs(q.args.slice(0, 8)) : [];
        return args ? { q: 'canvasCall', id: n.id, method: q.method, args } : null;
      }
      case 'elementFromPoint':
        return isNum(q.x) && isNum(q.y) ? { q: 'elementFromPoint', x: q.x, y: q.y } : null;
      case 'matchMedia':
        return isStr(q.query, 1024) ? { q: 'matchMedia', query: q.query } : null;
      case 'viewport':
        return { q: 'viewport' };
      case 'anims':
        return el() ? { q: 'anims', id: q.id as NodeId, subtree: q.subtree === true } : null;
      default:
        return null;
    }
  }

  private liveAnims = new Set<number>();

  validateAnim(list: unknown): AnimRecord[] {
    if (!Array.isArray(list) || list.length > MAX_ANIM_RECORDS) {
      this.diag('protocol-violation', 'anim batch is not an array or too large');
      return [];
    }
    const live = this.liveAnims;
    const out: AnimRecord[] = [];
    const drop = (aid: number) => { if (live.delete(aid)) out.push({ aid, remove: true }); };
    for (const a of list) {
      if (!isObj(a) || !Number.isInteger(a.aid) || (a.aid as number) < 1 || (a.aid as number) > 2147483647) {
        this.diag('protocol-violation', 'rejected anim record');
        continue;
      }
      const aid = a.aid as number;
      if (a.remove === true) { drop(aid); continue; }
      if (a.css !== undefined) {
        const c = a.css;
        const valid = this.renderedElement(a.target) && isObj(c) && (c.kind === 'animation' || c.kind === 'transition') && typeof c.name === 'string' && /^[\w-]{1,128}$/.test(c.name) && finiteWithin(c.origin, 1e15)
          && (a.startTime === null || finiteWithin(a.startTime, 1e15)) && (a.holdTime === null || finiteWithin(a.holdTime, 1e12)) && finiteWithin(a.playbackRate, 1000) && typeof a.cancelled === 'boolean';
        if (!valid) { this.diag('protocol-violation', 'rejected css anim record'); continue; }
        out.push({
          aid, target: a.target as NodeId, css: { kind: c.kind as 'animation' | 'transition', name: c.name as string, origin: c.origin as number },
          startTime: a.startTime as number | null, holdTime: a.holdTime as number | null, playbackRate: a.playbackRate as number, cancelled: a.cancelled as boolean,
        });
        continue;
      }
      const target = this.renderedElement(a.target);
      const t = a.timing;
      const ok = !!target && Array.isArray(a.keyframes) && a.keyframes.length <= MAX_ANIM_KEYFRAMES && isObj(t)
        && finiteWithin(t.duration, MAX_ANIM_TIME) && (t.duration as number) >= 0 && finiteWithin(t.delay, MAX_ANIM_TIME) && finiteWithin(t.endDelay, MAX_ANIM_TIME)
        && (t.iterations === null || (finiteWithin(t.iterations, 1e9) && (t.iterations as number) >= 0)) && finiteWithin(t.iterationStart, 1e9) && (t.iterationStart as number) >= 0
        && ANIM_DIRECTIONS.has(t.direction as string) && ANIM_FILLS.has(t.fill as string)
        && animEasing(t.easing) !== null && ANIM_COMPOSITES.has(a.composite as string) && ANIM_ITERATION_COMPOSITES.has(a.iterationComposite as string)
        && (a.startTime === null || finiteWithin(a.startTime, 1e15)) && (a.holdTime === null || finiteWithin(a.holdTime, 1e12)) && (a.startTime !== null || a.holdTime !== null)
        && finiteWithin(a.playbackRate, 1000);
      if (!ok) {
        if (target) this.diag('protocol-violation', 'rejected anim record');
        drop(aid);
        continue;
      }
      const timing = t as Record<string, unknown>;
      const frames: AnimKeyframe[] = [];
      let last = 0;
      let bad = false;
      for (const f of a.keyframes as unknown[]) {
        if (!isObj(f) || !finiteWithin(f.offset, 1) || (f.offset as number) < last || (f.offset as number) > 1 || animEasing(f.easing) === null
          || !(f.composite === null || ANIM_COMPOSITES.has(f.composite as string)) || !isObj(f.values)) { bad = true; break; }
        last = f.offset as number;
        const names = Object.keys(f.values);
        if (names.length > MAX_ANIM_VALUES) { bad = true; break; }
        const values: Record<string, string> = {};
        for (const name of names) {
          const value = f.values[name];
          if (typeof value !== 'string' || value.length > 4096 || !/^(?:--[\w-]{1,128}|-?[a-z][a-z-]{0,63})$/.test(name) || /^(?:-webkit-)?(?:animation|transition)(?:-|$)/.test(name)) continue;
          const clean = sanitizeStyleProperty(name, value, this.diag, this.env);
          if (clean && clean.value !== '') values[name] = clean.value;
        }
        frames.push({ offset: f.offset as number, easing: f.easing as string, composite: f.composite as AnimKeyframe['composite'], values });
      }
      if (bad) { this.diag('protocol-violation', 'rejected anim keyframes'); drop(aid); continue; }
      if (!live.has(aid) && live.size >= MAX_LIVE_ANIMS) { this.diag('animation-limit', 'more than ' + MAX_LIVE_ANIMS + ' live script animations'); continue; }
      live.add(aid);
      out.push({
        aid, target: a.target as NodeId, keyframes: frames,
        timing: {
          duration: timing.duration as number, delay: timing.delay as number, endDelay: timing.endDelay as number, iterations: timing.iterations as number | null,
          iterationStart: timing.iterationStart as number, direction: timing.direction as any, fill: timing.fill as any, easing: timing.easing as string,
        },
        composite: a.composite as any, iterationComposite: a.iterationComposite as any,
        startTime: a.startTime as number | null, holdTime: a.holdTime as number | null, playbackRate: a.playbackRate as number,
      });
    }
    return out;
  }

  /** Event types the sandbox wants forwarded. */
  validateListen(types: unknown): string[] {
    if (!Array.isArray(types)) return [];
    return types.filter((t): t is string => typeof t === 'string' && EVENT_TYPE_SET.has(t));
  }

  /** "Learned preventDefault" keys: `${nodeId}:${eventType}`. */
  validatePreventDefault(keys: unknown): string[] {
    if (!Array.isArray(keys)) return [];
    return keys.filter((k): k is string => typeof k === 'string' && /^\d{1,10}:[a-z]{1,20}(?::[\w ]{1,20})?$/.test(k)).slice(0, 256);
  }
}

const MAX_ANIM_RECORDS = 4096;
const MAX_LIVE_ANIMS = 1024;
const MAX_ANIM_KEYFRAMES = 64;
const MAX_ANIM_VALUES = 64;
const MAX_ANIM_TIME = 86_400_000;
const ANIM_DIRECTIONS = new Set(['normal', 'reverse', 'alternate', 'alternate-reverse']);
const ANIM_FILLS = new Set(['none', 'forwards', 'backwards', 'both']);
const ANIM_COMPOSITES = new Set(['replace', 'add', 'accumulate']);
const ANIM_ITERATION_COMPOSITES = new Set(['replace', 'accumulate']);
const ANIM_NUMBER = '[+-]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][+-]?\\d+)?';
const ANIM_EASING = new RegExp('^(?:linear|ease|ease-in|ease-out|ease-in-out|step-start|step-end|cubic-bezier\\(\\s*' + ANIM_NUMBER + '\\s*,\\s*' + ANIM_NUMBER + '\\s*,\\s*' + ANIM_NUMBER + '\\s*,\\s*' + ANIM_NUMBER
  + '\\s*\\)|steps\\(\\s*\\d{1,6}\\s*(?:,\\s*(?:start|end|jump-start|jump-end|jump-both|jump-none)\\s*)?\\)|linear\\([0-9eE.%,\\s+-]{1,8192}\\))$');

function animEasing(e: unknown): string | null {
  return typeof e === 'string' && e.length <= 8192 && ANIM_EASING.test(e) && e.split(',').length <= 257 ? e : null;
}

function finiteWithin(n: unknown, max: number): boolean {
  return typeof n === 'number' && Number.isFinite(n) && Math.abs(n) <= max;
}

function safeJson(v: unknown): string {
  try {
    const s = JSON.stringify(v);
    return s.length > 160 ? s.slice(0, 160) + '…' : s;
  } catch {
    return '[unserializable]';
  }
}
