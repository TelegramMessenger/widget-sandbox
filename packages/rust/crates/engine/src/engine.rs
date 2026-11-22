//! `sandbox_engine.cpp` and the in-process half of `sandbox_engine_shared.cpp`: one Nova runtime per widget on
//! its own thread (`widget.sandbox`), exactly one capability for the untrusted code (`__host(kind, arg)` and
//! `__hostBinary()`), the CPU and memory limits, the timers, frames and scripts.
//!
//! Everything that comes out of the context is untrusted: the delegate passes it through the validator before
//! it reaches a renderer. SDK v1 calls are checked here with `twidget_sdk_policy` before the delegate sees them.
//!
//! The owner's handle ([`SandboxEngine`]) only queues work; the state lives on the engine's thread
//! ([`Engine`]), in cells borrowed for no longer than a step: never across a call into the widget's code or
//! into the delegate, which may call the engine back from inside (`Engine::stop` from `on_ops`).

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::rc::{Rc, Weak};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, RwLock};

use twidget_bundle_stamp::{BundleStamp, SandboxResources};
use twidget_cpu_budget::CpuBudget;
use twidget_js::{Deadlines, HostReturn, JS_STACK_LIMIT, JsError, JsThread, NovaContext, Platform, Poster, Scheduled};
use twidget_json::{Json, Surrogates};
use twidget_sdk_policy as sdk;
use twidget_sdk_policy::{
    ActionKind, HapticDecision, HapticGate, RequestConditions, RequestGate, RequestVerdict, StorageArea, WalletCall, WalletConditions,
    WidgetAction, WidgetData, WidgetRequest, WidgetRequestFailure, WidgetRequestOutcome,
};

use crate::types::{QueryAnswer, SandboxConfig, SandboxStats, WidgetContentSafeArea, is_strict_policy};

const ACTIVATION_PREFIX: &[u8] = b"activation:";
const HIDE_HOST_GLOBALS: &[u8] =
    b"(function(g){for(const n of ['__host','__hostBinary','__hostCanvas','__hostOps','__twDispatch'])if(!delete g[n])throw new Error(n);})(globalThis);";
const WALL_BACKSTOP_FACTOR: f64 = 10.0;
const WALL_BACKSTOP_MIN_MS: f64 = 5000.0;
/// MAX_TIMER_DELAY_MS.
const MAX_TIMER_DELAY_SECONDS: f64 = 86400.0;

/// `IntlProvider`: the shim's `Intl` request object formatted ("" when unsupported). Pure, no I/O.
pub trait IntlProvider: Send + Sync {
    fn format(&self, request: &Json) -> Vec<u8>;
}

/// How a delegate completes a request (`on_bridge_request`): call it with the client's outcome, or
/// `unsupported()` when the client handles no requests; once, from any thread. Later calls, and calls after the
/// engine died or was destroyed, are ignored; every clone shares the once.
#[derive(Clone)]
pub struct WidgetRequestResponder {
    settle: Arc<dyn Fn(Option<WidgetRequestOutcome>) + Send + Sync>,
}

impl std::fmt::Debug for WidgetRequestResponder {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("WidgetRequestResponder")
    }
}

impl WidgetRequestResponder {
    pub fn respond(&self, outcome: WidgetRequestOutcome) {
        (self.settle)(Some(outcome));
    }

    /// No client side for requests: the widget gets NOT_ALLOWED with the reason `unsupported`.
    pub fn unsupported(&self) {
        (self.settle)(None);
    }
}

/// All delegate calls happen on the engine's thread, never on the caller's. `on_query` is synchronous: it may
/// block the engine thread (and must not block the UI thread).
pub trait EngineDelegate: Send + Sync {
    fn on_ops(&self, engine: &Engine, json: &[u8]);
    fn on_query(&self, engine: &Engine, json: &[u8]) -> QueryAnswer;
    fn on_listen(&self, engine: &Engine, json: &[u8]);
    fn on_prevent_default(&self, engine: &Engine, json: &[u8]);
    fn on_request_frame(&self, engine: &Engine);
    fn on_log(&self, engine: &Engine, level: &[u8], text: &[u8]);
    fn on_diag(&self, engine: &Engine, kind: &[u8], detail: &[u8]);
    /// A bridge call that passed every check (docs/sdk-v1.md §3.2: arguments, user activation, the haptic limit).
    fn on_bridge_action(&self, _engine: &Engine, _action: &WidgetAction) {}
    /// `TelegramWidget.setResizeAnchor(edge)`: "top" or "bottom"; not an action.
    fn on_resize_anchor(&self, _engine: &Engine, _edge: &[u8]) {}
    /// `TelegramWidget.watchPublicData()` / `unwatchPublicData()`; not an action.
    fn on_public_data_watch(&self, _engine: &Engine, _on: bool) {}
    /// The new full content of a Web Storage area ("local" or "session") at the end of a task that changed it.
    fn on_storage(&self, _engine: &Engine, _area: &[u8], _values: &StorageArea) {}
    /// `TelegramWidget.request(data)` that passed its checks and the request gate; complete `respond` once.
    fn on_bridge_request(&self, _engine: &Engine, _request: &WidgetRequest, respond: WidgetRequestResponder) {
        respond.unsupported();
    }
    /// A wallet call that passed its checks and the request gate; complete `respond` once.
    fn on_bridge_wallet(&self, _engine: &Engine, _call: &WalletCall, respond: WidgetRequestResponder) {
        respond.unsupported();
    }
    /// `TelegramWidget.copyText(text)`; a privileged call, not an action.
    fn on_bridge_copy_text(&self, _engine: &Engine, _text: &[u8]) {}
    /// The method and arguments JSON of each call `on_bridge_action` gets, right after it.
    fn on_bridge(&self, _engine: &Engine, _method: &[u8], _args_json: &[u8]) {}
    fn on_loaded(&self, engine: &Engine);
    fn on_stats(&self, engine: &Engine, stats: &SandboxStats);
    fn on_died(&self, engine: &Engine, reason: &[u8]);
    fn on_anim(&self, _engine: &Engine, _json: &[u8]) {}
    /// A task of the engine began / ended: the calls between are that task's; a death inside it comes before its
    /// end, and after a stop from `on_task_started` none of its code runs.
    fn on_task_started(&self, _engine: &Engine) {}
    fn on_task_completed(&self, _engine: &Engine) {}
}

/// What both the owner's handle and the engine's thread reach.
struct EngineShared {
    /// After the constructor's changes (a preview asks nothing, the wallet only where the widget may ask).
    config: SandboxConfig,
    delegate: RwLock<Option<Arc<dyn EngineDelegate>>>,
    /// The engine's thread while a request's `respond` may still reach it: none once the owner drops the engine.
    liveness: Mutex<Option<Poster<Rc<Engine>>>>,
    platform: Arc<dyn Platform>,
}

fn now_seconds(platform: &dyn Platform) -> f64 {
    platform.steady_seconds()
}

fn now_ms(platform: &dyn Platform) -> f64 {
    now_seconds(platform) * 1000.0
}

/// Widget clocks tick in 100 µs steps, like a browser's `performance.now()`, so they cannot time cache or pool
/// effects of other widgets.
fn coarse_now_ms(ms: f64) -> f64 {
    (ms / 0.1).floor() * 0.1
}

/// The engine of one widget, the owner's handle: every input is queued on the engine's thread.
pub struct SandboxEngine {
    shared: Arc<EngineShared>,
    thread: JsThread<Rc<Engine>>,
}

impl std::fmt::Debug for SandboxEngine {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SandboxEngine").field("thread", &self.thread).finish()
    }
}

impl SandboxEngine {
    /// An engine with its own runtime, on a thread named `widget.sandbox`; the thread's refusal when it cannot
    /// start (`JSThread widget.sandbox: …`). The widget runs only after `start()`.
    pub fn new(
        html: Vec<u8>,
        mut config: SandboxConfig,
        resources: Arc<SandboxResources>,
        intl: Arc<dyn IntlProvider>,
        platform: Arc<dyn Platform>,
    ) -> Result<SandboxEngine, String> {
        // A preview (an inline result not sent yet) asks nothing whatever the flags say (docs/sdk-v1.md §2.16).
        if config.context.preview {
            config.context.is_copy = false;
            config.context.can_query = false;
            config.context.background_query = false;
        }
        // The wallet only where the widget may ask the bot (docs/sdk-v1.md §2.18).
        config.context.wallet = config.context.wallet && config.context.can_query;
        let shared = Arc::new(EngineShared { config, delegate: RwLock::new(None), liveness: Mutex::new(None), platform });
        let for_thread = Arc::clone(&shared);
        let thread = JsThread::with_poster("widget.sandbox", move |poster| {
            Rc::new_cyclic(|me| Engine::new(me.clone(), poster, for_thread, html, resources, intl))
        })?;
        *lock(&shared.liveness) = Some(thread.poster());
        Ok(SandboxEngine { shared, thread })
    }

    /// Set before `start()`; the engine's thread calls it from then on.
    pub fn set_delegate(&self, delegate: Arc<dyn EngineDelegate>) {
        *self.shared.delegate.write().unwrap_or_else(std::sync::PoisonError::into_inner) = Some(delegate);
    }

    fn post(&self, task: impl FnOnce(&Engine) + Send + 'static) {
        self.thread.post_task(move |e: &Rc<Engine>| task(e));
    }

    pub fn start(&self) {
        self.post(Engine::boot);
    }

    pub fn terminate(&self) {
        self.post(|e| e.kill(b"terminated"));
    }

    /// For an engine that died: its runtime goes, the thread ending once what it queued ran and then its context
    /// freed; every later input is dropped (a probe answers "ERROR no runtime"). Never from the engine's thread.
    pub fn release(&self) {
        self.thread.join();
    }

    /// The host stops the widget: the runtime dies with `reason` as it does for memory or CPU. From the
    /// engine's thread the delegate calls [`Engine::stop`]; from here it is queued.
    pub fn stop(&self, reason: &[u8]) {
        let reason = reason.to_vec();
        self.post(move |e| e.stop(&reason));
    }

    /// The validator's CPU time for this widget's messages, recorded against the JS quota on the engine thread.
    pub fn charge_validator(&self, ms: f64) {
        if !(ms > 0.0) {
            return;
        }
        self.post(move |e| e.charge_validator(ms));
    }

    pub fn event(&self, json: &[u8]) {
        if self.shared.config.context.preview {
            return;
        }
        self.enqueue(b"event".to_vec(), json.to_vec());
    }

    /// A real user input event from the renderer; every event of one physical gesture carries the same
    /// increasing `gesture` number.
    pub fn activation_event(&self, json: &[u8], gesture: i32) {
        if self.shared.config.context.preview {
            return;
        }
        self.enqueue([ACTIVATION_PREFIX, gesture.to_string().as_bytes()].concat(), json.to_vec());
    }

    pub fn frame(&self, time: f64) {
        self.enqueue(b"frame".to_vec(), twidget_json::number(coarse_now_ms(time)).into_bytes());
    }

    pub fn viewport(&self, inner_width: f64, inner_height: f64, dpr: f64) {
        let mut v = Json::object();
        v.set("innerWidth", inner_width).set("innerHeight", inner_height).set("dpr", dpr);
        self.enqueue(b"viewport".to_vec(), v.dump());
    }

    pub fn resize(&self, entries_json: &[u8]) {
        self.enqueue(b"resize".to_vec(), entries_json.to_vec());
    }

    /// The chat's theme changed: the colour scheme and the palette (checked here, as at boot) in one task.
    pub fn theme(&self, color_scheme: &[u8], theme_params: &BTreeMap<Vec<u8>, Vec<u8>>) {
        let mut t = Json::object();
        t.set("theme", color_scheme).set("themeParams", sdk::theme_params_json(&sdk::check_theme_params_map(theme_params)));
        self.enqueue(b"theme".to_vec(), t.dump());
    }

    /// What the client draws over the widget changed; before boot it only replaces the config's.
    pub fn content_safe_area(&self, area: WidgetContentSafeArea) {
        self.post(move |e| {
            if e.dead() {
                return;
            }
            if !e.booted() {
                *e.safe_area.borrow_mut() = area;
                return;
            }
            e.input(b"safeArea", &area.json().dump());
        });
    }

    /// The bot's data changed: the full state of both strings, checked as at boot.
    pub fn set_data(&self, data: &WidgetData) {
        let checked = sdk::check_widget_data(data, self.shared.config.context.is_copy);
        self.post(move |e| {
            if e.dead() {
                return;
            }
            let mut event = Json::object();
            event.set("type", "data");
            e.put_data(&mut event, &checked);
            e.input(b"bridge", &event.dump());
        });
    }

    /// The renderer's primary press began / ended: the haptic gate's touch, stamped here with the clock its
    /// decisions read.
    pub fn press_began(&self) {
        let at = now_ms(&*self.shared.platform);
        self.post(move |e| e.st.borrow_mut().haptics.press(at));
    }

    pub fn press_ended(&self) {
        let at = now_ms(&*self.shared.platform);
        self.post(move |e| e.st.borrow_mut().haptics.release(at));
    }

    /// The host knows whether the widget is on screen; the sandbox does not.
    pub fn set_visible(&self, visible: bool) {
        self.post(move |e| e.set_visible(visible));
    }

    /// Evaluates trusted diagnostic code in the widget context (tests only): the string value or the error text.
    pub fn probe(&self, code: &[u8]) -> Vec<u8> {
        let code = code.to_vec();
        self.thread.sync(move |e: &Rc<Engine>| e.probe(&code)).unwrap_or_else(|| b"ERROR no runtime".to_vec())
    }

    fn enqueue(&self, kind: Vec<u8>, arg: Vec<u8>) {
        self.post(move |e| e.input(&kind, &arg));
    }
}

impl Drop for SandboxEngine {
    fn drop(&mut self) {
        // A request's `respond` called from now on is ignored; one that holds the lock has posted its task, which
        // runs before the thread stops.
        *lock(&self.shared.liveness) = None;
        self.thread.join();
    }
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

#[derive(Clone, Debug)]
struct Deferred {
    kind: Vec<u8>,
    arg: Vec<u8>,
}

/// A background request the widget made off screen: it goes to the delegate when the widget appears.
#[derive(Clone, Debug)]
struct HeldRequest {
    id: i64,
    data: Vec<u8>,
}

/// The engine's state on its thread (the C++'s members), borrowed one step at a time.
struct State {
    dead: bool,
    started: bool,
    stats: SandboxStats,
    timers: BTreeMap<i32, Arc<Scheduled>>,
    last_binary: Option<Vec<u8>>,
    last_stats_post: f64,
    last_mem_check: f64,
    timer_limit_reported: bool,
    visible: bool,
    frame_wanted: bool,
    deferred: Vec<Deferred>,
    wake_timer: Option<Arc<Scheduled>>,
    wake_at: f64,
    hidden_tick_at: f64,
    budget: CpuBudget,
    /// The main-thread window (`main_thread_quota_ms`): layout queries and the cost of the ops.
    main_budget: CpuBudget,
    current_slice_ms: f64,
    current_kind: Vec<u8>,
    exclude_ms: f64,
    task_query_ms: f64,
    task_queries: i32,
    task_ops_cost_ms: f64,
    query_limit_hit: &'static str,
    last_throttle_diag: f64,
    throttled_since_diag: BTreeMap<Vec<u8>, i32>,
    pending_before_boot: Vec<(Vec<u8>, Vec<u8>)>,
    #[cfg(not(feature = "oracle-stats-clock"))]
    trailing_stats: Option<Arc<Scheduled>>,
    #[cfg(feature = "oracle-stats-clock")]
    trailing_stats: Option<f64>,
    /// The gesture whose event task is running (0 outside one); one gesture unlocks one privileged bridge call.
    active_gesture: i32,
    consumed_gesture: i32,
    blocked_bridge_reported: i32,
    rejected_bridge_reported: i32,
    rejected_storage_reported: i32,
    haptics: HapticGate,
    requests: RequestGate,
    held_request: Option<HeldRequest>,
    blocked_requests_reported: i32,
    preview_blocked_reported: i32,
}

impl State {
    fn cancel_trailing_stats(&mut self) {
        #[cfg(not(feature = "oracle-stats-clock"))]
        if let Some(timer) = self.trailing_stats.take() {
            timer.cancel();
        }
        #[cfg(feature = "oracle-stats-clock")]
        {
            self.trailing_stats = None;
        }
    }
}

/// The engine on its thread: what `&Engine` the delegate gets is.
pub struct Engine {
    me: Weak<Engine>,
    poster: Poster<Rc<Engine>>,
    shared: Arc<EngineShared>,
    html: Vec<u8>,
    resources: Arc<SandboxResources>,
    intl: Arc<dyn IntlProvider>,
    /// The widget's runtime from `boot` on; borrowed only while the widget's code runs.
    js: RefCell<Option<NovaContext>>,
    deadlines: RefCell<Option<Rc<Deadlines>>>,
    /// The config's band until boot; the widget's own afterwards.
    safe_area: RefCell<WidgetContentSafeArea>,
    st: RefCell<State>,
}

impl std::fmt::Debug for Engine {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Engine").field("dead", &self.st.borrow().dead).finish()
    }
}

/// `{"type":"answer","id":…}`: the start of a `bridge` answer event (a refused request's, or an answer).
fn answer_event(id: i64) -> Json {
    let mut event = Json::object();
    event.set("type", "answer").set("id", id);
    event
}

fn split_prefix(s: &[u8]) -> (&[u8], &[u8]) {
    match s.iter().position(|&c| c == b':') {
        Some(i) => (&s[..i], &s[i + 1..]),
        None => (s, b""),
    }
}

fn count_occurrences(text: &[u8], needle: &[u8]) -> i32 {
    let mut count = 0;
    let mut pos = 0;
    while let Some(i) = find(&text[pos..], needle) {
        count += 1;
        pos += i + needle.len();
    }
    count
}

fn find(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() {
        return Some(0);
    }
    haystack.windows(needle.len()).position(|w| w == needle)
}

/// Commands in the canvas ops of an ops batch (JSON): the elements of each `"cmds":[…]` array. Like `"op":` the
/// key cannot occur inside a JSON string (a quote there is always `\"`); the strings inside the array are skipped.
fn count_canvas_commands(json: &[u8]) -> i32 {
    const KEY: &[u8] = b"\"cmds\":[";
    let mut count = 0;
    let mut from = 0;
    while let Some(found) = find(&json[from..], KEY) {
        let mut i = from + found + KEY.len();
        let mut depth = 1;
        let mut any = false;
        while i < json.len() && depth > 0 {
            match json[i] {
                b'"' => {
                    i += 1;
                    while i < json.len() && json[i] != b'"' {
                        if json[i] == b'\\' {
                            i += 1;
                        }
                        i += 1;
                    }
                    any = true;
                }
                b'[' | b'{' => {
                    any = true;
                    depth += 1;
                }
                b']' | b'}' => depth -= 1,
                b',' => {
                    if depth == 1 {
                        count += 1;
                    }
                }
                b' ' | b'\n' | b'\r' | b'\t' => {}
                _ => any = true,
            }
            i += 1;
        }
        // n elements are n - 1 commas
        if any {
            count += 1;
        }
        from = i;
    }
    count
}

fn truncate(s: &[u8], n: usize) -> &[u8] {
    &s[..s.len().min(n)]
}

/// C's `strtol(s, &end, 10)`: the value and the bytes it read (0 when there is no number); `long` saturates.
fn strtol(s: &[u8]) -> (i64, usize) {
    let mut i = 0;
    while i < s.len() && matches!(s[i], b' ' | b'\t' | b'\n' | b'\x0b' | b'\x0c' | b'\r') {
        i += 1;
    }
    let mut negative = false;
    if i < s.len() && (s[i] == b'+' || s[i] == b'-') {
        negative = s[i] == b'-';
        i += 1;
    }
    let digits = i;
    let (min, max) = if cfg!(windows) { (i64::from(i32::MIN), i64::from(i32::MAX)) } else { (i64::MIN, i64::MAX) };
    let mut value: i64 = 0;
    let mut over = false;
    while i < s.len() && s[i].is_ascii_digit() {
        let d = i64::from(s[i] - b'0');
        if !over {
            match value.checked_mul(10).and_then(|v| if negative { v.checked_sub(d) } else { v.checked_add(d) }) {
                Some(v) if v >= min && v <= max => value = v,
                _ => over = true,
            }
        }
        i += 1;
    }
    if i == digits {
        return (0, 0);
    }
    if over {
        value = if negative { min } else { max };
    }
    (value, i)
}

/// C's `atoi`: as `strtol`, an `int` (out of range wraps, as the C libraries do).
fn atoi(s: &[u8]) -> i32 {
    strtol(s).0 as i32
}

/// "chatData and publicData": the fields of a `data-rejected` detail.
fn join_fields(fields: &[&str]) -> String {
    fields.join(" and ")
}

/// Why a request was refused, for the `request-blocked` detail (REQUEST_REFUSED in widget-host.ts).
fn refusal_text(verdict: RequestVerdict) -> &'static str {
    match verdict {
        RequestVerdict::Disabled => {
            "the server allows no requests for this message (no can_query: a widget sent without allow_queries, or a forwarded copy of one sent without fwd_allow_queries)"
        }
        RequestVerdict::Busy => "a request is already in flight",
        RequestVerdict::NoActivation => {
            "it needs a tap on the visible widget, one request per tap; this message allows no background requests (no background_query)"
        }
        RequestVerdict::RateLimited => "background requests at most once per 5 s",
        RequestVerdict::NoWallet => "the client has no wallet for this user (no WidgetContext::wallet)",
        _ => "",
    }
}

/// Where a wallet call's refusal differs from a request's (WALLET_REFUSED in widget-host.ts).
fn wallet_refusal_text(verdict: RequestVerdict) -> &'static str {
    match verdict {
        RequestVerdict::NoActivation => {
            "it needs a tap on the visible widget, or the answer to a call made by a tap within 10 s; one call each"
        }
        RequestVerdict::Busy => "a request or a wallet call is already in flight",
        _ => refusal_text(verdict),
    }
}

impl Engine {
    fn new(
        me: Weak<Engine>,
        poster: Poster<Rc<Engine>>,
        shared: Arc<EngineShared>,
        html: Vec<u8>,
        resources: Arc<SandboxResources>,
        intl: Arc<dyn IntlProvider>,
    ) -> Engine {
        let c = &shared.config;
        // The runtime's two windows, from the caller's numbers.
        let budget = CpuBudget::new(c.cpu_limit_ms, c.cpu_quota_ms, c.cpu_quota_window_ms, c.cpu_kill_after_ms);
        let main_budget = CpuBudget::new(c.cpu_limit_ms, c.main_thread_quota_ms, c.cpu_quota_window_ms, 0.0);
        let st = State {
            dead: false,
            started: false,
            stats: SandboxStats::default(),
            timers: BTreeMap::new(),
            last_binary: None,
            last_stats_post: 0.0,
            last_mem_check: 0.0,
            timer_limit_reported: false,
            visible: c.visible,
            frame_wanted: false,
            deferred: Vec::new(),
            wake_timer: None,
            wake_at: 0.0,
            hidden_tick_at: 0.0,
            budget,
            main_budget,
            current_slice_ms: 0.0,
            current_kind: b"boot".to_vec(),
            exclude_ms: 0.0,
            task_query_ms: 0.0,
            task_queries: 0,
            task_ops_cost_ms: 0.0,
            query_limit_hit: "",
            last_throttle_diag: -1e300,
            throttled_since_diag: BTreeMap::new(),
            pending_before_boot: Vec::new(),
            trailing_stats: None,
            active_gesture: 0,
            consumed_gesture: 0,
            blocked_bridge_reported: 0,
            rejected_bridge_reported: 0,
            rejected_storage_reported: 0,
            haptics: HapticGate::new(),
            requests: RequestGate::new(),
            held_request: None,
            blocked_requests_reported: 0,
            preview_blocked_reported: 0,
        };
        let safe_area = c.content_safe_area;
        Engine {
            me,
            poster,
            shared,
            html,
            resources,
            intl,
            js: RefCell::new(None),
            deadlines: RefCell::new(None),
            safe_area: RefCell::new(safe_area),
            st: RefCell::new(st),
        }
    }

    fn config(&self) -> &SandboxConfig {
        &self.shared.config
    }

    fn platform(&self) -> &dyn Platform {
        &*self.shared.platform
    }

    fn now_ms(&self) -> f64 {
        now_ms(self.platform())
    }

    fn now_seconds(&self) -> f64 {
        now_seconds(self.platform())
    }

    fn cpu_seconds(&self) -> f64 {
        self.platform().thread_cpu_seconds()
    }

    fn delegate(&self) -> Option<Arc<dyn EngineDelegate>> {
        self.shared.delegate.read().unwrap_or_else(std::sync::PoisonError::into_inner).clone()
    }

    fn diag(&self, kind: &str, detail: impl AsRef<[u8]>) {
        if let Some(d) = self.delegate() {
            d.on_diag(self, kind.as_bytes(), detail.as_ref());
        }
    }

    fn dead(&self) -> bool {
        self.st.borrow().dead
    }

    /// The runtime exists (`js_` in the C++): set at boot with the context, never borrowed long.
    fn booted(&self) -> bool {
        self.deadlines.borrow().is_some()
    }

    fn visible(&self) -> bool {
        self.st.borrow().visible
    }

    fn deadlines(&self) -> Option<Rc<Deadlines>> {
        self.deadlines.borrow().clone()
    }

    /// Queues `task` on this thread after everything queued before it.
    fn post(&self, task: impl FnOnce(&Engine) + Send + 'static) {
        self.poster.post_task(move |e: &Rc<Engine>| task(e));
    }

    fn after(&self, seconds: f64, task: impl FnOnce(&Engine) + Send + 'static) -> Arc<Scheduled> {
        self.poster.after(seconds, move |e: &Rc<Engine>| task(e))
    }

    // MARK: lifecycle

    /// The host stops the widget (a counted cap of the validator passed under strict): the runtime dies with
    /// `reason`, and the task that is still on the stack is interrupted at the next poll (uncatchable, as the
    /// layout-query limit does); `host_call` forwards nothing more meanwhile.
    pub fn stop(&self, reason: &[u8]) {
        if self.dead() {
            return;
        }
        self.kill(reason);
        if let Some(d) = self.deadlines() {
            d.cpu_deadline.set(f64::NEG_INFINITY);
        }
    }

    fn boot(&self) {
        {
            let mut st = self.st.borrow_mut();
            if st.started {
                return;
            }
            st.started = true;
        }
        let mut js = match NovaContext::new(self.config().memory_limit_bytes, JS_STACK_LIMIT, Arc::clone(&self.shared.platform)) {
            Ok(js) => js,
            Err(e) => return self.kill(e.text.as_bytes()),
        };
        let me = self.me.clone();
        let registered = js
            .register_function(
                "__host",
                Box::new(move |args| {
                    let Some(e) = me.upgrade() else { return Ok(HostReturn::Undefined) };
                    let empty = Vec::new();
                    Ok(e.host_call(args.first().unwrap_or(&empty), args.get(1).unwrap_or(&empty)))
                }),
            )
            .and_then(|()| {
                let me = self.me.clone();
                js.register_function(
                    "__hostBinary",
                    Box::new(move |_| {
                        let Some(e) = me.upgrade() else { return Ok(HostReturn::Null) };
                        Ok(match e.st.borrow_mut().last_binary.take() {
                            Some(data) => HostReturn::ArrayBuffer(data),
                            None => HostReturn::Null,
                        })
                    }),
                )
            });
        *self.deadlines.borrow_mut() = Some(js.deadlines());
        *self.js.borrow_mut() = Some(js);
        if let Err(e) = registered {
            return self.kill(e.text.as_bytes());
        }
        let cpu_limit_ms = self.config().cpu_limit_ms;
        self.task(b"boot", cpu_limit_ms, || self.boot_task());
        self.task(b"boot", cpu_limit_ms, || {
            self.call_tw(b"loaded", b"");
        });
        if !self.dead()
            && let Some(d) = self.delegate()
        {
            d.on_loaded(self);
        }
        self.post_stats(true);
        let queued = std::mem::take(&mut self.st.borrow_mut().pending_before_boot);
        for (kind, arg) in queued {
            if !self.dead() {
                self.dispatch(&kind, &arg);
            }
        }
    }

    fn boot_task(&self) {
        let t0 = self.now_ms();
        let d = self.deadlines().expect("booted");
        // Generous one-off budget for our own bundle; its time is ours, not the widget's.
        d.deadline.set(self.now_seconds() + self.config().cpu_limit_ms.max(5000.0) / 1000.0);
        d.cpu_deadline.set(f64::INFINITY);
        let how = self.eval_bundle(&self.resources.sandbox_bundle, "sandbox-bundle.js");
        let exclude_ms = self.now_ms() - t0;
        self.st.borrow_mut().exclude_ms = exclude_ms;
        // Fail closed: a runtime whose shim did not load completely (its network stubs and DOM capture may be
        // missing) never runs widget code.
        if how == "failed" {
            if !self.dead() {
                self.kill(b"sandbox bundle failed to load");
            }
            return;
        }
        // The bundle holds private references to the host functions; widget code must not reach them, nor the
        // shim's dispatcher, through the global object.
        let hidden = {
            let mut js = self.js.borrow_mut();
            let js = js.as_mut().expect("booted");
            js.capture_global_function("__twDispatch") && js.evaluate(HIDE_HOST_GLOBALS, false).is_ok()
        };
        if !hidden {
            if !self.dead() {
                self.kill(b"sandbox bundle failed to load");
            }
            return;
        }
        self.diag("boot", format!("sandbox bundle eval {} ms ({how})", exclude_ms as i32));
        self.reset_deadline();
        let c = self.config();
        let mut config = Json::object();
        config
            .set("memoryLimitBytes", c.memory_limit_bytes as f64)
            .set("cpuLimitMs", c.cpu_limit_ms)
            .set("cpuQuotaMs", c.cpu_quota_ms)
            .set("cpuQuotaWindowMs", c.cpu_quota_window_ms)
            .set("cpuKillAfterMs", c.cpu_kill_after_ms)
            .set("mainThreadQuotaMs", c.main_thread_quota_ms)
            .set("maxTimers", c.max_timers)
            // The shim refuses at the validator's counted caps under strict.
            .set("policy", if is_strict_policy(&c.policy) { "strict" } else { "compat" })
            .set("visible", self.visible());
        let mut viewport = Json::object();
        viewport.set("innerWidth", c.inner_width).set("innerHeight", c.inner_height).set("dpr", c.device_pixel_ratio);
        config.set("viewport", viewport);
        config.set("theme", c.theme.as_slice());
        // SDK v1 (docs/sdk-v1.md §3.1): the client's values are trusted but still checked.
        config.set("themeParams", sdk::theme_params_json(&sdk::check_theme_params_map(&c.theme_params)));
        config.set("locale", sdk::check_locale(&c.locale).unwrap_or_else(|| sdk::DEFAULT_LOCALE.as_bytes().to_vec()));
        config.set("contentSafeArea", self.safe_area.borrow().json());
        config.set("profile", sdk::PROFILE_VERSION);
        // A preview has no message yet: no storage of its own.
        let (storage, preload_accepted) = if c.context.preview { (Json::object(), true) } else { sdk::storage_preload_json(&c.storage) };
        config.set("storage", storage);
        if !preload_accepted {
            self.diag(
                "storage-rejected",
                format!(
                    "the preload is not {{ local, session }} objects of strings within {} bytes each; the widget starts with empty storage",
                    sdk::STORAGE_QUOTA_BYTES
                ),
            );
        }
        // docs/sdk-v1.md §2.13, §2.16, §4.4: the bot's data and the message's flags. A copy gets no chatData and
        // no chat type; the viewer only where the widget may send requests (canQuery, never in a preview).
        let context = &c.context;
        let reveals_viewer = !context.preview && context.can_query;
        self.put_data(&mut config, &sdk::check_widget_data(&c.data, context.is_copy));
        config.set("isCopy", context.is_copy).set("canQuery", context.can_query).set("isPreview", context.preview);
        let chat_type = if context.preview || context.is_copy { None } else { sdk::check_chat_type(&context.chat_type) };
        config.set("chatType", chat_type.map_or(Json::Null, Json::from));
        // The viewer in the widget's field names (Mini Apps' WebAppUser); a field that breaks its rule is left out and reported.
        let viewer = if reveals_viewer { sdk::check_viewer(context.viewer.as_ref()) } else { sdk::CheckedViewer::default() };
        config.set("viewer", viewer.viewer.as_ref().map_or(Json::Null, sdk::viewer_json));
        config.set("wallet", context.wallet);
        for field in &viewer.rejected {
            self.diag("data-rejected", format!("viewer.{field}: {}; the widget does not see it", sdk::viewer_field_rule(field.as_bytes())));
        }
        // Only what this runtime implements (Swift runtimeFeatures, WEB_FEATURES in worker.ts).
        let mut features = Json::array();
        for feature in &c.features {
            if [&b"webAnimations"[..], b"noInPlaceText", b"noAttrFast", b"noCanvasRect"].contains(&feature.as_slice()) {
                features.push(feature.as_slice());
            }
        }
        config.set("features", features);
        let mut boot = Json::object();
        boot.set("html", self.html.as_slice()).set("config", config);
        self.call_tw(b"boot", &boot.dump());
    }

    // MARK: inputs from the host

    /// The body of `enqueue`, on the engine thread: buffered before boot, a hidden frame dropped, else dispatched.
    fn input(&self, kind: &[u8], arg: &[u8]) {
        if self.dead() {
            return;
        }
        if !self.booted() {
            self.st.borrow_mut().pending_before_boot.push((kind.to_vec(), arg.to_vec()));
            return;
        }
        // A frame that arrives after the widget was hidden is dropped; the shim's callbacks wait for the next visible frame.
        if kind == b"frame" && !self.visible() {
            self.st.borrow_mut().frame_wanted = true;
            return;
        }
        self.dispatch(kind, arg);
    }

    fn put_data(&self, event: &mut Json, checked: &sdk::CheckedWidgetData) {
        event.set("chatData", checked.chat_data.clone().map_or(Json::Null, Json::from));
        event.set("publicData", checked.public_data.clone().map_or(Json::Null, Json::from));
        if checked.rejected.is_empty() {
            return;
        }
        self.diag(
            "data-rejected",
            format!(
                "{}: not text of at most {} bytes (UTF-8, no NUL); the widget sees null",
                join_fields(&checked.rejected),
                sdk::MAX_WIDGET_DATA_BYTES
            ),
        );
    }

    fn set_visible(&self, v: bool) {
        {
            let st = self.st.borrow();
            if st.dead || st.visible == v {
                return;
            }
        }
        self.st.borrow_mut().visible = v;
        // The widget appeared on screen: its next background request may go out at once, the one it made off
        // screen now (docs/sdk-v1.md §2.14), before the widget hears it is visible.
        if v {
            let now = self.now_ms();
            let held = {
                let mut st = self.st.borrow_mut();
                if st.requests.show(now) { st.held_request.take() } else { None }
            };
            if let Some(held) = held {
                self.deliver_request(held.id, &held.data, false);
            }
        }
        // boot reads `visible` for the shim
        if !self.booted() {
            return;
        }
        self.dispatch(b"visibility", if v { b"{\"visible\":true}" } else { b"{\"visible\":false}" });
        if v {
            let wanted = std::mem::take(&mut self.st.borrow_mut().frame_wanted);
            if wanted && let Some(d) = self.delegate() {
                d.on_request_frame(self);
            }
            if !self.st.borrow().deferred.is_empty() {
                self.wake();
            }
        }
    }

    fn probe(&self, code: &[u8]) -> Vec<u8> {
        #[cfg(feature = "oracle-stats-clock")]
        if code == b"__twidget_oracle_advance_stats__" {
            if self.st.borrow().trailing_stats.is_some_and(|due| self.now_seconds() >= due) {
                self.post_stats(true);
            }
            return b"1".to_vec();
        }
        if self.dead() {
            return b"ERROR no runtime".to_vec();
        }
        // From a delegate call inside the widget's own code the runtime is busy: none to probe.
        let Ok(mut js) = self.js.try_borrow_mut() else { return b"ERROR no runtime".to_vec() };
        let Some(js) = js.as_mut() else { return b"ERROR no runtime".to_vec() };
        let source = [&b"(function(){ try { return String("[..], code, b"); } catch (e) { return 'THREW ' + e; } })()"].concat();
        match js.evaluate(&source, false) {
            Ok(r) => r.string.unwrap_or_else(|| match r.number {
                Some(n) => twidget_json::number(n).into_bytes(),
                None => b"undefined".to_vec(),
            }),
            Err(e) => [b"ERROR ", e.text.as_bytes()].concat(),
        }
    }

    fn charge_validator(&self, ms: f64) {
        if self.dead() {
            return;
        }
        let now = self.now_ms();
        {
            let mut st = self.st.borrow_mut();
            st.budget.record(b"validator", ms, now);
            st.stats.validator_ms += ms;
            st.stats.cpu_window_ms = st.budget.used(now);
        }
        self.post_stats(false);
    }

    /// Runs one task for an input (event, timer, frame, …) if the CPU quota and the main-thread window allow,
    /// otherwise defers it (false). Events and visibility always run.
    fn dispatch(&self, kind: &[u8], arg: &[u8]) -> bool {
        if self.dead() {
            return true;
        }
        if let Some(rest) = kind.strip_prefix(ACTIVATION_PREFIX) {
            let gesture = atoi(rest);
            {
                let mut st = self.st.borrow_mut();
                st.active_gesture = if st.visible && gesture > st.consumed_gesture { gesture } else { 0 };
            }
            let ran = self.dispatch(b"event", arg);
            self.st.borrow_mut().active_gesture = 0;
            return ran;
        }
        let now = self.now_ms();
        let c = self.config();
        let (js, main) = {
            let mut st = self.st.borrow_mut();
            (st.budget.decide(kind, now), st.main_budget.decide(kind, now))
        };
        if !js.run || !main.run {
            let kill = {
                let st = self.st.borrow();
                st.budget.should_kill(now).then(|| st.budget.throttled_for(now))
            };
            if let Some(throttled) = kill {
                self.kill(
                    format!(
                        "CPU quota: throttled for {:.1} s ({} ms per {} ms, cpuKillAfterMs={})",
                        throttled / 1000.0,
                        c.cpu_quota_ms as i32,
                        c.cpu_quota_window_ms as i32,
                        c.cpu_kill_after_ms as i32
                    )
                    .as_bytes(),
                );
                return true;
            }
            self.defer_task(kind, arg);
            let report = {
                let mut st = self.st.borrow_mut();
                st.stats.throttled_tasks += 1;
                *st.throttled_since_diag.entry(kind.to_vec()).or_insert(0) += 1;
                if now - st.last_throttle_diag >= 1000.0 {
                    st.last_throttle_diag = now;
                    let mut parts = Vec::new();
                    for (k, n) in std::mem::take(&mut st.throttled_since_diag) {
                        if !parts.is_empty() {
                            parts.extend_from_slice(b", ");
                        }
                        parts.extend_from_slice(format!("{n} ").as_bytes());
                        parts.extend_from_slice(&k);
                        if n != 1 {
                            parts.push(b's');
                        }
                    }
                    let window = if js.run {
                        format!(
                            "{} ms of the {} ms per {} ms main-thread budget used by layout queries and ops",
                            st.main_budget.used(now) as i32,
                            c.main_thread_quota_ms as i32,
                            c.cpu_quota_window_ms as i32
                        )
                    } else {
                        format!(
                            "{} ms of the {} ms per {} ms budget used",
                            st.budget.used(now) as i32,
                            c.cpu_quota_ms as i32,
                            c.cpu_quota_window_ms as i32
                        )
                    };
                    Some([&b"deferred "[..], &parts, b": ", window.as_bytes()].concat())
                } else {
                    None
                }
            };
            if let Some(detail) = report {
                self.diag("cpu-throttled", detail);
            }
            let js_at = if js.run { 0.0 } else { js.retry_at };
            let main_at = if main.run { 0.0 } else { main.retry_at };
            self.schedule_wake(js_at.max(main_at) / 1000.0);
            return false;
        }
        self.task(kind, js.slice_ms, || {
            self.call_tw(kind, arg);
        });
        true
    }

    fn defer_task(&self, kind: &[u8], arg: &[u8]) {
        let mut st = self.st.borrow_mut();
        if kind == b"frame"
            && let Some(item) = st.deferred.iter_mut().find(|item| item.kind == b"frame")
        {
            item.arg = arg.to_vec();
            return;
        }
        // intervals do not accumulate missed ticks
        if kind == b"timer" && st.deferred.iter().any(|item| item.kind == b"timer" && item.arg == arg) {
            return;
        }
        st.deferred.push(Deferred { kind: kind.to_vec(), arg: arg.to_vec() });
    }

    fn schedule_wake(&self, at: f64) {
        let mut st = self.st.borrow_mut();
        if st.wake_timer.is_some() && at >= st.wake_at {
            return;
        }
        if let Some(t) = st.wake_timer.take() {
            t.cancel();
        }
        st.wake_at = at;
        let delay = 0f64.max(at - self.now_seconds());
        st.wake_timer = Some(self.after(delay, Engine::wake));
    }

    /// Runs deferred tasks in order; stops (and re-queues the rest) if one is deferred again.
    fn wake(&self) {
        let items = {
            let mut st = self.st.borrow_mut();
            st.wake_timer = None;
            st.wake_at = f64::INFINITY;
            st.hidden_tick_at = 0.0;
            std::mem::take(&mut st.deferred)
        };
        for i in 0..items.len() {
            if self.dead() {
                return;
            }
            if !self.dispatch(&items[i].kind, &items[i].arg) {
                // The C++ puts the rest in front of what the failed dispatch deferred again, itself among it.
                let mut st = self.st.borrow_mut();
                let again = std::mem::take(&mut st.deferred);
                st.deferred = items[i..].iter().cloned().chain(again).collect();
                return;
            }
        }
    }

    fn on_timer_fired(&self, id: i32) {
        self.st.borrow_mut().timers.remove(&id);
        let id_text = id.to_string().into_bytes();
        if !self.visible() {
            self.defer_task(b"timer", &id_text);
            let at = {
                let mut st = self.st.borrow_mut();
                if st.hidden_tick_at == 0.0 {
                    st.hidden_tick_at = self.now_seconds() + 1.0;
                }
                st.hidden_tick_at
            };
            self.schedule_wake(at);
            return;
        }
        self.dispatch(b"timer", &id_text);
    }

    // MARK: the single capability

    fn host_call(&self, kind: &[u8], arg: &[u8]) -> HostReturn {
        // Stopped by the host (`stop`) from inside this task: until the interrupt lands, nothing more reaches the delegate.
        if self.dead() {
            return HostReturn::Undefined;
        }
        let delegate = self.delegate();
        match kind {
            b"ops" => {
                let ops = count_occurrences(arg, b"\"op\":");
                {
                    let mut st = self.st.borrow_mut();
                    st.stats.ops = st.stats.ops.wrapping_add(ops);
                    // What the batch costs the host's main thread (the validator, the renderer).
                    st.task_ops_cost_ms += CpuBudget::ops_cost_ms(f64::from(ops), f64::from(count_canvas_commands(arg)));
                }
                if let Some(d) = delegate {
                    d.on_ops(self, arg);
                }
                return HostReturn::Undefined;
            }
            b"query" => return HostReturn::String(self.sync_query(arg)),
            b"now" => return HostReturn::Number(coarse_now_ms(self.now_ms())),
            b"bridge" | b"intl" | b"timer" | b"stat" => {
                let limit = if kind == b"bridge" { sdk::MAX_BRIDGE_CALL_BYTES } else { sdk::MAX_HOST_CALL_BYTES };
                if arg.len() > limit {
                    return HostReturn::RangeError(
                        format!("arguments too long: {} bytes of JSON, at most {limit} ({})", arg.len(), String::from_utf8_lossy(kind))
                            .into_bytes(),
                    );
                }
            }
            _ => {}
        }
        match kind {
            b"timer" => self.timer(arg),
            b"clearTimer" => {
                let (id, read) = strtol(arg);
                // The whole text is the number (`*end == '\0'`): with no number at all strtol reads nothing, which is
                // the whole of an empty text.
                if read == arg.len() {
                    let removed = self.st.borrow_mut().timers.remove(&(id as i32));
                    if let Some(t) = removed {
                        t.cancel();
                    }
                }
            }
            b"raf" => {
                if !self.visible() {
                    // one frame is requested when the widget becomes visible again
                    self.st.borrow_mut().frame_wanted = true;
                } else if let Some(d) = delegate {
                    d.on_request_frame(self);
                }
            }
            b"log" => {
                let (level, text) = split_prefix(arg);
                if let Some(d) = delegate {
                    d.on_log(self, level, truncate(text, 4000));
                }
            }
            b"diag" => {
                let (k, detail) = split_prefix(arg);
                if let Some(d) = delegate {
                    d.on_diag(self, truncate(k, 64), truncate(detail, 4000));
                }
            }
            b"listen" => {
                if let Some(d) = delegate {
                    d.on_listen(self, arg);
                }
            }
            b"pd" => {
                if let Some(d) = delegate {
                    d.on_prevent_default(self, arg);
                }
            }
            b"anim" => {
                if let Some(d) = delegate {
                    d.on_anim(self, arg);
                }
            }
            b"bridge" => self.bridge(arg, delegate),
            b"storage" => self.storage(arg, delegate),
            b"intl" => {
                return match Json::parse(arg) {
                    Some(req) if req.is_object() => HostReturn::String(self.intl.format(&req)),
                    _ => HostReturn::String(Vec::new()),
                };
            }
            b"random" => {
                let n = strtol(arg).0.clamp(0, 65536) as usize;
                let mut bytes = vec![0u8; n];
                if n > 0 {
                    self.platform().random(&mut bytes);
                }
                let mut out = Vec::with_capacity(n * 4 + 2);
                out.push(b'[');
                for (i, b) in bytes.iter().enumerate() {
                    if i > 0 {
                        out.push(b',');
                    }
                    out.extend_from_slice(b.to_string().as_bytes());
                }
                out.push(b']');
                return HostReturn::String(out);
            }
            b"stat" => {
                if let Some(s) = Json::parse(arg)
                    && s.is_object()
                {
                    let mut st = self.st.borrow_mut();
                    st.stats.query_cache_hits = s.get("cacheHits").as_int(0);
                    st.stats.missed_mutations = s.get("missed").as_int(0);
                }
            }
            _ => {}
        }
        HostReturn::Undefined
    }

    fn timer(&self, arg: &[u8]) {
        let Some(pair) = Json::parse(arg) else { return };
        if !pair.is_array() || pair.size() != 2 || !pair.at(0).is_number() {
            return;
        }
        let id = pair.at(0).as_int(0);
        let mut delay = (0f64.max(pair.at(1).as_number(0.0)) / 1000.0).min(MAX_TIMER_DELAY_SECONDS);
        // Hidden: 1 Hz, like a background tab. The timers keep the order of their own delays.
        if !self.visible() {
            delay = delay.max(1.0 + delay / 10.0);
        }
        let report = {
            let mut st = self.st.borrow_mut();
            // An id that has a timer replaces it: dropping the old handle does not cancel it.
            if let Some(replaced) = st.timers.remove(&id) {
                replaced.cancel();
                false
            } else if st.timers.len() as i64 >= i64::from(self.config().max_timers) {
                // Backstop: the shim should have thrown RangeError before asking.
                let first = !st.timer_limit_reported;
                st.timer_limit_reported = true;
                if !first {
                    return;
                }
                true
            } else {
                false
            }
        };
        if report {
            self.diag(
                "timer-limit-enforced",
                format!("engine refused a timer beyond maxTimers={}; the shim did not stop it", self.config().max_timers),
            );
            return;
        }
        let handle = self.after(delay, move |e| {
            if e.dead() {
                return;
            }
            e.on_timer_fired(id);
        });
        self.st.borrow_mut().timers.insert(id, handle);
    }

    fn bridge(&self, arg: &[u8], delegate: Option<Arc<dyn EngineDelegate>>) {
        // docs/sdk-v1.md §3.2, in this order: the method and its arguments, user activation for the links and
        // copyText, the haptic limit; then the action. A request has its own gate. Lone surrogates stay WTF-8 so
        // checkLink and the string checks of the bot's data and requests refuse them.
        let call = Json::parse_with(arg, Surrogates::Keep, twidget_json::DEFAULT_MAX_DEPTH);
        let method = call.as_ref().filter(|c| c.is_array() && c.size() > 0).map(|c| c.at(0));
        let checked = call.as_ref().filter(|c| c.is_array()).and_then(|c| sdk::check_bridge_call(c.at(0), c.at(1)));
        let name: Vec<u8> = match method {
            None => b"undefined".to_vec(),
            Some(m) if m.is_string() => truncate(m.as_str(), 64).to_vec(),
            Some(m) => truncate(&m.dump(), 64).to_vec(),
        };
        let Some(checked) = checked else {
            let report = {
                let mut st = self.st.borrow_mut();
                let report = st.rejected_bridge_reported < sdk::MAX_REJECTION_REPORTS && delegate.is_some();
                if report {
                    st.rejected_bridge_reported += 1;
                }
                report
            };
            if report {
                self.diag("bridge-rejected", [&name[..], b": unknown method or malformed arguments"].concat());
            }
            return;
        };
        let c = self.config();
        // A preview may only say it is ready and set its anchor (docs/sdk-v1.md §2.16).
        if c.context.preview && !sdk::allowed_in_preview(&name) {
            let report = {
                let mut st = self.st.borrow_mut();
                let report = st.preview_blocked_reported < sdk::MAX_REJECTION_REPORTS && delegate.is_some();
                if report {
                    st.preview_blocked_reported += 1;
                }
                report
            };
            if report {
                self.diag("preview-blocked", [&name[..], b" ignored: a preview (an inline result not sent yet) takes no actions"].concat());
            }
            let id = checked.request.as_ref().map(|r| r.id).or(checked.wallet.as_ref().map(|w| w.id));
            if let Some(id) = id {
                let mut event = answer_event(id);
                event.set("error", "NOT_ALLOWED").set("reason", "preview");
                self.enqueue(b"bridge", event.dump());
            }
            return;
        }
        // setResizeAnchor is not an action (docs/sdk-v1.md §2.12): the delegate gets it now, ahead of the task's ops.
        if let Some(edge) = &checked.resize_anchor {
            if let Some(d) = &delegate {
                d.on_resize_anchor(self, edge);
            }
            return;
        }
        // Not an action either: whether the widget watches its public_data.
        if let Some(on) = checked.watch_public_data {
            if let Some(d) = &delegate {
                d.on_public_data_watch(self, on);
            }
            return;
        }
        // A request has its own gate (a press or the background rules; one in flight).
        if let Some(r) = &checked.request {
            return self.request(r.id, &r.data, r.wallet);
        }
        // A wallet call shares the request gate (a press or the grant, never the background).
        if let Some(w) = &checked.wallet {
            return self.wallet_call(w.id, &w.call);
        }
        if is_strict_policy(&c.policy) && sdk::is_privileged_method(&name) {
            let blocked = {
                let mut st = self.st.borrow_mut();
                if !(st.active_gesture > st.consumed_gesture && st.visible) {
                    let report = st.blocked_bridge_reported < sdk::MAX_REJECTION_REPORTS && delegate.is_some();
                    if report {
                        st.blocked_bridge_reported += 1;
                    }
                    Some(report.then(|| st.current_kind.clone()))
                } else {
                    st.consumed_gesture = st.active_gesture;
                    st.active_gesture = 0;
                    None
                }
            };
            if let Some(report) = blocked {
                if let Some(kind) = report {
                    self.diag(
                        "activation-blocked",
                        [
                            &name[..],
                            b" ignored: it needs a tap on the visible widget and runs in that tap's handler, one call per tap (called from the ",
                            &kind,
                            b" task)",
                        ]
                        .concat(),
                    );
                }
                return;
            }
        }
        // copyText goes to the client's clipboard: not an action, not a raw bridge call.
        if let Some(text) = &checked.copy_text {
            if let Some(d) = &delegate {
                d.on_bridge_copy_text(self, text);
            }
            return;
        }
        let Some(action) = &checked.action else { return };
        if action.kind == ActionKind::Haptic {
            let now = self.now_ms();
            let (decision, visible) = {
                let mut st = self.st.borrow_mut();
                let visible = st.visible;
                (st.haptics.decide(now, visible), visible)
            };
            if decision == HapticDecision::Report && delegate.is_some() {
                self.diag(
                    "haptic-limit",
                    if visible {
                        "haptics over 5 per burst or 10 per second, or outside a touch after a 1 s series, are dropped"
                    } else {
                        "haptics of a hidden widget are dropped"
                    },
                );
            }
            if decision != HapticDecision::Play {
                return;
            }
        }
        if let Some(d) = &delegate {
            d.on_bridge_action(self, action);
            let args = call.as_ref().map(|c| c.at(1).dump()).unwrap_or_default();
            d.on_bridge(self, &name, &args);
        }
    }

    fn storage(&self, arg: &[u8], delegate: Option<Arc<dyn EngineDelegate>>) {
        // A preview's Web Storage lives in this run only: there is no message to keep it for.
        if self.config().context.preview {
            return;
        }
        // The full areas a task changed, at most once per task after its ops (docs/sdk-v1.md §3.2).
        if arg.len() > sdk::MAX_STORAGE_COMMIT_BYTES {
            return self.reject_storage(format!("a commit of {} bytes cannot be within the quota", arg.len()));
        }
        let parsed = Json::parse_with(arg, Surrogates::Keep, twidget_json::DEFAULT_MAX_DEPTH);
        let commit = parsed.as_ref().and_then(sdk::check_storage_commit);
        let Some(commit) = commit.filter(|c| c.local.is_some() || c.session.is_some()) else {
            return self.reject_storage(format!(
                "a storage commit that is not {{ local?, session? }} objects of strings within {} bytes each; the client keeps the previous content",
                sdk::STORAGE_QUOTA_BYTES
            ));
        };
        if let Some(d) = delegate {
            if let Some(local) = &commit.local {
                d.on_storage(self, b"local", local);
            }
            if let Some(session) = &commit.session {
                d.on_storage(self, b"session", session);
            }
        }
    }

    fn reject_storage(&self, detail: String) {
        {
            let mut st = self.st.borrow_mut();
            if st.rejected_storage_reported >= sdk::MAX_REJECTION_REPORTS || self.delegate().is_none() {
                return;
            }
            st.rejected_storage_reported += 1;
        }
        self.diag("storage-rejected", detail);
    }

    fn enqueue(&self, kind: &'static [u8], arg: Vec<u8>) {
        self.post(move |e| e.input(kind, &arg));
    }

    // MARK: requests (docs/sdk-v1.md §2.14)

    /// `request-blocked` (at most 20 per widget) and the `bridge` answer event that refuses request `id`.
    fn refusal(&self, id: i64, error: &str, reason: &str, detail: Vec<u8>) -> Vec<u8> {
        let report = {
            let mut st = self.st.borrow_mut();
            let report = st.blocked_requests_reported < sdk::MAX_REJECTION_REPORTS && self.delegate().is_some();
            if report {
                st.blocked_requests_reported += 1;
            }
            report
        };
        if report {
            self.diag("request-blocked", detail);
        }
        // The shim's promise rejects with the code and a message for the reason.
        let mut event = answer_event(id);
        event.set("error", error).set("reason", reason);
        event.dump()
    }

    /// A press this task has not used yet, peeked: it is consumed only when the call goes out as the press's.
    fn pressed(&self) -> bool {
        let st = self.st.borrow();
        !is_strict_policy(&self.config().policy) || (st.active_gesture > st.consumed_gesture && st.visible)
    }

    fn consume_press(&self) {
        if is_strict_policy(&self.config().policy) {
            let mut st = self.st.borrow_mut();
            st.consumed_gesture = st.active_gesture;
            st.active_gesture = 0;
        }
    }

    fn request(&self, id: i64, data: &[u8], wallet: bool) {
        let pressed = self.pressed();
        // The gate. A refusal goes back as a `bridge` task of its own, after this one.
        let c = &self.config().context;
        let now = self.now_ms();
        let verdict = {
            let mut st = self.st.borrow_mut();
            let visible = st.visible;
            st.requests.decide(
                now,
                &RequestConditions {
                    gesture: pressed,
                    visible,
                    can_query: c.can_query,
                    background_query: c.background_query,
                    with_wallet: wallet,
                    wallet: c.wallet,
                },
            )
        };
        if let Some(error) = sdk::request_refusal(verdict) {
            let name = sdk::request_verdict_name(verdict);
            let why = if wallet && verdict == RequestVerdict::NoActivation {
                "a request for the wallet needs a tap on the visible widget, one request per tap"
            } else {
                refusal_text(verdict)
            };
            let event = self.refusal(id, error, name, format!("request refused ({name}): {why}").into_bytes());
            return self.enqueue(b"bridge", event);
        }
        if verdict == RequestVerdict::Foreground {
            self.consume_press();
        }
        // Off screen it waits for the widget to appear (set_visible); otherwise to the client now.
        if verdict == RequestVerdict::Held {
            self.st.borrow_mut().held_request = Some(HeldRequest { id, data: data.to_vec() });
        } else {
            self.deliver_request(id, data, wallet);
        }
    }

    fn wallet_call(&self, id: i64, call: &WalletCall) {
        // docs/sdk-v1.md §2.18: on a press or on the grant, never in the background; one call in flight with the
        // requests. Its outcome settles like a request's.
        let pressed = self.pressed();
        let c = &self.config().context;
        let now = self.now_ms();
        let verdict = {
            let mut st = self.st.borrow_mut();
            let visible = st.visible;
            st.requests.decide_wallet(now, &WalletConditions { gesture: pressed, visible, can_query: c.can_query, wallet: c.wallet })
        };
        if let Some(error) = sdk::request_refusal(verdict) {
            let name = sdk::request_verdict_name(verdict);
            let detail = format!("wallet.{} refused ({name}): {}", call.method_name(), wallet_refusal_text(verdict));
            let event = self.refusal(id, error, name, detail.into_bytes());
            return self.enqueue(b"bridge", event);
        }
        if verdict == RequestVerdict::Foreground {
            self.consume_press();
        }
        let respond = self.responder(id, true);
        match self.delegate() {
            Some(d) => d.on_bridge_wallet(self, call, respond),
            None => respond.unsupported(),
        }
    }

    /// The responder of request or wallet call `id`: once, from any thread, back to `answer_request` on the
    /// engine thread while the engine lives.
    fn responder(&self, id: i64, wallet: bool) -> WidgetRequestResponder {
        let once = AtomicBool::new(false);
        let shared = Arc::clone(&self.shared);
        WidgetRequestResponder {
            settle: Arc::new(move |outcome: Option<WidgetRequestOutcome>| {
                if once.swap(true, Ordering::SeqCst) {
                    return;
                }
                let alive = lock(&shared.liveness);
                if let Some(poster) = alive.as_ref() {
                    poster.post_task(move |e: &Rc<Engine>| e.answer_request(id, outcome.as_ref(), wallet));
                }
            }),
        }
    }

    fn deliver_request(&self, id: i64, data: &[u8], wallet: bool) {
        // The client decides at delivery whether it handles requests at all.
        let respond = self.responder(id, false);
        match self.delegate() {
            Some(d) => d.on_bridge_request(self, &WidgetRequest { data: data.to_vec(), wallet }, respond),
            None => respond.unsupported(),
        }
    }

    fn answer_request(&self, id: i64, outcome: Option<&WidgetRequestOutcome>, wallet: bool) {
        if self.dead() {
            return;
        }
        let Some(outcome) = outcome else {
            self.st.borrow_mut().requests.finish(None);
            let detail: &[u8] = if wallet {
                b"wallet call refused: the client handles no wallet calls (onWallet)"
            } else {
                b"request refused: the client handles no requests (onRequest)"
            };
            let event = self.refusal(id, "NOT_ALLOWED", "unsupported", detail.to_vec());
            return self.input(b"bridge", &event);
        };
        let mut event = answer_event(id);
        if outcome.failure != WidgetRequestFailure::None {
            self.st.borrow_mut().requests.finish(None);
            event.set("error", if wallet { sdk::wallet_failure_code(outcome.failure) } else { sdk::request_failure_code(outcome.failure) });
        } else if let Some(checked) = sdk::check_request_answer(&outcome.answer) {
            let now = self.now_ms();
            self.st.borrow_mut().requests.finish(Some(now));
            // only the result: the bot changes its data with setData
            event.set("result", checked.result.map_or(Json::Null, Json::from));
        } else {
            self.st.borrow_mut().requests.finish(None);
            self.diag(
                "answer-rejected",
                format!(
                    "the answer's result is not text of at most {} bytes (UTF-8, no NUL); the widget gets INVALID_ANSWER",
                    sdk::MAX_REQUEST_RESULT_BYTES
                ),
            );
            event.set("error", "INVALID_ANSWER");
        }
        self.input(b"bridge", &event.dump());
    }

    /// Blocks the engine thread until the host answers (the host must never wait for the engine).
    fn sync_query(&self, json: &[u8]) -> Vec<u8> {
        let Some(d) = self.delegate() else { return b"{\"error\":\"no host\"}".to_vec() };
        if !self.st.borrow().query_limit_hit.is_empty() {
            return b"{\"error\":\"query budget exceeded\"}".to_vec();
        }
        let t0 = self.now_seconds();
        let answer = d.on_query(self, json);
        let waited = self.now_seconds() - t0;
        let deadlines = self.deadlines();
        if let Some(dl) = &deadlines {
            // time spent waiting is not widget CPU time
            dl.deadline.set(dl.deadline.get() + waited);
        }
        // Nor is the part a served host's glue spent waiting for its embedder's platform layout time
        // (`std::clamp`'s comparisons, a NaN kept).
        let p = answer.platform_seconds;
        let platform = if p < 0.0 {
            0.0
        } else if waited < p {
            waited
        } else {
            p
        };
        let dt = waited - platform;
        let hit = {
            let mut st = self.st.borrow_mut();
            st.stats.queries = st.stats.queries.wrapping_add(1);
            st.stats.query_ms += dt * 1000.0;
            st.task_query_ms += dt * 1000.0;
            st.task_queries += 1;
            st.query_limit_hit = CpuBudget::query_limit_hit(st.task_queries, st.task_query_ms);
            st.last_binary = answer.binary;
            st.query_limit_hit
        };
        if !hit.is_empty()
            && let Some(dl) = &deadlines
        {
            // The host answered every query, but this task has asked too many or held the renderer too long:
            // interrupt it at the next poll.
            dl.cpu_deadline.set(f64::NEG_INFINITY);
        }
        answer.json
    }

    // MARK: running code

    fn kill(&self, reason: &[u8]) {
        {
            let mut st = self.st.borrow_mut();
            if st.dead {
                return;
            }
            st.dead = true;
            // never reaches the client
            st.held_request = None;
            st.pending_before_boot.clear();
            for t in st.timers.values() {
                t.cancel();
            }
            st.timers.clear();
            st.deferred.clear();
            if let Some(t) = st.wake_timer.take() {
                t.cancel();
            }
            st.cancel_trailing_stats();
        }
        if let Some(d) = self.delegate() {
            d.on_died(self, reason);
        }
    }

    fn handle_error(&self, e: &JsError, place: &[u8]) {
        let interrupted = self.deadlines().is_some_and(|d| d.interrupted.get());
        let (hit, kind, slice) = {
            let st = self.st.borrow();
            (st.query_limit_hit, st.current_kind.clone(), st.current_slice_ms)
        };
        if interrupted && !hit.is_empty() {
            return self.kill(&[&CpuBudget::query_limit_reason(hit.as_bytes(), &kind)[..], b" (", place, b")"].concat());
        }
        if interrupted {
            return self.kill(
                [format!("CPU limit exceeded: more than {} ms of continuous execution (", slice as i32).as_bytes(), place, b")"]
                    .concat()
                    .as_slice(),
            );
        }
        let text = e.text.as_bytes();
        if find(&text.to_ascii_lowercase(), b"out of memory").is_some() {
            return self.kill(format!("Memory limit exceeded ({} MB)", self.config().memory_limit_bytes >> 20).as_bytes());
        }
        if e.uncatchable && self.booted() {
            return self.kill(&[b"Engine error: ", truncate(text, 200), b" (", place, b")"].concat());
        }
        if let Some(d) = self.delegate() {
            d.on_log(self, b"error", truncate(&[b"Uncaught ", text].concat(), 4000));
        }
    }

    fn drain_jobs(&self) {
        for _ in 0..100 {
            if self.dead() {
                break;
            }
            let mut failed = None;
            let ran = {
                let mut js = self.js.borrow_mut();
                let Some(js) = js.as_mut() else { break };
                js.execute_pending_job(|e| failed = Some(e.clone()))
            };
            if let Some(e) = failed {
                self.handle_error(&e, b"promise job");
            }
            if !ran {
                break;
            }
        }
    }

    fn call_tw(&self, kind: &[u8], arg: &[u8]) -> Option<Vec<u8>> {
        if self.dead() {
            return None;
        }
        let r = {
            let mut js = self.js.borrow_mut();
            js.as_mut()?.call("__twDispatch", &[kind, arg])
        };
        match r {
            Ok(r) => r.string,
            Err(e) => {
                self.handle_error(&e, kind);
                None
            }
        }
    }

    fn has_pending_job(&self) -> bool {
        self.js.borrow().as_ref().is_some_and(NovaContext::has_pending_job)
    }

    fn settle(&self) {
        for _ in 0..20 {
            if self.dead() {
                break;
            }
            self.drain_jobs();
            if self.dead() {
                return;
            }
            let more = self.call_tw(b"endTick", b"");
            if more.as_deref() != Some(b"1") && !self.has_pending_job() {
                break;
            }
        }
    }

    /// One of our own bundles, from code compiled once per process when it can be shared, else from the source:
    /// "compiled", "source" or "failed" for the diagnostic.
    fn eval_bundle(&self, source: &[u8], filename: &str) -> &'static str {
        // Bundles of another release are refused before anything runs.
        if let Some(refusal) = BundleStamp::refusal_of(&self.resources) {
            self.diag("bundle-mismatch", refusal);
            return "failed";
        }
        let r = {
            let mut js = self.js.borrow_mut();
            js.as_mut().expect("booted").evaluate_bundle(source, filename)
        };
        match r {
            Ok(true) => "compiled",
            Ok(false) => "source",
            Err(e) => {
                self.handle_error(&e, filename.as_bytes());
                "failed"
            }
        }
    }

    fn eval_script(&self, code: &[u8], filename: &[u8], as_module: bool) {
        let r = {
            let mut js = self.js.borrow_mut();
            js.as_mut().expect("booted").evaluate(code, as_module)
        };
        if let Err(e) = r {
            self.handle_error(&e, filename);
        }
    }

    fn pump_scripts(&self) {
        for _ in 0..1000 {
            if self.dead() {
                break;
            }
            let Some(next) = self.call_tw(b"nextScript", b"") else { break };
            if next.is_empty() {
                break;
            }
            let Some(s) = Json::parse(&next).filter(Json::is_object) else { break };
            let id = s.get("id").as_int(0);
            let mut ok = true;
            self.reset_deadline();
            if let Some(src) = s.find("src").filter(|v| v.is_string()) {
                // Nothing is fetched and nothing is bundled (profile 1: no libraries): an external script never runs.
                ok = false;
                self.diag("script-skipped", [&b"external script not available: "[..], truncate(src.as_str(), 200)].concat());
            } else if let Some(code) = s.find("code").filter(|v| v.is_string()) {
                self.eval_script(code.as_str(), format!("inline-script-{id}.js").as_bytes(), s.get("type").as_str() == b"module");
            }
            if self.dead() {
                return;
            }
            self.settle();
            self.call_tw(b"scriptDone", format!("[{id},{ok}]").as_bytes());
            self.settle();
        }
    }

    /// The slice is measured in the JS thread's CPU time, so a busy machine does not kill widgets; wall-clock
    /// time is only a backstop for a thread that never runs.
    fn reset_deadline(&self) {
        let Some(d) = self.deadlines() else { return };
        let slice = self.st.borrow().current_slice_ms;
        d.interrupted.set(false);
        d.cpu_deadline.set(self.cpu_seconds() + slice / 1000.0);
        d.deadline.set(self.now_seconds() + (slice * WALL_BACKSTOP_FACTOR).max(WALL_BACKSTOP_MIN_MS) / 1000.0);
    }

    /// One task of `kind` with `slice_ms` of continuous execution; its JS time is recorded against the quota,
    /// its layout queries and the modelled cost of its ops against the main-thread window.
    fn task(&self, kind: &[u8], slice_ms: f64, body: impl FnOnce()) {
        if self.dead() {
            return;
        }
        let delegate = self.delegate();
        if let Some(d) = &delegate {
            d.on_task_started(self);
        }
        // Stopped from on_task_started: no widget code runs, and the task still ends.
        if self.dead() {
            if let Some(d) = &delegate {
                d.on_task_completed(self);
            }
            return;
        }
        {
            let mut st = self.st.borrow_mut();
            st.current_kind = kind.to_vec();
            st.current_slice_ms = slice_ms;
            st.exclude_ms = 0.0;
            st.task_query_ms = 0.0;
            st.task_queries = 0;
            st.task_ops_cost_ms = 0.0;
            st.query_limit_hit = "";
        }
        let c0 = self.cpu_seconds();
        self.reset_deadline();
        body();
        if !self.dead() {
            self.settle();
        }
        if !self.dead() {
            self.pump_scripts();
        }
        // Past the limit but done before the interrupt landed: stopped all the same.
        let hit = self.st.borrow().query_limit_hit;
        if !self.dead() && !hit.is_empty() {
            self.kill(&CpuBudget::query_limit_reason(hit.as_bytes(), kind));
        }
        let cpu = self.cpu_seconds();
        let end = self.now_ms();
        let now = self.now_seconds();
        let measure = {
            let mut st = self.st.borrow_mut();
            st.stats.tasks = st.stats.tasks.wrapping_add(1);
            let cpu_ms = (cpu - c0) * 1000.0 - st.exclude_ms;
            st.stats.js_ms += cpu_ms;
            st.budget.record(kind, cpu_ms, end);
            // what the task cost the host's main thread
            let main = st.task_query_ms + st.task_ops_cost_ms;
            st.main_budget.record(kind, main, end);
            st.stats.cpu_window_ms = st.budget.used(end);
            let measure = now - st.last_mem_check > 1.0 && !st.dead;
            if measure {
                st.last_mem_check = now;
            }
            measure
        };
        if measure {
            let used = self.js.borrow_mut().as_mut().map(NovaContext::memory_used);
            if let Some(used) = used {
                self.st.borrow_mut().stats.memory_bytes = used;
            }
        }
        self.post_stats(false);
        // Also after a death in the task: the delegate heard `on_died` first.
        if let Some(d) = &delegate {
            d.on_task_completed(self);
        }
    }

    /// At most one stats message per 200 ms, but the last state always gets out.
    fn post_stats(&self, force: bool) {
        let now = self.now_seconds();
        let send = {
            let mut st = self.st.borrow_mut();
            if force || now - st.last_stats_post > 0.2 {
                st.last_stats_post = now;
                st.cancel_trailing_stats();
                Some(st.stats)
            } else {
                #[cfg(feature = "oracle-stats-clock")]
                st.trailing_stats.get_or_insert(now + 0.2);
                #[cfg(not(feature = "oracle-stats-clock"))]
                if st.trailing_stats.is_none() {
                    st.trailing_stats = Some(self.after(0.2, |e| {
                        e.st.borrow_mut().trailing_stats = None;
                        e.post_stats(true);
                    }));
                }
                None
            }
        };
        if let Some(stats) = send
            && let Some(d) = self.delegate()
        {
            d.on_stats(self, &stats);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn canvas_commands_are_counted_as_the_cpp_counts_them() {
        assert_eq!(count_canvas_commands(br#"{"cmds":[]}"#), 0);
        assert_eq!(count_canvas_commands(br#"{"cmds":[ ]}"#), 0);
        assert_eq!(count_canvas_commands(br#"{"cmds":[["fill"],["m",1,2]]}"#), 2);
        assert_eq!(count_canvas_commands(br#"{"cmds":["a,b","c\"]"]},{"cmds":[1]}"#), 3);
        assert_eq!(count_occurrences(br#"[{"op":1},{"op":2}]"#, br#""op":"#), 2);
    }

    #[test]
    fn strtol_reads_as_c_does() {
        assert_eq!(strtol(b""), (0, 0));
        assert_eq!(strtol(b"  42x"), (42, 4));
        assert_eq!(strtol(b"-7"), (-7, 2));
        assert_eq!(strtol(b"+"), (0, 0));
        assert_eq!(strtol(b" "), (0, 0));
        let big = strtol(b"99999999999999999999");
        assert_eq!(big.1, 20);
        assert_eq!(big.0, if cfg!(windows) { i64::from(i32::MAX) } else { i64::MAX });
        assert_eq!(atoi(b"12"), 12);
    }
}
