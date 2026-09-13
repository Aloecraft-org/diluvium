#!/bin/sh
#
# Diluvium syntax freeness check (doc/Plan-2026-09.md 3.6).
#
# Principle 1 of the plan is 100% Lua compatibility, and the operational form
# of that claim is: every syntax form Diluvium adds is a *parse error* in stock
# Lua 5.5. A form that stock Lua would also accept, with some other meaning, is
# a form that silently changes what a valid Lua program does -- which is the one
# thing this fork promises never to do.
#
# "Free" is that property: the syntax was free to take.
#
# This checks it the only way it can be checked, by asking the two parsers.
# Each file under test/syntax/freeness/ must
#
#   * be REFUSED by a pristine upstream Lua built from the fork point, and
#   * be ACCEPTED by this tree's Lua.
#
# Both halves matter. Without the first the claim is untested. Without the
# second a typo passes: a file that is a syntax error everywhere satisfies the
# first half perfectly and proves nothing at all, and that is the failure mode
# a corpus of hand-written near-Lua drifts into.
#
# So a file belongs here once its form is implemented, not before. A form that
# is still being written has nothing to say about freeness yet.
#
# Usage:
#   script/freeness_check.sh            build both parsers and check the corpus
#   script/freeness_check.sh --keep DIR use (and keep) DIR for the built binaries
#
# ------------------------------------------------------------------- surface --
#
# Entry points:
#   main flow at the bottom of the file
#
# Configurable values:
#   FORK_POINT   upstream commit the pristine parser is built from; kept
#                identical to script/patch_series.sh, which is the tool that
#                defines what "upstream" means here
#   CORPUS       the directory of one-form-per-file programs
#
# Steps (in order, each a function):
#   build_upstream    pristine lua-5.5.1, from this repository's own history
#   build_diluvium    this tree's interpreter
#   write_driver      the two-line parse-only program both of them run
#   parses            the one question asked of each, per corpus file

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"

# Kept in step with script/patch_series.sh by hand, and checked below: the
# pristine parser has to come from the tree the patch series applies to, or
# "upstream refuses this" is a statement about some other Lua.
FORK_POINT=7579fc9d7ed90240487251dfb69168f8e64e9294
CORPUS=test/syntax/freeness

CC=${CC:-gcc}

series_fork_point=$(sed -n 's/^FORK_POINT=\(.*\)$/\1/p' script/patch_series.sh)
if [ "$series_fork_point" != "$FORK_POINT" ]; then
  echo "freeness_check: FORK_POINT here is $FORK_POINT but patch_series.sh"
  echo "says $series_fork_point. One of them was rebased and the other was not;"
  echo "they must name the same upstream tree." >&2
  exit 1
fi

keep=""
if [ "${1:-}" = "--keep" ]; then
  keep=${2:?--keep needs a directory}
  mkdir -p "$keep"
  WORK=$keep
else
  WORK=$(mktemp -d)
  trap 'rm -rf "$WORK"' EXIT INT TERM
fi

# The upstream interpreter, from this repository's own history -- no network,
# no vendored tarball, and no question about which Lua it is.
#
# The interpreter rather than luac, because upstream's tree at the fork point
# carries no luac.c: 'onelua.c' includes one under -DMAKE_LUAC and the file is
# not in the commit, so that build cannot be made. Nothing is lost -- 'loadfile'
# runs the same lexer and the same parser, which is the whole of what is being
# asked about -- and both sides are then driven identically, which matters more
# here than using the tool whose name says "compiler".
build_upstream() {
  mkdir -p "$WORK/upstream"
  for f in $(git ls-tree --name-only "$FORK_POINT" | grep -E '\.(c|h)$'); do
    git show "$FORK_POINT:$f" > "$WORK/upstream/$f"
  done
  ( cd "$WORK/upstream" \
    && $CC -O1 -std=c99 -o lua_upstream onelua.c -lm ) 2>/dev/null
  echo "$WORK/upstream/lua_upstream"
}

# This tree's interpreter, from the same three sources `make build_platform`
# hands to the compiler.
build_diluvium() {
  ( cd src \
    && $CC -O1 -std=c99 -o "$WORK/lua_diluvium" \
         onelua.c analyze.c diluvium_api.c -lm ) 2>/dev/null
  echo "$WORK/lua_diluvium"
}

# The driver both sides run. Parses and does not execute: a freeness file is a
# program neither Lua was written to run, and running one would be asking a
# different question.
write_driver() {
  cat > "$WORK/parse.lua" <<'LUA'
local path = ...
local chunk, err = loadfile(path)
if not chunk then
  io.stderr:write(err, "\n")
  os.exit(1)
end
os.exit(0)
LUA
}

parses() {  # parses <interpreter> <file>
  "$1" "$WORK/parse.lua" "$2" > "$WORK/out" 2>&1
}

echo "=== building the pristine upstream interpreter ($FORK_POINT)"
UPSTREAM=$(build_upstream)
echo "=== building this tree's interpreter"
DILUVIUM=$(build_diluvium)
write_driver

files=$(find "$CORPUS" -name '*.lua' | sort)
if [ -z "$files" ]; then
  echo "freeness_check: no files in $CORPUS -- nothing was checked, which is" >&2
  echo "not the same as everything passing." >&2
  exit 1
fi

fail=0
n=0
for f in $files; do
  n=$((n + 1))
  if parses "$UPSTREAM" "$f"; then
    echo "[FAIL] $f parses in stock Lua 5.5, so this form is NOT free."
    echo "       A valid Lua program could already contain it, and giving it a"
    echo "       new meaning would change what that program does."
    fail=1
    continue
  fi
  if ! parses "$DILUVIUM" "$f"; then
    echo "[FAIL] $f does not parse in this tree either:"
    sed 's/^/       /' "$WORK/out"
    echo "       A file that is a syntax error everywhere tests nothing. Fix the"
    echo "       file, or remove it until the form it names is implemented."
    fail=1
    continue
  fi
done

if [ "$fail" -eq 0 ]; then
  echo "freeness_check: $n forms, each refused by stock Lua 5.5 and accepted here"
else
  echo "freeness_check: failures above" >&2
fi
exit $fail
