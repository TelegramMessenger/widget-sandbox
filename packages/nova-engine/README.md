# Nova engine (`QuickJSBackend.nova`)

`NovaEngineFFI.xcframework` is [Nova](https://trynova.dev/), a JavaScript engine written in Rust, from the
nova-safe fork, where `nova_vm` is built with `#![forbid(unsafe_code)]`: no script can reach undefined behaviour in
the engine. It is linked as a static library behind a small C interface (`nova_engine.h`, the `nova_embed_ffi` crate),
the only Rust code of the engine with `unsafe`. The Swift side is `NovaEngine.swift`.

Status: macOS only, and the default there wherever it is linked: `QuickJSBackend.default` (every widget engine and the pools) and `QuickJSBackend.validatorDefault` (the trusted validator) start as `.nova` when `QuickJSBackend.isNovaAvailable`, which says whether this build has the library and whether it enforces its interrupt, heap and stack limits (`nova_embed_limits() == 7`). A build without it keeps the native engine; a host or a validator that asks for `.nova` where it is missing fails closed (`Engine fault: …`), it never falls back to QuickJS. Like a QuickJS context, every Nova engine has `atob` and `btoa` (the trusted validator decodes `data:` images with them).

## What is in the directory

Only this README is committed. The build writes, next to it (all three are in `.gitignore`):

| File | |
|---|---|
| `NovaEngineFFI.xcframework` | `libnova_embed_ffi.a` for macOS arm64 (from 11.0) and x86_64 (from 10.13), with `nova_engine.h` and a module map |
| `manifest.sha256` | the sha256 of every file of the xcframework |
| `BUILD_INFO` | the nova-safe commit and the rustc it was built with |

The package needs it: until it is built, SwiftPM stops with "local binary target 'NovaEngineFFI' … does not contain
a binary artifact". To build without Nova (iOS, or a machine without Rust), set `WIDGET_SANDBOX_WITHOUT_NOVA=1`:
`QuickJSBackend.isNovaAvailable` is then false, the default engine is QuickJS, a `.nova` engine fails closed, and
`QuickJSBackend.useNovaEverywhere()` does not exist, so a client that calls it does not compile. (An environment
variable and not a check for the file: SwiftPM caches a manifest's result by its text and environment, so a check for
a file would keep answering what it found first.)

## How it is built

Telegram for macOS has [nova-safe](https://github.com/macosswift/nova-safe) as the submodule `submodules/nova`
and builds the library with its other native libraries (`scripts/configure_frameworks.sh`, the `nova-engine`
project in `core-xprojects`), into this directory of its `third-party/widget-sandbox` checkout. It is rebuilt
when the submodule, the Rust toolchain or Xcode changes, and the build checks `manifest.sha256`.

Anywhere else, from a checkout of nova-safe (branch `main`), with the toolchain its `rust-toolchain.toml` pins:

```bash
safe/embed/build-xcframework.sh <widget-sandbox>/packages/nova-engine
```

The script builds `nova_embed_ffi` in release with fat LTO for both architectures, from nova-safe's committed
`Cargo.lock` (`--locked`: the dependency versions its `safe/TCB.md` audits), prelinks each architecture into one
object that exports only the `nova_*` functions (the Rust standard library inside stays private, so the library links
next to the app's other Rust libraries, built with other toolchains, without duplicate symbols), strips debug info,
makes the xcframework with `xcodebuild -create-xcframework`, and writes `manifest.sha256` and `BUILD_INFO`. The
library is about 16 MB per architecture; linked into a binary it adds about 8.5 MB (arm64, dead-stripped).
`nova_embed_version()` (`NovaEngine.version`, the `engine` diagnostic of a Nova host) names the commit too. Check a
copy with `shasum -a 256 -c manifest.sha256` from this directory.

The C interface is tested in nova-safe: `cargo test -p nova_embed -p nova_embed_ffi` (the API, and the C ABI driven
with random and invalid inputs) and `safe/embed/test-c-abi.sh` (the header compiled with `-Wall -Werror -pedantic` and
ASan/UBSan against the library).

## What differs from QuickJS

- **No bytecode.** The `.qbc` bundles are QuickJS bytecode: a Nova engine evaluates the sandbox and validator bundles
  from source, without trying the `.qbc` (no `bytecode` diagnostic). `evaluateBytecode` and `compileBytecode` throw.
- **Limits.** The interrupt (the CPU deadlines, polled every 4096 instructions, native loop steps and RegExp
  matcher steps; a termination that `try`/`catch`/`finally` cannot stop), the memory limit (an estimate of Nova's
  heap, `memoryUsed`) and the guard against deep recursion (a catchable `RangeError`, in JavaScript and in the
  engine's own recursion: bound functions, proxies, JSON, the parser). The stack guard gets three quarters of
  `maxStackBytes` (4.5 MB of the JSThread's 6 MB limit, on its 8 MB stack). `nova_embed_limits()` says which a build
  enforces, and a `.nova` engine fails closed (`Engine fault`) unless it is all three.
- **Local time** comes from the host (`TimeZone.current`, daylight saving time included), as QuickJS reads it from
  `localtime_r`.
- **No Atomics or SharedArrayBuffer** (nova-safe's `widget` feature set; widgets have no workers to share memory with).
- **Exceptions** read `Name: message`, without QuickJS's stack lines.
- **Host functions** receive their first 8 arguments (the hosts' own read 2 or 3), as in the box. Strings cross as
  WTF-8, exactly as QuickJS hands them out.
- **The engine.** Nova is slower than QuickJS (about 3 times on a median widget, more on heavy animations) and still
  differs on some scripts; the compatibility track of nova-safe works through them. `NovaEngineDifferentialTests`
  compares what the renderer receives from both engines.

## Turn it on

Nothing to do where the library is linked. A client that must never run widgets without it calls, once at start:

```swift
QuickJSBackend.useNovaEverywhere()        // main thread, before the first host
```

It sets `QuickJSBackend.default` (every widget engine of the process), `EnginePool.shared.backend` (the pools) and
`QuickJSBackend.validatorDefault` (the trusted validator, which runs our own code), and exists only in builds that
link Nova, so such a client does not compile without it.

A DEBUG build reads `WIDGET_SANDBOX_ENGINE` and `WIDGET_SANDBOX_VALIDATOR_ENGINE` (`native`, `boxed`, `nova`) from the
environment. With the library here the test suite runs on Nova:

```bash
swift test                                                                    # on Nova
WIDGET_SANDBOX_ENGINE=native WIDGET_SANDBOX_VALIDATOR_ENGINE=native swift test   # on QuickJS
WIDGET_SANDBOX_NOVA_DIFFERENTIAL=1 swift test --filter NovaEngineDifferentialTests      # opt-in; =full for every corpus
```
