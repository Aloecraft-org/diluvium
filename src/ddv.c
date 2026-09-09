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
**   dv_class        the four things a class table is given at creation
*/

#define ddv_c

#include "lprefix.h"

#include <stddef.h>

#include "lua.h"

#include "lauxlib.h"
#include "ddv.h"


#define DVL_SLICE	"__slice"

/*
** The key a class keeps its field defaults under (syntax proposals 5.1).
**
** Not '__'-prefixed on purpose: 'dv_class' copies the parent's
** '__'-prefixed entries into the child, and a copied defaults function
** would apply the parent's fields twice -- once here and once when the
** child's constructor reaches 'super'. Parenthesised so no source name
** can collide with it, which is the same trick 'lparser.c' uses for its
** hidden locals.
*/
#define DVL_DEFAULTS	"(defaults)"

/* How far 'dv.isa' will walk before deciding a chain is a cycle. A class
   hierarchy this deep is a bug either way; the limit is what stops a
   hand-made loop hanging the interpreter. */
#define DVL_MAXCHAIN	100


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


/*
** The parent of a class: the '__index' of its metatable, read raw.
**
** Pushes it and returns 1, or pushes nothing and returns 0. Raw because
** a class's metatable is this file's own and nothing should be able to
** interpose an '__index' on the walk.
*/
static int dv_parentof (lua_State *L, int idx) {
  if (!lua_getmetatable(L, idx))
    return 0;
  lua_pushliteral(L, "__index");
  if (lua_rawget(L, -2) == LUA_TNIL) {
    lua_pop(L, 2);
    return 0;
  }
  lua_remove(L, -2);   /* drop the metatable, keep the parent */
  return 1;
}


/*
** The constructor a class gets when its body does not declare 'new'.
**
** Upvalue 1 is the class, upvalue 2 its parent or nil. It applies this
** class's own field defaults and then hands the same arguments to the
** parent's constructor, which is the order syntax proposals 5.1 states:
** own defaults, then 'super'.
**
** The defaults are read with 'rawget' so an inherited defaults function
** is not run here as well as by the parent's own constructor.
*/
static int dv_newdefault (lua_State *L) {
  int nargs = lua_gettop(L);   /* self, and whatever the caller passed */
  luaL_checkany(L, 1);
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_pushliteral(L, DVL_DEFAULTS);
  if (lua_rawget(L, -2) == LUA_TFUNCTION) {
    lua_pushvalue(L, 1);
    lua_call(L, 1, 0);
  }
  else
    lua_pop(L, 1);
  lua_pop(L, 1);   /* the class */
  if (!lua_isnil(L, lua_upvalueindex(2))) {
    lua_getfield(L, lua_upvalueindex(2), "new");
    if (lua_isfunction(L, -1)) {
      int i;
      for (i = 1; i <= nargs; i++)
        lua_pushvalue(L, i);
      lua_call(L, nargs, 0);
    }
    else
      lua_pop(L, 1);
  }
  return 0;
}


/*
** '__call' on a class: 'Account("bob")' is an instance.
**
** A plain table with the class as its metatable, then 'new'. Nothing
** here is special to this file -- an instance is exactly what a
** hand-written Lua class library would have produced, which is the point
** of 5.1's "all plain metatables".
*/
static int dv_classcall (lua_State *L) {
  int nargs = lua_gettop(L);   /* the class, and the constructor's arguments */
  int i;
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_newtable(L);
  lua_pushvalue(L, 1);
  lua_setmetatable(L, -2);     /* setmetatable({}, cls) */
  lua_getfield(L, 1, "new");
  if (!lua_isfunction(L, -1)) {
    lua_getfield(L, 1, "__name");
    return luaL_error(L, "class '%s' has no constructor",
                      lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
  }
  lua_pushvalue(L, -2);        /* the instance */
  for (i = 2; i <= nargs; i++)
    lua_pushvalue(L, i);
  lua_call(L, nargs, 0);       /* new(o, ...) */
  return 1;                    /* the instance is back on top */
}


/*
** dv.class(name [, parent]) -- the table a 'class' statement declares.
**
** Four things happen here and the third is the one worth reading twice:
**
**   1. '__index' is the class itself and '__name' is its name, so an
**      instance finds its methods and 'tostring' says what it is.
**   2. The metatable carries '__index = parent' -- which is how a method
**      not on this class is looked up on its parent -- and '__call', the
**      constructor.
**   3. The parent's '__'-prefixed entries are *copied* in. Lua finds a
**      metamethod with a raw lookup on the metatable and does not follow
**      its '__index', so a child that inherited '__tostring' the ordinary
**      way would not have it when 'tostring' asked. This is the standard
**      inheritance gotcha, and 5.1 asks for it to be handled once, here,
**      rather than by every program. A child that declares its own
**      overwrites the copy, because the body's assignments run after
**      this call.
**   4. A default 'new' is installed. A body that declares one overwrites
**      it, for the same reason.
*/
static int dv_class (lua_State *L) {
  int hasparent = !lua_isnoneornil(L, 2);
  luaL_checkstring(L, 1);
  if (hasparent)
    luaL_checktype(L, 2, LUA_TTABLE);
  lua_settop(L, 2);            /* name, parent-or-none */
  lua_newtable(L);             /* the class */
  if (hasparent) {             /* 3: the parent's metamethods */
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
      if (lua_type(L, -2) == LUA_TSTRING) {
        const char *k = lua_tostring(L, -2);
        if (k[0] == '_' && k[1] == '_') {
          lua_pushvalue(L, -2);   /* the key */
          lua_pushvalue(L, -2);   /* the value */
          lua_rawset(L, -5);      /* class[k] = v */
        }
      }
      lua_pop(L, 1);           /* keep the key for 'lua_next' */
    }
  }
  lua_pushvalue(L, -1);        /* 1: class.__index = class */
  lua_setfield(L, -2, "__index");
  lua_pushvalue(L, 1);
  lua_setfield(L, -2, "__name");
  lua_pushvalue(L, -1);        /* 4: upvalue 1, the class itself */
  if (hasparent) lua_pushvalue(L, 2);
  else lua_pushnil(L);         /*    upvalue 2, the parent or nothing */
  lua_pushcclosure(L, dv_newdefault, 2);
  lua_setfield(L, -2, "new");
  lua_createtable(L, 0, 2);    /* 2: the class's own metatable */
  if (hasparent) {
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "__index");
  }
  lua_pushcfunction(L, dv_classcall);
  lua_setfield(L, -2, "__call");
  lua_pushvalue(L, 1);
  lua_setfield(L, -2, "__name");
  lua_setmetatable(L, -2);
  return 1;
}


/*
** dv.isa(obj, class) -- is 'obj' an instance of 'class', or of anything
** descended from it?
**
** The walk is: an instance's metatable is its class, and a class's
** parent is the '__index' of *its* metatable. Both are raw lookups, so
** nothing a program installs can make this answer differently than the
** method lookup does.
*/
static int dv_isa (lua_State *L) {
  int depth;
  luaL_checkany(L, 1);
  luaL_checkany(L, 2);
  if (!lua_getmetatable(L, 1)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  for (depth = 0; depth < DVL_MAXCHAIN; depth++) {
    if (lua_rawequal(L, -1, 2)) {
      lua_pushboolean(L, 1);
      return 1;
    }
    if (!dv_parentof(L, -1))
      break;
    lua_remove(L, -2);   /* the parent takes the child's place on the walk */
  }
  lua_pushboolean(L, 0);
  return 1;
}


static const luaL_Reg dvlib[] = {
  {"slice", dv_slice},
  {"class", dv_class},
  {"isa", dv_isa},
  {NULL, NULL}
};


LUAMOD_API int luaopen_ddv (lua_State *L) {
  luaL_newlib(L, dvlib);
  return 1;
}
