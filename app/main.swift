// UA-101 Control: the control panel for the Edirol UA-101.
//
//   ua101panel               opens the windows: the pretend unit, or the UA-101
//   ua101panel --selftest    drives the panel against the pretend unit and a stand-in for the
//                            helper, no windows; it never connects to the installed helper
//   ua101panel --render DIR  draws every window off screen into PNG files in DIR (the same)
//
// The end-user build (Build.swift) has neither option: it opens the windows on the UA-101
// (and keeps only --quit-after N, the build's own check). The pictures build has only --render.
//
// The real unit is reached only through the driver's helper, which passes on only the
// changes on its list. Nothing in this program makes sound.
import AppKit
import SwiftUI

let arguments = CommandLine.arguments
#if UA101_PICTURES
if let i = arguments.firstIndex(of: "--render"), i + 1 < arguments.count {
    let app = NSApplication.shared
    app.setActivationPolicy(.prohibited) // no Dock icon, no window on screen
    let written = renderUserWindows(into: arguments[i + 1])
    for path in written { print(path) }
    exit(written.isEmpty ? 1 : 0)
}
print("the pictures build only draws: ua101panel --render DIR")
exit(2)
#else
#if !UA101_USER
if arguments.contains("--selftest") {
    exit(runSelfTest(verbose: arguments.contains("--verbose")))
} else if let i = arguments.firstIndex(of: "--render"), i + 1 < arguments.count {
    let app = NSApplication.shared
    app.setActivationPolicy(.prohibited) // no Dock icon, no window on screen
    let written = renderAllWindows(into: arguments[i + 1])
    for path in written { print(path) }
    exit(written.isEmpty ? 1 : 0)
}
#endif
PanelApp.main()
#endif
