# gestures — what breaks if widgets cannot claim vertical drags

Widgets for one question: **should vertical drags over a widget always scroll the chat, so
that widgets can claim only horizontal drags?** The set has two sides:

- **What breaks without vertical**: two games in pairs, the same widget on
  `touch-action: none` (`…-xy`, it claims both axes) and on `pan-y` (`…-x`, horizontal only,
  which is what the rule would leave it), and a wheel picker on `pan-x`, a vertical control.
  The two files of a pair differ in one CSS value and their first line.
- **What is fine with horizontal only**: a paddle game, a card carousel and a range slider,
  all on `pan-y`, the rule's behaviour already.

A widget can restrict itself with `touch-action` today, so the "only horizontal" side can be
tried without a framework change.

## The rule they run on (v0.1.2)

A press is undecided while it stays within 10 px. Past that, the dominant axis decides
**once, for the whole press**: either the widget gets the drag (every move until the finger
lifts, on both axes), or the chat takes it (list scroll, swipe-to-reply) and the widget gets
`pointercancel`. A press held 500 ms within the slop is a hold (`docs/sdk-v1.md` §5.5,
`packages/core/src/shared/gesture-policy.ts`).

| `touch-action` | vertical-first drag | horizontal-first drag | hold | double tap |
|---|---|---|---|---|
| `none` (`…-xy`) | widget | widget | widget (`contextmenu`) | widget |
| `pan-y` (`…-x`) | chat: scroll | widget | chat: message menu | widget |
| `pan-x` (`…-y`) | widget | chat: swipe-to-reply | chat: message menu | widget |

The leading 20 px of a widget never take a claim (the system back gesture), so every claimed
surface starts at least 20 px in. Every widget says in its first line which `touch-action` it
uses and what to try, and counts **presses taken by the chat** (its `pointercancel`s).

## The widgets

Sizes as the server counts them (gzip -6, limit 4096 B); heights at a 320 px wide bubble.

| File | `touch-action` | What it is | Try | Size | Height |
|---|---|---|---|---|---|
| **Breaks without vertical** | | | | | |
| `1-pairs/01-joystick-xy.html` | `none` | Star catcher: fly a dot with a virtual stick, collect stars | start up or down, circle, rest your thumb on the stick | 1837 B | 435 px |
| `1-pairs/02-joystick-x.html` | `pan-y` | the same game | the same; then start sideways and steer up | 1899 B | 435 px |
| `1-pairs/03-2048-xy.html` | `none` | 2048: swipe in four directions, equal tiles merge; score, best and the game are kept | play a few moves | 2490 B | 404 px |
| `1-pairs/04-2048-x.html` | `pan-y` | the same game | swipe up or down | 2502 B | 404 px |
| `3-vertical/08-wheel-picker-y.html` | `pan-x` | an alarm time picker: hour and minute wheels with momentum and snap | spin, flick, tap a number; start sideways | 2214 B | 294 px |
| **Fine with horizontal only** | | | | | |
| `2-horizontal/05-paddle-x.html` | `pan-y` | Brick breaker: slide anywhere to move the paddle, tap to serve | play it | 2085 B | 353 px |
| `2-horizontal/06-card-carousel-x.html` | `pan-y` | trips on cards with peeking neighbours, flick, snap, dots | swipe and flick, tap a card, drag up over a card | 2075 B | 310 px |
| `2-horizontal/07-range-slider-x.html` | `pan-y` | a two-thumb price filter from divs, with a histogram and haptic steps | slide either thumb, press the track; start a drag up on the track | 2021 B | 274 px |

## What "only horizontal" would change

- **Joystick (01 → 02).** The first move decides. A thumb on a stick rarely starts sideways on
  purpose: aiming up loses the press to the chat (the stick snaps back, the chat scrolls);
  a diagonal works only when its horizontal part is the larger. Once a press is the widget's,
  up and down drift reaches it: nothing is taken away mid-press. `pan-y` also gives holds to
  the chat: a thumb resting on the stick for half a second opens the message menu. The rule as
  stated says nothing about holds, and CSS cannot express "horizontal plus holds", so 02 is the
  rule plus losing holds.
- **2048 (03 → 04).** Up and down swipes scroll the chat, so only left and right move the tiles
  and the board fills up. The author would have to add buttons for up and down.
- **Wheel picker (08).** A vertical control: under the rule it cannot be spun at all, every drag
  scrolls the chat. The author would have to turn it on its side or replace it with buttons.
- Drawing on a canvas, panning a map and guiding a ball through a maze are 2D in the same way
  as the joystick.

Fine with horizontal only, unchanged by the rule:

- **Paddle (05)**, **card carousel (06)** and **range slider (07)** move along one horizontal
  axis; a vertical drag over them is the chat's scroll, which is what a reader wants. (`pan-y`
  gives a resting thumb to the message menu; none of them needs holds.)

## Renderer notes

- **The card carousel** moves its track with
  `transform: translateX(calc(11% - i * (78% + 10px) + dx px))` and snaps to a card through a
  `transition` on `transform`. It does not move on the iOS and Android Telegram clients, while the
  paddle, also on `pan-y`, works there: the claim works, and the paddle moves with `left`. On the
  web prototypes, litehtml draws no transform (the dot moves on, the track does not) and Blitz
  does not apply a transform set through a transition (the track stays where the finger left it).
  The automatic conformance set checks these forms (`css.transform.2d`, `css.value.math` in
  `auto/09-transforms-effects-a`; `anim.transition` in `auto/10-css-animations`), and
  `visual/05-transforms-effects-a` draws them for its reference picture. The carousel keeps its
  transform as the real case; the other widgets move things with `left` and `top` only.
- **The 2048 board** was a 4-column grid whose height came from `aspect-ratio: 1`, with
  content-sized rows: browsers share the free height unevenly between rows with and without a
  number, and a renderer without `aspect-ratio` collapses the board to its numbers. It is now
  sixteen cells placed in whole px from JS (the board measured once and again on resize with
  `ResizeObserver`), which iframe, Shadow DOM, Blitz, litehtml and the reference draw alike.
  Both features are in profile 1 and covered: `css.box.aspect-ratio` (`auto/03-layout-b`) and
  `css.display.grid` (`auto/03-layout-a`: `repeat(4, 1fr)` tracks, `grid-template-rows: 20px 1fr`
  on a definite height; `visual/04-layout`), so a client that cannot draw them has a renderer gap.

## Notes for authors

- A tap's `click` goes to the element with the pointer capture, not to the child that was
  pressed: the carousel and the wheel picker remember the card or the number pressed at
  `pointerdown`.
- A control that reacts at `pointerdown` or to the first pixels of a move changes before the
  policy has decided; when the chat takes the press (`pointercancel`), put the value back or end
  the press cleanly: the range slider undoes the jump of its nearest thumb, the stick
  re-centres, the carousel and the wheel snap back.
- A mouse drag still ends with a `click` (a finger's never does): the carousel ignores a click
  after the pointer moved more than 6 px.
- A game board: fixed sizes in px rather than a grid sized by `aspect-ratio`.

## How to try them

- Dev app (`corepack pnpm dev`): the `gestures/` group in the corpus list, or
  `?file=gestures/1-pairs/04-2048-x.html&width=320`. The **gestures** select is `phone` by
  default: the mouse plays a finger and the bubble shows what it took (`↕ list scroll`,
  `↩ swipe: reply`, `⋯ message menu`). **compare all** (`&compare=1`) draws the widget on every
  renderer side by side (Blitz and litehtml need `pnpm build:blitz-web`, `pnpm build:litehtml-web`).
- Chat feed: `?feed=1&corpus=gestures`. On a touch device, or with touch emulation in Chrome's
  device toolbar, a drag the chat takes really scrolls the feed.
- Node harness, the press report:
  `corepack pnpm tsx packages/core/scripts/sandbox-node.ts corpus/gestures/1-pairs/02-joystick-x.html --width 360 --swipe 180,335,0,-60 --swipe 180,335,60,0 --hold 180,335`.
- `corepack pnpm test:gesture-corpus` loads the eight widgets in the sandbox and in reference
  mode (no JS errors, pixels within 3 %) → `docs/compat-report-gestures.md`; with
  `TW_RENDERER=shadow` on the Shadow DOM renderer.
- The Telegram test group: the **Gestures** topic of `scripts/conformance/publish-channel.ts`,
  not in the default run: `--topic gestures` (each file's `idea:` line is its description).

Rules: pointer events and `setPointerCapture`, no libraries, no form controls (out of profile
1), theme variables where a colour has a theme counterpart, at most 3800 B of gzip and 560 px
of height at a 320 px wide bubble.
