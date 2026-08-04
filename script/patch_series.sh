#!/bin/sh
#
# Diluvium core patch series tool.
#
# Diluvium keeps its changes to the Lua sources (src/) as small as possible;
# everything that can live outside them does (analyze.c, diluvium_api.c,
# wasm_stubs*.c). This script makes that boundary enforceable and the patch
# series producible, by diffing src/ against the upstream commit this fork
# branched from -- which is permanently available in this repository's own
# history, so no network access or vendored tarball is needed.
#
# The fork point predates the repo reorganization (files moved from the root
# into src/, testes/ into test/), which is why plain `git diff` cannot see
# these as modifications; this script maps the layouts.
#
# Usage:
#   script/patch_series.sh generate [outfile]   write the unified patch series
#                                               (default: stdout)
#   script/patch_series.sh list                 table of core files and status
#   script/patch_series.sh check                fail unless every modified core
#                                               file is in the allowlist below;
#                                               used by CI
#
# When a change to a new core file is intentional, add the file to
# CORE_PATCH_ALLOWLIST together with a reason -- that is the review point.
# The Lua 5.5 rebase reapplies exactly the files this tool reports.

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT"

# The last pure-upstream commit before Diluvium work began
# ("'lua.h' back to redundancy in version definitions", lua-5.4.7 line).
FORK_POINT=1ab3208a1fceb12fca8f24ba57d6e13c5bff15e3

# Core files that are allowed to differ from upstream, one per line:
#   <file>  <reason>
CORE_PATCH_ALLOWLIST='
llex.c      f-strings ($"..."), ?? token, $ must introduce a string
llex.h      TK_2Q/TK_FPART tokens, fstring_del/encrypted_flag on LexState
lparser.c   ~function statement forms, TK_FPART in simpleexp, ?? operator entry
lcode.c     OPR_2Q branch compile (EQK-nil + jump), luaK_stringK export
lcode.h     OPR_2Q enum entry, luaK_stringK export
lobject.h   is_encrypted flag on Proto
lfunc.c     is_encrypted initialization
ldump.c     XOR scramble of code/constants for secure protos
lundump.c   XOR unscramble of code/constants for secure protos
lundump.h   LUAC_FORMAT 0x44 (Diluvium bytecode format byte)
luaconf.h   fixed string hash seed (deterministic pairs order)
lua.h       Diluvium version/branding strings
lua.c       Diluvium branding in the REPL banner
'

usage() { sed -n '3,27p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

# Upstream files at the fork point (root layout), current tree in src/.
# (ls-tree pathspecs are prefixes, not globs, hence the grep.)
upstream_files() {
  git ls-tree --name-only "$FORK_POINT" | grep -E '\.(c|h)$'
}

allowlist_names() {
  printf '%s\n' "$CORE_PATCH_ALLOWLIST" | awk 'NF { print $1 }'
}

# Extract the pristine upstream file once per run; compare against the
# working tree (not HEAD), so uncommitted divergence is caught too.
TMPDIR_UP=$(mktemp -d)
trap 'rm -rf "$TMPDIR_UP"' EXIT INT TERM

upstream_copy() {  # upstream_copy <file> -> path of pristine copy
  if [ ! -f "$TMPDIR_UP/$1" ]; then
    git show "$FORK_POINT:$1" > "$TMPDIR_UP/$1"
  fi
  printf '%s\n' "$TMPDIR_UP/$1"
}

# status <file>: prints "modified", "pristine", or "missing"
file_status() {
  if [ ! -f "src/$1" ]; then
    echo missing
  elif cmp -s "$(upstream_copy "$1")" "src/$1"; then
    echo pristine
  else
    echo modified
  fi
}

cmd=${1:-}
case "$cmd" in

  generate)
    out=${2:-/dev/stdout}
    : > "$out"
    {
      echo "# Diluvium core patch series"
      echo "# upstream: lua-5.4.7 @ $FORK_POINT"
      echo "# tree:     $(git rev-parse HEAD)$(git diff --quiet -- src/ 2>/dev/null || echo ' + uncommitted changes')"
      echo "#"
      echo "# Apply to a pristine upstream tree laid out as src/ with 'patch -p1'."
    } >> "$out"
    for f in $(upstream_files); do
      if [ "$(file_status "$f")" = modified ]; then
        # a/src/<f> b/src/<f> labels so the series applies onto a src/ layout
        diff -u --label "a/src/$f" --label "b/src/$f" \
          "$(upstream_copy "$f")" "src/$f" >> "$out" || true
      fi
    done
    [ "$out" = /dev/stdout ] || echo "patch series written to $out" >&2
    ;;

  list)
    printf '%-14s %s\n' "STATUS" "FILE"
    for f in $(upstream_files); do
      printf '%-14s %s\n' "$(file_status "$f")" "$f"
    done
    echo ""
    echo "Files in src/ with no upstream counterpart (on-top or generated):"
    for f in $(git ls-tree --name-only HEAD:src); do
      git cat-file -e "$FORK_POINT:$f" 2>/dev/null || echo "  $f"
    done
    ;;

  check)
    fail=0
    for f in $(upstream_files); do
      case "$(file_status "$f")" in
        modified)
          if ! allowlist_names | grep -qx "$f"; then
            echo "NOT ALLOWED: src/$f differs from upstream but is not in the core patch allowlist"
            fail=1
          fi
          ;;
        missing)
          echo "MISSING: upstream file $f has no counterpart in src/"
          fail=1
          ;;
      esac
    done
    # Also flag allowlist entries that are no longer modified (stale entries).
    for f in $(allowlist_names); do
      if [ "$(file_status "$f")" = pristine ]; then
        echo "STALE ALLOWLIST ENTRY: src/$f no longer differs from upstream"
        fail=1
      fi
    done
    if [ "$fail" -eq 0 ]; then
      n=$(for f in $(upstream_files); do
            [ "$(file_status "$f")" = modified ] && echo x || true
          done | wc -l)
      echo "OK: $n core files modified, all allowlisted"
    fi
    exit "$fail"
    ;;

  *) usage ;;
esac
