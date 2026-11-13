// The boxed QuickJS engine: the same fork commit as the native one, compiled to
// wasm and translated back to C (packages/quickjs-box), behind the scalar C
// surface of CQuickJSBox. One box is one runtime and one context in a linear
// memory of its own. No JSValue and no engine pointer reaches this file: host
// functions receive their arguments as bytes inside the box, valid during the
// call, and never call back into it (the box does the conversions that
// hostTrampoline does for the native engine).
//
// "Contain and crash": a fault inside the box ends the process, as an engine
// crash does today (docs/decisions.md, "QuickJS in a wasm2c box"). What this
// class reports as `QuickJSError.engineFault` is only what it can refuse
// without the engine: a box that cannot be created, a call from inside a host
// function.

#if os(macOS) && canImport(CQuickJSBox)
import CQuickJSBox
import Foundation

final class BoxedQuickJS: QuickJSEngine {
    /// False when CQuickJSBox was compiled without optimization: such a box is 11-48 times slower than the native
    /// engine, far past a widget's CPU limits, and is refused.
    static var isOptimized: Bool { qjsbox_build_optimized() }
    /// "guard-pages" or "bounds-checks", and the QuickJS commit the box was built from.
    static var buildMode: String { String(cString: qjsbox_build_mode()) }
    static var quickJSCommit: String { String(cString: qjsbox_quickjs_commit()) }

    private var box: OpaquePointer!
    private var functions: [Int32: ([String]) -> HostReturn] = [:]
    private var wordFunctions: [Int32: (Double, UnsafeBufferPointer<Double>, String) -> Void] = [:]
    private var opsFunctions: [Int32: (UnsafeBufferPointer<Int32>, UnsafeBufferPointer<UInt8>) -> Void] = [:]
    private var captured: [String: Int32] = [:]

    // The deadlines live in the box's host layer, in C: the engine's interrupt check runs no Swift.
    var deadline: TimeInterval = .infinity { didSet { qjsbox_set_deadlines(box, deadline, cpuDeadline) } }
    var cpuDeadline: TimeInterval = .infinity { didSet { qjsbox_set_deadlines(box, deadline, cpuDeadline) } }
    var interrupted: Bool {
        get { qjsbox_interrupted(box) }
        set { if !newValue { qjsbox_clear_interrupted(box) } }
    }

    init(memoryLimitBytes: Int, maxStackBytes: Int) throws {
        var options = qjsbox_options(memory_limit_bytes: max(0, memoryLimitBytes), native_stack_bytes: max(0, maxStackBytes))
        var callbacks = qjsbox_callbacks(opaque: Unmanaged.passUnretained(self).toOpaque(), call: boxedCall, words: boxedWords, ops: boxedOps)
        var error: UnsafePointer<CChar>?
        guard let box = qjsbox_new(&options, &callbacks, &error) else {
            throw QuickJSError.engineFault(error.map { String(cString: $0) } ?? "the box could not be created")
        }
        self.box = box
    }

    deinit {
        // No engine code runs here: the box's memory is released whatever state the engine is in.
        qjsbox_free(box)
    }

    var memoryUsed: Int { qjsbox_memory_used(box) }

    /// The limit is the size the box's memory may grow to: this build of QuickJS cannot measure its own blocks.
    func setMemoryLimit(_ bytes: Int) { qjsbox_set_memory_limit(box, max(0, bytes)) }

    var memoryUsage: [String: Int] {
        var u = [Int64](repeating: 0, count: 11)
        qjsbox_memory_usage(box, &u)
        let keys = ["malloc", "used", "atoms", "strings", "objects", "properties", "shapes", "functions", "bytecode", "pc2line", "binary"]
        return Dictionary(uniqueKeysWithValues: zip(keys, u.map { Int($0) }))
    }

    func collectGarbage() { qjsbox_collect_garbage(box) }

    // MARK: host functions

    func registerFunction(_ name: String, _ handler: @escaping ([String]) -> HostReturn) {
        let magic = Int32(functions.count)
        functions[magic] = handler
        _ = qjsbox_register(box, name, Int32(QJSBOX_FN_STRINGS), magic, 2)
    }

    func registerWordsFunction(_ name: String, _ handler: @escaping (Double, UnsafeBufferPointer<Double>, String) -> Void) {
        let magic = Int32(wordFunctions.count)
        wordFunctions[magic] = handler
        _ = qjsbox_register(box, name, Int32(QJSBOX_FN_WORDS), magic, 3)
    }

    func registerOpsFunction(_ name: String, _ handler: @escaping (UnsafeBufferPointer<Int32>, UnsafeBufferPointer<UInt8>) -> Void) {
        let magic = Int32(opsFunctions.count)
        opsFunctions[magic] = handler
        _ = qjsbox_register(box, name, Int32(QJSBOX_FN_OPS), magic, 2)
    }

    fileprivate func dispatch(magic: Int32, args: [String]) -> HostReturn { functions[magic]?(args) ?? .undefined }

    fileprivate func dispatchWords(magic: Int32, number: Double, words: UnsafeBufferPointer<Double>, text: String) {
        wordFunctions[magic]?(number, words, text)
    }

    fileprivate func dispatchOps(magic: Int32, words: UnsafeBufferPointer<Int32>, strings: UnsafeBufferPointer<UInt8>) {
        opsFunctions[magic]?(words, strings)
    }

    // MARK: evaluating and calling

    private static let refused = "a call into the boxed engine was refused (it was made from inside one of its host functions)"

    private func text(_ r: qjsbox_result) -> String {
        guard Int(r.kind) == QJSBOX_RESULT_STRING, let p = r.str else { return "unknown error" }
        return String(decoding: UnsafeRawBufferPointer(start: p, count: r.len), as: UTF8.self)
    }

    /// The value of a call that ended with `status` (QuickJSContext.readResult), or its exception.
    private func take(_ status: Int32, _ r: qjsbox_result) throws -> JSResult {
        switch Int(status) {
        case QJSBOX_VALUE:
            if Int(r.kind) == QJSBOX_RESULT_STRING, let p = r.str {
                return .string(String(decoding: UnsafeRawBufferPointer(start: p, count: r.len), as: UTF8.self))
            }
            if Int(r.kind) == QJSBOX_RESULT_NUMBER { return .number(r.num) }
            return .undefined
        case QJSBOX_EXCEPTION:
            throw QuickJSError.exception(text(r))
        default:
            throw QuickJSError.engineFault(Self.refused)
        }
    }

    func evaluate(_ code: String, filename: String, asModule: Bool) throws -> JSResult {
        var code = code
        var result = qjsbox_result()
        let status = code.withUTF8 { b in
            b.withMemoryRebound(to: CChar.self) { qjsbox_eval(box, $0.baseAddress, $0.count, filename, asModule, &result) }
        }
        return try take(status, result)
    }

    func evaluateBytecode(_ blob: Data, source: String) throws -> JSResult {
        var source = source
        var result = qjsbox_result()
        let status = blob.withUnsafeBytes { b in
            source.withUTF8 { s in
                s.withMemoryRebound(to: CChar.self) {
                    qjsbox_eval_bytecode(box, b.bindMemory(to: UInt8.self).baseAddress, blob.count, $0.baseAddress, $0.count, &result)
                }
            }
        }
        return try take(status, result)
    }

    func compileBytecode(_ code: String, filename: String) throws -> Data {
        var code = code
        var result = qjsbox_result()
        let status = code.withUTF8 { b in
            b.withMemoryRebound(to: CChar.self) { qjsbox_compile(box, $0.baseAddress, $0.count, filename, &result) }
        }
        guard Int(status) == QJSBOX_VALUE, let p = result.str else {
            if Int(status) == QJSBOX_EXCEPTION { throw QuickJSError.exception(text(result)) }
            throw QuickJSError.engineFault(Self.refused)
        }
        return Data(bytes: p, count: result.len)
    }

    func captureGlobalFunction(_ name: String) -> Bool {
        let slot = qjsbox_capture(box, name)
        guard slot != 0 else { return false }
        if let old = captured[name] { qjsbox_release(box, old) }
        captured[name] = slot
        return true
    }

    func call(_ name: String, _ args: [String]) throws -> JSResult {
        guard args.count <= Int(QJSBOX_MAX_ARGS) else { throw QuickJSError.engineFault("more than \(QJSBOX_MAX_ARGS) arguments to \(name)") }
        // Every argument's UTF-8 in one buffer, so the pointers stay valid for the call.
        var bytes: [UInt8] = []
        var ranges: [(Int, Int)] = []
        for arg in args {
            let start = bytes.count
            bytes.append(contentsOf: arg.utf8)
            ranges.append((start, bytes.count - start))
        }
        var result = qjsbox_result()
        let status: Int32 = bytes.withUnsafeBufferPointer { buf in
            buf.withMemoryRebound(to: CChar.self) { chars in
                let pointers: [UnsafePointer<CChar>?] = ranges.map { range in chars.baseAddress.map { $0 + range.0 } }
                let lengths = ranges.map { $0.1 }
                return qjsbox_call(box, captured[name] ?? 0, name, Int32(args.count), pointers, lengths, &result)
            }
        }
        if Int(status) == QJSBOX_NOT_A_FUNCTION { throw QuickJSError.notAFunction(name) }
        return try take(status, result)
    }

    var hasPendingJob: Bool { qjsbox_job_pending(box) }

    func executePendingJobs(onError: (QuickJSError) -> Void) {
        for _ in 0..<100 {
            var result = qjsbox_result()
            let r = qjsbox_run_job(box, &result)
            if r < 0 { onError(.exception(text(result))); continue }
            if r == 0 { break }
        }
    }
}

// The three callbacks of the box (qjsbox_callbacks). They run on the engine's thread, inside a call into the box,
// and must not call into it: everything they need arrives as arguments.

/// What hostTrampoline builds for the native engine: strings up to their first NUL, numbers formatted, the rest empty.
private func boxedCall(_ opaque: UnsafeMutableRawPointer?, _ magic: Int32, _ argc: Int32, _ args: UnsafePointer<qjsbox_arg>?, _ ret: OpaquePointer?) {
    guard let opaque else { return }
    let engine = Unmanaged<BoxedQuickJS>.fromOpaque(opaque).takeUnretainedValue()
    var strings: [String] = []
    strings.reserveCapacity(Int(argc))
    for i in 0..<Int(argc) {
        guard let a = args?[i] else { break }
        if Int(a.kind) == QJSBOX_ARG_STRING, let p = a.str {
            let bytes = UnsafeRawBufferPointer(start: p, count: a.len)
            let end = bytes.firstIndex(of: 0) ?? bytes.count
            strings.append(String(decoding: UnsafeRawBufferPointer(rebasing: bytes[..<end]), as: UTF8.self))
        } else if Int(a.kind) == QJSBOX_ARG_NUMBER {
            let d = a.num
            strings.append(d == d.rounded() && abs(d) < 1e15 ? String(Int64(d)) : String(d))
        } else {
            strings.append("")
        }
    }
    switch engine.dispatch(magic: magic, args: strings) {
    case .undefined:
        break
    case .null:
        qjsbox_return_null(ret)
    case .number(let n):
        qjsbox_return_number(ret, n)
    case .string(var s):
        // The native engine takes it as a C string: up to the first NUL.
        s.withUTF8 { b in
            let end = b.firstIndex(of: 0) ?? b.count
            b.withMemoryRebound(to: CChar.self) { qjsbox_return_string(ret, $0.baseAddress, end) }
        }
    case .arrayBuffer(let data):
        data.withUnsafeBytes { qjsbox_return_bytes(ret, $0.bindMemory(to: UInt8.self).baseAddress, data.count) }
    }
}

private func boxedWords(_ opaque: UnsafeMutableRawPointer?, _ magic: Int32, _ number: Double, _ words: UnsafePointer<UInt8>?, _ bytes: Int,
                        _ text: UnsafePointer<CChar>?, _ textLength: Int) {
    guard let opaque, let words else { return }
    let engine = Unmanaged<BoxedQuickJS>.fromOpaque(opaque).takeUnretainedValue()
    let string = String(decoding: UnsafeRawBufferPointer(start: text, count: textLength), as: UTF8.self)
    let raw = UnsafeRawPointer(words)
    if Int(bitPattern: raw) % MemoryLayout<Double>.alignment == 0 {
        engine.dispatchWords(magic: magic, number: number, words: UnsafeBufferPointer(start: raw.assumingMemoryBound(to: Double.self), count: bytes / 8), text: string)
    } else {
        // A view at an odd offset of its buffer: the handler gets an aligned copy.
        let copy = [Double](unsafeUninitializedCapacity: bytes / 8) { buffer, count in
            memcpy(buffer.baseAddress, raw, bytes / 8 * 8)
            count = bytes / 8
        }
        copy.withUnsafeBufferPointer { engine.dispatchWords(magic: magic, number: number, words: $0, text: string) }
    }
}

private func boxedOps(_ opaque: UnsafeMutableRawPointer?, _ magic: Int32, _ words: UnsafePointer<UInt8>?, _ bytes: Int,
                      _ strings: UnsafePointer<CChar>?, _ stringsLength: Int) {
    guard let opaque, let words else { return }
    let engine = Unmanaged<BoxedQuickJS>.fromOpaque(opaque).takeUnretainedValue()
    let text = UnsafeBufferPointer(start: UnsafeRawPointer(strings)?.assumingMemoryBound(to: UInt8.self), count: strings == nil ? 0 : stringsLength)
    let raw = UnsafeRawPointer(words)
    if Int(bitPattern: raw) % MemoryLayout<Int32>.alignment == 0 {
        engine.dispatchOps(magic: magic, words: UnsafeBufferPointer(start: raw.assumingMemoryBound(to: Int32.self), count: bytes / 4), strings: text)
    } else {
        let copy = [Int32](unsafeUninitializedCapacity: bytes / 4) { buffer, count in
            memcpy(buffer.baseAddress, raw, bytes / 4 * 4)
            count = bytes / 4
        }
        copy.withUnsafeBufferPointer { engine.dispatchOps(magic: magic, words: $0, strings: text) }
    }
}
#endif
