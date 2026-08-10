/*
** dlibs.c
** Registration for Diluvium's own guest libraries. See dlibs.h.
*/

#define dlibs_c

#include "lprefix.h"

#include "lua.h"

#include "lauxlib.h"
#include "dlibs.h"
#include "dmsgpack.h"


static const luaL_Reg diluvium_libs[] = {
  {"msgpack", luaopen_dmsgpack},
  {NULL, NULL}
};


LUA_API void diluvium_openlibs (lua_State *L) {
  const luaL_Reg *lib;
  for (lib = diluvium_libs; lib->name != NULL; lib++) {
    luaL_requiref(L, lib->name, lib->func, 1);  /* set a global too */
    lua_pop(L, 1);  /* 'luaL_requiref' leaves the module on the stack */
  }
}
