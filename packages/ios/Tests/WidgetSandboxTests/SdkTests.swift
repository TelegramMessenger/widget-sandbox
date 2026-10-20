import XCTest
@testable import WidgetSandbox

/// SDK v1 through the real sandbox bundle in QuickJS and the real host glue (docs/sdk-v1.md §2–§4.1):
/// the Swift twin of packages/core/tests/unit/sdk.test.ts. The checks themselves are SdkPolicyTests.
final class SdkTests: XCTestCase {
    typealias Harness = UserActivationTests.Harness

    // MARK: links

    func testTapOpensOneNormalisedLinkAndBlocksTheSecond() {
        let h = Harness("<button id='b'>go</button><script>document.getElementById('b').addEventListener('click',function(){TelegramWidget.openLink('HTTPS://Example.com/поиск');TelegramWidget.openLink('https://example.com/second');});</script>")
        h.click(5)
        h.pump { !h.privileged.isEmpty }
        h.settle()
        XCTAssertEqual(h.actions, [.openLink(URL(string: "https://example.com/%D0%BF%D0%BE%D0%B8%D1%81%D0%BA")!)])
        XCTAssertEqual(h.blocked, 1)
    }

    func testInvalidLinksThrowInTheWidgetAndReachNothing() {
        let h = Harness("<script>var e=[];function t(m,u){try{TelegramWidget[m](u);}catch(x){e.push(m+':'+x.name);}}t('openLink','http://example.com/');t('openLink','https://t.me/x');t('openTelegramLink','tg://resolve?domain=x');t('openLink',42);console.log('errors='+e.join()+' send='+typeof TelegramWidget.sendMessage);</script>")
        h.settle()
        XCTAssertTrue(h.logs.contains("errors=openLink:TypeError,openLink:TypeError,openTelegramLink:TypeError,openLink:TypeError send=undefined"), "\(h.logs)")
        XCTAssertEqual(h.actions, [])
        XCTAssertEqual(h.count("bridge-rejected"), 0, "the shim sends only valid calls")
    }

    /// `window.open` and `<a href>` to a Telegram host go to openTelegramLink.
    func testWindowOpenToTelegramHost() {
        let h = Harness("<button id='b'>o</button><script>var w='?';document.getElementById('b').onclick=function(){w=window.open('https://telegram.me/x');console.log('w='+w);};</script>")
        h.click(5)
        h.pump { !h.privileged.isEmpty }
        XCTAssertEqual(h.actions, [.openTelegramLink(URL(string: "https://telegram.me/x")!)])
        XCTAssertTrue(h.logs.contains("w=null"), "\(h.logs)")
    }

    // MARK: haptics

    func testSevenImpactsInOneTaskGiveFiveAndOneLimit() {
        let h = Harness("<script>for(var i=0;i<7;i++)TelegramWidget.HapticFeedback.impactOccurred('medium');</script>")
        h.pump { h.actions.count >= 5 }
        h.settle()
        XCTAssertEqual(h.actions, Array(repeating: .haptic(.impact("medium")), count: 5), "no tap needed")
        XCTAssertEqual(h.count("haptic-limit"), 1)
    }

    func testHapticKindsAndAHiddenWidget() {
        let h = Harness("<button id='b'>x</button><script>TelegramWidget.HapticFeedback.notificationOccurred('success').selectionChanged();document.getElementById('b').onclick=function(){TelegramWidget.HapticFeedback.impactOccurred('soft');};</script>")
        h.pump { h.actions.count >= 2 }
        XCTAssertEqual(h.actions, [.haptic(.notification("success")), .haptic(.selection)])
        h.host.setVisible(false)
        h.settle(0.1)
        h.click(5)
        h.settle()
        XCTAssertEqual(h.actions.count, 2, "a hidden widget plays nothing")
        XCTAssertEqual(h.count("haptic-limit"), 1)
    }

    // MARK: storage

    func testPreloadIsReadableFromTheFirstLineAndEachTaskCommits() {
        var options = WidgetHost.Options()
        options.storage = WidgetStorage(local: ["count": "41"], session: ["s": "kept"])
        let h = Harness("<button id='b'>b</button><script>console.log('seen='+localStorage.getItem('count')+'/'+sessionStorage.getItem('s')+'/'+localStorage.length);localStorage.setItem('count',String(Number(localStorage.getItem('count'))+1));localStorage.extra=7;document.getElementById('b').onclick=function(){sessionStorage.setItem('s','tapped');localStorage.setItem('count',localStorage.count);localStorage.setItem('w','ж'.repeat(8185));};</script>", options: options)
        h.pump { !h.commits.isEmpty }
        XCTAssertTrue(h.logs.contains("seen=41/kept/1"), "\(h.logs)")
        XCTAssertEqual(h.commits.map(\.area), [.local])
        XCTAssertEqual(h.commits.first?.values, ["count": "42", "extra": "7"])
        h.click(5)
        h.pump { h.commits.count >= 3 }
        h.settle(0.1)
        XCTAssertEqual(h.commits.map(\.area), [.local, .local, .session], "one commit per task, local first; an unchanged value is no change")
        XCTAssertEqual(h.commits[1].values["w"]?.count, 8185, "count=42, extra=7 and w = 13 + 1 + 16370 bytes: exactly the quota, by the shim's count and the glue's")
        XCTAssertEqual(h.commits[2].values, ["s": "tapped"])
        XCTAssertEqual(h.count("storage-rejected"), 0)
    }

    func testPreloadOverTheQuotaIsDroppedWhole() {
        var options = WidgetHost.Options()
        options.storage = WidgetStorage(local: ["a": String(repeating: "x", count: 20000)], session: ["b": "1"])
        let h = Harness("<script>console.log('n='+(localStorage.length+sessionStorage.length));</script>", options: options)
        h.settle(0.1)
        XCTAssertEqual(h.count("storage-rejected"), 1)
        XCTAssertTrue(h.logs.contains("n=0"), "\(h.logs)")
    }

    func testSetItemPastTheQuotaThrowsAndCommitsNothing() {
        let h = Harness("<script>var out=[];localStorage.setItem('k','x'.repeat(16383));try{localStorage.setItem('k2','y');}catch(e){out.push(e.name);}console.log('out='+out.join());</script>")
        h.pump { !h.commits.isEmpty }
        XCTAssertTrue(h.logs.contains("out=QuotaExceededError"), "\(h.logs)")
        XCTAssertEqual(h.commits.count, 1)
        XCTAssertEqual(h.commits.first?.values.keys.sorted(), ["k"])
    }

    // MARK: theme, locale, gestures

    func testThemeParamsReachTheWidgetAndTheRenderer() throws {
        let theme = WidgetTheme(colorScheme: .dark, params: ["bg_color": "#ABCDEF", "text_color": "red", "made_up": "#123456"], button: WidgetButtonTokens(radius: "10px", pressedOpacity: 0.7), text: WidgetTextTokens(fontSize: 17, padding: "6px 10px"))
        let h = Harness("<script>var p=TelegramWidget.themeParams;console.log('p='+TelegramWidget.colorScheme+','+p.bg_color+','+p.text_color+','+('made_up' in p));TelegramWidget.onEvent('themeChanged',function(){var q=TelegramWidget.themeParams;console.log('changed='+TelegramWidget.colorScheme+','+q.bg_color+','+q.link_color);});</script>", theme: theme)
        h.pump { h.logs.contains { $0.hasPrefix("p=") } }
        XCTAssertTrue(h.logs.contains("p=dark,#abcdef,#ffffff,false"), "\(h.logs)")
        let themes = h.renderer.messages.compactMap { Self.object($0) }.filter { $0["type"] as? String == "theme" }
        XCTAssertEqual(themes.count, 1)
        XCTAssertEqual(Self.object(h.renderer.messages[0])?["type"] as? String, "theme", "the variables come before any op")
        let first = try XCTUnwrap(themes.first?["theme"] as? [String: Any])
        XCTAssertEqual(first["colorScheme"] as? String, "dark")
        XCTAssertEqual(first["params"] as? [String: String], ["bg_color": "#abcdef"])
        XCTAssertEqual(first["button"] as? NSDictionary, ["radius": "10px", "pressedOpacity": 0.7] as NSDictionary)
        XCTAssertEqual(first["text"] as? NSDictionary, ["fontSize": 17, "padding": "6px 10px"] as NSDictionary)

        h.host.setTheme(WidgetTheme(colorScheme: .light, params: ["bg_color": "#101010", "link_color": "#00FF00"]))
        h.pump { h.logs.contains { $0.hasPrefix("changed=") } }
        XCTAssertTrue(h.logs.contains("changed=light,#101010,#00ff00"), "\(h.logs)")
        let last = try XCTUnwrap(h.renderer.messages.compactMap { Self.object($0) }.last { $0["type"] as? String == "theme" }?["theme"] as? [String: Any])
        XCTAssertEqual(last["colorScheme"] as? String, "light")
        XCTAssertEqual(last["params"] as? [String: String], ["bg_color": "#101010", "link_color": "#00ff00"])
        XCTAssertNil(last["button"])
        XCTAssertNil(last["text"])
    }

    func testLocale() {
        var options = WidgetHost.Options()
        options.locale = "ru"
        let h = Harness("<script>console.log('l='+navigator.language+'|'+navigator.languages.join()+'|'+(1234.5).toLocaleString()+'|'+new Intl.NumberFormat('en-US').format(1234.5));</script>", options: options)
        h.pump { h.logs.contains { $0.hasPrefix("l=") } }
        let line = h.logs.first { $0.hasPrefix("l=") } ?? ""
        XCTAssertTrue(line.hasPrefix("l=ru|ru|1"), line)
        XCTAssertTrue(line.hasSuffix(",5|1,234.5"), line)
        options.locale = "ru_RU"
        let malformed = Harness("<script>console.log('l='+navigator.language);</script>", options: options)
        malformed.pump { malformed.logs.contains { $0.hasPrefix("l=") } }
        XCTAssertTrue(malformed.logs.contains("l=en-US"), "\(malformed.logs)")
    }

    func testGestureClaimsReachTheClient() {
        let h = Harness("<p>x</p>")
        h.renderer.onMessage?("{\"type\":\"gesture\",\"wants\":true,\"x\":false,\"y\":true}")
        h.renderer.onMessage?("{\"type\":\"gesture\",\"wants\":true,\"x\":true,\"y\":true}")
        h.renderer.onMessage?("{\"type\":\"gesture\",\"wants\":1,\"x\":1,\"y\":\"yes\"}")
        h.pump { h.claims.count >= 3 }
        XCTAssertEqual(h.claims, [WidgetGestureClaim(x: false, y: true), WidgetGestureClaim(x: true, y: true), WidgetGestureClaim(x: false, y: false)])
    }

    // MARK: the API before v1

    @available(*, deprecated)
    func testStringThemeAndOnBridgeStillWork() {
        let renderer = RecordingRenderer()
        let host = try! WidgetHost(html: "<script>console.log('scheme='+TelegramWidget.colorScheme);TelegramWidget.onTheme(function(t){console.log('onTheme='+t);});TelegramWidget.ready();</script>", theme: "dark", width: 300, rendererFactory: { _, _, _ in renderer })
        defer { host.destroy() }
        var raw: [(String, String)] = []
        var actions: [WidgetAction] = []
        host.onBridge = { raw.append(($0, $1)) }
        host.onAction = { actions.append($0) }
        pump { !raw.isEmpty }
        host.setTheme("light")
        pump { host.diagnostics.logs.contains { $0.text == "onTheme=light" } }
        let logs = host.diagnostics.logs.map(\.text)
        XCTAssertEqual(logs.filter { $0.hasPrefix("scheme=") || $0.hasPrefix("onTheme=") }, ["scheme=dark", "onTheme=dark", "onTheme=light"])
        XCTAssertEqual(actions, [.ready])
        XCTAssertEqual(raw.map(\.0), ["ready"])
        XCTAssertEqual(raw.first?.1, "[]")
        XCTAssertEqual(host.diagnostics.actions, [.ready])
    }

    // MARK: the WebView renderer

    /// The renderer core sets the variables inline and important on the widget's root: the widget's own `:root` rule cannot replace them.
    /// The UA layer draws the root on them: the bubble's colour, the text tokens' size, the body's padding.
    @MainActor
    func testThemeVariablesOnTheRendererRoot() async throws {
        let window = TestWindow.makeVisible()
        defer { window.close() }
        let theme = WidgetTheme(colorScheme: .dark, params: ["bg_color": "#123456"], button: WidgetButtonTokens(radius: "10px", font: "15px Helvetica"), text: WidgetTextTokens(fontSize: 17, lineHeight: 9))
        let host = try WidgetHost(html: "<style>:root{--tg-theme-bg-color:#ff0000}</style><p>x</p>", theme: theme, width: 300)
        defer { host.destroy() }
        var latest = WidgetDiagnostics()
        host.onChange = { latest = $0 }
        window.attach(host.view)
        let deadline = Date().addingTimeInterval(15)
        while Date() < deadline, !latest.rendered { try await Task.sleep(nanoseconds: 50_000_000) }
        XCTAssertTrue(latest.rendered, "\(latest.diags)")
        let probe = "(function(){var s=getComputedStyle(document.documentElement);return ['--tg-theme-bg-color','--tg-color-scheme','--tg-button-radius','--tg-button-font','--tg-theme-text-color','--tg-text-font-size','--tg-text-line-height','--tg-text-padding'].map(function(n){return s.getPropertyValue(n).trim();}).concat([s.fontSize,s.backgroundColor,getComputedStyle(document.body).paddingLeft]).join('|');})()"
        let first = await withCheckedContinuation { c in host.evaluateInRenderer(probe) { c.resume(returning: $0) } }
        XCTAssertEqual(first, "#123456|dark|10px||#ffffff|17px|1.35|8px 12px|17px|rgb(18, 52, 86)|12px", "a font that is not a generic family and a line height over 3 are dropped by the renderer")
        host.setTheme(WidgetTheme(colorScheme: .light, params: ["bg_color": "#ABCDEF"]))
        var second = ""
        let changed = Date().addingTimeInterval(5)
        while Date() < changed, second != "#abcdef|light|||#000000|15px|1.35|8px 12px|15px|rgb(171, 205, 239)|12px" {
            try await Task.sleep(nanoseconds: 50_000_000)
            second = await withCheckedContinuation { c in host.evaluateInRenderer(probe) { c.resume(returning: $0) } }
        }
        XCTAssertEqual(second, "#abcdef|light|||#000000|15px|1.35|8px 12px|15px|rgb(171, 205, 239)|12px")
    }

    // MARK: helpers

    private static func object(_ json: String) -> [String: Any]? {
        (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any]
    }

    private func pump(_ timeout: TimeInterval = 5, _ condition: () -> Bool) {
        let deadline = Date().addingTimeInterval(timeout)
        while !condition() && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.01)) }
    }
}
