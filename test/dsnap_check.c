/*
** dsnap_check.c
** The snapshot value graph: identity, cycles, metatables, refusals.
**
** In C rather than Lua for the same reason as dtask_check.c -- there is no
** guest binding yet and inventing one for a test would add a surface that has
** to be kept.
**
** What makes these worth writing carefully is that a graph serializer fails
** *quietly*. A round trip that produces equal-looking values while silently
** unsharing two references to one table passes any test written as
** "encode, decode, compare contents". So every case here asserts an identity
** relation -- that two paths reach the *same* table, that a cycle closes back
** on itself, that a metatable is the one that was there -- rather than
** comparing values. `same_table` exists because `==` on two tables is the only
** thing that can say it.
**
** Verified by mutation, each turning a named case red:
**
**   Interning a table *after* writing its contents instead of before turns
**   'sharing_is_preserved' red on every identity assertion, and the cycle cases
**   red with "table nesting deeper than 150" -- the table never meets its own
**   position on the way down, so it recurses until the cap.
**
**   Registering a decoded table after filling it instead of before turns
**   'sharing_is_preserved' and 'a_shared_metatable_stays_shared' red with
**   "backreference 3 is out of range (2 objects so far)", from the other side.
**
**   Caching the metatable worklist length instead of re-reading it turns
**   'a_metatable_with_a_metatable' red, and only that one.
**
**   Dropping the forward-reference check in ext 0x04 turns
**   'a_forward_backreference_is_refused' red.
**
**   Letting light userdata through turns 'light_userdata_is_refused' red.
**
** The first of those originally *hung* rather than failing: without the identity
** map the metatable worklist re-queues forever. A hang is a bad answer for a
** test to give, so the encoder now checks the invariant that made it possible
** ("a queued metatable owner has no position") and the same mutation reports
** eight named failures instead of nothing at all.
*/

#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "dlibs.h"
#include "dmsgpack.h"
#include "dqueue.h"
#include "dsnap.h"


static int failures = 0;
static int checks = 0;

static void ok (int cond, const char *what) {
  checks++;
  if (cond)
    printf("[PASS] %s\n", what);
  else {
    printf("[FAIL] %s\n", what);
    failures++;
  }
}


/* Run 'src' and leave its single return value on the stack. */
static int build (lua_State *L, const char *src) {
  if (luaL_dostring(L, src) != LUA_OK) {
    printf("[FAIL] chunk did not run: %s\n", lua_tostring(L, -1));
    failures++; checks++;
    lua_pop(L, 1);
    return 0;
  }
  return 1;
}


/*
** Encode the value at the top, decode it, and leave the result on top. The
** original stays below it, so a test can compare the two. Returns the index of
** the copy, or 0 having reported a failure.
**
** Protected, and that is not incidental. The failure mode of a graph encoder
** with broken identity tracking is unbounded recursion, which aborts the
** process -- taking the rest of the suite and, because stdout is buffered, the
** output with it. Under pcall the same bug is one named red line.
*/
static int protected_roundtrip (lua_State *L) {
  size_t len;
  const char *bytes;
  diluvium_msgpack_encode_graph(L, 1, NULL);
  bytes = lua_tolstring(L, -1, &len);
  diluvium_msgpack_decode_graph(L, bytes, len, NULL);
  return 1;
}


static int roundtrip_named (lua_State *L, const char *what) {
  int base = lua_gettop(L);            /* the original is at 'base' */
  lua_pushcfunction(L, protected_roundtrip);
  lua_pushvalue(L, base);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    printf("[FAIL] %s: the round trip raised: %s\n", what,
           lua_tostring(L, -1));
    failures++; checks++;
    lua_settop(L, base);
    return 0;
  }
  return base + 1;
}


/* Are the values at these two indices the same table? */
static int same_table (lua_State *L, int a, int b) {
  return lua_istable(L, a) && lua_istable(L, b) &&
         lua_topointer(L, a) == lua_topointer(L, b);
}


/* Follow a chain of string keys from the table at 'idx', pushing the result. */
static void at (lua_State *L, int idx, const char *path) {
  char key[64];
  const char *p = path;
  lua_pushvalue(L, idx);
  while (*p != '\0') {
    size_t n = 0;
    while (*p != '\0' && *p != '.' && n < sizeof(key) - 1)
      key[n++] = *p++;
    key[n] = '\0';
    if (*p == '.') p++;
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      lua_pushnil(L);
      return;
    }
    lua_getfield(L, -1, key);
    lua_remove(L, -2);
  }
}


/* ---------------------------------------------------------------- plain */

static void plain_values_survive (lua_State *L) {
  int base = lua_gettop(L);
  if (!build(L, "return {n = 42, f = 1.5, s = 'hi', t = true, no = false,"
                " nested = {1, 2, 3}, [1] = 'one'}"))
    return;
  if (roundtrip_named(L, __func__) == 0) return;
  at(L, -1, "n");   ok(lua_tointeger(L, -1) == 42, "an integer survives");
  lua_pop(L, 1);
  at(L, -1, "f");   ok(lua_tonumber(L, -1) == 1.5, "a float survives");
  lua_pop(L, 1);
  at(L, -1, "s");   ok(lua_tostring(L, -1) != NULL &&
                       strcmp(lua_tostring(L, -1), "hi") == 0,
                       "a string survives");
  lua_pop(L, 1);
  at(L, -1, "t");   ok(lua_toboolean(L, -1), "true survives");
  lua_pop(L, 1);
  at(L, -1, "no");  ok(lua_isboolean(L, -1) && !lua_toboolean(L, -1),
                       "false survives as false, not nil");
  lua_pop(L, 1);
  at(L, -1, "nested");
  ok(lua_istable(L, -1) && lua_rawlen(L, -1) == 3, "a nested array survives");
  lua_pop(L, 1);
  lua_settop(L, base);
}


/* ------------------------------------------------------------- identity */

static void sharing_is_preserved (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /* One table reached by two paths. This is the case a naive encoder gets
     wrong invisibly: it writes the contents twice and the restored graph has
     two tables that compare equal field by field. */
  if (!build(L, "local shared = {tag = 'the one'} "
                "return {a = shared, b = shared, list = {shared, shared}}"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "a");
  at(L, copy, "b");
  ok(same_table(L, -2, -1), "two fields referring to one table still do");
  lua_pop(L, 2);
  at(L, copy, "a");
  at(L, copy, "list");
  lua_rawgeti(L, -1, 1);
  ok(same_table(L, -3, -1), "and so does a reference from inside an array");
  lua_rawgeti(L, -2, 2);
  ok(same_table(L, -4, -1), "twice over");
  lua_pop(L, 4);
  /* The original must be untouched by having been encoded. */
  at(L, base + 1, "a");
  at(L, base + 1, "b");
  ok(same_table(L, -2, -1), "the original graph is unchanged");
  lua_pop(L, 2);
  lua_settop(L, base);
}


static void distinct_tables_stay_distinct (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /* The converse, and it matters: an encoder that interned by *contents*
     rather than identity would merge these, which is just as wrong. */
  if (!build(L, "return {a = {tag = 'same'}, b = {tag = 'same'}}"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "a");
  at(L, copy, "b");
  ok(!same_table(L, -2, -1),
     "two equal-looking but distinct tables are not merged");
  ok(lua_istable(L, -2) && lua_istable(L, -1), "and both are still tables");
  lua_pop(L, 2);
  lua_settop(L, base);
}


static void a_cycle_closes (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  if (!build(L, "local t = {name = 'root'} t.self = t "
                "t.child = {parent = t} return t"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "self");
  ok(same_table(L, copy, -1), "a table containing itself still does");
  lua_pop(L, 1);
  at(L, copy, "child.parent");
  ok(same_table(L, copy, -1), "and a two-step cycle closes on the same table");
  lua_pop(L, 1);
  at(L, copy, "child");
  at(L, copy, "child.parent.child");
  ok(same_table(L, -2, -1), "going round the cycle twice arrives back");
  lua_pop(L, 2);
  lua_settop(L, base);
}


static void the_root_can_be_referenced_from_deep_inside (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /* Position 1 is the root, so this is the backreference most likely to be
     off by one. */
  if (!build(L, "local r = {} r.a = {b = {c = {back = r}}} return r"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "a.b.c.back");
  ok(same_table(L, copy, -1), "a reference to the root from four levels down");
  lua_pop(L, 1);
  lua_settop(L, base);
}


static void deep_nesting_is_allowed (lua_State *L) {
  int base = lua_gettop(L);
  int copy, i, depth = 0;
  /* 60 levels: past the plain codec's cap of 16, which is the point. A linked
     list is the ordinary shape this comes up in. */
  if (!build(L, "local head = {} local t = head "
                "for i = 1, 60 do t.next = {i = i} t = t.next end return head"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  lua_pushvalue(L, copy);
  for (i = 0; i < 100; i++) {
    lua_getfield(L, -1, "next");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); break; }
    lua_remove(L, -2);
    depth++;
  }
  ok(depth == 60, "a 60-deep chain survives, well past the plain cap of 16");
  if (depth != 60)
    printf("      (reached %d)\n", depth);
  lua_settop(L, base);
}


/* ---------------------------------------------------------- metatables */

static void a_metatable_survives (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  if (!build(L, "local mt = {kind = 'point'} "
                "return setmetatable({x = 1, y = 2}, mt)"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  ok(lua_getmetatable(L, copy), "the restored table has a metatable");
  at(L, -1, "kind");
  ok(lua_tostring(L, -1) != NULL &&
     strcmp(lua_tostring(L, -1), "point") == 0,
     "and it is the metatable that was there");
  lua_pop(L, 2);
  lua_settop(L, base);
}


static void a_shared_metatable_stays_shared (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /* The usual arrangement: one class table as the metatable of many instances.
     If each instance got its own copy, method identity and any state on the
     class would silently fork. */
  if (!build(L, "local mt = {} "
                "return {setmetatable({}, mt), setmetatable({}, mt),"
                " setmetatable({}, mt)}"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  lua_rawgeti(L, copy, 1);
  lua_rawgeti(L, copy, 2);
  lua_rawgeti(L, copy, 3);
  ok(lua_getmetatable(L, -3) && lua_getmetatable(L, -3) &&
     lua_getmetatable(L, -3), "all three instances have metatables");
  ok(same_table(L, -3, -2) && same_table(L, -2, -1),
     "and it is one shared metatable, not three copies");
  lua_settop(L, base);
}


static void a_metatable_in_the_graph_is_not_duplicated (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /* The metatable is reachable *both* as a metatable and as ordinary data.
     Those are two different traversal routes to one table, and they must land
     on the same object -- otherwise a program that keeps a registry of its
     classes finds the registry entry is no longer the class its instances
     use. */
  if (!build(L, "local mt = {tag = 'cls'} "
                "return {class = mt, obj = setmetatable({}, mt)}"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "class");
  at(L, copy, "obj");
  ok(lua_getmetatable(L, -1), "the instance has its metatable");
  ok(same_table(L, -3, -1),
     "and it is the same table the graph also holds as data");
  lua_settop(L, base);
}


static void a_metatable_with_a_metatable (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /* Inheritance, roughly: the class has a metaclass. This is the case that
     fails if the metatable worklist is drained with a cached length, because
     the second metatable is discovered while the first is being written. */
  if (!build(L, "local meta = {level = 'meta'} "
                "local mt = setmetatable({level = 'mt'}, meta) "
                "return setmetatable({level = 'obj'}, mt)"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  ok(lua_getmetatable(L, copy), "the object has a metatable");
  at(L, -1, "level");
  ok(lua_tostring(L, -1) && strcmp(lua_tostring(L, -1), "mt") == 0,
     "which is the right one");
  lua_pop(L, 1);
  ok(lua_getmetatable(L, -1), "and the metatable has a metatable");
  at(L, -1, "level");
  ok(lua_tostring(L, -1) && strcmp(lua_tostring(L, -1), "meta") == 0,
     "which is also the right one");
  lua_settop(L, base);
}


static void a_self_metatable_survives (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /* 'setmetatable(t, t)' -- the shape 14 says 'defer' desugars to, so it will
     be on the critical path later. Both a cycle and a metatable at once. */
  if (!build(L, "local t = {tag = 'both'} return setmetatable(t, t)"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  ok(lua_getmetatable(L, copy), "the self-metatable table has a metatable");
  ok(same_table(L, copy, -1), "and it is itself");
  lua_settop(L, base);
}


static void mode_comes_along_with_the_metatable (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  if (!build(L, "return setmetatable({}, {__mode = 'k'})"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  ok(lua_getmetatable(L, copy), "a weak table's metatable survives");
  at(L, -1, "__mode");
  ok(lua_tostring(L, -1) && strcmp(lua_tostring(L, -1), "k") == 0,
     "and '__mode' with it, since it is just a field");
  lua_settop(L, base);
}


/* ------------------------------------------------------------- refusals */

static int protected_encode (lua_State *L) {
  diluvium_msgpack_encode_graph(L, 1, NULL);
  return 1;
}


static void expect_refusal (lua_State *L, const char *src, const char *needle,
                            const char *what) {
  int base = lua_gettop(L);
  if (!build(L, src))
    return;
  lua_pushcfunction(L, protected_encode);
  lua_insert(L, -2);
  if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
    printf("[FAIL] %s: encoding succeeded when it should have refused\n", what);
    failures++; checks++;
    lua_settop(L, base);
    return;
  }
  {
    const char *msg = lua_tostring(L, -1);
    checks++;
    if (msg == NULL || strstr(msg, needle) == NULL) {
      printf("[FAIL] %s\n  message: %s\n  wanted to contain: %s\n",
             what, (msg == NULL) ? "(none)" : msg, needle);
      failures++;
    }
    else
      printf("[PASS] %s\n", what);
  }
  lua_settop(L, base);
}


static void light_userdata_is_refused (lua_State *L) {
  /* 10.7 item 3, and the one refusal here that is permanent rather than
     "not yet": a bare pointer has no type and no hook. */
  lua_pushlightuserdata(L, (void *)&failures);
  lua_setglobal(L, "a_light_pointer");
  expect_refusal(L, "return {deep = {inside = a_light_pointer}}",
                 "light userdata",
                 "light userdata is refused, wherever it sits");
  expect_refusal(L, "return {deep = {inside = a_light_pointer}}",
                 "deep.inside",
                 "and the message names the key path (5.6)");
}


static void a_shape_wrapper_is_refused (lua_State *L) {
  expect_refusal(L, "return {payload = msgpack.as_array({1, 2, 3})}",
                 "shape wrapper",
                 "a msgpack shape wrapper is refused in a snapshot");
}


static void a_function_is_refused_without_hooks (lua_State *L) {
  /* Ext 0x06 is step two. Until then this must be a clean error naming the
     type and the path, not a crash and not a silent nil. */
  expect_refusal(L, "return {handler = function() end}",
                 "function",
                 "a function is refused while there are no hooks");
  expect_refusal(L, "return {handler = function() end}",
                 "handler",
                 "and that message names the path too");
}


/* -------------------------------------------------- malformed streams */

static int protected_decode (lua_State *L) {
  size_t len;
  const char *s = lua_tolstring(L, 1, &len);
  diluvium_msgpack_decode_graph(L, s, len, NULL);
  return 1;
}


static void expect_decode_refusal (lua_State *L, const char *bytes,
                                   size_t len, const char *needle,
                                   const char *what) {
  int base = lua_gettop(L);
  lua_pushcfunction(L, protected_decode);
  lua_pushlstring(L, bytes, len);
  if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
    printf("[FAIL] %s: decoding succeeded when it should have refused\n", what);
    failures++; checks++;
    lua_settop(L, base);
    return;
  }
  {
    const char *msg = lua_tostring(L, -1);
    checks++;
    if (msg == NULL || strstr(msg, needle) == NULL) {
      printf("[FAIL] %s\n  message: %s\n  wanted to contain: %s\n",
             what, (msg == NULL) ? "(none)" : msg, needle);
      failures++;
    }
    else
      printf("[PASS] %s\n", what);
  }
  lua_settop(L, base);
}


static void a_forward_backreference_is_refused (lua_State *L) {
  /* A map of one entry whose value is a backreference to position 9, which
     does not exist. 10.10 wants this refused rather than silently producing a
     nil where a table belongs. */
  static const char bad[] = {
    (char)0x81,                              /* map, 1 pair */
    (char)0xa1, 'k',                         /* key "k" */
    (char)0xd6, 0x04, 0, 0, 0, 9,            /* ext 4, code 0x04, pos 9 */
    (char)0xc0                               /* the metatable terminator */
  };
  expect_decode_refusal(L, bad, sizeof(bad), "out of range",
                        "a forward backreference is refused");
}


static void a_short_backreference_is_refused (lua_State *L) {
  static const char bad[] = {
    (char)0x81, (char)0xa1, 'k',
    (char)0xd5, 0x04, 0, 1,                  /* ext 2 bytes, not 4 */
    (char)0xc0
  };
  expect_decode_refusal(L, bad, sizeof(bad), "4 bytes",
                        "a backreference of the wrong width is refused");
}


static void a_missing_terminator_is_refused (lua_State *L) {
  static const char bad[] = { (char)0x80 };  /* an empty map and nothing else */
  expect_decode_refusal(L, bad, sizeof(bad), "terminator",
                        "a graph with no metatable terminator is refused");
}


static void trailing_bytes_are_refused (lua_State *L) {
  static const char bad[] = { (char)0x80, (char)0xc0, (char)0x01, (char)0x02 };
  expect_decode_refusal(L, bad, sizeof(bad), "left over",
                        "bytes after the graph are refused");
}


static void a_bad_owner_position_is_refused (lua_State *L) {
  static const char bad[] = {
    (char)0x80,                    /* root: empty map (position 1) */
    (char)0xa1, 'x',               /* owner given as a string, not a position */
    (char)0xc0
  };
  expect_decode_refusal(L, bad, sizeof(bad), "integer position",
                        "a non-integer metatable owner is refused");
}


static void an_out_of_range_owner_is_refused (lua_State *L) {
  static const char bad[] = {
    (char)0x80,                    /* root: empty map (position 1) */
    0x09, (char)0x80,              /* owner 9, which does not exist */
    (char)0xc0
  };
  expect_decode_refusal(L, bad, sizeof(bad), "out of range",
                        "a metatable owner position past the end is refused");
}


static void a_truncated_metatable_entry_is_refused (lua_State *L) {
  static const char bad[] = {
    (char)0x80,                    /* root: empty map (position 1) */
    0x01                           /* owner 1, and then the stream ends */
  };
  expect_decode_refusal(L, bad, sizeof(bad), "no metatable after it",
                        "an owner with no metatable after it is refused");
}


static void a_bad_metatable_entry_is_refused (lua_State *L) {
  static const char bad[] = {
    (char)0x80,                    /* root: empty map (position 1) */
    0x01, (char)0x2a,              /* owner 1, then 42 -- not a metatable */
    (char)0xc0
  };
  expect_decode_refusal(L, bad, sizeof(bad), "metatable must be a table",
                        "a non-table metatable is refused");
}


static void a_snapshot_ext_is_refused_by_plain_decode (lua_State *L) {
  /* 5.5: codes 0x03 through 0x07 are snapshot-only, and 'msgpack.decode' must
     reject them. The backreference is the one that now has a second meaning,
     so it is the one worth checking did not leak into the plain path. */
  int base = lua_gettop(L);
  if (luaL_dostring(L,
        "local ok, err = pcall(msgpack.decode, "
        "  string.char(0xd6, 0x04, 0, 0, 0, 1)) "
        "return (not ok) and err or 'decoded, which is wrong'") != LUA_OK) {
    ok(0, "the plain-decode check runs");
    lua_settop(L, base);
    return;
  }
  {
    const char *msg = lua_tostring(L, -1);
    ok(msg != NULL && strstr(msg, "snapshot stream") != NULL,
       "'msgpack.decode' still refuses ext 0x04 as snapshot-only");
    if (msg != NULL && strstr(msg, "snapshot stream") == NULL)
      printf("      (%s)\n", msg);
  }
  lua_settop(L, base);
}


static void plain_encode_still_refuses_a_cycle (lua_State *L) {
  /* The plain codec's behaviour must not have changed. A queue message with a
     cycle has no agreed meaning for whoever receives it, so refusing is right
     there even though the snapshot encoder handles it. */
  int base = lua_gettop(L);
  if (luaL_dostring(L,
        "local t = {} t.self = t "
        "local ok, err = pcall(msgpack.encode, t) "
        "return (not ok) and err or 'encoded, which is wrong'") != LUA_OK) {
    ok(0, "the plain-cycle check runs");
    lua_settop(L, base);
    return;
  }
  {
    const char *msg = lua_tostring(L, -1);
    ok(msg != NULL && strstr(msg, "cycle") != NULL,
       "'msgpack.encode' still refuses a cycle");
    if (msg != NULL && strstr(msg, "cycle") == NULL)
      printf("      (%s)\n", msg);
  }
  lua_settop(L, base);
}




/* ======================================================================
** The header (10.6) and the runtime fingerprint
** ====================================================================== */

static void the_fingerprint_is_stable_and_looks_like_a_hash (lua_State *L) {
  char a[DILUVIUM_SHA256_HEX], b[DILUVIUM_SHA256_HEX];
  int i, hexish = 1;
  diluvium_snap_fingerprint(L, a);
  diluvium_snap_fingerprint(L, b);   /* second call comes from the cache */
  ok(strlen(a) == DILUVIUM_SHA256_SIZE * 2, "the fingerprint is a full digest");
  ok(strcmp(a, b) == 0, "and it is the same on a second call");
  for (i = 0; a[i] != '\0'; i++)
    if (!((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'f')))
      hexish = 0;
  ok(hexish, "and it is lowercase hex");
  printf("      (runtime fingerprint %.16s...)\n", a);
}


static void a_fresh_header_is_accepted (lua_State *L) {
  char buf[4096];
  size_t n, used = 0;
  int rc;
  n = diluvium_snap_header(L, NULL, buf, sizeof(buf));
  ok(n > 0, "a header is written");
  if (n == 0) return;
  rc = diluvium_snap_checkheader(L, NULL, buf, n, &used);
  if (rc != DILUVIUM_SNAP_ACCEPT)
    printf("      (%s)\n", diluvium_snap_why(rc));
  ok(rc == DILUVIUM_SNAP_ACCEPT, "and this runtime accepts its own header");
  ok(used == n, "and reports the whole header as consumed");
}


static void a_payload_after_the_header_is_found (lua_State *L) {
  char buf[4096];
  size_t n, used = 0;
  /* The reason 'used' exists: the payload starts immediately after, and a
     restore has to know where. Trailing bytes must not make the header
     unreadable. */
  n = diluvium_snap_header(L, NULL, buf, sizeof(buf));
  if (n == 0 || n + 3 > sizeof(buf)) { ok(0, "header fits with a payload"); return; }
  buf[n] = (char)0xa2; buf[n + 1] = 'h'; buf[n + 2] = 'i';
  ok(diluvium_snap_checkheader(L, NULL, buf, n + 3, &used)
     == DILUVIUM_SNAP_ACCEPT, "a header followed by a payload is accepted");
  ok(used == n, "and the payload's start is reported correctly");
}


static void a_short_buffer_is_reported_not_overrun (lua_State *L) {
  char small[8];
  ok(diluvium_snap_header(L, NULL, small, sizeof(small)) == 0,
     "a buffer too small for the header returns 0 rather than overrunning");
}


static void garbage_is_refused_without_raising (lua_State *L) {
  /* 10.10: the first thing to touch untrusted bytes must not be able to take
     the host down. These are not msgpack, so the codec raises internally --
     which must not escape. */
  static const char *cases[] = {
    "", "not a snapshot at all", "\xc1", "\x91", "\xdd\xff\xff\xff\xff"
  };
  size_t i;
  int all_refused = 1;
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    size_t used = 123;
    int rc = diluvium_snap_checkheader(L, NULL, cases[i], strlen(cases[i]),
                                       &used);
    if (rc == DILUVIUM_SNAP_ACCEPT || used != 0)
      all_refused = 0;
  }
  ok(all_refused, "garbage is refused cleanly, with no bytes reported used");
}


/* Decode a header, change one field, re-encode. Leaves the bytes on top. */
static int tamper (lua_State *L, const char *key, const char *value) {
  char buf[4096];
  size_t n = diluvium_snap_header(L, NULL, buf, sizeof(buf));
  if (n == 0) return 0;
  lua_pushlstring(L, buf, n);
  lua_getglobal(L, "msgpack");
  lua_getfield(L, -1, "decode");
  lua_pushvalue(L, -3);
  lua_call(L, 1, 1);
  if (!lua_istable(L, -1)) return 0;
  if (value == NULL)
    lua_pushnil(L);
  else
    lua_pushstring(L, value);
  lua_setfield(L, -2, key);
  lua_getfield(L, -2, "encode");
  lua_pushvalue(L, -2);
  lua_call(L, 1, 1);
  return 1;
}


static void expect_header_refusal (lua_State *L, const char *key,
                                   const char *value, int want,
                                   const char *what) {
  int base = lua_gettop(L);
  size_t len, used = 0;
  const char *bytes;
  int rc;
  if (!tamper(L, key, value)) {
    ok(0, what);
    lua_settop(L, base);
    return;
  }
  bytes = lua_tolstring(L, -1, &len);
  rc = diluvium_snap_checkheader(L, NULL, bytes, len, &used);
  checks++;
  if (rc != want) {
    printf("[FAIL] %s\n  got %d (%s)\n  wanted %d (%s)\n", what,
           rc, diluvium_snap_why(rc), want, diluvium_snap_why(want));
    failures++;
  }
  else
    printf("[PASS] %s\n", what);
  lua_settop(L, base);
}


static void a_foreign_runtime_is_refused (lua_State *L) {
  /* The measured case from 10.6: the same source built with different codegen
     knobs produces different bytecode, so its Proto hashes can never match.
     Refusing at the header is the difference between one clear message and a
     missing-Proto failure deep inside restore. */
  expect_header_refusal(L, "runtime",
    "0000000000000000000000000000000000000000000000000000000000000000",
    DILUVIUM_SNAP_RUNTIME_MISMATCH, "a foreign runtime fingerprint is refused");
}


static void a_wrong_format_is_refused (lua_State *L) {
  int base = lua_gettop(L);
  size_t len, used = 0;
  const char *bytes;
  /* 'format' is a number, so it needs its own tampering. */
  char buf[4096];
  size_t n = diluvium_snap_header(L, NULL, buf, sizeof(buf));
  if (n == 0) { ok(0, "a header is written"); return; }
  lua_pushlstring(L, buf, n);
  lua_getglobal(L, "msgpack");
  lua_getfield(L, -1, "decode");
  lua_pushvalue(L, -3);
  lua_call(L, 1, 1);
  lua_pushinteger(L, DILUVIUM_SNAP_FORMAT + 99);
  lua_setfield(L, -2, "format");
  lua_getfield(L, -2, "encode");
  lua_pushvalue(L, -2);
  lua_call(L, 1, 1);
  bytes = lua_tolstring(L, -1, &len);
  ok(diluvium_snap_checkheader(L, NULL, bytes, len, &used)
     == DILUVIUM_SNAP_FORMAT_MISMATCH, "a future format version is refused");
  lua_settop(L, base);
}


static void a_missing_field_is_refused (lua_State *L) {
  expect_header_refusal(L, "runtime", NULL, DILUVIUM_SNAP_RUNTIME_MISMATCH,
                        "a header missing its runtime field is refused");
  expect_header_refusal(L, "permanents", NULL,
                        DILUVIUM_SNAP_PERMANENTS_MISMATCH,
                        "a header missing its permanents field is refused");
}


static void the_runtime_check_comes_first (lua_State *L) {
  /* Order matters for the diagnostic: with a foreign runtime, the permanents
     and capability strings were produced by a different build and comparing
     them says nothing. Reporting the most fundamental difference is what makes
     the message actionable. */
  int base = lua_gettop(L);
  size_t len, used = 0;
  const char *bytes;
  char buf[4096];
  size_t n = diluvium_snap_header(L, NULL, buf, sizeof(buf));
  if (n == 0) { ok(0, "a header is written"); return; }
  lua_pushlstring(L, buf, n);
  lua_getglobal(L, "msgpack");
  lua_getfield(L, -1, "decode");
  lua_pushvalue(L, -3);
  lua_call(L, 1, 1);
  lua_pushstring(L, "wrong-runtime");
  lua_setfield(L, -2, "runtime");
  lua_pushstring(L, "wrong-permanents");
  lua_setfield(L, -2, "permanents");
  lua_pushstring(L, "wrong-caps");
  lua_setfield(L, -2, "capabilities");
  lua_getfield(L, -2, "encode");
  lua_pushvalue(L, -2);
  lua_call(L, 1, 1);
  bytes = lua_tolstring(L, -1, &len);
  ok(diluvium_snap_checkheader(L, NULL, bytes, len, &used)
     == DILUVIUM_SNAP_RUNTIME_MISMATCH,
     "with several fields wrong, the runtime mismatch is what is reported");
  lua_settop(L, base);
}


static void the_host_stamp_is_asymmetric (lua_State *L) {
  char plain[4096], stamped[4096];
  size_t np, ns, used = 0;
  diluvium_snap_opts opts;
  memset(&opts, 0, sizeof(opts));
  np = diluvium_snap_header(L, NULL, plain, sizeof(plain));
  opts.host = "host-alpha";
  ns = diluvium_snap_header(L, &opts, stamped, sizeof(stamped));
  if (np == 0 || ns == 0) { ok(0, "both headers are written"); return; }

  ok(diluvium_snap_checkheader(L, NULL, plain, np, &used)
     == DILUVIUM_SNAP_ACCEPT,
     "an unstamped snapshot restores where no stamp is required");
  ok(diluvium_snap_checkheader(L, &opts, stamped, ns, &used)
     == DILUVIUM_SNAP_ACCEPT, "a stamped snapshot restores under its own host");
  /* The two directions that make stamping mean something rather than being
     advisory. */
  ok(diluvium_snap_checkheader(L, NULL, stamped, ns, &used)
     == DILUVIUM_SNAP_HOST_MISMATCH,
     "a stamped snapshot is refused where no stamp is expected");
  ok(diluvium_snap_checkheader(L, &opts, plain, np, &used)
     == DILUVIUM_SNAP_HOST_MISMATCH,
     "an unstamped snapshot is refused by a host that requires a stamp");
  {
    diluvium_snap_opts other;
    memset(&other, 0, sizeof(other));
    other.host = "host-beta";
    ok(diluvium_snap_checkheader(L, &other, stamped, ns, &used)
       == DILUVIUM_SNAP_HOST_MISMATCH,
       "and a different host is refused");
  }
}


static void the_capability_set_is_compared (lua_State *L) {
  char buf[4096];
  size_t n, used = 0;
  diluvium_snap_opts privileged, plain;
  memset(&privileged, 0, sizeof(privileged));
  memset(&plain, 0, sizeof(plain));
  privileged.capabilities = "spawn,net";
  n = diluvium_snap_header(L, &privileged, buf, sizeof(buf));
  if (n == 0) { ok(0, "a privileged header is written"); return; }
  ok(diluvium_snap_checkheader(L, &privileged, buf, n, &used)
     == DILUVIUM_SNAP_ACCEPT,
     "a snapshot restores under the capability set it was taken with");
  /* 10.6: "capability enforcement on restore is not optional". This is the
     check that makes a privilege hierarchy structural rather than a
     convention. */
  ok(diluvium_snap_checkheader(L, &plain, buf, n, &used)
     == DILUVIUM_SNAP_CAPABILITY_MISMATCH,
     "and is refused under a lesser one");
}


static void queue_names_travel_in_the_header (lua_State *L) {
  char buf[4096];
  size_t n;
  int base = lua_gettop(L);
  /* 10.8: handles do not survive, so restore re-declares by name. Names are
     what the header carries; a handle would be carrying the very thing 6.2
     says must not survive. */
  if (luaL_dostring(L, "queue.declare('snap/inbox', {cap = 4}) "
                       "queue.declare('snap/outbox', {cap = 4})") != LUA_OK) {
    ok(0, "the queues declare");
    lua_settop(L, base);
    return;
  }
  n = diluvium_snap_header(L, NULL, buf, sizeof(buf));
  if (n == 0) { ok(0, "a header is written"); lua_settop(L, base); return; }
  ok(diluvium_snap_headerqueues(L, buf, n), "the header's queue list reads back");
  if (lua_istable(L, -1)) {
    int found_in = 0, found_out = 0;
    lua_Integer i, len = (lua_Integer)lua_rawlen(L, -1);
    for (i = 1; i <= len; i++) {
      const char *nm;
      lua_rawgeti(L, -1, i);
      nm = lua_tostring(L, -1);
      if (nm != NULL && strcmp(nm, "snap/inbox") == 0) found_in = 1;
      if (nm != NULL && strcmp(nm, "snap/outbox") == 0) found_out = 1;
      lua_pop(L, 1);
    }
    ok(found_in && found_out, "and both declared queues are named in it");
  }
  else
    ok(0, "and both declared queues are named in it");
  lua_settop(L, base);
}


static void every_refusal_has_its_own_sentence (lua_State *L) {
  int c, distinct = 1;
  (void)L;
  for (c = 0; c <= DILUVIUM_SNAP_HOST_MISMATCH; c++) {
    const char *r = diluvium_snap_why(c);
    if (r == NULL || *r == '\0' ||
        strcmp(r, diluvium_snap_why(DILUVIUM_SNAP_HOST_MISMATCH + 50)) == 0)
      distinct = 0;
  }
  ok(distinct, "every header refusal code has its own sentence");
}


int main (void) {
  lua_State *L = luaL_newstate();
  if (L == NULL) {
    printf("[FAIL] could not create a state\n");
    return 1;
  }
  luaL_openlibs(L);
  diluvium_openlibs(L);

  printf("=== snapshot graph: values ===\n");
  plain_values_survive(L);

  printf("\n=== snapshot graph: identity ===\n");
  sharing_is_preserved(L);
  distinct_tables_stay_distinct(L);
  a_cycle_closes(L);
  the_root_can_be_referenced_from_deep_inside(L);
  deep_nesting_is_allowed(L);

  printf("\n=== snapshot graph: metatables ===\n");
  a_metatable_survives(L);
  a_shared_metatable_stays_shared(L);
  a_metatable_in_the_graph_is_not_duplicated(L);
  a_metatable_with_a_metatable(L);
  a_self_metatable_survives(L);
  mode_comes_along_with_the_metatable(L);

  printf("\n=== snapshot graph: refusals ===\n");
  light_userdata_is_refused(L);
  a_shape_wrapper_is_refused(L);
  a_function_is_refused_without_hooks(L);

  printf("\n=== snapshot graph: malformed streams (10.10) ===\n");
  a_forward_backreference_is_refused(L);
  a_short_backreference_is_refused(L);
  a_missing_terminator_is_refused(L);
  trailing_bytes_are_refused(L);
  a_bad_metatable_entry_is_refused(L);
  a_bad_owner_position_is_refused(L);
  an_out_of_range_owner_is_refused(L);
  a_truncated_metatable_entry_is_refused(L);

  printf("\n=== the runtime fingerprint and header (10.5, 10.6) ===\n");
  the_fingerprint_is_stable_and_looks_like_a_hash(L);
  a_fresh_header_is_accepted(L);
  a_payload_after_the_header_is_found(L);
  a_short_buffer_is_reported_not_overrun(L);
  garbage_is_refused_without_raising(L);
  a_wrong_format_is_refused(L);
  a_foreign_runtime_is_refused(L);
  a_missing_field_is_refused(L);
  the_runtime_check_comes_first(L);
  the_host_stamp_is_asymmetric(L);
  the_capability_set_is_compared(L);
  queue_names_travel_in_the_header(L);
  every_refusal_has_its_own_sentence(L);

  printf("\n=== the plain codec is unchanged ===\n");
  a_snapshot_ext_is_refused_by_plain_decode(L);
  plain_encode_still_refuses_a_cycle(L);

  lua_close(L);
  printf("\n%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
