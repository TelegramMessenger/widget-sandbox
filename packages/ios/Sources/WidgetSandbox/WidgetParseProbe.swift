#if DEBUG
import Foundation

public enum WidgetParseProbe {
    public enum Role: String {
        case host
        case service
    }

    private static let roleKey = "widget.parse.probe.role"
    private static let lock = NSLock()
    private static var observer: ((String, Role) -> Void)?

    public static func observe(_ body: ((String, Role) -> Void)?) {
        lock.lock()
        observer = body
        lock.unlock()
    }

    public static func markServiceThread() {
        Thread.current.threadDictionary[roleKey] = Role.service.rawValue
    }

    static func hit(_ site: String) {
        lock.lock()
        let current = observer
        lock.unlock()
        guard let current else { return }
        current(site, role)
    }

    private static var role: Role {
        if Thread.current.threadDictionary[roleKey] as? String == Role.service.rawValue { return .service }
        if Thread.callStackSymbols.contains(where: { $0.contains("WidgetEngineServer") || $0.contains("SandboxRemoteServer") }) { return .service }
        return .host
    }
}
#endif
