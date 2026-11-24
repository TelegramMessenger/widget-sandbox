//! The served `WidgetHost` (widget_host.h's `ServedTag` constructor, widget_host_local.cpp's render glue and
//! what widget_host.cpp runs of it): one hosted widget's engine in process, its trusted validator and its
//! renderer, the only place where the three meet in the engine process; nothing from the engine reaches the
//! renderer without passing the validator. Its glue reports to the hosted runtime (`GlueReports`) instead of
//! the observers a local host has, and its engine's delegate is the runtime's.
//!
//! Threading, as in the C++: the host thread (`twidget-host`) makes every renderer call and keeps the glue's
//! state ([`HostState`], borrowed one step at a time, never across a call into the renderer, which may answer
//! from inside); the engine's delegate calls come on the engine's thread, and a layout query waits there for
//! the renderer's answer (3 s of the glue's own time); the validator's diagnostics come on its thread.

use std::cell::{Cell, RefCell};
use std::collections::BTreeMap;
use std::rc::{Rc, Weak as RcWeak};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock, RwLock, Weak};
use std::thread::ThreadId;
use std::time::Duration;

use twidget_bundle_stamp::SandboxResources;
use twidget_engine::{
    Engine, EngineDelegate, IntlProvider, QueryAnswer, SandboxConfig, SandboxEngine, TrustedValidator, ValidatorError,
    WidgetContentSafeArea, WidgetSafeArea, WidgetTheme, is_strict_policy, surface,
};
use twidget_js::{JsThread, Platform, Poster};
use twidget_json::Json;
use twidget_sdk_policy as sdk;
use twidget_sdk_policy::WidgetData;
use twidget_stream::REMOTE_OPS_LIMIT;

use crate::renderer::{Renderer, WidgetPaintLimit, paint_limit_code, paint_limit_from_code, paint_limit_message};

/// What a query the glue cannot ask, or one it gave up, answers.
const DEAD: &[u8] = b"{\"error\":\"dead\"}";
/// qid of the host's own post-load ack query; engine queries count up from 1.
const RENDER_ACK_QID: i32 = -1;
/// The diagnostic of an engine whose renderer messages passed the backlog bounds (its death is
/// `BACKLOG_LIMIT_REASON`).
const BACKLOG_LIMIT: &[u8] = b"backlog-limit";
/// widget_host_internal.h's `kBacklogLimitReason`.
const BACKLOG_LIMIT_REASON: &[u8] = b"the widget engine process sent more than the host keeps before its renderer is ready";
/// Bytes of the engine's renderer messages the host keeps until the renderer's `ready`, each as serialized with its
/// wrapper (`WidgetHost::kRendererBacklogBytes`, `SandboxEngine::kRemoteQueuedBytes`).
pub const RENDERER_BACKLOG_BYTES: usize = 2 * REMOTE_OPS_LIMIT;
/// Those messages counted, under the same rule (`kRendererBacklogMessages`, `SandboxEngine::kRemoteQueuedCalls`).
pub const RENDERER_BACKLOG_MESSAGES: usize = 16384;
/// Distinct kinds the diagnostics keep, "diag-kind-dropped" among them (`WidgetHost::kMaxDiagKinds`).
const MAX_DIAG_KINDS: usize = 64;
/// Where a diagnostic of a new kind past `MAX_DIAG_KINDS` is counted.
const DIAG_KIND_DROPPED: &[u8] = b"diag-kind-dropped";
/// What widget_host.cpp's `wire` throws for a factory that built nothing.
const NO_RENDERER: &str = "WidgetHost: the renderer factory returned nothing";

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

fn find(haystack: &[u8], needle: &[u8]) -> bool {
    haystack.windows(needle.len()).any(|w| w == needle)
}

/// Small base64 decoder for `binaryBase64` in a renderer result; none on malformed input (widget_host_local.cpp).
pub fn base64_decode(text: &[u8]) -> Option<Vec<u8>> {
    let value = |c: u8| -> Option<u32> {
        match c {
            b'A'..=b'Z' => Some(u32::from(c - b'A')),
            b'a'..=b'z' => Some(u32::from(c - b'a') + 26),
            b'0'..=b'9' => Some(u32::from(c - b'0') + 52),
            b'+' => Some(62),
            b'/' => Some(63),
            _ => None,
        }
    };
    let mut out = Vec::new();
    let mut acc: u32 = 0;
    let mut bits = 0;
    for &c in text {
        if c == b'=' || c == b'\n' || c == b'\r' {
            continue;
        }
        let v = value(c)?;
        acc = (acc << 6) | v;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push(((acc >> bits) & 0xFF) as u8);
        }
    }
    Some(out)
}

/// `int(count)` of a finite count of at least 0, as MSVC and clang compile it on x86-64 (`cvttsd2si`: past
/// `INT_MAX` the integer indefinite, `INT_MIN`), and as arm64's `fcvtzs` saturates. Undefined in C++ past the
/// range; the C++ the comparison runs is x86-64's.
fn c_int(count: f64) -> i32 {
    if count < 2147483648.0 {
        count as i32
    } else if cfg!(target_arch = "x86_64") || cfg!(target_arch = "x86") {
        i32::MIN
    } else {
        i32::MAX
    }
}

/// Host side of user activation (widget_host.h's `GestureCounter`): one physical pointer press (pointerdown,
/// mousedown, pointerup, mouseup, click, dblclick) is one gesture; keys never are. A click without a click count
/// (detail 0, what Enter or Space on a focused control produces) counts only as the end of an open press.
#[derive(Clone, Debug, Default)]
pub struct GestureCounter {
    number: i32,
    press_open: bool,
    last_type: Vec<u8>,
}

impl GestureCounter {
    /// A pointer press or release, or a click with a click count (detail > 0).
    pub fn is_user_activation(ev: &Json) -> bool {
        if !ev.is_object() {
            return false;
        }
        let ty = ev.get("type").as_str();
        // Pointer presses only: widgets get no keyboard (docs/decisions.md, "Profile v1").
        const TYPES: [&[u8]; 6] = [b"click", b"dblclick", b"mousedown", b"mouseup", b"pointerdown", b"pointerup"];
        if !TYPES.contains(&ty) {
            return false;
        }
        // A click made by a pointer has a click count; the click Enter or Space activates has detail 0.
        if ty == b"click" || ty == b"dblclick" {
            return ev.get("detail").as_number(0.0) > 0.0;
        }
        true
    }

    /// The gesture number for this renderer event, 0 when it is not a user activation.
    pub fn gesture_for(&mut self, ev: &Json) -> i32 {
        if !ev.is_object() {
            return 0;
        }
        let ty = ev.get("type").as_str().to_vec();
        let continues = match ty.as_slice() {
            b"mousedown" => self.last_type == b"pointerdown",
            b"pointerup" | b"mouseup" | b"click" => self.press_open,
            b"dblclick" => self.last_type == b"click",
            b"pointerdown" => false,
            _ => return 0,
        };
        if !continues && !Self::is_user_activation(ev) {
            // A keyboard click ends any chain: a dblclick after it is not the pointer's.
            self.press_open = false;
            self.last_type.clear();
            return 0;
        }
        if !continues {
            self.number = self.number.wrapping_add(1);
        }
        self.press_open = ty != b"click" && ty != b"dblclick";
        self.last_type = ty;
        self.number
    }

    /// A primary press began (the renderer's `gesture` claim): what follows is a new gesture.
    pub fn press_began(&mut self) {
        self.press_open = false;
        self.last_type.clear();
    }
}

/// What the served host's glue reports as it happens (widget_host.h's `GlueReports`, each a call when set): a
/// diagnostic it recorded, the renderer's first `ready`, the first render acknowledgement, the node count after
/// a batch when it changed.
#[derive(Default)]
pub(crate) struct GlueReports {
    pub diag: Option<TextReport>,
    pub renderer_ready: Option<Box<dyn Fn() + Send + Sync>>,
    pub rendered: Option<Box<dyn Fn() + Send + Sync>>,
    pub nodes: Option<Box<dyn Fn(i32) + Send + Sync>>,
}

/// The served host's glue thread inside its renderer's platform requests as one reading (`GlueWaited`): `at`,
/// the total time up to `at` (a wait in progress included) and whether one is in progress then, in seconds of
/// the steady clock.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct GlueWaited {
    pub at: f64,
    pub total: f64,
    pub waiting: bool,
}

pub(crate) type GlueWaits = Box<dyn Fn() -> GlueWaited + Send + Sync>;
/// A report of two texts (a diagnostic's kind and detail).
pub(crate) type TextReport = Box<dyn Fn(&[u8], &[u8]) + Send + Sync>;
/// A report of one text (a validated batch).
pub(crate) type BytesReport = Box<dyn Fn(&[u8]) + Send + Sync>;

/// What builds the served host's renderer (`RendererFactory`): the resources, the colour scheme and the width in
/// CSS px; `Err` is what the factory threw, `Ok(None)` that it built nothing.
pub(crate) type RendererFactory = Box<dyn FnOnce(Arc<SandboxResources>, &[u8], f64) -> Result<Option<Box<dyn Renderer>>, String>>;

/// What the served host's glue keeps of `WidgetDiagnostics` (the rest is the host's side's): kind → (count, its
/// first 50 details), at most 64 kinds, one of a new kind past them counted under "diag-kind-dropped" with
/// `<kind>: <detail>`; the node count; the renderer's `ready`, the widget's load and its first render, in ms from
/// the host's creation; the death; the height and the first content as the renderer reports them.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct GlueDiagnostics {
    pub diags: BTreeMap<Vec<u8>, (i32, Vec<Vec<u8>>)>,
    pub nodes: i32,
    pub loaded: bool,
    pub renderer_ready_ms: Option<f64>,
    pub loaded_ms: Option<f64>,
    pub rendered: bool,
    pub rendered_ms: Option<f64>,
    pub dead: Option<Vec<u8>>,
    pub height: f64,
    pub first_content_ms: Option<f64>,
}

impl GlueDiagnostics {
    pub fn count_diag(&self, kind: &[u8]) -> i32 {
        self.diags.get(kind).map_or(0, |e| e.0)
    }
}

/// A message the renderer gets at its `ready`; the engine's count against the backlog bounds and are dropped at
/// the cut.
struct PendingMessage {
    json: Vec<u8>,
    from_engine: bool,
}

/// One layout query of the engine's (`PendingQuery`), under the queries' lock.
struct PendingQuery {
    id: i32,
    done: bool,
    json: Vec<u8>,
    binary: Option<Vec<u8>>,
    /// The glue's waits in its renderer's platform requests in all, as they stood when it was answered.
    glue_waited: f64,
}

impl Default for PendingQuery {
    fn default() -> Self {
        PendingQuery { id: 0, done: false, json: b"null".to_vec(), binary: None, glue_waited: 0.0 }
    }
}

/// `queryMutex_`'s members: the engine's queries by number (the engine thread keeps its own until it returns) and
/// the one the renderer's answer resolves (`pendingQuery_`).
#[derive(Default)]
struct Queries {
    next: u64,
    pending: Option<u64>,
    slots: BTreeMap<u64, PendingQuery>,
}

/// The glue's state on the host thread (the C++'s host-thread members), each cell borrowed for one step.
pub(crate) struct HostState {
    renderer: RefCell<Option<Arc<dyn Renderer>>>,
    renderer_ready: Cell<bool>,
    engine_started: Cell<bool>,
    started: Cell<bool>,
    /// Set at the end of `destroy`.
    callbacks_closed: Cell<bool>,
    pending: RefCell<Vec<PendingMessage>>,
    /// Bytes and count of the engine's messages in `pending`, within the backlog bounds.
    backlog_bytes: Cell<usize>,
    backlog_messages: Cell<usize>,
    /// Set once, when the engine's message passed those bounds: nothing the engine sends afterwards has an effect.
    engine_cut: Cell<bool>,
    viewport: Cell<(f64, f64, f64)>,
    /// A profile page's checked viewport (0 in a bubble), `set_viewport_height`: the runtime's from the hosted start,
    /// then the host's.
    page_viewport: Cell<f64>,
    qid: Cell<i32>,
    ops_sent: Cell<i32>,
    /// The colour scheme ("light" | "dark") and the whole theme with its palette checked.
    theme: RefCell<Vec<u8>>,
    widget_theme: RefCell<WidgetTheme>,
    /// What the engine booted with: the glue reads its policy, its preview flag and its safe area here.
    config: RefCell<SandboxConfig>,
    /// Fed only the events the renderer marks `trusted`.
    gestures: RefCell<GestureCounter>,
}

impl HostState {
    fn renderer(&self) -> Option<Arc<dyn Renderer>> {
        self.renderer.borrow().clone()
    }
}

thread_local! {
    /// The host state of the host thread this is (one served host per such thread): what `host` runs a call
    /// inline with when it already is on that thread, as from inside a renderer call.
    static HOST_STATE: RefCell<RcWeak<HostState>> = const { RefCell::new(RcWeak::new()) };
}

/// widget_host_internal.h's `normalized`: "light" unless exactly "dark", the palette checked.
fn normalized(mut t: WidgetTheme) -> WidgetTheme {
    t.color_scheme = if t.color_scheme == b"dark" { b"dark".to_vec() } else { b"light".to_vec() };
    t.params = sdk::check_theme_params_map(&t.params);
    t
}

/// `themed`: `c` with the scheme and the palette of `theme`.
fn themed(mut c: SandboxConfig, theme: &WidgetTheme) -> SandboxConfig {
    c.theme = theme.color_scheme.clone();
    c.theme_params = theme.params.clone();
    c
}

/// What every thread of the served host reaches: the C++ WidgetHost's members but the host thread's.
pub(crate) struct Served {
    /// Where the served host's `diag` reports too.
    reports: GlueReports,
    /// The clocks (the steady clock is the glue's `std::chrono::steady_clock`).
    platform: Arc<dyn Platform>,
    poster: Poster<Rc<HostState>>,
    /// The steady clock's seconds at the host's creation, as bits.
    created_at: AtomicU64,
    diagnostics: Mutex<GlueDiagnostics>,
    changed: Condvar,
    destroyed: AtomicBool,
    /// Set by the cut (`keep_renderer_only`) on the host thread, read there and, at a layout query's entry, on
    /// the engine's thread.
    renderer_only: AtomicBool,
    /// Whether the validator works for the engine's message now (`validating_`, the engine in process).
    validating: Mutex<bool>,
    queries: Mutex<Queries>,
    query_cv: Condvar,
    engine: OnceLock<SandboxEngine>,
    /// None once the cut destroyed it.
    validator: RwLock<Option<Arc<TrustedValidator>>>,
    /// The runtime's reading of the glue's platform waits, set before `start`: a layout query's 3 s leave that
    /// time out.
    glue_waits: OnceLock<GlueWaits>,
    /// Badge text the renderer shows over a dead widget.
    stopped_label: Mutex<Vec<u8>>,
    /// The validated batches, for a host that asked for them; set before `start`.
    on_render_ops: OnceLock<BytesReport>,
}

/// The renderer's ends of `onMessage` and `onDiag` (renderer.h), from any thread: each goes to the host thread
/// (inline when already there) and is dropped once the host is gone. Cloned freely.
#[derive(Clone)]
pub struct RendererLink {
    served: Weak<Served>,
}

impl std::fmt::Debug for RendererLink {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("RendererLink")
    }
}

impl RendererLink {
    /// Renderer → host message (JSON).
    pub fn message(&self, json: &[u8]) {
        if let Some(s) = self.served.upgrade() {
            s.on_renderer(json.to_vec());
        }
    }

    /// The renderer's diagnostic: recorded on the host thread unless only the renderer is kept.
    pub fn diag(&self, kind: &[u8], detail: &[u8]) {
        if let Some(s) = self.served.upgrade() {
            let (kind, detail) = (kind.to_vec(), detail.to_vec());
            s.host(move |s, _| {
                if !s.renderer_only.load(Ordering::SeqCst) {
                    s.diag(&kind, &detail);
                }
            });
        }
    }

    /// A passed paint limit, with the count (renderer.h's `limitExceeded`): before the renderer draws that frame,
    /// and once.
    pub fn limit_exceeded(&self, code: WidgetPaintLimit, count: i32) {
        let json =
            format!("{{\"type\":\"limit\",\"code\":\"{}\",\"count\":{}}}", paint_limit_code(code), if count < 0 { 0 } else { count });
        self.message(json.as_bytes());
    }
}

impl Served {
    fn engine(&self) -> Option<&SandboxEngine> {
        self.engine.get()
    }

    fn validator(&self) -> Option<Arc<TrustedValidator>> {
        self.validator.read().unwrap_or_else(std::sync::PoisonError::into_inner).clone()
    }

    fn now(&self) -> f64 {
        self.platform.steady_seconds()
    }

    fn elapsed_ms(&self) -> f64 {
        (self.now() - f64::from_bits(self.created_at.load(Ordering::SeqCst))) * 1000.0
    }

    /// Runs `f` on the host thread (inline when already there).
    pub(crate) fn host(self: &Arc<Self>, f: impl FnOnce(&Arc<Served>, &HostState) + Send + 'static) {
        if self.poster.is_current() {
            let st = HOST_STATE.try_with(|h| h.borrow().upgrade()).ok().flatten();
            if let Some(st) = st {
                f(self, &st);
            }
            return;
        }
        let me = Arc::clone(self);
        self.poster.post_task(move |st: &Rc<HostState>| f(&me, st));
    }

    /// `host` for the work an engine's message queued: skipped once the engine is cut or the renderer kept alone.
    fn engine_work(self: &Arc<Self>, f: impl FnOnce(&Arc<Served>, &HostState) + Send + 'static) {
        self.host(move |s, st| {
            if !st.engine_cut.get() && !s.renderer_only.load(Ordering::SeqCst) {
                f(s, st);
            }
        });
    }

    fn changed(&self) {
        self.changed.notify_all();
    }

    fn diag(&self, kind: &[u8], detail: &[u8]) {
        {
            let mut d = lock(&self.diagnostics);
            let diags = &mut d.diags;
            let (key, folded) = if diags.contains_key(kind) {
                (kind.to_vec(), false)
            } else {
                // A new kind leaves the last place to the overflow kind until that kind is there; the overflow
                // kind takes it.
                let reserved = if kind == DIAG_KIND_DROPPED || diags.contains_key(DIAG_KIND_DROPPED) { 0 } else { 1 };
                let folded = diags.len() + reserved >= MAX_DIAG_KINDS;
                (if folded { DIAG_KIND_DROPPED.to_vec() } else { kind.to_vec() }, folded)
            };
            let entry = diags.entry(key).or_default();
            entry.0 = entry.0.wrapping_add(1);
            if entry.1.len() < 50 {
                entry.1.push(if folded { [kind, b": ", detail].concat() } else { detail.to_vec() });
            }
        }
        if let Some(report) = &self.reports.diag {
            report(kind, detail);
        }
        self.changed();
    }

    /// The validator's diagnostic, on its thread: inside `validated` it is work of that engine's message (in
    /// process its hold always fits).
    fn validator_diag(self: &Arc<Self>, kind: &[u8], detail: &[u8]) {
        let from_engine = *lock(&self.validating);
        let (kind, detail) = (kind.to_vec(), detail.to_vec());
        if from_engine {
            self.engine_work(move |s, _| s.diag(&kind, &detail));
        } else {
            self.host(move |s, _| s.diag(&kind, &detail));
        }
    }

    /// `validate`'s value on the engine's thread, what the validator reports meanwhile counted with the engine's
    /// message (`validator_diag`).
    fn validated<R>(&self, validate: impl FnOnce() -> R) -> R {
        *lock(&self.validating) = true;
        let result = validate();
        *lock(&self.validating) = false;
        result
    }

    fn glue_waited(&self) -> GlueWaited {
        match self.glue_waits.get() {
            Some(waits) => waits(),
            None => GlueWaited { at: self.now(), total: 0.0, waiting: false },
        }
    }

    /// The runtime, on the glue thread, outside its accounting: a wait inside a platform request ended, so a
    /// layout query waiting through it reads again.
    pub(crate) fn glue_wait_ended(&self) {
        // Under the query's lock, so a query between its reading and its wait cannot miss it.
        drop(lock(&self.queries));
        self.query_cv.notify_all();
    }

    fn resolve_query(&self, json: &[u8], binary: Option<Vec<u8>>) {
        let mut q = lock(&self.queries);
        let Some(n) = q.pending.take() else { return };
        let glue_waited = self.glue_waited().total;
        if let Some(p) = q.slots.get_mut(&n) {
            p.json = json.to_vec();
            p.binary = binary;
            p.glue_waited = glue_waited;
            p.done = true;
        }
        self.query_cv.notify_all();
    }

    // MARK: engine → host (the engine's thread)

    pub(crate) fn on_ops(self: &Arc<Self>, engine: &Engine, json: &[u8]) {
        // The engine runs only while the validator lives: the cut releases the engine first.
        let Some(validator) = self.validator() else { return };
        // Validation on the validator's thread (synchronous from here), the renderer call on the host thread.
        let clean = self.validated(|| validator.process_ops(json));
        // A counted cap passed under strict (the validator's `limit`): the widget stops as it does for memory or
        // CPU; this batch came back empty.
        if let Some(reason) = validator.stopped() {
            return engine.stop(&reason);
        }
        let nodes = validator.node_count();
        self.engine_work(move |s, st| {
            if s.destroyed.load(Ordering::SeqCst) {
                return;
            }
            if clean != b"[]" {
                if !s.engine_to_renderer(st, [&b"{\"type\":\"ops\",\"ops\":"[..], &clean, b"}"].concat()) {
                    return;
                }
                st.ops_sent.set(st.ops_sent.get().wrapping_add(1));
                if let Some(f) = s.on_render_ops.get() {
                    f(&clean);
                }
            }
            let before = std::mem::replace(&mut lock(&s.diagnostics).nodes, nodes);
            if nodes != before
                && let Some(report) = &s.reports.nodes
            {
                report(nodes);
            }
        });
    }

    pub(crate) fn on_query(self: &Arc<Self>, _engine: &Engine, json: &[u8]) -> QueryAnswer {
        let dead = || QueryAnswer { json: DEAD.to_vec(), ..QueryAnswer::default() };
        if self.destroyed.load(Ordering::SeqCst) || self.renderer_only.load(Ordering::SeqCst) {
            return dead();
        }
        let Some(validator) = self.validator() else { return dead() };
        let n = {
            let mut q = lock(&self.queries);
            q.next += 1;
            let n = q.next;
            q.slots.insert(n, PendingQuery::default());
            q.pending = Some(n);
            n
        };
        let clean = self.validated(|| {
            let mut valid = validator.validate_query(json);
            if let Some(v) = &valid
                && find(v, b"\"matchMedia\"")
                && let Some(q) = Json::parse(v)
                && q.get("query").is_string()
            {
                let mut rewritten = q.clone();
                rewritten.set("query", validator.rewrite_media_query(q.get("query").as_str()));
                valid = Some(rewritten.dump());
            }
            valid
        });
        // 3 s of the glue's own time, read before the glue can run this query: a served host's glue thread waiting
        // inside its renderer's platform requests from here on (one ahead of this query too) is the embedder's
        // time, left out here and in the widget's query budget, however late this thread waits.
        let first = self.glue_waited();
        self.host(move |s, st| {
            if s.destroyed.load(Ordering::SeqCst) || st.engine_cut.get() || s.renderer_only.load(Ordering::SeqCst) {
                return s.resolve_query(DEAD, None);
            }
            let Some(clean) = clean else { return s.resolve_query(b"{\"error\":\"invalid query\"}", None) };
            let qid = st.qid.get().wrapping_add(1);
            st.qid.set(qid);
            if let Some(p) = lock(&s.queries).slots.get_mut(&n) {
                p.id = qid;
            }
            // Not kept: the cut, or destroy(), resolved this query and no longer waits for its qid.
            s.engine_to_renderer(
                st,
                [&b"{\"type\":\"query\",\"qid\":"[..], qid.to_string().as_bytes(), b",\"query\":", &clean, b"}"].concat(),
            );
        });
        let platform_seconds = |total: f64| {
            let waited = total - first.total;
            if waited < 0.0 { 0.0 } else { waited }
        };
        let mut q = lock(&self.queries);
        loop {
            if q.slots.get(&n).is_none_or(|p| p.done) {
                break;
            }
            let now = self.glue_waited();
            let deadline = first.at + 3.0 + (now.total - first.total);
            if now.at >= deadline {
                if q.pending == Some(n) {
                    q.pending = None;
                }
                q.slots.remove(&n);
                // Its 3 s of the glue's own time are charged, the platform waits in it are not, as for an answer.
                return QueryAnswer {
                    json: b"{\"error\":\"timeout\"}".to_vec(),
                    binary: None,
                    platform_seconds: platform_seconds(now.total),
                };
            }
            // A wait in progress moves the deadline with it: its end (`glue_wait_ended`) or the answer wakes this.
            q = if now.waiting {
                self.query_cv.wait(q).unwrap_or_else(std::sync::PoisonError::into_inner)
            } else {
                let left = Duration::try_from_secs_f64(deadline - now.at).unwrap_or(Duration::from_millis(1));
                self.query_cv.wait_timeout(q, left).unwrap_or_else(std::sync::PoisonError::into_inner).0
            };
        }
        // Up to the answer's own reading (`resolve_query`), whenever this thread woke.
        let p = q.slots.remove(&n).unwrap_or_default();
        QueryAnswer { json: p.json, binary: p.binary, platform_seconds: platform_seconds(p.glue_waited) }
    }

    pub(crate) fn on_listen(self: &Arc<Self>, _engine: &Engine, json: &[u8]) {
        let Some(validator) = self.validator() else { return };
        let types = self.validated(|| validator.validate_listen(json));
        self.engine_work(move |s, st| {
            s.engine_to_renderer(st, [&b"{\"type\":\"listen\",\"types\":"[..], &types, b"}"].concat());
        });
    }

    pub(crate) fn on_prevent_default(self: &Arc<Self>, _engine: &Engine, json: &[u8]) {
        let Some(validator) = self.validator() else { return };
        let keys = self.validated(|| validator.validate_prevent_default(json));
        self.engine_work(move |s, st| {
            s.engine_to_renderer(st, [&b"{\"type\":\"preventDefault\",\"keys\":"[..], &keys, b"}"].concat());
        });
    }

    pub(crate) fn on_anim(self: &Arc<Self>, _engine: &Engine, json: &[u8]) {
        let Some(validator) = self.validator() else { return };
        let anims = self.validated(|| validator.validate_anim(json));
        if anims == b"[]" {
            return;
        }
        self.engine_work(move |s, st| {
            let now = twidget_json::format_g(s.now() * 1000.0, 17);
            s.engine_to_renderer(st, [&b"{\"type\":\"anim\",\"now\":"[..], now.as_bytes(), b",\"anims\":", &anims, b"}"].concat());
        });
    }

    pub(crate) fn on_request_frame(self: &Arc<Self>, _engine: &Engine) {
        self.engine_work(|s, st| {
            s.engine_to_renderer(st, b"{\"type\":\"requestFrame\"}".to_vec());
        });
    }

    /// WidgetHost::onLoaded: the validator's mark on the engine's thread after the loading tasks' ops were
    /// validated (text changes are rate-limited from here on), then through the host thread after the ops of the
    /// loading tasks one trusted query to the renderer, whose answer comes after every size report those ops
    /// produced.
    pub(crate) fn on_loaded(self: &Arc<Self>) {
        if let Some(validator) = self.validator() {
            validator.mark_loaded();
        }
        let loaded_ms = self.elapsed_ms();
        self.engine_work(move |s, st| {
            {
                let mut d = lock(&s.diagnostics);
                d.loaded = true;
                d.loaded_ms = Some(loaded_ms);
            }
            s.changed();
            s.engine_to_renderer(
                st,
                format!("{{\"type\":\"query\",\"qid\":{RENDER_ACK_QID},\"query\":{{\"q\":\"viewport\"}}}}").into_bytes(),
            );
        });
    }

    /// WidgetHost::onDied: the death recorded once on the host thread (after the cut, the cut's), then the
    /// renderer stays with its last render, frozen with the stopped label unless the owner terminated it.
    pub(crate) fn on_died(self: &Arc<Self>, reason: &[u8]) {
        let reason = reason.to_vec();
        self.host(move |s, st| {
            // After the cut a death that won the race on the engine's thread is the cut's.
            let effective = if st.engine_cut.get() { BACKLOG_LIMIT_REASON.to_vec() } else { reason };
            {
                let mut d = lock(&s.diagnostics);
                if d.dead.is_some() {
                    return;
                }
                d.dead = Some(effective.clone());
            }
            s.changed();
            s.resolve_query(DEAD, None);
            if effective != b"terminated" {
                let mut frozen = Json::object();
                frozen.set("type", "frozen").set("reason", lock(&s.stopped_label).as_slice()).set("fatal", true);
                s.to_renderer(st, frozen.dump());
            }
        });
    }

    /// The served host's cut once its engine died, on the host thread after the death's reports, once and never
    /// after `destroy`: the renderer's messages, the engine's queued work and every input stop at the glue, a
    /// waiting layout query answers "dead", the engine is released (its thread and Nova context go) and the
    /// validator destroyed, so only the renderer, its platform and this thread stay. The engine is dead, so its
    /// thread never waits for this one again: the join cannot hang.
    pub(crate) fn keep_renderer_only(&self) {
        if self.destroyed.load(Ordering::SeqCst) || self.renderer_only.load(Ordering::SeqCst) {
            return;
        }
        self.renderer_only.store(true, Ordering::SeqCst);
        self.resolve_query(DEAD, None);
        if let Some(engine) = self.engine() {
            engine.release();
        }
        let validator = self.validator.write().unwrap_or_else(std::sync::PoisonError::into_inner).take();
        if let Some(validator) = validator {
            validator.set_cpu_sink(None);
            drop(validator);
        }
    }

    // MARK: renderer → host (the host thread)

    fn on_renderer(self: &Arc<Self>, json: Vec<u8>) {
        self.host(move |s, st| {
            if s.destroyed.load(Ordering::SeqCst) || s.renderer_only.load(Ordering::SeqCst) {
                return;
            }
            if let Some(msg) = Json::parse(&json) {
                s.handle_renderer_message(st, &msg);
            }
        });
    }

    fn handle_renderer_message(self: &Arc<Self>, st: &HostState, msg: &Json) {
        let Some(engine) = self.engine() else { return };
        let ty = msg.get("type").as_str();
        match ty {
            b"ready" => {
                if st.renderer_ready.get() {
                    return;
                }
                st.renderer_ready.set(true);
                lock(&self.diagnostics).renderer_ready_ms = Some(self.elapsed_ms());
                if let Some(report) = &self.reports.renderer_ready {
                    report();
                }
                let queued = std::mem::take(&mut *st.pending.borrow_mut());
                st.backlog_bytes.set(0);
                st.backlog_messages.set(0);
                let renderer = st.renderer();
                for m in &queued {
                    if let Some(r) = &renderer {
                        r.send(&m.json);
                    }
                }
                // Never once cut: an observer of the cut's diagnostic may announce the renderer before the stop is
                // posted.
                if !st.engine_started.get() && !st.engine_cut.get() {
                    st.engine_started.set(true);
                    engine.start();
                }
            }
            b"event" => {
                // A preview takes no input: a tap on it selects the result (the client's), and the widget gets
                // nothing (§2.16).
                if st.config.borrow().context.preview {
                    return;
                }
                if let Some(ev) = msg.find("ev") {
                    // Any pointerdown begins a press, trusted or not.
                    if ev.get("type").is_string() && ev.get("type").as_str() == b"pointerdown" {
                        st.gestures.borrow_mut().press_began();
                    }
                    // Only input the renderer saw as real is a gesture.
                    let gesture = if msg.get("trusted").as_bool(false) { st.gestures.borrow_mut().gesture_for(ev) } else { 0 };
                    if gesture != 0 {
                        engine.activation_event(&ev.dump(), gesture);
                    } else {
                        engine.event(&ev.dump());
                    }
                }
            }
            b"result" => {
                if !msg.get("qid").is_number() {
                    return;
                }
                let id = msg.get("qid").as_int(0);
                if id == RENDER_ACK_QID {
                    let first = {
                        let mut d = lock(&self.diagnostics);
                        if d.rendered {
                            false
                        } else {
                            d.rendered = true;
                            d.rendered_ms = Some(self.elapsed_ms());
                            true
                        }
                    };
                    if !first {
                        return;
                    }
                    self.changed();
                    if let Some(report) = &self.reports.rendered {
                        report();
                    }
                    return;
                }
                {
                    let q = lock(&self.queries);
                    let current = q.pending.and_then(|n| q.slots.get(&n)).map(|p| p.id);
                    if current != Some(id) {
                        return;
                    }
                }
                let binary = if msg.get("binaryBase64").is_string() { base64_decode(msg.get("binaryBase64").as_str()) } else { None };
                let result = msg.find("result").map_or_else(|| b"null".to_vec(), Json::dump);
                self.resolve_query(&result, binary);
            }
            b"size" => {
                // A bubble's height is the content's in [50, 600], a profile page's at least its viewport (`surface::height`).
                let place = st.config.borrow().context.surface;
                let height = surface::height(place, msg.get("height").as_number(0.0), st.page_viewport.get());
                let notify = {
                    let mut d = lock(&self.diagnostics);
                    let mut notify = false;
                    if d.first_content_ms.is_none() && st.ops_sent.get() > 0 {
                        d.first_content_ms = Some(self.elapsed_ms());
                        notify = true;
                    }
                    if height != d.height {
                        d.height = height;
                        notify = true;
                    }
                    notify
                };
                if notify {
                    self.changed();
                }
                let (vw, vh, vdpr) = st.viewport.get();
                let w = msg.get("innerWidth").as_number(vw);
                let h = msg.get("innerHeight").as_number(vh);
                let dpr = msg.get("dpr").as_number(vdpr);
                if w != vw || h != vh || dpr != vdpr {
                    st.viewport.set((w, h, dpr));
                    engine.viewport(w, h, dpr);
                }
            }
            b"resize" => {
                if let Some(entries) = msg.find("entries") {
                    engine.resize(&entries.dump());
                }
            }
            b"frame" => engine.frame(msg.get("time").as_number(0.0)),
            b"media" => {
                // OS-level change inside the renderer: the host theme is authoritative, just re-notify.
                let params = st.widget_theme.borrow().params.clone();
                engine.theme(&st.theme.borrow().clone(), &params);
            }
            b"violation" => self.diag(b"renderer-violation", msg.get("detail").as_str()),
            b"limit" => {
                // Past a paint limit: the renderer drew the frame without the effects, and the widget stops as for
                // memory or CPU. compat counts nothing.
                let limit = if msg.get("code").is_string() { paint_limit_from_code(msg.get("code").as_str()) } else { None };
                let count = if msg.get("count").is_number() { msg.get("count").as_number(0.0) } else { -1.0 };
                let Some(limit) = limit.filter(|_| count.is_finite() && count >= 0.0) else {
                    return self.diag(b"renderer-violation", b"a limit report with an unknown code or no count");
                };
                if !is_strict_policy(&st.config.borrow().policy) {
                    return;
                }
                if lock(&self.diagnostics).dead.is_some() {
                    return;
                }
                let reason = paint_limit_message(limit, c_int(count));
                self.diag(paint_limit_code(limit).as_bytes(), reason.as_bytes());
                engine.stop(reason.as_bytes());
            }
            b"release" => {
                // The primary press ended (pointerup or pointercancel; renderer-made): the haptic gate's 5 s touch
                // window starts.
                engine.press_ended();
            }
            b"gesture" => {
                // A primary press began (renderer-made: the widget cannot fake it): haptics are free while it lasts,
                // and 5 s after. The served host has no gesture observers: the claim stays here.
                engine.press_began();
                st.gestures.borrow_mut().press_began();
            }
            _ => {}
        }
    }

    /// The safe area's message, `frozen` and the host's own messages: queued until the renderer is ready.
    fn to_renderer(&self, st: &HostState, json: Vec<u8>) {
        if self.destroyed.load(Ordering::SeqCst) || self.renderer_only.load(Ordering::SeqCst) {
            return;
        }
        let Some(renderer) = st.renderer() else { return };
        if st.renderer_ready.get() {
            renderer.send(&json);
        } else {
            st.pending.borrow_mut().push(PendingMessage { json, from_engine: false });
        }
    }

    /// `to_renderer` for the engine's message, kept within the backlog bounds before `ready`: false when it is not
    /// sent or kept (destroyed, cut, the renderer kept alone, or the message that cuts).
    fn engine_to_renderer(&self, st: &HostState, json: Vec<u8>) -> bool {
        if self.destroyed.load(Ordering::SeqCst) || st.engine_cut.get() || self.renderer_only.load(Ordering::SeqCst) {
            return false;
        }
        let Some(renderer) = st.renderer() else { return false };
        if st.renderer_ready.get() {
            renderer.send(&json);
            return true;
        }
        if st.backlog_messages.get() < RENDERER_BACKLOG_MESSAGES && json.len() <= RENDERER_BACKLOG_BYTES - st.backlog_bytes.get() {
            st.backlog_bytes.set(st.backlog_bytes.get() + json.len());
            st.backlog_messages.set(st.backlog_messages.get() + 1);
            st.pending.borrow_mut().push(PendingMessage { json, from_engine: true });
            return true;
        }
        st.engine_cut.set(true);
        st.pending.borrow_mut().retain(|m| !m.from_engine);
        st.backlog_bytes.set(0);
        st.backlog_messages.set(0);
        st.ops_sent.set(0);
        self.diag(BACKLOG_LIMIT, BACKLOG_LIMIT_REASON);
        // A query waiting, or the one whose message passed the bounds, would hold the engine's thread for its 3 s
        // before the stop runs.
        self.resolve_query(DEAD, None);
        if let Some(engine) = self.engine() {
            engine.stop(BACKLOG_LIMIT_REASON);
        }
        false
    }

    /// `{"type":"theme","theme":{"colorScheme","params","button","text"}}` of the current theme.
    fn theme_message(st: &HostState) -> Vec<u8> {
        let wt = st.widget_theme.borrow();
        let mut theme = Json::object();
        theme
            .set("colorScheme", wt.color_scheme.as_slice())
            .set("params", sdk::theme_params_json(&wt.params))
            .set("button", wt.button.json())
            .set("text", wt.text.json());
        let mut msg = Json::object();
        msg.set("type", "theme").set("theme", theme);
        msg.dump()
    }

    /// `{"type":"safeArea","inset":{…}}`: the renderer core checks the values and sets the variables.
    fn safe_area_message(area: &WidgetContentSafeArea) -> Vec<u8> {
        let mut msg = Json::object();
        msg.set("type", "safeArea").set("inset", area.json());
        msg.dump()
    }

    /// `{"type":"deviceSafeArea","inset":{…}}`: the renderer core checks the values against the place.
    fn device_safe_area_message(area: &WidgetSafeArea) -> Vec<u8> {
        let mut msg = Json::object();
        msg.set("type", "deviceSafeArea").set("inset", area.json());
        msg.dump()
    }

    /// `{"type":"surface","surface":"message" | "profile","viewportHeight":…}`: the place and the page's checked
    /// viewport (0 in a bubble).
    fn surface_message(st: &HostState) -> Vec<u8> {
        let mut msg = Json::object();
        msg.set("type", "surface")
            .set("surface", sdk::surface_name(st.config.borrow().context.surface))
            .set("viewportHeight", st.page_viewport.get());
        msg.dump()
    }

    fn local_start(self: &Arc<Self>, st: &HostState) {
        // Queued until the renderer is ready, so the variables are its first message, before any ops.
        self.to_renderer(st, Self::theme_message(st));
        // The place first: it bounds both safe-area layers and sets a page's viewport.
        self.to_renderer(st, Self::surface_message(st));
        let (area, device) = {
            let config = st.config.borrow();
            (config.content_safe_area, config.safe_area)
        };
        self.to_renderer(st, Self::safe_area_message(&area));
        self.to_renderer(st, Self::device_safe_area_message(&device));
        // Under strict the renderer counts heavy boxes and reports `limit` past them; compat counts nothing.
        let strict = is_strict_policy(&st.config.borrow().policy);
        self.to_renderer(st, format!("{{\"type\":\"policy\",\"policy\":\"{}\"}}", if strict { "strict" } else { "compat" }).into_bytes());
        if let Some(renderer) = st.renderer() {
            renderer.load();
        }
    }

    fn local_set_theme(self: &Arc<Self>, st: &HostState) {
        // A served host kept for its renderer alone has no validator or engine to re-theme.
        if self.renderer_only.load(Ordering::SeqCst) {
            return;
        }
        let theme = st.theme.borrow().clone();
        if let Some(renderer) = st.renderer() {
            renderer.set_theme(&theme);
        }
        // Before the start no message goes ahead of the start's: its theme message is this theme, and no node is
        // there to re-theme yet.
        let started = st.started.get();
        if started {
            self.to_renderer(st, Self::theme_message(st));
        }
        // Past the cut the renderer never gets the nodes the re-themed stylesheets would name.
        if !st.engine_cut.get() {
            // A validated batch too: the asked-for batches see every batch the renderer gets.
            let clean = self.validator().map_or_else(|| b"[]".to_vec(), |v| v.set_theme(&theme));
            if started {
                self.to_renderer(st, [&b"{\"type\":\"ops\",\"ops\":"[..], &clean, b"}"].concat());
                if clean != b"[]"
                    && let Some(f) = self.on_render_ops.get()
                {
                    f(&clean);
                }
            }
        }
        let params = st.widget_theme.borrow().params.clone();
        if let Some(engine) = self.engine() {
            engine.theme(&theme, &params);
        }
    }
}

/// The served host (widget_host.h's `ServedTag` constructor): local, from the host's config as its hosted start
/// carried it (`inner_width` the renderer's width) with the scheme and the palette of its theme, its engine built
/// here directly with the runtime's delegate, its glue's reports to the runtime. Owned by the hosted runtime,
/// dropped on its owner's thread, never on one of its own.
pub(crate) struct ServedHost {
    /// Dropped after both threads were joined, on the thread that drops the host.
    renderer: Option<Arc<dyn Renderer>>,
    served: Arc<Served>,
    host_thread: JsThread<Rc<HostState>>,
}

fn validator_error(e: ValidatorError) -> String {
    match e {
        ValidatorError::Thread(why) => why,
        ValidatorError::Js(e) => e.text,
    }
}

impl ServedHost {
    /// The host thread first, then the validator (a diagnostic it reports while it is built, `bundle-mismatch`,
    /// reaches the host thread at once and is reported even when a later part fails), the renderer, the engine,
    /// then the wiring; what failed, as the C++ constructor throws it, with every part built so far gone.
    pub(crate) fn new(
        html: Vec<u8>,
        theme: WidgetTheme,
        config: SandboxConfig,
        resources: Arc<SandboxResources>,
        renderers: RendererFactory,
        intl: Arc<dyn IntlProvider>,
        delegate: impl FnOnce(Weak<Served>) -> Arc<dyn EngineDelegate>,
        reports: GlueReports,
        platform: Arc<dyn Platform>,
    ) -> Result<ServedHost, String> {
        let scheme = if theme.color_scheme == b"dark" { b"dark".to_vec() } else { b"light".to_vec() };
        let widget_theme = normalized(theme);
        let config = themed(config, &widget_theme);
        let state = (scheme.clone(), widget_theme, config.clone());
        let host_thread = JsThread::new("twidget-host", move || {
            let (theme, widget_theme, config) = state;
            let viewport = (config.inner_width, config.inner_height, config.device_pixel_ratio);
            let st = Rc::new(HostState {
                renderer: RefCell::new(None),
                renderer_ready: Cell::new(false),
                engine_started: Cell::new(false),
                started: Cell::new(false),
                callbacks_closed: Cell::new(false),
                pending: RefCell::new(Vec::new()),
                backlog_bytes: Cell::new(0),
                backlog_messages: Cell::new(0),
                engine_cut: Cell::new(false),
                viewport: Cell::new(viewport),
                page_viewport: Cell::new(0.0),
                qid: Cell::new(0),
                ops_sent: Cell::new(0),
                theme: RefCell::new(theme),
                widget_theme: RefCell::new(widget_theme),
                config: RefCell::new(config),
                gestures: RefCell::new(GestureCounter::default()),
            });
            HOST_STATE.with(|h| *h.borrow_mut() = Rc::downgrade(&st));
            st
        })?;
        let served = Arc::new(Served {
            reports,
            platform: Arc::clone(&platform),
            poster: host_thread.poster(),
            created_at: AtomicU64::new(platform.steady_seconds().to_bits()),
            diagnostics: Mutex::new(GlueDiagnostics::default()),
            changed: Condvar::new(),
            destroyed: AtomicBool::new(false),
            renderer_only: AtomicBool::new(false),
            validating: Mutex::new(false),
            queries: Mutex::new(Queries::default()),
            query_cv: Condvar::new(),
            engine: OnceLock::new(),
            validator: RwLock::new(None),
            glue_waits: OnceLock::new(),
            stopped_label: Mutex::new(b"Widget stopped".to_vec()),
            on_render_ops: OnceLock::new(),
        });
        let weak = Arc::downgrade(&served);
        let on_diag: twidget_engine::DiagSink = Arc::new(move |kind: &[u8], detail: &[u8]| {
            if let Some(s) = weak.upgrade() {
                s.validator_diag(kind, detail);
            }
        });
        let validator =
            TrustedValidator::new(&scheme, Arc::clone(&resources), Some(on_diag), Arc::clone(&platform)).map_err(validator_error)?;
        let renderer = renderers(Arc::clone(&resources), &scheme, config.inner_width)?;
        let engine = SandboxEngine::new(html, config.clone(), resources, intl, platform)?;
        // `createdAt_` is the C++'s last member: its clock reading comes after the engine was built.
        served.created_at.store(served.now().to_bits(), Ordering::SeqCst);
        // wire(): the validator's policy and CPU sink, the engine's delegate, the renderer's callbacks.
        let Some(renderer) = renderer else { return Err(NO_RENDERER.to_string()) };
        if is_strict_policy(&config.policy) {
            validator.set_policy(b"strict");
        }
        // The validator's time on the widget's messages counts against its JS quota.
        let weak = Arc::downgrade(&served);
        validator.set_cpu_sink(Some(Box::new(move |ms| {
            if let Some(engine) = weak.upgrade().as_ref().and_then(|s| s.engine()) {
                engine.charge_validator(ms);
            }
        })));
        engine.set_delegate(delegate(Arc::downgrade(&served)));
        let _ = served.engine.set(engine);
        *served.validator.write().unwrap_or_else(std::sync::PoisonError::into_inner) = Some(Arc::new(validator));
        let renderer: Arc<dyn Renderer> = Arc::from(renderer);
        let link = RendererLink { served: Arc::downgrade(&served) };
        // The renderer is the host thread's before its messages can reach that thread.
        let given = Arc::clone(&renderer);
        host_thread.post_task(move |st: &Rc<HostState>| *st.renderer.borrow_mut() = Some(given));
        renderer.connect(link);
        Ok(ServedHost { renderer: Some(renderer), served, host_thread })
    }

    pub(crate) fn served(&self) -> &Arc<Served> {
        &self.served
    }

    /// The host thread's id, for the platform's accounting of its waits.
    pub(crate) fn host_thread_id(&self) -> Option<ThreadId> {
        self.host_thread.sync(|_| std::thread::current().id())
    }

    pub(crate) fn set_stopped_label(&self, label: Vec<u8>) {
        *lock(&self.served.stopped_label) = label;
    }

    pub(crate) fn set_on_render_ops(&self, f: BytesReport) {
        let _ = self.served.on_render_ops.set(f);
    }

    pub(crate) fn set_glue_waits(&self, waits: GlueWaits) {
        let _ = self.served.glue_waits.set(waits);
    }

    /// Loads the renderer; the engine boots once the renderer announced itself (`ready`).
    pub(crate) fn start(&self) {
        self.served.host(|s, st| {
            if st.started.get() || s.destroyed.load(Ordering::SeqCst) {
                return;
            }
            st.started.set(true);
            s.local_start(st);
        });
    }

    /// The chat's theme changed: stylesheets are re-sanitised against the scheme, the renderer gets the scheme and
    /// the variables, the widget `colorScheme`, `themeParams` and `themeChanged` in one task.
    pub(crate) fn set_theme(&self, theme: WidgetTheme) {
        let theme = normalized(theme);
        self.served.host(move |s, st| {
            if s.destroyed.load(Ordering::SeqCst) {
                return;
            }
            *st.theme.borrow_mut() = theme.color_scheme.clone();
            *st.widget_theme.borrow_mut() = theme;
            s.local_set_theme(st);
        });
    }

    pub(crate) fn set_content_safe_area(&self, area: WidgetContentSafeArea) {
        self.served.host(move |s, st| {
            if s.destroyed.load(Ordering::SeqCst) || s.renderer_only.load(Ordering::SeqCst) {
                return;
            }
            st.config.borrow_mut().content_safe_area = area;
            if st.started.get() {
                s.to_renderer(st, Served::safe_area_message(&area));
            }
            if let Some(engine) = s.engine() {
                engine.content_safe_area(area);
            }
        });
    }

    /// The device's edges over the widget changed with the screen's layout: the renderer gets the
    /// `--tg-safe-area-inset-*` variables and the widget `safeAreaInset` and `safeAreaChanged`, from this one call.
    /// Always 0 in a bubble (the renderer core and the shim check the values against the place).
    pub(crate) fn set_safe_area(&self, area: WidgetSafeArea) {
        self.served.host(move |s, st| {
            if s.destroyed.load(Ordering::SeqCst) || s.renderer_only.load(Ordering::SeqCst) {
                return;
            }
            st.config.borrow_mut().safe_area = area;
            if st.started.get() {
                s.to_renderer(st, Served::device_safe_area_message(&area));
            }
            if let Some(engine) = s.engine() {
                engine.safe_area(area);
            }
        });
    }

    /// A profile page's viewport changed with the screen's layout: `--tg-viewport-height`, and the page's least
    /// height; ignored (0) in a bubble. The renderer gets the place again once started.
    pub(crate) fn set_viewport_height(&self, height: f64) {
        self.served.host(move |s, st| {
            if s.destroyed.load(Ordering::SeqCst) || s.renderer_only.load(Ordering::SeqCst) {
                return;
            }
            let place = st.config.borrow().context.surface;
            st.page_viewport.set(surface::check_viewport_height(place, height));
            if st.started.get() {
                s.to_renderer(st, Served::surface_message(st));
            }
        });
    }

    pub(crate) fn set_data(&self, data: &WidgetData) {
        if let Some(engine) = self.served.engine() {
            engine.set_data(data);
        }
    }

    pub(crate) fn set_visible(&self, visible: bool) {
        if let Some(engine) = self.served.engine() {
            engine.set_visible(visible);
        }
    }

    pub(crate) fn stop_engine(&self, reason: &[u8]) {
        if let Some(engine) = self.served.engine() {
            engine.stop(reason);
        }
    }

    pub(crate) fn terminate_engine(&self) {
        if let Some(engine) = self.served.engine() {
            engine.terminate();
        }
    }

    pub(crate) fn probe(&self, code: &[u8]) -> Vec<u8> {
        match self.served.engine() {
            Some(engine) => engine.probe(code),
            None => b"ERROR no runtime".to_vec(),
        }
    }

    /// Waits until everything queued on the host thread before has run.
    pub(crate) fn on_host_thread(&self) {
        self.host_thread.sync(|_| ());
    }

    /// Runs `f` on the host thread (inline when already there).
    pub(crate) fn host(&self, f: impl FnOnce() + Send + 'static) {
        self.served.host(move |_, _| f());
    }

    /// Stops the engine and the renderer, idempotent, never from the host thread: a waiting layout query answers
    /// "dead" before the engine is stopped, then the renderer's `destroy` on the host thread.
    fn destroy(&self) {
        if self.served.destroyed.swap(true, Ordering::SeqCst) {
            return;
        }
        self.served.resolve_query(DEAD, None);
        self.terminate_engine();
        self.host_thread.sync(|st: &Rc<HostState>| {
            if let Some(renderer) = st.renderer() {
                renderer.destroy();
            }
            st.callbacks_closed.set(true);
        });
    }
}

impl Drop for ServedHost {
    /// `~WidgetHost`: destroyed, then no delegate call once the engine's thread is gone, and the host thread
    /// drained while everything lives; the renderer goes after both.
    fn drop(&mut self) {
        self.destroy();
        if let Some(engine) = self.served.engine() {
            engine.release();
        }
        self.host_thread.join();
        drop(self.renderer.take());
    }
}
