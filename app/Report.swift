// Help → "Save Report for Developer…": one plain text file with what development
// and QA need to find a fault or plan a change. Nothing is sent anywhere: the user chooses
// where the file goes and hands it on. Nothing is asked of the unit either: its settings are
// the ones the app has read already.
//
// In it: the versions of the app, the helper and the plug-ins, and of macOS; an old
// go-ahead file if one is still there (nothing looks for them since link version 18);
// the unit's settings as read; the driver's settings; the messages
// of this run; the app's record (~/Library/Logs/UA-101 Control.log); the helper's log
// (/Library/Logs/ua101d.log); the plug-ins' system log lines of the last 24 hours.
// No personal data: the home folder is written as "~", the user's short and full names as
// "(user)", the Mac's name as "(this Mac)".
import AppKit
import UniformTypeIdentifiers
import Foundation

struct DeveloperReport {
    /// Where everything is read from. The self-test gives its own files and words.
    struct Sources {
        var appRecord = AppCore.recordPath
        var helperLog = "/Library/Logs/ua101d.log"
        var helper = "/Library/Application Support/ua101-driver/ua101d"
        var daemonPlist = "/Library/LaunchDaemons/io.github.delta90h.ua101d.plist"
        var audioPlugIn = "/Library/Audio/Plug-Ins/HAL/UA101.driver"
        var midiPlugIn = "/Library/Audio/MIDI Drivers/UA101MIDI.plugin"
        var goAhead = UA101_OLD_GO_AHEAD_PATH
        var memoryGoAhead = UA101_OLD_MEMORY_GO_AHEAD_PATH
        var backups = AppCore.backupFolder.path
        /// The plug-ins' system log lines; nil means `log show` is run.
        var systemLog: (() -> String)? = nil
        /// The driver's settings, read from Core Audio; nil leaves them out.
        var driver: (() -> DriverState)? = { CoreAudioDriverSettings().read() }
        /// What is taken out for privacy.
        var home = NSHomeDirectory()
        var userNames = [NSUserName(), NSFullUserName()]
        var macNames = [Host.current().localizedName ?? "", ProcessInfo.processInfo.hostName]
    }

    /// The most of each log the report takes, from its end: the record grows by itself.
    static let logLimit = 3_000_000
    static let sectionLine = String(repeating: "=", count: 78)

    static func fileName(at d: Date) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd HH.mm"
        return "UA-101 Report \(f.string(from: d)).txt"
    }

    /// Makes the whole text. Slow parts (log show, Core Audio) run here: call it off the main
    /// thread. `unit` and `messages` are taken on the main thread before.
    static func make(at date: Date, unit: UnitPart, messages: String, from src: Sources = Sources()) -> String {
        var out: [String] = []
        func section(_ title: String, _ body: String) {
            out.append(sectionLine + "\n" + title + "\n" + sectionLine + "\n" + body.trimmingCharacters(in: .newlines) + "\n")
        }
        let stamp = Self.stamp(date)
        section("UA-101 report for the developer", """
            Made: \(stamp)
            By: UA-101 Control, \(Build.name) build
            Nothing in this file was sent anywhere by the app. Home folder written as "~", user name as "(user)".
            """)
        section("Versions", versions(src))
        section("The unit's settings, as the app last read them", unit.text)
        section("The driver's settings (Core Audio)", driverText(src))
        section("Messages of this run (the Messages window, in full)", messages.isEmpty ? "(none)" : messages)
        section("Newest backups (\(Self.tilde(src.backups, src)))", backupsText(src))
        section("The app's record (\(Self.tilde(src.appRecord, src)), the last \(logLimit / 1_000_000) MB at most)", tail(src.appRecord))
        section("The helper's log (\(src.helperLog))", tail(src.helperLog))
        section("The plug-ins' system log lines, last 24 hours", src.systemLog?() ?? systemLog())
        return scrub(out.joined(separator: "\n"), src)
    }

    /// What the app knows of the unit: taken on the main thread.
    struct UnitPart {
        var text: String

        static func from(_ model: PanelModel) -> UnitPart {
            let s = model.snap
            var lines = ["State: \(s.state)\(s.writesAllowed ? ", changes on" : ", read-only")\(model.changesLocked ? " (locked by the user)" : "")",
                         "Reads sent \(s.readsSent), changes sent \(s.setsSent), memory commands sent \(s.memorySent), not taken \(s.disagreements)"]
            if let block = model.settingsAsRead {
                let b = SettingsBackup(block: block, unit: model.transport.displayName, saved: SettingsBackup.stamp(Date()), why: "for the report")
                lines.append("")
                lines.append(b.text())
            } else {
                lines.append("Not read: the app has no settings from the unit right now (it is off, not connected, or the helper isn't installed).")
            }
            return UnitPart(text: lines.joined(separator: "\n"))
        }
    }

    // MARK: the parts

    private static func stamp(_ d: Date) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd HH:mm:ss ZZZZZ"
        return f.string(from: d)
    }

    private static func versions(_ src: Sources) -> String {
        let os = ProcessInfo.processInfo.operatingSystemVersion
        var model = [CChar](repeating: 0, count: 64), size = model.count
        let hw = sysctlbyname("hw.model", &model, &size, nil, 0) == 0 ? String(cString: model) : "unknown"
        let info = Bundle.main.infoDictionary ?? [:]
        let appVersion = "\(info["CFBundleShortVersionString"] as? String ?? "?") (\(info["CFBundleVersion"] as? String ?? "?"))"
        var lines = [
            "macOS: \(os.majorVersion).\(os.minorVersion).\(os.patchVersion) (\(ProcessInfo.processInfo.operatingSystemVersionString)), \(hw)",
            "App: UA-101 Control \(appVersion), \(Build.name) build, link version \(UA101_LINK_VERSION), at \(tilde(Bundle.main.bundlePath, src))",
            "App program: " + fileLine(Bundle.main.executablePath ?? CommandLine.arguments[0]),
            "Helper: " + fileLine(src.helper),
            "Helper's launch file: " + fileLine(src.daemonPlist, hash: false),
            "Audio plug-in: " + bundleLine(src.audioPlugIn),
            "MIDI plug-in: " + bundleLine(src.midiPlugIn),
        ]
        // The two go-ahead files of link versions 5 to 17: named only if one is still there.
        for (name, path) in [("allow-changes", src.goAhead), ("allow-memory", src.memoryGoAhead)]
        where FileManager.default.fileExists(atPath: path) {
            lines.append("Old go-ahead file still there (\(name); nothing looks for it since link version 18, and the installer removes it): " + fileLine(path, hash: false))
        }
        lines.append("(The helper speaks link version \(UA101_LINK_VERSION) if the app attached to it: another version is refused, and the record says so.)")
        return lines.joined(separator: "\n")
    }

    private static let fileDate: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return f
    }()

    /// "there, 123,456 bytes, changed 2026-11-21 11:13:02, check 1A2B3C4D" or "not there".
    static func fileLine(_ path: String, hash: Bool = true) -> String {
        guard let a = try? FileManager.default.attributesOfItem(atPath: path) else { return "not there" }
        var t = "there, \((a[.size] as? NSNumber)?.intValue ?? 0) bytes"
        if let d = a[.modificationDate] as? Date { t += ", changed \(fileDate.string(from: d))" }
        if hash, let data = FileManager.default.contents(atPath: path) {
            t += String(format: ", check %08X", SettingsBackup.check(Array(data)))
        }
        return t
    }

    private static func bundleLine(_ path: String) -> String {
        guard FileManager.default.fileExists(atPath: path) else { return "not there" }
        let info = NSDictionary(contentsOfFile: path + "/Contents/Info.plist") as? [String: Any] ?? [:]
        let name = info["CFBundleExecutable"] as? String ?? ""
        return "version \(info["CFBundleShortVersionString"] as? String ?? "?") (\(info["CFBundleVersion"] as? String ?? "?")), program "
            + fileLine(path + "/Contents/MacOS/" + name)
    }

    private static func driverText(_ src: Sources) -> String {
        guard let read = src.driver else { return "(not asked)" }
        let d = read()
        guard d.found else { return "The UA-101's device isn't in Core Audio now (the unit is off, or the driver isn't installed)." }
        var lines = [d.haveSettings ? String(format: "Recording margin %.1f ms, playback delay %.1f ms", d.inputMs, d.outputMs) : "No settings (an older driver)"]
        if let c = d.correction { lines.append("Rate correction: \(c ? "on" : "off")") }
        if d.haveStats && d.anyRun {
            lines.append(String(format: "%@ run: %.0f Hz, %.0f s; offsets %.0f in, %.0f out (frames)", d.running ? "This" : "The last",
                                d.rate, d.seconds, d.inputOffset, d.outputOffset))
            lines.append("Least room: recording " + (d.captureSpare.map { String(format: "%.0f frames (%.1f ms)", $0, d.ms($0)) } ?? "not measured")
                         + ", playback " + (d.playbackSpare.map { String(format: "%.0f frames (%.1f ms)", $0, d.ms($0)) } ?? "not measured"))
            lines.append(String(format: "Recording missing %.0f frames, playback late %.0f frames", d.captureMissing, d.playbackLate))
        } else {
            lines.append("No sound has run since the driver started.")
        }
        return lines.joined(separator: "\n")
    }

    private static func backupsText(_ src: Sources) -> String {
        let names = ((try? FileManager.default.contentsOfDirectory(atPath: src.backups)) ?? [])
            .filter { $0.hasSuffix("." + SettingsBackup.fileExtension) }
        guard !names.isEmpty else { return "(none)" }
        let dated = names.map { n -> (String, Date) in
            let d = (try? FileManager.default.attributesOfItem(atPath: src.backups + "/" + n))?[.modificationDate] as? Date
            return (n, d ?? .distantPast)
        }.sorted { $0.1 > $1.1 }
        var lines = ["\(names.count) backup file\(names.count == 1 ? "" : "s"); the newest:"]
        for (n, d) in dated.prefix(10) { lines.append("  \(fileDate.string(from: d))  \(n)") }
        if let newest = dated.first, let text = try? String(contentsOfFile: src.backups + "/" + newest.0, encoding: .utf8) {
            lines.append("")
            lines.append("The newest, in full (\(newest.0)):")
            lines.append(text)
        }
        return lines.joined(separator: "\n")
    }

    /// The end of a log, at most `logLimit` bytes, from the start of a line.
    static func tail(_ path: String, limit: Int = logLimit) -> String {
        let logLimit = limit
        guard let h = FileHandle(forReadingAtPath: path) else { return "(not there, or not readable)" }
        defer { try? h.close() }
        let size = (try? h.seekToEnd()) ?? 0
        let from = size > UInt64(logLimit) ? size - UInt64(logLimit) : 0
        try? h.seek(toOffset: from)
        var data = (try? h.readToEnd()) ?? Data()
        if from > 0, let nl = data.firstIndex(of: 0x0A) { data = data.subdata(in: data.index(after: nl)..<data.endIndex) }
        let text = String(decoding: data, as: UTF8.self)
        return (from > 0 ? "(… the first \(from) bytes left out)\n" : "") + (text.isEmpty ? "(empty)" : text)
    }

    /// `log show` for the two plug-ins' subsystems (io.github.delta90h.ua101 and …ua101midi).
    /// About 5 seconds; it gives up after a minute.
    static func systemLog() -> String {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/log")
        p.arguments = ["show", "--last", "24h", "--style", "compact", "--predicate", "subsystem BEGINSWITH \"io.github.delta90h\""]
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        do { try p.run() } catch { return "(log show could not run: \(error.localizedDescription))" }
        let killer = DispatchWorkItem { if p.isRunning { p.terminate() } }
        DispatchQueue.global().asyncAfter(deadline: .now() + 60, execute: killer)
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        killer.cancel()
        let text = String(decoding: data.suffix(logLimit), as: UTF8.self)
        return text.isEmpty ? "(none)" : text
    }

    // MARK: privacy

    static func tilde(_ path: String, _ src: Sources) -> String {
        path.hasPrefix(src.home) ? "~" + path.dropFirst(src.home.count) : path
    }

    /// The home folder as "~", the user's names as "(user)", the Mac's names as "(this Mac)".
    static func scrub(_ text: String, _ src: Sources) -> String {
        var t = src.home.count > 1 ? text.replacingOccurrences(of: src.home, with: "~") : text
        func words(_ list: [String], _ with: String) {
            for name in Set(list.filter { $0.count >= 2 }).sorted(by: { $0.count > $1.count }) {
                let pattern = "(?<![A-Za-z0-9])" + NSRegularExpression.escapedPattern(for: name) + "(?![A-Za-z0-9])"
                t = t.replacingOccurrences(of: pattern, with: with, options: [.regularExpression, .caseInsensitive])
            }
        }
        words(src.macNames + src.macNames.map { $0.replacingOccurrences(of: ".local", with: "") }, "(this Mac)")
        words(src.userNames, "(user)")
        return t
    }
}

extension AppCore {
    /// Help → Save Report for Developer…: asks where, then gathers (a few seconds, off the
    /// main thread), saves, and shows the file in the Finder.
    func saveReport() {
        let folder = URL(fileURLWithPath: NSHomeDirectory() + "/Documents/UA-101 Reports", isDirectory: true)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let now = Date()
        let panel = NSSavePanel()
        panel.title = "Save Report for Developer"
        panel.message = "One text file with the app's record, the driver's logs, the unit's settings and the versions. Nothing is sent anywhere."
        panel.directoryURL = folder
        panel.nameFieldStringValue = DeveloperReport.fileName(at: now)
        panel.allowedContentTypes = [.plainText]
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let unit = DeveloperReport.UnitPart.from(model)
        let messages = self.messages.csv()
        AppCore.record("Save Report for Developer: gathering for \(url.lastPathComponent)")
        DispatchQueue.global(qos: .utility).async {
            let text = DeveloperReport.make(at: now, unit: unit, messages: messages)
            let ok = (try? text.data(using: .utf8)?.write(to: url, options: .atomic)) != nil
            DispatchQueue.main.async {
                AppCore.record("Save Report for Developer: " + (ok ? "saved, \(text.utf8.count) bytes" : "NOT SAVED"))
                if ok {
                    NSWorkspace.shared.activateFileViewerSelecting([url])
                } else {
                    let a = NSAlert()
                    a.messageText = "The report could not be saved"
                    a.informativeText = "Choose another folder and try again."
                    a.runModalFitted()
                }
            }
        }
    }
}
