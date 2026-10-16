# Widget profile feature catalogue

Generated from `scripts/conformance/catalogue.ts` (`corepack pnpm tsx scripts/conformance/emit-catalogue.ts`); machine-readable: [catalogue.json](catalogue.json). 241 features: 152 in (4 conditional on every renderer drawing them), 40 out, 49 candidates. Decision `proposed` marks this catalogue's own proposals (58).

| Area | Features | in | out | candidate | covered by tests |
|---|---|---|---|---|---|
| html | 34 | 20 | 9 | 5 | 34 |
| svg | 10 | 4 | 1 | 5 | 10 |
| css | 76 | 46 | 10 | 20 | 76 |
| selector | 13 | 7 | 4 | 2 | 13 |
| at-rule | 13 | 4 | 2 | 7 | 13 |
| animation | 8 | 7 | 1 | 0 | 8 |
| canvas | 21 | 13 | 2 | 6 | 21 |
| js | 33 | 24 | 7 | 2 | 33 |
| sdk | 8 | 7 | 1 | 0 | 8 |
| input | 12 | 7 | 3 | 2 | 12 |
| limit | 8 | 8 | 0 | 0 | 4 |
| behaviour | 5 | 5 | 0 | 0 | 5 |
| **all** | 241 | 152 | 40 | 49 | 237 |

Columns: **status** (in / out / candidate; "in*" = in if every renderer draws it, D3a), **decision** (see the legend at the end), **today** (what the validator and shim do now under `strict`), **tests** (conformance widgets that judge it).

## html

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `html.el.structure` | div, span, p, br, hr, wbr | in | D3a | kept |  | auto/01-html-elements |
| `html.el.headings` | h1–h6 with UA default sizes and margins | in | D3a | kept |  | auto/01-html-elements |
| `html.el.sections` | header, footer, main, nav, section, article, aside, hgroup, search, address | in | D3a | kept |  | auto/01-html-elements |
| `html.el.inline-text` | Inline text semantics with UA styles: b, i, u, s, em, strong, small, mark, code, kbd, samp, var, abbr, cite, q, del, ins, dfn, time, data | in | D3a | kept |  | auto/01-html-elements |
| `html.el.sub-sup` | sub, sup (baseline shift, smaller size) | in | D3a | kept |  | auto/01-html-elements |
| `html.el.pre` | pre (monospace, preserved white space and line breaks) | in | D3a | kept |  | auto/01-html-elements |
| `html.el.blockquote` | blockquote, figure, figcaption (UA indents) | in | D3a | kept |  | auto/01-html-elements |
| `html.el.lists` | ul, ol, li, dl, dt, dd, menu with markers | in | D3a | kept |  | auto/01-html-elements |
| `html.el.table` | table, thead, tbody, tfoot, tr, td, th, caption, colgroup, col; colspan/rowspan | in | D3a | kept |  | auto/01-html-elements, visual/04-layout |
| `html.el.ruby` | ruby, rt, rp, bdi, bdo<br><sub>Annotation layout is rarely implemented by small engines; measure before keeping.</sub> | candidate | proposed | kept |  | auto/01-html-elements |
| `html.el.legacy` | Legacy presentational elements: center, font, big, tt, strike, nobr<br><sub>Allowed today; a profile could drop them (render as span) to keep the table small.</sub> | candidate | proposed | kept |  | auto/01-html-elements |
| `html.el.img` | <img> with a data: PNG, JPEG, GIF or WebP; width/height attributes | in | D3a | kept |  | auto/02-attributes-images, visual/10-images |
| `html.el.img-svg` | <img src="data:image/svg+xml"><br><sub>Release plan, security stream: restrict image formats to PNG, JPEG, GIF, WebP (the decoder runs in the app process).</sub> | candidate | proposed | kept |  | auto/02-attributes-images, visual/10-images |
| `html.el.img-other-formats` | <img> with data: AVIF, BMP, ICO<br><sub>Release plan, security stream 7.</sub> | out | proposed | kept | url-policy refuses the MIME: placeholder image | auto/02-attributes-images, visual/10-images |
| `html.el.img-remote` | <img src="https://…"> (any non-data: URL) | out | inv4 | replaced | placeholder image; no request | auto/02-attributes-images, visual/10-images |
| `html.el.canvas` | <canvas> element (2D context through the command stream) | in | D3a | kept |  | auto/12-canvas |
| `html.el.button` | <button> as a tappable box with UA look | in | D4 | kept |  | auto/01-html-elements |
| `html.el.details` | details/summary: open state and toggle on tap<br><sub>Probes: drawn by Blitz and litehtml; it is a disclosure, not a form.</sub> | in | proposed | kept |  | auto/01-html-elements, auto/16-sdk-taps, visual/11-interaction |
| `html.el.a-fragment` | <a href="#id">: no navigation, :target-free | in | today | kept |  | auto/01-html-elements, auto/16-sdk-taps |
| `html.el.a-external` | <a href="https://…">: href becomes "#" + data-tg-link, the tap goes to openLink (with a gesture) | in | D8 | replaced |  | auto/01-html-elements, auto/16-sdk-taps, visual/11-interaction |
| `html.el.forms` | Form controls: form, input (all types), select, option, optgroup, textarea, label, fieldset, legend, datalist, output<br><sub>Kept and drawn natively today.</sub> | out | D3a | kept | not decided: drop the control (no box) or render it as an inert span; the tests expect "no control is drawn" | auto/18-out-of-profile, visual/13-out-of-profile |
| `html.el.progress-meter` | progress, meter (native gauges)<br><sub>Not input, but a native control look that differs per platform.</sub> | candidate | proposed | kept |  | auto/01-html-elements |
| `html.el.dialog-popover` | dialog (show/showModal), popover attribute<br><sub>The top layer is the page: in the Shadow DOM renderer a modal dialog or popover is drawn over the whole chat, outside the bubble.</sub> | out | proposed | kept | drop the methods and the popover attributes; a dialog renders as a block | auto/18-out-of-profile, visual/13-out-of-profile |
| `html.el.unknown` | Unknown / custom elements render as an inert <span data-tg-tag> | in | today | replaced |  | auto/01-html-elements |
| `html.el.media` | audio, video, source, track | out | today | stripped | dropped with the subtree | auto/18-out-of-profile |
| `html.el.embedding` | script (as DOM), iframe, frame, object, embed, link, meta, base, portal, template, slot, math, fencedframe, … | out | inv2 | stripped | dropped with the subtree | auto/18-out-of-profile |
| `html.attr.global` | class, id, title, hidden, lang, style attribute | in | D3a | kept |  | auto/02-attributes-images |
| `html.attr.dir` | dir="rtl" / dir="auto": bidi layout | in | D3a | kept |  | auto/02-attributes-images |
| `html.attr.data-aria` | data-* and aria-* attributes (selectors, attr()); no a11y semantics in v1<br><sub>Counted against the strict text budget.</sub> | in | D21 | kept |  | auto/02-attributes-images |
| `html.attr.legacy-presentational` | align, valign, bgcolor, border, cellpadding, cellspacing, color, face, nowrap | candidate | proposed | kept |  | auto/02-attributes-images |
| `html.attr.event-handlers` | Inline on* handler attributes (run inside QuickJS, never reach the renderer) | in | today | host |  | auto/02-attributes-images |
| `html.attr.contenteditable` | contenteditable, spellcheck, inputmode, enterkeyhint, autocapitalize<br><sub>Editing is form input; text selection is forbidden (D18).</sub> | out | proposed | kept | drop the attribute | auto/02-attributes-images |
| `html.attr.draggable` | draggable (HTML drag and drop) | out | D4 | kept | drop the attribute; drag in v1 is pointer events | auto/02-attributes-images |
| `html.attr.tabindex` | tabindex, autofocus (keyboard focus) | out | D4 | kept | drop the attribute (keyboard later) | auto/02-attributes-images |

## svg

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `svg.shapes` | Inline <svg>: rect, circle, ellipse, line, polyline, polygon, path with presentation attributes | in | D3a | kept |  | auto/13-svg, visual/09-svg |
| `svg.structure` | g, defs, symbol, use (fragment href), transform attribute, viewBox/preserveAspectRatio | in | D3a | kept |  | auto/13-svg, visual/09-svg |
| `svg.text` | SVG text, tspan | in | D3a | kept |  | auto/13-svg, visual/09-svg |
| `svg.gradients` | linearGradient, radialGradient, stop; fill="url(#id)" | in | proposed | kept |  | auto/13-svg, visual/09-svg |
| `svg.css` | CSS on inline SVG: fill/stroke from a stylesheet rule, :hover, CSS animation of SVG properties<br><sub>Blitz and the litehtml prototype ignore SVG CSS (probe svg-css).</sub> | candidate | brief | kept |  | auto/13-svg, visual/09-svg |
| `svg.clip-mask` | clipPath, mask, pattern, marker | candidate | proposed | kept |  | auto/13-svg, visual/09-svg |
| `svg.filters` | SVG filter primitives (fe*) | candidate | proposed | kept |  | auto/13-svg, visual/09-svg |
| `svg.smil` | SMIL animation: animate, animateTransform, animateMotion<br><sub>Animation driven by the renderer, outside CSS animations and WAAPI.</sub> | candidate | proposed | kept |  | auto/13-svg, visual/09-svg |
| `svg.foreign-object` | foreignObject (HTML inside SVG) | out | proposed | kept | drop the subtree | auto/13-svg |
| `svg.image` | <image href="data:…"> inside SVG | candidate | proposed | kept |  | auto/13-svg |

## css

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `css.display.basic` | display: block, inline, inline-block, none | in | D3a | kept |  | auto/03-layout |
| `css.display.flex` | Flexbox: display flex/inline-flex, direction, wrap, justify-content, align-items/self/content, flex grow/shrink/basis, order | in | D3a | kept |  | auto/03-layout, visual/04-layout |
| `css.gap.flex` | gap / row-gap / column-gap in flex containers<br><sub>Missing in the litehtml prototype (probe flex-gap).</sub> | in | D3a | kept |  | auto/03-layout, visual/04-layout |
| `css.display.grid` | CSS grid: display grid, grid-template-*, grid-area/column/row, auto-flow, place-* | candidate | brief | kept |  | auto/03-layout, visual/04-layout |
| `css.display.table` | display: table / table-row / table-cell | in | D3a | kept |  | auto/03-layout |
| `css.display.other` | display: flow-root, contents, list-item | candidate | proposed | kept |  | auto/03-layout |
| `css.box.size` | width, height, min-/max-width/height (px, %, auto, fit-content, min/max-content) | in | D3a | kept |  | auto/03-layout |
| `css.box.spacing` | margin, padding (incl. auto margins and negative margins) | in | D3a | kept |  | auto/03-layout |
| `css.box.border` | border width/style/colour (solid, dashed, dotted, double) | in | D3a | kept |  | auto/03-layout, visual/01-boxes |
| `css.box.radius` | border-radius, including clipping of the background | in | D3a | kept |  | auto/03-layout, visual/01-boxes |
| `css.box.sizing` | box-sizing: border-box / content-box | in | D3a | kept |  | auto/03-layout |
| `css.box.aspect-ratio` | aspect-ratio<br><sub>Missing in the litehtml prototype.</sub> | in | D3a | kept |  | auto/03-layout |
| `css.box.logical` | Logical properties: margin-inline, padding-block, inset-inline, inline-size, …<br><sub>Used by LLM widgets (15 % of generated); missing in the litehtml prototype.</sub> | candidate | proposed | kept |  | auto/03-layout |
| `css.box.outline` | outline (width, style, colour, offset) | in | D3a | kept |  | auto/03-layout, auto/05-colour-backgrounds, visual/01-boxes |
| `css.box.shadow` | box-shadow (outer, inset, spread, several layers) | in | D3a | kept |  | auto/05-colour-backgrounds, visual/01-boxes |
| `css.box.float` | float, clear | in | D3a | kept |  | auto/03-layout, visual/04-layout |
| `css.box.columns` | Multi-column layout (columns, column-count, column-gap) | candidate | proposed | kept |  | auto/03-layout, visual/04-layout |
| `css.box.object-fit` | object-fit, object-position on <img> | in | proposed | kept |  | auto/05-colour-backgrounds, visual/10-images |
| `css.position.relative-absolute` | position: relative / absolute with inset, top/right/bottom/left | in | D3a | kept |  | auto/03-layout, visual/04-layout |
| `css.position.fixed` | position: fixed (relative to the bubble, never the chat) | in | proposed | kept |  | auto/03-layout |
| `css.position.sticky` | position: sticky | out | proposed | kept | becomes relative (no scrolling inside a widget) | auto/03-layout, visual/13-out-of-profile |
| `css.position.z-index` | z-index and stacking order | in | D3a | kept |  | auto/03-layout, visual/04-layout |
| `css.overflow.clip` | overflow: hidden / clip clips the content (with border-radius) | in | D3a | kept |  | auto/03-layout, visual/01-boxes |
| `css.overflow.scroll` | overflow: auto / scroll (scrolling inside the widget) | out | D3a | kept | becomes hidden: the box clips, no scrollbar, no scroll gesture | auto/03-layout, visual/13-out-of-profile |
| `css.visibility` | visibility: hidden / visible | in | D3a | kept |  | auto/03-layout |
| `css.opacity` | opacity<br><sub>Missing in the litehtml prototype.</sub> | in | D3a | kept |  | auto/03-layout, auto/09-transforms-effects, visual/01-boxes |
| `css.color.basic` | Colours: named, #hex (3/4/6/8), rgb()/rgba(), hsl()/hsla(), transparent, currentColor | in | D3a | kept |  | auto/05-colour-backgrounds |
| `css.color.modern` | Colour functions: hwb(), lab(), lch(), oklab(), oklch(), color() | candidate | proposed | kept |  | auto/05-colour-backgrounds, visual/02-colours-gradients |
| `css.color.mix` | color-mix()<br><sub>Missing in the litehtml prototype.</sub> | candidate | proposed | kept |  | auto/05-colour-backgrounds, visual/02-colours-gradients |
| `css.color.light-dark` | light-dark() and color-scheme: light dark, resolved to the chat theme | in | today | resolved |  | auto/05-colour-backgrounds, visual/12-theme |
| `css.background.basic` | background-color, background-image: url(data:…), background-size/position/repeat, several layers | in | D3a | kept |  | auto/05-colour-backgrounds, visual/02-colours-gradients |
| `css.background.clip-text` | background-clip: text (gradient text) | candidate | proposed | kept |  | auto/05-colour-backgrounds |
| `css.gradient.linear` | linear-gradient(), repeating-linear-gradient() | in | D3a | kept |  | auto/05-colour-backgrounds, visual/02-colours-gradients |
| `css.gradient.radial` | radial-gradient(), repeating-radial-gradient() | in | D3a | kept |  | auto/05-colour-backgrounds, visual/02-colours-gradients |
| `css.gradient.conic` | conic-gradient() | candidate | brief | kept |  | auto/05-colour-backgrounds, visual/02-colours-gradients |
| `css.url.remote` | url() / image-set() with anything but a data: image or #fragment | out | inv4 | stripped | the declaration is dropped | auto/05-colour-backgrounds |
| `css.font.generic` | font-family with system generic families: system-ui, sans-serif, serif, monospace | in | D3b | kept |  | auto/06-text-fonts, visual/03-text |
| `css.font.ui-families` | font-family: ui-sans-serif, ui-serif, ui-monospace, ui-rounded<br><sub>Generic per CSS Fonts 4, but only WebKit resolves them; Chromium falls back to the default face.</sub> | candidate | proposed | kept |  | auto/06-text-fonts |
| `css.font.named` | font-family naming a font ("Helvetica", "Courier New", "Comic Sans MS", …) | out | D3b | kept | the validator drops non-generic names from the list; an empty list falls back to the default (system-ui) | auto/06-text-fonts, visual/13-out-of-profile |
| `css.font.face` | @font-face (web fonts) | out | D3b | stripped | the at-rule is dropped | auto/06-text-fonts |
| `css.font.basic` | font-size, font-weight (100–900, bold), font-style italic, line-height, font shorthand | in | D3a | kept |  | auto/06-text-fonts, visual/03-text |
| `css.font.variant-numeric` | font-variant-numeric: tabular-nums (and font-feature-settings "tnum") | candidate | brief | kept |  | auto/06-text-fonts |
| `css.text.align` | text-align (left, right, center, justify, start, end), text-indent | in | D3a | kept |  | auto/06-text-fonts, visual/03-text |
| `css.text.decoration` | text-decoration (underline, line-through, overline; colour, style, thickness) | in | D3a | kept |  | auto/06-text-fonts, visual/03-text |
| `css.text.transform` | text-transform, letter-spacing, word-spacing | in | D3a | kept |  | auto/06-text-fonts |
| `css.text.white-space` | white-space: normal, nowrap, pre, pre-wrap, pre-line<br><sub>pre missing in the litehtml prototype.</sub> | in | D3a | kept |  | auto/06-text-fonts |
| `css.text.wrap` | overflow-wrap / word-break (breaking long words) | in | D3a | kept |  | auto/06-text-fonts |
| `css.text.overflow` | text-overflow: ellipsis (single line) | candidate | brief | kept |  | auto/06-text-fonts, visual/03-text |
| `css.text.line-clamp` | -webkit-line-clamp / line-clamp (multi-line ellipsis) | candidate | proposed | kept |  | auto/06-text-fonts, visual/03-text |
| `css.text.shadow` | text-shadow<br><sub>Not drawn by Blitz or the litehtml prototype (probe text-shadow).</sub> | candidate | proposed | kept |  | auto/06-text-fonts, visual/03-text |
| `css.text.vertical-align` | vertical-align (baseline, middle, top, sub, super, lengths) | in | D3a | kept |  | auto/06-text-fonts |
| `css.text.list-style` | list-style-type / position (disc, decimal, none, …) | in | D3a | kept |  | auto/06-text-fonts |
| `css.text.writing-mode` | writing-mode, text-orientation (vertical text) | out | proposed | kept | the declaration is dropped | auto/06-text-fonts |
| `css.text.rtl` | Right-to-left and bidi text (direction, unicode-bidi, Hebrew/Arabic shaping) | in | D3a | kept |  | auto/06-text-fonts, visual/03-text |
| `css.text.emoji` | Colour emoji glyphs (system emoji font)<br><sub>Blitz on iOS had blank emoji on the vello backend.</sub> | in | D3a | kept |  | auto/06-text-fonts, visual/03-text |
| `css.text.selection` | Text selection (user-select: text, ::selection, select by drag or long press) | out | D18 | kept | the client renders with selection disabled whatever the widget says | auto/06-text-fonts |
| `css.transform.2d` | 2D transforms: translate, rotate, scale, skew, matrix; transform-origin; the translate/rotate/scale properties<br><sub>Missing in the litehtml prototype.</sub> | in | D3a | kept |  | auto/09-transforms-effects, visual/05-transforms-effects |
| `css.transform.3d` | 3D transforms: rotateX/Y/3d, translateZ/3d, scaleZ/3d, matrix3d, perspective, transform-style, backface-visibility | out | D3a | kept | a transform with a 3D function is dropped; perspective, transform-style and backface-visibility are dropped | auto/09-transforms-effects, visual/05-transforms-effects, visual/13-out-of-profile |
| `css.filter.blur-brightness-drop-shadow` | filter: blur(), brightness(), drop-shadow()<br><sub>Not drawn by Blitz or the litehtml prototype today (probe filter).</sub> | in* | D3a | kept |  | auto/09-transforms-effects, visual/05-transforms-effects |
| `css.filter.other` | filter: grayscale(), sepia(), saturate(), contrast(), hue-rotate(), invert(), opacity() | candidate | proposed | kept |  | auto/09-transforms-effects, visual/05-transforms-effects |
| `css.filter.url` | filter: url(#svg-filter) | out | proposed | kept | the declaration is dropped | auto/09-transforms-effects |
| `css.backdrop-filter` | backdrop-filter: blur(), brightness() | in* | D3a | kept |  | auto/09-transforms-effects, visual/05-transforms-effects |
| `css.blend` | mix-blend-mode, isolation, background-blend-mode | candidate | proposed | kept |  | auto/09-transforms-effects, visual/05-transforms-effects |
| `css.clip-path` | clip-path: inset(), circle(), ellipse(), polygon()<br><sub>Blitz draws it, the litehtml prototype does not.</sub> | candidate | proposed | kept |  | auto/09-transforms-effects, visual/05-transforms-effects |
| `css.mask` | mask / -webkit-mask with gradients or data: images | candidate | proposed | kept |  | auto/09-transforms-effects, visual/05-transforms-effects |
| `css.pointer-events` | pointer-events: none / auto (hit testing passes through) | candidate | brief | kept |  | auto/09-transforms-effects, auto/17-drag-hover |
| `css.touch-action` | touch-action: none / pan-x / pan-y (the widget claims a swipe from the chat)<br><sub>D4: drag in v1 needs gesture arbitration; the Android host reads it from the renderer (`gesture` message).</sub> | candidate | brief | kept |  | auto/09-transforms-effects |
| `css.cursor` | cursor (desktop only)<br><sub>Progressive enhancement like :hover; no-op on touch clients.</sub> | in | D3 | kept |  | auto/09-transforms-effects |
| `css.value.units` | Units px, em, rem, %, ch, ex; pt/cm/mm/in | in | D3a | kept |  | auto/04-values-units |
| `css.value.vw` | vw / vmin / vmax / vi: the bubble width<br><sub>Container units (cqw) on the Shadow DOM renderer.</sub> | in | today | replaced |  | auto/04-values-units |
| `css.value.vh` | vh / dvh / svh / vb: height-relative units<br><sub>The bubble height comes from the content, so vh is circular: the iframe uses its own height, the shadow renderer the container height.</sub> | candidate | proposed | kept |  | auto/04-values-units |
| `css.value.math` | calc(), min(), max(), clamp()<br><sub>Missing in the litehtml prototype.</sub> | in | D3a | kept |  | auto/04-values-units |
| `css.value.custom-properties` | Custom properties and var() with fallbacks; inheritance | in | D3a | kept |  | auto/04-values-units |
| `css.value.keywords` | inherit, initial, unset, revert; !important | in | D3a | kept |  | auto/04-values-units |
| `css.value.content` | content in ::before/::after: strings, attr(data-*), counters<br><sub>Strict: attr() reads only budgeted attributes.</sub> | in | D3a | kept |  | auto/04-values-units |
| `css.value.env` | env() (safe-area-inset-*, …) | out | proposed | kept | the declaration is dropped (or env() resolves to its fallback) | auto/04-values-units |

## selector

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `sel.basic` | Type, class, id, universal and attribute selectors; descendant, child, + and ~ combinators; selector lists | in | D3a | kept |  | auto/07-selectors |
| `sel.structural` | :first-child, :last-child, :nth-child(), :nth-last-child(), :nth-of-type(), :only-child, :empty, :not() | in | D3a | kept |  | auto/07-selectors |
| `sel.is-where` | :is(), :where() | in | D3a | kept |  | auto/07-selectors |
| `sel.has` | :has()<br><sub>Not matched by Blitz or the litehtml prototype (probe has-selector).</sub> | candidate | proposed | kept |  | auto/07-selectors |
| `sel.root` | :root (custom properties on the widget root)<br><sub>[data-tw-root] on the Shadow DOM renderer.</sub> | in | D3a | replaced |  | auto/07-selectors |
| `sel.before-after` | ::before, ::after | in | D3a | kept |  | auto/07-selectors |
| `sel.first-letter-line` | ::first-letter, ::first-line, ::marker | candidate | proposed | kept |  | auto/07-selectors |
| `sel.hover` | :hover (progressive enhancement: desktop with a pointer; no-op on touch) | in | D3 | kept |  | auto/07-selectors, auto/17-drag-hover, visual/11-interaction |
| `sel.active` | :active (pressed feedback while a tap is down)<br><sub>Used by 32 % of the HTMLBubbles demos for tap feedback.</sub> | in | proposed | kept |  | auto/16-sdk-taps, visual/11-interaction |
| `sel.focus` | :focus, :focus-visible, :focus-within | out | D4 | kept | never matches (no keyboard focus in v1) | auto/07-selectors |
| `sel.form-state` | :checked, :disabled, :enabled, :valid, :invalid, :placeholder-shown, ::placeholder | out | D3a | kept | follows the forms decision | auto/18-out-of-profile |
| `sel.host-scoped` | :host, :host-context(), ::slotted(), ::part(), :scope | out | inv6 | stripped | the rule is dropped (Shadow DOM scope); in a document scope they match nothing | auto/07-selectors |
| `sel.target` | :target | out | proposed | kept | never matches (no navigation) | auto/07-selectors |

## at-rule

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `at.media.width` | @media (min-width/max-width/orientation): the bubble width<br><sub>@container on the Shadow DOM renderer.</sub> | in | D3a | replaced |  | auto/08-at-rules |
| `at.media.color-scheme` | @media (prefers-color-scheme): the chat theme, live on theme change | in | D14 | resolved |  | auto/08-at-rules, visual/12-theme |
| `at.media.reduced-motion` | @media (prefers-reduced-motion)<br><sub>Today the renderer engine answers from the OS; the profile could make the client answer.</sub> | candidate | proposed | kept |  | auto/08-at-rules |
| `at.media.hover-pointer` | @media (hover: hover), (pointer: fine|coarse)<br><sub>Lets a widget gate :hover styles; the answer must come from the client, not the engine.</sub> | candidate | proposed | kept |  | auto/08-at-rules |
| `at.media.other` | @media print, resolution, other media features | candidate | proposed | kept |  | auto/08-at-rules |
| `at.supports` | @supports evaluated from the profile tables (not the renderer engine)<br><sub>Today the engine evaluates the condition: "yes" for features the validator strips.</sub> | in | D23 | kept |  | auto/08-at-rules |
| `at.keyframes` | @keyframes (and -webkit-keyframes) | in | D3a | kept |  | auto/10-css-animations |
| `at.import-namespace` | @import, @namespace | out | inv4 | stripped | dropped | auto/08-at-rules |
| `at.container` | @container queries (container-type in the widget) | candidate | proposed | kept |  | auto/08-at-rules |
| `at.layer` | @layer cascade layers | candidate | proposed | kept |  | auto/08-at-rules |
| `at.property` | @property (typed custom properties) | candidate | proposed | kept |  | auto/08-at-rules |
| `at.exotic` | @counter-style, @starting-style, @scope, @page, @font-feature-values, @font-palette-values, @position-try | out | proposed | kept | dropped like unknown at-rules | auto/08-at-rules |
| `at.nesting` | CSS nesting (.a { .b { } }, &)<br><sub>Validator hole today: css-tree regenerates .a{} and loses the nested rules (probe nesting).</sub> | candidate | brief | stripped | flatten in the validator, or drop the nested rules | auto/08-at-rules |

## animation

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `anim.transition` | CSS transitions (property, duration, delay incl. negative, timing functions) | in | D3a | kept |  | auto/10-css-animations, visual/06-css-animations |
| `anim.keyframes` | CSS animations: animation-name/duration/delay/iteration-count/direction/fill-mode/timing-function<br><sub>Not driven by Blitz or the litehtml prototype.</sub> | in | D3a | kept |  | auto/10-css-animations, visual/06-css-animations |
| `anim.play-state` | animation-play-state: paused (and negative delays) freeze a frame | in | D3a | kept |  | auto/10-css-animations, visual/06-css-animations |
| `anim.easing` | Timing functions: ease*, linear, cubic-bezier(), steps(), linear() stops | in | D3a | kept |  | auto/10-css-animations, visual/06-css-animations |
| `anim.events` | animationstart/end/iteration and transitionend events reach the widget | in | D3a | kept |  | auto/10-css-animations |
| `anim.waapi` | Web Animations: Element.animate, getAnimations, Animation play/pause/currentTime/finished, document.timeline<br><sub>Behind the webAnimations feature (D5: profile flag, the same on every client); without it animate() is a finished stub.</sub> | in* | D3a | kept |  | auto/11-web-animations, visual/07-web-animations |
| `anim.waapi-css-control` | Web Animations control of CSS animations/transitions (pause, seek, cancel via getAnimations) | in* | D3a | kept |  | auto/11-web-animations, visual/07-web-animations |
| `anim.magic-move` | Magic move (shared-element morph between renders) | out | D3a | missing | not offered (it belonged to the render() model) | auto/15-js-runtime |

## canvas

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `canvas.rect` | fillRect, strokeRect, clearRect | in | D3a | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.path` | Paths: beginPath, moveTo, lineTo, arc, arcTo, bezierCurveTo, quadraticCurveTo, ellipse, rect, roundRect, closePath, fill (nonzero/evenodd), stroke | in | D3a | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.line-style` | lineWidth, lineCap, lineJoin, miterLimit, setLineDash, lineDashOffset | in | D3a | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.colors` | fillStyle / strokeStyle with CSS colours | in | D3a | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.gradients` | createLinearGradient, createRadialGradient, addColorStop | in | D3a | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.conic-gradient` | createConicGradient | candidate | brief | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.pattern` | createPattern from a canvas or image | candidate | proposed | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.transform` | save, restore, translate, rotate, scale, transform, setTransform, resetTransform | in | D3a | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.clip` | clip() with a path | in | D3a | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.text` | fillText, strokeText, font, textAlign, textBaseline, measureText | in | D3a | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.alpha` | globalAlpha | in | D3a | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.composite` | globalCompositeOperation (source-over, destination-out, multiply, …) | candidate | proposed | kept |  | auto/12-canvas |
| `canvas.shadow` | shadowBlur, shadowColor, shadowOffsetX/Y | candidate | proposed | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.filter` | ctx.filter (CSS filter on canvas drawing) | candidate | brief | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.path2d` | Path2D objects (from SVG path data or built), fill/stroke/clip with them | in | D3a | kept |  | auto/12-canvas |
| `canvas.draw-image` | drawImage from an <img> (data:) or another canvas | in | D3a | kept |  | auto/12-canvas |
| `canvas.image-data` | getImageData / putImageData / createImageData (pixel read-back)<br><sub>getImageData is a sync query answered by the renderer: a native renderer must rasterize to answer.</sub> | candidate | proposed | kept |  | auto/12-canvas |
| `canvas.hit-test` | isPointInPath / isPointInStroke | in | proposed | kept |  | auto/12-canvas |
| `canvas.hidpi` | Backing store sized by width/height attributes, CSS size independent (devicePixelRatio scaling) | in | D3a | kept |  | auto/12-canvas, visual/08-canvas |
| `canvas.readback` | toDataURL, toBlob, captureStream | out | proposed | stub | toBlob/captureStream are stubs; toDataURL is a renderer query today | auto/12-canvas |
| `canvas.webgl` | WebGL, OffscreenCanvas, bitmaprenderer contexts | out | D6 | stub | getContext returns null; OffscreenCanvas throws | auto/12-canvas |

## js

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `js.dom.core` | DOM building: createElement(NS), append/insertBefore/remove, textContent, innerHTML (parsed in the sandbox), attributes, classList, dataset | in | D11 | kept |  | auto/14-dom-layout-api |
| `js.dom.query` | querySelector(All), getElementById, getElementsBy*, closest, matches | in | D11 | kept |  | auto/14-dom-layout-api |
| `js.dom.style` | element.style (CSSOM properties, setProperty, cssText), className | in | D11 | kept |  | auto/14-dom-layout-api |
| `js.dom.events` | addEventListener (capture and bubble), dispatchEvent of CustomEvent, preventDefault, stopPropagation | in | D11 | kept |  | auto/14-dom-layout-api |
| `js.layout.rect` | getBoundingClientRect, getClientRects (answered by the renderer) | in | today | host |  | auto/14-dom-layout-api |
| `js.layout.box` | offsetWidth/Height/Top/Left, clientWidth/Height, scrollWidth/Height | in | today | host |  | auto/14-dom-layout-api |
| `js.layout.computed-style` | getComputedStyle (incl. pseudo-elements and custom properties) | in | today | host |  | auto/14-dom-layout-api |
| `js.layout.match-media` | matchMedia (prefers-color-scheme answered from the chat theme) | in | today | host |  | auto/14-dom-layout-api |
| `js.layout.resize-observer` | ResizeObserver | in | today | host |  | auto/14-dom-layout-api |
| `js.layout.intersection-observer` | IntersectionObserver (emulated: always visible) | in | today | emulated |  | auto/14-dom-layout-api |
| `js.layout.element-from-point` | document.elementFromPoint | in | today | host |  | auto/14-dom-layout-api |
| `js.layout.measure-text` | Text measurement through canvas measureText (fonts of the client) | in | today | host |  | auto/14-dom-layout-api |
| `js.timers` | setTimeout / setInterval / clear*, ordering, clamped delays | in | today | kept |  | auto/15-js-runtime |
| `js.raf` | requestAnimationFrame / cancelAnimationFrame (frames from the renderer) | in | today | kept |  | auto/15-js-runtime |
| `js.microtasks` | Promise, async/await, queueMicrotask | in | today | kept |  | auto/15-js-runtime |
| `js.storage.local` | localStorage: per (account, bot, message), preloaded, 16 KB, QuotaExceededError past it<br><sub>Today an in-memory map per widget instance with no quota.</sub> | in | D17 | emulated |  | auto/15-js-runtime |
| `js.storage.session` | sessionStorage: same key, in memory until app restart | in | D17 | emulated |  | auto/15-js-runtime |
| `js.intl` | Intl.NumberFormat, DateTimeFormat, PluralRules, RelativeTimeFormat, ListFormat, Collator (host ICU) | in | today | host |  | auto/15-js-runtime |
| `js.intl.format-to-parts` | Intl.NumberFormat/DateTimeFormat formatToParts<br><sub>Open item: missing on iOS and Android glue.</sub> | candidate | proposed | missing |  | auto/15-js-runtime |
| `js.locale` | navigator.language reflects the client locale<br><sub>Hard-coded en-US today; the release plan wants the real locale.</sub> | in | proposed | stub |  | auto/15-js-runtime |
| `js.crypto-random` | crypto.getRandomValues, crypto.randomUUID, Math.random | in | today | host |  | auto/15-js-runtime |
| `js.builtins` | TextEncoder/TextDecoder, URL, URLSearchParams, structuredClone, AbortController, atob/btoa | in | today | kept |  | auto/15-js-runtime |
| `js.eval` | eval / new Function inside QuickJS (never in a browser context) | in | inv1 | kept |  | auto/15-js-runtime |
| `js.css-supports` | CSS.supports() answered from the profile tables<br><sub>Today always true.</sub> | in | D23 | stub |  | auto/15-js-runtime |
| `js.visibility` | document.hidden / visibilityState / visibilitychange when the bubble leaves the screen | in | today | host |  | auto/15-js-runtime |
| `js.libraries` | Bundled libraries by CDN name: Chart.js 4, D3 7 (nothing is fetched)<br><sub>The release plan asks for the v1 list.</sub> | candidate | proposed | host |  | auto/15-js-runtime |
| `js.network` | fetch, XMLHttpRequest, WebSocket, EventSource, RTCPeerConnection, sendBeacon, importScripts, Worker, BroadcastChannel | out | inv5 | stub | NetworkDisabledError (fetch rejects with it) | auto/15-js-runtime |
| `js.navigation` | location assignment, history, window.open (→ openLink), form submission | out | inv6 | stub | navigation never happens; window.open goes through openLink | auto/15-js-runtime |
| `js.dialogs` | alert, confirm, prompt, print | out | today | stub | non-blocking stubs: confirm → false, prompt → null | auto/15-js-runtime |
| `js.device` | clipboard, share, vibrate, geolocation, mediaDevices, Notification | out | D8 | stub | rejected promises / undefined; haptics come from the SDK | auto/15-js-runtime |
| `js.shadow-dom` | attachShadow, customElements.define | out | today | stub | attachShadow throws NotSupportedError, customElements.define is a no-op | auto/15-js-runtime |
| `js.blob-file` | Blob, FileReader, URL.createObjectURL | out | inv4 | stub | inert stubs | auto/15-js-runtime |
| `js.fullscreen` | requestFullscreen | out | D12a | stub | rejects | auto/15-js-runtime |

## sdk

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `tw.ready` | TelegramWidget.ready() | in | D8 | kept |  | auto/16-sdk-taps |
| `tw.theme` | TelegramWidget.theme and onTheme(cb): "light" | "dark", called on change | in | D14 | kept |  | auto/16-sdk-taps, visual/12-theme |
| `tw.theme-params` | themeParams palette and --tg-theme-* CSS variables, theme change event | in | D14 | missing |  | auto/16-sdk-taps, visual/12-theme |
| `tw.open-link` | openLink(url): https only, in the task of a real tap, confirmed by the client with the full URL | in | D8 | kept |  | auto/16-sdk-taps, visual/11-interaction |
| `tw.open-telegram-link` | openTelegramLink(url) for t.me links | in | D8 | missing |  | auto/16-sdk-taps |
| `tw.haptic` | Haptic feedback, rate-limited | in | D8 | missing |  | auto/16-sdk-taps |
| `tw.profile` | TelegramWidget.profile (integer) and isProfileAtLeast(n) | in | D23 | missing |  | auto/16-sdk-taps |
| `tw.send-message` | TelegramWidget.sendMessage | out | D8 | kept | removed from the SDK | auto/16-sdk-taps |

## input

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `input.tap` | Tap: click on the element under the finger, with clientX/Y and target | in | D4 | kept |  | auto/16-sdk-taps, visual/11-interaction |
| `input.pointer` | pointerdown / pointerup / pointercancel with coordinates, pointerId, pointerType, isPrimary | in | D4 | kept |  | auto/16-sdk-taps |
| `input.mouse-compat` | mousedown / mouseup compatibility events of a tap | in | D4 | kept |  | auto/16-sdk-taps |
| `input.touch-events` | touchstart / touchmove / touchend (touch clients only)<br><sub>Desktop clients have none; widgets should use pointer events.</sub> | candidate | proposed | kept |  | auto/17-drag-hover |
| `input.drag` | Drag: pointermove with buttons while pressed, setPointerCapture, movementX/Y<br><sub>v1 (not the PoC): drag with gesture arbitration.</sub> | in | D4 | kept |  | auto/17-drag-hover, visual/11-interaction |
| `input.hover-events` | pointerover/enter/leave/out, mouseover/enter/leave (desktop pointer only) | in | D3 | kept |  | auto/17-drag-hover |
| `input.dblclick` | dblclick<br><sub>Double tap is the bubble's reaction gesture (designers).</sub> | candidate | proposed | kept |  | auto/16-sdk-taps |
| `input.long-press` | contextmenu / long press | out | proposed | kept | the bubble menu wins; no contextmenu event | auto/16-sdk-taps |
| `input.keyboard` | keydown / keyup / keypress, focus and blur | out | D4 | kept | no keyboard in v1 | auto/16-sdk-taps |
| `input.wheel-scroll` | wheel and scroll events | out | D3a | kept | no scrolling inside a widget | auto/16-sdk-taps |
| `input.user-activation` | Privileged calls (openLink) only in the task of a real tap; not from timers or synthetic clicks | in | D8 | kept |  | auto/16-sdk-taps |
| `input.prevent-default` | preventDefault on pointer events keeps the gesture in the widget (learned by the renderer) | in | D4 | kept |  | auto/16-sdk-taps |

## limit

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `limit.text-node` | Text values (text node, string property) up to 64 KB (strict) | in | D5 | kept |  | auto/19-limits |
| `limit.text-budget` | Document text budget 256 K units, complex-script text 8192 units (strict) | in | D5 | kept |  | auto/19-limits |
| `limit.nodes` | At most 50 000 nodes | in | today | kept |  |  |
| `limit.timers` | At most 256 live timers (RangeError past it) | in | today | kept |  | auto/19-limits |
| `limit.cpu` | CPU quota 300 ms per second (timers deferred, not killed); 500 ms per task slice | in | today | kept |  |  |
| `limit.memory` | QuickJS heap 64 MB | in | today | kept |  |  |
| `limit.storage` | localStorage 16 KB per message (QuotaExceededError) | in | D17 | missing |  | auto/15-js-runtime |
| `limit.html-size` | HTML up to 128 KB (server-side, .tgw)<br><sub>Checked by the server, not the client; every conformance widget stays far below it.</sub> | in | D20 | host |  |  |

## behaviour

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `behaviour.theme-switch` | Live theme switch: prefers-color-scheme, light-dark() and onTheme follow the chat theme without a reload | in | D14 | host |  | auto/20-client-behaviour, visual/12-theme |
| `behaviour.offscreen-pause` | Off-screen: frames stop, timers at most 1 Hz, visibilitychange fires | in | today | host |  | auto/20-client-behaviour |
| `behaviour.auto-height` | The bubble takes the widget height; above the cap it is clipped hard | in | D12a | host |  | auto/20-client-behaviour |
| `behaviour.no-selection` | No text selection in a widget (drag over text selects nothing) | in | D18 | missing |  | auto/17-drag-hover |
| `behaviour.policy-strict` | strict policy on every client (gesture gate, text caps, budgets) | in | D5 | kept |  | auto/19-limits |

## Decisions

- **D3**: A feature is in when every client draws it; :hover is the exception (progressive enhancement)
- **D3a**: v1: canvas 2D; Web Animations, filter, backdrop-filter if clients draw them; out: forms, inner scroll, 3D transforms, magic move; CSS animations and transitions in
- **D3b**: Fonts: system generic families only, other names dropped
- **D4**: Input: taps in the PoC, drag in v1, keyboard later
- **D5**: strict policy everywhere; webAnimations and morphUpdates from the profile
- **D6**: 3D (__tw3d) cut from the release
- **D8**: SDK outward: openLink / openTelegramLink on a tap with confirmation, haptics; no sendMessage
- **D11**: One widget model: classic DOM in the sandbox
- **D12a**: Above the cap: hard clip, no fullscreen
- **D14**: Theme: themeParams + --tg-theme-*, theme change event
- **D17**: State: localStorage per (account, bot, message_id), 16 KB, QuotaExceededError; sessionStorage in memory
- **D18**: Text selection forbidden
- **D20**: .tgw = gzip of HTML, 128 KB HTML limit on the server
- **D21**: No accessibility in v1
- **D23**: Runtime profile detection: TelegramWidget.profile / isProfileAtLeast, CSS.supports and @supports from the profile
- **brief**: Named as a candidate in the conformance brief (decided by measurement)
- **inv1**: Invariant 1: widget JS only in QuickJS
- **inv2**: Invariant 2: the host validates everything against the allowlists
- **inv4**: Invariant 4: only data: images, host blob: and #fragment URLs reach the renderer
- **inv5**: Invariant 5: no network in QuickJS
- **inv6**: Invariant 6: navigation cancelled, shadow scope
- **today**: Current pipeline behaviour, no v1 decision recorded
- **proposed**: Proposed by this catalogue, to be confirmed
