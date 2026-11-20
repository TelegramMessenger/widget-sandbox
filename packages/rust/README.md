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
| `crates/stream` | `twidget_stream`: the remote engine's byte stream: kinds, shapes, the bounded codec, the exact frame reader (`src/remote_wire.h`) |
| `crates/testkit` | `twidget_testkit`, tests only: the C++ suites' seeded generator and mutator, the oracles' client |
| `oracle/` | the C++ the port is compared with: small programs built from `packages/desktop/sandbox`, cases on stdin |
| `tools/gate.py` | the gate: oracle build, `cargo test` with the comparison required, clippy, rustfmt, `unsafe` report |
| `tools/unsafe_report.py` | `unsafe` in the workspace and in every crate it links |

```sh
python -I tools/gate.py            # add --release to run the tests optimized too
```

nova-safe is checked out next to widget-sandbox (`../nova-safe`, as tdesktop has it): the workspace repeats its
`[patch.crates-io]` and starts from its `Cargo.lock`, so the engine links what nova-safe checked.

Fixtures come from `packages/core/tests/fixtures`, as every other port reads them.
