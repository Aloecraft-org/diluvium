#!/usr/bin/env python3
"""render.py — assemble diluvium.aloecraft.org into a directory.

    python3 site/render.py --out site/_out

Called by site/build.sh, the contract entry point. Offline, hermetic and
idempotent: it reads only this checkout and writes only --out, and the
same inputs produce the same bytes -- nothing here stamps a date, and the
one transform it performs is a function of file contents.

What it does, in order:

  1. Copies site/static/ -- the page's own JavaScript and CSS, the vendored
     xterm.js and Prism, the favicon, robots.txt -- into --out.
  2. Stamps every local asset reference with the content hash of the file
     it names: `assets/app.js` becomes `assets/app.js?v=1a2b3c4d`, in the
     template and in every import between the page's own modules. A
     changed file is a new URL, so assets/ can be cached hard
     (site/nginx/diluvium.conf) while index.html itself is revalidated on
     every visit. This is the job webpack's [contenthash] used to do; the
     bundler is gone, the guarantee is not.
  3. Writes index.html from site/template/index.html.
  4. Copies site/start.sh to --out/start: the installer, at the URL the
     one-liner on the page names.
"""

import argparse
import hashlib
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
TEMPLATE = os.path.join(HERE, "template", "index.html")
STATIC = os.path.join(HERE, "static")
INSTALLER = os.path.join(HERE, "start.sh")
INSTALLER_URL_NAME = "start"

# A local reference in the template: src="assets/…" or href="assets/…", and
# the favicon. Sibling paths on the vhost (/release/, /drt/, /lab/, /start),
# absolute URLs and fragment links are not files in this tree and are left
# alone.
ATTR_REF = re.compile(r'\b(src|href)="(assets/[^"?#]+|favicon\.ico)"')

# An import between the page's own modules: `from './x.js'` or
# `import './x.js'`. Relative specifiers only, and vendor/ is never
# rewritten (only hashed), so a minified bundle's internals cannot match
# by accident.
IMPORT_REF = re.compile(r"""(\bfrom\s+|\bimport\s+)(['"])(\.{1,2}/[^'"?]+\.js)\2""")
VENDOR = "assets/vendor/"

HASH_LEN = 8


def sha(data):
    return hashlib.sha256(data).hexdigest()[:HASH_LEN]


# depth: content hashing over the module graph, leaves first
class Stamper:
    """Hash the asset tree under `out`, rewriting each module's imports
    before hashing it, so a module's hash covers the hashes of everything
    it imports and a change anywhere below propagates up to index.html."""

    def __init__(self, out):
        self.out = out
        self.hashes = {}
        self.active = []

    def stamp(self, rel):
        if rel in self.hashes:
            return self.hashes[rel]
        if rel in self.active:
            sys.exit("import cycle: %s" % " -> ".join(self.active + [rel]))
        path = os.path.join(self.out, rel)
        if not os.path.isfile(path):
            sys.exit("%s is referenced but is not a file under site/static/" % rel)
        self.active.append(rel)
        with open(path, "rb") as fh:
            data = fh.read()
        if rel.endswith(".js") and not rel.startswith(VENDOR):
            text = IMPORT_REF.sub(lambda m: self._stamp_import(rel, m),
                                  data.decode("utf-8"))
            data = text.encode("utf-8")
            with open(path, "wb") as fh:
                fh.write(data)
        self.active.pop()
        self.hashes[rel] = sha(data)
        return self.hashes[rel]

    def _stamp_import(self, rel, m):
        dep = os.path.normpath(os.path.join(os.path.dirname(rel), m.group(3)))
        return "%s%s%s?v=%s%s" % (m.group(1), m.group(2), m.group(3),
                                  self.stamp(dep), m.group(2))


def render_page(stamper):
    with open(TEMPLATE, encoding="utf-8") as fh:
        page = fh.read()
    seen = []

    def sub(m):
        seen.append(m.group(2))
        return '%s="%s?v=%s"' % (m.group(1), m.group(2), stamper.stamp(m.group(2)))

    page = ATTR_REF.sub(sub, page)
    if not seen:
        sys.exit("the template references no local assets -- run site/check.py")
    return page


def main():
    ap = argparse.ArgumentParser(description="render diluvium.aloecraft.org")
    ap.add_argument("--out", required=True, help="output directory")
    args = ap.parse_args()
    out = os.path.abspath(args.out)

    shutil.copytree(STATIC, out, dirs_exist_ok=True)
    stamper = Stamper(out)
    page = render_page(stamper)
    with open(os.path.join(out, "index.html"), "w", encoding="utf-8") as fh:
        fh.write(page)
    # copyfile, not copy2: the installer's bytes are what matter, and its
    # mode on the server is nginx's business, not the checkout's.
    shutil.copyfile(INSTALLER, os.path.join(out, INSTALLER_URL_NAME))

    shown = os.path.relpath(out, REPO)
    if shown.startswith(".."):
        shown = out
    print("   stamped %d asset(s); index.html and /%s written -> %s"
          % (len(stamper.hashes), INSTALLER_URL_NAME, shown))


if __name__ == "__main__":
    main()
