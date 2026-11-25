// Every channel name and channel fact the windows show comes from this one table.
// Which socket each channel is, from the manual (docs/hardware.md): INPUT 1-2 are
// the front combo sockets of the mic preamps, 3-8 and OUTPUT 1-8 are rear 1/4-inch
// jacks, 9-10 are the optical connectors (no coaxial), and the monitor mix is also
// sent to the computer as inputs 11-12 (measured: not at 192 kHz).
import Foundation

struct StripID: Hashable {
    var group: Int   // 0 MONITOR OUT, 1 INPUT, 2 OUTPUT (the engine's numbering)
    var channel: Int // 0-based
}

enum ChannelNames {
    static let groupMonitor = 0, groupInput = 1, groupOutput = 2

    static let groupTitle = ["MONITOR OUT", "INPUT", "OUTPUT"]
    /// A line under the group's title.
    static let groupNote = ["what you hear", "into the monitor mix", "from the computer"]

    /// The short label on a strip.
    static let short: [[String]] = [
        ["L", "R"],
        ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10"],
        ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10"],
    ]
    /// The kind of socket a channel has on the unit: it decides the drawing.
    enum Socket { case combo, jack, optical, none }

    static let socket: [[Socket]] = [
        [.jack, .jack],
        [.combo, .combo, .jack, .jack, .jack, .jack, .jack, .jack, .optical, .optical],
        [.jack, .jack, .jack, .jack, .jack, .jack, .jack, .jack, .optical, .optical],
    ]
    /// The small word under a strip's socket.
    private static let tags: [[String]] = [
        ["LINE", "LINE"],
        ["MIC PRE", "MIC PRE", "LINE", "LINE", "LINE", "LINE", "LINE", "LINE", "OPTICAL", "OPTICAL"],
        ["LINE", "LINE", "LINE", "LINE", "LINE", "LINE", "LINE", "LINE", "OPTICAL", "OPTICAL"],
    ]
    /// The sample rate code at which the unit has 6 channels each way, no optical
    /// connectors and no monitor mix sent to the computer.
    static let rateCode192 = 4

    /// INPUT 11-12 are not sockets and have no strips of their own: the unit sends
    /// its monitor mix to the computer as two more inputs, so the MONITOR OUT
    /// strips carry these words instead of LINE. Not at 192 kHz: no mix is sent.
    private static let recordedAs = ["→ IN 11", "→ IN 12"]

    static func tag(_ s: StripID, rateCode: Int) -> String {
        if s.group == groupMonitor { return rateCode == rateCode192 ? "" : recordedAs[s.channel] }
        return tags[s.group][s.channel]
    }
    /// LINE is the plain case; the others are worth a brighter word.
    static func tagStandsOut(_ s: StripID) -> Bool { s.group == groupMonitor || tags[s.group][s.channel] != "LINE" }

    /// One sentence about the socket, for the pointer's help.
    static func about(_ s: StripID, rateCode: Int) -> String {
        switch s.group {
        case groupMonitor:
            return rateCode == rateCode192
                ? "MONITOR OUT and PHONES. At 192 kHz the monitor mix isn't sent to the computer."
                : "MONITOR OUT and PHONES. The monitor mix is also sent to the computer: software records it as input \(11 + s.channel)."
        case groupInput:
            switch socket[s.group][s.channel] {
            case .combo: return "INPUT \(s.channel + 1): a mic preamp, the combo socket (XLR and 1/4-inch) on the front. Its SENS knob and switches are on the unit."
            case .optical: return "INPUT \(s.channel + 1): the optical digital input (OPTICAL IN 9-10). Not available at 192 kHz."
            default: return "INPUT \(s.channel + 1): a line input, a 1/4-inch jack on the back."
            }
        default:
            return socket[s.group][s.channel] == .optical
                ? "OUTPUT \(s.channel + 1): the optical digital output (OPTICAL OUT 9-10). Not available at 192 kHz."
                : "OUTPUT \(s.channel + 1): a line output, a 1/4-inch jack on the back."
        }
    }

    // The two MIDI connectors on the back of the unit.
    static let midiPorts = ["IN", "OUT"]
    static let midiAbout = [
        "MIDI IN: the green light shows MIDI data arriving at the unit's MIDI IN connector.",
        "MIDI OUT: the green light shows MIDI data leaving through the unit's MIDI OUT connector.",
    ]
    #if UA101_USER
    static let midiNote = " It blinks while MIDI passes, with or without audio."
    #else
    static let midiNote = " It blinks while MIDI passes, with UA-101 chosen, with or without audio. The pretend unit has no MIDI."
    #endif

    /// The order strips have on screen, left to right: the cursor moves along it.
    static let screenOrder: [StripID] =
        (0..<10).map { StripID(group: groupInput, channel: $0) } +
        (0..<10).map { StripID(group: groupOutput, channel: $0) } +
        (0..<2).map { StripID(group: groupMonitor, channel: $0) }

    static func full(_ s: StripID) -> String { "\(groupTitle[s.group]) \(short[s.group][s.channel])" }

    static func pair(group: Int, pair: Int) -> String {
        "\(groupTitle[group]) \(short[group][pair * 2])-\(short[group][pair * 2 + 1])"
    }

    static let outputPairs = ["OUTPUT 1-2", "OUTPUT 3-4", "OUTPUT 5-6", "OUTPUT 7-8", "OUTPUT 9-10"]
    static func outputPairSocket(_ pair: Int) -> Socket { socket[groupOutput][pair * 2] }
    static func outputPairTag(_ pair: Int) -> String { outputPairSocket(pair) == .optical ? "OPTICAL" : "" }

    /// Patch bay sources, by the value the unit uses (1 to 11).
    struct Source {
        let value: UInt32; let short: String; let long: String
        var socket: Socket = .none
        var tag = ""
    }
    static let sources: [Source] = [
        Source(value: 1, short: "MON", long: "MONITOR OUT"),
        Source(value: 2, short: "1-2", long: "INPUT 1-2", socket: .combo, tag: "MIC PRE"),
        Source(value: 3, short: "3-4", long: "INPUT 3-4", socket: .jack),
        Source(value: 4, short: "5-6", long: "INPUT 5-6", socket: .jack),
        Source(value: 5, short: "7-8", long: "INPUT 7-8", socket: .jack),
        Source(value: 6, short: "9-10", long: "INPUT 9-10", socket: .optical, tag: "OPTICAL"),
        Source(value: 7, short: "1-2", long: "WAVE OUT 1-2"),
        Source(value: 8, short: "3-4", long: "WAVE OUT 3-4"),
        Source(value: 9, short: "5-6", long: "WAVE OUT 5-6"),
        Source(value: 10, short: "7-8", long: "WAVE OUT 7-8"),
        Source(value: 11, short: "9-10", long: "WAVE OUT 9-10"),
    ]
    static func sourceName(_ value: UInt32) -> String {
        sources.first { $0.value == value }?.long ?? "unknown source (\(value))"
    }

    static let memories = ["Memory 1", "Memory 2", "Memory 3", "Memory 4", "Memory 5"]
}

enum Format {
    static func dB(_ volume: UInt32) -> String {
        if volume == 0 { return "−∞" }
        let d = panel_volume_db(volume)
        if abs(d) < 0.05 { return "0.0" }
        return String(format: "%@%.1f", d < 0 ? "−" : "+", abs(d))
    }

    static func pan(_ pan: UInt32) -> String {
        let center = Int(PANEL_CENTER_PAN)
        let p = Int(pan) - center
        if p == 0 { return "C" }
        let percent = Int((Double(abs(p)) / Double(center) * 100).rounded())
        return percent == 0 ? "C" : (p < 0 ? "L\(percent)" : "R\(percent)")
    }

    static func rate(_ hz: UInt32) -> String {
        switch hz {
        case 44100: return "44.1"
        case 48000: return "48"
        case 88200: return "88.2"
        case 96000: return "96"
        case 192000: return "192"
        default: return "?"
        }
    }

    static func hex(_ bytes: [UInt8]) -> String {
        bytes.map { String(format: "%02X", $0) }.joined(separator: " ")
    }

    static func value(kind: Int, _ v: UInt32) -> String {
        switch Int32(kind) {
        case Int32(PANEL_F_VOLUME): return dB(v) + " dB"
        case Int32(PANEL_F_PAN): return pan(v)
        case Int32(PANEL_F_ON): return v != 0 ? "bit 1" : "bit 0"
        case Int32(PANEL_F_LINK): return v != 0 ? "linked" : "not linked"
        case Int32(PANEL_F_PATCH): return ChannelNames.sourceName(v)
        default: return "\(v)"
        }
    }
}

/// The fader's scale: the bottom is off, then −60 dB to +12 dB in equal steps.
enum FaderScale {
    static let bottomDB = -60.0, topDB = 12.0
    static let offZone = 0.04

    static func position(volume: UInt32) -> Double {
        if volume == 0 { return 0 }
        let d = panel_volume_db(volume)
        return position(dB: d)
    }

    static func position(dB d: Double) -> Double {
        let p = offZone + (1 - offZone) * (d - bottomDB) / (topDB - bottomDB)
        return min(1, max(offZone, p))
    }

    static func volume(position p: Double) -> UInt32 {
        if p < offZone { return 0 }
        let d = bottomDB + (min(1, p) - offZone) / (1 - offZone) * (topDB - bottomDB)
        return volume(dB: d)
    }

    static func volume(dB d: Double) -> UInt32 {
        let v = Double(PANEL_UNITY_VOLUME) * pow(10, d / 20)
        return UInt32(min(Double(PANEL_MAX_VOLUME), max(1, v.rounded())))
    }
}
