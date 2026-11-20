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
