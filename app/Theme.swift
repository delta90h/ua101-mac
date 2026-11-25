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

    /// The red light inside an optical socket.
    var optical: Color { scheme == .dark ? Color(red: 1.00, green: 0.24, blue: 0.20) : Color(red: 0.90, green: 0.14, blue: 0.12) }

    // Round 5: four clear steps: well darkest, then window, plate, key.
    // 2026-11-14, a design choice: steel blue, the blue of the app icon's panel, in place of
    // charcoal ("3 steel blue"); same steps, the lamps, meters and text unchanged.
    static let dark = Theme(
        name: "dark", scheme: .dark,
        window: Color(red: 0.056, green: 0.098, blue: 0.154),
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

/// A key: raised when off, lit in its colour when on. `flagged` rings it red.
struct KeyCap: ViewModifier {
    @Environment(\.theme) private var theme
    let on: Bool
    let color: Color
    let radius: CGFloat
    var flagged = false

    func body(content: Content) -> some View {
        let shape = RoundedRectangle(cornerRadius: radius)
        return content
            .background(shape.fill(LinearGradient(
                colors: on ? [color, color] : [theme.keyTop, theme.keyBottom], startPoint: .top, endPoint: .bottom)))
            // The shine on a lit key.
            .overlay(shape.fill(LinearGradient(
                colors: [Color.white.opacity(on ? 0.34 : 0), Color.white.opacity(0)], startPoint: .top, endPoint: .center)))
            .overlay(shape.strokeBorder(
                LinearGradient(colors: [on ? Color.white.opacity(0.55) : theme.edgeLight, theme.edgeDark],
                               startPoint: .top, endPoint: .bottom), lineWidth: 1))
            .overlay(shape.strokeBorder(flagged ? theme.bad : Color.clear, lineWidth: 2))
    }
}

extension View {
    func raised(_ radius: CGFloat = 6) -> some View { modifier(RaisedPlate(radius: radius)) }
    func inset(_ radius: CGFloat = 6, flagged: Bool = false) -> some View { modifier(InsetWell(radius: radius, flagged: flagged)) }
    func keycap(on: Bool, color: Color, radius: CGFloat = 5, flagged: Bool = false) -> some View {
        modifier(KeyCap(on: on, color: color, radius: radius, flagged: flagged))
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
                Circle().fill(RadialGradient(colors: [c.opacity(0.50), c.opacity(0)], center: .center,
                                             startRadius: size * 0.35, endRadius: size * 0.95))
                Circle().fill(RadialGradient(colors: [Color.white.opacity(0.95), c], center: UnitPoint(x: 0.42, y: 0.36),
                                             startRadius: 0, endRadius: size * 0.5))
                    .frame(width: size, height: size)
            } else {
                Circle().fill(theme.lampOff).frame(width: size, height: size)
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
