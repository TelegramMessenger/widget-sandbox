// Registers this target's engines with WidgetSandbox (QuickJSEngineRegistry): the native QuickJS engine, the boxed
// one on macOS, and the bundles' `.qbc`. Only an explicit call registers them (a client at start, the tests in
// setUp); linking the target alone does not, and WidgetSandbox never looks for it.

import Foundation
@_spi(QuickJSEngines) import WidgetSandbox

private let registered: Void = {
    QuickJSEngineRegistry.register(QuickJSEngineRegistration(
        isAvailable: { true },
        description: { "native QuickJS" },
        make: { NativeQuickJS(memoryLimitBytes: $0, maxStackBytes: $1) }
    ), for: .native)
    #if os(macOS) && canImport(CQuickJSBox)
    QuickJSEngineRegistry.register(QuickJSEngineRegistration(
        isAvailable: { BoxedQuickJS.isOptimized },
        description: { "boxed QuickJS \(BoxedQuickJS.quickJSCommit) (\(BoxedQuickJS.buildMode))" },
        make: { try BoxedQuickJS(memoryLimitBytes: $0, maxStackBytes: $1) }
    ), for: .boxed)
    #endif
    for name in ["sandbox-bundle", "validator-bundle"] {
        if let url = Bundle.module.url(forResource: name, withExtension: "qbc", subdirectory: "WidgetJS"), let data = try? Data(contentsOf: url) {
            QuickJSEngineRegistry.registerBytecode(data, named: name)
        }
    }
}()

public func registerQuickJSEngines() {
    _ = registered
}
