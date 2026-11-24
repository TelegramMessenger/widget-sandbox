//! The engine in process on the real sandbox bundle (packages/desktop/WidgetJS), a few of
//! packages/desktop/tests/test_main.cpp's engine tests ported by name; the rest run through the C++ suite against
//! this engine over the stream (`TW_ENGINE_STREAM=rust`). The places (`surface`) are also compared with the compiled
//! C++ (`oracle/sdk_policy_oracle`), and the engine's own chat type and resize anchor of a page, which the shim would
//! hide, are read at the native boundary.

use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use twidget_bundle_stamp::SandboxResources;
use twidget_engine::*;
use twidget_js::{Platform, now};
use twidget_json::Json;
use twidget_sdk_policy::{WidgetContext, WidgetSurface, surface_name};
use twidget_testkit::{Comparison, Fields, Oracle, framed};

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
            *b = (i * 13 + 1) as u8;
        }
    }
}

struct NoIntl;

impl IntlProvider for NoIntl {
    fn format(&self, _: &Json) -> Vec<u8> {
        Vec::new()
    }
}

#[derive(Default)]
struct Seen {
    ops: Vec<Vec<u8>>,
    logs: Vec<String>,
    diags: Vec<(String, String)>,
    loaded: bool,
    died: Option<String>,
    stats: usize,
    /// Each edge `on_resize_anchor` got.
    anchors: Vec<String>,
}

#[derive(Default)]
struct Recorder {
    seen: Mutex<Seen>,
    changed: Condvar,
}

impl Recorder {
    fn wait(&self, what: &str, done: impl Fn(&Seen) -> bool) {
        let deadline = Instant::now() + Duration::from_secs(20);
        let mut seen = self.seen.lock().unwrap();
        while !done(&seen) {
            let left = deadline.saturating_duration_since(Instant::now());
            assert!(!left.is_zero(), "timed out waiting for {what}: diags {:?}, logs {:?}, died {:?}", seen.diags, seen.logs, seen.died);
            seen = self.changed.wait_timeout(seen, left).unwrap().0;
        }
    }

    fn note(&self, f: impl FnOnce(&mut Seen)) {
        f(&mut self.seen.lock().unwrap());
        self.changed.notify_all();
    }
}

fn text(b: &[u8]) -> String {
    String::from_utf8_lossy(b).into_owned()
}

impl EngineDelegate for Recorder {
    fn on_ops(&self, _: &Engine, json: &[u8]) {
        self.note(|s| s.ops.push(json.to_vec()));
    }
    fn on_query(&self, _: &Engine, _: &[u8]) -> QueryAnswer {
        QueryAnswer { json: b"{\"rects\":[]}".to_vec(), ..QueryAnswer::default() }
    }
    fn on_listen(&self, _: &Engine, _: &[u8]) {}
    fn on_prevent_default(&self, _: &Engine, _: &[u8]) {}
    fn on_request_frame(&self, _: &Engine) {}
    fn on_log(&self, _: &Engine, level: &[u8], t: &[u8]) {
        self.note(|s| s.logs.push(format!("{}: {}", text(level), text(t))));
    }
    fn on_diag(&self, _: &Engine, kind: &[u8], detail: &[u8]) {
        self.note(|s| s.diags.push((text(kind), text(detail))));
    }
    fn on_resize_anchor(&self, _: &Engine, edge: &[u8]) {
        self.note(|s| s.anchors.push(text(edge)));
    }
    fn on_loaded(&self, _: &Engine) {
        self.note(|s| s.loaded = true);
    }
    fn on_stats(&self, _: &Engine, _: &SandboxStats) {
        self.note(|s| s.stats += 1);
    }
    fn on_died(&self, _: &Engine, reason: &[u8]) {
        self.note(|s| s.died = Some(text(reason)));
    }
}

fn resources() -> Arc<SandboxResources> {
    let dir = concat!(env!("CARGO_MANIFEST_DIR"), "/../../../desktop/WidgetJS");
    Arc::new(SandboxResources::from_directory(dir).expect("the desktop bundles (pnpm build:desktop)"))
}

/// What `recording_device_inputs` appends to the sandbox bundle.
const RECORD_DEVICE_INPUTS: &str = concat!(
    "\n(function (g) { var dispatch = g.__twDispatch, seen = [];",
    " Object.defineProperty(g, '__twDispatch', { configurable: true, value: function (kind, arg) {",
    " if (kind === 'boot') seen.push('boot:' + JSON.stringify(JSON.parse(arg).config.deviceSafeArea));",
    " if (kind === 'deviceSafeArea') seen.push('input:' + arg);",
    " return dispatch(kind, arg); } });",
    " Object.defineProperty(g, 'deviceInputs', { value: seen }); })(globalThis);\n"
);

/// test_profile_surface.cpp's `recordingDeviceInputs`: the shipped bundles, the sandbox's dispatcher wrapped so that
/// the realm keeps `deviceInputs`, the device's edges of the boot record (`boot:`) and each `deviceSafeArea` input
/// (`input:`), as the engine wrote them for the widget.
fn recording_device_inputs() -> Arc<SandboxResources> {
    sandbox_bundle_with(RECORD_DEVICE_INPUTS)
}

/// What `native_boundary` appends to the sandbox bundle.
const NATIVE_BOUNDARY: &str = concat!(
    "\n(function (g) { var host = g.__host, dispatch = g.__twDispatch, seen = [];",
    " Object.defineProperty(g, '__twDispatch', { configurable: true, value: function (kind, arg) {",
    " if (kind === 'boot') seen.push(JSON.stringify(JSON.parse(arg).config.chatType));",
    " return dispatch(kind, arg); } });",
    " Object.defineProperty(g, 'bootChatType', { value: seen });",
    " Object.defineProperty(g, 'nativeBridge', { value: function (call) { return host('bridge', call); } }); })(globalThis);\n"
);

/// The shipped bundles with the engine's own decisions in view, past the shim's: the realm keeps `bootChatType`, the
/// boot record's `chatType` as the engine wrote it, and widget code gets `nativeBridge(call)`, the host's bridge call
/// with `call` as it is, which the shim would have checked and kept first.
fn native_boundary() -> Arc<SandboxResources> {
    sandbox_bundle_with(NATIVE_BOUNDARY)
}

/// The shipped bundles with `script` after the sandbox bundle's last line, where it runs before the engine captures the
/// dispatcher and hides the host's globals.
fn sandbox_bundle_with(script: &str) -> Arc<SandboxResources> {
    let mut res = (*resources()).clone();
    res.sandbox_bundle.extend_from_slice(script.as_bytes());
    Arc::new(res)
}

fn engine(html: &str, config: SandboxConfig) -> (SandboxEngine, Arc<Recorder>) {
    engine_on(html, config, resources())
}

fn engine_on(html: &str, config: SandboxConfig, resources: Arc<SandboxResources>) -> (SandboxEngine, Arc<Recorder>) {
    let recorder = Arc::new(Recorder::default());
    let engine = SandboxEngine::new(html.as_bytes().to_vec(), config, resources, Arc::new(NoIntl), Arc::new(TestPlatform)).unwrap();
    engine.set_delegate(recorder.clone());
    engine.start();
    (engine, recorder)
}

#[cfg(feature = "oracle-stats-clock")]
#[test]
fn trailing_stats_follow_the_oracles_clock() {
    use std::sync::atomic::{AtomicU64, Ordering};

    struct Clock(AtomicU64);
    impl Platform for Clock {
        fn thread_cpu_seconds(&self) -> f64 {
            0.0
        }
        fn time_zone_offset(&self, _: i64) -> i32 {
            0
        }
        fn random(&self, bytes: &mut [u8]) {
            TestPlatform.random(bytes);
        }
        fn steady_seconds(&self) -> f64 {
            f64::from_bits(self.0.load(Ordering::Relaxed))
        }
    }

    let clock = Arc::new(Clock(AtomicU64::new(1000.0f64.to_bits())));
    let engine =
        SandboxEngine::new(b"<p>test</p>".to_vec(), SandboxConfig::default(), resources(), Arc::new(NoIntl), clock.clone()).unwrap();
    let recorder = Arc::new(Recorder::default());
    engine.set_delegate(recorder.clone());
    engine.start();
    let advance = |time: f64| {
        clock.0.store(time.to_bits(), Ordering::Relaxed);
        assert_eq!(engine.probe(b"__twidget_oracle_advance_stats__"), b"1");
        recorder.seen.lock().unwrap().stats
    };
    let initial = advance(1000.0);
    engine.charge_validator(1.0);
    engine.charge_validator(1.0);
    assert_eq!(advance(1000.0), initial);
    assert_eq!(advance(1000.199), initial);
    assert_eq!(advance(1000.2), initial + 1);
    assert_eq!(advance(1000.2), initial + 1);
    engine.charge_validator(1.0);
    engine.stop(b"done");
    assert_eq!(advance(1000.2), initial + 1);
    assert_eq!(advance(1001.0), initial + 1);
}

#[test]
fn boots_and_emits_initial_tree() {
    let (engine, r) = engine("<div id=\"x\">hello</div>", SandboxConfig::default());
    r.wait("loaded", |s| s.loaded || s.died.is_some());
    let seen = r.seen.lock().unwrap();
    assert!(seen.died.is_none(), "{:?}", seen.died);
    let all: String = seen.ops.iter().map(|o| text(o)).collect();
    assert!(all.contains("hello"), "{all}");
    assert!(seen.diags.iter().any(|(k, d)| k == "boot" && d.contains("sandbox bundle eval")), "{:?}", seen.diags);
    drop(seen);
    assert_eq!(engine.probe(b"document.getElementById('x').textContent"), b"hello");
}

#[test]
fn cpu_limit_kills_infinite_loop() {
    let (_engine, r) = engine("<script>for(;;){}</script>", SandboxConfig { cpu_limit_ms: 100.0, ..SandboxConfig::default() });
    r.wait("death", |s| s.died.is_some());
    let died = r.seen.lock().unwrap().died.clone().unwrap();
    assert!(died.contains("CPU limit exceeded"), "{died}");
}

#[test]
fn memory_limit_kills_allocation_flood() {
    let (_engine, r) = engine("<script>var a=[];for(;;){a.push(new Array(100000).fill(1));}</script>", SandboxConfig::default());
    r.wait("death", |s| s.died.is_some());
    let died = r.seen.lock().unwrap().died.clone().unwrap();
    assert!(died.contains("Memory limit exceeded") || died.contains("CPU limit"), "{died}");
}

#[test]
fn widget_code_cannot_reach_host_globals() {
    let (engine, r) = engine("<div>x</div>", SandboxConfig::default());
    r.wait("loaded", |s| s.loaded || s.died.is_some());
    assert_eq!(engine.probe(b"typeof __host + typeof __hostBinary + typeof __twDispatch"), b"undefinedundefinedundefined");
}

/// Probes `code` until it gives something other than `empty`, for at most 20 s; what it gave last.
fn probe_until(engine: &SandboxEngine, code: &str, empty: &str) -> String {
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        let got = text(&engine.probe(code.as_bytes()));
        if got != empty || Instant::now() > deadline {
            return got;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
}

/// The bot's profile page (docs/sdk-v1.md §2.19), the engine's half of test_main.cpp's test and of the realm's readings
/// in test_profile_surface.cpp's: the place and both layers reach the widget from its first line (no corners and no
/// chat type on a page), two equal device updates fire one safeAreaChanged, the geometry of each place, and the
/// widget's resize anchor goes nowhere on a page (the shim keeps it there, and the engine drops one that reaches it all
/// the same). In a bubble the engine gives the widget's realm four zeros for the device's edges, at boot and on every
/// change (a page's realm gets them as passed), and the anchor reaches the delegate (the bubble's script adds the
/// page's call to the C++ one).
#[test]
fn sdk_profile_surface_layers_height_and_anchor() {
    use WidgetSurface::{Message, Profile};
    // The C++ test's checks.
    assert!(surface::height(Message, 9000.0, 844.0) == 600.0 && surface::height(Message, 10.0, 844.0) == 50.0);
    assert!(surface::height(Profile, 120.0, 844.0) == 844.0 && surface::height(Profile, 9000.0, 844.0) == surface::PAGE_MAX_HEIGHT);
    assert!(surface::check_viewport_height(Profile, 844.123) == 844.12 && surface::check_viewport_height(Profile, 4001.0) == 0.0);
    assert!(surface::check_viewport_height(Message, 844.0) == 0.0 && surface::PAGE_MAX_HEIGHT == 4000.0);
    // The edges of the merged rule: the viewport's range is checked before it is rounded, a content height that is not
    // finite counts as 0, a page without a usable viewport is at least 50.
    for (s, h, want) in [
        (Profile, 0.5, 0.5f64),
        (Profile, 0.0, 0.0),
        (Profile, -1.0, 0.0),
        (Profile, 4000.0, 4000.0),
        (Profile, 4000.004, 0.0),
        (Profile, 0.004, 0.0),
        (Profile, f64::NAN, 0.0),
        (Profile, f64::INFINITY, 0.0),
        (Profile, 844.123, 844.12),
        (Message, 844.0, 0.0),
    ] {
        assert_eq!(surface::check_viewport_height(s, h).to_bits(), want.to_bits(), "check_viewport_height({s:?}, {h})");
    }
    for (s, content, viewport, want) in [
        (Message, f64::NAN, 0.0, 50.0),
        (Profile, f64::NAN, 844.0, 844.0),
        (Profile, 120.0, 0.0, 120.0),
        (Profile, 10.0, 0.0, 50.0),
        (Profile, 120.0, 844.0, 844.0),
        (Profile, 9000.0, 844.0, 4000.0),
        (Message, 9000.0, 844.0, 600.0),
        (Message, 10.0, 844.0, 50.0),
    ] {
        assert_eq!(surface::height(s, content, viewport), want, "height({s:?}, {content}, {viewport})");
    }
    assert!(surface::keeps_resize_anchor(Message) && !surface::keeps_resize_anchor(Profile));

    let mut page_config = SandboxConfig::default();
    // A page has no chat: null for the widget.
    page_config.context = WidgetContext { surface: Profile, chat_type: b"private".to_vec(), ..WidgetContext::default() };
    // Past the old ranges: as passed on a page; negative: 0 for the widget.
    page_config.safe_area = WidgetSafeArea { top: 120.0, bottom: 34.0, left: -5.0, ..WidgetSafeArea::default() };
    // No corners on a page: 0 for the widget.
    page_config.content_safe_area =
        WidgetContentSafeArea { top: 90.0, bottom: 150.0, left: 40.0, corner_right: 64.0, ..WidgetContentSafeArea::default() };
    let (page, r) = engine_on(
        concat!(
            "<p>x</p><script>var log = [], d = TelegramWidget.safeAreaInset, c = TelegramWidget.contentSafeAreaInset;",
            "var place = [TelegramWidget.surface, d.top, d.bottom, d.left, c.top, c.bottom, c.left, c.cornerRight, String(TelegramWidget.chatType)].join(':');",
            "TelegramWidget.onEvent('safeAreaChanged', function () { var a = TelegramWidget.safeAreaInset; log.push([a.top, a.left, a.right, a.bottom, Object.isFrozen(a), a !== d].join(':')); });",
            "TelegramWidget.setResizeAnchor('bottom');</script>"
        ),
        page_config,
        recording_device_inputs(),
    );
    r.wait("the page loaded", |s| s.loaded || s.died.is_some());
    assert_eq!(text(&page.probe(b"place")), "profile:120:34:0:90:150:40:0:null");
    // A page's realm gets the edges as passed, at boot and on every change (the shim checks them).
    let edges_given = r#"{"top":120,"right":0,"bottom":34,"left":-5}"#;
    assert_eq!(text(&page.probe(b"deviceInputs.join(' ')")), format!("boot:{edges_given}"));
    {
        let seen = r.seen.lock().unwrap();
        assert!(seen.died.is_none(), "{:?}", seen.died);
        assert!(seen.anchors.is_empty(), "a page's anchor reached the delegate: {:?}", seen.anchors);
        assert!(!seen.diags.iter().any(|(k, _)| k == "bridge-rejected"), "{:?}", seen.diags);
    }
    let turned = WidgetSafeArea { left: 59.0, right: 59.0, bottom: 21.0, ..WidgetSafeArea::default() };
    page.safe_area(turned);
    page.safe_area(turned);
    assert_eq!(probe_until(&page, "log.join('|')", ""), "0:59:59:21:true:true");
    let turned_given = r#"{"top":0,"right":59,"bottom":21,"left":59}"#;
    assert_eq!(
        probe_until(&page, "deviceInputs.length > 2 ? deviceInputs.join(' ') : ''", ""),
        format!("boot:{edges_given} input:{turned_given} input:{turned_given}")
    );
    assert!(r.seen.lock().unwrap().anchors.is_empty());
    drop(page);

    let bubble_config = SandboxConfig { safe_area: WidgetSafeArea { top: 47.0, ..WidgetSafeArea::default() }, ..SandboxConfig::default() };
    let (bubble, r) = engine_on(
        "<script>var s = TelegramWidget.surface + ':' + JSON.stringify(TelegramWidget.safeAreaInset);TelegramWidget.setResizeAnchor('bottom');</script>",
        bubble_config,
        recording_device_inputs(),
    );
    r.wait("the bubble's anchor", |s| !s.anchors.is_empty() || s.died.is_some());
    assert_eq!(r.seen.lock().unwrap().anchors, ["bottom"]);
    let none = r#"{"top":0,"right":0,"bottom":0,"left":0}"#;
    assert_eq!(text(&bubble.probe(b"s")), format!("message:{none}"));
    // The edges never reach a bubble's realm, at boot or later.
    bubble.safe_area(turned);
    assert_eq!(probe_until(&bubble, "deviceInputs.length > 1 ? deviceInputs.join(' ') : ''", ""), format!("boot:{none} input:{none}"));
}

/// The engine's own half of the place's chat type and resize anchor, which the shim would hide (it nulls a page's
/// chat type and keeps a page's anchor from the host): with a private chat supplied outside a copy and a preview, the
/// boot record has no chat type on a page and the supplied one in a bubble, and a checked bottom anchor that reaches
/// the native bridge goes nowhere on a page and to the delegate once in a bubble.
#[test]
fn sdk_profile_surface_chat_type_and_anchor_at_the_native_boundary() {
    for (surface, chat_type, anchors) in
        [(WidgetSurface::Profile, "null", &[][..]), (WidgetSurface::Message, "\"private\"", &["bottom"][..])]
    {
        let context = WidgetContext { surface, chat_type: b"private".to_vec(), ..WidgetContext::default() };
        let (engine, r) = engine_on(
            "<p>x</p><script>var sent = 'sent:' + nativeBridge(JSON.stringify(['setResizeAnchor', ['bottom']]));</script>",
            SandboxConfig { context, ..SandboxConfig::default() },
            native_boundary(),
        );
        r.wait("the widget loaded", |s| s.loaded || s.died.is_some());
        assert_eq!(text(&engine.probe(b"bootChatType.join(' ')")), chat_type, "{surface:?}'s boot record");
        // The call came back from the native bridge, which took it as a valid one (no bridge-rejected below).
        assert_eq!(text(&engine.probe(b"sent")), "sent:undefined", "{surface:?}");
        let seen = r.seen.lock().unwrap();
        assert!(seen.died.is_none(), "{surface:?}: {:?}", seen.died);
        assert!(!seen.diags.iter().any(|(k, _)| k == "bridge-rejected"), "{surface:?}: {:?}", seen.diags);
        assert_eq!(seen.anchors, anchors, "{surface:?}'s anchors at the delegate");
    }
}

/// The places against the compiled C++ (`oracle/sdk_policy_oracle`, record 'P'): each place's wire name, whether its
/// resize anchor moves anything and the three height limits of `sandbox_engine.h`. Skipped without
/// `TWIDGET_ORACLE_DIR`; `tools/gate.py` sets it.
#[test]
fn surfaces_alike() {
    let Some(oracle) = Oracle::find("sdk_policy_oracle") else { return };
    let mut c = Comparison::new();
    for (byte, place) in [(0u8, WidgetSurface::Message), (1, WidgetSurface::Profile)] {
        let mut f = Fields::new();
        f.str(surface_name(place).as_bytes()).flag(surface::keeps_resize_anchor(place));
        f.f64(surface::MIN_HEIGHT).f64(surface::MESSAGE_MAX_HEIGHT).f64(surface::PAGE_MAX_HEIGHT);
        c.case(&framed(b'P', &[byte]), (1, f.take()), format!("the place {place:?}"));
    }
    assert_eq!(c.check(&oracle), [0, 2, 0]);
}

type Diags = Arc<Mutex<Vec<(String, String)>>>;

fn validator(theme: &str) -> (TrustedValidator, Diags) {
    let diags: Diags = Arc::default();
    let sink = Arc::clone(&diags);
    let v = TrustedValidator::new(
        theme.as_bytes(),
        resources(),
        Some(Arc::new(move |k: &[u8], d: &[u8]| sink.lock().unwrap().push((text(k), text(d))))),
        Arc::new(TestPlatform),
    )
    .unwrap();
    (v, diags)
}

fn saw(diags: &Diags, kind: &str) -> bool {
    diags.lock().unwrap().iter().any(|(k, _)| k == kind)
}

#[test]
fn validator_diag_bridge() {
    let (v, diags) = validator("light");
    // An unknown element is reported through __diag (tag-replaced).
    v.process_ops(br#"[{"op":"el","id":2,"tag":"x-foo"}]"#);
    assert!(saw(&diags, "tag-replaced"), "{:?}", diags.lock().unwrap());
}

#[test]
fn validator_validates_anim_records() {
    let (v, diags) = validator("light");
    v.process_ops(br#"[{"op":"el","id":2,"tag":"html"},{"op":"el","id":3,"tag":"body"},{"op":"ins","parent":2,"id":3,"before":null},{"op":"el","id":7,"tag":"div"},{"op":"ins","parent":3,"id":7,"before":null}]"#);
    let record = |values: &str, duration: &str, easing: &str| {
        format!(
            r#"[{{"aid":1,"target":7,"keyframes":[{{"offset":0,"easing":"linear","composite":null,"values":{values}}}],"timing":{{"duration":{duration},"delay":0,"endDelay":0,"iterations":null,"iterationStart":0,"direction":"normal","fill":"none","easing":"{easing}"}},"composite":"replace","iterationComposite":"replace","startTime":1000,"holdTime":null,"playbackRate":1}}]"#
        )
    };
    for bad in [
        record("{}", "1e999", "linear"),
        record("{}", "1e12", "linear"),
        record("{}", "1000", "evil(1)"),
        r#"{"not":"a list"}"#.to_string(),
        r#"[{"aid":-1}]"#.to_string(),
    ] {
        diags.lock().unwrap().clear();
        assert_eq!(v.validate_anim(bad.as_bytes()), b"[]", "{bad}");
        assert!(saw(&diags, "protocol-violation"), "{bad}");
    }
    let clean = text(&v.validate_anim(
        record(
            r#"{"opacity":"0.5","background-image":"url(https://evil.example/x)","width":"expression(alert(1))","behavior":"url(x.htc)","animation-name":"x"}"#,
            "1000",
            "ease-in",
        )
        .as_bytes(),
    ));
    assert!(clean.contains(r#""opacity":"0.5""#), "{clean}");
    assert!(!clean.contains("url(") && !clean.contains("expression") && !clean.contains("animation-name"), "{clean}");
}

#[test]
fn validator_strips_external_urls_and_handlers() {
    let (v, diags) = validator("light");
    let ops = concat!(
        r#"[{"op":"el","id":2,"tag":"html"},{"op":"ins","parent":1,"id":2,"before":null},"#,
        r#"{"op":"el","id":3,"tag":"body"},{"op":"ins","parent":2,"id":3,"before":null},"#,
        r#"{"op":"el","id":4,"tag":"img","attrs":{"src":"http://127.0.0.1:9999/hit/x","onerror":"fetch('http://127.0.0.1:9999/e')"}},{"op":"ins","parent":3,"id":4,"before":null},"#,
        r#"{"op":"el","id":5,"tag":"iframe","attrs":{"src":"http://127.0.0.1:9999/hit/f"}},{"op":"ins","parent":3,"id":5,"before":null},"#,
        r#"{"op":"el","id":6,"tag":"style"},{"op":"ins","parent":3,"id":6,"before":null},"#,
        r#"{"op":"text","id":7,"data":"@import url(http://127.0.0.1:9999/i); p{color:red;background:url(http://127.0.0.1:9999/b)}"},{"op":"ins","parent":3,"id":7,"before":null}]"#
    );
    let out = v.process_ops(ops.as_bytes());
    let rendered = Json::parse(&out).unwrap();
    assert!(rendered.is_array());
    // Nothing that is attached, set as an attribute or emitted as CSS may carry the canary.
    let mut attached = String::new();
    for op in rendered.arr() {
        if [&b"ins"[..], b"attr", b"css", b"style", b"el"].contains(&op.get("op").as_str()) {
            attached += &text(&op.dump());
        }
    }
    assert!(!attached.contains("127.0.0.1"), "{attached}");
    assert!(!attached.to_lowercase().contains("onerror"), "{attached}");
    assert!(!attached.contains(r#""tag":"iframe""#), "{attached}");
    assert!(text(&out).contains("data-tg-placeholder"), "{}", text(&out));
    assert!(saw(&diags, "url-blocked"));
    assert_eq!(v.validate_query(br#"{"q":"rect","id":4}"#), Some(br#"{"q":"rect","id":4}"#.to_vec()));
    assert_eq!(v.validate_query(br#"{"q":"eval","code":"1"}"#), None);
    assert_eq!(v.rewrite_media_query(b"(prefers-color-scheme: dark)"), b"(min-width: 99999px)");
}

/// Churn validators, each with its own thread, and let them tear down.
#[test]
fn validator_repeated_create_and_teardown() {
    for i in 0..25 {
        let (v, _) = validator(if i % 2 == 0 { "light" } else { "dark" });
        assert!(
            text(&v.process_ops(br#"[{"op":"el","id":2,"tag":"p"},{"op":"ins","parent":1,"id":2,"before":null}]"#))
                .contains(r#""tag":"p""#)
        );
        assert_eq!(v.node_count(), 2);
    }
}

#[test]
fn validator_stylesheet_text_is_only_shipped_sanitized() {
    let (v, diags) = validator("dark");
    let ops = concat!(
        r#"[{"op":"el","id":2,"tag":"html"},{"op":"ins","parent":1,"id":2,"before":null},"#,
        r#"{"op":"el","id":6,"tag":"style"},{"op":"ins","parent":2,"id":6,"before":null},"#,
        r#"{"op":"text","id":7,"data":"@import url(http://127.0.0.1:9999/i); @media (prefers-color-scheme: dark){p{color:red}} p{background:url(http://127.0.0.1:9999/b)}"},{"op":"ins","parent":6,"id":7,"before":null}]"#
    );
    let out = text(&v.process_ops(ops.as_bytes()));
    assert!(out.contains(r#"{"op":"rm","id":7}"#), "{out}");
    assert!(out.contains("@media (min-width: 0px){p{color:red}}"), "{out}");
    assert!(saw(&diags, "url-blocked") || saw(&diags, "css-dropped"));
    assert!(!out.contains(r#""css","id":6,"text":"@import"#), "{out}");
    assert_eq!(v.node_count(), 4);
}

/// The validator's time on the widget's messages reaches its CPU sink, the widget's quota.
#[test]
fn validator_measures_its_calls_for_the_cpu_sink() {
    let (v, _) = validator("light");
    let charged = Arc::new(Mutex::new(Vec::new()));
    let sink = Arc::clone(&charged);
    v.set_cpu_sink(Some(Box::new(move |ms| sink.lock().unwrap().push(ms))));
    v.process_ops(br#"[{"op":"el","id":2,"tag":"p"}]"#);
    v.validate_query(br#"{"q":"rect","id":2}"#);
    v.set_theme(b"dark");
    assert_eq!(charged.lock().unwrap().len(), 2, "processOps and validateQuery are metered, setTheme is not");
    assert!(charged.lock().unwrap().iter().all(|&ms| ms >= 0.0));
}
