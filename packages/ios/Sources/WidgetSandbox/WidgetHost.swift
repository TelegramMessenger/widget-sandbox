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
        /// Initial visibility; change it later with `setVisible`.
        public var visible = true
        /// One WebKit environment (process pool, data store, content controller, rule list, scheme handler) for every renderer; see RendererEnvironment.
        public var sharedWebKit = false
        /// One JSThread for every validator instead of a thread per widget (see TrustedValidator).
        public var sharedValidatorThread = false
        /// Take prewarmed parts from RendererPool / EnginePool / ValidatorPool when they have any, and recycle the validator on destroy.
        public var pools = false
        /// Build the QuickJS runtime and evaluate the sandbox bundle while the renderer document is still loading, instead of after `ready`.
        public var parallelBoot = false
        /// Validate ops and queries on the validator's thread and hop to the main thread only to talk to the renderer, instead of blocking the main thread for each batch.
        public var asyncValidation = false
        /// Main-thread deliveries go through `MainPacer` (bounded work per run loop turn, order kept) instead of straight onto the main queue.
        public var pacedDelivery = false
        /// Optional wire forms to advertise (SandboxConfig.features), e.g. `canvasWords`; empty means plain JSON.
        public var features: Set<String> = []
        public init() {}
    }

    public let view: PlatformView
    public private(set) var diagnostics = WidgetDiagnostics()
    public var onChange: ((WidgetDiagnostics) -> Void)?
    public var onBridge: ((String, String) -> Void)?
    /// Non-fatal uncaught exception in widget code (also in `diagnostics.logs`).
    public var onError: ((String) -> Void)?
    /// Badge text shown by the renderer over a dead widget (user-facing, no error details).
    public var stoppedLabel = "Widget stopped"

    private let engine: SandboxEngine
    private let validator: TrustedValidator
    private let renderer: Renderer
    /// Read once on the main thread; validation threads decide from it whether to parse validator output into `WidgetOp`s.
    private var rendererAcceptsTypedOps = false
    private let options: Options
    private let html: String
    private let config: SandboxConfig
    private var theme: String
    private var rendererReady = false
    private var engineStarted = false
    private var pendingToRenderer: [String] = []
    private var destroyed = false
    private var viewport: (w: Double, h: Double, dpr: Double)
    private var ownerVisible: Bool
    private var inWindow = false
    private var qid = 0
    private var pendingQuery: PendingQuery?
    private let queryLock = NSLock()
    private let minHeight: CGFloat = 24, maxHeight: CGFloat = 900
    private let createdAt = Date()
    private var opsSent = 0

    /// `rendererFactory`: the renderer; the WebView renderer when nil.
    public init(html: String, theme: String = "light", width: CGFloat = 400, options: Options = Options(), resources: SandboxResources = .shared, rendererFactory: RendererFactory? = nil) throws {
        self.theme = theme
        self.html = html
        self.options = options
        ownerVisible = options.visible
        let scale = PlatformScreen.scale
        viewport = (Double(width), 300, scale)
        var prewarmed: [String] = []
        var rendererLoaded = false
        if let factory = rendererFactory {
            renderer = factory(resources, theme, width)
        } else if options.pools, let pooled = RendererPool.shared.take() {
            pooled.setTheme(theme)
            renderer = pooled
            rendererLoaded = true
            prewarmed.append("renderer")
        } else {
            renderer = RendererWebView(rendererBundle: resources.rendererBundle, theme: theme, environment: options.sharedWebKit ? .shared : nil)
        }
        let container = WidgetContainerView(frame: CGRect(x: 0, y: 0, width: width, height: 300))
        renderer.view.frame = container.bounds
        #if canImport(UIKit)
        renderer.view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        #else
        renderer.view.autoresizingMask = [.width, .height]
        #endif
        container.addSubview(renderer.view)
        view = container
        var diagSink: ((String, String) -> Void) = { _, _ in }
        if options.pools, let pooled = ValidatorPool.shared.take(theme: theme) {
            validator = pooled
            validator.onDiag = { k, d in diagSink(k, d) }
            prewarmed.append("validator")
        } else {
            validator = try TrustedValidator(theme: theme, resources: resources, thread: options.sharedValidatorThread ? TrustedValidator.sharedThread : nil) { k, d in diagSink(k, d) }
        }
        var config = SandboxConfig(innerWidth: viewport.w, innerHeight: viewport.h, devicePixelRatio: scale, theme: theme)
        config.features = options.features
        config.memoryLimitBytes = options.memoryLimitBytes
        config.cpuLimitMs = options.cpuLimitMs
        config.cpuQuotaMs = options.cpuQuotaMs
        config.cpuQuotaWindowMs = options.cpuQuotaWindowMs
        config.cpuKillAfterMs = options.cpuKillAfterMs
        config.maxTimers = options.maxTimers
        config.visible = options.visible
        self.config = config
        if options.pools, let pooled = EnginePool.shared.take(features: options.features) {
            engine = pooled
            prewarmed.append("engine")
        } else {
            engine = SandboxEngine(resources: resources, features: options.features)
        }
        diagnostics.prewarmed = prewarmed.joined(separator: "+")
        rendererAcceptsTypedOps = renderer.acceptsTypedOps
        // Binary ops are validated by the native node model, which must own the widget's nodes from the first batch.
        if engine.runtimeFeatures.contains("binaryOps") { validator.useNativeModel(true) }
        engine.delegate = self
        // Runtime + sandbox bundle now, on the engine's thread, while WebKit loads the document.
        if options.parallelBoot { engine.prewarm() }
        diagSink = { [weak self] k, d in self?.diag(k, d) }
        renderer.onDiag = { [weak self] k, d in self?.diag(k, d) }
        container.onWindowChange = { [weak self] inWindow in self?.inWindow = inWindow; self?.pushVisibility() }
        // The widget boots once the renderer announced itself, so the very first
        // layout queries have something to measure. A pooled renderer replays its
        // buffered `ready` as soon as the listener is set.
        renderer.onMessage = { [weak self] json in self?.onRenderer(json) }
        if !rendererLoaded { renderer.load() }
    }

    public func setTheme(_ theme: String) {
        self.theme = theme
        renderer.setTheme(theme)
        if options.asyncValidation {
            validate { [self] in
                let ops = validator.setTheme(theme)
                toMain { self.toRenderer(Self.json(["type": "ops", "ops": Self.raw(ops)])) }
            }
        } else {
            toRenderer(Self.json(["type": "ops", "ops": Self.raw(validator.setTheme(theme))]))
        }
        engine.theme(theme)
    }

    /// The last render as an image, for an owner that is about to `destroy()` a
    /// bubble scrolled far away and wants to keep its picture. nil when the
    /// renderer cannot snapshot (not a WebView, or WebKit refused).
    public func snapshot(completion: @escaping (PlatformImage?) -> Void) {
        if destroyed { return completion(nil) }
        renderer.snapshot(completion: completion)
    }

    public func bridgeEvent(_ event: String, dataJson: String = "null") {
        engine.bridgeEvent(event, dataJson: dataJson)
    }

    /// Off-screen pause. The owner reports whether the bubble is on screen
    /// (scrolled away, app in the background, …); the view's own window
    /// attachment is combined with it. Hidden: no animation frames, timers at
    /// 1 Hz, `document.hidden` / `visibilitychange` inside the widget.
    public func setVisible(_ visible: Bool) {
        ownerVisible = visible
        pushVisibility()
    }

    private func pushVisibility() {
        engine.setVisible(ownerVisible && inWindow)
    }

    public func destroy() {
        if destroyed { return }
        destroyed = true
        engine.terminate()
        renderer.destroy()
        resolveQuery(json: "{\"error\":\"dead\"}", binary: nil)
        validatorLock.lock()
        validatorReleased = true
        validatorLock.unlock()
        if options.pools, validator.thread === TrustedValidator.sharedThread { ValidatorPool.shared.recycle(validator) }
    }

    // MARK: engine → host (engine queue)

    /// Validation runs where the options say: inline on the main thread, or on
    /// the validator's thread with only the renderer call on main. Both keep the
    /// order of ops and queries: one serial validator, one serial main queue.
    /// Work queued before destroy() must not run on a validator that went back
    /// to the pool: it would feed this widget's ops into the next widget's model.
    /// Every hop of this host to the main thread, in order.
    private func toMain(_ block: @escaping () -> Void) {
        if options.pacedDelivery { MainPacer.shared.async(block) } else { DispatchQueue.main.async(execute: block) }
    }

    private let validatorLock = NSLock()
    private var validatorReleased = false

    private func validate(_ work: @escaping () -> Void) {
        guard options.asyncValidation else {
            toMain(work)
            return
        }
        validator.perform { [weak self] in
            guard let self else { return }
            self.validatorLock.lock()
            let released = self.validatorReleased
            self.validatorLock.unlock()
            if !released { work() }
        }
    }

    public func engine(_ engine: SandboxEngine, ops json: String) {
        self.engine(engine, ops: json, canvas: [:])
    }

    public func engine(_ engine: SandboxEngine, binaryOps payload: BinaryOpsPayload, canvas: [Int: WidgetCanvasBatch]) {
        let typed = rendererAcceptsTypedOps
        validate { [self] in
            let batch = validator.processBinary(payload, canvas: canvas, typed: typed)
            let nodes = validator.nodeCount
            let deliver = {
                switch batch {
                case .empty: break
                case .json(let clean): self.toRenderer("{\"type\":\"ops\",\"ops\":\(clean)}"); self.opsSent += 1
                case .ops(let ops): self.toRenderer(ops: ops); self.opsSent += 1
                }
                self.diagnostics.nodes = nodes
            }
            if options.asyncValidation { toMain(deliver) } else { deliver() }
        }
    }

    public func engine(_ engine: SandboxEngine, ops json: String, canvas: [Int: WidgetCanvasBatch]) {
        let typed = rendererAcceptsTypedOps
        validate { [self] in
            let batch = validator.processOpsForRenderer(json, canvas: canvas, typed: typed)
            let nodes = validator.nodeCount
            let deliver = {
                switch batch {
                case .empty: break
                case .json(let clean): self.toRenderer("{\"type\":\"ops\",\"ops\":\(clean)}"); self.opsSent += 1
                case .ops(let ops): self.toRenderer(ops: ops); self.opsSent += 1
                }
                self.diagnostics.nodes = nodes
            }
            if options.asyncValidation { toMain(deliver) } else { deliver() }
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
            if options.asyncValidation { toMain(send) } else { send() }
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
            let send = { self.toRenderer("{\"type\":\"listen\",\"types\":\(types)}") }
            if options.asyncValidation { toMain(send) } else { send() }
        }
    }
    public func engine(_ engine: SandboxEngine, preventDefault json: String) {
        validate { [self] in
            let keys = validator.validatePreventDefault(json)
            let send = { self.toRenderer("{\"type\":\"preventDefault\",\"keys\":\(keys)}") }
            if options.asyncValidation { toMain(send) } else { send() }
        }
    }
    public func engineRequestsFrame(_ engine: SandboxEngine) {
        toMain { self.toRenderer("{\"type\":\"requestFrame\"}") }
    }
    public func engine(_ engine: SandboxEngine, log level: String, text: String) {
        toMain {
            if self.diagnostics.logs.count < 5000 { self.diagnostics.logs.append((level, text)) }
            if level == "error", text.hasPrefix("Uncaught ") {
                self.diagnostics.lastError = text
                self.tasksAtError = self.diagnostics.stats.tasks
                self.onError?(text)
            }
            self.changed()
        }
    }
    private var tasksAtError = 0
    public func engine(_ engine: SandboxEngine, diag kind: String, detail: String) {
        toMain { self.diag(kind, detail) }
    }
    public func engine(_ engine: SandboxEngine, bridge method: String, argsJson: String) {
        toMain {
            self.diagnostics.bridge.append((method, argsJson))
            self.onBridge?(method, argsJson)
            self.changed()
        }
    }
    public func engineLoaded(_ engine: SandboxEngine) {
        let loadedMs = Date().timeIntervalSince(createdAt) * 1000
        // Through the validation pipeline, so `loaded` is observed after the ops of the loading tasks were sent;
        // then one trusted query to the renderer: its answer comes after every size report those ops produced.
        validate { [self] in
            let deliver = {
                self.diagnostics.loaded = true
                self.diagnostics.loadedMs = loadedMs
                self.changed()
                self.toRenderer("{\"type\":\"query\",\"qid\":\(Self.renderAckQid),\"query\":{\"q\":\"viewport\"}}")
            }
            if options.asyncValidation { toMain(deliver) } else { deliver() }
        }
    }
    /// qid of the host's own post-load ack query; engine queries count up from 1.
    private static let renderAckQid = -1
    public func engine(_ engine: SandboxEngine, stats: SandboxStats) {
        toMain {
            self.diagnostics.stats = stats
            if self.diagnostics.lastError != nil, stats.tasks > self.tasksAtError + 1 { self.diagnostics.lastError = nil }
            self.changed()
        }
    }
    public func engine(_ engine: SandboxEngine, died reason: String) {
        toMain {
            if self.diagnostics.dead != nil { return }
            self.diagnostics.dead = reason
            self.changed()
            self.resolveQuery(json: "{\"error\":\"dead\"}", binary: nil)
            // The renderer stays: last render as fallback, badge from the renderer, no more events or frames.
            self.toRenderer(Self.json(["type": "frozen", "reason": self.stoppedLabel, "fatal": true]))
        }
    }

    /// Trusted script evaluated in the renderer (tests and diagnostics only); "null" from a renderer without script.
    public func evaluateInRenderer(_ script: String, completion: @escaping (String) -> Void) {
        renderer.evaluate(script) { completion($0 ?? "null") }
    }

    // MARK: renderer → host (main thread)

    private func onRenderer(_ json: String) {
        guard let msg = SandboxEngine.parse(json) as? [String: Any], let type = msg["type"] as? String else { return }
        switch type {
        case "ready":
            if rendererReady { return }
            rendererReady = true
            diagnostics.rendererReadyMs = Date().timeIntervalSince(createdAt) * 1000
            let queued = pendingToRenderer
            pendingToRenderer = []
            queued.forEach(renderer.send)
            if !engineStarted {
                engineStarted = true
                engine.start(html: html, config: config)
            }
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
                if !diagnostics.rendered {
                    diagnostics.rendered = true
                    diagnostics.renderedMs = Date().timeIntervalSince(createdAt) * 1000
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
            if diagnostics.firstContentMs == nil, opsSent > 0 {
                diagnostics.firstContentMs = Date().timeIntervalSince(createdAt) * 1000
                changed()
            }
            if height != diagnostics.height {
                diagnostics.height = height
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
        if destroyed { return }
        if rendererReady { renderer.send(json) } else { pendingToRenderer.append(json) }
    }

    private func toRenderer(ops: [WidgetOp]) {
        if destroyed { return }
        if rendererReady { renderer.send(ops: ops) } else { pendingToRenderer.append("{\"type\":\"ops\",\"ops\":\(WidgetOp.json(ops))}") }
    }

    private func diag(_ kind: String, _ detail: String) {
        var entry = diagnostics.diags[kind] ?? (0, [])
        entry.count += 1
        if entry.details.count < 50 { entry.details.append(detail) }
        diagnostics.diags[kind] = entry
        changed()
    }

    private var changeScheduled = false
    private func changed() {
        // Coalesce UI updates: a misbehaving widget can emit thousands of diagnostics per second.
        if changeScheduled { return }
        changeScheduled = true
        toMain {
            self.changeScheduled = false
            self.onChange?(self.diagnostics)
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
    /// Embeds already-serialized JSON inside a dictionary passed to `json(_:)`.
    private static func raw(_ json: String) -> Any { SandboxEngine.parse(json) ?? [] }
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
