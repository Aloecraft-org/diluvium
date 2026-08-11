/*
** dlibs.c
** Registration for Diluvium's own guest libraries. See dlibs.h.
*/

#define dlibs_c

#include "lprefix.h"

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"
#include "dlibs.h"
#include "dtask.h"
#include "dmsgpack.h"
#include "dqueue.h"
#include "dendpoint.h"


static const luaL_Reg diluvium_libs[] = {
  {"msgpack", luaopen_dmsgpack},
  {"queue", luaopen_dqueue},
  {"endpoint", luaopen_dendpoint},
  {NULL, NULL}
};


LUA_API void diluvium_openlibs (lua_State *L) {
  /* Name the driver's continuation. Not a library, so it has no open function of
     its own to do it, and it has to happen before any snapshot is loaded. */
  diluvium_task_registerconts();
  const luaL_Reg *lib;
  for (lib = diluvium_libs; lib->name != NULL; lib++) {
    luaL_requiref(L, lib->name, lib->func, 1);  /* set a global too */
    lua_pop(L, 1);  /* 'luaL_requiref' leaves the module on the stack */
  }
}


/* ======================================================================
** The debug library, narrowed
** ====================================================================== */

/*
** Why this exists.
**
** Everything else in this runtime that hands a program authority does it
** deliberately: a queue handle comes from 'queue.declare', an endpoint comes
** from a reference the host authorised, a budget is set before the program
** starts. The 'debug' library is a hole underneath all of it, because its job
** is to reach past the abstractions the rest of the runtime is built out of.
** Three of its functions defeat named guarantees in doc/Messaging.md:
**
**   'debug.getregistry' returns the registry, which is where every anchor the
**   runtime has lives -- the instance pointer, the queue table, and the private
**   metatable that 7.3 says makes an endpoint reference unforgeable. A guest
**   reads that metatable out by its own '__name', wraps guessed peer bytes in a
**   table wearing it, and 'endpoint.bind' hands back a live handle to a peer it
**   was never given. That is finding 6 of the M0-M7 audit, reproduced end to
**   end.
**
**   'debug.getmetatable' walks past '__metatable', which is the other half of
**   the same claim.
**
**   'debug.sethook' takes the one hook slot a lua_State has, and 9.4's
**   instruction budget is a count hook in it. 'debug.sethook()' -- no
**   arguments, the documented way to clear a hook -- disarms the budget
**   entirely: an instance limited to 200,000 instructions runs three million
**   and reports 'insn_used' of nought. A budget stored, reported by an
**   accessor, and enforced by nothing is the defect the audit found twice
**   elsewhere; this is the guest-side third.
**
** The rest go for reasons that are the same in kind. The line is: a program may
** read its own frames, and may not write anything or reach outside itself.
** 'getinfo', 'getlocal', 'gethook' and 'traceback' are on the reading side of
** it and stay -- a traceback is how a program reports its own failure, and
** taking that away would buy nothing.
**
** Not a deletion. A missing field reports "attempt to call a nil value (field
** 'sethook')", which tells the author of a program nothing about what to do
** instead, and the refusal is the only documentation that reaches them. Keeping
** the names also keeps the permanents fingerprint (10.4) unchanged -- it hashes
** the sorted *names* the module walk finds -- so this change does not make
** every 5.5.1_build3 snapshot unrestorable, which deleting the fields would
** have done for no gain.
**
** A host whose programs are its own (profile A) can have the whole library back
** with DV_FLAG_UNSAFE_DEBUG. It is spelled that way on purpose.
*/
static int db_refused (lua_State *L) {
  return luaL_error(L, "%s", lua_tostring(L, lua_upvalueindex(1)));
}


/* Name, and the half-sentence that goes after the colon. */
static const char *const DB_REFUSED[] = {
  "debug",
    "it reads the host's standard input and runs what it finds, and that "
    "terminal belongs to the host rather than to the program",
  "getregistry",
    "the registry holds every anchor this runtime has, including the metatable "
    "that makes an endpoint reference a capability rather than a guessable name",
  "getmetatable",
    "it reads a metatable that '__metatable' says is not readable, which is "
    "what the runtime relies on to tell a reference it made from a lookalike",
  "setmetatable",
    "it attaches a metatable to a value that refused one, so a program could "
    "wear an identity the runtime issues rather than being given it",
  "getupvalue",
    "an upvalue is private to the closure holding it, and '_ENV' is one of "
    "them, so this reads the environment of any function it can name",
  "setupvalue",
    "it writes an upvalue of any closure, '_ENV' included",
  "upvalueid",
    "it distinguishes upvalues the closures holding them do not expose",
  "upvaluejoin",
    "it makes one closure's upvalue be another's, which rewrites a function "
    "the caller does not own",
  "getuservalue",
    "a userdata's values belong to the C code that created it",
  "setuservalue",
    "it writes values into a userdata the program did not create",
  "sethook",
    "a lua_State has one hook slot and 9.4's instruction budget is in it, so "
    "setting a hook here would switch the budget off",
  "setlocal",
    "it writes a local of any active frame, including the C frame of a "
    "library function that is part-way through a call into this program",
  NULL, NULL
};


/*
** 'luaopen_debug' builds the full table; this narrows it before anything can
** hold a reference to the wide one, and returns the same table. Called through
** 'luaL_requiref', so what lands in 'package.loaded' is the narrowed table too
** and there is no second route to the originals.
*/
static int db_openrestricted (lua_State *L) {
  int i;
  luaopen_debug(L);   /* the real library, on top of the stack */
  for (i = 0; DB_REFUSED[i] != NULL; i += 2) {
    lua_pushfstring(L, "debug.%s is not available inside a Diluvium instance: "
                       "%s. A host that wrote the program itself can have the "
                       "whole library back with DV_FLAG_UNSAFE_DEBUG.",
                    DB_REFUSED[i], DB_REFUSED[i + 1]);
    lua_pushcclosure(L, db_refused, 1);
    lua_setfield(L, -2, DB_REFUSED[i]);
  }
  return 1;
}


LUA_API void diluvium_openguestlibs (lua_State *L, unsigned int flags) {
  if (flags & DILUVIUM_GUEST_FULL_DEBUG) {
    luaL_openlibs(L);
  }
  else {
    /* Everything but 'debug', and then 'debug' as the narrowed table. Opening
       the full one first and overwriting fields afterwards would work too, and
       is not what this does: it would leave a window in which the wide table is
       the value in 'package.loaded', and a future caller between the two steps
       would find it. */
    luaL_openselectedlibs(L, ~0 & ~LUA_DBLIBK, 0);
    luaL_requiref(L, LUA_DBLIBNAME, db_openrestricted, 1);
    lua_pop(L, 1);
  }
  diluvium_openlibs(L);
}
