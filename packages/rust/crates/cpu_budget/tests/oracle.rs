//! The Rust CPU budget against widget-sandbox's cpu_budget.cpp (`oracle/cpu_budget_oracle`): scripts of
//! records, decisions, `used` and kill checks on budgets of every configuration, each step answered with
//! the throttling stretch and the kill verdict too, and the static helpers on edge and random values.
//! Skipped without `TWIDGET_ORACLE_DIR`; `tools/gate.py` sets it.

use std::path::{Path, PathBuf};

use twidget_cpu_budget::*;
use twidget_json::Json;
use twidget_testkit::{Comparison, Fields, Lcg, Oracle, mutate};

const KINDS: [&[u8]; 16] = [
    b"timer",
    b"frame",
    b"viewport",
    b"resize",
    b"theme",
    b"bridge",
    b"boot",
    b"event",
    b"visibility",
    b"",
    b"Timer",
    b"timer\0",
    b"bridge ",
    b"frames",
    b"\xfftimer",
    b"them",
];

fn pick<T: Copy>(random: &mut Lcg, items: &[T]) -> T {
    items[random.below(items.len())]
}

/// A time step: mostly forward by a little, a bucket or a window, sometimes far or backward.
fn advance(random: &mut Lcg, now: f64, bucket: f64) -> f64 {
    let next = match random.below(16) {
        0..=3 => now,
        4..=7 => now + random.below(60) as f64 + f64::from(random.below(4) as u8) * 0.25,
        8 => now + bucket,
        9 => now + bucket * random.below(12) as f64,
        10 => now + 1000.0 * random.below(3) as f64,
        11 => now + random.below(2_000_000) as f64,
        12 => now - random.below(500) as f64,
        13 => (now / bucket).floor() * bucket + pick(random, &[0.0, -0.0, 1e-9, -1e-9, 0.5]),
        14 => pick(random, &[0.0, -0.0, 1e15, -1e15, 4503599627370496.0, -1.0, 999.999]),
        _ => now + (random.next() as f64) / 1e3,
    };
    if next.abs() > 1e15 { now } else { next }
}

fn cpu_ms(random: &mut Lcg) -> f64 {
    match random.below(10) {
        0 => pick(random, &[0.0, -0.0, -1.0, f64::NAN, f64::INFINITY, f64::NEG_INFINITY, 1e-300, 1e300, 5e-324]),
        1 => pick(random, &[300.0, 299.99999, 300.00001, 100.0, 600.0, 0.1, 0.2]),
        _ => random.below(400) as f64 + random.below(1000) as f64 / 1000.0,
    }
}

/// A time for the steps that change nothing but may take any double without leaving C++'s defined ground:
/// decide and used take NaN and minus infinity (no bucket of theirs is written), the kill check anything.
fn odd_time(random: &mut Lcg, now: f64, any: bool) -> f64 {
    if !random.chance(12) {
        return now;
    }
    if any { pick(random, &[f64::NAN, f64::INFINITY, f64::NEG_INFINITY]) } else { pick(random, &[f64::NAN, f64::NEG_INFINITY]) }
}

/// What the scripts reached: deferrals, throttling stretches and kills.
static SEEN: [std::sync::atomic::AtomicUsize; 3] = [const { std::sync::atomic::AtomicUsize::new(0) }; 3];

fn tail(f: &mut Fields, budget: &CpuBudget, now: f64) -> (u8, Vec<u8>) {
    let seen = |i: usize, yes: bool| {
        if yes {
            SEEN[i].fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        }
    };
    seen(0, f.0.first() == Some(&0) && f.0.len() == 17);
    seen(1, budget.throttled_for(now) > 0.0);
    seen(2, budget.should_kill(now));
    f.f64(budget.throttled_for(now)).flag(budget.should_kill(now));
    (1, f.take())
}

fn config(random: &mut Lcg) -> [f64; 4] {
    [
        pick(random, &[500.0, 200.0, 0.0, 1.0, 1e9, f64::NAN, -1.0, 16.5]),
        pick(random, &[300.0, 100.0, 0.0, 1.0, 0.5, 1e-9, 1e6, f64::NAN, f64::INFINITY, -5.0, 299.99, 300.0, 300.0, 100.0]),
        pick(random, &[1000.0, 1000.0, 1000.0, 10.0, 100.0, 5000.0, 0.0, 1.0, 9.0, 10.5, -100.0, f64::NAN, f64::INFINITY, 1e7, 3.3]),
        pick(random, &[0.0, 0.0, 2000.0, 5000.0, f64::NAN, -1.0, f64::INFINITY, 1.0, 1000.0, 3000.0]),
    ]
}

/// A script on one budget: the setting that makes it, then its steps, each a case.
fn script(c: &mut Comparison, random: &mut Lcg, cfg: [f64; 4], steps: usize, what: &str) {
    let mut f = Fields::new();
    for v in cfg {
        f.f64(v);
    }
    c.setting(&f.framed(b'N'));
    f.take();
    let mut budget = CpuBudget::new(cfg[0], cfg[1], cfg[2], cfg[3]);
    let bucket = if cfg[2] / 10.0 > 1.0 { cfg[2] / 10.0 } else { 1.0 };
    let bucket = if bucket.is_finite() { bucket } else { 1000.0 };
    let mut now = pick(random, &[0.0, 0.0, 5.0, -3000.0, 1e12, 123456.75]);
    for step in 0..steps {
        now = advance(random, now, bucket);
        let what = format!("{what} {cfg:?}, step {step}");
        match random.below(10) {
            0..=3 => {
                let kind = pick(random, &KINDS);
                let ms = cpu_ms(random);
                budget.record(kind, ms, now);
                let record = f.str(kind).f64(ms).f64(now).framed(b'r');
                f.take();
                c.case(&record, tail(&mut f, &budget, now), format!("{what}: record {ms} at {now}"));
            }
            4..=6 => {
                let kind = pick(random, &KINDS);
                let at = odd_time(random, now, false);
                let record = f.str(kind).f64(at).framed(b'd');
                f.take();
                let d = budget.decide(kind, at);
                f.flag(d.run).f64(d.slice_ms).f64(d.retry_at);
                c.case(&record, tail(&mut f, &budget, at), format!("{what}: decide {:?} at {at}", String::from_utf8_lossy(kind)));
            }
            7 | 8 => {
                let at = odd_time(random, now, false);
                let record = f.f64(at).framed(b'u');
                f.take();
                let used = budget.used(at);
                f.f64(used);
                c.case(&record, tail(&mut f, &budget, at), format!("{what}: used at {at}"));
            }
            _ => {
                let later = now + random.below(3000) as f64;
                let at = odd_time(random, later, true);
                let record = f.f64(at).framed(b'k');
                f.take();
                c.case(&record, tail(&mut f, &budget, at), format!("{what}: kill check at {at}"));
            }
        }
    }
}

#[test]
fn scripts_alike() {
    let Some(oracle) = Oracle::find("cpu_budget_oracle") else { return };
    let mut random = Lcg::new(0xc0_b0d6);
    let mut c = Comparison::new();
    for i in 0..2500 {
        let cfg = config(&mut random);
        let steps = 10 + random.below(80);
        script(&mut c, &mut random, cfg, steps, &format!("script {i}"));
    }
    // The fixture's configurations, a greedy timer under each: the throttling, its stretch and the kill.
    for i in 0..200 {
        let cfg = [500.0, 300.0, 1000.0, pick(&mut random, &[0.0, 2000.0, 3000.0])];
        script(&mut c, &mut random, cfg, 200, &format!("long script {i}"));
    }
    let tally = c.check(&oracle);
    let seen: Vec<usize> = SEEN.iter().map(|n| n.load(std::sync::atomic::Ordering::Relaxed)).collect();
    eprintln!("CPU budget scripts: {} steps alike ({} deferrals, {} throttled, {} kills)", tally[1], seen[0], seen[1], seen[2]);
    assert!(seen.iter().all(|n| *n > 100));
}

fn widget_sandbox() -> PathBuf {
    std::env::var_os("TWIDGET_WIDGET_SANDBOX")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../.."))
}

/// The fixture's own steps on both, as given and with their numbers changed.
#[test]
fn fixture_steps_alike() {
    let Some(oracle) = Oracle::find("cpu_budget_oracle") else { return };
    let bytes = std::fs::read(widget_sandbox().join("packages/core/tests/fixtures/cpu-budget.json")).expect("the fixture");
    let root = Json::parse(&bytes).expect("the fixture parses");
    let mut random = Lcg::new(0xf1_c7);
    let mut c = Comparison::new();
    let mut f = Fields::new();
    for round in 0..40 {
        for case in root["cases"].arr() {
            let cfg = &case["config"];
            let mut values = [
                cfg["cpuLimitMs"].as_number(0.0),
                cfg["cpuQuotaMs"].as_number(0.0),
                cfg["cpuQuotaWindowMs"].as_number(0.0),
                cfg["cpuKillAfterMs"].as_number(0.0),
            ];
            if round > 0 && random.chance(3) {
                values[random.below(4)] *= pick(&mut random, &[0.5, 2.0, 0.999, 1.001]);
            }
            for v in values {
                f.f64(v);
            }
            c.setting(&f.framed(b'N'));
            f.take();
            let mut budget = CpuBudget::new(values[0], values[1], values[2], values[3]);
            for step in case["steps"].arr() {
                let mut at = step["at"].as_number(0.0);
                if round > 0 && random.chance(4) {
                    at += pick(&mut random, &[1.0, -1.0, 0.5, 99.0, 100.0, 1000.0]);
                }
                let what = format!("fixture round {round}, {}", step.dump_lossy());
                if let Some(kind) = step.find("decide") {
                    let record = f.str(kind.as_str()).f64(at).framed(b'd');
                    f.take();
                    let d = budget.decide(kind.as_str(), at);
                    f.flag(d.run).f64(d.slice_ms).f64(d.retry_at);
                    c.case(&record, tail(&mut f, &budget, at), what);
                } else if let Some(kind) = step.find("record") {
                    let mut ms = step["cpuMs"].as_number(0.0);
                    if round > 0 && random.chance(3) {
                        ms = cpu_ms(&mut random);
                    }
                    budget.record(kind.as_str(), ms, at);
                    let record = f.str(kind.as_str()).f64(ms).f64(at).framed(b'r');
                    f.take();
                    c.case(&record, tail(&mut f, &budget, at), what);
                } else if step.find("used").is_some() {
                    let record = f.f64(at).framed(b'u');
                    f.take();
                    let used = budget.used(at);
                    f.f64(used);
                    c.case(&record, tail(&mut f, &budget, at), what);
                } else if step.find("kill").is_some() {
                    let record = f.f64(at).framed(b'k');
                    f.take();
                    c.case(&record, tail(&mut f, &budget, at), what);
                }
            }
        }
    }
    let tally = c.check(&oracle);
    eprintln!("CPU budget fixture steps: {} answers alike", tally[0] + tally[1] + tally[2]);
}

fn query_case(c: &mut Comparison, queries: i32, ms: f64) {
    let record = Fields::new().i32(queries).f64(ms).framed(b'q');
    c.case(&record, (1, CpuBudget::query_limit_hit(queries, ms).as_bytes().to_vec()), format!("query limit of {queries}, {ms}"));
}

#[test]
fn helpers_alike() {
    let Some(oracle) = Oracle::find("cpu_budget_oracle") else { return };
    let mut random = Lcg::new(0x5_7a71c);
    let mut c = Comparison::new();
    let counts = [i32::MIN, -1, 0, 1, 127, 128, 129, 130, 1000, i32::MAX];
    let times = [f64::NAN, f64::INFINITY, f64::NEG_INFINITY, -0.0, 0.0, 999.9999999, 1000.0, 1000.0000000000001, 1001.0, 1e300];
    for q in counts {
        for t in times {
            query_case(&mut c, q, t);
        }
    }
    let doubles = [0.0, -0.0, 1.0, 3.0, 40.0, 1e6, 1e300, -5.0, f64::NAN, f64::INFINITY, f64::NEG_INFINITY, 0.1, 5e-324];
    let mut f = Fields::new();
    for _ in 0..20000 {
        query_case(&mut c, random.next() as i32 >> random.below(31), (random.next() as f64) / 1e6);
        let ops = if random.chance(3) { pick(&mut random, &doubles) } else { random.below(100000) as f64 };
        let canvas = if random.chance(3) { pick(&mut random, &doubles) } else { (random.next() as f64) / 7.0 };
        let record = f.f64(ops).f64(canvas).framed(b'o');
        f.take();
        f.f64(CpuBudget::ops_cost_ms(ops, canvas));
        c.case(&record, (1, f.take()), format!("ops cost of {ops}, {canvas}"));
        let mut kind = pick(&mut random, &KINDS).to_vec();
        let edits = if random.chance(2) { mutate(&mut random, &mut kind) } else { String::new() };
        c.case(
            &Fields::new().str(&kind).framed(b'f'),
            (u8::from(CpuBudget::deferrable(&kind)), Vec::new()),
            format!("deferrable {kind:?}{edits}"),
        );
        let hit = pick(&mut random, &[&b"count"[..], b"time", b"", b"Count", b"count\0", b"\xff"]);
        let record = f.str(hit).str(&kind).framed(b'R');
        f.take();
        c.case(&record, (1, CpuBudget::query_limit_reason(hit, &kind)), format!("query limit reason {hit:?} {kind:?}"));
    }
    let tally = c.check(&oracle);
    eprintln!("CPU budget helpers: {} answers alike", tally[0] + tally[1]);
}
