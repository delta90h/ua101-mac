// The dialogs: patch bay, Load / Store / Reset with their confirmation step,
// driver settings, and the pretend unit's front panel.
import SwiftUI

struct DialogFrame<Content: View>: View {
    @Environment(\.theme) private var theme
    let title: String
    let subtitle: String
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.panel(16, .bold)).foregroundColor(theme.text).copyable()
                Text(subtitle).font(.panel(12)).foregroundColor(theme.dim)
                    .fixedSize(horizontal: false, vertical: true).copyable()
            }
            content
        }
        .padding(18)
        .background(theme.window)
    }
}

/// The state every dialog shows above its own content: one line, always the same height
/// (2026-11-16, as on the mixer), and the Messages button. The notes, refusals and warnings
/// a dialog's clicks bring are in the Messages window, so nothing in the dialog moves.
struct DialogStateLine: View {
    enum Kind { case patchBay, memory }
    @ObservedObject var model: PanelModel
    let kind: Kind
    var messages: MessageCenter?
    var openMessages: () -> Void = {}

    var body: some View {
        HStack(spacing: 8) {
            StateLine(state: Self.state(model, kind))
            if let m = messages { MessagesButton(messages: m, open: openMessages) }
        }
    }

    static func state(_ model: PanelModel, _ kind: Kind) -> UnitState {
        let s = model.snap
        func line(_ symbol: String, _ tone: UnitState.Tone, _ title: String, _ short: String, _ long: String? = nil) -> UnitState {
            UnitState(symbol: symbol, tone: tone, title: title, short: short, long: long ?? short, actionLabel: nil, action: nil)
        }
        if s.state == .silent { return line("!", .bad, "The unit doesn't answer", "Nothing can be changed.", "Nothing can be changed until it does.") }
        if !s.known { return line("…", .dim, "Settings not known", "Waiting for the unit's settings.") }
        if !s.writesAllowed { return line("🔒", .warn, "Read-only", "Nothing is sent to the unit.") }
        switch kind {
        case .memory:
            if !model.transport.isPretend, !model.allowed.someMemory {
                return line("🔒", .warn, "Not with this helper", "It passes none on.", "The driver's helper that is installed passes on no Recall, Save or Reset.")
            }
            if let why = model.memoryBlocker() { return line("🔒", .warn, "Not yet", "The reason is below.", why) }
            return line("!", .warn, "Allowed", "Sent only after the next step.",
                        "Recall, Save and Reset are allowed. Nothing is sent before you confirm the next step.")
        case .patchBay:
            if model.transport.isPretend {
                return line("✓", .on, "Changes are on (pretend unit)", "Each change is read back from the unit.")
            }
            if !model.allowed.allPatch {
                let long = model.allowed.patchOutputs.isEmpty
                    ? "With the UA-101 this version changes nothing in the patch bay."
                    : "With the UA-101 this version changes \(model.allowed.patchOutputs.sorted().map { ChannelNames.outputPairs[$0] }.joined(separator: ", ")) only. The other rows and the presets are refused, here and by the driver's helper."
                return line("!", .warn, "Changes are on: \(model.allowed.short)", long, long)
            }
            return line("!", .warn, "Changes are on: \(model.allowed.short)", "Every row and both presets can be changed; each is read back.")
        }
    }
}

// MARK: put back from backup

/// Open Backup, step 2: the chosen file, what would change, what
/// can't be put back, and the one button that sends it. Step 1 was choosing the file.
struct PutBackView: View {
    @Environment(\.theme) private var theme
    @ObservedObject var model: PanelModel
    var close: () -> Void = {}
    var messages: MessageCenter?
    var openMessages: () -> Void = {}

    static let width: CGFloat = 560
    /// The list scrolls inside this height, so a long one doesn't push the buttons away.
    static let listHeight: CGFloat = 190

    /// The warning's words: what is overwritten, and what is saved first.
    static let overwriteWords = "The patch bay and the mixer in use now are overwritten with the backup's, in the settings listed above. Volumes, mutes and the patch bay change at once, and what you hear can jump."
    static let backupFirstWords = "A backup of the settings in use now is saved first, by itself, so you can go back with Open Backup. Turn the monitors down and take headphones off first."

    var body: some View {
        DialogFrame(title: "Open Backup",
                    subtitle: "Sets the patch bay and the mixer as they were when the backup was saved, with the same ordinary changes the mixer's own controls make.") {
            HStack(spacing: 8) {
                StateLine(state: Self.state(model))
                if let m = messages { MessagesButton(messages: m, open: openMessages) }
            }
            if let p = model.pendingPutBack {
                chosen(p)
            } else {
                Text(model.jobRunning ? "Under way. What it did is in the Messages window." : "No backup is chosen.")
                    .font(.panel(12)).foregroundColor(theme.dim).copyable()
                HStack {
                    Spacer()
                    Button("Close", action: close).buttonStyle(PanelButtonStyle()).keyboardShortcut(.cancelAction)
                }
            }
        }
        .frame(width: Self.width)
    }

    static func state(_ model: PanelModel) -> UnitState {
        func line(_ symbol: String, _ tone: UnitState.Tone, _ title: String, _ short: String) -> UnitState {
            UnitState(symbol: symbol, tone: tone, title: title, short: short, long: short, actionLabel: nil, action: nil)
        }
        if let why = model.putBackBlocker() { return line("🔒", .warn, "Not yet", why) }
        return line("!", .warn, "Ready", "Nothing is sent before you press Open Backup.")
    }

    private func heading(_ text: String) -> some View {
        Text(text).font(.panel(9, .bold)).foregroundColor(theme.dim).copyable()
    }

    @ViewBuilder private func chosen(_ p: PanelModel.PendingPutBack) -> some View {
        let plan = p.plan
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 3) {
                heading("THE BACKUP")
                Text(p.name).font(.panel(13, .bold)).foregroundColor(theme.text).copyable()
                Text("Saved \(p.backup.saved) from \(p.backup.unit == SettingsBackup.realUnit ? "the UA-101" : "the pretend unit")" + (p.backup.why.isEmpty ? "" : " (\(p.backup.why))") + ".")
                    .font(.panel(11)).foregroundColor(theme.dim).copyable()
            }
            VStack(alignment: .leading, spacing: 4) {
                heading(plan.changes.isEmpty ? "WHAT WILL CHANGE: NOTHING" : "WHAT WILL CHANGE: \(plan.changes.count) SETTING\(plan.changes.count == 1 ? "" : "S")")
                if plan.changes.isEmpty {
                    Text("The unit already holds everything this app can put back.")
                        .font(.panel(12)).foregroundColor(theme.text).copyable()
                } else {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 2) {
                            ForEach(Array(plan.changes.enumerated()), id: \.offset) { _, c in
                                HStack(spacing: 6) {
                                    Text(c.what).font(.panel(11.5, .semibold)).foregroundColor(theme.text)
                                        .frame(width: 210, alignment: .leading)
                                    Text(c.from).font(.readout(11)).foregroundColor(theme.dim)
                                    Text("→").font(.panel(11)).foregroundColor(theme.dim)
                                    Text(c.to).font(.readout(11, .semibold)).foregroundColor(theme.text)
                                    Spacer(minLength: 0)
                                }
                                .textSelection(.enabled)
                            }
                        }
                        .padding(8)
                    }
                    .frame(height: min(Self.listHeight, CGFloat(plan.changes.count) * 17 + 18))
                    .background(RoundedRectangle(cornerRadius: 8).fill(theme.label))
                    .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(theme.line, lineWidth: 1))
                }
            }
            VStack(alignment: .leading, spacing: 4) {
                heading("WHAT CAN'T BE PUT BACK")
                Text("The switches on the box: the sample rate, MONO, SOFT CTRL and the digital input. They are switches, so only your hand moves them.")
                    .font(.panel(11.5)).foregroundColor(theme.text).fixedSize(horizontal: false, vertical: true).copyable()
                ForEach(Array((plan.cantPutBack + plan.held).enumerated()), id: \.offset) { _, line in
                    Text("• " + line).font(.panel(11.5)).foregroundColor(theme.warn)
                        .fixedSize(horizontal: false, vertical: true).copyable()
                }
            }
            // The warning before changing covers Open Backup too (a design choice, 2026-11-22),
            // in the same order as before a Recall, a Save or a Reset.
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 8) {
                    Text("⚠︎").font(.panel(15, .bold)).foregroundColor(theme.bad)
                    Text("Warning: Open Backup?").font(.panel(13.5, .bold)).foregroundColor(theme.text).copyable()
                }
                Text(Self.overwriteWords).font(.panel(11.5)).foregroundColor(theme.text)
                    .fixedSize(horizontal: false, vertical: true).copyable()
                Text(MemoryCommand.cantBeUndone).font(.panel(12.5, .bold)).foregroundColor(theme.bad)
                    .fixedSize(horizontal: false, vertical: true).copyable()
                Text(Self.backupFirstWords).font(.panel(11.5, .semibold)).foregroundColor(theme.bad)
                    .fixedSize(horizontal: false, vertical: true).copyable()
            }
            .padding(10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 8).fill(theme.bad.opacity(0.10)))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(theme.bad.opacity(0.5), lineWidth: 1))
            HStack {
                Text("What it did, and whether the unit matches the backup, comes in the Messages window.")
                    .font(.panel(11)).foregroundColor(theme.dim).fixedSize(horizontal: false, vertical: true).copyable()
                Spacer()
                Button("Cancel") {
                    model.cancelPutBack()
                    close()
                }.buttonStyle(PanelButtonStyle()).keyboardShortcut(.cancelAction)
                Button("Open Backup") {
                    if model.confirmPutBack() == Int32(PANEL_OK) { close() }
                }
                .buttonStyle(PanelButtonStyle(prominent: true, color: theme.bad))
                .disabled(model.putBackBlocker() != nil)
            }
        }
    }
}

// MARK: a backup into a memory

/// Copy Backup to Memory: the chosen file, the memory, what
/// happens in order, and the one button that does all of it. The file's settings go onto the
/// unit, are stored into the memory, and the settings in use now are put back.
struct IntoMemoryView: View {
    @Environment(\.theme) private var theme
    @ObservedObject var model: PanelModel
    var close: () -> Void = {}
    var messages: MessageCenter?
    var openMessages: () -> Void = {}

    var body: some View {
        DialogFrame(title: "Copy Backup to Memory",
                    subtitle: "Writes a backup file's patch bay and mixer into one of the unit's five memories. The settings in use now are put back at the end.") {
            HStack(spacing: 8) {
                StateLine(state: Self.state(model))
                if let m = messages { MessagesButton(messages: m, open: openMessages) }
            }
            if let p = model.pendingIntoMemory {
                chosen(p)
            } else {
                Text(model.jobRunning ? "Under way. What it did is in the Messages window." : "No backup is chosen.")
                    .font(.panel(12)).foregroundColor(theme.dim).copyable()
                HStack {
                    Spacer()
                    Button("Close", action: close).buttonStyle(PanelButtonStyle()).keyboardShortcut(.cancelAction)
                }
            }
        }
        .frame(width: PutBackView.width)
    }

    static func state(_ model: PanelModel) -> UnitState {
        func line(_ symbol: String, _ tone: UnitState.Tone, _ title: String, _ short: String) -> UnitState {
            UnitState(symbol: symbol, tone: tone, title: title, short: short, long: short, actionLabel: nil, action: nil)
        }
        if let why = model.intoMemoryBlocker() { return line("🔒", .warn, "Not yet", why) }
        return line("!", .warn, "Ready", "Nothing is sent before you confirm.")
    }

    private func heading(_ text: String) -> some View {
        Text(text).font(.panel(9, .bold)).foregroundColor(theme.dim).copyable()
    }

    @ViewBuilder private func chosen(_ p: PanelModel.PendingIntoMemory) -> some View {
        let name = ChannelNames.memories[p.memory]
        let n = p.plan.changes.count
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 3) {
                heading("THE BACKUP")
                Text(p.name).font(.panel(13, .bold)).foregroundColor(theme.text).copyable()
                Text("Saved \(p.backup.saved) from \(p.backup.unit == SettingsBackup.realUnit ? "the UA-101" : "the pretend unit")" + (p.backup.why.isEmpty ? "" : " (\(p.backup.why))") + ".")
                    .font(.panel(11)).foregroundColor(theme.dim).fixedSize(horizontal: false, vertical: true).copyable()
            }
            VStack(alignment: .leading, spacing: 6) {
                heading("INTO WHICH MEMORY")
                HStack(spacing: 6) {
                    ForEach(0..<5, id: \.self) { m in
                        Button(action: { model.setIntoMemory(m) }) {
                            Text("\(m + 1)").font(.readout(14, .bold))
                                .foregroundColor(p.memory == m ? theme.onText : theme.text)
                                .frame(width: 44, height: 34)
                                .keycap(on: p.memory == m, color: theme.link, radius: 7)
                        }.buttonStyle(.plain)
                    }
                }
            }
            VStack(alignment: .leading, spacing: 4) {
                heading("WHAT HAPPENS, IN THIS ORDER")
                Group {
                    Text("1. The settings in use now are read and saved as a backup.")
                    Text("2. The file's patch bay and mixer are put on the unit: \(n == 0 ? "nothing differs, so nothing is sent" : "\(n) setting\(n == 1 ? "" : "s") change\(n == 1 ? "s" : "")"), with the same ordinary changes the mixer's own controls make.")
                    #if UA101_USER
                    Text("3. They are saved to \(name), the patch bay and the mixer together.")
                    #else
                    Text("3. They are saved to \(name), the patch bay and the mixer together, with this one message:")
                    Text(Format.hex(p.bytes)).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                    #endif
                    Text("4. Your settings from step 1 are put back, and the unit is read to see that they are.")
                }
                .font(.panel(11.5)).foregroundColor(theme.text).fixedSize(horizontal: false, vertical: true).copyable()
                ForEach(Array((p.plan.cantPutBack + p.plan.held).enumerated()), id: \.offset) { _, line in
                    Text("• Not in the memory as the file has it: " + line).font(.panel(11.5)).foregroundColor(theme.warn)
                        .fixedSize(horizontal: false, vertical: true).copyable()
                }
            }
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 8) {
                    Text("⚠︎").font(.panel(15, .bold)).foregroundColor(theme.bad)
                    Text("Turn the monitors down and take headphones off first.").font(.panel(12.5, .bold)).foregroundColor(theme.text).copyable()
                }
                Text("For a few seconds the unit plays with the file's settings: volumes, mutes and the patch bay jump there and back. What \(name) holds now is overwritten and can't be brought back from here.")
                    .font(.panel(11.5)).foregroundColor(theme.text).fixedSize(horizontal: false, vertical: true).copyable()
            }
            .padding(10)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 8).fill(theme.bad.opacity(0.10)))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(theme.bad.opacity(0.5), lineWidth: 1))
            HStack {
                Text(model.intoMemoryBlocker() ?? "What it did comes in the Messages window. It takes up to a quarter of a minute.")
                    .font(.panel(11)).foregroundColor(model.intoMemoryBlocker() == nil ? theme.dim : theme.warn)
                    .fixedSize(horizontal: false, vertical: true).copyable()
                Spacer()
                Button("Cancel") {
                    model.cancelIntoMemory()
                    close()
                }.buttonStyle(PanelButtonStyle()).keyboardShortcut(.cancelAction)
                Button("Copy to \(name)") {
                    if model.confirmIntoMemory() == Int32(PANEL_OK) { close() }
                }
                .buttonStyle(PanelButtonStyle(prominent: true, color: theme.bad))
                .disabled(model.intoMemoryBlocker() != nil)
            }
        }
    }
}

// MARK: patch bay

struct PatchBayView: View {
    @Environment(\.theme) private var theme
    @ObservedObject var model: PanelModel
    var close: () -> Void = {}
    var messages: MessageCenter?
    var openMessages: () -> Void = {}

    private static let cell: CGFloat = 46
    private static let rowLabel: CGFloat = 156
    private static let rowHeight: CGFloat = 36
    private static let words: CGFloat = 150
    /// The grid as drawn: the row label, eleven patch points, the words, and its padding.
    private static let gridWidth: CGFloat = rowLabel + cell * 11 + words + 20

    var body: some View {
        let s = model.snap
        DialogFrame(title: "Patch Bay", subtitle: "What each pair of OUTPUT sockets plays: the green light in its row. Click a dark point to change it.") {
            // Held to the grid's width, so a long sentence wraps and doesn't widen the window.
            DialogStateLine(model: model, kind: .patchBay, messages: messages, openMessages: openMessages)
                .frame(width: Self.gridWidth)
            VStack(alignment: .leading, spacing: 4) {
                // Column headings, in three blocks.
                HStack(spacing: 0) {
                    Color.clear.frame(width: Self.rowLabel, height: 1)
                    heading("MIX", columns: 1)
                    heading("INPUT (straight through)", columns: 5)
                    heading("WAVE OUT (from the computer)", columns: 5)
                }
                HStack(alignment: .top, spacing: 0) {
                    Color.clear.frame(width: Self.rowLabel, height: 1)
                    ForEach(ChannelNames.sources, id: \.value) { src in
                        VStack(spacing: 1) {
                            SocketGlyph(kind: src.socket, shines: false, size: 14)
                            Text(src.short).font(.readout(10, .semibold)).foregroundColor(theme.dim).copyable()
                            Text(src.tag).font(.panel(7, .bold)).tracking(0.3).lineLimit(1).fixedSize()
                                .foregroundColor(theme.text.opacity(0.85)).frame(height: 9).copyable()
                        }
                        .frame(width: Self.cell)
                        .help(src.long)
                    }
                }
                ForEach(0..<5, id: \.self) { out in
                    row(out, known: s.known, patch: s.patch[out])
                }
            }
            .padding(10)
            .inset(12)
            .opacity(s.known ? 1 : 0.3)

            VStack(alignment: .leading, spacing: 6) {
                Text("PRESETS").font(.panel(9, .bold)).foregroundColor(theme.dim).copyable()
                HStack(alignment: .firstTextBaseline, spacing: 10) {
                    Button("Hard Thru…") { model.askHardThru() }
                    Text("every INPUT pair straight to the same OUTPUT pair (asks first)").font(.panel(11)).foregroundColor(theme.dim).copyable()
                }
                // Hard Thru's second step: what it does, in words, before anything is sent.
                if model.hardThruAsked {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Send Hard Thru?").font(.panel(12, .bold)).foregroundColor(theme.text).copyable()
                        Text(PanelModel.hardThruWarning).font(.panel(11)).foregroundColor(theme.text)
                            .fixedSize(horizontal: false, vertical: true).copyable()
                        HStack(spacing: 10) {
                            Button("Cancel") { model.cancelHardThru() }.keyboardShortcut(.cancelAction)
                            Button("Send Hard Thru") { model.confirmHardThru() }
                        }
                    }
                    .padding(10)
                    .frame(width: Self.gridWidth - 24, alignment: .leading)
                    .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(theme.warn, lineWidth: 1))
                }
                HStack(alignment: .firstTextBaseline, spacing: 10) {
                    Button("Initial Setting") { model.patchPreset(hardThru: false) }
                    Text("every WAVE OUT pair to the same OUTPUT pair (as the unit is delivered)").font(.panel(11)).foregroundColor(theme.dim).copyable()
                }
            }
            .buttonStyle(PanelButtonStyle())
            HStack {
                Text(s.busy ? "Sending and checking…" : "Each change is read back from the unit.")
                    .font(.panel(11)).foregroundColor(theme.dim).copyable()
                Spacer()
                Button("Done", action: close).buttonStyle(PanelButtonStyle(prominent: true)).keyboardShortcut(.defaultAction)
            }
        }
        .fixedSize()
    }

    private func heading(_ text: String, columns: Int) -> some View {
        Text(text).font(.panel(9, .bold)).foregroundColor(theme.dim).lineLimit(1).copyable()
            .frame(width: Self.cell * CGFloat(columns) - 6, height: 16)
            .background(RoundedRectangle(cornerRadius: 3).fill(theme.panel))
            .padding(.horizontal, 3)
    }

    /// One OUTPUT pair: its socket and name, the eleven patch points, and in
    /// plain words what it plays. The chosen point is a lit green diode.
    private func row(_ out: Int, known: Bool, patch: UInt32) -> some View {
        let column = ChannelNames.sources.firstIndex { $0.value == patch }
        return HStack(spacing: 0) {
            HStack(spacing: 5) {
                SocketGlyph(kind: ChannelNames.outputPairSocket(out), size: 15)
                Text(ChannelNames.outputPairs[out]).font(.panel(12, .semibold)).foregroundColor(theme.text)
                    .lineLimit(1).fixedSize().copyable()
                Text(ChannelNames.outputPairTag(out)).font(.panel(7, .bold)).tracking(0.3)
                    .foregroundColor(theme.text.opacity(0.85)).lineLimit(1).fixedSize().copyable()
            }
            .padding(.leading, 6)
            .frame(width: Self.rowLabel, alignment: .leading)
            ForEach(ChannelNames.sources, id: \.value) { src in
                cell(output: out, source: src, chosen: known && patch == src.value)
            }
            Text(known && column != nil ? "plays \(ChannelNames.sourceName(patch))" : "")
                .font(.panel(11, .semibold)).foregroundColor(theme.text.opacity(0.85)).lineLimit(1).copyable()
                .padding(.leading, 8)
                .frame(width: Self.words, alignment: .leading)
        }
        .frame(height: Self.rowHeight)
        .background(RoundedRectangle(cornerRadius: 5).fill(out % 2 == 0 ? theme.panel : Color.clear))
    }

    private func cell(output: Int, source: ChannelNames.Source, chosen: Bool) -> some View {
        let flagged = model.disagreement.map { Int32($0.kind) == Int32(PANEL_F_PATCH) && $0.index == output && chosen } ?? false
        return Button(action: { model.setPatch(output: output, source: source.value) }) {
            ZStack {
                // A patch point is a diode: green and lit when chosen, dark otherwise.
                LED(lit: chosen, color: theme.signal, size: 11)
                Circle().strokeBorder(flagged ? theme.bad : Color.clear, lineWidth: 2).frame(width: 22, height: 22)
            }
            .frame(width: Self.cell, height: Self.rowHeight)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help("\(ChannelNames.outputPairs[output]) plays \(source.long)")
    }
}

// MARK: Load, Store, Reset

struct MemoryDialogView: View {
    enum Purpose { case load, store, reset }

    @Environment(\.theme) private var theme
    @ObservedObject var model: PanelModel
    let purpose: Purpose
    var close: () -> Void = {}
    var messages: MessageCenter?
    var openMessages: () -> Void = {}

    /// What Reset to Factory resets, as Roland's own dialog offers it: the settings in use
    /// ("Current Setting"), one of the five memories, or everything ("All Settings").
    enum ResetWhat { case current, memory, everything }

    @Kept private var memory: Int
    @Kept private var patchBay: Bool
    @Kept private var mixer: Bool
    @Kept private var resetWhat: ResetWhat

    init(model: PanelModel, purpose: Purpose, close: @escaping () -> Void = {}, memory: Int = 0,
         patchBay: Bool = true, mixer: Bool = true, reset: ResetWhat = .current,
         messages: MessageCenter? = nil, openMessages: @escaping () -> Void = {}) {
        self.model = model
        self.purpose = purpose
        self.close = close
        self.messages = messages
        self.openMessages = openMessages
        _memory = Kept(wrappedValue: memory)
        _patchBay = Kept(wrappedValue: patchBay)
        _mixer = Kept(wrappedValue: mixer)
        _resetWhat = Kept(wrappedValue: reset)
    }

    private var command: MemoryCommand {
        switch purpose {
        case .load: return MemoryCommand(kind: .load, memory: memory, patchBay: patchBay, mixer: mixer)
        case .store: return MemoryCommand(kind: .store, memory: memory, patchBay: patchBay, mixer: mixer)
        case .reset:
            switch resetWhat {
            case .current: return MemoryCommand(kind: .resetCurrent)
            case .memory: return MemoryCommand(kind: .resetMemory, memory: memory)
            case .everything: return MemoryCommand(kind: .resetAll)
            }
        }
    }

    private var title: String {
        switch purpose {
        case .load: return "Recall Memory"
        case .store: return "Save Memory"
        case .reset: return "Reset to Factory"
        }
    }

    private var subtitle: String {
        switch purpose {
        case .load: return "Replaces the settings in use with what one of the unit's five memories holds."
        case .store: return "Saves the settings in use into one of the unit's five memories."
        case .reset: return "Puts settings saved in the unit back to the way it was delivered."
        }
    }

    var body: some View {
        DialogFrame(title: title, subtitle: subtitle) {
            DialogStateLine(model: model, kind: .memory, messages: messages, openMessages: openMessages)
            if let pending = model.pendingMemory {
                confirmStep(pending)
            } else {
                chooseStep
            }
        }
        .frame(width: 470)
    }

    // Step 1: choose. Nothing can be sent from here.
    @ViewBuilder private var chooseStep: some View {
        let ready = model.snap.state == .ready && model.memoryBlocker(command) == nil
        VStack(alignment: .leading, spacing: 12) {
            if purpose == .reset {
                VStack(alignment: .leading, spacing: 6) {
                    Text("WHAT TO RESET").font(.panel(9, .bold)).foregroundColor(theme.dim).copyable()
                    choice("The settings in use (Current Setting); the memories stay", chosen: resetWhat == .current) { resetWhat = .current }
                    choice("One memory; the settings in use stay", chosen: resetWhat == .memory) { resetWhat = .memory }
                    choice("Everything: the settings in use and all five memories (All Settings)", chosen: resetWhat == .everything) { resetWhat = .everything }
                }
            }
            if purpose != .reset || resetWhat == .memory {
                VStack(alignment: .leading, spacing: 6) {
                    Text("MEMORY").font(.panel(9, .bold)).foregroundColor(theme.dim).copyable()
                    HStack(spacing: 6) {
                        ForEach(0..<5, id: \.self) { m in
                            Button(action: { memory = m }) {
                                Text("\(m + 1)").font(.readout(14, .bold))
                                    .foregroundColor(memory == m ? theme.onText : theme.text)
                                    .frame(width: 44, height: 34)
                                    .keycap(on: memory == m, color: theme.link, radius: 7)
                            }.buttonStyle(.plain)
                        }
                    }
                }
            }
            if purpose != .reset {
                VStack(alignment: .leading, spacing: 6) {
                    Text(purpose == .load ? "WHAT TO LOAD" : "WHAT TO STORE").font(.panel(9, .bold)).foregroundColor(theme.dim).copyable()
                    check("Patch bay", on: patchBay) { patchBay.toggle() }
                    check("Direct monitor mixer (mute, pan, volume, link)", on: mixer) { mixer.toggle() }
                }
            }
            // What a Recall, Save or Reset did is in the Messages window (2026-11-16), so the
            // buttons below never move.
            HStack {
                // Why it can't go on right now, in full, where the eye is; else what comes next.
                Text(model.memoryBlocker(command) ?? "Nothing is sent yet. The next step shows exactly what will happen.")
                    .font(.panel(11)).foregroundColor(model.memoryBlocker(command) == nil ? theme.dim : theme.warn)
                    .fixedSize(horizontal: false, vertical: true).copyable()
                Spacer()
                Button("Close", action: close).buttonStyle(PanelButtonStyle()).keyboardShortcut(.cancelAction)
                Button("Continue…") { model.prepareMemory(command) }
                    .buttonStyle(PanelButtonStyle(prominent: true))
                    .disabled(!ready || !command.isValid)
            }
        }
    }

    // Step 2: confirm. One click sends the one message shown.
    private func confirmStep(_ pending: PendingMemory) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 8) {
                    Text("⚠︎").font(.panel(16, .bold)).foregroundColor(theme.bad)
                    Text("Warning: " + pending.command.title + "?").font(.panel(14, .bold)).foregroundColor(theme.text).copyable()
                }
                // What will be overwritten on the UA-101; that it can't be undone; what is
                // saved first (a design choice of 2026-11-21).
                Text(pending.command.consequence).font(.panel(12)).foregroundColor(theme.text)
                    .fixedSize(horizontal: false, vertical: true).copyable()
                Text(MemoryCommand.cantBeUndone).font(.panel(13, .bold)).foregroundColor(theme.bad)
                    .fixedSize(horizontal: false, vertical: true).copyable()
                Text(pending.command.backupWords)
                    .font(.panel(12, .semibold)).foregroundColor(theme.bad).fixedSize(horizontal: false, vertical: true).copyable()
            }
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 8).fill(theme.bad.opacity(0.10)))
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(theme.bad.opacity(0.5), lineWidth: 1))

            #if !UA101_USER
            VStack(alignment: .leading, spacing: 3) {
                Text("THE ONE MESSAGE THAT WILL BE SENT").font(.panel(9, .bold)).foregroundColor(theme.dim).copyable()
                Text(Format.hex(pending.bytes)).font(.system(size: 11, design: .monospaced)).foregroundColor(theme.text)
                    .textSelection(.enabled)
            }
            #endif
            HStack {
                Text(model.memoryBlocker(pending.command) ?? "Waits \(Int(panel_ticket_seconds(model))) seconds for your answer, then gives up.")
                    .font(.panel(11)).foregroundColor(model.memoryBlocker(pending.command) == nil ? theme.dim : theme.warn)
                    .fixedSize(horizontal: false, vertical: true).copyable()
                Spacer()
                Button("Cancel") { model.cancelMemory() }.buttonStyle(PanelButtonStyle()).keyboardShortcut(.cancelAction)
                Button(pending.command.title) { model.confirmMemory() }
                    .buttonStyle(PanelButtonStyle(prominent: true, color: theme.bad))
                    .disabled(model.memoryBlocker(pending.command) != nil)
            }
        }
    }

    private func panel_ticket_seconds(_ model: PanelModel) -> Double {
        guard let p = model.pendingMemory else { return 0 }
        let now = model.scheduler.now
        return p.expires > now ? (Double(p.expires - now) / 1_000_000).rounded() : 0
    }

    private func choice(_ label: String, chosen: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 8) {
                ZStack {
                    Circle().stroke(theme.line, lineWidth: 1).background(Circle().fill(theme.track)).frame(width: 15, height: 15)
                    if chosen { Circle().fill(theme.link).frame(width: 8, height: 8) }
                }
                Text(label).font(.panel(12)).foregroundColor(theme.text)
            }.contentShape(Rectangle())
        }.buttonStyle(.plain)
    }

    private func check(_ label: String, on: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 8) {
                ZStack {
                    RoundedRectangle(cornerRadius: 3).fill(on ? theme.link : theme.track).frame(width: 15, height: 15)
                    RoundedRectangle(cornerRadius: 3).stroke(theme.line, lineWidth: 1).frame(width: 15, height: 15)
                    if on { Text("✓").font(.system(size: 10, weight: .bold)).foregroundColor(theme.onText) }
                }
                Text(label).font(.panel(12)).foregroundColor(theme.text)
            }.contentShape(Rectangle())
        }.buttonStyle(.plain)
    }
}

// MARK: driver settings

/// A slider with a few fixed places, drawn by hand so it looks the same in the picture and in
/// the app. A click or a drag picks the nearest place. Under each place there may be its value
/// and a short note, in the ordinary colours (2026-11-20: no red, no amber).
struct StepSlider: View {
    @Environment(\.theme) private var theme
    let count: Int
    let index: Int
    /// Under each place: its value ("8 ms"), and under that a short note ("buffer 128+").
    var labels: [String] = []
    var notes: [String] = []
    var enabled = true
    /// How wide it is drawn, and how far the first and last places lie from its ends.
    var width: CGFloat = StepSlider.width
    var inset: CGFloat = 7
    var pick: (Int) -> Void = { _ in }

    static let width: CGFloat = 170

    /// The place a point at `x` along the slider belongs to.
    static func place(at x: CGFloat, count: Int, width: CGFloat = StepSlider.width, inset: CGFloat = 7) -> Int {
        guard count > 1 else { return 0 }
        let t = (x - inset) / (width - 2 * inset)
        return min(count - 1, max(0, Int((t * CGFloat(count - 1)).rounded())))
    }

    /// Where place `i` lies along the slider.
    private func x(_ i: Int) -> CGFloat { inset + CGFloat(i) / CGFloat(max(1, count - 1)) * (width - 2 * inset) }

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            track
            if !labels.isEmpty || !notes.isEmpty { words }
        }
    }

    /// The value and the note under each place, centred on it; the chosen place's in full colour.
    private var words: some View {
        let room = count > 1 ? (width - 2 * inset) / CGFloat(count - 1) : width
        let w = min(room, 2 * inset + 6, 72)
        return ZStack(alignment: .topLeading) {
            ForEach(0..<count, id: \.self) { i in
                VStack(spacing: 1) {
                    if i < labels.count {
                        Text(labels[i]).font(.readout(9.5, i == index ? .bold : .regular))
                            .foregroundColor(i == index ? theme.text : theme.dim)
                    }
                    if i < notes.count {
                        Text(notes[i]).font(.panel(8, i == index ? .semibold : .regular))
                            .foregroundColor(i == index ? theme.text : theme.dim)
                    }
                }
                .lineLimit(1).fixedSize()
                .frame(width: w)
                .copyable()
                .offset(x: x(i) - w / 2)
            }
        }
        .frame(width: width, alignment: .topLeading)
        .opacity(enabled ? 1 : 0.45)
    }

    private var track: some View {
        ZStack(alignment: .leading) {
            Capsule().fill(theme.track).frame(width: width, height: 5)
                .overlay(Capsule().strokeBorder(theme.edgeDark.opacity(0.5), lineWidth: 0.5))
            ForEach(0..<count, id: \.self) { i in
                Rectangle().fill(theme.line).frame(width: 1, height: 8).offset(x: x(i) - 0.5)
            }
            Circle().fill(LinearGradient(colors: [theme.capTop, theme.capBottom], startPoint: .top, endPoint: .bottom))
                .overlay(Circle().strokeBorder(theme.edgeDark.opacity(0.5), lineWidth: 0.75))
                .frame(width: 14, height: 14).offset(x: x(index) - 7)
        }
        .frame(width: width, height: 20)
        .contentShape(Rectangle())
        .opacity(enabled ? 1 : 0.45)
        .gesture(DragGesture(minimumDistance: 0).onChanged { g in
            guard enabled else { return }
            let i = Self.place(at: g.location.x, count: count, width: width, inset: inset)
            if i != index { pick(i) }
        })
    }
}

/// The driver's own settings: the two delays, rate correction, and what the driver measured.
/// Nothing here goes to the unit. A change is sent only by Apply. Since 2026-11-20 (the user's
/// wish) a window of its own, with no red or amber on the sliders: every value has a plain note.
/// Measured keeps its green, amber and red.
struct DriverSettingsView: View {
    @Environment(\.theme) private var theme
    @ObservedObject var model: DriverSettingsModel
    var close: () -> Void = {}

    private static let clock = Timer.publish(every: 1, on: .main, in: .common).autoconnect()
    /// The window's width, and what a setting's slider gets of it.
    static let width: CGFloat = 540
    static let sliderWidth: CGFloat = width - 2 * 18 - 2 * 12

    var body: some View {
        let s = model.state
        let rate = model.noteRate
        DialogFrame(title: "Driver Settings",
                    subtitle: "How the driver trades delay against safety from clicks. Nothing here changes the unit.") {
            banner(s)
            setting("Playback delay",
                    "How early the Mac hands sound to the driver. Shorter: software instruments answer faster. Longer: more room when the Mac is busy. "
                        + "Under each value: the music app's buffer it needs.",
                    value: DriverValues.label(DriverValues.output[model.outputIndex]),
                    sentence: model.sentence(playback: true)) {
                StepSlider(count: DriverValues.output.count, index: model.outputIndex,
                           labels: DriverValues.output.map { DriverValues.number($0) },
                           notes: DriverValues.output.indices.map { DriverSettingsModel.tickNote(playback: true, index: $0, rate: rate) },
                           enabled: model.usable && !model.busy, width: Self.sliderWidth, inset: 28) {
                    model.choose(output: $0)
                }
            }
            setting("Recording margin",
                    "How far behind the unit recording is read. Shorter: less delay when you listen through the Mac. Longer: safer against gaps. "
                        + "The music app's buffer doesn't change it.",
                    value: DriverValues.label(DriverValues.input[model.inputIndex]),
                    sentence: model.sentence(playback: false)) {
                StepSlider(count: DriverValues.input.count, index: model.inputIndex,
                           labels: DriverValues.input.map { DriverValues.number($0) },
                           notes: DriverValues.input.indices.map { DriverSettingsModel.tickNote(playback: false, index: $0, rate: rate) },
                           enabled: model.usable && !model.busy, width: Self.sliderWidth, inset: 28) {
                    model.choose(input: $0)
                }
            }
            row("Rate correction", "Keeps the unit's own clock in step with the Mac. Off passes the samples through untouched.") {
                Button(action: { model.choose(correction: !model.correction) }) {
                    ZStack(alignment: model.correction ? .trailing : .leading) {
                        Capsule().fill(model.correction ? theme.on : theme.track).frame(width: 38, height: 21)
                        Circle().fill(theme.cap).frame(width: 17, height: 17).padding(.horizontal, 2)
                    }
                }
                .buttonStyle(.plain)
                .disabled(!model.usable || model.busy || s.correction == nil)
                .opacity(model.usable && s.correction != nil ? 1 : 0.45)
            }
            measured
            Text("The buffer size is not set here: each music app has its own (Logic Pro: Settings → Audio → I/O Buffer Size).")
                .font(.panel(11)).foregroundColor(theme.dim).fixedSize(horizontal: false, vertical: true).copyable()
            HStack(spacing: 8) {
                Text(footer).font(.panel(11)).foregroundColor(footerColor)
                    .fixedSize(horizontal: false, vertical: true).copyable()
                Spacer(minLength: 8)
                Button("Defaults") { model.chooseDefaults() }.buttonStyle(PanelButtonStyle())
                    .disabled(!model.usable || model.busy || model.atDefaults)
                Button("Close", action: close).buttonStyle(PanelButtonStyle()).keyboardShortcut(.cancelAction)
                Button("Apply") { model.apply() }.buttonStyle(PanelButtonStyle(prominent: true))
                    .disabled(!model.changed || model.busy).keyboardShortcut(.defaultAction)
            }
        }
        .frame(width: Self.width)
        .onReceive(Self.clock) { _ in model.refresh() }
    }

    /// The one line at the bottom: what Apply will do, or what it did.
    private var footer: String {
        if let note = model.note { return note.text }
        if model.busy { return "Sending…" }
        if model.changed { return "Not applied yet. Apply restarts the sound: it stops for a moment." }
        return "A change applies at once: the sound stops for a moment and carries on."
    }

    /// Green when applied; otherwise the ordinary colours, never red or amber (2026-11-20).
    private var footerColor: Color {
        switch model.note?.tone {
        case .good?: return theme.signal
        case .bad?, .warn?: return theme.text
        default: return model.changed ? theme.text : theme.dim
        }
    }

    @ViewBuilder private func banner(_ s: DriverState) -> some View {
        if model.source.isPretend {
            Banner(symbol: "i", color: theme.link, title: "Pretend unit",
                   text: "These are pretend settings: nothing reaches the driver. Choose UA-101 in the Device menu for the real ones.")
                .copyable()
        } else if !s.found {
            Banner(symbol: "i", color: theme.link, title: "The UA-101 isn't there",
                   text: "The settings live in the driver, which shows the UA-101 only while the unit is switched on. Switch it on to see and change them.")
                .copyable()
        } else if !s.haveSettings {
            Banner(symbol: "i", color: theme.link, title: "An older driver",
                   text: "The installed driver has no settings to change. Install the driver that came with this app.")
                .copyable()
        }
    }

    /// What the driver measured in the run that is on, or the last one.
    private var measured: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Text("Measured").font(.panel(13, .semibold)).foregroundColor(theme.text).copyable()
                Text(model.runLine).font(.panel(11)).foregroundColor(theme.dim)
                    .fixedSize(horizontal: false, vertical: true).copyable()
                Spacer(minLength: 0)
            }
            figure("Playback", model.room(playback: true))
            figure("Recording", model.room(playback: false))
            Text("Room is what was left of a delay at the busiest moment. With less than 1 ms, or with sound lost, choose a longer delay.")
                .font(.panel(11)).foregroundColor(theme.dim).fixedSize(horizontal: false, vertical: true).copyable()
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .raised(10)
    }

    /// One direction's figure: green with room to spare, amber under 1 ms of room, red when
    /// sound was lost (the user kept these colours in Measured, 2026-11-20; the red and
    /// amber went from the sliders only).
    private func figure(_ name: String, _ note: DriverSettingsModel.Note) -> some View {
        HStack(spacing: 8) {
            Circle().fill(color(note.tone)).frame(width: 7, height: 7)
            Text(name).font(.panel(12, .semibold)).foregroundColor(theme.text).frame(width: 76, alignment: .leading).copyable()
            Text(note.text).font(.readout(11)).foregroundColor(note.tone == .plain ? theme.dim : color(note.tone)).copyable()
            Spacer(minLength: 0)
        }
    }

    private func color(_ tone: DriverSettingsModel.Tone) -> Color {
        switch tone {
        case .plain: return theme.lampOff
        case .good: return theme.signal
        case .warn: return theme.warn
        case .bad: return theme.bad
        }
    }

    /// One delay: its name and what it does, the value chosen, the slider with a note under each
    /// place, and a sentence about the value chosen. The sentence always has room for two lines,
    /// so nothing moves as the slider does.
    private func setting<C: View>(_ title: String, _ text: String, value: String, sentence: String,
                                  @ViewBuilder control: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline, spacing: 14) {
                VStack(alignment: .leading, spacing: 3) {
                    Text(title).font(.panel(13, .semibold)).foregroundColor(theme.text).copyable()
                    Text(text).font(.panel(11)).foregroundColor(theme.dim).fixedSize(horizontal: false, vertical: true).copyable()
                }
                Spacer(minLength: 8)
                Text(value).font(.readout(13, .semibold)).foregroundColor(theme.text).fixedSize().copyable()
            }
            control()
            Text(sentence).font(.panel(11.5)).foregroundColor(theme.text)
                .lineLimit(2)
                .frame(maxWidth: .infinity, minHeight: 30, maxHeight: 30, alignment: .topLeading).copyable()
        }
        .padding(12)
        .raised(10)
    }

    /// A setting with a switch beside its words.
    private func row<C: View>(_ title: String, _ text: String, @ViewBuilder control: () -> C) -> some View {
        HStack(alignment: .center, spacing: 14) {
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.panel(13, .semibold)).foregroundColor(theme.text).copyable()
                Text(text).font(.panel(11)).foregroundColor(theme.dim).fixedSize(horizontal: false, vertical: true).copyable()
            }
            Spacer(minLength: 8)
            control()
        }
        .padding(12)
        .raised(10)
    }
}

// MARK: the pretend unit's front panel
#if !UA101_USER || UA101_PICTURES

/// Lets the user press the pretend unit's switches and make it misbehave,
/// to see how the panel answers. It never exists for a real unit.
struct PretendFrontPanelView: View {
    @Environment(\.theme) private var theme
    @ObservedObject var model: PanelModel
    let unit: PretendUnit
    @Kept private var redraw: Int

    init(model: PanelModel, unit: PretendUnit) {
        self.model = model
        self.unit = unit
        _redraw = Kept(wrappedValue: 0)
    }

    var body: some View {
        DialogFrame(title: "Pretend unit", subtitle: "A stand-in for the UA-101's front panel. Nothing here reaches a real unit.") {
            VStack(alignment: .leading, spacing: 10) {
                Text("FRONT PANEL").font(.panel(9, .bold)).foregroundColor(theme.dim).copyable()
                HStack(spacing: 8) {
                    toggle("SOFT CTRL", unit.softControl) { unit.press(Int32(PANEL_SWITCH_SOFT), in: !unit.softControl) }
                    toggle("MONO", unit.mono) { unit.press(Int32(PANEL_SWITCH_MONO), in: !unit.mono) }
                    toggle("DIGITAL IN", unit.digital) { unit.press(Int32(PANEL_SWITCH_DIGITAL), in: !unit.digital) }
                }
                Text("SAMPLE RATE (switches the pretend unit off and on)").font(.panel(9, .bold)).foregroundColor(theme.dim).copyable()
                HStack(spacing: 6) {
                    ForEach(Array(["44.1", "48", "88.2", "96", "192"].enumerated()), id: \.offset) { code, label in
                        toggle(label, unit.rateCode == code) {
                            unit.setRate(code: code)
                            // Only the pretend unit is "switched off and on" from here.
                            if model.transport === unit { model.reconnect() }
                        }
                    }
                }
                Text("MISBEHAVE").font(.panel(9, .bold)).foregroundColor(theme.dim).copyable()
                HStack(spacing: 8) {
                    toggle("Ignore volume changes", unit.ignoredKind == Int(PANEL_F_VOLUME)) {
                        unit.ignore(kind: unit.ignoredKind == Int(PANEL_F_VOLUME) ? -1 : Int(PANEL_F_VOLUME))
                    }
                    toggle("Stop answering", unit.silent) { model.setPretendSilent(unit, !unit.silent) }
                }
                Text("CHANGES (pretend unit only)").font(.panel(9, .bold)).foregroundColor(theme.dim).copyable()
                HStack(spacing: 8) {
                    toggle("Allow changes", model.snap.writesAllowed) { model.allowChanges(!model.snap.writesAllowed) }
                }
            }
        }
        .fixedSize()
        .id(redraw)
        .onAppear { unit.onFrontPanelChange = { redraw += 1 } }
    }

    private func toggle(_ label: String, _ on: Bool, action: @escaping () -> Void) -> some View {
        Button(action: {
            action()
            redraw += 1
        }) {
            Text(label).font(.panel(11, .semibold)).foregroundColor(on ? theme.onText : theme.text)
                .padding(.horizontal, 10).padding(.vertical, 6)
                .keycap(on: on, color: theme.on, radius: 13)
        }.buttonStyle(.plain)
    }
}
#endif
