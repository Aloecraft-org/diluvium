/*
** dqueue.c
** Diluvium queues. See dqueue.h for the model.
**
** Where the state lives, and why it is not in C. A queue's metadata and its
** contents are ordinary Lua tables anchored in the registry, driven from here.
** The alternative -- a C array of structs holding malloc'd names and integer
** refs to payload tables -- is faster per operation and adds a place to leak
** on every one of destroy, error and state close. Since 6.5 already accepts a
** msgpack encode on every push, a few table accesses beside it are noise, and
** the benchmark 14 asks for will say if that stops being true. Nothing here
** allocates outside the collector.
*/

#define dqueue_c

#include "lprefix.h"

#include <stddef.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "dmsgpack.h"
#include "dqueue.h"


/* Registry key; its address is the key, so it cannot collide. */
static const char DQ_STATE = 0;

/* Field names on a queue table. Kept as macros so a typo is a compile error
   in exactly one place rather than a silent nil somewhere else. */
#define DQ_NAME		"name"
#define DQ_CAP		"capacity"
#define DQ_FULL		"on_full"      /* the name, for 'info' */
#define DQ_POLICY	"policy"       /* the same thing as an int */
#define DQ_EXPORTED	"exported"
#define DQ_DIRECTION	"direction"
#define DQ_ENABLED	"enabled"
#define DQ_HEAD		"head"     /* 1-based slot of the oldest message */
#define DQ_COUNT	"count"
#define DQ_ITEMS	"items"    /* [1..capacity], msgpack strings */

#define DQ_DEFAULT_CAP	64


/*
** 'on_full' policies. "block" is accepted by the grammar in 6.3 but not by
** this milestone: it must yield the caller, which needs the wait-set protocol.
** Rejecting it at declare time is deliberate -- silently treating it as
** "reject" would give a program the opposite of the backpressure it asked for,
** and it would look like it worked.
*/
#define DQ_REJECT	0
#define DQ_DROP_OLDEST	1
#define DQ_DROP_NEWEST	2

static const char *const dq_full_names[] = {
  "reject", "drop_oldest", "drop_newest", "block", NULL
};


/* ======================================================================
** State access
** ====================================================================== */

/*
** Push the queue-state table, creating it on first use.
**
** Shape: [1..n] are queue tables by handle, and a 'names' subtable maps name
** to handle. A destroyed handle's slot becomes 'false' rather than nil, so the
** array stays dense and a stale handle is still distinguishable from one that
** was never issued.
*/
static void dq_state (lua_State *L) {
  if (lua_rawgetp(L, LUA_REGISTRYINDEX, &DQ_STATE) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_createtable(L, 4, 1);
    lua_createtable(L, 0, 4);
    lua_setfield(L, -2, "names");
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &DQ_STATE);
  }
}


/*
** Resolve a handle to its queue table, pushed on top. Raises on a handle that
** was never issued or has been destroyed.
**
** 6.2 calls handles runtime identity and not durable identity, and warns that
** storing one across a restore "fails by silently addressing the wrong
** queue". Handles are therefore never reused: a destroyed one stays destroyed
** for the life of the instance, so the stale-handle mistake is an error here
** instead of a wrong answer somewhere else. The cost is that a program which
** churns queues walks the handle space upward, which no real program does.
*/
static void dq_get (lua_State *L, lua_Integer id) {
  dq_state(L);
  if (lua_rawgeti(L, -1, id) != LUA_TTABLE) {
    int destroyed = (lua_type(L, -1) == LUA_TBOOLEAN);
    lua_pop(L, 2);
    if (destroyed)
      luaL_error(L, "queue: handle %I has been destroyed", id);
    else
      luaL_error(L, "queue: %I is not a queue handle", id);
  }
  lua_remove(L, -2);  /* drop the state table, leave the queue */
}


static lua_Integer dq_field_int (lua_State *L, int qidx, const char *k) {
  lua_Integer v;
  lua_getfield(L, qidx, k);
  v = lua_tointeger(L, -1);
  lua_pop(L, 1);
  return v;
}


static void dq_set_int (lua_State *L, int qidx, const char *k, lua_Integer v) {
  lua_pushinteger(L, v);
  lua_setfield(L, qidx, k);
}


static int dq_field_bool (lua_State *L, int qidx, const char *k) {
  int v;
  lua_getfield(L, qidx, k);
  v = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return v;
}


/* ======================================================================
** declare / lookup / destroy
** ====================================================================== */

static int dq_opt_enum (lua_State *L, int optsidx, const char *field,
                        const char *const names[], int def) {
  int r = def;
  if (lua_getfield(L, optsidx, field) != LUA_TNIL) {
    const char *s = lua_tostring(L, -1);
    int i;
    if (s == NULL)
      luaL_error(L, "queue: '%s' must be a string", field);
    for (i = 0; names[i] != NULL; i++) {
      if (strcmp(s, names[i]) == 0) { r = i; break; }
    }
    if (names[i] == NULL)
      luaL_error(L, "queue: '%s' is not a valid '%s'", s, field);
  }
  lua_pop(L, 1);
  return r;
}


/*
** Names are strings, and a number is not coerced into one. 'luaL_checkstring'
** would accept 5 and declare a queue called "5" -- and since handles are
** integers, a name that reads like a handle is a mistake waiting to be made.
*/
static const char *dq_checkname (lua_State *L, int idx) {
  luaL_checktype(L, idx, LUA_TSTRING);
  return lua_tostring(L, idx);
}


static int dq_declare (lua_State *L) {
  const char *name = dq_checkname(L, 1);
  lua_Integer cap = DQ_DEFAULT_CAP;
  int on_full = DQ_REJECT;
  int exported = 0;
  int direction = 0;
  lua_Integer id;
  static const char *const dirs[] = {
    "both", "guest_write", "guest_read", NULL
  };

  if (!lua_isnoneornil(L, 2)) {
    luaL_checktype(L, 2, LUA_TTABLE);
    if (lua_getfield(L, 2, DQ_CAP) != LUA_TNIL) {
      cap = luaL_checkinteger(L, -1);
      if (cap <= 0)
        return luaL_error(L, "queue: capacity must be greater than 0");
    }
    lua_pop(L, 1);
    on_full = dq_opt_enum(L, 2, DQ_FULL, dq_full_names, DQ_REJECT);
    if (on_full == 3)  /* "block" */
      return luaL_error(L, "queue: on_full 'block' needs the wait-set "
                           "protocol, which this build does not have yet; "
                           "'reject' or a drop policy is available now");
    direction = dq_opt_enum(L, 2, DQ_DIRECTION, dirs, 0);
    lua_getfield(L, 2, DQ_EXPORTED);
    exported = lua_toboolean(L, -1);
    lua_pop(L, 1);
  }

  dq_state(L);                       /* state */
  lua_getfield(L, -1, "names");      /* state names */
  if (lua_getfield(L, -1, name) != LUA_TNIL)
    return luaL_error(L, "queue: '%s' is already declared", name);
  lua_pop(L, 1);

  id = (lua_Integer)lua_rawlen(L, -2) + 1;

  lua_createtable(L, 0, 9);          /* state names q */
  lua_pushstring(L, name);
  lua_setfield(L, -2, DQ_NAME);
  dq_set_int(L, lua_gettop(L), DQ_CAP, cap);
  lua_pushstring(L, dq_full_names[on_full]);
  lua_setfield(L, -2, DQ_FULL);
  dq_set_int(L, lua_gettop(L), DQ_POLICY, on_full);
  lua_pushstring(L, dirs[direction]);
  lua_setfield(L, -2, DQ_DIRECTION);
  lua_pushboolean(L, exported);
  lua_setfield(L, -2, DQ_EXPORTED);
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, DQ_ENABLED);
  dq_set_int(L, lua_gettop(L), DQ_HEAD, 1);
  dq_set_int(L, lua_gettop(L), DQ_COUNT, 0);
  lua_createtable(L, (cap > 4096) ? 0 : (int)cap, 0);
  lua_setfield(L, -2, DQ_ITEMS);

  lua_rawseti(L, -3, id);            /* state[id] = q; leaves state names */
  lua_pushinteger(L, id);
  lua_setfield(L, -2, name);         /* names[name] = id */
  lua_pop(L, 2);
  lua_pushinteger(L, id);
  return 1;
}


static int dq_lookup (lua_State *L) {
  const char *name = dq_checkname(L, 1);
  dq_state(L);
  lua_getfield(L, -1, "names");
  lua_getfield(L, -1, name);  /* the handle, or nil when not declared */
  return 1;
}


static int dq_destroy (lua_State *L) {
  lua_Integer id = luaL_checkinteger(L, 1);
  dq_get(L, id);                     /* raises on a bad handle */
  lua_getfield(L, -1, DQ_NAME);
  dq_state(L);
  lua_getfield(L, -1, "names");      /* q name state names */
  lua_pushvalue(L, -3);              /* the name */
  lua_pushnil(L);
  lua_settable(L, -3);               /* names[name] = nil */
  lua_pop(L, 2);                     /* q name */
  lua_pop(L, 1);                     /* q */
  dq_state(L);
  lua_pushboolean(L, 0);             /* 'false' marks a destroyed slot */
  lua_rawseti(L, -2, id);
  lua_pop(L, 2);
  return 0;
}


/* ======================================================================
** push / pop
** ====================================================================== */

/*
** Slot of the i-th message counting from the oldest, in a ring of 'cap'.
** Modular rather than monotonically growing indices so the items table stays
** a fixed-size array for the life of the queue.
*/
static lua_Integer dq_slot (lua_Integer head, lua_Integer i, lua_Integer cap) {
  return ((head - 1 + i) % cap) + 1;
}


static int dq_push (lua_State *L) {
  lua_Integer id = luaL_checkinteger(L, 1);
  lua_Integer cap, count, head, policy;
  int q, items;
  luaL_checkany(L, 2);

  dq_get(L, id);
  q = lua_gettop(L);

  /* A disabled queue rejects, and that is a normal outcome and not an error
     (6.4): a program going down should reject cleanly rather than accept
     messages it will drop. */
  if (!dq_field_bool(L, q, DQ_ENABLED)) {
    lua_pushboolean(L, 0);
    lua_pushliteral(L, "disabled");
    return 2;
  }

  cap = dq_field_int(L, q, DQ_CAP);
  count = dq_field_int(L, q, DQ_COUNT);
  head = dq_field_int(L, q, DQ_HEAD);
  /* The integer, not the name: 'lua_tostring' on a field we then pop would
     leave a pointer alive only because the queue table happens to hold the
     string, which is true today and not a thing to depend on. */
  policy = dq_field_int(L, q, DQ_POLICY);

  /* Encode before touching the queue. 6.5: every value crosses as msgpack
     even for a queue that never leaves the instance, so a pushed table cannot
     be mutated by the sender afterwards, the contents are already bytes when a
     snapshot is taken, and there is one serializability rule to learn rather
     than two. An unencodable value raises here, before anything is committed. */
  diluvium_msgpack_encode(L, 2);     /* q ... str */

  lua_getfield(L, q, DQ_ITEMS);
  items = lua_gettop(L);             /* str items */

  if (count == cap) {
    if (policy == DQ_DROP_NEWEST) {
      /* The newest message is the one being pushed, so dropping it and
         rejecting it are the same action; 6.4 has no separate status. */
      lua_pop(L, 2);
      lua_pushboolean(L, 0);
      lua_pushliteral(L, "full");
      return 2;
    }
    else if (policy == DQ_DROP_OLDEST) {
      /* Overwrite the oldest and advance: the count does not change. */
      lua_pushvalue(L, items - 1);   /* the encoded string */
      lua_rawseti(L, items, head);
      dq_set_int(L, q, DQ_HEAD, (head % cap) + 1);
      lua_pop(L, 2);
      lua_pushboolean(L, 1);
      lua_pushliteral(L, "dropped_oldest");
      return 2;
    }
    else {  /* "reject" */
      lua_pop(L, 2);
      lua_pushboolean(L, 0);
      lua_pushliteral(L, "full");
      return 2;
    }
  }

  lua_pushvalue(L, items - 1);       /* the encoded string */
  lua_rawseti(L, items, dq_slot(head, count, cap));
  dq_set_int(L, q, DQ_COUNT, count + 1);
  lua_pop(L, 2);
  lua_pushboolean(L, 1);
  lua_pushliteral(L, "ok");
  return 2;
}


/*
** Never yields, by design (6.3). An empty queue is 'nil, "empty"' and a
** program that wants to wait uses 'queue.wait', which is a later milestone.
** Conflating the two would make every read a potential suspension point.
*/
static int dq_pop (lua_State *L) {
  lua_Integer id = luaL_checkinteger(L, 1);
  lua_Integer cap, count, head;
  size_t len;
  const char *s;
  int q;

  dq_get(L, id);
  q = lua_gettop(L);
  count = dq_field_int(L, q, DQ_COUNT);
  if (count == 0) {
    lua_pushnil(L);
    lua_pushliteral(L, "empty");
    return 2;
  }
  cap = dq_field_int(L, q, DQ_CAP);
  head = dq_field_int(L, q, DQ_HEAD);

  lua_getfield(L, q, DQ_ITEMS);
  lua_rawgeti(L, -1, head);
  s = lua_tolstring(L, -1, &len);
  if (s == NULL)
    return luaL_error(L, "queue: slot %I of handle %I is not a message",
                      head, id);
  /* Decode before mutating, so a corrupt payload leaves the queue as it was
     rather than losing the message to a raise. */
  diluvium_msgpack_decode(L, s, len);   /* q items str value */
  lua_pushnil(L);
  lua_rawseti(L, -4, head);             /* release the string for collection */
  dq_set_int(L, q, DQ_HEAD, (head % cap) + 1);
  dq_set_int(L, q, DQ_COUNT, count - 1);
  lua_pushliteral(L, "ok");
  return 2;  /* value, "ok" -- the value is below the status on the stack */
}


/* ======================================================================
** enable / disable / introspection
** ====================================================================== */

static int dq_set_enabled (lua_State *L, int on) {
  lua_Integer id = luaL_checkinteger(L, 1);
  dq_get(L, id);
  lua_pushboolean(L, on);
  lua_setfield(L, -2, DQ_ENABLED);
  return 0;
}

static int dq_enable (lua_State *L) { return dq_set_enabled(L, 1); }
static int dq_disable (lua_State *L) { return dq_set_enabled(L, 0); }


static int dq_len (lua_State *L) {
  lua_Integer id = luaL_checkinteger(L, 1);
  dq_get(L, id);
  lua_pushinteger(L, dq_field_int(L, lua_gettop(L), DQ_COUNT));
  return 1;
}


static int dq_capacity (lua_State *L) {
  lua_Integer id = luaL_checkinteger(L, 1);
  dq_get(L, id);
  lua_pushinteger(L, dq_field_int(L, lua_gettop(L), DQ_CAP));
  return 1;
}


static int dq_state_of (lua_State *L) {
  lua_Integer id = luaL_checkinteger(L, 1);
  dq_get(L, id);
  lua_pushstring(L, dq_field_bool(L, lua_gettop(L), DQ_ENABLED)
                    ? "enabled" : "disabled");
  return 1;
}


/*
** Not in 6.3, and added because the fields it reports are otherwise
** write-only: 'exported' and 'direction' are recorded at declare time and
** nothing could read them back, which makes them impossible to test and
** invisible to a program deciding how to treat a queue it did not declare.
*/
static int dq_info (lua_State *L) {
  lua_Integer id = luaL_checkinteger(L, 1);
  int q;
  dq_get(L, id);
  q = lua_gettop(L);
  lua_createtable(L, 0, 6);
  lua_getfield(L, q, DQ_NAME);       lua_setfield(L, -2, "name");
  lua_getfield(L, q, DQ_CAP);        lua_setfield(L, -2, "capacity");
  lua_getfield(L, q, DQ_FULL);       lua_setfield(L, -2, "on_full");
  lua_getfield(L, q, DQ_EXPORTED);   lua_setfield(L, -2, "exported");
  lua_getfield(L, q, DQ_DIRECTION);  lua_setfield(L, -2, "direction");
  lua_pushinteger(L, dq_field_int(L, q, DQ_COUNT));
  lua_setfield(L, -2, "len");
  return 1;
}


/* ======================================================================
** Library
** ====================================================================== */

static const luaL_Reg dq_lib[] = {
  {"declare", dq_declare},
  {"lookup", dq_lookup},
  {"destroy", dq_destroy},
  {"push", dq_push},
  {"pop", dq_pop},
  {"enable", dq_enable},
  {"disable", dq_disable},
  {"len", dq_len},
  {"capacity", dq_capacity},
  {"state", dq_state_of},
  {"info", dq_info},
  {NULL, NULL}
};


LUAMOD_API int luaopen_dqueue (lua_State *L) {
  luaL_newlib(L, dq_lib);
  /* 6.6: 'inbox' and 'outbox' are reserved per-instance defaults, declared
     here with exported = true so the zero-configuration case needs no code. A
     program may re-declare them with different options before first use, which
     is why 'declare' refuses a duplicate rather than silently reconfiguring:
     re-declaring is 'destroy' then 'declare', and saying so is better than a
     call that sometimes creates and sometimes mutates. */
  {
    static const char *const defaults[] = { "inbox", "outbox", NULL };
    int i;
    for (i = 0; defaults[i] != NULL; i++) {
      lua_getfield(L, -1, "declare");
      lua_pushstring(L, defaults[i]);
      lua_createtable(L, 0, 1);
      lua_pushboolean(L, 1);
      lua_setfield(L, -2, DQ_EXPORTED);
      lua_call(L, 2, 1);
      lua_pop(L, 1);  /* the handle: a program looks it up by name */
    }
  }
  return 1;
}
