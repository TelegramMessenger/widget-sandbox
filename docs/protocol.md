# Sandbox ↔ host ↔ renderer protocol

Three trust domains exchange plain JSON messages:

```
sandbox (QuickJS, UNTRUSTED) ──ops / queries / listen / pd / raf / log / diag / bridge──▶ host (trusted)
sandbox ◀── events / timer & frame ticks / query results / viewport / theme / bridge ──── host
host ──validated RenderOps / queries / listen / preventDefault / requestFrame / frozen──▶ renderer (trusted code, sandboxed iframe)
host ◀── events / query results / size / resize / frame / media / violation ──── renderer
```

Type definitions live in `packages/core/src/shared/protocol.ts`. Nothing from the sandbox is
forwarded to the renderer without passing through `Validator`
(`packages/core/src/host/validator.ts`); the renderer additionally refuses forbidden tags and
attributes on its own (defense in depth).

## Node ids

- Every node that the sandbox wants rendered gets a **stable integer id** chosen
  by the sandbox (`> 1`, `< 2^31`). Id `1` is reserved for the Document and is
  only valid as the parent of the `<html>` element.
- An id may be reused only after the host has been told to remove it (`rm`).
  Creating an existing id is a protocol violation and the op is dropped.
- The validator keeps its own model (kind, tag, parent, children, rendered or
  not) and never trusts anything the sandbox claims beyond the op itself.

## Sandbox → host: `ops` (mutation batch)

`{ type: 'ops', ops: SandboxOp[] }` — applied in order, at most 200 000 per
batch. The sandbox flushes a batch at the end of every task (event, timer,
frame, script) and *before* every sync query.

| op | fields | meaning |
|---|---|---|
| `el` | `id, tag, ns?: 'svg', attrs?: {name: value}` | create element (detached) |
| `text` | `id, data` | create text node |
| `comment` | `id, data` | create comment (tracked, never rendered) |
| `attr` | `id, name, value` | set attribute |
| `rattr` | `id, name` | remove attribute |
| `data` | `id, data` | set text/comment data |
| `ins` | `parent, id, before: id \| null` | insert `id` under `parent` before `before` (null = append) |
| `rm` | `id` | remove node and forget its subtree |
| `style` | `id, name, value, important?` | set one inline style property (`value: ''` removes) |
| `prop` | `id, name, value` | set a DOM property: `value`, `checked`, `indeterminate`, `selectedIndex`, `selected`, `scrollTop`, `scrollLeft` |
| `call` | `id, method, args?` | call a method: `focus blur select scrollIntoView scrollTo scrollBy setSelectionRange setPointerCapture releasePointerCapture showModal show close reset showPopover hidePopover togglePopover observeResize unobserveResize` |
| `canvas` | `id, cmds: CanvasCmd[]` | replay 2D canvas commands on `<canvas id>` |

### Canvas commands

`CanvasCmd` is an array `[kind, ...]`:

| kind | shape | meaning |
|---|---|---|
| `c` | `['c', method, ...args]` | call a method from the allowlist (`fillRect`, `arc`, `drawImage`, …) |
| `s` | `['s', prop, value]` | set a state property (`fillStyle`, `font`, …); `filter` is CSS-sanitized and may not contain `url()` |
| `r` | `['r', width, height]` | resize the bitmap (resets state), 0…16384 |
| `g` | `['g', gid, 'linear'\|'radial'\|'conic'\|'pattern', ...args]` | create a gradient / pattern object `gid` |
| `gs` | `['gs', gid, offset, color]` | `addColorStop` |
| `pn` | `['pn', pid, svgPath \| null]` | create a `Path2D` |
| `pc` | `['pc', pid, method, ...args]` | call a path-building method on `Path2D pid` |

Arguments are numbers, short strings, booleans, `null`, flat number arrays, or
object references: `{g: gid}` gradient/pattern, `{p: pid}` Path2D,
`{n: nodeId}` an image source that must be a rendered `canvas`/`img`/`video`,
`{w, h, b64}` an `ImageData`.

### Features (opt-in wire forms)

`config.features` lists the optional forms a host understands; absent or
empty means plain JSON only, which every host, glue and renderer speaks, so
web, Android and desktop keep working unchanged. The shim uses a form only when
the feature is listed **and** the glue installed the matching function.

### Web Animations (`webAnimations`)

With `webAnimations` in `config.features`, `Element.animate`, `getAnimations`,
`document.timeline`, `document.getAnimations` and the `Animation`,
`KeyframeEffect`, `AnimationEffect`, `AnimationTimeline`, `DocumentTimeline`,
`AnimationPlaybackEvent`, `CSSAnimation` and `CSSTransition` globals run in the
shim (`packages/core/src/sandbox/waapi.ts`). Timing, events and promises are
computed in the sandbox; the renderer only plays what it is told. Without the
feature `Element.animate` returns a finished stub and reports `unsupported`.
It uses no new host function: the shim sends `__host('anim', json)` after the
tick's ops, and `getAnimations` asks the renderer with the `anims` query.

`anim` is a JSON array of records (max 4096 per batch, 4 MB of JSON):

- `{aid, remove: true}`: the animation left the renderer.
- Script animation: `{aid, target, keyframes: [{offset, easing, composite, values: {css-property: value}}],
  timing: {duration, delay, endDelay, iterations (null = infinite), iterationStart, direction, fill, easing},
  composite, iterationComposite, startTime, holdTime, playbackRate}`. `startTime` is on the
  sandbox clock (`now`), `holdTime` is a current time while paused or pending; one of them is set.
- CSS animation control: `{aid, target, css: {kind: 'animation'|'transition', name, origin}, startTime,
  holdTime, playbackRate, cancelled}`, for a CSS animation the widget paused, seeked or
  cancelled; `origin` is the renderer's id from the `anims` answer.

The host validates with `validateAnim` (`validator.ts`; the Swift native node
model has its twin in `NativeAnimValidator.swift`): `target` must be a rendered
element, at most 64 keyframes with sorted offsets in [0, 1] and 64 values each,
times finite and within 24 h, playback rate within ±1000, easings from the CSS
easing grammar, and at most 1024 live script animations (`animation-limit`).
Every keyframe value goes through the same CSS property sanitizer as
`element.style` (no `url()` other than `#fragment`, no `expression()`), and
`animation-*` / `transition-*` properties are dropped. Rejected records are
reported as `protocol-violation` and a live one becomes a `remove` record.
The renderer gets `{type: 'anim', now, anims}`, where `now` is the sandbox
clock at delivery so it can map `startTime` onto its own timeline.

### Canvas words (`canvasWords`)

The glue installs `__hostCanvas(key, words: Float64Array, strings: string)`
(captured by the bundle and deleted from the global object like `__host`).
Commands are written into one word buffer per canvas; at flush the `canvas` op
becomes a placeholder `{"op":"canvas","id":N,"bin":K}` in the JSON batch and
payload `K` is handed over first, right before the `ops` call:

```
words   = commands… , length(string 0) … length(string m-1), m
command = kind, name, ident, argc, (tag, payload)…
kind    = 0 c | 1 s | 2 r | 3 g | 4 gs | 5 pn | 6 pc
name    = string index (c, s, g, pc) or -1
ident   = gradient/path id (g, gs, pn, pc) or 0
tag     = 0 number | 1 string index | 2 {g:id} | 3 {p:id} | 4 {n:id}
        | 5 list (payload = n, followed by n numbers) | 6 null | 7 boolean
strings = the m strings joined, lengths in UTF-16 units
```

Command K maps 1:1 onto the JSON command array; non-finite numbers mean JSON
`null`. Anything the words cannot express (`ImageData`, other objects,
ill-formed UTF-16) turns that canvas back into JSON arrays for the frame. At
most 64 payloads per batch, 2 M words and 2 M string units per payload; the
glue validates payloads (the same rules as JSON commands) and drops malformed
ones. The Node harness decodes payloads back into JSON (`canvasWords: true`,
`decodeCanvasWords`); the Swift glue validates them natively.

## Host → renderer: `RenderOp`

Same shapes as above, after validation:

- `el` carries `attrs` as `[name, value][]` already filtered, plus
  `adopt: 'html' | 'head' | 'body'` the first time each of those appears (the
  renderer maps them to its own existing elements instead of creating new ones).
- `css { id, text }` replaces the text of a `<style>` element with the
  **sanitized** stylesheet (the sandbox never sends `<style>` text through
  `text`/`data` to the renderer; those nodes stay in the host model only).
- `ins` with `parent: 0` detaches a node but keeps it alive (used when a
  subtree moves under an unrendered parent).

### Typed delivery (Swift, in-process renderers)

A native renderer living in the host process may set `acceptsTypedOps` and
receive the same validated batch as `[WidgetOp]` through `send(ops:)`
(`WidgetSandboxProtocol`, a dependency-free target of the Swift package), so
neither side formats or parses JSON. `WidgetOp` has typed kinds for `el`,
`text`, `data`, `ins`, `rm`, `attr`, `rattr`, `style`, `css` and canvas word
batches; every other op stays a generic field map. It is a different encoding
of the same `RenderOp`s, not a protocol change: `WidgetOp.json` renders the
batch back to this document's JSON, and the default `send(ops:)` sends exactly
that as `{type:'ops'}`, so the WebView renderer and every other port are
unaffected.

### Validation rules (summary — see `allowlist.ts`, `validator.ts`, `css-sanitizer.ts`, `url-policy.ts`)

- Tags: allowlist of HTML and SVG elements. `script iframe frame frameset
  object embed link meta base portal noscript template set applet param track
  area map math …` are dropped with their subtrees (tracked in the model so
  later ops referring to them are consistent). Unknown elements render as
  `<span data-tg-tag="…">`.
- Media: `audio video source track` are dropped with their subtrees, even with
  `data:` sources (no codec parser ever sees widget bytes; no autoplay).
- Attributes: every `on*` is dropped. URL-carrying attributes are allowed only
  for `img/input[src]`, SVG
  `image/feImage[href]` (with `data:` of an allowed image MIME) and SVG
  `use/linearGradient/radialGradient/pattern/filter/textPath/mpath[href]`
  (fragment only). `a[href]` keeps fragments; any other value becomes
  `href="#" data-tg-link` and the real URL stays in the sandbox (`openLink`).
  `srcset`, `ping`, `action`, `formaction`, `background`, `data`, `srcdoc`, …
  are never forwarded.
- SVG `animate*`: `attributeName` must not name an URL attribute; `from/to/by/values`
  are CSS-sanitized. `<set>` is dropped.
- CSS: parsed with css-tree, re-generated from the AST; `@import`, `@namespace`,
  `@font-face` (system fonts only: `font-family` can name only installed
  fonts) and unknown at-rules dropped; `url()`, `image-set()`, `image()`,
  `src()`, `cross-fade()` only with `data:` images or a fragment;
  `var()`/`env()`/`attr()` inside URL-taking functions rejected.
- Sizes: ≤ 50 000 nodes, text ≤ 2 MB, attribute ≤ 4 MB (data: URLs), batch ≤
  200 000 ops, canvas ≤ 500 000 commands.

### `icon:` images (Swift hosts, `WidgetHost.Options.assetResolver`)

A host that knows a built-in icon set passes `assetResolver: (String) -> String?`
(the Telegram app uses the twidget kit's `twa_icon_data_url`). Before validation,
`TrustedValidator` replaces `icon:<name>[?query]` tokens with the resolver's answer: a
whole JSON string value under any key except `data` and `text`, and `url(icon:…)` anywhere,
in JSON batches and in binary op fields alike (`IconExpansion.swift`). The resolver only
sees tokens of the grammar `icon:[A-Za-z0-9_.-]{0,96}(?:\?[A-Za-z0-9=&.%#_-]{0,400})?`.
An answer is used only if it is a `data:image/` URL of at most 64 KB made of
`[A-Za-z0-9-._~,/:=%;+]`, so it can never become a fetchable URL or break out of a
JSON string or a CSS `url()`. A batch expands at most 1024 tokens and 2 MB
(`icon-limit`, once per batch). The result is validated like any other `data:`
image. Without a resolver `icon:` URLs are blocked like every other non-`data:` URL.

## Sync queries

The sandbox posts `{ type: 'query', query }` and blocks with `Atomics.wait` on
the shared buffer (`SAB_STATE` word: 0 idle, 1 waiting, 2 done). The host
validates the query, relays it to the renderer with a `qid`, writes the
answer (UTF-8 JSON, optional binary payload for `getImageData`) into the buffer,
sets state 2 and `Atomics.notify`s. Timeout: 3 s (answer `{error:'timeout'}`).

| query | answer |
|---|---|
| `rect {id}` | DOMRect JSON |
| `rects {id}` | DOMRect[] |
| `box {id}` | `offsetWidth/Height/Top/Left, offsetParent (id), clientWidth/Height/Top/Left, scrollWidth/Height/Top/Left` |
| `cs {id, pseudo?, prop?}` | all computed properties as `{name: value}`, or one custom property |
| `svg {id, method, args}` | `getBBox`, `getCTM`, `getScreenCTM`, `getComputedTextLength`, `getTotalLength`, `getPointAtLength`, `getSubStringLength`, `getNumberOfChars` |
| `measureText {font, text}` | TextMetrics JSON |
| `imageData {id, x, y, w, h}` | `{width, height}` + binary RGBA |
| `canvasCall {id, method, args}` | `isPointInPath`, `isPointInStroke`, `getTransform`, `getLineDash`, `toDataURL` |
| `elementFromPoint {x, y}` | node id or 0 |
| `validity {id}` | ValidityState JSON + `message` |
| `matchMedia {query}` | `{matches}` — `prefers-color-scheme` is rewritten by the host against the chat theme before the renderer evaluates the query |
| `viewport` | `{innerWidth, innerHeight, dpr}` |
| `anims {id, subtree}` | `{list: [{kind, id, name, index, origin, currentTime, playbackRate, paused, duration, delay, iterations, direction, fill, easing}]}`: running CSS animations and transitions (`webAnimations`) |

Answers are cached in the sandbox until the next flush that sent ops. The
diagnostics panel shows query count, total latency and cache hits.

## Events (renderer → host → sandbox)

The renderer listens at document level (capture) and forwards an event only if
the sandbox declared interest in its type (`listen`) or it is one of
`click input change submit reset load error toggle`. Move-type events are
throttled to animation frames.

```ts
interface WidgetEvent {
  type; targetId; relatedId?;
  clientX/Y, offsetX/Y, pageX/Y, movementX/Y, button, buttons, detail,
  pointerId, pointerType, isPrimary, pressure, deltaX/Y/Mode,
  key, code, repeat, modifiers (bit0 alt, bit1 ctrl, bit2 meta, bit3 shift),
  value, checked, selectedIndex, inputType, data, submitterId,
  naturalWidth/Height (img load), open (details toggle), scrollTop/Left
}
```

`value`/`checked`/`selectedIndex` of the target form control travel with every
event, which is how the sandbox mirror of control state stays current.

### preventDefault

`preventDefault()` in the sandbox runs after the renderer has already
dispatched the event, so it cannot cancel native defaults. The renderer
cancels navigation and form submission unconditionally. For everything else the
sandbox reports keys `"<targetId>:<type>"` (or `"<id>:keydown:<key>"`) of events
it prevented (`pd` message); the renderer cancels those synchronously from the
next occurrence on ("learned preventDefault"). See `docs/decisions.md`.

### Touch gestures inside a scrolling host

On every primary `pointerdown` the renderer also posts
`{ type: 'gesture', wants }` — `wants` is true when an ancestor of the target
has a computed `touch-action` that forbids vertical panning (`none`, `pan-x`,
…), is a natively draggable control (`input[type=range]`), is a vertically
scrollable box with overflowing content, or the sandbox has learned to
prevent `pointerdown` / `touchstart` on that target. It exists for hosts whose renderer view sits inside a scrolling
container that cannot see into it: the Android host denies the container's
touch interception from `ACTION_DOWN` and releases it on `wants: false` (or
after 150 ms without an answer). The web and iOS hosts ignore the message —
the browser and WebKit resolve `touch-action` against the scroll gesture
themselves.

## Host ⇄ worker envelope

Host → worker: `init {html, sab, config, sandboxBundle}`, `event {ev}`,
`frame {time}`, `viewport`, `resize {entries}`, `theme`, `bridge {event, data}`,
`visibility {visible}`.

Host → renderer, besides ops and queries: `listen {types}`, `preventDefault
{keys}`, `requestFrame`, `anim {now, anims}` (`webAnimations`), and `frozen {reason, fatal}` — the runtime is gone
(or paused): the renderer keeps the DOM as it is, cancels the pending frame,
stops forwarding events, sets `data-tw-frozen="fatal"|"paused"` on `<html>`
(`pointer-events: none`, a slight grayscale when fatal) and shows a small badge
with `reason` (a renderer element, never in the widget's node table). The host
sends it from its death path; the iframe / WKWebView / WebView stays on screen
until the owner calls `destroy()`.

Worker → host (payload untrusted unless noted): `ops`, `query`, `listen
{types}`, `pd {keys}`, `anim {anims, now}` (`now` from the glue), `raf`, `log {level, text}`, `diag {kind, detail}`,
`bridge {method, args}`; trusted glue: `loaded`, `stats`, `dead {reason}`.
Diagnostic kinds added by the runtime limits: `timer-limit` (shim, once),
`timer-limit-enforced` (glue backstop — a shim bug), `cpu-throttled` (glue, at
most once per second).

Inside the worker the QuickJS context has exactly one capability,
`__host(kind: string, arg: string): string | number | undefined`, with kinds
`ops query now timer clearTimer raf log diag listen pd anim bridge intl random stat`,
plus `__hostBinary()` to fetch the binary part of the last query answer.

### User activation for privileged bridge calls

`bridge` methods other than `ready` (`openLink`, `sendMessage`, anchors the
shim turns into `openLink`) are privileged: the glue forwards one only while
the task of a trusted user gesture runs on a visible widget, and one gesture
unlocks one call. A gesture is one physical press (`pointerdown`, `mousedown`,
`pointerup`, `mouseup`, `click`, `dblclick` of the same press) or one key press
(`keydown`, not Escape, not a repeat, not a Command shortcut), as reported by
the renderer; the host numbers gestures and hands them to the engine with the
event. Calls from timers, frames, promises after the task, scripted
`click()`/`dispatchEvent` or a hidden widget are dropped with an
`activation-blocked` diag (at most 20). On the web the main-thread host
decides: the renderer's `event` message carries `trusted` (the DOM event's
`isTrusted`), the host's `event` to the worker carries `gesture: n` for
trusted gestures, the worker tags `bridge` messages made in that task with
`gesture`, and the host accepts each issued gesture once, within 5 s. Native
glue gates in the engine (`activationEvent(json, gesture)`). See decisions.md,
"User activation".

### `config` (SandboxConfig, same fields on iOS and Android)

| field | default | meaning |
|---|---|---|
| `memoryLimitBytes` | 64 MB | QuickJS heap limit; exceeding it kills the runtime |
| `cpuLimitMs` | 500 | longest continuous run of widget JS in one task (interrupt handler); exceeding it kills |
| `cpuQuotaMs` / `cpuQuotaWindowMs` | 300 / 1000 | JS time the widget may spend per sliding window (ten buckets); beyond it timers, frames, resize/viewport/theme/bridge tasks are **deferred**, never killed; events always run with at least 50 ms |
| `cpuKillAfterMs` | 0 (off) | kill once throttling has lasted this long without a gap longer than one window |
| `maxTimers` | 256 | live `setTimeout` + `setInterval` slots; the shim throws `RangeError` past it, the glue refuses as a backstop; timer delays are clamped to 24 h |
| `visible` | true | initial visibility (see `visibility` below) |
| `viewport`, `theme` | | as before |

The policy that counts and decides lives in trusted glue only:
`packages/core/src/host/cpu-budget.ts`, `ios/.../CpuBudget.swift`, `android/.../CpuBudget.kt`
(one fixture, `packages/core/tests/fixtures/cpu-budget.json`). The shim gets `maxTimers`
through `boot` and the visibility signal; it enforces nothing the host relies
on. Before each task the glue asks `decide(kind, now)`: `{run, sliceMs}` sets
the interrupt deadline (`min(cpuLimitMs, max(remaining, 50))`, `boot` always
gets `cpuLimitMs`), `{run: false, retryAt}` puts the task in the deferred
queue (frames collapse into one, one fire per timer id, order kept) and a
single wake-up timer. A task that overruns its slice is reported as
`CPU limit exceeded: more than N ms … (…; CPU quota: … left, so this timer
task got N ms)`. Diagnostics: `cpu-throttled` at most once per second;
`stats.throttledTasks`, `stats.cpuWindowMs`.

### `visibility {visible}`

The host knows whether the bubble is on screen (`WidgetHost.setVisible`); the
sandbox does not. Hidden: the glue requests no frames from the renderer (one
is requested on return if the shim asked meanwhile; a frame already in flight
is dropped), timers fire at most once per second (new timers are armed with
`max(delay, 1000)`, due ones are batched into one wake-up; intervals do not
accumulate missed ticks), events pass through as usual. The shim receives
`visibility` through `__twDispatch` and flips `document.hidden` /
`document.visibilityState` and dispatches `visibilitychange` on the document,
like a background tab (no `pagehide`/`pageshow`).

## Shared buffer layout

```
Int32[0]  state (0 idle, 1 waiting, 2 done)   ← Atomics.wait / notify
Int32[1]  json byte length
Int32[2]  binary byte length
Int32[3]  reserved
bytes[16…]  json, then binary (16 MB total)
```

## Mapping to MCP Apps later

The bridge (`TelegramWidget.*` ↔ `bridge` messages) is a thin method/args
envelope; wrapping it as JSON-RPC 2.0 over the same channel (`ui/initialize`,
`ui/notifications/*`, `tools/call`) is a renaming exercise — the sandbox
already cannot reach `postMessage` directly, so the host would be the JSON-RPC
endpoint.
