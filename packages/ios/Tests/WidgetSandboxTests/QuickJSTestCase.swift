import XCTest
import WidgetSandbox
import WidgetSandboxQuickJS

/// The base of every test class here. Its class `setUp` registers the QuickJS engines and their `.qbc`
/// (WidgetSandboxQuickJS) before the class's first test: the only way they get into a process. XCTest creates every
/// test case before the first class `setUp`, so a stored property's initial value must not reach
/// `SandboxResources.shared` (it would read the bytecode before it is registered).
class QuickJSTestCase: XCTestCase {
    override class func setUp() {
        super.setUp()
        registerQuickJSEngines()
    }

    /// The boxed engine: on macOS this package always builds it, so a missing one fails the test (a broken
    /// registration); elsewhere the test is skipped.
    func requireBoxedQuickJS(file: StaticString = #filePath, line: UInt = #line) throws {
        #if os(macOS)
        XCTAssertTrue(QuickJSBackend.isNativeAvailable, "native QuickJS is not registered", file: file, line: line)
        XCTAssertTrue(QuickJSBackend.isBoxedAvailable, "the boxed QuickJS engine is not registered", file: file, line: line)
        #else
        try XCTSkipUnless(QuickJSBackend.isBoxedAvailable, "the boxed engine is not part of this build")
        #endif
    }
}
