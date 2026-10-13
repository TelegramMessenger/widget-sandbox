import Foundation
import WidgetSandboxProtocol

enum CanvasWords {
    static let maxWords = 2_000_000
    static let maxStringBytes = 4 << 20
    static let maxStrings = 65_536
    static let maxPayloads = 64
    static let maxListLength = 4096

    static func batch(words: UnsafeBufferPointer<Double>, strings joined: String) -> WidgetCanvasBatch? {
        guard words.count >= 1, words.count <= maxWords, joined.utf8.count <= maxStringBytes else { return nil }
        let m = words[words.count - 1]
        guard m >= 0, m <= Double(min(maxStrings, words.count - 1)), m == m.rounded() else { return nil }
        let count = Int(m)
        let commandWords = words.count - 1 - count
        var strings: [String] = []
        strings.reserveCapacity(count)
        var joined = joined
        let ascii: Bool? = joined.withUTF8 { b in
            var total = 0.0
            for k in 0..<count { total += words[commandWords + k] }
            guard total == Double(b.count), !b.contains(where: { $0 >= 0x80 }) else { return nil }
            var at = 0
            for k in 0..<count {
                let length = words[commandWords + k]
                guard length >= 0, length == length.rounded(), Double(b.count - at) >= length else { return false }
                let end = at + Int(length)
                strings.append(String(decoding: UnsafeBufferPointer(rebasing: b[at..<end]), as: UTF8.self))
                at = end
            }
            return at == b.count
        }
        if ascii == false { return nil }
        if ascii == true { return WidgetCanvasBatch(words: Array(words[0..<commandWords]), strings: strings) }
        let units = Array(joined.utf16)
        var at = 0
        for k in 0..<count {
            let length = words[commandWords + k]
            guard length >= 0, length == length.rounded(), Double(units.count - at) >= length else { return nil }
            let end = at + Int(length)
            strings.append(String(decoding: units[at..<end], as: UTF16.self))
            at = end
        }
        guard at == units.count else { return nil }
        return WidgetCanvasBatch(words: Array(words[0..<commandWords]), strings: strings)
    }

    private static func plain(_ s: String) -> Bool {
        var s = s
        return s.withUTF8 { b in
            if b.count > CanvasBatchFastPath.maxStringBytes { return false }
            for c in b where c < 0x20 || c == 0x22 || c == 0x5c { return false }
            return true
        }
    }

    private static func mentionsIcon(_ s: String) -> Bool {
        var s = s
        return s.withUTF8 { b in
            guard b.count >= 5 else { return false }
            for i in 4..<b.count where b[i] == 0x3a && b[i - 4] | 0x20 == 0x69 && b[i - 3] | 0x20 == 0x63 && b[i - 2] | 0x20 == 0x6f && b[i - 1] | 0x20 == 0x6e { return true }
            return false
        }
    }

    private static func reference(_ id: Double) -> Bool {
        id.isFinite && id == id.rounded() && abs(id) < 1e15
    }

    static func envelope(_ batch: WidgetCanvasBatch) -> Bool {
        var plainStrings = [Bool](repeating: false, count: batch.strings.count)
        for (i, s) in batch.strings.enumerated() {
            plainStrings[i] = plain(s)
            if mentionsIcon(s) { return false }
        }
        var commands = 0
        func scalar(_ v: WidgetCanvasBatch.Value) -> Bool {
            switch v {
            case .number, .null, .bool: return true
            case .string(let i): return plainStrings[i]
            default: return false
            }
        }
        func argument(_ v: WidgetCanvasBatch.Value, lists: Bool) -> Bool {
            switch v {
            case .gradient(let id), .path(let id): return reference(id)
            case .node: return false
            case .list(let range): return lists && range.count <= maxListLength && !batch.words[range].contains { !$0.isFinite }
            default: return scalar(v)
            }
        }
        func name(_ index: Int, in set: CanvasBatchFastPath.WordSet) -> Bool {
            index >= 0 && plainStrings[index] && set.contains(batch.strings[index])
        }
        guard !batch.words.isEmpty else { return false }
        return batch.forEach { command, values in
            commands += 1
            if commands > CanvasBatchFastPath.maxCmdsPerOp { return false }
            switch command.kind {
            case 0:
                return name(command.name, in: CanvasBatchFastPath.callMethods) && values.count <= 14 && values.allSatisfy { argument($0, lists: true) }
            case 1:
                return name(command.name, in: CanvasBatchFastPath.setProperties) && values.count == 1 && argument(values[0], lists: false)
            case 2:
                guard command.name == -1, values.count == 2, case .number(let w) = values[0], case .number(let h) = values[1] else { return false }
                return w.isFinite && h.isFinite && w >= 0 && w <= 16384 && h >= 0 && h <= 16384
            case 3:
                return command.ident.isFinite && name(command.name, in: CanvasBatchFastPath.gradientTypes) && values.count <= 13 && values.allSatisfy { argument($0, lists: true) }
            case 4:
                guard command.ident.isFinite, command.name == -1, values.count == 2, case .number(let offset) = values[0], case .string(let color) = values[1] else { return false }
                return offset.isFinite && plainStrings[color] && batch.strings[color].utf8.count < 256
            case 5:
                guard command.ident.isFinite, command.name == -1, values.count == 1 else { return false }
                if case .null = values[0] { return true }
                if case .string(let d) = values[0] { return plainStrings[d] }
                return false
            case 6:
                return command.ident.isFinite && name(command.name, in: CanvasBatchFastPath.pathMethods) && values.count <= 13 && values.allSatisfy { argument($0, lists: true) }
            default:
                return false
            }
        }
    }

    static func commandsJSON(_ batch: WidgetCanvasBatch) -> String? {
        batch.json(number: JSNumberText.string)
    }

    private static let prefix = Array("{\"op\":\"canvas\",\"id\":".utf8)
    private static let middle = Array(",\"bin\":".utf8)

    private struct Placeholder {
        var range: Range<Int>
        var id: Int
        var key: Int
    }

    private static func placeholders(_ b: UnsafeBufferPointer<UInt8>) -> [Placeholder] {
        var out: [Placeholder] = []
        var i = 0
        let n = b.count
        func digits(_ i: inout Int) -> Int? {
            let start = i
            var v = 0
            while i < n, b[i] >= 0x30, b[i] <= 0x39, i - start < 10 { v = v * 10 + Int(b[i] - 0x30); i += 1 }
            return i > start ? v : nil
        }
        while i + prefix.count < n {
            guard let hit = memchr(b.baseAddress! + i, 0x7b, n - i) else { break }
            let at = b.baseAddress!.distance(to: hit.assumingMemoryBound(to: UInt8.self))
            var j = at
            if n - j > prefix.count, prefix.indices.allSatisfy({ b[j + $0] == prefix[$0] }) {
                j += prefix.count
                if let id = digits(&j), n - j > middle.count, middle.indices.allSatisfy({ b[j + $0] == middle[$0] }) {
                    j += middle.count
                    if let key = digits(&j), j < n, b[j] == 0x7d {
                        out.append(Placeholder(range: at..<(j + 1), id: id, key: key))
                        i = j + 1
                        continue
                    }
                }
            }
            i = at + 1
        }
        return out
    }

    static func only(_ json: String, _ canvas: [Int: WidgetCanvasBatch]) -> [(id: Int, batch: WidgetCanvasBatch)]? {
        var json = json
        return json.withUTF8 { b -> [(id: Int, batch: WidgetCanvasBatch)]? in
            let found = placeholders(b)
            guard !found.isEmpty, b.first == 0x5b, b.last == 0x5d else { return nil }
            var expected = 1
            var out: [(id: Int, batch: WidgetCanvasBatch)] = []
            for (index, p) in found.enumerated() {
                guard p.range.lowerBound == expected, let batch = canvas[p.key] else { return nil }
                expected = p.range.upperBound + 1
                if index == found.count - 1 { guard p.range.upperBound == b.count - 1 else { return nil } } else { guard b[p.range.upperBound] == 0x2c else { return nil } }
                out.append((p.id, batch))
            }
            return out
        }
    }

    static func splice(_ json: String, _ canvas: [Int: WidgetCanvasBatch]) -> String {
        var json = json
        return json.withUTF8 { b -> String in
            let found = placeholders(b)
            if found.isEmpty { return String(decoding: b, as: UTF8.self) }
            var out = ""
            out.reserveCapacity(b.count)
            var last = 0
            for p in found {
                out += String(decoding: b[last..<p.range.lowerBound], as: UTF8.self)
                out += "{\"op\":\"canvas\",\"id\":\(p.id),\"cmds\":" + (canvas[p.key].flatMap(commandsJSON) ?? "null") + "}"
                last = p.range.upperBound
            }
            out += String(decoding: b[last...], as: UTF8.self)
            return out
        }
    }
}
