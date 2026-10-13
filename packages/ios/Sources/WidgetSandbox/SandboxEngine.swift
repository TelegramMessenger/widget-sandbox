// The sandbox engine: the native counterpart of src/host/worker.ts. Owns one
// QuickJS runtime per widget on a private serial queue, exposes exactly one
// capability to the untrusted code (`__host(kind, arg)` + `__hostBinary()`),
// enforces CPU and memory limits, and drives timers / frames / scripts.
//
// Everything that comes out of the context is UNTRUSTED: the delegate must
// pass it through the validator before it reaches the renderer.

import Foundation
import WidgetSandboxProtocol
import QuartzCore

/// Mirrors `SandboxConfig` / `LIMIT_DEFAULTS` in src/shared/protocol.ts.
public struct SandboxConfig {
    public var memoryLimitBytes = 64 << 20
    /// Longest continuous run of widget JS in one task before the runtime is killed.
    public var cpuLimitMs = 500.0
    /// JS time the widget may use per `cpuQuotaWindowMs` (sliding window); beyond it timers and frames are deferred, never killed.
    public var cpuQuotaMs = 300.0
    public var cpuQuotaWindowMs = 1000.0
    /// Kill after this long of uninterrupted throttling; 0 disables (default).
    public var cpuKillAfterMs = 0.0
    /// Live timers a widget may hold; the shim throws RangeError beyond it, the engine refuses as a backstop.
    public var maxTimers = 256
    /// Initial visibility (see `setVisible`): hidden widgets get no frames and timers at 1 Hz.
    public var visible = true
    public var innerWidth: Double
    public var innerHeight: Double
    public var devicePixelRatio: Double
    public var theme: String
    /// Optional wire forms the host understands (docs/protocol.md, "Features"); empty means plain JSON.
    /// `canvasWords`: canvas commands arrive as word payloads and are spliced back into the ops JSON.
    public var features: Set<String> = []

    public init(innerWidth: Double, innerHeight: Double, devicePixelRatio: Double, theme: String) {
        self.innerWidth = innerWidth
        self.innerHeight = innerHeight
        self.devicePixelRatio = devicePixelRatio
        self.theme = theme
    }
}

public struct SandboxStats {
    public var jsMs = 0.0, tasks = 0, memoryBytes = 0, ops = 0, queries = 0, queryMs = 0.0, queryCacheHits = 0, missedMutations = 0
    /// Tasks deferred by the CPU quota; JS time used in the current quota window at the time of the report.
    public var throttledTasks = 0, cpuWindowMs = 0.0
}

/// All delegate calls happen on the engine's queue, never on the main thread.
/// `query` is synchronous: it may block the engine queue (and must not block the main thread).
public protocol SandboxEngineDelegate: AnyObject {
    func engine(_ engine: SandboxEngine, ops json: String)
    func engine(_ engine: SandboxEngine, query json: String) -> (json: String, binary: Data?)
    func engine(_ engine: SandboxEngine, listen json: String)
    func engine(_ engine: SandboxEngine, preventDefault json: String)
    func engineRequestsFrame(_ engine: SandboxEngine)
    func engine(_ engine: SandboxEngine, log level: String, text: String)
    func engine(_ engine: SandboxEngine, diag kind: String, detail: String)
    func engine(_ engine: SandboxEngine, bridge method: String, argsJson: String)
    func engineLoaded(_ engine: SandboxEngine)
    func engine(_ engine: SandboxEngine, stats: SandboxStats)
    func engine(_ engine: SandboxEngine, died reason: String)
}

public final class SandboxEngine {
    /// Everything below runs on this thread (see JSThread for why not a DispatchQueue).
    public let queue = JSThread(name: "widget.sandbox")
    private let delegateLock = NSLock()
    private weak var _delegate: SandboxEngineDelegate?
    /// Set on the owner's thread, read on the JS thread.
    public var delegate: SandboxEngineDelegate? {
        get {
            delegateLock.lock()
            defer { delegateLock.unlock() }
            return _delegate
        }
        set {
            delegateLock.lock()
            _delegate = newValue
            delegateLock.unlock()
        }
    }

    private var html = ""
    public private(set) var config: SandboxConfig
    private let resources: SandboxResources
    private var js: QuickJSContext!
    /// Set by `prewarm()`: the runtime exists and the sandbox bundle has been evaluated, no widget yet.
    private var prewarmed = false
    private var prewarmRequested = false
    private var started = false
    private var bundleEval: (ms: Double, how: String)?
    private var dead = false
    private var stats = SandboxStats()
    private var timers: [Int: JSThread.Scheduled] = [:]
    private var lastBinary: Data?
    private var lastStatsPost = 0.0
    private var lastMemCheck = 0.0
    private var timerLimitReported = false
    /// Timer delays are clamped to 24 h on every platform (MAX_TIMER_DELAY_MS in protocol.ts).
    private let maxTimerDelay: TimeInterval = 86_400

    // Visibility and the deferred-task queue (mirrors src/host/worker.ts).
    //
    // Hidden widget (`setVisible(false)`): no frames are requested from the
    // renderer (one is requested on return if the shim asked), timers fire at
    // most once per second, batched into one wake-up, and the shim gets
    // `visibilitychange`. Deferred tasks keep their order; one `fire` per timer
    // id, one frame at most.
    private var visible: Bool
    private var frameWanted = false
    private struct Deferred { let kind: String; var arg: String }
    private var deferred: [Deferred] = []
    private var wakeTimer: JSThread.Scheduled?
    private var wakeAt = Double.infinity
    /// Next 1 Hz boundary at which hidden timers fire (0 = none scheduled); CACurrentMediaTime seconds.
    private var hiddenTickAt = 0.0

    // CPU quota (mirrors worker.ts): decide before a task, record after it.
    private var budget: CpuBudget
    /// Slice given to the running task in ms (for the kill message); the boot task gets cpuLimitMs.
    private var currentSliceMs = 0.0
    private var currentKind = "boot"
    /// JS time of the current task that is ours, not the widget's (evaluating the sandbox bundle), ms.
    private var excludeMs = 0.0
    private var lastThrottleDiag = -Double.infinity
    private var throttledSinceDiag: [String: Int] = [:]
    private static func nowMs() -> Double { CACurrentMediaTime() * 1000 }
    /// Inputs that arrived before the runtime existed (the renderer reports its
    /// size before it announces `ready`); replayed after boot, like the web worker does.
    private var pendingBeforeBoot: [(String, String)] = []
    /// The user gesture whose event task is running, 0 outside one; a gesture unlocks one privileged bridge call, made in its own tasks.
    private var activeGesture = 0
    private var consumedGesture = 0
    private var blockedBridgeReported = 0
    /// Bridge methods a widget may call without a user gesture.
    static let unprivilegedBridgeMethods: Set<String> = ["ready"]
    private static let activationPrefix = "activation:"

    public init(html: String, config: SandboxConfig, resources: SandboxResources = .shared) {
        self.html = html
        self.config = config
        self.resources = resources
        visible = config.visible
        budget = CpuBudget(config: config)
    }

    /// An engine without a widget yet: `prewarm()` builds the runtime and
    /// evaluates the sandbox bundle ahead of time, `start(html:config:)` boots
    /// the widget later (a pool of such engines takes the bundle's evaluation
    /// off the critical path of a bubble that scrolls into view).
    public init(resources: SandboxResources = .shared) {
        self.resources = resources
        config = SandboxConfig(innerWidth: 400, innerHeight: 300, devicePixelRatio: 2, theme: "light")
        visible = true
        budget = CpuBudget(config: config)
    }

    deinit { queue.stop() }

    // MARK: lifecycle

    /// Runtime + sandbox bundle now, widget later. Idempotent; a no-op once started.
    public func prewarm() {
        if prewarmRequested { return }
        prewarmRequested = true
        queue.async {
            guard !self.started && !self.dead else { return }
            // A prewarm that hits a limit (a busy machine) must not leave a dead
            // engine in the pool: drop the runtime and let start() build a fresh one.
            self.prewarming = true
            self.createRuntime()
            self.prewarming = false
            if self.prewarmFailed {
                self.prewarmFailed = false
                self.js = nil
                self.prewarmed = false
                self.bundleEval = nil
            }
        }
    }

    static let hostGlobals = ["__host", "__hostBinary", "__hostCanvas", "__twDispatch"]
    static let hideHostGlobals = "(function(g){" + hostGlobals.map { "if(!delete g.\($0))throw new Error('\($0)');" }.joined() + "})(globalThis);"

    /// Canvas word payloads (`__hostCanvas`) for the next ops batch, by key.
    private var stagedCanvas: [Int: WidgetCanvasBatch] = [:]
    private var stagedWords = 0
    /// Word payloads accepted so far (tests).
    private(set) var canvasWordPayloads = 0

    private var prewarming = false
    private var prewarmFailed = false

    public var isPrewarmed: Bool { queue.sync { self.prewarmed } }

    public func start() {
        queue.async { self.boot() }
    }

    /// Boots `html` with `config` (the limits, viewport and theme of the host that adopted a prewarmed engine).
    public func start(html: String, config: SandboxConfig) {
        queue.async {
            self.html = html
            self.config = config
            self.visible = config.visible
            self.budget = CpuBudget(config: config)
            self.boot()
        }
    }

    public func terminate() {
        queue.async { self.kill("terminated") }
    }

    /// The runtime, the two host functions and the sandbox bundle (ours, so its time is excluded from the widget's quota).
    private func createRuntime() {
        if js != nil { return }
        js = QuickJSContext(memoryLimitBytes: config.memoryLimitBytes, maxStackBytes: JSThread.jsStackLimit)
        js.registerFunction("__host") { [unowned self] args in self.hostCall(kind: args.first ?? "", arg: args.count > 1 ? args[1] : "") }
        if config.features.contains("canvasWords") {
            js.registerWordsFunction("__hostCanvas") { [unowned self] key, words, strings in
                guard key >= 0, key < Double(CanvasWords.maxPayloads), key == key.rounded(), stagedCanvas.count < CanvasWords.maxPayloads,
                      stagedWords + words.count <= CanvasWords.maxWords, let batch = CanvasWords.batch(words: words, strings: strings) else { return }
                stagedWords += words.count
                stagedCanvas[Int(key)] = batch
                canvasWordPayloads += 1
            }
        }
        js.registerFunction("__hostBinary") { [unowned self] _ in
            guard let data = self.lastBinary else { return .null }
            self.lastBinary = nil
            return .arrayBuffer(data)
        }
        let t0 = CACurrentMediaTime()
        // Generous one-off budget for our own bundle; its time is ours, not the widget's.
        js.deadline = ProcessInfo.processInfo.systemUptime + max(config.cpuLimitMs, 5000) / 1000
        let how = evalBundle(resources.sandboxBundle, bytecode: resources.sandboxBytecode, filename: "sandbox-bundle.js")
        js.deadline = .infinity
        js.cpuDeadline = .infinity
        // Fail closed: a runtime whose shim did not load completely (its network
        // stubs and DOM capture may be missing) never runs widget code.
        if how == "failed" {
            if !dead && !prewarmFailed { kill("sandbox bundle failed to load") }
            return
        }
        // The bundle holds private references to the host functions; widget code
        // must not reach them, nor the shim's dispatcher, through the global object.
        guard js.captureGlobalFunction("__twDispatch"), (try? js.evaluate(Self.hideHostGlobals, filename: "hide-host.js")) != nil else {
            if !dead && !prewarmFailed { kill("sandbox bundle failed to load") }
            return
        }
        bundleEval = ((CACurrentMediaTime() - t0) * 1000, how)
        prewarmed = true
    }

    private func boot() {
        if started || dead { return }
        started = true
        let wasPrewarmed = js != nil
        createRuntime()
        if dead { return }
        js.setMemoryLimit(config.memoryLimitBytes)
        if let bundleEval {
            delegate?.engine(self, diag: "boot", detail: String(format: "sandbox bundle eval %.0f ms (%@%@)", bundleEval.ms, bundleEval.how, wasPrewarmed ? ", prewarmed" : ""))
        }
        task("boot", sliceMs: config.cpuLimitMs) {
            let bootArg = Self.json([
                "html": self.html,
                "config": [
                    "memoryLimitBytes": self.config.memoryLimitBytes, "cpuLimitMs": self.config.cpuLimitMs,
                    "cpuQuotaMs": self.config.cpuQuotaMs, "cpuQuotaWindowMs": self.config.cpuQuotaWindowMs, "cpuKillAfterMs": self.config.cpuKillAfterMs,
                    "maxTimers": self.config.maxTimers, "visible": self.visible,
                    "viewport": ["innerWidth": self.config.innerWidth, "innerHeight": self.config.innerHeight, "dpr": self.config.devicePixelRatio],
                    "theme": self.config.theme,
                    "features": Array(self.config.features).sorted(),
                ],
            ])
            self.callTw("boot", bootArg)
        }
        task("boot", sliceMs: config.cpuLimitMs) { self.callTw("loaded", "") }
        if !dead { delegate?.engineLoaded(self) }
        postStats(force: true)
        let queued = pendingBeforeBoot
        pendingBeforeBoot.removeAll()
        for (kind, arg) in queued where !dead { dispatch(kind, arg) }
    }

    // MARK: inputs from the host (all asynchronous)

    public func event(json: String) { enqueue("event", json) }
    /// A real user input event from the renderer (press, click, key press); every event of one physical gesture carries the same increasing `gesture` number.
    public func activationEvent(json: String, gesture: Int) { enqueue(Self.activationPrefix + String(gesture), json) }
    public func frame(time: Double) { enqueue("frame", String(Self.coarseNowMs(time))) }

    /// Widget clocks tick in 100 µs steps, like a browser's `performance.now()`, so they cannot time cache or pool effects of other widgets.
    static let clockResolutionMs = 0.1

    static func coarseNowMs(_ ms: Double) -> Double {
        (ms / clockResolutionMs).rounded(.down) * clockResolutionMs
    }
    public func viewport(innerWidth: Double, innerHeight: Double, dpr: Double) {
        enqueue("viewport", Self.json(["innerWidth": innerWidth, "innerHeight": innerHeight, "dpr": dpr]))
    }
    public func resize(entriesJson: String) { enqueue("resize", entriesJson) }
    public func theme(_ theme: String) { enqueue("theme", Self.json(theme)) }
    public func bridgeEvent(_ event: String, dataJson: String) { enqueue("bridge", "[\(Self.json(event)),\(dataJson)]") }

    /// The host knows whether the widget is on screen; the sandbox does not.
    public func setVisible(_ v: Bool) {
        queue.async {
            if self.dead || self.visible == v { return }
            self.visible = v
            guard self.js != nil else { return } // boot reads `visible` for the shim
            self.dispatch("visibility", "{\"visible\":\(v)}")
            if v {
                if self.frameWanted { self.frameWanted = false; self.delegate?.engineRequestsFrame(self) }
                if !self.deferred.isEmpty { self.wake() }
            }
        }
    }

    private func enqueue(_ kind: String, _ arg: String) {
        queue.async {
            if self.dead { return }
            guard self.js != nil else { self.pendingBeforeBoot.append((kind, arg)); return }
            // A frame that arrives after the widget was hidden is dropped; the shim's callbacks wait for the next visible frame.
            if kind == "frame", !self.visible { self.frameWanted = true; return }
            self.dispatch(kind, arg)
        }
    }

    /// Runs one task for an input (event, timer, frame, …) if the CPU quota
    /// allows, otherwise defers it (returns false). Events and visibility always run.
    @discardableResult
    private func dispatch(_ kind: String, _ arg: String) -> Bool {
        if dead { return true }
        if kind.hasPrefix(Self.activationPrefix) {
            let gesture = Int(kind.dropFirst(Self.activationPrefix.count)) ?? 0
            activeGesture = visible && gesture > consumedGesture ? gesture : 0
            defer { activeGesture = 0 }
            return dispatch("event", arg)
        }
        let now = Self.nowMs()
        switch budget.decide(kind, now: now) {
        case .defer_(let retryAt):
            if budget.shouldKill(now) {
                kill(String(format: "CPU quota: throttled for %.1f s (%d ms per %d ms, cpuKillAfterMs=%d)", budget.throttledFor(now) / 1000, Int(config.cpuQuotaMs), Int(config.cpuQuotaWindowMs), Int(config.cpuKillAfterMs)))
                return true
            }
            defer_(kind, arg)
            stats.throttledTasks += 1
            throttledSinceDiag[kind, default: 0] += 1
            if now - lastThrottleDiag >= 1000 {
                lastThrottleDiag = now
                let parts = throttledSinceDiag.sorted { $0.key < $1.key }.map { "\($0.value) \($0.key)\($0.value == 1 ? "" : "s")" }
                throttledSinceDiag.removeAll()
                delegate?.engine(self, diag: "cpu-throttled", detail: "deferred \(parts.joined(separator: ", ")): \(Int(budget.used(now))) ms of the \(Int(config.cpuQuotaMs)) ms per \(Int(config.cpuQuotaWindowMs)) ms budget used")
            }
            scheduleWake(at: retryAt / 1000)
            return false
        case .run(let sliceMs):
            task(kind, sliceMs: sliceMs) { self.callTw(kind, arg) }
            return true
        }
    }

    private func defer_(_ kind: String, _ arg: String) {
        if kind == "frame", let i = deferred.firstIndex(where: { $0.kind == "frame" }) { deferred[i].arg = arg; return }
        if kind == "timer", deferred.contains(where: { $0.kind == "timer" && $0.arg == arg }) { return } // intervals do not accumulate missed ticks
        deferred.append(Deferred(kind: kind, arg: arg))
    }

    private func scheduleWake(at: Double) {
        if wakeTimer != nil, at >= wakeAt { return }
        wakeTimer?.cancel()
        wakeAt = at
        wakeTimer = queue.after(max(0, at - CACurrentMediaTime())) { [weak self] in self?.wake() }
    }

    /// Runs deferred tasks in order; stops (and re-queues the rest) if one is deferred again.
    private func wake() {
        wakeTimer = nil
        wakeAt = .infinity
        hiddenTickAt = 0
        let items = deferred
        deferred.removeAll()
        for (i, item) in items.enumerated() {
            if dead { return }
            if !dispatch(item.kind, item.arg) {
                deferred.insert(contentsOf: items[i...], at: 0)
                return
            }
        }
    }

    private func onTimerFired(_ id: Int) {
        timers.removeValue(forKey: id)
        if !visible {
            defer_("timer", String(id))
            if hiddenTickAt == 0 { hiddenTickAt = CACurrentMediaTime() + 1 }
            scheduleWake(at: hiddenTickAt)
            return
        }
        dispatch("timer", String(id))
    }

    // MARK: the single capability

    private func hostCall(kind: String, arg: String) -> HostReturn {
        switch kind {
        case "ops":
            stats.ops += Self.countOccurrences(of: "\"op\":", in: arg)
            let canvas = stagedCanvas
            stagedCanvas = [:]
            stagedWords = 0
            delegate?.engine(self, ops: canvas.isEmpty ? arg : CanvasWords.splice(arg, canvas))
            return .undefined
        case "query":
            return .string(syncQuery(arg))
        case "now":
            return .number(Self.coarseNowMs(CACurrentMediaTime() * 1000))
        case "timer":
            guard let pair = Self.parse(arg) as? [Any], pair.count == 2, let id = (pair[0] as? NSNumber)?.intValue else { return .undefined }
            var delay = min(max(0, (pair[1] as? NSNumber)?.doubleValue ?? 0) / 1000, maxTimerDelay)
            if !visible { delay = max(delay, 1) } // hidden: 1 Hz, like a background tab
            guard timers.count < config.maxTimers else {
                // Backstop: the shim should have thrown RangeError before asking. Reaching this means a shim bug (or a tampered shim).
                if !timerLimitReported {
                    timerLimitReported = true
                    delegate?.engine(self, diag: "timer-limit-enforced", detail: "engine refused a timer beyond maxTimers=\(config.maxTimers); the shim did not stop it")
                }
                return .undefined
            }
            timers[id] = queue.after(delay) { [weak self] in
                guard let self, !self.dead else { return }
                self.onTimerFired(id)
            }
            return .undefined
        case "clearTimer":
            if let id = Int(arg), let item = timers.removeValue(forKey: id) { item.cancel() }
            return .undefined
        case "raf":
            if !visible { frameWanted = true; return .undefined } // one frame is requested when the widget becomes visible again
            delegate?.engineRequestsFrame(self)
            return .undefined
        case "log":
            let (level, text) = Self.splitPrefix(arg)
            delegate?.engine(self, log: level, text: String(text.prefix(4000)))
            return .undefined
        case "diag":
            let (k, detail) = Self.splitPrefix(arg)
            delegate?.engine(self, diag: String(k.prefix(64)), detail: String(detail.prefix(4000)))
            return .undefined
        case "listen":
            delegate?.engine(self, listen: arg)
            return .undefined
        case "pd":
            delegate?.engine(self, preventDefault: arg)
            return .undefined
        case "bridge":
            if let pair = Self.parse(arg) as? [Any], pair.count == 2, let method = pair[0] as? String,
               let argsData = try? JSONSerialization.data(withJSONObject: pair[1], options: [.fragmentsAllowed]) {
                let name = String(method.prefix(64))
                if !Self.unprivilegedBridgeMethods.contains(name) {
                    guard activeGesture > consumedGesture, visible else {
                        if blockedBridgeReported < 20 {
                            blockedBridgeReported += 1
                            delegate?.engine(self, diag: "activation-blocked", detail: "\(name) ignored: it needs a user click or key press on the visible widget and runs in that event's handler (called from the \(currentKind) task)")
                        }
                        return .undefined
                    }
                    consumedGesture = activeGesture
                    activeGesture = 0
                }
                delegate?.engine(self, bridge: name, argsJson: String(decoding: argsData, as: UTF8.self))
            }
            return .undefined
        case "intl":
            return .string(IntlBridge.format(requestJson: arg))
        case "random":
            let n = min(max(Int(arg) ?? 0, 0), 65536)
            var bytes = [UInt8](repeating: 0, count: n)
            _ = bytes.withUnsafeMutableBytes { SecRandomCopyBytes(kSecRandomDefault, n, $0.baseAddress!) }
            return .string("[" + bytes.map(String.init).joined(separator: ",") + "]")
        case "stat":
            if let s = Self.parse(arg) as? [String: Any] {
                stats.queryCacheHits = (s["cacheHits"] as? NSNumber)?.intValue ?? 0
                stats.missedMutations = (s["missed"] as? NSNumber)?.intValue ?? 0
            }
            return .undefined
        default:
            return .undefined
        }
    }

    /// Blocks the engine queue until the host answers (the host must never wait for the engine).
    private func syncQuery(_ json: String) -> String {
        guard let delegate else { return "{\"error\":\"no host\"}" }
        let t0 = CACurrentMediaTime()
        let (answer, binary) = delegate.engine(self, query: json)
        let dt = CACurrentMediaTime() - t0
        js.deadline += dt // time spent waiting is not widget CPU time
        stats.queries += 1
        stats.queryMs += dt * 1000
        lastBinary = binary
        return answer
    }

    // MARK: running code

    private func kill(_ reason: String) {
        if prewarming {
            prewarmFailed = true
            return
        }
        if dead { return }
        dead = true
        pendingBeforeBoot.removeAll()
        for (_, t) in timers { t.cancel() }
        timers.removeAll()
        deferred.removeAll()
        wakeTimer?.cancel()
        wakeTimer = nil
        delegate?.engine(self, died: reason)
    }

    private func handleError(_ error: Error, where location: String) {
        let text = (error as? QuickJSError)?.description ?? String(describing: error)
        if js?.interrupted == true {
            // The slice is cpuLimitMs unless the CPU quota shrank it; say so, the fix differs (infinite loop vs. a widget that was already over budget).
            let quota = currentSliceMs < config.cpuLimitMs ? "; CPU quota: \(Int(max(0, config.cpuQuotaMs - budget.used(Self.nowMs())))) ms of \(Int(config.cpuQuotaMs)) ms per \(Int(config.cpuQuotaWindowMs)) ms left, so this \(currentKind) task got \(Int(currentSliceMs)) ms" : ""
            return kill("CPU limit exceeded: more than \(Int(currentSliceMs)) ms of continuous execution (\(location)\(quota))")
        }
        if text.lowercased().contains("out of memory") { return kill("Memory limit exceeded (\(config.memoryLimitBytes >> 20) MB)") }
        delegate?.engine(self, log: "error", text: String(("Uncaught " + text).prefix(4000)))
    }

    private func drainJobs() {
        js.executePendingJobs { self.handleError($0, where: "promise job") }
    }

    @discardableResult
    private func callTw(_ kind: String, _ arg: String) -> String? {
        if dead { return nil }
        do {
            return try js.call("__twDispatch", [kind, arg]).string
        } catch {
            handleError(error, where: kind)
            return nil
        }
    }

    private func settle() {
        for _ in 0..<20 where !dead {
            drainJobs()
            if dead { return }
            let more = callTw("endTick", "")
            if more != "1" && !js.hasPendingJob { break }
        }
    }

    /// One of our own bundles: the precompiled `.qbc` when shipped and matching the source,
    /// else the source. Returns "bytecode" or "source" for the diag.
    private func evalBundle(_ source: String, bytecode: Data?, filename: String) -> String {
        if let bytecode {
            do {
                try js.evaluateBytecode(bytecode, source: source)
                return "bytecode"
            } catch {
                delegate?.engine(self, diag: "bytecode", detail: "\(filename): falling back to source — \("\(error)".split(separator: "\n").first ?? "")")
            }
        }
        do {
            try js.evaluate(source, filename: filename)
        } catch {
            handleError(error, where: filename)
            return "failed"
        }
        return "source"
    }

    private func evalScript(_ code: String, filename: String, asModule: Bool = false) {
        do { try js.evaluate(code, filename: filename, asModule: asModule) } catch { handleError(error, where: filename) }
    }

    private func pumpScripts() {
        for _ in 0..<1000 where !dead {
            guard let next = callTw("nextScript", ""), !next.isEmpty, let s = Self.parse(next) as? [String: Any] else { break }
            let id = (s["id"] as? NSNumber)?.intValue ?? 0
            var ok = true
            resetDeadline()
            if let src = s["src"] as? String {
                if let lib = resources.library(for: src), let source = resources.librarySource(lib) {
                    let t0 = CACurrentMediaTime()
                    let how = evalBundle(source, bytecode: resources.libraryBytecode(lib), filename: lib.file)
                    delegate?.engine(self, diag: "library", detail: "\(src) → bundled \(lib.name) (eval \(Int((CACurrentMediaTime() - t0) * 1000)) ms, \(how))")
                } else {
                    ok = false
                    delegate?.engine(self, diag: "script-skipped", detail: "external script not available: \(src.prefix(200))")
                }
            } else if let code = s["code"] as? String {
                evalScript(code, filename: "inline-script-\(id).js", asModule: (s["type"] as? String) == "module")
            }
            if dead { return }
            settle()
            callTw("scriptDone", "[\(id),\(ok)]")
            settle()
        }
    }

    /// The slice is measured in the JS thread's CPU time, so a busy machine does not
    /// kill widgets; wall-clock time is only a backstop for a thread that never runs.
    private func resetDeadline() {
        js.interrupted = false
        js.cpuDeadline = QuickJSContext.threadCPUTime() + currentSliceMs / 1000
        js.deadline = ProcessInfo.processInfo.systemUptime + max(currentSliceMs * Self.wallBackstopFactor, Self.wallBackstopMinMs) / 1000
    }

    static let wallBackstopFactor = 10.0
    static let wallBackstopMinMs = 5000.0

    /// One task of `kind` with `sliceMs` of continuous execution; its JS time is recorded against the quota.
    private func task(_ kind: String, sliceMs: Double, _ body: () -> Void) {
        if dead { return }
        currentKind = kind
        currentSliceMs = sliceMs
        excludeMs = 0
        let c0 = QuickJSContext.threadCPUTime()
        resetDeadline()
        body()
        if !dead { settle() }
        if !dead { pumpScripts() }
        stats.tasks += 1
        let cpuMs = (QuickJSContext.threadCPUTime() - c0) * 1000 - excludeMs
        stats.jsMs += cpuMs
        budget.record(kind, cpuMs: cpuMs, now: Self.nowMs())
        stats.cpuWindowMs = budget.used(Self.nowMs())
        let now = CACurrentMediaTime()
        if now - lastMemCheck > 1, !dead {
            lastMemCheck = now
            stats.memoryBytes = js.memoryUsed
        }
        postStats(force: false)
    }

    private var trailingStats: JSThread.Scheduled?

    /// At most one stats message per 200 ms, but the last state always gets out
    /// (a burst of tasks followed by silence used to leave the panel stale).
    private func postStats(force: Bool) {
        let now = CACurrentMediaTime()
        if force || now - lastStatsPost > 0.2 {
            lastStatsPost = now
            trailingStats?.cancel()
            trailingStats = nil
            delegate?.engine(self, stats: stats)
        } else if trailingStats == nil {
            trailingStats = queue.after(0.2) { [weak self] in
                guard let self else { return }
                self.trailingStats = nil
                self.postStats(force: true)
            }
        }
    }

    // MARK: helpers

    static func json(_ value: Any) -> String {
        guard let data = try? JSONSerialization.data(withJSONObject: value, options: [.fragmentsAllowed]) else { return "null" }
        return String(decoding: data, as: UTF8.self)
    }
    static func parse(_ json: String) -> Any? {
        try? JSONSerialization.jsonObject(with: Data(json.utf8), options: [.fragmentsAllowed])
    }
    private static func splitPrefix(_ s: String) -> (String, String) {
        guard let i = s.firstIndex(of: ":") else { return (s, "") }
        return (String(s[..<i]), String(s[s.index(after: i)...]))
    }
    private static func countOccurrences(of needle: String, in text: String) -> Int {
        var count = 0
        var range = text.startIndex..<text.endIndex
        while let r = text.range(of: needle, range: range) {
            count += 1
            range = r.upperBound..<text.endIndex
        }
        return count
    }
}

// MARK: - Resources

public struct BundledLibrary {
    public let name: String
    public let file: String
    public let patterns: [NSRegularExpression]
}

/// The JavaScript shipped with the package (built by `pnpm build:ios`). The
/// directory is deliberately not called "Resources": codesign rejects iOS
/// bundles with a top-level Resources/ folder ("bundle format unrecognized").
public final class SandboxResources {
    public static let shared = SandboxResources()

    public let sandboxBundle: String
    public let rendererBundle: String
    public let validatorBundle: String
    /// Precompiled bytecode of the bundles (`.qbc` next to the `.js`, built together; nil when not shipped).
    public let sandboxBytecode: Data?
    public let validatorBytecode: Data?
    public let libraries: [BundledLibrary]
    private var libraryCache: [String: String] = [:]
    private var bytecodeCache: [String: Data?] = [:]
    private let lock = NSLock()

    /// `bundle` defaults to the package's resource bundle.
    public init(bundle: Bundle? = nil) {
        let bundle = bundle ?? Bundle.module
        func read(_ name: String, _ ext: String, _ sub: String) -> String {
            guard let url = bundle.url(forResource: name, withExtension: ext, subdirectory: sub), let s = try? String(contentsOf: url, encoding: .utf8) else {
                fatalError("WidgetSandbox: missing resource \(sub)/\(name).\(ext) — run `pnpm build:ios`")
            }
            return s
        }
        sandboxBundle = read("sandbox-bundle", "js", "WidgetJS")
        rendererBundle = read("renderer-bundle", "js", "WidgetJS")
        validatorBundle = read("validator-bundle", "js", "WidgetJS")
        sandboxBytecode = bundle.url(forResource: "sandbox-bundle", withExtension: "qbc", subdirectory: "WidgetJS").flatMap { try? Data(contentsOf: $0) }
        validatorBytecode = bundle.url(forResource: "validator-bundle", withExtension: "qbc", subdirectory: "WidgetJS").flatMap { try? Data(contentsOf: $0) }
        var libs: [BundledLibrary] = []
        if let data = bundle.url(forResource: "libraries", withExtension: "json", subdirectory: "WidgetJS").flatMap({ try? Data(contentsOf: $0) }),
           let list = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] {
            for item in list {
                guard let name = item["name"] as? String, let file = item["file"] as? String, let patterns = item["patterns"] as? [String] else { continue }
                libs.append(BundledLibrary(name: name, file: file, patterns: patterns.compactMap { try? NSRegularExpression(pattern: $0, options: [.caseInsensitive]) }))
            }
        }
        libraries = libs
        self.bundle = bundle
    }
    private let bundle: Bundle

    /// Same resolution as src/host/loader.ts: known CDN URLs → bundled copy; anything else → nil.
    public func library(for url: String) -> BundledLibrary? {
        var u = url.trimmingCharacters(in: .whitespacesAndNewlines)
        if u.hasPrefix("//") { u = "https:" + u }
        if let i = u.firstIndex(where: { $0 == "?" || $0 == "#" }) { u = String(u[..<i]) }
        let range = NSRange(u.startIndex..., in: u)
        return libraries.first { lib in lib.patterns.contains { $0.firstMatch(in: u, range: range) != nil } }
    }

    public func librarySource(_ lib: BundledLibrary) -> String? {
        lock.lock(); defer { lock.unlock() }
        if let s = libraryCache[lib.file] { return s }
        let name = (lib.file as NSString).deletingPathExtension
        let ext = (lib.file as NSString).pathExtension
        guard let url = bundle.url(forResource: name, withExtension: ext, subdirectory: "WidgetJS/libs"), let s = try? String(contentsOf: url, encoding: .utf8) else { return nil }
        libraryCache[lib.file] = s
        return s
    }

    /// Precompiled bytecode of a bundled library (`WidgetJS/libs/<file>.qbc`), cached; nil when not shipped.
    public func libraryBytecode(_ lib: BundledLibrary) -> Data? {
        lock.lock(); defer { lock.unlock() }
        if let cached = bytecodeCache[lib.file] { return cached }
        let name = (lib.file as NSString).deletingPathExtension
        let data = bundle.url(forResource: name, withExtension: "qbc", subdirectory: "WidgetJS/libs").flatMap { try? Data(contentsOf: $0) }
        bytecodeCache[lib.file] = data
        return data
    }
}

// MARK: - Intl delegation (pure formatting, no I/O)

enum IntlBridge {
    static func format(requestJson: String) -> String {
        guard let req = SandboxEngine.parse(requestJson) as? [String: Any], let kind = req["kind"] as? String else { return "" }
        let locale = (req["locale"] as? String).map(Locale.init(identifier:)) ?? ((req["locale"] as? [String])?.first).map(Locale.init(identifier:)) ?? .current
        let opts = req["opts"] as? [String: Any] ?? [:]
        let value = (req["value"] as? NSNumber)?.doubleValue ?? 0
        switch kind {
        case "number":
            let f = NumberFormatter()
            f.locale = locale
            switch opts["style"] as? String {
            case "currency": f.numberStyle = .currency; if let c = opts["currency"] as? String { f.currencyCode = c }
            case "percent": f.numberStyle = .percent
            default: f.numberStyle = .decimal
            }
            if let n = (opts["minimumFractionDigits"] as? NSNumber)?.intValue { f.minimumFractionDigits = n }
            if let n = (opts["maximumFractionDigits"] as? NSNumber)?.intValue { f.maximumFractionDigits = n }
            if let g = opts["useGrouping"] as? Bool { f.usesGroupingSeparator = g }
            return f.string(from: NSNumber(value: value)) ?? String(value)
        case "date", "dateMethod":
            let date = Date(timeIntervalSince1970: value / 1000)
            let f = DateFormatter()
            f.locale = locale
            let method = req["method"] as? String
            let wantsDate = ["weekday", "year", "month", "day", "dateStyle"].contains { opts[$0] != nil }
            let wantsTime = ["hour", "minute", "second", "timeStyle"].contains { opts[$0] != nil }
            if wantsDate || wantsTime {
                var template = ""
                if let w = opts["weekday"] as? String { template += w == "long" ? "EEEE" : "EEE" }
                if let m = opts["month"] as? String { template += m == "long" ? "MMMM" : m == "short" ? "MMM" : m == "numeric" ? "M" : "MM" }
                if opts["day"] != nil { template += "d" }
                if opts["year"] != nil { template += "y" }
                if opts["hour"] != nil { template += "j" }
                if opts["minute"] != nil { template += "mm" }
                if opts["second"] != nil { template += "ss" }
                if template.isEmpty {
                    f.dateStyle = Self.style(opts["dateStyle"] as? String)
                    f.timeStyle = Self.style(opts["timeStyle"] as? String)
                } else {
                    f.setLocalizedDateFormatFromTemplate(template)
                }
            } else {
                f.dateStyle = method == "toLocaleTimeString" ? .none : .medium
                f.timeStyle = method == "toLocaleDateString" ? .none : .medium
            }
            return f.string(from: date)
        case "plural":
            return value == 1 ? "one" : "other"
        case "relative":
            guard #available(macOS 10.15, iOS 13.0, *) else {
                return ""
            }
            let f = RelativeDateTimeFormatter()
            f.locale = locale
            let unit = req["unit"] as? String ?? "second"
            let seconds: Double = ["second": 1, "minute": 60, "hour": 3600, "day": 86400, "week": 604800, "month": 2_592_000, "year": 31_536_000][unit.hasSuffix("s") ? String(unit.dropLast()) : unit] ?? 1
            return f.localizedString(fromTimeInterval: value * seconds)
        case "list":
            let items = (req["value"] as? [Any])?.map { "\($0)" } ?? []
            guard #available(macOS 10.15, iOS 13.0, *) else {
                return items.joined(separator: ", ")
            }
            return ListFormatter.localizedString(byJoining: items)
        case "compare":
            let a = req["a"] as? String ?? "", b = req["b"] as? String ?? ""
            return String(a.compare(b, options: [], range: nil, locale: locale).rawValue)
        case "upper":
            return (req["value"] as? String ?? "").uppercased(with: locale)
        case "lower":
            return (req["value"] as? String ?? "").lowercased(with: locale)
        default:
            return ""
        }
    }

    private static func style(_ s: String?) -> DateFormatter.Style {
        switch s { case "full": return .full; case "long": return .long; case "medium": return .medium; case "short": return .short; default: return .none }
    }
}
