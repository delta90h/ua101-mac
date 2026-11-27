// The look: every colour and size the windows use is here, so the user's
// choices are made in one place. Drawn from scratch; nothing is Roland's.
//
// A design choice (2026-11-11): hardware-like and futuristic: dark charcoal,
// raised plates and sunken wells, metal caps, lamps that glow. To stay cheap,
// depth is made of gradients and one-point edges only: no blur and no shadow
// anywhere, and nothing moves unless a value changes.
import SwiftUI

struct Theme {
    let name: String
    let scheme: ColorScheme
    let window: Color     // behind everything
    let panel: Color      // a raised plate, flat (dialog rows)
    let strip: Color      // a well, flat
    let line: Color       // hairlines and outlines
    let text: Color
    let dim: Color        // secondary text
    let track: Color      // fader slot, meter slot, display glass
    let cap: Color        // slider knobs in the dialogs
    let lampOff: Color
    let on: Color         // monitor ON
    let link: Color       // LINK, chosen things
    let cursor: Color     // the cursor line
    let signal: Color     // signal light, the meter's low part
    let warn: Color       // read-only, Soft Control out, the meter's middle
    let bad: Color        // the unit disagreed, the meter's top
    let onText: Color     // text on a lit button
    // Depth.
    let raisedTop: Color, raisedBottom: Color // a plate or key that stands out
    let insetTop: Color, insetBottom: Color   // a well that is sunk in
    let edgeLight: Color, edgeDark: Color     // the two sides of a bevel
    let capTop: Color, capBottom: Color       // metal: fader caps, knob rims
    let knobTop: Color, knobBottom: Color     // a knob's body
    let glow: Color                           // the light on a surface: pointers, arcs, the 0 dB mark
    let keyTop: Color, keyBottom: Color       // a key that is off
    let glass: Color                          // read-out glass
    let readout: Color                        // the figures on the glass
    let slotFill: Color                       // the light in a fader's slot
    let capMark: Color                        // the bright line across a fader cap
    let capGroove: Color                      // its grip lines
    let capShade: Color                       // its skirt, seen just below it
    let capEdge: Color                        // its outline
    let label: Color                          // under a coloured pill or banner

    /// A lit MUTE key (1.1): deep red, #A8000E, in both looks (the user's choice of eight, 2026-11-27).
    var mute: Color { Color(red: 168 / 255, green: 0, blue: 14 / 255) }
    /// A lit SOLO key (1.1): amber #FA9E12, between honey and Apple's orange, in both looks
    /// (the user's choice, 2026-11-27), with dark words on it. The other warnings keep `warn`.
    var solo: Color { Color(red: 250 / 255, green: 158 / 255, blue: 18 / 255) }
    var soloText: Color { Color(red: 0.04, green: 0.06, blue: 0.08) }
    /// The acid glow of a lit MUTE or SOLO key (the user's choice, 2026-11-27, with less halo than
    /// first shown): the key burns in its colour made as bright as it goes, white-hot at the middle.
    var muteGlow: KeyGlow { KeyGlow(light: Color(hex: 0xff0015), hot: Color(hex: 0xff8c98)) }
    var soloGlow: KeyGlow { KeyGlow(light: Color(hex: 0xffa112), hot: Color(hex: 0xffd594)) }

    /// The red light inside an optical socket.
    var optical: Color { scheme == .dark ? Color(red: 1.00, green: 0.24, blue: 0.20) : Color(red: 0.90, green: 0.14, blue: 0.12) }

    // Round 5: four clear steps: well darkest, then window, plate, key.
    // 2026-11-14, a design choice: steel blue, the blue of the app icon's panel, in place of
    // charcoal ("3 steel blue"); same steps, the lamps, meters and text unchanged.
    static let dark = Theme(
        name: "dark", scheme: .dark,
        // 1.1 (the user's choice, 2026-11-27, "C blue steel"): a darker window around the sections.
        window: Color(red: 0.031, green: 0.051, blue: 0.086),
        panel: Color(red: 0.128, green: 0.206, blue: 0.304),
        strip: Color(red: 0.034, green: 0.064, blue: 0.105),
        line: Color(white: 1, opacity: 0.10),
        text: Color(red: 0.93, green: 0.94, blue: 0.96),
        dim: Color(red: 0.640, green: 0.730, blue: 0.850),
        track: Color(red: 0.010, green: 0.020, blue: 0.036),
        cap: Color(red: 0.86, green: 0.87, blue: 0.89),
        lampOff: Color(white: 1, opacity: 0.11),
        on: Color(red: 0.34, green: 0.88, blue: 0.52),
        link: Color(red: 0.47, green: 0.76, blue: 1.00),
        cursor: Color(red: 0.90, green: 0.96, blue: 1.00),
        signal: Color(red: 0.34, green: 0.88, blue: 0.52),
        warn: Color(red: 0.98, green: 0.70, blue: 0.24),
        bad: Color(red: 0.98, green: 0.36, blue: 0.34),
        onText: Color(red: 0.04, green: 0.06, blue: 0.08),
        raisedTop: Color(red: 0.154, green: 0.240, blue: 0.349),
        raisedBottom: Color(red: 0.105, green: 0.173, blue: 0.259),
        insetTop: Color(red: 0.022, green: 0.045, blue: 0.075),
        insetBottom: Color(red: 0.045, green: 0.079, blue: 0.124),
        edgeLight: Color(white: 1, opacity: 0.17),
        edgeDark: Color(white: 0, opacity: 0.60),
        capTop: Color(red: 0.97, green: 0.98, blue: 0.99),
        capBottom: Color(red: 0.540, green: 0.610, blue: 0.710),
        knobTop: Color(red: 0.173, green: 0.225, blue: 0.292),
        knobBottom: Color(red: 0.038, green: 0.060, blue: 0.094),
        glow: Color(red: 0.90, green: 0.96, blue: 1.00),
        keyTop: Color(red: 0.199, green: 0.289, blue: 0.401),
        keyBottom: Color(red: 0.143, green: 0.221, blue: 0.315),
        glass: Color(red: 0.010, green: 0.020, blue: 0.036),
        readout: Color(red: 0.90, green: 0.96, blue: 1.00),
        slotFill: Color(red: 0.90, green: 0.96, blue: 1.00),
        capMark: Color(red: 0.030, green: 0.050, blue: 0.085),
        capGroove: Color(red: 0.520, green: 0.590, blue: 0.690),
        capShade: Color(red: 0.006, green: 0.012, blue: 0.024),
        capEdge: Color(red: 0.143, green: 0.195, blue: 0.262),
        label: Color(red: 0.056, green: 0.098, blue: 0.154))

    // A design choice (2026-11-11, round 4): easy on the eyes. No white anywhere:
    // warm putty-grey surfaces, each part a clear step from the next (window, plate,
    // well, key), and the holes (slots, sockets, read-out glass) dark as on real gear.
    static let light = Theme(
        name: "light", scheme: .light,
        window: Color(red: 0.511, green: 0.456, blue: 0.395),
        panel: Color(red: 0.744, green: 0.688, blue: 0.620),
        strip: Color(red: 0.601, green: 0.545, blue: 0.482),
        line: Color(red: 0.16, green: 0.12, blue: 0.06, opacity: 0.32),
        text: Color(red: 0.100, green: 0.085, blue: 0.065),
        dim: Color(red: 0.235, green: 0.205, blue: 0.165),
        track: Color(red: 0.195, green: 0.180, blue: 0.160),
        cap: Color(red: 0.896, green: 0.846, blue: 0.780),
        lampOff: Color(red: 0.16, green: 0.12, blue: 0.06, opacity: 0.26),
        on: Color(red: 0.085, green: 0.500, blue: 0.265),
        link: Color(red: 0.110, green: 0.380, blue: 0.780),
        cursor: Color(red: 0.110, green: 0.380, blue: 0.780),
        signal: Color(red: 0.180, green: 0.720, blue: 0.380),
        warn: Color(red: 0.720, green: 0.360, blue: 0.000),
        bad: Color(red: 0.740, green: 0.140, blue: 0.110),
        onText: Color(red: 0.985, green: 0.975, blue: 0.950),
        raisedTop: Color(red: 0.776, green: 0.720, blue: 0.652),
        raisedBottom: Color(red: 0.711, green: 0.655, blue: 0.587),
        insetTop: Color(red: 0.566, green: 0.511, blue: 0.450),
        insetBottom: Color(red: 0.631, green: 0.576, blue: 0.512),
        edgeLight: Color(red: 1.0, green: 0.98, blue: 0.93, opacity: 0.62),
        edgeDark: Color(red: 0.14, green: 0.10, blue: 0.05, opacity: 0.55),
        capTop: Color(red: 0.400, green: 0.385, blue: 0.360),
        capBottom: Color(red: 0.150, green: 0.142, blue: 0.130),
        knobTop: Color(red: 0.906, green: 0.858, blue: 0.790),
        knobBottom: Color(red: 0.706, green: 0.653, blue: 0.590),
        glow: Color(red: 0.080, green: 0.330, blue: 0.740),
        keyTop: Color(red: 0.896, green: 0.846, blue: 0.782),
        keyBottom: Color(red: 0.806, green: 0.753, blue: 0.685),
        glass: Color(red: 0.140, green: 0.130, blue: 0.115),
        readout: Color(red: 0.970, green: 0.890, blue: 0.720),
        slotFill: Color(red: 0.420, green: 0.680, blue: 0.980),
        capMark: Color(red: 0.930, green: 0.905, blue: 0.850),
        capGroove: Color(red: 0.470, green: 0.448, blue: 0.410),
        capShade: Color(red: 0.300, green: 0.275, blue: 0.235),
        capEdge: Color(red: 0.080, green: 0.072, blue: 0.062),
        label: Color(red: 0.876, green: 0.828, blue: 0.765))
}

/// The user's choice of appearance, as in macOS itself.
enum Look: String, CaseIterable, Identifiable {
    case system, dark, light
    static let defaultsKey = "look"

    var id: String { rawValue }
    var title: String {
        switch self {
        case .system: return "Follow the System"
        case .dark: return "Dark"
        case .light: return "Light"
        }
    }
    /// What the app tells macOS to draw it as; nil leaves it to the system's setting.
    var appearanceName: NSAppearance.Name? {
        switch self {
        case .system: return nil
        case .dark: return .darkAqua
        case .light: return .aqua
        }
    }
}

extension Theme {
    static func matching(_ scheme: ColorScheme) -> Theme { scheme == .dark ? .dark : .light }
}

/// Gives a window the theme that matches the appearance macOS draws it in, so
/// the title bar, the menus, the alerts and the app's own drawing always agree.
private struct FollowsAppearance: ViewModifier {
    @Environment(\.colorScheme) private var scheme
    func body(content: Content) -> some View { content.environment(\.theme, Theme.matching(scheme)) }
}

private struct ThemeKey: EnvironmentKey { static let defaultValue = Theme.dark }

extension EnvironmentValues {
    var theme: Theme {
        get { self[ThemeKey.self] }
        set { self[ThemeKey.self] = newValue }
    }
}

extension View {
    func followsAppearance() -> some View { modifier(FollowsAppearance()) }
    /// Words that can be selected and copied (2026-11-20). Only on plain text: the words inside a
    /// button, a slider or a fader stay part of the control, so a click still works it.
    func copyable() -> some View { textSelection(.enabled) }
    func themed(_ theme: Theme) -> some View {
        environment(\.theme, theme).environment(\.colorScheme, theme.scheme)
    }
}

enum Metrics {
    static let stripWidth: CGFloat = 50
    static let stripGap: CGFloat = 3
    static let faderHeight: CGFloat = 200
    static let knob: CGFloat = 34   // with its arc
    static let meterWidth: CGFloat = 5
    static let meterSegments = 30
    /// The three strip groups side by side, as drawn (measured: the mixer window without
    /// banners is this wide inside its padding). The banners are held to it, so a long
    /// sentence wraps instead of widening the window past MONITOR OUT. The self-test checks
    /// that a long banner leaves the window's width alone.
    static let mixerWidth: CGFloat = 1269
}

extension Font {
    static func panel(_ size: CGFloat, _ weight: Font.Weight = .regular) -> Font {
        .system(size: size, weight: weight, design: .default)
    }
    static func readout(_ size: CGFloat, _ weight: Font.Weight = .medium) -> Font {
        .system(size: size, weight: weight, design: .monospaced)
    }
}

// MARK: depth, made of gradients only


/// Stands out from the surface: light from above, a bright top edge.
struct RaisedPlate: ViewModifier {
    @Environment(\.theme) private var theme
    let radius: CGFloat

    func body(content: Content) -> some View {
        content
            .background(RoundedRectangle(cornerRadius: radius).fill(
                LinearGradient(colors: [theme.raisedTop, theme.raisedBottom], startPoint: .top, endPoint: .bottom)))
            .overlay(RoundedRectangle(cornerRadius: radius).strokeBorder(
                LinearGradient(colors: [theme.edgeLight, theme.edgeDark], startPoint: .top, endPoint: .bottom), lineWidth: 1))
    }
}

/// Sunk into the surface: dark at the top, a bright bottom edge.
struct InsetWell: ViewModifier {
    @Environment(\.theme) private var theme
    let radius: CGFloat
    var flagged = false

    func body(content: Content) -> some View {
        content
            .background(RoundedRectangle(cornerRadius: radius).fill(
                LinearGradient(colors: [theme.insetTop, theme.insetBottom], startPoint: .top, endPoint: .bottom)))
            .overlay(RoundedRectangle(cornerRadius: radius).strokeBorder(
                LinearGradient(colors: [theme.edgeDark, theme.edgeLight], startPoint: .top, endPoint: .bottom), lineWidth: 1))
            .overlay(RoundedRectangle(cornerRadius: radius).strokeBorder(flagged ? theme.bad : Color.clear, lineWidth: 2))
    }
}

/// A lit key's acid glow: its colour at full brightness, and the white-hot tint of it.
struct KeyGlow {
    let light: Color
    let hot: Color
}

/// A key: raised with a soft shadow when off; pressed in and glowing in its colour when on.
/// `flagged` rings it red. In a mixer section it takes that section's colours.
struct KeyCap: ViewModifier {
    @Environment(\.theme) private var theme
    @Environment(\.section) private var section
    let on: Bool
    let color: Color
    let radius: CGFloat
    var flagged = false
    var glow: KeyGlow? = nil

    func body(content: Content) -> some View {
        let shape = RoundedRectangle(cornerRadius: radius)
        let top = section?.keyTop ?? theme.keyTop, bottom = section?.keyBottom ?? theme.keyBottom
        let light = section?.edgeLight ?? theme.edgeLight
        return content
            .background(ZStack {
                if on, let glow {
                    // Acid glow: a tight halo of its full colour on the plate around it, a hot rim.
                    shape.fill(glow.light).shadow(color: glow.light.opacity(0.9), radius: 2).shadow(color: glow.light.opacity(0.45), radius: 5)
                    // Burning: white-hot a little below the middle.
                    shape.fill(EllipticalGradient(colors: [glow.hot.opacity(0.32), glow.light.opacity(0)],
                                                  center: UnitPoint(x: 0.5, y: 0.6), endRadiusFraction: 0.5))
                    shape.fill(LinearGradient(stops: [
                        .init(color: Color.black.opacity(0.34), location: 0), .init(color: Color.black.opacity(0), location: 0.45),
                        .init(color: Color.white.opacity(0), location: 0.6), .init(color: Color.white.opacity(0.22), location: 1),
                    ], startPoint: .top, endPoint: .bottom))
                    // A hot rim on its own edge.
                    shape.strokeBorder(glow.hot.opacity(0.7), lineWidth: 1)
                } else if on {
                    // Its light on the plate around it.
                    shape.inset(by: -2.5).fill(color.opacity(0.14))
                    shape.inset(by: -1.25).fill(color.opacity(0.22))
                    shape.fill(color)
                    // Pressed in: shaded under the top edge, lit towards the bottom.
                    shape.fill(LinearGradient(stops: [
                        .init(color: Color.black.opacity(0.34), location: 0), .init(color: Color.black.opacity(0), location: 0.45),
                        .init(color: Color.white.opacity(0), location: 0.6), .init(color: Color.white.opacity(0.22), location: 1),
                    ], startPoint: .top, endPoint: .bottom))
                } else {
                    SoftShadow(shape: shape, depth: 2.2, opacity: 0.5, steps: 3)
                    shape.fill(LinearGradient(colors: [top, bottom], startPoint: .top, endPoint: .bottom))
                    // The shine on its top.
                    shape.fill(LinearGradient(colors: [Color.white.opacity(0.13), Color.white.opacity(0)],
                                              startPoint: .top, endPoint: .center))
                }
            })
            .overlay(shape.strokeBorder(
                on ? LinearGradient(colors: [Color.black.opacity(glow == nil ? 0.55 : 0.33), Color.white.opacity(glow == nil ? 0.40 : 0.24)], startPoint: .top, endPoint: .bottom)
                   : LinearGradient(colors: [light, Color.black.opacity(0.55)], startPoint: .top, endPoint: .bottom),
                lineWidth: 1))
            // The unit disagreed: a ring just outside the key, so it shows on a lit red key too.
            .overlay(shape.inset(by: -2.5).strokeBorder(flagged ? theme.bad : Color.clear, lineWidth: 2))
    }
}

extension View {
    func raised(_ radius: CGFloat = 6) -> some View { modifier(RaisedPlate(radius: radius)) }
    func inset(_ radius: CGFloat = 6, flagged: Bool = false) -> some View { modifier(InsetWell(radius: radius, flagged: flagged)) }
    func keycap(on: Bool, color: Color, radius: CGFloat = 5, flagged: Bool = false, glow: KeyGlow? = nil) -> some View {
        modifier(KeyCap(on: on, color: color, radius: radius, flagged: flagged, glow: glow))
    }
}

/// A lamp as on a front panel. Lit, it has a bright core and a soft ring of its colour.
struct LED: View {
    @Environment(\.theme) private var theme
    let lit: Bool
    var color: Color?
    var size: CGFloat = 8

    var body: some View {
        let c = color ?? theme.on
        ZStack {
            if lit {
                Circle().fill(RadialGradient(colors: [c.opacity(0.62), c.opacity(0)], center: .center,
                                             startRadius: size * 0.35, endRadius: size * 0.95))
                Circle().fill(RadialGradient(colors: [Color.white.opacity(0.95), c], center: UnitPoint(x: 0.42, y: 0.36),
                                             startRadius: 0, endRadius: size * 0.5))
                    .frame(width: size, height: size)
            } else {
                Circle().fill(theme.lampOff).frame(width: size, height: size)
                // Its glass dome catches the light.
                Circle().fill(RadialGradient(colors: [Color.white.opacity(0.28), Color.white.opacity(0)],
                                             center: UnitPoint(x: 0.38, y: 0.32), startRadius: 0, endRadius: size * 0.32))
                    .frame(width: size, height: size)
            }
            Circle().strokeBorder(theme.edgeDark.opacity(0.7), lineWidth: 0.75).frame(width: size, height: size)
        }
        .frame(width: size * 1.9, height: size * 1.9)
    }
}

/// A small lamp with a label, as on a front panel.
struct Lamp: View {
    @Environment(\.theme) private var theme
    let label: String
    let lit: Bool
    var color: Color?

    var body: some View {
        HStack(spacing: 2) {
            LED(lit: lit, color: color, size: 8)
            Text(label).font(.panel(11, .semibold)).foregroundColor(lit ? theme.text : theme.dim).copyable()
        }
    }
}

/// A coloured pill for a state: READ-ONLY, PRETEND UNIT.
struct Pill: View {
    let text: String
    let color: Color
    var filled = false
    @Environment(\.theme) private var theme

    var body: some View {
        Text(text)
            .font(.panel(10, .bold))
            .lineLimit(1).fixedSize()
            .foregroundColor(filled ? theme.onText : color)
            .padding(.horizontal, 9).padding(.vertical, 3)
            .background(Capsule().fill(filled ? color : color.opacity(0.13)).background(Capsule().fill(theme.label)))
            .overlay(Capsule().strokeBorder(color.opacity(filled ? 0 : 0.55), lineWidth: 1))
            .copyable()
    }
}

/// One line across the window that tells the user about a state.
struct Banner: View {
    @Environment(\.theme) private var theme
    let symbol: String
    let color: Color
    let title: String
    let text: String
    var actionLabel: String?
    var action: (() -> Void)?

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text(symbol).font(.panel(12, .bold)).foregroundColor(color)
            Text(title).font(.panel(12, .bold)).foregroundColor(theme.text).copyable()
            Text(text).font(.panel(12)).foregroundColor(theme.text.opacity(0.85))
                .fixedSize(horizontal: false, vertical: true).copyable()
            Spacer(minLength: 8)
            if let label = actionLabel, let action = action {
                Button(label, action: action).buttonStyle(.plain)
                    .font(.panel(11, .semibold)).foregroundColor(color)
            }
        }
        .padding(.horizontal, 12).padding(.vertical, 7)
        .background(RoundedRectangle(cornerRadius: 8).fill(color.opacity(0.12)).background(RoundedRectangle(cornerRadius: 8).fill(theme.label)))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(color.opacity(0.5), lineWidth: 1))
    }
}

/// The app's own button look, the same in every window: a raised pill.
struct PanelButtonStyle: ButtonStyle {
    @Environment(\.theme) private var theme
    @Environment(\.isEnabled) private var enabled
    var prominent = false
    var color: Color?
    /// Less room at the sides: the mixer's top line, which holds six buttons since 2026-11-20.
    var compact = false

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.panel(12, .semibold))
            .foregroundColor(prominent ? theme.onText : theme.text)
            .lineLimit(1).fixedSize()
            .padding(.horizontal, compact ? 10 : 15).padding(.vertical, 6)
            .keycap(on: prominent, color: color ?? theme.link, radius: 14)
            .opacity(enabled ? (configuration.isPressed ? 0.7 : 1) : 0.4)
    }
}

/// A view's own small state. It is SwiftUI's State underneath; written this way
/// because the Command Line Tools' compiler can't expand `@State` by itself
/// (that needs a plug-in only Xcode carries), and this project builds without Xcode.
@propertyWrapper
struct Kept<Value>: DynamicProperty {
    private var state: State<Value>

    init(wrappedValue: Value) { state = State(initialValue: wrappedValue) }

    var wrappedValue: Value {
        get { state.wrappedValue }
        nonmutating set { state.wrappedValue = newValue }
    }

    var projectedValue: Binding<Value> { state.projectedValue }
}

// MARK: 1.1: each section its own metal (the user's choice, 2026-11-27)
//
// The three mixer sections are told apart by their metal and their fader caps: INPUT a lighter
// grey-blue with black caps, OUTPUT a darker slate with silver caps, MONITOR OUT dark with red
// caps. Depth stays cheap: gradients, one-point edges, and soft shadows made of a few
// see-through copies of a shape, never a blur. The brushed grain is one picture, drawn once
// when first needed and kept; nothing of it is drawn again when a meter moves.

extension Color {
    /// 0xRRGGBB.
    init(hex: UInt32, opacity: Double = 1) {
        self.init(red: Double((hex >> 16) & 0xff) / 255, green: Double((hex >> 8) & 0xff) / 255,
                  blue: Double(hex & 0xff) / 255, opacity: opacity)
    }
}

/// A fader cap's material.
struct CapLook {
    let top: Color, bottom: Color   // its body, lit from above
    let shine: Color                // the highlight on its top face
    let ridgeDark: Color, ridgeLight: Color
    let mark: Color                 // the line across its middle
    let edge: Color

    /// INPUT: black, with a white line.
    static let black = CapLook(top: Color(hex: 0x4a4e55), bottom: Color(hex: 0x0b0c0e), shine: Color.white.opacity(0.30),
                               ridgeDark: Color.black.opacity(0.85), ridgeLight: Color.white.opacity(0.16),
                               mark: Color(hex: 0xf4f6f8), edge: Color.black)
    /// OUTPUT: silver.
    static let silver = CapLook(top: Color(hex: 0xf6f8fa), bottom: Color(hex: 0x8c96a2), shine: Color.white.opacity(0.70),
                                ridgeDark: Color(hex: 0x56606c, opacity: 0.85), ridgeLight: Color.white.opacity(0.75),
                                mark: Color(hex: 0x101418), edge: Color(hex: 0x2a3038))
    /// MONITOR OUT: red.
    static let red = CapLook(top: Color(hex: 0xf0574a), bottom: Color(hex: 0x86140f), shine: Color.white.opacity(0.38),
                             ridgeDark: Color(hex: 0x3e0604, opacity: 0.80), ridgeLight: Color.white.opacity(0.22),
                             mark: Color(hex: 0xfff4f0), edge: Color(hex: 0x2a0402))
}

/// How one section of the mixer looks.
struct SectionLook {
    let plateTop: Color, plateBottom: Color   // its metal
    let laneTop: Color, laneBottom: Color     // a channel's lane, pressed a shade into the plate
    let text: Color, dim: Color, tick: Color  // words and scale marks on it
    let edgeLight: Color, edgeDark: Color     // its bevel
    let keyTop: Color, keyBottom: Color, keyText: Color // a key that is off
    let arc: Color                            // a knob's lit arc
    let grain: Double                         // how strong the brushed grain is
    let cap: CapLook
}

extension SectionLook {
    private static func plate(_ top: UInt32, _ bottom: UInt32, lane: (UInt32, UInt32), text: UInt32, dim: UInt32, tick: Double,
                              light: Double, key: (UInt32, UInt32), keyText: UInt32, arc: UInt32, grain: Double,
                              cap: CapLook) -> SectionLook {
        let darkWords = Color(hex: text).isDarkish
        return SectionLook(plateTop: Color(hex: top), plateBottom: Color(hex: bottom),
                           laneTop: Color(hex: lane.0), laneBottom: Color(hex: lane.1),
                           text: Color(hex: text), dim: Color(hex: dim),
                           tick: (darkWords ? Color.black : Color.white).opacity(tick),
                           edgeLight: Color.white.opacity(light), edgeDark: Color.black.opacity(0.62),
                           keyTop: Color(hex: key.0), keyBottom: Color(hex: key.1), keyText: Color(hex: keyText),
                           arc: Color(hex: arc), grain: grain, cap: cap)
    }

    // The dark look: "C blue steel", the user's choice of three (2026-11-27). [MONITOR OUT,
    // INPUT, OUTPUT], as the engine numbers the groups.
    static let darkBlue: [SectionLook] = [
        plate(0x16181c, 0x0c0d10, lane: (0x0e0f12, 0x121316), text: 0xeef0f3, dim: 0x959ba5, tick: 0.30,
              light: 0.13, key: (0x2c2f35, 0x1d1f24), keyText: 0xa0a7b1, arc: 0xff8a7c, grain: 0.30, cap: .red),
        plate(0x55708f, 0x435a77, lane: (0x445b78, 0x4c6483), text: 0xf6f9fc, dim: 0xd5e2f2, tick: 0.42,
              light: 0.30, key: (0x66819f, 0x506986), keyText: 0xe2ebf6, arc: 0xe8f3ff, grain: 0.70, cap: .black),
        plate(0x223247, 0x182537, lane: (0x172334, 0x1c293b), text: 0xeef3f8, dim: 0xa3b5cb, tick: 0.32,
              light: 0.16, key: (0x324560, 0x24354c), keyText: 0xafc0d5, arc: 0xd8e8ff, grain: 0.42, cap: .silver),
    ]
    /// The light look: the same three steps, lighter, on the putty window.
    static let lightSlate: [SectionLook] = [
        plate(0x34373c, 0x26282c, lane: (0x24262a, 0x2b2d31), text: 0xf1f2f4, dim: 0xb0b5bc, tick: 0.32,
              light: 0.20, key: (0x4a4d53, 0x37393e), keyText: 0xc0c5cc, arc: 0xff9a8c, grain: 0.50, cap: .red),
        plate(0xbcc6d2, 0xa4afbd, lane: (0xa5b0be, 0xb0bbc8), text: 0x141a22, dim: 0x333d4a, tick: 0.42,
              light: 0.60, key: (0xd2dae3, 0xb6c0cd), keyText: 0x2a3340, arc: 0x154c95, grain: 0.85, cap: .black),
        plate(0x66727f, 0x535e6b, lane: (0x515c69, 0x5a6572), text: 0xf3f5f8, dim: 0xd0d8e2, tick: 0.40,
              light: 0.28, key: (0x7b8794, 0x626d7a), keyText: 0xdde3ea, arc: 0xe4efff, grain: 0.65, cap: .silver),
    ]

    /// A plain bar plate in a dark look's colours.
    private static func darkBar(_ top: UInt32, _ bottom: UInt32) -> SectionLook {
        plate(top, bottom, lane: (0x000000, 0x000000), text: 0xeef1f5, dim: 0xa6b2c2, tick: 0.3, light: 0.16,
              key: (0x3a4250, 0x2a313c), keyText: 0xb2bdcb, arc: 0xe6f2ff, grain: 0.38, cap: .silver)
    }

    /// The dark look's bar below the mixer: navy steel, as the blue steel sections.
    static let darkBlueBar = darkBar(0x1e2b3d, 0x152131)

    /// The bar below the mixer: a plain brushed plate in the look's own colours.
    static func bar(_ t: Theme) -> SectionLook {
        SectionLook(plateTop: t.raisedTop, plateBottom: t.raisedBottom, laneTop: t.insetTop, laneBottom: t.insetBottom,
                    text: t.text, dim: t.dim, tick: t.line, edgeLight: t.edgeLight, edgeDark: t.edgeDark,
                    keyTop: t.keyTop, keyBottom: t.keyBottom, keyText: t.dim, arc: t.glow,
                    grain: t.scheme == .dark ? 0.55 : 0.75, cap: .silver)
    }
}

extension Color {
    /// Rough: is this a dark colour (for choosing light or dark marks on it)?
    var isDarkish: Bool {
        guard let c = NSColor(self).usingColorSpace(.sRGB) else { return false }
        return 0.299 * c.redComponent + 0.587 * c.greenComponent + 0.114 * c.blueComponent < 0.5
    }
}

extension Theme {
    /// The section a strip of `group` sits in.
    func section(_ group: Int) -> SectionLook {
        let set = scheme == .dark ? SectionLook.darkBlue : SectionLook.lightSlate
        return set[max(0, min(set.count - 1, group))]
    }
    /// The bar below the mixer.
    var bar: SectionLook { scheme == .dark ? SectionLook.darkBlueBar : SectionLook.bar(self) }
}

private struct SectionKey: EnvironmentKey { static let defaultValue: SectionLook? = nil }

extension EnvironmentValues {
    /// The section a control sits in; nil outside the mixer's sections.
    var section: SectionLook? {
        get { self[SectionKey.self] }
        set { self[SectionKey.self] = newValue }
    }
}

// MARK: brushed metal

/// The grain of brushed metal: fine streaks along the width, light and dark. One picture,
/// made once and kept, that tiles without a seam; laid over a plate's colour.
enum BrushedGrain {
    static let tile: CGImage? = make(width: 1024, height: 192, seed: 0x5eed_101)

    /// Same every time (a fixed seed), so the pictures don't change from one drawing to the next.
    static func make(width w: Int, height h: Int, seed: UInt64) -> CGImage? {
        var state = seed
        func random() -> Double { // -1 ... 1
            state = state &* 6364136223846793005 &+ 1442695040888963407
            return Double(Int64(bitPattern: state >> 11) % 1_000_000) / 1_000_000 * 2 - 1
        }
        var pixels = [UInt8](repeating: 0, count: w * h * 4)
        var line = [Double](repeating: 0, count: 2 * w)
        for y in 0..<h {
            // A slowly wandering streak along the row, twice as long as the tile; its two halves
            // are blended so the row's end meets its start.
            let base = random() * 0.55
            var s = 0.0
            for x in 0..<(2 * w) {
                s = s * 0.985 + random() * 0.11
                line[x] = s
            }
            for x in 0..<w {
                let t = Double(x) / Double(w)
                let v = base + (line[x] * t + line[x + w] * (1 - t)) + random() * 0.20
                let a = min(1, abs(v)) * 0.16 * 255
                let c = v > 0 ? a : 0 // premultiplied: white, or black
                let i = (y * w + x) * 4
                pixels[i] = UInt8(c); pixels[i + 1] = UInt8(c); pixels[i + 2] = UInt8(c); pixels[i + 3] = UInt8(a)
            }
        }
        guard let provider = CGDataProvider(data: Data(pixels) as CFData) else { return nil }
        return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                       space: CGColorSpaceCreateDeviceRGB(),
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
}

/// The grain over a shape, at a strength.
struct Grain<S: Shape>: View {
    let shape: S
    let strength: Double

    var body: some View {
        if let tile = BrushedGrain.tile, strength > 0 {
            Image(decorative: tile, scale: 2).resizable(resizingMode: .tile)
                .opacity(strength).clipShape(shape).allowsHitTesting(false)
        }
    }
}

/// A soft shadow made of a few see-through copies of a shape, each a little larger and lower.
/// No blur: it is drawn like any other shape.
struct SoftShadow<S: InsettableShape>: View {
    let shape: S
    var depth: CGFloat = 3
    var opacity: Double = 0.45
    var steps = 4

    var body: some View {
        ZStack {
            ForEach(0..<steps, id: \.self) { i in
                let k = CGFloat(i + 1) / CGFloat(steps)
                shape.inset(by: -k * depth * 0.7).fill(Color.black.opacity(opacity / Double(steps)))
                    .offset(y: k * depth)
            }
        }
        .allowsHitTesting(false)
    }
}

/// A plate of brushed metal with bevelled edges and a soft shadow on what is behind it.
struct MetalPlate: ViewModifier {
    let look: SectionLook
    let radius: CGFloat
    var shadow = true

    func body(content: Content) -> some View {
        let shape = RoundedRectangle(cornerRadius: radius)
        return content.background(ZStack {
            if shadow { SoftShadow(shape: shape, depth: 4, opacity: 0.5) }
            shape.fill(LinearGradient(colors: [look.plateTop, look.plateBottom], startPoint: .top, endPoint: .bottom))
            Grain(shape: shape, strength: look.grain)
            // A faint sheen across the plate, as light falls on metal.
            shape.fill(LinearGradient(stops: [
                .init(color: Color.white.opacity(0.07), location: 0), .init(color: Color.white.opacity(0), location: 0.35),
                .init(color: Color.white.opacity(0.04), location: 0.7), .init(color: Color.white.opacity(0), location: 1),
            ], startPoint: .topLeading, endPoint: .bottomTrailing))
            // The bevel: lit along the top and left, shaded along the bottom and right, and a fine
            // dark fold just inside.
            shape.strokeBorder(LinearGradient(colors: [look.edgeLight, look.edgeLight.opacity(0.4), look.edgeDark],
                                              startPoint: .topLeading, endPoint: .bottomTrailing), lineWidth: 1.5)
            shape.inset(by: 1.5).strokeBorder(Color.black.opacity(0.22), lineWidth: 0.5)
        }
        // Flattened into one picture, kept until the plate's size or colours change: a meter
        // moving over it doesn't draw the grain, the sheen and the bevel again.
        .drawingGroup())
    }
}

/// A channel's lane: pressed a shade into its plate, its edges engraved.
struct EngravedLane: ViewModifier {
    let look: SectionLook
    let radius: CGFloat

    func body(content: Content) -> some View {
        let shape = RoundedRectangle(cornerRadius: radius)
        return content
            .background(shape.fill(LinearGradient(colors: [look.laneTop, look.laneBottom], startPoint: .top, endPoint: .bottom)))
            .overlay(shape.strokeBorder(LinearGradient(colors: [Color.black.opacity(0.42), look.edgeLight.opacity(0.9)],
                                                       startPoint: .top, endPoint: .bottom), lineWidth: 1))
    }
}

extension View {
    func metalPlate(_ look: SectionLook, radius: CGFloat = 14, shadow: Bool = true) -> some View {
        modifier(MetalPlate(look: look, radius: radius, shadow: shadow))
    }
    func engravedLane(_ look: SectionLook, radius: CGFloat = 8) -> some View {
        modifier(EngravedLane(look: look, radius: radius))
    }
}
