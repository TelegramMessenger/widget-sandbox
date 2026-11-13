// One QuickJS runtime and context behind one interface, whichever build of the
// engine runs it: the native one (QuickJS.swift, the default everywhere) or the
// same engine inside a wasm2c box (QuickJSBoxed.swift; docs/decisions.md,
// "QuickJS in a wasm2c box"). All methods must be called from the thread/queue
// that owns the context.

import Foundation

/// Which build of QuickJS runs a widget's code.
public enum QuickJSBackend: String {
    /// The engine compiled into the process. The default on every platform.
    case native
    /// The same engine commit compiled to wasm and back to C, in a memory of its own: a memory bug in the engine
    /// reads and writes that memory only, or ends the process. macOS only for now (`isBoxedAvailable`); slower
    /// (JS about 1.4 times with guard pages). The validator's engine stays native.
    case boxed

    /// Whether this build can run `.boxed`. Where it cannot, a boxed engine fails closed: the widget stops with
    /// "Engine fault", it never falls back to the native engine.
    public static var isBoxedAvailable: Bool {
        #if os(macOS) && canImport(CQuickJSBox)
        return BoxedQuickJS.isOptimized
        #else
        return false
        #endif
    }

    /// The backend of hosts and engines that do not name one: `.native`. A client that wants every widget boxed sets
    /// it once at start, before the first host. A DEBUG build takes its first value from the environment
    /// (`WIDGET_SANDBOX_ENGINE=boxed`), which is how the test suite runs on the boxed engine.
    public static var `default`: QuickJSBackend = {
        #if DEBUG
        if let name = ProcessInfo.processInfo.environment["WIDGET_SANDBOX_ENGINE"], let backend = QuickJSBackend(rawValue: name) { return backend }
        #endif
        return .native
    }()
}

/// What `QuickJSContext` needs from an engine. Internal: the rest of the package sees `QuickJSContext` only.
protocol QuickJSEngine: AnyObject {
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
    func evaluateBytecode(_ blob: Data, source: String) throws -> JSResult
    func compileBytecode(_ code: String, filename: String) throws -> Data
    func captureGlobalFunction(_ name: String) -> Bool
    func call(_ name: String, _ args: [String]) throws -> JSResult
    func executePendingJobs(onError: (QuickJSError) -> Void)
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
        case .native:
            engine = NativeQuickJS(memoryLimitBytes: memoryLimitBytes, maxStackBytes: maxStackBytes)
            unavailableReason = nil
        case .boxed:
            #if os(macOS) && canImport(CQuickJSBox)
            do {
                if let reason = Self.boxedFailureForTesting { throw QuickJSError.engineFault(reason) }
                engine = try BoxedQuickJS(memoryLimitBytes: memoryLimitBytes, maxStackBytes: maxStackBytes)
                unavailableReason = nil
            } catch {
                let reason = (error as? QuickJSError).flatMap { if case .engineFault(let r) = $0 { return r } else { return nil } } ?? "\(error)"
                engine = UnavailableQuickJS(reason: reason)
                unavailableReason = reason
            }
            #else
            let reason = "the boxed QuickJS engine is not part of this build (macOS only)"
            engine = UnavailableQuickJS(reason: reason)
            unavailableReason = reason
            #endif
        }
    }

    /// For the tests: makes every new boxed context fail with this reason, as a build without the box does.
    static var boxedFailureForTesting: String?

    /// "native QuickJS", or the boxed engine with the commit it was built from and how its memory is fenced.
    public var engineDescription: String {
        #if os(macOS) && canImport(CQuickJSBox)
        if backend == .boxed { return "boxed QuickJS \(BoxedQuickJS.quickJSCommit) (\(BoxedQuickJS.buildMode))" }
        #endif
        return backend == .boxed ? "boxed QuickJS" : "native QuickJS"
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
    var nativeContext: OpaquePointer? { (engine as? NativeQuickJS)?.context }

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
