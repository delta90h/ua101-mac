// A backup of the unit's settings: the whole settings block, 256
// bytes, as the unit gave it, in a plain text file a person can read. And what putting such
// a backup back would do: which settings change, which can't be put back, and what the unit
// should read back as afterwards. Nothing here sends anything or touches the engine; the
// model (PanelModel.swift) does the sending, with ordinary control messages only.
import Foundation

/// Why a file can't be used as a backup, or can't be put back on this unit now.
enum BackupProblem: Error, Equatable {
    case unreadable(String)   // the file can't be opened
    case notABackup           // not one of this app's backup files
    case newerFormat(String)  // written by a later version of the app
    case cutShort(Int)        // fewer than 256 bytes of settings
    case damaged              // the bytes aren't the ones that were saved
    case fromPretend          // made from the pretend unit, asked to go onto the UA-101
    case otherLayout(String)  // the box is laid out differently than when it was made
    case badValue(String)     // holds a value the unit never takes

    /// In plain words. Each ends by saying nothing was sent.
    var text: String {
        switch self {
        case .unreadable(let why):
            return "The file can't be read (\(why)). Nothing was sent."
        case .notABackup:
            return "This isn't a UA-101 settings backup made by this app. Nothing was sent."
        case .newerFormat(let f):
            return "This backup was written by a later version of the app (format \(f)); this version reads format \(SettingsBackup.format). Nothing was sent."
        case .cutShort(let n):
            return "The file is cut short: it holds \(n) of the 256 bytes of settings. Nothing was sent."
        case .damaged:
            return "The file is damaged or was changed by hand: its bytes aren't the ones that were saved. Nothing was sent."
        case .fromPretend:
            return "This backup was made from the pretend unit. It can't be put back on the UA-101. Nothing was sent."
        case .otherLayout(let why):
            return "This backup doesn't fit the unit as it is now: \(why). Nothing was sent."
        case .badValue(let what):
            return "The backup holds a value the unit never takes (\(what)). Nothing was sent."
        }
    }
}

/// One backup: the block, and what the file says about it.
struct SettingsBackup: Equatable {
    static let fileExtension = "ua101"
    static let firstLine = "UA-101 settings backup"
    static let format = 1
    static let blockLine = "Raw settings block"
    /// The real unit's name in the file's "Unit:" line (HelperTransport.displayName).
    static let realUnit = "UA-101"

    var block: [UInt8]
    /// "UA-101", or "Pretend unit".
    var unit: String
    /// When it was read from the unit, as written in the file: "2026-11-20 14:05:07".
    var saved: String
    /// Why it was saved: "Save Backup", "before Open Backup", "before Reset Mixer",
    /// "before Recall Memory 3", "Memory 3 as read through Recall (the patch bay and the monitor
    /// mixer)", "after Reset everything".
    var why: String

    var fromRealUnit: Bool { unit == Self.realUnit }

    // MARK: the file

    /// A check over the 256 bytes (FNV-1a, 32 bits), so a file that was damaged or edited by
    /// hand is refused instead of being put back.
    static func check(_ block: [UInt8]) -> UInt32 {
        var h: UInt32 = 0x811C9DC5
        for b in block { h = (h ^ UInt32(b)) &* 0x01000193 }
        return h
    }

    private static let stampFormat: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return f
    }()
    private static let nameFormat: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd HH.mm"
        return f
    }()
    static func stamp(_ d: Date) -> String { stampFormat.string(from: d) }

    /// The file's name: "2026-11-20 14.05.ua101", "2026-11-20 14.05 before Reset Mixer.ua101",
    /// and for the pretend unit "… pretend unit.ua101". `taken` says whether a name is in use:
    /// a second backup in the same minute gets " (2)".
    /// `label`: other words for the name than `why`'s, for a file that isn't a "before" one:
    /// "Memory 3 as read", "after Reset everything".
    static func fileName(at d: Date, why: String, pretend: Bool, label: String? = nil, taken: (String) -> Bool) -> String {
        var base = nameFormat.string(from: d)
        if let label = label, !label.isEmpty { base += " " + label }
        else if why.hasPrefix("before ") { base += " " + why }
        if pretend { base += " pretend unit" }
        var name = base + "." + fileExtension
        var n = 2
        while taken(name) {
            name = "\(base) (\(n)).\(fileExtension)"
            n += 1
        }
        return name
    }

    /// The file's whole text.
    func text() -> String {
        var lines = [
            Self.firstLine,
            "Format: \(Self.format)",
            "Saved: \(saved)",
            "Unit: \(unit)",
            "Why: \(why)",
            String(format: "Check: %08X", Self.check(block)),
            "",
            "\(Self.blockLine) (256 bytes, 16 a line, numbered from 000; a put back uses only these lines):",
        ]
        for row in 0..<16 {
            let bytes = block[row * 16..<row * 16 + 16].map { String(format: "%02X", $0) }.joined(separator: " ")
            lines.append(String(format: "%03d: ", row * 16) + bytes)
        }
        lines.append("")
        lines.append("What it holds (for reading; nothing below is used when a backup is put back):")
        lines.append(contentsOf: Self.describe(block))
        return lines.joined(separator: "\n") + "\n"
    }

    /// Reads a file's text back. Strict: anything that isn't exactly a backup is refused.
    static func parse(_ text: String) -> Result<SettingsBackup, BackupProblem> {
        let lines = text.replacingOccurrences(of: "\r", with: "").components(separatedBy: "\n")
        guard let first = lines.first(where: { !$0.trimmingCharacters(in: .whitespaces).isEmpty }),
              first.trimmingCharacters(in: .whitespaces) == firstLine else { return .failure(.notABackup) }
        var head: [String: String] = [:]
        var at = 0
        while at < lines.count, !lines[at].hasPrefix(blockLine) {
            if let colon = lines[at].firstIndex(of: ":") {
                let key = String(lines[at][..<colon]).trimmingCharacters(in: .whitespaces)
                head[key] = String(lines[at][lines[at].index(after: colon)...]).trimmingCharacters(in: .whitespaces)
            }
            at += 1
        }
        guard let f = head["Format"] else { return .failure(.notABackup) }
        guard f == String(format) else { return .failure(Int(f) == nil ? .notABackup : .newerFormat(f)) }
        guard at < lines.count else { return .failure(.cutShort(0)) }
        var block: [UInt8] = []
        at += 1
        while at < lines.count {
            let line = lines[at].trimmingCharacters(in: .whitespaces)
            guard let colon = line.firstIndex(of: ":"), Int(line[..<colon]) != nil else { break }
            let cells = line[line.index(after: colon)...].split(separator: " ")
            guard !cells.isEmpty else { break }
            for cell in cells {
                guard cell.count == 2, let b = UInt8(cell, radix: 16) else { return .failure(.damaged) }
                block.append(b)
            }
            at += 1
        }
        guard block.count >= 256 else { return .failure(.cutShort(block.count)) }
        guard block.count == 256, let check = head["Check"], let unit = head["Unit"], !unit.isEmpty,
              UInt32(check, radix: 16) == Self.check(block) else { return .failure(.damaged) }
        return .success(SettingsBackup(block: block, unit: unit, saved: head["Saved"] ?? "", why: head["Why"] ?? ""))
    }

    static func read(_ url: URL) -> Result<SettingsBackup, BackupProblem> {
        do {
            return parse(try String(contentsOf: url, encoding: .utf8))
        } catch {
            return .failure(.unreadable(error.localizedDescription))
        }
    }

    // MARK: in words

    private static let fVolume = Int32(PANEL_F_VOLUME), fPan = Int32(PANEL_F_PAN), fOn = Int32(PANEL_F_ON)
    private static let fLink = Int32(PANEL_F_LINK), fSolo = Int32(PANEL_F_SOLO), fPatch = Int32(PANEL_F_PATCH)

    static func value(_ block: [UInt8], _ kind: Int32, _ group: Int, _ index: Int) -> UInt32 {
        panel_block_value(block, kind, UInt32(group), UInt32(index))
    }

    struct Switches: Equatable {
        var rateHz: UInt32 = 0
        var rateCode = 0
        var softControl = false
        var mono = false
        var is192: Bool { rateCode == ChannelNames.rateCode192 }
    }

    static func switches(_ block: [UInt8]) -> Switches {
        var hz: UInt32 = 0
        var code: Int32 = 0, soft: Int32 = 0, mono: Int32 = 0
        panel_block_switches(block, &hz, &code, &soft, &mono)
        return Switches(rateHz: hz, rateCode: Int(code), softControl: soft != 0, mono: mono != 0)
    }

    static func rateWords(_ s: Switches) -> String { s.rateHz == 0 ? "an unknown rate" : Format.rate(s.rateHz) + " kHz" }

    /// The on/off bit as the mute in words (1.1: the app calls it MUTE): bit 1 is "on", the
    /// channel muted (heard on INPUT 1, 2026-11-14, and taken to be so for every channel, as the
    /// engine does).
    static func onWords(_ bit: UInt32) -> String { bit != 0 ? "on" : "off" } // the mute (1.1)
    static func soloWords(group: Int, _ v: UInt32) -> String { v == 0 ? "none" : "\(ChannelNames.groupTitle[group]) \(v)" }

    /// The readable list in the file: the switches, the patch bay, and every channel's
    /// volume, pan, mute and link, and the solos.
    static func describe(_ block: [UInt8]) -> [String] {
        let sw = switches(block)
        var out: [String] = []
        out.append("Switches on the box (a put back can't move them): sample rate \(rateWords(sw)), SOFT CTRL \(sw.softControl ? "in" : "out"), MONO \(sw.mono ? "in" : "out")" + String(format: ", digital input byte %02X", block[5]))
        for o in 0..<5 {
            out.append("Patch bay, \(ChannelNames.outputPairs[o]): \(ChannelNames.sourceName(value(block, fPatch, 0, o)))")
        }
        for g in [ChannelNames.groupInput, ChannelNames.groupOutput] {
            out.append("Solo, \(ChannelNames.groupTitle[g]): \(soloWords(group: g, value(block, fSolo, g, 0)))")
        }
        for s in Allowed.volumeStrips {
            let v = value(block, fVolume, s.group, s.channel)
            var line = "\(ChannelNames.full(s)): volume \(Format.dB(v)) dB " + String(format: "(0x%06X)", v)
            if s.group != ChannelNames.groupMonitor {
                let p = value(block, fPan, s.group, s.channel)
                line += ", pan \(Format.pan(p)) " + String(format: "(0x%04X)", p)
            }
            line += ", mute \(onWords(value(block, fOn, s.group, s.channel)))"
            line += value(block, fLink, s.group, s.channel / 2) != 0 ? ", linked (\(ChannelNames.pair(group: s.group, pair: s.channel / 2)))" : ", not linked"
            out.append(line)
        }
        return out
    }
}

/// What putting a backup back would do to the unit as it is now. Made twice: when the file
/// is chosen (for the user to read), and again from a fresh read just before anything is sent.
struct PutBackPlan: Equatable {
    struct Change: Equatable {
        var what: String  // "INPUT 3 volume"
        var from: String  // "−6.0 dB"
        var to: String    // "0.0 dB"
    }
    /// The settings a put back changes: the ones that differ now. One message each.
    var changes: [Change] = []
    /// Volumes the backup holds above 0 dB: the app puts 0 dB there, and says so.
    var held: [String] = []
    /// What differs and can't be put back: the switches on the box, and bytes that aren't
    /// settings this app changes.
    var cantPutBack: [String] = []
    /// What the unit should read back as afterwards, all 256 bytes.
    var expected: [UInt8] = []
    /// The bits of each byte that a put back sets.
    var mask: [UInt8] = []

    private static let fVolume = Int32(PANEL_F_VOLUME), fPan = Int32(PANEL_F_PAN), fOn = Int32(PANEL_F_ON)
    private static let fLink = Int32(PANEL_F_LINK), fSolo = Int32(PANEL_F_SOLO), fPatch = Int32(PANEL_F_PATCH)

    /// The channels the mixer has at this rate: at 192 kHz, INPUT and OUTPUT 1 to 6 only.
    static func exists(_ s: StripID, is192: Bool) -> Bool { !is192 || s.group == ChannelNames.groupMonitor || s.channel < 6 }

    /// The bits of the block a put back sets: patch bay row 1, the two solos, every pair's
    /// link, and every channel's on/off, pan and volume. Never the switches (d[4] to d[6]).
    static func settableMask(is192: Bool) -> [UInt8] {
        var m = [UInt8](repeating: 0, count: 256)
        for o in 0..<5 { m[12 + o] = 0xFF }
        m[0x16] = 0xFF
        m[0x17] = 0xFF
        for s in Allowed.volumeStrips where exists(s, is192: is192) {
            let v = PanelModel.volumeOffset(s)
            for i in v..<v + 6 { m[i] = 0xFF }
            if s.group != ChannelNames.groupMonitor {
                let p = PanelModel.panOffset(s)
                for i in p..<p + 4 { m[i] = 0xFF }
            }
            let on = PanelModel.onPlace(s)
            m[on.byte] |= UInt8(1 << on.bit)
            let link = PanelModel.linkPlace(group: s.group, pair: s.channel / 2)
            m[link.byte] |= UInt8(1 << link.bit)
        }
        return m
    }

    /// The same without the patch bay: what Reset Mixer sets.
    static func mixerMask(is192: Bool) -> [UInt8] {
        var m = settableMask(is192: is192)
        for o in 0..<5 { m[12 + o] = 0 }
        return m
    }

    /// The setting a byte belongs to, for a note: "INPUT 3 volume", or what `byteName` says.
    static func settingName(at i: Int) -> String {
        if (12...16).contains(i) { return "patch bay, \(ChannelNames.outputPairs[i - 12])" }
        if i == 0x16 { return "INPUT solo" }
        if i == 0x17 { return "OUTPUT solo" }
        for s in Allowed.volumeStrips {
            let v = PanelModel.volumeOffset(s)
            if (v..<v + 6).contains(i) { return ChannelNames.full(s) + " volume" }
            if s.group != ChannelNames.groupMonitor {
                let p = PanelModel.panOffset(s)
                if (p..<p + 4).contains(i) { return ChannelNames.full(s) + " pan" }
            }
        }
        for g in 0..<3 {
            let pairs = g == ChannelNames.groupMonitor ? 1 : 5
            if (0..<pairs).contains(where: { PanelModel.linkPlace(group: g, pair: $0).byte == i }) { return "\(ChannelNames.groupTitle[g]) links" }
            if (0..<pairs * 2).contains(where: { PanelModel.onPlace(StripID(group: g, channel: $0)).byte == i }) { return "\(ChannelNames.groupTitle[g]) mute" }
        }
        return byteName(i)
    }

    /// What a byte outside the mask is, for the user.
    static func byteName(_ i: Int) -> String {
        switch i {
        case 0...3: return "a flag at the start of the block"
        case 4: return "the sample rate knob"
        case 5: return "the digital input and the clock"
        case 6: return "the MONO and SOFT CTRL switches"
        case 7...11, 17...21: return "a patch bay row this app doesn't change"
        case 22...247: return "a part of the mixer this app doesn't change"
        default: return "a byte whose meaning isn't known"
        }
    }

    /// `backup` onto a unit that holds `unit` now. Refused whole, before anything is sent, if
    /// the backup doesn't fit the unit or holds a value the unit never takes.
    static func make(backup: [UInt8], unit: [UInt8], ceiling: UInt32) -> Result<PutBackPlan, BackupProblem> {
        guard backup.count == 256, unit.count == 256 else { return .failure(.cutShort(backup.count)) }
        let was = SettingsBackup.switches(backup), now = SettingsBackup.switches(unit)
        // The four flags at the start name the block's sections: a block laid out another way
        // can't be compared byte for byte.
        guard Array(backup[0..<4]) == Array(unit[0..<4]) else {
            return .failure(.otherLayout("the four flags at the start of its settings are \(Format.hex(Array(backup[0..<4]))), the unit's are \(Format.hex(Array(unit[0..<4])))"))
        }
        // At 192 kHz the mixer has six channels of each kind: a backup from one side of that
        // line doesn't fit the other.
        guard was.is192 == now.is192 else {
            return .failure(.otherLayout("it was made at \(SettingsBackup.rateWords(was)) and the unit is at \(SettingsBackup.rateWords(now)); at 192 kHz the mixer has other channels. Set the unit's rate knob as it was then (and switch the unit off and on), then put back again"))
        }
        let is192 = now.is192
        var plan = PutBackPlan()
        plan.mask = settableMask(is192: is192)
        var target = backup

        func val(_ block: [UInt8], _ kind: Int32, _ g: Int, _ i: Int) -> UInt32 { SettingsBackup.value(block, kind, g, i) }
        func ok(_ kind: Int32, _ g: Int, _ i: Int, _ v: UInt32) -> Bool { panel_value_ok(kind, UInt32(g), UInt32(i), v) != 0 }
        func digitsOK(_ at: Int, _ n: Int) -> Bool { backup[at..<at + n].allSatisfy { $0 <= 0x0F } }

        for o in 0..<5 {
            let b = val(backup, fPatch, 0, o), u = val(unit, fPatch, 0, o)
            guard ok(fPatch, 0, o, b) else { return .failure(.badValue("patch bay, \(ChannelNames.outputPairs[o]): source \(b)")) }
            if b != u {
                plan.changes.append(Change(what: "Patch bay, \(ChannelNames.outputPairs[o])", from: ChannelNames.sourceName(u), to: ChannelNames.sourceName(b)))
            }
        }
        for g in [ChannelNames.groupInput, ChannelNames.groupOutput] {
            let b = val(backup, fSolo, g, 0), u = val(unit, fSolo, g, 0)
            guard ok(fSolo, g, 0, b), !is192 || b <= 6 else { return .failure(.badValue("\(ChannelNames.groupTitle[g]) solo: \(b)")) }
            if b != u {
                plan.changes.append(Change(what: "\(ChannelNames.groupTitle[g]) solo", from: SettingsBackup.soloWords(group: g, u), to: SettingsBackup.soloWords(group: g, b)))
            }
        }
        for s in Allowed.volumeStrips where exists(s, is192: is192) {
            let name = ChannelNames.full(s)
            let at = PanelModel.volumeOffset(s)
            var b = val(backup, fVolume, s.group, s.channel)
            let u = val(unit, fVolume, s.group, s.channel)
            guard digitsOK(at, 6), ok(fVolume, s.group, s.channel, b) else { return .failure(.badValue("\(name) volume: \(Format.hex(Array(backup[at..<at + 6])))")) }
            if b > ceiling {
                plan.held.append("\(name) volume: the backup holds \(Format.dB(b)) dB; this app never sends more than \(Format.dB(ceiling)) dB, so \(Format.dB(ceiling)) dB is put there.")
                b = ceiling
                target.replaceSubrange(at..<at + 6, with: PanelModel.volumeDigits(b))
            }
            if b != u { plan.changes.append(Change(what: "\(name) volume", from: Format.dB(u) + " dB", to: Format.dB(b) + " dB")) }
            if s.group != ChannelNames.groupMonitor {
                let pat = PanelModel.panOffset(s)
                let bp = val(backup, fPan, s.group, s.channel), up = val(unit, fPan, s.group, s.channel)
                guard digitsOK(pat, 4), ok(fPan, s.group, s.channel, bp) else { return .failure(.badValue("\(name) pan: \(Format.hex(Array(backup[pat..<pat + 4])))")) }
                if bp != up { plan.changes.append(Change(what: "\(name) pan", from: Format.pan(up), to: Format.pan(bp))) }
            }
            let bo = val(backup, fOn, s.group, s.channel), uo = val(unit, fOn, s.group, s.channel)
            if bo != uo { plan.changes.append(Change(what: "\(name) mute", from: SettingsBackup.onWords(uo), to: SettingsBackup.onWords(bo))) }
            if s.channel % 2 == 0 {
                let pair = s.channel / 2
                let bl = val(backup, fLink, s.group, pair), ul = val(unit, fLink, s.group, pair)
                if bl != ul {
                    plan.changes.append(Change(what: "\(ChannelNames.pair(group: s.group, pair: pair)) link", from: ul != 0 ? "linked" : "not linked", to: bl != 0 ? "linked" : "not linked"))
                }
            }
        }
        plan.expected = (0..<256).map { (unit[$0] & ~plan.mask[$0]) | (target[$0] & plan.mask[$0]) }

        // What differs outside the mask stays as the unit has it.
        if was.rateCode != now.rateCode {
            plan.cantPutBack.append("The sample rate: the backup was made at \(SettingsBackup.rateWords(was)), the unit is at \(SettingsBackup.rateWords(now)). That is the knob on the box.")
        }
        if was.softControl != now.softControl || was.mono != now.mono {
            var parts: [String] = []
            if was.softControl != now.softControl { parts.append("SOFT CTRL was \(was.softControl ? "in" : "out")") }
            if was.mono != now.mono { parts.append("MONO was \(was.mono ? "in" : "out")") }
            plan.cantPutBack.append("The switches: in the backup \(parts.joined(separator: " and ")). Those are switches on the box.")
        }
        if backup[5] != unit[5] {
            plan.cantPutBack.append(String(format: "The digital input: the backup holds %02X in d[5], the unit %02X. That is the DIGITAL IN switch and what is plugged in.", backup[5], unit[5]))
        }
        let known: Set<Int> = [5]
        for i in 0..<256 where (backup[i] ^ unit[i]) & ~plan.mask[i] != 0 && !known.contains(i) {
            if i == 4, (backup[4] ^ unit[4]) & ~0x07 == 0 { continue }       // only the rate: said above
            if i == 6, (backup[6] ^ unit[6]) & ~0x06 == 0 { continue }       // only MONO and SOFT CTRL: said above
            plan.cantPutBack.append(String(format: "d[%d] (%@): the backup holds %02X, the unit %02X. This app can't change it.", i, byteName(i), backup[i], unit[i]))
        }
        return .success(plan)
    }

    /// After the put back: `after` is what the unit read back as. The bytes that still differ
    /// from the backup, each with why; `unexpected` are the ones that should have matched.
    struct Outcome: Equatable {
        var differing: [Int] = []
        var unexpected: [Int] = []
    }

    func outcome(after: [UInt8], backup: [UInt8]) -> Outcome {
        Outcome(differing: (0..<256).filter { after[$0] != backup[$0] },
                unexpected: (0..<256).filter { after[$0] != expected[$0] })
    }
}
