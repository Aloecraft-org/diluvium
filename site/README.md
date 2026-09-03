# site/

`diluvium.aloecraft.org`: what the language adds, a working interpreter in
the page, the runtime (DRT) that shares the page, and the way in to
everything else. Built by the site contract every Aloecraft repo implements
(the reference copy is `doc/CONTRACT.md` in
[aloecraft-software-portal](https://github.com/Aloecraft-org/aloecraft-software-portal)):

```sh
./site/build.sh                 # -> site/_out
./site/build.sh --out /tmp/x
./site/build.sh --check         # verify the template and sources, build nothing
```

Offline, hermetic, idempotent, and it never deploys. `lk_web` in lk2 clones
this repo, runs `./site/build.sh`, and ships `site/_out` to the vhost; this
directory is what it finds. Python 3 is the only tool the build needs.

This used to be its own repository, `Aloecraft-org/diluvium-www`, and a
webpack build. It was folded in here with its history, because the page
drives the runtime built from this tree and because `npm ci` on every
staging run is a network fetch the contract forbids.

```
build.sh              the contract entry point
check.py              guards the template's load-bearing details; runs in build.sh and CI
render.py             static/ + template -> _out, with asset URLs content-hashed
site.json             what this site is, for the portal and the manifest
start.sh              the installer, served at /start
template/index.html   the page
static/
  assets/app.js       wiring
  assets/repl/        WASI shim, runtime loader, line editor
  assets/release.js   reading the two release mirrors
  assets/prism-diluvium.js   Lua highlighting, extended for Diluvium
  assets/styles.css   dark default, light behind [data-theme="light"]
  assets/vendor/      xterm.js 5.5.0, addon-fit 0.10.0, Prism 1.30.0 (MIT)
  favicon.ico, robots.txt
nginx/diluvium.conf   how the page wants to be served: /start's type, the cache policy
test/smoke.mjs        drives the real page in a real browser
test/stage-mirror.mjs copies enough of the deployed mirrors into _out/ for it
```

## Three projects, one page

DRT shares this landing page: `/drt/` on this vhost is only where its
artifacts live (the second release mirror), and the DRT section and its
install line are here. dollup has its own page at `dollup.aloecraft.org`;
this one carries the family strip under the hero, a section, and the
install line, so a reader arriving for any of the three can see the other
two. Links to the sibling sites and to the software portal are plain
`href`s in the template.

## The interpreter is real

The panel is not a transcript or a simulation. The page fetches
`libdiluvium_wasi.wasm` from the release mirror and drives it through
`repl_eval` — the same entry point the native REPL uses, so continuation
prompts, expression echo and tab completion all come from the runtime
rather than from a guess made in JavaScript.

`doc/repl-reference.html` is the authoritative description of that
contract, and `static/assets/repl/` is adapted from it. When the runtime's
exports or imports change, that file changes first. Three things about the
build are not guessable from outside, and `check.py` refuses a source tree
that forgets any of them:

- It must be `libdiluvium_wasi.wasm`. `diluvium_wasi.wasm` is a command module
  whose `_start` runs the interpreter against stdin, and
  `diluvium_compiler_wasi.wasm` is `luac`.
- Call `__wasm_call_ctors()` first. The module exports that rather than
  `_initialize`, so it is not a WASI reactor and nothing sets up libc for you.
- Drive it with `repl_eval`, not `run_lua`. `run_lua` cannot distinguish
  unfinished input from broken input, and `1 + 1` prints nothing.

## The release mirrors

Nothing here vendors a runtime. `/release/` and `/drt/` are generated on
the server by `script/release_mirror.py` in lk2 from `changelog.json` in
the diluvium and diluvium-drt trees. The page reads each mirror's
`releases.json` for the version badge and the download links, and DRT's
`latest/BUILDINFO.txt` for the connector list, so what the page says a
release carries is read off the artifact rather than written here.
`latest/` is a symlink the mirror maintains, so the kernel URL never needs
updating when a release is cut.

## No bundler, and what replaced it

The page is `template/index.html` plus the files under `static/`, served as
they are: xterm.js and Prism are classic scripts, `app.js` is an ES module
that imports the rest. The one thing webpack still did was content-hash the
asset filenames so a deploy could not leave a browser running last week's
JavaScript against this week's page; `render.py` does that now, stamping
every local `src`/`href` in the template and every `import` between the
modules with `?v=<sha256 prefix>` of the file it names, leaves first. A
changed file is a new URL. `nginx/diluvium.conf` is the policy that relies
on it — `assets/` immutable, `index.html` revalidated on every visit — and
it replaces the `start.conf` and the landing-page half of `cache.conf` that
lk2's `ansible-web.yaml` wrote inline for the webpack layout.

**Dark is the default**, as on dollup.aloecraft.org and the software portal,
with a toggle in the header that switches to light and remembers the
choice in `localStorage` under `aloecraft-theme`. The inline script that
applies a stored choice sits in `<head>` deliberately: it has to run before
first paint, or a light-mode visitor sees a flash of dark on every load.
`prefers-color-scheme` is intentionally *not* consulted. The terminal stays
dark in both themes, the way an editor's terminal pane does.

## Development

```sh
./site/build.sh
cd site
npm install                      # playwright, for the test; nothing else
npm run stage-mirror             # copy the real kernel and indexes into _out/
npm run serve                    # http://localhost:8081/
npm test                         # build must have run; drives the REPL in Chromium
```

Against the deployed mirrors instead of a staged copy, on localhost only:

```
http://localhost:8081/?release=https://diluvium.aloecraft.org/release&drt=https://diluvium.aloecraft.org/drt
```

`/release/` and `/drt/` send `Access-Control-Allow-Origin: *`, so that
works from a dev server. To test what is deployed rather than a local build:

```sh
SITE=https://diluvium.aloecraft.org npm test
```

## Invariants, and getting a design pass done elsewhere

The page carries details a designer has no reason to know about: the
element ids `app.js` fills, the script order that makes `Terminal` exist
before `app.js` runs, the pre-paint theme script, the absence of any
third-party request. Losing one does not look broken — the page renders
fine and quietly stops working — so they are checked mechanically:

```sh
python3 site/check.py                  # template, sources, site.json
python3 site/check.py candidate.html   # a returned file, before adopting it
```

When a file comes back from a design pass:

```sh
python3 site/check.py candidate.html && cp candidate.html site/template/index.html
git diff site/template/index.html
```
