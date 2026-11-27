// Patch Bay and Driver Settings as windows of their own (2026-11-20: neither blocks
// the mixer). Until then both were sheets on the mixer: it couldn't be used while
// one was open, and they moved with it. Recall, Save and Reset stay sheets.
import AppKit
import SwiftUI

/// The part of a screen a dialog must stay inside so that nothing of it, its buttons least of
/// all, is ever under the Dock. macOS's visible frame leaves out only the resting Dock; a
/// magnified Dock grows (icons 38 → 128 points), and while the pointer is on it the grown icons
/// cover about 90 points more. A hiding Dock is left out of the visible frame altogether.
enum DockClearance {
    struct Dock: Equatable {
        enum Side: String { case bottom, left, right }
        var side: Side = .bottom
        var hides = false
        var magnifies = false
        var tile: CGFloat = 48
        var large: CGFloat = 64
        /// What the Dock reaches into the screen at most, beyond its icons: its own margins.
        static let margin: CGFloat = 20
    }

    /// Kept free between a dialog and the Dock at its largest.
    static let gap: CGFloat = 8

    /// The Dock as its settings are now (com.apple.dock; read only, never changed).
    static func current() -> Dock {
        guard let d = UserDefaults(suiteName: "com.apple.dock") else { return Dock() }
        var k = Dock()
        k.side = (d.string(forKey: "orientation")).flatMap(Dock.Side.init(rawValue:)) ?? .bottom
        k.hides = d.bool(forKey: "autohide")
        k.magnifies = d.bool(forKey: "magnification")
        if let t = d.object(forKey: "tilesize") as? Double, t >= 16, t <= 128 { k.tile = CGFloat(t) }
        if let l = d.object(forKey: "largesize") as? Double, l >= 16, l <= 128 { k.large = CGFloat(l) }
        return k
    }

    /// The room on a screen (`frame`, `visible` without menu bar and resting Dock) that the
    /// Dock never reaches, grown icons included, less a small gap. A screen the resting Dock is
    /// not on (the iPad beside the Mac, say) keeps all of its visible frame, unless the Dock
    /// hides: then it may slide in on any screen.
    static func room(frame: NSRect, visible: NSRect, dock: Dock) -> NSRect {
        var r = visible
        let icons = dock.magnifies ? max(dock.tile, dock.large) : dock.tile
        switch dock.side {
        case .bottom:
            let resting = visible.minY - frame.minY
            guard resting > 0.5 || dock.hides else { return r }
            // The resting Dock's own margins, the icons at their largest, the gap.
            let margins = resting > 0.5 ? max(0, resting - dock.tile) : Dock.margin
            let top = frame.minY + margins + icons + gap
            if top > r.minY { r.size.height -= top - r.minY; r.origin.y = top }
        case .left:
            let resting = visible.minX - frame.minX
            guard resting > 0.5 || dock.hides else { return r }
            let margins = resting > 0.5 ? max(0, resting - dock.tile) : Dock.margin
            let edge = frame.minX + margins + icons + gap
            if edge > r.minX { r.size.width -= edge - r.minX; r.origin.x = edge }
        case .right:
            let resting = frame.maxX - visible.maxX
            guard resting > 0.5 || dock.hides else { return r }
            let margins = resting > 0.5 ? max(0, resting - dock.tile) : Dock.margin
            let edge = frame.maxX - margins - icons - gap
            if edge < r.maxX { r.size.width -= r.maxX - edge }
        }
        // Never less than half the screen, whatever the Dock's settings say.
        if r.height < visible.height / 2 { r.origin.y = visible.minY; r.size.height = visible.height / 2 }
        if r.width < visible.width / 2 { r.size.width = visible.width / 2 }
        return r
    }

    /// The same for a real screen, with the Dock's settings now.
    static func room(on screen: NSScreen?) -> NSRect {
        guard let s = screen ?? NSScreen.main else { return NSRect(x: 0, y: 0, width: 1440, height: 800) }
        return room(frame: s.frame, visible: s.visibleFrame, dock: current())
    }

    /// The zoom a sheet hanging from a window whose content starts at `top` (screen points)
    /// takes, so that all of it, down to its buttons, is in `room`.
    static func sheetZoom(natural: CGSize, wanted: CGFloat, top: CGFloat, room: NSRect) -> CGFloat {
        DialogWindow.zoom(natural: natural, wanted: wanted,
                          room: CGSize(width: room.width, height: min(top, room.maxY) - room.minY))
    }
}

/// One such window, kept by the app: opening it again brings the same window forward. It is an
/// ordinary window, never above other apps' windows, whatever Always on Top says (that is the
/// mixer's and the Messages window's). Its content is made each time it opens and dropped when
/// it closes, so nothing in it runs while it is closed (Driver Settings reads the driver once a
/// second while open).
final class DialogWindow: NSObject, NSWindowDelegate {
    enum Kind: CaseIterable {
        case patchBay, driverSettings
        var title: String {
            switch self {
            case .patchBay: return "UA-101 Patch Bay"
            case .driverSettings: return "UA-101 Driver Settings"
            }
        }
    }

    /// The titles of the windows Always on Top leaves alone.
    static let titles: Set<String> = Set(Kind.allCases.map { $0.title })

    /// The level a window of this app gets: these two always the ordinary one.
    static func level(title: String, alwaysOnTop: Bool) -> NSWindow.Level {
        titles.contains(title) ? .normal : (alwaysOnTop ? .floating : .normal)
    }

    /// The zoom the content is drawn at: the mixer's, made smaller when that would not fit
    /// on the screen above the Dock (2026-11-20: Driver Settings' buttons were hidden
    /// behind the Dock, and both windows crashed the app on opening).
    final class Fit: ObservableObject {
        @Published var zoom: CGFloat = 1
    }

    /// The zoom that fits: the one wanted, or less, so that the whole content fits the room.
    static func zoom(natural: CGSize, wanted: CGFloat, room: CGSize) -> CGFloat {
        guard natural.width > 1, natural.height > 1, room.width > 1, room.height > 1 else { return wanted }
        return max(0.3, min(wanted, room.width / natural.width, room.height / natural.height))
    }

    /// Where a window of this frame goes so that all of it is in `visible` (a screen without
    /// its menu bar and Dock), moved as little as possible.
    static func keep(_ frame: NSRect, in visible: NSRect) -> NSRect {
        var f = frame
        if f.maxY > visible.maxY { f.origin.y = visible.maxY - f.height }
        if f.minY < visible.minY { f.origin.y = visible.minY }
        if f.maxX > visible.maxX { f.origin.x = visible.maxX - f.width }
        if f.minX < visible.minX { f.origin.x = visible.minX }
        return f
    }

    let kind: Kind
    let fit = Fit()
    /// For the self-test: the room on a pretend screen, in place of the real screen's.
    var testRoom: NSRect?
    private var offScreen = false
    /// The content's own size, at zoom 1, as it last told it.
    private(set) var natural: CGSize = .zero
    private var wanted: CGFloat = 1
    private let content: (DialogWindow) -> AnyView
    /// What closing it does besides closing it (the window's close button, Done or Close, Esc).
    var onClose: () -> Void = {}
    private(set) var window: NSWindow?

    init(kind: Kind, content: @escaping (DialogWindow) -> AnyView) {
        self.kind = kind
        self.content = content
    }

    var isShown: Bool { window?.isVisible == true }

    /// The window, made the first time and kept after. Not shown by this.
    @discardableResult
    func prepare() -> NSWindow {
        if let w = window { return w }
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 500, height: 400),
                         styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: true)
        w.title = kind.title
        w.isReleasedWhenClosed = false
        w.hidesOnDeactivate = false
        w.level = .normal
        w.delegate = self
        window = w
        return w
    }

    /// Fills the window with its content, if it has none (it is dropped on closing).
    private func fill(_ w: NSWindow) {
        guard w.contentViewController == nil else { return }
        let host = NSHostingController(rootView: content(self))
        // The window's size is set here (resize), never by SwiftUI asking for it: with
        // .preferredContentSize a window taller than the screen made AppKit and SwiftUI undo
        // each other's size until AppKit gave up and the app crashed (2026-11-20).
        host.sizingOptions = []
        // Nor does the content hold the window to a least size: a shown window was kept at the
        // content's size at zoom 1 while the content was drawn smaller to fit above the Dock,
        // leaving an empty band right and below (2026-11-20, the user's screenshot).
        (host.view as? NSHostingView<AnyView>)?.sizingOptions = []
        w.contentViewController = host
    }

    /// The content tells its own size (at zoom 1) and the mixer's zoom. Taken up after the
    /// layout pass it came from, never inside it.
    func report(natural n: CGSize?, wanted z: CGFloat) {
        if let n, n.width > 1, n.height > 1 { natural = n }
        wanted = z
        DispatchQueue.main.async { [weak self] in self?.resize() }
    }

    /// The window's size: the content's at the zoom that fits, top left corner kept, all of
    /// it on the screen above the Dock.
    private func resize() {
        guard let w = window, w.contentViewController != nil, natural.width > 1 else { return }
        // Above the Dock at its largest (DockClearance), not only above the resting one.
        let visible = testRoom ?? DockClearance.room(on: w.screen)
        let bar = w.frame.height - w.contentLayoutRect.height
        let z = DialogWindow.zoom(natural: natural, wanted: wanted,
                                  room: CGSize(width: visible.width, height: visible.height - max(0, bar)))
        if abs(fit.zoom - z) > 0.001 { fit.zoom = z }
        let size = CGSize(width: (natural.width * z).rounded(.up), height: (natural.height * z).rounded(.up))
        var frame = w.frameRect(forContentRect: NSRect(origin: .zero, size: size))
        frame.origin = NSPoint(x: w.frame.minX, y: w.frame.maxY - frame.height)
        if !offScreen { frame = DialogWindow.keep(frame, in: visible) }
        if frame != w.frame { w.setFrame(frame, display: true) }
        if !offScreen && !w.ignoresMouseEvents && w.alphaValue < 1 { w.alphaValue = 1 }
    }

    func windowDidChangeScreen(_ notification: Notification) {
        DispatchQueue.main.async { [weak self] in self?.resize() }
    }

    /// Placed once; after that it stays where the user leaves it.
    private var placed = false
    private var placeKey: String { kind.title + " top left" }

    /// Opens it, or brings it forward. The first time: where the user left it last time if
    /// that is still on a screen, else near the mixer's top left corner. Only its place is
    /// kept: its size is always its content's.
    func show() {
        let w = prepare()
        offScreen = false
        if w.contentViewController == nil {
            // Shown once it has its size (resize), so it doesn't jump; at most 0.5 s later.
            w.alphaValue = 0
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak w] in w?.alphaValue = 1 }
        }
        fill(w)
        w.level = .normal
        if !placed {
            placed = true
            let d = UserDefaults.standard
            if let x = d.object(forKey: placeKey + " x") as? Double, let y = d.object(forKey: placeKey + " y") as? Double,
               x.isFinite, y.isFinite,
               NSScreen.screens.contains(where: { $0.visibleFrame.contains(CGPoint(x: x + 40, y: y - 10)) }) {
                w.setFrameTopLeftPoint(NSPoint(x: x, y: y))
            } else if let main = NSApp.windows.first(where: { $0.title == "UA-101 Control" }), main.isVisible {
                w.setFrameTopLeftPoint(NSPoint(x: main.frame.minX + 60, y: main.frame.maxY - 60))
            } else {
                w.center()
            }
        }
        w.makeKeyAndOrderFront(nil)
    }

    /// For the self-test: opens it as show() does, but transparent, far off every screen, never
    /// key and taking no mouse, so nothing appears on the Mac's screen.
    /// `onScreen`: still transparent and taking no mouse, but on the main screen, where macOS
    /// keeps a window from growing taller than the screen (the case that crashed).
    func openOffScreen(onScreen: Bool = false) {
        let w = prepare()
        offScreen = !onScreen
        fill(w)
        w.alphaValue = 0
        w.ignoresMouseEvents = true
        if onScreen, NSScreen.main != nil {
            let v = DockClearance.room(on: NSScreen.main)
            testRoom = v
            w.setFrameTopLeftPoint(NSPoint(x: v.minX + 40, y: v.maxY - 40))
        } else {
            w.setFrameTopLeftPoint(NSPoint(x: -30000, y: -30000))
        }
        placed = true
        w.orderFrontRegardless()
    }

    func windowDidMove(_ notification: Notification) {
        guard let w = window, AppDelegate.isUserRun else { return }
        UserDefaults.standard.set(Double(w.frame.minX), forKey: placeKey + " x")
        UserDefaults.standard.set(Double(w.frame.maxY), forKey: placeKey + " y")
    }

    func close() { window?.close() }

    func windowWillClose(_ notification: Notification) {
        onClose()
        // Drop the content: its once-a-second reading (Driver Settings) stops with it.
        window?.contentViewController = nil
    }
}

/// A dialog in its own window, drawn at the mixer's zoom (smaller if that wouldn't fit on the
/// screen), so the two still match. Its size at zoom 1 doesn't depend on the window, so the
/// window's size can't feed back into it.
struct DialogWindowContent<Content: View>: View {
    @ObservedObject var ui: PanelUI
    @ObservedObject var fit: DialogWindow.Fit
    let window: DialogWindow
    let content: Content
    /// The content's own size at zoom 1, as last measured.
    @Kept private var natural: CGSize = .zero

    init(ui: PanelUI, window: DialogWindow, content: Content) {
        self.ui = ui
        self.fit = window.fit
        self.window = window
        self.content = content
    }

    var body: some View {
        content
            .fixedSize()
            .background(GeometryReader { g in Color.clear.preference(key: DialogNaturalSizeKey.self, value: g.size) })
            .scaleEffect(fit.zoom, anchor: .topLeading)
            // Its size is what is drawn, not its size at zoom 1 (scaleEffect alone keeps that).
            .frame(width: natural.width > 0 ? (natural.width * fit.zoom).rounded(.up) : nil,
                   height: natural.height > 0 ? (natural.height * fit.zoom).rounded(.up) : nil, alignment: .topLeading)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .onPreferenceChange(DialogNaturalSizeKey.self) {
                if $0.width > 1 { natural = $0 }
                window.report(natural: $0, wanted: ui.zoom)
            }
            .onChange(of: ui.zoom) { _, z in window.report(natural: nil, wanted: z) }
            .followsAppearance()
    }
}

private struct DialogNaturalSizeKey: PreferenceKey {
    static var defaultValue: CGSize = .zero
    static func reduce(value: inout CGSize, nextValue: () -> CGSize) {
        let next = nextValue()
        if next.width > 0 { value = next }
    }
}

/// macOS's alert windows, made to fit too (2026-11-27: the uninstaller's last window, with a long
/// account, ran below the screen and its button couldn't be reached, even with the Dock hidden).
/// Words too tall for the room above the Dock go into a box that scrolls, and the alert is kept
/// inside the room: from below the menu bar to above the Dock at its largest.
extension NSAlert {
    /// What the alert needs besides its words: title, icon, margins and buttons, and the room
    /// left free above and below. Measured on macOS 26 with a generous margin.
    static let fitAllowance: CGFloat = 240
    /// The width of the scrolling box.
    static let fitWidth: CGFloat = 440

    /// Gets the alert ready to fit in `room` (screen points). Call before runModal.
    func prepareToFit(room: NSRect) {
        let text = informativeText
        let budget = max(120, room.height - Self.fitAllowance - (accessoryView?.frame.height ?? 0))
        let font = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
        let measured = (text as NSString).boundingRect(with: NSSize(width: 300, height: CGFloat.greatestFiniteMagnitude),
                                                       options: [.usesLineFragmentOrigin, .usesFontLeading],
                                                       attributes: [.font: font]).height
        if measured > budget {
            let scroll = NSScrollView(frame: NSRect(x: 0, y: 0, width: Self.fitWidth, height: 10))
            scroll.hasVerticalScroller = true
            scroll.borderType = .bezelBorder
            let view = NSTextView(frame: NSRect(x: 0, y: 0, width: Self.fitWidth - 20, height: 10))
            view.isEditable = false
            view.isSelectable = true
            view.font = font
            view.string = text
            view.textContainerInset = NSSize(width: 4, height: 4)
            view.isVerticallyResizable = true
            view.autoresizingMask = [.width]
            view.textContainer?.widthTracksTextView = true
            view.layoutManager?.ensureLayout(for: view.textContainer!)
            let used = view.layoutManager?.usedRect(for: view.textContainer!).height ?? budget
            scroll.documentView = view
            let height = min(budget, used + 12)
            scroll.setFrameSize(NSSize(width: Self.fitWidth, height: height))
            if let old = accessoryView {
                let stack = NSStackView(views: [scroll, old])
                stack.orientation = .vertical
                stack.alignment = .leading
                stack.spacing = 8
                stack.setFrameSize(NSSize(width: max(Self.fitWidth, old.frame.width), height: height + 8 + old.frame.height))
                accessoryView = stack
            } else {
                accessoryView = scroll
            }
            informativeText = ""
        }
        layout()
    }

    /// runModal, fitted to the room on the main screen.
    @discardableResult
    func runModalFitted() -> NSApplication.ModalResponse {
        let room = DockClearance.room(on: NSScreen.main)
        prepareToFit(room: room)
        // Once it shows (the modal loop runs the main queue): kept inside the room.
        DispatchQueue.main.async { [weak self] in
            guard let w = self?.window else { return }
            w.setFrame(DialogWindow.keep(w.frame, in: room), display: true)
        }
        return runModal()
    }
}

/// No button is chosen for the keyboard as a window opens (the owner's wish, 2026-11-27).
/// With Keyboard navigation on in System Settings, macOS gives the keyboard to a window's
/// first button as it opens and draws a blue ring around it (Driver Settings… on the mixer's
/// top line). The mixer, its sheets, Patch Bay and Driver Settings now open with none chosen;
/// Tab still chooses the first button, and Tab and Space work as before after that. Return and
/// Escape on a sheet's buttons don't depend on it. Not the Notes window (it is for typing) and
/// not panels (alerts, Open and Save, Messages).
enum NoFirstFocus {
    static let titles: Set<String> = DialogWindow.titles.union(["UA-101 Control"])
    /// The windows already opened without a chosen button: once each, until it closes, so a
    /// button chosen with Tab stays chosen when the app comes back to the front.
    private static let done = NSHashTable<NSWindow>.weakObjects()
    private static var started = false

    static func applies(to w: NSWindow) -> Bool {
        if w is NSPanel { return false }
        if let parent = w.sheetParent { return applies(to: parent) }
        return titles.contains(w.title)
    }

    static func start() {
        guard !started else { return }
        started = true
        let centre = NotificationCenter.default
        centre.addObserver(forName: NSWindow.didBecomeKeyNotification, object: nil, queue: .main) { n in
            guard let w = n.object as? NSWindow else { return }
            opened(w)
        }
        centre.addObserver(forName: NSWindow.willCloseNotification, object: nil, queue: .main) { n in
            guard let w = n.object as? NSWindow else { return }
            done.remove(w)
        }
        // The mixer is already the key window when the app has finished starting.
        if let key = NSApp.keyWindow { opened(key) }
    }

    /// The window became key: the first time since it opened, nothing chosen. Once more a
    /// moment later, as SwiftUI may choose its first button after the window became key.
    static func opened(_ w: NSWindow) {
        guard applies(to: w), !done.contains(w) else { return }
        done.add(w)
        clear(w)
        DispatchQueue.main.async { clear(w) }
    }

    static func clear(_ w: NSWindow) {
        if w.firstResponder !== w { w.makeFirstResponder(nil) }
    }
}

