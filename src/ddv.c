/*
** ddv.c
** The 'dv' library. See ddv.h for why it exists.
**
** ------------------------------------------------------------- surface --
**
** Entry points:
**   luaopen_ddv     registers the table
**
** Configurable values:
**   DVL_SLICE       the metamethod name a value may carry
**
** Fan-out points:
**   dvlib           the library's functions
**   dv_slice        the three cases a slice can be: a metamethod, a
**                   string, a table
*/

#define ddv_c

#include "lprefix.h"

#include <stddef.h>

#include "lua.h"

#include "lauxlib.h"
#include "ddv.h"


#define DVL_SLICE	"__slice"


/*
** Resolve one end of a slice.
**
** Absent means "as far as it goes", which is what makes 'xs[3:]' and
** 'xs[:n]' read the way they do. A negative index counts from the end,
** as 'string.sub' does -- the one convention a Lua programmer already
** has for this, and adopting a second would be gratuitous.
*/
static lua_Integer dv_slicebound (lua_State *L, int idx, lua_Integer len,
                                  lua_Integer dflt) {
  lua_Integer n;
  if (lua_isnoneornil(L, idx))
    return dflt;
  n = luaL_checkinteger(L, idx);
  if (n < 0)
    n = len + n + 1;
  return n;
}


/*
** dv.slice(v, i, j) -- the runtime half of 'v[i:j]'.
**
** Three cases, and the order matters. A '__slice' metamethod wins,
** always: that is how the 'array' type returns a view rather than a copy
** and how any other object says what a range of it means. Then a string,
** which is 'string.sub'. Then a table, which is a copy of the range --
** a copy and not a view, because a plain table has nothing to view
** through and a form that sometimes aliased and sometimes did not would
** be worse than one that never does.
**
** Anything else is an error naming what it got, rather than an empty
** result: 'nil[1:2]' is a mistake, not an empty slice.
*/
static int dv_slice (lua_State *L) {
  lua_Integer i, j, len;
  luaL_checkany(L, 1);
  if (luaL_getmetafield(L, 1, DVL_SLICE) != LUA_TNIL) {
    /* The metamethod decides. Handed the same three arguments, including
       the nils, so it can tell 'xs[3:]' from 'xs[3:#xs]' if it wants to. */
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 3);
    lua_call(L, 3, 1);
    return 1;
  }
  if (lua_type(L, 1) == LUA_TSTRING) {
    size_t l;
    const char *s = lua_tolstring(L, 1, &l);
    len = (lua_Integer)l;
    i = dv_slicebound(L, 2, len, 1);
    j = dv_slicebound(L, 3, len, len);
    if (i < 1) i = 1;
    if (j > len) j = len;
    if (i > j)
      lua_pushliteral(L, "");
    else
      lua_pushlstring(L, s + (size_t)(i - 1), (size_t)(j - i + 1));
    return 1;
  }
  if (lua_type(L, 1) == LUA_TTABLE) {
    lua_Integer k;
    len = (lua_Integer)luaL_len(L, 1);
    i = dv_slicebound(L, 2, len, 1);
    j = dv_slicebound(L, 3, len, len);
    if (i < 1) i = 1;
    if (j > len) j = len;
    lua_createtable(L, (j >= i) ? (int)(j - i + 1) : 0, 0);
    for (k = i; k <= j; k++) {
      lua_geti(L, 1, k);
      lua_seti(L, -2, k - i + 1);
    }
    return 1;
  }
  return luaL_error(L, "cannot slice a %s value", luaL_typename(L, 1));
}


static const luaL_Reg dvlib[] = {
  {"slice", dv_slice},
  {NULL, NULL}
};


LUAMOD_API int luaopen_ddv (lua_State *L) {
  luaL_newlib(L, dvlib);
  return 1;
}
