import XCTest
import JavaScriptCore
@testable import WidgetSandbox

final class IntlBridgeTests: XCTestCase {
    private func request(_ options: [String: Any], timestamp: Double, locale: String = "en-GB", method: String? = nil) throws -> String {
        var request: [String: Any] = ["kind": method == nil ? "date" : "dateMethod", "locale": locale, "opts": options, "value": timestamp]
        if let method = method { request["method"] = method }
        return String(decoding: try JSONSerialization.data(withJSONObject: request, options: [.sortedKeys]), as: UTF8.self)
    }

    private func check(_ options: [String: Any], timestamp: Double, locale: String = "en-GB", method: String? = nil, file: StaticString = #filePath, line: UInt = #line) throws {
        let json = try request(options, timestamp: timestamp, locale: locale, method: method)
        let context = try XCTUnwrap(JSContext())
        let expected = context.evaluateScript("var r = \(json); r.method ? new Date(r.value)[r.method](r.locale, r.opts) : new Intl.DateTimeFormat(r.locale, r.opts).format(r.value)")?.toString()
        XCTAssertNil(context.exception, file: file, line: line)
        XCTAssertEqual(IntlBridge.format(requestJson: json).replacingOccurrences(of: "\u{202f}", with: " "), expected?.replacingOccurrences(of: "\u{202f}", with: " "), "\(json)", file: file, line: line)
    }

    func testClockTimeZonesAcrossSeasons() throws {
        for timestamp in [1736942400000.0, 1752580800000.0, 1736971200000.0] {
            for zone in ["UTC", "Europe/Lisbon", "America/New_York", "Asia/Tokyo"] {
                try check(["timeZone": zone, "hour": "2-digit", "minute": "2-digit", "second": "2-digit", "hourCycle": "h23"], timestamp: timestamp)
                try check(["timeZone": zone, "weekday": "long", "day": "numeric", "month": "short"], timestamp: timestamp)
            }
        }
    }

    func testHourCyclesAtMidnightAndNoon() throws {
        for timestamp in [1736899200000.0, 1736942400000.0] {
            for cycle in ["h11", "h12", "h23", "h24"] {
                try check(["timeZone": "UTC", "hour": "2-digit", "minute": "2-digit", "hourCycle": cycle], timestamp: timestamp)
                try check(["timeZone": "UTC", "timeStyle": "short", "hourCycle": cycle], timestamp: timestamp)
            }
        }
    }

    func testFormattingOptionsRemainIndependent() throws {
        for timestamp in [1736899200000.0, 1736931600000.0, 1736942400000.0] {
            for locale in ["en-GB", "en-US", "de-DE"] {
                try check(["timeZone": "UTC", "hour": "numeric", "minute": "2-digit", "hour12": false], timestamp: timestamp, locale: locale)
                try check(["timeZone": "Asia/Tokyo", "hour": "numeric", "minute": "2-digit", "hour12": true], timestamp: timestamp, locale: locale)
                try check(["timeZone": "UTC", "year": "2-digit", "month": "2-digit", "day": "2-digit"], timestamp: timestamp, locale: locale)
                try check(["timeZone": "UTC", "dateStyle": "full"], timestamp: timestamp, locale: locale)
                try check(["timeZone": "UTC", "timeStyle": "short", "hour12": false], timestamp: timestamp, locale: locale)
                try check(["timeZone": "UTC", "timeStyle": "short", "hour12": true], timestamp: timestamp, locale: locale)
                try check(["timeZone": "UTC", "hour": "2-digit", "minute": "2-digit"], timestamp: timestamp, locale: locale, method: "toLocaleTimeString")
            }
        }
    }

    func testRepeatedClockFormattingCost() throws {
        let requests = try (0..<1000).map { index in
            try request(["timeZone": ["Europe/Lisbon", "America/New_York", "Asia/Tokyo"][index % 3], "hour": "2-digit", "minute": "2-digit", "second": "2-digit", "hourCycle": "h23"], timestamp: 1736942400000 + Double(index * 1000))
        }
        let start = Date()
        for request in requests { XCTAssertFalse(IntlBridge.format(requestJson: request).isEmpty) }
        print("IntlBridge 1000 clock formats: \(Date().timeIntervalSince(start) * 1000) ms")
    }

    func testConcurrentClockFormatting() throws {
        let requests = try ["UTC", "Europe/Lisbon", "America/New_York", "Asia/Tokyo"].map { zone in
            try request(["timeZone": zone, "hour": "2-digit", "minute": "2-digit", "hourCycle": "h23"], timestamp: 1736942400000)
        }
        let expected = ["12:00", "12:00", "07:00", "21:00"]
        DispatchQueue.concurrentPerform(iterations: 8) { worker in
            for iteration in 0..<100 {
                let index = (worker + iteration) % requests.count
                XCTAssertEqual(IntlBridge.format(requestJson: requests[index]), expected[index])
            }
        }
    }
}
