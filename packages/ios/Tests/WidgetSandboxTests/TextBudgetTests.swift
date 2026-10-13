import XCTest
@testable import WidgetSandbox

/// Text nodes are capped at 64 KB by the validator bundle every native glue loads.
final class TextBudgetTests: XCTestCase {
    func testTextNodesAreCappedAt64KB() {
        let renderer = RecordingRenderer()
        let host = try! WidgetHost(html: "<p id=a></p><p id=b></p><script>document.getElementById('a').textContent='a'.repeat(64*1024);document.getElementById('b').textContent='b'.repeat(64*1024+1);</script>", theme: "light", width: 300, rendererFactory: { _, _, _ in renderer })
        defer { host.destroy() }
        let deadline = Date().addingTimeInterval(5)
        while !(host.diagnostics.rendered || host.diagnostics.dead != nil) && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
        let all = renderer.messages.joined()
        XCTAssertTrue(all.contains(String(repeating: "a", count: 64 * 1024)))
        XCTAssertFalse(all.contains(String(repeating: "b", count: 64 * 1024 + 1)))
        XCTAssertGreaterThan(host.diagnostics.diags["protocol-violation"]?.count ?? 0, 0)
    }
}
