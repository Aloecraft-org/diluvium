#!/usr/bin/env python3
"""The two source lists must name the same Diluvium C files.

A Diluvium runtime file (src/d*.c) is built two ways: compiled on its own and
linked from src/makefile's AUX_O, and #included into the src/onelua.c
amalgamation that the tests, the host and the wasm build use. Add a file to one
and forget the other, and one path fails to link while the other is fine -- the
shape that made `make build_platform` fail on dbytes.o and surfaced three
layers away as a fuzzer error. This makes the divergence loud, at the source,
before a build hides it.

Scope: the 'd*.c' files only, and only the ones the amalgamation guards behind
`#ifndef MAKE_LUAC` (the runtime set dlibs registers). Core Lua files and the
compiler-only pieces are each single-listed by design and not compared.

Exit 0 when the two agree, 1 with a diff when they do not.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"

# Files that are in the per-file build's object list but intentionally NOT
# #included in the amalgamation, because they are their own translation unit
# there. 'diluvium_api' is compiled beside onelua.c for the luac build (see
# build_platform), not inside it. A new entry here needs a reason.
EXCLUDE = {"diluvium_api"}

# The runtime d*.c set, as onelua.c includes it inside the MAKE_LUAC guard.
# dv.c is included there too but is intentionally NOT in this comparison set:
# it is listed in both places already and has no header-registration twin. We
# compare the guest/runtime support files that dlibs and dsnap depend on.
def amalgamation_includes(text):
    # Only the block that is compiled into the library (not the compiler-only
    # or test includes). Grab every #include "dXXX.c".
    inc = set(re.findall(r'#include\s+"(d[a-z0-9_]+\.c)"', text))
    return {name[:-2] for name in inc}          # strip the .c


def makefile_objects(text):
    # AUX_O is a backslash-continued assignment; join its lines, then take the
    # d*.o basenames.
    m = re.search(r'^AUX_O=(.*?)(?=^\w+=|\Z)', text, re.M | re.S)
    if not m:
        sys.exit("check_source_lists: no AUX_O in src/makefile")
    body = m.group(1).replace("\\\n", " ")
    return {tok[:-2] for tok in body.split() if re.fullmatch(r'd[a-z0-9_]+\.o', tok)}


def main():
    one = amalgamation_includes((SRC / "onelua.c").read_text())
    mk = makefile_objects((SRC / "makefile").read_text()) - EXCLUDE
    if one == mk:
        print("check_source_lists: %d runtime files, both lists agree"
              % len(one))
        return 0
    only_one = sorted(one - mk)
    only_mk = sorted(mk - one)
    print("check_source_lists: the two source lists disagree", file=sys.stderr)
    if only_one:
        print("  in onelua.c but not src/makefile AUX_O: %s"
              % ", ".join(only_one), file=sys.stderr)
    if only_mk:
        print("  in src/makefile AUX_O but not onelua.c: %s"
              % ", ".join(only_mk), file=sys.stderr)
    print("  add the missing file to both, or the per-file build and the "
          "amalgamation will disagree at link time", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
