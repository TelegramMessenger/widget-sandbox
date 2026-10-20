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

- **SwiftPM (iOS, macOS)**: `.package(url: "https://github.com/TelegramMessenger/widget-sandbox.git", exact: "0.1.1")`
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
  `"@telegram/widget-sandbox": "github:TelegramMessenger/widget-sandbox#v0.1.1&path:packages/core"`;
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
after gzip and 32 KB uncompressed. Since API layer 233 it is message media of
its own: `messageMediaWidget width:int height:int payload:bytes`
(`docs/tgw-format.md` has the TL, the server's checks and the errors). The
server gzips the bot's plain HTML and enforces both limits. The client takes
`payload`, `gunzip`s it and passes the text to `WidgetHost`. Do not add size
checks of your own: the server enforces them and they can move without a
client update; the sandbox is the security boundary.

There is **no file download and no loading state for a file**: the bytes are
already in the message. The only thing to show before the first frame is the
bubble at the size the bot asked for (below) while the sandbox boots, which
takes tens of milliseconds warm and a few hundred cold. An edited message
carries a new payload: destroy the host and create a new one. A forwarded
message carries a copy of the bytes.

**Enabling.** The server has no flag for widgets. Each client turns rendering
on with its own platform/version flag (a debug or rollout flag, not app
config); with it off, and on clients below layer 233 which receive
`messageMediaUnsupported`, the message shows as unsupported media with its
caption. Keep that fallback.

**Caption and preview.** The media caption is the preview text for the chat
list, notifications and search; without one show the localized "Widget" stub,
as for other media without a caption.

**Testing.** Everything in the shared test channel is a real layer-233
message: a test bot sends each widget to a tester's private chat (bots may
send widgets only there) and the messages are forwarded into the channel; the
copy works like the original. That includes every conformance widget
(`packages/conformance`: each one fits a mini-widget), with its reference
picture posted as a photo next to it in the order of
`packages/conformance/channel.json`, the corpus widgets that fit, forward-copy
storage, bot edits and the size anchors from the bot. `.tgw` (gzip of the
HTML, rendered under the client's debug flag) is only the local development
format, for HTML from disk; it is never product delivery
(`docs/tgw-format.md`).

## Bubble sizing

`messageMediaWidget` carries two size anchors, `width` and `height`, in logical
pixels (CSS px, pt, dp); `0` means auto and each axis is independent. Every
axis of the bubble is bounded, auto included. The shared constants of v0.1.0,
the same on every client and tunable in a later release: minimum width 200,
height within **[50, 600]**.

| `width` | `height` | before the widget loads | after the first render |
|---|---|---|---|
| 0 | 0 | the default placeholder at the bubble's width | the content height the host reports, clamped to [50, 600], cached per message |
| 0 | H | a bubble of height clamp(H, 50, 600) at the bubble's width | unchanged: the fixed height stays; the reported content height is ignored |
| W | 0 | a widget W px wide, W clamped to [200, the bubble's maximum width]; the default placeholder height | the content height, clamped to [50, 600], cached per message |
| W | H | a fixed widget: width clamp(W, 200, max), height clamp(H, 50, 600) | unchanged |

Width: `0` means the widget takes the bubble's width, the widest the chat
gives a bubble; `W > 0` is a fixed-width widget, and the host is created with
that width (`WidgetHost(width:)`, `widthDp`, the web host's container). Height:
in auto mode use the height the host reports (`onChange` / `onResize`),
clamped to [50, 600], and **cache it per message**, so the bubble opens at
the right height next time the message scrolls in and the list does not jump;
a fixed height is applied from the start and the reported height does not
change it.

The hosts clamp the reported height to these bounds by default since v0.1.0
(web `MIN_HEIGHT` / `MAX_HEIGHT`, Swift `minHeight` 50 and `Options.maximumHeight`
600, Kotlin `minHeightDp` 50 / `maxHeightDp` 600, C++ [50, 600]), so a client on
defaults gets the rule; a client that needs other bounds passes its own cap.
Content taller than the bubble's height (600 in auto mode, or the fixed height)
is **hard-clipped**: it does not scroll inside the bubble, and there is no
fullscreen or "open" button in v0.1.0. The width is never changed by the
widget.

## Input

- **Text selection is the widget's choice** (since v0.1.1, `docs/sdk-v1.md`
  §2.4, §5.10). Text is not selectable by default; text the widget marks
  `user-select: text` is. Long press and double tap stay with the bubble, so on
  touch a selection inside a widget starts the way your client selects text in
  an ordinary message (its menu's Select, or its selection mode); with a mouse,
  a drag over selectable text selects it. Copy is your normal copy command.
  The bubble never lets the user start a drag of the widget's content. A
  native renderer that cannot select yet keeps the text unselectable.
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

A widget message may be forwarded to any chat, groups and channels included.
The copy works exactly like the original (isolated, no network, no messages to
the bot) and it is a different message, so **each copy has its own storage**: keep Web Storage per (account, chat, message id), pass it as the
host's `storage` at creation and keep what `onStorageCommit(area, values)`
hands you — `local` on disk (with a short delay, and at once when the widget
is hidden or destroyed and when the app goes to the background), `session` in
memory — and delete both with the message, the chat, the account and on "Clear
cache" (`docs/sdk-v1.md` §5.4). The framework enforces the 16 KB quota.

**Theme and language.** Pass the chat's theme as a `WidgetTheme` (colour
scheme, the Mini Apps palette keys as `#rrggbb`, your button's tokens, your
message text's tokens) and call `setTheme` on every change; pass the UI
language as `locale` (`docs/sdk-v1.md` §2.3, §2.4, §2.9).

- The palette is the colours of **this bubble**. Keep the widget view
  transparent over your bubble as before: the bubble you draw in `bg_color` is
  the widget's canvas, its text is drawn in `text_color`, and a background the
  widget sets on its root or body covers the whole bubble. A widget the user
  sent or forwarded sits in an outgoing bubble: pass the outgoing colours
  (fill, text, hint, link, separator).
- The text tokens (since v0.1.1) are your message text: `fontSize` in points /
  dp = CSS px, following the chat's text-size setting (call `setTheme` when the
  user changes it), `lineHeight` (unitless) and `padding` (the inset of text in
  your bubble, CSS lengths; the body's margin, so `body { margin: 0 }` in a
  widget still goes edge to edge). Without them the renderer uses the reference set
  15 / 1.35 / `8px 12px`. A widget without CSS is then drawn in your text style
  on your bubble's colour; any widget CSS wins (`docs/sdk-v1.md` §2.4).

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
let theme = WidgetTheme(colorScheme: .light, params: ["bg_color": "#ffffff", "text_color": "#000000" /* … every key, this bubble's */],
                        button: WidgetButtonTokens(radius: "10px", font: "500 15px system-ui"),
                        text: WidgetTextTokens(fontSize: 17, lineHeight: 1.3, padding: "6px 10px"))
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
val theme = WidgetTheme("light", params = mapOf("bg_color" to "#ffffff" /* … every key, this bubble's */), button = WidgetButtonTokens(radius = "10px"),
                        text = WidgetTextTokens(fontSize = 16.0, padding = "6px 10px"))
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
  theme: { colorScheme: 'light', params: { bg_color: '#ffffff' /* … every key, this bubble's */ }, button: { radius: '10px' }, text: { fontSize: 16, padding: '6px 10px' } },
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
twidget::WidgetTheme theme{"light", {{"bg_color", "#ffffff"} /* … every key, this bubble's */}, {}, {.fontSize = 14, .padding = "6px 10px"}};
twidget::WidgetHost host(html, theme, 360, options, resources, myRendererFactory);
host.onAction = [](const twidget::WidgetAction& a) { /* OpenLink: confirm, open; OpenTelegramLink; Haptic; Ready */ };
host.onStorageCommit = [](const std::string& area, const std::map<std::string, std::string>& values) {};
host.onGestureClaim = [](bool x, bool y) {};
host.start();
// host.setVisible(false); host.setTheme(darkTheme); host.destroy();
```

## The conformance set

`packages/conformance` ships with the tag: 39 self-checking widgets
(`auto/`), 20 visual widgets and 17 client-SDK widgets for the test channel
(`visual/`), all of them mini-widgets, the feature catalogue, reference images,
the posting order (`channel.json`) and the report tool. **`packages/conformance/CLIENTS.md`**
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
