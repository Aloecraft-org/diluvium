#!/bin/sh
#
# Vendor the openlibm subset the numeric spec's stage 1 lists.
#
# This is the tool that produced src/libm/, kept so that re-vendoring is a
# command rather than an afternoon of hand-editing -- and so that a reader
# can see exactly what was changed about the upstream sources, which is
# the only honest way to carry someone else's code.
#
# What it changes, and nothing else:
#
#   * the four upstream includes become one, src/libm/dvlibm_private.h,
#     which is a self-contained replacement for openlibm's math_private.h
#     and its per-architecture fpmath headers. Those exist to describe
#     long double layouts and platform config; nothing here is long
#     double, and dropping them is what makes this build the same way on
#     x86-64, aarch64, wasm32 and mingw.
#   * every exported symbol is prefixed 'dv_'. Required by
#     doc/Plan-2026-09.md 3.5: with the platform's names, a compiler is
#     free to substitute its own 'exp' or constant-fold a call, and the
#     whole point of an embedded libm is that it does neither.
#   * 'OLM_DLLEXPORT' and the trailing long-double weak-reference blocks
#     are removed. They export names and alias 'expl' to 'exp' on
#     platforms where long double is binary64 -- both wrong for a library
#     that is deliberately internal.
#   * complex-valued definitions are dropped. This reaches exactly one
#     function, k_exp.c's '__ldexp_cexp', which nothing vendored here
#     calls and which is the only reason that file would need
#     <openlibm_complex.h> and a working '_Complex' on every target.
#   * a left shift whose left operand is a signed value that can be
#     negative is cast to 'u_int32_t' first. Shifting a negative int is
#     undefined in C99 -- well defined only from C++20 -- and this
#     project's sanitizer job runs with halt_on_error, so four of these
#     stop the build on arguments as ordinary as 'exp(-1)'. The cast
#     changes no bit on any two's-complement machine, which is every
#     target here; it changes the code from working to defined. The list
#     is SHIFTFIX below, one entry per site, so a reader can check each
#     against upstream.
#
# Nothing else is touched: not a constant, not a branch, not a comment.
# A diff against upstream should show only the four kinds of change above.
#
# The one thing that is *generated* rather than edited: a pair of headers
# per file that scope its file-scope names. Upstream compiles each of
# these separately, so eight of them declare a 'static const double one'
# and they never meet; src/dlibm.c includes them all into one translation
# unit, where they collide. Rather than renaming the statics in the
# sources -- which would be editing someone else's code for the
# convenience of ours -- '<file>.pre.h' renames them with the
# preprocessor for the length of the include and '<file>.post.h' puts
# them back. The names come from 'nm' on an unoptimised build of the file
# itself, so the list cannot drift from what the file actually declares.
#
# Usage:
#   script/vendor_libm.sh [--upstream DIR]
#
#   --upstream  a checkout of https://github.com/JuliaMath/openlibm
#               (default: clone it into a temporary directory)
#
# ------------------------------------------------------------- surface --
#
# Configurable values:
#   UPSTREAM_URL   where openlibm comes from
#   UPSTREAM_REV   the commit src/libm/ was produced from; update it here
#                  and in src/libm/PROVENANCE when re-vendoring
#   PUBLIC         the functions the spec names
#   KERNELS        the internal helpers they need
#   RENAMES        the symbol map, one 'from to' pair per line
#   SHIFTFIX       the signed-shift sites, one 'file|search|replace' row
#   CC, NM         the toolchain used to read each file's local symbols

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"

UPSTREAM_URL=https://github.com/JuliaMath/openlibm
UPSTREAM_REV=5fe399749f9276eaa0b8403e507470da05cbbb3f
DEST=src/libm

# The spec's list (numeric spec stage 1). 'sqrt', 'floor', 'ceil', 'fabs'
# and 'fmod' are not here and that is deliberate: they are IEEE-exact on
# every target, so the platform's are already bit-identical and vendoring
# them would add code that could only match.
PUBLIC="e_exp e_log e_log2 e_log10 e_pow s_sin s_cos s_tan e_asin e_acos
        s_atan e_atan2 e_sinh e_cosh s_tanh s_expm1 s_log1p"

# What those call. Argument reduction for the trigonometric functions is
# the bulk of it, and k_rem_pio2.c is the largest file here.
KERNELS="k_sin k_cos k_tan e_rem_pio2 k_rem_pio2 k_exp"

RENAMES='
__ieee754_exp dv_exp
__ieee754_log dv_log
__ieee754_log2 dv_log2
__ieee754_log10 dv_log10
__ieee754_pow dv_pow
__ieee754_asin dv_asin
__ieee754_acos dv_acos
__ieee754_atan2 dv_atan2
__ieee754_sinh dv_sinh
__ieee754_cosh dv_cosh
__ieee754_rem_pio2 dv_rem_pio2
__kernel_sin dv_kernel_sin
__kernel_cos dv_kernel_cos
__kernel_tan dv_kernel_tan
__kernel_rem_pio2 dv_kernel_rem_pio2
__ldexp_exp dv_ldexp_exp
__frexp_exp dv_frexp_exp
sin dv_sin
cos dv_cos
tan dv_tan
atan dv_atan
tanh dv_tanh
expm1 dv_expm1
log1p dv_log1p
'

# One row per site: file, the text to find, the text to put there. The
# search strings are upstream's, verbatim, so a row that stops matching
# after an upstream update is a loud failure rather than a silent skip.
SHIFTFIX='
e_exp.c|(k<<20)|((u_int32_t)k<<20)
e_exp.c|((k+1000)<<20)|((u_int32_t)(k+1000)<<20)
e_pow.c|(j<<(52-k))|((u_int32_t)j<<(52-k))
e_pow.c|(j<<(20-k))|((u_int32_t)j<<(20-k))
e_pow.c|(k<<18)|((u_int32_t)k<<18)
e_pow.c|j += (n<<20);|j += (int32_t)((u_int32_t)n<<20);
e_rem_pio2.c|((int32_t)(e0<<20))|((int32_t)((u_int32_t)e0<<20))
k_rem_pio2.c|i<<(24-q0)|(int32_t)((u_int32_t)i<<(24-q0))
s_expm1.c|(k<<20)|((u_int32_t)k<<20)
s_expm1.c|((0x3ff-k)<<20)|((u_int32_t)(0x3ff-k)<<20)
'

UPSTREAM=""
while [ $# -gt 0 ]; do
  case $1 in
    --upstream) UPSTREAM=$2; shift 2 ;;
    -h|--help) sed -n '3,44p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "vendor_libm: unknown argument '$1'" >&2; exit 2 ;;
  esac
done

if [ -z "$UPSTREAM" ]; then
  UPSTREAM=$(mktemp -d)
  trap 'rm -rf "$UPSTREAM"' EXIT INT TERM
  echo "=== cloning $UPSTREAM_URL"
  git clone --quiet "$UPSTREAM_URL" "$UPSTREAM"
  git -C "$UPSTREAM" checkout --quiet "$UPSTREAM_REV"
fi

got=$(git -C "$UPSTREAM" rev-parse HEAD)
if [ "$got" != "$UPSTREAM_REV" ]; then
  echo "vendor_libm: the checkout is at $got but this script vendors" >&2
  echo "$UPSTREAM_REV. Update UPSTREAM_REV here and in $DEST/PROVENANCE" >&2
  echo "if that is deliberate." >&2
  exit 1
fi

mkdir -p "$DEST"

# The rename pass, as one sed program. Word boundaries throughout, so
# 'sin' does not touch 'sinh' and 'log1p' does not touch 'k_log1p'.
sedprog=$(printf '%s\n' "$RENAMES" | awk 'NF == 2 { printf "s/\\b%s\\b/%s/g;", $1, $2 }')

# The include and decoration rewrite, applied to every vendored file.
rewrite() {
  awk '
    # Everything from the first complex-valued definition to the end of
    # the file is dropped. k_exp.c is the only file this reaches, and the
    # only thing it loses is '__ldexp_cexp' -- complex, unreachable from
    # anything vendored here, and the sole reason the file would need
    # openlibm_complex.h and a C99 '_Complex' on every target.
    /^OLM_DLLEXPORT double complex$/ { skip = 1 }
    # Drop the upstream includes; dvlibm_private.h replaces all four.
    /^#include "cdefs-compat.h"$/ { next }
    /^#include <openlibm_complex.h>$/ { next }
    /^#include "math_private.h"$/ { print "#include \"dvlibm_private.h\""; next }
    /^#include <openlibm_math.h>$/ { next }
    /^#include <float.h>$/ { next }
    # Drop the trailing long-double alias block.
    /^#if +\(?LDBL_MANT_DIG *== *53\)?/ { skip = 1; next }
    skip && /^#endif/ { skip = 0; next }
    skip { next }
    # Not a library export.
    { gsub(/OLM_DLLEXPORT/, ""); print }
  ' "$1"
}

for f in $PUBLIC $KERNELS; do
  src="$UPSTREAM/src/$f.c"
  [ -f "$src" ] || { echo "vendor_libm: $src is missing" >&2; exit 1; }
  rewrite "$src" | sed -E "$sedprog" > "$DEST/$f.c"
done

# The signed-shift casts. Applied after the renames so the search strings
# stay upstream's own text; a row that matches nothing is a failure,
# because the alternative is quietly shipping the undefined behaviour it
# was written to remove.
printf '%s\n' "$SHIFTFIX" | while IFS='|' read -r file find repl; do
  [ -n "$file" ] || continue
  if ! grep -qF "$find" "$DEST/$file"; then
    echo "vendor_libm: SHIFTFIX row for $file no longer matches:" >&2
    echo "  $find" >&2
    echo "Upstream changed; check the site and update the row." >&2
    exit 1
  fi
  awk -v f="$find" -v r="$repl" '{
    i = index($0, f)
    if (i > 0) $0 = substr($0, 1, i - 1) r substr($0, i + length(f))
    print
  }' "$DEST/$file" > "$DEST/$file.tmp" && mv "$DEST/$file.tmp" "$DEST/$file"
done

# k_log.h is an inline helper included by e_log2.c and e_log10.c. It
# carries the same upstream includes and gets the same treatment; its
# statics are scoped per *including* file, since both end up in the one
# translation unit.
rewrite "$UPSTREAM/src/k_log.h" | sed -E "$sedprog" > "$DEST/k_log.h"

# The scoping headers. Compiled at -O0 on purpose: at any higher level a
# constant static is folded away and never reaches the symbol table, so
# the list would be short by exactly the names most likely to collide.
CC=${CC:-cc}
NM=${NM:-nm}
for f in $PUBLIC $KERNELS; do
  obj=$(mktemp)
  $CC -O0 -std=c99 -w -fno-common -I"$DEST" -c "$DEST/$f.c" -o "$obj"
  # Lowercase type letters are local symbols; '.LC0' and friends are the
  # compiler's own literals and have no source name to rename.
  statics=$($NM "$obj" | awk '$2 ~ /^[a-z]$/ && $3 !~ /^\./ { print $3 }' | sort -u)
  rm -f "$obj"
  # Object-like macros the file defines at file scope leak the same way a
  # static does -- k_tan.c's '#define one xxx[13]' is what taught this --
  # so they are undefined after the include too.
  macros=$(sed -n 's/^#[[:space:]]*define[[:space:]]\{1,\}\([A-Za-z_][A-Za-z0-9_]*\).*/\1/p' "$DEST/$f.c" | sort -u)
  {
    echo "/* Generated by script/vendor_libm.sh. Do not edit. */"
    echo "/* Scopes $f.c's file-scope names for the one translation unit"
    echo "   src/dlibm.c compiles every vendored source in. */"
    for n in $statics; do echo "#define $n ${f}_$n"; done
  } > "$DEST/$f.pre.h"
  {
    echo "/* Generated by script/vendor_libm.sh. Do not edit. */"
    for n in $statics $macros; do echo "#undef $n"; done
  } > "$DEST/$f.post.h"
done

cp "$UPSTREAM/LICENSE.md" "$DEST/LICENSE.md"

cat > "$DEST/PROVENANCE" <<EOF
openlibm, $UPSTREAM_URL
commit $UPSTREAM_REV

The files here are src/*.c from that commit, changed only by
script/vendor_libm.sh: the four upstream includes replaced by
dvlibm_private.h, every exported symbol prefixed 'dv_', OLM_DLLEXPORT
removed, and the trailing long-double weak-reference blocks removed. No
constant, branch or algorithm is altered. Re-run that script to update.

LICENSE.md is upstream's, verbatim. The vendored files are BSD, ISC, MIT
and public domain; openlibm's LGPL parts are its test files, which are
not vendored.
EOF

echo "=== vendored $(ls "$DEST"/*.c | wc -l) sources into $DEST"
