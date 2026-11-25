// Time for the control panel. The engine has no clock or timer of its own:
// the model asks it when it next needs waking and books one wake-up here.
// The app uses the main queue; the test and the picture maker move time by hand.
import Foundation

protocol PanelScheduler: AnyObject {
    /// Microseconds on a clock that never goes backwards.
    var now: UInt64 { get }
    /// Runs the work on the next turn (never inside the caller).
    func soon(_ work: @escaping () -> Void)
    /// Runs the work once at the given time. Returns a number to cancel it with.
    func schedule(at time: UInt64, _ work: @escaping () -> Void) -> Int
    func cancel(_ id: Int)
}

final class LiveScheduler: PanelScheduler {
    private var items: [Int: DispatchWorkItem] = [:]
    private var nextID = 1

    var now: UInt64 { DispatchTime.now().uptimeNanoseconds / 1000 }

    func soon(_ work: @escaping () -> Void) { DispatchQueue.main.async(execute: work) }

    func schedule(at time: UInt64, _ work: @escaping () -> Void) -> Int {
        let id = nextID
        nextID += 1
        let item = DispatchWorkItem { [weak self] in
            self?.items[id] = nil
            work()
        }
        items[id] = item
        let current = now
        let wait = time > current ? time - current : 0
        DispatchQueue.main.asyncAfter(deadline: .now() + .microseconds(Int(min(wait, 86_400_000_000))), execute: item)
        return id
    }

    func cancel(_ id: Int) {
        items[id]?.cancel()
        items[id] = nil
    }
}

/// Time moved by hand: exact and repeatable.
final class ManualScheduler: PanelScheduler {
    private(set) var now: UInt64 = 1_000_000
    private var soonQueue: [() -> Void] = []
    private var timed: [Int: (at: UInt64, work: () -> Void)] = [:]
    private var nextID = 1
    /// How many timed wake-ups have run, for the "idle costs nothing" check.
    private(set) var wakeUps = 0

    func soon(_ work: @escaping () -> Void) { soonQueue.append(work) }

    func schedule(at time: UInt64, _ work: @escaping () -> Void) -> Int {
        let id = nextID
        nextID += 1
        timed[id] = (max(time, now), work)
        return id
    }

    func cancel(_ id: Int) { timed[id] = nil }

    var pendingWakeUps: Int { timed.count }

    /// Runs everything due up to `micros` from now. Returns false if it had to give up
    /// (something kept rescheduling itself without time moving on).
    @discardableResult
    func advance(_ micros: UInt64) -> Bool {
        let target = now + micros
        var steps = 0
        while true {
            steps += 1
            if steps > 200_000 { now = target; return false }
            if !soonQueue.isEmpty {
                let work = soonQueue.removeFirst()
                work()
                continue
            }
            guard let next = timed.min(by: { $0.value.at < $1.value.at || ($0.value.at == $1.value.at && $0.key < $1.key) }),
                  next.value.at <= target else { break }
            timed[next.key] = nil
            now = max(now, next.value.at)
            wakeUps += 1
            next.value.work()
        }
        now = target
        return true
    }
}
