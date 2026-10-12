// Pre-warmed sandbox parts for hosts that come and go quickly (a scrolling
// chat): engines whose runtime already evaluated the sandbox bundle, and
// validators (one trusted QuickJS context each, all on the shared validator
// thread) that are recycled with a fresh instance when a host is destroyed.
// Renderers have their own pool next to the WebKit environment
// (`RendererPool`); `WidgetWarmup` fills all three. Main thread only.
//
// An engine is never recycled: once widget code ran in a runtime, the runtime
// dies with the widget. A validator never ran untrusted code, so recycling it
// is `__twValidator.create()` again in the same context.

import Foundation
import QuartzCore

public final class EnginePool {
    public static let shared = EnginePool()

    /// Engines to keep prewarmed; 0 disables the pool.
    public var targetSize = 0 { didSet { refill() } }
    public var resources: SandboxResources = .shared
    public private(set) var created = 0, taken = 0, hits = 0
    private var idle: [SandboxEngine] = []

    public var count: Int { idle.count }

    public func prewarm(_ n: Int) {
        targetSize = max(targetSize, n)
        refill()
    }

    /// A prewarmed engine (its bundle evaluation may still be running on its own thread), or nil when empty.
    public func take() -> SandboxEngine? {
        taken += 1
        let e = idle.popLast()
        if e != nil { hits += 1 }
        refill()
        return e
    }

    private func refill() {
        while idle.count < targetSize {
            let t0 = CACurrentMediaTime()
            let e = SandboxEngine(resources: resources)
            e.prewarm()
            idle.append(e)
            created += 1
            WidgetWarmup.mainThreadWork?("engine-create", (CACurrentMediaTime() - t0) * 1000)
        }
    }

    public func drain() {
        idle.forEach { $0.terminate() }
        idle.removeAll()
    }
}

public final class ValidatorPool {
    public static let shared = ValidatorPool()

    public var targetSize = 0 { didSet { refill() } }
    public var resources: SandboxResources = .shared
    public private(set) var created = 0, taken = 0, hits = 0, recycled = 0
    private var idle: [TrustedValidator] = []

    public var count: Int { idle.count }

    public func prewarm(_ n: Int) {
        targetSize = max(targetSize, n)
        refill()
    }

    public func take(theme: String) -> TrustedValidator? {
        taken += 1
        guard let v = idle.popLast() else { refill(); return nil }
        hits += 1
        // Pooled validators are created for "light"; the host's theme is applied by a fresh instance.
        v.perform { v.reset(theme: theme) }
        refill()
        return v
    }

    /// Back into the pool with a fresh instance (the previous widget's node model
    /// is gone): cheaper than a new context, so the pool holds up to twice its
    /// target of recycled validators.
    public func recycle(_ validator: TrustedValidator) {
        guard idle.count < max(targetSize * 2, 1) else { return }
        validator.onDiag = { _, _ in }
        validator.perform { validator.reset(theme: "light") }
        idle.append(validator)
        recycled += 1
    }

    /// New contexts are built on the shared validator thread; the main thread never waits for the bundle.
    private func refill() {
        while idle.count < targetSize {
            let t0 = CACurrentMediaTime()
            idle.append(TrustedValidator.background(theme: "light", resources: resources, thread: TrustedValidator.sharedThread, onDiag: { _, _ in }))
            created += 1
            WidgetWarmup.mainThreadWork?("validator-create", (CACurrentMediaTime() - t0) * 1000)
        }
    }

    public func drain() { idle.removeAll() }
}

/// Everything a chat screen prewarms before the first bubble: the WebKit
/// environment (rule list, web content process), renderer documents, engines
/// with the sandbox bundle evaluated, validator contexts.
public enum WidgetWarmup {
    /// Main-thread work the pools do (name, ms): a profiler hook for the demo's hitch hunt.
    public static var mainThreadWork: ((String, Double) -> Void)?

    public static func prewarm(renderers: Int, engines: Int, validators: Int, width: CGFloat = 360, parkingView: PlatformView? = nil) {
        RendererEnvironment.shared.prewarm()
        RendererPool.shared.width = width
        if let parkingView { RendererPool.shared.parkingView = parkingView }
        RendererPool.shared.prewarm(renderers)
        EnginePool.shared.prewarm(engines)
        ValidatorPool.shared.prewarm(validators)
    }

    /// One line for a diagnostics panel.
    public static var status: String {
        "pools: renderers \(RendererPool.shared.count)/\(RendererPool.shared.targetSize) (hits \(RendererPool.shared.hits)/\(RendererPool.shared.taken)), engines \(EnginePool.shared.count)/\(EnginePool.shared.targetSize) (hits \(EnginePool.shared.hits)/\(EnginePool.shared.taken)), validators \(ValidatorPool.shared.count)/\(ValidatorPool.shared.targetSize) (hits \(ValidatorPool.shared.hits)/\(ValidatorPool.shared.taken), recycled \(ValidatorPool.shared.recycled)), rule list \(RendererEnvironment.shared.ruleListSource)"
    }
}
