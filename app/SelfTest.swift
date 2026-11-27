// Drives the panel against the pretend unit, and against a stand-in for the
// helper (FakeHelper.swift), through the same calls the windows, menus and
// keys make, with time moved by hand. No window opens, and nothing here
// connects to the installed helper.
import AppKit
import Combine
import Foundation
import SwiftUI

private final class Checker {
    var passed = 0, failed = 0
    let verbose: Bool
    var section = ""
    init(verbose: Bool) { self.verbose = verbose }

    /// With UA101_SELFTEST_ONLY set (to a part of a section's name), only the sections whose
    /// name holds it are run: for the removal runs, which take a check out of a copy of the
    /// code and need the one section that should catch it, many times over. The last line
    /// then says that it wasn't the whole test.
    let only = ProcessInfo.processInfo.environment["UA101_SELFTEST_ONLY"]
    private(set) var sectionsRun = 0, sectionsSkipped = 0

    /// Starts a section. False if this run leaves it out.
    func begin(_ name: String) -> Bool {
        section = name
        if let only = only, !name.contains(only) {
            sectionsSkipped += 1
            return false
        }
        sectionsRun += 1
        if verbose { print("== \(name)") }
        return true
    }

    func check(_ ok: Bool, _ what: String, file: StaticString = #fileID, line: UInt = #line) {
        if ok {
            passed += 1
            if verbose { print("   ok    \(what)") }
        } else {
            failed += 1
            print("   FAIL  [\(section)] \(what)  (\(file):\(line))")
        }
    }

    /// A measured figure worth reading in the plain output, not a pass or a fail.
    func note(_ what: String) { print("   note  \(what)") }

    func equal<T: Equatable>(_ a: T, _ b: T, _ what: String, file: StaticString = #fileID, line: UInt = #line) {
        check(a == b, "\(what): got \(a), expected \(b)", file: file, line: line)
    }
}

func runSelfTest(verbose: Bool) -> Int32 {
    let t = Checker(verbose: verbose)
    let inp = ChannelNames.groupInput, out = ChannelNames.groupOutput, mon = ChannelNames.groupMonitor
    func strip(_ g: Int, _ c: Int) -> StripID { StripID(group: g, channel: c) }
    func unitValue(_ b: Bench, _ kind: Int32, _ g: Int, _ i: Int, real: Bool = false) -> UInt32 {
        panel_sim_value(real ? b.fake.unit.sim : b.unit.sim, kind, UInt32(g), UInt32(i))
    }
    let minus6 = FaderScale.volume(dB: -6), minus12 = FaderScale.volume(dB: -12)
    let unity = UInt32(PANEL_UNITY_VOLUME)
    // Every text a message gets anywhere in this test, for the end-user build's plain words.
    var seenTexts = Set<String>()
    MessageCenter.seen = { seenTexts.insert($0) }

    // ---- 1
    if t.begin("opening") {
        let b = Bench(connect: false)
        t.equal(b.model.snap.state, .offline, "before the link is up")
        b.model.connect()
        t.equal(b.model.snap.state, .syncing, "asked for the settings")
        b.settle()
        let s = b.model.snap
        t.equal(s.state, .ready, "settings read")
        t.check(!s.writesAllowed && !s.memoryAllowed, "starts read-only")
        t.equal(s.rateHz, 48000, "sample rate")
        t.check(!s.softControl && !s.mono, "Soft Control and Mono out, as the real unit was read")
        t.equal(s.clockSource, 0, "clock internal")
        t.equal(s.patch, [7, 8, 9, 10, 11], "patch bay: Initial Setting")
        t.equal(s.channels[inp][0].volume, unity, "INPUT 1 at 0 dB")
        t.equal(s.channels[inp][0].pan, 0, "INPUT 1 panned left")
        t.equal(s.channels[inp][1].pan, UInt32(PANEL_MAX_PAN), "INPUT 2 panned right")
        t.check(s.links[mon][0], "MONITOR OUT linked")
        t.check(!s.mixerLive, "mixer not live with Soft Control out")
        t.equal(panel_sim_reads(b.unit.sim), 1, "one read went to the unit")
    }

    // ---- 2
    if t.begin("read-only") {
        let b = Bench()
        b.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        b.settle()
        t.check(b.model.snap.softControl, "Soft Control seen going in")
        let reads = panel_sim_reads(b.unit.sim)
        t.equal(b.model.setVolume(strip(inp, 0), minus6), Int32(PANEL_ERR_READ_ONLY), "volume refused")
        t.equal(b.model.refusal?.kind, .readOnly, "the refusal says read-only")
        t.equal(b.model.setPan(strip(inp, 0), 0x4000), Int32(PANEL_ERR_READ_ONLY), "pan refused")
        t.equal(b.model.toggleMonitor(strip(inp, 0)), Int32(PANEL_ERR_READ_ONLY), "on/off refused")
        t.equal(b.model.toggleLink(group: inp, pair: 0), Int32(PANEL_ERR_READ_ONLY), "link refused")
        t.equal(b.model.setPatch(output: 0, source: 2), Int32(PANEL_ERR_READ_ONLY), "patch refused")
        t.equal(b.model.patchPreset(hardThru: true), Int32(PANEL_ERR_READ_ONLY), "preset refused")
        t.equal(b.model.resetMixer(), Int32(PANEL_ERR_READ_ONLY), "mixer reset refused")
        t.equal(b.model.prepareMemory(MemoryCommand(kind: .resetAll)), Int32(PANEL_ERR_READ_ONLY), "Reset refused")
        t.check(b.model.pendingMemory == nil, "no confirmation step offered")
        for key in [PanelKey.volumeDown, .panRight, .toggleMonitor, .toggleLink, .volumeUpBig] { b.model.handle(key) }
        b.model.beginDrag()
        b.model.setVolume(strip(out, 3), 0)
        b.model.endDrag()
        b.settle(5_000_000)
        t.equal(b.model.snap.channels[inp][0].volume, unity, "the picture didn't move")
        t.equal(b.model.snap.setsSent, 0, "the engine sent no change")
        t.equal(panel_sim_sets_applied(b.unit.sim), 0, "the unit got no change")
        t.equal(panel_sim_memory_commands(b.unit.sim), 0, "the unit got no memory command")
        t.equal(panel_sim_reads(b.unit.sim), reads, "and no extra read")
        t.equal(panel_sim_saving_paused(b.unit.sim), 0, "the unit's saving was never paused")
        t.equal(panel_guard_blocksCount(b), 0, "nothing even reached the guard")
        // Allowing memory without allowing changes does nothing.
        t.check(!b.model.snap.memoryAllowed, "memory can't be allowed while read-only")
    }

    // ---- 3
    if t.begin("Soft Control out") {
        let b = Bench()
        b.model.allowChanges(true)
        t.check(b.model.snap.writesAllowed, "changes allowed")
        t.equal(b.model.setVolume(strip(inp, 0), minus6), Int32(PANEL_ERR_SOFT_CTRL), "volume refused")
        t.equal(b.model.refusal?.kind, .softControl, "the refusal names Soft Control")
        t.equal(b.model.toggleMonitor(strip(out, 2)), Int32(PANEL_ERR_SOFT_CTRL), "on/off refused")
        t.equal(b.model.resetMixer(), Int32(PANEL_ERR_SOFT_CTRL), "mixer reset refused")
        t.equal(b.model.setPatch(output: 1, source: 2), Int32(PANEL_OK), "the patch bay still works")
        t.check(b.model.refusal == nil, "an accepted change clears the refusal")
        t.check(b.model.snap.busy, "busy until it is checked")
        b.settle()
        t.check(!b.model.snap.busy, "checked")
        t.equal(unitValue(b, Int32(PANEL_F_PATCH), 0, 1), 2, "the unit has OUTPUT 3-4 from INPUT 1-2")
        t.equal(panel_sim_sets_applied(b.unit.sim), 1, "exactly one change reached the unit")
        t.check(b.model.disagreement == nil, "no disagreement")
        // The switch goes in on the unit: the panel follows without being asked.
        b.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        b.time.advance(100_000)
        t.check(b.model.snap.settling, "a switch moved: waiting for it to settle")
        t.check(!b.model.snap.softControl, "the bouncing messages aren't believed")
        b.settle()
        t.check(b.model.snap.softControl && b.model.snap.mixerLive, "then the block is read: Soft Control is in")
        b.unit.press(Int32(PANEL_SWITCH_MONO), in: true)
        b.settle()
        t.check(b.model.snap.mono, "MONO follows too")
        b.unit.press(Int32(PANEL_SWITCH_DIGITAL), in: true)
        b.settle()
        t.equal(b.model.snap.clockSource, 1, "clock: digital in")
        t.check(b.model.snap.clockStatus != 0, "and no signal to lock to")
    }

    // ---- 4
    if t.begin("mixer") {
        let b = Bench()
        b.openUp()
        // A drag: many values, the unit ends on the last and saves once.
        let saves = panel_sim_saves(b.unit.sim)
        b.model.beginDrag()
        for step in 1...40 {
            b.model.setVolume(strip(inp, 2), FaderScale.volume(dB: -Double(step) * 0.3))
            b.time.advance(2_000)
        }
        t.equal(b.model.snap.channels[inp][2].volume, minus12, "the picture follows the drag at once")
        b.time.advance(20_000)
        t.equal(panel_sim_saving_paused(b.unit.sim), 1, "the unit's saving is paused during the drag")
        b.model.endDrag()
        b.settle()
        t.equal(unitValue(b, Int32(PANEL_F_VOLUME), inp, 2), minus12, "the unit ends on the last value")
        t.equal(panel_sim_saving_paused(b.unit.sim), 0, "saving resumed")
        t.equal(panel_sim_saves(b.unit.sim), saves + 1, "one save for the whole drag")
        t.check(b.model.snap.setsSent < 40, "only the latest values were sent (\(b.model.snap.setsSent) messages for 40 moves)")
        t.check(b.model.disagreement == nil && !b.model.snap.busy, "checked, no disagreement")

        // Above 0 dB (since 2026-11-27): up to the top of the range, as it is.
        let plus6 = FaderScale.volume(dB: 6)
        t.equal(b.model.setVolume(strip(inp, 3), plus6), Int32(PANEL_OK), "+6 dB goes as it is")
        t.check(b.model.refusal == nil && b.model.snap.channels[inp][3].volume == plus6, "nothing held, and the panel shows +6 dB")
        b.settle()
        t.equal(unitValue(b, Int32(PANEL_F_VOLUME), inp, 3), plus6, "the unit has +6 dB")
        b.model.setVolume(strip(inp, 3), unity)
        b.settle()

        // Pan, on/off.
        b.model.setPan(strip(inp, 4), 0x1234)
        b.model.toggleMonitor(strip(inp, 4))
        t.check(!b.model.snap.channels[inp][4].on, "INPUT 5 shows off")
        b.settle()
        t.equal(unitValue(b, Int32(PANEL_F_PAN), inp, 4), 0x1234, "pan reached the unit")
        t.equal(unitValue(b, Int32(PANEL_F_ON), inp, 4), 1, "the on/off bit reached the unit")
        t.equal(b.model.setPan(strip(mon, 0), 0x4000), Int32(PANEL_ERR_NO_CHANNEL), "MONITOR OUT has no pan")

        // A double-click: a fader back to 0 dB from below and from above, a pan to the centre.
        b.model.setVolume(strip(inp, 5), minus6)
        b.settle()
        t.equal(b.model.resetVolume(strip(inp, 5)), Int32(PANEL_OK), "a double-click on a fader is taken")
        t.equal(b.model.cursor, strip(inp, 5), "and selects its channel")
        b.settle()
        t.equal(unitValue(b, Int32(PANEL_F_VOLUME), inp, 5), unity, "a double-click takes a fader at −6 dB to 0 dB")
        b.model.setVolume(strip(inp, 5), plus6)
        b.settle()
        b.model.resetVolume(strip(inp, 5))
        b.settle()
        t.equal(unitValue(b, Int32(PANEL_F_VOLUME), inp, 5), unity, "and one at +6 dB down to 0 dB, never higher")
        b.model.centrePan(strip(inp, 4))
        b.settle()
        t.equal(unitValue(b, Int32(PANEL_F_PAN), inp, 4), UInt32(PANEL_CENTER_PAN), "a double-click puts a pan back in the centre")
        t.equal(b.model.centrePan(strip(mon, 0)), Int32(PANEL_ERR_NO_CHANNEL), "MONITOR OUT has no pan to centre")

        // Link: volume and on/off go to both.
        t.check(b.model.snap.links[out][1] && !b.model.snap.links[inp][1], "as delivered: OUTPUT pairs linked, INPUT pairs not")
        b.model.setVolume(strip(inp, 6), minus6)
        b.settle()
        t.equal(unitValue(b, Int32(PANEL_F_VOLUME), inp, 7), unity, "not linked: the partner keeps its volume")
        b.model.toggleLink(group: out, pair: 1)
        t.check(!b.model.snap.links[out][1], "a click unlinks OUTPUT 3-4")
        b.model.toggleLink(group: out, pair: 1)
        b.model.toggleLink(group: inp, pair: 4)
        b.model.setVolume(strip(inp, 9), minus12)
        b.model.setVolume(strip(out, 2), minus6)
        b.model.toggleMonitor(strip(out, 3))
        b.settle()
        t.equal(unitValue(b, Int32(PANEL_F_LINK), out, 1), 1, "OUTPUT 3-4 linked again on the unit")
        t.equal(unitValue(b, Int32(PANEL_F_LINK), inp, 4), 1, "INPUT 9-10 link reached the unit")
        t.equal(unitValue(b, Int32(PANEL_F_VOLUME), inp, 8), minus12, "INPUT 9 got its partner's volume")
        t.equal(unitValue(b, Int32(PANEL_F_VOLUME), out, 3), minus6, "the partner got the volume")
        t.equal(unitValue(b, Int32(PANEL_F_ON), out, 2), 1, "the partner got on/off")
        t.check(!b.model.snap.channels[out][2].on && !b.model.snap.channels[out][3].on, "both show off")

        // Solo: one number a group, 0 for none or the soloed channel 1 to 10 (as Roland's own
        // program does it); a click on a lit SOLO puts it out; a linked partner's lamp is lit too.
        func lamps(_ g: Int) -> [Int] { (0..<10).filter { b.model.snap.soloed[g][$0] } }
        t.check(b.model.snap.soloRaw == [0, 0, 0] && lamps(inp).isEmpty && lamps(out).isEmpty, "as delivered: no solo")
        var sent = b.model.snap.setsSent
        t.equal(b.model.toggleSolo(strip(inp, 2)), Int32(PANEL_OK), "a click on INPUT 3's SOLO")
        t.equal(lamps(inp), [2], "its lamp is lit at once, and no other")
        b.settle()
        t.check(unitValue(b, Int32(PANEL_F_SOLO), inp, 0) == 3 && unitValue(b, Int32(PANEL_F_SOLO), out, 0) == 0 && b.model.snap.setsSent == sent + 1,
                "the unit holds 3 for the INPUT group, nothing for OUTPUT; one message")
        b.model.toggleSolo(strip(inp, 6))
        b.settle()
        t.check(unitValue(b, Int32(PANEL_F_SOLO), inp, 0) == 7 && lamps(inp) == [6], "a click on INPUT 7's takes the solo over: the unit holds 7, one lamp")
        b.model.toggleSolo(strip(inp, 6))
        b.settle()
        t.check(unitValue(b, Int32(PANEL_F_SOLO), inp, 0) == 0 && lamps(inp).isEmpty, "a click on the lit one puts the solo out: the unit holds 0")
        // INPUT 9-10 was linked above: one number, both lamps, either lamp puts it out.
        sent = b.model.snap.setsSent
        b.model.toggleSolo(strip(inp, 9))
        b.settle()
        t.check(unitValue(b, Int32(PANEL_F_SOLO), inp, 0) == 10 && lamps(inp) == [8, 9] && b.model.snap.setsSent == sent + 1,
                "INPUT 10, linked with INPUT 9: one message with the number 10, both lamps lit")
        b.model.toggleSolo(strip(inp, 8))
        b.settle()
        t.check(unitValue(b, Int32(PANEL_F_SOLO), inp, 0) == 0 && lamps(inp).isEmpty, "a click on the partner's lit lamp puts it out")
        b.model.toggleSolo(strip(out, 0))
        b.model.toggleSolo(strip(inp, 0))
        b.settle()
        t.check(unitValue(b, Int32(PANEL_F_SOLO), out, 0) == 1 && unitValue(b, Int32(PANEL_F_SOLO), inp, 0) == 1 && lamps(out) == [0, 1] && lamps(inp) == [0],
                "INPUT and OUTPUT each have their own solo (OUTPUT 1-2 is linked: two lamps; INPUT 1-2 is not: one)")
        b.model.toggleSolo(strip(out, 1))
        b.model.toggleSolo(strip(inp, 0))
        b.settle()
        t.check(b.model.snap.soloRaw == [0, 0, 0], "and both out again")
        sent = b.model.snap.setsSent
        t.equal(b.model.toggleSolo(strip(mon, 0)), Int32(PANEL_ERR_NO_CHANNEL), "MONITOR OUT has no solo")
        b.settle()
        t.check(b.model.snap.setsSent == sent && b.model.disagreement == nil, "nothing was sent for it, and nothing disagreed")

        // Reset setting: back to the delivered mixer, patch bay untouched.
        b.model.setPatch(output: 0, source: 3)
        b.model.toggleSolo(strip(out, 6))
        b.settle()
        t.equal(unitValue(b, Int32(PANEL_F_SOLO), out, 0), 7, "OUTPUT 7 is soloed before the reset")
        t.equal(b.model.resetMixer(), Int32(PANEL_OK), "mixer reset accepted")
        b.settle()
        t.check(unitValue(b, Int32(PANEL_F_SOLO), out, 0) == 0 && b.model.snap.soloRaw == [0, 0, 0], "no solo after it, as delivered")
        var same = true
        for g in 0..<3 {
            let n = g == mon ? 2 : 10
            for c in 0..<n {
                same = same && unitValue(b, Int32(PANEL_F_VOLUME), g, c) == panel_factory_value(Int32(PANEL_F_VOLUME), UInt32(g), UInt32(c))
                same = same && unitValue(b, Int32(PANEL_F_ON), g, c) == panel_factory_value(Int32(PANEL_F_ON), UInt32(g), UInt32(c))
                if g != mon { same = same && unitValue(b, Int32(PANEL_F_PAN), g, c) == panel_factory_value(Int32(PANEL_F_PAN), UInt32(g), UInt32(c)) }
            }
            for p in 0..<(n / 2) {
                same = same && unitValue(b, Int32(PANEL_F_LINK), g, p) == panel_factory_value(Int32(PANEL_F_LINK), UInt32(g), UInt32(p))
            }
        }
        t.check(same, "every mixer setting on the unit is the delivered one")
        t.equal(unitValue(b, Int32(PANEL_F_PATCH), 0, 0), 3, "the patch bay wasn't touched")
        t.check(b.model.disagreement == nil && !b.model.snap.busy, "checked, no disagreement")
        t.equal(panel_sim_unknown(b.unit.sim), 0, "the unit understood every message")
    }

    // ---- 5
    if t.begin("cursor and keys") {
        let b = Bench()
        b.openUp()
        t.equal(b.model.cursor, strip(inp, 0), "the cursor starts on INPUT 1")
        b.model.handle(.cursorLeft)
        t.equal(b.model.cursor, strip(mon, 1), "left of INPUT 1 wraps to MONITOR OUT R")
        b.model.handle(.cursorRight)
        for _ in 0..<12 { b.model.handle(.cursorRight) }
        t.equal(b.model.cursor, strip(out, 2), "twelve to the right: OUTPUT 3")
        for _ in 0..<4 { b.model.handle(.volumeDown) }
        t.equal(Format.dB(b.model.snap.channels[out][2].volume), "−2.0", "four steps down: −2.0 dB")
        b.time.advance(100_000)
        t.equal(panel_sim_saving_paused(b.unit.sim), 1, "held keys count as one drag")
        b.model.handle(.volumeDownBig)
        t.equal(Format.dB(b.model.snap.channels[out][2].volume), "−5.0", "a big step: −5.0 dB")
        b.model.handle(.volumeUpBig)
        b.model.handle(.volumeUpBig)
        t.equal(b.model.snap.channels[out][2].volume, unity, "up from below stops at 0 dB (−5, −2, then 0, not +1)")
        b.model.handle(.volumeUpBig)
        t.equal(b.model.snap.channels[out][2].volume, unity, "the key still held (a repeat, as far as the app knows): it stays at 0 dB")
        b.model.handle(.volumeUpBig, repeated: false)
        t.equal(Format.dB(b.model.snap.channels[out][2].volume), "+3.0", "a fresh press goes past it: +3.0 dB")
        for _ in 0..<6 { b.model.handle(.volumeUpBig) }
        t.equal(b.model.snap.channels[out][2].volume, FaderScale.volume(dB: 12), "and up to +12 dB, the top, no higher")
        b.model.handle(.volumeDown)
        b.model.handle(.volumeUp)
        t.equal(b.model.snap.channels[out][2].volume, FaderScale.volume(dB: 12), "a step down and up again, above 0 dB: no stop there")
        for _ in 0..<5 { b.model.handle(.volumeDownBig) }
        b.model.handle(.panRight)
        b.model.handle(.toggleMonitor)
        b.model.handle(.toggleLink)
        b.settle()
        t.equal(panel_sim_saving_paused(b.unit.sim), 0, "saving resumed after the keys went quiet")
        t.equal(Format.dB(unitValue(b, Int32(PANEL_F_VOLUME), out, 2)), "−3.0", "the unit has −3.0 dB (+12 and five steps of 3 down)")
        t.equal(unitValue(b, Int32(PANEL_F_PAN), out, 2), UInt32(PanelModel.keyPanStep), "pan moved one step right of hard left")
        t.equal(unitValue(b, Int32(PANEL_F_ON), out, 2), 1, "on/off toggled")
        t.equal(unitValue(b, Int32(PANEL_F_LINK), out, 1), 0, "OUTPUT 3-4 unlinked by the key")
        b.model.handle(.panCenter)
        b.settle()
        t.equal(unitValue(b, Int32(PANEL_F_PAN), out, 2), UInt32(PANEL_CENTER_PAN), "pan centred")
        // All the way down goes to off, and up again comes back from the bottom.
        b.model.cursor = strip(inp, 5)
        for _ in 0..<25 { b.model.handle(.volumeDownBig) }
        t.equal(b.model.snap.channels[inp][5].volume, 0, "all the way down is off")
        b.model.handle(.volumeUp)
        t.equal(Format.dB(b.model.snap.channels[inp][5].volume), "−60.0", "one step up from off: −60 dB")
        b.settle()
        t.check(b.model.disagreement == nil && !b.model.snap.busy, "checked, no disagreement")
    }

    // ---- 5b
    if t.begin("a held ↑ key stops at 0 dB (the owner on the box, 2026-11-27)") {
        // On the box a held ↑ went from −13.0 dB in steps of 0.5 straight on to +12 dB: the
        // steps land on 0 dB exactly, and the next repeat went on from there.
        let b = Bench()
        b.openUp()
        let in2 = strip(inp, 1)
        b.model.setLink(group: inp, pair: 0, on: true)
        b.model.setVolumeExact(in2, FaderScale.volume(dB: -13))
        b.settle()
        b.model.cursor = in2
        var path: [String] = []
        for i in 0..<60 {
            b.model.handle(.volumeUp, repeated: i > 0)
            b.time.advance(80_000) // key repeat, 12 a second
            path.append(Format.dB(b.model.snap.channels[inp][1].volume))
        }
        t.check(path[25] == "0.0" && path.last == "0.0" && !path.contains { $0.hasPrefix("+") },
                "held for 60 repeats from −13 dB: up to 0.0 dB in 26 steps, then it stays there (last \(path.last ?? "?"))")
        t.check(b.model.snap.channels[inp][0].volume == unity, "INPUT 1, linked, stays at 0 dB with it")
        b.model.handle(.volumeUp) // not known whether it is a repeat, and the key was held a moment ago
        t.equal(b.model.snap.channels[inp][1].volume, unity, "a step that may be a repeat, just after: still 0 dB")
        b.model.handle(.volumeUp, repeated: false)
        t.equal(Format.dB(b.model.snap.channels[inp][1].volume), "+0.5", "let go and pressed again: on to +0.5 dB")
        for _ in 0..<40 { b.model.handle(.volumeUp, repeated: true); b.time.advance(80_000) }
        t.equal(b.model.snap.channels[inp][1].volume, FaderScale.volume(dB: 12), "held from there: up to +12 dB, the top")
        // Down, then up again with a key not known to be a repeat, after the keys were quiet.
        for _ in 0..<5 { b.model.handle(.volumeDownBig) }
        b.settle()
        t.equal(Format.dB(b.model.snap.channels[inp][1].volume), "−3.0", "five big steps down: −3.0 dB, no stop on the way down")
        b.model.handle(.volumeUpBig)
        b.model.handle(.volumeUpBig)
        t.equal(b.model.snap.channels[inp][1].volume, unity, "two big steps up from −3 dB: 0 dB, not +3")
        b.settle()
        b.model.handle(.volumeUpBig)
        t.equal(Format.dB(b.model.snap.channels[inp][1].volume), "+3.0", "after the keys were quiet a moment, the next step goes on: +3.0 dB")
        b.settle()
        t.check(b.model.disagreement == nil && !b.model.snap.busy && unitValue(b, Int32(PANEL_F_VOLUME), inp, 1) == FaderScale.volume(dB: 3),
                "the unit has +3.0 dB, no disagreement")
    }

    // ---- 6
    if t.begin("patch bay") {
        let b = Bench()
        b.model.allowChanges(true)
        b.model.patchPreset(hardThru: true)
        t.equal(b.model.snap.patch, [2, 3, 4, 5, 6], "Hard Thru shows at once")
        b.settle()
        t.equal((0..<5).map { unitValue(b, Int32(PANEL_F_PATCH), 0, $0) }, [2, 3, 4, 5, 6], "Hard Thru on the unit")
        b.model.patchPreset(hardThru: false)
        b.settle()
        t.equal((0..<5).map { unitValue(b, Int32(PANEL_F_PATCH), 0, $0) }, [7, 8, 9, 10, 11], "Initial Setting on the unit")
        t.equal(panel_sim_memory_commands(b.unit.sim), 0, "made of ordinary changes, not Roland's preset command")
        t.equal(b.model.setPatch(output: 2, source: 12), Int32(PANEL_ERR_VALUE), "a source that doesn't exist is refused")
        b.model.setPatch(output: 4, source: 1)
        b.settle()
        t.equal(unitValue(b, Int32(PANEL_F_PATCH), 0, 4), 1, "OUTPUT 9-10 plays MONITOR OUT")
        t.equal(ChannelNames.sourceName(b.model.snap.patch[4]), "MONITOR OUT", "and is named so")
    }

    // ---- 7
    if t.begin("the unit disagrees") {
        let b = Bench()
        b.openUp()
        b.unit.ignore(kind: Int(PANEL_F_VOLUME))
        b.model.setVolume(strip(inp, 6), minus12)
        t.equal(b.model.snap.channels[inp][6].volume, minus12, "the new value shows first")
        b.settle()
        t.equal(b.model.snap.channels[inp][6].volume, unity, "then the unit's value wins")
        t.check(b.model.disagreement != nil, "and the panel says the unit disagreed")
        if let d = b.model.disagreement {
            t.equal(d.sent, minus12, "with the value sent")
            t.equal(d.unit, unity, "and the value the unit holds")
            t.check(d.touches(strip(inp, 6), kind: Int32(PANEL_F_VOLUME)), "marked on INPUT 7's fader")
            t.check(!d.touches(strip(inp, 5), kind: Int32(PANEL_F_VOLUME)), "and on no other strip")
            t.check(d.text.contains("INPUT 7 volume") && d.text.contains("−12.0 dB") && d.text.contains("0.0 dB"), "in plain words: \(d.text)")
        }
        t.equal(b.model.snap.disagreements, 1, "counted once")
        b.unit.ignore(kind: -1)
        b.model.setVolume(strip(inp, 6), minus12)
        b.settle()
        t.equal(unitValue(b, Int32(PANEL_F_VOLUME), inp, 6), minus12, "a later change goes through")
        t.check(b.model.disagreement != nil, "the notice stays until dismissed")
        b.model.dismissDisagreement()
        t.check(b.model.disagreement == nil, "dismissed")
    }

    // ---- 8
    if t.begin("Load, Store, Reset") {
        let b = Bench()
        let store = MemoryCommand(kind: .store, memory: 2, patchBay: true, mixer: true)
        t.equal(b.model.prepareMemory(store), Int32(PANEL_ERR_READ_ONLY), "refused until changes are allowed")
        b.openUp()
        t.check(b.model.snap.memoryAllowed, "allowed with them")
        // The warning before each Recall, Save and Reset (a design choice of 2026-11-21, when
        // the go-ahead locks went): what is overwritten, that it can't be undone except from a
        // backup, and that a backup of the settings in use is saved first.
        let every = [MemoryCommand(kind: .load, memory: 2, patchBay: true, mixer: true), store,
                     MemoryCommand(kind: .resetCurrent), MemoryCommand(kind: .resetMemory, memory: 4), MemoryCommand(kind: .resetAll)]
        t.check(MemoryCommand.cantBeUndone == "This can't be undone, except from a backup.", "the warning's middle line")
        t.check(every.allSatisfy { !$0.consequence.isEmpty && $0.backupWords.hasPrefix("A backup of the settings in use now is saved first, by itself") },
                "every command says what it overwrites and that a backup is saved first")
        t.check(every.filter { $0.changesCurrent }.allSatisfy { $0.backupWords.contains("Open Backup") && $0.backupWords.contains("monitors down") }
                    && every.filter { $0.changesCurrent }.count == 3,
                "a Load, a Reset of the settings in use and a Reset of everything say how to go back, and to turn the monitors down")
        t.check(every[1].backupWords.contains("What Memory 3 holds now is in no backup") && every[3].backupWords.contains("What Memory 5 holds now is in no backup")
                    && every[4].backupWords.contains("What the five memories hold is in no backup"),
                "a Store, a Reset of a memory and a Reset of everything say that the memory's own content is in no backup")
        t.check(every.allSatisfy { !$0.backupWords.contains("go-ahead") && !$0.consequence.contains("go-ahead") }, "and none names a go-ahead")
        // The user's names of 2026-11-22: Recall Memory, Save Memory, Open Backup, Copy Backup
        // to Memory, under two headings; the warning covers Open Backup and Reset Mixer too.
        t.equal(every[0].title, "Recall Memory 3", "a Load is called Recall")
        t.equal(store.title, "Save to Memory 3", "a Store is called Save")
        t.check(PanelCommands.memoriesHeading == "Memories in the UA-101" && PanelCommands.backupsHeading == "Backup files on this Mac",
                "the Device menu's two headings")
        t.check(MixerWindowView.lineButtons.contains("Recall…") && MixerWindowView.lineButtons.contains("Save…")
                    && !MixerWindowView.lineButtons.contains("Load…") && !MixerWindowView.lineButtons.contains("Store…"),
                "the mixer's top line says Recall… and Save…")
        t.check([RootView.resetMixerWarning, PutBackView.overwriteWords + " " + MemoryCommand.cantBeUndone + " " + PutBackView.backupFirstWords]
                    .allSatisfy { $0.contains("overwritten") && $0.contains(MemoryCommand.cantBeUndone) && $0.contains("saved first, by itself") && $0.contains("monitors down") },
                "Reset Mixer and Open Backup warn in the same words: overwritten, can't be undone, a backup first, monitors down")
        let oldWords = ["Load Memory", "Store Memory", "Store into", "Restore Backup", "Load, Store", "a Load saves", "loaded earlier", "loaded each"]
        let shown = every.flatMap { [$0.title, $0.consequence, $0.backupWords] } + [RootView.resetMixerWarning, PutBackView.overwriteWords, PutBackView.backupFirstWords]
        t.check(shown.allSatisfy { s in !oldWords.contains { s.contains($0) } }, "none of these says the old names")
        t.equal(b.model.prepareMemory(MemoryCommand(kind: .load, memory: 1, patchBay: false, mixer: false)),
                Int32(PANEL_ERR_VALUE), "a Load of nothing is refused")

        b.model.setVolume(strip(inp, 0), minus6)
        b.model.setPatch(output: 0, source: 2)
        b.settle()
        // Step 1 sends nothing.
        t.equal(b.model.prepareMemory(store), Int32(PANEL_OK), "Store prepared")
        t.check(b.model.pendingMemory != nil, "the confirmation step is up")
        t.equal(b.model.pendingMemory.map { Format.hex($0.bytes) } ?? "", "F0 41 10 00 69 12 00 00 05 04 00 00 02 02 73 F7",
                "it shows the message it would send")
        b.settle(5_000_000)
        t.equal(panel_sim_memory_commands(b.unit.sim), 0, "nothing sent while waiting")
        // Cancel sends nothing.
        b.model.cancelMemory()
        b.settle()
        t.check(b.model.pendingMemory == nil, "cancelled")
        t.equal(panel_sim_memory_commands(b.unit.sim), 0, "nothing sent after Cancel")
        t.equal(b.model.confirmMemory(), Int32(PANEL_ERR_TICKET), "confirm without a prepared command is refused")
        // A confirmation that runs out sends nothing.
        b.model.prepareMemory(store)
        b.settle(31_000_000)
        t.check(b.model.pendingMemory == nil, "after 30 s the step closes itself")
        t.check(b.model.memoryNote?.contains("ran out") == true, "and says so")
        t.equal(panel_sim_memory_commands(b.unit.sim), 0, "nothing sent after it ran out")
        // Going read-only drops a prepared command.
        b.model.prepareMemory(store)
        b.model.allowChanges(false)
        t.check(b.model.pendingMemory == nil && !b.model.snap.memoryAllowed, "read-only drops it, and Recall, Save and Reset go off with changes")
        b.model.allowChanges(true)
        // Step 2 sends once.
        b.model.prepareMemory(store)
        t.equal(b.model.confirmMemory(), Int32(PANEL_OK), "Store confirmed")
        t.equal(b.model.confirmMemory(), Int32(PANEL_ERR_TICKET), "a second click does nothing")
        b.settle()
        t.equal(panel_sim_memory_commands(b.unit.sim), 1, "one memory command reached the unit")
        t.equal(b.model.snap.memorySent, 1, "the engine counts one")

        // Change things, then Load brings the stored ones back.
        b.model.setVolume(strip(inp, 0), unity)
        b.model.setPatch(output: 0, source: 7)
        b.settle()
        b.model.prepareMemory(MemoryCommand(kind: .load, memory: 2, patchBay: false, mixer: true))
        b.model.confirmMemory()
        b.settle()
        t.equal(b.model.snap.channels[inp][0].volume, minus6, "Load (mixer only) brought the volume back")
        t.equal(b.model.snap.patch[0], 7, "and left the patch bay alone")
        b.model.prepareMemory(MemoryCommand(kind: .load, memory: 2, patchBay: true, mixer: false))
        b.model.confirmMemory()
        b.settle()
        t.equal(b.model.snap.patch[0], 2, "Load (patch bay) brought the patch back")

        // Reset everything: the picture follows the unit.
        b.model.prepareMemory(MemoryCommand(kind: .resetAll))
        t.check(b.model.pendingMemory?.command.consequence.contains("all five memories") == true, "the confirmation names what is lost")
        b.model.confirmMemory()
        b.settle()
        t.equal(b.model.snap.channels[inp][0].volume, unity, "after Reset: INPUT 1 at 0 dB")
        t.equal(b.model.snap.patch, [7, 8, 9, 10, 11], "after Reset: Initial Setting")
        t.check(b.model.snap.softControl, "the switches are physical: Soft Control still in")
        t.equal(panel_sim_memory_commands(b.unit.sim), 4, "four memory commands in all")
        t.check(b.model.disagreement == nil, "no disagreement")
    }

    // ---- 9
    if t.begin("the unit stops answering") {
        let b = Bench()
        b.openUp()
        b.model.setVolume(strip(inp, 0), minus6)
        b.settle()
        b.unit.setSilent(true)
        b.model.setVolume(strip(inp, 1), minus6)
        b.settle(6_000_000)
        t.equal(b.model.snap.state, .silent, "three unanswered reads: silent")
        t.equal(b.model.snap.channels[inp][0].volume, minus6, "the last known values stay on show")
        t.equal(b.model.setVolume(strip(inp, 2), minus6), Int32(PANEL_ERR_NOT_READY), "changes are refused")
        t.equal(b.model.prepareMemory(MemoryCommand(kind: .resetAll)), Int32(PANEL_ERR_NOT_READY), "memory commands too")
        b.unit.setSilent(false)
        b.settle(4_000_000)
        t.equal(b.model.snap.state, .ready, "it answers again: ready")
        t.equal(b.model.snap.channels[inp][1].volume, unitValue(b, Int32(PANEL_F_VOLUME), inp, 1), "the picture is what the unit holds")
        t.check(!b.model.snap.busy, "nothing is left waiting from before")
        b.model.disconnect()
        t.equal(b.model.snap.state, .offline, "link down: offline")
        t.check(!b.model.snap.unitAnswers, "link down: the USB lamp is dark")
        t.equal(b.model.setPatch(output: 0, source: 2), Int32(PANEL_ERR_NOT_READY), "and nothing can be changed")

        // The pretend unit's "Stop answering" switch: the USB lamp follows within about 3 s,
        // with nothing else done (2026-11-14, the user saw it stay lit until the unit was
        // switched to UA-101 and back).
        let q = Bench()
        t.check(q.model.snap.unitAnswers, "the pretend unit answers: the USB lamp is lit")
        q.unit.setSilent(true)
        q.settle(10_000_000)
        t.check(q.model.snap.state == .ready, "the switch by itself: the app asks nothing, so it doesn't know (why the front panel asks)")
        q.unit.setSilent(false)
        q.model.setPretendSilent(q.unit, true)
        q.settle(3_500_000)
        t.equal(q.model.snap.state, .silent, "\"Stop answering\" from the front panel: silent within 3.5 s")
        t.check(!q.model.snap.unitAnswers, "and the USB lamp is dark")
        q.model.setPretendSilent(q.unit, false)
        q.settle(500_000)
        t.check(q.model.snap.state == .ready && q.model.snap.unitAnswers, "answering again: ready and the lamp lit within 0.5 s")
        t.equal(panel_sim_unknown(q.unit.sim), 0, "the unit understood every message")
    }

    // ---- 10
    if t.begin("192 kHz") {
        let b = Bench()
        b.unit.setRate(code: 4)
        b.model.reconnect()
        b.openUp()
        t.equal(b.model.snap.rateHz, 192_000, "sample rate shown")
        t.check(b.model.snap.channels[inp][5].exists && !b.model.snap.channels[inp][6].exists, "INPUT 1 to 6 exist, 7 doesn't")
        t.check(b.model.snap.channels[mon][1].exists, "MONITOR OUT is all there")
        t.equal(b.model.setVolume(strip(out, 7), minus6), Int32(PANEL_ERR_NO_CHANNEL), "OUTPUT 8 is refused")
        t.equal(b.model.toggleLink(group: inp, pair: 3), Int32(PANEL_ERR_NO_CHANNEL), "INPUT 7-8 link is refused")
        t.equal(b.model.setVolume(strip(out, 5), minus6), Int32(PANEL_OK), "OUTPUT 6 works")
        b.settle()
        t.equal(unitValue(b, Int32(PANEL_F_VOLUME), out, 5), minus6, "and reached the unit")
    }

    // ---- 11
    if t.begin("idle costs nothing") {
        let b = Bench()
        b.openUp()
        b.model.setVolume(strip(inp, 0), minus6)
        b.settle(10_000_000)
        let reads = panel_sim_reads(b.unit.sim), wakes = b.time.wakeUps
        t.equal(b.time.pendingWakeUps, 0, "no wake-up is booked")
        t.check(b.time.advance(3_600_000_000), "one hour passes")
        t.equal(b.time.wakeUps, wakes, "with no wake-up")
        t.equal(panel_sim_reads(b.unit.sim), reads, "and no message")
    }

    // ---- 12
    if t.begin("quitting in mid-drag") {
        let b = Bench()
        b.openUp()
        b.model.beginDrag()
        b.model.setVolume(strip(inp, 0), minus12)
        b.time.advance(50_000)
        t.equal(panel_sim_saving_paused(b.unit.sim), 1, "saving paused in mid-drag")
        b.model.finish()
        b.time.advance(1_000)
        t.equal(panel_sim_saving_paused(b.unit.sim), 0, "quitting resumes the unit's saving")
        t.equal(unitValue(b, Int32(PANEL_F_VOLUME), inp, 0), minus12, "and the last value was sent")
    }

    // ---- 13
    if t.begin("names and numbers") {
        t.equal(ChannelNames.screenOrder.count, 22, "22 strips on screen")
        t.equal(Set(ChannelNames.screenOrder).count, 22, "each once")
        t.equal(ChannelNames.full(strip(inp, 9)), "INPUT 10", "names come from the table")
        t.equal(ChannelNames.pair(group: out, pair: 4), "OUTPUT 9-10", "pair names too")
        t.equal(ChannelNames.sources.map { $0.value }, Array(1...11), "eleven patch sources")
        t.equal(Format.dB(unity), "0.0", "0 dB")
        t.equal(Format.dB(0), "−∞", "off")
        t.equal(Format.pan(UInt32(PANEL_CENTER_PAN)), "C", "pan centre")
        t.equal(Format.pan(0), "L100", "pan hard left")
        t.equal(Format.pan(UInt32(PANEL_MAX_PAN)), "R100", "pan hard right")
        var worst = 0.0
        for tenth in stride(from: -600, through: 120, by: 5) {
            let d = Double(tenth) / 10
            worst = max(worst, abs(panel_volume_db(FaderScale.volume(position: FaderScale.position(dB: d))) - d))
        }
        t.check(worst < 0.01, "the fader's scale goes there and back within 0.01 dB (worst \(worst))")
        t.equal(FaderScale.volume(position: 0), 0, "the bottom of the fader is off")
        t.equal(FaderScale.volume(position: 1), FaderScale.volume(dB: 12), "the top is +12 dB")
    }

    // ---- 14
    if t.begin("meters") {
        let b = Bench()
        let m = b.model.meters
        let in1 = strip(inp, 0), in5 = strip(inp, 4)
        t.check(!m.hasSource && !m.demoRunning, "as the app opens: no source, no timer")
        t.equal(MeterModel.segments(m.level(in1)), 0, "so a meter is dark")
        t.equal(MeterModel.segments(0), Metrics.meterSegments, "0 dBFS lights every segment")
        t.equal(MeterModel.segments(-60), 0, "-60 dBFS lights none")
        t.equal(MeterModel.segments(-30), Metrics.meterSegments / 2, "-30 dBFS lights half")
        t.equal(MeterModel.segments(6), Metrics.meterSegments, "above 0 dBFS stays at the top")
        m.showStill()
        t.check(m.hasSource && !m.demoRunning, "the still pretend levels run no timer")
        t.check(MeterModel.segments(m.level(in1)) > 0, "a pretend channel shows a level")
        t.equal(MeterModel.segments(m.level(in5)), 0, "a channel without one stays dark")
        let before = m.level(in1)
        m.feed([[], [], []], after: 0.5, from: "test")
        t.check(abs(m.level(in1) - (before - MeterModel.fallDBPerSecond * 0.5)) < 0.01, "a meter falls back at its steady rate")
        t.check(m.peak(in1) > m.level(in1), "the peak mark stays above for a while")
        m.feed([[], [-3], []], after: 0.05, from: "test")
        t.check(abs(m.level(in1) + 3) < 0.01, "and jumps up at once")
        m.feed([[], [], []], after: 10, from: "test")
        t.equal(MeterModel.segments(m.level(in1)), 0, "silence brings it to dark")
        t.equal(MeterModel.segments(m.peak(in1)), 0, "and the peak mark too")
        t.equal(MeterModel().midiActive, [false, false], "as the app opens: both MIDI lights are dark")
        m.feedMIDI(in: true, out: true)
        t.equal(m.midiActive, [true, true], "MIDI data lights both")
        m.clear()
        t.check(!m.hasSource, "cleared: no source")
        t.equal(m.midiActive, [false, false], "and the MIDI lights are dark")
        t.equal(b.ui.look, Look.system, "as the app opens the look follows macOS")
        t.check(Look.system.appearanceName == nil && Look.dark.appearanceName == .darkAqua && Look.light.appearanceName == .aqua,
                "the three looks are the system's, dark and light")
        t.check(Theme.matching(.dark).name == "dark" && Theme.matching(.light).name == "light", "the theme matches the appearance")
        let nat = CGSize(width: 1000, height: 500)
        t.check(WindowFit.scale(natural: nat, available: nat) == 1
                && WindowFit.scale(natural: nat, available: CGSize(width: 1500, height: 900)) == 1.5
                && WindowFit.scale(natural: nat, available: CGSize(width: 100, height: 100)) == WindowFit.smallest
                && WindowFit.scale(natural: .zero, available: nat) == 1, "the mixer scales to the window as one piece, within limits")
        t.check(WindowFit.step(from: 1, up: true) == 1.25 && WindowFit.step(from: 1, up: false) == 0.9
                && WindowFit.step(from: 1.1, up: false) == 1 && WindowFit.step(from: 2.5, up: true) == 2.5
                && WindowFit.step(from: 0.6, up: false) == 0.6, "Zoom In and Zoom Out go to the next step and stop at the ends")
        // The View menu's three fixed sizes, and their checkmark.
        t.check(WindowFit.fixedSizes.map(\.title) == ["Actual (100%)", "Medium (80%)", "Small (60%)"]
                && WindowFit.fixedSizes.map(\.zoom) == [1, 0.8, 0.6], "the View menu has Actual (100%), Medium (80%) and Small (60%)")
        t.check(WindowFit.fixedSizes.allSatisfy { WindowFit.steps.contains($0.zoom) }
                && WindowFit.step(from: 0.9, up: false) == 0.8 && WindowFit.step(from: 0.8, up: true) == 0.9,
                "each fixed size is a Zoom In/Out step too")
        for z: CGFloat in [1, 0.8, 0.6] {
            let marked = WindowFit.fixedSizes.filter { WindowFit.isCurrent($0.zoom, zoom: z) }.map(\.zoom)
            t.equal(marked, [z], "at zoom \(z) the checkmark is on that size alone")
        }
        t.check(WindowFit.fixedSizes.allSatisfy { !WindowFit.isCurrent($0.zoom, zoom: 1.25) }
                && WindowFit.isCurrent(0.8, zoom: 0.805), "between the sizes no checkmark; a hair off still counts")
        // Remember Size, against a defaults store of its own (the user's is never touched).
        let suite = "ua101panel.selftest.size"
        if let d = UserDefaults(suiteName: suite) {
            d.removePersistentDomain(forName: suite)
            let main = CGRect(x: 0, y: 0, width: 1728, height: 1080), side = CGRect(x: 1728, y: 0, width: 1920, height: 1080)
            t.check(SizeMemory.isOn(d) && SizeMemory.launch(from: d, screens: [main]) == (1, nil),
                    "Remember Size is on by default; with nothing kept yet: Actual (100%), default place")
            SizeMemory.save(zoom: 0.8, topLeft: CGPoint(x: 300, y: 900), to: d)
            let r = SizeMemory.launch(from: d, screens: [main])
            t.check(r.zoom == 0.8 && r.topLeft == CGPoint(x: 300, y: 900), "a kept zoom and place come back: \(r)")
            SizeMemory.save(zoom: 1.5, topLeft: CGPoint(x: 2000, y: 1000), to: d)
            t.check(SizeMemory.launch(from: d, screens: [main, side]).topLeft == CGPoint(x: 2000, y: 1000), "a place on the second screen comes back")
            let gone = SizeMemory.launch(from: d, screens: [main])
            t.check(gone.zoom == 1.5 && gone.topLeft.map { main.contains(CGPoint(x: $0.x + 40, y: $0.y - 10)) } == true,
                    "that screen unplugged: the window opens on the main screen instead (\(String(describing: gone.topLeft)))")
            SizeMemory.save(zoom: 9, topLeft: CGPoint(x: -5000, y: -5000), to: d)
            let wild = SizeMemory.launch(from: d, screens: [main])
            t.check(wild.zoom == WindowFit.largest && wild.topLeft.map { main.contains(CGPoint(x: $0.x + 40, y: $0.y - 10)) } == true,
                    "an out-of-range zoom is held at 250%, a place off every screen comes onto one")
            SizeMemory.save(zoom: 0.1, topLeft: CGPoint(x: 300, y: 900), to: d)
            t.equal(SizeMemory.launch(from: d, screens: [main]).zoom, WindowFit.smallest, "too small a zoom is held at 60%")
            d.set(false, forKey: SizeMemory.rememberKey)
            t.check(!SizeMemory.isOn(d) && SizeMemory.launch(from: d, screens: [main]) == (1, nil),
                    "Remember Size off: Actual (100%) in the default place, whatever was kept")
            d.removePersistentDomain(forName: suite)
        } else {
            t.check(false, "a defaults store for the test")
        }
        t.check(b.ui.rememberSize && !b.ui.sizeSettled, "the panel starts with Remember Size on, and keeps nothing before the window settles")
        let monR = strip(ChannelNames.groupMonitor, 1)
        t.equal(ChannelNames.tag(monR, rateCode: 1), "→ IN 12", "MONITOR OUT R says it is the computer's input 12")
        t.equal(ChannelNames.tag(monR, rateCode: ChannelNames.rateCode192), "", "not at 192 kHz: no monitor mix is sent there")
        t.check(!ChannelNames.screenOrder.contains { $0.group != ChannelNames.groupMonitor && ChannelNames.tag($0, rateCode: 1).contains("IN 1") },
                "only the MONITOR OUT strips carry the IN 11/12 words")
        t.equal(b.model.snap.setsSent, 0, "meters send nothing to the unit")
    }

    // ---- 15
    if t.begin("the UA-101 through the helper: look only until changes are switched on") {
        // A message that would set INPUT 1's volume (docs/control-protocol.md, "Examples").
        let volumeSet: [UInt8] = [0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x07, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0xF7]
        let readRequest: [UInt8] = [0xF0, 0x41, 0x10, 0x00, 0x69, 0x11, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x7C, 0xF7]
        t.check(panel_is_read_request(readRequest, readRequest.count) != 0 && panel_is_read_request(volumeSet, volumeSet.count) == 0,
                "the settings request is told from a message that sets something")

        // The helper isn't installed (or is the older one): said in words, nothing sent, no retry.
        let none = Bench()
        none.fake.installed = false
        none.chooseReal()
        t.check(!none.model.transport.isPretend && none.model.transport.displayName == "UA-101", "UA-101 is the chosen unit")
        t.equal(none.model.snap.state, .offline, "no helper: no unit")
        t.check(none.model.linkNote?.contains("install/install.sh") == true, "and the banner says to install: \(none.model.linkNote ?? "nil")")
        let tries = none.fake.attaches
        none.settle(3_600_000_000)
        t.equal(none.fake.attaches, tries, "it doesn't keep asking a helper that isn't there")
        none.fake.installed = true
        none.model.refresh()
        none.settle()
        t.equal(none.model.snap.state, .ready, "after an install, \"Refresh Settings\" finds the unit")

        // The unit is off; it shows by itself when switched on.
        let b = Bench()
        b.fake.unitPresent = false
        b.fake.unit.press(Int32(PANEL_SWITCH_MONO), in: true)
        b.chooseReal()
        t.equal(b.model.snap.state, .offline, "the unit is off: no unit")
        t.check(b.model.linkNote?.contains("switched off") == true, "and the banner says so")
        b.fake.unitComes()
        b.settle()
        t.equal(b.model.snap.state, .ready, "switched on: the settings are read without a click")
        t.check(b.model.linkNote == nil, "and the note is gone")
        t.check(b.model.snap.mono && !b.model.snap.softControl, "they are the real unit's, not the pretend unit's (MONO in)")
        t.equal(b.model.snap.rateHz, 48000, "sample rate")
        t.equal(b.fake.passedOn, 1, "one message went to the unit: the settings request")
        t.check(!b.model.snap.writesAllowed, "read-only")

        // Until changes are switched on in the app (the developer build's Allow Changes),
        // nothing that changes the unit leaves it.
        t.check(!b.helper.changesOn, "the transport is shut")
        t.check(!b.model.snap.memoryAllowed, "and so is Load, Store, Reset")
        t.equal(b.model.setVolume(strip(inp, 0), minus6), Int32(PANEL_ERR_READ_ONLY), "a fader: read-only")
        t.equal(b.model.setPatch(output: 0, source: 2), Int32(PANEL_ERR_READ_ONLY), "the patch bay: read-only")
        t.equal(b.model.prepareMemory(MemoryCommand(kind: .resetAll)), Int32(PANEL_ERR_READ_ONLY), "Reset: read-only")
        t.equal(b.model.resetMixer(), Int32(PANEL_ERR_READ_ONLY), "Reset Mixer: read-only")
        b.model.beginDrag()
        b.model.endDrag()
        b.model.handle(.volumeUp)
        b.model.finish()
        b.settle(5_000_000)
        t.equal(b.fake.passedOn, 1, "none of that sent anything")
        t.check(b.fake.refused == 0 && b.helper.blocked == 0 && b.model.guardBlocks == 0,
                "and no guard had to step in: the setters refused first")

        // The transport's own rule, tried without the engine above it; then the helper's, without the transport.
        t.check(volumeSet.withUnsafeBufferPointer { !b.helper.send($0) }, "the transport doesn't take a message that sets something")
        t.equal(b.helper.blocked, 1, "it counts it")
        t.check(b.fake.refused == 0 && b.fake.passedOn == 1, "and the helper never saw it")
        // INPUT 1's volume one step past the top of the range (0x800000): the helper never passes that on.
        let volumeOver: [UInt8] = [0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x07, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6D, 0xF7]
        t.check(volumeOver.withUnsafeBufferPointer { b.fake.send($0) }, "a volume past the top of the range, handed to the helper directly")
        b.settle()
        t.check(b.fake.refused == 1 && b.helper.helperRefusals == 1, "the helper refuses it and says so")
        t.equal(b.model.refusal?.kind, .helper, "the panel shows that the helper refused")
        t.check(b.fake.passedOn == 2 && b.fake.changesPassedOn == 0, "and reads the unit again, to show what it really holds")
        t.equal(panel_sim_value(b.fake.unit.sim, Int32(PANEL_F_VOLUME), UInt32(inp), 0), unity, "the unit's volume is untouched")
        t.equal(panel_sim_sets_applied(b.fake.unit.sim), 0, "the unit was never asked to change anything")

        // A switch on the real unit's front panel.
        b.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        b.settle()
        t.check(b.model.snap.softControl, "SOFT CTRL pressed on the unit shows in the app")
        t.equal(b.fake.passedOn, 3, "after one more read")
        t.check(b.model.snap.mixerLive, "the mixer is drawn live")
        t.equal(b.model.setVolume(strip(inp, 0), minus6), Int32(PANEL_ERR_READ_ONLY), "and is still read-only")

        // Idle costs nothing.
        let wakeUps = b.time.wakeUps, asks = b.fake.levelAsks
        b.settle(3_600_000_000)
        t.equal(b.time.wakeUps, wakeUps, "an hour in step with the unit: no wake-ups")
        t.check(b.fake.passedOn == 3 && b.fake.levelAsks == asks, "nothing sent, no levels asked for")

        // Levels, only while an app plays or records.
        b.helper.pollLevels()
        b.settle()
        t.check(!b.model.meters.hasSource, "no audio streaming: the meters stay dark")
        b.fake.setStreaming(true)
        b.settle()
        t.check(b.helper.streaming, "an app starts playing: the transport knows")
        // The real helper's first answers after a start say "nothing yet" (0.2 s, measured).
        // Taking that for "audio stopped" left the real unit's meters dark for good.
        b.fake.notRunningYet = true
        b.helper.pollLevels()
        b.settle()
        t.check(b.helper.streaming && b.helper.wantsLevels, "\"no levels yet\" right after the start: the asking goes on")
        t.check(!b.model.meters.hasSource, "and the meters stay dark meanwhile")
        b.fake.notRunningYet = false
        let half = UInt32(PANEL_LEVEL_FULL_SCALE / 2)
        b.fake.levels = HelperLevels(streaming: true, rate: 48000,
                                     capture: [half, 0, UInt32(PANEL_LEVEL_FULL_SCALE), 0, 0, 0, 0, 0, 0, 0, half / 2, half],
                                     playback: [half, half / 4, 0, 0, 0, 0, 0, 0, 0, 0])
        b.helper.pollLevels()
        b.settle(50_000)
        let m = b.model.meters
        t.equal(m.source, HelperTransport.levelsName, "the meters have a source: the UA-101's audio")
        t.check(abs(m.level(strip(inp, 0)) + 6.02) < 0.01, "INPUT 1 at half scale reads -6 dBFS (\(m.level(strip(inp, 0))))")
        t.equal(MeterModel.segments(m.level(strip(inp, 1))), 0, "a silent input is dark")
        t.check(m.level(strip(inp, 2)) == 0, "full scale reads 0 dBFS")
        t.check(abs(m.level(strip(mon, 0)) + 12.04) < 0.01 && abs(m.level(strip(mon, 1)) + 6.02) < 0.01,
                "MONITOR OUT's meters show the computer's inputs 11 and 12")
        t.check(abs(m.level(strip(out, 0)) + 6.02) < 0.01 && abs(m.level(strip(out, 1)) + 18.06) < 0.01, "OUTPUT's meters show what the computer plays")
        b.helper.pollLevels()
        b.settle(50_000)
        t.check(m.level(strip(inp, 0)) < -6.5 && m.level(strip(inp, 0)) > -9, "0.05 s on with silence: the meter falls back at its rate")
        let at192 = HelperTransport.meterLevels(HelperLevels(streaming: true, rate: 192000, capture: Array(repeating: half, count: 6), playback: Array(repeating: half, count: 6)))
        t.check(at192[inp][5] > -7 && at192[inp][6] == -Float.infinity && at192[mon][0] == -Float.infinity,
                "at 192 kHz: six inputs, and no monitor mix to show")
        b.fake.notRunningYet = true
        b.helper.pollLevels()
        b.settle()
        t.check(!m.hasSource && b.helper.wantsLevels, "the stream halts for a moment: dark meters, and the asking goes on")
        b.fake.notRunningYet = false
        b.helper.metersShown = false
        t.check(!b.helper.wantsLevels, "the window can't be seen: no asking")
        b.helper.metersShown = true
        // What a reading costs: a meter says "changed" only when its lights change, and the
        // model itself only when the source changes. (Every reading used to redraw them all.)
        do {
            let mm = MeterModel()
            var one = 0, whole = 0
            let w1 = mm.cell(strip(inp, 0)).objectWillChange.sink { one += 1 }
            let w2 = mm.objectWillChange.sink { whole += 1 }
            var reading: [[Float]] = [Array(repeating: -Float.infinity, count: 2),
                                      Array(repeating: -Float.infinity, count: 10),
                                      Array(repeating: -Float.infinity, count: 10)]
            mm.feed(reading, after: 0, from: "a test")
            t.check(one == 0 && whole == 1, "silence: no meter redrawn, the source named once (\(one), \(whole))")
            reading[inp][0] = -6
            mm.feed(reading, after: 0, from: "a test")
            let first = one
            t.check(first > 0 && mm.cell(strip(inp, 0)).lit == MeterModel.segments(-6), "a level: that meter is redrawn")
            mm.feed(reading, after: 0, from: "a test")
            reading[inp][1] = -20
            mm.feed(reading, after: 0, from: "a test")
            t.check(one == first && whole == 1, "the same level again, and another meter moving: it is left alone (\(one - first), \(whole))")
            mm.clear()
            t.check(one > first && mm.cell(strip(inp, 0)).lit == 0, "no source any more: dark")
            _ = (w1, w2)
        }
        t.equal(b.fake.passedOn, 3, "levels send nothing to the unit")
        b.fake.setStreaming(false)
        b.settle()
        t.check(!m.hasSource && !b.helper.streaming, "the app stops: the meters go dark")

        // Unplugged and back; the Mac sleeping and waking; another program holding the unit.
        b.fake.setStreaming(true)
        b.settle()
        b.fake.unitGoes()
        b.settle()
        t.equal(b.model.snap.state, .offline, "unplugged: no unit")
        t.check(b.model.linkNote != nil && !b.helper.streaming, "with a note, and no levels")
        b.fake.unitComes()
        b.settle()
        t.equal(b.model.snap.state, .ready, "plugged in again: in step again")
        b.fake.macSleeps()
        b.fake.macWakes()
        b.fake.asleep = true // still waking when the app asks
        b.settle(500_000)
        t.equal(b.model.snap.state, .offline, "the Mac is waking: no unit yet")
        b.fake.asleep = false
        b.settle(HelperTransport.asleepRetryMicros)
        t.equal(b.model.snap.state, .ready, "it asks again by itself and is in step")
        b.fake.unitGoes()
        b.fake.unitComes()
        b.fake.interfaceBusy = true
        b.settle(1_000_000)
        t.check(b.model.snap.state == .offline && b.model.linkNote?.contains("another program") == true, "another program holds the unit's control connection: said in words")
        b.fake.interfaceBusy = false
        b.settle(HelperTransport.busyRetryMicros)
        t.equal(b.model.snap.state, .ready, "it asks again every 3 s and gets through")

        // Switching between the two units: nothing carries over.
        let c = Bench()
        c.openUp()
        t.check(c.model.snap.writesAllowed, "on the pretend unit, changes are allowed")
        c.model.setVolume(strip(inp, 0), minus12) // still on its way when the unit is switched
        c.model.onChooseDevice?(true)
        t.check(!c.model.snap.writesAllowed && c.model.snap.state != .ready, "UA-101 chosen: read-only at once, nothing known yet")
        c.settle()
        t.equal(unitValue(c, Int32(PANEL_F_VOLUME), inp, 0), minus12, "the change on its way went to the pretend unit it was meant for")
        t.equal(c.model.snap.state, .ready, "the UA-101's settings are read")
        t.equal(c.model.snap.channels[inp][0].volume, unity, "they are its own: INPUT 1 at 0 dB")
        t.check(!c.model.snap.softControl, "its own SOFT CTRL, out")
        t.check(c.fake.refused == 0 && c.fake.passedOn == 1 && c.helper.blocked == 0, "only the settings request went its way")
        t.equal(panel_sim_sets_applied(c.fake.unit.sim), 0, "nothing was changed on it")
        c.model.setVolume(strip(inp, 0), minus6)
        t.equal(c.model.refusal?.kind, .readOnly, "a fader on the UA-101: refused, read-only")
        c.model.onChooseDevice?(false)
        c.settle()
        t.check(c.model.transport.isPretend && c.model.snap.state == .ready, "back on the pretend unit")
        t.check(!c.model.snap.writesAllowed, "read-only again: changes don't stay on across a switch of units")
        t.equal(c.model.snap.channels[inp][0].volume, minus12, "and it shows the pretend unit's own values")
        t.equal(c.fake.detaches, 1, "the helper was told the app let go of the unit")
        let before = c.fake.passedOn
        c.fake.unit.press(Int32(PANEL_SWITCH_MONO), in: true)
        c.settle()
        t.check(!c.model.snap.mono && c.fake.passedOn == before, "the UA-101's switches no longer reach the panel")
    }

    // ---- 15b
    if t.begin("the UA-101 with changes on: the unit's saving") {
        let pauseBytes = "F0 41 10 00 69 12 00 00 00 00 00 00 F7", resumeBytes = "F0 41 10 00 69 12 00 00 00 00 01 7F F7"
        let volumeSet: [UInt8] = [0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x07, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x73, 0xF7]
        let resetAll: [UInt8] = [0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x00, 0x02, 0x00, 0x7E, 0xF7]
        t.equal(PanelModel.hex(PanelModel.savingMessage(resume: false)), pauseBytes, "\"pause saving\" is the notes' 13 bytes")
        t.equal(PanelModel.hex(PanelModel.savingMessage(resume: true)), resumeBytes, "\"resume saving\" is the notes' 13 bytes")
        t.check(panel_is_saving_message(PanelModel.savingMessage(resume: false), 13) != 0 && panel_is_saving_message(PanelModel.savingMessage(resume: true), 13) != 0
                    && panel_is_saving_message(resetAll, resetAll.count) == 0 && panel_is_saving_message(volumeSet, volumeSet.count) == 0,
                "they are told from \"reset all\" (one byte away) and from a volume change")

        // A helper as link version 6 was: the saving and OUTPUT 1-2's source, no mixer.
        let g = Bench()
        g.fake.passes = FakeHelper.version6
        var lines: [String] = []
        g.model.onRecord = { lines.append($0) }
        g.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        g.chooseReal()
        t.check(!g.model.snap.writesAllowed, "the UA-101 chosen: nothing is switched on by that in this build")
        g.model.allowChanges(true)
        t.check(g.model.snap.writesAllowed && g.helper.changesOn && g.model.refusal == nil, "\"Allow changes\": on at once, with no file and no question to the helper")
        g.settle()
        t.check(!g.model.allowed.someMemory && g.model.memoryBlocker()?.contains("passes on no Recall, Save or Reset") == true, "Recall, Save and Reset are not: this helper passes none on")
        t.equal(g.fake.passedOn, 1, "nothing was sent for that")

        // Every control the helper doesn't pass on is refused before the engine.
        let reads = g.fake.passedOn
        t.equal(g.model.setVolume(strip(inp, 0), minus6), Int32(PANEL_ERR_READ_ONLY), "a fader: refused")
        t.equal(g.model.refusal?.kind, .notPassed, "and the banner says it isn't one of the changes this version makes")
        t.check(g.model.refusal?.text.contains("the unit's saving") == true && g.model.refusal?.text.contains("OUTPUT 1-2") == true
                    && g.model.refusal?.text.contains("Nothing was sent") == true,
                "it names the two that are: \(g.model.refusal?.text ?? "nil")")
        t.equal(g.model.setPan(strip(inp, 0), 0x2000), Int32(PANEL_ERR_READ_ONLY), "pan: refused")
        t.equal(g.model.setMonitor(strip(inp, 0), on: false), Int32(PANEL_ERR_READ_ONLY), "on/off: refused")
        t.equal(g.model.setLink(group: inp, pair: 0, on: true), Int32(PANEL_ERR_READ_ONLY), "link: refused")
        for out in 1..<5 {
            t.equal(g.model.setPatch(output: out, source: 2), Int32(PANEL_ERR_READ_ONLY), "the patch bay, \(ChannelNames.outputPairs[out]): refused")
        }
        t.equal(g.model.patchPreset(hardThru: false), Int32(PANEL_ERR_READ_ONLY), "Initial Setting: refused")
        t.equal(g.model.patchPreset(hardThru: true), Int32(PANEL_ERR_READ_ONLY), "Hard Thru: refused")
        t.equal(g.model.resetMixer(), Int32(PANEL_ERR_READ_ONLY), "Reset Mixer: refused")
        t.check(!g.model.allowed.someMemory, "this helper passes on no Recall, Save or Reset")
        t.equal(g.model.prepareMemory(MemoryCommand(kind: .resetAll)), Int32(PANEL_ERR_READ_ONLY), "Reset: refused")
        g.model.beginDrag()
        g.model.setVolume(strip(inp, 0), minus12)
        g.model.endDrag()
        g.model.handle(.volumeUp)
        g.model.handle(.toggleMonitor)
        g.settle(5_000_000)
        t.check(g.fake.passedOn == reads && g.fake.changesPassedOn == 0, "none of that sent anything, not even a pause for the drag")
        t.check(g.fake.refused == 0 && g.helper.blocked == 0 && g.model.guardBlocks == 0, "and no guard had to step in")
        t.check(g.model.snap.writesAllowed, "changes are still on")
        // The transport's own rule, with changes on.
        t.check(volumeSet.withUnsafeBufferPointer { !g.helper.send($0) } && resetAll.withUnsafeBufferPointer { !g.helper.send($0) },
                "the transport, changes on, still takes neither a volume change nor \"reset all\"")
        t.check(g.helper.blocked == 2 && g.fake.refused == 0, "it counts them and the helper never saw them")

        // The first write: pause, read back, resume, read back.
        t.equal(g.model.resumeSaving(), Int32(PANEL_ERR_VALUE), "resume before any pause: not done")
        t.equal(g.model.pauseSaving(), Int32(PANEL_OK), "pause the unit's saving")
        t.check(g.model.savingNote?.contains(pauseBytes) == true, "the note shows the bytes sent")
        g.settle()
        t.check(panel_sim_saving_paused(g.fake.unit.sim) != 0 && g.fake.changesPassedOn == 1, "one message reached the unit, and its saving is paused")
        t.check(g.model.snap.savingHeld, "the panel knows it is paused")
        t.check(g.model.savingNote?.contains("same as before, all 256 bytes") == true && g.model.savingNote?.contains("said nothing else") == true,
                "the read-back: nothing else changed: \(g.model.savingNote ?? "nil")")
        t.equal(g.fake.passedOn, reads + 2, "the pause and one read")
        t.equal(g.model.pauseSaving(), Int32(PANEL_ERR_BUSY), "a second pause: not done")
        g.settle(3_600_000_000)
        t.check(panel_sim_saving_paused(g.fake.unit.sim) != 0 && g.fake.passedOn == reads + 2, "an hour later: still paused, and nothing more was sent")
        t.equal(g.model.resumeSaving(), Int32(PANEL_OK), "resume the unit's saving")
        g.settle()
        t.check(panel_sim_saving_paused(g.fake.unit.sim) == 0 && g.fake.changesPassedOn == 2, "the unit saves again")
        t.check(!g.model.snap.savingHeld && !g.model.snap.busy, "nothing is waiting")
        t.check(g.model.savingNote?.contains(resumeBytes) == true && g.model.savingNote?.contains("same as before, all 256 bytes") == true,
                "the read-back after the resume: \(g.model.savingNote ?? "nil")")
        t.equal(g.fake.passedOn, reads + 4, "the resume and one read")
        t.check(g.fake.refused == 0 && g.model.guardBlocks == 0 && g.model.snap.disagreements == 0, "nothing refused, nothing disagreed")
        t.equal(panel_sim_sets_applied(g.fake.unit.sim), 0, "no setting of the unit was changed")
        t.check(lines.contains { $0.contains("Pause saving: sent " + pauseBytes) } && lines.contains { $0.contains("Resume saving: sent " + resumeBytes) }
                    && lines.filter { $0.contains("settings after:") }.count == 2 && lines.contains { $0.contains("Changes switched on") },
                "all of it is in the record (\(lines.count) lines)")

        // Something else changes while the read-back is out: it is said, not hidden.
        t.equal(g.model.pauseSaving(), Int32(PANEL_OK), "pause again")
        g.fake.unit.press(Int32(PANEL_SWITCH_MONO), in: true)
        g.settle()
        t.check(g.model.savingNote?.contains("DIFFERENT") == true && g.model.savingNote?.contains("d[6]") == true,
                "a switch moved meanwhile: the note names the byte: \(g.model.savingNote ?? "nil")")

        // The helper refuses while saving is paused: the resume doesn't reach the unit.
        g.fake.refuseChanges = "a refusal made by the test; the message was not sent"
        let passed = g.fake.changesPassedOn
        t.equal(g.model.resumeSaving(), Int32(PANEL_OK), "resume, handed over")
        g.settle()
        t.check(g.fake.changesPassedOn == passed && g.fake.refused == 1, "the helper refused it")
        t.check(g.model.refusal?.kind == .helper && !g.model.snap.writesAllowed && !g.helper.changesOn, "the panel says so and is read-only again")
        t.check(!g.model.snap.busy && !g.model.snap.savingHeld, "nothing is left waiting")
        let quiet = g.fake.passedOn
        g.settle(3_600_000_000)
        t.check(g.fake.passedOn == quiet && g.fake.refused == 1, "and it doesn't try again by itself")
        t.check(lines.contains { $0.contains("The helper refused a change") }, "the refusal is in the record")
        t.check(PanelModel.savingMessage(resume: false).withUnsafeBufferPointer { !g.helper.send($0) }, "the transport, changes off, doesn't take a pause either")

        // Quitting or switching units while paused: the unit is told to save again first.
        let q = Bench()
        q.chooseReal()
        q.model.allowChanges(true)
        q.settle()
        q.model.pauseSaving()
        q.settle()
        t.check(panel_sim_saving_paused(q.fake.unit.sim) != 0, "paused")
        q.model.onChooseDevice?(false)
        q.settle()
        t.check(panel_sim_saving_paused(q.fake.unit.sim) == 0, "switching to the pretend unit resumes the UA-101's saving on the way out")
        t.check(!q.model.snap.writesAllowed && !q.helper.changesOn, "and changes are off in the app")
        q.chooseReal()
        t.check(!q.model.snap.writesAllowed && !q.helper.changesOn, "back on the UA-101: read-only until asked again")

        // The unit goes away while paused: the resume is owed and sent when it is back.
        let u = Bench()
        u.chooseReal()
        u.model.allowChanges(true)
        u.settle()
        u.model.pauseSaving()
        u.settle()
        u.fake.unitGoes()
        u.settle()
        t.equal(u.model.snap.state, .offline, "the unit went away while its saving was paused")
        u.fake.unitComes()
        u.settle()
        t.check(u.model.snap.state == .ready && panel_sim_saving_paused(u.fake.unit.sim) == 0, "back: the resume it was owed was sent")

        // The pretend unit takes the same two commands, with the app's own switch.
        let p = Bench()
        var pretendLines = 0
        p.model.onRecord = { _ in pretendLines += 1 }
        t.equal(p.model.pauseSaving(), Int32(PANEL_ERR_READ_ONLY), "pretend unit, read-only: pause refused")
        p.model.allowChanges(true)
        t.equal(p.model.pauseSaving(), Int32(PANEL_OK), "pretend unit, changes allowed: pause")
        p.settle()
        t.check(panel_sim_saving_paused(p.unit.sim) != 0, "its saving is paused")
        p.model.setVolume(strip(inp, 0), minus6) // SOFT CTRL is out: refused, and saving stays paused
        t.equal(p.model.resumeSaving(), Int32(PANEL_OK), "resume")
        p.settle()
        t.check(panel_sim_saving_paused(p.unit.sim) == 0 && !p.model.snap.busy, "it saves again")
        t.equal(pretendLines, 0, "nothing about the pretend unit goes into the record")
    }

    // ---- 15c
    if t.begin("the UA-101 with changes on: one patch bay change and back") {
        let toInput = "F0 41 10 00 69 12 00 02 01 00 02 7B F7", toWave = "F0 41 10 00 69 12 00 02 01 00 07 76 F7"
        t.equal(PanelModel.hex(PanelModel.patchMessage(output: 0, source: 2)), toInput, "OUTPUT 1-2 to INPUT 1-2 is 13 bytes, as the notes' rule gives them")
        t.equal(PanelModel.hex(PanelModel.patchMessage(output: 0, source: 7)), toWave, "OUTPUT 1-2 to WAVE OUT 1-2 is the notes' example")
        t.check(panel_patch_message_output(PanelModel.patchMessage(output: 0, source: 2), 13) == 0
                    && panel_patch_message_output(PanelModel.patchMessage(output: 4, source: 11), 13) == 4
                    && panel_patch_message_output(PanelModel.patchMessage(output: 0, source: 12), 13) == -1
                    && panel_patch_message_output(PanelModel.patchMessage(output: 5, source: 2), 13) == -1
                    && panel_patch_message_output(PanelModel.savingMessage(resume: false), 13) == -1,
                "a patch message is told by its output pair, and from everything else")

        // What a helper says it passes on, read as a list.
        let v5 = Allowed(passes: UInt64(PANEL_PASS_SAVING))
        let v6 = Allowed(passes: UInt64(PANEL_PASS_SAVING | PANEL_PASS_PATCH | PANEL_PASS_PATCH_OUTPUT_0))
        let five = Allowed(passes: UInt64(PANEL_PASS_SAVING | PANEL_PASS_PATCH) | (0x1F * UInt64(PANEL_PASS_PATCH_OUTPUT_0)))
        t.check(v5.saving && v5.patchOutputs.isEmpty && !v5.mixer, "version 5's answer: the saving only")
        t.check(v6.saving && v6.patchOutputs == [0] && !v6.allPatch && !v6.mixer && v6.patch(0) && !v6.patch(1), "version 6's: the saving and OUTPUT 1-2's source")
        t.check(five.allPatch && !five.mixer, "all five outputs: the whole patch bay, still no mixer")
        t.check(Allowed(passes: UInt64(PANEL_PASS_PATCH)).patchOutputs.isEmpty && Allowed(passes: UInt64(PANEL_PASS_PATCH_OUTPUT_0)).patchOutputs.isEmpty
                    && Allowed(passes: 0).isEmpty,
                "the patch bit without an output, or an output without the bit, allows no patch change")
        t.check(Allowed.all.mixer && Allowed.all.allPatch, "the pretend unit: everything")

        let g = Bench()
        g.fake.passes = FakeHelper.version6
        g.fake.passesRead = 0 // and, as every helper before link version 20, no memory reads
        var lines: [String] = []
        g.model.onRecord = { lines.append($0) }
        g.chooseReal()
        g.model.allowChanges(true)
        g.settle()
        t.check(g.model.snap.writesAllowed && g.model.allowed == v6, "changes on; the model's list is the helper's")
        t.check(lines.contains { $0.contains("Changes switched on") && $0.contains("OUTPUT 1-2") }, "the record says what this helper passes on")
        let reads = g.fake.passedOn
        t.equal(g.model.snap.patch[0], 7, "OUTPUT 1-2 plays WAVE OUT 1-2")
        t.equal(g.model.setPatch(output: 0, source: 2), Int32(PANEL_OK), "OUTPUT 1-2 to INPUT 1-2")
        t.check(g.model.patchNote?.contains(toInput) == true, "the note shows the bytes sent")
        g.settle()
        t.equal(unitValue(g, Int32(PANEL_F_PATCH), 0, 0, real: true), 2, "the unit has it")
        t.check(g.fake.changesPassedOn == 1 && g.fake.passedOn == reads + 2, "one change and one read went to the unit: no pause of its saving around it")
        t.check(g.model.snap.patch[0] == 2 && g.model.disagreement == nil && !g.model.snap.busy, "the panel shows it, with no disagreement, nothing waiting")
        t.check(g.model.patchNote?.contains("The unit took it: d[12] 07→02") == true && g.model.patchNote?.contains("Every other byte of the 256 read back the same") == true
                    && g.model.patchNote?.contains("said nothing else") == true,
                "the read-back: d[12] and nothing else: \(g.model.patchNote ?? "nil")")
        t.equal(g.model.setPatch(output: 0, source: 7), Int32(PANEL_OK), "and back to WAVE OUT 1-2")
        g.settle()
        t.check(unitValue(g, Int32(PANEL_F_PATCH), 0, 0, real: true) == 7 && g.model.snap.patch[0] == 7, "the unit has WAVE OUT 1-2 again")
        t.check(g.model.patchNote?.contains(toWave) == true && g.model.patchNote?.contains("The unit took it: d[12] 02→07") == true,
                "the read-back: \(g.model.patchNote ?? "nil")")
        t.check(g.fake.changesPassedOn == 2 && g.fake.passedOn == reads + 4 && panel_sim_sets_applied(g.fake.unit.sim) == 2, "two changes, two reads")
        t.check(lines.contains { $0.contains("OUTPUT 1-2 to INPUT 1-2: sent " + toInput) } && lines.contains { $0.contains("OUTPUT 1-2 to WAVE OUT 1-2: sent " + toWave) }
                    && lines.filter { $0.contains("settings before:") }.count == 2 && lines.filter { $0.contains("settings after:") }.count == 2,
                "both, with the settings before and after, are in the record (\(lines.count) lines)")
        t.equal(g.model.setPatch(output: 0, source: 7), Int32(PANEL_OK), "the same value again")
        g.settle()
        t.check(g.model.patchNote?.contains("already plays WAVE OUT 1-2") == true && g.model.patchNote?.contains("lit point") == true && g.fake.passedOn == reads + 4, "sends nothing, and says so: \(g.model.patchNote ?? "nil")")

        // The other rows, the presets and the mixer stay refused, before the engine.
        let passed = g.fake.passedOn
        t.equal(g.model.setPatch(output: 1, source: 2), Int32(PANEL_ERR_READ_ONLY), "OUTPUT 3-4: refused")
        t.check(g.model.refusal?.kind == .notPassed && g.model.refusal?.text.contains("OUTPUT 3-4: not done") == true, "with a plain sentence: \(g.model.refusal?.text ?? "nil")")
        t.equal(g.model.patchPreset(hardThru: true), Int32(PANEL_ERR_READ_ONLY), "Hard Thru: refused (it changes all five)")
        t.equal(g.model.setVolume(strip(inp, 0), minus6), Int32(PANEL_ERR_READ_ONLY), "a fader: refused")
        g.settle()
        t.check(g.fake.passedOn == passed && g.fake.refused == 0 && g.helper.blocked == 0 && g.model.guardBlocks == 0, "nothing was sent and no guard had to step in")
        t.check(PanelModel.patchMessage(output: 1, source: 2).withUnsafeBufferPointer { !g.helper.send($0) } && g.helper.blocked == 1 && g.fake.refused == 0,
                "the transport, changes on, doesn't take OUTPUT 3-4's message either")

        // The unit keeps the old value: said in capitals, and the unit wins.
        g.fake.unit.ignore(kind: Int(PANEL_F_PATCH))
        t.equal(g.model.setPatch(output: 0, source: 3), Int32(PANEL_OK), "a change the unit ignores")
        g.settle()
        t.check(g.model.patchNote?.contains("THE UNIT DID NOT TAKE IT") == true && g.model.disagreement != nil && g.model.snap.patch[0] == 7,
                "the note and the panel say so: \(g.model.patchNote ?? "nil")")
        g.model.dismissDisagreement()
        g.fake.unit.ignore(kind: -1)
        // Something else moves while the read-back is out.
        t.equal(g.model.setPatch(output: 0, source: 2), Int32(PANEL_OK), "a change while a switch is pressed on the unit")
        g.fake.unit.press(Int32(PANEL_SWITCH_MONO), in: true)
        g.settle()
        t.check(g.model.patchNote?.contains("The unit took it") == true && g.model.patchNote?.contains("OTHER SETTINGS READ BACK DIFFERENT, 1 byte: d[6]") == true,
                "the note names the other byte: \(g.model.patchNote ?? "nil")")

        // The helper refuses: the note says nothing was sent.
        g.fake.refuseChanges = "a refusal made by the test; the message was not sent"
        t.equal(g.model.setPatch(output: 0, source: 7), Int32(PANEL_OK), "a change the helper will refuse, handed over")
        g.settle()
        t.check(g.fake.refused == 1 && g.model.refusal?.kind == .helper && !g.model.snap.writesAllowed, "the helper refused it; read-only again")
        t.check(g.model.patchNote?.contains("NOT SENT") == true && unitValue(g, Int32(PANEL_F_PATCH), 0, 0, real: true) == 2 && g.model.snap.patch[0] == 2,
                "the note says so, and the panel shows what the unit holds")

        // An older helper (link version 5's answer): no patch change at all.
        let old = Bench()
        old.fake.passes = UInt64(PANEL_PASS_SAVING)
        old.chooseReal()
        old.model.allowChanges(true)
        old.settle()
        t.equal(old.model.setPatch(output: 0, source: 2), Int32(PANEL_ERR_READ_ONLY), "a helper that passes on the saving only: OUTPUT 1-2 refused")
        t.check(old.model.refusal?.text.contains("OUTPUT 1-2 (") == false && old.model.refusal?.text.contains("the unit's saving") == true, "and the sentence names the saving only")
        t.check(PanelModel.patchMessage(output: 0, source: 2).withUnsafeBufferPointer { !old.helper.send($0) }, "nor does the transport take it")
        // A helper that passes on nothing: changes can't be switched on.
        let none = Bench()
        none.fake.passes = 0
        none.chooseReal()
        none.model.allowChanges(true)
        none.settle()
        t.check(!none.model.snap.writesAllowed, "a helper that passes on nothing: changes stay off")
        // A later helper (all five outputs): every row and the presets, still no mixer.
        let later = Bench()
        later.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        later.fake.passes = UInt64(PANEL_PASS_SAVING | PANEL_PASS_PATCH) | (0x1F * UInt64(PANEL_PASS_PATCH_OUTPUT_0))
        later.chooseReal()
        later.model.allowChanges(true)
        later.settle()
        t.equal(later.model.patchPreset(hardThru: true), Int32(PANEL_OK), "a helper that passes on all five outputs: Hard Thru")
        later.settle()
        t.check((0..<5).allSatisfy { unitValue(later, Int32(PANEL_F_PATCH), 0, $0, real: true) == UInt32(2 + $0) } && later.fake.refused == 0 && later.helper.blocked == 0,
                "the unit has all five")
        t.equal(later.model.setVolume(strip(inp, 0), minus6), Int32(PANEL_ERR_READ_ONLY), "a fader is still refused")

        // The pretend unit: no note, nothing in the record.
        let p = Bench()
        var pretendLines = 0
        p.model.onRecord = { _ in pretendLines += 1 }
        p.model.allowChanges(true)
        t.equal(p.model.setPatch(output: 3, source: 5), Int32(PANEL_OK), "pretend unit: any row")
        p.settle()
        t.check(p.model.patchNote == nil && pretendLines == 0 && p.model.snap.patch[3] == 5, "no note and no record for the pretend unit")
    }

    // ---- 15d
    if t.begin("the UA-101 with changes on: INPUT 1's volume, and all five patch rows") {
        let in1 = strip(inp, 0)
        let first = "F0 41 10 00 69 12 00 04 00 07 01 00 00 00 00 01 73 F7", zeroDB = "F0 41 10 00 69 12 00 04 00 07 02 00 00 00 00 00 73 F7"
        t.equal(PanelModel.hex(PanelModel.volumeMessage(in1, 0x100001)), first, "INPUT 1's volume to 0x100001 is 18 bytes, as the notes' rule gives them")
        t.equal(PanelModel.hex(PanelModel.volumeMessage(in1, unity)), zeroDB, "INPUT 1's volume to 0 dB is the notes' example")
        t.equal(PanelModel.volumeOffset(in1), 128, "INPUT 1's volume is d[128] to d[133] of the block")
        t.check(PanelModel.volumeOffset(strip(mon, 1)) == 122 && PanelModel.volumeOffset(strip(out, 9)) == 242, "MONITOR OUT R's is at d[122], OUTPUT 10's at d[242]")
        func channel(_ s: StripID, _ v: UInt32) -> Int32 { panel_volume_message_channel(PanelModel.volumeMessage(s, v), 18) }
        t.check(channel(in1, 0x100001) == 2 && channel(in1, unity) == 2 && channel(strip(mon, 1), 0) == 1 && channel(strip(out, 9), 0x100001) == 21,
                "a volume message is told by its channel")
        t.check(channel(in1, 0x1F0000) == 2 && channel(in1, 0x000007) == 2 && channel(in1, 0x16A09E) == 2 && channel(in1, unity - 1) == 2 && channel(in1, 0) == 2,
                "at any value up to 0 dB, those link version 7 left out too")
        t.check(channel(in1, unity + 1) == 2 && channel(in1, 0x400000) == 2 && channel(in1, UInt32(PANEL_MAX_VOLUME)) == 2,
                "since 2026-11-27 above 0 dB too, up to the top of the range (0x7FFFFF, about +12 dB)")
        // Past the top, made by hand (the builder makes no such message): 0x800000.
        let pastTop: [UInt8] = [0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x07, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6D, 0xF7]
        t.check(panel_volume_message_channel(pastTop, 18) == -1
                    && panel_volume_message_channel(PanelModel.patchMessage(output: 0, source: 2), 13) == -1,
                "but not with a value past the top; nor is any other message")

        // The values the UA-101 is sent: every one up to the top, the first digit the highest,
        // and none above. Worked out here a second way, digit by digit against 7 F F F F F.
        func atMostTop(_ v: UInt32) -> Bool {
            let d = (0..<6).map { (v >> UInt32(20 - 4 * $0)) & 0xF }
            return v <= 0xFFFFFF && d[0] <= 7
        }
        var wrong = 0, good = 0
        for v in UInt32(0)...(UInt32(PANEL_MAX_VOLUME) + 0x2000) {
            let ok = panel_volume_sendable(v) != 0
            if ok { good += 1 }
            if ok != atMostTop(v) { wrong += 1 }
        }
        t.check(wrong == 0 && good == Int(PANEL_MAX_VOLUME) + 1, "every value from silence to the top (about +12 dB) is one the UA-101 is sent, \(good) of them, and none of the next 8,192 (wrong: \(wrong))")
        t.check(panel_volume_sendable(0x800000) == 0 && panel_volume_sendable(0xFFFFFF) == 0 && panel_volume_sendable(0x900000) == 0,
                "nor one step past the top, nor 0xFFFFFF")

        let v8 = Allowed(passes: FakeHelper.version8)
        t.check(v8.saving && v8.allPatch && v8.volumes == [in1] && v8.volume(in1) && !v8.volume(strip(inp, 1)) && !v8.volume(strip(out, 0))
                    && !v8.mixer && v8.someMixer,
                "version 8's answer: the saving, the whole patch bay and INPUT 1's volume; no other mixer control")
        t.check(v8.words.contains("the volume of INPUT 1") && v8.words.contains("the patch bay") && v8.short.contains("INPUT 1 volume"), "in words: \(v8.words) / \(v8.short)")
        t.check(Allowed(passes: UInt64(PANEL_PASS_VOLUME)).volumes.isEmpty && Allowed(passes: UInt64(PANEL_PASS_VOLUME_CHANNEL_0) << 2).volumes.isEmpty,
                "the volume bit without a channel, or a channel without the bit, allows no volume")

        let g = Bench()
        g.fake.passes = FakeHelper.version9 // a helper of link version 9 or 10: no on/off
        g.fake.passesRead = 0 // and, as every helper before link version 20, no memory reads
        var lines: [String] = []
        g.model.onRecord = { lines.append($0) }
        g.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        g.chooseReal()
        g.model.allowChanges(true)
        g.settle()
        let v9 = Allowed(passes: FakeHelper.version9)
        t.check(v9.saving && v9.allPatch && v9.volumes == [in1] && v9.pans == [in1] && v9.pan(in1) && !v9.pan(strip(inp, 1)) && !v9.pan(strip(out, 0))
                    && !v9.pan(strip(mon, 0)) && !v9.mixer && v9.someMixer && !v8.pan(in1) && v8.pans.isEmpty,
                "version 9's answer: as version 8's, and INPUT 1's pan; version 8's has no pan")
        t.check(v9.words.hasSuffix("the volume of INPUT 1 (up to +12 dB) and the pan of INPUT 1") && !v9.words.contains("in steps") && v9.short.contains("INPUT 1 volume and pan"), "in words: \(v9.words) / \(v9.short)")
        t.check(Allowed(passes: UInt64(PANEL_PASS_PAN)).pans.isEmpty && Allowed(passes: UInt64(PANEL_PASS_PAN_CHANNEL_0) << 2).pans.isEmpty
                    && Allowed(passes: UInt64(PANEL_PASS_PAN) | UInt64(PANEL_PASS_PAN_CHANNEL_0)).pans.isEmpty,
                "the pan bit without a channel, a channel without the bit, or MONITOR OUT's number, allows no pan")
        t.check(g.model.snap.writesAllowed && g.model.allowed == v9, "changes on; the model's list is the helper's")
        t.check(lines.contains { $0.contains("Changes switched on") && $0.contains("the volume of INPUT 1") }, "the record says what this helper passes on")
        var reads = g.fake.passedOn
        t.equal(g.model.snap.channels[inp][0].volume, unity, "INPUT 1 is at 0 dB")

        // The first volume write: one exact value, once.
        t.equal(g.model.setVolumeExact(in1, 0x100001), Int32(PANEL_OK), "INPUT 1 to exactly 0x100001")
        t.check(g.model.volumeNote?.contains("INPUT 1 volume: −6.0 dB (0x100001) asked for") == true && g.model.volumeNote?.contains("F0") == false,
                "the note says what was asked for, and no bytes until they were read back: \(g.model.volumeNote ?? "nil")")
        g.settle()
        t.check(g.fake.changes.map(PanelModel.hex) == [first] && g.fake.passedOn == reads + 2,
                "one message and one read went to the unit: no pause of its saving around it")
        t.equal(unitValue(g, Int32(PANEL_F_VOLUME), inp, 0, real: true), 0x100001, "the unit has it")
        t.check(g.model.snap.channels[inp][0].volume == 0x100001 && g.model.disagreement == nil && !g.model.snap.busy, "the panel shows it, with no disagreement, nothing waiting")
        t.check(g.model.volumeNote?.contains("The unit took it: d[128…133] 02 00 00 00 00 00 → 01 00 00 00 00 01") == true
                    && g.model.volumeNote?.contains("Every other byte of the 256 read back the same") == true && g.model.volumeNote?.contains("said nothing else") == true,
                "the read-back: the six bytes and nothing else: \(g.model.volumeNote ?? "nil")")
        t.equal(g.model.setVolumeExact(in1, unity), Int32(PANEL_OK), "and back to exactly 0 dB")
        g.settle()
        t.check(g.fake.changes.map(PanelModel.hex) == [first, zeroDB] && g.fake.passedOn == reads + 4 && unitValue(g, Int32(PANEL_F_VOLUME), inp, 0, real: true) == unity,
                "one more message and read; the unit is at 0 dB again")
        t.check(g.model.volumeNote?.contains("The unit took it: d[128…133] 01 00 00 00 00 01 → 02 00 00 00 00 00") == true, "the read-back: \(g.model.volumeNote ?? "nil")")
        t.check(lines.contains("INPUT 1 volume: −6.0 dB (0x100001) asked for") && lines.contains("INPUT 1 volume: 0.0 dB (0x200000) asked for")
                    && lines.filter { $0.hasPrefix("  handed to the helper: ") } == ["  handed to the helper: " + first, "  handed to the helper: " + zeroDB]
                    && lines.contains { $0.contains("INPUT 1 volume to −6.0 dB (0x100001): sent " + first) } && lines.contains { $0.contains("(0x200000): sent " + zeroDB) }
                    && lines.filter { $0.contains("settings before:") }.count == 2 && lines.filter { $0.contains("settings after:") }.count == 2,
                "both are in the record: what was asked for, each message as it was handed to the helper, the settings before and after (\(lines.count) lines)")
        t.equal(g.model.setVolumeExact(in1, unity), Int32(PANEL_OK), "the same value again")
        g.settle()
        t.check(g.model.volumeNote?.contains("already has that value") == true && g.fake.changes.count == 2, "sends nothing, and says so")
        t.equal(g.model.setVolumeExact(in1, UInt32(PANEL_MAX_VOLUME) + 1), Int32(PANEL_ERR_VALUE), "an exact value one step past the top of the range: refused")
        t.check(g.model.refusal?.text.contains("0x800000") == true && g.model.refusal?.text.contains("Nothing was sent") == true, "with a plain sentence: \(g.model.refusal?.text ?? "nil")")
        let minus3 = "F0 41 10 00 69 12 00 04 00 07 01 06 0A 00 09 0E 4D F7"
        t.equal(g.model.setVolumeExact(in1, 0x16A09E), Int32(PANEL_OK), "INPUT 1 to exactly 0x16A09E, about −3 dB, which link version 7 didn't pass on")
        g.settle()
        t.check(g.fake.changes.last.map(PanelModel.hex) == minus3 && unitValue(g, Int32(PANEL_F_VOLUME), inp, 0, real: true) == 0x16A09E
                    && g.model.volumeNote?.contains("The unit took it: d[128…133] 02 00 00 00 00 00 → 01 06 0A 00 09 0E") == true
                    && g.model.volumeNote?.contains("Every other byte of the 256 read back the same") == true,
                "one message, \(minus3), and the read-back: \(g.model.volumeNote ?? "nil")")
        t.equal(g.model.setVolumeExact(in1, unity), Int32(PANEL_OK), "and 0 dB again")
        g.settle()
        t.check(g.fake.changes.count == 4 && g.fake.refused == 0 && g.helper.blocked == 0 && g.model.guardBlocks == 0, "four changes so far, and no guard had to step in")

        // A drag: the unit's saving is paused around it, the values are spaced out, one read.
        reads = g.fake.passedOn - g.fake.changesPassedOn
        var before = g.fake.changes.count
        g.model.beginDrag(volumeOf: in1)
        for i in 0..<300 { // 1.5 s of mouse, a value every 5 ms, from 0 dB down to −30 dB
            g.model.setVolume(in1, FaderScale.volume(dB: -Double(i) / 10))
            g.settle(5_000)
        }
        let lastAsked = FaderScale.volume(dB: -29.9)
        let asked = Set((0..<300).map { PanelModel.hex(PanelModel.volumeMessage(in1, FaderScale.volume(dB: -Double($0) / 10))) })
        g.model.endDrag()
        g.settle()
        var drag = Array(g.fake.changes[before...])
        t.check(drag.count >= 3 && PanelModel.hex(drag.first!) == "F0 41 10 00 69 12 00 00 00 00 00 00 F7" && PanelModel.hex(drag.last!) == "F0 41 10 00 69 12 00 00 00 00 01 7F F7",
                "a drag: the first message pauses the unit's saving and the last resumes it (\(drag.count) messages)")
        t.check(drag.dropFirst().dropLast().allSatisfy { panel_volume_message_channel($0, $0.count) == 2 }, "all between are INPUT 1 volumes the helper passes on")
        t.check(drag.count <= 30 && g.fake.tooMany == 0 && g.fake.refused == 0, "spaced out: \(drag.count) messages for 300 values in 1.5 s, never more than the helper's 20 a second")
        let dragVolumes = drag.dropFirst().dropLast().map(PanelModel.hex)
        t.check(dragVolumes.count >= 20 && Set(dragVolumes).count == dragVolumes.count && dragVolumes.allSatisfy { asked.contains($0) },
                "smooth: \(dragVolumes.count) volumes, each a value the fader asked for as it was asked, no two the same")
        t.check(PanelModel.hex(drag[drag.count - 2]) == PanelModel.hex(PanelModel.volumeMessage(in1, lastAsked))
                    && unitValue(g, Int32(PANEL_F_VOLUME), inp, 0, real: true) == lastAsked,
                "the last value asked for is the last sent, and the unit has it")
        t.equal(g.fake.passedOn - g.fake.changesPassedOn, reads + 1, "one read after the drag")
        t.check(panel_sim_saving_paused(g.fake.unit.sim) == 0 && !g.model.snap.busy && g.model.disagreement == nil, "the unit saves again; nothing waits; no disagreement")
        t.check(g.model.volumeNote?.contains("the last of 299 values asked for") == true && g.model.volumeNote?.contains("The unit took it") == true
                    && g.model.volumeNote?.contains("Every other byte") == true,
                "the note: \(g.model.volumeNote ?? "nil")")
        t.check(lines.filter { $0.contains("INPUT 1 volume") && !$0.hasPrefix("  ") }.count <= 8, "a drag writes its first value and its outcome to the record, not 300 lines")
        t.check(lines.filter { $0.hasPrefix("  handed to the helper: ") }.suffix(drag.count) == drag.map { "  handed to the helper: " + PanelModel.hex($0) },
                "and a line for each message as it left, in the order the helper got them")

        // The first value of a drag that never leaves (as on the unit, 2026-11-12):
        // a newer value takes its place before the pause of the saving is out of the way.
        let mark = lines.count
        let never = FaderScale.volume(dB: -10), newer = FaderScale.volume(dB: -11.3)
        before = g.fake.changes.count
        g.model.beginDrag(volumeOf: in1)
        g.model.setVolume(in1, never)
        g.settle(10_000)
        g.model.setVolume(in1, newer)
        g.settle(500_000)
        g.model.endDrag()
        g.settle()
        let neverBytes = PanelModel.hex(PanelModel.volumeMessage(in1, never)), newerBytes = PanelModel.hex(PanelModel.volumeMessage(in1, newer))
        let fresh = Array(lines[mark...])
        t.check(g.fake.changes[before...].map(PanelModel.hex) == ["F0 41 10 00 69 12 00 00 00 00 00 00 F7", newerBytes, "F0 41 10 00 69 12 00 00 00 00 01 7F F7"],
                "a drag whose first value is overtaken: the pause, the newer value, the resume; the first value never left")
        t.check(fresh.first?.hasPrefix("INPUT 1 volume: −10.0 dB (0x\(String(format: "%06X", never))) asked for, the first value of a drag") == true,
                "the record's first line says it was asked for: \(fresh.first ?? "nil")")
        t.check(!fresh.contains { $0.contains(neverBytes) }, "and nowhere prints bytes for it: they never left")
        t.check(fresh.filter { $0.hasPrefix("  handed to the helper: ") }.map { String($0.dropFirst(24)) } == g.fake.changes[before...].map(PanelModel.hex),
                "the lines for what left are exactly the three messages the helper got")
        t.equal(g.model.setVolumeExact(in1, unity), Int32(PANEL_OK), "back to 0 dB")
        g.settle()

        // Above 0 dB (since 2026-11-27): as it is, and the helper passes it on.
        t.equal(g.model.setVolume(in1, FaderScale.volume(dB: 6)), Int32(PANEL_OK), "the fader pushed to +6 dB")
        g.settle()
        t.check(unitValue(g, Int32(PANEL_F_VOLUME), inp, 0, real: true) == FaderScale.volume(dB: 6) && g.model.refusal == nil && g.fake.refused == 0,
                "the unit is at +6 dB, nothing held or refused")
        t.check(g.fake.changes.allSatisfy { $0.count != 18 || panel_volume_message_channel($0, 18) == 2 }, "every volume that ever left was INPUT 1's, at most the top of the range")
        t.equal(g.model.setVolumeExact(in1, unity), Int32(PANEL_OK), "and back to 0 dB")
        g.settle()

        // The keys: a step down, and a step up.
        g.model.cursor = in1
        g.model.handle(.volumeDown)
        g.settle()
        let down = unitValue(g, Int32(PANEL_F_VOLUME), inp, 0, real: true)
        t.check(down < unity && down == FaderScale.volume(dB: -0.5), "a key step down from 0 dB: −0.5 dB as it is (\(Format.dB(down)) dB)")
        g.model.handle(.volumeUp)
        g.settle()
        t.equal(unitValue(g, Int32(PANEL_F_VOLUME), inp, 0, real: true), unity, "a key step up from there: 0 dB")
        t.check(panel_sim_saving_paused(g.fake.unit.sim) == 0 && g.fake.tooMany == 0 && g.fake.refused == 0, "the saving was paused around the keys and resumed")

        // Every other mixer control stays refused, before the engine.
        var passed = g.fake.passedOn
        t.equal(g.model.setVolume(strip(inp, 1), minus6), Int32(PANEL_ERR_READ_ONLY), "INPUT 2's fader: refused")
        t.check(g.model.refusal?.kind == .notPassed && g.model.refusal?.text.contains("INPUT 2 volume: not done") == true && g.model.refusal?.text.contains("the volume of INPUT 1") == true,
                "with a plain sentence: \(g.model.refusal?.text ?? "nil")")
        t.equal(g.model.setVolume(strip(out, 0), minus6), Int32(PANEL_ERR_READ_ONLY), "OUTPUT 1's fader: refused")
        t.equal(g.model.setVolume(strip(mon, 0), minus6), Int32(PANEL_ERR_READ_ONLY), "MONITOR OUT's fader: refused")
        t.equal(g.model.setPan(strip(inp, 1), 0x2000), Int32(PANEL_ERR_READ_ONLY), "INPUT 2's pan: refused")
        t.equal(g.model.setPan(strip(out, 0), 0x2000), Int32(PANEL_ERR_READ_ONLY), "OUTPUT 1's pan: refused")
        t.equal(g.model.setMonitor(in1, on: false), Int32(PANEL_ERR_READ_ONLY), "on/off: refused")
        t.equal(g.model.setLink(group: inp, pair: 0, on: true), Int32(PANEL_ERR_READ_ONLY), "link: refused")
        t.equal(g.model.resetMixer(), Int32(PANEL_ERR_READ_ONLY), "Reset Mixer: refused")
        t.equal(g.model.prepareMemory(MemoryCommand(kind: .resetAll)), Int32(PANEL_ERR_READ_ONLY), "Reset: refused")
        g.model.beginDrag(panOf: strip(inp, 1)) // another channel's pan knob
        g.model.setPan(strip(inp, 1), 0x1000)
        g.model.endDrag()
        g.model.beginDrag(volumeOf: strip(inp, 1)) // another fader
        g.model.setVolume(strip(inp, 1), minus12)
        g.model.endDrag()
        g.model.cursor = strip(inp, 1)
        g.model.handle(.panLeft)
        g.model.handle(.toggleMonitor)
        g.model.cursor = in1
        g.settle(5_000_000)
        t.check(g.fake.passedOn == passed && g.fake.refused == 0 && g.helper.blocked == 0 && g.model.guardBlocks == 0,
                "none of that sent anything, not even a pause for the drags, and no guard had to step in")
        // The transport's own rule, with changes on.
        // Past the top, made by hand (the builder makes none): 0x800000 and 0x900000.
        let tooHigh = pastTop, higher: [UInt8] = [0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x04, 0x00, 0x07, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6C, 0xF7]
        let input2 = PanelModel.volumeMessage(strip(inp, 1), 0x100001)
        let pan = PanelModel.panMessage(strip(inp, 1), 0x4000)
        var odd = PanelModel.panMessage(in1, 0x8000) // made by hand: 0x8001, one past the end
        odd[13] = 1
        odd[14] = (odd[14] &+ 0x7F) & 0x7F
        t.check(tooHigh.withUnsafeBufferPointer { !g.helper.send($0) } && higher.withUnsafeBufferPointer { !g.helper.send($0) }
                    && input2.withUnsafeBufferPointer { !g.helper.send($0) } && pan.withUnsafeBufferPointer { !g.helper.send($0) }
                    && odd.withUnsafeBufferPointer { !g.helper.send($0) }
                    && g.helper.blocked == 5 && g.fake.refused == 0,
                "the transport takes no volume past the top of the range, no other channel's, no other channel's pan, and no pan past the far end (0x8001)")

        // All five patch rows now, and the presets.
        before = g.fake.changes.count
        t.equal(g.model.setPatch(output: 1, source: 3), Int32(PANEL_OK), "OUTPUT 3-4 to INPUT 3-4")
        g.settle()
        t.check(g.fake.changes.count == before + 1 && PanelModel.hex(g.fake.changes.last!) == "F0 41 10 00 69 12 00 02 01 01 03 79 F7"
                    && g.model.patchNote?.contains("The unit took it: d[13] 08→03") == true && g.model.patchNote?.contains("Every other byte") == true,
                "one message, and d[13] alone moved: \(g.model.patchNote ?? "nil")")
        t.equal(g.model.setPatch(output: 1, source: 8), Int32(PANEL_OK), "and back to WAVE OUT 3-4")
        g.settle()
        t.check(PanelModel.hex(g.fake.changes.last!) == "F0 41 10 00 69 12 00 02 01 01 08 74 F7" && g.model.patchNote?.contains("d[13] 03→08") == true, "read back: \(g.model.patchNote ?? "nil")")
        t.equal(g.model.setPatch(output: 4, source: 1), Int32(PANEL_OK), "OUTPUT 9-10 to MONITOR OUT")
        g.settle()
        t.equal(g.model.patchPreset(hardThru: true), Int32(PANEL_OK), "Hard Thru")
        g.settle()
        t.check((0..<5).allSatisfy { unitValue(g, Int32(PANEL_F_PATCH), 0, $0, real: true) == UInt32(2 + $0) }, "the unit has all five")
        t.equal(g.model.patchPreset(hardThru: false), Int32(PANEL_OK), "Initial Setting")
        g.settle()
        t.check((0..<5).allSatisfy { unitValue(g, Int32(PANEL_F_PATCH), 0, $0, real: true) == UInt32(7 + $0) } && g.fake.refused == 0 && g.fake.tooMany == 0 && g.model.disagreement == nil,
                "and Initial Setting again; nothing refused")

        // The unit keeps another value: said in capitals, and the unit wins.
        g.fake.unit.ignore(kind: Int(PANEL_F_VOLUME))
        t.equal(g.model.setVolumeExact(in1, 0x100001), Int32(PANEL_OK), "a volume the unit ignores")
        g.settle()
        t.check(g.model.volumeNote?.contains("THE UNIT HOLDS ANOTHER VALUE: d[128…133] 02 00 00 00 00 00 → 02 00 00 00 00 00, wanted 01 00 00 00 00 01") == true
                    && g.model.disagreement != nil && g.model.snap.channels[inp][0].volume == unity,
                "the note and the panel say so: \(g.model.volumeNote ?? "nil")")
        g.model.dismissDisagreement()
        g.fake.unit.ignore(kind: -1)

        // A broken app that hands the helper a volume it must not pass on: refused there.
        passed = g.fake.changesPassedOn
        t.check(higher.withUnsafeBufferPointer { g.fake.send($0) }, "handed to the helper directly, past the app's own checks")
        g.settle()
        t.check(g.fake.refused == 1 && g.fake.changesPassedOn == passed && g.model.refusal?.kind == .helper && !g.model.snap.writesAllowed,
                "the helper refused it, nothing reached the unit, and the panel is read-only again")

        // The helper refuses in the middle of a drag.
        let d = Bench()
        d.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        d.chooseReal()
        d.model.allowChanges(true)
        d.settle()
        d.model.beginDrag(volumeOf: in1)
        d.model.setVolume(in1, 0x100000)
        d.settle(200_000)
        d.fake.refuseChanges = "a refusal made by the test; the message was not sent"
        d.model.setVolume(in1, 0x010000)
        d.settle(200_000)
        d.model.endDrag()
        d.settle()
        t.check(d.fake.refused >= 1 && d.model.refusal?.kind == .helper && !d.model.snap.writesAllowed && !d.model.snap.busy,
                "a refusal in mid-drag: the helper refuses, the panel is read-only, nothing waits")
        t.check(d.model.volumeNote?.contains("refused") == true && d.model.snap.channels[inp][0].volume == unitValue(d, Int32(PANEL_F_VOLUME), inp, 0, real: true),
                "the note says so, and the panel shows what the unit holds (\(Format.dB(d.model.snap.channels[inp][0].volume)) dB)")
        let quiet = d.fake.passedOn + d.fake.refused
        d.settle(3_600_000_000)
        t.check(d.fake.passedOn + d.fake.refused == quiet, "and it doesn't try again by itself")
        drag = d.fake.changes
        t.check(drag.count == 2 && panel_is_saving_message(drag[0], drag[0].count) != 0, "what had left: the pause and the first value. The resume was refused with the rest")

        // SOFT CTRL out on the UA-101: the fader is refused as on the pretend unit.
        let soft = Bench()
        soft.chooseReal()
        soft.model.allowChanges(true)
        soft.settle()
        t.equal(soft.model.setVolumeExact(in1, 0x100001), Int32(PANEL_ERR_SOFT_CTRL), "SOFT CTRL out: the volume isn't sent")
        t.check(soft.fake.changesPassedOn == 0 && soft.model.volumeNote == nil, "nothing left, and no note")

        // The pretend unit: every value, no note, nothing in the record.
        let p = Bench()
        var pretendLines = 0
        p.model.onRecord = { _ in pretendLines += 1 }
        p.openUp()
        t.equal(p.model.setVolume(in1, 0x1F0000), Int32(PANEL_OK), "pretend unit: any value")
        p.settle()
        t.check(p.model.volumeNote == nil && pretendLines == 0 && unitValue(p, Int32(PANEL_F_VOLUME), inp, 0) == 0x1F0000, "as asked, no note and no record")
    }

    // ---- 15f
    if t.begin("the UA-101 with changes on: INPUT 1's pan") {
        let in1 = strip(inp, 0)
        let middle = "F0 41 10 00 69 12 00 04 00 03 04 00 00 00 75 F7", leftEnd = "F0 41 10 00 69 12 00 04 00 03 00 00 00 00 79 F7"
        t.equal(PanelModel.hex(PanelModel.panMessage(in1, 0x4000)), middle, "INPUT 1's pan to the middle is 16 bytes, as the notes' rule gives them")
        t.equal(PanelModel.hex(PanelModel.panMessage(in1, 0)), leftEnd, "and to 0x0000")
        t.equal(PanelModel.hex(PanelModel.panMessage(strip(out, 0), 0x4000)), "F0 41 10 00 69 12 00 05 00 03 04 00 00 00 74 F7", "OUTPUT 1's pan to the middle is the notes' example")
        t.check(PanelModel.panOffset(in1) == 36 && PanelModel.panOffset(strip(inp, 9)) == 72 && PanelModel.panOffset(strip(out, 0)) == 76,
                "INPUT 1's pan is d[36] to d[39] of the block, INPUT 10's at d[72], OUTPUT 1's at d[76]")
        func channel(_ s: StripID, _ v: UInt32) -> Int32 { panel_pan_message_channel(PanelModel.panMessage(s, v), 16) }
        t.check(channel(in1, 0x4000) == 2 && channel(in1, 0) == 2 && channel(in1, 0x8000) == 2 && channel(in1, 0x7777) == 2 && channel(strip(out, 9), 0x1234) == 21,
                "a pan message is told by its channel")
        var over = PanelModel.panMessage(in1, 0x8000) // made by hand: 0x8001, one past the end
        over[13] = 1
        over[14] = (over[14] &+ 0x7F) & 0x7F
        t.check(channel(in1, 0x0009) == 2 && channel(in1, 0x4800) == 2 && channel(in1, 0x7FFF) == 2 && channel(in1, 0x4ABC) == 2,
                "every value up to the far end is one too (0x0009, 0x4800, 0x7FFF, 0x4ABC: version 9 refused them)")
        t.check(panel_pan_message_channel(over, 16) == -1 && panel_pan_message_channel(PanelModel.volumeMessage(in1, 0x100001), 18) == -1,
                "but not 0x8001, one past the end; nor any other message")

        // The values the UA-101 is sent, worked out here another way: the four digits, the
        // first sent the highest (read back from the unit, 2026-11-12), are below 8 0 0 0 at
        // their first difference from it, or are it.
        func inRange(_ v: UInt32) -> Bool {
            let d = (0..<4).map { (v >> UInt32(12 - 4 * $0)) & 0xF }
            return d[0] < 8 || d == [8, 0, 0, 0]
        }
        var wrong = 0, good = 0
        for v in UInt32(0)...0xFFFF {
            let ok = panel_pan_sendable(v) != 0
            if ok { good += 1 }
            if ok != inRange(v) { wrong += 1 }
        }
        t.check(wrong == 0 && good == 0x8001, "of the 65,536 values four digits spell, \(good) are ones the UA-101 is sent: every one from 0x0000 to 0x8000 (wrong: \(wrong))")

        let g = Bench()
        var lines: [String] = []
        g.model.onRecord = { lines.append($0) }
        g.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        g.chooseReal()
        g.model.allowChanges(true)
        g.settle()
        t.check(lines.contains { $0.contains("Changes switched on") && $0.contains("the pan of every INPUT and OUTPUT channel") && !$0.contains("in steps") }, "the record says what this helper passes on")
        var reads = g.fake.passedOn
        t.check(g.model.snap.channels[inp][0].pan == 0 && g.model.snap.channels[inp][1].pan == 0x8000, "INPUT 1's pan reads 0x0000 and INPUT 2's 0x8000, as on the real unit")

        // The first pan write: one exact value, once.
        t.equal(g.model.setPanExact(in1, 0x4000), Int32(PANEL_OK), "INPUT 1's pan to exactly 0x4000, the middle")
        t.check(g.model.panNote?.contains("INPUT 1 pan: C (0x4000) asked for") == true, "the note says what was asked for: \(g.model.panNote ?? "nil")")
        g.settle()
        t.check(g.fake.changes.map(PanelModel.hex) == [middle] && g.fake.passedOn == reads + 2,
                "one message and one read went to the unit: no pause of its saving around it")
        t.equal(unitValue(g, Int32(PANEL_F_PAN), inp, 0, real: true), 0x4000, "the unit has it")
        t.check(g.model.snap.channels[inp][0].pan == 0x4000 && g.model.disagreement == nil && !g.model.snap.busy, "the panel shows it, with no disagreement, nothing waiting")
        t.check(g.model.panNote?.contains("INPUT 1 pan to C (0x4000): sent " + middle + ". The unit took it: d[36…39] 00 00 00 00 → 04 00 00 00.") == true
                    && g.model.panNote?.contains("Every other byte of the 256 read back the same") == true && g.model.panNote?.contains("said nothing else") == true,
                "the read-back: the four bytes and nothing else: \(g.model.panNote ?? "nil")")
        t.equal(g.model.setPanExact(in1, 0x1234), Int32(PANEL_OK), "to exactly 0x1234, a different digit in every place")
        g.settle()
        t.check(g.model.panNote?.contains("The unit took it: d[36…39] 04 00 00 00 → 01 02 03 04") == true, "the read-back: \(g.model.panNote ?? "nil")")
        t.equal(g.model.setPanExact(in1, 0), Int32(PANEL_OK), "and back to exactly 0x0000")
        g.settle()
        t.check(g.fake.changes.map(PanelModel.hex) == [middle, "F0 41 10 00 69 12 00 04 00 03 01 02 03 04 6F F7", leftEnd] && g.fake.passedOn == reads + 6
                    && unitValue(g, Int32(PANEL_F_PAN), inp, 0, real: true) == 0,
                "three messages and three reads in all; the unit is at 0x0000 again")
        t.check(g.model.panNote?.contains("The unit took it: d[36…39] 01 02 03 04 → 00 00 00 00") == true, "the read-back: \(g.model.panNote ?? "nil")")
        t.check(lines.contains("INPUT 1 pan: C (0x4000) asked for")
                    && lines.filter { $0.hasPrefix("  handed to the helper: ") }.map { String($0.dropFirst(24)) } == g.fake.changes.map(PanelModel.hex)
                    && lines.filter { $0.contains("settings before:") }.count == 3 && lines.filter { $0.contains("settings after:") }.count == 3,
                "all three are in the record: what was asked for, each message as it left, the settings before and after (\(lines.count) lines)")
        t.equal(g.model.setPanExact(in1, 0), Int32(PANEL_OK), "the same value again")
        g.settle()
        t.check(g.model.panNote?.contains("already has that value") == true && g.fake.changes.count == 3, "sends nothing, and says so")
        t.equal(g.model.setPanExact(in1, 0x8001), Int32(PANEL_ERR_VALUE), "an exact value past the far end (0x8001): refused")
        t.check(g.model.refusal?.text.contains("0x8001") == true && g.model.refusal?.text.contains("Nothing was sent") == true && g.fake.changes.count == 3,
                "with a plain sentence, and nothing left: \(g.model.refusal?.text ?? "nil")")

        // A drag of the knob: the unit's saving is paused around it, the values are spaced out
        // and each is sent as asked, none moved to a step, one read.
        reads = g.fake.passedOn - g.fake.changesPassedOn
        let before = g.fake.changes.count
        g.model.beginDrag(panOf: in1)
        for i in 0..<200 { // a second of mouse, a value every 5 ms, from the left end to past the middle
            g.model.setPan(in1, UInt32(i) * 0x60)
            g.settle(5_000)
        }
        let lastAsked = UInt32(199 * 0x60)
        g.model.endDrag()
        g.settle()
        let drag = Array(g.fake.changes[before...])
        t.check(drag.count >= 3 && PanelModel.hex(drag.first!) == "F0 41 10 00 69 12 00 00 00 00 00 00 F7" && PanelModel.hex(drag.last!) == "F0 41 10 00 69 12 00 00 00 00 01 7F F7",
                "a drag of the knob: the first message pauses the unit's saving and the last resumes it (\(drag.count) messages)")
        t.check(drag.dropFirst().dropLast().allSatisfy { panel_pan_message_channel($0, $0.count) == 2 }, "all between are INPUT 1 pans the helper passes on")
        t.check(drag.count >= 8 && drag.count <= 22 && g.fake.tooMany == 0 && g.fake.refused == 0 && g.helper.blocked == 0,
                "spaced out: \(drag.count) messages for 200 values in a second, never more than the helper's 20 a second")
        t.check(PanelModel.hex(drag[drag.count - 2]) == PanelModel.hex(PanelModel.panMessage(in1, lastAsked))
                    && unitValue(g, Int32(PANEL_F_PAN), inp, 0, real: true) == lastAsked,
                "the last value asked for (0x4AA0, which version 9 would have moved to a step) is the last sent as it is, and the unit has it")
        t.equal(g.fake.passedOn - g.fake.changesPassedOn, reads + 1, "one read after the drag")
        t.check(panel_sim_saving_paused(g.fake.unit.sim) == 0 && !g.model.snap.busy && g.model.disagreement == nil, "the unit saves again; nothing waits; no disagreement")
        t.check(g.model.panNote?.contains("values asked for") == true && g.model.panNote?.contains("The unit took it") == true && g.model.panNote?.contains("Every other byte") == true,
                "the note: \(g.model.panNote ?? "nil")")
        t.check(g.model.volumeNote == nil && unitValue(g, Int32(PANEL_F_VOLUME), inp, 0, real: true) == unity, "INPUT 1's volume was left alone")

        // The keys: to the middle, a step left, a step right; never stuck on a value.
        g.model.cursor = in1
        g.model.handle(.panCenter)
        g.settle()
        t.equal(unitValue(g, Int32(PANEL_F_PAN), inp, 0, real: true), 0x4000, "the centre key: the middle")
        g.model.handle(.panLeft)
        g.settle()
        let leftStep = unitValue(g, Int32(PANEL_F_PAN), inp, 0, real: true)
        t.check(leftStep < 0x4000 && panel_pan_sendable(leftStep) != 0, "a key step left from the middle: \(String(format: "0x%04X", leftStep)), a value the helper passes on")
        g.model.handle(.panRight)
        g.settle()
        t.equal(unitValue(g, Int32(PANEL_F_PAN), inp, 0, real: true), 0x4000, "a key step right from there: the middle again")
        var stuck = 0, last = UInt32(0x4000)
        for _ in 0..<40 {
            g.model.handle(.panRight)
            g.settle()
            let now = unitValue(g, Int32(PANEL_F_PAN), inp, 0, real: true)
            if now == last && now != 0x8000 { stuck += 1 }
            last = now
        }
        t.check(stuck == 0 && last == 0x8000, "the right key held: every step moves, up to the far end (0x8000)")
        t.check(panel_sim_saving_paused(g.fake.unit.sim) == 0 && g.fake.tooMany == 0 && g.fake.refused == 0 && g.helper.blocked == 0 && g.model.guardBlocks == 0,
                "the saving was paused around the keys and resumed; nothing refused; no guard had to step in")
        t.check(g.fake.changes.allSatisfy { $0.count != 16 || panel_pan_message_channel($0, 16) == 2 }, "every pan that ever left was INPUT 1's, with a value the helper passes on")

        // The unit keeps another value: said in capitals, and the unit wins.
        g.model.setPanExact(in1, 0)
        g.settle()
        g.fake.unit.ignore(kind: Int(PANEL_F_PAN))
        t.equal(g.model.setPanExact(in1, 0x4000), Int32(PANEL_OK), "a pan the unit ignores")
        g.settle()
        t.check(g.model.panNote?.contains("THE UNIT HOLDS ANOTHER VALUE: d[36…39] 00 00 00 00 → 00 00 00 00, wanted 04 00 00 00") == true
                    && g.model.disagreement != nil && g.model.snap.channels[inp][0].pan == 0,
                "the note and the panel say so: \(g.model.panNote ?? "nil")")
        g.model.dismissDisagreement()
        g.fake.unit.ignore(kind: -1)
        // If the unit read a pan's four digits another way, the note would name the way.
        t.check(PanelModel.panOrder(sent: [4, 0, 0, 0], held: [0, 0, 0, 4]) == "BACKWARDS"
                    && PanelModel.panOrder(sent: [4, 0, 0, 0], held: [0, 4, 0, 0])?.hasPrefix("WITH THE TWO DIGITS") == true
                    && PanelModel.panOrder(sent: [4, 0, 0, 0], held: [0, 0, 4, 0])?.hasPrefix("BACKWARDS, WITH") == true
                    && PanelModel.panOrder(sent: [4, 0, 0, 0], held: [4, 0, 0, 0]) == nil && PanelModel.panOrder(sent: [4, 0, 0, 0], held: [0, 0, 0, 0]) == nil
                    && PanelModel.panOrder(sent: [1, 2, 3, 4], held: [4, 3, 2, 1]) == "BACKWARDS",
                "a read-back in another order of the four digits is named: backwards, pairs changed over, or both")

        // An older helper (link version 8) behind the same app: the pan stays refused.
        let old = Bench()
        old.fake.passes = FakeHelper.version8
        old.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        old.chooseReal()
        old.model.allowChanges(true)
        old.settle()
        t.equal(old.model.setPan(in1, 0x4000), Int32(PANEL_ERR_READ_ONLY), "with a version 8 helper INPUT 1's pan is refused before the engine")
        old.model.beginDrag(panOf: in1)
        old.model.endDrag()
        old.settle()
        t.check(old.fake.changesPassedOn == 0 && old.fake.refused == 0, "and nothing was sent, not even a pause")

        // SOFT CTRL out: refused as on the pretend unit. The pretend unit: any value, no note.
        let soft = Bench()
        soft.chooseReal()
        soft.model.allowChanges(true)
        soft.settle()
        t.equal(soft.model.setPanExact(in1, 0x4000), Int32(PANEL_ERR_SOFT_CTRL), "SOFT CTRL out: the pan isn't sent")
        t.check(soft.fake.changesPassedOn == 0 && soft.model.panNote == nil, "nothing left, and no note")
        let p = Bench()
        var pretendLines = 0
        p.model.onRecord = { _ in pretendLines += 1 }
        p.openUp()
        t.equal(p.model.setPan(in1, 0x3BBB), Int32(PANEL_OK), "pretend unit: any pan")
        p.settle()
        t.check(p.model.panNote == nil && pretendLines == 0 && unitValue(p, Int32(PANEL_F_PAN), inp, 0) == 0x3BBB, "as asked (0x3BBB), no note and no record")
    }

    // ---- 15f2
    if t.begin("the UA-101 with changes on: INPUT 1's on/off (link version 11)") {
        let in1 = strip(inp, 0)
        let one = "F0 41 10 00 69 12 00 04 00 02 01 79 F7", zero = "F0 41 10 00 69 12 00 04 00 02 00 7A F7"
        t.equal(PanelModel.hex(PanelModel.onMessage(in1, bit: 1)), one, "INPUT 1's on/off bit to 1 is 13 bytes, as the notes' rule gives them")
        t.equal(PanelModel.hex(PanelModel.onMessage(in1, bit: 0)), zero, "and to 0")
        t.check(PanelModel.onMessage(in1, bit: 2).allSatisfy { $0 == 0 }, "there is no message for a bit 2")
        t.check(PanelModel.onPlace(in1) == (30, 0) && PanelModel.onPlace(strip(inp, 1)) == (30, 1) && PanelModel.onPlace(strip(inp, 4)) == (31, 0)
                    && PanelModel.onPlace(strip(mon, 0)) == (29, 0) && PanelModel.onPlace(strip(out, 0)) == (33, 0),
                "INPUT 1's bit is d[30] bit 0; INPUT 2's d[30] bit 1, INPUT 5's d[31] bit 0, MONITOR OUT L's d[29], OUTPUT 1's d[33]")
        func channel(_ bytes: [UInt8]) -> Int32 { panel_on_message_channel(bytes, bytes.count) }
        var two = PanelModel.onMessage(in1, bit: 1) // made by hand: the byte 2
        two[10] = 2
        two[11] = (two[11] &+ 0x7F) & 0x7F
        t.check(channel(PanelModel.onMessage(in1, bit: 1)) == 2 && channel(PanelModel.onMessage(in1, bit: 0)) == 2
                    && channel(PanelModel.onMessage(strip(out, 9), bit: 1)) == 21 && channel(two) == -1
                    && channel(PanelModel.panMessage(in1, 0x4000)) == -1 && channel(PanelModel.patchMessage(output: 2, source: 2)) == -1,
                "an on/off message is told by its channel; not one with the byte 2, nor a pan or a patch bay change")

        let v11 = Allowed(passes: FakeHelper.version11, passesOn: FakeHelper.version11On)
        t.check(v11.ons == [in1] && v11.on(in1) && !v11.on(strip(inp, 1)) && !v11.on(strip(out, 0)) && !v11.on(strip(mon, 0))
                    && v11.volumes == [in1] && v11.pans == [in1] && !v11.mixer,
                "version 11's answer: as version 9's, and INPUT 1's on/off")
        t.check(v11.words.hasSuffix("the pan of INPUT 1 and the mute of INPUT 1") && v11.short.contains("INPUT 1 volume, pan, mute"),
                "in words: \(v11.words) / \(v11.short)")
        t.check(Allowed(passes: FakeHelper.version9, passesOn: FakeHelper.version11On).ons.isEmpty
                    && Allowed(passes: UInt64(PANEL_PASS_ON), passesOn: 0).ons.isEmpty && Allowed(passes: FakeHelper.version11).ons.isEmpty,
                "the on/off bit without a channel, or channels without the bit, allow no on/off")

        let g = Bench()
        g.fake.passes = FakeHelper.version11 // a helper of link version 11 or 12: INPUT 1 only
        g.fake.passesRead = 0 // and, as every helper before link version 20, no memory reads
        g.fake.passesOn = FakeHelper.version11On
        var lines: [String] = []
        g.model.onRecord = { lines.append($0) }
        g.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        g.chooseReal()
        g.model.allowChanges(true)
        g.settle()
        t.check(g.model.allowed == v11 && lines.contains { $0.contains("Changes switched on") && $0.contains("the mute of INPUT 1") },
                "changes on; the model's list is the helper's, and the record says so")
        let reads = g.fake.passedOn
        t.check(g.model.snap.channels[inp][0].on && unitValue(g, Int32(PANEL_F_ON), inp, 0, real: true) == 0,
                "INPUT 1's bit reads 0, as on the real unit; the panel draws it lit (on)")

        // The first on/off write: one exact bit, once.
        t.equal(g.model.setOnBitExact(in1, 1), Int32(PANEL_OK), "INPUT 1's on/off to exactly the bit 1")
        t.check(g.model.onNote?.contains("INPUT 1 bit 1 (mute on) asked for") == true,
                "the note says what was asked for, with the bit beside the word: \(g.model.onNote ?? "nil")")
        g.settle()
        t.check(g.fake.changes.map(PanelModel.hex) == [one] && g.fake.passedOn == reads + 2,
                "one message and one read went to the unit: no pause of its saving around it")
        t.equal(unitValue(g, Int32(PANEL_F_ON), inp, 0, real: true), 1, "the unit has it")
        t.check(!g.model.snap.channels[inp][0].on && g.model.disagreement == nil && !g.model.snap.busy, "the panel draws it dark, with no disagreement, nothing waiting")
        t.check(g.model.onNote?.contains("sent " + one + ". The unit took it: d[30] 00→01, bit 0.") == true
                    && g.model.onNote?.contains("Every other byte of the 256 read back the same") == true && g.model.onNote?.contains("said nothing else") == true
                    && g.model.onNote?.contains("OTHER BITS") == false,
                "the read-back: the one bit and nothing else: \(g.model.onNote ?? "nil")")
        t.equal(g.model.setOnBitExact(in1, 0), Int32(PANEL_OK), "and back to exactly the bit 0")
        g.settle()
        t.check(g.fake.changes.map(PanelModel.hex) == [one, zero] && g.fake.passedOn == reads + 4 && unitValue(g, Int32(PANEL_F_ON), inp, 0, real: true) == 0,
                "two messages and two reads in all; the unit holds 0 again")
        t.check(g.model.onNote?.contains("The unit took it: d[30] 01→00, bit 0.") == true, "the read-back: \(g.model.onNote ?? "nil")")
        t.check(lines.contains("INPUT 1 mute: bit 1 (mute on) asked for")
                    && lines.filter { $0.hasPrefix("  handed to the helper: ") }.map { String($0.dropFirst(24)) } == g.fake.changes.map(PanelModel.hex)
                    && lines.filter { $0.contains("settings before:") }.count == 2 && lines.filter { $0.contains("settings after:") }.count == 2,
                "both are in the record: what was asked for, each message as it left, the settings before and after")
        t.equal(g.model.setOnBitExact(in1, 0), Int32(PANEL_OK), "the same bit again")
        g.settle()
        t.check(g.model.onNote?.contains("already has that bit") == true && g.fake.changes.count == 2, "sends nothing, and says so")

        // The strip's button and the M key do the same as the menu.
        g.model.cursor = in1
        t.equal(g.model.toggleMonitor(in1), Int32(PANEL_OK), "INPUT 1's ON button: off")
        g.settle()
        t.check(g.fake.changes.last.map(PanelModel.hex) == one && g.model.onNote?.contains("The unit took it") == true, "it sends the bit 1, read back")
        g.model.handle(.toggleMonitor)
        g.settle()
        t.check(g.fake.changes.last.map(PanelModel.hex) == zero && unitValue(g, Int32(PANEL_F_ON), inp, 0, real: true) == 0, "the M key: on again, the bit 0")

        // A bit the unit keeps another way, and another bit of the byte that moves: both said.
        let n = g.fake.changes.count
        g.fake.unit.ignore(kind: Int(PANEL_F_ON))
        t.equal(g.model.setOnBitExact(in1, 1), Int32(PANEL_OK), "a unit that doesn't take it")
        g.settle(3_000_000)
        t.check(g.model.onNote?.contains("THE UNIT HOLDS THE OTHER BIT: d[30] 00→00, bit 0 is 0, wanted 1") == true, "the read-back says so: \(g.model.onNote ?? "nil")")
        g.fake.unit.ignore(kind: -1)
        g.model.dismissDisagreement()
        t.check(g.fake.changes.count == n + 1, "one message left for it")

        // Every other channel's on/off, and the other mixer controls, stay refused.
        let passed = g.fake.passedOn
        t.equal(g.model.setMonitor(strip(inp, 1), on: false), Int32(PANEL_ERR_READ_ONLY), "INPUT 2's on/off: refused")
        t.check(g.model.refusal?.kind == .notPassed && g.model.refusal?.text.contains("the mute of INPUT 1") == true,
                "with a plain sentence: \(g.model.refusal?.text ?? "nil")")
        t.equal(g.model.setMonitor(strip(out, 0), on: false), Int32(PANEL_ERR_READ_ONLY), "OUTPUT 1's on/off: refused")
        t.equal(g.model.setOnBitExact(strip(mon, 0), 1), Int32(PANEL_ERR_READ_ONLY), "MONITOR OUT L's on/off: refused")
        t.equal(g.model.setLink(group: inp, pair: 0, on: true), Int32(PANEL_ERR_READ_ONLY), "link: refused")
        g.settle()
        t.check(g.fake.passedOn == passed && g.fake.refused == 0 && g.helper.blocked == 0, "none of that sent anything")
        // The transport's own rule: INPUT 2's bit, made by the app's builder, stops there.
        let input2 = PanelModel.onMessage(strip(inp, 1), bit: 1)
        t.check(input2.withUnsafeBufferPointer { !g.helper.send($0) } && two.withUnsafeBufferPointer { !g.helper.send($0) } && g.helper.blocked == 2,
                "handed straight to the way to the unit, INPUT 2's bit and the byte 2 stop there")

        // A helper of version 9 or 10, which doesn't name on/off: refused before the engine.
        let old = Bench()
        old.fake.passes = FakeHelper.version9
        old.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        old.chooseReal()
        old.model.allowChanges(true)
        old.settle()
        t.equal(old.model.setOnBitExact(in1, 1), Int32(PANEL_ERR_READ_ONLY), "a version 10 helper: INPUT 1's on/off refused")
        old.settle()
        t.check(old.fake.changesPassedOn == 0 && old.fake.refused == 0, "and nothing was sent")

        // SOFT CTRL out: refused. The pretend unit: no note and no record.
        let soft = Bench()
        soft.chooseReal()
        soft.model.allowChanges(true)
        soft.settle()
        t.equal(soft.model.setOnBitExact(in1, 1), Int32(PANEL_ERR_SOFT_CTRL), "SOFT CTRL out: the on/off isn't sent")
        t.check(soft.fake.changesPassedOn == 0 && soft.model.onNote == nil, "nothing left, and no note")
        let p = Bench()
        var pretendLines = 0
        p.model.onRecord = { _ in pretendLines += 1 }
        p.openUp()
        t.equal(p.model.toggleMonitor(in1), Int32(PANEL_OK), "pretend unit: INPUT 1 off")
        p.settle()
        t.check(p.model.onNote == nil && pretendLines == 0 && unitValue(p, Int32(PANEL_F_ON), inp, 0) == 1, "the bit 1, no note and no record")
    }

    // ---- 15f2
    if t.begin("the UA-101 with changes on: every channel's volume, pan and on/off (link version 13)") {
        let all = Allowed.volumeStrips
        let ok = Int32(PANEL_OK), readOnly = Int32(PANEL_ERR_READ_ONLY)
        func name(_ s: StripID) -> String { ChannelNames.full(s) }
        func unit(_ b: Bench, _ kind: Int32, _ s: StripID) -> UInt32 { unitValue(b, kind, s.group, s.channel, real: true) }
        /// A pair's link in the stand-in helper's pretend unit, set behind the helper's back
        /// as the real unit holds it (00 <group+3> <the pair's first channel> 01, one byte),
        /// then read again. No message of the app's does this here: a version 13 helper passes on no link.
        func unitLink(_ b: Bench, group: Int, pair: Int, on: Bool) {
            let a1 = UInt8(group + 3), a2 = UInt8(pair * 2), v: UInt8 = on ? 1 : 0
            let sum = Int(a1) + Int(a2) + 1 + Int(v)
            let bytes: [UInt8] = [0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, a1, a2, 0x01, v, UInt8((0x80 - (sum & 0x7F)) & 0x7F), 0xF7]
            panel_sim_receive(b.fake.unit.sim, bytes, bytes.count)
            b.model.refresh()
            b.settle()
        }

        t.check(FakeHelper.version13 == panel_helper_passes() & ~UInt64(PANEL_PASS_LINK | PANEL_PASS_SOLO | PANEL_PASS_MEMORY)
                    && FakeHelper.version13On == panel_helper_passes_on() & 0xFFFFFFFF,
                "version 13's two numbers are the real helper's of this build without the link, the solo and the memory commands")
        let v13 = Allowed(passes: FakeHelper.version13, passesOn: FakeHelper.version13On)
        t.check(v13.volumes == Set(all) && v13.ons == Set(all) && v13.pans == Allowed.panStrips && v13.pans.count == 20 && all.count == 22
                    && !v13.pan(strip(mon, 0)) && !v13.pan(strip(mon, 1)) && v13.saving && v13.allPatch && !v13.mixer,
                "version 13's answer: every channel's volume and on/off, the pan of the 20 that have one, and still no link")
        t.check(v13.words.hasSuffix("the volume of every channel (up to +12 dB), the pan of every INPUT and OUTPUT channel and the mute of every channel")
                    && v13.short == "saving · patch bay · mixer volume, pan, mute", "in words: \(v13.words) / \(v13.short)")
        t.check(Allowed.names([strip(inp, 0), strip(out, 1)]) == "INPUT 1, OUTPUT 2" && Allowed.shortNames([strip(mon, 0)]) == name(strip(mon, 0))
                    && Allowed(passes: FakeHelper.version11, passesOn: FakeHelper.version11On).short.contains("INPUT 1 volume, pan, mute"),
                "a shorter list is still given name by name")
        t.check(PanelModel.hex(PanelModel.volumeMessage(strip(mon, 0), unity)) == "F0 41 10 00 69 12 00 03 00 07 02 00 00 00 00 00 74 F7"
                    && PanelModel.hex(PanelModel.volumeMessage(strip(out, 9), 0x16A09E)) == "F0 41 10 00 69 12 00 05 09 07 01 06 0A 00 09 0E 43 F7"
                    && PanelModel.hex(PanelModel.panMessage(strip(out, 0), 0x4000)) == "F0 41 10 00 69 12 00 05 00 03 04 00 00 00 74 F7"
                    && PanelModel.hex(PanelModel.onMessage(strip(out, 9), bit: 1)) == "F0 41 10 00 69 12 00 05 09 02 01 6F F7"
                    && PanelModel.hex(PanelModel.onMessage(strip(mon, 1), bit: 0)) == "F0 41 10 00 69 12 00 03 01 02 00 7A F7",
                "MONITOR OUT's and OUTPUT 10's messages are the ones the helper's test writes out by hand")

        let g = Bench()
        var lines: [String] = []
        g.model.onRecord = { lines.append($0) }
        g.fake.passes = FakeHelper.version13 // a helper of link version 13: no link, no solo
        g.fake.passesRead = 0 // and, as every helper before link version 20, no memory reads
        g.fake.passesOn = FakeHelper.version13On
        g.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        g.chooseReal()
        g.model.allowChanges(true)
        g.settle()
        t.check(g.model.snap.writesAllowed && g.model.allowed == v13
                    && lines.contains { $0.contains("Changes switched on") && $0.contains("the volume of every channel (up to +12 dB)") },
                "changes on; the model's list is the helper's, and the record says so")

        // First round: every pair unlinked, so each channel moves alone.
        for group in [mon, inp, out] {
            for pair in 0..<g.model.snap.links[group].count where g.model.snap.links[group][pair] { unitLink(g, group: group, pair: pair, on: false) }
        }
        t.check(all.allSatisfy { g.model.linkedPartner($0) == nil }, "every pair is unlinked in the pretend unit, for the first round")
        var wrong: [String] = []
        let sentBefore = g.fake.changes.count
        for s in all {
            let at = PanelModel.volumeOffset(s)
            if g.model.setVolumeExact(s, 0x16A09E) != ok { wrong.append(name(s) + " volume: not taken by the model") }
            g.settle()
            if unit(g, Int32(PANEL_F_VOLUME), s) != 0x16A09E
                || g.model.volumeNote?.contains("\(name(s)) volume to −3.0 dB (0x16A09E): sent") != true
                || g.model.volumeNote?.contains("The unit took it: d[\(at)…\(at + 5)] 02 00 00 00 00 00 → 01 06 0A 00 09 0E") != true
                || g.model.volumeNote?.contains("Every other byte of the 256 read back the same") != true {
                wrong.append(name(s) + " volume: \(g.model.volumeNote ?? "no note")")
            }
            if g.model.setVolumeExact(s, UInt32(PANEL_MAX_VOLUME) + 1) != Int32(PANEL_ERR_VALUE) { wrong.append(name(s) + ": a volume past the top wasn't refused") }
            if g.model.setVolumeExact(s, unity) != ok { wrong.append(name(s) + " volume back") }
            g.settle()
            if unit(g, Int32(PANEL_F_VOLUME), s) != unity { wrong.append(name(s) + " volume: not back at 0 dB") }

            if s.group == mon {
                if g.model.setPanExact(s, 0x4000) != readOnly || g.model.refusal?.kind != .notPassed { wrong.append(name(s) + ": a pan wasn't refused") }
            } else {
                let was = unit(g, Int32(PANEL_F_PAN), s), pat = PanelModel.panOffset(s)
                if g.model.setPanExact(s, 0x4000) != ok { wrong.append(name(s) + " pan: not taken by the model") }
                g.settle()
                if unit(g, Int32(PANEL_F_PAN), s) != 0x4000
                    || g.model.panNote?.contains("The unit took it: d[\(pat)…\(pat + 3)]") != true
                    || g.model.panNote?.contains("→ 04 00 00 00") != true
                    || g.model.panNote?.contains("Every other byte of the 256 read back the same") != true {
                    wrong.append(name(s) + " pan: \(g.model.panNote ?? "no note")")
                }
                if g.model.setPanExact(s, 0x8001) != Int32(PANEL_ERR_VALUE) { wrong.append(name(s) + ": a pan past the end wasn't refused") }
                if g.model.setPanExact(s, was) != ok { wrong.append(name(s) + " pan back") }
                g.settle()
                if unit(g, Int32(PANEL_F_PAN), s) != was { wrong.append(name(s) + " pan: not back") }
            }

            let place = PanelModel.onPlace(s)
            if g.model.setOnBitExact(s, 1) != ok { wrong.append(name(s) + " on/off: not taken by the model") }
            g.settle()
            if unit(g, Int32(PANEL_F_ON), s) != 1
                || g.model.onNote?.contains("The unit took it: d[\(place.byte)]") != true
                || g.model.onNote?.contains("bit \(place.bit).") != true
                || g.model.onNote?.contains("Every other byte of the 256 read back the same") != true
                || g.model.onNote?.contains("OTHER BITS") == true {
                wrong.append(name(s) + " on/off: \(g.model.onNote ?? "no note")")
            }
            if g.model.setOnBitExact(s, 0) != ok { wrong.append(name(s) + " on/off back") }
            g.settle()
            if unit(g, Int32(PANEL_F_ON), s) != 0 || !g.model.snap.channels[s.group][s.channel].on { wrong.append(name(s) + " on/off: not back on") }
        }
        t.check(wrong.isEmpty, "all 22 channels, one by one: the volume to −3 dB and back, never past the top; the pan to the middle and back, "
                + "never past the end, and none for MONITOR OUT; the bit 1 and back; each read back with nothing else moved (\(wrong.count) wrong\(wrong.first.map { ": " + $0 } ?? ""))")
        t.check(g.fake.changes.count == sentBefore + 2 * 22 + 2 * 20 + 2 * 22 && g.fake.refused == 0 && g.fake.tooMany == 0 && g.helper.blocked == 0
                    && g.model.guardBlocks == 0 && g.model.snap.disagreements == 0 && g.model.snap.writesAllowed,
                "\(g.fake.changes.count - sentBefore) messages left for that, none refused by the helper or stopped by a guard, no disagreement; changes still on")
        t.check(lines.contains("\(name(strip(out, 9))) volume: −3.0 dB (0x16A09E) asked for") && lines.contains { $0.contains("\(name(strip(mon, 1))) mute: bit 1 (mute on) asked for") }
                    && lines.contains("  handed to the helper: F0 41 10 00 69 12 00 05 09 07 01 06 0A 00 09 0E 43 F7"),
                "the record names each channel and gives each message as it was handed to the helper")

        // Second round: linked pairs, as the real unit holds the computer's channels and
        // MONITOR OUT. A volume and an on/off go to both channels; a pan to the one.
        unitLink(g, group: out, pair: 2, on: true)
        unitLink(g, group: mon, pair: 0, on: true)
        let o5 = strip(out, 4), o6 = strip(out, 5), mL = strip(mon, 0), mR = strip(mon, 1)
        t.check(g.model.linkedPartner(o5) == o6 && g.model.linkedPartner(o6) == o5 && g.model.linkedPartner(mR) == mL
                    && g.model.linkedPartner(strip(inp, 0)) == nil && g.model.linkedPartner(strip(out, 0)) == nil,
                "OUTPUT 5-6 and MONITOR OUT are linked now, the other pairs aren't")
        var n = g.fake.changes.count
        t.equal(g.model.setVolumeExact(o5, 0x16A09E), ok, "OUTPUT 5, linked with OUTPUT 6, to −3 dB")
        g.settle()
        let a5 = PanelModel.volumeOffset(o5), a6 = PanelModel.volumeOffset(o6)
        t.check(g.fake.changes.count == n + 2 && unit(g, Int32(PANEL_F_VOLUME), o5) == 0x16A09E && unit(g, Int32(PANEL_F_VOLUME), o6) == 0x16A09E,
                "two messages left, one for each channel of the pair, and the unit has both")
        t.check(g.model.volumeNote?.contains("and, for \(name(o6)), which is linked with it, F0 41 10 00 69 12 00 05 05 07 01 06 0A 00 09 0E") == true
                    && g.model.volumeNote?.contains("The unit took it: d[\(a5)…\(a5 + 5)] 02 00 00 00 00 00 → 01 06 0A 00 09 0E") == true
                    && g.model.volumeNote?.contains("\(name(o6)) took it too: d[\(a6)…\(a6 + 5)] 02 00 00 00 00 00 → 01 06 0A 00 09 0E") == true
                    && g.model.volumeNote?.contains("Every other byte of the 256 read back the same") == true
                    && g.model.volumeNote?.contains("OTHER SETTINGS") == false && g.model.volumeNote?.contains("messages left in all") == false,
                "the read-back: both channels' six bytes, and nothing else called different: \(g.model.volumeNote ?? "nil")")
        n = g.fake.changes.count
        t.equal(g.model.setOnBitExact(o6, 1), ok, "OUTPUT 6's bit to 1")
        g.settle()
        t.check(g.fake.changes.count == n + 2 && unit(g, Int32(PANEL_F_ON), o5) == 1 && unit(g, Int32(PANEL_F_ON), o6) == 1
                    && g.model.onNote?.contains("The unit took it") == true && g.model.onNote?.contains("\(name(o5)) took it too: bit 0.") == true
                    && g.model.onNote?.contains("Every other byte of the 256 read back the same") == true && g.model.onNote?.contains("OTHER BITS") == false,
                "two messages; both bits are 1, and the read-back says so without calling the partner's bit a surprise: \(g.model.onNote ?? "nil")")
        n = g.fake.changes.count
        t.equal(g.model.setPanExact(o5, 0x4000), ok, "OUTPUT 5's pan to the middle")
        g.settle()
        t.check(g.fake.changes.count == n + 1 && unit(g, Int32(PANEL_F_PAN), o5) == 0x4000 && unit(g, Int32(PANEL_F_PAN), o6) == 0x8000
                    && g.model.panNote?.contains("took it too") == false && g.model.panNote?.contains("Every other byte of the 256 read back the same") == true,
                "one message: a pan goes to the one channel, linked or not, and OUTPUT 6 stays hard right")
        g.model.setPanExact(o5, 0)
        g.settle()
        g.model.setOnBitExact(o5, 0)
        g.settle()
        g.model.setVolumeExact(o6, unity)
        g.settle()
        t.check(unit(g, Int32(PANEL_F_VOLUME), o5) == unity && unit(g, Int32(PANEL_F_VOLUME), o6) == unity && unit(g, Int32(PANEL_F_ON), o5) == 0
                    && unit(g, Int32(PANEL_F_ON), o6) == 0 && unit(g, Int32(PANEL_F_PAN), o5) == 0,
                "put back from either channel of the pair: both at 0 dB and on, the pan hard left")
        n = g.fake.changes.count
        t.equal(g.model.setVolumeExact(mR, 0x100001), ok, "MONITOR OUT R, linked with L, to −6 dB")
        g.settle()
        t.check(g.fake.changes.count == n + 2 && unit(g, Int32(PANEL_F_VOLUME), mL) == 0x100001 && unit(g, Int32(PANEL_F_VOLUME), mR) == 0x100001
                    && g.model.volumeNote?.contains("\(name(mL)) took it too: d[116…121]") == true && g.model.volumeNote?.contains("OTHER SETTINGS") == false,
                "two messages, and the unit has both: \(g.model.volumeNote ?? "nil")")
        t.equal(g.model.setMonitor(mL, on: false), ok, "MONITOR OUT L off")
        g.settle()
        t.check(unit(g, Int32(PANEL_F_ON), mL) == 1 && unit(g, Int32(PANEL_F_ON), mR) == 1 && !g.model.snap.channels[mon][1].on, "both bits are 1: the pair shows off")
        g.model.setMonitor(mR, on: true)
        g.settle()
        g.model.setVolumeExact(mL, unity)
        g.settle()
        t.check(unit(g, Int32(PANEL_F_VOLUME), mL) == unity && unit(g, Int32(PANEL_F_VOLUME), mR) == unity && unit(g, Int32(PANEL_F_ON), mL) == 0 && unit(g, Int32(PANEL_F_ON), mR) == 0,
                "and back: both at 0 dB and on")
        t.check(g.fake.refused == 0 && g.fake.tooMany == 0 && g.helper.blocked == 0 && g.model.guardBlocks == 0 && g.model.snap.disagreements == 0 && g.model.snap.writesAllowed,
                "in all of it nothing was refused, no guard stepped in, nothing disagreed; changes are still on")

        // A drag of a linked pair's fader: two messages a value, still spaced out, the saving
        // paused around it, and both channels end on the last value.
        let dragStart = g.fake.changes.count
        g.model.beginDrag(volumeOf: o5)
        for i in 0..<300 { // 1.5 s of mouse, a value every 5 ms, from 0 dB down to −30 dB
            g.model.setVolume(o5, FaderScale.volume(dB: -Double(i) / 10))
            g.settle(5_000)
        }
        g.model.endDrag()
        g.settle()
        let pairDrag = Array(g.fake.changes[dragStart...])
        let between = pairDrag.dropFirst().dropLast()
        let lastValue = FaderScale.volume(dB: -29.9)
        t.check(pairDrag.count >= 5 && PanelModel.hex(pairDrag.first!) == "F0 41 10 00 69 12 00 00 00 00 00 00 F7" && PanelModel.hex(pairDrag.last!) == "F0 41 10 00 69 12 00 00 00 00 01 7F F7"
                    && between.allSatisfy { [16, 17].contains(panel_volume_message_channel($0, $0.count)) }
                    && between.contains { panel_volume_message_channel($0, $0.count) == 16 } && between.contains { panel_volume_message_channel($0, $0.count) == 17 },
                "a drag of OUTPUT 5's fader, linked: a pause, volumes for OUTPUT 5 and OUTPUT 6 only, a resume (\(pairDrag.count) messages)")
        t.check(pairDrag.count <= 32 && g.fake.tooMany == 0 && g.fake.refused == 0 && g.helper.blocked == 0,
                "spaced out as for one channel: \(pairDrag.count) messages for 300 values in 1.5 s, never more than the helper's 20 a second")
        t.check(unit(g, Int32(PANEL_F_VOLUME), o5) == lastValue && unit(g, Int32(PANEL_F_VOLUME), o6) == lastValue && panel_sim_saving_paused(g.fake.unit.sim) == 0
                    && g.model.snap.disagreements == 0 && g.model.volumeNote?.contains("OTHER SETTINGS") == false,
                "both channels of the pair end on the last value asked for; the saving is on again; nothing disagreed: \(g.model.volumeNote ?? "nil")")
        g.model.setVolumeExact(o5, unity)
        g.settle()
        t.check(unit(g, Int32(PANEL_F_VOLUME), o5) == unity && unit(g, Int32(PANEL_F_VOLUME), o6) == unity, "and back: both at 0 dB")

        // A link itself, and what needs one, stay refused before the engine.
        let passed = g.fake.passedOn
        t.equal(g.model.setLink(group: out, pair: 2, on: false), readOnly, "a link: still refused")
        t.check(g.model.refusal?.kind == .notPassed && g.model.refusal?.text.contains("the mute of every channel") == true,
                "with a plain sentence: \(g.model.refusal?.text ?? "nil")")
        t.equal(g.model.resetMixer(), readOnly, "Reset Mixer, which changes links: refused")
        g.settle()
        t.check(g.fake.passedOn == passed && g.fake.refused == 0 && g.helper.blocked == 0, "none of that sent anything")

        // A helper that passes on one channel of a linked pair but not the other: nothing is
        // sent, so the pair can't be pulled apart and the helper has nothing to refuse.
        let half = Bench()
        half.fake.passes = FakeHelper.version13 & ~(UInt64(PANEL_PASS_VOLUME_CHANNEL_0) << 17) // not OUTPUT 6's volume
        half.fake.passesOn = FakeHelper.version13On & ~(PANEL_PASS_ON_CHANNEL_0 << 17)        // nor its on/off
        half.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        half.chooseReal()
        half.model.allowChanges(true)
        half.settle()
        unitLink(half, group: out, pair: 2, on: true)
        let before = half.fake.changesPassedOn
        t.equal(half.model.setVolumeExact(o5, 0x16A09E), readOnly, "a helper without OUTPUT 6: OUTPUT 5's volume, linked with it, is refused before anything is sent")
        t.check(half.model.refusal?.kind == .notPassed && half.model.refusal?.text.contains("linked with \(name(o6))") == true, "with a plain sentence: \(half.model.refusal?.text ?? "nil")")
        t.equal(half.model.setOnBitExact(o5, 1), readOnly, "and so is its on/off")
        half.settle()
        t.check(half.fake.changesPassedOn == before && half.fake.refused == 0 && unit(half, Int32(PANEL_F_VOLUME), o5) == unity && half.model.snap.writesAllowed,
                "nothing was sent, the helper refused nothing, changes stay on")
        unitLink(half, group: out, pair: 2, on: false)
        t.equal(half.model.setVolumeExact(o5, 0x16A09E), ok, "unlinked, OUTPUT 5 moves alone")
        half.settle()
        t.check(unit(half, Int32(PANEL_F_VOLUME), o5) == 0x16A09E && unit(half, Int32(PANEL_F_VOLUME), o6) == unity && half.fake.refused == 0, "and OUTPUT 6 stays at 0 dB")
    }

    // ---- 15f3
    if t.begin("the UA-101 with changes on: the stereo link, the solo and the two presets (link version 14)") {
        let ok = Int32(PANEL_OK), readOnly = Int32(PANEL_ERR_READ_ONLY)
        let fLink = Int32(PANEL_F_LINK), fSolo = Int32(PANEL_F_SOLO), fVolume = Int32(PANEL_F_VOLUME), fPatch = Int32(PANEL_F_PATCH)
        func unit(_ b: Bench, _ kind: Int32, _ g: Int, _ i: Int) -> UInt32 { unitValue(b, kind, g, i, real: true) }
        func hex(_ bytes: [UInt8]) -> String { PanelModel.hex(bytes) }
        func handOver(_ b: Bench, _ bytes: [UInt8]) -> Bool { bytes.withUnsafeBufferPointer { b.helper.send($0) } }
        func ready(_ b: Bench, passes: UInt64 = FakeHelper.version14, passesOn: UInt64 = FakeHelper.version14On) {
            b.fake.passes = passes
            b.fake.passesOn = passesOn
            b.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
            b.chooseReal()
            b.model.allowChanges(true)
            b.settle()
        }

        // Link version 16 (2026-11-19, the driver's own settings) changed nothing a control app is
        // told. Link version 17 (2026-11-21) adds Recall, Save and Reset, in a third number; what
        // it says about the ordinary changes is version 14's, unchanged. Link version 18 (the
        // same day) names the same lists and asks for no go-ahead file any more. Link version 19
        // (2026-11-27) passes a volume up to the top of the range, with the same lists. Link
        // version 20 (the same day) adds the five memory reads, in a fourth number.
        t.check(panel_helper_link_version() == 20 && FakeHelper.version17 == panel_helper_passes() && FakeHelper.version14On == panel_helper_passes_on()
                    && FakeHelper.version17Memory == panel_helper_passes_memory() && FakeHelper.version20Read == panel_helper_passes_read()
                    && FakeHelper.version17 == FakeHelper.version14 | UInt64(PANEL_PASS_MEMORY),
                "the stand-in helper names what the real helper of this build (link version 20) names: version 14's two numbers, the memory commands and the memory reads")
        let v14 = Allowed(passes: FakeHelper.version17, passesOn: FakeHelper.version14On, passesMemory: FakeHelper.version17Memory, passesRead: FakeHelper.version20Read)
        t.check(v14.links == Set(0..<11) && v14.solos == [1, 2] && v14.link(group: mon, pair: 0) && v14.link(group: out, pair: 4)
                    && v14.solo(group: inp) && v14.solo(group: out) && !v14.solo(group: mon) && v14.mixer,
                "version 14's answer: every pair's link and the solo of INPUT and OUTPUT (MONITOR OUT has none)")
        t.check(v14.words.hasSuffix("the mute of every channel, the stereo link of every pair and the solo of INPUT and OUTPUT")
                    && v14.short == "saving · patch bay · mixer", "in words: \(v14.words) / \(v14.short)")
        let some = Allowed(passes: FakeHelper.version13 | UInt64(PANEL_PASS_LINK | PANEL_PASS_SOLO),
                           passesOn: FakeHelper.version13On | (PANEL_PASS_LINK_PAIR_0 << 1) | PANEL_PASS_SOLO_GROUP_1)
        t.check(some.links == [1] && some.solos == [1] && some.link(group: inp, pair: 0) && !some.link(group: out, pair: 0) && !some.solo(group: out)
                    && some.words.hasSuffix("the stereo link of INPUT 1-2 and the solo of INPUT")
                    && some.short.hasSuffix("mixer volume, pan, mute · INPUT 1-2 link · INPUT solo"),
                "a shorter list is given name by name: \(some.words) / \(some.short)")
        let unflagged = Allowed(passes: FakeHelper.version13, passesOn: FakeHelper.version14On)
        t.check(unflagged.links.isEmpty && unflagged.solos.isEmpty, "pairs and groups named without the two flags are not read")
        t.check(hex(PanelModel.linkMessage(group: inp, pair: 0, bit: 1)) == "F0 41 10 00 69 12 00 04 00 01 01 7A F7"
                    && hex(PanelModel.linkMessage(group: out, pair: 4, bit: 1)) == "F0 41 10 00 69 12 00 05 08 01 01 71 F7"
                    && hex(PanelModel.linkMessage(group: mon, pair: 0, bit: 0)) == "F0 41 10 00 69 12 00 03 00 01 00 7C F7"
                    && hex(PanelModel.soloMessage(group: inp, value: 3)) == "F0 41 10 00 69 12 00 04 00 00 03 79 F7"
                    && hex(PanelModel.soloMessage(group: out, value: 10)) == "F0 41 10 00 69 12 00 05 00 00 0A 71 F7"
                    && hex(PanelModel.soloMessage(group: inp, value: 0)) == "F0 41 10 00 69 12 00 04 00 00 00 7C F7"
                    && PanelModel.soloMessage(group: mon, value: 1).allSatisfy { $0 == 0 }
                    && PanelModel.soloMessage(group: inp, value: 11).allSatisfy { $0 == 0 }
                    && PanelModel.linkMessage(group: inp, pair: 5, bit: 1).allSatisfy { $0 == 0 }
                    && PanelModel.linkMessage(group: inp, pair: 0, bit: 2).allSatisfy { $0 == 0 },
                "the link's and the solo's messages are the ones the helper's test writes out by hand; none is made for what doesn't exist")
        t.check(PanelModel.linkPlace(group: mon, pair: 0) == (24, 0) && PanelModel.linkPlace(group: inp, pair: 1) == (25, 1)
                    && PanelModel.linkPlace(group: inp, pair: 4) == (26, 0) && PanelModel.linkPlace(group: out, pair: 4) == (28, 0),
                "where the link bits lie: d[24] MONITOR OUT, d[25] and d[26] INPUT, d[27] and d[28] OUTPUT")

        let g = Bench()
        var lines: [String] = []
        g.model.onRecord = { lines.append($0) }
        g.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        g.chooseReal()
        t.equal(g.model.setLink(group: inp, pair: 0, on: true), readOnly, "without the go-ahead a link is refused")
        t.equal(g.model.toggleSolo(strip(inp, 0)), readOnly, "and a solo")
        t.equal(g.model.askHardThru(), readOnly, "and Hard Thru isn't even asked about")
        t.check(!g.model.hardThruAsked && g.fake.changes.isEmpty, "nothing was sent")
        g.model.allowChanges(true)
        g.settle()
        t.check(g.model.snap.writesAllowed && g.model.allowed == v14
                    && lines.contains { $0.contains("Changes switched on") && $0.contains("the stereo link of every pair and the solo of INPUT and OUTPUT") },
                "changes on; the model's list is the helper's, and the record says so")

        // A link is one message, and nothing but its bit moves. INPUT 3 goes to -3 dB first, so
        // the two channels of the pair differ.
        g.model.setVolumeExact(strip(inp, 2), 0x16A09E)
        g.settle()
        var sent = g.fake.changes.count
        t.equal(g.model.setLink(group: inp, pair: 1, on: true), ok, "INPUT 3-4: link")
        t.check(g.model.pairNote?.contains("asked for") == true, "a note at once: \(g.model.pairNote ?? "nil")")
        g.settle()
        t.check(g.fake.changes.count == sent + 1 && hex(g.fake.changes.last!) == "F0 41 10 00 69 12 00 04 02 01 01 78 F7",
                "one message on the way to the unit: \(hex(g.fake.changes.last!))")
        t.check(unit(g, fLink, inp, 1) == 1 && unit(g, fVolume, inp, 2) == 0x16A09E && unit(g, fVolume, inp, 3) == unity && g.model.snap.links[inp][1],
                "the unit holds the pair linked, and each channel's own volume as before")
        var note = g.model.pairNote ?? "nil"
        t.check(note.contains("INPUT 3-4 link to bit 1 (linked): sent F0 41 10 00 69 12 00 04 02 01 01 78 F7")
                    && note.contains("The unit took it: d[25] 00→02, bit 1.") && note.contains("Every other byte of the 256 read back the same")
                    && note.contains("The unit said nothing else") && note.contains("still hold different volumes") && note.contains("0x16A09E") && note.contains("0x200000")
                    && note.contains("The next move of either fader or MUTE button sets both") && !note.contains("OTHER") && !note.contains("mutes ("),
                "the read-back: the one bit and nothing else, and the two volumes that differ: \(note)")
        t.check(lines.contains { $0.contains("INPUT 3-4 link: bit 1 (linked) asked for") }
                    && lines.contains { $0.contains("handed to the helper: F0 41 10 00 69 12 00 04 02 01 01 78 F7") }
                    && lines.contains { $0.contains("settings after:") }, "the record has the link, what left and what came back")
        sent = g.fake.changes.count
        g.model.setVolumeExact(strip(inp, 3), 0x100001)
        g.settle()
        t.check(unit(g, fVolume, inp, 2) == 0x100001 && unit(g, fVolume, inp, 3) == 0x100001 && g.fake.changes.count == sent + 2
                    && g.model.volumeNote?.contains("OTHER SETTINGS") == false,
                "the next move of INPUT 4's fader goes to both: two messages, both channels equal: \(g.model.volumeNote ?? "nil")")
        sent = g.fake.changes.count
        t.equal(g.model.setLink(group: inp, pair: 1, on: true), ok, "the same link again")
        g.settle()
        t.check(g.fake.changes.count == sent && g.model.pairNote?.contains("already has that bit. Nothing was sent") == true,
                "the unit already has it: nothing is sent: \(g.model.pairNote ?? "nil")")
        g.model.toggleLink(group: inp, pair: 1)
        g.settle()
        note = g.model.pairNote ?? "nil"
        t.check(unit(g, fLink, inp, 1) == 0 && g.fake.changes.count == sent + 1 && note.contains("INPUT 3-4 link to bit 0 (not linked)")
                    && note.contains("The unit took it: d[25] 02→00, bit 1.") && !note.contains("still hold") && !note.contains("OTHER"),
                "a click unlinks it: one message, the one bit: \(note)")
        g.model.setVolumeExact(strip(inp, 2), unity)
        g.model.setVolumeExact(strip(inp, 3), unity)
        g.settle()
        t.check(unit(g, fVolume, inp, 2) == unity && unit(g, fVolume, inp, 3) == unity, "each channel back to 0 dB by itself")
        // The first and the last pair of the helper's list, both linked as the unit is delivered.
        for (group, pair, at) in [(mon, 0, 24), (out, 4, 28)] {
            let name = ChannelNames.pair(group: group, pair: pair)
            let was = g.model.snap.links[group][pair]
            g.model.toggleLink(group: group, pair: pair)
            g.settle()
            let off = g.model.pairNote ?? "nil"
            let between = unit(g, fLink, group, pair)
            g.model.toggleLink(group: group, pair: pair)
            g.settle()
            let on = g.model.pairNote ?? "nil"
            t.check(was && between == 0 && unit(g, fLink, group, pair) == 1
                        && off.contains("\(name) link to bit 0 (not linked)") && off.contains("The unit took it: d[\(at)] ") && off.contains(", bit 0.") && !off.contains("OTHER")
                        && on.contains("\(name) link to bit 1 (linked)") && on.contains("The two channels hold the same volume and mute.") && !on.contains("OTHER"),
                    "\(name): unlinked and linked again, bit 0 of d[\(at)] each time: \(off) / \(on)")
        }

        // A solo is one number a group.
        sent = g.fake.changes.count
        t.equal(g.model.toggleSolo(strip(inp, 2)), ok, "INPUT 3: solo")
        t.check(g.model.snap.soloed[inp][2] && g.model.soloNote?.contains("asked for") == true, "its lamp is lit at once, and a note: \(g.model.soloNote ?? "nil")")
        g.settle()
        note = g.model.soloNote ?? "nil"
        t.check(g.fake.changes.count == sent + 1 && hex(g.fake.changes.last!) == "F0 41 10 00 69 12 00 04 00 00 03 79 F7"
                    && unit(g, fSolo, inp, 0) == 3 && unit(g, fSolo, out, 0) == 0 && g.model.snap.soloRaw == [0, 3, 0],
                "one message, and the unit holds 3 for the INPUT group: \(hex(g.fake.changes.last!))")
        t.check(note.contains("INPUT solo to 3 (INPUT 3 alone): sent F0 41 10 00 69 12 00 04 00 00 03 79 F7") && note.contains("The unit took it: d[22] 00→03.")
                    && note.contains("Every other byte of the 256 read back the same") && note.contains("The unit said nothing else")
                    && !note.contains("OTHER") && !note.contains("linked"),
                "the read-back: the one byte and nothing else: \(note)")
        g.model.toggleSolo(strip(inp, 2))
        g.settle()
        note = g.model.soloNote ?? "nil"
        t.check(unit(g, fSolo, inp, 0) == 0 && !g.model.snap.soloed[inp][2] && note.contains("INPUT solo to 0 (no solo)") && note.contains("d[22] 03→00"),
                "a click on the lit SOLO: the unit holds 0: \(note)")
        g.model.toggleSolo(strip(out, 5))
        g.settle()
        note = g.model.soloNote ?? "nil"
        t.check(unit(g, fSolo, out, 0) == 6 && g.model.snap.soloed[out][4] && g.model.snap.soloed[out][5] && !g.model.snap.soloed[out][3]
                    && note.contains("OUTPUT solo to 6 (OUTPUT 6 alone)") && note.contains("d[23] 00→06") && note.contains("OUTPUT 5 is linked with it") && note.contains("heard, not read"),
                "OUTPUT 6, linked with OUTPUT 5: one number, both lamps, and the note says what isn't known: \(note)")
        g.model.toggleSolo(strip(out, 4))
        g.settle()
        t.check(unit(g, fSolo, out, 0) == 0 && !g.model.snap.soloed[out][5], "a click on the partner's lamp puts it out")
        g.model.cursor = strip(inp, 4)
        g.model.handle(.toggleSolo)
        g.settle()
        let byKey = unit(g, fSolo, inp, 0)
        g.model.handle(.toggleSolo)
        g.settle()
        t.check(byKey == 5 && unit(g, fSolo, inp, 0) == 0, "the Mixer menu's SOLO On/Off (the S key) works on the strip under the cursor")
        sent = g.fake.changes.count
        t.equal(g.model.toggleSolo(strip(mon, 0)), Int32(PANEL_ERR_NO_CHANNEL), "MONITOR OUT has no solo")
        g.settle()
        t.check(g.fake.changes.count == sent && g.fake.refused == 0 && g.helper.blocked == 0 && g.model.snap.disagreements == 0 && g.model.snap.writesAllowed,
                "in all of that the helper refused nothing, the way to it stopped nothing, and nothing disagreed")

        // The two presets. Hard Thru asks first.
        sent = g.fake.changes.count
        t.equal(g.model.askHardThru(), ok, "Hard Thru, step 1")
        g.settle()
        t.check(g.model.hardThruAsked && g.fake.changes.count == sent && PanelModel.hardThruWarning.contains("howl") && PanelModel.hardThruWarning.contains("Initial Setting"),
                "it asks, in words, and sends nothing")
        g.model.cancelHardThru()
        t.check(!g.model.hardThruAsked, "Cancel takes the question away")
        t.equal(g.model.confirmHardThru(), Int32(PANEL_ERR_TICKET), "step 2 without step 1: not done")
        g.settle()
        t.check(g.fake.changes.count == sent && (0..<5).allSatisfy { unit(g, fPatch, 0, $0) == UInt32(7 + $0) }, "and nothing was sent")
        g.model.askHardThru()
        t.equal(g.model.confirmHardThru(), ok, "asked and confirmed")
        g.settle()
        note = g.model.patchNote ?? "nil"
        t.check(g.fake.changes.count == sent + 5 && (0..<5).allSatisfy { unit(g, fPatch, 0, $0) == UInt32(2 + $0) }
                    && g.fake.changes[sent...].allSatisfy { panel_patch_message_output($0, $0.count) >= 0 }
                    && panel_sim_memory_commands(g.fake.unit.sim) == 0 && !g.model.hardThruAsked,
                "five ordinary patch bay messages, no memory command, and the unit has Hard Thru")
        t.check(note.contains("Hard Thru: 5 messages sent. The unit took it in all five rows: OUTPUT 1-2 d[12] 07→02") && note.contains("OUTPUT 9-10 d[16] 0B→06")
                    && note.contains("Every byte outside the five rows read back the same") && note.contains("The unit said nothing else") && !note.contains("OTHER") && !note.contains("NOT"),
                "the read-back names all five rows: \(note)")
        g.model.askHardThru()
        g.model.confirmHardThru()
        g.settle()
        t.check(g.fake.changes.count == sent + 5 && g.model.patchNote?.contains("already has it in all five rows. Nothing was sent") == true,
                "Hard Thru again: the unit already has it, nothing is sent: \(g.model.patchNote ?? "nil")")
        g.model.setPatch(output: 0, source: 7)
        g.settle()
        let mid = g.fake.changes.count
        t.equal(g.model.patchPreset(hardThru: false), ok, "Initial Setting, with OUTPUT 1-2 already on WAVE OUT 1-2")
        g.settle()
        note = g.model.patchNote ?? "nil"
        t.check(g.fake.changes.count == mid + 4 && (0..<5).allSatisfy { unit(g, fPatch, 0, $0) == UInt32(7 + $0) }
                    && note.contains("Initial Setting: 4 messages sent. The unit took it in all five rows: OUTPUT 3-4 d[13] 03→08") && note.contains("The other 1 already had it")
                    && !note.contains("OTHER"),
                "four messages for the four rows that differ, read back: \(note)")
        g.model.askHardThru()
        g.model.setPatch(output: 0, source: 7)
        t.check(!g.model.hardThruAsked, "another patch bay change takes the question away")
        g.model.askHardThru()
        g.model.allowChanges(false)
        t.check(!g.model.hardThruAsked, "and so does switching changes off")
        t.check(g.fake.refused == 0 && g.helper.blocked == 0 && lines.contains { $0.contains("Patch bay, Hard Thru: 5 rows to change") },
                "the helper refused nothing in all of this; the record has the preset")

        // The pretend unit: the same two steps, no note.
        let p = Bench()
        p.model.allowChanges(true)
        t.equal(p.model.askHardThru(), ok, "the pretend unit: Hard Thru asks too")
        t.check(p.model.hardThruAsked && p.model.snap.patch == [7, 8, 9, 10, 11], "and changes nothing yet")
        p.model.confirmHardThru()
        p.settle()
        t.check((0..<5).allSatisfy { unitValue(p, fPatch, 0, $0) == UInt32(2 + $0) } && p.model.patchNote == nil && p.model.pairNote == nil && p.model.soloNote == nil,
                "confirmed: the pretend unit has it; no note for the pretend unit")

        // A helper of link version 13 behind this app: link and solo are refused before the
        // engine, and the way to the helper stops them by its own rule too.
        let old = Bench()
        ready(old, passes: FakeHelper.version13, passesOn: FakeHelper.version13On)
        let passedBefore = old.fake.passedOn
        t.equal(old.model.setLink(group: inp, pair: 0, on: true), readOnly, "a version 13 helper: a link is refused")
        t.check(old.model.refusal?.kind == .notPassed && old.model.refusal?.text.contains("the mute of every channel") == true, "with a plain sentence: \(old.model.refusal?.text ?? "nil")")
        t.equal(old.model.toggleSolo(strip(inp, 0)), readOnly, "and a solo")
        t.check(old.model.refusal?.kind == .notPassed, "for the same reason")
        old.settle()
        let link1 = PanelModel.linkMessage(group: inp, pair: 0, bit: 1), solo3 = PanelModel.soloMessage(group: inp, value: 3)
        let stopped = old.helper.blocked
        t.check(!handOver(old, link1) && !handOver(old, solo3) && old.helper.blocked == stopped + 2 && old.fake.passedOn == passedBefore && old.fake.refused == 0,
                "handed straight to the way to the unit, both stop there: the helper never sees them")

        // A unit that doesn't take them: said in capitals.
        let deaf = Bench()
        ready(deaf)
        deaf.fake.unit.ignore(kind: Int(PANEL_F_LINK))
        deaf.model.setLink(group: inp, pair: 0, on: true)
        deaf.settle()
        t.check(deaf.model.pairNote?.contains("THE UNIT HOLDS THE OTHER BIT: d[25] 00→00, bit 0 is 0, wanted 1") == true && deaf.model.disagreement != nil && !deaf.model.snap.links[inp][0],
                "a unit that ignores a link: the note says so, the red banner too, and the pair shows unlinked: \(deaf.model.pairNote ?? "nil")")
        deaf.model.dismissDisagreement()
        deaf.fake.unit.ignore(kind: Int(PANEL_F_SOLO))
        deaf.model.toggleSolo(strip(out, 9))
        deaf.settle()
        t.check(deaf.model.soloNote?.contains("THE UNIT HOLDS ANOTHER NUMBER: d[23] 00→00, wanted 0A") == true && deaf.model.disagreement?.text.contains("OUTPUT solo") == true
                    && !deaf.model.snap.soloed[out][9],
                "a unit that ignores a solo: the same, and the lamp goes dark again: \(deaf.model.soloNote ?? "nil")")
        deaf.fake.unit.ignore(kind: -1)
        // What a version 14 helper doesn't pass on stops at the way to it as well.
        var link2 = link1, solo11 = solo3
        link2[10] = 2
        link2[11] = 0x79
        solo11[10] = 11
        solo11[11] = 0x71
        let stopped2 = deaf.helper.blocked, passed2 = deaf.fake.passedOn
        t.check(deaf.model.snap.writesAllowed && !handOver(deaf, link2) && !handOver(deaf, solo11) && deaf.helper.blocked == stopped2 + 2 && deaf.fake.passedOn == passed2,
                "a link with the byte 2 and a solo of 11, well formed and handed straight over: stopped there")
        t.check(handOver(deaf, link1) && handOver(deaf, solo3) && deaf.fake.passedOn == passed2 + 2, "the right ones pass")

        // The helper refuses a link: the app says so.
        let gone = Bench()
        ready(gone)
        gone.fake.refuseChanges = "a refusal made by the test; the message was not sent"
        gone.model.setLink(group: inp, pair: 0, on: true)
        gone.settle()
        t.check(!gone.model.snap.writesAllowed && gone.model.refusal?.kind == .helper && unit(gone, fLink, inp, 0) == 0 && !gone.model.snap.links[inp][0]
                    && gone.model.pairNote?.contains("the driver's helper refused the message") == true,
                "the helper refuses the link, changes go off, the pair shows as the unit has it: \(gone.model.pairNote ?? "nil")")
    }

    // ---- 15g
    if t.begin("the mouse on INPUT 1's fader and pan knob: they follow the hand, not the window's layout") {
        let in1 = strip(inp, 0)
        // The rule by itself.
        var track = DragTrack(start: 0.5)
        t.check(track.value(travel: 0, full: 176, range: 1) == nil, "a press without a move asks for nothing")
        t.check(track.value(travel: -1, full: 176, range: 1).map { abs($0 - (0.5 - 1.0 / 176)) < 1e-12 } == true, "a point of travel is a 176th of the fader's way")
        t.check(track.value(travel: 0, full: 176, range: 1) == 0.5, "back where it was pressed, after a move: the start value is asked for again")
        t.check(track.value(travel: 500, full: 176, range: 1) == 1 && track.value(travel: -500, full: 176, range: 1) == 0, "held at the ends")
        // The stop at 0 dB (the user's choice, 2026-11-27): a push from below halts there; let go and push again to go on.
        let zero = FaderScale.position(dB: 0)
        var fromBelow = DragTrack(start: FaderScale.position(dB: -6), stopAt: zero)
        t.check(fromBelow.value(travel: 500, full: 176, range: 1) == zero && fromBelow.value(travel: -500, full: 176, range: 1) == 0,
                "a fader pushed up from below 0 dB stops at 0 dB, and still goes all the way down")
        var fromZero = DragTrack(start: zero, stopAt: zero)
        t.check(fromZero.value(travel: 500, full: 176, range: 1) == 1, "pushed again from 0 dB: up to +12 dB")
        var fromAbove = DragTrack(start: FaderScale.position(dB: 6), stopAt: zero)
        t.check(fromAbove.value(travel: -500, full: 176, range: 1) == 0 && fromAbove.value(travel: 10, full: 176, range: 1)! > FaderScale.position(dB: 6),
                "from above 0 dB it goes down past 0 dB, and up, with no stop")
        t.equal(FaderScale.volume(position: zero), unity, "the stop is exactly 0 dB")
        let dBperPoint = FaderScale.dBPerPoint(at: -3, travel: Double(Metrics.faderHeight - FaderCap.height))
        t.check(abs(dBperPoint - 6.0 / 22) < 0.001, "around 0 dB a point of mouse travel on a fader is \(String(format: "%.3f", dBperPoint)) dB: 6 dB in 22 points (the scale of 2026-11-27)")

        // The window, off screen, with real mouse events.
        let app = NSApplication.shared
        app.setActivationPolicy(.prohibited)
        let g = Bench()
        g.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        g.chooseReal()
        g.model.allowChanges(true)
        g.settle()
        g.model.setVolumeExact(in1, 0x16A09E) // as on the unit: −3 dB, and a long note above the mixer
        g.settle()
        let host = NSHostingView(rootView: MixerWindowView(model: g.model, ui: g.ui, messages: g.messages).themed(.dark))
        host.frame = NSRect(origin: .zero, size: host.fittingSize)
        let window = NSWindow(contentRect: host.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = host
        // SwiftUI follows the mouse only in a window that is ordered in. This one is fully
        // transparent, far off every screen, takes no real mouse and never becomes key: nothing
        // appears on the Mac's screen, and the app isn't activated.
        window.alphaValue = 0
        window.ignoresMouseEvents = true
        window.setFrameOrigin(NSPoint(x: -30000, y: -30000))
        app.setActivationPolicy(.accessory)
        window.orderFrontRegardless()
        defer {
            window.orderOut(nil)
            app.setActivationPolicy(.prohibited)
        }
        func turn() {
            host.layoutSubtreeIfNeeded()
            RunLoop.current.run(until: Date().addingTimeInterval(0.05))
            host.frame = NSRect(origin: .zero, size: host.fittingSize)
            window.setContentSize(host.frame.size)
            window.setFrameOrigin(NSPoint(x: -30000, y: -30000))
            host.layoutSubtreeIfNeeded()
            RunLoop.current.run(until: Date().addingTimeInterval(0.02))
        }
        turn()
        var eventNumber = 0
        func mouse(_ type: NSEvent.EventType, _ p: CGPoint) {
            eventNumber += 1
            // Mixer coordinates have their origin at the window's top left, y downwards.
            let at = NSPoint(x: p.x, y: host.bounds.height - p.y)
            if let e = NSEvent.mouseEvent(with: type, location: at, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime,
                                          windowNumber: window.windowNumber, context: nil, eventNumber: eventNumber, clickCount: 1,
                                          pressure: type == .leftMouseUp ? 0 : 1) {
                window.sendEvent(e)
            }
            turn()
        }
        func dB() -> Double { panel_volume_db(g.model.snap.channels[inp][0].volume) }
        let travel = Metrics.faderHeight - FaderCap.height
        func cap() -> CGPoint? {
            guard let f = g.ui.faderFrames[in1] else { return nil }
            let pos = FaderScale.position(volume: g.model.snap.channels[inp][0].volume)
            return CGPoint(x: f.minX + 29, y: f.minY + CGFloat(1 - pos) * travel + FaderCap.height / 2)
        }
        t.check(g.ui.faderFrames.count == 22 && g.ui.knobFrames.count == 20, "the window knows where its 22 faders and 20 pan knobs are (\(g.ui.faderFrames.count), \(g.ui.knobFrames.count))")
        let noteBefore = g.model.volumeNote ?? ""
        let twoLines = host.fittingSize.height
        g.model.clearVolumeNote()
        turn()
        let noNote = host.fittingSize.height
        g.model.setVolumeExact(in1, 0x16A09E) // "already has that value": a one-line note
        turn()
        let oneLine = host.fittingSize.height
        // 2026-11-16: the notes are in the Messages window; the mixer window keeps its height.
        t.check(twoLines == oneLine && oneLine == noNote && g.messages.now.contains { $0.slot == .volume },
                "a long note, a short note or none: the mixer window's height stays \(twoLines) points (until 2026-11-16 a note's text moved the mixer); the note is in the Messages window")
        // Put the long note back, as it stood on the unit before the drag.
        g.model.setVolumeExact(in1, unity)
        g.settle()
        g.model.setVolumeExact(in1, 0x16A09E)
        g.settle()
        turn()
        t.check(g.model.volumeNote == noteBefore && host.fittingSize.height == twoLines, "the long note is back")

        if let start = cap() {
            let changes = g.fake.changes.count
            let startDB = dB()
            mouse(.leftMouseDown, start)
            t.check(g.model.dragging && g.model.cursor == in1, "a press on INPUT 1's cap: the drag has begun")
            t.check(g.model.volumeNote == noteBefore && g.fake.changes.count == changes && dB() == startDB,
                    "the press asks for nothing: no new note, nothing sent, the value as it was")
            // What moved the mixer on 2026-11-12: the note above it gets shorter while the fader is held.
            g.model.clearVolumeNote()
            turn()
            t.check(host.fittingSize.height == twoLines && g.ui.faderFrames[in1].map { abs($0.minY + CGFloat(1 - FaderScale.position(dB: startDB)) * travel + FaderCap.height / 2 - start.y) < 0.01 } == true,
                    "the note goes while the fader is held: its space stays, and the cap is still under the hand")
            mouse(.leftMouseDragged, CGPoint(x: start.x, y: start.y + 1))
            g.settle(100_000)
            turn()
            t.check(abs(dB() - (startDB - dBperPoint)) < 0.01, "the hand moves down one point: the value goes down one point's worth, to \(String(format: "%.2f", dB())) dB (2026-11-12: 7 dB)")
            // A message that comes while the fader is held (until 2026-11-16 it pushed the mixer
            // down 35 points under the hand): it goes to the Messages window, the mixer stays.
            let framesBefore = g.ui.faderFrames[in1]
            g.model.resetMixer() // Reset Mixer is refused while a fader is held: a "Not done" message
            turn()
            let moved = (g.ui.faderFrames[in1]?.minY ?? 0) - (framesBefore?.minY ?? 0)
            t.check(moved == 0 && g.model.refusal != nil && g.messages.now.contains { $0.slot == .refusal },
                    "a new message while the fader is held: it is in the Messages window, and the mixer moved \(moved) points")
            mouse(.leftMouseDragged, CGPoint(x: start.x, y: start.y + 2))
            g.settle(100_000)
            turn()
            t.check(abs(dB() - (startDB - 2 * dBperPoint)) < 0.01, "the hand moves one more point: one more point's worth (\(String(format: "%.2f", dB())) dB), though the fader itself moved \(moved) points under it")
            mouse(.leftMouseDragged, CGPoint(x: start.x, y: start.y + 10))
            g.settle(100_000)
            mouse(.leftMouseUp, CGPoint(x: start.x, y: start.y + 10))
            g.settle()
            turn()
            let want = FaderScale.volume(position: FaderScale.position(dB: startDB) - 10 / Double(travel))
            t.check(!g.model.dragging && unitValue(g, Int32(PANEL_F_VOLUME), inp, 0, real: true) == want && abs(dB() - (startDB - 10 * dBperPoint)) < 0.01,
                    "ten points down in all and let go: the unit has −3 dB less ten points' worth (\(String(format: "%.2f", dB())) dB)")
            let sent = g.fake.changes[changes...].map(PanelModel.hex)
            t.check(sent.first == "F0 41 10 00 69 12 00 00 00 00 00 00 F7" && sent.last == "F0 41 10 00 69 12 00 00 00 00 01 7F F7" && sent.count == 5
                        && panel_sim_saving_paused(g.fake.unit.sim) == 0,
                    "on the way to the unit: a pause, the three values, a resume (\(sent.count) messages)")
        } else {
            t.check(false, "INPUT 1's fader was found in the window")
        }

        // The stop at 0 dB with real mouse events (the owner, 2026-11-27: on the box it went
        // straight past 0 dB). From below, small steps up as a hand makes them: it halts at
        // 0 dB however far the hand goes; let go and push again: on up to +12 dB.
        if let start = cap() {
            var path: [Double] = []
            mouse(.leftMouseDown, start)
            for i in 1...40 {
                mouse(.leftMouseDragged, CGPoint(x: start.x, y: start.y - CGFloat(5 * i)))
                g.settle(20_000)
                path.append(dB())
            }
            mouse(.leftMouseUp, CGPoint(x: start.x, y: start.y - 200))
            g.settle()
            turn()
            t.check(g.model.snap.channels[inp][0].volume == unity && path.allSatisfy { $0 <= 0.0001 },
                    "pushed up 200 points from below 0 dB: it stops at 0 dB (highest on the way \(String(format: "%.2f", path.max() ?? -99)) dB, now \(String(format: "%.2f", dB())) dB)")
            if let again = cap() {
                mouse(.leftMouseDown, again)
                for i in 1...10 {
                    mouse(.leftMouseDragged, CGPoint(x: again.x, y: again.y - CGFloat(5 * i)))
                    g.settle(20_000)
                }
                mouse(.leftMouseUp, CGPoint(x: again.x, y: again.y - 50))
                g.settle()
                turn()
                t.check(dB() > 6, "let go and pushed again from 0 dB: it goes on up (\(String(format: "%.2f", dB())) dB)")
            }
            g.model.setVolumeExact(in1, 0x16A09E)
            g.settle()
            turn()
        }
        // The same on INPUT 2 with INPUT 1-2 linked, as on the box.
        do {
            let in2 = strip(inp, 1)
            g.model.setLink(group: inp, pair: 0, on: true)
            g.settle()
            g.model.setVolumeExact(in2, 0x06E0E0) // −13.4 dB, both channels
            g.settle()
            turn()
            func cap2() -> CGPoint? {
                guard let f = g.ui.faderFrames[in2] else { return nil }
                let pos = FaderScale.position(volume: g.model.snap.channels[inp][1].volume)
                return CGPoint(x: f.minX + 29, y: f.minY + CGFloat(1 - pos) * travel + FaderCap.height / 2)
            }
            func dB2() -> Double { panel_volume_db(g.model.snap.channels[inp][1].volume) }
            if let start = cap2() {
                var path: [Double] = []
                mouse(.leftMouseDown, start)
                for i in 1...40 {
                    mouse(.leftMouseDragged, CGPoint(x: start.x, y: start.y - CGFloat(5 * i)))
                    g.settle(20_000)
                    path.append(dB2())
                }
                mouse(.leftMouseUp, CGPoint(x: start.x, y: start.y - 200))
                g.settle()
                turn()
                t.check(g.model.snap.channels[inp][1].volume == unity && g.model.snap.channels[inp][0].volume == unity && path.allSatisfy { $0 <= 0.0001 },
                        "linked INPUT 1-2, INPUT 2 pushed up 200 points from −13.4 dB: both stop at 0 dB (highest on the way \(String(format: "%.2f", path.max() ?? -99)) dB)")
            } else {
                t.check(false, "INPUT 2's fader was found in the window")
            }
            g.model.setLink(group: inp, pair: 0, on: false)
            g.model.setVolumeExact(in2, unity)
            g.model.setVolumeExact(in1, 0x16A09E)
            g.settle()
            turn()
        }

        // The pan knob, the same way.
        g.model.dismissRefusal()
        turn()
        if let k = g.ui.knobFrames[in1] {
            let centre = CGPoint(x: k.midX, y: k.midY)
            let changes = g.fake.changes.count
            mouse(.leftMouseDown, centre)
            t.check(g.model.dragging && g.model.panNote == nil && g.fake.changes.count == changes, "a press on INPUT 1's pan knob: the drag has begun, and nothing is asked for yet")
            mouse(.leftMouseDragged, CGPoint(x: centre.x + 12, y: centre.y))
            g.settle(100_000)
            let tenth = UInt32((12.0 / Double(PanKnob.travel) * Double(PANEL_MAX_PAN)).rounded())
            t.check(g.model.snap.channels[inp][0].pan == tenth && tenth == 0x0CCD,
                    "12 points to the right, a tenth of the knob's way: \(String(format: "0x%04X", tenth)), as the hand put it, no step")
            mouse(.leftMouseDragged, CGPoint(x: centre.x + 60, y: centre.y))
            g.settle(100_000)
            mouse(.leftMouseUp, CGPoint(x: centre.x + 60, y: centre.y))
            g.settle()
            turn()
            t.check(!g.model.dragging && unitValue(g, Int32(PANEL_F_PAN), inp, 0, real: true) == 0x4000 && g.model.snap.channels[inp][0].pan == 0x4000,
                    "60 points to the right and let go: the unit has the middle")
            let sent = g.fake.changes[changes...].map(PanelModel.hex)
            t.check(sent.first == "F0 41 10 00 69 12 00 00 00 00 00 00 F7" && sent.last == "F0 41 10 00 69 12 00 00 00 00 01 7F F7" && sent.count == 4
                        && g.fake.refused == 0 && g.helper.blocked == 0,
                    "on the way to the unit: a pause, the two pans, a resume (\(sent.count) messages)")
        } else {
            t.check(false, "INPUT 1's pan knob was found in the window")
        }

        // 2026-11-20: the words on the mixer can be selected for copying. A click on a strip's
        // name still chooses that strip, and the controls still work (the fader and knob above
        // were pressed and dragged with selectable words all round them). Nothing is sent.
        let in4 = strip(inp, 3)
        if let n = g.ui.nameFrames[in4] {
            let changes = g.fake.changes.count
            g.model.cursor = in1
            turn()
            mouse(.leftMouseDown, CGPoint(x: n.midX, y: n.midY))
            mouse(.leftMouseUp, CGPoint(x: n.midX, y: n.midY))
            g.settle()
            t.check(g.model.cursor == in4 && g.fake.changes.count == changes,
                    "a click on INPUT 4's name (selectable words): INPUT 4 is chosen, nothing sent (cursor: \(ChannelNames.full(g.model.cursor)))")
        } else {
            t.check(false, "INPUT 4's name was found in the window")
        }
    }

    // ---- 15e
    if t.begin("the UA-101: the MIDI IN and OUT lamps (link version 15)") {
        let b = Bench()
        b.chooseReal()
        let lamps = b.model.meters
        t.check(b.model.snap.state == .ready && !b.helper.streaming, "UA-101 chosen, no audio playing")
        t.equal(lamps.midiActive, [false, false], "no MIDI: both lamps dark")
        t.equal(lamps.midiWakeUps, 0, "and nothing booked for them")

        // One note at MIDI IN.
        b.fake.midiPasses(in: true, out: false)
        b.time.advance(1)
        t.equal(lamps.midiActive, [true, false], "MIDI at MIDI IN: the IN lamp lights, with no audio playing")
        t.check(!lamps.hasSource, "(the meters stay dark: no audio)")
        t.equal(lamps.midiWakeUps, 1, "one wake-up booked to turn it off")
        // Plain numbers, not midiLitMicros: it was chosen 0.15 s, and a test that reads the
        // constant passes whatever it says.
        b.time.advance(148_000)
        t.equal(lamps.midiActive, [true, false], "still lit just before 0.15 s")
        b.time.advance(3000)
        t.equal(lamps.midiActive, [false, false], "dark after 0.15 s")
        t.equal(lamps.midiWakeUps, 0, "and nothing left booked")

        // MIDI OUT, then both, steadily, as the helper tells it: every 0.1 s for 2 s.
        b.fake.midiPasses(in: false, out: true)
        b.time.advance(1)
        t.equal(lamps.midiActive, [false, true], "MIDI out of MIDI OUT: the OUT lamp lights")
        b.settle(1_000_000)
        let wakeUps = b.time.wakeUps
        var steady = true
        for _ in 0..<20 {
            b.fake.midiPasses(in: true, out: true)
            b.time.advance(1)
            steady = steady && lamps.midiActive == [true, true]
            b.time.advance(99_999)
            steady = steady && lamps.midiActive == [true, true]
        }
        t.check(steady, "steady MIDI both ways, told every 0.1 s: both lamps stay lit")
        t.check(b.time.wakeUps - wakeUps <= 2 * 20, "with at most one wake-up per lamp per event (\(b.time.wakeUps - wakeUps))")
        t.check(lamps.midiWakeUps <= 2, "and never more than one booked per lamp")
        b.time.advance(MeterModel.midiLitMicros)
        t.equal(lamps.midiActive, [false, false], "MIDI stops: both dark within 0.15 s")
        let quiet = b.time.wakeUps
        b.settle(3_600_000_000)
        t.equal(b.time.wakeUps, quiet, "an hour with MIDI quiet: no wake-ups at all")
        t.equal(lamps.midiWakeUps, 0, "and nothing booked")

        // Audio stopping doesn't touch the MIDI lamps; the unit going does.
        // Real levels first, so the meters have a source and the "no levels" path is taken.
        b.fake.setStreaming(true)
        b.settle(1)
        b.fake.levels = HelperLevels(streaming: true, rate: 48000,
                                     capture: Array(repeating: UInt32(PANEL_LEVEL_FULL_SCALE / 2), count: 12),
                                     playback: Array(repeating: 0, count: 10))
        b.helper.pollLevels()
        b.time.advance(1)
        t.equal(lamps.source, HelperTransport.levelsName, "(audio playing: the meters have a source)")
        b.fake.midiPasses(in: true, out: false)
        b.time.advance(1)
        b.fake.notRunningYet = true
        b.helper.pollLevels()
        b.time.advance(1)
        b.fake.notRunningYet = false
        t.check(!lamps.hasSource && lamps.midiActive == [true, false],
                "the stream halts: the meters go dark, a lit MIDI lamp stays as it is")
        b.fake.setStreaming(false)
        b.time.advance(1)
        t.equal(lamps.midiActive, [true, false], "audio stopping leaves a lit MIDI lamp as it is")
        b.settle(1_000_000)
        b.fake.midiPasses(in: true, out: true)
        b.time.advance(1)
        b.fake.unitGoes()
        b.time.advance(1)
        t.equal(lamps.midiActive, [false, false], "the unit goes: both lamps dark at once")
        t.equal(lamps.midiWakeUps, 0, "with nothing left booked")
        let counted = b.helper.midiEvents
        b.fake.midiInFlight(in: true, out: true)
        b.time.advance(1)
        t.check(lamps.midiActive == [false, false] && b.helper.midiEvents == counted,
                "a lamp event still on its way when the unit went is dropped: the lamps stay dark")
        b.fake.unitComes()
        b.settle()
        t.equal(b.model.snap.state, .ready, "the unit is back")
        t.check(b.helper.midiEvents > 0, "(the transport counted the helper's lamp events: \(b.helper.midiEvents))")

        // The pretend unit has no MIDI: lamps dark, and a lamp event for the old unit is ignored.
        b.fake.midiPasses(in: true, out: true)
        b.time.advance(1)
        b.model.onChooseDevice?(false)
        b.settle()
        t.equal(lamps.midiActive, [false, false], "the pretend unit chosen: both lamps dark")
        b.fake.midiPasses(in: true, out: true)
        b.settle()
        t.equal(lamps.midiActive, [false, false], "and the UA-101's MIDI no longer lights them")
        t.check(!ChannelNames.midiNote.contains("Dark for now") && ChannelNames.midiNote.contains("with or without audio"),
                "the lamps' tool-tip says what they show now: \(ChannelNames.midiNote)")
    }

    // (A section here tried the user's go-ahead file: that it counted for a day, and that the
    // app asked the helper for it every 30 s. Both went with link version 18.)

    // ---- 15m
    if t.begin("the Messages window (2026-11-16): every note and warning, Now and Earlier, copy and CSV") {
        let in1 = strip(inp, 0)
        let m = Bench()
        m.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        m.chooseReal()
        let ro = UnitState.of(m.model)
        t.check(ro.title == "Read-only" && ro.actionLabel == "Allow changes", "the mixer's line, read-only: \(ro.title), \(ro.actionLabel ?? "no button")")
        m.model.allowChanges(true)
        m.settle()
        let on = UnitState.of(m.model)
        t.check(on.title.hasPrefix("Changes are on") && on.actionLabel == "Switch off" && on.long.contains("Every other control is still refused"),
                "changes on: the line says \(on.title), with Switch off; the whole sentence is in the Messages window")
        let start = m.messages.now.count, arrived = m.arrivals

        m.model.setVolumeExact(in1, 0x16A09E)
        t.check(m.messages.now.filter { $0.slot == .volume }.count == 1 && m.arrivals == arrived + 1 && m.messages.now.count == start + 1,
                "a volume is asked for: one message, one arrival (grey: it only goes into the list)")
        let first = m.messages.now.first { $0.slot == .volume }
        m.settle()
        let read = m.messages.now.first { $0.slot == .volume }
        t.check(read?.id == first?.id && read?.text.contains("took it") == true && m.arrivals == arrived + 1 && m.messages.events.last?.kind == .changed,
                "its read-back replaces the words in the same message; no second arrival: \(read?.text.prefix(60) ?? "nil")")
        if let v = read { m.messages.ok(v) }
        t.check(m.model.volumeNote == nil && !m.messages.now.contains { $0.slot == .volume } && m.messages.earlier.first?.okTime != nil
                    && m.messages.earlier.first?.text == read?.text && m.messages.events.last?.kind == .ok,
                "OK: the note is cleared in the model, the message goes down into Earlier with the time it was OK'd")

        t.check(MessageCenter.isWarning("Patch bay: NOT SENT. The driver's helper refused it.") && MessageCenter.isWarning("THE UNIT HOLDS ANOTHER VALUE: d[66]")
                    && MessageCenter.isWarning("x. OTHER SETTINGS READ BACK DIFFERENT, 1 byte") && MessageCenter.isWarning("THE LINKED PARTNER INPUT 2 HOLDS")
                    && !MessageCenter.isWarning("INPUT 3 volume: not done. SOFT CTRL is out on the unit.") && !MessageCenter.isWarning("INPUT 3-4 link to bit 1 (linked): The unit took it")
                    && !MessageCenter.isWarning(read?.text ?? "x"),
                "warnings are the sentences in CAPITALS (NOT SENT, THE UNIT HOLDS …, OTHER SETTINGS …), not names like INPUT 3 or SOFT CTRL")

        m.fake.unit.ignore(kind: Int(PANEL_F_VOLUME))
        let before = m.arrivals
        m.model.setVolumeExact(in1, 0x100001)
        m.settle()
        t.check(m.model.disagreement != nil && m.messages.now.first?.slot == .disagreement && m.messages.now.first?.warning == true
                    && m.messages.warningCount >= 1 && m.arrivals > before,
                "the unit keeps another value: a warning, at the top of Now, and it arrives (red: it pops up, see the pop-up section) (\(m.messages.now.first?.title ?? "nil"))")
        let copied = MessageCenter.plain(Array(m.messages.now.prefix(1)))
        t.check(copied.contains("WARNING") && copied.contains(m.messages.now.first?.title ?? "?") && copied.hasPrefix(MessageCenter.clockText(m.messages.now[0].time)),
                "a message copies as plain words with its time: \(copied.prefix(70))")
        let all = m.messages.plainAll()
        t.check(all.hasPrefix("Now\n") && all.contains("\n\nEarlier\n"), "Copy All gives Now and Earlier")

        let csv = m.messages.csv()
        let rows = csv.dropFirst().components(separatedBy: "\r\n").filter { !$0.isEmpty }
        t.check(csv.hasPrefix("\u{FEFF}Date,Time,Event,Kind,Warning,Message\r\n") && rows.count == m.messages.events.count + 1
                    && rows.dropFirst().allSatisfy { $0.hasPrefix("\"20") } && rows.contains { $0.contains("\"OK\"") } && rows.contains { $0.contains("\"yes\"") },
                "the CSV: a header and one row for each of the \(m.messages.events.count) events, OK and warnings marked")
        t.equal(MessageCenter.csvField("a \"b\", c"), "\"a \"\"b\"\", c\"", "a comma and a quote inside a message keep the CSV's columns")

        m.messages.okAll()
        t.check(m.messages.now.isEmpty && m.model.disagreement == nil && m.model.volumeNote == nil, "OK to All: Now is empty, and so are the model's notes")
        m.fake.unit.ignore(kind: -1)
        for i in 0..<(MessageCenter.earlierLimit + 5) {
            m.model.setVolumeExact(in1, i % 2 == 0 ? 0x200000 : 0x100001)
            m.settle()
            if let v = m.messages.now.first(where: { $0.slot == .volume }) { m.messages.ok(v) }
        }
        t.check(m.messages.earlier.count == MessageCenter.earlierLimit && m.messages.earlier[0].time >= m.messages.earlier[1].time,
                "Earlier keeps the last \(MessageCenter.earlierLimit), newest first (\(m.messages.earlier.count))")
        m.messages.clearEarlier()
        t.check(m.messages.earlier.isEmpty, "Clear Earlier")

        m.model.setVolumeExact(in1, 0x16A09E)
        m.model.use(m.unit)
        t.check(m.messages.now.isEmpty && m.messages.events.last?.kind == .gone, "the other unit chosen: the notes go, and the record says so")
        t.check(UnitState.of(m.model).actionLabel?.contains("pretend") == true || UnitState.of(m.model).title != "Read-only",
                "the pretend unit's line: \(UnitState.of(m.model).title)")

        // Where the window opens the first time: beside the mixer, tops in line.
        let screen = CGRect(x: 0, y: 0, width: 1440, height: 845)
        let right = MessagesWindow.place(width: 400, height: 544, beside: CGRect(x: 58, y: 290, width: 973, height: 544), screen: screen)
        t.check(right.minX == 1039 && right.maxY == 834 && right.height == 544, "room on the right (your screen): it opens to the right, tops in line: \(right)")
        let left = MessagesWindow.place(width: 400, height: 544, beside: CGRect(x: 500, y: 290, width: 900, height: 544), screen: screen)
        t.check(left.maxX == 492, "no room on the right: on the left: \(left)")
        let edge = MessagesWindow.place(width: 400, height: 900, beside: CGRect(x: 100, y: 0, width: 1300, height: 845), screen: screen)
        t.check(edge.maxX == 1440 && edge.minY >= 0 && edge.height == 845, "no room on either side: at the screen's right edge, never taller than the screen: \(edge)")

        // The dialogs too: a note, a refusal or a Load's result never changes their height.
        let d = Bench()
        d.chooseReal()
        func tall<V: View>(_ v: V) -> CGFloat { NSHostingView(rootView: v.themed(.dark)).fittingSize.height }
        let patchBefore = tall(PatchBayView(model: d.model, messages: d.messages))
        d.model.setPatch(output: 0, source: 2) // read-only: refused, "Not done"
        let patchRefused = tall(PatchBayView(model: d.model, messages: d.messages))
        d.model.allowChanges(true)
        d.settle()
        let patchOpen = tall(PatchBayView(model: d.model, messages: d.messages))
        d.model.setPatch(output: 1, source: 3)
        d.settle()
        let patchNoted = tall(PatchBayView(model: d.model, messages: d.messages))
        t.check(patchBefore == patchRefused && patchRefused == patchOpen && patchOpen == patchNoted && d.model.patchNote != nil
                    && d.messages.now.contains { $0.slot == .patch },
                "the Patch Bay keeps its height (\(patchBefore), \(patchRefused), \(patchOpen), \(patchNoted) points; note \(d.model.patchNote != nil)) read-only, after a refusal, with changes on and with a note; the note is in the Messages window")
        let e = Bench()
        e.openUp()
        let loadBefore = tall(MemoryDialogView(model: e.model, purpose: .load, memory: 2, messages: e.messages))
        e.model.prepareMemory(MemoryCommand(kind: .load, memory: 2, patchBay: true, mixer: true))
        e.model.confirmMemory()
        e.settle()
        let loadAfter = tall(MemoryDialogView(model: e.model, purpose: .load, memory: 2, messages: e.messages))
        t.check(e.model.memoryNote != nil && loadBefore == loadAfter && e.messages.now.contains { $0.slot == .memory },
                "Load: its result is in the Messages window, and the dialog's buttons stay where they were (\(loadBefore) → \(loadAfter) points)")

        t.check(PanelUI.nextMessagesScale(from: 1, up: true) == 1.15 && PanelUI.nextMessagesScale(from: 1, up: false) == 0.9
                    && PanelUI.nextMessagesScale(from: 2, up: true) == 2 && PanelUI.nextMessagesScale(from: 0.8, up: false) == 0.8,
                "larger and smaller text: one step at a time, from 80% to 200%")
    }

    // ---- 16
    if t.begin("driver settings") {
        // The lists are the driver's own (src/ua101_drvset.h).
        t.equal(DriverValues.input, [1, 2, 3, 5, 8, 10], "the recording margins offered")
        t.equal(DriverValues.output.map { ($0 * 100).rounded() / 100 }, [2, 4, 6, 8, 10, 12, 14, 16, 21.33], "the playback delays offered")
        t.equal(DriverValues.input[DriverValues.inputDefault], 8, "the default recording margin (since 1.1)")
        t.equal(DriverValues.label(DriverValues.output[DriverValues.outputDefault]), "8 ms", "the default playback delay, as shown (since 1.1)")
        t.check(DriverValues.label(5) == "5 ms" && DriverValues.label(12) == "12 ms", "whole values are shown without a fraction")
        t.check(DriverValues.inputIndex(4) == nil && DriverValues.outputIndex(9) == nil && DriverValues.outputIndex(21.3) == 8
                    && DriverValues.inputIndex(0) == nil && DriverValues.outputIndex(0) == nil,
                "only the offered values name a place, and never zero")
        // 2026-11-20: the values below the recommended ones are offered too, with a warning.
        t.check(DriverValues.input[DriverValues.inputRecommended] == 3 && DriverValues.output[DriverValues.outputRecommended] == 10,
                "the shortest recommended values are 3 ms and 10 ms")
        // The same day, at the box: each shorter playback delay names the buffer it needs.
        t.check((0..<DriverValues.output.count).map { DriverValues.outputBuffer($0, rate: 48000) } == [512, 512, 256, 128, 0, 0, 0, 0, 0],
                "the buffers at 48 kHz: \((0..<DriverValues.output.count).map { DriverValues.outputBuffer($0, rate: 48000) })")
        t.check(DriverValues.outputBuffer(3, rate: 96000) == 256 && DriverValues.outputBuffer(3, rate: 0) == 128,
                "the same time at 96 kHz, and 48 kHz before any run")
        // 1.1 (2026-11-27): recording 8 ms is recommended; playback 8 ms is the owner's choice,
        // below the recommended, and needs the music app's buffer at 128 frames at 48 kHz.
        t.check(DriverValues.inputDefault >= DriverValues.inputRecommended && DriverValues.outputDefault < DriverValues.outputRecommended
                    && DriverValues.outputBuffer(DriverValues.outputDefault, rate: 48000) == 128,
                "the defaults: recording recommended, playback needing a buffer of 128 frames")
        // Places by value, so the checks below read as what the user sees.
        func o(_ ms: Double) -> Int { DriverValues.outputIndex(ms) ?? -1 }
        func i(_ ms: Double) -> Int { DriverValues.inputIndex(ms) ?? -1 }

        // As with the real driver, through a stand-in: nothing here asks Core Audio.
        let d = PretendDriverSettings(pretend: false)
        let m = DriverSettingsModel(source: d)
        var record: [String] = []
        m.onRecord = { record.append($0) }
        t.check(!m.usable && !m.changed, "before the first read nothing can be changed")
        m.refresh()
        t.check(m.usable && !m.changed && m.atDefaults && m.inputIndex == i(8) && m.outputIndex == o(8) && m.correction,
                "the window opens on what the driver has: 8 ms, 8 ms, correction on")
        m.apply()
        t.check(d.asked.isEmpty && record.isEmpty, "Apply with nothing changed sends nothing")

        // A choice stays in the window until Apply.
        m.choose(output: o(12))
        t.check(m.changed && m.outputIndex == o(12) && d.asked.isEmpty && d.state.outputMs == 8, "a moved slider sends nothing")
        m.refresh()
        t.equal(m.outputIndex, o(12), "and a read of the driver doesn't move it back")
        m.apply()
        t.equal(d.asked, ["output 12 ms"], "Apply sends the one changed setting")
        t.check(d.state.outputMs == 12 && !m.changed && !m.busy && m.note?.tone == .good, "the driver has it, and the window says Applied")
        t.equal(record, ["driver settings: playback delay 8 ms → 12 ms: taken"], "the change is in the record")
        m.choose(output: o(12))
        t.check(!m.changed, "choosing what the driver already has changes nothing")

        // All three at once, playback first.
        record = []
        m.choose(input: i(10))
        m.choose(output: o(10))
        m.choose(correction: false)
        t.check(m.changed && m.note == nil, "three choices waiting")
        m.apply()
        t.equal(d.asked.suffix(3).map { $0 }, ["output 10 ms", "input 10 ms", "correction off"], "sent one at a time: playback, recording, correction")
        t.check(d.state.outputMs == 10 && d.state.inputMs == 10 && d.state.correction == false && !m.changed, "all three taken")
        t.equal(record.count, 3, "three lines in the record")

        // The driver refuses: the window goes back to what the driver has, and says so.
        record = []
        d.refuseNext = Int32(bitPattern: 0x6E6F7065)
        m.choose(output: o(16))
        m.apply()
        t.check(d.state.outputMs == 10 && m.outputIndex == o(10) && !m.changed, "a refused value isn't shown as set")
        t.check(m.note?.tone == .bad && (m.note?.text.contains("not a value this driver offers") ?? false), "the window says why: \(m.note?.text ?? "")")
        t.check(record.count == 1 && record[0].contains("REFUSED"), "and the record has the refusal")
        t.check(DriverValues.words(for: Int32(bitPattern: 0x216F626A)).contains("isn't there")
                    && DriverValues.words(for: Int32(bitPattern: 0x77686F3F)).contains("older driver")
                    && DriverValues.words(for: -50).contains("-50"), "Core Audio's errors in plain words")

        // Defaults, then Apply.
        m.chooseDefaults()
        t.check(m.atDefaults && m.changed && d.state.outputMs == 10, "Defaults only moves the controls")
        m.apply()
        t.check(d.state.inputMs == 8 && d.state.outputMs == 8 && d.state.correction == true && !m.changed, "Apply sends them")
        let before = d.asked.count
        m.chooseDefaults()
        m.apply()
        t.equal(d.asked.count, before, "Defaults at the defaults does nothing")

        // Another program changed a setting meanwhile: the window follows, unless a choice waits.
        d.state.inputMs = 5
        m.refresh()
        t.equal(m.inputIndex, i(5), "the window follows the driver")
        m.choose(input: i(3))
        d.state.inputMs = 10
        m.refresh()
        t.check(m.inputIndex == i(3) && m.changed, "but not while the user's own choice waits")
        m.choose(input: i(10))
        t.check(!m.changed, "moved to what the driver has now: nothing left to apply")

        // What the driver measured.
        t.equal(m.runLine, "No sound has run yet. Play or record something, then look here.", "before any sound")
        t.equal(m.room(playback: true).text, "—", "no figure before any sound")
        _ = d.setOutput(ms: 12)
        _ = d.setInput(ms: 5)
        d.pretendRun(running: true, seconds: 200, playbackSpareMs: 4.2, captureSpareMs: 2.9)
        m.refresh()
        t.equal(m.runLine, "Sound is running: 3 min 20 s so far, with 12 ms and 5 ms.", "while sound runs")
        t.check(m.room(playback: true) == .init(tone: .good, text: "least room 4.2 ms · nothing late"), "playback's room: \(m.room(playback: true).text)")
        t.check(m.room(playback: false) == .init(tone: .good, text: "least room 2.9 ms · nothing missing"), "recording's room: \(m.room(playback: false).text)")
        d.pretendRun(running: false, seconds: 754, playbackSpareMs: -1.5, captureSpareMs: 0.6, late: 72)
        m.refresh()
        t.equal(m.runLine, "The last stretch of sound: 12 min 34 s, with 12 ms and 5 ms.", "after it stopped")
        t.check(m.room(playback: true) == .init(tone: .bad, text: "72 frames came too late (1.5 ms of sound)"), "late sound is red: \(m.room(playback: true).text)")
        t.check(m.room(playback: false).tone == .warn, "under 1 ms of room is a warning")
        d.pretendRun(running: true, seconds: 3, playbackSpareMs: nil, captureSpareMs: nil, missing: 480)
        m.refresh()
        t.check(m.room(playback: true) == .init(tone: .plain, text: "not measured yet"), "nothing measured yet")
        t.check(m.room(playback: false) == .init(tone: .bad, text: "480 frames were missing (10.0 ms of sound)"), "missing recording is red: \(m.room(playback: false).text)")
        // A changed delay restarts the sound: the figures start afresh.
        d.pretendRun(running: true, seconds: 90, playbackSpareMs: 0.4, captureSpareMs: 2.9)
        m.refresh()
        m.choose(output: o(16))
        m.apply()
        t.check(m.state.playbackSpare == nil && m.state.seconds == 0 && m.room(playback: true).text == "not measured yet",
                "after a new delay the room is measured anew")
        t.check(DriverSettingsModel.length(12) == "12 s" && DriverSettingsModel.length(200) == "3 min 20 s"
                    && DriverSettingsModel.length(3900) == "1 h 05 min" && DriverSettingsModel.length(-4) == "0 s", "lengths in plain words")

        // The unit is off: Core Audio has no device. Nothing can be chosen or sent.
        let asked = d.asked.count
        d.state = DriverState()
        m.refresh()
        t.check(!m.usable && !m.changed, "with no device nothing can be changed")
        m.choose(output: 0)
        m.choose(input: 0)
        m.choose(correction: false)
        m.chooseDefaults()
        m.apply()
        t.check(d.asked.count == asked && m.outputIndex == o(16), "the controls don't move and nothing is sent")
        t.check(m.runLine == "Nothing measured." && m.room(playback: false).text == "—", "and nothing is shown as measured")
        // An older driver: the device is there, without the settings.
        d.state = DriverState(found: true, haveSettings: false, correction: true)
        m.refresh()
        t.check(!m.usable, "a driver without the settings: nothing can be changed")
        m.apply()
        t.equal(d.asked.count, asked, "and nothing is sent")

        // The pretend unit's driver is another one: the window starts clean on it.
        let p = PretendDriverSettings()
        m.use(p)
        t.check(m.source.isPretend && m.usable && m.atDefaults && m.note == nil && !m.changed, "the pretend driver, at its defaults")
        m.choose(input: i(3))
        m.apply()
        t.check(p.asked == ["input 3 ms"] && d.asked.count == asked, "a change goes to the driver the window shows, not the other")

        // The notes under the sliders (2026-11-20). Every value
        // has a short note under its place and a sentence for when it is chosen, in plain words.
        // They follow the slider at once, before Apply.
        let playbackNotes: [(Double, String, String)] = [
            (2, "buffer 512+", "Needs the music app's buffer at 512 frames or more. Heard delay is about the same as 128 + 8 ms."),
            (4, "buffer 512+", "Needs the music app's buffer at 512 frames or more. At 128 it distorted at the box."),
            (6, "buffer 256+", "Needs the music app's buffer at 256 frames or more."),
            (8, "buffer 128+", "Needs the music app's buffer at 128 frames or more. The default: clean at 128 in the test."),
            (10, "any buffer", "Works with any buffer. The lowest that does."),
            (12, "any buffer", "Works with any buffer, a little more room."),
            (14, "any buffer", "Works with any buffer, more room for a busy Mac."),
            (16, "any buffer", "Works with any buffer, plenty of room."),
            (21.3, "any buffer", "Works with any buffer, the most room."),
        ]
        let recordingNotes: [(Double, String, String)] = [
            (1, "gaps", "Gaps in what you record: 2% of a take was missing in the test. The buffer doesn't change this."),
            (2, "a few clicks", "A few clicks in what you record: 5 frames missing in a take. The buffer doesn't change this."),
            (3, "clean, tight", "Clean in the test, with little to spare."),
            (5, "clean", "Clean, with some room to spare."),
            (8, "clean, roomy", "Clean, with more room for a busy Mac. The default."),
            (10, "most room", "Clean, the most room."),
        ]
        t.check(playbackNotes.map { DriverValues.output[o($0.0)] } == DriverValues.output
                    && recordingNotes.map { DriverValues.input[i($0.0)] } == DriverValues.input, "a note for every value offered, in order")
        for (ms, tick, sentence) in playbackNotes {
            m.choose(output: o(ms))
            t.check(m.outputIndex == o(ms) && DriverSettingsModel.tickNote(playback: true, index: o(ms), rate: m.noteRate) == tick
                        && m.sentence(playback: true) == sentence, "playback \(ms) ms: “\(tick)”, “\(m.sentence(playback: true))”")
        }
        for (ms, tick, sentence) in recordingNotes {
            m.choose(input: i(ms))
            t.check(m.inputIndex == i(ms) && DriverSettingsModel.tickNote(playback: false, index: i(ms), rate: m.noteRate) == tick
                        && m.sentence(playback: false) == sentence, "recording \(ms) ms: “\(tick)”, “\(m.sentence(playback: false))”")
        }
        let allWords = playbackNotes.flatMap { [$0.1, $0.2] } + recordingNotes.flatMap { [$0.1, $0.2] }
        t.check(!allWords.contains { $0.contains("!") || $0.lowercased().contains("warning") || $0.lowercased().contains("expect") },
                "no warnings among them: no “!”, no “warning”, no “expect”")
        // At 96 kHz the buffers are twice as many frames, and so is the "128" of the box.
        t.check([2.0, 4, 6, 8, 10].map { DriverSettingsModel.tickNote(playback: true, index: o($0), rate: 96000) }
                    == ["buffer 1024+", "buffer 1024+", "buffer 512+", "buffer 256+", "any buffer"]
                    && DriverSettingsModel.sentence(playback: true, index: o(4), rate: 96000)
                    == "Needs the music app's buffer at 1024 frames or more. At 256 it distorted at the box.",
                "at 96 kHz the frames double: \(DriverSettingsModel.sentence(playback: true, index: o(4), rate: 96000))")
        t.check(DriverSettingsModel.sentence(playback: false, index: i(1), rate: 96000) == recordingNotes[0].2,
                "the recording notes don't depend on the rate")
        // The low values are sent like any other, and the notes stay with the driver's values.
        m.chooseDefaults()
        m.apply()
        m.choose(output: o(6))
        m.choose(input: i(2))
        let sentBefore = p.asked.count
        t.check(m.changed && p.asked.count == sentBefore && m.sentence(playback: true) == playbackNotes[2].2,
                "the sentence follows the slider before anything is sent")
        m.apply()
        t.check(p.asked.suffix(2).map { $0 } == ["output 6 ms", "input 2 ms"] && p.state.outputMs == 6 && p.state.inputMs == 2 && !m.changed,
                "Apply sends a low value like any other: \(p.asked.suffix(2))")
        t.check(m.sentence(playback: true) == playbackNotes[2].2 && m.sentence(playback: false) == recordingNotes[1].2,
                "and the notes stay while the driver has them")
        m.chooseDefaults()
        t.check(m.sentence(playback: true) == playbackNotes[3].2 && m.sentence(playback: false) == recordingNotes[4].2, "Defaults: the defaults' notes")
        m.apply()
        // The wide slider under the notes: each of its places is picked at its own spot.
        let wide = DriverSettingsView.sliderWidth
        t.check((0..<9).allSatisfy { StepSlider.place(at: 28 + CGFloat($0) / 8 * (wide - 56), count: 9, width: wide, inset: 28) == $0 }
                    && StepSlider.place(at: 0, count: 9, width: wide, inset: 28) == 0 && StepSlider.place(at: wide, count: 9, width: wide, inset: 28) == 8,
                "the playback slider, \(Int(wide)) points wide: nine places, each picked at its own spot")

        // The slider: a click picks the nearest place, and stays inside.
        t.check(StepSlider.place(at: 7, count: 5) == 0 && StepSlider.place(at: StepSlider.width - 7, count: 5) == 4
                    && StepSlider.place(at: StepSlider.width / 2, count: 5) == 2, "the ends and the middle of five places")
        t.check(StepSlider.place(at: 7 + 0.4 * (StepSlider.width - 14), count: 5) == 2
                    && StepSlider.place(at: 7 + 0.6 * (StepSlider.width - 14), count: 5) == 2
                    && StepSlider.place(at: 7 + 0.65 * (StepSlider.width - 14), count: 5) == 3, "the nearest place, from either side")
        // The app's own way to Core Audio refuses a value the driver doesn't offer before
        // anything is asked (so this asks Core Audio nothing).
        t.check(ua101_drv_set_input_ms(4) == Int32(bitPattern: 0x6E6F7065) && ua101_drv_set_output_ms(9) == Int32(bitPattern: 0x6E6F7065)
                    && ua101_drv_set_input_ms(0) == Int32(bitPattern: 0x6E6F7065) && ua101_drv_set_output_ms(0) == Int32(bitPattern: 0x6E6F7065)
                    && ua101_drv_set_output_ms(.nan) == Int32(bitPattern: 0x6E6F7065), "a value that isn't offered never travels")
        t.check(StepSlider.place(at: -50, count: 4) == 0 && StepSlider.place(at: 900, count: 4) == 3 && StepSlider.place(at: 60, count: 4) == 1
                    && StepSlider.place(at: 10, count: 1) == 0, "clamped, and one place alone")
        // Nine places (the playback delays): each one's own spot picks it.
        t.check((0..<9).allSatisfy { StepSlider.place(at: 7 + CGFloat($0) / 8 * (StepSlider.width - 14), count: 9) == $0 },
                "nine places, each picked at its own spot")
    }

    // ---- 16c
    if t.begin("backup, put back and Reset Mixer on the box") {
        let ok = Int32(PANEL_OK), busy = Int32(PANEL_ERR_BUSY)
        let fVolume = Int32(PANEL_F_VOLUME), fPan = Int32(PANEL_F_PAN), fLink = Int32(PANEL_F_LINK), fPatch = Int32(PANEL_F_PATCH)
        func simBlock(_ u: PretendUnit) -> [UInt8] {
            var block = [UInt8](repeating: 0, count: 256)
            panel_sim_block(u.sim, &block)
            return block
        }
        func url(_ b: Bench, _ name: String) -> URL { b.backups.appendingPathComponent(name) }
        func file(_ b: Bench, _ name: String) -> SettingsBackup? {
            if case .success(let backup) = SettingsBackup.read(url(b, name)) { return backup }
            return nil
        }
        /// A backup file made by hand, with a true check: what a real file would be.
        func write(_ b: Bench, _ name: String, _ block: [UInt8], unit: String = "UA-101") -> URL {
            try? FileManager.default.createDirectory(at: b.backups, withIntermediateDirectories: true)
            let u = url(b, name)
            try? SettingsBackup(block: block, unit: unit, saved: "2026-11-20 14:05:07", why: "Save Backup").text().write(to: u, atomically: true, encoding: .utf8)
            return u
        }
        func writeText(_ b: Bench, _ name: String, _ text: String) -> URL {
            try? FileManager.default.createDirectory(at: b.backups, withIntermediateDirectories: true)
            let u = url(b, name)
            try? text.write(to: u, atomically: true, encoding: .utf8)
            return u
        }
        func real(passes: UInt64 = FakeHelper.version14, passesOn: UInt64 = FakeHelper.version14On) -> Bench {
            let g = Bench()
            g.fake.passes = passes
            g.fake.passesOn = passesOn
            g.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
            g.chooseReal()
            g.model.allowChanges(true)
            g.settle()
            return g
        }
        /// A believable set-up on the UA-101 behind the stand-in helper.
        func dress(_ g: Bench) {
            g.model.setVolumeExact(strip(inp, 0), 0x100001)
            g.settle()
            g.model.setPanExact(strip(inp, 2), 0x1234)
            g.settle()
            g.model.setMonitor(strip(out, 4), on: false)
            g.settle()
            g.model.setPatch(output: 1, source: 3)
            g.settle()
            g.model.toggleSolo(strip(inp, 1))
            g.settle()
            // INPUT 3-4 linked while the two hold different volumes: the link changes neither.
            g.model.setVolumeExact(strip(inp, 3), 0x16A09E)
            g.settle()
            g.model.setLink(group: inp, pair: 1, on: true)
            g.settle()
        }
        let pauseMsg = PanelModel.savingMessage(resume: false), resumeMsg = PanelModel.savingMessage(resume: true)

        // The file: written, read back, and refused when it isn't exactly a backup.
        var sample = [UInt8](repeating: 0, count: 256)
        panel_factory_block(&sample)
        let made = SettingsBackup(block: sample, unit: "UA-101", saved: "2026-11-20 14:05:07", why: "Save Backup")
        let text = made.text()
        if verbose { print(text) }
        t.check(SettingsBackup.parse(text) == .success(made), "a backup's text reads back as the same block, unit, time and reason")
        t.check(text.hasPrefix("UA-101 settings backup\nFormat: 1\nSaved: 2026-11-20 14:05:07\nUnit: UA-101\nWhy: Save Backup\nCheck: ")
                    && text.contains("\n000: 01 01 01 01 01 00 00 02 03 04 05 06 07 08 09 0A\n") && text.contains("\n240: 00 00 02 00 00 00 00 00 00 00 00 00 00 00 00 00\n"),
                "plain text: the bytes 16 a line, numbered")
        t.check(text.contains("Patch bay, OUTPUT 1-2: WAVE OUT 1-2") && text.contains("INPUT 2: volume 0.0 dB (0x200000), pan R100 (0x8000), mute off, not linked")
                    && text.contains("MONITOR OUT L: volume 0.0 dB (0x200000), mute off, linked (MONITOR OUT L-R)") && text.contains("Solo, INPUT: none")
                    && text.contains("sample rate 48 kHz, SOFT CTRL out, MONO out"),
                "and a list a person can read: the patch bay, every channel's volume, pan, mute and link, the solos")
        let cut = text.components(separatedBy: "\n").filter { !$0.hasPrefix("240: ") && !$0.hasPrefix("224: ") }.joined(separator: "\n")
        t.check(SettingsBackup.parse(cut) == .failure(.cutShort(224)), "a file cut short (224 of 256 bytes): refused, with the count")
        t.check(SettingsBackup.parse(text.components(separatedBy: "\n").prefix(6).joined(separator: "\n")) == .failure(.cutShort(0)), "a file with no bytes at all: refused")
        t.check(SettingsBackup.parse(text.components(separatedBy: "\n").filter { !$0.hasPrefix("096: ") }.joined(separator: "\n")) == .failure(.cutShort(240)), "a row missing in the middle: refused")
        let rows = text.components(separatedBy: "\n")
        if let a = rows.firstIndex(where: { $0.hasPrefix("000: ") }), let z = rows.firstIndex(where: { $0.hasPrefix("016: ") }) {
            var swapped = rows
            swapped.swapAt(a, z)
            t.check(SettingsBackup.parse(swapped.joined(separator: "\n")) == .failure(.damaged), "two rows changed over: refused, the check doesn't fit")
        }
        t.check(SettingsBackup.parse(text.replacingOccurrences(of: "\n016: 0B", with: "\n016: 0A")) == .failure(.damaged), "one byte changed by hand: refused, the check doesn't fit")
        t.check(SettingsBackup.parse(text.replacingOccurrences(of: "\n016: 0B", with: "\n016: 0G")) == .failure(.damaged), "a byte that isn't a number: refused")
        t.check(SettingsBackup.parse(text.replacingOccurrences(of: "\n000: 01 01 01 01 01 00 ", with: "\n000: 01 01 01 01 01 ZZ ")) == .failure(.damaged),
                "letters where a 00 was: refused, not read as 00")
        t.check(SettingsBackup.parse(text.replacingOccurrences(of: "UA-101 settings backup\n", with: "Some other settings backup\n")) == .failure(.notABackup),
                "another first line, the rest in order: not a backup")
        t.check(SettingsBackup.parse(text.replacingOccurrences(of: "\n\nWhat it holds", with: "\n256: 00\n\nWhat it holds")) == .failure(.damaged), "more than 256 bytes: refused")
        t.check(SettingsBackup.parse("Dear diary\n") == .failure(.notABackup) && SettingsBackup.parse("") == .failure(.notABackup), "another file: not a backup")
        t.check(SettingsBackup.parse(text.replacingOccurrences(of: "Format: 1", with: "Format: 2")) == .failure(.newerFormat("2")), "a later version's file: refused, and it says so")
        var other = sample
        other[200] ^= 1
        t.check(SettingsBackup.check(other) != SettingsBackup.check(sample), "the check follows every byte")
        let when = Bench.start
        t.check(SettingsBackup.fileName(at: when, why: "Save Backup", pretend: false) { _ in false } == "2026-11-16 18.30.ua101"
                    && SettingsBackup.fileName(at: when, why: "before Reset Mixer", pretend: false) { _ in false } == "2026-11-16 18.30 before Reset Mixer.ua101"
                    && SettingsBackup.fileName(at: when, why: "Save Backup", pretend: true) { _ in false } == "2026-11-16 18.30 pretend unit.ua101"
                    && SettingsBackup.fileName(at: when, why: "Save Backup", pretend: false) { $0 == "2026-11-16 18.30.ua101" } == "2026-11-16 18.30 (2).ua101",
                "the file's name: the date and time, why it was saved by itself, and a second one in the same minute gets (2)")

        // What the helper must pass on for Reset Mixer and Put Back (1c).
        let v14 = Allowed(passes: FakeHelper.version14, passesOn: FakeHelper.version14On)
        let v13 = Allowed(passes: FakeHelper.version13, passesOn: FakeHelper.version13On)
        t.check(v14.mixer && !v13.mixer && Allowed.all.mixer && !Allowed.nothing.mixer,
                "the whole mixer at once: allowed with the installed helper's list (link 14 to 16), not with an older one's")
        t.check(!Allowed(passes: FakeHelper.version14 & ~UInt64(PANEL_PASS_SAVING), passesOn: FakeHelper.version14On).mixer, "not without the pause of the unit's saving")
        t.check(!Allowed(passes: FakeHelper.version14 & ~(UInt64(PANEL_PASS_VOLUME_CHANNEL_0) << 7), passesOn: FakeHelper.version14On).mixer
                    && !Allowed(passes: FakeHelper.version14 & ~(PANEL_PASS_PAN_CHANNEL_0 << 21), passesOn: FakeHelper.version14On).mixer
                    && !Allowed(passes: FakeHelper.version14, passesOn: FakeHelper.version14On & ~(PANEL_PASS_ON_CHANNEL_0 << 0)).mixer
                    && !Allowed(passes: FakeHelper.version14, passesOn: FakeHelper.version14On & ~(PANEL_PASS_LINK_PAIR_0 << 10)).mixer
                    && !Allowed(passes: FakeHelper.version14, passesOn: FakeHelper.version14On & ~(PANEL_PASS_SOLO_GROUP_1 << 1)).mixer,
                "and not with one volume, one pan, one on/off, one link or one solo missing from the helper's list")

        // Before the unit's settings are known there is nothing to compare a backup with.
        let cold = Bench(connect: false)
        t.check(cold.model.preparePutBack(from: write(cold, "kept.ua101", sample)) == Int32(PANEL_ERR_NOT_READY) && cold.model.pendingPutBack == nil
                    && cold.model.saveBackup() == Int32(PANEL_ERR_NOT_READY) && !cold.model.jobRunning,
                "no unit yet: Save Backup and Put Back are refused, the settings aren't known")

        // Save Backup on the pretend unit: only a read, allowed while read-only.
        let b = Bench()
        let reads = panel_sim_reads(b.unit.sim)
        // Something changed the unit behind the app's back: the backup must hold what the unit
        // holds, not what the app last saw.
        PanelModel.volumeMessage(strip(inp, 4), minus12).withUnsafeBufferPointer { panel_sim_receive(b.unit.sim, $0.baseAddress, $0.count) }
        t.equal(b.model.snap.channels[inp][4].volume, unity, "the app's picture is the older one")
        t.equal(b.model.saveBackup(), ok, "Save Backup: accepted while read-only")
        t.check(b.model.jobRunning && b.backupFiles.isEmpty, "nothing is written before the fresh read has come")
        t.equal(b.model.saveBackup(), busy, "a second one meanwhile: refused")
        b.settle()
        let firstName = "2026-11-16 18.30 pretend unit.ua101"
        t.check(!b.model.jobRunning && b.backupFiles == [firstName], "one file: \(b.backupFiles)")
        let first = file(b, firstName)
        t.check(first?.block == simBlock(b.unit) && first?.unit == "Pretend unit" && first?.why == "Save Backup" && first?.saved == "2026-11-16 18:30:02",
                "it holds the unit's 256 bytes, which unit, why and when")
        t.check(first.map { SettingsBackup.value($0.block, fVolume, inp, 4) } == minus12 && b.model.snap.channels[inp][4].volume == minus12,
                "from a fresh read: the change the app hadn't seen is in it")
        t.check(panel_sim_reads(b.unit.sim) == reads + 1 && panel_sim_sets_applied(b.unit.sim) == 1 && b.model.snap.setsSent == 0 && panel_sim_saving_paused(b.unit.sim) == 0,
                "one read went to the unit and nothing else")
        t.check(b.model.backupNote?.contains("Backup saved: “\(firstName)”") == true && b.model.backupNote?.contains("all 256 bytes") == true
                    && b.messages.now.contains { $0.slot == .backup && !$0.warning },
                "the Messages window says where: \(b.model.backupNote ?? "nil")")
        b.model.saveBackup()
        b.settle()
        t.equal(b.backupFiles.count, 2, "a second backup in the same minute doesn't overwrite the first: \(b.backupFiles)")
        // No folder, or a folder that can't be written: said, and nothing else happens.
        let folder = b.model.backupFolder
        b.model.backupFolder = nil
        t.check(b.model.saveBackup() == Int32(PANEL_ERR_VALUE) && b.model.refusal?.text.contains("no folder") == true && !b.model.jobRunning, "no folder for backups: refused")
        let blocker = writeText(b, "not-a-folder", "x")
        b.model.backupFolder = blocker.appendingPathComponent("inside")
        b.model.saveBackup()
        b.settle()
        t.check(b.model.backupNote?.contains("NOT DONE. The backup couldn't be saved") == true && b.messages.now.contains { $0.slot == .backup && $0.warning },
                "a folder that can't be made: a warning, in capitals: \(b.model.backupNote ?? "nil")")
        b.model.backupFolder = folder
        try? FileManager.default.removeItem(at: blocker)

        // The whole way round on the pretend unit: backup, change things, put back, byte for byte.
        b.openUp()
        b.dressUp()
        b.model.setPatch(output: 1, source: 3)
        b.model.toggleSolo(strip(inp, 2))
        b.settle()
        b.model.setLink(group: inp, pair: 1, on: true) // INPUT 3-4 linked, at -12 and -18 dB
        b.settle()
        let kept = simBlock(b.unit)
        t.check(SettingsBackup.value(kept, fLink, inp, 1) == 1 && SettingsBackup.value(kept, fVolume, inp, 2) != SettingsBackup.value(kept, fVolume, inp, 3),
                "the set-up holds a linked pair with two different volumes")
        let before = Set(b.backupFiles)
        b.model.saveBackup()
        b.settle()
        let keptName = Set(b.backupFiles).subtracting(before).first ?? ""
        t.check(file(b, keptName)?.block == kept, "backup saved: \(keptName)")
        // Reset Mixer saves a backup by itself first, and says what the unit reads back as.
        t.equal(b.model.resetMixer(), ok, "Reset Mixer: accepted")
        b.settle()
        let resetName = "2026-11-16 18.30 before Reset Mixer pretend unit.ua101"
        t.check(file(b, resetName)?.block == kept && file(b, resetName)?.why == "before Reset Mixer", "Reset Mixer saved the settings from before by itself: \(b.backupFiles)")
        var factory = [UInt8](repeating: 0, count: 256)
        panel_factory_block(&factory)
        t.check(Array(simBlock(b.unit)[22...247]) == Array(factory[22...247]) && simBlock(b.unit)[13] == 3, "the mixer is the factory one, byte for byte; the patch bay wasn't touched")
        t.check(b.model.resetNote?.contains("The unit's mixer reads back as the factory block, every byte (d[22] to d[247])") == true
                    && b.model.resetNote?.contains("The patch bay and the switches weren't touched") == true && b.model.resetNote?.contains(resetName) == true
                    && b.messages.now.contains { $0.slot == .reset && !$0.warning },
                "and it says so: \(b.model.resetNote ?? "nil")")
        b.model.patchPreset(hardThru: true)
        b.settle()
        let changed = simBlock(b.unit)
        // Step 1: the file is read and what would change is listed. Nothing is sent.
        var sets = panel_sim_sets_applied(b.unit.sim)
        t.equal(b.model.preparePutBack(from: url(b, keptName)), ok, "Put Back, step 1: the file is taken")
        let plan = b.model.pendingPutBack?.plan
        let fields = (0..<256).filter { kept[$0] != changed[$0] }
        t.check(plan != nil && !fields.isEmpty && plan?.cantPutBack.isEmpty == true && plan?.held.isEmpty == true, "it holds a plan; nothing that can't be put back")
        t.check(plan?.changes.contains(PutBackPlan.Change(what: "Patch bay, OUTPUT 3-4", from: "INPUT 3-4", to: "INPUT 3-4")) == false
                    && plan?.changes.contains(PutBackPlan.Change(what: "Patch bay, OUTPUT 1-2", from: "INPUT 1-2", to: "WAVE OUT 1-2")) == true
                    && plan?.changes.contains(PutBackPlan.Change(what: "INPUT 1 volume", from: "0.0 dB", to: "−6.0 dB")) == true
                    && plan?.changes.contains(PutBackPlan.Change(what: "INPUT 3 pan", from: "L100", to: "L38")) == true
                    && plan?.changes.contains(PutBackPlan.Change(what: "INPUT 5 mute", from: "off", to: "on")) == true
                    && plan?.changes.contains(PutBackPlan.Change(what: "INPUT 3-4 link", from: "not linked", to: "linked")) == true
                    && plan?.changes.contains(PutBackPlan.Change(what: "INPUT solo", from: "none", to: "INPUT 3")) == true,
                "what will change, setting by setting: \(plan?.changes.map { "\($0.what) \($0.from)→\($0.to)" } ?? [])")
        b.settle(5_000_000)
        t.check(panel_sim_sets_applied(b.unit.sim) == sets && simBlock(b.unit) == changed && !b.model.jobRunning, "step 1 sent nothing")
        b.model.cancelPutBack()
        t.check(b.model.pendingPutBack == nil && b.model.confirmPutBack() == Int32(PANEL_ERR_TICKET), "Cancel: nothing to confirm any more")
        b.settle()
        t.check(simBlock(b.unit) == changed, "and nothing was sent")
        // Step 2.
        b.model.preparePutBack(from: url(b, keptName))
        let saves = panel_sim_saves(b.unit.sim)
        sets = panel_sim_sets_applied(b.unit.sim)
        t.equal(b.model.confirmPutBack(), ok, "Put Back, step 2: accepted")
        t.check(b.model.pendingPutBack == nil && b.model.jobRunning && !b.model.jobSending, "under way: reading the settings afresh")
        t.equal(b.model.resetMixer(), busy, "Reset Mixer meanwhile: refused")
        b.settle(20_000)
        // While it sends, nothing else is taken: it would land in the middle of the put back.
        var ended = 0
        b.model.onJobEnd = { ended += 1 }
        t.check(b.model.jobSending && b.model.setVolume(strip(out, 6), minus12) == Int32(PANEL_ERR_READ_ONLY)
                    && b.model.refusal?.kind == .busy && b.model.refusal?.text.contains("Open Backup is under way") == true
                    && b.model.setPatch(output: 4, source: 1) == Int32(PANEL_ERR_READ_ONLY) && b.model.toggleSolo(strip(out, 6)) == Int32(PANEL_ERR_READ_ONLY)
                    && b.model.setLink(group: out, pair: 3, on: false) == Int32(PANEL_ERR_READ_ONLY) && b.model.setMonitor(strip(out, 6), on: false) == Int32(PANEL_ERR_READ_ONLY)
                    && b.model.setPan(strip(out, 6), 0x4000) == Int32(PANEL_ERR_READ_ONLY) && b.model.patchPreset(hardThru: false) == Int32(PANEL_ERR_READ_ONLY)
                    && b.model.pauseSaving() == Int32(PANEL_ERR_READ_ONLY),
                "a fader, a knob, a button or the patch bay while it sends: not done, with the reason")
        b.settle()
        t.check(ended == 1 && !b.model.jobSending && b.model.onJobEnd == nil, "the app is told once when it has ended (quitting waits for that)")
        t.check(simBlock(b.unit) == kept, "the unit holds the backup again, byte for byte, all 256")
        t.check(b.model.putBackNote?.contains("The unit's settings match the backup, all 256 bytes") == true && b.model.putBackNote?.contains("“\(keptName)”") == true
                    && b.model.putBackNote?.contains("The switches on the box are never put back") == true
                    && b.messages.now.contains { $0.slot == .putBack && !$0.warning } && b.model.disagreement == nil,
                "and says so, with the plain word about the switches: \(b.model.putBackNote ?? "nil")")
        t.equal(Int(panel_sim_sets_applied(b.unit.sim) - sets), plan?.changes.count ?? -1, "one message for each setting that differed, no more")
        t.check(panel_sim_saves(b.unit.sim) == saves + 1 && panel_sim_saving_paused(b.unit.sim) == 0 && !b.model.snap.busy,
                "all of it inside one pause of the unit's saving: the unit saved once, and saves again")
        let putName = "2026-11-16 18.30 before Open Backup pretend unit.ua101"
        t.check(file(b, putName)?.block == changed, "the settings from before the put back were saved by themselves: \(b.backupFiles)")
        // And back again, from the backup the put back made.
        b.model.preparePutBack(from: url(b, putName))
        b.model.confirmPutBack()
        b.settle()
        t.check(simBlock(b.unit) == changed, "putting that one back undoes the put back, byte for byte")
        // Nothing differs: nothing is sent.
        sets = panel_sim_sets_applied(b.unit.sim)
        b.model.preparePutBack(from: url(b, putName))
        t.check(b.model.pendingPutBack?.plan.changes.isEmpty == true, "the same file again: nothing would change")
        b.model.confirmPutBack()
        b.settle()
        t.check(panel_sim_sets_applied(b.unit.sim) == sets && b.model.putBackNote?.contains("already held everything this app can put back. Nothing was sent") == true
                    && b.model.putBackNote?.contains("match the backup, all 256 bytes") == true, "nothing sent, and it says so: \(b.model.putBackNote ?? "nil")")

        // A linked pair whose two channels hold different volumes, onto a unit where the pair
        // is linked already: each channel gets its own (a linked pair is sent both at once).
        b.model.preparePutBack(from: url(b, keptName))
        b.model.confirmPutBack()
        b.settle()
        var crossed = kept
        let v3 = PanelModel.volumeOffset(strip(inp, 2)), v4 = PanelModel.volumeOffset(strip(inp, 3))
        crossed.replaceSubrange(v3..<v3 + 6, with: kept[v4..<v4 + 6])
        crossed.replaceSubrange(v4..<v4 + 6, with: kept[v3..<v3 + 6])
        b.model.preparePutBack(from: write(b, "crossed.ua101", crossed, unit: "Pretend unit"))
        t.equal(b.model.pendingPutBack?.plan.changes.map { $0.what } ?? [], ["INPUT 3 volume", "INPUT 4 volume"], "the two volumes of the linked pair INPUT 3-4 changed over")
        b.model.confirmPutBack()
        b.settle()
        t.check(simBlock(b.unit) == crossed && b.model.putBackNote?.contains("match the backup, all 256 bytes") == true && b.model.disagreement == nil,
                "each channel of the linked pair holds its own volume, and the pair is still linked: \(b.model.putBackNote ?? "nil")")
        b.model.preparePutBack(from: url(b, putName))
        b.model.confirmPutBack()
        b.settle()
        t.check(simBlock(b.unit) == changed, "back to where this began")
        // The unit stops answering while the dialog is up: the choice goes, nothing is sent.
        b.model.preparePutBack(from: url(b, keptName))
        b.unit.setSilent(true)
        b.model.refresh()
        b.settle(6_000_000)
        t.check(b.model.snap.state == .silent && b.model.putBackBlocker()?.contains("aren't known right now") == true
                    && b.model.confirmPutBack() == Int32(PANEL_ERR_NOT_READY) && !b.model.jobRunning, "the unit stops answering: step 2 is refused")
        b.unit.setSilent(false)
        b.settle(6_000_000)
        b.model.disconnect()
        t.check(b.model.pendingPutBack == nil && b.model.confirmPutBack() == Int32(PANEL_ERR_TICKET), "the way to the unit goes: the chosen backup is dropped")
        b.model.connect()
        b.settle()

        // What stops step 2: said plainly, nothing sent, the choice kept.
        b.model.preparePutBack(from: url(b, keptName))
        sets = panel_sim_sets_applied(b.unit.sim)
        b.model.allowChanges(false)
        t.check(b.model.putBackBlocker()?.contains("Changes are switched off") == true && b.model.confirmPutBack() == Int32(PANEL_ERR_READ_ONLY)
                    && b.model.pendingPutBack != nil && !b.model.jobRunning, "Changes off: refused, read-only")
        b.model.allowChanges(true)
        b.unit.press(Int32(PANEL_SWITCH_SOFT), in: false)
        b.settle()
        t.check(b.model.putBackBlocker()?.contains("SOFT CTRL is out") == true && b.model.confirmPutBack() == Int32(PANEL_ERR_SOFT_CTRL)
                    && b.model.refusal?.kind == .softControl && b.model.pendingPutBack != nil, "SOFT CTRL out: refused")
        t.equal(b.model.resetMixer(), Int32(PANEL_ERR_SOFT_CTRL), "and Reset Mixer too")
        b.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        b.settle()
        t.check(b.model.putBackBlocker() == nil, "SOFT CTRL in, changes on: it can go")
        // SOFT CTRL goes out, or changes go off, after the button but before the fresh read is in.
        b.unit.press(Int32(PANEL_SWITCH_SOFT), in: false)
        t.equal(b.model.confirmPutBack(), ok, "the app hasn't seen the switch move yet")
        b.settle()
        t.check(b.model.putBackNote?.contains("SOFT CTRL is out on the unit") == true && b.model.putBackNote?.contains("Nothing was sent") == true
                    && panel_sim_sets_applied(b.unit.sim) == sets && simBlock(b.unit)[12] == changed[12],
                "the fresh read shows SOFT CTRL out: nothing is sent, not even the patch bay: \(b.model.putBackNote ?? "nil")")
        b.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        b.settle()
        b.model.preparePutBack(from: url(b, keptName))
        b.model.confirmPutBack()
        b.model.allowChanges(false)
        b.settle()
        t.check(b.model.putBackNote?.contains("Changes were switched off meanwhile. Nothing was sent") == true && panel_sim_sets_applied(b.unit.sim) == sets,
                "changes switched off before the fresh read came: nothing is sent: \(b.model.putBackNote ?? "nil")")
        b.model.allowChanges(true)
        // The backup of the moment before can't be saved: nothing is changed.
        b.model.preparePutBack(from: url(b, keptName))
        b.model.backupFolder = URL(fileURLWithPath: "/dev/null/ua101")
        b.model.confirmPutBack()
        b.settle()
        t.check(b.model.putBackNote?.contains("NOT DONE. The backup of the settings as they are now couldn't be saved") == true
                    && b.model.putBackNote?.contains("Nothing was sent") == true && panel_sim_sets_applied(b.unit.sim) == sets
                    && b.messages.now.contains { $0.slot == .putBack && $0.warning },
                "no backup of the moment before, no put back: \(b.model.putBackNote ?? "nil")")
        t.equal(b.model.resetMixer(), ok, "Reset Mixer with nowhere to save")
        b.settle()
        t.check(b.model.resetNote?.contains("NOT DONE") == true && panel_sim_sets_applied(b.unit.sim) == sets, "the same: nothing is changed")
        b.model.backupFolder = folder
        // The unit's saving paused by hand: resumed first.
        b.model.pauseSaving()
        b.settle()
        t.check(b.model.resetMixer() == busy && b.model.refusal?.text.contains("Resume it first") == true, "the unit's saving is paused by hand: refused until it is resumed")
        b.model.resumeSaving()
        b.settle()
        // The unit stops answering before the fresh read.
        b.unit.setSilent(true)
        b.model.saveBackup()
        b.settle(6_000_000)
        t.check(b.model.backupNote?.contains("stopped answering before its settings were read") == true && !b.model.jobRunning, "a unit that doesn't answer: no backup, and it says so")
        b.unit.setSilent(false)
        b.settle(6_000_000)
        // Switching changes off while it sends: its last changes leave first.
        b.model.preparePutBack(from: url(b, keptName))
        b.model.confirmPutBack()
        b.settle(20_000) // the fresh read is in and the first changes have left
        t.check(b.model.jobRunning && panel_sim_sets_applied(b.unit.sim) > sets && simBlock(b.unit) != kept, "part of the put back has left")
        b.model.allowChanges(false)
        t.check(b.model.snap.writesAllowed && b.model.refusal?.text.contains("switch off by themselves as soon as Open Backup has finished") == true,
                "Switch off in the middle: it waits for the last changes")
        b.settle()
        t.check(simBlock(b.unit) == kept && !b.model.snap.writesAllowed && panel_sim_saving_paused(b.unit.sim) == 0
                    && b.model.putBackNote?.contains("match the backup, all 256 bytes") == true,
                "the unit holds the whole backup, its saving runs again, and then changes are off")
        // The unit doesn't take a change: said in capitals, a warning.
        b.model.allowChanges(true)
        b.unit.ignore(kind: Int(PANEL_F_PAN))
        b.model.resetMixer()
        b.settle()
        t.check(b.model.resetNote?.contains("THE UNIT DIFFERS from what the reset should give in") == true && b.model.resetNote?.contains("(INPUT 3 pan)") == true
                    && b.messages.now.contains { $0.slot == .reset && $0.warning },
                "Reset Mixer when the unit keeps a pan: a warning that names the bytes: \(b.model.resetNote ?? "nil")")
        b.model.dismissDisagreement()
        b.model.preparePutBack(from: url(b, keptName))
        b.model.confirmPutBack()
        b.settle()
        t.check(b.model.putBackNote?.contains("match the backup, all 256 bytes") == true, "the pans never moved, so the put back matches again")
        b.unit.ignore(kind: -1)
        b.model.resetMixer()
        b.settle()
        b.unit.ignore(kind: Int(PANEL_F_VOLUME))
        b.model.preparePutBack(from: url(b, keptName))
        b.model.confirmPutBack()
        b.settle()
        t.check(b.model.putBackNote?.contains("THE UNIT DIFFERS from the backup in") == true && b.model.putBackNote?.contains("that should match") == true
                    && b.model.putBackNote?.contains("(MONITOR OUT L volume)") == true && b.messages.now.contains { $0.slot == .putBack && $0.warning },
                "Put Back when the unit keeps its volumes: a warning that names the bytes: \(b.model.putBackNote ?? "nil")")
        b.unit.ignore(kind: -1)
        b.model.dismissDisagreement()

        // Files that must be refused at step 1, each with nothing sent.
        sets = panel_sim_sets_applied(b.unit.sim)
        func refused(_ u: URL, _ words: String, _ what: String) {
            let r = b.model.preparePutBack(from: u)
            t.check(r == Int32(PANEL_ERR_VALUE) && b.model.pendingPutBack == nil && b.model.refusal?.text.contains(words) == true
                        && b.model.refusal?.text.contains("Nothing was sent") == true, "\(what): \(b.model.refusal?.text ?? "nil")")
        }
        refused(url(b, "no-such-file.ua101"), "can't be read", "a file that isn't there")
        refused(writeText(b, "cut.ua101", cut), "cut short: it holds 224 of the 256 bytes", "a file cut short")
        refused(writeText(b, "edited.ua101", text.replacingOccurrences(of: "\n016: 0B", with: "\n016: 0A")), "damaged or was changed by hand", "a file changed by hand")
        refused(writeText(b, "diary.ua101", "Dear diary\n"), "isn't a UA-101 settings backup", "another file")
        var flags = kept
        flags[0] = 0
        refused(write(b, "layout.ua101", flags), "doesn't fit the unit as it is now: the four flags", "a backup from a different box layout")
        var fast = kept
        fast[4] = (fast[4] & ~0x07) | 4
        refused(write(b, "192k.ua101", fast), "it was made at 192 kHz and the unit is at 48 kHz", "a backup made at 192 kHz, the unit at 48 kHz")
        var badPan = kept
        badPan[PanelModel.panOffset(strip(inp, 2))] = 0x09
        refused(write(b, "bad-pan.ua101", badPan), "a value the unit never takes (INPUT 3 pan", "a pan past its end")
        var badDigit = kept
        badDigit[PanelModel.volumeOffset(strip(out, 1)) + 5] = 0x1F
        refused(write(b, "bad-digit.ua101", badDigit), "a value the unit never takes (OUTPUT 2 volume", "a volume digit above F")
        var badVolume = kept
        badVolume[PanelModel.volumeOffset(strip(out, 1))] = 0x08
        refused(write(b, "bad-volume.ua101", badVolume), "a value the unit never takes (OUTPUT 2 volume", "a volume above the highest there is")
        var badPatch = kept
        badPatch[14] = 0
        refused(write(b, "bad-patch.ua101", badPatch), "a value the unit never takes (patch bay, OUTPUT 5-6", "a patch bay source that doesn't exist")
        var badSolo = kept
        badSolo[0x17] = 11
        refused(write(b, "bad-solo.ua101", badSolo), "a value the unit never takes (OUTPUT solo", "a solo past channel 10")
        b.settle()
        t.check(panel_sim_sets_applied(b.unit.sim) == sets && !b.model.jobRunning, "none of those sent anything")
        // The layout changes between the two steps (the rate knob, then off and on): caught by the fresh read.
        b.model.preparePutBack(from: url(b, keptName))
        b.unit.setRate(code: 4)
        t.equal(b.model.confirmPutBack(), ok, "the app hasn't seen the new rate yet")
        b.settle()
        t.check(b.model.putBackNote?.contains("doesn't fit the unit as it is now") == true && b.model.putBackNote?.contains("Nothing was sent") == true
                    && panel_sim_sets_applied(b.unit.sim) == sets, "the fresh read shows 192 kHz: nothing is sent: \(b.model.putBackNote ?? "nil")")
        // What can't be put back: said before and after, not as a fault. A volume above 0 dB is put back as it is (since 2026-11-27).
        let c = Bench()
        c.openUp()
        c.unit.press(Int32(PANEL_SWITCH_MONO), in: true)
        c.settle()
        var loud = simBlock(c.unit)
        loud[6] &= ~0x02 // MONO was out when this backup was made
        loud[9] = 0x05   // a patch bay row this app doesn't change
        loud.replaceSubrange(PanelModel.volumeOffset(strip(inp, 2))..<PanelModel.volumeOffset(strip(inp, 2)) + 6, with: PanelModel.volumeDigits(0x400000)) // +6 dB
        loud.replaceSubrange(PanelModel.panOffset(strip(inp, 2))..<PanelModel.panOffset(strip(inp, 2)) + 4, with: PanelModel.panDigits(0x4000))
        t.equal(c.model.preparePutBack(from: write(c, "loud.ua101", loud, unit: "Pretend unit")), ok, "a backup with MONO out, another patch row and a volume of +6 dB: taken")
        let lp = c.model.pendingPutBack?.plan
        t.check(lp?.held.isEmpty == true
                    && lp?.cantPutBack.contains { $0.contains("MONO was out") && $0.contains("switches on the box") } == true
                    && lp?.cantPutBack.contains { $0.contains("d[9] (a patch bay row this app doesn't change): the backup holds 05, the unit 04") } == true
                    && lp?.changes.map(\.what) == ["INPUT 3 volume", "INPUT 3 pan"] && lp?.changes.first?.to.hasPrefix("+6.0") == true,
                "step 1: the +6 dB and the pan are put back, and it says what can't be: \(lp?.held ?? []) \(lp?.cantPutBack ?? []) \(lp?.changes.map { $0.what } ?? [])")
        c.model.confirmPutBack()
        c.settle()
        let cNote = c.model.putBackNote ?? "nil"
        t.check(unitValue(c, fVolume, inp, 2) == 0x400000 && unitValue(c, fPan, inp, 2) == 0x4000 && simBlock(c.unit)[6] & 0x02 != 0 && simBlock(c.unit)[9] == 4,
                "the volume (+6 dB) and the pan are put back; MONO stays in, the other row stays")
        t.check(cNote.contains("2 changes sent") && cNote.contains("The unit matches the backup in everything this app can put back. 2 bytes still differ")
                    && cNote.contains("d[6] 06, backup 04 (the MONO and SOFT CTRL switches)") && cNote.contains("d[9] 04, backup 05")
                    && cNote.contains("Not put back: The switches: in the backup MONO was out")
                    && !cNote.contains("THE UNIT DIFFERS") && !MessageCenter.isWarning(cNote),
                "afterwards: the bytes that differ, each with why, and no warning: \(cNote)")

        // 192 kHz: the mixer has six channels of each kind; the others are left alone.
        let f = Bench()
        f.unit.setRate(code: 4)
        f.model.reconnect()
        f.openUp()
        f.model.setVolume(strip(inp, 5), minus6)
        f.model.setLink(group: out, pair: 2, on: false)
        f.settle()
        let fKept = simBlock(f.unit)
        f.model.saveBackup()
        f.settle()
        let fName = f.backupFiles.first ?? ""
        t.equal(f.model.resetMixer(), ok, "192 kHz: Reset Mixer accepted")
        f.settle()
        t.check(unitValue(f, fVolume, inp, 5) == unity && unitValue(f, fLink, out, 2) == 1 && f.model.resetNote?.contains("reads back as the factory block") == true,
                "it resets channels 1 to 6: \(f.model.resetNote ?? "nil")")
        f.model.preparePutBack(from: url(f, fName))
        t.equal(f.model.pendingPutBack?.plan.changes.count, 2, "192 kHz: the put back's two changes")
        f.model.confirmPutBack()
        f.settle()
        t.check(simBlock(f.unit) == fKept && f.model.putBackNote?.contains("match the backup, all 256 bytes") == true, "and the put back matches, byte for byte")
        var beyond = fKept
        beyond.replaceSubrange(PanelModel.volumeOffset(strip(inp, 7))..<PanelModel.volumeOffset(strip(inp, 7)) + 6, with: PanelModel.volumeDigits(minus12))
        beyond.replaceSubrange(PanelModel.volumeOffset(strip(inp, 1))..<PanelModel.volumeOffset(strip(inp, 1)) + 6, with: PanelModel.volumeDigits(minus6))
        f.model.preparePutBack(from: write(f, "beyond.ua101", beyond, unit: "Pretend unit"))
        t.check(f.model.pendingPutBack?.plan.changes.map { $0.what } == ["INPUT 2 volume"]
                    && f.model.pendingPutBack?.plan.cantPutBack.contains { $0.contains("a part of the mixer this app doesn't change") } == true,
                "192 kHz: INPUT 8 isn't in the mixer, so its volume is listed as something that can't be put back")
        f.model.confirmPutBack()
        f.settle()
        t.check(unitValue(f, fVolume, inp, 1) == minus6 && unitValue(f, fVolume, inp, 7) == unity
                    && f.model.putBackNote?.contains("The unit matches the backup in everything this app can put back") == true
                    && !MessageCenter.isWarning(f.model.putBackNote ?? ""), "INPUT 2 is put back, INPUT 8 is left alone, and that is no fault: \(f.model.putBackNote ?? "nil")")
        var soloPast = fKept
        soloPast[0x17] = 9
        t.check(f.model.preparePutBack(from: write(f, "solo9.ua101", soloPast, unit: "Pretend unit")) == Int32(PANEL_ERR_VALUE)
                    && f.model.refusal?.text.contains("a value the unit never takes (OUTPUT solo: 9)") == true, "192 kHz: a solo on channel 9, which the mixer doesn't have: refused")
        refused192: do {
            var slow = fKept
            slow[4] = (slow[4] & ~0x07) | 1
            let r = f.model.preparePutBack(from: write(f, "48k.ua101", slow, unit: "Pretend unit"))
            t.check(r == Int32(PANEL_ERR_VALUE) && f.model.refusal?.text.contains("it was made at 48 kHz and the unit is at 192 kHz") == true, "a 48 kHz backup onto the unit at 192 kHz: refused")
        }

        // The UA-101 behind the stand-in for the installed helper (link 14 to 16 pass the same).
        let g = real()
        var lines: [String] = []
        g.model.onRecord = { lines.append($0) }
        dress(g)
        let gKept = simBlock(g.fake.unit)
        g.model.saveBackup()
        g.settle()
        let gName = g.backupFiles.first ?? ""
        t.check(gName == "2026-11-16 18.30.ua101" && file(g, gName)?.block == gKept && file(g, gName)?.unit == "UA-101", "Save Backup with the UA-101: \(g.backupFiles)")
        t.check(lines.contains { $0.contains("Backup saved: “\(gName)”") } && lines.contains { $0.hasPrefix("  settings: 01 01 01 01") }, "the record has it too")
        // Reset Mixer on the box (1c): allowed now, made of the messages the helper passes.
        var mark = g.fake.changes.count
        t.equal(g.model.resetMixer(), ok, "Reset Mixer with the UA-101: accepted")
        g.settle(12_000_000)
        let resetSent = Array(g.fake.changes[mark...])
        t.check(Array(simBlock(g.fake.unit)[22...247]) == Array(factory[22...247]) && simBlock(g.fake.unit)[13] == 3,
                "the UA-101's mixer is the factory one, byte for byte; its patch bay wasn't touched")
        t.check(resetSent.first == pauseMsg && resetSent.last == resumeMsg && resetSent.filter { $0 == pauseMsg || $0 == resumeMsg }.count == 2,
                "inside one pause of the unit's saving: the pause first, the resume last, one of each")
        t.check(resetSent.count == 9 && g.fake.refused == 0 && g.fake.tooMany == 0 && g.helper.blocked == 0 && g.model.guardBlocks == 0,
                "seven changes and the pause and resume; the helper refused nothing: \(resetSent.count)")
        let gReset = g.model.resetNote ?? "nil"
        t.check(gReset.contains("7 changes sent (9 messages with the pause and resume") && gReset.contains("reads back as the factory block, every byte (d[22] to d[247])")
                    && gReset.contains("“2026-11-16 18.30 before Reset Mixer.ua101”") && !MessageCenter.isWarning(gReset), "it says so: \(gReset)")
        t.check(file(g, "2026-11-16 18.30 before Reset Mixer.ua101")?.block == gKept && lines.contains { $0.contains("Reset Mixer: 7 changes to send") }
                    && lines.contains { $0.contains("the settings before were saved as “2026-11-16 18.30 before Reset Mixer.ua101”") }
                    && lines.filter { $0.contains("handed to the helper") }.count >= 9 && lines.contains { $0.hasPrefix("  settings after:  01 01") },
                "the backup from before, and every message, are in the record")
        // Put Back on the box (1b).
        mark = g.fake.changes.count
        t.equal(g.model.preparePutBack(from: url(g, gName)), ok, "Put Back with the UA-101, step 1")
        t.check(g.model.pendingPutBack?.plan.changes.count == 7 && g.fake.changes.count == mark, "seven settings would change; nothing sent")
        g.model.confirmPutBack()
        g.settle(12_000_000)
        let putSent = Array(g.fake.changes[mark...])
        t.check(simBlock(g.fake.unit) == gKept, "the UA-101 holds the backup again, byte for byte, the linked pair with its two volumes too")
        t.check(putSent.count == 9 && putSent.first == pauseMsg && putSent.last == resumeMsg && putSent.filter { $0 == pauseMsg || $0 == resumeMsg }.count == 2
                    && g.fake.refused == 0 && g.fake.tooMany == 0 && g.helper.blocked == 0,
                "seven changes inside one pause of the unit's saving; the helper refused nothing: \(putSent.count)")
        t.check(g.model.putBackNote?.contains("7 changes sent") == true && g.model.putBackNote?.contains("The unit's settings match the backup, all 256 bytes") == true
                    && lines.contains { $0.contains("Open Backup, “\(gName)”: 7 changes to send") }, "it says so: \(g.model.putBackNote ?? "nil")")
        // Every setting at once (about 90 messages): never more than the helper takes in a second.
        var all = gKept
        for s in Allowed.volumeStrips {
            all.replaceSubrange(PanelModel.volumeOffset(s)..<PanelModel.volumeOffset(s) + 6, with: PanelModel.volumeDigits(0x0FEDCB - UInt32(s.channel)))
            if s.group != mon { all.replaceSubrange(PanelModel.panOffset(s)..<PanelModel.panOffset(s) + 4, with: PanelModel.panDigits(0x2345 + UInt32(s.channel))) }
        }
        all[0x16] = 7; all[0x17] = 9
        for i in 0x18...0x1C { all[i] ^= PutBackPlan.settableMask(is192: false)[i] }
        for i in 0x1D...0x23 { all[i] ^= PutBackPlan.settableMask(is192: false)[i] }
        for o in 0..<5 { all[12 + o] = UInt8(6 - o) }
        mark = g.fake.changes.count
        g.model.preparePutBack(from: write(g, "all.ua101", all))
        t.equal(g.model.pendingPutBack?.plan.changes.count, 82, "a backup that differs in every setting: 82 changes")
        g.model.confirmPutBack()
        var tenths = 0
        while g.model.jobRunning && tenths < 300 {
            g.settle(100_000)
            tenths += 1
        }
        g.settle()
        t.check(simBlock(g.fake.unit) == all && g.fake.changes.count - mark == 84 && g.fake.tooMany == 0 && g.fake.refused == 0 && g.helper.blocked == 0
                    && g.model.putBackNote?.contains("82 changes sent (84 messages") == true && g.model.putBackNote?.contains("match the backup, all 256 bytes") == true,
                "all 82 put back, byte for byte, none refused for coming too fast: \(g.model.putBackNote ?? "nil")")
        t.check(tenths > 45 && tenths < 80, "at most 17 messages a second: all of it takes \(Double(tenths) / 10) s, the read-back included")
        t.note("a put back of all 82 settings to the UA-101 (the most there can be): \(Double(tenths) / 10) s from the button to the read-back's verdict")
        // A backup of the pretend unit never goes onto the UA-101; the other way is fine.
        mark = g.fake.changes.count
        let r = g.model.preparePutBack(from: write(g, "pretend.ua101", gKept, unit: "Pretend unit"))
        t.check(r == Int32(PANEL_ERR_VALUE) && g.model.pendingPutBack == nil && g.model.refusal?.text.contains("made from the pretend unit. It can't be put back on the UA-101") == true,
                "a pretend unit's backup onto the UA-101: refused")
        t.equal(c.model.preparePutBack(from: write(c, "real.ua101", simBlock(c.unit))), ok, "a UA-101 backup onto the pretend unit: taken")
        c.model.cancelPutBack()
        // A volume above 0 dB in a backup is put back as it is (since 2026-11-27), up to the top.
        var gLoud = all
        gLoud.replaceSubrange(PanelModel.volumeOffset(strip(out, 0))..<PanelModel.volumeOffset(strip(out, 0)) + 6, with: PanelModel.volumeDigits(0x7FFFFF))
        g.model.preparePutBack(from: write(g, "loud.ua101", gLoud))
        g.model.confirmPutBack()
        g.settle(12_000_000)
        t.check(unitValue(g, fVolume, out, 0, real: true) == 0x7FFFFF && g.fake.refused == 0 && g.helper.blocked == 0
                    && g.fake.changes[mark...].allSatisfy { panel_volume_message_channel($0, $0.count) >= 0 || $0.count != 18 }
                    && g.model.putBackNote?.contains("match the backup, all 256 bytes") == true && !MessageCenter.isWarning(g.model.putBackNote ?? ""),
                "+12 dB in the backup: put back as it is, nothing refused: \(g.model.putBackNote ?? "nil")")
        // The unit is unplugged half way: the app stops and says how far it got.
        g.model.preparePutBack(from: url(g, gName))
        let total = g.model.pendingPutBack?.plan.changes.count ?? 0
        mark = g.fake.changes.count
        g.model.confirmPutBack()
        g.settle(1_000_000)
        let left = g.fake.changes.count - mark - 1 // without the pause
        t.check(g.model.jobRunning && left > 5 && left < total, "after a second \(left) of \(total) changes have left")
        g.fake.unitGoes()
        g.settle()
        let stopped = g.model.putBackNote ?? "nil"
        t.check(!g.model.jobRunning && stopped.contains("THE PUT BACK STOPPED part way. The unit went away after \(left) of \(total) changes had left for it; the other \(total - left) were not sent")
                    && stopped.contains("before Open Backup") && g.messages.now.contains { $0.slot == .putBack && $0.warning },
                "unplugged half way: a warning with how far it got: \(stopped)")
        t.check(g.fake.changes.count == mark + 1 + left, "and nothing more left for it")
        g.fake.unitComes()
        g.settle(6_000_000)
        t.check(g.model.snap.state == .ready && g.fake.changes.last == resumeMsg && panel_sim_saving_paused(g.fake.unit.sim) == 0,
                "the unit is back: its saving is resumed, as after a drag")
        g.model.preparePutBack(from: url(g, gName))
        t.equal(g.model.pendingPutBack?.plan.changes.count, total - left, "the same file again: only what is missing")
        g.model.confirmPutBack()
        g.settle(12_000_000)
        t.check(simBlock(g.fake.unit) == gKept && g.model.putBackNote?.contains("match the backup, all 256 bytes") == true, "and that finishes it, byte for byte")
        // The helper refuses half way: the app says how far it got.
        g.model.preparePutBack(from: url(g, "all.ua101"))
        g.model.confirmPutBack()
        g.settle(1_000_000)
        g.fake.refuseChanges = "a refusal made by the test; the message was not sent"
        g.settle(12_000_000)
        let refusedNote = g.model.putBackNote ?? "nil"
        t.check(!g.model.jobRunning && !g.model.snap.writesAllowed && refusedNote.contains("THE PUT BACK STOPPED part way: the driver's helper refused a message after about")
                    && refusedNote.contains("of 82 changes had left") && g.model.refusal?.kind == .helper, "a refusal half way: \(refusedNote)")
        // The helper's list of what it passes on shrinks just before the fresh read comes.
        g.model.allowChanges(true)
        g.settle()
        mark = g.fake.changes.count
        let refusedSoFar = g.fake.refused
        g.model.preparePutBack(from: url(g, gName))
        g.fake.passes = FakeHelper.version13
        g.fake.passesOn = FakeHelper.version13On
        g.fake.tellPasses()
        g.model.confirmPutBack()
        g.settle(12_000_000)
        t.check(g.model.putBackNote?.contains("The driver's helper doesn't pass on every change it needs. Nothing was sent") == true
                    && g.fake.changes.count == mark && g.helper.blocked == 0 && g.fake.refused == refusedSoFar && !g.model.jobRunning,
                "the helper passes on less than a put back needs by the time the read is in: nothing is sent: \(g.model.putBackNote ?? "nil")")
        g.fake.passes = FakeHelper.version14
        g.fake.passesOn = FakeHelper.version14On
        g.fake.tellPasses()
        g.settle()
        // Choosing another unit drops what was under way.
        g.model.preparePutBack(from: url(g, gName))
        g.model.confirmPutBack()
        g.model.onChooseDevice?(false)
        g.settle()
        t.check(!g.model.jobRunning && g.model.pendingPutBack == nil && g.model.putBackNote == nil, "the pretend unit chosen meanwhile: nothing of it is kept")

        // A helper that passes on less than the whole mixer: refused before anything is sent.
        let old = real(passes: FakeHelper.version14 & ~(PANEL_PASS_PAN_CHANNEL_0 << 5))
        t.check(old.model.resetMixer() == Int32(PANEL_ERR_READ_ONLY) && old.model.refusal?.kind == .notPassed, "a helper without one pan: Reset Mixer refused")
        old.model.preparePutBack(from: write(old, "kept.ua101", gKept))
        t.check(old.model.putBackBlocker()?.contains("doesn't pass on every change") == true && old.model.confirmPutBack() == Int32(PANEL_ERR_READ_ONLY)
                    && old.model.refusal?.kind == .notPassed, "and Put Back too")
        let noPatch = real(passes: FakeHelper.version14 & ~(UInt64(PANEL_PASS_PATCH_OUTPUT_0) << 4))
        noPatch.model.preparePutBack(from: write(noPatch, "kept.ua101", gKept))
        t.check(noPatch.model.confirmPutBack() == Int32(PANEL_ERR_READ_ONLY) && noPatch.model.refusal?.kind == .notPassed, "a helper without one patch bay row: Put Back refused")
        t.equal(noPatch.model.resetMixer(), ok, "Reset Mixer doesn't need the patch bay")
        noPatch.settle(12_000_000)
        t.check(old.fake.changesPassedOn == 0 && old.fake.refused == 0 && noPatch.fake.refused == 0 && noPatch.helper.blocked == 0, "the helper had nothing to refuse")
        // Without the go-ahead: Save Backup works, the other two are refused.
        let look = Bench()
        look.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        look.chooseReal()
        look.model.saveBackup()
        look.settle()
        t.check(look.backupFiles == ["2026-11-16 18.30.ua101"] && look.fake.changesPassedOn == 0 && look.fake.refused == 0, "the UA-101 without your go-ahead: Save Backup works, only a read was sent")
        look.model.preparePutBack(from: url(look, "2026-11-16 18.30.ua101"))
        t.check(look.model.confirmPutBack() == Int32(PANEL_ERR_READ_ONLY) && look.model.resetMixer() == Int32(PANEL_ERR_READ_ONLY) && look.fake.changesPassedOn == 0,
                "Put Back and Reset Mixer: refused, read-only")
    }

    // ---- 16d
    if t.begin("Recall, Save and Reset to Factory on the box") {
        let ok = Int32(PANEL_OK), busy = Int32(PANEL_ERR_BUSY), readOnly = Int32(PANEL_ERR_READ_ONLY)
        func simBlock(_ u: PretendUnit) -> [UInt8] {
            var block = [UInt8](repeating: 0, count: 256)
            panel_sim_block(u.sim, &block)
            return block
        }
        func simMemory(_ u: PretendUnit, _ m: Int) -> [UInt8] {
            var block = [UInt8](repeating: 0, count: 256)
            panel_sim_memory(u.sim, UInt32(m), &block)
            return block
        }
        func url(_ b: Bench, _ name: String) -> URL { b.backups.appendingPathComponent(name) }
        func file(_ b: Bench, _ name: String) -> SettingsBackup? {
            if case .success(let backup) = SettingsBackup.read(url(b, name)) { return backup }
            return nil
        }
        func write(_ b: Bench, _ name: String, _ block: [UInt8], unit: String = "UA-101") -> URL {
            try? FileManager.default.createDirectory(at: b.backups, withIntermediateDirectories: true)
            let u = url(b, name)
            try? SettingsBackup(block: block, unit: unit, saved: "2026-11-21 10:00:07", why: "Memory 3 as read through Recall (the patch bay and the monitor mixer)").text().write(to: u, atomically: true, encoding: .utf8)
            return u
        }
        /// The newest of the bench's backup files whose name holds these words; "" if none.
        func named(_ b: Bench, _ words: String) -> String { b.backupFiles.filter { $0.contains(words) }.last ?? "" }
        var factory = [UInt8](repeating: 0, count: 256)
        panel_factory_block(&factory)
        /// The UA-101 behind the stand-in helper (link version 17), SOFT CTRL in, changes on.
        func real() -> Bench {
            let g = Bench()
            g.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
            g.chooseReal()
            g.model.allowChanges(true)
            g.settle()
            return g
        }
        /// The same (up to link version 17 this also made the user's second go-ahead and
        /// switched Recall, Save and Reset on by themselves).
        func open() -> Bench { real() }
        /// Both steps of one command, then time for it to end. Step 2's answer.
        @discardableResult func run(_ g: Bench, _ c: MemoryCommand, wait: UInt64 = 4_000_000) -> Int32 {
            g.model.prepareMemory(c)
            let r = g.model.confirmMemory()
            g.settle(wait)
            return r
        }
        /// Past the helper's five seconds between two commands.
        func rest(_ g: Bench) { g.settle(7_000_000) }
        func note(_ g: Bench) -> String { g.model.memoryNote ?? "nil" }
        func warned(_ g: Bench) -> Bool { g.messages.now.contains { $0.slot == .memory && $0.warning } }
        func bit(_ b: [UInt8]) -> UInt64 { b.withUnsafeBufferPointer { panel_memory_message_bit($0.baseAddress, $0.count) } }
        func hand(_ g: Bench, _ bytes: [UInt8]) -> Bool { bytes.withUnsafeBufferPointer { g.helper.send($0) } }
        func summed(_ b: [UInt8]) -> [UInt8] {
            var out = b
            out[b.count - 2] = UInt8((0x80 - (b[6..<b.count - 2].reduce(0) { $0 + Int($1) } & 0x7F)) & 0x7F)
            return out
        }
        let pauseMsg = PanelModel.savingMessage(resume: false), resumeMsg = PanelModel.savingMessage(resume: true)
        let store5 = MemoryCommand(kind: .store, memory: 4, patchBay: false, mixer: true)
        let load5 = MemoryCommand(kind: .load, memory: 4, patchBay: false, mixer: true)

        // The 37 messages, and the app's own rule for them.
        let parts = [(true, false), (false, true), (true, true)]
        let all: [MemoryCommand] = (0..<5).flatMap { m in parts.map { MemoryCommand(kind: .load, memory: m, patchBay: $0.0, mixer: $0.1) } }
            + (0..<5).flatMap { m in parts.map { MemoryCommand(kind: .store, memory: m, patchBay: $0.0, mixer: $0.1) } }
            + (0..<5).map { MemoryCommand(kind: .resetMemory, memory: $0) }
            + [MemoryCommand(kind: .resetCurrent), MemoryCommand(kind: .resetAll)]
        t.check(all.count == 37 && Set(all.map { $0.bytes }).count == 37 && all.allSatisfy { $0.isValid }, "37 commands, 37 different messages")
        t.check(all.allSatisfy { bit($0.bytes) == $0.bit && $0.bit != 0 }, "the app's own rule knows each, by the bit the helper names it with")
        t.equal(all.reduce(UInt64(0)) { $0 | $1.bit }, FakeHelper.version17Memory, "together they are the helper's list of 17 commands")
        t.equal(Format.hex(MemoryCommand(kind: .load, memory: 2).bytes), "F0 41 10 00 69 12 00 00 05 08 00 00 01 01 71 F7", "Recall Memory 3, both parts")
        t.equal(Format.hex(store5.bytes), "F0 41 10 00 69 12 00 00 07 04 00 00 00 02 73 F7", "Save to Memory 5, the mixer")
        t.equal(Format.hex(MemoryCommand(kind: .resetCurrent).bytes), "F0 41 10 00 69 12 00 00 00 03 00 7D F7", "Reset of the settings in use")
        t.equal(Format.hex(MemoryCommand(kind: .resetMemory, memory: 4).bytes), "F0 41 10 00 69 12 00 00 07 03 00 76 F7", "Reset of Memory 5")
        t.equal(Format.hex(MemoryCommand(kind: .resetAll).bytes), "F0 41 10 00 69 12 00 00 00 02 00 7E F7", "Reset of everything")
        do {
            let load3 = MemoryCommand(kind: .load, memory: 2).bytes
            var switches = load3, sixth = load3, three = load3, sum = load3, short = load3
            switches[10] = 1
            sixth[8] = 8
            three[12] = 3
            sum[14] ^= 1
            short.removeLast()
            let preset: [UInt8] = [0xF0, 0x41, 0x10, 0x00, 0x69, 0x12, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x04, 0x00, 0x74, 0xF7]
            var resetSixth = MemoryCommand(kind: .resetMemory, memory: 4).bytes, resetOne = MemoryCommand(kind: .resetCurrent).bytes
            resetSixth[8] = 8
            resetOne[10] = 1
            // The three kinds of Reset with everything right but one byte the address doesn't
            // show: the checksum, the device number at the start, the last byte.
            for reset in [MemoryCommand(kind: .resetMemory, memory: 2), MemoryCommand(kind: .resetCurrent), MemoryCommand(kind: .resetAll)] {
                var sum = reset.bytes, device = reset.bytes, end = reset.bytes
                sum[11] ^= 1
                device[2] = 0x11
                end[12] = 0xF0
                t.check(bit(reset.bytes) == reset.bit && bit(sum) == 0 && bit(device) == 0 && bit(end) == 0,
                        "\(reset.title): not with a wrong checksum, another device number or a wrong last byte")
            }
            for whole in [MemoryCommand(kind: .load, memory: 1, patchBay: true, mixer: false), MemoryCommand(kind: .store, memory: 3)] {
                var sum = whole.bytes, device = whole.bytes
                sum[14] ^= 1
                device[2] = 0x11
                t.check(bit(whole.bytes) == whole.bit && bit(sum) == 0 && bit(device) == 0, "\(whole.title): the same")
            }
            t.check(bit(summed(switches)) == 0 && bit(summed(sixth)) == 0 && bit(summed(three)) == 0 && bit(sum) == 0 && bit(short) == 0
                        && bit(preset) == 0 && bit(summed(resetSixth)) == 0 && bit(summed(resetOne)) == 0 && bit(pauseMsg) == 0 && bit(resumeMsg) == 0 && bit([]) == 0,
                    "not the switches' part, a sixth memory, section byte 3, a wrong checksum, a byte short, Roland's preset, or the saving messages")
        }
        let v17 = Allowed(passes: FakeHelper.version17, passesOn: FakeHelper.version14On, passesMemory: FakeHelper.version17Memory)
        t.check(all.allSatisfy { v17.memory($0.bit) } && v17.someMemory && v17.mixer && !v17.memory(0), "the installed helper's list (link 17): every one of them")
        t.check(!Allowed(passes: FakeHelper.version14, passesOn: FakeHelper.version14On, passesMemory: FakeHelper.version17Memory).someMemory,
                "a helper that doesn't raise the flag passes none, whatever its third number says")
        let less = Allowed(passes: FakeHelper.version17, passesOn: FakeHelper.version14On, passesMemory: FakeHelper.version17Memory & ~(PANEL_PASS_MEM_STORE_0 << 4))
        t.check(!less.memory(store5.bit) && less.memory(load5.bit) && Allowed.all.memory(store5.bit) && !Allowed.nothing.someMemory, "a helper that names fewer: only those")
        t.check(PanelModel.changeWords(from: factory, to: factory).count == 0
                    && PanelModel.loadMask(patchBay: true, mixer: false).enumerated().allSatisfy { $0.element == (12...21).contains($0.offset) }
                    && PanelModel.loadMask(patchBay: false, mixer: true).enumerated().allSatisfy { $0.element == (22...247).contains($0.offset) },
                "what a Load may change: the patch bay d[12] to d[21], the mixer d[22] to d[247], never the switches")
        t.check(AppCore.memoryNamed(in: "2026-11-21 10.00 Memory 3 as read.ua101") == 2 && AppCore.memoryNamed(in: "2026-11-21 10.00.ua101") == nil
                    && AppCore.memoryNamed(in: "x Memory 9.ua101") == nil, "a file's name says which memory it was read from")

        // Recall, Save and Reset are on and off with changes, whichever unit: there is one switch.
        let off = Bench()
        t.check(!off.model.snap.memoryAllowed && !off.model.snap.writesAllowed, "the pretend unit, read-only: Recall, Save and Reset are off")
        off.openUp()
        t.check(off.model.snap.memoryAllowed && off.model.memoryBlocker(store5) == nil, "with changes on they are on")
        off.model.allowChanges(false)
        t.check(!off.model.snap.memoryAllowed, "and they go off with them")

        // The UA-101: on with changes, with no second switch and no file anywhere.
        var lines: [String] = []
        let g = real()
        g.model.onRecord = { lines.append($0) }
        t.check(g.model.snap.writesAllowed && g.model.snap.memoryAllowed && g.model.memoryBlocker(store5) == nil && g.model.refusal == nil,
                "the UA-101 with changes on: Recall, Save and Reset are on with them")
        // The transport's own gate, while the helper's list (read when the app attached) names
        // every command: with changes off it keeps a Store handed to it from the helper.
        g.model.allowChanges(false)
        let stopped = g.helper.blocked
        t.check(g.helper.allowed.memory(store5.bit) && !hand(g, store5.bytes) && g.helper.blocked == stopped + 1 && g.fake.memoryCommands.isEmpty && g.fake.refused == 0,
                "the transport itself stops a Store while changes are off, though the helper names it")
        t.equal(g.model.prepareMemory(store5), readOnly, "and the app refuses a Store: read-only")
        t.check(g.model.memoryBlocker()?.contains("Allow Changes") == true, "the dialogs say why: \(g.model.memoryBlocker() ?? "nil")")
        g.model.allowChanges(true)
        // A helper that passes none of them on (as link version 14 was).
        g.fake.passes = FakeHelper.version14
        g.fake.tellPasses()
        g.settle()
        t.check(g.model.prepareMemory(store5) == readOnly && g.model.refusal?.kind == .notPassed && g.model.refusal?.text.contains("passes on no Recall, Save or Reset") == true
                    && g.model.memoryBlocker()?.contains("passes on no Recall, Save or Reset") == true,
                "a helper that passes none of them on: not done, and it says so")
        g.fake.passes = FakeHelper.version17
        g.fake.tellPasses()
        g.settle()
        t.check(g.model.snap.memoryAllowed && g.model.memoryBlocker(store5) == nil && !lines.contains { $0.contains("go-ahead") },
                "with the whole list back: nothing in the way, and the record names no go-ahead")

        // A Store, as at the box: a backup first, one message, read back twice.
        g.model.setVolumeExact(strip(inp, 0), 0x100001)
        g.settle()
        g.model.setPatch(output: 1, source: 3)
        g.settle()
        g.model.setPanExact(strip(inp, 2), 0x1234)
        g.settle()
        let kept = simBlock(g.fake.unit)
        var passed = g.fake.passedOn
        t.equal(g.model.prepareMemory(store5), ok, "Store, step 1")
        t.equal(g.model.pendingMemory.map { Format.hex($0.bytes) } ?? "", "F0 41 10 00 69 12 00 00 07 04 00 00 00 02 73 F7", "it shows the one message")
        g.settle(5_000_000)
        t.check(g.fake.memoryCommands.isEmpty && g.fake.passedOn == passed, "step 1 sent nothing")
        t.equal(g.model.confirmMemory(), ok, "Store, step 2: accepted")
        t.check(g.model.jobRunning && g.model.pendingMemory == nil && g.model.confirmMemory() == Int32(PANEL_ERR_TICKET), "under way; a second click does nothing")
        g.model.setVolume(strip(inp, 1), minus6)
        t.check(g.model.refusal?.kind == .busy && g.model.refusal?.text.contains("Save to Memory 5 is under way") == true, "another change meanwhile is refused")
        g.model.dismissRefusal()
        g.settle(4_000_000)
        let storeNote = note(g)
        t.check(g.fake.memoryCommands == [store5.bytes] && panel_sim_memory_commands(g.fake.unit.sim) == 1, "one command reached the unit: the Store")
        t.check(g.fake.memoryRecords.last == kept, "the helper held the settings as read just before it, for its log")
        t.equal(g.fake.passedOn, passed + 4, "four messages in all: a fresh read, the Store, and two reads after it")
        let memory5 = simMemory(g.fake.unit, 4)
        t.check(Array(memory5[22...247]) == Array(kept[22...247]) && Array(memory5[7...21]) == Array(factory[7...21]) && simBlock(g.fake.unit) == kept,
                "Memory 5 holds the mixer, not the patch bay; the settings in use didn't change")
        t.check(storeNote.contains("Save to Memory 5 (the monitor mixer): sent F0 41 10 00 69 12 00 00 07 04 00 00 00 02 73 F7")
                    && storeNote.contains("read back 2 times (1.5 s and 2.5 s after it), and the last two reads agree")
                    && storeNote.contains("The settings in use read back the same as before, all 256 bytes") && !warned(g)
                    && g.messages.now.contains { $0.slot == .memory },
                "and it says so, as a plain message: \(storeNote)")
        let beforeStore = named(g, "before Save to Memory 5")
        t.check(file(g, beforeStore)?.block == kept && file(g, beforeStore)?.why == "before Save to Memory 5" && file(g, beforeStore)?.unit == "UA-101"
                    && storeNote.contains(beforeStore), "the settings from before were saved by themselves: \(g.backupFiles)")
        t.check(lines.contains { $0.contains("Save to Memory 5: one message to send, F0 41 10 00 69 12 00 00 07 04 00 00 00 02 73 F7") }
                    && lines.contains { $0.contains("handed to the helper: F0 41 10 00 69 12 00 00 07 04") }
                    && lines.contains { $0.contains("read 1, 1.5 s after the command: 01 01") }
                    && lines.contains { $0.contains("read 2, 2.5 s after the command: the same as read 1") }
                    && lines.contains { $0.hasPrefix("  settings after:") },
                "the record holds the message, both reads and the settings after")
        // One at a time: the helper passes on one every five seconds.
        g.model.prepareMemory(load5)
        t.check(g.model.confirmMemory() == busy && g.model.refusal?.text.contains("Try again in") == true && g.model.pendingMemory == nil
                    && g.fake.memoryCommands.count == 1 && g.fake.refused == 0 && g.model.memoryBlocker()?.contains("Wait") == true,
                "a Load right after it: held back by the app itself, nothing sent: \(g.model.refusal?.text ?? "nil")")
        rest(g)
        // Change something, then Load brings the stored mixer back.
        g.model.setVolumeExact(strip(inp, 0), unity)
        g.settle()
        let changed = simBlock(g.fake.unit)
        t.equal(run(g, load5), ok, "Recall Memory 5, the mixer")
        let loadNote = note(g)
        t.check(simBlock(g.fake.unit) == kept && g.model.snap.channels[inp][0].volume == 0x100001, "the Load brought INPUT 1's volume back, and the app shows it")
        t.check(loadNote.contains("Recall Memory 5 (the monitor mixer): sent F0 41 10 00 69 12 00 00 07 08 00 00 00 01 70 F7")
                    && loadNote.contains("1 setting changed: INPUT 1 volume 0.0 dB → \(Format.dB(0x100001)) dB.") && !warned(g),
                "it says what changed: \(loadNote)")
        let asRead = named(g, "Memory 5 mixer as read")
        t.check(file(g, asRead)?.block == kept && file(g, asRead)?.why == "Memory 5 as read through Recall (the monitor mixer; the rest of the file is what was in use)"
                    && loadNote.contains(asRead) && file(g, named(g, "before Recall Memory 5"))?.block == changed,
                "the memory as read is saved as a file, and the settings from before too: \(g.backupFiles)")
        // A memory that holds Hard Thru and a volume above 0 dB.
        rest(g)
        var loud = factory
        for o in 0..<5 { loud[12 + o] = UInt8(2 + o) }
        let input3 = PanelModel.volumeOffset(strip(inp, 2))
        loud.replaceSubrange(input3..<input3 + 6, with: PanelModel.volumeDigits(0x400000))
        panel_sim_set_memory(g.fake.unit.sim, 2, loud)
        run(g, MemoryCommand(kind: .load, memory: 2))
        let loudNote = note(g), loudNow = simBlock(g.fake.unit)
        t.check(Array(loudNow[12...16]) == [2, 3, 4, 5, 6] && Array(loudNow[7...247]) == Array(loud[7...247]).enumerated().map { (7 + $0.offset) < 12 ? kept[7 + $0.offset] : $0.element }
                    && Array(loudNow[0...6]) == Array(kept[0...6]) && g.model.snap.channels[inp][2].volume == 0x400000,
                "Recall Memory 3, both parts: the patch bay and the mixer are the memory's, the switches stay, and the app shows +6 dB")
        t.check(loudNote.contains("Volumes above 0 dB are in use now: INPUT 3 \(Format.dB(0x400000)) dB")
                    && loudNote.contains("The patch bay now sends inputs straight to outputs (OUTPUT 1-2, OUTPUT 3-4, OUTPUT 5-6, OUTPUT 7-8, OUTPUT 9-10)") && !warned(g),
                "and it says what to know before the monitors go up: \(loudNote)")
        let memory3 = named(g, "Memory 3 as read")
        t.check(file(g, memory3)?.block == loudNow && file(g, memory3)?.why == "Memory 3 as read through Recall (the patch bay and the monitor mixer)", "saved as “Memory 3 as read”")
        rest(g)
        t.check(g.model.preparePutBack(from: url(g, named(g, "before Recall Memory 3"))) == ok && g.model.confirmPutBack() == ok, "Put Back from the file saved before the Load")
        g.settle(8_000_000)
        t.check(simBlock(g.fake.unit) == kept && g.model.putBackNote?.contains("match the backup, all 256 bytes") == true, "everything is back: \(g.model.putBackNote ?? "nil")")
        // The settings are read until two reads agree.
        rest(g)
        g.model.prepareMemory(load5)
        g.model.confirmMemory()
        g.settle(1_700_000)
        panel_sim_poke(g.fake.unit.sim, 0x85, 7)
        g.settle(4_000_000)
        t.check(note(g).contains("read back 3 times (1.5 s, 2.5 s and 3.5 s after it), and the last two reads agree") && !warned(g) && !g.model.jobRunning,
                "the unit still changing after the first read: read a third time: \(note(g))")
        rest(g)
        g.model.prepareMemory(load5)
        g.model.confirmMemory()
        g.settle(1_700_000)
        for v in 2...6 {
            panel_sim_poke(g.fake.unit.sim, 0x85, UInt8(v))
            g.settle(1_000_000)
        }
        g.settle(2_000_000)
        t.check(note(g).contains("THE UNIT'S SETTINGS DID NOT SETTLE: read back 6 times") && warned(g) && !g.model.jobRunning,
                "a unit that never settles: six reads, then a warning: \(note(g))")
        // Reset of the settings in use: the memories stay.
        rest(g)
        g.model.setVolumeExact(strip(inp, 0), 0x100001)
        g.settle()
        let beforeReset = simBlock(g.fake.unit)
        run(g, MemoryCommand(kind: .resetCurrent))
        let current = simBlock(g.fake.unit)
        t.check(Array(current[7...247]) == Array(factory[7...247]) && Array(current[0...6]) == Array(beforeReset[0...6]) && simMemory(g.fake.unit, 2) == loud,
                "Reset of the settings in use: the patch bay and the mixer are the factory ones, the switches and the memories stay")
        t.check(note(g).contains("Reset the settings in use: sent F0 41 10 00 69 12 00 00 00 03 00 7D F7")
                    && note(g).contains("now read back as the factory block this app holds, every byte (d[7] to d[247])") && !warned(g)
                    && file(g, named(g, "after Reset the settings in use"))?.block == current && file(g, named(g, "before Reset the settings in use"))?.block == beforeReset,
                "it says so, and both files are there: \(note(g))")
        // Reset of one memory: the settings in use stay.
        rest(g)
        run(g, MemoryCommand(kind: .resetMemory, memory: 2))
        t.check(simMemory(g.fake.unit, 2) == factory && simBlock(g.fake.unit) == current
                    && note(g).contains("Reset Memory 3: sent F0 41 10 00 69 12 00 00 05 03 00 78 F7") && note(g).contains("the same as before, all 256 bytes") && !warned(g),
                "Reset Memory 3: the memory is the factory one, the settings in use the same: \(note(g))")
        // A reset whose values aren't the app's factory ones.
        rest(g)
        g.model.prepareMemory(MemoryCommand(kind: .resetCurrent))
        g.model.confirmMemory()
        g.settle(500_000)
        panel_sim_poke(g.fake.unit.sim, 0x85, 3)
        g.settle(4_000_000)
        t.check(note(g).contains("THE UNIT'S RESET VALUES DIFFER from the factory block this app holds in 1 byte: d[133] 03, the app's factory 00 (INPUT 1 volume)") && warned(g),
                "a reset that gives other values than the app's factory block: a warning that names the byte: \(note(g))")
        // Reset of everything.
        rest(g)
        panel_sim_set_memory(g.fake.unit.sim, 0, loud)
        run(g, MemoryCommand(kind: .resetAll))
        t.check(simMemory(g.fake.unit, 0) == factory && simMemory(g.fake.unit, 4) == factory && Array(simBlock(g.fake.unit)[7...247]) == Array(factory[7...247])
                    && note(g).contains("The five memories were reset too") && named(g, "after Reset everything") != "" && !warned(g),
                "Reset of everything: the settings in use and all five memories: \(note(g))")
        // A Store that changes the settings in use, and a Load that changes more than was asked.
        rest(g)
        g.model.prepareMemory(MemoryCommand(kind: .store, memory: 0))
        g.model.confirmMemory()
        g.settle(500_000)
        panel_sim_poke(g.fake.unit.sim, 13, 5)
        g.settle(4_000_000)
        t.check(note(g).contains("THE SETTINGS IN USE CHANGED in 1 byte, which this command should not do: d[13] 08→05 (patch bay, OUTPUT 3-4)") && warned(g)
                    && named(g, "after Save to Memory 1") != "",
                "a Store after which the settings in use differ: a warning, and the result is saved: \(note(g))")
        rest(g)
        g.model.prepareMemory(MemoryCommand(kind: .load, memory: 0, patchBay: false, mixer: true))
        g.model.confirmMemory()
        g.settle(500_000)
        panel_sim_poke(g.fake.unit.sim, 8, 0x7F)
        g.settle(4_000_000)
        t.check(note(g).contains("THE UNIT ALSO CHANGED BYTES OUTSIDE the part asked for, in 1 byte: d[8] 03→7F (a patch bay row this app doesn't change)") && warned(g),
                "a Load of the mixer after which another part differs: a warning: \(note(g))")
        t.equal(g.fake.refused, 0, "in all of this the helper refused nothing: the app kept its rules")
        t.equal(g.helper.blocked, stopped + 1, "and the transport stopped nothing but the one Store handed to it by hand")

        // No Recall, Save or Reset while sound plays.
        let s = open()
        s.fake.setStreaming(true)
        s.settle()
        s.model.prepareMemory(store5)
        t.check(s.model.memoryBlocker()?.contains("Sound is playing") == true && s.model.confirmMemory() == busy
                    && s.model.refusal?.text.contains("Stop the sound first") == true && !s.model.jobRunning && s.fake.memoryCommands.isEmpty,
                "sound through the UA-101: refused by the app, nothing sent: \(s.model.refusal?.text ?? "nil")")
        s.fake.setStreaming(false)
        s.settle()
        s.model.prepareMemory(store5)
        s.fake.setStreaming(true) // it starts while the fresh read is on its way
        t.equal(s.model.confirmMemory(), ok, "the sound starts in the moment of the click: accepted")
        s.settle(4_000_000)
        t.check(note(s).contains("Sound started to play or record") && note(s).contains("Nothing was sent") && s.fake.memoryCommands.isEmpty && s.fake.refused == 0,
                "then stopped after the read, before anything was sent: \(note(s))")
        s.fake.setStreaming(false)
        s.settle()
        s.model.prepareMemory(store5)
        s.model.confirmMemory()
        s.settle(50_000)          // the fresh read is in; the command leaves a moment later
        s.fake.setStreaming(true) // and the sound starts in between
        s.settle(4_000_000)
        t.check(note(s).contains("NOT DONE. THE DRIVER'S HELPER REFUSED the command and sent nothing to the unit (sound is being played or recorded")
                    && !s.model.snap.writesAllowed && !s.model.snap.memoryAllowed && s.model.refusal?.kind == .helper && s.fake.memoryCommands.isEmpty
                    && simMemory(s.fake.unit, 4) == factory && warned(s),
                "sound that starts after the app's last look: the helper refuses, and the app says so: \(note(s))")

        // What can change between the click and the command.
        let m = open()
        m.model.prepareMemory(store5)
        m.model.confirmMemory()
        m.model.allowChanges(false)
        m.settle(4_000_000)
        t.check(note(m).contains("Changes were switched off meanwhile. Nothing was sent") && m.fake.memoryCommands.isEmpty, "the same for changes: \(note(m))")
        // The confirmation runs out while the settings are read.
        let x = open()
        x.model.prepareMemory(store5)
        x.settle(29_950_000)
        t.equal(x.model.confirmMemory(), ok, "confirmed in the last moment of the 30 seconds")
        x.settle(4_000_000)
        t.check(note(x).contains("NOT SENT: the confirmation ran out (30 seconds)") && x.fake.memoryCommands.isEmpty && warned(x),
                "it ran out before the command could leave: not sent: \(note(x))")
        // No backup, no command.
        let folder = x.model.backupFolder
        x.model.backupFolder = nil
        x.model.prepareMemory(store5)
        t.check(x.model.confirmMemory() == Int32(PANEL_ERR_VALUE) && x.model.refusal?.text.contains("no folder") == true && x.model.pendingMemory == nil && !x.model.jobRunning,
                "no folder for backups: refused")
        try? FileManager.default.createDirectory(at: x.backups, withIntermediateDirectories: true)
        let wall = x.backups.appendingPathComponent("not-a-folder")
        try? "x".write(to: wall, atomically: true, encoding: .utf8)
        x.model.backupFolder = wall.appendingPathComponent("inside")
        x.model.prepareMemory(store5)
        x.model.confirmMemory()
        x.settle(4_000_000)
        t.check(note(x).contains("NOT DONE. The backup of the settings as they are now couldn't be saved") && note(x).contains("Nothing was sent") && x.fake.memoryCommands.isEmpty,
                "a backup that can't be written: nothing is sent: \(note(x))")
        x.model.backupFolder = folder
        // The unit goes away.
        let u = open()
        u.model.prepareMemory(store5)
        u.model.confirmMemory()
        u.fake.unitGoes()
        u.settle(6_000_000)
        t.check(note(u).contains("The unit stopped answering before its settings were read. Nothing was sent") && u.fake.memoryCommands.isEmpty, "before the read: \(note(u))")
        let u2 = open()
        u2.model.prepareMemory(store5)
        u2.model.confirmMemory()
        u2.settle(500_000)
        u2.fake.unitGoes()
        u2.settle(6_000_000)
        t.check(u2.fake.memoryCommands.count == 1 && note(u2).contains("sent, but THE UNIT STOPPED ANSWERING before its settings were read back twice") && !u2.model.jobRunning && warned(u2),
                "after the command, before its read-back: a warning: \(note(u2))")
        // The helper refuses the command (here by the test's word; at the Mac, by one of its rules).
        let h = open()
        h.fake.refuseMemory = "a refusal made by the test; the message was not sent"
        h.model.prepareMemory(store5)
        h.model.confirmMemory()
        h.settle(4_000_000)
        t.check(note(h).contains("NOT DONE. THE DRIVER'S HELPER REFUSED the command and sent nothing to the unit (a refusal made by the test")
                    && !h.model.snap.writesAllowed && !h.model.snap.memoryAllowed && !h.helper.changesOn && h.fake.memoryCommands.isEmpty
                    && simMemory(h.fake.unit, 4) == factory && file(h, named(h, "before Save to Memory 5")) != nil,
                "refused at the helper: everything switched off, the backup still there: \(note(h))")
        // Changes off: Recall, Save and Reset are off with them.
        let w = open()
        w.model.allowChanges(false)
        w.settle()
        t.check(!w.model.snap.writesAllowed && !w.helper.changesOn && !w.model.snap.memoryAllowed && w.model.prepareMemory(store5) == readOnly,
                "changes off: Recall, Save and Reset are off with them")
        // A helper that names fewer commands.
        let f = real()
        f.fake.passesMemory = FakeHelper.version17Memory & ~(PANEL_PASS_MEM_STORE_0 << 4)
        f.fake.tellPasses()
        f.settle()
        f.model.prepareMemory(store5)
        let fBlocked = f.helper.blocked
        t.check(f.model.snap.memoryAllowed && f.model.memoryBlocker(store5)?.contains("doesn't pass on this command") == true && f.model.memoryBlocker(load5) == nil
                    && f.model.confirmMemory() == readOnly && f.model.refusal?.kind == .notPassed && !hand(f, store5.bytes) && f.helper.blocked == fBlocked + 1
                    && f.fake.memoryCommands.isEmpty && f.fake.refused == 0,
                "a helper that doesn't name Save to Memory 5: refused by the app and stopped by the transport")
        // Switching changes off while a command is being read back waits for it.
        let o = open()
        o.model.prepareMemory(store5)
        o.model.confirmMemory()
        o.settle(500_000)
        o.model.allowChanges(false)
        t.check(o.model.snap.writesAllowed && o.model.refusal?.text.contains("as soon as Save to Memory 5 has finished") == true, "changes stay on until it has finished")
        o.settle(4_000_000)
        t.check(!o.model.snap.writesAllowed && note(o).contains("the same as before, all 256 bytes"), "then they go off, and the result is in: \(note(o))")
        // The pretend unit: the same job, its files marked, no helper and so no five seconds.
        let p = Bench()
        p.openUp()
        p.model.setVolume(strip(inp, 0), minus6)
        p.settle()
        t.check(run(p, MemoryCommand(kind: .store, memory: 2), wait: 2_000_000) == ok && run(p, MemoryCommand(kind: .load, memory: 2), wait: 2_000_000) == ok
                    && named(p, "before Save to Memory 3 pretend unit") != "" && named(p, "Memory 3 as read pretend unit") != ""
                    && note(p).contains("read back 2 times (0.5 s and 1.0 s after it)") && panel_sim_memory_commands(p.unit.sim) == 2,
                "the pretend unit: Store and Load one after the other, backups marked as its own: \(note(p)) \(p.backupFiles)")

        // A backup file into a memory: put, Store, put back.
        let i = open()
        i.model.setVolumeExact(strip(inp, 0), 0x100001)
        i.settle()
        i.model.setPatch(output: 1, source: 3)
        i.settle()
        let mine = simBlock(i.fake.unit)
        var other = factory
        other[6] = mine[6]
        other[12] = 2
        let input2 = PanelModel.volumeOffset(strip(inp, 1)), pan5 = PanelModel.panOffset(strip(inp, 4))
        other.replaceSubrange(input2..<input2 + 6, with: PanelModel.volumeDigits(minus12))
        other.replaceSubrange(pan5..<pan5 + 4, with: [1, 2, 3, 4])
        let source = write(i, "2026-11-21 10.00 Memory 3 as read.ua101", other)
        let store3 = PanelModel.storeBoth(2)
        t.equal(i.model.prepareIntoMemory(from: source, memory: 2), ok, "Copy Backup to Memory, step 1: the file is taken")
        t.check(i.model.pendingIntoMemory?.memory == 2 && i.model.pendingIntoMemory?.bytes == store3.bytes
                    && Format.hex(store3.bytes) == "F0 41 10 00 69 12 00 00 05 04 00 00 02 02 73 F7" && i.model.pendingIntoMemory?.plan.changes.count == 5
                    && i.model.intoMemoryBlocker() == nil,
                "it holds the plan (5 settings differ) and the Store it will send: \(i.model.pendingIntoMemory?.plan.changes.map { $0.what } ?? [])")
        i.model.setIntoMemory(4)
        t.check(i.model.pendingIntoMemory?.memory == 4 && Format.hex(i.model.pendingIntoMemory?.bytes ?? []) == "F0 41 10 00 69 12 00 00 07 04 00 00 02 02 71 F7", "another memory: another message")
        i.model.setIntoMemory(2)
        passed = i.fake.passedOn
        i.settle(5_000_000)
        t.check(i.fake.passedOn == passed && !i.model.jobRunning, "step 1 sent nothing")
        i.model.cancelIntoMemory()
        t.check(i.model.pendingIntoMemory == nil && i.model.confirmIntoMemory() == Int32(PANEL_ERR_TICKET), "Cancel: nothing to confirm any more")
        i.model.prepareIntoMemory(from: source, memory: 2)
        var first = i.fake.changes.count
        t.equal(i.model.confirmIntoMemory(), ok, "step 2: accepted")
        i.settle(20_000_000)
        var done = note(i)
        t.check(Array(simMemory(i.fake.unit, 2)[12...247]) == Array(other[12...247]), "Memory 3 holds the file's patch bay and mixer, byte for byte")
        t.check(simBlock(i.fake.unit) == mine, "and the settings in use are back as they were, all 256 bytes")
        t.check(done.contains("Copy Backup to Memory 3, “2026-11-21 10.00 Memory 3 as read.ua101”: saved.")
                    && done.contains("saved to Memory 3 with F0 41 10 00 69 12 00 00 05 04 00 00 02 02 73 F7")
                    && done.contains("The Save left the settings in use as they were, all 256 bytes")
                    && done.contains("Your settings are back as before, all 256 bytes (5 changes)") && !warned(i) && !i.model.jobRunning,
                "it says so, as a plain message: \(done)")
        let sent = Array(i.fake.changes[first...])
        t.check(i.fake.memoryCommands == [store3.bytes] && sent.count == 15 && sent.first == pauseMsg && sent.last == resumeMsg
                    && sent.filter { $0 == pauseMsg }.count == 2 && sent[7] == store3.bytes && i.fake.refused == 0,
                "on the cable: a pause, 5 changes, a resume; the Store; a pause, 5 changes, a resume (\(sent.count) messages)")
        t.check(i.fake.memoryRecords.last.map { Array($0[12...247]) } == Array(other[12...247]),
                "the helper's record from just before the Store is the file's settings on the unit: a read came after the last change")
        t.check(file(i, named(i, "before Copy Backup to Memory 3"))?.block == mine, "the settings from before were saved by themselves: \(i.backupFiles)")
        // A file that holds what the unit already has: only the Store is sent.
        rest(i)
        let same = write(i, "same.ua101", mine)
        i.model.prepareIntoMemory(from: same, memory: 0)
        first = i.fake.changes.count
        i.model.confirmIntoMemory()
        i.settle(10_000_000)
        t.check(Array(i.fake.changes[first...]) == [PanelModel.storeBoth(0).bytes] && Array(simMemory(i.fake.unit, 0)[12...247]) == Array(mine[12...247])
                    && note(i).contains("saved.") && note(i).contains("Your settings are back as before, all 256 bytes (0 changes)") && !warned(i),
                "a file equal to the settings in use: one message, the Store: \(note(i))")
        // A volume above 0 dB in the file goes into the memory as it is (since 2026-11-27).
        rest(i)
        var hot = other
        let input4 = PanelModel.volumeOffset(strip(inp, 3))
        hot.replaceSubrange(input4..<input4 + 6, with: PanelModel.volumeDigits(0x400000))
        i.model.prepareIntoMemory(from: write(i, "hot.ua101", hot), memory: 1)
        i.model.confirmIntoMemory()
        i.settle(20_000_000)
        t.check(SettingsBackup.value(simMemory(i.fake.unit, 1), Int32(PANEL_F_VOLUME), inp, 3) == 0x400000 && !note(i).contains("Held")
                    && simBlock(i.fake.unit) == mine,
                "a file with a volume of +6 dB: the memory gets +6 dB there: \(note(i))")
        // The unit doesn't take the file's settings: nothing is stored, and yours are put back.
        rest(i)
        i.fake.unit.ignore(kind: Int(PANEL_F_PAN))
        i.model.prepareIntoMemory(from: source, memory: 3)
        var commands = i.fake.memoryCommands.count
        i.model.confirmIntoMemory()
        i.settle(20_000_000)
        i.fake.unit.ignore(kind: -1)
        done = note(i)
        t.check(done.contains("NOT SAVED: the unit didn't take the file's settings: 4 bytes that should match don't") && i.fake.memoryCommands.count == commands
                    && simMemory(i.fake.unit, 3) == factory && simBlock(i.fake.unit) == mine && done.contains("Your settings are back as before, all 256 bytes") && warned(i),
                "a unit that ignores a change: not stored, the memory untouched, your settings back: \(done)")
        i.model.dismissDisagreement()
        // The helper refuses the Store: the file's settings stay on the unit, and it says how to get yours back.
        rest(i)
        i.model.prepareIntoMemory(from: source, memory: 3)
        i.model.confirmIntoMemory()
        i.settle(300_000)
        i.fake.refuseMemory = "a refusal made by the test; the message was not sent"
        i.settle(20_000_000)
        done = note(i)
        t.check(done.contains("NOT SAVED. THE DRIVER'S HELPER REFUSED the Save") && done.contains("THE SETTINGS IN USE NOW HOLD THE FILE'S VALUES")
                    && !i.model.snap.writesAllowed && Array(simBlock(i.fake.unit)[12...247]) == Array(other[12...247]) && simMemory(i.fake.unit, 3) == factory
                    && i.fake.memoryCommands.count == commands && warned(i),
                "the helper refuses the Store: nothing stored, and the note says where things stand: \(done)")
        // The file the note names last: the one saved by itself at the start of this run.
        let way = done.components(separatedBy: "“").last?.components(separatedBy: "”").first ?? ""
        i.fake.refuseMemory = nil
        i.model.allowChanges(true)
        i.settle()
        t.check(way.contains("before Copy Backup to Memory 4") && file(i, way)?.block == mine
                    && i.model.preparePutBack(from: url(i, way)) == ok && i.model.confirmPutBack() == ok, "the way back it names: Put Back from that file (\(way))")
        i.settle(8_000_000)
        t.check(simBlock(i.fake.unit) == mine && i.model.putBackNote?.contains("match the backup, all 256 bytes") == true, "and your settings are back: \(i.model.putBackNote ?? "nil")")
        // The unit goes away while your settings are on their way back.
        i.settle()
        rest(i)
        i.model.prepareIntoMemory(from: source, memory: 3)
        commands = i.fake.memoryCommands.count
        i.model.confirmIntoMemory()
        var steps = 0
        while !(i.model.memoryNote?.contains("Putting your settings back") ?? false), steps < 400 {
            i.settle(20_000)
            steps += 1
        }
        i.fake.unitGoes()
        i.settle(6_000_000)
        done = note(i)
        t.check(i.fake.memoryCommands.count == commands + 1 && done.contains("saved. THE PUT BACK OF YOUR SETTINGS STOPPED part way: the unit went away after")
                    && done.contains("Open Backup") && !i.model.jobRunning && warned(i),
                "the unit switched off during the last leg: stored, and a warning says how to get your settings back: \(done)")
        // What it refuses before anything is sent.
        let r = open()
        t.check(r.model.prepareIntoMemory(from: write(r, "pretend.ua101", factory, unit: "Pretend unit"), memory: 0) == Int32(PANEL_ERR_VALUE)
                    && r.model.refusal?.text.contains("made from the pretend unit") == true && r.model.pendingIntoMemory == nil,
                "a pretend unit's backup never goes into the UA-101's memories")
        t.equal(r.model.prepareIntoMemory(from: write(r, "ok.ua101", simBlock(r.fake.unit)), memory: 5), Int32(PANEL_ERR_VALUE), "there is no sixth memory")
        r.model.prepareIntoMemory(from: url(r, "ok.ua101"), memory: 0)
        r.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: false)
        r.settle()
        t.check(r.model.intoMemoryBlocker()?.contains("SOFT CTRL") == true && r.model.confirmIntoMemory() == Int32(PANEL_ERR_SOFT_CTRL) && r.fake.changes.isEmpty,
                "SOFT CTRL out: refused")
        r.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        r.settle()
        r.model.allowChanges(false)
        t.check(r.model.intoMemoryBlocker()?.contains("Allow Changes") == true && r.model.confirmIntoMemory() == readOnly && r.fake.changes.isEmpty,
                "changes switched off: refused (\(r.model.intoMemoryBlocker() ?? "nil"))")
        t.check(!r.model.snap.memoryAllowed && !r.helper.changesOn && !hand(r, store5.bytes) && r.fake.memoryCommands.isEmpty
                    && r.fake.refused == 0,
                "switched off in the app: the transport is shut too, and a Store handed to it goes nowhere")
        t.check(r.model.pendingIntoMemory != nil, "the chosen file is still there")
        r.fake.unitGoes()
        r.settle()
        t.check(r.model.pendingIntoMemory == nil, "the unit goes away: the chosen file is dropped")
        let cold = Bench(connect: false)
        t.check(cold.model.prepareIntoMemory(from: write(cold, "cold.ua101", factory, unit: "Pretend unit"), memory: 0) == Int32(PANEL_ERR_NOT_READY)
                    && cold.model.pendingIntoMemory == nil && cold.model.memoryBlocker()?.contains("aren't known") == true,
                "before the unit's settings are known: no file is taken")
        let swap = open()
        swap.model.prepareIntoMemory(from: write(swap, "swap.ua101", simBlock(swap.fake.unit)), memory: 0)
        t.check(swap.model.pendingIntoMemory != nil, "a file chosen for the UA-101")
        swap.model.onChooseDevice?(false)
        swap.settle()
        t.check(swap.model.pendingIntoMemory == nil && !swap.helper.changesOn, "the pretend unit chosen: the file is dropped, and the UA-101's changes are off")
        // A helper that doesn't pass on the whole mixer: no backup goes into a memory through it.
        let thin = real()
        thin.fake.passes = FakeHelper.version17 & ~(UInt64(PANEL_PASS_VOLUME_CHANNEL_0) << 7)
        thin.fake.tellPasses()
        thin.settle()
        thin.model.prepareIntoMemory(from: write(thin, "thin.ua101", simBlock(thin.fake.unit)), memory: 0)
        t.check(thin.model.snap.memoryAllowed && thin.model.confirmIntoMemory() == readOnly && thin.model.refusal?.kind == .notPassed && thin.fake.changes.isEmpty,
                "a helper that leaves out one volume: refused")

        // Changes off: the dialogs say why nothing can be sent.
        let q = real()
        q.model.allowChanges(false)
        q.settle()
        t.check(!q.model.snap.memoryAllowed && !q.model.snap.writesAllowed, "changes switched off: Recall, Save and Reset are off too")
        t.check(q.model.memoryBlocker()?.contains("Changes are switched off") == true, "and the dialogs say why")
        // One job at a time.
        let two = open()
        two.model.prepareMemory(store5)
        two.model.confirmMemory()
        t.check(two.model.prepareMemory(load5) == busy && two.model.pendingMemory == nil && two.model.memoryBlocker()?.contains("still running") == true,
                "while a Store is under way no other command is even prepared")
        two.settle(4_000_000)
        // The helper's list gets shorter between the click and the command.
        let shrink = open()
        shrink.model.prepareMemory(store5)
        shrink.fake.passesMemory = FakeHelper.version17Memory & ~(PANEL_PASS_MEM_STORE_0 << 4)
        shrink.fake.tellPasses()
        t.equal(shrink.model.confirmMemory(), ok, "the helper's new list isn't in yet: accepted")
        shrink.settle(4_000_000)
        t.check(note(shrink).contains("The driver's helper doesn't pass on this command. Nothing was sent") && shrink.fake.memoryCommands.isEmpty && shrink.fake.refused == 0,
                "a helper that no longer names the command when the read is in: nothing sent: \(note(shrink))")

        // More of "into a memory": what can happen between its legs.
        /// A unit with a set-up of its own, a file with another, and the wait until the note
        /// holds these words (a leg has begun).
        func staged() -> (Bench, [UInt8], URL, [UInt8]) {
            let b = open()
            b.model.setVolumeExact(strip(inp, 0), 0x100001)
            b.settle()
            b.model.setPatch(output: 1, source: 3)
            b.settle()
            let own = simBlock(b.fake.unit)
            var theirs = factory
            theirs[6] = own[6]
            theirs[12] = 2
            let pan = PanelModel.panOffset(strip(inp, 4))
            theirs.replaceSubrange(pan..<pan + 4, with: [1, 2, 3, 4])
            return (b, own, write(b, "theirs.ua101", theirs), theirs)
        }
        func until(_ b: Bench, _ words: String) {
            var steps = 0
            while !(b.model.memoryNote?.contains(words) ?? false), steps < 600 {
                b.settle(20_000)
                steps += 1
            }
        }
        // Sound starts while the file's settings go onto the unit: no Store, and yours go back.
        do {
            let (b, own, source, _) = staged()
            b.model.prepareIntoMemory(from: source, memory: 1)
            b.model.confirmIntoMemory()
            until(b, "on their way")
            b.fake.setStreaming(true)
            b.settle(20_000_000)
            t.check(note(b).contains("NOT SAVED: sound started to play or record through the unit, and no Save is sent while it does")
                        && note(b).contains("Your settings are back as before, all 256 bytes") && simBlock(b.fake.unit) == own && simMemory(b.fake.unit, 1) == factory
                        && b.fake.memoryCommands.isEmpty && b.fake.refused == 0 && b.model.snap.writesAllowed,
                    "sound starts during the first leg: not stored, your settings back, and the helper had nothing to refuse: \(note(b))")
        }
        // A Store after which the settings in use differ: said, and yours still go back.
        do {
            let (b, own, source, theirs) = staged()
            b.model.prepareIntoMemory(from: source, memory: 1)
            b.model.confirmIntoMemory()
            until(b, ": sent F0 41") // the Store has left
            panel_sim_poke(b.fake.unit.sim, 13, 5)
            b.settle(20_000_000)
            t.check(note(b).contains("saved.") && note(b).contains("THE SAVE CHANGED THE SETTINGS IN USE in 1 byte, which it should not do (the first: d[13])")
                        && note(b).contains("Your settings are back as before, all 256 bytes") && simBlock(b.fake.unit) == own
                        && Array(simMemory(b.fake.unit, 1)[12...247]) == Array(theirs[12...247]) && warned(b),
                    "a Store that changes the settings in use: a warning: \(note(b))")
        }
        // The long put back's test file, when this
        // run is given one (UA101_SELFTEST_LONG_FILE: a backup of the stand-in UA-101 as it is
        // when it attaches, and the test file made from it): every one of the 82 settings
        // changes, in 84 messages inside one pause, and the unit then holds the file's.
        if let path = ProcessInfo.processInfo.environment["UA101_SELFTEST_LONG_FILE"] {
            let b = real()
            let start = simBlock(b.fake.unit)
            let first = b.fake.changes.count
            t.equal(b.model.preparePutBack(from: URL(fileURLWithPath: path)), ok, "the long test file is taken: \(b.model.refusal?.text ?? "")")
            let plan = b.model.pendingPutBack?.plan
            t.equal(plan?.changes.count ?? 0, 82, "all 82 settings differ")
            t.check(plan?.held.isEmpty == true, "no volume above 0 dB in it")
            t.equal(b.model.confirmPutBack(), ok, "the long put back: accepted")
            b.settle(12_000_000)
            let after = simBlock(b.fake.unit), sent = Array(b.fake.changes[first...])
            let wanted = b.model.putBackNote ?? "nil"
            t.check(sent.count == 84 && sent.first == pauseMsg && sent.last == resumeMsg && b.fake.refused == 0 && b.fake.tooMany == 0,
                    "84 messages inside one pause, none refused, never more than the helper's 20 a second (\(sent.count))")
            t.check(wanted.contains("82 changes sent (84 messages") && (wanted.contains("match the backup, all 256 bytes") || wanted.contains("matches the backup in everything this app can put back"))
                        && !wanted.contains("THE UNIT DIFFERS"), "and the unit holds the file's settings: \(wanted)")
            t.check((0..<256).filter { after[$0] != start[$0] }.count >= 200 && Array(after[0...11]) == Array(start[0...11]), "over 200 bytes changed; the switches and the flags didn't")
            t.check(Allowed.volumeStrips.allSatisfy { SettingsBackup.value(after, Int32(PANEL_F_VOLUME), $0.group, $0.channel) < unity
                                                         && SettingsBackup.value(after, Int32(PANEL_F_ON), $0.group, $0.channel) == 1 }
                        && (0..<5).allSatisfy { (7...11).contains(Int(after[12 + $0])) },
                    "every channel is off and below 0 dB, and the patch bay keeps to the computer's outputs")
        }
        // The unit doesn't take your settings back whole: said, with the bytes.
        do {
            let (b, _, source, _) = staged()
            b.model.prepareIntoMemory(from: source, memory: 1)
            b.model.confirmIntoMemory()
            until(b, "Putting your settings back")
            b.fake.unit.ignore(kind: Int(PANEL_F_PAN))
            b.settle(20_000_000)
            t.check(note(b).contains("saved.") && note(b).contains("THE UNIT DIFFERS from your settings before in 4 bytes that should match")
                        && note(b).contains("Open Backup") && warned(b) && !b.model.jobRunning,
                    "a unit that ignores a change on the way back: a warning that names the bytes: \(note(b))")
        }
    }

    // ---- 16e
    if t.begin("the memory preview in Recall Memory (link version 20)") {
        func simBlock(_ u: PretendUnit) -> [UInt8] {
            var block = [UInt8](repeating: 0, count: 256)
            panel_sim_block(u.sim, &block)
            return block
        }
        /// The unit's settings in use with INPUT 1 at -6 dB, and (`muted`) INPUT 1 muted too:
        /// the two test mixes the owner left in Memory 1 and Memory 5 (2026-11-26).
        func mix(_ u: PretendUnit, muted: Bool) -> [UInt8] {
            var b = simBlock(u)
            let at = PanelModel.volumeOffset(strip(inp, 0))
            b.replaceSubrange(at..<at + 6, with: PanelModel.volumeDigits(minus6))
            if muted { let on = PanelModel.onPlace(strip(inp, 0)); b[on.byte] |= UInt8(1 << on.bit) }
            return b
        }
        func held(_ m: PanelModel, _ memory: Int) -> MemoryPreview? {
            if case .read(let p) = m.memoryPreview(memory) { return p }
            return nil
        }
        func setMemories(_ u: PretendUnit) {
            panel_sim_set_memory(u.sim, 0, mix(u, muted: false))
            panel_sim_set_memory(u.sim, 1, simBlock(u))
            panel_sim_set_memory(u.sim, 4, mix(u, muted: true))
        }
        let volume1 = PutBackPlan.Change(what: "INPUT 1 volume", from: Format.dB(unity) + " dB", to: Format.dB(minus6) + " dB")
        let mute1 = PutBackPlan.Change(what: "INPUT 1 mute", from: "off", to: "on")

        // The pretend unit: read-only, as the app starts.
        let b = Bench()
        setMemories(b.unit)
        let before = b.model.snap
        let tick = b.model.previewChanged
        t.equal(b.model.memoryPreview(0), .notRead, "nothing read before it is asked for")
        b.model.readMemoryPreview(0)
        b.settle()
        t.check(held(b.model, 0)?.changes(patchBay: true, mixer: true) == [volume1], "Memory 1: INPUT 1 volume, 0 dB now, -6 dB in the memory, and nothing else")
        t.check(held(b.model, 0)?.changes(patchBay: true, mixer: false) == [], "and nothing in the patch bay")
        t.check(b.model.previewChanged > tick, "the dialog is told")
        t.equal(panel_sim_memory_reads(b.unit.sim), 1, "one memory read reached the unit")
        t.check(b.model.snap == before && panel_sim_sets_applied(b.unit.sim) == 0 && panel_sim_memory_commands(b.unit.sim) == 0,
                "the settings in use didn't change, in the app or on the unit; nothing else was sent")
        b.model.readMemoryPreview(4)
        b.settle()
        t.check(held(b.model, 4)?.mixer == [volume1, mute1], "Memory 5: INPUT 1 at -6 dB and muted")
        b.model.readMemoryPreview(1)
        b.settle()
        t.check(held(b.model, 1) == MemoryPreview(), "Memory 2 holds the settings in use: nothing would change")
        t.check(b.model.previewBlocker(5) != nil && b.model.memoryPreview(5) != .notRead, "there is no Memory 6")

        // Never while a fader is held; asked again after, it goes.
        let reads = panel_sim_memory_reads(b.unit.sim)
        b.openUp()
        b.model.beginDrag()
        b.model.readMemoryPreview(2)
        b.settle()
        t.equal(panel_sim_memory_reads(b.unit.sim), reads, "no read while a fader is held")
        b.model.endDrag()
        b.settle()
        b.model.readMemoryPreview(2)
        b.settle()
        t.equal(panel_sim_memory_reads(b.unit.sim), reads + 1, "after it, the read goes")

        // Never while a job runs; a Recall forgets what was read.
        b.model.prepareMemory(MemoryCommand(kind: .load, memory: 0, patchBay: true, mixer: true))
        b.model.confirmMemory()
        t.check(b.model.jobRunning, "a Recall under way")
        b.model.readMemoryPreview(4)
        t.check(b.model.memoryPreview(4) != .reading && b.model.previewBlocker(4) != nil, "the preview waits while a job runs: \(b.model.previewBlocker(4) ?? "nil")")
        b.settle(4_000_000)
        t.check(!b.model.jobRunning && panel_sim_memory_reads(b.unit.sim) == reads + 1, "no memory read meanwhile")
        t.equal(b.model.memoryPreview(4), .notRead, "after the Recall, what was read is forgotten")
        b.model.readMemoryPreview(0)
        b.settle()
        t.check(held(b.model, 0) == MemoryPreview(), "read again: Memory 1 is now what is in use")

        // The UA-101 behind the stand-in helper, changes off.
        let g = Bench()
        setMemories(g.fake.unit)
        g.chooseReal()
        t.check(FakeHelper.version20Read == panel_helper_passes_read() && g.helper.allowed.memoryReads == Set(0..<5),
                "the helper names the five memories; the app knows them")
        let passed = g.fake.passedOn
        g.model.readMemoryPreview(0)
        g.settle()
        t.check(held(g.model, 0)?.changes(patchBay: true, mixer: true) == [volume1] && g.fake.memoryReads == [0] && g.fake.passedOn == passed + 1 && g.fake.refused == 0,
                "through the helper with changes off: one read, passed on, and the preview")
        t.check(g.fake.changesPassedOn == 0 && panel_sim_sets_applied(g.fake.unit.sim) == 0 && !g.model.snap.writesAllowed, "nothing changed on the unit")
        var request = [UInt8](repeating: 0, count: 16)
        panel_memory_read_message(0, &request)
        t.equal(Format.hex(request), "F0 41 10 00 69 11 02 03 00 00 00 00 02 00 79 F7", "the read of Memory 1, byte for byte")

        // An older helper (link version 19): no preview, said plainly; the Recall still works.
        g.fake.passesRead = 0
        g.fake.tellPasses()
        g.settle()
        let old = g.model.memoryPreview(4)
        t.check(old == .cantShow(g.model.previewBlocker(4) ?? "") && g.model.previewBlocker(4)?.contains("older") == true, "an older helper: can't show it, and why")
        g.model.readMemoryPreview(4)
        g.settle()
        let stopped = request.withUnsafeBufferPointer { g.helper.send($0) }
        t.check(g.fake.memoryReads == [0] && !stopped, "nothing asked of an older helper; the transport itself stops the read")
        g.fake.passesRead = FakeHelper.version20Read
        g.fake.tellPasses()
        g.settle()

        // No answer: tried again, then "can't show it".
        g.fake.memoryReadsLost = true
        g.model.readMemoryPreview(3)
        g.settle(10_000_000)
        if case .cantShow(let why) = g.model.memoryPreview(3) {
            t.check(why.contains("didn't answer"), "a memory that never answers: can't show it: \(why)")
        } else { t.check(false, "a memory that never answers: \(g.model.memoryPreview(3))") }
        t.check(g.model.snap.state == .ready, "and the unit is still read")
    }

    // ---- 17
    if t.begin("every window draws") {
        let app = NSApplication.shared
        app.setActivationPolicy(.prohibited)
        let dir = NSTemporaryDirectory() + "ua101panel-selftest-\(getpid())"
        let files = renderAllWindows(into: dir)
        t.equal(files.count, 42, "42 pictures written")
        // The mixer window is as wide as its strips, whatever a banner says.
        func width(_ prefix: String) -> Int {
            files.first { ($0 as NSString).lastPathComponent.hasPrefix(prefix) }
                .flatMap { NSImage(contentsOfFile: $0)?.representations.first?.pixelsWide } ?? 0
        }
        for n in ["02", "05", "21", "22", "24", "26", "27"] {
            t.equal(width(n), width("01"), "picture \(n): a banner doesn't widen the mixer window")
        }
        for n in ["09", "25", "28"] {
            t.equal(width(n), width("08"), "picture \(n): a banner doesn't widen the patch bay window")
        }
        // 2026-11-16: whatever the state or the messages, the mixer window is always as high:
        // nothing above the mixer grows or shrinks.
        func height(_ prefix: String) -> Int {
            files.first { ($0 as NSString).lastPathComponent.hasPrefix(prefix) }
                .flatMap { NSImage(contentsOfFile: $0)?.representations.first?.pixelsHigh } ?? 0
        }
        for n in ["09", "25"] {
            t.equal(height(n), height("08"), "picture \(n): the Patch Bay is as high as in picture 08, whatever the state")
        }
        for n in ["15", "19"] {
            t.equal(height(n), height("10"), "picture \(n): the Load dialog is as high as in picture 10, whatever the state")
        }
        for n in ["20", "32"] {
            t.equal(height(n), height("16"), "picture \(n): Driver Settings is as high as in picture 16, whatever was measured")
            t.equal(width(n), width("16"), "picture \(n): and as wide")
        }
        // 2026-11-20: no warnings any more, so the shortest values make the window no larger.
        t.check(height("35") == height("16") && width("35") == width("16"), "picture 35: the shortest values: the same size as picture 16")
        // And no red or amber on the sliders, in either look, whatever the values. Measured keeps its colours:
        // amber and red there when room was thin or sound was lost (pictures 32 and 35), none
        // when there was room (16, 20), and none at all with nothing measured (33, 34).
        func picture(_ n: String) -> String { files.first { ($0 as NSString).lastPathComponent.hasPrefix(n) } ?? "" }
        for n in ["16", "20", "32", "33", "34", "35"] {
            let top = redOrAmberPixels(in: picture(n), above: 0.62)
            t.check(top == 0, "picture \(n), Driver Settings' sliders and notes: \(top) red or amber pixels")
        }
        for n in ["16", "20", "33", "34"] {
            let all = redOrAmberPixels(in: picture(n))
            t.check(all == 0, "picture \(n), all of Driver Settings with room or nothing measured: \(all) red or amber pixels")
        }
        for n in ["32", "35"] {
            let all = redOrAmberPixels(in: picture(n))
            t.check(all > 1000, "picture \(n), sound lost: Measured is red and amber (\(all) pixels)")
        }
        // The same count finds them where they belong: picture 30's red warning in Messages
        // (and the old pictures 16 and 35 had 804 and 20,098).
        let red30 = redOrAmberPixels(in: files.first { ($0 as NSString).lastPathComponent.hasPrefix("30") } ?? "")
        t.check(red30 > 1000, "picture 30, a warning in Messages: \(red30) red pixels (the count works)")
        for n in ["02", "04", "05", "06", "07", "21", "22", "24", "26", "27"] {
            t.equal(height(n), height("01"), "picture \(n): the mixer window is as high as in picture 01, whatever the state and the messages")
        }
        for f in files {
            let rep = NSImage(contentsOfFile: f)?.representations.first
            let w = rep?.pixelsWide ?? 0, h = rep?.pixelsHigh ?? 0
            t.check(w >= 300 && h >= 150, "\((f as NSString).lastPathComponent): \(w) x \(h) pixels")
        }
        try? FileManager.default.removeItem(atPath: dir)
    }

    // ---- 18
    if t.begin("Patch Bay and Driver Settings: windows of their own (2026-11-20)") {
        // The top line: Driver Settings… between the MIDI lamps and Messages.
        t.equal(MixerWindowView.lineButtons, ["Driver Settings…", "Messages", "Patch Bay…", "Recall…", "Save…", "Reset…"],
                "the buttons after the MIDI lamps")
        // Recall, Save and Reset are sheets on the mixer; Patch Bay and Driver Settings are sheets
        // too unless the View menu puts each in its own window (the user, 10:40). As delivered:
        // sheets, each choice kept on its own.
        t.check(PanelUI.Sheet(rawValue: "patchBay") == .patchBay && PanelUI.Sheet(rawValue: "driverSettings") == .driverSettings
                    && PanelUI.Sheet(rawValue: "load") == .load, "Patch Bay and Driver Settings can be sheets again, as Load, Store, Reset are")
        let suite = "ua101panel.selftest.ownwindow"
        if let d = UserDefaults(suiteName: suite) {
            d.removePersistentDomain(forName: suite)
            let pk = PanelUI.patchBayOwnWindowKey, dk = PanelUI.driverSettingsOwnWindowKey
            t.check(!PanelUI.ownWindow(pk, in: d) && !PanelUI.ownWindow(dk, in: d), "as delivered: both are sheets (unchecked)")
            d.set(true, forKey: dk)
            t.check(!PanelUI.ownWindow(pk, in: d) && PanelUI.ownWindow(dk, in: d), "Driver Settings checked: its own window, Patch Bay still a sheet")
            d.set(true, forKey: pk); d.set(false, forKey: dk)
            t.check(PanelUI.ownWindow(pk, in: d) && !PanelUI.ownWindow(dk, in: d), "Patch Bay checked alone: each is chosen separately")
            d.removePersistentDomain(forName: suite)
        }
        // The choice is kept: the menu item writes it (AppCore.setOwnWindow), the next start reads it.
        let src = (try? String(contentsOfFile: #filePath.replacingOccurrences(of: "SelfTest.swift", with: "PanelApp.swift"), encoding: .utf8)) ?? ""
        t.check(src.isEmpty || (src.contains("UserDefaults.standard.set(on, forKey: PanelUI.patchBayOwnWindowKey)")
                    && src.contains("UserDefaults.standard.set(on, forKey: PanelUI.driverSettingsOwnWindowKey)")
                    && src.contains("ui.patchBayOwnWindow = PanelUI.ownWindow(PanelUI.patchBayOwnWindowKey, in: .standard)")
                    && src.contains("ui.driverSettingsOwnWindow = PanelUI.ownWindow(PanelUI.driverSettingsOwnWindowKey, in: .standard)")),
                "each choice is written when changed and read at the next start")
        let ui0 = PanelUI()
        t.check(!ui0.patchBayOwnWindow && !ui0.driverSettingsOwnWindow, "a new PanelUI starts with both as sheets")
        // Never above other windows, Always on Top or not; the mixer and Messages follow it.
        for kind in DialogWindow.Kind.allCases {
            t.check(DialogWindow.level(title: kind.title, alwaysOnTop: true) == .normal
                        && DialogWindow.level(title: kind.title, alwaysOnTop: false) == .normal, "\(kind.title): never on top")
        }
        t.check(DialogWindow.level(title: "UA-101 Control", alwaysOnTop: true) == .floating
                    && DialogWindow.level(title: MessagesWindow.title, alwaysOnTop: true) == .floating
                    && DialogWindow.level(title: "UA-101 Control", alwaysOnTop: false) == .normal, "the mixer and Messages follow Always on Top as before")
        // One of each: asking again gives the same window. Made, never shown here.
        var made = 0
        let w = DialogWindow(kind: .patchBay) { _ in made += 1; return AnyView(Text("x")) }
        let first = w.prepare(), second = w.prepare()
        t.check(first === second && first.level == .normal && !first.isVisible && made == 0,
                "asked twice: one window, ordinary level, not shown, no content until it opens")
        t.check(first.title == "UA-101 Patch Bay" && first.styleMask.contains(.titled) && first.styleMask.contains(.closable)
                    && !first.isReleasedWhenClosed && !first.hidesOnDeactivate, "a movable window with a close button, kept when closed")
        var closed = 0
        w.onClose = { closed += 1 }
        w.windowWillClose(Notification(name: NSWindow.willCloseNotification, object: first))
        t.check(closed == 1 && first.contentViewController == nil, "closing it runs its own step and drops the content")
        // What closing the Patch Bay does: drops an unsent Hard Thru; nothing reaches the unit.
        let b = Bench()
        b.openUp()
        let changes = panel_sim_sets_applied(b.unit.sim)
        t.check(b.model.askHardThru() == Int32(PANEL_OK) && b.model.hardThruAsked, "Hard Thru asked for")
        b.model.cancelHardThru()
        b.settle()
        t.check(!b.model.hardThruAsked && panel_sim_sets_applied(b.unit.sim) == changes, "closed: the ask is gone and nothing was sent")
        // Really opened (off screen) with their real content: they must lay out without AppKit
        // giving up (2026-11-20 both crashed the app on opening, "update constraints"
        // looping between the window's size and the content's).
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        let ui = PanelUI()
        let drv = DriverSettingsModel(source: PretendDriverSettings())
        let kinds: [DialogWindow.Kind] = [.driverSettings, .patchBay]
        // The user's MacBook (2026-11-20): screen 1440 x 900, menu bar 30, resting Dock 58 at
        // the bottom with 38-point icons that magnify to 128. The room is what the grown Dock
        // never reaches: a button row below its top fails (2026-11-20, a design choice).
        let testDock = DockClearance.Dock(side: .bottom, hides: false, magnifies: true, tile: 38, large: 128)
        let testScreen = NSRect(x: 0, y: 0, width: 1440, height: 900)
        let testVisible = NSRect(x: 0, y: 58, width: 1440, height: 812)
        let room = DockClearance.room(frame: testScreen, visible: testVisible, dock: testDock)
        // The grown Dock's top: its margins (58 - 38), the icons at 128.
        let dockTop: CGFloat = 20 + 128
        t.check(room.minY >= dockTop + 1 && room.maxY == testVisible.maxY,
                "the user's screen: dialogs stay above \(Int(room.minY)) (the grown Dock reaches \(Int(dockTop))), up to the menu bar")
        for kind in kinds {
            let dw = DialogWindow(kind: kind) { w in
                kind == .driverSettings
                    ? AnyView(DialogWindowContent(ui: ui, window: w, content: DriverSettingsView(model: drv, close: {})))
                    : AnyView(DialogWindowContent(ui: ui, window: w, content: PatchBayView(model: b.model, close: {}, messages: b.messages, openMessages: {})))
            }
            dw.testRoom = room
            ui.zoom = 1
            dw.openOffScreen()
            RunLoop.current.run(until: Date().addingTimeInterval(0.6))
            let at1 = dw.window?.contentLayoutRect.size ?? .zero
            t.check(at1.width > 200 && at1.height > 100, "\(kind.title) opened and laid out at zoom 1: \(Int(at1.width)) x \(Int(at1.height)), its own size \(Int(dw.natural.width)) x \(Int(dw.natural.height))")
            print("  \(kind.title): own size \(Int(dw.natural.width)) x \(Int(dw.natural.height)), at zoom 1 on the user's MacBook above the grown Dock: zoom \(String(format: "%.2f", dw.fit.zoom))")
            // The mixer zoomed big: the window takes a smaller zoom so that all of it fits.
            ui.zoom = 1.6
            RunLoop.current.run(until: Date().addingTimeInterval(0.4))
            let big = dw.window?.frame.size ?? .zero
            t.check(big.height <= room.height + 0.5 && big.width <= room.width + 0.5 && dw.fit.zoom <= 1.6,
                    "\(kind.title) at the mixer's zoom 1.6: \(Int(big.width)) x \(Int(big.height)), fits the room above the Dock (zoom \(String(format: "%.2f", dw.fit.zoom)))")
            // Placed low on that screen, as if dragged there: its bottom row, the buttons, comes
            // back up above the grown Dock.
            // (Off screen the window itself is never moved, so its size is placed as resize() does.)
            if let w = dw.window {
                let placed = DialogWindow.keep(NSRect(origin: NSPoint(x: 200, y: 30), size: w.frame.size), in: room)
                let buttons = placed.minY
                t.check(buttons >= dockTop + 1 && placed.maxY <= room.maxY + 0.5,
                        "\(kind.title)'s buttons at \(Int(buttons)) points, above the grown Dock's top (\(Int(dockTop)))")
            }
            ui.zoom = 0.8
            RunLoop.current.run(until: Date().addingTimeInterval(0.4))
            let small = dw.window?.contentLayoutRect.size ?? .zero
            t.check(abs(dw.fit.zoom - 0.8) < 0.01 && abs(small.width - dw.natural.width * 0.8) < 3,
                    "\(kind.title) follows the mixer's zoom 0.8 when it fits: \(Int(small.width)) x \(Int(small.height))")
            dw.close()
            RunLoop.current.run(until: Date().addingTimeInterval(0.1))
            t.check(dw.window?.contentViewController == nil, "\(kind.title) closed: its content is dropped")
        }
        // The same on the real screen, invisible: the mixer's zoom so big that both windows
        // would be taller than the screen. This crashed the app before (10:14).
        for kind in kinds {
            let dw = DialogWindow(kind: kind) { w in
                kind == .driverSettings
                    ? AnyView(DialogWindowContent(ui: ui, window: w, content: DriverSettingsView(model: drv, close: {})))
                    : AnyView(DialogWindowContent(ui: ui, window: w, content: PatchBayView(model: b.model, close: {}, messages: b.messages, openMessages: {})))
            }
            ui.zoom = 2.5
            dw.openOffScreen(onScreen: true)
            RunLoop.current.run(until: Date().addingTimeInterval(0.8))
            let v = DockClearance.room(on: NSScreen.main)
            let f = dw.window?.frame ?? .zero
            t.check(f.height > 100 && v.insetBy(dx: -0.5, dy: -0.5).contains(f),
                    "\(kind.title) on the real screen at zoom 2.5: no crash, \(Int(f.width)) x \(Int(f.height)) all above the Dock as it is set now (from \(Int(v.minY)) up)")
            // The window is as big as what is drawn in it, no empty strip right or below
            // (2026-11-20, the user's screenshot: a dark band on both sides).
            let c = dw.window?.contentLayoutRect.size ?? .zero
            let drawn = CGSize(width: dw.natural.width * dw.fit.zoom, height: dw.natural.height * dw.fit.zoom)
            t.check(abs(c.width - drawn.width) < 2 && abs(c.height - drawn.height) < 2,
                    "\(kind.title) on the real screen: window \(Int(c.width)) x \(Int(c.height)), its content drawn \(Int(drawn.width)) x \(Int(drawn.height)) at \(String(format: "%.2f", dw.fit.zoom))")
            // What the content asks of the window at least (AppKit holds a shown window to it):
            // no more than what is drawn, or the window can't shrink to the drawing.
            let hosting = dw.window?.contentViewController?.view as? NSHostingView<AnyView>
            t.check(hosting != nil && hosting?.sizingOptions == [],
                    "\(kind.title): its content sets no least size on the window (that kept a shown window at zoom 1's size)")
            dw.close()
            RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        }
        ui.zoom = 1
        app.setActivationPolicy(.prohibited)
        // The sums behind it.
        t.check(DialogWindow.zoom(natural: CGSize(width: 540, height: 900), wanted: 1.2, room: CGSize(width: 1440, height: 772)) < 0.86
                    && DialogWindow.zoom(natural: CGSize(width: 540, height: 600), wanted: 1.2, room: CGSize(width: 1440, height: 772)) == 1.2,
                "the zoom is the mixer's unless that is too tall for the screen")
        let kept = DialogWindow.keep(NSRect(x: 100, y: 20, width: 500, height: 700), in: room)
        t.check(kept.minY == room.minY && kept.maxY <= room.maxY && kept.minX == 100, "a window reaching under the Dock is moved up, all of it in view")
        // Before this, a dialog stopped at the resting Dock (58): the grown icons covered it.
        let old = DialogWindow.keep(NSRect(x: 100, y: 20, width: 500, height: 700), in: testVisible)
        t.check(old.minY < dockTop && kept.minY > dockTop, "the old room let the buttons sit under the grown Dock; the new one doesn't")
        // The Dock's other settings.
        let plain = DockClearance.room(frame: testScreen, visible: testVisible,
                                       dock: DockClearance.Dock(side: .bottom, magnifies: false, tile: 38, large: 128))
        t.check(plain.minY == 58 + DockClearance.gap, "no magnification: just above the resting Dock, with a small gap")
        let hiding = DockClearance.room(frame: testScreen, visible: NSRect(x: 0, y: 0, width: 1440, height: 870),
                                        dock: DockClearance.Dock(side: .bottom, hides: true, magnifies: true, tile: 38, large: 128))
        t.check(hiding.minY >= 128 + DockClearance.gap, "a hiding Dock: kept clear of where it slides in, grown")
        let ipad = NSRect(x: 1440, y: 88, width: 1133, height: 744)
        t.check(DockClearance.room(frame: ipad, visible: ipad, dock: testDock) == ipad, "a screen without the Dock (the iPad beside the Mac): all of it")
        let left = DockClearance.room(frame: testScreen, visible: NSRect(x: 58, y: 0, width: 1382, height: 870),
                                      dock: DockClearance.Dock(side: .left, magnifies: true, tile: 38, large: 128))
        t.check(left.minX >= dockTop + 1 && left.maxX == 1440 && left.minY == 0, "a Dock on the left: kept clear of it sideways")
        // A sheet hangs from the mixer's title bar: the mixer at the top of the user's screen
        // (content from 842), Driver Settings at 540 x 754, at every zoom of the View menu.
        let ds = CGSize(width: 540, height: 754)
        for wanted in WindowFit.steps {
            let k = DockClearance.sheetZoom(natural: ds, wanted: wanted, top: 842, room: room)
            t.check(842 - ds.height * k >= room.minY - 0.5 && k <= wanted,
                    "Driver Settings as a sheet at the mixer's zoom \(wanted): drawn at \(String(format: "%.2f", k)), buttons at \(Int(842 - ds.height * k)), above the grown Dock")
        }
        // The mixer lower down: the sheet shrinks more, its buttons still above the Dock.
        let lowK = DockClearance.sheetZoom(natural: ds, wanted: 1, top: 600, room: room)
        t.check(600 - ds.height * lowK >= room.minY - 0.5, "mixer lower on the screen: the sheet at \(String(format: "%.2f", lowK)), still above the Dock")
    }

    // ---- The end-user build: the Messages window in plain words.
    if t.begin("plain words for the end-user build") {
        MessageCenter.seen = nil
        let examples: [(String, String)] = [
            ("INPUT 1 volume to −6.0 dB (0x100001): sent F0 41 10 00 69 12 00 04 00 07 00 0A 01 04 09 07 56 F7. The unit took it: d[40…45] 00 02 00 00 00 00 → 00 01 00 00 00 01. Every other byte of the 256 read back the same. The unit said nothing else.",
             "INPUT 1 volume to −6.0 dB: sent. The unit took it. Nothing else changed. The unit said nothing else."),
            ("Pause saving: sent F0 41 10 00 69 12 00 00 00 00 00 00 F7. Reading the settings back…",
             "Pause saving: sent. Reading the settings back…"),
            ("INPUT 1 pan to L50 (0x2000): sent F0 41 10 00 69 12 00 04 00 03 00 00 02 00 78 F7. THE UNIT HOLDS ANOTHER VALUE: d[46…49] 00 00 00 00 → 00 00 00 00, wanted 00 00 02 00. OTHER SETTINGS READ BACK DIFFERENT, 2 bytes: d[5] 00→01, d[9] 01→00.",
             "INPUT 1 pan to L50: sent. THE UNIT HOLDS ANOTHER VALUE. OTHER SETTINGS READ BACK DIFFERENT (2 places)."),
            ("Reset Mixer: 7 changes sent. The unit's mixer reads back as the factory block, every byte (d[22] to d[247]). The settings from before are in “2026-11-16 18.30 before Reset Mixer.ua101”.",
             "Reset Mixer: 7 changes sent. The unit's mixer reads back as the factory block, every byte. The settings from before are in “2026-11-16 18.30 before Reset Mixer.ua101”."),
            ("OUTPUT 2 on/off to bit 1 (off): sent F0 41 10 00 69 12 00 05 01 02 01 77 F7. The unit took it: d[33] 00→02, bit 1. Every other byte of the 256 read back the same.",
             "OUTPUT 2 on/off to off: sent. The unit took it. Nothing else changed."),
            ("Initial Setting: 5 messages sent. The unit took it in all five rows: OUTPUT 1-2 d[12] 02→07, OUTPUT 3-4 d[13] 03→08.",
             "Initial Setting: 5 messages sent. The unit took it in all five rows: OUTPUT 1-2, OUTPUT 3-4."),
            ("3 bytes still differ: d[6] 06, backup 04 (the MONO and SOFT CTRL switches); d[140] 02, backup 04 (INPUT 3 volume). Not put back: d[9] (a patch bay row this app doesn't change): the backup holds 05, the unit 04.",
             "3 bytes still differ: the MONO and SOFT CTRL switches; INPUT 3 volume. Not put back: a patch bay row this app doesn't change (the backup and the unit differ)."),
            ("INPUT link to bit 1 (linked): sent F0 41 10 00 69 12 00 04 08 00 01 73 F7. ", "INPUT link to linked: sent."),
        ]
        for (full, want) in examples {
            t.equal(PlainWords.of(full), want, "plain words: \(full.prefix(40))…")
        }
        // Every message this test has made, in plain words: no bytes, places or hex numbers.
        let texts = seenTexts.sorted()
        let left = texts.filter { PlainWords.hasBytes(PlainWords.of($0)) }
        for l in left.prefix(15) { print("  still has bytes: \(PlainWords.of(l))\n    from: \(l)") }
        t.check(t.only != nil || texts.count > 200, "the whole test made \(texts.count) different message texts")
        t.equal(left.count, 0, "none of the \(texts.count) message texts keeps bytes in plain words")
        // The full text stays as it was: the CSV and the record have it.
        let full = examples[0].0
        t.check(PlainWords.hasBytes(full) && MessageCenter.Message(id: 1, slot: .volume, symbol: "i", title: "Volume", text: full, warning: false, time: Date()).text == full,
                "the message keeps its full text")
        if let path = ProcessInfo.processInfo.environment["UA101_PLAIN_SAMPLES"] {
            try? texts.map { "FULL:  \($0)\nPLAIN: \(PlainWords.of($0))\n" }.joined(separator: "\n").write(toFile: path, atomically: true, encoding: .utf8)
        }
    }

    // ---- Help → Save Report for Developer…, from files made here.
    if t.begin("report for the developer") {
        let dir = NSTemporaryDirectory() + "ua101panel-report-\(getpid())"
        let fm = FileManager.default
        try? fm.removeItem(atPath: dir)
        try? fm.createDirectory(atPath: dir + "/Backups", withIntermediateDirectories: true)
        let home = "/Users/testperson"
        func write(_ name: String, _ text: String) { try? text.write(toFile: dir + "/" + name, atomically: true, encoding: .utf8) }
        write("record.log", "2026-11-21 18:00:00.000  INPUT 1 volume: sent F0 41 10 00 F7\n2026-11-21 18:00:01.000  saved \(home)/Documents/UA-101 Backups/x.ua101 for TestPerson on Test Person's Mac\n")
        write("helper.log", "2026-11-21 17:54:53.915 ua101d: passed on to the unit: resume saving\n")
        write("ua101d", "helper program")
        try? fm.createDirectory(atPath: dir + "/UA101.driver/Contents/MacOS", withIntermediateDirectories: true)
        write("UA101.driver/Contents/MacOS/UA101", "plug-in program")
        (["CFBundleExecutable": "UA101", "CFBundleShortVersionString": "0.3", "CFBundleVersion": "3"] as NSDictionary)
            .write(toFile: dir + "/UA101.driver/Contents/Info.plist", atomically: true)
        write("allow-changes", "")
        let b = Bench()
        b.model.setVolume(strip(inp, 0), minus6) // read-only: a "Not done" message
        b.openUp()
        b.model.setVolume(strip(inp, 0), minus6)
        b.settle()
        let backup = SettingsBackup(block: b.model.settingsAsRead ?? [], unit: "UA-101", saved: "2026-11-21", why: "Save Backup")
        write("Backups/2026-11-21 18.00.ua101", backup.text())
        var src = DeveloperReport.Sources()
        src.appRecord = dir + "/record.log"
        src.helperLog = dir + "/helper.log"
        src.helper = dir + "/ua101d"
        src.daemonPlist = dir + "/none.plist"
        src.audioPlugIn = dir + "/UA101.driver"
        src.midiPlugIn = dir + "/none.plugin"
        src.goAhead = dir + "/allow-changes"
        src.memoryGoAhead = dir + "/allow-memory"
        src.backups = dir + "/Backups"
        src.systemLog = { "2026-11-21 11:52:20.161 Df coreaudiod [io.github.delta90h.ua101:plugin] UA-101 is back as it was" }
        src.driver = { DriverState(found: true, haveSettings: true, inputMs: 8, outputMs: 8, correction: true, haveStats: true, running: false, anyRun: true, rate: 48000, seconds: 60) }
        src.home = home
        src.userNames = ["testperson", "Test Person"]
        src.macNames = ["Test Person's Mac"]
        let when = Date(timeIntervalSince1970: 1_795_000_000)
        let text = DeveloperReport.make(at: when, unit: .from(b.model), messages: b.messages.csv(), from: src)
        for title in ["Versions", "The unit's settings, as the app last read them", "The driver's settings", "Messages of this run",
                      "Newest backups", "The app's record", "The helper's log", "The plug-ins' system log lines"] {
            t.check(text.contains("\n" + title), "the report has “\(title)”")
        }
        t.check(text.contains("sent F0 41 10 00 F7") && text.contains("resume saving") && text.contains("UA-101 is back as it was"),
                "it holds the record, the helper's log and the plug-ins' lines in full")
        t.check(text.contains("Audio plug-in: version 0.3 (3), program there") && text.contains("MIDI plug-in: not there")
                    && text.contains("Old go-ahead file still there (allow-changes;") && !text.contains("allow-memory") && text.contains("macOS: "),
                "it names the versions and what is installed; of the old go-ahead files only the one that is still there")
        t.check(text.contains(SettingsBackup.blockLine) && text.contains("INPUT 1: volume") && text.contains("Recording margin 8.0 ms, playback delay 8.0 ms"),
                "it holds the unit's settings as read, in words and bytes, and the driver's settings")
        t.check(text.contains("2026-11-21 18.00.ua101"), "it lists the newest backup")
        t.check(text.contains("Not done"), "it holds this run's messages")
        if t.failed > 0, let dump = ProcessInfo.processInfo.environment["UA101_REPORT_DUMP"] { try? text.write(toFile: dump, atomically: true, encoding: .utf8) }
        t.check(text.range(of: "testperson", options: .caseInsensitive) == nil && !text.contains("Test Person")
                    && text.contains("~/Documents/UA-101 Backups") && text.contains("for (user) on (this Mac)"),
                "no user name, full name, Mac name or home folder in it")
        t.check(DeveloperReport.fileName(at: when).hasPrefix("UA-101 Report 20") && DeveloperReport.fileName(at: when).hasSuffix(".txt"), "its file name")
        // A long log: only its end, from the start of a line.
        write("long.log", (1...200).map { "line \($0) of the long log" }.joined(separator: "\n") + "\n")
        let end = DeveloperReport.tail(dir + "/long.log", limit: 100)
        t.check(end.hasPrefix("(… the first ") && end.contains("line 200 of the long log") && !end.contains("line 1 of"), "a long log: only its end")
        t.check(DeveloperReport.tail(dir + "/missing.log").contains("not there"), "a missing log is named as missing")
        try? fm.removeItem(atPath: dir)

        // The user's own Mac, by request (UA101_REPORT_TO=path): a real report from the real
        // files. Nothing is asked of the unit (the test never connects to it).
        if let path = ProcessInfo.processInfo.environment["UA101_REPORT_TO"] {
            let real = DeveloperReport.make(at: Date(), unit: DeveloperReport.UnitPart(text: "Not read: this report was made by the self-test, which never connects to the unit."),
                                            messages: "", from: DeveloperReport.Sources())
            t.check((try? real.write(toFile: path, atomically: true, encoding: .utf8)) != nil, "a report from this Mac written to \(path)")
        }
    }

    // ---- The message rules of 2026-11-23: a volume held at the top is normal use
    // (grey); and red pop-ups have only OK, as they come after the change was sent.
    if t.begin("pop-ups only for red, with OK only (2026-11-23)") {
        let in1 = strip(inp, 0)
        let m = Bench()
        m.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        m.chooseReal()
        t.check(m.popupsShown.isEmpty && m.popups.showing == nil, "the app opens with the UA-101: no pop-up")

        // Grey: a refusal while read-only, and a volume that went through.
        m.model.setVolumeExact(in1, 0x16A09E)
        t.check(m.messages.now.contains { $0.slot == .refusal && $0.level == .info } && m.popupsShown.isEmpty,
                "a grey \"Not done\" (read-only): only in the list, no pop-up")
        m.model.allowChanges(true)
        m.settle()
        let arrived = m.arrivals
        m.model.setVolumeExact(in1, 0x16A09E)
        m.settle()
        t.check(m.arrivals > arrived && m.messages.now.contains { $0.slot == .volume && $0.level == .info } && m.popupsShown.isEmpty && m.popups.showing == nil,
                "a grey volume message (sent, the unit took it): it arrives in the list, no pop-up, nothing opens")
        t.check(!MessagePopups.popsUp(m.messages.now.first { $0.slot == .volume }!), "grey never pops up")

        // A fader above 0 dB (since 2026-11-27): no refusal, no pop-up.
        t.equal(m.model.setVolume(strip(inp, 3), FaderScale.volume(dB: 6)), Int32(PANEL_OK), "+6 dB goes as it is")
        t.check(!m.messages.now.contains { $0.slot == .refusal } && m.popupsShown.isEmpty && m.popups.showing == nil,
                "nothing held, nothing in the list as a refusal, no pop-up")
        m.model.setVolume(strip(inp, 3), FaderScale.volume(dB: -10))
        m.settle()

        // Red: the unit keeps another value.
        m.fake.unit.ignore(kind: Int(PANEL_F_VOLUME))
        m.model.setVolumeExact(in1, 0x100001)
        m.settle()
        let reds = m.messages.now.filter { $0.level == .red }.count
        t.check(reds >= 1 && m.popupsShown.count == 1 && m.popups.showing?.level == .red,
                "red (the unit kept another value): it pops up, one at a time (\(reds) red in the list)")
        while m.popups.showing != nil { m.pressPopupOK() }
        m.settle()
        t.check(m.popupsShown.count == reds && !m.messages.now.contains { $0.level == .red }
                    && m.messages.earlier.first?.level == .red && m.messages.earlier.first?.okTime != nil,
                "OK on each: the next red comes up after it (\(m.popupsShown.count) in all), and each moves down into Earlier with the time")
        t.check(m.model.snap.writesAllowed && !m.model.changesLocked, "OK changes nothing else: changes stay on")
        m.fake.unit.ignore(kind: -1)

        // A red one OK'd in the Messages window before its turn doesn't come up.
        let q = Bench()
        q.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        q.chooseReal()
        q.model.allowChanges(true)
        q.settle()
        q.fake.unit.ignore(kind: Int(PANEL_F_VOLUME))
        q.model.setVolumeExact(in1, 0x100001)
        q.settle()
        let waiting = q.messages.now.filter { $0.level == .red && $0.id != q.popups.showing?.id }
        for w in waiting { q.messages.ok(w) }
        let shownBefore = q.popupsShown.count
        q.pressPopupOK()
        t.check(q.popups.showing == nil && q.popupsShown.count == shownBefore,
                "a red one OK'd in the Messages window while it waited: it doesn't come up (\(waiting.count) waited)")

        // The end-user app: the same.
        let u = Bench()
        u.model.changesByThemselves = true
        u.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        u.chooseReal()
        u.fake.unit.ignore(kind: Int(PANEL_F_VOLUME))
        u.model.setVolumeExact(in1, 0x100001)
        u.settle()
        t.check(u.popups.showing?.level == .red, "end-user app, red: it pops up")
        while u.popups.showing != nil { u.pressPopupOK() }
        u.settle()
        t.check(!u.model.changesLocked && u.model.snap.writesAllowed && UnitState.of(u.model).title != "Changes locked",
                "end-user app, OK: nothing locked, the line says \(UnitState.of(u.model).title)")

        // The pop-up's content draws.
        if let r = m.messages.earlier.first(where: { $0.level == .red }) {
            let h = NSHostingView(rootView: PopupView(m: r, ok: {}).themed(.dark)).fittingSize
            t.check(h.width == 420 && h.height > 100 && h.height < 600, "the pop-up draws, 420 points wide: \(h)")
        }
    }

    // ---- The end-user app's changes by themselves (a design choice of
    // 2026-11-21): on as soon as the UA-101 is read, with no go-ahead file since link version
    // 18 (a design choice of the same evening), and Lock Changes keeps them off.
    if t.begin("changes by themselves") {
        func bench(passes: UInt64 = FakeHelper.version17) -> (Bench, () -> [String]) {
            let b = Bench()
            var lines: [String] = []
            b.model.onRecord = { lines.append($0) }
            b.model.changesByThemselves = true
            b.fake.passes = passes
            b.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
            b.chooseReal()
            return (b, { lines })
        }
        let (on, onLines) = bench()
        t.check(on.model.snap.writesAllowed && on.model.snap.memoryAllowed, "changes are on by themselves once the UA-101 is read, Recall, Save and Reset with them, with no file anywhere")
        t.check(on.fake.passedOn == 1 && on.fake.changesPassedOn == 0, "nothing but the settings request went to the unit for it")
        t.check(onLines().contains { $0.hasPrefix("Changes switched on by themselves") }, "the record says so")
        t.check(on.model.refusal == nil, "no refusal shown")
        var st = UnitState.of(on.model)
        t.check(st.title == "Ready" && st.actionLabel == "Lock Changes", "the state: Ready, with Lock Changes (\(st.title))")
        let light = StatusLight.of(on.model, .dark)
        t.check(light.label == "Ready" && light.lit, "the light: Ready")
        on.model.setVolume(strip(inp, 0), minus6)
        on.settle(8_000_000)
        t.check(unitValue(on, Int32(PANEL_F_VOLUME), inp, 0, real: true) == minus6, "a volume change goes through")

        on.model.lockChanges(true)
        on.settle(70_000_000)
        t.check(!on.model.snap.writesAllowed && on.model.changesLocked, "Lock Changes: off, and still off after 70 s")
        st = UnitState.of(on.model)
        t.check(st.title == "Changes locked" && st.actionLabel == "Unlock", "the state: Changes locked, with Unlock")
        t.check(StatusLight.of(on.model, .dark).label == "Changes locked", "the light: Changes locked")
        on.model.setVolume(strip(inp, 0), FaderScale.volume(dB: -12))
        on.settle()
        t.check(on.model.refusal?.text.contains("Lock Changes") == true, "a change while locked says how to unlock (\(on.model.refusal?.text ?? "none"))")
        on.model.lockChanges(false)
        on.settle()
        t.check(on.model.snap.writesAllowed && !on.model.changesLocked, "unlock: on again at once")

        // The helper refuses a change (a test's refusal): changes go off, the refusal shows,
        // and they come back on by themselves, so the next change goes through.
        on.fake.refuseChanges = "a refusal made by the test; the message was not sent"
        on.model.setVolume(strip(inp, 0), FaderScale.volume(dB: -12))
        on.settle(8_000_000)
        t.check(on.fake.refused >= 1 && on.model.refusal?.kind == .helper, "a refusal by the helper is shown (\(on.model.refusal?.text ?? "none"))")
        on.fake.refuseChanges = nil
        on.settle()
        t.check(on.model.snap.writesAllowed && !on.model.changesLocked, "and changes are on again by themselves")
        t.check(onLines().filter { $0.hasPrefix("Changes switched on by themselves") }.count >= 2, "the record has both times")

        // A helper that passes on nothing at all (none such was ever built): look only, said quietly.
        let (off, offLines) = bench(passes: 0)
        t.check(off.model.snap.state == .ready && !off.model.snap.writesAllowed, "a helper that passes on nothing: read, and off")
        t.check(off.model.refusal == nil, "quiet: no refusal shown for it")
        st = UnitState.of(off.model)
        t.check(st.title == "Look only" && st.short.contains("passes on none") && st.actionLabel == nil && !st.long.contains("sudo") && !st.long.contains("go-ahead"),
                "the state: Look only, with the reason and nothing to type (\(st.short))")
        t.check(StatusLight.of(off.model, .dark).label == "Look only", "the light: Look only")
        let sent = off.fake.passedOn
        off.settle(300_000_000)
        t.check(off.fake.passedOn == sent && !off.model.snap.writesAllowed, "five minutes on: still off, and nothing was asked of the helper or the unit for it")
        t.check(offLines().filter { $0.hasPrefix("Changes") }.isEmpty, "the record has no line about changes")

        // The developer build's way is unchanged: its Allow Changes switch, off at the start.
        let dev = Bench()
        dev.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        dev.chooseReal()
        dev.settle(40_000_000)
        t.check(!dev.model.snap.writesAllowed && UnitState.of(dev.model).actionLabel == "Allow changes", "the developer build: still Allow changes")

        // The pretend unit is never switched on by itself.
        let pretend = Bench()
        pretend.model.changesByThemselves = true
        pretend.settle(40_000_000)
        t.check(!pretend.model.snap.writesAllowed, "the pretend unit stays read-only")
    }

    // The user manual: the Help menu, the PDF's copy, the help book's names, and
    // the pages against the app's own names. The pages are read from the project, as the
    // other source checks here are; manual/build.py checks every menu name again when it builds.
    if t.begin("manual") {
        t.check(Manual.helpItems == ["UA-101 Control Help", "Save Manual as PDF…", "Save Report for Developer…"],
                "the Help menu: the help first, then Save Manual as PDF…, then the report")
        let here = (#filePath as NSString).deletingLastPathComponent
        let root = (here as NSString).deletingLastPathComponent
        func source(_ path: String) -> String { (try? String(contentsOfFile: (root as NSString).appendingPathComponent(path), encoding: .utf8)) ?? "" }
        let app = source("app/PanelApp.swift")
        if let r = app.range(of: "CommandGroup(replacing: .help) {"), let end = app.range(of: "\n        }", range: r.upperBound..<app.endIndex) {
            let block = String(app[r.upperBound..<end.lowerBound])
            let titles = block.components(separatedBy: "Button(\"").dropFirst().map { String($0.prefix { $0 != "\"" }) }
            t.equal(titles, Manual.helpItems, "the Help menu's buttons are in that order in the app")
        } else {
            t.check(app.isEmpty, "the Help menu is found in the app's source")
        }
        let plist = NSDictionary(contentsOfFile: (root as NSString).appendingPathComponent("app/Info.plist"))
        t.check(plist == nil || (plist?["CFBundleHelpBookFolder"] as? String == Manual.helpFolder
                                 && plist?["CFBundleHelpBookName"] as? String == Manual.helpBookName),
                "Info.plist names the help book the app opens")
        let builder = source("manual/build.py")
        t.check(builder.isEmpty || (builder.contains("BOOK = \"\(Manual.helpFolder)\"") && builder.contains("BOOK_ID = \"\(Manual.helpBookName)\"")),
                "manual/build.py makes the help book under the same names")
        t.check(source("Makefile").isEmpty || source("Makefile").contains("$(MANUAL)/UA-101 Control Manual.pdf\" \"$(USER_BUNDLE)/Contents/Resources/"),
                "make user-app puts the PDF in the app as \(Manual.pdfName)")

        // Saving a copy: new, over an older file, and the two ways it can't.
        let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("ua101panel-manual-\(getpid())", isDirectory: true)
        try? FileManager.default.removeItem(at: dir)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let pdf = dir.appendingPathComponent("in.pdf"), copy = dir.appendingPathComponent(Manual.pdfName)
        let bytes = Data((0..<5000).map { UInt8($0 % 251) })
        try? bytes.write(to: pdf)
        t.check(Manual.copy(pdf, to: copy) == nil && (try? Data(contentsOf: copy)) == bytes, "the copy is saved, byte for byte")
        try? Data("an older copy".utf8).write(to: copy)
        t.check(Manual.copy(pdf, to: copy) == nil && (try? Data(contentsOf: copy)) == bytes, "a file of that name is replaced")
        let left = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        t.equal(left.sorted(), [Manual.pdfName, "in.pdf"].sorted(), "and nothing half-written is left beside it")
        t.check(Manual.copy(nil, to: copy)?.contains("not inside this copy of the app") == true, "no PDF in the app: it says so")
        let nowhere = dir.appendingPathComponent("no such folder/\(Manual.pdfName)")
        t.check(Manual.copy(pdf, to: nowhere)?.hasPrefix("It could not be written there") == true
                    && !FileManager.default.fileExists(atPath: nowhere.path), "a folder that isn't there: it says why, and writes nothing")

        // The PDF and the help book are found in an app's Resources, and only there.
        let bundleURL = dir.appendingPathComponent("Test.app", isDirectory: true)
        let resources = bundleURL.appendingPathComponent("Contents/Resources", isDirectory: true)
        try? FileManager.default.createDirectory(at: resources, withIntermediateDirectories: true)
        try? Data("<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>CFBundleIdentifier</key><string>test.ua101.manual</string></dict></plist>".utf8)
            .write(to: bundleURL.appendingPathComponent("Contents/Info.plist"))
        if let bundle = Bundle(url: bundleURL) {
            t.check(Manual.bundledPDF(in: bundle) == nil && !Manual.hasHelpBook(in: bundle), "an app without the manual: neither found")
            try? bytes.write(to: resources.appendingPathComponent(Manual.pdfName))
            let book = resources.appendingPathComponent(Manual.helpFolder + "/Contents", isDirectory: true)
            try? FileManager.default.createDirectory(at: book, withIntermediateDirectories: true)
            try? Data("x".utf8).write(to: book.appendingPathComponent("Info.plist"))
            t.check(Manual.bundledPDF(in: bundle)?.lastPathComponent == Manual.pdfName && Manual.hasHelpBook(in: bundle), "with them: both found")
        } else {
            t.check(false, "a test app could be made")
        }
        try? FileManager.default.removeItem(at: dir)

        // The pages: every name the user sees on the Help menu, the Device menu's headings, the
        // mixer's buttons, the View menu's sizes and looks; none of the old names.
        let pagesDir = (root as NSString).appendingPathComponent("manual/pages")
        let files = ((try? FileManager.default.contentsOfDirectory(atPath: pagesDir)) ?? []).filter { $0.hasSuffix(".html") }.sorted()
        let text = files.map { (try? String(contentsOfFile: (pagesDir as NSString).appendingPathComponent($0), encoding: .utf8)) ?? "" }.joined(separator: "\n")
        // 19 since 2026-11-23: "Use the UA-101 with your Mac and music apps" and "Install, update and remove";
        // 20 since 1.1 (2026-11-27): "What's new in 1.1".
        t.check(files.isEmpty || files.count == 20, "20 pages, one task each (\(files.count))")
        t.check(files.isEmpty || (files.contains("01b-whats-new.html") && text.contains("What's new in 1.1")
                    && text.contains("playback 8 ms") && text.contains("recording 8 ms")),
                "the page added for 1.1: what's new, with the new defaults")
        t.check(files.isEmpty || (files.contains("02b-use-with-mac-and-apps.html") && files.contains("16b-install-update-remove.html")
                    && text.contains("Problems and questions") && text.contains("uninstall.sh")),
                "the pages added 2026-11-23: the Mac and music apps, Problems and questions, Install, update and remove")
        let names = Manual.helpItems + [PanelCommands.memoriesHeading, PanelCommands.backupsHeading] + MixerWindowView.lineButtons
            + WindowFit.fixedSizes.map(\.title) + Look.allCases.map(\.title)
            + [Uninstall.menuTitle, Uninstall.keepTitle, Uninstall.everythingTitle, Uninstall.keepFilesTitle]
            + ["Recall Memory…", "Save Memory…", "Reset to Factory…", "Save Backup…", "Open Backup…", "Copy Backup to Memory…", "Lock Changes", "Reset Mixer…"]
        let missing = names.filter { !text.isEmpty && !text.contains($0) }
        t.check(missing.isEmpty, "the manual names every one of them" + (missing.isEmpty ? "" : ", not: " + missing.joined(separator: ", ")))
        let old = ["Load Memory", "Store Memory", "Restore Backup", "Allow Load", "Allow Changes", "go-ahead", "sudo"]
        t.check(!old.contains { text.contains($0) }, "and none of the old names")
        t.check(text.isEmpty || ["The settings in use", "The five memories in the UA-101", "Backup files on this Mac"].allSatisfy { text.contains($0) },
                "it explains the three places settings live")
        let picturesDir = (root as NSString).appendingPathComponent("manual/pictures")
        let crops = ["light-ready.png", "light-locked.png", "light-none.png", "bottom-bar.png", "top-buttons.png", "strip.png"]
        let used = text.components(separatedBy: "src=\"pictures/").dropFirst().map { String($0.prefix { $0 != "\"" }) }
        let absent = used.filter { !crops.contains($0) && !FileManager.default.fileExists(atPath: (picturesDir as NSString).appendingPathComponent($0)) }
        t.check(absent.isEmpty && (text.isEmpty || used.count >= 20), "every picture it shows is one of the end-user pictures (\(used.count))" + (absent.isEmpty ? "" : ", missing: " + absent.joined(separator: ", ")))
    }

    // The notes pad (1.1, 2026-11-27): kept in a plain text file, saved when asked, the folder
    // made when missing, a failing save told once; the one-key shortcuts off while typing.
    if t.begin("notes pad") {
        let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("ua101panel-notes-\(getpid())", isDirectory: true)
        try? FileManager.default.removeItem(at: dir)
        let url = dir.appendingPathComponent("UA-101 Backups", isDirectory: true).appendingPathComponent(NotesStore.fileName)
        let a = NotesStore(url: url)
        t.equal(a.text, "", "no file yet: empty")
        t.check(!a.hasUnsaved, "nothing to save at first")
        a.text = "Input 1-2: the desk microphone\nOutput 3-4: the speakers — ünïcode"
        t.check(a.hasUnsaved, "a change waits to be saved")
        t.check(a.saveNow() && !a.hasUnsaved && a.lastError == nil, "saved, the folder made")
        let b = NotesStore(url: url)
        t.equal(b.text, a.text, "read back the same")
        var told = 0
        // A file where the folder should be: every save fails, told once only.
        let blocked = dir.appendingPathComponent("blocked")
        FileManager.default.createFile(atPath: blocked.path, contents: Data())
        let c = NotesStore(url: blocked.appendingPathComponent(NotesStore.fileName))
        c.onError = { _ in told += 1 }
        c.text = "x"
        t.check(!c.saveNow() && c.lastError != nil, "a save that can't be made says so")
        c.text = "xy"
        _ = c.saveNow()
        t.equal(told, 1, "told once, not on every key")
        t.equal(c.text, "xy", "the words stay in the window")
        let ui = PanelUI()
        let w = NotesWindow(store: b, ui: ui)
        w.windowDidBecomeKey(Notification(name: NSWindow.didBecomeKeyNotification))
        t.check(ui.typingNotes, "with the keyboard: the one-key shortcuts are off")
        b.text += "\nMain: the monitors"
        w.windowDidResignKey(Notification(name: NSWindow.didResignKeyNotification))
        t.check(!ui.typingNotes && !b.hasUnsaved, "without it: on again, and the words saved")
        t.equal(NotesStore(url: url).text, b.text, "saved on leaving the window")
        try? FileManager.default.removeItem(at: dir)
    }

    // Uninstall UA-101… (2026-11-24): what the app asks the script for, how its
    // answer is read, the grey MIDI rule, and the script itself (its dry runs and refusals only:
    // nothing is removed and no password is asked here).
    if t.begin("uninstall") {
        t.equal(Uninstall.arguments(.keepSettings, uid: 501), ["--keep-settings", "--asked-by=501", "--from-app"], "keep settings")
        t.equal(Uninstall.arguments(.everything(keepMyFiles: true), uid: 502), ["--everything", "--asked-by=502", "--from-app"],
                "everything, own files kept (the tick as delivered)")
        t.equal(Uninstall.arguments(.everything(keepMyFiles: false), uid: 501), ["--everything", "--with-my-files", "--asked-by=501", "--from-app"],
                "everything, own files too")
        let s = Uninstall.appleScript(.keepSettings, uid: 501)
        t.check(s.hasPrefix("do shell script \"'/bin/sh' '/Library/Application Support/ua101-driver/uninstall.sh' '--keep-settings'")
                    && s.hasSuffix("with administrator privileges without altering line endings")
                    && s.contains("with prompt \"\(Uninstall.prompt)\""), "the AppleScript: the script, macOS's password window, its words")
        let odd = Uninstall.appleScript(.keepSettings, uid: 501, script: "/tmp/a 'b\" c\\d.sh")
        t.check(odd.contains("'/tmp/a '\\\\''b\\\" c\\\\d.sh'"), "a path with a quote, a double quote and a backslash stays one word")
        t.equal(Uninstall.shellQuoted("it's"), "'it'\\''s'", "shell quoting")
        t.equal(Uninstall.outcome(stdout: "", stderr: "0:12: execution error: User canceled. (-128)\n", exitStatus: 1), .cancelled,
                "Cancel in the password window: nothing ran")
        t.equal(Uninstall.outcome(stdout: "", stderr: "", exitStatus: 1), .notRun("osascript stopped with status 1"), "osascript failed without words")
        t.equal(Uninstall.outcome(stdout: "step\n== What happened ==\nDone.\n\(Uninstall.statusPrefix)0\n", stderr: "", exitStatus: 0),
                .ran(output: "step\n== What happened ==\nDone.", status: 0), "the script ran: its words and its status")
        t.equal(Uninstall.outcome(stdout: "x\n\(Uninstall.statusPrefix)1", stderr: "", exitStatus: 0), .ran(output: "x", status: 1), "status 1: something stayed")
        t.equal(Uninstall.outcome(stdout: "no status line", stderr: "", exitStatus: 0), .notRun("the uninstaller's answer wasn't understood"),
                "no status line: not taken as done")
        t.equal(Uninstall.summary(of: "  rm -rf x\n\n== What happened ==\nAll gone.\nKept: y\n"), "All gone.\nKept: y", "the window shows the short account")
        t.equal(Uninstall.summary(of: "Stopped: z\n"), "Stopped: z", "no heading: all of it")
        // The user's own files (2026-11-27): moved to the Trash by the app, put back if the script didn't run.
        t.equal(Uninstall.MyFiles.folders(home: "/Users/x").map(\.path), ["/Users/x/Documents/UA-101 Backups", "/Users/x/Documents/UA-101 Reports"],
                "my files: this account's two folders in Documents")
        t.check(Uninstall.MyFiles.folders().map(\.path).first == AppCore.backupFolder.path, "my files: the folder the backups are written to")
        do {
            let fm = FileManager.default
            let dir = NSTemporaryDirectory() + "ua101-myfiles-\(getpid())"
            try? fm.removeItem(atPath: dir)
            try? fm.createDirectory(atPath: dir + "/Documents/UA-101 Backups", withIntermediateDirectories: true)
            try? fm.createDirectory(atPath: dir + "/Trash", withIntermediateDirectories: true)
            fm.createFile(atPath: dir + "/Documents/UA-101 Backups/a.ua101", contents: Data("x".utf8))
            let from = URL(fileURLWithPath: dir + "/Documents/UA-101 Backups"), to = URL(fileURLWithPath: dir + "/Trash/UA-101 Backups")
            try? fm.moveItem(at: from, to: to) // as the Trash would hold it
            let m = Uninstall.MyFiles.Moved(from: from, to: to)
            t.check(Uninstall.MyFiles.putBack([m]).isEmpty && fm.fileExists(atPath: from.path + "/a.ua101") && !fm.fileExists(atPath: to.path),
                    "my files: the script didn't run, so the folder is put back, with what's in it")
            t.equal(Uninstall.MyFiles.putBack([m]), [m], "my files: one that can't go back (its place is taken again) stays in the Trash, and is named")
            t.equal(Uninstall.MyFiles.moveToTrash([URL(fileURLWithPath: dir + "/Documents/UA-101 Reports")]), [], "my files: a folder that isn't there: nothing moved")
            try? fm.removeItem(atPath: dir)
            t.check(Uninstall.MyFiles.line([]) == nil && Uninstall.MyFiles.stillInTrash([]) == nil
                        && Uninstall.MyFiles.line([m])?.contains("Moved to the Trash") == true && Uninstall.MyFiles.line([m])?.contains(from.path) == true
                        && Uninstall.MyFiles.stillInTrash([m])?.contains("Put Back") == true,
                    "my files: the window says what went to the Trash, or what is still there")
        }

        let ua = Uninstall.midiDriverOwner, other = "com.apple.AppleMIDIUSBDriver"
        t.equal(GreyMIDI.decide([]), .none, "no MIDI devices: nothing")
        t.equal(GreyMIDI.decide([.init(owner: other, offline: true)]), .none, "another driver's grey device: never touched")
        t.equal(GreyMIDI.decide([.init(owner: other, offline: true), .init(owner: ua, offline: false)]), .wait,
                "the UA-101 still online (the MIDI server not caught up): wait, remove nothing")
        t.equal(GreyMIDI.decide([.init(owner: other, offline: true), .init(owner: ua, offline: true), .init(owner: ua, offline: true)]), .remove([1, 2]),
                "the UA-101 offline: only its own entries")
        t.check(GreyMIDI.line(.removed(1)) != nil && GreyMIDI.line(.noneFound) == nil
                    && GreyMIDI.line(.stillOnline)?.contains("Show MIDI Studio") == true && GreyMIDI.line(.failed(-50)) == GreyMIDI.line(.stillOnline),
                "the line at the end: gone, nothing, or the one click by hand")

        // Alerts fit between the menu bar and the Dock (2026-11-27: the last window's button was
        // below the screen): long words scroll, the buttons stay in the room.
        do {
            let room = NSRect(x: 0, y: 60, width: 1200, height: 600)
            let long = NSAlert()
            long.messageText = "The UA-101 is uninstalled, but not everything went"
            long.informativeText = (1...300).map { "rm -rf /a/long/path/number/\($0)" }.joined(separator: "\n")
            long.addButton(withTitle: "Quit")
            long.prepareToFit(room: room)
            t.check(long.accessoryView is NSScrollView && long.informativeText.isEmpty && long.window.frame.height <= room.height,
                    "a long alert: its words scroll, and all of it (\(Int(long.window.frame.height)) points high) fits a room 600 high")
            let short = NSAlert()
            short.messageText = "The notes could not be saved"
            short.informativeText = "Try again."
            short.prepareToFit(room: room)
            t.check(short.accessoryView == nil && short.informativeText == "Try again.", "a short alert: as it was")
            let kept = DialogWindow.keep(NSRect(x: 100, y: -300, width: 400, height: 500), in: room)
            t.check(room.contains(kept), "an alert placed partly below the room: moved inside it")
        }
        let here = (#filePath as NSString).deletingLastPathComponent
        let root = (here as NSString).deletingLastPathComponent
        func source(_ path: String) -> String { (try? String(contentsOfFile: (root as NSString).appendingPathComponent(path), encoding: .utf8)) ?? "" }
        let app = source("app/PanelApp.swift"), script = source("install/uninstall.sh")
        t.check(app.isEmpty || app.contains("CommandGroup(after: .appSettings) {\n            Divider()\n            Button(\"\(Uninstall.menuTitle)\") { AppCore.shared.uninstall() }"),
                "the app menu has \(Uninstall.menuTitle)")
        t.check(app.isEmpty || (app.contains("guard !recordStopped else { return }") && app.contains("if !self.uninstalling { self.popups.arrived(m) }")),
                "while uninstalling: no record written with everything, no red pop-ups")
        func plistID(_ path: String) -> String? {
            NSDictionary(contentsOfFile: (root as NSString).appendingPathComponent(path))?["CFBundleIdentifier"] as? String
        }
        if !script.isEmpty {
            for a in ["--keep-settings)", "--everything)", "--with-my-files)", "--asked-by=*)", "--from-app)", "--dry-run)", "say \"\(Uninstall.summaryMarker)\""] {
                t.check(script.contains(a), "the script knows \(a)")
            }
            t.check(script.contains("HELPER_DIR=\"\((Uninstall.scriptPath as NSString).deletingLastPathComponent)\""), "the app looks for the script where the package puts it")
            t.check(source("install/pkg/build-pkg.sh").contains("\"$INSTALL_DIR/uninstall.sh\""), "and the package carries it")
            t.check(plistID("app/Info.plist").map { script.contains("APP_ID=\($0)\n") } ?? false, "the script knows the app's bundle id")
            t.check(plistID("src/plugin/Info.plist").map { script.contains("Plug-In.\($0)\n") } ?? false, "the Driver Settings: Core Audio's entry under the plug-in's bundle id")
            // The device's UID, as src/ua101_drvset.h gives it to the plug-in.
            let uidLine = source("src/ua101_drvset.h").components(separatedBy: "\n").first { $0.hasPrefix("#define UA101_DEVICE_UID ") } ?? ""
            let deviceUID = uidLine.components(separatedBy: "\"").dropFirst().first ?? "?"
            t.check(script.contains("device.\(deviceUID)\"") && script.contains("CA_DEVICES_KEYS=\"\(deviceUID)\""),
                    "what Core Audio remembers of the device: under the device's UID")
            t.equal(plistID("src/midiplugin/Info.plist"), Uninstall.midiDriverOwner, "the grey MIDI device: owned by the MIDI plug-in's bundle id")
            t.check(script.contains("Documents/UA-101 Backups") && AppCore.backupFolder.path.hasSuffix("Documents/UA-101 Backups")
                        && script.contains("Documents/UA-101 Reports") && source("app/Report.swift").contains("/Documents/UA-101 Reports"),
                    "the user's own files: the folders the app writes them to")
            t.check(!script.contains("com.apple.MIDI") && !script.contains("ByHost"), "macOS's MIDI settings files: never named, never touched")
            let unfitted = ["app/Manual.swift", "app/PanelApp.swift", "app/Report.swift", "app/Uninstall.swift", "app/Notes.swift", "app/Messages.swift",
                            "app/DialogWindows.swift", "app/Backup.swift", "app/DriverSettings.swift"]
                .flatMap { source($0).components(separatedBy: "\n") }
                .filter { $0.contains(".runModal()") && !$0.contains("panel.runModal()") }
            t.check(unfitted.isEmpty, "every alert of the app is fitted to the room above the Dock\(unfitted.isEmpty ? "" : ": " + unfitted.joined(separator: " | "))")
            // 2026-11-27: what macOS's privacy protection refuses stays, is listed, and doesn't stop the rest.
            t.check(script.contains("HV_CACHES=\"Library/Group Containers/group.com.apple.helpviewer.content/Library/Caches\"")
                        && script.contains("HV_NAME=\"$APP_ID.$APP_ID.help\"")
                        && (NSDictionary(contentsOfFile: (root as NSString).appendingPathComponent("app/Info.plist"))?["CFBundleHelpBookName"] as? String)
                            == plistID("app/Info.plist").map { $0 + ".help" },
                    "Help Viewer's copy of the help: in its container, under the app's id and the help book's name")
            t.check(script.contains("say \"    kept (macOS refused): $1\"") && script.contains("privacy protection didn't let the uninstaller remove them")
                        && script.contains("trash_in_home \"$home\" \"$uid\" \"$home/$d\"") && !script.contains("rm -rf \"$home/$d\"") && !script.contains("step rm -rf \"$home/"),
                    "in the accounts: a refusal is kept and listed, the script goes on, and your own files go to the Trash, never deleted for good")

            // The script itself: refusals and dry runs only.
            func runScript(_ args: [String]) -> (Int32, String) {
                let p = Process()
                p.executableURL = URL(fileURLWithPath: "/bin/sh")
                p.arguments = [(root as NSString).appendingPathComponent("install/uninstall.sh")] + args
                let out = Pipe()
                p.standardOutput = out
                p.standardError = out
                p.standardInput = FileHandle.nullDevice
                guard (try? p.run()) != nil else { return (-1, "") }
                let d = out.fileHandleForReading.readDataToEndOfFile()
                p.waitUntilExit()
                return (p.terminationStatus, String(decoding: d, as: UTF8.self))
            }
            t.equal(runScript(["--with-my-files"]).0, 2, "--with-my-files without --everything: refused")
            t.equal(runScript(["--keep-settings", "--everything"]).0, 2, "both ways at once: refused")
            t.equal(runScript(["--now"]).0, 2, "a word it doesn't know: refused")
            let notRoot = runScript(["--everything", "--asked-by=501"])
            t.check(notRoot.0 == 1 && notRoot.1.contains("for the app"), "--asked-by without the administrator's rights: refused, nothing done")
            let noAnswer = runScript([])
            t.check(noAnswer.0 == 1 && noAnswer.1.contains("Nothing was changed"), "no answer to the question: stops, nothing changed")
            for (args, words) in [(["--keep-settings", "--dry-run"], "remove the driver, keep the settings"),
                                  (["--everything", "--dry-run"], "remove everything, your own files stay"),
                                  (["--everything", "--with-my-files", "--dry-run"], "remove everything, your own files to the Trash")] {
                let (status, out) = runScript(args)
                t.check(status == 0 && out.contains("Dry run (\(words))") && out.hasSuffix("Nothing was changed.\n")
                            && !out.components(separatedBy: "\n").contains { $0.hasPrefix("  sudo") },
                        "dry run, \(words): steps shown, nothing changed")
                if args.contains("--everything") {
                    t.check(out.contains("group.com.apple.helpviewer.content"), "dry run, \(words): Help Viewer's copy named, not looked into")
                }
            }
        }
    }

    if let only = t.only {
        // Not the whole test, and never to be read as one. No section by that name: a failure.
        print("ua101panel self-test, ONLY \(t.sectionsRun) of \(t.sectionsRun + t.sectionsSkipped) sections (the ones named “\(only)”): \(t.passed) checks passed, \(t.failed) failed")
        return t.failed == 0 && t.sectionsRun > 0 ? 0 : 1
    }
    print("ua101panel self-test: \(t.passed) checks passed, \(t.failed) failed")
    return t.failed == 0 ? 0 : 1
}

private func panel_guard_blocksCount(_ b: Bench) -> UInt64 { b.model.guardBlocks }

/// How many pixels of a picture are red to amber: a strong colour with a hue from red to
/// amber (theme.bad and theme.warn in both looks, and their smoothed edges). -1 if unreadable.
/// `above`: only the top part of the picture, that share of its height.
private func redOrAmberPixels(in path: String, above share: Double = 1) -> Int {
    guard let rep = NSImage(contentsOfFile: path)?.representations.first as? NSBitmapImageRep else { return -1 }
    var n = 0
    for y in 0..<Int(Double(rep.pixelsHigh) * share) { autoreleasepool {
        for x in 0..<rep.pixelsWide {
            guard let c = rep.colorAt(x: x, y: y)?.usingColorSpace(.sRGB), c.alphaComponent > 0.5 else { continue }
            var h: CGFloat = 0, sat: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
            c.getHue(&h, saturation: &sat, brightness: &b, alpha: &a)
            if sat > 0.45 && b > 0.4 && (h < 50.0 / 360 || h > 345.0 / 360) { n += 1 }
        }
    } }
    return n
}
