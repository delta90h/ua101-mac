// Level meters. Levels are not a setting of the unit: nothing is asked of it
// for them. They are worked out from the audio itself, by the driver's helper,
// which streams it anyway (HelperTransport in UnitTransport.swift feeds them
// here). So they light only with UA-101 chosen and while some app plays or
// records through it; otherwise there is no source and every meter stays dark.
//
// For judging the look there are pretend levels: a still set for the pictures
// and a moving set (Device menu), which is the only thing in this file that
// runs a timer, and only while it is switched on.
//
// The MIDI IN and OUT lamps need no audio: the helper says when MIDI passes the
// unit's MIDI sockets (link version 15, at most 10 times a second and only while
// MIDI passes), and each lamp then stays lit for a moment. One one-shot wake-up
// per lit lamp turns it off; while MIDI is quiet nothing is booked.
import Foundation
import SwiftUI

/// What one meter shows, as whole segments. Each meter's lights watch their own cell, and a
/// cell says "changed" only when a light changes, so a moving level repaints that meter and
/// nothing else. (Measured 2026-11-11: with every meter watching one model that published
/// each reading, SwiftUI laid the whole mixer out again 20 times a second, 95% of a core.)
final class MeterCell: ObservableObject {
    @Published fileprivate(set) var lit = 0
    @Published fileprivate(set) var peak = 0
    /// Whole dBFS for a read-out, kept only where one is drawn; nil: no signal.
    @Published fileprivate(set) var readout: Int?
    fileprivate var wantsReadout = false
}

/// One small light that goes on and off by itself (MIDI data passing).
final class LampCell: ObservableObject {
    @Published fileprivate(set) var lit = false
}

final class MeterModel: ObservableObject {
    /// The bottom of the meter's scale, in dBFS. The top is 0.
    static let floorDB: Float = -60
    static let demoHz = 20.0
    /// How fast a meter falls back and how long its peak mark stays.
    static let fallDBPerSecond: Float = 26, peakHoldSeconds: Float = 1.2

    /// dBFS for [group][channel], in the engine's group numbering; at or below the floor is dark.
    /// Not published: the views watch `cells`.
    private(set) var levels: [[Float]]
    private(set) var peaks: [[Float]]
    /// What each meter shows, [group][channel].
    let cells: [[MeterCell]]
    /// What the levels come from, for the tool-tip. nil: nothing, the meters are dark.
    @Published private(set) var source: String?

    /// MIDI data passing the unit's two connectors just now: [IN, OUT]. The helper says
    /// when (`blinkMIDI`); without it both stay dark.
    private(set) var midiActive = [false, false]
    let midiLamps = [LampCell(), LampCell()]
    /// How long a lamp stays lit after the helper said MIDI passed: 0.15 s (a design choice,
    /// 2026-11-17). The helper says it at most every 0.1 s, so steady MIDI is a steady light.
    static let midiLitMicros: UInt64 = 150_000
    /// When each lamp goes dark, and its one booked wake-up (nil: none, the lamp is dark).
    private var midiOffAt: [UInt64] = [0, 0]
    private var midiOffID: [Int?] = [nil, nil]
    private var midiScheduler: PanelScheduler?

    private var peakAge: [[Float]]
    private var timer: Timer?
    private var demoStep = 0

    private static let sizes = [2, 10, 10]
    private static func blank() -> [[Float]] { sizes.map { Array(repeating: -Float.infinity, count: $0) } }

    init() {
        levels = Self.blank()
        peaks = Self.blank()
        peakAge = Self.sizes.map { Array(repeating: 0, count: $0) }
        cells = Self.sizes.map { (0..<$0).map { _ in MeterCell() } }
        // INPUT 11 and 12 have a read-out under their meter.
        for c in cells[ChannelNames.groupMonitor] { c.wantsReadout = true }
    }

    func cell(_ id: StripID) -> MeterCell { cells[id.group][id.channel] }

    /// Hands the levels to the meters' cells. A cell is touched only if what it shows changes.
    private func show() {
        for g in 0..<Self.sizes.count {
            for c in 0..<Self.sizes[g] {
                let cell = cells[g][c]
                let lit = Self.segments(levels[g][c]), peak = Self.segments(peaks[g][c])
                if cell.lit != lit { cell.lit = lit }
                if cell.peak != peak { cell.peak = peak }
                if cell.wantsReadout {
                    let r: Int? = lit > 0 ? Int(levels[g][c].rounded()) : nil
                    if cell.readout != r { cell.readout = r }
                }
            }
        }
    }

    var hasSource: Bool { source != nil }
    var demoRunning: Bool { timer != nil }

    /// 0 at the floor and below, 1 at 0 dBFS and above.
    static func fraction(_ dB: Float) -> Double {
        guard dB > floorDB else { return 0 }
        return Double(min(1, (dB - floorDB) / -floorDB))
    }

    /// How many of the meter's segments a level lights.
    static func segments(_ dB: Float, of count: Int = Metrics.meterSegments) -> Int {
        Int((fraction(dB) * Double(count)).rounded(.up))
    }

    func level(_ id: StripID) -> Float { levels[id.group][id.channel] }
    func peak(_ id: StripID) -> Float { peaks[id.group][id.channel] }

    /// One reading from a source, `seconds` after the one before. A meter jumps
    /// up at once and falls back at a steady rate; the peak mark waits, then falls.
    func feed(_ fresh: [[Float]], after seconds: Float, from name: String) {
        var l = levels, p = peaks
        for g in 0..<Self.sizes.count {
            for c in 0..<Self.sizes[g] {
                let new = g < fresh.count && c < fresh[g].count ? fresh[g][c] : -Float.infinity
                let fallen = l[g][c] - Self.fallDBPerSecond * seconds
                l[g][c] = max(new, fallen > Self.floorDB ? fallen : -Float.infinity)
                if l[g][c] >= p[g][c] {
                    p[g][c] = l[g][c]
                    peakAge[g][c] = 0
                } else {
                    peakAge[g][c] += seconds
                    if peakAge[g][c] > Self.peakHoldSeconds {
                        let down = p[g][c] - Self.fallDBPerSecond * seconds
                        p[g][c] = max(l[g][c], down > Self.floorDB ? down : -Float.infinity)
                    }
                }
            }
        }
        levels = l
        peaks = p
        show()
        if source != name { source = name }
    }

    /// What a source saw on the MIDI connectors since its last reading (the pretend levels).
    func feedMIDI(in i: Bool, out o: Bool) {
        setLamp(0, i)
        setLamp(1, o)
    }

    private func setLamp(_ n: Int, _ on: Bool) {
        midiActive[n] = on
        if midiLamps[n].lit != on { midiLamps[n].lit = on }
    }

    /// The helper said MIDI passed MIDI IN (`in`) or MIDI OUT (`out`) just now: that lamp
    /// lights, and goes dark `midiLitMicros` after the last time it was told.
    func blinkMIDI(in i: Bool, out o: Bool, scheduler: PanelScheduler) {
        midiScheduler = scheduler
        for (n, passed) in [i, o].enumerated() where passed {
            midiOffAt[n] = scheduler.now + Self.midiLitMicros
            setLamp(n, true)
            if midiOffID[n] == nil { bookMIDIOff(n, scheduler) }
        }
    }

    /// One wake-up for lamp `n`: dark if nothing new came, else once more at the new time.
    private func bookMIDIOff(_ n: Int, _ scheduler: PanelScheduler) {
        midiOffID[n] = scheduler.schedule(at: midiOffAt[n]) { [weak self] in
            guard let self = self else { return }
            self.midiOffID[n] = nil
            if scheduler.now + 1000 >= self.midiOffAt[n] {
                self.setLamp(n, false)
            } else {
                self.bookMIDIOff(n, scheduler)
            }
        }
    }

    /// Lamps booked to go dark: 0, 1 or 2. 0 whenever both are dark.
    var midiWakeUps: Int { midiOffID.compactMap { $0 }.count }

    /// No way to the unit's MIDI any more: both lamps dark at once, nothing left booked.
    func midiDark() {
        for n in 0..<2 {
            if let id = midiOffID[n] { midiScheduler?.cancel(id) }
            midiOffID[n] = nil
            setLamp(n, false)
        }
    }

    /// No source any more: dark. `midi`: the MIDI lamps too; the helper's MIDI lamps don't
    /// depend on audio, so audio stopping leaves them as they are.
    func clear(midi: Bool = true) {
        if midi { midiDark() }
        timer?.invalidate()
        timer = nil
        levels = Self.blank()
        peaks = Self.blank()
        peakAge = Self.sizes.map { Array(repeating: 0, count: $0) }
        show()
        if source != nil { source = nil }
    }

    // MARK: pretend levels, for the look only

    static let pretendName = "pretend levels, for the look only"

    /// What a pretend channel reads at a step. Plain arithmetic, repeatable.
    static func pretend(step: Int) -> [[Float]] {
        func wave(_ base: Float, _ swing: Float, _ rate: Double, _ phase: Double) -> Float {
            let t = Double(step) / demoHz
            let slow = sin(t * rate + phase), quick = sin(t * rate * 7.3 + phase * 3)
            return base + swing * Float(0.65 * slow + 0.35 * quick)
        }
        var out = blank()
        let inp = ChannelNames.groupInput, wav = ChannelNames.groupOutput, mon = ChannelNames.groupMonitor
        out[inp][0] = wave(-14, 9, 2.1, 0.0)
        out[inp][1] = wave(-16, 9, 2.1, 0.6)
        out[inp][2] = wave(-9, 8, 3.4, 1.9)
        out[inp][3] = wave(-30, 12, 1.3, 4.0)
        out[wav][0] = wave(-11, 7, 2.7, 2.5)
        out[wav][1] = wave(-12, 7, 2.7, 3.0)
        out[mon][0] = wave(-8, 7, 2.4, 1.0)
        out[mon][1] = wave(-9, 7, 2.4, 1.5)
        return out
    }

    /// A still set, for the pictures. No timer.
    func showStill(step: Int = 23) {
        timer?.invalidate()
        timer = nil
        feed(Self.pretend(step: step), after: 0, from: Self.pretendName)
        // One mark a little above each level, as after a louder moment.
        peaks = levels.map { $0.map { $0 > Self.floorDB ? min(0, $0 + 5) : $0 } }
        show()
        feedMIDI(in: true, out: false)
    }

    /// Pretend MIDI data at a step: short bursts in, fewer out.
    static func pretendMIDI(step: Int) -> (in: Bool, out: Bool) {
        (step % 9 < 2 || step % 31 < 4, step % 47 < 3)
    }

    /// Moving pretend levels on or off. On is the only time this file wakes the Mac up.
    func setDemo(_ on: Bool) {
        guard on != demoRunning else { return }
        if !on {
            clear()
            return
        }
        let t = Timer(timeInterval: 1 / Self.demoHz, repeats: true) { [weak self] _ in
            guard let self = self else { return }
            self.demoStep += 1
            self.feed(Self.pretend(step: self.demoStep), after: Float(1 / Self.demoHz), from: Self.pretendName)
            let midi = Self.pretendMIDI(step: self.demoStep)
            self.feedMIDI(in: midi.in, out: midi.out)
        }
        t.tolerance = 0.01
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }
}

/// The lit or unlit segments of one meter, as a single shape.
struct MeterSegments: Shape {
    var lit: Int
    var count = Metrics.meterSegments
    /// Draw only this one segment (the peak mark); nil draws 0..<lit.
    var only: Int?

    func path(in rect: CGRect) -> Path {
        var p = Path()
        let pitch = rect.height / CGFloat(count)
        let range = only.map { $0..<($0 + 1) } ?? 0..<min(lit, count)
        for i in range where i >= 0 && i < count {
            p.addRect(CGRect(x: rect.minX, y: rect.maxY - CGFloat(i + 1) * pitch + 1, width: rect.width, height: pitch - 1))
        }
        return p
    }
}

/// The lights of one meter. They sit in an overlay of the meter's slot and always have the
/// same three shapes, so a change is a repaint and never a new layout of the mixer.
struct MeterLights: View {
    @Environment(\.theme) private var theme
    @ObservedObject var cell: MeterCell

    var body: some View {
        // Green to -12 dBFS, amber to -3, red above.
        let colours = LinearGradient(stops: [
            .init(color: theme.signal, location: 0), .init(color: theme.signal, location: 0.80),
            .init(color: theme.warn, location: 0.80), .init(color: theme.warn, location: 0.95),
            .init(color: theme.bad, location: 0.95), .init(color: theme.bad, location: 1),
        ], startPoint: .bottom, endPoint: .top)
        ZStack {
            MeterSegments(lit: cell.lit).fill(colours)
            MeterSegments(lit: 0, only: cell.peak > cell.lit ? cell.peak - 1 : -1).fill(colours)
        }
    }
}

/// One meter beside a fader: -60 dBFS at the bottom, 0 at the top.
struct MeterBar: View {
    @Environment(\.theme) private var theme
    @ObservedObject var meters: MeterModel
    let id: StripID
    let height: CGFloat
    /// Between the slot's edge and the first segment.
    static let inset: CGFloat = 1.5

    var body: some View {
        MeterSegments(lit: Metrics.meterSegments).fill(theme.lampOff)
            .overlay(MeterLights(cell: meters.cell(id)))
            .padding(Self.inset)
            .frame(width: Metrics.meterWidth + 3, height: height)
            .background(RoundedRectangle(cornerRadius: 2).fill(theme.track))
            .overlay(RoundedRectangle(cornerRadius: 2).strokeBorder(theme.edgeDark.opacity(0.6), lineWidth: 0.5))
            .help(meters.source.map { "Level (\($0))" }
                  ?? "Level meter. Dark until there is audio to show: with UA-101 chosen, it lights while an app plays or records through the UA-101.")
    }
}

/// A light that follows a meter (signal present) or a lamp cell, drawn over a fixed space.
struct CellLED<Cell: ObservableObject>: View {
    @Environment(\.theme) private var theme
    @ObservedObject var cell: Cell
    let size: CGFloat
    let lit: (Cell) -> Bool

    var body: some View {
        Color.clear.frame(width: size * 1.9, height: size * 1.9)
            .overlay(LED(lit: lit(cell), color: theme.signal, size: size))
    }
}

/// INPUT 11 or 12's read-out: whole dBFS, or a dash without signal. Over a fixed space too.
struct MeterReadout: View {
    @Environment(\.theme) private var theme
    @ObservedObject var cell: MeterCell
    let shown: Bool

    var body: some View {
        Text(shown ? cell.readout.map { "\($0)" } ?? "–" : "–").font(.readout(10)).foregroundColor(theme.readout)
    }
}
