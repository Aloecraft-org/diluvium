#!/bin/sh
#
# Ctrl-C has to interrupt a runaway loop, in both execution modes.
#
# This test exists because of a hazard that is invisible to the rest of the
# suite. 'lua.c' keeps a static 'globalL' and its SIGINT handler installs a
# debug hook on it; a hook is per-state, so once a chunk runs on a thread --
# which is what '--task' does -- a 'globalL' still naming the main state
# installs the hook where nothing is executing. Ctrl-C then does nothing at
# all, and does it quietly: the handler still runs, still resets the signal,
# and still looks like it worked. Nothing else in the suite presses Ctrl-C,
# so that regression would ship green.
#
# Verified to fail with the mitigation removed: dropping the
# 'diluvium_task_sethook' call in 'pmain' leaves the --task case hanging
# until the timeout.
#
# Usage: test/interrupt_check.sh [--bin path]

set -eu

BIN=./dist/diluvium_debug
while [ $# -gt 0 ]; do
  case $1 in
    --bin) BIN=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

failures=0
checks=0

# Interrupt a loop that never yields and never returns, and report what
# happened. A run that has to be killed is a failure, not a slow pass.
try_interrupt () {
  mode=$1
  label=$2
  checks=$((checks + 1))

  # shellcheck disable=SC2086
  "$BIN" $mode -e 'while true do end' \
    >"$WORK/out" 2>"$WORK/err" &
  pid=$!

  # Let it get properly into the loop before signalling.
  sleep 1
  kill -INT "$pid" 2>/dev/null || true

  # Give it a moment to unwind, then insist.
  waited=0
  while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt 50 ]; do
    sleep 0.1
    waited=$((waited + 1))
  done

  if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "[FAIL] $label: still running 5s after SIGINT, had to be killed"
    failures=$((failures + 1))
    return
  fi

  set +e
  wait "$pid"
  status=$?
  set -e

  if ! grep -q 'interrupted!' "$WORK/err"; then
    echo "[FAIL] $label: exited $status but said nothing about being interrupted"
    echo "       stderr: $(head -c 200 "$WORK/err")"
    failures=$((failures + 1))
    return
  fi
  if [ "$status" -eq 0 ]; then
    echo "[FAIL] $label: reported the interrupt but exited 0"
    failures=$((failures + 1))
    return
  fi
  echo "[PASS] $label: interrupted, exit $status"
}

echo "=== interrupt handling ==="
try_interrupt "" "default mode"
try_interrupt "--task" "--task mode (hook must follow the running thread)"

echo
echo "$checks checks, $failures failed"
[ "$failures" -eq 0 ]
