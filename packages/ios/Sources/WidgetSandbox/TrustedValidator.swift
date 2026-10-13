// The validator / sanitizer (src/host/validator.ts) compiled into
// validator-bundle.js and run in a TRUSTED QuickJS context — a separate
// runtime that never executes widget code. One instance per widget. Calls are
// synchronous from the caller's point of view (WidgetHost on the main thread)
// but execute on the validator's JSThread, whose 8 MB stack is what the
// css-tree parser needs in a debug build. Input and output are JSON strings.
//
// The thread can be the validator's own (the default) or one shared by every
// validator in the process (`TrustedValidator.sharedThread`): validator calls
// are short and never block, so one thread serves a whole chat of widgets,
// and a host that validates asynchronously (`perform`) keeps the css-tree
// work off the main thread without a thread per bubble.

import Foundation
import WidgetSandboxProtocol

public final class TrustedValidator {
    /// One thread for every validator that asks for it (see the header).
    public static let sharedThread = JSThread(name: "widget.validator")

    public let thread: JSThread
    private let ownsThread: Bool
    private var js: QuickJSContext!
    public var onDiag: (String, String) -> Void
    /// Set once the bundle is evaluated: from the precompiled blob or from source.
    public private(set) var bundleSource = "pending"

    /// Builds the context and evaluates the bundle before returning (the caller's thread waits for the validator's).
    public convenience init(theme: String, resources: SandboxResources = .shared, thread: JSThread? = nil, onDiag: @escaping (String, String) -> Void) throws {
        self.init(thread: thread, onDiag: onDiag)
        try self.thread.sync { Result { try self.build(theme: theme, resources: resources) } }.get()
    }

    /// A validator whose context is built on its thread without blocking the
    /// caller (a pool refilling while the main thread scrolls). Every later call
    /// queues behind the build; a failed build is reported as `validator-error`.
    public static func background(theme: String, resources: SandboxResources = .shared, thread: JSThread? = nil, onDiag: @escaping (String, String) -> Void) -> TrustedValidator {
        let v = TrustedValidator(thread: thread, onDiag: onDiag)
        v.thread.async { [weak v] in
            guard let v else { return }
            do { try v.build(theme: theme, resources: resources) } catch { v.onDiag("validator-error", "build: \(error)") }
        }
        return v
    }

    private init(thread: JSThread?, onDiag: @escaping (String, String) -> Void) {
        self.onDiag = onDiag
        self.thread = thread ?? JSThread(name: "widget.validator")
        ownsThread = thread == nil
    }

    /// On the validator's thread.
    private func build(theme: String, resources: SandboxResources) throws {
        let js = QuickJSContext(memoryLimitBytes: 256 << 20, maxStackBytes: JSThread.jsStackLimit)
        self.js = js
        var fromBytecode = false
        js.registerFunction("__diag") { [weak self] args in
            self?.onDiag(args.first ?? "", args.count > 1 ? args[1] : "")
            return .undefined
        }
        // Precompiled when shipped and matching the source, else the source (same rule as the engine).
        if let bytecode = resources.validatorBytecode {
            do { try js.evaluateBytecode(bytecode, source: resources.validatorBundle); fromBytecode = true }
            catch { onDiag("bytecode", "validator-bundle.js: falling back to source — \("\(error)".split(separator: "\n").first ?? "")") }
        }
        if !fromBytecode { try js.evaluate(resources.validatorBundle, filename: "validator-bundle.js") }
        // Flatten the API into globals so QuickJSContext.call can reach it.
        try js.evaluate("""
            globalThis.__v_processOps = (j) => __twValidator.processOps(j);
            globalThis.__v_validateQuery = (j) => __twValidator.validateQuery(j);
            globalThis.__v_validateListen = (j) => __twValidator.validateListen(j);
            globalThis.__v_validatePreventDefault = (j) => __twValidator.validatePreventDefault(j);
            globalThis.__v_setTheme = (t) => __twValidator.setTheme(t);
            globalThis.__v_rewriteMediaQuery = (q) => __twValidator.rewriteMediaQuery(q);
            globalThis.__v_nodeCount = () => __twValidator.nodeCount();
            globalThis.__v_isCanvas = (id) => __twValidator.isCanvas(Number(id)) ? 1 : 0;
            globalThis.__v_create = (t) => { __twValidator.create(t); return ''; };
            """, filename: "validator-glue.js")
        try js.call("__v_create", [theme])
        bundleSource = fromBytecode ? "bytecode" : "source"
    }

    deinit { if ownsThread { thread.stop() } }

    /// A fresh validator instance in the same context (the bundle stays
    /// evaluated): how a pool recycles a validator for the next widget.
    public func reset(theme: String) {
        _ = str("__v_create", [theme])
    }

    /// Runs `block` on the validator's thread, after everything queued before
    /// it; the validator's methods are inline there. Hosts use it to validate
    /// off the main thread while keeping the order of ops and queries.
    public func perform(_ block: @escaping () -> Void) {
        thread.async(block)
    }

    private func str(_ name: String, _ args: [String]) -> String {
        thread.sync { [self] in
            guard let js else { return "" } // the background build failed; already reported
            do {
                return try js.call(name, args).string ?? ""
            } catch {
                self.onDiag("validator-error", "\(name): \(error)")
                return ""
            }
        }
    }

    /// Untrusted ops batch (JSON) → validated RenderOps (JSON array; "[]" if nothing survived).
    public func processOps(_ json: String) -> String {
        let out = str("__v_processOps", [json])
        return out.isEmpty ? "[]" : out
    }

    /// What a renderer gets for one ops batch: nothing, the JSON array, or typed ops for a renderer that takes them.
    public enum RenderBatch {
        case empty
        case json(String)
        case ops([WidgetOp])
    }

    /// Batches of canvas word payloads validated without JSON (tests and stats).
    public private(set) var nativeCanvasBatches = 0

    /// Validates a batch for a renderer. With `typed`, the output is parsed into `WidgetOp`s here, on the
    /// validator's thread, so the main thread never parses JSON. A batch made only of canvas placeholders whose
    /// word payloads pass `CanvasWords.envelope` (the same rules as the JS validator, stricter where they differ)
    /// on known canvas elements goes to the renderer as typed canvas ops without any JSON; anything else is
    /// spliced back into JSON and validated by the JS validator as before.
    public func processOpsForRenderer(_ json: String, canvas: [Int: WidgetCanvasBatch] = [:], typed: Bool) -> RenderBatch {
        thread.sync { [self] in
            guard let js else { return .empty }
            if typed, !canvas.isEmpty, let list = CanvasWords.only(json, canvas),
               list.allSatisfy({ ((try? js.call("__v_isCanvas", [String($0.id)]))?.number ?? 0) == 1 && CanvasWords.envelope($0.batch) }) {
                nativeCanvasBatches += 1
                return .ops(list.map { WidgetOp(canvas: $0.id, batch: $0.batch) })
            }
            let out = processOps(canvas.isEmpty ? json : CanvasWords.splice(json, canvas))
            if out == "[]" { return .empty }
            return typed ? Self.typedOps(out) : .json(out)
        }
    }

    static func typedOps(_ json: String) -> RenderBatch {
        guard let list = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [Any] else { return .json(json) }
        var ops: [WidgetOp] = []
        ops.reserveCapacity(list.count)
        for item in list {
            guard let fields = item as? [String: Any] else { return .json(json) }
            ops.append(WidgetOp(fields))
        }
        return ops.isEmpty ? .empty : .ops(ops)
    }

    /// Untrusted query → clean query JSON, or nil if rejected.
    public func validateQuery(_ json: String) -> String? {
        let out = str("__v_validateQuery", [json])
        return out.isEmpty ? nil : out
    }

    public func validateListen(_ json: String) -> String { str("__v_validateListen", [json]).nilIfEmpty ?? "[]" }
    public func validatePreventDefault(_ json: String) -> String { str("__v_validatePreventDefault", [json]).nilIfEmpty ?? "[]" }
    public func setTheme(_ theme: String) -> String { str("__v_setTheme", [theme]).nilIfEmpty ?? "[]" }
    public func rewriteMediaQuery(_ query: String) -> String { str("__v_rewriteMediaQuery", [query]) }
    public var nodeCount: Int { thread.sync { [self] in Int((try? js?.call("__v_nodeCount"))?.number ?? 0) } }
}

private extension String {
    var nilIfEmpty: String? { isEmpty ? nil : self }
}
