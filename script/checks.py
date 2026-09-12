#!/usr/bin/env python3
"""Diluvium's bespoke consistency invariants.

The changelog tool calls consistency(doc, ctx) if this file exists. What
lives here is what does NOT generalise to the other Aloecraft repositories:
the constants Diluvium inherits from embedding Lua, each tied by NAME to the
fact the newest changelog entry records -- never to Diluvium's own version.
See doc/ALIGNMENT.md sections 1 and 3.

Surface -------------------------------------------------------------------
Entry point:
    consistency(doc, ctx) -> list[str]   problems; empty when the tree agrees

Invariants (each a recorded fact vs. the tree, by name):
    bytecode_format  <->  LUAC_FORMAT       in src/lundump.h
    lua_base         <->  LUA_VERSION_*_N   in src/lua.h

ctx, supplied by the tool (a dict, matching the shared engine's contract):
    ctx["read"](relpath) -> str   read a repo file, path relative to the root
    ctx["root"]                   absolute repository root
    ctx["base"]                   the newest entry's base version, 'X.Y.Z'

Why by name and not by digits: lua_base is the Lua release Diluvium forks,
recorded as a fact; it is NOT Diluvium's version. The two were the same
string through 5.5.1_build14 and diverge at 0.15.0, when Diluvium took its
own version line. Equating them -- which this repo's changelog.py once did,
inline -- is the weld doc/ALIGNMENT.md removes. Diluvium's own version is
checked against VERSION and .technoproj by the caller, not here.
---------------------------------------------------------------------------
"""
import re


def consistency(doc, ctx):
    read = ctx["read"]
    bad = []
    r = doc["releases"][0]
    where = "newest entry (%s)" % r["version"]

    # depth: bytecode_format <-> LUAC_FORMAT, compared as integers so 0x46 and
    # 70 are one value.
    want_fmt = r.get("bytecode_format")
    fmt = re.search(r"^#define\s+LUAC_FORMAT\s+(\S+)", read("src/lundump.h"), re.M)
    if want_fmt is None:
        bad.append("%s: no bytecode_format recorded to check LUAC_FORMAT against"
                   % where)
    elif not fmt:
        bad.append("src/lundump.h: no LUAC_FORMAT")
    elif int(fmt.group(1), 0) != want_fmt:
        bad.append("src/lundump.h LUAC_FORMAT is %s but %s records "
                   "bytecode_format %s" % (fmt.group(1), where, hex(want_fmt)))

    # depth: lua_base <-> the three LUA_VERSION_*_N. lua_base is the upstream
    # Lua release recorded as a fact -- NOT Diluvium's own version.
    lua_base = r.get("lua_base")
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)$",
                 str(lua_base if lua_base is not None else ""))
    if not m:
        bad.append("%s: lua_base %r is not X.Y.Z" % (where, lua_base))
    else:
        lua_h = read("src/lua.h")
        for key, want in zip(("MAJOR", "MINOR", "RELEASE"), m.groups()):
            got = re.search(r"^#define\s+LUA_VERSION_%s_N\s+(\d+)" % key,
                            lua_h, re.M)
            if not got:
                bad.append("src/lua.h: no LUA_VERSION_%s_N" % key)
            elif got.group(1) != want:
                bad.append("src/lua.h LUA_VERSION_%s_N is %s but %s records "
                           "lua_base %s" % (key, got.group(1), where, lua_base))
    return bad
