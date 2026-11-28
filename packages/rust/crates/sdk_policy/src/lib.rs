//! widget-sandbox's SDK v1 checks (`packages/desktop/sandbox/include/twidget/sdk_policy.h`,
//! `src/sdk_policy.cpp`; the checked part of sdk-policy.ts, SdkPolicy.swift and SdkPolicy.kt, docs/sdk-v1.md):
//! bridge calls, links, haptics, Web Storage, theme parameters, the locale, the bot's data, the viewer,
//! requests and their gate, the wallet. Checked against `packages/core/tests/fixtures/sdk-policy.json`.
//!
//! Strings are UTF-8 bytes; a JS string's lone surrogate arrives in its 3-byte WTF-8 form (ED A0..BF xx), as
//! the engine hands it over and as `Json::parse_with(…, Surrogates::Keep)` gives it. `utf8_length` is the
//! byte length; `well_formed` is valid UTF-8 without a surrogate, what the TS `wellFormed` means for a JS
//! string.

#![forbid(unsafe_code)]

use std::collections::BTreeMap;

use twidget_json::Json;

/// One Web Storage area: keys and values as the widget set them.
pub type StorageArea = BTreeMap<Vec<u8>, Vec<u8>>;

/// Web Storage of one message as the client keeps it: the preload at start.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WidgetStorage {
    pub local: StorageArea,
    pub session: StorageArea,
}

/// What a `WidgetAction` asks for.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ActionKind {
    #[default]
    Ready,
    OpenLink,
    OpenTelegramLink,
    Haptic,
}

/// What a widget asks the client to do, after the glue's checks.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WidgetAction {
    pub kind: ActionKind,
    /// OpenLink, OpenTelegramLink: the URL to open (scheme and host lowercase, non-ASCII percent-encoded).
    pub url: Vec<u8>,
    /// Haptic: "impact", "notification" or "selection".
    pub haptic_kind: Vec<u8>,
    /// Haptic: the impact style or the notification type; empty for "selection".
    pub haptic_value: Vec<u8>,
}

impl WidgetAction {
    /// The TS `WidgetAction` shape: {kind:'ready'} | {kind:'openLink', url} | {kind:'haptic', haptic, value?}.
    pub fn to_json(&self) -> Json {
        let mut o = Json::object();
        match self.kind {
            ActionKind::Ready => {
                o.set("kind", "ready");
            }
            ActionKind::OpenLink => {
                o.set("kind", "openLink").set("url", self.url.as_slice());
            }
            ActionKind::OpenTelegramLink => {
                o.set("kind", "openTelegramLink").set("url", self.url.as_slice());
            }
            ActionKind::Haptic => {
                o.set("kind", "haptic").set("haptic", self.haptic_kind.as_slice());
                if !self.haptic_value.is_empty() {
                    o.set("value", self.haptic_value.as_slice());
                }
            }
        }
        o
    }
}

/// The bot's two strings as the client has them for this message (none = None), the full state each time.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WidgetData {
    pub chat_data: Option<Vec<u8>>,
    pub public_data: Option<Vec<u8>>,
}

/// Who looks at the widget, as the client passes it (Mini Apps' `WebAppUser`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WidgetViewer {
    pub id: i64,
    pub first_name: Option<Vec<u8>>,
    pub last_name: Option<Vec<u8>>,
    pub username: Option<Vec<u8>>,
    pub is_premium: bool,
}

/// Where the widget is and what it may do, from the message and the chat; fixed for the host's life.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WidgetContext {
    /// `is_copy`: the message is a forwarded copy: never any chatData.
    pub is_copy: bool,
    /// `can_query`: requests are allowed, on a tap.
    pub can_query: bool,
    /// `background_query`: requests without a tap too.
    pub background_query: bool,
    /// "private" | "group" | "supergroup" | "channel"; anything else is none.
    pub chat_type: Vec<u8>,
    /// The account that looks at the widget.
    pub viewer: Option<WidgetViewer>,
    /// An inline result shown before it is sent.
    pub preview: bool,
    /// The client has Gram Wallet for this user.
    pub wallet: bool,
}

/// What the client sends to the bot, as a callback button's press.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WidgetRequest {
    pub data: Vec<u8>,
    pub wallet: bool,
}

/// One message of a transaction, as TON Connect's `sendTransaction` has it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WalletMessage {
    pub address: Vec<u8>,
    pub amount: Vec<u8>,
    pub payload: Option<Vec<u8>>,
    pub state_init: Option<Vec<u8>>,
}

impl WalletMessage {
    pub fn to_json(&self) -> Json {
        let mut o = Json::object();
        o.set("address", self.address.as_slice()).set("amount", self.amount.as_slice());
        if let Some(p) = &self.payload {
            o.set("payload", p.as_slice());
        }
        if let Some(s) = &self.state_init {
            o.set("stateInit", s.as_slice());
        }
        o
    }
}

/// A transaction the widget asks the user to send: TON Connect's `sendTransaction` request.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WalletTransaction {
    /// `valid_until`, Unix seconds.
    pub valid_until: Option<i64>,
    /// "-239" (the mainnet) or "-3" (the testnet).
    pub network: Option<Vec<u8>>,
    pub from: Option<Vec<u8>>,
    /// 1–255 messages.
    pub messages: Vec<WalletMessage>,
}

impl WalletTransaction {
    pub fn to_json(&self) -> Json {
        let mut list = Json::array();
        for m in &self.messages {
            list.push(m.to_json());
        }
        let mut o = Json::object();
        if let Some(v) = self.valid_until {
            o.set("valid_until", v);
        }
        if let Some(n) = &self.network {
            o.set("network", n.as_slice());
        }
        if let Some(f) = &self.from {
            o.set("from", f.as_slice());
        }
        o.set("messages", list);
        o
    }
}

/// What the widget asks the user to sign: TON Connect's `signData` payload; `kind` is its `type`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SignDataPayload {
    pub kind: Vec<u8>,
    pub text: Option<Vec<u8>>,
    pub bytes: Option<Vec<u8>>,
    pub schema: Option<Vec<u8>>,
    pub cell: Option<Vec<u8>>,
    pub network: Option<Vec<u8>>,
    pub from: Option<Vec<u8>>,
}

impl SignDataPayload {
    pub fn to_json(&self) -> Json {
        let mut o = Json::object();
        o.set("type", self.kind.as_slice());
        for (k, v) in [
            ("text", &self.text),
            ("bytes", &self.bytes),
            ("schema", &self.schema),
            ("cell", &self.cell),
            ("network", &self.network),
            ("from", &self.from),
        ] {
            if let Some(v) = v {
                o.set(k, v.as_slice());
            }
        }
        o
    }
}

/// The wallet call's method.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum WalletMethod {
    #[default]
    SendTransaction,
    SignData,
}

/// A wallet call that passed its checks: `transaction` is set for `SendTransaction`, `payload` for `SignData`.
#[derive(Clone, Debug, Default)]
pub struct WalletCall {
    pub method: WalletMethod,
    pub transaction: WalletTransaction,
    pub payload: SignDataPayload,
    pub data: Vec<u8>,
}

impl PartialEq for WalletCall {
    fn eq(&self, o: &WalletCall) -> bool {
        self.method == o.method
            && self.data == o.data
            && if self.method == WalletMethod::SignData { self.payload == o.payload } else { self.transaction == o.transaction }
    }
}

impl WalletCall {
    /// "sendTransaction" or "signData".
    pub fn method_name(&self) -> &'static str {
        if self.method == WalletMethod::SignData { "signData" } else { "sendTransaction" }
    }

    /// {method, transaction | payload, data}, as the fixture writes it.
    pub fn to_json(&self) -> Json {
        let mut o = Json::object();
        o.set("method", self.method_name());
        if self.method == WalletMethod::SignData {
            o.set("payload", self.payload.to_json());
        } else {
            o.set("transaction", self.transaction.to_json());
        }
        o.set("data", self.data.as_slice());
        o
    }
}

/// The bot's answer as the client hands it over: the server's `result`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WidgetRequestAnswer {
    pub result: Option<Vec<u8>>,
}

/// A request the client could not complete; `None`: answered.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum WidgetRequestFailure {
    #[default]
    None,
    Timeout,
    RateLimited,
    Failed,
    Rejected,
    BadRequest,
    /// The host refused the call itself, as a client-mode engine does when its own checks refuse it.
    NotAllowed,
}

/// What the client reports for a request: the answer when `failure` is `None`, else the failure.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WidgetRequestOutcome {
    pub failure: WidgetRequestFailure,
    pub answer: WidgetRequestAnswer,
}

pub const MAX_LINK_LENGTH: i32 = 256;
/// Bytes of keys and values (UTF-8) per storage area and message.
pub const STORAGE_QUOTA_BYTES: i32 = 4096;
pub const MAX_LOCALE_LENGTH: i32 = 35;
pub const DEFAULT_LOCALE: &str = "en-US";
pub const HAPTIC_BURST: i32 = 5;
pub const HAPTIC_PER_SECOND: i32 = 10;
/// `haptic-limit` is reported at most this often.
pub const HAPTIC_REPORT_MS: f64 = 1000.0;
/// Haptics are free (under the rate limit) while a press is down and this long after it is lifted.
pub const HAPTIC_TOUCH_WINDOW_MS: f64 = 5000.0;
/// Outside a touch, the first haptic opens a series whose first second passes.
pub const HAPTIC_SERIES_MS: f64 = 1000.0;
/// Without a touch since the last series, the next one may start this long after the last one began.
pub const HAPTIC_SERIES_GAP_MS: f64 = 10000.0;
/// A press whose end the host never saw counts as lifted this long after it began.
pub const HAPTIC_PRESS_MAX_MS: f64 = 30000.0;
/// Handlers per `onEvent` type in the shim.
pub const MAX_HANDLERS_PER_EVENT: i32 = 16;
/// The widget profile this glue enforces.
pub const PROFILE_VERSION: i32 = twidget_bundle_stamp::version::PROFILE;
/// Two full areas of the quota at 12 JSON bytes per byte: a longer `storage` host call cannot be within the quota.
pub const MAX_STORAGE_COMMIT_BYTES: usize = 2 * 12 * STORAGE_QUOTA_BYTES as usize + 64;
/// `bridge-rejected`, `activation-blocked` and `storage-rejected` are reported at most this often per widget.
pub const MAX_REJECTION_REPORTS: i32 = 20;

pub const THEME_KEYS: [&str; 15] = [
    "bg_color",
    "text_color",
    "hint_color",
    "link_color",
    "button_color",
    "button_text_color",
    "secondary_bg_color",
    "accent_text_color",
    "subtitle_text_color",
    "destructive_text_color",
    "section_bg_color",
    "section_header_text_color",
    "section_separator_color",
    "header_bg_color",
    "bottom_bar_bg_color",
];
pub const HAPTIC_IMPACT_STYLES: [&str; 5] = ["light", "medium", "heavy", "rigid", "soft"];
pub const HAPTIC_NOTIFICATION_TYPES: [&str; 3] = ["error", "success", "warning"];
pub const TELEGRAM_HOSTS: [&str; 3] = ["t.me", "telegram.me", "telegram.dog"];
/// Methods that need the task's user gesture; one press unlocks one of them.
pub const PRIVILEGED_METHODS: [&str; 3] = ["openLink", "openTelegramLink", "copyText"];
/// The bridge methods a preview may call.
pub const PREVIEW_METHODS: [&str; 2] = ["ready", "setResizeAnchor"];
/// The edges of the bubble that can stay put on screen when the widget's height changes.
pub const RESIZE_ANCHORS: [&str; 2] = ["top", "bottom"];
/// Until the widget calls `setResizeAnchor`.
pub const DEFAULT_RESIZE_ANCHOR: &str = "top";

/// UTF-8 bytes of `chatData` and of `publicData` each.
pub const MAX_WIDGET_DATA_BYTES: i32 = 1024;
/// UTF-8 bytes of a request's `data`.
pub const MAX_REQUEST_DATA_BYTES: i32 = 64;
/// UTF-8 bytes of an answer's `result`.
pub const MAX_REQUEST_RESULT_BYTES: i32 = 1024;
/// UTF-16 code units of a `copyText` text.
pub const MAX_COPY_TEXT_LENGTH: i32 = 256;
/// A background request at most this often, per widget.
pub const BACKGROUND_REQUEST_INTERVAL_MS: f64 = 5000.0;
/// The request ids the shim numbers its calls with.
pub const MAX_REQUEST_ID: i64 = 2147483647;
/// A Telegram user id fits in 53 bits.
pub const MAX_VIEWER_ID: i64 = 9007199254740991;
/// UTF-16 code units of the viewer's first and last name.
pub const MAX_VIEWER_NAME_LENGTH: i32 = 64;
/// The viewer's username: 5–32 characters of A–Z, a–z, 0–9 and _.
pub const MIN_VIEWER_USERNAME_LENGTH: i32 = 5;
pub const MAX_VIEWER_USERNAME_LENGTH: i32 = 32;
/// The chat kinds a host may report.
pub const CHAT_TYPES: [&str; 4] = ["private", "group", "supergroup", "channel"];
/// What a refused or failed request rejects with in the widget; REJECTED and BAD_REQUEST only for a wallet call.
pub const REQUEST_ERRORS: [&str; 8] =
    ["NOT_ALLOWED", "BUSY", "RATE_LIMITED", "TIMEOUT", "FAILED", "INVALID_ANSWER", "REJECTED", "BAD_REQUEST"];

/// Messages in one transaction.
pub const MAX_WALLET_MESSAGES: usize = 255;
/// Characters of a message's `payload` or `stateInit`.
pub const MAX_WALLET_BOC_LENGTH: usize = 8192;
/// Characters of every message's `payload` and `stateInit` together.
pub const MAX_WALLET_BOC_TOTAL: usize = 65536;
/// UTF-8 bytes of a `signData` text; characters of its `bytes` or `cell`.
pub const MAX_SIGN_DATA_LENGTH: usize = 4096;
/// UTF-8 bytes of a `cell` payload's TL-B schema.
pub const MAX_SIGN_DATA_SCHEMA_BYTES: usize = 1024;
/// The longest valid `bridge` argument: a wallet transaction.
pub const MAX_BRIDGE_CALL_BYTES: usize = MAX_WALLET_BOC_TOTAL + 256 * MAX_WALLET_MESSAGES + 1024;
/// The other arguments the glue parses; a longer one is refused unparsed.
pub const MAX_HOST_CALL_BYTES: usize = 65536;
/// After the answer to a call made by a tap, one wallet call may follow without a new tap this long.
pub const WALLET_GRANT_MS: f64 = 10000.0;
/// TON Connect's network ids: the mainnet and the testnet.
pub const WALLET_NETWORKS: [&str; 2] = ["-239", "-3"];

fn contains(list: &[&str], s: &[u8]) -> bool {
    list.iter().any(|x| x.as_bytes() == s)
}

fn is_hex(c: u8) -> bool {
    c.is_ascii_hexdigit()
}

fn is_alpha(c: u8) -> bool {
    c.is_ascii_alphabetic()
}

fn is_digit(c: u8) -> bool {
    c.is_ascii_digit()
}

/// A JSON number that is an integer within [min, max]: not 1.5, not "1".
fn integer_in(v: &Json, min: f64, max: f64) -> Option<i64> {
    let n = v.as_number(0.0);
    if !v.is_number() || !n.is_finite() || n.floor() != n || n < min || n > max {
        return None;
    }
    Some(n as i64)
}

/// Lowercases A–Z only: a Unicode mapping must not turn a non-ASCII host into an ASCII one.
fn ascii_lower(s: &[u8]) -> Vec<u8> {
    s.to_ascii_lowercase()
}

fn is_telegram_host(host: &[u8]) -> bool {
    TELEGRAM_HOSTS.iter().any(|t| {
        let t = t.as_bytes();
        host == t || (host.len() > t.len() && host.ends_with(t) && host[host.len() - t.len() - 1] == b'.')
    })
}

/// ASCII that may stand unescaped after the authority: RFC 3986 unreserved, reserved and `%`.
fn is_url_ascii(c: u8) -> bool {
    is_alpha(c) || is_digit(c) || b"-._~:/?#[]@!$&'()*+,;=%".contains(&c)
}

/// Length of the well-formed UTF-8 sequence of a scalar value at `s[i]` (a non-ASCII lead byte), 0 when it
/// is not one: a bad lead or continuation byte, an overlong form, a surrogate, above U+10FFFF, truncated.
fn utf8_sequence(s: &[u8], i: usize) -> usize {
    let at = |k: usize| -> u32 { s.get(i + k).map_or(0x100, |&b| u32::from(b)) };
    let cont = |k: usize, lo: u32, hi: u32| (lo..=hi).contains(&at(k));
    let any = |k: usize| cont(k, 0x80, 0xBF);
    match at(0) {
        0xC2..=0xDF if any(1) => 2,
        0xE0 if cont(1, 0xA0, 0xBF) && any(2) => 3,
        0xE1..=0xEC | 0xEE | 0xEF if any(1) && any(2) => 3,
        0xED if cont(1, 0x80, 0x9F) && any(2) => 3,
        0xF0 if cont(1, 0x90, 0xBF) && any(2) && any(3) => 4,
        0xF1..=0xF3 if any(1) && any(2) && any(3) => 4,
        0xF4 if cont(1, 0x80, 0x8F) && any(2) && any(3) => 4,
        _ => 0,
    }
}

/// The member a JS object built by JSON.parse would have: with a key given twice, the last value.
fn member<'a>(object: &'a Json, key: &[u8]) -> Option<&'a Json> {
    let mut found = None;
    for (k, v) in object.obj() {
        if k.as_slice() == key {
            found = Some(v);
        }
    }
    found
}

fn is_theme_color(v: &[u8]) -> bool {
    v.len() == 7 && v[0] == b'#' && v[1..].iter().all(|&c| is_hex(c))
}

pub fn is_privileged_method(method: &[u8]) -> bool {
    contains(&PRIVILEGED_METHODS, method)
}

/// Exactly one of `PREVIEW_METHODS` (case-sensitive).
pub fn allowed_in_preview(method: &[u8]) -> bool {
    contains(&PREVIEW_METHODS, method)
}

/// UTF-8 length of a string: its byte length (a lone surrogate in WTF-8 is 3 bytes, as the TS version counts it).
pub fn utf8_length(s: &[u8]) -> usize {
    s.len()
}

/// UTF-16 length of a UTF-8 string (the JS `length`): 2 for each 4-byte sequence, 1 for every other one (and
/// for each byte that starts none).
pub fn utf16_length(s: &[u8]) -> usize {
    let mut n = 0;
    let mut i = 0;
    while i < s.len() {
        let len = if s[i] < 0x80 { 1 } else { utf8_sequence(s, i) };
        n += if len == 4 { 2 } else { 1 };
        i += if len != 0 { len } else { 1 };
    }
    n
}

/// Valid UTF-8 without a surrogate code point (WTF-8 ED A0..BF xx).
pub fn well_formed(s: &[u8]) -> bool {
    let mut i = 0;
    while i < s.len() {
        let len = if s[i] < 0x80 { 1 } else { utf8_sequence(s, i) };
        if len == 0 {
            return false;
        }
        i += len;
    }
    true
}

/// The URL a link call opens, or None when it breaks the rules: an absolute `https:` URL, no user name or
/// password, an ASCII host of LDH labels, an optional port, at most 256 bytes as given and at most 256 once
/// non-ASCII bytes after the host are percent-encoded. `openTelegramLink` takes only t.me, telegram.me and
/// telegram.dog (and their subdomains); any other `method` is `openLink`, which takes every other host.
/// Scheme and host come out lowercase (A–Z only), the rest as given.
pub fn check_link(method: &[u8], url: &[u8]) -> Option<Vec<u8>> {
    if utf8_length(url) > MAX_LINK_LENGTH as usize {
        return None;
    }
    if url.len() < 8 || ascii_lower(&url[..8]) != b"https://" {
        return None;
    }
    let mut end = 8;
    while end < url.len() && !matches!(url[end], b'/' | b'?' | b'#') {
        end += 1;
    }
    let authority = &url[8..end];
    if authority.contains(&b'@') {
        return None;
    }
    let colon = authority.iter().position(|&c| c == b':');
    let host = ascii_lower(match colon {
        None => authority,
        Some(c) => &authority[..c],
    });
    let port = colon.map(|c| &authority[c + 1..]);
    if let Some(port) = port {
        if port.is_empty() || port.len() > 5 {
            return None;
        }
        let mut value: i64 = 0;
        for &c in port {
            if !is_digit(c) {
                return None;
            }
            value = value * 10 + i64::from(c - b'0');
        }
        if value > 65535 {
            return None;
        }
    }
    if host.is_empty() || host.len() > 253 {
        return None;
    }
    let mut start = 0;
    loop {
        let dot = host[start..].iter().position(|&c| c == b'.').map(|n| n + start);
        let label = &host[start..dot.unwrap_or(host.len())];
        if label.is_empty() || label.len() > 63 || label[0] == b'-' || label[label.len() - 1] == b'-' {
            return None;
        }
        if !label.iter().all(|&c| is_digit(c) || c.is_ascii_lowercase() || c == b'-') {
            return None;
        }
        match dot {
            None => break,
            Some(dot) => start = dot + 1,
        }
    }
    if is_telegram_host(&host) != (method == b"openTelegramLink") {
        return None;
    }
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    let mut rest = Vec::new();
    let mut i = end;
    while i < url.len() {
        let c = url[i];
        if c < 0x80 {
            if !is_url_ascii(c) {
                return None;
            }
            if c == b'%' && !(i + 2 < url.len() && is_hex(url[i + 1]) && is_hex(url[i + 2])) {
                return None;
            }
            rest.push(c);
            i += 1;
            continue;
        }
        let n = utf8_sequence(url, i);
        if n == 0 {
            return None;
        }
        for &b in &url[i..i + n] {
            rest.extend_from_slice(&[b'%', HEX[usize::from(b >> 4)], HEX[usize::from(b & 15)]]);
        }
        i += n;
    }
    let mut out = b"https://".to_vec();
    out.extend_from_slice(&host);
    if let Some(port) = port {
        out.push(b':');
        out.extend_from_slice(port);
    }
    out.extend_from_slice(&rest);
    if out.len() > MAX_LINK_LENGTH as usize {
        return None;
    }
    Some(out)
}

/// The same for a JSON value: anything but a string is None.
pub fn check_link_value(method: &[u8], url: &Json) -> Option<Vec<u8>> {
    if !url.is_string() {
        return None;
    }
    check_link(method, url.as_str())
}

/// A `haptic` bridge call's haptic.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct HapticCall {
    /// "impact" | "notification" | "selection".
    pub haptic: Vec<u8>,
    /// The style or type; none for "selection".
    pub value: Option<Vec<u8>>,
}

/// The haptic of a `haptic` bridge call's args, or None: `['impact', style]`, `['notification', type]`, `['selection']`.
pub fn check_haptic(args: &Json) -> Option<HapticCall> {
    if !args.is_array() {
        return None;
    }
    let kind = &args[0];
    let value = &args[1];
    let call =
        |haptic: &str, value: Option<&[u8]>| Some(HapticCall { haptic: haptic.as_bytes().to_vec(), value: value.map(<[u8]>::to_vec) });
    if kind.is_string()
        && kind.as_str() == b"impact"
        && args.size() == 2
        && value.is_string()
        && contains(&HAPTIC_IMPACT_STYLES, value.as_str())
    {
        return call("impact", Some(value.as_str()));
    }
    if kind.is_string()
        && kind.as_str() == b"notification"
        && args.size() == 2
        && value.is_string()
        && contains(&HAPTIC_NOTIFICATION_TYPES, value.as_str())
    {
        return call("notification", Some(value.as_str()));
    }
    if kind.is_string() && kind.as_str() == b"selection" && args.size() == 1 {
        return call("selection", None);
    }
    None
}

/// The resize anchor of a `setResizeAnchor` call's argument, or None: exactly "top" or "bottom".
pub fn check_resize_anchor(edge: &[u8]) -> Option<Vec<u8>> {
    if !contains(&RESIZE_ANCHORS, edge) {
        return None;
    }
    Some(edge.to_vec())
}

/// The same for a JSON value: anything but a string is None.
pub fn check_resize_anchor_value(edge: &Json) -> Option<Vec<u8>> {
    if edge.is_string() { check_resize_anchor(edge.as_str()) } else { None }
}

/// A `request` bridge call: the shim's id, the data and whether it asks for the wallet.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BridgeRequest {
    /// The shim's number for the call, 1 … 2147483647.
    pub id: i64,
    /// 1–64 bytes of UTF-8, well-formed.
    pub data: Vec<u8>,
    pub wallet: bool,
}

/// A `wallet` bridge call: the shim's id and the checked call.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct BridgeWallet {
    pub id: i64,
    pub call: WalletCall,
}

/// A bridge call that passed its checks (the TS `BridgeCall`); exactly one is set.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct BridgeCall {
    pub action: Option<WidgetAction>,
    /// `setResizeAnchor`: "top" or "bottom".
    pub resize_anchor: Option<Vec<u8>>,
    pub request: Option<BridgeRequest>,
    pub wallet: Option<BridgeWallet>,
    /// `copyText`: 1–256 UTF-16 code units, well-formed.
    pub copy_text: Option<Vec<u8>>,
    /// `watchPublicData`: whether the widget watches its shared `public_data`.
    pub watch_public_data: Option<bool>,
}

impl BridgeCall {
    /// The TS `BridgeCall` shape: the action's, {kind:'setResizeAnchor', edge}, {kind:'request', id, data[, wallet: true]},
    /// {kind:'wallet', id, call}, {kind:'copyText', text} or {kind:'watchPublicData', on}.
    pub fn to_json(&self) -> Json {
        if let Some(action) = &self.action {
            return action.to_json();
        }
        let mut o = Json::object();
        if let Some(edge) = &self.resize_anchor {
            o.set("kind", "setResizeAnchor").set("edge", edge.as_slice());
        }
        if let Some(r) = &self.request {
            o.set("kind", "request").set("id", r.id).set("data", r.data.as_slice());
            if r.wallet {
                o.set("wallet", true);
            }
        }
        if let Some(w) = &self.wallet {
            o.set("kind", "wallet").set("id", w.id).set("call", w.call.to_json());
        }
        if let Some(text) = &self.copy_text {
            o.set("kind", "copyText").set("text", text.as_slice());
        }
        if let Some(on) = self.watch_public_data {
            o.set("kind", "watchPublicData").set("on", on);
        }
        o
    }
}

/// The checked call, or None when the method is unknown or its arguments are wrong (`bridge-rejected`).
pub fn check_bridge_call(method: &Json, args: &Json) -> Option<BridgeCall> {
    if !args.is_array() || !method.is_string() {
        return None;
    }
    let m = method.as_str();
    let mut call = BridgeCall::default();
    match m {
        b"ready" => {
            if args.size() != 0 {
                return None;
            }
            call.action = Some(WidgetAction { kind: ActionKind::Ready, ..WidgetAction::default() });
        }
        b"openLink" | b"openTelegramLink" => {
            let url = if args.size() == 1 { check_link_value(m, &args[0]) } else { None }?;
            let kind = if m == b"openLink" { ActionKind::OpenLink } else { ActionKind::OpenTelegramLink };
            call.action = Some(WidgetAction { kind, url, ..WidgetAction::default() });
        }
        b"haptic" => {
            let h = check_haptic(args)?;
            call.action = Some(WidgetAction {
                kind: ActionKind::Haptic,
                haptic_kind: h.haptic,
                haptic_value: h.value.unwrap_or_default(),
                ..WidgetAction::default()
            });
        }
        b"setResizeAnchor" => {
            call.resize_anchor = if args.size() == 1 { check_resize_anchor_value(&args[0]) } else { None };
            call.resize_anchor.as_ref()?;
        }
        b"request" => {
            let data = if args.size() == 2 || args.size() == 3 { check_request_data_value(&args[1]) } else { None };
            let wallet = if args.size() == 3 { check_request_options(&args[2]) } else { Some(false) };
            let id = integer_in(&args[0], 1.0, MAX_REQUEST_ID as f64);
            let (Some(data), Some(wallet), Some(id)) = (data, wallet, id) else { return None };
            call.request = Some(BridgeRequest { id, data, wallet });
        }
        b"wallet" => {
            let id = integer_in(&args[0], 1.0, MAX_REQUEST_ID as f64);
            let data = if args.size() == 4 { check_request_data_value(&args[3]) } else { None };
            let (Some(id), Some(data)) = (id, data) else { return None };
            let mut w = WalletCall { data, ..WalletCall::default() };
            if args[1].is_string() && args[1].as_str() == b"sendTransaction" {
                w.transaction = check_wallet_transaction(&args[2])?;
            } else if args[1].is_string() && args[1].as_str() == b"signData" {
                w.payload = check_sign_data_payload(&args[2])?;
                w.method = WalletMethod::SignData;
            } else {
                return None;
            }
            call.wallet = Some(BridgeWallet { id, call: w });
        }
        b"watchPublicData" => {
            if args.size() != 1 || !args[0].is_bool() {
                return None;
            }
            call.watch_public_data = Some(args[0].as_bool(false));
        }
        b"copyText" => {
            call.copy_text = if args.size() == 1 { check_copy_text_value(&args[0]) } else { None };
            call.copy_text.as_ref()?;
        }
        _ => return None,
    }
    Some(call)
}

/// Whether a `request`'s options ask for the wallet, or None: an object with at most `wallet`, a boolean
/// (absent or null: false).
pub fn check_request_options(options: &Json) -> Option<bool> {
    if !options.is_object() {
        return None;
    }
    if options.obj().iter().any(|(k, _)| k.as_slice() != b"wallet") {
        return None;
    }
    let w = &options["wallet"];
    if w.is_null() {
        return Some(false);
    }
    if !w.is_bool() {
        return None;
    }
    Some(w.as_bool(false))
}

/// A–Z, a–z, 0–9, `+`, `/`, `_`, `-`: base64 and base64url together.
fn is_base64_char(c: u8) -> bool {
    is_alpha(c) || is_digit(c) || c == b'+' || c == b'/' || c == b'_' || c == b'-'
}

fn only_keys(o: &Json, keys: &[&str]) -> bool {
    o.obj().iter().all(|(k, _)| contains(keys, k))
}

/// Base64 or base64url of 1 to `max` characters, with at most two `=` at the end.
fn base64(v: &Json, max: usize) -> Option<Vec<u8>> {
    if !v.is_string() || v.as_str().len() > max {
        return None;
    }
    let mut s = v.as_str();
    for _ in 0..2 {
        match s {
            [rest @ .., b'='] => s = rest,
            _ => break,
        }
    }
    if s.is_empty() || !s.iter().all(|&c| is_base64_char(c)) {
        return None;
    }
    Some(v.as_str().to_vec())
}

/// Nanotons: a decimal string of at most 37 digits without a leading zero (`0` itself is an amount).
fn is_amount(s: &[u8]) -> bool {
    !s.is_empty() && s.len() <= 37 && s.iter().all(|&c| is_digit(c)) && (s.len() == 1 || s[0] != b'0')
}

/// Text the server and the bot can take: well-formed, no NUL, 1 to `max` UTF-8 bytes.
fn sign_text(v: &Json, max: usize) -> Option<Vec<u8>> {
    if !v.is_string() {
        return None;
    }
    let s = v.as_str();
    if s.is_empty() || !well_formed(s) || s.contains(&0) || utf8_length(s) > max {
        return None;
    }
    Some(s.to_vec())
}

/// The optional `network` and `from` of a transaction or a payload; false when one breaks its rule.
fn wallet_target(input: &Json, network: &mut Option<Vec<u8>>, from: &mut Option<Vec<u8>>) -> bool {
    let n = &input["network"];
    if !n.is_null() {
        if !n.is_string() || !contains(&WALLET_NETWORKS, n.as_str()) {
            return false;
        }
        *network = Some(n.as_str().to_vec());
    }
    let f = &input["from"];
    if !f.is_null() {
        *from = check_wallet_address(f);
        if from.is_none() {
            return false;
        }
    }
    true
}

/// A TON address, or None: raw (`0:` or `-1:` and 64 hex digits) or user-friendly (48 characters of base64 or
/// base64url); a transaction's `from`.
pub fn check_wallet_address(address: &Json) -> Option<Vec<u8>> {
    if !address.is_string() {
        return None;
    }
    let s = address.as_str();
    if s.len() == 48 && s.iter().all(|&c| is_base64_char(c)) {
        return Some(s.to_vec());
    }
    let prefix = if s.starts_with(b"0:") {
        2
    } else if s.starts_with(b"-1:") {
        3
    } else {
        0
    };
    if prefix == 0 || s.len() != prefix + 64 || !s[prefix..].iter().all(|&c| is_hex(c)) {
        return None;
    }
    Some(s.to_vec())
}

/// A message's destination, or None: user-friendly only.
pub fn check_wallet_destination(address: &Json) -> Option<Vec<u8>> {
    if !address.is_string() {
        return None;
    }
    let s = address.as_str();
    if s.len() != 48 || !s.iter().all(|&c| is_base64_char(c)) {
        return None;
    }
    Some(s.to_vec())
}

/// A transaction for `sendTransaction`, or None: TON Connect's fields only (`valid_until`, `network`, `from`,
/// `messages`), 1–255 messages of `address`, `amount` and optionally `payload` and `stateInit`. Null fields
/// are left out.
pub fn check_wallet_transaction(tx: &Json) -> Option<WalletTransaction> {
    if !tx.is_object() || !only_keys(tx, &["valid_until", "network", "from", "messages"]) {
        return None;
    }
    let mut out = WalletTransaction::default();
    let v = &tx["valid_until"];
    if !v.is_null() {
        out.valid_until = Some(integer_in(v, 1.0, 4294967295.0)?);
    }
    if !wallet_target(tx, &mut out.network, &mut out.from) {
        return None;
    }
    let list = &tx["messages"];
    if !list.is_array() || list.size() < 1 || list.size() > MAX_WALLET_MESSAGES {
        return None;
    }
    let mut total = 0;
    for m in list.arr() {
        if !m.is_object() || !only_keys(m, &["address", "amount", "payload", "stateInit"]) {
            return None;
        }
        let address = check_wallet_destination(&m["address"]);
        let amount = &m["amount"];
        let address = address?;
        if !amount.is_string() || !is_amount(amount.as_str()) {
            return None;
        }
        let mut message = WalletMessage { address, amount: amount.as_str().to_vec(), payload: None, state_init: None };
        if !m["payload"].is_null() {
            message.payload = Some(base64(&m["payload"], MAX_WALLET_BOC_LENGTH)?);
        }
        if !m["stateInit"].is_null() {
            message.state_init = Some(base64(&m["stateInit"], MAX_WALLET_BOC_LENGTH)?);
        }
        total += message.payload.as_ref().map_or(0, Vec::len) + message.state_init.as_ref().map_or(0, Vec::len);
        out.messages.push(message);
    }
    if total > MAX_WALLET_BOC_TOTAL {
        return None;
    }
    Some(out)
}

/// A `signData` payload, or None: TON Connect's three kinds with their fields only, and optionally `network`
/// and `from` as a transaction's. Null fields are left out.
pub fn check_sign_data_payload(payload: &Json) -> Option<SignDataPayload> {
    if !payload.is_object() || !payload["type"].is_string() {
        return None;
    }
    let kind = payload["type"].as_str();
    let mut out = SignDataPayload { kind: kind.to_vec(), ..SignDataPayload::default() };
    match kind {
        b"text" => {
            if !only_keys(payload, &["type", "network", "from", "text"]) {
                return None;
            }
            out.text = Some(sign_text(&payload["text"], MAX_SIGN_DATA_LENGTH)?);
        }
        b"binary" => {
            if !only_keys(payload, &["type", "network", "from", "bytes"]) {
                return None;
            }
            out.bytes = Some(base64(&payload["bytes"], MAX_SIGN_DATA_LENGTH)?);
        }
        b"cell" => {
            if !only_keys(payload, &["type", "network", "from", "schema", "cell"]) {
                return None;
            }
            out.schema = Some(sign_text(&payload["schema"], MAX_SIGN_DATA_SCHEMA_BYTES)?);
            out.cell = Some(base64(&payload["cell"], MAX_SIGN_DATA_LENGTH)?);
        }
        _ => return None,
    }
    if !wallet_target(payload, &mut out.network, &mut out.from) {
        return None;
    }
    Some(out)
}

/// A request's `data`, or None: a well-formed string of 1–64 UTF-8 bytes.
pub fn check_request_data(data: &[u8]) -> Option<Vec<u8>> {
    if !well_formed(data) {
        return None;
    }
    let n = utf8_length(data);
    if n < 1 || n > MAX_REQUEST_DATA_BYTES as usize {
        return None;
    }
    Some(data.to_vec())
}

pub fn check_request_data_value(data: &Json) -> Option<Vec<u8>> {
    if data.is_string() { check_request_data(data.as_str()) } else { None }
}

/// A `copyText` text, or None: a well-formed string of 1–256 UTF-16 code units.
pub fn check_copy_text(text: &[u8]) -> Option<Vec<u8>> {
    if !well_formed(text) {
        return None;
    }
    let n = utf16_length(text);
    if n < 1 || n > MAX_COPY_TEXT_LENGTH as usize {
        return None;
    }
    Some(text.to_vec())
}

pub fn check_copy_text_value(text: &Json) -> Option<Vec<u8>> {
    if text.is_string() { check_copy_text(text.as_str()) } else { None }
}

/// One of `chatData` / `publicData`: a well-formed string of 1–1024 UTF-8 bytes without NUL; None for none
/// (the empty string the bot clears it with) and for anything else.
pub fn check_widget_data_value(value: &[u8]) -> Option<Vec<u8>> {
    if value.is_empty() || !well_formed(value) || value.contains(&0) || utf8_length(value) > MAX_WIDGET_DATA_BYTES as usize {
        return None;
    }
    Some(value.to_vec())
}

fn or_null(s: &Option<Vec<u8>>) -> Json {
    match s {
        Some(s) => Json::String(s.clone()),
        None => Json::Null,
    }
}

fn names_json(names: &[&str]) -> Json {
    let mut r = Json::array();
    for name in names {
        r.push(*name);
    }
    r
}

/// The bot's data as the widget sees it, and the fields the client passed that broke the rules
/// ("chatData" and/or "publicData", in that order).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CheckedWidgetData {
    pub chat_data: Option<Vec<u8>>,
    pub public_data: Option<Vec<u8>>,
    pub rejected: Vec<&'static str>,
}

impl CheckedWidgetData {
    /// The TS `CheckedWidgetData` shape: {chatData, publicData, rejected} with JSON nulls for none.
    pub fn to_json(&self) -> Json {
        let mut o = Json::object();
        o.set("chatData", or_null(&self.chat_data)).set("publicData", or_null(&self.public_data));
        o.set("rejected", names_json(&self.rejected));
        o
    }
}

/// The full state of the bot's two strings, as the client passes it at creation and in `setData`. A copy has
/// no `chatData`: the glue drops what the client passed, silently.
pub fn check_widget_data(data: &WidgetData, copy: bool) -> CheckedWidgetData {
    let mut out = CheckedWidgetData::default();
    let check = |v: &Option<Vec<u8>>, name: &'static str, rejected: &mut Vec<&'static str>| {
        let field = v.as_deref().and_then(check_widget_data_value);
        if field.is_none() && v.as_ref().is_some_and(|v| !v.is_empty()) {
            rejected.push(name);
        }
        field
    };
    if !copy {
        out.chat_data = check(&data.chat_data, "chatData", &mut out.rejected);
    }
    out.public_data = check(&data.public_data, "publicData", &mut out.rejected);
    out
}

/// The viewer as the widget sees it, and the fields the client passed that broke their rules.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CheckedViewer {
    /// None without an id of 1 … 2^53 − 1 (not reported); else the id and the fields that passed.
    pub viewer: Option<WidgetViewer>,
    /// "first_name", "last_name", "username", in this order.
    pub rejected: Vec<&'static str>,
}

impl CheckedViewer {
    /// The TS shape: {viewer: {id, first_name?, last_name?, username?, is_premium?: true} | null, rejected}.
    pub fn to_json(&self) -> Json {
        let mut o = Json::object();
        o.set("viewer", self.viewer.as_ref().map_or(Json::Null, viewer_json)).set("rejected", names_json(&self.rejected));
        o
    }
}

/// The viewer's check (the TS `checkViewer`): no viewer without a valid id; a name of 1–64 UTF-16 code units,
/// well-formed; a username of 5–32 ASCII letters, digits and _; Premium kept only when true.
pub fn check_viewer(viewer: Option<&WidgetViewer>) -> CheckedViewer {
    let mut out = CheckedViewer::default();
    let Some(viewer) = viewer else { return out };
    if viewer.id < 1 || viewer.id > MAX_VIEWER_ID {
        return out;
    }
    let mut v = WidgetViewer { id: viewer.id, ..WidgetViewer::default() };
    let name = |s: &Option<Vec<u8>>, key: &'static str, rejected: &mut Vec<&'static str>| {
        let s = s.as_ref()?;
        let n = utf16_length(s);
        if well_formed(s) && n >= 1 && n <= MAX_VIEWER_NAME_LENGTH as usize {
            Some(s.clone())
        } else {
            rejected.push(key);
            None
        }
    };
    v.first_name = name(&viewer.first_name, "first_name", &mut out.rejected);
    v.last_name = name(&viewer.last_name, "last_name", &mut out.rejected);
    if let Some(u) = &viewer.username {
        let mut ok = u.len() >= MIN_VIEWER_USERNAME_LENGTH as usize && u.len() <= MAX_VIEWER_USERNAME_LENGTH as usize;
        for &c in u {
            ok = ok && (is_alpha(c) || is_digit(c) || c == b'_');
        }
        if ok {
            v.username = Some(u.clone());
        } else {
            out.rejected.push("username");
        }
    }
    v.is_premium = viewer.is_premium;
    out.viewer = Some(v);
    out
}

/// The viewer in the widget's field names: `id`, then `first_name`, `last_name`, `username` when set,
/// `is_premium` only when true. Unchecked: pass a checked viewer.
pub fn viewer_json(viewer: &WidgetViewer) -> Json {
    let mut o = Json::object();
    o.set("id", viewer.id);
    if let Some(s) = &viewer.first_name {
        o.set("first_name", s.as_slice());
    }
    if let Some(s) = &viewer.last_name {
        o.set("last_name", s.as_slice());
    }
    if let Some(s) = &viewer.username {
        o.set("username", s.as_slice());
    }
    if viewer.is_premium {
        o.set("is_premium", true);
    }
    o
}

/// Why a viewer field was left out, for `data-rejected`: "first_name", "last_name", "username", "is_premium".
pub fn viewer_field_rule(field: &[u8]) -> &'static str {
    match field {
        b"first_name" | b"last_name" => "not text of 1\u{2013}64 UTF-16 code units",
        b"username" => "not 5\u{2013}32 characters of A\u{2013}Z, a\u{2013}z, 0\u{2013}9 and _",
        b"is_premium" => "not a boolean",
        _ => "broke its rule",
    }
}

/// Exactly "private", "group", "supergroup" or "channel", else None.
pub fn check_chat_type(chat_type: &[u8]) -> Option<Vec<u8>> {
    if !contains(&CHAT_TYPES, chat_type) {
        return None;
    }
    Some(chat_type.to_vec())
}

/// An answer the glue passes into the widget: the bot's `result` (none for none).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CheckedAnswer {
    pub result: Option<Vec<u8>>,
}

impl CheckedAnswer {
    /// The TS shape: {result} with JSON null for none.
    pub fn to_json(&self) -> Json {
        let mut o = Json::object();
        o.set("result", or_null(&self.result));
        o
    }
}

/// The client's answer: `result` none, or text the glue passes on: well-formed, no NUL, at most 1024 UTF-8
/// bytes (empty allowed). None for anything else (`INVALID_ANSWER`, `answer-rejected`).
pub fn check_request_answer(answer: &WidgetRequestAnswer) -> Option<CheckedAnswer> {
    if let Some(r) = &answer.result
        && (!well_formed(r) || r.contains(&0) || utf8_length(r) > MAX_REQUEST_RESULT_BYTES as usize)
    {
        return None;
    }
    Some(CheckedAnswer { result: answer.result.clone() })
}

/// What the widget's promise rejects with for a client's failure: "TIMEOUT", "RATE_LIMITED", "NOT_ALLOWED",
/// anything else "FAILED".
pub fn request_failure_code(failure: WidgetRequestFailure) -> &'static str {
    match failure {
        WidgetRequestFailure::Timeout => "TIMEOUT",
        WidgetRequestFailure::RateLimited => "RATE_LIMITED",
        WidgetRequestFailure::NotAllowed => "NOT_ALLOWED",
        _ => "FAILED",
    }
}

/// The same for a wallet call: also "REJECTED" and "BAD_REQUEST".
pub fn wallet_failure_code(failure: WidgetRequestFailure) -> &'static str {
    match failure {
        WidgetRequestFailure::Rejected => "REJECTED",
        WidgetRequestFailure::BadRequest => "BAD_REQUEST",
        _ => request_failure_code(failure),
    }
}

/// The verdict on a request: `Foreground`, `Background` and `Granted` go to the client; `Held` waits in the
/// glue for the widget to appear on screen; the rest are refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RequestVerdict {
    Foreground,
    Background,
    Held,
    Granted,
    Disabled,
    NoWallet,
    Busy,
    NoActivation,
    RateLimited,
}

/// The TS verdict: "foreground", "background", "held", "granted", "disabled", "no-wallet", "busy",
/// "no-activation", "rate-limited".
pub fn request_verdict_name(verdict: RequestVerdict) -> &'static str {
    match verdict {
        RequestVerdict::Foreground => "foreground",
        RequestVerdict::Background => "background",
        RequestVerdict::Held => "held",
        RequestVerdict::Granted => "granted",
        RequestVerdict::Disabled => "disabled",
        RequestVerdict::NoWallet => "no-wallet",
        RequestVerdict::Busy => "busy",
        RequestVerdict::NoActivation => "no-activation",
        RequestVerdict::RateLimited => "rate-limited",
    }
}

/// What the widget's promise rejects with for a refused request ("BUSY", "RATE_LIMITED", "NOT_ALLOWED"); None
/// for the verdicts that go to the client.
pub fn request_refusal(verdict: RequestVerdict) -> Option<&'static str> {
    match verdict {
        RequestVerdict::Foreground | RequestVerdict::Background | RequestVerdict::Held | RequestVerdict::Granted => None,
        RequestVerdict::Busy => Some("BUSY"),
        RequestVerdict::RateLimited => Some("RATE_LIMITED"),
        _ => Some("NOT_ALLOWED"),
    }
}

/// What `RequestGate::decide` knows of a request.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RequestConditions {
    pub gesture: bool,
    pub visible: bool,
    pub can_query: bool,
    pub background_query: bool,
    pub with_wallet: bool,
    pub wallet: bool,
}

/// What `RequestGate::decide_wallet` knows of a wallet call.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WalletConditions {
    pub gesture: bool,
    pub visible: bool,
    pub can_query: bool,
    pub wallet: bool,
}

/// Only where the message allows requests; one in flight per widget, a held one included; a press unlocks
/// one; without a press only with `background_query`: one when the widget appears on screen, then at most one
/// per 5 s while it stays visible; one made off screen is held for its next appearance. A wallet call, or a
/// request that asks for the wallet, needs the visible widget and a press, or the grant of the answer to the
/// last call made by a press or a grant, within 10 s. Same order of checks as the TS class.
#[derive(Clone, Debug, Default)]
pub struct RequestGate {
    in_flight: bool,
    held: bool,
    last_background: Option<f64>,
    granting: bool,
    grant_at: Option<f64>,
}

impl RequestGate {
    pub fn new() -> RequestGate {
        RequestGate::default()
    }

    fn out(&mut self, verdict: RequestVerdict) -> RequestVerdict {
        self.in_flight = true;
        self.granting = true;
        self.grant_at = None;
        verdict
    }

    pub fn decide(&mut self, now: f64, s: &RequestConditions) -> RequestVerdict {
        if !s.can_query {
            return RequestVerdict::Disabled;
        }
        if s.with_wallet && !s.wallet {
            return RequestVerdict::NoWallet;
        }
        if self.in_flight {
            return RequestVerdict::Busy;
        }
        if s.gesture && s.visible {
            return self.out(RequestVerdict::Foreground);
        }
        if !s.background_query || s.with_wallet {
            return RequestVerdict::NoActivation;
        }
        if !s.visible {
            self.in_flight = true;
            self.held = true;
            return RequestVerdict::Held;
        }
        if let Some(last) = self.last_background
            && now - last < BACKGROUND_REQUEST_INTERVAL_MS
        {
            return RequestVerdict::RateLimited;
        }
        self.last_background = Some(now);
        self.in_flight = true;
        RequestVerdict::Background
    }

    /// A wallet call (`sendTransaction`, `signData`): on a press, or on the grant within 10 s of its answer.
    pub fn decide_wallet(&mut self, now: f64, s: &WalletConditions) -> RequestVerdict {
        if !s.can_query {
            return RequestVerdict::Disabled;
        }
        if !s.wallet {
            return RequestVerdict::NoWallet;
        }
        if self.in_flight {
            return RequestVerdict::Busy;
        }
        if s.gesture && s.visible {
            return self.out(RequestVerdict::Foreground);
        }
        if s.visible
            && let Some(at) = self.grant_at
            && now - at <= WALLET_GRANT_MS
        {
            return self.out(RequestVerdict::Granted);
        }
        RequestVerdict::NoActivation
    }

    /// The answer or the failure went into the widget, or the glue dropped a held request; `answered_at_ms`
    /// when an answer with no error went in then.
    pub fn finish(&mut self, answered_at_ms: Option<f64>) {
        if self.granting && answered_at_ms.is_some() {
            self.grant_at = answered_at_ms;
        }
        self.in_flight = false;
        self.held = false;
        self.granting = false;
    }

    /// The widget appeared on screen: its next background request may go out at once. True when a held
    /// request goes out now, as this appearance's background request.
    pub fn show(&mut self, now: f64) -> bool {
        self.last_background = None;
        if !self.held {
            return false;
        }
        self.held = false;
        self.last_background = Some(now);
        true
    }

    pub fn busy(&self) -> bool {
        self.in_flight
    }
}

/// What the haptic gate does with a call: deliver it, drop it silently, or drop it and report `haptic-limit`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HapticDecision {
    Play,
    Drop,
    Report,
}

impl HapticDecision {
    pub fn name(self) -> &'static str {
        match self {
            HapticDecision::Play => "play",
            HapticDecision::Drop => "drop",
            HapticDecision::Report => "report",
        }
    }
}

/// `std::min`: `b` when it is less than `a`, else `a`.
fn min(a: f64, b: f64) -> f64 {
    if b < a { b } else { a }
}

/// `std::max`: `b` when `a` is less than it, else `a`.
fn max(a: f64, b: f64) -> f64 {
    if a < b { b } else { a }
}

/// Haptics need a visible widget and room in the bucket (5 calls, refilled at 10 per second), and a touch:
/// while a press is down and for 5 s after it is lifted they are free (under the rate limit); outside that,
/// the first call opens a series whose first second passes, then nothing until the next touch, or 10 s after
/// the series began. `press` / `release` are the host's trusted presses. Same arithmetic, in the same order,
/// as the TS class.
#[derive(Clone, Debug)]
pub struct HapticGate {
    tokens: f64,
    last: Option<f64>,
    reported: Option<f64>,
    down: Option<f64>,
    lifted: Option<f64>,
    series: Option<f64>,
}

impl Default for HapticGate {
    fn default() -> HapticGate {
        HapticGate { tokens: f64::from(HAPTIC_BURST), last: None, reported: None, down: None, lifted: None, series: None }
    }
}

impl HapticGate {
    pub fn new() -> HapticGate {
        HapticGate::default()
    }

    pub fn decide(&mut self, now: f64, visible: bool) -> HapticDecision {
        if let Some(last) = self.last {
            self.tokens = min(f64::from(HAPTIC_BURST), self.tokens + (max(0.0, now - last) * f64::from(HAPTIC_PER_SECOND)) / 1000.0);
        }
        self.last = Some(now);
        if let Some(down) = self.down
            && now - down > HAPTIC_PRESS_MAX_MS
        {
            self.release(down + HAPTIC_PRESS_MAX_MS);
        }
        let touching = self.down.is_some() || self.lifted.is_some_and(|l| now - l <= HAPTIC_TOUCH_WINDOW_MS);
        let mut allowed = visible;
        if allowed && !touching && !self.series.is_some_and(|s| now - s < HAPTIC_SERIES_MS) {
            let touched_since = self.lifted.is_some_and(|l| self.series.is_none_or(|s| l > s));
            if self.series.is_none_or(|s| touched_since || now - s >= HAPTIC_SERIES_GAP_MS) {
                self.series = Some(now);
            } else {
                allowed = false;
            }
        }
        if allowed && self.tokens >= 1.0 {
            self.tokens -= 1.0;
            return HapticDecision::Play;
        }
        if self.reported.is_some_and(|r| now - r < HAPTIC_REPORT_MS) {
            return HapticDecision::Drop;
        }
        self.reported = Some(now);
        HapticDecision::Report
    }

    /// A finger or a mouse button went down on the widget (trusted input only).
    pub fn press(&mut self, now: f64) {
        self.down = Some(now);
    }

    /// The press ended: the finger was lifted or the press was cancelled.
    pub fn release(&mut self, now: f64) {
        self.down = None;
        self.lifted = Some(now);
    }
}

/// The theme keys of `params` whose values are `#rrggbb` (lowercased); everything else is dropped.
pub fn check_theme_params(params: &Json) -> BTreeMap<Vec<u8>, Vec<u8>> {
    let mut out = BTreeMap::new();
    if !params.is_object() {
        return out;
    }
    for key in THEME_KEYS {
        if let Some(v) = member(params, key.as_bytes())
            && v.is_string()
            && is_theme_color(v.as_str())
        {
            out.entry(key.as_bytes().to_vec()).or_insert_with(|| ascii_lower(v.as_str()));
        }
    }
    out
}

/// The same for the theme a client passes as a map.
pub fn check_theme_params_map(params: &BTreeMap<Vec<u8>, Vec<u8>>) -> BTreeMap<Vec<u8>, Vec<u8>> {
    let mut out = BTreeMap::new();
    for key in THEME_KEYS {
        if let Some(v) = params.get(key.as_bytes())
            && is_theme_color(v)
        {
            out.entry(key.as_bytes().to_vec()).or_insert_with(|| ascii_lower(v));
        }
    }
    out
}

/// The checked theme as the widget gets it, in `THEME_KEYS` order.
pub fn theme_params_json(checked: &BTreeMap<Vec<u8>, Vec<u8>>) -> Json {
    let mut o = Json::object();
    for key in THEME_KEYS {
        if let Some(v) = checked.get(key.as_bytes()) {
            o.set(key, v.as_slice());
        }
    }
    o
}

/// A BCP 47 tag of at most 35 characters (`ru`, `en-US`, `pt-BR`, `zh-Hant-TW`), or None.
pub fn check_locale(locale: &[u8]) -> Option<Vec<u8>> {
    if locale.is_empty() || locale.len() > MAX_LOCALE_LENGTH as usize {
        return None;
    }
    let mut i = 0;
    while i < locale.len() && is_alpha(locale[i]) {
        i += 1;
    }
    if !(2..=8).contains(&i) {
        return None;
    }
    while i < locale.len() {
        if locale[i] != b'-' {
            return None;
        }
        i += 1;
        let start = i;
        while i < locale.len() && (is_alpha(locale[i]) || is_digit(locale[i])) {
            i += 1;
        }
        if !(1..=8).contains(&(i - start)) {
            return None;
        }
    }
    Some(locale.to_vec())
}

pub fn check_locale_value(locale: &Json) -> Option<Vec<u8>> {
    if !locale.is_string() {
        return None;
    }
    check_locale(locale.as_str())
}

/// UTF-8 bytes of an area's keys and values.
pub fn storage_bytes(area: &StorageArea) -> usize {
    area.iter().map(|(k, v)| utf8_length(k) + utf8_length(v)).sum()
}

/// The members of an object as JSON.parse keeps them: a key given twice with its last value, in key order.
fn last_members(object: &Json) -> BTreeMap<&[u8], &Json> {
    let mut members = BTreeMap::new();
    for (k, v) in object.obj() {
        members.insert(k.as_slice(), v);
    }
    members
}

/// An object of strings within the quota, or None. A key given twice counts once, with its last value.
pub fn check_storage_area(area: &Json) -> Option<StorageArea> {
    if !area.is_object() {
        return None;
    }
    let mut out = StorageArea::new();
    for (k, v) in last_members(area) {
        if !v.is_string() {
            return None;
        }
        out.insert(k.to_vec(), v.as_str().to_vec());
    }
    if storage_bytes(&out) > STORAGE_QUOTA_BYTES as usize {
        return None;
    }
    Some(out)
}

/// The same for an area the client passed (only the quota can be wrong).
pub fn storage_area_fits(area: &StorageArea) -> bool {
    storage_bytes(area) <= STORAGE_QUOTA_BYTES as usize
}

/// A commit (or a preload) of `local` and/or `session`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct StorageCommit {
    pub local: Option<StorageArea>,
    pub session: Option<StorageArea>,
}

fn area_json(area: &StorageArea) -> Json {
    Json::Object(area.iter().map(|(k, v)| (k.clone(), Json::String(v.clone()))).collect())
}

impl StorageCommit {
    pub fn to_json(&self) -> Json {
        let mut o = Json::object();
        if let Some(local) = &self.local {
            o.set("local", area_json(local));
        }
        if let Some(session) = &self.session {
            o.set("session", area_json(session));
        }
        o
    }
}

/// A commit of `local` and/or `session`, each checked whole; None when anything is wrong (`storage-rejected`).
pub fn check_storage_commit(commit: &Json) -> Option<StorageCommit> {
    if !commit.is_object() {
        return None;
    }
    let mut out = StorageCommit::default();
    for (k, v) in last_members(commit) {
        if k != b"local" && k != b"session" {
            return None;
        }
        let area = check_storage_area(v)?;
        if k == b"local" {
            out.local = Some(area);
        } else {
            out.session = Some(area);
        }
    }
    Some(out)
}

/// `{"local": {…}, "session": {…}}` of a client preload, or `{}` and false when either area is over the quota.
pub fn storage_preload_json(storage: &WidgetStorage) -> (Json, bool) {
    let ok = storage_area_fits(&storage.local) && storage_area_fits(&storage.session);
    if !ok {
        return (Json::object(), false);
    }
    let mut o = Json::object();
    o.set("local", area_json(&storage.local)).set("session", area_json(&storage.session));
    (o, true)
}
