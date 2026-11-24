//! The engine process's end of a stream (`remote_stream_engine.cpp`): the loop `SandboxRemoteEngineEnd::serve`
//! runs, and `serveHosting` through [`serve_hosting`], on the thread that reads, each runtime's server or hosted
//! runtime writing its calls as frames through its sink.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};

use twidget_bundle_stamp::SandboxResources;
use twidget_engine::{QueryAnswer, SandboxConfig, SandboxHostedStart, SandboxStats, WidgetContentSafeArea, WidgetSafeArea, WidgetTheme};
use twidget_js::{JsThread, Platform};
use twidget_sdk_policy::WidgetData;
use twidget_stream::{
    Field, Frame, FrameReader, Kind, MAX_FRAME_BYTES, MAX_OPENING_BYTES, MAX_STAMP_BYTES, REMOTE_MESSAGE_LIMIT, REMOTE_OPS_LIMIT,
    REMOTE_PLATFORM_LIMIT, Side, Stream, VERSION, cut, encode, kind_on,
};

use crate::coding::{
    SandboxRemoteOutcome, decode_config, decode_data, decode_device_safe_area, decode_hosted_start, decode_outcome, decode_safe_area,
    decode_theme, decode_theme_params, encode_stats,
};
use crate::server::{SandboxRemoteServer, SandboxRemoteSink};

/// The engine's side of one hosted widget (`detail::HostedRuntime`, remote_hosted.cpp's `SandboxHostedRuntime`),
/// with `SandboxRemoteServer`'s rules: the calls but `probe` come one at a time in the host's order (the reader)
/// and never wait for the widget, the inputs before `start` are kept and those after `close` dropped, the owner
/// drops it on its own thread after its last call.
pub trait HostedRuntime: Send + Sync {
    /// Builds the widget's engine, validator and renderer and starts it, once, never after `close`; a build that
    /// fails reports the widget's death to the sink and closes the runtime.
    fn start(&self, html: &[u8], config: SandboxConfig, start: SandboxHostedStart);
    fn theme(&self, theme: WidgetTheme);
    fn content_safe_area(&self, area: WidgetContentSafeArea);
    /// The device's edges (the served host's `set_safe_area`).
    fn safe_area(&self, area: WidgetSafeArea);
    /// A profile page's viewport as the host checked it (the served host's `set_viewport_height`).
    fn viewport_height(&self, height: f64);
    fn set_data(&self, data: WidgetData);
    fn set_visible(&self, visible: bool);
    /// The host stopped the widget: its engine dies with `reason`.
    fn stop(&self, reason: &[u8]);
    /// Settles the forwarded request or wallet call `id`, once.
    fn answer(&self, id: i64, outcome: &SandboxRemoteOutcome);
    /// The widget's probe, a barrier both ways: it runs after the engine work the inputs before it queued
    /// through the glue's thread, and what its glue sent before it is on the sink before it returns; "ERROR no
    /// runtime" before `start`, after `close` and once its engine died. Never from the thread answering the
    /// runtime's synchronous calls.
    fn probe(&self, code: &[u8]) -> Vec<u8>;
    /// A platform message for the renderer, on the glue's host thread in order.
    fn platform(&self, message: &[u8]);
    /// The client is gone: the kept inputs dropped, the forwarded calls refused, the platform closed (its
    /// requests answer none) and the engine terminated without waiting. Idempotent.
    fn close(&self);
}

/// What a hosting stream's engine end makes for each runtime it opens, on its reader: the runtime sending its
/// widget's calls to the sink, its widget built from the resources (the hosting opening's).
pub type HostedRuntimes<'a> = &'a dyn Fn(Arc<dyn SandboxRemoteSink>, Arc<SandboxResources>) -> Arc<dyn HostedRuntime>;

/// What a query that gets no answer returns to the engine, as a client's dead engine answers it.
const DEAD_QUERY: &[u8] = b"{\"error\":\"dead\"}";
/// The death of a runtime whose `widget.probe` cannot start, before the refusal itself.
const PROBE_UNSTARTED: &str = "the engine process could not start the widget's probe thread";
/// Why serving ends when `widget.reaper` cannot start, before the refusal itself.
const REAPER_UNSTARTED: &str = "the engine process could not start the thread that destroys closed runtimes";
/// Bytes of the host's text a refusal quotes.
const QUOTED_BYTES: usize = 64;

fn message(s: &[u8]) -> &[u8] {
    cut(s, REMOTE_MESSAGE_LIMIT)
}

fn platform_text(s: &[u8]) -> &[u8] {
    cut(s, REMOTE_PLATFORM_LIMIT)
}

fn in_quotes(s: &[u8]) -> Vec<u8> {
    [&b"\""[..], &s[..s.len().min(QUOTED_BYTES)], if s.len() > QUOTED_BYTES { b"...\"" } else { b"\"" }].concat()
}

fn undecodable(f: &Frame) -> Vec<u8> {
    format!("{} with a form that does not decode", kind_on(f)).into_bytes()
}

/// Why the engine's end refuses a first frame, empty for an opening it serves: another kind (`HostingOpening`
/// too unless `hosting`), another version or build, a stamp past `MAX_STAMP_BYTES` or of more than one line
/// (never cut into another stamp); a hosting opening's only stamp is the renderer's, its validator checks the
/// whole bundle's own.
fn refusal_of(f: &Frame, build: &[u8], hosting: bool) -> Vec<u8> {
    let hosted = hosting && f.kind == Kind::HostingOpening;
    if f.kind != Kind::Opening && !hosted {
        return format!("kind {} first, not the opening", f.kind as u8).into_bytes();
    }
    if f.i64 != VERSION {
        return format!("stream version {}, this end's is {VERSION}", f.i64).into_bytes();
    }
    if f.str[0] != build {
        return [&b"build "[..], &in_quotes(f.str[0]), b", this end's is ", &in_quotes(build)].concat();
    }
    let stamps: &[(&str, &[u8])] = if hosted { &[("renderer", f.str[3])] } else { &[("validator", f.str[2]), ("renderer", f.str[3])] };
    for &(name, stamp) in stamps {
        if stamp.len() > MAX_STAMP_BYTES {
            return format!("a {name} stamp of {} bytes, past the bound of {MAX_STAMP_BYTES}", stamp.len()).into_bytes();
        }
        if stamp.contains(&b'\n') {
            return format!("a {name} stamp of more than one line").into_bytes();
        }
    }
    Vec::new()
}

/// The runtimes' resources from the opening alone, no file read: the sandbox bundle, then the validator's and
/// renderer's stamp lines for the stamp's checks at boot, or a hosting opening's whole validator bundle and the
/// renderer's stamp line.
fn resources_of(f: &Frame) -> SandboxResources {
    SandboxResources { sandbox_bundle: f.str[1].to_vec(), validator_bundle: f.str[2].to_vec(), renderer_bundle: f.str[3].to_vec() }
}

/// The host kinds only a hosting stream takes: a hosted widget's start, its whole theme, its renderer's platform
/// traffic and a profile page's viewport (the glue's, which only a hosted widget has in the engine process).
fn hosted_kind(kind: Kind) -> bool {
    matches!(kind, Kind::HostedStart | Kind::HostedTheme | Kind::Platform | Kind::PlatformAnswer | Kind::HostedViewportHeight)
}

/// The host kinds a hosting stream takes on a runtime: its own and those a hosted widget shares with an engine;
/// the engine's start, input, theme, presses, validator time and query answers are made inside the engine
/// process.
fn hosting_takes(kind: Kind) -> bool {
    match kind {
        Kind::Open
        | Kind::SafeArea
        | Kind::DeviceSafeArea
        | Kind::Data
        | Kind::Visible
        | Kind::Stop
        | Kind::Answer
        | Kind::Probe
        | Kind::IntlAnswer
        | Kind::Close => true,
        _ => hosted_kind(kind),
    }
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// The engine's writes: whole frames, one at a time under one lock; `break_off` takes no lock, so the end of the
/// stream never waits for a write blocked inside it.
struct Output {
    stream: Arc<dyn Stream>,
    writing: Mutex<()>,
    broken: AtomicBool,
}

impl Output {
    /// A frame of `kind` on `runtime`, encoded before the lock: false once the stream broke, a failed write
    /// breaking it.
    fn send(&self, runtime: u32, kind: Kind, fields: &[Field]) -> bool {
        if self.broken.load(Ordering::SeqCst) {
            return false;
        }
        let Some(frame) = encode(runtime, kind, MAX_FRAME_BYTES, fields) else { return false };
        let _writing = lock(&self.writing);
        if self.broken.load(Ordering::SeqCst) {
            return false;
        }
        if self.stream.write(&frame) {
            return true;
        }
        self.break_off();
        false
    }

    /// Every later `send` fails, then the stream's shutdown wakes a write and the read blocked in it.
    fn break_off(&self) {
        self.broken.store(true, Ordering::SeqCst);
        self.stream.shutdown();
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Wait {
    None,
    Query,
    Intl,
    Platform,
}

struct SlotState {
    waiting: Wait,
    answer: Option<QueryAnswer>,
}

/// One synchronous request of a runtime's at a time (its engine's query or Intl request; a hosted renderer's
/// platform request in a slot of its own): marked before it is written, filled by the reader with an answer of
/// its kind only, given up once killed (the runtime closed or the stream ended). Its lock is never held across a
/// write or a call out.
struct SyncSlot {
    state: Mutex<SlotState>,
    changed: Condvar,
    killed: AtomicBool,
}

impl SyncSlot {
    fn new() -> SyncSlot {
        SyncSlot {
            state: Mutex::new(SlotState { waiting: Wait::None, answer: None }),
            changed: Condvar::new(),
            killed: AtomicBool::new(false),
        }
    }

    /// Before the request is written: false once killed.
    fn begin(&self, kind: Wait) -> bool {
        let mut st = lock(&self.state);
        if self.killed.load(Ordering::SeqCst) {
            return false;
        }
        st.waiting = kind;
        st.answer = None;
        true
    }

    /// The request was not written: nothing waits.
    fn cancel(&self) {
        lock(&self.state).waiting = Wait::None;
    }

    /// The answer to the request `begin` marked, none once killed without one.
    fn wait(&self) -> Option<QueryAnswer> {
        let mut st = lock(&self.state);
        while st.answer.is_none() && !self.killed.load(Ordering::SeqCst) {
            st = self.changed.wait(st).unwrap_or_else(std::sync::PoisonError::into_inner);
        }
        st.waiting = Wait::None;
        st.answer.take()
    }

    /// The reader's: an answer for a request of `kind` that waits for one, dropped otherwise.
    fn fill(&self, kind: Wait, answer: QueryAnswer) {
        {
            let mut st = lock(&self.state);
            if self.killed.load(Ordering::SeqCst) || st.waiting != kind || st.answer.is_some() {
                return;
            }
            st.answer = Some(answer);
        }
        self.changed.notify_all();
    }

    fn kill(&self) {
        {
            let _st = lock(&self.state);
            self.killed.store(true, Ordering::SeqCst);
        }
        self.changed.notify_all();
    }

    fn killed(&self) -> bool {
        self.killed.load(Ordering::SeqCst)
    }
}

/// A runtime's sink on the engine's side: each call of its server's or hosted runtime's goes out as its frame,
/// every string cut to its limit and one byte more; a query or an Intl request waits for its answer in its
/// slot, a hosted renderer's platform request in the second one (the two may wait at once, from their own
/// threads), and once killed nothing more goes out.
struct EngineSink {
    runtime: u32,
    output: Arc<Output>,
    slot: SyncSlot,
    platform_slot: SyncSlot,
}

impl EngineSink {
    fn send(&self, kind: Kind, fields: &[Field]) -> bool {
        !self.slot.killed() && self.output.send(self.runtime, kind, fields)
    }

    /// Marked waiting in `slot` before its frame is written, so the reader finds it whenever the answer comes;
    /// none once the runtime or the stream is gone.
    fn request(&self, slot: &SyncSlot, wait: Wait, kind: Kind, text: &[u8]) -> Option<QueryAnswer> {
        if !slot.begin(wait) {
            return None;
        }
        if !self.send(kind, &[Field::Str(text)]) {
            slot.cancel();
            return None;
        }
        slot.wait()
    }

    /// The reader's: the host's answer to a request of `kind`, for the one this runtime waits for.
    fn answer(&self, kind: Wait, answer: QueryAnswer) {
        self.slot.fill(kind, answer);
    }

    /// The reader's: the host's answer to the platform request this runtime's renderer waits for.
    fn answer_platform(&self, text: Vec<u8>) {
        self.platform_slot.fill(Wait::Platform, QueryAnswer { json: text, ..QueryAnswer::default() });
    }

    /// A probe thread's: the server's answer to probe `seq`.
    fn probed(&self, seq: i64, text: &[u8]) {
        self.send(Kind::ProbeAnswer, &[Field::I64(seq), Field::Str(message(text))]);
    }

    /// The runtime closed or the stream ended: a waiting request of either slot gets no answer, and nothing more
    /// goes out.
    fn kill(&self) {
        self.slot.kill();
        self.platform_slot.kill();
    }
}

impl SandboxRemoteSink for EngineSink {
    fn remote_task_started(&self) {
        self.send(Kind::TaskStarted, &[]);
    }
    fn remote_task_completed(&self) {
        self.send(Kind::TaskCompleted, &[]);
    }
    fn remote_ops(&self, json: &[u8]) {
        self.send(Kind::Ops, &[Field::Str(cut(json, REMOTE_OPS_LIMIT))]);
    }
    fn remote_query(&self, json: &[u8]) -> QueryAnswer {
        self.request(&self.slot, Wait::Query, Kind::Query, message(json))
            .unwrap_or_else(|| QueryAnswer { json: DEAD_QUERY.to_vec(), ..QueryAnswer::default() })
    }
    fn remote_intl(&self, request_json: &[u8]) -> Vec<u8> {
        self.request(&self.slot, Wait::Intl, Kind::Intl, message(request_json)).map(|a| a.json).unwrap_or_default()
    }
    fn remote_listen(&self, json: &[u8]) {
        self.send(Kind::Listen, &[Field::Str(message(json))]);
    }
    fn remote_prevent_default(&self, json: &[u8]) {
        self.send(Kind::PreventDefault, &[Field::Str(message(json))]);
    }
    fn remote_anim(&self, json: &[u8]) {
        self.send(Kind::Anim, &[Field::Str(message(json))]);
    }
    fn remote_requests_frame(&self) {
        self.send(Kind::RequestsFrame, &[]);
    }
    fn remote_log(&self, level: &[u8], text: &[u8]) {
        self.send(Kind::Log, &[Field::Str(message(level)), Field::Str(message(text))]);
    }
    fn remote_diag(&self, kind: &[u8], detail: &[u8]) {
        self.send(Kind::Diag, &[Field::Str(message(kind)), Field::Str(message(detail))]);
    }
    fn remote_bridge(&self, method: &[u8], args_json: &[u8]) {
        self.send(Kind::Bridge, &[Field::Str(message(method)), Field::Str(message(args_json))]);
    }
    fn remote_copy_text(&self, text: &[u8]) {
        self.send(Kind::CopyText, &[Field::Str(message(text))]);
    }
    fn remote_request(&self, id: i64, data: &[u8], wallet: bool) {
        self.send(Kind::Request, &[Field::I64(id), Field::Str(message(data)), Field::Bool(wallet)]);
    }
    fn remote_wallet(&self, id: i64, payload: &[u8]) {
        self.send(Kind::Wallet, &[Field::I64(id), Field::Str(message(payload))]);
    }
    fn remote_storage(&self, commit_json: &[u8]) {
        self.send(Kind::Storage, &[Field::Str(message(commit_json))]);
    }
    fn remote_resize_anchor(&self, edge: &[u8]) {
        self.send(Kind::ResizeAnchor, &[Field::Str(message(edge))]);
    }
    fn remote_watches_public_data(&self, on: bool) {
        self.send(Kind::WatchesPublicData, &[Field::Bool(on)]);
    }
    fn remote_loaded(&self) {
        self.send(Kind::Loaded, &[]);
    }
    fn remote_stats(&self, stats: &SandboxStats) {
        let fields: Vec<Field> = encode_stats(stats).into_iter().map(Field::F64).collect();
        self.send(Kind::Stats, &fields);
    }
    fn remote_died(&self, reason: &[u8]) {
        self.send(Kind::Died, &[Field::Str(message(reason))]);
    }
    /// No frame: a server never reports one, and the host's end tells its clients of a lost stream itself.
    fn remote_disconnected(&self) {}
    fn remote_platform_message(&self, text: &[u8]) {
        self.send(Kind::PlatformMessage, &[Field::Str(platform_text(text))]);
    }
    fn remote_platform_request(&self, text: &[u8]) -> Option<Vec<u8>> {
        self.request(&self.platform_slot, Wait::Platform, Kind::PlatformRequest, platform_text(text)).map(|a| a.json)
    }
    fn remote_validated_ops(&self, json: &[u8]) {
        self.send(Kind::ValidatedOps, &[Field::Str(cut(json, REMOTE_OPS_LIMIT))]);
    }
    fn remote_renderer_ready(&self) {
        self.send(Kind::RendererReady, &[]);
    }
    fn remote_rendered(&self) {
        self.send(Kind::Rendered, &[]);
    }
    fn remote_nodes(&self, nodes: i64) {
        self.send(Kind::Nodes, &[Field::I64(nodes)]);
    }
}

/// One runtime on the engine's side: its server (an engine stream's) or its hosted runtime (a hosting
/// stream's), the sink it writes through and, once probed, the thread its probes run on. The reader opens and
/// closes it; `widget.reaper` destroys it, its probes' thread first, or the end of serving on the reader's
/// thread once nothing waits for the reader.
struct Route {
    sink: Arc<EngineSink>,
    server: Mutex<Option<Arc<SandboxRemoteServer>>>,
    hosted: Mutex<Option<Arc<dyn HostedRuntime>>>,
    probes: Mutex<Option<JsThread<()>>>,
}

impl Route {
    fn server(&self) -> Option<Arc<SandboxRemoteServer>> {
        lock(&self.server).clone()
    }

    fn hosted(&self) -> Option<Arc<dyn HostedRuntime>> {
        lock(&self.hosted).clone()
    }

    /// A waiting request of either slot given up before the server or hosted runtime closes, so none of its
    /// threads waits for the reader any more. Idempotent.
    fn close(&self) {
        self.sink.kill();
        if let Some(hosted) = self.hosted() {
            hosted.close();
        }
        if let Some(server) = self.server() {
            server.close();
        }
    }

    /// After `close`, never on the runtime's own threads: the probes' thread joined, then the server or hosted
    /// runtime destroyed (joining theirs).
    fn destroy(&self) {
        let probes = lock(&self.probes).take();
        drop(probes);
        let server = lock(&self.server).take();
        drop(server);
        let hosted = lock(&self.hosted).take();
        drop(hosted);
    }
}

/// `serve` and `serve_hosting` on the thread that reads, which alone keeps the runtimes by id and the reaper; the
/// opening decides whether every runtime is an engine's server or a hosted widget's runtime.
struct Serving<'a> {
    stream: Arc<dyn Stream>,
    build: &'a [u8],
    lockdown: Option<&'a dyn Fn() -> Vec<u8>>,
    platform: Arc<dyn Platform>,
    /// None for `serve`, which refuses a hosting opening.
    hosted: Option<HostedRuntimes<'a>>,
    output: Arc<Output>,
    resources: Arc<SandboxResources>,
    /// Whether the opening was `HostingOpening`.
    hosting: bool,
    routes: BTreeMap<u32, Arc<Route>>,
    last_opened: u32,
    reaper: Option<JsThread<()>>,
}

impl Serving<'_> {
    fn run(&mut self) -> Vec<u8> {
        let stream = Arc::clone(&self.stream);
        let mut reader = FrameReader::new(&*stream, Side::Host);
        match reader.next(MAX_OPENING_BYTES) {
            Err(why) => return self.refuse(why.into_bytes()),
            Ok(None) => return Vec::new(),
            Ok(Some(f)) => {
                let why = refusal_of(&f, self.build, self.hosted.is_some());
                if !why.is_empty() {
                    return self.refuse(why);
                }
                self.hosting = f.kind == Kind::HostingOpening;
                self.resources = Arc::new(resources_of(&f));
            }
        }
        if let Some(lockdown) = self.lockdown {
            let why = lockdown();
            if !why.is_empty() {
                return self.refuse(why);
            }
        }
        self.output.send(0, Kind::Ready, &[Field::I64(VERSION)]);
        let mut reason = Vec::new();
        loop {
            match reader.next(MAX_FRAME_BYTES) {
                Ok(Some(f)) => {
                    reason = self.dispatch(&f);
                    if !reason.is_empty() {
                        break;
                    }
                }
                Ok(None) => break,
                Err(why) => {
                    reason = why.into_bytes();
                    break;
                }
            }
        }
        self.end();
        reason
    }

    /// Before the opening is served: no runtime and no thread, and the stream is left as it is so the host can
    /// read the reason.
    fn refuse(&self, why: Vec<u8>) -> Vec<u8> {
        if !why.is_empty() {
            self.output.send(0, Kind::Refused, &[Field::Str(&why)]);
        }
        why
    }

    /// One frame for the runtimes, in the host's order: why it cannot be accepted, empty when it was (a closed
    /// runtime's is dropped); a kind of the other mode ends serving whatever its runtime.
    fn dispatch(&mut self, f: &Frame) -> Vec<u8> {
        if f.kind == Kind::Opening || f.kind == Kind::HostingOpening {
            return format!("kind {} after the opening", f.kind as u8).into_bytes();
        }
        if self.hosting && !hosting_takes(f.kind) {
            return format!("{}, which a hosting stream does not take", kind_on(f)).into_bytes();
        }
        if !self.hosting && hosted_kind(f.kind) {
            return format!("{}, which only a hosting stream takes", kind_on(f)).into_bytes();
        }
        if f.kind == Kind::Open {
            return self.open(f.runtime);
        }
        let Some(route) = self.routes.get(&f.runtime).cloned() else {
            return if f.runtime > self.last_opened { format!("{}, which was never opened", kind_on(f)).into_bytes() } else { Vec::new() };
        };
        match f.kind {
            Kind::Probe => {
                self.probe(&route, f.i64, f.str[0].to_vec());
                return Vec::new();
            }
            Kind::IntlAnswer => {
                route.sink.answer(Wait::Intl, QueryAnswer { json: f.str[0].to_vec(), ..QueryAnswer::default() });
                return Vec::new();
            }
            Kind::Close => {
                // A route the reaper could not take stays here for `end`.
                let why = self.retire(&route);
                if why.is_empty() {
                    self.routes.remove(&f.runtime);
                }
                return why;
            }
            _ => {}
        }
        if self.hosting { Self::hosted_input(&route, f) } else { Self::server_input(&route, f) }
    }

    /// An engine stream's input for its runtime's server, which keeps what comes before its start.
    fn server_input(route: &Route, f: &Frame) -> Vec<u8> {
        let Some(server) = route.server() else { return Vec::new() };
        match f.kind {
            Kind::Start => match decode_config(f.str[1]) {
                Some(config) => server.start(f.str[0], config),
                None => return undecodable(f),
            },
            Kind::Event => server.event(f.str[0]),
            Kind::Activation => server.activation_event(f.str[0], f.i64 as i32),
            Kind::AnimationFrame => server.frame(f.f64[0]),
            Kind::Viewport => server.viewport(f.f64[0], f.f64[1], f.f64[2]),
            Kind::Resize => server.resize(f.str[0]),
            Kind::Theme => match decode_theme_params(f.str[1]) {
                Some(params) => server.theme(f.str[0], params),
                None => return undecodable(f),
            },
            Kind::SafeArea => match decode_safe_area(f.str[0]) {
                Some(area) => server.content_safe_area(area),
                None => return undecodable(f),
            },
            Kind::DeviceSafeArea => match decode_device_safe_area(f.str[0]) {
                Some(area) => server.safe_area(area),
                None => return undecodable(f),
            },
            Kind::Data => match decode_data(f.str[0]) {
                Some(data) => server.set_data(data),
                None => return undecodable(f),
            },
            Kind::Visible => server.set_visible(f.flag),
            Kind::PressBegan => server.press_began(),
            Kind::PressEnded => server.press_ended(),
            Kind::ChargeValidator => server.charge_validator(f.f64[0]),
            Kind::Stop => server.stop(f.str[0]),
            Kind::Answer => match decode_outcome(f.str[0]) {
                Some(outcome) => server.answer(f.i64, &outcome),
                None => return undecodable(f),
            },
            Kind::QueryAnswer => {
                let binary = f.flag.then(|| f.str[1].to_vec());
                route.sink.answer(Wait::Query, QueryAnswer { json: f.str[0].to_vec(), binary, platform_seconds: 0.0 });
            }
            _ => {}
        }
        Vec::new()
    }

    /// A hosting stream's input for its runtime, which keeps what comes before its start; the start builds the
    /// widget's engine, validator and renderer here, on the reader.
    fn hosted_input(route: &Route, f: &Frame) -> Vec<u8> {
        let Some(runtime) = route.hosted() else { return Vec::new() };
        match f.kind {
            Kind::HostedStart => {
                let config = decode_config(f.str[1]);
                let start = decode_hosted_start(f.str[2]);
                let (Some(config), Some(start)) = (config, start) else { return undecodable(f) };
                runtime.start(f.str[0], config, start);
            }
            Kind::HostedTheme => match decode_theme(f.str[0]) {
                Some(theme) => runtime.theme(theme),
                None => return undecodable(f),
            },
            Kind::Platform => runtime.platform(f.str[0]),
            Kind::PlatformAnswer => route.sink.answer_platform(f.str[0].to_vec()),
            Kind::SafeArea => match decode_safe_area(f.str[0]) {
                Some(area) => runtime.content_safe_area(area),
                None => return undecodable(f),
            },
            Kind::DeviceSafeArea => match decode_device_safe_area(f.str[0]) {
                Some(area) => runtime.safe_area(area),
                None => return undecodable(f),
            },
            Kind::HostedViewportHeight => runtime.viewport_height(f.f64[0]),
            Kind::Data => match decode_data(f.str[0]) {
                Some(data) => runtime.set_data(data),
                None => return undecodable(f),
            },
            Kind::Visible => runtime.set_visible(f.flag),
            Kind::Stop => runtime.stop(f.str[0]),
            Kind::Answer => match decode_outcome(f.str[0]) {
                Some(outcome) => runtime.answer(f.i64, &outcome),
                None => return undecodable(f),
            },
            _ => {}
        }
        Vec::new()
    }

    /// A runtime whose id is above every earlier one, its server or hosted runtime not started.
    fn open(&mut self, runtime: u32) -> Vec<u8> {
        if runtime <= self.last_opened {
            return format!("kind 2 on runtime {runtime}, not above the last opened runtime {}", self.last_opened).into_bytes();
        }
        self.last_opened = runtime;
        let sink =
            Arc::new(EngineSink { runtime, output: Arc::clone(&self.output), slot: SyncSlot::new(), platform_slot: SyncSlot::new() });
        let route = Route { sink: Arc::clone(&sink), server: Mutex::new(None), hosted: Mutex::new(None), probes: Mutex::new(None) };
        match self.hosted.filter(|_| self.hosting) {
            Some(hosted) => *lock(&route.hosted) = Some(hosted(sink, Arc::clone(&self.resources))),
            None => {
                let server = SandboxRemoteServer::new(sink, Arc::clone(&self.resources), Arc::clone(&self.platform));
                *lock(&route.server) = Some(Arc::new(server));
            }
        }
        self.routes.insert(runtime, Arc::new(route));
        Vec::new()
    }

    /// On the runtime's `widget.probe` thread, made at its first probe, so the reader goes on answering the
    /// queries the probe may wait for. A runtime whose `widget.probe` cannot start is stopped with why
    /// (`PROBE_UNSTARTED`, then the refusal) as the host's `Stop` stops it, its renderer kept until `Close`, and
    /// the probe answered "ERROR no runtime" at once; a later probe tries the thread again.
    fn probe(&self, route: &Arc<Route>, seq: i64, code: Vec<u8>) {
        let mut probes = lock(&route.probes);
        if probes.is_none() {
            match JsThread::new("widget.probe", || ()) {
                Ok(thread) => *probes = Some(thread),
                Err(e) => {
                    drop(probes);
                    let why = format!("{PROBE_UNSTARTED}: {e}");
                    if let Some(hosted) = route.hosted() {
                        hosted.stop(why.as_bytes());
                    } else if let Some(server) = route.server() {
                        server.stop(why.as_bytes());
                    }
                    route.sink.probed(seq, b"ERROR no runtime");
                    return;
                }
            }
        }
        let task_route = Arc::clone(route);
        probes.as_ref().expect("made above").post_task(move |_| {
            let answer = match (task_route.hosted(), task_route.server()) {
                (Some(hosted), _) => hosted.probe(&code),
                (None, Some(server)) => server.probe(&code),
                (None, None) => b"ERROR no runtime".to_vec(),
            };
            task_route.sink.probed(seq, &answer);
        });
    }

    /// In the host's order, the route closed, then destroyed on `widget.reaper`, never on the reader or the
    /// runtime's own threads: empty, or why serving cannot go on when the reaper cannot start.
    fn retire(&mut self, route: &Arc<Route>) -> Vec<u8> {
        route.close();
        if self.reaper.is_none() {
            match JsThread::new("widget.reaper", || ()) {
                Ok(thread) => self.reaper = Some(thread),
                Err(e) => return format!("{REAPER_UNSTARTED}: {e}").into_bytes(),
            }
        }
        let route = Arc::clone(route);
        self.reaper.as_ref().expect("made above").post_task(move |_| route.destroy());
        Vec::new()
    }

    /// The writes stop and a blocked one wakes before any lock is taken, then every runtime left is closed before
    /// anything is joined, the reaper drained if one was made, and the rest destroyed on this thread.
    fn end(&mut self) {
        self.output.break_off();
        for route in self.routes.values() {
            route.close();
        }
        if let Some(reaper) = self.reaper.take() {
            reaper.join();
        }
        for route in self.routes.values() {
            route.destroy();
        }
        self.routes.clear();
    }
}

fn serve_stream(
    stream: Arc<dyn Stream>,
    build: &[u8],
    lockdown: Option<&dyn Fn() -> Vec<u8>>,
    platform: Arc<dyn Platform>,
    hosted: Option<HostedRuntimes>,
) -> Vec<u8> {
    let output = Arc::new(Output { stream: Arc::clone(&stream), writing: Mutex::new(()), broken: AtomicBool::new(false) });
    let mut serving = Serving {
        stream,
        build,
        lockdown,
        platform,
        hosted,
        output,
        resources: Arc::new(SandboxResources::default()),
        hosting: false,
        routes: BTreeMap::new(),
        last_opened: 0,
        reaper: None,
    };
    serving.run()
}

/// Serves one engine stream until it ends, on the calling thread: the opening must be the host's of this
/// `build` (a hosting opening is refused as another first kind), then `lockdown` runs (its refusal, empty when
/// the process locked itself down), `Ready` goes out and each frame reaches its runtime. Why serving ended, empty
/// at the end of the stream; every runtime is destroyed before it returns.
pub fn serve(stream: Arc<dyn Stream>, build: &[u8], lockdown: Option<&dyn Fn() -> Vec<u8>>, platform: Arc<dyn Platform>) -> Vec<u8> {
    serve_stream(stream, build, lockdown, platform, None)
}

/// `serve` for a stream whose opening may also be a hosting one (`SandboxRemoteEngineEnd::serveHosting`): after a
/// `HostingOpening` every runtime is `hosted`'s, made on the reader at its `Open`, and the stream takes the hosted
/// kinds in place of an engine's; after an engine `Opening` it is an engine stream as `serve` serves one.
pub fn serve_hosting(
    stream: Arc<dyn Stream>,
    build: &[u8],
    lockdown: Option<&dyn Fn() -> Vec<u8>>,
    platform: Arc<dyn Platform>,
    hosted: HostedRuntimes,
) -> Vec<u8> {
    serve_stream(stream, build, lockdown, platform, Some(hosted))
}
