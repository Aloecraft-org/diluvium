#!/bin/sh
# deploy/cloud1/docs/deploy.sh — the one file you run for the docs site.
#
#   ./deploy.sh build    npm ci && docusaurus build -> docsite/build
#   ./deploy.sh diff     what sync would change on cloud1. Changes nothing.
#   ./deploy.sh sync     rsync docsite/build to cloud1, under /docs/
#
# RUNS ON THE LAPTOP. /docs/ is static files behind the nginx that already
# serves diluvium.aloecraft.org, so there is no install.sh and nothing to
# stage on the box. Not the shared driver (discofetch's deploy/_driver.sh):
# that one stages a kit under /opt and runs an install.sh there, and this
# kit has neither -- the same reason discofetch/deploy/cloud1/www stands
# alone, and this file is modelled on it.
#
# ── why this is not site/build.sh ──────────────────────────────────────
# `build` is an `npm ci`: a network fetch, and the site contract forbids
# one. site/build.sh is offline, hermetic and idempotent, lk_web runs it
# with no credentials and test.yml's `site` job fails a build that grew a
# fetch -- which is the whole reason the webpack build went away. So the
# landing page at / keeps that contract and is deployed by lk_web, and
# the docs at /docs/ are built and shipped from here. One vhost, two
# builds, and neither calls the other.
#
# ── /docs/ has to be declared in lk2, or it is deleted ─────────────────
# lk_web/deploy.py rsyncs diluvium-www with --delete over
# /var/www/html/diluvium/, and lk_web/sites.py derives what to spare from
# the OTHER rows in manifest/sites.json -- a subtree is safe because a
# sibling entry owns it, not because anyone remembered an --exclude. With
# no `diluvium-docs` row at path /docs/, the next landing-page deploy
# removes everything this script ships. That row is the same device the
# /release/ and /drt/ rows exist to provide. See ../README.md.
set -eu

# ── configurable values ────────────────────────────────────────────────
HOST=${DV_DOCS_HOST:-cloud1}
# The vhost's document root is manifest/domains.json in lk2
# ({"node":"cloud1","domain":"diluvium.aloecraft.org",
#   "www_root":"/var/www/html/diluvium/"}); /docs/ is the subtree this
# kit owns and the ONLY thing it may ever write to. Never the parent:
# that is the landing page, the Lab and two frozen mirrors.
DEST=${DV_DOCS_DEST:-/var/www/html/diluvium/docs/}
# Set DV_DOCS_SUDO=0 for a destination you already own.
SUDO=${DV_DOCS_SUDO:-1}

# --checksum rather than rsync's default size-and-mtime quick check, and
# --chmod because -a would preserve whatever umask the operator's shell
# carried: a tree built under `umask 077` deploys at 0600 and nginx
# answers 403 "Permission denied", which reads exactly like an access
# rule. Both are lk_web/deploy.py's settings, deliberately, because that
# script writes the directory above this one and the two must not
# disagree about what a served file looks like.
RSYNC="rsync -a --checksum --omit-dir-times --chmod=D755,F644 --delete"

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(dirname "$(dirname "$(dirname "$HERE")")")
DOCSITE="$REPO/docsite"
OUT="$DOCSITE/build"

usage() { sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-1}"; }

[ "$SUDO" = 1 ] && RSYNC="$RSYNC --rsync-path=sudo rsync"

# ── depth: the guards, and why each one is here ────────────────────────

# rsync --delete onto an empty or missing tree is not a no-op, it is
# "remove everything /docs/ serves" -- and it is exactly what a forgotten
# `build` looks like. lk_web/deploy.py refuses the same shape for the same
# reason.
require_build() {
    [ -f "$OUT/index.html" ] || {
        echo "deploy: $OUT/index.html is missing -- run ./deploy.sh build" >&2
        exit 1
    }
    # A build older than the sources is the fetch1 lesson: `stage` then
    # `install` with no `sync` between put yesterday's release back on the
    # box and reported success. Here the equivalent is editing doc/ and
    # syncing the build from before the edit.
    stale=$(find "$REPO/doc" "$DOCSITE/docusaurus.config.js" \
                 "$DOCSITE/sidebars.js" "$DOCSITE/src" \
                 -newer "$OUT/index.html" 2>/dev/null | head -5)
    [ -z "$stale" ] || {
        echo "deploy: the build is older than these sources:" >&2
        printf '%s\n' "$stale" | sed "s|^$REPO/|    |" >&2
        echo "deploy: run ./deploy.sh build, then sync." >&2
        exit 1
    }
}

# ── the verbs ──────────────────────────────────────────────────────────
[ $# -gt 0 ] || usage
case "$1" in
    build)
        echo "== building $DOCSITE =="
        # `npm ci` and not `npm install`: the lockfile is the point of
        # having one, and a deploy build must not resolve differently
        # from the build that was tested. Same call lk_web's npm kind makes.
        (cd "$DOCSITE" && npm ci --no-audit --no-fund && npm run build)
        echo "   -> $OUT"
        ;;
    diff)
        require_build
        echo "== what sync would change on $HOST:$DEST =="
        # Deletions are the only line that can lose something that was
        # never in this repo, and --itemize-changes buries them in the
        # transfer list. lk_web/deploy.py pulls them out for the same reason.
        # Two passes, not one sed with two commands: `s/…/…/p` rewrites the
        # pattern space, so a second command testing for the ORIGINAL
        # prefix matches the line it just rewrote and prints it twice.
        $RSYNC --dry-run --itemize-changes "$OUT/" "$HOST:$DEST" \
          | sed 's/^\*deleting  */WOULD DELETE: /' | sed 's/^/  /'
        ;;
    sync)
        require_build
        echo "== syncing to $HOST:$DEST =="
        $RSYNC "$OUT/" "$HOST:$DEST"
        echo "   https://diluvium.aloecraft.org/docs/"
        ;;
    -h|--help|help) usage 0 ;;
    *) echo "deploy: unknown verb '$1'" >&2; usage ;;
esac
