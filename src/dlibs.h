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


/*
** The whole library set an *instance* gets: the standard libraries, with
** 'debug' narrowed, and then Diluvium's own.
**
** Separate from 'diluvium_openlibs' because the narrowing is not wanted
** everywhere. The CLI and the browser build both host a program that is the
** person at the keyboard's own, and the upstream test suite drives 'debug'
** hard; those two callers keep 'luaL_openlibs'. An instance is the case where
** the program came from somewhere else, and that is where the standard set
** stops being a reasonable default -- see the narrowing's own comment in
** dlibs.c for which functions go and why each one has to.
*/
#define DILUVIUM_GUEST_FULL_DEBUG	0x1u

/*
** Leave out `io`, `os` and `package` as well: the three libraries that reach
** anything outside this state's own memory. What is left is the language, the
** queues, and whatever the host pushes into them -- which is the model 7.1
** describes, made true rather than assumed.
*/
#define DILUVIUM_GUEST_SEALED		0x2u

LUA_API void diluvium_openguestlibs (lua_State *L, unsigned int flags);

#endif
