// SDK v1 checks (docs/sdk-v1.md §2.5, §2.6, §2.8, §2.9, §3): a line-for-line
// mirror of the checked part of src/shared/sdk-policy.ts (see the comments
// there). The shim checks the same calls, but it is untrusted, so the glue
// checks them again: link URLs, haptic arguments and the 5 + 10/s gate, theme
// params, the locale, storage commits and preloads, bridge calls.
//
// Pure and side-effect free; checked against tests/fixtures/sdk-policy.json
// (copied to Tests/WidgetSandboxTests/Fixtures by `pnpm build:ios`), shared
// with sdk-policy.ts, SdkPolicy.kt and sdk_policy.cpp: a rule change goes to
// the four copies and the fixture together. Strings are measured in UTF-16
// code units where the TS measures `.length`, so the limits are the same.

import Foundation

public enum SdkPolicy {
    public static let maxLinkLength = 256
    /// Bytes of keys and values (UTF-8) per storage area and message.
    public static let storageQuotaBytes = 16384
    public static let maxLocaleLength = 35
    public static let defaultLocale = "en-US"
    public static let hapticBurst = 5.0
    public static let hapticPerSecond = 10.0
    /// `haptic-limit` is reported at most this often.
    public static let hapticReportMs = 1000.0
    /// The widget profile this glue enforces (`PROFILE_VERSION` in profile-tables.ts); the shim reports a mismatch with its own.
    public static let profileVersion = 1

    public static let themeKeys = [
        "bg_color", "text_color", "hint_color", "link_color", "button_color", "button_text_color",
        "secondary_bg_color", "accent_text_color", "subtitle_text_color", "destructive_text_color",
        "section_bg_color", "section_header_text_color", "section_separator_color", "header_bg_color", "bottom_bar_bg_color",
    ]
    public static let hapticImpactStyles = ["light", "medium", "heavy", "rigid", "soft"]
    public static let hapticNotificationTypes = ["error", "success", "warning"]
    public static let telegramHosts = ["t.me", "telegram.me", "telegram.dog"]
    /// Methods that need the task's user gesture (docs/protocol.md, "User activation").
    public static let privilegedMethods: Set<String> = ["openLink", "openTelegramLink"]

    /// A checked haptic: `impact` / `notification` with their value, `selection` without.
    public struct HapticCall: Equatable {
        public var haptic: String
        public var value: String?
    }

    /// A checked bridge call (the TS `WidgetAction`): the URL as the check gives it, a string.
    public enum Action: Equatable {
        case ready
        case openLink(String)
        case openTelegramLink(String)
        case haptic(HapticCall)
    }

    /// A checked storage commit (or preload): the areas present, each in full.
    public struct StorageCommit: Equatable {
        public var local: [String: String]?
        public var session: [String: String]?
        public var isEmpty: Bool { local == nil && session == nil }
    }

    /// UTF-8 length of a string (a Swift string holds no lone surrogates, which the TS counts 3 bytes).
    public static func utf8Length(_ s: String) -> Int { s.utf8.count }

    // MARK: links

    private static let hexDigits = Array("0123456789ABCDEF".utf8)
    private static func isHex(_ c: UInt16) -> Bool { (c >= 48 && c <= 57) || (c >= 65 && c <= 70) || (c >= 97 && c <= 102) }
    /// ASCII that may stand unescaped after the authority: RFC 3986 unreserved, reserved and `%`.
    private static let urlASCII = Set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~:/?#[]@!$&'()*+,;=%".utf16)
    /// Lowercases A–Z only: a Unicode mapping (U+212A KELVIN SIGN → `k`) must not turn a non-ASCII host into an ASCII one.
    private static func asciiLower<S: Sequence>(_ units: S) -> [UInt16] where S.Element == UInt16 {
        units.map { $0 >= 65 && $0 <= 90 ? $0 + 32 : $0 }
    }
    private static func isTelegramHost(_ host: String) -> Bool {
        telegramHosts.contains { host == $0 || host.hasSuffix("." + $0) }
    }

    /// The URL a link call opens, or nil when it breaks the rules: an absolute `https:` URL, no user
    /// name or password, an ASCII host of LDH labels (an IDN in its `xn--` form), an optional port, at
    /// most 256 characters once non-ASCII characters after the host are percent-encoded as UTF-8.
    /// `openTelegramLink` takes only t.me, telegram.me and telegram.dog (and their subdomains), any
    /// path; `openLink` takes every other host. Scheme and host come out lowercase, the rest as given.
    public static func checkLink(_ method: String, _ url: Any?) -> String? {
        guard let url = url as? String, utf8Length(url) <= maxLinkLength else { return nil }
        let u = Array(url.utf16)
        guard u.count >= 8, asciiLower(u[0..<8]) == Array("https://".utf16) else { return nil }
        var end = 8
        while end < u.count, u[end] != 0x2F, u[end] != 0x3F, u[end] != 0x23 { end += 1 } // / ? #
        let authority = u[8..<end]
        if authority.contains(0x40) { return nil } // @
        let colon = authority.firstIndex(of: 0x3A)
        let host = asciiLower(colon.map { authority[authority.startIndex..<$0] } ?? authority)
        let port = colon.map { Array(authority[authority.index(after: $0)...]) }
        if let port {
            guard (1...5).contains(port.count), port.allSatisfy({ $0 >= 48 && $0 <= 57 }) else { return nil }
            guard let n = Int(String(decoding: port, as: UTF16.self)), n <= 65535 else { return nil }
        }
        guard !host.isEmpty, host.count <= 253 else { return nil }
        for label in host.split(separator: 0x2E, omittingEmptySubsequences: false) { // .
            guard !label.isEmpty, label.count <= 63, label.first != 0x2D, label.last != 0x2D,
                  label.allSatisfy({ ($0 >= 97 && $0 <= 122) || ($0 >= 48 && $0 <= 57) || $0 == 0x2D }) else { return nil }
        }
        let hostString = String(decoding: host, as: UTF16.self)
        guard isTelegramHost(hostString) == (method == "openTelegramLink") else { return nil }
        var rest: [UInt8] = []
        var i = end
        while i < u.count {
            let c = u[i]
            if c < 0x80 {
                guard urlASCII.contains(c) else { return nil }
                if c == 0x25, !(i + 2 < u.count && isHex(u[i + 1]) && isHex(u[i + 2])) { return nil } // %
                rest.append(UInt8(c))
                i += 1
                continue
            }
            var cp = UInt32(c)
            if c >= 0xD800, c <= 0xDBFF, i + 1 < u.count, u[i + 1] & 0xFC00 == 0xDC00 {
                i += 1
                cp = 0x10000 + ((UInt32(c) - 0xD800) << 10) + (UInt32(u[i]) - 0xDC00)
            } else if c >= 0xD800, c <= 0xDFFF {
                return nil
            }
            let bytes: [UInt32] = cp < 0x800 ? [0xC0 | (cp >> 6), 0x80 | (cp & 63)]
                : cp < 0x10000 ? [0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63)]
                : [0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 63), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63)]
            for b in bytes { rest += [0x25, hexDigits[Int(b >> 4)], hexDigits[Int(b & 15)]] }
            i += 1
        }
        let out = "https://" + hostString + (port.map { ":" + String(decoding: $0, as: UTF16.self) } ?? "") + String(decoding: rest, as: UTF8.self)
        return out.utf16.count > maxLinkLength ? nil : out
    }

    // MARK: haptics and bridge calls

    /// The haptic of a `haptic` bridge call's args, or nil: `['impact', style]`, `['notification', type]`, `['selection']`.
    public static func checkHaptic(_ args: Any?) -> HapticCall? {
        guard let args = args as? [Any], let kind = args.first as? String else { return nil }
        let value = args.count == 2 ? args[1] as? String : nil
        if kind == "impact", args.count == 2, let value, hapticImpactStyles.contains(value) { return HapticCall(haptic: "impact", value: value) }
        if kind == "notification", args.count == 2, let value, hapticNotificationTypes.contains(value) { return HapticCall(haptic: "notification", value: value) }
        if kind == "selection", args.count == 1 { return HapticCall(haptic: "selection", value: nil) }
        return nil
    }

    /// The action of a bridge call, or nil when the method is unknown or its arguments are wrong (`bridge-rejected`).
    public static func checkBridgeCall(_ method: Any?, _ args: Any?) -> Action? {
        guard let args = args as? [Any], let method = method as? String else { return nil }
        switch method {
        case "ready":
            return args.isEmpty ? .ready : nil
        case "openLink", "openTelegramLink":
            guard args.count == 1, let url = checkLink(method, args[0]) else { return nil }
            return method == "openLink" ? .openLink(url) : .openTelegramLink(url)
        case "haptic":
            return checkHaptic(args).map { .haptic($0) }
        default:
            return nil
        }
    }

    public enum HapticDecision: String {
        /// Deliver.
        case play
        /// Drop silently.
        case drop
        /// Drop and report `haptic-limit` (at most once per second).
        case report
    }

    /// Haptics need no gesture but a visible widget and room in the bucket (5 calls, refilled at 10 per second).
    public struct HapticGate {
        private var tokens = SdkPolicy.hapticBurst
        private var last: Double?
        private var reported: Double?

        public init() {}

        public mutating func decide(nowMs now: Double, visible: Bool) -> HapticDecision {
            if let last { tokens = min(SdkPolicy.hapticBurst, tokens + (max(0, now - last) * SdkPolicy.hapticPerSecond) / 1000) }
            last = now
            if visible, tokens >= 1 {
                tokens -= 1
                return .play
            }
            if let reported, now - reported < SdkPolicy.hapticReportMs { return .drop }
            reported = now
            return .report
        }
    }

    // MARK: theme and locale

    private static func isColor(_ v: String) -> Bool {
        let u = Array(v.utf16)
        return u.count == 7 && u[0] == 0x23 && u[1...].allSatisfy(isHex)
    }

    /// The theme keys of `params` whose values are `#rrggbb` (lowercased); everything else is dropped.
    public static func checkThemeParams(_ params: Any?) -> [String: String] {
        guard let params = params as? [String: Any] else { return [:] }
        var out: [String: String] = [:]
        for key in themeKeys {
            if let v = params[key] as? String, isColor(v) { out[key] = v.lowercased() }
        }
        return out
    }

    private static func isASCIILetter(_ c: UInt16) -> Bool { (c >= 65 && c <= 90) || (c >= 97 && c <= 122) }

    /// A BCP 47 tag of at most 35 characters (`ru`, `en-US`, `pt-BR`, `zh-Hant-TW`), or nil.
    public static func checkLocale(_ locale: Any?) -> String? {
        guard let locale = locale as? String else { return nil }
        let u = Array(locale.utf16)
        guard !u.isEmpty, u.count <= maxLocaleLength else { return nil }
        // ^[A-Za-z]{2,8}(-[A-Za-z0-9]{1,8})*$
        let parts = u.split(separator: 0x2D, omittingEmptySubsequences: false) // -
        guard let first = parts.first, (2...8).contains(first.count), first.allSatisfy(isASCIILetter) else { return nil }
        for part in parts.dropFirst() {
            guard (1...8).contains(part.count), part.allSatisfy({ isASCIILetter($0) || ($0 >= 48 && $0 <= 57) }) else { return nil }
        }
        return locale
    }

    // MARK: storage

    /// UTF-8 bytes of an area's keys and values.
    public static func storageBytes(_ area: [String: String]) -> Int {
        area.reduce(0) { $0 + utf8Length($1.key) + utf8Length($1.value) }
    }

    /// An object of strings within the quota, or nil.
    public static func checkStorageArea(_ area: Any?) -> [String: String]? {
        guard let area = area as? [String: Any] else { return nil }
        var out: [String: String] = [:]
        for (k, v) in area {
            guard let s = v as? String else { return nil }
            out[k] = s
        }
        return storageBytes(out) <= storageQuotaBytes ? out : nil
    }

    /// A commit (or a preload) of `local` and/or `session`, each checked whole; nil when anything is wrong (`storage-rejected`).
    public static func checkStorageCommit(_ commit: Any?) -> StorageCommit? {
        guard let commit = commit as? [String: Any] else { return nil }
        var out = StorageCommit()
        for (k, v) in commit {
            guard k == "local" || k == "session", let area = checkStorageArea(v) else { return nil }
            if k == "local" { out.local = area } else { out.session = area }
        }
        return out
    }
}
