#!/bin/sh
#
# Bytecode compiled by one Diluvium build loads on every other one.
#
# That is a promise about the numeric types, because the dump header records
# sizeof(lua_Integer) and sizeof(lua_Number) and the loader refuses a chunk
# that disagrees. luaconf.h pins both rather than letting the platform pick,
# and this checks the two halves of that.
#
# Part A is the regression guard for the bug this exists because of. Stock
# luaconf takes LUA_USE_C89 -- a statement about the *library* a target has --
# as a statement about integer width too, so the browser build, which sets it
# for the wasm shim's sake, was a 32-bit-integer build. Nothing on an LP64
# host notices: 'long' is 64 bits there, so the wrong branch gives the right
# answer and the bug is invisible. Checking the *macro* rather than the
# sizeof is what makes this check mean something on the machine CI runs on.
#
# Part B builds a deliberately mismatched interpreter and cross-loads, so the
# refusal is a demonstrated behaviour rather than a claim about upstream code.
# It needs two binaries, which is why this is not in test/dump_check.c.
#
# Usage: test/dump_cross_check.sh

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

CC=${CC:-gcc}

checks=0
failures=0

ok () {
  checks=$((checks + 1))
  if [ "$1" = 1 ]; then
    printf '[PASS] %s\n' "$2"
  else
    printf '[FAIL] %s\n' "$2"
    failures=$((failures + 1))
  fi
}

mkdir -p "$REPO_ROOT/.data"
[ -f "$REPO_ROOT/.data/onelua.c" ] || cp -r "$REPO_ROOT"/src/* "$REPO_ROOT/.data"
DATA="$REPO_ROOT/.data"

printf '== dump_cross_check: a chunk loads on every Diluvium build ==\n\n'

# ---------------------------------------------------------------- part A --
# The build flags that used to choose the numeric types no longer do.

cat > "$WORK/print_types.c" <<'EOF'
#include <stdio.h>
#include "lua.h"
int main (void) {
  printf("%d %d\n", (int)LUA_INT_TYPE, (int)LUA_FLOAT_TYPE);
  return 0;
}
EOF

want='3 2'

for flags in '' '-DLUA_USE_C89' '-DLUA_32BITS' '-DLUA_USE_C89 -DLUA_32BITS'; do
  # shellcheck disable=SC2086
  $CC -O0 -std=c99 -I"$DATA" $flags -o "$WORK/types" "$WORK/print_types.c"
  got=$("$WORK/types")
  label=${flags:-"(no flags)"}
  [ "$got" = "$want" ] && same=1 || same=0
  ok "$same" "the numeric types are long long + double under $label"
  [ "$same" = 1 ] || printf '       got "%s", want "%s"\n' "$got" "$want"
done

# ---------------------------------------------------------------- part B --
# A build that really does disagree, and what happens when it does.

$CC -O2 -std=c99 -DLUA_USE_LINUX -DMAKE_LUA -I"$DATA" \
  -o "$WORK/dv_pinned" "$DATA/onelua.c" -lm
$CC -O2 -std=c99 -DLUA_USE_LINUX -DMAKE_LUA -I"$DATA" \
  -DDILUVIUM_NUMBERS_UNPINNED -DLUA_32BITS \
  -o "$WORK/dv_narrow" "$DATA/onelua.c" -lm

# Guard against a vacuous pass: if the second build were not actually
# narrower, every refusal below would "succeed" for the wrong reason.
pinned_max=$("$WORK/dv_pinned" -e 'io.write(math.maxinteger)')
narrow_max=$("$WORK/dv_narrow" -e 'io.write(math.maxinteger)')
printf '      pinned math.maxinteger %s\n      narrow math.maxinteger %s\n' \
  "$pinned_max" "$narrow_max"
[ "$pinned_max" != "$narrow_max" ] && differ=1 || differ=0
ok "$differ" "the two builds really do disagree about integers"

"$WORK/dv_pinned" -e 'local f=io.open("'"$WORK"'/pinned.luac","wb")
  f:write(string.dump(load("return 1 + 1"))) f:close()'
"$WORK/dv_narrow" -e 'local f=io.open("'"$WORK"'/narrow.luac","wb")
  f:write(string.dump(load("return 1 + 1"))) f:close()'

# Each build loads what it wrote, so a refusal below is about the mismatch
# and not about the dump being broken. The path is baked into the chunk
# rather than passed as an argument: with '-e' and no script there is no
# 'arg' table to read it from.
tryload () {  # tryload <interpreter> <chunk file>
  "$1" -e 'local d = io.open("'"$2"'", "rb"):read("a")
           local c, err = load(d)
           if c then io.write("ok ", tostring(c()))
           else io.write("refused: ", err) end'
}

got=$(tryload "$WORK/dv_pinned" "$WORK/pinned.luac")
[ "$got" = "ok 2" ] && self=1 || self=0
ok "$self" "the pinned build loads its own dump"

got=$(tryload "$WORK/dv_narrow" "$WORK/narrow.luac")
[ "$got" = "ok 2" ] && self=1 || self=0
ok "$self" "the narrow build loads its own dump"

# And the point of all of it: across the two, refused rather than misread.
got=$(tryload "$WORK/dv_narrow" "$WORK/pinned.luac")
printf '      narrow reading the pinned chunk: %s\n' "$got"
case "$got" in *"size mismatch"*) refused=1 ;; *) refused=0 ;; esac
ok "$refused" "a pinned chunk is refused by a narrow build, not misread"

got=$(tryload "$WORK/dv_pinned" "$WORK/narrow.luac")
printf '      pinned reading the narrow chunk: %s\n' "$got"
case "$got" in *"size mismatch"*) refused=1 ;; *) refused=0 ;; esac
ok "$refused" "a narrow chunk is refused by a pinned build, not misread"

printf '\n%d checks, %d failed\n' "$checks" "$failures"
[ "$failures" -eq 0 ]
