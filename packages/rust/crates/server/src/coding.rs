//! `SandboxRemoteCoding` (`remote_engine.h`, `remote_coding.cpp`): the wire forms both processes write and
//! read, their numbers bounded and their text parsed as the engine has it in process. JSON is written by
//! `Json::dump` (bytes from 0x20 up raw).

use std::collections::BTreeMap;

use twidget_engine::{
    SandboxConfig, SandboxHostedStart, SandboxStats, WidgetButtonTokens, WidgetContentSafeArea, WidgetTextTokens, WidgetTheme,
};
use twidget_json::{Json, Surrogates};
use twidget_sdk_policy as sdk;
use twidget_sdk_policy::{
    StorageCommit, WalletCall, WalletMethod, WidgetContext, WidgetData, WidgetRequestFailure, WidgetRequestOutcome, WidgetStorage,
    WidgetViewer,
};
use twidget_stream::{REMOTE_MESSAGE_LIMIT, REMOTE_TEXT_LIMIT, STATS_FIELDS};

/// How the host settled a request or a wallet call the engine's side forwarded: the bot's answer, a failure
/// (never `None`), or `Unsupported` (the host handles none).
#[derive(Clone, Debug, Default)]
pub enum SandboxRemoteOutcome {
    /// The bot's result, none for none.
    Answer(Option<Vec<u8>>),
    Failure(WidgetRequestFailure),
    #[default]
    Unsupported,
}

impl SandboxRemoteOutcome {
    /// A responder's outcome: none is `Unsupported`, a failure other than `None` is `Failure`, else the answer's
    /// result.
    pub fn from(outcome: Option<&WidgetRequestOutcome>) -> SandboxRemoteOutcome {
        match outcome {
            None => SandboxRemoteOutcome::Unsupported,
            Some(o) if o.failure != WidgetRequestFailure::None => SandboxRemoteOutcome::Failure(o.failure),
            Some(o) => SandboxRemoteOutcome::Answer(o.answer.result.clone()),
        }
    }
}

impl PartialEq for SandboxRemoteOutcome {
    fn eq(&self, o: &SandboxRemoteOutcome) -> bool {
        match (self, o) {
            (SandboxRemoteOutcome::Answer(a), SandboxRemoteOutcome::Answer(b)) => a == b,
            (SandboxRemoteOutcome::Failure(a), SandboxRemoteOutcome::Failure(b)) => a == b,
            (SandboxRemoteOutcome::Unsupported, SandboxRemoteOutcome::Unsupported) => true,
            _ => false,
        }
    }
}

/// Nesting a decoder accepts (the deepest form, a wallet call's message, has 4).
pub const MAX_DEPTH: i32 = 8;

/// 2^N for an N-bit integer type, the first number past its range.
const INT_END: f64 = 2147483648.0;
const INT64_END: f64 = 9223372036854775808.0;
/// A JSON number is a double: the wire's integers are exact up to 2^53.
const EXACT_INTEGERS: f64 = 9007199254740992.0;
const STATS_MAX: f64 = 1e15;

/// A number truncated into a type that cannot be negative here: 0 below 0, the type's largest value past its range.
fn truncated_int(v: f64) -> i32 {
    if !(v > 0.0) {
        0
    } else if v >= INT_END {
        i32::MAX
    } else {
        v as i32
    }
}

fn truncated_size(v: f64) -> usize {
    if !(v > 0.0) { 0 } else { v as usize }
}

/// Untrusted text: refused over the message limit before it is parsed; an escaped lone surrogate stays WTF-8,
/// the bytes the engine has in process.
fn parse(text: &[u8]) -> Option<Json> {
    if text.len() > REMOTE_MESSAGE_LIMIT {
        return None;
    }
    Json::parse_with(text, Surrogates::Keep, MAX_DEPTH)
}

fn parse_object(text: &[u8]) -> Option<Json> {
    parse(text).filter(Json::is_object)
}

/// A member that is a finite number; with a repeated key the first.
fn finite_number(object: &Json, key: &str) -> Option<f64> {
    let v = object.find(key)?;
    let n = v.as_number(0.0);
    (v.is_number() && n.is_finite()).then_some(n)
}

fn string_of(v: &Json) -> Option<Vec<u8>> {
    v.is_string().then(|| v.as_str().to_vec())
}

fn or_null(s: &Option<Vec<u8>>) -> Json {
    s.clone().map_or(Json::Null, Json::String)
}

fn string_list(v: &Json) -> Option<Vec<Vec<u8>>> {
    if !v.is_array() {
        return None;
    }
    v.arr().iter().map(string_of).collect()
}

/// An object of strings, or none; a repeated key counts with its last value.
fn string_map(v: &Json) -> Option<BTreeMap<Vec<u8>, Vec<u8>>> {
    if !v.is_object() {
        return None;
    }
    let mut last: BTreeMap<&[u8], &Json> = BTreeMap::new();
    for (k, s) in v.obj() {
        last.insert(k, s);
    }
    let mut out = BTreeMap::new();
    for (k, s) in last {
        out.insert(k.to_vec(), string_of(s)?);
    }
    Some(out)
}

fn string_map_json(values: &BTreeMap<Vec<u8>, Vec<u8>>) -> Json {
    let mut o = Json::object();
    for (k, v) in values {
        o.set(k.clone(), v.as_slice());
    }
    o
}

fn data_json(data: &WidgetData) -> Json {
    let mut o = Json::object();
    o.set("chatData", or_null(&data.chat_data)).set("publicData", or_null(&data.public_data));
    o
}

fn data_of(o: &Json) -> WidgetData {
    WidgetData { chat_data: string_of(o.get("chatData")), public_data: string_of(o.get("publicData")) }
}

fn safe_area_of(o: &Json) -> WidgetContentSafeArea {
    let side = |key| finite_number(o, key).unwrap_or(0.0);
    WidgetContentSafeArea {
        top: side("top"),
        right: side("right"),
        bottom: side("bottom"),
        left: side("left"),
        corner_left: side("cornerLeft"),
        corner_right: side("cornerRight"),
    }
}

/// A finite number inside `i64`'s range, truncated toward zero; the range is checked exactly, [-2^63, 2^63).
fn int64_of(v: &Json) -> Option<i64> {
    let n = v.as_number(0.0);
    if !v.is_number() || !n.is_finite() || !(-INT64_END..INT64_END).contains(&n) {
        return None;
    }
    Some(n as i64)
}

fn context_json(c: &WidgetContext) -> Json {
    let mut o = Json::object();
    o.set("isCopy", c.is_copy)
        .set("canQuery", c.can_query)
        .set("backgroundQuery", c.background_query)
        .set("preview", c.preview)
        .set("wallet", c.wallet);
    o.set("chatType", c.chat_type.as_slice());
    if let Some(viewer) = &c.viewer {
        o.set("viewer", sdk::viewer_json(viewer));
    }
    o
}

/// A fresh context, as Swift's decoder builds one: each flag a bool or false, the chat type a string or none,
/// a viewer only with a usable `id`.
fn context_of(o: &Json) -> WidgetContext {
    let mut c = WidgetContext {
        is_copy: o.get("isCopy").as_bool(false),
        can_query: o.get("canQuery").as_bool(false),
        background_query: o.get("backgroundQuery").as_bool(false),
        preview: o.get("preview").as_bool(false),
        wallet: o.get("wallet").as_bool(false),
        chat_type: string_of(o.get("chatType")).unwrap_or_default(),
        viewer: None,
    };
    let viewer = o.get("viewer");
    if viewer.is_object()
        && let Some(id) = int64_of(viewer.get("id"))
    {
        c.viewer = Some(WidgetViewer {
            id,
            first_name: string_of(viewer.get("first_name")),
            last_name: string_of(viewer.get("last_name")),
            username: string_of(viewer.get("username")),
            is_premium: viewer.get("is_premium").as_bool(false),
        });
    }
    c
}

/// `encode_theme`'s object: the scheme and the palette always, the tokens as the renderer's theme message has them.
fn theme_json(t: &WidgetTheme) -> Json {
    let mut o = Json::object();
    o.set("colorScheme", t.color_scheme.as_slice())
        .set("params", string_map_json(&t.params))
        .set("button", t.button.json())
        .set("text", t.text.json());
    o
}

fn theme_of(o: &Json) -> Option<WidgetTheme> {
    let scheme = if o.is_object() { string_of(o.get("colorScheme")) } else { None }?;
    Some(WidgetTheme {
        color_scheme: scheme,
        params: string_map(o.get("params")).unwrap_or_default(),
        button: WidgetButtonTokens::from(o.get("button")),
        text: WidgetTextTokens::from(o.get("text")),
    })
}

/// Every `SandboxConfig` field as one JSON object (a non-finite number is written as null).
pub fn encode_config(c: &SandboxConfig) -> Vec<u8> {
    let mut features = Json::array();
    for feature in &c.features {
        features.push(feature.as_slice());
    }
    let mut o = Json::object();
    o.set("memoryLimitBytes", c.memory_limit_bytes as f64)
        .set("cpuLimitMs", c.cpu_limit_ms)
        .set("cpuQuotaMs", c.cpu_quota_ms)
        .set("cpuQuotaWindowMs", c.cpu_quota_window_ms);
    o.set("cpuKillAfterMs", c.cpu_kill_after_ms)
        .set("mainThreadQuotaMs", c.main_thread_quota_ms)
        .set("maxTimers", c.max_timers)
        .set("visible", c.visible);
    o.set("innerWidth", c.inner_width)
        .set("innerHeight", c.inner_height)
        .set("devicePixelRatio", c.device_pixel_ratio)
        .set("theme", c.theme.as_slice());
    o.set("themeParams", string_map_json(&c.theme_params))
        .set("locale", c.locale.as_slice())
        .set("contentSafeArea", c.content_safe_area.json());
    o.set("storage", StorageCommit { local: Some(c.storage.local.clone()), session: Some(c.storage.session.clone()) }.to_json())
        .set("data", data_json(&c.data))
        .set("context", context_json(&c.context));
    o.set("policy", c.policy.as_slice()).set("features", features);
    o.dump()
}

/// None without finite numbers `innerWidth`, `innerHeight`, `devicePixelRatio` and a string `theme`; any other
/// member absent or of another type keeps its default.
pub fn decode_config(text: &[u8]) -> Option<SandboxConfig> {
    let o = parse_object(text)?;
    let width = finite_number(&o, "innerWidth");
    let height = finite_number(&o, "innerHeight");
    let dpr = finite_number(&o, "devicePixelRatio");
    let theme = string_of(o.get("theme"));
    let (Some(width), Some(height), Some(dpr), Some(theme)) = (width, height, dpr, theme) else { return None };
    let mut c = SandboxConfig { inner_width: width, inner_height: height, device_pixel_ratio: dpr, theme, ..SandboxConfig::default() };
    // The rest is optional as in Swift's decodeConfig: absent, of another type or a non-finite number keeps the default.
    if let Some(v) = finite_number(&o, "memoryLimitBytes") {
        c.memory_limit_bytes = truncated_size(v.min(EXACT_INTEGERS));
    }
    for (key, field) in [
        ("cpuLimitMs", &mut c.cpu_limit_ms),
        ("cpuQuotaMs", &mut c.cpu_quota_ms),
        ("cpuQuotaWindowMs", &mut c.cpu_quota_window_ms),
        ("cpuKillAfterMs", &mut c.cpu_kill_after_ms),
        ("mainThreadQuotaMs", &mut c.main_thread_quota_ms),
    ] {
        if let Some(v) = finite_number(&o, key) {
            *field = v;
        }
    }
    c.max_timers = o.get("maxTimers").as_int(c.max_timers);
    if o.get("visible").is_bool() {
        c.visible = o.get("visible").as_bool(false);
    }
    if let Some(v) = string_map(o.get("themeParams")) {
        c.theme_params = v;
    }
    if let Some(v) = string_of(o.get("locale")) {
        c.locale = v;
    }
    if o.get("contentSafeArea").is_object() {
        c.content_safe_area = safe_area_of(o.get("contentSafeArea"));
    }
    // Each area an object of strings or empty; the quota stays the receiving engine's check.
    let storage = o.get("storage");
    if storage.is_object() {
        c.storage = WidgetStorage {
            local: string_map(storage.get("local")).unwrap_or_default(),
            session: string_map(storage.get("session")).unwrap_or_default(),
        };
    }
    if o.get("data").is_object() {
        c.data = data_of(o.get("data"));
    }
    if o.get("context").is_object() {
        c.context = context_of(o.get("context"));
    }
    if let Some(v) = string_of(o.get("policy")) {
        c.policy = v;
    }
    if let Some(v) = string_list(o.get("features")) {
        c.features = v;
    }
    Some(c)
}

pub fn encode_safe_area(area: &WidgetContentSafeArea) -> Vec<u8> {
    area.json().dump()
}

/// An object, or none; each side a finite number, else 0.
pub fn decode_safe_area(text: &[u8]) -> Option<WidgetContentSafeArea> {
    parse_object(text).map(|o| safe_area_of(&o))
}

/// `{"chatData": text | null, "publicData": text | null}`.
pub fn encode_data(data: &WidgetData) -> Vec<u8> {
    data_json(data).dump()
}

/// An object, or none; a member that is not a string is none.
pub fn decode_data(text: &[u8]) -> Option<WidgetData> {
    parse_object(text).map(|o| data_of(&o))
}

/// A theme's palette as one object of strings, as `encode_config` writes its `themeParams`.
pub fn encode_theme_params(params: &BTreeMap<Vec<u8>, Vec<u8>>) -> Vec<u8> {
    string_map_json(params).dump()
}

/// An object of strings, or none; a repeated key counts with its last value.
pub fn decode_theme_params(text: &[u8]) -> Option<BTreeMap<Vec<u8>, Vec<u8>>> {
    string_map(&parse_object(text)?)
}

/// A hosted widget's whole theme.
pub fn encode_theme(theme: &WidgetTheme) -> Vec<u8> {
    theme_json(theme).dump()
}

/// An object with a string `colorScheme`, or none.
pub fn decode_theme(text: &[u8]) -> Option<WidgetTheme> {
    theme_of(&parse_object(text)?)
}

/// A hosted start: `{"theme": encode_theme's object, "stoppedLabel": text, "validatedOps": bool}`.
pub fn encode_hosted_start(start: &SandboxHostedStart) -> Vec<u8> {
    let mut o = Json::object();
    o.set("theme", theme_json(&start.theme)).set("stoppedLabel", start.stopped_label.as_slice()).set("validatedOps", start.validated_ops);
    o.dump()
}

/// An object whose `theme` decodes, or none; `stoppedLabel` a string cut to the text limit, else the default.
pub fn decode_hosted_start(text: &[u8]) -> Option<SandboxHostedStart> {
    let o = parse_object(text)?;
    let theme = theme_of(o.get("theme"))?;
    let mut start = SandboxHostedStart { theme, ..SandboxHostedStart::default() };
    if let Some(label) = string_of(o.get("stoppedLabel")) {
        start.stopped_label = label[..label.len().min(REMOTE_TEXT_LIMIT)].to_vec();
    }
    start.validated_ops = o.get("validatedOps").as_bool(false);
    Some(start)
}

/// `{"answer": text | null}`, `{"failure": code}` or `{"unsupported": true}`.
pub fn encode_outcome(outcome: &SandboxRemoteOutcome) -> Vec<u8> {
    let mut o = Json::object();
    match outcome {
        SandboxRemoteOutcome::Answer(result) => o.set("answer", or_null(result)),
        SandboxRemoteOutcome::Failure(failure) => o.set("failure", sdk::wallet_failure_code(*failure)),
        SandboxRemoteOutcome::Unsupported => o.set("unsupported", true),
    };
    o.dump()
}

/// An object, or none: any `unsupported` member first, then a string `failure` (an unknown code is
/// `NotAllowed`), then any `answer` (a result only when a string).
pub fn decode_outcome(text: &[u8]) -> Option<SandboxRemoteOutcome> {
    let o = parse_object(text)?;
    if o.find("unsupported").is_some() {
        return Some(SandboxRemoteOutcome::Unsupported);
    }
    if let Some(failure) = o.find("failure").filter(|f| f.is_string()) {
        for known in [
            WidgetRequestFailure::Timeout,
            WidgetRequestFailure::RateLimited,
            WidgetRequestFailure::Failed,
            WidgetRequestFailure::Rejected,
            WidgetRequestFailure::BadRequest,
        ] {
            if sdk::wallet_failure_code(known).as_bytes() == failure.as_str() {
                return Some(SandboxRemoteOutcome::Failure(known));
            }
        }
        return Some(SandboxRemoteOutcome::Failure(WidgetRequestFailure::NotAllowed));
    }
    o.find("answer").map(|answer| SandboxRemoteOutcome::Answer(string_of(answer)))
}

/// `[1, method, transaction | payload, data]`: the arguments of a `wallet` bridge call; the real id travels apart.
pub fn encode_wallet(call: &WalletCall) -> Vec<u8> {
    let mut args = Json::array();
    args.push(1).push(call.method_name());
    args.push(if call.method == WalletMethod::SignData { call.payload.to_json() } else { call.transaction.to_json() });
    args.push(call.data.as_slice());
    args.dump()
}

/// The call as `check_bridge_call("wallet", …)` checks it, or none.
pub fn decode_wallet(text: &[u8]) -> Option<WalletCall> {
    let args = parse(text)?;
    sdk::check_bridge_call(&Json::from("wallet"), &args)?.wallet.map(|w| w.call)
}

/// `[jsMs, tasks, memoryBytes, ops, queries, queryMs, queryCacheHits, missedMutations, throttledTasks,
/// cpuWindowMs, validatorMs]`.
pub fn encode_stats(s: &SandboxStats) -> [f64; STATS_FIELDS] {
    [
        s.js_ms,
        f64::from(s.tasks),
        s.memory_bytes as f64,
        f64::from(s.ops),
        f64::from(s.queries),
        s.query_ms,
        f64::from(s.query_cache_hits),
        f64::from(s.missed_mutations),
        f64::from(s.throttled_tasks),
        s.cpu_window_ms,
        s.validator_ms,
    ]
}

/// Exactly 11 numbers, or none; a non-finite one is 0, a finite one is kept within [0, 1e15] and then within its
/// field's type.
pub fn decode_stats(values: &[f64]) -> Option<SandboxStats> {
    if values.len() != STATS_FIELDS {
        return None;
    }
    let v: Vec<f64> = values.iter().map(|&x| if x.is_finite() && x > 0.0 { x.min(STATS_MAX) } else { 0.0 }).collect();
    Some(SandboxStats {
        js_ms: v[0],
        tasks: truncated_int(v[1]),
        memory_bytes: truncated_size(v[2]),
        ops: truncated_int(v[3]),
        queries: truncated_int(v[4]),
        query_ms: v[5],
        query_cache_hits: truncated_int(v[6]),
        missed_mutations: truncated_int(v[7]),
        throttled_tasks: truncated_int(v[8]),
        cpu_window_ms: v[9],
        validator_ms: v[10],
    })
}
