# widget-sandbox — no-network sandbox for untrusted HTML widgets

Prototype of "variant B" from [`docs/overview.md`](docs/overview.md): widget
JavaScript runs **only inside QuickJS (WASM) in a Worker**, against a DOM shim.
Mutations are serialized, validated by trusted host code, and applied by a
renderer (sandboxed `srcdoc` iframe, strict CSP) that executes only our own
code. The widget can never cause a network request from the device.

`twidget` is the project's working name and survives as an internal codename
(the `twidget-run` / `twidget-tests` binaries, the `TwidgetDemo` apps, the
`tw*` prefixes); the repo and the packages a client depends on are `widget-sandbox` (`org.telegram.widgetsandbox` where a hyphen is not allowed).

```
Host page (trusted, cross-origin isolated)
 ├─ Worker: one QuickJS runtime per widget — widget scripts + linkedom + shim (untrusted)
 │     ── ops, canvas commands, sync queries ──▶ host
 │     ◀── events, timer/frame ticks, query results ── host
 ├─ Validator / sanitizer (packages/core/src/host/validator.ts, css-sanitizer.ts, url-policy.ts)
 └─ Renderer: <iframe sandbox="allow-scripts" srcdoc> with nonce CSP (packages/core/src/renderer/)
```

## Prerequisites

Everything is built from this repo; nothing is fetched at run time. What you
need depends on which part you touch:

| To work on | Install |
|---|---|
| the web package, the dev app, the Playwright suites | Node ≥ 22 with corepack (`corepack pnpm` — the repo pins pnpm in `package.json`); then `corepack pnpm exec playwright install chromium` once for the browser suites |
| `packages/ios` / `apps/ios`, `apps/macos` | Xcode 26 (iOS 16 / macOS 13 deployment targets); [xcodegen](https://github.com/yonaskolb/XcodeGen) to regenerate the demo projects; a C compiler is already there for `qjsc` |
| `packages/android` / `apps/android` | JDK 21, Android SDK with platform 35, NDK 28.2 and cmake 3.31 (`local.properties` or `ANDROID_HOME`); Gradle comes from the wrapper |
| `packages/desktop` | cmake ≥ 3.22, ninja, a C++20 compiler |
| the renderer prototypes (`apps/renderer-blitz`, `apps/renderer-litehtml`) — optional, dev tooling | rustup's cargo (+ `cargo-ndk` for Android, `wasm-bindgen-cli` for the web build); cmake + ninja (+ emscripten for the litehtml web build) |

The native packages ship the JS bundles and their bytecode, so a client
needs none of the Node tooling; only changing `packages/core/src` requires
`pnpm build:<platform>` (which compiles `packages/tools/qjsc` from the vendored
quickjs-ng on first use, into `.cache/`).

## Getting started

```bash
corepack pnpm install
corepack pnpm dev                       # dev app on http://127.0.0.1:5173 — pick a corpus file, drop any .html
corepack pnpm typecheck && corepack pnpm test:unit
corepack pnpm test:attacks              # the security suite: 93 files, 0 canary hits
```

Native: `xcodebuild test -scheme WidgetSandbox -destination 'platform=macOS'`
for the Swift package; `cd packages/android && ./gradlew :widget-sandbox:assembleDebug`
(+ `connectedDebugAndroidTest` with an emulator); `cd packages/desktop && cmake -S . -B build -G Ninja && cmake --build build && ./build/twidget-tests`.
The demo apps are in `apps/`, each README has the commands.

To put the sandbox into your own client, read [`docs/integration.md`](docs/integration.md).

## Testing

| Suite | What it proves | Run |
|---|---|---|
| unit (`packages/core/tests/unit`, `apps/web/tests/unit`) | validator and CSS sanitizer rules, the DOM shim through QuickJS in Node, the CPU budget against the shared fixture, the wasm memory cap | `pnpm test:unit` |
| attacks (`tests/attacks.spec.ts`, `corpus/attacks`) | every exfiltration vector gets 0 canary hits and 0 outside requests; also on the Shadow DOM renderer with `TW_RENDERER=shadow` | `pnpm test:attacks` |
| compat / generated / common / showcase / htmlbubbles | widgets load, run their scripted interactions, and the sandbox render matches the native reference (screenshot diff) → `docs/compat-report*.md` | `pnpm test:compat` … |
| lifecycle | off-screen pause: frames stop, timers at 1 Hz, `visibilitychange` | `pnpm test:lifecycle` |
| Swift package (26 tests), Android instrumented (27), desktop (23 + the attack corpus with 0 leaks) | the same invariants through the native glue: engine, validator, renderer, limits, pools | see above |
| demo autoruns (`--autorun attacks|generated|compat|pause` on iOS/macOS, `--es autorun …` on Android) | the corpora on a device, JSON reports in the app container | `apps/*/README.md` |

Before a commit that touches `packages/core/src`: typecheck, unit, attacks,
and `pnpm build:ios`/`android`/`desktop` so the shipped bundles and `.qbc`
match the source (a stale `.qbc` is refused at load time and the native
tests fail).

## Commands

```bash
pnpm install
pnpm dev            # dev app on http://127.0.0.1:5173 (COOP/COEP set by Vite)
pnpm test:unit      # validator / sanitizer unit tests + shim regression tests through QuickJS (Node)
pnpm test:attacks   # every corpus/attacks/*.html → zero canary hits, no outside requests
pnpm test:compat    # corpus/compat/*.html in sandbox vs native reference → docs/compat-report.md (screenshots in reports/compat/)
pnpm test:generated # corpus/generated/** (committed corpus, or regenerate with pnpm generate-corpus) → docs/compat-report-generated.md
pnpm typecheck
pnpm tsx packages/core/scripts/sandbox-node.ts corpus/compat/tabs.html --verbose   # run a widget through the sandbox in Node
ANTHROPIC_API_KEY=… pnpm generate-corpus [--model claude-sonnet-5-5] [--count 20]
```

The committed `corpus/generated/` (20 prompts × {without,with}-profile) was authored
by Claude inside a Claude Code session rather than through the API (no key was
available); `manifest.json` in each batch says so. Results and caveats are in
`docs/compat-report-generated.md` and the "Phase 6" section of `docs/decisions.md`.

The dev app: corpus list and drag-and-drop on the left, the widget in a
chat-bubble-sized frame in the middle (light/dark toggle, reference-mode
toggle), diagnostics on the right (widget console, unsupported APIs,
ops/queries/latency, JS time, memory, errors). URL parameters:
`?file=compat/tabs.html&theme=dark&reference=1`.

## Layout

`packages/` is the framework — what a client depends on; nothing in it imports
from `apps/`. `apps/` is the demos, the native renderer prototypes and the dev
tooling, consuming the packages the way a client would. `corpus/`, `tests/`
(Playwright, run against `apps/web`) and `docs/` are shared.

| Path | What |
|---|---|
| `packages/core/` | npm package `@telegram/widget-sandbox`: `src/host/` (trusted: `widget-host.ts`, `worker.ts`, `validator.ts`, `css-sanitizer.ts`, `url-policy.ts`, `allowlist.ts`), `src/renderer/` (trusted code inside the sandboxed iframe / shadow root), `src/sandbox/` (**untrusted** once loaded: the DOM shim over linkedom), `src/shared/protocol.ts`; `vite.ts` (the plugin that builds the bundles), `scripts/` (bundle build, Node harness), `tests/unit/` |
| `packages/ios/` | Swift package `WidgetSandbox` (quickjs-ng + WKWebView, same JS bundles) for iOS and macOS; the manifest is the repo-root `Package.swift` — see [packages/ios/README.md](packages/ios/README.md); `pnpm build:ios` builds its bundles |
| `packages/android/` | Gradle library `widget-sandbox` = `org.telegram.widgetsandbox:widget-sandbox` (the same quickjs-ng sources via NDK + `android.webkit.WebView`, same JS bundles) — see [packages/android/README.md](packages/android/README.md); `pnpm build:android` |
| `packages/desktop/` | Qt-free C++ core for tdesktop-class hosts — see [packages/desktop/README.md](packages/desktop/README.md); `pnpm build:desktop` |
| `packages/tools/qjsc` | bytecode compiler from the vendored quickjs-ng (`.qbc` next to each bundle) |
| `apps/web/` | the dev app (`src/main.ts`, exposes `window.__tw` for the Playwright suites), the chat feed demo (`src/feed.ts`), bench pages, the Blitz / litehtml wasm renderer glue (`src/renderers/`) |
| `apps/ios/DemoApp`, `apps/macos/DemoApp` | SwiftUI demos (the macOS one shares the iOS sources) with `--autorun attacks|generated|compat|pause` — see [apps/macos/README.md](apps/macos/README.md) |
| `apps/android/` | the Compose demo and the Blitz / litehtml renderer modules; a second Gradle build over `packages/android` (`includeBuild`) |
| `apps/renderer-blitz/`, `apps/renderer-litehtml/` | native renderer prototypes behind the `Renderer` seam (Rust crate; C++ core with vendored litehtml) — dev tooling, not a product path |
| `corpus/attacks/` | one exfiltration vector per file (93) |
| `corpus/compat/` | realistic widgets (21) with scripted interactions in `tests/compat.spec.ts` |
| `tests/` | Playwright suites, canary server |
| `docs/` | `decisions.md`, `protocol.md`, `widget-runtime-profile.md`, `compat-report.md`, `native-port-notes.md` |

## Security invariants

See the prompt in `docs/overview.md` and the per-file comments in `packages/core/src/host/`
and `packages/core/src/renderer/`. In short: widget JS only in QuickJS; everything from the
sandbox is validated against allowlists; only `data:` / host `blob:` / `#fragment`
URLs reach the renderer; the renderer has `connect-src 'none'`, a nonce CSP and
cancels every navigation; QuickJS has no network capability at all.
