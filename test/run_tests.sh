#!/bin/sh
#
# Diluvium test-suite runner.
#
# Single source of truth for which tests run, which are skipped and why, and
# what extra arguments each needs. Used by `make test_cases` / `make test_ci`
# and by .github/workflows/test.yml.
#
# Unlike a plain `make` recipe this keeps going after a failure, so one broken
# test does not hide the state of the other thirty.
#
# Exit status is 0 only when every test that ran passed.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(dirname "$SCRIPT_DIR")

BIN="$REPO_ROOT/dist/diluvium_debug"
TIMEOUT=300
INCLUDE_SKIPPED=0
MODE=run
SELECTED=""

# ---------------------------------------------------------------------------
# The test table.
#
#   name | run|skip | extra args | reason (required for skip, optional for run)
#
# Keep this list alphabetical. When adding a lean test for a bug fix, add it
# here -- this is the only list; the Makefile and CI both read it.
# ---------------------------------------------------------------------------
read_table() {
  cat <<'EOF'
api|skip||needs the C API test harness (ltests.c is not linked into the standalone binary)
attrib|skip||needs the C API test harness (ltests.c is not linked into the standalone binary)
benchmark|run||
big|skip||designed to consume 2GB-16GB of RAM
bitwise|run||
bwcoercion|run||
calls|run||
closure|run||
code|run||
constructs|run||
coroutine|run||
cstack|run||
db|run||
errors|run||
events|run||
files|run|-e _port=true|_port skips the stdin-seek assertion, which is not portable to CI (stdin is not a seekable tty)
gc|run||
gengc|run||
goto|run||
heavy|skip||designed to consume 2GB-16GB of RAM
literals|skip||musl vs glibc float conversion differences
locals|run||
main|skip||fails on the static binary
math|run||
nextvar|run||
pm|run||
secure_function|run||
sort|run||
strings|run||
test_analysis|run||
test_determinism|run||
test_fstrings|run||
test_nullco|run||
tpack|run||
tracegc|run||
utf8|run||
vararg|run||
verybig|run||
EOF
}

usage() {
  cat <<'EOF'
Usage: run_tests.sh [options] [test ...]

  --bin PATH          test binary (default: <repo>/dist/diluvium_debug)
  --timeout SECONDS   per-test timeout, 0 disables (default: 300)
  --include-skipped   also run tests marked 'skip'
  --list              print the tests that would run, one per line
  --list-skipped      print skipped tests with their reasons
  -h, --help          this message

Naming tests explicitly (e.g. `run_tests.sh strings gc`) runs only those,
overriding the skip list.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --bin)             BIN=$2; shift 2 ;;
    --timeout)         TIMEOUT=$2; shift 2 ;;
    --include-skipped) INCLUDE_SKIPPED=1; shift ;;
    --list)            MODE=list; shift ;;
    --list-skipped)    MODE=list-skipped; shift ;;
    -h|--help)         usage; exit 0 ;;
    -*)                echo "run_tests.sh: unknown option '$1'" >&2; exit 2 ;;
    *)                 SELECTED="$SELECTED $1"; shift ;;
  esac
done

TABLE=$(mktemp)
trap 'rm -rf "$TABLE" ${LOG_DIR:-}' EXIT INT TERM
read_table > "$TABLE"

# --list / --list-skipped ---------------------------------------------------
if [ "$MODE" = list ] || [ "$MODE" = list-skipped ]; then
  while IFS='|' read -r name status extra reason; do
    [ -z "$name" ] && continue
    if [ "$MODE" = list ] && [ "$status" = run ]; then
      printf '%s\n' "$name"
    elif [ "$MODE" = list-skipped ] && [ "$status" = skip ]; then
      printf '%-18s %s\n' "$name" "$reason"
    fi
  done < "$TABLE"
  exit 0
fi

# ---------------------------------------------------------------------------
# Tests run with the cwd set to test/, so a relative --bin would be resolved
# against the wrong directory. Pin it to an absolute path against the cwd we
# were invoked from, before anything changes directory.
case "$BIN" in
  /*) ;;
  *)
    bin_dir=$(dirname -- "$BIN")
    if [ -d "$bin_dir" ]; then
      BIN="$(CDPATH= cd -- "$bin_dir" && pwd)/$(basename -- "$BIN")"
    fi
    ;;
esac

if [ ! -x "$BIN" ]; then
  echo "run_tests.sh: test binary not found or not executable: $BIN" >&2
  echo "              build it first with: make test_build" >&2
  exit 2
fi

# 'timeout' is GNU coreutils and is absent on stock macOS. Degrade to running
# unbounded rather than failing every test.
TIMEOUT_CMD=""
case "$TIMEOUT" in
  ''|*[!0-9]*) echo "run_tests.sh: --timeout must be a whole number" >&2; exit 2 ;;
  0) ;;
  *)
    if command -v timeout >/dev/null 2>&1; then
      TIMEOUT_CMD="timeout $TIMEOUT"
    elif command -v gtimeout >/dev/null 2>&1; then
      TIMEOUT_CMD="gtimeout $TIMEOUT"
    else
      echo "note: no 'timeout' command available; running tests unbounded" >&2
    fi
    ;;
esac

echo "Diluvium test suite"
echo "  binary : $BIN"
echo "  version: $("$BIN" -v 2>&1 | head -1)"
echo "  tests  : $SCRIPT_DIR"
echo ""

PASSED=0
FAILED=0
SKIPPED=0
FAILED_NAMES=""
LOG_DIR=$(mktemp -d)

selected_contains() {
  for s in $SELECTED; do
    [ "$s" = "$1" ] && return 0
  done
  return 1
}

# Redirecting the loop from a file (rather than piping into it) keeps the body
# in this shell, so the counters below survive the loop.
while IFS='|' read -r name status extra reason; do
  [ -z "$name" ] && continue

  if [ -n "$SELECTED" ]; then
    selected_contains "$name" || continue
  elif [ "$status" = skip ] && [ "$INCLUDE_SKIPPED" -eq 0 ]; then
    printf 'SKIP  %-18s %s\n' "$name" "$reason"
    SKIPPED=$((SKIPPED + 1))
    continue
  fi

  if [ ! -f "$SCRIPT_DIR/$name.lua" ]; then
    printf 'FAIL  %-18s missing file %s.lua\n' "$name" "$name"
    FAILED=$((FAILED + 1))
    FAILED_NAMES="$FAILED_NAMES $name"
    continue
  fi

  log="$LOG_DIR/$name.log"
  start=$(date +%s)
  # shellcheck disable=SC2086 -- $TIMEOUT_CMD and $extra are meant to split
  if (cd "$SCRIPT_DIR" && $TIMEOUT_CMD "$BIN" $extra "$name.lua") >"$log" 2>&1; then
    rc=0
  else
    rc=$?
  fi
  elapsed=$(( $(date +%s) - start ))

  if [ "$rc" -eq 0 ]; then
    printf 'PASS  %-18s %ss\n' "$name" "$elapsed"
    PASSED=$((PASSED + 1))
  else
    if [ "$rc" -eq 124 ]; then
      printf 'FAIL  %-18s timed out after %ss\n' "$name" "$TIMEOUT"
    else
      printf 'FAIL  %-18s exit %s\n' "$name" "$rc"
    fi
    tail -20 "$log" | sed 's/^/        | /'
    FAILED=$((FAILED + 1))
    FAILED_NAMES="$FAILED_NAMES $name"
  fi
done < "$TABLE"

echo ""
echo "-------------------------------------------------------"
echo "passed $PASSED   failed $FAILED   skipped $SKIPPED"
[ -n "$FAILED_NAMES" ] && echo "failing:$FAILED_NAMES"
echo "-------------------------------------------------------"

# GitHub Actions job summary, when running under CI.
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo "### Test suite &mdash; ${RUNNER_OS:-local}"
    echo ""
    if [ "$FAILED" -eq 0 ]; then
      echo "**$PASSED passed**, $SKIPPED skipped."
    else
      echo "**$FAILED failed**, $PASSED passed, $SKIPPED skipped."
      echo ""
      echo "Failing:"
      for n in $FAILED_NAMES; do echo "- \`$n\`"; done
    fi
  } >> "$GITHUB_STEP_SUMMARY"
fi

[ "$FAILED" -eq 0 ]
