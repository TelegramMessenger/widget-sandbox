# widget-sandbox desktop core (C++)

The sandbox for Telegram Desktop–class hosts (Qt/C++), on the Nova engine
since 2026-11-17 on this branch (`nova-safe`; every record below dated before
that was measured on QuickJS, and names its build's parts as they were): the engine,
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
    trusted_validator.h      validator-bundle.js in a second, trusted Nova engine
    widget_host.h            WidgetHost: engine + validator + Renderer, one widget (= WidgetHost.swift / .kt)
    renderer.h               Renderer seam + RendererFactory (= Renderer.swift / .kt)
    cpu_budget.h             CpuBudget (fixture: packages/core/tests/fixtures/cpu-budget.json)
    sdk_policy.h             SDK v1 checks: links, haptics + HapticGate, theme params, locale, storage, bridge calls;
                             the widget's state, viewer, chat type, request answers + RequestGate, previews
                             (= SdkPolicy.swift / .kt; fixture: packages/core/tests/fixtures/sdk-policy.json)
    gesture_policy.h         the widget or the bubble: claimOf + GestureArbiter for the client's recognizers
                             (= GesturePolicy.swift / .kt; fixture: packages/core/tests/fixtures/gesture-policy.json)
    js_thread.h              one dedicated thread per runtime, 8 MB stack, timers
    nova_context.h           thin wrapper over the Nova engine via nova_embed_ffi's C interface (nova_engine.h)
    resources.h              WidgetJS/ loader + CDN URL → bundled library
    intl.h                   IntlProvider; BasicIntl fallback (plug QLocale/ICU here)
    json.h                   small strict JSON (hostile input: bounded depth)
  sandbox/src/               the sources; remote_internal.h is what the remote transports share without making it public
                             (twidget::detail: the loopback's sink, the remote Intl, the hosted runtime, the serving loop),
                             remote_wire.h the stream's frame codec both its ends use, steady_clock.h the engine's clock,
                             widget_host_internal.h what both WidgetHost sources use; the host's side and the rest:
                             "The host's side alone" below
  renderer/                  BlitzRenderer (twidget/blitz_renderer.h): the Renderer over ../renderer-blitz's C ABI; -DTW_BLITZ=ON only
                             LitehtmlRenderer (twidget/litehtml_renderer.h): the same over ../renderer-litehtml's C ABI; -DTW_LITEHTML=ON, macOS only
  harness/                   twidget-run + RecordingHost (fake host with canned layout answers) + png.h
                             + stream_peers.h (test byte streams and the TW_ENGINE_STREAM switch, not the library's;
                               memory_stream.h its in-memory pair alone)
                             + engine_end.h/.cpp (TW_ENGINE_END=rust: the engine process's end in ../rust, for engine and hosting streams; -DTW_RUST_ENGINE_END=ON only)
                             + hosted_peers.h (the TW_ENGINE_HOSTED switch and the canned renderer, not the library's)
                             + host_only.cpp (twidget-host-only, built from the host's side alone)
  tests/                     twidget-tests (XCTest suite ported) + fixtures/
  WidgetJS/                  bundles from `pnpm build:desktop` (committed)
```

The engine is Nova (github.com/TelegramMessenger/nova-safe: its engine crates
forbid `unsafe` code), linked as the static library of its `nova_embed_ffi`
crate: `-DTW_NOVA_ROOT=<a nova-safe checkout>` builds it with cargo, or
`-DTW_NOVA_LIBRARY=<the library> -DTW_NOVA_INCLUDE=<the directory of nova_engine.h>`
links a built one (tdesktop builds it from its own nova-safe submodule, for its
widget helper). It needs nova-safe from the commit that added
`nova_engine_set_out_of_memory_fatal` and `nova_return_range_error`: out of
memory ends a script wherever it happens, a host function throws its RangeError
itself, and strings go into the engine as WTF-8, as they come out. The bundles
run from code compiled once per process (`NovaContext::evaluateBundle`): there
is no bytecode file. Besides it the library links only the C++ standard library,
pthreads and `m`; on Windows `bcrypt` (CSPRNG) and `_beginthreadex`.

## Build and run

```bash
corepack pnpm build:desktop                       # bundles → packages/desktop/WidgetJS, fixture → packages/desktop/tests/fixtures
cd packages/desktop && cmake -S . -B build -G Ninja -DTW_NOVA_ROOT=<a nova-safe checkout> && cmake --build build
./build/twidget-tests                             # 249 tests (engine, limits, validator, JSThread, CpuBudget, SDK and gesture fixtures, SDK v1, JSON, the remote engine, its byte stream and the hosted mode); `./build/twidget-tests remote_` runs the remote engine's 126, `remote_stream_` the byte stream's 63 and `hosted_` the hosted mode's 55 of them
./build/twidget-host-only                         # built from the host's side alone ("The host's side alone" below): prints `host-only: hosted=1 dead=the widget engine process ended kept=0` and exits 0
./build/twidget-run ../corpus/compat/tabs.html --wait-ms 500 [--verbose] [--ops] [--probe 'document.title']
./build/twidget-run w.html --theme dark --theme-params '{"bg_color":"#212121"}' --locale ru --storage '{"local":{"n":"1"}}' \
    --events '{"type":"click","targetId":5,"detail":1,"gesture":1}'   # SDK v1: prints `action …`, `storage …` and `resize anchor …`; "gesture" makes a trusted press (and the haptic gate's press, or its release for anything but pointerdown / mousedown)
./build/twidget-run w.html --message group --viewer 42,Ann,Lee,ann_lee,premium --chat-data c1 --public-data p1 --answer '{"result":"ok"}' \
    --events '{"type":"click","targetId":5,"detail":1,"gesture":1}'   # the bot's requests: prints `request …` and `copyText …`; --message private|group|via-bot|no-queries|live-copy|copy|preview, or --copy / --can-query / --background-query / --chat-type; --answer-error TIMEOUT, --set-data JSON
./build/twidget-run --corpus ../../corpus/compat     # loaded, alive, no uncaught errors; also generated/with-profile, generated/without-profile
./build/twidget-run --attacks                     # 133 files: nothing but data:/#fragment may reach the validated ops
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTW_NOVA_ROOT=<a nova-safe checkout> && cmake --build build-debug
TW_ENGINE_LOOPBACK=1 ./build-debug/twidget-tests   # the same 249 with every engine SandboxEngine::make builds behind the in-process loopback (the last lines count them)
TW_ENGINE_LOOPBACK=1 ./build-debug/twidget-run --attacks   # 133 attacks, 0 leaks, "133 through a client-mode engine"; NDEBUG (the default RelWithDebInfo) compiles the switch out
TW_ENGINE_STREAM=1 ./build-debug/twidget-tests     # the same 249 with every engine SandboxEngine::make builds over the byte stream (below), in any build type; fails unless every fixture engine went over it
TW_ENGINE_STREAM=1 ./build-debug/twidget-run --attacks     # 133 attacks, 0 leaks, "133 of them over the stream"; a stream that ends before the run's own stop fails the run
TW_ENGINE_STREAM=1 TW_ENGINE_END=rust ./build-rust/twidget-tests   # the same with every engine stream (the switch's and the stream tests' own) served by the engine end in Rust (../rust, crates/engine_ffi), in a build configured with -DTW_RUST_ENGINE_END=ON (cargo builds it, and Nova comes from it: the build's one Nova, from the nova-safe beside widget-sandbox that ../rust depends on, which TW_NOVA_ROOT may name and no other; TW_NOVA_LIBRARY is refused); 2026-11-20 (Windows x64 RelWithDebInfo): 230 tests, 0 failures, 128 of 128 fixture engines over the stream; --attacks and --corpus give the C++ end's rows
TW_ENGINE_HOSTED=loopback ./build/twidget-tests    # the same 249 with every fixture WidgetHost hosted (below): its engine, validator and glue behind a SandboxHostedLoopbackChannel, its renderer here; any build type; fails unless every fixture widget went hosted
TW_ENGINE_HOSTED=stream ./build/twidget-tests      # the same over a hosting stream per widget; also fails when a kind that stays in the engine process crossed one
TW_ENGINE_HOSTED=stream ./build/twidget-run --attacks      # 133 attacks, 0 leaks in the validated ops the run asked for, "133 hosted over the stream"; =loopback too, and --corpus ../../corpus/compat with either
TW_ENGINE_HOSTED=stream TW_ENGINE_END=rust ./build-rust/twidget-tests   # the same with every hosting stream (the switch's and the hosted stream tests' own) served by the end in Rust: the widgets' engines, validators and glue in ../rust's twidget_host (twidget_engine_serve_hosting), their C++ renderers behind its C renderer factory (harness/engine_end.cpp); 2026-11-20 (Windows x64 RelWithDebInfo): 230 tests, 0 failures, 19 of 19 fixture widgets over the stream, the C++ end's frame tallies; twidget-run --attacks and --corpus give the C++ end's rows
```

On Windows (MSVC, Git Bash) the same suites build and run from `packages/desktop` as checked out with
`core.autocrlf=true`: no LF export and no bundle rebuild (`SandboxResources::fromDirectory` reads the bundles' CRLF
sources as LF). CMake on `PATH`, Visual Studio 2026 with the v143 toolset; each run clears the three switches and sets
at most one:

```bash
cmake -S . -B build-win -G "Visual Studio 18 2026" -A Win32 -T v143
cmake --build build-win --config Debug --target twidget-tests twidget-run --parallel
env -u TW_ENGINE_STREAM -u TW_ENGINE_LOOPBACK -u TW_ENGINE_HOSTED ./build-win/Debug/twidget-tests.exe                       # in process
env -u TW_ENGINE_STREAM -u TW_ENGINE_LOOPBACK -u TW_ENGINE_HOSTED ./build-win/Debug/twidget-run.exe --attacks
env -u TW_ENGINE_STREAM -u TW_ENGINE_LOOPBACK -u TW_ENGINE_HOSTED TW_ENGINE_STREAM=1 ./build-win/Debug/twidget-tests.exe    # over the byte stream
env -u TW_ENGINE_STREAM -u TW_ENGINE_LOOPBACK -u TW_ENGINE_HOSTED TW_ENGINE_STREAM=1 ./build-win/Debug/twidget-run.exe --attacks
env -u TW_ENGINE_STREAM -u TW_ENGINE_LOOPBACK -u TW_ENGINE_HOSTED TW_ENGINE_LOOPBACK=1 ./build-win/Debug/twidget-tests.exe  # behind the loopback
env -u TW_ENGINE_STREAM -u TW_ENGINE_LOOPBACK -u TW_ENGINE_HOSTED TW_ENGINE_LOOPBACK=1 ./build-win/Debug/twidget-run.exe --attacks
cmake --build build-win --config RelWithDebInfo --target twidget-run --parallel   # the timing control; NDEBUG compiles the loopback switch out, so it is timed in process
env -u TW_ENGINE_STREAM -u TW_ENGINE_LOOPBACK -u TW_ENGINE_HOSTED ./build-win/RelWithDebInfo/twidget-run.exe ../../corpus/attacks/timer-flood.html \
    --wait-ms 1500 --verbose --events '{"type":"click","targetId":2,"clientX":5,"clientY":5}'   # one run's `js N ms` and its cpu-throttled detail; the click is the one --attacks adds; ./build-win/Debug/ and a switch for the Debug figures
```

Windows record (2026-11-16): Windows 11 Pro 10.0.26300 (x64), Intel Core i9-14900K (32 logical processors), MSVC
19.44 (v143), Win32, Debug: the interpreter (`twquickjs`) `/O2` with inlining, the package's C++ `/Od /Ob0 /RTC1
/MDd`, every target `/utf-8`. On this processor the widget's CPU time is the thread's cycles over CPUID leaf 0x15's rate
(GenuineIntel, invariant TSC, Hyper-V's root partition: `docs/decisions.md`, "CPU time, not wall-clock time"). The
three Windows fixture corrections are in `docs/changes.md`. One run of each command above:

| Mode | `twidget-tests` | Fixture engines and widgets | `twidget-run --attacks` |
| --- | --- | --- | --- |
| in process | 201 tests, 13204 checks, 0 failures | 125 engines, none remote; 19 widgets, none hosted | 133 attacks, 0 leaks, 7 runtimes killed |
| `TW_ENGINE_STREAM=1` | 201 tests, 13206 checks, 0 failures | 125 of 125 over the stream (148 channels on 8 streams, 0 ends before its finish); 1 widget, not hosted | the same, 133 through a client-mode engine, 133 of them over the stream |
| `TW_ENGINE_LOOPBACK=1` | 201 tests, 13204 checks, 0 failures | 125 of 125 behind the loopback; 19 widgets, none hosted | the same, 133 through a client-mode engine |

201 tests compile on Windows: the 204 less six `remote_stream_` cases that are macOS and Linux only, plus the three
Windows-only `icu_intl_` tests. Every test has a result in each mode; no bundle was refused and none fell back to
source. `--attacks` gives 126 `ok` and 7 `dead` in each mode, the dead ones `deep-nesting.html`,
`fallback-at-boot.html`, `layout-query-loop.html`, `mem-cap-probe.html`, `mem-huge-bitmaps.html`,
`mem-steady-growth.html` and `mem-uncaught-growth.html`, and every per-file row is the same in the three modes without
exception: status, diagnostic kinds and counts, death text.

One Windows exception, in Debug builds: `timer-flood.html` reads `ok cpu-throttled, timer-limit` where Linux's
committed in-process row (Debug, an older framework commit) reads `ok timer-limit`. Its measured cause is the Debug
build's execution cost against the unchanged quota: the widget's JS passes the 300 ms per 1000 ms quota by the time its
first timer comes due, so the quota defers that timer and reports it, while an optimised build of the same tree stays
under it. The quota acts as designed (the boot task's overrun is a debt, the next timer waits it out, the widget stays
`ok`) and nothing is stopped, so this is not a product deviation. 20 runs of each, interleaved, with the commands
above (`js` is the run's whole JS, the boot task most of it; the detail is the quota's own reading in "deferred 1
timer: N ms of the 300 ms per 1000 ms budget used"):

| `timer-flood.html` | Single-file runs: diagnostics | `cpu-throttled` | `js`, ms (min / median / max) | Detail, ms of 300 | `--attacks` on the file alone | Deaths |
| --- | --- | --- | --- | --- | --- | --- |
| Debug, in process | `cpu-throttled, timer-limit` | 20 of 20 | 310 / 317 / 329 | 307–323 | `ok cpu-throttled, timer-limit` 20 of 20 | none in 40 |
| Debug, `TW_ENGINE_STREAM=1` | `cpu-throttled, timer-limit` | 20 of 20 | 309 / 316.5 / 344 | 306–341 | `ok cpu-throttled, timer-limit` 20 of 20 | none in 40 |
| Debug, `TW_ENGINE_LOOPBACK=1` | `cpu-throttled, timer-limit` | 20 of 20 | 311 / 316 / 324 | 308–319 | `ok cpu-throttled, timer-limit` 20 of 20 | none in 40 |
| RelWithDebInfo, in process | `timer-limit` | 0 of 20 | 171 / 172 / 173 | none printed | `ok timer-limit` 20 of 20 | none in 40 |

The diagnostic came in every Debug run of every mode, so it does not follow the mode, and the full rows agree. No run
died: the widget never reached the 500 ms slice ("CPU limit exceeded: more than 500 ms of continuous execution"). The
machine was otherwise idle: in 1 s windows over both batches the processes other than the runs used 0.9–5.4 % of its
32 logical processors (median 1.7 %) and no compiler or linker ran; each full run above started with the whole machine
at 1.0–5.6 %. The A/B changes the build configuration as a whole (the C++ glue `/O2 /Ob1` without `/RTC1`, the
release CRT and its heap, no checked iterators, `NDEBUG`, and the interpreter's `/Ob1` and release CRT): it shows the
Debug build's cost, not which of the CRT heap, checked iterators, inlining or `/RTC1` causes it.

Per-file equality with macOS was not checked: no committed record holds macOS's per-file `--attacks` rows. What
committed macOS evidence states (Debug) is what these runs give: 133 attacks, 0 leaks, 126 `ok`, the same seven `dead`
files, each dead row with the diagnostics macOS's records print and a death text that begins with theirs (cut short
there), and every row the same across the modes. For the diagnostics of `ok` rows only Linux's committed in-process rows
exist (Linux, Debug, an older framework commit): `timer-flood.html` is the exception above, and `cpu-kill-after.html`
reads `ok cpu-throttled` here in all three modes where Linux read `ok cpu-throttled×2` in 9 of 10 runs (the report comes
at most once a second inside the 1500 ms wait; one run per mode here, and no cause is measured for the count).

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
  transport's end (`remoteDisconnected`) is the transport's word, not the
  engine's side's: it is queued once, in order after what came before it,
  without taking a place, so a client that holds exactly `kRemoteQueuedCalls`
  legal calls still hears the end as the end. A message past the bound came
  before the end and keeps its own death: the end defers to an overflow claimed
  by the time it runs on the dispatch thread, so the widget dies flooded also
  when a route's `widget.answer` claimed it after the reader's fan-out queued
  the end. The bounds' check, the rejection and the overflow's claim are one
  step under the backlog's lock, and the end's choice takes that lock too: a
  message the admission found past the bounds before the end chose is that
  flood, never a rejection lost between the two (a test holds a rejection in
  the middle of that step with `SandboxEngine::setRemoteRejectionStep`, whose
  second callback reports the end's choice on the dispatch thread as it takes
  that lock, so the test sees the choice wait for the held step). The
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

On macOS a blocking `recv` can miss the end of its stream (seen there; other
systems were not measured): when it starts to wait just as the end comes, the
peer's `SHUT_WR` or this end's own `SHUT_RD`, now and then it sleeps on with
the end already in its socket, so the engine process never sees the host stop,
or a host's `stop` waits on its own reader. A `select` or `poll` that waits
for the socket sees that end. So a macOS adapter's `read` calls
`recv(fd, out, size, MSG_DONTWAIT)` and, on `EAGAIN`, waits with no timeout
until the socket is readable, then calls it again. Bytes and the end (0)
return as they came, `EINTR` from the call or the wait tries again, and any
other error ends the read with 0. It waits in `select` while the descriptor
is below `FD_SETSIZE` (`FD_SET` past it writes outside the set, so never hand
it a larger one) and in `poll` past it: `poll` fails with `EINVAL` in a
process whose `RLIMIT_NOFILE` is 0, which an engine process that locks itself
down may set, and `select` still works there. A receive timeout
(`SO_RCVTIMEO`) is no way out: `SHUT_RD` clears it.

One reader and one writer at a time; the owner destroys the stream only after
both returned. The host's end holds the stream until `stop` has joined its
threads, then lets it go. The test peers are in `harness/stream_peers.h`, test
and harness code, not the library's: an in-memory pair (`harness/memory_stream.h`,
which needs nothing but the standard library and this interface), and on POSIX
a socket pair whose writes really block, which shuts each direction on its own
and reads as above (`SocketStream::read`, on every POSIX system). The tests
write and read the frames below by hand and play the engine process's end
with them against a real host's end (`tests/test_stream_frames.h`, with no
test runner either: `ScriptedEngineEnd` opens the host's end on a memory pair,
keeps the frames it writes, answers its opening and closes, each wait under
its owner's bound, and joins its reader within one), so a host's own check of
a stream plays the engine process's end with the same peer the tests'
`ScriptedEngine` wraps.

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
| 1 | `Opening` | 0 | i64 version, str build, str sandboxBundle, str validatorStamp, str rendererStamp | `SandboxRemoteHostEnd::start` |
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
| 32 | `HostingOpening` | 0 | i64 version, str build, str sandboxBundle, str validatorBundle, str rendererStamp | `SandboxRemoteHostEnd::startHosting` |
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
(`SandboxRemoteWire::kVersion`, 1), the host's build, the sandbox bundle, and of the validator
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
the whole validator bundle, which a hosted
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
deadline, the queue bounds and `stop`, and on a hosting stream opened with its
owner's `SandboxHostedAnswers` a hosted renderer's answer time and the bound
of the messages kept for one route ("A renderer that stops answering" in the
hosted mode below). The first reason wins and is the one `Events::ended`
gets. Every open runtime's client hears `remoteDisconnected`
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
  order they were queued, and `widget.monitor` keeps the write deadline (and
  a hosting stream's answer times) and alone calls `Events`. A runtime's query or Intl request (and a hosted
  widget's platform request) goes to its own `widget.answer`, made at the
  first one, where the client's sink answers it as before (the delegate, or
  the host's `IntlProvider`, on the client's dispatch thread; a platform
  request waits there for its responder, which the dispatch thread hands the
  delegate without waiting): one runtime waiting for its layout answer stalls
  no other runtime's frames. An engine's runtime has one such request at a
  time; a hosted widget has at most one of each kind at once, an Intl request
  and a platform request, which may wait together. A probe's answer that comes
  meanwhile is published once none waits any more and their answers were
  queued, so a probe still returns after everything that came before it.
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
- A thread the OS refuses to start (`JSThread` throws `std::system_error`)
  ends only what it would serve, never the process, and nothing waits for it.
  On the host's end `start` and `startHosting` throw when one of their three
  threads cannot start ("The owner's part" below), and a runtime whose
  `widget.answer` cannot start is lost alone: its client hears why (the
  refusal, which names the thread and the OS's error) as its death, then
  `remoteDisconnected`, and its `Close` goes out. On the engine's end a
  runtime whose `widget.sandbox` cannot start dies with why, what was kept for
  it dropped (behind the in-process loopback too), and one whose
  `widget.probe` cannot start is stopped with why as `Stop` stops it, that
  probe answered "ERROR no runtime" at once (a later probe tries the thread
  again). The stream and the other runtimes go on; a hosted widget's own
  threads are in the hosted mode's "Threads" below. A `widget.reaper` that
  cannot start ends serving with why, the closed runtime it could not take
  left to the end. The end closes every runtime still open before it joins
  anything, then destroys them on `serve`'s thread, so ending starts no
  thread.

### The owner's part

- `Events::runtimes(open)` gives the current count of open runtimes, a hosted
  route whose renderer is kept not among them ("Kept renderers" below): changes
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
  releases its own waits. When the stream ended otherwise first (a refusal
  before `Ready` among them), it returns that end's `std::chrono::steady_clock`
  instant, taken as the end came (for an answer time that ran out, the instant
  it was judged to have run out, at which the widgets' deaths were decided),
  also while `ended` has yet to report that end and at a second stop; none
  when it or an earlier stop ended the stream.
- `start` and `startHosting` throw `std::system_error` when `widget.reader`,
  `widget.writer` or `widget.monitor` cannot start: those already started are
  joined, nothing was written to the stream, `Events` hear nothing, and the
  stream stays the caller's to shut down.
- `serve` returns once every runtime and thread it started is gone; a
  `shutdown` of its stream from another thread ends it. It returns why when
  its `widget.reaper` cannot start (tdesktop's helper prints a non-empty
  reason and exits 1); a runtime's own thread that cannot start is that
  runtime's death, never the end of serving.

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

`twidget-tests remote_stream_` runs 45 tests: the 29 described here and the
hosted mode's sixteen `remote_stream_hosted_` (in its section below).
The 29 check the wire against the tests' own reading of the tables above, both
ends against scripted bytes, a real `WidgetHost` over the stream (validated
ops, layout answers, Intl through the host's provider, requests and wallet
calls, the bytecode of the opening, several runtimes on one stream, the
refusing channel), the engine process's `lockdown` (held between the opening
and `Ready`, its refusals at scripted bytes and through a real host's end, the
openings it is never called for), 47 hostile inputs, a stuck layout query,
closes and ends while a call waits, races, POSIX socket pairs whose reader
stops in either direction, payload limits, and in a child process on an
inherited socket the thread names: no `widget.sandbox` with three remote
`WidgetHost`s, exactly one once an engine runs in process; and, on macOS, in
child processes that use up their thread allowance (every other thread made
first), a host's end whose threads cannot start, a runtime whose
`widget.answer` cannot start (on scripted engine and hosting streams and over
both real ends), runtimes whose `widget.sandbox` (over the stream and behind
the loopback) or `widget.probe` cannot start, and a `widget.reaper` that
cannot start at a `Close` and at the end of a stream. With the switch on,
the 24 of 2026-11-13 also ran under ASan and UBSan and under TSan: 0 failures,
no report. 2026-11-14, Debug, in process: the 25 and the whole suite's 183
tests, 0 failures. 2026-11-17, Debug, with the threads that cannot start: the
whole suite's 223 tests, 0 failures in each of the five modes (124 of 124
fixture engines over the stream on 148 channels and 8 streams, 19 of 19
widgets hosted on 19 streams, no early end) and in process under ASan and
UBSan with no report; `--attacks` 133 attacks, 0 leaks, 7 runtimes killed in
process, over the stream and hosted over the stream, every per-file row that
of in process.

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
  once, from any thread, within `WidgetHost::kPlatformAnswerSeconds`, 3 s; the
  client waits for it on the request's own thread, never on its dispatch
  thread, but still never wait for the widget inside it: a `probe` from there
  may wait for the very request it answers), `pressDelivered` /
  `releaseDelivered` (below), and `keepsRenderer` with `keepRenderer`, which
  keep the engine process's renderer past the widget ("Kept renderers"
  below).
- For an embedder that queues the renderer's traffic behind its own work:
  the constructor's last argument, an optional
  `std::shared_ptr<SandboxHostedWait>` ("Bounds" below: how long its platform
  may hold a request); `WidgetHost::platformDelivery()`, inside
  `onPlatformMessage` or `onPlatformRequest` (a kept renderer's too), the
  `HostedPlatformDelivery` being delivered: its `hold`, which keeps the
  message or request counted against the client's queued bytes and calls
  while a copy lives, and a request's number for the record; `onDeath(reason)`,
  the widget's death once on the host thread, right after
  `diagnostics().dead` took it and before a local renderer's `frozen`
  (its own limit, a validator cap, the client's own end of it: a lost engine
  process, a flood, a platform request left unanswered), never for the end
  the owner asked for (`destroy()`, `keepRenderer()`); and
  `platformDiag(kind, detail)`, a diagnostic of the embedder's own checks of
  that traffic, recorded as a local renderer's `onDiag` (the bounded
  `diags`, `onChange`), nothing once `destroy()` or `keepRenderer()`
  returned.
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
  `send(message)` and `request(message)` (from any thread, the glue's
  included, inside `onMessage` and inside the renderer calls the glue makes,
  but never inside the factory: the host's answer, or nullopt once the runtime
  closed or the stream ended; one waits at a time, a second waits for the
  first) towards the host, and `onMessage`, set when the renderer is built, the
  host's messages in order on the glue's thread. The framework knows no
  particular renderer. A factory that throws or returns nothing ends only that
  widget ("the widget could not be built in the engine process: …"); a
  validator bundle that does not load ends the engine process, every widget on
  its stream with it, as it ends a host whose validator is in process. The
  factory never waits for the host (no `request` inside it).
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
Neither goes on the stream. An embedder that hands the press over with its
message (`sendPlatformPress(message)`, `SandboxEngine::platformPress`) gets
both in one job on the dispatch thread, stamped at its call: the press counts
only when the channel's writer admitted the message for the open route of a
widget never kept (`SandboxHostedChannel::platform` returns that admission:
a stream's queue took the frame; false for a route closed, not attached or
ending, and for a queue past its bounds). A refused down grants nothing,
touches no haptic gate, and its `releaseDelivered()` opens no touch window;
the route's close revokes every grant. `sendPlatformPress` itself stays
`void`: the client decides. Every privileged call is then decided by the
client's code as for a remote engine: `sdk::checkBridgeCall`, the preview,
the grant, the haptic gate, the `RequestGate` with the host's visibility and
in-flight state, the copy and storage checks, the size and count bounds. No
engine frame creates a grant or opens the haptic window: a call the engine
process allowed on a tap the host never delivered gets `activation-blocked`,
a "press" in a platform message is only a message, and `PressBegan` or
`Activation` from the engine's side ends the stream as the other direction's
kinds always did.

### Kept renderers

A hosted widget's renderer can outlive its engine, its validator and its
`WidgetHost`, so an embedder that keeps a stopped widget's picture (tdesktop
destroys the host and keeps the renderer) keeps the engine process's document
behind it. Desktop core only, opt-in: a host that never keeps behaves as
before, a death included.

- `std::unique_ptr<HostedKeptRenderer> WidgetHost::keepRenderer(Observers)`,
  hosted only, from the owner's thread: it ends the widget as `destroy()`
  does (a live one dies "terminated" and `Stop` crosses, never `Close`) and
  hands its route to the handle, which outlives the `WidgetHost`; destroy
  that afterwards as usual, its inputs doing nothing. `bool keepsRenderer`
  (false), read at `start()`, also keeps the route of a widget that dies of
  its own (a CPU or memory limit, a validator cap, its error): the death the
  engine process reports leaves the route open, the renderer's traffic still
  reaching this `WidgetHost`'s `sendPlatform`, `onPlatformMessage` and
  `onPlatformRequest`, until `keepRenderer` takes it or `destroy()` closes
  it; unset, such a death closes the route at once, as before. Null, the
  widget ended as `destroy()` ends it, when there is nothing to keep: a local
  host, a host never started, destroyed or kept already, a renderer that
  never announced itself, a route that ended (the stream, a flood, a lost
  frame, a platform request unanswered or with no `onPlatformRequest`), a
  death reported without `keepsRenderer`, or a handle whose own
  `twidget-host` cannot start (nothing is thrown: a live widget dies
  "terminated" with `Stop` and `Close`, a dead one's route closes with
  `Close`, and the engine process destroys its renderer).
- The handle: `Observers{onPlatformMessage, onPlatformRequest(message,
  respond), onEnded(reason)}`, given at the keep and called in order on the
  handle's own `twidget-host` thread; `sendPlatform(message)`, `ended()`,
  `remoteBacklog()` (tests) and `release()` (idempotent, also the
  destructor's: the client is destroyed, its dispatch thread joined and
  `Close` sent, queued observer calls are dropped, then the handle's thread
  ends; from the owner's thread, never its own). Nothing else exists on it:
  no press, grant, haptic, bridge call, request, storage, theme, data, safe
  area or probe.
- The keep's boundary: everything the client handed the `WidgetHost` before
  the keep reaches that host's observers before `keepRenderer` returns, a
  request among it with a `respond` that still answers after the keep; the
  handle's observers start after that, with the rest, so each platform
  message reaches one observer, in order. The handle owns the same client, so
  a delivery the WidgetHost's observer kept (`platformDelivery()`) stays
  counted in the handle's `remoteBacklog()`, and the constructor's
  `SandboxHostedWait` goes on applying to the route; every handle observer
  call, the first ones too (which may run before `keepRenderer` returned),
  finds its own delivery through the same `platformDelivery()`.
- What crosses a kept route: only the renderer's platform traffic, both ways,
  under the live widget's bounds (below), one request waiting at a time. The
  keep clears the grants; every other kind a hosted runtime may send is taken
  and has no effect (the widget is dead: no action, haptic, copy, storage,
  request or wallet answer, validated batch or report, and a straggler
  `Died`, the engine's report of the keep's own stop, closes nothing), its
  Intl requests are answered "", and what ends a live route's stream (a kind
  a hosted runtime never sends, a second platform or Intl request, a
  malformed or oversized frame) ends it on a kept one too.
- What ends it: the host's `release()`, which the host asked for and hears
  nothing of; or, reported by `onEnded` once and last (and `ended()`), a
  flood, a platform request left unanswered until it is due (3 s, or as the
  route's `SandboxHostedWait` moved it) or arriving with no
  `onPlatformRequest` on the handle, and the end of the stream or of the
  engine process ("the widget engine process ended"). A dead route, kept or
  kept open past its death, closes with `Close` alone: its engine is gone, so
  no `Stop` goes with it.
- The engine process: every death of a hosted widget's engine (its own limit
  or the host's `Stop`) keeps the renderer until `Close`. After the death's
  reports and `frozen`, on the glue's `twidget-host`, the served host's cut
  stops the renderer's messages to the glue, the engine's queued work and
  every input but the platform traffic, answers a waiting layout query dead,
  releases the engine (`SandboxEngine::release()`: `widget.sandbox` joined and
  its QuickJS context freed, the object left for the references to it) and
  destroys the validator (`widget.validator` and its context). The renderer,
  its `SandboxHostedPlatform` and the glue's thread stay; a probe answers
  "ERROR no runtime". No frame kind is added: a host that keeps withholds
  `Close`, one that does not sends it right after the death as before, and
  `Close` destroys the runtime on `widget.reaper` (the loopback: in its
  destructor) as it always did.
- The count and the process's lifetime: `Events::runtimes` leaves kept
  routes out (the client tells its channel, `SandboxHostedChannel::kept()`),
  so an embedder that idles its engine process at 0 needs no count of its
  own. The framework neither keeps that process alive for kept renderers nor
  ends it for them: when it exits, every kept handle hears `onEnded`.
- Under it, for a delegate of your own: `SandboxEngine::startHosted(start,
  keepAtDeath)`, `keepHosted(SandboxEngineDelegate&)` (synchronous on the
  dispatch thread: a live widget's death through the old delegate and
  `stop("terminated")`, then the grants go, the new delegate hears only
  platform traffic and the channel hears `kept()`) and
  `SandboxEngineDelegate::onHostedEnded(engine, reason)`.

```cpp
host->keepsRenderer = true; // before start(): a death of its own keeps the route too
host->start();
// ...the widget stops or dies; the embedder keeps its picture:
twidget::HostedKeptRenderer::Observers observers;
observers.onPlatformMessage = [](const std::string& message) { postToRenderer(message); };
observers.onPlatformRequest = [](const std::string& message, std::function<void(std::string)> respond) { respond(measure(message)); };
observers.onEnded = [](const std::string& reason) { dropPicture(reason); };
std::unique_ptr<twidget::HostedKeptRenderer> kept = host->keepRenderer(std::move(observers));
host.reset();
if (kept) kept->sendPlatform(resizeMessage);
kept.reset(); // release(): Close, and the engine process destroys the renderer
```

### Bounds

- `PlatformMessage` and `PlatformRequest` strings are cut to
  `SandboxEngine::kRemotePlatformLimit` (32 MiB) plus one byte at both ends,
  `ValidatedOps` to `kRemoteOpsLimit` plus one, within `kMaxFrameBytes` as
  every frame: a message past its limit is a `protocol-violation`, dropped, a
  request past it is answered "" with one, validated ops past it are one too.
  Each is admitted against `kRemoteQueuedBytes` and `kRemoteQueuedCalls`
  before anything of it is copied and stays counted until `onPlatformMessage`
  returned (or the batch reached `onRenderOps`), and past that for as long as
  the embedder keeps a copy of its `WidgetHost::platformDelivery()` (a queue
  of its own, a request until it is answered): a flood kills the widget
  once ("the widget engine process sent more than the host keeps queued"),
  while the host's end goes on reading for the stream's other runtimes.
  `Nodes` is clamped to [0, `INT_MAX`]. `Platform` from the host is bound by
  the frame: a message that does not fit loses its runtime.
- One platform request waits per hosted widget; a second while the first is
  in the host's hands ends the stream (a transport that hands a client a
  second one anyway gets none for it, a `protocol-violation`). It goes to its
  runtime's `widget.answer`, and the client waits for it there, on the
  request's own caller thread (on the loopback, the engine side's caller):
  its dispatch thread only hands it to the delegate, the asynchronous
  `SandboxEngineDelegate::onHostedRequest(engine, message,
  HostedRequestResponder)`, and never waits, so neither that widget's other
  traffic nor any other widget waits behind it. Without `respond` within
  `SandboxEngine::kPlatformAnswerSeconds` (3 s) of its arrival (later only
  under the embedder's hold, below), or with no
  `onPlatformRequest` when it arrives (`respond.unanswered()`), that widget
  dies with diagnostic `platform-timeout` ("the host's platform did not
  answer the widget's renderer within 3 s", or "the host's platform answers
  no requests of the widget's renderer"): its client sends `Stop` and `Close`
  (`Close` alone for a dead widget's route, a kept one among them), the
  renderer's `request` returns nullopt, a late `respond` changes nothing, and
  the stream's other widgets go on. `destroy()` while a request waits settles
  it at once, and the widget dies "terminated"; a request that comes once the
  client closed its route, or is being destroyed, is answered none at once.
- The embedder's hold: an embedder that serves the renderer's traffic in
  order behind a meter of its own (tdesktop holds a widget's platform work
  while the widget's painting is over its share, so a request may wait more
  than 3 s for its turn) gives the hosted constructor a
  `SandboxHostedWait`, one per widget, shared with that meter in its
  process; nothing of it crosses the stream. The meter publishes its release
  (`hold(time_point)`, the end of the hold on `std::chrono::steady_clock`, at
  each charge that moves it, with nothing queued too) and its input
  exemption (`exempt(time_point)`). A request is due 3 s after its arrival
  unless the meter actually holds the widget while the request (and so the
  embedder's queue ahead of it) waits for its admission: held means a
  release still ahead and no exemption in force, at its arrival, when the
  meter publishes a later release, or from the end of an exemption that a
  release outlasts when no renewal reached it before it ended. Then it is due
  3 s after that release, and a later release moves it again. An exemption
  renewed before it ends keeps the arrival's 3 s however long the renewals
  go on (a request continuously eligible behind a 2 s debt still dies 3 s
  after its arrival, not 5 s); the waiter wakes at the exemption's end to
  read the record again, each renewal moving that wake.
  `admitted(number)` (the request's `platformDelivery().request`,
  `HostedRequestResponder::request()` under it) when the embedder takes it
  makes it due at most 3 s from then, never later than it was (a hold met
  until then counted), and nothing moves it after that. A repeated or
  earlier release, the number of another (older) request and that request's
  responder change nothing; no request is ever suspended, and one never
  answered still ends at its deadline without another call, with the same
  death. `due(number)` reads the waiting request's deadline (tests). The
  client registers each request and its first deadline before the delegate
  hears it, re-reads both at every wake under the request's own lock and
  never calls out of the record.
- A kept route has the same bounds: the frame bound and
  `kRemotePlatformLimit`, the client's queued bytes and calls (counted until
  the handle's observer returned, or its deliveries' copies went), one waiting
  request and its deadline (its client's record still applies).

### A renderer that stops answering

Every bound above is a count or the host's own deadline: a renderer that stops
answering the host (a glue thread stuck in the widget's content, an engine
process stopped) on a stream that still takes writes waits for ever, and the
stream's end is every widget's death alike. A hosting stream opened with its
owner's `SandboxHostedAnswers` (`remote_stream.h`) bounds the renderers'
answer times and says at its end what that end is to each widget. Opt-in:
without it (the default, every engine stream, the harness) the stream keeps
and times nothing, and every death is as above.

- The reading: `SandboxRemoteHostEnd::startHosting(stream, resources, build,
  events, limits, answers)`, where `answers.read(message)` is the owner's
  reading of one of its opaque platform messages, the host's or the
  renderer's: a role (`Expects` for a host's message its renderer answers with
  exactly one, in the order sent; `Answers` for the renderer's answer; `Last`
  for its last word, after which it answers nothing; `None`), for the first two
  a key (a `family` of the owner's and, for a family that numbers its messages,
  their `sequence`), and for an expected message whether its time counts only
  when it was sent visible (`whenVisible`: a frame request) or always (an input
  event). It is pure and quick: the stream calls it on the sending thread (a
  hosted client's `widget.client`) and on `widget.reader`, outside every lock,
  never for a message past `kRemotePlatformLimit`; a reading that throws and a
  role of the other side's count as `None`.
- What the stream keeps, per route: each host's `Platform` read `Expects` from
  the moment the writer admitted it (a refused one keeps nothing), on a route
  neither kept nor halted, as its key and two times, never the message. Its time
  counts unless it is `whenVisible` and its route was hidden when it was sent:
  hidden or visible as the `HostedStart`'s config says, unless a `Visible` was
  admitted before it, then as each `Visible` says in the order the writer
  admitted them with the messages; one sent before either is judged by the
  `HostedStart`'s config. A later change of visibility neither starts nor stops
  the time of a message sent before it, and a message whose time does not count
  is kept in its place all the same. Past `SandboxEngine::kRemoteQueuedCalls`
  (16384) kept on one route, the next message that expects an answer ends the
  stream instead ("more than 16384 platform messages unanswered on runtime N")
  and is not sent.
- Which answer settles which message: a renderer's `PlatformMessage` read
  `Answers` settles its route's oldest kept message when their keys are the same
  (the family, and the sequence or none for both), and nothing else: an answer of
  another family, a stale, missing or repeated sequence, a report, a role of the
  wrong side and an answer on another route settle nothing, and nothing restarts
  the oldest message's time. The renderer's `Last` and its engine's `Died`
  settle every message of the route and nothing is kept for it again; so do the
  host's keep (`SandboxHostedChannel::kept`), its `close` and the route's loss.
  A renderer's message is read only once the writer admitted its route's
  `HostedStart`, where the engine process builds the renderer: an answer or a
  last word before it settles and halts nothing (a helper cannot turn the
  account of a widget off before its delegate could hear it), while the host's
  messages sent before it are kept as any others and its engine's `Died`
  still settles them.
- The host's time is never the renderer's: from the arrival of a route's
  `PlatformRequest` until the writer handed its `PlatformAnswer` whole to the
  stream (or the renderer's next request showed that the answer arrived), no
  message of that route counts time, those sent meanwhile included, while every
  other route's time goes on; an older answer written after the renderer's next
  request came ends nothing of that request's wait. A message sent at s has used
  (t − s) − (H(t) − H(s)) of its time at t, H being the host's time owed to its
  route, the current wait's elapsed part included. A request its client answers
  with none (`remotePlatformRequest`'s nullopt: the client stopped the widget,
  or had no delegate to ask yet) would keep that wait for ever, as the renderer
  waits for an answer that never comes: it loses its route if the route is
  still live, as a host's form that does not decode does. Its client hears that
  the stream is lost after any death of its own it queued first (a platform
  request left unanswered keeps its reason), its `Close` goes out, and the
  stream and the other routes go on.
- The answer time: `SandboxEngine::kPlatformAnswerSeconds` (3 s) of the
  renderer's own time, not configurable. `widget.monitor` sleeps until the
  earliest one of any route (or the write deadline, whichever comes first), woken
  only for an earlier one, so a renderer that answers in time costs about one
  wake per 3 s, not one per message; no poll, no timer per route. When one ran
  out the stream ends ("a hosted renderer left a platform message unanswered for
  more than 3000 ms") as at any other end: every open runtime's client hears
  `remoteDisconnected`, then `Events::ended` comes, and the owner kills the
  engine process as it does after any end. A look more than 1 s after the wake
  the monitor planned (a machine that slept, which `steady_clock` counts on
  macOS, or a thread that did not run) concludes nothing and asks for a second
  look 1 s later. No look concludes before that one, and that one, however late
  it comes itself, judges each message with all the time it had by then and is
  spent: it is never asked for again in its place, and never a new full 3 s. A
  frame that would settle a message whose time ran out (its answer, the
  renderer's last word, its `Died`) ends the stream first, as a look at that
  moment would have: before the second look it settles the message, at or after
  it it cannot. The monitor and the reader take this from one pure rule,
  `SandboxRemoteHostEnd::lookAtAnswers(due, wake, recheck, now)`, the monitor all
  of it, the reader only whether the stream ends.
- What the end is to each widget (`SandboxHostedChannel::Death`): the stream's
  first end decides it once for every open route, under the connection's lock
  and before any client hears the end; a close or keep before the reader's
  fan-out changes nothing of it. At a run-out answer time the routes whose time
  ran out by then are `Stalled` and every other one `Spared`, one with a younger
  message waiting included. At any other end but the owner's `stop` (the engine
  process's end of the stream, a read, write or frame the stream cannot go on
  with, the write deadline, a queue past its bounds) the routes with a message
  kept, timed or not, keep an `Ordinary` death and the others are `Spared`, or
  every one keeps it when none had one. A route kept, closed or lost alone, or
  whose renderer halted, takes no part and keeps its own death; so does one
  whose client claimed a death of the widget's own first (a flood, a platform
  request left unanswered, claimed before its `platform-timeout` diagnostic
  reaches the delegate) or that the host stopped first (`localStop`,
  below): that claim is one step with the end, and from it nothing of the
  route is kept, timed or blamed, so that dead widget alone never ends its
  siblings' stream, and an end that finds the claimed route still open settles
  its death as that one, `Ordinary`. The owner's `stop` decides nothing; a
  widget attached to a stream that ended otherwise is `Spared`.
- How the host stops a widget of its own accord:
  `SandboxHostedChannel::localStop()`, from any thread (a live platform end
  that refuses the widget's renderer for good, on its own thread), is one step
  with the end under the connection's lock, before anything tells anyone of
  that stop. When no end took the route into its decision yet it returns none:
  the stop is the widget's own death, claimed as a flood's is, so nothing of
  the route is kept, timed or blamed from then on, a later end finds it decided
  (`decided()`) and a withdrawal reads it `Ordinary`; it returns none too when
  a death of the widget's own or the owner's withdrawal came first, which stays
  as it was. When the end came first it returns that end's decision, frozen or
  settled, and changes nothing: the end's snapshot stands, the client still
  reports that end and publishes it for `death()`, and a withdrawal waiting for
  it still hears it once. A later call finds the same winner. It credits
  nothing the renderer owes (a reply of another sequence still answers
  nothing), and it closes nothing: the host's own cleanup and its client do.
  A hosted client's counted `SandboxEngine::stop` makes the same call on its
  dispatch thread before its delegate hears the death (inline when it is
  called there): none, and the widget dies of its stop, `Ordinary`; a
  decision, and the stop yields: nothing dies in that call, and the end's job,
  which the client still runs, kills the widget with that decision ("the widget
  engine process ended"). From that call until then the client is as at the
  stop's own death, one state it keeps on its dispatch thread: nothing of the
  widget's own that it queued (a storage commit, a bridge call, a request, an
  Intl request, a report) reaches the delegate or its `IntlProvider`, its
  renderer's platform messages and requests reach the delegate no more (such
  a request answers none, with no kill), and that death is left to the end's
  job: a platform request the delegate then leaves unanswered or refuses
  claims nothing and sends no `platform-timeout`, `keepHosted` returns false,
  and a later `terminate` or `stop` does nothing. The end's job keeps the
  flood's precedence and its single publication, and a renderer kept before
  the stop never yields (its widget is dead), so its traffic goes on as
  before. Otherwise the owner's `terminate` and keep claim nothing and stay
  on the owner's path. A channel that decides nothing returns none: the
  stop comes first; so does a hosting stream opened without the owner's
  `SandboxHostedAnswers`, whose routes no stop or claim ever decides.
- How the embedder reads it: `SandboxHostedChannel::death()`, from any thread,
  is `Ordinary` until the hosted client reported the widget's first death, and
  only the transport's end publishes its decision there, on the client's
  dispatch thread before its delegate hears that death (or keeps it for a
  delegate set later, which hears it once). The end reaches a client at its
  bounds as the end (it takes no place of the engine's side's traffic, above),
  and a client past them as its flood, `Ordinary`, however the end's job and
  the flood's kill were queued.
  `death()` is a virtual whose default is `Ordinary` and the base class holds
  no state, so a channel of your own keeps its copies and moves and reports
  `Ordinary`; the stream's channel keeps the value it publishes. A death the
  widget had first (its own limit, the host's stop decided before the end, a
  flood, a platform request left unanswered, a refused start, its engine's
  `Died`, a keep) keeps `Ordinary`, whatever an end decides later, and so does
  a later end; a host's stop that comes after an end that decided the widget
  yields to it (`localStop`, above), and the widget dies of that end. So the
  embedder reads it in the hosted `WidgetHost`'s `onDeath`, through the very
  channel it gave that host's constructor, held weakly (the client owns it);
  the death's reason stays "the widget engine process ended".
- Whether it is decided yet: `SandboxHostedChannel::decided()`, from any
  thread, reads under the connection's lock and changes nothing. It is true
  once the end took the route into its decision (froze or settled its death,
  `Ordinary` included) and from then on, and once the client claimed a death
  of the widget's own first or the host stopped it first (`localStop`), unless
  the owner's withdrawal then came before any end; false before either, for a
  route the owner withdrew first and at the owner's `stop`. It tells what `death()` cannot before the client ran the
  end: an `Ordinary` death reads alike before and after its publication, and
  a `Stalled` or `Spared` one is published only when the client's dispatch
  thread gets to it. So an owner that counts deaths per widget can hold back
  another run of the same widget while one reads decided and its death is not
  reported yet. A virtual whose default is false, the base still with no
  state, so a channel of your own reads false and keeps its copies and moves;
  a hosting stream opened without the owner's `SandboxHostedAnswers` reads
  false too, whatever stops or claims the widget.
- The same, with no lock: `SandboxHostedChannel::decidedView()` is a view of
  the route's own (`std::shared_ptr<const SandboxHostedDecidedView>`, the same
  one on every call, so obtain it once) whose `decided()` reads what the
  channel's `decided()` reads. The stream keeps it in the very step, under the
  connection's lock, that changes what `decided()` reads, before anyone hears
  of that change: the client's claim of the widget's own death or the host's
  `localStop` (true), the end that froze the route or attached it to a stream
  already ending so (true), the owner's withdrawal (false, also of a claim no
  end decided); a withdrawal after the end, the settlement of a frozen death
  and the owner's `stop` change nothing. So the view reads true once the
  transition is done, though the client's delegate has not heard of it yet.
  The end freezes its routes one after another and then turns all their views
  at once, with one flag of the stream's that each of them reads: a reader
  never sees one route of that end decided and, later, another one it froze
  still undecided, so two owners that each sample their own route (two
  messages' paints) see the end in one order. The view owns no channel, route,
  connection, sink or thread, so an owner reads it from any thread as often as
  it likes (a paint, say) without a lock of the stream's, without promoting or
  releasing the channel, and after the channel went. A virtual with a default
  of none, the base still with no state, for a channel that decides nothing
  (`decided()` always false); a hosting stream opened without the owner's
  `SandboxHostedAnswers` gives one that stays false.
- How the owner leaves it: `SandboxHostedChannel::withdraw(settled)`, once, from
  any thread (a host that retires the widget, before it resets anything it
  counts), is one step with the end under the connection's lock. Before the end
  decided the route's death, the withdrawal comes first (`ended` false): the
  route takes no part in any end from now on, nothing of it is kept, timed or
  blamed, and it still carries what it carries (a kept renderer's traffic).
  That withdrawal also tells whether a death of the widget's own was claimed
  on the route before it (`Withdrawal::own`: a flood, a platform request left
  unanswered, its engine's `Died`, the host's `localStop`), read in the same
  step under the connection's lock, so an owner whose `decided()` read false
  just before still learns of a claim its client made meanwhile; only the
  route's first withdrawal tells it, nothing settles for it and `settled`
  hears nothing, and a claim that comes after the withdrawal finds the route
  withdrawn and is no claim.
  After, the widget's death is the one the end settles, once: the end's
  decision when the client's first death is that end or cleanup takes the
  route from the client first (the owner's stop, a keep, a close), `Ordinary`
  when a death of the widget's own came first (a flood, a platform request
  left unanswered, its engine's `Died`, the host's `localStop`), also one
  claimed before the end on a route the end found open. A route lost alone
  (its `widget.answer`, a form that does not encode) settles nothing itself: its client still hears that
  loss and settles the death by its own first death, an own one it queued
  before the loss included. The withdrawal reads it at once when it settled
  (`Withdrawal::death`), else `settled` hears it once, outside every lock, on a
  thread of the stream's or the client's (the dispatch thread before its
  delegate hears the death), so a death decided before the owner let the
  widget go is never lost with its callbacks and the owner's later stop cannot
  turn it into another. A channel that decides nothing has the withdrawal
  first.

```cpp
// The host: the reading of its own messages (pure: their kinds and sequences from their headers).
twidget::SandboxHostedAnswers answers;
answers.read = [](std::string_view message) { return readOwnMessage(message); };
auto end = twidget::SandboxRemoteHostEnd::startHosting(stream, *resources, "build-1", events, {}, std::move(answers));
std::shared_ptr<twidget::SandboxHostedChannel> channel = end->hostedChannel();
auto host = std::make_unique<twidget::WidgetHost>(html, theme, width, options, channel, intl);
host->onDeath = [weak = std::weak_ptr<twidget::SandboxHostedChannel>(channel)](const std::string& reason) {
    const std::shared_ptr<twidget::SandboxHostedChannel> channel = weak.lock();
    switch (channel ? channel->death() : twidget::SandboxHostedChannel::Death::Ordinary) {
    case twidget::SandboxHostedChannel::Death::Stalled: stopForGood("stopped responding"); break; // this widget hung it
    case twidget::SandboxHostedChannel::Death::Spared: restartUncounted(); break;                 // another one did, or none
    case twidget::SandboxHostedChannel::Death::Ordinary: countDeath(reason); break;
    }
};
host->start();
```

The tests, on a scripted engine process with generic keys and real hosted
clients: `remote_stream_hosted_answers_settle_the_oldest_matching_message`
(twenty-one routes judged at one end while the monitor is held, one whose
renderer's answer and last word came before its start among them),
`remote_stream_hosted_answers_leave_out_the_hosts_own_time` (requests held
past the answer time, a send inside a hold, an answer queued behind a blocked
writer, another route's time going on),
`remote_stream_hosted_answers_decide_each_death_once_at_the_end`,
`remote_stream_hosted_answers_keep_a_widgets_first_death` (the owner's
`terminate` queued behind a held dispatch thread before the end stays
`Ordinary`, a counted stop queued the same way yields to the end and is
`Spared`; a channel of your own copied and moved too),
`remote_stream_hosted_answers_end_reaches_a_full_client`
(exactly `kRemoteQueuedCalls` messages held at the end, one more, and a request
past the bound taken on `widget.answer` after the end was queued),
`remote_stream_hosted_answers_end_and_a_rejection_in_its_step` (a request past
the bound held in the middle of its rejection's step; the end's choice seen as
it takes the step's lock, then half a second with no death and its job not
done: flooded once the step goes on),
`remote_stream_hosted_answers_withdraw_in_one_step_with_the_end` (a withdrawal
before the end, with a timed message unanswered; after it, settled and still
to settle; the owner's stop and a flood ahead of the end),
`remote_stream_hosted_answers_claim_an_own_death_in_one_step_with_the_end` (a
flood held in its delegate before its route closes: with its own timed
message 3.4 s old nothing ends, and a sibling's run-out time leaves it
`Ordinary`; a loss on `widget.answer` after the end and before the reader's
fan-out, the client's flood queued first: `Ordinary`; a platform request its
delegate answers with none, the diagnostic held in that delegate and the
route's loss held on `widget.answer` across the engine process's end: claimed
first, so the idle sibling stays `Ordinary`),
`remote_stream_hosted_answers_stop_a_widget_locally_in_one_step_with_the_end`
(a reply of another sequence answers nothing and the stream ends at 3 s, but
the host's `localStop` of that widget before its time ran out, its client then
held, leaves the stream and its sibling's answers going 3.4 s on; the end
first: its stalled decision stands; an idle widget stopped before the engine
process's end is `Ordinary`, a channel of your own),
`remote_stream_hosted_answers_claim_a_counted_stop_in_one_step_with_the_end`
(a counted `stop` held in its delegate with an input unanswered: nothing ends
3.4 s on and the sibling answers; the end first, with a job ahead of it that
stops the widget inline: the end's stalled decision, one death, and a storage
commit queued behind that job reaches no delegate where the same commit before
it did; an idle widget stopped inline before the end is `Ordinary`;
`terminate` claims nothing; a stream opened without the owner's answers
decides nothing of the same stop, one opened with them does),
`remote_stream_hosted_answers_leave_a_yielded_stop_to_the_ends_kill` (the
watchdog's end with the reader held in a delivery before it tells any client,
each widget stopped after it: a keep and a terminate take nothing; platform
requests then refused, each refusal's kill queued before its open route is
lost for the missing answer: no `platform-timeout`, and each dies of the end
once, stalled or spared as the end decided; at the engine process's end, its
fan-out held before the clients, a platform message, a platform request and
an Intl request queued behind a job that stops the widget inline run while
the client is yielded and reach neither the delegate nor the `IntlProvider`,
where the same ones before it did, and the request kills nothing),
`remote_stream_hosted_answers_tell_a_decided_death_before_it_is_published`
(an end's decision read while both clients are held, an `Ordinary` and a
`Spared` one, a withdrawal before it, the owner's stop, a flood held in its
delegate, a channel of your own) and
`remote_stream_hosted_answers_lose_a_route_left_without_an_answer` (no delegate
yet, a sink with no answer, a delegate that has no platform). The late look's
rule takes no clock of the stream's:
`remote_stream_hosted_answers_look_again_once_after_a_late_wake` gives
`lookAtAnswers` its times (an ordinary wake, an answer in the second look's
time, a late look before it, a late second look, the reader after it, a later
sleep).

### Threads

- The host's process, per hosted widget: `twidget-host`, `widget.client` and,
  from its first Intl or platform request, `widget.answer`; per stream
  `widget.reader`, `widget.writer` and `widget.monitor`. There is no
  `widget.validator` and no `widget.sandbox` for a hosted widget: no QuickJS
  runtime in the host. A kept renderer has the handle's `twidget-host` (its
  observers) and its client's `widget.client` and `widget.answer`, until its
  release; a keep whose handle's thread cannot start keeps nothing ("Kept
  renderers" above).
- The engine process, per hosted widget: the served host's `twidget-host`
  (the glue and every renderer call), `widget.validator`, `widget.sandbox`,
  the renderer's own threads, and `widget.probe` from its first probe; per
  stream `serve`'s caller (the reader, which builds each widget at its start,
  the validator's construction included) and `widget.reaper`. Every library
  thread is a `JSThread`, and a widget's are made while it is built, inside
  the catch that turns a failure into that widget's death: a widget whose
  glue's `twidget-host`, `widget.validator`, renderer (a thread the factory's
  renderer starts) or `widget.sandbox` cannot start dies with why, everything
  it had started joined first, and a diagnostic its validator reported while
  it was built (`bundle-mismatch`, `bytecode`) reaches the host before that
  death. Its `widget.probe` and the stream's `widget.reaper` fail as an
  engine's do ("Threads and order" above); one stopped for its probe thread
  keeps its renderer until `Close`, as at any death of its engine. A kept
  renderer keeps the glue's `twidget-host`, its own threads and a
  `widget.probe` already made, no `widget.sandbox` and no `widget.validator`:
  `remote_stream_hosted_kept_engine_process_keeps_only_the_renderer` reads
  them in a child engine process (one of each while live, none while kept).
- The glue's thread may wait for the platform: a renderer's `request` inside
  `onMessage` or inside a renderer call the glue makes (a layout that asks
  for a text's width), live or kept, gets its answer. No thread that answers
  it ever waits for that glue thread (on a stream the reader posts the
  request and a frame of the host's answers it; on the loopback the client's
  record does, and the host thread runs `onPlatformRequest` without waiting),
  so nothing deadlocks; the engine waits for its glue only in a layout
  query (3 s), and the glue for its engine only in the cut after the
  engine's death. Such a wait holds up only that widget's glue (its layout
  answers, its other platform messages, a probe's flush), never another
  widget's, and a request left unanswered ends that widget alone once it is
  due.
- A layout query's 3 s are its glue's own time: the time the served host's
  glue thread spends inside its renderer's platform requests while the query
  waits (one the glue was in when the query came, its wait for its turn
  behind another thread's request included) is left out of them, read from
  one coherent count (`HostedPlatform`'s, under a lock of its own held for no
  call; a wait's end wakes the waiting query), and that time is left out of
  the widget's query budget and main-thread window too
  (`QueryAnswer::platformSeconds`, a timeout's too). Both count from a
  reading taken before the glue can run the query and end at the answer's
  own reading, so an engine thread that waits late, descheduled right after
  handing the query over, misses none of the glue's wait. A request of
  another thread (the renderer's own) or another widget buys no time, and a
  glue that is slow with no request of its own still times out after 3 s.
  In-process and local queries keep their 3 s unchanged.
- A renderer that keeps its `SandboxHostedPlatform` past its widget (from
  any thread) reaches neither host nor sink once the runtime closed:
  `request` answers nullopt at once, not after the turn of a request still
  under way, and the runtime stops counting and waking for its glue thread's
  waits (under the count's lock, after a wake under way returned) before it
  destroys the served host and joins that thread, so a thread later given
  that thread's id is no glue of a host that is gone.

### The lockdown

`lockdown` is called at the same point for either opening: once on `serve`'s
thread, after `HostingOpening` was accepted and its resources built, before
`Ready` and before any runtime exists; its refusals are `serve`'s. With every
bundle in the opening the engine process reads no file:
`remote_stream_hosted_engine_process_reads_no_file` serves `serveHosting` in
a child process whose lockdown sets `RLIMIT_NOFILE` to {0, 0}, and its hosted
widgets load, render and fill the tap.

### The host's side alone

A program whose `WidgetHost`s are all hosted ones links no `TrustedValidator`,
no QuickJS, no in-process engine, no local glue and no engine end. The core's
sources are divided by translation unit, never by a macro, and the public API
is unchanged:

- The host's side: `js_thread.cpp`, `sdk_policy.cpp`, `gesture_policy.cpp`,
  `intl.cpp`, `resources.cpp`, `remote_coding.cpp` (`SandboxRemoteCoding`),
  `remote_stream.cpp` (the stream's host end), `sandbox_engine_shared.cpp`
  (what every `SandboxEngine` runs: its shared constructor and destructor, the
  entry points both modes take, the clock, the value types' JSON),
  `sandbox_engine_client.cpp` (the client mode) and `widget_host.cpp` (what
  every `WidgetHost` runs, the hosted mode and `HostedKeptRenderer`).
- The rest: `engine_platform.cpp` (`twidget/engine_platform.h`: the thread CPU
  clock, the steady clock, the time zone and the CSPRNG the engine asks the
  OS for, apart from Nova, so an engine process whose end is in Rust links
  them alone), `nova_context.cpp`, `cpu_budget.cpp`, `bundle_stamp.cpp`,
  `trusted_validator.cpp`, `sandbox_engine.cpp` (the in-process engine,
  `release`, `make` and the channel factory slot), `remote_engine.cpp`
  (`SandboxRemoteServer` and the loopback), `remote_stream_engine.cpp` (the
  stream's engine end: `serve`), `remote_hosted.cpp` and
  `widget_host_local.cpp` (the local mode and the served host), with the
  private headers `remote_internal.h`, `remote_wire.h` (the frame codec both
  ends of a stream use), `steady_clock.h` (the one clock of the engine's
  deadlines and of QuickJS's interrupt handler) and `widget_host_internal.h`
  (what both `WidgetHost` sources use). A build that lists the sources by name
  lists all of them.
- `SandboxEngine` reaches the in-process halves of the entry points both modes
  take through a private table that only its in-process constructor installs.
- `WidgetHost` reaches its local glue (the six render calls and the local
  halves of `start`, `setTheme`, `setContentSafeArea`, `onLoaded`, `onDied`
  and `destroy`) through a private table that only its local and served
  constructors install; a hosted host has none, and its client never makes
  the render calls.
- The members whose type is the engine side's are deleted through a function
  their constructor installs (`SandboxEngine`'s QuickJS context, `WidgetHost`'s
  validator, which clears its CPU sink first) or are a `std::optional` the
  in-process constructor emplaces (the CPU budgets), so no host-side source
  names their destructors. `WidgetHost` stays one class (`createdAt_` a direct
  member).

CMake builds the host's side as the object library `twidget-sandbox-host`, and
`twidget-sandbox`, which every other program links as before, from the rest
plus those objects. `twidget-host-only` (`harness/host_only.cpp`) is built from
the host objects alone: a hosted `WidgetHost` that keeps its renderer, over the
`hostedChannel()` of an end `SandboxRemoteHostEnd::startHosting` opened on a
stream whose engine's end is gone (none can link there); it uses the keep API
(null: the route ended) and passes when its widget ran hosted, died "the widget
engine process ended" and kept nothing. Its link line holds the ten host
objects and its own, no other object and no `libtwquickjs.a`, and the link of
an executable leaves no symbol undefined, so no host-side source needs the
rest; its symbols hold no member of `TrustedValidator`, `QuickJSContext`,
`CpuBudget`, `BundleStamp` or the engine end's classes, no in-process engine or
local glue function and nothing of QuickJS's C API, only weak `std::`
instantiations of the type-erased members that name those types as template
arguments. The hosted suites run the same objects. An engine process that calls
only `serve` (tdesktop's helper) still links no `WidgetHost`, validator or
hosted runtime, and no longer the stream's host end: `serve` is in
`remote_stream_engine.cpp`. 2026-11-16, Debug: `twidget-host-only` exits 0;
its symbols hold none of those (`nm -C` grep in CLAUDE.md: 0 lines, 3271 in
`twidget-tests`); the suites in the five modes, `--attacks` and `--corpus
../../corpus/compat` in process and under both hosted values give what they
gave before the division, test for test and row for row.

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
same checks to its streams. 2026-11-16, Debug, with kept renderers and the
host's side divided: 214 tests, 0 failures with either value, 19 of 19
fixture widgets hosted (over the stream on 19 streams, no frame of a kind that stays in the engine process on
any, `ValidatedOps` only on the 2 whose widget asked); `--attacks` 133
attacks, 0 leaks, 7 runtimes killed, the same files and reasons as in
process, 133 hosted; `--corpus ../../corpus/compat` 19 / 19 loaded, every
row's status that of in process.

`twidget-tests hosted_` (31) runs the hosted mode's own tests: the coders, the
hosted client's grants from delivered presses and its bounds against a fake
channel, the client's kept route (only platform traffic crosses it, and the
ends it decides: a flood, an unanswered request, the transport's end), the
loopback (the whole widget in the engine's side, the tap, privileged calls on
the host's presses, platform traffic and the 3 s rule, a stopped and a dead
renderer kept, with nothing lost across the keep and one engine and one
validator thread fewer, what cannot be kept, waits on the glue's thread,
build failures), a probe behind the safe area and theme that the engine's
side's glue thread relays, over the loopback and a hosting stream, a probe
that answers when its engine's thread stops under it, and
`remote_stream_hosted_` (16): the wire against the tests' own reading of the
tables, the hosting opening and the lockdown, the engine's end serving a
widget, a real `WidgetHost` over the stream with a frame count (and an
engine-mode control that shows `Ops` and `Query`), a lying engine end written
frame by frame, hostile frames, a platform flood within the bounds, an
unanswered request that kills only its runtime among three, a stopped and a
dead renderer kept on one stream (the runtime count, one `Close` at the
release, the engine process's end), kept routes against a lying engine end,
a glue-thread wait that holds up no other widget, three child processes (an
engine end that cannot open a file, a host's thread names, no
`widget.validator` or `widget.sandbox` with three hosted widgets, and an
engine end that keeps only a kept renderer) and two more at the process's
thread limit on macOS: hosted widgets whose served threads (`twidget-host`,
`widget.validator`, the renderer's, `widget.sandbox`) or probe thread cannot
start, and a keep whose handle's thread cannot start, of a live and of a dead
widget.
