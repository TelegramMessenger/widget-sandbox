# Conformance results

Generated 2026-10-17 09:22 UTC by `packages/conformance/tools/report.mjs` (profile 1; web iframe — Chromium 153.0.8010.12 (headless, Playwright) @ 3342023; web shadow — Chromium 153.0.8010.12 (headless, Playwright) @ 3342023; web blitz — Chromium 153.0.8010.12 (headless, Playwright) @ 3342023; web litehtml — Chromium 153.0.8010.12 (headless, Playwright) @ 3342023; policy strict, features webAnimations).

Verdict per catalogue ID over the automatic set: ✅ pass, ◐ partly, ❌ fail, ⏳ expected to fail (not implemented yet: D23, the profile in the validator, …), ❗ passes although expected to fail, – not applicable here, · not tested. For **out** features "pass" means *neutralised as the profile says*. The native columns are a plain browser engine without the pipeline: a reference for the tests, not an expectation.

## Totals

| Renderer | in (173) ✅ / ◐ / ❌ / ⏳ | out (82) neutralised ✅ / not yet ⏳ / ❌ | auto runs loaded | errors |
|---|---|---|---|---|
| web iframe | 158 / 5 / 0 / 0 | 81 / 0 / 0 | 105 / 105 | 0 |
| web shadow | 158 / 5 / 0 / 0 | 81 / 0 / 0 | 105 / 105 | 0 |
| web blitz | 105 / 29 / 25 / 0 | 66 / 2 / 7 | 23 / 23 | 0 |
| web litehtml | 66 / 41 / 52 / 0 | 60 / 3 / 12 | 23 / 23 | 0 |

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
| `svg.css` | ✅ | ✅ | ◐ | ❌ | ✅ |
| `css.display.basic` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `css.display.flex` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.gap.flex` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.display.grid` | ✅ | ✅ | ✅ | ❌ | ✅ |
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
| `css.gradient.conic` | ✅ | ✅ | ❌ | ❌ | · |
| `css.font.generic` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `css.font.ui-families` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `css.font.basic` | ✅ | ✅ | ◐ | ◐ | ✅ |
| `css.font.variant-numeric` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.text.align` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.text.decoration` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.text.transform` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.text.white-space` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `css.text.wrap` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.text.overflow` | ✅ | ✅ | ❌ | ❌ | · |
| `css.text.vertical-align` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `css.text.list-style` | ✅ | ✅ | ✅ | ❌ | ✅ |
| `css.text.rtl` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.text.emoji` | ✅ | ✅ | ◐ | ✅ | ✅ |
| `css.transform.2d` | ✅ | ✅ | ◐ | ❌ | ✅ |
| `css.filter.blur-brightness-drop-shadow` * | ✅ | ✅ | ◐ | ❌ | ✅ |
| `css.backdrop-filter` * | ✅ | ✅ | ❌ | ❌ | · |
| `css.pointer-events` | ✅ | ✅ | ✅ | ◐ | ✅ |
| `css.touch-action` | ✅ | ✅ | ✅ | ❌ | ✅ |
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
| `at.media.reduced-motion` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `at.supports` | ✅ | ✅ | ✅ | ✅ | ✅ |
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
| `js.intl.format-to-parts` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.locale` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.crypto-random` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.builtins` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.eval` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.css-supports` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `js.visibility` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `tw.ready` | ✅ | ✅ | ✅ | ✅ | – |
| `tw.theme` | ✅ | ✅ | ✅ | ✅ | – |
| `tw.theme-params` | ✅ | ✅ | ◐ | ◐ | – |
| `tw.events` | ✅ | ✅ | ✅ | ✅ | – |
| `tw.is-active` | ✅ | ✅ | ✅ | ✅ | – |
| `tw.open-link` | ✅ | ✅ | ✅ | ✅ | – |
| `tw.open-telegram-link` | ✅ | ✅ | ✅ | ✅ | – |
| `tw.haptic` | ✅ | ✅ | ✅ | ✅ | – |
| `tw.profile` | ✅ | ✅ | ✅ | ✅ | – |
| `input.tap` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `input.pointer` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `input.mouse-compat` | ✅ | ✅ | ❌ | ✅ | ✅ |
| `input.drag` | ✅ | ✅ | – | – | ✅ |
| `input.hover-events` | ✅ | ✅ | – | – | ✅ |
| `input.user-activation` | ✅ | ✅ | ✅ | ✅ | · |
| `input.prevent-default` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `limit.text-node` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `limit.text-budget` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `limit.nodes` | · | · | · | · | · |
| `limit.timers` | ✅ | ✅ | ✅ | ✅ | – |
| `limit.cpu` | · | · | · | · | · |
| `limit.memory` | · | · | · | · | · |
| `limit.storage` | ✅ | ✅ | ✅ | ✅ | – |
| `limit.url-length` | ✅ | ✅ | ✅ | ✅ | – |
| `limit.event-handlers` | ✅ | ✅ | ✅ | ✅ | – |
| `limit.haptic-rate` | ✅ | ✅ | ✅ | ✅ | · |
| `limit.html-size` | · | · | · | · | · |
| `behaviour.theme-switch` | ✅ | ✅ | ✅ | ✅ | – |
| `behaviour.offscreen-pause` | ✅ | ✅ | ✅ | ✅ | – |
| `behaviour.auto-height` | ✅ | ✅ | ✅ | ✅ | · |
| `behaviour.no-selection` | ✅ | ✅ | – | – | · |
| `behaviour.restart` | · | · | · | · | · |
| `behaviour.gesture-claim` | · | · | · | · | · |
| `behaviour.storage-persist` | · | · | · | · | · |
| `behaviour.storage-forward` | · | · | · | · | · |
| `behaviour.message-edit` | · | · | · | · | · |
| `behaviour.size-hint` | · | · | · | · | · |
| `behaviour.policy-strict` | ✅ | ✅ | ✅ | ◐ | ✅ |

## Out of the profile (pass = neutralised)

| ID | web iframe | web shadow | web blitz | web litehtml | native chromium |
|---|---|---|---|---|---|
| `html.el.ruby` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `html.el.legacy` | ✅ | ✅ | ✅ | ◐ | ❌ |
| `html.el.img-svg` | ✅ | ✅ | ◐ | ❌ | ❌ |
| `html.el.img-other-formats` | ✅ | ✅ | ❌ | ❌ | ❌ |
| `html.el.img-remote` | ✅ | ✅ | ❌ | ❌ | – |
| `html.el.forms` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `html.el.progress-meter` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `html.el.dialog-popover` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `html.el.media` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `html.el.embedding` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `html.attr.legacy-presentational` | ✅ | ✅ | ✅ | ◐ | ❌ |
| `html.attr.contenteditable` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `html.attr.draggable` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `html.attr.tabindex` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `svg.clip-mask` | ✅ | ✅ | ✅ | ❌ | · |
| `svg.filters` | ✅ | ✅ | ✅ | ❌ | · |
| `svg.smil` | ✅ | ✅ | ✅ | ❌ | · |
| `svg.foreign-object` | ✅ | ✅ | ✅ | ✅ | · |
| `svg.image` | ✅ | ✅ | ✅ | ✅ | · |
| `css.display.other` | ✅ | ✅ | ◐ | ✅ | ❌ |
| `css.box.logical` | ✅ | ✅ | ✅ | ◐ | ❌ |
| `css.box.columns` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `css.position.sticky` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `css.overflow.scroll` | ✅ | ✅ | ✅ | ❌ | ❌ |
| `css.color.modern` | ✅ | ✅ | ✅ | ✅ | · |
| `css.color.mix` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `css.background.clip-text` | ✅ | ✅ | ✅ | ✅ | · |
| `css.url.remote` | ✅ | ✅ | ✅ | ❌ | – |
| `css.font.named` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `css.font.face` | ✅ | ✅ | ❌ | ✅ | ✅ |
| `css.text.line-clamp` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `css.text.shadow` | ✅ | ✅ | ✅ | ✅ | · |
| `css.text.writing-mode` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `css.text.selection` | ✅ | ✅ | ❌ | ❌ | ✅ |
| `css.transform.3d` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `css.filter.other` | ✅ | ✅ | ✅ | ✅ | · |
| `css.filter.url` | ✅ | ✅ | ✅ | ✅ | · |
| `css.blend` | ✅ | ✅ | ✅ | ✅ | · |
| `css.clip-path` | ✅ | ✅ | ✅ | ✅ | · |
| `css.mask` | ✅ | ✅ | ✅ | ✅ | · |
| `css.value.vh` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `css.value.env` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `sel.has` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `sel.first-letter-line` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `sel.focus` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `sel.form-state` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `sel.host-scoped` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `sel.target` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `at.media.hover-pointer` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `at.media.other` | ✅ | ✅ | ✅ | ✅ | ✅ |
| `at.import-namespace` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `at.container` | ✅ | ✅ | ✅ | ✅ | ◐ |
| `at.layer` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `at.property` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `at.exotic` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `at.nesting` | ✅ | ✅ | ✅ | ✅ | ◐ |
| `anim.magic-move` | ✅ | ✅ | ✅ | ✅ | – |
| `canvas.conic-gradient` | ✅ | ✅ | ✅ | ✅ | · |
| `canvas.pattern` | ✅ | ✅ | ✅ | ✅ | · |
| `canvas.composite` | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.shadow` | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.filter` | ✅ | ✅ | ❌ | ❌ | · |
| `canvas.image-data` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `canvas.readback` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `canvas.webgl` | ✅ | ✅ | ✅ | ✅ | – |
| `js.focus` | ✅ | ✅ | ✅ | ✅ | ❌ |
| `js.libraries` | ✅ | ✅ | ✅ | ✅ | – |
| `js.network` | ✅ | ✅ | ✅ | ✅ | – |
| `js.navigation` | ✅ | ✅ | ✅ | ✅ | – |
| `js.dialogs` | ✅ | ✅ | ✅ | ✅ | – |
| `js.device` | ✅ | ✅ | ✅ | ✅ | – |
| `js.shadow-dom` | ✅ | ✅ | ✅ | ✅ | – |
| `js.blob-file` | ✅ | ✅ | ✅ | ✅ | – |
| `js.fullscreen` | ✅ | ✅ | ✅ | ✅ | – |
| `tw.send-message` | ✅ | ✅ | ✅ | ✅ | – |
| `input.touch-events` | – | – | – | – | – |
| `input.dblclick` | ✅ | ✅ | – | – | ❌ |
| `input.long-press` | ✅ | ✅ | – | – | ❌ |
| `input.keyboard` | ✅ | ✅ | – | – | ❌ |
| `input.key-activation` | ✅ | ✅ | – | – | ❌ |
| `input.key-gesture` | ✅ | ✅ | – | – | · |
| `input.wheel-scroll` | ✅ | ✅ | – | – | ❌ |

`*` in the profile if every client draws it (D3a).

## web iframe: in-profile failures

- `anim.events`: 10-css-animations: animation events carry animationName and elapsedTime ("" at 0); 10-css-animations: transitionend carries propertyName ("")
- `anim.waapi`: 11-web-animations: a seek is visible to a layout query in the same task (got 20, want 45 ±0.5)
- `canvas.draw-image`: 12-canvas: draw-image (diff 50.4 % of ink (max 5 %))
- `js.dom.query`: 14-dom-layout-api: getElementsByClassName is live (0 → 0)
- `js.layout.computed-style`: 14-dom-layout-api: shorthand white-space (got "", want "nowrap"); 14-dom-layout-api: shorthand overflow (got "", want "hidden"); 14-dom-layout-api: shorthand margin (got "", want "4px 0px")

## web shadow: in-profile failures

- `anim.events`: 10-css-animations: animation events carry animationName and elapsedTime ("" at 0); 10-css-animations: transitionend carries propertyName ("")
- `anim.waapi`: 11-web-animations: a seek is visible to a layout query in the same task (got 20, want 45 ±0.5)
- `canvas.draw-image`: 12-canvas: draw-image (diff 50.4 % of ink (max 5 %))
- `js.dom.query`: 14-dom-layout-api: getElementsByClassName is live (0 → 0)
- `js.layout.computed-style`: 14-dom-layout-api: shorthand white-space (got "", want "nowrap"); 14-dom-layout-api: shorthand overflow (got "", want "hidden"); 14-dom-layout-api: shorthand margin (got "", want "4px 0px")

## web blitz: in-profile failures

- `html.el.structure`: 01-html-elements: wbr is a break opportunity (h 16)
- `html.el.inline-text`: 01-html-elements: q adds quotation marks (16.5 vs 16.5)
- `html.el.sub-sup`: 01-html-elements: sub sits lower, sup higher (base 420-443 sub 443 sup 420)
- `html.el.table`: 01-html-elements: rowspan=2 spans two rows (12 vs 12)
- `html.el.img`: 02-attributes-images: PNG decodes (naturalWidth 2) (timed out); 02-attributes-images: GIF decodes (naturalWidth 1) (timed out); 02-attributes-images: raster (diff 80.5 % of ink (max 5 %))
- `html.el.canvas`: 12-canvas: rect (diff 100.0 % of ink (max 5 %))
- `html.el.details`: 16-sdk-taps: tapping summary opens details (toggle event)
- `html.attr.global`: 02-attributes-images: lang reaches the renderer (:lang(ru)) (got "17,17,17,1", want "0,100,0,1")
- `html.attr.dir`: 02-attributes-images: dir=rtl lays out inline boxes right to left (a 188 b 198); 02-attributes-images: dir=auto with Hebrew text resolves to rtl (got "ltr", want "rtl")
- `svg.shapes`: 13-svg: getBBox() of a rect (0,0,0,0); 13-svg: getTotalLength() of a path (got 0, want 40 ±0.1)
- `svg.structure`: 13-svg: viewBox scales content (10 user units → 20px) (0 at -8); 13-svg: structure (diff 10.9 % of ink (max 5 %))
- `svg.text`: 13-svg: getComputedTextLength() of a text (0); 13-svg: text (diff 67.7 % of ink (max 50 %); #222 0/696px, #c33 0/235px, #36c 0/204px)
- `svg.gradients`: 13-svg: gradients (diff 97.1 % of ink (max 5 %))
- `svg.css`: 13-svg: css (diff 91.9 % of ink (max 5 %))
- `css.display.table`: 03-layout: table cells side by side, the second takes the rest (0/125 125/75)
- `css.box.outline`: 05-colour-backgrounds: outline (diff 40.4 % of ink (max 5 %))
- `css.box.float`: 03-layout: float:left pushes inline text right (text x 0); 03-layout: float:right sits at the right edge (got 30, want 200 ±0.5)
- `css.background.basic`: 05-colour-backgrounds: background-position (got "", want "1px 2px"); 05-colour-backgrounds: background (diff 78.1 % of ink (max 5 %))
- `css.gradient.radial`: 05-colour-backgrounds: radial (diff 48.9 % of ink (max 5 %))
- `css.gradient.conic`: 05-colour-backgrounds: conic (diff 72.1 % of ink (max 5 %))
- `css.font.generic`: 06-text-fonts: monospace: i and M have the same advance (got 22, want 67.5 ±0.5); 06-text-fonts: serif and sans-serif are different faces (112.5 vs 112.5)
- `css.font.ui-families`: 06-text-fonts: ui-monospace: i and M have the same advance (got 22, want 67.5 ±0.5)
- `css.font.basic`: 06-text-fonts: font-weight: 700 is wider than 400 (59 vs 59)
- `css.text.overflow`: 06-text-fonts: ellipsis (diff 27.1 % of ink (max 50 %); #36c 0/70px)
- `css.text.emoji`: 06-text-fonts: emoji (diff 89.6 % of ink (max 50 %))
- `css.transform.2d`: 09-transforms-effects: translateX(30px) moves the box (got 0, want 30 ±0.5); 09-transforms-effects: rotate(90deg) swaps the bounding box (40x20); 09-transforms-effects: scale(2) with transform-origin 0 0 (110 10); 09-transforms-effects: the translate property (150,0)
- `css.filter.blur-brightness-drop-shadow`: 09-transforms-effects: blur (diff 37.7 % of ink (max 10 %)); 09-transforms-effects: brightness-drop-shadow (diff 68.0 % of ink (max 5 %))
- `css.backdrop-filter`: 09-transforms-effects: backdrop (diff 43.1 % of ink (max 10 %))
- `css.value.content`: 04-values-units: counters: counter-reset / increment / counter() (normal)
- `sel.before-after`: 07-selectors: ::after computed style (got "17,17,17,1", want "51,0,0,1")
- `anim.keyframes`: 10-css-animations: fill-forwards (diff 100.0 % of ink (max 5 %))
- `anim.events`: 10-css-animations: animationstart fires (timed out); 10-css-animations: animationend fires (timed out); 10-css-animations: animation events carry animationName and elapsedTime (timed out); 10-css-animations: animationiteration fires twice (timed out)
- `anim.waapi`: 11-web-animations: the renderer applies the seeked frame (computed transform) (got NaN, want 20 ±0.5); 11-web-animations: a seek is visible to a layout query in the same task (got 20, want 45 ±0.5); 11-web-animations: a seeked width animation lays out in a later task (got 20, want 45 ±0.5); 11-web-animations: animate (diff 100.0 % of ink (max 5 %))
- `anim.waapi-css-control`: 11-web-animations: a CSS animation shows up in getAnimations() (0 undefined); 11-web-animations: pause() + currentTime seek a CSS animation (got undefined, want "paused"); 11-web-animations: the renderer shows the seeked CSS frame (got 0, want 10 ±0.5); 11-web-animations: css-control (diff 66.7 % of ink (max 5 %))
- `canvas.rect`: 12-canvas: rect (diff 100.0 % of ink (max 5 %))
- `canvas.path`: 12-canvas: path (diff 90.7 % of ink (max 5 %))
- `canvas.line-style`: 12-canvas: line-style (diff 91.9 % of ink (max 5 %))
- `canvas.colors`: 12-canvas: rect (diff 100.0 % of ink (max 5 %))
- `canvas.gradients`: 12-canvas: gradients (diff 98.0 % of ink (max 5 %))
- `canvas.transform`: 12-canvas: transform (diff 94.4 % of ink (max 5 %))
- `canvas.clip`: 12-canvas: clip (diff 95.8 % of ink (max 5 %))
- `canvas.text`: 12-canvas: text (diff 68.6 % of ink (max 50 %); #222 0/932px, #36c 0/334px)
- `canvas.alpha`: 12-canvas: alpha (diff 100.0 % of ink (max 5 %))
- `canvas.path2d`: 12-canvas: path2d (diff 91.0 % of ink (max 5 %))
- `canvas.draw-image`: 12-canvas: drawImage of a loaded <img> (timed out); 12-canvas: draw-image (diff 96.4 % of ink (max 5 %))
- `canvas.hit-test`: 12-canvas: isPointInPath inside / outside (false false); 12-canvas: isPointInStroke (false)
- `canvas.hidpi`: 12-canvas: rect (diff 100.0 % of ink (max 5 %))
- `js.dom.query`: 14-dom-layout-api: getElementsByClassName is live (0 → 0)
- `js.layout.rect`: 14-dom-layout-api: getClientRects of a wrapped inline gives one rect per line (1)
- `js.layout.box`: 14-dom-layout-api: scrollHeight of overflowing content (got 136, want 70); 14-dom-layout-api: offsetParent (got false, want true)
- `js.layout.computed-style`: 14-dom-layout-api: shorthand white-space (got "", want "nowrap"); 14-dom-layout-api: shorthand overflow (got "", want "hidden"); 14-dom-layout-api: shorthand margin (got "", want "4px 0px")
- `js.layout.resize-observer`: 14-dom-layout-api: ResizeObserver reports the new width (timed out)
- `tw.theme-params`: 16-sdk-taps: --tg-theme-bg-color is defined for CSS (1,2,3,1); 20-client-behaviour: themeParams and --tg-theme-bg-color change with the theme, before themeChanged fires (bg #ffffff → #212121, css )
- `input.mouse-compat`: 16-sdk-taps: mousedown → mouseup on a tap (pointerdown,pointerup,click)

## web litehtml: in-profile failures

- `html.el.structure`: 01-html-elements: p has 1em block margins (got NaN, want 13 ±0.5)
- `html.el.headings`: 01-html-elements: headings are bold (NaN,NaN)
- `html.el.sections`: 01-html-elements: sectioning elements are blocks (block,block,inline,block,block,block,block,block,inline,inline); 01-html-elements: address is italic (got "", want "italic")
- `html.el.inline-text`: 01-html-elements: b, strong are bold (NaN,NaN); 01-html-elements: i, em, cite, dfn, var are italic; 01-html-elements: u, ins underline; s, del strike ( / ); 01-html-elements: code, kbd, samp are monospace
- `html.el.sub-sup`: 01-html-elements: sub sits lower, sup higher (base 399-399 sub 399 sup 399)
- `html.el.pre`: 01-html-elements: pre is monospace
- `html.el.blockquote`: 01-html-elements: blockquote is indented 40px (got NaN, want 40 ±0.5); 01-html-elements: figure is indented 40px (got NaN, want 40 ±0.5)
- `html.el.lists`: 01-html-elements: ul is padded 40px (got NaN, want 40 ±0.5); 01-html-elements: ul items have discs (got "", want "disc"); 01-html-elements: ol items are numbered (got "", want "decimal"); 01-html-elements: dd is indented 40px (got NaN, want 40 ±0.5)
- `html.el.table`: 01-html-elements: th is bold and centred (NaN )
- `html.el.img`: 02-attributes-images: PNG decodes (naturalWidth 2) (timed out); 02-attributes-images: GIF decodes (naturalWidth 1) (timed out)
- `html.el.canvas`: 12-canvas: rect (diff 100.0 % of ink (max 5 %))
- `html.el.button`: 01-html-elements: button has a box with padding (54.28857421875x23.399999618530273)
- `html.el.details`: 01-html-elements: closed details is only as tall as its summary (0 vs 16); 01-html-elements: summary is a list item (disclosure marker) (got "block", want "list-item"); 16-sdk-taps: tapping summary opens details (toggle event)
- `html.el.a-fragment`: 01-html-elements: a[href=#…] is a link (UA link colour, underline) (0,0,255,1)
- `html.el.a-external`: 01-html-elements: it is still drawn as a link
- `html.attr.global`: 02-attributes-images: lang reaches the renderer (:lang(ru)) (got "17,17,17,1", want "0,100,0,1")
- `html.attr.dir`: 02-attributes-images: dir=rtl sets direction (got "", want "rtl"); 02-attributes-images: dir=rtl lays out inline boxes right to left (a 8 b 18); 02-attributes-images: dir=auto with Hebrew text resolves to rtl (got "", want "rtl")
- `svg.shapes`: 13-svg: getBBox() of a rect (0,0,0,0); 13-svg: getTotalLength() of a path (got 0, want 40 ±0.1); 13-svg: shapes (diff 89.2 % of ink (max 5 %))
- `svg.structure`: 13-svg: viewBox scales content (10 user units → 20px) (0 at 0); 13-svg: structure (diff 93.3 % of ink (max 5 %))
- `svg.text`: 13-svg: getComputedTextLength() of a text (0); 13-svg: text (diff 58.6 % of ink (max 50 %); #222 394/696px, #c33 0/235px, #36c 0/204px)
- `svg.gradients`: 13-svg: gradients (diff 97.1 % of ink (max 5 %))
- `svg.css`: 13-svg: a stylesheet fill reaches SVG (computed fill); 13-svg: css (diff 92.9 % of ink (max 5 %))
- `css.display.flex`: 03-layout: row places children side by side (8,48,88); 03-layout: justify-content: space-between (got 208, want 200 ±0.5); 03-layout: align-items: center (got 247.55, want 10 ±0.5); 03-layout: flex-direction: column stacks (8,280.54998779296875)
- `css.gap.flex`: 03-layout: gap between flex items (got 48, want 50 ±0.5); 03-layout: row-gap between wrapped lines (got 365.55, want 16 ±0.5); 03-layout: flex (diff 11.8 % of ink (max 5 %))
- `css.display.grid`: 03-layout: grid-template-columns: 50px 1fr (200 / 0 200); 03-layout: auto-placement wraps to the next row (0,20); 03-layout: repeat(4, 1fr) gives equal tracks (0/200 0/200 0/200 0/200); 03-layout: grid-column: 2 / span 2 on row 2 (0,10 w 200)
- `css.box.size`: 03-layout: width: fit-content shrinks to the text (w 200)
- `css.box.border`: 03-layout: border (diff 93.1 % of ink (max 5 %))
- `css.box.aspect-ratio`: 03-layout: aspect-ratio: 2 makes a 60px wide box 30px high (got 0, want 30 ±0.5)
- `css.box.outline`: 03-layout: computed outline is 4px solid ( ); 05-colour-backgrounds: outline (diff 23.4 % of ink (max 5 %))
- `css.box.shadow`: 05-colour-backgrounds: shadow (diff 100.0 % of ink (max 5 %))
- `css.box.float`: 03-layout: float:left pushes inline text right (text x 0)
- `css.box.object-fit`: 05-colour-backgrounds: object-fit (diff 28.3 % of ink (max 5 %))
- `css.position.relative-absolute`: 03-layout: inset: 0 fills the parent (0x0)
- `css.overflow.clip`: 03-layout: overflow: hidden is computed (got "", want "hidden"); 03-layout: overflow: clip is computed (got "", want "clip")
- `css.visibility`: 03-layout: visibility: hidden is skipped by hit testing (p); 03-layout: a hidden box keeps its size
- `css.opacity`: 03-layout: computed opacity (got "", want "0.5"); 09-transforms-effects: opacity (diff 60.0 % of ink (max 5 %))
- `css.color.basic`: 05-colour-backgrounds: named colour (got "0,0,0,0", want "102,51,153,1"); 05-colour-backgrounds: currentColor (got "", want "0,0,200,1"); 05-colour-backgrounds: basic (diff 25.0 % of ink (max 5 %))
- `css.color.light-dark`: 05-colour-backgrounds: light-dark (diff 100.0 % of ink (max 5 %))
- `css.background.basic`: 05-colour-backgrounds: background-image keeps the data: image; 05-colour-backgrounds: background-size (got "", want "10px 12px"); 05-colour-backgrounds: background-position (got "", want "1px 2px"); 05-colour-backgrounds: background-repeat (got "", want "no-repeat")
- `css.gradient.conic`: 05-colour-backgrounds: conic (diff 56.5 % of ink (max 5 %))
- `css.font.generic`: 06-text-fonts: computed font-family keeps the generic name
- `css.font.ui-families`: 06-text-fonts: the validator adds a generic fallback (ui-monospace → …, monospace)
- `css.font.basic`: 06-text-fonts: font-style: italic (got "", want "italic"); 06-text-fonts: font shorthand ( 15px)
- `css.font.variant-numeric`: 06-text-fonts: tabular-nums: 1111 as wide as 0000 (got 23.82, want 32.45 ±0.5); 06-text-fonts: font-feature-settings "tnum" (got 23.82, want 32.45 ±0.5)
- `css.text.align`: 06-text-fonts: text-align: center (got 0, want 60 ±1); 06-text-fonts: text-align: right (got 0, want 120 ±1); 06-text-fonts: text-indent (got 0, want 20 ±1)
- `css.text.decoration`: 06-text-fonts: text-decoration: underline
- `css.text.transform`: 06-text-fonts: letter-spacing: 5px adds 5px per letter (got 0, want 20 ±1.5); 06-text-fonts: word-spacing: 10px adds 10px per space (got 0, want 20 ±1.5)
- `css.text.wrap`: 06-text-fonts: overflow-wrap: anywhere breaks a long word (40x16); 06-text-fonts: word-break: break-all breaks a long word (16)
- `css.text.overflow`: 06-text-fonts: ellipsis (diff 15.1 % of ink (max 50 %); #36c 0/70px)
- `css.text.list-style`: 06-text-fonts: list-style-position: inside moves the text right (48 vs 48); 06-text-fonts: list-style-type: none (got "", want "none")
- `css.text.rtl`: 06-text-fonts: Hebrew in dir=rtl starts at the right edge (got 8, want 128 ±1)
- `css.transform.2d`: 09-transforms-effects: translateX(30px) moves the box (got 0, want 30 ±0.5); 09-transforms-effects: rotate(90deg) swaps the bounding box (40x20); 09-transforms-effects: scale(2) with transform-origin 0 0 (110 10); 09-transforms-effects: the translate property (150,0)
- `css.filter.blur-brightness-drop-shadow`: 09-transforms-effects: filter functions are kept; 09-transforms-effects: blur (diff 37.7 % of ink (max 10 %)); 09-transforms-effects: brightness-drop-shadow (diff 68.1 % of ink (max 5 %))
- `css.backdrop-filter`: 09-transforms-effects: backdrop (diff 91.1 % of ink (max 10 %))
- `css.pointer-events`: 09-transforms-effects: pointer-events: none lets hits through (pe-over)
- `css.touch-action`: 09-transforms-effects: touch-action reaches the renderer (got "", want "none")
- `css.cursor`: 09-transforms-effects: cursor reaches the renderer (got "", want "pointer")
- `css.value.units`: 04-values-units: ch is the width of "0" (got 78.01, want 0 ±1)
- `css.value.math`: 04-values-units: calc(50% - 10px) (got 200, want 90 ±0.5); 04-values-units: min(50px, 30%) (got 200, want 50 ±0.5); 04-values-units: max(10px, 20%) (got 200, want 40 ±0.5); 04-values-units: clamp(20px, 50%, 60px) (got 200, want 60 ±0.5)
- `css.value.custom-properties`: 04-values-units: getPropertyValue reads a custom property (got "", want "rgb(0,120,0)")
- `css.value.keywords`: 04-values-units: display: initial is inline (got "block", want "inline"); 04-values-units: color: unset inherits (got "200,0,0,1", want "0,0,140,1"); 04-values-units: display: revert is the UA block (got "inline", want "block")
- `css.value.content`: 04-values-units: ::before content takes space (0); 04-values-units: content: attr(data-label) (0); 04-values-units: counters: counter-reset / increment / counter(); 04-values-units: quotes
- `sel.structural`: 07-selectors: :empty (got "29,0,0,1", want "28,0,0,1")
- `sel.before-after`: 07-selectors: ::before box takes space (got "0", want "ok"); 07-selectors: ::after computed style (got "17,17,17,1", want "51,0,0,1")
- `at.keyframes`: 10-css-animations: keyframes (diff 100.0 % of ink (max 5 %))
- `anim.transition`: 10-css-animations: a transition with a negative delay starts part-way (got 110, want 35 ±1); 10-css-animations: transition (diff 100.0 % of ink (max 5 %))
- `anim.keyframes`: 10-css-animations: a paused animation at -2.5s of 10s is at 25% (got "", want 10 ±0.5); 10-css-animations: keyframes (diff 100.0 % of ink (max 5 %)); 10-css-animations: alternate (diff 100.0 % of ink (max 5 %)); 10-css-animations: colour (diff 100.0 % of ink (max 5 %))
- `anim.play-state`: 10-css-animations: paused stays put (got "", want 10 ±0.5); 10-css-animations: keyframes (diff 100.0 % of ink (max 5 %))
- `anim.easing`: 10-css-animations: steps(4, end) at 37.5% is the first step (got 374, want 25 ±0.5); 10-css-animations: ease-in at 50% lags behind linear (374); 10-css-animations: steps (diff 20.0 % of ink (max 5 %))
- `anim.events`: 10-css-animations: animationstart fires (timed out); 10-css-animations: animationend fires (timed out); 10-css-animations: animation events carry animationName and elapsedTime (timed out); 10-css-animations: animationiteration fires twice (timed out)
- `anim.waapi`: 11-web-animations: the renderer applies the seeked frame (computed transform) (got NaN, want 20 ±0.5); 11-web-animations: a seek is visible to a layout query in the same task (got 20, want 45 ±0.5); 11-web-animations: a seeked width animation lays out in a later task (got 20, want 45 ±0.5); 11-web-animations: animate (diff 100.0 % of ink (max 5 %))
- `anim.waapi-css-control`: 11-web-animations: a CSS animation shows up in getAnimations() (0 undefined); 11-web-animations: pause() + currentTime seek a CSS animation (got undefined, want "paused"); 11-web-animations: the renderer shows the seeked CSS frame (got NaN, want 10 ±0.5); 11-web-animations: css-control (diff 66.7 % of ink (max 5 %))
- `canvas.rect`: 12-canvas: rect (diff 100.0 % of ink (max 5 %))
- `canvas.path`: 12-canvas: path (diff 90.7 % of ink (max 5 %))
- `canvas.line-style`: 12-canvas: line-style (diff 91.9 % of ink (max 5 %))
- `canvas.colors`: 12-canvas: rect (diff 100.0 % of ink (max 5 %))
- `canvas.gradients`: 12-canvas: gradients (diff 98.0 % of ink (max 5 %))
- `canvas.transform`: 12-canvas: transform (diff 94.4 % of ink (max 5 %))
- `canvas.clip`: 12-canvas: clip (diff 95.8 % of ink (max 5 %))
- `canvas.text`: 12-canvas: text (diff 68.6 % of ink (max 50 %); #222 0/932px, #36c 0/334px)
- `canvas.alpha`: 12-canvas: alpha (diff 100.0 % of ink (max 5 %))
- `canvas.path2d`: 12-canvas: path2d (diff 91.0 % of ink (max 5 %))
- `canvas.draw-image`: 12-canvas: drawImage of a loaded <img> (timed out); 12-canvas: draw-image (diff 96.4 % of ink (max 5 %))
- `canvas.hidpi`: 12-canvas: rect (diff 100.0 % of ink (max 5 %))
- `js.dom.core`: 14-dom-layout-api: innerHTML replaces the children (1)
- `js.dom.query`: 14-dom-layout-api: getElementsByClassName is live (0 → 0)
- `js.layout.rect`: 14-dom-layout-api: getClientRects of a wrapped inline gives one rect per line (1)
- `js.layout.box`: 14-dom-layout-api: scrollHeight of overflowing content (got 30, want 70); 14-dom-layout-api: offsetParent (got false, want true)
- `js.layout.computed-style`: 14-dom-layout-api: longhand (width) (got "116.000000px", want "100px"); 14-dom-layout-api: shorthand white-space (got "", want "nowrap"); 14-dom-layout-api: shorthand overflow (got "", want "hidden"); 14-dom-layout-api: shorthand margin (got "", want "4px 0px")
- `js.layout.resize-observer`: 14-dom-layout-api: ResizeObserver reports the new width (timed out)
- `js.layout.measure-text`: 14-dom-layout-api: measureText agrees with DOM text width (got 88.92, want 0 ±1.5)
- `tw.theme-params`: 16-sdk-taps: --tg-theme-bg-color is defined for CSS (1,2,3,1); 20-client-behaviour: themeParams and --tg-theme-bg-color change with the theme, before themeChanged fires (bg #ffffff → #212121, css )
- `behaviour.policy-strict`: 19-limits: attr() of a text attribute (data-*) works (0)

## Visual set

For people (the test channel): judged by eye. Runners only check that each widget loads without errors.

| Widget | web iframe | web shadow | web blitz | web litehtml |
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
| visual/sdk/01-link-external | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/02-link-telegram | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/03-link-rejected | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/04-link-timer | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/05-haptics | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/06-haptics-rate | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/07-storage-recreate | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/08-storage-restart | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/09-storage-delete | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/10-storage-quota | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/11-crash-restart | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/12-drag-gestures | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/13-max-height | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/14-theme-palette | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/15-storage-forward | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/16-bot-edit | ✅ | ✅ | ✅ | ✅ |
| visual/sdk/17-size-hint | ✅ | ✅ | ✅ | ✅ |
