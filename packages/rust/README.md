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
| `crates/bundle_stamp` | `twidget_bundle_stamp`: the bundles' VERSION.json stamp, the versions, the bundles as read (`bundle_stamp.h`) |
| `crates/stream` | `twidget_stream`: the remote engine's byte stream: kinds, shapes, the bounded codec, the exact frame reader (`src/remote_wire.h`) |
| `crates/testkit` | `twidget_testkit`, tests only: the C++ suites' seeded generator and mutator, the oracles' client |
| `oracle/` | the C++ the port is compared with: small programs built from `packages/desktop/sandbox`, cases on stdin |
| `tools/gate.py` | the gate: oracle build, `cargo test` with the comparison required, clippy, rustfmt, `unsafe` report |
| `tools/unsafe_report.py` | `unsafe` in the workspace and in every crate it links |

```sh
python -I tools/gate.py            # add --release to run the tests optimized too
```

Fixtures come from `packages/core/tests/fixtures`, as every other port reads them.
