// The Messages window (a design choice, 2026-11-16: resizable, larger and
// smaller text, copy, export as CSV). Every note and warning that used to be a bar above the
// mixer is here, so nothing on the mixer moves when one comes or goes. The mixer keeps one
// line of fixed height for the state (UnitState) and a Messages button.
import AppKit
import Combine
import SwiftUI
import UniformTypeIdentifiers

/// What the panel's notes say, as a list: "Now" (what the model holds) and "Earlier" (what the
/// user pressed OK on), with a record of every change for the CSV export. It only watches
/// the model; the model's notes and their clear calls are as they were.
final class MessageCenter: ObservableObject {
    /// One place a message can come from: one of the model's notes. A kind holds one message
    /// at a time, and a newer text of the same kind replaces the older one (a fader drag
    /// gives one line, not fifty).
    enum Slot: Int, CaseIterable {
        case disagreement, refusal, saving, patch, volume, pan, onOff, link, solo, memory, backup, putBack, reset
    }

    struct Message: Identifiable, Equatable {
        let id: Int
        let slot: Slot
        var symbol: String
        var title: String
        var text: String
        var warning: Bool
        /// When the latest text came.
        var time: Date
        /// When the user pressed OK (Earlier only).
        var okTime: Date?
        /// What the window shows: the full text in the developer build, plain words in the
        /// end-user build. The CSV export and the record keep the full text.
        var shown: String { Build.isEndUser ? PlainWords.of(text) : text }
        /// Its colour in the window: red for a warning, grey for everything else. A volume held
        /// at 0 dB is grey too (normal use); it was amber until then.
        var level: Level { warning ? .red : .info }
    }

    enum Level: Int, Comparable {
        case info, red
        static func < (a: Level, b: Level) -> Bool { a.rawValue < b.rawValue }
    }

    /// One line of the record: a message shown, changed, OK'd, or taken away by the app.
    struct Event: Equatable {
        enum Kind: String { case shown, changed, ok = "OK", gone }
        let time: Date
        let kind: Kind
        let title: String
        let warning: Bool
        let text: String
    }

    static let earlierLimit = 100
    static let eventLimit = 5000

    /// Newest first, warnings above everything else.
    @Published private(set) var now: [Message] = []
    /// What the user pressed OK on, newest first, until the app quits (the last 100).
    @Published private(set) var earlier: [Message] = []
    /// Every change since the app opened (the last 5000), oldest first: the CSV export.
    private(set) var events: [Event] = []
    /// A message came that wasn't there, or one turned red. What follows is MessagePopups'
    /// rule (a design choice of 2026-11-23): grey ones only go into the list, red ones ask.
    var onArrival: ((Message) -> Void)?

    private weak var model: PanelModel?
    private let clock: () -> Date
    private var watchers: [AnyCancellable] = []
    private var nextID = 1
    private var okNow: Slot?

    var warningCount: Int { now.filter { $0.warning }.count }

    init(model: PanelModel, clock: @escaping () -> Date = Date.init) {
        self.model = model
        self.clock = clock
        // A @Published value comes here as it changes, before the property itself has it:
        // the new value is the one passed in, never the property read back.
        watch(model.$disagreement, slot: .disagreement) { $0.map { ("≠", $0.title, $0.text, true) } }
        watch(model.$refusal, slot: .refusal) { r in
            r.map { ($0.kind == .held ? "▲" : "✕", $0.kind == .held ? "Held" : "Not done", $0.text,
                     $0.kind == .helper || Self.isWarning($0.text)) }
        }
        func note(_ p: Published<String?>.Publisher, _ slot: Slot, _ title: String) {
            watch(p, slot: slot) { $0.map { ("i", title, $0, Self.isWarning($0)) } }
        }
        note(model.$savingNote, .saving, "The unit's saving")
        note(model.$patchNote, .patch, "Patch bay")
        note(model.$volumeNote, .volume, "Volume")
        note(model.$panNote, .pan, "Pan")
        note(model.$onNote, .onOff, "On/off")
        note(model.$pairNote, .link, "Link")
        note(model.$soloNote, .solo, "Solo")
        note(model.$memoryNote, .memory, "Memory")
        note(model.$backupNote, .backup, "Backup")
        note(model.$putBackNote, .putBack, "Open Backup")
        note(model.$resetNote, .reset, "Reset Mixer")
    }

    private func watch<T>(_ p: Published<T?>.Publisher, slot: Slot,
                          _ content: @escaping (T?) -> (String, String, String, Bool)?) {
        watchers.append(p.sink { [weak self] value in self?.set(slot, content(value)) })
    }

    /// The warnings: the sentences in CAPITALS the model writes when the unit holds something
    /// other than what was asked ("THE UNIT HOLDS ANOTHER VALUE", "NOT SENT", "OTHER SETTINGS
    /// READ BACK DIFFERENT", "THE LINKED PARTNER …", and "NOT SAVED" for a backup that didn't
    /// get into a memory). Names like "INPUT 3" or "SOFT CTRL" are
    /// capitals too, but never a "THE" followed by two more such words, nor "NOT SENT".
    static func isWarning(_ text: String) -> Bool {
        text.range(of: #"NOT SENT|NOT DONE|NOT SAVED|OTHER SETTINGS|\bTHE [A-Z']{2,} [A-Z']{2,}"#, options: .regularExpression) != nil
    }

    private func log(_ kind: Event.Kind, _ m: Message) {
        events.append(Event(time: clock(), kind: kind, title: m.title, warning: m.warning, text: m.text))
        if events.count > Self.eventLimit { events.removeFirst(events.count - Self.eventLimit) }
    }

    /// For the self-test: every text a message has had, so it can check the plain words of all.
    static var seen: ((String) -> Void)?

    private func set(_ slot: Slot, _ content: (symbol: String, title: String, text: String, warning: Bool)?) {
        if let c = content { Self.seen?(c.text) }
        let i = now.firstIndex { $0.slot == slot }
        guard let c = content else {
            if let i = i {
                if okNow != slot { log(.gone, now[i]) }
                now.remove(at: i)
            }
            return
        }
        if let i = i {
            var m = now[i]
            guard m.text != c.text || m.title != c.title || m.warning != c.warning else { return }
            let was = m.level
            m.symbol = c.symbol; m.title = c.title; m.text = c.text; m.warning = c.warning
            m.time = clock()
            now[i] = m
            log(.changed, m)
            sortNow()
            if m.level > was { onArrival?(m) }
        } else {
            let m = Message(id: nextID, slot: slot, symbol: c.symbol, title: c.title, text: c.text, warning: c.warning, time: clock())
            nextID += 1
            now.append(m)
            log(.shown, m)
            sortNow()
            onArrival?(m)
        }
    }

    private func sortNow() {
        now.sort { a, b in a.warning != b.warning ? a.warning : (a.time != b.time ? a.time > b.time : a.id > b.id) }
    }

    /// OK on one message: it goes down into Earlier with the time, and the model's note is cleared.
    func ok(_ m: Message) {
        guard let i = now.firstIndex(where: { $0.id == m.id }) else { return }
        var done = now[i]
        done.okTime = clock()
        earlier.insert(done, at: 0)
        if earlier.count > Self.earlierLimit { earlier.removeLast(earlier.count - Self.earlierLimit) }
        log(.ok, done)
        okNow = done.slot
        clear(done.slot)
        okNow = nil
        now.removeAll { $0.id == done.id } // in case the model had nothing to clear
    }

    func okAll() { for m in now { ok(m) } }
    func clearEarlier() { earlier = [] }

    private func clear(_ slot: Slot) {
        guard let model = model else { return }
        switch slot {
        case .disagreement: model.dismissDisagreement()
        case .refusal: model.dismissRefusal()
        case .saving: model.clearSavingNote()
        case .patch: model.clearPatchNote()
        case .volume: model.clearVolumeNote()
        case .pan: model.clearPanNote()
        case .onOff: model.clearOnNote()
        case .link: model.clearPairNote()
        case .solo: model.clearSoloNote()
        case .memory: model.clearMemoryNote()
        case .backup: model.clearBackupNote()
        case .putBack: model.clearPutBackNote()
        case .reset: model.clearResetNote()
        }
    }

    // MARK: words out: the clipboard and the CSV file

    private static func formatter(_ f: String) -> DateFormatter {
        let d = DateFormatter()
        d.locale = Locale(identifier: "en_US_POSIX")
        d.dateFormat = f
        return d
    }
    private static let clockFormat = formatter("HH:mm:ss")
    private static let dayFormat = formatter("yyyy-MM-dd")
    private static let fineFormat = formatter("HH:mm:ss.SSS")
    static func clockText(_ d: Date) -> String { clockFormat.string(from: d) }

    /// Plain lines for the clipboard: "18:31:02  Solo  INPUT solo to 3 …".
    static func plain(_ list: [Message]) -> String {
        list.map { "\(clockText($0.time))  \($0.warning ? "WARNING  " : "")\($0.title)  \($0.shown)" }.joined(separator: "\n")
    }

    /// Everything in the window, for "Copy All".
    func plainAll() -> String {
        var parts: [String] = []
        if !now.isEmpty { parts.append("Now\n" + Self.plain(now)) }
        if !earlier.isEmpty { parts.append("Earlier\n" + Self.plain(earlier)) }
        return parts.isEmpty ? "No messages." : parts.joined(separator: "\n\n")
    }

    static func csvField(_ s: String) -> String { "\"" + s.replacingOccurrences(of: "\"", with: "\"\"") + "\"" }

    /// The record as CSV: one row for each message shown, changed, OK'd or taken away, oldest
    /// first. A byte order mark first, so Numbers and Excel read the "−", "→" and "≠" right.
    func csv() -> String {
        var lines = ["Date,Time,Event,Kind,Warning,Message"]
        for e in events {
            lines.append([Self.dayFormat.string(from: e.time), Self.fineFormat.string(from: e.time), e.kind.rawValue,
                          e.title, e.warning ? "yes" : "no", e.text].map(Self.csvField).joined(separator: ","))
        }
        return "\u{FEFF}" + lines.joined(separator: "\r\n") + "\r\n"
    }

    static func copy(_ text: String) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    /// Asks where to save the CSV file, and saves it.
    func exportCSV() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.commaSeparatedText]
        panel.nameFieldStringValue = "UA-101 messages " + Self.formatter("yyyy-MM-dd HH-mm").string(from: Date()) + ".csv"
        panel.title = "Export the Messages as CSV"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        try? csv().data(using: .utf8)?.write(to: url)
    }
}

/// What is true now about the unit: one line on the mixer, always the same height, and the
/// box at the top of the Messages window. Not a message: it has no OK and changes by itself.
struct UnitState {
    enum Tone { case dim, warn, bad, on }
    let symbol: String
    let tone: Tone
    let title: String
    /// What the mixer's line says after the title (cut to one line).
    let short: String
    /// All of it: the Messages window, and the line's tooltip.
    let long: String
    let actionLabel: String?
    let action: (() -> Void)?

    static func of(_ model: PanelModel) -> UnitState {
        let s = model.snap
        let pretend = model.transport.isPretend
        // At 192 kHz the mixer has six channels: said at the end of the line, as the old bar did.
        let rate = s.known && s.rateCode == 4 ? " At 192 kHz the mixer has channels 1 to 6 only." : ""
        // The end-user app has no switch to turn off here: Lock Changes is in the Device menu.
        let byThemselves = model.changesByThemselves && !pretend
        let off: (String?, (() -> Void)?) = s.writesAllowed && !byThemselves ? ("Switch off", { model.allowChanges(false) }) : (nil, nil)
        switch s.state {
        case .offline:
            let text = model.linkNote ?? (pretend ? "There is no way to a unit right now." : "Looking for the UA-101…")
            return UnitState(symbol: "○", tone: .dim, title: "No unit", short: text + rate, long: text + rate, actionLabel: off.0, action: off.1)
        case .syncing:
            let text = "The mixer shows them as soon as they arrive."
            return UnitState(symbol: "…", tone: .dim, title: "Reading the unit's settings", short: text + rate, long: text + rate, actionLabel: off.0, action: off.1)
        case .silent:
            return UnitState(symbol: "!", tone: .bad, title: "The unit doesn't answer",
                             short: "Nothing can be changed; asking again every 3 seconds." + rate,
                             long: "What you see is the last that was read. Nothing can be changed; asking again every 3 seconds." + rate,
                             actionLabel: off.0, action: off.1)
        case .ready:
            break
        }
        if byThemselves { return endUser(model, rate: rate) }
        if !s.writesAllowed {
            let long = pretend
                ? "You can look at everything. Nothing is sent to the unit and changes are refused."
                : "You can look at everything. Nothing that changes the UA-101 is sent until you allow changes (Device menu); this version then changes only \(model.allowed.isEmpty ? "what the driver's helper passes on" : model.allowed.words)."
            return UnitState(symbol: "🔒", tone: .warn, title: "Read-only",
                             short: (pretend ? "Nothing is sent to the unit." : "Nothing that changes the UA-101 is sent until you allow changes.") + rate,
                             long: long + rate,
                             actionLabel: pretend ? "Allow changes (pretend unit)" : "Allow changes",
                             action: { model.allowChanges(true) })
        }
        if pretend {
            return UnitState(symbol: "✓", tone: .on, title: "Changes are on (pretend unit)", short: "Nothing goes to a real unit." + rate,
                             long: "Changes go to the pretend unit only. Nothing goes to a real unit." + rate,
                             actionLabel: off.0, action: off.1)
        }
        // Recall, Save and Reset are on with the other changes where the helper passes any on.
        let memory = s.memoryAllowed && model.allowed.someMemory
        return UnitState(symbol: "!", tone: .warn, title: "Changes are on: \(model.allowed.short)" + (memory ? " · Recall, Save, Reset" : ""),
                         short: "Every other control is refused." + rate,
                         long: "This version can change \(model.allowed.words). "
                             + (memory ? "Recall, Save and Reset of the unit's memories can be sent too (Device menu); each warns first and saves a backup. " : "")
                             + "Every other control is still refused, here and by the helper." + rate,
                         actionLabel: off.0, action: off.1)
    }

    /// The end-user app with the UA-101 read: changes on by themselves.
    private static func endUser(_ model: PanelModel, rate: String) -> UnitState {
        let s = model.snap
        if s.writesAllowed {
            let memory = s.memoryAllowed && model.allowed.someMemory
            return UnitState(symbol: "✓", tone: .on, title: "Ready",
                             short: "Changes go to the UA-101, and each is read back." + rate,
                             long: "Changes go to the UA-101, and each is read back. The driver's helper passes on only \(model.allowed.words)"
                                 + (memory ? "; and Recall, Save and Reset of the unit's memories, each of which warns first and saves a backup" : "")
                                 + ". To keep the UA-101 as it is: Device menu, Lock Changes." + rate,
                             actionLabel: "Lock Changes", action: { model.lockChanges(true) })
        }
        if model.changesLocked {
            let text = "Nothing that changes the UA-101 is sent until you unlock (Device menu, Lock Changes)."
            return UnitState(symbol: "🔒", tone: .warn, title: "Changes locked", short: text + rate, long: text + rate,
                             actionLabel: "Unlock", action: { model.lockChanges(false) })
        }
        // Changes are off, not locked: a moment before they come on by themselves, or the
        // installed helper passes on nothing at all.
        let text = model.changesOffWords
        return UnitState(symbol: "🔒", tone: .warn, title: "Look only", short: text + rate,
                         long: text + " Nothing that changes the UA-101 is sent." + rate, actionLabel: nil, action: nil)
    }

    func color(_ theme: Theme) -> Color {
        switch tone {
        case .dim: return theme.dim
        case .warn: return theme.warn
        case .bad: return theme.bad
        case .on: return theme.on
        }
    }
}

/// The state, as one line across the mixer: always exactly `height` high, whatever it says.
struct StateLine: View {
    @Environment(\.theme) private var theme
    let state: UnitState
    static let height: CGFloat = 30

    var body: some View {
        let color = state.color(theme)
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(state.symbol).font(.panel(12, .bold)).foregroundColor(color)
            Text(state.title).font(.panel(12, .bold)).foregroundColor(theme.text).layoutPriority(1).copyable()
            Text(state.short).font(.panel(12)).foregroundColor(theme.text.opacity(0.85)).copyable()
            Spacer(minLength: 8)
            if let label = state.actionLabel, let action = state.action {
                Button(label, action: action).buttonStyle(.plain)
                    .font(.panel(11, .semibold)).foregroundColor(color).layoutPriority(2)
            }
        }
        .lineLimit(1)
        .truncationMode(.tail)
        .padding(.horizontal, 12)
        .frame(height: Self.height)
        .background(RoundedRectangle(cornerRadius: 8).fill(color.opacity(0.12)).background(RoundedRectangle(cornerRadius: 8).fill(theme.label)))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(color.opacity(0.5), lineWidth: 1))
        .help(state.title + ". " + state.long)
    }
}

/// The mixer's Messages button: how many there are now, red while one is a warning. Always
/// the same size, so nothing beside it moves.
struct MessagesButton: View {
    @Environment(\.theme) private var theme
    @ObservedObject var messages: MessageCenter
    let open: () -> Void

    var body: some View {
        let n = messages.now.count, warn = messages.warningCount > 0
        Button(action: open) {
            HStack(spacing: 6) {
                Text("Messages")
                Text(warn ? "\(n) !" : "\(n)")
                    .font(.panel(10, .bold))
                    .foregroundColor(warn ? Color.white : theme.text)
                    .frame(minWidth: 26).padding(.vertical, 1)
                    .background(Capsule().fill(warn ? theme.bad : theme.dim.opacity(n > 0 ? 0.45 : 0.18)))
            }
            .frame(width: 104)
        }
        .buttonStyle(PanelButtonStyle())
        .help(n == 0 ? "No messages. Opens the Messages window." : "\(n) message\(n == 1 ? "" : "s")\(warn ? ", a warning among them" : ""). Opens the Messages window.")
    }
}

/// One message in the window. Its words can be selected and copied.
struct MessageRow: View {
    @Environment(\.theme) private var theme
    let m: MessageCenter.Message
    let k: CGFloat
    var old = false
    var ok: (() -> Void)?

    var body: some View {
        let color = m.level.color(theme)
        VStack(alignment: .leading, spacing: 3 * k) {
            HStack(alignment: .firstTextBaseline, spacing: 6 * k) {
                Text(m.symbol).font(.panel(12 * k, .bold)).foregroundColor(color)
                Text(m.title).font(.panel(12 * k, .bold)).foregroundColor(m.warning ? theme.bad : theme.text).copyable()
                Text(MessageCenter.clockText(m.time)).font(.readout(10.5 * k)).foregroundColor(theme.dim).copyable()
                Spacer(minLength: 4)
                if let ok = ok {
                    Button("OK", action: ok).buttonStyle(.plain)
                        .font(.panel(12 * k, .bold)).foregroundColor(m.warning ? theme.bad : theme.warn)
                } else if let t = m.okTime {
                    Text("OK \(MessageCenter.clockText(t))").font(.readout(10 * k)).foregroundColor(theme.dim)
                }
            }
            Text(m.shown).font(.panel(12 * k)).foregroundColor(theme.text.opacity(old ? 0.7 : 0.9))
                .fixedSize(horizontal: false, vertical: true)
                .textSelection(.enabled)
        }
        .padding(.horizontal, 9 * k).padding(.vertical, 7 * k)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8).fill(color.opacity(m.warning ? 0.16 : 0.08)).background(RoundedRectangle(cornerRadius: 8).fill(theme.label)))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(color.opacity(m.warning ? 0.75 : 0.35), lineWidth: m.warning ? 1.5 : 1))
        .opacity(old ? 0.8 : 1)
        .contextMenu {
            Button("Copy This Message") { MessageCenter.copy(MessageCenter.plain([m])) }
        }
    }
}

/// The Messages window's content: the state at the top, then Now, then Earlier.
struct MessagesView: View {
    @Environment(\.theme) private var theme
    @ObservedObject var messages: MessageCenter
    @ObservedObject var model: PanelModel
    @ObservedObject var ui: PanelUI

    var body: some View {
        let k = ui.messagesScale
        let state = UnitState.of(model)
        let color = state.color(theme)
        VStack(alignment: .leading, spacing: 0) {
            // The state: no OK, it changes by itself. The same as the mixer's line, in full.
            VStack(alignment: .leading, spacing: 4 * k) {
                HStack(alignment: .firstTextBaseline, spacing: 6 * k) {
                    Text(state.symbol).font(.panel(12 * k, .bold)).foregroundColor(color)
                    Text(state.title).font(.panel(12 * k, .bold)).foregroundColor(theme.text).copyable()
                    Spacer(minLength: 4)
                    if let label = state.actionLabel, let action = state.action {
                        Button(label, action: action).buttonStyle(.plain).font(.panel(11.5 * k, .semibold)).foregroundColor(color)
                    }
                }
                Text(state.long).font(.panel(11.5 * k)).foregroundColor(theme.text.opacity(0.85))
                    .fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
            }
            .padding(9 * k)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 8).fill(color.opacity(0.12)).background(RoundedRectangle(cornerRadius: 8).fill(theme.label)))
            .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(color.opacity(0.5), lineWidth: 1))
            .padding(10)

            HStack(spacing: 8) {
                Text("NOW").font(.panel(10.5 * k, .bold)).tracking(1).foregroundColor(theme.dim)
                Text(messages.now.isEmpty ? "no messages" : "\(messages.now.count)").font(.panel(10.5 * k)).foregroundColor(theme.dim)
                Spacer()
                Button("Copy All") { MessageCenter.copy(messages.plainAll()) }
                Button("OK to All") { messages.okAll() }.disabled(messages.now.isEmpty)
            }
            .buttonStyle(SmallButtonStyle())
            .padding(.horizontal, 10).padding(.bottom, 6)

            ScrollView {
                LazyVStack(alignment: .leading, spacing: 7 * k) {
                    if messages.now.isEmpty {
                        Text("No messages.").font(.panel(12 * k)).foregroundColor(theme.dim).copyable().padding(.vertical, 6)
                    }
                    ForEach(messages.now) { m in
                        MessageRow(m: m, k: k, ok: { messages.ok(m) })
                    }
                    if !messages.earlier.isEmpty {
                        HStack {
                            Text("EARLIER").font(.panel(10.5 * k, .bold)).tracking(1).foregroundColor(theme.dim)
                            Text("since the app opened, the last \(MessageCenter.earlierLimit)").font(.panel(10.5 * k)).foregroundColor(theme.dim)
                            Spacer()
                            Button("Clear Earlier") { messages.clearEarlier() }.buttonStyle(SmallButtonStyle())
                        }
                        .padding(.top, 8)
                        ForEach(messages.earlier) { m in
                            MessageRow(m: m, k: k, old: true)
                        }
                    }
                }
                .padding(.horizontal, 10).padding(.bottom, 10)
            }

            Rectangle().fill(theme.line).frame(height: 1)
            HStack(spacing: 8) {
                Button("Export as CSV…") { messages.exportCSV() }
                Button("Open the Full Record…") { NSWorkspace.shared.open(URL(fileURLWithPath: AppCore.recordPath)) }
                    .help("Every change sent to the unit and what came back, since the first day: ~/Library/Logs/UA-101 Control.log")
                Spacer()
                Button("A−") { ui.stepMessagesScale(up: false) }.help("Smaller text (⌥⌘−)")
                Button("A+") { ui.stepMessagesScale(up: true) }.help("Larger text (⌥⌘=)")
            }
            .buttonStyle(SmallButtonStyle())
            .padding(10)
        }
        .frame(minWidth: 300, minHeight: 240)
        .background(theme.window)
    }
}

/// A small flat button for the Messages window's rows.
struct SmallButtonStyle: ButtonStyle {
    @Environment(\.theme) private var theme
    @Environment(\.isEnabled) private var enabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.panel(11, .semibold))
            .foregroundColor(theme.text)
            .padding(.horizontal, 10).padding(.vertical, 3)
            .keycap(on: false, color: theme.link, radius: 10)
            .opacity(enabled ? (configuration.isPressed ? 0.7 : 1) : 0.4)
    }
}

/// The Messages window itself: an AppKit panel, so it can come forward without taking the
/// keyboard from the mixer. A click on OK or a button leaves the mixer the key window; only
/// selecting words makes the Messages window key.
final class MessagesWindow: NSObject, NSWindowDelegate {
    static let title = "UA-101 Messages"
    static let frameName = "UA-101 Messages"
    static let width: CGFloat = 400

    private let messages: MessageCenter
    private let model: PanelModel
    private let ui: PanelUI
    private var panel: NSPanel?

    init(messages: MessageCenter, model: PanelModel, ui: PanelUI) {
        self.messages = messages
        self.model = model
        self.ui = ui
    }

    var isShown: Bool { panel?.isVisible == true }

    /// Where it opens the first time: beside the mixer (right if there is room, else left),
    /// tops in line; with no room on either side, at the screen's right edge.
    static func place(width: CGFloat, height: CGFloat, beside main: CGRect, screen: CGRect) -> CGRect {
        let gap: CGFloat = 8
        let h = min(max(height, 240), screen.height)
        let x: CGFloat
        if main.maxX + gap + width <= screen.maxX { x = main.maxX + gap }
        else if main.minX - gap - width >= screen.minX { x = main.minX - gap - width }
        else { x = screen.maxX - width }
        let top = min(main.maxY, screen.maxY)
        return CGRect(x: x, y: max(screen.minY, top - h), width: width, height: h)
    }

    private static var mainWindow: NSWindow? { NSApp.windows.first { $0.title == "UA-101 Control" } }

    private func make() -> NSPanel {
        let p = NSPanel(contentRect: NSRect(x: 0, y: 0, width: Self.width, height: 520),
                        styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        p.title = Self.title
        p.isReleasedWhenClosed = false
        p.hidesOnDeactivate = false
        p.isFloatingPanel = false
        p.becomesKeyOnlyIfNeeded = true
        p.minSize = NSSize(width: 300, height: 240)
        p.contentView = NSHostingView(rootView: MessagesView(messages: messages, model: model, ui: ui).followsAppearance())
        p.delegate = self
        p.level = ui.alwaysOnTop ? .floating : .normal
        // Where the user left it last time, if that is still on a screen; else beside the mixer.
        let remembered = p.setFrameUsingName(Self.frameName)
            && NSScreen.screens.contains { $0.visibleFrame.contains(CGPoint(x: p.frame.minX + 40, y: p.frame.maxY - 10)) }
        if !remembered {
            if let main = Self.mainWindow, let screen = (main.screen ?? NSScreen.main)?.visibleFrame {
                p.setFrame(Self.place(width: Self.width, height: main.frame.height, beside: main.frame, screen: screen), display: false)
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

    /// Opens it, or brings it forward. Never takes the keyboard from the mixer, and never
    /// moves the mixer.
    func show() {
        let p = panel ?? make()
        panel = p
        p.level = ui.alwaysOnTop ? .floating : .normal
        p.orderFront(nil)
    }

    func close() { panel?.close() }
}

extension MessageCenter.Level {
    func color(_ theme: Theme) -> Color {
        switch self {
        case .info: return theme.dim
        case .red: return theme.bad
        }
    }
}

/// The rules since 2026-11-23. A grey message only goes into the list; the Messages window opens only when the user
/// opens it. A red one (the unit kept another value, the helper refused, a run stopped part
/// way: always after the fact) comes up by itself in a small window with one button, OK, which
/// is the same as OK in the Messages window: the message moves down into Earlier, with the
/// time. Nothing else happens; changes stay as they are. One at a time; the next waits.
final class MessagePopups {
    private let messages: MessageCenter
    /// Shows one message and calls back once when the user has pressed OK: the app's window,
    /// or the self-test's stand-in.
    var show: (MessageCenter.Message, @escaping () -> Void) -> Void
    /// The one shown now.
    private(set) var showing: MessageCenter.Message?
    private var waiting: [Int] = []
    /// How many have been shown (for the self-test).
    private(set) var shown = 0

    init(messages: MessageCenter, show: @escaping (MessageCenter.Message, @escaping () -> Void) -> Void) {
        self.messages = messages
        self.show = show
    }

    /// Red pops up; grey only goes into the list.
    static func popsUp(_ m: MessageCenter.Message) -> Bool { m.level == .red }

    /// From MessageCenter.onArrival, just after the change (never inside it).
    func arrived(_ m: MessageCenter.Message) {
        guard Self.popsUp(m), showing?.id != m.id, !waiting.contains(m.id) else { return }
        waiting.append(m.id)
        next()
    }

    private func next() {
        while showing == nil, !waiting.isEmpty {
            let id = waiting.removeFirst()
            // OK'd in the Messages window meanwhile, or no longer red: not shown.
            guard let m = messages.now.first(where: { $0.id == id }), Self.popsUp(m) else { continue }
            showing = m
            shown += 1
            show(m) { [weak self] in self?.ok() }
        }
    }

    /// OK on the pop-up: the message goes into Earlier, and the next one, if any, comes up.
    func ok() {
        guard let m = showing else { return }
        showing = nil
        if let current = messages.now.first(where: { $0.id == m.id }) { messages.ok(current) }
        next()
    }
}

/// The pop-up itself: what the message says, and OK.
struct PopupView: View {
    @Environment(\.theme) private var theme
    let m: MessageCenter.Message
    let ok: () -> Void

    var body: some View {
        let color = m.level.color(theme)
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Text(m.symbol).font(.panel(15, .bold)).foregroundColor(color)
                Text(m.title).font(.panel(15, .bold)).foregroundColor(color)
                Spacer(minLength: 4)
                Text(MessageCenter.clockText(m.time)).font(.readout(11)).foregroundColor(theme.dim)
            }
            Text(m.shown).font(.panel(12.5)).foregroundColor(theme.text)
                .fixedSize(horizontal: false, vertical: true).textSelection(.enabled)
            Text("This already happened; nothing more is done. The message stays in the Messages window, under Earlier.")
                .font(.panel(11.5)).foregroundColor(theme.text.opacity(0.75))
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Spacer()
                Button("OK", action: ok).keyboardShortcut(.defaultAction)
            }
            .buttonStyle(PanelButtonStyle())
        }
        .padding(16)
        .frame(width: 420, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10).fill(color.opacity(0.10)))
        .background(theme.window)
    }
}

/// The app's pop-up window: in front of the mixer and the Messages window, above other apps'
/// windows, and nothing waits on it (the meters and everything else go on). Closing it is OK.
final class PopupWindow: NSObject, NSWindowDelegate {
    static let title = "UA-101 Warning"
    private var panel: NSPanel?
    private var reply: (() -> Void)?

    func show(_ m: MessageCenter.Message, ok: @escaping () -> Void) {
        panel?.close()
        reply = ok
        let p = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 420, height: 200),
                        styleMask: [.titled, .closable], backing: .buffered, defer: false)
        p.title = Self.title
        p.isReleasedWhenClosed = false
        p.hidesOnDeactivate = false
        p.level = .floating
        p.delegate = self
        let host = NSHostingView(rootView: PopupView(m: m, ok: { [weak self] in self?.finish() }).followsAppearance())
        p.contentView = host
        p.setContentSize(host.fittingSize)
        if let main = NSApp.windows.first(where: { $0.title == "UA-101 Control" && $0.isVisible }) {
            let f = p.frame
            p.setFrameOrigin(NSPoint(x: main.frame.midX - f.width / 2, y: main.frame.midY - f.height / 2 + main.frame.height / 6))
        } else {
            p.center()
        }
        panel = p
        // Return is OK when the app is in front; with another app in front the pop-up shows
        // without taking its keyboard.
        if NSApp.isActive { p.makeKeyAndOrderFront(nil) } else { p.orderFrontRegardless() }
    }

    private func finish() {
        let r = reply
        reply = nil
        panel?.close()
        panel = nil
        r?()
    }

    func windowWillClose(_ notification: Notification) {
        guard (notification.object as? NSPanel) === panel else { return }
        let r = reply
        reply = nil
        panel = nil
        r?()
    }
}

/// The end-user build's Messages window says what happened in words, without the bytes the
/// notes carry for development and QA: no "sent F0 41 …", no "d[40…45] 0A 14 →
/// 10 00", no "(0x100001)". The note itself is unchanged; the record file and the CSV export
/// keep it in full. Whether a message is a warning is decided on the full text, as before.
enum PlainWords {
    private static let hexRun = #"[0-9A-F]{2}(?: [0-9A-F]{2})*"#
    private static let byte = #"[0-9A-F]{2}(?:→[0-9A-F]{2}|, (?:the app's factory|wanted|backup) [0-9A-F]{2})?"#
    private static let rules: [(String, String)] = [
        // The messages themselves: "sent F0 41 … F7", "and, for INPUT 2, which is linked with it, F0 … F7", "with F0 … F7"
        (#", which is linked with it, F0(?: [0-9A-F]{2})+ F7"#, ", which is linked with it"),
        (#" sent F0(?: [0-9A-F]{2})+ F7 and, for ([^,]+), which is linked with it"#, " sent, and to $1, which is linked with it"),
        (#" sent F0(?: [0-9A-F]{2})+ F7"#, " sent"),
        (#" with F0(?: [0-9A-F]{2})+ F7"#, ""),
        (#"(?:handed to the helper|one message to send):? F0(?: [0-9A-F]{2})+ F7"#, "one message"),
        // A list of changed bytes after a warning: the record has them.
        (#"OTHER SETTINGS READ BACK DIFFERENT, (\d+) bytes?: [^.]*\."#, "OTHER SETTINGS READ BACK DIFFERENT ($1 places)."),
        // "d[9] (a patch bay row this app doesn't change): the backup holds 05, the unit 04"
        (#"d\[\d+\] \(([^)]*)\): the backup holds [0-9A-F]{2}, the unit [0-9A-F]{2}"#, "$1 (the backup and the unit differ)"),
        // Single bytes with their names: "d[5] 00→01 (INPUT 3 volume)", "d[170] 02, backup 00 (INPUT 8 volume)"
        (#"d\[\d+\] "# + byte + #" \(([^)]*)\)"#, "$1"),
        // An on/off bit: "The unit took it: d[33] 00→02, bit 1."
        (#": d\[\d+\] [0-9A-F]{2}→[0-9A-F]{2}, bit \d"#, ""),
        // A byte without a name: "OUTPUT 1-2 d[12] 02→07"
        (#" ?d\[\d+\] [0-9A-F]{2}→[0-9A-F]{2}"#, ""),
        // A place with its old and new bytes, and what was wanted: "d[40…45] 0A 14 → 10 00, wanted 10 00"
        (#":? ?\(?d\[\d+(?:…\d+)?\] "# + hexRun + #"(?: → "# + hexRun + #")?(?:, wanted "# + hexRun + #")?\)?"#, ""),
        // Places alone: "(d[22] to d[247])", "(the first: d[13])", "d[170] (a part of …)"
        (#",? ?\((?:the first: )?d\[\d+\](?: to d\[\d+\])?\)"#, ""),
        (#"d\[\d+\] \("#, "("),
        (#" ?\bd\[\d+(?:…\d+)?\]"#, ""),
        (#"Every other byte of the 256 read back the same\."#, "Nothing else changed."),
        (#" ?\(0x[0-9A-F]+\)"#, ""),
        (#",? ?\b0x[0-9A-F]+\b"#, ""),
        (#"\bbit [01] \(([^)]*)\)"#, "$1"),
        (#"\b\d+ \((no solo|[A-Z]+ \d+ alone)\)"#, "$1"),
        // Whatever bytes are left: two or more in a row.
        (#" ?\b[0-9A-F]{2}(?: [0-9A-F]{2})+\b"#, ""),
        (#"  +"#, " "),
        (#" ([.,;:])"#, "$1"),
        (#"\(\s*\)"#, ""),
        (#"[,:]\s*\."#, "."),
        (#"\.\s*\."#, "."),
    ]
    private static let compiled: [(NSRegularExpression, String)] = rules.map { (try! NSRegularExpression(pattern: $0.0), $0.1) }

    private static let quoted = try! NSRegularExpression(pattern: "“[^”]*”")

    static func of(_ text: String) -> String {
        // File names in “…” stay as they are ("2026-11-16 18.30 before Reset Mixer.ua101").
        var names: [String] = []
        var t = text
        for m in quoted.matches(in: text, range: NSRange(text.startIndex..., in: text)).reversed() {
            guard let r = Range(m.range, in: t) else { continue }
            names.insert(String(t[r]), at: 0)
            t.replaceSubrange(r, with: "\u{E000}")
        }
        for (re, with) in compiled {
            t = re.stringByReplacingMatches(in: t, range: NSRange(t.startIndex..., in: t), withTemplate: with)
        }
        for name in names {
            if let r = t.range(of: "\u{E000}") { t.replaceSubrange(r, with: name) }
        }
        return t.trimmingCharacters(in: .whitespaces)
    }

    /// What may never show in the end-user window: bytes, places and hex numbers.
    static func hasBytes(_ text: String) -> Bool {
        let text = quoted.stringByReplacingMatches(in: text, range: NSRange(text.startIndex..., in: text), withTemplate: "“…”")
        return text.range(of: #"\b[0-9A-F]{2} [0-9A-F]{2}\b|d\[\d|0x[0-9A-F]|\bF0\b"#, options: .regularExpression) != nil
    }
}
