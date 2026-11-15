import Foundation
import QuartzCore

/// Main-thread time spent on behalf of one widget connection (its renderer, engine and validator output). Every block
/// a widget runs on the main thread goes through `measure`, which records the block's own duration on the monotonic
/// clock plus `blockOverhead` (the dispatch and bookkeeping around even an empty block). Time is kept in 100 ms slots
/// over a 2 s sliding window.
///
/// The first `grace` seconds after a widget's first block are its open (first layout, first frames) and count for
/// nothing. After that a widget trips when it has taken more than 40 ms of the main thread in the last 1 s and more than
/// 80 ms in the last 2 s, however that time is packed, or when one block runs longer than `maxBlock`. All widgets
/// together trip the heaviest one past 120 ms in 1 s and 240 ms in 2 s. A tripped meter calls `onTrip` once, on the
/// main thread, after the block.
public final class MainTimeMeter {
    public static let connectionLimit: CFTimeInterval = 0.040
    public static let globalLimit: CFTimeInterval = 0.120
    public static let slot: CFTimeInterval = 0.1
    public static let slots = 20
    public static let grace: CFTimeInterval = 2
    public static let maxBlock: CFTimeInterval = 0.1
    public static let blockOverhead: CFTimeInterval = 0.00002

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
    private var graceEnds: CFTimeInterval?

    public init(label: String) {
        self.label = label
    }

    /// Runs `body` on the main thread now and charges its own duration to this connection.
    public func measure(_ body: () -> Void) {
        let started = CACurrentMediaTime()
        body()
        let ended = CACurrentMediaTime()
        record(ended - started, now: ended)
    }

    public func inGrace(now: CFTimeInterval) -> Bool {
        guard let graceEnds = graceEnds else { return true }
        return now < graceEnds
    }

    public func record(_ seconds: CFTimeInterval, now: CFTimeInterval) {
        if !registered {
            registered = true
            MainTimeMeter.meters.removeAll { $0.meter == nil }
            MainTimeMeter.meters.append(WeakMeter(self))
        }
        if graceEnds == nil {
            graceEnds = now - max(0, seconds) + MainTimeMeter.grace
        }
        guard !inGrace(now: now) else { return }
        let charged = max(0, seconds) + MainTimeMeter.blockOverhead
        let index = Int((now / MainTimeMeter.slot).rounded(.down))
        let position = ((index % MainTimeMeter.slots) + MainTimeMeter.slots) % MainTimeMeter.slots
        if bucketSlot[position] != index {
            bucketSlot[position] = index
            buckets[position] = 0
        }
        buckets[position] += charged
        guard !tripped else { return }
        if seconds > MainTimeMeter.maxBlock || over(now: now) {
            trip()
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
            let last = meter.usage(over: 2, now: now).time
            one += meter.usage(over: 1, now: now).time
            two += last
            if last > (heaviest?.time ?? 0) {
                heaviest = (meter, last)
            }
        }
        guard one > globalLimit, two > globalLimit * 2, let heaviest = heaviest else { return }
        heaviest.meter.trip()
    }
}

/// A renderer whose main-thread work the host charges to the host's own meter.
public protocol MainTimedRenderer: AnyObject {
    var mainMeter: MainTimeMeter? { get set }
    func mainTimeTripped(_ reason: String)
}
