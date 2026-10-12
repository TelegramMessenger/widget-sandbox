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
{keys}`, `requestFrame`, and `frozen {reason, fatal}` — the runtime is gone
(or paused): the renderer keeps the DOM as it is, cancels the pending frame,
stops forwarding events, sets `data-tw-frozen="fatal"|"paused"` on `<html>`
(`pointer-events: none`, a slight grayscale when fatal) and shows a small badge
with `reason` (a renderer element, never in the widget's node table). The host
sends it from its death path; the iframe / WKWebView / WebView stays on screen
until the owner calls `destroy()`.

Worker → host (payload untrusted unless noted): `ops`, `query`, `listen
{types}`, `pd {keys}`, `raf`, `log {level, text}`, `diag {kind, detail}`,
`bridge {method, args}`; trusted glue: `loaded`, `stats`, `dead {reason}`.
Diagnostic kinds added by the runtime limits: `timer-limit` (shim, once),
`timer-limit-enforced` (glue backstop — a shim bug), `cpu-throttled` (glue, at
most once per second).

Inside the worker the QuickJS context has exactly one capability,
`__host(kind: string, arg: string): string | number | undefined`, with kinds
`ops query now timer clearTimer raf log diag listen pd bridge intl random stat`,
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
`activation-blocked` diag (at most 20). Swift glue only so far
(`SandboxEngine.activationEvent`, `WidgetHost`); the worker, Node harness,
Kotlin and C++ copies still forward every bridge call.

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
