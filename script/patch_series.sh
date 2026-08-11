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
# The fork point is upstream lua-5.5.1 (root layout); the Diluvium tree lives
# in src/, which is why plain `git diff` cannot see these as modifications --
# this script maps the layouts.
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

# Pristine upstream lua-5.5.1 (tag v5.5.1). The core patch series is the
# delta between this and src/. Bump this (and re-run 'check') when rebasing
# onto a newer upstream release.
FORK_POINT=7579fc9d7ed90240487251dfb69168f8e64e9294

# Core files that are allowed to differ from upstream, one per line:
#   <file>  <reason>
CORE_PATCH_ALLOWLIST='
llex.c      f-strings ($"..."), ?? token, $ must introduce a string, contextual keyword names
llex.h      TK_2Q/TK_FPART tokens, fstring_del/encrypted_flag, contextual keyword names
lparser.c   ~function forms, f-string codegen, switch, defer, with, compound assignment, ?? and ?.
lcode.c     OPR_2Q branch compile (EQK-nil + jump), luaK_skipifnil for ?.
lcode.h     OPR_2Q enum entry, luaK_skipifnil export
lobject.h   is_encrypted flag on Proto
lfunc.c     is_encrypted initialization
ldump.c     XOR scramble of code/constant strings for secure protos; taint pass
lundump.c   XOR unscramble per string flag; forced copy of fixed buffers
lundump.h   LUAC_FORMAT 0x46 (Diluvium bytecode format byte)
lvm.c       type-test R[A] in OP_SETLIST before reading it as a table (corrupt-bytecode guard the load-time verifier cannot make; other opcodes already test via luaV_fastget)
ltm.c       same table type-test for the vararg-table path in luaT_getvarargs
luaconf.h   fixed string hash seed (deterministic pairs order)
lua.h       Diluvium version/branding strings
lua.c       Diluvium branding; REPL input handling moved to drepl.c; --task delegates to dtask.c
onelua.c    include analyze.c; rename ltests.c resetCI (amalgamation clash)
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

# The fork point must actually be in this repository.
#
# Audit finding 30: without this, `check` reported "OK: 0 core files modified,
# all allowlisted" and exited 0 having examined nothing. `set -eu` does not catch
# it -- errexit does not fire on a failed command substitution in a `for` word
# list, so `for f in $(upstream_files)` with an unreachable fork point simply
# runs its body zero times and carries on. The stale-allowlist loop cannot
# compensate either: `upstream_copy` fails inside a command substitution, so
# every file compares as "modified" rather than "pristine" and nothing is
# flagged.
#
# A shallow clone is the ordinary way to get there; `git clone --depth 1` does
# not fetch the fork point, and the script's own header promises it is
# "permanently available in this repository's own history", which is true only of
# full clones. The guard job is safe today because test.yml sets fetch-depth: 0 --
# that is, by a setting in a different file, which is exactly the kind of thing
# that gets changed by someone who does not know it is load-bearing.
#
# A guard that cannot check is a failure, not a pass. Exit 2 rather than 1, so it
# is distinguishable from "found an unallowlisted file".
require_fork_point() {
  if ! git cat-file -e "$FORK_POINT^{commit}" 2>/dev/null; then
    echo "patch_series: fork point $FORK_POINT is not in this repository." >&2
    echo "  A shallow clone will do this. Run 'git fetch --unshallow', or set" >&2
    echo "  fetch-depth: 0 on the checkout step in CI." >&2
    echo "  Refusing to check nothing and report success." >&2
    exit 2
  fi
  if [ -z "$(upstream_files)" ]; then
    echo "patch_series: no .c/.h files at $FORK_POINT; refusing to continue." >&2
    exit 2
  fi
}

cmd=${1:-}
case "$cmd" in

  generate)
    require_fork_point
    out=${2:-/dev/stdout}
    : > "$out"
    {
      echo "# Diluvium core patch series"
      echo "# upstream: lua-5.5.1 @ $FORK_POINT"
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
    require_fork_point
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
    require_fork_point
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
      if [ "$n" -eq 0 ]; then
        # Every allowlist entry names a file that is supposed to differ, and the
        # stale-entry loop above would have caught a genuinely empty series. Zero
        # here means the comparison itself is not working.
        echo "patch_series: $(upstream_files | wc -l) core files compared and" >&2
        echo "  none differ from upstream, which cannot be right while the" >&2
        echo "  allowlist is not empty. Refusing to report success." >&2
        exit 2
      fi
      echo "OK: $n core files modified, all allowlisted"
    fi
    exit "$fail"
    ;;

  *) usage ;;
esac
