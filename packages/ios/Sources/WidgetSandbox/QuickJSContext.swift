// One JavaScript runtime and context behind one interface, whichever engine runs
// it: Nova (NovaEngine.swift), or, when the WidgetSandboxQuickJS target is linked,
// the native QuickJS engine or the same engine inside a wasm2c box
// (docs/decisions.md, "QuickJS in a wasm2c box"). WidgetSandbox itself links no
// QuickJS: those two register here at run time (`QuickJSEngineRegistry`) when a
// client calls `registerQuickJSEngines()`, and a process without them fails closed. All methods must be called from the
// thread/queue that owns the context.

import Foundation

/// Which build of QuickJS runs a widget's code.
public enum QuickJSBackend: String {
    /// quickjs-ng compiled into the process, from the WidgetSandboxQuickJS target; fails closed where it is not linked.
    case native
    /// The same engine commit compiled to wasm and back to C, in a memory of its own: a memory bug in the engine
    /// reads and writes that memory only, or ends the process. macOS only for now (`isBoxedAvailable`); slower
    /// (JS about 1.4 times with guard pages). The validator's engine stays native.
    case boxed
    /// Nova (`packages/nova-engine`): a JavaScript engine written in Rust with no `unsafe`, so a script cannot reach
    /// undefined behaviour in it, behind a small audited C interface. macOS only (`isNovaAvailable`); it runs the
    /// sandbox bundle from source (no `.qbc`). Like `.boxed`, a build without it fails closed.
    case nova

    /// Whether this build can run `.nova`: the library is linked and enforces every limit (`NovaEngine.enforcesLimits`).
    public static var isNovaAvailable: Bool {
        #if os(macOS) && canImport(NovaEngineFFI)
        return NovaEngine.enforcesLimits
        #else
        return false
        #endif
    }

    /// Whether this build can run `.boxed`. Where it cannot, a boxed engine fails closed: the widget stops with
    /// "Engine fault", it never falls back to the native engine.
    public static var isBoxedAvailable: Bool {
        QuickJSEngineRegistry.registration(for: .boxed)?.isAvailable() ?? false
    }

    /// Whether this build can run `.native`: WidgetSandboxQuickJS is linked.
    public static var isNativeAvailable: Bool {
        QuickJSEngineRegistry.registration(for: .native)?.isAvailable() ?? false
    }

    /// The backend of hosts and engines that do not name one: `.nova` in a build that links Nova and whose library
    /// enforces its limits (`isNovaAvailable`), else `.native`. A client that wants another one sets it once at start,
    /// before the first host. A DEBUG build takes its first value from the environment (`WIDGET_SANDBOX_ENGINE=native`
    /// or `boxed`), which is how the test suite runs on the other engines.
    public static var `default`: QuickJSBackend = {
        #if DEBUG
        if let name = ProcessInfo.processInfo.environment["WIDGET_SANDBOX_ENGINE"], let backend = QuickJSBackend(rawValue: name) { return backend }
        #endif
        return isNovaAvailable ? .nova : .native
    }()

    /// The backend of the trusted validator (`TrustedValidator`): `.nova` where `isNovaAvailable`, else `.native`, unless
    /// a client sets it once at start. A DEBUG build takes its first value from `WIDGET_SANDBOX_VALIDATOR_ENGINE`.
    /// `.boxed` is not offered for it.
    public static var validatorDefault: QuickJSBackend = {
        #if DEBUG
        if let name = ProcessInfo.processInfo.environment["WIDGET_SANDBOX_VALIDATOR_ENGINE"], let backend = QuickJSBackend(rawValue: name), backend != .boxed {
            return backend
        }
        #endif
        return isNovaAvailable ? .nova : .native
    }()

    #if os(macOS) && canImport(NovaEngineFFI)
    /// Runs every widget engine, the engine pools and the trusted validator of the process on Nova: sets `default`,
    /// `validatorDefault` and `EnginePool.shared.backend`. Main thread, once at start, before the first host. It
    /// exists only in builds that link Nova, so a client that must not run widgets without it calls this and does not
    /// compile without the library, instead of failing closed at run time.
    public static func useNovaEverywhere() {
        `default` = .nova
        validatorDefault = .nova
        EnginePool.shared.backend = .nova
    }
    #endif

    /// Whether this backend runs the precompiled `.qbc` bundles (QuickJS bytecode); Nova evaluates their source.
    var runsBytecode: Bool { self != .nova }
}

@_spi(QuickJSEngines) public extension QuickJSEngine {
    func evaluateBundle(_ source: String, filename: String) throws -> Bool {
        try evaluate(source, filename: filename, asModule: false)
        return false
    }

    var nativeContext: OpaquePointer? { nil }

    func registerSideChannelFunction(_ name: String, _ handler: @escaping ([Widget3DArgument]) -> HostReturn) -> Bool { false }
}

/// What `QuickJSContext` needs from an engine. SPI for the engines of WidgetSandboxQuickJS; the rest of the package
/// and its clients see `QuickJSContext` only.
@_spi(QuickJSEngines) public protocol QuickJSEngine: AnyObject {
    var deadline: TimeInterval { get set }
    var cpuDeadline: TimeInterval { get set }
    var interrupted: Bool { get set }
    var memoryUsed: Int { get }
    var memoryUsage: [String: Int] { get }
    var hasPendingJob: Bool { get }
    func setMemoryLimit(_ bytes: Int)
    func collectGarbage()
    func registerFunction(_ name: String, _ handler: @escaping ([String]) -> HostReturn)
    func registerWordsFunction(_ name: String, _ handler: @escaping (Double, UnsafeBufferPointer<Double>, String) -> Void)
    func registerOpsFunction(_ name: String, _ handler: @escaping (UnsafeBufferPointer<Int32>, UnsafeBufferPointer<UInt8>) -> Void)
    func evaluate(_ code: String, filename: String, asModule: Bool) throws -> JSResult
    /// One of our own bundles as a global script; true when it ran from code compiled once for the process and shared
    /// by every engine (Nova), false when it was evaluated from its source.
    func evaluateBundle(_ source: String, filename: String) throws -> Bool
    func evaluateBytecode(_ blob: Data, source: String) throws -> JSResult
    func compileBytecode(_ code: String, filename: String) throws -> Data
    func captureGlobalFunction(_ name: String) -> Bool
    func call(_ name: String, _ args: [String]) throws -> JSResult
    func executePendingJobs(onError: (QuickJSError) -> Void)
    /// quickjs-ng's context, for tests; nil on every other engine.
    var nativeContext: OpaquePointer? { get }
    /// `name(op, ...)` for a side channel that reads typed arrays in place (Widget3DChannel); false when this engine
    /// cannot have one.
    func registerSideChannelFunction(_ name: String, _ handler: @escaping ([Widget3DArgument]) -> HostReturn) -> Bool
}

/// An engine WidgetSandboxQuickJS provides for `.native` or `.boxed`.
@_spi(QuickJSEngines) public struct QuickJSEngineRegistration {
    public let isAvailable: () -> Bool
    public let description: () -> String
    public let make: (_ memoryLimitBytes: Int, _ maxStackBytes: Int) throws -> QuickJSEngine

    public init(isAvailable: @escaping () -> Bool, description: @escaping () -> String, make: @escaping (_ memoryLimitBytes: Int, _ maxStackBytes: Int) throws -> QuickJSEngine) {
        self.isAvailable = isAvailable
        self.description = description
        self.make = make
    }
}

/// The QuickJS engines of this process. WidgetSandboxQuickJS registers them, with the bundles' `.qbc`, when its
/// `registerQuickJSEngines()` is called, and only then: nothing here looks for that target, so a process that does
/// not make that call (an app that does not link the target) has no QuickJS, and `.native` and `.boxed` fail closed.
/// Nova is not registered here: it is part of WidgetSandbox.
@_spi(QuickJSEngines) public enum QuickJSEngineRegistry {
    private static let lock = NSLock()
    private static var engines: [QuickJSBackend: QuickJSEngineRegistration] = [:]
    private static var bytecode: [String: Data] = [:]

    public static func register(_ registration: QuickJSEngineRegistration, for backend: QuickJSBackend) {
        precondition(backend != .nova, "Nova is part of WidgetSandbox")
        lock.lock()
        engines[backend] = registration
        lock.unlock()
    }

    /// The `.qbc` of one of our bundles, by its name without the extension ("sandbox-bundle").
    public static func registerBytecode(_ data: Data, named name: String) {
        lock.lock()
        bytecode[name] = data
        lock.unlock()
    }

    static func registration(for backend: QuickJSBackend) -> QuickJSEngineRegistration? {
        lock.lock()
        defer { lock.unlock() }
        return engines[backend]
    }

    static func bytecode(named name: String) -> Data? {
        lock.lock()
        defer { lock.unlock() }
        return bytecode[name]
    }
}

public final class QuickJSContext {
    public let backend: QuickJSBackend
    private let engine: QuickJSEngine
    /// Why this context cannot run anything (a boxed engine this build cannot provide), or nil. Every evaluation
    /// and call of such a context throws `QuickJSError.engineFault` with it.
    public let unavailableReason: String?

    /// `maxStackBytes` must stay below the real stack of whichever thread enters JS
    /// (GCD pool threads: 512 KB; iOS main thread: 1 MB; JSThread: 8 MB). QuickJS
    /// only *detects* overflow against this limit; the OS kills the process at the
    /// real one. A debug build of quickjs-ng needs ~10 KB of C stack per JS call.
    /// The boxed engine derives its own limit from it (the box's code uses several
    /// times more native stack than the stack QuickJS measures there).
    public init(memoryLimitBytes: Int, maxStackBytes: Int = 384 << 10, backend: QuickJSBackend = .native) {
        self.backend = backend
        switch backend {
        case .native, .boxed:
            do {
                if backend == .boxed, let reason = Self.boxedFailureForTesting { throw QuickJSError.engineFault(reason) }
                guard let registration = QuickJSEngineRegistry.registration(for: backend) else {
                    throw QuickJSError.engineFault(backend == .native
                        ? "native QuickJS is not part of this build (WidgetSandboxQuickJS is not linked)"
                        : "the boxed QuickJS engine is not part of this build (WidgetSandboxQuickJS is not linked, macOS only)")
                }
                engine = try registration.make(memoryLimitBytes, maxStackBytes)
                unavailableReason = nil
            } catch {
                let reason = (error as? QuickJSError).flatMap { if case .engineFault(let r) = $0 { return r } else { return nil } } ?? "\(error)"
                engine = UnavailableQuickJS(reason: reason)
                unavailableReason = reason
            }
        case .nova:
            #if os(macOS) && canImport(NovaEngineFFI)
            do {
                engine = try NovaEngine(memoryLimitBytes: memoryLimitBytes, maxStackBytes: maxStackBytes)
                unavailableReason = nil
            } catch {
                let reason = (error as? QuickJSError).flatMap { if case .engineFault(let r) = $0 { return r } else { return nil } } ?? "\(error)"
                engine = UnavailableQuickJS(reason: reason)
                unavailableReason = reason
            }
            #else
            let reason = "the Nova engine is not part of this build (macOS only)"
            engine = UnavailableQuickJS(reason: reason)
            unavailableReason = reason
            #endif
        }
    }

    /// For the tests: makes every new boxed context fail with this reason, as a build without the box does.
    static var boxedFailureForTesting: String?

    /// "native QuickJS", the boxed engine with the commit it was built from and how its memory is fenced, or Nova's
    /// version with its commit.
    public var engineDescription: String {
        if backend != .nova, unavailableReason == nil, let registration = QuickJSEngineRegistry.registration(for: backend) {
            return registration.description()
        }
        #if os(macOS) && canImport(NovaEngineFFI)
        if backend == .nova { return NovaEngine.version }
        #endif
        switch backend {
        case .native: return "native QuickJS"
        case .boxed: return "boxed QuickJS"
        case .nova: return "Nova"
        }
    }

    /// CPU time consumed by the calling thread, in seconds; unaffected by how busy the machine is.
    public static func threadCPUTime() -> TimeInterval {
        var t = timespec()
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)
        return TimeInterval(t.tv_sec) + TimeInterval(t.tv_nsec) / 1e9
    }

    /// Interrupt deadline in `ProcessInfo.systemUptime` seconds; `.infinity` disables it.
    public var deadline: TimeInterval {
        get { engine.deadline }
        set { engine.deadline = newValue }
    }
    /// Interrupt deadline in CPU seconds of the thread running JS (`threadCPUTime`); `.infinity` disables it.
    public var cpuDeadline: TimeInterval {
        get { engine.cpuDeadline }
        set { engine.cpuDeadline = newValue }
    }
    /// Set by the interrupt handler when the deadline fired; reset by the caller.
    public internal(set) var interrupted: Bool {
        get { engine.interrupted }
        set { engine.interrupted = newValue }
    }

    /// The native engine's context, for the one piece of glue that talks to quickjs-ng itself (the 3D side
    /// channel, debug builds only); nil on the boxed engine, where no engine pointer leaves the box.
    var nativeContext: OpaquePointer? { engine.nativeContext }

    func registerSideChannelFunction(_ name: String, _ handler: @escaping ([Widget3DArgument]) -> HostReturn) -> Bool {
        engine.registerSideChannelFunction(name, handler)
    }

    public var memoryUsed: Int { engine.memoryUsed }

    /// A prewarmed runtime adopts the limit of the host that takes it.
    public func setMemoryLimit(_ bytes: Int) { engine.setMemoryLimit(bytes) }

    /// `JS_ComputeMemoryUsage` in bytes, for diagnostics and memory tests.
    public var memoryUsage: [String: Int] { engine.memoryUsage }

    public func collectGarbage() { engine.collectGarbage() }

    /// Exposes `name(...)` to the sandbox. Arguments arrive as strings (numbers are formatted).
    public func registerFunction(_ name: String, _ handler: @escaping ([String]) -> HostReturn) { engine.registerFunction(name, handler) }

    /// `name(number, words: Float64Array, text)`: the words are only valid during the call.
    public func registerWordsFunction(_ name: String, _ handler: @escaping (Double, UnsafeBufferPointer<Double>, String) -> Void) {
        engine.registerWordsFunction(name, handler)
    }

    /// `name(words: Int32Array, strings: string)`: the words and the string's UTF-8 are only valid during the call.
    public func registerOpsFunction(_ name: String, _ handler: @escaping (UnsafeBufferPointer<Int32>, UnsafeBufferPointer<UInt8>) -> Void) {
        engine.registerOpsFunction(name, handler)
    }

    @discardableResult
    public func evaluate(_ code: String, filename: String, asModule: Bool = false) throws -> JSResult {
        try engine.evaluate(code, filename: filename, asModule: asModule)
    }

    /// Runs one of our own bundles, never widget code: on Nova from code compiled once for the process and shared by
    /// every engine (the first engine compiles it), elsewhere from its source. True when it ran from the shared code.
    @discardableResult
    func evaluateBundle(_ source: String, filename: String) throws -> Bool {
        try engine.evaluateBundle(source, filename: filename)
    }

    /// Runs one of our own precompiled bundles (`.qbc`, see qjs_shim.h), checked
    /// against the source it was built from; throws on any mismatch so the caller
    /// can fall back to `evaluate`. Never for widget code: bytecode is not verified.
    @discardableResult
    public func evaluateBytecode(_ blob: Data, source: String) throws -> JSResult {
        try engine.evaluateBytecode(blob, source: source)
    }

    /// Compiles `code` into the same container the build ships (tests).
    public func compileBytecode(_ code: String, filename: String) throws -> Data {
        try engine.compileBytecode(code, filename: filename)
    }

    /// Keeps a private reference to the global function `name`, which `call(name)` uses from then on even after the global is deleted.
    @discardableResult
    public func captureGlobalFunction(_ name: String) -> Bool { engine.captureGlobalFunction(name) }

    /// Calls the global function `name` with string arguments.
    @discardableResult
    public func call(_ name: String, _ args: [String] = []) throws -> JSResult { try engine.call(name, args) }

    public var hasPendingJob: Bool { engine.hasPendingJob }

    /// Runs queued promise jobs. Errors thrown by jobs are reported through `onError` and do not stop the loop.
    public func executePendingJobs(onError: (QuickJSError) -> Void) { engine.executePendingJobs(onError: onError) }
}

/// The engine of a context that cannot have one: nothing runs, every evaluation and call fails with the reason.
private final class UnavailableQuickJS: QuickJSEngine {
    let reason: String
    init(reason: String) { self.reason = reason }

    var deadline: TimeInterval = .infinity
    var cpuDeadline: TimeInterval = .infinity
    var interrupted = false
    var memoryUsed: Int { 0 }
    var memoryUsage: [String: Int] { [:] }
    var hasPendingJob: Bool { false }
    func setMemoryLimit(_ bytes: Int) {}
    func collectGarbage() {}
    func registerFunction(_ name: String, _ handler: @escaping ([String]) -> HostReturn) {}
    func registerWordsFunction(_ name: String, _ handler: @escaping (Double, UnsafeBufferPointer<Double>, String) -> Void) {}
    func registerOpsFunction(_ name: String, _ handler: @escaping (UnsafeBufferPointer<Int32>, UnsafeBufferPointer<UInt8>) -> Void) {}
    func evaluate(_ code: String, filename: String, asModule: Bool) throws -> JSResult { throw QuickJSError.engineFault(reason) }
    func evaluateBytecode(_ blob: Data, source: String) throws -> JSResult { throw QuickJSError.engineFault(reason) }
    func compileBytecode(_ code: String, filename: String) throws -> Data { throw QuickJSError.engineFault(reason) }
    func captureGlobalFunction(_ name: String) -> Bool { false }
    func call(_ name: String, _ args: [String]) throws -> JSResult { throw QuickJSError.engineFault(reason) }
    func executePendingJobs(onError: (QuickJSError) -> Void) {}
}
