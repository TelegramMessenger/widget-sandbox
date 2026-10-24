# Writing widgets

*The guide for widget authors and the models that write widgets for them
(v0.1.2, widget profile 1, SDK v1). The exact contracts: the profile feature by
feature in `docs/profile-v1.md` (generated from `packages/core/profile.json`),
the `TelegramWidget` API in `docs/sdk-v1.md` §2, the payload in
`docs/tgw-format.md`. A block for a model's system prompt is at the end, and
`sandbox-node.ts --guide` prints it.*

## In one paragraph

A widget is **one self-contained HTML document** (or a bare fragment) with
inline CSS and JavaScript, sent by a bot in a message and drawn inside the
chat bubble. It has **no network access of any kind** and cannot navigate; no
library is bundled. It is drawn the same way on iOS, Android, macOS, desktop and
the web because every client removes what is outside the profile before it
reaches the renderer. The bubble is **200–600 px wide** (phones lay it out at
about 320–430) and at most 600 px tall; what does not fit is clipped, and
nothing scrolls inside a widget.

## Size: mini-HTML

- At most **32 768 bytes of HTML** and **4096 bytes after gzip**. The server
  gzips at the default level (6) and refuses anything larger with
  `INPUT_MEDIA_WIDGET_TOO_LARGE`. Keep a margin: aim for 3800 B.
- Measure: `gzip -6 -n < widget.html | wc -c`, or let the harness print it
  (`sandbox-node.ts widget.html`, the line `size:`), or read the dev app's
  stats panel.
- What costs bytes: `data:` images barely compress; repeated inline styles and
  long class names compress well; SVG paths are cheap until they are long.

## The bubble

- **Size.** The bot sends `width` and `height` with the widget (`0` = auto, each
  axis on its own). Width `0`: the bubble's width; `W`: a fixed width clamped
  to [200, the bubble's maximum]. Height `0`: the content height clamped to
  [50, 600]; `H`: a fixed height clamp(H, 50, 600) that the content does not
  change. Above the height the content is clipped hard: no inner scrolling, no
  fullscreen. Lay out with `%`, flex or grid; never with `vh` (out of the
  profile).
- **The bubble is the canvas.** The widget view fills the bubble with no
  padding of its own, and the bubble clips it at its rounded corners. `html`
  has no background: the bubble's colour (`--tg-theme-bg-color`) shows through,
  and a background you set on `html` or `body` paints the whole bubble.
- **Defaults** (the renderer's own layer, `docs/sdk-v1.md` §2.4): the text is the
  client's message text (`--tg-text-font-size`, `--tg-text-line-height`), in
  `--tg-theme-text-color`; `body` has the client's text padding as its margin
  (`--tg-text-padding`, about `8px 12px`), so `body { margin: 0 }` goes edge to
  edge; headings, paragraphs, lists, tables, `pre`, quotes get native sizes
  and margins; a `<button>` you do not style looks like the client's own. Any
  CSS of yours wins over these defaults.
- **Theme.** The chat's palette is on the root as `--tg-theme-bg-color`,
  `--tg-theme-text-color`, `--tg-theme-hint-color`, `--tg-theme-link-color`,
  `--tg-theme-button-color`, `--tg-theme-button-text-color`,
  `--tg-theme-secondary-bg-color`, … and `--tg-color-scheme`; in JS
  `TelegramWidget.themeParams` and `colorScheme`. `prefers-color-scheme`,
  `color-scheme: light dark` and `light-dark()` follow the **chat** theme, live.
- **Text is not selectable** unless you say `user-select: text` (and
  `-webkit-user-select: text`) on what a user may want to copy.

## Input: who gets a press

Taps (`click`) and pointer events (`pointerdown`/`move`/`up`/`cancel`,
enter/leave/over/out, `setPointerCapture`) reach the widget, with the mouse
compatibility events. **Taps and double taps are always the widget's**, on every
element: a double tap arrives as both clicks and then `dblclick` (the chat's
reaction stays on the bubble outside the widget). **Vertical drags are always
the chat's**: the chat scrolls over every widget, whatever `touch-action` says.
Holds and horizontal drags are claimed **per element with CSS `touch-action`**,
decided when the finger goes down:

| over an element with | hold | horizontal drag | vertical drag |
|---|---|---|---|
| `auto` (the default) | the chat's message menu | swipe-to-reply | the chat scrolls |
| `pan-y` | menu | **the widget's** | the chat scrolls |
| `none` | **the widget's** (+ `contextmenu`) | **the widget's** | the chat scrolls |

`pan-x` claims nothing (it is `auto` here). The first move past 10 px decides
for the whole press, also after a hold: a drag that starts sideways on a
claimed element is the widget's on both axes until the finger lifts; one that
starts up or down is the chat's. When the chat takes a press, the widget gets
`pointercancel` and no `click`. The leading 20 px of the widget never take a claim (the system back
gesture), and the bubble outside the widget stays the chat's. With a mouse, a
right click follows the same claim; clicks, double clicks, holds and drags are
the widget's. Thresholds: a hold is 500 ms within 10 px, a double tap 300 ms and
40 px apart.

**Rule of thumb: a button you hold, a game control, a canvas → `touch-action:
none` on it; a horizontal slider, carousel or paddle → `pan-y`; everything else
stays `auto`**, so people keep the chat's menu and reply over the rest of the
widget. Nothing vertical can be dragged: no vertical sliders, faders or wheel
pickers, no up/down swipes, no 2D joysticks, drawing or map panning. Lay such
controls out sideways or use taps (buttons for up and down). When the chat takes
a press, undo what `pointerdown` started (put the value back, re-centre the
stick). `touch-action` on a container covers its children, as in CSS. Keep
controls at least 20 px from the widget's leading edge.

No keyboard and no focus at all: no key events, no text input, `focus()` does
nothing. No touch events (use pointer events), no wheel or scroll events.

## What works

- **Markup:** text, headings, lists, tables, `<details>`/`<summary>`, `<a>`,
  `<img>` (`data:` PNG, JPEG, GIF, WebP), `<canvas>`, `<button>`, inline SVG
  (shapes, paths, text, `<use>`, gradients). `<form>` renders as a block, its
  controls are removed.
- **CSS:** flexbox, grid, tables, positioning (`fixed` is inside the bubble),
  box model, borders, radii, shadows, linear, radial and conic gradients, 2D
  transforms, `opacity`, `filter` / `backdrop-filter` with `blur()`,
  `brightness()`, `drop-shadow()`, transitions and `@keyframes`, custom
  properties, `calc()`/`min()`/`max()`/`clamp()`, `:hover` (desktop only),
  `:active`, structural pseudo-classes, `::before`/`::after`,
  `text-overflow: ellipsis`, `font-variant-numeric`, `touch-action`,
  `pointer-events`, `user-select`, `@media` on the width,
  `prefers-color-scheme`, `prefers-reduced-motion`, `@supports`. Fonts:
  `system-ui`, `sans-serif`, `serif`, `monospace`, `ui-*` only.
- **JavaScript:** ES2023, Promises, timers, `requestAnimationFrame`, the DOM
  (`querySelector`, `innerHTML`, `classList`, `style`, observers), layout reads
  (`getBoundingClientRect`, `getComputedStyle`, `offsetWidth`, `matchMedia`,
  `measureText`), canvas 2D, Web Animations (`element.animate`), `Intl` in the
  client's locale (`navigator.language`), `crypto.getRandomValues`,
  `TextEncoder`, `URL`, `structuredClone`, `localStorage` / `sessionStorage`.

## Out of the profile

Removed by every client before rendering: a declaration, a selector, an
element or an event that is out never reaches the renderer, the rest of the
widget still works. Detect anything newer with `CSS.supports`, `@supports`
or `TelegramWidget.isProfileAtLeast(n)`. `fetch`, `XMLHttpRequest`,
`WebSocket` and the like throw `NetworkDisabledError`; external `<script src>`,
stylesheets, fonts and images are never loaded. The full list, generated
from the profile:

<!-- profile:summary -->
*Generated from `packages/core/profile.json` (profile 1): 257 features, 178 in, 79 out; each one with how it is removed in `docs/profile-v1.md`.*

- **HTML elements and attributes** (20 in, 14 out): ruby, rt, rp, bdi, bdo · Legacy presentational elements: center, font, big, tt, strike, nobr · &lt;img src="data:image/svg+xml"> · &lt;img> with data: AVIF, BMP, ICO · &lt;img src="https://…"> · Form controls: form, input · progress, meter · dialog · audio, video, source, track · script · align, valign, bgcolor, border, cellpadding, cellspacing, color, face, nowrap · contenteditable, spellcheck, inputmode, enterkeyhint, autocapitalize · draggable · tabindex, autofocus, accesskey
- **Inline SVG** (5 in, 5 out): clipPath, mask, pattern, marker · SVG filter primitives · SMIL animation: animate, animateTransform, animateMotion · foreignObject · &lt;image href="data:…"> inside SVG
- **CSS** (54 in, 22 out): display: flow-root, contents, list-item · Logical properties: margin-inline, padding-block, inset-inline, inline-size, … · Multi-column layout · position: sticky · overflow: auto / scroll · Colour functions: hwb(), lab(), lch(), oklab(), oklch(), color() · color-mix() · background-clip: text · url() / image-set() with anything but a data: image or #fragment · font-family naming a font · @font-face · -webkit-line-clamp / line-clamp · text-shadow · writing-mode, text-orientation · 3D transforms: rotateX/Y/3d, translateZ/3d, scaleZ/3d, matrix3d, perspective, transform-style, backface-visibility · filter: grayscale(), sepia(), saturate(), contrast(), hue-rotate(), invert(), opacity() · filter: url(#svg-filter) · mix-blend-mode, isolation, background-blend-mode · clip-path: inset(), circle(), ellipse(), polygon() · mask / -webkit-mask with gradients or data: images · vh / dvh / svh / vb: height-relative units · env()
- **Selectors** (7 in, 6 out): :has() · ::first-letter, ::first-line, ::marker · :focus, :focus-visible, :focus-within · :checked, :disabled, :enabled, :valid, :invalid, :placeholder-shown, ::placeholder · :host, :host-context(), ::slotted(), ::part(), :scope · :target
- **At-rules** (5 in, 8 out): @media · @media print, resolution, other media features · @import, @namespace · @container queries · @layer cascade layers · @property · @counter-style, @starting-style, @scope, @page, @font-feature-values, @font-palette-values, @position-try · CSS nesting
- **Animation** (7 in, 1 out): Magic move
- **Canvas 2D** (13 in, 8 out): createConicGradient · createPattern from a canvas or image · globalCompositeOperation · shadowBlur, shadowColor, shadowOffsetX/Y · ctx.filter · getImageData / putImageData / createImageData · toDataURL, toBlob, captureStream · WebGL, OffscreenCanvas, bitmaprenderer contexts
- **JavaScript runtime** (25 in, 9 out): element.focus() / blur() / select(), document.activeElement: the widget takes keyboard focus · Bundled libraries by CDN name: Chart.js 4, D3 7 · fetch, XMLHttpRequest, WebSocket, EventSource, RTCPeerConnection, sendBeacon, importScripts, Worker, BroadcastChannel · location assignment, history, window.open · alert, confirm, prompt, print · clipboard, share, vibrate, geolocation, mediaDevices, Notification · attachShadow, customElements.define · Blob, FileReader, URL.createObjectURL · requestFullscreen
- **TelegramWidget (SDK v1)** (10 in, 1 out): TelegramWidget.sendMessage
- **Input** (9 in, 5 out): touchstart / touchmove / touchend · Key events: keydown / keyup / keypress, beforeinput / input from keys, focus / blur / focusin / focusout from keys · Enter / Space activate a focused button or link · A key press counts as a user gesture for privileged bridge calls · wheel and scroll events
- **Limits** (11 in, 0 out)
- **Client behaviour** (12 in, 0 out)
<!-- /profile:summary -->

## `TelegramWidget` in brief

```js
TelegramWidget.profile;                      // 1; isProfileAtLeast(n)
TelegramWidget.colorScheme;                  // 'light' | 'dark', the chat's
TelegramWidget.themeParams.bg_color;         // the palette, '#rrggbb' (Mini Apps keys)
TelegramWidget.onEvent('themeChanged', fn);  // also 'activated' / 'deactivated'; isActive
TelegramWidget.openLink('https://…');        // in a tap handler: the user confirms the full URL
TelegramWidget.openTelegramLink('https://t.me/…');
TelegramWidget.HapticFeedback.impactOccurred('light'); // notificationOccurred, selectionChanged
TelegramWidget.ready();                      // optional: first state drawn
```
TelegramWidget.setResizeAnchor('bottom');    // v0.1.3: grow upward ('top', the default: downward)

- **Links:** `https:` only, at most 256 characters, one call per tap, only in
  the handler of a real tap (not from a timer or a promise); a bad URL throws
  `TypeError`. A tapped `<a href="https://…">` does the same. The widget is not
  told whether the user opened it.
- **Storage:** `localStorage` is kept for this message on this device, there
  from the first line of script, 16 KB per area (`QuotaExceededError` past
  it: catch it); `sessionStorage` until the app restarts. A forwarded copy
  starts empty; a bot edit with new HTML restarts the widget on the same
  storage.
- **Haptics:** no tap needed, only while visible, 5 in a burst then 10 per
  second; desktop and web play nothing.
- **Gone:** `sendMessage` (nothing is sent to the chat or the bot), `alert` /
- **Resize anchor** (v0.1.3): when your height changes, the client keeps one
  edge of the bubble still on screen. `'top'` (the default) grows and shrinks
  downward: answers that expand, "show more", a summary that opens.
  `'bottom'` keeps the bottom still and grows upward: a chat log or thread where
  new lines appear at the bottom and older ones move up. Call it before the
  change, at the start of the script if the widget always grows one way; it
  applies until you call it again. Only `'top'` and `'bottom'` (anything else
  throws `TypeError`); older clients lack it:
  `if (typeof TelegramWidget.setResizeAnchor === 'function') …`.
  `confirm` / `prompt` (no dialog), clipboard, share, fullscreen.
- Outside Telegram (a plain browser) `window.TelegramWidget` is undefined:
  check for it.

## Lifecycle and limits

- Off screen: `document.hidden`, no animation frames, timers at most once per
  second. The widget can be destroyed and recreated at any time; only Web
  Storage survives: keep the user's choices in `localStorage`.
- CPU: 300 ms per second (timers and frames wait beyond that), 500 ms in one
  task; 64 MB of memory; 256 live timers; 50 000 nodes. A widget over a limit
  or crashed keeps its last frame with a "Widget stopped" badge.
- Keep the first render synchronous and fast; animate with
  `requestAnimationFrame`, never read layout in every frame.

## How to test

- **The harness** (no browser window, prints text for people and agents):
  `corepack pnpm tsx packages/core/scripts/sandbox-node.ts widget.html` runs the
  widget in the real sandbox and prints the size, what the profile removed
  (`diag` lines), errors and the actions it asked for. Add `--width 360` to lay
  it out in the real renderer at that bubble width (the content height), and
  presses to check gestures: `--tap x,y`, `--hold x,y[,ms]`,
  `--double-tap x,y`, `--swipe x,y,dx,dy`, `--right-click x,y` (`--desktop`
  for a mouse client). Each press reports the element, its claim, who got it,
  what the widget received and a hint, for example
  `hint: the hold went to the message menu and the widget got pointercancel: set touch-action: none on button#fire …`
  or `hint: vertical drags always scroll the chat; a widget cannot claim them …`.
  `--json` gives the whole result.
- **The dev app** (`corepack pnpm dev`): drop the file, pick a width (200–600 or
  free), toggle dark, and use the mouse as a finger (`gestures: phone`, the
  default): the bubble takes holds and swipes as a phone does and says so over
  the bubble.
- **On clients:** a test bot sends the widget to your private chat.

## Agent block

For a model's system prompt (also `sandbox-node.ts --guide`):

<!-- agent-block -->
```text
You write Telegram widgets: one self-contained HTML document with inline CSS and
JS, shown inside a chat bubble.
- Size: at most 4096 bytes after gzip (level 6) and 32768 bytes of HTML. Aim for
  3800 B gzip. data: images barely compress.
- Offline: no network (fetch, XHR, WebSocket throw), no external scripts,
  stylesheets, fonts or images, no libraries. Images only as data: PNG/JPEG/GIF/
  WebP or inline SVG. Draw charts with SVG or canvas.
- Layout: the bubble is 200-600 px wide and at most 600 px tall; content beyond
  is clipped, nothing scrolls inside. Stretch with %, flex or grid; no vh units.
  body has the client's padding as its margin (body{margin:0} goes edge to edge);
  html has no background, the bubble's colour shows through.
- Fonts: system-ui, sans-serif, serif, monospace, ui-* only. No @font-face.
- Colours: var(--tg-theme-bg-color), --tg-theme-text-color, --tg-theme-hint-color,
  --tg-theme-link-color, --tg-theme-button-color, --tg-theme-button-text-color,
  --tg-theme-secondary-bg-color. prefers-color-scheme follows the chat theme.
- Out of the profile (removed): form controls except <button>, inner scrolling,
  3D transforms, :has(), @container, @layer, CSS nesting, clip-path, masks,
  blend modes, text-shadow, getImageData, keyboard input and focus.
- Input: taps and pointer events; double taps are always yours (two clicks,
  then dblclick). Vertical drags always scroll the chat: nothing can be dragged
  up or down (no vertical sliders, pickers, up/down swipes, 2D joysticks or
  drawing); use horizontal controls or buttons. touch-action decides holds and
  horizontal drags: on auto (default) a hold is the chat's menu and a sideways
  swipe its reply, and the widget gets pointercancel (undo what pointerdown
  started). touch-action: pan-y gives the widget horizontal drags (sliders,
  carousels); none gives it horizontal drags and holds, plus contextmenu (game
  buttons, held controls). It covers an element's children too. Keep controls
  20 px from the leading edge. No keyboard, no text input.
- Text is not selectable unless you set user-select: text.
- API: window.TelegramWidget (check it exists): openLink(https URL, max 256
  chars) and openTelegramLink(https://t.me/...) only inside a click handler, one
  per tap; HapticFeedback.impactOccurred('light'); themeParams; colorScheme;
  onEvent('themeChanged', fn). No sendMessage, no alert/confirm/prompt.
- State: localStorage, 16 KB, per message; catch QuotaExceededError. The widget
- Height changes grow the bubble downward; for a log or thread that grows at
  the bottom call TelegramWidget.setResizeAnchor('bottom') first (if typeof
  TelegramWidget.setResizeAnchor === 'function'): the bubble then grows upward.
  may be recreated at any time; restore state on load.
- Limits: 300 ms CPU per second, 500 ms per task, 64 MB, 256 timers. Render the
  first state synchronously; animate with requestAnimationFrame.
```
<!-- /agent-block -->
