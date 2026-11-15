import XCTest
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
}
