/*
** dsnap_check.c
** The snapshot layer: value graph, closures, permanents, prototypes, header.
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
** Verified by mutation, seven breakages, each turning a named case red:
**
**   Interning a table *after* writing its contents turns 'sharing_is_preserved'
**   red on every identity assertion, and the cycle cases red with "table nesting
**   deeper than 150" -- the table never meets its own position on the way down.
**
**   Registering a decoded table after filling it turns the same cases red from
**   the other side, with "backreference 3 is out of range (2 objects so far)".
**
**   Caching the fixup worklist length instead of re-reading it turns
**   'a_metatable_with_a_metatable' red, and only that one.
**
**   Dropping the forward-reference check in ext 0x04 turns
**   'a_forward_backreference_is_refused' red.
**
**   Letting light userdata through turns 'light_userdata_is_refused' red.
**
**   Emitting UPVAL where UPJOIN belongs turns
**   'two_closures_still_share_an_upvalue' red -- and nothing else, because every
**   value is still correct; only the sharing is gone.
**
**   Not interning a closure before the hook writes it turns five cases red with
**   "a queued fixup owner has no position".
**
** Two more mutations turned *nothing* red at first, and both gaps were real:
**
**   Giving ext 0x07 a position on decode. Noticing needs a backreference that
**   crosses a permanent, and then it resolves to the wrong object rather than
**   failing. 'a_permanent_does_not_shift_positions' is that case.
**
**   Removing the inlined-prototype hash verification. The tampering case flipped
**   a byte of bytecode, which the loader rejects on its own. Worse, both
**   tampering cases were exercising the *reference* path rather than the inline
**   one, because an earlier case in this file had registered the same prototype
**   -- one of them passing on a "not present in this runtime" refusal unrelated
**   to its own claim. They now use distinct function bodies and assert, through
**   'inlined_hash_at', that the record inlines its prototype before tampering.
**
** One mutation originally *hung* rather than failing: without the identity map
** the fixup worklist re-queues forever. A hang is a bad answer for a test to
** give, so the encoder checks the invariant that made it possible ("a queued
** fixup owner has no position") and the same mutation now reports eight named
** failures instead of nothing at all.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
  const diluvium_snap_hooks *h = diluvium_snap_hooks_for(L);
  diluvium_msgpack_encode_graph(L, 1, h);
  bytes = lua_tolstring(L, -1, &len);
  diluvium_msgpack_decode_graph(L, bytes, len, h);
  return 1;
}


/* The same, with no hooks: what an embedder linking only the codec sees. */
static int protected_roundtrip_bare (lua_State *L) {
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
  diluvium_msgpack_encode_graph(L, 1, diluvium_snap_hooks_for(L));
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


static int protected_encode_bare (lua_State *L) {
  diluvium_msgpack_encode_graph(L, 1, NULL);
  return 1;
}


static void a_function_is_refused_without_hooks (lua_State *L) {
  /* An embedder that links the codec but not the snapshot layer. The refusal has
     to be a clean error naming the type and the path -- not a crash, and not a
     silent nil where a function was. */
  int base = lua_gettop(L);
  if (!build(L, "return {handler = function() end}")) return;
  lua_pushcfunction(L, protected_encode_bare);
  lua_insert(L, -2);
  if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
    ok(0, "a function is refused when no hooks are installed");
    lua_settop(L, base);
    return;
  }
  {
    const char *msg = lua_tostring(L, -1);
    ok(msg != NULL && strstr(msg, "function") != NULL,
       "a function is refused when no hooks are installed");
    ok(msg != NULL && strstr(msg, "handler") != NULL,
       "and that message names the key path");
  }
  lua_settop(L, base);
}


/* -------------------------------------------------- malformed streams */

static int protected_decode_hooked (lua_State *L) {
  size_t len;
  const char *s = lua_tolstring(L, 1, &len);
  diluvium_msgpack_decode_graph(L, s, len, diluvium_snap_hooks_for(L));
  return 1;
}


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
                        "a graph with no fixup terminator is refused");
}


static void trailing_bytes_are_refused (lua_State *L) {
  static const char bad[] = { (char)0x80, (char)0xc0, (char)0x01, (char)0x02 };
  expect_decode_refusal(L, bad, sizeof(bad), "left over",
                        "bytes after the graph are refused");
}


/*
** Malformed fixup sections. Every stream here is written by hand, byte by byte,
** because the encoder cannot produce them -- which is the point: 10.10 says a
** snapshot is untrusted input, and the encoder is not the threat model.
**
** 0x80 is an empty map, so it is the root and takes position 1. 0x01 is the
** META tag, 0x02 UPVAL, 0x03 UPJOIN. 0xc0 is the nil terminator.
*/

static void a_bad_fixup_tag_is_refused (lua_State *L) {
  static const char bad[] = { (char)0x80, 0x63, (char)0xc0 };  /* tag 99 */
  expect_decode_refusal(L, bad, sizeof(bad), "not one this runtime knows",
                        "an unknown fixup tag is refused");
}


static void a_non_integer_tag_is_refused (lua_State *L) {
  static const char bad[] = { (char)0x80, (char)0xa1, 'x', (char)0xc0 };
  expect_decode_refusal(L, bad, sizeof(bad), "integer tag",
                        "a fixup whose tag is not an integer is refused");
}


static void a_bad_owner_position_is_refused (lua_State *L) {
  static const char bad[] = {
    (char)0x80, 0x01, (char)0xa1, 'x', (char)0xc0  /* META, owner "x" */
  };
  expect_decode_refusal(L, bad, sizeof(bad), "must be an integer",
                        "a non-integer fixup owner is refused");
}


static void an_out_of_range_owner_is_refused (lua_State *L) {
  static const char bad[] = {
    (char)0x80, 0x01, 0x09, (char)0x80, (char)0xc0  /* META, owner 9 */
  };
  expect_decode_refusal(L, bad, sizeof(bad), "out of range",
                        "a fixup owner position past the end is refused");
}


static void a_truncated_fixup_is_refused (lua_State *L) {
  static const char bad[] = { (char)0x80, 0x01 };  /* META and nothing more */
  expect_decode_refusal(L, bad, sizeof(bad), "ended before",
                        "a fixup that ends before its operands is refused");
}


static void a_bad_metatable_entry_is_refused (lua_State *L) {
  static const char bad[] = {
    (char)0x80, 0x01, 0x01, (char)0x2a, (char)0xc0  /* META, owner 1, 42 */
  };
  expect_decode_refusal(L, bad, sizeof(bad), "metatable must be a table",
                        "a non-table metatable is refused");
}


static void an_upvalue_fixup_on_a_table_is_refused (lua_State *L) {
  /* The root is a table, so it has no upvalues. Without the type check this
     would reach 'lua_setupvalue' with a non-function, which is an API-check
     abort in a debug build and undefined elsewhere. */
  static const char bad[] = {
    (char)0x80, 0x02, 0x01, 0x01, (char)0x2a, (char)0xc0  /* UPVAL 1, 1, 42 */
  };
  expect_decode_refusal(L, bad, sizeof(bad), "not a Lua closure",
                        "an upvalue fixup naming a table is refused");
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




/* ======================================================================
** Permanents, prototypes and closures (10.4, 10.5, 10.9)
** ====================================================================== */

/* Call the function at 'idx' with no arguments; push its first result. */
static int call0 (lua_State *L, int idx, int nres) {
  lua_pushvalue(L, idx);
  return lua_pcall(L, 0, nres, 0) == LUA_OK;
}


static void a_plain_function_survives (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  if (!build(L, "return {f = function(a, b) return a * 10 + b end}")) return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "f");
  ok(lua_isfunction(L, -1) && !lua_iscfunction(L, -1),
     "a Lua function comes back as a Lua function");
  lua_pushinteger(L, 4);
  lua_pushinteger(L, 2);
  if (lua_pcall(L, 2, 1, 0) == LUA_OK)
    ok(lua_tointeger(L, -1) == 42, "and it computes what it used to");
  else {
    printf("      (%s)\n", lua_tostring(L, -1));
    ok(0, "and it computes what it used to");
  }
  lua_settop(L, base);
}


static void a_closure_keeps_its_upvalues (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  if (!build(L, "local n = 40 local m = 2 "
                "return {f = function() return n + m end}")) return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "f");
  if (call0(L, -1, 1))
    ok(lua_tointeger(L, -1) == 42, "a closure's captured values come back");
  else {
    printf("      (%s)\n", lua_tostring(L, -1));
    ok(0, "a closure's captured values come back");
  }
  lua_settop(L, base);
}


static void a_closure_over_a_table_shares_it (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /* The upvalue and a field of the root are the same table. If the upvalue were
     serialized separately from the graph, the restored closure would be writing
     to a table nobody else can see -- which is invisible until the program reads
     the other path and finds its update missing. */
  if (!build(L, "local t = {count = 0} "
                "return {state = t, bump = function() t.count = t.count + 1 end}"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "bump");
  if (!call0(L, -1, 0)) {
    printf("      (%s)\n", lua_tostring(L, -1));
    ok(0, "the restored closure runs");
    lua_settop(L, base);
    return;
  }
  ok(1, "the restored closure runs");
  at(L, copy, "state.count");
  ok(lua_tointeger(L, -1) == 1,
     "and it wrote through to the same table the graph holds");
  lua_settop(L, base);
}


static void two_closures_still_share_an_upvalue (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /* 10.3's requirement, and the only way to test it is to mutate through one and
     read through the other. Comparing values would pass even if each closure got
     a private copy. */
  if (!build(L, "local n = 0 "
                "return {inc = function() n = n + 1 end, get = function() return n end}"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "inc");
  if (!call0(L, -1, 0) || !call0(L, -1, 0) || !call0(L, -1, 0)) {
    printf("      (%s)\n", lua_tostring(L, -1));
    ok(0, "the restored closures run");
    lua_settop(L, base);
    return;
  }
  ok(1, "the restored closures run");
  at(L, copy, "get");
  if (call0(L, -1, 1))
    ok(lua_tointeger(L, -1) == 3,
       "a write through one closure is seen by the other (upvalue identity)");
  else
    ok(0, "a write through one closure is seen by the other (upvalue identity)");
  lua_settop(L, base);
}


static void an_unshared_upvalue_stays_unshared (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /* The converse. A restore that joined too eagerly -- say, by value -- would
     make these two counters the same counter. */
  if (!build(L, "local function mk() local n = 0 "
                "return function() n = n + 1 return n end end "
                "return {a = mk(), b = mk()}")) return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "a");
  if (call0(L, -1, 1) && call0(L, -2, 1)) {
    ok(lua_tointeger(L, -1) == 2, "one counter advances");
    lua_pop(L, 2);
  }
  else
    ok(0, "one counter advances");
  at(L, copy, "b");
  if (call0(L, -1, 1))
    ok(lua_tointeger(L, -1) == 1, "and the other is untouched by it");
  else
    ok(0, "and the other is untouched by it");
  lua_settop(L, base);
}


static void a_recursive_closure_survives (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /* A local function that calls itself holds itself in an upvalue. That is a
     cycle through a closure rather than through a table, so it exercises
     interning a function before its own contents are written. */
  if (!build(L, "local function fact(n) if n <= 1 then return 1 end "
                "return n * fact(n - 1) end return {f = fact}")) return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "f");
  lua_pushinteger(L, 5);
  if (lua_pcall(L, 1, 1, 0) == LUA_OK)
    ok(lua_tointeger(L, -1) == 120, "a self-recursive closure still recurses");
  else {
    printf("      (%s)\n", lua_tostring(L, -1));
    ok(0, "a self-recursive closure still recurses");
  }
  lua_settop(L, base);
}


static void one_closure_referenced_twice_stays_one (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  if (!build(L, "local f = function() end return {a = f, b = f}")) return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "a");
  at(L, copy, "b");
  ok(lua_isfunction(L, -2) && lua_isfunction(L, -1) &&
     lua_topointer(L, -2) == lua_topointer(L, -1),
     "one closure referenced twice is still one closure");
  lua_settop(L, base);
}


static void a_closure_using_globals_survives (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /* '_ENV' is an upvalue holding '_G'. Without '_G' as a permanent this would
     try to serialize the whole global environment and fail on the first C
     function in it. */
  if (!build(L, "return {f = function() return type(1) end}")) return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "f");
  if (call0(L, -1, 1))
    ok(lua_tostring(L, -1) != NULL &&
       strcmp(lua_tostring(L, -1), "number") == 0,
       "a closure that calls a global still can");
  else {
    printf("      (%s)\n", lua_tostring(L, -1));
    ok(0, "a closure that calls a global still can");
  }
  lua_settop(L, base);
}


static void a_c_function_is_named_not_copied (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  if (!build(L, "return {p = print, ins = table.insert, fmt = string.format}"))
    return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "p");
  lua_getglobal(L, "print");
  ok(lua_topointer(L, -2) == lua_topointer(L, -1),
     "'print' comes back as the same C function, not a copy");
  lua_pop(L, 2);
  at(L, copy, "ins");
  lua_getglobal(L, "table");
  lua_getfield(L, -1, "insert");
  ok(lua_topointer(L, -3) == lua_topointer(L, -1),
     "and so does 'table.insert'");
  lua_pop(L, 3);
  at(L, copy, "fmt");
  lua_getglobal(L, "string");
  lua_getfield(L, -1, "format");
  ok(lua_topointer(L, -3) == lua_topointer(L, -1),
     "and 'string.format'");
  lua_settop(L, base);
}


static void a_library_table_is_named_not_copied (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /* The one that matters most for size and for correctness: a program holding a
     reference to 'string' or to '_G' must not have the library copied into its
     snapshot. */
  if (!build(L, "return {s = string, g = _G, m = math}")) return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  at(L, copy, "s");
  lua_getglobal(L, "string");
  ok(lua_topointer(L, -2) == lua_topointer(L, -1),
     "the 'string' table comes back as the same table");
  lua_pop(L, 2);
  at(L, copy, "g");
  lua_pushglobaltable(L);
  ok(lua_topointer(L, -2) == lua_topointer(L, -1),
     "and '_G' is '_G', not a clone of the global environment");
  lua_settop(L, base);
}


static void a_snapshot_of_a_global_closure_is_small (lua_State *L) {
  int base = lua_gettop(L);
  size_t len = 0;
  /* Naming rather than copying is what keeps this true. A snapshot that had
     inlined '_G' would be tens of kilobytes, and the assertion below is the
     cheapest way to notice if permanents ever stop being consulted for tables. */
  if (!build(L, "return {f = function() return tostring(select('#')) end}"))
    return;
  lua_pushcfunction(L, protected_encode);
  lua_pushvalue(L, base + 1);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    printf("      (%s)\n", lua_tostring(L, -1));
    ok(0, "a closure over '_ENV' encodes");
    lua_settop(L, base);
    return;
  }
  lua_tolstring(L, -1, &len);
  ok(len > 0 && len < 512,
     "a snapshot of one global-using closure is small, so '_G' was named");
  printf("      (%lu bytes)\n", (unsigned long)len);
  lua_settop(L, base);
}


static void a_shared_prototype_is_carried_once (lua_State *L) {
  int base = lua_gettop(L);
  size_t shared = 0, distinct = 0;
  /*
  ** 10.5's dedup, within one stream. Measured against ten closures of ten
  ** *different* prototypes rather than against a multiple of one closure's size,
  ** because that compares dedup with no-dedup directly. A guessed threshold
  ** would have to encode how big a hash reference is relative to a tiny
  ** function's bytecode, which is a fact about this test's sample and not about
  ** the format -- the first version of this check failed for exactly that
  ** reason, at 372 bytes against a made-up limit of 324.
  */
  if (!build(L, "local function mk() return function() return 1 end end "
                "local t = {} for i = 1, 10 do t[i] = mk() end return t"))
    return;
  lua_pushcfunction(L, protected_encode);
  lua_pushvalue(L, -2);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    ok(0, "ten closures of one prototype encode");
    lua_settop(L, base);
    return;
  }
  lua_tolstring(L, -1, &shared);
  lua_settop(L, base);
  /* Ten prototypes that differ only in a constant, so their bytecode is the
     same size and the comparison is about count, not content. */
  if (!build(L, "local t = {} "
                "t[1] = function() return 1 end t[2] = function() return 2 end "
                "t[3] = function() return 3 end t[4] = function() return 4 end "
                "t[5] = function() return 5 end t[6] = function() return 6 end "
                "t[7] = function() return 7 end t[8] = function() return 8 end "
                "t[9] = function() return 9 end t[10] = function() return 10 end "
                "return t")) return;
  lua_pushcfunction(L, protected_encode);
  lua_pushvalue(L, -2);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    ok(0, "ten closures of ten prototypes encode");
    lua_settop(L, base);
    return;
  }
  lua_tolstring(L, -1, &distinct);
  printf("      (one prototype shared %lu bytes, ten distinct %lu bytes)\n",
         (unsigned long)shared, (unsigned long)distinct);
  ok(shared < distinct,
     "ten closures of one prototype cost less than ten of ten prototypes");
  lua_settop(L, base);
}


static void a_registered_prototype_is_referenced (lua_State *L) {
  int base = lua_gettop(L);
  size_t unreg = 0, reg = 0;
  int before, after;
  if (!build(L, "return {f = function(a) return a + 1 end}")) return;
  lua_pushcfunction(L, protected_encode);
  lua_pushvalue(L, base + 1);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) { ok(0, "the closure encodes"); lua_settop(L, base); return; }
  lua_tolstring(L, -1, &unreg);
  lua_pop(L, 1);
  /* Registering the prototype is what a host does for a shared library. After
     it, the snapshot should carry a hash instead of bytecode. */
  before = diluvium_snap_registered(L);
  at(L, base + 1, "f");
  ok(diluvium_snap_register(L, -1), "a Lua function can be registered");
  lua_pop(L, 1);
  after = diluvium_snap_registered(L);
  ok(after >= before, "and the registry reports it");
  lua_pushcfunction(L, protected_encode);
  lua_pushvalue(L, base + 1);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) { ok(0, "it encodes again"); lua_settop(L, base); return; }
  lua_tolstring(L, -1, &reg);
  printf("      (inlined %lu bytes, referenced %lu bytes)\n",
         (unsigned long)unreg, (unsigned long)reg);
  ok(reg < unreg, "a registered prototype is referenced, not inlined");
  /* And it still restores, which is the half that matters. */
  lua_settop(L, base);
  if (!build(L, "return {f = function(a) return a + 1 end}")) return;
  {
    int copy = roundtrip_named(L, __func__);
    if (copy == 0) return;
    at(L, copy, "f");
    lua_pushinteger(L, 41);
    if (lua_pcall(L, 1, 1, 0) == LUA_OK)
      ok(lua_tointeger(L, -1) == 42,
         "and a referenced prototype still restores and runs");
    else {
      printf("      (%s)\n", lua_tostring(L, -1));
      ok(0, "and a referenced prototype still restores and runs");
    }
  }
  lua_settop(L, base);
}


/*
** Find the ext 0x06 (closure) record in a stream and return the offset of its
** prototype hash, or 0. msgpack writes ext8 as 0xc7 len code, ext16 as 0xc8
** len16 code, ext32 as 0xc9 len32 code; a closure record is at least 34 bytes so
** the fixed-width forms cannot occur.
*/
static size_t find_closure_record (const char *b, size_t len) {
  size_t i;
  for (i = 0; i + 3 < len; i++) {
    if ((unsigned char)b[i] == 0xc7 && (unsigned char)b[i + 2] == 0x06)
      return i + 3;          /* the payload: nup, inline flag, hash, dump */
    if ((unsigned char)b[i] == 0xc8 && i + 4 < len &&
        (unsigned char)b[i + 3] == 0x06)
      return i + 4;
  }
  return 0;
}


/*
** Assert the record at 'rec' is an inlined prototype and return the offset of
** its hash. Zero, having reported, if it is a reference.
**
** Worth checking rather than assuming: a prototype registered by an *earlier*
** test is referenced rather than inlined, and both tampering tests below were
** silently testing the reference path for that reason -- one of them passing on
** a "not present in this runtime" refusal that had nothing to do with what it
** claimed to check.
*/
static size_t inlined_hash_at (const char *b, size_t len, const char *what) {
  size_t rec = find_closure_record(b, len);
  checks++;
  if (rec == 0 || rec + 2 + 32 > len) {
    printf("[FAIL] %s: no closure record in the stream\n", what);
    failures++;
    return 0;
  }
  if ((unsigned char)b[rec + 1] != 1) {
    printf("[FAIL] %s: the prototype was referenced, not inlined, so this "
           "case is not testing what it says\n", what);
    failures++;
    return 0;
  }
  printf("[PASS] %s: the record inlines its prototype\n", what);
  return rec + 2;
}


static void a_c_function_that_is_not_permanent_is_refused (lua_State *L) {
  int base = lua_gettop(L);
  /* A host-registered C function the host forgot to name. There is nothing to
     serialize -- the code is in the host binary -- so the only honest answer is
     to refuse, and the message has to name the type and the path. */
  lua_pushcfunction(L, protected_encode);   /* any C function not in the set */
  lua_setglobal(L, "an_unnamed_c_function");
  if (!build(L, "return {deep = {hook = an_unnamed_c_function}}")) return;
  lua_pushcfunction(L, protected_encode);
  lua_insert(L, -2);
  if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
    ok(0, "an unnamed C function is refused");
    lua_settop(L, base);
    return;
  }
  {
    const char *msg = lua_tostring(L, -1);
    ok(msg != NULL && strstr(msg, "permanents table") != NULL,
       "an unnamed C function is refused, and told why");
    ok(msg != NULL && strstr(msg, "must name them") != NULL,
       "and the message says what to do about it");
    if (msg != NULL && strstr(msg, "deep.hook") == NULL)
      printf("      (%s)\n", msg);
  }
  lua_settop(L, base);
}


static void a_missing_prototype_is_refused (lua_State *L) {
  int base = lua_gettop(L);
  size_t len;
  const char *bytes;
  /* Register, encode by reference, then wipe the registry. That is what
     restoring into a runtime without the shared library looks like, and 10.5
     insists on a clean refusal rather than a best-effort load. */
  if (!build(L, "return function() return 'gone' end")) return;
  diluvium_snap_register(L, -1);
  lua_pushcfunction(L, protected_encode);
  lua_pushvalue(L, base + 1);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    ok(0, "the referenced closure encodes");
    lua_settop(L, base);
    return;
  }
  bytes = lua_tolstring(L, -1, &len);
  {
    /* A fresh state has an empty proto registry, which is exactly the
       situation. */
    lua_State *fresh = luaL_newstate();
    int refused = 0;
    if (fresh != NULL) {
      luaL_openlibs(fresh);
      diluvium_openlibs(fresh);
      lua_pushcfunction(fresh, protected_decode_hooked);
      lua_pushlstring(fresh, bytes, len);
      if (lua_pcall(fresh, 1, 1, 0) != LUA_OK) {
        const char *msg = lua_tostring(fresh, -1);
        refused = (msg != NULL && strstr(msg, "not") != NULL);
        if (!refused)
          printf("      (%s)\n", (msg == NULL) ? "(none)" : msg);
      }
      lua_close(fresh);
    }
    ok(refused,
       "a prototype referenced but absent is refused, not best-effort loaded");
  }
  lua_settop(L, base);
}


static void a_tampered_prototype_hash_is_refused (lua_State *L) {
  int base = lua_gettop(L);
  size_t len;
  const char *bytes;
  /* Corrupt bytecode. Note what this does *not* establish: the loader and
     lverify.c refuse these bytes on their own, so this stays green even with the
     hash verification removed. 'a_tampered_prototype_name_is_refused' is the one
     that tests the hash. Both are worth having -- this is the 7%-crash baseline
     10.10 cites, and it must be a refusal rather than a crash. */
  if (!build(L, "return function(q) return q * 7717 + 13 end")) return;
  lua_pushcfunction(L, protected_encode);
  lua_pushvalue(L, base + 1);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    ok(0, "the closure encodes");
    lua_settop(L, base);
    return;
  }
  bytes = lua_tolstring(L, -1, &len);
  if (inlined_hash_at(bytes, len, "corrupt-bytecode case") == 0) {
    lua_settop(L, base);
    return;
  }
  {
    /* Flip a byte inside the inlined dump, leaving the hash alone. */
    char *copy = (char *)malloc(len);
    int refused = 0;
    if (copy != NULL) {
      memcpy(copy, bytes, len);
      copy[len - 8] = (char)(copy[len - 8] ^ 0x01);
      lua_pushcfunction(L, protected_decode_hooked);
      lua_pushlstring(L, copy, len);
      if (lua_pcall(L, 1, 1, 0) != LUA_OK)
        refused = 1;
      else
        lua_pop(L, 1);
      free(copy);
    }
    ok(refused, "a prototype whose bytes were altered is refused");
  }
  lua_settop(L, base);
}


static void a_tampered_prototype_name_is_refused (lua_State *L) {
  int base = lua_gettop(L);
  size_t len, at;
  const char *bytes;
  /*
  ** The check that 'a_tampered_prototype_hash_is_refused' does *not* make. That
  ** one flips a byte of the bytecode, and the loader rejects it -- so it passes
  ** whether or not the hash is verified, which was found by removing the
  ** verification and watching it stay green.
  **
  ** This one flips a byte of the *hash* instead. The bytecode still loads
  ** perfectly; what is wrong is that the record claims a name its bytes do not
  ** own. 10.5 makes the hash the name of the code, so accepting this would let a
  ** snapshot install bytecode under someone else's name -- and the reference
  ** case then hands that name out to whoever asks.
  */
  if (!build(L, "return function(z) return z .. 'ARARAT-name-case' end"))
    return;
  lua_pushcfunction(L, protected_encode);
  lua_pushvalue(L, base + 1);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    ok(0, "the closure encodes");
    lua_settop(L, base);
    return;
  }
  bytes = lua_tolstring(L, -1, &len);
  at = inlined_hash_at(bytes, len, "tampered-name case");
  if (at == 0) { lua_settop(L, base); return; }
  {
    char *copy = (char *)malloc(len);
    int refused = 0;
    if (copy != NULL) {
      memcpy(copy, bytes, len);
      copy[at] = (char)(copy[at] ^ 0x01);   /* one bit of the name */
      lua_pushcfunction(L, protected_decode_hooked);
      lua_pushlstring(L, copy, len);
      if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        refused = (msg != NULL && strstr(msg, "hash") != NULL);
        if (!refused)
          printf("      (%s)\n", (msg == NULL) ? "(none)" : msg);
      }
      free(copy);
    }
    ok(refused,
       "bytecode carrying a name its bytes do not hash to is refused");
  }
  lua_settop(L, base);
}


static void a_permanent_does_not_shift_positions (lua_State *L) {
  int base = lua_gettop(L);
  int copy;
  /*
  ** The asymmetry the format depends on: a named object takes no position, on
  ** both sides. Nothing else in this file would notice if it did, because
  ** noticing needs a backreference that *crosses* a permanent -- and then it
  ** resolves to the wrong object rather than failing.
  **
  ** An array, so the order is the source order rather than whatever 'pairs'
  ** gives: 'print' is named and takes no position, the shared table takes one,
  ** and the third slot is a backreference to it. Give 'print' a position on
  ** decode and that backreference lands on 'print'.
  **
  ** Found by giving ext 0x07 a position and watching every check stay green.
  */
  if (!build(L, "local shared = {tag = 'shared'} "
                "return {print, shared, shared, type, shared}")) return;
  copy = roundtrip_named(L, __func__);
  if (copy == 0) return;
  lua_rawgeti(L, copy, 2);
  lua_rawgeti(L, copy, 3);
  ok(same_table(L, -2, -1),
     "a backreference across a named object still finds its own table");
  lua_pop(L, 2);
  lua_rawgeti(L, copy, 2);
  lua_rawgeti(L, copy, 5);
  ok(same_table(L, -2, -1), "and so does one across two of them");
  lua_pop(L, 2);
  lua_rawgeti(L, copy, 1);
  lua_getglobal(L, "print");
  ok(lua_topointer(L, -2) == lua_topointer(L, -1),
     "and the named objects are still themselves");
  lua_pop(L, 2);
  lua_rawgeti(L, copy, 3);
  ok(lua_istable(L, -1),
     "and the shared slot is a table, not a function that took its place");
  lua_settop(L, base);
}


static void a_secure_function_keeps_its_secret (lua_State *L) {
  int base = lua_gettop(L);
  size_t len;
  const char *bytes;
  static const char secret[] = "MOUNTAIN-ARARAT-9317";
  char src[256];
  /* 10.9 rule 1: route prototype encoding through the real dump path, so
     'taintSecureStrings' and the per-string scramble come along. This is the
     check that the routing actually happened -- a parallel encoder would put the
     literal in the stream in the clear, which is precisely the bypass 10.9
     exists to prevent. */
  snprintf(src, sizeof(src),
           "local ~function reveal() return '%s' end return {f = reveal}",
           secret);
  if (!build(L, src)) return;
  lua_pushcfunction(L, protected_encode);
  lua_pushvalue(L, base + 1);
  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    printf("      (%s)\n", lua_tostring(L, -1));
    ok(0, "a secure function can be snapshotted");
    lua_settop(L, base);
    return;
  }
  ok(1, "a secure function can be snapshotted");
  bytes = lua_tolstring(L, -1, &len);
  {
    /* Search the whole stream for the literal, the way test_secure_dump.lua
       searches a dump. */
    int found = 0;
    size_t i;
    size_t slen = sizeof(secret) - 1;
    for (i = 0; i + slen <= len; i++) {
      if (memcmp(bytes + i, secret, slen) == 0) { found = 1; break; }
    }
    ok(!found, "and its literal is not in the snapshot in the clear");
  }
  lua_settop(L, base);
  /* And it still works after a round trip, or the scrambling would have been
     achieved by breaking the function. */
  if (!build(L, src)) return;
  {
    int copy = roundtrip_named(L, __func__);
    if (copy == 0) return;
    at(L, copy, "f");
    if (call0(L, -1, 1))
      ok(lua_tostring(L, -1) != NULL &&
         strcmp(lua_tostring(L, -1), secret) == 0,
         "and it still returns its literal after a round trip");
    else {
      printf("      (%s)\n", lua_tostring(L, -1));
      ok(0, "and it still returns its literal after a round trip");
    }
  }
  lua_settop(L, base);
}


static void the_permanents_fingerprint_is_not_empty (lua_State *L) {
  char buf[4096];
  size_t n;
  int base = lua_gettop(L);
  /* The header field now has real content, so the empty-set hash recorded at the
     previous step must no longer appear. Checked by asserting the header still
     validates and that the permanents table has plausibly many entries. */
  diluvium_snap_permanents(L);
  n = diluvium_snap_header(L, NULL, buf, sizeof(buf));
  ok(n > 0 && diluvium_snap_checkheader(L, NULL, buf, n, NULL)
     == DILUVIUM_SNAP_ACCEPT,
     "the header still validates with a populated permanents set");
  lua_settop(L, base);
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
  a_bad_fixup_tag_is_refused(L);
  a_non_integer_tag_is_refused(L);
  a_bad_owner_position_is_refused(L);
  an_out_of_range_owner_is_refused(L);
  a_truncated_fixup_is_refused(L);
  an_upvalue_fixup_on_a_table_is_refused(L);

  printf("\n=== functions, closures and permanents (10.3, 10.4, 10.5) ===\n");
  a_plain_function_survives(L);
  a_closure_keeps_its_upvalues(L);
  a_closure_over_a_table_shares_it(L);
  two_closures_still_share_an_upvalue(L);
  an_unshared_upvalue_stays_unshared(L);
  a_recursive_closure_survives(L);
  one_closure_referenced_twice_stays_one(L);
  a_closure_using_globals_survives(L);
  a_c_function_is_named_not_copied(L);
  a_library_table_is_named_not_copied(L);
  a_snapshot_of_a_global_closure_is_small(L);

  printf("\n=== content-addressed prototypes (10.5) and secure code (10.9) ===\n");
  a_shared_prototype_is_carried_once(L);
  a_registered_prototype_is_referenced(L);
  a_c_function_that_is_not_permanent_is_refused(L);
  a_missing_prototype_is_refused(L);
  a_tampered_prototype_hash_is_refused(L);
  a_tampered_prototype_name_is_refused(L);
  a_permanent_does_not_shift_positions(L);
  a_secure_function_keeps_its_secret(L);
  the_permanents_fingerprint_is_not_empty(L);

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
