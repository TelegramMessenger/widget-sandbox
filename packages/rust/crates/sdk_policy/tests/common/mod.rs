//! What the SDK policy's tests share: the fixture, read as test_main.cpp reads it, and its helpers.

#![allow(dead_code)]

use std::path::{Path, PathBuf};

use twidget_json::{Json, Type};

pub fn widget_sandbox() -> PathBuf {
    std::env::var_os("TWIDGET_WIDGET_SANDBOX")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../.."))
}

/// packages/core/tests/fixtures/sdk-policy.json, parsed.
pub fn fixture() -> Json {
    let path = widget_sandbox().join("packages/core/tests/fixtures/sdk-policy.json");
    let bytes = std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    Json::parse(&bytes).expect("the fixture parses")
}

/// Deep equality of JSON values, object members in any order (what node:assert's deepEqual compares).
pub fn json_equal(a: &Json, b: &Json) -> bool {
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

/// `{ "repeat": [s, n] }` in the fixture stands for s repeated n times.
pub fn expand_repeat(v: &Json) -> Json {
    if v.is_array() {
        let mut out = Json::array();
        for x in v.arr() {
            out.push(expand_repeat(x));
        }
        return out;
    }
    if v.is_object() {
        if v.size() == 1 && v["repeat"].is_array() {
            let mut s = Vec::new();
            for _ in 0..v["repeat"][1].as_int(0) {
                s.extend_from_slice(v["repeat"][0].as_str());
            }
            return Json::String(s);
        }
        let mut out = Json::object();
        for (k, x) in v.obj() {
            out.set(k.clone(), expand_repeat(x));
        }
        return out;
    }
    v.clone()
}

pub fn or_null(s: Option<Vec<u8>>) -> Json {
    s.map_or(Json::Null, Json::String)
}

pub fn shown(v: &Json) -> String {
    let text = v.dump_lossy();
    text.chars().take(100).collect()
}
