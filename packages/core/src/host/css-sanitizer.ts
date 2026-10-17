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
//   - the widget profile (packages/core/profile.json, docs/profile-v1.md): only
//     its properties, functions, units, pseudo-classes, at-rules and media
//     features pass; `@supports` is decided here from the profile; nested rules
//     are dropped; font families are reduced to the generic ones; sticky and
//     scrolling overflow are rewritten. A removal the profile causes is reported
//     as the `profile` diagnostic, one a security rule causes as `css-dropped`
//     or `url-blocked`.

import * as csstree from 'css-tree';
import { checkUrl, type UrlKind } from './url-policy';
import { PROFILE_TABLES } from '../shared/profile-tables';
import {
  cssFunctionInProfile, cssPropertyInProfile, cssPseudoClassInProfile, cssPseudoElementInProfile, cssUnitInProfile,
  fontShorthandFamilyStart, isGenericFont, supportsCondition,
} from '../shared/css-supports';

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
  /**
   * Strict policy: `attr()` may only read attributes the validator's text budgets
   * count (`isTextAttr`), so no other attribute can become visible text.
   */
  strict?: boolean;
  /** The client's answer to `prefers-reduced-motion` (the OS setting); default: no preference. */
  reducedMotion?: boolean;
}
export const DEFAULT_ENV: CssEnv = { colorScheme: 'light' };

const VISIBLE_TEXT_ATTRS = new Set(['value', 'placeholder', 'alt', 'label', 'title']);

/**
 * Attributes whose values count against the strict text budgets: the ones a
 * renderer draws as text, plus `data-*` and `aria-*`, which `attr()` may read.
 */
export function isTextAttr(name: string): boolean {
  return VISIBLE_TEXT_ATTRS.has(name) || name.startsWith('data-') || name.startsWith('aria-');
}
export const TEXT_ATTRS = [...VISIBLE_TEXT_ATTRS];
export const TEXT_ATTR_PREFIXES = ['data-', 'aria-'];

/**
 * The contents of the string tokens in sanitized CSS: what `content`, `quotes`,
 * `list-style` or a custom property substituted into them can draw. The input is
 * this sanitizer's output, so escapes are already decoded except `\"` and `\\`.
 * NativeValidator.cssStringText is the same loop over UTF-16 units.
 */
export function cssStringText(css: string): string {
  let out = '';
  for (let i = 0; i < css.length;) {
    const c = css.charCodeAt(i);
    if (c === 0x5c) { i += 2; continue; }
    if (c !== 0x22 && c !== 0x27) { i++; continue; }
    let j = i + 1;
    while (j < css.length && css.charCodeAt(j) !== c) {
      if (css.charCodeAt(j) === 0x5c && j + 1 < css.length) j++;
      out += css[j];
      j++;
    }
    i = j + 1;
  }
  return out;
}
const ALWAYS = '(min-width: 0px)';
const NEVER = '(min-width: 99999px)';

const PROFILE_CSS = PROFILE_TABLES.css;
const ALLOWED_ATRULES = new Set<string>(PROFILE_CSS.atRules);
/** At-rules dropped for security (a URL, a font parser), whatever the profile says. */
const SECURITY_ATRULES = new Set(['import', 'namespace', 'font-face']);
const MEDIA_TYPES = new Set<string>(PROFILE_CSS.mediaTypes);
const MEDIA_FEATURES = new Set<string>(PROFILE_CSS.mediaFeatures);
const VALUE_SETS = PROFILE_CSS.values as Record<string, readonly string[]>;
const REWRITES = PROFILE_CSS.rewrite as Record<string, Record<string, string>>;
const FILTER_FUNCTIONS = new Set<string>(PROFILE_CSS.filterFunctions);
const TRANSFORM_3D = new Set<string>(PROFILE_CSS.transform3d);
const FONT_FALLBACK = PROFILE_CSS.fonts.fallback as Record<string, string>;
const PSEUDO_REWRITE = PROFILE_CSS.pseudoRewrite as Record<string, string>;
const GLOBAL_KEYWORDS = new Set(['inherit', 'initial', 'unset', 'revert', 'revert-layer']);
const FONT_SIZE_KEYWORDS = new Set(['xx-small', 'x-small', 'small', 'medium', 'large', 'x-large', 'xx-large', 'xxx-large', 'smaller', 'larger']);
const SUBSTITUTED = (n: csstree.CssNode) => n.type === 'Function' && SUBSTITUTION_NAMES.has(n.name.toLowerCase());
const SUBSTITUTION_NAMES = new Set(['var', 'env', 'attr']);

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
          if (!cssUnitInProfile(node.unit)) {
            ok = false;
            diag('profile', `${where}: unit ${truncate(node.unit)}`);
            break;
          }
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
          } else if (!cssFunctionInProfile(name)) {
            ok = false;
            diag('profile', `${where}: ${truncate(name)}()`);
          } else if (fnStack.some((f) => URL_FUNCTIONS.has(f)) && SUBSTITUTION_FUNCTIONS.has(name)) {
            ok = false;
            diag('css-dropped', `${where}: ${name}() inside a URL-taking function`);
          } else if (name === 'attr' && env.strict) {
            const first = node.children.first;
            const attr = first?.type === 'Identifier' ? first.name.toLowerCase() : '';
            if (!isTextAttr(attr)) {
              ok = false;
              diag('css-dropped', `${where}: attr(${truncate(attr)}) reads an attribute outside the text budget`);
            }
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
  if (s.length <= 80) return s;
  const last = s.charCodeAt(79);
  return s.slice(0, last >= 0xd800 && last <= 0xdbff ? 79 : 80) + '…';
}

function declarationIsSafe(decl: csstree.Declaration, diag: Diag, env: CssEnv): boolean {
  const prop = decl.property.toLowerCase();
  if (FORBIDDEN_PROPERTIES.has(prop)) return false;
  if (!cssPropertyInProfile(prop)) {
    diag('profile', `property ${truncate(prop)}`);
    return false;
  }
  if (prop === 'color-scheme') {
    // `light dark` means "follow the preference": resolve it to the host theme.
    const t = csstree.generate(decl.value).toLowerCase();
    if (/\blight\b/.test(t) && /\bdark\b/.test(t)) decl.value = csstree.parse(env.colorScheme, { context: 'value' }) as csstree.Value;
  }
  // Filter lists lose their out-of-profile functions before the function check would drop the whole list.
  if (FILTER_PROPERTIES.has(prop) && !pruneFilterList(prop, decl, diag)) return false;
  return valueIsSafe(decl.value, 'image', diag, prop, env) && valueInProfile(prop, decl, diag);
}

const FILTER_PROPERTIES = new Set(['filter', 'backdrop-filter', '-webkit-backdrop-filter']);

/** `filter: blur(2px) grayscale(1)` → `filter: blur(2px)`; false when nothing is left. */
function pruneFilterList(prop: string, decl: csstree.Declaration, diag: Diag): boolean {
  if (decl.value.type !== 'Value') return true;
  const items = decl.value.children.toArray();
  const kept = items.filter((n) => {
    if (n.type === 'Url' || (n.type === 'Function' && !FILTER_FUNCTIONS.has(n.name.toLowerCase()) && !SUBSTITUTION_NAMES.has(n.name.toLowerCase()))) {
      diag('profile', `${prop}: ${n.type === 'Url' ? 'url()' : (n as csstree.FunctionNode).name.toLowerCase() + '()'}`);
      return false;
    }
    return true;
  });
  if (kept.length === 0) return false;
  if (kept.length !== items.length) decl.value.children = listOf(kept);
  return true;
}

const ident = (n: csstree.CssNode | undefined) => (n?.type === 'Identifier' ? n.name.toLowerCase() : null);
const listOf = (nodes: csstree.CssNode[]) => new csstree.List<csstree.CssNode>().fromArray(nodes);
const comma = (): csstree.CssNode => ({ type: 'Operator', value: ',' });

/**
 * The profile's rules for one declaration's value (after the security checks):
 * false drops the declaration; rewrites happen in place. A value with var(),
 * env() or attr() is judged only by what is visible here (the substituted text
 * is checked where it is declared, as a custom property).
 */
function valueInProfile(prop: string, decl: csstree.Declaration, diag: Diag): boolean {
  if (prop.startsWith('--') || decl.value.type !== 'Value') return true;
  const items = decl.value.children.toArray();
  const substituted = items.some((n) => csstree.find(n, SUBSTITUTED) !== null);
  const only = items.length === 1 ? ident(items[0]) : null;
  if (only && GLOBAL_KEYWORDS.has(only)) return true;
  const drop = (why: string) => { diag('profile', `${prop}: ${truncate(why)}`); return false; };

  const allowed = VALUE_SETS[prop];
  const rewrite = REWRITES[prop === 'overflow-x' || prop === 'overflow-y' ? 'overflow' : prop];
  if (rewrite) {
    for (const n of items) {
      const v = ident(n);
      if (v && Object.prototype.hasOwnProperty.call(rewrite, v)) {
        diag('profile', `${prop}: ${v} → ${rewrite[v]}`);
        (n as csstree.Identifier).name = rewrite[v];
      }
    }
  }
  if (allowed && !substituted) {
    const v = items.length === 1 ? ident(items[0]) : null;
    if (!v || !allowed.includes(v)) return drop(csstree.generate(decl.value));
  }
  switch (prop) {
    case 'background-clip':
      if (items.some((n) => ident(n) === 'text')) return drop('text');
      break;
    case 'transform':
      if (csstree.find(decl.value, (n) => n.type === 'Function' && TRANSFORM_3D.has(n.name.toLowerCase()))) return drop('3D function');
      break;
    case 'rotate':
      if (items.some((n) => ['x', 'y', 'z'].includes(ident(n) ?? '')) || items.filter((n) => n.type === 'Number').length > 1) return drop('3D rotation');
      break;
    case 'translate': case 'scale':
      if (items.filter((n) => n.type !== 'Operator').length > 2) return drop('3D');
      break;
    case 'font-family':
      decl.value.children = listOf(genericFamilies(items, diag));
      break;
    case 'font': {
      const start = fontShorthandFamilyStart(items, fontTokenKind);
      if (start > 0 && start < items.length) decl.value.children = listOf([...items.slice(0, start), ...genericFamilies(items.slice(start), diag)]);
      break;
    }
  }
  return true;
}

function fontTokenKind(n: csstree.CssNode) {
  const size = n.type === 'Dimension' || n.type === 'Percentage' || (n.type === 'Identifier' && FONT_SIZE_KEYWORDS.has(n.name.toLowerCase()))
    || (n.type === 'Function' && ['calc', 'min', 'max', 'clamp'].includes(n.name.toLowerCase()));
  return { size, slash: n.type === 'Operator' && n.value === '/' };
}

/**
 * A font-family list reduced to the profile's generic families (D3b): named
 * families are removed, `ui-monospace` & co. get their basic generic after them
 * (only WebKit resolves them), an empty list becomes `system-ui`.
 */
function genericFamilies(items: csstree.CssNode[], diag: Diag): csstree.CssNode[] {
  const groups: csstree.CssNode[][] = [[]];
  for (const n of items) {
    if (n.type === 'Operator' && n.value === ',') groups.push([]);
    else groups[groups.length - 1].push(n);
  }
  const kept: csstree.CssNode[][] = [];
  const names = new Set<string>();
  for (const g of groups) {
    if (g.length === 0) continue;
    if (g.some((n) => n.type === 'Function')) { kept.push(g); continue; }
    const name = g.length === 1 ? ident(g[0]) : null;
    if (name && (isGenericFont(name) || GLOBAL_KEYWORDS.has(name))) {
      kept.push(g);
      names.add(name);
      continue;
    }
    diag('profile', `font-family: ${truncate(g.map((n) => (n.type === 'String' ? JSON.stringify(n.value) : csstree.generate(n))).join(' '))}`);
  }
  const out: csstree.CssNode[][] = [];
  kept.forEach((g, i) => {
    out.push(g);
    const name = g.length === 1 ? ident(g[0]) : null;
    const fallback = name ? FONT_FALLBACK[name] : undefined;
    // Its fallback goes right after it unless the list already names it later on.
    if (fallback && !kept.slice(i + 1).some((h) => h.length === 1 && ident(h[0]) === fallback) && !out.some((h) => h.length === 1 && ident(h[0]) === fallback)) {
      out.push([{ type: 'Identifier', name: fallback }]);
    }
  });
  if (out.length === 0) out.push([{ type: 'Identifier', name: PROFILE_CSS.fonts.default }]);
  return out.flatMap((g, i) => (i ? [comma(), ...g] : g));
}

/** A font-family list or `font` shorthand outside a stylesheet (SVG `font-family`, canvas `font`, measureText): the same reduction. */
export function sanitizeFontValue(prop: 'font' | 'font-family', value: string, diag: Diag): string | null {
  try {
    const ast = parse(`${prop}:${value}`, 'declaration');
    if (ast.type !== 'Declaration' || !declarationIsSafe(ast, diag, DEFAULT_ENV)) return null;
    return csstree.generate(ast.value);
  } catch {
    return null;
  }
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

/**
 * The profile's selectors: a selector with a pseudo-class or pseudo-element
 * outside the profile (`:focus`, `:has()`, `::marker`, `:host`, `:scope`, …) or
 * a nesting selector never matches, so it is dropped from its list; `:disabled`
 * and `:enabled` become `[disabled]` and `:not([disabled])` (the `<button>`
 * state, the only form state left). Returns false when nothing is left.
 */
function profileSelectorList(prelude: csstree.SelectorList, diag: Diag): boolean {
  const keep = new csstree.List<csstree.CssNode>();
  prelude.children.forEach((sel) => {
    let bad: string | null = null;
    csstree.walk(sel, {
      enter(node: csstree.CssNode, item: csstree.ListItem<csstree.CssNode>, list: csstree.List<csstree.CssNode>) {
        if (bad) return;
        if (node.type === 'NestingSelector') bad = '&';
        else if (node.type === 'PseudoElementSelector' && !cssPseudoElementInProfile(node.name)) bad = '::' + node.name;
        else if (node.type === 'PseudoClassSelector') {
          const name = node.name.toLowerCase();
          const replacement = Object.prototype.hasOwnProperty.call(PSEUDO_REWRITE, name) ? PSEUDO_REWRITE[name] : null;
          if (replacement && list) {
            const parsed = csstree.parse(replacement, { context: 'selector' }) as csstree.Selector;
            list.replace(item, parsed.children);
          } else if (!cssPseudoClassInProfile(name)) bad = ':' + name;
        }
      },
    });
    if (bad) diag('profile', `selector ${truncate(bad)}`);
    else keep.appendData(sel);
  });
  prelude.children = keep;
  return !keep.isEmpty;
}

/** Shadow scope: `:root` → the renderer's `[data-tw-root]` (the host-reaching selectors are outside the profile on every renderer). */
function scopeSelectorList(prelude: csstree.SelectorList): boolean {
  const keep = new csstree.List<csstree.CssNode>();
  prelude.children.forEach((sel) => {
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

/**
 * Does a media query list ask only what the profile answers: the media types
 * `all` and `screen`, the bubble's width and orientation, and the two
 * preferences the client answers? (No height: the bubble's height comes from
 * the content; no hover, pointer, print, resolution…)
 */
export function mediaInProfile(text: string): boolean {
  let s = text.toLowerCase();
  const features: string[] = [];
  // Innermost parentheses first: `(hover)`, `(min-width: 1px)`, `(400px <= width < 700px)`, then `(not (…))`.
  for (let guard = 0; /\([^()]*\)/.test(s) && guard < 64; guard++) {
    s = s.replace(/\(([^()]*)\)/g, (_, inner: string) => {
      const t = inner.trim();
      if (t === '' || t === '\u0001' || /^(?:not\s+)?\u0001(?:\s+(?:and|or)\s+\u0001)*$/.test(t)) return ' \u0001 ';
      const colon = t.indexOf(':');
      if (colon >= 0) features.push(t.slice(0, colon).trim());
      else if (/[<>=]/.test(t)) features.push(...(t.match(/[a-z][a-z-]*/g) ?? []));
      else features.push(t);
      return ' \u0001 ';
    });
  }
  if (/[()]/.test(s)) return false;
  for (const f of features) if (!MEDIA_FEATURES.has(f)) return false;
  for (const word of s.match(/[a-z][a-z-]*/g) ?? []) if (!['and', 'or', 'not', 'only'].includes(word) && !MEDIA_TYPES.has(word)) return false;
  return true;
}

/**
 * Evaluate the preferences in a media query list against the client's answers
 * (the chat theme, the OS reduced-motion setting); a query outside the profile
 * never matches.
 */
export function rewriteMediaQuery(text: string, env: CssEnv): string {
  if (!mediaInProfile(text)) return NEVER;
  return text
    .replace(/\(\s*prefers-color-scheme\s*:\s*(dark|light)\s*\)/gi, (_, v: string) => (v.toLowerCase() === env.colorScheme ? ALWAYS : NEVER))
    .replace(/\(\s*prefers-reduced-motion\s*(?::\s*(reduce|no-preference)\s*)?\)/gi, (_, v: string | undefined) =>
      ((v ?? 'reduce').toLowerCase() === 'reduce') === !!env.reducedMotion ? ALWAYS : NEVER);
}

/**
 * `@supports` decided from the profile (D23): a block whose condition holds is
 * unwrapped, the others are dropped, before anything reaches the renderer.
 */
function resolveSupports(list: csstree.List<csstree.CssNode>): csstree.List<csstree.CssNode> {
  const out: csstree.CssNode[] = [];
  list.forEach((node) => {
    if (node.type === 'Atrule' && node.name.toLowerCase() === 'supports') {
      if (node.block && node.prelude && supportsCondition(csstree.generate(node.prelude))) out.push(...resolveSupports(node.block.children).toArray());
      return;
    }
    if ((node.type === 'Atrule' || node.type === 'Rule') && node.block) node.block.children = resolveSupports(node.block.children);
    out.push(node);
  });
  return new csstree.List<csstree.CssNode>().fromArray(out);
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
  /** Rules entered (outside @keyframes): a rule or at-rule inside one is nested CSS, outside the profile. */
  let ruleDepth = 0;
  if (ast.type === 'StyleSheet') ast.children = resolveSupports(ast.children);
  csstree.walk(ast, {
    enter(node: csstree.CssNode, item: csstree.ListItem<csstree.CssNode>, list: csstree.List<csstree.CssNode>) {
      if ((node.type === 'Atrule' || node.type === 'Rule') && ruleDepth > 0 && list) {
        diag('profile', 'nested rule');
        list.remove(item);
        return csstree.walk.skip;
      }
      if (node.type === 'Atrule') {
        const name = node.name.toLowerCase();
        if (!ALLOWED_ATRULES.has(name)) {
          diag(SECURITY_ATRULES.has(name) ? 'css-dropped' : 'profile', `@${name}`);
          list.remove(item);
          return csstree.walk.skip;
        }
        if (name === 'media' && node.prelude) {
          let text = csstree.generate(node.prelude);
          if (!mediaInProfile(text)) {
            diag('profile', `@media ${truncate(text)}`);
            list.remove(item);
            return csstree.walk.skip;
          }
          if (/prefers-(?:color-scheme|reduced-motion)/i.test(text)) {
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
        const inKeyframes = /keyframes$/.test(atruleStack[atruleStack.length - 1] ?? '');
        if (!inKeyframes && node.prelude.type === 'SelectorList' && !profileSelectorList(node.prelude, diag)) {
          list.remove(item);
          return csstree.walk.skip;
        }
        if (env.scope === 'shadow' && node.prelude.type === 'SelectorList' && !scopeSelectorList(node.prelude)) {
          diag('css-dropped', 'rule: shadow-host selector');
          list.remove(item);
          return csstree.walk.skip;
        }
        if (!inKeyframes) ruleDepth++;
        atruleStack.push(inKeyframes ? 'keyframe' : 'rule');
      } else if (node.type === 'Declaration') {
        if (!declarationIsSafe(node, diag, env)) list.remove(item);
        return csstree.walk.skip;
      } else if (node.type === 'Raw') {
        // Raw at block level = something that is neither a rule nor a declaration
        // (css-tree leaves `.a { .b { … } }` there: nested CSS, outside the profile).
        if (ruleDepth > 0 && node.value.includes('{')) diag('profile', 'nested rule');
        if (list) list.remove(item);
        return csstree.walk.skip;
      }
    },
    leave(node: csstree.CssNode) {
      if (node.type === 'Atrule') atruleStack.pop();
      else if (node.type === 'Rule' && atruleStack.pop() === 'rule') ruleDepth--;
    },
  });
}

function parse(css: string, context: string): csstree.CssNode {
  return csstree.parse(stripUnicodeControls(css), { ...PARSE_OPTS, context } as csstree.ParseOptions);
}

/** Sanitize the text of a <style> element. */
// Sanitized stylesheets by colour scheme, scope and text, with the diagnostics they
// produced (replayed on every use, so a cached result reports exactly what a fresh
// one would). Widgets re-send the same <style> text on re-renders and themes;
// native hosts also warm it while the sandbox boots (`warmStylesheets`).
const SHEET_CACHE_SIZE = 32;
const SHEET_CACHE_MAX_TEXT = 262_144;
const sheetCache = new Map<string, { out: string; diags: [string, string][] }>();

export function clearStylesheetCache(): void {
  sheetCache.clear();
}

export function sanitizeStylesheet(css: string, diag: Diag, env: CssEnv = DEFAULT_ENV): string {
  const key = `${env.colorScheme}\u0001${env.scope ?? ''}\u0001${env.strict ? 1 : 0}${env.reducedMotion ? 1 : 0}\u0001${css}`;
  let hit = sheetCache.get(key);
  if (!hit) {
    const diags: [string, string][] = [];
    hit = { out: sanitizeStylesheetUncached(css, (k, d) => diags.push([k, d]), env), diags };
    if (css.length < SHEET_CACHE_MAX_TEXT) {
      if (sheetCache.size >= SHEET_CACHE_SIZE) sheetCache.delete(sheetCache.keys().next().value!);
      sheetCache.set(key, hit);
    }
  }
  for (const [k, d] of hit.diags) diag(k, d);
  return hit.out;
}

function sanitizeStylesheetUncached(css: string, diag: Diag, env: CssEnv): string {
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
