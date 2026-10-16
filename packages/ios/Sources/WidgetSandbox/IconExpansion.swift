import Foundation

public typealias WidgetAssetResolver = (String) -> String?

/// `icon:<name>[?query]` tokens → the host resolver's `data:image/` URL, before validation. One instance per widget
/// (`TrustedValidator.setAssetResolver`). The prefix is exactly `icon:`, lowercase; only the CSS function name `url(`
/// is ASCII case-insensitive, as in CSS.
final class IconExpansion {
    static let maxURLBytes = 64 * 1024
    static let maxIconsPerBatch = 1024
    static let maxBytesPerBatch = 2 << 20
    /// Per widget: distinct tokens the resolver is ever asked for, and the bytes of its answers kept for the widget.
    /// Past either, new tokens stay unresolved (blocked like any other non-`data:` URL); known ones keep working.
    static let maxIconsPerWidget = 256
    static let maxResolvedBytesPerWidget = 2 << 20

    private static let attribute = try! NSRegularExpression(pattern: "(\"(?!data\"|text\")[A-Za-z_:-]{1,32}\":\")(icon:[A-Za-z0-9_.-]{0,96}(?:\\?[A-Za-z0-9=&.%#_-]{0,400})?)(?=\")")
    private static let url = try! NSRegularExpression(pattern: "((?i:url)\\((?:\\\\?[\"'])?\\s*)(icon:[A-Za-z0-9_.-]{0,96}(?:\\?[A-Za-z0-9=&.%#_-]{0,400})?)")
    private static let fieldValue = try! NSRegularExpression(pattern: "\\Aicon:[A-Za-z0-9_.-]{0,96}(?:\\?[A-Za-z0-9=&.%#_-]{0,400})?\\z")
    private static let fieldKey = try! NSRegularExpression(pattern: "\\A[A-Za-z_:-]{1,32}\\z")
    private static let rawURL = try! NSRegularExpression(pattern: "((?i:url)\\((?:[\"'])?\\p{Z}*)(icon:[A-Za-z0-9_.-]{0,96}(?:\\?[A-Za-z0-9=&.%#_-]{0,400})?)")

    private let resolver: WidgetAssetResolver
    private let diag: (String, String) -> Void
    /// Every token this widget asked for and the answer ("" for none); never evicted, at most `maxIconsPerWidget`.
    private var cache: [String: String] = [:]
    private var resolvedBytes = 0
    private var widgetLimitReported = false
    private var icons = 0
    private var bytes = 0
    private var limitReported = false

    init(resolver: @escaping WidgetAssetResolver, diag: @escaping (String, String) -> Void) {
        self.resolver = resolver
        self.diag = diag
    }

    func beginBatch() {
        icons = 0
        bytes = 0
        limitReported = false
    }

    static func checked(_ url: String) -> String? {
        let b = Array(url.utf8)
        guard b.count <= maxURLBytes, url.hasPrefix("data:image/"), b.count > 11 else { return nil }
        for c in b {
            let ok = (c >= 0x61 && c <= 0x7a) || (c >= 0x41 && c <= 0x5a) || (c >= 0x30 && c <= 0x39)
                || c == 0x2d || c == 0x2e || c == 0x5f || c == 0x7e || c == 0x2c || c == 0x2f || c == 0x3a || c == 0x3d || c == 0x25 || c == 0x3b || c == 0x2b
            if !ok { return nil }
        }
        return url
    }

    private func limit(_ detail: String) {
        if limitReported { return }
        limitReported = true
        diag("icon-limit", detail)
    }

    /// The widget's own budget: reported once per widget, and the rest of the batch still expands known tokens.
    private func widgetLimit(_ detail: String) {
        if widgetLimitReported { return }
        widgetLimitReported = true
        diag("icon-limit", detail)
    }

    func dataURL(_ icon: String) -> String? {
        guard icons < Self.maxIconsPerBatch else {
            limit("more than \(Self.maxIconsPerBatch) icon: URLs in one batch")
            return nil
        }
        icons += 1
        let url: String
        if let hit = cache[icon] {
            url = hit
        } else {
            guard cache.count < Self.maxIconsPerWidget, resolvedBytes < Self.maxResolvedBytesPerWidget else {
                widgetLimit("more than \(Self.maxIconsPerWidget) distinct icon: tokens or \(Self.maxResolvedBytesPerWidget) bytes of icons in this widget")
                return nil
            }
            let answer = resolver(icon).flatMap(Self.checked) ?? ""
            if resolvedBytes + answer.utf8.count > Self.maxResolvedBytesPerWidget {
                cache[icon] = ""
                resolvedBytes = Self.maxResolvedBytesPerWidget
                widgetLimit("more than \(Self.maxResolvedBytesPerWidget) bytes of icons in this widget")
                return nil
            }
            resolvedBytes += answer.utf8.count
            cache[icon] = answer
            url = answer
        }
        if url.isEmpty { return nil }
        guard bytes + url.utf8.count <= Self.maxBytesPerBatch else {
            limit("icon: expansion over \(Self.maxBytesPerBatch) bytes in one batch")
            return nil
        }
        bytes += url.utf8.count
        return url
    }

    func expand(_ json: String) -> String {
        guard Self.mayContainIcon(json) else { return json }
        return replace(Self.url, in: replace(Self.attribute, in: json))
    }

    func expandField(_ value: String, key: String?) -> String? {
        guard value.utf8.count >= 5, Self.mayContainIcon(value) else { return nil }
        var text = value
        if let key, key != "data", key != "text", Self.matches(Self.fieldKey, key), Self.matches(Self.fieldValue, value), let data = dataURL(value) {
            text = data
        }
        let expanded = replace(Self.rawURL, in: text)
        return expanded == value ? nil : expanded
    }

    /// A cheap pre-filter (ASCII case-insensitive, so it may say yes for `ICON:`); the patterns above decide.
    static func mayContainIcon(_ json: String) -> Bool {
        let found = json.utf8.withContiguousStorageIfAvailable { mayContainIcon(bytes: $0) }
        return found ?? (json.range(of: "icon:", options: .caseInsensitive) != nil)
    }

    static func mayContainIcon(bytes b: UnsafeBufferPointer<UInt8>) -> Bool {
        guard let base = b.baseAddress, b.count >= 5 else { return false }
        var i = 4
        while i < b.count, let hit = memchr(base + i, 0x3a, b.count - i) {
            let k = base.distance(to: hit.assumingMemoryBound(to: UInt8.self))
            if b[k - 4] | 0x20 == 0x69, b[k - 3] | 0x20 == 0x63, b[k - 2] | 0x20 == 0x6f, b[k - 1] | 0x20 == 0x6e { return true }
            i = k + 1
        }
        return false
    }

    private static func matches(_ regex: NSRegularExpression, _ s: String) -> Bool {
        regex.firstMatch(in: s, range: NSRange(location: 0, length: (s as NSString).length)) != nil
    }

    private func replace(_ regex: NSRegularExpression, in text: String) -> String {
        let ns = text as NSString
        var out = ""
        var last = 0
        regex.enumerateMatches(in: text, range: NSRange(location: 0, length: ns.length)) { match, _, stop in
            guard let match else { return }
            if limitReported {
                stop.pointee = true
                return
            }
            let icon = ns.substring(with: match.range(at: 2))
            guard let data = dataURL(icon) else { return }
            out += ns.substring(with: NSRange(location: last, length: match.range(at: 2).location - last))
            out += data
            last = NSMaxRange(match.range(at: 2))
        }
        if last == 0 { return text }
        return out + ns.substring(from: last)
    }
}
