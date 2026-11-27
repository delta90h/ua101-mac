// The end-user build's windows, drawn off screen into PNG files, for the user
// and the manual. Only the pictures build has it (`make user-pictures`): the
// end-user screens, with the UA-101 played by a stand-in helper and a pretend unit behind it.
// Nothing here reaches the installed helper or the unit, and no window appears on screen.
#if UA101_PICTURES
import AppKit
import SwiftUI

/// Returns the files written; empty if anything failed.
func renderUserWindows(into directory: String) -> [String] {
    try? FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: true)
    var written: [String] = []
    var failed = false
    let inp = ChannelNames.groupInput, out = ChannelNames.groupOutput
    func strip(_ g: Int, _ c: Int) -> StripID { StripID(group: g, channel: c) }

    func shot<V: View>(_ name: String, _ theme: Theme = .dark, _ view: V) {
        let path = (directory as NSString).appendingPathComponent(name + ".png")
        if renderPNG(view, theme: theme, to: path) { written.append(path) } else { failed = true }
    }
    func mixer(_ b: Bench) -> MixerWindowView { MixerWindowView(model: b.model, ui: b.ui, messages: b.messages) }
    func messages(_ b: Bench, height: CGFloat = 520) -> some View {
        MessagesView(messages: b.messages, model: b.model, ui: b.ui).frame(width: MessagesWindow.width, height: height)
    }
    /// The UA-101 (a stand-in), SOFT CTRL in. Its settings are read and changes come on by
    /// themselves, as in the end-user app.
    func ua101() -> Bench {
        let b = Bench()
        b.fake.unit.press(Int32(PANEL_SWITCH_SOFT), in: true)
        b.chooseReal()
        b.settle()
        return b
    }
    func okAll(_ b: Bench) {
        for m in b.messages.now { b.messages.ok(m) }
        b.messages.clearEarlier()
    }
    func peak(_ dB: Float) -> UInt32 { UInt32(Float(PANEL_LEVEL_FULL_SCALE) * pow(10, dB / 20)) }

    // 01. As it opens: the UA-101, changes on by themselves, with
    // levels while an app plays and records.
    let look = ua101()
    look.fake.setStreaming(true)
    look.settle()
    look.fake.levels = HelperLevels(streaming: true, rate: 48000,
                                    capture: [-14, -17, -9, -31, 0, 0, 0, 0, 0, 0, -8, -9].map { $0 == 0 ? 0 : peak($0) },
                                    playback: [-11, -12, 0, 0, 0, 0, 0, 0, 0, 0].map { $0 == 0 ? 0 : peak($0) })
    look.helper.pollLevels()
    look.settle(100_000)
    shot("01-mixer-as-it-opens", .dark, mixer(look))

    // 02, 03. Changes allowed, a working set-up; the dark and the light look.
    let use = ua101()
    use.dressUp()
    use.settle(8_000_000)
    okAll(use)
    shot("02-mixer-changes-allowed", .dark, mixer(use))
    shot("03-mixer-light-look", .light, mixer(use))

    // 04. The Messages window in plain words: a volume, a link, a solo.
    let told = ua101()
    told.model.setVolume(strip(inp, 2), FaderScale.volume(dB: -6))
    told.settle(8_000_000)
    if let v = told.messages.now.first(where: { $0.slot == .volume }) { told.messages.ok(v) }
    told.settle(5_000_000)
    told.model.setLink(group: inp, pair: 1, on: true)
    told.settle(9_000_000)
    told.model.toggleSolo(strip(inp, 2))
    told.settle()
    shot("04-messages-plain-words", .dark, messages(told))

    // 05. The unit kept another value: a warning, in words.
    let kept = ua101()
    kept.fake.unit.ignore(kind: Int(PANEL_F_VOLUME))
    kept.model.setVolume(strip(inp, 2), FaderScale.volume(dB: -3))
    kept.settle(8_000_000)
    shot("05-messages-warning", .dark, messages(kept, height: 420))

    // 06. Patch Bay.
    let patch = ua101()
    patch.model.setPatch(output: 1, source: 2)
    patch.settle(8_000_000)
    shot("06-patch-bay", .dark, PatchBayView(model: patch.model, messages: patch.messages))

    // 07. Driver Settings, as with the real driver while music plays (a stand-in for the driver).
    let drv = PretendDriverSettings(pretend: false)
    _ = drv.setOutput(ms: 8)
    _ = drv.setInput(ms: 8)
    drv.pretendRun(running: true, seconds: 200, playbackSpareMs: 4.2, captureSpareMs: 2.9)
    let driver = DriverSettingsModel(source: drv)
    driver.refresh()
    shot("07-driver-settings", .dark, DriverSettingsView(model: driver))

    // 08 to 10. Open Backup: a backup saved, then changes; the dialog, and the window after.
    let back = ua101()
    back.model.saveBackup()
    back.settle()
    back.model.clearBackupNote()
    back.model.setVolume(strip(inp, 0), FaderScale.volume(dB: -6))
    back.settle(8_000_000)
    back.model.setPatch(output: 1, source: 3)
    back.settle(8_000_000)
    okAll(back)
    if let name = back.backupFiles.first { back.model.preparePutBack(from: back.backups.appendingPathComponent(name)) }
    shot("08-open-backup", .dark, PutBackView(model: back.model, messages: back.messages))
    back.model.confirmPutBack()
    back.settle(8_000_000)
    shot("09-messages-after-open-backup", .dark, messages(back, height: 420))

    // 10 to 13. Recall, Save and Reset: Load's confirmation with its warning, the window
    // after, Copy Backup to Memory, Reset to Factory.
    let box = ua101()
    box.model.setVolume(strip(inp, 0), FaderScale.volume(dB: -6))
    box.settle(8_000_000)
    okAll(box)
    box.model.prepareMemory(MemoryCommand(kind: .load, memory: 2))
    shot("10-recall-memory-confirm", .dark, MemoryDialogView(model: box.model, purpose: .load, memory: 2, messages: box.messages))
    box.model.confirmMemory()
    box.settle(11_000_000)
    shot("11-messages-after-recall", .dark, messages(box, height: 420))
    okAll(box)
    if let name = box.backupFiles.first(where: { $0.contains("before Recall Memory 3") }) {
        box.model.prepareIntoMemory(from: box.backups.appendingPathComponent(name), memory: 4)
    }
    shot("12-copy-backup-to-memory", .dark, IntoMemoryView(model: box.model, messages: box.messages))
    box.model.cancelIntoMemory()
    shot("13-reset-to-factory", .dark, MemoryDialogView(model: box.model, purpose: .reset, reset: .current, messages: box.messages))

    // (The two "look only" pictures, without the user's go-ahead file, went with link version 18.)

    // 15, 16. Lock Changes: the mixer, and what the Messages window says.
    let locked = ua101()
    locked.model.lockChanges(true)
    locked.settle()
    shot("15-mixer-changes-locked", .dark, mixer(locked))
    shot("16-messages-changes-locked", .dark, messages(locked, height: 300))

    // 14. UA-101 switched off, or the driver not installed.
    let none = Bench()
    none.fake.installed = false
    none.chooseReal()
    shot("14-mixer-no-ua101", .dark, mixer(none))

    // 17 to 20, for the manual.
    // 17, 18. Save Memory: the first step (which memory, what to save), then its warning.
    let keep = ua101()
    okAll(keep)
    shot("17-save-memory-choose", .dark, MemoryDialogView(model: keep.model, purpose: .store, memory: 4, messages: keep.messages))
    keep.model.prepareMemory(MemoryCommand(kind: .store, memory: 4))
    shot("18-save-memory-confirm", .dark, MemoryDialogView(model: keep.model, purpose: .store, memory: 4, messages: keep.messages))
    keep.model.cancelMemory()

    // 19. Recall Memory, the first step, with what Memory 3 would change (1.1).
    keep.memoryToShow(2, real: true)
    shot("19-recall-memory-choose", .dark, MemoryDialogView(model: keep.model, purpose: .load, memory: 2, messages: keep.messages))

    // 20. SOFT CTRL out: the unit ignores the mixer.
    let soft = Bench()
    soft.chooseReal()
    soft.settle()
    shot("20-mixer-soft-ctrl-out", .dark, mixer(soft))

    return failed ? [] : written
}
#endif
