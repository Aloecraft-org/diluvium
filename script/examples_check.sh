#!/bin/sh
#
# Run every example the way its README says a reader runs it, and compare
# what it prints with its expected.txt. An example that drifts from the
# runtime is a lie told to the one reader who believed it.
#
#   script/examples_check.sh [--bin path/to/diluvium]
#
# Exit status is 0 only when every example printed exactly what it expects.

set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="$ROOT/dist/diluvium_debug"
[ "${1:-}" = "--bin" ] && BIN=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")

failed=0
for dir in "$ROOT"/examples/*/; do
  [ -f "$dir/expected.txt" ] || continue
  name=$(basename "$dir")
  if (cd "$dir" && "$BIN" app.dlua) > "$dir/.actual" 2>&1 \
     && diff -u "$dir/expected.txt" "$dir/.actual"; then
    echo "PASS  $name"
  else
    echo "FAIL  $name"
    failed=1
  fi
  rm -f "$dir/.actual"
  (cd "$dir" && git clean -qfX . 2>/dev/null) || true  # what the run wrote
done
exit $failed
