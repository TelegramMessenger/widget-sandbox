# widget-sandbox — working notes for Claude Code

`twidget` in file, target and binary names is the internal codename; the
repo and packages are `widget-sandbox` (`org.telegram.widgetsandbox` where a hyphen is not allowed).

No-network sandbox for untrusted HTML widgets in Telegram messages (a widget is
mini-HTML: at most 4 KB after gzip and 32 KB uncompressed, `docs/tgw-format.md`). Read
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
  `scripts/profile-build.ts` generates `src/shared/profile-tables.ts` and
  `docs/profile-v1.md` from it (never edit those two by hand).
- `packages/ios` — the Swift package (iOS + macOS); its manifest is the
  repo-root `Package.swift` (SwiftPM needs it there for a git dependency).
- `packages/android` — the `widget-sandbox` Gradle library,
  `org.telegram.widgetsandbox:widget-sandbox` (its own settings.gradle.kts).
- `packages/desktop` — the Qt-free C++ core for tdesktop-class hosts.
- `packages/tools/qjsc` — the bytecode compiler built from the vendored quickjs-ng.
- `packages/conformance` — the widget profile conformance set that ships with the
  framework tag: `catalogue.json` (244 profile features, statuses synced from `profile.json`), `auto/`
  (21 self-checking widgets), `visual/` (13 widgets for the test channel), `refs/`,
  `manifest.json`, `tools/report.mjs`; `CLIENTS.md` tells a client team how to run it
  on its renderer. Built and run by `scripts/conformance/{build,run-web}.ts`.
- `apps/web` — the dev app, chat feed demo, bench pages and the Playwright
  harness page (`src/main.ts` exposes `window.__tw`); `apps/ios/DemoApp`,
  `apps/macos/DemoApp`, `apps/android` (demo + the renderer modules, a second
  Gradle build with `includeBuild("../../packages/android")`);
  `apps/renderer-blitz`, `apps/renderer-litehtml` — the native renderer prototypes.
- Shared data at the root: `corpus/` (attacks, compat, generated, common,
  showcase, htmlbubbles*), `tests/` (the Playwright suites, run against
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
bridge calls, 64 KB text caps, text budgets over text nodes, visible and
`data-*`/`aria-*` attributes and CSS strings); anything but exactly `compat`
is strict, `compat` is for tests and tooling (`docs/changes.md`).

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
  C++ `sandbox_engine.cpp`) in the same commit.

## Commands

```bash
git submodule update --init           # QuickJS (TelegramMessenger/quickjs fork) in packages/ios/Sources/CQuickJS/quickjs
corepack pnpm install                 # pnpm is not on PATH here
corepack pnpm dev                     # 127.0.0.1:5173 (or .claude/launch.json "dev")
corepack pnpm test:unit
corepack pnpm test:attacks            # 98 files, canary hits must be 0 (the web demo runs strict; VITE_TW_POLICY=compat or ?policy=compat for the old limits)
corepack pnpm test:activation         # privileged bridge calls need a real gesture (strict)
corepack pnpm test:sdk                # SDK v1 on the web host (docs/sdk-v1.md): theme variables on the renderer root, gesture claims, storage across a restart, locale, link and haptic actions; iframe + shadow
corepack pnpm test:compat             # → docs/compat-report.md
corepack pnpm test:generated          # → docs/compat-report-generated.md
corepack pnpm test:lifecycle          # off-screen pause (frames stop, timers 1 Hz, visibilitychange)
corepack pnpm test:htmlbubbles        # 116 Telegram demos ported to classic widgets (corpus/htmlbubbles-ported) → docs/compat-report-htmlbubbles-ported.md; 107–110/116: 6–9 known screenshot-only or timing failures (see corpus/htmlbubbles-ported/README.md)
TW_RENDERER=shadow corepack pnpm test:attacks   # the same suite on the Shadow DOM renderer (no iframe): must stay at 0 hits / 0 requests; also test:generated / test:common → docs/compat-report-<group>-shadow.md
corepack pnpm tsx scripts/feed-bench.ts --counts 50,200 --mode ops,host --cv --pool 8,32 --out docs/feed-bench-web.md   # iframe vs shadow renderer in a feed of N bubbles (needs pnpm dev); ops = renderer only, host = full WidgetHost per bubble, --pool = also with a warmed worker pool
corepack pnpm tsx scripts/feed-report.ts --passes 5 --count 60 --origin http://127.0.0.1:5173   # the feed's bench headless (6 renderer × pool cells × 5 passes × 60 bubbles, ~30 min; --combos iframe:on,blitz:off,…) → docs/feed-report-web.json + .md; needs pnpm dev and the two wasm builds
corepack pnpm test:showcase           # 21 interactive demos on the full web profile (corpus/showcase, README there; the web feed's content) → docs/compat-report-showcase.md; in the iOS/macOS demo: the showcase/<batch> sections, --autorun showcase, and the iOS chat feed's Widgets picker (showcase by default; --feed-set corpus=showcase from a shell, edge-to-edge bubbles)
corepack pnpm test:common             # 32 widgets on the common WebView/Blitz/litehtml profile (corpus/common, README there) → docs/compat-report-common.md; the same files run on every renderer: --autorun common [--blitz|--litehtml] in the iOS/macOS demo, --es autorun common on Android, CORPUS=common scripts/android-bench.sh <renderer> 20
corepack pnpm build:blitz-web         # Blitz renderer for the web dev app: cargo (wasm32, rustup) + wasm-bindgen-cli (version from apps/renderer-blitz/Cargo.lock) → apps/renderer-blitz/pkg-web (gitignored, served at /blitz/); then the Blitz toggle / ?blitz=1, apps/web/tests/unit/blitz-wasm.test.ts stops skipping
TW_RENDERER=blitz corepack pnpm test:generated   # generated corpus on the Blitz wasm renderer → docs/compat-report-generated-blitz.md + reports/generated-blitz (pixel diff informational, no clicks: a canvas)
corepack pnpm build:ios               # bundles → packages/ios/Sources/WidgetSandbox/WidgetJS, corpus → apps/ios/DemoApp/Corpus
corepack pnpm build:macos             # same outputs (the macOS demo references them)
corepack pnpm build:android           # bundles → packages/android/widget-sandbox/src/main/assets/WidgetJS, corpus → apps/android/demo/src/main/assets/Corpus
corepack pnpm build:desktop           # bundles → packages/desktop/WidgetJS, fixture → packages/desktop/tests/fixtures
cd packages/desktop && cmake -S . -B build -G Ninja && cmake --build build && ./build/twidget-tests && ./build/twidget-run --attacks   # C++ core: 45 tests; 98 attacks, 0 leaks into validated ops; also --corpus ../../corpus/compat
cd packages/desktop && cmake -S . -B build -G Ninja -DTW_BLITZ=ON && cmake --build build && ./build/twidget-tests && ./build/twidget-run --corpus ../../corpus/compat --blitz --dpr 2 --png-dir /tmp/compat   # + Blitz renderer (rustup cargo): 48 tests; --attacks --blitz counts renderer fetch attempts as leaks; single file: --blitz --png out.png [--click X,Y]
TWB_BACKEND=cg ./build/twidget-run --corpus ../../corpus/generated/with-profile --blitz --dpr 2 --png-dir /tmp/gen-cg   # Blitz laid out by Rust, painted by CoreGraphics/CoreText (apps/renderer-blitz/src/cg.rs, Apple only; twb_set_backend in the C ABI); three-way gallery: corepack pnpm tsx scripts/blitz-experiment/gallery-native.mts reports/blitz-experiment/native/generated vello=DIR:TABLE cg=DIR:TABLE litehtml=DIR:TABLE --strips 20; on the simulator: SIMCTL_CHILD_TWB_BACKEND=cg xcrun simctl launch booted org.telegram.widgetsandbox.TwidgetDemo --blitz (iOS emoji paint only on this backend); Android twin: apps/renderer-blitz/src/dl.rs records the scene as a display list, CanvasScene.kt replays it with Canvas.drawGlyphs — `--es renderer blitz_canvas` in the demo (Blitz·Canvas in the picker)
cd packages/desktop && cmake -S . -B build -G Ninja -DTW_BLITZ=ON -DTW_LITEHTML=ON && cmake --build build && ./build/twidget-tests && ./build/twidget-run --corpus ../../corpus/compat --litehtml --dpr 2 --png-dir /tmp/compat   # + litehtml renderer (macOS): 51 tests with both; the same flags as --blitz
corepack pnpm build:litehtml-web      # litehtml → wasm (brew install emscripten) → apps/renderer-litehtml/pkg-web; the dev UI's litehtml toggle / ?litehtml=1, TW_RENDERER=litehtml pnpm test:generated, apps/web/tests/unit/litehtml-wasm.test.ts
# dev UI "compare all" / ?compare=1: iframe, Blitz, litehtml and the reference side by side (one sandbox each, booted one after another)
corepack pnpm tsx packages/core/scripts/profile-build.ts   # after editing packages/core/profile.json: regenerates src/shared/profile-tables.ts + docs/profile-v1.md (--check: exit 1 when stale; profile.test.ts runs it); then pnpm build:ios|android|desktop
corepack pnpm tsx scripts/conformance/build.ts   # rebuild the conformance set (catalogue, inlined harness, Chromium refs, manifest) after editing packages/conformance/{auto,visual} or scripts/conformance
corepack pnpm tsx scripts/conformance/run-web.ts --origin http://127.0.0.1:5173 && node packages/conformance/tools/report.mjs --out packages/conformance/results reports/conformance/{iframe,shadow}.json   # the set on the web renderers (needs pnpm dev; blitz/litehtml too when their wasm is built) → packages/conformance/results/summary.md; the dev app lists it as the conformance/ group
corepack pnpm tsx scripts/profile/probes.ts && corepack pnpm tsx scripts/profile/corpus.ts   # the feature probes and corpus usage behind the historical draft docs/widget-profile-draft.md and widget profile 1 (docs/profile-v1.md): 53 probes through validator + Blitz + litehtml (desktop build with both), then feature usage over the four corpora → reports/profile/
cd packages/android && JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home ./gradlew :widget-sandbox:connectedDebugAndroidTest   # needs a booted emulator (AVD twidget-35)
corepack pnpm tsx packages/core/scripts/sandbox-node.ts corpus/compat/tabs.html --verbose
scripts/android-bench.sh webview|blitz|litehtml|none 20 [prewarm]   # renderer start-up/memory bench in the Android demo (needs installDebug + emulator); POOLS=1 = validator pool; python3 -I scripts/android-bench-print.py --table <names>; docs/android-renderer-bench.md
xcodebuild test -scheme WidgetSandbox-Package -destination 'platform=iOS Simulator,name=iPhone 17,OS=26.5'   # 221 tests (the test scheme SwiftPM generates); corpus/attacks also runs through the native node model (AttackCorpusNativeModelTests)
cd apps/ios/DemoApp && xcodegen generate && xcodebuild build -scheme TwidgetDemo -destination 'platform=iOS Simulator,name=iPhone 17,OS=26.5' CODE_SIGNING_ALLOWED=NO -derivedDataPath /tmp/twidget-sim && xcrun simctl install booted /tmp/twidget-sim/Build/Products/Debug-iphonesimulator/TwidgetDemo.app && xcrun simctl launch booted org.telegram.widgetsandbox.TwidgetDemo --blitz   # demo on the Blitz renderer (--litehtml: litehtml); the pre-build phases run apps/renderer-blitz/build-apple.sh (rustup cargo) and apps/renderer-litehtml/build-apple.sh (cmake + ninja)
cd apps/renderer-litehtml && cmake -S . -B build -G Ninja && cmake --build build && ./build/twidget-litehtml-tests && ./build/twidget-litehtml-run msgs.json out.png --width 360 --dpr 2   # litehtml core on the host: 6 tests; msgs.json from `pnpm tsx packages/core/scripts/sandbox-node.ts <widget> --ops-file msgs.json`
xcrun simctl launch booted org.telegram.widgetsandbox.TwidgetDemo --autorun feed --feed max --feed-count 40 --feed-scroll step   # chat feed (iOS only): scripted scroll, feed-report.json in the app container; `--feed baseline`, `--feed-set pools=false,…`; build with -configuration Release for numbers (docs/chat-feed-ios.md); the feed has the web feed's bar: renderer WebView | Blitz | litehtml switched live, pool on/off, hostile bubble, corpus, count, reset, ▶ scroll (`--feed-set renderer=blitz,hostile=cpu-raf-burn,corpus=showcase`)
xcodebuild test -scheme WidgetSandbox-Package -destination 'platform=macOS'   # 220 tests; the frame-pause test is iOS-only (xctest windows are never "visible" on macOS)
cd apps/macos/DemoApp && xcodegen generate && xcodebuild build -scheme TwidgetDemoMac -destination 'platform=macOS' -derivedDataPath /tmp/twidget-mac && /tmp/twidget-mac/Build/Products/Debug/TwidgetDemoMac.app/Contents/MacOS/TwidgetDemoMac --autorun attacks   # also generated|compat|pause; --features binaryOps,canvasWords runs the attacks through the Swift native node model (NativeValidator), which Playwright never exercises; reports in ~/Library/Application Support/org.telegram.widgetsandbox.TwidgetDemoMac
```

Pitfalls: Playwright suites must not run concurrently (shared canary on
127.0.0.1:9999); `reuseExistingServer` means a dev server already on 5173 (another
checkout) gets tested instead of yours — run with `TW_PORT=5174` from a worktree;
editing `packages/core/src` or `apps/web` during a Playwright run reloads Vite and breaks
the run; `tests/global-setup.ts` wipes `reports/` so a `-g` partial run yields a
partial report; macOS has no `timeout`.

## Status (Oct 2026)

Done: phases 0–6 (spike, shim, compat 22 widgets, attacks 82, iOS port with
device install, generated corpus 40/40 web + iOS), the Android port
(emulator: 45 instrumented tests, 82 attacks with 0 canary hits, compat and
generated corpora load; the same on a real phone), the runtime limits (`docs/decisions.md`, Runtime limits)
on all three platforms (CPU quota 300 ms/s with deferral, 256-timer cap,
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
integration guide `docs/integration.md`, the widget payload (mini-HTML; `.tgw` only as the
test-channel format) in `docs/tgw-format.md`.
2026-10-17: widget profile 1 frozen and enforced (`packages/core/profile.json`,
`docs/profile-v1.md`, `docs/decisions.md` "Frozen as widget profile 1"): the validator,
`NativeValidator`, the shim (`TelegramWidget.profile`, `isProfileAtLeast`, `CSS.supports`
and `@supports` from one evaluator) and the renderer (no keyboard or focus, no selection,
the client-styled `<button>`); no bundled libraries on any platform; `Intl.formatToParts`
in every glue copy; the compat widgets that need libraries or form controls moved to
`corpus/out-of-profile/`.
2026-10-17: SDK v1 (`docs/sdk-v1.md`): `TelegramWidget` v1 in the shim, the checks in one
policy with four copies and a fixture (`packages/core/src/shared/sdk-policy.ts`,
`SdkPolicy.swift`, `SdkPolicy.kt`, `sdk_policy.cpp`, `packages/core/tests/fixtures/sdk-policy.json`),
`WidgetTheme` / `onAction` / `onStorageCommit` / `onGestureClaim` on every host, Web Storage
per message, `sendMessage` gone; the corpora reworked for profile 1 and SDK v1.
Open: a reproducible build of `packages/quickjs-wasm/dist`, versions
and tags (VERSION.json), the renderer and engine pools on Android (the validator pool is there: `ValidatorPool`, `WidgetHost.Options.pools` / `sharedValidatorThread`; the Android renderer bench, `docs/android-renderer-bench.md`, puts QuickJS + validator at 6.3 MB per bubble against 1–4 MB for any renderer), litehtml on the C++ core and the web (wasm) for the full three-way matrix, device pentest of the
WebView blockers on both platforms.
