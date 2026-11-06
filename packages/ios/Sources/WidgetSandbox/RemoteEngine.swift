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
    func remoteRequest(id: Int, data: String)
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

    public init(sink: SandboxRemoteSink, features: Set<String>, resources: SandboxResources = .shared) {
        self.sink = sink
        engine = SandboxEngine(resources: resources, features: features)
        engine.delegate = self
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

    public func engineTaskStarted(_ engine: SandboxEngine) { sink.remoteTaskStarted() }
    public func engineTaskCompleted(_ engine: SandboxEngine) { sink.remoteTaskCompleted() }
    public func engine(_ engine: SandboxEngine, ops json: String) { sink.remoteOps(json, canvas: [:]) }
    public func engine(_ engine: SandboxEngine, ops json: String, canvas: [Int: WidgetCanvasBatch]) { sink.remoteOps(json, canvas: canvas) }
    public func engine(_ engine: SandboxEngine, binaryOps payload: BinaryOpsPayload, canvas: [Int: WidgetCanvasBatch]) { sink.remoteBinaryOps(payload, canvas: canvas) }
    public func engine(_ engine: SandboxEngine, query json: String) -> (json: String, binary: Data?) { sink.remoteQuery(json) }
    public func engine(_ engine: SandboxEngine, listen json: String) { sink.remoteListen(json) }
    public func engine(_ engine: SandboxEngine, preventDefault json: String) { sink.remotePreventDefault(json) }
    public func engine(_ engine: SandboxEngine, anim json: String) { sink.remoteAnim(json) }
    public func engineRequestsFrame(_ engine: SandboxEngine) { sink.remoteRequestsFrame() }
    public func engine(_ engine: SandboxEngine, log level: String, text: String) { sink.remoteLog(level: level, text: text) }
    public func engine(_ engine: SandboxEngine, diag kind: String, detail: String) { sink.remoteDiag(kind: kind, detail: detail) }
    public func engine(_ engine: SandboxEngine, bridge method: String, argsJson: String) { sink.remoteBridge(method: method, argsJson: argsJson) }
    public func engine(_ engine: SandboxEngine, action: WidgetAction) {}
    public func engine(_ engine: SandboxEngine, copyText: String) { sink.remoteCopyText(copyText) }
    public func engine(_ engine: SandboxEngine, storageCommit: SdkPolicy.StorageCommit) { sink.remoteStorage(SandboxRemoteCoding.storageJson(storageCommit)) }
    public func engine(_ engine: SandboxEngine, resizeAnchor: WidgetResizeAnchor) { sink.remoteResizeAnchor(resizeAnchor.rawValue) }
    public func engine(_ engine: SandboxEngine, watchesPublicData: Bool) { sink.remoteWatchesPublicData(watchesPublicData) }
    public func engineLoaded(_ engine: SandboxEngine) { sink.remoteLoaded() }
    public func engine(_ engine: SandboxEngine, stats: SandboxStats) { sink.remoteStats(stats) }
    public func engine(_ engine: SandboxEngine, died reason: String) { sink.remoteDied(reason) }

    public func engine(_ engine: SandboxEngine, request: WidgetRequest, respond: WidgetRequestCompletion) {
        lock.lock()
        if closed {
            lock.unlock()
            return respond.unsupported()
        }
        nextRequest += 1
        let id = nextRequest
        pending[id] = respond
        lock.unlock()
        sink.remoteRequest(id: id, data: request.data)
    }
}

/// Both halves in one process, the engine's thread standing in for the other process: the protocol of a remote
/// engine without a transport (tests, labs and the parity check against an in-process engine).
public final class SandboxLoopbackChannel: SandboxRemoteChannel {
    private var server: SandboxRemoteServer?
    private let features: Set<String>
    private let resources: SandboxResources

    public init(features: Set<String>, resources: SandboxResources = .shared) {
        self.features = features
        self.resources = resources
    }

    public func attach(_ sink: SandboxRemoteSink) {
        server = SandboxRemoteServer(sink: sink, features: features, resources: resources)
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

    public static func encode(config: SandboxConfig) -> Data {
        var context: [String: Any] = [
            "isCopy": config.context.isCopy, "canQuery": config.context.canQuery,
            "backgroundQuery": config.context.backgroundQuery, "preview": config.context.preview,
        ]
        if let chatType = config.context.chatType { context["chatType"] = chatType.rawValue }
        if let viewer = config.context.viewer { context["viewer"] = viewer.json }
        let object: [String: Any] = [
            "memoryLimitBytes": config.memoryLimitBytes, "cpuLimitMs": config.cpuLimitMs, "cpuQuotaMs": config.cpuQuotaMs,
            "cpuQuotaWindowMs": config.cpuQuotaWindowMs, "cpuKillAfterMs": config.cpuKillAfterMs,
            "mainThreadQuotaMs": config.mainThreadQuotaMs, "maxTimers": config.maxTimers, "visible": config.visible,
            "innerWidth": config.innerWidth, "innerHeight": config.innerHeight, "devicePixelRatio": config.devicePixelRatio,
            "theme": config.theme, "features": Array(config.features).sorted(), "policy": config.policy.rawValue,
            "themeParams": config.themeParams, "locale": config.locale, "contentSafeArea": config.contentSafeArea.json,
            "storage": ["local": config.storage.local, "session": config.storage.session], "data": config.data.json,
            "context": context,
        ]
        return (try? JSONSerialization.data(withJSONObject: object)) ?? Data()
    }

    public static func decodeConfig(_ data: Data) -> SandboxConfig? {
        guard let o = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return nil }
        let number = { (key: String) -> Double? in (o[key] as? NSNumber)?.doubleValue }
        guard let width = number("innerWidth"), let height = number("innerHeight"), let dpr = number("devicePixelRatio"), let theme = o["theme"] as? String else { return nil }
        var config = SandboxConfig(innerWidth: width, innerHeight: height, devicePixelRatio: dpr, theme: theme)
        if let v = number("memoryLimitBytes") { config.memoryLimitBytes = Int(v) }
        if let v = number("cpuLimitMs") { config.cpuLimitMs = v }
        if let v = number("cpuQuotaMs") { config.cpuQuotaMs = v }
        if let v = number("cpuQuotaWindowMs") { config.cpuQuotaWindowMs = v }
        if let v = number("cpuKillAfterMs") { config.cpuKillAfterMs = v }
        if let v = number("mainThreadQuotaMs") { config.mainThreadQuotaMs = v }
        if let v = number("maxTimers") { config.maxTimers = Int(v) }
        if let v = o["visible"] as? Bool { config.visible = v }
        if let v = o["features"] as? [String] { config.features = Set(v) }
        if let v = (o["policy"] as? String).flatMap(SandboxPolicy.init(rawValue:)) { config.policy = v }
        if let v = o["themeParams"] as? [String: String] { config.themeParams = v }
        if let v = o["locale"] as? String { config.locale = v }
        if let v = o["contentSafeArea"] as? [String: Any] { config.contentSafeArea = safeArea(v) }
        if let v = o["storage"] as? [String: Any] {
            config.storage = WidgetStorage(local: v["local"] as? [String: String] ?? [:], session: v["session"] as? [String: String] ?? [:])
        }
        if let v = o["data"] as? [String: Any] { config.data = WidgetData(chatData: v["chatData"] as? String, publicData: v["publicData"] as? String) }
        if let c = o["context"] as? [String: Any] {
            var context = WidgetContext()
            context.isCopy = c["isCopy"] as? Bool ?? false
            context.canQuery = c["canQuery"] as? Bool ?? false
            context.backgroundQuery = c["backgroundQuery"] as? Bool ?? false
            context.preview = c["preview"] as? Bool ?? false
            context.chatType = (c["chatType"] as? String).flatMap(WidgetChatType.init(rawValue:))
            if let v = c["viewer"] as? [String: Any], let id = (v["id"] as? NSNumber)?.int64Value {
                context.viewer = WidgetViewer(id: id, firstName: v["first_name"] as? String, lastName: v["last_name"] as? String, username: v["username"] as? String, isPremium: v["is_premium"] as? Bool ?? false)
            }
            config.context = context
        }
        return config
    }

    public static func encode(safeArea: WidgetContentSafeArea) -> Data {
        (try? JSONSerialization.data(withJSONObject: safeArea.json)) ?? Data()
    }

    public static func decodeSafeArea(_ data: Data) -> WidgetContentSafeArea? {
        ((try? JSONSerialization.jsonObject(with: data)) as? [String: Any]).map(safeArea)
    }

    private static func safeArea(_ o: [String: Any]) -> WidgetContentSafeArea {
        let v = { (key: String) -> Double in (o[key] as? NSNumber)?.doubleValue ?? 0 }
        return WidgetContentSafeArea(top: v("top"), right: v("right"), bottom: v("bottom"), left: v("left"), cornerLeft: v("cornerLeft"), cornerRight: v("cornerRight"))
    }

    public static func encode(data: WidgetData) -> Data {
        (try? JSONSerialization.data(withJSONObject: data.json)) ?? Data()
    }

    public static func decodeData(_ data: Data) -> WidgetData? {
        guard let o = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return nil }
        return WidgetData(chatData: o["chatData"] as? String, publicData: o["publicData"] as? String)
    }

    public static func encode(outcome: SandboxRemoteOutcome) -> Data {
        let object: [String: Any]
        switch outcome {
        case .answer(let result): object = result.map { ["answer": $0] } ?? ["answer": NSNull()]
        case .failure(let failure): object = ["failure": failure.code.rawValue]
        case .unsupported: object = ["unsupported": true]
        }
        return (try? JSONSerialization.data(withJSONObject: object)) ?? Data()
    }

    public static func decodeOutcome(_ data: Data) -> SandboxRemoteOutcome? {
        guard let o = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return nil }
        if o["unsupported"] != nil { return .unsupported }
        if let code = o["failure"] as? String {
            switch SdkPolicy.RequestError(rawValue: code) {
            case .timeout: return .failure(.timeout)
            case .rateLimited: return .failure(.rateLimited)
            case .failed: return .failure(.failed)
            default: return .failure(.notAllowed)
            }
        }
        if let answer = o["answer"] { return .answer(answer as? String) }
        return nil
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
