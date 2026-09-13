#!/bin/sh
#
# The cross-target numeric corpus (doc/Plan-2026-09.md A2).
#
# Stage 0's acceptance criterion is one sentence: a corpus of array
# programs produces bit-identical output on native x86-64, aarch64 and
# wasm32. This is the thing that checks it.
#
# test/numeric/corpus.lua prints nothing but IEEE bit patterns and
# integers -- never a decimal, per 3.3, because glibc, musl, wasmtime's
# host, Chromium and mingw do not all format a double the same way and
# one expected file has to be valid on all of them. So a difference here
# is a difference in the arithmetic, not in the printing, and a single
# hex digit is a real report.
#
# What a failure most likely means, in the order worth checking:
#
#   1. The floating-point flags in 3.5 did not reach a build path. Run
#      'make contraction_check' on the same machine; it answers that
#      question directly and needs neither Lua nor libm.
#   2. A kernel stopped reducing in the canonical order (numeric spec
#      section 4). The 'sum_hard' and 'dot_hard' lines are the sensitive
#      ones: their magnitudes are chosen so a naive left-to-right sum
#      gives a different answer.
#   3. Something in the corpus genuinely changed and the expected file is
#      stale. That is what '--update' is for, and the diff belongs in the
#      commit message either way.
#
# Usage:
#   script/numeric_corpus.sh [--bin BIN] [--update]
#
#   --bin      the interpreter to run it with (default: dist/diluvium_debug)
#   --update   rewrite the expected file from this run
#
# ------------------------------------------------------------- surface --
#
# Configurable values:
#   CORPUS     the program
#   EXPECTED   the output it must produce, on every target
#   BIN        what runs it

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"

CORPUS=test/numeric/corpus.lua
EXPECTED=test/numeric/expected.txt
BIN=dist/diluvium_debug
UPDATE=0

while [ $# -gt 0 ]; do
  case $1 in
    --bin) BIN=$2; shift 2 ;;
    --update) UPDATE=1; shift ;;
    -h|--help) sed -n '3,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "numeric_corpus: unknown argument '$1'" >&2; exit 2 ;;
  esac
done

[ -x "$BIN" ] || { echo "numeric_corpus: no interpreter at '$BIN'" >&2; exit 2; }

# The library is behind a build feature, so a build without it has nothing
# to say here. Skipping loudly rather than passing: a green run that
# tested nothing is the failure mode this whole corpus exists against.
# 'assert' rather than 'os.exit': under the ltests.h debug build an
# 'os.exit' that does not close the state trips the final memory check,
# which would report a leak where there is only an early exit.
if ! "$BIN" -e 'assert(array)' 2>/dev/null; then
  echo "numeric_corpus: '$BIN' has no 'array' library, so it was built"
  echo "without the numeric feature. Build with NUMERIC=1 and re-run."
  exit 2
fi

OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT INT TERM

"$BIN" "$CORPUS" > "$OUT" 2>&1 || {
  echo "numeric_corpus: the corpus did not run to completion:" >&2
  sed 's/^/  /' "$OUT" >&2
  exit 1
}

if [ "$UPDATE" -eq 1 ]; then
  cp "$OUT" "$EXPECTED"
  echo "numeric_corpus: $EXPECTED rewritten from this run ($(wc -l < "$EXPECTED") lines)"
  exit 0
fi

if diff -u "$EXPECTED" "$OUT"; then
  echo "numeric_corpus: $(wc -l < "$EXPECTED") lines, bit-identical to $EXPECTED"
else
  echo >&2
  echo "numeric_corpus: the arithmetic on this target does not match the" >&2
  echo "expected bits. See the header of this script for what to check" >&2
  echo "first; 'make contraction_check' answers the most likely cause." >&2
  exit 1
fi
