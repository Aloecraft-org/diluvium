#!/bin/sh
# The next free dev tag (doc/ALIGNMENT.md §7): v<version>-dev.<n>, where
# <version> is .technoproj's major.minor.patch and <n> is one more than the
# highest -dev. number any tag in this repository has ever carried. The
# counter is global and never reused, so a number names one build forever;
# it is allocated from the tags that exist rather than stored in the tree,
# so a nightly commits nothing and two branches cannot collide.
#
#   script/dev-tag.sh               prints the tag, e.g. v0.17.0-dev.3
#   script/dev-tag.sh --if-changed  prints nothing and exits 3 when HEAD is
#                                   the commit the newest dev tag points at,
#                                   so a nightly does not cut one build twice
#
# Dispatch the Release workflow with the printed tag and publish=true, or
# push the tag; either way the suffix selects the fast path.
#
# Deliberately the same script, option for option, as diluvium-drt's
# script/dev-tag.sh. The two repositories cut dev builds the same way because
# §7 is one scheme across all of them, and a second spelling of it here would
# be a second thing to keep in step.
set -eu
cd "$(dirname "$0")/.."

# The tags that exist, not the ones this clone happened to have. A shallow
# checkout reads no tags and would allocate dev.1 forever.
git fetch --tags -q origin 2>/dev/null || true

# .technoproj rather than VERSION, because VERSION carries the prerelease
# suffix of the series being developed ('0.17.0-dev.1') and the base is what
# the tag needs. TECHNO_VERSION holds the three numbers separately, so there
# is nothing to strip and no suffix to mis-parse.
version=$(python3 -c '
import json
v = json.load(open(".technoproj"))["TECHNO_VERSION"]
print("%d.%d.%d" % (v["major"], v["minor"], v["patch"]))
')

# 'sort -n', never plain sort: the bug §7 warns about appears at dev.10,
# where a lexical sort puts it behind dev.9 and the counter walks backwards.
last=$(git tag --list 'v*-dev.*' | sed -n 's/.*-dev\.\([0-9][0-9]*\)$/\1/p' | sort -n | tail -1)

if [ "${1:-}" = --if-changed ] && [ -n "$last" ]; then
    newest=$(git tag --list "v*-dev.$last" | head -1)
    if [ "$(git rev-parse "$newest^{commit}")" = "$(git rev-parse HEAD)" ]; then
        exit 3
    fi
fi

echo "v$version-dev.$(( ${last:-0} + 1 ))"
