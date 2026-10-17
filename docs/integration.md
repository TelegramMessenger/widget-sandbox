# Integrating the sandbox into a client

One `WidgetHost` per widget, on every platform: it owns the QuickJS engine
(on its own thread), the validator and the renderer, gives you a view to put
in the bubble, and reports height, diagnostics and bridge calls through
callbacks. The HTML you pass is the whole widget (inline CSS and JS); nothing
in it can reach the network.

The same `Options` exist everywhere with the same defaults (`memoryLimitBytes`
64 MB, `cpuLimitMs` 500, `cpuQuotaMs` 300 per `cpuQuotaWindowMs` 1000,
`maxTimers` 256, `visible`, `policy` `strict`), see `packages/core/src/shared/protocol.ts`.
Call `setVisible(false)` when the bubble leaves the screen (frames stop, timers
drop to 1 Hz), `setTheme` when the chat theme changes, `destroy()` when the
bubble is recycled. A widget that exceeds its limits is killed; the renderer
keeps the last frame with a "Widget stopped" badge (`stoppedLabel`) and `onDead`
fires — destroy or re-create, your choice.

The rest of this page is in the order a client team meets it: pinning a
release, the policy and features, the widget payload, bubble sizing, input,
forwarding, then the per-platform API and the conformance set.

## Pinning a release

Releases are annotated tags on this repository (`v0.1.0` is the first). Pin
the tag, never a branch: `main` moves, and the JS bundles, the native
bytecode and the QuickJS submodule change together, so a mixed checkout does
not boot.

- **SwiftPM (iOS, macOS)**: `.package(url: "https://github.com/TelegramMessenger/widget-sandbox.git", exact: "0.1.0")`
  (SwiftPM reads the `v` prefix of the tag). The manifest is the repo-root
  `Package.swift`; SwiftPM clones the QuickJS submodule itself. A registry or
  zip copy without the submodule does not build.
- **Git submodule (Bazel, CMake, tdesktop)**: add the repository as a submodule
  at the tag and `git submodule update --init --recursive` (the sandbox has its
  own submodule, `packages/ios/Sources/CQuickJS/quickjs`). The C++ core is
  `add_subdirectory(<submodule>/packages/desktop)`.
- **Gradle (Android)**: a checkout at the tag and `includeBuild("<checkout>/packages/android")`
  in `settings.gradle.kts`, then depend on `org.telegram.widgetsandbox:widget-sandbox`
  as `apps/android` does. The AAR carries the native library (arm64-v8a, x86_64)
  and the bundles as assets; a Maven publication comes later.
- **npm (web)**: `@telegram/widget-sandbox` lives in `packages/core`. With pnpm:
  `"@telegram/widget-sandbox": "github:TelegramMessenger/widget-sandbox#v0.1.0&path:packages/core"`;
  with another package manager, a checkout at the tag and a `file:` / workspace
  dependency on `packages/core`. The package is not on the npm registry yet.

What you get at a tag is one state of the framework: the three JS bundles, the
`.qbc` bytecode next to them, the QuickJS submodule they were compiled with,
`packages/quickjs-wasm/dist` for the web, and `packages/conformance` (below).
Never mix files from two tags.

## Policy and features

**`strict` is the default on every host.** Privileged bridge calls need a real
user gesture, text is budgeted, `data-*`/`aria-*` and CSS strings included, and
the limits of `docs/changes.md` apply. `compat` exists for tests and tooling;
a product client never sets it (Kotlin and C++ take a string: anything but
exactly `"compat"` is strict).

**Features** are opt-in wire forms the host advertises to the runtime; the
runtime uses none it did not list. The option has the same name everywhere:

| | option | type |
|---|---|---|
| Swift | `WidgetHost.Options.features` | `Set<String>` |
| Kotlin | `WidgetHost.Options.features` | `Set<String>` |
| C++ | `WidgetHostOptions.features` | `std::vector<std::string>` |
| TypeScript | `WidgetHost` option `features` | `string[]` |

The profile sets them, not the client: today that is `["webAnimations"]` on
every platform (Web Animations records instead of per-frame style writes).
`canvasWords`, `binaryOps` and `morphUpdates` are Swift-only optimisations of
the same validated output (`docs/changes.md`, "Swift-only"); list them only on
iOS/macOS and only after measuring. A feature the renderer behind your
`Renderer` seam cannot draw must not be listed.

## The widget payload

A **widget is mini-HTML**: one self-contained HTML document of at most 4 KB
after gzip and 32 KB uncompressed, delivered right in the message
(`docs/tgw-format.md`). The server gzips the bot's plain HTML and enforces
both limits. The client takes the bytes from the message, `gunzip`s them and
passes the text to `WidgetHost`. Do not add size checks of your own: the
limits live in the server's config and may move without a client update; the
sandbox is the security boundary.

There is **no file download and no loading state for a file**: the bytes are
already in the message. The only thing to show before the first frame is the
bubble at the size the bot asked for (below) while the sandbox boots, which
takes tens of milliseconds warm and a few hundred cold. An edited message
carries new bytes: destroy the host and create a new one.

How the bytes sit in the message, who may send widgets and where, what
forwarding does and what clients without widget support show are under
revision; none of it changes the host API.

For the PoC, before the server side exists, each client keeps a **debug
flag** that renders `.tgw` documents (gzip of a widget's HTML) posted to the
shared test channel as widgets, off in release builds: gunzip the document and
hand the HTML to `WidgetHost` the same way, with no size limit, because the
conformance set posted there is larger than a mini-widget. `.tgw` is the
test-channel and development format only.

## Bubble sizing

The bot may send two optional integers with the message, `width` and
`height`, in logical pixels (CSS px, pt, dp). The client decides the bubble
geometry before the widget has loaded:

| `width` | `height` | bubble |
|---|---|---|
| > 0 | > 0 | the bubble keeps the aspect ratio `width : height` at the bubble width |
| 0 | H | a fixed height of H at the bubble width |
| absent | absent | the default placeholder height |

Then the widget loads and reports its real height (`onChange` / `onResize`):
animate to it, keep it, and **cache it per message**, so the bubble opens at
the right height next time the message scrolls in and the list does not jump.
The host already clamps the reported height (`minHeight` 24, `maximumHeight`
/ `maxHeightDp` 900 by default; the web client passes its own cap); set the
cap to what your chat list tolerates. The cap is a **hard clip**: a widget
taller than it is cut, it does not scroll inside the bubble, and there is no
fullscreen or "open" button in v0.1.0. The height is the only size a widget
controls; the width is always the bubble's.

## Input

- **No text selection.** A widget is a drawing, not a text message: the
  client's bubble must not let the user select or copy its text or start a
  drag from it (the web renderers do not disable it yet, `docs/changes.md`
  known issues; native containers do it in the view).
- **No keyboard input, no focus.** Widgets get no key events and never take
  keyboard focus (`docs/decisions.md`, "Profile v1"). The chat's input field
  keeps the focus while a widget is on screen; a widget's `focus()` must not
  move it (one of the known issues of v0.1.0 on the web, see below) and the
  conformance set checks `chatFocused` for exactly this.
- **User activation is a pointer press only.** A link call (`openLink`,
  `openTelegramLink`, a tapped `<a href>`) goes through only from the handler of a real
  tap or click the renderer saw (`trusted`), one call per press; no key press
  counts. A renderer behind your own `Renderer` implementation must mark its
  real input `trusted: true` or the widget never gets a gesture.
- **Drags.** On every primary pointer press the host reports `onGestureClaim(x, y)`:
  whether the widget takes horizontal (`x`) and vertical (`y`) drags of this
  touch, from `touch-action` and the widget's `preventDefault`. Keep
  swipe-to-reply off a touch with `x`, the list still on one with `y`; with
  neither the touch is the chat's. Double tap and long press stay with the
  bubble (`docs/sdk-v1.md` §5.5). On Android `RendererWebView` already applies
  `y` to its parent.
- **What the widget asks for** arrives as `onAction`, checked: `openLink(url)`
  (show a confirmation with the full URL, then the browser),
  `openTelegramLink(url)` (your own t.me handling), `haptic(…)` (play it or
  ignore it on desktop and web), `ready`. `docs/sdk-v1.md` §5 is the client's
  side of each.

## Forwarding and state

Whether and where a widget message can be forwarded is under revision. Where
a copy exists it works exactly like the original (isolated, no network, no
messages to the bot) and it is a different message, so **each copy has its own
storage**: keep Web Storage per (account, chat, message id), pass it as the
host's `storage` at creation and keep what `onStorageCommit(area, values)`
hands you — `local` on disk (with a short delay, and at once when the widget
is hidden or destroyed and when the app goes to the background), `session` in
memory — and delete both with the message, the chat, the account and on "Clear
cache" (`docs/sdk-v1.md` §5.4). The framework enforces the 16 KB quota.

**Theme and language.** Pass the chat's theme as a `WidgetTheme` (colour
scheme, the Mini Apps palette keys as `#rrggbb`, your button's tokens) and call
`setTheme` on every change; pass the UI language as `locale`
(`docs/sdk-v1.md` §2.3, §2.4, §2.9).

## iOS and macOS (Swift package)

Add the repo as a package dependency (the manifest is the repo-root
`Package.swift`; a local checkout works with a path dependency), product
`WidgetSandbox`. iOS 16 / macOS 10.13 (the demo apps need macOS 13).

```swift
import WidgetSandbox

var options = WidgetHost.Options()                  // policy .strict, features [] by default
options.features = ["webAnimations"]
options.locale = "ru"
options.storage = WidgetStorage(local: savedLocal, session: savedSession)   // this message's Web Storage
let theme = WidgetTheme(colorScheme: .light, params: ["bg_color": "#ffffff", "text_color": "#000000" /* … every key */],
                        button: WidgetButtonTokens(radius: "10px", font: "500 15px system-ui"))
let host = try WidgetHost(html: html, theme: theme, width: 360, options: options)
host.onChange = { d in /* d.height, d.loaded, d.dead, d.lastError, d.stats */ }
host.onAction = { action in /* .openLink(url): confirm, open; .openTelegramLink; .haptic; .ready */ }
host.onStorageCommit = { area, values in /* keep values as the new content of that area */ }
host.onGestureClaim = { claim in /* claim.x: no swipe-to-reply for this touch; claim.y: hold the list */ }
bubble.addSubview(host.view)          // UIView on iOS, NSView on macOS; its height follows the widget
// later:
host.setVisible(false)                // off screen
host.setTheme(darkTheme)
host.destroy()
```

SwiftUI: `WidgetView(host:)` wraps the view, `WidgetModel` is an
`ObservableObject` that owns a host and publishes its diagnostics.

For a list of many bubbles, `WidgetHost.Options` has `sharedWebKit`, `pools`,
`parallelBoot`, `asyncValidation`, `sharedValidatorThread`; `docs/chat-feed-ios.md`
explains what each buys and `apps/ios/DemoApp/Sources/ChatFeed` is a working
list.

## Android (Gradle library)

Coordinates `org.telegram.widgetsandbox:widget-sandbox`. Inside this repo
`apps/android` takes them from `packages/android` through
`includeBuild("../../packages/android")`; a client outside the repo does the
same with a checkout at the tag, or takes the AAR from a Maven repository once
one is published. minSdk 26; the AAR carries the QuickJS native library for
arm64-v8a and x86_64 and the JS bundles as assets.

```kotlin
import org.telegram.widgetsandbox.WidgetHost

val options = WidgetHost.Options(features = setOf("webAnimations"),   // policy "strict" by default
    locale = "ru", storage = WidgetStorage(local = savedLocal, session = savedSession))
val theme = WidgetTheme("light", params = mapOf("bg_color" to "#ffffff" /* … every key */), button = WidgetButtonTokens(radius = "10px"))
val host = WidgetHost(context, html, theme, widthDp = 360.0, options = options)
host.onChange = { d -> /* d.height, d.loaded, d.dead, d.lastError */ }
host.onAction = { action -> /* WidgetAction.OpenLink(url): confirm, open; OpenTelegramLink; Haptic; Ready */ }
host.onStorageCommit = { area, values -> /* "local" | "session": keep values */ }
host.onGestureClaim = { x, y -> }
bubble.addView(host.view)             // a View whose height follows the widget (minHeightDp / maxHeightDp in Options)
host.setVisible(false); host.setTheme(darkTheme); host.destroy()
```

`SandboxResources.get(context)` loads the bundles from the library's assets
once per process. `Options.pools` / `sharedValidatorThread` are the list
optimisations ported so far (`docs/android-renderer-bench.md`).

## Web (npm package)

`@telegram/widget-sandbox` (a pnpm workspace package here; pin it as above).
The host runs the widget in a Worker with QuickJS compiled to wasm and renders
through the Shadow DOM renderer (the product renderer, `docs/shadow-dom-renderer.md`)
or into a sandboxed `srcdoc` iframe (the dev app's default and the reference).
Two requirements on the embedding page:

- the page must be **cross-origin isolated** (`Cross-Origin-Opener-Policy:
  same-origin`, `Cross-Origin-Embedder-Policy: require-corp`) — the sync
  query bridge is a `SharedArrayBuffer`; the Vite plugin sets the headers in
  dev, your server must send them in production — and, for the Shadow DOM
  renderer, the page's own CSP must close the network as `docs/decisions.md`
  ("The web renders through Shadow DOM") lists;
- the two in-page bundles are built from the package's sources by its Vite
  plugin (`virtual:sandbox-bundle`, `virtual:renderer-bundle`), so the host
  must be bundled with Vite + `widgetSandbox()` from `@telegram/widget-sandbox/vite`.

```ts
// vite.config.ts
import { widgetSandbox } from '@telegram/widget-sandbox/vite';
export default { plugins: [widgetSandbox()] };

// in the app
import { WidgetHost } from '@telegram/widget-sandbox/host/widget-host';
import { ShadowDomRenderer } from '@telegram/widget-sandbox/host/shadow-renderer';
const host = new WidgetHost(bubbleElement, html, {
  theme: { colorScheme: 'light', params: { bg_color: '#ffffff' /* … every key */ }, button: { radius: '10px' } },
  locale: 'ru',
  storage: { local: savedLocal, session: savedSession },   // this message's Web Storage
  features: ['webAnimations'],     // policy 'strict' by default
  rendererFactory: ShadowDomRenderer.factory,
  onResize: (h) => {},            // CSS px
  onAction: (action) => {},       // { kind: 'openLink', url } | 'openTelegramLink' | 'haptic' | 'ready'
  onStorageCommit: (area, values) => {},
  onGestureClaim: ({ x, y }) => {},
  onDead: (reason) => {},
});
host.setVisible(false); host.setTheme(darkTheme); host.destroy();
```

`workerPool` (a `WorkerPool` of warmed workers) is the option for a feed of
many bubbles; `docs/chat-feed-web.md`.

## Desktop (C++ core)

`packages/desktop` is a CMake project (`add_subdirectory` or `FetchContent`),
targets `twidget-sandbox` (+ `twquickjs`), C++20, no Qt. It has no renderer of
its own: you implement `twidget::Renderer` (`renderer.h`) over the widget
toolkit you have, or go one level down and drive `SandboxEngine` with your own
delegate. `packages/desktop/README.md` § Embedding has the threading rules
(engine callbacks arrive on the engine thread; `onQuery` is synchronous).

```cpp
#include <twidget/widget_host.h>
auto resources = twidget::SandboxResources::fromDirectory("<path to WidgetJS>");  // the three bundles and their bytecode
twidget::WidgetHostOptions options;            // policy "strict" by default
options.features = {"webAnimations"};
options.locale = "ru";
options.storage = {savedLocal, savedSession};  // this message's Web Storage
twidget::WidgetTheme theme{"light", {{"bg_color", "#ffffff"} /* … every key */}, {}};
twidget::WidgetHost host(html, theme, 360, options, resources, myRendererFactory);
host.onAction = [](const twidget::WidgetAction& a) { /* OpenLink: confirm, open; OpenTelegramLink; Haptic; Ready */ };
host.onStorageCommit = [](const std::string& area, const std::map<std::string, std::string>& values) {};
host.onGestureClaim = [](bool x, bool y) {};
host.start();
// host.setVisible(false); host.setTheme(darkTheme); host.destroy();
```

## The conformance set

`packages/conformance` ships with the tag: 21 self-checking widgets
(`auto/`), 13 visual widgets for the test channel (`visual/`), the feature
catalogue, reference images and the report tool. **`packages/conformance/CLIENTS.md`**
is the step-by-step guide for a client team: configure the host as in
production, load each widget, read its result from the validated ops, perform
the scripted taps, crop the stages, and run `node packages/conformance/tools/report.mjs`
to get the per-renderer report that is compared across clients. Run it at
every framework update; `catalogue.json` `version` is the profile version.

## What the host does not do

- Open a link, play a haptic or show a dialog: `onAction` is the widget
  asking, already checked; the app confirms and does it (or ignores it). The
  widget is never told the outcome.
- Persist anything, or share anything between widgets: the framework hands
  you each storage commit, the app stores it; each widget is a fresh runtime
  with its own limits.
- Load fonts, media or remote images: `data:` images only (`docs/decisions.md`).
- Check the payload's size or origin: the server gzips it and enforces the
  limits, the client trusts it and sandboxes the content.
