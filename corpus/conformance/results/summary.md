# Conformance results

Generated 2026-10-16 09:29 UTC by `scripts/conformance/report.ts` from the runs of `scripts/conformance/run-web.ts` (commit c7b91a8, Chromium 153.0.8010.12 (headless, Playwright), policy strict, features webAnimations). Per-renderer reports in the common format: [web-iframe.json](web-iframe.json), [web-shadow.json](web-shadow.json), [web-blitz.json](web-blitz.json), [web-litehtml.json](web-litehtml.json).

Verdict per catalogue ID over the automatic set: ✅ pass, ◐ partly, ❌ fail, ⏳ expected to fail (not implemented yet: D23, the profile in the validator, …), ❗ passes although expected to fail, – not applicable here, · not tested. For **out** features "pass" means *neutralised as the profile says*. The native columns are a plain browser engine without the pipeline: a reference for the tests, not an expectation.

## Totals

| Renderer | in (152) ✅ / ◐ / ❌ / ⏳ | candidate (49) ✅ / ◐ / ❌ | out (40) neutralised ✅ / not yet ⏳ / ❌ | auto runs loaded | errors |
|---|---|---|---|---|---|
| web iframe | 135 / 5 / 0 / 8 | 46 / 2 / 0 | 16 / 23 / 1 | 100 / 100 | 0 |
| web shadow | 134 / 5 / 1 / 8 | 45 / 2 / 1 | 17 / 23 / 0 | 100 / 100 | 0 |
| web blitz | 88 / 26 / 23 / 7 | 18 / 8 / 21 | 14 / 12 / 4 | 22 / 22 | 0 |
| web litehtml | 53 / 39 / 46 / 6 | 6 / 7 / 34 | 15 / 11 / 3 | 22 / 22 | 0 |

## Candidates

Membership decided by measurement (D3: in when every client draws it).

| ID | Feature | web iframe | web shadow | web blitz | web litehtml | native chromium |
|---|---|---|---|---|---|---|
| `html.el.ruby` | ruby, rt, rp, bdi, bdo | ✅ | ✅ | ◐ | ❌ | ✅ |
| `html.el.legacy` | Legacy presentational elements: center, font, big, tt, strike, nobr | ✅ | ✅ | ◐ | ◐ | ✅ |
| `html.el.img-svg` | <img src="data:image/svg+xml"> | ✅ | ✅ | ◐ | ❌ | ✅ |
| `html.el.progress-meter` | progress, meter (native gauges) | ✅ | ✅ | ❌ | ✅ | ✅ |
| `html.attr.legacy-presentational` | align, valign, bgcolor, border, cellpadding, cellspacing, color, face, nowrap | ✅ | ✅ | ◐ | ◐ | ✅ |
| `svg.css` | CSS on inline SVG: fill/stroke from a stylesheet rule, :hover, CSS animation of SVG properties | ✅ | ✅ | ◐ | ❌ | ✅ |
| `svg.clip-mask` | clipPath, mask, pattern, marker | ✅ | ✅ | ❌ | ❌ | · |
| `svg.filters` | SVG filter primitives (fe*) | ✅ | ✅ | ❌ | ❌ | · |
| `svg.smil` | SMIL animation: animate, animateTransform, animateMotion | ✅ | ✅ | ❌ | ❌ | · |
| `svg.image` | <image href="data:…"> inside SVG | ✅ | ✅ | ✅ | ❌ | · |
| `css.display.grid` | CSS grid: display grid, grid-template-*, grid-area/column/row, auto-flow, place-* | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.display.other` | display: flow-root, contents, list-item | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.box.logical` | Logical properties: margin-inline, padding-block, inset-inline, inline-size, … | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.box.columns` | Multi-column layout (columns, column-count, column-gap) | ✅ | ✅ | ❌ | ❌ | ✅ |
| `css.color.modern` | Colour functions: hwb(), lab(), lch(), oklab(), oklch(), color() | ✅ | ✅ | ✅ | ❌ | · |
| `css.color.mix` | color-mix() | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.background.clip-text` | background-clip: text (gradient text) | ✅ | ✅ | ❌ | ❌ | · |
| `css.gradient.conic` | conic-gradient() | ✅ | ✅ | ❌ | ❌ | · |
| `css.font.ui-families` | font-family: ui-sans-serif, ui-serif, ui-monospace, ui-rounded | ◐ | ◐ | ◐ | ◐ | ◐ |
| `css.font.variant-numeric` | font-variant-numeric: tabular-nums (and font-feature-settings "tnum") | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.text.overflow` | text-overflow: ellipsis (single line) | ✅ | ✅ | ❌ | ❌ | · |
| `css.text.line-clamp` | -webkit-line-clamp / line-clamp (multi-line ellipsis) | ✅ | ✅ | ❌ | ❌ | ✅ |
| `css.text.shadow` | text-shadow | ✅ | ✅ | ❌ | ❌ | · |
| `css.filter.other` | filter: grayscale(), sepia(), saturate(), contrast(), hue-rotate(), invert(), opacity() | ✅ | ✅ | ❌ | ❌ | · |
| `css.blend` | mix-blend-mode, isolation, background-blend-mode | ✅ | ✅ | ❌ | ❌ | · |
| `css.clip-path` | clip-path: inset(), circle(), ellipse(), polygon() | ✅ | ✅ | ✅ | ❌ | · |
| `css.mask` | mask / -webkit-mask with gradients or data: images | ✅ | ✅ | ✅ | ❌ | · |
| `css.pointer-events` | pointer-events: none / auto (hit testing passes through) | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.touch-action` | touch-action: none / pan-x / pan-y (the widget claims a swipe from the chat) | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.value.vh` | vh / dvh / svh / vb: height-relative units | ✅ | ✅ | ✅ | ✅ | ✅ |
| `sel.has` | :has() | ✅ | ✅ | ❌ | ❌ | ✅ |
| `sel.first-letter-line` | ::first-letter, ::first-line, ::marker | ✅ | ✅ | ❌ | ❌ | ✅ |
| `at.media.reduced-motion` | @media (prefers-reduced-motion) | ✅ | ✅ | ✅ | ✅ | ✅ |
| `at.media.hover-pointer` | @media (hover: hover), (pointer: fine\|coarse) | ✅ | ✅ | ❌ | ◐ | ✅ |
| `at.media.other` | @media print, resolution, other media features | ✅ | ✅ | ✅ | ✅ | ✅ |
| `at.container` | @container queries (container-type in the widget) | ✅ | ✅ | ◐ | ◐ | ✅ |
| `at.layer` | @layer cascade layers | ✅ | ✅ | ✅ | ❌ | ✅ |
| `at.property` | @property (typed custom properties) | ✅ | ❌ | ✅ | ❌ | ✅ |
| `at.nesting` | CSS nesting (.a { .b { } }, &) | ◐ | ◐ | ◐ | ◐ | ✅ |
| `canvas.conic-gradient` | createConicGradient | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.pattern` | createPattern from a canvas or image | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.composite` | globalCompositeOperation (source-over, destination-out, multiply, …) | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.shadow` | shadowBlur, shadowColor, shadowOffsetX/Y | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.filter` | ctx.filter (CSS filter on canvas drawing) | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.image-data` | getImageData / putImageData / createImageData (pixel read-back) | ✅ | ✅ | ❌ | ❌ | ✅ |
| `js.intl.format-to-parts` | Intl.NumberFormat/DateTimeFormat formatToParts | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.libraries` | Bundled libraries by CDN name: Chart.js 4, D3 7 (nothing is fetched) | ✅ | ✅ | ✅ | ✅ | – |
| `input.touch-events` | touchstart / touchmove / touchend (touch clients only) | – | – | – | – | – |
| `input.dblclick` | dblclick | ✅ | ✅ | – | – | ✅ |

## In the profile

| ID | web iframe | web shadow | web blitz | web litehtml | native chromium |
|---|---|---|---|---|---|
| `html.el.structure` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `html.el.headings` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `html.el.sections` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `html.el.inline-text` | ✅ | ✅ | ◐ | ❌ | ✅ |
| `html.el.sub-sup` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `html.el.pre` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `html.el.blockquote` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `html.el.lists` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `html.el.table` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `html.el.img` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `html.el.canvas` | ✅ | ✅ | ❌ | ❌ | · |
| `html.el.button` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `html.el.details` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `html.el.a-fragment` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `html.el.a-external` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `html.el.unknown` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `html.attr.global` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `html.attr.dir` | ✅ | ✅ | ◐ | ❌ | ✅ |
| `html.attr.data-aria` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `html.attr.event-handlers` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `svg.shapes` | ✅ | ✅ | ◐ | ❌ | ✅ |
| `svg.structure` | ✅ | ✅ | ❌ | ❌ | ✅ |
| `svg.text` | ✅ | ✅ | ❌ | ❌ | ✅ |
| `svg.gradients` | ✅ | ✅ | ❌ | ❌ | · |
| `css.display.basic` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `css.display.flex` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.gap.flex` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.display.table` | ✅ | ✅ | ❌ | ✅ | ✅ |
| `css.box.size` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.box.spacing` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `css.box.border` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.box.radius` | ✅ | ✅ | ✅ | ✅ | · |
| `css.box.sizing` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `css.box.aspect-ratio` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.box.outline` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `css.box.shadow` | ✅ | ✅ | ✅ | ❌ | · |
| `css.box.float` | ✅ | ✅ | ❌ | ◐ | ✅ |
| `css.box.object-fit` | ✅ | ✅ | ✅ | ❌ | · |
| `css.position.relative-absolute` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.position.fixed` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `css.position.z-index` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `css.overflow.clip` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.visibility` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.opacity` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.color.basic` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.color.light-dark` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.background.basic` | ✅ | ✅ | ◐ | ❌ | ✅ |
| `css.gradient.linear` | ✅ | ✅ | ✅ | ✅ | · |
| `css.gradient.radial` | ✅ | ✅ | ❌ | ✅ | · |
| `css.font.generic` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `css.font.basic` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `css.text.align` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.text.decoration` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.text.transform` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.text.white-space` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `css.text.wrap` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.text.vertical-align` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `css.text.list-style` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.text.rtl` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.text.emoji` | ✅ | ✅ | ◐ | ✅ | ✅ |
| `css.transform.2d` | ✅ | ✅ | ◐ | ❌ | ✅ |
| `css.filter.blur-brightness-drop-shadow` * | ✅ | ✅ | ◐ | ❌ | ✅ |
| `css.backdrop-filter` * | ✅ | ✅ | ❌ | ❌ | · |
| `css.cursor` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.value.units` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.value.vw` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `css.value.math` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.value.custom-properties` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.value.keywords` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.value.content` | ✅ | ✅ | ◐ | ❌ | ✅ |
| `sel.basic` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `sel.structural` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `sel.is-where` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `sel.root` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `sel.before-after` | ✅ | ✅ | ◐ | ❌ | ✅ |
| `sel.hover` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `sel.active` | ✅ | ✅ | – | – | ✅ |
| `at.media.width` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `at.media.color-scheme` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `at.supports` | ⏳ | ⏳ | ⏳ | ◐ | ⏳ |
| `at.keyframes` | ✅ | ✅ | ✅ | ❌ | · |
| `anim.transition` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `anim.keyframes` | ✅ | ✅ | ◐ | ❌ | ✅ |
| `anim.play-state` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `anim.easing` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `anim.events` | ◐ | ◐ | ❌ | ❌ | ✅ |
| `anim.waapi` * | ◐ | ◐ | ◐ | ◐ | ✅ |
| `anim.waapi-css-control` * | ✅ | ✅ | ❌ | ❌ | ✅ |
| `canvas.rect` | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.path` | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.line-style` | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.colors` | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.gradients` | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.transform` | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.clip` | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.text` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `canvas.alpha` | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.path2d` | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.draw-image` | ◐ | ◐ | ❌ | ❌ | ✅ |
| `canvas.hit-test` | ✅ | ✅ | ❌ | ✅ | ✅ |
| `canvas.hidpi` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `js.dom.core` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `js.dom.query` | ◐ | ◐ | ◐ | ◐ | ✅ |
| `js.dom.style` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.dom.events` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.layout.rect` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `js.layout.box` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `js.layout.computed-style` | ◐ | ◐ | ◐ | ❌ | ✅ |
| `js.layout.match-media` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.layout.resize-observer` | ✅ | ✅ | ❌ | ❌ | ✅ |
| `js.layout.intersection-observer` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.layout.element-from-point` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.layout.measure-text` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `js.timers` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.raf` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.microtasks` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.storage.local` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.storage.session` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.intl` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.locale` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.crypto-random` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.builtins` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.eval` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.css-supports` | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ |
| `js.visibility` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `tw.ready` | ✅ | ✅ | ✅ | ✅ | – |
| `tw.theme` | ✅ | ✅ | ✅ | ✅ | – |
| `tw.theme-params` | ⏳ | ⏳ | ⏳ | ⏳ | – |
| `tw.open-link` | ✅ | ✅ | ✅ | ✅ | – |
| `tw.open-telegram-link` | ⏳ | ⏳ | ⏳ | ⏳ | – |
| `tw.haptic` | ⏳ | ⏳ | ⏳ | ⏳ | – |
| `tw.profile` | ⏳ | ⏳ | ⏳ | ⏳ | – |
| `input.tap` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `input.pointer` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `input.mouse-compat` | ✅ | ✅ | ❌ | ✅ | ✅ |
| `input.drag` | ✅ | ✅ | – | – | ✅ |
| `input.hover-events` | ✅ | ❌ | – | – | ✅ |
| `input.user-activation` | ✅ | ✅ | ✅ | ✅ | · |
| `input.prevent-default` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `limit.text-node` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `limit.text-budget` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `limit.nodes` | · | · | · | · | · |
| `limit.timers` | ✅ | ✅ | ✅ | ✅ | – |
| `limit.cpu` | · | · | · | · | · |
| `limit.memory` | · | · | · | · | · |
| `limit.storage` | ⏳ | ⏳ | ⏳ | ⏳ | – |
| `limit.html-size` | · | · | · | · | · |
| `behaviour.theme-switch` | ✅ | ✅ | ✅ | ✅ | – |
| `behaviour.offscreen-pause` | ✅ | ✅ | ✅ | ✅ | – |
| `behaviour.auto-height` | ✅ | ✅ | ✅ | ✅ | · |
| `behaviour.no-selection` | ⏳ | ⏳ | – | – | · |
| `behaviour.policy-strict` | ✅ | ✅ | ✅ | ◐ | ✅ |

## Out of the profile (pass = neutralised)

| ID | web iframe | web shadow | web blitz | web litehtml | native chromium |
|---|---|---|---|---|---|
| `html.el.img-other-formats` | ⏳ | ⏳ | ❌ | ❌ | ⏳ |
| `html.el.img-remote` | ✅ | ✅ | ❌ | ❌ | – |
| `html.el.forms` | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ |
| `html.el.dialog-popover` | ⏳ | ⏳ | ❗ | ⏳ | ⏳ |
| `html.el.media` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `html.el.embedding` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `html.attr.contenteditable` | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ |
| `html.attr.draggable` | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ |
| `html.attr.tabindex` | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ |
| `svg.foreign-object` | ⏳ | ⏳ | ❗ | ⏳ | · |
| `css.position.sticky` | ⏳ | ⏳ | ⏳ | ❗ | ⏳ |
| `css.overflow.scroll` | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ |
| `css.url.remote` | ✅ | ✅ | ✅ | ❌ | – |
| `css.font.named` | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ |
| `css.font.face` | ✅ | ✅ | ❌ | ✅ | ✅ |
| `css.text.writing-mode` | ⏳ | ⏳ | ❗ | ❗ | ⏳ |
| `css.text.selection` | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ |
| `css.transform.3d` | ⏳ | ⏳ | ❗ | ❗ | ⏳ |
| `css.filter.url` | ⏳ | ⏳ | ❗ | ❗ | · |
| `css.value.env` | ⏳ | ⏳ | ⏳ | ❗ | ⏳ |
| `sel.focus` | ⏳ | ⏳ | ⏳ | ❗ | ⏳ |
| `sel.form-state` | ⏳ | ⏳ | ⏳ | ⏳ | ⏳ |
| `sel.host-scoped` | ❌ | ✅ | ❌ | ✅ | ❌ |
| `sel.target` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `at.import-namespace` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `at.exotic` | ⏳ | ⏳ | ❗ | ❗ | ⏳ |
| `anim.magic-move` | ✅ | ✅ | ✅ | ✅ | – |
| `canvas.readback` | ⏳ | ⏳ | ❗ | ❗ | ⏳ |
| `canvas.webgl` | ✅ | ✅ | ✅ | ✅ | – |
| `js.network` | ✅ | ✅ | ✅ | ✅ | – |
| `js.navigation` | ✅ | ✅ | ✅ | ✅ | – |
| `js.dialogs` | ✅ | ✅ | ✅ | ✅ | – |
| `js.device` | ✅ | ✅ | ✅ | ✅ | – |
| `js.shadow-dom` | ✅ | ✅ | ✅ | ✅ | – |
| `js.blob-file` | ✅ | ✅ | ✅ | ✅ | – |
| `js.fullscreen` | ✅ | ✅ | ✅ | ✅ | – |
| `tw.send-message` | ⏳ | ⏳ | ⏳ | ⏳ | – |
| `input.long-press` | ⏳ | ⏳ | – | – | ⏳ |
| `input.keyboard` | ⏳ | ⏳ | – | – | ⏳ |
| `input.wheel-scroll` | ⏳ | ⏳ | – | – | ⏳ |

`*` in the profile if every client draws it (D3a).

## web iframe: in-profile failures

- `anim.events`: 10-css-animations: animation events carry animationName and elapsedTime ("" at 0); 10-css-animations: transitionend carries propertyName (""); 10-css-animations: animation events carry animationName and elapsedTime ("" at 0); 10-css-animations: transitionend carries propertyName ("")
- `anim.waapi`: 11-web-animations: a seek is visible to a layout query in the same task (got 20, want 45 ±0.5); 11-web-animations: a seek is visible to a layout query in the same task (got 20, want 45 ±0.5); 11-web-animations: a seek is visible to a layout query in the same task (got 20, want 45 ±0.5); 11-web-animations: a seek is visible to a layout query in the same task (got 20, want 45 ±0.5)
- `canvas.draw-image`: 12-canvas: draw-image (diff 50.4 % of ink (max 5 %)); 12-canvas: draw-image (diff 50.4 % of ink (max 5 %)); 12-canvas: draw-image (diff 50.4 % of ink (max 5 %))
- `js.dom.query`: 14-dom-layout-api: getElementsByClassName is live (0 → 0); 14-dom-layout-api: getElementsByClassName is live (0 → 0); 14-dom-layout-api: getElementsByClassName is live (0 → 0); 14-dom-layout-api: getElementsByClassName is live (0 → 0)
- `js.layout.computed-style`: 14-dom-layout-api: shorthand white-space (got "", want "nowrap"); 14-dom-layout-api: shorthand overflow (got "", want "hidden"); 14-dom-layout-api: shorthand margin (got "", want "4px 0px"); 14-dom-layout-api: shorthand white-space (got "", want "nowrap")

## web shadow: in-profile failures

- `anim.events`: 10-css-animations: animation events carry animationName and elapsedTime ("" at 0); 10-css-animations: transitionend carries propertyName (""); 10-css-animations: animation events carry animationName and elapsedTime ("" at 0); 10-css-animations: transitionend carries propertyName ("")
- `anim.waapi`: 11-web-animations: a seek is visible to a layout query in the same task (got 20, want 45 ±0.5); 11-web-animations: a seek is visible to a layout query in the same task (got 20, want 45 ±0.5); 11-web-animations: a seek is visible to a layout query in the same task (got 20, want 45 ±0.5); 11-web-animations: a seek is visible to a layout query in the same task (got 20, want 45 ±0.5)
- `canvas.draw-image`: 12-canvas: draw-image (diff 50.4 % of ink (max 5 %)); 12-canvas: draw-image (diff 50.4 % of ink (max 5 %)); 12-canvas: draw-image (diff 50.4 % of ink (max 5 %))
- `js.dom.query`: 14-dom-layout-api: getElementsByClassName is live (0 → 0); 14-dom-layout-api: getElementsByClassName is live (0 → 0); 14-dom-layout-api: getElementsByClassName is live (0 → 0); 14-dom-layout-api: getElementsByClassName is live (0 → 0)
- `js.layout.computed-style`: 14-dom-layout-api: shorthand white-space (got "", want "nowrap"); 14-dom-layout-api: shorthand overflow (got "", want "hidden"); 14-dom-layout-api: shorthand margin (got "", want "4px 0px"); 14-dom-layout-api: shorthand white-space (got "", want "nowrap")
- `input.hover-events`: 17-drag-hover: pointerenter, pointerover, mouseenter reach the widget; 17-drag-hover: pointerenter, pointerover, mouseenter reach the widget; 17-drag-hover: pointerenter, pointerover, mouseenter reach the widget; 17-drag-hover: pointerenter, pointerover, mouseenter reach the widget

## Visual set

For people (the test channel): judged by eye. The runner only checks that each widget loads without errors and records its few self-checks.

| Widget | iframe | shadow | blitz | litehtml |
|---|---|---|---|---|
| visual/01-boxes | ✅ | ✅ | ✅ | ✅ |
| visual/02-colours-gradients | ✅ | ✅ | ✅ | ✅ |
| visual/03-text | ✅ | ✅ | ✅ | ✅ |
| visual/04-layout | ✅ | ✅ | ✅ | ✅ |
| visual/05-transforms-effects | ✅ | ✅ | ✅ | ✅ |
| visual/06-css-animations | ✅ | ✅ | ✅ | ✅ |
| visual/07-web-animations | ✅ | ✅ | ✅ | ✅ |
| visual/08-canvas | ✅ | ✅ | ✅ | ✅ |
| visual/09-svg | ✅ | ✅ | ✅ | ✅ |
| visual/10-images | ✅ | ✅ | ✅ | ✅ |
| visual/11-interaction | ✅ | ✅ | ✅ | ✅ |
| visual/12-theme | ✅ | ✅ | ✅ | ✅ |
| visual/13-out-of-profile | ✅ | ✅ | ✅ | ✅ |
