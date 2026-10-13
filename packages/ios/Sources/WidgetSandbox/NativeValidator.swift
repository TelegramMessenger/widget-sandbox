import Foundation
import WidgetSandboxProtocol

protocol NativeValidatorOracle: AnyObject {
    func styleProperty(_ name: String, _ value: String) -> String?
    func styleAttribute(_ value: String) -> String
    func cssValue(_ value: String, where: String) -> String?
    func stylesheet(_ text: String) -> String
    func canvasCommands(_ json: String) -> String
    var diagnostics: Int { get }
}

struct NativeValidatorTables {
    var html: Set<String> = []
    var svg: [String: String] = [:]
    var dropped: Set<String> = []
    var attributes: Set<String> = []
    var silent: Set<String> = []
    var url: Set<String> = []
    var css: Set<String> = []
    var animationValues: Set<String> = []
    var animationTags: Set<String> = []
    var fragmentTags: Set<String> = []
    var svgCase: [String: String] = [:]
    var props: [String: (type: String, tags: Set<String>?)] = [:]
    var methods: [String: Int] = [:]
    var mime: Set<String> = []
    var placeholder = ""
    var maxNodes = 50_000
    var maxText = 64 * 1024
    var maxAttr = 4 * 1024 * 1024
    var maxOps = 200_000
    var maxCanvasCommands = 500_000
    var limbo = 0
    var complexTextBudget = 0
    var textBudget = 0

    init?(json: String) {
        guard let root = JSONText.parse(json)?.object else { return nil }
        func strings(_ key: String) -> [String]? {
            guard let list = root[key]?.array else { return nil }
            var out: [String] = []
            for item in list {
                guard let s = item.string else { return nil }
                out.append(s)
            }
            return out
        }
        func pairs(_ key: String) -> [String: String]? {
            guard let list = root[key]?.array else { return nil }
            var out: [String: String] = [:]
            for item in list {
                guard let pair = item.array, pair.count == 2, let k = pair[0].string, let v = pair[1].string else { return nil }
                out[k] = v
            }
            return out
        }
        guard let html = strings("html"), let svg = pairs("svg"), let dropped = strings("dropped"), let attributes = strings("attrs"), let silent = strings("silent"),
              let url = strings("url"), let css = strings("css"), let anim = strings("anim"), let animTags = strings("animTags"), let fragTags = strings("fragTags"),
              let svgCase = pairs("svgCase"), let mime = strings("mime"), let placeholder = root["placeholder"]?.string,
              let props = root["props"]?.array, let methods = root["methods"]?.array, let limits = root["limits"]?.array, limits.count == 6 else { return nil }
        self.html = Set(html)
        self.svg = svg
        self.dropped = Set(dropped)
        self.attributes = Set(attributes)
        self.silent = Set(silent)
        self.url = Set(url)
        self.css = Set(css)
        animationValues = Set(anim)
        animationTags = Set(animTags)
        fragmentTags = Set(fragTags)
        self.svgCase = svgCase
        self.mime = Set(mime)
        self.placeholder = placeholder
        for item in props {
            guard let p = item.array, p.count == 3, let name = p[0].string, let type = p[1].string else { return nil }
            var tags: Set<String>?
            if let list = p[2].array { tags = Set(list.compactMap { $0.string }) }
            self.props[name] = (type, tags)
        }
        for item in methods {
            guard let p = item.array, p.count == 2, let name = p[0].string, let n = p[1].number else { return nil }
            self.methods[name] = Int(n)
        }
        let numbers = limits.compactMap { $0.number }
        guard numbers.count == 6 else { return nil }
        maxNodes = Int(numbers[0])
        maxText = Int(numbers[1])
        maxAttr = Int(numbers[2])
        maxOps = Int(numbers[3])
        maxCanvasCommands = Int(numbers[4])
        limbo = Int(numbers[5])
        complexTextBudget = Int(root["complexTextBudget"]?.number ?? 0)
        textBudget = Int(root["textBudget"]?.number ?? 0)
    }
}

final class NativeValidator {
    static let documentId = 1
    static let maxId = 1 << 31

    enum Kind: UInt8 {
        case element
        case text
        case comment
        case document
    }

    final class Node {
        let id: Int
        let kind: Kind
        let tag: String
        let svg: Bool
        var rendered: Bool
        let isStyle: Bool
        let adopted: Bool
        var parent: Int?
        var children: [Int] = []
        var data: String
        var attached = false
        var complex = 0
        let seq: Int

        init(id: Int, kind: Kind, tag: String = "", svg: Bool = false, rendered: Bool = true, isStyle: Bool = false, adopted: Bool = false, data: String = "", seq: Int) {
            self.id = id
            self.kind = kind
            self.tag = tag
            self.svg = svg
            self.rendered = rendered
            self.isStyle = isStyle
            self.adopted = adopted
            self.data = data
            self.seq = seq
        }
    }

    let tables: NativeValidatorTables
    weak var oracle: NativeValidatorOracle?
    var diag: (String, String) -> Void
    let intern = BinaryInternTable()

    private(set) var nodes: [Int: Node] = [:]
    private var adoptedHTML = false, adoptedHead = false, adoptedBody = false
    private var dirtyStyles: [Int: Int] = [:]
    private var dirtySeq = 0
    private var nodeSeq = 0
    private var out: [WidgetOp] = []
    private var styleCache: [String: [String: String]] = [:]
    private var styleCacheSize = 0
    var liveAnims = Set<Double>()

    init(tables: NativeValidatorTables, oracle: NativeValidatorOracle?, diag: @escaping (String, String) -> Void) {
        self.tables = tables
        self.oracle = oracle
        self.diag = diag
        reset()
    }

    func reset() {
        nodes.removeAll()
        adoptedHTML = false
        adoptedHead = false
        adoptedBody = false
        dirtyStyles.removeAll()
        liveAnims.removeAll()
        churnTokens = Double(Self.complexTextBudget)
        churnAt = nil
        complexText = 0
        totalText = 0
        intern.reset()
        nodeSeq = 0
        nodes[Self.documentId] = Node(id: Self.documentId, kind: .document, rendered: false, seq: nextSeq())
    }

    var nodeCount: Int { nodes.count }

    private func nextSeq() -> Int {
        nodeSeq += 1
        return nodeSeq
    }

    func renderedTag(_ id: Int) -> String? {
        guard isId(id), let n = nodes[id], n.kind == .element, n.rendered else { return nil }
        return n.tag
    }

    @inline(__always) private func isId(_ v: Int) -> Bool { v > Self.documentId && v < Self.maxId }

    private func isId(_ v: JSONValue?) -> Int? {
        guard case .number(let d)? = v, d.isFinite, d == d.rounded(.towardZero), d > Double(Self.documentId), d < Double(Self.maxId) else { return nil }
        return Int(d)
    }

    private func isStr(_ v: JSONValue?, _ max: Int) -> String? {
        guard case .string(let s)? = v else { return nil }
        if s.utf8.count > max, s.utf16.count > max { return nil }
        return s
    }

    @inline(__always) private func fits(_ s: BinaryString, _ max: Int) -> Bool { s.units <= max }

    // MARK: batches

    func process(_ decoded: BinaryOps.Decoded, canvas: [Int: WidgetCanvasBatch]) -> [WidgetOp] {
        out = []
        if decoded.tooMany {
            diag("protocol-violation", "ops batch is not an array or too large")
            return []
        }
        for op in decoded.ops {
            if !apply(op, canvas: canvas) {
                diag("protocol-violation", "rejected op " + JSONText.safe(BinaryOps.json(op, canvas: canvas)))
            }
        }
        return finish()
    }

    func process(json value: JSONValue) -> [WidgetOp] {
        out = []
        guard case .array(let list) = value, list.count <= tables.maxOps else {
            diag("protocol-violation", "ops batch is not an array or too large")
            return []
        }
        for item in list {
            if case .object(let o) = item, apply(o) { continue }
            var text: [UInt8] = []
            JSONText.write(item, to: &text)
            diag("protocol-violation", "rejected op " + JSONText.safe(text))
        }
        return finish()
    }

    private func finish() -> [WidgetOp] {
        if !dirtyStyles.isEmpty {
            for (id, _) in dirtyStyles.sorted(by: { $0.value < $1.value }) {
                if let style = nodes[id] { out.append(WidgetOp(css: id, text: styleText(style))) }
            }
            dirtyStyles.removeAll()
        }
        let result = out
        out = []
        return result
    }

    func setTheme() -> [WidgetOp] {
        var styles: [Node] = []
        for n in nodes.values where n.kind == .element && n.isStyle && n.rendered { styles.append(n) }
        styleCache.removeAll()
        styleCacheSize = 0
        return styles.sorted { $0.seq < $1.seq }.map { WidgetOp(css: $0.id, text: styleText($0)) }
    }

    private func styleText(_ style: Node) -> String {
        var css = ""
        for c in style.children { css += nodes[c]?.data ?? "" }
        return oracle?.stylesheet(css) ?? ""
    }

    private func markDirty(_ id: Int) {
        if dirtyStyles[id] == nil {
            dirtySeq += 1
            dirtyStyles[id] = dirtySeq
        }
    }

    // MARK: binary front end

    private func apply(_ op: BinaryOp, canvas: [Int: WidgetCanvasBatch]) -> Bool {
        switch op {
        case .element(let id, let tag, let svg, let attributes):
            guard isId(id), fits(tag, 64) else { return false }
            let list = BinaryOps.normalizedAttributes(attributes).map { pair -> (String, String?) in
                (pair.0.s, fits(pair.1, tables.maxAttr) ? pair.1.s : nil)
            }
            return element(id, tag: tag.s, svg: svg, attributes: list)
        case .text(let id, let data):
            guard isId(id), fits(data, tables.maxText) else { return false }
            return characterData(id, data.s, text: true)
        case .comment(let id, let data):
            guard isId(id), fits(data, tables.maxText) else { return false }
            return characterData(id, data.s, text: false)
        case .data(let id, let data):
            guard isId(id), fits(data, tables.maxText) else { return false }
            return setData(id, data.s)
        case .attribute(let id, let name, let value):
            guard isId(id), fits(name, 128), fits(value, tables.maxAttr) else { return false }
            return attribute(id, name.s, value.s)
        case .removeAttribute(let id, let name):
            guard isId(id), fits(name, 128) else { return false }
            return removeAttribute(id, name.s)
        case .insert(let parent, let id, let before):
            guard isId(id), isId(parent) || parent == Self.documentId, before == 0 || isId(before) else { return false }
            return insert(parent: parent, id: id, before: before == 0 ? nil : before)
        case .remove(let id):
            guard isId(id) else { return false }
            return remove(id)
        case .style(let id, let name, let value, let important):
            guard isId(id), fits(name, 128), fits(value, tables.maxAttr) else { return false }
            return style(id, name.s, value.s, important: important)
        case .canvas(let id, let key):
            guard let batch = canvas[key] else { return false }
            return canvasWords(id, batch)
        case .json(let value):
            guard case .object(let o) = value else { return false }
            return apply(o)
        }
    }

    // MARK: JSON front end

    private func apply(_ o: JSONObject) -> Bool {
        guard case .string(let kind)? = o["op"] else { return false }
        switch kind {
        case "el":
            guard let id = isId(o["id"]), let tag = isStr(o["tag"], 64) else { return false }
            var svg = false
            if let ns = o["ns"] {
                guard case .string("svg") = ns else { return false }
                svg = true
            }
            var list: [(String, String?)] = []
            if let attrs = o["attrs"] {
                guard case .object(let a) = attrs else { return false }
                list = a.entries.map { ($0.0, isStr($0.1, tables.maxAttr)) }
            }
            return element(id, tag: tag, svg: svg, attributes: list)
        case "text", "comment":
            guard let id = isId(o["id"]), let data = isStr(o["data"], tables.maxText) else { return false }
            return characterData(id, data, text: kind == "text")
        case "attr":
            guard let id = isId(o["id"]), let name = isStr(o["name"], 128), let value = isStr(o["value"], tables.maxAttr) else { return false }
            return attribute(id, name, value)
        case "rattr":
            guard let id = isId(o["id"]), let name = isStr(o["name"], 128) else { return false }
            return removeAttribute(id, name)
        case "data":
            guard let id = isId(o["id"]), let data = isStr(o["data"], tables.maxText) else { return false }
            return setData(id, data)
        case "ins":
            guard let id = isId(o["id"]) else { return false }
            let parent: Int
            if let p = isId(o["parent"]) { parent = p } else if case .number(let d)? = o["parent"], d == Double(Self.documentId) { parent = Self.documentId } else { return false }
            var before: Int?
            switch o["before"] {
            case .null?: before = nil
            default:
                guard let b = isId(o["before"]) else { return false }
                before = b
            }
            return insert(parent: parent, id: id, before: before)
        case "rm":
            guard let id = isId(o["id"]) else { return false }
            return remove(id)
        case "style":
            guard let id = isId(o["id"]), let name = isStr(o["name"], 128), let value = isStr(o["value"], tables.maxAttr) else { return false }
            var important = false
            if case .bool(true)? = o["important"] { important = true }
            return style(id, name, value, important: important)
        case "prop":
            return prop(o)
        case "call":
            return call(o)
        case "canvas":
            guard let id = isId(o["id"]), case .array(let cmds)? = o["cmds"], cmds.count <= tables.maxCanvasCommands else { return false }
            guard let node = nodes[id], node.kind == .element, node.tag == "canvas" else { return false }
            var text: [UInt8] = []
            JSONText.write(.array(cmds), to: &text)
            canvasJSON(id, String(decoding: text, as: UTF8.self))
            return true
        default:
            return false
        }
    }

    // MARK: rules

    private func newNode(_ id: Int, _ make: () -> Node) -> Node? {
        if nodes[id] != nil || nodes.count >= tables.maxNodes { return nil }
        let n = make()
        nodes[id] = n
        return n
    }

    private static func isTagName(_ s: String) -> Bool {
        var s = s
        return s.withUTF8 { b in
            guard let first = b.first, isLetter(first) else { return false }
            for c in b.dropFirst() where !(isWord(c) || c == 0x2e || c == 0x2d) { return false }
            return true
        }
    }

    private static func isAttrName(_ s: String) -> Bool {
        var s = s
        return s.withUTF8 { b in
            guard let first = b.first, isLetter(first) || first == 0x5f else { return false }
            for c in b.dropFirst() where !(isWord(c) || c == 0x2e || c == 0x3a || c == 0x2d) { return false }
            return true
        }
    }

    private static func isStyleName(_ s: String) -> Bool {
        var s = s
        return s.withUTF8 { b in
            var i = 0
            while i < 2, i < b.count, b[i] == 0x2d { i += 1 }
            guard i < b.count, isLetter(b[i]) || b[i] == 0x5f else { return false }
            i += 1
            while i < b.count {
                guard isWord(b[i]) || b[i] == 0x2d else { return false }
                i += 1
            }
            return true
        }
    }

    @inline(__always) private static func isLetter(_ c: UInt8) -> Bool { (c | 0x20) >= 0x61 && (c | 0x20) <= 0x7a }
    @inline(__always) private static func isWord(_ c: UInt8) -> Bool { isLetter(c) || (c >= 0x30 && c <= 0x39) || c == 0x5f }

    private static func asciiLower(_ s: String) -> String {
        var s = s
        let needs = s.withUTF8 { b in b.contains { $0 >= 0x41 && $0 <= 0x5a } }
        return needs ? s.lowercased() : s
    }

    private func element(_ id: Int, tag: String, svg: Bool, attributes: [(String, String?)]) -> Bool {
        guard Self.isTagName(tag) else { return false }
        let lower = Self.asciiLower(tag)
        var outTag: String?
        var extra: [(name: String, value: String)] = []
        let isStyle = lower == "style"
        if svg {
            if isStyle { outTag = "style" } else if tables.dropped.contains(lower) && lower != "title" { outTag = nil } else { outTag = tables.svg[lower] }
            if outTag == nil && !tables.dropped.contains(lower) { diag("tag-dropped", "<svg:\(tag)>") }
        } else if tables.dropped.contains(lower) {
            outTag = nil
            if lower != "script" && lower != "title" && lower != "meta" { diag("tag-dropped", "<\(lower)>") }
        } else if isStyle || tables.html.contains(lower) {
            outTag = lower
        } else {
            outTag = "span"
            extra = [("data-tg-tag", lower)]
            diag("tag-replaced", "<\(lower)> rendered as <span>")
        }
        var adopt: String?
        if !svg && (lower == "html" || lower == "head" || lower == "body") {
            let already = lower == "html" ? adoptedHTML : lower == "head" ? adoptedHead : adoptedBody
            if already {
                outTag = "div"
                extra = [("data-tg-tag", lower)]
            } else {
                if lower == "html" { adoptedHTML = true } else if lower == "head" { adoptedHead = true } else { adoptedBody = true }
                adopt = lower
            }
        }
        let seq = nextSeq()
        guard let node = newNode(id, { Node(id: id, kind: .element, tag: outTag ?? "", svg: svg, rendered: outTag != nil, isStyle: isStyle, adopted: adopt != nil, seq: seq) }) else { return false }
        guard let rendered = outTag else { return true }
        var attrs = extra
        for (name, value) in attributes {
            guard let value else { continue }
            attrs += sanitizeAttribute(node, name, value)
        }
        out.append(WidgetOp(element: id, tag: rendered, svg: node.svg, attributes: attrs, adopt: adopt))
        return true
    }

    static let complexTextBudget = 8192
    static let complexChurnPerSecond = 1024.0
    private var complexText = 0
    private var totalText = 0
    private var churnTokens = Double(NativeValidator.complexTextBudget)
    private var churnAt: Double?
    var clock: () -> Double = { Date().timeIntervalSince1970 }

    static let smallText = 256

    static func chargesChurn(_ s: String) -> Bool {
        if s.utf16.count > smallText { return true }
        for c in s.utf16 {
            if (c >= 0x0590 && c < 0x2000) || (c >= 0x2e80 && c <= 0xd7ff) || (c >= 0xf900 && c <= 0xffef) { return true }
        }
        return false
    }

    static func textWork(_ s: String) -> Int {
        var count = 0
        var run = false
        var units = 0
        for scalar in s.unicodeScalars {
            let c = scalar.value
            units += c > 0xffff ? 2 : 1
            if (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7ff) || c >= 0xf900 {
                if run { count += 1 }
                run = false
                count += 1
            } else if c <= 0x20 || c == 0x2d || c == 0x3f || c == 0x2f || c == 0x200b {
                if run { count += 1 }
                run = false
            } else {
                run = true
            }
        }
        if run { count += 1 }
        return max(count, (units + 1023) / 1024)
    }

    private func takeChurn(_ words: Int) -> Bool {
        let now = clock()
        if let at = churnAt {
            churnTokens = min(Double(Self.complexTextBudget), churnTokens + max(0, now - at) * Self.complexChurnPerSecond)
        }
        churnAt = now
        guard churnTokens >= Double(words) else { return false }
        churnTokens -= Double(words)
        return true
    }

    private func overChurn() -> Bool {
        diag("text-churn", "text changes over \(Int(Self.complexChurnPerSecond)) units per second")
        return false
    }

    static func complexUnits(_ s: String) -> Int {
        var count = 0
        for unit in s.utf16 where unit >= 0x0590 { count += 1 }
        return count
    }

    private func overTextBudget() -> Bool {
        diag("text-budget", "document complex-script text over \(tables.complexTextBudget) units")
        return false
    }

    private func characterData(_ id: Int, _ data: String, text: Bool) -> Bool {
        guard nodes[id] == nil, nodes.count < tables.maxNodes else { return false }
        let length = data.utf16.count
        if tables.textBudget > 0, totalText + length > tables.textBudget {
            diag("text-budget", "document text over \(tables.textBudget) units")
            return false
        }
        let units = text ? Self.complexUnits(data) : 0
        if units > 0, tables.complexTextBudget > 0, complexText + units > tables.complexTextBudget { return overTextBudget() }
        if tables.textBudget > 0, Self.chargesChurn(data), !takeChurn(Self.textWork(data)) { return overChurn() }
        let seq = nextSeq()
        guard let node = newNode(id, { Node(id: id, kind: text ? .text : .comment, rendered: text, data: data, seq: seq) }) else { return false }
        node.complex = units
        complexText += units
        totalText += length
        if text { out.append(WidgetOp(text: id, data: data)) }
        return true
    }

    private func setData(_ id: Int, _ data: String) -> Bool {
        guard let node = nodes[id], node.kind != .element, node.kind != .document else { return false }
        let textDelta = data.utf16.count - node.data.utf16.count
        if textDelta > 0, tables.textBudget > 0, totalText + textDelta > tables.textBudget {
            diag("text-budget", "document text over \(tables.textBudget) units")
            return false
        }
        let units = node.kind == .text ? Self.complexUnits(data) : 0
        let delta = units - node.complex
        if delta > 0, tables.complexTextBudget > 0, complexText + delta > tables.complexTextBudget { return overTextBudget() }
        if tables.textBudget > 0, Self.chargesChurn(data), !takeChurn(Self.textWork(data)) { return overChurn() }
        complexText += delta
        totalText += textDelta
        node.complex = units
        node.data = data
        let parent = node.parent.flatMap { nodes[$0] }
        if let parent, parent.isStyle {
            markDirty(parent.id)
        } else if node.rendered {
            out.append(WidgetOp(data: id, data: data))
        }
        return true
    }

    private func insert(parent parentId: Int, id: Int, before: Int?) -> Bool {
        guard let node = nodes[id], let p = nodes[parentId], p.kind != .text, p.kind != .comment else { return false }
        var a: Node? = p
        while let current = a {
            if current === node { return false }
            a = current.parent.flatMap { nodes[$0] }
        }
        var ref: Node?
        if let before {
            guard let r = nodes[before], r.parent == p.id, r !== node else { return false }
            ref = r
        }
        unlink(node)
        node.parent = p.id
        if let ref, let index = p.children.firstIndex(of: ref.id) {
            p.children.insert(id, at: index)
        } else if ref != nil {
            p.children.insert(id, at: max(0, p.children.count - 1))
        } else {
            p.children.append(id)
        }
        if p.isStyle { markDirty(p.id) }
        let renderHere = node.rendered && !node.adopted && p.rendered && !p.isStyle && p.kind == .element
        if !renderHere {
            if p.isStyle && node.rendered && node.kind != .element {
                out.append(WidgetOp(remove: id))
                node.rendered = false
            } else if node.attached {
                out.append(WidgetOp(insert: id, parent: tables.limbo, before: nil))
            }
            node.attached = false
            return true
        }
        var refId: Int?
        if let index = p.children.firstIndex(of: id) {
            var k = index + 1
            while k < p.children.count {
                if let sibling = nodes[p.children[k]], sibling.attached {
                    refId = sibling.id
                    break
                }
                k += 1
            }
        }
        out.append(WidgetOp(insert: id, parent: p.id, before: refId))
        node.attached = true
        return true
    }

    private func unlink(_ node: Node) {
        guard let parentId = node.parent else { return }
        if let old = nodes[parentId] {
            if let i = old.children.firstIndex(of: node.id) { old.children.remove(at: i) }
            if old.isStyle { markDirty(old.id) }
        }
        node.parent = nil
    }

    private func remove(_ id: Int) -> Bool {
        guard let node = nodes[id] else { return false }
        if node.adopted {
            for c in node.children { _ = isId(c) ? remove(c) : false }
            return true
        }
        unlink(node)
        if node.rendered { out.append(WidgetOp(remove: id)) }
        var stack = [node]
        while let n = stack.popLast() {
            complexText -= n.complex
            totalText -= n.data.utf16.count
            nodes[n.id] = nil
            dirtyStyles[n.id] = nil
            for c in n.children {
                if let child = nodes[c] {
                    if child.rendered && !child.attached { out.append(WidgetOp(remove: child.id)) }
                    stack.append(child)
                }
            }
        }
        return true
    }

    private func attribute(_ id: Int, _ name: String, _ value: String) -> Bool {
        guard let node = nodes[id], node.kind == .element else { return false }
        if !node.rendered { return true }
        let pairs = sanitizeAttribute(node, name, value)
        if pairs.isEmpty {
            if let canon = canonicalAttributeName(node, name) { out.append(WidgetOp(removeAttribute: id, name: canon)) }
            return true
        }
        for (n, v) in pairs { out.append(WidgetOp(attribute: id, name: n, value: v)) }
        return true
    }

    private func removeAttribute(_ id: Int, _ name: String) -> Bool {
        guard let node = nodes[id], node.kind == .element else { return false }
        if !node.rendered { return true }
        guard let canon = canonicalAttributeName(node, name) else { return true }
        out.append(WidgetOp(removeAttribute: id, name: canon))
        if node.tag == "a" && (canon == "href" || canon == "xlink:href") { out.append(WidgetOp(removeAttribute: id, name: "data-tg-link")) }
        return true
    }

    private func canonicalAttributeName(_ node: Node, _ name: String) -> String? {
        guard Self.isAttrName(name) else { return nil }
        let lower = Self.asciiLower(name)
        if lower.hasPrefix("on") { return nil }
        if node.svg { return lower == "xlink:href" ? "href" : (tables.svgCase[lower] ?? lower) }
        return lower
    }

    private static func mentionsURL(_ value: String) -> Bool {
        var value = value
        return value.withUTF8 { b in
            guard b.count >= 3 else { return false }
            for i in 0...(b.count - 3) {
                let c0 = b[i] | 0x20, c1 = b[i + 1] | 0x20, c2 = b[i + 2] | 0x20
                if c0 == 0x73 && c1 == 0x72 && c2 == 0x63 { return true }
                if i + 3 < b.count, c0 == 0x68 && c1 == 0x72 && c2 == 0x65 && (b[i + 3] | 0x20) == 0x66 { return true }
            }
            return false
        }
    }

    private func sanitizeAttribute(_ node: Node, _ rawName: String, _ value: String) -> [(name: String, value: String)] {
        guard let name = canonicalAttributeName(node, rawName) else { return [] }
        let lower = Self.asciiLower(name)
        let tag = node.tag
        if lower == "style" { return [("style", oracle?.styleAttribute(value) ?? "")] }
        if tables.url.contains(lower) { return sanitizeURLAttribute(node, name, lower, value) }
        if lower.hasPrefix("data-tg-") { return [] }
        if lower.hasPrefix("data-") || lower.hasPrefix("aria-") { return [(lower, value)] }
        if tables.silent.contains(lower) { return [] }
        if !tables.attributes.contains(lower) {
            diag("attr-dropped", "\(tag)[\(lower)]")
            return []
        }
        if node.svg {
            if tables.animationTags.contains(tag) {
                if lower == "attributename" && Self.mentionsURL(value) { return [] }
                if tables.animationValues.contains(lower) {
                    var parts: [String] = []
                    for part in value.split(separator: ";", omittingEmptySubsequences: false) {
                        guard let clean = oracle?.cssValue(String(part), where: "\(tag)[\(lower)]") else { return [] }
                        parts.append(clean)
                    }
                    return [(name, parts.joined(separator: ";"))]
                }
            }
            if tables.css.contains(lower) {
                guard let clean = oracle?.cssValue(value, where: "\(tag)[\(lower)]") else { return [] }
                return [(name, clean)]
            }
        }
        return [(name, value)]
    }

    private func sanitizeURLAttribute(_ node: Node, _ name: String, _ lower: String, _ value: String) -> [(name: String, value: String)] {
        let tag = node.tag
        func block() -> [(name: String, value: String)] {
            diag("url-blocked", "\(tag)[\(lower)]=\(JSONText.truncated(value, 80))")
            return []
        }
        let isHref = lower == "href" || lower == "xlink:href"
        if tag == "a" && isHref {
            if URLPolicy.isFragment(value) {
                if let fragment = URLPolicy.check(value, image: false, mime: tables.mime) { return [(name, fragment)] }
                return [(name, "#")]
            }
            return [(name, "#"), ("data-tg-link", "")]
        }
        if node.svg {
            if !isHref { return block() }
            if tag == "image" {
                if let url = URLPolicy.check(value, image: true, mime: tables.mime), !url.hasPrefix("#") { return [(name, url)] }
                return block()
            }
            if tables.fragmentTags.contains(tag) {
                if let url = URLPolicy.check(value, image: tag == "feImage", mime: tables.mime) { return [(name, url)] }
                return block()
            }
            return block()
        }
        if lower == "src" {
            if tag == "img" || tag == "input" {
                if let url = URLPolicy.check(value, image: true, mime: tables.mime), !url.hasPrefix("#") { return [("src", url)] }
                _ = block()
                return tag == "img" ? [("src", tables.placeholder), ("data-tg-placeholder", "")] : []
            }
            return block()
        }
        if lower == "usemap" { return [] }
        return block()
    }

    private func style(_ id: Int, _ name: String, _ value: String, important: Bool) -> Bool {
        guard let node = nodes[id], node.kind == .element else { return false }
        if !node.rendered { return true }
        guard Self.isStyleName(name) else { return false }
        out.append(WidgetOp(style: id, name: name, value: styleValue(name, value), important: important))
        return true
    }

    private func styleValue(_ name: String, _ value: String) -> String {
        if value.isEmpty { return "" }
        var name = name
        if value.utf8.count <= ElementBatchFastPath.maxValueBytes, name.withUTF8({ ElementBatchFastPath.styleNames.contains($0[...]) }) {
            var bytes: [UInt8] = []
            var copy = value
            let ok = copy.withUTF8 { b -> Bool in
                CSSValueCanon.append(b[...], to: &bytes)
            }
            if ok { return String(decoding: bytes, as: UTF8.self) }
        }
        if let cached = styleCache[name]?[value] { return cached }
        let before = oracle?.diagnostics ?? 0
        let clean = oracle?.styleProperty(name, value) ?? ""
        if styleCacheSize < 4096, value.utf8.count < 512, oracle?.diagnostics == before {
            styleCache[name, default: [:]][value] = clean
            styleCacheSize += 1
        }
        return clean
    }

    private func prop(_ o: JSONObject) -> Bool {
        guard let id = isId(o["id"]), case .string(let name)? = o["name"] else { return false }
        guard let spec = tables.props[name], let node = nodes[id], node.kind == .element else { return false }
        let value = o["value"]
        let type: String
        let any: Any
        switch value {
        case .string(let s)?:
            type = "string"
            if s.utf8.count > tables.maxText, s.utf16.count > tables.maxText { return false }
            any = s
        case .number(let n)?:
            type = "number"
            any = NSNumber(value: n)
        case .bool(let b)?:
            type = "boolean"
            any = b
        default:
            return false
        }
        guard type == spec.type else { return false }
        if !node.rendered || (spec.tags.map { !$0.contains(node.tag) } ?? false) { return true }
        out.append(WidgetOp(["op": "prop", "id": id, "name": name, "value": any]))
        return true
    }

    private func call(_ o: JSONObject) -> Bool {
        guard let id = isId(o["id"]), case .string(let method)? = o["method"], let n = tables.methods[method] else { return false }
        guard let node = nodes[id], node.kind == .element else { return false }
        if !node.rendered { return true }
        var args: [Double] = []
        if n > 0 {
            guard case .array(let list)? = o["args"] else { return false }
            for k in 0..<n {
                guard k < list.count, case .number(let d) = list[k], d.isFinite else { return false }
                args.append(d)
            }
        }
        out.append(WidgetOp(["op": "call", "id": id, "method": method, "args": args]))
        return true
    }

    private func canvasWords(_ id: Int, _ batch: WidgetCanvasBatch) -> Bool {
        guard isId(id), let node = nodes[id], node.kind == .element, node.tag == "canvas" else { return false }
        if CanvasWords.envelope(batch) {
            out.append(WidgetOp(canvas: id, batch: batch))
            return true
        }
        guard let json = CanvasWords.commandsJSON(batch) else { return false }
        canvasJSON(id, json)
        return true
    }

    private func canvasJSON(_ id: Int, _ json: String) {
        guard let clean = oracle?.canvasCommands(json), clean != "[]", let cmds = (try? JSONSerialization.jsonObject(with: Data(clean.utf8))) as? [Any], !cmds.isEmpty else { return }
        out.append(WidgetOp(["op": "canvas", "id": id, "cmds": cmds]))
    }
}

enum URLPolicy {
    private static func isJSSpace(_ v: UInt32) -> Bool {
        switch v {
        case 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x20, 0xa0, 0x1680, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff: return true
        case 0x2000...0x200a: return true
        default: return false
        }
    }

    static func normalize(_ raw: String) -> String {
        var scalars = Array(raw.unicodeScalars.filter { $0 != "\t" && $0 != "\n" && $0 != "\r" })
        var start = 0
        while start < scalars.count, scalars[start].value <= 0x20 { start += 1 }
        var end = scalars.count
        while end > start, scalars[end - 1].value <= 0x20 { end -= 1 }
        if start == 0 && end == scalars.count && scalars.count == raw.unicodeScalars.count { return raw }
        scalars = Array(scalars[start..<end])
        var out = String.UnicodeScalarView()
        out.append(contentsOf: scalars)
        return String(out)
    }

    static func isFragment(_ raw: String) -> Bool {
        normalize(raw).hasPrefix("#")
    }

    private static func hasPrefixIgnoringASCIICase(_ s: String, _ prefix: String) -> Bool {
        let a = Array(s.utf8.prefix(prefix.utf8.count)), b = Array(prefix.utf8)
        guard a.count == b.count else { return false }
        for (x, y) in zip(a, b) where (x >= 0x41 && x <= 0x5a ? x | 0x20 : x) != y { return false }
        return true
    }

    static func check(_ raw: String, image: Bool, mime allowed: Set<String>) -> String? {
        let url = normalize(raw)
        if url.hasPrefix("#") {
            for scalar in url.unicodeScalars.dropFirst() {
                if isJSSpace(scalar.value) { return nil }
                switch scalar {
                case "\"", "'", "(", ")", "\\", "<", ">": return nil
                default: continue
                }
            }
            return url
        }
        if !image { return nil }
        if hasPrefixIgnoringASCIICase(url, "data:") {
            guard let mime = dataMime(url) else { return nil }
            return allowed.contains(mime) ? url : nil
        }
        return nil
    }

    static func dataMime(_ url: String) -> String? {
        let scalars = Array(url.unicodeScalars.dropFirst(5))
        var i = 0
        while i < scalars.count, scalars[i] != ",", scalars[i] != ";" { i += 1 }
        guard i < scalars.count else { return nil }
        let mimeEnd = i
        if scalars[i] == ";" {
            guard scalars[(i + 1)...].contains(",") else { return nil }
        }
        var start = 0, end = mimeEnd
        while start < end, isJSSpace(scalars[start].value) || isLineTerminator(scalars[start].value) || isZs(scalars[start]) { start += 1 }
        while end > start, isJSSpace(scalars[end - 1].value) || isLineTerminator(scalars[end - 1].value) || isZs(scalars[end - 1]) { end -= 1 }
        var view = String.UnicodeScalarView()
        view.append(contentsOf: scalars[start..<end])
        return String(view).lowercased()
    }

    private static func isLineTerminator(_ v: UInt32) -> Bool { v == 0x0a || v == 0x0d || v == 0x2028 || v == 0x2029 }
    private static func isZs(_ s: Unicode.Scalar) -> Bool { s.properties.generalCategory == .spaceSeparator }
}
