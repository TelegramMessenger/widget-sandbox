import Foundation
import QuartzCore

public final class WidgetNativeWatchdog {
    public static let shared = WidgetNativeWatchdog()
    private let lock = NSLock()
    private let timer: DispatchSourceTimer
    private let timeout: TimeInterval
    private let expired: () -> Void
    private var active: [UUID: TimeInterval] = [:]
    private var failed = false

    public init(timeout: TimeInterval = 8, expired: @escaping () -> Void = { _exit(EXIT_FAILURE) }) {
        self.timeout = timeout
        self.expired = expired
        timer = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "widget.native.watchdog", qos: .utility))
        timer.schedule(deadline: .now() + 0.05, repeating: 0.05, leeway: .milliseconds(10))
        timer.setEventHandler { [weak self] in self?.check() }
        timer.resume()
    }

    public func begin() -> () -> Void {
        let id = UUID()
        lock.lock()
        guard !failed else { lock.unlock(); return {} }
        let overloaded = active.count >= 128
        if !overloaded { active[id] = CACurrentMediaTime() + timeout }
        lock.unlock()
        if overloaded { fail() }
        return { [weak self] in
            guard let self else { return }
            self.lock.lock()
            self.active.removeValue(forKey: id)
            self.lock.unlock()
        }
    }

    private func check() {
        let now = CACurrentMediaTime()
        lock.lock()
        let timedOut = active.values.contains { $0 <= now }
        lock.unlock()
        if timedOut { fail() }
    }

    private func fail() {
        lock.lock()
        let first = !failed
        failed = true
        lock.unlock()
        if first { expired() }
    }

    deinit { timer.cancel() }
}
