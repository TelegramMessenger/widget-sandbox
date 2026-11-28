// An engine in another process (on macOS an XPC service without network or file access): the host keeps a
// client-mode `SandboxEngine` (`SandboxEngine(remote:features:)`) whose inputs go out through a
// `SandboxRemoteChannel`; the engine's process runs a `SandboxRemoteServer` around a real `SandboxEngine` and sends
// everything its delegate hears back through a `SandboxRemoteSink`.
//
// The sink side is as hostile as the widget: a widget that escaped QuickJS owns the engine's process and can say
// anything. So the client-mode engine re-decides every privileged call with the host's own inputs (the gestures it
// sent, its visibility, the presses, the message's flags): a link, copyText or a request needs a tap the host
// delivered, a haptic passes the host's own gate, storage and bridge calls are checked again with `SdkPolicy`; ops,
// queries, listen, preventDefault and anim go to the host's validator as they always did. The honest engine still
// applies its exact rules first, so a widget sees the same behaviour as in process.
//
// Order: the channel delivers the sink's calls in the order the engine made them; the client engine hands them to its
// delegate on its own queue in that order (a query waits for everything before it).

import Foundation
import WidgetSandboxProtocol

public enum SandboxRemoteOutcome: Equatable {
    case answer(String?)
    case failure(WidgetRequestFailure)
    case unsupported

    var completion: WidgetRequestCompletion.Outcome {
        switch self {
        case .answer(let result): return .answer(.success(WidgetRequestAnswer(result: result)))
        case .failure(let failure): return .answer(.failure(failure))
        case .unsupported: return .unsupported
        }
    }

    init(_ outcome: WidgetRequestCompletion.Outcome) {
        switch outcome {
        case .answer(.success(let answer)): self = .answer(answer.result)
        case .answer(.failure(let failure)): self = .failure(failure)
        case .unsupported: self = .unsupported
        }
    }
}

/// Host → engine, for a client-mode engine. Calls come in the host's order from the engine's queue; a transport keeps
/// that order and never blocks on the engine's process (everything here is asynchronous).
public protocol SandboxRemoteChannel: AnyObject {
    func attach(_ sink: SandboxRemoteSink)
    func prewarm(features: Set<String>)
    func start(html: String, config: SandboxConfig)
    func event(_ json: String)
    func activationEvent(_ json: String, gesture: Int)
    func frame(_ time: Double)
    func viewport(innerWidth: Double, innerHeight: Double, dpr: Double)
    func resize(_ entriesJson: String)
    func theme(_ theme: String, params: [String: String])
    func contentSafeArea(_ area: WidgetContentSafeArea)
    func setData(_ data: WidgetData)
    func setVisible(_ visible: Bool)
    func pressBegan()
    func pressEnded()
    func stop(reason: String)
    func answer(request id: Int, outcome: SandboxRemoteOutcome)
    func morph(html: String, completion: @escaping (Bool) -> Void)
    func evaluateTrusted(_ code: String, completion: @escaping (String?) -> Void)
    func collectGarbage()
    func close()
}

/// Engine → host: what the remote engine's delegate heard, in order. Untrusted.
public protocol SandboxRemoteSink: AnyObject {
    func remoteTaskStarted()
    func remoteTaskCompleted()
    func remoteOps(_ json: String, canvas: [Int: WidgetCanvasBatch])
    func remoteBinaryOps(_ payload: BinaryOpsPayload, canvas: [Int: WidgetCanvasBatch])
    func remoteQuery(_ json: String) -> (json: String, binary: Data?)
    func remoteListen(_ json: String)
    func remotePreventDefault(_ json: String)
    func remoteAnim(_ json: String)
    func remoteRequestsFrame()
    func remoteLog(level: String, text: String)
    func remoteDiag(kind: String, detail: String)
    func remoteBridge(method: String, argsJson: String)
    func remoteCopyText(_ text: String)
    func remoteRequest(id: Int, data: String, wallet: Bool)
    func remoteWallet(id: Int, payload: Data)
    func remoteStorage(_ commitJson: String)
    func remoteResizeAnchor(_ edge: String)
    func remoteWatchesPublicData(_ on: Bool)
    func remoteLoaded()
    func remoteStats(_ stats: SandboxStats)
    func remoteDied(_ reason: String)
    func remoteDisconnected()
}

/// The engine's side: one real engine for one client-mode engine, its delegate relayed to `sink`.
public final class SandboxRemoteServer: SandboxEngineDelegate {
    public let engine: SandboxEngine
    private let sink: SandboxRemoteSink
    private let lock = NSLock()
    private var nextRequest = 0
    private var pending: [Int: WidgetRequestCompletion] = [:]
    private var closed = false
    private var taskDepth = 0
    private var announcedDepth = 0

    public init(sink: SandboxRemoteSink, features: Set<String>, resources: SandboxResources = .shared, backend: QuickJSBackend = .default) {
        self.sink = sink
        engine = SandboxEngine(resources: resources, features: features, backend: backend)
        engine.delegate = self
        #if DEBUG
        engine.queue.async { WidgetParseProbe.markServiceThread() }
        #endif
    }

    public func prewarm() { engine.prewarm() }
    public func start(html: String, config: SandboxConfig) { engine.start(html: html, config: config) }
    public func event(_ json: String) { engine.event(json: json) }
    public func activationEvent(_ json: String, gesture: Int) { engine.activationEvent(json: json, gesture: gesture) }
    public func frame(_ time: Double) { engine.frame(time: time) }
    public func viewport(innerWidth: Double, innerHeight: Double, dpr: Double) { engine.viewport(innerWidth: innerWidth, innerHeight: innerHeight, dpr: dpr) }
    public func resize(_ entriesJson: String) { engine.resize(entriesJson: entriesJson) }
    public func theme(_ theme: String, params: [String: String]) { engine.theme(theme, params: params) }
    public func contentSafeArea(_ area: WidgetContentSafeArea) { engine.contentSafeArea(area) }
    public func setData(_ data: WidgetData) { engine.setData(data) }
    public func setVisible(_ visible: Bool) { engine.setVisible(visible) }
    public func pressBegan() { engine.pressBegan() }
    public func pressEnded() { engine.pressEnded() }
    public func stop(reason: String) { engine.stop(reason: reason) }
    public func morph(html: String, completion: @escaping (Bool) -> Void) { engine.morph(html: html, completion: completion) }
    public func evaluateTrusted(_ code: String, completion: @escaping (String?) -> Void) { engine.evaluateTrusted(code, completion: completion) }
    public func collectGarbage() { engine.collectGarbage() }

    public func answer(request id: Int, outcome: SandboxRemoteOutcome) {
        lock.lock()
        let respond = pending.removeValue(forKey: id)
        lock.unlock()
        respond?.settle(outcome.completion)
    }

    /// The client is gone: the widget dies with its connection, its requests are refused.
    public func close() {
        lock.lock()
        closed = true
        let open = pending
        pending.removeAll()
        lock.unlock()
        open.values.forEach { $0.unsupported() }
        engine.terminate()
    }

    public func engineTaskStarted(_ engine: SandboxEngine) { taskDepth += 1 }
    public func engineTaskCompleted(_ engine: SandboxEngine) {
        guard taskDepth > 0 else { return }
        defer { taskDepth -= 1 }
        guard announcedDepth == taskDepth else { return }
        announcedDepth -= 1
        sink.remoteTaskCompleted()
    }
    public func engine(_ engine: SandboxEngine, ops json: String) { announce(); sink.remoteOps(json, canvas: [:]) }
    public func engine(_ engine: SandboxEngine, ops json: String, canvas: [Int: WidgetCanvasBatch]) { announce(); sink.remoteOps(json, canvas: canvas) }
    public func engine(_ engine: SandboxEngine, binaryOps payload: BinaryOpsPayload, canvas: [Int: WidgetCanvasBatch]) { announce(); sink.remoteBinaryOps(payload, canvas: canvas) }
    public func engine(_ engine: SandboxEngine, query json: String) -> (json: String, binary: Data?) {
        announce()
        return sink.remoteQuery(json)
    }
    public func engine(_ engine: SandboxEngine, listen json: String) { announce(); sink.remoteListen(json) }
    public func engine(_ engine: SandboxEngine, preventDefault json: String) { announce(); sink.remotePreventDefault(json) }
    public func engine(_ engine: SandboxEngine, anim json: String) { announce(); sink.remoteAnim(json) }
    public func engineRequestsFrame(_ engine: SandboxEngine) { announce(); sink.remoteRequestsFrame() }
    public func engine(_ engine: SandboxEngine, log level: String, text: String) { announce(); sink.remoteLog(level: level, text: text) }
    public func engine(_ engine: SandboxEngine, diag kind: String, detail: String) { announce(); sink.remoteDiag(kind: kind, detail: detail) }
    public func engine(_ engine: SandboxEngine, bridge method: String, argsJson: String) { announce(); sink.remoteBridge(method: method, argsJson: argsJson) }
    public func engine(_ engine: SandboxEngine, action: WidgetAction) {}
    public func engine(_ engine: SandboxEngine, copyText: String) { announce(); sink.remoteCopyText(copyText) }
    public func engine(_ engine: SandboxEngine, storageCommit: SdkPolicy.StorageCommit) { announce(); sink.remoteStorage(SandboxRemoteCoding.storageJson(storageCommit)) }
    public func engine(_ engine: SandboxEngine, resizeAnchor: WidgetResizeAnchor) { announce(); sink.remoteResizeAnchor(resizeAnchor.rawValue) }
    public func engine(_ engine: SandboxEngine, watchesPublicData: Bool) { announce(); sink.remoteWatchesPublicData(watchesPublicData) }
    public func engineLoaded(_ engine: SandboxEngine) { announce(); sink.remoteLoaded() }
    public func engine(_ engine: SandboxEngine, stats: SandboxStats) { announce(); sink.remoteStats(stats) }
    public func engine(_ engine: SandboxEngine, died reason: String) { announce(); sink.remoteDied(reason) }

    public func engine(_ engine: SandboxEngine, request: WidgetRequest, respond: WidgetRequestCompletion) {
        announce()
        forward(respond) { sink.remoteRequest(id: $0, data: request.data, wallet: request.wallet) }
    }

    public func engine(_ engine: SandboxEngine, wallet: WalletCall, respond: WidgetRequestCompletion) {
        announce()
        forward(respond) { sink.remoteWallet(id: $0, payload: SandboxRemoteCoding.encode(wallet: wallet)) }
    }

    /// A task's boundaries reach the client with its first callback: a task that says nothing (a timer that changes
    /// nothing) costs the client no callbacks.
    private func announce() {
        while announcedDepth < taskDepth {
            announcedDepth += 1
            sink.remoteTaskStarted()
        }
    }

    private func forward(_ respond: WidgetRequestCompletion, send: (Int) -> Void) {
        lock.lock()
        if closed {
            lock.unlock()
            return respond.unsupported()
        }
        nextRequest += 1
        let id = nextRequest
        pending[id] = respond
        lock.unlock()
        send(id)
    }
}

/// Both halves in one process, the engine's thread standing in for the other process: the protocol of a remote
/// engine without a transport (tests, labs and the parity check against an in-process engine).
public final class SandboxLoopbackChannel: SandboxRemoteChannel {
    private var server: SandboxRemoteServer?
    private let features: Set<String>
    private let resources: SandboxResources
    private let backend: QuickJSBackend

    public init(features: Set<String>, resources: SandboxResources = .shared, backend: QuickJSBackend = .default) {
        self.features = features
        self.resources = resources
        self.backend = backend
    }

    public func attach(_ sink: SandboxRemoteSink) {
        server = SandboxRemoteServer(sink: sink, features: features, resources: resources, backend: backend)
    }

    public var serverEngine: SandboxEngine? { server?.engine }

    public func prewarm(features: Set<String>) { server?.prewarm() }
    public func start(html: String, config: SandboxConfig) { server?.start(html: html, config: config) }
    public func event(_ json: String) { server?.event(json) }
    public func activationEvent(_ json: String, gesture: Int) { server?.activationEvent(json, gesture: gesture) }
    public func frame(_ time: Double) { server?.frame(time) }
    public func viewport(innerWidth: Double, innerHeight: Double, dpr: Double) { server?.viewport(innerWidth: innerWidth, innerHeight: innerHeight, dpr: dpr) }
    public func resize(_ entriesJson: String) { server?.resize(entriesJson) }
    public func theme(_ theme: String, params: [String: String]) { server?.theme(theme, params: params) }
    public func contentSafeArea(_ area: WidgetContentSafeArea) { server?.contentSafeArea(area) }
    public func setData(_ data: WidgetData) { server?.setData(data) }
    public func setVisible(_ visible: Bool) { server?.setVisible(visible) }
    public func pressBegan() { server?.pressBegan() }
    public func pressEnded() { server?.pressEnded() }
    public func stop(reason: String) { server?.stop(reason: reason) }
    public func answer(request id: Int, outcome: SandboxRemoteOutcome) { server?.answer(request: id, outcome: outcome) }
    public func morph(html: String, completion: @escaping (Bool) -> Void) {
        guard let server else { return completion(false) }
        server.morph(html: html, completion: completion)
    }
    public func evaluateTrusted(_ code: String, completion: @escaping (String?) -> Void) {
        guard let server else { return completion(nil) }
        server.evaluateTrusted(code, completion: completion)
    }
    public func collectGarbage() { server?.collectGarbage() }

    public func close() {
        server?.close()
        server = nil
    }
}

/// Wire forms for transports that cross a process boundary. Decoders take untrusted bytes: every count is capped
/// and a malformed value is nil, never a crash.
@objc(WidgetSandboxConfigWire)
public final class SandboxConfigWire: NSObject, NSSecureCoding {
    public static var supportsSecureCoding: Bool { true }

    public let config: SandboxConfig

    public init(_ config: SandboxConfig) {
        self.config = config
    }

    public func encode(with coder: NSCoder) {
        coder.encode(config.memoryLimitBytes, forKey: "memoryLimitBytes")
        coder.encode(config.cpuLimitMs, forKey: "cpuLimitMs")
        coder.encode(config.cpuQuotaMs, forKey: "cpuQuotaMs")
        coder.encode(config.cpuQuotaWindowMs, forKey: "cpuQuotaWindowMs")
        coder.encode(config.cpuKillAfterMs, forKey: "cpuKillAfterMs")
        coder.encode(config.mainThreadQuotaMs, forKey: "mainThreadQuotaMs")
        coder.encode(config.maxTimers, forKey: "maxTimers")
        coder.encode(config.visible, forKey: "visible")
        coder.encode(config.innerWidth, forKey: "innerWidth")
        coder.encode(config.innerHeight, forKey: "innerHeight")
        coder.encode(config.devicePixelRatio, forKey: "devicePixelRatio")
        coder.encode(config.theme as NSString, forKey: "theme")
        coder.encode(Array(config.features).sorted() as NSArray, forKey: "features")
        coder.encode(config.policy.rawValue as NSString, forKey: "policy")
        coder.encode(config.themeParams as NSDictionary, forKey: "themeParams")
        coder.encode(config.locale as NSString, forKey: "locale")
        coder.encode(SandboxRemoteCoding.values(safeArea: config.contentSafeArea) as NSArray, forKey: "contentSafeArea")
        coder.encode(config.storage.local as NSDictionary, forKey: "storageLocal")
        coder.encode(config.storage.session as NSDictionary, forKey: "storageSession")
        coder.encode(config.data.chatData.map { $0 as NSString }, forKey: "chatData")
        coder.encode(config.data.publicData.map { $0 as NSString }, forKey: "publicData")
        let context = config.context
        coder.encode(context.isCopy, forKey: "isCopy")
        coder.encode(context.canQuery, forKey: "canQuery")
        coder.encode(context.backgroundQuery, forKey: "backgroundQuery")
        coder.encode(context.preview, forKey: "preview")
        coder.encode(context.wallet, forKey: "wallet")
        coder.encode(context.surface.rawValue as NSString, forKey: "surface")
        coder.encode(context.chatType.map { $0.rawValue as NSString }, forKey: "chatType")
        if let viewer = context.viewer {
            coder.encode(true, forKey: "hasViewer")
            coder.encode(viewer.id, forKey: "viewerId")
            coder.encode(viewer.firstName.map { $0 as NSString }, forKey: "viewerFirstName")
            coder.encode(viewer.lastName.map { $0 as NSString }, forKey: "viewerLastName")
            coder.encode(viewer.username.map { $0 as NSString }, forKey: "viewerUsername")
            coder.encode(viewer.isPremium, forKey: "viewerIsPremium")
        }
    }

    public init?(coder: NSCoder) {
        func text(_ key: String, limit: Int) -> String?? {
            let value = coder.decodeObject(of: NSString.self, forKey: key) as String?
            guard coder.error == nil else { return nil }
            guard let value else { return .some(nil) }
            guard value.utf8.count <= limit else { return nil }
            return .some(value)
        }
        func strings(_ key: String) -> [String: String]? {
            guard let value = coder.decodeObject(of: [NSDictionary.self, NSString.self], forKey: key) as? [String: String] else { return nil }
            guard value.count <= 4096, value.allSatisfy({ $0.key.utf8.count <= 4096 && $0.value.utf8.count <= 1 << 20 }) else { return nil }
            return value
        }
        let width = coder.decodeDouble(forKey: "innerWidth")
        let height = coder.decodeDouble(forKey: "innerHeight")
        let dpr = coder.decodeDouble(forKey: "devicePixelRatio")
        guard width.isFinite, height.isFinite, dpr.isFinite, width > 0, width <= 4096, height > 0, height <= 16384, dpr >= 1, dpr <= 4,
              let theme = coder.decodeObject(of: NSString.self, forKey: "theme") as String?, theme.utf8.count <= 64 else { return nil }
        var config = SandboxConfig(innerWidth: width, innerHeight: height, devicePixelRatio: dpr, theme: theme)
        let doubles = ["cpuLimitMs", "cpuQuotaMs", "cpuQuotaWindowMs", "cpuKillAfterMs", "mainThreadQuotaMs"].map { coder.decodeDouble(forKey: $0) }
        guard doubles.allSatisfy({ $0.isFinite && $0 >= 0 && $0 <= 1e9 }) else { return nil }
        config.cpuLimitMs = doubles[0]
        config.cpuQuotaMs = doubles[1]
        config.cpuQuotaWindowMs = doubles[2]
        config.cpuKillAfterMs = doubles[3]
        config.mainThreadQuotaMs = doubles[4]
        let memory = coder.decodeInteger(forKey: "memoryLimitBytes")
        let timers = coder.decodeInteger(forKey: "maxTimers")
        guard (1...(512 << 20)).contains(memory), (0...4096).contains(timers) else { return nil }
        config.memoryLimitBytes = memory
        config.maxTimers = timers
        config.visible = coder.decodeBool(forKey: "visible")
        guard let features = coder.decodeObject(of: [NSArray.self, NSString.self], forKey: "features") as? [String], features.count <= 256,
              let policy = (coder.decodeObject(of: NSString.self, forKey: "policy") as String?).flatMap(SandboxPolicy.init(rawValue:)),
              let themeParams = strings("themeParams"),
              let locale = coder.decodeObject(of: NSString.self, forKey: "locale") as String?, locale.utf8.count <= 128,
              let area = (coder.decodeObject(of: [NSArray.self, NSNumber.self], forKey: "contentSafeArea") as? [NSNumber]).flatMap({ SandboxRemoteCoding.safeArea($0.map(\.doubleValue)) }),
              let local = strings("storageLocal"), let session = strings("storageSession"),
              let chatData = text("chatData", limit: 1 << 20), let publicData = text("publicData", limit: 1 << 20),
              let surface = (coder.decodeObject(of: NSString.self, forKey: "surface") as String?).flatMap(WidgetSurface.init(rawValue:)),
              let chatType = text("chatType", limit: 64) else { return nil }
        config.features = Set(features)
        config.policy = policy
        config.themeParams = themeParams
        config.locale = locale
        config.contentSafeArea = area
        config.storage = WidgetStorage(local: local, session: session)
        config.data = WidgetData(chatData: chatData, publicData: publicData)
        var context = WidgetContext()
        context.isCopy = coder.decodeBool(forKey: "isCopy")
        context.canQuery = coder.decodeBool(forKey: "canQuery")
        context.backgroundQuery = coder.decodeBool(forKey: "backgroundQuery")
        context.preview = coder.decodeBool(forKey: "preview")
        context.wallet = coder.decodeBool(forKey: "wallet")
        context.surface = surface
        if let chatType {
            guard let type = WidgetChatType(rawValue: chatType) else { return nil }
            context.chatType = type
        }
        if coder.decodeBool(forKey: "hasViewer") {
            guard let first = text("viewerFirstName", limit: 4096), let last = text("viewerLastName", limit: 4096),
                  let username = text("viewerUsername", limit: 4096) else { return nil }
            context.viewer = WidgetViewer(id: coder.decodeInt64(forKey: "viewerId"), firstName: first, lastName: last, username: username,
                                          isPremium: coder.decodeBool(forKey: "viewerIsPremium"))
        }
        config.context = context
        self.config = config
    }
}

public enum SandboxRemoteCoding {
    public static let maxCanvasWords = 4 << 20
    public static let maxCanvasStrings = 1 << 17
    public static let maxCanvasStringBytes = 32 << 20
    public static let maxCanvasBatches = 4096

    static func storageJson(_ commit: SdkPolicy.StorageCommit) -> String {
        var out: [String: Any] = [:]
        if let local = commit.local { out["local"] = local }
        if let session = commit.session { out["session"] = session }
        return SandboxEngine.json(out)
    }

    public static func values(safeArea area: WidgetContentSafeArea) -> [Double] {
        [area.top, area.right, area.bottom, area.left, area.cornerLeft, area.cornerRight]
    }

    public static func safeArea(_ values: [Double]) -> WidgetContentSafeArea? {
        guard values.count == 6, values.allSatisfy({ $0.isFinite }) else { return nil }
        return WidgetContentSafeArea(top: values[0], right: values[1], bottom: values[2], left: values[3], cornerLeft: values[4], cornerRight: values[5])
    }

    public static func encode(wallet: WalletCall) -> Data {
        let payload: [String: Any]
        switch wallet {
        case let .sendTransaction(transaction, _): payload = transaction.json
        case let .signData(data, _): payload = data.json
        }
        return (try? JSONSerialization.data(withJSONObject: [1, wallet.method, payload, wallet.data])) ?? Data()
    }

    static func decodeWallet(_ data: Data) -> WalletCall? {
        #if DEBUG
        WidgetParseProbe.hit("SandboxRemoteCoding.decodeWallet")
        #endif
        guard data.count <= SandboxEngine.remoteMessageLimit,
              let args = try? JSONSerialization.jsonObject(with: data),
              case let .wallet(_, call)? = SdkPolicy.checkBridgeCall("wallet", args) else { return nil }
        return call
    }

    public static func wire(outcome: SandboxRemoteOutcome) -> (answered: Bool, result: String?, failure: String?) {
        switch outcome {
        case .answer(let result): return (true, result, nil)
        case .failure(let failure): return (false, nil, failure.code.rawValue)
        case .unsupported: return (false, nil, nil)
        }
    }

    public static func outcome(answered: Bool, result: String?, failure: String?) -> SandboxRemoteOutcome? {
        if answered {
            guard failure == nil, (result?.utf8.count ?? 0) <= SandboxEngine.remoteMessageLimit else { return nil }
            return .answer(result)
        }
        guard result == nil else { return nil }
        guard let code = failure else { return .unsupported }
        switch SdkPolicy.RequestError(rawValue: code) {
        case .timeout: return .failure(.timeout)
        case .rateLimited: return .failure(.rateLimited)
        case .failed: return .failure(.failed)
        case .rejected: return .failure(.rejected)
        case .badRequest: return .failure(.badRequest)
        default: return .failure(.notAllowed)
        }
    }

    public static func encode(stats: SandboxStats) -> [Double] {
        [stats.jsMs, Double(stats.tasks), Double(stats.memoryBytes), Double(stats.ops), Double(stats.queries), stats.queryMs,
         Double(stats.queryCacheHits), Double(stats.missedMutations), Double(stats.throttledTasks), stats.cpuWindowMs]
    }

    public static func decodeStats(_ values: [Double]) -> SandboxStats? {
        guard values.count == 10 else { return nil }
        let v = values.map { $0.isFinite ? min(max($0, 0), 1e15) : 0 }
        var s = SandboxStats()
        s.jsMs = v[0]; s.tasks = Int(v[1]); s.memoryBytes = Int(v[2]); s.ops = Int(v[3]); s.queries = Int(v[4])
        s.queryMs = v[5]; s.queryCacheHits = Int(v[6]); s.missedMutations = Int(v[7]); s.throttledTasks = Int(v[8]); s.cpuWindowMs = v[9]
        return s
    }

    public static func encode(binary payload: BinaryOpsPayload) -> (words: Data, strings: Data) {
        (payload.words.withUnsafeBufferPointer { Data(buffer: $0) }, Data(payload.strings))
    }

    public static func decodeBinary(words: Data, strings: Data) -> BinaryOpsPayload? {
        guard words.count % 4 == 0, words.count / 4 <= BinaryOps.maxWords, strings.count <= BinaryOps.maxStringBytes else { return nil }
        let count = words.count / 4
        let decoded = [Int32](unsafeUninitializedCapacity: count) { buffer, initialized in
            _ = words.copyBytes(to: UnsafeMutableRawBufferPointer(buffer))
            initialized = count
        }
        return BinaryOpsPayload(words: decoded, strings: [UInt8](strings))
    }

    /// `[u32 batches] { [i64 key][u32 words][f64 …][u32 strings]{[u32 bytes][utf8]} }`, little-endian.
    public static func encode(canvas: [Int: WidgetCanvasBatch]) -> Data {
        var out = Data()
        func u32(_ v: Int) { var x = UInt32(truncatingIfNeeded: v).littleEndian; withUnsafeBytes(of: &x) { out.append(contentsOf: $0) } }
        func i64(_ v: Int) { var x = Int64(v).littleEndian; withUnsafeBytes(of: &x) { out.append(contentsOf: $0) } }
        u32(canvas.count)
        for (key, batch) in canvas.sorted(by: { $0.key < $1.key }) {
            i64(key)
            u32(batch.words.count)
            for w in batch.words { var x = w.bitPattern.littleEndian; withUnsafeBytes(of: &x) { out.append(contentsOf: $0) } }
            u32(batch.strings.count)
            for s in batch.strings {
                let bytes = Array(s.utf8)
                u32(bytes.count)
                out.append(contentsOf: bytes)
            }
        }
        return out
    }

    public static func decodeCanvas(_ data: Data) -> [Int: WidgetCanvasBatch]? {
        let maxBytes = 4 + 16 * maxCanvasBatches + 8 * maxCanvasWords + 4 * maxCanvasStrings + maxCanvasStringBytes
        guard data.count <= maxBytes else { return nil }
        let bytes = [UInt8](data)
        var i = 0
        func u32() -> Int? {
            guard bytes.count - i >= 4 else { return nil }
            var v: UInt32 = 0
            for k in 0 ..< 4 { v |= UInt32(bytes[i + k]) << (8 * UInt32(k)) }
            i += 4
            return Int(v)
        }
        func u64() -> UInt64? {
            guard bytes.count - i >= 8 else { return nil }
            var v: UInt64 = 0
            for k in 0 ..< 8 { v |= UInt64(bytes[i + k]) << (8 * UInt64(k)) }
            i += 8
            return v
        }
        guard let batches = u32(), batches <= maxCanvasBatches else { return nil }
        var out: [Int: WidgetCanvasBatch] = [:]
        var totalWords = 0, totalStrings = 0, totalBytes = 0
        for _ in 0 ..< batches {
            guard let key = u64(), let wordCount = u32() else { return nil }
            totalWords += wordCount
            guard totalWords <= maxCanvasWords, bytes.count - i >= wordCount * 8 else { return nil }
            var words: [Double] = []
            words.reserveCapacity(wordCount)
            for _ in 0 ..< wordCount {
                guard let bits = u64() else { return nil }
                words.append(Double(bitPattern: bits))
            }
            guard let stringCount = u32() else { return nil }
            totalStrings += stringCount
            guard totalStrings <= maxCanvasStrings else { return nil }
            var strings: [String] = []
            strings.reserveCapacity(min(stringCount, 1024))
            for _ in 0 ..< stringCount {
                guard let length = u32(), bytes.count - i >= length else { return nil }
                totalBytes += length
                guard totalBytes <= maxCanvasStringBytes, let s = String(bytes: bytes[i ..< i + length], encoding: .utf8) else { return nil }
                i += length
                strings.append(s)
            }
            out[Int(Int64(bitPattern: key))] = WidgetCanvasBatch(words: words, strings: strings)
        }
        return i == bytes.count ? out : nil
    }
}
