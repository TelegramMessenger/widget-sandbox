# Integrating the sandbox into a client

One `WidgetHost` per widget, on every platform: it owns the QuickJS engine
(on its own thread), the validator and the renderer, gives you a view to put
in the bubble, and reports height, diagnostics and bridge calls through
callbacks. The HTML you pass is the whole widget (inline CSS and JS); nothing
in it can reach the network.

The same `Options` exist everywhere with the same defaults (`memoryLimitBytes`
64 MB, `cpuLimitMs` 500, `cpuQuotaMs` 300 per `cpuQuotaWindowMs` 1000,
`maxTimers` 256, `visible`), see `packages/core/src/shared/protocol.ts`.
Call `setVisible(false)` when the bubble leaves the screen (frames stop, timers
drop to 1 Hz), `setTheme` when the chat theme changes, `destroy()` when the
bubble is recycled. A widget that exceeds its limits is killed; the renderer
keeps the last frame with a "Widget stopped" badge (`stoppedLabel`) and `onDead`
fires — destroy or re-create, your choice.

## iOS and macOS (Swift package)

Add the repo as a package dependency (the manifest is the repo-root
`Package.swift`; a local checkout works with a path dependency), product
`WidgetSandbox`. iOS 16 / macOS 13.

```swift
import WidgetSandbox

let host = try WidgetHost(html: html, theme: "light", width: 360)   // Options() by default
host.onChange = { d in /* d.height, d.loaded, d.dead, d.lastError, d.stats */ }
host.onBridge = { method, argsJson in /* widget → app calls */ }
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
same with a checkout, or takes the AAR from a Maven repository once one is
published. minSdk 26; the AAR carries the quickjs-ng native library for
arm64-v8a and x86_64 and the JS bundles as assets.

```kotlin
import org.telegram.widgetsandbox.WidgetHost

val host = WidgetHost(context, html, theme = "light", widthDp = 360.0)   // Options() by default
host.onChange = { d -> /* d.height, d.loaded, d.dead, d.lastError */ }
host.onBridge = { method, argsJson -> }
bubble.addView(host.view)             // a View whose height follows the widget (minHeightDp / maxHeightDp in Options)
host.setVisible(false); host.setTheme("dark"); host.destroy()
```

`SandboxResources.get(context)` loads the bundles from the library's assets
once per process. `Options.pools` / `sharedValidatorThread` are the list
optimisations ported so far (`docs/android-renderer-bench.md`).

## Web (npm package)

`@telegram/widget-sandbox` (a pnpm workspace package here; publish it or
depend on the checkout). The host runs the widget in a Worker with QuickJS
compiled to wasm and renders into a sandboxed `srcdoc` iframe. Two
requirements on the embedding page:

- the page must be **cross-origin isolated** (`Cross-Origin-Opener-Policy:
  same-origin`, `Cross-Origin-Embedder-Policy: require-corp`) — the sync
  query bridge is a `SharedArrayBuffer`; the Vite plugin sets the headers in
  dev, your server must send them in production;
- the two in-page bundles are built from the package's sources by its Vite
  plugin (`virtual:sandbox-bundle`, `virtual:renderer-bundle`), so the host
  must be bundled with Vite + `widgetSandbox()` from `@telegram/widget-sandbox/vite`.

```ts
// vite.config.ts
import { widgetSandbox } from '@telegram/widget-sandbox/vite';
export default { plugins: [widgetSandbox()] };

// in the app
import { WidgetHost } from '@telegram/widget-sandbox/host/widget-host';
const host = new WidgetHost(bubbleElement, html, {
  theme: 'light',
  onResize: (h) => {},            // CSS px
  onBridge: (method, args) => {},
  onDead: (reason) => {},
});
host.setVisible(false); host.setTheme('dark'); host.destroy();
```

`workerPool` (a `WorkerPool` of warmed workers) and the Shadow DOM renderer
(`rendererFactory: ShadowDomRenderer.factory`, prototype, relaxes the
per-widget frame) are the options for a feed of many bubbles;
`docs/chat-feed-web.md` and `docs/shadow-dom-renderer.md`.

## Desktop (C++ core)

`packages/desktop` is a CMake project (`add_subdirectory` or `FetchContent`),
targets `twidget-sandbox` (+ `twquickjs`), C++20, no Qt. It has no renderer of
its own: you implement `twidget::Renderer` (`renderer.h`) over the widget
toolkit you have, or go one level down and drive `SandboxEngine` with your own
delegate. `packages/desktop/README.md` § Embedding has the threading rules
(engine callbacks arrive on the engine thread; `onQuery` is synchronous).

```cpp
#include <twidget/widget_host.h>
auto resources = twidget::SandboxResources::fromDirectory("<path to WidgetJS>");  // the three bundles + libraries.json
twidget::WidgetHost host(html, "light", 360, twidget::WidgetHostOptions{}, resources, myRendererFactory);
host.start();
// host.setVisible(false); host.setTheme("dark"); host.destroy();
```

## What the host does not do

- Decide what a bridge call means: `onBridge(method, args)` is the widget
  asking the app for something; the app validates and answers (or ignores).
- Persist anything, or share anything between widgets: each one is a fresh
  runtime with its own limits.
- Load fonts, media or remote images: `data:` images only (`docs/decisions.md`).
