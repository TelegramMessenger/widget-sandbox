# widget-sandbox — working notes for Claude Code

`twidget` in file, target and binary names is the internal codename; the
repo and packages are `widget-sandbox` (`org.telegram.widgetsandbox` where a hyphen is not allowed).

No-network sandbox for untrusted HTML widgets in Telegram messages (a widget is
mini-HTML: at most 4 KB after zlib and 32 KB uncompressed, `docs/tgw-format.md`). Read
`docs/overview.md` (the problem), `docs/decisions.md` (why things are
the way they are), `docs/protocol.md`, `docs/native-port-notes.md` (native
plan + "Android port: start here").

## Layout: `packages/` is the framework, `apps/` is everything else

`packages/` is what a client pulls in; nothing in it imports from `apps/`.
`apps/` is the demos, the renderer experiments and the dev tooling; they consume
the packages the way a client would (SwiftPM path, Gradle `includeBuild`,
pnpm workspace). A framework change touches `packages/` only.

- `packages/core` — the web host and the source of the three JS bundles
  (`src/{host,sandbox,renderer,shared}`), the Node harness, the unit tests;
  npm package `@telegram/widget-sandbox` (`exports`: `./host/*`, `./shared/*`,
  `./vite` = the Vite plugin that builds the bundles, `./node` = the harness).
  `profile.json` is the widget profile (profile 1, frozen 2026-10-17): the single
  source of what the validator, the shim and the renderer enforce;
  `scripts/profile-build.ts` generates `src/shared/profile-tables.ts`,
  `docs/profile-v1.md` and the out lists of the authors' guide
  `docs/widget-runtime-profile.md` and of its agent block (the standalone system prompt
  for a model writing widgets, `sandbox-node.ts --guide`) from it (never edit those by hand). Its
  `tables.uaStylesheet` is the renderer's UA layer (the root and text defaults on
  `--tg-theme-*` / `--tg-text-*`, the client-styled `<button>`, since v0.1.1);
  `src/shared/reference.ts` adds the reference theme to it for every reference rendering.
- `packages/ios` — the Swift package (iOS + macOS); its manifest is the
  repo-root `Package.swift` (SwiftPM needs it there for a git dependency).
- `packages/android` — the `widget-sandbox` Gradle library,
  `org.telegram.widgetsandbox:widget-sandbox` (its own settings.gradle.kts).
- `packages/desktop` — the Qt-free C++ core for tdesktop-class hosts.
- `packages/tools/qjsc` — the bytecode compiler built from the vendored quickjs-ng.
- `packages/conformance` — the widget profile conformance set that ships with the
  framework tag: `catalogue.json` (273 profile features, statuses synced from `profile.json`), `auto/`
  (55 self-checking widgets; `auto/26-context-*` per message context; `auto/31`–`34` must be stopped by the host, `"stops"` in the header), `visual/` (20 widgets for people + `visual/sdk`, 26 client-SDK cases),
  `refs/` (reference pictures posted next to the visual widgets), `manifest.json`, `channel.json`
  (the test-channel posts), `tools/report.mjs`; every widget fits a mini-widget (`build.ts --sizes`
  gates 3800 B after zlib at level 9) and a bubble (at most 560 px tall at 320 px wide, under the 600 px cap: `build.ts`
  for the visual set, `heights.ts` through the sandbox for all); every visual picture shows what the profile draws
  (`picture-check.ts`: the build fails when one differs from the widget drawn through the sandbox, so an out-of-profile
  feature is neutralised in reference mode by a template or `T.mode == 'ref'`). `CLIENTS.md` tells a client team how to
  run it on its renderer. Built and run by `scripts/conformance/{build,run-web}.ts`;
  `publish-channel.ts` fills the Telegram test group through a test bot; `test-bot.ts` is the reference bot for the cases
  that talk to it (sends them with their layer-233 fields, answers their requests, edits their state; `--self-test`).
- `packages/stress` — the renderer stress kit that ships with the framework tag: 127 widgets in 19 load ladders
  (`widgets/<load>/<value>.html`: paint effects, document size, churn, text, canvas, SVG, layout queries, CSS rules; the
  ladders cross every strict limit, in force or proposed, `crosses` in `manifest.json`), each publishing its own frame rate
  in `data-stress`; `CLIENTS.md` is the runner spec the client teams implement around their real renderer (measure pass:
  `compat`, `cpuQuotaMs` 10000 / window 1000, `mainThreadQuotaMs` 10000, `maxTimers` 256, 64 MB; production pass: strict),
  `run.schema.json`, `tools/report.mjs` (run files → `summary.md` / `.json`: per-load tables, µs per changed box for the
  WebView cost model's A/B, the full-rate level, where strict stops). Built by `scripts/stress/build.ts` (size ≤ 3800 B;
  through the sandbox: boots, inside profile 1, ≤ 560 px at 320 px), reference runner `scripts/stress/run-web.ts` (iframe,
  shadow → `reports/stress/`).
- `apps/web` — the dev app, chat feed demo, bench pages and the Playwright
  harness page (`src/main.ts` exposes `window.__tw`); `apps/ios/DemoApp`,
  `apps/macos/DemoApp`, `apps/android` (demo + the renderer modules, a second
  Gradle build with `includeBuild("../../packages/android")`);
  `apps/renderer-blitz`, `apps/renderer-litehtml` — the native renderer prototypes.
- Shared data at the root: `corpus/` (attacks, compat, generated, common,
  showcase, defaults, gestures, resize, interactive, htmlbubbles*), `tests/` (the Playwright suites, run against
  `apps/web`), `docs/`, `scripts/` (repo tooling: corpus generation, benches,
  profiling, `sync-demo-corpus.ts`).

## Invariants (never relax without asking)

1. Widget JS runs only inside QuickJS. No `eval`/`new Function`/inline
   script/`innerHTML` of untrusted strings in any browser or WebView context.
2. Everything coming out of the QuickJS context is hostile; the host validates
   every message against the strict schema and allowlists (`packages/core/src/host/validator.ts`,
   `allowlist.ts`, `css-sanitizer.ts`, `url-policy.ts`).
3. The renderer runs only trusted code and receives only validated ops.
4. No URL other than `data:` images, host-created `blob:` (none today)
   and `#fragment` reaches renderer DOM or CSS.
5. QuickJS has no network capability; `fetch`/XHR/WebSocket/EventSource/
   RTCPeerConnection/sendBeacon/importScripts/Worker are stubs throwing
   `NetworkDisabledError`.
6. Defense in depth on the renderer: a sandboxed frame with a strict
   per-widget CSP (iframe, WKWebView, WebView), or a shadow root with a scoped
   validator (`scope: 'shadow'`) on a page whose own CSP closes the network
   (`connect-src` only Telegram's endpoints, `form-action 'none'`, no
   third-party `img-src`/`media-src`/`font-src`/`frame-src`); every navigation
   cancelled, platform request blocking (`docs/decisions.md`, "The web renders
   through Shadow DOM").

Policy: `strict` is the default on every host (gesture gate for privileged
bridge calls, the counted caps of "Strict limits from the start" in `docs/decisions.md`:
32 K per text value, 48 K of document text, 8 K per attribute, 10 000 ops per batch, 2 000
canvas commands per op, 400 CSS rules / 32 `@keyframes`, past which the validator's fatal
`limit` diagnostic stops the widget; text budgets over text nodes, visible and
`data-*`/`aria-*` attributes and CSS strings); anything but exactly `compat`
is strict, `compat` (the old caps, nothing stops) is for tests, tooling and the stress kit's
measuring runs (`docs/changes.md`). Defaults on every host: 8 MB heap, 64 timers, CPU quota
300 ms/s (JS only) and the main-thread window `mainThreadQuotaMs` 100 ms/s (layout queries +
`opsCostMs`: 25 µs per op, 2 µs per canvas command), 4 KB of Web Storage per area.

**Shadow DOM renderer** (`packages/core/src/host/shadow-renderer.ts`, `docs/shadow-dom-renderer.md`,
the **shadow** toggle / `?shadow=1`, `TW_RENDERER=shadow` for the Playwright suites):
the renderer core (`packages/core/src/renderer/core.ts`, shared with the iframe and native
bundles) on a shadow root inside the host page, no iframe. **The web product
renderer since 2026-10-16** (release plan D2; invariant 6 reworded above, the
page CSP the web client needs is in `docs/decisions.md`); the validator runs in
`scope: 'shadow'` for it (`css-sanitizer.ts`). The iframe stays as the dev
app's default and the reference. Feed bench:
`apps/web/bench/feed.html` + `scripts/feed-bench.ts`. **Worker pool** (`packages/core/src/host/worker-pool.ts`,
`WidgetHost.Options.workerPool`, `?pool=N`, `TW_POOL=N pnpm test:compat`): the
worker protocol's `init` is `warm` (QuickJS + bundle) + `start` (the widget);
the pool keeps N warmed workers and buffers what they say while idle. Web host
glue only, the twin of `Pools.swift`. **Chat feed demo** (`?feed=1` / the
**feed** toggle, `apps/web/src/feed.ts`, `docs/chat-feed-web.md`): 72 bubbles on
`corpus/showcase` (21 interactive, animated demos on the full web profile,
`corpus/showcase/README.md`; `corpus=common` / the **corpus** select for the flat
renderer-comparison set) with the list's own lifecycle (prefetch, hide, drop, re-create),
the pool on/off, the renderer switched live (iframe | shadow | litehtml | blitz),
one hostile bubble from `corpus/attacks`, a metrics overlay; `window.__tw.feed`
for scripts. Dev tooling, the web twin of the iOS chat feed. **Bench panel**
(`apps/web/src/feed-bench.ts`, the **📊 bench** button / `?bench=1`): N passes of the
feed per renderer × pool cell with the scripted scroll, every boot recorded,
two live tables (median / p90) and a JSON report, nothing typed in;
`scripts/feed-report.ts` runs it headless → `docs/feed-report-web.{json,md}`.

Reference mode (`?reference=1` on the web; the **Reference** toggle and
`--autorun compare` in the iOS/macOS demo, `ReferenceWebView.swift` in the demo
sources, not in the package; the **Ref** toggle and `ReferenceWebView.kt` in the
Android demo, not in `widget-sandbox`) is native rendering of the same file:
dev-only tooling, never a product path, never for `attacks/`.

The native renderer prototypes replace only the renderer: the sandbox and
validator are the same, the native side gets validated ops only. **Blitz**
(`apps/renderer-blitz/`, Rust; `docs/blitz-experiment.md`) and **litehtml**
(`apps/renderer-litehtml/`, C++ core + vendored litehtml; `docs/litehtml-renderer.md`),
chosen with the **Renderer** picker WebView | Blitz | litehtml in the demos
(`--renderer <kind>` / `--blitz` / `--litehtml` in the iOS/macOS demo via
`NativeRenderer.swift` + the engine tables `BlitzRenderer.swift` /
`LitehtmlRenderer.swift`; `--es renderer <kind>` in the Android demo via
`apps/android/renderer-blitz` / `apps/android/renderer-litehtml`). Dev tooling for the
three-way comparison, not a product path; `Renderer.swift` / `Renderer.kt` is
the seam. litehtml has no DOM API, so its core keeps a shadow tree and
re-parses the serialised HTML on every batch (the HTMLBubbles model).

## Working rules

- `CONTRIBUTING.md` is the checklist for a commit to `main`: what must change with it, the tests, the notes.
- Gestures: who gets a press is `packages/core/src/shared/gesture-policy.ts` (four copies:
  `GesturePolicy.swift`, `GesturePolicy.kt`, `gesture_policy.cpp`, checked against
  `packages/core/tests/fixtures/gesture-policy.json`; change all four and the fixture together).
  Double taps over the widget are always the widget's, vertical drags always the chat's (v0.1.3: no claim has
  `y`); `touch-action: auto` leaves holds and horizontal swipes to the bubble, `pan-y` gives the widget horizontal
  drags, `none` horizontal drags and holds, `pan-x` nothing; the renderer core gives the engine every touch-action
  widened to allow vertical panning (`src/renderer/touch-action.ts`), sends `dblclick` / `contextmenu` itself and,
  with `arbitrate` (the web's `bubbleGestures`, the dev app and the feed by default), plays the bubble.
- The resize anchor (`TelegramWidget.setResizeAnchor`, v0.1.3) is a checked bridge call, not an action: every host
  keeps the latest edge next to the height it reports; the list keeps that edge of the bubble in place.
- Fix the shim generally (`packages/core/src/sandbox/*`), never per widget. The attack suite
  must stay green; a compat/generated regression is a shim bug until proven
  otherwise.
- Ask before adding heavy dependencies or relaxing an invariant.
- No library is bundled (profile 1): an external `<script src>`, CDN names
  included, never runs and nothing is fetched.
- The widget profile changes only through `packages/core/profile.json` (then
  `profile-build.ts`, the bundles, `profile.test.ts`); a feature goes in only when
  every client draws it, and adding one is profile 2, not an edit of profile 1.
- The same three JS bundles ship to every native platform via
  `packages/core/scripts/build-native-bundles.ts`, with precompiled `.qbc` bytecode next to
  the two QuickJS bundles and the libraries (never commit a `.js` without
  re-running the build: the engine refuses a stale `.qbc` and falls back to
  source, and the tests on every platform fail). Port the host glue, not the JS. iOS and
  macOS share one package: platform differences are `#if canImport(UIKit)` /
  `canImport(AppKit)` branches in `Platform.swift`, `RendererWebView.swift`,
  `WidgetHost.swift`, `WidgetView.swift`, never a second copy.
- Each native QuickJS runtime lives on its own dedicated thread with an 8 MB
  stack (iOS `JSThread`, Android `JSThread.kt`, C++ `js_thread.h`); never on a shared pool/queue
  and never on the main thread.
- Runtime limits (CPU quota, timer cap, visibility, `frozen`) are decided in
  trusted glue only. The quota policy is one pure module in four copies
  (`packages/core/src/host/cpu-budget.ts`, `CpuBudget.swift`, `CpuBudget.kt`,
  `packages/desktop/sandbox/src/cpu_budget.cpp`) checked against
  `packages/core/tests/fixtures/cpu-budget.json`; change all four and the fixture together.
  Glue changes go to all five copies (worker, Node harness, Swift, Kotlin,
  C++ `sandbox_engine.cpp`) in the same commit. The `intl` glue (`IntlBridge.swift`,
  `IntlBridge.kt`, `intl.cpp`; V8 itself on the web and in the harness, `src/shared/intl.ts`)
  is checked against `packages/core/tests/fixtures/intl-cases.json`, V8's answers written by
  `packages/core/scripts/intl-cases.ts` (add a case there, never by hand).

## Commands

```bash
git submodule update --init           # QuickJS (TelegramMessenger/quickjs fork) in packages/ios/Sources/CQuickJS/quickjs
corepack pnpm install                 # pnpm is not on PATH here
corepack pnpm dev                     # 127.0.0.1:5173 (or .claude/launch.json "dev")
corepack pnpm test:unit               # 775 core tests, including coalesced move deltas and projected 3D showcase checks; binary ops corpus uses a fixed Date
corepack pnpm test:attacks            # 110 files (5 request-* on the bot's requests; haptic-timer-buzz: haptics need a touch, v0.2.2; activation-pointer-capture, activation-touch-swipe with CDP touch input, css-light-dark-url in both themes, mem-huge-bitmaps, timer-id-reuse: the tweb audit), canary hits must be 0 (the web demo runs strict; VITE_TW_POLICY=compat or ?policy=compat for the old limits)
corepack pnpm test:activation         # privileged bridge calls need a real gesture (strict)
corepack pnpm test:gestures           # who gets a press (docs/sdk-v1.md §5.5, gesture-policy.ts): the dev app's bubble in phone mode cancels a hold / swipe on touch-action: auto, none gets contextmenu, a vertical drag is the chat's on none and pan-x too, the engine's widened touch-action, a double tap is always the widget's (dblclick), the leading 20 px, desktop and off modes, the width control and the size row; iframe + shadow
corepack pnpm test:sdk                # SDK v1 on the web host (docs/sdk-v1.md): theme variables on the renderer root, the UA layer's root and text defaults on the text tokens, gesture claims, storage across a restart, locale, link and haptic actions; the widget's state and requests through the dev app's mock bot (requests in an original, a group, a live and a plain forward, the state events, copyText, background requests, the poll demo); iframe + shadow
corepack pnpm test:compat             # → docs/compat-report.md
corepack pnpm test:generated          # → docs/compat-report-generated.md
corepack pnpm test:lifecycle          # off-screen pause (frames stop, timers 1 Hz, visibilitychange)
corepack pnpm test:htmlbubbles        # 116 Telegram demos ported to classic widgets (corpus/htmlbubbles-ported) → docs/compat-report-htmlbubbles-ported.md; 107–110/116: 6–9 known screenshot-only or timing failures (see corpus/htmlbubbles-ported/README.md)
TW_RENDERER=shadow corepack pnpm test:attacks   # the same suite on the Shadow DOM renderer (no iframe): must stay at 0 hits / 0 requests; also test:generated / test:common → docs/compat-report-<group>-shadow.md
corepack pnpm tsx scripts/feed-bench.ts --counts 50,200 --mode ops,host --cv --pool 8,32 --out docs/feed-bench-web.md   # iframe vs shadow renderer in a feed of N bubbles (needs pnpm dev); ops = renderer only, host = full WidgetHost per bubble, --pool = also with a warmed worker pool
corepack pnpm tsx scripts/feed-report.ts --passes 5 --count 60 --origin http://127.0.0.1:5173   # the feed's bench headless (6 renderer × pool cells × 5 passes × 60 bubbles, ~30 min; --combos iframe:on,blitz:off,…) → docs/feed-report-web.json + .md; needs pnpm dev and the two wasm builds
corepack pnpm test:defaults           # 6 widgets with little or no CSS, drawn by the renderer's UA layer (corpus/defaults, README there; the test group's Defaults topic) → docs/compat-report-defaults.md
corepack pnpm test:gesture-corpus     # what the horizontal-only rule (v0.1.3) removed (corpus/gestures, README there): joystick and 2048 each on touch-action none (-xy) and pan-y (-x), now alike, a wheel picker (pan-x) that no longer turns, and fine with horizontal only (pan-y): paddle game, card carousel, range slider → docs/compat-report-gestures.md; the feed's corpus=gestures; the test group's Gestures topic, --topic gestures
corepack pnpm test:resize-corpus      # one widget per resize anchor (corpus/resize, README there): an FAQ on the default (top), a support thread that grows upward (bottom), results that grow downward (top) → docs/compat-report-resize.md; the test group's Resize topic, --topic resize
corepack pnpm test:showcase           # 27 interactive demos on widget profile 1 (corpus/showcase, README there; the web feed's content) → docs/compat-report-showcase.md; in the iOS/macOS demo: the showcase/<batch> sections, --autorun showcase, and the iOS chat feed's Widgets picker (showcase by default; --feed-set corpus=showcase from a shell)
corepack pnpm test:common             # 32 widgets on the common WebView/Blitz/litehtml profile (corpus/common, README there) → docs/compat-report-common.md; the same files run on every renderer: --autorun common [--blitz|--litehtml] in the iOS/macOS demo, --es autorun common on Android, CORPUS=common scripts/android-bench.sh <renderer> 20
corepack pnpm build:blitz-web         # Blitz renderer for the web dev app: cargo (wasm32, rustup) + wasm-bindgen-cli (version from apps/renderer-blitz/Cargo.lock) → apps/renderer-blitz/pkg-web (gitignored, served at /blitz/); then the Blitz toggle / ?blitz=1, apps/web/tests/unit/blitz-wasm.test.ts stops skipping
TW_RENDERER=blitz corepack pnpm test:generated   # generated corpus on the Blitz wasm renderer → docs/compat-report-generated-blitz.md + reports/generated-blitz (pixel diff informational, no clicks: a canvas)
corepack pnpm build:ios               # bundles → packages/ios/Sources/WidgetSandbox/WidgetJS, corpus → apps/ios/DemoApp/Corpus
corepack pnpm build:macos             # same outputs (the macOS demo references them)
corepack pnpm build:android           # bundles → packages/android/widget-sandbox/src/main/assets/WidgetJS, corpus → apps/android/demo/src/main/assets/Corpus
corepack pnpm build:desktop           # bundles → packages/desktop/WidgetJS, fixture → packages/desktop/tests/fixtures
cd packages/desktop && cmake -S . -B build -G Ninja && cmake --build build && ./build/twidget-tests && ./build/twidget-run --attacks   # C++ core: 75 tests; 105 attacks, 0 leaks into validated ops; also --corpus ../../corpus/compat
cd packages/desktop && cmake -S . -B build -G Ninja -DTW_BLITZ=ON && cmake --build build && ./build/twidget-tests && ./build/twidget-run --corpus ../../corpus/compat --blitz --dpr 2 --png-dir /tmp/compat   # + Blitz renderer (rustup cargo): 78 tests; --attacks --blitz counts renderer fetch attempts as leaks; single file: --blitz --png out.png [--click X,Y]
TWB_BACKEND=cg ./build/twidget-run --corpus ../../corpus/generated/with-profile --blitz --dpr 2 --png-dir /tmp/gen-cg   # Blitz laid out by Rust, painted by CoreGraphics/CoreText (apps/renderer-blitz/src/cg.rs, Apple only; twb_set_backend in the C ABI); three-way gallery: corepack pnpm tsx scripts/blitz-experiment/gallery-native.mts reports/blitz-experiment/native/generated vello=DIR:TABLE cg=DIR:TABLE litehtml=DIR:TABLE --strips 20; on the simulator: SIMCTL_CHILD_TWB_BACKEND=cg xcrun simctl launch booted org.telegram.widgetsandbox.TwidgetDemo --blitz (iOS emoji paint only on this backend); Android twin: apps/renderer-blitz/src/dl.rs records the scene as a display list, CanvasScene.kt replays it with Canvas.drawGlyphs — `--es renderer blitz_canvas` in the demo (Blitz·Canvas in the picker)
cd packages/desktop && cmake -S . -B build -G Ninja -DTW_BLITZ=ON -DTW_LITEHTML=ON && cmake --build build && ./build/twidget-tests && ./build/twidget-run --corpus ../../corpus/compat --litehtml --dpr 2 --png-dir /tmp/compat   # + litehtml renderer (macOS): 81 tests with both; the same flags as --blitz
corepack pnpm build:litehtml-web      # litehtml → wasm (brew install emscripten) → apps/renderer-litehtml/pkg-web; the dev UI's litehtml toggle / ?litehtml=1, TW_RENDERER=litehtml pnpm test:generated, apps/web/tests/unit/litehtml-wasm.test.ts
# dev UI "compare all" / ?compare=1: iframe, Blitz, litehtml and the reference side by side (one sandbox each, booted one after another)
corepack pnpm tsx packages/core/scripts/profile-build.ts   # after editing packages/core/profile.json: regenerates src/shared/profile-tables.ts + docs/profile-v1.md + the out lists in docs/widget-runtime-profile.md (the guide's and its agent block's) (--check: exit 1 when stale; profile.test.ts runs it); then pnpm build:ios|android|desktop
corepack pnpm tsx packages/core/scripts/intl-cases.ts   # after adding an Intl case (or an ICU update of Node): rewrites packages/core/tests/fixtures/intl-cases.json with V8's answers (--check: exit 1 when stale; intl-cases.test.ts runs it); then pnpm build:ios|android|desktop copy it to the native tests
corepack pnpm tsx scripts/conformance/build.ts   # rebuild the conformance set (catalogue, inlined harness, Chromium refs, manifest) after editing packages/conformance/{auto,visual} or scripts/conformance; starts a dev server (or --origin) for the automatic set's heights and the picture check (picture-check.ts: each visual picture against the widget drawn through the sandbox, diffs in reports/conformance/pictures)
corepack pnpm tsx scripts/conformance/run-web.ts --origin http://127.0.0.1:5173 && node packages/conformance/tools/report.mjs --out packages/conformance/results reports/conformance/{iframe,shadow}.json   # the set on the web renderers (needs pnpm dev; blitz/litehtml too when their wasm is built) → packages/conformance/results/summary.md; the dev app lists it as the conformance/ group
corepack pnpm tsx scripts/stress/build.ts   # rebuild the stress kit (packages/stress) after editing scripts/stress/loads.ts: widgets, manifest, README table; gates: size, and through the sandbox boots/profile/height (starts a dev server, or --origin; --sizes: sizes only)
corepack pnpm tsx scripts/stress/run-web.ts --origin http://127.0.0.1:5173 && node packages/stress/tools/report.mjs --out reports/stress/summary reports/stress/*.json   # the stress kit on the web renderers (iframe, shadow; --pass production = strict, where each ladder stops; --warmup/--measure) → summary.md
corepack pnpm tsx scripts/profile/probes.ts && corepack pnpm tsx scripts/profile/corpus.ts   # the feature probes and corpus usage behind the historical draft docs/widget-profile-draft.md and widget profile 1 (docs/profile-v1.md): 53 probes through validator + Blitz + litehtml (desktop build with both), then feature usage over the four corpora → reports/profile/
cd packages/android && JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home ./gradlew :widget-sandbox:testDebugUnitTest :widget-sandbox:connectedDebugAndroidTest   # 26 JVM + 113 instrumented tests (the SdkPolicy fixture runs in both); needs a booted emulator (AVD twidget-35)
corepack pnpm tsx packages/core/scripts/sandbox-node.ts corpus/compat/tabs.html --verbose   # also prints the widget's size as the server counts it (zlib -9 / raw, limits 4096 / 32768, src/shared/widget-size.ts) and the resize anchor when the widget set one with a warning or an error line
corepack pnpm tsx packages/core/scripts/sandbox-node.ts --guide   # the agent block of the authors' guide (docs/widget-runtime-profile.md): the standalone system prompt for a model writing widgets (a bot's model sees only it; its out list is generated by profile-build.ts)
corepack pnpm tsx packages/core/scripts/sandbox-node.ts widget.html --width 360 --hold 120,40 --double-tap 120,40 --swipe 60,80,120,0 --tap x,y --right-click x,y [--desktop] [--json]   # the real renderer core in headless Chromium at that bubble width (content height) and the presses on a phone's bubble: per press the element, its touch-action claim, the policy's outcome, the events the widget received, a hint
corepack pnpm tsx packages/core/scripts/sandbox-node.ts corpus/interactive/01-poll.html --message private --public-data '{"v":[3,5,1]}' --viewer 1001,Ann,Lee,ann_lee --width 360 --tap 60,50 --answer '{"result":"{\"voted\":0}"}' [--message group|via-bot|no-queries|live-copy|copy|preview] [--chat-data s] [--answer-error TIMEOUT] [--set-data json] [--offscreen]   # the widget's state (chat state, shared state) and the server's flags for a kind of message, every request answered (or failed); a `request` line per request (sent by a tap / in the background / held off screen / refused and why, what came back), a `copyText` line per copy; `--offscreen` boots the widget hidden and then shows it, as Telegram's clients do
scripts/android-bench.sh webview|blitz|litehtml|none 20 [prewarm]   # renderer start-up/memory bench in the Android demo (needs installDebug + emulator); POOLS=1 = validator pool; python3 -I scripts/android-bench-print.py --table <names>; docs/android-renderer-bench.md
xcodebuild test -scheme WidgetSandbox-Package -destination 'platform=iOS Simulator,name=iPhone 17,OS=26.5'   # 295 discovered tests, including 8 task-boundary regressions (the test scheme SwiftPM generates; on the iOS 27 simulator IntlBridgeTests.testPluralRulesMatchICU fails on es/bg ordinals, an ICU difference of the OS, older than this code); corpus/attacks also runs through the native node model (AttackCorpusNativeModelTests)
cd apps/ios/DemoApp && xcodegen generate && xcodebuild build -scheme TwidgetDemo -destination 'platform=iOS Simulator,name=iPhone 17,OS=26.5' CODE_SIGNING_ALLOWED=NO -derivedDataPath /tmp/twidget-sim && xcrun simctl install booted /tmp/twidget-sim/Build/Products/Debug-iphonesimulator/TwidgetDemo.app && xcrun simctl launch booted org.telegram.widgetsandbox.TwidgetDemo --blitz   # demo on the Blitz renderer (--litehtml: litehtml); the pre-build phases run apps/renderer-blitz/build-apple.sh (rustup cargo) and apps/renderer-litehtml/build-apple.sh (cmake + ninja)
cd apps/renderer-litehtml && cmake -S . -B build -G Ninja && cmake --build build && ./build/twidget-litehtml-tests && ./build/twidget-litehtml-run msgs.json out.png --width 360 --dpr 2   # litehtml core on the host: 7 tests; msgs.json (the theme message, then the ops) from `pnpm tsx packages/core/scripts/sandbox-node.ts <widget> --ops-file msgs.json [--dark] [--font-size 17]`
xcrun simctl launch booted org.telegram.widgetsandbox.TwidgetDemo --autorun feed --feed max --feed-count 40 --feed-scroll step   # chat feed (iOS only): scripted scroll, feed-report.json in the app container; `--feed baseline`, `--feed-set pools=false,…`; build with -configuration Release for numbers (docs/chat-feed-ios.md); the feed has the web feed's bar: renderer WebView | Blitz | litehtml switched live, pool on/off, hostile bubble, corpus, count, reset, ▶ scroll (`--feed-set renderer=blitz,hostile=cpu-raf-burn,corpus=showcase`)
swift test --filter TaskRendererHostTests   # 8 Swift host regressions: task/ops/query ordering, JSON/binary, startup buffering, exceptions, CPU stops, destroy, plain-renderer parity; Debug, no WebView or native renderer needed
xcodebuild test -scheme WidgetSandbox-Package -destination 'platform=macOS'   # 294 discovered tests, including 8 task-boundary regressions (testPluralRulesMatchICU as on iOS 27 on macOS 27); the frame-pause test is iOS-only (xctest windows are never "visible" on macOS)
cd apps/macos/DemoApp && xcodegen generate && xcodebuild build -scheme TwidgetDemoMac -destination 'platform=macOS' -derivedDataPath /tmp/twidget-mac && /tmp/twidget-mac/Build/Products/Debug/TwidgetDemoMac.app/Contents/MacOS/TwidgetDemoMac --autorun attacks   # also generated|compat|pause; --features binaryOps,canvasWords runs the attacks through the Swift native node model (NativeValidator), which Playwright never exercises; reports in ~/Library/Application Support/org.telegram.widgetsandbox.TwidgetDemoMac
```

Pitfalls: Playwright suites must not run concurrently (shared canary on
127.0.0.1:9999); `reuseExistingServer` means a dev server already on 5173 (another
checkout) gets tested instead of yours — run with `TW_PORT=5174` from a worktree;
editing `packages/core/src` or `apps/web` during a Playwright run reloads Vite and breaks
the run; `tests/global-setup.ts` wipes `reports/` so a `-g` partial run yields a
partial report; macOS has no `timeout`. On Windows `build:*` needs a C compiler for qjsc in `CC` (Visual
Studio's `VC\Tools\Llvm\x64\bin\clang.exe`): its bundles and `.qbc` are byte-identical to the macOS ones.

### Projected 3D showcase checks

`corepack pnpm exec tsx --test packages/core/tests/unit/showcase-3d.test.ts`
runs 33 tests for the six `corpus/showcase/3d` widgets: payload limits,
strict validation and finite canvas output in both themes, model/zoom persistence,
reset, full cat mouse-pitch restoration, canceled-pointer rollback, query-free redraws and live theme changes. `corepack pnpm test:showcase --grep '3d/'`
runs the six sandbox/reference comparisons; repeat with `TW_RENDERER=shadow`.
These widgets use canvas 2D projection, without the experimental 3D channel.

## Status (Oct 2026)

Done: phases 0–6 (spike, shim, compat 22 widgets, attacks 82, iOS port with
device install, generated corpus 40/40 web + iOS), the Android port
(emulator: 45 instrumented tests, 82 attacks with 0 canary hits, compat and
generated corpora load; the same on a real phone), the runtime limits (`docs/decisions.md`, Runtime limits)
on all three platforms (CPU quota 300 ms/s with deferral, timer cap (64 since 2026-10-29),
last render kept as a frozen fallback, off-screen pause; attacks 93, incl. the memory cap:
on the web the wasm `Memory.maximum` is the limit, `setMemoryLimit` alone is not) and the
macOS port (same Swift package on AppKit, demo with autorun: attacks 0 hits,
generated 40/40, compat 21/21, window-occlusion pause) and bytecode
precompilation of the bundles (`.qbc` next to each `.js`, built by
`packages/tools/qjsc` from the vendored quickjs-ng; stale blobs are refused and the
glue falls back to source) and the Blitz renderer prototype behind the
`Renderer` seam on Android, iOS, macOS, the C++ core and the web (one Rust crate: JNI,
C ABI, and the same C ABI as wasm32 through wasm-bindgen; `packages/desktop/` got `WidgetHost` +
`Renderer`, the web host got `Renderer` + `IframeRenderer`; 2026-10-10 a second
`anyrender` backend on CoreGraphics/CoreText, `TWB_BACKEND=cg`, same scene, platform
glyphs, diff vs vello < 2 %), the litehtml
renderer prototype behind the same seam on iOS, macOS, Android, the C++ core
(`-DTW_LITEHTML=ON`, macOS only: the CoreGraphics backend) and the web
(Emscripten wasm, `pnpm build:litehtml-web`, `?litehtml=1`) — C++ core
`apps/renderer-litehtml/` with vendored litehtml, shadow tree re-parsed per batch,
CoreText/CoreGraphics, JNI/Canvas and canvas-2D backends; macOS autorun
generated 40/40, desktop compat 21/21 and attacks 0 leaks, host harness + 6
tests, 3 desktop tests, 3 Node wasm tests; the demos pick WebView | Blitz |
litehtml — and
the iOS chat feed (`apps/ios/DemoApp/Sources/ChatFeed`, `docs/chat-feed-ios.md`):
shared WebKit environment, renderer/engine/validator pools, parallel boot,
validation off the main thread, snapshot-and-drop in the list — all
`WidgetHost.Options` flags, iOS only so far (Android/desktop can port them).
2026-10-12: `corpus/showcase`, 21 interactive, animated demos on the full web
profile (`corpus/showcase/README.md`), 21/21 on `pnpm test:showcase`, the web
feed's default corpus (the **corpus** select / `?corpus=common` keeps the flat set);
not in the native demos yet.
2026-10-16: the `telegram-macos` integration branch (opt-in `features`:
`webAnimations`, `canvasWords`, `binaryOps` with the Swift `NativeValidator`,
`morphUpdates`; the `strict`/`compat` policy and user activation; the
TelegramMessenger/quickjs fork as a submodule; the Swift feed and pools) merged
with `tdesktop` and hardened before it becomes `main` (`docs/decisions.md`,
"Pre-release hardening"): `strict` by default everywhere, budgets over
`data-*`/`aria-*` and CSS strings, one canvas payload per reference, `anim`
keyframe members and per-renderer animation state, 3D only in debug builds,
`features` on Android and the web, shortest/locale-free numbers in `json.h`.
Decisions: `docs/decisions.md`; the SDK and storage contract `docs/sdk-v1.md`; the
profile `docs/profile-v1.md`; the web product renderer is the shadow DOM one (D2).
Working-process documents (reviews, briefs, discussions) stay out of the repository
(`notes/`, gitignored).
2026-10-16: the QuickJS fork reviewed (`docs/decisions.md`, "The QuickJS fork review"; all 22
allocator/interpreter commits kept) and the submodule moved to fork master c24d6b0: an uncatchable exception (CPU interrupt,
out of memory) is a runtime state, so a Promise executor, thenable, async generator or iterator
`return()` can no longer swallow the CPU limit; `.qbc` unchanged, `packages/quickjs-wasm/dist` rebuilt.
Same day: the conformance set moved from `corpus/conformance` to `packages/conformance` so it
ships with the tag; `v0.1.0` release notes and known issues in `docs/changes.md`, the client
integration guide `docs/integration.md`, the widget payload (mini-HTML as `messageMediaWidget`,
layer 233) in `docs/tgw-format.md`.
2026-10-17: widget profile 1 frozen and enforced (`packages/core/profile.json`,
`docs/profile-v1.md`, `docs/decisions.md` "Frozen as widget profile 1"): the validator,
`NativeValidator`, the shim (`TelegramWidget.profile`, `isProfileAtLeast`, `CSS.supports`
and `@supports` from one evaluator) and the renderer (no keyboard or focus, no selection (opt-in since v0.1.1),
the client-styled `<button>`); no bundled libraries on any platform; `Intl.formatToParts`
in every glue copy; the compat widgets that need libraries or form controls moved to
`corpus/out-of-profile/`.
2026-10-17: SDK v1 (`docs/sdk-v1.md`): `TelegramWidget` v1 in the shim, the checks in one
policy with four copies and a fixture (`packages/core/src/shared/sdk-policy.ts`,
`SdkPolicy.swift`, `SdkPolicy.kt`, `sdk_policy.cpp`, `packages/core/tests/fixtures/sdk-policy.json`),
`WidgetTheme` / `onAction` / `onStorageCommit` / `onGestureClaim` on every host, Web Storage
per message, `sendMessage` gone; the corpora reworked for profile 1 and SDK v1.
2026-10-20: v0.1.1 (`docs/changes.md`): the renderer's UA layer covers the root and the text
elements (a clarification of profile 1, `docs/decisions.md` "The renderer's defaults"; the
padding token is the body's margin, the bubble under the transparent renderer is the canvas,
the shadow renderer copies a root or body background onto its host), text tokens
`WidgetTheme.text` → `--tg-text-*` on every host, `bg_color` = this bubble's colour; text is
not selectable by default and `user-select: text` enables it (D18 replaced, "Text selection");
litehtml bakes the layer in (CMake reads `profile.json`, `src/theme.cpp` sets the variables);
`corpus/defaults`, conformance `visual/14-unstyled-document`, `15-body-background`,
`16-selection` and `auto/22-ua-defaults`;
`scripts/conformance/publish-channel.ts` fills the test group (config and state in
`~/.config/widget-sandbox`, never in the repository).
Same release: GitHub main's 23edeeb, d470c5b and 7654e69 (2026-10-19) folded in: live
`getElementsByClassName` / `getElementsByTagName` (re-scanned only after an element insert or
removal or a class write, `tests/unit/live-collections.test.ts`), a layout query sends pending
Web Animations state first and the Apple host keeps `anim` in order with queries, CLDR plural
rules and fuller Intl options on Apple; the ECMA-402 date defaults moved into the shim
(`installIntl` in `globals.ts`) for every host; Intl parity on Swift, Kotlin and the C++
`BasicIntl`, all checked against V8's answers in `packages/core/tests/fixtures/intl-cases.json`
(what stays approximate per platform: `docs/changes.md`, v0.1.1).
2026-10-22: v0.1.2 (`docs/changes.md`, `docs/decisions.md` "Gestures: `touch-action` is the
claim"): who gets a press is decided by one shared policy (`gesture-policy.ts` and its Swift,
Kotlin and C++ twins on `tests/fixtures/gesture-policy.json`; 10 px slop, 500 ms hold, 300 ms /
40 px double tap, 20 px leading edge strip): double taps over the widget are always the widget's
(`dblclick` on every element; the reaction stays outside the widget); holds and drags by CSS
`touch-action` per element: `auto` leaves holds (menu) and swipes to the bubble and the widget
gets `pointercancel`; `pan-y` / `pan-x` give it the other axis; `none` gives it everything, with
the renderer's own `contextmenu` (`input.dblclick` in, `input.long-press` in on `none`, decision
G1); a container's touch-action covers its children, as in CSS. The claim gains `hold` (Kotlin and
C++: `onGesture`); the web
renderer can play the bubble (`bubbleGestures`, `arbitrate` / `bubble` messages), which the dev
app (`gestures` select: phone | desktop | off, a bubble-width control) and the feed use by
default; the Node harness got `--width` and simulated presses (`gesture-run.ts`, headless
Chromium); the demo bubbles on iOS/macOS and Android follow the policy; conformance
`auto/23-gestures`, `visual/sdk/12-drag-gestures` rewritten.
2026-10-24: v0.1.3 (`docs/changes.md`): vertical drags are always the chat's (`docs/decisions.md` "Vertical drags
are the chat's", G2: `claimOf` never claims `y`, the arbiter gives every vertical touch drag to the chat, also after a
hold; `pan-x` claims nothing; the renderer widens the engine's touch-action to allow vertical panning, the claim reads
the widget's own value from `--tw-touch-action`), decided with `corpus/gestures` (the gesture examples branch, now the
record of what the rule removed); `TelegramWidget.setResizeAnchor('top' | 'bottom')` (R1: a checked bridge call in
`sdk-policy` and its three twins, the host keeps the latest edge next to the height; web `onResize(height, anchor)`,
`WidgetDiagnostics.resizeAnchor` elsewhere; the dev app, the feed, the iOS demo (single-widget screen and chat feed) and the Android demo list keep the anchored edge;
`corpus/resize`, `auto/24-resize-anchor`, `visual/sdk/18-resize-anchor`).
2026-10-26: v0.2.0 (`docs/changes.md`), the widget's state and requests, widget interactivity (`docs/decisions.md` "The bot's data and requests",
decision I1; `docs/sdk-v1.md` §2.13–§2.16, §5.11; the server's final spec in `docs/tgw-format.md`; part of SDK v1 at the
launch in layer 233): a widget is fixed code plus changeable state; the HTML never changes after sending, only the state, which
only the bot writes: `TelegramWidget.chatData` / `publicData` (the chat state and the shared state, ≤ 1 KB of text each;
`chatDataChanged` / `publicDataChanged`), `isForward`, `chatType`, `viewer`
(`{ id, first_name, last_name?, username?, is_premium? }` as Mini Apps' `WebAppUser`, only where the message has `can_query`; a copy is told nothing about its chat: `chatType` null in every copy); `request(data)` (a callback button inside the widget, resolves `{ result }`; where it may ask is the
server's flag per message: `can_query` on a tap, `background_query` in the background while visible, one when the widget
appears then one per 5 s; one in flight; the client shows a network marker exactly where `can_query`), `copyText(text)`, `watchPublicData()` / `unwatchPublicData()` (live shared state: the client polls a shared widget's `public_data` only while it watches) and `isPreview` (an inline result before it is sent: its state,
no input, no requests or actions, nothing about the user, no storage). Every host takes `data` and `context` (the server's
flags `isCopy` / `canQuery` / `backgroundQuery`, the chat type, the viewer, `preview`), `setData(full state)` (the server's update,
the polled shared state), `onRequest({ data }, complete)` (`{ result }` only) and `onCopyText`; the checks are
`sdk-policy` and its twins on the fixture (`RequestGate` with `show`). The dev app's **Bot (mock)** panel
(`apps/web/src/mock-bot.ts`, `?message=private|group|via-bot|no-queries|live-copy|copy|preview`, the network marker in the bubble's header) plays the bot; `corpus/interactive`; five
`request-*` attacks. Released ahead of the full matrix.
2026-10-26: v0.2.1 (`docs/changes.md`): the full matrix on v0.2.0's code, conformance cases for the widget's state and
requests (`auto/25`, `auto/26-context-*`, `visual/sdk/16`, `19`–`25`; case headers say how to send a case and what the bot
answers; `scripts/conformance/test-bot.ts`), `auto/23-gestures` split under the 560 px gate (now enforced for the automatic
set), and the docs reworded: a widget is fixed code plus changeable state (chat state, shared state).
2026-10-26: v0.2.2 (`docs/changes.md`): haptics need a touch (a Telegram Android bug: a timer vibrated the phone non-stop):
free during a press and 5 s after; otherwise a 1 s series, the next one after a touch or 10 s later (`HapticGate`, the
renderer's `release`, `docs/decisions.md` "Haptics need a touch").
2026-10-27: v0.2.3 (`docs/changes.md`): Fela's fixes (canvas arguments converted as a real context does; a script's
`new Image()` loads before it is inserted; the tooling builds on Windows), conformance pictures checked against the sandbox
(`scripts/conformance/picture-check.ts`), and `nativeScrolling` (Swift, tmacos) honoured only in DEBUG or
`TWIDGET_EXPERIMENTAL_SCROLLING` builds (`docs/decisions.md`, "Native scrolling: an experiment").
2026-10-28 (unreleased, `docs/changes.md`): Telegram's iOS and Android clients create the host off screen, so a
background request made off screen (with `backgroundQuery`) is now held in the glue and goes out when the widget appears
(`RequestGate` verdict `held`, `show(now)` → whether it goes out; `hidden` is gone), and the shim no longer answers `BUSY`
itself: it forwards every request and the glue's gate alone decides (`docs/sdk-v1.md` §2.14); the harness's
`--offscreen`, the dev app's `?offscreen=1`.
2026-10-29: strict limits from the start (`docs/decisions.md`, `docs/changes.md` "Next release"): §9 of the
paint-limits draft, every count and size limit at about twice the largest real widget; past a counted cap the widget
stops instead of losing the op; the main-thread window as a second `CpuBudget`; `compat` keeps the old caps.
Open: a reproducible build of `packages/quickjs-wasm/dist`, versions
and tags (VERSION.json), the renderer and engine pools on Android (the validator pool is there: `ValidatorPool`, `WidgetHost.Options.pools` / `sharedValidatorThread`; the Android renderer bench, `docs/android-renderer-bench.md`, puts QuickJS + validator at 6.3 MB per bubble against 1–4 MB for any renderer), litehtml on the C++ core and the web (wasm) for the full three-way matrix, device pentest of the
WebView blockers on both platforms.
