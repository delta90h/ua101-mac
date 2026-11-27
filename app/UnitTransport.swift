// The way to a unit. There are two: the pretend unit, and the real UA-101
// through the helper (ua101d). For the real one, only the changes the helper
// says it passes on leave here (`Allowed`), and only while changes are on in
// the app (its Lock Changes switches them off). Up to link version 17 the
// helper also wanted the user's go-ahead files; since version 18 there are
// none (src/ua101_link.h). The list, since link version 14: pausing and resuming the
// unit's saving, the patch bay sources, of every channel of the mixer the
// volume up to 0 dB, the pan from 0x0000 to 0x8000 (MONITOR OUT has none) and
// the on/off bit, 0 or 1, of every pair the stereo link, 0 or 1, and of the
// INPUT and the OUTPUT group the solo, 0 for none or a channel 1 to 10.
// "Reset Mixer" and "Open Backup" are made of those same messages
// and nothing else. Since link version 17 Load, Store
// and Reset to Factory can leave too: only the commands the helper names.
// No code in this app opens USB or MIDI itself.
import Foundation

/// The changes a way to the unit lets through once changes are on. For the UA-101 it is read
/// from the helper's answer (PANEL_PASS_ bits), never assumed: an older or newer helper gives
/// another list, and the app follows it.
struct Allowed: Equatable {
    /// The pretend unit: everything.
    var everything = false
    /// Pausing and resuming the unit's saving.
    var saving = false
    /// The output pairs, 0 to 4, whose patch bay source may be changed.
    var patchOutputs: Set<Int> = []
    /// The channels whose volume may be changed, to any value up to 0 dB (`panel_volume_sendable`).
    var volumes: Set<StripID> = []
    /// The channels whose pan may be changed, to any value from 0x0000 to 0x8000 (`panel_pan_sendable`).
    var pans: Set<StripID> = []
    /// The channels whose on/off bit may be set, to 0 or 1 (since link version 11; bit 1 is
    /// "muted", heard on INPUT 1 on 2026-11-14 and taken to be so for every channel).
    var ons: Set<StripID> = []
    /// The pairs whose stereo link may be set, to 0 or 1 (since link version 14), by the
    /// helper's numbers: 0 MONITOR OUT L-R, 1 to 5 INPUT 1-2 to 9-10, 6 to 10 OUTPUT 1-2 to 9-10.
    var links: Set<Int> = []
    /// The groups whose solo may be set, to 0 (none) or a channel 1 to 10 (since link version
    /// 14): 1 INPUT, 2 OUTPUT. MONITOR OUT has no solo.
    var solos: Set<Int> = []
    /// The Recall, Save and Reset commands that may be sent
    /// (since link version 17), a bit each (PANEL_PASS_MEM_): Load and Store of each
    /// memory, Reset of each memory, of the settings in use, and of everything.
    var memory: UInt64 = 0

    /// A pair's number in the helper's list.
    static func pairNumber(group: Int, pair: Int) -> Int {
        group == ChannelNames.groupMonitor ? pair : 1 + (group - 1) * 5 + pair
    }
    static let allPairs = 11

    /// A channel's number in the helper's list: 0 and 1 MONITOR OUT, 2 to 11 INPUT, 12 to 21 OUTPUT.
    static func volumeNumber(_ s: StripID) -> Int {
        s.group == ChannelNames.groupMonitor ? s.channel : 2 + (s.group - 1) * 10 + s.channel
    }
    static let volumeStrips: [StripID] =
        (0..<2).map { StripID(group: ChannelNames.groupMonitor, channel: $0) }
        + (0..<10).map { StripID(group: ChannelNames.groupInput, channel: $0) }
        + (0..<10).map { StripID(group: ChannelNames.groupOutput, channel: $0) }

    static let all = Allowed(everything: true)
    static let nothing = Allowed()

    init(everything: Bool = false) { self.everything = everything }

    /// `passesOn`: the helper's second number, the channels whose on/off it passes on (only
    /// read when `passes` has PANEL_PASS_ON).
    /// `passesMemory`: its third number, the Recall, Save and Reset commands it passes on
    /// (only read when `passes` has PANEL_PASS_MEMORY).
    init(passes: UInt64, passesOn: UInt64 = 0, passesMemory: UInt64 = 0) {
        saving = passes & UInt64(PANEL_PASS_SAVING) != 0
        if passes & UInt64(PANEL_PASS_MEMORY) != 0 { memory = passesMemory }
        if passes & UInt64(PANEL_PASS_ON) != 0 {
            for (n, strip) in Self.volumeStrips.enumerated() where passesOn & (PANEL_PASS_ON_CHANNEL_0 << UInt64(n)) != 0 {
                ons.insert(strip)
            }
        }
        if passes & UInt64(PANEL_PASS_LINK) != 0 {
            for n in 0..<Self.allPairs where passesOn & (PANEL_PASS_LINK_PAIR_0 << UInt64(n)) != 0 { links.insert(n) }
        }
        if passes & UInt64(PANEL_PASS_SOLO) != 0 {
            for g in 1...2 where passesOn & (PANEL_PASS_SOLO_GROUP_1 << UInt64(g - 1)) != 0 { solos.insert(g) }
        }
        if passes & UInt64(PANEL_PASS_PATCH) != 0 {
            for output in 0..<5 where passes & (UInt64(PANEL_PASS_PATCH_OUTPUT_0) << UInt64(output)) != 0 {
                patchOutputs.insert(output)
            }
        }
        if passes & UInt64(PANEL_PASS_VOLUME) != 0 {
            for (n, strip) in Self.volumeStrips.enumerated() where passes & (UInt64(PANEL_PASS_VOLUME_CHANNEL_0) << UInt64(n)) != 0 {
                volumes.insert(strip)
            }
        }
        if passes & UInt64(PANEL_PASS_PAN) != 0 {
            // Numbered as the volumes are; MONITOR OUT (0 and 1) has no pan.
            for (n, strip) in Self.volumeStrips.enumerated() where n >= 2 && passes & (UInt64(PANEL_PASS_PAN_CHANNEL_0) << UInt64(n)) != 0 {
                pans.insert(strip)
            }
        }
    }

    var isEmpty: Bool {
        !everything && !saving && patchOutputs.isEmpty && volumes.isEmpty && pans.isEmpty && ons.isEmpty
            && links.isEmpty && solos.isEmpty
    }
    /// Every mixer control at once, for "Reset Mixer" and "Open Backup", which
    /// change them all inside one pause of the unit's saving: the pretend unit, and a helper
    /// that passes on the saving and every control of every strip, pair and group (link
    /// version 14 and later). A helper that passes on less: no.
    var mixer: Bool { everything || (saving && wholeMixer) }
    func link(group: Int, pair: Int) -> Bool { everything || links.contains(Self.pairNumber(group: group, pair: pair)) }
    func solo(group: Int) -> Bool { group != ChannelNames.groupMonitor && (everything || solos.contains(group)) }
    /// Every control of every strip is passed on: the pill then says just "mixer".
    private var wholeMixer: Bool {
        volumes.count == Self.volumeStrips.count && ons == volumes && pans == Self.panStrips
            && links.count == Self.allPairs && solos == [1, 2]
    }
    /// One Recall, Save or Reset command, by its bit (PANEL_PASS_MEM_).
    func memory(_ bit: UInt64) -> Bool { everything || (bit != 0 && memory & bit == bit) }
    /// Some Recall, Save or Reset command is passed on at all.
    var someMemory: Bool { everything || memory != 0 }
    func volume(_ s: StripID) -> Bool { everything || volumes.contains(s) }
    func pan(_ s: StripID) -> Bool { s.group != ChannelNames.groupMonitor && (everything || pans.contains(s)) }
    func on(_ s: StripID) -> Bool { everything || ons.contains(s) }
    /// Some mixer control can be dragged, so a drag pauses the unit's saving.
    var someMixer: Bool { everything || !volumes.isEmpty || !pans.isEmpty }
    func patch(_ output: Int) -> Bool { everything || patchOutputs.contains(output) }
    var allPatch: Bool { everything || patchOutputs.count == 5 }

    private var patchWords: String? {
        if patchOutputs.isEmpty { return nil }
        if patchOutputs.count == 5 { return "the patch bay" }
        let names = patchOutputs.sorted().map { ChannelNames.outputPairs[$0] }
        return "the patch bay source of " + names.joined(separator: ", ")
    }

    /// The channels that have a pan: every one but MONITOR OUT L and R.
    static let panStrips = Set(volumeStrips.filter { $0.group != ChannelNames.groupMonitor })

    /// A list of channels for a sentence. A helper of link version 13 names all 22 channels
    /// (or, for the pan, the 20 that have one): that is said in a few words; any other list
    /// is given name by name.
    static func names(_ set: Set<StripID>) -> String {
        if set.count == volumeStrips.count { return "every channel" }
        if set == panStrips { return "every INPUT and OUTPUT channel" }
        return volumeStrips.filter { set.contains($0) }.map { ChannelNames.full($0) }.joined(separator: ", ")
    }

    /// The same for a title or a pill: "mixer" for all 22 channels.
    static func shortNames(_ set: Set<StripID>) -> String {
        if set.count == volumeStrips.count { return "mixer" }
        if set == panStrips { return "INPUT and OUTPUT" }
        return volumeStrips.filter { set.contains($0) }.map { ChannelNames.full($0) }.joined(separator: ", ")
    }

    private var volumeNames: String { Self.names(volumes) }
    private var panNames: String { Self.names(pans) }
    private var onNames: String { Self.names(ons) }
    private var linkNames: String {
        if links.count == Self.allPairs { return "every pair" }
        var names: [String] = []
        for g in 0..<3 {
            for p in 0..<(g == ChannelNames.groupMonitor ? 1 : 5) where links.contains(Self.pairNumber(group: g, pair: p)) {
                names.append(ChannelNames.pair(group: g, pair: p))
            }
        }
        return names.joined(separator: ", ")
    }
    private var soloNames: String {
        solos == [1, 2] ? "INPUT and OUTPUT" : solos.sorted().map { ChannelNames.groupTitle[$0] }.joined(separator: ", ")
    }

    /// For a sentence: "the unit's saving (paused and resumed, Device menu), the patch bay
    /// (Patch Bay window), the volume of INPUT 1 (up to 0 dB) and the pan of INPUT 1".
    var words: String {
        var parts: [String] = []
        if saving { parts.append("the unit's saving (paused and resumed, Device menu)") }
        if let p = patchWords { parts.append(p + " (Patch Bay window)") }
        if !volumes.isEmpty { parts.append("the volume of \(volumeNames) (up to 0 dB)") }
        if !pans.isEmpty { parts.append("the pan of \(panNames)") }
        if !ons.isEmpty { parts.append("the mute of \(onNames)") }
        if !links.isEmpty { parts.append("the stereo link of \(linkNames)") }
        if !solos.isEmpty { parts.append("the solo of \(soloNames)") }
        if parts.count < 3 { return parts.isEmpty ? "nothing" : parts.joined(separator: " and ") }
        return parts.dropLast().joined(separator: ", ") + " and " + parts.last!
    }

    /// For a title or a pill: "saving, OUTPUT 1-2's source".
    var short: String {
        var parts: [String] = []
        if saving { parts.append("saving") }
        if patchOutputs.count == 5 { parts.append("patch bay") }
        else if !patchOutputs.isEmpty {
            parts.append(patchOutputs.sorted().map { ChannelNames.outputPairs[$0] }.joined(separator: ", ") + " patch")
        }
        // The pan goes with the volume where it is passed for the same channels, MONITOR OUT
        // (which has none) left aside.
        if wholeMixer {
            parts.append("mixer")
            return parts.joined(separator: " · ")
        }
        let samePans = !pans.isEmpty && pans == volumes.intersection(Self.panStrips)
        let together = samePans && volumes == ons
        if together { parts.append(Self.shortNames(volumes) + " volume, pan, mute") }
        else if samePans { parts.append(Self.shortNames(volumes) + " volume and pan") }
        else {
            if !volumes.isEmpty { parts.append(Self.shortNames(volumes) + " volume") }
            if !pans.isEmpty { parts.append(Self.shortNames(pans) + " pan") }
        }
        if !ons.isEmpty, !together { parts.append(Self.shortNames(ons) + " mute") }
        if !links.isEmpty { parts.append((links.count == Self.allPairs ? "" : linkNames + " ") + "link") }
        if !solos.isEmpty { parts.append((solos == [1, 2] ? "" : soloNames + " ") + "solo") }
        return parts.isEmpty ? "nothing" : parts.joined(separator: " · ")
    }
}

protocol UnitTransport: AnyObject {
    var displayName: String { get }
    /// True only for the pretend unit.
    var isPretend: Bool { get }
    /// The model's switch: changes may leave through here, Recall, Save and Reset among them.
    /// Off at the start and after `stop`.
    var changesOn: Bool { get set }
    /// Which changes this way lets through while `changesOn`. For the UA-101: what the helper
    /// said it passes on when the app attached (nothing before that).
    var allowed: Allowed { get }
    /// Sound is being played or recorded through the unit: the helper passes on no Load,
    /// Store or Reset meanwhile. Always false for the pretend unit.
    var soundPlaying: Bool { get }
    /// The way to the unit refused a change, with the reason. Nothing was sent.
    var onRefused: ((String) -> Void)? { get set }
    /// One complete SysEx message from the unit.
    var onMessage: (([UInt8]) -> Void)? { get set }
    /// The way to the unit came up, or went away (with the reason in plain words).
    var onLink: ((Bool, String?) -> Void)? { get set }
    /// Levels from the unit's audio, in dBFS for [group][channel], and the seconds since the
    /// reading before; nil when there are none any more (no app plays or records).
    var onLevels: (([[Float]]?, Float) -> Void)? { get set }
    /// MIDI passed the unit's MIDI IN socket (first value) or MIDI OUT socket (second) just now.
    /// The UA-101's helper says so at most 10 times a second (link version 15); the pretend
    /// unit has no MIDI and never says it.
    var onMIDI: ((Bool, Bool) -> Void)? { get set }
    /// Starts looking for the unit, and stops. The pretend unit is simply there.
    func start()
    func stop()
    /// Hands one SysEx message towards the unit. False if it couldn't be taken.
    func send(_ bytes: UnsafeBufferPointer<UInt8>) -> Bool
}

#if !UA101_USER || UA101_PICTURES
/// The pretend UA-101 (src/ua101_ctlsim.c), with its front panel for the test and the user.
final class PretendUnit: UnitTransport {
    let displayName = "Pretend unit"
    let isPretend = true
    var changesOn = false
    let soundPlaying = false
    let allowed = Allowed.all
    var onRefused: ((String) -> Void)?
    var onMessage: (([UInt8]) -> Void)?
    var onLink: ((Bool, String?) -> Void)?
    var onLevels: (([[Float]]?, Float) -> Void)?
    var onMIDI: ((Bool, Bool) -> Void)?
    /// Called after the pretend front panel changed, so its own window redraws.
    var onFrontPanelChange: (() -> Void)?

    let sim: OpaquePointer
    private let scheduler: PanelScheduler
    private var drainBooked = false
    private(set) var ignoredKind = -1
    private(set) var silent = false

    init(scheduler: PanelScheduler) {
        self.scheduler = scheduler
        sim = panel_sim_new()
    }

    deinit { panel_sim_free(sim) }

    func start() {}
    func stop() { changesOn = false }

    func send(_ bytes: UnsafeBufferPointer<UInt8>) -> Bool {
        panel_sim_receive(sim, bytes.baseAddress, bytes.count)
        bookDrain()
        return true
    }

    /// Replies travel on a later turn, as they would from a real unit.
    private func bookDrain() {
        if drainBooked { return }
        drainBooked = true
        scheduler.soon { [weak self] in self?.drain() }
    }

    private func drain() {
        drainBooked = false
        var buffer = [UInt8](repeating: 0, count: Int(PANEL_MAX_MESSAGE))
        while true {
            let n = panel_sim_next(sim, &buffer)
            if n == 0 { break }
            onMessage?(Array(buffer[0..<n]))
        }
    }

    // The pretend unit's front panel.
    var softControl: Bool { panel_sim_soft_control(sim) != 0 }
    var mono: Bool { panel_sim_mono(sim) != 0 }
    var digital: Bool { panel_sim_digital(sim) != 0 }
    var rateCode: Int { Int(panel_sim_rate_code(sim)) }

    /// Presses or releases a switch. A real switch bounces: three messages.
    func press(_ which: Int32, in pressed: Bool) {
        panel_sim_switch(sim, which, pressed ? 1 : 0, 3)
        bookDrain()
        onFrontPanelChange?()
    }

    /// As on the unit: the rate knob only counts after a power cycle, which
    /// the caller does by taking the link down and up (PanelModel.reconnect).
    func setRate(code: Int) {
        panel_sim_set_rate_code(sim, UInt32(code))
        onFrontPanelChange?()
    }

    /// Makes the pretend unit ignore one kind of change (PANEL_F_...), or -1 for none.
    func ignore(kind: Int) {
        ignoredKind = kind
        panel_sim_ignore_kind(sim, Int32(kind))
        onFrontPanelChange?()
    }

    func setSilent(_ on: Bool) {
        silent = on
        panel_sim_set_silent(sim, on ? 1 : 0)
        onFrontPanelChange?()
    }
}
#endif

// MARK: the real unit, through the helper

/// What the helper says about the audio it is streaming: the largest sample of each channel
/// since the last ask (PANEL_LEVEL_FULL_SCALE is full scale).
struct HelperLevels: Equatable {
    var streaming = false
    var rate = 0.0
    var capture: [UInt32] = []
    var playback: [UInt32] = []
}

/// The app's end of the link to the helper (src/ua101_ctllink.h). Every answer comes back on a
/// later turn. The real one is XPCHelperLink; the test and the pictures use FakeHelper.
protocol HelperLink: AnyObject {
    /// The answer to attach: a PANEL_LINK_ status, whether audio is streaming, whether the
    /// helper will say when a unit appears, and the reason when the status isn't OK.
    var onAttached: ((Int32, Bool, Bool, String) -> Void)? { get set }
    var onMessage: (([UInt8]) -> Void)? { get set }
    /// The unit came, went or was lost, the Mac woke, or the helper restarted: attach again.
    var onChanged: (() -> Void)? { get set }
    var onAudio: ((Bool) -> Void)? { get set }
    var onLevels: ((HelperLevels) -> Void)? { get set }
    /// The helper didn't pass a message on to the unit.
    var onRefused: ((String) -> Void)? { get set }
    /// Which changes the helper passes on. Comes before each attach that succeeded. The
    /// first value: the PANEL_PASS_ bits; the second: the channels whose on/off it passes on,
    /// and since link version 14 the linked pairs and the solo groups (higher bits); the
    /// third: the Recall, Save and Reset commands (PANEL_PASS_MEM_ bits).
    var onPasses: ((UInt64, UInt64, UInt64) -> Void)? { get set }
    /// While attached (link version 15): MIDI passed the unit's MIDI IN (first) or MIDI OUT
    /// (second) socket just now. At most 10 a second, only while MIDI passes.
    var onMIDI: ((Bool, Bool) -> Void)? { get set }

    func attach()
    func send(_ bytes: UnsafeBufferPointer<UInt8>) -> Bool
    func askLevels()
    func detach()
}

/// The link to the installed helper, over XPC, on the main queue.
final class XPCHelperLink: HelperLink {
    var onAttached: ((Int32, Bool, Bool, String) -> Void)?
    var onMessage: (([UInt8]) -> Void)?
    var onChanged: (() -> Void)?
    var onAudio: ((Bool) -> Void)?
    var onLevels: ((HelperLevels) -> Void)?
    var onRefused: ((String) -> Void)?
    var onPasses: ((UInt64, UInt64, UInt64) -> Void)?
    var onMIDI: ((Bool, Bool) -> Void)?

    private var link: OpaquePointer?

    /// The first `count` numbers of one of the C struct's fixed arrays.
    private static func list<T>(_ tuple: T, _ count: UInt32) -> [UInt32] {
        withUnsafeBytes(of: tuple) { raw in
            Array(raw.bindMemory(to: UInt32.self).prefix(Int(min(count, UInt32(UA101_CTLLINK_MAX_CHANNELS)))))
        }
    }

    private static func me(_ ctx: UnsafeMutableRawPointer?) -> XPCHelperLink? {
        ctx.map { Unmanaged<XPCHelperLink>.fromOpaque($0).takeUnretainedValue() }
    }

    init() {
        var events = ua101_ctllink_events(
            attached: { ctx, status, streaming, watching, why in
                XPCHelperLink.me(ctx)?.onAttached?(status, streaming != 0, watching != 0, why.map { String(cString: $0) } ?? "")
            },
            message: { ctx, msg, len in
                guard let msg = msg else { return }
                XPCHelperLink.me(ctx)?.onMessage?(Array(UnsafeBufferPointer(start: msg, count: len)))
            },
            changed: { ctx in XPCHelperLink.me(ctx)?.onChanged?() },
            audio: { ctx, streaming in XPCHelperLink.me(ctx)?.onAudio?(streaming != 0) },
            levels: { ctx, levels in
                guard let l = levels?.pointee else { return }
                XPCHelperLink.me(ctx)?.onLevels?(HelperLevels(
                    streaming: l.streaming != 0, rate: l.rate_hz,
                    capture: XPCHelperLink.list(l.capture, l.capture_channels),
                    playback: XPCHelperLink.list(l.playback, l.playback_channels)))
            },
            refused: { ctx, _, why in XPCHelperLink.me(ctx)?.onRefused?(why.map { String(cString: $0) } ?? "") },
            passes: { ctx, passes, passesOn, passesMemory in
                XPCHelperLink.me(ctx)?.onPasses?(passes, passesOn, passesMemory)
            },
            midi: { ctx, i, o in XPCHelperLink.me(ctx)?.onMIDI?(i != 0, o != 0) })
        link = ua101_ctllink_open(DispatchQueue.main, &events, Unmanaged.passUnretained(self).toOpaque())
    }

    deinit { if let link = link { ua101_ctllink_close(link) } }

    func attach() { if let link = link { ua101_ctllink_attach(link) } }
    func send(_ bytes: UnsafeBufferPointer<UInt8>) -> Bool {
        guard let link = link else { return false }
        return ua101_ctllink_send(link, bytes.baseAddress, bytes.count) != 0
    }
    func askLevels() { if let link = link { ua101_ctllink_ask_levels(link) } }
    func detach() { if let link = link { ua101_ctllink_detach(link) } }
}

/// The real UA-101, through the helper. `send` hands on the request for the settings, and,
/// while the model has switched changes on, the messages for the changes the helper said it
/// passes on (`allowed`): the two that pause and resume the unit's saving, the patch bay
/// source of the output pairs it named, the volume of the channels it named (a value
/// that is at most 0 dB), the pan of the channels it named (0x0000 to 0x8000), the on/off
/// bit of the channels it named (0 or 1), the stereo link of the pairs it named (0 or 1) and
/// the solo of the groups it named (0 to 10); and the Recall, Save and Reset commands the
/// helper named, byte for byte (`panel_memory_message_bit`). Everything else stops here,
/// whatever the engine above it was allowed to do. The helper has the same rules of its own,
/// so two things would have to break at once.
final class HelperTransport: UnitTransport {
    let displayName = "UA-101"
    let isPretend = false
    var changesOn = false
    var onRefused: ((String) -> Void)?
    /// What the helper said it passes on, with the last attach that succeeded.
    private(set) var passes: UInt64 = 0
    private(set) var passesOn: UInt64 = 0
    private(set) var passesMemory: UInt64 = 0
    /// What the helper passes on, as it said itself.
    var allowed: Allowed { Allowed(passes: passes, passesOn: passesOn, passesMemory: passesMemory) }
    var soundPlaying: Bool { streaming }
    var onMessage: (([UInt8]) -> Void)?
    var onLink: ((Bool, String?) -> Void)?
    var onLevels: (([[Float]]?, Float) -> Void)?
    var onMIDI: ((Bool, Bool) -> Void)?
    /// "midi_lamps" events heard while attached (for the self-test).
    private(set) var midiEvents = 0

    static let asleepRetryMicros: UInt64 = 2_000_000, busyRetryMicros: UInt64 = 3_000_000
    /// What the meters' tool-tip says the levels come from.
    static let levelsName = "the UA-101's audio"

    private let link: HelperLink
    private let scheduler: PanelScheduler
    /// How often levels are asked for while audio streams; nil: only when asked by hand (tests).
    private let pollHz: Double?
    private var wanted = false
    private var retryID: Int?
    private var pollTimer: Timer?
    private var lastLevelsAt: UInt64?

    /// Attached to the unit's control messages.
    private(set) var up = false
    /// An app is playing or recording through the UA-101, so there are levels to show.
    private(set) var streaming = false
    /// Messages stopped here: neither the settings request nor, with changes on, one of the
    /// changes the helper passes on. Should stay 0: the model and the engine stop them first.
    private(set) var blocked = 0
    /// Messages the helper refused: a Recall, Save or Reset against one of its rules (sound
    /// started, say), or something got past `blocked`, which should never happen.
    private(set) var helperRefusals = 0
    /// The window with the meters can be seen. Levels are only asked for while it can.
    var metersShown = true { didSet { updatePolling() } }
    /// Levels are being asked for, or would be with a timer (the tests ask by hand).
    var wantsLevels: Bool { streaming && metersShown }

    init(link: HelperLink, scheduler: PanelScheduler, pollHz: Double? = nil) {
        self.link = link
        self.scheduler = scheduler
        self.pollHz = pollHz
        link.onAttached = { [weak self] status, streaming, watching, why in
            self?.attached(status, streaming: streaming, watching: watching, why: why)
        }
        link.onMessage = { [weak self] bytes in
            guard let self = self, self.up else { return }
            self.onMessage?(bytes)
        }
        link.onChanged = { [weak self] in self?.changed() }
        link.onAudio = { [weak self] on in self?.setStreaming(on) }
        link.onLevels = { [weak self] levels in self?.gotLevels(levels) }
        link.onMIDI = { [weak self] i, o in
            guard let self = self, self.up else { return }
            self.midiEvents += 1
            self.onMIDI?(i, o)
        }
        link.onRefused = { [weak self] why in
            guard let self = self else { return }
            self.helperRefusals += 1
            self.changesOn = false
            self.onRefused?(why)
        }
        link.onPasses = { [weak self] passes, passesOn, passesMemory in
            guard let self = self else { return }
            self.passes = passes
            self.passesOn = passesOn
            self.passesMemory = passesMemory
        }
    }

    deinit { pollTimer?.invalidate() }

    func start() {
        wanted = true
        cancelRetry()
        link.attach()
    }

    func stop() {
        wanted = false
        cancelRetry()
        setStreaming(false)
        up = false
        changesOn = false
        link.detach()
    }

    func send(_ bytes: UnsafeBufferPointer<UInt8>) -> Bool {
        guard up else { return false }
        if panel_is_read_request(bytes.baseAddress, bytes.count) != 0 { return link.send(bytes) }
        if changesOn {
            let allowed = self.allowed
            if allowed.saving, panel_is_saving_message(bytes.baseAddress, bytes.count) != 0 { return link.send(bytes) }
            let output = Int(panel_patch_message_output(bytes.baseAddress, bytes.count))
            if output >= 0, allowed.patchOutputs.contains(output) { return link.send(bytes) }
            let channel = Int(panel_volume_message_channel(bytes.baseAddress, bytes.count))
            if channel >= 0, allowed.volumes.contains(Allowed.volumeStrips[channel]) { return link.send(bytes) }
            let knob = Int(panel_pan_message_channel(bytes.baseAddress, bytes.count))
            if knob >= 0, allowed.pans.contains(Allowed.volumeStrips[knob]) { return link.send(bytes) }
            let switchOf = Int(panel_on_message_channel(bytes.baseAddress, bytes.count))
            if switchOf >= 0, allowed.ons.contains(Allowed.volumeStrips[switchOf]) { return link.send(bytes) }
            let pair = Int(panel_link_message_pair(bytes.baseAddress, bytes.count))
            if pair >= 0, allowed.links.contains(pair) { return link.send(bytes) }
            let soloOf = Int(panel_solo_message_group(bytes.baseAddress, bytes.count))
            if soloOf >= 0, allowed.solos.contains(soloOf) { return link.send(bytes) }
            // Recall, Save and Reset: only a command the helper named, byte for byte one of
            // its 37 messages.
            let command = panel_memory_message_bit(bytes.baseAddress, bytes.count)
            if command != 0, allowed.memory & command == command { return link.send(bytes) }
        }
        blocked += 1
        return false
    }

    /// Asks the helper for levels once. The answer goes to `onLevels`.
    func pollLevels() {
        if up { link.askLevels() }
    }

    // MARK: what the helper says

    /// Why there is no unit, in plain words.
    static func note(status: Int32, watching: Bool, why: String) -> String {
        switch status {
        case Int32(PANEL_LINK_ERR_NO_HELPER):
            return Build.isEndUser
                ? "The driver's helper can't be reached. The driver isn't installed, or an older one is. Install UA-101 Driver.pkg, then choose UA-101 again."
                : "The driver's helper can't be reached. It isn't installed yet, or an older one is. Install with install/install.sh, then choose UA-101 again."
        case Int32(PANEL_LINK_ERR_VERSION):
            return "The driver's helper is from another build than this app. Install again with install/install.sh, then choose UA-101 again."
        case Int32(PANEL_LINK_ERR_ABSENT):
            return watching
                ? "The UA-101 isn't connected or is switched off. It shows here by itself when it comes on."
                : "The UA-101 is there, but its control connection can't be opened: \(why). Trying again every 3 seconds."
        case Int32(PANEL_LINK_ERR_ASLEEP):
            return "The Mac is going to sleep or waking. Trying again."
        case Int32(PANEL_LINK_ERR_DENIED):
            return "The driver's helper doesn't let this account in. It serves the accounts people log in to, and an older helper only the account that installed it: install UA-101 Driver.pkg again."
        default:
            return "The driver's helper answered: \(why)"
        }
    }

    private func attached(_ status: Int32, streaming: Bool, watching: Bool, why: String) {
        guard wanted else { return }
        if status == Int32(PANEL_LINK_OK) {
            up = true
            onLink?(true, nil)
            setStreaming(streaming)
            return
        }
        up = false
        setStreaming(false)
        onLink?(false, Self.note(status: status, watching: watching, why: why))
        // Asked again only where no event will come: while the Mac wakes, and while another
        // program holds the unit's control connection.
        if status == Int32(PANEL_LINK_ERR_ASLEEP) {
            retry(after: Self.asleepRetryMicros)
        } else if status == Int32(PANEL_LINK_ERR_ABSENT), !watching {
            retry(after: Self.busyRetryMicros)
        }
    }

    private func changed() {
        guard wanted else { return }
        if up {
            up = false
            setStreaming(false)
            onLink?(false, "The UA-101 went away. Looking for it again.")
        }
        cancelRetry()
        link.attach()
    }

    private func retry(after micros: UInt64) {
        cancelRetry()
        retryID = scheduler.schedule(at: scheduler.now + micros) { [weak self] in
            guard let self = self else { return }
            self.retryID = nil
            if self.wanted, !self.up { self.link.attach() }
        }
    }

    private func cancelRetry() {
        if let id = retryID { scheduler.cancel(id) }
        retryID = nil
    }

    // MARK: levels

    private func setStreaming(_ on: Bool) {
        let now = on && up
        if streaming && !now {
            lastLevelsAt = nil
            onLevels?(nil, 0)
        }
        streaming = now
        updatePolling()
    }

    private func gotLevels(_ levels: HelperLevels) {
        guard up else { return }
        guard levels.streaming else {
            // The helper has no levels to give. That isn't news that audio stopped: for the
            // first 0.2 s after a start (measured on the unit, 2026-11-11) the stream isn't
            // running yet and every answer is this one. Audio stopping comes as its own event.
            // So the asking goes on, and the meters show nothing until a live answer.
            if lastLevelsAt != nil {
                lastLevelsAt = nil
                onLevels?(nil, 0)
            }
            return
        }
        if !streaming { setStreaming(true) }
        let now = scheduler.now
        let seconds = lastLevelsAt.map { Float(now - $0) / 1_000_000 } ?? 0
        lastLevelsAt = now
        onLevels?(Self.meterLevels(levels), seconds)
    }

    /// The only thing here that wakes the Mac by a timer, and only while audio streams and
    /// the meters can be seen.
    private func updatePolling() {
        let want = wantsLevels && pollHz != nil
        if want, pollTimer == nil, let hz = pollHz {
            let t = Timer(timeInterval: 1 / hz, repeats: true) { [weak self] _ in self?.pollLevels() }
            t.tolerance = 0.2 / hz
            RunLoop.main.add(t, forMode: .common)
            pollTimer = t
        } else if !want, let t = pollTimer {
            t.invalidate()
            pollTimer = nil
        }
    }

    static func decibels(_ peak: UInt32) -> Float {
        peak == 0 ? -Float.infinity : 20 * log10(Float(peak) / Float(PANEL_LEVEL_FULL_SCALE))
    }

    /// The helper's channels as the meters' groups. The computer's inputs 1 to 10 are the
    /// INPUT strips; its inputs 11 and 12, where the unit has them (not at 192 kHz), carry the
    /// monitor mix and feed the MONITOR OUT meters; what the computer plays feeds OUTPUT.
    static func meterLevels(_ l: HelperLevels) -> [[Float]] {
        var out: [[Float]] = [
            Array(repeating: -Float.infinity, count: 2),
            Array(repeating: -Float.infinity, count: 10),
            Array(repeating: -Float.infinity, count: 10),
        ]
        for c in 0..<min(10, l.capture.count) { out[ChannelNames.groupInput][c] = decibels(l.capture[c]) }
        if l.capture.count >= 12 {
            out[ChannelNames.groupMonitor][0] = decibels(l.capture[10])
            out[ChannelNames.groupMonitor][1] = decibels(l.capture[11])
        }
        for c in 0..<min(10, l.playback.count) { out[ChannelNames.groupOutput][c] = decibels(l.playback[c]) }
        return out
    }
}
