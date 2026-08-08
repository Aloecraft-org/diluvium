#!/bin/sh
#
# Run the WASI artifact we actually ship.
#
# diluvium_wasi.wasm is published on every release and, until this existed,
# was never executed anywhere in CI -- it was built, checksummed and handed
# out. This runs the suite through it under a WASI runtime, so a break in
# the wasm build fails a build instead of reaching whoever downloads it.
#
# Usage:
#   script/wasi_check.sh [--wasm FILE] [--runtime CMD] [--native BIN]
#
#   --wasm     the module to test        (default: dist/diluvium_wasi.wasm)
#   --runtime  WASI runtime to drive it  (default: wasmtime)
#   --native   a native binary; when given, benchmark instruction counts
#              are compared between it and the module
#
# The runtime is reached through a small wrapper script rather than being
# invoked directly, because run_tests.sh takes a binary to run tests with
# and a wasm module is not one. The wrapper is that binary.
#
# The comparison against a native build is the interesting part. Benchmark
# instruction counts are supposed to be a property of the bytecode and not
# of the machine underneath it; running the same cases on wasm32 and on the
# host is what turns that from a claim into a check.

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

WASM="$REPO_ROOT/dist/diluvium_wasi.wasm"
RUNTIME=wasmtime
NATIVE=""

while [ $# -gt 0 ]; do
  case "$1" in
    --wasm)    WASM=$2; shift 2 ;;
    --runtime) RUNTIME=$2; shift 2 ;;
    --native)  NATIVE=$2; shift 2 ;;
    -h|--help) sed -n '3,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "wasi_check: unknown option '$1'" >&2; exit 2 ;;
  esac
done

# Absolutise before anything else: run_tests.sh runs each test with the cwd
# set to test/, so a relative --wasm or --native stops resolving the moment
# the suite starts. run_tests.sh already does this for its own --bin; the
# same reasoning applies to every path handed across that boundary.
abspath() {
  case "$1" in
    /*) printf '%s\n' "$1" ;;
    *)  printf '%s/%s\n' "$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)" \
                          "$(basename -- "$1")" ;;
  esac
}
[ -e "$WASM" ] && WASM=$(abspath "$WASM")
[ -n "$NATIVE" ] && [ -e "$NATIVE" ] && NATIVE=$(abspath "$NATIVE")

[ -f "$WASM" ] || { echo "wasi_check: no such module: $WASM" >&2; exit 2; }
command -v "$RUNTIME" >/dev/null 2>&1 || {
  echo "wasi_check: no WASI runtime '$RUNTIME' on PATH" >&2; exit 2; }

# ---------------------------------------------------------------------------
# Tests to run under wasm.
#
# Deliberately a list rather than the whole suite: WASI has no subprocesses
# and a restricted filesystem, so tests built around those would fail for
# reasons that say nothing about Diluvium. What is here covers every
# Diluvium construct plus the core language semantics they rest on.
#
# When adding to this list, add the test rather than removing one that
# fails -- a failure here is the point of the job.
# ---------------------------------------------------------------------------
WASI_TESTS="
  test_fstrings test_switch test_compound test_defer test_safenav test_with
  test_interop test_nullco test_secure_dump test_determinism secure_function
  strings math sort bitwise bwcoercion calls closure constructs events goto
  locals nextvar pm tpack utf8 vararg
"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

# The module needs the WebAssembly exception-handling proposal. Lua signals
# errors with setjmp/longjmp, and wasi-sdk implements those on top of wasm
# EH, so every Diluvium wasm build carries EH instructions whether or not
# anything throws. Browsers have had EH for years -- which is why the
# browser REPL works -- but wasmtime needs it asked for, and versions
# before roughly 28 have no flag to ask with, failing at parse with
# "exceptions proposal not enabled".
#
# Granting '/' keeps the wrapper indifferent to where the suite puts its
# working directory; this is a build-time check on a trusted module, not a
# sandbox demonstration.
RUNTIME_FLAGS=${WASI_RUNTIME_FLAGS--W exceptions=y}

cat > "$WORK/diluvium_wasi" <<EOF
#!/bin/sh
exec $RUNTIME run $RUNTIME_FLAGS --dir / ${WASI_DIRS:-} "$WASM" "\$@"
EOF
chmod +x "$WORK/diluvium_wasi"

echo "wasi_check: $RUNTIME $($RUNTIME --version 2>&1 | head -1)"
echo "wasi_check: module $WASM"
echo ""

echo "=== version banner ==="
"$WORK/diluvium_wasi" -v

echo ""
echo "=== Diluvium constructs, through the wasm ==="
# shellcheck disable=SC2086 -- the list is meant to split
"$REPO_ROOT/test/run_tests.sh" --bin "$WORK/diluvium_wasi" $WASI_TESTS

# ---------------------------------------------------------------------------
# Instruction counts are meant to be architecture-independent. Check it.
# ---------------------------------------------------------------------------
if [ -n "$NATIVE" ]; then
  echo ""
  echo "=== benchmark counts: wasm32 against native ==="
  # Full scale on both. Scaling down looked tempting for wasm's sake, but
  # the smaller cases then round to zero against the hook interval and the
  # comparison silently stops saying anything about them.
  "$WORK/diluvium_wasi" "$REPO_ROOT/script/bench.lua" --json > "$WORK/wasm.json"
  "$NATIVE" "$REPO_ROOT/script/bench.lua" --json > "$WORK/native.json"

  WASM_JSON="$WORK/wasm.json" NATIVE_JSON="$WORK/native.json" python3 - <<'PY'
import json, os, sys

w = json.load(open(os.environ["WASM_JSON"]))["cases"]
n = json.load(open(os.environ["NATIVE_JSON"]))["cases"]
bad = []
for name in sorted(set(w) & set(n)):
    wi, ni = w[name]["instructions"], n[name]["instructions"]
    flag = "" if wi == ni else "  <-- DIFFERS"
    if wi != ni:
        bad.append((name, ni, wi))
    elif ni == 0:
        print(f"  {name:<14} WARNING: zero instructions counted, "
              f"nothing was compared")
    print(f"  {name:<14} native {ni:>12d}   wasm {wi:>12d}{flag}")
missing = sorted(set(w) ^ set(n))
if missing:
    print(f"  (present in only one: {', '.join(missing)})")
if bad:
    print("\nFAIL: instruction counts differ between wasm32 and native.")
    print("Counts are supposed to describe the bytecode, not the machine, so")
    print("a difference means either the two builds compiled the same source")
    print("differently or one of them is not running the same code.")
    sys.exit(1)
print("\n  counts identical on both targets")
PY
fi

echo ""
echo "wasi_check: OK"
