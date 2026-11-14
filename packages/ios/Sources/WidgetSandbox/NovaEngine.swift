// The Nova engine (packages/nova-engine): a JavaScript engine written in Rust
// with `#![forbid(unsafe_code)]`, behind the C interface of its nova_embed_ffi
// crate (nova_engine.h), the only place with `unsafe`. No engine pointer or
// value reaches this file: scripts and arguments go in as UTF-8 copies, and a
// host function receives copies of its arguments (strings, numbers, the bytes
// of typed arrays), valid during the call, and never calls back into the
// engine (the library refuses such a call).
//
// Nova runs the sandbox bundle from source: the `.qbc` files are QuickJS
// bytecode. A panic inside the engine is caught by the library and makes the
// engine dead; every later call fails with `QuickJSError.engineFault`.

#if os(macOS) && canImport(NovaEngineFFI)
import Foundation
import NovaEngineFFI

final class NovaEngine: QuickJSEngine {
    /// "nova_embed <version> (nova <commit>)".
    static var version: String { String(cString: nova_embed_version()) }

    /// Whether this build of the library enforces the interrupt (CPU deadlines), the memory limit and the stack guard
    /// (`nova_embed_limits`). An engine is never created without all three: a script that never ends would never be
    /// stopped, and runaway recursion would overflow the thread's stack.
    static var enforcesLimits: Bool {
        let all = UInt32(NOVA_LIMIT_INTERRUPT) | UInt32(NOVA_LIMIT_HEAP) | UInt32(NOVA_LIMIT_STACK)
        return nova_embed_limits() & all == all
    }

    /// The native stack Nova's JavaScript may use on a thread whose QuickJS limit is `maxStackBytes`: three quarters
    /// of it, so the frames between two of the engine's checks and the host's own stay inside the thread's stack.
    static func stackLimit(maxStackBytes: Int) -> Int { max(0, maxStackBytes) / 4 * 3 }

    private enum Handler {
        case strings(([String]) -> HostReturn)
        case words((Double, UnsafeBufferPointer<Double>, String) -> Void)
        case ops((UnsafeBufferPointer<Int32>, UnsafeBufferPointer<UInt8>) -> Void)
    }

    private var engine: OpaquePointer!
    private var handlers: [Handler] = []

    // Read by the interrupt callback, which the engine polls every 4096 instructions, inside a call on this thread.
    var deadline: TimeInterval = .infinity
    var cpuDeadline: TimeInterval = .infinity
    var interrupted = false

    init(memoryLimitBytes: Int, maxStackBytes: Int) throws {
        guard Self.enforcesLimits else {
            throw QuickJSError.engineFault("this build of the Nova engine does not enforce its limits (\(nova_embed_limits()) of 7)")
        }
        var error = nova_result()
        let opaque = Unmanaged.passUnretained(self).toOpaque()
        guard let engine = nova_engine_new(max(0, memoryLimitBytes), Self.stackLimit(maxStackBytes: maxStackBytes), novaHostCall, novaInterrupt, opaque, &error) else {
            defer { nova_result_free(&error) }
            throw QuickJSError.engineFault(Self.text(error).isEmpty ? "the Nova engine could not be created" : Self.text(error))
        }
        nova_result_free(&error)
        nova_engine_set_time_zone(engine, novaTimeZoneOffset)
        self.engine = engine
    }

    deinit {
        nova_engine_free(engine)
    }

    var memoryUsed: Int { nova_engine_memory_used(engine) }

    func setMemoryLimit(_ bytes: Int) { _ = nova_engine_set_memory_limit(engine, max(0, bytes)) }

    var memoryUsage: [String: Int] {
        let used = memoryUsed
        return ["malloc": used, "used": used]
    }

    func collectGarbage() { _ = nova_engine_collect_garbage(engine) }

    // MARK: host functions

    private func register(_ name: String, length: UInt32, mode: Int32, _ handler: Handler) {
        let id = UInt32(handlers.count)
        handlers.append(handler)
        var name = name
        var result = nova_result()
        _ = name.withUTF8 { nova_engine_register(engine, $0.baseAddress, $0.count, id, length, mode, &result) }
        nova_result_free(&result)
    }

    func registerFunction(_ name: String, _ handler: @escaping ([String]) -> HostReturn) {
        register(name, length: 2, mode: Int32(NOVA_ARGS_VALUES), .strings(handler))
    }

    func registerWordsFunction(_ name: String, _ handler: @escaping (Double, UnsafeBufferPointer<Double>, String) -> Void) {
        register(name, length: 3, mode: Int32(NOVA_ARGS_VALUES_AND_BYTES), .words(handler))
    }

    func registerOpsFunction(_ name: String, _ handler: @escaping (UnsafeBufferPointer<Int32>, UnsafeBufferPointer<UInt8>) -> Void) {
        register(name, length: 2, mode: Int32(NOVA_ARGS_VALUES_AND_BYTES), .ops(handler))
    }

    /// What hostTrampoline builds for the native engine: strings up to their first NUL, numbers formatted, the rest
    /// empty; the words and ops functions take the arguments the native trampolines accept and ignore anything else.
    fileprivate func dispatch(id: UInt32, args: UnsafeBufferPointer<nova_arg>, ret: OpaquePointer?) {
        guard Int(id) < handlers.count else { return }
        switch handlers[Int(id)] {
        case .strings(let handler):
            Self.give(handler(args.map(Self.string)), to: ret)
        case .words(let handler):
            guard args.count >= 3, Int(args[0].kind) == NOVA_VALUE_NUMBER, Int(args[2].kind) == NOVA_VALUE_STRING,
                  Int(args[1].kind) == NOVA_VALUE_BYTES, Int(args[1].array_kind) == NOVA_ARRAY_FLOAT64, args[1].len % 8 == 0 else { return }
            let text = Self.utf8(args[2])
            Self.withAligned(args[1], of: Double.self) { handler(args[0].number, $0, text) }
        case .ops(let handler):
            guard args.count >= 2, Int(args[1].kind) == NOVA_VALUE_STRING,
                  Int(args[0].kind) == NOVA_VALUE_BYTES, Int(args[0].array_kind) == NOVA_ARRAY_INT32, args[0].len % 4 == 0 else { return }
            let strings = UnsafeBufferPointer(start: args[1].bytes, count: args[1].bytes == nil ? 0 : args[1].len)
            Self.withAligned(args[0], of: Int32.self) { handler($0, strings) }
        }
    }

    private static func utf8(_ arg: nova_arg) -> String {
        guard let bytes = arg.bytes else { return "" }
        return String(decoding: UnsafeBufferPointer(start: bytes, count: arg.len), as: UTF8.self)
    }

    private static func string(_ arg: nova_arg) -> String {
        switch Int(arg.kind) {
        case NOVA_VALUE_STRING:
            guard let bytes = arg.bytes else { return "" }
            let buffer = UnsafeBufferPointer(start: bytes, count: arg.len)
            let end = buffer.firstIndex(of: 0) ?? buffer.count
            return String(decoding: UnsafeBufferPointer(rebasing: buffer[..<end]), as: UTF8.self)
        case NOVA_VALUE_NUMBER:
            let d = arg.number
            return d == d.rounded() && abs(d) < 1e15 ? String(Int64(d)) : String(d)
        default:
            return ""
        }
    }

    /// The argument's bytes as `T`s: in place when aligned, else an aligned copy.
    private static func withAligned<T>(_ arg: nova_arg, of: T.Type, _ body: (UnsafeBufferPointer<T>) -> Void) {
        let count = arg.len / MemoryLayout<T>.stride
        guard let bytes = arg.bytes, count > 0 else {
            body(UnsafeBufferPointer(start: nil, count: 0))
            return
        }
        let raw = UnsafeRawPointer(bytes)
        if Int(bitPattern: raw) % MemoryLayout<T>.alignment == 0 {
            body(UnsafeBufferPointer(start: raw.assumingMemoryBound(to: T.self), count: count))
        } else {
            let copy = UnsafeMutableBufferPointer<T>.allocate(capacity: count)
            defer { copy.deallocate() }
            UnsafeMutableRawBufferPointer(copy).copyMemory(from: UnsafeRawBufferPointer(start: raw, count: count * MemoryLayout<T>.stride))
            body(UnsafeBufferPointer(copy))
        }
    }

    private static func give(_ value: HostReturn, to ret: OpaquePointer?) {
        switch value {
        case .undefined:
            break
        case .null:
            nova_return_null(ret)
        case .number(let n):
            nova_return_number(ret, n)
        case .string(var s):
            // The native engine takes it as a C string: up to the first NUL.
            s.withUTF8 { b in
                let end = b.firstIndex(of: 0) ?? b.count
                nova_return_string(ret, b.baseAddress, end)
            }
        case .arrayBuffer(let data):
            data.withUnsafeBytes { nova_return_bytes(ret, $0.bindMemory(to: UInt8.self).baseAddress, data.count) }
        }
    }

    // MARK: evaluating and calling

    private static func text(_ r: nova_result) -> String {
        guard let p = r.bytes else { return "" }
        return String(decoding: UnsafeBufferPointer(start: p, count: r.len), as: UTF8.self)
    }

    private func error(_ status: Int32, _ r: nova_result) -> QuickJSError {
        let text = Self.text(r)
        switch Int(status) {
        case NOVA_EXCEPTION: return .exception(text.isEmpty ? "unknown error" : text)
        case NOVA_NOT_A_FUNCTION: return .notAFunction(text)
        case NOVA_INTERRUPTED: return .exception("InternalError: interrupted")
        case NOVA_OUT_OF_MEMORY: return .exception("InternalError: out of memory")
        default: return .engineFault(text.isEmpty ? "the Nova engine failed (status \(status))" : text)
        }
    }

    private func take(_ status: Int32, _ r: inout nova_result) throws -> JSResult {
        defer { nova_result_free(&r) }
        guard Int(status) == NOVA_OK else { throw error(status, r) }
        switch Int(r.kind) {
        case NOVA_VALUE_STRING: return .string(Self.text(r))
        case NOVA_VALUE_NUMBER: return .number(r.number)
        default: return .undefined
        }
    }

    func evaluate(_ code: String, filename: String, asModule: Bool) throws -> JSResult {
        var code = code
        var result = nova_result()
        let status = code.withUTF8 { nova_engine_eval(engine, $0.baseAddress, $0.count, asModule, &result) }
        return try take(status, &result)
    }

    func evaluateBytecode(_ blob: Data, source: String) throws -> JSResult {
        throw QuickJSError.exception("the Nova engine does not run QuickJS bytecode")
    }

    func compileBytecode(_ code: String, filename: String) throws -> Data {
        throw QuickJSError.exception("the Nova engine does not compile QuickJS bytecode")
    }

    func captureGlobalFunction(_ name: String) -> Bool {
        var name = name
        var captured = false
        var result = nova_result()
        let status = name.withUTF8 { nova_engine_capture(engine, $0.baseAddress, $0.count, &captured, &result) }
        nova_result_free(&result)
        return Int(status) == NOVA_OK && captured
    }

    func call(_ name: String, _ args: [String]) throws -> JSResult {
        guard args.count <= Int(NOVA_MAX_CALL_ARGS) else { throw QuickJSError.engineFault("more than \(NOVA_MAX_CALL_ARGS) arguments to \(name)") }
        // Every argument's UTF-8 in one buffer, so the pointers stay valid for the call.
        var bytes: [UInt8] = []
        var ranges: [(Int, Int)] = []
        for arg in args {
            let start = bytes.count
            bytes.append(contentsOf: arg.utf8)
            ranges.append((start, bytes.count - start))
        }
        var name = name
        var result = nova_result()
        let status: Int32 = bytes.withUnsafeBufferPointer { buf in
            let strs = ranges.map { range in nova_str(ptr: range.1 == 0 ? nil : buf.baseAddress.map { $0 + range.0 }, len: range.1) }
            return strs.withUnsafeBufferPointer { argv in
                name.withUTF8 { nova_engine_call(engine, $0.baseAddress, $0.count, argv.baseAddress, argv.count, &result) }
            }
        }
        return try take(status, &result)
    }

    var hasPendingJob: Bool { nova_engine_has_pending_job(engine) }

    func executePendingJobs(onError: (QuickJSError) -> Void) {
        for _ in 0..<100 {
            var ran = false
            var result = nova_result()
            let status = nova_engine_run_job(engine, &ran, &result)
            if !ran {
                nova_result_free(&result)
                break
            }
            if Int(status) != NOVA_OK {
                let failure = error(status, result)
                nova_result_free(&result)
                onError(failure)
                if case .engineFault = failure { break }
                continue
            }
            nova_result_free(&result)
        }
    }
}

// The two callbacks of the library. They run on the engine's thread, inside a call into it, and must not call into
// it: everything they need arrives as arguments.

private func novaHostCall(_ opaque: UnsafeMutableRawPointer?, _ id: UInt32, _ args: UnsafePointer<nova_arg>?, _ argc: Int, _ ret: OpaquePointer?) {
    guard let opaque else { return }
    let engine = Unmanaged<NovaEngine>.fromOpaque(opaque).takeUnretainedValue()
    engine.dispatch(id: id, args: UnsafeBufferPointer(start: args, count: args == nil ? 0 : argc), ret: ret)
}

/// Seconds east of UTC at that instant in the user's time zone, daylight saving time included: what QuickJS reads from
/// localtime_r, and the box from its `tz_offset` import.
private func novaTimeZoneOffset(_ opaque: UnsafeMutableRawPointer?, _ unixSeconds: Int64) -> Int32 {
    Int32(TimeZone.current.secondsFromGMT(for: Date(timeIntervalSince1970: TimeInterval(unixSeconds))))
}

private func novaInterrupt(_ opaque: UnsafeMutableRawPointer?) -> Bool {
    guard let opaque else { return false }
    let engine = Unmanaged<NovaEngine>.fromOpaque(opaque).takeUnretainedValue()
    if ProcessInfo.processInfo.systemUptime > engine.deadline
        || (engine.cpuDeadline != .infinity && QuickJSContext.threadCPUTime() > engine.cpuDeadline) {
        engine.interrupted = true
        return true
    }
    return false
}
#endif
