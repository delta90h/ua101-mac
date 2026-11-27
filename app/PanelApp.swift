// The app itself: its windows, sheets and menus. Everything a menu item or a
// key does goes through PanelModel, the same calls the self-test makes.
import AppKit
import Combine
import SwiftUI
import UniformTypeIdentifiers

/// The one model the running app has, and the two units it can talk to: the pretend one and
/// the real UA-101 through the driver's helper (only the changes the helper passes on).
/// The end-user build (UA101_USER) has no pretend unit: it talks to the UA-101 only.
final class AppCore {
    static let shared = AppCore()
    static let deviceKey = "device", deviceReal = "ua101", devicePretend = "pretend"
    let scheduler = LiveScheduler()
    #if !UA101_USER
    let unit: PretendUnit
    #endif
    let helper: HelperTransport
    let model: PanelModel
    /// Uninstall UA-101… is under way (app/Uninstall.swift): the helper and the driver go, and the
    /// red messages that brings aren't popped up.
    var uninstalling = false
    let ui = PanelUI()
    /// Every note and warning, and the window that shows them.
    let messages: MessageCenter
    let messagesWindow: MessagesWindow
    /// The notes pad (View menu, Show Notes) and its window.
    let notes = NotesStore(url: AppCore.backupFolder.appendingPathComponent(NotesStore.fileName))
    private(set) var notesWindow: NotesWindow!
    /// The pop-up a red message brings, and its window.
    private(set) var popups: MessagePopups!
    private let popupWindow = PopupWindow()
    /// The driver's own settings (Driver Settings…): the real driver through Core Audio, asked
    /// off the main thread, and a pretend one for when the pretend unit is chosen.
    let driver: DriverSettingsModel
    private let realDriver = CoreAudioDriverSettings()
    #if !UA101_USER
    private let pretendDriver = PretendDriverSettings()
    #endif
    /// Patch Bay and Driver Settings: windows of their own (2026-11-20), one of each, made the
    /// first time they are asked for.
    private(set) lazy var patchBayWindow: DialogWindow = {
        let (model, ui, messages) = (self.model, self.ui, self.messages)
        let w = DialogWindow(kind: .patchBay) { w in
            AnyView(DialogWindowContent(ui: ui, window: w, content: PatchBayView(model: model, close: { w.close() }, messages: messages,
                                                                     openMessages: { ui.showMessages() })))
        }
        // Closing the Patch Bay drops a Hard Thru that was asked for and not sent. Nothing is
        // sent by closing it, as nothing was when it was a sheet.
        w.onClose = { model.cancelHardThru() }
        return w
    }()
    private(set) lazy var driverWindow: DialogWindow = {
        let (ui, driver) = (self.ui, self.driver)
        return DialogWindow(kind: .driverSettings) { w in
            AnyView(DialogWindowContent(ui: ui, window: w, content: DriverSettingsView(model: driver, close: { w.close() })))
        }
    }()

    private init() {
        // Nothing is connected until UA-101 is chosen (end-user build: until the app has opened).
        helper = HelperTransport(link: XPCHelperLink(), scheduler: scheduler, pollHz: 15)
        #if UA101_USER
        model = PanelModel(transport: helper, scheduler: scheduler)
        #else
        unit = PretendUnit(scheduler: scheduler)
        model = PanelModel(transport: unit, scheduler: scheduler)
        #endif
        messages = MessageCenter(model: model)
        messagesWindow = MessagesWindow(messages: messages, model: model, ui: ui)
        // Nothing is asked of Core Audio until the window is opened with UA-101 chosen.
        #if UA101_USER
        driver = DriverSettingsModel(source: realDriver, queue: DispatchQueue(label: "ua101.driver-settings"))
        #else
        driver = DriverSettingsModel(source: pretendDriver, queue: DispatchQueue(label: "ua101.driver-settings"))
        #endif
        if let k = UserDefaults.standard.object(forKey: PanelUI.messagesScaleKey) as? Double, k.isFinite {
            ui.messagesScale = min(PanelUI.messagesScales.last!, max(PanelUI.messagesScales.first!, CGFloat(k)))
        }
        ui.showMessages = { [unowned self] in self.messagesWindow.show() }
        notesWindow = NotesWindow(store: notes, ui: ui)
        ui.showNotes = { [unowned self] in self.notesWindow.show() }
        notes.onError = { e in
            AppCore.record(e)
            let a = NSAlert()
            a.messageText = "The notes could not be saved"
            a.informativeText = e + "\n\nThe words are still in the Notes window; the app tries again after the next change."
            a.runModal()
        }
        ui.patchBayOwnWindow = PanelUI.ownWindow(PanelUI.patchBayOwnWindowKey, in: .standard)
        ui.driverSettingsOwnWindow = PanelUI.ownWindow(PanelUI.driverSettingsOwnWindowKey, in: .standard)
        ui.showPatchBay = { [unowned self] in self.openPatchBay() }
        ui.showDriverSettings = { [unowned self] in self.openDriverSettings() }
        // A design rule: a grey message only goes into the list (the Messages
        // window opens only when the user opens it); a red one pops up, with OK only (it already happened).
        // Just after the change, not inside it: nothing is made mid-update.
        popups = MessagePopups(messages: messages) { [popupWindow] m, ok in popupWindow.show(m, ok: ok) }
        messages.onArrival = { [unowned self] m in DispatchQueue.main.async { if !self.uninstalling { self.popups.arrived(m) } } }
        ui.look = UserDefaults.standard.string(forKey: Look.defaultsKey).flatMap(Look.init(rawValue:)) ?? .system
        ui.rememberSize = SizeMemory.isOn(.standard)
        model.onChooseDevice = { [unowned self] real in self.choose(real: real) }
        model.onRecord = { AppCore.record($0) }
        model.backupFolder = AppCore.backupFolder
        // A backup the user asked for: shown in the Finder, so it is plain where it went.
        model.onBackupSaved = { NSWorkspace.shared.activateFileViewerSelecting([$0]) }
        driver.onRecord = { AppCore.record($0) }
    }

    /// Driver Settings… (⌘, in the Device menu and the button on the mixer's top line): the
    /// driver that goes with the unit chosen, read afresh; a sheet on the mixer, or its own
    /// window when the View menu says so.
    func openDriverSettings() {
        followDriver()
        if ui.driverSettingsOwnWindow { driverWindow.show() } else { openSheet(.driverSettings) }
    }

    /// Patch Bay (its button, Device menu's Patch Bay…): a sheet or its own window.
    func openPatchBay() {
        if ui.patchBayOwnWindow { patchBayWindow.show() } else { openSheet(.patchBay) }
    }

    /// A sheet on the mixer, if no other one is open (one sheet at a time, as before).
    private func openSheet(_ sheet: PanelUI.Sheet) {
        if ui.sheet == nil { ui.sheet = sheet }
    }

    /// The View menu's choice changed: the one it was shown in closes (nothing is sent by
    /// closing either), and the choice is kept for the next start.
    func setOwnWindow(patchBay: Bool? = nil, driverSettings: Bool? = nil) {
        if let on = patchBay, on != ui.patchBayOwnWindow {
            if ui.sheet == .patchBay { ui.sheet = nil }
            patchBayWindow.close()
            ui.patchBayOwnWindow = on
            UserDefaults.standard.set(on, forKey: PanelUI.patchBayOwnWindowKey)
        }
        if let on = driverSettings, on != ui.driverSettingsOwnWindow {
            if ui.sheet == .driverSettings { ui.sheet = nil }
            driverWindow.close()
            ui.driverSettingsOwnWindow = on
            UserDefaults.standard.set(on, forKey: PanelUI.driverSettingsOwnWindowKey)
        }
    }

    /// The window shows the driver that goes with the unit chosen: when the Device menu's
    /// choice changes while it is open, it follows.
    private func followDriver() {
        #if UA101_USER
        let source: DriverSettingsSource = realDriver
        #else
        let source: DriverSettingsSource = model.transport.isPretend ? pretendDriver : realDriver
        #endif
        if driver.source !== source { driver.use(source) } else { driver.refresh() }
    }

    /// Where backups of the unit's settings are kept (Device menu, Save Backup…).
    static let backupFolder = URL(fileURLWithPath: NSHomeDirectory() + "/Documents/UA-101 Backups", isDirectory: true)

    /// Device menu, "Open Backup…", step 1: choose a file. Nothing is sent: the file
    /// is read, and its dialog then shows what would change and asks.
    func choosePutBack() {
        guard ui.sheet == nil else { return }
        let panel = NSOpenPanel()
        panel.title = "Open Backup"
        panel.message = "Choose a backup of the unit's settings. Nothing is sent yet: the next step shows what would change."
        panel.prompt = "Choose"
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        if let type = UTType(filenameExtension: SettingsBackup.fileExtension) { panel.allowedContentTypes = [type] }
        if FileManager.default.fileExists(atPath: Self.backupFolder.path) { panel.directoryURL = Self.backupFolder }
        guard panel.runModal() == .OK, let url = panel.url else { return }
        if model.preparePutBack(from: url) == Int32(PANEL_OK) { ui.sheet = .putBack }
    }

    /// Device menu, "Copy Backup to Memory…", step 1: choose a file. Nothing is sent: its
    /// dialog then asks which memory, shows what happens, and has the one button.
    func chooseIntoMemory() {
        guard ui.sheet == nil else { return }
        let panel = NSOpenPanel()
        panel.title = "Copy Backup to Memory"
        panel.message = "Choose a backup of the unit's settings (for a memory you recalled earlier: its “Memory … as read” file). Nothing is sent yet."
        panel.prompt = "Choose"
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        if let type = UTType(filenameExtension: SettingsBackup.fileExtension) { panel.allowedContentTypes = [type] }
        if FileManager.default.fileExists(atPath: Self.backupFolder.path) { panel.directoryURL = Self.backupFolder }
        guard panel.runModal() == .OK, let url = panel.url else { return }
        if model.prepareIntoMemory(from: url, memory: Self.memoryNamed(in: url.lastPathComponent) ?? 0) == Int32(PANEL_OK) { ui.sheet = .intoMemory }
    }

    /// The memory a file's name speaks of ("… Memory 3 as read.ua101" gives 2), so the dialog
    /// starts on the memory the file was read from. The user can still choose another.
    static func memoryNamed(in name: String) -> Int? {
        guard let r = name.range(of: "Memory "), let digit = name[r.upperBound...].first, let n = Int(String(digit)),
              (1...5).contains(n) else { return nil }
        return n - 1
    }

    /// Where every change sent to the real unit is written down, with what came back.
    static let recordPath = NSHomeDirectory() + "/Library/Logs/UA-101 Control.log"
    private static let stamp: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"
        return f
    }()

    /// Set by Uninstall UA-101… with Remove Everything: the record is about to go, and isn't
    /// written again.
    static var recordStopped = false

    static func record(_ line: String) {
        guard !recordStopped else { return }
        guard let data = "\(stamp.string(from: Date()))  \(line)\n".data(using: .utf8) else { return }
        if !FileManager.default.fileExists(atPath: recordPath) {
            FileManager.default.createFile(atPath: recordPath, contents: nil)
        }
        guard let file = FileHandle(forWritingAtPath: recordPath) else { return }
        file.seekToEndOfFile()
        file.write(data)
        file.closeFile()
    }

    /// The device buttons. The choice is kept for the next start.
    func choose(real: Bool) {
        #if UA101_USER
        // Only the UA-101 here. Asked once, as the app opens.
        model.startUnit()
        #else
        model.use(real ? helper : unit)
        UserDefaults.standard.set(real ? Self.deviceReal : Self.devicePretend, forKey: Self.deviceKey)
        if driverWindow.isShown || ui.sheet == .driverSettings { followDriver() }
        #endif
    }

    /// Levels are asked for only while the mixer's window can be seen.
    func windowVisibilityChanged() {
        helper.metersShown = NSApp.windows.contains { $0.title == "UA-101 Control" && $0.occlusionState.contains(.visible) }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    /// The app as the user opens it; false in the build's own check (--quit-after), which
    /// neither uses nor changes the remembered size.
    static let isUserRun = !CommandLine.arguments.contains("--quit-after")

    func applicationDidFinishLaunching(_ notification: Notification) {
        let args = CommandLine.arguments
        #if !UA101_USER
        // Developer build only: moving pretend levels from the start, to measure what the
        // drawing costs (the 1.1 look, 2026-11-27). The pretend unit; nothing reaches a real one.
        if args.contains("--pretend-levels") {
            AppCore.shared.ui.pretendLevels = true
            AppCore.shared.model.meters.setDemo(true)
        }
        #endif
        // For the build's own check: start, count the windows, leave. Stays in the background.
        if let i = args.firstIndex(of: "--quit-after"), i + 1 < args.count, let seconds = Double(args[i + 1]) {
            NSApp.setActivationPolicy(.accessory)
            // Part of the check: make the window larger by hand, then use the View menu's step.
            let ui = AppCore.shared.ui
            DispatchQueue.main.asyncAfter(deadline: .now() + seconds * 0.4) {
                if let w = NSApp.windows.first(where: { $0.isVisible }) {
                    w.setFrame(NSRect(x: w.frame.minX, y: w.frame.minY, width: w.frame.width * 0.8, height: w.frame.height * 0.8), display: true)
                }
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + seconds * 0.6) {
                print("after a resize to 80%: zoom \(ui.zoom)")
                WindowFit.setWindow(zoom: WindowFit.step(from: ui.zoom, up: false), natural: ui.naturalSize)
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + seconds * 0.8) {
                print("after Zoom Out: zoom \(ui.zoom)")
                // Leave the window as the user would want to find it.
                WindowFit.setWindow(zoom: 1, natural: ui.naturalSize)
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + seconds) {
                let model = AppCore.shared.model
                print("windows: \(NSApp.windows.filter { $0.isVisible }.count); unit: \(model.transport.displayName); state: \(model.snap.state); " +
                      "read-only: \(!model.snap.writesAllowed); sizes: \(NSApp.windows.filter { $0.isVisible }.map { "\(Int($0.frame.width))x\(Int($0.frame.height))" }); zoom: \(AppCore.shared.ui.zoom); natural: \(AppCore.shared.ui.naturalSize); menus: \(NSApp.mainMenu?.items.map { $0.title } ?? [])")
                // Which items the app menu and the View menu hold.
                func titles(_ m: NSMenu?) -> [String] { m?.items.filter { !$0.isSeparatorItem }.map { $0.title } ?? [] }
                print("app menu: \(titles(NSApp.mainMenu?.items.first?.submenu))")
                print("View menu: \(titles(NSApp.mainMenu?.items.first { $0.title == "View" }?.submenu))")
                print("Device menu: \(titles(NSApp.mainMenu?.items.first { $0.title == "Device" }?.submenu))")
                print("Mixer menu: \(titles(NSApp.mainMenu?.items.first { $0.title == "Mixer" }?.submenu))")
                print("Help menu: \(titles(NSApp.mainMenu?.items.first { $0.title == "Help" }?.submenu))")
                print("build: \(Build.name)")
                print("manual: PDF \(Manual.bundledPDF(in: .main) != nil ? "inside" : "missing"), help book \(Manual.hasHelpBook(in: .main) ? "inside" : "missing")")
                NSApp.terminate(nil)
            }
            // The build's check never goes near the real unit, whatever was chosen last time
            // (the end-user build's check leaves the UA-101 alone: nothing is attached).
            if !Build.isEndUser { AppCore.shared.model.connect() }
            // For measuring what moving meters cost: pretend levels on, and the processor time
            // this run used printed as it leaves.
            if !Build.isEndUser, ProcessInfo.processInfo.environment["UA101_PRETEND_LEVELS"] != nil {
                AppCore.shared.model.meters.setDemo(true)
                DispatchQueue.main.asyncAfter(deadline: .now() + seconds - 0.05) {
                    var u = rusage()
                    getrusage(RUSAGE_SELF, &u)
                    let used = Double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + Double(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6
                    print(String(format: "pretend levels for %.0f s: %.2f s of processor time", seconds, used))
                }
            }
        } else {
            NSApp.setActivationPolicy(.regular)
            NSApp.activate(ignoringOtherApps: true)
            if Build.isEndUser || UserDefaults.standard.string(forKey: AppCore.deviceKey) == AppCore.deviceReal {
                AppCore.shared.choose(real: true)
            } else {
                AppCore.shared.model.connect()
            }
        }
        NotificationCenter.default.addObserver(forName: NSWindow.didChangeOcclusionStateNotification, object: nil, queue: .main) { _ in
            AppCore.shared.windowVisibilityChanged()
        }
        // Closing the mixer closes the Patch Bay and Driver Settings windows too, so the app
        // quits as before.
        NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: nil, queue: .main) { note in
            guard (note.object as? NSWindow)?.title == "UA-101 Control" else { return }
            AppCore.shared.patchBayWindow.close()
            AppCore.shared.driverWindow.close()
        }
        // When the user lets go of the window's edge: the window takes the mixer's exact shape.
        NotificationCenter.default.addObserver(forName: NSWindow.didEndLiveResizeNotification, object: nil, queue: .main) { note in
            guard (note.object as? NSWindow)?.title == "UA-101 Control" else { return }
            let ui = AppCore.shared.ui
            WindowFit.setWindow(zoom: ui.zoom, natural: ui.naturalSize)
            SizeMemory.keep(ui)
        }
        // When the window has been moved: its place is kept (Remember Size).
        NotificationCenter.default.addObserver(forName: NSWindow.didMoveNotification, object: nil, queue: .main) { note in
            guard (note.object as? NSWindow)?.title == "UA-101 Control" else { return }
            SizeMemory.keep(AppCore.shared.ui)
        }
    }

    /// Quitting while a put back, a reset or a memory command is sending waits for its end (a
    /// few seconds; a backup put into a memory up to a quarter of a minute; never more than
    /// `quitWaitSeconds`), so the unit isn't left with a mix of settings.
    static let quitWaitSeconds = 25.0
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        let model = AppCore.shared.model
        guard model.jobSending else { return .terminateNow }
        var answered = false
        let answer = {
            if answered { return }
            answered = true
            NSApp.reply(toApplicationShouldTerminate: true)
        }
        model.onJobEnd = answer
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.quitWaitSeconds, execute: answer)
        return .terminateLater
    }

    func applicationWillTerminate(_ notification: Notification) {
        SizeMemory.keep(AppCore.shared.ui)
        AppCore.shared.notes.saveNow()
        AppCore.shared.model.finish()
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

/// How the mixer fits a window of any size: it is drawn at its own size and
/// scaled as one piece, so every part keeps its place and proportion.
enum WindowFit {
    static let smallest: CGFloat = 0.6, largest: CGFloat = 2.5

    /// The sizes the View menu steps through.
    static let steps: [CGFloat] = [0.6, 0.75, 0.8, 0.9, 1, 1.25, 1.5, 1.75, 2, 2.5]

    /// The three fixed sizes at the top of the View menu.
    static let fixedSizes: [(title: String, zoom: CGFloat)] = [("Actual (100%)", 1), ("Medium (80%)", 0.8), ("Small (60%)", 0.6)]

    /// Whether a View menu size is the one the mixer has now (its checkmark).
    static func isCurrent(_ size: CGFloat, zoom: CGFloat) -> Bool { abs(zoom - size) < 0.01 }

    /// The next step up or down from a zoom that may lie between steps.
    static func step(from zoom: CGFloat, up: Bool) -> CGFloat {
        up ? (steps.first { $0 > zoom + 0.01 } ?? largest) : (steps.last { $0 < zoom - 0.01 } ?? smallest)
    }

    /// Gives the mixer's window a zoom: its top left corner stays put, and it never
    /// grows past the screen it is on.
    static func setWindow(zoom: CGFloat, natural: CGSize) {
        guard natural.width > 0, natural.height > 0,
              let w = NSApp.windows.first(where: { $0.title == "UA-101 Control" }) else { return }
        var k = min(largest, max(smallest, zoom))
        // The title bar lies over the top of the window; the mixer gets what is below it.
        let bar = w.frame.height - w.contentLayoutRect.height
        if let room = w.screen?.visibleFrame.size {
            k = max(smallest, min(k, room.width / natural.width, (room.height - bar) / natural.height))
        }
        let size = NSSize(width: (natural.width * k).rounded(), height: (natural.height * k).rounded() + bar)
        // Dragging an edge keeps this shape, so the mixer always fills the window.
        w.aspectRatio = size
        w.setFrame(NSRect(x: w.frame.minX, y: w.frame.maxY - size.height, width: size.width, height: size.height), display: true)
    }

    /// What the mixer has to fill in its window.
    static func room(in w: NSWindow) -> CGSize { w.contentLayoutRect.size }

    static func scale(natural: CGSize, available: CGSize) -> CGFloat {
        guard natural.width > 0, natural.height > 0 else { return 1 }
        return min(largest, max(smallest, min(available.width / natural.width, available.height / natural.height)))
    }
}

/// Remember Size (app menu): the mixer's zoom and the window's top left corner, kept
/// for the next start. Off: the app opens at Actual (100%) in the default place.
enum SizeMemory {
    static let rememberKey = "rememberSize", zoomKey = "windowZoom", xKey = "windowLeft", yKey = "windowTop"

    /// On unless the user turned it off.
    static func isOn(_ d: UserDefaults) -> Bool { d.object(forKey: rememberKey) as? Bool ?? true }

    static func save(zoom: CGFloat, topLeft: CGPoint, to d: UserDefaults) {
        d.set(Double(zoom), forKey: zoomKey)
        d.set(Double(topLeft.x), forKey: xKey)
        d.set(Double(topLeft.y), forKey: yKey)
    }

    /// Where and how large the window opens. `screens` are the screens' visible frames, the
    /// main one first. A place no screen holds any more (a screen unplugged) moves onto the
    /// main screen; nil means the default place.
    static func launch(from d: UserDefaults, screens: [CGRect]) -> (zoom: CGFloat, topLeft: CGPoint?) {
        guard isOn(d), let z = d.object(forKey: zoomKey) as? Double, z.isFinite else { return (1, nil) }
        let zoom = min(WindowFit.largest, max(WindowFit.smallest, CGFloat(z)))
        guard let x = d.object(forKey: xKey) as? Double, let y = d.object(forKey: yKey) as? Double,
              x.isFinite, y.isFinite else { return (zoom, nil) }
        let p = CGPoint(x: x, y: y)
        // The title bar must be reachable: the corner lies inside a screen, with room to grab.
        if screens.contains(where: { $0.contains(CGPoint(x: p.x + 40, y: p.y - 10)) }) { return (zoom, p) }
        guard let main = screens.first else { return (zoom, nil) }
        return (zoom, CGPoint(x: min(max(p.x, main.minX), main.maxX - 200), y: min(max(p.y, main.minY + 200), main.maxY)))
    }

    /// The window as it is now, kept if Remember Size is on and the window has settled.
    static func keep(_ ui: PanelUI) {
        guard ui.rememberSize, ui.sizeSettled, AppDelegate.isUserRun,
              let w = NSApp.windows.first(where: { $0.title == "UA-101 Control" }), w.isVisible else { return }
        save(zoom: ui.zoom, topLeft: CGPoint(x: w.frame.minX, y: w.frame.maxY), to: .standard)
    }
}

private struct ZoomKey: PreferenceKey {
    // 0 means "nobody said": a view that says nothing must not wipe out one that did.
    static var defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) {
        let next = nextValue()
        if next > 0 { value = next }
    }
}

/// A dialog drawn at the mixer's zoom, so the two always match; smaller when that would put
/// its bottom, and its buttons, under the Dock (2026-11-20: a sheet hangs from the mixer's
/// title bar, and Driver Settings at 754 points reached the Dock on the user's MacBook).
struct ZoomedDialog<Content: View>: View {
    let zoom: CGFloat
    @Kept private var natural: CGSize = .zero
    let content: Content

    /// The zoom it is drawn at: the mixer's, or less so that all of it lies above the Dock.
    private var fitted: CGFloat {
        guard natural.width > 0,
              let w = NSApp?.windows.first(where: { $0.title == "UA-101 Control" }) else { return zoom }
        let top = w.frame.minY + w.contentLayoutRect.maxY
        return DockClearance.sheetZoom(natural: natural, wanted: zoom, top: top, room: DockClearance.room(on: w.screen))
    }

    var body: some View {
        let k = fitted
        content
            .fixedSize()
            .background(GeometryReader { g in Color.clear.preference(key: NaturalSizeKey.self, value: g.size) })
            .scaleEffect(k, anchor: .topLeading)
            .frame(width: natural.width > 0 ? (natural.width * k).rounded(.up) : nil,
                   height: natural.height > 0 ? (natural.height * k).rounded(.up) : nil, alignment: .topLeading)
            .onPreferenceChange(NaturalSizeKey.self) { if $0.width > 0 { natural = $0 } }
    }
}

private struct NaturalSizeKey: PreferenceKey {
    static var defaultValue: CGSize = .zero
    static func reduce(value: inout CGSize, nextValue: () -> CGSize) {
        let next = nextValue()
        if next.width > 0 { value = next }
    }
}

/// Scales its content to the window. No animation: the size follows the window's edge.
struct FitToWindow<Content: View>: View {
    @Environment(\.theme) private var theme
    @Kept private var natural: CGSize = .zero
    let ui: PanelUI
    let content: Content

    var body: some View {
        GeometryReader { geo in
            let k = WindowFit.scale(natural: natural, available: geo.size)
            content
                .background(GeometryReader { g in Color.clear.preference(key: NaturalSizeKey.self, value: g.size) })
                .scaleEffect(k)
                .frame(width: geo.size.width, height: geo.size.height)
                .preference(key: ZoomKey.self, value: k)
        }
        .onPreferenceChange(ZoomKey.self) { k in
            if k > 0, abs(ui.zoom - k) > 0.001 { ui.zoom = k }
        }
        .frame(minWidth: natural.width * WindowFit.smallest, idealWidth: natural.width,
               minHeight: natural.height * WindowFit.smallest, idealHeight: natural.height)
        .onPreferenceChange(NaturalSizeKey.self) { new in
            guard new.width > 0 else { return }
            let old = natural
            natural = new
            ui.naturalSize = new
            DispatchQueue.main.async { Self.fitWindow(old: old, new: new, ui: ui) }
        }
        .background(theme.window)
    }

    /// The mixer's own size changed (first drawn, or a banner came or went): the
    /// window keeps its zoom and takes the new shape, its top left corner staying put.
    private static func fitWindow(old: CGSize, new: CGSize, ui: PanelUI) {
        guard new.width > 0, new.height > 0,
              let w = NSApp.windows.first(where: { $0.title == "UA-101 Control" }) else { return }
        let now = WindowFit.room(in: w)
        let first = old.width <= 0
        // As it opens, for the user: the remembered zoom and place, or Actual (100%) in the
        // default place when Remember Size is off (or nothing is remembered yet).
        if first && AppDelegate.isUserRun {
            let start = SizeMemory.launch(from: .standard, screens: NSScreen.screens.map { $0.visibleFrame })
            if let p = start.topLeft { w.setFrameTopLeftPoint(p) }
            WindowFit.setWindow(zoom: start.zoom, natural: new)
            if start.topLeft == nil { w.center() }
            ui.sizeSettled = true
            return
        }
        // As it opens: its own size, unless macOS brought back a size it was chosen.
        let k = first ? (now.width > new.width * WindowFit.smallest + 2 ? WindowFit.scale(natural: new, available: now) : 1)
                      : WindowFit.scale(natural: old, available: now)
        WindowFit.setWindow(zoom: k, natural: new)
    }
}

struct RootView: View {
    @ObservedObject var model: PanelModel
    @ObservedObject var ui: PanelUI

    /// Reset Mixer's confirmation: what is overwritten, that it can't be undone, what is saved first.
    static let resetMixerWarning = "Every mute, pan, volume, link and solo in use now is overwritten with the way the unit was delivered: no channel muted, every one at 0 dB. "
        + MemoryCommand.cantBeUndone
        + " A backup of the settings in use now is saved first, by itself (Documents, UA-101 Backups); to go back: Device menu, Open Backup. Turn the monitors down and take headphones off first: what you hear can jump. The patch bay and the unit's memories don't change."

    var body: some View {
        FitToWindow(ui: ui, content: MixerWindowView(model: model, ui: ui, messages: AppCore.shared.messages))
            .sheet(item: $ui.sheet, onDismiss: {
                model.cancelMemory()
                model.cancelPutBack()
                model.cancelIntoMemory()
                // As the Patch Bay window does: an unanswered "Send Hard Thru?" goes; nothing is sent.
                if model.hardThruAsked { model.cancelHardThru() }
            }) { sheet in
                ZoomedDialog(zoom: ui.zoom, content: Group {
                    switch sheet {
                    case .patchBay: PatchBayView(model: model, close: { ui.sheet = nil }, messages: AppCore.shared.messages, openMessages: ui.showMessages)
                    case .driverSettings: DriverSettingsView(model: AppCore.shared.driver, close: { ui.sheet = nil })
                    case .load: MemoryDialogView(model: model, purpose: .load, close: { ui.sheet = nil }, messages: AppCore.shared.messages, openMessages: ui.showMessages)
                    case .store: MemoryDialogView(model: model, purpose: .store, close: { ui.sheet = nil }, messages: AppCore.shared.messages, openMessages: ui.showMessages)
                    case .reset: MemoryDialogView(model: model, purpose: .reset, close: { ui.sheet = nil }, messages: AppCore.shared.messages, openMessages: ui.showMessages)
                    case .putBack: PutBackView(model: model, close: { ui.sheet = nil }, messages: AppCore.shared.messages, openMessages: ui.showMessages)
                    case .intoMemory: IntoMemoryView(model: model, close: { ui.sheet = nil }, messages: AppCore.shared.messages, openMessages: ui.showMessages)
                    }
                })
                .followsAppearance()
            }
            // The warning before changing covers Reset Mixer too (a design choice, 2026-11-22).
            .alert("Warning: Reset the mixer?", isPresented: $ui.askResetMixer) {
                Button("Reset the mixer", role: .destructive) { model.resetMixer() }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text(Self.resetMixerWarning)
            }
            .followsAppearance()
            // The app's appearance: macOS then draws every window, menu and alert to match.
            .onReceive(ui.$look) { look in
                NSApp.appearance = look.appearanceName.flatMap { NSAppearance(named: $0) }
                UserDefaults.standard.set(look.rawValue, forKey: Look.defaultsKey)
            }
            .onReceive(ui.$rememberSize) { on in
                UserDefaults.standard.set(on, forKey: SizeMemory.rememberKey)
                SizeMemory.keep(ui)
            }
            // A new zoom is kept once the window has taken it (Remember Size).
            .onReceive(ui.$zoom.debounce(for: .milliseconds(300), scheduler: RunLoop.main)) { _ in SizeMemory.keep(ui) }
            // Always on Top: the mixer and the others, but never Patch Bay or Driver Settings
            // (2026-11-20, a design choice).
            .onReceive(ui.$alwaysOnTop) { top in
                for window in NSApp.windows { window.level = DialogWindow.level(title: window.title, alwaysOnTop: top) }
            }
    }
}

struct PanelApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @ObservedObject private var ui = AppCore.shared.ui
    @ObservedObject private var model = AppCore.shared.model

    var body: some Scene {
        Window("UA-101 Control", id: "main") {
            RootView(model: model, ui: ui)
        }
        .windowResizability(.contentMinSize)
        .commands { PanelCommands(model: model, ui: ui) }

        #if !UA101_USER
        Window("Pretend unit", id: "pretend") {
            PretendFrontPanelView(model: model, unit: AppCore.shared.unit)
                .followsAppearance()
        }
        .windowResizability(.contentSize)
        #endif
    }
}

#if !UA101_USER
private struct OpenPretendButton: View {
    @Environment(\.openWindow) private var openWindow
    var body: some View {
        Button("Pretend Unit's Front Panel") { openWindow(id: "pretend") }
            .keyboardShortcut("p", modifiers: [.command, .shift])
    }
}
#endif

struct PanelCommands: Commands {
    @ObservedObject var model: PanelModel
    @ObservedObject var ui: PanelUI

    /// The Device menu's two grey headings.
    static let memoriesHeading = "Memories in the UA-101"
    static let backupsHeading = "Backup files on this Mac"

    /// A Mixer menu shortcut with no ⌘: off while the notes pad has the keyboard.
    private func oneKey(_ key: KeyEquivalent, _ modifiers: EventModifiers) -> KeyboardShortcut? {
        ui.typingNotes ? nil : KeyboardShortcut(key, modifiers: modifiers)
    }

    var body: some Commands {
        CommandGroup(replacing: .appInfo) {
            Button("About UA-101 Control") {
                // The version comes from Info.plist (it said "0.5" from here until 1.0, 2026-11-24).
                let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
                NSApp.orderFrontStandardAboutPanel(options: [
                    .applicationName: "UA-101 Control",
                    .applicationVersion: Build.isEndUser ? version : version + " (" + Build.name + " build)",
                    .credits: NSAttributedString(string: "A driver and control panel for the Edirol UA-101 on Macs with Apple silicon.\nFree to use, with no warranty. Not made, supported or endorsed by Roland."),
                ])
            }
        }
        // Nothing here: "Always Indicate Signals" (Roland's) did nothing in this app, the meters
        // show whenever the mixer can be seen; removed by a design choice (2026-11-20).
        // Driver Settings… is in the Device menu.
        CommandGroup(replacing: .appSettings) {}
        // The two ways to remove the driver, from the app (app/Uninstall.swift).
        CommandGroup(after: .appSettings) {
            Divider()
            Button("Uninstall UA-101…") { AppCore.shared.uninstall() }
        }
        CommandGroup(replacing: .newItem) {}
        CommandGroup(before: .toolbar) {
            // Three fixed sizes; the checkmark is on the one the mixer has now.
            ForEach(WindowFit.fixedSizes, id: \.zoom) { size in
                Toggle(size.title, isOn: Binding(get: { WindowFit.isCurrent(size.zoom, zoom: ui.zoom) },
                                                 set: { _ in WindowFit.setWindow(zoom: size.zoom, natural: ui.naturalSize) }))
                    .keyboardShortcut(size.zoom == 1 ? KeyboardShortcut("0", modifiers: .command) : nil)
            }
            Divider()
            Button("Zoom In") { WindowFit.setWindow(zoom: WindowFit.step(from: ui.zoom, up: true), natural: ui.naturalSize) }
                .keyboardShortcut("=", modifiers: .command)
            Button("Zoom Out") { WindowFit.setWindow(zoom: WindowFit.step(from: ui.zoom, up: false), natural: ui.naturalSize) }
                .keyboardShortcut("-", modifiers: .command)
            Divider()
            // The window's own ways (moved here from the app menu, 2026-11-14, a design choice).
            // The Messages window (2026-11-16): every note and warning, beside the mixer.
            Button("Show Messages") { ui.showMessages() }.keyboardShortcut("m", modifiers: [.command, .option])
            Button("Larger Message Text") { ui.stepMessagesScale(up: true) }.keyboardShortcut("=", modifiers: [.command, .option])
            Button("Smaller Message Text") { ui.stepMessagesScale(up: false) }.keyboardShortcut("-", modifiers: [.command, .option])
            // The notes pad (1.1): what is wired where, kept on this Mac (app/Notes.swift).
            Button("Show Notes") { ui.showNotes() }.keyboardShortcut("n", modifiers: [.command, .option])
            Divider()
            Toggle("Always on Top", isOn: $ui.alwaysOnTop)
            Toggle("Remember Window Size", isOn: $ui.rememberSize)
            Divider()
            // Checked: its own window. Unchecked (as delivered): a sheet on the mixer, as before.
            Toggle("Patch Bay in Its Own Window", isOn: Binding(get: { ui.patchBayOwnWindow },
                                                                 set: { AppCore.shared.setOwnWindow(patchBay: $0) }))
            Toggle("Driver Settings in Its Own Window", isOn: Binding(get: { ui.driverSettingsOwnWindow },
                                                                       set: { AppCore.shared.setOwnWindow(driverSettings: $0) }))
            Picker("Appearance", selection: $ui.look) {
                ForEach(Look.allCases) { Text($0.title).tag($0) }
            }
            Divider()
        }

        CommandMenu("Device") {
            Button("Driver Settings…") { AppCore.shared.openDriverSettings() }.keyboardShortcut(",", modifiers: .command)
            Button("Patch Bay…") { ui.showPatchBay() }.keyboardShortcut("b", modifiers: .command)
            Divider()
            // Two groups under grey headings that can't be clicked (the user's names of
            // 2026-11-22): which place each command touches.
            Section(Self.memoriesHeading) {
                Button("Recall Memory…") { ui.sheet = .load }.keyboardShortcut("l", modifiers: .command)
                Button("Save Memory…") { ui.sheet = .store }.keyboardShortcut("s", modifiers: .command)
                Button("Reset to Factory…") { ui.sheet = .reset }
            }
            Divider()
            Section(Self.backupsHeading) {
                // A backup of the unit's settings as a file, and the way back. Saving
                // one only reads the unit; opening one asks before anything is sent.
                Button("Save Backup…") { model.saveBackup() }
                Button("Open Backup…") { AppCore.shared.choosePutBack() }
                // A backup file into one of the unit's five memories: it
                // asks which memory and shows what happens before anything is sent.
                Button("Copy Backup to Memory…") { AppCore.shared.chooseIntoMemory() }
            }
            Divider()
            Button("Refresh Settings") { model.refresh() }.keyboardShortcut("r", modifiers: .command)
            #if !UA101_USER
            Divider()
            Picker("Unit", selection: Binding(get: { !model.transport.isPretend }, set: { model.onChooseDevice?($0) })) {
                Text("Pretend Unit").tag(false)
                Text("UA-101").tag(true)
            }
            #endif
            Divider()
            if model.changesByThemselves && !model.transport.isPretend {
                // The end-user app: changes are on by themselves; this keeps them off.
                Toggle("Lock Changes", isOn: Binding(get: { model.changesLocked }, set: { model.lockChanges($0) }))
            } else {
                Toggle("Allow Changes", isOn: Binding(get: { model.snap.writesAllowed }, set: { model.allowChanges($0) }))
            }
            Button("Pause Auto-Save") { model.pauseSaving() }
                .disabled(!model.snap.writesAllowed || model.snap.savingHeld)
            Button("Resume Auto-Save") { model.resumeSaving() }
                .disabled(!model.snap.savingHeld)
            #if !UA101_USER
            // One exact value, once, for the selected channel (the one with the keyboard's
            // frame; click a strip or move with the arrow keys): one message (two in a linked
            // pair), no drag and no pause of the saving around it, and the read-back in the
            // record. The way to try a channel's control on the unit for the first time. The
            // first two values were the first volumes ever sent to the UA-101 (INPUT 1,
            // 2026-11-12); the third has a digit above 1 in every place but one.
            let selected = model.cursor
            let name = ChannelNames.full(selected)
            Menu("\(name) Volume, One Exact Value") {
                Button("0x100001  (about −6 dB)") { model.setVolumeExact(selected, 0x100001) }
                Button("0x16A09E  (about −3 dB)") { model.setVolumeExact(selected, 0x16A09E) }
                Button("0x200000  (0 dB)") { model.setVolumeExact(selected, 0x200000) }
            }
            .disabled(!model.snap.writesAllowed)
            // The same for the pan: one message each. 0x4000 is the middle; 0x1234 has a
            // different digit in every place; 0x0000 and 0x8000 are the ends, where the unit
            // holds the odd and the even channels as delivered. 0x4000 was the first pan ever
            // sent to the UA-101 (INPUT 1, 2026-11-12): its read-back proved the order of a
            // pan's four digits. MONITOR OUT has no pan.
            Menu("\(name) Pan, One Exact Value") {
                Button("0x4000  (the middle)") { model.setPanExact(selected, 0x4000) }
                Button("0x1234  (between the left end and the middle)") { model.setPanExact(selected, 0x1234) }
                Button("0x0000  (the left end)") { model.setPanExact(selected, 0x0000) }
                Button("0x8000  (the right end)") { model.setPanExact(selected, 0x8000) }
            }
            .disabled(!model.snap.writesAllowed || selected.group == ChannelNames.groupMonitor)
            // The on/off bit itself, one message each. Bit 1 means "off" (heard on INPUT 1,
            // 2026-11-14). The unit holds 0 on every channel as delivered, so "bit 1" mutes
            // the channel and "bit 0" goes back.
            Menu("\(name) Mute, One Exact Bit") {
                Button("Bit 1  (mute on)") { model.setOnBitExact(selected, 1) }
                Button("Bit 0  (mute off, as delivered)") { model.setOnBitExact(selected, 0) }
            }
            .disabled(!model.snap.writesAllowed)
            #endif
            #if !UA101_USER
            Divider()
            OpenPretendButton()
            Toggle("Pretend Levels on the Meters (for the look)", isOn: Binding(
                get: { ui.pretendLevels },
                set: {
                    ui.pretendLevels = $0
                    model.meters.setDemo($0)
                }))
            #endif
        }

        // Roland called it "Panel"; "Mixer" by a design choice (2026-11-20).
        CommandMenu("Mixer") {
            Menu("Select Channel") {
                Button("Previous Channel") { model.handle(.cursorLeft) }.keyboardShortcut(oneKey(.leftArrow, []))
                Button("Next Channel") { model.handle(.cursorRight) }.keyboardShortcut(oneKey(.rightArrow, []))
                Divider()
                ForEach(ChannelNames.screenOrder, id: \.self) { id in
                    Button(ChannelNames.full(id)) { model.cursor = id }
                }
            }
            Menu("Mute") {
                Button("Selected Channel") { model.handle(.toggleMonitor) }.keyboardShortcut(oneKey("m", []))
                Divider()
                ForEach(ChannelNames.screenOrder.filter { $0.group != ChannelNames.groupMonitor }, id: \.self) { id in
                    Button(ChannelNames.full(id)) { model.toggleMonitor(id) }
                }
            }
            Menu("Solo") {
                Button("Selected Channel") { model.handle(.toggleSolo) }.keyboardShortcut(oneKey("s", []))
                Divider()
                ForEach(ChannelNames.screenOrder.filter { $0.group != ChannelNames.groupMonitor }, id: \.self) { id in
                    Button(ChannelNames.full(id)) { model.toggleSolo(id) }
                }
            }
            Menu("Pan") {
                Button("Pan Left") { model.handle(.panLeft) }.keyboardShortcut(oneKey(",", []))
                Button("Pan Right") { model.handle(.panRight) }.keyboardShortcut(oneKey(".", []))
                Button("Centre") { model.handle(.panCenter) }.keyboardShortcut(oneKey("c", []))
            }
            Menu("Volume") {
                Button("Up 0.5 dB") { model.handle(.volumeUp) }.keyboardShortcut(oneKey(.upArrow, []))
                Button("Down 0.5 dB") { model.handle(.volumeDown) }.keyboardShortcut(oneKey(.downArrow, []))
                Button("Up 3 dB") { model.handle(.volumeUpBig) }.keyboardShortcut(oneKey(.upArrow, .shift))
                Button("Down 3 dB") { model.handle(.volumeDownBig) }.keyboardShortcut(oneKey(.downArrow, .shift))
            }
            Menu("Link") {
                Button("Selected Channel") { model.handle(.toggleLink) }.keyboardShortcut(oneKey("l", []))
                Divider()
                ForEach([ChannelNames.groupInput, ChannelNames.groupOutput], id: \.self) { g in
                    ForEach(0..<5, id: \.self) { p in
                        Button(ChannelNames.pair(group: g, pair: p)) { model.toggleLink(group: g, pair: p) }
                    }
                }
                Button(ChannelNames.pair(group: ChannelNames.groupMonitor, pair: 0)) {
                    model.toggleLink(group: ChannelNames.groupMonitor, pair: 0)
                }
            }
            Divider()
            Button("Reset Mixer…") { ui.askResetMixer = true }
        }

        // Help : the help book first, then "Save Manual as PDF…", then the report. Their order is Manual.helpItems (the self-test checks it).
        // macOS keeps its search field at the top; with the help book it searches the manual too.
        CommandGroup(replacing: .help) {
            Button("UA-101 Control Help") { AppCore.shared.openHelp() }.keyboardShortcut("?", modifiers: .command)
            Button("Save Manual as PDF…") { AppCore.shared.saveManualPDF() }
            Divider()
            Button("Save Report for Developer…") { AppCore.shared.saveReport() }
        }
    }
}
