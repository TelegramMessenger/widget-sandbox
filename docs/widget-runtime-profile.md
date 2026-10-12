# Widget runtime profile

*What an HTML widget in a Telegram rich message can rely on. Written for widget
authors and for the tool description a bot gives to its model.*

## In one paragraph

A widget is **one self-contained HTML file** (a full document or a bare
fragment) with inline CSS and JavaScript. It is rendered inside a chat bubble
about **360–420 px wide**; height follows the content (up to ~900 px, then
scrolls). The widget has **no network access of any kind** — not for scripts,
images, fonts, stylesheets, `fetch`, WebSockets or WebRTC — and it cannot
navigate anywhere. A short list of popular libraries is available offline
when referenced by their usual CDN URL. User actions reach the bot through a
small `TelegramWidget` bridge.

## What works

- **Markup & CSS.** Standard HTML (text, lists, tables, forms, `<details>`,
  `<dialog>`, `<progress>`, `<meter>`), inline SVG (shapes, text, gradients,
  patterns, masks, filters, SMIL animation of non-URL attributes), modern CSS
  (flexbox, grid, custom properties, transitions, `@keyframes`, `@media`,
  `@container`, `@supports`, `@layer`).
- **Scripts.** Inline `<script>` runs in document order, then
  `DOMContentLoaded` and `load` fire. ES2023 language features, Promises,
  `async/await`, timers, `requestAnimationFrame`, `queueMicrotask`,
  `console.*`, `JSON`, `Intl` (number/date formatting, collation, plural rules),
  `crypto.getRandomValues/randomUUID`, `structuredClone`, `TextEncoder/Decoder`,
  `URL`/`URLSearchParams` (parsing only).
- **DOM.** `querySelector*`, `createElement(NS)`, `innerHTML`,
  `insertAdjacentHTML`, `classList`, `dataset`, `style`, `cloneNode`,
  `MutationObserver`, `ResizeObserver`, `matchMedia`, form APIs (`value`,
  `checked`, `selectedIndex`, `FormData`, `checkValidity`), tables
  (`insertRow`, `rows`, `cells`), `getBoundingClientRect`, `offsetWidth` & co,
  `getComputedStyle`, SVG `getBBox`, `getCTM`, `getTotalLength`,
  `getPointAtLength`.
- **Events.** `click`, `dblclick`, `input`, `change`, `submit`, keyboard,
  pointer/mouse/touch, `focus`/`blur`, `wheel`, `scroll`, `transitionend`,
  `animationend`, `toggle`; `on*` attributes and properties; `addEventListener`
  with capture/once/signal. `preventDefault()` works for link navigation and form
  submission (always cancelled) and for keys/clicks the widget has cancelled
  before (learned); the very first occurrence of a cancelled checkbox toggle or
  text input may still show its native default for one frame.
- **Canvas 2D** — paths, text, gradients, patterns, `drawImage` from other
  canvases / `data:` images, `measureText`, `getImageData`, `toDataURL`.
- **Images** — only as `data:` URLs embedded in the file (`image/*`), or inline
  SVG. **Fonts**: system fonts only (`system-ui`, `ui-monospace`, …);
  `@font-face` is dropped. **Audio and video are not rendered** at all.
- **Storage.** `localStorage` / `sessionStorage` exist but are **in-memory for
  the lifetime of the widget** — treat them as a cache, not persistence.
- **Theme.** Follow the chat theme with `@media (prefers-color-scheme: dark)`,
  `color-scheme: light dark`, `light-dark()`, `matchMedia` and/or
  `TelegramWidget.onTheme(cb)`. `prefers-color-scheme` reflects the **chat
  theme** (not the OS setting) and the widget is re-styled live when the user
  switches themes.

## Libraries available offline

Reference them by their normal CDN URL; the runtime substitutes a bundled copy
of the same major version. Any other external script is **ignored** (the page
still renders; `typeof Lib === 'undefined'` lets you show a fallback).

| Library | Version | Recognised `<script src>` patterns |
|---|---|---|
| Chart.js | 4.x | `https://cdn.jsdelivr.net/npm/chart.js`, `…/chart.js@4/dist/chart.umd.js`, `https://unpkg.com/chart.js`, `https://cdnjs.cloudflare.com/ajax/libs/Chart.js/<ver>/chart.umd.min.js` |
| D3 | 7.x | `https://cdn.jsdelivr.net/npm/d3@7`, `https://unpkg.com/d3`, `https://cdnjs.cloudflare.com/ajax/libs/d3/<ver>/d3.min.js`, `https://d3js.org/d3.v7.min.js` |

(Candidates for the next revision, not yet bundled: Tailwind CDN build, Mermaid,
KaTeX, marked.)

## Bridge: `window.TelegramWidget`

```js
TelegramWidget.ready();                 // optional: "initial render done"
TelegramWidget.sendMessage(text);       // asks the host to send `text` to the chat as the user
TelegramWidget.openLink(url);           // asks the host to open a link (user confirms in a native dialog)
TelegramWidget.onTheme(cb);             // cb('light' | 'dark'), called immediately and on change
TelegramWidget.theme;                   // current theme
```

Clicking an `<a href="https://…">` is equivalent to `openLink(href)`; links
never navigate the widget itself. In-page `#fragment` links scroll as usual.

`window.open`, `location.*` writes, `form.submit()` to an external action and
`<meta http-equiv=refresh>` are ignored (and logged).

## Not available

| Feature | Behaviour |
|---|---|
| `fetch`, `XMLHttpRequest`, `WebSocket`, `EventSource`, `navigator.sendBeacon`, `RTCPeerConnection`, `Worker`, `importScripts`, `import()` | throw `NetworkDisabledError` (`fetch` rejects) |
| External `<script src>`, `<link rel=stylesheet>`, `@import`, external images, `<iframe>`, `<object>`, `<embed>` | dropped; external `<img>` shows a placeholder |
| `@font-face` (any source), `<audio>`, `<video>`, `<source>`, `<track>` | dropped: system fonts only, no media playback |
| WebGL / WebGPU / Three.js, Web Audio, `OffscreenCanvas`, `Blob`/`File`/`URL.createObjectURL` | `getContext('webgl')` returns `null`; the rest throw or are absent |
| `alert`, `confirm`, `prompt`, `window.print`, clipboard, fullscreen, geolocation, camera | no-ops / rejected promises |
| Shadow DOM, custom elements, Web Animations API (`element.animate`) | not rendered / no-op |
| `document.cookie`, `document.write` after load, `eval` of remote code | no-ops |
| Persistent storage, IndexedDB, service workers, notifications | absent |

## Recommendations for authors (and for models)

1. One file, no external resources — inline everything, prefer inline SVG and
   CSS over images, use system fonts (`system-ui, sans-serif`).
2. Design for a **400 px wide** column; make charts `width: 100%` with a
   `viewBox`; avoid fixed heights taller than the screen.
3. Keep initial rendering synchronous and fast (the sandbox has a CPU budget of
   ~500 ms per task and 64 MB of memory). Use `requestAnimationFrame` for
   animation, `setInterval` for clocks.
4. Give interactive elements `id`s and visible state; the message may be
   re-rendered from a static snapshot after the widget is paused.
5. Check `typeof Chart !== 'undefined'` / `typeof d3 !== 'undefined'` and show a
   plain HTML fallback otherwise.
6. For user actions that should reach the bot, call
   `TelegramWidget.sendMessage(...)`; never rely on `fetch`.
7. Support both themes via `prefers-color-scheme`; use `color-scheme: light dark`.

## Example tool description for a bot

> `show_widget(html: string)` — Renders a self-contained HTML widget in the
> chat. The widget runs offline: no network, no external scripts/images/fonts;
> Chart.js 4 and D3 7 are available via their jsDelivr/cdnjs/unpkg URLs. Width
> ≈ 400 px, height auto. Use `TelegramWidget.sendMessage(text)` to send the
> user's choice back; links open via a confirmation dialog. No WebGL. Inline
> `<script>`, `<style>` and `data:` images only.
