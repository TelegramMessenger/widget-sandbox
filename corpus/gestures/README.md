# gestures — widgets that claim drags on one axis, both, or none

Ten real-world widgets for trying out who gets a drag over a widget in a chat. Each claims its
drags with CSS `touch-action` under the v0.1.2 rule (`docs/sdk-v1.md` §5.5,
`packages/core/src/shared/gesture-policy.ts`). They exist to answer one question before
it is decided: **should vertical drags over a widget always scroll the chat, so that
widgets can claim only horizontal drags?** None of them needs a framework change: each
widget sets its own claim with `touch-action`, so the "horizontal only" behaviour can
be tried today (`2-horizontal/04` is `1-both-axes/01` with one CSS value changed).

## The rule they run on (v0.1.2)

A press is undecided while it stays within 10 px. Past that, the dominant axis decides
**once, for the whole press**: either the widget gets the drag (every move until the
finger lifts, on both axes), or the chat takes it (list scroll, swipe-to-reply) and the
widget gets `pointercancel`. A press held 500 ms within the slop is a hold.

| `touch-action` | vertical-first drag | horizontal-first drag | hold | double tap |
|---|---|---|---|---|
| `none` | widget | widget | widget (`contextmenu`) | widget |
| `pan-y` | chat: scroll | widget | chat: message menu | widget |
| `pan-x` | widget | chat: swipe-to-reply | chat: message menu | widget |
| `auto` | chat: scroll | chat: swipe-to-reply | chat: message menu | widget |

The leading 20 px of a widget never take a claim (the system back gesture), so every claimed
surface here starts at 20 px: the body's 12 px margin plus 8 px of its own.

## The widgets

Each one says in its first line which `touch-action` it uses and what to try, and counts
**presses taken by the chat** (its `pointercancel`s), so a tester sees at once when the chat
took a press. Sizes as the server counts them (gzip -6, limit 4096 B); heights at a 320 px wide bubble.

| File | `touch-action` | What it is | Try | Size | Height |
|---|---|---|---|---|---|
| `1-both-axes/01-joystick-game.html` | `none` | Star catcher: fly a dot with a virtual stick, collect stars | start up or down, circle, rest your thumb on the stick | 1820 B | 435 px |
| `1-both-axes/02-drawing-pad.html` | `none` | a canvas to draw on, five inks, Clear | a stroke straight down, a long press for a dot | 1536 B | 364 px |
| `1-both-axes/03-map-pan.html` | `none` | a city map (SVG) in a clipped viewport | pan diagonally and up first, double-tap to zoom, tap a pin | 1955 B | 325 px |
| `2-horizontal/04-joystick-game-x.html` | `pan-y` | **the 01 game**, one CSS value changed | start up: the chat scrolls; start sideways, then steer up: it works; rest your thumb: the menu | 1885 B | 435 px |
| `2-horizontal/05-paddle-game.html` | `pan-y` | Brick breaker: slide anywhere to move the paddle, tap to serve | play it; then start a drag upwards | 2085 B | 353 px |
| `2-horizontal/06-card-carousel.html` | `pan-y` | trips on cards with peeking neighbours, flick, snap, dots | swipe and flick, tap a card, drag up over a card | 2075 B | 310 px |
| `2-horizontal/07-range-slider.html` | `pan-y` | a two-thumb price filter from divs, with a histogram and haptic steps | slide either thumb, press the track; start a drag up on the track | 2021 B | 274 px |
| `3-vertical/08-volume-faders.html` | `pan-x` | a four-channel mixer, faders that follow the finger | slide a fader, double-tap to mute, start sideways | 1523 B | 318 px |
| `3-vertical/09-wheel-picker.html` | `pan-x` | an alarm time picker: hour and minute wheels with momentum and snap | spin, flick, tap a number; start sideways | 2221 B | 294 px |
| `4-control/10-plain-list.html` | `auto` | the control: a packing checklist | tap to tick, double-tap to star; any drag or hold is the chat's | 1407 B | 421 px |

## What "only horizontal" would change

| Widget | Today | Under the rule |
|---|---|---|
| 01 joystick, 02 drawing pad, 03 map (`none`) | every press is the widget's | a press whose first move is vertical scrolls the chat; once a press has started sideways it keeps both axes. 04 shows the joystick under it. Holds: the rule as stated says nothing about them; `pan-y` gives them to the chat, so 04 also loses the still thumb, which the rule may not |
| 04 joystick on `pan-y` | as the rule | unchanged |
| 05 paddle, 06 carousel, 07 range slider (`pan-y`) | horizontal is theirs, vertical the chat's | unchanged: a horizontal control needs nothing more |
| 08 faders, 09 wheel picker (`pan-x`) | vertical is theirs | **cannot be dragged at all**: every drag on them scrolls the chat or swipes to reply; the author has to change the control (horizontal faders, a wheel on its side, tap-only steppers) |
| 10 list (`auto`) | every drag and hold is the chat's | unchanged |

What to feel when comparing 01 and 04:

- **The first move decides.** A thumb on a stick rarely starts sideways on purpose. On 04,
  aiming up loses the press to the chat (the stick snaps back, the chat scrolls); aiming
  diagonally works when the horizontal part is larger. Once a press is the widget's, up and
  down drift reaches it: nothing is taken away mid-press.
- **Holds.** On `pan-y`, a thumb resting on the stick for half a second opens the message menu.
  Any game control that is pressed and held needs `none` (or a rule that keeps holds).
- **The chat's side.** On 01 the stick pad is a dead zone for scrolling: a reader who only
  wants to scroll past the bubble has to put the finger on the arena, the text or the bubble's
  edges. On 04 the pad scrolls the chat like text. The arena itself is `auto` in both, so it
  scrolls either way.

## Notes for authors (found while building these)

- **A tap's `click` goes to the element with the pointer capture**, not to the child that was
  pressed: a widget that calls `setPointerCapture` on a container finds the tapped child at
  `pointerdown` (`03` pins, `06` cards, `09` numbers).
- **Undo on `pointercancel`.** A control that reacts at `pointerdown` (07 jumps the nearest thumb
  to the press) or to the first few pixels of a move (08) changes its value before the policy
  has decided; when the chat takes the press, put the value back. 08 moves relative to the
  finger instead of jumping, which also lets a double tap mute without moving the level.
- **A mouse drag still ends with a `click`** (a finger's never does): 03 and 06 ignore a click
  after the pointer moved more than 6 px.
- Every widget handles `pointercancel` as the end of its press: the stick re-centres, the
  carousel snaps back to its card, the wheel snaps to its value.

## How to try them

- Dev app (`corepack pnpm dev`): the `gestures/` group in the corpus list, or
  `?file=gestures/2-horizontal/04-joystick-game-x.html&width=320`. The **gestures** select is
  `phone` by default: the mouse plays a finger and the bubble shows what it took
  (`↕ list scroll`, `↩ swipe: reply`, `⋯ message menu`); the Gestures panel lists the claim of
  each press.
- Chat feed: `?feed=1&corpus=gestures` (the **corpus** select): the ten widgets in a scrolling
  list of bubbles. On a touch device, or with touch emulation in Chrome's device toolbar, a
  drag the chat takes really scrolls the feed.
- Node harness, the press report:
  `corepack pnpm tsx packages/core/scripts/sandbox-node.ts corpus/gestures/2-horizontal/04-joystick-game-x.html --width 360 --swipe 180,335,0,-60 --swipe 180,335,60,0 --hold 180,335`
  (the stick pad's centre at 360 px; 01: `180,317`).
- `corepack pnpm test:gesture-corpus` loads each widget in the sandbox and in reference mode
  (load, no JS errors, pixels within 3 %) → `docs/compat-report-gestures.md`; with
  `TW_RENDERER=shadow` on the Shadow DOM renderer.
- The Telegram test group: the **Gestures** topic of `scripts/conformance/publish-channel.ts`,
  not in the default run: `--topic gestures` (each widget's `idea:` line is its description).

Rules, as for `corpus/defaults`: pointer events and `setPointerCapture`, no libraries, no form
controls (out of profile 1), theme variables where a colour has a theme counterpart (the map,
the cards, the paper and the inks keep their own), at most 3800 B of gzip and 560 px of height
at a 320 px wide bubble.
