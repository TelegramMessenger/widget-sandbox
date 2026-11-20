//! widget-sandbox's SDK policy tests (packages/desktop/tests/test_main.cpp: sdk_policy_shared_fixture against
//! packages/core/tests/fixtures/sdk-policy.json, read as the C++ reads it;
//! sdk_request_strings_refuse_surrogates_and_invalid_utf8, sdk_link_refuses_surrogates_and_invalid_utf8, the
//! SDK's part of strict_limit_defaults and of bundle_stamp_shipped_and_parsed), ported. A
//! `WidgetRequestFailure(42)` has no counterpart: a Rust enum holds only its own values.

mod common;

use common::*;
use twidget_json::{Json, Surrogates};
use twidget_sdk_policy::*;

/// A fixture's data input as a native host passes it: each field a string, or none for null / absent.
fn widget_data_of(input: &Json) -> WidgetData {
    let field = |v: &Json| if v.is_string() { Some(v.as_str().to_vec()) } else { None };
    WidgetData { chat_data: field(&input["chatData"]), public_data: field(&input["publicData"]) }
}

fn note_or(c: &Json, input: &Json, n: usize) -> String {
    if c["note"].is_string() {
        String::from_utf8_lossy(c["note"].as_str()).into_owned()
    } else {
        input.dump_lossy().chars().take(n).collect()
    }
}

const ALL_VERDICTS: [RequestVerdict; 9] = [
    RequestVerdict::Foreground,
    RequestVerdict::Background,
    RequestVerdict::Held,
    RequestVerdict::Granted,
    RequestVerdict::Disabled,
    RequestVerdict::NoWallet,
    RequestVerdict::Busy,
    RequestVerdict::NoActivation,
    RequestVerdict::RateLimited,
];

#[test]
fn sdk_policy_shared_fixture() {
    let root = fixture();
    let mut cases = 0;
    let check = |ok: bool, what: String| assert!(ok, "{what}");
    for c in root["links"].arr() {
        cases += 1;
        let got = or_null(check_link_value(c["method"].as_str(), &c["url"]));
        check(json_equal(&got, &c["expect"]), format!("{} {} → {}", c["method"].dump_lossy(), shown(&c["url"]), shown(&got)));
    }
    for c in root["haptics"].arr() {
        cases += 1;
        let mut got = Json::Null;
        if let Some(h) = check_haptic(&c["args"]) {
            got = Json::object();
            got.set("haptic", h.haptic);
            if let Some(v) = h.value {
                got.set("value", v);
            }
        }
        check(json_equal(&got, &c["expect"]), format!("{} → {}", c["args"].dump_lossy(), got.dump_lossy()));
    }
    let (mut anchors, mut requests, mut copies, mut watches, mut wallets) = (0, 0, 0, 0, 0);
    for c in root["bridge"].arr() {
        cases += 1;
        let a = check_bridge_call(&c["method"], &c["args"]);
        let got = a.as_ref().map_or(Json::Null, BridgeCall::to_json);
        check(json_equal(&got, &c["expect"]), format!("{} {} → {}", c["method"].dump_lossy(), shown(&c["args"]), shown(&got)));
        if let Some(a) = &a {
            let set = usize::from(a.action.is_some())
                + usize::from(a.resize_anchor.is_some())
                + usize::from(a.request.is_some())
                + usize::from(a.copy_text.is_some())
                + usize::from(a.watch_public_data.is_some())
                + usize::from(a.wallet.is_some());
            check(set == 1, c["method"].dump_lossy());
            wallets += usize::from(a.wallet.is_some());
        }
        let method: &[u8] = if c["method"].is_string() { c["method"].as_str() } else { b"" };
        anchors += usize::from(method == b"setResizeAnchor");
        requests += usize::from(method == b"request");
        copies += usize::from(method == b"copyText");
        watches += usize::from(method == b"watchPublicData");
    }
    check(anchors >= 15, format!("{anchors} setResizeAnchor entries"));
    check(requests >= 19 && copies >= 12, format!("{requests} request, {copies} copyText entries"));
    check(watches >= 6, format!("{watches} watchPublicData entries"));
    let mut touch_steps = 0;
    for c in root["hapticGate"].arr() {
        cases += 1;
        let mut gate = HapticGate::new();
        for (i, s) in c["steps"].arr().iter().enumerate() {
            if s["touch"].is_string() {
                touch_steps += 1;
                if s["touch"].as_str() == b"down" {
                    gate.press(s["at"].as_number(0.0));
                } else {
                    gate.release(s["at"].as_number(0.0));
                }
            } else {
                let got = gate.decide(s["at"].as_number(0.0), s["visible"].as_bool(false)).name();
                check(got.as_bytes() == s["expect"].as_str(), format!("{}, step {i}: {got}", c["name"].dump_lossy()));
            }
        }
    }
    check(touch_steps >= 6, format!("{touch_steps} touch steps"));
    for c in root["themeParams"].arr() {
        cases += 1;
        let got = theme_params_json(&check_theme_params(&c["input"]));
        check(json_equal(&got, &c["expect"]), format!("{} → {}", c["input"].dump_lossy(), got.dump_lossy()));
    }
    for c in root["locales"].arr() {
        cases += 1;
        let got = or_null(check_locale_value(&c["input"]));
        check(json_equal(&got, &c["expect"]), format!("{} → {}", c["input"].dump_lossy(), got.dump_lossy()));
    }
    for c in root["storage"].arr() {
        cases += 1;
        let got = check_storage_commit(&expand_repeat(&c["input"])).map_or(Json::Null, |commit| commit.to_json());
        check(json_equal(&got, &expand_repeat(&c["expect"])), note_or(c, &c["input"], 1000));
    }
    for c in root["utf8"].arr() {
        cases += 1;
        check(utf8_length(c["s"].as_str()) == c["bytes"].as_int(0) as usize, c["s"].dump_lossy());
    }
    for c in root["widgetData"].arr() {
        cases += 1;
        let got = check_widget_data(&widget_data_of(&expand_repeat(&c["input"])), c["copy"].as_bool(false)).to_json();
        check(json_equal(&got, &expand_repeat(&c["expect"])), format!("{} → {}", note_or(c, &c["input"], 80), shown(&got)));
    }
    let mut viewer_shapes = 0;
    for c in root["viewer"].arr() {
        cases += 1;
        let input = expand_repeat(&c["input"]);
        let viewer = input["id"].is_number().then(|| {
            let field = |v: &Json| if v.is_string() { Some(v.as_str().to_vec()) } else { None };
            WidgetViewer {
                id: input["id"].as_number(0.0) as i64,
                first_name: field(&input["first_name"]),
                last_name: field(&input["last_name"]),
                username: field(&input["username"]),
                is_premium: input["is_premium"].as_bool(false),
            }
        });
        let got = check_viewer(viewer.as_ref()).to_json();
        check(json_equal(&got, &expand_repeat(&c["expect"])), format!("{} → {}", note_or(c, &input, 80), shown(&got)));
        viewer_shapes += usize::from(c["expect"].find("rejected").is_some());
    }
    check(viewer_shapes >= 20, format!("{viewer_shapes} viewer cases with a rejected list"));
    for c in root["chatType"].arr() {
        cases += 1;
        let got = or_null(check_chat_type(c["input"].as_str()));
        check(json_equal(&got, &c["expect"]), format!("{} → {}", c["input"].dump_lossy(), got.dump_lossy()));
    }
    for c in root["answers"].arr() {
        cases += 1;
        let input = expand_repeat(&c["input"]);
        let answer = WidgetRequestAnswer { result: input["result"].is_string().then(|| input["result"].as_str().to_vec()) };
        let got = check_request_answer(&answer).map_or(Json::Null, |a| a.to_json());
        check(json_equal(&got, &expand_repeat(&c["expect"])), format!("{} → {}", note_or(c, &input, 80), shown(&got)));
    }
    for c in root["requestGate"].arr() {
        cases += 1;
        let mut gate = RequestGate::new();
        let name = c["name"].dump_lossy();
        for (i, s) in c["steps"].arr().iter().enumerate() {
            let op = s["op"].as_str();
            if op == b"finish" {
                gate.finish(s["answeredAt"].is_number().then(|| s["answeredAt"].as_number(0.0)));
            } else if op == b"decideWallet" {
                let at = WalletConditions {
                    gesture: s["gesture"].as_bool(false),
                    visible: s["visible"].as_bool(false),
                    can_query: s["canQuery"].as_bool(false),
                    wallet: s["wallet"].as_bool(false),
                };
                let got = request_verdict_name(gate.decide_wallet(s["at"].as_number(0.0), &at));
                check(got.as_bytes() == s["expect"].as_str(), format!("{name}, step {i}: {got}"));
            } else if op == b"show" {
                let busy = gate.busy();
                check(gate.show(s["at"].as_number(0.0)) == s["expect"].as_bool(false), format!("{name}, step {i}"));
                check(gate.busy() == busy, format!("{name}, step {i}: show changes nothing in flight"));
            } else {
                let at = RequestConditions {
                    gesture: s["gesture"].as_bool(false),
                    visible: s["visible"].as_bool(false),
                    can_query: s["canQuery"].as_bool(false),
                    background_query: s["backgroundQuery"].as_bool(false),
                    with_wallet: s["withWallet"].as_bool(false),
                    wallet: s["wallet"].as_bool(false),
                };
                let got = request_verdict_name(gate.decide(s["at"].as_number(0.0), &at));
                check(got.as_bytes() == s["expect"].as_str(), format!("{name}, step {i}: {got}"));
            }
        }
        check(!c["steps"].arr().is_empty(), name);
    }
    let mut verdicts = Vec::new();
    for c in root["requestRefusals"].arr() {
        cases += 1;
        let v = ALL_VERDICTS.iter().find(|x| request_verdict_name(**x).as_bytes() == c["verdict"].as_str());
        check(v.is_some(), c["verdict"].dump_lossy());
        let Some(v) = v else { continue };
        verdicts.push(c["verdict"].as_str().to_vec());
        let got = request_refusal(*v).map_or(Json::Null, Json::from);
        check(json_equal(&got, &c["expect"]), format!("{} → {}", c["verdict"].dump_lossy(), got.dump_lossy()));
    }
    check(verdicts.len() == 9, format!("{} verdicts", verdicts.len()));
    check(wallets == 3, format!("{wallets} valid wallet calls"));
    for c in root["walletTransactions"].arr() {
        cases += 1;
        let got = check_wallet_transaction(&expand_repeat(&c["input"])).map_or(Json::Null, |tx| tx.to_json());
        check(json_equal(&got, &expand_repeat(&c["expect"])), format!("{} → {}", note_or(c, &c["input"], 100), shown(&got)));
    }
    for c in root["signDataPayloads"].arr() {
        cases += 1;
        let got = check_sign_data_payload(&expand_repeat(&c["input"])).map_or(Json::Null, |p| p.to_json());
        check(json_equal(&got, &expand_repeat(&c["expect"])), format!("{} → {}", note_or(c, &c["input"], 100), shown(&got)));
    }
    let (mut flagged, mut shows, mut nul_data, mut edits) = (0, 0, 0, 0);
    for c in root["requestGate"].arr() {
        for st in c["steps"].arr() {
            flagged += usize::from(st.find("canQuery").is_some());
            shows += usize::from(st["op"].as_str() == b"show");
        }
    }
    for c in root["widgetData"].arr() {
        nul_data += usize::from(c.find("copy").is_some() && c["input"].dump().windows(6).any(|w| w == b"\\u0000"));
    }
    for c in root["answers"].arr() {
        edits += usize::from(c["expect"].find("edit").is_some());
    }
    check(
        flagged >= 10 && shows >= 2 && nul_data >= 1 && edits == 0,
        format!("{flagged} gate steps with canQuery, {shows} show steps, {nul_data} NUL data cases, {edits} answers with an edit"),
    );
    for c in root["previewMethods"].arr() {
        cases += 1;
        check(allowed_in_preview(c["method"].as_str()) == c["allowed"].as_bool(false), c["method"].dump_lossy());
    }
    eprintln!("    {cases} fixture cases");
    check(cases > 150, format!("{cases} cases"));
    for section in [
        "links",
        "haptics",
        "bridge",
        "hapticGate",
        "themeParams",
        "locales",
        "storage",
        "utf8",
        "widgetData",
        "viewer",
        "chatType",
        "answers",
        "requestGate",
        "requestRefusals",
        "previewMethods",
        "walletTransactions",
        "signDataPayloads",
    ] {
        check(root[section].size() > 0, section.to_string());
    }
}

fn bytes(s: &str) -> Vec<u8> {
    s.as_bytes().to_vec()
}

#[test]
fn sdk_request_strings_refuse_surrogates_and_invalid_utf8() {
    let lone: &[u8] = b"\xED\xA0\x80";
    let lone_low: &[u8] = b"\xED\xB0\x80";
    let emoji: &[u8] = b"\xF0\x9F\x98\x80";
    for bad in [
        [b"a", lone].concat(),
        [lone_low, b"b"].concat(),
        [lone, lone_low].concat(),
        b"\xFF".to_vec(),
        b"x\xC3".to_vec(),
        b"\xC0\xAF".to_vec(),
    ] {
        assert!(check_request_data(&bad).is_none(), "{bad:?}");
        assert!(check_copy_text(&bad).is_none(), "{bad:?}");
        assert!(check_widget_data_value(&bad).is_none(), "{bad:?}");
        assert!(check_request_answer(&WidgetRequestAnswer { result: Some(bad.clone()) }).is_none());
        let d = check_widget_data(&WidgetData { chat_data: Some(bad.clone()), public_data: Some(bytes("ok")) }, false);
        assert!(d.chat_data.is_none() && d.public_data == Some(bytes("ok")) && d.rejected == ["chatData"]);
    }
    assert_eq!(check_request_data(emoji).as_deref(), Some(emoji));
    assert!(check_request_data(b"a\0b").is_some());
    let call = Json::parse_with("[\"request\",[1,\"a\\ud800\"]]", Surrogates::Keep, 128).expect("parses");
    assert!(check_bridge_call(&call[0], &call[1]).is_none());
    let call = Json::parse_with("[\"copyText\",[\"\\udc00\"]]", Surrogates::Keep, 128).expect("parses");
    assert!(check_bridge_call(&call[0], &call[1]).is_none());
    assert!(utf16_length(emoji) == 2 && utf16_length(b"\xD0\xB6") == 1 && utf16_length(&[b"a", emoji, b"\xE2\x82\xAC"].concat()) == 4);
    let emojis = emoji.repeat(128);
    let cyrillic = b"\xD0\xB6".repeat(256);
    assert!(check_copy_text(&emojis).is_some() && check_copy_text(&[&emojis[..], b"a"].concat()).is_none());
    assert!(check_copy_text(&cyrillic).is_some() && check_copy_text(&[&cyrillic[..], b"a"].concat()).is_none());
    for args in ["[1e0,\"x\"]", "[2147483647.0,\"x\"]"] {
        assert!(check_bridge_call(&Json::from("request"), &Json::parse(args).expect("parses")).is_some(), "{args}");
    }
    for args in ["[0.5,\"x\"]", "[1e10,\"x\"]", "[true,\"x\"]", "[null,\"x\"]", "[[1],\"x\"]", "[-0,\"x\"]"] {
        assert!(check_bridge_call(&Json::from("request"), &Json::parse(args).expect("parses")).is_none(), "{args}");
    }
    let viewer = |id: i64| WidgetViewer { id, ..WidgetViewer::default() };
    assert!(check_viewer(Some(&viewer(i64::MIN))).viewer.is_none() && check_viewer(Some(&viewer(i64::MAX))).viewer.is_none());
    assert!(check_viewer(None).viewer.is_none());
    assert_eq!(check_viewer(Some(&viewer(MAX_VIEWER_ID))).viewer.map(|v| v.id), Some(MAX_VIEWER_ID));
    for bad in [[b"A", lone].concat(), b"\xFF".to_vec(), b"x\xC3".to_vec()] {
        let v = WidgetViewer { first_name: Some(bad.clone()), last_name: Some(bytes("Lee")), ..viewer(7) };
        let c = check_viewer(Some(&v));
        let seen = c.viewer.as_ref().expect("a viewer");
        assert!(seen.first_name.is_none() && seen.last_name == Some(bytes("Lee")) && c.rejected == ["first_name"], "{bad:?}");
    }
    {
        let v = WidgetViewer { username: Some(b"\xD0\xB0nnax".to_vec()), is_premium: false, ..viewer(7) };
        let c = check_viewer(Some(&v));
        let seen = c.viewer.as_ref().expect("a viewer");
        assert!(seen.username.is_none() && c.rejected == ["username"]);
        assert_eq!(viewer_json(seen).dump(), b"{\"id\":7}");
        let names = b"\xD0\xB6".repeat(64);
        let v = WidgetViewer { first_name: Some(names.clone()), ..viewer(7) };
        assert_eq!(check_viewer(Some(&v)).viewer.and_then(|v| v.first_name), Some(names.clone()));
        let v = WidgetViewer { first_name: Some([&names[..], b"a"].concat()), ..viewer(7) };
        assert_eq!(check_viewer(Some(&v)).rejected, ["first_name"]);
    }
    assert!(
        viewer_field_rule(b"first_name") == "not text of 1–64 UTF-16 code units"
            && viewer_field_rule(b"username") == "not 5–32 characters of A–Z, a–z, 0–9 and _"
            && viewer_field_rule(b"is_premium") == "not a boolean"
    );
    assert!(
        request_failure_code(WidgetRequestFailure::Timeout) == "TIMEOUT"
            && request_failure_code(WidgetRequestFailure::RateLimited) == "RATE_LIMITED"
    );
    assert!(request_failure_code(WidgetRequestFailure::Failed) == "FAILED");
    assert!(is_privileged_method(b"copyText") && !is_privileged_method(b"request"));
}

#[test]
fn sdk_link_refuses_surrogates_and_invalid_utf8() {
    let link = |url: &[u8]| check_link(b"openLink", url).unwrap_or_else(|| b"null".to_vec());
    assert_eq!(link(b"https://example.com/\xC3\xA9"), b"https://example.com/%C3%A9");
    assert_eq!(link(b"https://example.com/\xF0\x9F\x98\x80"), b"https://example.com/%F0%9F%98%80");
    assert_eq!(link(b"https://example.com/\xED\x9F\xBF"), b"https://example.com/%ED%9F%BF");
    assert_eq!(link(b"https://example.com/\xEE\x80\x80"), b"https://example.com/%EE%80%80");
    for bad in [
        &b"https://example.com/\xED\xA0\x80"[..],
        b"https://example.com/\xED\xBF\xBF",
        b"https://example.com/\xED\xA0\xBD\xED\xB8\x80",
        b"https://example.com/\xC3",
        b"https://example.com/\xC3x",
        b"https://example.com/\x80",
        b"https://example.com/\xFF",
        b"https://example.com/\xC0\xAF",
        b"https://example.com/\xE0\x80\xAF",
        b"https://example.com/\xF0\x80\x80\xAF",
        b"https://example.com/\xF4\x90\x80\x80",
        b"https://example.com/\xF8\x88\x80\x80\x80",
    ] {
        assert_eq!(link(bad), b"null", "{bad:?}");
    }
    assert_eq!(link(b"https://example.com/a\0b"), b"null");
    let keep = |text: &str| Json::parse_with(text, Surrogates::Keep, 128).expect("parses");
    assert_eq!(keep("\"\\ud800\"").as_str(), b"\xED\xA0\x80");
    assert_eq!(keep("\"\\udc00x\"").as_str(), b"\xED\xB0\x80x");
    assert_eq!(keep("\"\\ud83d\\ude00\"").as_str(), b"\xF0\x9F\x98\x80");
    assert_eq!(Json::parse("\"\\ud800\"").expect("parses").as_str(), b"\xEF\xBF\xBD");
    assert_eq!(Json::parse("\"\\ud800\\u0041\"").expect("parses").as_str(), b"\xEF\xBF\xBDA");
    assert_eq!(keep("\"\\ud800\\u0041\"").as_str(), b"\xED\xA0\x80A");
    let call = keep("[\"openLink\",[\"https://example.com/\\ud800\"]]");
    assert!(check_bridge_call(&call[0], &call[1]).is_none());
    assert_eq!(utf8_length(keep("\"\\udc00\"").as_str()), 3);
    assert_eq!(link(b"https://\xE2\x84\xAAelvin.com/"), b"null");
}

/// The SDK's share of the defaults every host shares.
#[test]
fn strict_limit_defaults() {
    assert_eq!(STORAGE_QUOTA_BYTES, 4096);
}

/// The profile the SDK enforces is the glue's (bundle_stamp_shipped_and_parsed's `PROFILE_VERSION` check).
#[test]
fn profile_version_is_the_glues() {
    assert_eq!(PROFILE_VERSION, twidget_bundle_stamp::version::PROFILE);
}
