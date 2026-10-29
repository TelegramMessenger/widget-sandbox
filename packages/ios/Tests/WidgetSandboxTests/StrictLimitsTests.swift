import XCTest
import QuartzCore
import WidgetSandboxProtocol
@testable import WidgetSandbox

/// The strict caps of docs/decisions.md, "Strict limits from the start" (tests/unit/strict-limits.test.ts on the web):
/// past a counted cap the widget stops (the validator's `stopped`, the host's `dead`), compat keeps the caps hosts had
/// before; the shim refuses at the same numbers (canvas, timers) so the author gets an error in the widget; the
/// main-thread window (layout queries + the modelled cost of the ops) defers frames apart from the JS quota. The native
/// node model (`binaryOps`) and the canvas-word shortcut stop exactly where the script validator does.
final class StrictLimitsTests: XCTestCase {
    typealias Rig = NativeValidatorDifferentialTests.Rig

    private static let skeleton = "[{\"op\":\"el\",\"id\":2,\"tag\":\"html\"},{\"op\":\"ins\",\"parent\":1,\"id\":2,\"before\":null},{\"op\":\"el\",\"id\":3,\"tag\":\"body\"},{\"op\":\"ins\",\"parent\":2,\"id\":3,\"before\":null}]"

    private func rig(native: Bool, policy: SandboxPolicy = .strict) -> Rig {
        let r = Rig(native: native, policy: policy)
        _ = r.json(Self.skeleton)
        return r
    }

    private func json(_ ops: [[String: Any]]) -> String {
        String(decoding: try! JSONSerialization.data(withJSONObject: ops), as: UTF8.self)
    }

    /// A `<style>` element `id` with `css` as its text (text node `id + 1`).
    private func style(_ id: Int, _ css: String) -> [[String: Any]] {
        [["op": "el", "id": id, "tag": "style"], ["op": "ins", "parent": 3, "id": id, "before": NSNull()],
         ["op": "text", "id": id + 1, "data": css], ["op": "ins", "parent": id, "id": id + 1, "before": NSNull()]]
    }

    private func rules(_ n: Int, from: Int = 0) -> String { (0..<n).map { ".r\(from + $0){color:red}" }.joined() }
    private func keyframes(_ n: Int) -> String { (0..<n).map { "@keyframes k\($0){from{opacity:0}50%{opacity:.5}to{opacity:1}}" }.joined() }

    private static let canvas: [[String: Any]] = [["op": "el", "id": 9, "tag": "canvas"], ["op": "ins", "parent": 3, "id": 9, "before": NSNull()]]

    /// `n` fillRect commands as one canvas word payload.
    private func fillRects(_ n: Int) -> WidgetCanvasBatch {
        var words: [Double] = []
        for i in 0..<n { words += [0, 0, 0, 4, 0, Double(i), 0, 0, 0, 1, 0, 1] }
        return WidgetCanvasBatch(words: words, strings: ["fillRect"])
    }

    // MARK: defaults

    func testDefaults() {
        let config = SandboxConfig(innerWidth: 300, innerHeight: 200, devicePixelRatio: 2, theme: "light")
        XCTAssertEqual(config.memoryLimitBytes, 8 * 1024 * 1024)
        XCTAssertEqual(config.maxTimers, 64)
        XCTAssertEqual(config.mainThreadQuotaMs, 100)
        XCTAssertEqual(config.cpuQuotaMs, 300)
        let options = WidgetHost.Options()
        XCTAssertEqual(options.memoryLimitBytes, 8 * 1024 * 1024)
        XCTAssertEqual(options.maxTimers, 64)
        XCTAssertEqual(options.mainThreadQuotaMs, 100)
        XCTAssertEqual(CpuBudget.queryBudgetMs, 100)
        XCTAssertEqual(CpuBudget.queryBudgetMs(sliceMs: 500), 100)
        XCTAssertEqual(CpuBudget.opsCostMs(ops: 4000, canvasCommands: 0), 100, accuracy: 1e-9)
        XCTAssertEqual(CpuBudget.opsCostMs(ops: 1, canvasCommands: 2000), 4.025, accuracy: 1e-9)
    }

    func testTablesCarryThePolicyCaps() throws {
        let strict = try XCTUnwrap(NativeValidatorTables(json: Rig(native: true, policy: .strict).validator.tablesJSON()))
        XCTAssertEqual([strict.maxText, strict.maxAttr, strict.maxOps, strict.maxCanvasCommands], [32768, 8192, 10_000, 2000])
        XCTAssertEqual([strict.textBudget, strict.maxCssRules, strict.maxKeyframes, strict.maxLiveAnims], [49152, 400, 32, 64])
        XCTAssertTrue(strict.stop)
        let compat = try XCTUnwrap(NativeValidatorTables(json: Rig(native: true, policy: .compat).validator.tablesJSON()))
        XCTAssertEqual([compat.maxText, compat.maxAttr, compat.maxOps, compat.maxCanvasCommands], [2 << 20, 4 << 20, 200_000, 500_000])
        XCTAssertEqual([compat.textBudget, compat.maxCssRules, compat.maxKeyframes, compat.maxLiveAnims], [0, 0, 0, 1024])
        XCTAssertFalse(compat.stop)
    }

    // MARK: CSS rules and @keyframes

    func testCountCssRulesMatchesTheScript() {
        XCTAssertTrue(NativeValidator.countCssRules("a{color:red}@media (min-width:1px){b{color:red}c{color:red}}") == (3, 0))
        XCTAssertTrue(NativeValidator.countCssRules("@keyframes x{from{opacity:0}to{opacity:1}}@-webkit-keyframes y{0%{top:0}}") == (0, 2))
        XCTAssertTrue(NativeValidator.countCssRules("a::before{content:\"}{\"}/* x{y} */b{color:red}") == (2, 0))
        XCTAssertTrue(NativeValidator.countCssRules("@supports (display:grid){@container (min-width:1px){a{color:red}}}") == (1, 0))
        XCTAssertTrue(NativeValidator.countCssRules("@KEYFRAMES x{to{top:0}}@keyframesx{a{b:c}}\u{a0}@keyframes z{to{top:0}}/* open") == (1, 2))
        XCTAssertTrue(NativeValidator.countCssRules("@\u{212A}eyframes q{to{top:0}}") == (0, 1), "lowercased as String.prototype.toLowerCase does (KELVIN SIGN)")
    }

    func testCssRuleAndKeyframeCapsStopOnBothValidatorPaths() {
        var stops: [String?] = []
        for native in [false, true] {
            let v = rig(native: native)
            let first = v.json(json(style(10, rules(250)) + style(20, rules(150, from: 250))))
            XCTAssertEqual((first.0 as! [[String: Any]]).filter { $0["op"] as? String == "css" }.count, 2, "native \(native)")
            XCTAssertNil(v.validator.stopped)
            _ = v.json(json([["op": "rm", "id": 20]]))
            _ = v.json(json(style(30, rules(150, from: 400))))
            XCTAssertNil(v.validator.stopped, "the removed sheet was refunded (native \(native))")
            let over = v.json(json(style(40, rules(1, from: 900))))
            XCTAssertEqual(over.0.count, 0)
            XCTAssertTrue(over.2.contains("limit|CSS limit exceeded: 401 rules in the document's stylesheets (max 400)"), "\(over.2)")
            stops.append(v.validator.stopped)

            let k = rig(native: native)
            _ = k.json(json(style(10, keyframes(32))))
            XCTAssertNil(k.validator.stopped)
            XCTAssertEqual(k.json(json([["op": "data", "id": 11, "data": keyframes(33)]])).0.count, 0)
            XCTAssertEqual(k.validator.stopped, "CSS limit exceeded: 33 @keyframes in the document's stylesheets (max 32)", "native \(native)")

            let compat = rig(native: native, policy: .compat)
            XCTAssertEqual((compat.json(json(style(10, rules(1000)))).0 as! [[String: Any]]).filter { $0["op"] as? String == "css" }.count, 1)
            XCTAssertNil(compat.validator.stopped)
        }
        XCTAssertEqual(stops, Array(repeating: "CSS limit exceeded: 401 rules in the document's stylesheets (max 400)", count: 2))
    }

    // MARK: ops per batch, canvas commands per send

    func testOpsPerBatchCapStopsOnEveryPath() {
        let texts = { (n: Int, from: Int) in (0..<n).map { ["op": "text", "id": from + $0, "data": ""] as [String: Any] } }
        for native in [false, true] {
            let v = rig(native: native)
            XCTAssertEqual(v.json(json(texts(10_000, 100))).0.count, 10_000)
            let over = v.json(json(texts(10_001, 20_100)))
            XCTAssertEqual(over.0.count, 0)
            XCTAssertEqual(over.2, ["limit|Ops limit exceeded: 10001 operations in one batch (max 10000)"])
            XCTAssertEqual(v.validator.stopped, "Ops limit exceeded: 10001 operations in one batch (max 10000)")
            let compat = rig(native: native, policy: .compat)
            XCTAssertEqual(compat.json(json(texts(10_001, 100))).0.count, 10_001)
            XCTAssertNil(compat.validator.stopped)
        }
        for policy in [SandboxPolicy.strict, .compat] {
            let v = rig(native: true, policy: policy)
            var encoder = BinaryOpsEncoder()
            encoder.begin(reset: true)
            for i in 0..<10_001 { encoder.text(Int32(100 + i), "", code: .text) }
            let out = v.binary(encoder.finish())
            XCTAssertEqual(out.0.count, policy == .strict ? 0 : 10_001)
            XCTAssertEqual(v.validator.stopped, policy == .strict ? "Ops limit exceeded: 10001 operations in one batch (max 10000)" : nil)
        }
    }

    func testCanvasCommandsPerSendStopOnEveryPath() {
        let cmds = { (n: Int) in (0..<n).map { ["c", "fillRect", $0, 0, 1, 1] as [Any] } }
        let reason = "Canvas limit exceeded: 2001 commands in one send (max 2000)"
        for native in [false, true] {
            let v = rig(native: native)
            _ = v.json(json(Self.canvas))
            let fits = v.json(json([["op": "canvas", "id": 9, "cmds": cmds(2000)]])).0 as! [[String: Any]]
            XCTAssertEqual((fits.first?["cmds"] as? [Any])?.count, 2000)
            XCTAssertEqual(v.json(json([["op": "canvas", "id": 9, "cmds": cmds(2001)]])).0.count, 0)
            XCTAssertEqual(v.validator.stopped, reason, "native \(native)")
            let compat = rig(native: native, policy: .compat)
            _ = compat.json(json(Self.canvas))
            let all = compat.json(json([["op": "canvas", "id": 9, "cmds": cmds(2001)]])).0 as! [[String: Any]]
            XCTAssertEqual((all.first?["cmds"] as? [Any])?.count, 2001)
        }
        // Canvas words through the native model (binary ops).
        for (count, stops) in [(2000, false), (2001, true)] {
            let v = rig(native: true)
            var encoder = BinaryOpsEncoder()
            encoder.begin(reset: true)
            encoder.element(9, tag: "canvas", svg: false, attributes: [])
            encoder.insert(parent: 3, 9, before: 0)
            encoder.canvas(9, key: 7)
            let out = v.binary(encoder.finish(), canvas: [7: fillRects(count)])
            XCTAssertEqual(out.0.count, stops ? 0 : 3, "\(count)")
            XCTAssertEqual(v.validator.stopped, stops ? reason : nil)
        }
        // The canvas-word shortcut of the script validator's host takes 2000; 2001 go through the validator, which stops.
        for (count, stops) in [(2000, false), (2001, true)] {
            let v = rig(native: false)
            _ = v.json(json(Self.canvas))
            let result = v.validator.processOpsForRenderer("[{\"op\":\"canvas\",\"id\":9,\"bin\":7}]", canvas: [7: fillRects(count)], typed: true)
            if stops {
                guard case .empty = result else { return XCTFail("2001 commands reached the renderer") }
                XCTAssertEqual(v.validator.stopped, reason)
                XCTAssertEqual(v.validator.nativeCanvasBatches, 0)
                guard case .empty = v.validator.processOpsForRenderer("[{\"op\":\"canvas\",\"id\":9,\"bin\":7}]", canvas: [7: fillRects(1)], typed: true) else { return XCTFail("a stopped validator took the shortcut") }
            } else {
                guard case .ops(let ops) = result else { return XCTFail("expected the shortcut") }
                XCTAssertEqual(ops.count, 1)
                XCTAssertEqual(v.validator.nativeCanvasBatches, 1)
            }
        }
    }

    // MARK: text values and attributes

    func testTextAndAttributeCapsStopOnBothValidatorPaths() {
        let big = String(repeating: "b", count: 32 * 1024 + 1)
        let text = "Text limit exceeded: a text value of 32769 characters (max 32768)"
        let attribute = "Attribute limit exceeded: a value of 8193 characters (max 8192)"
        let cases: [(String, [[String: Any]], String)] = [
            ("a text node", [["op": "text", "id": 21, "data": big]], text),
            ("text data", [["op": "text", "id": 21, "data": "x"], ["op": "data", "id": 21, "data": big]], text),
            ("a canvas string", Self.canvas + [["op": "canvas", "id": 9, "cmds": [["c", "fillText", "short", 0, 0], ["c", "fillText", big, 0, 0]]]], text),
            ("Path2D data", Self.canvas + [["op": "canvas", "id": 9, "cmds": [["pn", 1, big]]]], text),
            ("an attribute on creation", [["op": "el", "id": 22, "tag": "div", "attrs": ["data-label": String(repeating: "x", count: 8193)]]], attribute),
            ("an attribute", [["op": "el", "id": 22, "tag": "div"], ["op": "attr", "id": 22, "name": "VALUE", "value": String(repeating: "x", count: 8193)]], attribute),
            ("an inline style", [["op": "el", "id": 22, "tag": "div"], ["op": "style", "id": 22, "name": "--x", "value": String(repeating: "x", count: 8193)]], attribute),
        ]
        for (what, ops, reason) in cases {
            var results: [(NSArray, Int, [String])] = []
            for native in [false, true] {
                let v = rig(native: native)
                let out = v.json(json([["op": "text", "id": 30, "data": "before"]] + ops))
                XCTAssertEqual(out.0.count, 0, "\(what): its batch is refused whole (native \(native))")
                XCTAssertEqual(v.validator.stopped, reason, "\(what) (native \(native))")
                XCTAssertTrue(out.2.contains("limit|" + reason), "\(what) (native \(native)): \(out.2)")
                XCTAssertEqual(v.json(json([["op": "text", "id": 31, "data": "after"]])).0.count, 0, "\(what): a stopped validator refuses every batch")
                results.append(out)
            }
            XCTAssertEqual(results[1].0, results[0].0, what)
            XCTAssertEqual(results[1].2, results[0].2, what)
        }
        // 32 KB and 8 KB pass.
        for native in [false, true] {
            let v = rig(native: native)
            let fits = v.json(json([["op": "text", "id": 20, "data": "x"], ["op": "ins", "parent": 3, "id": 20, "before": NSNull()], ["op": "data", "id": 20, "data": String(repeating: "a", count: 32 * 1024)],
                                    ["op": "el", "id": 22, "tag": "div", "attrs": ["data-label": String(repeating: "x", count: 8192)]]]))
            XCTAssertEqual(fits.0.count, 4)
            XCTAssertNil(v.validator.stopped)
        }
        // Compat keeps the old caps and never stops.
        for native in [false, true] {
            let v = rig(native: native, policy: .compat)
            XCTAssertEqual(v.json(json([["op": "text", "id": 21, "data": big], ["op": "el", "id": 22, "tag": "div", "attrs": ["data-label": big]]])).0.count, 2)
            XCTAssertNil(v.validator.stopped)
        }
    }

    /// A canvas string past 32 KB in a word payload never takes a shortcut to the renderer: the native model and the
    /// script validator's host both send it through the validator, which stops the widget.
    func testLongCanvasStringsInWordPayloadsStop() {
        let big = String(repeating: "b", count: 32 * 1024 + 1)
        let payloads = [
            WidgetCanvasBatch(words: [1, 0, 0, 1, 1, 1], strings: ["fillStyle", big]),
            WidgetCanvasBatch(words: [5, -1, 1, 1, 1, 0], strings: [big]),
        ]
        for batch in payloads {
            let native = rig(native: true)
            var encoder = BinaryOpsEncoder()
            encoder.begin(reset: true)
            encoder.element(9, tag: "canvas", svg: false, attributes: [])
            encoder.insert(parent: 3, 9, before: 0)
            encoder.canvas(9, key: 7)
            XCTAssertEqual(native.binary(encoder.finish(), canvas: [7: batch]).0.count, 0)
            XCTAssertEqual(native.validator.stopped, "Text limit exceeded: a text value of 32769 characters (max 32768)")

            let script = rig(native: false)
            _ = script.json(json(Self.canvas))
            guard case .empty = script.validator.processOpsForRenderer("[{\"op\":\"canvas\",\"id\":9,\"bin\":7}]", canvas: [7: batch], typed: true) else { return XCTFail("a 32 KB + 1 string reached the renderer") }
            XCTAssertEqual(script.validator.stopped, "Text limit exceeded: a text value of 32769 characters (max 32768)")
        }
    }

    // MARK: the engine's cost model

    func testCanvasCommandsAreCountedForTheMainThreadCost() {
        XCTAssertEqual(SandboxEngine.countCanvasCommands(inJSON: "[{\"op\":\"canvas\",\"id\":9,\"cmds\":[[\"c\",\"fillRect\",0,0,1,1],[\"s\",\"fillStyle\",\"]\\\"cmds\\\":[\"],[\"c\",\"drawImage\",{\"n\":5},0,0]]},{\"op\":\"text\",\"id\":3,\"data\":\"\\\"cmds\\\":[1,2]\"},{\"op\":\"canvas\",\"id\":9,\"cmds\":[]}]"), 3)
        XCTAssertEqual(SandboxEngine.countCanvasCommands(inJSON: "[{\"op\":\"canvas\",\"id\":9,\"cmds\":[[\"c\",\"save\"]]},{\"op\":\"canvas\",\"id\":10,\"cmds\":[[\"c\",\"restore\"],[\"r\",1,1]]}]"), 3)
        XCTAssertEqual(SandboxEngine.countCanvasCommands(inJSON: "[{\"op\":\"canvas\",\"id\":9,\"cmds\":[[\"c\""), 1)
        XCTAssertEqual(CanvasWords.commandCount(fillRects(37)), 37)
        XCTAssertNil(CanvasWords.commandCount(WidgetCanvasBatch(words: [0, 0, 0, 4, 0], strings: ["fillRect"])))
    }

    // MARK: through the engine and the host

    func testThe65thLiveTimerThrowsRangeError() {
        let d = RecordingDelegate()
        let engine = SandboxEngine(html: "<script>var n = 0; try { for (; n < 100; n++) setTimeout(function () {}, 60000); } catch (e) { console.log(e.name + ' at ' + n); }</script>",
                                   config: SandboxConfig(innerWidth: 300, innerHeight: 200, devicePixelRatio: 2, theme: "light"))
        engine.delegate = d
        engine.start()
        wait(for: [d.loaded], timeout: 20)
        engine.queue.sync {}
        XCTAssertTrue(d.logs.contains { $0.1 == "RangeError at 64" }, "\(d.logs)")
        XCTAssertFalse(d.diags.contains { $0.0 == "timer-limit-enforced" })
        engine.terminate()
    }

    /// The boot config carries the policy: under strict the shim throws at the validator's 2000 commands per canvas
    /// and task, so the author sees the error where the host would stop the widget; compat draws them all.
    func testTheShimRefusesTheCanvasCommandPastTheCapUnderStrict() {
        let html = "<canvas id=c></canvas><script>var x = document.getElementById('c').getContext('2d'); try { for (var i = 0; i < 2001; i++) x.fillRect(i, 0, 1, 1); console.log('drew all'); } catch (e) { console.log(e.name + ': ' + e.message); }</script>"
        for policy in [SandboxPolicy.strict, .compat] {
            let d = RecordingDelegate()
            var config = SandboxConfig(innerWidth: 300, innerHeight: 200, devicePixelRatio: 2, theme: "light")
            config.cpuLimitMs = 2000
            config.policy = policy
            let engine = SandboxEngine(html: html, config: config)
            engine.delegate = d
            engine.start()
            wait(for: [d.loaded], timeout: 20)
            engine.queue.sync {}
            let expected = policy == .strict ? "RangeError: Canvas limit exceeded: more than 2000 commands for one canvas in one task (max 2000)" : "drew all"
            XCTAssertTrue(d.logs.contains { $0.1 == expected }, "\(policy): \(d.logs)")
            engine.terminate()
        }
    }

    /// A widget whose script sets a 40 000-character text node is stopped with the validator's reason, through the
    /// real engine and host, on both validator paths: the renderer keeps its last render, frozen, and gets nothing after.
    func testATextNodeOver32KBStopsTheWidget() {
        let html = "<p id=p>first</p><script>setTimeout(function () { document.getElementById('p').textContent = 'x'.repeat(40000); }, 10); setTimeout(function () { document.getElementById('p').textContent = 'never'; }, 30); setTimeout(function () { localStorage.setItem('k', 'late'); }, 40);</script>"
        let reason = "Text limit exceeded: a text value of 40000 characters (max 32768)"
        for binary in [false, true] {
            let renderer = RecordingRenderer()
            var options = WidgetHost.Options()
            options.cpuLimitMs = 2000
            if binary { options.features = ["binaryOps"] }
            let host = try! WidgetHost(html: html, theme: WidgetTheme(), width: 300, options: options, rendererFactory: { _, _, _ in renderer })
            var commits = 0
            host.onStorageCommit = { _, _ in commits += 1 }
            defer { host.destroy() }
            let deadline = Date().addingTimeInterval(10)
            while host.diagnostics.dead == nil && Date() < deadline {
                RunLoop.main.run(until: Date().addingTimeInterval(0.01))
            }
            RunLoop.main.run(until: Date().addingTimeInterval(0.2))
            XCTAssertEqual(host.diagnostics.dead, reason, "binary \(binary)")
            XCTAssertEqual(host.diagnostics.diags["limit"]?.details, [reason], "binary \(binary)")
            let all = renderer.messages.joined()
            XCTAssertTrue(all.contains("first"), "the last render stays (binary \(binary))")
            XCTAssertFalse(all.contains(String(repeating: "x", count: 40000)), "binary \(binary)")
            XCTAssertFalse(all.contains("never"), "binary \(binary)")
            XCTAssertTrue(renderer.messages.contains { $0.contains("\"frozen\"") }, "binary \(binary)")
            XCTAssertEqual(commits, 0, "the engine was stopped (binary \(binary))")
        }
    }

    /// Answers the engine's frame requests like a renderer at ~60 Hz and records what the widget did.
    private final class FrameDriver: SandboxEngineDelegate {
        let loaded = XCTestExpectation(description: "loaded")
        let lock = NSLock()
        var diags: [(String, String)] = []
        var frames = 0
        var died: String?
        func engineRequestsFrame(_ engine: SandboxEngine) {
            DispatchQueue.global().asyncAfter(deadline: .now() + 0.016) { [weak engine] in engine?.frame(time: CACurrentMediaTime() * 1000) }
        }
        func engine(_ engine: SandboxEngine, ops json: String) {}
        func engine(_ engine: SandboxEngine, query json: String) -> (json: String, binary: Data?) { ("{}", nil) }
        func engine(_ engine: SandboxEngine, listen json: String) {}
        func engine(_ engine: SandboxEngine, preventDefault json: String) {}
        func engine(_ engine: SandboxEngine, log level: String, text: String) {
            if text == "frame" { lock.withLock { frames += 1 } }
        }
        func engine(_ engine: SandboxEngine, diag kind: String, detail: String) { lock.withLock { diags.append((kind, detail)) } }
        func engine(_ engine: SandboxEngine, bridge method: String, argsJson: String) {}
        func engineLoaded(_ engine: SandboxEngine) { loaded.fulfill() }
        func engine(_ engine: SandboxEngine, stats: SandboxStats) {}
        func engine(_ engine: SandboxEngine, died reason: String) { lock.withLock { died = reason }; loaded.fulfill() }
    }

    /// 200 text updates per frame cost the host 5 ms of modelled main-thread work: a 2 ms window lets about one frame
    /// a second through and says so; with the window open the same widget runs every frame. The JS quota is wide in both.
    func testTheMainThreadWindowDefersFrames() {
        let html = "<div id=list></div><script>var list = document.getElementById('list'); for (var i = 0; i < 200; i++) list.appendChild(document.createElement('span')); var n = 0;" +
            "function frame() { n++; for (var s = list.firstChild; s; s = s.nextSibling) s.textContent = String(n); console.log('frame'); requestAnimationFrame(frame); } requestAnimationFrame(frame);</script>"
        func run(mainThreadQuotaMs: Double) -> (frames: Int, diags: [(String, String)], died: String?) {
            let d = FrameDriver()
            var config = SandboxConfig(innerWidth: 300, innerHeight: 200, devicePixelRatio: 2, theme: "light")
            config.cpuLimitMs = 2000
            config.cpuQuotaMs = 100_000
            config.mainThreadQuotaMs = mainThreadQuotaMs
            let engine = SandboxEngine(html: html, config: config)
            engine.delegate = d
            engine.start()
            wait(for: [d.loaded], timeout: 20)
            RunLoop.current.run(until: Date().addingTimeInterval(1.5))
            engine.queue.sync {}
            let result = d.lock.withLock { (d.frames, d.diags, d.died) }
            engine.terminate()
            return result
        }
        let open = run(mainThreadQuotaMs: 100_000)
        XCTAssertNil(open.died)
        XCTAssertGreaterThan(open.frames, 10)
        XCTAssertFalse(open.diags.contains { $0.0 == "cpu-throttled" }, "\(open.diags.filter { $0.0 == "cpu-throttled" })")
        let tight = run(mainThreadQuotaMs: 2)
        XCTAssertNil(tight.died)
        XCTAssertLessThanOrEqual(tight.frames, 4, "frames through a 2 ms window in 1.5 s")
        XCTAssertGreaterThanOrEqual(tight.frames, 1)
        XCTAssertTrue(tight.diags.contains { $0.0 == "cpu-throttled" && $0.1.contains("main-thread budget used by layout queries and ops") }, "\(tight.diags)")
    }
}
