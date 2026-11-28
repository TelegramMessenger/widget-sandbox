//! widget-sandbox's CPU budget tests (packages/desktop/tests/test_main.cpp: cpu_budget_shared_fixture
//! against packages/core/tests/fixtures/cpu-budget.json, read as the C++ reads it, and the CPU budget's
//! part of strict_limit_defaults; its SandboxConfig and WidgetHostOptions defaults come with the engine).

use std::path::{Path, PathBuf};

use twidget_cpu_budget::*;
use twidget_json::Json;

fn widget_sandbox() -> PathBuf {
    std::env::var_os("TWIDGET_WIDGET_SANDBOX")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../.."))
}

/// C++'s `std::to_string(int(d))` of a fixture time.
fn whole(d: f64) -> i32 {
    d as i32
}

#[test]
fn cpu_budget_shared_fixture() {
    let path = widget_sandbox().join("packages/core/tests/fixtures/cpu-budget.json");
    let bytes = std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let root = Json::parse(&bytes).expect("the fixture parses");
    let cases = root["cases"].arr();
    assert!(cases.len() > 5);
    let mut cost_steps = 0;
    let mut checks = 0;
    for c in cases {
        let name = String::from_utf8_lossy(c["name"].as_str()).into_owned();
        let cfg = &c["config"];
        let mut b = CpuBudget::new(
            cfg["cpuLimitMs"].as_number(0.0),
            cfg["cpuQuotaMs"].as_number(0.0),
            cfg["cpuQuotaWindowMs"].as_number(0.0),
            cfg["cpuKillAfterMs"].as_number(0.0),
        );
        for (i, step) in c["steps"].arr().iter().enumerate() {
            let at = step["at"].as_number(0.0);
            let place = format!("{name}: step {i} @{}", whole(at));
            checks += 1;
            if let Some(kind) = step.find("decide") {
                let expect = &step["expect"];
                let want = if expect["run"].as_bool(false) {
                    CpuDecision { run: true, slice_ms: expect["sliceMs"].as_number(0.0), retry_at: 0.0 }
                } else {
                    CpuDecision { run: false, slice_ms: 0.0, retry_at: expect["retryAt"].as_number(0.0) }
                };
                let got = b.decide(kind.as_str(), at);
                assert!(got == want, "{place} got run={} slice={} retryAt={}", got.run, got.slice_ms, got.retry_at);
            } else if let Some(kind) = step.find("record") {
                b.record(kind.as_str(), step["cpuMs"].as_number(0.0), at);
            } else if let Some(kill) = step.find("kill") {
                assert!(b.should_kill(at) == kill.as_bool(false), "{place}");
            } else if let Some(used) = step.find("used") {
                let got = b.used(at);
                assert!((got - used.as_number(0.0)).abs() < 1e-9, "{place} used {got}");
            } else if let Some(q) = step.find("queries") {
                let want: &[u8] = if step["expect"].is_null() { b"" } else { step["expect"].as_str() };
                assert!(CpuBudget::query_limit_hit(q[0].as_number(0.0) as i32, q[1].as_number(0.0)).as_bytes() == want, "{place}");
            } else if let Some(cost) = step.find("opsCost") {
                cost_steps += 1;
                let got = CpuBudget::ops_cost_ms(cost[0].as_number(0.0), cost[1].as_number(0.0));
                assert!((got - step["expect"].as_number(0.0)).abs() < 1e-9, "{place} got {got}");
            } else {
                panic!("{place}: unknown step");
            }
        }
    }
    assert!(cost_steps >= 6, "{cost_steps} opsCost steps");
    eprintln!("{} cases, {checks} steps", cases.len());
}

/// The CPU budget's share of the defaults every host shares.
#[test]
fn strict_limit_defaults() {
    assert_eq!(CpuBudget::QUERY_LIMIT, 128);
    assert_eq!(CpuBudget::QUERY_BUDGET_MS, 1000.0);
}

/// The kill texts of the layout-query guard, as the engine stops a task with them.
#[test]
fn query_limit_reasons() {
    assert_eq!(CpuBudget::query_limit_hit(129, 0.0), "count");
    assert_eq!(CpuBudget::query_limit_hit(128, 1000.5), "time");
    assert_eq!(CpuBudget::query_limit_hit(128, 1000.0), "");
    assert_eq!(
        CpuBudget::query_limit_reason(b"count", b"timer"),
        b"Layout query limit exceeded: more than 128 layout queries in one timer task"
    );
    assert_eq!(
        CpuBudget::query_limit_reason(b"time", b"boot"),
        b"Layout query limit exceeded: host queries blocked the renderer for more than 1000 ms in one boot task"
    );
}
