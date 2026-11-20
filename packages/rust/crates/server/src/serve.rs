//! The engine process's end of a stream (`remote_stream_engine.cpp`): the loop `SandboxRemoteEngineEnd::serve`
//! runs on the thread that reads, each runtime's server writing its calls as frames through its sink.

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};

use twidget_bundle_stamp::SandboxResources;
use twidget_engine::{QueryAnswer, SandboxStats};
use twidget_js::{JsThread, Platform};
use twidget_stream::{
    Field, Frame, FrameReader, Kind, MAX_FRAME_BYTES, MAX_OPENING_BYTES, MAX_STAMP_BYTES, REMOTE_MESSAGE_LIMIT, REMOTE_OPS_LIMIT,
    REMOTE_PLATFORM_LIMIT, Side, Stream, VERSION, cut, encode, kind_on,
};

use crate::coding::{decode_config, decode_data, decode_outcome, decode_safe_area, decode_theme_params, encode_stats};
use crate::server::{SandboxRemoteServer, SandboxRemoteSink};

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

fn in_quotes(s: &[u8]) -> Vec<u8> {
    [&b"\""[..], &s[..s.len().min(QUOTED_BYTES)], if s.len() > QUOTED_BYTES { b"...\"" } else { b"\"" }].concat()
}

fn undecodable(f: &Frame) -> Vec<u8> {
    format!("{} with a form that does not decode", kind_on(f)).into_bytes()
}

/// Why the engine's end refuses a first frame, empty for an opening it serves: another kind (a hosting opening
/// too, which only a hosting end serves), another version or build, a stamp past `MAX_STAMP_BYTES` or of more
/// than one line (never cut into another stamp).
fn refusal_of(f: &Frame, build: &[u8]) -> Vec<u8> {
    if f.kind != Kind::Opening {
        return format!("kind {} first, not the opening", f.kind as u8).into_bytes();
    }
    if f.i64 != VERSION {
        return format!("stream version {}, this end's is {VERSION}", f.i64).into_bytes();
    }
    if f.str[0] != build {
        return [&b"build "[..], &in_quotes(f.str[0]), b", this end's is ", &in_quotes(build)].concat();
    }
    for (name, stamp) in [("validator", f.str[2]), ("renderer", f.str[3])] {
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
/// renderer's stamp lines for the stamp's checks at boot.
fn resources_of(f: &Frame) -> SandboxResources {
    SandboxResources { sandbox_bundle: f.str[1].to_vec(), validator_bundle: f.str[2].to_vec(), renderer_bundle: f.str[3].to_vec() }
}

/// The host kinds only a hosting stream takes.
fn hosted_kind(kind: Kind) -> bool {
    matches!(kind, Kind::HostedStart | Kind::HostedTheme | Kind::Platform | Kind::PlatformAnswer)
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
}

struct SlotState {
    waiting: Wait,
    answer: Option<QueryAnswer>,
}

/// One synchronous request of a runtime's at a time (its engine's query or Intl request): marked before it is
/// written, filled by the reader with an answer of its kind only, given up once killed. Its lock is never held
/// across a write or a call out.
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

/// A runtime's sink on the engine's side: each call of its server goes out as its frame, every string cut to
/// its limit and one byte more; a query or an Intl request waits for its answer in its slot, and once killed
/// nothing more goes out.
struct EngineSink {
    runtime: u32,
    output: Arc<Output>,
    slot: SyncSlot,
}

impl EngineSink {
    fn send(&self, kind: Kind, fields: &[Field]) -> bool {
        !self.slot.killed() && self.output.send(self.runtime, kind, fields)
    }

    /// Marked waiting before its frame is written, so the reader finds it whenever the answer comes; none once
    /// the runtime or the stream is gone.
    fn request(&self, wait: Wait, kind: Kind, text: &[u8]) -> Option<QueryAnswer> {
        if !self.slot.begin(wait) {
            return None;
        }
        if !self.send(kind, &[Field::Str(text)]) {
            self.slot.cancel();
            return None;
        }
        self.slot.wait()
    }

    /// The reader's: the host's answer to a request of `kind`, for the one this runtime waits for.
    fn answer(&self, kind: Wait, answer: QueryAnswer) {
        self.slot.fill(kind, answer);
    }

    /// A probe thread's: the server's answer to probe `seq`.
    fn probed(&self, seq: i64, text: &[u8]) {
        self.send(Kind::ProbeAnswer, &[Field::I64(seq), Field::Str(message(text))]);
    }

    /// The runtime closed or the stream ended: a waiting request gets no answer, and nothing more goes out.
    fn kill(&self) {
        self.slot.kill();
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
        self.request(Wait::Query, Kind::Query, message(json))
            .unwrap_or_else(|| QueryAnswer { json: DEAD_QUERY.to_vec(), ..QueryAnswer::default() })
    }
    fn remote_intl(&self, request_json: &[u8]) -> Vec<u8> {
        self.request(Wait::Intl, Kind::Intl, message(request_json)).map(|a| a.json).unwrap_or_default()
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
}

/// One runtime on the engine's side: its server, the sink it writes through and, once probed, the thread its
/// probes run on. The reader opens and closes it; `widget.reaper` destroys it, its probes' thread first, or the
/// end of serving on the reader's thread once nothing waits for the reader.
struct Route {
    sink: Arc<EngineSink>,
    server: Mutex<Option<Arc<SandboxRemoteServer>>>,
    probes: Mutex<Option<JsThread<()>>>,
}

impl Route {
    fn server(&self) -> Option<Arc<SandboxRemoteServer>> {
        lock(&self.server).clone()
    }

    /// A waiting request given up before the server closes, so none of its threads waits for the reader any
    /// more. Idempotent.
    fn close(&self) {
        self.sink.kill();
        if let Some(server) = self.server() {
            server.close();
        }
    }

    /// After `close`, never on the runtime's own threads: the probes' thread joined, then the server destroyed
    /// (joining the engine's).
    fn destroy(&self) {
        let probes = lock(&self.probes).take();
        drop(probes);
        let server = lock(&self.server).take();
        drop(server);
    }
}

/// `serve` on the thread that reads, which alone keeps the runtimes by id and the reaper.
struct Serving<'a> {
    stream: Arc<dyn Stream>,
    build: &'a [u8],
    lockdown: Option<&'a dyn Fn() -> Vec<u8>>,
    platform: Arc<dyn Platform>,
    output: Arc<Output>,
    resources: Arc<SandboxResources>,
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
                let why = refusal_of(&f, self.build);
                if !why.is_empty() {
                    return self.refuse(why);
                }
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
    /// runtime's is dropped).
    fn dispatch(&mut self, f: &Frame) -> Vec<u8> {
        if f.kind == Kind::Opening || f.kind == Kind::HostingOpening {
            return format!("kind {} after the opening", f.kind as u8).into_bytes();
        }
        if hosted_kind(f.kind) {
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
        Self::server_input(&route, f)
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

    /// A runtime whose id is above every earlier one, its server not started.
    fn open(&mut self, runtime: u32) -> Vec<u8> {
        if runtime <= self.last_opened {
            return format!("kind 2 on runtime {runtime}, not above the last opened runtime {}", self.last_opened).into_bytes();
        }
        self.last_opened = runtime;
        let sink = Arc::new(EngineSink { runtime, output: Arc::clone(&self.output), slot: SyncSlot::new() });
        let server = SandboxRemoteServer::new(sink.clone(), Arc::clone(&self.resources), Arc::clone(&self.platform));
        let route = Route { sink, server: Mutex::new(Some(Arc::new(server))), probes: Mutex::new(None) };
        self.routes.insert(runtime, Arc::new(route));
        Vec::new()
    }

    /// On the runtime's `widget.probe` thread, made at its first probe, so the reader goes on answering the
    /// queries the probe may wait for. A runtime whose `widget.probe` cannot start is stopped with why, and the
    /// probe answered "ERROR no runtime" at once; a later probe tries the thread again.
    fn probe(&self, route: &Arc<Route>, seq: i64, code: Vec<u8>) {
        let mut probes = lock(&route.probes);
        if probes.is_none() {
            match JsThread::new("widget.probe", || ()) {
                Ok(thread) => *probes = Some(thread),
                Err(e) => {
                    drop(probes);
                    let why = format!("{PROBE_UNSTARTED}: {e}");
                    if let Some(server) = route.server() {
                        server.stop(why.as_bytes());
                    }
                    route.sink.probed(seq, b"ERROR no runtime");
                    return;
                }
            }
        }
        let task_route = Arc::clone(route);
        probes.as_ref().expect("made above").post_task(move |_| {
            let answer = match task_route.server() {
                Some(server) => server.probe(&code),
                None => b"ERROR no runtime".to_vec(),
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

/// Serves one engine stream until it ends, on the calling thread: the opening must be the host's of this
/// `build`, then `lockdown` runs (its refusal, empty when the process locked itself down), `Ready` goes out
/// and each frame reaches its runtime. Why serving ended, empty at the end of the stream; every runtime is
/// destroyed before it returns.
pub fn serve(stream: Arc<dyn Stream>, build: &[u8], lockdown: Option<&dyn Fn() -> Vec<u8>>, platform: Arc<dyn Platform>) -> Vec<u8> {
    let output = Arc::new(Output { stream: Arc::clone(&stream), writing: Mutex::new(()), broken: AtomicBool::new(false) });
    let mut serving = Serving {
        stream,
        build,
        lockdown,
        platform,
        output,
        resources: Arc::new(SandboxResources::default()),
        routes: BTreeMap::new(),
        last_opened: 0,
        reaper: None,
    };
    serving.run()
}

/// The platform limit, kept for the hosted mode's sink.
#[allow(dead_code)]
const PLATFORM_LIMIT: usize = REMOTE_PLATFORM_LIMIT;
