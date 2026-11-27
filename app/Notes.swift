// The notes pad (1.1, an idea from UA1000Sidecard, 2026-11-27): a plain text window for what is
// wired where ("Input 1-2: the desk microphone"). The words stay on this Mac in one text file,
// Documents/UA-101 Backups/Notes.txt, beside the backups, so they are kept when the app is
// removed (as the backups are) and can be opened in any editor. Nothing goes to the unit.

import AppKit
import SwiftUI

/// The words, and the file they are kept in. Saved a second after the last key, and at once
/// when the window closes or the app quits.
final class NotesStore: ObservableObject {
    static let fileName = "Notes.txt"
    static let saveDelay = 1.0

    let url: URL
    @Published var text: String {
        didSet { if text != oldValue { scheduleSave() } }
    }
    /// What the file holds now (so an unchanged text isn't written again).
    private var saved: String
    private var pending: DispatchWorkItem?
    /// The last save that failed, for the self-test and the report: nil when all is well.
    private(set) var lastError: String?
    /// Told when saving starts to fail (once, not again on every key until a save works).
    var onError: (String) -> Void = { _ in }

    init(url: URL) {
        self.url = url
        let s = (try? String(contentsOf: url, encoding: .utf8)) ?? ""
        text = s
        saved = s
    }

    var hasUnsaved: Bool { text != saved }

    private func scheduleSave() {
        pending?.cancel()
        let w = DispatchWorkItem { [weak self] in self?.saveNow() }
        pending = w
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.saveDelay, execute: w)
    }

    /// Writes the words now if they changed. Makes the folder when it isn't there yet.
    @discardableResult func saveNow() -> Bool {
        pending?.cancel()
        pending = nil
        guard hasUnsaved else { return true }
        do {
            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try text.write(to: url, atomically: true, encoding: .utf8)
            saved = text
            lastError = nil
            return true
        } catch {
            let e = "The notes could not be saved in \(url.path): \(error.localizedDescription)"
            let first = lastError == nil
            lastError = e
            if first { onError(e) }
            return false
        }
    }
}

struct NotesView: View {
    @Environment(\.theme) private var theme
    @ObservedObject var store: NotesStore

    static let hint = "What is wired where, for example:\nInput 1-2: the microphone on the desk\nOutput 3-4: the speakers"

    var body: some View {
        ZStack(alignment: .topLeading) {
            TextEditor(text: $store.text)
                .font(.panel(13))
                .foregroundColor(theme.text)
                .scrollContentBackground(.hidden)
                .padding(8)
            if store.text.isEmpty {
                // TextEditor has no placeholder of its own.
                Text(Self.hint)
                    .font(.panel(13))
                    .foregroundColor(theme.dim)
                    .padding(.horizontal, 13)
                    .padding(.vertical, 8)
                    .allowsHitTesting(false)
            }
        }
        .background(theme.window)
    }
}

/// The window: beside the mixer the first time, then where the user left it. It takes the
/// keyboard when opened, so typing starts at once; while it has the keyboard, the mixer's
/// one-key shortcuts (M, S, C, L, the arrows…) are off, so the letters go into the notes.
final class NotesWindow: NSObject, NSWindowDelegate {
    static let title = "UA-101 Notes"
    static let frameName = "UA-101 Notes"
    static let width: CGFloat = 340

    private let store: NotesStore
    private let ui: PanelUI
    private var panel: NSPanel?

    init(store: NotesStore, ui: PanelUI) {
        self.store = store
        self.ui = ui
    }

    var isShown: Bool { panel?.isVisible == true }

    private static var mainWindow: NSWindow? { NSApp.windows.first { $0.title == "UA-101 Control" } }

    private func make() -> NSPanel {
        let p = NSPanel(contentRect: NSRect(x: 0, y: 0, width: Self.width, height: 360),
                        styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        p.title = Self.title
        p.isReleasedWhenClosed = false
        p.hidesOnDeactivate = false
        p.isFloatingPanel = false
        p.becomesKeyOnlyIfNeeded = false
        p.minSize = NSSize(width: 240, height: 160)
        p.contentView = NSHostingView(rootView: NotesView(store: store).followsAppearance())
        p.delegate = self
        let remembered = p.setFrameUsingName(Self.frameName)
            && NSScreen.screens.contains { $0.visibleFrame.contains(CGPoint(x: p.frame.minX + 40, y: p.frame.maxY - 10)) }
        if !remembered {
            if let main = Self.mainWindow, let screen = (main.screen ?? NSScreen.main)?.visibleFrame {
                p.setFrame(MessagesWindow.place(width: Self.width, height: 360, beside: main.frame, screen: screen), display: false)
            } else {
                p.center()
            }
        }
        p.setFrameAutosaveName(Self.frameName)
        // Closing the mixer closes this too, so the app can quit as before.
        NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: nil, queue: .main) { [weak self] note in
            guard (note.object as? NSWindow)?.title == "UA-101 Control" else { return }
            self?.panel?.close()
        }
        return p
    }

    /// Opens it, or brings it forward, with the keyboard.
    func show() {
        let p = panel ?? make()
        panel = p
        p.level = ui.alwaysOnTop ? .floating : .normal
        p.makeKeyAndOrderFront(nil)
    }

    func close() { panel?.close() }

    func windowDidBecomeKey(_ notification: Notification) { ui.typingNotes = true }
    func windowDidResignKey(_ notification: Notification) {
        ui.typingNotes = false
        store.saveNow()
    }
    func windowWillClose(_ notification: Notification) {
        ui.typingNotes = false
        store.saveNow()
    }
}
