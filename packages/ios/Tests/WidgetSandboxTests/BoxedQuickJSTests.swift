import XCTest
@testable import WidgetSandbox

/// The boxed QuickJS engine (docs/decisions.md, "QuickJS in a wasm2c box"; packages/quickjs-box) behind the same
/// `QuickJSContext` as the native one: the same answers, the same host functions, the same shipped bytecode, and the
/// limits that are enforced differently inside a box (memory, stack). What a fault inside the box does (the process
/// ends) is checked without Swift, by `packages/quickjs-box/tools/build.sh test`.
final class BoxedQuickJSTests: XCTestCase {
    private func context(_ backend: QuickJSBackend, memory: Int = 16 << 20) throws -> QuickJSContext {
        if backend == .boxed { try XCTSkipUnless(QuickJSBackend.isBoxedAvailable, "the boxed engine is not part of this build") }
        let js = QuickJSContext(memoryLimitBytes: memory, maxStackBytes: JSThread.jsStackLimit, backend: backend)
        XCTAssertNil(js.unavailableReason)
        XCTAssertEqual(js.backend, backend)
        return js
    }

    private func answer(_ js: QuickJSContext, _ code: String) -> String {
        do {
            let r = try js.evaluate(code, filename: "t.js")
            js.executePendingJobs { _ in }
            switch r {
            case .string(let s): return "string:" + s
            case .number(let n): return "number:\(n)"
            case .undefined: return "undefined"
            }
        } catch {
            return "threw:\(error)"
        }
    }

    func testNativeIsTheDefault() throws {
        XCTAssertEqual(QuickJSContext(memoryLimitBytes: 8 << 20).backend, .native)
        guard ProcessInfo.processInfo.environment["WIDGET_SANDBOX_ENGINE"] == nil else { return } // a boxed run of the suite
        XCTAssertEqual(QuickJSBackend.default, .native)
        XCTAssertEqual(WidgetHost.Options().engine, .native)
        XCTAssertEqual(SandboxEngine().backend, .native)
        XCTAssertEqual(EnginePool.shared.backend, .native)
    }

    func testTheBoxRunsTheEngineTheNativePackagesCompile() throws {
        let js = try context(.boxed)
        // packages/quickjs-box/build.sh --check proves the box is built from the submodule's commit; here: it says so.
        XCTAssertTrue(js.engineDescription.hasPrefix("boxed QuickJS "), js.engineDescription)
        // Guard pages on macOS; a build with WIDGET_SANDBOX_BOX_BOUNDS_CHECKS=1 runs the mode of every other platform.
        let boundsChecks = ProcessInfo.processInfo.environment["WIDGET_SANDBOX_BOX_BOUNDS_CHECKS"] != nil
        XCTAssertTrue(js.engineDescription.hasSuffix(boundsChecks ? "(bounds-checks)" : "(guard-pages)"), js.engineDescription)
        XCTAssertEqual(try context(.native).engineDescription, "native QuickJS")
    }

    /// Scripts whose answer does not depend on the C library's last digit (Math.tan and friends differ by one ulp:
    /// the box carries its own libm, the one the web build has).
    private static let scripts = [
        "[1, 2, 3].map((x) => x * 2).join('-')",
        "6 * 7", "0.1 + 0.2", "-0", "1 / 3", "1e21", "NaN", "({})", "undefined", "null", "true", "Symbol('s')", "12345678901234567890n",
        "'a\\u0000b\\u00e9\\ud83c\\udf89'", "'\\ud83d'", "''", "'x'.repeat(100000).length",
        "function f() { null.x; } f()", "throw new RangeError('r')", "throw 'plain'", "throw {name: 'N', message: 'm'}", "var 1x", "undefinedFn()",
        "JSON.stringify({a: [1, {b: null, c: 'x\\u2028y'}], n: 1e21, e: -0})", "JSON.parse('{\"a\":[1,2,{\"b\":\"\\\\ud83d\\\\ude00\"}]}').a[2].b",
        "/(?<y>\\d{4})-(?<m>\\d{2})/u.exec('on 2026-11-12').groups.y + /\\p{Script=Cyrillic}+/u.exec('abc привет')[0]",
        "[3, 1, 10, 2].sort().join() + [3, 1, 10, 2].sort((a, b) => a - b).join()",
        "Array.from(new Uint8Array(new Float64Array([1.5, -0]).buffer)).join()",
        "new Date(Date.UTC(2026, 10, 12, 22, 21, 5, 123)).toISOString()",
        "new Date(2026, 0, 15, 12).toString() + '|' + new Date(2026, 6, 15, 12).getTimezoneOffset()",
        "(123.456).toFixed(2) + (0.1).toString(2) + (255).toString(16) + (1e21).toPrecision(5) + parseFloat('3.14e-2x')",
        "var log = []; Promise.resolve(1).then((v) => log.push('a' + v)); Promise.reject(new Error('x')).catch((e) => log.push(e.message)); 'sync'",
        "log.join()",
        "function* g(n) { for (let i = 0; i < n; i++) yield i * i; } [...g(5)].join()",
        "class A { #p = 1; get p() { return this.#p; } } new A().p",
        "var d = 0; function deep() { d++; deep(); } try { deep(); } catch (e) { e.name + ':' + (d > 100) }",
        "typeof Atomics + typeof WeakRef + typeof performance + typeof globalThis.os + typeof print",
        "Object.getOwnPropertyNames(globalThis).sort().join()",
        "import('x').then(() => 1, (e) => { globalThis.imp = e.name; }); 0", "imp",
        "new Intl.NumberFormat()", "[] + {}", "2 ** 53 + 2", "Math.sqrt(2) + Math.floor(-1.5) + Math.abs(-3) + Math.max(1, 2) + Math.round(2.5)",
    ]

    func testSameAnswersAsTheNativeEngine() throws {
        let native = try context(.native), boxed = try context(.boxed)
        for script in Self.scripts {
            XCTAssertEqual(answer(boxed, script), answer(native, script), script)
        }
        XCTAssertEqual(boxed.hasPendingJob, native.hasPendingJob)
    }

    func testStringsCrossByLength() throws {
        let js = try context(.boxed)
        try js.evaluate("globalThis.echo = (s) => s + '|' + s.length;", filename: "t.js")
        XCTAssertEqual(try js.call("echo", ["a\u{0}b\u{0}é"]).string, "a\u{0}b\u{0}é|5")
        XCTAssertEqual(try js.evaluate("'x\\u0000y'", filename: "u.js").string, "x\u{0}y")
        XCTAssertEqual(try js.call("echo", [""]).string, "|0")
        XCTAssertEqual(try js.call("echo", [String(repeating: "я", count: 300_000)]).string?.hasSuffix("|300000"), true)
        XCTAssertThrowsError(try js.call("missing")) { XCTAssertEqual("\($0)", "missing is not a function") }
    }

    func testCapturedFunctionOutlivesItsGlobal() throws {
        for backend in [QuickJSBackend.native, .boxed] {
            let js = try context(backend)
            try js.evaluate("globalThis.f = () => 'first';", filename: "t.js")
            XCTAssertTrue(js.captureGlobalFunction("f"))
            XCTAssertFalse(js.captureGlobalFunction("Math"))
            try js.evaluate("delete globalThis.f;", filename: "t.js")
            XCTAssertEqual(try js.call("f").string, "first", "\(backend)")
            try js.evaluate("globalThis.f = () => 'second';", filename: "t.js")
            XCTAssertEqual(try js.call("f").string, "first", "\(backend): the captured one is called")
            XCTAssertTrue(js.captureGlobalFunction("f"))
            XCTAssertEqual(try js.call("f").string, "second", "\(backend)")
        }
    }

    /// What a host function receives and what the script gets back, argument by argument, on both engines.
    func testHostFunctionsSeeWhatTheNativeEngineGivesThem() throws {
        var seen: [QuickJSBackend: [[String]]] = [:]
        var answers: [QuickJSBackend: String] = [:]
        for backend in [QuickJSBackend.native, .boxed] {
            let js = try context(backend)
            js.registerFunction("__h") { args in
                seen[backend, default: []].append(args)
                switch args.first {
                case "null": return .null
                case "number": return .number(-1.25)
                case "string": return .string("caf\u{e9} \u{1F389}")
                case "nul": return .string("cut\u{0}here")
                case "empty": return .string("")
                case "bytes": return .arrayBuffer(Data([1, 2, 3, 0, 255]))
                case "nobytes": return .arrayBuffer(Data())
                case "big": return .string(String(repeating: "\u{4E2D}", count: 200_000))
                default: return .undefined
                }
            }
            answers[backend] = answer(js, """
                var out = [];
                out.push(String(__h('x', 'two', 'three')), String(__h('null')), __h('number'), __h('string'), __h('nul'), __h('empty') === '', \
                Array.from(new Uint8Array(__h('bytes'))).join(), __h('nobytes').byteLength, __h('big').length);
                __h(1, 1.5, -0, 1e21, NaN, Infinity, 123456789012345, 1e15);
                __h('a\\u0000b', {}, null, undefined, [1], true, function () {});
                __h();
                __h('\\ud83d lone', '\\u00e9');
                out.join('|')
                """)
        }
        XCTAssertEqual(answers[.boxed], answers[.native])
        XCTAssertEqual(answers[.boxed], "string:undefined|null|-1.25|caf\u{e9} \u{1F389}|cut|true|1,2,3,0,255|0|200000")
        XCTAssertEqual(seen[.boxed], seen[.native])
        XCTAssertEqual(seen[.boxed]?[9], ["1", "1.5", "0", "1e+21", "nan", "inf", "123456789012345", "1000000000000000.0"])
        XCTAssertEqual(seen[.boxed]?[10], ["a", "", "", "", "", "", ""], "a string ends at its first NUL, as for the native engine")
    }

    /// The one difference: the box passes a host function its first eight arguments (the hosts' own read two).
    func testHostFunctionArgumentsBeyondEightStayInTheBox() throws {
        let js = try context(.boxed)
        var count = -1
        js.registerFunction("__h") { args in count = args.count; return .undefined }
        try js.evaluate("__h(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)", filename: "t.js")
        XCTAssertEqual(count, 8)
    }

    func testWordsAndOpsFunctions() throws {
        var seen: [QuickJSBackend: [String]] = [:]
        for backend in [QuickJSBackend.native, .boxed] {
            let js = try context(backend)
            js.registerWordsFunction("__w") { number, words, text in
                seen[backend, default: []].append("w \(number) \(Array(words)) \(text.unicodeScalars.map { $0.value })")
            }
            js.registerOpsFunction("__o") { words, strings in
                seen[backend, default: []].append("o \(Array(words)) \(Array(strings))")
            }
            _ = answer(js, """
                __w(3, new Float64Array([1.5, -2, 1e300]), 'a\\u0000\\u00e9');
                __w(4, new Float64Array(0), '');
                __w(5, new Int32Array(2), 'wrong array type');
                __w('5', new Float64Array(1), 'wrong first argument');
                __w(6, new Float64Array(new ArrayBuffer(32), 8, 2), 'a view');
                __o(new Int32Array([7, 2, -1, 2147483647]), 'str\\u0000ings');
                __o(new Int32Array(new ArrayBuffer(16), 4, 2), '');
                __o(new Float64Array(1), 'wrong array type');
                __o(new Int32Array(1), 5);
                0
                """)
        }
        XCTAssertEqual(seen[.boxed], seen[.native])
        XCTAssertEqual(seen[.boxed]?.count, 5)
        XCTAssertEqual(seen[.boxed]?.first, "w 3.0 [1.5, -2.0, 1e+300] [97, 0, 233]")
    }

    /// The boxed twin of BundledBytecodeTests: one set of `.qbc` serves both engines.
    func testShippedBytecodeMatchesTheBoxedEngineAndRunsInIt() throws {
        let r = SandboxResources.shared
        for (name, source, bytecode) in [("sandbox-bundle.js", r.sandboxBundle, r.sandboxBytecode), ("validator-bundle.js", r.validatorBundle, r.validatorBytecode)] {
            let shipped = try XCTUnwrap(bytecode, name)
            let js = try context(.boxed, memory: 256 << 20)
            XCTAssertEqual(try js.compileBytecode(source, filename: name), shipped, "\(name)'s .qbc is not what the boxed engine compiles")
            XCTAssertEqual(try context(.native, memory: 256 << 20).compileBytecode(source, filename: name), shipped)
            js.registerFunction("__host") { _ in .undefined }
            js.registerFunction("__hostBinary") { _ in .null }
            js.registerFunction("__twNode") { _ in .string("") }
            js.registerFunction("__diag") { _ in .undefined }
            XCTAssertNoThrow(try js.evaluateBytecode(shipped, source: source), name)
            XCTAssertEqual(try js.evaluate("typeof __twDispatch + typeof __twValidator", filename: "t.js").string, name.hasPrefix("sandbox") ? "functionundefined" : "undefinedobject")
            var stale = shipped
            stale[stale.count / 2] ^= 1
            XCTAssertThrowsError(try js.evaluateBytecode(shipped, source: source + " "), "a .qbc of another source is refused") { XCTAssertTrue("\($0)".contains("different source"), "\($0)") }
        }
    }

    /// `JS_SetMemoryLimit` cannot work in the box (this build of QuickJS does not measure its blocks): the limit
    /// is the size the box's memory may grow to, as on the web.
    func testMemoryLimitStopsTheBoxedHeap() throws {
        let js = try context(.boxed, memory: 8 << 20)
        XCTAssertThrowsError(try js.evaluate("globalThis.keep = []; for (var n = 0; n < 200; n++) keep.push(new Uint8Array(1 << 20).fill(1));", filename: "m.js")) {
            XCTAssertTrue("\($0)".lowercased().contains("out of memory"), "\($0)")
        }
        let kept = try XCTUnwrap(js.evaluate("keep.length", filename: "m.js").number)
        XCTAssertGreaterThan(kept, 3)
        XCTAssertLessThan(kept, 10, "an 8 MB limit, not the 64 MB the engine would take without one")
        XCTAssertLessThan(js.memoryUsed, 12 << 20)
        js.setMemoryLimit(32 << 20) // a prewarmed runtime adopts its host's limit
        XCTAssertNoThrow(try js.evaluate("for (var k = 0; k < 12; k++) keep.push(new Uint8Array(1 << 20));", filename: "m.js"))
        let native = try context(.native, memory: 8 << 20)
        XCTAssertThrowsError(try native.evaluate("globalThis.keep = []; for (var n = 0; n < 200; n++) keep.push(new Uint8Array(1 << 20).fill(1));", filename: "m.js"))
    }

    func testMemoryUsageReportsTheBoxedHeap() throws {
        let js = try context(.boxed, memory: 32 << 20)
        try js.evaluate("globalThis.big = Array.from({length: 20000}, (_, i) => ({i, s: 'v' + i}));", filename: "m.js")
        let before = js.memoryUsage["used"] ?? 0
        XCTAssertGreaterThan(before, 1 << 20)
        XCTAssertEqual(Set(js.memoryUsage.keys), Set(try context(.native).memoryUsage.keys))
        try js.evaluate("globalThis.big = null;", filename: "n.js")
        js.collectGarbage()
        XCTAssertLessThan(js.memoryUsage["used"] ?? .max, before / 2)
    }

    /// QuickJS limits its recursion on the box's own stack; the limit is set so that the native stack the box's
    /// code uses stays inside what the thread has (the tools' selftest measures 17 recursion shapes).
    func testRunawayRecursionIsARangeErrorOnAnEngineThread() throws {
        try XCTSkipUnless(QuickJSBackend.isBoxedAvailable)
        let thread = JSThread(name: "boxed.recursion")
        defer { thread.stop() }
        let (plain, viaForEach): (String?, String?) = thread.sync {
            let js = QuickJSContext(memoryLimitBytes: 16 << 20, maxStackBytes: JSThread.jsStackLimit, backend: .boxed)
            let a = try? js.evaluate("var d = 0; function f() { d++; f(); } try { f(); } catch (e) { e.name + ':' + (d > 1000) }", filename: "r.js").string
            let b = try? js.evaluate("var d = 0; function g() { d++; [1].forEach(g); } try { g(); } catch (e) { e.name + ':' + (d > 300) }", filename: "r.js").string
            return (a ?? nil, b ?? nil)
        }
        XCTAssertEqual(plain, "RangeError:true")
        XCTAssertEqual(viaForEach, "RangeError:true")
    }

    func testCpuDeadlineInterruptsTheBoxedEngine() throws {
        let js = try context(.boxed)
        js.cpuDeadline = QuickJSContext.threadCPUTime() + 0.05
        let t0 = QuickJSContext.threadCPUTime()
        XCTAssertThrowsError(try js.evaluate("for (;;) { try { for (;;) {} } catch (e) {} }", filename: "spin.js")) { XCTAssertTrue("\($0)".contains("interrupted"), "\($0)") }
        XCTAssertTrue(js.interrupted)
        XCTAssertLessThan(QuickJSContext.threadCPUTime() - t0, 0.5)
        js.interrupted = false
        js.cpuDeadline = .infinity
        XCTAssertFalse(js.interrupted)
        XCTAssertEqual(try js.evaluate("1 + 1", filename: "t.js").number, 2)
        js.deadline = ProcessInfo.processInfo.systemUptime - 1 // the wall-clock backstop, already past
        XCTAssertThrowsError(try js.evaluate("for (;;) {}", filename: "spin.js"))
        XCTAssertTrue(js.interrupted)
    }

    /// A host function may move the deadline while it runs (the engine does, for the time a layout query waited).
    func testDeadlinesCanBeSetFromInsideAHostFunction() throws {
        let js = try context(.boxed)
        js.registerFunction("__h") { _ in
            js.cpuDeadline = -.infinity
            return .undefined
        }
        XCTAssertThrowsError(try js.evaluate("__h(); for (;;) {}", filename: "t.js"))
        XCTAssertTrue(js.interrupted)
    }

    func testPromiseJobErrorsAreReported() throws {
        for backend in [QuickJSBackend.native, .boxed] {
            let js = try context(backend)
            try js.evaluate("Promise.resolve().then(() => { throw new TypeError('in a job'); }); Promise.resolve().then(() => { globalThis.ran = 1; });", filename: "j.js")
            XCTAssertTrue(js.hasPendingJob)
            var errors: [String] = []
            js.executePendingJobs { errors.append($0.description) }
            XCTAssertFalse(js.hasPendingJob)
            XCTAssertEqual(try js.evaluate("ran", filename: "j.js").number, 1, "\(backend)")
            XCTAssertTrue(errors.isEmpty, "\(backend): a rejection is not a job error: \(errors)")
        }
    }

    // MARK: the engine and the host on the boxed engine

    /// What the renderer received, parsed: the host writes some of its own messages from dictionaries, in no fixed key order.
    private func parsed(_ messages: [String]) -> NSArray {
        messages.map { (try? JSONSerialization.jsonObject(with: Data($0.utf8))) ?? $0 } as NSArray
    }

    private func run(_ html: String, engine: QuickJSBackend, features: Set<String> = [], pools: Bool = false) throws -> (WidgetDiagnostics, [String]) {
        let renderer = RecordingRenderer()
        var options = WidgetHost.Options()
        options.engine = engine
        options.features = features
        options.pools = pools
        let host = try WidgetHost(html: html, theme: WidgetTheme(), width: 300, options: options, rendererFactory: { _, _, _ in renderer })
        let deadline = Date().addingTimeInterval(5)
        while !(host.diagnostics.rendered || host.diagnostics.dead != nil) && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        let diagnostics = host.diagnostics
        host.destroy()
        return (diagnostics, renderer.messages)
    }

    private static let widget = """
        <style>.c { color: red }</style><div class="c" id="out">…</div><canvas id="cv" width="40" height="20"></canvas><script>
        var c = document.getElementById('cv').getContext('2d'); c.fillStyle = '#123456'; c.fillRect(1, 2, 3, 4);
        document.getElementById('out').textContent = 'sum ' + [1, 2, 3].reduce(function (a, b) { return a + b; }) + ' ' + JSON.stringify({ok: true});
        var p = document.createElement('p'); p.textContent = String(new Date(Date.UTC(2026, 0, 2)).getUTCDay()); document.body.appendChild(p);
        setTimeout(function () { p.setAttribute('data-late', 'yes'); }, 5);
        </script>
        """

    func testAWidgetOnTheBoxedEngineRendersWhatTheNativeEngineRenders() throws {
        try XCTSkipUnless(QuickJSBackend.isBoxedAvailable)
        for features in [Set<String>(), ["binaryOps", "canvasWords"]] {
            let (native, nativeMessages) = try run(Self.widget, engine: .native, features: features)
            let (boxed, boxedMessages) = try run(Self.widget, engine: .boxed, features: features)
            XCTAssertNil(boxed.dead)
            XCTAssertTrue(boxed.rendered)
            XCTAssertEqual(parsed(boxedMessages), parsed(nativeMessages), "features \(features.sorted())")
            XCTAssertGreaterThan(boxedMessages.count, 4)
            XCTAssertTrue(boxedMessages.joined().contains("sum 6 {\\\"ok\\\":true}"), "features \(features.sorted())")
            XCTAssertNotNil(boxed.diags["engine"], "a boxed host says which engine runs it")
            XCTAssertTrue(boxed.diags["engine"]?.details.first?.hasPrefix("boxed QuickJS ") == true)
            XCTAssertNil(native.diags["engine"])
            XCTAssertTrue(boxed.diags["boot"]?.details.first?.contains("(bytecode") == true, "the shipped .qbc boots the boxed engine: \(String(describing: boxed.diags["boot"]))")
        }
    }

    func testLimitsStopABoxedWidgetLikeANativeOne() throws {
        try XCTSkipUnless(QuickJSBackend.isBoxedAvailable)
        let spin = try run("<p>x</p><script>for (;;) {}</script>", engine: .boxed).0
        XCTAssertTrue(spin.dead?.hasPrefix("CPU limit exceeded") == true, String(describing: spin.dead))
        let hog = try run("<p>x</p><script>var keep = []; for (;;) keep.push(new Uint8Array(1 << 20).fill(1));</script>", engine: .boxed).0
        XCTAssertTrue(hog.dead?.hasPrefix("Memory limit exceeded") == true, String(describing: hog.dead))
        let deep = try run("<p id=o>x</p><script>function f() { f(); } try { f(); } catch (e) { document.getElementById('o').textContent = e.name; }</script>", engine: .boxed)
        XCTAssertNil(deep.0.dead)
        XCTAssertTrue(deep.1.joined().contains("RangeError"))
    }

    /// A build that cannot run the box never falls back to the native engine: the widget stops. On macOS the test
    /// makes the box fail; on a platform without it (iOS today) that is what asking for `.boxed` does anyway.
    func testABoxThatCannotBeCreatedFailsClosed() throws {
        var reason = "the boxed QuickJS engine is not part of this build (macOS only)"
        if QuickJSBackend.isBoxedAvailable {
            reason = "the box was compiled without optimization"
            QuickJSContext.boxedFailureForTesting = reason
        }
        defer { QuickJSContext.boxedFailureForTesting = nil }
        let js = QuickJSContext(memoryLimitBytes: 8 << 20, backend: .boxed)
        XCTAssertEqual(js.unavailableReason, reason)
        XCTAssertThrowsError(try js.evaluate("1", filename: "t.js")) { XCTAssertEqual("\($0)", "engine fault: \(reason)") }
        XCTAssertThrowsError(try js.call("f"))
        XCTAssertFalse(js.captureGlobalFunction("f"))
        let (diagnostics, messages) = try run("<p>never</p><script>document.body.textContent = 'ran';</script>", engine: .boxed)
        XCTAssertEqual(diagnostics.dead, "Engine fault: \(reason)")
        XCTAssertFalse(messages.joined().contains("ran"))
        XCTAssertFalse(messages.joined().contains("never"), "nothing of the widget is rendered")
        XCTAssertNil(try run("<p>fine</p>", engine: .native).0.dead)
    }

    func testThePoolHandsOutOnlyTheBackendThatWasAskedFor() throws {
        try XCTSkipUnless(QuickJSBackend.isBoxedAvailable)
        let pool = EnginePool.shared
        let (size, backend) = (pool.targetSize, pool.backend)
        defer { pool.backend = backend; pool.targetSize = size; pool.drain() }
        pool.backend = .native
        pool.targetSize = 2
        XCTAssertNil(pool.take(features: [], backend: .boxed), "a native engine is never handed to a host that asked for the box")
        XCTAssertEqual(pool.take(features: [], backend: .native)?.backend, .native)
        pool.backend = .boxed
        XCTAssertNil(pool.take(features: [], backend: .native))
        let boxed = try XCTUnwrap(pool.take(features: [], backend: .boxed))
        XCTAssertEqual(boxed.backend, .boxed)
        boxed.terminate()
        let (diagnostics, messages) = try run(Self.widget, engine: .boxed, pools: true)
        XCTAssertNil(diagnostics.dead)
        XCTAssertTrue(messages.joined().contains("sum 6"))
        XCTAssertTrue(diagnostics.diags["boot"]?.details.first?.contains("prewarmed") == true, String(describing: diagnostics.diags["boot"]))
    }
}
