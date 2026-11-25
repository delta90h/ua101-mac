// The user manual inside the app: Help → "Save Manual as PDF…" saves a copy of
// the PDF that comes inside the app where the user chooses; Help → "UA-101 Control Help" opens
// the same pages as a searchable Apple help book. Both are made by `make manual` from
// manual/pages and copied into the app by `make app` and `make user-app`. Nothing here reaches
// the helper or the unit.
import AppKit
import Foundation
import UniformTypeIdentifiers

enum Manual {
    /// The Help menu, top to bottom (the help first, then
    /// the PDF, then the report).
    static let helpItems = ["UA-101 Control Help", "Save Manual as PDF…", "Save Report for Developer…"]
    /// The PDF in the app's Resources, and the name offered when saving a copy.
    static let pdfName = "UA-101 Control Manual.pdf"
    /// The help book's folder in Resources, and its identifier (= CFBundleHelpBookFolder and
    /// CFBundleHelpBookName in app/Info.plist, = BOOK and BOOK_ID in manual/build.py).
    static let helpFolder = "UA-101 Control.help"
    static let helpBookName = "local.ua101.control.help"

    /// The PDF inside `bundle`, if it is there (a bare `build/ua101panel` has none).
    static func bundledPDF(in bundle: Bundle) -> URL? {
        guard let dir = bundle.resourceURL else { return nil }
        let url = dir.appendingPathComponent(pdfName)
        return FileManager.default.fileExists(atPath: url.path) ? url : nil
    }

    /// Whether `bundle` holds the help book.
    static func hasHelpBook(in bundle: Bundle) -> Bool {
        guard let dir = bundle.resourceURL else { return false }
        return FileManager.default.fileExists(atPath: dir.appendingPathComponent(helpFolder).appendingPathComponent("Contents/Info.plist").path)
    }

    /// Copies the PDF to `destination`, replacing a file of that name (the save panel has
    /// already asked about that). nil when done; else why not, in plain words.
    static func copy(_ source: URL?, to destination: URL) -> String? {
        guard let source = source else { return "The manual is not inside this copy of the app." }
        let fm = FileManager.default
        let temp = destination.deletingLastPathComponent()
            .appendingPathComponent(".\(destination.lastPathComponent).\(getpid()).part")
        do {
            try? fm.removeItem(at: temp)
            try fm.copyItem(at: source, to: temp)
            if fm.fileExists(atPath: destination.path) {
                _ = try fm.replaceItemAt(destination, withItemAt: temp)
            } else {
                try fm.moveItem(at: temp, to: destination)
            }
            return nil
        } catch {
            try? fm.removeItem(at: temp)
            return "It could not be written there (\(error.localizedDescription))."
        }
    }
}

extension AppCore {
    /// Help → Save Manual as PDF…: asks where (Documents is offered), copies the PDF that is
    /// inside the app, and shows the copy in the Finder.
    func saveManualPDF() {
        let source = Manual.bundledPDF(in: .main)
        guard source != nil else {
            AppCore.record("Save Manual as PDF: no manual inside this app")
            Self.tell("The manual is not inside this copy of the app", "Build it with make user-app, which puts the manual in.")
            return
        }
        let panel = NSSavePanel()
        panel.title = "Save Manual as PDF"
        panel.message = "A copy of the UA-101 Control manual, to read or print away from the app."
        panel.directoryURL = URL(fileURLWithPath: NSHomeDirectory() + "/Documents", isDirectory: true)
        panel.nameFieldStringValue = Manual.pdfName
        panel.allowedContentTypes = [.pdf]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        if let why = Manual.copy(source, to: url) {
            AppCore.record("Save Manual as PDF: NOT SAVED, \(why)")
            Self.tell("The manual could not be saved", why + " Choose another folder and try again.")
        } else {
            AppCore.record("Save Manual as PDF: saved as \(url.lastPathComponent)")
            NSWorkspace.shared.activateFileViewerSelecting([url])
        }
    }

    /// Help → UA-101 Control Help: the help book in Help Viewer (searchable from the Help menu).
    func openHelp() {
        guard Manual.hasHelpBook(in: .main) else {
            Self.tell("The help is not inside this copy of the app", "Build it with make user-app, which puts the help in.")
            return
        }
        NSHelpManager.shared.registerBooks(in: .main)
        NSApp.showHelp(nil)
    }

    private static func tell(_ title: String, _ text: String) {
        let a = NSAlert()
        a.messageText = title
        a.informativeText = text
        a.runModal()
    }
}
