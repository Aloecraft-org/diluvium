#!/usr/bin/env python3
"""check.py — verify the things a design pass, or a merge, tends to drop.

    python3 site/check.py                  # template, sources, site.json
    python3 site/check.py candidate.html   # a returned page, before adopting it

The page carries load-bearing details a designer has no reason to know
about: the element ids the JavaScript fills, the script order that makes
`Terminal` exist before app.js runs, the pre-paint theme read, and the
three facts about the WASM build that each cost an afternoon to learn.
Losing one does not look broken -- the page renders fine and quietly stops
working -- so this checks mechanically instead of by eye. site/build.sh
runs it before every build, and CI runs it too. Same discipline as
dollup's and the portal's check.py, and the same reason.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TEMPLATE = os.path.join(HERE, "template", "index.html")
STATIC = os.path.join(HERE, "static")
ASSETS = os.path.join(STATIC, "assets")
SITE_JSON = os.path.join(HERE, "site.json")
INSTALLER = os.path.join(HERE, "start.sh")

# (needle, why it matters when it goes missing)
TEMPLATE_MUST_HAVE = [
    ('<html lang="en" data-theme="dark">',
     "the dark default on <html>; the ask is a dark default with a light "
     "toggle, not a system-following one, matching dollup and the portal"),
    ('localStorage.getItem("aloecraft-theme")',
     "the pre-paint theme read; move or lose it and a light-mode visitor "
     "gets a flash of dark on every load"),
    ('id="theme"',
     "the toggle button the footer script looks up by id"),
    ('localStorage.setItem("aloecraft-theme"',
     "the toggle's write; without it the choice never survives a reload"),
    ('id="term"',
     "where xterm mounts; without it the interpreter has nowhere to render"),
    ('id="repl-status"',
     "the status word app.js sets and the smoke test waits on"),
    ('id="repl-reset"', "the Restart button"),
    ('id="examples"', "where the example chips under the terminal are added"),
    ('id="version-badge"', "the Diluvium version, read off the release mirror"),
    ('id="drt-version-badge"', "the DRT version, read off DRT's mirror"),
    ('id="downloads"', "the per-platform Diluvium downloads"),
    ('id="drt-downloads"', "the per-platform DRT downloads"),
    ('id="release-note"', "which Diluvium release the downloads show"),
    ('id="drt-release-note"', "which DRT release the downloads show"),
    ('id="drt-buildinfo"',
     "what the DRT binary carries, read off BUILDINFO.txt on the mirror"),
    ('id="footer-version"', "the release tags in the footer"),
    ('id="install-cmd"', "the one-liner the hero's copy button copies"),
    ('curl -fsSL https://diluvium.aloecraft.org/start | sh',
     "the install one-liner, at the URL start.sh is served from"),
    ('curl -fsSL https://diluvium.aloecraft.org/drt/latest/install.sh | sh',
     "the DRT install one-liner, at the URL DRT's mirror serves it from"),
    ('href="assets/vendor/xterm.css"', "xterm's stylesheet; the terminal is unusable without it"),
    ('href="assets/styles.css"', "the page's stylesheet"),
    ('src="assets/vendor/xterm.js"',
     "the classic script that defines the Terminal global app.js reaches for"),
    ('src="assets/vendor/addon-fit.js"',
     "the classic script that defines the FitAddon global"),
    ('src="assets/vendor/prism-core.min.js" data-manual',
     "Prism must not highlight on DOMContentLoaded: app.js extends the Lua "
     "grammar first and then highlights once. data-manual is what stops it"),
    ('src="assets/vendor/prism-lua.min.js"', "the Lua grammar the Diluvium grammar extends"),
    ('<script type="module" src="assets/app.js">',
     "the page's own code, a module so it can import the REPL pieces "
     "without a bundler"),
    ('href="/lab/"', "the way in to the Lab"),
    ('href="/release/"', "the way in to the release mirror"),
    ('href="/drt/"', "the way in to DRT's release mirror"),
    ('rel="icon"', "the favicon; without it every visit 404s once"),
]

# (regex, why it must stay out)
TEMPLATE_MUST_NOT_HAVE = [
    (r'href="/docs/"',
     "there is no /docs/ on this vhost; the link shipped as a 404 for months. "
     "Link the Guide on GitHub instead"),
    (r'(src|href)="[^"]*\?v=',
     "a pre-stamped URL; render.py stamps at build time and would stamp it twice"),
]

# (earlier, later): script order that has to hold. app.js is a module and
# runs after every classic script whatever its position, but the classic
# scripts run in document order and Prism's grammar needs Prism's core.
TEMPLATE_ORDER = [
    ('src="assets/vendor/prism-core.min.js"', 'src="assets/vendor/prism-lua.min.js"'),
    ('src="assets/vendor/xterm.js"', 'src="assets/vendor/addon-fit.js"'),
]

# Every byte from this origin: no webfont CDN, no analytics, no remote
# script. The page hands out install commands, so it loads nothing from
# anyone else. A webfont CDN is the usual way this gets lost.
NO_THIRD_PARTY = [
    (r"<script[^>]+src=[\"']https?://", "external <script src>"),
    (r"<link[^>]+rel=[\"']stylesheet[\"'][^>]*href=[\"']https?://", "external stylesheet"),
    (r"@import\s+url\(\s*[\"']?https?://", "@import of a remote stylesheet"),
    (r"url\(\s*[\"']?https?://", "remote url() (webfont or image)"),
    (r"<img[^>]+src=[\"']https?://", "remote <img>"),
]

# (file under static/assets, needle, why). The three facts about the WASM
# build from doc/repl-reference.html, plus the highlight call data-manual
# above defers to.
SOURCE_MUST_HAVE = [
    ("release.js", "libdiluvium_wasi.wasm",
     "it must be libdiluvium_wasi.wasm: diluvium_wasi.wasm is a command module "
     "whose _start runs the interpreter against stdin"),
    ("repl/runtime.js", "__wasm_call_ctors",
     "the module exports that rather than _initialize, so nothing initialises "
     "libc unless the page does"),
    ("repl/runtime.js", "repl_eval",
     "run_lua cannot tell unfinished input from broken input, and `1 + 1` "
     "prints nothing"),
    ("app.js", "Prism.highlightAll()",
     "with data-manual on Prism's script tag, nothing else highlights the samples"),
]

# The one URL the installer is built around. Everything else in start.sh
# is an override.
INSTALLER_MUST_HAVE = [
    ("#!/bin/sh", "POSIX sh, because it runs on whatever /bin/sh the machine has"),
    ('BASE="${DILUVIUM_BASE:-https://diluvium.aloecraft.org/release}"',
     "the mirror the installer downloads from by default"),
]

IMPORT_REF = re.compile(r"""(?:\bfrom\s+|\bimport\s+)['"](\.{1,2}/[^'"?]+\.js)['"]""")
VENDOR = os.path.join(ASSETS, "vendor")
STATUSES = ("live", "planned")


def problems_in_template(text):
    bad = []
    for needle, why in TEMPLATE_MUST_HAVE:
        if needle not in text:
            bad.append("%s\n        %s" % (needle, why))
    for pattern, why in TEMPLATE_MUST_NOT_HAVE:
        m = re.search(pattern, text)
        if m:
            bad.append("%s is present\n        %s" % (m.group(0), why))
    for first, then in TEMPLATE_ORDER:
        if first in text and then in text and text.index(first) > text.index(then):
            bad.append("%s must come before %s" % (first, then))
    for pattern, what in NO_THIRD_PARTY:
        if re.search(pattern, text, re.I):
            bad.append("%s -- the page must make no third-party requests" % what)
    return bad


# depth: the sources, which a candidate.html check does not cover
def problems_in_sources():
    bad = []
    for rel, needle, why in SOURCE_MUST_HAVE:
        path = os.path.join(ASSETS, rel)
        if not os.path.isfile(path):
            bad.append("%s is missing" % rel)
            continue
        with open(path, encoding="utf-8") as fh:
            if needle not in fh.read():
                bad.append("%s lost %r\n        %s" % (rel, needle, why))

    # Every import between the page's own modules names a file, checked
    # here so the message says which import rather than which hash.
    for root, dirs, files in os.walk(ASSETS):
        if root.startswith(VENDOR):
            continue
        for name in files:
            if not name.endswith(".js"):
                continue
            path = os.path.join(root, name)
            with open(path, encoding="utf-8") as fh:
                for spec in IMPORT_REF.findall(fh.read()):
                    dep = os.path.normpath(os.path.join(root, spec))
                    if not os.path.isfile(dep):
                        bad.append("%s imports %s, which does not exist"
                                   % (os.path.relpath(path, ASSETS), spec))

    # render.py stamps references in HTML and JS, not in CSS. A url() in the
    # stylesheet would be the one asset reference that never busts.
    css = os.path.join(ASSETS, "styles.css")
    with open(css, encoding="utf-8") as fh:
        if "url(" in fh.read():
            bad.append("styles.css uses url(); render.py does not stamp CSS references, "
                       "so that asset would never cache-bust")

    with open(INSTALLER, encoding="utf-8") as fh:
        installer = fh.read()
    for needle, why in INSTALLER_MUST_HAVE:
        if needle not in installer:
            bad.append("start.sh lost %r\n        %s" % (needle, why))
    return bad


def problems_in_site_json():
    with open(SITE_JSON) as fh:
        d = json.load(fh)
    bad = []
    for key in ("name", "title", "tagline", "vhost", "path", "source"):
        if not d.get(key):
            bad.append("site.json: no %s" % key)
    for c in d.get("channels", []):
        if c.get("status") not in STATUSES:
            bad.append("site.json: channel %r has status %r" % (c.get("title"), c.get("status")))
        if c.get("status") == "live" and not c.get("url"):
            bad.append("site.json: channel %r is live with no url" % c.get("title"))
    return bad


def report(label, bad):
    if bad:
        print("%s:" % label, file=sys.stderr)
        for b in bad:
            print("  FAIL  %s" % b, file=sys.stderr)
        sys.exit(1)
    print("  ok   %s" % label)


def main():
    if len(sys.argv) > 1:
        path = sys.argv[1]
        with open(path, encoding="utf-8") as fh:
            report(path, problems_in_template(fh.read()))
        return
    with open(TEMPLATE, encoding="utf-8") as fh:
        report("%s (%d invariants)" % (os.path.relpath(TEMPLATE, REPO), len(TEMPLATE_MUST_HAVE)),
               problems_in_template(fh.read()))
    report("site/static/assets and site/start.sh", problems_in_sources())
    report("site/site.json", problems_in_site_json())
    print("PASS")


if __name__ == "__main__":
    main()
