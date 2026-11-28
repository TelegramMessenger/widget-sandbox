//! The engine's side of one client-mode engine (`SandboxRemoteServer`, `remote_engine.cpp`): a real
//! [`SandboxEngine`] built at `start` with the server as its delegate, each delegate call sent on to the sink
//! from the engine's thread in order (an action only as its raw `remote_bridge`), requests and wallet calls
//! numbered from one counter for `answer`.

use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};

use twidget_bundle_stamp::SandboxResources;
use twidget_engine::{
    Engine, EngineDelegate, IntlProvider, QueryAnswer, SandboxConfig, SandboxEngine, SandboxStats, WidgetContentSafeArea,
    WidgetRequestResponder,
};
use twidget_js::Platform;
use twidget_json::Json;
use twidget_sdk_policy::{
    StorageArea, StorageCommit, WalletCall, WidgetAction, WidgetData, WidgetRequest, WidgetRequestAnswer, WidgetRequestFailure,
    WidgetRequestOutcome,
};

use crate::coding::{SandboxRemoteOutcome, encode_wallet};

/// Engine → host, untrusted: the client checks every call again. A transport may call the sink after the
/// client is gone: such calls return at once and change nothing.
pub trait SandboxRemoteSink: Send + Sync {
    /// A task of the engine's side began; `remote_task_completed` ends it, also after a death inside it.
    fn remote_task_started(&self);
    fn remote_task_completed(&self);
    fn remote_ops(&self, json: &[u8]);
    /// Synchronous: the delegate's answer after every earlier call reached it.
    fn remote_query(&self, json: &[u8]) -> QueryAnswer;
    /// Synchronous like `remote_query`: the host's `IntlProvider` formats the shim's request object, "" for a
    /// malformed one.
    fn remote_intl(&self, request_json: &[u8]) -> Vec<u8>;
    fn remote_listen(&self, json: &[u8]);
    fn remote_prevent_default(&self, json: &[u8]);
    fn remote_anim(&self, json: &[u8]);
    fn remote_requests_frame(&self);
    fn remote_log(&self, level: &[u8], text: &[u8]);
    fn remote_diag(&self, kind: &[u8], detail: &[u8]);
    /// A bridge action's method and raw arguments.
    fn remote_bridge(&self, method: &[u8], args_json: &[u8]);
    fn remote_copy_text(&self, text: &[u8]);
    /// A request numbered by the engine's side, one counter with `remote_wallet`.
    fn remote_request(&self, id: i64, data: &[u8], wallet: bool);
    /// A wallet call in `encode_wallet`'s form.
    fn remote_wallet(&self, id: i64, payload: &[u8]);
    /// One area of a storage commit, `{"local":{…}}` or `{"session":{…}}`.
    fn remote_storage(&self, commit_json: &[u8]);
    fn remote_resize_anchor(&self, edge: &[u8]);
    fn remote_watches_public_data(&self, on: bool);
    fn remote_loaded(&self);
    fn remote_stats(&self, stats: &SandboxStats);
    fn remote_died(&self, reason: &[u8]);
    /// The transport lost the engine's side or never reached it: the widget dies.
    fn remote_disconnected(&self);

    /// The first of a hosted runtime's calls (`remote_hosted.h`: the widget's validator and renderer run on the
    /// engine's side too), which a client of an engine-only runtime refuses (`protocol-violation`) and a sink
    /// that does not carry them ignores: a message of the engine's side's renderer for the host's platform,
    /// opaque (`SandboxHostedPlatform::send`).
    fn remote_platform_message(&self, _message: &[u8]) {}
    /// Synchronous like `remote_query`: the renderer's one waiting request (`SandboxHostedPlatform::request`)
    /// and the host's answer, none when there is none (the runtime closed, the widget died, the transport ended).
    fn remote_platform_request(&self, _message: &[u8]) -> Option<Vec<u8>> {
        None
    }
    /// A batch the engine's side's validator passed, for a host that asked for them at its hosted start.
    fn remote_validated_ops(&self, _json: &[u8]) {}
    /// The engine's side's renderer announced itself.
    fn remote_renderer_ready(&self) {}
    /// The engine's side's glue saw the loaded widget's first render.
    fn remote_rendered(&self) {}
    /// The widget's node count after a validated batch, as the engine's side reports it (the client clamps it).
    fn remote_nodes(&self, _nodes: i64) {}
}

/// The engine's Intl provider on the engine's side: the host's provider formats each request through the
/// synchronous `remote_intl` (`detail::RemoteIntl`).
pub struct RemoteIntl {
    sink: Arc<dyn SandboxRemoteSink>,
}

impl std::fmt::Debug for RemoteIntl {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("RemoteIntl")
    }
}

impl RemoteIntl {
    pub fn new(sink: Arc<dyn SandboxRemoteSink>) -> RemoteIntl {
        RemoteIntl { sink }
    }
}

impl IntlProvider for RemoteIntl {
    fn format(&self, request: &Json) -> Vec<u8> {
        self.sink.remote_intl(&request.dump())
    }
}

/// The death of a runtime whose engine cannot be built (its `widget.sandbox` cannot start), before the refusal itself.
const ENGINE_UNSTARTED: &str = "the widget engine could not start";

type Input = Box<dyn FnOnce(&SandboxEngine) + Send>;

/// What the server's lock guards: the C++'s members.
struct ServerState {
    started: bool,
    closed: bool,
    waiting: Vec<Input>,
    engine: Option<Arc<SandboxEngine>>,
    last_request: i64,
    pending: BTreeMap<i64, WidgetRequestResponder>,
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// The engine's delegate: the sink, and the numbering of the calls it forwards.
struct ServerDelegate {
    sink: Arc<dyn SandboxRemoteSink>,
    state: Arc<Mutex<ServerState>>,
}

impl ServerDelegate {
    /// Numbers `respond` and keeps it for `answer`, then `send`s the number; refused (`unsupported`) after `close`.
    fn forward(&self, respond: WidgetRequestResponder, send: impl FnOnce(i64)) {
        let id = {
            let mut st = lock(&self.state);
            // Every id is new: past the last one the calls are refused as after `close`.
            if !st.closed && st.last_request < i64::MAX {
                st.last_request += 1;
                let id = st.last_request;
                st.pending.insert(id, respond.clone());
                id
            } else {
                0
            }
        };
        if id == 0 {
            return respond.unsupported();
        }
        send(id);
    }
}

impl EngineDelegate for ServerDelegate {
    fn on_task_started(&self, _: &Engine) {
        self.sink.remote_task_started();
    }
    fn on_task_completed(&self, _: &Engine) {
        self.sink.remote_task_completed();
    }
    fn on_ops(&self, _: &Engine, json: &[u8]) {
        self.sink.remote_ops(json);
    }
    fn on_query(&self, _: &Engine, json: &[u8]) -> QueryAnswer {
        self.sink.remote_query(json)
    }
    fn on_listen(&self, _: &Engine, json: &[u8]) {
        self.sink.remote_listen(json);
    }
    fn on_prevent_default(&self, _: &Engine, json: &[u8]) {
        self.sink.remote_prevent_default(json);
    }
    fn on_anim(&self, _: &Engine, json: &[u8]) {
        self.sink.remote_anim(json);
    }
    fn on_request_frame(&self, _: &Engine) {
        self.sink.remote_requests_frame();
    }
    fn on_log(&self, _: &Engine, level: &[u8], text: &[u8]) {
        self.sink.remote_log(level, text);
    }
    fn on_diag(&self, _: &Engine, kind: &[u8], detail: &[u8]) {
        self.sink.remote_diag(kind, detail);
    }
    // The raw call right after it is what the client checks again: an action goes once.
    fn on_bridge_action(&self, _: &Engine, _: &WidgetAction) {}
    fn on_bridge(&self, _: &Engine, method: &[u8], args_json: &[u8]) {
        self.sink.remote_bridge(method, args_json);
    }
    fn on_bridge_copy_text(&self, _: &Engine, text: &[u8]) {
        self.sink.remote_copy_text(text);
    }
    fn on_storage(&self, _: &Engine, area: &[u8], values: &StorageArea) {
        let mut commit = StorageCommit::default();
        if area == b"local" {
            commit.local = Some(values.clone());
        } else {
            commit.session = Some(values.clone());
        }
        self.sink.remote_storage(&commit.to_json().dump());
    }
    fn on_resize_anchor(&self, _: &Engine, edge: &[u8]) {
        self.sink.remote_resize_anchor(edge);
    }
    fn on_public_data_watch(&self, _: &Engine, on: bool) {
        self.sink.remote_watches_public_data(on);
    }
    fn on_bridge_request(&self, _: &Engine, request: &WidgetRequest, respond: WidgetRequestResponder) {
        self.forward(respond, |id| self.sink.remote_request(id, &request.data, request.wallet));
    }
    fn on_bridge_wallet(&self, _: &Engine, call: &WalletCall, respond: WidgetRequestResponder) {
        self.forward(respond, |id| self.sink.remote_wallet(id, &encode_wallet(call)));
    }
    fn on_loaded(&self, _: &Engine) {
        self.sink.remote_loaded();
    }
    fn on_stats(&self, _: &Engine, stats: &SandboxStats) {
        self.sink.remote_stats(stats);
    }
    fn on_died(&self, _: &Engine, reason: &[u8]) {
        self.sink.remote_died(reason);
    }
}

/// `start`, the inputs, `answer` and `close` come one at a time in the host's order (a channel's thread) and
/// never wait for the engine, which may be waiting for the host's answer to a query or an Intl request;
/// `probe`, the one synchronous call, may come from another thread but never from the one answering those.
/// The owner drops the server on its own thread (never the engine's) after its last call.
pub struct SandboxRemoteServer {
    sink: Arc<dyn SandboxRemoteSink>,
    resources: Arc<SandboxResources>,
    platform: Arc<dyn Platform>,
    state: Arc<Mutex<ServerState>>,
}

impl std::fmt::Debug for SandboxRemoteServer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let st = lock(&self.state);
        f.debug_struct("SandboxRemoteServer").field("started", &st.started).field("closed", &st.closed).finish()
    }
}

impl SandboxRemoteServer {
    pub fn new(sink: Arc<dyn SandboxRemoteSink>, resources: Arc<SandboxResources>, platform: Arc<dyn Platform>) -> SandboxRemoteServer {
        let state =
            ServerState { started: false, closed: false, waiting: Vec::new(), engine: None, last_request: 0, pending: BTreeMap::new() };
        SandboxRemoteServer { sink, resources, platform, state: Arc::new(Mutex::new(state)) }
    }

    /// Builds the engine (directly), gives it the inputs that came before in their order for its own pre-boot
    /// rules, then starts it; once, never after `close`. An engine that cannot be built closes the server, the
    /// inputs kept for it dropped, and the sink hears the runtime's death with why.
    pub fn start(&self, html: &[u8], config: SandboxConfig) {
        {
            let mut st = lock(&self.state);
            if st.closed || st.started {
                return;
            }
            st.started = true;
        }
        let intl: Arc<dyn IntlProvider> = Arc::new(RemoteIntl::new(Arc::clone(&self.sink)));
        let engine = match SandboxEngine::new(html.to_vec(), config, Arc::clone(&self.resources), intl, Arc::clone(&self.platform)) {
            Ok(engine) => Arc::new(engine),
            Err(why) => {
                // Closed as `close` closes it (nothing is outstanding before an engine), then the death outside the lock.
                {
                    let mut st = lock(&self.state);
                    st.closed = true;
                    st.waiting.clear();
                }
                self.sink.remote_died(format!("{ENGINE_UNSTARTED}: {why}").as_bytes());
                return;
            }
        };
        engine.set_delegate(self.delegate());
        let waiting = {
            let mut st = lock(&self.state);
            st.engine = Some(Arc::clone(&engine));
            std::mem::take(&mut st.waiting)
        };
        for call in waiting {
            call(&engine);
        }
        engine.start();
    }

    /// `call` with the engine, kept until `start` and dropped after `close`; the lock only decides.
    fn input(&self, call: impl FnOnce(&SandboxEngine) + Send + 'static) {
        let engine = {
            let mut st = lock(&self.state);
            if st.closed {
                return;
            }
            match &st.engine {
                None => return st.waiting.push(Box::new(call)),
                Some(engine) => Arc::clone(engine),
            }
        };
        call(&engine);
    }

    pub fn event(&self, json: &[u8]) {
        let json = json.to_vec();
        self.input(move |e| e.event(&json));
    }
    pub fn activation_event(&self, json: &[u8], gesture: i32) {
        let json = json.to_vec();
        self.input(move |e| e.activation_event(&json, gesture));
    }
    pub fn frame(&self, time: f64) {
        self.input(move |e| e.frame(time));
    }
    pub fn viewport(&self, inner_width: f64, inner_height: f64, dpr: f64) {
        self.input(move |e| e.viewport(inner_width, inner_height, dpr));
    }
    pub fn resize(&self, entries_json: &[u8]) {
        let json = entries_json.to_vec();
        self.input(move |e| e.resize(&json));
    }
    pub fn theme(&self, color_scheme: &[u8], theme_params: BTreeMap<Vec<u8>, Vec<u8>>) {
        let scheme = color_scheme.to_vec();
        self.input(move |e| e.theme(&scheme, &theme_params));
    }
    pub fn content_safe_area(&self, area: WidgetContentSafeArea) {
        self.input(move |e| e.content_safe_area(area));
    }
    pub fn set_data(&self, data: WidgetData) {
        self.input(move |e| e.set_data(&data));
    }
    pub fn set_visible(&self, visible: bool) {
        self.input(move |e| e.set_visible(visible));
    }
    pub fn press_began(&self) {
        self.input(SandboxEngine::press_began);
    }
    pub fn press_ended(&self) {
        self.input(SandboxEngine::press_ended);
    }
    pub fn charge_validator(&self, ms: f64) {
        self.input(move |e| e.charge_validator(ms));
    }
    pub fn stop(&self, reason: &[u8]) {
        let reason = reason.to_vec();
        self.input(move |e| e.stop(&reason));
    }

    /// Settles the outstanding request or wallet call `id`, once; an id that is not outstanding changes nothing.
    pub fn answer(&self, id: i64, outcome: &SandboxRemoteOutcome) {
        let Some(respond) = lock(&self.state).pending.remove(&id) else { return };
        match outcome {
            SandboxRemoteOutcome::Answer(result) => respond.respond(WidgetRequestOutcome {
                failure: WidgetRequestFailure::None,
                answer: WidgetRequestAnswer { result: result.clone() },
            }),
            // A failure stays one: `None` here would read as an answer.
            SandboxRemoteOutcome::Failure(failure) => respond.respond(WidgetRequestOutcome {
                failure: if *failure == WidgetRequestFailure::None { WidgetRequestFailure::Failed } else { *failure },
                answer: WidgetRequestAnswer::default(),
            }),
            SandboxRemoteOutcome::Unsupported => respond.unsupported(),
        }
    }

    /// The server as its engine's delegate (the C++'s `SandboxRemoteServer` is one): each call sent on to the
    /// sink, requests and wallet calls numbered for `answer` and refused after `close`. A hosted runtime's
    /// never-started server relays its own engine's calls through it.
    pub fn delegate(&self) -> Arc<dyn EngineDelegate> {
        Arc::new(ServerDelegate { sink: Arc::clone(&self.sink), state: Arc::clone(&self.state) })
    }

    /// The engine's `probe`, waiting for its thread; "ERROR no runtime" before `start` and after `close`.
    pub fn probe(&self, code: &[u8]) -> Vec<u8> {
        let engine = {
            let st = lock(&self.state);
            if st.closed { None } else { st.engine.clone() }
        };
        match engine {
            Some(engine) => engine.probe(code),
            None => b"ERROR no runtime".to_vec(),
        }
    }

    /// The client is gone: the waiting inputs are dropped, the outstanding calls refused (`unsupported`) and
    /// the engine terminated without waiting for it; it lives until the server is dropped. Idempotent.
    pub fn close(&self) {
        let (open, engine) = {
            let mut st = lock(&self.state);
            if std::mem::replace(&mut st.closed, true) {
                return;
            }
            st.waiting.clear();
            (std::mem::take(&mut st.pending), st.engine.clone())
        };
        for respond in open.values() {
            respond.unsupported();
        }
        if let Some(engine) = engine {
            engine.terminate();
        }
    }
}

impl Drop for SandboxRemoteServer {
    /// Closes, then joins the engine while the sink and its delegate still exist.
    fn drop(&mut self) {
        self.close();
        let engine = lock(&self.state).engine.take();
        drop(engine);
    }
}
