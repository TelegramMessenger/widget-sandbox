import Foundation
import QuartzCore

/// Main-thread time spent on behalf of one widget connection (its renderer, engine and validator output). Every block
/// a widget runs on the main thread goes through `measure`, which records only that block's own duration on the
/// monotonic clock. Time is kept in 100 ms slots over a 2 s sliding window; a slot counts at most `slotCap`.
///
/// A connection trips when, sustained over the window, it takes more than `connectionLimit` of the main thread: over
/// 80 ms in the last 2 s, over 40 ms in the last 1 s, and spread over at least `sustainedSlots` of the 20 slots, so one
/// heavy first layout or open never trips it. All connections together trip the heaviest one past `globalLimit` (120 ms
/// in the last 1 s, 240 ms in the last 2 s). A tripped meter calls `onTrip` once, on the main thread, after the block.
public final class MainTimeMeter {
    public static let connectionLimit: CFTimeInterval = 0.040
    public static let globalLimit: CFTimeInterval = 0.120
    public static let slot: CFTimeInterval = 0.1
    public static let slots = 20
    public static let sustainedSlots = 8
    /// What one slot can add to a window: twice the limit's share of a slot, so one long block (a first layout, a
    /// heavy open) counts as 8 ms however long it ran, and only work spread over many slots adds up to a trip.
    public static let slotCap: CFTimeInterval = 0.008

    private static var meters: [WeakMeter] = []

    private final class WeakMeter {
        weak var meter: MainTimeMeter?
        init(_ meter: MainTimeMeter) { self.meter = meter }
    }

    public let label: String
    public var onTrip: (() -> Void)?
    public private(set) var tripped = false
    private var buckets = [CFTimeInterval](repeating: 0, count: MainTimeMeter.slots)
    private var bucketSlot = [Int](repeating: Int.min, count: MainTimeMeter.slots)
    private var registered = false

    public init(label: String) {
        self.label = label
    }

    /// Runs `body` on the main thread now and charges its own duration to this connection.
    public func measure(_ body: () -> Void) {
        let started = CACurrentMediaTime()
        body()
        record(CACurrentMediaTime() - started, now: CACurrentMediaTime())
    }

    public func record(_ seconds: CFTimeInterval, now: CFTimeInterval) {
        if !registered {
            registered = true
            MainTimeMeter.meters.removeAll { $0.meter == nil }
            MainTimeMeter.meters.append(WeakMeter(self))
        }
        let index = Int((now / MainTimeMeter.slot).rounded(.down))
        let position = ((index % MainTimeMeter.slots) + MainTimeMeter.slots) % MainTimeMeter.slots
        if bucketSlot[position] != index {
            bucketSlot[position] = index
            buckets[position] = 0
        }
        buckets[position] += max(0, seconds)
        guard !tripped else { return }
        if sustained(now: now) {
            trip()
            return
        }
        MainTimeMeter.checkGlobal(now: now)
    }

    /// Main time in the last `seconds` (at most 2 s) and the number of slots with any.
    public func usage(over seconds: CFTimeInterval, now: CFTimeInterval) -> (time: CFTimeInterval, slots: Int) {
        let current = Int((now / MainTimeMeter.slot).rounded(.down))
        let span = min(MainTimeMeter.slots, Int((seconds / MainTimeMeter.slot).rounded()))
        var time: CFTimeInterval = 0
        var used = 0
        for position in 0..<MainTimeMeter.slots where bucketSlot[position] > current - span && bucketSlot[position] <= current {
            time += min(buckets[position], MainTimeMeter.slotCap)
            if buckets[position] > 0 { used += 1 }
        }
        return (time, used)
    }

    private func sustained(now: CFTimeInterval) -> Bool {
        let two = usage(over: 2, now: now)
        let one = usage(over: 1, now: now)
        return two.time > MainTimeMeter.connectionLimit * 2 && one.time > MainTimeMeter.connectionLimit && two.slots >= MainTimeMeter.sustainedSlots
    }

    private func trip() {
        guard !tripped else { return }
        tripped = true
        let action = onTrip
        onTrip = nil
        DispatchQueue.main.async { action?() }
    }

    private static func checkGlobal(now: CFTimeInterval) {
        meters.removeAll { $0.meter == nil }
        var one: CFTimeInterval = 0
        var two: CFTimeInterval = 0
        var heaviest: (meter: MainTimeMeter, time: CFTimeInterval)?
        for weak in meters {
            guard let meter = weak.meter, !meter.tripped else { continue }
            let last = meter.usage(over: 1, now: now)
            one += last.time
            two += meter.usage(over: 2, now: now).time
            if last.time > (heaviest?.time ?? 0) {
                heaviest = (meter, last.time)
            }
        }
        guard one > globalLimit, two > globalLimit * 2, let heaviest = heaviest, heaviest.meter.usage(over: 2, now: now).slots >= sustainedSlots else { return }
        heaviest.meter.trip()
    }
}

/// A renderer whose main-thread work the host charges to the host's own meter.
public protocol MainTimedRenderer: AnyObject {
    var mainMeter: MainTimeMeter? { get set }
    func mainTimeTripped(_ reason: String)
}
