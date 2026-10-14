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

### Shadow DOM renderer (prototype): the renderer core without the frame

The web product path is still the sandboxed `srcdoc` iframe, but a feed of
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

Strict validators allow at most 64 Ki UTF-16 units per text value, 256 Ki units across tracked text and comment nodes, and 8192 complex-script units across text nodes. Removal and shrinking release the document budget; resetting a pooled native validator clears it.

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

### 256 timers, and an error instead of a hang

- Telegram allows 16 timers; browsers have no limit. D3 transitions and
  staggered animations put dozens of timers in flight, so 256 is the default,
  configurable (`maxTimers`).
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
  The footprint of a worker is therefore bounded by 80 MB; a polling or
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
  physical press (`pointerdown` … `click`/`dblclick`) or one key press (not
  Escape, not a repeat, not a Command shortcut) on the visible widget unlocks
  one call. Timers, frames, promise jobs after the task, scripted
  `click()`/`dispatchEvent` and hidden widgets get an `activation-blocked`
  diag instead. `ready` stays free.
- Native glue (Swift, Kotlin, C++): the host numbers gestures from renderer
  events and hands the number to the engine with the event; the engine opens
  the gesture for that event's task only and consumes it with the first
  privileged call. The renderer is trusted code and the only source of events.
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
  `WidgetHost::gestureFor` + `SandboxEngine` in C++, each with its tests
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
- **`Intl` is an interface** (`IntlProvider`); `BasicIntl` is an English-style
  fallback for the harness and tests. A product host plugs `QLocale`/ICU in —
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

A returned WKWebView loads a new trusted renderer document at a fresh random
origin, with a fresh CSP nonce. The previous document is unregistered, callbacks
and buffered messages are cleared, and messages from the previous frame URL are
rejected. Only a new `ready` message admits the view to the idle pool. Failed or
timed-out resets destroy the view. This preserves per-widget origins rather than
resetting untrusted content inside a shared document.

The pool holds at most twice its warm target, counting resets in flight. Draining
also cancels pending refills and resets. Memory pressure drains the pool and
suppresses refills/recycling for 30 seconds. Async bubble snapshots are accepted
only for the host generation that requested them.
