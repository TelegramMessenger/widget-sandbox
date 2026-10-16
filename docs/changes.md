# Changes since 862eec8 (telegram-macos)

What a host that upgrades from 862eec8 sees, per platform. "Opt-in" items change
nothing until the host lists them; "default" items apply to every host.

## Policy: `strict` (default) and `compat`

Hosts choose a policy: `SandboxConfig.policy` / `WidgetHost.Options.policy` (Swift, Kotlin), `SandboxConfig.policy` / `WidgetHostOptions.policy` (C++), `config.policy` / `WidgetHost` option `policy` (TS). **`strict` is the default on every host** (it used to be `compat`); `compat` keeps the limits hosts had before the hardening and is meant for tests and tooling. Kotlin and C++ take a string: anything but exactly `"compat"` is strict.

| | strict (default) | compat |
|---|---|---|
| Privileged bridge calls | need a real user gesture (one click or Enter/Space press on the visible widget unlocks one call; `ready` is exempt; only renderer events marked `trusted` count), else `activation-blocked` | forwarded as before |
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
| QuickJS is the TelegramMessenger/quickjs fork as a git submodule on every platform | all | none | SwiftPM and Xcode clone it for git dependencies; registry/zip copies without submodules fail to build. Web uses `packages/quickjs-wasm` (workspace package, committed build) instead of the npm variant. |

Under `strict` also: complex-script text changes draw from 8192 words refilling at 1024 per second (`text-churn`), all platforms.

## Opt-in (nothing changes until the host lists them)

- `config.features`: `canvasWords`, `binaryOps` (native node model validates binary batches), `webAnimations` (Element.animate and the `anim` message). Android (`SandboxConfig.features`, `WidgetHost.Options.features`) and the web (`WidgetHostOptions.features`, plus `libraries`) take them now; every glue drops what it does not implement (see the table below).
- `WidgetHost.Options.assetResolver` (iOS/macOS): `icon:` images expanded to `data:image/` before validation.
- `WidgetHost.Options.sideChannels` (iOS/macOS): side channels such as `Widget3DChannel`.
- `WidgetHost.Options` (iOS/macOS) keep their old defaults (`asyncValidation` and `sharedValidatorThread` false, the widget starts on the renderer's `ready`, `parallelBoot` only prewarms the runtime). New: `startBeforeRendererReady` runs the widget while the renderer loads. Ops, queries, bridge calls, logs and diagnostics reach the main thread in the order the widget produced them in both validation modes.
- `SandboxResources.provide` (iOS/macOS): extra bundled libraries such as three.js.

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
| Extra bundled libraries (`SandboxResources.provide`) | yes | yes | yes (`libraries`) | no | no |
| Engine/renderer/validator pools, `WidgetFeed`, `WidgetBubble`, typed ops, WebKit view reuse | yes | — | worker pool only | validator pool only | no |

## Hardening before main (2026-10-16)

- A canvas word payload may be referenced once per batch; a binary batch that repeats a key is rejected whole.
- `anim` keyframe values may not be named `offset`, `easing`, `composite` or `computed-offset`; renderers keep animation state per widget and cancel an animation whose update fails; the web worker forwards `anim` only with `webAnimations`.
- The native node model rejects non-finite `prop` numbers like the script validator.
- `completeEditorCommand` is accepted only on a `<textarea>` with integer arguments.
- `Widget3DChannel` exposes `__tw3d` only in `DEBUG` builds (or with `TWIDGET_EXPERIMENTAL_3D`); release builds report a `3d` diag instead.
- The desktop `json.h` prints the shortest round-trip number on every target and parses numbers in the C locale.
- Gestures: every host counts only renderer events marked `trusted`; a key gesture is a press of Enter or Space, and the click it activates (`detail` 0) is the same gesture.
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
- Kotlin: `SandboxEngine.Delegate.onAnim` has a default body; Java implementers of the interface must implement it (no `-Xjvm-default`). `GestureCounter.gestureFor` takes the event's `detail` as a new last parameter (default 0).
- C++: `SandboxEngineDelegate::onAnim` is a non-pure virtual at the end of the vtable, but new data members change the layout of `QuickJSContext`, `SandboxEngine` and `WidgetHost`: prebuilt consumers must recompile. New class `GestureCounter` (the host's gesture numbering); `WidgetHost::isUserActivation` forwards to it.
- TS: protocol additions only (`anims` query, `anim` messages, `trusted` on events, `gesture` on bridge and events, `features`). No exports removed, no diag kinds renamed. New diag kinds: `activation-blocked`, `text-budget`, `animation-limit`, `icon-limit`, `3d`, `3d-rejected`, `3d-stopped`.
- `LIMIT_DEFAULTS` are unchanged.
