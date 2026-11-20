# widget-sandbox in safe Rust

The sixth copy of the host glue (after the web Worker, the Node harness, Swift, Kotlin and C++), in Rust whose
crates all forbid `unsafe` code, for Telegram Desktop's widget helper: it runs the widget's JavaScript on Nova
(`nova_embed`, nova-safe's safe embedding API) and speaks the desktop core's stream to the host. It is ported
from `packages/desktop/sandbox` and compared with it at every stage; the plan and its stages are twidget's
`rust/PLAN.md`, the renderer side of the port is twidget's `rust/` workspace, which depends on this one.

| Path | What |
|---|---|
| `crates/js` | `twidget_js`: the engine thread (`js_thread.h`) and the Nova context on it (`nova_context.h`), over nova-safe's `nova_embed` |
| `crates/json` | `twidget_json`: `twidget::Json` (`packages/desktop/sandbox/include/twidget/json.h`) |
| `crates/cpu_budget` | `twidget_cpu_budget`: the CPU quota, its deferrals and kill, the layout-query guard (`cpu_budget.h`) |
| `crates/sdk_policy` | `twidget_sdk_policy`: the SDK v1 checks, the request and haptic gates (`sdk_policy.h`) |
| `crates/gesture` | `twidget_gesture`: the touch-action claim and the gesture arbiter (`gesture_policy.h`) |
| `crates/engine` | `twidget_engine`: the sandbox engine in process, the widget's Nova runtime on its own thread with its one capability and limits (`sandbox_engine.h`) |
| `crates/server` | `twidget_server`: the engine process's end: the wire forms (`remote_coding.cpp`), the server of one client-mode engine (`remote_engine.cpp`) and the stream's serving loop (`remote_stream_engine.cpp`) |
| `crates/engine_ffi` | `twidget_engine_ffi`, the one crate with `unsafe`: the C ABI the C++ OS glue serves a stream through (`include/twidget_engine.h`), with Nova's C interface in the same library |
| `crates/bundle_stamp` | `twidget_bundle_stamp`: the bundles' VERSION.json stamp, the versions, the bundles as read (`bundle_stamp.h`) |
| `crates/stream` | `twidget_stream`: the remote engine's byte stream: kinds, shapes, the bounded codec, the exact frame reader (`src/remote_wire.h`) |
| `crates/testkit` | `twidget_testkit`, tests only: the C++ suites' seeded generator and mutator, the oracles' client |
| `oracle/` | the C++ the port is compared with: small programs built from `packages/desktop/sandbox`, cases on stdin |
| `tools/gate.py` | the gate: oracle build, `cargo test` with the comparison required, clippy, rustfmt, `unsafe` report |
| `tools/unsafe_report.py` | `unsafe` in the workspace and in every crate it links |

```sh
python -I tools/gate.py            # add --release to run the tests optimized too
```

The desktop core's own suite runs against the engine end in Rust: configure `packages/desktop` with
`-DTW_RUST_ENGINE_END=ON` (cargo builds `twidget_engine_ffi`, which also gives the C++ its Nova) and run
`TW_ENGINE_STREAM=1 TW_ENGINE_END=rust ./twidget-tests` and `twidget-run --attacks`: every fixture engine and every
attack then runs over a byte stream served by this workspace's `serve`.

Fixtures come from `packages/core/tests/fixtures`, as every other port reads them.
