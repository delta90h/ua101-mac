// Draws every window off screen into PNG files, each from a real model that
// was driven against the pretend unit into the state the picture shows.
// No window appears on the Mac's screen.
import AppKit
import SwiftUI

/// A panel with its own pretend unit, a stand-in for the helper with another pretend unit
/// behind it (the "real" one), and hand-moved time. Nothing here reaches the installed helper.
final class Bench {
    let time = ManualScheduler()
    let unit: PretendUnit
    let fake: FakeHelper
    let helper: HelperTransport
    let model: PanelModel
    let ui = PanelUI()
    let messages: MessageCenter
    /// The messages' clock: 2026-11-16 18:30:00 here, as the bench's time starts, then
    /// following the bench's time, so the pictures always show the same times.
    static let start: Date = {
        var c = DateComponents()
        (c.year, c.month, c.day, c.hour, c.minute) = (2026, 11, 16, 18, 30)
        return Calendar.current.date(from: c) ?? Date(timeIntervalSince1970: 0)
    }()
    /// Times of the messages, on the bench's clock, and how many arrivals opened the window.
    private(set) var arrivals = 0
    /// The user's rule of 2026-11-23, with a stand-in for the pop-up window: what it showed,
    /// and the OK it waits for.
    private(set) var popups: MessagePopups!
    private(set) var popupsShown: [MessageCenter.Message] = []
    private(set) var popupOK: (() -> Void)?
    /// The stand-in's OK, as a click would.
    func pressPopupOK() {
        let r = popupOK
        popupOK = nil
        r?()
    }
    /// Where this bench's backups go: a folder of its own, thrown away with it. Never the
    /// user's Documents.
    let backups: URL
    private static var benches = 0

    init(connect: Bool = true) {
        unit = PretendUnit(scheduler: time)
        fake = FakeHelper(scheduler: time)
        helper = HelperTransport(link: fake, scheduler: time)
        model = PanelModel(transport: unit, scheduler: time)
        let clock = time
        Bench.benches += 1
        backups = URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
            .appendingPathComponent("ua101panel-bench-\(getpid())-\(Bench.benches)", isDirectory: true)
        model.backupFolder = backups
        model.backupClock = { Bench.start.addingTimeInterval(Double(clock.now) / 1e6 - 1) }
        messages = MessageCenter(model: model, clock: { Bench.start.addingTimeInterval(Double(clock.now) / 1e6 - 1) })
        popups = MessagePopups(messages: messages) { [unowned self] m, ok in
            self.popupsShown.append(m)
            self.popupOK = ok
        }
        messages.onArrival = { [unowned self] m in
            self.arrivals += 1
            self.popups.arrived(m)
        }
        model.onChooseDevice = { [unowned self] real in self.model.use(real ? self.helper : self.unit) }
        if connect {
            model.connect()
            settle()
        }
    }

    deinit { try? FileManager.default.removeItem(at: backups) }

    /// The names of the backup files this bench's model has written, oldest first.
    var backupFiles: [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: backups.path)) ?? []).sorted()
    }

    /// The UA-101 button, then time for the answers.
    func chooseReal() {
        model.onChooseDevice?(true)
        settle()
    }

    /// Lets everything in flight arrive and be checked (2 s of pretend time by default).
    func settle(_ micros: UInt64 = 2_000_000) { time.advance(micros) }

    /// Changes allowed and SOFT CTRL pressed in: the mixer works.
    func openUp() {
        model.allowChanges(true)
        unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        settle()
    }

    /// For the pictures of Recall Memory: memory `m` of the unit (`real`: the one behind the
    /// stand-in helper) holds the settings in use with INPUT 1 at -6 dB, INPUT 3 and 4 muted and
    /// the patch bay's OUTPUT 3-4 from INPUT 1-2; then the app reads it, as the dialog does.
    func memoryToShow(_ m: Int, real: Bool) {
        let u = real ? fake.unit : unit
        var b = [UInt8](repeating: 0, count: 256)
        panel_sim_block(u.sim, &b)
        let inp = ChannelNames.groupInput
        let at = PanelModel.volumeOffset(StripID(group: inp, channel: 0))
        b.replaceSubrange(at..<at + 6, with: PanelModel.volumeDigits(FaderScale.volume(dB: -6)))
        for c in 2..<4 { let on = PanelModel.onPlace(StripID(group: inp, channel: c)); b[on.byte] |= UInt8(1 << on.bit) }
        b[13] = 2 // patch bay, OUTPUT 3-4: INPUT 1-2
        panel_sim_set_memory(u.sim, UInt32(m), b)
        model.readMemoryPreview(m)
        settle()
    }

    /// A believable working set-up, for the pictures.
    func dressUp() {
        let inp = ChannelNames.groupInput, out = ChannelNames.groupOutput
        func strip(_ g: Int, _ c: Int) -> StripID { StripID(group: g, channel: c) }
        model.setLink(group: inp, pair: 0, on: true)
        model.setVolume(strip(inp, 0), FaderScale.volume(dB: -6))
        model.setVolume(strip(inp, 2), FaderScale.volume(dB: -12))
        model.setPan(strip(inp, 2), 0x2800)
        model.setVolume(strip(inp, 3), FaderScale.volume(dB: -18))
        model.setPan(strip(inp, 3), 0x5800)
        for c in 4..<10 { model.setMonitor(strip(inp, c), on: false) }
        model.setLink(group: out, pair: 0, on: true)
        model.setVolume(strip(out, 0), FaderScale.volume(dB: -3))
        for c in 2..<10 { model.setMonitor(strip(out, c), on: false) }
        model.setVolume(strip(out, 4), 0)
        model.setVolume(strip(out, 5), 0)
        model.setVolume(strip(ChannelNames.groupMonitor, 0), FaderScale.volume(dB: -9))
        model.cursor = strip(inp, 2)
        settle()
    }
}

@discardableResult
func renderPNG<V: View>(_ view: V, theme: Theme, to path: String) -> Bool {
    let host = NSHostingView(rootView: view.themed(theme))
    let size = host.fittingSize
    guard size.width > 1, size.height > 1 else { return false }
    host.frame = NSRect(origin: .zero, size: size)
    let window = NSWindow(contentRect: host.frame, styleMask: [.borderless], backing: .buffered, defer: false)
    window.appearance = NSAppearance(named: theme.scheme == .dark ? .darkAqua : .aqua)
    window.contentView = host
    host.layoutSubtreeIfNeeded()
    // One turn of the run loop, so SwiftUI finishes its first layout.
    RunLoop.current.run(until: Date().addingTimeInterval(0.05))
    guard let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds) else { return false }
    host.cacheDisplay(in: host.bounds, to: rep)
    guard let data = rep.representation(using: .png, properties: [:]) else { return false }
    return (try? data.write(to: URL(fileURLWithPath: path))) != nil
}

/// Returns the files written; empty if anything failed.
func renderAllWindows(into directory: String) -> [String] {
    try? FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true)
    var written: [String] = []
    var failed = false

    func shot<V: View>(_ name: String, _ theme: Theme = .dark, _ view: V) {
        let path = (directory as NSString).appendingPathComponent(name + ".png")
        let ok = autoreleasepool { renderPNG(view, theme: theme, to: path) }
        if ok { written.append(path) } else { failed = true }
    }
    func mixer(_ b: Bench) -> MixerWindowView { MixerWindowView(model: b.model, ui: b.ui, messages: b.messages) }
    func messages(_ b: Bench, scale: CGFloat = 1, height: CGFloat = 600) -> some View {
        b.ui.messagesScale = scale
        return MessagesView(messages: b.messages, model: b.model, ui: b.ui).frame(width: MessagesWindow.width, height: height)
    }

    // 1. As the app opens: read-only, and this unit's SOFT CTRL is out.
    let opened = Bench()
    shot("01-mixer-as-it-opens", .dark, mixer(opened))

    // 2. In use.
    let inUse = Bench()
    inUse.openUp()
    inUse.dressUp()
    // Pretend levels, so the meters can be judged. The other pictures show them as they are today: dark.
    inUse.model.meters.showStill()
    shot("02-mixer-in-use", .dark, mixer(inUse))
    shot("03-mixer-in-use-light-look", .light, mixer(inUse))
    inUse.model.meters.clear()

    // 4. The unit didn't take a change.
    let disagreed = Bench()
    disagreed.openUp()
    disagreed.dressUp()
    disagreed.unit.ignore(kind: Int(PANEL_F_VOLUME))
    disagreed.model.setVolume(StripID(group: ChannelNames.groupInput, channel: 2), FaderScale.volume(dB: -3))
    disagreed.settle()
    shot("04-mixer-unit-disagreed", .dark, mixer(disagreed))

    // 5. A click while read-only.
    let refused = Bench()
    refused.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
    refused.settle()
    refused.model.setVolume(StripID(group: ChannelNames.groupInput, channel: 0), FaderScale.volume(dB: -6))
    shot("05-mixer-read-only-click-refused", .dark, mixer(refused))

    // 6. The unit stopped answering.
    let silent = Bench()
    silent.openUp()
    silent.dressUp()
    silent.unit.setSilent(true)
    silent.model.refresh()
    silent.settle(6_000_000)
    shot("06-mixer-unit-not-answering", .dark, mixer(silent))

    // 7. 192 kHz: six mixer channels.
    let fast = Bench()
    fast.unit.setRate(code: 4)
    fast.model.reconnect()
    fast.openUp()
    shot("07-mixer-192k", .dark, mixer(fast))

    // 8. Patch bay.
    let patch = Bench()
    patch.openUp()
    patch.model.setPatch(output: 1, source: 2)
    patch.model.setPatch(output: 4, source: 1)
    patch.settle()
    shot("08-patch-bay", .dark, PatchBayView(model: patch.model, messages: patch.messages))
    let patchLocked = Bench()
    patchLocked.model.setPatch(output: 0, source: 2)
    shot("09-patch-bay-read-only", .dark, PatchBayView(model: patchLocked.model, messages: patchLocked.messages))

    // 10 to 14. Load, Store, Reset.
    let memory = Bench()
    memory.openUp()
    memory.memoryToShow(2, real: false)
    shot("10-load-step-1-choose", .dark, MemoryDialogView(model: memory.model, purpose: .load, memory: 2, messages: memory.messages))
    memory.model.prepareMemory(MemoryCommand(kind: .load, memory: 2, patchBay: true, mixer: true))
    shot("11-load-step-2-confirm", .dark, MemoryDialogView(model: memory.model, purpose: .load, memory: 2, messages: memory.messages))
    memory.model.cancelMemory()
    memory.model.clearMemoryNote()
    shot("12-store-step-1-choose", .dark, MemoryDialogView(model: memory.model, purpose: .store, memory: 0, patchBay: false, mixer: true, messages: memory.messages))
    shot("13-reset-step-1-choose", .dark, MemoryDialogView(model: memory.model, purpose: .reset, memory: 0, reset: .everything, messages: memory.messages))
    memory.model.prepareMemory(MemoryCommand(kind: .resetAll))
    shot("14-reset-step-2-confirm", .dark, MemoryDialogView(model: memory.model, purpose: .reset, memory: 0, reset: .everything, messages: memory.messages))
    let memoryLocked = Bench()
    shot("15-load-read-only", .dark, MemoryDialogView(model: memoryLocked.model, purpose: .load, messages: memoryLocked.messages))

    // 16. Driver settings, as with the real driver while music plays: a stand-in for the
    // driver, so nothing is asked of Core Audio here.
    let drv = PretendDriverSettings(pretend: false)
    _ = drv.setOutput(ms: 12)
    drv.pretendRun(running: true, seconds: 200, playbackSpareMs: 4.2, captureSpareMs: 2.9)
    let driver = DriverSettingsModel(source: drv)
    driver.refresh()
    shot("16-driver-settings", .dark, DriverSettingsView(model: driver))

    // 17. The pretend unit's front panel (only for trying the app out).
    shot("17-pretend-unit-front-panel", .dark, PretendFrontPanelView(model: inUse.model, unit: inUse.unit))

    // 18 to 20. The light look of the dialogs.
    shot("18-patch-bay-light-look", .light, PatchBayView(model: patch.model, messages: patch.messages))
    memory.model.cancelMemory()
    memory.model.clearMemoryNote()
    shot("19-load-step-1-light-look", .light, MemoryDialogView(model: memory.model, purpose: .load, memory: 2, messages: memory.messages))
    shot("20-driver-settings-light-look", .light, DriverSettingsView(model: driver))

    // 21. The real unit chosen: look only until Allow Changes, with levels while an app plays and records.
    let real = Bench()
    real.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
    real.chooseReal()
    real.fake.setStreaming(true)
    real.settle()
    func peak(_ dB: Float) -> UInt32 { UInt32(Float(PANEL_LEVEL_FULL_SCALE) * pow(10, dB / 20)) }
    real.fake.levels = HelperLevels(streaming: true, rate: 48000,
                                    capture: [-14, -17, -9, -31, 0, 0, 0, 0, 0, 0, -8, -9].map { $0 == 0 ? 0 : peak($0) },
                                    playback: [-11, -12, 0, 0, 0, 0, 0, 0, 0, 0].map { $0 == 0 ? 0 : peak($0) })
    real.helper.pollLevels()
    real.settle(100_000)
    shot("21-ua101-look-only", .dark, mixer(real))

    // 22. The real unit chosen, but the helper isn't installed (or is an older one).
    let noHelper = Bench()
    noHelper.fake.installed = false
    noHelper.chooseReal()
    shot("22-ua101-no-helper", .dark, mixer(noHelper))

    // (23 was the UA-101 without the user's go-ahead file: gone with link version 18.)

    // 24. The real unit: changes on, for what the helper passes on. INPUT 1's
    // volume was set to the first test's value and read back; the link of INPUT 1-2 was then
    // asked for (passed on since link version 14; its read-back hasn't come yet).
    let volume = Bench()
    volume.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
    volume.chooseReal()
    volume.model.allowChanges(true)
    volume.settle()
    volume.model.setVolumeExact(StripID(group: ChannelNames.groupInput, channel: 0), 0x100001)
    volume.settle()
    volume.model.setLink(group: ChannelNames.groupInput, pair: 0, on: true)
    shot("24-ua101-changes-on-volume-input-1", .dark, mixer(volume))

    // 25. The real unit's patch bay: OUTPUT 3-4 was changed to INPUT 3-4
    // and read back.
    let patched = Bench()
    patched.chooseReal()
    patched.model.allowChanges(true)
    patched.settle()
    patched.model.setPatch(output: 1, source: 3)
    patched.settle()
    shot("25-ua101-patch-bay-output-3-4", .dark, PatchBayView(model: patched.model, messages: patched.messages))

    // 26. The real unit: INPUT 1's pan was set to the middle and read back.
    let panned = Bench()
    panned.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
    panned.chooseReal()
    panned.model.allowChanges(true)
    panned.settle()
    panned.model.setPanExact(StripID(group: ChannelNames.groupInput, channel: 0), 0x4000)
    panned.settle()
    shot("26-ua101-changes-on-pan-input-1", .dark, mixer(panned))

    // 27. The real unit: INPUT 3-4 linked while the two channels held
    // different volumes, then INPUT 3 soloed; both read back.
    let linked = Bench()
    linked.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
    linked.chooseReal()
    linked.model.allowChanges(true)
    linked.settle()
    linked.model.setVolumeExact(StripID(group: ChannelNames.groupInput, channel: 2), 0x16A09E)
    linked.settle()
    linked.model.clearVolumeNote()
    linked.model.setLink(group: ChannelNames.groupInput, pair: 1, on: true)
    linked.settle()
    linked.model.toggleSolo(StripID(group: ChannelNames.groupInput, channel: 2))
    linked.settle()
    shot("27-ua101-link-and-solo-input-3-4", .dark, mixer(linked))

    // 28. The real unit's patch bay: Hard Thru was asked for and waits for
    // its second step. Nothing has been sent.
    let thru = Bench()
    thru.chooseReal()
    thru.model.allowChanges(true)
    thru.settle()
    thru.model.askHardThru()
    shot("28-ua101-patch-bay-hard-thru-asks", .dark, PatchBayView(model: thru.model, messages: thru.messages))

    // 29 to 31. The Messages window (2026-11-16): beside the mixer, every note and warning.
    // 29: picture 27's set-up, with the volume's read-back OK'd into Earlier.
    let told = Bench()
    told.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
    told.chooseReal()
    told.model.allowChanges(true)
    told.settle()
    told.model.setVolumeExact(StripID(group: ChannelNames.groupInput, channel: 2), 0x16A09E)
    told.settle(8_000_000)
    if let v = told.messages.now.first(where: { $0.slot == .volume }) { told.messages.ok(v) }
    told.settle(5_000_000)
    told.model.setLink(group: ChannelNames.groupInput, pair: 1, on: true)
    told.settle(9_000_000)
    told.model.toggleSolo(StripID(group: ChannelNames.groupInput, channel: 2))
    told.settle()
    shot("29-messages-window", .dark, messages(told))
    // 30: the unit kept another value (picture 04's mixer, whose Messages button is red): a
    // warning, red, at the top.
    shot("30-messages-window-warning", .dark, messages(disagreed, height: 420))
    // 31: the light look, the text one step larger.
    shot("31-messages-window-light-larger-text", .light, messages(told, scale: 1.15))

    // 32 to 35. Driver settings in its other states.
    // 32. The shortest delays left too little room and sound came late; a longer playback
    // delay is chosen in the window and not applied yet.
    let thin = PretendDriverSettings(pretend: false)
    _ = thin.setOutput(ms: 10)
    _ = thin.setInput(ms: 3)
    thin.pretendRun(running: false, seconds: 754, playbackSpareMs: -1.5, captureSpareMs: 0.6, late: 72)
    let thinModel = DriverSettingsModel(source: thin)
    thinModel.refresh()
    thinModel.choose(output: DriverValues.outputIndex(14) ?? 0)
    shot("32-driver-settings-not-applied", .dark, DriverSettingsView(model: thinModel))
    // 33. The unit is off: Core Audio has no UA-101 to ask.
    let off = PretendDriverSettings(pretend: false)
    off.state = DriverState()
    let offModel = DriverSettingsModel(source: off)
    offModel.refresh()
    shot("33-driver-settings-unit-off", .dark, DriverSettingsView(model: offModel))
    // 34. With the pretend unit chosen.
    let pretendDriver = DriverSettingsModel(source: PretendDriverSettings())
    pretendDriver.refresh()
    shot("34-driver-settings-pretend-unit", .dark, DriverSettingsView(model: pretendDriver))
    // 35. Both sliders below the recommended values: the red warnings, while the driver has
    // them and sound is being lost.
    let low = PretendDriverSettings(pretend: false)
    _ = low.setOutput(ms: 6)
    _ = low.setInput(ms: 2)
    low.pretendRun(running: true, seconds: 41, playbackSpareMs: -2.1, captureSpareMs: -0.2, late: 3168, missing: 912)
    let lowModel = DriverSettingsModel(source: low)
    lowModel.refresh()
    shot("35-driver-settings-below-recommended", .dark, DriverSettingsView(model: lowModel))

    // 36 to 38. Open Backup, with the UA-101.
    // A backup was saved, then INPUT 1's volume, INPUT 3's pan, a patch bay row and a link
    // were changed: the dialog shows what putting the backup back would change. Nothing is sent.
    let back = Bench()
    back.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
    back.chooseReal()
    back.model.allowChanges(true)
    back.settle()
    back.model.saveBackup()
    back.settle()
    back.model.clearBackupNote()
    back.model.setVolumeExact(StripID(group: ChannelNames.groupInput, channel: 0), 0x100001)
    back.settle()
    back.model.setPanExact(StripID(group: ChannelNames.groupInput, channel: 2), 0x4000)
    back.settle()
    back.model.setPatch(output: 1, source: 3)
    back.settle()
    back.model.setLink(group: ChannelNames.groupInput, pair: 0, on: true)
    back.settle()
    for m in back.messages.now { back.messages.ok(m) }
    back.messages.clearEarlier()
    if let name = back.backupFiles.first {
        back.model.preparePutBack(from: back.backups.appendingPathComponent(name))
    }
    shot("36-put-back-from-backup", .dark, PutBackView(model: back.model, messages: back.messages))
    shot("37-put-back-light-look", .light, PutBackView(model: back.model, messages: back.messages))
    // 38: after Put Back: the Messages window says the unit matches the backup.
    back.model.confirmPutBack()
    back.settle(8_000_000)
    shot("38-messages-after-put-back", .dark, messages(back, height: 420))

    // 39 to 43. Recall, Save and Reset to Factory with the UA-101: the unit
    // behind the stand-in helper.
    let box = Bench()
    box.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
    box.chooseReal()
    box.model.allowChanges(true)
    box.settle()
    // 39: Reset to Factory as Roland's own dialog offers it: the first step, where nothing is sent.
    shot("39-reset-to-factory-step-1", .dark, MemoryDialogView(model: box.model, purpose: .reset, reset: .current, messages: box.messages))
    box.model.setVolumeExact(StripID(group: ChannelNames.groupInput, channel: 0), 0x100001)
    box.settle()
    for m in box.messages.now { box.messages.ok(m) }
    box.messages.clearEarlier()
    // 40: Recall Memory 3, the confirmation: the warning, the one message, and what happens first.
    box.model.prepareMemory(MemoryCommand(kind: .load, memory: 2))
    shot("40-load-ua101-step-2-confirm", .dark, MemoryDialogView(model: box.model, purpose: .load, memory: 2, messages: box.messages))
    // 41: after it: the Messages window says what changed and where the files are.
    box.model.confirmMemory()
    box.settle(4_000_000)
    shot("41-messages-after-load", .dark, messages(box, height: 420))
    box.settle(7_000_000)
    for m in box.messages.now { box.messages.ok(m) }
    box.messages.clearEarlier()
    // 42: a backup file into a memory: the file, the memory, what happens in order.
    if let name = box.backupFiles.first(where: { $0.contains("before Recall Memory 3") }) {
        box.model.prepareIntoMemory(from: box.backups.appendingPathComponent(name), memory: 4)
    }
    shot("42-put-a-backup-into-a-memory", .dark, IntoMemoryView(model: box.model, messages: box.messages))
    // 43: after it.
    box.model.confirmIntoMemory()
    box.settle(20_000_000)
    shot("43-messages-after-into-a-memory", .dark, messages(box, height: 420))

    return failed ? [] : written
}
