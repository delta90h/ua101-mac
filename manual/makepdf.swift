// Prints the manual's one-page HTML (manual/build.py) to a PDF, A4, one task per page
// Run by `make manual`:  makepdf IN.html OUT.pdf
// WebKit in a window that is never shown; no Dock icon. Nothing here touches the UA-101.
import AppKit
import WebKit

final class Printer: NSObject, WKNavigationDelegate {
    let input: URL, output: URL
    let window: NSWindow
    let web: WKWebView
    var status: Int32 = 1

    init(input: URL, output: URL) {
        self.input = input
        self.output = output
        // A4 less the margins, at 72 points to the inch.
        let frame = NSRect(x: 0, y: 0, width: 523, height: 770)
        web = WKWebView(frame: frame, configuration: WKWebViewConfiguration())
        window = NSWindow(contentRect: frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.contentView = web
        super.init()
        web.navigationDelegate = self
        web.loadFileURL(input, allowingReadAccessTo: input.deletingLastPathComponent())
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        // Let the pictures decode before printing.
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { self.print() }
    }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) { finish("load failed: \(error)") }
    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) { finish("load failed: \(error)") }

    func print() {
        let info = NSPrintInfo()
        info.paperSize = NSSize(width: 595.28, height: 841.89)
        info.topMargin = 36; info.bottomMargin = 36; info.leftMargin = 36; info.rightMargin = 36
        info.horizontalPagination = .fit
        info.verticalPagination = .automatic
        info.isHorizontallyCentered = false
        info.isVerticallyCentered = false
        info.jobDisposition = .save
        info.dictionary()[NSPrintInfo.AttributeKey.jobSavingURL] = output
        try? FileManager.default.removeItem(at: output)
        let op = web.printOperation(with: info)
        op.showsPrintPanel = false
        op.showsProgressPanel = false
        op.view?.frame = web.bounds
        op.runModal(for: window, delegate: self, didRun: #selector(printed(_:success:contextInfo:)), contextInfo: nil)
    }

    @objc func printed(_ op: NSPrintOperation, success: Bool, contextInfo: UnsafeMutableRawPointer?) {
        let size = (try? FileManager.default.attributesOfItem(atPath: output.path)[.size] as? Int) ?? 0
        if success && size > 10_000 {
            status = 0
            finish("makepdf: \(output.lastPathComponent), \(size / 1024) KB")
        } else {
            finish("makepdf: printing failed")
        }
    }

    func finish(_ text: String) {
        FileHandle.standardError.write((text + "\n").data(using: .utf8)!)
        exit(status)
    }
}

let args = CommandLine.arguments
guard args.count == 3 else {
    FileHandle.standardError.write("usage: makepdf IN.html OUT.pdf\n".data(using: .utf8)!)
    exit(2)
}
let app = NSApplication.shared
app.setActivationPolicy(.prohibited)
let printer = Printer(input: URL(fileURLWithPath: args[1]).absoluteURL.standardizedFileURL,
                      output: URL(fileURLWithPath: args[2]).absoluteURL.standardizedFileURL)
// Give up after a minute rather than hang a build.
DispatchQueue.main.asyncAfter(deadline: .now() + 60) { printer.finish("makepdf: timed out") }
app.run()
