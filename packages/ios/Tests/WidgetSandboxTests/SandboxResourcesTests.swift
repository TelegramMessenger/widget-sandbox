import XCTest
@testable import WidgetSandbox

final class SandboxResourcesTests: XCTestCase {
    func testAProcessWithTheResourceProductFindsTheBundles() {
        let resources = SandboxResources()
        XCTAssertNil(resources.refusal, String(describing: resources.refusal))
        XCTAssertFalse(resources.sandboxBundle.isEmpty)
        XCTAssertFalse(resources.validatorBundle.isEmpty)
    }

    func testMissingResourcesAreARefusalNotATrap() throws {
        let empty = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("empty-widget-resources-\(UUID().uuidString).bundle")
        try FileManager.default.createDirectory(at: empty, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: empty) }
        let bundle = try XCTUnwrap(Bundle(url: empty))
        let resources = SandboxResources(bundle: bundle)
        XCTAssertEqual(resources.refusal, SandboxResources.missingRefusal)
        XCTAssertTrue(resources.sandboxBundle.isEmpty)
        XCTAssertNil(resources.sandboxBytecode)
    }
}
