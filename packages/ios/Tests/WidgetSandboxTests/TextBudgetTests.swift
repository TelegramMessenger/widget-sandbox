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

    /// 8192 complex-script units per document, through the validator bundle every native glue loads.
    func testComplexTextIsBudgetedPerDocument() {
        let renderer = RecordingRenderer()
        let host = try! WidgetHost(html: "<p id=a></p><p id=b></p><script>document.getElementById('a').textContent='\\u0627'.repeat(8000);document.getElementById('b').textContent='\\u0628'.repeat(500);</script>", theme: "light", width: 300, rendererFactory: { _, _, _ in renderer })
        defer { host.destroy() }
        let deadline = Date().addingTimeInterval(5)
        while !(host.diagnostics.rendered || host.diagnostics.dead != nil) && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
        let all = renderer.messages.joined()
        XCTAssertTrue(all.contains(String(repeating: "\u{0627}", count: 8000)))
        XCTAssertFalse(all.contains(String(repeating: "\u{0628}", count: 500)))
        XCTAssertGreaterThan(host.diagnostics.diags["text-budget"]?.count ?? 0, 0)
    }

    /// The same budget in the native node model (binary ops), checked against the JS validator op for op.
    func testComplexTextBudgetIsTheSameInTheNativeModel() {
        let arabic = { (n: Int) in String(repeating: "\u{0627}", count: n) }
        let batch = """
            [{"op":"el","id":2,"tag":"html"},{"op":"ins","parent":1,"id":2,"before":null},{"op":"el","id":3,"tag":"body"},{"op":"ins","parent":2,"id":3,"before":null},
             {"op":"text","id":20,"data":"\(arabic(5000))"},{"op":"ins","parent":3,"id":20,"before":null},
             {"op":"text","id":21,"data":"\(arabic(4000))"},{"op":"ins","parent":3,"id":21,"before":null},
             {"op":"text","id":22,"data":"\(arabic(3000))"},{"op":"ins","parent":3,"id":22,"before":null},
             {"op":"data","id":22,"data":"\(arabic(3193))"},{"op":"data","id":20,"data":"shrunk"},
             {"op":"text","id":23,"data":"\(arabic(5000))"},{"op":"ins","parent":3,"id":23,"before":null},{"op":"rm","id":23},
             {"op":"text","id":24,"data":"\(arabic(5000))"},{"op":"ins","parent":3,"id":24,"before":null}]
            """
        let js = NativeValidatorDifferentialTests.Rig(native: false).json(batch)
        let native = NativeValidatorDifferentialTests.Rig(native: true).json(batch)
        XCTAssertEqual(native.0, js.0)
        XCTAssertEqual(native.1, js.1)
        XCTAssertEqual(native.2, js.2)
        XCTAssertGreaterThanOrEqual(js.2.filter { $0.hasPrefix("text-budget") }.count, 2)
    }
}
