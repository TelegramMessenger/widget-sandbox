//! `sandbox_engine.h`'s value types: what the host gives an engine (its configuration, the chat's theme, the
//! band the client draws over the widget, the device's edges), the geometry of each place, and what the engine
//! reports back (its counters, a query's answer).

use std::collections::BTreeMap;

use twidget_json::Json;
use twidget_sdk_policy::{DEFAULT_LOCALE, WidgetContext, WidgetData, WidgetStorage};

/// What the client draws over the widget, in CSS px (docs/sdk-v1.md §2.17, §5.12): the band of the message
/// time and status over a widget with no caption below it, and the corner the time takes in it. Passed as
/// given: the renderer core and the shim keep an inset of 0–64 and a corner of 0–200 in a bubble, and on a
/// profile page any finite inset ≥ 0 with the corners 0; anything else is 0.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct WidgetContentSafeArea {
    pub top: f64,
    pub right: f64,
    pub bottom: f64,
    pub left: f64,
    pub corner_left: f64,
    pub corner_right: f64,
}

impl WidgetContentSafeArea {
    /// The TS `ContentSafeArea` shape; a value that is not finite is 0 (JSON has no NaN).
    pub fn json(&self) -> Json {
        let mut out = Json::object();
        for (name, v) in [
            ("top", self.top),
            ("right", self.right),
            ("bottom", self.bottom),
            ("left", self.left),
            ("cornerLeft", self.corner_left),
            ("cornerRight", self.corner_right),
        ] {
            out.set(name, if v.is_finite() { v } else { 0.0 });
        }
        out
    }
}

/// The device's edges over the widget, in CSS px (docs/sdk-v1.md §2.17): on a profile page the status bar and the
/// cutout (`top`), the home indicator or the navigation bar (`bottom`), the sides in landscape. The widget sees it as
/// `env(safe-area-inset-*)` (the sum with `WidgetContentSafeArea`), the `--tg-safe-area-inset-*` variables and
/// `TelegramWidget.safeAreaInset`. Passed as given: the renderer core and the shim keep any finite value ≥ 0 on a page
/// (NaN, infinite and negative are 0), and everything is 0 in a bubble.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct WidgetSafeArea {
    pub top: f64,
    pub right: f64,
    pub bottom: f64,
    pub left: f64,
}

impl WidgetSafeArea {
    /// The TS `SafeAreaInset` shape; a value that is not finite is 0 (JSON has no NaN).
    pub fn json(&self) -> Json {
        let mut out = Json::object();
        for (name, v) in [("top", self.top), ("right", self.right), ("bottom", self.bottom), ("left", self.left)] {
            out.set(name, if v.is_finite() { v } else { 0.0 });
        }
        out
    }
}

/// The geometry of each place, profile.json's `tables.surfaces` (checked by `profile-surface.test.ts`): a bubble is
/// 50–600 high and its resize anchor moves it, a page is at least its viewport and at most `PAGE_MAX_HEIGHT` and never
/// moves.
pub mod surface {
    use twidget_sdk_policy::WidgetSurface;

    /// The least height of either place, in CSS px.
    pub const MIN_HEIGHT: f64 = 50.0;
    /// A bubble's greatest height, in CSS px.
    pub const MESSAGE_MAX_HEIGHT: f64 = 600.0;
    /// A profile page's greatest height, in CSS px. Provisional: the page ceiling waits for the clients' measurements.
    pub const PAGE_MAX_HEIGHT: f64 = 4000.0;

    /// Whether the widget's `setResizeAnchor` moves anything here: a bubble keeps the anchored edge, a page never moves.
    pub fn keeps_resize_anchor(s: WidgetSurface) -> bool {
        s == WidgetSurface::Message
    }

    /// A profile page's viewport as the glue passes it on: finite, above 0 and at most the page ceiling (checked
    /// before the rounding), two decimals; anything else, and every bubble, is 0 (no `--tg-viewport-height`).
    pub fn check_viewport_height(s: WidgetSurface, height: f64) -> f64 {
        if s != WidgetSurface::Profile || !height.is_finite() || height <= 0.0 || height > PAGE_MAX_HEIGHT {
            return 0.0;
        }
        (height * 100.0).round() / 100.0
    }

    /// The height the client lays the widget out at: a bubble's content in [50, 600], a page's at least its viewport
    /// (and 50), at most the page ceiling; a content height that is not finite counts as 0.
    pub fn height(s: WidgetSurface, content: f64, viewport: f64) -> f64 {
        let h = if content.is_finite() { content } else { 0.0 };
        if s == WidgetSurface::Message {
            return min(max(h, MIN_HEIGHT), MESSAGE_MAX_HEIGHT);
        }
        min(max(h, max(MIN_HEIGHT, check_viewport_height(s, viewport))), PAGE_MAX_HEIGHT)
    }

    /// `std::max` as the C++ compares: `(a < b) ? b : a`.
    fn max(a: f64, b: f64) -> f64 {
        if a < b { b } else { a }
    }

    /// `std::min` as the C++ compares: `(b < a) ? b : a`.
    fn min(a: f64, b: f64) -> f64 {
        if b < a { b } else { a }
    }
}

/// A theme token's member: text or a number (`ThemeToken` in sandbox_engine_shared.cpp).
enum Token<'a> {
    Text(&'a mut Option<Vec<u8>>),
    Number(&'a mut Option<f64>),
}

fn tokens_json(tokens: Vec<(&str, Token)>) -> Json {
    let mut out = Json::object();
    for (key, token) in tokens {
        match token {
            Token::Text(v) => {
                if let Some(v) = v {
                    out.set(key, v.as_slice());
                }
            }
            Token::Number(v) => {
                if let Some(v) = *v
                    && v.is_finite()
                {
                    out.set(key, v);
                }
            }
        }
    }
    out
}

fn tokens_from(object: &Json, tokens: Vec<(&str, Token)>) {
    if !object.is_object() {
        return;
    }
    for (key, token) in tokens {
        let v = object.get(key);
        match token {
            Token::Text(t) => {
                if v.is_string() {
                    *t = Some(v.as_str().to_vec());
                }
            }
            Token::Number(n) => {
                if v.is_number() && v.as_number(0.0).is_finite() {
                    *n = Some(v.as_number(0.0));
                }
            }
        }
    }
}

/// The client's own `<button>` (docs/sdk-v1.md §2.4); every field optional, the template's defaults stand
/// for the rest. Passed to the renderer as given: the renderer core keeps only values that match their grammar.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct WidgetButtonTokens {
    pub bg: Option<Vec<u8>>,
    pub text: Option<Vec<u8>>,
    pub radius: Option<Vec<u8>>,
    pub font: Option<Vec<u8>>,
    pub padding: Option<Vec<u8>>,
    pub height: Option<Vec<u8>>,
    pub pressed_opacity: Option<f64>,
    pub disabled_opacity: Option<f64>,
}

impl WidgetButtonTokens {
    /// The tokens in the order the renderer's theme message and the wire write them.
    fn tokens(&mut self) -> Vec<(&'static str, Token<'_>)> {
        vec![
            ("bg", Token::Text(&mut self.bg)),
            ("text", Token::Text(&mut self.text)),
            ("radius", Token::Text(&mut self.radius)),
            ("font", Token::Text(&mut self.font)),
            ("padding", Token::Text(&mut self.padding)),
            ("height", Token::Text(&mut self.height)),
            ("pressedOpacity", Token::Number(&mut self.pressed_opacity)),
            ("disabledOpacity", Token::Number(&mut self.disabled_opacity)),
        ]
    }

    /// The `button` object of the renderer's theme message and of the wire's theme (docs/sdk-v1.md §3.4): each
    /// token that is set, in this order, a number only when finite.
    pub fn json(&self) -> Json {
        tokens_json(self.clone().tokens())
    }

    /// Such an object read back: a token of its type (a finite number) is set, anything else unset; not an
    /// object, none.
    pub fn from(object: &Json) -> WidgetButtonTokens {
        let mut out = WidgetButtonTokens::default();
        tokens_from(object, out.tokens());
        out
    }
}

/// The client's message text (docs/sdk-v1.md §2.4): the root's font size, its unitless line height, the padding.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct WidgetTextTokens {
    pub font_size: Option<f64>,
    pub line_height: Option<f64>,
    pub padding: Option<Vec<u8>>,
}

impl WidgetTextTokens {
    fn tokens(&mut self) -> Vec<(&'static str, Token<'_>)> {
        vec![
            ("fontSize", Token::Number(&mut self.font_size)),
            ("lineHeight", Token::Number(&mut self.line_height)),
            ("padding", Token::Text(&mut self.padding)),
        ]
    }

    /// The `text` object, as `WidgetButtonTokens::json` writes the button's.
    pub fn json(&self) -> Json {
        tokens_json(self.clone().tokens())
    }

    /// Read back as `WidgetButtonTokens::from` reads the button's.
    pub fn from(object: &Json) -> WidgetTextTokens {
        let mut out = WidgetTextTokens::default();
        tokens_from(object, out.tokens());
        out
    }
}

/// The chat's theme for the bubble the widget sits in (docs/sdk-v1.md §2.3, §2.4, §4.3).
#[derive(Clone, Debug, PartialEq)]
pub struct WidgetTheme {
    pub color_scheme: Vec<u8>,
    pub params: BTreeMap<Vec<u8>, Vec<u8>>,
    pub button: WidgetButtonTokens,
    pub text: WidgetTextTokens,
}

impl Default for WidgetTheme {
    fn default() -> Self {
        WidgetTheme { color_scheme: b"light".to_vec(), params: BTreeMap::new(), button: Default::default(), text: Default::default() }
    }
}

/// What a hosted widget's start carries beyond its `SandboxConfig` (remote_hosted.h).
#[derive(Clone, Debug, PartialEq)]
pub struct SandboxHostedStart {
    pub theme: WidgetTheme,
    pub stopped_label: Vec<u8>,
    pub validated_ops: bool,
}

impl Default for SandboxHostedStart {
    fn default() -> Self {
        SandboxHostedStart { theme: WidgetTheme::default(), stopped_label: b"Widget stopped".to_vec(), validated_ops: false }
    }
}

/// Mirrors `SandboxConfig` / `LIMIT_DEFAULTS` in src/shared/protocol.ts (docs/decisions.md, "Strict limits from
/// the start").
#[derive(Clone, Debug, PartialEq)]
pub struct SandboxConfig {
    pub memory_limit_bytes: usize,
    /// Longest continuous run of widget JS in one task before the runtime is killed.
    pub cpu_limit_ms: f64,
    /// JS time the widget may use per `cpu_quota_window_ms` (sliding window); beyond it timers and frames are
    /// deferred, never killed.
    pub cpu_quota_ms: f64,
    pub cpu_quota_window_ms: f64,
    /// Kill after this long of uninterrupted throttling; 0 disables (default).
    pub cpu_kill_after_ms: f64,
    /// The host's main-thread work for the widget per `cpu_quota_window_ms`, apart from the JS quota.
    pub main_thread_quota_ms: f64,
    /// Live timers a widget may hold; the shim throws RangeError beyond it, the engine refuses as a backstop.
    pub max_timers: i32,
    /// Initial visibility: hidden widgets get no frames and timers at 1 Hz.
    pub visible: bool,
    pub inner_width: f64,
    pub inner_height: f64,
    pub device_pixel_ratio: f64,
    pub theme: Vec<u8>,
    /// The chat's palette (docs/sdk-v1.md §2.3); the engine passes only the 15 theme keys with `#rrggbb` values.
    pub theme_params: BTreeMap<Vec<u8>, Vec<u8>>,
    /// The client's UI language, BCP 47; `en-US` when malformed.
    pub locale: Vec<u8>,
    /// What the client draws over the widget at boot; later changes go through `content_safe_area`.
    pub content_safe_area: WidgetContentSafeArea,
    /// The device's edges over the widget at boot (docs/sdk-v1.md §2.17), 0 in a bubble whatever is passed; later
    /// changes go through `safe_area`.
    pub safe_area: WidgetSafeArea,
    /// Web Storage of this message as the client keeps it.
    pub storage: WidgetStorage,
    /// The bot's `chatData` and `publicData` at start.
    pub data: WidgetData,
    /// The message's flags from the server, the chat type and the viewer.
    pub context: WidgetContext,
    /// "strict" (default) or "compat"; anything but exactly "compat" is strict.
    pub policy: Vec<u8>,
    /// Opt-in sandbox features of docs/protocol.md; this runtime honours webAnimations, noInPlaceText,
    /// noAttrFast and noCanvasRect and drops the rest.
    pub features: Vec<Vec<u8>>,
}

impl Default for SandboxConfig {
    fn default() -> Self {
        SandboxConfig {
            memory_limit_bytes: 8 << 20,
            cpu_limit_ms: 500.0,
            cpu_quota_ms: 300.0,
            cpu_quota_window_ms: 1000.0,
            cpu_kill_after_ms: 0.0,
            main_thread_quota_ms: 100.0,
            max_timers: 64,
            visible: true,
            inner_width: 400.0,
            inner_height: 300.0,
            device_pixel_ratio: 1.0,
            theme: b"light".to_vec(),
            theme_params: BTreeMap::new(),
            locale: DEFAULT_LOCALE.as_bytes().to_vec(),
            content_safe_area: WidgetContentSafeArea::default(),
            safe_area: WidgetSafeArea::default(),
            storage: WidgetStorage::default(),
            data: WidgetData::default(),
            context: WidgetContext::default(),
            policy: b"strict".to_vec(),
            features: Vec::new(),
        }
    }
}

/// Fail-closed policy parsing, the twin of resolvePolicy in src/shared/protocol.ts.
pub fn is_strict_policy(policy: &[u8]) -> bool {
    policy != b"compat"
}

/// The engine's counters as it reports them.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SandboxStats {
    pub js_ms: f64,
    pub tasks: i32,
    pub memory_bytes: usize,
    pub ops: i32,
    pub queries: i32,
    pub query_ms: f64,
    pub query_cache_hits: i32,
    pub missed_mutations: i32,
    /// Tasks deferred by the CPU quota or the main-thread window.
    pub throttled_tasks: i32,
    /// JS time used in the current quota window at the time of the report.
    pub cpu_window_ms: f64,
    /// The validator's CPU time on this widget's messages, charged to the JS quota next to `js_ms`.
    pub validator_ms: f64,
}

/// A layout query's answer.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct QueryAnswer {
    pub json: Vec<u8>,
    pub binary: Option<Vec<u8>>,
    /// The part of the answer's time a served host's glue spent inside its renderer's platform requests: not
    /// the widget's time. 0 elsewhere.
    pub platform_seconds: f64,
}
