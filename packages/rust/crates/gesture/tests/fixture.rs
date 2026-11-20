//! widget-sandbox's gesture policy test (packages/desktop/tests/test_main.cpp: gesture_policy_shared_fixture
//! against packages/core/tests/fixtures/gesture-policy.json, read as the C++ reads it), ported.

use std::path::{Path, PathBuf};

use twidget_gesture::*;
use twidget_json::{Json, Type};

fn widget_sandbox() -> PathBuf {
    std::env::var_os("TWIDGET_WIDGET_SANDBOX")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../.."))
}

/// Deep equality of JSON values, object members in any order (what node:assert's deepEqual compares).
fn json_equal(a: &Json, b: &Json) -> bool {
    if a.kind() != b.kind() {
        return false;
    }
    match a.kind() {
        Type::Null => true,
        Type::Bool => a.as_bool(false) == b.as_bool(false),
        Type::Number => a.as_number(0.0) == b.as_number(0.0),
        Type::String => a.as_str() == b.as_str(),
        Type::Array => a.size() == b.size() && (0..a.size()).all(|i| json_equal(&a[i], &b[i])),
        Type::Object => a.size() == b.size() && a.obj().iter().all(|(k, v)| b.find(k).is_some_and(|w| json_equal(v, w))),
    }
}

fn claim_json(c: &WidgetGestureClaim) -> Json {
    let mut o = Json::object();
    o.set("x", c.x).set("y", c.y).set("hold", c.hold);
    o
}

/// The TS `GestureVerdict` shape (owner, gesture, cancel always; action, event, edge only when set), or null.
fn verdict_json(v: &Option<GestureVerdict>) -> Json {
    let Some(v) = v else { return Json::Null };
    let mut o = Json::object();
    o.set("owner", v.owner.name()).set("gesture", v.gesture.name()).set("cancel", v.cancel);
    if let Some(a) = v.action {
        o.set("action", a.name());
    }
    if let Some(e) = v.event {
        o.set("event", e.name());
    }
    if v.edge {
        o.set("edge", true);
    }
    o
}

/// Every section. A `down` step's claim is the `claimed` object {x, y, hold} as given, else claim_of([claim])
/// (default "auto"), as gesture-policy.test.ts reads it.
#[test]
fn gesture_policy_shared_fixture() {
    let path = widget_sandbox().join("packages/core/tests/fixtures/gesture-policy.json");
    let bytes = std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let root = Json::parse(&bytes).expect("the fixture parses");
    let constants: [(&str, f64); 5] = [
        ("TOUCH_SLOP_PX", TOUCH_SLOP_PX),
        ("HOLD_MS", HOLD_MS),
        ("DOUBLE_TAP_MS", DOUBLE_TAP_MS),
        ("DOUBLE_TAP_SLOP_PX", DOUBLE_TAP_SLOP_PX),
        ("EDGE_STRIP_PX", EDGE_STRIP_PX),
    ];
    assert_eq!(root["constants"].size(), constants.len());
    for (name, value) in root["constants"].obj() {
        let known = constants.iter().find(|c| c.0.as_bytes() == name.as_slice());
        assert!(known.is_some_and(|c| c.1 == value.as_number(0.0)), "{}", String::from_utf8_lossy(name));
    }
    let mut claims = 0;
    for c in root["claims"].arr() {
        claims += 1;
        let touch_actions: Vec<Vec<u8>> = c["touchActions"].arr().iter().map(|ta| ta.as_str().to_vec()).collect();
        let got = claim_json(&claim_of(&touch_actions, c["prevented"].as_bool(false)));
        assert!(
            json_equal(&got, &c["expect"]),
            "{} prevented {} → {}",
            c["touchActions"].dump_lossy(),
            c["prevented"].dump_lossy(),
            got.dump_lossy()
        );
    }
    let (mut cases, mut steps, mut claimed) = (0, 0, 0);
    for c in root["cases"].arr() {
        cases += 1;
        let mut a = GestureArbiter::new();
        for (i, s) in c["steps"].arr().iter().enumerate() {
            steps += 1;
            let place = format!("{}: step {i} {}", c["name"].dump_lossy(), s.dump_lossy().chars().take(80).collect::<String>());
            let got;
            if let Some(p) = s.find("down") {
                let pointer: &[u8] = if s["pointer"].is_string() { s["pointer"].as_str() } else { b"touch" };
                let kind = [PointerKind::Touch, PointerKind::Pen, PointerKind::Mouse].into_iter().find(|k| k.name().as_bytes() == pointer);
                assert!(kind.is_some(), "{place}: unknown pointer");
                let claim = if let Some(given) = s.find("claimed") {
                    claimed += 1;
                    assert!(
                        given.is_object() && given["x"].is_bool() && given["y"].is_bool() && given["hold"].is_bool(),
                        "{place}: claimed is not {{x, y, hold}}"
                    );
                    WidgetGestureClaim { x: given["x"].as_bool(false), y: given["y"].as_bool(false), hold: given["hold"].as_bool(false) }
                } else {
                    claim_of(&[if s["claim"].is_string() { s["claim"].as_str() } else { b"auto" }], false)
                };
                got = Some(a.down(
                    p[0].as_number(0.0),
                    p[1].as_number(0.0),
                    p[2].as_number(0.0),
                    claim,
                    kind.unwrap_or(PointerKind::Touch),
                    s["button"].as_int(0),
                ));
            } else if let Some(p) = s.find("move") {
                got = a.move_to(p[0].as_number(0.0), p[1].as_number(0.0), p[2].as_number(0.0));
            } else if let Some(p) = s.find("up") {
                got = a.up(p[0].as_number(0.0), p[1].as_number(0.0), p[2].as_number(0.0));
            } else if let Some(t) = s.find("tick") {
                got = a.tick(t.as_number(0.0));
            } else if s["cancel"].as_bool(false) {
                a.cancel();
                continue;
            } else if let Some(deadline) = s.find("holdDeadline") {
                let dj = a.hold_deadline().map_or(Json::Null, Json::Number);
                assert!(json_equal(&dj, deadline), "{place} → {}", dj.dump_lossy());
                continue;
            } else {
                panic!("{place}: unknown step");
            }
            let gj = verdict_json(&got);
            assert!(s.find("expect").is_some() && json_equal(&gj, &s["expect"]), "{place} → {}", gj.dump_lossy());
        }
    }
    eprintln!("    {claims} claims, {cases} cases, {steps} steps ({claimed} with a claim given as is)");
    assert!(claims >= 21 && cases >= 28 && steps >= 174 && claimed >= 3);
    let mut a = GestureArbiter::new();
    let v = a.down(0.0, 100.0, 50.0, claim_of(&["auto"], false), PointerKind::Touch, 0);
    a.tick(500.0);
    assert_eq!(verdict_json(&Some(v)).dump(), b"{\"owner\":\"pending\",\"gesture\":\"press\",\"cancel\":false}");
    assert_eq!(NO_CLAIM, WidgetGestureClaim { x: false, y: false, hold: false });
}
