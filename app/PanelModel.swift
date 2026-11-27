// The control panel's model: it owns the engine, turns what the windows ask
// for into engine calls, and publishes one plain picture for them to draw.
// The windows hold no state of the unit and never call the engine themselves,
// so the test drives exactly what a click or a key drives.
import Foundation
import Combine

struct ChannelState: Equatable {
    var on = true
    var pan: UInt32 = 0
    var volume: UInt32 = 0
    var exists = true // false for mixer channels 7 to 10 at 192 kHz
}

enum LinkState: Int { case offline = 0, syncing, ready, silent }

/// Everything the windows show about the unit, copied out of the engine after each call.
struct PanelSnapshot: Equatable {
    var state = LinkState.offline
    var writesAllowed = false
    var memoryAllowed = false
    var busy = false     // changes on their way or not checked yet
    var savingHeld = false // the unit's saving was paused by itself and not resumed yet
    var settling = false // a front-panel switch moved; a fresh read follows

    var rateHz: UInt32 = 0
    var rateCode = 0
    var clockSource = 0
    var clockStatus = 0
    var mono = false
    var softControl = false

    var channels: [[ChannelState]] = [
        Array(repeating: ChannelState(), count: 2),
        Array(repeating: ChannelState(), count: 10),
        Array(repeating: ChannelState(), count: 10),
    ]
    var links: [[Bool]] = [[false], Array(repeating: false, count: 5), Array(repeating: false, count: 5)]
    /// A group's solo as the unit holds it: 0 for none, or the soloed channel's number 1 to 10.
    var soloRaw = [0, 0, 0]
    /// The SOLO lamps: the group's soloed channel, and its partner where the pair is linked.
    var soloed: [[Bool]] = [
        Array(repeating: false, count: 2),
        Array(repeating: false, count: 10),
        Array(repeating: false, count: 10),
    ]
    var patch: [UInt32] = Array(repeating: 0, count: 5)
    var volumeCeiling: UInt32 = 0

    var readsSent: UInt64 = 0
    var setsSent: UInt64 = 0
    var memorySent: UInt64 = 0
    var disagreements: UInt64 = 0

    /// The settings are known (possibly stale, when the unit stopped answering).
    var known: Bool { state == .ready || state == .silent }
    /// The red USB lamp: the unit is there and answers (its settings read, or being read).
    var unitAnswers: Bool { state == .ready || state == .syncing }
    /// The mixer's controls do something: the unit answers and Soft Control is in.
    var mixerLive: Bool { state == .ready && softControl }
    /// The unit answers and its SOFT CTRL switch is out: the mixer is bypassed.
    var softControlOff: Bool { known && state == .ready && !softControl }
}

/// Why the last action wasn't done (or was done differently). Shown until the next action.
struct Refusal: Equatable {
    enum Kind {
        case readOnly, softControl, notReady, noChannel, busy, ticket, value, held, pretendOnly
        case notPassed  // the UA-101: not one of the changes this version's helper passes on
        case helper     // the driver's helper refused a change
    }
    var kind: Kind
    var what: String
    var text: String
}

/// The unit kept another value than the one sent. Shown until dismissed.
struct Disagreement: Equatable {
    var kind: Int
    var group: Int
    var index: Int
    var sent: UInt32
    var unit: UInt32
    var count: UInt64
    var title: String
    var text: String

    func touches(_ s: StripID, kind k: Int32) -> Bool {
        guard Int32(kind) == k, group == s.group else { return false }
        return Int32(kind) == Int32(PANEL_F_LINK) ? index == s.channel / 2 : index == s.channel
    }
}

struct MemoryCommand: Equatable {
    /// The numbers are the shim's PANEL_MEM_ ones. `resetCurrent` is Roland's "Current
    /// Setting": the settings in use go back to the factory ones, the memories stay.
    enum Kind: Int { case resetAll = 0, resetMemory, store, load, resetCurrent }
    var kind: Kind
    var memory = 0       // 0 to 4
    var patchBay = true  // Load and Store only
    var mixer = true

    var title: String {
        switch kind {
        case .resetAll: return "Reset everything"
        case .resetCurrent: return "Reset the settings in use"
        case .resetMemory: return "Reset \(ChannelNames.memories[memory])"
        case .store: return "Save to \(ChannelNames.memories[memory])"
        case .load: return "Recall \(ChannelNames.memories[memory])"
        }
    }

    /// The parts a Load or a Store names, for a sentence.
    var parts: String {
        switch (patchBay, mixer) {
        case (true, true): return "the patch bay and the monitor mixer"
        case (true, false): return "the patch bay"
        case (false, true): return "the monitor mixer"
        default: return "nothing"
        }
    }

    /// The command's bit in a helper's list of what it passes on (PANEL_PASS_MEM_).
    var bit: UInt64 {
        guard (0..<5).contains(memory) else { return 0 }
        switch kind {
        case .load: return PANEL_PASS_MEM_LOAD_0 << UInt64(memory)
        case .store: return PANEL_PASS_MEM_STORE_0 << UInt64(memory)
        case .resetMemory: return PANEL_PASS_MEM_RESET_0 << UInt64(memory)
        case .resetCurrent: return PANEL_PASS_MEM_RESET_CURRENT
        case .resetAll: return PANEL_PASS_MEM_RESET_ALL
        }
    }

    /// It replaces settings in use (a Load, a Reset of them or of everything); a Store and a
    /// Reset of one memory change only a memory.
    var changesCurrent: Bool { kind == .load || kind == .resetCurrent || kind == .resetAll }

    /// The one message it is, for the words on screen; empty for a command that doesn't exist.
    var bytes: [UInt8] {
        var out = [UInt8](repeating: 0, count: 18)
        let n = panel_memory_message(Int32(kind.rawValue), UInt32(max(0, memory)), patchBay ? 1 : 0, mixer ? 1 : 0, &out)
        return Array(out[0..<n])
    }

    /// What it does to the unit, in plain words, for the confirmation step.
    var consequence: String {
        switch kind {
        case .resetAll:
            return "The unit goes back to its factory settings: the patch bay, the monitor mixer and all five memories. What is saved in the memories is lost."
        case .resetCurrent:
            return "The settings in use now, the patch bay and the monitor mixer, go back to the factory settings. The five memories aren't touched."
        case .resetMemory:
            return "\(ChannelNames.memories[memory]) on the unit is erased and set back to the factory settings. The settings in use now don't change."
        case .store:
            return "\(ChannelNames.memories[memory]) on the unit is overwritten with \(parts) as they are now. What was saved there is lost."
        case .load:
            return "\(parts.prefix(1).uppercased() + parts.dropFirst()) in use now \(patchBay && mixer ? "are" : "is") replaced by what \(ChannelNames.memories[memory]) holds. What is in use now is overwritten."
        }
    }

    /// The warning before each Recall, Save and Reset (a design choice of 2026-11-21, when the
    /// go-ahead locks went): said in the confirmation, under what will be overwritten.
    static let cantBeUndone = "This can't be undone, except from a backup."
    /// What is saved first and what is in no backup, for the same warning.
    var backupWords: String {
        let first = "A backup of the settings in use now is saved first, by itself"
        switch kind {
        case .load, .resetCurrent:
            return first + "; to go back: Device menu, Open Backup. Turn the monitors down and take headphones off first: what you hear can change at once."
        case .resetAll:
            return first + "; to go back: Device menu, Open Backup. What the five memories hold is in no backup, unless you recalled each one earlier (a Recall saves what the memory held as a file). Turn the monitors down and take headphones off first: what you hear can change at once."
        case .store, .resetMemory:
            return first + ". What \(ChannelNames.memories[memory]) holds now is in no backup, unless you recalled it earlier (a Recall saves what the memory held as a file)."
        }
    }

    var isValid: Bool { kind == .resetAll || kind == .resetCurrent || kind == .resetMemory || patchBay || mixer }
}

struct PendingMemory: Equatable {
    var command: MemoryCommand
    var ticket: UInt32
    var expires: UInt64
    var bytes: [UInt8]
}

enum PanelKey {
    case cursorLeft, cursorRight
    case volumeUp, volumeDown, volumeUpBig, volumeDownBig
    case panLeft, panRight, panCenter
    case toggleMonitor, toggleSolo, toggleLink
}

final class PanelModel: ObservableObject {
    @Published private(set) var snap = PanelSnapshot()
    @Published var cursor = StripID(group: ChannelNames.groupInput, channel: 0)
    @Published private(set) var refusal: Refusal?
    /// The end-user build (a design choice of 2026-11-21): with the UA-101,
    /// changes switch on by themselves as soon as its settings are read. Up to link version 17
    /// that also took the user's go-ahead file; since version 18 there is none (the user's
    /// choice of 2026-11-21). The developer build keeps the Allow Changes switch, which is the
    /// app's own and off at the start; the self-test sets this to try both.
    var changesByThemselves = Build.isEndUser
    /// The user's Lock Changes (Device menu, end-user build): changes stay off until unlocked.
    @Published private(set) var changesLocked = false
    @Published private(set) var disagreement: Disagreement?
    @Published private(set) var pendingMemory: PendingMemory?
    /// Goes up each time a memory's block comes for the preview, or reading it fails, or the
    /// blocks read are forgotten (PANEL_EV_MEMORY): the dialog then asks `memoryPreview` again.
    @Published private(set) var previewChanged = 0
    /// One line about the last Recall, Save or Reset: sent, cancelled, ran out.
    @Published private(set) var memoryNote: String?
    /// What the last pause or resume of the unit's saving sent, and what the unit's settings
    /// read back as afterwards.
    @Published private(set) var savingNote: String?
    /// What the last patch bay change sent to the UA-101, and what its settings read back as.
    /// Only for the real unit: there each change is a measurement.
    @Published private(set) var patchNote: String?
    /// What the last volume change sent to the UA-101, and what its settings read back as.
    @Published private(set) var volumeNote: String?
    /// What the last pan change sent to the UA-101, and what its settings read back as.
    @Published private(set) var panNote: String?
    /// What the last on/off change sent to the UA-101, and what its settings read back as.
    @Published private(set) var onNote: String?
    /// What the last stereo link change sent to the UA-101, and what its settings read back as.
    @Published private(set) var pairNote: String?
    /// What the last solo change sent to the UA-101, and what its settings read back as.
    @Published private(set) var soloNote: String?
    /// Hard Thru was asked for and waits for its second step (the patch bay window shows what
    /// it does and asks). Nothing is sent before `confirmHardThru`.
    @Published private(set) var hardThruAsked = false
    /// A fader or a pan knob is held with the mouse. The window keeps its banners' height
    /// meanwhile, so the mixer doesn't move under the hand (MixerViews.swift).
    @Published private(set) var dragging = false
    /// Every change that leaves for the real unit, and what came back, in plain lines: the
    /// app writes them to a file (PanelApp.swift). Nothing in the tests or the pictures.
    var onRecord: ((String) -> Void)?

    /// The unit the panel talks to now: the pretend one, or the UA-101 through the helper.
    @Published private(set) var transport: UnitTransport
    /// Why there is no unit, when the transport says (the helper isn't installed, the unit is off).
    @Published private(set) var linkNote: String?
    /// The device buttons: true asks for the real unit. Whoever owns both transports sets it.
    var onChooseDevice: ((Bool) -> Void)?
    let scheduler: PanelScheduler
    /// Level meters. Not the unit's state and not the engine's: see Meters.swift.
    let meters = MeterModel()

    private var engine: OpaquePointer!
    private var events: UInt32 = 0
    private var wakeID: Int?
    private var keyHoldID: Int?
    private var ticketID: Int?
    private var dragDepth = 0
    private var keyChanging = false // inside a key's change: it is one of a run, as a drag's values are
    /// The channel a held up key stopped at 0 dB, while that key is held (2026-11-27: a held ↑
    /// went on past 0 dB, one 0.5 dB repeat after the other). Its repeats stay at 0 dB; a fresh
    /// press goes on, as a fader pushed again does.
    private var keyStoppedAt: StripID?
    /// A pause or resume of the saving whose read-back hasn't come yet.
    private var savingWatch: SavingWatch?
    /// A patch bay change to the UA-101 whose read-back hasn't come yet.
    private var patchWatch: PatchWatch?
    /// A volume change to the UA-101 (one value, or a drag's many) whose read-back hasn't come yet.
    private var volumeWatch: ValueWatch?
    /// The same for a pan.
    private var panWatch: ValueWatch?
    /// An on/off change to the UA-101 whose read-back hasn't come yet.
    private var onWatch: OnWatch?
    /// A link change to the UA-101 whose read-back hasn't come yet.
    private var linkWatch: LinkWatch?
    /// A solo change to the UA-101 whose read-back hasn't come yet.
    private var soloWatch: SoloWatch?
    /// Hard Thru or Initial Setting sent to the UA-101, its read-back not come yet.
    private var presetWatch: PresetWatch?
    /// Messages that change the unit and were handed to the helper since the last engine
    /// call was noted: lines for the record, so it shows what really left and when.
    private var handed: [String] = []

    /// The two mixer values the UA-101 is sent a channel at a time, each one read back.
    enum ValueKind {
        case volume, pan
        var word: String { self == .volume ? "volume" : "pan" }
    }

    private struct ValueWatch {
        var kind: ValueKind
        var strip: StripID
        var value: UInt32   // the last value asked for
        var asked = 1       // values asked for since `before`
        var before: [UInt8]
        var blocks: UInt64
        var others: UInt64
        var sets: UInt64    // messages that set something, sent before this began
        /// The other channel of a linked pair: the engine sends it the same volume, so its
        /// bytes are expected to move too.
        var partner: StripID?
    }

    /// How things stood just before a value was handed to the engine.
    private struct ValueMark {
        var before: [UInt8]
        var blocks: UInt64
        var others: UInt64
        var waiting: UInt32
        var sets: UInt64
    }

    private struct PatchWatch {
        var what: String    // "Patch bay, OUTPUT 1-2 to INPUT 1-2"
        var output: Int
        var source: UInt32
        var sent: [UInt8]
        var before: [UInt8]
        var blocks: UInt64
        var others: UInt64
    }

    private struct OnWatch {
        var strip: StripID
        var bit: Int        // the bit asked for, 0 or 1
        var sent: [UInt8]
        var before: [UInt8]
        var blocks: UInt64
        var others: UInt64
        /// The other channel of a linked pair: the engine sends it the same bit.
        var partner: StripID?
    }

    private struct LinkWatch {
        var group: Int
        var pair: Int
        var bit: Int        // the bit asked for: 1 linked, 0 not
        var sent: [UInt8]
        var before: [UInt8]
        var blocks: UInt64
        var others: UInt64
    }

    private struct SoloWatch {
        var group: Int
        var value: Int      // the number asked for: 0 none, or the channel 1 to 10
        var sent: [UInt8]
        var before: [UInt8]
        var blocks: UInt64
        var others: UInt64
    }

    private struct PresetWatch {
        var hardThru: Bool
        var sent: [[UInt8]] // one message for each row that had another source
        var before: [UInt8]
        var blocks: UInt64
        var others: UInt64
    }

    private struct SavingWatch {
        var resume: Bool
        var before: [UInt8]
        var blocks: UInt64
        var others: UInt64
    }

    static let keyStepDB = 0.5, keyBigStepDB = 3.0
    static let keyPanStep = 0x0400 // 1/16 of the way from centre to a side
    static let keyHoldMicros: UInt64 = 800_000
    /// The shortest time between two messages to the UA-101: the helper passes on 20 a second
    /// at most and refuses the rest, so a drag sends at most 17. The
    /// pretend unit keeps the engine's own 5 ms.
    static let unitGapMicros: UInt64 = 60_000
    /// After a Recall, Save or Reset: how long before the settings are first read back, and how
    /// long between that read and the next (they are read until two reads agree, at most
    /// `memoryMaxReads` times). The real unit gets longer than the engine's own half second:
    /// nothing shows yet how long it needs (measured on the unit).
    static let memorySettleUnit: UInt64 = 1_500_000, memorySettlePretend: UInt64 = 500_000
    static let memoryRereadUnit: UInt64 = 1_000_000, memoryRereadPretend: UInt64 = 500_000
    static let memoryMaxReads = 6
    /// The engine takes a memory command only a moment after its last message: tried again.
    static let commandRetryMicros: UInt64 = 100_000

    init(transport: UnitTransport, scheduler: PanelScheduler) {
        self.transport = transport
        self.scheduler = scheduler
        engine = makeEngine()
        if !transport.isPretend { panel_set_min_gap(engine, Self.unitGapMicros) }
        panel_set_memory_settle(engine, transport.isPretend ? Self.memorySettlePretend : Self.memorySettleUnit)
        bind()
    }

    /// A new engine: read-only, knowing nothing, with nothing waiting to be sent.
    private func makeEngine() -> OpaquePointer {
        let me = Unmanaged.passUnretained(self).toOpaque()
        return panel_engine_new({ ctx, msg, len in
            guard let ctx = ctx, let msg = msg else { return 0 }
            let model = Unmanaged<PanelModel>.fromOpaque(ctx).takeUnretainedValue()
            let bytes = UnsafeBufferPointer(start: msg, count: len)
            guard model.transport.send(bytes) else { return 0 }
            model.handedOver(bytes)
            return 1
        }, { ctx, bits in
            guard let ctx = ctx else { return }
            Unmanaged<PanelModel>.fromOpaque(ctx).takeUnretainedValue().events |= bits
        }, me)
    }

    /// A message was taken by the way to the unit. For the UA-101, one that changes the unit
    /// gets a line in the record: only these lines say what left, and when. (The helper can
    /// still refuse it; its own log is the proof of what reached the cable.)
    private func handedOver(_ bytes: UnsafeBufferPointer<UInt8>) {
        guard !transport.isPretend, panel_is_read_request(bytes.baseAddress, bytes.count) == 0,
              panel_memory_read_number(bytes.baseAddress, bytes.count) < 0 else { return }
        handed.append("  handed to the helper: " + Self.hex(Array(bytes)))
    }

    private func bind() {
        transport.onMessage = { [weak self] bytes in self?.receive(bytes) }
        transport.onLink = { [weak self] up, note in
            guard let self = self else { return }
            self.linkNote = note
            if up { self.connect() } else {
                self.meters.midiDark()
                self.disconnect()
            }
        }
        transport.onLevels = { [weak self] fresh, seconds in
            guard let self = self else { return }
            if let fresh = fresh {
                self.meters.feed(fresh, after: seconds, from: HelperTransport.levelsName)
            } else if self.meters.source == HelperTransport.levelsName {
                self.meters.clear(midi: false)
            }
        }
        // MIDI passing the unit's MIDI sockets: the lamps blink, with or without audio.
        transport.onMIDI = { [weak self] i, o in
            guard let self = self else { return }
            self.meters.blinkMIDI(in: i, out: o, scheduler: self.scheduler)
        }
        transport.onRefused = { [weak self] why in self?.helperRefused(why) }
        // Sound started or stopped: one of the reasons the dialogs give for not sending a
        // Recall, Save or Reset. It is the transport's, not a published value, so the
        // dialogs are told here, or Continue stayed grey after the sound stopped.
        transport.onSound = { [weak self] _ in self?.objectWillChange.send() }
    }

    private func unbind() {
        transport.onMessage = nil
        transport.onLink = nil
        transport.onLevels = nil
        transport.onMIDI = nil
        transport.onRefused = nil
        transport.onSound = nil
    }

    deinit {
        if let id = wakeID { scheduler.cancel(id) }
        if let id = keyHoldID { scheduler.cancel(id) }
        if let id = ticketID { scheduler.cancel(id) }
        if let id = waitID { scheduler.cancel(id) }
        unbind()
        panel_engine_free(engine)
    }

    /// Changes the unit the panel talks to. The old engine is thrown away with everything it
    /// knew, was allowed and still had waiting, so nothing meant for one unit can reach the
    /// other, and the panel is read-only again.
    func use(_ new: UnitTransport) {
        guard new !== transport else { return }
        finish() // what waits goes to the unit it was meant for, and that unit may save again
        panel_transport(engine, 0, scheduler.now)
        transport.stop()
        unbind()
        if let id = wakeID { scheduler.cancel(id); wakeID = nil }
        if let id = ticketID { scheduler.cancel(id); ticketID = nil }
        panel_engine_free(engine)
        engine = makeEngine()
        if !new.isPretend { panel_set_min_gap(engine, Self.unitGapMicros) }
        panel_set_memory_settle(engine, new.isPretend ? Self.memorySettlePretend : Self.memorySettleUnit)
        events = 0
        dragDepth = 0
        refusal = nil
        disagreement = nil
        pendingMemory = nil
        memoryNote = nil
        savingNote = nil
        savingWatch = nil
        patchNote = nil
        patchWatch = nil
        volumeNote = nil
        volumeWatch = nil
        panNote = nil
        panWatch = nil
        onNote = nil
        onWatch = nil
        pairNote = nil
        linkWatch = nil
        soloNote = nil
        soloWatch = nil
        presetWatch = nil
        job = nil
        jobEnded()
        offWanted = false
        pendingPutBack = nil
        backupNote = nil
        putBackNote = nil
        resetNote = nil
        hardThruAsked = false
        handed = []
        dragging = false
        linkNote = nil
        if meters.source == HelperTransport.levelsName { meters.clear() }
        meters.midiDark()
        transport = new
        bind()
        afterCall()
        new.start()
        if new.isPretend { connect() }
    }

    /// Messages the engine's last check stopped. Should stay 0: the setters refuse first.
    var guardBlocks: UInt64 { panel_guard_blocks(engine) }

    // MARK: the link

    func connect() {
        panel_transport(engine, 1, scheduler.now)
        afterCall()
    }

    /// The end-user build: the model is made with the UA-101 already, so the
    /// helper is asked for it once, as the app opens, as `use` would after a choice.
    func startUnit() {
        afterCall()
        transport.start()
        if transport.isPretend { connect() }
    }

    func disconnect() {
        panel_transport(engine, 0, scheduler.now)
        afterCall()
    }

    /// As after the unit was switched off and on.
    func reconnect() {
        disconnect()
        connect()
    }

    /// The pretend unit's "Stop answering" (its front panel window). The app only finds out
    /// that a unit is silent when it asks it something, so it asks at once: the USB lamp and
    /// the banner then follow within about 3 s (three unanswered reads), and come back as
    /// soon as it answers again. The real UA-101's going away the helper reports by itself.
    #if !UA101_USER || UA101_PICTURES
    func setPretendSilent(_ unit: PretendUnit, _ on: Bool) {
        unit.setSilent(on)
        if transport === unit { refresh() }
    }
    #endif

    func refresh() {
        // No way to the real unit right now: look for it again (the helper may have been
        // installed, or the unit switched on, since).
        if !transport.isPretend, snap.state == .offline {
            transport.stop()
            linkNote = nil
            transport.start()
            return
        }
        panel_refresh(engine, scheduler.now)
        afterCall()
    }

    /// Before quitting: sends what waits and lets the unit save again.
    func finish() {
        endKeyHold()
        panel_finish(engine, scheduler.now)
        afterCall()
    }

    private func receive(_ bytes: [UInt8]) {
        bytes.withUnsafeBufferPointer { panel_receive(engine, $0.baseAddress, $0.count, scheduler.now) }
        afterCall()
    }

    private func tick() {
        wakeID = nil
        panel_tick(engine, scheduler.now)
        afterCall()
    }

    /// After every engine call: pick up what changed and book the next wake-up.
    private func afterCall() {
        let bits = events
        events = 0
        if !handed.isEmpty {
            let lines = handed
            handed = []
            for line in lines { record(line) }
        }
        if bits & UInt32(PANEL_EV_DISAGREE) != 0 { readDisagreement() }
        if bits & UInt32(PANEL_EV_MEMORY) != 0 { previewChanged &+= 1 }
        let fresh = readSnapshot()
        if fresh != snap { snap = fresh }
        checkSaving()
        checkPatch()
        checkValue(.volume)
        checkValue(.pan)
        checkOn()
        checkLink()
        checkSolo()
        checkPreset()
        checkJob()
        if !snap.known, hardThruAsked { hardThruAsked = false }
        if !snap.known, pendingPutBack != nil { pendingPutBack = nil }
        if !snap.known, pendingIntoMemory != nil { pendingIntoMemory = nil }
        if !snap.known, pendingMemory != nil { dropPending(note: "The unit stopped answering. Nothing was sent.") }

        if let id = wakeID { scheduler.cancel(id); wakeID = nil }
        let deadline = panel_next_deadline(engine)
        if deadline != UInt64.max {
            wakeID = scheduler.schedule(at: deadline) { [weak self] in self?.tick() }
        }
        if changesByThemselves { switchOnByThemselves() }
    }

    // MARK: changes by themselves (the end-user build)

    /// With the UA-101 read and changes off (not locked): switches changes on, as Allow Changes
    /// would, for what the driver's helper says it passes on. Quiet: no refusal is shown, the
    /// record has it once.
    private func switchOnByThemselves() {
        guard !transport.isPretend, !changesLocked, snap.state == .ready, !snap.writesAllowed,
              !offWanted, !transport.allowed.isEmpty else { return }
        switchOn()
        record("Changes switched on by themselves. The driver's helper passes on: \(transport.allowed.words). Nothing else.")
        afterCall()
    }

    /// Both of the engine's switches on, always together: ordinary changes, and Load, Store
    /// and Reset. Which of them this way to the unit passes on is `transport.allowed`, asked
    /// at each command.
    private func switchOn() {
        transport.changesOn = true
        panel_allow_writes(engine, 1)
        panel_allow_memory(engine, 1)
    }

    /// Lock Changes (end-user build): changes off and kept off until unlocked.
    func lockChanges(_ lock: Bool) {
        guard lock != changesLocked else { return }
        changesLocked = lock
        if lock {
            record("Changes locked by the user (Device menu, Lock Changes).")
            allowChanges(false)
        } else {
            record("Changes unlocked by the user.")
            if changesByThemselves { switchOnByThemselves() } else { allowChanges(true) }
        }
    }

    /// How changes come back on, for the notes: the end-user app's way or the switch.
    var changesOffWords: String {
        guard changesByThemselves else { return "Changes are switched off (Device menu, Allow Changes)." }
        if changesLocked { return "Changes are locked (Device menu, Lock Changes)." }
        if snap.state == .ready, transport.allowed.isEmpty { return "Changes are off: the driver's helper that is installed passes on none." }
        return "Changes are off until the UA-101's settings are read; they then come on by themselves."
    }
    private var allowAgainWords: String { changesByThemselves ? "once changes are on again (they come back by themselves)" : "allow changes again" }

    private func readSnapshot() -> PanelSnapshot {
        var s = PanelSnapshot()
        s.state = LinkState(rawValue: Int(panel_state(engine))) ?? .offline
        s.writesAllowed = panel_writes_allowed(engine) != 0
        s.memoryAllowed = panel_memory_allowed(engine) != 0
        s.busy = panel_busy(engine) != 0
        s.savingHeld = panel_saving_held(engine) != 0
        s.settling = panel_settling(engine) != 0
        s.volumeCeiling = panel_volume_ceiling(engine)
        s.readsSent = panel_reads_sent(engine)
        s.setsSent = panel_sets_sent(engine)
        s.memorySent = panel_memory_sent(engine)
        s.disagreements = panel_disagreements(engine)
        guard s.known else { return s }
        s.rateHz = panel_rate_hz(engine)
        s.rateCode = Int(panel_rate_code(engine))
        s.clockSource = Int(panel_clock_source(engine))
        s.clockStatus = Int(panel_clock_status(engine))
        s.mono = panel_mono(engine) != 0
        s.softControl = panel_soft_control(engine) != 0
        for g in 0..<3 {
            let n = Int(panel_group_channels(UInt32(g)))
            for c in 0..<n {
                s.channels[g][c] = ChannelState(
                    on: panel_monitor_on(engine, UInt32(g), UInt32(c)) != 0,
                    pan: panel_pan(engine, UInt32(g), UInt32(c)),
                    volume: panel_volume(engine, UInt32(g), UInt32(c)),
                    exists: panel_channel_exists(engine, UInt32(g), UInt32(c)) != 0)
                s.soloed[g][c] = panel_soloed(engine, UInt32(g), UInt32(c)) != 0
            }
            for p in 0..<(n / 2) { s.links[g][p] = panel_link(engine, UInt32(g), UInt32(p)) != 0 }
            s.soloRaw[g] = Int(panel_solo_raw(engine, UInt32(g)))
        }
        for o in 0..<5 { s.patch[o] = panel_patch(engine, UInt32(o)) }
        return s
    }

    private func readDisagreement() {
        var kind: Int32 = 0, group: Int32 = 0, index: Int32 = 0
        var sent: UInt32 = 0, unit: UInt32 = 0
        panel_last_disagree(engine, &kind, &group, &index, &sent, &unit)
        let k = Int(kind), g = Int(group), i = Int(index)
        var name: String
        switch kind {
        case Int32(PANEL_F_VOLUME): name = ChannelNames.full(StripID(group: g, channel: i)) + " volume"
        case Int32(PANEL_F_PAN): name = ChannelNames.full(StripID(group: g, channel: i)) + " pan"
        case Int32(PANEL_F_ON): name = ChannelNames.full(StripID(group: g, channel: i)) + " mute"
        case Int32(PANEL_F_LINK): name = ChannelNames.pair(group: g, pair: i) + " link"
        case Int32(PANEL_F_SOLO): name = "\(ChannelNames.groupTitle[min(g, 2)]) solo"
        case Int32(PANEL_F_PATCH): name = "Patch bay, " + ChannelNames.outputPairs[min(i, 4)]
        default: name = "A setting"
        }
        if g >= 3 || i >= 10 { name = "A setting" }
        disagreement = Disagreement(
            kind: k, group: g, index: i, sent: sent, unit: unit, count: panel_disagreements(engine),
            title: "The unit didn't take a change",
            text: "\(name): sent \(Format.value(kind: k, sent)), but the unit holds \(Format.value(kind: k, unit)). The unit's value is shown.")
    }

    func dismissDisagreement() { disagreement = nil }
    func dismissRefusal() { refusal = nil }

    // MARK: changes on and off

    /// Lets changes through, or makes the panel read-only again. With the UA-101 the changes
    /// the driver's helper says it passes on can then be made (`transport.allowed`), Load,
    /// Store and Reset among them, and no others. (Up to link version 17 the helper also had
    /// to find the user's go-ahead files; there are none any more.)
    func allowChanges(_ allow: Bool) {
        guard allow else {
            // A put back or a reset stopped half way would leave the unit with a mix of
            // settings and its saving paused: its last changes leave first (a few seconds).
            if let j = job, j.sending {
                offWanted = true
                refusal = Refusal(kind: .busy, what: "Switch changes off",
                                  text: "Changes switch off by themselves as soon as \(j.kind.title) has finished: its last changes are leaving now, a few seconds at most.")
                return
            }
            let was = snap.writesAllowed
            transport.changesOn = false // and with it the transport's switch for Recall, Save and Reset
            panel_allow_writes(engine, 0)
            panel_allow_memory(engine, 0)
            dropPending(note: nil)
            hardThruAsked = false
            refusal = nil
            if was { record("Changes switched off in the app.") }
            afterCall()
            return
        }
        if !transport.isPretend, snap.state != .ready {
            refusal = Refusal(kind: .notReady, what: "Allow changes",
                              text: "Allow changes: not done. The UA-101's settings aren't known right now.")
            return
        }
        guard !transport.allowed.isEmpty else {
            refusal = Refusal(kind: .notPassed, what: "Allow changes",
                              text: "Allow changes: not done. The driver's helper that is installed passes on no changes. Nothing was sent.")
            return
        }
        switchOn()
        refusal = nil
        record(transport.isPretend ? "Changes switched on (the pretend unit)."
                                   : "Changes switched on. The driver's helper passes on: \(transport.allowed.words). Nothing else.")
        afterCall()
    }

    /// The driver's helper didn't pass a change on: a Recall, Save or Reset against one of its
    /// rules, or the app sent something this helper never passes on. Read-only again, and the
    /// unit is read. (In the end-user app changes come back on by themselves after it.)
    private func helperRefused(_ why: String) {
        endChanges(
            refusal: Refusal(kind: .helper, what: "A change",
                             text: "The driver's helper refused a change and sent nothing to the unit: \(why)."
                                 + (changesByThemselves && !changesLocked ? " What the unit holds is shown." : " Changes are switched off again.")),
            line: "The helper refused a change: \(why). Changes switched off.", why: why)
    }

    /// Read-only again, and the unit is read: whatever was on its way is dropped. `why`: the
    /// helper's own reason, when it was the helper that refused.
    private func endChanges(refusal ended: Refusal, line: String, why: String? = nil) {
        panel_writes_refused(engine, scheduler.now)
        savingWatch = nil
        if let w = patchWatch {
            patchWatch = nil
            patchNote = "\(w.what): NOT SENT. The driver's helper refused it."
        }
        if let w = onWatch {
            onWatch = nil
            onNote = "\(ChannelNames.full(w.strip)) mute: the driver's helper refused a message. What the unit holds now is shown."
        }
        if let w = linkWatch {
            linkWatch = nil
            pairNote = "\(ChannelNames.pair(group: w.group, pair: w.pair)) link: the driver's helper refused the message. What the unit holds now is shown."
        }
        if let w = soloWatch {
            soloWatch = nil
            soloNote = "\(ChannelNames.groupTitle[w.group]) solo: the driver's helper refused the message. What the unit holds now is shown."
        }
        if let w = presetWatch {
            presetWatch = nil
            patchNote = "\(w.hardThru ? "Hard Thru" : "Initial Setting"): the driver's helper refused a message. What the unit holds now is shown."
        }
        if let j = job, j.sending {
            job = nil
            offWanted = false
            jobEnded()
            let reason = why.map { " (\($0))" } ?? ""
            let back = "The settings from before are in “\(j.saved)”."
            switch (j.kind, j.stage) {
            case (.memory, _):
                setJobNote(j.kind, "\(j.kind.title): NOT DONE. THE DRIVER'S HELPER REFUSED the command and sent nothing to the unit\(reason). The unit's settings are as they were; what it holds now is shown. \(back)")
            case (.intoMemory, .command):
                setJobNote(j.kind, "\(j.kind.title), “\(j.fileName)”: NOT SAVED. THE DRIVER'S HELPER REFUSED the Save and sent nothing to the unit\(reason). THE SETTINGS IN USE NOW HOLD THE FILE'S VALUES, not yours, and changes are switched off. To get yours back: \(allowAgainWords), then Device menu, Open Backup, with “\(j.saved)”.")
            case (.intoMemory, .changes):
                setJobNote(j.kind, "\(j.kind.title), “\(j.fileName)”: NOT SAVED. THE \(j.kind.noun) STOPPED part way: the driver's helper refused a message after about \(j.handed) of \(j.planned) changes had left\(reason). The unit may hold a mix of your settings and the file's, and changes are switched off. To get yours back: \(allowAgainWords), then Device menu, Open Backup, with “\(j.saved)”.")
            case (.intoMemory, .restore):
                setJobNote(j.kind, "\(j.kind.title), “\(j.fileName)”: \(j.stored ? "saved, but" : "not saved, and") THE PUT BACK OF YOUR SETTINGS STOPPED part way: the driver's helper refused a message after about \(j.handed) of \(j.planned) changes had left\(reason). The unit may hold a mix of the file's settings and yours. To get yours back: \(allowAgainWords), then Device menu, Open Backup, with “\(j.saved)”.")
            default:
                setJobNote(j.kind, "\(j.kind.title): THE \(j.kind.noun) STOPPED part way: the driver's helper refused a message after about \(j.handed) of \(j.planned) changes had left. The unit may hold a mix of the old and the new settings; what it holds now is shown. \(back)")
            }
        }
        pendingPutBack = nil
        pendingIntoMemory = nil
        hardThruAsked = false
        for kind in [ValueKind.volume, .pan] {
            guard let w = watch(kind) else { continue }
            setWatch(kind, nil)
            setNote(kind, "\(ChannelNames.full(w.strip)) \(kind.word): the driver's helper refused a message. What the unit holds now is shown.")
        }
        dropPending(note: nil)
        refusal = ended
        record(line)
        afterCall()
    }

    /// A kind of change, for the question "does the way to the unit pass this on?".
    enum Change {
        case saving, mixer
        case volume(StripID) // one channel's volume
        case pan(StripID)    // one channel's pan
        case on(StripID)     // one channel's on/off bit
        case link(Int, Int)  // one pair's stereo link: group, pair
        case solo(Int)       // one group's solo
        case patch(Int) // one output pair's source
        case patchAll   // Hard Thru, Initial Setting: all five
    }

    /// The changes the way to the unit passes on now: everything for the pretend unit, the
    /// helper's own list for the UA-101.
    var allowed: Allowed { transport.allowed }

    /// With the UA-101 and changes on, a change the helper doesn't pass on is refused here,
    /// before the engine, with a sentence: the transport and the helper would refuse it too.
    private func notPassed(_ change: Change, _ what: String) -> Bool {
        // While a put back or a reset is under way no other change is taken: it would land in
        // the middle of it, and its read-back would be held against the backup.
        if let j = job, j.kind != .save {
            refusal = Refusal(kind: .busy, what: what,
                              text: "\(what): not done. \(j.kind.title) is under way; try again when its message has come.")
            afterCall()
            return true
        }
        guard !transport.isPretend, snap.writesAllowed else { return false }
        let allowed = transport.allowed
        let ok: Bool
        switch change {
        case .saving: ok = allowed.saving
        case .mixer: ok = allowed.mixer
        case .volume(let strip): ok = allowed.volume(strip)
        case .pan(let strip): ok = allowed.pan(strip)
        case .on(let strip): ok = allowed.on(strip)
        case .link(let group, let pair): ok = allowed.link(group: group, pair: pair)
        case .solo(let group): ok = allowed.solo(group: group)
        case .patch(let output): ok = allowed.patch(output)
        case .patchAll: ok = allowed.allPatch
        }
        if ok { return false }
        refusal = Refusal(kind: .notPassed, what: what,
                          text: "\(what): not done. With the UA-101 this version changes only \(allowed.words). Nothing was sent.")
        afterCall()
        return true
    }

    private func record(_ line: String) {
        if !transport.isPretend { onRecord?(line) }
    }

    // MARK: the unit's saving, by itself

    static func hex(_ bytes: [UInt8]) -> String { bytes.map { String(format: "%02X", $0) }.joined(separator: " ") }
    static func savingMessage(resume: Bool) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: 13)
        panel_saving_message(resume ? 1 : 0, &bytes)
        return bytes
    }

    /// The unit's 256 settings bytes as the app holds them now, if it has read them (for the
    /// report for the developer). Nothing is asked of the unit.
    var settingsAsRead: [UInt8]? { snap.known ? unitBlock() : nil }

    private func unitBlock() -> [UInt8] {
        var block = [UInt8](repeating: 0, count: 256)
        panel_unit_block(engine, &block)
        return block
    }

    /// Pauses the unit's saving and nothing else, then reads the settings back.
    @discardableResult
    func pauseSaving() -> Int32 {
        let what = "Pause the unit's saving"
        if notPassed(.saving, what) { return Int32(PANEL_ERR_READ_ONLY) }
        let watch = SavingWatch(resume: false, before: unitBlock(), blocks: panel_blocks_received(engine),
                                others: panel_other_messages(engine))
        let r = panel_pause_saving(engine, scheduler.now)
        if r == Int32(PANEL_OK) {
            let bytes = Self.hex(Self.savingMessage(resume: false))
            savingWatch = watch
            savingNote = "Pause saving: sent \(bytes). Reading the settings back…"
            record("Pause saving: sent \(bytes)")
            record("  settings before: \(Self.hex(watch.before))")
        }
        return done(r, what)
    }

    /// Lets the unit save again, then reads the settings back.
    @discardableResult
    func resumeSaving() -> Int32 {
        let what = "Resume the unit's saving"
        guard snap.savingHeld else {
            refusal = Refusal(kind: .value, what: what, text: "\(what): not done. This app hasn't paused it.")
            afterCall()
            return Int32(PANEL_ERR_VALUE)
        }
        let watch = SavingWatch(resume: true, before: unitBlock(), blocks: panel_blocks_received(engine),
                                others: panel_other_messages(engine))
        let r = panel_resume_saving(engine, scheduler.now)
        if r == Int32(PANEL_OK) {
            let bytes = Self.hex(Self.savingMessage(resume: true))
            savingWatch = watch
            savingNote = "Resume saving: sent \(bytes). Reading the settings back…"
            record("Resume saving: sent \(bytes)")
            record("  settings before: \(Self.hex(watch.before))")
        }
        return done(r, what)
    }

    func clearSavingNote() { savingNote = nil }

    /// After every engine call: has the read-back for a pause or resume come? Whether the unit
    /// took the message can't be read from its settings; what can be seen is whether anything
    /// else changed, and whether the unit said something the app doesn't know.
    private func checkSaving() {
        guard let w = savingWatch else { return }
        if !snap.known {
            savingWatch = nil
            savingNote = (w.resume ? "Resume saving" : "Pause saving") + ": sent, but the unit stopped answering before the settings were read back."
            record("  " + savingNote!)
            return
        }
        guard panel_blocks_received(engine) > w.blocks else { return }
        if w.resume, snap.busy { return } // the resume itself hasn't gone out yet
        savingWatch = nil
        let after = unitBlock()
        let differing = (0..<256).filter { after[$0] != w.before[$0] }
        let others = panel_other_messages(engine) - w.others
        var text = (w.resume ? "Resume saving" : "Pause saving") + ": sent \(Self.hex(Self.savingMessage(resume: w.resume))). "
        if differing.isEmpty {
            text += "The settings read back the same as before, all 256 bytes."
        } else {
            let list = differing.prefix(8).map { String(format: "d[%d] %02X→%02X", $0, w.before[$0], after[$0]) }.joined(separator: ", ")
            text += "THE SETTINGS READ BACK DIFFERENT in \(differing.count) of 256 bytes: \(list)\(differing.count > 8 ? ", …" : "")."
        }
        text += others == 0 ? " The unit said nothing else." : " The unit also sent \(others) message\(others == 1 ? "" : "s") this app doesn't know."
        if !w.resume { text += " Saving stays paused until you resume it." }
        savingNote = text
        record("  settings after:  \(Self.hex(after))")
        record("  " + text)
    }

    // MARK: results

    @discardableResult
    private func done(_ result: Int32, _ what: String) -> Int32 {
        switch result {
        case Int32(PANEL_OK):
            refusal = nil
        case Int32(PANEL_CLAMPED):
            refusal = Refusal(kind: .held, what: what,
                              text: "\(what): held at \(Format.dB(snap.volumeCeiling)) dB, the highest volume allowed for now.")
        case Int32(PANEL_ERR_READ_ONLY):
            refusal = Refusal(kind: .readOnly, what: what,
                              text: "\(what): not done. The panel is read-only, so nothing is sent to the unit." + (changesByThemselves ? " " + changesOffWords : ""))
        case Int32(PANEL_ERR_SOFT_CTRL):
            refusal = Refusal(kind: .softControl, what: what,
                              text: "\(what): not done. SOFT CTRL is out on the unit, so the unit bypasses this mixer.")
        case Int32(PANEL_ERR_NOT_READY):
            refusal = Refusal(kind: .notReady, what: what,
                              text: "\(what): not done. The unit's settings aren't known right now.")
        case Int32(PANEL_ERR_NO_CHANNEL):
            refusal = Refusal(kind: .noChannel, what: what,
                              text: "\(what): not done. The mixer doesn't have this channel at this sample rate.")
        case Int32(PANEL_ERR_BUSY):
            refusal = Refusal(kind: .busy, what: what,
                              text: "\(what): not done. Earlier changes are still on their way; try again in a moment.")
        case Int32(PANEL_ERR_TICKET):
            refusal = Refusal(kind: .ticket, what: what,
                              text: "\(what): not done. The confirmation was used, cancelled or ran out.")
        default:
            refusal = Refusal(kind: .value, what: what, text: "\(what): not done. The value isn't one the unit accepts.")
        }
        afterCall()
        return result
    }

    // MARK: the mixer

    static func volumeMessage(_ s: StripID, _ volume: UInt32) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: 18)
        panel_volume_message(UInt32(s.group), UInt32(s.channel), volume, &bytes)
        return bytes
    }

    /// Where a channel's volume lies in the settings block: six bytes, a digit each.
    static func volumeOffset(_ s: StripID) -> Int { [0x74, 0x80, 0xBC][s.group] + 6 * s.channel }
    static func volumeDigits(_ v: UInt32) -> [UInt8] { (0..<6).map { UInt8((v >> UInt32(20 - 4 * $0)) & 0xF) } }
    static func volumeWords(_ v: UInt32) -> String { String(format: "%@ dB (0x%06X)", Format.dB(v), v) }

    static func panMessage(_ s: StripID, _ pan: UInt32) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: 16)
        panel_pan_message(UInt32(s.group), UInt32(s.channel), pan, &bytes)
        return bytes
    }

    /// Where a channel's pan lies in the settings block: four bytes, a digit each. INPUT 1's
    /// is d[36] to d[39]; MONITOR OUT has none.
    static func panOffset(_ s: StripID) -> Int { [0, 0x24, 0x4C][s.group] + 4 * s.channel }
    static func panDigits(_ v: UInt32) -> [UInt8] { (0..<4).map { UInt8((v >> UInt32(12 - 4 * $0)) & 0xF) } }
    static func panWords(_ v: UInt32) -> String { String(format: "%@ (0x%04X)", Format.pan(v), v) }

    private static func offset(_ k: ValueKind, _ s: StripID) -> Int { k == .volume ? volumeOffset(s) : panOffset(s) }
    private static func digits(_ k: ValueKind, _ v: UInt32) -> [UInt8] { k == .volume ? volumeDigits(v) : panDigits(v) }
    private static func words(_ k: ValueKind, _ v: UInt32) -> String { k == .volume ? volumeWords(v) : panWords(v) }
    private static func message(_ k: ValueKind, _ s: StripID, _ v: UInt32) -> [UInt8] { k == .volume ? volumeMessage(s, v) : panMessage(s, v) }

    private func watch(_ k: ValueKind) -> ValueWatch? { k == .volume ? volumeWatch : panWatch }
    private func setWatch(_ k: ValueKind, _ w: ValueWatch?) { if k == .volume { volumeWatch = w } else { panWatch = w } }
    private func setNote(_ k: ValueKind, _ text: String?) { if k == .volume { volumeNote = text } else { panNote = text } }

    private func mark() -> ValueMark {
        ValueMark(before: unitBlock(), blocks: panel_blocks_received(engine), others: panel_other_messages(engine),
                  waiting: panel_unchecked(engine), sets: panel_sets_sent(engine))
    }

    /// The other channel of a pair the unit holds linked, or nil. The engine sends a volume
    /// and an on/off to both channels of such a pair (the unit's MONITOR OUT and the
    /// computer's five pairs are linked as delivered); a pan goes to the one channel only.
    func linkedPartner(_ s: StripID) -> StripID? {
        guard s.group < snap.links.count, s.channel / 2 < snap.links[s.group].count,
              snap.links[s.group][s.channel / 2] else { return nil }
        return StripID(group: s.group, channel: s.channel ^ 1)
    }

    /// One channel's volume. The UA-101 is sent any value up to the top of its range
    /// (0x7FFFFF, about +12 dB) as it is (the helper passes on no higher one; up to
    /// 2026-11-27 the top was 0 dB); with `exact` a value above that is refused instead of held. The change and what the settings read back as
    /// afterwards are noted and written to the record, as for the patch bay. In a linked pair
    /// both channels' messages must be ones the helper passes on, or nothing is sent.
    @discardableResult
    func setVolume(_ s: StripID, _ volume: UInt32, exact: Bool = false) -> Int32 {
        let what = ChannelNames.full(s) + " volume"
        if notPassed(.volume(s), what) { return Int32(PANEL_ERR_READ_ONLY) }
        if let p = linkedPartner(s), notPassed(.volume(p), what + " (linked with \(ChannelNames.full(p)))") {
            return Int32(PANEL_ERR_READ_ONLY)
        }
        let v = min(volume, UInt32(PANEL_MAX_VOLUME))
        if !transport.isPretend, exact, panel_volume_sendable(volume) == 0 {
            refusal = Refusal(kind: .value, what: what,
                              text: "\(what) to \(Self.volumeWords(volume)): not done. The UA-101 is never sent a volume above the top of its range (+12 dB). Nothing was sent.")
            afterCall()
            return Int32(PANEL_ERR_VALUE)
        }
        let before = mark()
        let r = panel_set_volume(engine, UInt32(s.group), UInt32(s.channel), v, scheduler.now)
        if !transport.isPretend, r == Int32(PANEL_OK) || r == Int32(PANEL_CLAMPED) {
            noteValue(.volume, s, min(v, snap.volumeCeiling), before)
        }
        return done(r, what)
    }

    /// A value went to the engine for the UA-101: the first of a change opens a watch for the
    /// read-back, a later one only moves what is asked for. The first line in the record says
    /// what was asked for and no bytes: in a drag a newer value can take its place before it
    /// leaves (it did on the unit, 2026-11-12). What leaves is on the "handed to the helper"
    /// lines, written as each message goes.
    private func noteValue(_ kind: ValueKind, _ s: StripID, _ sent: UInt32, _ m: ValueMark) {
        let what = ChannelNames.full(s) + " " + kind.word
        if var w = watch(kind), w.strip == s {
            w.value = sent
            w.asked += 1
            setWatch(kind, w)
        } else if watch(kind) == nil, panel_unchecked(engine) == m.waiting {
            // The engine sends nothing for a value the unit already holds.
            let text = "\(what) to \(Self.words(kind, sent)): the unit already has that value. Nothing was sent."
            setNote(kind, text)
            record(text)
        } else {
            if let old = watch(kind) { record("  \(ChannelNames.full(old.strip)) \(kind.word): another \(kind.word) was changed before its read-back came.") }
            setWatch(kind, ValueWatch(kind: kind, strip: s, value: sent, before: m.before, blocks: m.blocks, others: m.others, sets: m.sets,
                                      partner: kind == .volume ? linkedPartner(s) : nil))
            let held = dragging || keyHoldID != nil || keyChanging
            setNote(kind, "\(what): \(Self.words(kind, sent)) asked for. Reading the settings back…")
            record("\(what): \(Self.words(kind, sent)) asked for" + (held ? ", the first value of a drag or a run of keys (a later value may leave in its place)" : ""))
            record("  settings before: \(Self.hex(m.before))")
        }
    }

    /// One exact value, once, with no drag around it: so one message leaves, and the unit's
    /// saving isn't paused (Device menu; the first volume ever sent to the UA-101).
    @discardableResult
    func setVolumeExact(_ s: StripID, _ volume: UInt32) -> Int32 {
        endKeyHold()
        return setVolume(s, volume, exact: true)
    }

    func clearVolumeNote() { volumeNote = nil }
    func clearPanNote() { panNote = nil }

    /// After every engine call: has the read-back for a volume or a pan change to the UA-101
    /// come? The channel's own bytes should be the only thing that moved, and hold the last
    /// value; for a volume in a linked pair, the partner's bytes too, and nothing else.
    private func checkValue(_ kind: ValueKind) {
        guard let w = watch(kind) else { return }
        let what = ChannelNames.full(w.strip) + " " + kind.word
        if !snap.known {
            setWatch(kind, nil)
            let text = "\(what): sent, but the unit stopped answering before the settings were read back."
            setNote(kind, text)
            record("  " + text)
            return
        }
        guard snap.writesAllowed else { // switched off, or the helper refused: nothing to wait for
            setWatch(kind, nil)
            return
        }
        guard panel_blocks_received(engine) > w.blocks, panel_unchecked(engine) == 0, !snap.busy else { return }
        setWatch(kind, nil)
        let after = unitBlock()
        let at = Self.offset(kind, w.strip)
        let want = Self.digits(kind, w.value)
        let span = at..<at + want.count
        let place = "d[\(at)…\(at + want.count - 1)]"
        let was = Array(w.before[span]), now = Array(after[span])
        let partnerSpan: Range<Int>? = w.partner.map { let p = Self.offset(kind, $0); return p..<p + want.count }
        let elsewhere = (0..<256).filter { after[$0] != w.before[$0] && !span.contains($0) && !(partnerSpan?.contains($0) ?? false) }
        let others = panel_other_messages(engine) - w.others
        let messages = panel_sets_sent(engine) - w.sets
        var text = "\(what) to \(Self.words(kind, w.value)): sent \(Self.hex(Self.message(kind, w.strip, w.value)))"
        if let p = w.partner {
            text += " and, for \(ChannelNames.full(p)), which is linked with it, \(Self.hex(Self.message(kind, p, w.value)))"
        }
        text += w.asked > 1 || messages > (w.partner == nil ? 1 : 2)
            ? ", the last of \(w.asked) value\(w.asked == 1 ? "" : "s") asked for; \(messages) messages left in all, with any pause and resume of the unit's saving. "
            : ". "
        if now == want {
            text += was == now ? "The unit already had that value (\(place) \(Self.hex(now)))."
                               : "The unit took it: \(place) \(Self.hex(was)) → \(Self.hex(now))."
        } else {
            text += "THE UNIT HOLDS ANOTHER VALUE: \(place) \(Self.hex(was)) → \(Self.hex(now)), wanted \(Self.hex(want))."
            if kind == .pan, let order = Self.panOrder(sent: want, held: now) {
                text += " THE UNIT READS A PAN'S FOUR DIGITS \(order). Send nothing more, and tell the user."
            }
        }
        if let p = w.partner, let ps = partnerSpan {
            let pwas = Array(w.before[ps]), pnow = Array(after[ps])
            let pplace = "d[\(ps.lowerBound)…\(ps.upperBound - 1)]"
            if pnow == want {
                text += pwas == pnow ? " \(ChannelNames.full(p)) already had that value (\(pplace) \(Self.hex(pnow)))."
                                     : " \(ChannelNames.full(p)) took it too: \(pplace) \(Self.hex(pwas)) → \(Self.hex(pnow))."
            } else {
                text += " THE LINKED PARTNER \(ChannelNames.full(p)) HOLDS ANOTHER VALUE: \(pplace) \(Self.hex(pwas)) → \(Self.hex(pnow)), wanted \(Self.hex(want))."
            }
        }
        if elsewhere.isEmpty {
            text += " Every other byte of the 256 read back the same."
        } else {
            let list = elsewhere.prefix(8).map { String(format: "d[%d] %02X→%02X", $0, w.before[$0], after[$0]) }.joined(separator: ", ")
            text += " OTHER SETTINGS READ BACK DIFFERENT, \(elsewhere.count) byte\(elsewhere.count == 1 ? "" : "s"): \(list)\(elsewhere.count > 8 ? ", …" : "")."
        }
        text += others == 0 ? " The unit said nothing else." : " The unit also sent \(others) message\(others == 1 ? "" : "s") this app doesn't know."
        setNote(kind, text)
        record("  settings after:  \(Self.hex(after))")
        record("  " + text)
    }

    /// If the unit holds a pan's four digits in another order than they were sent in, which
    /// one: the order is proven for a volume, not for a pan.
    static func panOrder(sent: [UInt8], held: [UInt8]) -> String? {
        guard sent.count == 4, held.count == 4, sent != held else { return nil }
        let orders: [(String, [Int])] = [
            ("BACKWARDS", [3, 2, 1, 0]),
            ("WITH THE TWO DIGITS OF EACH BYTE CHANGED OVER", [1, 0, 3, 2]),
            ("BACKWARDS, WITH THE TWO DIGITS OF EACH BYTE CHANGED OVER", [2, 3, 0, 1]),
        ]
        return orders.first { $0.1.map { sent[$0] } == held }?.0
    }

    /// One channel's pan. Both units take any value from 0x0000 to 0x8000 as it is (the
    /// order of a pan's four digits was read back from the UA-101 on 2026-11-12, the first
    /// sent the highest, so the knob moves smoothly); a value past the end is held at the end.
    /// With `exact` such a value is refused instead of held. Noted and read back as a volume is.
    @discardableResult
    func setPan(_ s: StripID, _ pan: UInt32, exact: Bool = false) -> Int32 {
        let what = ChannelNames.full(s) + " pan"
        if notPassed(.pan(s), what) { return Int32(PANEL_ERR_READ_ONLY) }
        if !transport.isPretend, exact, panel_pan_sendable(pan) == 0 {
            refusal = Refusal(kind: .value, what: what,
                              text: "\(what) to \(Self.panWords(pan)): not done. The UA-101 is never sent a pan past its end, 0x8000. Nothing was sent.")
            afterCall()
            return Int32(PANEL_ERR_VALUE)
        }
        let p = min(pan, UInt32(PANEL_MAX_PAN))
        let before = mark()
        let r = panel_set_pan(engine, UInt32(s.group), UInt32(s.channel), p, scheduler.now)
        if !transport.isPretend, r == Int32(PANEL_OK) { noteValue(.pan, s, p, before) }
        return done(r, what)
    }

    /// One exact pan, once, with no drag around it: one message, and the unit's saving isn't
    /// paused (Device menu; the first pan ever sent to the UA-101).
    @discardableResult
    func setPanExact(_ s: StripID, _ pan: UInt32) -> Int32 {
        endKeyHold()
        return setPan(s, pan, exact: true)
    }

    /// Where a channel's on/off bit lies in the settings block: a byte and a bit. INPUT 1's is
    /// d[30] bit 0 (group 0 starts at 0x1D, group 1 at 0x1E, group 2 at 0x21; four a byte).
    static func onPlace(_ s: StripID) -> (byte: Int, bit: Int) { ([0x1D, 0x1E, 0x21][s.group] + s.channel / 4, s.channel & 3) }
    static func onMessage(_ s: StripID, bit: Int) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: 13)
        panel_on_message(UInt32(s.group), UInt32(s.channel), UInt32(bit), &bytes)
        return bytes
    }
    /// The bit with its meaning: "bit 1 (mute on)". Bit 1 mutes: heard on INPUT 1, 2026-11-14.
    /// Since 1.1 the app calls the switch MUTE (the user's wish, 2026-11-27): lit, the channel
    /// is muted.
    func onWords(_ bit: Int) -> String {
        let muted = (bit != 0) == (panel_on_bit_is_mute(engine) != 0)
        return "bit \(bit) (mute \(muted ? "on" : "off"))"
    }

    /// One channel's monitor on/off. For the UA-101 this sets the block's bit, 0 or 1; which
    /// one means "off" was heard on INPUT 1 (2026-11-14: bit 1 mutes, as the engine takes it);
    /// the note and the record still give the bit beside the word. Read back as the patch bay is: the one bit should be the
    /// only thing that moved.
    @discardableResult
    func setMonitor(_ s: StripID, on: Bool) -> Int32 {
        let bit = on == (panel_on_bit_is_mute(engine) == 0) ? 1 : 0
        return setOnBit(s, bit, what: ChannelNames.full(s) + (on ? " mute off" : " mute on"))
    }

    /// The bit itself, 0 or 1, once, with no drag around it (Device menu; the first on/off
    /// ever sent to the UA-101).
    @discardableResult
    func setOnBitExact(_ s: StripID, _ bit: Int) -> Int32 {
        endKeyHold()
        return setOnBit(s, bit, what: ChannelNames.full(s) + " mute")
    }

    private func setOnBit(_ s: StripID, _ bit: Int, what: String) -> Int32 {
        if notPassed(.on(s), what) { return Int32(PANEL_ERR_READ_ONLY) }
        if let p = linkedPartner(s), notPassed(.on(p), what + " (linked with \(ChannelNames.full(p)))") {
            return Int32(PANEL_ERR_READ_ONLY)
        }
        let before = unitBlock(), blocks = panel_blocks_received(engine), others = panel_other_messages(engine)
        let waiting = panel_unchecked(engine)
        let r = panel_set_on_bit(engine, UInt32(s.group), UInt32(s.channel), Int32(bit), scheduler.now)
        if r == Int32(PANEL_OK), !transport.isPretend, onWatch == nil, panel_unchecked(engine) == waiting {
            // The engine sends nothing for a value the unit already holds.
            onNote = "\(ChannelNames.full(s)) \(onWords(bit)): the unit already has that bit. Nothing was sent."
            record(onNote!)
        } else if r == Int32(PANEL_OK), !transport.isPretend {
            if let old = onWatch { record("  \(ChannelNames.full(old.strip)) mute: another mute was changed before its read-back came.") }
            let sent = Self.onMessage(s, bit: bit)
            onWatch = OnWatch(strip: s, bit: bit, sent: sent, before: before, blocks: blocks, others: others, partner: linkedPartner(s))
            onNote = "\(ChannelNames.full(s)) \(onWords(bit)) asked for. Reading the settings back…"
            record("\(ChannelNames.full(s)) mute: \(onWords(bit)) asked for")
            record("  settings before: \(Self.hex(before))")
        }
        return done(r, what)
    }

    func clearOnNote() { onNote = nil }

    /// After every engine call: has the read-back for an on/off change to the UA-101 come? The
    /// channel's bit should be the only thing that moved in all 256 bytes; in a linked pair,
    /// the partner's bit too (it lies in the same byte), and nothing else.
    private func checkOn() {
        guard let w = onWatch else { return }
        let what = ChannelNames.full(w.strip) + " mute"
        if !snap.known {
            onWatch = nil
            onNote = "\(what): sent, but the unit stopped answering before the settings were read back."
            record("  " + onNote!)
            return
        }
        guard snap.writesAllowed else { // switched off, or the helper refused: nothing to wait for
            onWatch = nil
            return
        }
        guard panel_blocks_received(engine) > w.blocks, panel_unchecked(engine) == 0, !snap.busy else { return }
        onWatch = nil
        let after = unitBlock()
        let (at, bit) = Self.onPlace(w.strip)
        let mask = UInt8(1 << bit)
        let held = (after[at] & mask) != 0 ? 1 : 0
        let byte = String(format: "d[%d] %02X→%02X", at, w.before[at], after[at])
        // The partner of a linked pair: channels 2k and 2k+1 share a byte, four bits a byte.
        let partnerMask: UInt8 = w.partner.map { UInt8(1 << Self.onPlace($0).bit) } ?? 0
        var text = "\(ChannelNames.full(w.strip)) \(onWords(w.bit)) sent \(Self.hex(w.sent))"
        if let p = w.partner {
            text += " and, for \(ChannelNames.full(p)), which is linked with it, \(Self.hex(Self.onMessage(p, bit: w.bit)))"
        }
        text += ". "
        if held == w.bit {
            text += (w.before[at] & mask) == (after[at] & mask)
                ? "The unit already had that bit (\(byte), bit \(bit))."
                : "The unit took it: \(byte), bit \(bit)."
        } else {
            text += "THE UNIT HOLDS THE OTHER BIT: \(byte), bit \(bit) is \(held), wanted \(w.bit)."
        }
        if let p = w.partner {
            let pbit = Self.onPlace(p).bit
            let pheld = (after[at] & partnerMask) != 0 ? 1 : 0
            if pheld == w.bit {
                text += (w.before[at] & partnerMask) == (after[at] & partnerMask)
                    ? " \(ChannelNames.full(p)) already had that bit (bit \(pbit))."
                    : " \(ChannelNames.full(p)) took it too: bit \(pbit)."
            } else {
                text += " THE LINKED PARTNER \(ChannelNames.full(p)) HOLDS THE OTHER BIT: bit \(pbit) is \(pheld), wanted \(w.bit)."
            }
        }
        if (w.before[at] & ~(mask | partnerMask)) != (after[at] & ~(mask | partnerMask)) {
            text += String(format: " THE OTHER BITS OF d[%d] MOVED TOO (other channels' mute).", at)
        }
        let elsewhere = (0..<256).filter { after[$0] != w.before[$0] && $0 != at }
        if elsewhere.isEmpty {
            text += " Every other byte of the 256 read back the same."
        } else {
            let list = elsewhere.prefix(8).map { String(format: "d[%d] %02X→%02X", $0, w.before[$0], after[$0]) }.joined(separator: ", ")
            text += " OTHER SETTINGS READ BACK DIFFERENT, \(elsewhere.count) byte\(elsewhere.count == 1 ? "" : "s"): \(list)\(elsewhere.count > 8 ? ", …" : "")."
        }
        let others = panel_other_messages(engine) - w.others
        text += others == 0 ? " The unit said nothing else." : " The unit also sent \(others) message\(others == 1 ? "" : "s") this app doesn't know."
        onNote = text
        record("  settings after:  \(Self.hex(after))")
        record("  " + text)
    }

    @discardableResult
    func toggleMonitor(_ s: StripID) -> Int32 {
        setMonitor(s, on: !snap.channels[s.group][s.channel].on)
    }

    /// Where a pair's link bit lies in the settings block: a byte and a bit (MONITOR OUT L-R
    /// is d[24] bit 0; the INPUT pairs start at d[25], the OUTPUT pairs at d[27]; four a byte).
    static func linkPlace(group: Int, pair: Int) -> (byte: Int, bit: Int) { ([0x18, 0x19, 0x1B][group] + pair / 4, pair & 3) }
    static func linkMessage(group: Int, pair: Int, bit: Int) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: 13)
        panel_link_message(UInt32(group), UInt32(pair), UInt32(bit), &bytes)
        return bytes
    }
    static func linkWords(_ bit: Int) -> String { "bit \(bit) (\(bit != 0 ? "linked" : "not linked"))" }

    /// A pair's stereo link. One message and nothing with it, as Roland's own panel does: the
    /// two channels keep the volume and on/off each has, and the next change of either's
    /// volume or on/off goes to both. For the UA-101 the settings are read back: the one bit
    /// should be the only thing that moved.
    @discardableResult
    func setLink(group: Int, pair: Int, on: Bool) -> Int32 {
        let name = ChannelNames.pair(group: group, pair: pair)
        let what = name + (on ? " link" : " unlink")
        if notPassed(.link(group, pair), what) { return Int32(PANEL_ERR_READ_ONLY) }
        let bit = on ? 1 : 0
        let before = unitBlock(), blocks = panel_blocks_received(engine), others = panel_other_messages(engine)
        let waiting = panel_unchecked(engine)
        let r = panel_set_link(engine, UInt32(group), UInt32(pair), Int32(bit), scheduler.now)
        if r == Int32(PANEL_OK), !transport.isPretend, linkWatch == nil, panel_unchecked(engine) == waiting {
            // The engine sends nothing for a value the unit already holds.
            pairNote = "\(name) link to \(Self.linkWords(bit)): the unit already has that bit. Nothing was sent."
            record(pairNote!)
        } else if r == Int32(PANEL_OK), !transport.isPretend {
            if let old = linkWatch { record("  \(ChannelNames.pair(group: old.group, pair: old.pair)) link: another link was changed before its read-back came.") }
            linkWatch = LinkWatch(group: group, pair: pair, bit: bit, sent: Self.linkMessage(group: group, pair: pair, bit: bit),
                                  before: before, blocks: blocks, others: others)
            pairNote = "\(name) link to \(Self.linkWords(bit)) asked for. Reading the settings back…"
            record("\(name) link: \(Self.linkWords(bit)) asked for")
            record("  settings before: \(Self.hex(before))")
        }
        return done(r, what)
    }

    @discardableResult
    func toggleLink(group: Int, pair: Int) -> Int32 {
        setLink(group: group, pair: pair, on: !snap.links[group][pair])
    }

    func clearPairNote() { pairNote = nil }

    /// After every engine call: has the read-back for a link change to the UA-101 come? The
    /// pair's bit should be the only thing that moved in all 256 bytes. If the unit moved a
    /// volume or an on/off by itself to make the pair equal, that shows here, in capitals.
    private func checkLink() {
        guard let w = linkWatch else { return }
        let name = ChannelNames.pair(group: w.group, pair: w.pair)
        let what = name + " link"
        if !snap.known {
            linkWatch = nil
            pairNote = "\(what): sent, but the unit stopped answering before the settings were read back."
            record("  " + pairNote!)
            return
        }
        guard snap.writesAllowed else { // switched off, or the helper refused: nothing to wait for
            linkWatch = nil
            return
        }
        guard panel_blocks_received(engine) > w.blocks, panel_unchecked(engine) == 0, !snap.busy else { return }
        linkWatch = nil
        let after = unitBlock()
        let (at, bit) = Self.linkPlace(group: w.group, pair: w.pair)
        let mask = UInt8(1 << bit)
        let held = (after[at] & mask) != 0 ? 1 : 0
        let byte = String(format: "d[%d] %02X→%02X", at, w.before[at], after[at])
        var text = "\(what) to \(Self.linkWords(w.bit)): sent \(Self.hex(w.sent)). "
        if held == w.bit {
            text += (w.before[at] & mask) == (after[at] & mask)
                ? "The unit already had that bit (\(byte), bit \(bit))."
                : "The unit took it: \(byte), bit \(bit)."
        } else {
            text += "THE UNIT HOLDS THE OTHER BIT: \(byte), bit \(bit) is \(held), wanted \(w.bit)."
        }
        if (w.before[at] & ~mask) != (after[at] & ~mask) {
            text += String(format: " THE OTHER BITS OF d[%d] MOVED TOO (other pairs' links).", at)
        }
        let elsewhere = (0..<256).filter { after[$0] != w.before[$0] && $0 != at }
        if elsewhere.isEmpty {
            text += " Every other byte of the 256 read back the same."
        } else {
            let list = elsewhere.prefix(8).map { String(format: "d[%d] %02X→%02X", $0, w.before[$0], after[$0]) }.joined(separator: ", ")
            text += " OTHER SETTINGS READ BACK DIFFERENT, \(elsewhere.count) byte\(elsewhere.count == 1 ? "" : "s"): \(list)\(elsewhere.count > 8 ? ", …" : "")."
        }
        let others = panel_other_messages(engine) - w.others
        text += others == 0 ? " The unit said nothing else." : " The unit also sent \(others) message\(others == 1 ? "" : "s") this app doesn't know."
        // What the two channels hold now, so a linked pair that isn't equal is no surprise.
        if held == 1, w.bit == 1 {
            let a = StripID(group: w.group, channel: w.pair * 2), b = StripID(group: w.group, channel: w.pair * 2 + 1)
            let va = snap.channels[a.group][a.channel].volume, vb = snap.channels[b.group][b.channel].volume
            let oa = snap.channels[a.group][a.channel].on, ob = snap.channels[b.group][b.channel].on
            if va == vb, oa == ob {
                text += " The two channels hold the same volume and mute."
            } else {
                var differ: [String] = []
                if va != vb { differ.append("volumes (\(Self.volumeWords(va)) and \(Self.volumeWords(vb)))") }
                if oa != ob { differ.append("mutes (\(oa ? "off" : "on") and \(ob ? "off" : "on"))") }
                text += " The two channels still hold different \(differ.joined(separator: " and ")): the link itself changed neither. The next move of either fader or MUTE button sets both."
            }
        }
        pairNote = text
        record("  settings after:  \(Self.hex(after))")
        record("  " + text)
    }

    static func soloMessage(group: Int, value: Int) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: 13)
        panel_solo_message(UInt32(group), UInt32(value), &bytes)
        return bytes
    }
    /// The number with its meaning: "3 (INPUT 3 alone)", "0 (no solo)".
    static func soloWords(group: Int, value: Int) -> String {
        value == 0 ? "0 (no solo)" : "\(value) (\(ChannelNames.groupTitle[group]) \(value) alone)"
    }

    /// A click on a channel's SOLO, as Roland's own panel does it (read from its code,
    /// 2026-11-16): the INPUT group and the OUTPUT group each hold one number, 0 for no solo
    /// or the one soloed channel, 1 to 10. A click on a dark SOLO sends the channel's number;
    /// a click on a lit one (the soloed channel, or its linked partner) sends 0. One message.
    /// For the UA-101 the settings are read back: the group's one byte should be the only
    /// thing that moved. What the unit does to the sound was never heard before the user's
    /// first try.
    @discardableResult
    func toggleSolo(_ s: StripID) -> Int32 {
        let what = ChannelNames.full(s) + " solo"
        guard s.group != ChannelNames.groupMonitor else { return done(Int32(PANEL_ERR_NO_CHANNEL), what) }
        if notPassed(.solo(s.group), what) { return Int32(PANEL_ERR_READ_ONLY) }
        let value = snap.soloed[s.group][s.channel] ? 0 : s.channel + 1
        let before = unitBlock(), blocks = panel_blocks_received(engine), others = panel_other_messages(engine)
        let r = panel_toggle_solo(engine, UInt32(s.group), UInt32(s.channel), scheduler.now)
        if r == Int32(PANEL_OK), !transport.isPretend {
            if let old = soloWatch { record("  \(ChannelNames.groupTitle[old.group]) solo: another solo was changed before its read-back came.") }
            soloWatch = SoloWatch(group: s.group, value: value, sent: Self.soloMessage(group: s.group, value: value),
                                  before: before, blocks: blocks, others: others)
            soloNote = "\(ChannelNames.groupTitle[s.group]) solo to \(Self.soloWords(group: s.group, value: value)) asked for. Reading the settings back…"
            record("\(ChannelNames.groupTitle[s.group]) solo: \(Self.soloWords(group: s.group, value: value)) asked for")
            record("  settings before: \(Self.hex(before))")
        }
        return done(r, what)
    }

    func clearSoloNote() { soloNote = nil }

    /// After every engine call: has the read-back for a solo change to the UA-101 come? The
    /// group's one byte (d[22] for INPUT, d[23] for OUTPUT) should be the only one that moved.
    private func checkSolo() {
        guard let w = soloWatch else { return }
        let what = ChannelNames.groupTitle[w.group] + " solo"
        if !snap.known {
            soloWatch = nil
            soloNote = "\(what): sent, but the unit stopped answering before the settings were read back."
            record("  " + soloNote!)
            return
        }
        guard snap.writesAllowed else {
            soloWatch = nil
            return
        }
        guard panel_blocks_received(engine) > w.blocks, panel_unchecked(engine) == 0, !snap.busy else { return }
        soloWatch = nil
        let after = unitBlock()
        let at = 0x16 + w.group - 1
        let byte = String(format: "d[%d] %02X→%02X", at, w.before[at], after[at])
        var text = "\(what) to \(Self.soloWords(group: w.group, value: w.value)): sent \(Self.hex(w.sent)). "
        if Int(after[at]) == w.value {
            text += w.before[at] == after[at] ? "The unit already had that number (\(byte))." : "The unit took it: \(byte)."
        } else {
            text += "THE UNIT HOLDS ANOTHER NUMBER: \(byte), wanted \(String(format: "%02X", w.value))."
        }
        let elsewhere = (0..<256).filter { after[$0] != w.before[$0] && $0 != at }
        if elsewhere.isEmpty {
            text += " Every other byte of the 256 read back the same."
        } else {
            let list = elsewhere.prefix(8).map { String(format: "d[%d] %02X→%02X", $0, w.before[$0], after[$0]) }.joined(separator: ", ")
            text += " OTHER SETTINGS READ BACK DIFFERENT, \(elsewhere.count) byte\(elsewhere.count == 1 ? "" : "s"): \(list)\(elsewhere.count > 8 ? ", …" : "")."
        }
        let others = panel_other_messages(engine) - w.others
        text += others == 0 ? " The unit said nothing else." : " The unit also sent \(others) message\(others == 1 ? "" : "s") this app doesn't know."
        if w.value > 0, Int(after[at]) == w.value, let p = linkedPartner(StripID(group: w.group, channel: w.value - 1)) {
            text += " \(ChannelNames.full(p)) is linked with it, so its SOLO lamp is lit here too, as in Roland's own panel; one number was sent. Whether the unit lets the partner through is heard, not read."
        }
        soloNote = text
        record("  settings after:  \(Self.hex(after))")
        record("  " + text)
    }

    /// A mouse drag: the unit's saving is paused from its first change to its last.
    /// `volumeOf`: the channel whose fader is dragged; `panOf`: the one whose pan knob is.
    func beginDrag(volumeOf strip: StripID? = nil, panOf knob: StripID? = nil) {
        endKeyHold()
        dragDepth += 1
        dragging = true
        // Only the mixer is dragged. Where the UA-101's helper doesn't pass this control on,
        // a drag changes nothing there, so it pauses nothing either.
        if dragPauses(volume: strip, pan: knob) { panel_drag_begin(engine, scheduler.now) }
        afterCall()
    }

    private func dragPauses(volume: StripID? = nil, pan: StripID? = nil) -> Bool {
        let allowed = transport.allowed
        return allowed.mixer || (volume.map { allowed.volume($0) } ?? false) || (pan.map { allowed.pan($0) } ?? false)
    }

    func endDrag() {
        guard dragDepth > 0 else { return }
        dragDepth -= 1
        if dragDepth == 0 { dragging = false }
        panel_drag_end(engine, scheduler.now)
        afterCall()
    }

    /// A double-click on a fader: back to 0 dB, from above or below; never higher. One change, as a
    /// key press is, and to both channels of a linked pair, as every volume is.
    @discardableResult func resetVolume(_ s: StripID) -> Int32 {
        cursor = s
        return setVolume(s, UInt32(PANEL_UNITY_VOLUME))
    }

    /// A double-click on a pan knob: back to the centre.
    @discardableResult func centrePan(_ s: StripID) -> Int32 {
        cursor = s
        return setPan(s, UInt32(PANEL_CENTER_PAN))
    }

    // MARK: backup, put back, Reset Mixer

    /// Where backups are written (the app: ~/Documents/UA-101 Backups). nil: nowhere, and then
    /// nothing that needs a backup first is done.
    var backupFolder: URL?
    /// The time a backup gets in its name and its first lines.
    var backupClock: () -> Date = Date.init
    /// A backup the user asked for was saved (Device menu): the app shows the file in the Finder.
    var onBackupSaved: ((URL) -> Void)?
    /// What the last Save Backup did.
    @Published private(set) var backupNote: String?
    /// What the last Open Backup did, and what the unit's settings read back as.
    @Published private(set) var putBackNote: String?
    /// What the last Reset Mixer did, and what the unit's settings read back as.
    @Published private(set) var resetNote: String?
    /// A backup was chosen for Put Back and waits for its second step (its dialog shows what
    /// would change). Nothing is sent before `confirmPutBack`.
    @Published private(set) var pendingPutBack: PendingPutBack?

    struct PendingPutBack: Equatable {
        var path: String
        var name: String
        var backup: SettingsBackup
        var plan: PutBackPlan
    }

    enum JobKind: Equatable {
        case save, putBack, reset
        /// One Recall, Save or Reset of the unit's memories.
        case memory(MemoryCommand)
        /// A backup file put into one of the unit's memories, 0 to 4: the file's settings are
        /// put on the unit, stored, and the settings from before put back.
        case intoMemory(Int)
        var title: String {
            switch self {
            case .save: return "Save Backup"
            case .putBack: return "Open Backup"
            case .reset: return "Reset Mixer"
            case .memory(let command): return command.title
            case .intoMemory(let memory): return "Copy Backup to \(ChannelNames.memories[memory])"
            }
        }
        /// For the capitals of a warning: "THE PUT BACK STOPPED".
        var noun: String {
            switch self {
            case .reset: return "RESET"
            case .memory: return "COMMAND"
            case .intoMemory: return "PUT INTO THE MEMORY"
            default: return "PUT BACK"
            }
        }
        /// It sends ordinary mixer changes, so it needs SOFT CTRL in and a helper that passes
        /// on the whole mixer.
        var changesMixer: Bool {
            switch self {
            case .putBack, .reset, .intoMemory: return true
            default: return false
            }
        }
        /// The Recall, Save or Reset it sends, if any.
        var command: MemoryCommand? {
            switch self {
            case .memory(let command): return command
            case .intoMemory(let memory): return MemoryCommand(kind: .store, memory: memory, patchBay: true, mixer: true)
            default: return nil
            }
        }
    }

    /// Save Backup, Open Backup or Reset Mixer under way. Each begins with a fresh
    /// read of the whole block; the two that change the unit then save that block as a backup
    /// by themselves, send their changes inside one pause of the unit's saving, and compare
    /// what the unit reads back as.
    private struct Job {
        var kind: JobKind
        var blocks: UInt64            // blocks received when the read, then the sending, began
        var backup: SettingsBackup?   // Put Back: the file's contents
        var fileName = ""             // Put Back: the file's name
        var sending = false
        var before: [UInt8] = []      // the fresh read
        var plan: PutBackPlan?        // Put Back: made from the fresh read
        var expected: [UInt8] = []    // Reset: what the unit should read back as
        var sets: UInt64 = 0
        var others: UInt64 = 0
        var planned = 0               // changes to send
        var handed = 0                // of those, how many have left
        var saved = ""                // the backup saved by itself, its name
        var refused = Int32(PANEL_OK) // a change the engine itself refused: should never happen
        /// The pause of the unit's saving is held open, as a drag holds it, until the last
        /// change has left: the engine sends the pause with the first change, which can be a
        /// moment after the changes were handed to it.
        var holding = false

        // Part 2: a Recall, Save or Reset, by itself or as the middle of "into a memory".
        /// What `sending` is busy with: ordinary changes on their way (a put back, a reset,
        /// and the first leg of "into a memory"); the one memory command, to send or being
        /// read back; or, for "into a memory", the settings from before on their way back.
        enum Stage { case changes, command, restore }
        var stage = Stage.changes
        var ticket: UInt32 = 0        // the engine's, from the user's first step
        var ticketExpires: UInt64 = 0
        var bytes: [UInt8] = []       // the command's one message, as the user was shown it
        var tries = 0                 // times the engine said "busy"
        var commandSent = false
        var commandAt: UInt64 = 0
        var reads: [[UInt8]] = []     // the settings as read back after the command, in order
        var readTimes: [UInt64] = []  // each read's time after the command
        var stored = false            // "into a memory": the Store left and was read back
        var settled = true            // its last two read-backs agreed
        var failed: String?           // "into a memory": why nothing was stored
        var putPlanned = 0            // "into a memory": the changes its first leg sent
        var putAfter: [UInt8] = []    // and what the unit read back as after them
        var storeChanged: [Int] = []  // bytes of the settings in use that the Store changed
        var restorePlan: PutBackPlan? // the way back to `before`
    }
    private var job: Job?
    /// When the last Recall, Save or Reset left for the UA-101: its helper passes on one every
    /// few seconds at most, so the next is held back here first.
    private var lastMemoryAt: UInt64?
    /// The seconds still to wait after the last Recall, Save or Reset before the next may go,
    /// as the dialogs show them; nil when none. Counted down once a second (2026-11-27): the
    /// wait ends with time alone, and before this nothing told the dialogs, so Continue stayed
    /// grey, with "Wait N seconds", until another memory was picked.
    @Published private(set) var memoryWaitLeft: Int?
    private var waitID: Int?
    /// A wake-up of the job under way: the next try to send its command, or its next read.
    private var jobTimerID: Int?
    /// Changes were switched off while a put back or a reset was sending: done when it ends.
    private var offWanted = false

    /// True from the start of a Save Backup, Put Back or Reset Mixer until its result is in.
    var jobRunning: Bool { job != nil }
    /// A put back or a reset is sending its changes or waiting for its read-back. Quitting
    /// then would hand the helper everything that waits at once, more than it takes in a
    /// second, and leave the unit with a mix of settings: the app waits for `onJobEnd` first.
    var jobSending: Bool { job?.sending == true }
    /// Called once when the job under way has ended, however it ended.
    var onJobEnd: (() -> Void)?

    private func jobEnded() {
        if let id = jobTimerID { scheduler.cancel(id); jobTimerID = nil }
        let told = onJobEnd
        onJobEnd = nil
        told?()
    }

    /// Books the job's one wake-up. It does nothing if the job has ended meanwhile.
    private func jobTimer(after micros: UInt64, _ work: @escaping () -> Void) {
        if let id = jobTimerID { scheduler.cancel(id) }
        jobTimerID = scheduler.schedule(at: scheduler.now + micros) { [weak self] in
            guard let self = self else { return }
            self.jobTimerID = nil
            guard self.job != nil else { return }
            work()
        }
    }

    func clearBackupNote() { backupNote = nil }
    func clearPutBackNote() { putBackNote = nil }
    func clearResetNote() { resetNote = nil }

    private func setJobNote(_ kind: JobKind, _ text: String, recorded: Bool = true) {
        switch kind {
        case .save: backupNote = text
        case .putBack: putBackNote = text
        case .reset: resetNote = text
        case .memory, .intoMemory: memoryNote = text
        }
        if recorded { record(text) }
    }

    /// One at a time: refused while a backup, a put back or a reset is under way.
    private func jobRunningRefusal(_ what: String) -> Int32? {
        guard job != nil else { return nil }
        refusal = Refusal(kind: .busy, what: what,
                          text: "\(what): not done. A backup, a put back, a reset or a memory command is still running; try again when its message has come.")
        afterCall()
        return Int32(PANEL_ERR_BUSY)
    }

    /// Why a backup, a put back or a reset can't start right now, as a refusal; nil if it can.
    private func jobRefusal(_ what: String) -> Int32? {
        if let r = jobRunningRefusal(what) { return r }
        guard snap.state == .ready else { return done(Int32(PANEL_ERR_NOT_READY), what) }
        if snap.savingHeld {
            refusal = Refusal(kind: .busy, what: what,
                              text: "\(what): not done. The unit's saving is paused (Device menu). Resume it first, then try again.")
            afterCall()
            return Int32(PANEL_ERR_BUSY)
        }
        if snap.busy || snap.settling || dragging || keyHoldID != nil { return done(Int32(PANEL_ERR_BUSY), what) }
        if backupFolder == nil {
            refusal = Refusal(kind: .value, what: what, text: "\(what): not done. There is no folder to save a backup in. Nothing was sent.")
            afterCall()
            return Int32(PANEL_ERR_VALUE)
        }
        return nil
    }

    private func startJob(_ j: Job) -> Int32 {
        job = j
        setJobNote(j.kind, "\(j.kind.title): reading the unit's settings afresh…", recorded: false)
        panel_refresh(engine, scheduler.now)
        return done(Int32(PANEL_OK), j.kind.title)
    }

    /// Device menu, "Save Backup…": a fresh read of the whole block, written to a file. Only a
    /// read: it works with changes locked too and changes nothing on the unit.
    @discardableResult
    func saveBackup() -> Int32 {
        if let r = jobRefusal(JobKind.save.title) { return r }
        return startJob(Job(kind: .save, blocks: panel_blocks_received(engine)))
    }

    /// The Mixer menu's "Reset Mixer": the mixer (not the patch bay) back to what a reset
    /// gives, made of ordinary changes, each one checked like any other. With the UA-101 it
    /// needs every mixer control to be one the helper passes on (`Allowed.mixer`). The settings
    /// are read afresh and saved as a backup first; afterwards the block is read back and
    /// compared with the factory block.
    @discardableResult
    func resetMixer() -> Int32 {
        let what = "Reset the mixer"
        if let r = jobRunningRefusal(what) { return r }
        if notPassed(.mixer, what) { return Int32(PANEL_ERR_READ_ONLY) }
        guard snap.state == .ready else { return done(Int32(PANEL_ERR_NOT_READY), what) }
        guard snap.writesAllowed else { return done(Int32(PANEL_ERR_READ_ONLY), what) }
        guard snap.softControl else { return done(Int32(PANEL_ERR_SOFT_CTRL), what) }
        if let r = jobRefusal(what) { return r }
        return startJob(Job(kind: .reset, blocks: panel_blocks_received(engine)))
    }

    /// Open Backup, step 1: reads the file and works out what would change. Nothing
    /// is sent. On success `pendingPutBack` holds it for the dialog.
    @discardableResult
    func preparePutBack(from url: URL) -> Int32 {
        let what = JobKind.putBack.title
        pendingPutBack = nil
        guard snap.state == .ready else { return done(Int32(PANEL_ERR_NOT_READY), what) }
        func refuse(_ p: BackupProblem) -> Int32 {
            refusal = Refusal(kind: .value, what: what, text: "\(what), “\(url.lastPathComponent)”: not done. " + p.text)
            afterCall()
            return Int32(PANEL_ERR_VALUE)
        }
        switch SettingsBackup.read(url) {
        case .failure(let p): return refuse(p)
        case .success(let b):
            // A backup of the pretend unit never goes onto the real one.
            if !transport.isPretend, !b.fromRealUnit { return refuse(.fromPretend) }
            switch PutBackPlan.make(backup: b.block, unit: unitBlock(), ceiling: snap.volumeCeiling) {
            case .failure(let p): return refuse(p)
            case .success(let plan):
                pendingPutBack = PendingPutBack(path: url.path, name: url.lastPathComponent, backup: b, plan: plan)
                refusal = nil
                return Int32(PANEL_OK)
            }
        }
    }

    func cancelPutBack() { pendingPutBack = nil }

    /// Why step 2 can't be done right now, for the dialog's button; nil if it can.
    func putBackBlocker() -> String? {
        if snap.state != .ready { return "The unit's settings aren't known right now." }
        if !snap.writesAllowed { return changesOffWords }
        if !transport.isPretend, !(transport.allowed.mixer && transport.allowed.allPatch) {
            return "The driver's helper doesn't pass on every change a put back needs."
        }
        if !snap.softControl { return "SOFT CTRL is out on the unit: press it in, the mixer only takes changes then." }
        if job != nil { return "A backup, a put back or a reset is still running." }
        return nil
    }

    /// Open Backup, step 2: sends it. Only after step 1. The settings are read afresh
    /// and saved as a backup first.
    @discardableResult
    func confirmPutBack() -> Int32 {
        let what = JobKind.putBack.title
        guard let p = pendingPutBack else { return done(Int32(PANEL_ERR_TICKET), what) }
        if let r = jobRunningRefusal(what) { return r }
        if notPassed(.mixer, what) || notPassed(.patchAll, what) { return Int32(PANEL_ERR_READ_ONLY) }
        guard snap.state == .ready else { return done(Int32(PANEL_ERR_NOT_READY), what) }
        guard snap.writesAllowed else { return done(Int32(PANEL_ERR_READ_ONLY), what) }
        guard snap.softControl else { return done(Int32(PANEL_ERR_SOFT_CTRL), what) }
        if let r = jobRefusal(what) { return r }
        pendingPutBack = nil
        return startJob(Job(kind: .putBack, blocks: panel_blocks_received(engine), backup: p.backup, fileName: p.name))
    }

    /// Writes one backup file. The name of the file, or why it couldn't be written.
    private struct NotWritten: Error { let why: String }
    private func writeBackup(_ block: [UInt8], why: String, label: String? = nil) -> Result<URL, NotWritten> {
        guard let folder = backupFolder else { return .failure(NotWritten(why: "there is no folder for backups")) }
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            let now = backupClock()
            let name = SettingsBackup.fileName(at: now, why: why, pretend: transport.isPretend, label: label) {
                FileManager.default.fileExists(atPath: folder.appendingPathComponent($0).path)
            }
            let url = folder.appendingPathComponent(name)
            let backup = SettingsBackup(block: block, unit: transport.displayName, saved: SettingsBackup.stamp(now), why: why)
            try backup.text().write(to: url, atomically: true, encoding: .utf8)
            return .success(url)
        } catch {
            return .failure(NotWritten(why: error.localizedDescription))
        }
    }

    /// Every mixer setting to the values a settings block holds, as ordinary changes. A linked
    /// pair is sent a volume and an on/off on both channels at once, so where its two channels
    /// are to hold different ones, or the pair is to end unlinked, its link goes off first.
    /// Returns the worst result (PANEL_OK when every change was taken).
    private func putMixer(_ target: [UInt8], now: UInt64) -> Int32 {
        var worst = Int32(PANEL_OK)
        func took(_ r: Int32) { if r != Int32(PANEL_OK), r != Int32(PANEL_CLAMPED) { worst = r } }
        func val(_ kind: Int32, _ g: Int, _ i: Int) -> UInt32 { panel_block_value(target, kind, UInt32(g), UInt32(i)) }
        let fVolume = Int32(PANEL_F_VOLUME), fPan = Int32(PANEL_F_PAN), fOn = Int32(PANEL_F_ON)
        let fLink = Int32(PANEL_F_LINK), fSolo = Int32(PANEL_F_SOLO)
        for g in 0..<3 {
            let n = Int(panel_group_channels(UInt32(g)))
            let pairs = (0..<(n / 2)).filter { snap.channels[g][$0 * 2].exists }
            for p in pairs where panel_link(engine, UInt32(g), UInt32(p)) != 0 {
                let apart = val(fLink, g, p) == 0 || val(fVolume, g, p * 2) != val(fVolume, g, p * 2 + 1)
                    || val(fOn, g, p * 2) != val(fOn, g, p * 2 + 1)
                if apart { took(panel_set_link(engine, UInt32(g), UInt32(p), 0, now)) }
            }
            for c in 0..<n where snap.channels[g][c].exists {
                took(panel_set_volume(engine, UInt32(g), UInt32(c), min(val(fVolume, g, c), snap.volumeCeiling), now))
                took(panel_set_on_bit(engine, UInt32(g), UInt32(c), Int32(val(fOn, g, c)), now))
                if g != ChannelNames.groupMonitor { took(panel_set_pan(engine, UInt32(g), UInt32(c), val(fPan, g, c), now)) }
            }
            for p in pairs { took(panel_set_link(engine, UInt32(g), UInt32(p), val(fLink, g, p) != 0 ? 1 : 0, now)) }
            if g != ChannelNames.groupMonitor { took(panel_set_solo(engine, UInt32(g), Int32(val(fSolo, g, 0)) - 1, now)) }
        }
        return worst
    }

    /// A list of bytes for a note: at most ten, the rest counted.
    private static func byteList(_ at: [Int], _ line: (Int) -> String) -> String {
        at.prefix(10).map(line).joined(separator: "; ") + (at.count > 10 ? "; and \(at.count - 10) more" : "")
    }

    private func endJob(_ kind: JobKind, _ text: String) {
        job = nil
        setJobNote(kind, text)
        jobEnded()
        if offWanted {
            offWanted = false
            allowChanges(false)
        }
    }

    /// The changes: ordinary ones, inside one pause of the unit's saving. `patchBay`: the five
    /// patch bay rows too (a put back), or the mixer alone (Reset Mixer).
    private func sendChanges(_ j: inout Job, target: [UInt8], patchBay: Bool) {
        j.blocks = panel_blocks_received(engine)
        let now = scheduler.now
        panel_drag_begin(engine, now)
        if patchBay {
            for o in 0..<5 {
                let r = panel_set_patch(engine, UInt32(o), UInt32(target[12 + o]), now)
                if r != Int32(PANEL_OK) { j.refused = r }
            }
        }
        let r = putMixer(target, now: now)
        if r != Int32(PANEL_OK) { j.refused = r }
        j.planned = Int(panel_unchecked(engine))
        j.handed = 0
        j.holding = panel_waiting(engine) != 0
        if !j.holding { panel_drag_end(engine, now) }
    }

    /// After every engine call: the next step of a Save Backup, a Put Back, a Reset Mixer, a
    /// Recall, Save or Reset of the memories, or a backup put into a memory.
    private func checkJob() {
        guard var j = job else { return }
        let what = j.kind.title
        guard j.sending else {
            // Waiting for the fresh read.
            guard snap.state == .ready else {
                endJob(j.kind, "\(what): not done. The unit stopped answering before its settings were read. Nothing was sent.")
                return
            }
            guard panel_blocks_received(engine) > j.blocks else { return }
            let fresh = unitBlock()
            j.before = fresh
            if j.kind != .save {
                // Things may have changed while the read was on its way.
                guard snap.writesAllowed else {
                    endJob(j.kind, "\(what): not done. Changes were switched off meanwhile. Nothing was sent.")
                    return
                }
                if j.kind.changesMixer {
                    guard snap.softControl else {
                        endJob(j.kind, "\(what): not done. SOFT CTRL is out on the unit, so the unit bypasses the mixer. Nothing was sent.")
                        return
                    }
                    guard transport.isPretend || (transport.allowed.mixer && (j.kind == .reset || transport.allowed.allPatch)) else {
                        endJob(j.kind, "\(what): not done. The driver's helper doesn't pass on every change it needs. Nothing was sent.")
                        return
                    }
                }
                if let command = j.kind.command {
                    guard snap.memoryAllowed else {
                        endJob(j.kind, "\(what): not done. Changes were switched off meanwhile. Nothing was sent.")
                        return
                    }
                    guard transport.allowed.memory(command.bit) else {
                        endJob(j.kind, "\(what): not done. The driver's helper doesn't pass on this command. Nothing was sent.")
                        return
                    }
                    guard !transport.soundPlaying else {
                        endJob(j.kind, "\(what): not done. Sound started to play or record through the unit meanwhile, and no Recall, Save or Reset is sent while it does. Nothing was sent.")
                        return
                    }
                }
                if let b = j.backup {
                    switch PutBackPlan.make(backup: b.block, unit: fresh, ceiling: snap.volumeCeiling) {
                    case .failure(let p):
                        endJob(j.kind, "\(what), “\(j.fileName)”: not done. " + p.text)
                        return
                    case .success(let plan):
                        j.plan = plan
                    }
                }
            }
            // The backup of this moment. Without it nothing is changed.
            let why = j.kind == .save ? JobKind.save.title : "before " + what
            switch writeBackup(fresh, why: why) {
            case .failure(let failure):
                endJob(j.kind, j.kind == .save
                    ? "\(what): NOT DONE. The backup couldn't be saved (\(failure.why))."
                    : "\(what): NOT DONE. The backup of the settings as they are now couldn't be saved (\(failure.why)), and without it nothing is changed. Nothing was sent.")
                return
            case .success(let url):
                j.saved = url.lastPathComponent
                if j.kind == .save {
                    endJob(.save, "Backup saved: “\(url.lastPathComponent)” in \(url.deletingLastPathComponent().path). It holds the unit's settings as just read, all 256 bytes.")
                    record("  settings: \(Self.hex(fresh))")
                    onBackupSaved?(url)
                    return
                }
            }
            if case .memory = j.kind {
                // One Recall, Save or Reset: the one message the user was shown, and nothing else.
                j.sending = true
                j.stage = .command
                j.others = panel_other_messages(engine)
                job = j
                record("\(what): one message to send, \(Self.hex(j.bytes)), after this fresh read")
                record("  settings before: \(Self.hex(fresh))")
                record("  the settings before were saved as “\(j.saved)”")
                setJobNote(j.kind, "\(what): the settings before were saved as “\(j.saved)”. Sending the command…", recorded: false)
                sendJobCommand()
                return
            }
            // The changes: ordinary ones, inside one pause of the unit's saving.
            var target = fresh
            if j.kind == .reset {
                var factory = [UInt8](repeating: 0, count: 256)
                panel_factory_block(&factory)
                target = factory
                let mask = PutBackPlan.mixerMask(is192: snap.rateCode == ChannelNames.rateCode192)
                j.expected = (0..<256).map { (fresh[$0] & ~mask[$0]) | (factory[$0] & mask[$0]) }
            } else if let b = j.backup {
                target = b.block
            }
            j.sets = panel_sets_sent(engine)
            j.others = panel_other_messages(engine)
            sendChanges(&j, target: target, patchBay: j.kind != .reset)
            j.sending = true
            j.stage = .changes
            job = j
            let after = readSnapshot()
            if after != snap { snap = after }
            record("\(what)\(j.backup != nil ? ", “\(j.fileName)”" : ""): \(j.planned) change\(j.planned == 1 ? "" : "s") to send, inside one pause of the unit's saving")
            record("  settings before: \(Self.hex(fresh))")
            record("  \(j.kind == .reset ? "factory block: " : "the backup:    ") \(Self.hex(target))")
            record("  the settings before were saved as “\(j.saved)”")
            if j.planned == 0 {
                changesDone(j, after: fresh)
            } else {
                setJobNote(j.kind, "\(what): \(j.planned) change\(j.planned == 1 ? "" : "s") on \(j.planned == 1 ? "its" : "their") way to the unit. The settings before were saved as “\(j.saved)”. Reading the settings back…", recorded: false)
            }
            return
        }
        // Sending, then waiting for the read-back.
        if snap.state != .ready {
            jobLostUnit(j)
            return
        }
        guard snap.writesAllowed else { // the helper refused (endChanges said so), or changes went off
            job = nil
            jobEnded()
            return
        }
        if j.stage == .command {
            checkCommand(j)
            return
        }
        j.handed = max(j.handed, j.planned - Int(panel_waiting(engine)))
        if j.holding, panel_waiting(engine) == 0 {
            // The last change has left: the unit may save again.
            j.holding = false
            job = j
            panel_drag_end(engine, scheduler.now)
            let now = readSnapshot()
            if now != snap { snap = now }
        }
        job = j
        guard panel_blocks_received(engine) > j.blocks, panel_unchecked(engine) == 0, !snap.busy else { return }
        changesDone(j, after: unitBlock())
    }

    /// The changes of a put back, a reset, or one leg of "into a memory" have left and the
    /// unit's settings are read back.
    private func changesDone(_ j: Job, after: [UInt8]) {
        switch (j.kind, j.stage) {
        case (.intoMemory, .changes): intoMemoryPut(j, after: after)
        case (.intoMemory, _): finishIntoMemory(j, after: after)
        default: finishJob(j, after: after)
        }
    }

    /// The unit went away or stopped answering in the middle of a job that was sending.
    private func jobLostUnit(_ j: Job) {
        let what = j.kind.title
        let left = j.planned - j.handed
        let back = "The settings from before are in “\(j.saved)”."
        let text: String
        switch (j.kind, j.stage) {
        case (.memory, _):
            text = j.commandSent
                ? "\(what): sent, but THE UNIT STOPPED ANSWERING before its settings were read back twice. When it answers again, look at what it holds (Device menu, Refresh Settings). \(back)"
                : "\(what): NOT SENT. The unit stopped answering first. \(back)"
        case (.intoMemory, .changes):
            text = "\(what), “\(j.fileName)”: NOT SAVED. THE \(j.kind.noun) STOPPED part way: the unit went away after \(j.handed) of \(j.planned) changes had left for it. The unit may now hold a mix of your settings and the file's. When it is back: Device menu, Open Backup, with “\(j.saved)”, then start again."
        case (.intoMemory, .command):
            text = "\(what), “\(j.fileName)”: THE UNIT STOPPED ANSWERING \(j.commandSent ? "after the Save was sent, before it was read back" : "before the Save was sent"). The settings in use hold the file's values, not yours. When the unit answers again: Device menu, Open Backup, with “\(j.saved)”."
        case (.intoMemory, .restore):
            text = "\(what), “\(j.fileName)”: \(j.stored ? "saved" : "not saved"). THE PUT BACK OF YOUR SETTINGS STOPPED part way: the unit went away after \(j.handed) of \(j.planned) changes had left for it. The unit may hold a mix of the file's settings and yours. When it is back: Device menu, Open Backup, with “\(j.saved)”."
        default:
            if left > 0 {
                text = "\(what): THE \(j.kind.noun) STOPPED part way. The unit went away after \(j.handed) of \(j.planned) changes had left for it; the other \(left) were not sent. The unit may now hold a mix of the old and the new settings. When it is back, do it again: that sends what is missing. The settings from before are in “\(j.saved)”."
            } else {
                text = "\(what): all \(j.planned) changes left, but THE UNIT STOPPED ANSWERING before its settings were read back. When it answers again, look at the mixer, or do it again. The settings from before are in “\(j.saved)”."
            }
        }
        endJob(j.kind, text)
    }

    // MARK: Recall, Save and Reset on their way

    /// Sends the job's Recall, Save or Reset. For a command by itself it is the engine's second
    /// step, with the ticket from the user's first; for "into a memory" both steps are made
    /// here (the Store of the patch bay and the mixer into the memory it was chosen, the
    /// message their dialog showed). The engine takes it only a moment after its last
    /// message, so "busy" is tried again shortly.
    private func sendJobCommand() {
        guard var j = job, j.sending, j.stage == .command, !j.commandSent else { return }
        let what = j.kind.title
        let now = scheduler.now
        var r: Int32
        if case .intoMemory(let memory) = j.kind {
            var ticket: UInt32 = 0
            var bytes = [UInt8](repeating: 0, count: 18)
            var len = 0
            r = panel_memory_prepare(engine, Int32(PANEL_MEM_STORE), UInt32(memory), 1, 1, now, &ticket, &bytes, &len)
            if r == Int32(PANEL_OK) { r = panel_memory_confirm(engine, ticket, now) }
        } else {
            r = panel_memory_confirm(engine, j.ticket, now)
        }
        if r == Int32(PANEL_ERR_BUSY), j.tries < 100 {
            j.tries += 1
            job = j
            jobTimer(after: Self.commandRetryMicros) { [weak self] in
                self?.sendJobCommand()
                self?.afterCall()
            }
            return
        }
        guard r == Int32(PANEL_OK) else {
            let why: String
            switch r {
            case Int32(PANEL_ERR_TICKET): why = "the confirmation ran out (\(Int(panel_ticket_life_us(engine) / 1_000_000)) seconds) while the unit's settings were being read"
            case Int32(PANEL_ERR_BUSY): why = "the app was still busy with earlier messages"
            case Int32(PANEL_ERR_READ_ONLY): why = "Recall, Save and Reset were switched off meanwhile"
            case Int32(PANEL_ERR_NOT_READY): why = "the way to the unit didn't take it"
            default: why = "the app refused its own message (code \(r))"
            }
            if case .intoMemory = j.kind {
                j.failed = "the Save wasn't sent: \(why)"
                beginRestore(j)
            } else {
                endJob(j.kind, "\(what): NOT SENT: \(why). Nothing was changed. The settings as they are were saved as “\(j.saved)”.")
            }
            return
        }
        j.commandSent = true
        j.commandAt = now
        j.blocks = panel_blocks_received(engine)
        lastMemoryAt = now
        watchMemoryWait()
        job = j
        let fresh = readSnapshot()
        if fresh != snap { snap = fresh }
        if case .intoMemory(let memory) = j.kind { record("  Save to \(ChannelNames.memories[memory]): sent \(Self.hex(j.bytes))") }
        setJobNote(j.kind, "\(what): sent \(Self.hex(j.bytes)). Reading the settings back…", recorded: false)
    }

    /// A job's command is to be sent or was sent: the next read-back. The settings are read
    /// until two reads in a row agree, `memoryMaxReads` times at most.
    private func checkCommand(_ current: Job) {
        var j = current
        guard j.commandSent else { return } // its next try is booked (`sendJobCommand`)
        guard panel_blocks_received(engine) > j.blocks else { return }
        j.blocks = panel_blocks_received(engine)
        let block = unitBlock()
        let n = j.reads.count + 1
        let agrees = n >= 2 && j.reads[n - 2] == block
        j.reads.append(block)
        j.readTimes.append(scheduler.now - j.commandAt)
        record("  read \(n), \(Self.seconds(scheduler.now - j.commandAt)) s after the command: " + (agrees ? "the same as read \(n - 1)" : Self.hex(block)))
        j.settled = agrees
        if agrees || n >= Self.memoryMaxReads {
            job = j
            if case .intoMemory = j.kind { intoMemoryStored(j) } else { finishMemory(j) }
            return
        }
        job = j
        jobTimer(after: transport.isPretend ? Self.memoryRereadPretend : Self.memoryRereadUnit) { [weak self] in
            guard let self = self else { return }
            panel_refresh(self.engine, self.scheduler.now)
            self.afterCall()
        }
    }

    private static func seconds(_ micros: UInt64) -> String { String(format: "%.1f", Double(micros) / 1_000_000) }

    /// How the read-backs after a command went, for a note.
    private static func readWords(_ j: Job) -> String {
        let times = j.readTimes.map { seconds($0) + " s" }
        let list = times.count <= 1 ? (times.first ?? "") : times.dropLast().joined(separator: ", ") + " and " + times.last!
        if j.settled {
            return "The settings were read back \(times.count) times (\(list) after it), and the last two reads agree. "
        }
        return "THE UNIT'S SETTINGS DID NOT SETTLE: read back \(times.count) times (\(list) after it), and the last two reads still differ. The last read is taken. "
    }

    /// What differs between two settings blocks, setting by setting, for a note: how many
    /// settings, and the first twelve in words. Bytes that are no setting this app knows are
    /// given by their place.
    static func changeWords(from before: [UInt8], to after: [UInt8]) -> (count: Int, text: String) {
        let differing = (0..<256).filter { before[$0] != after[$0] }
        guard !differing.isEmpty else { return (0, "") }
        var items: [String] = []
        var known = [UInt8](repeating: 0, count: 256)
        if case .success(let plan) = PutBackPlan.make(backup: after, unit: before, ceiling: UInt32(PANEL_MAX_VOLUME)) {
            items = plan.changes.map { "\($0.what) \($0.from) → \($0.to)" }
            known = plan.mask
        }
        let other = differing.filter { (before[$0] ^ after[$0]) & ~known[$0] != 0 }
        items += other.map { String(format: "d[%d] %02X→%02X (%@)", $0, before[$0], after[$0], PutBackPlan.byteName($0)) }
        return (items.count, items.prefix(12).joined(separator: "; ") + (items.count > 12 ? "; and \(items.count - 12) more" : ""))
    }

    /// The bytes of the block a Load of these parts may change, as the unit is thought to
    /// keep them (the pretend unit does): the patch bay is rows 1 and 2 (d[12] to d[21]), the
    /// mixer d[22] to d[247]. Never the switches, the flags, or the last eight bytes.
    static func loadMask(patchBay: Bool, mixer: Bool) -> [Bool] {
        (0..<256).map { (patchBay && (12...21).contains($0)) || (mixer && (22...247).contains($0)) }
    }

    /// Things in a block just loaded that the user should know before the monitors go up.
    private func cautions(in block: [UInt8]) -> String {
        var out = ""
        let is192 = SettingsBackup.switches(block).is192
        let loud = Allowed.volumeStrips.filter {
            PutBackPlan.exists($0, is192: is192) && SettingsBackup.value(block, Int32(PANEL_F_VOLUME), $0.group, $0.channel) > UInt32(PANEL_UNITY_VOLUME)
        }
        if !loud.isEmpty {
            let names = loud.prefix(6).map { "\(ChannelNames.full($0)) \(Format.dB(SettingsBackup.value(block, Int32(PANEL_F_VOLUME), $0.group, $0.channel))) dB" }.joined(separator: ", ")
            out += "Volumes above 0 dB are in use now: \(names)\(loud.count > 6 ? ", and \(loud.count - 6) more" : ""). Keep the monitors down until you have looked. "
        }
        let thru = (0..<5).filter { (2...6).contains(Int(SettingsBackup.value(block, Int32(PANEL_F_PATCH), 0, $0))) }
        if !thru.isEmpty {
            out += "The patch bay now sends inputs straight to outputs (\(thru.map { ChannelNames.outputPairs[$0] }.joined(separator: ", "))): keep the monitors down until you have looked. "
        }
        return out
    }

    /// A Recall, Save or Reset was sent and read back: what the unit holds now against what
    /// it held before, and the result as a file.
    private func finishMemory(_ j: Job) {
        guard case .memory(let c) = j.kind, let after = j.reads.last else { return }
        let before = j.before
        let what = c.title
        let others = panel_other_messages(engine) - j.others
        let differing = (0..<256).filter { after[$0] != before[$0] }
        let changed = Self.changeWords(from: before, to: after)
        func bytes(_ at: [Int]) -> String { Self.byteList(at) { String(format: "d[%d] %02X→%02X (%@)", $0, before[$0], after[$0], PutBackPlan.settingName(at: $0)) } }
        var text = "\(what)\(c.kind == .load || c.kind == .store ? " (\(c.parts))" : ""): sent \(Self.hex(j.bytes)). " + Self.readWords(j)
        var label: String? = nil
        var why = "after " + what
        switch c.kind {
        case .store, .resetMemory:
            if differing.isEmpty {
                text += "The settings in use read back the same as before, all 256 bytes, as they should: this changes only the memory. What \(ChannelNames.memories[c.memory]) holds now can be seen by recalling it. "
            } else {
                text += "THE SETTINGS IN USE CHANGED in \(differing.count) byte\(differing.count == 1 ? "" : "s"), which this command should not do: \(bytes(differing)). "
                label = why
            }
        case .load:
            let mask = Self.loadMask(patchBay: c.patchBay, mixer: c.mixer)
            let outside = differing.filter { !mask[$0] }
            text += differing.isEmpty
                ? "Nothing changed: \(ChannelNames.memories[c.memory]) holds the same \(c.parts) as were in use. "
                : "\(changed.count) setting\(changed.count == 1 ? "" : "s") changed: \(changed.text). "
            if !outside.isEmpty {
                text += "THE UNIT ALSO CHANGED BYTES OUTSIDE the part\(c.patchBay && c.mixer ? "s" : "") asked for, in \(outside.count) byte\(outside.count == 1 ? "" : "s"): \(bytes(outside)). "
            }
            text += cautions(in: after)
            label = ChannelNames.memories[c.memory] + (c.patchBay && c.mixer ? "" : c.mixer ? " mixer" : " patch bay") + " as read"
            why = "\(ChannelNames.memories[c.memory]) as read through Recall (\(c.parts)\(c.patchBay && c.mixer ? "" : "; the rest of the file is what was in use"))"
        case .resetCurrent, .resetAll:
            var factory = [UInt8](repeating: 0, count: 256)
            panel_factory_block(&factory)
            let notFactory = (7...247).filter { after[$0] != factory[$0] }
            let outside = differing.filter { !(7...247).contains($0) }
            text += differing.isEmpty ? "Nothing changed: the settings in use already were what a reset gives. "
                                      : "\(changed.count) setting\(changed.count == 1 ? "" : "s") changed: \(changed.text). "
            text += notFactory.isEmpty
                ? "The patch bay and the mixer now read back as the factory block this app holds, every byte (d[7] to d[247]). "
                : "THE UNIT'S RESET VALUES DIFFER from the factory block this app holds in \(notFactory.count) byte\(notFactory.count == 1 ? "" : "s"): "
                    + Self.byteList(notFactory) { String(format: "d[%d] %02X, the app's factory %02X (%@)", $0, after[$0], factory[$0], PutBackPlan.settingName(at: $0)) } + ". "
            if !outside.isEmpty {
                text += "THE UNIT ALSO CHANGED BYTES OUTSIDE the patch bay and the mixer, in \(outside.count) byte\(outside.count == 1 ? "" : "s"): \(bytes(outside)). "
            }
            if c.kind == .resetAll { text += "The five memories were reset too; that can't be read here: recall one to see what it holds. " }
            label = why
        }
        if let label = label {
            switch writeBackup(after, why: why, label: label) {
            case .success(let url): text += "The settings as read back were saved as “\(url.lastPathComponent)”. "
            case .failure(let failure): text += "THE RESULT COULDN'T BE SAVED as a file (\(failure.why)); it is in the app's record. "
            }
        }
        if others != 0 { text += "The unit also sent \(others) message\(others == 1 ? "" : "s") this app doesn't know. " }
        text += "The settings from before are in “\(j.saved)”."
        record("  settings after:  \(Self.hex(after))")
        endJob(j.kind, text)
    }

    // MARK: a backup into a memory: put, Store, put back

    /// The first leg is read back: the unit should hold the file's settings now. If it does,
    /// they are stored.
    private func intoMemoryPut(_ current: Job, after: [UInt8]) {
        var j = current
        let plan = j.plan ?? PutBackPlan()
        let file = j.backup?.block ?? after
        let result = plan.outcome(after: after, backup: file)
        j.putPlanned = j.planned
        j.putAfter = after
        record("  settings after the file's were put: \(Self.hex(after))")
        if j.refused != Int32(PANEL_OK) {
            j.failed = "the app refused some of its own changes (code \(j.refused))"
            j.refused = Int32(PANEL_OK)
        } else if !result.unexpected.isEmpty {
            j.failed = "the unit didn't take the file's settings: \(result.unexpected.count) byte\(result.unexpected.count == 1 ? "" : "s") that should match don't ("
                + Self.byteList(result.unexpected) { String(format: "d[%d] %02X, wanted %02X (%@)", $0, after[$0], plan.expected[$0], PutBackPlan.settingName(at: $0)) } + ")"
        } else if transport.soundPlaying {
            j.failed = "sound started to play or record through the unit, and no Save is sent while it does"
        }
        guard j.failed == nil else {
            beginRestore(j)
            return
        }
        // The read just held against the file came after the last change of this leg (the
        // engine reads back only once the unit's saving is resumed), so it is the fresh read
        // the helper wants before a Store: the Store can go.
        j.stage = .command
        j.tries = 0
        job = j
        setJobNote(j.kind, "\(j.kind.title), “\(j.fileName)”: the file's settings are on the unit (\(j.putPlanned) change\(j.putPlanned == 1 ? "" : "s")). Saving them to the memory…", recorded: false)
        sendJobCommand()
    }

    /// The Store was read back: the settings in use should be the same as before it.
    private func intoMemoryStored(_ current: Job) {
        var j = current
        let after = j.reads.last ?? unitBlock()
        j.stored = true
        j.storeChanged = (0..<256).filter { after[$0] != j.putAfter[$0] }
        beginRestore(j)
    }

    /// The last leg: the settings from before go back onto the unit, as ordinary changes.
    private func beginRestore(_ current: Job) {
        var j = current
        let now = unitBlock()
        switch PutBackPlan.make(backup: j.before, unit: now, ceiling: snap.volumeCeiling) {
        case .failure(let p):
            endJob(j.kind, intoMemoryWords(j) + "THE SETTINGS FROM BEFORE COULD NOT BE PUT BACK (\(p.text)) They are in “\(j.saved)”: Device menu, Open Backup.")
            return
        case .success(let plan):
            j.restorePlan = plan
        }
        j.stage = .restore
        sendChanges(&j, target: j.before, patchBay: true)
        job = j
        let fresh = readSnapshot()
        if fresh != snap { snap = fresh }
        record("  putting the settings from before back: \(j.planned) change\(j.planned == 1 ? "" : "s")")
        if j.planned == 0 {
            finishIntoMemory(j, after: now)
        } else {
            setJobNote(j.kind, "\(j.kind.title), “\(j.fileName)”: \(j.stored ? "saved" : "not saved"). Putting your settings back (\(j.planned) change\(j.planned == 1 ? "" : "s"))…", recorded: false)
        }
    }

    /// What became of the Store, for the job's last note.
    private func intoMemoryWords(_ j: Job) -> String {
        guard case .intoMemory(let memory) = j.kind else { return "" }
        let name = ChannelNames.memories[memory]
        var text = "\(j.kind.title), “\(j.fileName)”: "
        if j.stored {
            text += "saved. The file's patch bay and mixer were put on the unit (\(j.putPlanned) change\(j.putPlanned == 1 ? "" : "s")) and saved to \(name) with \(Self.hex(j.bytes)). " + Self.readWords(j)
            text += j.storeChanged.isEmpty
                ? "The Save left the settings in use as they were, all 256 bytes. What \(name) holds now can be seen by recalling it. "
                : "THE SAVE CHANGED THE SETTINGS IN USE in \(j.storeChanged.count) byte\(j.storeChanged.count == 1 ? "" : "s"), which it should not do (the first: d[\(j.storeChanged[0])]). "
            let plan = j.plan ?? PutBackPlan()
            if !plan.held.isEmpty { text += "Held in the memory: " + plan.held.joined(separator: " ") + " " }
            if !plan.cantPutBack.isEmpty { text += "Not in the memory as the file has it: " + plan.cantPutBack.joined(separator: " ") + " " }
        } else {
            text += "NOT SAVED: \(j.failed ?? "the Save wasn't sent"). \(name) is as it was. "
        }
        return text
    }

    /// The settings from before were sent back and read: the end of "into a memory".
    private func finishIntoMemory(_ j: Job, after: [UInt8]) {
        let plan = j.restorePlan ?? PutBackPlan()
        let result = plan.outcome(after: after, backup: j.before)
        var text = intoMemoryWords(j)
        func byte(_ i: Int, _ wanted: UInt8) -> String {
            String(format: "d[%d] %02X, before %02X (%@)", i, after[i], wanted, PutBackPlan.settingName(at: i))
        }
        if !result.unexpected.isEmpty {
            text += "THE UNIT DIFFERS from your settings before in \(result.unexpected.count) byte\(result.unexpected.count == 1 ? "" : "s") that should match: "
                + Self.byteList(result.unexpected) { byte($0, plan.expected[$0]) } + ". Device menu, Open Backup, with “\(j.saved)”. "
        } else if result.differing.isEmpty {
            text += "Your settings are back as before, all 256 bytes (\(j.planned) change\(j.planned == 1 ? "" : "s")). "
        } else {
            text += "Your settings are back in everything this app can put back (\(j.planned) change\(j.planned == 1 ? "" : "s")). \(result.differing.count) byte\(result.differing.count == 1 ? "" : "s") still differ\(result.differing.count == 1 ? "s" : "") from before: "
                + Self.byteList(result.differing) { byte($0, j.before[$0]) } + ". "
            if !plan.held.isEmpty { text += plan.held.joined(separator: " ") + " " }
        }
        if j.refused != Int32(PANEL_OK) { text += "THE APP REFUSED some of its own changes before they left (code \(j.refused)). " }
        let others = panel_other_messages(engine) - j.others
        if others != 0 { text += "The unit also sent \(others) message\(others == 1 ? "" : "s") this app doesn't know. " }
        text += "The settings from before are in “\(j.saved)”."
        record("  settings after:  \(Self.hex(after))")
        endJob(j.kind, text)
    }

    /// The read-back is in: what the unit holds now against what it should.
    private func finishJob(_ j: Job, after: [UInt8]) {
        let what = j.kind.title
        let messages = panel_sets_sent(engine) - j.sets
        let others = panel_other_messages(engine) - j.others
        var text: String
        func byte(_ i: Int, _ wanted: UInt8, _ wantedName: String) -> String {
            String(format: "d[%d] %02X, %@ %02X (%@)", i, after[i], wantedName, wanted, PutBackPlan.settingName(at: i))
        }
        if j.kind == .reset {
            var factory = [UInt8](repeating: 0, count: 256)
            panel_factory_block(&factory)
            let unexpected = (0..<256).filter { after[$0] != j.expected[$0] }
            let mixer = (22...247).filter { after[$0] != factory[$0] }
            text = j.planned == 0 ? "\(what): the mixer already was as delivered. Nothing was sent. "
                                  : "\(what): \(j.planned) change\(j.planned == 1 ? "" : "s") sent (\(messages) messages with the pause and resume of the unit's saving). "
            if !unexpected.isEmpty {
                text += "THE UNIT DIFFERS from what the reset should give in \(unexpected.count) byte\(unexpected.count == 1 ? "" : "s"): "
                    + Self.byteList(unexpected) { byte($0, j.expected[$0], "wanted") } + ". "
            } else if mixer.isEmpty {
                text += "The unit's mixer reads back as the factory block, every byte (d[22] to d[247]). "
            } else {
                text += "The unit took every change. \(mixer.count) byte\(mixer.count == 1 ? "" : "s") of the mixer still differ\(mixer.count == 1 ? "s" : "") from the factory block, in places this app doesn't change: "
                    + Self.byteList(mixer) { byte($0, factory[$0], "factory") } + ". "
            }
            text += "The patch bay and the switches weren't touched."
        } else {
            let plan = j.plan ?? PutBackPlan()
            let backup = j.backup?.block ?? after
            let result = plan.outcome(after: after, backup: backup)
            text = "\(what), “\(j.fileName)”: " + (j.planned == 0 ? "the unit already held everything this app can put back. Nothing was sent. "
                                                               : "\(j.planned) change\(j.planned == 1 ? "" : "s") sent (\(messages) messages with the pause and resume of the unit's saving). ")
            if !result.unexpected.isEmpty {
                text += "THE UNIT DIFFERS from the backup in \(result.unexpected.count) byte\(result.unexpected.count == 1 ? "" : "s") that should match: "
                    + Self.byteList(result.unexpected) { byte($0, plan.expected[$0], "wanted") } + ". "
            } else if result.differing.isEmpty {
                text += "The unit's settings match the backup, all 256 bytes. "
            } else {
                text += "The unit matches the backup in everything this app can put back. \(result.differing.count) byte\(result.differing.count == 1 ? "" : "s") still differ\(result.differing.count == 1 ? "s" : ""): "
                    + Self.byteList(result.differing) { byte($0, backup[$0], "backup") } + ". "
            }
            if !plan.held.isEmpty { text += plan.held.joined(separator: " ") + " " }
            if !plan.cantPutBack.isEmpty { text += "Not put back: " + plan.cantPutBack.joined(separator: " ") + " " }
            text += "The switches on the box are never put back."
        }
        if j.refused != Int32(PANEL_OK) { text += " THE APP REFUSED some of its own changes before they left (code \(j.refused))." }
        if others != 0 { text += " The unit also sent \(others) message\(others == 1 ? "" : "s") this app doesn't know." }
        text += " The settings from before are in “\(j.saved)”."
        record("  settings after:  \(Self.hex(after))")
        endJob(j.kind, text)
    }

    // MARK: the patch bay

    static func patchMessage(output: Int, source: UInt32) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: 13)
        panel_patch_message(UInt32(output), source, &bytes)
        return bytes
    }

    /// One output pair's source. A single message: the unit's saving isn't paused around it,
    /// as it is around a drag. With the UA-101 the change and what the
    /// settings read back as afterwards are noted and written to the record.
    @discardableResult
    func setPatch(output: Int, source: UInt32) -> Int32 {
        let what = "Patch bay, \(ChannelNames.outputPairs[output])"
        hardThruAsked = false
        if notPassed(.patch(output), what) { return Int32(PANEL_ERR_READ_ONLY) }
        let before = unitBlock(), blocks = panel_blocks_received(engine), others = panel_other_messages(engine)
        let waiting = panel_unchecked(engine)
        let r = panel_set_patch(engine, UInt32(output), source, scheduler.now)
        if r == Int32(PANEL_OK), !transport.isPretend, patchWatch == nil, panel_unchecked(engine) == waiting {
            // The engine sends nothing for a value the unit already holds.
            patchNote = "\(ChannelNames.outputPairs[output]) already plays \(ChannelNames.sourceName(source)): you clicked the lit point. Nothing was sent, and nothing is wrong. To change this row, click one of its dark points."
            record(patchNote!)
        } else if r == Int32(PANEL_OK), !transport.isPretend {
            if let old = patchWatch { record("  \(old.what): another patch bay change was made before its read-back came.") }
            let sent = Self.patchMessage(output: output, source: source)
            let w = PatchWatch(what: "\(what) to \(ChannelNames.sourceName(source))", output: output, source: source,
                               sent: sent, before: before, blocks: blocks, others: others)
            patchWatch = w
            patchNote = "\(w.what): sent \(Self.hex(sent)). Reading the settings back…"
            record("\(w.what): sent \(Self.hex(sent))")
            record("  settings before: \(Self.hex(before))")
        }
        return done(r, what)
    }

    func clearPatchNote() { patchNote = nil }

    /// After every engine call: has the read-back for a patch bay change to the UA-101 come?
    /// The change should be the only thing that moved: byte 12 + output of the block.
    private func checkPatch() {
        guard let w = patchWatch else { return }
        if !snap.known {
            patchWatch = nil
            patchNote = "\(w.what): sent, but the unit stopped answering before the settings were read back."
            record("  " + patchNote!)
            return
        }
        guard snap.writesAllowed else { // switched off, or the helper refused: nothing to wait for
            patchWatch = nil
            return
        }
        guard panel_blocks_received(engine) > w.blocks, panel_unchecked(engine) == 0 else { return }
        patchWatch = nil
        let after = unitBlock()
        let at = 12 + w.output
        let differing = (0..<256).filter { after[$0] != w.before[$0] }
        let elsewhere = differing.filter { $0 != at }
        let others = panel_other_messages(engine) - w.others
        var text = "\(w.what): sent \(Self.hex(w.sent)). "
        let byte = String(format: "d[%d] %02X→%02X", at, w.before[at], after[at])
        if after[at] == UInt8(truncatingIfNeeded: w.source) {
            text += w.before[at] == after[at]
                ? "The unit already had that value (d[\(at)] \(String(format: "%02X", after[at])))."
                : "The unit took it: \(byte)."
        } else {
            text += "THE UNIT DID NOT TAKE IT: \(byte), wanted \(String(format: "%02X", w.source))."
        }
        if elsewhere.isEmpty {
            text += " Every other byte of the 256 read back the same."
        } else {
            let list = elsewhere.prefix(8).map { String(format: "d[%d] %02X→%02X", $0, w.before[$0], after[$0]) }.joined(separator: ", ")
            text += " OTHER SETTINGS READ BACK DIFFERENT, \(elsewhere.count) byte\(elsewhere.count == 1 ? "" : "s"): \(list)\(elsewhere.count > 8 ? ", …" : "")."
        }
        text += others == 0 ? " The unit said nothing else." : " The unit also sent \(others) message\(others == 1 ? "" : "s") this app doesn't know."
        patchNote = text
        record("  settings after:  \(Self.hex(after))")
        record("  " + text)
    }

    /// What Hard Thru does, for its second step: the words the patch bay window shows.
    static let hardThruWarning = "Hard Thru sends every INPUT pair straight to the OUTPUT pair with the same number (INPUT 1-2 to OUTPUT 1-2, and so on, OPTICAL IN to OPTICAL OUT), in place of what the computer plays. With a microphone near speakers that are on, or a cable from an output back to an input, that can howl. Turn the speakers or the amplifier down first. Initial Setting puts the computer's playback back."

    /// Hard Thru, step 1. Nothing is sent: the patch bay window then shows what it does and
    /// asks. Refused here for the same reasons the preset itself would be.
    @discardableResult
    func askHardThru() -> Int32 {
        let what = "Patch bay, Hard Thru"
        hardThruAsked = false
        if notPassed(.patchAll, what) { return Int32(PANEL_ERR_READ_ONLY) }
        guard snap.state == .ready else { return done(Int32(PANEL_ERR_NOT_READY), what) }
        guard snap.writesAllowed else { return done(Int32(PANEL_ERR_READ_ONLY), what) }
        refusal = nil
        hardThruAsked = true
        return Int32(PANEL_OK)
    }

    /// Hard Thru, step 2: sends it. Only after step 1.
    @discardableResult
    func confirmHardThru() -> Int32 {
        guard hardThruAsked else { return done(Int32(PANEL_ERR_TICKET), "Patch bay, Hard Thru") }
        hardThruAsked = false
        return patchPreset(hardThru: true)
    }

    func cancelHardThru() { hardThruAsked = false }

    /// Hard Thru or Initial Setting: five ordinary patch bay changes, one for each row that
    /// plays another source now, each within what the helper passes on. Roland's own
    /// one-message preset belongs with Recall, Save and Reset and is never sent. For the UA-101
    /// the settings are read back: d[12] to d[16] should be the only bytes that moved.
    @discardableResult
    func patchPreset(hardThru: Bool) -> Int32 {
        let name = hardThru ? "Hard Thru" : "Initial Setting"
        let what = "Patch bay, " + name
        hardThruAsked = false
        if notPassed(.patchAll, what) { return Int32(PANEL_ERR_READ_ONLY) }
        let before = unitBlock(), blocks = panel_blocks_received(engine), others = panel_other_messages(engine)
        let waiting = panel_unchecked(engine)
        let first: UInt32 = hardThru ? 2 : 7
        let r = panel_patch_preset(engine, hardThru ? 1 : 0, scheduler.now)
        if r == Int32(PANEL_OK), !transport.isPretend, presetWatch == nil, patchWatch == nil, panel_unchecked(engine) == waiting {
            patchNote = "\(name): the unit already has it in all five rows. Nothing was sent."
            record(patchNote!)
        } else if r == Int32(PANEL_OK), !transport.isPretend {
            let sent = (0..<5).filter { UInt32(before[12 + $0]) != first + UInt32($0) }
                .map { Self.patchMessage(output: $0, source: first + UInt32($0)) }
            presetWatch = PresetWatch(hardThru: hardThru, sent: sent, before: before, blocks: blocks, others: others)
            patchNote = "\(name) asked for: \(sent.count) row\(sent.count == 1 ? "" : "s") to change. Reading the settings back…"
            record("\(what): \(sent.count) row\(sent.count == 1 ? "" : "s") to change")
            for bytes in sent { record("  to send: \(Self.hex(bytes))") }
            record("  settings before: \(Self.hex(before))")
        }
        return done(r, what)
    }

    /// After every engine call: has the read-back for a preset sent to the UA-101 come? Each
    /// of the five rows is named with what it holds; nothing outside d[12] to d[16] should
    /// have moved.
    private func checkPreset() {
        guard let w = presetWatch else { return }
        let name = w.hardThru ? "Hard Thru" : "Initial Setting"
        if !snap.known {
            presetWatch = nil
            patchNote = "\(name): sent, but the unit stopped answering before the settings were read back."
            record("  " + patchNote!)
            return
        }
        guard snap.writesAllowed else {
            presetWatch = nil
            return
        }
        guard panel_blocks_received(engine) > w.blocks, panel_unchecked(engine) == 0, !snap.busy else { return }
        presetWatch = nil
        let after = unitBlock()
        let first = w.hardThru ? 2 : 7
        let wrong = (0..<5).filter { Int(after[12 + $0]) != first + $0 }
        let moved = (0..<5).filter { after[12 + $0] != w.before[12 + $0] }
        var text = "\(name): \(w.sent.count) message\(w.sent.count == 1 ? "" : "s") sent. "
        if wrong.isEmpty {
            let rows = moved.map { String(format: "%@ d[%d] %02X→%02X", ChannelNames.outputPairs[$0], 12 + $0, w.before[12 + $0], after[12 + $0]) }
            text += "The unit took it in all five rows" + (rows.isEmpty ? "." : ": " + rows.joined(separator: ", ") + ".")
            if moved.count < 5 { text += " The other \(5 - moved.count) already had it." }
        } else {
            let rows = wrong.map { String(format: "%@ d[%d] is %02X, wanted %02X", ChannelNames.outputPairs[$0], 12 + $0, after[12 + $0], first + $0) }
            text += "THE UNIT DID NOT TAKE IT IN \(wrong.count) ROW\(wrong.count == 1 ? "" : "S"): " + rows.joined(separator: ", ") + "."
        }
        let elsewhere = (0..<256).filter { after[$0] != w.before[$0] && !(12...16).contains($0) }
        if elsewhere.isEmpty {
            text += " Every byte outside the five rows read back the same."
        } else {
            let list = elsewhere.prefix(8).map { String(format: "d[%d] %02X→%02X", $0, w.before[$0], after[$0]) }.joined(separator: ", ")
            text += " OTHER SETTINGS READ BACK DIFFERENT, \(elsewhere.count) byte\(elsewhere.count == 1 ? "" : "s"): \(list)\(elsewhere.count > 8 ? ", …" : "")."
        }
        let others = panel_other_messages(engine) - w.others
        text += others == 0 ? " The unit said nothing else." : " The unit also sent \(others) message\(others == 1 ? "" : "s") this app doesn't know."
        patchNote = text
        record("  settings after:  \(Self.hex(after))")
        record("  " + text)
    }

    // MARK: the memory preview (link version 20)

    /// Why what memory `m` (0 to 4) holds can't be shown now, in plain words; nil if it can be
    /// read. A read changes nothing on the unit, so changes need not be on.
    func previewBlocker(_ m: Int) -> String? {
        guard (0..<5).contains(m) else { return "There is no such memory." }
        if snap.state != .ready { return "The unit's settings aren't known right now." }
        if !transport.isPretend, !transport.allowed.memoryRead(m) {
            return "The driver's helper that is installed is older and doesn't read the memories. The Recall itself still works."
        }
        if job != nil { return "A backup, a put back, a reset or a memory command is still running." }
        return nil
    }

    /// Asks the unit what memory `m` holds: one read, which changes nothing. Not while a job
    /// runs or a fader moves; the engine also holds it while a change is on its way.
    func readMemoryPreview(_ m: Int) {
        guard previewBlocker(m) == nil, !dragging, keyHoldID == nil else { return }
        let r = panel_read_memory(engine, UInt32(m), scheduler.now)
        if r == Int32(PANEL_OK), !transport.isPretend {
            record("Asked the unit what Memory \(m + 1) holds, for the preview in Recall Memory (a read; it changes nothing).")
        }
        afterCall()
    }

    /// What memory `m` holds against the settings in use, or where reading it stands.
    func memoryPreview(_ m: Int) -> MemoryPreviewState {
        if let why = previewBlocker(m) { return .cantShow(why) }
        var block = [UInt8](repeating: 0, count: 256)
        if panel_memory_block(engine, UInt32(m), &block) != 0 {
            return .read(MemoryPreview.make(memory: block, live: unitBlock()))
        }
        switch panel_memory_reading(engine, UInt32(m)) {
        case 1: return .reading
        case 2: return .cantShow("The unit didn't answer the read. Nothing was changed. The Recall itself still works.")
        default: return .notRead
        }
    }

    // MARK: Load, Store, Reset: two steps

    /// How long the last Recall, Save or Reset for the UA-101 must lie back before the next:
    /// the helper's own time and a second more. nil: it can go now. The pretend unit has no
    /// helper and no such rule.
    private func memoryWaitSeconds(at time: UInt64? = nil) -> Int? {
        guard !transport.isPretend, let last = lastMemoryAt else { return nil }
        let gap = Self.memoryWaitMicros
        let passed = (time ?? scheduler.now) - last
        return passed < gap ? Int((gap - passed + 999_999) / 1_000_000) : nil
    }
    private static let memoryWaitMicros = (UInt64(panel_memory_gap_seconds()) + 1) * 1_000_000

    /// Keeps `memoryWaitLeft` as `memoryWaitSeconds` says, with a wake-up each time it goes
    /// down by one, the last when the wait is over.
    private func watchMemoryWait() {
        if let id = waitID { scheduler.cancel(id); waitID = nil }
        let now = scheduler.now // read once: the live clock moves between two reads
        let left = memoryWaitSeconds(at: now)
        if memoryWaitLeft != left { memoryWaitLeft = left }
        guard left != nil, let last = lastMemoryAt else { return }
        let rest = Self.memoryWaitMicros - (now - last) // more than 0: `left` isn't nil
        let step = rest % 1_000_000 == 0 ? 1_000_000 : rest % 1_000_000
        waitID = scheduler.schedule(at: now + step) { [weak self] in
            self?.waitID = nil
            self?.watchMemoryWait()
        }
    }

    /// Why a Recall, Save or Reset can't be sent right now, in plain words, for the
    /// dialogs; nil if it can. `command`: also whether the helper passes this one on.
    func memoryBlocker(_ command: MemoryCommand? = nil) -> String? {
        if snap.state != .ready { return "The unit's settings aren't known right now." }
        if !snap.writesAllowed { return changesOffWords }
        if !transport.isPretend, !transport.allowed.someMemory { return "The driver's helper that is installed passes on no Recall, Save or Reset." }
        if !snap.memoryAllowed { return changesOffWords } // on and off with changes: the app's one switch
        if let c = command, !transport.isPretend, !transport.allowed.memory(c.bit) { return "The driver's helper doesn't pass on this command." }
        if transport.soundPlaying { return "Sound is playing or being recorded through the UA-101. Stop it first: no Recall, Save or Reset is sent meanwhile." }
        if job != nil { return "A backup, a put back, a reset or a memory command is still running." }
        if let wait = memoryWaitSeconds() { return "The last Recall, Save or Reset was a moment ago. Wait \(wait) second\(wait == 1 ? "" : "s")." }
        return nil
    }

    /// The blocker as a refusal, with everything a command needs checked; nil if it can go.
    private func memoryRefusal(_ command: MemoryCommand, _ what: String) -> Int32? {
        if let r = jobRunningRefusal(what) { return r }
        guard snap.state == .ready else { return done(Int32(PANEL_ERR_NOT_READY), what) }
        guard snap.writesAllowed, snap.memoryAllowed else { return done(Int32(PANEL_ERR_READ_ONLY), what) }
        func refuse(_ kind: Refusal.Kind, _ text: String, _ r: Int32) -> Int32 {
            refusal = Refusal(kind: kind, what: what, text: "\(what): not done. \(text) Nothing was sent.")
            afterCall()
            return r
        }
        if !transport.isPretend, !transport.allowed.someMemory {
            return refuse(.notPassed, "The driver's helper that is installed passes on no Recall, Save or Reset.", Int32(PANEL_ERR_READ_ONLY))
        }
        if !transport.isPretend, !transport.allowed.memory(command.bit) {
            return refuse(.notPassed, "The driver's helper doesn't pass on this command.", Int32(PANEL_ERR_READ_ONLY))
        }
        if transport.soundPlaying {
            return refuse(.busy, "Sound is playing or being recorded through the UA-101, and the driver's helper passes on no Recall, Save or Reset meanwhile. Stop the sound first.", Int32(PANEL_ERR_BUSY))
        }
        if let wait = memoryWaitSeconds() {
            return refuse(.busy, "The driver's helper passes on one Recall, Save or Reset every \(panel_memory_gap_seconds()) seconds at most, and the last was a moment ago. Try again in \(wait) second\(wait == 1 ? "" : "s").", Int32(PANEL_ERR_BUSY))
        }
        return nil
    }

    /// Step 1. Nothing is sent. On success `pendingMemory` holds what step 2 would send.
    @discardableResult
    func prepareMemory(_ command: MemoryCommand) -> Int32 {
        dropPending(note: nil)
        guard command.isValid else { return done(Int32(PANEL_ERR_VALUE), command.title) }
        if let r = jobRunningRefusal(command.title) { return r }
        // A helper that passes on no Recall, Save or Reset at all: not even prepared.
        if !transport.isPretend, snap.writesAllowed, !transport.allowed.someMemory {
            refusal = Refusal(kind: .notPassed, what: command.title,
                              text: "\(command.title): not done. The driver's helper that is installed passes on no Recall, Save or Reset. Nothing was sent.")
            afterCall()
            return Int32(PANEL_ERR_READ_ONLY)
        }
        var ticket: UInt32 = 0
        var bytes = [UInt8](repeating: 0, count: 18)
        var len = 0
        let now = scheduler.now
        let r = panel_memory_prepare(engine, Int32(command.kind.rawValue), UInt32(command.memory),
                                     command.patchBay ? 1 : 0, command.mixer ? 1 : 0, now, &ticket, &bytes, &len)
        if r == Int32(PANEL_OK) {
            let expires = now + panel_ticket_life_us(engine)
            pendingMemory = PendingMemory(command: command, ticket: ticket, expires: expires, bytes: Array(bytes[0..<len]))
            memoryNote = nil
            ticketID = scheduler.schedule(at: expires) { [weak self] in
                guard let self = self, self.pendingMemory?.ticket == ticket else { return }
                self.ticketID = nil
                self.dropPending(note: "\(command.title): the confirmation ran out. Nothing was sent.")
                self.afterCall()
            }
        }
        return done(r, command.title)
    }

    /// Step 2. Sends the prepared command, once: the unit's settings are read afresh and saved
    /// as a backup first, then the one message goes, and the settings are read back until two
    /// reads agree (`checkJob`). What it did comes as `memoryNote`.
    @discardableResult
    func confirmMemory() -> Int32 {
        guard let pending = pendingMemory else { return done(Int32(PANEL_ERR_TICKET), "Confirm") }
        let what = pending.command.title
        if let r = memoryRefusal(pending.command, what) ?? jobRefusal(what) {
            dropPending(note: "\(what): not sent.")
            return r
        }
        // The engine keeps the ticket for the job; this model's own watch of it ends here.
        if let id = ticketID { scheduler.cancel(id); ticketID = nil }
        pendingMemory = nil
        var j = Job(kind: .memory(pending.command), blocks: panel_blocks_received(engine))
        j.ticket = pending.ticket
        j.ticketExpires = pending.expires
        j.bytes = pending.bytes
        return startJob(j)
    }

    func cancelMemory() {
        guard let pending = pendingMemory else { return }
        dropPending(note: "\(pending.command.title): cancelled. Nothing was sent.")
        afterCall()
    }

    func clearMemoryNote() { memoryNote = nil }

    private func dropPending(note: String?) {
        if let id = ticketID { scheduler.cancel(id); ticketID = nil }
        if pendingMemory != nil {
            panel_memory_cancel(engine)
            pendingMemory = nil
        }
        if let note = note { memoryNote = note }
    }

    // MARK: a backup into a memory

    /// A backup file was chosen to go into a memory and waits for its one confirmation (its
    /// dialog shows what happens). Nothing is sent before `confirmIntoMemory`.
    @Published private(set) var pendingIntoMemory: PendingIntoMemory?

    struct PendingIntoMemory: Equatable {
        var path: String
        var name: String
        var backup: SettingsBackup
        var plan: PutBackPlan
        var memory: Int      // 0 to 4
        var bytes: [UInt8]   // the Store that will be sent
    }

    /// The Store of "into a memory": always the patch bay and the mixer together.
    static func storeBoth(_ memory: Int) -> MemoryCommand { MemoryCommand(kind: .store, memory: memory, patchBay: true, mixer: true) }

    /// Step 1: reads the file and works out what would change meanwhile. Nothing is sent.
    @discardableResult
    func prepareIntoMemory(from url: URL, memory: Int) -> Int32 {
        let what = "Copy Backup to Memory"
        pendingIntoMemory = nil
        guard (0..<5).contains(memory) else { return done(Int32(PANEL_ERR_VALUE), what) }
        guard snap.state == .ready else { return done(Int32(PANEL_ERR_NOT_READY), what) }
        func refuse(_ p: BackupProblem) -> Int32 {
            refusal = Refusal(kind: .value, what: what, text: "\(what), “\(url.lastPathComponent)”: not done. " + p.text)
            afterCall()
            return Int32(PANEL_ERR_VALUE)
        }
        switch SettingsBackup.read(url) {
        case .failure(let p): return refuse(p)
        case .success(let b):
            // A backup of the pretend unit never goes onto the real one, nor into its memories.
            if !transport.isPretend, !b.fromRealUnit { return refuse(.fromPretend) }
            switch PutBackPlan.make(backup: b.block, unit: unitBlock(), ceiling: snap.volumeCeiling) {
            case .failure(let p): return refuse(p)
            case .success(let plan):
                pendingIntoMemory = PendingIntoMemory(path: url.path, name: url.lastPathComponent, backup: b, plan: plan,
                                                      memory: memory, bytes: Self.storeBoth(memory).bytes)
                refusal = nil
                return Int32(PANEL_OK)
            }
        }
    }

    /// The dialog's memory buttons: another memory for the file that is chosen.
    func setIntoMemory(_ memory: Int) {
        guard var p = pendingIntoMemory, (0..<5).contains(memory) else { return }
        p.memory = memory
        p.bytes = Self.storeBoth(memory).bytes
        pendingIntoMemory = p
    }

    func cancelIntoMemory() { pendingIntoMemory = nil }

    /// Why step 2 can't be done right now, for the dialog's button; nil if it can.
    func intoMemoryBlocker() -> String? {
        putBackBlocker() ?? memoryBlocker(pendingIntoMemory.map { Self.storeBoth($0.memory) })
    }

    /// Step 2: the file's settings onto the unit, a Store of the patch bay and the mixer into
    /// the memory, and the settings from before back onto the unit. The settings are read
    /// afresh and saved as a backup first.
    @discardableResult
    func confirmIntoMemory() -> Int32 {
        guard let p = pendingIntoMemory else { return done(Int32(PANEL_ERR_TICKET), "Copy Backup to Memory") }
        let kind = JobKind.intoMemory(p.memory)
        let what = kind.title
        if let r = jobRunningRefusal(what) { return r }
        if notPassed(.mixer, what) || notPassed(.patchAll, what) { return Int32(PANEL_ERR_READ_ONLY) }
        guard snap.state == .ready else { return done(Int32(PANEL_ERR_NOT_READY), what) }
        guard snap.writesAllowed else { return done(Int32(PANEL_ERR_READ_ONLY), what) }
        guard snap.softControl else { return done(Int32(PANEL_ERR_SOFT_CTRL), what) }
        if let r = memoryRefusal(Self.storeBoth(p.memory), what) ?? jobRefusal(what) { return r }
        pendingIntoMemory = nil
        var j = Job(kind: kind, blocks: panel_blocks_received(engine), backup: p.backup, fileName: p.name)
        j.bytes = p.bytes
        return startJob(j)
    }

    // MARK: cursor and keys

    func moveCursor(by step: Int) {
        let order = ChannelNames.screenOrder
        guard let i = order.firstIndex(of: cursor) else { cursor = order[0]; return }
        cursor = order[(i + step + order.count) % order.count]
    }

    /// `repeated`: whether the key is a held key's repeat (true), a fresh press (false), or not
    /// known (nil). Only the stop at 0 dB looks at it; an unknown one counts as a repeat while
    /// the keys haven't been quiet (`keyHoldMicros`), so a held key never runs past the stop.
    func handle(_ key: PanelKey, repeated: Bool? = nil) {
        let s = cursor
        let ch = snap.channels[s.group][s.channel]
        switch key {
        case .cursorLeft: moveCursor(by: -1)
        case .cursorRight: moveCursor(by: 1)
        case .volumeUp: nudgeVolume(s, ch, Self.keyStepDB, repeated: repeated)
        case .volumeDown: nudgeVolume(s, ch, -Self.keyStepDB)
        case .volumeUpBig: nudgeVolume(s, ch, Self.keyBigStepDB, repeated: repeated)
        case .volumeDownBig: nudgeVolume(s, ch, -Self.keyBigStepDB)
        case .panLeft: nudgePan(s, ch, -Self.keyPanStep)
        case .panRight: nudgePan(s, ch, Self.keyPanStep)
        case .panCenter:
            if s.group == ChannelNames.groupMonitor { return }
            setPan(s, UInt32(PANEL_CENTER_PAN))
        case .toggleMonitor: toggleMonitor(s)
        case .toggleSolo: toggleSolo(s)
        case .toggleLink: toggleLink(group: s.group, pair: s.channel / 2)
        }
    }

    private func nudgeVolume(_ s: StripID, _ ch: ChannelState, _ stepDB: Double, repeated: Bool? = nil) {
        let unity = UInt32(PANEL_UNITY_VOLUME)
        // Held at the stop: a held key's repeats stay at 0 dB, and keep the hold going.
        if stepDB > 0, ch.volume == unity, keyStoppedAt == s, keyHoldID != nil, repeated != false {
            keyChange(pauses: dragPauses(volume: s)) {}
            return
        }
        keyStoppedAt = nil
        var target: UInt32
        if ch.volume == 0 {
            if stepDB < 0 { return }
            target = FaderScale.volume(dB: FaderScale.bottomDB)
        } else {
            let d = (panel_volume_db(ch.volume) / Self.keyStepDB).rounded() * Self.keyStepDB + stepDB
            target = d < FaderScale.bottomDB ? 0 : FaderScale.volume(dB: min(d, FaderScale.topDB))
        }
        if abs(panel_volume_db(target)) < 0.01 { target = UInt32(PANEL_UNITY_VOLUME) }
        // From below 0 dB a step up stops at 0 dB, as the fader does, also when it lands there
        // exactly; while the key is held it stays there, and a fresh press goes on.
        if stepDB > 0, ch.volume < unity, target >= unity {
            target = unity
            keyStoppedAt = s
        }
        keyChange(pauses: dragPauses(volume: s)) { self.setVolume(s, target) }
    }

    private func nudgePan(_ s: StripID, _ ch: ChannelState, _ step: Int) {
        if s.group == ChannelNames.groupMonitor { return }
        let p = UInt32(min(Int(PANEL_MAX_PAN), max(0, Int(ch.pan) + step)))
        keyChange(pauses: dragPauses(pan: s)) { self.setPan(s, p) }
    }

    /// Repeated keys count as one drag: the unit's saving is paused until the
    /// keys have been quiet for a moment, so a held key doesn't make it save 30 times a second.
    private func keyChange(pauses: Bool, _ change: () -> Void) {
        if let id = keyHoldID {
            scheduler.cancel(id)
        } else if pauses {
            panel_drag_begin(engine, scheduler.now)
        }
        keyChanging = true
        change()
        keyChanging = false
        keyHoldID = scheduler.schedule(at: scheduler.now + Self.keyHoldMicros) { [weak self] in
            self?.keyHoldID = nil
            self?.releaseKeyHold()
        }
    }

    private func endKeyHold() {
        guard let id = keyHoldID else { return }
        scheduler.cancel(id)
        keyHoldID = nil
        releaseKeyHold()
    }

    private func releaseKeyHold() {
        keyStoppedAt = nil
        panel_drag_end(engine, scheduler.now)
        afterCall()
    }
}
