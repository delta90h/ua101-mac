// The preview in Recall Memory (link version 20): what one of the unit's five memories holds,
// set against the settings in use, in plain words. Made from the memory's 256 bytes as read
// from the unit (src/ua101_ctlengine.c, ua101_eng_read_memory) and the settings in use. Only
// words: nothing here is sent, and a Recall does not use it (it sends its one message only).

import Foundation

/// What a Recall of one memory would change, part by part: `from` is the settings in use,
/// `to` what the memory holds. Nothing in the switches on the box: a Recall leaves them.
struct MemoryPreview: Equatable {
    var patchBay: [PutBackPlan.Change] = []
    var mixer: [PutBackPlan.Change] = []

    /// The changes for the parts chosen to load.
    func changes(patchBay pb: Bool, mixer mx: Bool) -> [PutBackPlan.Change] {
        (pb ? patchBay : []) + (mx ? mixer : [])
    }

    private static let fVolume = Int32(PANEL_F_VOLUME), fPan = Int32(PANEL_F_PAN), fOn = Int32(PANEL_F_ON)
    private static let fLink = Int32(PANEL_F_LINK), fSolo = Int32(PANEL_F_SOLO), fPatch = Int32(PANEL_F_PATCH)

    /// The memory `memory` against the settings in use `live`, field by field, as Backup does
    /// for a put back, but never refused: a value the app wouldn't send is still shown as read.
    /// The channels are those the mixer has at the unit's rate now (at 192 kHz, 1 to 6).
    static func make(memory: [UInt8], live: [UInt8]) -> MemoryPreview {
        var p = MemoryPreview()
        guard memory.count == 256, live.count == 256 else { return p }
        let is192 = SettingsBackup.switches(live).is192
        func val(_ block: [UInt8], _ kind: Int32, _ g: Int, _ i: Int) -> UInt32 { SettingsBackup.value(block, kind, g, i) }
        typealias Change = PutBackPlan.Change

        for o in 0..<5 {
            let m = val(memory, fPatch, 0, o), u = val(live, fPatch, 0, o)
            if m != u { p.patchBay.append(Change(what: "Patch bay, \(ChannelNames.outputPairs[o])", from: ChannelNames.sourceName(u), to: ChannelNames.sourceName(m))) }
        }
        for g in [ChannelNames.groupInput, ChannelNames.groupOutput] {
            let m = val(memory, fSolo, g, 0), u = val(live, fSolo, g, 0)
            if m != u { p.mixer.append(Change(what: "\(ChannelNames.groupTitle[g]) solo", from: SettingsBackup.soloWords(group: g, u), to: SettingsBackup.soloWords(group: g, m))) }
        }
        for s in Allowed.volumeStrips where PutBackPlan.exists(s, is192: is192) {
            let name = ChannelNames.full(s)
            let mv = val(memory, fVolume, s.group, s.channel), uv = val(live, fVolume, s.group, s.channel)
            if mv != uv { p.mixer.append(Change(what: "\(name) volume", from: Format.dB(uv) + " dB", to: Format.dB(mv) + " dB")) }
            if s.group != ChannelNames.groupMonitor {
                let mp = val(memory, fPan, s.group, s.channel), up = val(live, fPan, s.group, s.channel)
                if mp != up { p.mixer.append(Change(what: "\(name) pan", from: Format.pan(up), to: Format.pan(mp))) }
            }
            let mo = val(memory, fOn, s.group, s.channel), uo = val(live, fOn, s.group, s.channel)
            if mo != uo { p.mixer.append(Change(what: "\(name) mute", from: SettingsBackup.onWords(uo), to: SettingsBackup.onWords(mo))) }
            if s.channel % 2 == 0 {
                let pair = s.channel / 2
                let ml = val(memory, fLink, s.group, pair), ul = val(live, fLink, s.group, pair)
                if ml != ul {
                    p.mixer.append(Change(what: "\(ChannelNames.pair(group: s.group, pair: pair)) link", from: ul != 0 ? "linked" : "not linked", to: ml != 0 ? "linked" : "not linked"))
                }
            }
        }
        return p
    }
}

/// Where the preview of one memory stands, for the dialog.
enum MemoryPreviewState: Equatable {
    /// Not asked for yet.
    case notRead
    /// Asked; the unit's answer hasn't come yet.
    case reading
    case read(MemoryPreview)
    /// Can't show it, and why, in plain words (an older helper, no answer, the unit not read).
    case cantShow(String)
}
