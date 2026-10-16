# htmlbubbles-ported — Telegram's HTMLBubbles demos as classic widgets

The messages from `corpus/htmlbubbles/` (Telegram macOS beta 12.10 demo chats)
rewritten for twidget: **plain HTML + CSS + JS, no `render()` / `onAction`
harness**. Same look, same texts and numbers, same state machine and the same
interactions, but expressed the way a normal self-contained widget is written
(static markup, event listeners, targeted DOM updates, CSS transitions).
One file per message, same folder and file name as the original; plain-text
messages are not ported.

Verify a file with the Node harness (no browser, no Playwright):

```
corepack pnpm tsx packages/core/scripts/sandbox-node.ts corpus/htmlbubbles-ported/<chat>/<file>.html
```

It must print no `[error]` lines and no `diag validator:url-blocked` /
`unsupported` / `tag-dropped` lines. Interactions can be probed with
`--eval "<expression>"` (evaluated inside the sandbox after load).

## Porting rules

1. **Read** `docs/widget-runtime-profile.md` (what the sandbox supports),
   `corpus/htmlbubbles/prelude.js` (exact semantics of `animate`, `tween`,
   `ease.*`, `transition`, `esc`, `mix`, `alpha`, `lerp`, `clamp`, `sleep`) and
   `corpus/htmlbubbles/kit.css` (the host stylesheet every original relies on).
2. **Structure.** Markup is written in HTML; state lives in JS variables; the
   script updates the DOM in place (`textContent`, `classList`, `style`,
   `hidden`, `value`). Rebuilding a *list* with a template literal and
   `innerHTML` is fine; rebuilding the whole widget from a string on every
   change is not (that is the architecture we are moving away from).
   `data-action` / `onAction(name, value)` become `addEventListener('click')`
   on the elements (event delegation on the root is fine). Keep `esc()`-style
   escaping wherever user-controlled or dynamic text goes into `innerHTML`.
3. **Kit.** Inline only the kit rules the file uses, with the theme
   placeholders resolved through CSS variables. Every file starts its `<style>`
   with this block (trim variables you do not use):

   ```css
   :root { color-scheme: light dark;
     --fg: light-dark(#000, #fff); --muted: light-dark(#6e6e73, #98989d);
     --card: light-dark(#fff, #1c1c1e); --border: light-dark(rgba(0,0,0,0.10), rgba(255,255,255,0.14));
     --sep: light-dark(rgba(0,0,0,0.10), rgba(255,255,255,0.14)); --kbd: light-dark(#f2f2f7, #2c2c2e);
     --blue: #007aff; --blue-h: #0a6ee0; --green: #34c759; --red: #ff3b30; --orange: #ff9500;
     --purple: #af52de; --teal: #30b0c7; }
   body { margin: 0; font-family: -apple-system, system-ui, sans-serif; font-size: 14px; line-height: 1.3; color: var(--fg); }
   ```

   The originals' own `<style>` stays as it is (class names included); only
   `.lc-dark …` rules become `@media (prefers-color-scheme: dark)` or
   `light-dark()`.
4. **Images.** `app:logo|aurora|ocean|dunes|forest|sunset|map` → an inline SVG
   or a CSS gradient block of the same size (keep it pretty: a gradient that
   evokes the name is enough). `sf:<symbol>?size=N&color=HEX` → an inline SVG
   icon of size N in that colour, or an emoji when the symbol is one
   (`heart.fill` → ❤️). No external URLs, no `data:` images larger than a few KB.
5. **Animation.** `tween`/`animate` on opacity, transform, size, colour → CSS
   `transition` / `@keyframes` with the same duration and a matching easing
   (`ease.outCubic` → `cubic-bezier(.33,1,.68,1)`, `spring`/`outBounce`/
   `outElastic` → `@keyframes` with a few overshoot steps). Animated
   *numbers* (counters, progress labels) and sequenced steps keep JS with
   `requestAnimationFrame` and the prelude's easing formula. `transition(kind)`
   on the whole bubble → a short CSS animation on the widget root
   (`fade`, `slide-*`, `pop`), triggered by toggling a class.
6. **Links.** `chat:toggle?key=x` and `chat:select?key=…&group=…` → buttons or
   `<a href="#">` with `preventDefault()` toggling the `on-x` class on the
   widget root. `copyText(s)` → visual "Copied" feedback only (clipboard is a
   no-op in the sandbox). `update()` → nothing (the DOM is already updated).
7. **Timers.** Same delays and intervals as the original; clean up intervals
   when the original cancels them. Nothing may spin a tight loop.
8. **Width.** The widget renders in a ~400 px column; keep the originals'
   `width: 392px; max-width: 100%` style roots.
9. **Header.** Keep the original's leading comment, add one line
   `ported: classic widget, see corpus/htmlbubbles-ported/README.md`.
10. **Scope.** Write only into this folder; never touch `src/`, `tests/`,
    `scripts/`, and never run Playwright or the dev server (shared ports).

## Status (2026-10-09)

116 messages ported (12 chats; the 20 plain-text messages were never
extracted). Every file passes the Node harness and the desktop C++ runner
(`twidget-run --corpus Corpus/htmlbubbles-ported/<chat>`, 116/116); the
group is copied into the iOS/macOS, Android and desktop demos by the build
scripts (`--autorun htmlbubbles` / `--es autorun htmlbubbles` run it).
Android on a real phone (debug APK): 116/116. macOS demo: these widgets are
5–15× heavier at boot than the compat corpus (50–90 KB of markup parsed by
the shim), and the CPU quota is wall-clock, so on a loaded Mac (load average
20–50 during the only runs so far) the Debug build killed 67 of them at boot
(`CPU limit exceeded … (boot)`) while compat passed 21/21; the Release build
(`xcodebuild -configuration Release`, QuickJS optimised) brought it to 95/116
under the same load. Re-run `--autorun htmlbubbles` on an idle machine before
reading anything into the macOS number. `corepack pnpm test:htmlbubbles`
(sandbox vs native Chrome screenshot after up to 4 clicks, report in
`docs/compat-report-htmlbubbles-ported.md`) passes all but a handful, and
those are limits of the screenshot method, not of the sandbox:

- `11-live-scripts/17-pixel-bare.html` throws an uncaught `ReferenceError` on
  purpose (the "Buggy button" demo); the test counts any JS error as a failure.
- `11-live-scripts/15-pixel-bare.html` ("Don't press") has no row at all: the
  test clicks its Hang button, the sandbox kills the runtime as designed, but
  the native reference iframe really hangs and the test times out.
- `12-motion-lab/03-mo-bare.html` (confetti), `05-mo-bare.html` (equaliser),
  `06-trip-lisbon/11-tom-bare.html` (rolling dice) use `Math.random` per frame;
  `12-motion-lab/08-mo.html` (typewriter), `10-mo-bare.html` (1.5 s fake
  load) and `13-mo-bare.html` (just over the 3 % tolerance) are time-based, and the reference screenshot is taken ~800 ms later
  than the sandbox one. Two runs never match pixel for pixel, and a loaded
  machine (`cpu-throttled` in the diagnostics column) adds a few more such
  rows, e.g. `06-trip-lisbon/09-me-bare.html`, `12-ana.html`.

Porting notes worth keeping: a top-level `const top = …` runs in QuickJS but
is a `SyntaxError` in a real browser (`window.top` is non-configurable), so
the ported files avoid `top`, `parent`, `self`, `location`, `window`,
`document` as global names. `alert()` is replaced by `console.log` (dialogs
are unsupported in the sandbox). The static fallback markup that the originals
show when their script is killed is dropped (twidget keeps the last render
instead). The "Runaway at load" card (`11-live-scripts/14`) is static: a
`while (true) {}` at load would only demonstrate the CPU kill.
