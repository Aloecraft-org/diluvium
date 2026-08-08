#!/usr/bin/env python3
"""Fuzz the loader by *running* what it accepts.

script/fuzz_undump.lua mutates dumps and loads them, and reports clean. It
is not wrong -- it is testing the wrong half. Corrupting an instruction's
operand usually leaves a chunk that still loads, because nothing checks
operands against the prototype they belong to; the damage only appears
when the VM executes it. A fuzzer that never runs its mutants therefore
counts those as passes.

This one runs them, each in its own process, so a segfault is an
observation rather than the end of the run.

Usage:
  script/fuzz_exec.py --bin BINARY [--rounds N] [--seed N] [--allowed N]

Exit status is 0 when the number of abnormal terminations is at or below
--allowed (default 0). Set it deliberately and lower it as the loader is
hardened; never raise it to make a build green.

An abnormal termination is a signal, a timeout, or any exit status the
harness did not ask for. A mutant that loads and raises a Lua error is a
pass: refusing bad bytecode is the desired behaviour. A mutant that stays
valid and runs is also a pass, and says nothing either way.
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

# Small and dense: locals, a table constructor, indexing, arithmetic and a
# call. A short dump means a single byte lands somewhere structural far
# more often than in a long one padded with string data.
SOURCE = ("local function f(a, b) local t = {a, b} "
          "return t[1] + t[2] + #t end return f(1, 2)")


def base_dump(binary):
    r = subprocess.run(
        [binary, "-e", "io.write(string.dump(assert(load(%r))))" % SOURCE],
        capture_output=True)
    if r.returncode != 0 or not r.stdout:
        sys.exit("fuzz_exec: could not produce a base dump with %s" % binary)
    return bytearray(r.stdout)


def mutate(base, rng):
    b = bytearray(base)
    for _ in range(rng.choice([1, 1, 2])):
        b[rng.randrange(len(b))] = rng.choice(
            [0, 1, 2, 3, 0x7f, 0xff, rng.randrange(256)])
    return bytes(b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--rounds", type=int, default=500)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--allowed", type=int, default=0)
    ap.add_argument("--timeout", type=float, default=10.0)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    base = base_dump(args.bin)

    tally = {"rejected": 0, "ran": 0, "raised": 0}
    abnormal, seen = [], set()

    with tempfile.TemporaryDirectory() as work:
        path = os.path.join(work, "mutant.luac")
        for i in range(args.rounds):
            with open(path, "wb") as f:
                f.write(mutate(base, rng))
            prog = ("local f = loadfile(%r)\n"
                    "if not f then print('REJECTED') else\n"
                    "  local ok = pcall(f); print(ok and 'RAN' or 'RAISED')\n"
                    "end\n") % path
            try:
                r = subprocess.run([args.bin, "-e", prog],
                                   capture_output=True, timeout=args.timeout)
            except subprocess.TimeoutExpired:
                abnormal.append((i, "timeout", ""))
                continue
            out = r.stdout.decode(errors="replace").strip()
            if r.returncode != 0:
                first = r.stderr.decode(errors="replace").strip().split("\n")
                abnormal.append((i, r.returncode, first[0][:110] if first else ""))
            elif out == "REJECTED":
                tally["rejected"] += 1
            elif out == "RAN":
                tally["ran"] += 1
            elif out == "RAISED":
                tally["raised"] += 1
            else:
                abnormal.append((i, "unexpected output", out[:110]))

    print("fuzz_exec: %s" % args.bin)
    print("  %d rounds, seed %d, %d bytes per dump"
          % (args.rounds, args.seed, len(base)))
    print("  rejected %(rejected)d   ran %(ran)d   raised %(raised)d" % tally)
    print("  abnormal %d (allowed %d)" % (len(abnormal), args.allowed))

    for i, code, detail in abnormal:
        key = (code, detail[:80])
        if key in seen:
            continue
        seen.add(key)
        sig = ""
        if isinstance(code, int) and code < 0:
            sig = " (signal %d)" % -code
        print("    round %-5d %s%s  %s" % (i, code, sig, detail))

    if len(abnormal) > args.allowed:
        print("\nFAIL: %d abnormal terminations, over the %d allowed."
              % (len(abnormal), args.allowed))
        print("A corrupt chunk that loads and then crashes is the shape")
        print("luai_verifycode exists to prevent: operands are not checked")
        print("against the prototype that owns them.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
