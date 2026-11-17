import Foundation
import QuartzCore
import WidgetSandboxProtocol

final class RemoteCallbackBudget {
    static let maximumBytes = 64 << 20
    static let maximumCount = 1024
    static let maximumRate = 8192
    static let maximumByteRate = 128 << 20
    private static let lock = NSLock()
    private static var retainedBytes = 0
    private static var retainedCount = 0
    private static var rateWindow: TimeInterval = 0
    private static var rateCount = 0
    private static var rateBytes = 0

    static var usage: (bytes: Int, count: Int) {
        Self.lock.lock()
        defer { Self.lock.unlock() }
        return (retainedBytes, retainedCount)
    }
    private var bytes = 0
    private var count = 0
    private var window: TimeInterval = 0
    private var windowCount = 0
    private var windowBytes = 0
    private var tripped = false

    func take(_ size: Int, now: TimeInterval = CACurrentMediaTime()) -> (allowed: Bool, firstFailure: Bool) {
        Self.lock.lock()
        defer { Self.lock.unlock() }
        guard !tripped else { return (false, false) }
        if now - Self.rateWindow >= 1 {
            Self.rateWindow = now
            Self.rateCount = 0
            Self.rateBytes = 0
        }
        if now - window >= 1 {
            window = now
            windowCount = 0
            windowBytes = 0
        }
        guard size >= 0, size <= Self.maximumBytes - bytes, count < Self.maximumCount,
              windowCount < Self.maximumRate, size <= Self.maximumByteRate - windowBytes,
              size <= (256 << 20) - Self.retainedBytes, Self.retainedCount < 8192,
              Self.rateCount < 32768, size <= (256 << 20) - Self.rateBytes else {
            tripped = true
            return (false, true)
        }
        Self.retainedBytes += size
        Self.retainedCount += 1
        Self.rateCount += 1
        Self.rateBytes += size
        bytes += size
        count += 1
        windowBytes += size
        windowCount += 1
        return (true, false)
    }

    func give(_ size: Int) {
        Self.lock.lock()
        Self.retainedBytes -= size
        Self.retainedCount -= 1
        bytes -= size
        count -= 1
        Self.lock.unlock()
    }

    private static let contextKey = "WidgetSandbox.remoteCallbackLease"

    final class Lease {
        private let budget: RemoteCallbackBudget
        private let bytes: Int

        init(_ budget: RemoteCallbackBudget, bytes: Int) {
            self.budget = budget
            self.bytes = bytes
        }

        func perform<T>(_ block: () -> T) -> T {
            let context = Thread.current.threadDictionary
            let previous = context[RemoteCallbackBudget.contextKey]
            context[RemoteCallbackBudget.contextKey] = self
            defer { context[RemoteCallbackBudget.contextKey] = previous }
            return block()
        }

        deinit { budget.give(bytes) }
    }

    static func preserving(_ block: @escaping () -> Void) -> () -> Void {
        guard let lease = Thread.current.threadDictionary[contextKey] as? Lease else { return block }
        return { lease.perform(block) }
    }

    static func cost(_ sizes: Int...) -> Int {
        var total = 256
        for size in sizes {
            guard size >= 0, size <= (maximumBytes - total) / 2 else { return maximumBytes + 1 }
            total += size * 2
        }
        return total
    }

    static func canvasCost(_ canvas: [Int: WidgetCanvasBatch]) -> Int {
        guard canvas.count <= 64 else { return maximumBytes + 1 }
        var total = 0
        for batch in canvas.values {
            guard batch.words.count <= maximumBytes / 8, batch.strings.count <= 65536 else { return maximumBytes + 1 }
            let cost = batch.words.count * 8 + batch.strings.count * 32 + 256
            guard cost <= maximumBytes - total else { return maximumBytes + 1 }
            total += cost
            for string in batch.strings {
                let bytes = string.utf8.count
                guard bytes <= maximumBytes - total else { return maximumBytes + 1 }
                total += bytes
            }
        }
        return total
    }
}
