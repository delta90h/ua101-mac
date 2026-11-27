// The driver's own settings (2026-11-19): the two delays that trade speed
// against safety from clicks, rate correction, and what the driver measured of the room they
// leave. They live in the driver's Core Audio plug-in, as properties of the UA-101's device,
// and are read and set through Core Audio (src/ua101_drvclient.h). Nothing here goes to the
// helper or to the unit. The self-test and the pictures use the pretend source only: they
// never ask Core Audio anything.
import Combine
import Foundation

/// What the driver says, as one read gave it.
struct DriverState: Equatable {
    /// The UA-101's device is in Core Audio (the unit is on and the driver installed).
    var found = false
    /// The two delays could be read (an older driver has none).
    var haveSettings = false
    var inputMs = 0.0, outputMs = 0.0
    var correction: Bool?
    /// The run that is on, or the last one. `anyRun` is false until sound has run once.
    var haveStats = false, running = false, anyRun = false
    var rate = 0.0, seconds = 0.0
    /// The safety offsets that run used, in frames.
    var inputOffset = 0.0, outputOffset = 0.0
    /// The least room each direction had, in frames; nil if not measured. Negative: lost.
    var captureSpare: Double?, playbackSpare: Double?
    var captureMissing = 0.0, playbackLate = 0.0

    func ms(_ frames: Double) -> Double { rate > 0 ? frames * 1000 / rate : 0 }
}

/// Where the settings come from: Core Audio, or a pretend driver.
protocol DriverSettingsSource: AnyObject {
    var isPretend: Bool { get }
    func read() -> DriverState
    /// Each returns 0, or Core Audio's error.
    func setInput(ms: Double) -> Int32
    func setOutput(ms: Double) -> Int32
    func setCorrection(_ on: Bool) -> Int32
}

/// The values the driver offers, from the same lists the driver itself checks against.
enum DriverValues {
    static let input: [Double] = (0..<Int(ua101_drv_input_count())).map { ua101_drv_input_value(Int32($0)) }
    static let output: [Double] = (0..<Int(ua101_drv_output_count())).map { ua101_drv_output_value(Int32($0)) }
    static let inputDefault = Int(ua101_drv_input_default())
    static let outputDefault = Int(ua101_drv_output_default())
    /// The place of the shortest recommended value (2026-11-20, a design choice: the whole
    /// scale, to try by ear). The ones before it come with a plain note, never a warning.
    static let inputRecommended = Int(ua101_drv_input_recommended())
    static let outputRecommended = Int(ua101_drv_output_recommended())
    /// The shortest music-app buffer, in frames at `rate`, that keeps playback delay `index`
    /// clean; 0 when any buffer will do (2026-11-20, measured at the box: 8 ms needs 128 frames,
    /// 6 ms 256, 4 and 2 ms 512, at 48 kHz). The notes under the playback slider name it.
    static func outputBuffer(_ index: Int, rate: Double) -> Int {
        Int(ua101_drv_output_min_buffer(Int32(index), rate > 0 ? rate : 48000))
    }

    static func inputIndex(_ ms: Double) -> Int? { let i = Int(ua101_drv_input_index(ms)); return i >= 0 ? i : nil }
    static func outputIndex(_ ms: Double) -> Int? { let i = Int(ua101_drv_output_index(ms)); return i >= 0 ? i : nil }

    /// "5 ms", "21.3 ms".
    static func label(_ ms: Double) -> String {
        abs(ms - ms.rounded()) < 0.05 ? String(format: "%.0f ms", ms) : String(format: "%.1f ms", ms)
    }

    /// "5", "21.3": a value without its unit, for under a slider's place.
    static func number(_ ms: Double) -> String {
        abs(ms - ms.rounded()) < 0.05 ? String(format: "%.0f", ms) : String(format: "%.1f", ms)
    }

    /// What Core Audio's error means, in plain words.
    static func words(for status: Int32) -> String {
        switch UInt32(bitPattern: status) {
        case 0x216F626A: return "the UA-101 isn't there (is the unit on?)"            // '!obj'
        case 0x6E6F7065: return "not a value this driver offers"                       // 'nope'
        case 0x77686F3F: return "this driver has no such setting (an older driver?)"   // 'who?'
        case 0x73746F70: return "Core Audio isn't running"                             // 'stop'
        default: return "Core Audio's error \(status)"
        }
    }
}

/// The real driver, through Core Audio.
final class CoreAudioDriverSettings: DriverSettingsSource {
    let isPretend = false

    func read() -> DriverState {
        var c = ua101_drv_state()
        var s = DriverState()
        guard ua101_drv_read(&c) == 0, c.found != 0 else { return s }
        s.found = true
        s.haveSettings = c.have_settings != 0
        s.inputMs = c.input_ms
        s.outputMs = c.output_ms
        s.correction = c.correction < 0 ? nil : c.correction != 0
        s.haveStats = c.have_stats != 0
        s.running = c.running != 0
        s.anyRun = c.any_run != 0
        s.rate = c.rate
        s.seconds = c.seconds
        s.inputOffset = c.input_offset
        s.outputOffset = c.output_offset
        s.captureSpare = c.capture_spare_known != 0 ? c.capture_spare : nil
        s.playbackSpare = c.playback_spare_known != 0 ? c.playback_spare : nil
        s.captureMissing = c.capture_missing
        s.playbackLate = c.playback_late
        return s
    }

    func setInput(ms: Double) -> Int32 { ua101_drv_set_input_ms(ms) }
    func setOutput(ms: Double) -> Int32 { ua101_drv_set_output_ms(ms) }
    func setCorrection(_ on: Bool) -> Int32 { ua101_drv_set_correction(on ? 1 : 0) }
}

#if !UA101_USER || UA101_PICTURES
/// A pretend driver, for trying the window out, for the self-test and for the pictures. It
/// takes only the values the real one offers, and a changed delay starts its figures afresh,
/// as the real driver's restart of the sound does.
final class PretendDriverSettings: DriverSettingsSource {
    /// False only for the pictures and the self-test, which show the window as it is with
    /// the real driver, without ever asking Core Audio.
    let isPretend: Bool
    var state: DriverState
    /// For the self-test: the next set is refused with this, once.
    var refuseNext: Int32 = 0
    /// Every set that was asked, in order.
    private(set) var asked: [String] = []
    private(set) var reads = 0

    init(pretend: Bool = true) {
        isPretend = pretend
        state = DriverState(found: true, haveSettings: true,
                            inputMs: DriverValues.input[DriverValues.inputDefault],
                            outputMs: DriverValues.output[DriverValues.outputDefault],
                            correction: true, haveStats: true)
    }

    func read() -> DriverState {
        reads += 1
        return state
    }

    private func refused() -> Int32? {
        guard state.found else { return Int32(bitPattern: 0x216F626A) }
        guard refuseNext != 0 else { return nil }
        defer { refuseNext = 0 }
        return refuseNext
    }

    func setInput(ms: Double) -> Int32 {
        asked.append("input \(DriverValues.label(ms))")
        if let no = refused() { return no }
        guard let i = DriverValues.inputIndex(ms) else { return Int32(bitPattern: 0x6E6F7065) }
        if state.inputMs != DriverValues.input[i] { restart() }
        state.inputMs = DriverValues.input[i]
        return 0
    }

    func setOutput(ms: Double) -> Int32 {
        asked.append("output \(DriverValues.label(ms))")
        if let no = refused() { return no }
        guard let i = DriverValues.outputIndex(ms) else { return Int32(bitPattern: 0x6E6F7065) }
        if state.outputMs != DriverValues.output[i] { restart() }
        state.outputMs = DriverValues.output[i]
        return 0
    }

    func setCorrection(_ on: Bool) -> Int32 {
        asked.append("correction \(on ? "on" : "off")")
        if let no = refused() { return no }
        state.correction = on
        return 0
    }

    /// The sound stops and starts: a new run, with nothing measured yet.
    private func restart() {
        guard state.anyRun else { return }
        state.seconds = 0
        state.captureSpare = nil
        state.playbackSpare = nil
        state.captureMissing = 0
        state.playbackLate = 0
    }

    /// A run with these figures, at 48 kHz, with the delays as they are set now.
    func pretendRun(running: Bool, seconds: Double, playbackSpareMs: Double?, captureSpareMs: Double?,
                    late: Double = 0, missing: Double = 0) {
        state.haveStats = true
        state.anyRun = true
        state.running = running
        state.rate = 48000
        state.seconds = seconds
        state.inputOffset = (state.inputMs * 48).rounded(.up)
        state.outputOffset = (state.outputMs * 48 - 0.001).rounded(.up)
        state.playbackSpare = playbackSpareMs.map { ($0 * 48).rounded() }
        state.captureSpare = captureSpareMs.map { ($0 * 48).rounded() }
        state.playbackLate = late
        state.captureMissing = missing
    }
}
#endif

/// The Driver Settings window's model: what the driver has, what the user has chosen in the
/// window and not applied yet, and the sending of it.
final class DriverSettingsModel: ObservableObject {
    enum Tone { case plain, good, warn, bad }
    struct Note: Equatable {
        var tone: Tone
        var text: String
    }

    @Published private(set) var state = DriverState()
    /// The window's own choice, applied only by Apply.
    @Published private(set) var inputIndex = DriverValues.inputDefault
    @Published private(set) var outputIndex = DriverValues.outputDefault
    @Published private(set) var correction = true
    @Published private(set) var note: Note?
    @Published private(set) var busy = false

    private(set) var source: DriverSettingsSource
    /// Where Core Audio is asked: off the main thread, so a slow answer never freezes the
    /// window. Nil runs everything at once on the caller's thread (pretend, self-test).
    private let queue: DispatchQueue?
    /// The user moved a control and hasn't applied it: reads don't move the controls then.
    private var touched = false
    /// Every change sent, with what came back, for the app's record.
    var onRecord: (String) -> Void = { _ in }

    init(source: DriverSettingsSource, queue: DispatchQueue? = nil) {
        self.source = source
        self.queue = queue
    }

    /// Another driver to talk to (the Device menu's choice). Starts clean.
    func use(_ source: DriverSettingsSource) {
        self.source = source
        touched = false
        note = nil
        state = DriverState()
        refresh()
    }

    /// The settings can be changed: the device is there and has them.
    var usable: Bool { state.found && state.haveSettings }

    /// What the window shows differs from what the driver has.
    var changed: Bool {
        guard usable else { return false }
        return DriverValues.inputIndex(state.inputMs) != inputIndex
            || DriverValues.outputIndex(state.outputMs) != outputIndex
            || (state.correction != nil && state.correction != correction)
    }

    // MARK: the notes under the sliders.
    // Plain words for every value, in the ordinary colours: what each needs or does. They follow
    // the slider at once, before Apply.

    /// The rate the buffer figures are at: the last run's, or 48 kHz before any.
    var noteRate: Double { state.rate > 0 ? state.rate : 48000 }

    /// The short note under one place of a slider: "buffer 512+", "any buffer"; "gaps", "clean".
    static func tickNote(playback: Bool, index: Int, rate: Double) -> String {
        if playback {
            guard DriverValues.output.indices.contains(index) else { return "" }
            let frames = DriverValues.outputBuffer(index, rate: rate)
            return frames > 0 ? "buffer \(frames)+" : "any buffer"
        }
        return String(cString: ua101_drv_input_note(Int32(index)))
    }

    /// The sentence under a slider, for the value the window shows. The choice at the box
    /// (2026-11-20: playback 8 ms, recording 8 ms, Logic's buffer 128) is the default since 1.1.
    static func sentence(playback: Bool, index: Int, rate: Double) -> String {
        if playback {
            guard DriverValues.output.indices.contains(index) else { return "" }
            let ms = DriverValues.output[index]
            let frames = DriverValues.outputBuffer(index, rate: rate)
            // The buffer the box was played at (128 frames at 48 kHz): 8 ms's own, at this rate.
            let tested = DriverValues.outputIndex(8).map { DriverValues.outputBuffer($0, rate: rate) } ?? 128
            let needs = "Needs the music app's buffer at \(frames) frames or more."
            switch (ms * 10).rounded() {
            case 20: return needs + " Heard delay is about the same as \(tested) + 8 ms."
            case 40: return needs + " At \(tested) it distorted at the box."
            case 60: return needs
            case 80: return needs + " The default: clean at \(tested) in the test."
            case 100: return "Works with any buffer. The lowest that does."
            case 120: return "Works with any buffer, a little more room."
            case 140: return "Works with any buffer, more room for a busy Mac."
            case 160: return "Works with any buffer, plenty of room."
            case 213: return "Works with any buffer, the most room."
            default: return frames > 0 ? needs : "Works with any buffer."
            }
        }
        guard DriverValues.input.indices.contains(index) else { return "" }
        switch (DriverValues.input[index] * 10).rounded() {
        case 10: return "Gaps in what you record: 2% of a take was missing in the test. The buffer doesn't change this."
        case 20: return "A few clicks in what you record: 5 frames missing in a take. The buffer doesn't change this."
        case 30: return "Clean in the test, with little to spare."
        case 50: return "Clean, with some room to spare."
        case 80: return "Clean, with more room for a busy Mac. The default."
        case 100: return "Clean, the most room."
        default: return ""
        }
    }

    /// The sentence for the value each slider shows now.
    func sentence(playback: Bool) -> String {
        Self.sentence(playback: playback, index: playback ? outputIndex : inputIndex, rate: noteRate)
    }

    var atDefaults: Bool {
        inputIndex == DriverValues.inputDefault && outputIndex == DriverValues.outputDefault && correction
    }

    private func run(_ work: @escaping () -> Void) {
        if let q = queue { q.async(execute: work) } else { work() }
    }

    private func onMain(_ work: @escaping () -> Void) {
        if queue != nil { DispatchQueue.main.async(execute: work) } else { work() }
    }

    /// Reads the driver again. The controls follow it unless the user has moved one.
    func refresh() {
        let source = self.source
        run { [weak self] in
            let s = source.read()
            self?.onMain {
                guard let self = self, self.source === source else { return }
                self.take(s)
            }
        }
    }

    private func take(_ s: DriverState) {
        if s != state { state = s }
        guard !touched, !busy, s.found, s.haveSettings else { return }
        // A value that isn't on the list (the helper's own at 44.1 kHz is 5.01 ms) shows as
        // the nearest one: the lists' own matching takes it, or the default.
        let i = DriverValues.inputIndex(s.inputMs) ?? nearest(DriverValues.input, s.inputMs)
        let o = DriverValues.outputIndex(s.outputMs) ?? nearest(DriverValues.output, s.outputMs)
        if i != inputIndex { inputIndex = i }
        if o != outputIndex { outputIndex = o }
        if let c = s.correction, c != correction { correction = c }
    }

    private func nearest(_ list: [Double], _ ms: Double) -> Int {
        list.indices.min { abs(list[$0] - ms) < abs(list[$1] - ms) } ?? 0
    }

    func choose(input index: Int) {
        guard usable, !busy, DriverValues.input.indices.contains(index), index != inputIndex else { return }
        inputIndex = index
        moved()
    }

    func choose(output index: Int) {
        guard usable, !busy, DriverValues.output.indices.contains(index), index != outputIndex else { return }
        outputIndex = index
        moved()
    }

    func choose(correction on: Bool) {
        guard usable, !busy, state.correction != nil, on != correction else { return }
        correction = on
        moved()
    }

    /// The values the driver came with, in the window; Apply sends them.
    func chooseDefaults() {
        guard usable, !busy, !atDefaults else { return }
        inputIndex = DriverValues.inputDefault
        outputIndex = DriverValues.outputDefault
        if state.correction != nil { correction = true }
        moved()
    }

    private func moved() {
        touched = changed
        note = nil
    }

    /// Sends what the window shows and the driver doesn't have, one setting at a time, then
    /// reads the driver again. A delay the driver takes makes Core Audio restart the sound.
    func apply() {
        guard usable, !busy, changed else { return }
        let source = self.source
        let was = state
        let input = DriverValues.input[inputIndex], output = DriverValues.output[outputIndex]
        let wantCorrection = correction
        busy = true
        note = nil
        run { [weak self] in
            var lines: [String] = []
            var failures: [String] = []
            func sent(_ what: String, _ from: String, _ to: String, _ status: Int32) {
                if status == 0 {
                    lines.append("driver settings: \(what) \(from) → \(to): taken")
                } else {
                    lines.append("driver settings: \(what) \(from) → \(to): REFUSED, \(DriverValues.words(for: status)) (\(status))")
                    failures.append("\(what): \(DriverValues.words(for: status))")
                }
            }
            if DriverValues.outputIndex(was.outputMs) != DriverValues.outputIndex(output) {
                sent("playback delay", DriverValues.label(was.outputMs), DriverValues.label(output), source.setOutput(ms: output))
            }
            if DriverValues.inputIndex(was.inputMs) != DriverValues.inputIndex(input) {
                sent("recording margin", DriverValues.label(was.inputMs), DriverValues.label(input), source.setInput(ms: input))
            }
            if let c = was.correction, c != wantCorrection {
                sent("rate correction", c ? "on" : "off", wantCorrection ? "on" : "off", source.setCorrection(wantCorrection))
            }
            let now = source.read()
            self?.onMain {
                guard let self = self else { return }
                lines.forEach(self.onRecord)
                self.busy = false
                self.touched = false
                guard self.source === source else { return }
                self.take(now)
                if failures.isEmpty {
                    self.note = Note(tone: .good, text: "Applied. The driver uses these values from now on.")
                } else {
                    self.note = Note(tone: .bad, text: "Not applied: " + failures.joined(separator: "; ") + ". The window shows what the driver has.")
                }
            }
        }
    }

    // MARK: what the window says about the measured room

    /// "12 s", "3 min 20 s", "1 h 05 min".
    static func length(_ seconds: Double) -> String {
        let s = Int(max(0, seconds).rounded())
        if s < 60 { return "\(s) s" }
        if s < 3600 { return "\(s / 60) min \(String(format: "%02d", s % 60)) s" }
        return "\(s / 3600) h \(String(format: "%02d", s % 3600 / 60)) min"
    }

    /// The line above the two figures.
    var runLine: String {
        let s = state
        guard s.found, s.haveStats else { return "Nothing measured." }
        guard s.anyRun else { return "No sound has run yet. Play or record something, then look here." }
        let with = "with \(DriverValues.label(s.ms(s.outputOffset))) and \(DriverValues.label(s.ms(s.inputOffset)))"
        return s.running ? "Sound is running: \(Self.length(s.seconds)) so far, \(with)."
                         : "The last stretch of sound: \(Self.length(s.seconds)), \(with)."
    }

    /// One direction's figure: the least room, and what was lost.
    func room(playback: Bool) -> Note {
        let s = state
        guard s.found, s.haveStats, s.anyRun else { return Note(tone: .plain, text: "—") }
        let spare = playback ? s.playbackSpare : s.captureSpare
        let lost = playback ? s.playbackLate : s.captureMissing
        if lost > 0 {
            return Note(tone: .bad, text: String(format: "%.0f frames %@ (%.1f ms of sound)", lost,
                                                 playback ? "came too late" : "were missing", s.ms(lost)))
        }
        guard let frames = spare else { return Note(tone: .plain, text: "not measured yet") }
        let ms = s.ms(frames)
        let text = String(format: "least room %.1f ms · nothing %@", ms, playback ? "late" : "missing")
        return Note(tone: ms < Self.thinRoomMs ? .warn : .good, text: text)
    }

    /// Under this much room the window warns: one late moment of the Mac would be heard.
    static let thinRoomMs = 1.0
}
