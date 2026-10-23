# gestures — what breaks if widgets cannot claim vertical drags

Widgets for one question: **should vertical drags over a widget always scroll the chat, so
that widgets can claim only horizontal drags?** The set shows what would break. Three games
come in pairs: the same widget on `touch-action: none` (`…-xy`, it claims both axes) and on
`pan-y` (`…-x`, horizontal only, which is what the rule would leave it). The two files of a
pair differ in one CSS value and their first line. A widget can restrict itself with
`touch-action` today, so the "only horizontal" side can be tried without a framework change.

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
| `1-pairs/01-joystick-xy.html` | `none` | Star catcher: fly a dot with a virtual stick, collect stars | start up or down, circle, rest your thumb on the stick | 1837 B | 435 px |
| `1-pairs/02-joystick-x.html` | `pan-y` | the same game | the same; then start sideways and steer up | 1899 B | 435 px |
| `1-pairs/03-maze-xy.html` | `none` | Ball maze: drag the ball to the flag, a wall sends it back; a timer and the best time | finish it | 2616 B | 383 px |
| `1-pairs/04-maze-x.html` | `pan-y` | the same maze | try to leave the start | 2639 B | 383 px |
| `1-pairs/05-2048-xy.html` | `none` | 2048: swipe in four directions, equal tiles merge; score, best and the game are kept | play a few moves | 2345 B | 406 px |
| `1-pairs/06-2048-x.html` | `pan-y` | the same game | swipe up or down | 2354 B | 406 px |
| `2-one-axis/07-paddle-x.html` | `pan-y` | Brick breaker: slide anywhere to move the paddle, tap to serve | play it | 2085 B | 353 px |
| `2-one-axis/08-wheel-picker-y.html` | `pan-x` | an alarm time picker: hour and minute wheels with momentum and snap | spin, flick, tap a number; start sideways | 2214 B | 294 px |
| `probe-transform.html` | — | the transform probe (below) | open it on your client | 1563 B | 527 px |

## What "only horizontal" would change

- **Joystick (01 → 02).** The first move decides. A thumb on a stick rarely starts sideways on
  purpose: aiming up loses the press to the chat (the stick snaps back, the chat scrolls);
  a diagonal works only when its horizontal part is the larger. Once a press is the widget's,
  up and down drift reaches it: nothing is taken away mid-press. `pan-y` also gives holds to
  the chat: a thumb resting on the stick for half a second opens the message menu. The rule as
  stated says nothing about holds, and CSS cannot express "horizontal plus holds", so 02 is the
  rule plus losing holds.
- **Maze (03 → 04).** The start cell opens only downwards. On 04 a press that starts down
  scrolls the chat, and one that starts sideways moves the ball into the wall first: a
  corridor leaves the ball about 6 px either way, and a press must move more than 7 px
  sideways before it counts as horizontal. The maze cannot be left from its start; anywhere
  else, a press can only begin at a junction and must never be lifted in a vertical corridor.
- **2048 (05 → 06).** Up and down swipes scroll the chat, so only left and right move the tiles
  and the board fills up. The author would have to add buttons for up and down.
- **Paddle (07).** Horizontal only is all it needs: unchanged by the rule. (`pan-y` still
  gives a resting thumb to the message menu.)
- **Wheel picker (08).** A vertical control: under the rule it cannot be spun at all, every drag
  scrolls the chat. The author would have to turn it on its side or replace it with buttons.
- Drawing on a canvas and panning a map are 2D in the same way as the joystick and the maze.

## The transform probe

The card carousel of an earlier version of this set (it moved its track with
`transform: translateX(calc(11% - i * (78% + 10px) + dx px))` and snapped to a card through a
`transition` on `transform`) did not move on the iOS and Android Telegram clients, while the
paddle, also on `pan-y`, worked there: the claim works, and the paddle moves with `left`.
`probe-transform.html` shows a client team which forms of a transform its renderer draws. Every
row moves a box onto its dashed outline 120 px in:

- `left: 50%`, the control;
- `transform: translateX()` in px, in %, with `calc(% + px)` and with the carousel's
  `calc()` (a `*` and nested parentheses), and the `translate` property, all from the
  stylesheet;
- the same four `translateX()` forms set from script (`style.transform`);
- the same four set from script after a class adds `transition: transform .3s`, the way the
  carousel snapped.

A box short of its outline is a form that renderer does not draw. The ✓ / ✗ after a row is
where the renderer's own layout puts the box (`getBoundingClientRect`): a renderer may draw a
transform and still report the box where it was, so judge by the picture.

On the web renderers (dev app, 2026-10-23):

| Renderer | stylesheet | from script | through a transition | layout (✓ / ✗) |
|---|---|---|---|---|
| iframe, shadow (Chromium) | all | all | all | 14 / 14 |
| Blitz (wasm prototype) | all | all | **none**: the box stays at the start | only the control: Blitz's layout ignores transforms |
| litehtml (wasm prototype) | **none** | **none** | **none** | only the control |

So the carousel fails on a renderer without transforms (litehtml: the dot moves on, the track
does not) and on one that does not apply a transform through a transition (Blitz: the track
stays where the finger left it). Which one the Telegram clients have is for their teams to
check with the probe. The conformance set covers the same forms since this change:
`auto/09-transforms-effects-a` checks `translateX(%)`, `calc()` in `translateX()` (with `*` and
nested parentheses too) and a transform set from script; `visual/05-transforms-effects-a` draws
them for the reference picture.

The gesture widgets here move things with `left` and `top` only, never with a transform, so they
test gestures, not the renderer's transforms.

## Notes for authors

- A tap's `click` goes to the element with the pointer capture, not to the child that was
  pressed: the wheel picker remembers the number pressed at `pointerdown`.
- A control that reacts to the first pixels of a move changes before the policy has decided;
  when the chat takes the press (`pointercancel`), put the value back or end the press cleanly:
  the stick re-centres, the wheel snaps back.

## How to try them

- Dev app (`corepack pnpm dev`): the `gestures/` group in the corpus list, or
  `?file=gestures/1-pairs/04-maze-x.html&width=320`. The **gestures** select is `phone` by
  default: the mouse plays a finger and the bubble shows what it took (`↕ list scroll`,
  `↩ swipe: reply`, `⋯ message menu`). `&blitz=1` / `&litehtml=1` for the probe on the wasm
  renderers (`pnpm build:blitz-web`, `pnpm build:litehtml-web`).
- Chat feed: `?feed=1&corpus=gestures`. On a touch device, or with touch emulation in Chrome's
  device toolbar, a drag the chat takes really scrolls the feed.
- Node harness, the press report:
  `corepack pnpm tsx packages/core/scripts/sandbox-node.ts corpus/gestures/1-pairs/04-maze-x.html --width 360 --swipe 180,201,0,-60 --swipe 180,201,60,0 --hold 180,201`.
- `corepack pnpm test:gesture-corpus` loads the eight widgets in the sandbox and in reference
  mode (no JS errors, pixels within 3 %) → `docs/compat-report-gestures.md`; with
  `TW_RENDERER=shadow` on the Shadow DOM renderer. The probe is not in it: it measures itself.
- The Telegram test group: the **Gestures** topic of `scripts/conformance/publish-channel.ts`,
  not in the default run: `--topic gestures` (the eight widgets and the probe; each file's
  `idea:` line is its description).

Rules: pointer events and `setPointerCapture`, no libraries, no form controls (out of profile
1), theme variables where a colour has a theme counterpart, at most 3800 B of gzip and 560 px
of height at a 320 px wide bubble.
