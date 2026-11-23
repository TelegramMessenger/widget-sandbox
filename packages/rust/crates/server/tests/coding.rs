//! The wire forms of `coding.rs` against the C++ suites' expectations for them, ported by name from the coding tests
//! of `packages/desktop/tests/test_remote.cpp` and `test_hosted.cpp`: the checks of the bot's profile page's members
//! (the place inside the config's context, the device's edges beside the content's, the hosted start's viewport and
//! the standalone device form), with the same inputs and expectations. The C++ client's own forms reach these
//! decoders when the C++ suite runs against this end (`TW_ENGINE_STREAM=1 TW_ENGINE_END=rust`,
//! `TW_ENGINE_HOSTED=stream TW_ENGINE_END=rust`).

use std::collections::{BTreeMap, BTreeSet};

use twidget_engine::{
    SandboxConfig, SandboxHostedStart, WidgetButtonTokens, WidgetContentSafeArea, WidgetSafeArea, WidgetTextTokens, WidgetTheme,
};
use twidget_json::Json;
use twidget_sdk_policy::{WidgetContext, WidgetData, WidgetStorage, WidgetSurface, WidgetViewer};
use twidget_server::coding::*;
use twidget_stream::REMOTE_MESSAGE_LIMIT;
use twidget_testkit::Lcg;

/// test_remote.cpp's `JsonDamage`: the edits json_fuzz_never_crashes makes to its seeds, plus any byte, 1–3 per text.
struct JsonDamage(Lcg);

impl JsonDamage {
    fn damage(&mut self, mut text: Vec<u8>) -> Vec<u8> {
        const ALPHABET: &[u8] = b"{}[]\":,.-+eE0123456789truefalsn \\u/abcdef\x00\x80\xff";
        let edits = 1 + self.0.next() % 3;
        let mut e = 0;
        while e < edits && !text.is_empty() {
            let at = self.0.next() as usize % text.len();
            match self.0.next() % 5 {
                0 => text[at] = ALPHABET[self.0.next() as usize % ALPHABET.len()],
                1 => {
                    let n = 1 + self.0.next() as usize % 4;
                    text.drain(at..(at + n).min(text.len()));
                }
                2 => text.insert(at, ALPHABET[self.0.next() as usize % ALPHABET.len()]),
                3 => text[at] = (self.0.next() & 0xFF) as u8,
                _ => text.truncate(at),
            }
            e += 1;
        }
        text
    }
}

/// Damages `wire` `rounds` times: nothing panics, and what a decoder accepts encodes to a form that decodes back to
/// itself. Returns how many it accepted.
fn damage_round_trips<T>(wire: &[u8], rounds: usize, seed: u64, decode: fn(&[u8]) -> Option<T>, encode: impl Fn(&T) -> Vec<u8>) -> usize {
    let mut damage = JsonDamage(Lcg::new(seed));
    let mut accepted = 0;
    for _ in 0..rounds {
        let Some(decoded) = decode(&damage.damage(wire.to_vec())) else { continue };
        accepted += 1;
        let again = encode(&decoded);
        let twice = decode(&again);
        assert!(twice.is_some_and(|t| encode(&t) == again), "{}", String::from_utf8_lossy(&again[..again.len().min(200)]));
    }
    accepted
}

/// How many proper prefixes of `wire` decode: a cut message must give nothing.
fn decodable_prefixes<T>(wire: &[u8], decode: fn(&[u8]) -> Option<T>) -> usize {
    (0..wire.len()).filter(|&length| decode(&wire[..length]).is_some()).count()
}

/// `wire` padded with spaces to exactly the message limit decodes, one byte more does not.
fn bounded_at_the_message_limit<T>(wire: &[u8], decode: fn(&[u8]) -> Option<T>) -> bool {
    let mut padded = wire.to_vec();
    padded.resize(REMOTE_MESSAGE_LIMIT, b' ');
    let at_limit = decode(&padded).is_some();
    padded.push(b' ');
    at_limit && decode(&padded).is_none()
}

/// No object in `v` names a member twice.
fn unique_keys(v: &Json) -> bool {
    if v.is_array() {
        return v.arr().iter().all(unique_keys);
    }
    let mut seen = BTreeSet::new();
    v.obj().iter().all(|(k, x)| seen.insert(k.clone()) && unique_keys(x))
}

fn nested_arrays(depth: usize) -> String {
    format!("{}0{}", "[".repeat(depth), "]".repeat(depth))
}

fn text(s: &str) -> Vec<u8> {
    s.as_bytes().to_vec()
}

fn map(pairs: &[(&[u8], &[u8])]) -> BTreeMap<Vec<u8>, Vec<u8>> {
    pairs.iter().map(|(k, v)| (k.to_vec(), v.to_vec())).collect()
}

/// test_remote.cpp's `everyConfigField`: every field away from its default, the strings with a NUL, a WTF-8 lone
/// surrogate and invalid UTF-8 where a host may pass them.
fn every_config_field() -> SandboxConfig {
    SandboxConfig {
        memory_limit_bytes: 12 << 20,
        cpu_limit_ms: 750.5,
        cpu_quota_ms: 250.0,
        cpu_quota_window_ms: 2000.0,
        cpu_kill_after_ms: 9000.0,
        main_thread_quota_ms: 80.25,
        max_timers: 12,
        visible: false,
        inner_width: 320.0,
        inner_height: 240.5,
        device_pixel_ratio: 2.75,
        theme: text("dark"),
        theme_params: map(&[(b"bg_color", b"#000000"), (b"text_color", b"#FFFFFF")]),
        locale: text("ru-RU"),
        content_safe_area: WidgetContentSafeArea { top: 1.0, right: 2.0, bottom: 3.5, left: 4.0, corner_left: 5.0, corner_right: 6.0 },
        safe_area: WidgetSafeArea { top: 47.0, right: 8.5, bottom: 34.0, left: 9.0 },
        storage: WidgetStorage {
            local: map(&[(b"a", b"b"), (b"lone", b"\xED\xA0\x80"), (b"bytes", b"\xFF\xC3")]),
            session: map(&[(b"nul", b"d\0e")]),
        },
        data: WidgetData { chat_data: Some(text("chat")), public_data: Some(text("public\x01")) },
        context: WidgetContext {
            is_copy: true,
            can_query: true,
            background_query: true,
            preview: true,
            wallet: true,
            chat_type: text("group"),
            viewer: Some(WidgetViewer {
                id: 42,
                first_name: Some(text("Ann")),
                last_name: Some(text("Lee")),
                username: Some(text("ann_lee")),
                is_premium: true,
            }),
            surface: WidgetSurface::Profile,
        },
        policy: text("compat"),
        features: vec![text("webAnimations"), text("binaryOps"), text("canvasWords")],
    }
}

fn device(top: f64, right: f64, bottom: f64, left: f64) -> WidgetSafeArea {
    WidgetSafeArea { top, right, bottom, left }
}

#[test]
fn remote_config_coding_round_trips_and_survives_damage() {
    let full = every_config_field();
    let wire = encode_config(&full);
    assert_eq!(decode_config(&wire), Some(full.clone()));
    let parsed = Json::parse(&wire).expect("the config's form parses");
    assert!(parsed.is_object() && parsed.size() == 21 && unique_keys(&parsed), "{}", String::from_utf8_lossy(&wire));
    // The device's edges beside the content's, the place inside the context.
    assert_eq!(String::from_utf8_lossy(&parsed.get("safeArea").dump()), "{\"top\":47,\"right\":8.5,\"bottom\":34,\"left\":9}");
    assert_eq!(parsed.get("context").get("surface").as_str(), b"profile");
    let default_wire = encode_config(&SandboxConfig::default());
    let defaults = decode_config(&default_wire).expect("the default config decodes");
    assert_eq!(defaults, SandboxConfig::default());
    assert!(defaults.context.viewer.is_none() && defaults.data.chat_data.is_none() && defaults.data.public_data.is_none());
    let default_parsed = Json::parse(&default_wire).expect("the default config's form parses");
    assert_eq!(String::from_utf8_lossy(&default_parsed.get("safeArea").dump()), "{\"top\":0,\"right\":0,\"bottom\":0,\"left\":0}");
    assert_eq!(default_parsed.get("context").get("surface").as_str(), b"message");
    // Each new member alone: a page without the device's edges, the device's edges on a bubble (carried as given; the
    // engine and the renderer bound them).
    let mut page = SandboxConfig::default();
    page.context.surface = WidgetSurface::Profile;
    let edged = SandboxConfig { safe_area: device(0.0, 59.0, 21.0, 59.0), ..SandboxConfig::default() };
    for one in [page, edged] {
        assert_eq!(decode_config(&encode_config(&one)), Some(one));
    }

    let required = "\"innerWidth\":320,\"innerHeight\":240,\"devicePixelRatio\":2,\"theme\":\"dark\"";
    let with = |members: &str| decode_config(format!("{{{required}{}{members}}}", if members.is_empty() { "" } else { "," }).as_bytes());
    let minimal =
        SandboxConfig { inner_width: 320.0, inner_height: 240.0, device_pixel_ratio: 2.0, theme: text("dark"), ..SandboxConfig::default() };
    assert_eq!(with(""), Some(minimal.clone()));

    // Optional members of another type keep their defaults.
    let wrong = with(&format!(
        "{}{}",
        concat!(
            "\"memoryLimitBytes\":\"1\",\"cpuLimitMs\":true,\"cpuQuotaMs\":null,\"cpuQuotaWindowMs\":[],\"cpuKillAfterMs\":{},",
            "\"mainThreadQuotaMs\":1e999,\"maxTimers\":\"7\",\"visible\":0,\"themeParams\":{\"bg_color\":1},\"locale\":5,",
            "\"contentSafeArea\":[1],\"safeArea\":[1],\"storage\":\"x\",\"data\":[],\"context\":1,\"policy\":1,",
            "\"features\":[\"webAnimations\",1],\"unknown\":"
        ),
        nested_arrays(MAX_DEPTH as usize - 1)
    ));
    assert_eq!(wrong, Some(minimal.clone()));
    let inner = with(concat!(
        "\"storage\":{\"local\":{\"a\":1},\"session\":{\"b\":\"c\"}},\"data\":{\"chatData\":1,\"publicData\":\"p\"},",
        "\"contentSafeArea\":{\"top\":\"1\",\"bottom\":2},",
        "\"safeArea\":{\"top\":\"1\",\"right\":null,\"bottom\":34,\"left\":1e999,\"cornerLeft\":5},",
        "\"context\":{\"isCopy\":1,\"canQuery\":true,\"chatType\":7,\"viewer\":{\"id\":\"42\"},\"surface\":\"PROFILE\"}"
    ))
    .expect("the inner members' form decodes");
    assert!(inner.storage.local.is_empty() && inner.storage.session == map(&[(b"b", b"c")]));
    assert!(inner.data.chat_data.is_none() && inner.data.public_data == Some(text("p")));
    assert!(inner.content_safe_area.top == 0.0 && inner.content_safe_area.bottom == 2.0);
    assert!(!inner.context.is_copy && inner.context.can_query && inner.context.chat_type.is_empty() && inner.context.viewer.is_none());
    assert_eq!(inner.safe_area, device(0.0, 0.0, 34.0, 0.0));
    assert_eq!(inner.context.surface, WidgetSurface::Message);
    // A form from before the place and the device's edges (no `surface`, no `safeArea`) is a bubble without them.
    let legacy = with(concat!(
        "\"contentSafeArea\":{\"top\":0,\"right\":0,\"bottom\":24,\"left\":0,\"cornerLeft\":0,\"cornerRight\":64},",
        "\"context\":{\"isCopy\":false,\"canQuery\":true,\"backgroundQuery\":false,\"preview\":false,\"wallet\":false,",
        "\"chatType\":\"group\"}"
    ))
    .expect("a legacy form decodes");
    assert_eq!(legacy.context.surface, WidgetSurface::Message);
    assert_eq!(legacy.safe_area, WidgetSafeArea::default());
    assert!(legacy.context.can_query && legacy.context.chat_type == b"group");
    assert!(legacy.content_safe_area.bottom == 24.0 && legacy.content_safe_area.corner_right == 64.0);
    // The place: exactly "profile" is a page; another case, another type or none is a bubble.
    for (value, is_page) in [
        ("\"profile\"", true),
        ("\"message\"", false),
        ("\"PROFILE\"", false),
        ("\"Profile\"", false),
        ("\"profile \"", false),
        ("\"\"", false),
        ("1", false),
        ("true", false),
        ("null", false),
        ("[\"profile\"]", false),
        ("{\"surface\":\"profile\"}", false),
    ] {
        let c = with(&format!("\"context\":{{\"surface\":{value}}}")).expect(value);
        assert_eq!(c.context.surface, if is_page { WidgetSurface::Profile } else { WidgetSurface::Message }, "{value}");
    }
    // The device's edges: any finite side as given (negative, fractional, huge), anything else 0; a member that is no
    // object keeps the default.
    let odd = with("\"safeArea\":{\"top\":-5,\"right\":0.25,\"bottom\":1e300,\"left\":4000.5}").expect("odd edges decode");
    assert_eq!(odd.safe_area, device(-5.0, 0.25, 1e300, 4000.5));
    for not_object in ["\"x\"", "null", "1", "true", "[47,0,34,0]"] {
        let c = with(&format!("\"safeArea\":{not_object}")).expect(not_object);
        assert_eq!(c.safe_area, WidgetSafeArea::default(), "{not_object}");
    }
    // A repeated member: the first.
    let placed_twice = with(concat!(
        "\"context\":{\"surface\":\"profile\",\"surface\":\"message\"},",
        "\"safeArea\":{\"top\":1,\"top\":2},\"safeArea\":{\"top\":3}"
    ))
    .expect("repeated members decode");
    assert_eq!(placed_twice.context.surface, WidgetSurface::Profile);
    assert_eq!(placed_twice.safe_area, device(1.0, 0.0, 0.0, 0.0));

    assert_eq!(decodable_prefixes(&wire, decode_config), 0);
    assert!(bounded_at_the_message_limit(&wire, decode_config));
    let mut accepted = damage_round_trips(&wire, 5000, 1, decode_config, encode_config);
    let doubled = [&wire[..wire.len() - 1], b",", &wire[1..]].concat();
    assert_eq!(decode_config(&doubled), Some(full));
    accepted += damage_round_trips(&doubled, 2000, 2, decode_config, encode_config);
    assert!(accepted > 100, "{accepted} damaged configs accepted");
}

#[test]
fn remote_data_and_safe_area_coding_round_trip_and_survive_damage() {
    let content = WidgetContentSafeArea { top: 1.5, right: 2.0, bottom: 3.0, left: 4.0, corner_left: 5.0, corner_right: 6.0 };
    let wire = encode_safe_area(&content);
    let (nan, inf) = (f64::NAN, f64::INFINITY);

    // The device's edges, as the content's: four sides, a non-object refused, a side that is no finite number 0, the
    // content's corners no part of it.
    let area = device(47.0, 0.5, 34.0, 2.0);
    let device_wire = encode_device_safe_area(&area);
    assert_eq!(String::from_utf8_lossy(&device_wire), "{\"top\":47,\"right\":0.5,\"bottom\":34,\"left\":2}");
    assert_eq!(decode_device_safe_area(&device_wire), Some(area));
    assert_eq!(decodable_prefixes(&device_wire, decode_device_safe_area), 0);
    assert!(bounded_at_the_message_limit(&device_wire, decode_device_safe_area));
    let accepted = damage_round_trips(&device_wire, 2000, 306, decode_device_safe_area, encode_device_safe_area);
    assert!(accepted > 0, "{accepted} damaged device safe areas accepted");
    assert_eq!(decode_device_safe_area(&encode_device_safe_area(&device(nan, -inf, -3.0, 1e300))), Some(device(0.0, 0.0, -3.0, 1e300)));
    assert_eq!(encode_device_safe_area(&device(nan, inf, -inf, 0.0)), b"{\"top\":0,\"right\":0,\"bottom\":0,\"left\":0}");
    assert_eq!(decode_device_safe_area(b"{\"top\":\"1\",\"right\":null,\"bottom\":true,\"left\":1e999}"), Some(WidgetSafeArea::default()));
    assert_eq!(decode_device_safe_area(b"{}"), Some(WidgetSafeArea::default()));
    assert_eq!(decode_device_safe_area(b"{\"top\":1,\"top\":2,\"left\":-1e999}"), Some(device(1.0, 0.0, 0.0, 0.0)));
    assert_eq!(decode_device_safe_area(&wire), Some(device(1.5, 2.0, 3.0, 4.0)));
    for bad in ["[]", "null", "1", "\"top\"", "", "{\"top\":1", "[{\"top\":1}]"] {
        assert_eq!(decode_device_safe_area(bad.as_bytes()), None, "{bad}");
    }
}

/// test_hosted.cpp's `everyToken`: a theme with every token set.
fn every_token() -> WidgetTheme {
    WidgetTheme {
        color_scheme: text("dark"),
        params: map(&[(b"bg_color", b"#112233"), (b"text_color", b"#ffffff"), (b"not a key", b"kept as given")]),
        button: WidgetButtonTokens {
            bg: Some(text("#0088cc")),
            text: Some(text("#ffffff")),
            radius: Some(text("8px")),
            font: Some(text("600 15px system-ui")),
            padding: Some(text("6px 12px")),
            height: Some(text("36px")),
            pressed_opacity: Some(0.7),
            disabled_opacity: Some(0.4),
        },
        text: WidgetTextTokens { font_size: Some(16.5), line_height: Some(1.4), padding: Some(text("10px 14px")) },
    }
}

#[test]
fn hosted_coding_round_trips_and_refuses_damage() {
    let start = SandboxHostedStart {
        theme: every_token(),
        stopped_label: text("Stopped by the host"),
        validated_ops: true,
        viewport_height: 844.12,
    };
    let start_wire = encode_hosted_start(&start);
    assert_eq!(decode_hosted_start(&start_wire), Some(start));
    let defaults = encode_hosted_start(&SandboxHostedStart::default());
    assert_eq!(
        String::from_utf8_lossy(&defaults),
        concat!(
            "{\"theme\":{\"colorScheme\":\"light\",\"params\":{},\"button\":{},\"text\":{}},",
            "\"stoppedLabel\":\"Widget stopped\",\"validatedOps\":false,\"viewportHeight\":0}"
        )
    );
    assert_eq!(decode_hosted_start(&defaults), Some(SandboxHostedStart::default()));
    // The viewport as given when finite (the served host checks it against the place), 0 when not: written as null,
    // so read back as 0.
    for finite in [0.5, 4000.0, 4000.01, -5.0, 1e300] {
        let sized = SandboxHostedStart { viewport_height: finite, ..SandboxHostedStart::default() };
        assert_eq!(decode_hosted_start(&encode_hosted_start(&sized)).map(|s| s.viewport_height), Some(finite));
    }
    for non_finite in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        let sized = SandboxHostedStart { viewport_height: non_finite, ..SandboxHostedStart::default() };
        let sized_wire = encode_hosted_start(&sized);
        assert!(sized_wire.ends_with(b",\"viewportHeight\":null}"), "{}", String::from_utf8_lossy(&sized_wire));
        assert_eq!(decode_hosted_start(&sized_wire).map(|s| s.viewport_height), Some(0.0));
    }
    // A label, flag or viewport of another type keeps the default, a flag is true only when it is true; a form from
    // before the viewport has none.
    for odder in [
        "{\"theme\":{\"colorScheme\":\"light\"},\"stoppedLabel\":5,\"validatedOps\":\"true\"}",
        "{\"theme\":{\"colorScheme\":\"light\"},\"validatedOps\":1}",
        "{\"theme\":{\"colorScheme\":\"light\"},\"stoppedLabel\":null,\"validatedOps\":null}",
        concat!(
            "{\"theme\":{\"colorScheme\":\"light\",\"params\":{},\"button\":{},\"text\":{}},",
            "\"stoppedLabel\":\"Widget stopped\",\"validatedOps\":false}"
        ),
        "{\"theme\":{\"colorScheme\":\"light\"},\"viewportHeight\":\"844\"}",
        "{\"theme\":{\"colorScheme\":\"light\"},\"viewportHeight\":true}",
        "{\"theme\":{\"colorScheme\":\"light\"},\"viewportHeight\":null}",
        "{\"theme\":{\"colorScheme\":\"light\"},\"viewportHeight\":[844]}",
        "{\"theme\":{\"colorScheme\":\"light\"},\"viewportHeight\":{\"height\":844}}",
        "{\"theme\":{\"colorScheme\":\"light\"},\"viewportHeight\":1e999}",
        "{\"theme\":{\"colorScheme\":\"light\"},\"viewportHeight\":-1e999}",
    ] {
        let started = decode_hosted_start(odder.as_bytes()).expect(odder);
        assert!(started.stopped_label == b"Widget stopped" && !started.validated_ops && started.viewport_height == 0.0, "{odder}");
    }
    let twice = decode_hosted_start(b"{\"theme\":{\"colorScheme\":\"light\"},\"viewportHeight\":700.125,\"viewportHeight\":1}");
    assert_eq!(twice.map(|s| s.viewport_height), Some(700.125));
    assert_eq!(decodable_prefixes(&start_wire, decode_hosted_start), 0);
}
