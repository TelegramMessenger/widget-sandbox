import XCTest
import QuartzCore
@testable import WidgetSandbox

/// One task's whole run, its pending promise jobs and what an idle task costs a remote client: a task of many inline
/// scripts stops within the task's limit (the service watchdog ends the whole process at 8 s), jobs a task leaves
/// pending go on in a follow-up task charged to the quota, and a task that says nothing sends a client no callbacks.
final class EngineTaskLimitTests: QuickJSTestCase {
    private final class Recorder: SandboxEngineDelegate {
        private let lock = NSLock()
        private var _died: String?
        private var _logs: [String] = []
        private var _callbacks = 0
        private var _depth = 0
        private var _started = 0
        private var _outsideTask = 0
        private var _loaded = false
        private var _throttled = 0

        var died: String? { lock.lock(); defer { lock.unlock() }; return _died }
        var logs: [String] { lock.lock(); defer { lock.unlock() }; return _logs }
        var callbacks: Int { lock.lock(); defer { lock.unlock() }; return _callbacks }
        var started: Int { lock.lock(); defer { lock.unlock() }; return _started }
        var depth: Int { lock.lock(); defer { lock.unlock() }; return _depth }
        var outsideTask: Int { lock.lock(); defer { lock.unlock() }; return _outsideTask }
        var loaded: Bool { lock.lock(); defer { lock.unlock() }; return _loaded }
        var throttled: Int { lock.lock(); defer { lock.unlock() }; return _throttled }

        private func note(_ body: () -> Void = {}) {
            lock.lock()
            _callbacks += 1
            body()
            lock.unlock()
        }

        func engineTaskStarted(_ engine: SandboxEngine) { note { _depth += 1; _started += 1 } }
        func engineTaskCompleted(_ engine: SandboxEngine) { note { _depth -= 1 } }
        func engine(_ engine: SandboxEngine, ops json: String) { note { if _depth == 0 { _outsideTask += 1 } } }
        func engine(_ engine: SandboxEngine, query json: String) -> (json: String, binary: Data?) { note(); return ("{}", nil) }
        func engine(_ engine: SandboxEngine, listen json: String) { note() }
        func engine(_ engine: SandboxEngine, preventDefault json: String) { note() }
        func engineRequestsFrame(_ engine: SandboxEngine) { note() }
        func engine(_ engine: SandboxEngine, log level: String, text: String) { note { _logs.append(text) } }
        func engine(_ engine: SandboxEngine, diag kind: String, detail: String) { note() }
        func engine(_ engine: SandboxEngine, bridge method: String, argsJson: String) { note() }
        func engineLoaded(_ engine: SandboxEngine) { note { _loaded = true } }
        func engine(_ engine: SandboxEngine, stats: SandboxStats) { note { _throttled = max(_throttled, stats.throttledTasks) } }
        func engine(_ engine: SandboxEngine, died reason: String) { note { _died = reason } }
    }

    private var backends: [QuickJSBackend] {
        QuickJSBackend.isNovaAvailable ? [.native, .nova] : [.native]
    }

    private func start(_ html: String, backend: QuickJSBackend, remote: Bool = false) -> (SandboxEngine, Recorder) {
        let engine = remote
            ? SandboxEngine(remote: SandboxLoopbackChannel(features: [], backend: backend), features: [], backend: backend)
            : SandboxEngine(resources: .shared, features: [], backend: backend)
        let recorder = Recorder()
        engine.delegate = recorder
        engine.start(html: html, config: SandboxConfig(innerWidth: 300, innerHeight: 200, devicePixelRatio: 2, theme: "light"))
        return (engine, recorder)
    }

    private func wait(_ seconds: TimeInterval, until done: () -> Bool) {
        let deadline = Date().addingTimeInterval(seconds)
        while !done() && Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.01)) }
    }

    private func busyScripts(_ count: Int, ms: Int) -> String {
        "<div>x</div>" + String(repeating: "<script>var t0=Date.now();while(Date.now()-t0<\(ms)){}</script>", count: count)
    }

    func testInlineScriptsShareOneLimitPerTask() {
        for backend in backends {
            let t0 = CACurrentMediaTime()
            let (engine, recorder) = start(busyScripts(10, ms: 300), backend: backend)
            wait(10) { recorder.died != nil || recorder.loaded }
            let elapsed = CACurrentMediaTime() - t0
            engine.terminate()
            XCTAssertTrue(recorder.died?.contains("ms of scripts in one task") ?? false, "\(backend): \(recorder.died ?? "loaded")")
            XCTAssertLessThan(elapsed, 3.5, "\(backend): ten 300 ms scripts held the engine for \(elapsed) s")
        }
    }

    func testInlineScriptsWithinTheTaskLimitAllRun() {
        for backend in backends {
            let (engine, recorder) = start(busyScripts(4, ms: 250) + "<script>console.log('all ran')</script>", backend: backend)
            wait(10) { recorder.died != nil || recorder.loaded }
            engine.terminate()
            XCTAssertNil(recorder.died, "\(backend)")
            XCTAssertTrue(recorder.logs.contains { $0.contains("all ran") }, "\(backend): \(recorder.logs)")
        }
    }

    func testPromiseJobsLeftByATaskGoOnWhileTheWidgetIsIdle() {
        for backend in backends {
            let (engine, recorder) = start("<div>x</div><script>(async function(){for(var i=0;i<8000;i++){await null;} console.log('awaited 8000');})()</script>", backend: backend)
            wait(5) { recorder.died != nil || recorder.logs.contains { $0.contains("awaited 8000") } }
            engine.terminate()
            XCTAssertNil(recorder.died, "\(backend)")
            XCTAssertTrue(recorder.logs.contains { $0.contains("awaited 8000") }, "\(backend): the await chain stopped while idle")
        }
    }

    func testEndlessPromiseJobsAreChargedToTheQuota() {
        for backend in backends {
            let (engine, recorder) = start("<div>x</div><script>var c=0;function f(){c++;Promise.resolve().then(f)}f();</script>", backend: backend)
            wait(2.5) { recorder.died != nil || recorder.throttled > 0 }
            engine.terminate()
            XCTAssertNil(recorder.died, "\(backend)")
            XCTAssertGreaterThan(recorder.throttled, 0, "\(backend): follow-up job tasks must wait for the CPU quota")
        }
    }

    func testIdleTimerTasksCostARemoteClientNoCallbacks() {
        for backend in backends {
            let (engine, recorder) = start("<div id=a>x</div><script>var c=0;function f(){c++;setTimeout(f,0)}for(var i=0;i<12;i++)f();</script>", backend: backend, remote: true)
            wait(1) { recorder.died != nil }
            let before = recorder.callbacks
            wait(1.5) { recorder.died != nil }
            let rate = Double(recorder.callbacks - before) / 1.5
            engine.terminate()
            XCTAssertNil(recorder.died, "\(backend)")
            XCTAssertLessThan(rate, 100, "\(backend): \(rate) client callbacks per second for timers that change nothing")
        }
    }

    func testRemoteTasksThatSayAnythingKeepTheirBoundaries() {
        for backend in backends {
            let (engine, recorder) = start("<div id=a>x</div><script>var n=0,a=document.getElementById('a');setInterval(function(){a.textContent=String(++n)},20);setInterval(function(){},5);</script>", backend: backend, remote: true)
            wait(1.5) { recorder.died != nil }
            let started = recorder.started
            engine.terminate()
            XCTAssertNil(recorder.died, "\(backend)")
            XCTAssertEqual(recorder.outsideTask, 0, "\(backend): ops outside a task")
            XCTAssertGreaterThan(started, 20, "\(backend): the 20 ms text changes")
            XCTAssertLessThan(started, 150, "\(backend): the 5 ms timer that changes nothing")
        }
    }
}
