import WebKit
import XCTest
@testable import WidgetSandbox

final class SingleWebProcessTests: XCTestCase {
    @MainActor
    func testRenderersShareOneWebContentProcess() async throws {
        let environment = RendererEnvironment()
        environment.singleWebProcess = true
        environment.prewarm()
        XCTAssertEqual(environment.processMode, "single")

        let window = TestWindow.makeVisible()
        let renderers = (0..<3).map { _ in
            RendererWebView(rendererBundle: SandboxResources.shared.rendererBundle, theme: "light", environment: environment)
        }
        defer {
            renderers.forEach { $0.destroy() }
            window.close()
        }
        for renderer in renderers {
            window.attach(renderer.webView)
            renderer.load()
        }

        var pids: [Int32] = []
        let deadline = Date().addingTimeInterval(15)
        repeat {
            pids = renderers.map { ($0.webView.value(forKey: "_webProcessIdentifier") as? NSNumber)?.int32Value ?? 0 }
            if !pids.contains(0) { break }
            try await Task.sleep(nanoseconds: 20_000_000)
        } while Date() < deadline
        XCTAssertFalse(pids.contains(0), "\(pids)")
        XCTAssertEqual(Set(pids).count, 1, "\(pids)")
    }
}
