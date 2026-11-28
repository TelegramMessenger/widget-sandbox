//! The Rust SDK policy against widget-sandbox's sdk_policy.cpp (`oracle/sdk_policy_oracle`): generated and
//! mutated strings through every string check, JSON values (bridge arguments, transactions, payloads, commits,
//! themes, the fixture's inputs and their changes) through every JSON check, bridge calls, viewers, the bot's
//! data and answers, scripts of request-gate and haptic-gate steps, maps, and the `to_json` of values built by
//! hand. Skipped without `TWIDGET_ORACLE_DIR`; `tools/gate.py` sets it.

mod common;

use std::collections::BTreeMap;

use common::*;
use twidget_json::{Json, Surrogates, quote};
use twidget_sdk_policy::*;
use twidget_testkit::{Comparison, Fields, Lcg, Oracle, mutate};

fn parse(text: &[u8]) -> Option<Json> {
    Json::parse_with(text, Surrogates::Keep, 128)
}

fn pick<T: Copy>(random: &mut Lcg, items: &[T]) -> T {
    items[random.below(items.len())]
}

fn opt(f: &mut Fields, v: Option<Vec<u8>>) {
    f.opt_str(v.as_deref());
}

// ── what the oracle answers, computed by the port ──

fn string_facts(s: &[u8]) -> (u8, Vec<u8>) {
    let mut f = Fields::new();
    f.u64(utf8_length(s) as u64).u64(utf16_length(s) as u64).flag(well_formed(s));
    opt(&mut f, check_request_data(s));
    opt(&mut f, check_copy_text(s));
    opt(&mut f, check_widget_data_value(s));
    opt(&mut f, check_chat_type(s));
    opt(&mut f, check_locale(s));
    opt(&mut f, check_resize_anchor(s));
    opt(&mut f, check_link(b"openLink", s));
    opt(&mut f, check_link(b"openTelegramLink", s));
    f.flag(is_privileged_method(s)).flag(allowed_in_preview(s)).str(viewer_field_rule(s).as_bytes());
    (1, f.take())
}

fn json_facts(text: &[u8]) -> (u8, Vec<u8>) {
    let Some(v) = parse(text) else { return (3, Vec::new()) };
    let mut f = Fields::new();
    opt(&mut f, check_link_value(b"openLink", &v));
    opt(&mut f, check_link_value(b"openTelegramLink", &v));
    let h = check_haptic(&v);
    f.flag(h.is_some()).str(h.as_ref().map_or(&b""[..], |h| &h.haptic));
    opt(&mut f, h.and_then(|h| h.value));
    opt(&mut f, check_resize_anchor_value(&v));
    let options = check_request_options(&v);
    f.flag(options.is_some()).flag(options.unwrap_or(false));
    opt(&mut f, check_wallet_address(&v));
    opt(&mut f, check_wallet_destination(&v));
    opt(&mut f, check_wallet_transaction(&v).map(|tx| tx.to_json().dump()));
    opt(&mut f, check_sign_data_payload(&v).map(|p| p.to_json().dump()));
    opt(&mut f, check_request_data_value(&v));
    opt(&mut f, check_copy_text_value(&v));
    f.str(&theme_params_json(&check_theme_params(&v)).dump());
    opt(&mut f, check_locale_value(&v));
    let area = check_storage_area(&v);
    f.flag(area.is_some());
    if let Some(a) = area {
        f.u64(storage_bytes(&a) as u64).flag(storage_area_fits(&a));
        f.str(&StorageCommit { local: Some(a), session: None }.to_json().dump());
    }
    opt(&mut f, check_storage_commit(&v).map(|c| c.to_json().dump()));
    (1, f.take())
}

fn bridge_facts(method: &[u8], args: &[u8]) -> (u8, Vec<u8>) {
    let (Some(method), Some(args)) = (parse(method), parse(args)) else { return (3, Vec::new()) };
    let Some(call) = check_bridge_call(&method, &args) else { return (0, Vec::new()) };
    let mask = u8::from(call.action.is_some())
        | u8::from(call.resize_anchor.is_some()) << 1
        | u8::from(call.request.is_some()) << 2
        | u8::from(call.wallet.is_some()) << 3
        | u8::from(call.copy_text.is_some()) << 4
        | u8::from(call.watch_public_data.is_some()) << 5;
    (1, Fields::new().u8(mask).str(&call.to_json().dump()).take())
}

fn json_case(c: &mut Comparison, text: &[u8], what: impl Into<String>) {
    c.case(&Fields::new().str(text).framed(b'j'), json_facts(text), what);
}

fn bridge_case(c: &mut Comparison, method: &[u8], args: &[u8], what: impl Into<String>) {
    c.case(&Fields::new().str(method).str(args).framed(b'b'), bridge_facts(method, args), what);
}

fn string_case(c: &mut Comparison, s: &[u8], what: impl Into<String>) {
    c.case(&Fields::new().str(s).framed(b's'), string_facts(s), what);
}

fn link_case(c: &mut Comparison, method: &[u8], url: &[u8], what: impl Into<String>) {
    let rust = match check_link(method, url) {
        Some(u) => (1, u),
        None => (0, Vec::new()),
    };
    c.case(&Fields::new().str(method).str(url).framed(b'L'), rust, what);
}

// ── generated inputs ──

const PIECES: &[&[u8]] = &[
    b"a",
    b"Z",
    b"0",
    b"_",
    b"-",
    b".",
    b" ",
    b"\0",
    b"\x01",
    b"\x7f",
    b"\xC3\xA9",
    b"\xD0\xB6",
    b"\xE2\x82\xAC",
    b"\xF0\x9F\x98\x80",
    b"\xED\xA0\x80",
    b"\xED\xB0\x80",
    b"\xED\x9F\xBF",
    b"\xEE\x80\x80",
    b"\xEF\xBF\xBD",
    b"\xC0\xAF",
    b"\xC1\xBF",
    b"\xC2\x80",
    b"\xDF\xBF",
    b"\xE0\x80\xAF",
    b"\xE0\xA0\x80",
    b"\xF0\x80\x80\xAF",
    b"\xF0\x90\x80\x80",
    b"\xF4\x8F\xBF\xBF",
    b"\xF4\x90\x80\x80",
    b"\xF5\x80\x80\x80",
    b"\xF8\x88\x80\x80\x80",
    b"\xC3",
    b"\xE2\x82",
    b"\xF0\x9F\x98",
    b"\x80",
    b"\xBF",
    b"\xFF",
    b"\xFE",
];

/// Bytes from the pieces of UTF-8, broken UTF-8 and WTF-8, around a length that matters.
fn text_bytes(random: &mut Lcg) -> Vec<u8> {
    let mut out = Vec::new();
    match random.below(4) {
        0 => {
            for _ in 0..random.below(8) {
                out.extend_from_slice(pick(random, PIECES));
            }
        }
        1 => {
            let unit = pick(random, &[&b"a"[..], b"r", b"\xD0\xB6", b"\xF0\x9F\x98\x80", b"\xE2\x82\xAC"]);
            let n = pick(random, &[0usize, 1, 31, 32, 63, 64, 65, 127, 128, 129, 255, 256, 257, 341, 342, 512, 1023, 1024, 1025]);
            out = unit.repeat(n / unit.len().max(1) + usize::from(random.chance(2)));
            if random.chance(3) {
                let at = random.below(out.len() + 1);
                out.splice(at..at, pick(random, PIECES).iter().copied());
            }
        }
        2 => out = pick(random, STRINGS).to_vec(),
        _ => {
            for _ in 0..random.below(12) {
                out.push(random.next() as u8);
            }
        }
    }
    out
}

const STRINGS: &[&[u8]] = &[
    b"",
    b"a",
    b"x",
    b"ok",
    b"top",
    b"bottom",
    b"Top",
    b"top ",
    b"impact",
    b"notification",
    b"selection",
    b"light",
    b"medium",
    b"heavy",
    b"rigid",
    b"soft",
    b"error",
    b"success",
    b"warning",
    b"Light",
    b"ru",
    b"en-US",
    b"zh-Hant-TW",
    b"e",
    b"en-",
    b"-en",
    b"abcdefghi",
    b"en-123456789",
    b"en-12345678",
    b"en--US",
    b"en_US",
    b"abcdefgh-a-b-c-d-e-f-g-h-i-j-k-l-m-no",
    b"#abcdef",
    b"#ABCDEF",
    b"#abcde",
    b"#abcdeg",
    b"#AbCdEf",
    b"red",
    b"#abcdef ",
    b"private",
    b"group",
    b"supergroup",
    b"channel",
    b"Private",
    b"-239",
    b"-3",
    b"239",
    b"-239 ",
    b"0",
    b"00",
    b"1",
    b"01",
    b"10",
    b"1234567890123456789012345678901234567",
    b"12345678901234567890123456789012345678",
    b"-1",
    b"1.5",
    b"openLink",
    b"openTelegramLink",
    b"copyText",
    b"request",
    b"ready",
    b"setResizeAnchor",
    b"first_name",
    b"last_name",
    b"username",
    b"is_premium",
    b"chatData",
    b"abcde",
    b"abcd",
    b"a-bcd",
    b"ABC_123",
    b"\xD0\xB0nnax",
    b"abcdefghijklmnopqrstuvwxyz012345",
    b"abcdefghijklmnopqrstuvwxyz0123456",
    b"text",
    b"binary",
    b"cell",
    b"local",
    b"session",
    b"wallet",
    b"EQD4FPq-PRDieyQKkizFTRtSDyucUIqrj0v_zXJmqaDp6_0t",
    b"UQD4FPq+PRDieyQKkizFTRtSDyucUIqrj0v/zXJmqaDp6/0t",
    b"EQD4FPq-PRDieyQKkizFTRtSDyucUIqrj0v_zXJmqaDp6_0",
    b"EQD4FPq-PRDieyQKkizFTRtSDyucUIqrj0v_zXJmqaDp6_0tt",
    b"EQD4FPq-PRDieyQKkizFTRtSDyucUIqrj0v_zXJmqaDp6_0=",
    b"EQD4FPq.PRDieyQKkizFTRtSDyucUIqrj0v_zXJmqaDp6_0t",
    b"0:83dfd552e63729b472fcbcc8c45ebcc6691702558b68ec7527e1ba403a0f31a8",
    b"-1:83dfd552e63729b472fcbcc8c45ebcc6691702558b68ec7527e1ba403a0f31a8",
    b"1:83dfd552e63729b472fcbcc8c45ebcc6691702558b68ec7527e1ba403a0f31a8",
    b"0:83DFD552E63729B472FCBCC8C45EBCC6691702558B68EC7527E1BA403A0F31A8",
    b"0:83dfd552e63729b472fcbcc8c45ebcc6691702558b68ec7527e1ba403a0f31a",
    b"0:83dfd552e63729b472fcbcc8c45ebcc6691702558b68ec7527e1ba403a0f31ag",
    b"te6cckEBAQEAAgAAAEysuc0=",
    b"te6cckEBAQEAAgAAAEysuc0",
    b"te6cc-_EBAQ",
    b"a=",
    b"a==",
    b"a===",
    b"=",
    b"==",
    b"ab=c",
    b"te6+/",
    b"te6 cc",
    b"\xF0\x9F\x98\x80",
    b"\xED\xA0\x80",
    b"a\0b",
    b"\xFF",
    b"PROMO-2026 \xE2\x9C\x93",
];

thread_local! {
    /// Whether the generators make only values the checks take, for a whole transaction or payload now and then.
    static STRICT: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

fn strict() -> bool {
    STRICT.with(|s| s.get())
}

/// `make`, every other time strictly.
fn strictly(random: &mut Lcg, make: impl FnOnce(&mut Lcg) -> Vec<u8>) -> Vec<u8> {
    let was = strict();
    let now = was || random.chance(2);
    STRICT.with(|s| s.set(now));
    let made = make(random);
    STRICT.with(|s| s.set(was));
    made
}

/// One of `good`, or now and then one of `bad`.
fn good_or<'a>(random: &mut Lcg, good: &[&'a [u8]], bad: &[&'a [u8]]) -> &'a [u8] {
    if !strict() && random.chance(6) { pick(random, bad) } else { pick(random, good) }
}

const GOOD_HOSTS: &[&[u8]] = &[
    b"t.me",
    b"telegram.me",
    b"telegram.dog",
    b"T.ME",
    b"x.t.me",
    b"a.b.telegram.me",
    b"Telegram.Dog",
    b"example.com",
    b"EXAMPLE.com",
    b"xn--80ak6aa92e.com",
    b"sub.example.org",
    b"1.2.3.4",
    b"localhost",
    b"a-b.c",
    b"xt.me",
    b"t.me.example.com",
    b"0",
];
const BAD_HOSTS: &[&[u8]] = &[
    b"telegram.dog.",
    b".t.me",
    b"t..me",
    b"\xE2\x84\xAAelvin.com",
    b"",
    b"[::1]",
    b"-ex.com",
    b"ex-.com",
    b"ex_ample.com",
    b"a b.com",
    b"\xC3\xA9.com",
    b"ex%41.com",
    b"u@t.me",
];
const GOOD_REST: &[&[u8]] = &[
    b"/",
    b"/path",
    b"?q=1&r=2",
    b"#frag",
    b"%41",
    b"%aF",
    b"~",
    b"[",
    b"]",
    b"@",
    b":",
    b"!$&'()*+,;=",
    b"\xC3\xA9",
    b"\xF0\x9F\x98\x80",
    b"\xED\x9F\xBF",
    b"\xEE\x80\x80",
    b"\xF4\x8F\xBF\xBF",
    b"\xC2\x80",
    b"\xE0\xA0\x80",
    b"\xF0\x90\x80\x80",
    b"/a/b",
    b"?",
    b"#",
    b"-._",
];
const BAD_REST: &[&[u8]] = &[
    b"%4",
    b"%zz",
    b"%",
    b"%4g",
    b"%%41",
    b" ",
    b"\"",
    b"<",
    b">",
    b"\\",
    b"^",
    b"`",
    b"{",
    b"|",
    b"}",
    b"\0",
    b"\x01",
    b"\x7f",
    b"\xED\xA0\x80",
    b"\xED\xBF\xBF",
    b"\xC0\xAF",
    b"\xE0\x80\xAF",
    b"\xF0\x80\x80\xAF",
    b"\xF4\x90\x80\x80",
    b"\xF8\x88\x80\x80\x80",
    b"\xC3",
    b"\xC3x",
    b"\x80",
    b"\xFF",
];

/// A URL from the pieces the link check looks at: mostly one it takes, with one part wrong now and then.
fn url(random: &mut Lcg) -> Vec<u8> {
    let mut out = good_or(
        random,
        &[&b"https://"[..], b"https://", b"HTTPS://", b"hTtPs://"],
        &[b"http://", b"https:/", b"https:", b"", b"https://\xC3\xA9", b"ftp://"],
    )
    .to_vec();
    if random.chance(25) {
        out.extend_from_slice(pick(random, &[&b"user@"[..], b"u:p@", b"@"]));
    }
    if random.chance(4) {
        for i in 0..1 + random.below(4) {
            if i > 0 {
                out.push(b'.');
            }
            let label = good_or(
                random,
                &[&b"example"[..], b"t", b"me", b"telegram", b"dog", b"a", b"xn--p1ai", b"EX", b"ex-ample", b"1"],
                &[b"-ex", b"ex-", b"ex_ample", b"\xC3\xA9", b"a b", b""],
            );
            out.extend_from_slice(label);
            if random.chance(15) {
                out.extend_from_slice(&b"a".repeat(pick(random, &[60usize, 62, 63, 64])));
            }
        }
        if random.chance(10) {
            let long = [b"a".repeat(63), b".".to_vec()].concat().repeat(4);
            out.extend_from_slice(&long[..pick(random, &[252usize, 253, 254, 255])]);
        }
    } else {
        out.extend_from_slice(good_or(random, GOOD_HOSTS, BAD_HOSTS));
    }
    if random.chance(4) {
        out.push(b':');
        out.extend_from_slice(good_or(
            random,
            &[&b"0"[..], b"80", b"443", b"65535", b"00080", b"8080"],
            &[b"", b"65536", b"99999", b"123456", b"8a", b"-1", b" 80"],
        ));
    }
    for _ in 0..random.below(7) {
        out.extend_from_slice(good_or(random, GOOD_REST, BAD_REST));
    }
    if random.chance(4) {
        if !matches!(out.last(), Some(b'/' | b'?' | b'#')) {
            out.push(b'/');
        }
        let unit = pick(random, &[&b"a"[..], b"\xC3\xA9", b"\xF0\x9F\x98\x80", b"%41"]);
        let target = pick(random, &[84usize, 85, 86, 87, 250, 254, 255, 256, 257, 260]);
        while out.len() + unit.len() <= target {
            out.extend_from_slice(unit);
        }
    }
    out
}

const METHODS: &[&[u8]] =
    &[b"openLink", b"openTelegramLink", b"", b"openlink", b"openTelegramLink\0", b"copyText", b"x", b"openTelegramLink ", b"\xffopenLink"];

const NUMBERS: &[&[u8]] = &[
    b"0",
    b"-0",
    b"1",
    b"1.5",
    b"-1",
    b"2",
    b"7",
    b"1e0",
    b"2147483647",
    b"2147483647.0",
    b"2147483648",
    b"4294967295",
    b"4294967296",
    b"1e10",
    b"1e999",
    b"-1e999",
    b"9007199254740993",
    b"1700000000",
    b"0.1",
    b"5e-324",
    b"1E+2",
];

fn number(random: &mut Lcg) -> Vec<u8> {
    pick(random, NUMBERS).to_vec()
}

/// A JSON string: a quoted pool string or generated bytes, or an escape the engine's parse keeps as WTF-8.
fn string(random: &mut Lcg) -> Vec<u8> {
    match random.below(10) {
        0 => pick(
            random,
            &[&b"\"\\ud800\""[..], b"\"a\\udc00\"", b"\"\\u0000\"", b"\"\\ud83d\\ude00\"", b"\"\\uD800\\u0041\"", b"\"\\u00e9\""],
        )
        .to_vec(),
        1..=3 => quote(text_bytes(random)),
        _ => quote(pick(random, STRINGS)),
    }
}

fn scalar(random: &mut Lcg) -> Vec<u8> {
    match random.below(8) {
        0 => pick(random, &[&b"null"[..], b"true", b"false"]).to_vec(),
        1 | 2 => number(random),
        3 => b"[]".to_vec(),
        4 => b"{}".to_vec(),
        _ => string(random),
    }
}

fn list(items: Vec<Vec<u8>>) -> Vec<u8> {
    [b"[".to_vec(), items.join(&b","[..]), b"]".to_vec()].concat()
}

fn object(members: Vec<(Vec<u8>, Vec<u8>)>) -> Vec<u8> {
    let items: Vec<Vec<u8>> = members.into_iter().map(|(k, v)| [quote(k), b":".to_vec(), v].concat()).collect();
    [b"{".to_vec(), items.join(&b","[..]), b"}".to_vec()].concat()
}

type Maker = fn(&mut Lcg) -> Vec<u8>;

/// An object of `fields` (a name, whether it is required, its value maker): now and then a field left out, of
/// another type or null, a member twice, or an unknown one.
fn shaped(random: &mut Lcg, fields: &[(&str, bool, Maker)]) -> Vec<u8> {
    let mut members = Vec::new();
    let strict = strict();
    for (k, required, make) in fields {
        if (!strict || !required) && random.chance(if *required { 30 } else { 3 }) {
            continue;
        }
        let value = if !strict && random.chance(30) {
            scalar(random)
        } else if !required && random.chance(10) {
            b"null".to_vec()
        } else {
            make(random)
        };
        members.push((k.as_bytes().to_vec(), value));
    }
    if !strict && random.chance(20) {
        members.push((pick(random, &[&b"extra"[..], b"type", b"wallet", b"", b"messages", b"text"]).to_vec(), scalar(random)));
    }
    if !strict && !members.is_empty() && random.chance(15) {
        let twice = members[random.below(members.len())].clone();
        members.insert(random.below(members.len() + 1), (twice.0, if random.chance(2) { twice.1 } else { scalar(random) }));
    }
    object(members)
}

const FRIENDLY: &[&[u8]] = &[
    b"EQD4FPq-PRDieyQKkizFTRtSDyucUIqrj0v_zXJmqaDp6_0t",
    b"UQD4FPq+PRDieyQKkizFTRtSDyucUIqrj0v/zXJmqaDp6/0t",
    b"kQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
];
const RAW: &[&[u8]] = &[
    b"0:83dfd552e63729b472fcbcc8c45ebcc6691702558b68ec7527e1ba403a0f31a8",
    b"-1:83dfd552e63729b472fcbcc8c45ebcc6691702558b68ec7527e1ba403a0f31a8",
    b"0:83DFD552E63729B472FCBCC8C45EBCC6691702558B68EC7527E1BA403A0F31A8",
];
const BAD_ADDRESSES: &[&[u8]] = &[
    b"EQD4FPq-PRDieyQKkizFTRtSDyucUIqrj0v_zXJmqaDp6_0",
    b"EQD4FPq-PRDieyQKkizFTRtSDyucUIqrj0v_zXJmqaDp6_0tt",
    b"EQD4FPq-PRDieyQKkizFTRtSDyucUIqrj0v_zXJmqaDp6_0=",
    b"EQD4FPq.PRDieyQKkizFTRtSDyucUIqrj0v_zXJmqaDp6_0t",
    b"1:83dfd552e63729b472fcbcc8c45ebcc6691702558b68ec7527e1ba403a0f31a8",
    b"0:83dfd552e63729b472fcbcc8c45ebcc6691702558b68ec7527e1ba403a0f31a",
    b"0:83dfd552e63729b472fcbcc8c45ebcc6691702558b68ec7527e1ba403a0f31ag",
    b"-1:83dfd552e63729b472fcbcc8c45ebcc6691702558b68ec7527e1ba403a0f31a8a",
    b"",
    b"0:",
];

/// A message's destination: user-friendly, or now and then raw or broken.
fn destination(random: &mut Lcg) -> Vec<u8> {
    if random.chance(8) { quote(pick(random, RAW)) } else { quote(good_or(random, FRIENDLY, BAD_ADDRESSES)) }
}

/// A `from`: user-friendly or raw, now and then broken.
fn from_address(random: &mut Lcg) -> Vec<u8> {
    if random.chance(2) { quote(good_or(random, RAW, BAD_ADDRESSES)) } else { quote(good_or(random, FRIENDLY, BAD_ADDRESSES)) }
}

/// The address a value of any shape is checked as.
fn address(random: &mut Lcg) -> Vec<u8> {
    if random.chance(2) { destination(random) } else { from_address(random) }
}

const GOOD_BASE64: &[&[u8]] = &[b"te6cckEBAQEAAgAAAEysuc0=", b"te6cc-_EBAQ", b"a=", b"a==", b"QUJD", b"_-+/", b"A"];
const BAD_BASE64: &[&[u8]] = &[b"a===", b"=", b"==", b"", b"ab=c", b"te6 cc", b"\xC3\xA9", b"a.b", b"===a"];

/// Base64 of one of `sizes` characters (strictly, of at most `max`), or a short one from the pools.
fn base64_sized(random: &mut Lcg, sizes: &[usize], max: usize) -> Vec<u8> {
    if random.chance(5) {
        let n = pick(random, sizes);
        let n = if strict() { n.min(max) } else { n };
        let mut s = b"A".repeat(n.saturating_sub(2));
        s.extend_from_slice(pick(random, &[&b"AA"[..], b"A=", b"==", b"AA"]));
        return quote(&s[..n.min(s.len())]);
    }
    quote(good_or(random, GOOD_BASE64, BAD_BASE64))
}

fn boc(random: &mut Lcg) -> Vec<u8> {
    base64_sized(random, &[1, 4096, 8000, 8191, 8192, 8193], 4096)
}

fn base64_text(random: &mut Lcg) -> Vec<u8> {
    base64_sized(random, &[1, 4095, 4096, 4097, 8192, 8193], 8192)
}

fn amount(random: &mut Lcg) -> Vec<u8> {
    if !strict() && random.chance(15) {
        return number(random);
    }
    quote(good_or(
        random,
        &[&b"0"[..], b"1", b"5", b"1000000000", b"1234567890123456789012345678901234567"],
        &[b"00", b"01", b"12345678901234567890123456789012345678", b"-1", b"1.5", b"", b"1e9", b" 1"],
    ))
}

fn network(random: &mut Lcg) -> Vec<u8> {
    good_or(random, &[&b"\"-239\""[..], b"\"-3\"", b"null"], &[b"-239", b"\"239\"", b"\"-239 \"", b"\"\"", b"\"-3\\u0000\""]).to_vec()
}

fn valid_until(random: &mut Lcg) -> Vec<u8> {
    good_or(
        random,
        &[&b"1"[..], b"1700000000", b"4294967295", b"2", b"1e3", b"4294967295.0"],
        &[b"0", b"1.5", b"4294967296", b"\"1\"", b"-1", b"1e999", b"true"],
    )
    .to_vec()
}

fn message(random: &mut Lcg) -> Vec<u8> {
    shaped(random, &[("address", true, destination), ("amount", true, amount), ("payload", false, boc), ("stateInit", false, boc)])
}

fn messages(random: &mut Lcg) -> Vec<u8> {
    let n = if strict() {
        if random.chance(20) { 255 } else { 1 + random.below(3) }
    } else if random.chance(12) {
        pick(random, &[0usize, 9, 254, 255, 256])
    } else {
        1 + random.below(4)
    };
    if n > 8 {
        let one = message(random);
        return list(vec![one; n]);
    }
    list((0..n).map(|_| message(random)).collect())
}

fn transaction(random: &mut Lcg) -> Vec<u8> {
    shaped(
        random,
        &[("valid_until", false, valid_until), ("network", false, network), ("from", false, from_address), ("messages", true, messages)],
    )
}

/// A text of `sizes` bytes (strictly, of at most `max`) or from the pools: mostly well-formed and without NUL.
fn sized_text(random: &mut Lcg, sizes: &[usize], max: usize) -> Vec<u8> {
    match random.below(5) {
        0 => quote(b"s".repeat(if strict() { pick(random, sizes).min(max) } else { pick(random, sizes) })),
        1 if !strict() => string(random),
        _ => quote(good_or(
            random,
            &[&b"x"[..], b"Sign this", b"\xD0\xB6\xF0\x9F\x98\x80", b"a b", b"\x01"],
            &[b"", b"a\0b", b"\xFF", b"\xED\xA0\x80"],
        )),
    }
}

fn sign_data(random: &mut Lcg) -> Vec<u8> {
    base64_sized(random, &[1, 4095, 4096, 4097], 4096)
}

fn sign_text(random: &mut Lcg) -> Vec<u8> {
    sized_text(random, &[1, 4095, 4096, 4097], 4096)
}

fn schema(random: &mut Lcg) -> Vec<u8> {
    sized_text(random, &[1, 1023, 1024, 1025], 1024)
}

/// A `signData` payload: mostly its kind's fields, now and then one more, one less, of another type or moved.
fn payload(random: &mut Lcg) -> Vec<u8> {
    let kind = good_or(random, &[&b"text"[..], b"binary", b"cell"], &[b"Text", b"", b"bytes", b"text "]);
    let strict = strict();
    let mut members = vec![(b"type".to_vec(), if !strict && random.chance(25) { scalar(random) } else { quote(kind) })];
    let fields: [(&str, Maker, bool); 6] = [
        ("text", sign_text, kind == b"text"),
        ("bytes", sign_data, kind == b"binary"),
        ("schema", schema, kind == b"cell"),
        ("cell", sign_data, kind == b"cell"),
        ("network", network, random.chance(3)),
        ("from", from_address, random.chance(3)),
    ];
    for (name, make, wanted) in fields {
        if wanted != (!strict && random.chance(if wanted { 25 } else { 12 })) {
            members.push((name.as_bytes().to_vec(), if !strict && random.chance(25) { scalar(random) } else { make(random) }));
        }
    }
    if !strict && random.chance(20) {
        members.push((b"extra".to_vec(), scalar(random)));
    }
    if random.chance(4) {
        let at = random.below(members.len());
        let moved = members.remove(at);
        members.push(moved);
    }
    object(members)
}

fn color(random: &mut Lcg) -> Vec<u8> {
    quote(pick(random, &[&b"#abcdef"[..], b"#ABCDEF", b"#AbCdEf", b"#abcde", b"#abcdeg", b"red", b"#abcdef ", b"", b"#0000001"]))
}

fn theme(random: &mut Lcg) -> Vec<u8> {
    let mut members = Vec::new();
    for _ in 0..random.below(8) {
        let key = if random.chance(6) { b"unknown".to_vec() } else { pick(random, &THEME_KEYS).as_bytes().to_vec() };
        members.push((key, if random.chance(8) { scalar(random) } else { color(random) }));
    }
    object(members)
}

fn storage_area(random: &mut Lcg) -> Vec<u8> {
    let mut members = Vec::new();
    let big = random.chance(4);
    for i in 0..random.below(if big { 3 } else { 6 }) {
        let key = if random.chance(5) { b"k".to_vec() } else { format!("key{i}").into_bytes() };
        let value = if random.chance(10) {
            scalar(random)
        } else if big {
            quote(b"v".repeat(pick(random, &[2040usize, 2043, 2044, 2045, 4090, 4091, 4092, 4093])))
        } else {
            string(random)
        };
        members.push((key, value));
    }
    object(members)
}

fn commit(random: &mut Lcg) -> Vec<u8> {
    let mut members = Vec::new();
    for _ in 0..random.below(4) {
        let key = pick(random, &[&b"local"[..], b"local", b"session", b"session", b"other", b""]).to_vec();
        members.push((key, if random.chance(10) { scalar(random) } else { storage_area(random) }));
    }
    object(members)
}

fn options(random: &mut Lcg) -> Vec<u8> {
    pick(
        random,
        &[
            &b"{}"[..],
            b"{\"wallet\":true}",
            b"{\"wallet\":false}",
            b"{\"wallet\":null}",
            b"{\"wallet\":1}",
            b"{\"x\":1}",
            b"{\"wallet\":true,\"x\":1}",
            b"{\"wallet\":false,\"wallet\":true}",
            b"[]",
            b"null",
            b"true",
        ],
    )
    .to_vec()
}

fn id(random: &mut Lcg) -> Vec<u8> {
    match random.below(6) {
        0 => scalar(random),
        1 => number(random),
        _ => pick(random, &[&b"1"[..], b"7", b"2147483647", b"1e0", b"2147483647.0", b"12"]).to_vec(),
    }
}

/// A request's data: mostly 1–64 bytes of well-formed text.
fn data(random: &mut Lcg) -> Vec<u8> {
    match random.below(4) {
        0 => string(random),
        1 => quote(b"d".repeat(pick(random, &[1usize, 63, 64, 65]))),
        _ => quote(pick(random, &[&b"tap"[..], b"buy:1", b"\xD0\xB6\xF0\x9F\x98\x80", b"x"])),
    }
}

/// A plausible argument list for `method`, now and then with an argument too many, too few or of another type.
fn args(random: &mut Lcg, method: &[u8]) -> Vec<u8> {
    let mut items: Vec<Vec<u8>> = match method {
        b"ready" => Vec::new(),
        b"openLink" | b"openTelegramLink" => vec![quote(url(random))],
        b"haptic" => {
            let kind = pick(random, &[&b"impact"[..], b"notification", b"selection", b"Impact", b""]);
            let mut a = vec![quote(kind)];
            if kind != b"selection" || random.chance(4) {
                a.push(string(random));
            }
            a
        }
        b"setResizeAnchor" => vec![if random.chance(3) { quote(pick(random, &[&b"top"[..], b"bottom"])) } else { string(random) }],
        b"request" => {
            let mut a = vec![id(random), data(random)];
            if random.chance(3) {
                a.push(options(random));
            }
            a
        }
        b"wallet" => {
            let which = pick(random, &[&b"sendTransaction"[..], b"signData", b"sendTransaction", b"signData", b"signMessage", b""]);
            let call = strictly(random, |r| if which == b"signData" { payload(r) } else { transaction(r) });
            vec![id(random), quote(which), call, data(random)]
        }
        b"watchPublicData" => vec![pick(random, &[&b"true"[..], b"false", b"\"true\"", b"1", b"null"]).to_vec()],
        b"copyText" => vec![string(random)],
        _ => (0..random.below(3)).map(|_| scalar(random)).collect(),
    };
    match random.below(12) {
        0 => {
            items.push(scalar(random));
        }
        1 => {
            items.pop();
        }
        2 if !items.is_empty() => {
            let at = random.below(items.len());
            items[at] = scalar(random);
        }
        _ => {}
    }
    list(items)
}

const BRIDGE_METHODS: &[&[u8]] = &[
    b"ready",
    b"openLink",
    b"openTelegramLink",
    b"haptic",
    b"setResizeAnchor",
    b"request",
    b"wallet",
    b"watchPublicData",
    b"copyText",
    b"copytext",
    b"",
    b"ready ",
    b"signData",
];

/// A JSON value of any of the shapes the checks take.
fn value(random: &mut Lcg) -> Vec<u8> {
    match random.below(14) {
        0 => strictly(random, transaction),
        1 => strictly(random, payload),
        2 => commit(random),
        3 => storage_area(random),
        4 => theme(random),
        5 => options(random),
        6 => quote(url(random)),
        7 => {
            let method = pick(random, BRIDGE_METHODS);
            args(random, method)
        }
        8 => address(random),
        9 => base64_text(random),
        10 => list((0..random.below(4)).map(|_| scalar(random)).collect()),
        _ => scalar(random),
    }
}

/// `v` with one of its parts changed: a string's bytes, a number, a type, a member dropped, repeated or added,
/// an item dropped or repeated.
fn changed(random: &mut Lcg, v: &Json, depth: usize) -> Json {
    let here = depth > 3 || random.chance(3);
    match v {
        Json::String(s) if here => {
            let mut s = s.clone();
            mutate(random, &mut s);
            Json::String(s)
        }
        Json::Array(items) if !items.is_empty() && !here => {
            let mut items = items.clone();
            let at = random.below(items.len());
            match random.below(4) {
                0 => {
                    items.remove(at);
                }
                1 => {
                    let again = items[at].clone();
                    items.insert(at, again);
                }
                _ => items[at] = changed(random, &items[at], depth + 1),
            }
            Json::Array(items)
        }
        Json::Object(members) if !members.is_empty() && !here => {
            let mut members = members.clone();
            let at = random.below(members.len());
            match random.below(6) {
                0 => {
                    members.remove(at);
                }
                1 => {
                    let again = members[at].clone();
                    members.push(again);
                }
                2 => members.push((pick(random, &[&b"extra"[..], b"wallet", b"type", b"local"]).to_vec(), Json::Null)),
                _ => members[at].1 = changed(random, &members[at].1, depth + 1),
            }
            Json::Object(members)
        }
        _ => parse(&scalar(random)).unwrap_or_default(),
    }
}

#[test]
fn strings_and_links_alike() {
    let Some(oracle) = Oracle::find("sdk_policy_oracle") else { return };
    let mut random = Lcg::new(0x5d_0001);
    let mut c = Comparison::new();
    let mut accepted = 0;
    for i in 0..40000 {
        let s = text_bytes(&mut random);
        string_case(&mut c, &s, format!("string {i}: {s:?}"));
        let u = url(&mut random);
        let mut changed = u.clone();
        let edits = if random.chance(4) { mutate(&mut random, &mut changed) } else { String::new() };
        let telegram = check_link(b"openTelegramLink", &u).is_some();
        let method = if random.chance(4) {
            pick(&mut random, METHODS)
        } else if telegram {
            &b"openTelegramLink"[..]
        } else {
            b"openLink"
        };
        accepted += usize::from(check_link(method, &u).is_some());
        link_case(&mut c, method, &u, format!("link {i}: {:?}", String::from_utf8_lossy(&u)));
        link_case(&mut c, method, &changed, format!("link {i} changed{edits}"));
        if i % 4 == 0 {
            string_case(&mut c, &u, format!("url {i} as a string"));
        }
    }
    let tally = c.check(&oracle);
    eprintln!("SDK strings and links: {} answers alike, {accepted} links taken", tally[0] + tally[1]);
    assert!(accepted > 3000);
}

#[test]
fn json_values_alike() {
    let Some(oracle) = Oracle::find("sdk_policy_oracle") else { return };
    let mut random = Lcg::new(0x5d_0002);
    let mut c = Comparison::new();
    let mut taken = [0usize; 4];
    for i in 0..30000 {
        let text = value(&mut random);
        if let Some(v) = parse(&text) {
            taken[0] += usize::from(check_wallet_transaction(&v).is_some());
            taken[1] += usize::from(check_sign_data_payload(&v).is_some());
            taken[2] += usize::from(check_storage_commit(&v).is_some_and(|c| c.local.is_some() || c.session.is_some()));
            taken[3] += usize::from(!check_theme_params(&v).is_empty());
            if random.chance(2) {
                let other = changed(&mut random, &v, 0).dump();
                json_case(&mut c, &other, format!("value {i} changed: {}", String::from_utf8_lossy(&other)));
            }
        }
        json_case(&mut c, &text, format!("value {i}: {}", String::from_utf8_lossy(&text)));
        if random.chance(5) {
            let mut broken = text.clone();
            let edits = mutate(&mut random, &mut broken);
            json_case(&mut c, &broken, format!("value {i} broken{edits}"));
        }
    }
    let tally = c.check(&oracle);
    eprintln!("SDK JSON values: {} answers alike ({} not JSON); taken: {taken:?}", tally[1] + tally[0], tally[2]);
    assert!(taken.iter().all(|n| *n > 300), "{taken:?}");
}

#[test]
fn bridge_calls_alike() {
    let Some(oracle) = Oracle::find("sdk_policy_oracle") else { return };
    let mut random = Lcg::new(0x5d_0003);
    let mut c = Comparison::new();
    let mut kinds = [0usize; 6];
    for i in 0..40000 {
        let method = pick(&mut random, BRIDGE_METHODS);
        let method_text = if random.chance(30) { scalar(&mut random) } else { quote(method) };
        let args_text = if random.chance(30) { scalar(&mut random) } else { args(&mut random, method) };
        let (status, bytes) = bridge_facts(&method_text, &args_text);
        if status == 1 {
            for (k, n) in kinds.iter_mut().enumerate() {
                *n += usize::from(bytes[0] >> k & 1 == 1);
            }
        }
        bridge_case(
            &mut c,
            &method_text,
            &args_text,
            format!("bridge {i}: {} {}", String::from_utf8_lossy(&method_text), String::from_utf8_lossy(&args_text)),
        );
        if let Some(a) = parse(&args_text)
            && random.chance(2)
        {
            let other = changed(&mut random, &a, 0).dump();
            bridge_case(&mut c, &method_text, &other, format!("bridge {i} changed: {}", String::from_utf8_lossy(&other)));
        }
    }
    let tally = c.check(&oracle);
    eprintln!("SDK bridge calls: {} refused, {} taken, alike; by kind {kinds:?}", tally[0], tally[1]);
    assert!(kinds.iter().all(|n| *n > 200), "{kinds:?}");
}

/// Every input of the fixture through both, as given and changed: links, bridge calls, transactions, payloads,
/// commits, themes, locales, the bot's data, viewers and answers.
#[test]
fn fixture_inputs_alike() {
    let Some(oracle) = Oracle::find("sdk_policy_oracle") else { return };
    let root = fixture();
    let mut random = Lcg::new(0x5d_0004);
    let mut c = Comparison::new();
    for round in 0..30 {
        for (i, l) in root["links"].arr().iter().enumerate() {
            let mut u = l["url"].as_str().to_vec();
            let edits = if round > 0 { mutate(&mut random, &mut u) } else { String::new() };
            link_case(&mut c, l["method"].as_str(), &u, format!("fixture link {i}{edits}"));
            if l["url"].is_string() {
                string_case(&mut c, &u, format!("fixture link {i} as a string{edits}"));
            }
        }
        for (i, b) in root["bridge"].arr().iter().enumerate() {
            let args = if round > 0 { changed(&mut random, &b["args"], 0) } else { b["args"].clone() };
            bridge_case(&mut c, &b["method"].dump(), &args.dump(), format!("fixture bridge {i} round {round}: {}", args.dump_lossy()));
        }
        for section in ["walletTransactions", "signDataPayloads", "storage", "themeParams", "locales", "haptics"] {
            for (i, e) in root[section].arr().iter().enumerate() {
                let input = expand_repeat(if section == "haptics" { &e["args"] } else { &e["input"] });
                let input = if round > 0 { changed(&mut random, &input, 0) } else { input };
                json_case(&mut c, &input.dump(), format!("fixture {section} {i} round {round}"));
            }
        }
        for (i, e) in root["widgetData"].arr().iter().enumerate() {
            let input = expand_repeat(&e["input"]);
            let input = if round > 0 { changed(&mut random, &input, 0) } else { input };
            let field = |v: &Json| v.is_string().then(|| v.as_str().to_vec());
            data_case(
                &mut c,
                field(&input["chatData"]),
                field(&input["publicData"]),
                e["copy"].as_bool(false) ^ (round % 2 == 1),
                format!("fixture data {i}"),
            );
        }
        for (i, e) in root["answers"].arr().iter().enumerate() {
            let input = expand_repeat(&e["input"]);
            let input = if round > 0 { changed(&mut random, &input, 0) } else { input };
            answer_case(&mut c, input["result"].is_string().then(|| input["result"].as_str().to_vec()), format!("fixture answer {i}"));
        }
        for (i, e) in root["viewer"].arr().iter().enumerate() {
            let input = expand_repeat(&e["input"]);
            let input = if round > 0 { changed(&mut random, &input, 0) } else { input };
            let field = |v: &Json| v.is_string().then(|| v.as_str().to_vec());
            let v = WidgetViewer {
                id: input["id"].as_number(0.0) as i64,
                first_name: field(&input["first_name"]),
                last_name: field(&input["last_name"]),
                username: field(&input["username"]),
                is_premium: input["is_premium"].as_bool(false),
            };
            viewer_case(&mut c, input["id"].is_number(), &v, format!("fixture viewer {i}"));
        }
    }
    let tally = c.check(&oracle);
    eprintln!("SDK fixture inputs: {} answers alike", tally[0] + tally[1]);
}

fn data_case(c: &mut Comparison, chat: Option<Vec<u8>>, public: Option<Vec<u8>>, copy: bool, what: String) {
    let record = Fields::new().opt_str(chat.as_deref()).opt_str(public.as_deref()).flag(copy).framed(b'w');
    let got = check_widget_data(&WidgetData { chat_data: chat, public_data: public }, copy).to_json().dump();
    c.case(&record, (1, Fields::new().str(&got).take()), what);
}

fn answer_case(c: &mut Comparison, result: Option<Vec<u8>>, what: String) {
    let record = Fields::new().opt_str(result.as_deref()).framed(b'a');
    let rust = match check_request_answer(&WidgetRequestAnswer { result }) {
        Some(a) => (1, a.to_json().dump()),
        None => (0, Vec::new()),
    };
    c.case(&record, rust, what);
}

fn viewer_case(c: &mut Comparison, has: bool, v: &WidgetViewer, what: String) {
    let record = Fields::new()
        .flag(has)
        .i64(v.id)
        .opt_str(v.first_name.as_deref())
        .opt_str(v.last_name.as_deref())
        .opt_str(v.username.as_deref())
        .flag(v.is_premium)
        .framed(b'v');
    let checked = check_viewer(has.then_some(v)).to_json().dump();
    c.case(&record, (1, Fields::new().str(&checked).str(&viewer_json(v).dump()).take()), what);
}

fn some_text(random: &mut Lcg) -> Option<Vec<u8>> {
    if random.chance(5) { None } else { Some(text_bytes(random)) }
}

#[test]
fn data_viewers_and_answers_alike() {
    let Some(oracle) = Oracle::find("sdk_policy_oracle") else { return };
    let mut random = Lcg::new(0x5d_0005);
    let mut c = Comparison::new();
    let mut seen = [0usize; 2];
    for i in 0..20000 {
        data_case(&mut c, some_text(&mut random), some_text(&mut random), random.chance(3), format!("data {i}"));
        answer_case(&mut c, some_text(&mut random), format!("answer {i}"));
        let name = |random: &mut Lcg| if random.chance(4) { None } else { Some(text_bytes(random)) };
        let v = WidgetViewer {
            id: match random.below(6) {
                0 => pick(&mut random, &[0, 1, -5, MAX_VIEWER_ID, MAX_VIEWER_ID + 1, i64::MIN, i64::MAX, 7]),
                _ => i64::from(random.next()) - 1000,
            },
            first_name: name(&mut random),
            last_name: name(&mut random),
            username: if random.chance(3) { None } else { Some(pick(&mut random, STRINGS).to_vec()) },
            is_premium: random.chance(2),
        };
        let checked = check_viewer(Some(&v));
        seen[0] += usize::from(checked.viewer.as_ref().is_some_and(|v| v.username.is_some() && v.first_name.is_some()));
        seen[1] += usize::from(!checked.rejected.is_empty());
        viewer_case(&mut c, !random.chance(10), &v, format!("viewer {i}: {v:?}"));
        let failure = random.below(7) as u8;
        let verdict = random.below(9) as u8;
        let field = pick(&mut random, STRINGS);
        let failures = [
            WidgetRequestFailure::None,
            WidgetRequestFailure::Timeout,
            WidgetRequestFailure::RateLimited,
            WidgetRequestFailure::Failed,
            WidgetRequestFailure::Rejected,
            WidgetRequestFailure::BadRequest,
            WidgetRequestFailure::NotAllowed,
        ];
        let verdicts = [
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
        let (f, v) = (failures[usize::from(failure)], verdicts[usize::from(verdict)]);
        let mut out = Fields::new();
        out.str(request_failure_code(f).as_bytes()).str(wallet_failure_code(f).as_bytes()).str(request_verdict_name(v).as_bytes());
        out.opt_str(request_refusal(v).map(str::as_bytes)).str(viewer_field_rule(field).as_bytes());
        c.case(
            &Fields::new().u8(failure).u8(verdict).str(field).framed(b'e'),
            (1, out.take()),
            format!("codes {failure} {verdict} {field:?}"),
        );
    }
    let tally = c.check(&oracle);
    eprintln!("SDK data, viewers, answers and codes: {} answers alike", tally[0] + tally[1] + tally[2]);
    assert!(seen.iter().all(|n| *n > 500), "{seen:?}");
}

#[test]
fn gates_alike() {
    let Some(oracle) = Oracle::find("sdk_policy_oracle") else { return };
    let mut random = Lcg::new(0x5d_0006);
    let mut c = Comparison::new();
    let mut verdicts = BTreeMap::new();
    let mut decisions = BTreeMap::new();
    let time = |random: &mut Lcg, now: &mut f64| -> f64 {
        *now += match random.below(10) {
            0 => 0.0,
            1..=4 => random.below(1200) as f64,
            5 | 6 => pick(random, &[999.0, 1000.0, 1001.0, 4999.0, 5000.0, 5001.0, 9999.0, 10000.0, 10001.0, 29999.0, 30000.0, 30001.0]),
            7 => random.below(40000) as f64,
            8 => -(random.below(100) as f64),
            _ => random.below(100) as f64 / 7.0,
        };
        if random.chance(60) { pick(random, &[f64::NAN, f64::INFINITY, f64::NEG_INFINITY, -0.0]) } else { *now }
    };
    for script in 0..1500 {
        c.setting(&Fields::new().framed(b'G'));
        let mut gate = RequestGate::new();
        let mut now = pick(&mut random, &[0.0, 1e12, -5000.0]);
        let (can_query, background, wallet) = (!random.chance(10), random.chance(2), !random.chance(4));
        for step in 0..60 {
            let kind = pick(&mut random, &[0u8, 0, 0, 1, 1, 2, 2, 3]);
            let at = time(&mut random, &mut now);
            let mut bits = random.next() as u8 & 0x3f;
            if !random.chance(4) {
                bits = u8::from(random.chance(4))
                    | u8::from(!random.chance(5)) << 1
                    | u8::from(can_query) << 2
                    | u8::from(background) << 3
                    | u8::from(random.chance(6)) << 4
                    | u8::from(wallet) << 5;
            }
            let answered = random.chance(2);
            let answered_at = if random.chance(2) { at } else { time(&mut random, &mut now) };
            let bit = |i: u8| bits >> i & 1 == 1;
            let mut out = Fields::new();
            match kind {
                0 => {
                    let s = RequestConditions {
                        gesture: bit(0),
                        visible: bit(1),
                        can_query: bit(2),
                        background_query: bit(3),
                        with_wallet: bit(4),
                        wallet: bit(5),
                    };
                    let v = request_verdict_name(gate.decide(at, &s));
                    *verdicts.entry(v).or_insert(0usize) += 1;
                    out.str(v.as_bytes());
                }
                1 => {
                    let s = WalletConditions { gesture: bit(0), visible: bit(1), can_query: bit(2), wallet: bit(5) };
                    let v = request_verdict_name(gate.decide_wallet(at, &s));
                    *verdicts.entry(v).or_insert(0usize) += 1;
                    out.str(v.as_bytes());
                }
                2 => gate.finish(answered.then_some(answered_at)),
                _ => {
                    out.flag(gate.show(at));
                }
            }
            out.flag(gate.busy());
            let record = Fields::new().u8(kind).f64(at).u8(bits).flag(answered).f64(answered_at).framed(b'g');
            c.case(&record, (1, out.take()), format!("request gate {script}, step {step}: {kind} at {at}"));
        }
        c.setting(&Fields::new().framed(b'H'));
        let mut haptics = HapticGate::new();
        let mut now = pick(&mut random, &[0.0, 1e12, -5000.0]);
        for step in 0..60 {
            let kind = pick(&mut random, &[0u8, 0, 0, 0, 1, 2]);
            let at = time(&mut random, &mut now);
            let visible = !random.chance(8);
            let mut out = Fields::new();
            match kind {
                0 => {
                    let d = haptics.decide(at, visible).name();
                    *decisions.entry(d).or_insert(0usize) += 1;
                    out.str(d.as_bytes());
                }
                1 => haptics.press(at),
                _ => haptics.release(at),
            }
            let record = Fields::new().u8(kind).f64(at).flag(visible).framed(b'h');
            c.case(&record, (1, out.take()), format!("haptic gate {script}, step {step}: {kind} at {at}"));
        }
    }
    let tally = c.check(&oracle);
    eprintln!("SDK gates: {} steps alike; verdicts {verdicts:?}, haptics {decisions:?}", tally[1]);
    assert!(verdicts.len() == 9 && verdicts.values().all(|n| *n > 50), "{verdicts:?}");
    assert!(decisions.len() == 3 && decisions.values().all(|n| *n > 500), "{decisions:?}");
}

fn map(random: &mut Lcg, keys: &[&str]) -> BTreeMap<Vec<u8>, Vec<u8>> {
    let mut out = BTreeMap::new();
    let big = random.chance(3);
    for i in 0..random.below(8) {
        let key = if random.chance(2) { pick(random, keys).as_bytes().to_vec() } else { format!("k{i}").into_bytes() };
        let value = if big {
            b"v".repeat(pick(random, &[500usize, 1000, 2000, 2046, 2047, 2048, 4090, 4094, 4095]))
        } else if random.chance(2) {
            parse(&color(random)).map(|v| v.as_str().to_vec()).unwrap_or_default()
        } else {
            text_bytes(random)
        };
        out.insert(key, value);
    }
    out
}

fn put_map(f: &mut Fields, m: &BTreeMap<Vec<u8>, Vec<u8>>) {
    f.u32(m.len() as u32);
    for (k, v) in m {
        f.str(k).str(v);
    }
}

/// What the oracle builds a wallet call from: the method, the data, the texts of a transaction and a payload.
type WalletParts = (bool, Vec<u8>, Vec<u8>, Vec<u8>);

fn wallet_parts(random: &mut Lcg) -> WalletParts {
    let sign = random.chance(2);
    let data = text_bytes(random);
    let tx = if random.chance(5) { value(random) } else { transaction(random) };
    let p = if random.chance(5) { value(random) } else { payload(random) };
    (sign, data, tx, p)
}

/// The call the oracle builds of `parts`: each text the checked value, or the default when it is refused.
fn wallet_call(parts: &WalletParts, f: &mut Fields) -> WalletCall {
    let (sign, data, tx, p) = parts;
    f.flag(*sign).str(data).str(tx).str(p);
    WalletCall {
        method: if *sign { WalletMethod::SignData } else { WalletMethod::SendTransaction },
        transaction: parse(tx).and_then(|v| check_wallet_transaction(&v)).unwrap_or_default(),
        payload: parse(p).and_then(|v| check_sign_data_payload(&v)).unwrap_or_default(),
        data: data.clone(),
    }
}

fn action(random: &mut Lcg, f: &mut Fields) -> WidgetAction {
    let kind = random.below(4) as u8;
    let a = WidgetAction {
        kind: [ActionKind::Ready, ActionKind::OpenLink, ActionKind::OpenTelegramLink, ActionKind::Haptic][usize::from(kind)],
        url: if random.chance(2) { url(random) } else { Vec::new() },
        haptic_kind: pick(random, STRINGS).to_vec(),
        haptic_value: if random.chance(3) { Vec::new() } else { pick(random, STRINGS).to_vec() },
    };
    f.u8(kind).str(&a.url).str(&a.haptic_kind).str(&a.haptic_value);
    a
}

#[test]
fn maps_and_values_built_by_hand_alike() {
    let Some(oracle) = Oracle::find("sdk_policy_oracle") else { return };
    let mut random = Lcg::new(0x5d_0007);
    let mut c = Comparison::new();
    let mut equal = 0;
    for i in 0..15000 {
        let params = map(&mut random, &THEME_KEYS);
        let mut f = Fields::new();
        put_map(&mut f, &params);
        c.case(
            &f.framed(b'T'),
            (1, Fields::new().str(&theme_params_json(&check_theme_params_map(&params)).dump()).take()),
            format!("theme map {i}"),
        );
        let storage = WidgetStorage { local: map(&mut random, &["a", "b"]), session: map(&mut random, &["a", "c"]) };
        let mut f = Fields::new();
        put_map(&mut f, &storage.local);
        put_map(&mut f, &storage.session);
        let (preload, accepted) = storage_preload_json(&storage);
        let mut out = Fields::new();
        out.u64(storage_bytes(&storage.local) as u64).u64(storage_bytes(&storage.session) as u64);
        out.flag(storage_area_fits(&storage.local)).flag(storage_area_fits(&storage.session)).flag(accepted).str(&preload.dump());
        c.case(&f.framed(b'S'), (1, out.take()), format!("storage preload {i}"));
        let mut f = Fields::new();
        let a = action(&mut random, &mut f);
        c.case(&f.framed(b'A'), (1, Fields::new().str(&a.to_json().dump()).take()), format!("action {i}"));
        let mut f = Fields::new();
        let parts = wallet_parts(&mut random);
        let first = wallet_call(&parts, &mut f);
        let fresh = wallet_parts(&mut random);
        let keep: [bool; 4] = std::array::from_fn(|_| !random.chance(3));
        let mixed = (
            if keep[0] { parts.0 } else { fresh.0 },
            if keep[1] { parts.1.clone() } else { fresh.1 },
            if keep[2] { parts.2.clone() } else { fresh.2 },
            if keep[3] { parts.3.clone() } else { fresh.3 },
        );
        let second = wallet_call(&mixed, &mut f);
        equal += usize::from(first == second);
        let mut out = Fields::new();
        out.str(&first.to_json().dump()).str(first.method_name().as_bytes()).flag(first == second);
        c.case(&f.framed(b'W'), (1, out.take()), format!("wallet call {i}"));
        let mask = random.next() as u8 & 0x3f;
        let mut f = Fields::new();
        f.u8(mask);
        let a = action(&mut random, &mut f);
        let edge = pick(&mut random, STRINGS).to_vec();
        let request = BridgeRequest { id: i64::from(random.next()), data: text_bytes(&mut random), wallet: random.chance(2) };
        f.str(&edge).i64(request.id).str(&request.data).flag(request.wallet);
        let wallet_id = i64::from(random.next() as i32);
        f.i64(wallet_id);
        let call = wallet_call(&wallet_parts(&mut random), &mut f);
        let copy = text_bytes(&mut random);
        let watch = random.chance(2);
        f.str(&copy).flag(watch);
        let bridge = BridgeCall {
            action: (mask & 1 != 0).then_some(a),
            resize_anchor: (mask & 2 != 0).then_some(edge),
            request: (mask & 4 != 0).then_some(request),
            wallet: (mask & 8 != 0).then_some(BridgeWallet { id: wallet_id, call }),
            copy_text: (mask & 16 != 0).then_some(copy),
            watch_public_data: (mask & 32 != 0).then_some(watch),
        };
        c.case(
            &f.framed(b'B'),
            (1, Fields::new().str(&bridge.to_json().dump()).take()),
            format!("bridge call by hand {i}, mask {mask:#x}"),
        );
    }
    let tally = c.check(&oracle);
    eprintln!("SDK maps and values built by hand: {} answers alike", tally[0] + tally[1] + tally[2]);
    assert!(equal > 1000, "{equal} equal wallet calls");
}
