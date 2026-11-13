import XCTest
import WidgetSandboxProtocol
@testable import WidgetSandbox

/// A client-mode engine whose remote side is the test: every privileged call is forged, so only the host's own
/// gates (taps it delivered, visibility, the message's flags) decide.
final class RemoteEngineTests: XCTestCase {
    private final class Channel: SandboxRemoteChannel {
        var sink: SandboxRemoteSink?
        var calls: [String] = []
        var answers: [(Int, SandboxRemoteOutcome)] = []
        var closed = false
        func attach(_ sink: SandboxRemoteSink) { self.sink = sink }
        func prewarm(features: Set<String>) { calls.append("prewarm") }
        func start(html: String, config: SandboxConfig) { calls.append("start") }
        func event(_ json: String) { calls.append("event") }
        func activationEvent(_ json: String, gesture: Int) { calls.append("activation:\(gesture)") }
        func frame(_ time: Double) { calls.append("frame") }
        func viewport(innerWidth: Double, innerHeight: Double, dpr: Double) { calls.append("viewport") }
        func resize(_ entriesJson: String) { calls.append("resize") }
        func theme(_ theme: String, params: [String: String]) { calls.append("theme") }
        func contentSafeArea(_ area: WidgetContentSafeArea) { calls.append("safeArea") }
        func setData(_ data: WidgetData) { calls.append("data") }
        func setVisible(_ visible: Bool) { calls.append("visible:\(visible)") }
        func pressBegan() { calls.append("press") }
        func pressEnded() { calls.append("release") }
        func stop(reason: String) { calls.append("stop:\(reason)") }
        func answer(request id: Int, outcome: SandboxRemoteOutcome) { answers.append((id, outcome)) }
        func morph(html: String, completion: @escaping (Bool) -> Void) { completion(true) }
        func evaluateTrusted(_ code: String, completion: @escaping (String?) -> Void) { completion("{\"ok\":true}") }
        func collectGarbage() {}
        func close() { closed = true }
    }

    private final class Delegate: SandboxEngineDelegate {
        var actions: [WidgetAction] = []
        var copies: [String] = []
        var requests: [(WidgetRequest, WidgetRequestCompletion)] = []
        var walletCalls: [(WalletCall, WidgetRequestCompletion)] = []
        var commits: [SdkPolicy.StorageCommit] = []
        var diags: [String] = []
        var died: [String] = []
        var order: [String] = []
        var anchors: [WidgetResizeAnchor] = []
        var watches: [Bool] = []
        func engine(_ engine: SandboxEngine, ops json: String) { order.append("ops") }
        func engine(_ engine: SandboxEngine, query json: String) -> (json: String, binary: Data?) { order.append("query"); return ("{}", nil) }
        func engine(_ engine: SandboxEngine, listen json: String) {}
        func engine(_ engine: SandboxEngine, preventDefault json: String) {}
        func engineRequestsFrame(_ engine: SandboxEngine) { order.append("frame") }
        func engine(_ engine: SandboxEngine, log level: String, text: String) { order.append("log:\(level):\(text.count)") }
        func engine(_ engine: SandboxEngine, diag kind: String, detail: String) { diags.append(kind) }
        func engine(_ engine: SandboxEngine, bridge method: String, argsJson: String) {}
        func engine(_ engine: SandboxEngine, action: WidgetAction) { actions.append(action) }
        func engine(_ engine: SandboxEngine, copyText: String) { copies.append(copyText) }
        func engine(_ engine: SandboxEngine, request: WidgetRequest, respond: WidgetRequestCompletion) { requests.append((request, respond)) }
        func engine(_ engine: SandboxEngine, wallet: WalletCall, respond: WidgetRequestCompletion) { walletCalls.append((wallet, respond)) }
        func engine(_ engine: SandboxEngine, storageCommit: SdkPolicy.StorageCommit) { commits.append(storageCommit) }
        func engine(_ engine: SandboxEngine, resizeAnchor: WidgetResizeAnchor) { anchors.append(resizeAnchor) }
        func engine(_ engine: SandboxEngine, watchesPublicData: Bool) { watches.append(watchesPublicData) }
        func engineLoaded(_ engine: SandboxEngine) {}
        func engine(_ engine: SandboxEngine, stats: SandboxStats) {}
        func engine(_ engine: SandboxEngine, died reason: String) { died.append(reason) }
    }

    private var channel: Channel!
    private var engine: SandboxEngine!
    private var delegate: Delegate!

    private func make(context: WidgetContext = WidgetContext(canQuery: true), policy: SandboxPolicy = .strict) {
        channel = Channel()
        engine = SandboxEngine(remote: channel, features: [])
        delegate = Delegate()
        engine.delegate = delegate
        var config = SandboxConfig(innerWidth: 300, innerHeight: 200, devicePixelRatio: 2, theme: "light")
        config.context = context
        config.policy = policy
        engine.start(html: "<p>x</p>", config: config)
        settle()
    }

    private func settle() {
        _ = engine.queue.sync { true }
    }

    private var sink: SandboxRemoteSink { channel.sink! }

    private func tap(_ gesture: Int) {
        engine.activationEvent(json: "{\"type\":\"click\"}", gesture: gesture)
        settle()
    }

    func testInputsGoToTheChannelInOrder() {
        make()
        engine.event(json: "{}")
        engine.frame(time: 1)
        engine.viewport(innerWidth: 1, innerHeight: 2, dpr: 2)
        engine.theme("dark")
        engine.setVisible(false)
        engine.setVisible(true)
        engine.pressBegan()
        engine.pressEnded()
        settle()
        XCTAssertEqual(channel.calls, ["start", "event", "frame", "viewport", "theme", "visible:false", "visible:true", "press", "release"])
    }

    func testForgedLinkWithoutATapIsRefused() {
        make()
        sink.remoteBridge(method: "openLink", argsJson: "[\"https://example.com/\"]")
        settle()
        XCTAssertTrue(delegate.actions.isEmpty)
        XCTAssertEqual(delegate.diags, ["activation-blocked"])
    }

    func testOneTapAllowsOnePrivilegedCall() {
        make()
        tap(1)
        sink.remoteBridge(method: "openLink", argsJson: "[\"https://example.com/\"]")
        sink.remoteBridge(method: "openLink", argsJson: "[\"https://example.com/again\"]")
        sink.remoteCopyText("secret")
        settle()
        XCTAssertEqual(delegate.actions, [.openLink(URL(string: "https://example.com/")!)])
        XCTAssertTrue(delegate.copies.isEmpty)
        tap(2)
        sink.remoteCopyText("ok")
        settle()
        XCTAssertEqual(delegate.copies, ["ok"])
    }

    func testEachTapAllowsOneCallEvenWhenTheCallsArriveLater() {
        make()
        tap(1)
        tap(2)
        tap(2)
        sink.remoteBridge(method: "openLink", argsJson: "[\"https://example.com/1\"]")
        sink.remoteBridge(method: "openLink", argsJson: "[\"https://example.com/2\"]")
        sink.remoteBridge(method: "openLink", argsJson: "[\"https://example.com/3\"]")
        settle()
        XCTAssertEqual(delegate.actions, [.openLink(URL(string: "https://example.com/1")!), .openLink(URL(string: "https://example.com/2")!)])
        tap(1)
        sink.remoteBridge(method: "openLink", argsJson: "[\"https://example.com/old\"]")
        settle()
        XCTAssertEqual(delegate.actions.count, 2, "a gesture older than a spent one grants nothing")
    }

    func testDeathInsideATaskStillEndsTheTask() {
        make()
        final class Tasks: SandboxEngineDelegate {
            var events: [String] = []
            func engineTaskStarted(_ engine: SandboxEngine) { events.append("begin") }
            func engineTaskCompleted(_ engine: SandboxEngine) { events.append("end") }
            func engine(_ engine: SandboxEngine, ops json: String) { events.append("ops") }
            func engine(_ engine: SandboxEngine, query json: String) -> (json: String, binary: Data?) { ("{}", nil) }
            func engine(_ engine: SandboxEngine, listen json: String) {}
            func engine(_ engine: SandboxEngine, preventDefault json: String) {}
            func engineRequestsFrame(_ engine: SandboxEngine) {}
            func engine(_ engine: SandboxEngine, log level: String, text: String) {}
            func engine(_ engine: SandboxEngine, diag kind: String, detail: String) {}
            func engine(_ engine: SandboxEngine, bridge method: String, argsJson: String) {}
            func engineLoaded(_ engine: SandboxEngine) {}
            func engine(_ engine: SandboxEngine, stats: SandboxStats) {}
            func engine(_ engine: SandboxEngine, died reason: String) { events.append("died") }
        }
        let tasks = Tasks()
        engine.delegate = tasks
        sink.remoteTaskStarted()
        sink.remoteOps("[]", canvas: [:])
        sink.remoteDied("CPU")
        sink.remoteTaskCompleted()
        sink.remoteTaskStarted()
        settle()
        XCTAssertEqual(tasks.events, ["begin", "ops", "died", "end"])
        make()
        engine.delegate = tasks
        tasks.events = []
        sink.remoteTaskStarted()
        sink.remoteDisconnected()
        settle()
        XCTAssertEqual(tasks.events, ["begin", "died", "end"])
    }

    func testTapOnHiddenWidgetGrantsNothing() {
        make()
        engine.setVisible(false)
        tap(1)
        engine.setVisible(true)
        sink.remoteBridge(method: "openTelegramLink", argsJson: "[\"https://t.me/durov\"]")
        settle()
        XCTAssertTrue(delegate.actions.isEmpty)
    }

    func testGrantExpires() {
        make()
        tap(1)
        Thread.sleep(forTimeInterval: SandboxEngine.remoteGrantWindowMs / 1000 + 0.1)
        sink.remoteCopyText("late")
        settle()
        XCTAssertTrue(delegate.copies.isEmpty)
    }

    func testMalformedBridgeCallsAreRejected() {
        make()
        tap(1)
        sink.remoteBridge(method: "openLink", argsJson: "[\"javascript:alert(1)\"]")
        sink.remoteBridge(method: "openLink", argsJson: "not json")
        sink.remoteBridge(method: "exec", argsJson: "[]")
        sink.remoteBridge(method: "copyText", argsJson: "[\"x\"]")
        sink.remoteBridge(method: "request", argsJson: "[1,\"x\"]")
        settle()
        XCTAssertTrue(delegate.actions.isEmpty)
        XCTAssertTrue(delegate.copies.isEmpty)
        XCTAssertTrue(delegate.requests.isEmpty)
        XCTAssertEqual(delegate.diags.filter { $0 == "bridge-rejected" }.count, 5)
    }

    func testRequestWithoutCanQueryIsRefusedAndSettles() {
        make(context: WidgetContext(canQuery: false))
        tap(1)
        sink.remoteRequest(id: 7, data: "x", wallet: false)
        settle()
        XCTAssertTrue(delegate.requests.isEmpty)
        XCTAssertEqual(channel.answers.first?.0, 7)
        XCTAssertEqual(channel.answers.first?.1, .failure(.notAllowed))
    }

    func testForegroundRequestNeedsATapAndOneInFlight() {
        make()
        sink.remoteRequest(id: 1, data: "no tap", wallet: false)
        settle()
        XCTAssertTrue(delegate.requests.isEmpty)
        XCTAssertEqual(channel.answers.last?.1, .failure(.rateLimited))
        tap(1)
        sink.remoteRequest(id: 2, data: "tapped", wallet: false)
        sink.remoteRequest(id: 3, data: "second", wallet: false)
        settle()
        XCTAssertEqual(delegate.requests.map { $0.0.data }, ["tapped"])
        XCTAssertEqual(channel.answers.last?.0, 3)
        delegate.requests[0].1(.success(WidgetRequestAnswer(result: "done")))
        settle()
        XCTAssertEqual(channel.answers.last?.0, 2)
        XCTAssertEqual(channel.answers.last?.1, .answer("done"))
    }

    func testBackgroundRequestsNeedTheFlagAndAreRateLimited() {
        make(context: WidgetContext(canQuery: true, backgroundQuery: true))
        sink.remoteRequest(id: 1, data: "a", wallet: false)
        settle()
        XCTAssertEqual(delegate.requests.count, 1)
        delegate.requests[0].1(.success(WidgetRequestAnswer()))
        settle()
        sink.remoteRequest(id: 2, data: "b", wallet: false)
        settle()
        XCTAssertEqual(delegate.requests.count, 1)
        XCTAssertEqual(channel.answers.last?.1, .failure(.rateLimited))
        engine.setVisible(false)
        engine.setVisible(true)
        settle()
        sink.remoteRequest(id: 3, data: "c", wallet: false)
        settle()
        XCTAssertEqual(delegate.requests.count, 2)
        engine.setVisible(false)
        settle()
        delegate.requests[1].1(.failure(.timeout))
        sink.remoteRequest(id: 4, data: "hidden", wallet: false)
        settle()
        XCTAssertEqual(delegate.requests.count, 2)
    }

    func testWalletRequestAndFollowUpUseHostPermissionAndGrant() {
        make(context: WidgetContext(canQuery: true, backgroundQuery: true, wallet: true))
        let call = WalletCall.signData(SignDataPayload(type: .text, text: "Sign me"), data: "data")
        let payload = SandboxRemoteCoding.encode(wallet: call)
        sink.remoteRequest(id: 1, data: "wallet", wallet: true)
        sink.remoteWallet(id: 2, payload: payload)
        settle()
        XCTAssertTrue(delegate.requests.isEmpty)
        XCTAssertTrue(delegate.walletCalls.isEmpty)
        XCTAssertEqual(channel.answers.map { $0.1 }, [.failure(.notAllowed), .failure(.notAllowed)])

        tap(1)
        sink.remoteRequest(id: 3, data: "wallet", wallet: true)
        settle()
        XCTAssertEqual(delegate.requests.first?.0, WidgetRequest(data: "wallet", wallet: true))
        delegate.requests.first?.1(.success(WidgetRequestAnswer(result: "grant")))
        settle()

        sink.remoteWallet(id: 4, payload: Data("[1,\"signData\",{},\"data\"]".utf8))
        sink.remoteWallet(id: 5, payload: payload)
        settle()
        XCTAssertEqual(delegate.walletCalls.first?.0, call)
        delegate.walletCalls.first?.1(.failure(.rejected))
        settle()
        XCTAssertEqual(channel.answers.last?.1, .failure(.rejected))

        sink.remoteWallet(id: 6, payload: payload)
        settle()
        XCTAssertEqual(delegate.walletCalls.count, 1)
        XCTAssertEqual(channel.answers.last?.1, .failure(.notAllowed))
    }

    func testPreviewTakesNoActionsOrStorage() {
        make(context: WidgetContext(canQuery: true, preview: true))
        tap(1)
        sink.remoteBridge(method: "openLink", argsJson: "[\"https://example.com/\"]")
        sink.remoteStorage("{\"local\":{\"a\":\"b\"}}")
        sink.remoteWatchesPublicData(true)
        sink.remoteRequest(id: 1, data: "x", wallet: false)
        sink.remoteResizeAnchor("bottom")
        settle()
        XCTAssertTrue(delegate.actions.isEmpty)
        XCTAssertTrue(delegate.commits.isEmpty)
        XCTAssertTrue(delegate.watches.isEmpty)
        XCTAssertTrue(delegate.requests.isEmpty)
        XCTAssertEqual(delegate.anchors, [.bottom])
    }

    func testStorageIsCheckedAgain() {
        make()
        sink.remoteStorage("{\"local\":{\"a\":\"b\"}}")
        sink.remoteStorage("{\"local\":{\"a\":\"" + String(repeating: "x", count: 10_000) + "\"}}")
        sink.remoteStorage("[1,2]")
        settle()
        XCTAssertEqual(delegate.commits, [SdkPolicy.StorageCommit(local: ["a": "b"], session: nil)])
        XCTAssertEqual(delegate.diags.filter { $0 == "storage-rejected" }.count, 2)
    }

    func testHapticsPassTheHostGate() {
        make()
        for _ in 0 ..< 40 { sink.remoteBridge(method: "haptic", argsJson: "[\"selection\"]") }
        settle()
        XCTAssertLessThan(delegate.actions.count, 40)
        XCTAssertGreaterThan(delegate.actions.count, 0)
    }

    func testOrderIsKeptAcrossQueriesAndTextIsBounded() {
        make()
        sink.remoteOps("[]", canvas: [:])
        let answer = sink.remoteQuery("{\"q\":\"rect\"}")
        sink.remoteLog(level: "weird", text: String(repeating: "y", count: 100_000))
        sink.remoteRequestsFrame()
        settle()
        XCTAssertEqual(answer.json, "{}")
        XCTAssertEqual(delegate.order, ["ops", "query", "log:log:\(SandboxEngine.remoteTextLimit)", "frame"])
    }

    func testHiddenWidgetGetsNoFrameRequests() {
        make()
        engine.setVisible(false)
        sink.remoteRequestsFrame()
        settle()
        XCTAssertFalse(delegate.order.contains("frame"))
    }

    func testDisconnectKillsOnceAndSilencesTheRest() {
        make()
        sink.remoteDisconnected()
        sink.remoteDied("again")
        sink.remoteOps("[]", canvas: [:])
        tap(1)
        sink.remoteCopyText("x")
        settle()
        XCTAssertEqual(delegate.died, ["the widget engine process ended"])
        XCTAssertTrue(delegate.order.isEmpty)
        XCTAssertTrue(delegate.copies.isEmpty)
        XCTAssertTrue(channel.closed)
    }

    func testADeathBeforeTheDelegateReachesItLater() {
        channel = Channel()
        engine = SandboxEngine(remote: channel, features: [])
        channel.sink!.remoteDied("refused at open")
        settle()
        delegate = Delegate()
        engine.delegate = delegate
        settle()
        XCTAssertEqual(delegate.died, ["refused at open"])
        XCTAssertTrue(channel.closed)
    }

    func testTerminateTellsTheEngineAndCloses() {
        make()
        engine.terminate()
        settle()
        XCTAssertEqual(delegate.died, ["terminated"])
        XCTAssertTrue(channel.calls.contains("stop:terminated"))
        XCTAssertTrue(channel.closed)
    }

    func testConfigRoundTrip() {
        var config = SandboxConfig(innerWidth: 320, innerHeight: 240, devicePixelRatio: 2, theme: "dark")
        config.features = ["binaryOps", "canvasWords"]
        config.policy = .compat
        config.themeParams = ["bg_color": "#000000"]
        config.locale = "ru-RU"
        config.contentSafeArea = WidgetContentSafeArea(top: 1, right: 2, bottom: 3, left: 4, cornerLeft: 5, cornerRight: 6)
        config.storage = WidgetStorage(local: ["a": "b"], session: ["c": "d"])
        config.data = WidgetData(chatData: "chat", publicData: "public")
        config.context = WidgetContext(isCopy: true, canQuery: true, backgroundQuery: true, chatType: .group, viewer: WidgetViewer(id: 42, firstName: "Ann", username: "ann", isPremium: true), preview: false, wallet: true)
        config.maxTimers = 12
        config.visible = false
        let decoded = SandboxRemoteCoding.decodeConfig(SandboxRemoteCoding.encode(config: config))
        XCTAssertEqual(decoded?.features, config.features)
        XCTAssertEqual(decoded?.policy, .compat)
        XCTAssertEqual(decoded?.themeParams, config.themeParams)
        XCTAssertEqual(decoded?.locale, "ru-RU")
        XCTAssertEqual(decoded?.contentSafeArea, config.contentSafeArea)
        XCTAssertEqual(decoded?.storage, config.storage)
        XCTAssertEqual(decoded?.data, config.data)
        XCTAssertEqual(decoded?.context, config.context)
        XCTAssertEqual(decoded?.maxTimers, 12)
        XCTAssertEqual(decoded?.visible, false)
        XCTAssertEqual(decoded?.theme, "dark")
    }

    func testOutcomeAndStatsRoundTrip() {
        for outcome: SandboxRemoteOutcome in [.answer("x"), .answer(nil), .failure(.timeout), .failure(.rateLimited), .failure(.failed), .failure(.notAllowed), .failure(.rejected), .failure(.badRequest), .unsupported] {
            XCTAssertEqual(SandboxRemoteCoding.decodeOutcome(SandboxRemoteCoding.encode(outcome: outcome)), outcome)
        }
        var stats = SandboxStats()
        stats.jsMs = 12.5; stats.tasks = 3; stats.queries = 9; stats.queryMs = 1.25
        let decoded = SandboxRemoteCoding.decodeStats(SandboxRemoteCoding.encode(stats: stats))
        XCTAssertEqual(decoded?.tasks, 3)
        XCTAssertEqual(decoded?.queryMs, 1.25)
        XCTAssertEqual(SandboxRemoteCoding.decodeStats([.nan, -1, 1e300, 0, 0, .infinity, 0, 0, 0, 0])?.jsMs, 0)
        XCTAssertNil(SandboxRemoteCoding.decodeStats([1, 2]))
    }

    func testCanvasCodingRoundTripsAndSurvivesDamage() {
        let canvas = [3: WidgetCanvasBatch(words: [0, 1, -1, 2, 1, 5, 0, 7], strings: ["fillRect", "Привет"]), -2: WidgetCanvasBatch(words: [], strings: [])]
        let data = SandboxRemoteCoding.encode(canvas: canvas)
        let decoded = SandboxRemoteCoding.decodeCanvas(data)
        XCTAssertEqual(decoded?[3]?.words, canvas[3]?.words)
        XCTAssertEqual(decoded?[3]?.strings, canvas[3]?.strings)
        XCTAssertEqual(decoded?[-2]?.words.count, 0)
        for length in 0 ..< data.count {
            XCTAssertNil(SandboxRemoteCoding.decodeCanvas(data.prefix(length)))
        }
        var generator = SystemRandomNumberGenerator()
        for _ in 0 ..< 5000 {
            var damaged = [UInt8](data)
            for _ in 0 ..< Int.random(in: 1 ... 3, using: &generator) {
                damaged[Int.random(in: 0 ..< damaged.count, using: &generator)] = UInt8.random(in: 0 ... 255, using: &generator)
            }
            _ = SandboxRemoteCoding.decodeCanvas(Data(damaged))
        }
        var huge = Data()
        var count = UInt32(1).littleEndian
        withUnsafeBytes(of: &count) { huge.append(contentsOf: $0) }
        huge.append(Data(count: 8))
        var words = UInt32.max.littleEndian
        withUnsafeBytes(of: &words) { huge.append(contentsOf: $0) }
        XCTAssertNil(SandboxRemoteCoding.decodeCanvas(huge))
    }

    func testBinaryPayloadCodingIsBounded() {
        let payload = BinaryOpsPayload(words: [BinaryOps.magic, 1, -5], strings: [104, 105])
        let encoded = SandboxRemoteCoding.encode(binary: payload)
        let decoded = SandboxRemoteCoding.decodeBinary(words: encoded.words, strings: encoded.strings)
        XCTAssertEqual(decoded?.words, payload.words)
        XCTAssertEqual(decoded?.strings, payload.strings)
        XCTAssertNil(SandboxRemoteCoding.decodeBinary(words: Data([1, 2, 3]), strings: Data()))
    }

    func testLoopbackRunsAWidgetThroughTheHost() throws {
        let previous = SandboxEngine.remoteChannelFactory
        SandboxEngine.remoteChannelFactory = { SandboxLoopbackChannel(features: $0, backend: $1) }
        defer { SandboxEngine.remoteChannelFactory = previous }
        let renderer = LoopbackProbe()
        var options = WidgetHost.Options()
        options.features = ["binaryOps", "canvasWords", "webAnimations", "morphUpdates"]
        let host = try WidgetHost(html: "<div id='d'>a</div><script>var d=document.getElementById('d');d.style.height='50px';d.textContent='h:'+d.offsetHeight;</script>", options: options, rendererFactory: { _, _, _ in renderer })
        defer { host.destroy() }
        let deadline = Date().addingTimeInterval(5)
        while !(host.diagnostics.loaded && host.diagnostics.rendered) && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.005)) }
        XCTAssertTrue(host.diagnostics.loaded)
        XCTAssertNil(host.diagnostics.dead)
        XCTAssertTrue(renderer.log.contains("h:20"), renderer.log)
    }

    private final class LoopbackProbe: Renderer {
        let view = PlatformView(frame: CGRect(x: 0, y: 0, width: 300, height: 100))
        var onMessage: ((String) -> Void)?
        var onDiag: ((String, String) -> Void)?
        var log = ""
        func load() { onMessage?("{\"type\":\"ready\"}") }
        func send(_ json: String) {
            log += json
            guard let message = try? JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any], message["type"] as? String == "query", let id = message["qid"] as? Int else { return }
            onMessage?("{\"type\":\"result\",\"qid\":\(id),\"result\":{\"x\":0,\"y\":0,\"width\":100,\"height\":20,\"offsetHeight\":20}}")
        }
        func setTheme(_ theme: String) {}
        func evaluate(_ script: String, completion: @escaping (String?) -> Void) { completion(nil) }
        func destroy() {}
    }
}
