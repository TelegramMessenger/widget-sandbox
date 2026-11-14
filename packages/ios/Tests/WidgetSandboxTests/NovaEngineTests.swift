import XCTest
@testable import WidgetSandbox

/// The Nova engine (packages/nova-engine) behind the same `QuickJSContext` as QuickJS: the same host functions with the
/// same arguments, the same answers to the sandbox's own needs, the bundles from source, and a widget rendered the same.
/// A build of the library that does not enforce all three limits (interrupt, memory, stack) is refused.
final class NovaEngineTests: XCTestCase {
    private func context(_ backend: QuickJSBackend, memory: Int = 16 << 20) throws -> QuickJSContext {
        if backend == .nova { try XCTSkipUnless(QuickJSBackend.isNovaAvailable, "the Nova engine is not part of this build") }
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

    func testTheEngineSaysWhatItIs() throws {
        let js = try context(.nova)
        XCTAssertTrue(js.engineDescription.hasPrefix("nova_embed "), js.engineDescription)
        XCTAssertNil(js.nativeContext)
    }

    private static let scripts = [
        "[1, 2, 3].map((x) => x * 2).join('-')",
        "6 * 7", "0.1 + 0.2", "-0", "1 / 3", "1e21", "NaN", "({})", "undefined", "null", "true", "12345678901234567890n",
        "'a\\u0000b\\u00e9\\ud83c\\udf89'", "''", "'x'.repeat(100000).length",
        "throw new RangeError('r')", "throw 'plain'", "throw {name: 'N', message: 'm'}",
        "JSON.stringify({a: [1, {b: null, c: 'x\\u2028y'}], n: 1e21, e: -0})",
        "[3, 1, 10, 2].sort().join() + [3, 1, 10, 2].sort((a, b) => a - b).join()",
        "Array.from(new Uint8Array(new Float64Array([1.5, -0]).buffer)).join()",
        "new Date(Date.UTC(2026, 10, 12, 22, 21, 5, 123)).toISOString()",
        "(123.456).toFixed(2) + (255).toString(16) + parseFloat('3.14e-2x')",
        "var log = []; Promise.resolve(1).then((v) => log.push('a' + v)); Promise.reject(new Error('x')).catch((e) => log.push(e.message)); 'sync'",
        "log.join()",
        "function* g(n) { for (let i = 0; i < n; i++) yield i * i; } [...g(5)].join()",
        "class A { #p = 1; get p() { return this.#p; } } new A().p",
        "typeof Atomics + typeof WeakRef + typeof globalThis.os + typeof print",
        "[] + {}", "2 ** 53 + 2", "Math.sqrt(2) + Math.floor(-1.5) + Math.abs(-3) + Math.max(1, 2) + Math.round(2.5)",
    ]

    /// Differences in the engine itself (the compatibility track of nova-safe), not in how it is embedded: each one is
    /// expected to fail until Nova is fixed, and the test says when one starts to pass.
    private static let engineDifferences = [
        "typeof Atomics + typeof WeakRef + typeof globalThis.os + typeof print": "by design: the widget build has no Atomics or SharedArrayBuffer",
    ]

    /// An exception's first line: QuickJS adds the stack after it, Nova has none.
    private func firstLine(_ answer: String) -> String {
        answer.hasPrefix("threw:") ? String(answer.prefix(while: { $0 != "\n" })) : answer
    }

    func testSameAnswersAsTheNativeEngine() throws {
        let native = try context(.native), nova = try context(.nova)
        for script in Self.scripts {
            let (expected, actual) = (firstLine(answer(native, script)), firstLine(answer(nova, script)))
            if let why = Self.engineDifferences[script] {
                XCTExpectFailure("Nova: \(why)") { XCTAssertEqual(actual, expected, script) }
            } else {
                XCTAssertEqual(actual, expected, script)
            }
        }
        XCTAssertEqual(nova.hasPendingJob, native.hasPendingJob)
    }

    func testSyntaxErrorsAndMissingFunctions() throws {
        let js = try context(.nova)
        XCTAssertTrue(answer(js, "var 1x").hasPrefix("threw:SyntaxError: "), answer(js, "var 1x"))
        XCTAssertThrowsError(try js.call("missing")) { XCTAssertEqual("\($0)", "missing is not a function") }
        XCTAssertTrue(answer(js, "import('x').then(() => 1, (e) => { globalThis.imp = e.name; }); 0").hasPrefix("number:0"))
    }

    func testStringsCrossByLength() throws {
        let js = try context(.nova)
        try js.evaluate("globalThis.echo = (s) => s + '|' + s.length;", filename: "t.js")
        XCTAssertEqual(try js.call("echo", ["a\u{0}b\u{0}é"]).string, "a\u{0}b\u{0}é|5")
        XCTAssertEqual(try js.evaluate("('x\\u0000y')", filename: "u.js").string, "x\u{0}y")
        XCTAssertEqual(try js.call("echo", [""]).string, "|0")
        XCTAssertEqual(try js.call("echo", [String(repeating: "я", count: 300_000)]).string?.hasSuffix("|300000"), true)
    }

    func testCapturedFunctionOutlivesItsGlobal() throws {
        let js = try context(.nova)
        try js.evaluate("globalThis.f = () => 'first';", filename: "t.js")
        XCTAssertTrue(js.captureGlobalFunction("f"))
        XCTAssertFalse(js.captureGlobalFunction("Math"))
        try js.evaluate("delete globalThis.f;", filename: "t.js")
        XCTAssertEqual(try js.call("f").string, "first")
        try js.evaluate("globalThis.f = () => 'second';", filename: "t.js")
        XCTAssertEqual(try js.call("f").string, "first", "the captured one is called")
        XCTAssertTrue(js.captureGlobalFunction("f"))
        XCTAssertEqual(try js.call("f").string, "second")
    }

    func testHostFunctionsSeeWhatTheNativeEngineGivesThem() throws {
        var seen: [QuickJSBackend: [[String]]] = [:]
        var answers: [QuickJSBackend: String] = [:]
        for backend in [QuickJSBackend.native, .nova] {
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
        XCTAssertEqual(answers[.nova], answers[.native])
        XCTAssertEqual(answers[.nova], "string:undefined|null|-1.25|caf\u{e9} \u{1F389}|cut|true|1,2,3,0,255|0|200000")
        XCTAssertEqual(seen[.nova], seen[.native])
    }

    func testWordsAndOpsFunctions() throws {
        var seen: [QuickJSBackend: [String]] = [:]
        for backend in [QuickJSBackend.native, .nova] {
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
        XCTAssertEqual(seen[.nova], seen[.native])
        XCTAssertEqual(seen[.nova]?.count, 5)
    }

    func testPromiseJobErrorsAreReported() throws {
        let js = try context(.nova)
        try js.evaluate("Promise.resolve().then(() => { throw new TypeError('in a job'); }); Promise.resolve().then(() => { globalThis.ran = 1; });", filename: "j.js")
        XCTAssertTrue(js.hasPendingJob)
        var errors: [String] = []
        js.executePendingJobs { errors.append($0.description) }
        XCTAssertFalse(js.hasPendingJob)
        XCTAssertEqual(try js.evaluate("ran", filename: "j.js").number, 1)
        XCTAssertTrue(errors.isEmpty, "a rejection is not a job error: \(errors)")
    }

    func testTheBundlesRunFromSource() throws {
        let r = SandboxResources.shared
        for (name, source) in [("sandbox-bundle.js", r.sandboxBundle), ("validator-bundle.js", r.validatorBundle)] {
            let js = try context(.nova, memory: 256 << 20)
            js.registerFunction("__host") { _ in .undefined }
            js.registerFunction("__hostBinary") { _ in .null }
            js.registerFunction("__twNode") { _ in .string("") }
            js.registerFunction("__diag") { _ in .undefined }
            XCTAssertThrowsError(try js.evaluateBytecode(r.sandboxBytecode ?? Data(), source: source))
            XCTAssertNoThrow(try js.evaluate(source, filename: name), name)
            XCTAssertEqual(try js.evaluate("typeof __twDispatch + typeof __twValidator", filename: "t.js").string, name.hasPrefix("sandbox") ? "functionundefined" : "undefinedobject")
        }
    }

    func testCpuDeadlineInterruptsTheEngine() throws {
        try XCTSkipUnless(QuickJSBackend.isNovaAvailable)
        let js = try context(.nova)
        js.cpuDeadline = QuickJSContext.threadCPUTime() + 0.05
        let t0 = QuickJSContext.threadCPUTime()
        XCTAssertThrowsError(try js.evaluate("for (;;) { try { for (;;) {} } catch (e) {} finally { } }", filename: "spin.js")) { XCTAssertTrue("\($0)".contains("interrupted"), "\($0)") }
        XCTAssertTrue(js.interrupted)
        XCTAssertLessThan(QuickJSContext.threadCPUTime() - t0, 0.5)
        js.interrupted = false
        js.cpuDeadline = .infinity
        XCTAssertEqual(try js.evaluate("1 + 1", filename: "t.js").number, 2)
        js.registerFunction("__h") { _ in
            js.cpuDeadline = -.infinity
            return .undefined
        }
        XCTAssertThrowsError(try js.evaluate("__h(); for (;;) {}", filename: "t.js"))
        XCTAssertTrue(js.interrupted)
    }

    /// Recursion in JavaScript and in the engine's own native code (bound functions, proxies, JSON, the parser) ends with
    /// a catchable error or a termination on an engine thread, never a stack overflow.
    func testRunawayRecursionIsARangeErrorOnAnEngineThread() throws {
        try XCTSkipUnless(QuickJSBackend.isNovaAvailable)
        let probes = [
            "var d = 0; function f() { d++; f(); } try { f(); } catch (e) { e.name + ':' + (d > 1000) }",
            "var d = 0; function g() { d++; [1].forEach(g); } try { g(); } catch (e) { e.name + ':' + (d > 300) }",
            "var f = function () {}; for (var i = 0; i < 1e5; i++) f = f.bind(null); try { f(); 'ran' } catch (e) { e.name }",
            "var p = { x: 1 }; for (var i = 0; i < 1e5; i++) p = new Proxy(p, {}); try { p.x } catch (e) { e.name }",
            "var a = []; for (var i = 0; i < 1e5; i++) a = [a]; try { JSON.stringify(a) } catch (e) { e.name }",
            "try { eval('['.repeat(1e5) + ']'.repeat(1e5)) } catch (e) { e.name }",
        ]
        let thread = JSThread(name: "nova.recursion")
        defer { thread.stop() }
        let answers: [String] = thread.sync {
            probes.map { probe in
                let js = QuickJSContext(memoryLimitBytes: 64 << 20, maxStackBytes: JSThread.jsStackLimit, backend: .nova)
                do {
                    let r = try js.evaluate(probe, filename: "r.js")
                    return r.string ?? "\(r)"
                } catch {
                    return "threw:\(error)"
                }
            }
        }
        XCTAssertEqual(answers[0], "RangeError:true")
        XCTAssertEqual(answers[1], "RangeError:true")
        for (probe, answer) in zip(probes.dropFirst(2), answers.dropFirst(2)) {
            XCTAssertTrue(["RangeError", "SyntaxError", "InternalError", "threw:InternalError: out of memory"].contains(answer), "\(probe): \(answer)")
        }
    }

    func testLocalTimeIsTheSystemTimeZone() throws {
        let native = try context(.native), nova = try context(.nova)
        for script in [
            "new Date(2026, 0, 15, 12).getTimezoneOffset() + '|' + new Date(2026, 6, 15, 12).getTimezoneOffset()",
            "new Date(Date.UTC(2026, 10, 14, 1, 2, 3)).getHours() + ':' + new Date(Date.UTC(2026, 10, 14, 1, 2, 3)).getMinutes()",
            "new Date(2026, 2, 29, 2, 30).getTime()",
            "new Date(2026, 9, 25, 2, 30).getTime()",
            "String(new Date(Date.UTC(2026, 0, 2, 3, 4, 5)))",
            "new Date(Date.UTC(2026, 6, 2, 3, 4, 5)).toTimeString()",
        ] {
            XCTAssertEqual(answer(nova, script), answer(native, script), script)
        }
    }

    func testLimitsStopANovaWidgetLikeANativeOne() throws {
        try XCTSkipUnless(QuickJSBackend.isNovaAvailable)
        let spin = try run("<p>x</p><script>for (;;) { try { for (;;) {} } catch (e) {} }</script>", engine: .nova).0
        XCTAssertTrue(spin.dead?.hasPrefix("CPU limit exceeded") == true, String(describing: spin.dead))
        let regexp = try run("<p>x</p><script>var s = 'a'.repeat(40) + 'b'; for (;;) { try { /(a+)+$/.test(s); } catch (e) {} }</script>", engine: .nova).0
        XCTAssertTrue(regexp.dead?.hasPrefix("CPU limit exceeded") == true, String(describing: regexp.dead))
        let hog = try run("<p>x</p><script>var keep = []; for (;;) keep.push(new Uint8Array(1 << 20).fill(1));</script>", engine: .nova).0
        XCTAssertTrue(hog.dead?.hasPrefix("Memory limit exceeded") == true, String(describing: hog.dead))
        let deep = try run("<p id=o>x</p><script>function f() { f(); } try { f(); } catch (e) { document.getElementById('o').textContent = e.name; }</script>", engine: .nova)
        XCTAssertNil(deep.0.dead)
        XCTAssertTrue(deep.1.joined().contains("RangeError"))
    }

    func testMemoryLimitStopsTheHeap() throws {
        try XCTSkipUnless(QuickJSBackend.isNovaAvailable)
        let js = try context(.nova, memory: 8 << 20)
        XCTAssertThrowsError(try js.evaluate("globalThis.keep = []; for (var n = 0; n < 200; n++) keep.push(new Uint8Array(1 << 20).fill(1));", filename: "m.js")) {
            XCTAssertTrue("\($0)".lowercased().contains("out of memory"), "\($0)")
        }
        XCTAssertGreaterThan(js.memoryUsed, 0)
    }

    // MARK: the engine and the host on Nova

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

    func testAWidgetOnNovaRendersWhatTheNativeEngineRenders() throws {
        try XCTSkipUnless(QuickJSBackend.isNovaAvailable)
        for features in [Set<String>(), ["binaryOps", "canvasWords"]] {
            let (native, nativeMessages) = try run(Self.widget, engine: .native, features: features)
            let (nova, novaMessages) = try run(Self.widget, engine: .nova, features: features)
            XCTAssertNil(nova.dead)
            XCTAssertTrue(nova.rendered)
            XCTAssertEqual(parsed(novaMessages), parsed(nativeMessages), "features \(features.sorted())")
            XCTAssertTrue(novaMessages.joined().contains("sum 6 {\\\"ok\\\":true}"), "features \(features.sorted())")
            XCTAssertTrue(nova.diags["engine"]?.details.first?.hasPrefix("nova_embed ") == true, String(describing: nova.diags["engine"]))
            XCTAssertNil(native.diags["engine"])
            XCTAssertTrue(nova.diags["boot"]?.details.first?.contains("(source") == true, String(describing: nova.diags["boot"]))
            XCTAssertNil(nova.diags["bytecode"], "no fallback diag: Nova never tries the .qbc")
        }
    }

    func testTheValidatorRunsOnNova() throws {
        try XCTSkipUnless(QuickJSBackend.isNovaAvailable)
        let saved = QuickJSBackend.validatorDefault
        defer { QuickJSBackend.validatorDefault = saved }
        QuickJSBackend.validatorDefault = .nova
        let (nova, messages) = try run(Self.widget, engine: .nova)
        XCTAssertNil(nova.dead)
        XCTAssertTrue(messages.joined().contains("sum 6"))
    }

    func testThePoolHandsOutOnlyTheBackendThatWasAskedFor() throws {
        try XCTSkipUnless(QuickJSBackend.isNovaAvailable)
        let pool = EnginePool.shared
        let (size, backend) = (pool.targetSize, pool.backend)
        defer { pool.backend = backend; pool.targetSize = size; pool.drain() }
        pool.backend = .nova
        pool.targetSize = 2
        XCTAssertNil(pool.take(features: [], backend: .native))
        let nova = try XCTUnwrap(pool.take(features: [], backend: .nova))
        XCTAssertEqual(nova.backend, .nova)
        nova.terminate()
        let (diagnostics, messages) = try run(Self.widget, engine: .nova, pools: true)
        XCTAssertNil(diagnostics.dead)
        XCTAssertTrue(messages.joined().contains("sum 6"))
    }
}
