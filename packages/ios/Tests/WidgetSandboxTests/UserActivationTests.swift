import XCTest
@testable import WidgetSandbox

final class UserActivationTests: XCTestCase {
    private final class Harness {
        let renderer = RecordingRenderer()
        let host: WidgetHost
        var bridge: [(String, String)] = []

        init(_ html: String, timeout: TimeInterval = 5) {
            let renderer = self.renderer
            host = try! WidgetHost(html: html, theme: "light", width: 300, rendererFactory: { _, _, _ in renderer })
            host.onBridge = { [weak self] method, args in self?.bridge.append((method, args)) }
            pump(timeout) { self.host.diagnostics.rendered || self.host.diagnostics.dead != nil }
        }

        var privileged: [(String, String)] { bridge.filter { $0.0 != "ready" } }
        var blocked: Int { host.diagnostics.diags["activation-blocked"]?.count ?? 0 }

        func event(_ ev: [String: Any]) {
            let data = try! JSONSerialization.data(withJSONObject: ["type": "event", "ev": ev])
            renderer.onMessage?(String(decoding: data, as: UTF8.self))
        }

        func click(_ target: Int) {
            event(["type": "click", "targetId": target, "clientX": 5, "clientY": 5, "button": 0, "buttons": 0, "detail": 1])
        }

        func settle(_ seconds: TimeInterval = 0.3) {
            pump(seconds) { false }
        }

        func pump(_ timeout: TimeInterval = 5, _ condition: () -> Bool) {
            let deadline = Date().addingTimeInterval(timeout)
            while !condition() && Date() < deadline {
                RunLoop.main.run(until: Date().addingTimeInterval(0.01))
            }
        }

        deinit { host.destroy() }
    }

    private static let anchor = 5

    func testScriptedClicksAtBootAreIgnored() {
        let h = Harness("<a id='a' href='https://example.com/x'>link</a><script>var a=document.getElementById('a');a.click();a.dispatchEvent(new MouseEvent('click',{bubbles:true,cancelable:true}));TelegramWidget.openLink('https://example.com/y');TelegramWidget.sendMessage('hi');window.open('https://example.com/z');TelegramWidget.ready();</script>")
        h.settle()
        XCTAssertEqual(h.privileged.map { $0.0 }, [])
        XCTAssertGreaterThanOrEqual(h.blocked, 4)
        XCTAssertEqual(h.bridge.map { $0.0 }, ["ready"])
    }

    func testRealClickOnAnchorOpensOnce() {
        let h = Harness("<a id='a' href='https://example.com/x'>link</a>")
        h.click(Self.anchor)
        h.pump { !h.privileged.isEmpty }
        h.settle()
        XCTAssertEqual(h.privileged.map { $0.0 }, ["openLink"])
        XCTAssertEqual(h.privileged.first?.1, "[\"https:\\/\\/example.com\\/x\"]")
    }

    func testChatSendAnchorStillWorks() {
        let h = Harness("<div class='widget'><a id='a' href='chat:send?text=/help'>help</a></div>")
        h.click(6)
        h.pump { !h.privileged.isEmpty }
        XCTAssertEqual(h.privileged.map { $0.1 }, ["[\"chat:send?text=\\/help\"]"])
    }

    func testActivationIsConsumedOnce() {
        let h = Harness("<a id='a' href='#'>x</a><script>document.getElementById('a').addEventListener('click',function(){TelegramWidget.openLink('https://a.example');TelegramWidget.openLink('https://b.example');TelegramWidget.sendMessage('c');});</script>")
        h.click(Self.anchor)
        h.pump { !h.privileged.isEmpty }
        h.settle()
        XCTAssertEqual(h.privileged.count, 1)
        XCTAssertEqual(h.blocked, 2)
    }

    func testTimersAndPromisesAfterClick() {
        let h = Harness("<a id='a' href='#'>x</a><script>document.getElementById('a').addEventListener('click',function(){Promise.resolve().then(function(){TelegramWidget.openLink('https://micro.example');});setTimeout(function(){TelegramWidget.openLink('https://timer.example');},0);});</script>")
        h.click(Self.anchor)
        h.settle(0.5)
        XCTAssertEqual(h.privileged.map { $0.1 }, ["[\"https:\\/\\/micro.example\"]"])
        XCTAssertEqual(h.blocked, 1)
    }

    func testNonGestureEventsDoNotActivate() {
        let h = Harness("<a id='a' href='https://example.com/x'>x</a><script>var a=document.getElementById('a');['mousemove','mouseover','animationend','transitionend','scroll','focus','input','change','submit','load','wheel','keyup'].forEach(function(t){a.addEventListener(t,function(){a.click();TelegramWidget.openLink('https://'+t+'.example');});});</script>")
        for type in ["mousemove", "mouseover", "animationend", "transitionend", "scroll", "focus", "input", "change", "submit", "load", "wheel", "keyup"] {
            h.event(["type": type, "targetId": Self.anchor])
        }
        h.event(["type": "keydown", "targetId": 4, "key": "Escape", "code": "Escape", "repeat": false, "modifiers": 0])
        h.event(["type": "keydown", "targetId": 4, "key": "a", "code": "KeyA", "repeat": true, "modifiers": 0])
        h.event(["type": "keydown", "targetId": 4, "key": "w", "code": "KeyW", "repeat": false, "modifiers": 4])
        h.settle(0.5)
        XCTAssertEqual(h.privileged.map { $0.1 }, [])
        XCTAssertGreaterThanOrEqual(h.blocked, 12)
    }

    func testOnePhysicalPressIsOneGesture() {
        let h = Harness("<a id='a' href='#'>x</a><script>['pointerdown','mousedown','pointerup','mouseup','click','dblclick'].forEach(function(t){document.addEventListener(t,function(){TelegramWidget.openLink('https://'+t+'.example');});});</script>")
        func press(_ detail: Int) {
            for type in ["pointerdown", "mousedown", "pointerup", "mouseup", "click"] + (detail == 2 ? ["dblclick"] : []) {
                h.event(["type": type, "targetId": Self.anchor, "clientX": 5, "clientY": 5, "detail": detail])
            }
        }
        press(1)
        h.settle()
        XCTAssertEqual(h.privileged.map { $0.1 }, ["[\"https:\\/\\/pointerdown.example\"]"])
        press(2)
        h.settle()
        XCTAssertEqual(h.privileged.count, 2)
        XCTAssertEqual(h.blocked, 9)
    }

    func testDropOnPointerUpStillWorks() {
        let h = Harness("<div id='d'>card</div><script>var drag=false;document.getElementById('d').addEventListener('pointerdown',function(){drag=true;});document.addEventListener('pointerup',function(){if(drag){drag=false;TelegramWidget.sendMessage('Done: card');}});</script>")
        h.event(["type": "pointerdown", "targetId": 5])
        h.event(["type": "pointermove", "targetId": 4])
        h.event(["type": "pointerup", "targetId": 4])
        h.pump { !h.privileged.isEmpty }
        XCTAssertEqual(h.privileged.map { $0.0 }, ["sendMessage"])
    }

    func testKeyPressActivates() {
        let h = Harness("<div>x</div><script>document.addEventListener('keydown',function(e){if(e.key==='Enter')TelegramWidget.sendMessage('go');});</script>")
        h.event(["type": "keydown", "targetId": 4, "key": "Enter", "code": "Enter", "repeat": false, "modifiers": 0])
        h.pump { !h.privileged.isEmpty }
        XCTAssertEqual(h.privileged.map { $0.0 }, ["sendMessage"])
    }

    func testHiddenWidgetCannotUseActivation() {
        let h = Harness("<a id='a' href='https://example.com/x'>x</a>")
        h.host.setVisible(false)
        h.settle(0.1)
        h.click(Self.anchor)
        h.settle()
        XCTAssertEqual(h.privileged.count, 0)
        let window = NSWindow(contentRect: CGRect(x: 0, y: 0, width: 300, height: 100), styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.orderFrontRegardless()
        window.contentView?.addSubview(h.host.view)
        defer { window.orderOut(nil) }
        h.host.setVisible(true)
        h.settle(0.1)
        h.click(Self.anchor)
        h.pump { !h.privileged.isEmpty }
        XCTAssertEqual(h.privileged.count, 1)
    }

    private static let attacks = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../../corpus/attacks").standardized

    func testTimerDrivenAttackStaysBlocked() throws {
        try XCTSkipIf(!FileManager.default.fileExists(atPath: Self.attacks.path), "needs the repository's corpus/attacks")
        let h = Harness(try String(contentsOf: Self.attacks.appendingPathComponent("activation-timers.html")))
        h.settle(1.6)
        XCTAssertEqual(h.privileged.map { $0.0 }, [])
        XCTAssertGreaterThanOrEqual(h.blocked, 5)
    }

    func testSyntheticInputAttackStaysBlocked() throws {
        try XCTSkipIf(!FileManager.default.fileExists(atPath: Self.attacks.path), "needs the repository's corpus/attacks")
        let h = Harness(try String(contentsOf: Self.attacks.appendingPathComponent("activation-synthetic-input.html")))
        h.settle(0.3)
        XCTAssertEqual(h.privileged.map { $0.1 }, [])
    }

    func testFloodAfterOneClickYieldsOneCall() throws {
        try XCTSkipIf(!FileManager.default.fileExists(atPath: Self.attacks.path), "needs the repository's corpus/attacks")
        let h = Harness(try String(contentsOf: Self.attacks.appendingPathComponent("activation-flood.html")))
        h.click(6)
        h.pump { !h.privileged.isEmpty }
        h.settle(0.3)
        XCTAssertEqual(h.privileged.count, 1)
    }

    func testBundledAttackWidgetsReachNoPrivilegedBridge() throws {
        try XCTSkipIf(!FileManager.default.fileExists(atPath: Self.attacks.path), "needs the repository's corpus/attacks")
        let directory = Self.attacks
        let files = try FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil).filter { $0.pathExtension == "html" }
        XCTAssertGreaterThan(files.count, 50)
        for file in files {
            let h = Harness(try String(contentsOf: file), timeout: 1)
            h.settle(0.05)
            XCTAssertEqual(h.privileged.map { $0.0 }, [], file.lastPathComponent)
        }
    }
}

final class RecordingRenderer: Renderer {
    let view = PlatformView(frame: CGRect(x: 0, y: 0, width: 300, height: 100))
    var onMessage: ((String) -> Void)?
    var onDiag: ((String, String) -> Void)?
    var messages: [String] = []

    func load() {
        onMessage?("{\"type\":\"ready\"}")
    }

    func send(_ json: String) {
        messages.append(json)
        guard let data = json.data(using: .utf8), let message = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any], let type = message["type"] as? String else {
            return
        }
        if type == "ops" {
            onMessage?("{\"type\":\"size\",\"width\":300,\"height\":120}")
        } else if type == "query", let qid = message["qid"] as? Int {
            DispatchQueue.main.async { [weak self] in
                self?.onMessage?("{\"type\":\"result\",\"qid\":\(qid),\"result\":null}")
            }
        }
    }

    func setTheme(_ theme: String) {}
    func evaluate(_ script: String, completion: @escaping (String?) -> Void) { completion(nil) }
    func destroy() {}
}
