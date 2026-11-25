// A stand-in for the installed helper, for the self-test and the pictures:
// it answers as ua101d does (src/ua101_service.c), with a pretend UA-101
// behind it in place of the real one. Like the real helper it passes on the
// request for the settings always; a pause and a resume of the unit's saving,
// the patch bay source of an output pair, and the volume (a value at most
// 0 dB), the pan (0x0000 to 0x8000) and the on/off bit (0 or 1) of the mixer's
// channels, the stereo link (0 or 1) of its pairs and the solo (0 to 10) of
// the INPUT and the OUTPUT group
// (link version 14: every channel, every pair, both groups; MONITOR OUT has no
// pan and no solo); and refuses every other message, and any message past 20
// in one second. As link version 17 it also passes on the Load, Store and
// Reset commands it names, by the real helper's rules: no sound streaming,
// none within 5 seconds of the one before, and only after the unit's settings
// were read in the 30 seconds before, with no change since. As link version 18
// it looks for no go-ahead file: there is none.
//
// The real helper's own code is tested against the app's real link in
// tests/test_ctlhelper.c. This one lets the model and the windows be driven
// with time moved by hand.
import Foundation

final class FakeHelper: HelperLink {
    var onAttached: ((Int32, Bool, Bool, String) -> Void)?
    var onMessage: (([UInt8]) -> Void)?
    var onChanged: (() -> Void)?
    var onAudio: ((Bool) -> Void)?
    var onLevels: ((HelperLevels) -> Void)?
    var onRefused: ((String) -> Void)?
    var onPasses: ((UInt64, UInt64, UInt64) -> Void)?
    var onMIDI: ((Bool, Bool) -> Void)?

    /// The Load, Store and Reset commands this helper says it passes on (PANEL_PASS_MEM_ bits).
    var passesMemory = FakeHelper.version17Memory
    /// As the real helper (src/ua101_link.h): a Load, Store or Reset is passed on only if the
    /// settings were read at most this long before, and none was within the second time.
    static let memoryFreshMicros = UInt64(panel_memory_fresh_seconds()) * 1_000_000
    static let memoryGapMicros = UInt64(panel_memory_gap_seconds()) * 1_000_000
    /// When the unit's whole settings block last reached the app, and its place among the
    /// events here (the pretend clock can stand still between two of them); the same for the
    /// last change and the last Load, Store or Reset passed on. nil: never.
    private var blockAt: UInt64?, memoryAt: UInt64?
    private var events = 0, blockEvent = 0, changeEvent = 0, memoryEvent = 0
    /// Every Load, Store or Reset passed on, in order, as sent; and the settings block the
    /// helper held (and would have written to its log) just before each.
    private(set) var memoryCommands: [[UInt8]] = []
    private(set) var memoryRecords: [[UInt8]] = []
    private var lastBlock: [UInt8] = []

    /// What this helper says it passes on. As link version 17: the saving,
    /// the five patch bay sources, every channel's volume, pan and on/off, every pair's link
    /// and both groups' solo (`passesOn` names the channels of the on/off, the pairs and the
    /// groups). A test may make it an older or a later helper.
    var passes = FakeHelper.version17
    var passesOn = FakeHelper.version14On
    static let version5 = UInt64(PANEL_PASS_SAVING)
    static let version6 = UInt64(PANEL_PASS_SAVING | PANEL_PASS_PATCH | PANEL_PASS_PATCH_OUTPUT_0)
    static let version8 = UInt64(PANEL_PASS_SAVING | PANEL_PASS_PATCH | PANEL_PASS_VOLUME)
        | (0x1F * UInt64(PANEL_PASS_PATCH_OUTPUT_0)) | (UInt64(PANEL_PASS_VOLUME_CHANNEL_0) << 2)
    static let version9 = version8 | UInt64(PANEL_PASS_PAN) | (UInt64(PANEL_PASS_PAN_CHANNEL_0) << 2)
    static let version11 = version9 | UInt64(PANEL_PASS_ON)
    static let version11On = PANEL_PASS_ON_CHANNEL_0 << 2
    /// Link version 13: all 22 channels' volume and on/off (bits 0 to 21), and the pan of the
    /// 20 that have one (bits 2 to 21). The self-test compares these with the real helper's
    /// own numbers (`panel_helper_passes`).
    static let version13 = UInt64(PANEL_PASS_SAVING | PANEL_PASS_PATCH | PANEL_PASS_VOLUME | PANEL_PASS_PAN | PANEL_PASS_ON)
        | (0x1F * UInt64(PANEL_PASS_PATCH_OUTPUT_0)) | (0x3FFFFF * UInt64(PANEL_PASS_VOLUME_CHANNEL_0))
        | (0x3FFFFC * PANEL_PASS_PAN_CHANNEL_0)
    static let version13On = 0x3FFFFF * PANEL_PASS_ON_CHANNEL_0
    /// Link version 14: also the stereo link of all 11 pairs and the solo of the INPUT and
    /// the OUTPUT group, named in the higher bits of the second number.
    static let version14 = version13 | UInt64(PANEL_PASS_LINK | PANEL_PASS_SOLO)
    static let version14On = version13On | (0x7FF * PANEL_PASS_LINK_PAIR_0) | (0x3 * PANEL_PASS_SOLO_GROUP_1)
    /// Link version 17: also Load, Store and Reset, named in a third number: Load and Store of
    /// each of the five memories, Reset of each, of the settings in use, and of everything.
    static let version17 = version14 | UInt64(PANEL_PASS_MEMORY)
    static let version17Memory = (0x1F * PANEL_PASS_MEM_LOAD_0) | (0x1F * PANEL_PASS_MEM_STORE_0)
        | (0x1F * PANEL_PASS_MEM_RESET_0) | PANEL_PASS_MEM_RESET_CURRENT | PANEL_PASS_MEM_RESET_ALL
    /// For a test: from now on this helper refuses every message that changes the unit, with
    /// this reason, as the real one refuses what is against its rules. nil: as usual.
    var refuseChanges: String?
    /// The same for Load, Store and Reset only: ordinary changes pass as usual.
    var refuseMemory: String?
    /// As the real helper: at most this many messages passed on in one second.
    static let sendsPerSecond = 20
    private var second: UInt64 = 0, sendsNow = 0
    /// Messages refused because they were more than `sendsPerSecond`.
    private(set) var tooMany = 0
    /// Every message that changes the unit and was passed on, in order, as sent.
    private(set) var changes: [[UInt8]] = []

    /// The unit behind the helper. Its switches can be pressed like the real one's.
    let unit: PretendUnit
    private let scheduler: PanelScheduler

    // How things stand on this pretend Mac.
    var installed = true      // false: no helper, or an older one that turns the app away
    var unitPresent = true    // plugged in and switched on
    var interfaceBusy = false // another program holds the unit's MIDI interface
    var asleep = false
    private(set) var streaming = false
    var levels = HelperLevels()
    /// As the real helper in the first 0.2 s after audio starts: it says "streaming", but
    /// answers a levels request with "nothing yet".
    var notRunningYet = false

    private(set) var attached = false
    private(set) var attaches = 0, detaches = 0, levelAsks = 0
    /// Messages passed on to the unit, and messages refused.
    private(set) var passedOn = 0, refused = 0
    /// Of those passed on: the ones that change the unit (saving, patch bay, volume, pan, on/off, link, solo).
    private(set) var changesPassedOn = 0

    init(scheduler: PanelScheduler) {
        self.scheduler = scheduler
        unit = PretendUnit(scheduler: scheduler)
        unit.onMessage = { [weak self] bytes in
            guard let self = self, self.attached else { return }
            // As the real helper: the whole settings block, when it reaches the app, is kept
            // with its time; a switch that moved on the unit makes the one kept an old one.
            if bytes.count == 268 {
                self.lastBlock = Array(bytes[10..<266])
                self.blockAt = self.scheduler.now
                self.events += 1
                self.blockEvent = self.events
            } else if bytes.count == 13, bytes[7] == 0x01 {
                self.blockAt = nil
            }
            self.onMessage?(bytes)
        }
    }

    /// Why the real helper would refuse this Load, Store or Reset at this moment; nil if it
    /// would pass it on (src/ua101_service.c, ua101_service_memory_passes, in its order).
    private func memoryRefusal(_ bit: UInt64) -> String? {
        if bit == 0 || passes & UInt64(PANEL_PASS_MEMORY) == 0 || passesMemory & bit != bit {
            return "of Load, Store and Reset this helper passes on 37 exact messages only: Load and Store of Memory 1 to 5 with the patch bay, the mixer or both, and Reset of the settings in use, of Memory 1 to 5, or of everything; the message was not sent"
        }
        if let refusal = refuseMemory { return refusal }
        if streaming { return "sound is being played or recorded through the unit, and a Load, Store or Reset is never passed on meanwhile; the message was not sent" }
        let now = scheduler.now
        if let m = memoryAt, now - m < Self.memoryGapMicros {
            return "another Load, Store or Reset was passed on less than \(panel_memory_gap_seconds()) seconds ago; the message was not sent"
        }
        guard let b = blockAt, now - b <= Self.memoryFreshMicros else {
            return "the unit's settings weren't read in the \(panel_memory_fresh_seconds()) seconds before it (or a switch on the unit moved since), so there would be no record of them; the message was not sent"
        }
        if blockEvent < changeEvent || blockEvent < memoryEvent {
            return "the unit's settings weren't read again after the last change, so there would be no record of them as they are now; the message was not sent"
        }
        return nil
    }

    func attach() {
        attaches += 1
        scheduler.soon { [weak self] in
            guard let self = self else { return }
            if !self.installed {
                self.onAttached?(Int32(PANEL_LINK_ERR_NO_HELPER), false, false, "the helper can't be reached")
            } else if self.asleep {
                self.onAttached?(Int32(PANEL_LINK_ERR_ASLEEP), false, false, "the Mac is going to sleep or waking")
            } else if !self.unitPresent {
                self.onAttached?(Int32(PANEL_LINK_ERR_ABSENT), false, true, "no UA-101 in Hi-Speed mode was found")
            } else if self.interfaceBusy {
                self.onAttached?(Int32(PANEL_LINK_ERR_ABSENT), false, false, "another program is using it")
            } else {
                self.attached = true
                // A new connection: what was read before no longer counts (the real helper
                // forgets it when it opens the unit's interface).
                self.blockAt = nil
                self.onPasses?(self.passes, self.passesOn, self.passesMemory)
                self.onAttached?(Int32(PANEL_LINK_OK), self.streaming, false, "")
            }
        }
    }

    func send(_ bytes: UnsafeBufferPointer<UInt8>) -> Bool {
        guard attached else { return false }
        let read = panel_is_read_request(bytes.baseAddress, bytes.count) != 0
        let allowed = Allowed(passes: passes, passesOn: passesOn)
        let output = Int(panel_patch_message_output(bytes.baseAddress, bytes.count))
        let channel = Int(panel_volume_message_channel(bytes.baseAddress, bytes.count))
        let knob = Int(panel_pan_message_channel(bytes.baseAddress, bytes.count))
        let switchOf = Int(panel_on_message_channel(bytes.baseAddress, bytes.count))
        let pair = Int(panel_link_message_pair(bytes.baseAddress, bytes.count))
        let soloOf = Int(panel_solo_message_group(bytes.baseAddress, bytes.count))
        let known = (allowed.saving && panel_is_saving_message(bytes.baseAddress, bytes.count) != 0)
            || (output >= 0 && allowed.patchOutputs.contains(output))
            || (channel >= 0 && allowed.volumes.contains(Allowed.volumeStrips[channel]))
            || (knob >= 0 && allowed.pans.contains(Allowed.volumeStrips[knob]))
            || (switchOf >= 0 && allowed.ons.contains(Allowed.volumeStrips[switchOf]))
            || (pair >= 0 && allowed.links.contains(pair))
            || (soloOf >= 0 && allowed.solos.contains(soloOf))
        var why = "this helper passes on seven changes only: pausing and resuming the unit's saving, the patch bay source of an output pair, the volume (up to 0 dB), the pan and the on/off (0 or 1) of a mixer channel, the stereo link (0 or 1) of a pair, and the solo of the INPUT or the OUTPUT group (0 for none, or a channel 1 to 10); the message was not sent"
        // Load, Store and Reset go by rules of their own.
        let command = panel_memory_message_bit(bytes.baseAddress, bytes.count)
        var memoryPasses = false
        if !read, !known, command != 0 || (bytes.count == 16 && bytes[7] == 0) {
            if let refusal = memoryRefusal(command) { why = refusal } else { memoryPasses = true }
        }
        if let refusal = refuseChanges, !read {
            refused += 1
            scheduler.soon { [weak self] in self?.onRefused?(refusal) }
            return true
        }
        if read || known || memoryPasses {
            let now = scheduler.now / 1_000_000
            if now != second { second = now; sendsNow = 0 }
            sendsNow += 1
            if sendsNow <= Self.sendsPerSecond {
                passedOn += 1
                if !read {
                    changesPassedOn += 1
                    changes.append(Array(bytes))
                    events += 1
                    if memoryPasses {
                        memoryCommands.append(Array(bytes))
                        memoryRecords.append(lastBlock)
                        memoryAt = scheduler.now
                        memoryEvent = events
                    } else {
                        changeEvent = events
                    }
                }
                return unit.send(bytes)
            }
            tooMany += 1
            why = "too many requests in one second; not sent"
        }
        refused += 1
        scheduler.soon { [weak self] in self?.onRefused?(why) }
        return true
    }

    /// For a test: the list of what this helper passes on reaches the app anew, on a later
    /// turn, as it does with an attach (after the test changed `passes`).
    func tellPasses() {
        scheduler.soon { [weak self] in
            guard let self = self, self.attached else { return }
            self.onPasses?(self.passes, self.passesOn, self.passesMemory)
        }
    }

    func askLevels() {
        levelAsks += 1
        scheduler.soon { [weak self] in
            guard let self = self, self.attached else { return }
            var l = self.levels
            l.streaming = self.streaming && !self.notRunningYet
            if !l.streaming { l = HelperLevels() }
            self.onLevels?(l)
            // As the real helper: what was reported isn't reported again.
            self.levels.capture = self.levels.capture.map { _ in 0 }
            self.levels.playback = self.levels.playback.map { _ in 0 }
        }
    }

    func detach() {
        detaches += 1
        attached = false
    }

    // MARK: what happens on the pretend Mac

    private func tellChanged() {
        scheduler.soon { [weak self] in self?.onChanged?() }
    }

    func unitGoes() {
        unitPresent = false
        if attached {
            attached = false
            tellChanged()
        }
    }

    func unitComes() {
        unitPresent = true
        tellChanged()
    }

    func macSleeps() {
        asleep = true
        attached = false
    }

    func macWakes() {
        asleep = false
        tellChanged()
    }

    /// MIDI passes the unit's MIDI IN (`in`) or MIDI OUT (`out`) socket: as the real helper
    /// (link version 15), told to the app while attached, on a later turn. The pacing to 10 a
    /// second is the helper's (src/ua101_midilamp.h, tested in tests/test_midilamp.c); this
    /// stand-in tells each one it is given.
    func midiPasses(in i: Bool, out o: Bool) {
        scheduler.soon { [weak self] in
            guard let self = self, self.attached else { return }
            self.onMIDI?(i, o)
        }
    }

    /// A lamp event that was already on its way when the app stopped being attached: it
    /// arrives anyway. The transport itself must drop it.
    func midiInFlight(in i: Bool, out o: Bool) {
        scheduler.soon { [weak self] in self?.onMIDI?(i, o) }
    }

    /// An app starts or stops playing or recording through the UA-101.
    func setStreaming(_ on: Bool) {
        streaming = on
        scheduler.soon { [weak self] in
            guard let self = self, self.attached else { return }
            self.onAudio?(on)
        }
    }
}
