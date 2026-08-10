/*
** dlibs.h
** Registration for Diluvium's own guest libraries.
**
** This exists because 'linit.c' cannot host them. Its 'stdlibs' table is
** index-coupled to the LUA_<lib>K bitmask constants that
** 'luaL_openselectedlibs' takes, and it asserts on the count -- so adding an
** entry there would mean editing a core file that is not on the patch-series
** allowlist, and changing the meaning of every existing mask. One extra call
** from each embedder is cheaper and keeps the core patch series where it is.
**
** Every future guest library -- queue, endpoint, hibernate -- registers here
** rather than growing a second mechanism.
*/

#ifndef dlibs_h
#define dlibs_h

#include "lua.h"


/*
** Require Diluvium's libraries into globals. Call once, after the standard
** libraries are open: 'msgpack.encode' has to be able to see them, and a
** library that shadows a standard name would otherwise win by ordering.
*/
LUA_API void diluvium_openlibs (lua_State *L);

#endif
