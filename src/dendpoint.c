/*
** dendpoint.c
** Endpoints. See dendpoint.h for the model and why it is this small.
**
** Separate from dqueue.c for two reasons. It is a different concern -- a queue is
** a buffer, an endpoint is a name for something outside -- and dqueue.c had
** reached 13.9 KB against a 15 KB budget, which this would have pushed over.
** Splitting on a real boundary while the number says to is better than splitting
** later on whatever line is convenient.
*/

#define dendpoint_c

#include "lprefix.h"

#include <stddef.h>

#include "lua.h"

#include "lauxlib.h"
#include "dendpoint.h"
#include "dmsgpack.h"
#include "dqueue.h"


/* Registry keys; their addresses are the keys, so they cannot collide. */
static const char DE_HANDLER = 0;   /* the host's bind callback */
static const char DE_TOKENS = 0;    /* token -> queue handle */
static const char DE_REF_MT = 0;    /* metatable for a reference */
static const char DE_ALLOWED = 0;   /* reference bytes -> token */

typedef struct de_handler_box {
  diluvium_endpoint_bind fn;
  void *ud;
} de_handler_box;


LUA_API void diluvium_endpoint_sethandler (lua_State *L,
                                           diluvium_endpoint_bind fn,
                                           void *ud) {
  if (fn == NULL)
    lua_pushnil(L);
  else {
    de_handler_box *box =
      (de_handler_box *)lua_newuserdatauv(L, sizeof(de_handler_box), 0);
    box->fn = fn;
    box->ud = ud;
  }
  lua_rawsetp(L, LUA_REGISTRYINDEX, &DE_HANDLER);
}


LUA_API void diluvium_endpoint_allow (lua_State *L, const char *ref, size_t len,
                                      unsigned int token) {
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DE_ALLOWED) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_createtable(L, 0, 4);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DE_ALLOWED);
  }
  lua_pushlstring(L, ref, len);
  lua_pushinteger(L, (lua_Integer)token);
  lua_rawset(L, -3);
  lua_pop(L, 1);
}


/* The token a host pre-authorised for these bytes, or 0. */
static unsigned int de_allowed (lua_State *L, const char *ref, size_t len) {
  unsigned int token = 0;
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DE_ALLOWED) == LUA_TTABLE) {
    lua_pushlstring(L, ref, len);
    lua_rawget(L, -2);
    token = (unsigned int)lua_tointeger(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return token;
}


static void de_tokens (lua_State *L) {
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DE_TOKENS) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_createtable(L, 0, 4);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DE_TOKENS);
  }
}


LUA_API lua_Integer diluvium_endpoint_queue (lua_State *L, unsigned int token) {
  lua_Integer id;
  de_tokens(L);
  lua_rawgeti(L, -1, (lua_Integer)token);
  id = lua_tointeger(L, -1);
  lua_pop(L, 2);
  return id;
}


LUA_API int diluvium_endpoint_setlive (lua_State *L, lua_Integer id, int live) {
  if (!diluvium_queue_is_endpoint(L, id))
    return 0;
  /* Once gone, stays gone. An endpoint names one particular far end; something
     new at the same address is a new endpoint, and letting a handle come back to
     life would make "gone" mean "not right now", which is a different and much
     weaker promise. */
  if (!live)
    return diluvium_queue_set_gone(L, id, 1);
  return 1;
}


/* ======================================================================
** References
** ====================================================================== */

/*
** An endpoint reference is an opaque object holding bytes the guest never reads
** and never builds. 7.3 is explicit that a guest receives one rather than
** constructing one, and giving it a private metatable is what makes that true
** rather than merely documented: there is no constructor exposed anywhere.
*/
static void de_ref_mt (lua_State *L) {
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DE_REF_MT) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_createtable(L, 0, 2);
    lua_pushliteral(L, "diluvium.endpoint.ref");
    lua_setfield(L, -2, "__name");
    /* No __index: there is nothing inside a reference a program should read. */
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");  /* and it cannot be inspected */
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DE_REF_MT);
  }
}


static void de_push_ref (lua_State *L, const char *data, size_t len) {
  lua_createtable(L, 1, 0);
  lua_pushlstring(L, data, len);
  lua_rawseti(L, -2, 1);
  de_ref_mt(L);
  lua_setmetatable(L, -2);
}


/* The bytes inside a reference, or NULL when the value is not one. */
static const char *de_ref_bytes (lua_State *L, int idx, size_t *len) {
  const char *s = NULL;
  if (!lua_istable(L, idx))
    return NULL;
  if (!lua_getmetatable(L, idx))
    return NULL;
  de_ref_mt(L);
  if (lua_rawequal(L, -1, -2)) {
    lua_rawgeti(L, idx, 1);
    s = lua_tolstring(L, -1, len);
    lua_pop(L, 1);
  }
  lua_pop(L, 2);
  return s;
}


/*
** The msgpack resolver for ext 0x02 (4.2). This is the seam the codec was given
** so it need not know the swarm layer exists: with this installed an endpoint
** reference in a message arrives as a reference object, and without it the same
** bytes arrive as an opaque ext value, so an embedder that never uses endpoints
** links nothing extra and sees no error.
*/
static int de_resolve (lua_State *L, const unsigned char *data, size_t len,
                       void *ud) {
  (void)ud;
  de_push_ref(L, (const char *)data, len);
  return 1;
}


static const diluvium_msgpack_resolver de_resolver = {
  de_resolve, NULL, NULL
};


/* ======================================================================
** Lua API
** ====================================================================== */

/*
** endpoint.bind(ref, name) -> id
**
** Resolve now and get an ordinary queue handle, so everything downstream --
** push, len, capacity, wait -- is unchanged. That is the whole trick of 7.3: a
** sender cannot tell an endpoint from a local queue, and does not need to.
*/
static int de_bind (lua_State *L) {
  size_t len;
  const char *ref = de_ref_bytes(L, 1, &len);
  const char *name = luaL_checkstring(L, 2);
  unsigned int token = 0;
  de_handler_box *box;
  lua_Integer id, existing;

  if (ref == NULL) {
    return luaL_error(L, "endpoint.bind: the first argument is not an endpoint "
                         "reference. A program receives one in a message and "
                         "never builds one; see 7.3.");
  }
  /* A pre-authorised reference first: a host that already knows what its
     references mean needs no call out, and for a host reaching the ABI through
     wasm that is the only shape available. */
  token = de_allowed(L, ref, len);
  if (token == 0) {
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DE_HANDLER) != LUA_TUSERDATA) {
      lua_pop(L, 1);
      return luaL_error(L, "endpoint.bind: this host binds no endpoints, so "
                           "there is nothing for a reference to resolve "
                           "against");
    }
    box = (de_handler_box *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!box->fn((const unsigned char *)ref, len, &token, box->ud)) {
      /* A refused bind is an error, unlike a push to a dead endpoint. The
         program asked for one specific destination and did not get it, which is
         not the same as asking a known destination that has since closed. */
      return luaL_error(L, "endpoint.bind: the host refused this reference");
    }
  }

  /* Binding the same token twice gives the same handle: an endpoint is a name
     for a far end, and two handles onto one far end would let a program lose
     ordering against itself for no reason. */
  existing = diluvium_endpoint_queue(L, token);
  if (existing != 0) {
    lua_pushinteger(L, existing);
    return 1;
  }

  id = diluvium_queue_declare(L, name, 0, 0 /* reject */, 0, 1 /* endpoint */);
  if (id == 0)
    return luaL_error(L, "endpoint.bind: '%s' is already declared", name);
  de_tokens(L);
  lua_pushinteger(L, id);
  lua_rawseti(L, -2, (lua_Integer)token);
  lua_pop(L, 1);
  lua_pushinteger(L, id);
  return 1;
}


/*
** endpoint.status(id) -> "live" | "gone"
**
** A cached flag the host maintains, not a call out to it. The host is the only
** thing that knows when a far end died, and a callback here would put whatever
** it does inside the cheapest operation in the system.
*/
static int de_status (lua_State *L) {
  lua_Integer id = luaL_checkinteger(L, 1);
  if (!diluvium_queue_is_endpoint(L, id))
    return luaL_error(L, "endpoint.status: handle %I is not an endpoint", id);
  lua_pushstring(L, diluvium_queue_is_gone(L, id) ? "gone" : "live");
  return 1;
}


/* Whether a handle is an endpoint at all, which a relay needs to know about a
   handle it was handed rather than one it bound itself. */
static int de_is (lua_State *L) {
  lua_Integer id = luaL_checkinteger(L, 1);
  lua_pushboolean(L, diluvium_queue_is_endpoint(L, id));
  return 1;
}


static const luaL_Reg de_lib[] = {
  {"bind", de_bind},
  {"status", de_status},
  {"is_endpoint", de_is},
  {NULL, NULL}
};


LUAMOD_API int luaopen_dendpoint (lua_State *L) {
  luaL_newlib(L, de_lib);
  /* Installing the resolver here rather than in dlibs.c keeps the codec's only
     dependency on this file an injected one, which is what 4.2 asks for. */
  diluvium_msgpack_setresolver(L, &de_resolver);
  return 1;
}
