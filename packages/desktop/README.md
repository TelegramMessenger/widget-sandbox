# widget-sandbox desktop core (C++)

The QuickJS sandbox for Telegram Desktop–class hosts (Qt/C++): the engine,
the trusted validator, the CPU budget and the WidgetHost glue as a **Qt-free
C++20 static library**, plus a console harness and a test suite. It is the
fourth copy of the host glue (web Worker, Swift, Kotlin, C++) over the same
three JS bundles; see `docs/decisions.md` (Desktop C++ core) for why the product
renderer is not here (tdesktop has no WebView inside the chat) and what the
product path on tdesktop looks like. The Blitz renderer prototype
(`-DTW_BLITZ=ON`, below) is dev tooling behind the same `Renderer` seam the
iOS/macOS and Android demos have; it and the litehtml one (`-DTW_LITEHTML=ON`) are
frozen since 2026-10-30 (`docs/decisions.md`, "The renderer prototypes are frozen"): no check builds them.

```
packages/desktop/
  sandbox/include/twidget/   public headers
    sandbox_engine.h         SandboxEngine + SandboxEngineDelegate (= SandboxEngine.swift / .kt); make + the channel factory
    remote_engine.h          the engine in another process: SandboxRemoteChannel / SandboxRemoteSink, SandboxRemoteServer,
                             SandboxLoopbackChannel, SandboxRemoteCoding (= RemoteEngine.swift; Kotlin has none)
    remote_stream.h          the remote engine over one byte stream per engine process: SandboxRemoteStream (the OS
                             boundary a host supplies), SandboxRemoteWire, SandboxRemoteHostEnd, SandboxRemoteEngineEnd
    remote_hosted.h          the whole widget in the engine process (desktop core only): SandboxHostedChannel,
                             SandboxHostedPlatform, HostedRendererFactory, SandboxHostedLoopbackChannel
    trusted_validator.h      validator-bundle.js in a second, trusted QuickJS runtime
    widget_host.h            WidgetHost: engine + validator + Renderer, one widget (= WidgetHost.swift / .kt)
    renderer.h               Renderer seam + RendererFactory (= Renderer.swift / .kt)
    cpu_budget.h             CpuBudget (fixture: packages/core/tests/fixtures/cpu-budget.json)
    sdk_policy.h             SDK v1 checks: links, haptics + HapticGate, theme params, locale, storage, bridge calls;
                             the widget's state, viewer, chat type, request answers + RequestGate, previews
                             (= SdkPolicy.swift / .kt; fixture: packages/core/tests/fixtures/sdk-policy.json)
    gesture_policy.h         the widget or the bubble: claimOf + GestureArbiter for the client's recognizers
                             (= GesturePolicy.swift / .kt; fixture: packages/core/tests/fixtures/gesture-policy.json)
    js_thread.h              one dedicated thread per runtime, 8 MB stack, timers
    quickjs_context.h        thin wrapper over quickjs-ng via packages/ios/Sources/CQuickJS/shim
    resources.h              WidgetJS/ loader + CDN URL → bundled library
    intl.h                   IntlProvider; BasicIntl fallback (plug QLocale/ICU here)
    json.h                   small strict JSON (hostile input: bounded depth)
  sandbox/src/               the sources; remote_internal.h is what the remote transports share without making it public
                             (twidget::detail: the loopback's sink, the remote Intl, the hosted runtime, the serving loop)
  renderer/                  BlitzRenderer (twidget/blitz_renderer.h): the Renderer over ../renderer-blitz's C ABI; -DTW_BLITZ=ON only
                             LitehtmlRenderer (twidget/litehtml_renderer.h): the same over ../renderer-litehtml's C ABI; -DTW_LITEHTML=ON, macOS only
  harness/                   twidget-run + RecordingHost (fake host with canned layout answers) + png.h
                             + stream_peers.h (test byte streams and the TW_ENGINE_STREAM switch, not the library's)
                             + hosted_peers.h (the TW_ENGINE_HOSTED switch and the canned renderer, not the library's)
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
./build/twidget-tests                             # 204 tests (engine, limits, validator, JSThread, CpuBudget, SDK and gesture fixtures, SDK v1, JSON, the remote engine, its byte stream and the hosted mode); `./build/twidget-tests remote_` runs the remote engine's 98, `remote_stream_` the byte stream's 35 and `hosted_` the hosted mode's 19 of them
./build/twidget-run ../corpus/compat/tabs.html --wait-ms 500 [--verbose] [--ops] [--probe 'document.title']
./build/twidget-run w.html --theme dark --theme-params '{"bg_color":"#212121"}' --locale ru --storage '{"local":{"n":"1"}}' \
    --events '{"type":"click","targetId":5,"detail":1,"gesture":1}'   # SDK v1: prints `action …`, `storage …` and `resize anchor …`; "gesture" makes a trusted press (and the haptic gate's press, or its release for anything but pointerdown / mousedown)
./build/twidget-run w.html --message group --viewer 42,Ann,Lee,ann_lee,premium --chat-data c1 --public-data p1 --answer '{"result":"ok"}' \
    --events '{"type":"click","targetId":5,"detail":1,"gesture":1}'   # the bot's requests: prints `request …` and `copyText …`; --message private|group|via-bot|no-queries|live-copy|copy|preview, or --copy / --can-query / --background-query / --chat-type; --answer-error TIMEOUT, --set-data JSON
./build/twidget-run --corpus ../../corpus/compat     # loaded, alive, no uncaught errors; also generated/with-profile, generated/without-profile
./build/twidget-run --attacks                     # 133 files: nothing but data:/#fragment may reach the validated ops
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build-debug
TW_ENGINE_LOOPBACK=1 ./build-debug/twidget-tests   # the same 204 with every engine SandboxEngine::make builds behind the in-process loopback (the last lines count them)
TW_ENGINE_LOOPBACK=1 ./build-debug/twidget-run --attacks   # 133 attacks, 0 leaks, "133 through a client-mode engine"; NDEBUG (the default RelWithDebInfo) compiles the switch out
TW_ENGINE_STREAM=1 ./build-debug/twidget-tests     # the same 204 with every engine SandboxEngine::make builds over the byte stream (below), in any build type; fails unless every fixture engine went over it
TW_ENGINE_STREAM=1 ./build-debug/twidget-run --attacks     # 133 attacks, 0 leaks, "133 of them over the stream"; a stream that ends before the run's own stop fails the run
TW_ENGINE_HOSTED=loopback ./build/twidget-tests    # the same 204 with every fixture WidgetHost hosted (below): its engine, validator and glue behind a SandboxHostedLoopbackChannel, its renderer here; any build type; fails unless every fixture widget went hosted
TW_ENGINE_HOSTED=stream ./build/twidget-tests      # the same over a hosting stream per widget; also fails when a kind that stays in the engine process crossed one
TW_ENGINE_HOSTED=stream ./build/twidget-run --attacks      # 133 attacks, 0 leaks in the validated ops the run asked for, "133 hosted over the stream"; =loopback too, and --corpus ../../corpus/compat with either
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
./build/twidget-tests                                                            # 72 tests: + layout answers from the document, hit-tested clicks, frozen fallback
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
./build/twidget-tests                                                           # 75 tests: + 3 litehtml twins of the Blitz ones
./build/twidget-run ../corpus/compat/tabs.html --litehtml --dpr 2 --png /tmp/tabs.png [--click X,Y]...
./build/twidget-run --corpus ../../corpus/compat --litehtml --dpr 2 --png-dir /tmp/compat   # 12/12, paint 0.3–6 ms at 800 px
./build/twidget-run --attacks --litehtml                                         # 104 attacks, 0 leaks, 0 fetch attempts
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
  by waiting on the UI thread from the UI thread. `probe` (tests, the harness,
  a barrier behind earlier inputs) is synchronous too: call it from the owner's
  thread, never from the thread that answers `onQuery` (a probe's code may ask
  a layout query), and on a client-mode engine never from its dispatch thread
  (`thread()`), where it answers `ERROR no runtime`. `WidgetHost` does all of
  this on its host thread; `destroy()` it from the owner's thread.
- Every `onOps` batch goes through `TrustedValidator::processOps` before it is
  sent to a renderer. Nothing else from the engine is trusted.
- Pass an `IntlProvider` built on `QLocale` to the engine; `BasicIntl` is only
  a fallback.
- The engine in another process (opt-in, `twidget/remote_engine.h`;
  `docs/changes.md`, "the desktop engine in another process"): `WidgetHost`
  builds its engine with `SandboxEngine::make`, a client-mode engine over the
  channel of the factory you set, else (a build without `NDEBUG` and
  `TW_ENGINE_LOOPBACK=1`) over the in-process loopback, else one with its own
  runtime. A transport is a `SandboxRemoteChannel` whose calls reach a
  `SandboxRemoteServer` in the other process, and whose `SandboxRemoteSink`
  (given in `attach`) gets the server's calls back in order;
  `SandboxLoopbackChannel` is both halves in one process, the model for one,
  and `SandboxRemoteHostEnd` the one over a byte stream (next section):

  ```cpp
  #include "twidget/remote_engine.h"

  // Before the first widget: every engine `make` builds from now on is a client of the channel this returns.
  twidget::SandboxEngine::setRemoteChannelFactory([](std::shared_ptr<twidget::SandboxResources> resources) {
      return std::make_shared<twidget::SandboxLoopbackChannel>(std::move(resources)); // your transport's channel
  });
  twidget::SandboxEngine::setRemoteChannelFactory(nullptr); // engines in process again
  ```

  The factory runs on the thread that calls `make`; a null channel gives a
  widget that dies ("the widget engine process ended"), never one in process
  (`WidgetHost` hears that death, as everything else, after `start()`).
  The host's side stays here: the delegate hears every call on the client's
  dispatch thread (`thread()`), the `IntlProvider` you pass answers the
  engine's Intl requests (`remoteIntl`), and the client decides every
  privileged call again on your taps, visibility, message flags and haptic
  gate with the SDK checks. Channel calls come from the dispatch thread in your
  order and never wait for the other side, `probe` aside; the sink's
  `remoteQuery` and `remoteIntl` are synchronous and never made from inside a
  channel call. The client sizes every sink call before it queues it (1 MiB
  per JSON message, 32 MiB per ops batch) and counts at most
  `SandboxEngine::kRemoteQueuedBytes` (64 MiB) and `kRemoteQueuedCalls`
  (16384 calls) of them from arrival until each, and the work its delegate
  queued with it, has run or been dropped: `WidgetHost`'s host-thread work
  keeps its message counted (`SandboxEngine::remoteHold`) and counts as work
  of its own what it keeps beyond it (a validated result larger than its
  input, each validator diagnostic, a bridge call's copy next to its action:
  its checked arguments re-serialized, while the call's own place still counts
  what came); a delegate of your own that queues work holds `remoteHold()` in
  it the same way. A query or an Intl request (`remoteQuery`, `remoteIntl`)
  that the dispatch thread runs gives its own place back as that delivery
  ends, before its answer is returned, so a caller that has the answer no
  longer finds that call counted, unless work its delegate queued with it
  still shares the place (`WidgetHost`'s host-thread work, until it has run or
  been dropped, also after the query timed out). The message past either bound
  kills the widget once ("the widget engine process sent more than the host
  keeps queued"), and the sink then takes nothing more and never waits. The
  bounds count payload bytes and queued calls up to the moment that work runs,
  not allocator memory (each call's closure and containers come on top) and
  not what the host keeps afterwards. Of that, `WidgetHost` keeps at most
  `WidgetHost::kRendererBacklogBytes` (64 MiB, each message serialized with
  its wrapper) and `kRendererBacklogMessages` (16384 messages) of the engine's
  renderer messages until the renderer's `ready`, its own theme, safe area,
  policy and `frozen` not counted: the message past either stops the widget
  once ("the widget engine process sent more than the host keeps before its
  renderer is ready", diagnostic `backlog-limit`), what was kept is dropped,
  and nothing the engine sends after it takes effect; `diagnostics().diags`
  keeps at most `WidgetHost::kMaxDiagKinds` (64) kinds, a diagnostic of a new
  kind past them counted under `diag-kind-dropped`; `diagnostics().bridge`,
  like the deprecated `onBridgeCall`, gets each call's checked arguments
  re-serialized compact, the value that was sent, at most 276 bytes of method
  and arguments a call and 276 000 for the first 1000 (`docs/changes.md` has
  the details; the rest of what the host keeps is as before).
  The engine's destructor, on the owner's thread, closes and
  releases the channel; a transport may keep the sink, whose late calls
  change nothing.
- Limits live in `SandboxConfig` / `WidgetHostOptions` (same fields and
  defaults as `protocol.ts`: 8 MB of QuickJS heap, 64 timers, 300 ms of JS and
  100 ms of main-thread work per second; `docs/decisions.md`, "Strict limits
  from the start"). The main-thread window charges a task's layout queries and
  25 µs per op and 2 µs per canvas command it sent; past it, as past the JS
  quota, timers and frames wait. Under `strict` a counted cap of the validator
  (32 KB per text value, 48 KB of document text, 8 KB per attribute, 10 000 ops
  per batch, 10 000 canvas commands per send, 400 CSS rules, 32 `@keyframes`)
  stops the widget: `diagnostics().dead` carries the reason
  (`TrustedValidator::stopped()`, `SandboxEngine::stop`), as for memory or CPU.
  `setVisible(false)` when the bubble leaves the screen.
- SDK v1 (`docs/sdk-v1.md` §4.3, §5): build the host with a `WidgetTheme`
  (scheme, this bubble's 15 `themeParams` colours, `WidgetButtonTokens`,
  `WidgetTextTokens` with the chat's text size) and call
  `setTheme(WidgetTheme)` on every chat theme change; pass `locale` and the
  stored `storage` (`WidgetStorage { local, session }`) in `WidgetHostOptions`.
  `onAction` gets checked actions only (`OpenLink` / `OpenTelegramLink` with
  the normalised URL, at most one per pointer press; `Haptic`, rate-limited,
  and since v0.2.2 free only while a press is down and 5 s after it is lifted,
  else one 1 s series per touch or per 10 s: the presses are the renderer's
  `gesture` and `release` messages, which WidgetHost passes to the engine's
  `pressBegan` / `pressEnded`; a renderer that sends neither gets the no-touch
  rule; `Ready`); `onStorageCommit(area, values)` the full new content of an area at
  the end of each task that changed it, within the 4 KB quota (persist
  `local`, keep `session` in memory); `onGesture(WidgetGestureClaim)` on every
  primary press: what of it the widget claims with `touch-action` (`x`:
  horizontal drags, `hold`; taps and double taps are always the widget's, and
  since v0.1.3 a vertical drag is always the chat's: `y` is always false and
  the arbiter scrolls the list over any widget). Feed the claim
  to a `GestureArbiter` (`gesture_policy.h`) in your recognizers over the
  widget and act on its verdicts: on `cancel` end the press for the widget,
  then do the bubble's `action` (message menu, swipe-to-reply or back, list
  scroll). `diagnostics().resizeAnchor` (since v0.1.3, `"top"` until the
  widget calls `TelegramWidget.setResizeAnchor`) is the edge of the bubble that
  stays put on screen when `height` changes: for `"bottom"` the bubble grows
  upward and the content above it moves (in a top-down list, add the height
  delta to the scroll offset while the bubble is on screen, and pin the widget
  to the bubble's bottom during the transition). A change reaches `onChange`
  before the height it anchors; it is not an action. With no caption below the
  widget (none, or one shown above it) the client draws the message time over the widget's bottom corner and passes
  `WidgetHostOptions::contentSafeArea` (`bottom`, `cornerRight` or
  `cornerLeft`, CSS px; 0 by default) at creation and `setContentSafeArea` when
  the bubble's layout changes (`docs/sdk-v1.md` §2.17, §5.12); the renderer gets
  `{type:'safeArea'}` right after the theme.
  Strings are UTF-8; a widget's lone surrogate arrives in its 3-byte WTF-8
  form. The string-theme constructor, `setTheme(std::string)`, `onBridgeCall`
  and `onGestureClaim(x, y)` are deprecated forwards; the host → widget
  `bridgeEvent` is gone.
- The widget's state and requests (`docs/sdk-v1.md` §2.13–§2.16, §4.4): pass the
  widget's state, which only the bot writes, in `WidgetHostOptions::data`
  (`WidgetData { chatData, publicData }`: the chat state and the shared state,
  each 1–1024 bytes of text without NUL, none = `std::nullopt`) and the
  message's flags as the server sends them in `context` (`WidgetContext {
  isCopy, canQuery, backgroundQuery, chatType, viewer }`; all false by default: no requests; a
  copy gets no chat type; the viewer, `WidgetViewer { id, firstName, lastName,
  username, isPremium }`, reaches the widget only with `canQuery`, as Mini Apps'
  `WebAppUser`, a field that breaks its rule left out with `data-rejected`); call
  `setData(WidgetData)` with the full state on every edit from the server (the
  widget gets `chatDataChanged`, then `publicDataChanged`, for each field that
  changed; a copy keeps no chat state, its shared state can change).
  `diagnostics().watchesPublicData` (false until the widget calls
  `TelegramWidget.watchPublicData()`) says whether to poll a shared widget's
  shared state (`public_data`) while it is on screen; `onWatchPublicData(on)` gets each
  change (not an action, no gesture). A widget
  in an inline-results list before it is sent gets `context.preview`: its state
  as usual but no input (the host drops the renderer's events and gesture
  claims; a tap is the client's), no requests or bridge calls but `ready` and
  `setResizeAnchor` (`preview-blocked`), no chat type, viewer or storage
  preload, and no `onStorageCommit`.
  `onRequest(request, respond)` gets a checked request (`data` 1–64 bytes; only
  with `canQuery`; one press unlocks one privileged call, without a press only
  with `backgroundQuery`, while visible: one when the widget appears on screen,
  then one per 5 s; one in flight): send it like a callback button's press and
  call `respond` once, from any thread, with a `WidgetRequestOutcome` (the
  server's answer, `result`: text of at most 1024 bytes without NUL; or a
  `Timeout` / `RateLimited` / `Failed` failure); later calls and calls after
  `destroy()` are ignored. Without `onRequest` when a request is delivered, the
  widget gets `NOT_ALLOWED`. `onCopyText(text)` (1–256 UTF-16 code units, one
  per press): copy it and show a toast. Neither is an action; both are read on
  the host thread. On a bare `SandboxEngine` the delegate gets
  `onBridgeRequest` (completing a `WidgetRequestResponder`, or
  `respond.unsupported()`) and `onBridgeCopyText`.

## The engine process over a byte stream (`twidget/remote_stream.h`)

The remote engine's transport, opt-in like the rest of it: one duplex byte
stream per engine process, frames tagged with a runtime id, the string
protocol (the `__host` JSON, `SandboxRemoteCoding`'s forms) as their payload.
It is the byte-level twin of `SandboxLoopbackChannel`, its two ends sharing
nothing but the stream. Standard C++ only, and nothing here is an OS stream, a
process launcher or a sandbox profile: the host supplies the stream (a POSIX
socket, a Windows pipe), the process and the process's lockdown. No codec is
public either: the frame layout below is documented so that a peer can be
checked against it, and both ends share the library's private one. Nothing
uses it until a host sets the factory; tdesktop sets one on macOS, outside its
Mac App Store build and its builds against packaged libraries (an external Qt
without its startup patches), and on Linux x86_64 and aarch64 in every build,
and its engine process locks itself down in `lockdown`.

- `SandboxRemoteHostEnd::start(stream, resources, build, events, limits)`, in
  the host's process, opens the stream and hands out a `SandboxRemoteChannel`
  per runtime (`channel()`, from any thread) for
  `SandboxEngine::setRemoteChannelFactory`. Every runtime on a stream runs the
  bundles of its opening, whatever resources the factory is given.
- `SandboxRemoteEngineEnd::serve(stream, build, lockdown)`, in the engine
  process, needs no Qt and reads no file, so it may run before Qt starts: it
  checks the opening, builds the runtimes' resources from it, keeps a
  `SandboxRemoteServer` per runtime id and returns when the stream ends.
  `lockdown`, when set, is called once on `serve`'s thread after the opening
  was accepted and its resources built, before `Ready` and before any runtime
  exists: the process holds the bundles and no widget code has run, the place
  to apply an OS sandbox from inside. "" goes on serving; any other text
  refuses the stream with it (the host's end ends with "the engine process
  refused the stream: " and that text, and every widget on the stream dies
  with "the widget engine process ended" without loading), and an exception
  refuses it with "the engine process could not lock itself down". A refused
  opening and a stream that ends before one never call it.
- `SandboxRemoteHostEnd::refusing(reason)` is the channel of a host that has
  no engine process: `attach` reports `remoteDied(reason)`, so a `WidgetHost`
  dies with that reason after `start()`, and the widget runs nowhere, not in
  process either; its `probe` answers `ERROR no runtime`.

```cpp
#include "twidget/remote_stream.h"

// The host: `stream` is your SandboxRemoteStream to the engine process, `resources` the process-wide bundles.
std::shared_ptr<twidget::SandboxRemoteHostEnd> end = twidget::SandboxRemoteHostEnd::start(
    stream, *resources, "build-1",
    {[](size_t open) { postToOwner(open); }, [](const std::string& reason) { postToOwner(reason); }},
    {}); // the default Limits: a probe waits 10 s, one write may block 10 s, 64 MiB of frames queued
twidget::SandboxEngine::setRemoteChannelFactory([end](std::shared_ptr<twidget::SandboxResources>) {
    return end->channel();
});
// Without an engine process: each widget dies with this reason after start(), none runs in process.
twidget::SandboxEngine::setRemoteChannelFactory([](std::shared_ptr<twidget::SandboxResources>) {
    return twidget::SandboxRemoteHostEnd::refusing("no widget engine process");
});
// On the owner's thread, never inside the events: no new engine on it, then the stream ends.
twidget::SandboxEngine::setRemoteChannelFactory(nullptr);
end->stop();
```

```cpp
// The engine process: returns why the stream ended ("" at its end on a frame's boundary) once every runtime and thread it started is gone.
// `std::string lockDown()` is yours: "" once the process gave up what a widget must not reach, else why it could not, which refuses the stream.
int serveEngines(twidget::SandboxRemoteStream& stream) {
    return twidget::SandboxRemoteEngineEnd::serve(stream, "build-1", lockDown).empty() ? 0 : 1;
}
```

### The stream a host supplies

`SandboxRemoteStream` is the whole OS boundary, three calls:

- `read(out, size)` blocks until at least one byte came and returns how many,
  a partial read as `recv` and `ReadFile` give it; 0 at the end of the stream,
  on an error and after `shutdown`.
- `write(bytes)` blocks until all of it is written: true, or false once the
  stream broke or was shut down.
- `shutdown()`, from any thread, also while a `read` or a `write` blocks,
  idempotent and without waiting: the blocked calls return and every later one
  fails. This holds whatever the peer did, also after it half-closed: on macOS
  one `shutdown(fd, SHUT_RDWR)` on a socket whose peer already shut its writes
  fails with `ENOTCONN` without shutting the write side, and a thread blocked
  in `send` stays blocked, so a POSIX adapter shuts `SHUT_WR`, then `SHUT_RD`.
  Never `close` a descriptor that another call may still be in (its number can
  be reused): close it once both returned.

One reader and one writer at a time; the owner destroys the stream only after
both returned. The host's end holds the stream until `stop` has joined its
threads, then lets it go. The test peers are in `harness/stream_peers.h`, test
and harness code, not the library's: an in-memory pair, and on POSIX a socket
pair whose writes really block and which shuts each direction on its own.

### Frames

A frame is a `u32` size of the bytes after it (at least 5), a `u32` runtime
id (0 is the connection), a `u8` kind, then the kind's fields in order and
nothing after them, every integer little-endian. A `str` is a `u32` length and
that many raw bytes (kept as they are, NUL and WTF-8 included), an `i64` 8
bytes of two's complement, an `f64` the 8 bytes of its IEEE-754 pattern, a
`bool` one byte of 0 or 1. The host's kinds count from 1 and the engine's from
64, a hosting stream's own (the hosted mode, below) from 32 and 96, so a kind
of the other direction is an unknown one (`SandboxRemoteWire::Kind`). Each
channel call has its kind and so does each sink call but `remoteDisconnected`:
a lost stream is told to the clients by the host's end itself.

| # | Host to engine | Runtime | Fields | Channel call |
| --- | --- | --- | --- | --- |
| 1 | `Opening` | 0 | i64 version, str build, str sandboxBundle, str sandboxBytecode, str validatorStamp, str rendererStamp | `SandboxRemoteHostEnd::start` |
| 2 | `Open` | id | none | `attach` |
| 3 | `Start` | id | str html, str config (`encodeConfig`) | `start` |
| 4 | `Event` | id | str json | `event` |
| 5 | `Activation` | id | str json, i64 gesture (within `int`) | `activationEvent` |
| 6 | `AnimationFrame` | id | f64 time | `frame` |
| 7 | `Viewport` | id | f64 innerWidth, f64 innerHeight, f64 dpr | `viewport` |
| 8 | `Resize` | id | str entriesJson | `resize` |
| 9 | `Theme` | id | str colorScheme, str params (`encodeThemeParams`) | `theme` |
| 10 | `SafeArea` | id | str area (`encodeSafeArea`) | `contentSafeArea` |
| 11 | `Data` | id | str data (`encodeData`) | `setData` |
| 12 | `Visible` | id | bool visible | `setVisible` |
| 13 | `PressBegan` | id | none | `pressBegan` |
| 14 | `PressEnded` | id | none | `pressEnded` |
| 15 | `ChargeValidator` | id | f64 ms | `chargeValidator` |
| 16 | `Stop` | id | str reason | `stop` |
| 17 | `Answer` | id | i64 id, str outcome (`encodeOutcome`) | `answer` |
| 18 | `Probe` | id | i64 seq (from 1), str code | `probe` |
| 19 | `QueryAnswer` | id | str json, bool hasBinary, str binary ("" unless hasBinary) | what the sink's `remoteQuery` returned |
| 20 | `IntlAnswer` | id | str text | what the sink's `remoteIntl` returned |
| 21 | `Close` | id | none | `close` |
| 32 | `HostingOpening` | 0 | i64 version, str build, str sandboxBundle, str sandboxBytecode, str validatorBundle, str validatorBytecode, str rendererStamp | `SandboxRemoteHostEnd::startHosting` |
| 33 | `HostedStart` | id | str html, str config (`encodeConfig`), str hosted (`encodeHostedStart`) | `startHosted` |
| 34 | `HostedTheme` | id | str theme (`encodeTheme`) | `hostedTheme` |
| 35 | `Platform` | id | str message | `platform` |
| 36 | `PlatformAnswer` | id | str answer | what the sink's `remotePlatformRequest` returned |

| # | Engine to host | Runtime | Fields | Sink call |
| --- | --- | --- | --- | --- |
| 64 | `Ready` | 0 | i64 version | the opening served |
| 65 | `Refused` | 0 | str reason | the opening refused |
| 66 | `TaskStarted` | id | none | `remoteTaskStarted` |
| 67 | `TaskCompleted` | id | none | `remoteTaskCompleted` |
| 68 | `Ops` | id | str json | `remoteOps` |
| 69 | `Query` | id | str json | `remoteQuery`, answered by `QueryAnswer` |
| 70 | `Intl` | id | str requestJson | `remoteIntl`, answered by `IntlAnswer` |
| 71 | `Listen` | id | str json | `remoteListen` |
| 72 | `PreventDefault` | id | str json | `remotePreventDefault` |
| 73 | `Anim` | id | str json | `remoteAnim` |
| 74 | `RequestsFrame` | id | none | `remoteRequestsFrame` |
| 75 | `Log` | id | str level, str text | `remoteLog` |
| 76 | `Diag` | id | str kind, str detail | `remoteDiag` |
| 77 | `Bridge` | id | str method, str argsJson | `remoteBridge` |
| 78 | `CopyText` | id | str text | `remoteCopyText` |
| 79 | `Request` | id | i64 id, str data, bool wallet | `remoteRequest` |
| 80 | `Wallet` | id | i64 id, str payload (`encodeWallet`) | `remoteWallet` |
| 81 | `Storage` | id | str commitJson | `remoteStorage` |
| 82 | `ResizeAnchor` | id | str edge | `remoteResizeAnchor` |
| 83 | `WatchesPublicData` | id | bool on | `remoteWatchesPublicData` |
| 84 | `Loaded` | id | none | `remoteLoaded` |
| 85 | `Stats` | id | 11 f64 in `encodeStats`' order | `remoteStats` |
| 86 | `Died` | id | str reason | `remoteDied` |
| 87 | `ProbeAnswer` | id | i64 seq (from 1), str text | the answer to a `Probe` |
| 96 | `PlatformMessage` | id | str message | `remotePlatformMessage` |
| 97 | `PlatformRequest` | id | str message | `remotePlatformRequest`, answered by `PlatformAnswer` |
| 98 | `ValidatedOps` | id | str json | `remoteValidatedOps` |
| 99 | `RendererReady` | id | none | `remoteRendererReady` |
| 100 | `Rendered` | id | none | `remoteRendered` |
| 101 | `Nodes` | id | i64 nodes | `remoteNodes` |

Kinds 32 to 36 and 96 to 101 belong to a hosting stream, one `startHosting`
opened with `HostingOpening`: the opening decides what every `Open` of the
stream is, an engine after `Opening`, a hosted widget after `HostingOpening`.

Both ends read exactly. A frame is malformed when its size is below 5 or past
its bound, the stream ends inside it, its kind is unknown or the other
direction's, a connection kind comes on a runtime or a runtime kind on the
connection, a field runs past the frame or bytes are left after the last, a
`bool` is neither 0 nor 1, an `Activation`'s gesture is outside `int`, a
`Probe`'s or `ProbeAnswer`'s sequence is below 1, or a `QueryAnswer` without
`hasBinary` carries binary bytes.

### The opening

The first frame, on the connection: the stream's version
(`SandboxRemoteWire::kVersion`, 1), the host's build, the sandbox bundle and
its bytecode (without one the engine boots from source), and of the validator
and renderer bundles, which stay in the host, only their stamp lines. The engine
needs those because its boot checks the stamps of all three bundles
(`BundleStamp`), so a bundle of another release is refused with the same
verdict and text as in process. A stamp is its bundle's first line (all of it
when there is no newline, "" when there is no renderer bundle, a placeholder
no stamp parses when the first line is empty); the engine's end refuses one
past `SandboxRemoteWire::kMaxStampBytes` (4096) or of more than one line and
never cuts it into another. It answers `Ready` with its version, or `Refused`
with why (another version or build, a malformed opening) and returns. The
host's end queues the runtimes' frames behind the opening without waiting for
the answer, and accepts nothing of a runtime's before `Ready`.

A hosting stream opens with `HostingOpening` instead: the sandbox bundle and
its bytecode, the whole validator bundle and its bytecode, which a hosted
widget's validator runs in the engine process, and the renderer bundle's stamp
line, within the same 16 MiB (about 1.9 MB today), so the engine process still
reads no file. It is checked as `Opening` is (version, build, the renderer's
stamp); the validator checks its own bundle's stamp when it is built, so a
validator bundle of another release gives that widget `bundle-mismatch` and
nothing rendered. `serveHosting` serves either opening; plain `serve` refuses
`HostingOpening` ("kind 32 first, not the opening"), and either opening after
the first ends serving.

### Bounds

- A frame's size is checked from its 4 bytes before its body is read or room
  is made for it: `SandboxRemoteWire::kMaxFrameBytes` in both directions
  (`SandboxEngine::kRemoteOpsLimit` + 64 KiB, an ops batch with room for its
  frame) and `kMaxOpeningBytes` (16 MiB) for the opening, which the host's end
  checks at `start` too (past it, the stream ends at once). A reader's buffer
  is 64 KiB, or one frame when that is larger, and shrinks back after it.
- The string cut: the engine's end cuts every string of its frames to its
  limit plus one byte, `kRemoteOpsLimit` for `Ops` and `kRemoteMessageLimit`
  (1 MiB) for every other one (a probe's answer too), and the host's end cuts
  them the same way before a sink sees them or it keeps a query, an Intl
  request or a probe's answer. A byte past the limit is still past it, so the
  client's own checks give the verdicts they give over the loopback (refused,
  truncated, `protocol-violation`): a widget's oversized message costs that
  message, never the stream every runtime shares.
- The host's end never writes a frame past the bound. A form that does not
  decode again (config, theme params, safe area, data, an outcome, checked as
  the loopback checks them) or a frame past the bound (an html or a query's
  answer that large) loses its runtime only: its client hears a lost
  connection, as over the loopback, and the engine's side gets `Close`.
- What the host's end keeps to write is bounded by `Limits::queuedBytes` (64
  MiB) and `SandboxEngine::kRemoteQueuedCalls` (16384) frames, the one being
  written counted until it is: past either, the stream ends.
- One write, at most 64 KiB of a frame (of the opening too), may block for
  `Limits::writeStallMs` (10 s): past it `widget.monitor` ends the stream, with
  no other call needed. This is a deadline on each write's completion, not a
  measure of byte progress, which the stream does not report: a peer that
  still reads, but too slowly to take 64 KiB within the bound, is cut as one
  that stopped.
- A `probe` waits `Limits::probeWaitMs` (10 s) for its answer and then answers
  `ERROR timeout`, and `ERROR no runtime` once its runtime closed or the stream
  ended; a late answer is dropped and answers no other probe. The bound is the
  transport's wait only: the client's barriers around the call wait for its
  dispatch thread as in process, so delegate or provider code that blocks there
  is not cut short.

A `Limits` value that is not positive and finite takes its default.

### What ends the stream

The engine process is hostile. A byte stream cannot be resynchronized and one
engine process hosts every runtime, so the host's end ends the whole stream on
any frame it cannot accept: a malformed one, a runtime's before `Ready`, a
second `Ready` or `Refused`, a `Refused`, a `Ready` of another version, one of
a runtime id never given, any frame of a runtime but a probe's answer while
that runtime's query or Intl request is in the host's hands, a `ProbeAnswer`
for a sequence never sent. So do the stream's end, a failed write, the write
deadline, the queue bounds and `stop`. The first reason wins and is the one
`Events::ended` gets. Every open runtime's client hears `remoteDisconnected`
once ("the widget engine process ended"), nothing after it is delivered, and a
channel attached later is disconnected at once. A well-formed frame of a
runtime the host already closed is dropped. The engine's end checks the host's
frames the same way and ends serving on one it cannot accept (a form that does
not decode included: the host checked it, so only a broken host sends one); it
drops a closed runtime's frames and an answer that nothing waits for.

The two modes' kinds stay apart, whatever the runtime's state. On an engine
stream the host's end ends the stream on the six hosted kinds of the engine
("kind N on runtime M, which only a hosted runtime sends") and the engine's
end on the four of the host ("… which only a hosting stream takes"). On a
hosting stream the host's end ends it on a runtime's render frames (`Ops`,
`Query`, `Listen`, `PreventDefault`, `Anim`, `RequestsFrame`: "… which a
hosted runtime never sends"), on a second `Intl` while the first is in the
host's hands and on a second `PlatformRequest` while the first is; every other
frame of a hosted runtime is delivered meanwhile, as its engine, glue and
renderer send on their own threads, and a probe's answer waits until neither
request is pending. The engine's end of a hosting stream ends serving on the
engine's host kinds (`Start`, `Event`, `Activation`, `AnimationFrame`,
`Viewport`, `Resize`, `Theme`, `PressBegan`, `PressEnded`, `ChargeValidator`,
`QueryAnswer`: "… which a hosting stream does not take") and on a
`HostedStart` or `HostedTheme` whose form does not decode.

### Threads and order

- The host's end: `widget.reader` reads and gives each frame to its runtime's
  sink in order, `widget.writer` alone blocks in writes, whole frames in the
  order they were queued, and `widget.monitor` keeps the write deadline and
  alone calls `Events`. A runtime's query or Intl request (and a hosted
  widget's platform request) goes to its own `widget.answer`, made at the
  first one, where the client's sink answers it as before (the delegate, or
  the host's `IntlProvider`, on the client's dispatch thread): one runtime
  waiting for its layout answer stalls no other runtime's frames. An engine's
  runtime has one such request at a time; a hosted widget has at most one of
  each kind at once, an Intl request and a platform request, which may wait
  together. A probe's answer that comes meanwhile is published once none
  waits any more and their answers were queued, so a probe still returns
  after everything that came before it.
- A client-mode engine's dispatch thread is `widget.client`, an engine with a
  runtime keeps `widget.sandbox`: a host process whose engines are all remote
  has no thread of that name (JSThread names threads on Apple and Linux).
- The engine's end reads on `serve`'s caller; each runtime is a real engine on
  its `widget.sandbox` thread; a runtime's probes run on its `widget.probe`,
  made at the first one, so the reader goes on answering the queries a probe
  may wait for; closed runtimes are destroyed on `widget.reaper`, their probe
  thread first, never on the reader or their own thread. Frames are written
  whole under one lock; at the end the output is marked broken and the stream
  shut down before any lock is taken, so a write blocked inside it wakes.

### The owner's part

- `Events::runtimes(open)` gives the current count of open runtimes: changes
  close together may come as their last count only, so expect the latest
  count, not one report per change. A stream that ends with runtimes open
  reports 0, then `Events::ended(reason)` comes once and last. Both come from
  `widget.monitor`, outside every lock of the end: return at once and post the
  work; never stop, destroy or call into the end from them, nor drop its last
  reference there. No channel call waits for them.
- `stop()`, also the destructor's, on the owner's thread: ends the stream at
  once, abortively (what is queued is dropped, and the engine process may read
  a cut last frame: a stop is not a drain), joins `widget.reader`,
  `widget.writer` and `widget.monitor` (`ended` came before it returns), then
  lets the stream go. Channels may outlive it: each keeps the end's shared
  state, which holds no OS handle and no thread of the end's, and joins its own
  `widget.answer` when the engine destroys it on the owner's thread. Neither
  cuts short delegate or provider code that already runs: `WidgetHost`'s
  `destroy()` answers its pending layout query dead, and a delegate of your own
  releases its own waits.
- `serve` returns once every runtime and thread it started is gone; a
  `shutdown` of its stream from another thread ends it.

### The test switch

`TW_ENGINE_STREAM=1`, in twidget-tests and twidget-run of any build type
(`StreamSwitch` in `harness/stream_peers.h`), installs a factory whose every
channel is on a host's end joined by an in-process memory pair (1 MiB each
way) to an engine process's end serving on a worker of its own, one stream per
resources object. The tests' last lines count the fixture engines that went
over it, and the run fails unless every one did; a stream that ends before the
switch's own stop, or an engine's end that returns before it, fails the run
(twidget-run exits nonzero). 2026-11-13, Debug: 179 tests, 0 failures, 119 of
119 fixture engines over the stream (142 channels on 5 streams); `--attacks`
133 attacks, 0 leaks, 7 runtimes killed, 133 of them over the stream, every
per-file row the same as in process.

`twidget-tests remote_stream_` runs 35 tests: the 25 described here and the
hosted mode's ten `remote_stream_hosted_` (described in its section below).
The 25 check the wire against the tests' own reading of the tables above, both
ends against scripted bytes, a real `WidgetHost` over the stream (validated
ops, layout answers, Intl through the host's provider, requests and wallet
calls, the bytecode of the opening, several runtimes on one stream, the
refusing channel), the engine process's `lockdown` (held between the opening
and `Ready`, its refusals at scripted bytes and through a real host's end, the
openings it is never called for), 47 hostile inputs, a stuck layout query,
closes and ends while a call waits, races, POSIX socket pairs whose reader
stops in either direction, payload limits, and in a child process on an
inherited socket the thread names: no `widget.sandbox` with three remote
`WidgetHost`s, exactly one once an engine runs in process. With the switch on,
the 24 of 2026-11-13 also ran under ASan and UBSan and under TSan: 0 failures,
no report. 2026-11-14, Debug, in process: the 25 and the whole suite's 183
tests, 0 failures.

## The whole widget in the engine process (hosted mode, `twidget/remote_hosted.h`)

The second remote mode, on the desktop core only: the Swift package and the
Kotlin library have no hosted mode (Swift's remote engine is the engine-only
split above), and tdesktop does not use it yet. The engine process hosts the
whole widget, its engine, a `TrustedValidator` and a renderer its embedder
builds, so ops batches, layout queries, events and frame requests never leave
that process. The host keeps one hosted `WidgetHost` per widget: it delivers
the host's inputs, decides every privileged call again with the remote
engine's client's code, and carries the renderer's traffic with the host's
platform (fonts, text widths, pictures, input) as opaque messages both ways.
It is opt-in and changes nothing for a host that does not use it; the engine
mode above keeps its frames, rules and switches, and a stream carries one
mode, the one its opening names.

- The host: `WidgetHost(html, theme, width, options,
  std::shared_ptr<SandboxHostedChannel>, intl)` is hosted whatever factory is
  set: no validator, no renderer and no QuickJS runtime here, its engine a
  hosted client (`SandboxEngine::hostedClient`) over the channel, the
  `hostedChannel()` of an end `SandboxRemoteHostEnd::startHosting` opened or a
  `SandboxHostedLoopbackChannel` (both halves in one process, the hosted twin
  of `SandboxLoopbackChannel`); a null channel gives a widget that dies ("the
  widget engine process ended"). Tests and harnesses enter the mode with
  `WidgetHost::setHostedChannelFactory(HostedChannelFactory)`: it applies to a
  `WidgetHost` built with a `RendererFactory` while no engine channel factory
  is set (`SandboxEngine::remoteChannelFactorySet()`), so a test of the engine
  mode that set its factory keeps testing that mode. It runs on the
  constructing thread with the host not built yet (keep the reference, call
  nothing on it before the constructor returned) and gets the resources, the
  renderer factory, the scheme and the width the host would have built its
  renderer with. `isHosted()` says which mode a host is in.
- What an embedder uses stays as it is: `start`, `setTheme` (the whole theme
  crosses), `setVisible`, `setData`, `setContentSafeArea`, `probe`, `destroy`,
  the observers, `stoppedLabel` (it travels in the hosted start), the
  diagnostics, the waits, `onHostThread`, `remoteBacklog`. New: `sendPlatform`
  (an opaque message for the engine process's renderer, in order with the
  other inputs), `onPlatformMessage` (the renderer's, at most
  `SandboxEngine::kRemotePlatformLimit` bytes, in its order and in order with
  the widget's other reports, on the host thread), `onPlatformRequest(message,
  respond)` (a request the renderer waits for, one at a time: call `respond`
  once, from any thread, within `WidgetHost::kPlatformAnswerSeconds`, 3 s;
  never wait for the widget inside it, its dispatch thread waits for the
  answer), and `pressDelivered` / `releaseDelivered` (below).
- What hosted mode has not: `renderer()` throws `std::logic_error`; `event()`,
  `onGesture` and `onGestureClaim` do nothing, the renderer's input and its
  gesture claims being its platform traffic; `diagnostics().height` stays 0
  and `firstContentMs` unset, the embedder's to take from that traffic.
  `rendererReadyMs`, `rendered` and `renderedMs` (so `waitRendered`) and
  `nodes` come from the engine process's glue (its first report, on the host's
  clock); the rest of the diagnostics come as over the engine mode, with the
  engine process's validator in `stats.validatorMs`.
- The tap: a host that set `onRenderOps` before `start()` asks for every
  validated batch in its hosted start and gets them (`ValidatedOps`, sent by
  the engine process next to its renderer's); set later, or unset, it gets
  none, and a batch it did not ask for is a `protocol-violation`, dropped.
  `twidget-run --attacks` and an embedder's test harness use it.
- The engine process: `SandboxRemoteEngineEnd::serveHosting(stream, build,
  lockdown, HostedRendererFactory)` serves either opening (an engine stream's
  runtimes as `serve` does). At each hosted widget's `HostedStart`, on its
  reader thread, it builds the widget's engine (on `widget.sandbox`), a
  `TrustedValidator` from the opening's bundle (on `widget.validator`),
  `WidgetHost`'s own glue between them (a served `WidgetHost` on its
  `twidget-host`: the validated ops, the 3 s layout query, listen,
  preventDefault, anim, frame requests, theme re-sanitising, `frozen` with the
  host's `stoppedLabel`) and a renderer from the factory. The factory gets the
  opening's resources, the scheme, the width and a `SandboxHostedPlatform`:
  `send(message)` and `request(message)` (from any thread but the glue's: the
  host's answer, or nullopt once the runtime closed or the stream ended; one
  waits at a time, a second waits for the first) towards the host, and
  `onMessage`, set when the renderer is built, the host's messages in order on
  the glue's thread. The framework knows no particular renderer. A factory
  that throws or returns nothing ends only that widget ("the widget could not
  be built in the engine process: …"); a validator bundle that does not load
  ends the engine process, every widget on its stream with it, as it ends a
  host whose validator is in process. The factory never waits for the host
  (no `request` inside it).
  `serveHosting` is defined in `remote_hosted.cpp`, so a helper that calls only
  `serve` links no `WidgetHost` or validator.

```cpp
#include "twidget/remote_stream.h"

// The host: a hosting stream, one hosted WidgetHost per widget on it.
std::shared_ptr<twidget::SandboxRemoteHostEnd> end = twidget::SandboxRemoteHostEnd::startHosting(stream, *resources, "build-1", events, {});
auto host = std::make_unique<twidget::WidgetHost>(html, theme, width, options, end->hostedChannel(), intl);
host->onPlatformMessage = [](const std::string& message) { postToRenderer(message); };
host->onPlatformRequest = [](const std::string& message, std::function<void(std::string)> respond) { respond(measure(message)); };
host->start();
// A primary press the embedder delivers to the visible widget, then its release: the only source of grants and of the haptic touch.
host->pressDelivered();
host->sendPlatform(pressMessage);
host->releaseDelivered();
```

```cpp
// The engine process: the embedder's renderer for each hosted widget, which sets platform->onMessage before it returns.
twidget::SandboxRemoteEngineEnd::serveHosting(stream, "build-1", lockDown,
    [](std::shared_ptr<twidget::SandboxResources> resources, const std::string& scheme, double width, std::shared_ptr<twidget::SandboxHostedPlatform> platform) {
        return makeRenderer(std::move(resources), scheme, width, std::move(platform));
    });
```

### What crosses and what stays inside

- Host to engine on a hosting stream: `HostingOpening`, `Open`, `HostedStart`,
  `HostedTheme`, `Platform`, `PlatformAnswer`, and as for an engine `SafeArea`,
  `Data`, `Visible`, `Stop`, `Answer`, `Probe`, `IntlAnswer`, `Close`.
- Engine to host: `Ready` or `Refused`, `PlatformMessage`, `PlatformRequest`,
  `ValidatedOps`, `RendererReady`, `Rendered`, `Nodes`, and as for an engine
  `Intl` (the host's `IntlProvider` still formats), `Log`, `Diag`, `Bridge`,
  `CopyText`, `Request`, `Wallet`, `Storage`, `ResizeAnchor`,
  `WatchesPublicData`, `Loaded`, `Stats`, `Died`, `ProbeAnswer`
  (`TaskStarted` and `TaskCompleted` are accepted; a hosted widget sends none).
- Inside the engine process only: `Start`, `Event`, `Activation`,
  `AnimationFrame`, `Viewport`, `Resize`, `Theme`, `PressBegan`, `PressEnded`,
  `ChargeValidator`, `QueryAnswer`, `Ops`, `Query`, `Listen`,
  `PreventDefault`, `Anim`, `RequestsFrame`. A hosted client's `start()`,
  `event`, `activationEvent`, `frame`, `viewport`, `resize`, `theme`,
  `pressBegan`, `pressEnded` and `chargeValidator` do nothing (`WidgetHost`
  starts it with `startHosted`), so no such frame is written; each end of a
  hosting stream ends it on the other side's ("What ends the stream" above),
  and a hosted client refuses the render calls of a transport that delivers
  them (`protocol-violation`, before they are admitted).

### Who decides

The engine process stays hostile: its renderer's word is not the host's
input. A grant exists only for a primary press the embedder says it delivered
to the visible widget: `pressDelivered()` stamps the time on the caller's
thread and, on the client's dispatch thread, unless the widget is dead or a
preview, opens the haptic gate's press and, while visible, records one grant:
one privileged call (a link, `copyText`, a foreground request, a wallet call)
within `SandboxEngine::kRemoteGrantWindowMs` (2 s), the newest
`kRemoteMaxGrants` (32) kept, in the same list as a remote engine's taps.
`releaseDelivered()` ends the press, and the gate's 5 s touch window starts.
Neither goes on the stream. Every privileged call is then decided by the
client's code as for a remote engine: `sdk::checkBridgeCall`, the preview,
the grant, the haptic gate, the `RequestGate` with the host's visibility and
in-flight state, the copy and storage checks, the size and count bounds. No
engine frame creates a grant or opens the haptic window: a call the engine
process allowed on a tap the host never delivered gets `activation-blocked`,
a "press" in a platform message is only a message, and `PressBegan` or
`Activation` from the engine's side ends the stream as the other direction's
kinds always did.

### Bounds

- `PlatformMessage` and `PlatformRequest` strings are cut to
  `SandboxEngine::kRemotePlatformLimit` (32 MiB) plus one byte at both ends,
  `ValidatedOps` to `kRemoteOpsLimit` plus one, within `kMaxFrameBytes` as
  every frame: a message past its limit is a `protocol-violation`, dropped, a
  request past it is answered "" with one, validated ops past it are one too.
  Each is admitted against `kRemoteQueuedBytes` and `kRemoteQueuedCalls`
  before anything of it is copied and stays counted until `onPlatformMessage`
  returned (or the batch reached `onRenderOps`): a flood kills the widget
  once ("the widget engine process sent more than the host keeps queued"),
  while the host's end goes on reading for the stream's other runtimes.
  `Nodes` is clamped to [0, `INT_MAX`]. `Platform` from the host is bound by
  the frame: a message that does not fit loses its runtime.
- One platform request waits per hosted widget; a second while the first is
  in the host's hands ends the stream. It goes to its runtime's
  `widget.answer` and is answered through the client on its dispatch thread,
  as a query is, so one widget's wait delays no other. Without `respond`
  within 3 s, or with no `onPlatformRequest` when it arrives, that widget dies
  with diagnostic `platform-timeout` ("the host's platform did not answer the
  widget's renderer within 3 s", or "the host's platform answers no requests
  of the widget's renderer"): its client sends `Stop` and `Close`, the
  renderer's `request` returns nullopt, a late `respond` changes nothing, and
  the stream's other widgets go on. `destroy()` while a request waits settles
  it at once, and the widget dies "terminated".

### Threads

- The host's process, per hosted widget: `twidget-host`, `widget.client` and,
  from its first Intl or platform request, `widget.answer`; per stream
  `widget.reader`, `widget.writer` and `widget.monitor`. There is no
  `widget.validator` and no `widget.sandbox` for a hosted widget: no QuickJS
  runtime in the host.
- The engine process, per hosted widget: the served host's `twidget-host`
  (the glue and every renderer call), `widget.validator`, `widget.sandbox`,
  the renderer's own threads, and `widget.probe` from its first probe; per
  stream `serve`'s caller (the reader, which builds each widget at its start,
  the validator's construction included) and `widget.reaper`. Every library
  thread is a `JSThread`, and a widget's are made while it is built, inside
  the catch that turns a failure into that widget's death.

### The lockdown

`lockdown` is called at the same point for either opening: once on `serve`'s
thread, after `HostingOpening` was accepted and its resources built, before
`Ready` and before any runtime exists; its refusals are `serve`'s. With every
bundle in the opening the engine process reads no file:
`remote_stream_hosted_engine_process_reads_no_file` serves `serveHosting` in
a child process whose lockdown sets `RLIMIT_NOFILE` to {0, 0}, and its hosted
widgets load, render and fill the tap.

### The hosted test switch

`TW_ENGINE_HOSTED=loopback` or `=stream`, in twidget-tests and twidget-run of
any build type (`HostedSwitch` in `harness/hosted_peers.h`, test and harness
code; another value, or either with `TW_ENGINE_LOOPBACK=1` or
`TW_ENGINE_STREAM=1`, exits 2), installs the hook as each test's baseline:
every `WidgetHost` built with a renderer factory while no engine channel
factory is set runs hosted, behind a `SandboxHostedLoopbackChannel` or a
stream of its own (a 1 MiB memory pair, `startHosting`, `serveHosting` on a
worker, the engine's side counting the frames by kind), torn down when the
widget lets its channel go. The fixture's renderer is built at once on the
constructing thread with the fixture's own factory, and the engine's side
gets a proxy that forwards the glue's calls to it. The renderer's `gesture` is
`pressDelivered`, its `release` `releaseDelivered`, a trusted event that
begins a new gesture with no `gesture` before it both, and each then goes on
as `sendPlatform` to the glue; its other messages go to the glue directly.

The fixture widgets are the `WidgetHost`s whose renderer `scripted()` builds
while no engine channel factory is set. Three tests whose subject is the
in-process glue's own renderer boundary (its gesture claims, its height) open
an `InProcessWidgets` scope and run in process:
`sdk_widget_host_theme_locale_and_gestures`,
`sdk_deprecated_host_api_forwards` and
`sdk_resize_anchor_is_host_state_next_to_the_height`. The 121 `Run` fixtures
are engines with a recording delegate and no renderer, so they stay in process
too (the engine switches cover them). The tests' last lines count the fixture
widgets and name the in-process tests; the run fails unless every fixture
widget went hosted, when a stream ended before its widget let it go, and over
the stream when a kind that stays in the engine process crossed a hosted
stream or `ValidatedOps` crossed one whose widget did not ask. twidget-run
runs every widget hosted with `CannedRenderer` (`ready` on load,
`RecordingHost::cannedAnswer` for every query, no frames), scans the validated
ops it asked for, and delivers `--events` as the renderer's (a numeric
`gesture` makes one trusted, framed by `gesture` or `release`); it applies the
same checks to its streams. 2026-11-16, Debug: 204 tests, 0 failures with
either value, 19 of 19 fixture widgets hosted (over the stream on 19 streams,
no frame of a kind that stays in the engine process on any, `ValidatedOps`
only on the 2 whose widget asked); `--attacks` 133 attacks, 0 leaks, 7
runtimes killed, the same files and reasons as in process, 133 hosted;
`--corpus ../../corpus/compat` 19 / 19 loaded, every row's status that of in
process.

`twidget-tests hosted_` (19) runs the hosted mode's own tests: the coders, the
hosted client's grants from delivered presses and its bounds against a fake
channel, the loopback (the whole widget in the engine's side, the tap,
privileged calls on the host's presses, platform traffic and the 3 s rule,
build failures), a probe behind the safe area and theme that the engine's
side's glue thread relays, over the loopback and a hosting stream, and
`remote_stream_hosted_` (10): the wire against the tests' own reading of the
tables, the hosting opening and the lockdown, the engine's end serving a
widget, a real `WidgetHost` over the stream with a frame count (and an
engine-mode control that shows `Ops` and `Query`), a lying engine end written
frame by frame, hostile frames, a platform flood within the bounds, an
unanswered request that kills only its runtime among three, and two child
processes: an engine end that cannot open a file and a host's thread names (no
`widget.validator` or `widget.sandbox` with three hosted widgets).
