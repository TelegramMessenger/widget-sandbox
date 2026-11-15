import Foundation
import QuartzCore
import WidgetSandboxProtocol

public struct Widget3DBytes {
    public enum Kind {
        case float32
        case uint16
        case uint32
        case other
    }

    public let borrowedPointer: UnsafeRawPointer
    public let length: Int
    public let kind: Kind

    public init(borrowedPointer: UnsafeRawPointer, length: Int, kind: Kind) {
        self.borrowedPointer = borrowedPointer
        self.length = length
        self.kind = kind
    }

    public func copy() -> Data {
        Data(bytes: borrowedPointer, count: length)
    }
}

public enum Widget3DArgument {
    case number(Double)
    case string(String)
    case bytes(Widget3DBytes)
    case none

    public var number: Double? {
        if case let .number(value) = self, value.isFinite {
            return value
        }
        return nil
    }

    public var int: Int {
        guard let value = number else {
            return 0
        }
        return Int(max(-1e9, min(1e9, value)))
    }

    public var bytes: Widget3DBytes? {
        if case let .bytes(value) = self {
            return value
        }
        return nil
    }
}

public protocol Widget3DSurface: AnyObject {
    var onDiag: ((String, String) -> Void)? { get set }
    func call(_ op: String, _ args: [Widget3DArgument]) -> HostReturn
    func makeLayer() -> CALayer
    func layout(_ layer: CALayer, size: CGSize, scale: CGFloat)
    func setPaused(_ paused: Bool)
    func stop(_ reason: String)
}

public protocol Widget3DBackend: AnyObject {
    var isAvailable: Bool { get }
    var adapterSource: String? { get }
    func makeSurface() -> Widget3DSurface
}

public final class Widget3DChannel: WidgetSideChannel {
    public static let defaultMaxSurfaces = 4
    public static let maxArguments = 16
    public static let defaultMaxBytesPerCall = 64 << 20
    public let maxSurfaces: Int
    public let maxBytesPerCall: Int
    private let callLock = NSRecursiveLock()
    public static let maxToken = 1_000_000

    public var onDiag: ((String, String) -> Void)?
    private let backend: Widget3DBackend
    private let lock = NSLock()
    private var surfaces: [Int: Widget3DSurface] = [:]
    private var layers: [Int: CALayer] = [:]
    private var visible = true
    private var destroyed = false
    private weak var renderer: SurfaceHostingRenderer?

    public init(backend: Widget3DBackend, maxSurfaces: Int = Widget3DChannel.defaultMaxSurfaces, maxBytesPerCall: Int = Widget3DChannel.defaultMaxBytesPerCall) {
        self.backend = backend
        self.maxSurfaces = max(0, min(maxSurfaces, Widget3DChannel.defaultMaxSurfaces * 4))
        self.maxBytesPerCall = max(0, min(maxBytesPerCall, Widget3DChannel.defaultMaxBytesPerCall))
    }

    public var isDestroyed: Bool {
        lock.lock()
        defer { lock.unlock() }
        return destroyed
    }

    public var surfaceCount: Int {
        lock.lock()
        defer { lock.unlock() }
        return surfaces.count
    }

    public func surface(_ token: Int) -> Widget3DSurface? {
        lock.lock()
        defer { lock.unlock() }
        return surfaces[token]
    }

    public func install(in js: QuickJSContext) {
        #if !(DEBUG || TWIDGET_EXPERIMENTAL_3D)
        // 3D is not part of release builds (docs/decisions.md, "3D side channel"): `__tw3d` hands
        // raw buffers to the backend past the validator. Debug builds keep it for the twidget lab.
        onDiag?("3d", "not available in release builds")
        return
        #else
        guard backend.isAvailable, let adapter = backend.adapterSource else {
            return
        }
        // `__tw3d` hands typed arrays over in place, which only the native engine does (WidgetSandboxQuickJS); the
        // boxed engine lets no engine pointer out and Nova copies.
        lock.lock()
        if destroyed {
            lock.unlock()
            return
        }
        lock.unlock()
        let installed = js.registerSideChannelFunction("__tw3d") { [weak self] args in
            guard let self, !self.isDestroyed else {
                return .undefined
            }
            return self.call(args)
        }
        guard installed else {
            onDiag?("3d", js.backend == .nova ? "not available on Nova" : "not available on the boxed engine")
            return
        }
        do {
            _ = try js.evaluate("try { (\(adapter))(globalThis.__tw3d); } finally { delete globalThis.__tw3d; }", filename: "twidget3d.js")
        } catch {
            onDiag?("3d", "adapter failed: \(error)")
        }
        let hidden = (try? js.evaluate("delete globalThis.__tw3d; typeof globalThis.__tw3d === 'undefined' ? 'hidden' : 'visible'", filename: "twidget3d-hide.js"))?.string == "hidden"
        if !hidden {
            onDiag?("3d", "host function could not be hidden")
            destroy()
        }
        #endif
    }

    func call(_ args: [Widget3DArgument]) -> HostReturn {
        guard case let .string(op)? = args.first else {
            return .undefined
        }
        let token = args.count > 1 ? args[1].int : 0
        guard token > 0, token <= Self.maxToken else {
            return .undefined
        }
        let total = args.reduce(0) { $0 + ($1.bytes?.length ?? 0) }
        guard total <= maxBytesPerCall else {
            onDiag?("3d-rejected", "\(op): \(total) bytes in one call")
            return .undefined
        }
        if op == "bind" {
            return .number(bind(token) ? 1 : 0)
        }
        callLock.lock()
        defer { callLock.unlock() }
        guard !isDestroyed, let surface = surface(token) else {
            return .undefined
        }
        return surface.call(op, args)
    }

    private func bind(_ token: Int) -> Bool {
        guard backend.isAvailable else {
            return false
        }
        lock.lock()
        defer { lock.unlock() }
        if destroyed {
            return false
        }
        if surfaces[token] != nil {
            return true
        }
        if surfaces.count >= maxSurfaces {
            return false
        }
        let surface = backend.makeSurface()
        surface.setPaused(!visible)
        surface.onDiag = { [weak self] kind, detail in
            self?.onDiag?(kind, detail)
        }
        surfaces[token] = surface
        return true
    }

    public func connect(_ renderer: Renderer) {
        guard let renderer = renderer as? SurfaceHostingRenderer else {
            return
        }
        lock.lock()
        if destroyed {
            lock.unlock()
            return
        }
        self.renderer = renderer
        lock.unlock()
        renderer.surfaceLayout = { [weak self] token, size, scale, visible in
            self?.layout(token: token, size: size, scale: scale, visible: visible)
        }
        renderer.surfaceLayer = { [weak self] token in
            self?.layer(token: token)
        }
    }

    private static func token(_ text: String) -> Int? {
        guard text.utf8.count <= 7, let id = Int(text), id > 0, id <= maxToken, String(id) == text else {
            return nil
        }
        return id
    }

    private func layer(token: String) -> CALayer? {
        guard let id = Self.token(token), let surface = surface(id) else {
            return nil
        }
        lock.lock()
        defer { lock.unlock() }
        if destroyed {
            return nil
        }
        if let layer = layers[id] {
            return layer
        }
        let layer = surface.makeLayer()
        layers[id] = layer
        return layer
    }

    private func layout(token: String, size: CGSize, scale: CGFloat, visible: Bool) {
        guard let id = Self.token(token), let surface = surface(id) else {
            return
        }
        lock.lock()
        let layer = layers[id]
        let shown = visible && self.visible && !destroyed
        lock.unlock()
        guard let layer, shown, size.width >= 1, size.height >= 1, size.width.isFinite, size.height.isFinite, scale.isFinite, scale > 0 else {
            surface.setPaused(true)
            return
        }
        surface.layout(layer, size: size, scale: scale)
        surface.setPaused(false)
    }

    public func setVisible(_ value: Bool) {
        lock.lock()
        visible = value
        let all = Array(surfaces.values)
        lock.unlock()
        if !value {
            all.forEach { $0.setPaused(true) }
        }
    }

    public func destroy() {
        lock.lock()
        if destroyed {
            lock.unlock()
            return
        }
        destroyed = true
        let all = Array(surfaces.values)
        surfaces.removeAll()
        let removed = Array(layers.values)
        layers.removeAll()
        let renderer = self.renderer
        self.renderer = nil
        lock.unlock()
        callLock.lock()
        defer { callLock.unlock() }
        for surface in all {
            surface.onDiag = nil
            surface.stop("destroyed")
        }
        let detach = {
            if let renderer {
                renderer.surfaceLayer = nil
                renderer.surfaceLayout = nil
            }
            removed.forEach { $0.removeFromSuperlayer() }
        }
        if let scheduled = renderer as? ScheduledRenderer {
            if scheduled.isOnExecutor() { detach() } else { scheduled.schedule(detach) }
        } else if Thread.isMainThread {
            detach()
        } else {
            DispatchQueue.main.async(execute: detach)
        }
    }
}
