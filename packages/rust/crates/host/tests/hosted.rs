//! `serve_hosting` end to end on the shipped bundles (packages/desktop/WidgetJS) over an in-memory stream, with a
//! renderer of the test's: what the C++ suite cannot reach of the hosted end in Rust (the Rust renderer seam, the
//! hosting refusals of either opening, the factory's failures as the reasons the host reads), and the glue's small
//! helpers. The rest runs through the C++ suite against this end (`TW_ENGINE_HOSTED=stream TW_ENGINE_END=rust`).

use std::collections::VecDeque;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use twidget_bundle_stamp::SandboxResources;
use twidget_engine::{SandboxConfig, SandboxHostedStart};
use twidget_host::*;
use twidget_js::{Platform, now};
use twidget_json::Json;
use twidget_server::coding::{encode_config, encode_hosted_start};
use twidget_stream::{Field, FrameReader, Kind, MAX_FRAME_BYTES, Side, Stream, VERSION, encode};

const BUILD: &[u8] = b"rust-hosted-tests";

/// A config and a hosted start as a client wrote them before the bot's profile page (no `context.surface`, no
/// `safeArea`, no `viewportHeight`), by hand, so the defaults are read from the form an older client sends and not
/// from what this workspace's encoder writes: a bubble 300 px wide whose host asks for the validated batches.
const LEGACY_CONFIG: &str = concat!(
    "{\"memoryLimitBytes\":8388608,\"cpuLimitMs\":500,\"cpuQuotaMs\":300,\"cpuQuotaWindowMs\":1000,\"cpuKillAfterMs\":0,",
    "\"mainThreadQuotaMs\":100,\"maxTimers\":64,\"visible\":true,\"innerWidth\":300,\"innerHeight\":300,\"devicePixelRatio\":1,",
    "\"theme\":\"light\",\"themeParams\":{},\"locale\":\"en-US\",",
    "\"contentSafeArea\":{\"top\":0,\"right\":0,\"bottom\":0,\"left\":0,\"cornerLeft\":0,\"cornerRight\":0},",
    "\"storage\":{\"local\":{},\"session\":{}},\"data\":{\"chatData\":null,\"publicData\":null},",
    "\"context\":{\"isCopy\":false,\"canQuery\":false,\"backgroundQuery\":false,\"preview\":false,\"wallet\":false,\"chatType\":\"\"},",
    "\"policy\":\"strict\",\"features\":[]}"
);
const LEGACY_START: &str = concat!(
    "{\"theme\":{\"colorScheme\":\"light\",\"params\":{},\"button\":{},\"text\":{}},",
    "\"stoppedLabel\":\"Widget stopped\",\"validatedOps\":true}"
);

struct TestPlatform;

impl Platform for TestPlatform {
    fn thread_cpu_seconds(&self) -> f64 {
        now()
    }
    fn time_zone_offset(&self, _: i64) -> i32 {
        0
    }
    fn random(&self, bytes: &mut [u8]) {
        for (i, b) in bytes.iter_mut().enumerate() {
            *b = (i * 7 + 3) as u8;
        }
    }
}

/// One direction of the in-memory pair.
#[derive(Default)]
struct Pipe {
    state: Mutex<(VecDeque<u8>, bool)>,
    changed: Condvar,
}

impl Pipe {
    fn close(&self) {
        self.state.lock().unwrap().1 = true;
        self.changed.notify_all();
    }
}

/// One end of the pair: reads its inbox, writes the other's; `shutdown` ends both directions.
struct End {
    inbox: Arc<Pipe>,
    outbox: Arc<Pipe>,
}

impl Stream for End {
    fn read(&self, out: &mut [u8]) -> usize {
        let mut st = self.inbox.state.lock().unwrap();
        while st.0.is_empty() && !st.1 {
            st = self.inbox.changed.wait(st).unwrap();
        }
        let n = out.len().min(st.0.len());
        for (i, b) in st.0.drain(..n).enumerate() {
            out[i] = b;
        }
        n
    }
    fn write(&self, bytes: &[u8]) -> bool {
        let mut st = self.outbox.state.lock().unwrap();
        if st.1 {
            return false;
        }
        st.0.extend(bytes);
        self.outbox.changed.notify_all();
        true
    }
    fn shutdown(&self) {
        self.inbox.close();
        self.outbox.close();
    }
}

fn pair() -> (Arc<End>, Arc<End>) {
    let (a, b) = (Arc::new(Pipe::default()), Arc::new(Pipe::default()));
    (Arc::new(End { inbox: Arc::clone(&a), outbox: Arc::clone(&b) }), Arc::new(End { inbox: b, outbox: a }))
}

fn resources() -> SandboxResources {
    let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../desktop/WidgetJS");
    SandboxResources::from_directory(dir).expect("the desktop bundles (pnpm build:desktop)")
}

/// A frame the engine's end sent, owned.
#[derive(Clone, Debug)]
struct Seen {
    runtime: u32,
    kind: Kind,
    text: Vec<Vec<u8>>,
    number: i64,
}

impl Seen {
    fn first(&self) -> String {
        String::from_utf8_lossy(self.text.first().map_or(&b""[..], Vec::as_slice)).into_owned()
    }
}

/// The host's side: its writes, and a reader that keeps every frame and answers each platform request
/// `A:<request>`.
struct Host {
    end: Arc<End>,
    writing: Mutex<()>,
    seen: Arc<(Mutex<Vec<Seen>>, Condvar)>,
    reader: Option<std::thread::JoinHandle<()>>,
}

impl Host {
    fn new(end: Arc<End>) -> Host {
        let seen: Arc<(Mutex<Vec<Seen>>, Condvar)> = Arc::default();
        let (reading, kept) = (Arc::clone(&end), Arc::clone(&seen));
        let reader = std::thread::spawn(move || {
            let mut frames = FrameReader::new(&*reading, Side::Engine);
            while let Ok(Some(f)) = frames.next(MAX_FRAME_BYTES) {
                let text: Vec<Vec<u8>> = f.str.iter().map(|s| s.to_vec()).collect();
                let s = Seen { runtime: f.runtime, kind: f.kind, text, number: f.i64 };
                if s.kind == Kind::PlatformRequest {
                    let answer = [&b"A:"[..], &s.text[0]].concat();
                    if let Some(frame) = encode(s.runtime, Kind::PlatformAnswer, MAX_FRAME_BYTES, &[Field::Str(&answer)]) {
                        reading.write(&frame);
                    }
                }
                kept.0.lock().unwrap().push(s);
                kept.1.notify_all();
            }
        });
        Host { end, writing: Mutex::new(()), seen, reader: Some(reader) }
    }

    fn send(&self, runtime: u32, kind: Kind, fields: &[Field]) {
        let frame = encode(runtime, kind, MAX_FRAME_BYTES, fields).expect("a frame of its kind's shape");
        let _w = self.writing.lock().unwrap();
        assert!(self.end.write(&frame), "the engine's end is gone");
    }

    /// An engine opening carries the sandbox bundle and the other two bundles' stamp lines, a hosting one the
    /// sandbox and validator bundles whole and the renderer's stamp line.
    fn opening(&self, kind: Kind, resources: &SandboxResources) {
        let stamp = |bundle: &[u8]| bundle.split(|&c| c == b'\n').next().unwrap_or(b"").to_vec();
        let renderer = stamp(&resources.renderer_bundle);
        let validator = if kind == Kind::HostingOpening { resources.validator_bundle.clone() } else { stamp(&resources.validator_bundle) };
        self.send(
            0,
            kind,
            &[Field::I64(VERSION), Field::Str(BUILD), Field::Str(&resources.sandbox_bundle), Field::Str(&validator), Field::Str(&renderer)],
        );
    }

    fn hosted_start(&self, runtime: u32, html: &str, validated_ops: bool) {
        let config = SandboxConfig { inner_width: 300.0, ..SandboxConfig::default() };
        let start = SandboxHostedStart { validated_ops, ..SandboxHostedStart::default() };
        self.hosted_start_forms(runtime, html, &encode_config(&config), &encode_hosted_start(&start));
    }

    /// The route opened and its hosted start with these coded forms.
    fn hosted_start_forms(&self, runtime: u32, html: &str, config: &[u8], start: &[u8]) {
        self.send(runtime, Kind::Open, &[]);
        self.send(runtime, Kind::HostedStart, &[Field::Str(html.as_bytes()), Field::Str(config), Field::Str(start)]);
    }

    fn all(&self) -> Vec<Seen> {
        self.seen.0.lock().unwrap().clone()
    }

    /// The first frame `pick` takes, waiting up to 20 s.
    fn wait(&self, what: &str, pick: impl Fn(&Seen) -> bool) -> Seen {
        let deadline = Instant::now() + Duration::from_secs(20);
        let mut seen = self.seen.0.lock().unwrap();
        loop {
            if let Some(s) = seen.iter().find(|s| pick(s)) {
                return s.clone();
            }
            let left = deadline.saturating_duration_since(Instant::now());
            assert!(
                !left.is_zero(),
                "no {what} within 20 s: {:?}",
                seen.iter().map(|s| (s.runtime, s.kind, s.first())).collect::<Vec<_>>()
            );
            seen = self.seen.1.wait_timeout(seen, left).unwrap().0;
        }
    }

    fn count(&self, runtime: u32, kind: Kind) -> usize {
        self.all().iter().filter(|s| s.runtime == runtime && s.kind == kind).count()
    }

    fn finish(mut self) {
        self.end.shutdown();
        if let Some(reader) = self.reader.take() {
            reader.join().unwrap();
        }
    }
}

/// The test's renderer: `ready` on load, every engine query answered with an empty rect list, the glue's
/// messages kept; its platform's `ask:<x>` asks x on the glue's thread and sends `got:<answer>`.
struct TestRenderer {
    link: Mutex<Option<RendererLink>>,
    sent: Arc<Mutex<Vec<String>>>,
}

impl TestRenderer {
    fn tell(&self, json: &str) {
        let link = self.link.lock().unwrap().clone();
        if let Some(link) = link {
            link.message(json.as_bytes());
        }
    }
}

impl Renderer for TestRenderer {
    fn connect(&self, link: RendererLink) {
        *self.link.lock().unwrap() = Some(link);
    }
    fn load(&self) {
        self.tell("{\"type\":\"ready\"}");
    }
    fn send(&self, json: &[u8]) {
        let text = String::from_utf8_lossy(json).into_owned();
        self.sent.lock().unwrap().push(text);
        let Some(m) = Json::parse(json) else { return };
        if m.get("type").as_str() == b"query" {
            let qid = m.get("qid").as_int(0);
            self.tell(&format!("{{\"type\":\"result\",\"qid\":{qid},\"result\":{{\"rects\":[]}}}}"));
        }
    }
    fn set_theme(&self, _: &[u8]) {}
    fn set_width(&self, _: f64) {}
    fn destroy(&self) {}
}

fn test_renderers(sent: Arc<Mutex<Vec<String>>>, built: Arc<AtomicUsize>) -> HostedRendererFactory {
    Arc::new(move |_resources, theme, width, platform| {
        assert_eq!((theme, width), (&b"light"[..], 300.0));
        built.fetch_add(1, Ordering::SeqCst);
        let asked = Arc::clone(&platform);
        platform.set_on_message(move |message| {
            if let Some(question) = message.strip_prefix(b"ask:") {
                let answer = asked.request(question).unwrap_or_else(|| b"none".to_vec());
                asked.send(&[&b"got:"[..], &answer].concat());
            }
        });
        Ok(Some(Box::new(TestRenderer { link: Mutex::new(None), sent: Arc::clone(&sent) }) as Box<dyn Renderer>))
    })
}

fn serving(engine: Arc<End>, renderers: Option<HostedRendererFactory>) -> std::thread::JoinHandle<Vec<u8>> {
    std::thread::spawn(move || serve_hosting(engine, BUILD, None, Arc::new(TestPlatform), renderers))
}

#[test]
fn serves_a_hosted_widget_and_keeps_its_renderer_past_its_death() {
    let res = resources();
    let (host_end, engine_end) = pair();
    let sent = Arc::new(Mutex::new(Vec::new()));
    let built = Arc::new(AtomicUsize::new(0));
    let served = serving(engine_end, Some(test_renderers(Arc::clone(&sent), Arc::clone(&built))));
    let host = Host::new(host_end);
    host.opening(Kind::HostingOpening, &res);
    host.wait("Ready", |s| s.kind == Kind::Ready);
    // An older client's forms: a bubble.
    host.hosted_start_forms(1, "<p id='p'>hello</p>", LEGACY_CONFIG.as_bytes(), LEGACY_START.as_bytes());
    host.wait("Rendered", |s| s.runtime == 1 && s.kind == Kind::Rendered);
    assert_eq!(built.load(Ordering::SeqCst), 1);
    for kind in [Kind::RendererReady, Kind::Loaded, Kind::Nodes] {
        assert_eq!(host.count(1, kind), 1, "{kind:?}");
    }
    let ops = host.wait("ValidatedOps", |s| s.kind == Kind::ValidatedOps);
    assert!(ops.first().contains("hello"), "{}", ops.first());
    // What stays in the engine process never crosses: the render calls, the engine's tasks.
    for kind in [Kind::Ops, Kind::Query, Kind::Listen, Kind::RequestsFrame, Kind::TaskStarted, Kind::TaskCompleted] {
        assert_eq!(host.count(1, kind), 0, "{kind:?}");
    }
    // The served host's start, in the C++ glue's order (widget_host_local.cpp's `localStart`), all before the first
    // ops: the theme, the place with its viewport, the content's safe area, the device's, the policy.
    let started = sent.lock().unwrap().clone();
    assert!(started.len() > 5 && started[0].starts_with("{\"type\":\"theme\""), "{started:?}");
    assert_eq!(
        started[1..5],
        [
            "{\"type\":\"surface\",\"surface\":\"message\",\"viewportHeight\":0}",
            "{\"type\":\"safeArea\",\"inset\":{\"top\":0,\"right\":0,\"bottom\":0,\"left\":0,\"cornerLeft\":0,\"cornerRight\":0}}",
            "{\"type\":\"deviceSafeArea\",\"inset\":{\"top\":0,\"right\":0,\"bottom\":0,\"left\":0}}",
            "{\"type\":\"policy\",\"policy\":\"strict\"}",
        ]
    );
    let first_ops = started.iter().position(|m| m.starts_with("{\"type\":\"ops\""));
    assert!(first_ops.is_some_and(|at| at >= 5), "{started:?}");

    host.send(1, Kind::Probe, &[Field::I64(1), Field::Str(b"document.getElementById('p').textContent")]);
    assert_eq!(host.wait("the probe's answer", |s| s.kind == Kind::ProbeAnswer && s.number == 1).first(), "hello");

    // The renderer's request on the glue's thread, answered by the host.
    host.send(1, Kind::Platform, &[Field::Str(b"ask:q1")]);
    host.wait("got:A:q1", |s| s.kind == Kind::PlatformMessage && s.first() == "got:A:q1");

    // The host stops it: its death, then the renderer frozen with the label, the probe refused, while the
    // renderer's platform traffic still crosses both ways.
    host.send(1, Kind::Stop, &[Field::Str(b"stopped by the test")]);
    assert_eq!(host.wait("Died", |s| s.kind == Kind::Died).first(), "stopped by the test");
    let deadline = Instant::now() + Duration::from_secs(5);
    while !sent.lock().unwrap().iter().any(|m| m.contains("\"frozen\"")) {
        assert!(Instant::now() < deadline, "no frozen: {:?}", sent.lock().unwrap());
        std::thread::sleep(Duration::from_millis(10));
    }
    host.send(1, Kind::Probe, &[Field::I64(2), Field::Str(b"1 + 1")]);
    assert_eq!(host.wait("the second probe's answer", |s| s.kind == Kind::ProbeAnswer && s.number == 2).first(), "ERROR no runtime");
    host.send(1, Kind::Platform, &[Field::Str(b"ask:q2")]);
    host.wait("got:A:q2", |s| s.kind == Kind::PlatformMessage && s.first() == "got:A:q2");

    host.send(1, Kind::Close, &[]);
    host.finish();
    assert_eq!(served.join().unwrap(), b"");
}

#[test]
fn a_hosting_stream_takes_only_its_kinds_and_an_engine_stream_none_of_them() {
    let res = resources();
    // A hosting stream: an engine's start ends serving.
    let (host_end, engine_end) = pair();
    let served = serving(engine_end, None);
    let host = Host::new(host_end);
    host.opening(Kind::HostingOpening, &res);
    host.wait("Ready", |s| s.kind == Kind::Ready);
    host.send(1, Kind::Open, &[]);
    host.send(1, Kind::Start, &[Field::Str(b"<p>x</p>"), Field::Str(b"{}")]);
    assert_eq!(String::from_utf8_lossy(&served.join().unwrap()), "kind 3 on runtime 1, which a hosting stream does not take");
    host.finish();

    // An engine opening on the hosting end: an engine stream, where a hosted start ends serving.
    let (host_end, engine_end) = pair();
    let served = serving(engine_end, None);
    let host = Host::new(host_end);
    host.opening(Kind::Opening, &res);
    host.wait("Ready", |s| s.kind == Kind::Ready);
    host.hosted_start(1, "<p>x</p>", false);
    assert_eq!(String::from_utf8_lossy(&served.join().unwrap()), "kind 33 on runtime 1, which only a hosting stream takes");
    host.finish();

    // The engine-only end refuses a hosting opening as another first kind.
    let (host_end, engine_end) = pair();
    let served = std::thread::spawn(move || twidget_server::serve(engine_end, BUILD, None, Arc::new(TestPlatform)));
    let host = Host::new(host_end);
    host.opening(Kind::HostingOpening, &res);
    assert_eq!(host.wait("Refused", |s| s.kind == Kind::Refused).first(), "kind 32 first, not the opening");
    assert_eq!(String::from_utf8_lossy(&served.join().unwrap()), "kind 32 first, not the opening");
    host.finish();
}

#[test]
fn a_factory_that_builds_nothing_or_fails_ends_only_its_widget() {
    let res = resources();
    let (host_end, engine_end) = pair();
    let calls = Arc::new(AtomicUsize::new(0));
    let counted = Arc::clone(&calls);
    let renderers: HostedRendererFactory = Arc::new(move |_, _, _, _| match counted.fetch_add(1, Ordering::SeqCst) {
        0 => Ok(None),
        _ => Err("the test's renderer could not start".to_string()),
    });
    let served = serving(engine_end, Some(renderers));
    let host = Host::new(host_end);
    host.opening(Kind::HostingOpening, &res);
    host.wait("Ready", |s| s.kind == Kind::Ready);
    host.hosted_start(1, "<p>a</p>", false);
    assert_eq!(
        host.wait("runtime 1's death", |s| s.runtime == 1 && s.kind == Kind::Died).first(),
        "the widget could not be built in the engine process: WidgetHost: the renderer factory returned nothing"
    );
    host.hosted_start(2, "<p>b</p>", false);
    assert_eq!(
        host.wait("runtime 2's death", |s| s.runtime == 2 && s.kind == Kind::Died).first(),
        "the widget could not be built in the engine process: the test's renderer could not start"
    );
    // A runtime whose build failed is closed: its probe answers as a closed one.
    host.send(2, Kind::Probe, &[Field::I64(1), Field::Str(b"1")]);
    assert_eq!(host.wait("the probe's answer", |s| s.kind == Kind::ProbeAnswer).first(), "ERROR no runtime");
    host.finish();
    assert_eq!(served.join().unwrap(), b"");
    assert_eq!(calls.load(Ordering::SeqCst), 2);
}

#[test]
fn base64_reads_as_the_glue_reads_it() {
    assert_eq!(base64_decode(b"aGVsbG8="), Some(b"hello".to_vec()));
    assert_eq!(base64_decode(b"aGVs\r\nbG8"), Some(b"hello".to_vec()));
    assert_eq!(base64_decode(b""), Some(Vec::new()));
    assert_eq!(base64_decode(b"aGV*bG8="), None);
    // Padding is skipped wherever it is, as the C++ skips it.
    assert_eq!(base64_decode(b"a=GVsbG8"), Some(b"hello".to_vec()));
}

#[test]
fn gestures_count_presses_not_keys() {
    let ev = |t: &str, detail: f64| {
        let mut e = Json::object();
        e.set("type", t).set("detail", detail);
        e
    };
    let mut g = GestureCounter::default();
    assert_eq!(g.gesture_for(&ev("pointerdown", 0.0)), 1);
    assert_eq!(g.gesture_for(&ev("mousedown", 0.0)), 1);
    assert_eq!(g.gesture_for(&ev("pointerup", 0.0)), 1);
    assert_eq!(g.gesture_for(&ev("click", 1.0)), 1);
    assert_eq!(g.gesture_for(&ev("dblclick", 2.0)), 1);
    // A keyboard click (detail 0) is no gesture and ends the chain.
    assert_eq!(g.gesture_for(&ev("click", 0.0)), 0);
    assert_eq!(g.gesture_for(&ev("dblclick", 2.0)), 2);
    assert_eq!(g.gesture_for(&ev("keydown", 0.0)), 0);
    g.press_began();
    assert_eq!(g.gesture_for(&ev("pointerup", 0.0)), 3);
    assert!(!GestureCounter::is_user_activation(&ev("click", 0.0)));
    assert!(GestureCounter::is_user_activation(&ev("mouseup", 0.0)));
}

#[test]
fn paint_limits_read_and_say_as_the_policy_does() {
    assert_eq!(paint_limit_from_code(b"paint-limit"), Some(WidgetPaintLimit::Paint));
    assert_eq!(paint_limit_from_code(b"backdrop-limit"), Some(WidgetPaintLimit::Backdrop));
    assert_eq!(paint_limit_from_code(b"limit"), None);
    assert_eq!(paint_limit_code(WidgetPaintLimit::Backdrop), "backdrop-limit");
    assert_eq!(
        paint_limit_message(WidgetPaintLimit::Paint, 17),
        "Paint limit exceeded: 17 boxes with shadows, filters or transparency (max 16)"
    );
    assert_eq!(paint_limit_message(WidgetPaintLimit::Backdrop, 3), "Paint limit exceeded: 3 boxes with backdrop-filter (max 2)");
}
