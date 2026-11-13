# The boxed QuickJS engine: a client's guide

The widget engine is QuickJS, which is C. Every engine release since v0.6.1 fixed memory bugs in it that a widget's
plain JavaScript could reach, below the validator and the sandbox. The box is a second wall under the engine: the
same fork commit compiled to WebAssembly and translated back to C (wasm2c), so the engine runs inside a linear memory
of its own. A memory bug in it then reads and writes that one widget's memory, or stops the process. It cannot reach
the host's memory, another widget, or a system call.

Status: opt-in, macOS only, "contain and crash". The native engine stays the default on every platform, and nothing
changes for a client that does not opt in. Why it is built this way is in `docs/decisions.md`, "QuickJS in a wasm2c
box".

## 1. What changes for a widget

Nothing a widget should notice. The same engine runs the same shim from the same `.qbc`, and the renderer receives
the same messages (section 4 has the check). The differences that exist:

| | Native | Boxed |
|---|---|---|
| Speed | 1 | JS about 1.4 times slower with guard pages (macOS), about 1.7 to 1.9 times with bounds checks. Boot is not slower |
| Memory limit | counted by the engine (`JS_SetMemoryLimit`) | the size the box's memory may grow to: the limit plus the 2 MB the box starts with. The box's heap is 32-bit, so the same objects take fewer bytes: a widget that dies of the limit natively may live on in the box, to its next limit |
| Runaway recursion | `RangeError` after about 6 600 calls | `RangeError` after about 2 400 calls |
| `Math.tan`, `asin`, `acos`, `atanh` and friends | the system's libm | the box's own libm (the web's): the last digit may differ |
| A host function's arguments | all of them | the first 8 (the hosts' own read 2) |
| The 3D side channel (debug builds) | available | not available: it talks to the engine directly |
| An engine memory bug | undefined behavior inside the process | that widget's memory only, or the process stops with a reason |
| The validator's engine | native | native: it runs our code, and the box keeps the widget's engine away from it |

## 2. Turn it on

The Swift package builds the box with `WidgetSandbox` on macOS (`CQuickJSBox`). Nothing uses it until a host asks.

```swift
// Every widget of the process (set it once, before the first host):
QuickJSBackend.default = .boxed
EnginePool.shared.backend = .boxed        // if you use the pools

// Or one host:
var options = WidgetHost.Options()
options.engine = .boxed
```

- **Check that it took.** A boxed host reports the diagnostic `engine` once, for example
  `boxed QuickJS b526a81 (guard-pages)`. `QuickJSBackend.isBoxedAvailable` says whether this build can run the box.
- **It fails closed.** A build that cannot run the box does not fall back to the native engine: the widget stops
  with `Engine fault: …` and nothing of it is rendered.
- **Compile it optimized, in every configuration.** An unoptimized box is 11 to 48 times slower than the native
  engine, far past the CPU limits a widget lives under, so it refuses to start (`Engine fault: the QuickJS box was
  compiled without optimization`). The package passes `-O3` to its C targets when it is a local checkout. If your
  build compiles the package's C with your own flags, make sure Debug gets `-O2` for `CQuickJSBox`.
- **Without SwiftPM:** compile every `.c` in `packages/quickjs-box/src` and nothing else, with the include paths
  `packages/quickjs-box/include`, `generated` and `wabt`. No defines are needed: `src/qjsbox_config.h` holds the whole
  configuration, and the generated code and WABT's runtime are compiled only through the files in `src`.
- **The other memory mode.** macOS fences the box with guard pages: each box reserves 8 GiB of address space, and an
  access outside its memory hits an unmapped page. Everywhere else the build uses bounds checks: no reservation, no
  signals, the same C, slower. `QJSBOX_BOUNDS_CHECKS=1` (the package: `WIDGET_SANDBOX_BOX_BOUNDS_CHECKS=1` in the
  environment of the build) forces bounds checks on macOS, to test the mode the other platforms will run.

## 3. What a fault looks like

A fault inside the box ends the process. This is the same thing an engine crash does today, with one difference:
the bug could not touch anything outside that widget's memory first. In `WidgetEngineService.xpc` that means the
service dies and is started again, as on any crash.

- With bounds checks, and for every fault that is not a memory access, the process writes one line to stderr and
  calls `abort()`:
  `widget-sandbox: QuickJS box fault: trap: out-of-bounds access in the box's memory or table (QuickJS b526a81, bounds-checks)`.
  The line ends with the last thing the engine wrote to its own stderr, if anything (`; engine said: …`: the text
  of an assertion that stopped it). The backtrace
  has `qjsbox_fault` and a frame that names the trap (`qjsbox_trap_out_of_bounds`, `qjsbox_trap_call_indirect`,
  `qjsbox_trap_unreachable`, `qjsbox_bad_request`, …).
- With guard pages a memory access outside the box is a plain `EXC_BAD_ACCESS` (SIGBUS or SIGSEGV). The crashed
  thread's top frames are `w2c_qjsbox_…` with the engine's function names, and the fault address lies inside an
  8 GiB reservation.
- `qjsbox_set_fault_hook` (C) is called with the reason right before `abort()`, to log it or to annotate a crash
  report. It is not called for the guard-page case, where there is no code of ours between the fault and the end.

**Report these like crashes, and tell us.** A fault is an engine bug that the box contained. Without the report we
stop learning about engine bugs, because inside the box some of them no longer crash at all.

**Do not install a signal handler for the box.** WABT ships one, and it must never be compiled in: it claims every
SIGSEGV and SIGBUS of the process and jumps out of them. This build leaves it out (`WASM_RT_SKIP_SIGNAL_RECOVERY`).
Recovering from a fault so that only one widget stops is a later phase with its own design. A handler added on the
client's side would bring back exactly the risks that phase has to solve.

One rule if you write glue against `CQuickJSBox` yourself: never call into a box from inside one of its callbacks.
The box does the argument conversions itself, so nothing needs to. A call made anyway is refused, and a debug build
asserts.

## 4. Run the checks, in both memory modes

From the repository root. `corepack pnpm install` and `git submodule update --init` first.

```bash
# The artifacts are what the pinned tools write (emscripten 6.0.12, wasm2c at WABT commit ff0ef7e…):
packages/quickjs-box/build.sh --check            # --docker: the wasm in the pinned emsdk image instead of a local emcc
# The box on its own, in C, guard pages and bounds checks: values, host functions, limits, the time zone, the
# shipped .qbc, and what a corrupted engine does (the process ends):
packages/quickjs-box/tools/build.sh test

# The Swift suite: native (the default), then every widget engine boxed, then boxed with bounds checks.
swift test
WIDGET_SANDBOX_ENGINE=boxed swift test
WIDGET_SANDBOX_BOX_BOUNDS_CHECKS=1 WIDGET_SANDBOX_ENGINE=boxed swift test --scratch-path .cache/build-bounds

# Boxed against native over every corpus, the conformance set, the stress kit and the attacks (several minutes);
# it prints how many widgets were compared and the JS time on each engine:
WIDGET_SANDBOX_BOX_DIFFERENTIAL=full swift test --filter BoxedEngineDifferentialTests

# The attack corpus through the macOS demo, on the boxed engine (hits must be 0):
corepack pnpm build:macos                        # once: the bundles and the demo's copy of the corpora
cd apps/macos/DemoApp && xcodegen generate && xcodebuild build -scheme TwidgetDemoMac -destination 'platform=macOS' -derivedDataPath /tmp/twidget-mac
APP=/tmp/twidget-mac/Build/Products/Debug/TwidgetDemoMac.app/Contents/MacOS/TwidgetDemoMac
corepack pnpm canary &
"$APP" --autorun attacks --engine boxed          # attack-report.json: "hits": 0, "engine": "boxed QuickJS … (guard-pages)"
"$APP" --autorun generated --engine boxed        # generated-report.json: every row has "engine": 1 in its diags, and jsMs
"$APP" --autorun compat --engine boxed
```

`WIDGET_SANDBOX_ENGINE` is read by DEBUG builds only. It sets `QuickJSBackend.default`, so every host and engine of
the test process that does not name a backend is boxed. The validator stays native.

Expected on all three runs: the same result. One test fails on every engine on macOS 27 and iOS 27,
`IntlBridgeTests.testPluralRulesMatchICU` (an ICU difference of the OS, older than the box). The tests of the 3D side
channel name the native engine themselves, so they pass on a boxed run too.

Timings:

```bash
packages/quickjs-box/tools/build.sh
J=packages/ios/Sources/WidgetSandbox/WidgetJS
.cache/qjsbox-tools/qjsbox-run-guard-pages bench $J/sandbox-bundle.js $J/sandbox-bundle.qbc
.cache/qjsbox-tools/qjsbox-run-bounds-checks bench $J/sandbox-bundle.js $J/sandbox-bundle.qbc
```

## 5. What to send back

From the build that will ship (Release, your flags), on the slowest Mac you support, and on an Intel Mac if you
support one. Nothing here was measured on x86_64.

1. **The build.** The Mac (model, CPU), the OS, the configuration, and the `engine` diagnostic's text. Whether
   `QuickJSBackend.isBoxedAvailable` is true in your Debug build. The size of the service's binary with and without
   `CQuickJSBox`.
2. **The suite.** For each of the three `swift test` runs above: tests executed, and the names of the failures.
3. **The attacks.** `hits` and `engine` from `attack-report.json`, and every row whose `dead` starts with
   `Engine fault`.
4. **Faults.** Every end of the service with `QuickJS box fault` on stderr or `qjsbox_` / `w2c_qjsbox_` frames in
   the crash report: the line, the backtrace, and the widget if you know it. Zero is a result too.
5. **Speed, native against boxed, the same widgets:**
   - the `boot` diagnostic's milliseconds (median over at least 50 hosts, pooled and not);
   - `stats.jsMs` per widget from `generated-report.json` and `compat-report.json` (the sum and the median ratio);
   - how many widgets were throttled (`stats.cpuWindowMs` at the 300 ms quota) and how many died with
     `CPU limit exceeded`, on each engine;
   - the stress kit (`packages/stress/CLIENTS.md`): per ladder the full-rate level and the level where strict stops.
6. **Memory.** The service's footprint with 20 and with 100 widgets alive, on each engine.
7. **Anything a widget did differently**, with the widget.

## 6. Our numbers

An Apple-silicon Mac, macOS 27, the package's own optimized build. CPU time, best of 7.

| | Native | Guard pages | Bounds checks |
|---|---|---|---|
| A new engine plus the sandbox bundle from `.qbc` | 5.2 ms | 4.1 ms (×0.78) | 4.6 ms (×0.89) |
| Call-heavy JS (`fib(30)`) | 35.8 ms | 70.5 ms (×1.97) | 104.1 ms (×2.91) |
| Properties and arrays | 73.2 ms | 86.2 ms (×1.18) | 131.0 ms (×1.79) |
| Strings | 10.6 ms | 12.8 ms (×1.22) | 16.6 ms (×1.57) |
| JSON | 103.2 ms | 146.4 ms (×1.42) | 170.7 ms (×1.65) |
| RegExp | 23.3 ms | 25.8 ms (×1.11) | 31.3 ms (×1.34) |
| `Map` and `Set` | 19.0 ms | 21.6 ms (×1.14) | 27.9 ms (×1.47) |
| Typed arrays | 109.9 ms | 130.2 ms (×1.18) | 194.0 ms (×1.76) |
| Closures and array methods | 24.8 ms | 42.9 ms (×1.73) | 61.5 ms (×2.48) |

Real widgets through the whole host (`BoxedEngineDifferentialTests`, 440 widgets, JS time until loaded plus 150 ms):

| | Guard pages | Bounds checks |
|---|---|---|
| All 440 | ×1.41 | ×1.66 |
| `corpus/compat` | ×1.45 | ×1.75 |
| `corpus/generated` | ×1.43 | ×1.73 |
| `corpus/showcase` | ×1.44 | ×1.80 |
| The conformance set | ×1.45 | ×1.77 |
| The stress kit | ×1.63 | ×1.89 |
| `corpus/attacks` | ×1.13 | ×1.22 |

Of the 440, the renderer received the same messages from both engines for all that can be compared (411 on every
message, 22 on the first batch, 1 follows the clock). One showcase widget differs in the last digits of two numbers.
Six stress-kit levels past the node limit end with "Node limit exceeded" boxed and "Memory limit exceeded" natively
(`dom-static` and `dom-tick` 3000 and 4000, `style-churn` 2000 and 3000: the memory row of the table in section 1). The attack corpus through the macOS demo: 0 hits on each engine, and all 133 rows end the
same way.

Through Swift, a call into the engine costs 0.15 µs boxed and 0.10 µs natively, and a host function call 0.08 µs
against 0.07 µs.

A box costs 0.07 ms and about 0.2 MB of resident memory to create, and microseconds to drop (no engine code runs at teardown). The
compiled box adds about 1.2 MB of code with guard pages and 2.1 MB with bounds checks.

## 7. Rebuilding it

Only when the engine pin, emscripten or WABT changes. Never edit `wasm/`, `generated/`, `wabt/` or `manifest.sha256`
by hand.

```bash
packages/quickjs-box/build.sh            # rebuilds all four from the submodule's commit
packages/quickjs-box/build.sh --check    # must pass before the commit
packages/quickjs-box/tools/build.sh test
```

`packages/core/tests/unit/quickjs-box.test.ts` fails when the submodule moved and the box was not rebuilt.
