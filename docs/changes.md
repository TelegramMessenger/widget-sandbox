# Changes since 862eec8 (telegram-macos)

What a host that upgrades from 862eec8 sees, per platform. "Opt-in" items change
nothing until the host lists them; "default" items apply to every host.

## Policy: `compat` (default) and `strict`

Hosts choose a policy: `SandboxConfig.policy` / `WidgetHost.Options.policy` (Swift, Kotlin), `SandboxConfig.policy` / `WidgetHostOptions.policy` (C++), `config.policy` / `WidgetHost` option `policy` (TS). `compat` is the default and keeps the limits hosts had before; `strict` is what the Telegram apps and the web demo use.

| | compat (default) | strict |
|---|---|---|
| Privileged bridge calls | forwarded as before | need a real user gesture (one click or key press on the visible widget unlocks one call; `ready` is exempt), else `activation-blocked` |
| Text nodes, string properties, Path2D data | 2 MB | 64 KB |
| Canvas string arguments | 4 MB | 64 KB |
| Complex-script text (units >= U+0590) | not budgeted | 8192 units per document (`text-budget`) |

The validator learns the policy through `__twValidator.setPolicy('strict')` (glue `__v_setPolicy`), sent only for `strict`, so glue built against an older validator bundle keeps working. The native node model reads the limits from `__twValidator.tables()`, which reflect the policy.

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

- `config.features`: `canvasWords`, `binaryOps` (native node model validates binary batches), `webAnimations` (Element.animate and the `anim` message).
- `WidgetHost.Options.assetResolver` (iOS/macOS): `icon:` images expanded to `data:image/` before validation.
- `WidgetHost.Options.sideChannels` (iOS/macOS): side channels such as `Widget3DChannel`.
- `WidgetHost.Options` (iOS/macOS) keep their old defaults (`asyncValidation` and `sharedValidatorThread` false, the widget starts on the renderer's `ready`, `parallelBoot` only prewarms the runtime). New: `startBeforeRendererReady` runs the widget while the renderer loads. Ops, queries, bridge calls, logs and diagnostics reach the main thread in the order the widget produced them in both validation modes.
- `SandboxResources.provide` (iOS/macOS): extra bundled libraries such as three.js.

## Embedding contracts

- `Widget3DBytes.borrowedPointer` points into the widget's heap and is valid only during `Widget3DSurface.call`; a backend that keeps data for later uses `copy()`. Calls and channel teardown are serialised, and a call after `destroy()` never reaches a surface.
- `Widget3DChannel` limits are per channel (`init(backend:maxSurfaces:maxBytesPerCall:)`), clamped to at most 16 surfaces and 64 MB per call.
- `SandboxResources.provide`: the provider's `source` and `bytecode` closures run outside the resources lock (they may call back into `SandboxResources`); provided bytecode is trusted and must come from the app bundle or memory, never from a writable cache.

## API

- Swift: `SandboxPolicy`, `SandboxConfig.policy`, `WidgetHost.Options.policy` and `TrustedValidator.setPolicy` are new. Additions only otherwise, with default implementations for new protocol requirements (`SandboxEngineDelegate` ops/binaryOps/anim, `Renderer.acceptsTypedOps`/`send(ops:)`). New parameters with defaults: `EnginePool.take(features:)`, `SandboxEngine.init(resources:features:)`, `WidgetHost.init(... executor:shell:)` (function references to the old signatures no longer type-check). New products `WidgetSandboxProtocol`, `CQuickJS`. Minimum macOS lowered from 13 to 10.13.
- Kotlin: `SandboxEngine.Delegate.onAnim` has a default body; Java implementers of the interface must implement it (no `-Xjvm-default`).
- C++: `SandboxEngineDelegate::onAnim` is a non-pure virtual at the end of the vtable, but new data members change the layout of `QuickJSContext`, `SandboxEngine` and `WidgetHost`: prebuilt consumers must recompile.
- TS: protocol additions only (`anims` query, `anim` messages, `trusted` on events, `gesture` on bridge and events, `features`). No exports removed, no diag kinds renamed. New diag kinds: `activation-blocked`, `text-budget`, `animation-limit`, `icon-limit`, `3d`, `3d-rejected`, `3d-stopped`.
- `LIMIT_DEFAULTS` are unchanged.
