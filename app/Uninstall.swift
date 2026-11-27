// UA-101 Control > Uninstall UA-101… (2026-11-24): the two ways it was chosen,
// "Remove the Driver, Keep My Settings" and "Remove Everything" (with a tick, on as delivered,
// that keeps the user's own files: Documents/UA-101 Backups and UA-101 Reports).
//
// The removing is done by the same script Terminal uses (install/uninstall.sh; the package puts
// it in the helper's folder), run as an administrator through macOS's own password window
// (AppleScript's "do shell script … with administrator privileges"): no password ever passes
// through this app, and there is one list of what goes. With "Remove Everything" the app then
// takes the grey UA-101 out of Audio MIDI Setup in this account, through CoreMIDI
// (MIDISetupRemoveDevice, what Audio MIDI Setup's Delete does), only once the MIDI server sees
// the driver gone; macOS's MIDI settings files are never touched. The app quits at the end: it
// has been removed too. With the tick off, the app moves this account's two folders to the Trash
// itself first (macOS's privacy protection refuses the script the Documents folder; see MyFiles).
import AppKit
import CoreMIDI

enum Uninstall {
    /// Where the installer package puts the script.
    static let scriptPath = "/Library/Application Support/ua101-driver/uninstall.sh"
    static let menuTitle = "Uninstall UA-101…"
    static let keepTitle = "Remove the Driver, Keep My Settings"
    static let everythingTitle = "Remove Everything"
    static let keepFilesTitle = "Keep my backup files and reports"
    /// The script's short account starts after this line; the window at the end shows that part.
    static let summaryMarker = "== What happened =="
    /// The MIDI plug-in's bundle id: what CoreMIDI names as the owner of the UA-101's MIDI device.
    static let midiDriverOwner = "io.github.delta90h.ua101.midi"
    /// The password window's words (macOS adds its own line about the password).
    static let prompt = "UA-101 Control wants to uninstall the UA-101 driver."

    enum Choice: Equatable {
        case keepSettings
        case everything(keepMyFiles: Bool)
        var isEverything: Bool { if case .everything = self { return true } else { return false } }
    }

    /// The script's arguments for a choice asked in the app by the account `uid`.
    static func arguments(_ c: Choice, uid: uid_t) -> [String] {
        var a: [String]
        switch c {
        case .keepSettings: a = ["--keep-settings"]
        case .everything(let keep): a = ["--everything"] + (keep ? [] : ["--with-my-files"])
        }
        return a + ["--asked-by=\(uid)", "--from-app"]
    }

    /// One word for the shell, in single quotes.
    static func shellQuoted(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }
    /// An AppleScript string.
    static func appleScriptQuoted(_ s: String) -> String {
        "\"" + s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
    }

    /// The status line the command adds after the script's own words, so its output is had
    /// whatever the script's exit status.
    static let statusPrefix = "uninstall.sh exit status: "

    /// The AppleScript the app runs: the script as an administrator, after macOS's password window.
    static func appleScript(_ c: Choice, uid: uid_t, script: String = scriptPath) -> String {
        let command = (["/bin/sh", script] + arguments(c, uid: uid)).map(shellQuoted).joined(separator: " ")
            + " 2>&1; echo \"\(statusPrefix)$?\""
        return "do shell script \(appleScriptQuoted(command)) with prompt \(appleScriptQuoted(prompt))"
            + " with administrator privileges without altering line endings"
    }

    enum Outcome: Equatable {
        /// The password window was closed with Cancel: nothing ran.
        case cancelled
        /// The script didn't run (or its status wasn't had): why.
        case notRun(String)
        /// The script ran: what it said, and its exit status (0: all went; 1: something stayed).
        case ran(output: String, status: Int32)
    }

    /// osascript's results read: what it printed, what it said went wrong, and its exit status.
    static func outcome(stdout: String, stderr: String, exitStatus: Int32) -> Outcome {
        if exitStatus != 0 {
            if stderr.contains("(-128)") || stderr.localizedCaseInsensitiveContains("User canceled") { return .cancelled }
            let why = stderr.trimmingCharacters(in: .whitespacesAndNewlines)
            return .notRun(why.isEmpty ? "osascript stopped with status \(exitStatus)" : why)
        }
        let lines = stdout.trimmingCharacters(in: .whitespacesAndNewlines).components(separatedBy: "\n")
        guard let last = lines.last, last.hasPrefix(statusPrefix), let status = Int32(last.dropFirst(statusPrefix.count)) else {
            return .notRun("the uninstaller's answer wasn't understood")
        }
        return .ran(output: lines.dropLast().joined(separator: "\n"), status: status)
    }

    /// What the window at the end shows: the script's short account, from its heading on.
    static func summary(of output: String) -> String {
        guard let r = output.range(of: summaryMarker + "\n") else { return output.trimmingCharacters(in: .whitespacesAndNewlines) }
        return String(output[r.upperBound...]).trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// Remove Everything with the tick off (2026-11-27): macOS's privacy protection refuses the
    /// script, run as an administrator, the Documents folder ("Operation not permitted"). This app
    /// writes there, so it moves this account's two folders to the Trash itself, just before the
    /// script runs, and puts them back if the script doesn't run. What it couldn't move, the script
    /// still tries, and lists as "kept (macOS refused)" if refused again.
    enum MyFiles {
        static let names = ["UA-101 Backups", "UA-101 Reports"]
        static func folders(home: String = NSHomeDirectory()) -> [URL] {
            names.map { URL(fileURLWithPath: home + "/Documents/" + $0, isDirectory: true) }
        }
        struct Moved: Equatable { var from: URL; var to: URL }

        /// Those that are there, to the Trash. Off the main thread (macOS may ask first).
        static func moveToTrash(_ folders: [URL], fm: FileManager = .default) -> [Moved] {
            folders.compactMap { u in
                guard fm.fileExists(atPath: u.path) else { return nil }
                var out: NSURL?
                guard (try? fm.trashItem(at: u, resultingItemURL: &out)) != nil, let to = out as URL? else { return nil }
                return Moved(from: u, to: to)
            }
        }
        /// The script didn't run: each back where it was. Returns those that couldn't go back.
        static func putBack(_ moved: [Moved], fm: FileManager = .default) -> [Moved] {
            moved.filter { m in fm.fileExists(atPath: m.from.path) || (try? fm.moveItem(at: m.to, to: m.from)) == nil }
        }
        /// The line the window at the end adds once the script has run.
        static func line(_ moved: [Moved]) -> String? {
            moved.isEmpty ? nil : "Moved to the Trash (empty the Trash to delete them for good):" + moved.map { "\n  " + $0.from.path }.joined()
        }
        /// The script didn't run, and these couldn't be put back.
        static func stillInTrash(_ moved: [Moved]) -> String? {
            moved.isEmpty ? nil : "These are in the Trash: put them back from there (Put Back) if you want them:"
                + moved.map { "\n  " + $0.from.path }.joined()
        }
    }

    /// Runs the script (off the main thread: the password window is macOS's own).
    static func run(_ c: Choice, uid: uid_t = getuid()) -> Outcome {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        p.arguments = ["-e", appleScript(c, uid: uid)]
        let out = Pipe(), err = Pipe()
        p.standardOutput = out
        p.standardError = err
        do { try p.run() } catch { return .notRun("osascript couldn't start: \(error.localizedDescription)") }
        // Read before waiting, so a long answer can't fill the pipe and hold the script up.
        let o = out.fileHandleForReading.readDataToEndOfFile(), e = err.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return outcome(stdout: String(decoding: o, as: UTF8.self), stderr: String(decoding: e, as: UTF8.self),
                       exitStatus: p.terminationStatus)
    }
}

/// The grey UA-101 in Audio MIDI Setup: the MIDI device the plug-in made, kept by macOS in this
/// account's MIDI setup after the plug-in has gone.
enum GreyMIDI {
    struct Seen: Equatable { var owner: String; var offline: Bool }
    enum Decision: Equatable { case none, wait, remove([Int]) }
    enum Result: Equatable { case removed(Int), noneFound, stillOnline, failed(OSStatus) }

    /// Only the UA-101's devices, and only once each one is offline (its driver gone): as long as
    /// one of them is online the MIDI server hasn't caught up yet, and nothing is removed.
    static func decide(_ devices: [Seen]) -> Decision {
        let ours = devices.indices.filter { devices[$0].owner == Uninstall.midiDriverOwner }
        if ours.isEmpty { return .none }
        return ours.allSatisfy { devices[$0].offline } ? .remove(ours) : .wait
    }

    private static func owner(_ d: MIDIDeviceRef) -> String {
        var s: Unmanaged<CFString>?
        guard MIDIObjectGetStringProperty(d, kMIDIPropertyDriverOwner, &s) == noErr, let v = s else { return "" }
        return v.takeRetainedValue() as String
    }
    private static func offline(_ d: MIDIDeviceRef) -> Bool {
        var v: Int32 = 0
        return MIDIObjectGetIntegerProperty(d, kMIDIPropertyOffline, &v) == noErr && v != 0
    }

    /// Looks for up to `wait` seconds (the MIDI server starts again after the script stopped it).
    /// Off the main thread.
    static func removeOffline(wait: Double = 8) -> Result {
        let deadline = Date().addingTimeInterval(wait)
        while true {
            let devices = (0..<MIDIGetNumberOfDevices()).map { MIDIGetDevice($0) }
            switch decide(devices.map { Seen(owner: owner($0), offline: offline($0)) }) {
            case .none:
                return .noneFound
            case .remove(let which):
                for i in which {
                    let status = MIDISetupRemoveDevice(devices[i])
                    if status != noErr { return .failed(status) }
                }
                return .removed(which.count)
            case .wait:
                if Date() >= deadline { return .stillOnline }
                Thread.sleep(forTimeInterval: 0.5)
            }
        }
    }

    /// The line the window at the end adds for this account.
    static func line(_ r: Result) -> String? {
        switch r {
        case .removed: return "The grey UA-101 is gone from Audio MIDI Setup in this account."
        case .noneFound: return nil
        case .stillOnline, .failed:
            return "Audio MIDI Setup may still show a grey UA-101 in this account. To remove it: open Audio MIDI Setup, "
                + "choose Window > Show MIDI Studio, click the UA-101, press Delete."
        }
    }
}

extension AppCore {
    /// UA-101 Control > Uninstall UA-101…: the question, then the script, then the end.
    func uninstall() {
        guard !uninstalling else { return }
        let a = NSAlert()
        a.alertStyle = .warning
        a.messageText = "Uninstall UA-101?"
        a.informativeText = """
            \(Uninstall.keepTitle): the driver, its MIDI port and this app go. Your Driver Settings and the app's own choices stay, for a later install.

            \(Uninstall.everythingTitle): also every UA-101 setting, record and log on this Mac, in every account.

            The UA-101's own memories and settings are not touched. Quit any music app using the UA-101 first: all sound and MIDI on this Mac stop for a moment. macOS then asks for an administrator's password. UA-101 Control quits at the end.
            """
        a.addButton(withTitle: Uninstall.keepTitle)
        a.addButton(withTitle: Uninstall.everythingTitle)
        a.addButton(withTitle: "Cancel")
        // Return does neither of the two: each is a click.
        a.buttons[0].keyEquivalent = ""
        let tick = NSButton(checkboxWithTitle: Uninstall.keepFilesTitle, target: nil, action: nil)
        tick.state = .on
        let note = NSTextField(wrappingLabelWithString: "For Remove Everything: the folders UA-101 Backups and UA-101 Reports in Documents, in every account. Untick to remove them too.")
        note.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        note.textColor = .secondaryLabelColor
        let stack = NSStackView(views: [tick, note])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 4
        stack.frame = NSRect(x: 0, y: 0, width: 320, height: 10)
        note.preferredMaxLayoutWidth = 320
        stack.setFrameSize(NSSize(width: 320, height: stack.fittingSize.height))
        a.accessoryView = stack
        let choice: Uninstall.Choice
        switch a.runModalFitted() {
        case .alertFirstButtonReturn: choice = .keepSettings
        case .alertSecondButtonReturn: choice = .everything(keepMyFiles: tick.state == .on)
        default: return
        }
        guard FileManager.default.isExecutableFile(atPath: Uninstall.scriptPath) || FileManager.default.fileExists(atPath: Uninstall.scriptPath) else {
            let b = NSAlert()
            b.messageText = "The uninstaller isn't on this Mac"
            b.informativeText = "It comes with the installer package, in \(Uninstall.scriptPath). Nothing was changed."
            b.runModalFitted()
            return
        }
        uninstalling = true
        // With everything, nothing more goes into the record: it is about to go.
        if choice.isEverything { AppCore.recordStopped = true } else { AppCore.record("Uninstall: remove the driver, keep the settings") }
        let progress = UninstallProgress()
        progress.show()
        DispatchQueue.global(qos: .userInitiated).async {
            // The user's own files in this account: to the Trash by the app (see Uninstall.MyFiles).
            let moved = choice == .everything(keepMyFiles: false) ? Uninstall.MyFiles.moveToTrash(Uninstall.MyFiles.folders()) : []
            let outcome = Uninstall.run(choice)
            var midi: GreyMIDI.Result?
            var notBack: [Uninstall.MyFiles.Moved] = []
            if case .ran = outcome {
                if choice.isEverything { midi = GreyMIDI.removeOffline() }
            } else {
                notBack = Uninstall.MyFiles.putBack(moved)
            }
            DispatchQueue.main.async { progress.close(); self.finishUninstall(choice, outcome, midi, moved: moved, notBack: notBack) }
        }
    }

    private func finishUninstall(_ choice: Uninstall.Choice, _ outcome: Uninstall.Outcome, _ midi: GreyMIDI.Result?,
                                 moved: [Uninstall.MyFiles.Moved], notBack: [Uninstall.MyFiles.Moved]) {
        let a = NSAlert()
        switch outcome {
        case .cancelled, .notRun:
            // Nothing ran: the app goes on as before (the folders it moved are back).
            uninstalling = false
            AppCore.recordStopped = false
            let trash = Uninstall.MyFiles.stillInTrash(notBack)
            if !moved.isEmpty { AppCore.record("Uninstall didn't run: the backup and report folders were put back" + (trash.map { ". " + $0 } ?? "")) }
            guard case .notRun(let why) = outcome else {
                guard let trash else { return }
                a.messageText = "The UA-101 wasn't uninstalled"
                a.informativeText = "Nothing else was changed. " + trash
                a.runModalFitted()
                return
            }
            AppCore.record("Uninstall didn't run: \(why)")
            a.messageText = "The UA-101 wasn't uninstalled"
            a.informativeText = (trash.map { "Nothing else was changed. " + $0 } ?? "Nothing was changed.") + "\n\n" + why
            a.runModalFitted()
            return
        case .ran(let output, let status):
            if !choice.isEverything { AppCore.record("Uninstall (exit status \(status)):\n" + output) }
            a.messageText = status == 0 ? "The UA-101 is uninstalled" : "The UA-101 is uninstalled, but not everything went"
            a.alertStyle = status == 0 ? .informational : .warning
            a.informativeText = ([Uninstall.summary(of: output)] + [Uninstall.MyFiles.line(moved), midi.flatMap(GreyMIDI.line)].compactMap { $0 })
                .joined(separator: "\n\n") + "\n\nUA-101 Control quits now."
            a.addButton(withTitle: "Quit")
            a.runModalFitted()
            if choice.isEverything { forgetOwnFiles() }
            // Not NSApp.terminate: nothing more is saved on the way out (window places, state).
            exit(0)
        }
    }

    /// Remove Everything, last: what this app may have written again since the script ran (its
    /// settings, its record, its saved windows), in this account.
    private func forgetOwnFiles() {
        let id = Bundle.main.bundleIdentifier ?? "local.ua101.control"
        UserDefaults.standard.removePersistentDomain(forName: id)
        CFPreferencesAppSynchronize(id as CFString)
        let lib = NSHomeDirectory() + "/Library/"
        for p in ["Preferences/\(id).plist", "Saved Application State/\(id).savedState", "Logs/UA-101 Control.log"] {
            try? FileManager.default.removeItem(atPath: lib + p)
        }
    }
}

/// A small window while the script runs: macOS's password window comes first, then the work,
/// which takes a few seconds.
final class UninstallProgress {
    private var window: NSWindow?

    func show() {
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 360, height: 90), styleMask: [.titled], backing: .buffered, defer: false)
        w.title = "Uninstall UA-101"
        w.isReleasedWhenClosed = false
        let spinner = NSProgressIndicator()
        spinner.style = .spinning
        spinner.controlSize = .small
        spinner.startAnimation(nil)
        let text = NSTextField(wrappingLabelWithString: "Uninstalling… macOS asks for an administrator's password first. This takes a few seconds.")
        text.preferredMaxLayoutWidth = 300
        let stack = NSStackView(views: [spinner, text])
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.spacing = 10
        stack.edgeInsets = NSEdgeInsets(top: 16, left: 16, bottom: 16, right: 16)
        w.contentView = stack
        w.center()
        w.level = .floating
        w.makeKeyAndOrderFront(nil)
        window = w
    }

    func close() { window?.orderOut(nil); window = nil }
}
