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

v1 starts with **mini-widgets**: the widget arrives inline in the `widget`
block of the rich message as gzip of its UTF-8 HTML, at most 4 KB compressed
and 32 KB uncompressed (`docs/tgw-format.md`). The server gzips the bot's
plain HTML and enforces both limits. The client takes the bytes from the
block, `gunzip`s them and passes the text to `WidgetHost`. Do not add size
checks of your own: the limits live in the server's config and may move
without a client update; the sandbox is the security boundary.

There is **no file download and no loading state for a file**: the bytes are
already in the message. The only thing to show before the first frame is the
bubble at the size the bot asked for (below) while the sandbox boots, which
takes tens of milliseconds warm and a few hundred cold. An edited message
carries new bytes: destroy the host and create a new one. A forwarded message
carries a copy of the bytes.

Big widgets as `.tgw` documents (`application/x-tgwidget`, 128 KB of HTML,
downloaded like a sticker file) are **deferred**. For the PoC, before the
server side exists, each client keeps a **debug flag** that renders `.tgw`
documents posted to the shared test channel as widgets (off in release
builds): gunzip the document and hand the HTML to `WidgetHost` the same way,
with no size limit, because the conformance set posted there is larger than a
mini-widget.

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
- **User activation is a pointer press only.** A privileged bridge call
  (`openLink`, `sendMessage`, …) goes through only from the handler of a real
  tap or click the renderer saw (`trusted`), one call per press; no key press
  counts. A renderer behind your own `Renderer` implementation must mark its
  real input `trusted: true` or the widget never gets a gesture.
- **Scrolling.** A vertical swipe that starts on a plain widget scrolls the
  chat; one that starts on an element with `touch-action: none`, a range
  slider or the widget's own scroll area stays with the widget. The renderer
  decides and tells the host (`gesture` message); on Android `RendererWebView`
  already acts on it.
- **PoC scope: taps only, no bridge.** For the first client builds, widgets
  react to taps (and drags inside themselves) and have no bridge: ignore
  `onBridge` or answer every call with a refusal. The bridge and the SDK are
  the SDK v1 milestone (`docs/bridge-discussion.md`).

## Forwarding and state

A widget message may be forwarded anywhere, groups and channels included;
the copy works exactly like the original (isolated, no network, no messages
to the bot). Each copy is a different message, so **each copy has its own
storage** when widget state ships (`docs/widget-state.md`, v1 design); nothing
is shared between copies or between widgets, and v0.1.0 persists nothing.

## iOS and macOS (Swift package)

Add the repo as a package dependency (the manifest is the repo-root
`Package.swift`; a local checkout works with a path dependency), product
`WidgetSandbox`. iOS 16 / macOS 10.13 (the demo apps need macOS 13).

```swift
import WidgetSandbox

var options = WidgetHost.Options()                  // policy .strict, features [] by default
options.features = ["webAnimations"]
let host = try WidgetHost(html: html, theme: "light", width: 360, options: options)
host.onChange = { d in /* d.height, d.loaded, d.dead, d.lastError, d.stats */ }
host.onBridge = { method, argsJson in /* widget → app calls; PoC: ignore */ }
bubble.addSubview(host.view)          // UIView on iOS, NSView on macOS; its height follows the widget
// later:
host.setVisible(false)                // off screen
host.setTheme("dark")
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

val options = WidgetHost.Options(features = setOf("webAnimations"))   // policy "strict" by default
val host = WidgetHost(context, html, theme = "light", widthDp = 360.0, options = options)
host.onChange = { d -> /* d.height, d.loaded, d.dead, d.lastError */ }
host.onBridge = { method, argsJson -> }
bubble.addView(host.view)             // a View whose height follows the widget (minHeightDp / maxHeightDp in Options)
host.setVisible(false); host.setTheme("dark"); host.destroy()
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
  theme: 'light',
  features: ['webAnimations'],     // policy 'strict' by default
  rendererFactory: ShadowDomRenderer.factory,
  onResize: (h) => {},            // CSS px
  onBridge: (method, args) => {},
  onDead: (reason) => {},
});
host.setVisible(false); host.setTheme('dark'); host.destroy();
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
twidget::WidgetHost host(html, "light", 360, options, resources, myRendererFactory);
host.start();
// host.setVisible(false); host.setTheme("dark"); host.destroy();
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

- Decide what a bridge call means: `onBridge(method, args)` is the widget
  asking the app for something; the app validates and answers (or ignores).
- Persist anything, or share anything between widgets: each one is a fresh
  runtime with its own limits.
- Load fonts, media or remote images: `data:` images only (`docs/decisions.md`).
- Check the payload's size or origin: the server gzips it and enforces the
  limits, the client trusts it and sandboxes the content.
