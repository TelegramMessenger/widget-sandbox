//! remote_hosted.cpp's engine side: `SandboxHostedRuntime` builds a hosted widget's engine, validator and
//! renderer as a served host and routes that engine's calls, keeps the renderer alone once that engine died
//! until the host's `Close`, and `serveHosting` serves a hosting stream with such runtimes; `HostedPlatform` is
//! the renderer's traffic with the host's platform over the runtime's sink.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, Weak};
use std::thread::ThreadId;

use twidget_bundle_stamp::SandboxResources;
use twidget_engine::{
    Engine, EngineDelegate, QueryAnswer, SandboxConfig, SandboxHostedStart, SandboxStats, WidgetContentSafeArea, WidgetTheme,
};
use twidget_engine::{IntlProvider, WidgetRequestResponder};
use twidget_js::Platform;
use twidget_sdk_policy::{StorageArea, WalletCall, WidgetAction, WidgetData, WidgetRequest};
use twidget_server::{HostedRuntime, RemoteIntl, SandboxRemoteOutcome, SandboxRemoteServer, SandboxRemoteSink};
use twidget_stream::Stream;

use crate::renderer::Renderer;
use crate::served::{GlueReports, GlueWaited, RendererFactory, Served, ServedHost};

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// What the engine process's embedder gives `serve_hosting` (remote_hosted.h's `HostedRendererFactory`): one
/// hosted widget's renderer, given the opening's resources (the sandbox and validator bundles, of the renderer
/// bundle only its stamp line), the colour scheme, the bubble's width in CSS px and the widget's platform. Called
/// on the engine process's reader thread at the widget's hosted start; `Err` (what it threw) or `Ok(None)` ends
/// only that widget, its death reported to the host.
pub type HostedRendererFactory =
    Arc<dyn Fn(Arc<SandboxResources>, &[u8], f64, Arc<HostedPlatform>) -> Result<Option<Box<dyn Renderer>>, String> + Send + Sync>;

type MessageObserver = Arc<dyn Fn(&[u8]) + Send + Sync>;

/// Under the accounting's lock: the watched glue thread (none before `watch_glue` and after `unwatch_glue`), what
/// hears its waits end (called outside the lock, changed only while no call of it is under way), how many such
/// calls are, the glue's time in `request` up to the wait in progress and that wait's start.
struct Accounting {
    glue: Option<ThreadId>,
    wait_ended: Option<Arc<dyn Fn() + Send + Sync>>,
    hearing: i32,
    total: f64,
    since: Option<f64>,
}

/// A hosted renderer's platform over its runtime's sink (remote_hosted.h's `SandboxHostedPlatform`, the
/// runtime's `HostedPlatform`): each message straight to it, the requests one at a time; nothing once the
/// runtime closed. It keeps the time the served host's glue thread spends inside `request`, its wait for its
/// turn included, as the glue's layout queries read it, and wakes them at each such wait's end while the
/// runtime watches that thread: from `watch_glue` to `unwatch_glue`, which the runtime calls while its host and
/// that thread live, so a renderer that keeps its platform past its widget never reaches a host that is gone,
/// from that thread's id given to another thread either.
pub struct HostedPlatform {
    sink: Arc<dyn SandboxRemoteSink>,
    clock: Arc<dyn Platform>,
    requesting: Mutex<()>,
    closed: AtomicBool,
    accounting: Mutex<Accounting>,
    idle: Condvar,
    /// The host's platform messages for the renderer (`onMessage`).
    on_message: Mutex<Option<MessageObserver>>,
}

impl std::fmt::Debug for HostedPlatform {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("HostedPlatform").field("closed", &self.closed.load(Ordering::SeqCst)).finish()
    }
}

/// Admitted on the watched glue thread alone, under the accounting's lock: that thread's wait in `request`
/// counts, and its end is heard if the watch still lasts then; never under `requesting`, and the accounting's
/// lock is held for no call.
struct GlueWait<'a> {
    platform: &'a HostedPlatform,
    counted: bool,
}

impl<'a> GlueWait<'a> {
    fn new(platform: &'a HostedPlatform) -> GlueWait<'a> {
        let mut a = lock(&platform.accounting);
        if a.glue != Some(std::thread::current().id()) {
            return GlueWait { platform, counted: false };
        }
        a.since = Some(platform.clock.steady_seconds());
        GlueWait { platform, counted: true }
    }
}

impl Drop for GlueWait<'_> {
    fn drop(&mut self) {
        if !self.counted {
            return;
        }
        let ended = {
            let mut a = lock(&self.platform.accounting);
            if let Some(since) = a.since.take() {
                a.total += self.platform.clock.steady_seconds() - since;
            }
            // Unwatched meanwhile: the host is being destroyed, which answers a query that waited through this.
            if a.glue != Some(std::thread::current().id()) {
                return;
            }
            a.hearing += 1;
            a.wait_ended.clone()
        };
        if let Some(ended) = ended {
            ended();
        }
        lock(&self.platform.accounting).hearing -= 1;
        self.platform.idle.notify_all();
    }
}

impl HostedPlatform {
    fn new(sink: Arc<dyn SandboxRemoteSink>, clock: Arc<dyn Platform>) -> HostedPlatform {
        HostedPlatform {
            sink,
            clock,
            requesting: Mutex::new(()),
            closed: AtomicBool::new(false),
            accounting: Mutex::new(Accounting { glue: None, wait_ended: None, hearing: 0, total: 0.0, since: None }),
            idle: Condvar::new(),
            on_message: Mutex::new(None),
        }
    }

    /// A message for the host's platform, from any thread; the host gets them in the order they were sent.
    pub fn send(&self, message: &[u8]) {
        if !self.closed.load(Ordering::SeqCst) {
            self.sink.remote_platform_message(message);
        }
    }

    /// A request the renderer waits for, from any thread, the glue's host thread included, but never inside the
    /// factory: the host's answer, or none once the runtime closed or the transport ended (also for a request that
    /// was waiting then, and at once for one made after the close, neither counted nor behind the turn of a
    /// request still under way). One waits at a time per widget: a second waits here for the first.
    pub fn request(&self, message: &[u8]) -> Option<Vec<u8>> {
        if self.closed.load(Ordering::SeqCst) {
            return None;
        }
        let _counted = GlueWait::new(self);
        let _turn = lock(&self.requesting);
        if self.closed.load(Ordering::SeqCst) {
            return None;
        }
        self.sink.remote_platform_request(message)
    }

    /// The host's platform messages for the renderer, in order, on the glue's host thread (`onMessage`); set it
    /// when the renderer is built, before the factory returns.
    pub fn set_on_message(&self, observer: impl Fn(&[u8]) + Send + Sync + 'static) {
        *lock(&self.on_message) = Some(Arc::new(observer));
    }

    /// The host's message to the renderer's observer, if one is set.
    fn deliver(&self, message: &[u8]) {
        let observer = lock(&self.on_message).clone();
        if let Some(observer) = observer {
            observer(message);
        }
    }

    /// From now on no message goes out, and every request, one waiting for its turn too, answers none.
    fn close(&self) {
        self.closed.store(true, Ordering::SeqCst);
    }

    /// Once, before the served host starts: its glue thread, whose time in `request` counts, and what hears each
    /// such wait end (on that thread, outside the accounting's lock, the count already updated).
    fn watch_glue(&self, glue: Option<ThreadId>, ended: Arc<dyn Fn() + Send + Sync>) {
        let mut a = lock(&self.accounting);
        a.glue = glue;
        a.wait_ended = Some(ended);
    }

    /// While the served host and its glue thread still live, before the join that frees that thread's id: from
    /// now on no wait in `request` is counted from it or heard, and once this returns an `ended` under way has
    /// returned too, so none reaches the host after it.
    fn unwatch_glue(&self) {
        let mut a = lock(&self.accounting);
        a.glue = None;
        while a.hearing != 0 {
            a = self.idle.wait(a).unwrap_or_else(std::sync::PoisonError::into_inner);
        }
        a.wait_ended = None;
    }

    /// The glue thread's time in `request` to now, a wait in progress included, and whether one is, read
    /// together.
    fn waited(&self) -> GlueWaited {
        let a = lock(&self.accounting);
        let now = self.clock.steady_seconds();
        GlueWaited { at: now, total: a.total + a.since.map_or(0.0, |since| now - since), waiting: a.since.is_some() }
    }
}

const NOT_BUILT: &str = "the widget could not be built in the engine process: ";

type Input = Box<dyn FnOnce(&ServedHost) + Send>;

/// The runtime's lock: whether it started, closed, its served engine died (only the renderer kept), the inputs
/// kept until the host is built, and the host.
struct Gate {
    started: bool,
    closed: bool,
    engine_ended: bool,
    waiting: Vec<Input>,
    host: Option<Arc<ServedHost>>,
}

/// The served engine's delegate (the C++ runtime's own `SandboxEngineDelegate` half): the render calls go to the
/// served host's glue, the engine's other calls to the never-started forwarder, the load and the death through
/// the served host's thread first, so they reach the sink after what the glue sent before them.
struct RuntimeDelegate {
    served: Weak<Served>,
    gate: Weak<Mutex<Gate>>,
    forwarder: Arc<dyn EngineDelegate>,
    sink: Arc<dyn SandboxRemoteSink>,
}

impl EngineDelegate for RuntimeDelegate {
    fn on_ops(&self, engine: &Engine, json: &[u8]) {
        if let Some(s) = self.served.upgrade() {
            s.on_ops(engine, json);
        }
    }
    fn on_query(&self, engine: &Engine, json: &[u8]) -> QueryAnswer {
        match self.served.upgrade() {
            Some(s) => s.on_query(engine, json),
            None => QueryAnswer { json: b"{\"error\":\"dead\"}".to_vec(), ..QueryAnswer::default() },
        }
    }
    fn on_listen(&self, engine: &Engine, json: &[u8]) {
        if let Some(s) = self.served.upgrade() {
            s.on_listen(engine, json);
        }
    }
    fn on_prevent_default(&self, engine: &Engine, json: &[u8]) {
        if let Some(s) = self.served.upgrade() {
            s.on_prevent_default(engine, json);
        }
    }
    fn on_anim(&self, engine: &Engine, json: &[u8]) {
        if let Some(s) = self.served.upgrade() {
            s.on_anim(engine, json);
        }
    }
    fn on_request_frame(&self, engine: &Engine) {
        if let Some(s) = self.served.upgrade() {
            s.on_request_frame(engine);
        }
    }
    fn on_log(&self, engine: &Engine, level: &[u8], text: &[u8]) {
        self.forwarder.on_log(engine, level, text);
    }
    fn on_diag(&self, engine: &Engine, kind: &[u8], detail: &[u8]) {
        self.forwarder.on_diag(engine, kind, detail);
    }
    fn on_bridge_action(&self, engine: &Engine, action: &WidgetAction) {
        self.forwarder.on_bridge_action(engine, action);
    }
    fn on_bridge(&self, engine: &Engine, method: &[u8], args_json: &[u8]) {
        self.forwarder.on_bridge(engine, method, args_json);
    }
    fn on_bridge_copy_text(&self, engine: &Engine, text: &[u8]) {
        self.forwarder.on_bridge_copy_text(engine, text);
    }
    fn on_storage(&self, engine: &Engine, area: &[u8], values: &StorageArea) {
        self.forwarder.on_storage(engine, area, values);
    }
    fn on_resize_anchor(&self, engine: &Engine, edge: &[u8]) {
        self.forwarder.on_resize_anchor(engine, edge);
    }
    fn on_public_data_watch(&self, engine: &Engine, on: bool) {
        self.forwarder.on_public_data_watch(engine, on);
    }
    fn on_bridge_request(&self, engine: &Engine, request: &WidgetRequest, respond: WidgetRequestResponder) {
        self.forwarder.on_bridge_request(engine, request, respond);
    }
    fn on_bridge_wallet(&self, engine: &Engine, call: &WalletCall, respond: WidgetRequestResponder) {
        self.forwarder.on_bridge_wallet(engine, call, respond);
    }
    fn on_stats(&self, engine: &Engine, stats: &SandboxStats) {
        self.forwarder.on_stats(engine, stats);
    }
    // The `Loaded` report after the loading tasks' batches and node counts, then the glue's render acknowledgement.
    fn on_loaded(&self, _engine: &Engine) {
        let Some(s) = self.served.upgrade() else { return };
        let sink = Arc::clone(&self.sink);
        s.host(move |_, _| sink.remote_loaded());
        s.on_loaded();
    }
    // The death after the glue's reports before it (a validator's `limit` among them), then the renderer frozen
    // with the host's stopped label, then the cut that keeps the renderer alone: in the glue thread's order, so
    // the death's reports and `frozen` come first.
    fn on_died(&self, _engine: &Engine, reason: &[u8]) {
        let Some(s) = self.served.upgrade() else { return };
        let (sink, died) = (Arc::clone(&self.sink), reason.to_vec());
        s.host(move |_, _| sink.remote_died(&died));
        s.on_died(reason);
        if let Some(gate) = self.gate.upgrade() {
            lock(&gate).engine_ended = true;
        }
        s.host(|s, _| s.keep_renderer_only());
    }
}

/// One hosted widget on the engine's side (remote_hosted.cpp's `SandboxHostedRuntime`): from its start a served
/// host (the glue between its engine, validator and renderer, as in process) whose engine has the runtime's
/// delegate. The render calls go to the served host's glue, the engine's other calls to an unstarted
/// `SandboxRemoteServer` that relays them to the sink and numbers its requests and wallet calls for `answer`; the
/// glue's reports, the asked-for batches and the renderer's platform traffic go to the sink directly. At its
/// engine's death (a limit of its own, the host's `stop`) the renderer stays, without engine or validator, until
/// `close` and the owner's drop: after the death's reports the served host's cut frees them on the glue's thread,
/// every input but the renderer's platform traffic is dropped and a probe answers "ERROR no runtime", while that
/// traffic still crosses both ways.
pub struct SandboxHostedRuntime {
    sink: Arc<dyn SandboxRemoteSink>,
    resources: Arc<SandboxResources>,
    renderers: Option<HostedRendererFactory>,
    engine_platform: Arc<dyn Platform>,
    /// Never started: the relay of the engine's own calls and the numbering of its requests and wallet calls.
    forwarder: SandboxRemoteServer,
    gate: Arc<Mutex<Gate>>,
    platform: Arc<HostedPlatform>,
}

impl std::fmt::Debug for SandboxHostedRuntime {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let g = lock(&self.gate);
        f.debug_struct("SandboxHostedRuntime").field("started", &g.started).field("closed", &g.closed).finish()
    }
}

impl SandboxHostedRuntime {
    /// A runtime that sends every call of its widget to `sink`, its widget built from `resources` with
    /// `renderers`' renderer (none: a widget that dies, the factory having built nothing), on `platform`'s clocks.
    pub fn new(
        sink: Arc<dyn SandboxRemoteSink>,
        resources: Arc<SandboxResources>,
        renderers: Option<HostedRendererFactory>,
        platform: Arc<dyn Platform>,
    ) -> SandboxHostedRuntime {
        let forwarder = SandboxRemoteServer::new(Arc::clone(&sink), Arc::clone(&resources), Arc::clone(&platform));
        let hosted = Arc::new(HostedPlatform::new(Arc::clone(&sink), Arc::clone(&platform)));
        SandboxHostedRuntime {
            sink,
            resources,
            renderers,
            engine_platform: platform,
            forwarder,
            gate: Arc::new(Mutex::new(Gate { started: false, closed: false, engine_ended: false, waiting: Vec::new(), host: None })),
            platform: hosted,
        }
    }

    /// The served host for `start` (what failed, as WidgetHost's constructor throws it).
    fn build(&self, html: &[u8], config: SandboxConfig, start: SandboxHostedStart) -> Result<Arc<ServedHost>, String> {
        let renderers = self.renderers.clone();
        let platform = Arc::clone(&self.platform);
        let factory: RendererFactory = Box::new(move |resources, theme, width| match &renderers {
            None => Ok(None),
            Some(renderers) => renderers(resources, theme, width, platform),
        });
        let (diag, ready, rendered, nodes) =
            (Arc::clone(&self.sink), Arc::clone(&self.sink), Arc::clone(&self.sink), Arc::clone(&self.sink));
        let reports = GlueReports {
            diag: Some(Box::new(move |kind, detail| diag.remote_diag(kind, detail))),
            renderer_ready: Some(Box::new(move || ready.remote_renderer_ready())),
            rendered: Some(Box::new(move || rendered.remote_rendered())),
            nodes: Some(Box::new(move |n| nodes.remote_nodes(i64::from(n)))),
        };
        let intl: Arc<dyn IntlProvider> = Arc::new(RemoteIntl::new(Arc::clone(&self.sink)));
        let (gate, forwarder, sink) = (Arc::downgrade(&self.gate), self.forwarder.delegate(), Arc::clone(&self.sink));
        let host = ServedHost::new(
            html.to_vec(),
            start.theme,
            config,
            Arc::clone(&self.resources),
            factory,
            intl,
            move |served| Arc::new(RuntimeDelegate { served, gate, forwarder, sink }),
            reports,
            Arc::clone(&self.engine_platform),
        )?;
        host.set_stopped_label(start.stopped_label);
        if start.validated_ops {
            let sink = Arc::clone(&self.sink);
            host.set_on_render_ops(Box::new(move |json| sink.remote_validated_ops(json)));
        }
        // Only the glue's own waits in its platform requests leave its layout queries' time; `ended` is called on
        // the glue thread alone, until the drop unwatches it before the host goes.
        let platform = Arc::clone(&self.platform);
        host.set_glue_waits(Box::new(move || platform.waited()));
        let served = Arc::downgrade(host.served());
        self.platform.watch_glue(
            host.host_thread_id(),
            Arc::new(move || {
                if let Some(s) = served.upgrade() {
                    s.glue_wait_ended();
                }
            }),
        );
        Ok(Arc::new(host))
    }

    /// `call` with the served host: kept until it is built, dropped after `close` and, unless it is the renderer's
    /// platform traffic, once the engine ended; the lock only decides, the host is called after it.
    fn input(&self, call: impl FnOnce(&ServedHost) + Send + 'static, renderer_traffic: bool) {
        let host = {
            let mut g = lock(&self.gate);
            if g.closed || (g.engine_ended && !renderer_traffic) {
                return;
            }
            match &g.host {
                None => return g.waiting.push(Box::new(call)),
                Some(host) => Arc::clone(host),
            }
        };
        call(&host);
    }
}

impl HostedRuntime for SandboxHostedRuntime {
    fn start(&self, html: &[u8], config: SandboxConfig, start: SandboxHostedStart) {
        {
            let mut g = lock(&self.gate);
            if g.closed || g.started {
                return;
            }
            g.started = true;
        }
        let host = match self.build(html, config, start) {
            Ok(host) => host,
            Err(failure) => {
                {
                    let mut g = lock(&self.gate);
                    g.closed = true;
                    g.waiting.clear();
                }
                self.platform.close();
                self.forwarder.close();
                self.sink.remote_died(format!("{NOT_BUILT}{failure}").as_bytes());
                return;
            }
        };
        let (waiting, closed) = {
            let mut g = lock(&self.gate);
            g.host = Some(Arc::clone(&host));
            (std::mem::take(&mut g.waiting), g.closed)
        };
        // Closed while it was built: it never starts and goes with the runtime.
        if closed {
            return;
        }
        for call in waiting {
            call(&host);
        }
        host.start();
    }

    fn theme(&self, theme: WidgetTheme) {
        self.input(move |host| host.set_theme(theme), false);
    }

    fn content_safe_area(&self, area: WidgetContentSafeArea) {
        self.input(move |host| host.set_content_safe_area(area), false);
    }

    fn set_data(&self, data: WidgetData) {
        self.input(move |host| host.set_data(&data), false);
    }

    fn set_visible(&self, visible: bool) {
        self.input(move |host| host.set_visible(visible), false);
    }

    fn stop(&self, reason: &[u8]) {
        let reason = reason.to_vec();
        self.input(move |host| host.stop_engine(&reason), false);
    }

    fn answer(&self, id: i64, outcome: &SandboxRemoteOutcome) {
        self.forwarder.answer(id, outcome);
    }

    fn probe(&self, code: &[u8]) -> Vec<u8> {
        let host = {
            let g = lock(&self.gate);
            if !g.closed && !g.engine_ended { g.host.clone() } else { None }
        };
        let Some(host) = host else { return b"ERROR no runtime".to_vec() };
        // A barrier both ways, as in process: the inputs before the probe posted their engine work from the glue's
        // thread first, and what the glue made of the engine's calls before it is on the sink before its answer.
        host.on_host_thread();
        let answer = host.probe(code);
        host.on_host_thread();
        answer
    }

    fn platform(&self, message: &[u8]) {
        let (platform, message) = (Arc::clone(&self.platform), message.to_vec());
        self.input(move |host| host.host(move || platform.deliver(&message)), true);
    }

    fn close(&self) {
        let host = {
            let mut g = lock(&self.gate);
            if std::mem::replace(&mut g.closed, true) {
                return;
            }
            g.waiting.clear();
            g.host.clone()
        };
        self.platform.close();
        self.forwarder.close();
        // A released engine (its death's cut) drops it.
        if let Some(host) = host {
            host.terminate_engine();
        }
    }
}

impl Drop for SandboxHostedRuntime {
    /// Closes, stops the platform's watch of the glue thread, then drops the served host, joining its threads
    /// while this runtime's delegate and forwarder still exist.
    fn drop(&mut self) {
        self.close();
        let host = lock(&self.gate).host.take();
        // While the host and its glue thread live: a renderer that keeps the platform reaches neither from here on,
        // its glue thread's id free for another thread after the join.
        self.platform.unwatch_glue();
        drop(host);
    }
}

/// Serves one stream until it ends, on the calling thread (`SandboxRemoteEngineEnd::serveHosting`): a hosting
/// opening's runtimes are hosted widgets whose renderers `renderers` builds (none: every widget dies, its factory
/// having built nothing), an engine opening's are engines as `twidget_server::serve` serves them.
pub fn serve_hosting(
    stream: Arc<dyn Stream>,
    build: &[u8],
    lockdown: Option<&dyn Fn() -> Vec<u8>>,
    platform: Arc<dyn Platform>,
    renderers: Option<HostedRendererFactory>,
) -> Vec<u8> {
    let engines = Arc::clone(&platform);
    let runtimes = move |sink: Arc<dyn SandboxRemoteSink>, resources: Arc<SandboxResources>| -> Arc<dyn HostedRuntime> {
        Arc::new(SandboxHostedRuntime::new(sink, resources, renderers.clone(), Arc::clone(&engines)))
    };
    twidget_server::serve_hosting(stream, build, lockdown, platform, &runtimes)
}
