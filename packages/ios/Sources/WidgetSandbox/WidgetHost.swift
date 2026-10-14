// WidgetHost: one widget = one SandboxEngine (QuickJS on its own queue) + one
// TrustedValidator + one Renderer (RendererWebView unless a `rendererFactory`
// supplies another, see Renderer.swift). The only place where the three meet;
// nothing from the engine reaches the renderer without passing the validator.
// Mirrors src/host/widget-host.ts. All public API is main-thread.
// iOS and macOS share this file; `PlatformView` is UIView or NSView.

import Foundation
import WidgetSandboxProtocol
#if canImport(UIKit)
import UIKit
#elseif canImport(AppKit)
import AppKit
#endif

public struct WidgetDiagnostics {
    public var logs: [(level: String, text: String)] = []
    public var diags: [String: (count: Int, details: [String])] = [:]
    public var bridge: [(method: String, argsJson: String)] = []
    public var stats = SandboxStats()
    public var nodes = 0
    public var loaded = false
    /// Wall clock from WidgetHost creation to the renderer's `ready` and to `loaded` (the same definition as the compare runner).
    public var rendererReadyMs: Double?
    public var loadedMs: Double?
    /// `loaded` means the widget's scripts ran; `rendered` means the renderer has applied every op sent up to that
    /// point and its size reports for them have arrived (an ack round trip after `loaded`). A list that lays
    /// bubbles out from remembered heights trusts a new height only once this is set.
    public var rendered = false
    public var renderedMs: Double?
    /// Fatal: the runtime is gone. The renderer keeps the last state (frozen); the owner removes the view with `destroy()`.
    public var dead: String?
    /// Last uncaught widget error (non-fatal), cleared once a later task completes without one.
    public var lastError: String?
    public var height: CGFloat = 0
    /// Wall clock to the first `size` report that follows validated ops (the widget's first content on screen).
    public var firstContentMs: Double?
    /// Which parts came prewarmed from the pools ("renderer+engine+validator"), empty when none.
    public var prewarmed = ""
    public init() {}
}

public final class WidgetHost: SandboxEngineDelegate {
    /// Limits for the widget's QuickJS runtime (see SandboxConfig); mirrors WidgetHost.Options on Android.
    public struct Options {
        public var memoryLimitBytes = 64 << 20
        public var cpuLimitMs = 500.0
        public var cpuQuotaMs = 300.0
        public var cpuQuotaWindowMs = 1000.0
        public var cpuKillAfterMs = 0.0
        public var maxTimers = 256
        public var maximumHeight: CGFloat = 900
        /// Initial visibility; change it later with `setVisible`.
        public var visible = true
        /// One WebKit environment (process pool, data store, content controller, rule list, scheme handler) for every renderer; see RendererEnvironment.
        public var sharedWebKit = false
        /// A few JSThreads shared by every validator instead of a thread per widget (see TrustedValidator).
        public var sharedValidatorThread = false
        /// Take prewarmed parts from RendererPool / EnginePool / ValidatorPool when they have any, and recycle the validator on destroy.
        public var pools = false
        public var recyclesRenderers = false
        /// Optional wire forms to advertise (SandboxConfig.features), e.g. `canvasWords`, `binaryOps`; empty means plain JSON.
        public var features: Set<String> = []
        /// Side channels next to the DOM protocol, one fresh instance per host (e.g. the twidget kit's 3D surfaces).
        public var sideChannels: [() -> WidgetSideChannel] = []
        /// Build the QuickJS runtime and evaluate the sandbox bundle while the renderer document is still loading, instead of after `ready`.
        public var parallelBoot = false
        /// Run the widget's own scripts as soon as the host exists instead of after the renderer's `ready`; queries made before the renderer loads wait for it.
        public var startBeforeRendererReady = false
        /// Validate ops and queries on the validator's thread and only talk to the renderer on the executor; false validates on the executor itself.
        public var asyncValidation = false
        /// Boot hosts that the feed creates ahead of the viewport on a background queue (WidgetPreparer) and adopt them once their first content is laid out.
        public var backgroundBoot = false
        public var assetResolver: WidgetAssetResolver?
        public var policy: SandboxPolicy = .compat
        public init() {}
    }

    public let view: PlatformView
    private var state = WidgetDiagnostics()
    private let publishLock = NSLock()
    private var published = WidgetDiagnostics()
    public var diagnostics: WidgetDiagnostics {
        publishLock.lock()
        defer { publishLock.unlock() }
        return published
    }
    public let executor: WidgetExecutor
    public var rendererObject: Renderer { renderer }
    public var onChange: ((WidgetDiagnostics) -> Void)?
    /// Called on the host's executor with every change, before the coalesced main-thread `onChange`.
    public var onChangeOnExecutor: ((WidgetDiagnostics) -> Void)?
    public var onBridge: ((String, String) -> Void)?
    /// Non-fatal uncaught exception in widget code (also in `state.logs`).
    public var onError: ((String) -> Void)?
    /// Badge text shown by the renderer over a dead widget (user-facing, no error details).
    public var stoppedLabel = "Widget stopped"

    private let engine: SandboxEngine
    private let sideChannels: [WidgetSideChannel]
    private let validator: TrustedValidator
    private let renderer: Renderer
    private let options: Options
    private let config: SandboxConfig
    private var theme: String
    private var rendererReady = false
    private var engineStarted = false
    private let html: String
    private enum Outgoing {
        case json(String)
        case ops([WidgetOp])
    }
    private var pendingToRenderer: [Outgoing] = []
    private let typedOps: Bool
    private var destroyed = false
    private var viewport: (w: Double, h: Double, dpr: Double)
    private var ownerVisible: Bool
    private var inWindow = false
    private var qid = 0
    private var pendingQuery: PendingQuery?
    private let queryLock = NSLock()
    private let minHeight: CGFloat = 24
    private let maxHeight: CGFloat
    private let createdAt = Date()
    private var opsSent = 0
    public private(set) var frameCount = 0

    /// `rendererFactory`: the renderer; the WebView renderer when nil.
    public init(html: String, theme: String = "light", width: CGFloat = 400, options: Options = Options(), resources: SandboxResources = .shared, rendererFactory: RendererFactory? = nil, executor: WidgetExecutor = WidgetExecutor(), shell: WidgetShellPool.Shell? = nil) throws {
        self.executor = executor
        self.html = html
        sideChannels = options.sideChannels.map { $0() }
        self.theme = theme
        self.options = options
        maxHeight = options.maximumHeight.isFinite ? max(24, options.maximumHeight) : 900
        ownerVisible = options.visible
        let scale = PlatformScreen.scale
        viewport = (Double(width), 300, scale)
        var prewarmed: [String] = []
        var rendererLoaded = false
        if let shell = shell {
            renderer = shell.renderer
            prewarmed.append("shell")
        } else if let factory = rendererFactory {
            renderer = factory(resources, theme, width)
        } else if options.pools, let pooled = RendererPool.shared.take() {
            pooled.setTheme(theme)
            renderer = pooled
            rendererLoaded = true
            prewarmed.append("renderer")
        } else {
            renderer = RendererWebView(rendererBundle: resources.rendererBundle, theme: theme, environment: options.sharedWebKit ? .shared : nil)
        }
        typedOps = renderer.acceptsTypedOps
        let container = shell?.container ?? WidgetShellPool.makeContainer(renderer, width: width)
        view = container
        var diagSink: ((String, String) -> Void) = { _, _ in }
        if options.pools, let pooled = ValidatorPool.shared.take(theme: theme) {
            validator = pooled
            validator.onDiag = { k, d in diagSink(k, d) }
            prewarmed.append("validator")
        } else {
            validator = TrustedValidator.background(theme: theme, resources: resources, thread: options.sharedValidatorThread ? TrustedValidator.sharedThreadForNextValidator() : nil) { k, d in diagSink(k, d) }
        }
        var config = SandboxConfig(innerWidth: viewport.w, innerHeight: viewport.h, devicePixelRatio: scale, theme: theme)
        config.memoryLimitBytes = options.memoryLimitBytes
        config.cpuLimitMs = options.cpuLimitMs
        config.cpuQuotaMs = options.cpuQuotaMs
        config.cpuQuotaWindowMs = options.cpuQuotaWindowMs
        config.cpuKillAfterMs = options.cpuKillAfterMs
        config.maxTimers = options.maxTimers
        config.features = options.features
        config.visible = options.visible
        config.policy = options.policy
        self.config = config
        if options.pools, let pooled = EnginePool.shared.take(features: options.features) {
            engine = pooled
            prewarmed.append("engine")
        } else {
            engine = SandboxEngine(resources: resources, features: options.features)
        }
        state.prewarmed = prewarmed.joined(separator: "+")
        validator.setAssetResolver(options.assetResolver)
        validator.setPolicy(options.policy)
        validator.useNativeModel(engine.runtimeFeatures.contains("binaryOps"))
        engine.delegate = self
        let channels = sideChannels
        engine.afterBoot = { js in channels.forEach { $0.install(in: js) } }
        for channel in channels {
            channel.connect(renderer)
            channel.onDiag = { [weak self] k, d in self?.executor.async { self?.diag(k, d) } }
        }
        diagSink = { [weak self] k, d in self?.diag(k, d) }
        renderer.onDiag = { [weak self] k, d in self?.diag(k, d) }
        if let scheduled = renderer as? ScheduledRenderer {
            scheduled.schedule = { [weak executor] block in executor?.async(block) }
            scheduled.scheduleAfter = { [weak executor] seconds, block in executor?.after(seconds, block) }
            scheduled.isOnExecutor = { [weak executor] in executor?.isCurrent ?? Thread.isMainThread }
            scheduled.scheduleUrgent = { [weak executor] block in executor?.urgent(block) }
            scheduled.executorIsMain = { [weak executor] in executor?.isMain ?? true }
        }
        if shell != nil {
            if executor.isMain || executor.isCurrent {
                renderer.setTheme(theme)
            } else {
                let themed = renderer
                executor.async { themed.setTheme(theme) }
            }
        }
        container.onWindowChange = { [weak self] inWindow in
            self?.onExecutor {
                self?.inWindow = inWindow
                self?.pushVisibility()
            }
        }
        renderer.onMessage = { [weak self] json in self?.onRenderer(json) }
        publish()
        if !rendererLoaded && !executor.isMain && !executor.isCurrent {
            rendererLoaded = true
            let loading = renderer
            executor.async { loading.load() }
        }
        if options.startBeforeRendererReady {
            startEngine()
        } else if options.parallelBoot {
            engine.prewarm()
        }
        let warmed = validator
        warmed.perform { warmed.warmStylesheets(Self.styleTexts(in: html)) }
        if !rendererLoaded { renderer.load() }
    }

    private func startEngine() {
        if engineStarted { return }
        engineStarted = true
        engine.start(html: html, config: config)
    }

    private func onExecutor(_ block: @escaping () -> Void) {
        if executor.isCurrent {
            block()
        } else {
            executor.async(block)
        }
    }

    private func onMain(_ block: @escaping () -> Void) {
        if Thread.isMainThread {
            block()
        } else {
            DispatchQueue.main.async(execute: block)
        }
    }

    public var isAdopted: Bool {
        return executor.isMain
    }

    public func moveToMain(_ completion: @escaping () -> Void) {
        executor.moveToMain(completion)
    }

    public func setTheme(_ theme: String) {
        guard executor.isCurrent else {
            return executor.async { [weak self] in self?.setTheme(theme) }
        }
        guard !destroyed else { return }
        self.theme = theme
        renderer.setTheme(theme)
        validate { [self] in
            let ops = validator.setTheme(theme)
            deliver { self.toRenderer("{\"type\":\"ops\",\"ops\":\(ops)}") }
        }
        engine.theme(theme)
    }

    /// The last render as an image, for an owner that is about to `destroy()` a
    /// bubble scrolled far away and wants to keep its picture. nil when the
    /// renderer cannot snapshot (not a WebView, or WebKit refused).
    public func snapshot(completion: @escaping (PlatformImage?) -> Void) {
        guard executor.isCurrent else {
            return executor.async { [weak self] in
                guard let self = self else { return DispatchQueue.main.async { completion(nil) } }
                self.snapshot { image in DispatchQueue.main.async { completion(image) } }
            }
        }
        if destroyed { return completion(nil) }
        renderer.snapshot(completion: completion)
    }

    public func bridgeEvent(_ event: String, dataJson: String = "null") {
        engine.bridgeEvent(event, dataJson: dataJson)
    }

    /// Sends a newer version of the widget's HTML into the running sandbox (`morphUpdates`); `completion` runs on main.
    public func morph(html: String, completion: @escaping (Bool) -> Void) {
        engine.morph(html: html) { applied in
            DispatchQueue.main.async { completion(applied) }
        }
    }

    /// Off-screen pause. The owner reports whether the bubble is on screen
    /// (scrolled away, app in the background, …); the view's own window
    /// attachment is combined with it. Hidden: no animation frames, timers at
    /// 1 Hz, `document.hidden` / `visibilitychange` inside the widget.
    public func setVisible(_ visible: Bool) {
        guard executor.isCurrent else {
            return executor.async { [weak self] in self?.setVisible(visible) }
        }
        ownerVisible = visible
        pushVisibility()
    }

    private func pushVisibility() {
        engine.setVisible(ownerVisible && inWindow)
        sideChannels.forEach { $0.setVisible(ownerVisible && inWindow) }
    }


    public func destroy() {
        guard executor.isCurrent else {
            return executor.async { [self] in destroy() }
        }
        if destroyed { return }
        destroyed = true
        engine.terminate()
        sideChannels.forEach { $0.destroy() }
        renderer.onMessage = nil
        renderer.onDiag = nil
        if options.pools, options.recyclesRenderers, let web = renderer as? RendererWebView {
            RendererPool.shared.recycle(web)
        } else {
            renderer.destroy()
        }
        resolveQuery(json: "{\"error\":\"dead\"}", binary: nil)
        validatorLock.lock()
        validatorReleased = true
        validatorLock.unlock()
        if options.pools, TrustedValidator.isShared(validator.thread) { ValidatorPool.shared.recycle(validator) }
    }

    // MARK: engine → host (engine queue)

    /// Validation runs where the options say: on the executor, or on the
    /// validator's thread with only the renderer call on the executor. Both keep
    /// the order of ops, queries, bridge calls and diagnostics: one serial
    /// validator, one serial paced queue.
    private let validatorLock = NSLock()
    private var validatorReleased = false

    private func validate(_ work: @escaping () -> Void) {
        let guarded = { [weak self] in
            guard let self = self else { return }
            self.validatorLock.lock()
            let released = self.validatorReleased
            self.validatorLock.unlock()
            if !released { work() }
        }
        if options.asyncValidation {
            validator.perform(guarded)
        } else {
            executor.paced(guarded)
        }
    }

    private func deliver(_ block: @escaping () -> Void) {
        if options.asyncValidation {
            executor.paced(block)
        } else {
            block()
        }
    }

    private func inOrder(_ block: @escaping () -> Void) {
        validate { [self] in deliver(block) }
    }

    public func engine(_ engine: SandboxEngine, ops json: String) {
        self.engine(engine, ops: json, canvas: [:])
    }

    public func engine(_ engine: SandboxEngine, ops json: String, canvas: [Int: WidgetCanvasBatch]) {
        validate { [self] in
            let batch = validator.processOpsForRenderer(json, canvas: canvas, typed: typedOps)
            let nodes = validator.nodeCount
            deliver {
                switch batch {
                case .empty:
                    break
                case .json(let clean):
                    self.opsSent += 1
                    self.toRenderer(.json("{\"type\":\"ops\",\"ops\":\(clean)}"))
                case .ops(let ops):
                    self.opsSent += 1
                    self.toRenderer(.ops(ops))
                }
                self.state.nodes = nodes
            }
        }
    }

    public func engine(_ engine: SandboxEngine, binaryOps: BinaryOpsPayload, canvas: [Int: WidgetCanvasBatch]) {
        validate { [self] in
            let batch = validator.processBinary(binaryOps, canvas: canvas, typed: typedOps)
            let nodes = validator.nodeCount
            deliver {
                switch batch {
                case .empty:
                    break
                case .json(let clean):
                    self.opsSent += 1
                    self.toRenderer(.json("{\"type\":\"ops\",\"ops\":\(clean)}"))
                case .ops(let ops):
                    self.opsSent += 1
                    self.toRenderer(.ops(ops))
                }
                self.state.nodes = nodes
            }
        }
    }

    public func engine(_ engine: SandboxEngine, query json: String) -> (json: String, binary: Data?) {
        let pending = PendingQuery()
        queryLock.lock()
        pendingQuery = pending
        queryLock.unlock()
        validate { [self] in
            // Validation (css-tree, media query rewrite) on the validator's thread or inline; the send on main.
            var clean = validator.validateQuery(json)
            if let c = clean, c.contains("\"matchMedia\""), var obj = SandboxEngine.parse(c) as? [String: Any], let q = obj["query"] as? String {
                obj["query"] = validator.rewriteMediaQuery(q)
                clean = Self.json(obj)
            }
            let send = {
                guard !self.destroyed else { return self.resolveQuery(json: "{\"error\":\"dead\"}", binary: nil) }
                guard let clean else { return self.resolveQuery(json: "{\"error\":\"invalid query\"}", binary: nil) }
                self.qid += 1
                pending.id = self.qid
                self.toRenderer("{\"type\":\"query\",\"qid\":\(self.qid),\"query\":\(clean)}")
            }
            deliver(send)
        }
        if pending.semaphore.wait(timeout: .now() + 3) == .timedOut {
            queryLock.lock()
            if pendingQuery === pending { pendingQuery = nil }
            queryLock.unlock()
            return ("{\"error\":\"timeout\"}", nil)
        }
        return (pending.json, pending.binary)
    }

    private func resolveQuery(json: String, binary: Data?) {
        queryLock.lock()
        let p = pendingQuery
        pendingQuery = nil
        queryLock.unlock()
        guard let p else { return }
        p.json = json
        p.binary = binary
        p.semaphore.signal()
    }

    public func engine(_ engine: SandboxEngine, listen json: String) {
        validate { [self] in
            let types = validator.validateListen(json)
            deliver { self.toRenderer("{\"type\":\"listen\",\"types\":\(types)}") }
        }
    }
    public func engine(_ engine: SandboxEngine, preventDefault json: String) {
        validate { [self] in
            let keys = validator.validatePreventDefault(json)
            deliver { self.toRenderer("{\"type\":\"preventDefault\",\"keys\":\(keys)}") }
        }
    }
    public func engine(_ engine: SandboxEngine, anim json: String) {
        validate { [self] in
            let clean = validator.validateAnim(json)
            guard clean != "[]" else { return }
            let send = { self.toRenderer("{\"type\":\"anim\",\"now\":\(CACurrentMediaTime() * 1000),\"anims\":\(clean)}") }
            executor.async(send)
        }
    }
    public func engineRequestsFrame(_ engine: SandboxEngine) {
        executor.paced { self.toRenderer("{\"type\":\"requestFrame\"}") }
    }
    public func engine(_ engine: SandboxEngine, log level: String, text: String) {
        inOrder {
            if self.state.logs.count < 5000 { self.state.logs.append((level, text)) }
            if level == "error", text.hasPrefix("Uncaught ") {
                self.state.lastError = text
                self.tasksAtError = self.state.stats.tasks
                let onError = self.onError
                self.onMain { onError?(text) }
            }
            self.changed()
        }
    }
    private var tasksAtError = 0
    public func engine(_ engine: SandboxEngine, diag kind: String, detail: String) {
        inOrder { self.diag(kind, detail) }
    }
    public func engine(_ engine: SandboxEngine, bridge method: String, argsJson: String) {
        inOrder {
            self.state.bridge.append((method, argsJson))
            self.changed()
            self.onMain { self.onBridge?(method, argsJson) }
        }
    }
    public func engineLoaded(_ engine: SandboxEngine) {
        let loadedMs = Date().timeIntervalSince(createdAt) * 1000
        // Through the validation pipeline, so `loaded` is observed after the ops of the loading tasks were sent;
        // then one trusted query to the renderer: its answer comes after every size report those ops produced.
        validate { [self] in
            let deliver = {
                self.state.loaded = true
                self.state.loadedMs = loadedMs
                self.changed()
                self.toRenderer("{\"type\":\"query\",\"qid\":\(Self.renderAckQid),\"query\":{\"q\":\"viewport\"}}")
            }
            self.deliver(deliver)
        }
    }
    /// qid of the host's own post-load ack query; engine queries count up from 1.
    private static let renderAckQid = -1
    public func engine(_ engine: SandboxEngine, stats: SandboxStats) {
        executor.async {
            self.state.stats = stats
            if self.state.lastError != nil, stats.tasks > self.tasksAtError + 1 { self.state.lastError = nil }
            self.changed()
        }
    }
    public func engine(_ engine: SandboxEngine, died reason: String) {
        executor.paced {
            if self.state.dead != nil { return }
            self.state.dead = reason
            self.changed()
            self.resolveQuery(json: "{\"error\":\"dead\"}", binary: nil)
            // The renderer stays: last render as fallback, badge from the renderer, no more events or frames.
            self.toRenderer(Self.json(["type": "frozen", "reason": self.stoppedLabel, "fatal": true]))
        }
    }

    /// Trusted script evaluated in the renderer (tests and diagnostics only); "null" from a renderer without script.
    public func evaluateInRenderer(_ script: String, completion: @escaping (String) -> Void) {
        guard executor.isCurrent else {
            return executor.async { [weak self] in self?.evaluateInRenderer(script) { value in DispatchQueue.main.async { completion(value) } } }
        }
        renderer.evaluate(script) { completion($0 ?? "null") }
    }

    // MARK: renderer → host (main thread)

    private func onRenderer(_ json: String) {
        guard let msg = SandboxEngine.parse(json) as? [String: Any], let type = msg["type"] as? String else { return }
        switch type {
        case "ready":
            if rendererReady { return }
            rendererReady = true
            state.rendererReadyMs = Date().timeIntervalSince(createdAt) * 1000
            let queued = pendingToRenderer
            pendingToRenderer = []
            queued.forEach(deliver)
            startEngine()
        case "event":
            if let ev = msg["ev"] {
                if let gesture = gesture(for: ev) {
                    engine.activationEvent(json: Self.json(ev), gesture: gesture)
                } else {
                    engine.event(json: Self.json(ev))
                }
            }
        case "result":
            guard let id = (msg["qid"] as? NSNumber)?.intValue else { return }
            if id == Self.renderAckQid {
                if !state.rendered {
                    state.rendered = true
                    state.renderedMs = Date().timeIntervalSince(createdAt) * 1000
                    changed()
                }
                return
            }
            queryLock.lock()
            let match = pendingQuery?.id == id
            queryLock.unlock()
            guard match else { return }
            let binary = (msg["binaryBase64"] as? String).flatMap { Data(base64Encoded: $0) }
            resolveQuery(json: Self.json(msg["result"] ?? NSNull()), binary: binary)
        case "size":
            let height = min(max(CGFloat((msg["height"] as? NSNumber)?.doubleValue ?? 0), minHeight), maxHeight)
            if state.firstContentMs == nil, opsSent > 0, (msg["painted"] as? Bool) != false {
                state.firstContentMs = Date().timeIntervalSince(createdAt) * 1000
                changed()
            }
            if height != state.height {
                state.height = height
                changed()
            }
            let vp = ((msg["innerWidth"] as? NSNumber)?.doubleValue ?? viewport.w, (msg["innerHeight"] as? NSNumber)?.doubleValue ?? viewport.h, (msg["dpr"] as? NSNumber)?.doubleValue ?? viewport.dpr)
            if vp != viewport {
                viewport = vp
                engine.viewport(innerWidth: vp.0, innerHeight: vp.1, dpr: vp.2)
            }
        case "resize":
            if let entries = msg["entries"] { engine.resize(entriesJson: Self.json(entries)) }
        case "frame":
            frameCount += 1
            engine.frame(time: (msg["time"] as? NSNumber)?.doubleValue ?? 0)
        case "media":
            engine.theme(theme)
        case "violation":
            diag("renderer-violation", msg["detail"] as? String ?? "")
        default:
            break
        }
    }

    private func toRenderer(_ json: String) {
        toRenderer(.json(json))
    }

    private func toRenderer(_ message: Outgoing) {
        if destroyed { return }
        if rendererReady { deliver(message) } else { pendingToRenderer.append(message) }
    }

    private func deliver(_ message: Outgoing) {
        switch message {
        case .json(let json): renderer.send(json)
        case .ops(let ops): renderer.send(ops: ops)
        }
    }

    private func diag(_ kind: String, _ detail: String) {
        guard executor.isCurrent else {
            return executor.async { [weak self] in self?.diag(kind, detail) }
        }
        var entry = state.diags[kind] ?? (0, [])
        entry.count += 1
        if entry.details.count < 50 { entry.details.append(detail) }
        state.diags[kind] = entry
        changed()
    }

    private var changeScheduled = false
    private func publish() {
        publishLock.lock()
        published = state
        publishLock.unlock()
    }

    private func changed() {
        // Coalesce UI updates: a misbehaving widget can emit thousands of diagnostics per second.
        onChangeOnExecutor?(state)
        publishLock.lock()
        published = state
        let schedule = !changeScheduled
        changeScheduled = true
        publishLock.unlock()
        guard schedule else { return }
        DispatchQueue.main.async {
            self.publishLock.lock()
            self.changeScheduled = false
            let snapshot = self.published
            self.publishLock.unlock()
            self.onChange?(snapshot)
        }
    }

    static func json(_ value: Any) -> String { SandboxEngine.json(value) }

    static let activationEventTypes: Set<String> = ["click", "dblclick", "mousedown", "mouseup", "pointerdown", "pointerup", "keydown"]
    private var gestureNumber = 0
    private var pressOpen = false
    private var lastActivationType = ""

    /// One physical press (pointerdown, mousedown, pointerup, mouseup, click, dblclick) or one key press is one gesture.
    private func gesture(for ev: Any) -> Int? {
        guard Self.isUserActivation(ev), let type = (ev as? [String: Any])?["type"] as? String else { return nil }
        let continues: Bool
        switch type {
        case "pointerdown": continues = false
        case "mousedown": continues = lastActivationType == "pointerdown"
        case "pointerup", "mouseup", "click": continues = pressOpen
        case "dblclick": continues = lastActivationType == "click"
        default: continues = false
        }
        if !continues { gestureNumber += 1 }
        pressOpen = type != "click" && type != "dblclick" && type != "keydown"
        lastActivationType = type
        return gestureNumber
    }

    /// Renderer events that count as a user gesture: clicks and key presses (not Escape, not a repeat, not a Command shortcut).
    static func isUserActivation(_ ev: Any) -> Bool {
        guard let ev = ev as? [String: Any], let type = ev["type"] as? String, activationEventTypes.contains(type) else { return false }
        if type == "keydown" {
            let modifiers = (ev["modifiers"] as? NSNumber)?.intValue ?? 0
            return ev["key"] as? String != "Escape" && (ev["repeat"] as? Bool) != true && modifiers & 4 == 0
        }
        return true
    }

    static func styleTexts(in html: String, limit: Int = 4) -> [String] {
        var texts: [String] = []
        var rest = html[...]
        while texts.count < limit, let open = rest.range(of: "<style", options: .caseInsensitive), let tagEnd = rest[open.upperBound...].firstIndex(of: ">"),
              let close = rest[rest.index(after: tagEnd)...].range(of: "</style", options: .caseInsensitive) {
            texts.append(String(rest[rest.index(after: tagEnd)..<close.lowerBound]))
            rest = rest[close.upperBound...]
        }
        return texts
    }
}

#if canImport(UIKit)
/// Reports window attachment to the host (the UIKit side of the off-screen pause).
final class WidgetContainerView: UIView {
    var onWindowChange: ((Bool) -> Void)?
    override func didMoveToWindow() {
        super.didMoveToWindow()
        onWindowChange?(window != nil)
    }
}
#elseif canImport(AppKit)
/// The AppKit side of the off-screen pause: the view counts as hidden when it
/// has no window or when that window is not visible on any screen (minimised,
/// fully covered, on another Space — `NSWindow.occlusionState`).
final class WidgetContainerView: NSView {
    var onWindowChange: ((Bool) -> Void)?
    private var occlusionObserver: NSObjectProtocol?
    /// `occlusionState` is trusted once the window server has reported on this
    /// window at all (a notification, or `.visible` seen). A process that never
    /// finishes launching NSApplication — the xctest runner — gets no occlusion
    /// updates and its windows stay "not visible" forever; `isVisible` is the
    /// fallback there.
    private var occlusionTrusted = false

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if let occlusionObserver { NotificationCenter.default.removeObserver(occlusionObserver) }
        occlusionObserver = nil
        occlusionTrusted = false
        if let window {
            occlusionObserver = NotificationCenter.default.addObserver(forName: NSWindow.didChangeOcclusionStateNotification, object: window, queue: .main) { [weak self] _ in
                self?.occlusionTrusted = true
                self?.report()
            }
        }
        report()
    }

    private func report() {
        guard let window else { return onWindowChange?(false) ?? () }
        let occluded = !window.occlusionState.contains(.visible)
        if !occluded { occlusionTrusted = true }
        onWindowChange?(occlusionTrusted ? !occluded : window.isVisible)
    }

    deinit {
        if let occlusionObserver { NotificationCenter.default.removeObserver(occlusionObserver) }
    }
}
#endif

private final class PendingQuery {
    let semaphore = DispatchSemaphore(value: 0)
    var id = 0
    var json = "null"
    var binary: Data?
}
