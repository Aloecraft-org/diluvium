#!/bin/sh
#
# The runtime fingerprint has to separate builds, not just versions.
#
# doc/Messaging.md 10.6 asks a snapshot header to carry "runtime identity", and
# the first draft listed version, opcode set, number config and LUAC_FORMAT. That
# is not enough, and this is the measurement that showed it: the debug and
# release builds of this same tree compile the same source to different bytecode,
# because ltests.h sets MAXINDEXRK to 1 and that is a codegen knob. Every field
# on that list is identical between them.
#
# So a header carrying only those would accept a snapshot whose Proto hashes can
# never match, and 10.5's "restore requires an exact match" would fail somewhere
# deep inside restore instead of at the header.
#
# The fix is to fingerprint by hashing a canary chunk's stripped dump, which
# covers every codegen knob including ones added later. This checks the property
# that fix exists for, and it needs two builds, which is why it cannot live in
# dsnap_check.c.
#
# Usage: test/fingerprint_check.sh

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

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

cat > "$WORK/print_fp.c" <<'EOF'
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "dsnap.h"
int main (void) {
  char fp[DILUVIUM_SHA256_HEX];
  lua_State *L = luaL_newstate();
  if (L == NULL) return 1;
  luaL_openlibs(L);
  diluvium_snap_fingerprint(L, fp);
  printf("%s\n", fp);
  lua_close(L);
  return 0;
}
EOF

mkdir -p "$REPO_ROOT/.data"
[ -f "$REPO_ROOT/.data/onelua.c" ] || cp -r "$REPO_ROOT"/src/* "$REPO_ROOT/.data"

# Two builds that differ only in the debug harness, which is exactly the pair
# the design's original field list could not tell apart.
gcc -O2 -std=c99 -DLUA_USE_LINUX -DMAKE_LIB -I"$REPO_ROOT/.data" \
  -o "$WORK/fp_release" "$WORK/print_fp.c" "$REPO_ROOT/.data/onelua.c" -lm
gcc -DLUA_USER_H='"ltests.h"' -O0 -g -DLUA_USE_LINUX -DMAKE_LIB \
  -I"$REPO_ROOT/.data" \
  -o "$WORK/fp_debug" "$WORK/print_fp.c" "$REPO_ROOT/.data/onelua.c" -lm

rel=$("$WORK/fp_release")
dbg=$("$WORK/fp_debug")

printf '      release %s\n      debug   %s\n' "$rel" "$dbg"

# Determinism first: a fingerprint that varied run to run would make the
# inequality below meaningless.
rel2=$("$WORK/fp_release")
rel3=$("$WORK/fp_release")
[ "$rel" = "$rel2" ] && [ "$rel2" = "$rel3" ] && same=1 || same=0
ok "$same" "the same build fingerprints identically across processes"

[ "$rel" != "$dbg" ] && differ=1 || differ=0
ok "$differ" "the debug and release builds fingerprint differently"

# And the reason, stated as a check rather than only in a comment: it is codegen
# and not the version, so a field list of version plus LUAC_FORMAT would miss it.
cat > "$WORK/print_ver.c" <<'EOF'
#include <stdio.h>
#include "lua.h"
#include "lundump.h"
int main (void) {
  printf("%s|%d|%d\n", LUA_RELEASE, (int)LUAC_VERSION, (int)LUAC_FORMAT);
  return 0;
}
EOF
gcc -O2 -std=c99 -DLUA_USE_LINUX -I"$REPO_ROOT/.data" -c \
  -o "$WORK/ver_rel.o" "$WORK/print_ver.c" 2>/dev/null || true
gcc -O2 -std=c99 -DLUA_USE_LINUX -DMAKE_LIB -I"$REPO_ROOT/.data" \
  -o "$WORK/ver_release" "$WORK/print_ver.c" "$REPO_ROOT/.data/onelua.c" -lm
gcc -DLUA_USER_H='"ltests.h"' -O0 -g -DLUA_USE_LINUX -DMAKE_LIB \
  -I"$REPO_ROOT/.data" \
  -o "$WORK/ver_debug" "$WORK/print_ver.c" "$REPO_ROOT/.data/onelua.c" -lm
vrel=$("$WORK/ver_release")
vdbg=$("$WORK/ver_debug")
printf '      identity fields: release %s / debug %s\n' "$vrel" "$vdbg"
[ "$vrel" = "$vdbg" ] && listsame=1 || listsame=0
ok "$listsame" \
  "version and LUAC_FORMAT are identical across those builds, which is why hashing them would not have worked"

printf '\n%d checks, %d failed\n' "$checks" "$failures"
[ "$failures" -eq 0 ]
