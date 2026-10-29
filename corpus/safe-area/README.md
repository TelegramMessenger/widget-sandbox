# safe-area — the client's time over a widget without a caption

Seven widgets that show what a client passes as the content safe area (`docs/sdk-v1.md` §2.17
and §5.12) and what widgets do with it. A message with a widget and no caption gets its time
and status drawn by the client over the widget's bottom corner, on a backing, as over a photo:
the right corner in a left-to-right interface, the left one in a right-to-left one. The widget
gets the band at the bottom as `env(safe-area-inset-bottom)` (the validator rewrites it to
`--tg-content-safe-area-inset-bottom`), the corner's width as
`--tg-content-safe-area-corner-right` / `-left`, and both in script as
`TelegramWidget.contentSafeAreaInset`. With a caption every value is 0.

Post them without a caption (the test group's **Safe area** topic does): each one then shows
whether your client passed a band and a corner, and drew its time where it said. Each file's
first line says what to check.

| File | What it is | Check | Size | Height at 320 px, band / caption |
|---|---|---|---|---|
| `1-diagnostic/01-what-the-client-passed.html` | the six numbers of `contentSafeAreaInset`, the band (stripes) and the corner (red box) drawn from the variables, live | your time sits in the red box over the stripes and covers nothing else; with a caption: all 0, no stripes; an edit that adds or removes the caption counts a change | 1475 B | 313 / 273 px |
| `2-cards/02-full-bleed-card.html` | a trip offer painted to the bubble's edges; its bottom row runs into the band | the Book button sits in the band beside your time, the row ends before it, the card reaches the bubble's bottom edge | 1142 B | 261 / 261 px |
| `2-cards/03-rtl-card.html` | the same card in Hebrew, `dir=rtl`; the row pads the physical sides | the corner follows your interface, not the widget's direction: in a left-to-right client the time is on its right (its start) and the button keeps out of it | 1181 B | 261 / 261 px |
| `3-css/04-unstyled-note.html` | a notice with no CSS at all | the renderer's defaults pad the body by the band: the last line ends above your time | 497 B | 313 / 289 px |
| `3-css/05-model-css.html` | an order styled as models write it: `max(16px, env(safe-area-inset-left))`, `calc(12px + env(safe-area-inset-bottom))`, `@supports`, `env(keyboard-inset-height, 0px)` | the Pay button ends above your time; the `@supports` line says yes; the other `env()` name takes its fallback (a `profile` diagnostic) | 1094 B | 359 / 335 px |
| `4-script/06-canvas-chart.html` | a week of cycling on a canvas, laid out from `contentSafeAreaInset` | the bars and day labels end above the band; the week's total is in the band on the side away from your time; it redraws on `contentSafeAreaChanged` and `themeChanged` | 1825 B | 273 / 271 px |
| `5-caption/07-caption-pair.html` | a parcel tracker posted twice, without a caption and with one | without: the courier row runs into the band and stops before your time; with: every value 0, the row to the edge, the time in the caption's line | 1207 B | 194 / 194 px |

Sizes as the harness counts them (zlib at level 9, limit 4096 B, 3800 B in this repository);
heights at a 320 px wide bubble with the reference band (24 px, a 64 px corner on the right)
and with a caption (`sandbox-node.ts --width 320 [--safe-area 24,64]`).

## Notes for authors

- Keep `env(safe-area-inset-bottom)` clear at the bottom, but only the corner of that band is
  covered: content may run into the band on the other side (`02`, `07`). Pad that side with
  the physical variables, `--tg-content-safe-area-corner-right` and `-left`, never with
  inline-start or -end: the corner follows the client's interface, not your text's direction
  (`03`).
- An unstyled body is padded by the band for you (`04`). **If you handle the band yourself,
  set the body's padding**: `body { margin: 0 }` alone keeps the renderer's default bottom
  padding, and your own band comes on top of it (a full-bleed background stops 24 px short of
  the edge). `body { margin: 0; padding: 0 }` (`02`, `03`, `05`, `06`).
- A canvas cannot use CSS: read `TelegramWidget.contentSafeAreaInset` and redraw on
  `contentSafeAreaChanged` (`06`). Do not read the variables with `getComputedStyle` every
  frame: it is a layout query.
- Nothing a user taps goes under the time: taps there are the client's.
- With a caption every value is 0, and the same file must still look right (`07`).

## How to try them

- Dev app (`corepack pnpm dev`): the `safe-area/` group with the **time** control on *over the
  corner* (`?time=corner`, or `corner-rtl` for a right-to-left client), for example
  `?file=safe-area/1-diagnostic/01-what-the-client-passed.html&width=320&time=corner`. The dev
  app draws its time over the bubble and passes the band; the reference column gets the same
  root variables (its `env()` stays the browser's own).
- Chat feed: `?feed=1&corpus=safe-area`: every bubble without a caption, the time over it.
- Node harness: `corepack pnpm tsx packages/core/scripts/sandbox-node.ts corpus/safe-area/4-script/06-canvas-chart.html --width 320 --safe-area 24,64`
  (`24,0,64` for the corner on the left).
- `corepack pnpm test:safe-area-corpus` loads the seven in the sandbox and in reference mode
  with the band on both sides (the dev app's `?time=corner`; Chromium's safe-area override for
  the reference frame's own `env()`): no JS errors, pixels within 3 % →
  `docs/compat-report-safe-area.md`; with `TW_RENDERER=shadow` on the Shadow DOM renderer.
- The Telegram test group: the **Safe area** topic of `scripts/conformance/publish-channel.ts`,
  not in the default run: `--topic safe-area`. Every widget goes out without a caption, `07`
  once more with one (its header says `caption: both`); each file's `idea:` line is its
  description.
- The native demos list the group (`pnpm build:ios|android` copy it) and pass the reference
  band with the time over the bubble.

Rules: no libraries, no form controls (out of profile 1), theme variables for every colour,
at most 3800 B after compression and 560 px of height at a 320 px wide bubble.
