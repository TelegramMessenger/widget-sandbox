import XCTest
@testable import WidgetSandbox

final class IconExpansionTests: XCTestCase {
    /// A raster icon: SVG images are outside widget profile 1 (docs/profile-v1.md), so a resolver answers with PNG, JPEG, GIF or WebP.
    static let airplane = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="

    static let stubResolver: WidgetAssetResolver = { icon in
        icon.hasPrefix("icon:airplane") ? airplane : nil
    }

    private final class Box {
        var diags: [String] = []
        var asked: [String] = []
    }

    private static let skeleton = "[{\"op\":\"el\",\"id\":2,\"tag\":\"html\"},{\"op\":\"ins\",\"parent\":1,\"id\":2,\"before\":null},{\"op\":\"el\",\"id\":3,\"tag\":\"body\"},{\"op\":\"ins\",\"parent\":2,\"id\":3,\"before\":null}]"

    private func validator(native: Bool, box: Box, resolver: WidgetAssetResolver? = IconExpansionTests.stubResolver) throws -> TrustedValidator {
        let v = try TrustedValidator(theme: "light") { _, _ in }
        v.onDiag = { k, d in box.diags.append(k + "|" + d) }
        if native { v.useNativeModel(true) }
        v.setAssetResolver(resolver.map { r in { icon in box.asked.append(icon); return r(icon) } })
        _ = v.processOps(Self.skeleton)
        return v
    }

    private func image(_ id: Int, _ src: String) -> String {
        let quoted = String(decoding: try! JSONSerialization.data(withJSONObject: [src], options: [.fragmentsAllowed]), as: UTF8.self)
        return "{\"op\":\"el\",\"id\":\(id),\"tag\":\"img\",\"attrs\":{\"src\":\(quoted.dropFirst().dropLast())}},{\"op\":\"ins\",\"parent\":3,\"id\":\(id),\"before\":null}"
    }

    func testIconsBecomeDataURLsBeforeValidation() throws {
        for native in [false, true] {
            let box = Box()
            let v = try validator(native: native, box: box)
            let out = v.processOps("[\(image(10, "icon:airplane")),{\"op\":\"style\",\"id\":10,\"name\":\"background-image\",\"value\":\"url(icon:airplane)\",\"important\":false}]")
            XCTAssertTrue(out.contains(Self.airplane), "native \(native) \(out.prefix(300))")
            XCTAssertFalse(out.contains("icon:airplane"), "native \(native)")
        }
    }

    func testWithoutAResolverIconsStayBlocked() throws {
        for native in [false, true] {
            let box = Box()
            let v = try validator(native: native, box: box, resolver: nil)
            let out = v.processOps("[\(image(10, "icon:airplane"))]")
            XCTAssertFalse(out.contains(Self.airplane), "native \(native)")
            XCTAssertFalse(out.contains("icon:airplane"), "native \(native) \(out.prefix(300))")
        }
    }

    func testResolverOutputThatIsNotAnInlineImageIsIgnored() throws {
        let hostile = [
            "https://evil.example/x.png", "//evil.example/x.png", "javascript:alert(1)", "data:text/html,%3Cscript%3E", "DATA:image/png;base64,AAAA",
            "data:image/svg+xml,\"onload=alert(1)", "data:image/svg+xml,a)b", "data:image/svg+xml,a b", "data:image/svg+xml,\\u0022", "data:image/svg+xml,<svg/onload=1>",
            "data:image/png;base64," + String(repeating: "A", count: IconExpansion.maxURLBytes), "data:image/", "blob:null/1", "",
        ]
        for url in hostile {
            XCTAssertNil(IconExpansion.checked(url), url)
            for native in [false, true] {
                let box = Box()
                let v = try validator(native: native, box: box, resolver: { _ in url })
                let out = v.processOps("[\(image(10, "icon:airplane")),{\"op\":\"style\",\"id\":10,\"name\":\"background\",\"value\":\"url(icon:airplane)\",\"important\":false}]")
                XCTAssertFalse(out.contains("evil.example") || out.contains("javascript:") || out.contains("text/html") || out.contains("onload") || out.contains("blob:"), "native \(native) \(url): \(out.prefix(300))")
            }
        }
        XCTAssertEqual(IconExpansion.checked(Self.airplane), Self.airplane)
    }

    func testResolverOnlySeesTokensFromTheGrammar() throws {
        let box = Box()
        let v = try validator(native: false, box: box)
        let long = "icon:" + String(repeating: "a", count: 300)
        let tricky = ["icon:../../etc/passwd", "icon:a\"b", "icon:a<b>", "icon:a b", "icon:a?x=1&y=<", "icon:%2e%2e", "icon:a\\u0000b", long, "icon:a?" + String(repeating: "q", count: 600), "icon:airplane?weight=bold"]
        _ = v.processOps("[{\"op\":\"el\",\"id\":30,\"tag\":\"div\",\"attrs\":{}},{\"op\":\"style\",\"id\":30,\"name\":\"background\",\"value\":\"url(\(long)) url(icon:a?\(String(repeating: "q", count: 600)))\",\"important\":false}]")
        _ = v.processOps("[" + tricky.enumerated().map { image(10 + $0.offset, $0.element) }.joined(separator: ",") + "]")
        let grammar = try NSRegularExpression(pattern: "\\Aicon:[A-Za-z0-9_.-]{0,96}(?:\\?[A-Za-z0-9=&.%#_-]{0,400})?\\z", options: [.caseInsensitive])
        let asked = v.thread.sync { box.asked }
        XCTAssertFalse(asked.isEmpty)
        for icon in asked {
            XCTAssertNotNil(grammar.firstMatch(in: icon, range: NSRange(location: 0, length: (icon as NSString).length)), icon)
            XCTAssertLessThanOrEqual(icon.utf8.count, 5 + 96 + 1 + 400)
        }
    }

    func testExpansionIsCappedPerBatch() throws {
        for native in [false, true] {
            let box = Box()
            let v = try validator(native: native, box: box)
            let value = (0..<3000).map { _ in "url(icon:airplane)" }.joined(separator: ",")
            let batch = "[{\"op\":\"el\",\"id\":10,\"tag\":\"div\",\"attrs\":{}},{\"op\":\"ins\",\"parent\":3,\"id\":10,\"before\":null},{\"op\":\"style\",\"id\":10,\"name\":\"background-image\",\"value\":\"\(value)\",\"important\":false}]"
            _ = v.processOps(batch)
            let asked = v.thread.sync { box.asked.count }
            XCTAssertLessThanOrEqual(asked, IconExpansion.maxIconsPerBatch, "native \(native)")
            XCTAssertEqual(v.thread.sync { box.diags.filter { $0.hasPrefix("icon-limit|") }.count }, 1, "native \(native)")
            box.diags.removeAll()
            let out = v.processOps("[\(image(11, "icon:airplane"))]")
            XCTAssertTrue(out.contains(Self.airplane), "the budget is per batch")
        }
        let big = "data:image/png;base64," + String(repeating: "A", count: IconExpansion.maxURLBytes - 30)
        var diags: [String] = []
        let icons = IconExpansion(resolver: { _ in big }) { k, _ in diags.append(k) }
        icons.beginBatch()
        let text = "[\"" + (0..<100).map { "url(icon:n\($0))" }.joined(separator: " ") + "\"]"
        let out = icons.expand(text)
        XCTAssertLessThanOrEqual(out.utf8.count - text.utf8.count, IconExpansion.maxBytesPerBatch)
        XCTAssertEqual(diags, ["icon-limit"])
    }

    func testTheIconPrefixIsExactlyLowercase() throws {
        for native in [false, true] {
            let box = Box()
            let v = try validator(native: native, box: box)
            let out = v.processOps("[\(image(10, "ICON:airplane")),\(image(11, "Icon:airplane")),{\"op\":\"el\",\"id\":12,\"tag\":\"div\",\"attrs\":{}},{\"op\":\"ins\",\"parent\":3,\"id\":12,\"before\":null},{\"op\":\"style\",\"id\":12,\"name\":\"background-image\",\"value\":\"url(ICON:airplane), url('Icon:airplane')\",\"important\":false}]")
            XCTAssertFalse(out.contains(Self.airplane), "native \(native) \(out.prefix(300))")
            XCTAssertEqual(v.thread.sync { box.asked }, [], "native \(native)")
            let css = v.processOps("[{\"op\":\"style\",\"id\":12,\"name\":\"background-image\",\"value\":\"URL(icon:airplane)\",\"important\":false}]")
            XCTAssertTrue(css.contains(Self.airplane), "the CSS function name stays case-insensitive; native \(native) \(css.prefix(300))")
        }
    }

    func testDistinctIconsAreLimitedPerWidget() {
        var asked: [String] = []
        var diags: [String] = []
        let icons = IconExpansion(resolver: { icon in asked.append(icon); return Self.airplane }) { k, _ in diags.append(k) }
        for batch in 0..<3 {
            icons.beginBatch()
            let text = "[\"" + (0..<200).map { "url(icon:n\(batch * 200 + $0))" }.joined(separator: " ") + "\"]"
            _ = icons.expand(text)
        }
        XCTAssertEqual(asked.count, IconExpansion.maxIconsPerWidget)
        XCTAssertEqual(diags, ["icon-limit"], "reported once per widget")
        icons.beginBatch()
        XCTAssertTrue(icons.expand("[\"url(icon:n0) url(icon:n599)\"]").contains("url(" + Self.airplane + ") url(icon:n599)"), "known tokens keep working, new ones stay unresolved")
        XCTAssertEqual(asked.count, IconExpansion.maxIconsPerWidget)

        let big = "data:image/png;base64," + String(repeating: "A", count: 60_000)
        var bigAsked = 0
        var bigDiags: [String] = []
        let heavy = IconExpansion(resolver: { _ in bigAsked += 1; return big }) { k, _ in bigDiags.append(k) }
        var expanded = 0
        for batch in 0..<10 {
            heavy.beginBatch()
            let out = heavy.expand("[\"" + (0..<10).map { "url(icon:b\(batch * 10 + $0))" }.joined(separator: " ") + "\"]")
            expanded += out.components(separatedBy: big).count - 1
        }
        XCTAssertEqual(expanded, IconExpansion.maxResolvedBytesPerWidget / big.utf8.count)
        XCTAssertLessThanOrEqual(bigAsked, expanded + 1)
        XCTAssertEqual(bigDiags, ["icon-limit"])
    }

    private func render(_ html: String, options: WidgetHost.Options) -> (RecordingRenderer, WidgetHost) {
        let renderer = RecordingRenderer()
        let host = try! WidgetHost(html: html, theme: "light", width: 300, options: options, rendererFactory: { _, _, _ in renderer })
        let deadline = Date().addingTimeInterval(10)
        while !(renderer.messages.joined().contains("\"img\"") || host.diagnostics.dead != nil) && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        return (renderer, host)
    }

    func testTheHostResolverReachesARunningWidget() {
        let html = "<img id=a src=\"icon:airplane\"><div id=b></div><script>document.getElementById('b').style.backgroundImage = 'url(icon:airplane?weight=bold)'; const i = document.createElement('img'); i.src = 'icon:airplane'; document.body.appendChild(i);</script>"
        for binary in [false, true] {
            var asked: [String] = []
            let lock = NSLock()
            var options = WidgetHost.Options()
            if binary { options.features = ["binaryOps"] }
            options.assetResolver = { icon in lock.withLock { asked.append(icon) }; return Self.stubResolver(icon) }
            let (renderer, host) = render(html, options: options)
            let sent = renderer.messages.joined()
            XCTAssertTrue(sent.contains(Self.airplane), "binary \(binary): \(sent.prefix(400))")
            XCTAssertFalse(sent.contains("[\"src\",\"icon:") || sent.contains("\"value\":\"url(icon:"), "binary \(binary)")
            XCTAssertTrue(sent.contains("\"name\":\"background-image\",\"value\":\"url(\"" + Self.airplane) || sent.contains("url(" + Self.airplane), "binary \(binary)")
            XCTAssertGreaterThanOrEqual(lock.withLock { asked.count }, 2, "binary \(binary)")
            host.destroy()

            var plain = WidgetHost.Options()
            if binary { plain.features = ["binaryOps"] }
            let (without, other) = render(html, options: plain)
            XCTAssertFalse(without.messages.joined().contains(Self.airplane), "binary \(binary)")
            other.destroy()
        }
    }

    func testBinaryBatchesExpandIconsThroughTheNativeModel() throws {
        let box = Box()
        let v = try validator(native: true, box: box)
        var encoder = BinaryOpsEncoder()
        encoder.begin()
        encoder.element(10, tag: "img", svg: false, attributes: [("src", "icon:airplane")])
        encoder.insert(parent: 3, 10, before: 0)
        encoder.style(10, "background-image", "url(icon:airplane)", important: false)
        let batch = v.processBinary(encoder.finish(), typed: false)
        guard case .json(let out) = batch else { return XCTFail("\(batch)") }
        XCTAssertTrue(out.contains(Self.airplane), out)
    }
}
