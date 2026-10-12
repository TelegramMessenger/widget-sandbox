// CSS sanitizer (security-critical, invariants 3 and 4).
//
// All CSS that reaches the renderer — <style> text, style="" attributes,
// individual style property values and SVG presentation attributes — is parsed
// with css-tree and RE-GENERATED from the AST. Nothing is passed through
// verbatim: any region the parser could not understand (a `Raw` node) is
// dropped, so the renderer never sees text the sanitizer did not understand.
//
// Rules:
//   - at-rules: allowlist only. @import / @namespace / @font-face / unknown
//     at-rules are dropped. No @font-face means widgets render with system
//     fonts only: `font-family` can only name fonts installed on the device.
//   - url(): allowed only for data: images and "#fragment". A declaration with
//     any other URL is dropped entirely.
//   - image-set(), image(), src(), cross-fade(): bare strings are URLs there, so
//     they get the same check; var()/env()/attr() inside them is rejected
//     because the substituted value cannot be checked statically.

import * as csstree from 'css-tree';
import { checkUrl, type UrlKind } from './url-policy';

export type Diag = (kind: string, detail: string) => void;

/**
 * The host decides the colour scheme (the chat theme), not the renderer's OS
 * setting: `prefers-color-scheme` media queries, `color-scheme: light dark`
 * and `light-dark()` are evaluated here against it.
 */
export interface CssEnv {
  colorScheme: 'light' | 'dark';
  /**
   * 'shadow': the renderer is a shadow root in the host page (ShadowDomRenderer
   * prototype), not a document of its own. Selectors that would reach the host
   * (`:host`, `:host-context`, `::slotted`, `::part`, `:scope`) are dropped,
   * `:root` becomes the renderer's `[data-tw-root]`, width/height media queries
   * become `@container tw` queries on the bubble and viewport units become
   * container units. Default: 'document' (iframe, native WebViews), untouched.
   */
  scope?: 'document' | 'shadow';
}
export const DEFAULT_ENV: CssEnv = { colorScheme: 'light' };
const ALWAYS = '(min-width: 0px)';
const NEVER = '(min-width: 99999px)';

const ALLOWED_ATRULES = new Set([
  'media', 'supports', 'keyframes', '-webkit-keyframes', 'container', 'layer', 'property',
  'counter-style', 'starting-style', 'scope', 'page', 'font-feature-values', 'font-palette-values', 'position-try',
]);

/** Functions whose string arguments are interpreted as URLs. */
const URL_FUNCTIONS = new Set(['image-set', '-webkit-image-set', 'image', 'src', 'cross-fade', '-webkit-cross-fade']);
const SUBSTITUTION_FUNCTIONS = new Set(['var', 'env', 'attr']);
const FORBIDDEN_PROPERTIES = new Set(['behavior', '-moz-binding']);

/** Secondary check for at-rule preludes css-tree left unparsed (new syntax). */
const SAFE_RAW_PRELUDE = /^[\w\s\-.,:()<>=%/*+!#]*$/;

const PARSE_OPTS = { parseCustomProperty: true, positions: false } as const;

function stripUnicodeControls(s: string): string {
  return s.replace(/\u0000/g, '�');
}

/**
 * Check one declaration value in place. Returns false if the declaration must be dropped.
 * `urlKind` is always 'image' today (the only URL-taking context left in CSS).
 */
function valueIsSafe(value: csstree.CssNode, urlKind: UrlKind, diag: Diag, where: string, env: CssEnv): boolean {
  if (value.type === 'Raw') {
    if (value.value.trim() === '') return true;
    diag('css-dropped', `${where}: unparsable value`);
    return false;
  }
  let ok = true;
  const fnStack: string[] = [];
  csstree.walk(value, {
    enter(node: csstree.CssNode, item: csstree.ListItem<csstree.CssNode>, list: csstree.List<csstree.CssNode>) {
      if (!ok) return;
      if (node.type === 'Function' && node.name.toLowerCase() === 'light-dark' && list) {
        // light-dark(a, b): pick the branch for the host theme.
        const parts: csstree.CssNode[][] = [[]];
        node.children.forEach((c) => (c.type === 'Operator' && c.value === ',' ? parts.push([]) : parts[parts.length - 1].push(c)));
        const chosen = parts[env.colorScheme === 'dark' ? 1 : 0] ?? parts[0];
        const repl = new csstree.List<csstree.CssNode>();
        for (const c of chosen) repl.appendData(c);
        list.replace(item, repl);
        return csstree.walk.skip;
      }
      switch (node.type) {
        case 'Raw':
          ok = false;
          diag('css-dropped', `${where}: unparsable value`);
          break;
        case 'Dimension':
          if (env.scope === 'shadow') {
            const unit = CONTAINER_UNITS[node.unit.toLowerCase()];
            if (unit) node.unit = unit;
          }
          break;
        case 'Url': {
          const checked = checkUrl(node.value, urlKind);
          if (checked === null) {
            ok = false;
            diag('url-blocked', `${where}: url(${truncate(node.value)})`);
          } else node.value = checked;
          break;
        }
        case 'Function': {
          const name = node.name.toLowerCase();
          if (name === 'url') {
            // css-tree yields a Function for url() with modifiers; never allow.
            ok = false;
            diag('url-blocked', `${where}: url() with modifiers`);
          } else if (fnStack.some((f) => URL_FUNCTIONS.has(f)) && SUBSTITUTION_FUNCTIONS.has(name)) {
            ok = false;
            diag('css-dropped', `${where}: ${name}() inside a URL-taking function`);
          }
          fnStack.push(name);
          break;
        }
        case 'String':
          if (fnStack.some((f) => URL_FUNCTIONS.has(f))) {
            const checked = checkUrl(node.value, 'image');
            if (checked === null) {
              ok = false;
              diag('url-blocked', `${where}: "${truncate(node.value)}" in ${fnStack[fnStack.length - 1]}()`);
            } else node.value = checked;
          }
          break;
      }
    },
    leave(node: csstree.CssNode) {
      if (node.type === 'Function') fnStack.pop();
    },
  });
  return ok;
}

function truncate(s: string): string {
  return s.length > 80 ? s.slice(0, 80) + '…' : s;
}

function declarationIsSafe(decl: csstree.Declaration, diag: Diag, env: CssEnv): boolean {
  const prop = decl.property.toLowerCase();
  if (FORBIDDEN_PROPERTIES.has(prop)) return false;
  if (prop === 'color-scheme') {
    // `light dark` means "follow the preference": resolve it to the host theme.
    const t = csstree.generate(decl.value).toLowerCase();
    if (/\blight\b/.test(t) && /\bdark\b/.test(t)) decl.value = csstree.parse(env.colorScheme, { context: 'value' }) as csstree.Value;
  }
  return valueIsSafe(decl.value, 'image', diag, prop, env);
}

/** Shadow scope: viewport units resolve against the bubble (`container-type: inline-size` on the host; the block axis falls back to the small viewport). */
const CONTAINER_UNITS: Record<string, string> = {
  vw: 'cqw', svw: 'cqw', lvw: 'cqw', dvw: 'cqw', vh: 'cqh', svh: 'cqh', lvh: 'cqh', dvh: 'cqh',
  vmin: 'cqmin', svmin: 'cqmin', lvmin: 'cqmin', dvmin: 'cqmin', vmax: 'cqmax', svmax: 'cqmax', lvmax: 'cqmax', dvmax: 'cqmax',
  vi: 'cqi', svi: 'cqi', lvi: 'cqi', dvi: 'cqi', vb: 'cqb', svb: 'cqb', lvb: 'cqb', dvb: 'cqb',
};

const SIZE_FEATURE = /^\(\s*(?:(?:min-|max-)?(?:width|height|aspect-ratio)\s*:\s*[^()]+|orientation\s*:\s*(?:portrait|landscape)\s*|[^()]*\b(?:width|height)\b[^()]*[<>][^()]*)\)$/i;

/**
 * Shadow scope: a media query list that only asks about the viewport's size
 * (what the iframe's viewport was: the bubble) as a container query on the
 * bubble, e.g. `screen and (max-width: 480px), (orientation: portrait)` →
 * `tw (max-width: 480px) or (orientation: portrait)`. Null when it asks
 * anything else (hover, pointer, print, resolution…): left as a media query.
 */
export function mediaToContainer(text: string): string | null {
  const out: string[] = [];
  for (let q of text.split(',')) {
    q = q.trim().replace(/^only\s+/i, '');
    if (!q) return null;
    const type = /^(screen|all)\b\s*(?:and\b)?\s*/i.exec(q);
    if (type) q = q.slice(type[0].length).trim();
    if (!q) { out.push('(min-width: 0px)'); continue; }
    const features = q.split(/\s+and\s+/i);
    for (const f of features) if (!SIZE_FEATURE.test(f.trim())) return null;
    out.push(features.map((f) => f.trim()).join(' and '));
  }
  return 'tw ' + out.join(' or ');
}

const HOST_PSEUDO_CLASSES = new Set(['host', 'host-context', 'scope']);
const HOST_PSEUDO_ELEMENTS = new Set(['slotted', 'part']);

/** Shadow scope: drop selectors that could match outside the widget's tree; `:root` → the renderer's `[data-tw-root]`. Returns false when nothing is left. */
function scopeSelectorList(prelude: csstree.SelectorList): boolean {
  const keep = new csstree.List<csstree.CssNode>();
  prelude.children.forEach((sel) => {
    const hostish = csstree.find(sel, (n) =>
      (n.type === 'PseudoClassSelector' && HOST_PSEUDO_CLASSES.has(n.name.toLowerCase())) ||
      (n.type === 'PseudoElementSelector' && HOST_PSEUDO_ELEMENTS.has(n.name.toLowerCase())));
    if (hostish) return;
    csstree.walk(sel, {
      enter(node: csstree.CssNode, item: csstree.ListItem<csstree.CssNode>, list: csstree.List<csstree.CssNode>) {
        if (node.type === 'PseudoClassSelector' && node.name.toLowerCase() === 'root' && !node.children && list) {
          const attr: csstree.AttributeSelector = { type: 'AttributeSelector', name: { type: 'Identifier', name: 'data-tw-root' }, matcher: null, value: null, flags: null };
          list.replace(item, csstree.List.createItem(attr));
        }
      },
    });
    keep.appendData(sel);
  });
  prelude.children = keep;
  return !keep.isEmpty;
}

/** Evaluate prefers-color-scheme in a media query list against the host theme. */
export function rewriteMediaQuery(text: string, env: CssEnv): string {
  return text.replace(/\(\s*prefers-color-scheme\s*:\s*(dark|light)\s*\)/gi, (_, v: string) => (v.toLowerCase() === env.colorScheme ? ALWAYS : NEVER));
}

function hasRaw(node: csstree.CssNode | null): boolean {
  return !!node && csstree.find(node, (n) => n.type === 'Raw') !== null;
}

function rawIsSafe(node: csstree.CssNode): boolean {
  let ok = true;
  csstree.walk(node, (n) => {
    if (n.type === 'Raw' && !SAFE_RAW_PRELUDE.test(n.value)) ok = false;
  });
  return ok;
}

function sanitizeTree(ast: csstree.CssNode, diag: Diag, env: CssEnv): void {
  const atruleStack: string[] = [];
  csstree.walk(ast, {
    enter(node: csstree.CssNode, item: csstree.ListItem<csstree.CssNode>, list: csstree.List<csstree.CssNode>) {
      if (node.type === 'Atrule') {
        const name = node.name.toLowerCase();
        if (!ALLOWED_ATRULES.has(name)) {
          diag('css-dropped', `@${name}`);
          list.remove(item);
          return csstree.walk.skip;
        }
        if (name === 'media' && node.prelude) {
          let text = csstree.generate(node.prelude);
          if (/prefers-color-scheme/i.test(text)) {
            text = rewriteMediaQuery(text, env);
            node.prelude = { type: 'Raw', value: text } as csstree.Raw;
          }
          if (env.scope === 'shadow') {
            const container = mediaToContainer(text);
            if (container) {
              node.name = 'container';
              node.prelude = { type: 'Raw', value: container } as csstree.Raw;
            }
          }
        }
        if (node.prelude && hasRaw(node.prelude) && !rawIsSafe(node.prelude)) {
          diag('css-dropped', `@${name}: unparsable prelude`);
          list.remove(item);
          return csstree.walk.skip;
        }
        atruleStack.push(name);
      } else if (node.type === 'Rule') {
        if (hasRaw(node.prelude)) {
          diag('css-dropped', 'rule with unparsable selector');
          list.remove(item);
          return csstree.walk.skip;
        }
        if (env.scope === 'shadow' && node.prelude.type === 'SelectorList' && !scopeSelectorList(node.prelude)) {
          diag('css-dropped', 'rule: shadow-host selector');
          list.remove(item);
          return csstree.walk.skip;
        }
      } else if (node.type === 'Declaration') {
        if (!declarationIsSafe(node, diag, env)) list.remove(item);
        return csstree.walk.skip;
      } else if (node.type === 'Raw') {
        // Raw at block level = something that is neither a rule nor a declaration.
        if (list) list.remove(item);
        return csstree.walk.skip;
      }
    },
    leave(node: csstree.CssNode) {
      if (node.type === 'Atrule') atruleStack.pop();
    },
  });
}

function parse(css: string, context: string): csstree.CssNode {
  return csstree.parse(stripUnicodeControls(css), { ...PARSE_OPTS, context } as csstree.ParseOptions);
}

/** Sanitize the text of a <style> element. */
export function sanitizeStylesheet(css: string, diag: Diag, env: CssEnv = DEFAULT_ENV): string {
  try {
    const ast = parse(css, 'stylesheet');
    sanitizeTree(ast, diag, env);
    return csstree.generate(ast);
  } catch (e) {
    diag('css-dropped', 'stylesheet: ' + (e as Error).message);
    return '';
  }
}

/** Sanitize the value of a style="" attribute. */
export function sanitizeStyleAttribute(css: string, diag: Diag, env: CssEnv = DEFAULT_ENV): string {
  try {
    const ast = parse(css, 'declarationList');
    sanitizeTree(ast, diag, env);
    return csstree.generate(ast);
  } catch (e) {
    diag('css-dropped', 'style attribute: ' + (e as Error).message);
    return '';
  }
}

const PROPERTY_NAME = /^-{0,2}[a-zA-Z_][\w-]*$/;

/** Sanitize a single property set via CSSOM. Returns null if it must be dropped. */
export function sanitizeStyleProperty(name: string, value: string, diag: Diag, env: CssEnv = DEFAULT_ENV): { name: string; value: string } | null {
  if (!PROPERTY_NAME.test(name)) return null;
  if (value === '') return { name, value: '' };
  try {
    const ast = parse(`${name}:${value}`, 'declaration');
    if (ast.type !== 'Declaration' || !declarationIsSafe(ast, diag, env)) return null;
    return { name, value: csstree.generate(ast.value) };
  } catch {
    return null;
  }
}

/**
 * Sanitize a free-standing CSS value, e.g. an SVG presentation attribute
 * (fill="url(#g)") or a canvas `filter`. Returns null if it must be dropped.
 */
export function sanitizeCssValue(value: string, diag: Diag, where = 'value', env: CssEnv = DEFAULT_ENV): string | null {
  try {
    const ast = parse(value, 'value');
    if (!valueIsSafe(ast, 'image', diag, where, env)) return null;
    return csstree.generate(ast);
  } catch {
    return null;
  }
}
