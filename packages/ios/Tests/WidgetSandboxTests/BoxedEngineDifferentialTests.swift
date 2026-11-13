import XCTest
@testable import WidgetSandbox

/// The corpora through the whole host (engine, validator, a recording renderer), once on the native engine and once
/// on the boxed one: the renderer must receive the same messages (docs/decisions.md, "QuickJS in a wasm2c box").
///
/// A widget's later batches depend on when its timers and frames fall, so the native engine runs twice first. Where
/// both native runs agree on everything, the boxed run must too, as far as both got (the slower engine fits fewer
/// frames into the same time), with two more tries: agreeing twice is not always agreeing. Where they agree on the first batch only (the document after the widget's scripts ran), or the widget
/// was stopped (what was in flight then is a race), that batch is compared. A widget that already differs from itself
/// there (a clock, random numbers) says nothing and is skipped.
///
/// Two differences are expected and counted, not failed. Numbers may differ in their last digits: the box carries
/// its own libm (`Math.tan` and friends are one ulp apart from the system's). And a widget that dies of the memory
/// limit on the native engine may live on in the box: the limit counts bytes, and the box's 32-bit heap holds the
/// same objects in fewer of them.
///
/// By default a few widgets of two corpora. `WIDGET_SANDBOX_BOX_DIFFERENTIAL=full` runs every corpus, the conformance
/// set, the stress kit and the attacks (several minutes), and prints the JS time each engine took.
final class BoxedEngineDifferentialTests: XCTestCase {
    private static let root = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()

    private struct Run {
        var messages: [Any] = []
        var firstOps: Any?
        var dead: String?
        var jsMs = 0.0
    }

    private func run(_ html: String, engine: QuickJSBackend) -> Run {
        let renderer = RecordingRenderer()
        var options = WidgetHost.Options()
        options.engine = engine
        guard let host = try? WidgetHost(html: html, theme: WidgetTheme(), width: 320, options: options, rendererFactory: { _, _, _ in renderer }) else { return Run() }
        let deadline = Date().addingTimeInterval(4)
        while !(host.diagnostics.loaded || host.diagnostics.dead != nil) && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.005))
        }
        RunLoop.main.run(until: Date().addingTimeInterval(0.15))
        let d = host.diagnostics
        host.destroy()
        var r = Run(dead: d.dead, jsMs: d.stats.jsMs)
        for message in renderer.messages {
            guard let object = try? JSONSerialization.jsonObject(with: Data(message.utf8)) else { continue }
            r.messages.append(object)
            if r.firstOps == nil, (object as? [String: Any])?["type"] as? String == "ops" { r.firstOps = object }
        }
        return r
    }

    private static let number = try! NSRegularExpression(pattern: #"-?\d+(?:\.\d+)?(?:e[-+]?\d+)?"#)

    /// Equal, with numbers (also the ones inside strings, as in a `transform` or a canvas command) equal to nine digits.
    private func same(_ a: Any?, _ b: Any?) -> Bool {
        switch (a, b) {
        case (nil, nil): return true
        case (let x as NSNumber, let y as NSNumber):
            let (p, q) = (x.doubleValue, y.doubleValue)
            return p == q || abs(p - q) <= 1e-9 * max(abs(p), abs(q)) || (p.isNaN && q.isNaN)
        case (let x as String, let y as String):
            if x == y { return true }
            let (nx, ny) = (x as NSString, y as NSString)
            let (mx, my) = (Self.number.matches(in: x, range: NSRange(location: 0, length: nx.length)), Self.number.matches(in: y, range: NSRange(location: 0, length: ny.length)))
            guard mx.count == my.count, !mx.isEmpty else { return false }
            guard Self.number.stringByReplacingMatches(in: x, range: NSRange(location: 0, length: nx.length), withTemplate: "#")
                == Self.number.stringByReplacingMatches(in: y, range: NSRange(location: 0, length: ny.length), withTemplate: "#") else { return false }
            return zip(mx, my).allSatisfy { m, n in
                let (p, q) = (Double(nx.substring(with: m.range)) ?? .nan, Double(ny.substring(with: n.range)) ?? .nan)
                return p == q || abs(p - q) <= 1e-9 * max(abs(p), abs(q))
            }
        case (let x as [Any], let y as [Any]):
            return x.count == y.count && zip(x, y).allSatisfy { same($0, $1) }
        case (let x as [String: Any], let y as [String: Any]):
            return x.count == y.count && x.allSatisfy { key, value in y[key].map { same(value, $0) } ?? false }
        case (is NSNull, is NSNull): return true
        default: return false
        }
    }

    private func exact(_ a: Any?, _ b: Any?) -> Bool {
        switch (a, b) {
        case (nil, nil): return true
        case (let x as NSObject, let y as NSObject): return x.isEqual(y)
        default: return false
        }
    }

    /// The same messages as far as both runs got: the slower engine fits fewer frames into the same wall-clock window,
    /// so one run may simply be longer. Every message both sent must match, the first batch among them.
    private func agree(_ a: [Any], _ b: [Any], _ equal: (Any?, Any?) -> Bool) -> Bool {
        let n = min(a.count, b.count)
        guard n > 0, a.prefix(n).contains(where: { ($0 as? [String: Any])?["type"] as? String == "ops" }) || a.count == b.count else { return false }
        return equal(Array(a.prefix(n)), Array(b.prefix(n)))
    }

    /// Where a widget died says which engine and how many ms; the reason before that is what must match.
    private func reason(_ dead: String?) -> String? {
        dead.map { String($0.prefix(while: { $0 != ":" && $0 != "(" })) }
    }

    /// Where two runs part: the index of the first message that differs and the start of the difference in each.
    private func difference(_ a: [Any], _ b: [Any]) -> String {
        func text(_ object: Any) -> String {
            (try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])).map { String(decoding: $0, as: UTF8.self) } ?? "\(object)"
        }
        for i in 0..<max(a.count, b.count) {
            guard i < a.count, i < b.count else { return "message \(i): only one engine sent it (\(a.count) native, \(b.count) boxed): \(text(i < a.count ? a[i] : b[i]).prefix(300))" }
            if exact(a[i], b[i]) { continue }
            let (x, y) = (Array(text(a[i]).unicodeScalars), Array(text(b[i]).unicodeScalars))
            let at = zip(x, y).prefix(while: { $0 == $1 }).count
            func cut(_ s: [Unicode.Scalar]) -> String { String(String.UnicodeScalarView(s[max(0, at - 60)..<min(s.count, at + 120)])) }
            return "message \(i) of \(a.count)/\(b.count), at character \(at):\n  native: \(cut(x))\n  boxed:  \(cut(y))"
        }
        return "no difference"
    }

    private func widgets(_ full: Bool) -> [(String, URL)] {
        // One widget by its path from the repository root, to look at a difference.
        if let only = ProcessInfo.processInfo.environment["WIDGET_SANDBOX_BOX_DIFFERENTIAL"], only.hasSuffix(".html") {
            return [(only, Self.root.appendingPathComponent(only))]
        }
        var groups = ["corpus/compat", "corpus/showcase/commerce"]
        if full {
            groups = ["corpus/compat", "corpus/defaults", "corpus/interactive", "corpus/gestures", "corpus/resize", "corpus/safe-area", "corpus/attacks",
                      "packages/conformance/auto", "packages/conformance/visual"]
            for nested in ["corpus/showcase", "corpus/generated", "packages/stress/widgets"] {
                let dirs = (try? FileManager.default.contentsOfDirectory(atPath: Self.root.appendingPathComponent(nested).path)) ?? []
                groups += dirs.sorted().map { nested + "/" + $0 }
            }
        }
        var out: [(String, URL)] = []
        for group in groups {
            let dir = Self.root.appendingPathComponent(group)
            let files = ((try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []).filter { $0.hasSuffix(".html") }.sorted()
            for file in full ? files : Array(files.prefix(4)) { out.append((group + "/" + file, dir.appendingPathComponent(file))) }
        }
        return out
    }

    func testCorpusWidgetsRenderTheSameOnBothEngines() throws {
        try XCTSkipUnless(QuickJSBackend.isBoxedAvailable, "the boxed engine is not part of this build")
        let full = ProcessInfo.processInfo.environment["WIDGET_SANDBOX_BOX_DIFFERENTIAL"] == "full"
        let widgets = widgets(full)
        guard !widgets.isEmpty else { throw XCTSkip("the corpora are not readable from this test process") }
        var whole = 0, firstBatch = 0, skipped = 0, lastDigits: [String] = [], memory: [String] = []
        var nativeMs = 0.0, boxedMs = 0.0, groupMs: [String: (native: Double, boxed: Double)] = [:]
        for (name, url) in widgets {
            let html = try String(contentsOf: url, encoding: .utf8)
            let a = run(html, engine: .native), b = run(html, engine: .native)
            let boxed = run(html, engine: .boxed)
            XCTAssertFalse(boxed.dead?.hasPrefix("Engine fault") == true, "\(name): \(boxed.dead ?? "")")
            let group = name.split(separator: "/").prefix(2).joined(separator: "/")
            nativeMs += min(a.jsMs, b.jsMs)
            boxedMs += boxed.jsMs
            groupMs[group, default: (0, 0)].native += min(a.jsMs, b.jsMs)
            groupMs[group, default: (0, 0)].boxed += boxed.jsMs

            // How the widget ended. The memory limit counts bytes, and the box's 32-bit heap holds the same objects
            // in fewer of them: a widget that dies of it on the native engine may live on in the box, to its next limit.
            if reason(a.dead) == reason(b.dead), reason(boxed.dead) != reason(a.dead) {
                if a.dead?.hasPrefix("Memory limit exceeded") == true {
                    memory.append(name)
                    continue
                }
                XCTFail("\(name): the boxed widget ended differently (\(boxed.dead ?? "alive") against \(a.dead ?? "alive"))")
            }
            // A widget that was stopped: what was in flight when it stopped is a race between threads. The first batch is not.
            let stopped = a.dead != nil || b.dead != nil || boxed.dead != nil
            if !stopped, exact(a.messages, b.messages) {
                // Later batches follow timers and frames: two native runs that agree do not prove they always do.
                // So a boxed run that differs gets two more tries against fresh native runs. If the engine then
                // differs from itself between tries, the widget follows the clock: only its first batch can be compared.
                var natives = [a, b], boxeds = [boxed]
                while !agree(boxeds.last!.messages, natives.last!.messages, same), boxeds.count < 3 {
                    natives.append(run(html, engine: .native))
                    boxeds.append(run(html, engine: .boxed))
                }
                let (native, last) = (natives.last!, boxeds.last!)
                if agree(last.messages, native.messages, exact) { whole += 1; continue }
                if agree(last.messages, native.messages, same) { whole += 1; lastDigits.append(name); continue }
                let steady = natives.allSatisfy { agree($0.messages, a.messages, same) } && boxeds.allSatisfy { agree($0.messages, boxed.messages, same) }
                if steady {
                    XCTFail("\(name): the renderer received other messages from the boxed engine: \(difference(native.messages, last.messages))")
                    continue
                }
                firstBatch += 1
                if exact(boxed.firstOps, a.firstOps) { continue }
                if same(boxed.firstOps, a.firstOps) { lastDigits.append(name); continue }
                XCTFail("\(name): the first batch differs on the boxed engine: \(difference([a.firstOps as Any], [boxed.firstOps as Any]))")
            } else if a.firstOps != nil, exact(a.firstOps, b.firstOps) {
                firstBatch += 1
                if exact(boxed.firstOps, a.firstOps) { continue }
                if same(boxed.firstOps, a.firstOps) { lastDigits.append(name); continue }
                XCTFail("\(name): the first batch differs on the boxed engine: \(difference([a.firstOps as Any], [boxed.firstOps as Any]))")
            } else {
                skipped += 1
            }
        }
        print("boxed against native: \(widgets.count) widgets; \(whole) compared on every message, \(firstBatch) on the first batch, \(skipped) differ from themselves (skipped); "
            + "\(lastDigits.count) differ in the last digits of numbers only\(lastDigits.isEmpty ? "" : ": " + lastDigits.joined(separator: ", ")); "
            + "\(memory.count) die of the memory limit on the native engine only\(memory.isEmpty ? "" : ": " + memory.joined(separator: ", "))")
        print(String(format: "JS time to loaded + 150 ms: native %.0f ms, boxed %.0f ms (x%.2f)", nativeMs, boxedMs, nativeMs > 0 ? boxedMs / nativeMs : 0))
        if full {
            for (group, ms) in groupMs.sorted(by: { $0.key < $1.key }) where ms.native >= 5 {
                print(String(format: "  %@: native %.0f ms, boxed %.0f ms (x%.2f)", group, ms.native, ms.boxed, ms.boxed / ms.native))
            }
        }
        if widgets.count > 1 { XCTAssertGreaterThan(whole + firstBatch, widgets.count / 2, "most widgets are comparable") }
    }
}
