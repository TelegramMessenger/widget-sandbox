import Foundation
import QuartzCore

/// Main-thread CPU time spent on behalf of one widget (its renderer, engine and validator output). Every block a widget
/// runs on the main thread goes through `measure`, which charges the main thread's own CPU time for the block (so a
/// system stall or a modal menu tracked inside it costs nothing) plus `blockOverhead`. Time is kept in 100 ms slots.
///
/// Per widget, keyed by `key` (the widget's identity, so a restart or a scroll rebuild keeps its history):
/// - The first `grace` seconds of a key's first meter are its open: the 40 ms/s rule does not apply, but the open may
///   spend at most `graceBudget`. A key gets one grace per `graceMemory`; a meter of a key that tripped gets none.
/// - After the first block, one block over `maxBlock` trips it.
/// - Past the grace it trips over 40 ms in the last 1 s and 80 ms in the last 2 s.
/// All widgets together, graced ones included, trip the heaviest past 120 ms in 1 s and 240 ms in 2 s when more than
/// one widget is spending (one widget alone answers to its own rules). Above all of
/// them, the service breaker: all widget time over `serviceLimit` of the main thread across `serviceWindow` calls
/// `onServiceTrip` (the client ends every widget connection and backs off). A tripped meter calls `onTrip` once, on the
/// main thread, after the block.
public final class MainTimeMeter {
    public static let connectionLimit: CFTimeInterval = 0.040
    public static let globalLimit: CFTimeInterval = 0.120
    public static let slot: CFTimeInterval = 0.1
    public static let slots = 20
    public static let grace: CFTimeInterval = 2
    public static let graceBudget: CFTimeInterval = 0.3
    public static let graceMemory: CFTimeInterval = 600
    public static let maxBlock: CFTimeInterval = 0.1
    public static let blockOverhead: CFTimeInterval = 0.00002
    public static let serviceWindow: CFTimeInterval = 10
    public static let serviceLimit: Double = 0.15

    /// Called on the main thread when all widgets together pass the service breaker.
    public static var onServiceTrip: (() -> Void)?

    private static var meters: [WeakMeter] = []
    private static var keys: [String: (grace: CFTimeInterval, tripped: CFTimeInterval)] = [:]
    private static var serviceSlots = [CFTimeInterval](repeating: 0, count: 100)
    private static var serviceSlotIndex = [Int](repeating: Int.min, count: 100)

    private final class WeakMeter {
        weak var meter: MainTimeMeter?
        init(_ meter: MainTimeMeter) { self.meter = meter }
    }

    public let label: String
    public let key: String?
    public var onTrip: (() -> Void)?
    public private(set) var tripped = false
    private var buckets = [CFTimeInterval](repeating: 0, count: MainTimeMeter.slots)
    private var bucketSlot = [Int](repeating: Int.min, count: MainTimeMeter.slots)
    private var registered = false
    private var graceEnds: CFTimeInterval?
    private var graceSpent: CFTimeInterval = 0
    private var blocks = 0

    public init(label: String, key: String? = nil) {
        self.label = label
        self.key = key
    }

    public static func threadCPUTime() -> CFTimeInterval {
        return CFTimeInterval(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)) / 1_000_000_000
    }

    /// Runs `body` on the main thread now and charges its main-thread CPU time to this widget.
    public func measure(_ body: () -> Void) {
        let started = MainTimeMeter.threadCPUTime()
        body()
        record(MainTimeMeter.threadCPUTime() - started, now: CACurrentMediaTime())
    }

    public func inGrace(now: CFTimeInterval) -> Bool {
        guard let graceEnds = graceEnds else { return true }
        return now < graceEnds
    }

    private func startGrace(now: CFTimeInterval, seconds: CFTimeInterval) {
        let start = now - seconds
        if let key = key, let known = MainTimeMeter.keys[key], start - known.tripped < MainTimeMeter.graceMemory || start - known.grace < MainTimeMeter.graceMemory {
            graceEnds = start
            return
        }
        graceEnds = start + MainTimeMeter.grace
        if let key = key {
            MainTimeMeter.keys[key] = (start, MainTimeMeter.keys[key]?.tripped ?? -.infinity)
            if MainTimeMeter.keys.count > 4096 {
                MainTimeMeter.keys = MainTimeMeter.keys.filter { start - $0.value.grace < MainTimeMeter.graceMemory || start - $0.value.tripped < MainTimeMeter.graceMemory }
            }
        }
    }

    public func record(_ seconds: CFTimeInterval, now: CFTimeInterval) {
        if !registered {
            registered = true
            MainTimeMeter.meters.removeAll { $0.meter == nil }
            MainTimeMeter.meters.append(WeakMeter(self))
        }
        let spent = max(0, seconds)
        if graceEnds == nil {
            startGrace(now: now, seconds: spent)
        }
        let charged = spent + MainTimeMeter.blockOverhead
        let index = Int((now / MainTimeMeter.slot).rounded(.down))
        let position = ((index % MainTimeMeter.slots) + MainTimeMeter.slots) % MainTimeMeter.slots
        if bucketSlot[position] != index {
            bucketSlot[position] = index
            buckets[position] = 0
        }
        buckets[position] += charged
        blocks += 1
        MainTimeMeter.chargeService(charged, now: now)
        guard !tripped else { return }
        let graced = inGrace(now: now)
        if graced {
            graceSpent += charged
        }
        if (blocks > 1 && spent > MainTimeMeter.maxBlock) || (graced && graceSpent > MainTimeMeter.graceBudget) || (!graced && over(now: now)) {
            trip(now: now)
            return
        }
        MainTimeMeter.checkGlobal(now: now)
    }

    /// Main time charged in the last `seconds` (at most 2 s) and the number of slots with any.
    public func usage(over seconds: CFTimeInterval, now: CFTimeInterval) -> (time: CFTimeInterval, slots: Int) {
        let current = Int((now / MainTimeMeter.slot).rounded(.down))
        let span = min(MainTimeMeter.slots, Int((seconds / MainTimeMeter.slot).rounded()))
        var time: CFTimeInterval = 0
        var used = 0
        for position in 0..<MainTimeMeter.slots where bucketSlot[position] > current - span && bucketSlot[position] <= current {
            time += buckets[position]
            if buckets[position] > 0 { used += 1 }
        }
        return (time, used)
    }

    private func over(now: CFTimeInterval) -> Bool {
        return usage(over: 1, now: now).time > MainTimeMeter.connectionLimit && usage(over: 2, now: now).time > MainTimeMeter.connectionLimit * 2
    }

    private func trip(now: CFTimeInterval) {
        guard !tripped else { return }
        tripped = true
        if let key = key {
            MainTimeMeter.keys[key] = (MainTimeMeter.keys[key]?.grace ?? -.infinity, now)
        }
        let action = onTrip
        onTrip = nil
        DispatchQueue.main.async { action?() }
    }

    /// Trips every live meter (the service breaker's end of all widget connections).
    public static func tripAll() {
        let now = CACurrentMediaTime()
        meters.removeAll { $0.meter == nil }
        for weak in meters {
            weak.meter?.trip(now: now)
        }
    }

    /// Widget main time across `serviceWindow`.
    public static func serviceUsage(now: CFTimeInterval) -> CFTimeInterval {
        let current = Int((now / slot).rounded(.down))
        var total: CFTimeInterval = 0
        for position in 0..<serviceSlots.count where serviceSlotIndex[position] > current - serviceSlots.count && serviceSlotIndex[position] <= current {
            total += serviceSlots[position]
        }
        return total
    }

    private static var serviceTripped: CFTimeInterval = -.infinity

    private static func chargeService(_ seconds: CFTimeInterval, now: CFTimeInterval) {
        let index = Int((now / slot).rounded(.down))
        let position = ((index % serviceSlots.count) + serviceSlots.count) % serviceSlots.count
        if serviceSlotIndex[position] != index {
            serviceSlotIndex[position] = index
            serviceSlots[position] = 0
        }
        serviceSlots[position] += seconds
        guard serviceUsage(now: now) > serviceWindow * serviceLimit, abs(now - serviceTripped) > serviceWindow else { return }
        serviceTripped = now
        for position in 0..<serviceSlots.count {
            serviceSlots[position] = 0
        }
        let action = onServiceTrip
        DispatchQueue.main.async {
            tripAll()
            action?()
        }
    }

    private static func checkGlobal(now: CFTimeInterval) {
        meters.removeAll { $0.meter == nil }
        var one: CFTimeInterval = 0
        var two: CFTimeInterval = 0
        var heaviest: (meter: MainTimeMeter, time: CFTimeInterval)?
        var contributors = 0
        for weak in meters {
            guard let meter = weak.meter, !meter.tripped else { continue }
            let last = meter.usage(over: 2, now: now).time
            if last > 0 { contributors += 1 }
            one += meter.usage(over: 1, now: now).time
            two += last
            if last > (heaviest?.time ?? 0) {
                heaviest = (meter, last)
            }
        }
        guard contributors > 1, one > globalLimit, two > globalLimit * 2, let heaviest = heaviest else { return }
        heaviest.meter.trip(now: now)
    }
}

/// A renderer whose main-thread work the host charges to the host's own meter.
public protocol MainTimedRenderer: AnyObject {
    var mainMeter: MainTimeMeter? { get set }
    func mainTimeTripped(_ reason: String)
}
