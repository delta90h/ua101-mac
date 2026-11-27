// The main window: the direct monitor mixer with the device panel under it.
import SwiftUI

/// Which dialog is open, and the two menu options. Not the unit's state.
final class PanelUI: ObservableObject {
    /// The dialogs that are sheets on the mixer. Patch Bay and Driver Settings are sheets as
    /// before unless the View menu puts them in a window of their own (DialogWindows.swift).
    enum Sheet: String, Identifiable {
        case load, store, reset, patchBay, driverSettings, putBack, intoMemory
        var id: String { rawValue }
    }
    @Published var sheet: Sheet?
    /// View menu (a design choice, 2026-11-20): Patch Bay and Driver Settings each in a
    /// window of their own (checked) or a sheet on the mixer as before (unchecked, the default).
    /// Kept for the next start.
    @Published var patchBayOwnWindow = false
    @Published var driverSettingsOwnWindow = false
    static let patchBayOwnWindowKey = "patchBayOwnWindow", driverSettingsOwnWindowKey = "driverSettingsOwnWindow"
    /// The kept choice: a sheet unless the user checked the View menu's item.
    static func ownWindow(_ key: String, in d: UserDefaults) -> Bool { d.bool(forKey: key) }
    @Published var alwaysOnTop = false
    /// Open next time at the zoom and place the window had (app menu, Remember Size). On by default.
    @Published var rememberSize = true
    /// False until the window has taken its first zoom and place: nothing is saved before,
    /// so the size the app opens with can't overwrite the one it remembers.
    var sizeSettled = false
    /// Dark, light, or whatever macOS is set to (System Settings, Appearance).
    @Published var look = Look.system
    @Published var askResetMixer = false
    /// Moving pretend levels on the meters, to judge the look. Off as the app opens.
    @Published var pretendLevels = false
    /// How large the mixer is drawn right now: 1 is its own size. Follows the window.
    @Published var zoom: CGFloat = 1
    /// The mixer's own size, as last measured.
    var naturalSize: CGSize = .zero
    /// Where each fader's and each pan knob's area lies in the mixer's own coordinates
    /// (`MixerWindowView.space`), as last laid out. For the self-test, which presses and
    /// drags them with real mouse events; nothing in the app reads it.
    var faderFrames: [StripID: CGRect] = [:]
    var knobFrames: [StripID: CGRect] = [:]
    /// Where each strip's name lies (the self-test clicks it: its words can be selected, and a
    /// click on it still chooses the strip).
    var nameFrames: [StripID: CGRect] = [:]
    /// Opens the Messages window (the mixer's Messages button, the View menu).
    var showMessages: () -> Void = {}
    /// Open Patch Bay and Driver Settings: as a sheet or in their own window (View menu).
    var showPatchBay: () -> Void = {}
    var showDriverSettings: () -> Void = {}
    /// The Messages window's text size, 1 = its own (A− and A+, the View menu). Kept for the next start.
    @Published var messagesScale: CGFloat = 1
    static let messagesScaleKey = "messagesScale"
    static let messagesScales: [CGFloat] = [0.8, 0.9, 1, 1.15, 1.3, 1.5, 1.75, 2]

    static func nextMessagesScale(from k: CGFloat, up: Bool) -> CGFloat {
        let steps = messagesScales
        return up ? (steps.first { $0 > k + 0.01 } ?? steps.last!) : (steps.last { $0 < k - 0.01 } ?? steps.first!)
    }

    func stepMessagesScale(up: Bool) {
        messagesScale = Self.nextMessagesScale(from: messagesScale, up: up)
        UserDefaults.standard.set(Double(messagesScale), forKey: Self.messagesScaleKey)
    }
}

/// How a control follows the mouse. The place of the mouse is taken in the mixer's own
/// coordinates, never in the control's: when a banner above the mixer grows or shrinks, the
/// controls move on the screen, and a place measured from the control would change though
/// the hand didn't. That made INPUT 1's fader jump 7 dB at its first move on 2026-11-12
/// (the press replaced a two-line banner by a one-line one;).
struct DragTrack {
    /// The control's value where the drag began (a fader's position 0 to 1, a pan's number).
    let start: Double
    /// A stop on the way up: a drag that began below it goes no higher (a fader's 0 dB, the
    /// user's choice of 2026-11-27). Let go and push again from there to go past it.
    let stop: Double?
    private(set) var moved = false

    init(start: Double, stopAt: Double? = nil) {
        self.start = start
        self.stop = stopAt.flatMap { start < $0 ? $0 : nil }
    }

    /// The value for a hand that is `travel` (points; up or right is more) from where it
    /// pressed, when `full` points are the control's whole range `range`. Nil while the hand
    /// hasn't moved since the press: a press alone asks for nothing.
    mutating func value(travel: CGFloat, full: CGFloat, range: Double) -> Double? {
        if !moved && travel == 0 { return nil }
        moved = true
        return min(stop ?? range, range, max(0, start + Double(travel / full) * range))
    }
}

private struct ControlFramesKey: PreferenceKey {
    static var defaultValue: [StripID: CGRect] = [:]
    static func reduce(value: inout [StripID: CGRect], nextValue: () -> [StripID: CGRect]) {
        value.merge(nextValue()) { _, new in new }
    }
}
private struct NameFramesKey: PreferenceKey {
    static var defaultValue: [StripID: CGRect] = [:]
    static func reduce(value: inout [StripID: CGRect], nextValue: () -> [StripID: CGRect]) {
        value.merge(nextValue()) { _, new in new }
    }
}
private struct FaderFramesKey: PreferenceKey {
    static var defaultValue: [StripID: CGRect] = [:]
    static func reduce(value: inout [StripID: CGRect], nextValue: () -> [StripID: CGRect]) {
        value.merge(nextValue()) { _, new in new }
    }
}

// MARK: one strip

/// A jack socket, as on the back of the unit: a metal nut around a dark hole.
/// The hole's rim lights when the channel has signal.
struct JackGlyph: View {
    @Environment(\.theme) private var theme
    var lit = false
    var size: CGFloat = 17

    var body: some View {
        ZStack {
            Circle().fill(LinearGradient(colors: [theme.capTop, theme.capBottom], startPoint: .top, endPoint: .bottom))
            Circle().strokeBorder(theme.edgeDark.opacity(0.55), lineWidth: 0.75)
            Circle().fill(LinearGradient(colors: [theme.knobBottom, theme.knobTop], startPoint: .top, endPoint: .bottom))
                .frame(width: size * 0.66, height: size * 0.66)
            Circle().fill(theme.track).frame(width: size * 0.40, height: size * 0.40)
            Circle().strokeBorder(lit ? theme.signal : Color.clear, lineWidth: 1.5)
                .frame(width: size * 0.56, height: size * 0.56)
        }
        .frame(width: size, height: size)
    }
}

/// A combo socket, as on the unit's front for the two mic preamps: an XLR's
/// three pin holes around a 1/4-inch hole, in a metal ring.
struct ComboGlyph: View {
    @Environment(\.theme) private var theme
    var lit = false
    var size: CGFloat = 20

    var body: some View {
        ZStack {
            Circle().fill(LinearGradient(colors: [theme.capTop, theme.capBottom], startPoint: .top, endPoint: .bottom))
            Circle().strokeBorder(theme.edgeDark.opacity(0.55), lineWidth: 0.75)
            Circle().fill(LinearGradient(colors: [theme.knobBottom, theme.knobTop], startPoint: .top, endPoint: .bottom))
                .frame(width: size * 0.82, height: size * 0.82)
            // The three pin holes.
            ForEach(0..<3, id: \.self) { i in
                let a = (Double(i) * 120 - 90 + 180) * Double.pi / 180
                Circle().fill(theme.track).frame(width: size * 0.13, height: size * 0.13)
                    .offset(x: CGFloat(cos(a)) * size * 0.27, y: CGFloat(sin(a)) * size * 0.27)
            }
            // The 1/4-inch hole in the middle.
            Circle().fill(LinearGradient(colors: [theme.capBottom, theme.capTop], startPoint: .top, endPoint: .bottom))
                .frame(width: size * 0.34, height: size * 0.34)
            Circle().fill(theme.track).frame(width: size * 0.22, height: size * 0.22)
            Circle().strokeBorder(lit ? theme.signal : Color.clear, lineWidth: 1.5)
                .frame(width: size * 0.72, height: size * 0.72)
        }
        .frame(width: size, height: size)
    }
}

/// The opening of an optical connector: a square with its two top corners cut.
struct OpticalPort: Shape {
    func path(in r: CGRect) -> Path {
        let c = r.width * 0.28
        var p = Path()
        p.move(to: CGPoint(x: r.minX + c, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX - c, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX, y: r.minY + c))
        p.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX, y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX, y: r.minY + c))
        p.closeSubpath()
        return p
    }
}

/// An optical socket: a square metal frame around the opening. An output shines
/// its red light; an input is a receiver, so its red is only a dark glint.
struct OpticalGlyph: View {
    @Environment(\.theme) private var theme
    var lit = false
    var shines = true
    var size: CGFloat = 20

    var body: some View {
        let frame = size * 0.92, port = size * 0.58
        ZStack {
            RoundedRectangle(cornerRadius: size * 0.16)
                .fill(LinearGradient(colors: [theme.capTop, theme.capBottom], startPoint: .top, endPoint: .bottom))
                .frame(width: frame, height: frame)
            RoundedRectangle(cornerRadius: size * 0.16).strokeBorder(theme.edgeDark.opacity(0.55), lineWidth: 0.75)
                .frame(width: frame, height: frame)
            OpticalPort().fill(theme.track).frame(width: port, height: port)
            if shines {
                Circle().fill(RadialGradient(colors: [theme.optical.opacity(0.55), theme.optical.opacity(0)],
                                             center: .center, startRadius: size * 0.08, endRadius: size * 0.27))
                    .frame(width: size * 0.54, height: size * 0.54)
                Circle().fill(RadialGradient(colors: [Color.white.opacity(0.9), theme.optical], center: .center,
                                             startRadius: 0, endRadius: size * 0.10))
                    .frame(width: size * 0.22, height: size * 0.22)
            } else {
                Circle().fill(theme.optical.opacity(0.55)).frame(width: size * 0.18, height: size * 0.18)
            }
            OpticalPort().stroke(lit ? theme.signal : Color.clear, lineWidth: 1.5).frame(width: port, height: port)
        }
        .frame(width: size, height: size)
    }
}

/// The drawing for a channel's socket, by its kind in the names table.
struct SocketGlyph: View {
    let kind: ChannelNames.Socket
    var lit = false
    /// An optical output shines; an optical input doesn't.
    var shines = true
    var size: CGFloat = 20

    var body: some View {
        ZStack {
            switch kind {
            case .combo: ComboGlyph(lit: lit, size: size)
            case .optical: OpticalGlyph(lit: lit, shines: shines, size: size)
            case .jack: JackGlyph(lit: lit, size: size * 0.85)
            case .none: Color.clear
            }
        }
        .frame(width: size, height: size)
    }
}

/// A turning knob drawn like a real one: a dark rounded body in a metal rim, lit from above,
/// with a soft shadow on the panel, a white pointer, and a thin arc of light from the middle
/// to where it points.
struct KnobFace: View {
    @Environment(\.theme) private var theme
    @Environment(\.section) private var section
    /// -1 hard left, 0 middle, 1 hard right.
    let turn: Double
    var flagged = false
    var size: CGFloat = Metrics.knob

    var body: some View {
        let t = 0.375 + turn * 0.375 // along a 270 degree sweep that starts bottom-left
        let body = size - 9
        let arc = section?.arc ?? theme.glow
        ZStack {
            Circle().trim(from: 0, to: 0.75)
                .stroke(theme.track, style: StrokeStyle(lineWidth: 2, lineCap: .round))
                .rotationEffect(.degrees(135))
            Circle().trim(from: min(0.375, t), to: max(0.375, t))
                .stroke(flagged ? theme.bad : arc, style: StrokeStyle(lineWidth: 2, lineCap: .round))
                .rotationEffect(.degrees(135))
            SoftShadow(shape: Circle(), depth: 2.5, opacity: 0.55, steps: 3).frame(width: body, height: body)
            // The rim: bright metal, lit at the top.
            Circle().fill(LinearGradient(colors: [theme.capTop, theme.capBottom], startPoint: .top, endPoint: .bottom))
                .frame(width: body, height: body)
            // The body, rounded: lit at its upper left.
            Circle().fill(RadialGradient(colors: [Color(hex: 0x4c535d), Color(hex: 0x16191e), Color(hex: 0x0a0b0d)],
                                         center: UnitPoint(x: 0.38, y: 0.30), startRadius: 0, endRadius: body * 0.62))
                .frame(width: body - 3, height: body - 3)
            // Its edge: light along the top, dark along the bottom.
            Circle().strokeBorder(LinearGradient(colors: [Color.white.opacity(0.45), Color.black.opacity(0.6)],
                                                 startPoint: .top, endPoint: .bottom), lineWidth: 0.75)
                .frame(width: body - 3, height: body - 3)
            Capsule().fill(Color.white).frame(width: 2, height: body / 2 - 4)
                .offset(y: -(body / 4) - 0.5)
                .rotationEffect(.degrees(turn * 135))
        }
        .padding(1)
        .frame(width: size, height: size)
    }
}

struct PanKnob: View {
    @ObservedObject var model: PanelModel
    let id: StripID
    let pan: UInt32
    let flagged: Bool
    @Kept private var track: DragTrack?
    /// Points of travel from one end of the pan to the other.
    static let travel: CGFloat = 120

    var body: some View {
        let turn = (Double(pan) - Double(PANEL_CENTER_PAN)) / Double(PANEL_CENTER_PAN) // -1 to 1
        KnobFace(turn: turn, flagged: flagged)
        .contentShape(Rectangle())
        .background(GeometryReader { g in
            Color.clear.preference(key: ControlFramesKey.self, value: [id: g.frame(in: .named(MixerWindowView.space))])
        })
        .gesture(DragGesture(minimumDistance: 0, coordinateSpace: .named(MixerWindowView.space))
            .onChanged { g in
                if track == nil {
                    track = DragTrack(start: Double(pan))
                    model.cursor = id
                    model.beginDrag(panOf: id)
                }
                var t = track!
                if let p = t.value(travel: g.translation.width, full: Self.travel, range: Double(PANEL_MAX_PAN)) {
                    model.setPan(id, UInt32(p.rounded()))
                }
                track = t
            }
            .onEnded { _ in
                track = nil
                model.endDrag()
            })
        .help("Pan: drag left or right")
    }
}

/// The fader's cap, in its section's material: ridges for the finger, a highlight on the top
/// face, a line across the middle, and a soft shadow on the panel below it.
struct FaderCap: View {
    @Environment(\.theme) private var theme
    @Environment(\.section) private var section
    let flagged: Bool
    static let width: CGFloat = 20, height: CGFloat = 24

    var body: some View {
        let cap = (section ?? theme.section(ChannelNames.groupOutput)).cap
        let shape = RoundedRectangle(cornerRadius: 3.5)
        ZStack {
            // Every part of the cap is opaque, so nothing behind it (the slot, its light, the
            // scale) shows through; only its shadow lies beyond it.
            SoftShadow(shape: shape, depth: 3.5, opacity: 0.6, steps: 4)
            shape.fill(LinearGradient(colors: [cap.top, cap.bottom], startPoint: .top, endPoint: .bottom))
            // The top face catches the light; the lower half curves away from it.
            shape.fill(LinearGradient(stops: [
                .init(color: cap.shine, location: 0), .init(color: cap.shine.opacity(0), location: 0.42),
                .init(color: Color.white.opacity(0), location: 0.58), .init(color: cap.shine.opacity(0.35), location: 0.62),
                .init(color: Color.white.opacity(0), location: 0.9),
            ], startPoint: .top, endPoint: .bottom))
            // The ridges: each a dark groove with a lit lip under it.
            VStack(spacing: 0) {
                ForEach(0..<7, id: \.self) { i in
                    if i == 3 {
                        Rectangle().fill(cap.mark).frame(width: Self.width - 2, height: 1.5)
                            .padding(.vertical, 0.75)
                    } else {
                        VStack(spacing: 0) {
                            Rectangle().fill(cap.ridgeDark).frame(height: 1)
                            Rectangle().fill(cap.ridgeLight).frame(height: 0.75)
                        }
                        .frame(width: Self.width - 6)
                        .padding(.vertical, 0.6)
                    }
                }
            }
            shape.strokeBorder(flagged ? AnyShapeStyle(theme.bad)
                               : AnyShapeStyle(LinearGradient(colors: [Color.white.opacity(0.55), cap.edge],
                                                              startPoint: .top, endPoint: .bottom)),
                               lineWidth: flagged ? 2 : 0.75)
        }
        .frame(width: Self.width, height: Self.height)
    }
}

struct Fader: View {
    @Environment(\.theme) private var theme
    @Environment(\.section) private var section
    @ObservedObject var model: PanelModel
    let id: StripID
    let volume: UInt32
    let ceiling: UInt32
    let flagged: Bool
    @Kept private var track: DragTrack?

    private static let slotWidth: CGFloat = 6
    private static let slotCentre: CGFloat = 29 // from the strip's left edge; the scale is left of it, the meter right
    private static let marks: [(dB: Double, label: String)] = [
        (12, "12"), (6, "6"), (0, "0"), (-6, "6"), (-12, "12"), (-20, "20"), (-30, "30"), (-40, "40"),
    ]

    var body: some View {
        let h = Metrics.faderHeight
        let half = FaderCap.height / 2
        let travel = h - FaderCap.height
        let pos = FaderScale.position(volume: volume)
        let ceilingPos = FaderScale.position(volume: ceiling)
        let sec = section ?? theme.section(id.group)
        ZStack(alignment: .topLeading) {
            // The scale.
            ForEach(Self.marks, id: \.dB) { mark in
                let y = CGFloat(1 - FaderScale.position(dB: mark.dB)) * travel + half
                HStack(spacing: 2) {
                    Text(mark.label).font(.readout(7)).foregroundColor(mark.dB == 0 ? sec.text : sec.dim)
                        .frame(width: 11, alignment: .trailing)
                    Rectangle().fill(mark.dB == 0 ? sec.text.opacity(0.85) : sec.tick)
                        .frame(width: mark.dB == 0 ? 6 : 3, height: 1)
                }
                .offset(x: 1, y: y - 4.5)
            }
            // Off, at the very bottom.
            HStack(spacing: 2) {
                Text("∞").font(.readout(8)).foregroundColor(sec.dim).frame(width: 11, alignment: .trailing)
                Rectangle().fill(sec.tick).frame(width: 3, height: 1)
            }
            .offset(x: 1, y: travel + half - 5)
            Group {
                // The slot, cut into the panel: a lit lip around it, dark inside, deepest at
                // its left wall.
                Capsule().fill(sec.edgeLight.opacity(0.8)).frame(width: Self.slotWidth + 1.5, height: travel + 7.5)
                    .offset(x: -0.75, y: half - 3)
                Capsule().fill(LinearGradient(colors: [Color.black, theme.track, Color(white: 0.16)],
                                              startPoint: .leading, endPoint: .trailing))
                    .frame(width: Self.slotWidth, height: travel + 6)
                    .overlay(Capsule().strokeBorder(Color.black.opacity(0.8), lineWidth: 0.75))
                    .offset(y: half - 3)
                // The part above the ceiling: there, but held for now.
                if ceilingPos < 1 {
                    Capsule().fill(theme.warn.opacity(0.60))
                        .frame(width: 2, height: CGFloat(1 - ceilingPos) * travel)
                        .offset(x: 2, y: half)
                }
                // A line of light from the bottom up to the cap.
                Capsule().fill(LinearGradient(colors: [theme.slotFill.opacity(0.85), theme.slotFill.opacity(0.25)],
                                              startPoint: .top, endPoint: .bottom))
                    .frame(width: 2, height: max(0, CGFloat(pos) * travel))
                    .offset(x: 2, y: half + CGFloat(1 - pos) * travel)
            }
            .offset(x: Self.slotCentre - Self.slotWidth / 2)
            FaderCap(flagged: flagged)
                .offset(x: Self.slotCentre - FaderCap.width / 2, y: CGFloat(1 - pos) * travel)
            MeterBar(meters: model.meters, id: id, height: travel + 6)
                .offset(x: Metrics.stripWidth - Metrics.meterWidth - 6, y: half - 3)
        }
        .frame(width: Metrics.stripWidth, height: h, alignment: .topLeading)
        .contentShape(Rectangle())
        .background(GeometryReader { g in
            Color.clear.preference(key: FaderFramesKey.self, value: [id: g.frame(in: .named(MixerWindowView.space))])
        })
        .gesture(DragGesture(minimumDistance: 0, coordinateSpace: .named(MixerWindowView.space))
            .onChanged { g in
                if track == nil {
                    // From below 0 dB the fader stops at 0 dB; from there a new push goes higher.
                    track = DragTrack(start: pos, stopAt: FaderScale.position(dB: 0))
                    model.cursor = id
                    model.beginDrag(volumeOf: id)
                }
                // The cap follows the mouse from where it was: a press never makes the volume
                // jump, and asks for nothing until the hand moves.
                var t = track!
                if let p = t.value(travel: -g.translation.height, full: travel, range: 1) {
                    model.setVolume(id, FaderScale.volume(position: p))
                }
                track = t
            }
            .onEnded { _ in
                track = nil
                model.endDrag()
            })
        .help("Volume: drag up or down. It stops at 0 dB; let go and push again to go up to +12 dB.")
    }
}

struct StripView: View {
    @Environment(\.theme) private var theme
    @ObservedObject var model: PanelModel
    let id: StripID

    var body: some View {
        let sec = theme.section(id.group)
        let ch = model.snap.channels[id.group][id.channel]
        let d = model.disagreement
        let hasPan = id.group != ChannelNames.groupMonitor
        VStack(spacing: 6) {
            // The socket as it is on the unit, and a word for what it is.
            // No signal light yet: it comes with the meters' source.
            VStack(spacing: 3) {
                SocketGlyph(kind: ChannelNames.socket[id.group][id.channel], shines: id.group != ChannelNames.groupInput, size: 22)
                Text(ChannelNames.tag(id, rateCode: Int(model.snap.rateCode)))
                    .font(.panel(7, .bold)).tracking(0.3).lineLimit(1).fixedSize()
                    .foregroundColor(ChannelNames.tagStandsOut(id) ? sec.text.opacity(0.85) : sec.dim.opacity(0.8))
                    .frame(height: 9).copyable()
            }
            .help(ChannelNames.about(id, rateCode: Int(model.snap.rateCode)))
            Text(ChannelNames.short[id.group][id.channel])
                .font(.panel(13, .bold)).foregroundColor(sec.text).copyable()
                .background(GeometryReader { g in
                    Color.clear.preference(key: NameFramesKey.self, value: [id: g.frame(in: .named(MixerWindowView.space))])
                })
            Button(action: {
                model.cursor = id
                model.toggleMonitor(id)
            }) {
                // MUTE (1.1, the user's wish of 2026-11-27, in place of ON): lit red while the
                // channel is muted, dark while it plays. The same bit as before; only the word and
                // which way the key lights are turned round.
                Text("MUTE").font(.panel(9.5, .bold))
                    .foregroundColor(ch.on ? sec.keyText : Color.white)
                    .frame(width: 38, height: 20)
                    .keycap(on: !ch.on, color: theme.mute, radius: 10,
                            flagged: d?.touches(id, kind: Int32(PANEL_F_ON)) == true)
            }
            .buttonStyle(.plain)
            .help(ch.on ? "Mute: click to mute this channel in the monitor mix" : "Muted: click to let this channel into the monitor mix again")
            .accessibilityLabel("Mute \(ChannelNames.full(id))")
            .accessibilityAddTraits(ch.on ? [] : .isSelected)
            // SOLO: one channel of the group at a time (MONITOR OUT has none). The lamp is lit
            // for the soloed channel and, in a linked pair, for its partner, as Roland's panel
            // lights it.
            let soloed = model.snap.soloed[id.group][id.channel]
            Button(action: {
                model.cursor = id
                model.toggleSolo(id)
            }) {
                Text("SOLO").font(.panel(8, .bold))
                    .foregroundColor(soloed ? theme.onText : sec.keyText)
                    .frame(width: 38, height: 15)
                    .keycap(on: soloed, color: theme.warn, radius: 7.5,
                            flagged: d.map { Int32($0.kind) == Int32(PANEL_F_SOLO) && $0.group == id.group } ?? false)
            }
            .buttonStyle(.plain)
            .help("Solo: this channel alone, of its group. One channel of INPUT and one of OUTPUT can be soloed; a click on a lit SOLO puts it out")
            .opacity(hasPan ? 1 : 0)
            .allowsHitTesting(hasPan)
            if hasPan {
                VStack(spacing: 1) {
                    PanKnob(model: model, id: id, pan: ch.pan,
                            flagged: d?.touches(id, kind: Int32(PANEL_F_PAN)) == true)
                    Text(Format.pan(ch.pan)).font(.readout(9)).foregroundColor(sec.dim).copyable()
                }
                .padding(.top, 2)
            } else {
                Color.clear.frame(width: Metrics.knob, height: Metrics.knob + 14)
            }
            Fader(model: model, id: id, volume: ch.volume, ceiling: model.snap.volumeCeiling,
                  flagged: d?.touches(id, kind: Int32(PANEL_F_VOLUME)) == true)
            // The read-out: a small display behind glass.
            Text(Format.dB(ch.volume)).font(.readout(10)).foregroundColor(theme.readout).copyable()
                .frame(width: Metrics.stripWidth - 6, height: 17)
                .background(RoundedRectangle(cornerRadius: 4).fill(theme.glass))
                .overlay(RoundedRectangle(cornerRadius: 4).strokeBorder(
                    LinearGradient(colors: [theme.edgeDark, theme.edgeLight], startPoint: .top, endPoint: .bottom), lineWidth: 0.75))
        }
        .padding(.vertical, 8)
        .frame(width: Metrics.stripWidth)
        .engravedLane(sec, radius: 8)
        .compositingGroup()
        .opacity(ch.exists ? 1 : 0.28)
        .allowsHitTesting(ch.exists)
        .contentShape(Rectangle())
        .simultaneousGesture(TapGesture().onEnded { model.cursor = id })
    }
}

/// A MIDI connector as on the back of the unit: a round 5-pin socket.
struct MidiPortGlyph: View {
    @Environment(\.theme) private var theme
    var size: CGFloat = 22

    var body: some View {
        ZStack {
            Circle().fill(LinearGradient(colors: [theme.capTop, theme.capBottom], startPoint: .top, endPoint: .bottom))
            Circle().strokeBorder(theme.edgeDark.opacity(0.55), lineWidth: 0.75)
            Circle().fill(LinearGradient(colors: [theme.knobBottom, theme.knobTop], startPoint: .top, endPoint: .bottom))
                .frame(width: size * 0.84, height: size * 0.84)
            // The five pin holes, in a half circle.
            ForEach(0..<5, id: \.self) { i in
                let a = Double(i) * 45 * Double.pi / 180
                Circle().fill(theme.track).frame(width: size * 0.13, height: size * 0.13)
                    .offset(x: CGFloat(cos(a)) * size * 0.26, y: CGFloat(sin(a)) * size * 0.26)
            }
            // The key slot, opposite the pins.
            RoundedRectangle(cornerRadius: 0.5).fill(theme.track)
                .frame(width: size * 0.17, height: size * 0.15).offset(y: -size * 0.35)
        }
        .frame(width: size, height: size)
    }
}

/// MIDI IN and MIDI OUT: each connector with a green light under it for data passing.
struct MidiLampsView: View {
    @Environment(\.theme) private var theme
    @ObservedObject var meters: MeterModel

    var body: some View {
        HStack(spacing: 6) {
            ForEach(0..<2, id: \.self) { i in
                HStack(spacing: 4) {
                    VStack(alignment: .trailing, spacing: 0) {
                        Text("MIDI").font(.panel(7, .bold)).tracking(0.6).foregroundColor(theme.dim).copyable()
                        Text(ChannelNames.midiPorts[i]).font(.panel(10, .bold)).foregroundColor(theme.text).copyable()
                    }
                    .fixedSize()
                    VStack(spacing: 0) {
                        MidiPortGlyph(size: 22)
                        CellLED(cell: meters.midiLamps[i], size: 6) { $0.lit }
                    }
                }
                .help(ChannelNames.midiAbout[i] + ChannelNames.midiNote)
            }
        }
        .padding(.trailing, 6)
    }
}

/// Two strips, the cursor line over each, and their LINK button.
struct PairView: View {
    @Environment(\.theme) private var theme
    @ObservedObject var model: PanelModel
    let group: Int
    let pair: Int

    var body: some View {
        let sec = theme.section(group)
        let linked = model.snap.links[group][pair]
        let exists = model.snap.channels[group][pair * 2].exists
        let flagged = model.disagreement.map { Int32($0.kind) == Int32(PANEL_F_LINK) && $0.group == group && $0.index == pair } ?? false
        // Spacing 1 + 4 + 3 + 4 + 1 between a strip and LINK: the same 13 points as before
        // the cursor line became a button, so nothing else moves.
        VStack(spacing: 1) {
            HStack(spacing: Metrics.stripGap) {
                ForEach(0..<2, id: \.self) { i in
                    let id = StripID(group: group, channel: pair * 2 + i)
                    let chosen = model.cursor == id
                    VStack(spacing: 1) {
                        StripView(model: model, id: id)
                        // The cursor: the keyboard and the Device menu's exact values work on the
                        // strip it sits under. A click on it chooses that strip (a design choice,
                        // 2026-11-18): lit when chosen, dim when not. It sends nothing to the unit.
                        Button(action: { model.cursor = id }) {
                            Capsule().fill(chosen ? (sec.text.isDarkish ? theme.link : theme.cursor) : sec.tick)
                                .frame(width: Metrics.stripWidth - 12, height: 3)
                                .padding(.vertical, 4)
                                .frame(width: Metrics.stripWidth)
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .help(chosen ? "\(ChannelNames.full(id)) is chosen: the arrow keys and the Device menu's exact values work on it"
                                     : "Click to choose \(ChannelNames.full(id)): the arrow keys and the Device menu's exact values then work on it")
                        .accessibilityLabel("Choose \(ChannelNames.full(id))")
                        .accessibilityAddTraits(chosen ? .isSelected : [])
                    }
                }
            }
            Button(action: { model.toggleLink(group: group, pair: pair) }) {
                Text("LINK").font(.panel(9, .bold))
                    .foregroundColor(linked ? theme.onText : sec.keyText)
                    .frame(width: Metrics.stripWidth * 2 + Metrics.stripGap, height: 19)
                    .keycap(on: linked, color: theme.link, radius: 9.5, flagged: flagged)
            }
            .buttonStyle(.plain)
            .opacity(exists ? 1 : 0.28)
            .allowsHitTesting(exists)
            .help("Link the pair: mute and volume move together from the next change on. Linking by itself changes neither channel")
        }
    }
}

struct GroupView: View {
    @Environment(\.theme) private var theme
    @ObservedObject var model: PanelModel
    let group: Int

    var body: some View {
        let pairs = model.snap.channels[group].count / 2
        let sec = theme.section(group)
        VStack(alignment: .leading, spacing: 8) {
            Text(ChannelNames.sectionTitle[group]).font(.panel(11, .bold)).tracking(1.2).foregroundColor(sec.text)
                .lineLimit(1).frame(maxWidth: .infinity, alignment: .center).copyable()
                .help(ChannelNames.groupTitle[group] + ": " + ChannelNames.groupNote[group])
            HStack(spacing: 7) {
                ForEach(0..<pairs, id: \.self) { p in PairView(model: model, group: group, pair: p) }
            }
        }
        .padding(10)
        .metalPlate(sec, radius: 14)
        .environment(\.section, sec)
    }
}

// MARK: the unit's front panel

/// A push switch as on the unit's front: its lamp above, the cap pressed in when on.
/// It only shows what the unit reports; the switch itself is on the unit.
struct HardSwitch: View {
    @Environment(\.theme) private var theme
    let label: String
    let on: Bool
    var color: Color?
    var known = true
    /// Only the pretend unit has this: a click presses its switch. A real unit's
    /// switch is on the box, and the drawing only shows it.
    var press: (() -> Void)?

    var body: some View {
        Group {
            if let press = press {
                Button(action: press) { face.contentShape(Rectangle()) }.buttonStyle(.plain)
                    .help("\(label): \(on ? "pressed in" : "out"). Pretend unit: click to press its switch.")
            } else {
                face.help("\(label): \(known ? (on ? "pressed in" : "out") : "not known"). The switch is on the unit; this shows it.")
            }
        }
    }

    private var face: some View {
        VStack(spacing: 3) {
            LED(lit: known && on, color: color ?? theme.on, size: 7)
            // Black like the SAMPLE RATE knob, in both looks, as the unit's own switches are
            // (the user, 2026-11-14): out it stands up with a light top edge; pressed in, its
            // light is at the bottom and its top edge is dark.
            let hw = Theme.dark
            let shape = RoundedRectangle(cornerRadius: 4)
            let down = known && on
            Color.clear.frame(width: 30, height: 17)
                .background(ZStack {
                    if !down { SoftShadow(shape: shape, depth: 2.5, opacity: 0.55, steps: 3) }
                    shape.fill(LinearGradient(colors: down ? [Color.black.opacity(0.6), hw.knobBottom, hw.knobTop]
                                                           : [Color(hex: 0x3d4652), hw.knobTop, hw.knobBottom],
                                              startPoint: .top, endPoint: .bottom))
                })
                .overlay(shape.strokeBorder(LinearGradient(colors: down ? [hw.edgeDark, hw.edgeLight] : [hw.edgeLight, hw.edgeDark],
                                                           startPoint: .top, endPoint: .bottom), lineWidth: 1))
            Text(label).font(.panel(8, .bold)).tracking(0.6).foregroundColor(known && on ? theme.text : theme.dim)
        }
    }
}

/// The unit's SAMPLE RATE knob: five places, the pointer at the one the unit reports.
struct RateSelector: View {
    @Environment(\.theme) private var theme
    /// 0 to 4, or nil when not known.
    let code: Int?

    private static let labels = ["44.1", "48", "88.2", "96", "192"]
    private static let angles: [Double] = [-100, -50, 0, 50, 100]

    var body: some View {
        let cx: CGFloat = 56, cy: CGFloat = 45
        ZStack {
            ForEach(0..<5, id: \.self) { i in
                let a = Self.angles[i] * Double.pi / 180
                let here = code == i
                Text(Self.labels[i]).font(.readout(8, here ? .bold : .regular))
                    .foregroundColor(here ? theme.glow : theme.dim).copyable()
                    .position(x: cx + CGFloat(sin(a)) * 38, y: cy - CGFloat(cos(a)) * 34)
                Circle().fill(here ? theme.glow : theme.lampOff).frame(width: here ? 4 : 3, height: here ? 4 : 3)
                    .position(x: cx + CGFloat(sin(a)) * 22, y: cy - CGFloat(cos(a)) * 22)
            }
            // The knob itself is the dark look's in both looks: black with a metal rim, as on
            // the unit (the user, 2026-11-14). The marks and figures around it follow the look.
            let hw = Theme.dark
            ZStack {
                SoftShadow(shape: Circle(), depth: 3, opacity: 0.6, steps: 4)
                Circle().fill(LinearGradient(colors: [hw.capTop, hw.capBottom], startPoint: .top, endPoint: .bottom))
                Circle().fill(RadialGradient(colors: [Color(hex: 0x4c535d), Color(hex: 0x16191e), Color(hex: 0x0a0b0d)],
                                             center: UnitPoint(x: 0.38, y: 0.30), startRadius: 0, endRadius: 22))
                    .padding(2)
                Circle().strokeBorder(LinearGradient(colors: [Color.white.opacity(0.45), Color.black.opacity(0.6)],
                                                     startPoint: .top, endPoint: .bottom), lineWidth: 0.75)
                    .padding(2)
                if let code = code, code >= 0, code < 5 {
                    Capsule().fill(hw.glow).frame(width: 2.5, height: 11).offset(y: -8.5)
                        .rotationEffect(.degrees(Self.angles[code]))
                }
            }
            .frame(width: 34, height: 34)
            .position(x: cx, y: cy)
        }
        .frame(width: 112, height: 66)
        .help("SAMPLE RATE: the knob is on the unit; this shows where it stands. The app never changes it.")
    }
}

struct DevicePanelView: View {
    @Environment(\.theme) private var theme
    @ObservedObject var model: PanelModel

    private func heading(_ s: String) -> some View {
        Text(s).font(.panel(9, .bold)).tracking(1).foregroundColor(theme.dim).fixedSize().copyable()
    }

    private var divider: some View {
        Rectangle().fill(LinearGradient(colors: [theme.edgeDark, theme.edgeLight], startPoint: .leading, endPoint: .trailing))
            .frame(width: 2, height: 62)
    }

    /// One of the two device buttons. The chosen one is lit.
    private func deviceKey(_ label: String, chosen: Bool, real: Bool, help: String) -> some View {
        Button(action: { model.onChooseDevice?(real) }) {
            Text(label).font(.panel(11, .semibold)).foregroundColor(chosen ? theme.onText : theme.text)
                .padding(.horizontal, 11).padding(.vertical, 4)
                .keycap(on: chosen, color: theme.link, radius: 11)
        }
        .buttonStyle(.plain)
        .help(help)
    }

    var body: some View {
        let s = model.snap
        // With the pretend unit the three switches can be clicked: there is no box to press them on.
        #if UA101_USER
        let (digitalPress, monoPress, softPress): ((() -> Void)?, (() -> Void)?, (() -> Void)?) = (nil, nil, nil)
        #else
        let pretend = model.transport as? PretendUnit
        let digitalPress = pretend.map { u in { u.press(Int32(PANEL_SWITCH_DIGITAL), in: !u.digital) } }
        let monoPress = pretend.map { u in { u.press(Int32(PANEL_SWITCH_MONO), in: !u.mono) } }
        let softPress = pretend.map { u in { u.press(Int32(PANEL_SWITCH_SOFT), in: !u.softControl) } }
        #endif
        HStack(spacing: 18) {
            VStack(alignment: .leading, spacing: 7) {
                HStack(spacing: 10) {
                    heading("DEVICE")
                    // The unit's USB link, as a red diode on a front panel: lit while the unit
                    // answers, dark with no unit or when it stopped answering.
                    Lamp(label: "USB", lit: s.unitAnswers, color: theme.optical)
                        .help("Glows red while the unit answers over USB. Dark: no unit, or it doesn't answer.")
                }
                .frame(height: 11)
                HStack(spacing: 6) {
                    #if UA101_USER
                    // The end-user build talks to the UA-101 only: a lit name, not a choice.
                    Text("UA-101").font(.panel(11, .semibold)).foregroundColor(theme.onText)
                        .padding(.horizontal, 11).padding(.vertical, 4)
                        .keycap(on: true, color: theme.link, radius: 11)
                        .help("The UA-101, through the driver's helper.")
                    #else
                    deviceKey("Pretend unit", chosen: model.transport.isPretend, real: false,
                              help: "A stand-in for the UA-101 inside this app, for trying everything out. Nothing reaches the real unit.")
                    deviceKey("UA-101", chosen: !model.transport.isPretend, real: true,
                              help: "The real unit, through the driver's helper. Look only until you allow changes (Device menu): its settings and levels are read, and nothing that changes it is sent.")
                    #endif
                }
            }
            divider
            HStack(spacing: 4) {
                VStack(alignment: .leading, spacing: 4) {
                    heading("SAMPLE RATE")
                    HStack(alignment: .firstTextBaseline, spacing: 3) {
                        Text(s.known ? Format.rate(s.rateHz) : "–").font(.readout(20, .semibold)).foregroundColor(theme.readout).copyable()
                        Text("kHz").font(.panel(10)).foregroundColor(theme.readout.opacity(0.65)).copyable()
                    }
                    .frame(width: 84, height: 30)
                    .background(RoundedRectangle(cornerRadius: 5).fill(theme.glass))
                    .overlay(RoundedRectangle(cornerRadius: 5).strokeBorder(
                        LinearGradient(colors: [theme.edgeDark, theme.edgeLight], startPoint: .top, endPoint: .bottom), lineWidth: 0.75))
                }
                RateSelector(code: s.known ? Int(s.rateCode) : nil)
            }
            divider
            VStack(alignment: .leading, spacing: 5) {
                heading("DIGITAL IN")
                HStack(alignment: .top, spacing: 12) {
                    HardSwitch(label: "DIGITAL", on: s.clockSource != 0, known: s.known,
                               press: digitalPress)
                    VStack(alignment: .leading, spacing: 2) {
                        Lamp(label: "SYNC", lit: s.known && s.clockSource != 0 && s.clockStatus == 0)
                        Text(!s.known ? "–" : (s.clockSource == 0 ? "clock: internal" : (s.clockSource == 1 ? "clock: digital in" : "clock: source \(s.clockSource)")))
                            .font(.panel(10)).foregroundColor(theme.text).copyable()
                        Text(s.known && s.clockStatus != 0 ? "no signal to lock to" : " ")
                            .font(.panel(10)).foregroundColor(theme.warn).copyable()
                    }
                }
            }
            divider
            VStack(alignment: .leading, spacing: 5) {
                heading("DIRECT MONITOR")
                HStack(alignment: .top, spacing: 14) {
                    HardSwitch(label: "MONO", on: s.mono, color: theme.warn, known: s.known,
                               press: monoPress)
                    HardSwitch(label: "SOFT CTRL", on: s.softControl, known: s.known,
                               press: softPress)
                }
            }
            Spacer(minLength: 12)
            if model.changesByThemselves && !model.transport.isPretend {
                let light = StatusLight.of(model, theme)
                VStack(alignment: .trailing, spacing: 5) {
                    Lamp(label: light.label, lit: light.lit, color: light.color)
                    Text(light.line).font(.panel(10)).foregroundColor(theme.dim)
                        .lineLimit(1).truncationMode(.middle).copyable()
                }
                .frame(width: 250, alignment: .trailing)
                .help(UnitState.of(model).long)
            } else {
                VStack(alignment: .trailing, spacing: 5) {
                    Lamp(label: s.state == .ready ? (s.busy || s.settling ? "Sending and checking…" : "In step with the unit")
                            : (s.state == .syncing ? "Reading the settings…" : (s.state == .silent ? "The unit doesn't answer" : "No unit")),
                         lit: s.state == .ready && !s.busy && !s.settling,
                         color: model.disagreement == nil ? theme.on : theme.warn)
                    Text("read \(s.readsSent) · changes sent \(s.setsSent) · not taken \(s.disagreements)")
                        .font(.readout(10)).foregroundColor(theme.dim).copyable()
                }
            }
        }
        .padding(.horizontal, 16).padding(.vertical, 10)
        .metalPlate(theme.bar, radius: 14)
    }
}

/// The end-user app's one status light (a design choice of 2026-11-21):
/// "Ready" while all is well, words only when something isn't. No counts and no flicker
/// while a change is sent: those are in the record and the report.
struct StatusLight {
    let label: String
    let lit: Bool
    let color: Color
    let line: String

    static func of(_ model: PanelModel, _ theme: Theme) -> StatusLight {
        let s = model.snap
        let rate = s.known && s.rateCode == 4 ? "At 192 kHz: channels 1 to 6 only." : " "
        switch s.state {
        case .offline: return StatusLight(label: "No UA-101", lit: false, color: theme.dim, line: "Switched off, or not plugged in.")
        case .syncing: return StatusLight(label: "Reading the settings…", lit: false, color: theme.dim, line: " ")
        case .silent: return StatusLight(label: "The UA-101 doesn't answer", lit: true, color: theme.bad, line: "Asking again every 3 seconds.")
        case .ready: break
        }
        if model.disagreement != nil {
            return StatusLight(label: "The UA-101 kept another value", lit: true, color: theme.warn, line: "See the Messages window.")
        }
        if !s.writesAllowed {
            if model.changesLocked { return StatusLight(label: "Changes locked", lit: true, color: theme.warn, line: "Device menu → Lock Changes to unlock.") }
            // A moment before changes come on by themselves, or a helper that passes on none.
            return StatusLight(label: "Look only", lit: true, color: theme.warn,
                               line: model.transport.allowed.isEmpty ? "The driver's helper passes on no changes." : " ")
        }
        return StatusLight(label: "Ready", lit: true, color: theme.on, line: rate)
    }
}

// MARK: the window

/// The sign across the mixer while SOFT CTRL is out on the unit.
struct SoftControlSign: View {
    @Environment(\.theme) private var theme
    var pretend = false

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                LED(lit: false, size: 8)
                Text("SOFT CTRL switch is turned off.").font(.panel(15, .bold)).foregroundColor(theme.text).copyable()
            }
            Text(pretend ? "To control the mixer, click the SOFT CTRL switch at the bottom of this window (pretend unit)."
                         : "To control the mixer, press SOFT CTRL in on the unit.").font(.panel(13)).foregroundColor(theme.text).copyable()
            Text("Until then the unit ignores these settings: inputs and the computer's outputs pass as they are. The patch bay still works.")
                .font(.panel(11)).foregroundColor(theme.dim).copyable()
        }
        .padding(.horizontal, 22).padding(.vertical, 16)
        .raised(12)
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(theme.warn.opacity(0.6), lineWidth: 1))
    }
}

struct MixerWindowView: View {
    @Environment(\.theme) private var theme
    @ObservedObject var model: PanelModel
    @ObservedObject var ui: PanelUI
    @ObservedObject var messages: MessageCenter
    /// The mixer's own coordinates: the drags of its faders and knobs are measured in them.
    static let space = "mixer"
    /// The buttons on the top line, after the MIDI lamps, left to right.
    static let lineButtons = ["Driver Settings…", "Messages", "Patch Bay…", "Recall…", "Save…", "Reset…"]

    var body: some View {
        let s = model.snap
        let byThemselves = model.changesByThemselves && !model.transport.isPretend
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 8) {
                Text("UA-101").font(.panel(17, .heavy)).tracking(1).foregroundColor(theme.text).copyable()
                Text("Monitor mixer").font(.panel(13)).foregroundColor(theme.dim).lineLimit(1).copyable()
                if byThemselves {
                    // The end-user app: nothing while all is well.
                    if s.state == .ready && !s.writesAllowed {
                        Pill(text: model.changesLocked ? "CHANGES LOCKED" : "LOOK ONLY", color: theme.warn, filled: true)
                    }
                } else {
                    if model.transport.isPretend { Pill(text: "PRETEND UNIT", color: theme.link) }
                    else { Pill(text: s.writesAllowed ? "UA-101 · " + model.allowed.short.uppercased() : "UA-101 · LOOK ONLY", color: theme.on) }
                    Pill(text: s.writesAllowed ? "CHANGES ALLOWED" : "READ-ONLY",
                         color: s.writesAllowed ? theme.on : theme.warn, filled: !s.writesAllowed)
                }
                Spacer()
                MidiLampsView(meters: model.meters)
                // In this order (a design choice, 2026-11-20: Driver Settings between the MIDI
                // lamps and Messages): Self.lineButtons.
                Button(Self.lineButtons[0]) { ui.showDriverSettings() }
                MessagesButton(messages: messages, open: { ui.showMessages() })
                Button(Self.lineButtons[2]) { ui.showPatchBay() }
                Button(Self.lineButtons[3]) { ui.sheet = .load }
                Button(Self.lineButtons[4]) { ui.sheet = .store }
                Button(Self.lineButtons[5]) { ui.sheet = .reset }
            }
            .buttonStyle(PanelButtonStyle(compact: true))
            .frame(width: Metrics.mixerWidth)

            // The state, in one line that is always the same height (a design choice,
            // 2026-11-16): nothing above the mixer grows or shrinks, so the faders and knobs
            // never move under the hand. Every note and warning is in the Messages window.
            // The end-user app has no line: the light at the bottom right says the state, and the
            // Messages window says it in full (a design choice, 2026-11-21). Either way the
            // window's height never changes while it is open.
            if !byThemselves {
                StateLine(state: UnitState.of(model))
                    .frame(width: Metrics.mixerWidth)
            }

            HStack(alignment: .top, spacing: 10) {
                GroupView(model: model, group: ChannelNames.groupInput)
                GroupView(model: model, group: ChannelNames.groupOutput)
                GroupView(model: model, group: ChannelNames.groupMonitor)
            }
            // Greyed while the mixer does nothing: unknown settings, or Soft Control out.
            .compositingGroup()
            .opacity(s.mixerLive ? 1 : (s.softControlOff ? 0.32 : (s.known ? 0.5 : 0.25)))
            .allowsHitTesting(!s.softControlOff)
            // As Roland's panel does: a sign across the mixer while the switch is out.
            .overlay(Group { if s.softControlOff { SoftControlSign(pretend: model.transport.isPretend) } })

            DevicePanelView(model: model)
        }
        .padding(14)
        .background(theme.window)
        .coordinateSpace(name: Self.space)
        .onPreferenceChange(FaderFramesKey.self) { ui.faderFrames = $0 }
        .onPreferenceChange(ControlFramesKey.self) { ui.knobFrames = $0 }
        .onPreferenceChange(NameFramesKey.self) { ui.nameFrames = $0 }
        .fixedSize()
    }
}
