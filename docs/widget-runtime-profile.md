# Widget runtime profile

*What an HTML widget in a Telegram message can rely on. Written for widget
authors and for the tool description a bot gives to its model. The exact
contract, feature by feature and table by table, is **widget profile 1**:
`docs/profile-v1.md`, generated from `packages/core/profile.json`. The
`TelegramWidget` API is in `docs/sdk-v1.md`.*

## In one paragraph

A widget is **one self-contained HTML file** (a full document or a bare
fragment) with inline CSS and JavaScript. It is rendered inside a chat bubble
about **360–420 px wide**; height follows the content up to the client's cap,
and what does not fit is clipped (nothing scrolls inside a widget). The widget
has **no network access of any kind** — not for scripts, images, fonts,
stylesheets, `fetch`, WebSockets or WebRTC — and it cannot navigate anywhere.
No library is bundled. It is drawn the same way on every Telegram client
because every client's validator removes what is outside the profile before it
reaches the renderer; a widget can ask which profile it runs on
(`TelegramWidget.profile`, `CSS.supports`, `@supports`).

## What works

- **Markup.** Text, headings, lists, tables, `<details>`/`<summary>`, `<a>`,
  `<img>`, `<canvas>`, `<button>` (drawn in the client's own button style
  unless the widget styles it), inline SVG (shapes, paths, text, `<use>`,
  gradients). `<form>` and `<dialog>` render as plain blocks.
- **CSS.** Flexbox, grid, tables, positioning (`relative`, `absolute`, `fixed`
  inside the bubble), box model, borders, radii, shadows, backgrounds and
  linear, radial and conic gradients, 2D transforms, `opacity`, `filter` and
  `backdrop-filter` with `blur()`, `brightness()` and `drop-shadow()`,
  transitions and `@keyframes`, custom properties, `calc()`/`min()`/`max()`/
  `clamp()`, `:hover`/`:active`/structural pseudo-classes, `::before`/`::after`,
  `text-overflow: ellipsis`, `font-variant-numeric`, `touch-action`,
  `pointer-events`, `@media` on the bubble's width, `prefers-color-scheme`,
  `prefers-reduced-motion`, `@supports`.
- **Fonts.** System generic families only: `system-ui`, `sans-serif`, `serif`,
  `monospace`, `ui-sans-serif`, `ui-serif`, `ui-monospace`, `ui-rounded`; other
  names are removed from the list (an empty list becomes `system-ui`).
- **Scripts.** Inline `<script>` runs in document order, then
  `DOMContentLoaded` and `load` fire. ES2023 language features, Promises,
  `async/await`, timers, `requestAnimationFrame`, `queueMicrotask`,
  `console.*`, `JSON`, `Intl` (number/date formatting with `formatToParts`,
  collation, plural rules, relative time, lists), `crypto.getRandomValues/
  randomUUID`, `structuredClone`, `TextEncoder/Decoder`, `URL`/
  `URLSearchParams` (parsing only).
- **DOM.** `querySelector*`, `createElement(NS)`, `innerHTML`,
  `insertAdjacentHTML`, `classList`, `dataset`, `style`, `cloneNode`,
  `MutationObserver`, `ResizeObserver`, `IntersectionObserver`, `matchMedia`,
  `getBoundingClientRect`, `offsetWidth` & co, `getComputedStyle`,
  `elementFromPoint`, SVG `getBBox`, `getCTM`, `getTotalLength`,
  `getPointAtLength`; Web Animations (`element.animate`) where the client
  enables it.
- **Input.** Taps (`click`), pointer events (`pointerdown`/`move`/`up`/
  `cancel`, enter/leave/over/out, `setPointerCapture`) and the mouse
  compatibility events; drags when the widget claims them with `touch-action`.
  `preventDefault()` works. No keyboard, no focus, no `dblclick` (a double tap is
  the bubble's reaction) and no context menu (a long press is the message menu).
- **Canvas 2D** — paths, `Path2D`, text and `measureText`, linear and radial
  gradients, transforms, clipping, `drawImage` from other canvases and `data:`
  images, hit testing.
- **Images** — only as `data:` URLs embedded in the file: PNG, JPEG, GIF, WebP.
  **Audio and video are not rendered** at all.
- **Storage.** `localStorage` / `sessionStorage`, 16 KB each, kept by the client
  for this message and there from the first line of script (`docs/sdk-v1.md` §2.8).
- **Theme.** Follow the chat theme with `@media (prefers-color-scheme: dark)`,
  `color-scheme: light dark`, `light-dark()`, `matchMedia` and/or
  `TelegramWidget.onEvent('themeChanged', fn)`; the chat's palette is in
  `TelegramWidget.themeParams` and the `--tg-theme-*` CSS variables.
  `prefers-color-scheme` reflects the **chat theme** (not the OS setting) and
  the widget is re-styled live when the user switches themes.

## Out of the profile

The validator removes these on every client; `docs/profile-v1.md` says how each
one is neutralised.

| Feature | Behaviour |
|---|---|
| Form controls (`input`, `select`, `textarea`, `progress`, `meter`, `fieldset`…), `contenteditable`, `tabindex` | dropped; use `<button>` and taps |
| Keyboard, focus, `:focus`, text selection, `dblclick`, context menu, wheel, scrolling inside the widget | never delivered; `overflow: auto/scroll` becomes `hidden`, `position: sticky` becomes `relative` |
| `<dialog>` modality, popovers | a dialog is a plain block, popovers stay in place |
| Named fonts, `@font-face` | generic families only |
| `vh`/`dvh`, 3D transforms, logical properties, columns, `color-mix()`, `oklch()` and the other new colour functions, `text-shadow`, `line-clamp`, `clip-path`, `mask`, blend modes, filters other than blur/brightness/drop-shadow | the declaration (or the filter function) is dropped |
| `:has()`, `::marker`, `::first-letter`, `::first-line`, `:target`, form states | the selector never matches |
| `@container`, `@layer`, `@property`, nesting, `@media` on height, hover, pointer, print | dropped |
| SVG clip paths, masks, patterns, markers, filters, SMIL, `<image>`, `<foreignObject>` | dropped |
| Canvas patterns, conic gradients, shadows, compositing, `ctx.filter`, `getImageData`/`putImageData`, `toDataURL` | not drawn / transparent / empty |
| SVG, AVIF, BMP and ICO images | placeholder image |
| `fetch`, `XMLHttpRequest`, `WebSocket`, `EventSource`, `navigator.sendBeacon`, `RTCPeerConnection`, `Worker`, `importScripts`, `import()` | throw `NetworkDisabledError` (`fetch` rejects) |
| External `<script src>` (CDN libraries included), `<link rel=stylesheet>`, `@import`, external images, `<iframe>`, `<object>`, `<embed>` | dropped; external `<img>` shows a placeholder |
| `<audio>`, `<video>`, `<source>`, `<track>` | dropped: no media playback |
| WebGL / WebGPU, Web Audio, `OffscreenCanvas`, `Blob`/`File`/`URL.createObjectURL` | `getContext('webgl')` returns `null`; the rest throw or are absent |
| `alert`, `confirm`, `prompt`, `window.print`, clipboard, `navigator.share`, fullscreen, geolocation, camera | no-ops / rejected promises (`NotAllowedError`); nothing reaches the client |
| `TelegramWidget.sendMessage` | removed: a widget keeps its state in `localStorage` or links out |
| Shadow DOM, custom elements | not rendered / no-op |
| `document.cookie`, `document.write` after load, `eval` of remote code | no-ops |

## Bridge: `window.TelegramWidget`

```js
TelegramWidget.profile;                    // 1: the widget profile this client enforces
TelegramWidget.isProfileAtLeast(2);        // false on a profile-1 client
TelegramWidget.colorScheme;                // 'light' | 'dark': the chat's theme
TelegramWidget.themeParams.bg_color;       // the chat's palette, '#rrggbb', Mini Apps keys
TelegramWidget.onEvent('themeChanged', fn); // also 'activated' / 'deactivated' (on / off screen)
TelegramWidget.isActive;                   // !document.hidden
TelegramWidget.openLink('https://…');      // in a tap handler; the user confirms in a native dialog
TelegramWidget.openTelegramLink('https://t.me/…');
TelegramWidget.HapticFeedback.impactOccurred('light'); // also notificationOccurred, selectionChanged
TelegramWidget.ready();                    // optional: "initial render done"
```

The full SDK v1 is in `docs/sdk-v1.md`. The palette is also on the root as CSS
variables (`var(--tg-theme-bg-color)`, `--tg-theme-text-color`, …,
`--tg-color-scheme`). Links must be `https:` and at most 256 characters; a bad
URL throws `TypeError`, and one tap opens one link. `localStorage` /
`sessionStorage` keep 16 KB each for this message (`QuotaExceededError` past
it) and survive the widget being recreated. `navigator.language` is the
client's language. There is no `sendMessage`: nothing is sent to the chat or
the bot on the user's behalf. Clicking an `<a href="https://…">` is
equivalent to `openLink(href)` (`openTelegramLink` for t.me); links never
navigate the widget, and `#fragment` links do not scroll. `location.*`
writes, `form.submit()` and `<meta http-equiv=refresh>` are ignored (and
logged).

## Recommendations for authors (and for models)

1. One file, no external resources — inline everything, prefer inline SVG and
   CSS over images, use the generic font families.
2. Design for a **400 px wide** column; make charts `width: 100%` with a
   `viewBox`; never size anything from the viewport height.
3. Keep initial rendering synchronous and fast (the sandbox has a CPU budget of
   ~500 ms per task and 64 MB of memory). Use `requestAnimationFrame` for
   animation, `setInterval` for clocks.
4. Give interactive elements `id`s and visible state; the message may be
   re-rendered from a static snapshot after the widget is paused, and the
   widget recreated at any time: keep what the user chose in `localStorage`
   and restore it on load.
5. Draw charts yourself with inline SVG or canvas: no library is available.
6. Interaction is taps and drags on buttons and other elements: no text input.
7. Gate anything newer on `CSS.supports(...)`, `@supports` or
   `TelegramWidget.isProfileAtLeast(n)`; what an older client does not support is
   removed, not half-drawn.
8. Support both themes via `prefers-color-scheme`; use `color-scheme: light dark`.

## Example tool description for a bot

> `show_widget(html: string)` — Renders a self-contained HTML widget in the
> chat. The widget runs offline: no network, no external scripts, images or
> fonts, no libraries; draw charts with inline SVG or canvas. Width ≈ 400 px,
> height auto, clipped at the cap, no inner scrolling. Input is taps and drags
> only (no text fields, no keyboard). Generic font families only. Links open
> via a confirmation dialog (`TelegramWidget.openLink`, https only, one per
> tap); nothing can be sent to the chat. Keep state in `localStorage` (16 KB).
> Follow the chat theme with `prefers-color-scheme` or the `--tg-theme-*`
> variables. No WebGL. Inline `<script>`, `<style>` and `data:`
> PNG/JPEG/GIF/WebP images only.
