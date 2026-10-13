import XCTest
@testable import WidgetSandbox

final class WidgetBubbleTests: XCTestCase {
    private func wait(_ condition: () -> Bool, timeout: TimeInterval = 5) {
        let deadline = Date().addingTimeInterval(timeout)
        while !condition() && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
    }

    func testHeightIsRememberedAcrossDropAndRecreatedHostCannotShrinkItBeforeRendering() {
        let feed = WidgetFeed()
        var queued: [() -> Void] = []
        feed.schedule = { queued.append($0) }
        feed.delay = { _, block in queued.append(block) }
        var generation = 0
        let bubble = WidgetBubble(html: "<div style='height:120px'>Hello</div>", theme: "light", width: 300, feed: feed, rendererFactory: { _, _, _ in
            generation += 1
            return ScriptedSizeRenderer(early: generation == 1 ? 120 : 30, settled: 120)
        })
        var heights: [CGFloat] = []
        bubble.onHeightChange = { heights.append(bubble.height) }

        XCTAssertNotNil(bubble.ensureHost())
        wait { bubble.hasContent && !bubble.isAwaitingRender && bubble.host?.diagnostics.rendered == true }
        XCTAssertEqual(bubble.height, 120)

        let space = OneSlotSpace(bubble.slot, rect: CGRect(x: 0, y: 0, width: 300, height: 120))
        feed.attach(space)
        space.origin = 100_000
        feed.update()
        XCTAssertNil(bubble.host)
        XCTAssertFalse(bubble.slot.isLive)
        XCTAssertEqual(bubble.height, 120)

        space.origin = 0
        feed.update()
        XCTAssertNotNil(bubble.host)
        XCTAssertTrue(bubble.slot.isVisible)
        XCTAssertTrue(bubble.isAwaitingRender)
        wait { bubble.host?.diagnostics.rendered == true && bubble.hostHasContent && !bubble.isAwaitingRender }
        XCTAssertEqual(bubble.height, 120)
        XCTAssertFalse(heights.contains(30), "\(heights)")
        withExtendedLifetime(queued) {}
    }

    func testVisibilityFollowsTheFeedAndReachesTheHost() {
        let feed = WidgetFeed()
        feed.schedule = { _ in }
        let bubble = WidgetBubble(html: "<div>Hi</div>", theme: "light", width: 300, feed: feed, rendererFactory: { _, _, _ in ScriptedSizeRenderer(early: 40, settled: 40) })
        var changes: [Bool] = []
        bubble.onVisibilityChange = { changes.append($0) }
        let space = OneSlotSpace(bubble.slot, rect: CGRect(x: 0, y: 0, width: 300, height: 40))
        feed.attach(space)
        feed.update()
        XCTAssertTrue(bubble.isVisible)
        space.origin = 500
        feed.update()
        XCTAssertFalse(bubble.isVisible)
        XCTAssertNotNil(bubble.host)
        XCTAssertEqual(changes, [true, false])
    }
}

private final class OneSlotSpace: WidgetFeedSpace {
    let slot: WidgetFeedSlot
    let rect: CGRect
    var origin: CGFloat = 0

    init(_ slot: WidgetFeedSlot, rect: CGRect) {
        self.slot = slot
        self.rect = rect
        slot.space = self
        slot.locate = { rect }
    }

    var feedViewport: CGRect { CGRect(x: 0, y: origin, width: 300, height: 400) }
    var feedOnScreen: Bool { true }
    func feedSlots(in rect: CGRect) -> [WidgetFeedSlot] { rect.intersects(self.rect) ? [slot] : [] }
}

private final class ScriptedSizeRenderer: Renderer {
    let view = PlatformView(frame: CGRect(x: 0, y: 0, width: 300, height: 100))
    var onMessage: ((String) -> Void)?
    var onDiag: ((String, String) -> Void)?
    let early: Int
    let settled: Int

    init(early: Int, settled: Int) {
        self.early = early
        self.settled = settled
    }

    func load() {
        onMessage?("{\"type\":\"ready\"}")
    }

    func send(_ json: String) {
        guard let data = json.data(using: .utf8), let message = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any], let type = message["type"] as? String else {
            return
        }
        if type == "ops" {
            onMessage?("{\"type\":\"size\",\"width\":300,\"height\":\(early)}")
        } else if type == "query", let qid = message["qid"] as? Int {
            DispatchQueue.main.asyncAfter(deadline: .now() + (qid < 0 ? 0.1 : 0)) { [weak self] in
                guard let self = self else {
                    return
                }
                if qid < 0 {
                    self.onMessage?("{\"type\":\"size\",\"width\":300,\"height\":\(self.settled)}")
                }
                self.onMessage?("{\"type\":\"result\",\"qid\":\(qid),\"result\":null}")
            }
        }
    }

    func setTheme(_ theme: String) {}
    func evaluate(_ script: String, completion: @escaping (String?) -> Void) { completion(nil) }
    func destroy() {}
}
