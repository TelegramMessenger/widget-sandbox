import XCTest
@testable import WidgetSandbox

/// Regression tests for engine and host lifecycle fixes ported from the macOS app.
final class EngineLifecycleTests: XCTestCase {
    private func config(memory: Int = 64 << 20) -> SandboxConfig {
        var c = SandboxConfig(innerWidth: 400, innerHeight: 300, devicePixelRatio: 2, theme: "light")
        c.memoryLimitBytes = memory
        return c
    }

    /// A heap too small for the sandbox bundle used to log the bundle's error and
    /// boot the widget anyway, without the shim's network stubs and DOM capture
    /// (under memory pressure the parser even reports a SyntaxError). The engine
    /// must die before any widget code runs, prewarmed or not.
    func testBundleThatFailsToLoadKillsTheEngineBeforeWidgetCode() {
        for kb in [64, 128, 512] {
            for prewarm in [false, true] {
                let d = RecordingDelegate()
                let engine = SandboxEngine(html: "<script>TelegramWidget.ready(); console.log('widget ran');</script>", config: config(memory: kb << 10))
                engine.delegate = d
                if prewarm {
                    engine.prewarm()
                    engine.queue.sync {}
                    XCTAssertFalse(engine.isPrewarmed, "\(kb) KB")
                    XCTAssertNil(d.lock.withLock { d.died }, "a failed prewarm reports nothing (\(kb) KB)")
                }
                engine.start()
                wait(for: [d.loaded], timeout: 20)
                engine.queue.sync {}
                d.lock.lock()
                XCTAssertNotNil(d.died, "\(kb) KB")
                XCTAssertTrue(d.bridge.isEmpty, "\(kb) KB")
                XCTAssertFalse(d.logs.contains { $0.1.contains("widget ran") }, "\(kb) KB")
                XCTAssertTrue(d.ops.isEmpty, "\(kb) KB")
                d.lock.unlock()
                engine.terminate()
            }
        }
    }

    func testSuccessfulPrewarmStillBoots() {
        let d = RecordingDelegate()
        let engine = SandboxEngine(html: "<p>x</p>", config: config())
        engine.delegate = d
        engine.prewarm()
        engine.queue.sync {}
        XCTAssertTrue(engine.isPrewarmed)
        engine.start()
        wait(for: [d.loaded], timeout: 20)
        XCTAssertNil(d.lock.withLock { d.died })
        engine.terminate()
    }

    /// The delegate is written on the owner's thread and read on the JS thread.
    func testDelegateCanBeSwappedWhileTheEngineRuns() {
        let engine = SandboxEngine(html: "<script>setInterval(function(){console.log('t')},1)</script>", config: config())
        let delegates = (0..<4).map { _ in RecordingDelegate() }
        engine.delegate = delegates[0]
        engine.start()
        DispatchQueue.concurrentPerform(iterations: 4) { i in
            for n in 0..<2000 { engine.delegate = (n % 3 == 0) ? nil : delegates[i] }
        }
        engine.delegate = delegates[0]
        engine.queue.sync {}
        engine.terminate()
        engine.queue.sync {}
    }
}
