import XCTest
@testable import WidgetSandbox

/// SdkPolicy.swift against the fixture shared with sdk-policy.ts, SdkPolicy.kt and sdk_policy.cpp
/// (tests/fixtures/sdk-policy.json, copied here by `pnpm build:ios`): every section.
final class SdkPolicyTests: XCTestCase {
    private var fixture: [String: Any] = [:]

    override func setUpWithError() throws {
        let url = try XCTUnwrap(Bundle.module.url(forResource: "sdk-policy", withExtension: "json", subdirectory: "Fixtures"))
        fixture = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any])
    }

    private func cases(_ section: String) throws -> [[String: Any]] {
        let list = try XCTUnwrap(fixture[section] as? [[String: Any]], section)
        XCTAssertFalse(list.isEmpty, section)
        return list
    }

    /// JSON null is no value.
    private func value(_ v: Any?) -> Any? { v is NSNull ? nil : v }

    /// `{ "repeat": [s, n] }` stands for s repeated n times.
    private func expand(_ v: Any?) -> Any? {
        if let a = v as? [Any] { return a.map { expand($0) ?? NSNull() } }
        if let o = v as? [String: Any] {
            if o.count == 1, let r = o["repeat"] as? [Any], r.count == 2, let s = r[0] as? String, let n = (r[1] as? NSNumber)?.intValue {
                return String(repeating: s, count: n)
            }
            return o.mapValues { expand($0) ?? NSNull() }
        }
        return value(v)
    }

    private func short(_ v: Any?) -> String { String(String(describing: v ?? "nil").prefix(80)) }

    func testLinks() throws {
        for c in try cases("links") {
            let method = try XCTUnwrap(c["method"] as? String)
            XCTAssertEqual(SdkPolicy.checkLink(method, value(c["url"])), value(c["expect"]) as? String, "\(method) \(short(c["url"]))")
        }
    }

    private func hapticDict(_ h: SdkPolicy.HapticCall?) -> [String: String]? {
        guard let h else { return nil }
        var d = ["haptic": h.haptic]
        if let v = h.value { d["value"] = v }
        return d
    }

    func testHaptics() throws {
        for c in try cases("haptics") {
            XCTAssertEqual(hapticDict(SdkPolicy.checkHaptic(c["args"])), value(c["expect"]) as? [String: String], short(c["args"]))
        }
    }

    private func actionDict(_ a: SdkPolicy.Action?) -> [String: String]? {
        switch a {
        case nil: return nil
        case .ready: return ["kind": "ready"]
        case .openLink(let url): return ["kind": "openLink", "url": url]
        case .openTelegramLink(let url): return ["kind": "openTelegramLink", "url": url]
        case .haptic(let h): return hapticDict(h)!.merging(["kind": "haptic"]) { a, _ in a }
        case .setResizeAnchor(let edge): return ["kind": "setResizeAnchor", "edge": edge.rawValue]
        }
    }

    func testBridgeCalls() throws {
        var anchors = 0
        for c in try cases("bridge") {
            let checked = SdkPolicy.checkBridgeCall(value(c["method"]), value(c["args"]))
            XCTAssertEqual(actionDict(checked), value(c["expect"]) as? [String: String], "\(short(c["method"])) \(short(c["args"]))")
            if case .setResizeAnchor = checked {
                anchors += 1
                XCTAssertNil(WidgetAction(checked!), "the anchor is never a WidgetAction")
            }
        }
        XCTAssertEqual(anchors, 2, "the fixture's setResizeAnchor entries for top and bottom")
    }

    /// The single-argument check behind `setResizeAnchor` (the fixture covers it through `bridge`).
    func testResizeAnchor() {
        XCTAssertEqual(SdkPolicy.defaultResizeAnchor, .top)
        XCTAssertEqual(SdkPolicy.checkResizeAnchor("top"), .top)
        XCTAssertEqual(SdkPolicy.checkResizeAnchor("bottom"), .bottom)
        for bad: Any? in [nil, NSNull(), "Top", "bottom ", "", "left", 1, true, ["top"], ["edge": "top"]] {
            XCTAssertNil(SdkPolicy.checkResizeAnchor(bad), short(bad))
        }
    }

    func testHapticGate() throws {
        for c in try cases("hapticGate") {
            let name = c["name"] as? String ?? "?"
            var gate = SdkPolicy.HapticGate()
            for (i, step) in try XCTUnwrap(c["steps"] as? [[String: Any]]).enumerated() {
                let at = try XCTUnwrap((step["at"] as? NSNumber)?.doubleValue)
                let visible = try XCTUnwrap(step["visible"] as? Bool)
                XCTAssertEqual(gate.decide(nowMs: at, visible: visible).rawValue, step["expect"] as? String, "\(name), step \(i)")
            }
        }
    }

    func testThemeParams() throws {
        for c in try cases("themeParams") {
            XCTAssertEqual(SdkPolicy.checkThemeParams(value(c["input"])), try XCTUnwrap(c["expect"] as? [String: String]), short(c["input"]))
        }
    }

    func testLocales() throws {
        for c in try cases("locales") {
            XCTAssertEqual(SdkPolicy.checkLocale(value(c["input"])), value(c["expect"]) as? String, short(c["input"]))
        }
    }

    private func commitDict(_ c: SdkPolicy.StorageCommit?) -> [String: [String: String]]? {
        guard let c else { return nil }
        var d: [String: [String: String]] = [:]
        if let local = c.local { d["local"] = local }
        if let session = c.session { d["session"] = session }
        return d
    }

    func testStorage() throws {
        for c in try cases("storage") {
            let note = c["note"] as? String ?? short(c["input"])
            let expected = expand(c["expect"])
            let got = commitDict(SdkPolicy.checkStorageCommit(expand(c["input"])))
            if expected == nil {
                XCTAssertNil(got, note)
            } else {
                XCTAssertEqual(got, try XCTUnwrap(expected as? [String: [String: String]], note), note)
            }
        }
    }

    func testUtf8() throws {
        for c in try cases("utf8") {
            let s = try XCTUnwrap(c["s"] as? String)
            XCTAssertEqual(SdkPolicy.utf8Length(s), (c["bytes"] as? NSNumber)?.intValue, s)
        }
    }

    /// The constants the fixture does not spell out, as sdk-policy.ts has them.
    func testConstants() {
        XCTAssertEqual(SdkPolicy.themeKeys.count, 15)
        XCTAssertEqual(Set(SdkPolicy.themeKeys).count, 15)
        XCTAssertEqual(SdkPolicy.privilegedMethods, ["openLink", "openTelegramLink"])
        XCTAssertEqual(SdkPolicy.maxLinkLength, 256)
        XCTAssertEqual(SdkPolicy.storageQuotaBytes, 16384)
        XCTAssertEqual(SdkPolicy.maxLocaleLength, 35)
        XCTAssertEqual(SdkPolicy.defaultLocale, "en-US")
        XCTAssertEqual(SdkPolicy.profileVersion, 1)
        XCTAssertEqual(SandboxEngine.maxStorageCommitChars, 2 * 12 * 16384 + 64)
    }
}
