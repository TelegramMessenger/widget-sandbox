# Changes since 862eec8 (telegram-macos)

What a host that upgrades from 862eec8 sees, per platform. "Opt-in" items change
nothing until the host lists them; "default" items apply to every host.

## v0.1.1 — release notes (2026-10-20)

A clarification of widget profile 1, opt-in text selection, and the tooling around the
test group; profile, SDK and protocol versions stay 1, and every widget valid under v0.1.0
stays valid.

**Default widget styles** (`docs/sdk-v1.md` §2.4, `docs/decisions.md`, "The renderer's
defaults"): a widget without CSS now looks native on every client.

- The renderer's user-agent layer (`tables.uaStylesheet` in `packages/core/profile.json`,
  `@layer tw-ua`, every selector in `:where()`, nothing `!important`, so any author rule
  wins) covers the root and the text elements, not only `<button>`: `html` in
  `--tg-theme-text-color` and the client's text font; `body` with the client's padding token
  as its margin (inside the reported height), so `body { margin: 0 }` still goes edge to
  edge; headings, paragraphs, lists, `dl`, tables, `hr`, `pre`/`code`, quotes, figures,
  images and inline elements get explicit sizes and margins in `em`, the same in WebKit,
  Chromium and litehtml. Before, each engine's own stylesheet showed (Times 16 px and an
  8 px margin in a WebView).
- The renderer paints no background: the client's bubble under the transparent renderer,
  drawn in `bg_color`, is the canvas, and a root or body background a widget sets covers the
  whole bubble as in a browser. The shadow-DOM renderer, which has no canvas, paints that
  background on its host element.
- **Text tokens** next to the button tokens, on every host: `WidgetTheme.text` with
  `fontSize` (the chat's text-size setting, 8–64 CSS px), `lineHeight` (1–3) and
  `padding` (one to four lengths) → `--tg-text-font-size`, `--tg-text-line-height`,
  `--tg-text-padding`, always set on the renderer root; the reference set 15 px / 1.35 /
  `8px 12px` stands for what a client does not pass. Swift `WidgetTextTokens`, Kotlin
  `WidgetTextTokens`, C++ `twidget::WidgetTextTokens`, TS `TextTokens`; the Node harness
  takes `text` / `button` and writes the `theme` message into `--ops-file`.
- **`bg_color` is this bubble's colour**: a widget the user sent or forwarded sits in an
  outgoing bubble and gets that bubble's palette (`docs/sdk-v1.md` §2.3, §5.1).
- Visible changes for existing widgets: none of the 208 corpus widgets changes its box
  layout; text that relied on the engine's defaults gets the layer's (the font, heading
  sizes, list indents, links without an underline).
- Native renderers bake the template into their master CSS: `apps/renderer-litehtml` reads
  it from `profile.json` at build time and sets the theme variables itself
  (`src/theme.cpp`); what TwidgetCore and the Swift renderer must do is in
  `docs/decisions.md`. Reference pictures everywhere (dev app, conformance build, the
  native demos' reference modes) use one stylesheet, `packages/core/src/shared/reference.ts`.

**Text selection** (D18 replaced, `docs/decisions.md`, "Text selection"): text is not
selectable by default; a widget enables it with `user-select: text` (or
`-webkit-user-select`), children included. The profile takes `user-select: none | text |
auto`; `all`, `contain` and `::selection` stay out. The rule moved into the layer without
`!important`; the web renderers let a mouse drag select only over selectable text, and a
press anywhere else still selects nothing and keeps the chat input's focus.
**New client work** (`docs/sdk-v1.md` §5.10): on touch, long press and double tap stay
with the bubble, so selection inside a widget starts the way the client selects message
text, and copy is the client's copy. A native renderer that cannot select yet keeps the
text unselectable (an accepted exception to "every client draws it", like `:hover`).

**Examples and conformance**

- `corpus/defaults`: six widgets with little or no CSS (text card, list, table, FAQ with
  `<button>`, survey, link card), `pnpm test:defaults`.
- `packages/conformance`: `visual/14-unstyled-document` (no CSS at all, light and dark
  pictures on the reference tokens), `visual/15-body-background` (a body background without
  a margin reset fills the bubble), `visual/16-selection` (only the opted-in paragraph can be
  selected) and `auto/22-ua-defaults` (the computed defaults against the client's own tokens;
  author CSS wins); `auto/06-text-fonts-c` checks the selection values. "bare" widgets carry
  the harness script without its CSS. The harness sets `body { padding: 0 }`, so the other
  pictures did not move. Reference pictures get the bubble's colour as the page's canvas.

**The test group**: `scripts/conformance/publish-channel.ts` fills the Telegram test forum
group through a test bot (topics Visual, Client SDK, Auto and, new, Defaults; Showcase,
Common, Generated on request), with size anchors in the file name, `--dry-run`, `--only`,
`--rebuild`, `--reset`; config and state outside the repository. Visual 03, 05, 08 and
09 are split into `-a`/`-b` so every widget fits the 600 px bubble; `build.ts` gates 560 px
at 320 px wide and `heights.ts` measures every widget through the sandbox.

## v0.1.0 — release notes (2026-10-17)

The first tagged state of the framework: the integration branch `telegram-macos`
with the pre-release hardening, widget profile 1 and SDK v1, verified on every platform (the per-suite numbers are in the tag's commit history
and CLAUDE.md).

**In the tag**

- The sandbox on five hosts — Swift (iOS + macOS), Kotlin, C++ core, web (Worker
  + Shadow DOM renderer) and the Node harness — on one QuickJS: the
  TelegramMessenger/quickjs fork at c24d6b0 (uncatchable interrupt and OOM in every
  engine path), as a submodule, with precompiled bytecode next to each bundle.
- `strict` policy by default everywhere; user activation from a pointer press only;
  runtime limits (CPU quota, timer cap, memory cap, off-screen pause, frozen last frame, and
  since 2026-10-17 a layout-query budget per task: queries that block the host for more than
  max(slice, 100 ms) in one task kill the widget with "Layout query limit exceeded" and count
  toward the CPU quota; `docs/decisions.md`, "Runtime limits").
- Opt-in features: `webAnimations` on every host; `canvasWords`, `binaryOps` with the
  Swift `NativeValidator`, `morphUpdates` Swift-only.
- Renderers: WKWebView / WebView / Shadow DOM as products; Blitz and litehtml as
  prototypes behind the same seam.
- The widget profile conformance set, `packages/conformance` (`CLIENTS.md`): every widget
  fits a mini-widget (under 3800 bytes of gzip), so the test bot sends the whole set as
  layer-233 messages, each with its reference picture next to it (`channel.json`). And the
  corpora: 99 attacks, compat, generated, common, showcase, 116 ported HTML bubbles.
- What a widget is (decided 2026-10-17): **mini-HTML**, one HTML document of at most 4 KB
  after gzip and 32 KB uncompressed, delivered right in the message as its own media.
  **The server API ships with this release: layer 233**, `inputMediaWidget width height html`
  / `messageMediaWidget width height payload` (`docs/tgw-format.md`): the server checks UTF-8,
  no NUL, 32 KB, gzips, checks 4 KB, puts the bytes in the message; bots only, private chats
  only, forwarding allowed anywhere; the caption is the preview text; clients below layer 233
  get `messageMediaUnsupported`; no server flag, clients enable rendering themselves. `.tgw`
  (gzip of the HTML as a file) is only an optional development format behind a client's
  debug flag.
- **Widget profile 1** (frozen 2026-10-17), enforced on every client:
  `packages/core/profile.json` is the single source and `docs/profile-v1.md` the
  generated contract. The validator drops form controls, named fonts and every
  property, function, unit, selector, at-rule and media feature outside the profile,
  rewrites sticky and scrolling overflow, drops nested rules and decides `@supports`;
  `TelegramWidget.profile`, `isProfileAtLeast(n)` and `CSS.supports` answer from the
  same tables; the renderer forwards no keyboard, focus, `dblclick`, context-menu,
  touch, wheel or scroll event, keeps the focus out of the widget, disables text
  selection and draws an unstyled `<button>` in the client's style (`--tg-button-*`).
- No bundled libraries: Chart.js and D3, `config.libraries` and
  `SandboxResources.provide` are gone on every platform (`docs/decisions.md`).
- `Intl.formatToParts` (numbers and dates) in every glue copy.
- **SDK v1** (`docs/sdk-v1.md`, 2026-10-17) in the shim, all five glue copies and the
  Node harness: `TelegramWidget.colorScheme`, `themeParams` (and the `--tg-theme-*`,
  `--tg-color-scheme`, `--tg-button-*` variables on the renderer root), `isActive`,
  `onEvent` / `offEvent` (`themeChanged`, `activated`, `deactivated`), `openLink` /
  `openTelegramLink` with the URL rules and one call per tap, `HapticFeedback` with a
  rate limit, Web Storage kept per message by the client (16 KB per area, preload at
  start, a commit per task), `navigator.language` and the Intl default from the
  client's locale, gesture claims per press (`x`, `y`). `sendMessage` is removed;
  `alert`/`confirm`/`prompt`, clipboard and share no longer reach the client. The
  checks are one policy in four copies with a shared fixture
  (`packages/core/src/shared/sdk-policy.ts`, `tests/fixtures/sdk-policy.json`).
  The corpus widgets that called `sendMessage` keep their state in `localStorage`.
- Docs for client teams: `docs/client-checklist.md` (start here), `docs/integration.md`,
  `docs/tgw-format.md`, `docs/sdk-v1.md` (the SDK and storage contract), `docs/profile-v1.md`,
  `docs/decisions.md` ("Profile v1").

**Not in the tag yet** (the next milestones)

- The client's answer to `prefers-reduced-motion` arrives with its config (today: no
  preference); per-bot `DeviceStorage` and everything else outside SDK v1 (§6 there).
- `VERSION.json`, CI, a reproducible build of `packages/quickjs-wasm/dist`, published
  Maven and npm artifacts.
- Renderer and engine pools on Android and the desktop core (the validator pool is on
  Android).

**API changes for clients**

- Bubble height bounds: the hosts clamp the reported height to **[50, 600]** by default
  (was [24, 900]) — web `MIN_HEIGHT` / `MAX_HEIGHT`, Swift `minHeight` 50 and
  `WidgetHost.Options.maximumHeight` 600, Kotlin `Options.minHeightDp` 50 / `maxHeightDp` 600,
  C++ `WidgetHost` [50, 600] — the shared v0.1.0 sizing constants, with a minimum widget
  width of 200 (`docs/integration.md`, "Bubble sizing": width 0 = the bubble's width, W = a
  fixed width; height 0 = auto under 600, H = a fixed height). A client that needs other
  bounds passes its own cap. The dev app's and the demos' reference mode clamp the same way.
- Removed: `SandboxResources.provide`, `ProvidedLibrary`, `BundledLibrary`,
  `SandboxResources.libraries` / `library(for:)` / `librarySource` / `libraryBytecode` (Swift);
  `SandboxResources.libraries` / `library()` / `librarySource()` / `libraryBytecode()` (Kotlin);
  `SandboxResources::libraries` / `library()` / `librarySource()` / `libraryBytecode()` (C++);
  the web `WidgetHost` option `libraries` and `@telegram/widget-sandbox/host/loader`.
  `WidgetJS/libs/` and `libraries.json` are no longer shipped.
- Diagnostics: a removal the profile causes is reported as `profile`; `library` is gone.
- Kill reasons: next to `CPU limit exceeded` and `Memory limit exceeded` a host can now see
  `Layout query limit exceeded: host queries blocked the renderer for more than N ms in one <kind>
  task (…)` (`onDead` / `died`), and a widget's later queries in that task get
  `{"error":"query budget exceeded"}`. The CPU budget policy gained `queryBudgetMs(sliceMs)`
  (`QUERY_BUDGET_MIN_MS` 100) in all four copies.
- SDK v1, every platform: the host takes a theme (`WidgetTheme`: colour scheme, palette,
  button tokens; the plain scheme is still accepted), a `locale` and a Web Storage
  preload (`storage`), and reports `onAction` (ready, openLink, openTelegramLink,
  haptic), `onStorageCommit(area, values)` and `onGestureClaim(x, y)`; `setTheme`
  takes a `WidgetTheme`. `onBridge` is deprecated (it still gets the raw calls that
  passed the checks). Removed: the host → widget bridge events (web
  `WidgetHost.bridgeEvent`, the `bridge` worker message, `TelegramWidget.on`) and the
  wire methods `sendMessage`, `alert` and `clipboard`. New diagnostics:
  `bridge-rejected`, `haptic-limit`, `storage-rejected`, `profile-mismatch`. The
  renderer's `gesture` message carries `x` and `y` next to `wants`.
  Per platform: Swift `WidgetTheme`, `WidgetButtonTokens`, `WidgetStorage`,
  `WidgetStorageArea`, `WidgetHaptic`, `WidgetAction`, `WidgetGestureClaim`, `SdkPolicy`;
  source-breaking: `WidgetBubble.theme`, `WidgetPrepared.theme` and
  `WidgetPreparer.prepare(theme:)` are `WidgetTheme`; removed
  `SandboxEngine.unprivilegedBridgeMethods`. Kotlin `WidgetTheme`, `WidgetButtonTokens`,
  `WidgetStorage`, `WidgetAction`, `SdkPolicy`, `SandboxEngine.Delegate.onAction` /
  `onStorageCommit` (default bodies); removed `UserActivation.UNPRIVILEGED_BRIDGE_METHODS`.
  C++ `twidget::sdk` (`sdk_policy.h`), `WidgetTheme`, `WidgetButtonTokens`, `WidgetStorage`,
  `WidgetAction`; `SandboxEngineDelegate::onBridgeAction` / `onStorage` (empty defaults;
  `onBridge` is no longer pure); `Json::parse(text, Json::Surrogates::Keep)`; new members
  change the layout of `SandboxEngine` and `WidgetHost` (recompile). TS: the web host's
  `UNPRIVILEGED_BRIDGE_METHODS` export is gone (`PRIVILEGED_METHODS` in `sdk-policy.ts`).

**Known issues**

- *Resolved by profile 1* (2026-10-17): `focus()` and key events no longer reach the
  renderer or the widget, text selection is off, form controls are dropped, and the
  Shadow DOM renderer's gaps are out of the profile on every renderer (popovers,
  `showModal()`, `@property`, `:scope`); CSS nesting is dropped with a diagnostic.
- *Native renderers*: the Swift renderer and TwidgetCore apply the same validated ops
  but must also forward no key events, disable selection and bake the `<button>`
  template into their master CSS (`tables.uaStylesheet`); grid is in profile 1 on the
  condition that both draw it.
- *Shim gaps found by the conformance set*: `getComputedStyle` answers `""` for
  shorthands (`white-space`, `overflow`, `margin`); `animation*` / `transitionend` events carry no `animationName`,
  `elapsedTime`, `propertyName`; `getElementsByClassName` is not live; canvas
  `drawImage(off-DOM canvas)` draws nothing.
- *Android instrumented suite on the emulator*: `GestureTest.rangeSliderAndInnerScrollerKeepTheGesture`
  fails on AVD twidget-35 (API 35, software GLES): the first WebView in the test
  process takes ~25 s to initialise against the test's 15 s load deadline, and under
  host load the renderer's gesture decision misses `GestureWebView`'s 150 ms timeout, so
  the list scrolls. Three other tests time out when the host is loaded; all pass
  alone. Not a framework regression; rerun on a hardware-GPU emulator or a phone.
- *Timing-sensitive tests*: `binary-ops.test.ts` "same render ops as JSON: clock.html"
  (the widget prints `new Date()`), `shim.test.ts` "defers timers once the widget has
  burnt its quota", Swift `CanvasWordsTests.testWordPayloadsSpliceToTheSameJSONAsTheJSONPath`
  (a 1 ms timer races the hand-driven frames), Swift `CpuTimeLimitTests.testBusyMachineDoesNotKillAWidgetWithinItsCPUBudget`
  (`loaded` and the engine stats reach the diagnostics on two async paths, so the
  "alone" run can read `jsMs` 0.0 and set the contention run's limit to the 100 ms
  floor), desktop `cpu_quota_throttles_greedy_timer`: each fails now and then under
  load and passes alone.
- *htmlbubbles-ported*: 107–110 of 116 pass (2026-10-17, after the profile-1 rework); the rest
  are time-based or random drawings, the hang demo, one intentional JS error and
  `06-trip-lisbon/09-me-bare`, a 690 px widget that fails steadily since the 600 px bubble
  cap (its click pass scrolls the clipped document; before the cap it failed now and then)
  (`corpus/htmlbubbles-ported/README.md`).
- *Locale in the Intl fallbacks*: the Kotlin `IntlBridge` has no `numberOptions` /
  `dateOptions`, so `resolvedOptions().locale` does not show the client locale on Android
  (formatting and `navigator.language` do follow it); the C++ `BasicIntl` ignores the locale
  (a product host plugs in ICU or QLocale).
- *3D channel* (`__tw3d`): debug builds only.

## Policy: `strict` (default) and `compat`

Hosts choose a policy: `SandboxConfig.policy` / `WidgetHost.Options.policy` (Swift, Kotlin), `SandboxConfig.policy` / `WidgetHostOptions.policy` (C++), `config.policy` / `WidgetHost` option `policy` (TS). **`strict` is the default on every host** (it used to be `compat`); `compat` keeps the limits hosts had before the hardening and is meant for tests and tooling. Kotlin and C++ take a string: anything but exactly `"compat"` is strict.

| | strict (default) | compat |
|---|---|---|
| Privileged bridge calls | need a real user gesture (one pointer press on the visible widget unlocks one call, keys never do; `ready` is exempt; only renderer events marked `trusted` count), else `activation-blocked` | forwarded as before |
| Text nodes, string properties, Path2D data | 64 KB | 2 MB |
| Canvas string arguments | 64 KB | 4 MB |
| Visible text: text nodes, `value`/`placeholder`/`alt`/`label`/`title`, `data-*`, `aria-*`, strings in `style` attributes, `style` ops and stylesheets | 256 Ki units per document, 8192 complex-script units (`text-budget`), churn bucket (`text-churn`) | not budgeted |
| `attr()` in CSS | only the budgeted attributes above | any attribute |

The validator learns the policy through `__twValidator.setPolicy('strict')` (glue `__v_setPolicy`), sent only for `strict`, so glue built against an older validator bundle keeps working. The native node model reads the limits and the budgeted attribute names from `__twValidator.tables()`, which reflect the policy.

## Visible by default

| Change | Platforms | Opt-out | Notes |
|---|---|---|---|
| `performance.now()`, the host clock and frame times tick in 100 microsecond steps | all | none | Timing side channel hardening. |
| The CPU limit and quota count the JS thread's own CPU time, with a wall-clock backstop of max(10x slice, 5 s) | iOS, macOS, Android, desktop | none | Busy machines kill fewer widgets; `stats.jsMs` changes meaning. Web and the Node harness keep wall clock. |
| Out-of-memory is uncatchable on the web (fork wasm), as it already was on native | web, Node | none | Widgets that caught `InternalError: out of memory` now die. |
| The host functions and `__twDispatch` are deleted from the global object after boot, and a sandbox bundle that fails to load (or cannot hide them) kills the widget before any widget code runs | all | none | A host loading an older JS bundle through `SandboxResources(bundle:)` gets every widget killed. |
| In-place `textContent` updates and the attribute fast paths run by default | all | features `noInPlaceText`, `noAttrFast` | Same ops as the full paths (differential fuzz tests on TS and Swift). |
| QuickJS is the TelegramMessenger/quickjs fork as a git submodule on every platform (pinned at c24d6b0: the CPU interrupt and out-of-memory are uncatchable in every engine path, including Promise executors, thenables, async generators and iterator `return()`) | all | none | SwiftPM and Xcode clone it for git dependencies; registry/zip copies without submodules fail to build. Web uses `packages/quickjs-wasm` (workspace package, committed build) instead of the npm variant. |

Under `strict` also: complex-script text changes draw from 8192 words refilling at 1024 per second (`text-churn`), all platforms.

## Opt-in (nothing changes until the host lists them)

- `config.features`: `canvasWords`, `binaryOps` (native node model validates binary batches), `webAnimations` (Element.animate and the `anim` message). Android (`SandboxConfig.features`, `WidgetHost.Options.features`) and the web (`WidgetHostOptions.features`) take them now; every glue drops what it does not implement (see the table below).
- `WidgetHost.Options.assetResolver` (iOS/macOS): `icon:` images expanded to `data:image/` before validation.
- `WidgetHost.Options.sideChannels` (iOS/macOS): side channels such as `Widget3DChannel`.
- `WidgetHost.Options` (iOS/macOS) keep their old defaults (`asyncValidation` and `sharedValidatorThread` false, the widget starts on the renderer's `ready`, `parallelBoot` only prewarms the runtime). New: `startBeforeRendererReady` runs the widget while the renderer loads. Ops, queries, bridge calls, logs and diagnostics reach the main thread in the order the widget produced them in both validation modes.
- ~~`SandboxResources.provide` (iOS/macOS): extra bundled libraries such as three.js.~~ Removed with profile 1 (no libraries).

## Swift-only

What exists only in the Swift glue (iOS, macOS). Everything here is opt-in; the
other hosts drop the feature names they do not implement.

| Feature | Swift | Node harness | Web | Kotlin | C++ |
|---|---|---|---|---|---|
| `webAnimations`, `noInPlaceText`, `noAttrFast`, `noCanvasRect` | yes | yes | yes | yes | yes |
| `canvasWords` (word payloads, `CanvasWords.swift`) | yes | yes | no | no | no |
| `binaryOps` + native node model (`NativeValidator.swift`, `NativeAnimValidator.swift`) | yes | decoder only | no | no | no |
| `morphUpdates` (`morph {html}`) | yes | yes | no | no | no |
| `icon:` images (`assetResolver`, `IconExpansion.swift`) | yes | no | no | no | no |
| Side channels (`Widget3DChannel`; debug builds only) | yes | no | no | no | no |
| Engine/renderer/validator pools, `WidgetFeed`, `WidgetBubble`, typed ops, WebKit view reuse | yes | — | worker pool only | validator pool only | no |

## Hardening before main (2026-10-16)

- A canvas word payload may be referenced once per batch; a binary batch that repeats a key is rejected whole.
- `anim` keyframe values may not be named `offset`, `easing`, `composite` or `computed-offset`; renderers keep animation state per widget and cancel an animation whose update fails; the web worker forwards `anim` only with `webAnimations`.
- The native node model rejects non-finite `prop` numbers like the script validator.
- `completeEditorCommand` is accepted only on a `<textarea>` with integer arguments.
- `Widget3DChannel` exposes `__tw3d` only in `DEBUG` builds (or with `TWIDGET_EXPERIMENTAL_3D`); release builds report a `3d` diag instead.
- The desktop `json.h` prints the shortest round-trip number on every target and parses numbers in the C locale.
- Gestures: every host counts only renderer events marked `trusted`, and only pointer presses: no `keydown`, and a click starts a gesture only with `detail` ≥ 1 (the click Enter or Space activates has 0).
- `CallSite.getFunction` / `getThis` answer `undefined` inside the sandbox (the shim no longer leaks its closures through `Error.prepareStackTrace`).
- Provided-library patterns must compile on their own (an unbalanced `)` could unanchor them); `icon:` is matched case-sensitively and limited per widget (256 distinct tokens, 2 MB of answers).
- The Node harness quantizes frame times to 100 µs and gates gestures on visibility (`RunOptions.visible`).

## Embedding contracts

- `Widget3DBytes.borrowedPointer` points into the widget's heap and is valid only during `Widget3DSurface.call`; a backend that keeps data for later uses `copy()`. Calls and channel teardown are serialised, and a call after `destroy()` never reaches a surface.
- `Widget3DChannel` limits are per channel (`init(backend:maxSurfaces:maxBytesPerCall:)`), clamped to at most 16 surfaces and 64 MB per call.
- `SandboxResources.provide`: the provider's `source` and `bytecode` closures run outside the resources lock (they may call back into `SandboxResources`); provided bytecode is trusted and must come from the app bundle or memory, never from a writable cache.
- Library patterns (`SandboxResources.provide`, the web `libraries` option, `provideLibrary`) match the whole URL case-insensitively; each must be a regular expression on its own, and one that matches `''`, `app:` or a generic CDN URL is refused (`provide` returns false).
- A renderer behind the `Renderer` seam marks real input `{"type":"event", …, "trusted": true}`; without the flag its events reach the widget but unlock no privileged bridge call. The bundled renderers (WebView/iframe from `isTrusted`, Blitz, litehtml) do.

## API

- Swift: `SandboxPolicy`, `SandboxConfig.policy`, `WidgetHost.Options.policy` and `TrustedValidator.setPolicy` are new. Additions only otherwise, with default implementations for new protocol requirements (`SandboxEngineDelegate` ops/binaryOps/anim, `Renderer.acceptsTypedOps`/`send(ops:)`). New parameters with defaults: `EnginePool.take(features:)`, `SandboxEngine.init(resources:features:)`, `WidgetHost.init(... executor:shell:)` (function references to the old signatures no longer type-check). New products `WidgetSandboxProtocol`, `CQuickJS`. Minimum macOS lowered from 13 to 10.13.
- Kotlin: `SandboxEngine.Delegate.onAnim` has a default body; Java implementers of the interface must implement it (no `-Xjvm-default`). `GestureCounter.gestureFor(type, detail)` and `UserActivation.isUserActivation(type, detail)` take the event type and its `detail` only (the key, repeat and modifier parameters are gone with keyboard gestures); `UserActivation.ACTIVATION_KEYS` is removed.
- C++: `SandboxEngineDelegate::onAnim` is a non-pure virtual at the end of the vtable, but new data members change the layout of `QuickJSContext`, `SandboxEngine` and `WidgetHost`: prebuilt consumers must recompile. New class `GestureCounter` (the host's gesture numbering); `WidgetHost::isUserActivation` forwards to it.
- TS: protocol additions only (`anims` query, `anim` messages, `trusted` on events, `gesture` on bridge and events, `features`). No exports removed, no diag kinds renamed. New diag kinds: `activation-blocked`, `text-budget`, `animation-limit`, `icon-limit`, `3d`, `3d-rejected`, `3d-stopped`.
- `LIMIT_DEFAULTS` are unchanged.
