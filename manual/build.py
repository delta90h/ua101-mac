#!/usr/bin/env python3
"""The UA-101 Control manual: checks the pages, then builds the help book and the
page the PDF is printed from. Run by `make manual`; the PDF itself is printed by makepdf.swift.

  manual/build.py OUT_DIR

Writes OUT_DIR/UA-101 Control.help (an Apple help book, with its search index) and
OUT_DIR/print/manual.html (every page in one, for the PDF). Exits with 1, writing nothing that
counts, if a check fails: a menu or button name of the app missing from the manual, an old name
still in it, a picture or a link that doesn't exist.

Reads only: manual/pages/*.html, manual/style.css, app/PanelApp.swift, app/MixerViews.swift,
app/Theme.swift and the end-user pictures in manual/pictures (made by
`make user-pictures`). Uses sips (crops, smaller copies) and hiutil (the index), both part of
macOS. Nothing here touches the UA-101.
"""
import html, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGES = os.path.join(ROOT, "manual", "pages")
PICTURES = os.path.join(ROOT, "manual", "pictures")
BOOK = "UA-101 Control.help"
BOOK_ID = "local.ua101.control.help"   # = CFBundleHelpBookName in app/Info.plist
BOOK_TITLE = "UA-101 Control Help"
INDEX_NAME = "UA-101 Control"
WIDEST = 1600                          # pictures wider than this are made smaller copies

# Old names (before version 1.0) and words of the developer build: never in the manual.
FORBIDDEN = ["Load Memory", "Store Memory", "Restore Backup", "Allow Load", "Allow Changes",
             "go-ahead", "sudo", "Pretend", "One Exact Value", "Store into"]

# Crops of the mixer pictures (2594 × 1288 pixels): name, source, x, y, width, height.
CROPS = [
    ("light-ready", "02-mixer-changes-allowed.png", 2040, 1110, 528, 110),
    ("light-locked", "15-mixer-changes-locked.png", 2040, 1110, 528, 110),
    ("light-none", "14-mixer-no-ua101.png", 2040, 1110, 528, 110),
    ("bottom-bar", "02-mixer-changes-allowed.png", 26, 1085, 2542, 180),
    ("top-buttons", "02-mixer-changes-allowed.png", 1180, 20, 1390, 88),
    ("strip", "02-mixer-changes-allowed.png", 40, 172, 222, 880),
]

failures = []


def fail(text):
    failures.append(text)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def plain(text):
    """The words of a page, without tags, as a reader sees them."""
    text = re.sub(r"<!--.*?-->", " ", text, flags=re.S)
    text = re.sub(r"<[^>]+>", "", text)
    return re.sub(r"\s+", " ", html.unescape(text))


def load_pages():
    pages = []
    for name in sorted(os.listdir(PAGES)):
        if not name.endswith(".html"):
            continue
        src = read(os.path.join(PAGES, name))
        title = re.search(r"<!-- title: (.+?) -->", src)
        summary = re.search(r"<!-- summary: (.+?) -->", src)
        if not title or not summary:
            fail(f"{name}: no title or summary line")
        if len(re.findall(r"<h1>", src)) != 1:
            fail(f"{name}: not exactly one <h1>")
        pages.append({"file": name, "stem": name[:-5], "src": src,
                      "title": title.group(1) if title else name, "summary": summary.group(1) if summary else ""})
    if not pages or not pages[0]["file"].startswith("01-"):
        fail("no first page 01-…")
    return pages


def literal_strings(code):
    """The menu and button titles of the end-user app, from its source: every literal of
    Button("…"), Toggle("…"), Menu("…"), Section(…Heading) in PanelCommands, the View menu's
    sizes and looks, and the mixer's top-line buttons. Not what only the developer build has."""
    names = set()
    app = read(os.path.join(ROOT, "app", "PanelApp.swift"))
    start = app.index("struct PanelCommands")
    body = app[start:]
    # Drop the developer build's parts.
    body = re.sub(r"#if !UA101_USER.*?#endif", "", body, flags=re.S)
    body = re.sub(r"if model\.changesByThemselves[^{]*\{(.*?)\} else \{.*?\n            \}", r"\1", body, flags=re.S)
    for m in re.finditer(r'(?:Button|Toggle|Menu|Picker)\("([^"\\]+)"', body):
        names.add(m.group(1))
    for m in re.finditer(r'static let (?:memoriesHeading|backupsHeading) = "([^"]+)"', app):
        names.add(m.group(1))
    for m in re.finditer(r'\("([^"]+)", (?:1|0\.8|0\.6)\)', app):
        names.add(m.group(1))
    names.add("Reset the mixer")                      # the Reset Mixer alert's button
    theme = read(os.path.join(ROOT, "app", "Theme.swift"))
    look = theme[theme.index("enum Look"):]
    look = look[:look.index("}\n    }")]
    for m in re.finditer(r'return "([^"]+)"', look):
        names.add(m.group(1))
    mixer = read(os.path.join(ROOT, "app", "MixerViews.swift"))
    m = re.search(r"static let lineButtons = \[(.*?)\]", mixer)
    for b in re.findall(r'"([^"]+)"', m.group(1) if m else ""):
        names.add(b)
    names.discard("Cancel")
    return names


def check(pages):
    words = " ".join(plain(p["src"]) for p in pages)
    names = literal_strings(None)
    if len(names) < 40:
        fail(f"only {len(names)} menu names found in the app's source: the reading of it broke")
    for n in sorted(names):
        if n not in words:
            fail(f"the app's “{n}” is not in the manual")
    for w in FORBIDDEN:
        for p in pages:
            if w in plain(p["src"]):
                fail(f"{p['file']}: old or developer word “{w}”")
    stems = {p["file"] for p in pages}
    anchors = {}
    for p in pages:
        anchors[p["file"]] = set(re.findall(r'id="([^"]+)"', p["src"]))
    crops = {c[0] + ".png" for c in CROPS}
    for p in pages:
        for src in re.findall(r'<img [^>]*src="pictures/([^"]+)"', p["src"]):
            if src not in crops and not os.path.exists(os.path.join(PICTURES, src)):
                fail(f"{p['file']}: picture {src} doesn't exist (make user-pictures)")
        for alt in re.findall(r"<img (?![^>]*alt=)", p["src"]):
            fail(f"{p['file']}: a picture without words (alt)")
        for href in re.findall(r'href="([^"]+)"', p["src"]):
            target, _, anchor = href.partition("#")
            if target not in stems:
                fail(f"{p['file']}: link to {href}, no such page")
            elif anchor and anchor not in anchors[target]:
                fail(f"{p['file']}: link to {href}, no such place in it")
    return names


def sips(*args):
    r = subprocess.run(["sips", *args], capture_output=True, text=True)
    if r.returncode != 0:
        fail("sips " + " ".join(args) + ": " + r.stderr.strip())


def width(path):
    r = subprocess.run(["sips", "-g", "pixelWidth", path], capture_output=True, text=True)
    m = re.search(r"pixelWidth: (\d+)", r.stdout)
    return int(m.group(1)) if m else 0


def pictures(pages, into):
    os.makedirs(into, exist_ok=True)
    used = set()
    for p in pages:
        used.update(re.findall(r'<img [^>]*src="pictures/([^"]+)"', p["src"]))
    for name, source, x, y, w, h in CROPS:
        if name + ".png" in used:
            out = os.path.join(into, name + ".png")
            shutil.copyfile(os.path.join(PICTURES, source), out)
            sips("-c", str(h), str(w), "--cropOffset", str(y), str(x), out)
    for name in sorted(used):
        out = os.path.join(into, name)
        if not os.path.exists(out):
            shutil.copyfile(os.path.join(PICTURES, name), out)
        if width(out) > WIDEST:
            sips("--resampleWidth", str(WIDEST), out)


def contents_list(pages, link):
    items = "\n".join(f'  <li><a href="{link(p)}">{html.escape(p["title"])}</a></li>' for p in pages[1:])
    return f'<ol class="contents">\n{items}\n</ol>'


def help_page(p, pages, i):
    body = p["src"]
    if "<!-- contents -->" in body:
        body = body.replace("<!-- contents -->", contents_list(pages, lambda q: q["file"]))
    first = pages[0]
    prev_link = f'<a href="{pages[i - 1]["file"] if i > 1 else "index.html"}">← {html.escape(pages[i - 1]["title"])}</a>' if i > 0 else "<span></span>"
    next_link = f'<a href="{pages[i + 1]["file"]}">{html.escape(pages[i + 1]["title"])} →</a>' if i + 1 < len(pages) else "<span></span>"
    top = f'<nav><a href="index.html">{BOOK_TITLE}: Contents</a></nav>' if i > 0 else ""
    apple_title = f'<meta name="AppleTitle" content="{BOOK_TITLE}">\n' if i == 0 else ""
    body = body.replace(f'href="{first["file"]}', 'href="index.html')
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
{apple_title}<meta name="description" content="{html.escape(p['summary'])}">
<meta name="viewport" content="width=device-width">
<title>{html.escape(p['title'])}</title>
<link rel="stylesheet" href="style.css">
</head>
<body>
<a name="{p['stem']}"></a>
{top}
{body}
<nav class="bottom">{prev_link}{next_link}</nav>
</body>
</html>
"""


def build_help(pages, out):
    book = os.path.join(out, BOOK)
    if os.path.exists(book):
        shutil.rmtree(book)
    lproj = os.path.join(book, "Contents", "Resources", "en.lproj")
    os.makedirs(lproj)
    shutil.copyfile(os.path.join(ROOT, "manual", "style.css"), os.path.join(lproj, "style.css"))
    pictures(pages, os.path.join(lproj, "pictures"))
    for i, p in enumerate(pages):
        name = "index.html" if i == 0 else p["file"]
        with open(os.path.join(lproj, name), "w", encoding="utf-8") as f:
            f.write(help_page(p, pages, i))
    with open(os.path.join(book, "Contents", "Info.plist"), "w", encoding="utf-8") as f:
        f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>en</string>
	<key>CFBundleIdentifier</key><string>{BOOK_ID}</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>{BOOK_TITLE}</string>
	<key>CFBundlePackageType</key><string>BNDL</string>
	<key>CFBundleShortVersionString</key><string>0.5</string>
	<key>CFBundleSignature</key><string>hbwr</string>
	<key>CFBundleVersion</key><string>5</string>
	<key>HPDBookAccessPath</key><string>index.html</string>
	<key>HPDBookIndexPath</key><string>{INDEX_NAME}.helpindex</string>
	<key>HPDBookTitle</key><string>{BOOK_TITLE}</string>
	<key>HPDBookType</key><string>3</string>
</dict>
</plist>
""")
    # The search index, both kinds (older and newer Help Viewer), written beside the pages.
    for kind, ext in (("lsm", "helpindex"), ("corespotlight", "cshelpindex")):
        tmp = os.path.join(out, f"{INDEX_NAME}.{ext}")
        r = subprocess.run(["hiutil", "-I", kind, "-Caf", tmp, "-l", "en", "-s", "en", lproj],
                           capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(tmp):
            fail(f"hiutil {kind}: {r.stderr.strip()}")
        else:
            shutil.move(tmp, os.path.join(lproj, f"{INDEX_NAME}.{ext}"))
    return book


def build_print(pages, out):
    where = os.path.join(out, "print")
    if os.path.exists(where):
        shutil.rmtree(where)
    os.makedirs(where)
    shutil.copyfile(os.path.join(ROOT, "manual", "style.css"), os.path.join(where, "style.css"))
    pictures(pages, os.path.join(where, "pictures"))

    def local(src):
        src = re.sub(r'href="([0-9][^"#]*)\.html#([^"]+)"', r'href="#\2"', src)
        return re.sub(r'href="([0-9][^"#]*)\.html"', r'href="#\1"', src)

    sections = []
    for p in pages:
        body = p["src"].replace("<!-- contents -->", contents_list(pages, lambda q: "#" + q["stem"]))
        sections.append(f'<section class="page" id="{p["stem"]}">\n{local(body)}\n</section>')
    title = ('<section class="page titlepage"><h1>UA-101 Control</h1>'
             '<p>The user manual of the control app for the Edirol UA-101</p>'
             '<p>English · for the end-user app, version 0.5</p></section>')
    with open(os.path.join(where, "manual.html"), "w", encoding="utf-8") as f:
        f.write(f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>UA-101 Control Manual</title>
<link rel="stylesheet" href="style.css"></head>
<body>
{title}
{chr(10).join(sections)}
</body></html>
""")
    return os.path.join(where, "manual.html")


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    pages = load_pages()
    names = check(pages)
    if failures:
        for f in failures:
            print("manual: FAIL " + f)
        return 1
    book = build_help(pages, out)
    page = build_print(pages, out)
    if failures:
        for f in failures:
            print("manual: FAIL " + f)
        return 1
    print(f"manual: {len(pages)} pages, {len(names)} menu and button names all in it, no old names")
    print(f"manual: help book {os.path.relpath(book, ROOT)}")
    print(f"manual: page for the PDF {os.path.relpath(page, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
