#!/bin/sh
#
# Diluvium build statistics.
#
# Records what a build produced -- the size of every artifact, and where
# inside the native ones that size lives -- so a jump in binary size can be
# traced to the change that caused it instead of being noticed months later.
#
# The release build already writes BUILDINFO.txt with the commit and a
# SHA256 line per artifact. This adds the two things that were missing from
# it: sizes and timings.
#
# Usage:
#   script/build_stats.sh record [--dist DIR] [--out FILE] [--seconds N]
#                                [--bench BIN]
#       One JSON object describing DIR (default: dist/). '--seconds' records
#       how long the build took, when the caller knows. '--bench' runs
#       script/bench.lua under BIN and embeds the result.
#
#   script/build_stats.sh compare OLD.json NEW.json [--fail-over BYTES]
#                                 [--fail-over-instr PERCENT]
#       A table of what changed, in size and in benchmark instruction
#       counts. Exits non-zero if any artifact grew by more than BYTES or
#       any benchmark by more than PERCENT, so CI can refuse a regression
#       rather than just log it.
#
#   script/build_stats.sh append RECORD.json FILE.jsonl
#       Add a record to the history, newest last.
#
# Sizes are only comparable within a toolchain, so each record carries the
# compiler version: an unexplained jump is usually the runner image moving,
# and that is much easier to see than to remember.
#
# Benchmarks carry the same caveat and one of their own. The comparable
# figure is the instruction count, which is deterministic: the same code on
# the same build gives the same number on any machine, so a change in it is
# a change in work done. Times are recorded beside it but are advisory --
# a shared CI runner varies by more than most regressions worth catching.
# Counts are comparable only within a build configuration, since the debug
# build changes codegen parameters and so emits different bytecode.

set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

usage() { sed -n '3,27p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

# --- helpers ---------------------------------------------------------------

json_escape() {  # minimal: the values here are paths, versions and hashes
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

file_size() {
  # BSD stat and GNU stat disagree on flags; wc is on every box that has sh.
  wc -c < "$1" | tr -d ' '
}

# text/data/bss for an ELF or Mach-O binary, "" when 'size' cannot read it.
# This is the useful part: total bytes say a build grew, sections say whether
# it was code, tables or something that should not have been linked at all.
sections() {
  command -v size >/dev/null 2>&1 || return 0
  size "$1" 2>/dev/null | awk 'NR==2 { printf "%s %s %s", $1, $2, $3 }'
}

compiler_version() {
  if command -v cc >/dev/null 2>&1; then
    cc --version 2>/dev/null | head -1
  elif command -v gcc >/dev/null 2>&1; then
    gcc --version 2>/dev/null | head -1
  else
    echo "unknown"
  fi
}

# --- record ----------------------------------------------------------------

cmd_record() {
  dist="$REPO_ROOT/dist"
  out=/dev/stdout
  seconds=""
  bench=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --dist)    dist=$2; shift 2 ;;
      --out)     out=$2; shift 2 ;;
      --seconds) seconds=$2; shift 2 ;;
      --bench)   bench=$2; shift 2 ;;
      *) echo "build_stats: unknown option '$1'" >&2; exit 2 ;;
    esac
  done
  [ -d "$dist" ] || { echo "build_stats: no such directory: $dist" >&2; exit 2; }

  commit=$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)
  subject=$(git -C "$REPO_ROOT" log -1 --format=%s 2>/dev/null || echo unknown)
  committed=$(git -C "$REPO_ROOT" log -1 --format=%cI 2>/dev/null || echo unknown)
  version=$(cat "$REPO_ROOT/VERSION" 2>/dev/null || echo unknown)

  {
    printf '{\n'
    printf '  "schema": 1,\n'
    printf '  "version": "%s",\n'   "$(json_escape "$version")"
    printf '  "commit": "%s",\n'    "$(json_escape "$commit")"
    printf '  "subject": "%s",\n'   "$(json_escape "$subject")"
    printf '  "committed": "%s",\n' "$(json_escape "$committed")"
    printf '  "recorded": "%s",\n'  "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '  "os": "%s",\n'        "$(json_escape "$(uname -s)")"
    printf '  "arch": "%s",\n'      "$(json_escape "$(uname -m)")"
    printf '  "compiler": "%s",\n'  "$(json_escape "$(compiler_version)")"
    printf '  "run": "%s",\n'       "$(json_escape "${GITHUB_RUN_ID:-}")"
    [ -n "$seconds" ] && printf '  "build_seconds": %s,\n' "$seconds"
    printf '  "artifacts": {\n'
    first=1
    # Sorted, so two records diff cleanly.
    for f in $(ls "$dist" 2>/dev/null | sort); do
      [ -f "$dist/$f" ] || continue
      # The build's own metadata is not an artifact whose size means
      # anything, and BUILDSTATS.json cannot contain itself.
      case "$f" in
        SHA256SUMS.txt|BUILDINFO.txt|BUILDSTATS.json) continue ;;
      esac
      [ "$first" -eq 1 ] || printf ',\n'
      first=0
      sec=$(sections "$dist/$f")
      if [ -n "$sec" ]; then
        set -- $sec
        printf '    "%s": { "bytes": %s, "text": %s, "data": %s, "bss": %s }' \
          "$(json_escape "$f")" "$(file_size "$dist/$f")" "$1" "$2" "$3"
      else
        printf '    "%s": { "bytes": %s }' \
          "$(json_escape "$f")" "$(file_size "$dist/$f")"
      fi
    done
    [ "$first" -eq 0 ] && printf '\n'
    printf '  }'
    if [ -n "$bench" ]; then
      printf ',\n  "benchmarks":\n'
      # Indented two spaces so the embedded object reads as part of this one.
      "$bench" "$REPO_ROOT/script/bench.lua" --json | sed 's/^/  /'
    else
      printf '\n'
    fi
    printf '}\n'
  } > "$out"
  [ "$out" = /dev/stdout ] || echo "build stats written to $out" >&2
}

# --- compare ---------------------------------------------------------------

cmd_compare() {
  [ $# -ge 2 ] || usage
  old=$1; new=$2; shift 2
  failover=""
  failinstr=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --fail-over)       failover=$2; shift 2 ;;
      --fail-over-instr) failinstr=$2; shift 2 ;;
      *) echo "build_stats: unknown option '$1'" >&2; exit 2 ;;
    esac
  done
  [ -f "$old" ] || { echo "build_stats: no such file: $old" >&2; exit 2; }
  [ -f "$new" ] || { echo "build_stats: no such file: $new" >&2; exit 2; }

  OLD="$old" NEW="$new" FAILOVER="$failover" FAILINSTR="$failinstr" python3 - <<'PY'
import json, os, sys

old = json.load(open(os.environ["OLD"]))
new = json.load(open(os.environ["NEW"]))
limit = os.environ.get("FAILOVER") or ""
limit = int(limit) if limit else None
ilimit = os.environ.get("FAILINSTR") or ""
ilimit = float(ilimit) if ilimit else None

a, b = old.get("artifacts", {}), new.get("artifacts", {})
names = sorted(set(a) | set(b))

def kb(n): return f"{n/1024:.1f} KB"

rows, worst = [], 0
for n in names:
    ob = a.get(n, {}).get("bytes")
    nb = b.get(n, {}).get("bytes")
    if ob is None:   rows.append((n, "-", kb(nb), "added", nb)); worst = max(worst, nb or 0)
    elif nb is None: rows.append((n, kb(ob), "-", "removed", 0))
    else:
        d = nb - ob
        if d: worst = max(worst, d)
        pct = (d / ob * 100) if ob else 0
        mark = "" if d == 0 else f"{d:+d} B ({pct:+.2f}%)"
        rows.append((n, kb(ob), kb(nb), mark, d))

changed = [r for r in rows if r[3]]
print(f"{old.get('commit','?')[:8]} -> {new.get('commit','?')[:8]}"
      f"   {new.get('subject','')[:60]}")
if old.get("compiler") != new.get("compiler"):
    print(f"  NOTE: compiler changed, sizes are not directly comparable")
    print(f"        old: {old.get('compiler')}")
    print(f"        new: {new.get('compiler')}")
print()
if not changed:
    print("  no artifact changed size")
else:
    w = max(len(r[0]) for r in changed)
    print(f"  {'artifact'.ljust(w)}  {'before':>10}  {'after':>10}  change")
    print(f"  {'-'*w}  {'-'*10}  {'-'*10}  ------")
    for n, o, nn, mark, _ in changed:
        print(f"  {n.ljust(w)}  {o:>10}  {nn:>10}  {mark}")

# Sections, for anything that grew: total bytes say a build grew, sections
# say whether it was code or data.
for n in names:
    ov, nv = a.get(n, {}), b.get(n, {})
    if "text" in ov and "text" in nv:
        deltas = {k: nv[k] - ov[k] for k in ("text", "data", "bss")
                  if nv.get(k) is not None and ov.get(k) is not None}
        if any(deltas.values()):
            print(f"\n  {n}: " + ", ".join(f"{k} {v:+d}" for k, v in deltas.items() if v))

# Benchmarks. The instruction count is the comparable figure -- it is
# deterministic, so any movement is real work added or removed. Times are
# printed for context and never gate anything: a shared runner varies by
# more than the regressions worth catching.
oc = (old.get("benchmarks") or {}).get("cases", {})
nc = (new.get("benchmarks") or {}).get("cases", {})
iworst, iworst_name = 0.0, None
if oc and nc:
    shared = sorted(set(oc) & set(nc))
    moved = []
    for n in shared:
        oi, ni = oc[n].get("instructions"), nc[n].get("instructions")
        if not oi or ni is None or oc[n].get("iters") != nc[n].get("iters"):
            continue
        pct = (ni - oi) / oi * 100
        if abs(pct) >= 0.01:
            moved.append((n, oi, ni, pct))
            if pct > iworst:
                iworst, iworst_name = pct, n
    print()
    if not moved:
        print("  no benchmark changed instruction count")
    else:
        w = max(len(m[0]) for m in moved)
        print(f"  {'benchmark'.ljust(w)}  {'before':>12}  {'after':>12}  change")
        print(f"  {'-'*w}  {'-'*12}  {'-'*12}  ------")
        for n, oi, ni, pct in moved:
            print(f"  {n.ljust(w)}  {oi:>12d}  {ni:>12d}  {ni-oi:+d} ({pct:+.2f}%)")
    dropped = sorted(set(oc) ^ set(nc))
    if dropped:
        print(f"  (not compared, present in only one record: {', '.join(dropped)})")
elif ilimit is not None:
    print("\n  NOTE: no benchmarks in one of the records; nothing to gate on")

fail = False
if limit is not None and worst > limit:
    print(f"\nFAIL: an artifact grew by {worst} bytes, over the {limit} allowed")
    fail = True
if ilimit is not None and iworst > ilimit:
    print(f"\nFAIL: benchmark '{iworst_name}' grew {iworst:.2f}%, over the "
          f"{ilimit}% allowed")
    fail = True
sys.exit(1 if fail else 0)
PY
}

# --- append ----------------------------------------------------------------

cmd_append() {
  [ $# -eq 2 ] || usage
  rec=$1; hist=$2
  [ -f "$rec" ] || { echo "build_stats: no such file: $rec" >&2; exit 2; }
  # One object per line, so history is appended to rather than rewritten and
  # two builds landing at once conflict in a way git can actually resolve.
  python3 -c "
import json, sys
print(json.dumps(json.load(open(sys.argv[1])), separators=(',', ':')))
" "$rec" >> "$hist"
  echo "appended to $hist ($(wc -l < "$hist" | tr -d ' ') records)" >&2
}

# --- main ------------------------------------------------------------------

cmd=${1:-}
[ $# -gt 0 ] && shift
case "$cmd" in
  record)  cmd_record "$@" ;;
  compare) cmd_compare "$@" ;;
  append)  cmd_append "$@" ;;
  *) usage ;;
esac
