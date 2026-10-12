# showcase — interactive, animated demos on the full web profile

21 widgets written the way a widget author would write them today, with
nothing held back for a `render()` harness or for a native renderer: CSS
transitions and `@keyframes`, grid, transforms and 3D flips, inline SVG with
gradients and `stroke-dashoffset`, canvas 2D, pointer events (drag, swipe,
scrub), `<input type=range>`, live clocks and tweened numbers. Each one is a
realistic chat-message case with a small state machine behind it, not a
feature probe. They are the content of the web chat feed (`?feed=1`,
`apps/web/src/feed.ts`, `docs/chat-feed-web.md`) and run in the dev picker like every
other group. Product-shaped dev content, not a test of the renderers: the
canvas renderers (Blitz, litehtml) draw only the subset they support.

The difference from the other groups: `htmlbubbles-ported` keeps Telegram's
`render()`-era demos faithful (static cards, little motion), `common` is
deliberately flat so three renderers compare like for like, `generated` is
what a model writes from a prompt. This group is what the feature should look
like at its best on the WebView path.

| Batch | File | The case | What moves |
|---|---|---|---|
| `travel/` | `01-boarding-pass` | airline boarding pass | gate countdown (1 Hz, turns orange → red), tap flips the pass to a QR pattern (3D `rotateY`), Add to Wallet settles into a done state, the plane glides along the route |
| | `02-ride-tracking` | taxi on its way | the car drives the route on a stylised SVG map (polyline interpolation, no layout queries), the ETA and distance count down, status steps light up, Call / Message reach the bot |
| | `03-hotel-booking` | hotel card | swipeable photo carousel (pointer events + snap), two-tap date range on a mini calendar, guests stepper, price breakdown tweens, Book → a drawn check mark |
| | `04-weather` | weather forecast | animated SVG sky (sun rays, drifting clouds, rain), an hourly temperature curve that morphs (`d` transition) when a day is tapped, the big number tweens |
| | `05-itinerary` | a day's plan | vertical timeline whose fill and NOW marker move with a 60× demo clock (1 Hz), cards expand with a height transition, Navigate / Push 30 min |
| `commerce/` | `01-order-status` | food delivery | steps complete in sequence (check marks drawn with `stroke-dashoffset`), "stops away" ticks, receipt collapses, a star rating slides in after delivery |
| | `02-product-configurator` | headphones in a shop | the inline-SVG product recolours with a transition per swatch, cushions and fit chips, the price rolls, Add to cart bounces the cart badge |
| | `03-split-bill` | restaurant bill | tap a dish then the friends who shared it, per-person totals tween, a tip range slider, Request marks friends as paid one by one |
| | `04-flash-sale` | flash sale | flip-clock countdown (two alternating class names restart the flip without a reflow), a stock bar that depletes as others buy, tiles lift on hover, Buy turns into a stepper, one item sells out |
| | `05-subscription-plans` | pricing plans | monthly / yearly segmented thumb, prices roll to the new numbers, the chosen plan lifts with a gradient border and reveals its features |
| `life/` | `01-fitness-rings` | activity summary | three rings sweep in with staggered easing, the step count rolls up, a week of bars grows; tapping a day replays its rings |
| | `02-smart-home` | smart-home panel | animated toggles, a dimmer that lights the tile, a thermostat dial dragged with pointer events (SVG arc), scenes that set everything at once |
| | `03-pomodoro` | focus timer | an SVG ring drains at 1 Hz, Start ↔ Pause, session dots, a break turns the card calm with a breathing animation; a "demo speed ×20" link |
| | `04-habit-tracker` | weekly habits | tapping a cell pops a check in, the streak flame and completion ring update, a perfect day fires canvas confetti |
| | `05-recipe` | cooking steps | a servings stepper rescales every quantity (with a bump), ingredients tick off, step cards slide with a progress bar, a step's timer counts down on a ring |
| | `06-music-player` | music player | spinning vinyl art (paused when paused), a seekable waveform (pointer scrub), play/pause icon morph, an equaliser on the playing row; no audio (the sandbox has none) |
| `work/` | `01-ci-pipeline` | CI run | stages spin, pass and fail in sequence, log lines stream into a terminal pane, durations tick, Re-run failed replays it green |
| | `02-kanban` | sprint board | drag a card between columns (a fixed-position clone follows the pointer, the column under it highlights), counts and the progress bar update, Add pops a card in |
| | `03-event-rsvp` | event invite | blurred gradient orbs on the cover, an avatar stack that grows as others accept (timers), Going / Maybe / Can't, confetti on Going, a live "starts in" clock |
| | `04-live-poll` | live poll | other voters trickle in (seeded), options re-sort with a FLIP transform, bars and percentages tween, your vote is highlighted and retractable |
| | `05-stock-card` | stock quote | the price ticks with a green/red flash, the sparkline (path + gradient area) grows with each tick, range chips morph the curve, a scrubber reads the price at a point |

## Conventions

- A `<!-- showcase: full web profile … idea: … -->` header, then one
  `:root` block with `color-scheme: light dark` and `light-dark()` variables
  (both themes), the system font stack, a `.card` at `width: 100%` with no
  radius and no shadow of its own: the widget owns the whole bubble like a
  photo message, the host decides the width and clips with the bubble's
  radius (the dev page and the feed mark these bubbles `.bubble.edge`,
  `apps/web/src/style.css`: no padding, `overflow: hidden`, 18 px). The older groups
  (`htmlbubbles-ported`, `common`) are 392 px cards inside a padded bubble,
  matching Telegram's macOS beta, and keep that.
- State lives in JS, the DOM is updated in place; lists may be rebuilt with a
  template literal, the whole widget never is. No `top`/`parent`/`self`/
  `window`/`document` as global names.
- Everything that moves is cheap for the sandbox: CSS transitions and
  keyframes where the browser can do it alone, `requestAnimationFrame` only
  for tweened numbers and short sweeps (≤ 1.5 s), 1 Hz `setInterval` for
  clocks, 4 Hz for the player's progress. **No layout query per frame**: the
  ride's car is interpolated along a JS polyline instead of
  `getPointAtLength`, the itinerary measures `offsetTop` once a second, the
  flip clock and the price flash restart their animation by alternating two
  class names instead of `void el.offsetWidth` (every such read is a sync
  round trip to the renderer, `docs/protocol.md`).
- Demo clocks are sped up where a real one would show nothing in a minute
  (the itinerary runs at 60×, the ride takes 90 s, the recipe timer ×10,
  the CI run is real seconds); countdowns start at a value that reads well.
- Seeded pseudo-random data (an LCG) so two runs lay out the same; "other
  people" arrive on fixed timers.
- `TelegramWidget.sendMessage` on the actions a bot would want (vote, RSVP,
  order, scene); `openLink` once (the CI run's Open).
- Every clickable element that is a real button carries `role="button"` or
  is a `<button>`: `tests/generated.spec.ts` clicks the first four visible
  ones, so the first four must leave the widget in a state that is still
  deterministic enough to screenshot (the pomodoro's speed link is a plain
  span for that reason, and hidden steps of the recipe's deck are
  `visibility: hidden` so Playwright does not scroll them into view).

## How to run

```bash
corepack pnpm tsx packages/core/scripts/sandbox-node.ts corpus/showcase/work/04-live-poll.html --verbose   # the Node harness: no [error], no validator diagnostics
corepack pnpm test:showcase                      # sandbox vs native Chromium, up to 4 clicks → docs/compat-report-showcase.md, reports/showcase/
TW_RENDERER=shadow corepack pnpm test:showcase   # the same on the Shadow DOM renderer → docs/compat-report-showcase-shadow.md
corepack pnpm dev                                # 127.0.0.1:5173/?feed=1 — the feed on this corpus (corpus=common for the flat one); the picker lists showcase/… too
```

`pnpm build:ios|macos|android|desktop` copies the group into the native demos
(`packages/core/scripts/build-native-bundles.ts`); the iOS/macOS demo lists the
`showcase/<batch>` sections, runs them with `--autorun showcase`, and the iOS
chat feed's **Widgets** picker fills the feed with them (edge to edge, like
the web feed; `--feed-set corpus=showcase`). The widgets use grid, transforms,
SVG and canvas, so the Blitz and litehtml renderers show their gaps there,
which is a fair thing to look at but not what the group is for.

## Status (2026-10-12)

21 files. Every file passes the Node harness (no errors, no validator
diagnostics, no layout queries at load except the itinerary's and the poll's
`offsetTop`, which are on 1 Hz / per-vote clocks, not per frame).
`pnpm test:showcase` (sandbox vs native Chromium, up to 4 clicks, 3 % pixel
tolerance): **21/21**; 17 files at ≤ 0.25 %, the live ones a little higher
(`work/03-event-rsvp` 2.3 % — confetti and the avatar stack arrive on timers,
`work/01-ci-pipeline` 1.8 % — a streaming log, `commerce/04-flash-sale` 1.2 % —
the stock bar) (`docs/compat-report-showcase.md`, screenshots in
`reports/showcase/`). Looked at in the feed on the iframe and the Shadow DOM
renderer, light and dark: content in ~50 ms pooled / ~250 ms cold per bubble.
Not run on Blitz / litehtml on purpose (see above).
