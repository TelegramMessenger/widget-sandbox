# widget-sandbox desktop core (C++)

The QuickJS sandbox for Telegram Desktop–class hosts (Qt/C++): the engine,
the trusted validator, the CPU budget and the WidgetHost glue as a **Qt-free
C++20 static library**, plus a console harness and a test suite. It is the
fourth copy of the host glue (web Worker, Swift, Kotlin, C++) over the same
three JS bundles; see `docs/decisions.md` (Desktop C++ core) for why the product
renderer is not here (tdesktop has no WebView inside the chat) and what the
product path on tdesktop looks like. The Blitz renderer prototype
(`-DTW_BLITZ=ON`, below) is dev tooling behind the same `Renderer` seam the
iOS/macOS and Android demos have.

```
packages/desktop/
  sandbox/include/twidget/   public headers
    sandbox_engine.h         SandboxEngine + SandboxEngineDelegate (= SandboxEngine.swift / .kt)
    trusted_validator.h      validator-bundle.js in a second, trusted QuickJS runtime
    widget_host.h            WidgetHost: engine + validator + Renderer, one widget (= WidgetHost.swift / .kt)
    renderer.h               Renderer seam + RendererFactory (= Renderer.swift / .kt)
    cpu_budget.h             CpuBudget (fixture: packages/core/tests/fixtures/cpu-budget.json)
    sdk_policy.h             SDK v1 checks: links, haptics + HapticGate, theme params, locale, storage, bridge calls
                             (= SdkPolicy.swift / .kt; fixture: packages/core/tests/fixtures/sdk-policy.json)
    js_thread.h              one dedicated thread per runtime, 8 MB stack, timers
    quickjs_context.h        thin wrapper over quickjs-ng via packages/ios/Sources/CQuickJS/shim
    resources.h              WidgetJS/ loader + CDN URL → bundled library
    intl.h                   IntlProvider; BasicIntl fallback (plug QLocale/ICU here)
    json.h                   small strict JSON (hostile input: bounded depth)
  renderer/                  BlitzRenderer (twidget/blitz_renderer.h): the Renderer over ../renderer-blitz's C ABI; -DTW_BLITZ=ON only
                             LitehtmlRenderer (twidget/litehtml_renderer.h): the same over ../renderer-litehtml's C ABI; -DTW_LITEHTML=ON, macOS only
  harness/                   twidget-run + RecordingHost (fake host with canned layout answers) + png.h
  tests/                     twidget-tests (XCTest suite ported) + fixtures/
  WidgetJS/                  bundles from `pnpm build:desktop` (committed)
```

quickjs-ng is not copied: CMake compiles `../ios/Sources/CQuickJS` (the same
sources Android builds). The library links only the C++ standard library,
pthreads and `m`; on Windows `bcrypt` (CSPRNG) and `_beginthreadex`.

## Build and run

```bash
corepack pnpm build:desktop                       # bundles → packages/desktop/WidgetJS, fixture → packages/desktop/tests/fixtures
cd packages/desktop && cmake -S . -B build -G Ninja && cmake --build build
./build/twidget-tests                             # 45 tests (engine, limits, validator, JSThread, CpuBudget and SDK fixtures, SDK v1, JSON)
./build/twidget-run ../corpus/compat/tabs.html --wait-ms 500 [--verbose] [--ops] [--probe 'document.title']
./build/twidget-run w.html --theme dark --theme-params '{"bg_color":"#212121"}' --locale ru --storage '{"local":{"n":"1"}}' \
    --events '{"type":"click","targetId":5,"detail":1,"gesture":1}'   # SDK v1: prints `action …` and `storage …`; "gesture" makes a trusted press
./build/twidget-run --corpus ../../corpus/compat     # loaded, alive, no uncaught errors; also generated/with-profile, generated/without-profile
./build/twidget-run --attacks                     # 98 files: nothing but data:/#fragment may reach the validated ops
```

Status (2026-10-17, macOS arm64): tests 45/45; compat 12/12 and
generated/with-profile 20/20 load without errors; attacks 98 with 0 URLs
leaking into validated ops and the four expected kills (`fallback-at-boot.html`,
three `mem-*`). Boot of the sandbox bundle takes
~20 ms in a release quickjs-ng from the precompiled `.qbc` (38–74 ms from
source; the diag says which one was used).

## What the harness checks and what it cannot

Without `--blitz` there is no renderer, so layout queries (`rect`, `box`,
`measureText`, …) get the same canned answers as `packages/core/scripts/sandbox-node.ts`.
That is enough to prove the engine, the limits and the validator; pixels and
the renderer-side defenses (CSP, request blocking) are checked on the platforms
that have a WebView.

`--attacks` parses every validated `RenderOp` and flags, as a **LEAK**, any
attribute, inline style, stylesheet or canvas argument through which a renderer
could reach the network: non-`data:` URLs in attributes, `url()`-family
functions whose argument is not `data:`/`#…`, and `@import`. Two attack files
produce a **note**, not a leak: an external URL *inside* a `data:image/svg+xml`
payload. SVG loaded as an image may not fetch external resources in any engine,
and the browser attack suite confirms zero canary hits for those files. A
native renderer that is not a browser engine must keep that property.

## Blitz renderer prototype (`-DTW_BLITZ=ON`)

The same Rust crate the mobile and macOS demos use (`../renderer-blitz`:
blitz-dom + blitz-paint + vello_cpu, validated `RenderOp`s in, layout answers
and events out, premultiplied RGBA8 on request) linked into the C++ core
through its C ABI (`include/twidget_blitz.h`). CMake runs rustup's `cargo`
for the host triple (`TW_RUST_TARGET`, autodetected) and links the static
library; macOS and Linux only for now (the Windows link line is not wired).
Dev tooling for `docs/blitz-experiment.md`, not a product path: the sandbox
and the validator are untouched, the Rust side only ever sees validated ops
and `data:` images.

```bash
cd packages/desktop && cmake -S . -B build -G Ninja -DTW_BLITZ=ON && cmake --build build   # first cargo build: 1–2 min, ≈ 39 MB static library
./build/twidget-tests                                                            # 48 tests: + layout answers from the document, hit-tested clicks, frozen fallback
./build/twidget-run ../corpus/compat/tabs.html --blitz --dpr 2 --png /tmp/tabs.png [--click X,Y]...
./build/twidget-run --corpus ../../corpus/compat --blitz --dpr 2 --png-dir /tmp/compat   # + height / paint / loaded columns, one PNG per widget
./build/twidget-run --attacks --blitz                                            # a fetch attempt from the Rust document counts as a leak too
```

## litehtml renderer prototype (`-DTW_LITEHTML=ON`, macOS)

The C++ core in `../renderer-litehtml` (vendored litehtml, the validated DOM
kept as a shadow tree and re-parsed per batch, fonts and pixels from
CoreText/CoreGraphics — hence macOS only until a QPainter backend exists)
behind the same owner contract as `BlitzRenderer`: `twidget/litehtml_renderer.h`
is `blitz_renderer.h` with `twl_*` for `twb_*`. `docs/litehtml-renderer.md`.

```bash
cd packages/desktop && cmake -S . -B build -G Ninja -DTW_BLITZ=ON -DTW_LITEHTML=ON && cmake --build build
./build/twidget-tests                                                           # 51 tests: + 3 litehtml twins of the Blitz ones
./build/twidget-run ../corpus/compat/tabs.html --litehtml --dpr 2 --png /tmp/tabs.png [--click X,Y]...
./build/twidget-run --corpus ../../corpus/compat --litehtml --dpr 2 --png-dir /tmp/compat   # 12/12, paint 0.3–6 ms at 800 px
./build/twidget-run --attacks --litehtml                                         # 98 attacks, 0 leaks, 0 fetch attempts
```

What it is on this platform:

- `twidget/renderer.h` is the seam (`Renderer` + `RendererFactory`), the C++
  twin of `Renderer.swift` / `Renderer.kt`. `WidgetHost` takes a factory; the
  product renderer on tdesktop (its Web App window) would be another
  implementation of it.
- `twidget/blitz_renderer.h` has no view: the owner takes rasters
  (`paint()`, or `onPaint` from the renderer's own 60 Hz clock, which also
  delivers `requestAnimationFrame` ticks) and feeds pointers in CSS px
  (`touch()`, `click()`) — on tdesktop a `QWidget::paintEvent` and mouse
  events, in the harness `--click` and `--png`.
- Threading: a Qt-free library has no main thread, so `WidgetHost` owns a
  host thread that stands in for it (every renderer call, every diagnostics
  update); the Rust document is used from the host thread, the clock thread
  and the owner's thread one at a time, behind one mutex in `BlitzRenderer`.
  A Qt host replaces the host thread with the UI thread one for one.
- Numbers (macOS arm64, 2×, RelWithDebInfo): compat 21/21 and generated 40/40
  load and paint, 1–9 ms per paint at 800 px wide; attacks 89 with 0 leaks and
  0 fetch attempts from the renderer. Gaps are the crate's, the same on every
  platform: empty `<canvas>`, CSS not reaching inline SVG, form values and
  sliders not drawn, no 3D transforms.

## Embedding (tdesktop or any Qt app)

- One `WidgetHost` per widget with a `RendererFactory` for your renderer, or,
  one level down, one `SandboxEngine` with your own `SandboxEngineDelegate`.
  Engine callbacks arrive on the engine's thread; hop to the UI thread
  yourself (`QMetaObject::invokeMethod` / `crl::on_main`). `onQuery` is
  synchronous and blocks the engine thread until you answer — never answer it
  by waiting on the UI thread from the UI thread. `WidgetHost` does all of
  this on its host thread; `destroy()` it from the owner's thread.
- Every `onOps` batch goes through `TrustedValidator::processOps` before it is
  sent to a renderer. Nothing else from the engine is trusted.
- Pass an `IntlProvider` built on `QLocale` to the engine; `BasicIntl` is only
  a fallback.
- Limits live in `SandboxConfig` / `WidgetHostOptions` (same fields and
  defaults as `protocol.ts`). `setVisible(false)` when the bubble leaves the
  screen.
- SDK v1 (`docs/sdk-v1.md` §4.3, §5): build the host with a `WidgetTheme`
  (scheme, the 15 `themeParams` colours, `WidgetButtonTokens`) and call
  `setTheme(WidgetTheme)` on every chat theme change; pass `locale` and the
  stored `storage` (`WidgetStorage { local, session }`) in `WidgetHostOptions`.
  `onAction` gets checked actions only (`OpenLink` / `OpenTelegramLink` with
  the normalised URL, at most one per pointer press; `Haptic`, rate-limited;
  `Ready`); `onStorageCommit(area, values)` the full new content of an area at
  the end of each task that changed it, within the 16 KB quota (persist
  `local`, keep `session` in memory); `onGestureClaim(x, y)` on every primary
  press. Strings are UTF-8; a widget's lone surrogate arrives in its 3-byte
  WTF-8 form. The string-theme constructor, `setTheme(std::string)` and
  `onBridgeCall` are deprecated forwards; the host → widget `bridgeEvent` is gone.
