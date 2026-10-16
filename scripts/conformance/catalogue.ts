// The widget profile feature catalogue: every feature a widget may use (or
// tries to use), with a stable ID, its v1 status and the decision it comes
// from. Source of corpus/conformance/catalogue.json (`pnpm tsx
// scripts/conformance/build.ts catalogue`), which is meant to seed the
// versioned profile.json of the release plan: `match` names the tags,
// attributes, properties, values, selectors, at-rules, canvas members and JS
// APIs a validator/shim table would key on, `neutralise` says what happens to
// an out-of-profile feature.
//
// status:   in        — in profile v1; every client's renderer must draw it
//           out       — not in v1; the validator/shim neutralises it the same way everywhere
//           candidate — membership decided by the conformance results
// decision: the release-plan decision (D3, D3a, …), an invariant (inv2…inv6),
//           `today` (current pipeline behaviour, no v1 decision yet) or
//           `proposed` (this catalogue's proposal, to be confirmed)
// today:    what the current pipeline (validator + shim, strict) does with it

export type Status = 'in' | 'out' | 'candidate';
export type Today = 'kept' | 'stripped' | 'resolved' | 'replaced' | 'stub' | 'emulated' | 'missing' | 'host';

export interface Match {
  tags?: string[];
  svgTags?: string[];
  attrs?: string[];
  properties?: string[];
  values?: string[];
  functions?: string[];
  units?: string[];
  selectors?: string[];
  atrules?: string[];
  media?: string[];
  canvas?: string[];
  apis?: string[];
  events?: string[];
}

export interface Feature {
  id: string;
  area: string;
  title: string;
  status: Status;
  decision: string;
  today: Today;
  /** D3a: in v1 if every client's renderer draws it. */
  conditional?: boolean;
  match?: Match;
  /** For out features: what the profile does with it. */
  neutralise?: string;
  note?: string;
}

const F: Feature[] = [];
function add(area: string, id: string, title: string, status: Status, decision: string, today: Today, extra: Partial<Feature> = {}) {
  F.push({ id, area, title, status, decision, today, ...extra });
}

// ───────────── HTML elements ─────────────
const h = (id: string, title: string, s: Status, d: string, t: Today, x: Partial<Feature> = {}) => add('html', 'html.' + id, title, s, d, t, x);
h('el.structure', 'div, span, p, br, hr, wbr', 'in', 'D3a', 'kept', { match: { tags: ['div', 'span', 'p', 'br', 'hr', 'wbr'] } });
h('el.headings', 'h1–h6 with UA default sizes and margins', 'in', 'D3a', 'kept', { match: { tags: ['h1', 'h2', 'h3', 'h4', 'h5', 'h6'] } });
h('el.sections', 'header, footer, main, nav, section, article, aside, hgroup, search, address', 'in', 'D3a', 'kept', { match: { tags: ['header', 'footer', 'main', 'nav', 'section', 'article', 'aside', 'hgroup', 'search', 'address'] } });
h('el.inline-text', 'Inline text semantics with UA styles: b, i, u, s, em, strong, small, mark, code, kbd, samp, var, abbr, cite, q, del, ins, dfn, time, data', 'in', 'D3a', 'kept', { match: { tags: ['b', 'i', 'u', 's', 'em', 'strong', 'small', 'mark', 'code', 'kbd', 'samp', 'var', 'abbr', 'cite', 'q', 'del', 'ins', 'dfn', 'time', 'data'] } });
h('el.sub-sup', 'sub, sup (baseline shift, smaller size)', 'in', 'D3a', 'kept', { match: { tags: ['sub', 'sup'] } });
h('el.pre', 'pre (monospace, preserved white space and line breaks)', 'in', 'D3a', 'kept', { match: { tags: ['pre'] } });
h('el.blockquote', 'blockquote, figure, figcaption (UA indents)', 'in', 'D3a', 'kept', { match: { tags: ['blockquote', 'figure', 'figcaption'] } });
h('el.lists', 'ul, ol, li, dl, dt, dd, menu with markers', 'in', 'D3a', 'kept', { match: { tags: ['ul', 'ol', 'li', 'dl', 'dt', 'dd', 'menu'] } });
h('el.table', 'table, thead, tbody, tfoot, tr, td, th, caption, colgroup, col; colspan/rowspan', 'in', 'D3a', 'kept', { match: { tags: ['table', 'thead', 'tbody', 'tfoot', 'tr', 'td', 'th', 'caption', 'colgroup', 'col'], attrs: ['colspan', 'rowspan'] } });
h('el.ruby', 'ruby, rt, rp, bdi, bdo', 'candidate', 'proposed', 'kept', { match: { tags: ['ruby', 'rt', 'rp', 'bdi', 'bdo'] }, note: 'Annotation layout is rarely implemented by small engines; measure before keeping.' });
h('el.legacy', 'Legacy presentational elements: center, font, big, tt, strike, nobr', 'candidate', 'proposed', 'kept', { match: { tags: ['center', 'font', 'big', 'tt', 'strike', 'nobr'] }, note: 'Allowed today; a profile could drop them (render as span) to keep the table small.' });
h('el.img', '<img> with a data: PNG, JPEG, GIF or WebP; width/height attributes', 'in', 'D3a', 'kept', { match: { tags: ['img', 'picture'], attrs: ['src', 'width', 'height', 'alt'] } });
h('el.img-svg', '<img src="data:image/svg+xml">', 'candidate', 'proposed', 'kept', { match: { tags: ['img'], values: ['data:image/svg+xml'] }, note: 'Release plan, security stream: restrict image formats to PNG, JPEG, GIF, WebP (the decoder runs in the app process).' });
h('el.img-other-formats', '<img> with data: AVIF, BMP, ICO', 'out', 'proposed', 'kept', { match: { values: ['data:image/avif', 'data:image/bmp', 'data:image/x-icon', 'data:image/vnd.microsoft.icon'] }, neutralise: 'url-policy refuses the MIME: placeholder image', note: 'Release plan, security stream 7.' });
h('el.img-remote', '<img src="https://…"> (any non-data: URL)', 'out', 'inv4', 'replaced', { match: { attrs: ['src', 'srcset'] }, neutralise: 'placeholder image; no request' });
h('el.canvas', '<canvas> element (2D context through the command stream)', 'in', 'D3a', 'kept', { match: { tags: ['canvas'] } });
h('el.button', '<button> as a tappable box with UA look', 'in', 'D4', 'kept', { match: { tags: ['button'] } });
h('el.details', 'details/summary: open state and toggle on tap', 'in', 'proposed', 'kept', { match: { tags: ['details', 'summary'], events: ['toggle'] }, note: 'Probes: drawn by Blitz and litehtml; it is a disclosure, not a form.' });
h('el.a-fragment', '<a href="#id">: no navigation, :target-free', 'in', 'today', 'kept', { match: { tags: ['a'], attrs: ['href'] } });
h('el.a-external', '<a href="https://…">: href becomes "#" + data-tg-link, the tap goes to openLink (with a gesture)', 'in', 'D8', 'replaced', { match: { tags: ['a'], attrs: ['href'] } });
h('el.forms', 'Form controls: form, input (all types), select, option, optgroup, textarea, label, fieldset, legend, datalist, output', 'out', 'D3a', 'kept', { match: { tags: ['form', 'input', 'select', 'option', 'optgroup', 'textarea', 'label', 'fieldset', 'legend', 'datalist', 'output'] }, neutralise: 'not decided: drop the control (no box) or render it as an inert span; the tests expect "no control is drawn"', note: 'Kept and drawn natively today.' });
h('el.progress-meter', 'progress, meter (native gauges)', 'candidate', 'proposed', 'kept', { match: { tags: ['progress', 'meter'] }, note: 'Not input, but a native control look that differs per platform.' });
h('el.dialog-popover', 'dialog (show/showModal), popover attribute', 'out', 'proposed', 'kept', { match: { tags: ['dialog'], attrs: ['popover', 'popovertarget', 'popovertargetaction'], apis: ['showModal', 'show', 'showPopover', 'togglePopover'] }, neutralise: 'drop the methods and the popover attributes; a dialog renders as a block', note: 'The top layer is the page: in the Shadow DOM renderer a modal dialog or popover is drawn over the whole chat, outside the bubble.' });
h('el.unknown', 'Unknown / custom elements render as an inert <span data-tg-tag>', 'in', 'today', 'replaced', { match: { tags: ['*unknown*'] } });
h('el.media', 'audio, video, source, track', 'out', 'today', 'stripped', { match: { tags: ['audio', 'video', 'source', 'track'] }, neutralise: 'dropped with the subtree' });
h('el.embedding', 'script (as DOM), iframe, frame, object, embed, link, meta, base, portal, template, slot, math, fencedframe, …', 'out', 'inv2', 'stripped', { match: { tags: ['iframe', 'frame', 'frameset', 'object', 'embed', 'link', 'meta', 'base', 'portal', 'noscript', 'template', 'slot', 'math', 'fencedframe', 'applet', 'param', 'area', 'map'] }, neutralise: 'dropped with the subtree' });

// ───────────── HTML attributes ─────────────
h('attr.global', 'class, id, title, hidden, lang, style attribute', 'in', 'D3a', 'kept', { match: { attrs: ['class', 'id', 'title', 'hidden', 'lang', 'style'] } });
h('attr.dir', 'dir="rtl" / dir="auto": bidi layout', 'in', 'D3a', 'kept', { match: { attrs: ['dir'] } });
h('attr.data-aria', 'data-* and aria-* attributes (selectors, attr()); no a11y semantics in v1', 'in', 'D21', 'kept', { match: { attrs: ['data-*', 'aria-*'] }, note: 'Counted against the strict text budget.' });
h('attr.legacy-presentational', 'align, valign, bgcolor, border, cellpadding, cellspacing, color, face, nowrap', 'candidate', 'proposed', 'kept', { match: { attrs: ['align', 'valign', 'bgcolor', 'border', 'cellpadding', 'cellspacing', 'color', 'face', 'nowrap'] } });
h('attr.event-handlers', 'Inline on* handler attributes (run inside QuickJS, never reach the renderer)', 'in', 'today', 'host', { match: { attrs: ['on*'] } });
h('attr.contenteditable', 'contenteditable, spellcheck, inputmode, enterkeyhint, autocapitalize', 'out', 'proposed', 'kept', { match: { attrs: ['contenteditable', 'spellcheck', 'inputmode', 'enterkeyhint', 'autocapitalize'] }, neutralise: 'drop the attribute', note: 'Editing is form input; text selection is forbidden (D18).' });
h('attr.draggable', 'draggable (HTML drag and drop)', 'out', 'D4', 'kept', { match: { attrs: ['draggable'] }, neutralise: 'drop the attribute; drag in v1 is pointer events' });
h('attr.tabindex', 'tabindex, autofocus (keyboard focus)', 'out', 'D4', 'kept', { match: { attrs: ['tabindex'] }, neutralise: 'drop the attribute (keyboard later)' });

// ───────────── SVG ─────────────
const s = (id: string, title: string, st: Status, d: string, t: Today, x: Partial<Feature> = {}) => add('svg', 'svg.' + id, title, st, d, t, x);
s('shapes', 'Inline <svg>: rect, circle, ellipse, line, polyline, polygon, path with presentation attributes', 'in', 'D3a', 'kept', { match: { svgTags: ['svg', 'rect', 'circle', 'ellipse', 'line', 'polyline', 'polygon', 'path'], attrs: ['fill', 'stroke', 'stroke-width', 'd', 'viewBox'] } });
s('structure', 'g, defs, symbol, use (fragment href), transform attribute, viewBox/preserveAspectRatio', 'in', 'D3a', 'kept', { match: { svgTags: ['g', 'defs', 'symbol', 'use'], attrs: ['transform', 'viewBox', 'preserveAspectRatio'] } });
s('text', 'SVG text, tspan', 'in', 'D3a', 'kept', { match: { svgTags: ['text', 'tspan'] } });
s('gradients', 'linearGradient, radialGradient, stop; fill="url(#id)"', 'in', 'proposed', 'kept', { match: { svgTags: ['linearGradient', 'radialGradient', 'stop'] } });
s('css', 'CSS on inline SVG: fill/stroke from a stylesheet rule, :hover, CSS animation of SVG properties', 'candidate', 'brief', 'kept', { match: { properties: ['fill', 'stroke', 'stroke-width', 'stroke-dasharray', 'stroke-dashoffset'] }, note: 'Blitz and the litehtml prototype ignore SVG CSS (probe svg-css).' });
s('clip-mask', 'clipPath, mask, pattern, marker', 'candidate', 'proposed', 'kept', { match: { svgTags: ['clipPath', 'mask', 'pattern', 'marker'] } });
s('filters', 'SVG filter primitives (fe*)', 'candidate', 'proposed', 'kept', { match: { svgTags: ['filter', 'feGaussianBlur', 'feColorMatrix', 'feOffset', 'feMerge', 'feDropShadow', '…'] } });
s('smil', 'SMIL animation: animate, animateTransform, animateMotion', 'candidate', 'proposed', 'kept', { match: { svgTags: ['animate', 'animateTransform', 'animateMotion', 'mpath'] }, note: 'Animation driven by the renderer, outside CSS animations and WAAPI.' });
s('foreign-object', 'foreignObject (HTML inside SVG)', 'out', 'proposed', 'kept', { match: { svgTags: ['foreignObject'] }, neutralise: 'drop the subtree' });
s('image', '<image href="data:…"> inside SVG', 'candidate', 'proposed', 'kept', { match: { svgTags: ['image'] } });

// ───────────── CSS: layout and box model ─────────────
const c = (id: string, title: string, st: Status, d: string, t: Today, x: Partial<Feature> = {}) => add('css', 'css.' + id, title, st, d, t, x);
c('display.basic', 'display: block, inline, inline-block, none', 'in', 'D3a', 'kept', { match: { properties: ['display'], values: ['block', 'inline', 'inline-block', 'none'] } });
c('display.flex', 'Flexbox: display flex/inline-flex, direction, wrap, justify-content, align-items/self/content, flex grow/shrink/basis, order', 'in', 'D3a', 'kept', { match: { properties: ['display', 'flex', 'flex-direction', 'flex-wrap', 'flex-flow', 'justify-content', 'align-items', 'align-self', 'align-content', 'flex-grow', 'flex-shrink', 'flex-basis', 'order'], values: ['flex', 'inline-flex'] } });
c('gap.flex', 'gap / row-gap / column-gap in flex containers', 'in', 'D3a', 'kept', { match: { properties: ['gap', 'row-gap', 'column-gap'] }, note: 'Missing in the litehtml prototype (probe flex-gap).' });
c('display.grid', 'CSS grid: display grid, grid-template-*, grid-area/column/row, auto-flow, place-*', 'candidate', 'brief', 'kept', { match: { properties: ['grid-template-columns', 'grid-template-rows', 'grid-template-areas', 'grid-column', 'grid-row', 'grid-area', 'grid-auto-flow', 'grid-auto-rows', 'grid-auto-columns', 'place-items', 'place-content', 'place-self', 'justify-items'], values: ['grid', 'inline-grid'] } });
c('display.table', 'display: table / table-row / table-cell', 'in', 'D3a', 'kept', { match: { properties: ['display'], values: ['table', 'table-row', 'table-cell', 'table-caption'] } });
c('display.other', 'display: flow-root, contents, list-item', 'candidate', 'proposed', 'kept', { match: { values: ['flow-root', 'contents', 'list-item'] } });
c('box.size', 'width, height, min-/max-width/height (px, %, auto, fit-content, min/max-content)', 'in', 'D3a', 'kept', { match: { properties: ['width', 'height', 'min-width', 'min-height', 'max-width', 'max-height'] } });
c('box.spacing', 'margin, padding (incl. auto margins and negative margins)', 'in', 'D3a', 'kept', { match: { properties: ['margin', 'margin-*', 'padding', 'padding-*'] } });
c('box.border', 'border width/style/colour (solid, dashed, dotted, double)', 'in', 'D3a', 'kept', { match: { properties: ['border', 'border-*'] } });
c('box.radius', 'border-radius, including clipping of the background', 'in', 'D3a', 'kept', { match: { properties: ['border-radius', 'border-*-radius'] } });
c('box.sizing', 'box-sizing: border-box / content-box', 'in', 'D3a', 'kept', { match: { properties: ['box-sizing'] } });
c('box.aspect-ratio', 'aspect-ratio', 'in', 'D3a', 'kept', { match: { properties: ['aspect-ratio'] }, note: 'Missing in the litehtml prototype.' });
c('box.logical', 'Logical properties: margin-inline, padding-block, inset-inline, inline-size, …', 'candidate', 'proposed', 'kept', { match: { properties: ['margin-inline*', 'margin-block*', 'padding-inline*', 'padding-block*', 'inset-inline*', 'inset-block*', 'inline-size', 'block-size'] }, note: 'Used by LLM widgets (15 % of generated); missing in the litehtml prototype.' });
c('box.outline', 'outline (width, style, colour, offset)', 'in', 'D3a', 'kept', { match: { properties: ['outline', 'outline-*'] } });
c('box.shadow', 'box-shadow (outer, inset, spread, several layers)', 'in', 'D3a', 'kept', { match: { properties: ['box-shadow'] } });
c('box.float', 'float, clear', 'in', 'D3a', 'kept', { match: { properties: ['float', 'clear'] } });
c('box.columns', 'Multi-column layout (columns, column-count, column-gap)', 'candidate', 'proposed', 'kept', { match: { properties: ['columns', 'column-count', 'column-width', 'column-rule'] } });
c('box.object-fit', 'object-fit, object-position on <img>', 'in', 'proposed', 'kept', { match: { properties: ['object-fit', 'object-position'] } });
c('position.relative-absolute', 'position: relative / absolute with inset, top/right/bottom/left', 'in', 'D3a', 'kept', { match: { properties: ['position', 'top', 'right', 'bottom', 'left', 'inset'], values: ['relative', 'absolute'] } });
c('position.fixed', 'position: fixed (relative to the bubble, never the chat)', 'in', 'proposed', 'kept', { match: { values: ['fixed'] } });
c('position.sticky', 'position: sticky', 'out', 'proposed', 'kept', { match: { values: ['sticky'] }, neutralise: 'becomes relative (no scrolling inside a widget)' });
c('position.z-index', 'z-index and stacking order', 'in', 'D3a', 'kept', { match: { properties: ['z-index'] } });
c('overflow.clip', 'overflow: hidden / clip clips the content (with border-radius)', 'in', 'D3a', 'kept', { match: { properties: ['overflow', 'overflow-x', 'overflow-y'], values: ['hidden', 'clip'] } });
c('overflow.scroll', 'overflow: auto / scroll (scrolling inside the widget)', 'out', 'D3a', 'kept', { match: { properties: ['overflow', 'overflow-x', 'overflow-y'], values: ['auto', 'scroll', 'overlay'] }, neutralise: 'becomes hidden: the box clips, no scrollbar, no scroll gesture' });
c('visibility', 'visibility: hidden / visible', 'in', 'D3a', 'kept', { match: { properties: ['visibility'] } });
c('opacity', 'opacity', 'in', 'D3a', 'kept', { match: { properties: ['opacity'] }, note: 'Missing in the litehtml prototype.' });

// ───────────── CSS: colour and backgrounds ─────────────
c('color.basic', 'Colours: named, #hex (3/4/6/8), rgb()/rgba(), hsl()/hsla(), transparent, currentColor', 'in', 'D3a', 'kept', { match: { properties: ['color', 'background-color', 'border-color'], functions: ['rgb', 'rgba', 'hsl', 'hsla'] } });
c('color.modern', 'Colour functions: hwb(), lab(), lch(), oklab(), oklch(), color()', 'candidate', 'proposed', 'kept', { match: { functions: ['hwb', 'lab', 'lch', 'oklab', 'oklch', 'color'] } });
c('color.mix', 'color-mix()', 'candidate', 'proposed', 'kept', { match: { functions: ['color-mix'] }, note: 'Missing in the litehtml prototype.' });
c('color.light-dark', 'light-dark() and color-scheme: light dark, resolved to the chat theme', 'in', 'today', 'resolved', { match: { functions: ['light-dark'], properties: ['color-scheme'] } });
c('background.basic', 'background-color, background-image: url(data:…), background-size/position/repeat, several layers', 'in', 'D3a', 'kept', { match: { properties: ['background', 'background-color', 'background-image', 'background-size', 'background-position', 'background-repeat', 'background-origin'] } });
c('background.clip-text', 'background-clip: text (gradient text)', 'candidate', 'proposed', 'kept', { match: { properties: ['background-clip', '-webkit-background-clip'], values: ['text'] } });
c('gradient.linear', 'linear-gradient(), repeating-linear-gradient()', 'in', 'D3a', 'kept', { match: { functions: ['linear-gradient', 'repeating-linear-gradient'] } });
c('gradient.radial', 'radial-gradient(), repeating-radial-gradient()', 'in', 'D3a', 'kept', { match: { functions: ['radial-gradient', 'repeating-radial-gradient'] } });
c('gradient.conic', 'conic-gradient()', 'candidate', 'brief', 'kept', { match: { functions: ['conic-gradient', 'repeating-conic-gradient'] } });
c('url.remote', 'url() / image-set() with anything but a data: image or #fragment', 'out', 'inv4', 'stripped', { match: { functions: ['url', 'image-set', 'image', 'cross-fade'] }, neutralise: 'the declaration is dropped' });

// ───────────── CSS: text and fonts ─────────────
c('font.generic', 'font-family with system generic families: system-ui, sans-serif, serif, monospace', 'in', 'D3b', 'kept', { match: { properties: ['font-family', 'font'], values: ['system-ui', 'sans-serif', 'serif', 'monospace'] } });
c('font.ui-families', 'font-family: ui-sans-serif, ui-serif, ui-monospace, ui-rounded', 'candidate', 'proposed', 'kept', { match: { properties: ['font-family', 'font'], values: ['ui-sans-serif', 'ui-serif', 'ui-monospace', 'ui-rounded'] }, note: 'Generic per CSS Fonts 4, but only WebKit resolves them; Chromium falls back to the default face.' });
c('font.named', 'font-family naming a font ("Helvetica", "Courier New", "Comic Sans MS", …)', 'out', 'D3b', 'kept', { match: { properties: ['font-family', 'font'] }, neutralise: 'the validator drops non-generic names from the list; an empty list falls back to the default (system-ui)' });
c('font.face', '@font-face (web fonts)', 'out', 'D3b', 'stripped', { match: { atrules: ['font-face'] }, neutralise: 'the at-rule is dropped' });
c('font.basic', 'font-size, font-weight (100–900, bold), font-style italic, line-height, font shorthand', 'in', 'D3a', 'kept', { match: { properties: ['font-size', 'font-weight', 'font-style', 'line-height', 'font'] } });
c('font.variant-numeric', 'font-variant-numeric: tabular-nums (and font-feature-settings "tnum")', 'candidate', 'brief', 'kept', { match: { properties: ['font-variant-numeric', 'font-feature-settings'] } });
c('text.align', 'text-align (left, right, center, justify, start, end), text-indent', 'in', 'D3a', 'kept', { match: { properties: ['text-align', 'text-indent'] } });
c('text.decoration', 'text-decoration (underline, line-through, overline; colour, style, thickness)', 'in', 'D3a', 'kept', { match: { properties: ['text-decoration', 'text-decoration-*'] } });
c('text.transform', 'text-transform, letter-spacing, word-spacing', 'in', 'D3a', 'kept', { match: { properties: ['text-transform', 'letter-spacing', 'word-spacing'] } });
c('text.white-space', 'white-space: normal, nowrap, pre, pre-wrap, pre-line', 'in', 'D3a', 'kept', { match: { properties: ['white-space'] }, note: 'pre missing in the litehtml prototype.' });
c('text.wrap', 'overflow-wrap / word-break (breaking long words)', 'in', 'D3a', 'kept', { match: { properties: ['overflow-wrap', 'word-wrap', 'word-break'] } });
c('text.overflow', 'text-overflow: ellipsis (single line)', 'candidate', 'brief', 'kept', { match: { properties: ['text-overflow'] } });
c('text.line-clamp', '-webkit-line-clamp / line-clamp (multi-line ellipsis)', 'candidate', 'proposed', 'kept', { match: { properties: ['-webkit-line-clamp', 'line-clamp', '-webkit-box-orient'] } });
c('text.shadow', 'text-shadow', 'candidate', 'proposed', 'kept', { match: { properties: ['text-shadow'] }, note: 'Not drawn by Blitz or the litehtml prototype (probe text-shadow).' });
c('text.vertical-align', 'vertical-align (baseline, middle, top, sub, super, lengths)', 'in', 'D3a', 'kept', { match: { properties: ['vertical-align'] } });
c('text.list-style', 'list-style-type / position (disc, decimal, none, …)', 'in', 'D3a', 'kept', { match: { properties: ['list-style', 'list-style-type', 'list-style-position'] } });
c('text.writing-mode', 'writing-mode, text-orientation (vertical text)', 'out', 'proposed', 'kept', { match: { properties: ['writing-mode', 'text-orientation'] }, neutralise: 'the declaration is dropped' });
c('text.rtl', 'Right-to-left and bidi text (direction, unicode-bidi, Hebrew/Arabic shaping)', 'in', 'D3a', 'kept', { match: { properties: ['direction', 'unicode-bidi'] } });
c('text.emoji', 'Colour emoji glyphs (system emoji font)', 'in', 'D3a', 'kept', { note: 'Blitz on iOS had blank emoji on the vello backend.' });
c('text.selection', 'Text selection (user-select: text, ::selection, select by drag or long press)', 'out', 'D18', 'kept', { match: { properties: ['user-select', '-webkit-user-select'], selectors: ['::selection'] }, neutralise: 'the client renders with selection disabled whatever the widget says' });

// ───────────── CSS: transforms, effects ─────────────
c('transform.2d', '2D transforms: translate, rotate, scale, skew, matrix; transform-origin; the translate/rotate/scale properties', 'in', 'D3a', 'kept', { match: { properties: ['transform', 'transform-origin', 'translate', 'rotate', 'scale'], functions: ['translate', 'translateX', 'translateY', 'rotate', 'scale', 'scaleX', 'scaleY', 'skew', 'skewX', 'skewY', 'matrix'] }, note: 'Missing in the litehtml prototype.' });
c('transform.3d', '3D transforms: rotateX/Y/3d, translateZ/3d, scaleZ/3d, matrix3d, perspective, transform-style, backface-visibility', 'out', 'D3a', 'kept', { match: { properties: ['perspective', 'perspective-origin', 'transform-style', 'backface-visibility'], functions: ['rotateX', 'rotateY', 'rotateZ', 'rotate3d', 'translateZ', 'translate3d', 'scaleZ', 'scale3d', 'matrix3d', 'perspective'] }, neutralise: 'a transform with a 3D function is dropped; perspective, transform-style and backface-visibility are dropped' });
c('filter.blur-brightness-drop-shadow', 'filter: blur(), brightness(), drop-shadow()', 'in', 'D3a', 'kept', { conditional: true, match: { properties: ['filter'], functions: ['blur', 'brightness', 'drop-shadow'] }, note: 'Not drawn by Blitz or the litehtml prototype today (probe filter).' });
c('filter.other', 'filter: grayscale(), sepia(), saturate(), contrast(), hue-rotate(), invert(), opacity()', 'candidate', 'proposed', 'kept', { match: { properties: ['filter'], functions: ['grayscale', 'sepia', 'saturate', 'contrast', 'hue-rotate', 'invert', 'opacity'] } });
c('filter.url', 'filter: url(#svg-filter)', 'out', 'proposed', 'kept', { match: { properties: ['filter'], functions: ['url'] }, neutralise: 'the declaration is dropped' });
c('backdrop-filter', 'backdrop-filter: blur(), brightness()', 'in', 'D3a', 'kept', { conditional: true, match: { properties: ['backdrop-filter', '-webkit-backdrop-filter'] } });
c('blend', 'mix-blend-mode, isolation, background-blend-mode', 'candidate', 'proposed', 'kept', { match: { properties: ['mix-blend-mode', 'isolation', 'background-blend-mode'] } });
c('clip-path', 'clip-path: inset(), circle(), ellipse(), polygon()', 'candidate', 'proposed', 'kept', { match: { properties: ['clip-path'] }, note: 'Blitz draws it, the litehtml prototype does not.' });
c('mask', 'mask / -webkit-mask with gradients or data: images', 'candidate', 'proposed', 'kept', { match: { properties: ['mask', 'mask-*', '-webkit-mask', '-webkit-mask-*'] } });
c('pointer-events', 'pointer-events: none / auto (hit testing passes through)', 'candidate', 'brief', 'kept', { match: { properties: ['pointer-events'] } });
c('touch-action', 'touch-action: none / pan-x / pan-y (the widget claims a swipe from the chat)', 'candidate', 'brief', 'kept', { match: { properties: ['touch-action'] }, note: 'D4: drag in v1 needs gesture arbitration; the Android host reads it from the renderer (`gesture` message).' });
c('cursor', 'cursor (desktop only)', 'in', 'D3', 'kept', { match: { properties: ['cursor'] }, note: 'Progressive enhancement like :hover; no-op on touch clients.' });

// ───────────── CSS: values, units, functions ─────────────
c('value.units', 'Units px, em, rem, %, ch, ex; pt/cm/mm/in', 'in', 'D3a', 'kept', { match: { units: ['px', 'em', 'rem', '%', 'ch', 'ex', 'pt', 'cm', 'mm', 'in'] } });
c('value.vw', 'vw / vmin / vmax / vi: the bubble width', 'in', 'today', 'replaced', { match: { units: ['vw', 'svw', 'lvw', 'dvw', 'vi', 'vmin', 'vmax'] }, note: 'Container units (cqw) on the Shadow DOM renderer.' });
c('value.vh', 'vh / dvh / svh / vb: height-relative units', 'candidate', 'proposed', 'kept', { match: { units: ['vh', 'svh', 'lvh', 'dvh', 'vb'] }, note: 'The bubble height comes from the content, so vh is circular: the iframe uses its own height, the shadow renderer the container height.' });
c('value.math', 'calc(), min(), max(), clamp()', 'in', 'D3a', 'kept', { match: { functions: ['calc', 'min', 'max', 'clamp'] }, note: 'Missing in the litehtml prototype.' });
c('value.custom-properties', 'Custom properties and var() with fallbacks; inheritance', 'in', 'D3a', 'kept', { match: { functions: ['var'], properties: ['--*'] } });
c('value.keywords', 'inherit, initial, unset, revert; !important', 'in', 'D3a', 'kept', { match: { values: ['inherit', 'initial', 'unset', 'revert', '!important'] } });
c('value.content', 'content in ::before/::after: strings, attr(data-*), counters', 'in', 'D3a', 'kept', { match: { properties: ['content', 'counter-reset', 'counter-increment', 'quotes'], functions: ['attr', 'counter', 'counters'] }, note: 'Strict: attr() reads only budgeted attributes.' });
c('value.env', 'env() (safe-area-inset-*, …)', 'out', 'proposed', 'kept', { match: { functions: ['env'] }, neutralise: 'the declaration is dropped (or env() resolves to its fallback)' });

// ───────────── CSS: selectors ─────────────
const sel = (id: string, title: string, st: Status, d: string, t: Today, x: Partial<Feature> = {}) => add('selector', 'sel.' + id, title, st, d, t, x);
sel('basic', 'Type, class, id, universal and attribute selectors; descendant, child, + and ~ combinators; selector lists', 'in', 'D3a', 'kept', { match: { selectors: ['E', '.c', '#i', '*', '[a]', '[a=v]', '[a^=v]', '[a$=v]', '[a*=v]', '[a~=v]', 'A B', 'A > B', 'A + B', 'A ~ B'] } });
sel('structural', ':first-child, :last-child, :nth-child(), :nth-last-child(), :nth-of-type(), :only-child, :empty, :not()', 'in', 'D3a', 'kept', { match: { selectors: [':first-child', ':last-child', ':nth-child', ':nth-last-child', ':nth-of-type', ':first-of-type', ':last-of-type', ':only-child', ':empty', ':not'] } });
sel('is-where', ':is(), :where()', 'in', 'D3a', 'kept', { match: { selectors: [':is', ':where'] } });
sel('has', ':has()', 'candidate', 'proposed', 'kept', { match: { selectors: [':has'] }, note: 'Not matched by Blitz or the litehtml prototype (probe has-selector).' });
sel('root', ':root (custom properties on the widget root)', 'in', 'D3a', 'replaced', { match: { selectors: [':root'] }, note: '[data-tw-root] on the Shadow DOM renderer.' });
sel('before-after', '::before, ::after', 'in', 'D3a', 'kept', { match: { selectors: ['::before', '::after'] } });
sel('first-letter-line', '::first-letter, ::first-line, ::marker', 'candidate', 'proposed', 'kept', { match: { selectors: ['::first-letter', '::first-line', '::marker'] } });
sel('hover', ':hover (progressive enhancement: desktop with a pointer; no-op on touch)', 'in', 'D3', 'kept', { match: { selectors: [':hover'] } });
sel('active', ':active (pressed feedback while a tap is down)', 'in', 'proposed', 'kept', { match: { selectors: [':active'] }, note: 'Used by 32 % of the HTMLBubbles demos for tap feedback.' });
sel('focus', ':focus, :focus-visible, :focus-within', 'out', 'D4', 'kept', { match: { selectors: [':focus', ':focus-visible', ':focus-within'] }, neutralise: 'never matches (no keyboard focus in v1)' });
sel('form-state', ':checked, :disabled, :enabled, :valid, :invalid, :placeholder-shown, ::placeholder', 'out', 'D3a', 'kept', { match: { selectors: [':checked', ':disabled', ':enabled', ':valid', ':invalid', ':placeholder-shown', '::placeholder'] }, neutralise: 'follows the forms decision' });
sel('host-scoped', ':host, :host-context(), ::slotted(), ::part(), :scope', 'out', 'inv6', 'stripped', { match: { selectors: [':host', ':host-context', '::slotted', '::part', ':scope'] }, neutralise: 'the rule is dropped (Shadow DOM scope); in a document scope they match nothing' });
sel('target', ':target', 'out', 'proposed', 'kept', { match: { selectors: [':target'] }, neutralise: 'never matches (no navigation)' });

// ───────────── CSS: at-rules and nesting ─────────────
const at = (id: string, title: string, st: Status, d: string, t: Today, x: Partial<Feature> = {}) => add('at-rule', 'at.' + id, title, st, d, t, x);
at('media.width', '@media (min-width/max-width/orientation): the bubble width', 'in', 'D3a', 'replaced', { match: { atrules: ['media'], media: ['width', 'min-width', 'max-width', 'orientation', 'aspect-ratio'] }, note: '@container on the Shadow DOM renderer.' });
at('media.color-scheme', '@media (prefers-color-scheme): the chat theme, live on theme change', 'in', 'D14', 'resolved', { match: { atrules: ['media'], media: ['prefers-color-scheme'] } });
at('media.reduced-motion', '@media (prefers-reduced-motion)', 'candidate', 'proposed', 'kept', { match: { media: ['prefers-reduced-motion'] }, note: 'Today the renderer engine answers from the OS; the profile could make the client answer.' });
at('media.hover-pointer', '@media (hover: hover), (pointer: fine|coarse)', 'candidate', 'proposed', 'kept', { match: { media: ['hover', 'any-hover', 'pointer', 'any-pointer'] }, note: 'Lets a widget gate :hover styles; the answer must come from the client, not the engine.' });
at('media.other', '@media print, resolution, other media features', 'candidate', 'proposed', 'kept', { match: { media: ['print', 'resolution', 'display-mode', 'forced-colors', 'prefers-contrast'] } });
at('supports', '@supports evaluated from the profile tables (not the renderer engine)', 'in', 'D23', 'kept', { match: { atrules: ['supports'] }, note: 'Today the engine evaluates the condition: "yes" for features the validator strips.' });
at('keyframes', '@keyframes (and -webkit-keyframes)', 'in', 'D3a', 'kept', { match: { atrules: ['keyframes', '-webkit-keyframes'] } });
at('import-namespace', '@import, @namespace', 'out', 'inv4', 'stripped', { match: { atrules: ['import', 'namespace'] }, neutralise: 'dropped' });
at('container', '@container queries (container-type in the widget)', 'candidate', 'proposed', 'kept', { match: { atrules: ['container'], properties: ['container', 'container-type', 'container-name'] } });
at('layer', '@layer cascade layers', 'candidate', 'proposed', 'kept', { match: { atrules: ['layer'] } });
at('property', '@property (typed custom properties)', 'candidate', 'proposed', 'kept', { match: { atrules: ['property'] } });
at('exotic', '@counter-style, @starting-style, @scope, @page, @font-feature-values, @font-palette-values, @position-try', 'out', 'proposed', 'kept', { match: { atrules: ['counter-style', 'starting-style', 'scope', 'page', 'font-feature-values', 'font-palette-values', 'position-try'] }, neutralise: 'dropped like unknown at-rules' });
at('nesting', 'CSS nesting (.a { .b { } }, &)', 'candidate', 'brief', 'stripped', { match: { selectors: ['&'] }, neutralise: 'flatten in the validator, or drop the nested rules', note: 'Validator hole today: css-tree regenerates .a{} and loses the nested rules (probe nesting).' });

// ───────────── Animation ─────────────
const an = (id: string, title: string, st: Status, d: string, t: Today, x: Partial<Feature> = {}) => add('animation', 'anim.' + id, title, st, d, t, x);
an('transition', 'CSS transitions (property, duration, delay incl. negative, timing functions)', 'in', 'D3a', 'kept', { match: { properties: ['transition', 'transition-*'] } });
an('keyframes', 'CSS animations: animation-name/duration/delay/iteration-count/direction/fill-mode/timing-function', 'in', 'D3a', 'kept', { match: { properties: ['animation', 'animation-*'] }, note: 'Not driven by Blitz or the litehtml prototype.' });
an('play-state', 'animation-play-state: paused (and negative delays) freeze a frame', 'in', 'D3a', 'kept', { match: { properties: ['animation-play-state'] } });
an('easing', 'Timing functions: ease*, linear, cubic-bezier(), steps(), linear() stops', 'in', 'D3a', 'kept', { match: { functions: ['cubic-bezier', 'steps', 'linear'] } });
an('events', 'animationstart/end/iteration and transitionend events reach the widget', 'in', 'D3a', 'kept', { match: { events: ['animationstart', 'animationend', 'animationiteration', 'transitionend'] } });
an('waapi', 'Web Animations: Element.animate, getAnimations, Animation play/pause/currentTime/finished, document.timeline', 'in', 'D3a', 'kept', { conditional: true, match: { apis: ['Element.animate', 'Element.getAnimations', 'document.getAnimations', 'document.timeline', 'Animation', 'KeyframeEffect'] }, note: 'Behind the webAnimations feature (D5: profile flag, the same on every client); without it animate() is a finished stub.' });
an('waapi-css-control', 'Web Animations control of CSS animations/transitions (pause, seek, cancel via getAnimations)', 'in', 'D3a', 'kept', { conditional: true, match: { apis: ['CSSAnimation', 'CSSTransition'] } });
an('magic-move', 'Magic move (shared-element morph between renders)', 'out', 'D3a', 'missing', { neutralise: 'not offered (it belonged to the render() model)' });

// ───────────── Canvas 2D ─────────────
const cv = (id: string, title: string, st: Status, d: string, t: Today, x: Partial<Feature> = {}) => add('canvas', 'canvas.' + id, title, st, d, t, x);
cv('rect', 'fillRect, strokeRect, clearRect', 'in', 'D3a', 'kept', { match: { canvas: ['fillRect', 'strokeRect', 'clearRect'] } });
cv('path', 'Paths: beginPath, moveTo, lineTo, arc, arcTo, bezierCurveTo, quadraticCurveTo, ellipse, rect, roundRect, closePath, fill (nonzero/evenodd), stroke', 'in', 'D3a', 'kept', { match: { canvas: ['beginPath', 'moveTo', 'lineTo', 'arc', 'arcTo', 'bezierCurveTo', 'quadraticCurveTo', 'ellipse', 'rect', 'roundRect', 'closePath', 'fill', 'stroke'] } });
cv('line-style', 'lineWidth, lineCap, lineJoin, miterLimit, setLineDash, lineDashOffset', 'in', 'D3a', 'kept', { match: { canvas: ['lineWidth', 'lineCap', 'lineJoin', 'miterLimit', 'setLineDash', 'lineDashOffset'] } });
cv('colors', 'fillStyle / strokeStyle with CSS colours', 'in', 'D3a', 'kept', { match: { canvas: ['fillStyle', 'strokeStyle'] } });
cv('gradients', 'createLinearGradient, createRadialGradient, addColorStop', 'in', 'D3a', 'kept', { match: { canvas: ['createLinearGradient', 'createRadialGradient', 'addColorStop'] } });
cv('conic-gradient', 'createConicGradient', 'candidate', 'brief', 'kept', { match: { canvas: ['createConicGradient'] } });
cv('pattern', 'createPattern from a canvas or image', 'candidate', 'proposed', 'kept', { match: { canvas: ['createPattern'] } });
cv('transform', 'save, restore, translate, rotate, scale, transform, setTransform, resetTransform', 'in', 'D3a', 'kept', { match: { canvas: ['save', 'restore', 'translate', 'rotate', 'scale', 'transform', 'setTransform', 'resetTransform'] } });
cv('clip', 'clip() with a path', 'in', 'D3a', 'kept', { match: { canvas: ['clip'] } });
cv('text', 'fillText, strokeText, font, textAlign, textBaseline, measureText', 'in', 'D3a', 'kept', { match: { canvas: ['fillText', 'strokeText', 'font', 'textAlign', 'textBaseline', 'measureText'] } });
cv('alpha', 'globalAlpha', 'in', 'D3a', 'kept', { match: { canvas: ['globalAlpha'] } });
cv('composite', 'globalCompositeOperation (source-over, destination-out, multiply, …)', 'candidate', 'proposed', 'kept', { match: { canvas: ['globalCompositeOperation'] } });
cv('shadow', 'shadowBlur, shadowColor, shadowOffsetX/Y', 'candidate', 'proposed', 'kept', { match: { canvas: ['shadowBlur', 'shadowColor', 'shadowOffsetX', 'shadowOffsetY'] } });
cv('filter', 'ctx.filter (CSS filter on canvas drawing)', 'candidate', 'brief', 'kept', { match: { canvas: ['filter'] } });
cv('path2d', 'Path2D objects (from SVG path data or built), fill/stroke/clip with them', 'in', 'D3a', 'kept', { match: { canvas: ['Path2D'] } });
cv('draw-image', 'drawImage from an <img> (data:) or another canvas', 'in', 'D3a', 'kept', { match: { canvas: ['drawImage'] } });
cv('image-data', 'getImageData / putImageData / createImageData (pixel read-back)', 'candidate', 'proposed', 'kept', { match: { canvas: ['getImageData', 'putImageData', 'createImageData'] }, note: 'getImageData is a sync query answered by the renderer: a native renderer must rasterize to answer.' });
cv('hit-test', 'isPointInPath / isPointInStroke', 'in', 'proposed', 'kept', { match: { canvas: ['isPointInPath', 'isPointInStroke'] } });
cv('hidpi', 'Backing store sized by width/height attributes, CSS size independent (devicePixelRatio scaling)', 'in', 'D3a', 'kept', { match: { attrs: ['width', 'height'], apis: ['devicePixelRatio'] } });
cv('readback', 'toDataURL, toBlob, captureStream', 'out', 'proposed', 'stub', { match: { canvas: ['toDataURL', 'toBlob', 'captureStream'] }, neutralise: 'toBlob/captureStream are stubs; toDataURL is a renderer query today' });
cv('webgl', 'WebGL, OffscreenCanvas, bitmaprenderer contexts', 'out', 'D6', 'stub', { match: { apis: ['getContext("webgl")', 'getContext("webgl2")', 'OffscreenCanvas'] }, neutralise: 'getContext returns null; OffscreenCanvas throws' });

// ───────────── JS and DOM APIs of the shim ─────────────
const js = (id: string, title: string, st: Status, d: string, t: Today, x: Partial<Feature> = {}) => add('js', 'js.' + id, title, st, d, t, x);
js('dom.core', 'DOM building: createElement(NS), append/insertBefore/remove, textContent, innerHTML (parsed in the sandbox), attributes, classList, dataset', 'in', 'D11', 'kept', { match: { apis: ['document.createElement', 'Node.appendChild', 'Element.innerHTML', 'Element.classList', 'HTMLElement.dataset'] } });
js('dom.query', 'querySelector(All), getElementById, getElementsBy*, closest, matches', 'in', 'D11', 'kept', { match: { apis: ['querySelector', 'querySelectorAll', 'getElementById', 'closest', 'matches'] } });
js('dom.style', 'element.style (CSSOM properties, setProperty, cssText), className', 'in', 'D11', 'kept', { match: { apis: ['CSSStyleDeclaration.setProperty', 'element.style'] } });
js('dom.events', 'addEventListener (capture and bubble), dispatchEvent of CustomEvent, preventDefault, stopPropagation', 'in', 'D11', 'kept', { match: { apis: ['EventTarget.addEventListener', 'CustomEvent', 'Event.stopPropagation'] } });
js('layout.rect', 'getBoundingClientRect, getClientRects (answered by the renderer)', 'in', 'today', 'host', { match: { apis: ['Element.getBoundingClientRect', 'Element.getClientRects'] } });
js('layout.box', 'offsetWidth/Height/Top/Left, clientWidth/Height, scrollWidth/Height', 'in', 'today', 'host', { match: { apis: ['offsetWidth', 'offsetHeight', 'clientWidth', 'scrollHeight'] } });
js('layout.computed-style', 'getComputedStyle (incl. pseudo-elements and custom properties)', 'in', 'today', 'host', { match: { apis: ['getComputedStyle'] } });
js('layout.match-media', 'matchMedia (prefers-color-scheme answered from the chat theme)', 'in', 'today', 'host', { match: { apis: ['matchMedia'] } });
js('layout.resize-observer', 'ResizeObserver', 'in', 'today', 'host', { match: { apis: ['ResizeObserver'] } });
js('layout.intersection-observer', 'IntersectionObserver (emulated: always visible)', 'in', 'today', 'emulated', { match: { apis: ['IntersectionObserver'] } });
js('layout.element-from-point', 'document.elementFromPoint', 'in', 'today', 'host', { match: { apis: ['document.elementFromPoint'] } });
js('layout.measure-text', 'Text measurement through canvas measureText (fonts of the client)', 'in', 'today', 'host', { match: { apis: ['CanvasRenderingContext2D.measureText'] } });
js('timers', 'setTimeout / setInterval / clear*, ordering, clamped delays', 'in', 'today', 'kept', { match: { apis: ['setTimeout', 'setInterval', 'clearTimeout', 'clearInterval'] } });
js('raf', 'requestAnimationFrame / cancelAnimationFrame (frames from the renderer)', 'in', 'today', 'kept', { match: { apis: ['requestAnimationFrame', 'cancelAnimationFrame'] } });
js('microtasks', 'Promise, async/await, queueMicrotask', 'in', 'today', 'kept', { match: { apis: ['Promise', 'queueMicrotask'] } });
js('storage.local', 'localStorage: per (account, bot, message), preloaded, 16 KB, QuotaExceededError past it', 'in', 'D17', 'emulated', { match: { apis: ['localStorage'] }, note: 'Today an in-memory map per widget instance with no quota.' });
js('storage.session', 'sessionStorage: same key, in memory until app restart', 'in', 'D17', 'emulated', { match: { apis: ['sessionStorage'] } });
js('intl', 'Intl.NumberFormat, DateTimeFormat, PluralRules, RelativeTimeFormat, ListFormat, Collator (host ICU)', 'in', 'today', 'host', { match: { apis: ['Intl.NumberFormat', 'Intl.DateTimeFormat', 'Intl.PluralRules', 'Intl.RelativeTimeFormat', 'Intl.ListFormat', 'Intl.Collator'] } });
js('intl.format-to-parts', 'Intl.NumberFormat/DateTimeFormat formatToParts', 'candidate', 'proposed', 'missing', { match: { apis: ['formatToParts'] }, note: 'Open item: missing on iOS and Android glue.' });
js('locale', 'navigator.language reflects the client locale', 'in', 'proposed', 'stub', { match: { apis: ['navigator.language'] }, note: 'Hard-coded en-US today; the release plan wants the real locale.' });
js('crypto-random', 'crypto.getRandomValues, crypto.randomUUID, Math.random', 'in', 'today', 'host', { match: { apis: ['crypto.getRandomValues', 'crypto.randomUUID'] } });
js('builtins', 'TextEncoder/TextDecoder, URL, URLSearchParams, structuredClone, AbortController, atob/btoa', 'in', 'today', 'kept', { match: { apis: ['TextEncoder', 'TextDecoder', 'URL', 'URLSearchParams', 'structuredClone', 'AbortController', 'atob', 'btoa'] } });
js('eval', 'eval / new Function inside QuickJS (never in a browser context)', 'in', 'inv1', 'kept', { match: { apis: ['eval', 'Function'] } });
js('css-supports', 'CSS.supports() answered from the profile tables', 'in', 'D23', 'stub', { match: { apis: ['CSS.supports'] }, note: 'Today always true.' });
js('visibility', 'document.hidden / visibilityState / visibilitychange when the bubble leaves the screen', 'in', 'today', 'host', { match: { apis: ['document.hidden', 'document.visibilityState'], events: ['visibilitychange'] } });
js('libraries', 'Bundled libraries by CDN name: Chart.js 4, D3 7 (nothing is fetched)', 'candidate', 'proposed', 'host', { match: { apis: ['<script src=cdn chart.js>', '<script src=cdn d3>'] }, note: 'The release plan asks for the v1 list.' });
js('network', 'fetch, XMLHttpRequest, WebSocket, EventSource, RTCPeerConnection, sendBeacon, importScripts, Worker, BroadcastChannel', 'out', 'inv5', 'stub', { match: { apis: ['fetch', 'XMLHttpRequest', 'WebSocket', 'EventSource', 'RTCPeerConnection', 'navigator.sendBeacon', 'importScripts', 'Worker', 'SharedWorker', 'BroadcastChannel', 'WebTransport'] }, neutralise: 'NetworkDisabledError (fetch rejects with it)' });
js('navigation', 'location assignment, history, window.open (→ openLink), form submission', 'out', 'inv6', 'stub', { match: { apis: ['location', 'history.pushState', 'window.open'] }, neutralise: 'navigation never happens; window.open goes through openLink' });
js('dialogs', 'alert, confirm, prompt, print', 'out', 'today', 'stub', { match: { apis: ['alert', 'confirm', 'prompt', 'print'] }, neutralise: 'non-blocking stubs: confirm → false, prompt → null' });
js('device', 'clipboard, share, vibrate, geolocation, mediaDevices, Notification', 'out', 'D8', 'stub', { match: { apis: ['navigator.clipboard', 'navigator.share', 'navigator.vibrate', 'navigator.geolocation', 'navigator.mediaDevices'] }, neutralise: 'rejected promises / undefined; haptics come from the SDK' });
js('shadow-dom', 'attachShadow, customElements.define', 'out', 'today', 'stub', { match: { apis: ['Element.attachShadow', 'customElements.define'] }, neutralise: 'attachShadow throws NotSupportedError, customElements.define is a no-op' });
js('blob-file', 'Blob, FileReader, URL.createObjectURL', 'out', 'inv4', 'stub', { match: { apis: ['Blob', 'FileReader', 'URL.createObjectURL'] }, neutralise: 'inert stubs' });
js('fullscreen', 'requestFullscreen', 'out', 'D12a', 'stub', { match: { apis: ['Element.requestFullscreen'] }, neutralise: 'rejects' });

// ───────────── TelegramWidget SDK ─────────────
const tw = (id: string, title: string, st: Status, d: string, t: Today, x: Partial<Feature> = {}) => add('sdk', 'tw.' + id, title, st, d, t, x);
tw('ready', 'TelegramWidget.ready()', 'in', 'D8', 'kept', { match: { apis: ['TelegramWidget.ready'] } });
tw('theme', 'TelegramWidget.theme and onTheme(cb): "light" | "dark", called on change', 'in', 'D14', 'kept', { match: { apis: ['TelegramWidget.theme', 'TelegramWidget.onTheme'] } });
tw('theme-params', 'themeParams palette and --tg-theme-* CSS variables, theme change event', 'in', 'D14', 'missing', { match: { apis: ['TelegramWidget.themeParams'], properties: ['--tg-theme-*'] } });
tw('open-link', 'openLink(url): https only, in the task of a real tap, confirmed by the client with the full URL', 'in', 'D8', 'kept', { match: { apis: ['TelegramWidget.openLink'] } });
tw('open-telegram-link', 'openTelegramLink(url) for t.me links', 'in', 'D8', 'missing', { match: { apis: ['TelegramWidget.openTelegramLink'] } });
tw('haptic', 'Haptic feedback, rate-limited', 'in', 'D8', 'missing', { match: { apis: ['TelegramWidget.HapticFeedback'] } });
tw('profile', 'TelegramWidget.profile (integer) and isProfileAtLeast(n)', 'in', 'D23', 'missing', { match: { apis: ['TelegramWidget.profile', 'TelegramWidget.isProfileAtLeast'] } });
tw('send-message', 'TelegramWidget.sendMessage', 'out', 'D8', 'kept', { match: { apis: ['TelegramWidget.sendMessage'] }, neutralise: 'removed from the SDK' });

// ───────────── Input ─────────────
const inp = (id: string, title: string, st: Status, d: string, t: Today, x: Partial<Feature> = {}) => add('input', 'input.' + id, title, st, d, t, x);
inp('tap', 'Tap: click on the element under the finger, with clientX/Y and target', 'in', 'D4', 'kept', { match: { events: ['click'] } });
inp('pointer', 'pointerdown / pointerup / pointercancel with coordinates, pointerId, pointerType, isPrimary', 'in', 'D4', 'kept', { match: { events: ['pointerdown', 'pointerup', 'pointercancel'] } });
inp('mouse-compat', 'mousedown / mouseup compatibility events of a tap', 'in', 'D4', 'kept', { match: { events: ['mousedown', 'mouseup'] } });
inp('touch-events', 'touchstart / touchmove / touchend (touch clients only)', 'candidate', 'proposed', 'kept', { match: { events: ['touchstart', 'touchmove', 'touchend', 'touchcancel'] }, note: 'Desktop clients have none; widgets should use pointer events.' });
inp('drag', 'Drag: pointermove with buttons while pressed, setPointerCapture, movementX/Y', 'in', 'D4', 'kept', { match: { events: ['pointermove'], apis: ['setPointerCapture', 'releasePointerCapture'] }, note: 'v1 (not the PoC): drag with gesture arbitration.' });
inp('hover-events', 'pointerover/enter/leave/out, mouseover/enter/leave (desktop pointer only)', 'in', 'D3', 'kept', { match: { events: ['pointerover', 'pointerenter', 'pointerleave', 'pointerout', 'mouseover', 'mouseenter', 'mouseleave', 'mouseout', 'mousemove'] } });
inp('dblclick', 'dblclick', 'candidate', 'proposed', 'kept', { match: { events: ['dblclick'] }, note: 'Double tap is the bubble\'s reaction gesture (designers).' });
inp('long-press', 'contextmenu / long press', 'out', 'proposed', 'kept', { match: { events: ['contextmenu'] }, neutralise: 'the bubble menu wins; no contextmenu event' });
inp('keyboard', 'keydown / keyup / keypress, focus and blur', 'out', 'D4', 'kept', { match: { events: ['keydown', 'keyup', 'keypress', 'focus', 'blur', 'focusin', 'focusout'] }, neutralise: 'no keyboard in v1' });
inp('wheel-scroll', 'wheel and scroll events', 'out', 'D3a', 'kept', { match: { events: ['wheel', 'scroll'] }, neutralise: 'no scrolling inside a widget' });
inp('user-activation', 'Privileged calls (openLink) only in the task of a real tap; not from timers or synthetic clicks', 'in', 'D8', 'kept', { match: { apis: ['TelegramWidget.openLink'] } });
inp('prevent-default', 'preventDefault on pointer events keeps the gesture in the widget (learned by the renderer)', 'in', 'D4', 'kept', { match: { apis: ['Event.preventDefault'] } });

// ───────────── Limits ─────────────
const lim = (id: string, title: string, st: Status, d: string, t: Today, x: Partial<Feature> = {}) => add('limit', 'limit.' + id, title, st, d, t, x);
lim('text-node', 'Text values (text node, string property) up to 64 KB (strict)', 'in', 'D5', 'kept');
lim('text-budget', 'Document text budget 256 K units, complex-script text 8192 units (strict)', 'in', 'D5', 'kept');
lim('nodes', 'At most 50 000 nodes', 'in', 'today', 'kept');
lim('timers', 'At most 256 live timers (RangeError past it)', 'in', 'today', 'kept');
lim('cpu', 'CPU quota 300 ms per second (timers deferred, not killed); 500 ms per task slice', 'in', 'today', 'kept');
lim('memory', 'QuickJS heap 64 MB', 'in', 'today', 'kept');
lim('storage', 'localStorage 16 KB per message (QuotaExceededError)', 'in', 'D17', 'missing');
lim('html-size', 'HTML up to 128 KB (server-side, .tgw)', 'in', 'D20', 'host', { note: 'Checked by the server, not the client; every conformance widget stays far below it.' });

// ───────────── Client behaviour ─────────────
const b = (id: string, title: string, st: Status, d: string, t: Today, x: Partial<Feature> = {}) => add('behaviour', 'behaviour.' + id, title, st, d, t, x);
b('theme-switch', 'Live theme switch: prefers-color-scheme, light-dark() and onTheme follow the chat theme without a reload', 'in', 'D14', 'host');
b('offscreen-pause', 'Off-screen: frames stop, timers at most 1 Hz, visibilitychange fires', 'in', 'today', 'host');
b('auto-height', 'The bubble takes the widget height; above the cap it is clipped hard', 'in', 'D12a', 'host');
b('no-selection', 'No text selection in a widget (drag over text selects nothing)', 'in', 'D18', 'missing');
b('policy-strict', 'strict policy on every client (gesture gate, text caps, budgets)', 'in', 'D5', 'kept');

export const CATALOGUE: Feature[] = F;
export const CATALOGUE_VERSION = 1;

/** Decisions referenced above, for the generated table. */
export const DECISIONS: Record<string, string> = {
  D3: 'A feature is in when every client draws it; :hover is the exception (progressive enhancement)',
  D3a: 'v1: canvas 2D; Web Animations, filter, backdrop-filter if clients draw them; out: forms, inner scroll, 3D transforms, magic move; CSS animations and transitions in',
  D3b: 'Fonts: system generic families only, other names dropped',
  D4: 'Input: taps in the PoC, drag in v1, keyboard later',
  D5: 'strict policy everywhere; webAnimations and morphUpdates from the profile',
  D6: '3D (__tw3d) cut from the release',
  D8: 'SDK outward: openLink / openTelegramLink on a tap with confirmation, haptics; no sendMessage',
  D11: 'One widget model: classic DOM in the sandbox',
  D12a: 'Above the cap: hard clip, no fullscreen',
  D14: 'Theme: themeParams + --tg-theme-*, theme change event',
  D17: 'State: localStorage per (account, bot, message_id), 16 KB, QuotaExceededError; sessionStorage in memory',
  D18: 'Text selection forbidden',
  D20: '.tgw = gzip of HTML, 128 KB HTML limit on the server',
  D21: 'No accessibility in v1',
  D23: 'Runtime profile detection: TelegramWidget.profile / isProfileAtLeast, CSS.supports and @supports from the profile',
  brief: 'Named as a candidate in the conformance brief (decided by measurement)',
  inv1: 'Invariant 1: widget JS only in QuickJS',
  inv2: 'Invariant 2: the host validates everything against the allowlists',
  inv4: 'Invariant 4: only data: images, host blob: and #fragment URLs reach the renderer',
  inv5: 'Invariant 5: no network in QuickJS',
  inv6: 'Invariant 6: navigation cancelled, shadow scope',
  today: 'Current pipeline behaviour, no v1 decision recorded',
  proposed: 'Proposed by this catalogue, to be confirmed',
};
