//! The Rust gesture policy against widget-sandbox's gesture_policy.cpp (`oracle/gesture_policy_oracle`):
//! generated and mutated touch-action lists through `claim_of`, scripts of presses (downs, moves, ticks, ups
//! and cancels around the slop, the hold, the double tap and the edge strip, every pointer and button) through
//! the arbiter step by step, the names, and the C library's `hypot` the slop tests stand on. Skipped without
//! `TWIDGET_ORACLE_DIR`; `tools/gate.py` sets it.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use twidget_gesture::*;
use twidget_json::Json;
use twidget_testkit::{Comparison, Fields, Lcg, Oracle, mutate};

fn pick<T: Copy>(random: &mut Lcg, items: &[T]) -> T {
    items[random.below(items.len())]
}

const TOUCH_ACTIONS: &[&[u8]] = &[
    b"auto",
    b"none",
    b"manipulation",
    b"pan-x",
    b"pan-y",
    b"pan-left",
    b"pan-right",
    b"pan-up",
    b"pan-down",
    b"pinch-zoom",
    b"pan-x pan-y",
    b"pan-y pinch-zoom",
    b"pan-left pan-up",
    b"PAN-X",
    b" none ",
    b"\x0bnone\x0c",
    b"\tauto\r\n",
    b"pan-xx",
    b"xpan-x",
    b"pan-x-",
    b"pan_x",
    b"pan-",
    b"pan-left2",
    b"pan-x\xC3\xA9",
    b"\xC3\xA9pan-x",
    b"",
    b"  ",
    b"None",
    b"AUTO",
    b"pan-pan-x",
    b"apan-xpan-x",
    b"-pan-x",
    b"pan-y,pan-x",
    b"pan-\xE2\x80\x8Bx",
    b"nonE",
    b"inherit",
    b"pan-X",
    b"\xA0none",
];

fn claim_case(c: &mut Comparison, list: &[Vec<u8>], prevented: bool, what: String) {
    let mut f = Fields::new();
    f.u32(list.len() as u32);
    for ta in list {
        f.str(ta);
    }
    f.flag(prevented);
    let claim = claim_of(list, prevented);
    c.case(&f.framed(b'C'), (1, Fields::new().flag(claim.x).flag(claim.y).flag(claim.hold).take()), what);
}

#[test]
fn claims_alike() {
    let Some(oracle) = Oracle::find("gesture_policy_oracle") else { return };
    let mut random = Lcg::new(0x6e_0001);
    let mut c = Comparison::new();
    let mut seen = BTreeMap::new();
    for i in 0..30000 {
        let mut list: Vec<Vec<u8>> = (0..random.below(5)).map(|_| pick(&mut random, TOUCH_ACTIONS).to_vec()).collect();
        if random.chance(3) && !list.is_empty() {
            let at = random.below(list.len());
            if random.chance(2) {
                let joined = [
                    list[at].clone(),
                    pick(&mut random, &[&b" "[..], b"  ", b"\t", b""]).to_vec(),
                    pick(&mut random, TOUCH_ACTIONS).to_vec(),
                ]
                .concat();
                list[at] = joined;
            } else {
                mutate(&mut random, &mut list[at]);
            }
        }
        let prevented = random.chance(4);
        let claim = claim_of(&list, prevented);
        *seen.entry((claim.x, claim.hold)).or_insert(0usize) += 1;
        claim_case(&mut c, &list, prevented, format!("claim {i}: {list:?} prevented {prevented}"));
    }
    let tally = c.check(&oracle);
    eprintln!("gesture claims: {} alike, {seen:?}", tally[1]);
    assert!(seen.len() == 3 && seen.values().all(|n| *n > 1000), "{seen:?}");
}

fn verdict_fields(f: &mut Fields, v: Option<GestureVerdict>) {
    f.flag(v.is_some());
    let Some(v) = v else { return };
    f.u8(v.owner as u8).u8(v.gesture as u8).flag(v.cancel);
    f.flag(v.action.is_some()).u8(v.action.map_or(0, |a| a as u8));
    f.flag(v.event.is_some()).u8(v.event.map_or(0, |e| e as u8));
    f.flag(v.edge);
}

fn answer(a: &GestureArbiter, v: Option<GestureVerdict>) -> (u8, Vec<u8>) {
    let mut f = Fields::new();
    verdict_fields(&mut f, v);
    let deadline = a.hold_deadline();
    f.flag(deadline.is_some()).f64(deadline.unwrap_or(0.0));
    (1, f.take())
}

/// A move from the press's origin: on, inside and past the slop, on the diagonal, far.
fn offset(random: &mut Lcg) -> (f64, f64) {
    let (dx, dy) = match random.below(8) {
        0 => pick(
            random,
            &[(10.0, 0.0), (0.0, 10.0), (6.0, 8.0), (8.0, 6.0), (-6.0, -8.0), (10.000000000000002, 0.0), (9.999999999999998, 0.0)],
        ),
        1 => (7.0710678118654755, pick(random, &[7.0710678118654755, 7.071067811865475, 7.071067811865476])),
        2 => {
            let d = random.below(40) as f64 * 0.5;
            (d, if random.chance(2) { d } else { -d })
        }
        3 => (24.0, 32.0),
        4 => (random.below(30) as f64 - 15.0, random.below(30) as f64 - 15.0),
        5 => (random.next() as f64 / 1e8 - 20.0, random.next() as f64 / 1e8 - 20.0),
        6 => pick(random, &[(f64::NAN, 0.0), (0.0, f64::INFINITY), (1e300, 1e300), (-0.0, -0.0), (5e-324, 5e-324)]),
        _ => (random.below(400) as f64 - 200.0, random.below(400) as f64 - 200.0),
    };
    if random.chance(2) { (dx, dy) } else { (dy, dx) }
}

fn later(random: &mut Lcg, t: f64) -> f64 {
    let step = match random.below(8) {
        0 => 0.0,
        1 => pick(random, &[499.0, 499.99999999999994, 500.0, 500.00000000000006, 300.0, 300.00000000000006, 299.0, 301.0]),
        2 => random.below(600) as f64,
        3 => random.below(100) as f64 / 8.0,
        4 => pick(random, &[f64::NAN, -1.0, 1e9, -500.0]),
        _ => random.below(120) as f64,
    };
    t + step
}

#[test]
fn presses_alike() {
    let Some(oracle) = Oracle::find("gesture_policy_oracle") else { return };
    let mut random = Lcg::new(0x6e_0002);
    let mut c = Comparison::new();
    let mut seen = BTreeMap::new();
    for script in 0..4000 {
        c.setting(&Fields::new().framed(b'N'));
        let mut a = GestureArbiter::new();
        let mut t = pick(&mut random, &[0.0, 1e12, -100.0, 12345.5]);
        let (mut x0, mut y0) = (100.0, 50.0);
        let mut queued: Vec<u8> = Vec::new();
        for step in 0..1 + random.below(50) {
            let quick = !queued.is_empty();
            t = if quick { t + random.below(320) as f64 } else { later(&mut random, t) };
            let what = format!("script {script}, step {step}");
            let kind = if step == 0 {
                0
            } else if let Some(k) = queued.pop() {
                k
            } else {
                pick(&mut random, &[0u8, 0, 1, 1, 1, 1, 2, 3, 3, 3, 4])
            };
            let mut f = Fields::new();
            let v = match kind {
                0 => {
                    let near_last = quick || random.chance(2);
                    let (dx, dy) =
                        if quick { (random.below(60) as f64 - 30.0, random.below(60) as f64 - 30.0) } else { offset(&mut random) };
                    if near_last {
                        x0 += dx;
                        y0 += dy;
                    } else {
                        x0 = pick(&mut random, &[0.0, 19.0, 19.999999999999996, 20.0, 20.000000000000004, 21.0, 100.0, 300.0, -5.0]);
                        y0 = random.below(300) as f64;
                    }
                    let bits = random.below(8) as u8;
                    let pointer = pick(&mut random, &[0u8, 0, 0, 1, 2, 2]);
                    let button = pick(&mut random, &[0, 0, 0, 2, 2, 1, -1]);
                    let claim = WidgetGestureClaim { x: bits & 1 != 0, y: bits & 2 != 0, hold: bits & 4 != 0 };
                    let kinds = [PointerKind::Touch, PointerKind::Pen, PointerKind::Mouse];
                    f.f64(t).f64(x0).f64(y0).u8(bits).u8(pointer).i32(button);
                    let v = Some(a.down(t, x0, y0, claim, kinds[usize::from(pointer)], button));
                    c.case(
                        &f.framed(b'd'),
                        answer(&a, v),
                        format!("{what}: down at {t}, {x0}, {y0}, claim {bits}, pointer {pointer}, button {button}"),
                    );
                    v
                }
                1 | 3 => {
                    let (dx, dy) = if quick { (random.below(8) as f64, random.below(8) as f64) } else { offset(&mut random) };
                    let (x, y) = (x0 + dx, y0 + dy);
                    f.f64(t).f64(x).f64(y);
                    let v = if kind == 1 { a.move_to(t, x, y) } else { a.up(t, x, y) };
                    c.case(
                        &f.framed(if kind == 1 { b'm' } else { b'u' }),
                        answer(&a, v),
                        format!("{what}: {} at {t}, {x}, {y}", if kind == 1 { "move" } else { "up" }),
                    );
                    v
                }
                2 => {
                    f.f64(t);
                    let v = a.tick(t);
                    c.case(&f.framed(b't'), answer(&a, v), format!("{what}: tick at {t}"));
                    v
                }
                _ => {
                    a.cancel();
                    c.case(&f.framed(b'c'), answer(&a, None), format!("{what}: cancel"));
                    None
                }
            };
            if v.is_some_and(|v| v.gesture == Gesture::Tap) && random.chance(2) {
                queued = vec![3, 0];
            }
            if let Some(v) = v {
                *seen
                    .entry((v.owner.name(), v.gesture.name(), v.action.map(BubbleAction::name), v.event.map(GestureEvent::name)))
                    .or_insert(0usize) += 1;
            }
        }
    }
    let tally = c.check(&oracle);
    eprintln!("gesture presses: {} steps alike, {} kinds of verdict", tally[1], seen.len());
    for (k, n) in &seen {
        eprintln!("    {k:?}: {n}");
    }
    assert!(seen.len() >= 14, "{seen:?}");
}

#[test]
fn names_and_hypot_alike() {
    let Some(oracle) = Oracle::find("gesture_policy_oracle") else { return };
    let mut random = Lcg::new(0x6e_0003);
    let mut c = Comparison::new();
    for which in 0..5u8 {
        for value in 0..8u8 {
            let name = match which {
                0 => [PointerKind::Touch, PointerKind::Pen, PointerKind::Mouse][usize::from(value % 3)].name(),
                1 => [GestureOwner::Pending, GestureOwner::Widget, GestureOwner::Bubble][usize::from(value % 3)].name(),
                2 => [Gesture::Press, Gesture::Tap, Gesture::DoubleTap, Gesture::Hold, Gesture::DragX, Gesture::DragY, Gesture::MenuClick]
                    [usize::from(value % 7)]
                .name(),
                3 => [BubbleAction::Menu, BubbleAction::Swipe, BubbleAction::Scroll, BubbleAction::Back][usize::from(value % 4)].name(),
                _ => [GestureEvent::DblClick, GestureEvent::ContextMenu][usize::from(value % 2)].name(),
            };
            c.case(&Fields::new().u8(which).u8(value).framed(b'n'), (1, name.as_bytes().to_vec()), format!("name {which} {value}"));
        }
    }
    let specials = [
        0.0,
        -0.0,
        1.0,
        3.0,
        4.0,
        6.0,
        8.0,
        10.0,
        7.0710678118654755,
        5e-324,
        2.2250738585072014e-308,
        1e154,
        1e308,
        f64::MAX,
        f64::NAN,
        f64::INFINITY,
        f64::NEG_INFINITY,
    ];
    for _ in 0..60000 {
        let part = |random: &mut Lcg| match random.below(4) {
            0 => pick(random, &specials) * if random.chance(2) { 1.0 } else { -1.0 },
            1 => f64::from_bits((u64::from(random.next()) << 32) | u64::from(random.next())),
            2 => random.next() as f64 / 1e7,
            _ => (random.below(4000) as f64 - 2000.0) / 8.0,
        };
        let (x, y) = (part(&mut random), part(&mut random));
        let h = x.hypot(y);
        c.case(&Fields::new().f64(x).f64(y).framed(b'y'), (1, Fields::new().f64(h).take()), format!("hypot({x:e}, {y:e}) = {h:e}"));
    }
    let tally = c.check(&oracle);
    eprintln!("gesture names and hypot: {} answers alike", tally[0] + tally[1] + tally[2]);
}

fn widget_sandbox() -> PathBuf {
    std::env::var_os("TWIDGET_WIDGET_SANDBOX")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../.."))
}

/// The fixture's claims and press scripts on both, as given and with their numbers and values changed.
#[test]
fn fixture_alike() {
    let Some(oracle) = Oracle::find("gesture_policy_oracle") else { return };
    let bytes = std::fs::read(widget_sandbox().join("packages/core/tests/fixtures/gesture-policy.json")).expect("the fixture");
    let root = Json::parse(&bytes).expect("the fixture parses");
    let mut random = Lcg::new(0x6e_0004);
    let mut c = Comparison::new();
    for round in 0..60 {
        for (i, claim) in root["claims"].arr().iter().enumerate() {
            let mut list: Vec<Vec<u8>> = claim["touchActions"].arr().iter().map(|ta| ta.as_str().to_vec()).collect();
            if round > 0 && !list.is_empty() {
                let at = random.below(list.len());
                mutate(&mut random, &mut list[at]);
            }
            claim_case(&mut c, &list, claim["prevented"].as_bool(false), format!("fixture claim {i} round {round}"));
        }
        for case in root["cases"].arr() {
            c.setting(&Fields::new().framed(b'N'));
            let mut a = GestureArbiter::new();
            let nudge = |random: &mut Lcg, v: f64| {
                if round > 0 && random.chance(5) { v + pick(random, &[1.0, -1.0, 0.5, 10.0, -10.0, 1e-9]) } else { v }
            };
            for (i, s) in case["steps"].arr().iter().enumerate() {
                let what = format!("fixture {} round {round}, step {i}", case["name"].dump_lossy());
                let mut f = Fields::new();
                if let Some(p) = s.find("down") {
                    let (t, x, y) = (
                        nudge(&mut random, p[0].as_number(0.0)),
                        nudge(&mut random, p[1].as_number(0.0)),
                        nudge(&mut random, p[2].as_number(0.0)),
                    );
                    let pointer = match s["pointer"].as_str() {
                        b"pen" => 1u8,
                        b"mouse" => 2,
                        _ => 0,
                    };
                    let claim = if let Some(given) = s.find("claimed") {
                        WidgetGestureClaim {
                            x: given["x"].as_bool(false),
                            y: given["y"].as_bool(false),
                            hold: given["hold"].as_bool(false),
                        }
                    } else {
                        claim_of(&[if s["claim"].is_string() { s["claim"].as_str() } else { b"auto" }], false)
                    };
                    let bits = u8::from(claim.x) | u8::from(claim.y) << 1 | u8::from(claim.hold) << 2;
                    let bits = if round > 0 && random.chance(5) { random.below(8) as u8 } else { bits };
                    let claim = WidgetGestureClaim { x: bits & 1 != 0, y: bits & 2 != 0, hold: bits & 4 != 0 };
                    let button = s["button"].as_int(0);
                    let v =
                        a.down(t, x, y, claim, [PointerKind::Touch, PointerKind::Pen, PointerKind::Mouse][usize::from(pointer)], button);
                    c.case(&f.f64(t).f64(x).f64(y).u8(bits).u8(pointer).i32(button).framed(b'd'), answer(&a, Some(v)), what);
                } else if let Some(p) = s.find("move").or_else(|| s.find("up")) {
                    let (t, x, y) = (
                        nudge(&mut random, p[0].as_number(0.0)),
                        nudge(&mut random, p[1].as_number(0.0)),
                        nudge(&mut random, p[2].as_number(0.0)),
                    );
                    let up = s.find("up").is_some();
                    let v = if up { a.up(t, x, y) } else { a.move_to(t, x, y) };
                    c.case(&f.f64(t).f64(x).f64(y).framed(if up { b'u' } else { b'm' }), answer(&a, v), what);
                } else if let Some(t) = s.find("tick") {
                    let t = nudge(&mut random, t.as_number(0.0));
                    let v = a.tick(t);
                    c.case(&f.f64(t).framed(b't'), answer(&a, v), what);
                } else if s["cancel"].as_bool(false) {
                    a.cancel();
                    c.case(&f.framed(b'c'), answer(&a, None), what);
                }
            }
        }
    }
    let tally = c.check(&oracle);
    eprintln!("gesture fixture: {} answers alike", tally[1]);
}
