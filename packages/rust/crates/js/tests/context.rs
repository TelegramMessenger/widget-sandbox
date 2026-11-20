//! packages/desktop/tests/test_main.cpp's tests of the JS thread and the Nova context, ported by name: deep
//! recursion on the pinned thread, timers and their sweep, what crosses into host functions and back, the
//! failures that end a script, and bundles compiled once per process.

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, mpsc};
use std::time::Duration;

use twidget_js::*;

/// A platform for tests: the thread's CPU time is the wall clock's (the helper's embedder gives the real one).
struct TestPlatform;

impl Platform for TestPlatform {
    fn thread_cpu_seconds(&self) -> f64 {
        now()
    }
    fn time_zone_offset(&self, _: i64) -> i32 {
        4 * 3600
    }
    fn random(&self, bytes: &mut [u8]) {
        for (i, b) in bytes.iter_mut().enumerate() {
            *b = (i * 7 + 3) as u8;
        }
    }
}

fn platform() -> Arc<dyn Platform> {
    Arc::new(TestPlatform)
}

const DEPTH_PROBE: &[u8] =
    b"(function(){ var n = 0; function r(){ n++; r(); } try { r(); } catch (e) { if (!(e instanceof RangeError)) throw e; } return n; })()";

#[test]
fn pinned_thread_gives_deep_recursion() {
    let t = JsThread::new("test.js", || ()).unwrap();
    let depth = t
        .sync(|_| {
            let mut js = NovaContext::new(16 << 20, JS_STACK_LIMIT, platform()).unwrap();
            js.evaluate(DEPTH_PROBE, false).unwrap().number.unwrap_or(-1.0)
        })
        .unwrap();
    eprintln!("recursion depth on the JS thread: {depth}");
    assert!(depth > 300.0, "{depth}");
    let (tx, rx) = mpsc::channel();
    let cancelled_ran = Arc::new(AtomicUsize::new(0));
    let ran = Arc::clone(&cancelled_ran);
    let c = t.after(0.01, move |_| {
        ran.fetch_add(1, Ordering::SeqCst);
    });
    c.cancel();
    t.after(0.03, move |_| tx.send(std::thread::current().name().map(str::to_string)).unwrap());
    assert_eq!(rx.recv_timeout(Duration::from_secs(2)).unwrap().as_deref(), Some("test.js"), "a timer fires on the thread");
    assert_eq!(cancelled_ran.load(Ordering::SeqCst), 0, "a cancelled timer never runs");
    let live = t.after(86400.0, |_| {});
    for _ in 0..10000 {
        t.after(86400.0, |_| {}).cancel();
    }
    assert!(t.queued_timers() <= 128, "{} timers queued", t.queued_timers());
    live.cancel();
    t.stop();
}

#[test]
fn timers_keep_their_order_and_tasks_run_in_order() {
    let t = JsThread::new("test.order", || std::cell::RefCell::new(Vec::new())).unwrap();
    for i in 0..5 {
        t.after(0.02, move |s: &std::cell::RefCell<Vec<i32>>| s.borrow_mut().push(i));
    }
    t.after(0.01, |s: &std::cell::RefCell<Vec<i32>>| s.borrow_mut().push(-1));
    for i in 10..13 {
        t.post_task(move |s: &std::cell::RefCell<Vec<i32>>| s.borrow_mut().push(i));
    }
    std::thread::sleep(Duration::from_millis(80));
    let seen = t.sync(|s| s.borrow().clone()).unwrap();
    assert_eq!(seen, vec![10, 11, 12, -1, 0, 1, 2, 3, 4], "tasks first, then timers by deadline, equal ones in the order they were set");
    t.join();
    assert!(t.sync(|_| ()).is_none(), "nothing runs after the thread stopped");
}

#[test]
fn a_string_past_the_memory_limit_never_reaches_a_host_function() {
    let t = JsThread::new("test.js", || ()).unwrap();
    let (calls, error) = t
        .sync(|_| {
            let calls = std::rc::Rc::new(std::cell::Cell::new(0));
            let mut js = NovaContext::new(8 << 20, JS_STACK_LIMIT, platform()).unwrap();
            let c = std::rc::Rc::clone(&calls);
            js.register_function(
                "__probe",
                Box::new(move |_| {
                    c.set(c.get() + 1);
                    Ok(HostReturn::Undefined)
                }),
            )
            .unwrap();
            let r = js.evaluate(
                b"__probe('fits'); var s; try { s = '\\u00e9'.repeat(5 << 20); } catch (e) {} __probe(s); __probe('after');",
                false,
            );
            (calls.get(), r.err().map(|e| e.text).unwrap_or_else(|| "no error".into()))
        })
        .unwrap();
    assert_eq!(calls, 1, "{calls} calls");
    assert!(error.contains("out of memory"), "{error}");
}

#[test]
fn an_exception_from_a_host_function_stops_the_script() {
    let t = JsThread::new("test.js", || ()).unwrap();
    let after = Arc::new(AtomicUsize::new(0));
    let outcome = |which: &'static str| {
        let after = Arc::clone(&after);
        t.sync(move |_| {
            let mut js = NovaContext::new(8 << 20, JS_STACK_LIMIT, platform()).unwrap();
            js.register_function(
                "__throws",
                Box::new(|args| match args[0].as_slice() {
                    b"runtime" => Err(HostFailure::Other("broken glue".into())),
                    b"alloc" => Err(HostFailure::Memory),
                    _ => Ok(HostReturn::String(vec![b'x'; 64 << 20])),
                }),
            )
            .unwrap();
            js.register_function(
                "__after",
                Box::new(move |_| {
                    after.fetch_add(1, Ordering::SeqCst);
                    Ok(HostReturn::Undefined)
                }),
            )
            .unwrap();
            let code = format!("try {{ __throws('{which}'); }} catch (e) {{ __after(); }} __after();");
            match js.evaluate(code.as_bytes(), false) {
                Err(e) => format!("{}{}", e.text, if e.uncatchable { " (uncatchable)" } else { "" }),
                Ok(_) => "no error".into(),
            }
        })
        .unwrap()
    };
    assert_eq!(outcome("runtime"), "InternalError: host function failed: broken glue (uncatchable)");
    assert_eq!(outcome("alloc"), "InternalError: out of memory (uncatchable)");
    assert_eq!(outcome("a result past the limit"), "InternalError: out of memory (uncatchable)");
    assert_eq!(after.load(Ordering::SeqCst), 0, "no call after a failed one");
}

#[test]
fn a_result_that_cannot_be_copied_is_an_error() {
    let t = JsThread::new("test.js", || ()).unwrap();
    for by_call in [false, true] {
        let text = t
            .sync(move |_| {
                let mut js = NovaContext::new(8 << 20, JS_STACK_LIMIT, platform()).unwrap();
                js.evaluate(b"function big() { return '\\u00e9'.repeat(5 << 20); }", false).unwrap();
                let r = if by_call { js.call("big", &[]) } else { js.evaluate(b"big()", false) };
                match r {
                    Ok(r) => format!("a result of {} bytes", r.string.unwrap_or_default().len()),
                    Err(e) => e.text,
                }
            })
            .unwrap();
        assert!(text.contains("out of memory"), "{text}");
    }
}

#[test]
fn bundle_is_compiled_once_and_a_broken_one_throws_its_syntax_error() {
    let t = JsThread::new("test.js", || ()).unwrap();
    t.sync(|_| {
        let source = b"globalThis.answer = (globalThis.answer || 0) + 6 * 7;";
        let mut first = NovaContext::new(16 << 20, JS_STACK_LIMIT, platform()).unwrap();
        assert!(first.evaluate_bundle(source, "test-shared-bundle.js").unwrap());
        assert_eq!(first.evaluate(b"answer", false).unwrap().number, Some(42.0));
        let mut second = NovaContext::new(16 << 20, JS_STACK_LIMIT, platform()).unwrap();
        assert!(second.evaluate_bundle(source, "test-shared-bundle.js").unwrap());
        assert_eq!(second.evaluate(b"answer", false).unwrap().number, Some(42.0));
        assert!(first.evaluate_bundle(source, "test-shared-bundle.js").unwrap());
        assert_eq!(first.evaluate(b"answer", false).unwrap().number, Some(84.0));
        assert_eq!(second.evaluate(b"answer", false).unwrap().number, Some(42.0));
        assert!(second.evaluate_bundle(b"globalThis.answer = 6 * 8;", "test-shared-bundle.js").unwrap());
        assert_eq!(second.evaluate(b"answer", false).unwrap().number, Some(48.0), "another source under the same name runs as itself");
        for _ in 0..2 {
            let e = second.evaluate_bundle(b"globalThis.answer = 6 * ;", "test-broken-bundle.js").unwrap_err();
            assert!(e.text.starts_with("SyntaxError"), "{}", e.text);
        }
        assert_eq!(second.evaluate(b"answer", false).unwrap().number, Some(48.0));
    })
    .unwrap();
}

#[test]
fn arguments_cross_as_the_c_interface_gave_them() {
    let t = JsThread::new("test.js", || ()).unwrap();
    let seen = t
        .sync(|_| {
            let seen = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
            let mut js = NovaContext::new(8 << 20, JS_STACK_LIMIT, platform()).unwrap();
            let s = std::rc::Rc::clone(&seen);
            js.register_function(
                "__args",
                Box::new(move |args| {
                    s.borrow_mut().push(args.to_vec());
                    Ok(HostReturn::String(b"\xED\xA0\x80back".to_vec()))
                }),
            )
            .unwrap();
            let r = js
                .evaluate(b"__args('a\\u0000b', 3, -0, 0.1, 1e21, NaN, -Infinity, {}, 'ninth'); __args(2147483648.5, '\\ud800')", false)
                .unwrap();
            assert_eq!(r.string, Some(b"\xED\xA0\x80back".to_vec()), "a lone surrogate goes in and comes back as it was");
            let d = js.evaluate(b"new Date(0).getTimezoneOffset()", false).unwrap();
            assert_eq!(d.number, Some(-240.0), "the platform's time zone");
            seen.take()
        })
        .unwrap();
    let texts: Vec<Vec<u8>> = seen[0].clone();
    assert_eq!(texts[0], b"a", "a string ends at its first NUL, as strnlen cut it");
    assert_eq!(texts[1], b"3");
    assert_eq!(texts[2], b"0", "-0 is 0 by %lld");
    assert_eq!(texts[3], b"0.10000000000000001", "%.17g");
    assert_eq!(texts[4], b"1e+21");
    assert_eq!(texts[5], b"nan");
    assert_eq!(texts[6], b"-inf");
    assert_eq!(texts[7], b"", "anything else is empty");
    assert_eq!(texts.len(), 8, "at most eight arguments arrive");
    assert_eq!(seen[1][0], b"2147483648.5");
    assert_eq!(seen[1][1], b"\xED\xA0\x80", "a lone surrogate as WTF-8");
}

#[test]
fn deadlines_end_a_script_uncatchably() {
    let t = JsThread::new("test.js", || ()).unwrap();
    let (text, interrupted) = t
        .sync(|_| {
            let mut js = NovaContext::new(8 << 20, JS_STACK_LIMIT, platform()).unwrap();
            let d = js.deadlines();
            d.cpu_deadline.set(now() + 0.05);
            let e = js.evaluate(b"try { for (;;) {} } finally { for (;;) {} }", false).unwrap_err();
            (format!("{}{}", e.text, if e.uncatchable { " (uncatchable)" } else { "" }), d.interrupted.get())
        })
        .unwrap();
    assert_eq!(text, "InternalError: interrupted (uncatchable)");
    assert!(interrupted);
    assert_eq!(repaired_utf8(b"a\xffb\xed\xa0\x80"), "a\u{FFFD}b\u{FFFD}\u{FFFD}\u{FFFD}");
}
