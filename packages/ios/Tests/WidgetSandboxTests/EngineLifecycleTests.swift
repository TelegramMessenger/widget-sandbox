import XCTest
@testable import WidgetSandbox

/// Regression tests for engine and host lifecycle fixes ported from the macOS app.
final class EngineLifecycleTests: XCTestCase {
    private func config(memory: Int = 64 << 20) -> SandboxConfig {
        var c = SandboxConfig(innerWidth: 400, innerHeight: 300, devicePixelRatio: 2, theme: "light")
        c.memoryLimitBytes = memory
        return c
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
