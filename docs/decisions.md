# Decisions

Choices made while building the prototype, alternatives rejected, and features
dropped because they conflicted with a security invariant.

## Phase 0 — spike findings

### QuickJS: `quickjs-emscripten-core` + `@jitl/quickjs-ng-wasmfile-release-sync`

- The **quickjs-ng sync variant** works in a dedicated Worker. Host functions
  (`vm.newFunction`) may block the worker thread with `Atomics.wait` — that is
  how the sync query bridge works (see `packages/core/src/host/worker.ts`). The asyncify
  variant was not needed and is ~2× slower.
- `setInterruptHandler` fires often enough to kill `for(;;){}` within a few ms
  of the deadline. `setMemoryLimit(64 MB)` alone is **not** a memory limit in
  this build (see "The memory limit is the wasm memory's maximum" below): the
  worker also hands the module a `WebAssembly.Memory` whose `maximum` is
  16 MB + the limit. Allocation failures then throw `InternalError: out of
  memory`, which the worker turns into a clear UI error when uncaught.
- `evalCode(..., { compileOnly: true })` returns a bytecode handle, but
  `encodeBinaryJSON` cannot serialize function bytecode in this build
  ("Couldn't allocate memory to get ArrayBuffer"). **Bytecode caching of
  bundled libraries is therefore done differently**: the library source is
  fetched once per worker (sync XHR of a same-origin file) and kept as a string;
  QuickJS compiles ~200 KB of minified Chart.js in well under 100 ms, so the
  cost is acceptable for the prototype. A native port would use
  `JS_WriteObject`/`JS_ReadObject` directly (see `native-port-notes.md`).
- The runtime has **no module loader** installed, so `import()` and static
  `import` of anything fail inside the sandbox — this is the intended behaviour
  for invariant 5.
- quickjs-ng has no `Intl`. Rather than shipping ICU into the sandbox, `Intl`
  and the `toLocale*` methods are **delegated to the host** through the single
  `__host('intl', …)` capability (pure formatting, no I/O).

### DOM library: linkedom (+ parse5 for parsing)

linkedom runs inside QuickJS after two adjustments:

- it needs `atob`/`btoa` at module-evaluation time (`packages/core/src/sandbox/prelude.ts`);
- its optional `canvas` dependency is aliased to a stub; `<canvas>` is served by
  our recording proxy instead.

Known linkedom gaps that the shim fills (`packages/core/src/sandbox/*`):

| Gap | Shim |
|---|---|
| HTML parsing differs from browsers (no implied `<tbody>`, `<head>`/`<body>` for fragments, SVG attribute casing) | **parse5** parses; linkedom only holds the tree (`dom.ts`) |
| `EventTarget` has no capture phase and no `on*` attribute handlers | own event system (`events.ts`) |
| `MutationObserver` misses text nodes, style writes and several insertion paths | own observer on top of the reconciliation pass (`capture.ts`) |
| `element.style` splits on `;` (breaks `data:` URLs) | own `CSSStyleDeclaration` (`style.ts`) |
| no layout, no `getBoundingClientRect`, `offsetWidth`, `getBBox`, … | sync queries to the renderer (`layout.ts`) |
| no form control state (`value`, `checked`, `selectedIndex`), tables API | `forms.ts` |
| doctype node is not linked to `<html>` through `nextSibling` | the document's children are taken from `documentElement` |

Alternatives considered: **jsdom** (far too heavy, depends on Node APIs),
**happy-dom** (also Node-oriented, larger, slower to boot in QuickJS),
**remote-dom** (Shopify; would have required rewriting widgets against a
custom element model instead of running them unchanged). linkedom boots in
~0.5 s (QuickJS, unminified eval of a 500 KB bundle) which is acceptable for a
prototype; bytecode would cut this on native.

### Mutation capture: reconciliation, not MutationObserver

The shim does not rely on linkedom's MutationObserver. DOM mutators are wrapped
only to mark nodes dirty; at the end of every task the dirty nodes are
re-read and diffed against a mirror of what the host already knows. This is
correct regardless of which internal path linkedom used to mutate the tree,
and a periodic full walk (every 500 ms) catches anything unwrapped — such
misses are counted in the diagnostics (`missed mutations`).

### Cross-origin isolation and the sandboxed `srcdoc` iframe

`COOP: same-origin` + `COEP: require-corp` on the dev server makes
`crossOriginIsolated === true` and `SharedArrayBuffer` available. A
`<iframe sandbox="allow-scripts" srcdoc=…>` **does** load and run under COEP
(an opaque-origin srcdoc document is same-site for embedding purposes), and the
renderer bundle, images (`data:`) and fonts all work there.

### Reference mode under cross-origin isolation

A nested document loaded into a COEP `require-corp` page must itself carry
`Cross-Origin-Embedder-Policy` (and CORP) headers, otherwise Chrome blocks it
with `ERR_BLOCKED_BY_RESPONSE`. Responses ended by the dev-server middleware
bypass Vite's `server.headers`, so the middleware sets the isolation headers
itself. (The product path — `srcdoc` — never hits this.)

### Fragments always render in standards mode

A bare fragment (no `<!DOCTYPE html>`) is rendered in the renderer's own
document, which has a doctype, so it is laid out in standards mode. A browser
opening the same file directly would use quirks mode (different inline line
boxes: a 1–2 px drift that showed up as a 2 % pixel diff). Reference mode
therefore prepends a doctype to doctype-less files — the comparison target is
"the widget inside our document", not "the file opened as a page". Full
documents are untouched.

### Shadow DOM renderer: the renderer core without the frame

Since 2026-10-16 this is the web product renderer (decision D2; see "The web
renders through Shadow DOM; invariant 6 reworded" below). Until then the web
product path was the sandboxed `srcdoc` iframe, but a feed of
hundreds of bubbles pays per bubble for a document: asynchronous boot, a
style context, nothing buildable before insertion. The renderer code was
therefore split into `packages/core/src/renderer/core.ts` (`createRenderer(env)`, one
instance per widget, an environment abstracting document vs shadow root) and
the transport entry `packages/core/src/renderer/renderer.ts`; `ShadowDomRenderer`
(`packages/core/src/host/shadow-renderer.ts`) runs the same core on an open shadow root in
the host page with real `<html>/<head>/<body>` elements inside, `:host { all:
initial; contain: layout paint style; container-type: inline-size }`, and the
validator in `scope: 'shadow'` (host-reaching selectors dropped, `:root` →
`[data-tw-root]`, size media queries → `@container tw`, viewport units →
container units). This relaxes invariant 6 (no opaque origin, no per-widget
CSP): the attack suite on `TW_RENDERER=shadow` measures exactly what the frame
used to add, and the decision is made on its result plus the feed bench
(`docs/shadow-dom-renderer.md`). Native platforms keep their WebView documents
(`scope: 'document'`, the default).

### Renderer iframe: `allow-scripts allow-forms`

Without `allow-forms`, Chrome blocks a form submission *before* dispatching the
`submit` event ("Blocked form submission … 'allow-forms' permission is not
set"), so widgets never see `submit` and implicit submission (Enter in a text
field) stops working. `allow-forms` is therefore set. Navigation through a form
is still impossible three times over: the renderer calls `preventDefault()` on
every `submit` unconditionally, the CSP has `form-action 'none'`, and the
`action`/`formaction` attributes never reach the renderer (so the only possible
target would be the frame's own `about:srcdoc`). `allow-popups`,
`allow-top-navigation*`, `allow-same-origin` and `allow-modals` stay off.

### The host owns the colour scheme

Setting `color-scheme` on the iframe element does **not** change
`prefers-color-scheme` inside a sandboxed `srcdoc` frame in Chromium 153
(verified with a probe), and in the product the chat theme is the app's
choice anyway, not the OS setting. So the host is authoritative: the CSS
sanitizer evaluates `@media (prefers-color-scheme: …)` against the host
theme (rewriting the feature to an always-true/always-false width query),
resolves `color-scheme: light dark` to the current scheme and picks the branch
of `light-dark()`; the validator re-emits every stylesheet when the theme
changes; `matchMedia('(prefers-color-scheme: …)')` queries are rewritten the
same way before they reach the renderer; and the sandbox receives a `theme`
event for `TelegramWidget.onTheme`. Known gap: `light-dark()` inside a `style`
attribute is resolved once, when the attribute is set.

### Renderer ↔ host communication

A `MessageChannel` port is handed to the renderer with a single
`postMessage` after it announces itself; the renderer checks `e.source ===
window.parent` and ignores everything else. All later traffic goes through the
private port, so other scripts on the host page cannot inject ops.

### Why plain JSON for ops

The protocol is JSON so that it can be validated with a strict schema on every
platform and logged/replayed in tests. Binary encodings would be faster but
are a production concern (non-goal).

### Stack

| Dependency | Why |
|---|---|
| `quickjs-emscripten-core`, `@jitl/quickjs-ng-wasmfile-release-sync` | the sandbox engine; the prompt asked for quickjs-ng sync |
| `linkedom` | in-sandbox DOM (see above) |
| `parse5` | spec-compliant HTML parsing in the sandbox; also used by linkedom's competitors; small |
| `css-tree` | real CSS parser for the sanitizer (no regex-only sanitizing) |
| `chart.js`, `d3` | the bundled libraries the loader rewrites CDN URLs to |
| `vite`, `esbuild` | dev server with COOP/COEP; esbuild builds the in-sandbox and renderer IIFE bundles |
| `@playwright/test`, `pixelmatch`, `pngjs` | attack suite and screenshot diffing |
| `ws` | canary WebSocket server |
| `tsx` | runs the Node harness / scripts without a build step |

## Shim lessons from the compat suite

Each of these was found by a failing compat scenario and fixed generally in
the shim (never per widget):

- **`window`, `self`, `top`, `parent`, `frames` must be writable.** They are
  `[Replaceable]` in browsers; a top-level `var frames = 0` silently kept the
  window object when the alias was read-only, and the widget's counter became
  `NaN`.
- **`submit` needs `allow-forms`** on the sandboxed frame (see above); without
  it the event is never dispatched, which looked like "the handler never ran".
- **Parsing must match the browser's tree.** linkedom's own parser does not
  insert `<tbody>`, puts fragment-level `<style>` in the wrong place and keeps
  SVG attribute casing differently; parse5 removed a whole class of layout
  diffs.
- **Doctype-less fragments** are standards-mode in the product; reference mode
  normalizes them the same way (see "Fragments always render in standards
  mode").
- **`:checked` / `:focus` must reflect live state.** css-select (linkedom's
  selector engine) resolves `:checked` from the `checked` *attribute*, and its
  built-in string aliases take precedence over user-supplied pseudo functions,
  so overriding by name is impossible. The build redirects linkedom's
  `import 'css-select'` to `packages/core/src/sandbox/css-select-shim.ts`, which rewrites
  `:checked` → `:tw-checked` (outside quoted strings) and registers
  state-backed predicates under the rewritten names. A quiz widget scored 0/3
  before this.
- **Scripts inserted by fragment parsing are inert.** Browsers never run a
  `<script>` that arrives through `innerHTML`, `outerHTML` or
  `insertAdjacentHTML`; the shim initially queued them, so a widget doing
  `document.body.innerHTML += …` re-created and re-ran itself until the
  document had ~100 000 elements (found by `corpus/attacks/api-innerhtml.html`,
  which also exposed that the dev UI re-rendered its diagnostics panel per
  rejected op and froze; panel rendering is now coalesced per frame). The
  validator's 50 000-node cap and the 64 MB memory limit were the only things
  bounding it before.
- **Animated widgets need deterministic hit targets for the harness**, not for
  the sandbox: a counter next to a button made the button "unstable" for
  Playwright in both modes. The corpus widget got a fixed-width counter; the
  harness parks the mouse before each run so hover state cannot leak between
  layouts.

## iOS implementation (`ios/`)

- **quickjs-ng v0.17.0 vendored as a C target** (4 source files + headers,
  MIT) behind a 40-line C shim that hides `JSValue` macros and inline helpers
  from Swift. Chosen over JavaScriptCore because JSC has no public interrupt /
  memory-limit API and the design calls for one engine, one shim, one protocol
  on every platform.
- **The validator is not ported to Swift** — the same `validator.ts` (with
  css-tree) is bundled (`validator-bundle.js`, built for the browser platform
  so css-tree's prebuilt data modules are used) and runs in a *second, trusted*
  QuickJS runtime. One implementation to audit; the cost is a JSON round trip
  per batch.
- **Renderer bundle is shared** with the web host; only the transport differs
  (`__TW_TRANSPORT__` define: MessagePort vs `WKScriptMessageHandler` +
  `evaluateJavaScript`). Binary query results travel as base64.
- **Each widget gets its own origin**: the renderer document is served from
  `tgwidget://w-<uuid>/` by a `WKURLSchemeHandler` that refuses every other
  URL; CSP comes both as a response header and a `<meta>`.
- **Found while writing the Swift tests**: the validator forwarded a
  `<style>` element's raw text as a *detached* text node to the renderer. Not
  a leak (never in the DOM), but it now emits `rm` for it so only the sanitized
  `css` op is ever shipped.
- **Found only on the simulator** (at the time the WKWebView path was
  UIKit-only, so the macOS `swift test` run could not see them):
  - `WKURLSchemeHandler` / script message handlers must be registered on the
    configuration *before* `WKWebView` is created — the view copies its
    configuration, and a handler added afterwards is silently ignored (the
    document failed with `NSURLErrorUnsupportedURL`, so the engine never
    received `ready` and sat at "loading" forever).
  - codesign refuses an iOS bundle with a top-level `Resources/` directory
    ("bundle format unrecognized"), so the package's resource directory is
    `WidgetJS/`.
  - The renderer reports its `size` before `ready`; the engine therefore
    receives input before its QuickJS context exists. Inputs are now buffered
    until boot (`pendingBeforeBoot`), mirroring what the web Worker gets for
    free from the message queue. Regression test: `inputsBeforeStartAreBuffered`.
  - QuickJS measures stack depth from the stack pointer it recorded at
    `JS_NewRuntime`. A serial `DispatchQueue` is not a thread: later blocks run
    on other pool threads with other stacks, and every call from one of them
    failed with a bogus `RangeError: Maximum call stack size exceeded` (seen
    when the Dark toggle fired after d3 had loaded). Pinning the limit under the
    512 KB GCD stack was not an option either: a debug build of quickjs-ng
    spends ~10 KB of C stack per JS call, which gave 40 levels — css-tree in the
    validator already failed at that. **Each runtime is now pinned to its own
    `JSThread`** (a `Thread` with an 8 MB stack and a run loop; the QuickJS limit
    is 6 MB, ~680 levels in debug) and `QuickJSContext` re-reads the stack top
    on every entry as a belt-and-braces measure. The validator's calls stay
    synchronous for the host but execute on its thread. Regression tests:
    `stackCheckFollowsTheCallingThread`, `pinnedThreadGivesDeepRecursion`.
- **Verification status**: XCTest suite (engine, trusted validator, renderer
  in a real WKWebView) passes on the iPhone 17 simulator (iOS 26.5) and the
  engine/validator subset passes natively on macOS. The demo app renders the
  compat corpus (Chart.js, d3, forms, drag, dark theme) and its attack runner
  loads every `corpus/attacks` file against the canary on the Mac — see
  `packages/ios/README.md` for the recorded result.

## macOS implementation (`macos/` + the `ios/` package)

- **No fourth port: the iOS package compiles for AppKit.** `Package.swift`
  already declared `.macOS(.v13)`; the engine, validator, JSThread and Intl
  bridge are Foundation-only, and WebKit's API is shared. What was UIKit-only
  (`RendererWebView`, `WidgetHost`, `WidgetView`) is now split by
  `#if canImport(UIKit) / canImport(AppKit)`: `PlatformView` (UIView | NSView),
  the container view, the SwiftUI representable, and five WKWebView knobs
  (background, colour scheme, scrolling, context menu, file panel). The
  bundles, the corpus and `pnpm build:macos` (= `build:ios`) are the same files.
- **Off-screen pause on AppKit = window attachment + `NSWindow.occlusionState`.**
  A minimised, fully covered or other-Space window counts as hidden (the
  background-tab analogy). `occlusionState` is trusted only after the window
  server has reported on the window (a notification, or `.visible` seen);
  before that `isVisible` decides. Reason: a process that never finishes
  launching `NSApplication` — the xctest runner — gets no occlusion events, its
  windows stay "not visible" forever, and WebKit in that process keeps the page
  `hidden` too (no `requestAnimationFrame`, timers at 1 Hz; checked with a bare
  WKWebView). So `testRemovingTheViewFromTheWindowPausesTheWidget` is iOS-only,
  and the macOS demo runs the same scenario in a real app
  (`--autorun pause`: minimise → tasks stop → restore → tasks resume, widget
  sees `visible,hidden,visible,`).
- **Context menu and file dialogs are closed on macOS.** A `WKWebView`
  subclass empties the menu in `willOpenMenu` (Reload / Open in New Window /
  Download / Inspect Element); `runOpenPanelWith` answers `nil` so
  `<input type=file>` never shows an NSOpenPanel. Neither is a new barrier
  (navigations were already cancelled) but a bubble must not expose them.
- **Transparent page on macOS** needs `drawsBackground = false` through KVC;
  the public `underPageBackgroundColor` only tints the area past the
  document's end. It is the switch WebKit itself reads; not security-relevant.
- **The demos share their sources**: `apps/macos/DemoApp/project.yml` points at
  `apps/ios/DemoApp/Sources` and `apps/ios/DemoApp/Corpus`; the platform differences are
  `DemoPlatform.swift` (colours, report location, `--autorun`) and the scroll
  visibility check, which now compares the bubble with the enclosing
  ScrollView's bounds instead of `UIScreen.main` (same result on iOS, works on
  both). Runners got launch-argument autorun and JSON reports like the Android
  demo, plus a compat runner.
- **Reference mode in the native demo** (`ReferenceWebView.swift`, demo
  sources, shared by iOS and macOS): the web prototype's `?reference=1`
  rebuilt on a WKWebView — rewritten library URLs, doctype, private scheme,
  `REFERENCE_CSP`, rule list, navigations cancelled — plus a `compare` runner
  that times every compat/generated widget both ways. Kept out of the package
  and off the attack corpus, as CLAUDE.md requires. The reference view is
  sized and attached before it navigates, like the web iframe. One finding:
  `d3-bar` fails natively on WebKit (`getBBox()` on a just-appended SVG
  `<text>` during parsing throws `NotFoundError`; Chromium does not) while the
  sandboxed copy passes, because its layout queries run against the laid-out
  renderer document — a case where the protocol is more forgiving than the
  engine.
- **Verification status**: package tests green natively (21; the 22nd is
  iOS-only as explained) and still 22/22 on the iPhone 17 simulator; the macOS
  demo loads generated 40/40 and compat 21/21, the attack corpus (89 files)
  with 0 canary hits, and the pause probe passes — see `apps/macos/README.md`,
  which also has the sandbox-vs-reference timings (debug build: ~2.3× wall
  time to loaded, dominated by the source-eval boot).

## Android implementation (`android/`)

The iOS architecture, ported glue for glue (Oct 2026). What was reused
unchanged: the three JS bundles, the vendored quickjs-ng sources *and*
`qjs_shim.c` (the CMake file points at `packages/ios/Sources/CQuickJS`), the protocol,
the `__host` string API, the engine/validator/host structure. What differed
from iOS and why:

- **JNI strings are byte arrays.** JNI's `jstring` helpers speak *modified*
  UTF-8: supplementary characters (emoji, common in widgets) become
  surrogate-pair sequences that QuickJS does not understand, and
  `NewStringUTF` on a real 4-byte sequence is undefined behaviour under
  CheckJNI. Every string crossing the boundary is therefore a `byte[]` of
  real UTF-8, decoded on the Kotlin side (`QuickJSContext.kt`). Exceptions
  are raised the same way: C calls a private Kotlin `raise(byte[])` that
  throws `QuickJSException`, so the message survives non-ASCII too.
- **The interpreter is compiled with `-O2` even in debug builds.** Measured
  on the emulator: 394 JS frames at the default 384 KB limit and ~6300 on a
  `JSThread` (6 MB), i.e. ~1 KB of C stack per JS call, versus ~10 KB in the
  iOS debug build. Widget boot (sandbox bundle eval + initial tree) is
  60–90 ms instead of ~150 ms. Nobody debugs quickjs-ng internals through the
  app, so the trade is free.
- **`JSThread` is a plain `Thread` + `Looper`**, not a `HandlerThread`:
  `HandlerThread` has no stack-size constructor and the ~1 MB default is too
  small for css-tree inside the validator. The looper gives timers
  (`postDelayed`) and ordered posts for free, same role as the iOS run loop.
- **Interrupt deadline on `CLOCK_BOOTTIME`** (= `SystemClock.elapsedRealtimeNanos()`),
  compared inside the C interrupt handler; Kotlin credits the time spent in a
  sync query back to the deadline, like Swift does.
- **Memory and CPU limits are public API from day one**
  (`WidgetHost.Options(memoryLimitBytes, cpuLimitMs)`), the gap noted for iOS.
  A test covers both through the host, not only through the engine.
- **The renderer origin is `https://w-<uuid>.tgwidget.invalid/`**, served by
  `shouldInterceptRequest`, not a custom scheme: Android WebView has no
  `WKURLSchemeHandler`, and an `https` origin keeps the document a secure
  context. `.invalid` can never resolve, and `setBlockNetworkLoads(true)`
  stays on because interception runs before the network stack — verified
  on the emulator (the document loads with the flag set, an injected
  external `<img>` is refused and reported, `location.href` navigation is
  cancelled). Every non-document request is answered with an empty 403 and a
  `url-blocked` diagnostic instead of being merely dropped, so a leak attempt
  is visible in the panel. `WebViewAssetLoader` was not used: it is a
  convenience over the same callback, not a boundary.
- **Hardening lives in a document-start script** through
  `WebViewCompat.addDocumentStartJavaScript` (androidx.webkit), the WKWebView
  `WKUserScript` equivalent, with the same list plus `WebTransport`,
  `RTCDataChannel` and `navigator.serviceWorker`. `RendererWebViewTest`
  checks `typeof fetch` etc. through `evaluateJavascript` — the only place
  where the host evaluates trusted script other than `__twReceive`.
- **Safe Browsing is switched off twice** (`WebSettingsCompat` and the
  manifest meta-data merged from the library): it is the one way a stock
  WebView talks to Google on its own, which would show up as unexplained
  traffic in a pentest even though it is not a widget leak.
- **The attack runner's canary address is editable and remembered**
  (default `10.0.2.2:9999`, the emulator's alias for the host); the iOS demo
  hardcodes `127.0.0.1` and cannot run on a phone without a source edit.
- **Runners are drivable from `adb`** (`--es autorun attacks|generated|compat`)
  and write JSON reports into the app's `files/` directory; the compat corpus
  got a runner of its own because Android has no reference renderer for the
  pixel diff, so loaded / errors / diagnostics are what is recorded.
- **Theme toggling needs no WebView-level dark mode.** `WKWebView` has
  `overrideUserInterfaceStyle`; a `WebView`'s `prefers-color-scheme` is read
  from its context's configuration at creation and cannot be flipped later
  (`dispatchConfigurationChanged` with a night-mode `uiMode` was tried on the
  emulator: no effect). The first write-up of this port called that a gap and
  proposed a host→renderer `theme` message; measuring showed it is not one:
  the validator already resolves `prefers-color-scheme` media queries,
  `color-scheme: light dark` and `light-dark()` against the host theme and
  re-emits every stylesheet on `setTheme`, and `matchMedia` is answered by the
  host. After a toggle the root `color-scheme`, `light-dark()` colours and
  the UA's own form-control colours switch (regression test
  `themeToggleReachesComputedStylesAndFormControls`). The renderer's internal
  preference stays stale but nothing a widget can observe depends on it, so
  the protocol stays as it is.
- **Found on a real phone: drags died under the chat scroll.** drag.html moved
  a few px and stopped whenever the finger had a vertical component, while
  horizontal drags worked; the diagnostics were clean. The Compose scroll
  container intercepted the touch past the slop and the WebView got
  `ACTION_CANCEL` → `pointercancel`. iOS and the web never had this: WebKit
  resolves `touch-action` against `UIScrollView` itself, and the web
  prototype's iframe shares the page compositor. Fix: the renderer posts a
  `gesture` message on every primary `pointerdown` (`touch-action` on the
  ancestor chain forbids vertical panning, a range input, a vertically
  scrollable box, or a learned `preventDefault` — the range sliders of
  inputs.html stopped the same way, Chromium's own thumb drag does not ask
  the parent to stand back either),
  and `RendererWebView` denies parent interception from `ACTION_DOWN` until
  the answer says no (or 150 ms pass). The message is part of the shared
  renderer; web and iOS hosts ignore it. Regression test: `GestureTest`.
- **Found on a real phone: pickers did nothing.** Tapping `<input type=color>`
  (inputs.html) and the `<select>` of form-validation.html opened nothing,
  with no error anywhere. Chromium shows those dialogs only when it finds an
  Activity behind the WebView's context, and the renderer view was created
  from `createConfigurationContext(...)` — a bare context, chosen to pin the
  initial `uiMode`. The themed context is now a `ContextThemeWrapper` over
  the caller's context with the same override configuration, so the Activity
  stays reachable; colour picker and select popup work, the value reaches
  the widget (`#ff0000`, preview recoloured). Regression test:
  `themedContextKeepsTheActivityAndSetsNightMode`.
- **Found on the emulator only**: `[null].join()` is `''` (a test
  expectation, not a bug); a throw inside a promise reaction does not make
  `JS_ExecutePendingJob` fail (only the derived promise rejects), so the
  engine's job-error path only ever sees interrupts and OOM — same as iOS,
  now covered by a test.
- **Verification status**: see `packages/android/README.md` (26 instrumented tests,
  attack runner, generated and compat runners on the Android 15 emulator).

## Phase 6 — generated corpus (`corpus/generated/`)

- **How it was produced.** No Anthropic API key was available, so the two
  batches (20 prompts from `scripts/generate-corpus.ts`, with and without
  `docs/widget-runtime-profile.md`) were written by Claude inside a Claude Code
  session instead of by `pnpm generate-corpus`. The "without" batch was written
  the way an assistant writes for a generic `show_widget(html)` tool — Google
  Fonts `<link>`s, a CDN-hosted hero image, `fetch` for live rates,
  `navigator.clipboard`, `alert`/`confirm`, `Notification`, `localStorage`,
  `element.animate`, `location.reload()`; the "with" batch follows the profile
  (inline everything, `color-scheme: light dark`, `TelegramWidget.sendMessage`
  for user actions, `typeof Chart` fallbacks, no blocking dialogs). Caveat:
  generator and sandbox came from the same model, so this measures the profile
  as a prompt, not what a third-party model would do.
- **Result** (`pnpm test:generated`, see `docs/compat-report-generated.md`):
  every widget in both batches loads, runs without JS errors and renders within
  3 % of the native reference after the harness clicks up to four buttons. The
  profile did not change the pass rate — the sandbox absorbs the "without"
  habits: external fonts and images are dropped (`tag-dropped`, `url-blocked`;
  system font and a blank hero render instead), `fetch` rejects and the
  widget's own catch path shows "offline" (`network-blocked`), `confirm()`
  returns `false` so deletes are silently refused (`unsupported`), `localStorage`
  simply does not persist. What the profile *does* change is behaviour the
  pixel diff cannot see: the "with" widgets send results to the chat, follow
  the dark theme and never attempt a network request, while the "without" ones
  log a blocked request or a no-op dialog per widget.
- **Same corpus on iOS** (demo app → "Run the generated corpus", iPhone 17
  simulator, iOS 26.5): 40/40 load without JS errors or termination; median
  JS time per widget 171 ms including the ~150 ms sandbox-bundle boot (debug
  build, no bytecode cache yet); the same `tag-dropped` / `url-blocked` /
  `network-blocked` diagnostics appear on the "without" widgets as on the web.
  No pixel comparison on iOS (there is no reference renderer there); the runner
  writes `Documents/generated-report.json` in the app container.
- **One corpus bug, not a sandbox bug.** The tic-tac-toe widget (both batches)
  accepted clicks during the computer's 350–400 ms "thinking" delay, so the
  harness's rapid clicks finished the game three times ("You 3") and the two
  modes diverged on timing (3.9 % diff). Both files were fixed identically
  (ignore clicks while busy) and the suite re-run; the race is a realistic
  thing for a model to write, and the sandbox handled it the same way a browser
  does.

## Dropped features

- **`<a target=_blank>` / `window.open` opening anything** — links never
  navigate; they go through `TelegramWidget.openLink`, logged by the host.
- **`document.cookie`, `document.write`, Blob/File, `URL.createObjectURL`** —
  no-ops with an `unsupported` diagnostic. `createObjectURL` would create a
  URL the sandbox could inject into markup; the host mints no blob: URLs in
  this prototype, so the renderer accepts none.
- **Shadow DOM, custom elements, Web Animations API** — out of scope for the
  prototype; logged as unsupported.
- **`alert/confirm/prompt`** — cannot block the sandbox thread; they are
  forwarded to the host as a bridge call and return `false`/`null`.

## Runtime limits (Oct 2026)

Four changes.
Everything that counts and decides lives in trusted glue; the shim only gets
information.

### Throttle, not kill: a CPU quota per second

- Telegram kills a widget the moment it exceeds its budget. We **defer**: a
  sliding window of 1 s in ten 100 ms buckets, 300 ms of JS per window;
  past it timers, frames, resize/viewport/theme/bridge tasks wait until the
  oldest busy bucket leaves the window (frames collapse into one, one fire per
  timer id, order kept). The widget animates less often but keeps working,
  which is what the compat corpus (Chart.js animations, rAF loops, D3
  transitions) needs. `cpuKillAfterMs` (default 0 = off) brings back the kill
  for products that want it: sustained throttling, no gap longer than one
  window, for that long.
- Events always run with a floor of 50 ms so the widget answers the user even
  with the budget spent; the floor applies to every slice (`min(cpuLimitMs,
  max(remaining, 50))`), `boot` always gets the full `cpuLimitMs`. The flip
  side, accepted: a task that overruns its slice while the window is nearly
  full is killed like an infinite loop would be, and the message says why
  (`…; CPU quota: 12 ms of 300 ms per 1000 ms left, so this timer task got
  50 ms`). So under the defaults the effective per-task cap outside boot is
  300 ms, not 500.
- Our own cost is not the widget's: evaluating the sandbox bundle (~0.5 s of
  source eval on the web, ~150 ms on an iOS debug build) is excluded from the
  boot task's recorded time, otherwise every widget would spend its first
  second throttled.
- One pure policy, three copies, one fixture: `packages/core/src/host/cpu-budget.ts`,
  `CpuBudget.swift`, `CpuBudget.kt` are line-for-line mirrors checked against
  `packages/core/tests/fixtures/cpu-budget.json` (the build script copies it into the native
  test resources). The glue around them (worker.ts, the Node harness,
  SandboxEngine.swift/.kt) is ported, as everything else in the native layer.
- Debug builds of quickjs-ng are several times slower than release, so the iOS
  demo and the native tests use a wider quota (1000 ms/s, or 100 ms/s where a
  test wants throttling quickly); the product default stays 300.

### Strict-policy text limits

Strict validators allow at most 32 Ki UTF-16 units per text value and 48 Ki units across tracked text and comment nodes (64 Ki and 256 Ki until the strict limits; past either the widget now stops, "Strict limits from the start"), and 8192 complex-script units across text nodes. Removal and shrinking release the document budget; resetting a pooled native validator clears it. Since the pre-release hardening the budgets also charge `data-*` / `aria-*` values and the strings of sanitized CSS (see "Strict budgets reach attributes and CSS strings").

Text mutations longer than 256 UTF-16 units, or containing complex-script, CJK, Kana, Hangul or fullwidth characters, also consume a shared token bucket with an 8192-work-unit burst and a 1024-unit-per-second refill; short Latin, digit and symbol text such as ticker cells is never charged, so a 400-cell ticker updating every frame keeps all its updates. Work counts words separated by whitespace, hyphens, question marks, slashes, or zero-width spaces, and counts CJK, Kana, Hangul, fullwidth and supplementary characters individually. Long unbroken runs cost at least one unit per 1024 UTF-16 units. Creating, replacing, splitting, or removing nodes cannot refund mutation work. Rejected mutations preserve the last accepted text. Compatibility policy keeps its existing limits.

The TypeScript validator and Swift native validator enforce the same accounting. The native JS bundles and matching QuickJS bytecode are regenerated for iOS/macOS, Android, and desktop together.

### CPU time, not wall-clock time (Oct 2026)

- The slice and the quota count the JS thread's own CPU time
  (`CLOCK_THREAD_CPUTIME_ID` in Swift, C++ and the Android JNI layer,
  `GetThreadTimes` on Windows), not wall-clock time. On a loaded machine (a
  build, a busy chat list, other widgets) wall-clock slices killed widgets that
  had used a fraction of their budget. Waiting for a sync query no longer needs
  subtracting: a blocked thread uses no CPU.
- Wall-clock time stays as a backstop, `max(10 x slice, 5 s)`, for a thread
  that is starved or stuck outside JS.
- The web worker and the Node harness keep wall-clock slices: browsers and
  Node expose no per-thread CPU clock to a worker. A loaded browser can still
  throttle or kill a widget early there; the quota policy (`cpu-budget.ts` and
  its twins) is unchanged, only what the native glue feeds it.

### Layout queries are bounded per task and charged to the quota (Oct 2026)

A synchronous layout query (`offsetHeight`, `getBoundingClientRect`,
`getComputedStyle`, …) blocks the sandbox thread until the host's renderer has
answered. Only the CPU of the JS thread counted toward the slice, and the time
spent waiting moved the deadline, so a widget looping on `a.style.width = …;
a.offsetHeight` burned almost no CPU of its own, was never interrupted, and
kept the renderer answering forever: the host was busy, the widget alive. Found
by Misha on the macOS client (54bdfa5); now the rule on every host:

- the wall time of a task's queries is summed (`taskQueryMs`); past
  `queryBudgetMs = 100 ms` (`cpu-budget.ts` and its three twins, checked against
  `tests/fixtures/cpu-budget.json`; `max(slice, 100 ms)` until the strict limits, so up to
  500 ms at boot) the task is
  interrupted at the next poll (the CPU deadline is moved into the past) and
  killed with "Layout query limit exceeded: host queries blocked the renderer
  for more than N ms in one <kind> task"; any further query of that task is
  answered `{"error":"query budget exceeded"}`;
- the query time is charged to the host's main-thread window (with the modelled
  cost of the task's ops; it was charged to the CPU quota with the task's CPU
  until the strict limits): a layout-heavy widget pays for what it costs the host,
  and the window throttles it like a busy one;
- both counters reset at the start of each task.

The budget is per task, not per widget, so an ordinary widget that measures a
few elements per frame is far from it (tens of queries at a millisecond each),
while a forced-layout loop reaches it within the first task. The Node harness
keeps the same accounting (its answers are in-process, so only a slow `answer`
callback can trip it). `corpus/attacks/layout-query-loop.html` is the attack.

### 64 timers, and an error instead of a hang

- Telegram allows 16 timers; browsers have no limit. D3 transitions and
  staggered animations put dozens of timers in flight, so 256 was the default,
  configurable (`maxTimers`); 64 since the strict limits (the busiest widget of
  the corpora holds 31).
- The cap used to be 2000 and silent: the glue dropped the timer and the shim
  returned an id whose callback never came — a hang with no error. Now the
  shim throws `RangeError('too many timers (max N)')` (reported once as
  `timer-limit`), the glue keeps its own count as a backstop and reports
  `timer-limit-enforced` if it is ever hit (which means a shim bug or a
  tampered shim). Delays are clamped to 24 h on every platform.

### The last render stays on screen when the runtime dies

- The renderer was never told that the runtime had died: buttons kept
  "working", frames and events went nowhere. The host now sends `frozen` and
  the renderer makes the snapshot honest: `pointer-events: none`, a slight
  grayscale, a badge ("Widget stopped", host-provided text, no error details),
  no more event forwarding, pending frame cancelled. `destroy()` stays an
  explicit owner action, on every platform.
- Mid-task kills are atomic for the renderer: the shim flushes ops at the end
  of a task only, so a handler that mutates 500 nodes and then hangs leaves
  none of them on screen (`fallback-mid-batch.html` pins this down).
- One difference from a browser, documented by `fallback-at-boot.html`: the
  whole document is parsed and shipped before the first script runs, so
  markup *after* a hanging boot script is visible too.
- Non-fatal uncaught exceptions get a dedicated `onError` callback on all
  three hosts, so a product can show an error line under the bubble the way
  Telegram does; the dev UI and the iOS demo show the last one until a later
  task completes without one, plus a Restart action (a new host from the same
  HTML).

### The memory limit is the wasm memory's maximum (web only)

- Found with `corpus/attacks/mem-*.html` (Oct 2026): a widget retaining 1 MB
  typed arrays grew the runtime past 1.2 GB with `rt.setMemoryLimit(64 MB)` in
  place, while one 256 MB allocation was refused. quickjs-ng counts its heap as
  Σ `malloc_usable_size(ptr)`; `cutils.h` implements that for Apple, Windows,
  glibc/Android and wasi but has no `EMSCRIPTEN` branch, so in the
  quickjs-emscripten build every block counts as 0 bytes (+ 8 of overhead) and
  the limit check `malloc_size + size > limit` only bites for a single
  oversized allocation. `computeMemoryUsage().memory_used_size` walks the
  object graph and does include `ArrayBuffer` bytes, which is why the stats
  showed the growth that the limit did not see.
- **Fix in the glue, not the shim**: the wasm module imports its memory, so the
  worker (and `packages/core/scripts/sandbox-node.ts`) creates it with
  `maximum = INITIAL (16 MB) + memoryLimitBytes` and passes it through
  `newVariant(..., { wasmMemory })`. When dlmalloc cannot grow the memory,
  quickjs throws the same catchable `InternalError: out of memory` that
  `JS_SetMemoryLimit` throws natively; the glue keeps working at the cap
  (host → VM strings, calls) and memory is reusable after the widget frees it.
  `setMemoryLimit` stays as the cheap first check for one huge allocation.
  The footprint of a worker is therefore bounded by 16 MB + the limit (24 MB at
  the 8 MB default of the strict limits, 80 MB at the 64 MB it was); a polling or
  interrupt-handler guard was rejected because quickjs polls interrupts every
  10 000 back-edges, i.e. gigabytes of 1 MB allocations between polls.
- Semantics match native: a widget that catches the error lives on at the cap
  (`mem-steady-growth.html` shows "62 (alloc refused: InternalError)"), an
  uncaught one is killed with "Memory limit exceeded (64 MB)" and keeps the
  last render frozen (`mem-uncaught-growth.html`). Verified natively by
  compiling the vendored quickjs-ng v0.17.0 on macOS (Apple `malloc_size`):
  steady growth capped at 62 MB, burst refused, arrays capped. Android uses
  bionic `malloc_usable_size` through the `__linux__` branch. Swift, Kotlin and
  the C++ core need nothing extra.

### Strict limits from the start (2026-10-29)

Widgets are not launched yet. A limit can be loosened later without breaking anything; tightening it breaks widgets
that worked, and with honest strict limits bots adapt to them (Artem, 2026-10-28: "лучше посильнее зарезать на старте").
So every count and size limit was re-measured over 346 widgets of the corpora and the conformance set (5 s each in the
dev app, a tap on the first button; memory also in the native C++ harness) and set to about twice the largest real use:

| Limit | Largest real use | Limit | Past it |
|---|---|---|---|
| QuickJS heap | 3.1 MB (1.9 MB of it the runtime and the shim) | 8 MB (was 64) | stops |
| Live timers | 31 | 64 (was 256) | `RangeError` |
| Live script animations | 4 | 64 (was 1024) | not rendered |
| One text value | 12.9 K (a stylesheet) | 32 K (was 64 K) | stops |
| The document's text | 21.7 K | 48 K (was 256 K) | stops |
| An attribute value | 1 066 (an inline style) | 8 K (was 4 MB) | stops |
| Ops in one batch | 2 700 | 10 000 (was 200 000) | stops |
| CSS rules / `@keyframes` | 167 / 12 | 400 / 32 (no cap) | stops |
| Canvas commands per canvas and task | 237 | 2 000 (was 400 000 buffered) | `RangeError`; stops past it in a batch |
| Web Storage per area | 585 characters | 4 KB (was 16 KB) | `QuotaExceededError` |
| Layout queries per task | 14.6 ms over a whole run | 100 ms (was the slice, up to 500 ms) | stops |
| Canvas pixels, all of a widget's canvases | 0.41 M (736 × 560) | 2048² = 4.2 M (the audit's 4096²) | stops |
| A data: image's pixels | 800 (40 × 20) | 1024² = 1 M (the audit's 4096²) | the placeholder |

- **Stop, not drop.** A counted cap used to drop the op with a diagnostic and the widget went on with a hole in its
  document, whose look depended on what had passed. Now the validator (`validator.ts`, the same bundle on every native
  host, and Swift's `NativeValidator`) sets `stopped`, reports the `limit` diagnostic with the reason and refuses that
  batch whole and every later one; the host stops the widget with that reason, as for memory or CPU, and the bubble
  keeps its last render. The author sees the cause at once. The complex-script budget and text churn still drop:
  a widget in those scripts needs all of its budget, and the numbers did not change.
- **The shim refuses at the same numbers**, so the author gets an error inside the widget where the host would stop it:
  timers (`RangeError`), script animations (not rendered, `animation-limit`) and canvas commands (the 2001st command for
  one canvas in one task throws `RangeError`; the validator stops a batch with more, i.e. a tampered shim).
- **The main-thread window.** The JS quota (300 ms per second, unchanged: a widget's JS runs on its own thread, and the
  busiest widgets on a weak phone are already throttled by it) no longer pays for what the widget costs the client's main
  thread. That is a second `CpuBudget` in the glue, `mainThreadQuotaMs` = 100 ms per `cpuQuotaWindowMs`: the wall time of
  the widget's layout queries plus a modelled, deterministic cost of the ops the glue forwards (25 µs per op, 2 µs per
  canvas command; `opsCostMs` in `cpu-budget.ts` and its twins). A deferrable task waits while either window is spent.
  The busiest widget of the corpora (`htmlbubbles-ported/06-trip-lisbon/10-ana-bare`, a list redrawn whole) costs
  82 ms per second; 1000 elements moved every frame get about 6 frames a second instead of 60.
- **Bitmaps.** The tweb audit (`docs/changes.md`, "Bitmaps a widget makes the page hold are bounded") found that
  nothing bounded the bitmaps a widget makes the page hold: four canvases of 16384² grew a browser by ~16 GB, and a
  data: image is decoded at the size its header claims (2 KB of a one-colour PNG is a real 4096², 64 MB; the attribute
  cap bounds the bytes, not the pixels). The audit capped both at 4096² (64 MB) in the renderer and in `checkUrl`; by
  the rule above they are lower. The corpora's canvases, run at a devicePixelRatio of 3, hold at most 0.41 M pixels
  together, but the guide tells authors to scale a canvas by devicePixelRatio, and one across a whole 430 px bubble at
  3× is 2.3 M: so the validator stops a widget whose canvases would hold more than 2048² together (a new canvas is
  300 × 150; its `width` and `height` attributes and the shim's `r` command size it; removing it refunds it). The
  renderer keeps the audit's 4096² as its own backstop for compat and for any path past the validator. A data: image
  is a URL, so past 1024² it gets the placeholder as a refused URL does, and the widget goes on: the corpora's largest
  is 40 × 20, and an image across a 320 px bubble at 3× fits.
- **compat keeps the old caps** and never stops: tests, tooling, and the measuring runs of the stress kit
  (`packages/stress`), which must see the cost past the limits.
- Not in this change: the count of heavy boxes (shadows, filters, opacity) and the renderer's frame cost of the
  paint-limits draft, and a lower node cap. They depend on the client renderers, so they wait for the stress kit's
  numbers from the clients.

### Off-screen pause mirrors a background tab

- The host knows visibility, the sandbox does not: `setVisible(false)` stops
  frame requests (one is made on return if the shim asked meanwhile), runs
  timers at 1 Hz through the same deferred queue the CPU quota uses, and gives
  the shim `document.hidden` / `visibilitychange` — the signal widgets are
  already written against (the clock and the generated video players pause on
  it). Timers set while hidden are armed with `max(delay, 1000)`; due ones are
  batched into one wake-up; intervals never catch up on missed ticks.
- Who calls it: the web dev UI from an `IntersectionObserver` on the iframe
  plus the page's own `visibilitychange`; the iOS host from the container
  view's window attachment combined with the owner's call (the demo wires
  `scenePhase`); the Android host from the WebView's attach state combined
  with the owner's call. Snapshot-and-drop after N minutes hidden
  (`native-port-notes.md`, Lifecycle) is still open; the hidden state it needs
  now exists.

## QuickJS for the web and Node from our fork (Oct 2026)

- The npm wasm build (`@jitl/quickjs-ng-wasmfile-release-sync` 0.32.0) vendors
  quickjs-ng 0.12.1, which frees a mapped `arguments` object's variable refs too
  early (quickjs-ng#1400): any non-strict widget function reading `arguments`
  plus enough allocation to run the GC corrupts the heap and aborts the worker
  (`gc_scan`, `mark_weak_map_value`, out-of-bounds; the assertion moves with the
  heap layout). The native builds (quickjs-ng 0.17) were not affected; ASan runs
  of the same widget on the Swift and C++ cores are clean.
- `packages/quickjs-wasm` (`@telegram/widget-sandbox-quickjs`) is that variant
  rebuilt from the TelegramMessenger/quickjs fork: `build.sh <fork checkout>`
  amalgamates the fork with its own `qjs`, applies quickjs-emscripten's one
  quickjs-ng patch and compiles quickjs-emscripten 0.32.0's `interface.c`
  (adapted to quickjs-ng 0.17's ArrayBuffer and interrupt-handler signatures)
  with the variant's emcc flags. `@jitl`'s FFI and `quickjs-emscripten-core`
  are unchanged; the output in `dist/` is committed, so the web build and CI
  need no emcc (it is excluded from the repository-wide `dist` ignore rule on
  purpose). `dist/QUICKJS_VERSION` names the fork commit; `build.sh` defaults to
  the `packages/ios/Sources/CQuickJS/quickjs` submodule, so the wasm and the
  native builds come from the same commit.
- Behaviour change, now the same on every platform: out-of-memory is
  uncatchable in quickjs-ng 0.17, so a widget that hits the cap is stopped
  (`Memory limit exceeded`, last render frozen) even inside `try/catch`.
- The web validator is not in this wasm: it runs in the host page's own JS
  engine on the main thread (`WidgetHost`); only widget code runs in the
  worker's QuickJS heap.

## User activation for privileged bridge calls (Oct 2026)

- `openLink`, `sendMessage` and any future privileged bridge method act for the
  user, so a widget may only call them from the handler of a real gesture: one
  physical pointer press (`pointerdown` … `click`/`dblclick`) on the visible
  widget unlocks one call. Keys never do (no keyboard for widgets, "Profile
  v1" below), nor does the click a key activates (`detail` 0). Timers, frames,
  promise jobs after the task, scripted `click()`/`dispatchEvent` and hidden
  widgets get an `activation-blocked` diag instead. `ready` stays free.
- A finger's or a pen's press is a gesture from its `pointerup` on (Oct 2026, after
  v0.2.3), as HTML's activation-triggering input events have it: its `pointerdown` may
  still become the chat's scroll, which ends it in `pointercancel`, and a swipe that
  started on a widget let it copy text and send a request. The renderer core marks
  such a `pointerdown` not `trusted`, nor a mouse event of a finger or a pen still
  down: the `mousedown` a pen's contact makes at once, its barrel button pressed or
  let go with the tip down (by the pointer whose buttons changed last: a mouse
  pressed while a finger is held keeps its own `mousedown`; by the input's own
  `pointerType`: a mouse that
  plays a finger in the dev app's phone mode keeps its press), so the rule is one
  JS change for every host. The counters no longer see that `pointerdown`, so every
  host starts a new gesture at the renderer's `gesture` claim, which comes before
  the press's events for every primary press, listened to or not, and at every
  `pointerdown` event, trusted or not, which a second finger (no claim) has
  (`GestureCounter.pressBegan`): without it a finger's release and click after a
  press that ended without a click (a drag) continued that press's spent gesture.
- Native glue (Swift, Kotlin, C++): the host numbers gestures from renderer
  events marked `trusted` and hands the number to the engine with the event;
  the engine opens the gesture for that event's task only and consumes it with
  the first privileged call. The renderer is trusted code and the only source
  of events, but a WebView renderer also sees events its own page dispatched,
  so the hosts take its word (`isTrusted`) like the web host does.
- Web: the worker shares a thread with nothing trusted, but it is a separate
  realm whose glue we would rather not have to trust with this decision, so
  the main-thread host decides. The renderer marks an event `trusted` only
  when the DOM event's `isTrusted` is true; the host numbers gestures from
  trusted events only, sends the number with the event, and accepts a
  privileged `bridge` message only when it carries a gesture the host issued,
  has not consumed and issued within 5 s. A worker that lies can at most spend
  a real gesture once, which is what the gesture allowed anyway.
- One policy in four copies: `packages/core/src/host/user-activation.ts`,
  `UserActivation.kt`, `WidgetHost.gesture(for:)` + `SandboxEngine` in Swift,
  `GestureCounter` (`widget_host.h`) + `SandboxEngine` in C++, each with its tests
  (`user-activation.test.ts`, `UserActivationTest.kt`, `UserActivationTests`,
  `bridge_and_timers`); attack widgets `corpus/attacks/activation-*.html`.

## Desktop C++ core (`packages/desktop/`, Oct 2026)

Written for the tdesktop question: the part of
the stack that is new on a Qt/C++ client is the glue, so the glue was ported
first and alone, as a library with no renderer.

- **No Qt in the core.** `std::thread` cannot choose a stack size, so
  `js_thread.h` is a 150-line pthread/`_beginthreadex` loop (tasks, timers,
  `sync`) rather than a `QThread`; the engine's delegate is a plain abstract
  class and the host hops to its UI thread itself. tdesktop can wrap it in Qt;
  the Node-style harness and the tests do not need Qt at all, which keeps the
  attack and compat runs a `cmake --build` away on any machine.
- **No JSON library either.** The glue parses only what the shim sends
  (`[id, delay]`, `[method, args]`, `nextScript`, `stat`, Intl requests) and
  builds the boot config; `json.h` is a 300-line strict parser with a depth
  bound, which is what hostile input needs and all a dependency would add.
- **quickjs-ng is compiled from `packages/ios/Sources/CQuickJS`**, through the same
  `qjs_shim.c` (quickjs.h's `JS_UNDEFINED` & co are C compound literals, not
  C++). One engine, one shim, one set of sources, now on four platforms.
- **Validator unchanged**: the same `validator-bundle.js` in a second trusted
  runtime (`TrustedValidator`), exactly as on iOS and Android. 256 MB heap,
  6 MB QuickJS stack on an 8 MB thread (6 340 JS frames in release — css-tree
  needs > 40).
- **Fourth `CpuBudget` copy** (`cpu_budget.cpp`) against the shared fixture;
  the engine (`sandbox_engine.cpp`) is `SandboxEngine.swift` line for line:
  same `__host` kinds, same deferred queue, same 1 Hz hidden timers, same
  kill messages.
- **`Intl` is an interface** (`IntlProvider`); `BasicIntl` is the fallback without
  locale data: en-US formatting with CLDR plural rules and a few dozen languages'
  number separators, checked against V8 on the `basic` cases of
  `tests/fixtures/intl-cases.json`. A product host plugs `QLocale`/ICU in —
  deliberately not done here, to keep the core Qt-free.
- **Harness leak scan instead of a canary.** Without a renderer there is no
  network to hit, so `twidget-run --attacks` inspects the *validated* ops: any
  non-`data:` URL in an attribute, any `url()`-family function not pointing at
  `data:`/`#`, any `@import` is a leak. First run flagged three false
  positives — external URLs inside `data:image/svg+xml` payloads and a
  `"http://…"` string token in a CSS custom property — all inert in browsers
  (SVG-as-image loads nothing external; a string is not a `url()`), confirmed
  by the Playwright suite's zero canary hits on the same files. The scanner now
  reports the data: cases as notes. A native, non-browser renderer would have to
  honour the same two properties; that is a constraint on option D in the
  feasibility note, not on the validator.
- Results on the first green run: 21 tests / 180 checks; compat 21/21 and
  generated 40/40 load without uncaught errors; attacks 89, 0 leaks, one
  expected kill (`fallback-at-boot.html`). Sandbox bundle boot: tens of ms in
  release quickjs-ng from source (bytecode still open).

## One QuickJS for every platform: the TelegramMessenger/quickjs fork (Oct 2026)

- `packages/ios/Sources/CQuickJS/quickjs` is a git submodule of the
  TelegramMessenger/quickjs fork (quickjs-ng 0.17 plus fixes and speed-ups),
  replacing the vendored quickjs-ng 0.17.0. The iOS/macOS package, the Android
  library, the desktop core, `packages/tools/qjsc` and the web/Node wasm
  (`packages/quickjs-wasm`) all build from it; clone with
  `git submodule update --init` (SwiftPM does it for package dependencies).
- The fork's interrupt handler receives the `JSContext`, not the runtime; the
  C++ and JNI glue follow.
- Every `.qbc` was regenerated with qjsc built from the fork;
  `BundledBytecodeTests` (Swift) fails when a shipped `.qbc` is not exactly what
  the engine compiles from the shipped `.js`.
- The fork makes out-of-memory uncatchable everywhere; the memory attacks that
  catch the error are now stopped like the ones that do not.
- Fork master c24d6b0 (2026-10-16) makes the uncatchable state a property of
  the runtime, not only a bit on the error object: until the embedder takes the
  exception with `JS_GetException` outside any JS call, no catch block, Promise
  executor, thenable, async generator, disposer, iterator `return()` or
  `Error.prepareStackTrace` can observe it, and every interrupt poll raises it
  again. Before it, `for (;;) new Promise(function () { for (;;) {} })` turned
  the CPU interrupt into a rejection and never returned to the glue (found by
  the fork review below). The glue needs no change: every host already calls
  `JS_GetException` at top level. Bytecode format unchanged.

### The QuickJS fork review (Oct 2026)

The TelegramMessenger/quickjs fork at 7d1085a is upstream quickjs-ng 9b06171
(v0.17.0 plus 21 upstream commits) plus 22 commits of its own: allocator arenas
and slabs, inline interpreter paths, early allocation of shapes, dtoa changes.
All 22 were read against upstream; **the fork is accepted** and every commit
kept. No memory-safety bug was found; three medium-risk commits (27787d3 slabs,
e65afb8 zombie shapes, ee1ef12 inline properties) carry follow-ups rather than
blockers. Checked: test262 at the same 40 of 82 220 failures as upstream; the
fork's `tests.conf` and `api-test` on Release and under ASan + UBSan (Apple
clang does not define `__SANITIZE_ADDRESS__` under `-fsanitize=address`, so the
flag has to be passed by hand or the arenas stay on and ASan is blind inside
small blocks); a differential dtoa run (the Grisu3 change is right, matching
V8, 54 powers of two differ from upstream); instruction counts over the desktop
corpora (fork −8.7 % against upstream); footprint after 30 freed runtimes
(7.5 MB against 13.6 MB, the slabs). The one release blocker was upstream's,
not the fork's: the CPU interrupt swallowed by Promise executors and the other
constructs above, fixed by the patch now in fork master c24d6b0, which the
submodule pins. Still owed: fuzzing of the allocator and interpreter commits
(a differential fuzz against upstream over the corpora exists as a harness, not
as CI), test262 on CI, and a second pair of eyes on the uncatchable-exception
patch from the fork's owner.

## Bytecode precompilation of the bundles (Oct 2026)

- **Build time, not first launch.** `packages/core/scripts/build-native-bundles.ts` compiles
  `sandbox-bundle.js`, `validator-bundle.js` and `libs/*.js` to `.qbc` with
  `packages/tools/qjsc/qjsc.c`, built on the fly from the vendored
  `packages/ios/Sources/CQuickJS` (one quickjs-ng copy for iOS, macOS, Android and the
  desktop core, all 64-bit little-endian, so one set of blobs serves every
  port). A first-launch cache was rejected: it keeps the slow first boot and
  adds cache invalidation code to three ports for no gain, since the bundles
  never change at runtime.
- **A stale blob is refused, never run.** The `TWQB` header carries the
  source's length and FNV-1a 64 hash; `qjs_eval_bytecode` compares them with
  the `.js` the host would otherwise evaluate and quickjs-ng checks its own
  `BC_VERSION`. Any mismatch is an exception; the glue falls back to the source
  and reports a `bytecode` diag. A test on each platform boots a widget with a
  bundled library and asserts `(bytecode)` in the `boot` and `library` diags and
  no fallback, so a `.js` committed without its `.qbc` fails CI, not a phone.
- **Only our own code goes through `JS_ReadObject`.** It has no verifier
  (quickjs.h says so); widget scripts keep going through the parser. Debug
  info is kept for shim stack traces, source text is stripped.
- **What it buys.** Desktop core, release quickjs-ng: sandbox boot 38–74 ms →
  20–22 ms, Chart.js 18 → 4 ms. The remainder is the bundle's top-level
  execution (building the shim's DOM classes); QuickJS cannot snapshot a heap,
  so that part stays. Cost: ~1.6 MB of `.qbc` per platform (1.2–1.5× the
  minified source).

## Chat feed on iOS: pools, one WebKit environment, validation off the main thread (Oct 2026)

`docs/chat-feed-ios.md` has the design and the numbers; the decisions:

- **Optimise the glue, not the bundles.** Every gain came from the host side
  (`RendererEnvironment`, `Pools.swift`, `WidgetHost.Options`); the three JS
  bundles, the validator and the invariants are unchanged, so the web, Android
  and desktop hosts keep working and can port the same ideas (Android:
  `WebView` pool + `SandboxEngine.prewarm`; the C++ core already separates
  runtime creation from boot).
- **Flags, not a rewrite.** `sharedWebKit`, `sharedValidatorThread`, `pools`,
  `parallelBoot`, `asyncValidation` are options with the old behaviour as the
  default of the single-widget path; the feed demo measures `baseline` against
  `max` in one build. A product turns on what it needs; the pools are filled
  by the app (`WidgetWarmup.prewarm`), never implicitly.
- **Engines are never recycled, validators are, renderers are not (yet).** A
  QuickJS runtime that ran widget code is dead on `destroy()`; a validator
  context ran only trusted code and is reused with a fresh
  `__twValidator.create()`; a `WKWebView` could be reused with a `reset` op in
  the renderer, but that would share one document (and origin) between widgets
  sequentially, weakening the per-widget origin of invariant 6 — left as an
  option to discuss, not done.
- **Order over speed in async validation.** Ops and sync queries go through
  one serial validator thread and then one serial main queue; a query
  therefore always follows the ops flushed before it. The alternative — a
  validator thread per widget — was rejected: a chat of 40 widgets would run
  80 threads for no gain.
- **Snapshot-and-drop is the list's policy, not the host's.** `WidgetHost`
  only offers `snapshot(completion:)`; the feed decides when (beyond
  `keepScreens` screens, after scrolling settles or every 0.5 s during a
  flick) and keeps the JPEG. The host keeps the off-screen pause
  (`setVisible`) as before.
- **Heights are the app's problem.** The renderer reports sizes; the feed
  remembers them per widget and refuses to shrink a bubble before `loaded`.
  A messenger would get the height from the server with the message.

## Android validator pool and the renderer bench (Oct 2026)

`docs/android-renderer-bench.md` has the numbers; the decisions:

- **The validator half of the iOS pools, nothing else yet.** `ValidatorPool`,
  `WidgetHost.Options.pools` / `sharedValidatorThread`, `TrustedValidator.background`
  / `reset` / `sharedThread` mirror `Pools.swift` and `TrustedValidator.swift`
  one to one; renderer and engine pools stay open. The pool is filled by the
  app (`ValidatorPool.prewarm`), never implicitly, and a pooled validator is
  recycled on `destroy()` with a fresh `__twValidator.create()`.
- **It buys start-up, not memory**, and the doc says so: the bundle evaluation
  (~17 ms on a phone) leaves the main thread and the bubble's critical path, one
  thread serves every validator; a live widget still owns a ~3.7 MB validator
  context. One context per process with an instance per widget would halve
  that, but it is a change to `validator-bundle.ts` and every platform's `.qbc`,
  so it is written down as the next step rather than done here.
- **Measure the renderer as a difference.** The bench's `none` renderer (engine +
  validator, no renderer) is the floor every renderer is compared against; on a
  phone the engine floor is 6.3 MB per bubble, WebView adds ~14 (mostly graphics),
  Blitz ~7 (its CPU bitmap and the texture of it), litehtml ~1.3.

## Renderer choice and the core: two tracks, one profile (Oct 2026)

Written after the Android renderer bench (`docs/android-renderer-bench.md`),
the three-way renderer matrix (`docs/blitz-experiment.md`,
`docs/litehtml-renderer.md`, `corpus/common`) and the iOS chat feed
(`docs/chat-feed-ios.md`).

- **The core is renderer-independent, by evidence, not by design intent.**
  Three renderers (WebView, Blitz, litehtml) run on five hosts behind the
  `Renderer` seam with the three JS bundles, the validator and the invariants
  untouched; the attack suite is at zero on every combination. Work on the
  core (bytecode, pools, snapshot-and-drop, validator memory, engine boot)
  proceeds independently of which renderer a platform ends up with.
- **A WebView in every bubble is not the product path on Android or
  Telegram Desktop.** Android, phone: the first WebView in the process costs
  168 ms of main-thread stall and ≈ 120 MiB that never come back, then
  ≈ 14 MiB per live bubble, most of it graphics memory the emulator does not
  show; prewarming moves the stall to app launch, it does not remove it.
  tdesktop: inline is unreachable on Linux (WebKitGTK helper process) and a
  whole Edge runtime on Windows. On iOS the
  pooled WKWebView path measures as good enough (ready before display,
  126 MB peak for 120 bubbles) and stays a candidate there.
- **Once the WebView is out, the engine is the biggest per-bubble cost.**
  Phone, 20 live bubbles, over the `none` floor: engine + validator 6.3 MB,
  litehtml ≈ 1.3, Blitz ≈ 7 (its bitmap and texture; the engine is the
  bigger half of a Blitz bubble too). The widget's own heap is 1.7 MB of that;
  the rest is the runtime, the validator context (≈ 3.7 MB of css-tree per
  live widget) and two threads. A shared QuickJS runtime with one context per
  widget was rejected: one memory limit and one interrupt handler for all
  widgets lets one widget starve the others. The core track's levers are
  therefore *fewer live runtimes* (snapshot-and-drop, pools, fast boot from
  bytecode) and *one validator context per process with an instance per
  widget*, not a smaller runtime.
- **Per platform for the implementation, one profile for the promise.** A
  platform may pick its renderer, but the widget profile must be one across
  platforms: the same file has to draw the same picture on every client or
  the format is not a format. The profile is the intersection of the chosen
  renderers; `corpus/common` (32 widgets all three draw alike) is that
  intersection today and the acceptance test for any renderer on any
  platform. Every extra renderer family narrows the profile, which is the
  argument for converging on one native engine everywhere (Blitz and
  litehtml both already build on all five hosts, wasm included) and keeping
  the WebView for the expanded widget, not the bubble.
- **What moves with the renderer** and belongs on its checklist, not in the
  core: the answers to layout queries and `measureText`, the canvas command
  stream, event delivery and form state, and the replacement for invariant 6
  (a native renderer has no CSP and no separate process, so it needs a layout
  timeout, `catch_unwind` around every call, and a memory cap per document;
  the 160–190 ms layout outlier on an emoji font fallback in
  `docs/blitz-experiment.md` is the DoS vector that timeout exists for).
- **Options, ranked.** (1) Blitz as the product renderer: the only open
  engine above litehtml's profile (grid, var, calc, shadows, 2D transforms)
  at 3 ms per card; gaps are concrete (canvas on vello, forms, CSS into SVG,
  `overflow: auto`, 3D) but it is beta, Rust in three client builds, and
  14 MB of `.so`. (2) Hybrid by capability: the validator sees the whole DOM
  and CSS before rendering, so "uses canvas / 3D / `<input type=range>`" is a
  deterministic flag; such widgets get a static native card in the bubble and
  the full WebView on tap, like a Web App — one prewarmed WebView per app,
  never a hundred. (3) litehtml with a reduced CSS profile: cheap, but 17 of 40 generated widgets use grid and that is not fixable in a
  sanitizer. (4) A renderer of our own on platform text (variant D): months,
  not before Blitz has failed. Recommendation: (2) as the product shape with
  (1) as its renderer, conditional on the measurements below.
- **Not measured yet, and the recommendation rests on it:** the chat feed on
  Blitz (the iOS feed takes no `--blitz`), Blitz on `corpus/compat` (Chart.js
  and D3 need the canvas), 3× rasterisation on a device (CPU vs vello on the
  GPU), Blitz's size after strip, a low-end phone for the WebView stall, the
  renderer and engine pools on Android.

## Opt-in WebKit view reuse (Oct 2026)

`WidgetHost.Options.recyclesRenderers` returns shared-environment WebKit renderers
on destruction when pools are enabled. It remains off by default. The QuickJS
runtime is terminated; it is never reused after running a widget.

A returned WKWebView replaces its current history entry with a new trusted renderer
document at a fresh random origin, with a fresh CSP nonce. Repeated reuse does not
accumulate old pages in the back-forward list. The previous document is unregistered, callbacks
and buffered messages are cleared, and messages from the previous frame URL are
rejected. Only a new `ready` message admits the view to the idle pool. Failed or
timed-out resets destroy the view. This preserves per-widget origins rather than
resetting untrusted content inside a shared document.

The pool holds at most twice its warm target, counting resets in flight. Draining
also cancels pending refills and resets. Memory pressure drains the pool and
suppresses refills/recycling for 30 seconds. Async bubble snapshots are accepted
only for the host generation that requested them.

## Pre-release hardening of the integration branch (Oct 2026)

Release plan decisions D2, D5, D6 and D13, and the blockers found by the
pre-release branch review (2026-10-16), before `telegram-macos` becomes `main`.

### `strict` is the default policy, parsed fail-closed

The gesture gate for privileged bridge calls, the 64 KB text caps and the text
budgets applied only under `strict`, and every glue copy defaulted to `compat`:
a client that forgot the option let `openLink` run from a timer. `strict` is now
the default in all five copies (web `WidgetHost`, Node harness, Swift
`SandboxConfig` / `WidgetHost.Options`, Kotlin, C++). Kotlin and C++ keep a
string option; anything but exactly `"compat"` is strict (`isStrictPolicy`,
`resolvePolicy` in TS, `Validator.setPolicy`), so a typo never weakens the
sandbox. `compat` stays for tests and tooling.

### Strict budgets reach attributes and CSS strings

The budgets of "Strict-policy text limits" charged text nodes, form values and
five visible attributes (`value`, `placeholder`, `alt`, `label`, `title`). A
widget could still put 60 000 ideographs on screen through `data-x` and
`content: attr(data-x)`, or through a string in a custom property and
`content: var(--x)`. Under `strict` now:

- `data-*` and `aria-*` values are charged like the visible attributes
  (`isTextAttr`, exported to the Swift native model through `tables()`);
- `attr()` may only read those attributes; any other name drops the declaration;
- the string tokens of a sanitized `style` attribute, of each `style` op and of
  each sanitized stylesheet are charged too, decoded (`cssStringText`), so
  `\5b57` escapes count as the ideographs they draw. The raw stylesheet text
  stays charged as text nodes; strings are short in real widgets, so the double
  count costs nothing. An over-budget property is cleared, an over-budget
  stylesheet ships empty.

SVG path data, classes and other attributes that cannot become text stay
outside the budgets: charging them would break animated SVG under the churn
bucket.

### One canvas payload per reference

With `canvasWords` a batch referenced a word payload by key, and nothing
stopped a batch from naming the same key thousands of times: each reference
re-validated (native model) or re-serialised (JSON splice) the whole payload,
which could hang the validator thread or exhaust memory from one message. A key
now counts once per batch: a binary batch with a repeated key is rejected whole
(`BinaryOps.Failure.canvasKey`, mirrored in `binary-ops-decode.ts`), a JSON batch
splices only the first placeholder and the validator rejects the rest, and the
Node harness takes each payload once.

### Web Animations records

`values` of a keyframe may no longer use `offset`, `easing`, `composite` or
`computed-offset` (validator and `NativeAnimValidator`), and the renderer
writes those members after the values, so a value can never override the
checked fields. A failed `setKeyframes` cancels the old animation instead of
leaving it running outside `MAX_LIVE_ANIMS`. Animation state moved from module
globals into `AnimationState`, one per renderer instance: on the shadow-DOM
renderer every widget shares the page, and widgets cancelled each other's
animations. A CSS-animation record applies only to an animation whose effect
target is the record's `target`. The web worker forwards `anim` only when
`webAnimations` is enabled.

### 3D side channel (`__tw3d`): out of release builds

`Widget3DChannel` (twidget kit) installs `__tw3d`, a C function that takes an
op string and byte buffers from the widget's heap (up to 64 MB per call) and
hands them to a Metal backend: a second capability next to `__host`, with no
validator, policy, gesture or visibility check. Decision D6: 3D is cut from the
release. `Widget3DChannel.install` is compiled only under `DEBUG ||
TWIDGET_EXPERIMENTAL_3D`; release builds keep the API (the kit still compiles)
but never expose the function and report a `3d` diag. Before it comes back:
a security review of the backend, ops checked by trusted glue against a schema,
visibility and CPU accounting, and `destroy()` that does not wait on the lock
the JS thread holds during a backend call (a main-thread hang today).

### Swift-only extensions and parity

`binaryOps` with the native node model (`NativeValidator.swift`), `canvasWords`,
`morphUpdates`, `icon:` expansion, side channels and the feed/pool machinery
exist only in the Swift glue; they are performance paths for Telegram macOS,
opt-in, and listed in `docs/changes.md`. Every glue now passes on only the
features it implements (`RUNTIME_FEATURES` in Kotlin, `WEB_FEATURES` in the
worker, the C++ engine's own list), and Android and the web take `features`
(so Web Animations work there). A platform copy of a validator part is allowed
only with parity tests (D13): the differential fuzz of `NativeValidator`
against the script validator runs under both policies, and the attack corpus
runs through the native model.

### Review follow-ups: gestures, harness clock, library patterns, `icon:`, CallSite

The lower-priority findings of the pre-release branch review, closed after the
hardening above:

- **Native hosts require `trusted`** (§3.5, §4.10). Swift, Kotlin and C++ counted
  any renderer event of a gesture type, the web host only `trusted` ones. No
  validated op makes the WebView renderer dispatch a click today, but the
  difference was one renderer feature away from a gesture forged by the widget.
  All four hosts now count only `trusted: true`. Blitz and litehtml have no
  script-driven document, so they mark every input event trusted; any other
  renderer behind the seam has to do the same or it gets no gestures.
- **Only Enter and Space were key gestures, one press one gesture** (since
  replaced: no key is a gesture, see "Profile v1"). Any
  non-repeat `keydown` used to count, so Tab or an arrow key while moving
  through the chat handed the focused widget an `openLink`, and Enter on a
  button was two gestures (the `keydown` and the click it activates). The
  click a key press activates comes with `detail` 0, which a pointer click
  never has; that click, and the clicks of a held key, continue the key's
  gesture. A pointer click (`detail` ≥ 1) after a key press is a new one.
  Letters do not count either: a widget that sends on Enter still works, one
  that would send on every keystroke does not.
- **Node harness** (§3.6): frame times now tick in 100 µs steps like `now`
  (the worker coarsens both), and the gesture gate checks visibility; a hidden
  widget (`RunOptions.visible: false` or a `{visible: false}` step) gets no
  frames, like the worker.
- **Provided-library patterns** (§4.11). A pattern is wrapped as `^(?:p)$`
  (TS) / `\A(?:p)\z` (Swift); `x)|(.*` closed the group early and left an
  alternative unanchored, so a prefix pattern could claim every URL that starts
  with it (the four probe URLs miss most such cases). A pattern now has to
  compile on its own first: balanced, it cannot leave the group. Patterns and
  bytecode stay trusted input from the kit.
- **`icon:`** (§4.13): the prefix is exact (`icon:`, as the grammar says);
  only `url(` keeps CSS's ASCII case-insensitivity. Next to the per-batch
  limits a widget now has a budget of its own: 256 distinct tokens asked of
  the resolver and 2 MB of answers kept. Without it a widget could cycle
  through tokens and make the resolver run up to 1024 times per batch, and the
  cache (512 entries of up to 64 KB, cleared when full) could hold 32 MB per
  widget. The budget does not refill; known tokens keep expanding, which is
  what a re-rendering widget needs.
- **CallSite** (§4.12): the fork's `CallSite.getFunction()` returned the
  function of every frame, so a widget handler could take the shim's closures,
  the dispatcher among them, from its own stack. The shim's prelude now
  replaces `getFunction` and `getThis` on the shared CallSite prototype with
  functions that answer `undefined` (V8 does the same for strict frames). The
  glue still checks gestures, limits and batches, so this was defence in
  depth; `fn.caller` and `arguments.callee.caller` already answer `undefined`.

### `completeEditorCommand`

The twidget renderers keep a textarea's Enter pending until the widget's
`keydown`/`keypress` handlers ran; the shim then calls
`completeEditorCommand(generation, command, allowDefault)`. It is documented in
`docs/protocol.md` ("Native editor commands") and the validators accept it only
on a `<textarea>` with integer arguments. Forms are outside the v1 profile
(D3a); the call goes when the profile drops them.

### The web renders through Shadow DOM; invariant 6 reworded

Decision D2 (2026-10-16): the web product renderer is the shadow-DOM renderer,
not the iframe. Invariant 6 now reads "a sandboxed frame with a per-widget CSP,
or a shadow root with a scoped validator inside a page whose CSP closes the
network". The frame was never the primary boundary: widget JS never reaches the
page (invariant 1), every op is validated (2, 4), and the renderer core refuses
forbidden tags, attributes and URLs itself and cancels every navigation and
submit. What the per-widget CSP added, the web client's page CSP has to give:

- `connect-src` only Telegram's own endpoints, no wildcards;
- `form-action 'none'`, `frame-src` / `child-src` without third-party origins;
- `img-src` and `media-src` without third-party hosts (`data:` and `blob:` plus
  Telegram's own), `font-src` likewise;
- `style-src` admits the renderer's `<style>` elements in the shadow roots
  (a nonce the renderer sets, or `'unsafe-inline'`), nothing remote.

`TW_RENDERER=shadow corepack pnpm test:attacks` keeps measuring exactly what
the frame used to add (0 canary hits, 0 requests).

## Profile v1: no keyboard, no form controls, a client-styled `<button>` (Oct 2026)

Decided 2026-10-17 (release plan D3a, D4). Implemented in the profile v1 and SDK
milestones; recorded here so the work in between does not drift from it.

- **No keyboard input.** Widgets get no key events and never take keyboard
  focus: Enter or Space in a bubble would fight the chat's input field. User
  activation for privileged bridge calls comes from a pointer press only: no
  `keydown` counts in any host, and a `click` starts a gesture only with a
  click count (`detail` ≥ 1), so the click Enter or Space produces on a focused
  control does not count either (done 2026-10-17, all hosts). Since profile 1
  the renderer forwards no key event, swallows the keys' default actions, never
  lets the focus into the widget (a mouse press moves no focus, a `focusin` is
  blurred) and drops a trusted click without a press (`detail` 0); `focus()`,
  `blur()`, `select()` and `scrollIntoView()` do nothing.
- **Form controls are outside the profile.** `input`, `textarea`, `select`,
  `option`, `optgroup`, `datalist`, `fieldset`, `progress` and `meter` are drawn
  by each engine in its own way (WebKit, Chromium, litehtml, Blitz), and text
  entry needs a keyboard. The validator drops them with their subtree; `<form>`
  renders as a `<div>` and `<label>`, `<legend>` and `<output>` as a `<span>`,
  so a button inside a form stays. The compat widgets built on controls moved
  to `corpus/out-of-profile/`; the other corpora keep theirs (they show what
  the profile removes). `completeEditorCommand` went with `textarea`. If
  controls come back, they look like the client's own UI.
- **`<button>` stays** (95 corpus files; the main tap target), and a button
  the widget did not style is drawn in the client's own style. Any author CSS
  wins over it.
  - Layer: the renderer, as a user-agent-level stylesheet. Neither the
    validator (it would change the cascade the author wrote) nor the shim (it
    draws nothing; `getComputedStyle` answers from the renderer, so the widget
    sees the client's real colours).
  - Form: one template in `packages/core` that every renderer inserts as
    trusted CSS before the widget's: `@layer tw-ua` declared first and
    `:where(button)` selectors, so unlayered author rules beat it whatever
    their specificity. It fixes the structure: base look, `:active`,
    `[disabled]` (`:disabled` itself is outside the profile and is rewritten to
    it), no focus ring. It is `tables.uaStylesheet` in `profile.json`, with
    text selection switched off in the same sheet (since v0.1.1 inside the layer and overridable, "Text selection" below).
  - Values are per-client tokens (background and text from `themeParams`,
    radius, font, padding, height, pressed and disabled opacity) that the host
    sets as variables on the renderer root and updates with the theme. Native
    renderers bake the same template into their master CSS with the values
    filled in.
  - The defaults (before the SDK delivers tokens) survive partial overrides,
    which widgets write against browsers: a translucent accent tint as the
    background and the inherited text colour, no minimum height. A first
    version painted white text on the accent colour and set a 34 px minimum
    height; a widget that styled only the background got white on white (9
    buttons in 6 corpus widgets), and one that set a small height got a taller
    button. With the tint and inherited text, 1 corpus button in the light
    theme stays under 2:1 contrast (the author's own colours) of 318. A client
    whose tokens set a light text colour on a solid background brings the
    first problem back; `docs/sdk-v1.md` says so next to the tokens.
  - Height and padding tokens stay within a narrow range: they move the
    bubble's height. Corpus references render with one fixed reference token
    set so renderers stay comparable; per-client tokens are checked by eye.

### Frozen as widget profile 1 (2026-10-17)

The candidate table went to Artem on 2026-10-17 and came back decided: grid
in, on condition that the Swift renderer and TwidgetCore implement it before
their releases; SVG CSS, `font-variant-numeric`, conic gradients and
`text-overflow` in; `touch-action`, `pointer-events`, the `ui-*` families,
`prefers-reduced-motion` and `Intl` `formatToParts` in; no bundled
libraries; the rarely used rest out until the native renderers are measured,
when profile 2 can add them. Freeze now, with these defaults.

- **One source.** `packages/core/profile.json`: the 255 features of the
  conformance catalogue with their status (173 in, 82 out; 174 and 81 since v0.1.1 made text selection opt-in), the decision each
  comes from, how each out feature is neutralised, and the tables that are
  enforced. `packages/core/scripts/profile-build.ts` generates
  `src/shared/profile-tables.ts` and `docs/profile-v1.md` from it;
  `tests/unit/profile.test.ts` checks both are current, that the features
  agree with the tables, and that `CSS.supports` answers what the sanitizer
  does. The catalogue in `packages/conformance` syncs its statuses from it.
- **Where it is enforced.** The validator (`allowlist.ts` builds its element,
  attribute, event, method, property and canvas lists from the tables;
  `css-sanitizer.ts` checks properties, functions, units, pseudo-classes,
  at-rules and media features, rewrites sticky and scrolling overflow, reduces
  font families, drops nested rules and decides `@supports`), the Swift node
  model (`NativeValidator` reads the same tables through `validatorTables()`,
  now with the SVG attributes and the replaced elements apart), the shim
  (`TelegramWidget.profile` and `isProfileAtLeast`, `CSS.supports` from the
  same evaluator as `@supports`, out-of-profile element methods and canvas
  read-back are local no-ops) and the renderer (no key, focus, touch, wheel
  or scroll events; `dblclick` and `contextmenu` only the renderer's own since
  v0.1.2: `dblclick` for every double tap, `contextmenu` on `touch-action:
  none`, "Gestures" below; the user-agent stylesheet).
  A removal the profile causes is the `profile` diagnostic; `css-dropped`,
  `url-blocked` and `tag-dropped` stay the security ones.
- **The Shadow DOM gaps resolved by the profile**: the top layer (`<dialog>`
  is a block, popovers and `showModal` are out), `@property` and `:scope` (out
  everywhere), and `scrollIntoView` and `#fragment` scrolling (out: on that
  renderer they scroll the chat).
- **No bundled libraries.** Chart.js 4 and D3 7 left the product: the CDN-name
  loader, `config.libraries` / `SandboxResources.provide` on every platform,
  the `WidgetJS/libs` bundles and their bytecode, and the npm dependencies. A
  `<script src>` to a CDN is an external script like any other and never runs.
  Kept dev-only it would still have been a code path in five glue copies and
  an API (`provide`) clients could build on; v1 starts with mini-widgets of
  4 KB compressed that cannot carry a library's worth of code anyway, the profile
  cannot describe a library's own rendering, and git keeps the code for a
  profile that wants it back. The compat widgets built on them moved to
  `corpus/out-of-profile/`; the generated ones stay (with the profile prompt
  they fall back to a table; without it they fail, which is what that batch
  measures).
- **Reference mode** (dev tooling) adds the renderer's user-agent stylesheet,
  so a sandbox-vs-reference diff shows what the profile removes, not the
  client's button style.


### The renderer's defaults: the root and the text (v0.1.1, 2026-10-20)

The first client implementations showed that a widget without CSS does not look
native: profile 1 defined no defaults for the root and the text, so every
renderer fell back to its own stylesheet. A WebView drew Times at 16 px with an
8 px body margin; litehtml, TwidgetCore and the Swift renderer each have their
own master CSS. The same widget looked different on every client and like the
client on none.

Artem decided (2026-10-20):

- **A full normalized UA layer** for the profile-1 elements, in the same
  `@layer tw-ua` as the button template, declared first, every selector in
  `:where()`, so any unlayered author rule wins. `html` takes the bubble's text
  colour and the client's text font; `body` takes the client's padding token as
  its margin; headings, paragraphs, lists, `dl`, tables, `hr`, `pre`/`code`,
  quotes, figures, images and the inline elements get explicit sizes and
  margins in `em` (the table in `docs/sdk-v1.md` §2.4).
- **Text tokens next to the button tokens** on every host (`WidgetTheme.text`:
  `fontSize`, `lineHeight`, `padding` in TS, Swift, Kotlin, C++, and in the Node
  harness's renderer message), set as `--tg-text-*` on the renderer root. The
  renderer always sets them; the reference set 15 px / 1.35 / `8px 12px` stands
  for what the client does not pass, so a client on defaults gets sane output.
  `fontSize` follows the client's chat text-size setting.
- **`bg_color` is this bubble's colour**: an outgoing bubble's when the user
  sent or forwarded the widget. The palette is the bubble's (`docs/sdk-v1.md`
  §2.3, §5.1).
- **Shipped in v0.1.1 as a clarification of profile 1**, not as profile 2.

Why a clarification. The profile's rule, a feature goes in when every client
draws it, governs what a widget may use and detect. The UA layer adds nothing
a widget can use or detect: no element, property, value or API; `profile` stays
1, `CSS.supports` and `@supports` answer as before. Profile 1 already had a
user-agent layer (the `<button>` template) and already promised UA defaults:
"h1–h6 with UA default sizes and margins", "blockquote, figure, figcaption (UA
indents)", "lists with markers". It did not say whose UA, and the engines'
answers differ. Saying it now defines what was unspecified; every widget valid
under profile 1 stays valid, and one that styles its own elements keeps its
look (any author rule wins). "UA default" in profile 1 means this layer
(decision `P1-ua` in `profile.json`).

**The padding token is the body's margin, not its padding.** The first draft
said `body { margin: 0; padding: var(--tg-text-padding) }`. 169 of the 208
corpus widgets (every widget of `showcase`, `common` and `htmlbubbles-ported`)
write `body { margin: 0 }` without a padding to go edge to edge, as authors do
in a browser; under the draft each of them gained an 8/12 px inset and 16 px of
height. With `body { margin: var(--tg-text-padding); padding: 0 }` none of the
208 changes, an unstyled widget looks the same, and the margin is inside the
reported height as the padding was (the root is a flow root on every
renderer). Artem's goal is that an unstyled widget looks native; the padding
was wording, not a decision (2026-10-20).

**The bubble is the canvas; the layer paints no background.** Painting
`html { background: var(--tg-theme-bg-color) }` would stop CSS's root
background propagation: a widget's `body { background: X }` with the default
margin would show a ring of the bubble's colour around the body, where a
browser paints X over the whole canvas. So the renderer stays transparent and
the client's bubble under it, drawn in `bg_color`, is the canvas, as before
v0.1.1. The engines propagate a root or body background to the canvas
themselves: WebKit and Chromium in the iframe and the WebViews, litehtml in its
root background (`html_tag::get_background`, painted over the whole raster).
The shadow-DOM renderer has no canvas, so the renderer core does it on the host
element (`RendererEnv.canvas`): after every batch that touches the root, the
body or a stylesheet, it copies the root's background, or the body's when the
root has none, onto the host. Colours match a frame exactly; a body gradient is
drawn once on the host and once on the body box, so its geometry can differ at
the body's edges. `visual/15-body-background` checks the result on every
renderer against one picture. Reference pictures get the bubble's colour as
the page's default background (Chromium, outside the document), so
propagation shows there too.

Two details make the transparent renderer hold on the web. The bubble itself
has no padding around the widget and clips it at its corner radius (the
widget's body margin is the inset): the dev app, the web and iOS chat feeds
and the native demos draw their bubbles that way, in exactly the `bg_color`
they pass, and `tests/sdk.spec.ts` screenshots the whole demo bubble. And the
web's frame: `IframeRenderer` puts the theme's `color-scheme` on the frame
element, and Chromium gives a frame whose root scheme differs from its
element's an opaque canvas, white in a dark chat. The renderer therefore sets
the same scheme on the frame's root, in the UA layer so a widget's own
`color-scheme` still wins (`RendererEnv.frameColorScheme`, the web frame only:
a dark root in a top-level WebView makes Chromium paint its own dark canvas);
the dev app's reference frame gets the same.

How the values were chosen.

- Every property that WebKit's, Chromium's or litehtml's own stylesheet sets
  differently is set explicitly, so the three agree (the conformance widget
  `visual/14-unstyled-document` measures 494 px at 320 px wide in Chromium and
  500 px through litehtml; the difference is litehtml's line boxes in table
  cells).
- Sizes are compact, for a 300 px bubble rather than a page: headings 1.6 to
  0.87 em at weight 600, 0.6 em between blocks, 1.5 em list indents, quotes
  and figures without the 40 px indent, a table with separator lines and its
  outer cells flush with the text, `pre` wrapping (the bubble does not scroll).
- `em`, never `rem`: on the shadow-DOM renderer `rem` resolves against the chat
  page's root, not the widget's.
- Physical properties first, logical ones after (`padding-left` then
  `padding-inline-start`): litehtml keeps the first, the WebViews take the
  second, so RTL widgets indent on the right where the engine can.
- `img` gets `max-width: 100%` but not `height: auto`: a layered rule outranks
  the presentational `height` attribute, so `height: auto` would replace the
  author's `<img height>` (measured in Chromium).
- Colours in the template have no fallback: a renderer that sets no variables
  keeps the engine's text colour. The text sizes and the margin fall back to
  the reference set.

Consequences for existing widgets: none of the 208 corpus widgets changes its
box layout (all of them set the body's margin); a widget that never mentions
the body's margin gets the client's inset instead of the engine's 8 px; text
that relied on the engine's defaults (Times, 16 px, 40 px list indents,
underlined links) now gets the layer's.

Where it lives.

- The WebView renderers (iframe, shadow DOM, WKWebView, Android WebView) insert
  `PROFILE_UA_STYLESHEET` through the shared renderer core and set the
  variables from the host's `theme` message (`themeVariables`). The shadow
  renderer's base stylesheet lost its unlayered `body { margin: 8px }`, which
  would have beaten the layer.
- Native renderers bake the template into their master CSS.
  `apps/renderer-litehtml` reads it from `profile.json` at build time (CMake;
  `scripts/build-litehtml-web.ts` for the wasm build), so there is no copy to
  keep in sync, and handles the `theme` message with `src/theme.cpp`, a twin of
  `themeVariables`, writing the variables inline and `!important` on `<html>`.
  Two litehtml behaviours needed care: it applies a `var()` inside a shorthand
  after the cascade (so `margin: var(…)` from the master CSS would beat a
  widget's `margin: 0`), and it sorts the master CSS by specificity (so its own
  `body { margin: 8px }` beat a later `:where(body)`). The renderer therefore
  puts the theme's values into the template and unwraps `:where()` before
  handing it over; litehtml applies the whole master CSS before any author CSS,
  so no author rule can lose to it. TwidgetCore and the Swift renderer (in the
  twidget repository) must do the same: append `tables.uaStylesheet` to their
  master CSS (with litehtml: values substituted, `:where()` unwrapped), set the
  variables of the `theme` message on the root with `themeVariables`'
  semantics (the fixed names, the grammars, the palette and text tokens always
  complete), refuse the widget's own style ops for those names on the root,
  paint no background of their own (a root or body background propagates to
  the whole raster), report the content height with the body's margins, and
  honour `user-select: text` (below). The Blitz prototype has neither the
  button template nor the variables yet.
- Reference pictures use one stylesheet, `packages/core/src/shared/reference.ts`:
  the layer plus the default palettes and the reference text tokens. The dev
  app's reference mode, the conformance build's native runs and the iOS/macOS
  and Android demos' reference modes (`Corpus/reference.css`, written by
  `scripts/sync-demo-corpus.ts`) apply it, so a corpus diff still shows what
  the profile removes.
- The conformance harness sets `body { padding: 0 }` next to its `margin: 0`,
  so the existing pictures did not move; `auto/22-ua-defaults` checks the
  computed defaults against the client's own tokens, and
  `visual/14-unstyled-document` is a page with no CSS at all, both "bare" (no
  harness CSS). `corpus/defaults` holds six example widgets with little or no
  CSS.

### Text selection: off by default, `user-select: text` turns it on (v0.1.1, 2026-10-20)

Replaces D18 ("text selection forbidden"), Artem's decision. Text in a widget
is not selectable by default anywhere; a widget makes text selectable with
`user-select: text` (or `-webkit-user-select: text`). Widgets that show data a
user wants to copy (an address, a code, a result) no longer need a workaround,
and a widget that is a drawing stays one.

- **The default is in the UA layer, without `!important`**:
  `-webkit-user-select: none; user-select: none; -webkit-touch-callout: none`
  on `:where(html)`. Before, a `*{… !important}` rule outside the layer beat
  any author CSS. Only the root: `user-select` is not inherited, and an
  element's `auto` follows its parent, so `user-select: text` on a container
  reaches its children (with `:where(*)` every child would have been reset to
  `none`; WebKit inherits `-webkit-user-select`, with the same result).
- **The profile** takes `user-select` and `-webkit-user-select` with `none`,
  `text` and `auto` (`css.text.selection` is in; `all`, `contain` and
  `::selection` stay out and are dropped). `behaviour.no-selection` now reads
  "not selectable by default". Editing (`contenteditable`) stays out. The
  widget cannot read or change the selection: `getSelection()` stays a stub.
- **The web renderers**: a mouse press no longer cancels its default action
  when it lands on selectable text, so a drag selects there; anywhere else it
  is still cancelled (no selection, no focus move, the chat input keeps the
  focus), and `selectstart` likewise. Copy is the browser's.
- **Touch**: a long press over ordinary text (`touch-action: auto`) is the
  bubble's (since v0.1.2 by the gesture rule below), so a selection inside a
  widget starts the way the client selects text in an ordinary message, and
  copy is the client's: client work
  (`docs/sdk-v1.md` §5.10).
- **An accepted exception to "every client draws it"**, like `:hover`: a native
  renderer that cannot select text yet (litehtml here; TwidgetCore and the
  Swift renderer until they implement it) keeps the text unselectable, and the
  widget still works. Conformance: `auto/06-text-fonts-c` checks the computed
  values, `auto/17-drag-hover` that a drag over ordinary text selects nothing,
  and `visual/16-selection` lets a tester select one paragraph and nothing
  else.

## Gestures: `touch-action` is the claim (v0.1.2, 2026-10-22)

Artem's decisions, after a client developer (Android) showed the sandbox and a
client disagreeing. Replaces "double tap and long press always stay with the
bubble" (SDK v1, decided 2026-10-17, `docs/sdk-v1.md` §6 item 5). Since v0.1.3
vertical drags are always the chat's ("Vertical drags are the chat's" below):
`pan-x` claims nothing and `none` no longer takes vertical drags.

**What went wrong.** In the web dev app a widget's "hold a button" worked
(`pointerdown` … `pointerup`, the game controls of a wolf3d port), and a
double tap made of two clicks worked too. On Android the held touch got
`pointercancel` unless the element had `touch-action: none`. The rule "long
press and double tap stay with the bubble" was enforced only by each client's
own recognizers (they cancel the touch), the demo apps had none, and the
renderer dropped `dblclick` and `contextmenu` for everyone. So a widget that
worked in the sandbox failed in a client, and AI agents that test widgets in
the sandbox were misled.

**The rule.** The framework decides who gets a press over the widget, with one
shared policy:

- Taps and **double taps are always the widget's**, on every element, whatever
  `touch-action` says: a double tap reaches it as both clicks and then
  `dblclick`. The bubble's double-tap reaction stays on the bubble outside the
  widget (caption, time, edges). A first version gave the second tap on
  `touch-action: auto` to the reaction; the compat suite showed what that costs
  (a counter's "+", a tab switch or a sort header tapped twice quickly lost the
  second tap: tabs, fragment, table-sort and stepper failed under the phone
  bubble), and the client developer's view was that nobody double-taps a widget
  to react. Decided the same day: the double tap is the widget's.
- The rest is claimed per element, at `pointerdown`, by standard CSS
  `touch-action` on the target and its ancestors (the renderer already reported
  the drag axes from it):
  - `auto` (the default): the bubble wins. A hold opens the message menu and
    the widget gets `pointercancel` when the hold is recognised; a horizontal
    swipe is reply or back; a vertical one scrolls the list.
  - `pan-y` / `pan-x`: the widget gets drags on the other axis; holds stay
    with the bubble.
  - `none`: the widget gets everything over that element: drags on both axes
    and holds (no menu; it gets `contextmenu`). The platform's own context menu
    stays cancelled.
- Never claimable: the bubble outside the widget stays the place for the menu
  and the reaction, and the leading 20 px of the widget (the left edge, the
  right one in a right-to-left UI) take no claim, so the system back gesture
  keeps working where the widget touches the screen edge.
- With a mouse (desktop): a right click is the message menu over anything but
  `none`, and `contextmenu` for the widget on `none`; clicks, double clicks,
  holds and drags are the widget's (desktop has no long-press menu and no
  swipes).

Why CSS `touch-action`: it is the web's own declaration of "this element
handles its touches", authors and models already write it for sliders and
canvases, the renderer reads it synchronously at `pointerdown` (a computed
style, no round trip to the sandbox), and every client's WebView already obeys
it for scrolling. A widget-side API (`TelegramWidget.claimGestures()`) would
have been a second, non-standard way to say the same thing, asked for after the
press had started. A learned `preventDefault` on `pointerdown` keeps claiming
the drag axes (as before), never holds: those need the declaration.

**The hold is per element.** Decided the same day: no whole-widget flag (no
`<meta>`, no SDK call), and nothing treats the root differently. `touch-action`
on an ancestor covers its descendants through the normal intersection, as in
browsers (the chain the renderer reads runs up to the widget's `<html>`).

**One shared policy.** `packages/core/src/shared/gesture-policy.ts` is pure: given
the claim and the press's movement and timing, a `GestureArbiter` says who wins
and when the widget's pointer must end with `pointercancel`. Twins:
`GesturePolicy.swift`, `GesturePolicy.kt`, `gesture_policy.cpp`, all checked
against `packages/core/tests/fixtures/gesture-policy.json` like the CPU budget
and the SDK checks; every host exposes it, so a client drives its recognizers
from it instead of inventing thresholds. The values (open for Artem; they are
constants in four copies and the fixture):

| constant | value | from |
|---|---|---|
| `TOUCH_SLOP_PX` | 10 | iOS `UILongPressGestureRecognizer.allowableMovement` 10 pt; Android `ViewConfiguration` touch slop 8 dp |
| `HOLD_MS` | 500 | iOS `minimumPressDuration` 0.5 s; Android's long-press timeout (500 ms in AOSP 9, 400 ms in later releases) |
| `DOUBLE_TAP_MS` | 300 | Android `DOUBLE_TAP_TIMEOUT` (first up to second down); decides when the widget gets `dblclick` |
| `DOUBLE_TAP_SLOP_PX` | 40 | tighter than Android's 100 dp: two adjacent buttons tapped quickly are two taps, not a `dblclick` |
| `EDGE_STRIP_PX` | 20 | iOS's edge-pan region (about 20 pt); Android's gesture-navigation back inset is 18–40 dp by setting |

The axis of a drag is the dominant one when the press leaves the slop (a tie
is vertical: the list scroll is the commoner gesture).

**Where it runs.**

- The renderer core always runs an arbiter on the primary pointer: it is what
  sends the widget `dblclick` (after the second tap's click, for every double
  tap) and `contextmenu` (at a hold or a right-button press the widget claims).
  The engine's own `dblclick` and `contextmenu` never reach the widget, so
  WebKit, Chromium and the native renderers deliver the same events at the same
  thresholds. `input.dblclick` is now in the profile, and `input.long-press` on
  `touch-action: none` elements (decision G1 in `profile.json`;
  `TelegramWidget.profile` stays 1: what changes is which presses the bubble
  keeps, not what a renderer draws).
- Native clients keep their own recognizers, driven by the policy and the
  claim the host reports (`onGestureClaim` / `onGesture`: `x`, `y`, `hold`), and
  cancel the touch to the WebView the platform's way (it becomes
  `pointercancel`); a double-tap recognizer must not act over the widget. The
  demo apps' bubbles do the same.
- The web has no recognizers over the widget, so there the renderer plays the
  bubble (`WidgetHost` `bubbleGestures`, the `arbitrate` message): a press the
  bubble takes ends for the widget with `pointercancel` and nothing more of that
  pointer, and the host gets `onBubbleGesture` (menu, swipe, scroll, back). The
  dev app and the feed demo use it by default with a mouse arbitrated as a
  finger (the `gestures` select: phone, desktop, off), and every Playwright
  suite runs on that default, so "works in the sandbox" now means "works in a
  client". In that mode the widget also sees `pointerType: 'touch'` for the
  press and no click after a drag or a hold, as on a phone.
- The Node harness (`sandbox-node.ts --width N --tap / --hold / --double-tap /
  --swipe / --right-click`) runs the same renderer core in headless Chromium on
  the sandbox's ops and reports per press the element, its claim, the policy's
  outcome, what the widget received and a hint for the author.

**Possible later step: an area quota.** A widget that puts `touch-action: none`
on its whole surface takes the bubble's menu and swipes away from the widget's
area entirely (they remain on the caption, the time and the bubble's edges).
A client developer suggested capping how much of the widget may claim holds,
like Android's limit on gesture-exclusion rects (200 dp of each edge). Not now:
the rule is simple and testable as it is, and the conformance and corpus
widgets claim small areas (buttons, knobs, canvases). If widgets start
claiming everything, the policy can take an area budget per widget and fall
back to `auto` beyond it.

## Vertical drags are the chat's (v0.1.3, 2026-10-24)

Artem's decision, after trying the widgets of `corpus/gestures` in the Telegram
test group: a widget can claim only horizontal drags. A vertical drag over a
widget always scrolls the chat, whatever `touch-action` says; the widget gets
`pointercancel`. Replaces the vertical half of G1 (decision G2 in
`profile.json`).

| `touch-action` | hold | horizontal drag | vertical drag |
|---|---|---|---|
| `auto`, `manipulation` | bubble: message menu | bubble: swipe / back | chat |
| `pan-y` | bubble | widget | chat |
| `pan-x` | bubble | bubble (it claimed vertical drags until v0.1.3) | chat |
| `none` | widget (`contextmenu`) | widget | chat (the widget's until v0.1.3) |

Unchanged: taps and double taps are the widget's on every element, the axis
lock (the first move past the 10 px slop decides for the whole press: a
horizontal-first drag stays the widget's on both axes until the finger lifts,
a vertical-first one is the chat's; a hold the widget won on `none` is locked
the same way at its first move past the slop), the 20 px leading strip, and the
mouse (a drag on any axis is the widget's: desktop scrolls with the wheel).
The hold's lock was first meant to keep the press for the widget whatever
moved next; the Android port showed that a WebView, whose engine now always
allows vertical panning, cancels such a press itself, so the policy says what
every engine does.

**Why.** The chat never gets stuck under a widget: a reader scrolling through
a chat cannot know which bubble claims vertical drags, and a widget that does
(a full-surface canvas, a game on `none`) stops the scroll exactly where the
finger is. With vertical always the chat's, scrolling works the same over
every bubble, and what a widget can take is one predictable thing: horizontal
drags (and holds on `none`), where the chat's own gesture, swipe-to-reply, is
rarer and already gives way per element.

**What is lost.** Everything whose input is vertical or two-dimensional:
two-axis joysticks and game pads, drawing and signature pads, map and image
panning, vertical sliders, faders and wheel pickers, swipe-up/down in games
(2048). Authors turn such controls horizontal, use taps (buttons for up and
down, a tap-to-place board), or both.

**How it was decided.** `corpus/gestures` (v0.1.3, `corpus/gestures/README.md`)
put the cost side by side before the change: a joystick game and 2048, each on
`touch-action: none` (both axes) and on `pan-y` (what the rule leaves them),
and a wheel picker on `pan-x`, against what is fine with horizontal only (a
paddle game, a card carousel, a range slider on `pan-y`). Artem tried them in
the Telegram test group (2026-10-23); the pairs now show what the rule
removed, and the `-xy` variants behave like their `-x` twins.

**Where it is enforced.** In the policy, four copies and the fixture:
`claimOf` never claims `y` (the field stays in the claim types, always false,
so code written for v0.1.2 builds), and `GestureArbiter` gives a vertical touch
drag to the chat even for a claim built by hand with `y`. In the renderer: the
engine under it (Chromium, WebKit) gets each `touch-action` value widened to
allow vertical panning (`none` → `pan-y`, `pan-x` → `pan-x pan-y`,
`src/renderer/touch-action.ts`), so a browser on a touch screen, the web
client on a phone, scrolls the page over any widget; the claim and the
widget's `getComputedStyle` read the widget's own value. On Android the
WebView renderer keeps its parent from intercepting only for a hold claim, and
only until the press leaves the slop vertically.

## The resize anchor (v0.1.3, 2026-10-24)

Artem's decision: a widget chooses which edge of its bubble stays put when its
height changes, with `TelegramWidget.setResizeAnchor('top' | 'bottom')`
(decision R1 in `profile.json`; `docs/sdk-v1.md` §2.12, §5.6).

**Why.** A chat list keeps the top of a growing bubble still, which is right
for an FAQ, "show more" or an order summary that opens below the line the
reader tapped, and wrong for a widget that grows at its bottom, a chat log or
a support thread: there the reader's finger is on the quick replies at the
bottom, and every new message pushes them away. Only the widget knows which
kind it is. `top` stays the default, so every widget written before keeps its
behaviour.

**The shape.** One call with two values, applied to the height changes that
follow until it is called again, rather than an argument of each change (the
renderer measures the height; the widget never reports it). It goes through
the bridge as a checked call like the others (`sdk-policy`: four copies and
the fixture; only `'top'` and `'bottom'`, case-sensitive; no gesture, no
visibility gate), but it is not an action: the host keeps the latest value and
reports it with the height (`WidgetDiagnostics.resizeAnchor`, web
`onResize(height, anchor)`), and the shim sends only changes, so a widget that
calls it in a loop costs the client nothing. It is an SDK v1 addition inside
profile 1: `TelegramWidget.profile` stays 1 and widgets detect the method with
`typeof`, as a client whose framework predates v0.1.3 lacks it.

**The client's part.** The list compensates its scroll so the anchored edge of
a bubble on screen stays in place, also when the bubble opens at its cached
height and the first report differs and after it is re-created; a bubble off
screen keeps the visible content in place; during a height animation the old
pixels are aligned to the anchored edge. The web dev app and the feed do it
(`corpus/resize` has one widget per anchor).

## The bot's data and requests: widget interactivity (2026-10-25)

Artem's decisions after the product discussion of October 2026 and the server's
spec of 2026-10-25 (decision I1 in `profile.json`; `docs/sdk-v1.md` §2.13–§2.15
and §5.11; the server's side, `docs/tgw-format.md` "The widget's state and
requests"): **a widget is fixed code plus changeable state: the HTML never
changes after sending; the state changes, and only the bot writes it.** The
chat state, `chat_data`, is tied to one original message and seen only in its
chat (the user's in the private chat, shared by the members of a group) and is
never forwarded; the shared state, `public_data`, is seen wherever the widget is
shown (a forward gets a frozen copy, or the state of a server object shared by
the original and all its forwards). The widget reads its state and cannot
change it itself; it can only ask the bot. A request from the widget is a
callback button inside it: the bot gets what an inline button's press gives
(`from`, the message with its state, `chat_instance`, the chat type; from a
forward only `from` and the shared object's id) plus `data` of at most 64 bytes,
and answers with a `result` of at most 1 KB for the widget that asked. The bot
changes the state only with separate calls, never in an answer. Whether a
widget may ask at all is the bot's choice per widget (`allow_queries`, and
`fwd_allow_queries` for its forwards); where it may ask is the server's flags
per message: `can_query` (originals of a widget sent with `allow_queries`;
forwards of one sent with `fwd_allow_queries`), `background_query` (only the
original in the private chat with the bot). The second version of the server's
spec (same day) replaced `fwd_live` with these two flags, showed the shared
object's id to the bot and took the place out of a forward's request; the
framework reads the flags, so only the docs changed. The final spec (2026-10-26)
made the shared object the widget itself (the HTML, the size, `public_data`, its
refresh period and its flags, named by `widget_id`, sent by reference with
`inputMediaWidgetId`), let `fwd_allow_queries` imply `allow_queries`, gave
the bot three updates (an original, a via-bot message, a copy with only
`from`, `data` and `widget_id`), set the request limit to 5 per 5 s and made
the poll one message per call again; the framework's side did not change
(`docs/tgw-format.md` has the server's side).

**Why this model.** Code that the bot can swap after sending would let one
edit change a thousand forwarded copies and make every widget a moving target
for moderation; fixed code with changing state keeps what a reader saw
reviewable and lets the widget animate into its new state instead of
restarting. A request shaped as a callback query needs no signature (it
reaches the bot as an update from Telegram) and no bot server at all with
Telegram's serverless handlers. The widget knows locally only what the bot
gets with a press anyway (in an original that may ask: the viewer's id and the
chat type; in a live forward: the viewer's id; where it cannot ask: no viewer),
so a request has nothing extra to leak. A copy is told nothing about the chat it is
in (Artem: the bot must learn nothing about where its widget was forwarded, and
whatever the widget knows can go to the bot in a request's 64 bytes).

**Part of SDK v1 and layer 233 from the launch** (Artem: everything ships with
the widgets' launch; no client ever has widgets without state and requests). No
version of its own, no detection: every client has it. A client that passes no
flags gets widgets whose requests are refused (`NOT_ALLOWED`); the profile
stays 1.

**The shapes we chose.**
- Names follow Mini Apps where one exists (`chatType` from `chat_type`,
  `viewer` for `initDataUnsafe.user`); `request` returns a promise of
  `{ result }` and rejects with `error.code` (`NOT_ALLOWED`, `BUSY`,
  `RATE_LIMITED`, `TIMEOUT`, `FAILED`, `INVALID_ANSWER`), so a widget always
  learns how its request ended.
- **The client reads the server's flags, it derives nothing.** The host's
  context is `isCopy`, `canQuery`, `backgroundQuery` as the media has them, plus
  the chat type and the viewer; no flags, no requests. The request gate decides from them: no `canQuery` →
  refused; a tap → out (and the tap is used); no `backgroundQuery` → refused;
  hidden → refused; a background request: one when the widget appears on screen,
  then at most one per 5 s while it stays visible (Artem; one constant,
  `BACKGROUND_REQUEST_INTERVAL_MS`, and the gate's `show`). The server and the bot
  cannot tell a tap from a background request, as for callback buttons, so the
  request carries only `data` and the limit is the client's. One request in
  flight per widget, whatever its kind.
- **`viewer` is a `WebAppUser`** (Artem, 2026-10-26): `{ id, first_name,
  last_name?, username?, is_premium? }`, as Mini Apps' `initDataUnsafe.user`,
  since the SDK already follows Mini Apps' names (`themeParams`,
  `HapticFeedback`). The client passes the account's own names, username and
  Premium flag; the glue checks them like the id (names 1–64 UTF-16 code units,
  a username 5–32 of `[A-Za-z0-9_]`, `is_premium` a boolean, kept only when
  true) and leaves out a field that breaks its rule (`data-rejected`), not the
  whole viewer. On a tap the bot gets `from` with the same fields, so nothing
  new leaks; a fake Telegram dialog that greets the reader by name was the
  worry before, and the rule below answers it: only a widget that may ask
  knows. **The viewer only with `canQuery`** (Artem, after the
  server's spec v2): an original or a forward that may ask gets the viewer, whom
  the bot learns with its first tap anyway; an original sent without
  `allow_queries`, a copy that may not ask and a preview get none. A widget that
  may not talk to the bot does not know who views it, so it cannot pass the id
  out through `openLink` either, and the rule matches the network marker; in the
  private chat the bot can put the user's id into the chat state itself. No copy
  gets the chat type, with or without `canQuery`.
- **Polling only for widgets that ask: `watchPublicData()`** (Artem, 2026-10-26;
  the second proposal to the server, `notes/server-proposal-widget-object.md`).
  A bot may make a widget shared only to send it by `widget_id`, and polling
  every such card on screen would ask the server for nothing. So the widget asks
  to follow its shared state live with `TelegramWidget.watchPublicData()` (and
  stops with `unwatchPublicData()`): synchronous, no tap, idempotent; the shim
  sends only changes, the host keeps the latest value and reports a change, like
  the resize anchor; a preview never watches. The client polls
  `getWidgetPublicData` only for watching widgets on screen with a `widget_id`,
  one carrier per id; every other widget gets its shared state with the
  message. The protocol does not change. The same proposal moved `period` to 5–3600 s with a default of 30 s
  (`cache_time` 0 sets it back to 30 s; the final spec had 2–3600 s and 5 s);
  the server made it so (Artem: "на сервере сделал 5, 30, 3600").
- **The network marker** (the server's spec, a UI contract): where the message
  has `can_query`, the client marks the widget as one whose tap may send data to
  the bot's server; without it there is no marker, and the server refuses every
  request from that message. The marker is the client's own UI from the flag it
  already passes (`context.canQuery`), so the framework adds no API for it; the
  dev app draws one in its bubble's header.
- **The answer is only `result`** (no toast, alert, URL, `cache_time` or state):
  the widget shows the result itself and the client shows nothing for a
  request. Changes of the state reach the widget the usual ways (the server's
  update for the chat state, polling for a shared widget's shared state), and
  the client does not re-fetch after an answer: a bot whose widget needs the new
  state at once returns it in `result`. This keeps one path for the state and
  the edits under their own rate limits.
- **Two events**, `chatDataChanged` (the chat state) and `publicDataChanged`
  (the shared state), not one: a widget redraws what changed, and in a forward
  only `publicDataChanged` can fire. One `setData` that changes both fires
  both, chat first, in one task; a value that did not change fires nothing (so a
  duplicate delivery is harmless). `setData` always takes the full state: the
  client knows its message's values.
- The chat state, the shared state and `result` are the server's text fields,
  each a text of up to 1024 bytes (UTF-8) without a NUL; the glue refuses the
  same (`data-rejected`, `INVALID_ANSWER`), so a client bug cannot hand the
  widget what the server would refuse.
- One tap unlocks one privileged call, shared by `openLink`,
  `openTelegramLink`, `copyText` and a request: the rule a reader can rely on
  ("one tap, one thing"). A request looks at the tap without consuming it and
  consumes it only when it goes out as the tap's.
- A client without `onRequest` refuses at delivery, after the gate, on every
  host (`NOT_ALLOWED`, reason `unsupported`): the native hosts take `onRequest`
  as a property set after creation, and a widget that asks at boot must not
  lose that race.
- `copyText` is a separate callback, not a `WidgetAction` case, so clients'
  exhaustive switches keep compiling; the client's toast shows the copied text
  so a widget cannot show one address and copy another unnoticed.

**Where it lives.** The checks are one policy with four copies and the
fixture (`sdk-policy`: `checkRequestData`, `checkCopyText`,
`checkWidgetData`, `checkViewer`, `checkChatType`, `checkRequestAnswer`,
`RequestGate`, `requestRefusal`; `tests/fixtures/sdk-policy.json`). The shim
checks the same limits to give the widget a `TypeError` or an immediate `BUSY`
or `NOT_ALLOWED`; every glue checks again (a widget can rewrite what the shim
sends: `JSON.stringify` is a global, `corpus/attacks/request-forged-wire.html`).
Answers and changes of the state go into the sandbox as a host → sandbox
`bridge` dispatch, the task kind the CPU budget already defers (it carried the
profile-0 bot answers), so no policy change was needed there. The request and
the polling go over the client's own connection: the sandbox still has no
network (invariant 5).

**Downsides we accept.** Through a request the bot learns whatever the widget's
code knows, up to 64 bytes (language, theme, text size, bubble width, time
zone, what it kept locally); in the private chat background requests also
show the bot when the user has the chat open. A tap "for the game" and a tap
"for the bot" look the same to the reader, so the bot is always visible as
the sender or via, and a widget is never forwarded without its author.
The chat state in a group is visible to every member. A bug in a sent widget
cannot be fixed, only replaced by a new message. Forwards of widgets not sent
live cannot vote or play: that goes through the bot.

**Previews** (Artem, 2026-10-25: "give all the data, but the client must know
the widget is in a preview and give it no interactivity and no data about the
user"). A widget in an inline-results list, before it is sent, runs with its
state and `TelegramWidget.isPreview`; the host (`context.preview`) takes no
input from it (a tap selects the result), lets through only `ready` and
`setResizeAnchor` (`PREVIEW_METHODS` in the policy: no request, link, copy or
haptic), gives it no chat type or viewer and the request flags off, and never
commits its Web Storage (there is no message yet). Timers, animations, the theme
and the limits are as for any bubble, so the preview is the first state the user
is about to send.

## Haptics need a touch (v0.2.2)

**The bug.** Telegram Android 13.2.0 (7255): a widget with
`setInterval(function(){TelegramWidget.HapticFeedback.notificationOccurred("error")},0)` vibrated the phone for as
long as it was on screen. The gate allowed haptics with no gesture, under a rate limit only (a burst of 5, 10 per
second), and 10 a second is a continuous buzz.

**The rule** (Artem, 2026-10-26; `HapticGate` in `sdk-policy` and its three twins, the numbers named constants he may
tune): while a finger or a mouse button is down on the widget and 5 s after it is lifted, haptics pass under the rate
limit; outside a touch the first call opens a 1 s series, and the next series needs a touch (one series at any time
after it) or 10 s since the last one began. Hidden widgets and previews get nothing, as before. The touch is trusted
input the renderer reports (its claim at `pointerdown`, and since v0.2.2 `release` at the end of the press), never an
event the widget dispatches; a press whose end the host never sees (the finger left the view) counts as lifted 30 s
after it began.

**Why not a gesture-only rule** (haptics only within a tap's task, like links): widgets play haptics where no tap is
at hand. A bot's answer arrives 1.5–15 s after the tap that asked; an animation ends a second later; an arcade game
buzzes on a collision while the finger is still down or just lifted. The touch window covers the game and the
animation, the one series after a touch covers the late answer, and the 1 s series every 10 s keeps a widget with no
touch at all (a timer, a ticker) from becoming a vibrator.


## Native scrolling: an experiment (v0.2.3, 2026-10-27)

`WidgetHost.Options.nativeScrolling` (Swift, from the Telegram macOS work, a5a7023) lets the validator keep `overflow:
auto` / `scroll` and lets a widget listen to `scroll`. Widget profile 1 clips instead (no inner scrolling, `docs/profile-v1.md`),
and every other client does. Earlier Swift-only options (`canvasWords`, `binaryOps`, `morphUpdates`) change how the same
validated output travels; this one changes what a widget can do, so a widget written against a client that turns it on
breaks on every other client. It stays as an experiment, the way the 3D side channel does: only DEBUG builds or builds
with `TWIDGET_EXPERIMENTAL_SCROLLING` honour it, and a release build reports a `native-scrolling` diag and keeps profile 1.
The shared validator code is unchanged and inert unless a host turns the flag on (Kotlin, C++ and the web never do).
Inner scrolling for every client is a question for widget profile 2: with a wheel or a trackpad it is safe; on a touch
screen horizontal scrolling fits the gesture rule, vertical scrolling conflicts with "vertical drags are the chat's" (v0.1.3).
