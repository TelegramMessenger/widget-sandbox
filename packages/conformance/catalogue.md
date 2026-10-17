# Widget profile feature catalogue

Generated from `scripts/conformance/catalogue.ts` (`corepack pnpm tsx scripts/conformance/emit-catalogue.ts`); machine-readable: [catalogue.json](catalogue.json). 255 features: 173 in (4 conditional on every renderer drawing them), 82 out, 0 candidates. Decision `proposed` marks this catalogue's own proposals (0).

| Area | Features | in | out | candidate | covered by tests |
|---|---|---|---|---|---|
| html | 34 | 20 | 14 | 0 | 34 |
| svg | 10 | 5 | 5 | 0 | 10 |
| css | 76 | 53 | 23 | 0 | 76 |
| selector | 13 | 7 | 6 | 0 | 13 |
| at-rule | 13 | 5 | 8 | 0 | 13 |
| animation | 8 | 7 | 1 | 0 | 8 |
| canvas | 21 | 13 | 8 | 0 | 21 |
| js | 34 | 25 | 9 | 0 | 34 |
| sdk | 10 | 9 | 1 | 0 | 10 |
| input | 14 | 7 | 7 | 0 | 14 |
| limit | 11 | 11 | 0 | 0 | 7 |
| behaviour | 11 | 11 | 0 | 0 | 11 |
| **all** | 255 | 173 | 82 | 0 | 251 |

Columns: **status** (in / out / candidate; "in*" = in if every renderer draws it, D3a), **decision** (see the legend at the end), **today** (what the pipeline of the tag does with it under `strict`: profile 1 enforced, SDK v1; legend below), **tests** (conformance widgets that judge it).

## html

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `html.el.structure` | div, span, p, br, hr, wbr | in | D3a | kept |  | auto/01-html-elements-a |
| `html.el.headings` | h1–h6 with UA default sizes and margins | in | D3a | kept |  | auto/01-html-elements-a |
| `html.el.sections` | header, footer, main, nav, section, article, aside, hgroup, search, address | in | D3a | kept |  | auto/01-html-elements-a |
| `html.el.inline-text` | Inline text semantics with UA styles: b, i, u, s, em, strong, small, mark, code, kbd, samp, var, abbr, cite, q, del, ins, dfn, time, data | in | D3a | kept |  | auto/01-html-elements-a |
| `html.el.sub-sup` | sub, sup (baseline shift, smaller size) | in | D3a | kept |  | auto/01-html-elements-c |
| `html.el.pre` | pre (monospace, preserved white space and line breaks) | in | D3a | kept |  | auto/01-html-elements-b |
| `html.el.blockquote` | blockquote, figure, figcaption (UA indents) | in | D3a | kept |  | auto/01-html-elements-b |
| `html.el.lists` | ul, ol, li, dl, dt, dd, menu with markers | in | D3a | kept |  | auto/01-html-elements-b |
| `html.el.table` | table, thead, tbody, tfoot, tr, td, th, caption, colgroup, col; colspan/rowspan | in | D3a | kept |  | auto/01-html-elements-b, visual/04-layout |
| `html.el.ruby` | ruby, rt, rp, bdi, bdo<br><sub>Annotation layout is rarely implemented by small engines.</sub> | out | P1 | replaced | rendered as <span> (the annotation shows inline) | auto/01-html-elements-b |
| `html.el.legacy` | Legacy presentational elements: center, font, big, tt, strike, nobr | out | P1 | replaced | <center> becomes a <div>, the others a <span>; the presentational attributes are dropped | auto/01-html-elements-c |
| `html.el.img` | <img> with a data: PNG, JPEG, GIF or WebP; width/height attributes | in | D3a | kept |  | auto/02-attributes-images-a, auto/02-attributes-images-b, visual/10-images |
| `html.el.img-svg` | <img src="data:image/svg+xml"><br><sub>Release plan, security stream: restrict image formats to PNG, JPEG, GIF, WebP (the decoder runs in the app process).</sub> | out | P1 | replaced | url-policy refuses the MIME: placeholder image | auto/02-attributes-images-b, visual/10-images |
| `html.el.img-other-formats` | <img> with data: AVIF, BMP, ICO<br><sub>Release plan, security stream 7.</sub> | out | P1 | replaced | url-policy refuses the MIME: placeholder image | auto/02-attributes-images-b, visual/10-images |
| `html.el.img-remote` | <img src="https://…"> (any non-data: URL) | out | inv4 | replaced | placeholder image; no request | auto/02-attributes-images-b, visual/10-images |
| `html.el.canvas` | <canvas> element (2D context through the command stream) | in | D3a | kept |  | auto/12-canvas-a |
| `html.el.button` | <button> as a tappable box with UA look<br><sub>An unstyled <button> is drawn in the client's style: the profile's UA stylesheet and --tg-button-*.</sub> | in | D4 | kept |  | auto/01-html-elements-c |
| `html.el.details` | details/summary: open state and toggle on tap<br><sub>Probes: drawn by Blitz and litehtml; it is a disclosure, not a form.</sub> | in | P1 | kept |  | auto/01-html-elements-c, auto/16-sdk-taps-b, visual/11-interaction |
| `html.el.a-fragment` | <a href="#id">: no navigation, :target-free | in | P1 | kept |  | auto/01-html-elements-c, auto/16-sdk-taps-b |
| `html.el.a-external` | <a href="https://…">: href becomes "#" + data-tg-link, the tap goes to openLink (with a gesture) | in | D8 | replaced |  | auto/01-html-elements-c, auto/16-sdk-taps-a, visual/11-interaction, visual/sdk/01-link-external |
| `html.el.forms` | Form controls: form, input (all types), select, option, optgroup, textarea, label, fieldset, legend, datalist, output | out | D3a | stripped | controls are dropped with their subtree (input, select, option, optgroup, textarea, datalist, fieldset, progress, meter); <form> becomes a <div>, <label>, <legend> and <output> a <span> | auto/18-out-of-profile, visual/13-out-of-profile |
| `html.el.progress-meter` | progress, meter (native gauges)<br><sub>Not input, but a native control look that differs per platform.</sub> | out | P1 | stripped | dropped with the subtree, like the other controls | auto/01-html-elements-c |
| `html.el.dialog-popover` | dialog (show/showModal), popover attribute<br><sub>The top layer is the page: in the Shadow DOM renderer a modal dialog or popover is drawn over the whole chat, outside the bubble.</sub> | out | P1 | replaced | a dialog renders as a <div>; the popover attributes and the dialog and popover methods are dropped | auto/18-out-of-profile, visual/13-out-of-profile |
| `html.el.unknown` | Unknown / custom elements render as an inert <span data-tg-tag> | in | P1 | replaced |  | auto/01-html-elements-c |
| `html.el.media` | audio, video, source, track | out | P1 | stripped | dropped with the subtree | auto/18-out-of-profile |
| `html.el.embedding` | script (as DOM), iframe, frame, object, embed, link, meta, base, portal, template, slot, math, fencedframe, … | out | inv2 | stripped | dropped with the subtree | auto/18-out-of-profile |
| `html.attr.global` | class, id, title, hidden, lang, style attribute | in | D3a | kept |  | auto/02-attributes-images-a |
| `html.attr.dir` | dir="rtl" / dir="auto": bidi layout | in | D3a | kept |  | auto/02-attributes-images-a |
| `html.attr.data-aria` | data-* and aria-* attributes (selectors, attr()); no a11y semantics in v1<br><sub>Counted against the strict text budget.</sub> | in | D21 | kept |  | auto/02-attributes-images-a |
| `html.attr.legacy-presentational` | align, valign, bgcolor, border, cellpadding, cellspacing, color, face, nowrap | out | P1 | stripped | the attributes are dropped | auto/02-attributes-images-a |
| `html.attr.event-handlers` | Inline on* handler attributes (run inside QuickJS, never reach the renderer) | in | P1 | host |  | auto/02-attributes-images-a |
| `html.attr.contenteditable` | contenteditable, spellcheck, inputmode, enterkeyhint, autocapitalize<br><sub>Editing is form input; text selection is forbidden (D18).</sub> | out | P1 | stripped | drop the attribute | auto/02-attributes-images-a |
| `html.attr.draggable` | draggable (HTML drag and drop) | out | D4 | stripped | drop the attribute; drag in v1 is pointer events | auto/02-attributes-images-a |
| `html.attr.tabindex` | tabindex, autofocus, accesskey (keyboard focus and shortcuts) | out | D4 | stripped | drop the attributes (no keyboard: D4) | auto/02-attributes-images-a |

## svg

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `svg.shapes` | Inline <svg>: rect, circle, ellipse, line, polyline, polygon, path with presentation attributes | in | D3a | kept |  | auto/13-svg-a, visual/09-svg |
| `svg.structure` | g, defs, symbol, use (fragment href), transform attribute, viewBox/preserveAspectRatio | in | D3a | kept |  | auto/13-svg-a, visual/09-svg |
| `svg.text` | SVG text, tspan | in | D3a | kept |  | auto/13-svg-a, visual/09-svg |
| `svg.gradients` | linearGradient, radialGradient, stop; fill="url(#id)" | in | P1 | kept |  | auto/13-svg-a, visual/09-svg |
| `svg.css` | CSS on inline SVG: fill/stroke from a stylesheet rule, :hover, CSS animation of SVG properties<br><sub>Blitz and the litehtml prototype ignore SVG CSS (probe svg-css).</sub> | in | P1 | kept |  | auto/13-svg-a, visual/09-svg |
| `svg.clip-mask` | clipPath, mask, pattern, marker | out | P1 | stripped | the elements are dropped with their subtree; clip-path, mask and marker attributes are dropped | auto/13-svg-b, visual/09-svg |
| `svg.filters` | SVG filter primitives (fe*) | out | P1 | stripped | filter and the fe* elements are dropped with their subtree; the filter attribute is dropped | auto/13-svg-b, visual/09-svg |
| `svg.smil` | SMIL animation: animate, animateTransform, animateMotion<br><sub>Animation driven by the renderer, outside CSS animations and WAAPI.</sub> | out | P1 | stripped | animate, animateTransform, animateMotion and mpath are dropped | auto/13-svg-b, visual/09-svg |
| `svg.foreign-object` | foreignObject (HTML inside SVG) | out | P1 | stripped | drop the subtree | auto/13-svg-b |
| `svg.image` | <image href="data:…"> inside SVG | out | P1 | stripped | dropped | auto/13-svg-b |

## css

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `css.display.basic` | display: block, inline, inline-block, none | in | D3a | kept |  | auto/03-layout-a |
| `css.display.flex` | Flexbox: display flex/inline-flex, direction, wrap, justify-content, align-items/self/content, flex grow/shrink/basis, order | in | D3a | kept |  | auto/03-layout-a, visual/04-layout |
| `css.gap.flex` | gap / row-gap / column-gap in flex containers<br><sub>Missing in the litehtml prototype (probe flex-gap).</sub> | in | D3a | kept |  | auto/03-layout-a, visual/04-layout |
| `css.display.grid` | CSS grid: display grid, grid-template-*, grid-area/column/row, auto-flow, place-* | in | P1-grid | kept |  | auto/03-layout-a, visual/04-layout |
| `css.display.table` | display: table / table-row / table-cell | in | D3a | kept |  | auto/03-layout-c |
| `css.display.other` | display: flow-root, contents, list-item | out | P1 | stripped | the declaration is dropped | auto/03-layout-c |
| `css.box.size` | width, height, min-/max-width/height (px, %, auto, fit-content, min/max-content) | in | D3a | kept |  | auto/03-layout-b |
| `css.box.spacing` | margin, padding (incl. auto margins and negative margins) | in | D3a | kept |  | auto/03-layout-b |
| `css.box.border` | border width/style/colour (solid, dashed, dotted, double) | in | D3a | kept |  | auto/03-layout-b, visual/01-boxes |
| `css.box.radius` | border-radius, including clipping of the background | in | D3a | kept |  | auto/03-layout-c, visual/01-boxes |
| `css.box.sizing` | box-sizing: border-box / content-box | in | D3a | kept |  | auto/03-layout-b |
| `css.box.aspect-ratio` | aspect-ratio<br><sub>Missing in the litehtml prototype.</sub> | in | D3a | kept |  | auto/03-layout-b |
| `css.box.logical` | Logical properties: margin-inline, padding-block, inset-inline, inline-size, …<br><sub>Used by LLM widgets (15 % of generated); missing in the litehtml prototype.</sub> | out | P1 | stripped | the declaration is dropped | auto/03-layout-b |
| `css.box.outline` | outline (width, style, colour, offset) | in | D3a | kept |  | auto/03-layout-b, auto/05-colour-backgrounds-b, visual/01-boxes |
| `css.box.shadow` | box-shadow (outer, inset, spread, several layers) | in | D3a | kept |  | auto/05-colour-backgrounds-b, visual/01-boxes |
| `css.box.float` | float, clear | in | D3a | kept |  | auto/03-layout-b, visual/04-layout |
| `css.box.columns` | Multi-column layout (columns, column-count, column-gap) | out | P1 | stripped | the declaration is dropped | auto/03-layout-b, visual/04-layout |
| `css.box.object-fit` | object-fit, object-position on <img> | in | P1 | kept |  | auto/05-colour-backgrounds-b, visual/10-images |
| `css.position.relative-absolute` | position: relative / absolute with inset, top/right/bottom/left | in | D3a | kept |  | auto/03-layout-c, visual/04-layout |
| `css.position.fixed` | position: fixed (relative to the bubble, never the chat) | in | P1 | kept |  | auto/03-layout-c |
| `css.position.sticky` | position: sticky | out | P1 | replaced | becomes relative (no scrolling inside a widget) | auto/03-layout-c, visual/13-out-of-profile |
| `css.position.z-index` | z-index and stacking order | in | D3a | kept |  | auto/03-layout-c, visual/04-layout |
| `css.overflow.clip` | overflow: hidden / clip clips the content (with border-radius) | in | D3a | kept |  | auto/03-layout-c, visual/01-boxes |
| `css.overflow.scroll` | overflow: auto / scroll (scrolling inside the widget) | out | D3a | replaced | becomes hidden: the box clips, no scrollbar, no scroll gesture | auto/03-layout-c, visual/13-out-of-profile |
| `css.visibility` | visibility: hidden / visible | in | D3a | kept |  | auto/03-layout-c |
| `css.opacity` | opacity<br><sub>Missing in the litehtml prototype.</sub> | in | D3a | kept |  | auto/03-layout-c, auto/09-transforms-effects-b, visual/01-boxes |
| `css.color.basic` | Colours: named, #hex (3/4/6/8), rgb()/rgba(), hsl()/hsla(), transparent, currentColor | in | D3a | kept |  | auto/05-colour-backgrounds-a |
| `css.color.modern` | Colour functions: hwb(), lab(), lch(), oklab(), oklch(), color() | out | P1 | stripped | the declaration is dropped (an earlier fallback declaration applies) | auto/05-colour-backgrounds-a, visual/02-colours-gradients |
| `css.color.mix` | color-mix()<br><sub>Missing in the litehtml prototype.</sub> | out | P1 | stripped | the declaration is dropped | auto/05-colour-backgrounds-a, visual/02-colours-gradients |
| `css.color.light-dark` | light-dark() and color-scheme: light dark, resolved to the chat theme | in | P1 | resolved |  | auto/05-colour-backgrounds-a, visual/12-theme |
| `css.background.basic` | background-color, background-image: url(data:…), background-size/position/repeat, several layers | in | D3a | kept |  | auto/05-colour-backgrounds-b, visual/02-colours-gradients |
| `css.background.clip-text` | background-clip: text (gradient text) | out | P1 | stripped | the declaration is dropped | auto/05-colour-backgrounds-b |
| `css.gradient.linear` | linear-gradient(), repeating-linear-gradient() | in | D3a | kept |  | auto/05-colour-backgrounds-b, visual/02-colours-gradients |
| `css.gradient.radial` | radial-gradient(), repeating-radial-gradient() | in | D3a | kept |  | auto/05-colour-backgrounds-b, visual/02-colours-gradients |
| `css.gradient.conic` | conic-gradient() | in | P1 | kept |  | auto/05-colour-backgrounds-b, visual/02-colours-gradients |
| `css.url.remote` | url() / image-set() with anything but a data: image or #fragment | out | inv4 | stripped | the declaration is dropped | auto/05-colour-backgrounds-b |
| `css.font.generic` | font-family with system generic families: system-ui, sans-serif, serif, monospace | in | D3b | kept |  | auto/06-text-fonts-a, visual/03-text |
| `css.font.ui-families` | font-family: ui-sans-serif, ui-serif, ui-monospace, ui-rounded<br><sub>Generic per CSS Fonts 4, but only WebKit resolves them; the validator appends the generic fallback (ui-monospace → monospace).</sub> | in | P1 | kept |  | auto/06-text-fonts-a |
| `css.font.named` | font-family naming a font ("Helvetica", "Courier New", "Comic Sans MS", …) | out | D3b | stripped | the validator drops non-generic names from the list; -apple-system and BlinkMacSystemFont, the vendor names of the system UI font, become system-ui; an empty list falls back to the default (system-ui) | auto/06-text-fonts-a, visual/13-out-of-profile |
| `css.font.face` | @font-face (web fonts) | out | D3b | stripped | the at-rule is dropped | auto/06-text-fonts-a |
| `css.font.basic` | font-size, font-weight (100–900, bold), font-style italic, line-height, font shorthand | in | D3a | kept |  | auto/06-text-fonts-a, visual/03-text |
| `css.font.variant-numeric` | font-variant-numeric: tabular-nums (and font-feature-settings "tnum") | in | P1 | kept |  | auto/06-text-fonts-a |
| `css.text.align` | text-align (left, right, center, justify, start, end), text-indent | in | D3a | kept |  | auto/06-text-fonts-b, visual/03-text |
| `css.text.decoration` | text-decoration (underline, line-through, overline; colour, style, thickness) | in | D3a | kept |  | auto/06-text-fonts-b, auto/06-text-fonts-c, visual/03-text |
| `css.text.transform` | text-transform, letter-spacing, word-spacing | in | D3a | kept |  | auto/06-text-fonts-b |
| `css.text.white-space` | white-space: normal, nowrap, pre, pre-wrap, pre-line<br><sub>pre missing in the litehtml prototype.</sub> | in | D3a | kept |  | auto/06-text-fonts-b |
| `css.text.wrap` | overflow-wrap / word-break (breaking long words) | in | D3a | kept |  | auto/06-text-fonts-b |
| `css.text.overflow` | text-overflow: ellipsis (single line) | in | P1 | kept |  | auto/06-text-fonts-c, visual/03-text |
| `css.text.line-clamp` | -webkit-line-clamp / line-clamp (multi-line ellipsis) | out | P1 | stripped | the declaration is dropped | auto/06-text-fonts-b, visual/03-text |
| `css.text.shadow` | text-shadow<br><sub>Not drawn by Blitz or the litehtml prototype (probe text-shadow).</sub> | out | P1 | stripped | the declaration is dropped | auto/06-text-fonts-c, visual/03-text |
| `css.text.vertical-align` | vertical-align (baseline, middle, top, sub, super, lengths) | in | D3a | kept |  | auto/06-text-fonts-b |
| `css.text.list-style` | list-style-type / position (disc, decimal, none, …) | in | D3a | kept |  | auto/06-text-fonts-b |
| `css.text.writing-mode` | writing-mode, text-orientation (vertical text) | out | P1 | stripped | the declaration is dropped | auto/06-text-fonts-b |
| `css.text.rtl` | Right-to-left and bidi text (direction, unicode-bidi, Hebrew/Arabic shaping) | in | D3a | kept |  | auto/06-text-fonts-c, visual/03-text |
| `css.text.emoji` | Colour emoji glyphs (system emoji font)<br><sub>Blitz on iOS had blank emoji on the vello backend.</sub> | in | D3a | kept |  | auto/06-text-fonts-c, visual/03-text |
| `css.text.selection` | Text selection (user-select: text, ::selection, select by drag or long press) | out | D18 | stripped | the renderer disables selection on its root and the validator drops user-select | auto/06-text-fonts-c |
| `css.transform.2d` | 2D transforms: translate, rotate, scale, skew, matrix; transform-origin; the translate/rotate/scale properties<br><sub>Missing in the litehtml prototype.</sub> | in | D3a | kept |  | auto/09-transforms-effects-a, visual/05-transforms-effects |
| `css.transform.3d` | 3D transforms: rotateX/Y/3d, translateZ/3d, scaleZ/3d, matrix3d, perspective, transform-style, backface-visibility | out | D3a | stripped | a transform with a 3D function is dropped; perspective, transform-style and backface-visibility are dropped | auto/09-transforms-effects-a, visual/05-transforms-effects, visual/13-out-of-profile |
| `css.filter.blur-brightness-drop-shadow` | filter: blur(), brightness(), drop-shadow()<br><sub>Not drawn by Blitz or the litehtml prototype today (probe filter).</sub> | in* | D3a | kept |  | auto/09-transforms-effects-a, visual/05-transforms-effects |
| `css.filter.other` | filter: grayscale(), sepia(), saturate(), contrast(), hue-rotate(), invert(), opacity() | out | P1 | stripped | the function is removed from the filter list; an empty list drops the declaration | auto/09-transforms-effects-a, visual/05-transforms-effects |
| `css.filter.url` | filter: url(#svg-filter) | out | P1 | stripped | the declaration is dropped | auto/09-transforms-effects-a |
| `css.backdrop-filter` | backdrop-filter: blur(), brightness() | in* | D3a | kept |  | auto/09-transforms-effects-b, visual/05-transforms-effects |
| `css.blend` | mix-blend-mode, isolation, background-blend-mode | out | P1 | stripped | the declaration is dropped | auto/09-transforms-effects-b, visual/05-transforms-effects |
| `css.clip-path` | clip-path: inset(), circle(), ellipse(), polygon()<br><sub>Blitz draws it, the litehtml prototype does not.</sub> | out | P1 | stripped | the declaration is dropped | auto/09-transforms-effects-b, visual/05-transforms-effects |
| `css.mask` | mask / -webkit-mask with gradients or data: images | out | P1 | stripped | the declaration is dropped | auto/09-transforms-effects-b, visual/05-transforms-effects |
| `css.pointer-events` | pointer-events: none / auto (hit testing passes through) | in | P1 | kept |  | auto/09-transforms-effects-b, auto/17-drag-hover |
| `css.touch-action` | touch-action: none / pan-x / pan-y (the widget claims a swipe from the chat)<br><sub>D4: drag in v1 needs gesture arbitration; the Android host reads it from the renderer (`gesture` message).</sub> | in | P1 | kept |  | auto/09-transforms-effects-b |
| `css.cursor` | cursor (desktop only)<br><sub>Progressive enhancement like :hover; no-op on touch clients.</sub> | in | D3 | kept |  | auto/09-transforms-effects-b |
| `css.value.units` | Units px, em, rem, %, ch, ex; pt/cm/mm/in | in | D3a | kept |  | auto/04-values-units |
| `css.value.vw` | vw / vmin / vmax / vi: the bubble width<br><sub>Container units (cqw) on the Shadow DOM renderer.</sub> | in | P1 | replaced |  | auto/04-values-units |
| `css.value.vh` | vh / dvh / svh / vb: height-relative units<br><sub>The bubble height comes from the content, so vh is circular: the iframe uses its own height, the shadow renderer the container height.</sub> | out | P1 | stripped | the declaration is dropped | auto/04-values-units |
| `css.value.math` | calc(), min(), max(), clamp()<br><sub>Missing in the litehtml prototype.</sub> | in | D3a | kept |  | auto/04-values-units |
| `css.value.custom-properties` | Custom properties and var() with fallbacks; inheritance | in | D3a | kept |  | auto/04-values-units |
| `css.value.keywords` | inherit, initial, unset, revert; !important | in | D3a | kept |  | auto/04-values-units |
| `css.value.content` | content in ::before/::after: strings, attr(data-*), counters<br><sub>Strict: attr() reads only budgeted attributes.</sub> | in | D3a | kept |  | auto/04-values-units |
| `css.value.env` | env() (safe-area-inset-*, …) | out | P1 | stripped | the declaration is dropped (or env() resolves to its fallback) | auto/04-values-units |

## selector

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `sel.basic` | Type, class, id, universal and attribute selectors; descendant, child, + and ~ combinators; selector lists | in | D3a | kept |  | auto/07-selectors-a, auto/07-selectors-b |
| `sel.structural` | :first-child, :last-child, :nth-child(), :nth-last-child(), :nth-of-type(), :only-child, :empty, :not() | in | D3a | kept |  | auto/07-selectors-b |
| `sel.is-where` | :is(), :where() | in | D3a | kept |  | auto/07-selectors-b |
| `sel.has` | :has()<br><sub>Not matched by Blitz or the litehtml prototype (probe has-selector).</sub> | out | P1 | stripped | the selector never matches (dropped from its list) | auto/07-selectors-b |
| `sel.root` | :root (custom properties on the widget root)<br><sub>[data-tw-root] on the Shadow DOM renderer.</sub> | in | D3a | replaced |  | auto/07-selectors-b |
| `sel.before-after` | ::before, ::after | in | D3a | kept |  | auto/07-selectors-b |
| `sel.first-letter-line` | ::first-letter, ::first-line, ::marker | out | P1 | stripped | the selector never matches (dropped from its list) | auto/07-selectors-b |
| `sel.hover` | :hover (progressive enhancement: desktop with a pointer; no-op on touch) | in | D3 | kept |  | auto/07-selectors-b, auto/17-drag-hover, visual/11-interaction |
| `sel.active` | :active (pressed feedback while a tap is down)<br><sub>Used by 32 % of the HTMLBubbles demos for tap feedback.</sub> | in | P1 | kept |  | auto/16-sdk-taps-b, visual/11-interaction |
| `sel.focus` | :focus, :focus-visible, :focus-within | out | D4 | stripped | the selector never matches (dropped from its list); the widget never owns keyboard focus | auto/07-selectors-b |
| `sel.form-state` | :checked, :disabled, :enabled, :valid, :invalid, :placeholder-shown, ::placeholder | out | D3a | replaced | :disabled and :enabled become [disabled] and :not([disabled]) (the <button> state); the other form states never match | auto/18-out-of-profile |
| `sel.host-scoped` | :host, :host-context(), ::slotted(), ::part(), :scope | out | inv6 | stripped | the selector never matches (dropped from its list) on every renderer | auto/07-selectors-b |
| `sel.target` | :target | out | P1 | stripped | the selector never matches (dropped from its list) | auto/07-selectors-b |

## at-rule

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `at.media.width` | @media (min-width/max-width/orientation): the bubble width<br><sub>@container on the Shadow DOM renderer.</sub> | in | D3a | replaced |  | auto/08-at-rules |
| `at.media.color-scheme` | @media (prefers-color-scheme): the chat theme, live on theme change | in | D14 | resolved |  | auto/08-at-rules, visual/12-theme |
| `at.media.reduced-motion` | @media (prefers-reduced-motion)<br><sub>The validator answers it with the client's setting; until the client passes it, no preference.</sub> | in | P1 | resolved |  | auto/08-at-rules |
| `at.media.hover-pointer` | @media (hover: hover), (pointer: fine\|coarse)<br><sub>Lets a widget gate :hover styles; the answer must come from the client, not the engine.</sub> | out | P1 | stripped | the @media rule is dropped; matchMedia answers false | auto/08-at-rules |
| `at.media.other` | @media print, resolution, other media features | out | P1 | stripped | the @media rule is dropped; matchMedia answers false | auto/08-at-rules |
| `at.supports` | @supports evaluated from the profile tables (not the renderer engine)<br><sub>Decided by the validator from the profile tables: false for anything outside the profile.</sub> | in | D23 | resolved |  | auto/08-at-rules |
| `at.keyframes` | @keyframes (and -webkit-keyframes) | in | D3a | kept |  | auto/10-css-animations |
| `at.import-namespace` | @import, @namespace | out | inv4 | stripped | dropped | auto/08-at-rules |
| `at.container` | @container queries (container-type in the widget) | out | P1 | stripped | the at-rule is dropped; container properties are dropped | auto/08-at-rules |
| `at.layer` | @layer cascade layers | out | P1 | stripped | the at-rule is dropped with its rules | auto/08-at-rules |
| `at.property` | @property (typed custom properties) | out | P1 | stripped | the at-rule is dropped | auto/08-at-rules |
| `at.exotic` | @counter-style, @starting-style, @scope, @page, @font-feature-values, @font-palette-values, @position-try | out | P1 | stripped | dropped like unknown at-rules | auto/08-at-rules |
| `at.nesting` | CSS nesting (.a { .b { } }, &)<br><sub>Nested rules are dropped with a profile diagnostic.</sub> | out | P1 | stripped | nested rules are dropped with a css-dropped diagnostic | auto/08-at-rules |

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
| `anim.magic-move` | Magic move (shared-element morph between renders) | out | D3a | missing | not offered (it belonged to the render() model) | auto/15-js-runtime-c |

## canvas

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `canvas.rect` | fillRect, strokeRect, clearRect | in | D3a | kept |  | auto/12-canvas-a, visual/08-canvas |
| `canvas.path` | Paths: beginPath, moveTo, lineTo, arc, arcTo, bezierCurveTo, quadraticCurveTo, ellipse, rect, roundRect, closePath, fill (nonzero/evenodd), stroke | in | D3a | kept |  | auto/12-canvas-a, visual/08-canvas |
| `canvas.line-style` | lineWidth, lineCap, lineJoin, miterLimit, setLineDash, lineDashOffset | in | D3a | kept |  | auto/12-canvas-a, visual/08-canvas |
| `canvas.colors` | fillStyle / strokeStyle with CSS colours | in | D3a | kept |  | auto/12-canvas-a, visual/08-canvas |
| `canvas.gradients` | createLinearGradient, createRadialGradient, addColorStop | in | D3a | kept |  | auto/12-canvas-a, visual/08-canvas |
| `canvas.conic-gradient` | createConicGradient | out | P1 | replaced | createConicGradient returns a gradient that draws nothing | auto/12-canvas-a, visual/08-canvas |
| `canvas.pattern` | createPattern from a canvas or image | out | P1 | replaced | createPattern returns a pattern that draws nothing | auto/12-canvas-b, visual/08-canvas |
| `canvas.transform` | save, restore, translate, rotate, scale, transform, setTransform, resetTransform | in | D3a | kept |  | auto/12-canvas-b, visual/08-canvas |
| `canvas.clip` | clip() with a path | in | D3a | kept |  | auto/12-canvas-b, visual/08-canvas |
| `canvas.text` | fillText, strokeText, font, textAlign, textBaseline, measureText | in | D3a | kept |  | auto/12-canvas-a, auto/12-canvas-b, visual/08-canvas |
| `canvas.alpha` | globalAlpha | in | D3a | kept |  | auto/12-canvas-b, visual/08-canvas |
| `canvas.composite` | globalCompositeOperation (source-over, destination-out, multiply, …) | out | P1 | stripped | the property is not applied (source-over) | auto/12-canvas-b |
| `canvas.shadow` | shadowBlur, shadowColor, shadowOffsetX/Y | out | P1 | stripped | the properties are not applied | auto/12-canvas-b, visual/08-canvas |
| `canvas.filter` | ctx.filter (CSS filter on canvas drawing) | out | P1 | stripped | the property is not applied | auto/12-canvas-b, visual/08-canvas |
| `canvas.path2d` | Path2D objects (from SVG path data or built), fill/stroke/clip with them | in | D3a | kept |  | auto/12-canvas-b |
| `canvas.draw-image` | drawImage from an <img> (data:) or another canvas | in | D3a | kept |  | auto/12-canvas-b |
| `canvas.image-data` | getImageData / putImageData / createImageData (pixel read-back)<br><sub>getImageData is a sync query answered by the renderer: a native renderer must rasterize to answer.</sub> | out | P1 | stub | getImageData returns transparent pixels, putImageData draws nothing | auto/12-canvas-a, auto/12-canvas-b |
| `canvas.hit-test` | isPointInPath / isPointInStroke | in | P1 | kept |  | auto/12-canvas-a |
| `canvas.hidpi` | Backing store sized by width/height attributes, CSS size independent (devicePixelRatio scaling) | in | D3a | kept |  | auto/12-canvas-a, visual/08-canvas |
| `canvas.readback` | toDataURL, toBlob, captureStream | out | P1 | stub | toBlob/captureStream are stubs; toDataURL returns an empty image (data:,) | auto/12-canvas-a |
| `canvas.webgl` | WebGL, OffscreenCanvas, bitmaprenderer contexts | out | D6 | stub | getContext returns null; OffscreenCanvas throws | auto/12-canvas-a |

## js

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `js.dom.core` | DOM building: createElement(NS), append/insertBefore/remove, textContent, innerHTML (parsed in the sandbox), attributes, classList, dataset | in | D11 | kept |  | auto/14-dom-layout-api-a |
| `js.dom.query` | querySelector(All), getElementById, getElementsBy*, closest, matches | in | D11 | kept |  | auto/14-dom-layout-api-b |
| `js.dom.style` | element.style (CSSOM properties, setProperty, cssText), className | in | D11 | kept |  | auto/14-dom-layout-api-a |
| `js.dom.events` | addEventListener (capture and bubble), dispatchEvent of CustomEvent, preventDefault, stopPropagation | in | D11 | kept |  | auto/14-dom-layout-api-a |
| `js.layout.rect` | getBoundingClientRect, getClientRects (answered by the renderer) | in | P1 | host |  | auto/14-dom-layout-api-b |
| `js.layout.box` | offsetWidth/Height/Top/Left, clientWidth/Height, scrollWidth/Height | in | P1 | host |  | auto/14-dom-layout-api-b |
| `js.layout.computed-style` | getComputedStyle (incl. pseudo-elements and custom properties) | in | P1 | host |  | auto/14-dom-layout-api-b |
| `js.layout.match-media` | matchMedia (prefers-color-scheme answered from the chat theme) | in | P1 | host |  | auto/14-dom-layout-api-b |
| `js.layout.resize-observer` | ResizeObserver | in | P1 | host |  | auto/14-dom-layout-api-b |
| `js.layout.intersection-observer` | IntersectionObserver (emulated: always visible) | in | P1 | emulated |  | auto/14-dom-layout-api-b |
| `js.layout.element-from-point` | document.elementFromPoint | in | P1 | host |  | auto/14-dom-layout-api-b |
| `js.layout.measure-text` | Text measurement through canvas measureText (fonts of the client) | in | P1 | host |  | auto/14-dom-layout-api-b |
| `js.focus` | element.focus() / blur() / select(), document.activeElement: the widget takes keyboard focus | out | D4 | stub | focus(), blur(), select() and setSelectionRange() do nothing; no call op reaches the renderer | auto/21-no-keyboard |
| `js.timers` | setTimeout / setInterval / clear*, ordering, clamped delays | in | P1 | kept |  | auto/15-js-runtime-a |
| `js.raf` | requestAnimationFrame / cancelAnimationFrame (frames from the renderer) | in | P1 | kept |  | auto/15-js-runtime-a |
| `js.microtasks` | Promise, async/await, queueMicrotask | in | P1 | kept |  | auto/15-js-runtime-a |
| `js.storage.local` | localStorage: per (account, bot, message), preloaded, 16 KB, QuotaExceededError past it<br><sub>The shim keeps the map (16 KB per area, QuotaExceededError past it); the client preloads it and keeps each commit per message.</sub> | in | D17 | host |  | auto/15-js-runtime-a, visual/sdk/07-storage-recreate, visual/sdk/08-storage-restart |
| `js.storage.session` | sessionStorage: same key, in memory until app restart | in | D17 | host |  | auto/15-js-runtime-a, visual/sdk/07-storage-recreate, visual/sdk/08-storage-restart |
| `js.intl` | Intl.NumberFormat, DateTimeFormat, PluralRules, RelativeTimeFormat, ListFormat, Collator (host ICU) | in | P1 | host |  | auto/15-js-runtime-b |
| `js.intl.format-to-parts` | Intl.NumberFormat/DateTimeFormat formatToParts<br><sub>Numbers and dates, in every glue copy.</sub> | in | P1 | host |  | auto/15-js-runtime-b |
| `js.locale` | navigator.language reflects the client locale<br><sub>navigator.language and the Intl default come from the client's locale.</sub> | in | P1 | host |  | auto/15-js-runtime-b |
| `js.crypto-random` | crypto.getRandomValues, crypto.randomUUID, Math.random | in | P1 | host |  | auto/15-js-runtime-b |
| `js.builtins` | TextEncoder/TextDecoder, URL, URLSearchParams, structuredClone, AbortController, atob/btoa | in | P1 | kept |  | auto/15-js-runtime-b |
| `js.eval` | eval / new Function inside QuickJS (never in a browser context) | in | inv1 | kept |  | auto/15-js-runtime-b |
| `js.css-supports` | CSS.supports() answered from the profile tables<br><sub>Answers from the profile tables, like @supports.</sub> | in | D23 | resolved |  | auto/15-js-runtime-b |
| `js.visibility` | document.hidden / visibilityState / visibilitychange when the bubble leaves the screen | in | P1 | host |  | auto/15-js-runtime-b |
| `js.libraries` | Bundled libraries by CDN name: Chart.js 4, D3 7 (nothing is fetched) | out | P1-libs | stripped | nothing is bundled: a <script src> to a CDN is an external script like any other and is not run | auto/15-js-runtime-b |
| `js.network` | fetch, XMLHttpRequest, WebSocket, EventSource, RTCPeerConnection, sendBeacon, importScripts, Worker, BroadcastChannel | out | inv5 | stub | NetworkDisabledError (fetch rejects with it) | auto/15-js-runtime-c |
| `js.navigation` | location assignment, history, window.open (→ openLink), form submission | out | inv6 | stub | navigation never happens; window.open goes through openLink | auto/15-js-runtime-c |
| `js.dialogs` | alert, confirm, prompt, print | out | P1 | stub | non-blocking stubs: confirm → false, prompt → null | auto/15-js-runtime-c |
| `js.device` | clipboard, share, vibrate, geolocation, mediaDevices, Notification | out | D8 | stub | rejected promises / undefined; haptics come from the SDK | auto/15-js-runtime-c |
| `js.shadow-dom` | attachShadow, customElements.define | out | P1 | stub | attachShadow throws NotSupportedError, customElements.define is a no-op | auto/15-js-runtime-c |
| `js.blob-file` | Blob, FileReader, URL.createObjectURL | out | inv4 | stub | inert stubs | auto/15-js-runtime-c |
| `js.fullscreen` | requestFullscreen | out | D12a | stub | rejects | auto/15-js-runtime-c |

## sdk

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `tw.ready` | TelegramWidget.ready() | in | D8 | kept |  | auto/16-sdk-taps-c |
| `tw.theme` | TelegramWidget.colorScheme ("light" \| "dark"); theme and onTheme(cb) kept as deprecated aliases | in | D14 | kept |  | auto/16-sdk-taps-c, visual/12-theme, visual/sdk/14-theme-palette |
| `tw.theme-params` | themeParams (every Mini Apps key, #rrggbb) and the --tg-theme-* / --tg-color-scheme variables on the root, updated in one task with themeChanged | in | D14 | kept |  | auto/16-sdk-taps-c, auto/20-client-behaviour, visual/12-theme, visual/sdk/14-theme-palette |
| `tw.events` | onEvent / offEvent for themeChanged, activated, deactivated; at most 16 handlers per event type | in | SDK | kept |  | auto/16-sdk-taps-c, visual/sdk/14-theme-palette |
| `tw.is-active` | TelegramWidget.isActive mirrors visibility (activated / deactivated) | in | SDK | kept |  | auto/16-sdk-taps-c, auto/20-client-behaviour |
| `tw.open-link` | openLink(url): an absolute https URL of at most 256 characters, no user info, not a t.me host; one call per pointer press; the client confirms showing the full URL<br><sub>Invalid arguments throw TypeError; one call per tap reaches the client.</sub> | in | D8 | host |  | auto/16-sdk-taps-a, visual/11-interaction, visual/sdk/01-link-external, visual/sdk/03-link-rejected, visual/sdk/04-link-timer |
| `tw.open-telegram-link` | openTelegramLink(url): any https://t.me/… (telegram.me, telegram.dog) path, at most 256 characters, opened inside the client; tg: URLs refused | in | D8 | host |  | auto/16-sdk-taps-c, visual/sdk/02-link-telegram, visual/sdk/03-link-rejected |
| `tw.haptic` | HapticFeedback: impactOccurred(light\|medium\|heavy\|rigid\|soft), notificationOccurred(error\|success\|warning), selectionChanged(); no gesture needed, visible widgets only<br><sub>Desktop and web accept the calls and play nothing.</sub> | in | D8 | host |  | auto/16-sdk-taps-c, visual/sdk/05-haptics, visual/sdk/06-haptics-rate |
| `tw.profile` | TelegramWidget.profile (integer) and isProfileAtLeast(n) | in | D23 | kept |  | auto/16-sdk-taps-c |
| `tw.send-message` | TelegramWidget.sendMessage | out | D8 | missing | removed from the SDK | auto/16-sdk-taps-c |

## input

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `input.tap` | Tap: click on the element under the finger, with clientX/Y and target | in | D4 | kept |  | auto/16-sdk-taps-a, visual/11-interaction |
| `input.pointer` | pointerdown / pointerup / pointercancel with coordinates, pointerId, pointerType, isPrimary | in | D4 | kept |  | auto/16-sdk-taps-a |
| `input.mouse-compat` | mousedown / mouseup compatibility events of a tap | in | D4 | kept |  | auto/16-sdk-taps-a |
| `input.touch-events` | touchstart / touchmove / touchend (touch clients only)<br><sub>Desktop clients have none; widgets should use pointer events.</sub> | out | D4 | stripped | the renderer never forwards touch events; use pointer events | auto/17-drag-hover |
| `input.drag` | Drag: pointermove with buttons while pressed, setPointerCapture, movementX/Y<br><sub>v1 (not the PoC): drag with gesture arbitration.</sub> | in | D4 | kept |  | auto/17-drag-hover, visual/11-interaction, visual/sdk/12-drag-gestures |
| `input.hover-events` | pointerover/enter/leave/out, mouseover/enter/leave (desktop pointer only) | in | D3 | kept |  | auto/17-drag-hover |
| `input.dblclick` | dblclick / double tap | out | D4 | stripped | the renderer never forwards dblclick: a double tap belongs to the bubble (the reaction) | auto/16-sdk-taps-b, visual/sdk/12-drag-gestures |
| `input.long-press` | contextmenu / long press | out | P1 | stripped | the renderer never forwards contextmenu and cancels the platform menu: a long press belongs to the bubble (the message menu) | auto/16-sdk-taps-b, visual/sdk/12-drag-gestures |
| `input.keyboard` | Key events: keydown / keyup / keypress, beforeinput / input from keys, focus / blur / focusin / focusout from keys | out | D4 | stripped | the renderer never forwards key events; the widget never owns keyboard focus | auto/16-sdk-taps-b, auto/21-no-keyboard |
| `input.key-activation` | Enter / Space activate a focused button or link (a click from the keyboard) | out | D4 | stripped | keys do nothing in a widget: no synthetic click from Enter or Space | auto/21-no-keyboard |
| `input.key-gesture` | A key press counts as a user gesture for privileged bridge calls | out | D4 | stripped | only taps (pointer presses) grant a gesture | auto/21-no-keyboard |
| `input.wheel-scroll` | wheel and scroll events | out | D3a | stripped | the renderer never forwards wheel or scroll; no scrolling inside a widget | auto/16-sdk-taps-b |
| `input.user-activation` | Privileged calls (openLink) only in the task of a real tap; not from timers or synthetic clicks | in | D8 | kept |  | auto/16-sdk-taps-a, visual/sdk/04-link-timer |
| `input.prevent-default` | preventDefault on pointer events keeps the gesture in the widget (learned by the renderer) | in | D4 | kept |  | auto/16-sdk-taps-a |

## limit

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `limit.text-node` | Text values (text node, string property) up to 64 KB (strict) | in | D5 | kept |  | auto/19-limits |
| `limit.text-budget` | Document text budget 256 K units, complex-script text 8192 units (strict) | in | D5 | kept |  | auto/19-limits |
| `limit.nodes` | At most 50 000 nodes | in | P1 | kept |  |  |
| `limit.timers` | At most 256 live timers (RangeError past it) | in | P1 | kept |  | auto/19-limits |
| `limit.cpu` | CPU quota 300 ms per second (timers deferred, not killed); 500 ms per task slice; layout queries count toward both and may block the host for at most max(slice, 100 ms) per task | in | P1 | kept |  |  |
| `limit.memory` | QuickJS heap 64 MB | in | P1 | kept |  |  |
| `limit.storage` | localStorage and sessionStorage: 16 KB per area and message (16384 bytes of keys and values in UTF-8); over it setItem throws QuotaExceededError and stores nothing | in | D17 | kept |  | auto/15-js-runtime-a, visual/sdk/10-storage-quota |
| `limit.url-length` | openLink / openTelegramLink URLs of at most 256 characters (TypeError past it) | in | SDK | kept |  | auto/16-sdk-taps-a, visual/sdk/03-link-rejected |
| `limit.event-handlers` | At most 16 handlers per event type in onEvent (more are ignored) | in | SDK | kept |  | auto/20-client-behaviour |
| `limit.haptic-rate` | Haptics: a bucket of 5 calls refilled at 10 per second; the rest dropped (haptic-limit) | in | SDK | kept |  | auto/16-sdk-taps-c, visual/sdk/06-haptics-rate |
| `limit.html-size` | HTML up to 32 KB uncompressed and 4 KB after gzip (server-side)<br><sub>Checked by the server, not the client. Every conformance widget stays under it (build.ts keeps each one under 3800 bytes of gzip at levels 6 and 9) and reaches a client from a test bot.</sub> | in | D20 | host |  |  |

## behaviour

| ID | Feature | Status | Decision | Today | Out: neutralised as | Tests |
|---|---|---|---|---|---|---|
| `behaviour.theme-switch` | Live theme switch: prefers-color-scheme, light-dark() and onTheme follow the chat theme without a reload | in | D14 | host |  | auto/20-client-behaviour, visual/12-theme, visual/sdk/14-theme-palette |
| `behaviour.offscreen-pause` | Off-screen: frames stop, timers at most 1 Hz, visibilitychange fires | in | P1 | host |  | auto/20-client-behaviour |
| `behaviour.auto-height` | The bubble takes the widget height; above the cap it is clipped hard | in | D12a | host |  | auto/20-client-behaviour, visual/sdk/13-max-height, visual/sdk/17-size-hint |
| `behaviour.no-selection` | No text selection in a widget (drag over text selects nothing) | in | D18 | kept |  | auto/17-drag-hover |
| `behaviour.restart` | After 2 consecutive crashes the widget stays stopped on its last frame with the "Widget stopped" badge; before that the client rebuilds it when shown again<br><sub>The frozen frame and badge are the framework's; the restart count is the client's.</sub> | in | SDK | host |  | visual/sdk/11-crash-restart |
| `behaviour.gesture-claim` | Drag inside a widget that claims the axis (touch-action / preventDefault on pointerdown) does not start swipe-to-reply or scroll the list; elsewhere the chat keeps its gestures<br><sub>onGestureClaim(x, y) per primary pointerdown (v1).</sub> | in | SDK | host |  | visual/sdk/12-drag-gestures |
| `behaviour.storage-persist` | localStorage survives the widget being dropped and re-created, and an app restart; sessionStorage survives re-creation but not a restart; both are deleted with the message | in | SDK | host |  | visual/sdk/07-storage-recreate, visual/sdk/08-storage-restart, visual/sdk/09-storage-delete |
| `behaviour.storage-forward` | A forwarded copy has its own storage, keyed by its own chat and message, so it starts empty<br><sub>Testable with a test bot: forward its message into another chat; the copy starts with empty storage.</sub> | in | SDK | host |  | visual/sdk/15-storage-forward |
| `behaviour.message-edit` | A bot edit with the same HTML keeps the running widget; with new HTML the widget restarts on the same storage<br><sub>Testable with a test bot: edit the message with the same and with new HTML.</sub> | in | SDK | host |  | visual/sdk/16-bot-edit |
| `behaviour.size-hint` | Before load the bubble takes the bot's size anchors (width 0 = the bubble's width, W = a fixed width in [200, max]; height 0 = the placeholder, then the content height in [50, 600]; H = a fixed clamp(H, 50, 600)), cached per message<br><sub>Testable with a test bot sending inputMediaWidget with the anchors set; 200 / 50 / 600 are the shared v0.1.0 constants.</sub> | in | D12a | host |  | visual/sdk/17-size-hint |
| `behaviour.policy-strict` | strict policy on every client (gesture gate, text caps, budgets) | in | D5 | kept |  | auto/19-limits |

## Today

- **kept**: passes through to the renderer, or the shim provides it as specified
- **stripped**: dropped by the validator, or never forwarded by the renderer
- **replaced**: rewritten to an in-profile equivalent (an element, a value, a placeholder image, a gradient that paints nothing)
- **resolved**: answered by the pipeline from the profile, the theme or the client settings, not by the renderer engine
- **stub**: present but inert: a fixed answer, a throw, or nothing drawn
- **emulated**: implemented by the shim with its own semantics
- **host**: needs the client: the host glue answers or acts
- **missing**: not provided

## Decisions

- **D3**: A feature is in when every client draws it; :hover is the exception (progressive enhancement)
- **D3a**: v1: canvas 2D; Web Animations, filter, backdrop-filter if clients draw them; out: forms, inner scroll, 3D transforms, magic move; CSS animations and transitions in
- **D3b**: Fonts: system generic families only, other names dropped
- **D4**: Input: taps in the PoC, drag in v1; no keyboard at all, not even Enter or Space (it conflicts with the chat input field; Artem, 2026-10-16)
- **D5**: strict policy everywhere; webAnimations and morphUpdates from the profile
- **D6**: 3D (__tw3d) cut from the release
- **D8**: SDK outward: openLink / openTelegramLink on a tap with confirmation, haptics; no sendMessage
- **SDK**: SDK v1 (docs/sdk-v1.md), with Artem's answers of 2026-10-17: haptics without a gesture (bucket 5, 10 per second), any t.me path, URLs of at most 256 characters, 16 handlers per event type, stopped after 2 crashes in a row, double tap and long press stay with the bubble
- **D11**: One widget model: classic DOM in the sandbox
- **D12a**: Above the cap: hard clip, no fullscreen
- **D14**: Theme: themeParams + --tg-theme-*, theme change event
- **D17**: State: localStorage per (account, bot, message_id), 16 KB, QuotaExceededError; sessionStorage in memory
- **D18**: Text selection forbidden
- **D20**: Mini-HTML (2026-10-17): a widget is one HTML document of at most 4 KB after gzip and 32 KB uncompressed, delivered in the message; the server gzips the HTML from the bot and enforces both limits
- **D21**: No accessibility in v1
- **D23**: Runtime profile detection: TelegramWidget.profile / isProfileAtLeast, CSS.supports and @supports from the profile
- **brief**: Named as a candidate in the conformance brief (decided by measurement)
- **inv1**: Invariant 1: widget JS only in QuickJS
- **inv2**: Invariant 2: the host validates everything against the allowlists
- **inv4**: Invariant 4: only data: images, host blob: and #fragment URLs reach the renderer
- **inv5**: Invariant 5: no network in QuickJS
- **inv6**: Invariant 6: navigation cancelled, shadow scope
- **proposed**: Proposed by this catalogue, to be confirmed
- **P1**: Profile v1 freeze (2026-10-17): a candidate is in when the corpus uses it and the web renderers draw it; the rest are out until the native renderers are measured (profile 2)
- **P1-grid**: Grid is in on condition that the Swift renderer and TwidgetCore implement it before their releases (2026-10-17)
- **P1-libs**: No bundled libraries in v1 (2026-10-17)
