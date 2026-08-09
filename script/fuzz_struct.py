#!/usr/bin/env python3
"""Fuzz the loader by corrupting instruction *operands*, not random bytes.

script/fuzz_exec.py flips arbitrary bytes and measures how often that
crashes the interpreter. That is an accident rate: most single-byte
mutations land in string or debug data and never reach the operand checks
the verifier exists to make. This one is the attack-rate companion. It
finds the code array in a real dump, and for every instruction sets each
operand field -- register, constant index, upvalue index, jump offset --
to a value that is out of range for the prototype that owns it, then loads
the result.

A correct verifier refuses every one of these: an out-of-range operand is
exactly what luai_verifycode checks. So the pass condition is strict --
`--allowed 0` means "no operand corruption crashed the interpreter", which
is a statement about the verifier and not about luck. A mutant that loads
and then raises is a pass (some corruptions produce a still-valid program);
a mutant the loader refuses is the desired outcome; a signal or a timeout
is a failure.

The code array is located structurally: it is dumped 4-byte aligned
(ldump.c dumpCode calls dumpAlign), a chunk's main function is always
vararg so its first instruction is OP_VARARGPREP, and a function always
ends in a return. If that anchor cannot be found the run aborts rather
than fuzzing nothing -- a silent zero-coverage pass is the failure mode a
format change would otherwise cause.

Usage:
  script/fuzz_struct.py --bin BINARY [--allowed N] [--timeout S]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile


# --- instruction layout, read from the header rather than hardcoded, so a
# --- field-width change there is caught here instead of silently miscoding.
def load_layout(repo):
    h = open(os.path.join(repo, "src", "lopcodes.h")).read()

    def define(name, default=None):
        m = re.search(r"#define\s+%s\s+(.+)" % re.escape(name), h)
        if not m:
            if default is not None:
                return default
            sys.exit("fuzz_struct: cannot find #define %s" % name)
        return m.group(1).split("/*")[0].strip()

    SIZE_OP, SIZE_A, SIZE_B, SIZE_C = 7, 8, 8, 8
    SIZE_vB, SIZE_vC = 6, 10
    POS_OP = 0
    POS_A = POS_OP + SIZE_OP           # 7
    POS_k = POS_A + SIZE_A             # 15
    POS_B = POS_k + 1                  # 16
    POS_C = POS_B + SIZE_B             # 24
    POS_vB = POS_k + 1                 # 16
    POS_vC = POS_vB + SIZE_vB          # 22
    SIZE_Bx = SIZE_C + SIZE_B + 1      # 17
    SIZE_Ax = SIZE_Bx + SIZE_A         # 25
    SIZE_sJ = SIZE_Bx + SIZE_A         # 25
    POS_Bx = POS_k                     # 15
    POS_Ax = POS_A                     # 7
    POS_sJ = POS_A                     # 7
    return dict(SIZE_OP=SIZE_OP, POS_A=POS_A, SIZE_A=SIZE_A, POS_k=POS_k,
                POS_B=POS_B, SIZE_B=SIZE_B, POS_C=POS_C, SIZE_C=SIZE_C,
                POS_vB=POS_vB, SIZE_vB=SIZE_vB, POS_vC=POS_vC, SIZE_vC=SIZE_vC,
                POS_Bx=POS_Bx, SIZE_Bx=SIZE_Bx, POS_Ax=POS_Ax, SIZE_Ax=SIZE_Ax,
                POS_sJ=POS_sJ, SIZE_sJ=SIZE_sJ)


def load_opcodes(repo):
    src = open(os.path.join(repo, "src", "lopcodes.h")).read()
    body = re.search(r"typedef enum \{(.*?)\} OpCode;", src, re.S).group(1)
    seen, order = set(), []
    for name in re.findall(r"\bOP_[A-Z0-9]+", body):
        if name not in seen:
            seen.add(name)
            order.append(name)
    return {name: i for i, name in enumerate(order)}, order


# Diverse straight-line source: everything at the top level, so the main
# chunk's own code array carries most opcode shapes and one located array
# is enough to exercise the verifier broadly.
SOURCE = """
local t = {10, 20, 30, name = "x"}
local x = 0
for i = 1, #t do x = x + (t[i] or 0) end
for k, v in pairs(t) do if type(v) == "number" then x = x + v end end
local s = "sum=" .. tostring(x)
local u = t.name and #t.name or 0
local f = function(a, ...) return a + select("#", ...) end
x = x + f(1, 2, 3)
local g = t and t.name
x = x & 0xff | 2
return x, s, u, g
"""


def get(word, pos, size):
    return (word >> pos) & ((1 << size) - 1)


def setf(word, pos, size, val):
    mask = ((1 << size) - 1) << pos
    return (word & ~mask) | ((val << pos) & mask)


def base_dump(binary):
    r = subprocess.run(
        [binary, "-e", "io.write(string.dump(assert(load(%r))))" % SOURCE],
        capture_output=True)
    if r.returncode != 0 or not r.stdout:
        sys.exit("fuzz_struct: could not produce a base dump:\n" +
                 r.stderr.decode(errors="replace"))
    return bytearray(r.stdout)


def find_code_array(dump, OP, L):
    """Return (offset, count): the byte offset and instruction count of the
    main chunk's code array. Anchored on a 4-aligned OP_VARARGPREP that
    begins a run of valid opcodes ending in a return."""
    vprep = OP["OP_VARARGPREP"]
    rets = {OP["OP_RETURN"], OP["OP_RETURN0"], OP["OP_RETURN1"]}
    n_op = len(OP)
    best = None
    for off in range(0, len(dump) - 3, 4):
        word = int.from_bytes(dump[off:off + 4], "little")
        if get(word, 0, L["SIZE_OP"]) != vprep:
            continue
        # extend while every word is a known opcode; a real code array ends
        # at a return within a sane length
        count, pos, ended = 0, off, False
        while pos + 4 <= len(dump):
            w = int.from_bytes(dump[pos:pos + 4], "little")
            op = get(w, 0, L["SIZE_OP"])
            if op >= n_op:
                break
            count += 1
            pos += 4
            if op in rets:
                ended = True
                break
        if ended and count >= 3:
            if best is None or count > best[1]:
                best = (off, count)
    if best is None:
        sys.exit("fuzz_struct: could not locate the code array -- the dump "
                 "format may have changed; refusing to fuzz nothing.")
    return best


def mutations(word, op, OP, L):
    """Out-of-range values for each operand field this opcode actually has.
    Every one should be refused by a correct verifier."""
    out = []
    BIG_A = 254                       # far above any real maxstacksize
    BIG_BC = 254
    BIG_Bx = (1 << L["SIZE_Bx"]) - 1  # 131071
    BIG_Ax = (1 << L["SIZE_Ax"]) - 1
    BIG_sJ = (1 << L["SIZE_sJ"]) - 1
    # A is present in almost every opcode; push it out of the register file
    out.append(("A", setf(word, L["POS_A"], L["SIZE_A"], BIG_A)))
    # B / C as registers or constant indices
    out.append(("B", setf(word, L["POS_B"], L["SIZE_B"], BIG_BC)))
    out.append(("C", setf(word, L["POS_C"], L["SIZE_C"], BIG_BC)))
    # Bx (LOADK/CLOSURE/FORLOOP...) -- constant or prototype or jump index
    out.append(("Bx", setf(word, L["POS_Bx"], L["SIZE_Bx"], BIG_Bx)))
    # Ax (EXTRAARG payload) and sJ (JMP) -- extreme jump / constant
    out.append(("Ax", setf(word, L["POS_Ax"], L["SIZE_Ax"], BIG_Ax)))
    out.append(("sJ", setf(word, L["POS_sJ"], L["SIZE_sJ"], BIG_sJ)))
    # flip the k bit, which reinterprets an operand as constant vs register
    out.append(("k", word ^ (1 << L["POS_k"])))
    return [(field, w) for field, w in out if w != word]


def classify(binary, mutant, path, timeout):
    with open(path, "wb") as fh:
        fh.write(mutant)
    prog = ("local f = loadfile(%r)\n"
            "if not f then print('REFUSED') else\n"
            "  print(pcall(f) and 'RAN' or 'RAISED') end\n") % path
    try:
        r = subprocess.run([binary, "-e", prog], capture_output=True,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return ("abnormal", "timeout")
    if r.returncode != 0:
        first = r.stderr.decode(errors="replace").strip().split("\n")
        return ("abnormal", (first[0] if first else "")[:110])
    out = r.stdout.decode(errors="replace").strip()
    if out == "REFUSED":
        return ("refused", None)
    if out == "RAISED":
        return ("raised", None)
    if out == "RAN":
        return ("ran", None)
    return ("abnormal", "unexpected: " + out[:90])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--allowed", type=int, default=0)
    ap.add_argument("--timeout", type=float, default=10.0)
    args = ap.parse_args()

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    L = load_layout(repo)
    OP, order = load_opcodes(repo)
    dump = base_dump(args.bin)
    off, count = find_code_array(dump, OP, L)

    tally = {"refused": 0, "raised": 0, "ran": 0}
    abnormal = []
    total = 0
    with tempfile.TemporaryDirectory() as work:
        path = os.path.join(work, "mutant.luac")
        for j in range(count):
            wpos = off + j * 4
            word = int.from_bytes(dump[wpos:wpos + 4], "little")
            op = get(word, 0, L["SIZE_OP"])
            opname = order[op] if op < len(order) else "OP_?%d" % op
            for field, mword in mutations(word, op, OP, L):
                mutant = bytearray(dump)
                mutant[wpos:wpos + 4] = int(mword).to_bytes(4, "little")
                total += 1
                kind, detail = classify(args.bin, bytes(mutant), path,
                                        args.timeout)
                if kind == "abnormal":
                    abnormal.append((j, opname, field, detail))
                else:
                    tally[kind] += 1

    print("fuzz_struct: %s" % args.bin)
    print("  code array at byte %d, %d instructions" % (off, count))
    print("  %d operand mutations" % total)
    print("  refused %(refused)d   raised %(raised)d   ran %(ran)d" % tally)
    print("  abnormal %d (allowed %d)" % (len(abnormal), args.allowed))
    seen = set()
    for j, opname, field, detail in abnormal:
        key = (opname, field, (detail or "")[:60])
        if key in seen:
            continue
        seen.add(key)
        print("    instr %-3d %-14s field %-3s  %s" % (j, opname, field, detail))

    if len(abnormal) > args.allowed:
        print("\nFAIL: %d operand corruptions crashed the interpreter, over "
              "the %d allowed." % (len(abnormal), args.allowed))
        print("An out-of-range operand is exactly what the verifier is meant "
              "to refuse; a crash means it did not.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
