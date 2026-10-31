/*
** dlocalhost.h
** The standalone binary's own host. See dlocalhost.c.
**
** On-top code: public Lua C API only, nothing in the core patch series.
*/

#ifndef dlocalhost_h
#define dlocalhost_h

#include "lua.h"

/* Replace the 'host' global (and package.loaded.host) with a host library
   whose calls are answered in this process, from Lua's own libraries. For
   the standalone interpreter only: an embedding host never calls it. */
LUA_API void diluvium_localhost_install (lua_State *L);

#endif
