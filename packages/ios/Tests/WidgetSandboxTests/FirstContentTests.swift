import XCTest
@testable import WidgetSandbox

final class FirstContentTests: XCTestCase {
    func testSizeReportedWhileApplyingOpsCountsAsFirstContent() {
        let host = try! WidgetHost(html: "<div style='height:120px'>Hello</div>", theme: WidgetTheme(), width: 300, rendererFactory: { _, _, _ in SynchronousSizeRenderer() })
        defer { host.destroy() }
        let deadline = Date().addingTimeInterval(5)
        while host.diagnostics.firstContentMs == nil && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
        XCTAssertNil(host.view.window)
        XCTAssertNotNil(host.diagnostics.firstContentMs)
        XCTAssertEqual(host.diagnostics.height, 120)
    }

    /// A renderer that was ready before the engine sent anything reports its size once, before any ops; ops that keep
    /// that size bring no new report. The first content is then the render ack after them (WidgetPreparer waits for it).
    func testOpsThatKeepTheReportedSizeAreTheFirstContentAtTheRenderAck() throws {
        let renderer = EarlySizeRenderer()
        let host = try WidgetHost(html: "<p id='p'>hello</p><script>document.body.setAttribute('data-x', '1')</script>", width: 300, rendererFactory: { _, _, _ in renderer })
        defer { host.destroy() }
        pump { host.diagnostics.rendered && host.diagnostics.firstContentMs != nil }
        XCTAssertTrue(host.diagnostics.loaded)
        XCTAssertTrue(host.diagnostics.rendered)
        XCTAssertGreaterThan(renderer.opsBatches, 0)
        XCTAssertNotNil(host.diagnostics.firstContentMs, "the size was reported before the ops and never again")
        XCTAssertNil(host.diagnostics.dead)
    }

    func testAnUnpaintedSizeReportIsNoFirstContent() throws {
        let renderer = EarlySizeRenderer(painted: false)
        let host = try WidgetHost(html: "<p>hello</p>", width: 300, rendererFactory: { _, _, _ in renderer })
        defer { host.destroy() }
        pump { host.diagnostics.rendered }
        XCTAssertTrue(host.diagnostics.rendered)
        pump(0.3) { false }
        XCTAssertNil(host.diagnostics.firstContentMs)
    }

    /// With the size reported before the ops and not changed by them, the first content is the answer to the render ack
    /// asked for after the ops (the document's skeleton at least), not earlier.
    func testFirstContentWaitsForTheAnswerToAnAckAskedForAfterTheOps() throws {
        let renderer = ManualAckRenderer()
        let host = try WidgetHost(html: "<p>hello</p>", width: 300, rendererFactory: { _, _, _ in renderer })
        defer { host.destroy() }
        pump { renderer.acks.count == 1 }
        XCTAssertEqual(renderer.acks.count, 1)
        XCTAssertGreaterThan(renderer.acks.first ?? 0, 0, "the post-load ack is asked for after the loading ops")
        pump(0.3) { false }
        XCTAssertNil(host.diagnostics.firstContentMs, "no size report followed the ops and the ack is not answered yet")
        renderer.answerAck()
        pump { host.diagnostics.firstContentMs != nil }
        XCTAssertNotNil(host.diagnostics.firstContentMs)
        XCTAssertTrue(host.diagnostics.rendered)
        XCTAssertEqual(renderer.acks.count, 1)
    }

    private func pump(_ seconds: TimeInterval = 10, until done: () -> Bool) {
        let deadline = Date().addingTimeInterval(seconds)
        while !done() && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
    }
}

private final class EarlySizeRenderer: Renderer {
    let view = PlatformView(frame: CGRect(x: 0, y: 0, width: 300, height: 100))
    var onMessage: ((String) -> Void)?
    var onDiag: ((String, String) -> Void)?
    let painted: Bool
    var opsBatches = 0

    init(painted: Bool = true) {
        self.painted = painted
    }

    func load() {
        onMessage?("{\"type\":\"ready\"}")
        onMessage?("{\"type\":\"size\",\"width\":300,\"height\":60,\"innerWidth\":300,\"innerHeight\":60,\"dpr\":2\(painted ? "" : ",\"painted\":false")}")
    }

    func send(_ json: String) {
        guard let message = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] else {
            opsBatches += 1
            return
        }
        if message["type"] as? String == "ops" {
            opsBatches += 1
        }
        guard message["type"] as? String == "query", let qid = (message["qid"] as? NSNumber)?.intValue else { return }
        DispatchQueue.main.async { [weak self] in
            self?.onMessage?("{\"type\":\"result\",\"qid\":\(qid),\"result\":{\"innerWidth\":300,\"innerHeight\":60}}")
        }
    }

    func setTheme(_ theme: String) {}
    func evaluate(_ script: String, completion: @escaping (String?) -> Void) { completion(nil) }
    func destroy() {}
}

private final class SynchronousSizeRenderer: Renderer {
    let view = PlatformView(frame: CGRect(x: 0, y: 0, width: 300, height: 100))
    var onMessage: ((String) -> Void)?
    var onDiag: ((String, String) -> Void)?

    func load() {
        onMessage?("{\"type\":\"ready\"}")
    }

    func send(_ json: String) {
        guard let data = json.data(using: .utf8), let message = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any], let type = message["type"] as? String else {
            return
        }
        if type == "ops" {
            onMessage?("{\"type\":\"size\",\"width\":300,\"height\":120}")
        } else if type == "query", let qid = message["qid"] as? Int {
            DispatchQueue.main.async { [weak self] in
                self?.onMessage?("{\"type\":\"result\",\"qid\":\(qid),\"result\":null}")
            }
        }
    }

    func setTheme(_ theme: String) {}
    func evaluate(_ script: String, completion: @escaping (String?) -> Void) { completion(nil) }
    func destroy() {}
}

/// Reports its size at load, answers the host's render acks only when the test says so, and remembers how many op
/// batches it had applied when each ack was asked for.
private final class ManualAckRenderer: Renderer {
    let view = PlatformView(frame: CGRect(x: 0, y: 0, width: 300, height: 100))
    var onMessage: ((String) -> Void)?
    var onDiag: ((String, String) -> Void)?
    var opsBatches = 0
    var acks: [Int] = []
    private var unanswered = 0

    func load() {
        onMessage?("{\"type\":\"ready\"}")
        onMessage?("{\"type\":\"size\",\"width\":300,\"height\":60,\"innerWidth\":300,\"innerHeight\":60,\"dpr\":2}")
    }

    func send(_ json: String) {
        guard let message = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] else {
            opsBatches += 1
            return
        }
        if message["type"] as? String == "ops" {
            opsBatches += 1
        }
        guard message["type"] as? String == "query", let qid = (message["qid"] as? NSNumber)?.intValue else { return }
        if qid < 0 {
            acks.append(opsBatches)
            unanswered += 1
            return
        }
        DispatchQueue.main.async { [weak self] in
            self?.onMessage?("{\"type\":\"result\",\"qid\":\(qid),\"result\":null}")
        }
    }

    func answerAck() {
        guard unanswered > 0 else { return }
        unanswered -= 1
        onMessage?("{\"type\":\"result\",\"qid\":-1,\"result\":{\"innerWidth\":300,\"innerHeight\":60}}")
    }

    func setTheme(_ theme: String) {}
    func evaluate(_ script: String, completion: @escaping (String?) -> Void) { completion(nil) }
    func destroy() {}
}
