//! The engine in process on the real sandbox bundle (packages/desktop/WidgetJS), a few of
//! packages/desktop/tests/test_main.cpp's engine tests ported by name; the rest run through the C++ suite against
//! this engine over the stream (`TW_ENGINE_STREAM=rust`).

use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use twidget_bundle_stamp::SandboxResources;
use twidget_engine::*;
use twidget_js::{Platform, now};
use twidget_json::Json;

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

fn engine(html: &str, config: SandboxConfig) -> (SandboxEngine, Arc<Recorder>) {
    let recorder = Arc::new(Recorder::default());
    let engine = SandboxEngine::new(html.as_bytes().to_vec(), config, resources(), Arc::new(NoIntl), Arc::new(TestPlatform)).unwrap();
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
