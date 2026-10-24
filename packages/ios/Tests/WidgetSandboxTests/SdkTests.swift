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

    // MARK: resize anchor (v0.1.3)

    /// `setResizeAnchor` is state next to the height, not an action: no tap, any visibility, nothing in `actions` or
    /// `bridge`, only the latest value kept (sdk.test.ts "SDK v1: setResizeAnchor").
    func testSetResizeAnchorIsStateNotAnAction() {
        var options = WidgetHost.Options()
        options.visible = false
        let h = Harness("<button id='b'>b</button><script>var errs=[];['left','Bottom',' top','',null,1].forEach(function(v){try{TelegramWidget.setResizeAnchor(v);}catch(e){errs.push(e.name);}});" +
            "console.log('errs='+errs.join());TelegramWidget.setResizeAnchor('top');for(var i=0;i<1000;i++)TelegramWidget.setResizeAnchor('bottom');document.body.appendChild(document.createElement('p')).textContent='grown';" +
            "document.getElementById('b').onclick=function(){TelegramWidget.setResizeAnchor('top');console.log('tapped');};</script>", options: options)
        h.pump { h.host.diagnostics.resizeAnchor == .bottom }
        h.settle(0.1)
        XCTAssertEqual(h.host.diagnostics.resizeAnchor, .bottom, "a hidden widget sets it at boot, no tap")
        XCTAssertTrue(h.logs.contains("errs=" + Array(repeating: "TypeError", count: 6).joined(separator: ",")), "\(h.logs)")
        XCTAssertEqual(h.actions, [])
        XCTAssertTrue(h.host.diagnostics.bridge.isEmpty)
        XCTAssertEqual(h.count("bridge-rejected"), 0)
        XCTAssertEqual(h.count("activation-blocked"), 0)
        h.host.setVisible(true)
        h.click(5)
        h.pump { h.logs.contains("tapped") && h.host.diagnostics.resizeAnchor == .top }
        XCTAssertEqual(h.host.diagnostics.resizeAnchor, .top)
        XCTAssertEqual(h.actions, [])

        let plain = Harness("<p>never anchored</p>")
        plain.settle(0.1)
        XCTAssertEqual(plain.host.diagnostics.resizeAnchor, .top)
    }

    /// The order bridge → ops → renderer → size: the anchor of a task is in the host's state before the size report
    /// of that task's ops, here a renderer that reports the height synchronously while it receives the ops. An equal
    /// value (the glue's own check, the shim sends only changes) changes nothing.
    func testResizeAnchorComesBeforeTheHeightOfItsTask() throws {
        let renderer = MarkerSizeRenderer()
        let html = "<p id='p'>h100</p><script>TelegramWidget.setResizeAnchor('bottom');document.getElementById('p').textContent='h200';" +
            "setTimeout(function(){TelegramWidget.setResizeAnchor('top');document.getElementById('p').textContent='h300';},100);</script>"
        let host = try WidgetHost(html: html, theme: WidgetTheme(), width: 300, rendererFactory: { _, _, _ in renderer })
        defer { host.destroy() }
        var states: [(height: CGFloat, anchor: WidgetResizeAnchor)] = []
        let lock = NSLock()
        host.onChangeOnExecutor = { d in lock.lock(); states.append((d.height, d.resizeAnchor)); lock.unlock() }
        pump { host.diagnostics.height == 300 }
        pump(0.2) { false }
        lock.lock()
        let seen = states
        lock.unlock()
        XCTAssertEqual(seen.first { $0.height == 200 }?.anchor, .bottom, "\(seen)")
        XCTAssertEqual(seen.first { $0.height == 300 }?.anchor, .top, "\(seen)")
        XCTAssertFalse(seen.contains { $0.height == 300 && $0.anchor == .bottom }, "the second task's anchor before its height: \(seen)")
        XCTAssertEqual(host.diagnostics.resizeAnchor, .top)

        let engine = try XCTUnwrap(Mirror(reflecting: host).children.first { $0.label == "engine" }?.value as? SandboxEngine)
        // Once the host is quiet (the last task's stats arrive after its ops), an equal anchor publishes nothing.
        let count = { () -> Int in lock.lock(); defer { lock.unlock() }; return states.count }
        var quiet = count(), quietSince = Date()
        let limit = Date().addingTimeInterval(5)
        while Date() < limit, Date().timeIntervalSince(quietSince) < 0.5 {
            pump(0.05) { false }
            if count() != quiet { quiet = count(); quietSince = Date() }
        }
        host.engine(engine, resizeAnchor: .top)
        pump(0.3) { false }
        XCTAssertEqual(count(), quiet, "an equal anchor publishes nothing")
        host.engine(engine, resizeAnchor: .bottom)
        pump { host.diagnostics.resizeAnchor == .bottom }
        XCTAssertEqual(host.diagnostics.resizeAnchor, .bottom)
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

    /// Vertical drags are the chat's (v0.1.3): the claim the client gets has `y` false whatever the renderer sends.
    func testGestureClaimsReachTheClient() {
        let h = Harness("<p>x</p>")
        h.renderer.onMessage?("{\"type\":\"gesture\",\"wants\":true,\"x\":false,\"y\":true}")
        h.renderer.onMessage?("{\"type\":\"gesture\",\"wants\":true,\"x\":true,\"y\":true}")
        h.renderer.onMessage?("{\"type\":\"gesture\",\"wants\":1,\"x\":1,\"y\":\"yes\"}")
        h.renderer.onMessage?("{\"type\":\"gesture\",\"wants\":true,\"x\":true,\"y\":true,\"hold\":true}")
        h.renderer.onMessage?("{\"type\":\"gesture\",\"wants\":false,\"x\":false,\"y\":false,\"hold\":1}")
        h.renderer.onMessage?("{\"type\":\"gesture\",\"wants\":false,\"x\":true,\"y\":false,\"hold\":true}")
        // A renderer that sends only `wants` (the Blitz and litehtml prototypes) claims nothing.
        h.renderer.onMessage?("{\"type\":\"gesture\",\"wants\":true}")
        h.pump { h.claims.count >= 7 }
        XCTAssertEqual(h.claims, [
            WidgetGestureClaim(x: false, y: false), WidgetGestureClaim(x: true, y: false), WidgetGestureClaim(x: false, y: false),
            WidgetGestureClaim(x: true, y: false, hold: true), WidgetGestureClaim(x: false, y: false),
            WidgetGestureClaim(x: true, y: false, hold: true),
            WidgetGestureClaim(x: false, y: false),
        ])
    }

    /// The claim the WebView renderer computes from `touch-action` at a pointerdown (gesture-policy.ts `claimOf`) reaches
    /// `onGestureClaim` whole: `none` on an ancestor claims horizontal drags and holds, `pan-y` horizontal drags, `auto`
    /// and `pan-x` nothing; no claim has `y` (v0.1.3). WebKit itself gets the widened value (renderer/touch-action.ts:
    /// vertical pans always allowed, so the list scrolls over the widget) and the widget's script still reads its own.
    @MainActor
    func testTouchActionClaimsFromTheWebViewRenderer() async throws {
        let html = "<div id='free'>auto</div><div id='box' style='touch-action:none'><p id='own'>none</p></div><div id='pan' style='touch-action:pan-y'>pan-y</div><div id='px' style='touch-action:pan-x'>pan-x</div>" +
            "<script>setTimeout(function(){console.log('own='+getComputedStyle(document.getElementById('box')).touchAction+'|'+getComputedStyle(document.getElementById('px')).touchAction);},50);</script>"
        let host = try WidgetHost(html: html, theme: WidgetTheme(), width: 300)
        defer { host.destroy() }
        var claims: [WidgetGestureClaim] = []
        var latest = WidgetDiagnostics()
        host.onChange = { latest = $0 }
        host.onGestureClaim = { claims.append($0) }
        let window = TestWindow.makeVisible()
        window.attach(host.view)
        var deadline = Date().addingTimeInterval(15)
        while Date() < deadline, !latest.rendered { try await Task.sleep(nanoseconds: 50_000_000) }
        XCTAssertTrue(latest.rendered, "\(latest.diags)")
        for (n, id) in ["free", "own", "pan", "px"].enumerated() {
            // A synthetic press: the renderer computes the claim at any primary pointerdown (only trusted input activates).
            let script = "var t=document.getElementById('\(id)'),o={bubbles:true,cancelable:true,isPrimary:true,pointerId:\(n + 1),pointerType:'touch',clientX:60,clientY:5};" +
                "t.dispatchEvent(new PointerEvent('pointerdown',o));t.dispatchEvent(new PointerEvent('pointerup',o));'ok'"
            let result = await withCheckedContinuation { c in host.evaluateInRenderer(script) { c.resume(returning: $0) } }
            XCTAssertEqual(result, "ok", id)
            deadline = Date().addingTimeInterval(5)
            while Date() < deadline, claims.count <= n { try await Task.sleep(nanoseconds: 20_000_000) }
        }
        XCTAssertEqual(claims, [
            WidgetGestureClaim(x: false, y: false, hold: false),
            WidgetGestureClaim(x: true, y: false, hold: true),
            WidgetGestureClaim(x: true, y: false, hold: false),
            WidgetGestureClaim(x: false, y: false, hold: false),
        ])
        let engine = await withCheckedContinuation { c in
            host.evaluateInRenderer("getComputedStyle(document.getElementById('own').parentNode).touchAction+'|'+getComputedStyle(document.getElementById('px')).touchAction") { c.resume(returning: $0) }
        }
        XCTAssertEqual(engine, "pan-y|pan-x pan-y", "WebKit always pans vertically")
        deadline = Date().addingTimeInterval(5)
        while Date() < deadline, !latest.logs.contains(where: { $0.text.hasPrefix("own=") }) { try await Task.sleep(nanoseconds: 20_000_000) }
        XCTAssertEqual(latest.logs.first { $0.text.hasPrefix("own=") }?.text, "own=none|pan-x", "the widget reads its own touch-action")
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
    /// The UA layer draws the root on them: the text tokens' size and the padding token as the body's margin; the root
    /// has no background of its own (the bubble behind the transparent renderer is the canvas).
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
        let probe = "(function(){var s=getComputedStyle(document.documentElement);return ['--tg-theme-bg-color','--tg-color-scheme','--tg-button-radius','--tg-button-font','--tg-theme-text-color','--tg-text-font-size','--tg-text-line-height','--tg-text-padding'].map(function(n){return s.getPropertyValue(n).trim();}).concat([s.fontSize,s.backgroundColor,getComputedStyle(document.body).marginLeft,getComputedStyle(document.body).paddingLeft]).join('|');})()"
        let first = await withCheckedContinuation { c in host.evaluateInRenderer(probe) { c.resume(returning: $0) } }
        XCTAssertEqual(first, "#123456|dark|10px||#ffffff|17px|1.35|8px 12px|17px|rgba(0, 0, 0, 0)|12px|0px", "a font that is not a generic family and a line height over 3 are dropped by the renderer")
        host.setTheme(WidgetTheme(colorScheme: .light, params: ["bg_color": "#ABCDEF"]))
        var second = ""
        let changed = Date().addingTimeInterval(5)
        while Date() < changed, second != "#abcdef|light|||#000000|15px|1.35|8px 12px|15px|rgba(0, 0, 0, 0)|12px|0px" {
            try await Task.sleep(nanoseconds: 50_000_000)
            second = await withCheckedContinuation { c in host.evaluateInRenderer(probe) { c.resume(returning: $0) } }
        }
        XCTAssertEqual(second, "#abcdef|light|||#000000|15px|1.35|8px 12px|15px|rgba(0, 0, 0, 0)|12px|0px")
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

/// Reports a size synchronously while it receives an ops batch: 100, 200 or 300 for the batch that carries the text
/// `h100`, `h200` or `h300` (the latest marker in the batch), so a state's height names the task whose ops made it.
final class MarkerSizeRenderer: Renderer {
    let view = PlatformView(frame: CGRect(x: 0, y: 0, width: 300, height: 100))
    var onMessage: ((String) -> Void)?
    var onDiag: ((String, String) -> Void)?

    func load() { onMessage?("{\"type\":\"ready\"}") }

    func send(_ json: String) {
        guard let message = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any], let type = message["type"] as? String else { return }
        if type == "ops" {
            if let height = [("h300", 300), ("h200", 200), ("h100", 100)].first(where: { json.contains($0.0) })?.1 {
                onMessage?("{\"type\":\"size\",\"width\":300,\"height\":\(height)}")
            }
        } else if type == "query", let qid = message["qid"] as? Int {
            DispatchQueue.main.async { [weak self] in self?.onMessage?("{\"type\":\"result\",\"qid\":\(qid),\"result\":null}") }
        }
    }

    func setTheme(_ theme: String) {}
    func evaluate(_ script: String, completion: @escaping (String?) -> Void) { completion(nil) }
    func destroy() {}
}
