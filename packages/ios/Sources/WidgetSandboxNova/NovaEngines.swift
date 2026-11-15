// Registers Nova with WidgetSandbox (QuickJSEngineRegistry), as WidgetSandboxQuickJS registers QuickJS: WidgetSandbox
// links no engine, so a process that does not link this target and call `registerNovaEngine()` (or
// `QuickJSBackend.useNovaEverywhere()`) has no Nova and every `.nova` context fails closed. Telegram for macOS links it
// only in its widget service, where widgets and the trusted validator run; the app process runs no JavaScript.

#if os(macOS) && canImport(NovaEngineFFI)
import Foundation
@_spi(QuickJSEngines) import WidgetSandbox

private let registered: Void = {
    QuickJSEngineRegistry.register(QuickJSEngineRegistration(
        isAvailable: { NovaEngine.enforcesLimits },
        description: { NovaEngine.version },
        make: { try NovaEngine(memoryLimitBytes: $0, maxStackBytes: $1) }
    ), for: .nova)
}()

/// Makes `.nova` contexts possible in this process. Linking the target alone does not.
public func registerNovaEngine() {
    _ = registered
}

public extension QuickJSBackend {
    /// Registers Nova and runs every widget engine, the engine pools and the trusted validator of the process on it:
    /// sets `default`, `validatorDefault` and `EnginePool.shared.backend`. Main thread, once at start, before the first
    /// host. It exists only in builds that link Nova, so a client that must not run widgets without it calls this and
    /// does not compile without the library, instead of failing closed at run time.
    static func useNovaEverywhere() {
        registerNovaEngine()
        `default` = .nova
        validatorDefault = .nova
        EnginePool.shared.backend = .nova
    }
}
#endif
