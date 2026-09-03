#!/bin/sh
#
# Every way a guest can spend its budget and not come back, one subprocess each.
#
# A hang cannot be asserted from inside the process it happens in -- the same
# reason 'snap_fuzz' runs its target as a subprocess, and the same shape as
# test/interrupt_check.sh. So each door runs as one, under a timeout, and its
# verdict is compared against the table below.
#
# The table is the point. It records which doors are closed *today*, so a door
# that reopens fails as REGRESSED, and a door that has just been closed fails
# as CLOSED -- update the table. That second direction is what makes this drive
# the increments in doc/FM-4.md 9 rather than merely watch them.
#
# Draft, in the attic rather than in test/, because it is increment 1 of that
# plan and not yet wired to a Makefile target. Promoting it means: a
# 'fm4_probe' target beside 'dv_check', this file as test/budget_check.sh, and
# a step in .github/workflows/test.yml.
#
# Usage: fm4-budget-check.sh [--probe path] [--timeout seconds]

set -eu

# -- entry points ------------------------------------------------------------
#   main: falls through the file. No functions; the table is the program.

# -- configurable values -----------------------------------------------------
PROBE=./dist/fm4_probe     # built from doc/attic/fm4-probe.c; see its header
TIMEOUT=10                 # seconds before a door is called a hang

# -- the doors: name, expected verdict (error|done|hang), why ----------------
#
# doc/FM-4.md 1 measured every row and 9 says which increment closes it.
DOORS='
control   error  a plain runaway: the budget has always stopped this
pcall     hang   FM-4 as named: the guest catches the raise and buys 1000 more
xpcall    hang   the same through xpcall
sortpcall hang   the same in a non-yieldable pcall, which takes another path
load      hang   the same through the protected parser
xhandler  hang   an xpcall message handler runs inside the hook, unhooked
gc_run    hang   a __gc finalizer runs with hooks off
gc_remark hang   and is finalized again every cycle once it re-marks itself
gc_alloc  hang   the same, driven by allocation rather than by collectgarbage
gc_free   hang   close-time finalizers run on the main state, never hooked
memcatch  hang   a memory budget caught in a loop
coresume  error  a cross-thread catch: the parent trips its own hook
coclose   error  the same through coroutine.close
matcher   hang   one C call, no VM instruction, so no hook -- host-side only
'

# depth: argument parsing, then one subprocess per row
while [ $# -gt 0 ]; do
  case $1 in
    --probe)   PROBE=$2; shift 2 ;;
    --timeout) TIMEOUT=$2; shift 2 ;;
    -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "budget_check: unknown argument '$1'" >&2; exit 2 ;;
  esac
done

if [ ! -x "$PROBE" ]; then
  echo "budget_check: no probe at $PROBE" >&2
  echo "  build it: see the header of doc/attic/fm4-probe.c" >&2
  exit 2
fi

# The table goes through a file rather than a pipe: a 'while read' fed by a
# pipe runs in a subshell, so the counters below would be discarded and this
# would exit 0 whatever it found -- a guard that cannot fail.
TABLE=$(mktemp)
trap 'rm -f "$TABLE"' EXIT INT TERM
printf '%s\n' "$DOORS" > "$TABLE"

checks=0
failures=0
printf '%-10s %-7s %-7s %s\n' DOOR EXPECT GOT NOTE
while read -r door expect why; do
  [ -n "$door" ] || continue
  checks=$((checks + 1))
  out=$("$PROBE" "$door" "$TIMEOUT" 2>&1) && rc=0 || rc=$?
  if [ "$rc" = 124 ]; then                            got=hang
  elif printf '%s' "$out" | grep -q 'run=DV_ERROR'; then got=error
  elif printf '%s' "$out" | grep -q 'run=DV_DONE'; then  got=done
  else                                                got="?($rc)"
  fi
  if [ "$got" = "$expect" ]; then
    printf '%-10s %-7s %-7s ok\n' "$door" "$expect" "$got"
  else
    failures=$((failures + 1))
    if [ "$expect" = hang ]; then
      printf '%-10s %-7s %-7s CLOSED -- update the table (%s)\n' \
             "$door" "$expect" "$got" "$why"
    else
      printf '%-10s %-7s %-7s REGRESSED (%s)\n' "$door" "$expect" "$got" "$why"
    fi
  fi
done < "$TABLE"

echo ""
if [ "$failures" -eq 0 ]; then
  echo "$checks doors, all as tabled"
else
  echo "$checks doors, $failures disagree with the table"
fi
exit $([ "$failures" -eq 0 ] && echo 0 || echo 1)
