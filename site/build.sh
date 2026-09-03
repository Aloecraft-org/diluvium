#!/usr/bin/env bash
# build.sh — build diluvium.aloecraft.org into a staging directory.
#
#   ./site/build.sh                 # build into site/_out
#   ./site/build.sh --out /tmp/x    # build somewhere else
#   ./site/build.sh --check         # verify the template and sources, build nothing
#
# This is the site contract every Aloecraft repo implements, so the
# deployment tooling (lk_web in lk2) can stage any of them without knowing
# what they are. The reference copy is doc/CONTRACT.md in
# aloecraft-software-portal:
#
#   * it builds into --out, defaulting to site/_out (gitignored)
#   * it is offline and hermetic: no network, no credentials, no npm
#   * it is idempotent: same inputs, same bytes
#   * it NEVER deploys. Getting the tree onto a server is the deployment
#     tooling's job; a repo that rsyncs to a host of its own is a second
#     deploy path to keep working and a second way to take a vhost down.
#
# Python 3 is the only thing this needs. The previous site was a webpack
# build, which is an `npm ci` -- a network fetch -- on every staging run;
# xterm.js and Prism are vendored under static/assets/vendor/ instead, and
# render.py does the one job the bundler still had, content-hashing the
# asset URLs. node is for the browser smoke test in test/, which is a test
# and not a build step.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(dirname "$here")"
out="$repo/site/_out"
check_only=0

while [ $# -gt 0 ]; do
    case "$1" in
        --out)   out="$2"; shift 2 ;;
        --check) check_only=1; shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# A template that lost a load-bearing detail renders fine and quietly stops
# working, so it is refused before anything is written.
python3 "$here/check.py"
[ "$check_only" = 1 ] && exit 0

# --delete semantics without the flag: a stale file from a previous build is
# a file the deployment tooling would faithfully ship.
rm -rf "$out"
python3 "$here/render.py" --out "$out"

echo "   -> $out"
