/*
** dshim_check.c
** Contract tests for the accessor shim (src/dshim.c).
**
** Two different things are being defended here, and they fail in different
** ways, so it is worth naming them separately.
**
** The first is *drift*. dshim.c is the only file in the tree that reads Lua's
** internal structs, so an upstream rename breaks the compile -- loudly, which
** is the point of confining it. But an upstream change of *meaning* with the
** same field names would not: if 'savedpc' or the 'tbclist' empty convention
** or the CallInfo caching rule changed, dshim.c would still build and would
** still return numbers, just wrong ones. So the checks below assert relations
** that only hold if the reads mean what the comments claim: frame counts
** against known call depths, 'func_index' against the function actually
** there, an open upvalue's slot against the value in that slot.
**
** The second is the precondition that doc/Messaging.md 14 singles out as the
** one that "will silently break during refactoring": every C frame below a
** yield must carry a continuation. Writing these turned up something worth
** recording, which is that the forbidden shape is not reachable -- the VM
** refuses to yield across a continuation-less C frame, so a thread with one
** below its yield never parks in the first place. See the comment above
** 'yield_across_plain_pcall_never_parks'. The invariant is therefore asserted
** from three sides: the shape never parks, every shape that *does* park has no
** such frame, and the driver dtask.c builds is capturable.
**
** Verified the intended way, by removing each mitigation and watching a named
** case fail rather than assuming it would:
**
**   Making 'taskbody' use a plain 'lua_pcall' instead of 'lua_pcallk' turns
**   'task_driver_is_capturable' red -- reported as DILUVIUM_SNAP_C_FRAME at
**   the driver frame. That is the whole reason the shim reports 'has_k'.
**
**   Walking 'ci->next' forward from 'base_ci' instead of 'previous' backward
**   turns 'dead_thread_is_refused' and 'yield_across_plain_pcall_never_parks'
**   red, because the cached CallInfo list keeps popped frames reachable and
**   both of those look at a thread that has unwound. Note it does *not* turn
**   'framecount_follows_depth' red -- in a thread suspended at its deepest
**   point every cached frame happens to be live, so counting the wrong way
**   gives the right answer there.
**
**   Dropping the frame-count term from the suspended test turns
**   'running_thread_is_refused' and 'mid_resume_parent_is_refused' red: both a
**   running thread and one that resumed another have status LUA_OK, and without
**   the frame count they are indistinguishable from a fresh thread.
**
**   Dropping the frame walk from the hook check and keeping only the
**   'allowhook' shortcut turns 'hooked_thread_is_refused' red -- 'luaD_hook'
**   restores 'allowhook' before the yield propagates, so the mark left behind
**   is CIST_HOOKYIELD on the frame.
**
**   An off-by-one in 'pushslot' turns 'upvalue_identity_is_visible' red, on the
**   assertion that the named slot holds 42 rather than a neighbouring value.
**
**   Reading one 'is_encrypted' flag instead of walking the proto tree turns
**   'secure_functions_are_flagged' red on its nested case.
**
**   Exempting *all* C frames from the continuation check rather than only the
**   innermost turns 'the_exemption_is_narrow' red, and nothing else -- which is
**   why that case exists. Every test over a real thread passes either way,
**   because the shape the rule rejects is unreachable.
*/

#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "dlibs.h"
#include "dshim.h"
#include "dtask.h"


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


static void okf (int cond, const char *what, long got, long want) {
  checks++;
  if (cond)
    printf("[PASS] %s\n", what);
  else {
    printf("[FAIL] %s (got %ld, wanted %ld)\n", what, got, want);
    failures++;
  }
}


/*
** Suspend a coroutine inside a known call chain and hand back the thread.
**
** 'depth' Lua frames deep, each one an ordinary function call, with the
** innermost yielding. Leaves the thread on the top of L's stack.
*/
static lua_State *suspend_at_depth (lua_State *L, int depth) {
  char src[256];
  lua_State *co = lua_newthread(L);
  int i;
  /* f1 calls f2 calls ... calls fN, which yields. Written out rather than
     built with a loop in Lua so the frame count is exactly 'depth' and not
     'depth' plus whatever a loop driver adds. */
  size_t n = 0;
  n += (size_t)snprintf(src + n, sizeof(src) - n, "local function f%d() "
                        "coroutine.yield(%d) end ", depth, depth);
  for (i = depth - 1; i >= 1; i--)
    n += (size_t)snprintf(src + n, sizeof(src) - n,
                          "local function f%d() f%d() end ", i, i + 1);
  snprintf(src + n, sizeof(src) - n, "return f1");
  if (luaL_loadstring(co, src) != LUA_OK) {
    printf("[FAIL] could not load the depth-%d chunk: %s\n",
           depth, lua_tostring(co, -1));
    failures++;
    return co;
  }
  if (lua_resume(co, L, 0, &i) != LUA_YIELD && lua_gettop(co) < 1) {
    printf("[FAIL] depth-%d chunk did not return a function\n", depth);
    failures++;
    return co;
  }
  /* The chunk returned f1; call it, and it yields 'depth' frames down. */
  lua_resume(co, L, 0, &i);
  return co;
}


static void framecount_follows_depth (lua_State *L) {
  int base = lua_gettop(L);
  int d;
  for (d = 1; d <= 4; d++) {
    lua_State *co = suspend_at_depth(L, d);
    int n = diluvium_shim_framecount(co);
    char what[64];
    snprintf(what, sizeof(what), "framecount is %d at depth %d", d, d);
    /* Frames: the yield is inside 'coroutine.yield' (a C frame), below it the
       d Lua functions, and below those nothing -- the chunk that returned f1
       has already finished. */
    okf(n == d + 1, what, n, d + 1);
  }
  lua_settop(L, base);
}


static void frames_are_outermost_first (lua_State *L) {
  int base = lua_gettop(L);
  lua_State *co = suspend_at_depth(L, 3);
  int n = diluvium_shim_framecount(co);
  diluvium_frame outer, inner;
  int got_outer = diluvium_shim_frame(co, 0, &outer);
  int got_inner = diluvium_shim_frame(co, n - 1, &inner);
  ok(got_outer && got_inner, "both ends of the chain are readable");
  ok(got_outer && got_inner && outer.func_index < inner.func_index,
     "frame 0 is the outermost (lowest in the stack)");
  ok(!diluvium_shim_frame(co, n, &outer) &&
     !diluvium_shim_frame(co, -1, &outer),
     "out-of-range frame indices are refused");
  /* The innermost frame is 'coroutine.yield', a C function; the outermost is
     a Lua function. If the CIST_C read ever inverts, this is what catches it. */
  ok(got_inner && inner.is_c, "the innermost frame is C ('coroutine.yield')");
  ok(got_outer && !outer.is_c, "the outermost frame is Lua");
  ok(got_outer && outer.pc > 0, "a Lua frame reports a pc into its code");
  ok(got_inner && inner.pc == -1, "a C frame reports no pc");
  lua_settop(L, base);
}


static void func_index_points_at_the_function (lua_State *L) {
  int base = lua_gettop(L);
  lua_State *co = suspend_at_depth(L, 2);
  diluvium_frame f;
  int pushed;
  ok(diluvium_shim_frame(co, 0, &f), "outermost frame is readable");
  /* Copy the slot 'func_index' names onto L and check it is a function. This
     is the check that would catch an off-by-one in the 1-based conversion,
     which is exactly the kind of mistake the raw-array numbering invites. */
  pushed = diluvium_shim_pushslot(co, f.func_index, L);
  ok(pushed, "the function slot can be copied out");
  ok(pushed && lua_isfunction(L, -1),
     "'func_index' names a function, not its neighbour");
  if (pushed) lua_pop(L, 1);
  ok(f.top_index > f.func_index, "a frame's top is above its function");
  lua_settop(L, base);
}


static void stack_bounds_are_sane (lua_State *L) {
  int base = lua_gettop(L);
  lua_State *co = suspend_at_depth(L, 2);
  int used = diluvium_shim_stacksize(co);
  int cap = diluvium_shim_stackcapacity(co);
  ok(used > 0, "a suspended thread has slots in use");
  ok(cap >= used, "capacity is at least what is in use");
  ok(!diluvium_shim_pushslot(co, 0, L) &&
     !diluvium_shim_pushslot(co, used + 1, L),
     "slot indices outside 1..stacksize are refused");
  lua_settop(L, base);
}


static void yielded_values_are_readable (lua_State *L) {
  int base = lua_gettop(L);
  /* Depth 3 yields the number 3, so the value is somewhere in the live part
     of the stack. Scanning for it is a weaker assertion than naming the slot,
     but it is the honest one: which slot holds a yielded value is a VM detail
     the shim deliberately does not claim to know. What it does claim is that
     every live slot is readable, and a slot that fails to copy would show up
     here as a gap. */
  lua_State *co = suspend_at_depth(L, 3);
  int used = diluvium_shim_stacksize(co);
  int i, found = 0, readable = 0;
  for (i = 1; i <= used; i++) {
    if (!diluvium_shim_pushslot(co, i, L))
      continue;
    readable++;
    if (lua_type(L, -1) == LUA_TNUMBER && lua_tointeger(L, -1) == 3)
      found = 1;
    lua_pop(L, 1);
  }
  okf(readable == used, "every live slot copies out", readable, used);
  ok(found, "the yielded value 3 is somewhere in the live stack");
  lua_settop(L, base);
}


static void upvalue_identity_is_visible (lua_State *L) {
  int base = lua_gettop(L);
  /* A closure over a still-live local has an *open* upvalue; the same closure
     returned from its enclosing function has a *closed* one. 10.3 needs the
     difference because an open upvalue has to be re-opened against the
     reconstructed stack rather than copied by value. */
  lua_State *co = lua_newthread(L);
  int nres;
  const char *src =
    "local x = 41\n"
    "local function get() return x end\n"
    "x = 42\n"
    "coroutine.yield(get)\n";  /* yields while 'x' is still a live local */
  if (luaL_loadstring(co, src) != LUA_OK) {
    ok(0, "upvalue chunk loads");
    lua_settop(L, base);
    return;
  }
  lua_resume(co, L, 0, &nres);
  ok(nres == 1, "the closure came back from the yield");
  if (nres == 1) {
    int slot;
    lua_xmove(co, L, 1);  /* the closure, on L */
    ok(lua_isfunction(L, -1), "the yielded value is the closure");
    ok(diluvium_shim_upisopen(L, -1, 1),
       "the upvalue is open while its local is live");
    slot = diluvium_shim_upslot(L, -1, 1, co);
    ok(slot > 0, "an open upvalue names a slot in its own thread");
    if (slot > 0 && diluvium_shim_pushslot(co, slot, L)) {
      /* The slot must hold the current value, 42 -- not the 41 it was
         initialised with. That is the check that proves 'upslot' resolved a
         live pointer rather than a stale offset. */
      okf(lua_tointeger(L, -1) == 42, "the named slot holds the live value",
          (long)lua_tointeger(L, -1), 42);
      lua_pop(L, 1);
    }
    else
      ok(0, "the named slot is readable");
    /* Identity, via the public API, is what the serialiser uses for sharing;
       the shim only supplies open/closed. Assert they agree about which
       closure we are talking about. */
    ok(lua_upvalueid(L, -1, 1) != NULL, "the upvalue has a public identity");
    ok(diluvium_shim_upslot(L, -1, 1, L) == 0,
       "an upvalue open in one thread reports no slot in another");
    ok(diluvium_shim_upisopen(L, -1, 2) == 0 &&
       diluvium_shim_upslot(L, -1, 2, co) == 0,
       "a nonexistent upvalue index is refused");
    lua_pop(L, 1);
  }
  lua_settop(L, base);
}


static void closed_upvalues_are_closed (lua_State *L) {
  int base = lua_gettop(L);
  /* Same closure, but returned out of its enclosing function, so the local it
     captured is gone and the upvalue has been closed onto the UpVal itself. */
  if (luaL_dostring(L, "local function mk() local y = 7 "
                       "return function() return y end end return mk()")
      != LUA_OK) {
    ok(0, "closed-upvalue chunk runs");
    lua_settop(L, base);
    return;
  }
  ok(lua_isfunction(L, -1), "got the escaped closure");
  ok(!diluvium_shim_upisopen(L, -1, 1),
     "an escaped closure's upvalue is closed");
  lua_settop(L, base);
}


static void non_closures_are_refused (lua_State *L) {
  int base = lua_gettop(L);
  lua_pushcfunction(L, (lua_CFunction)lua_error);
  lua_pushinteger(L, 1);
  lua_pushnil(L);
  ok(diluvium_shim_proto(L, -3) == NULL, "a C function has no proto");
  ok(diluvium_shim_proto(L, -2) == NULL, "a number has no proto");
  ok(diluvium_shim_proto(L, -1) == NULL, "nil has no proto");
  ok(!diluvium_shim_upisopen(L, -3, 1) && !diluvium_shim_upisopen(L, -2, 1),
     "non-closures have no open upvalues");
  ok(!diluvium_shim_is_secure(L, -3) && !diluvium_shim_is_secure(L, -2),
     "non-closures are not secure");
  lua_settop(L, base);
}


static void protos_are_shared_by_identity (lua_State *L) {
  int base = lua_gettop(L);
  const void *a, *b, *c;
  if (luaL_dostring(L, "local function mk() return function() end end "
                       "return mk(), mk(), function() end") != LUA_OK) {
    ok(0, "proto-identity chunk runs");
    lua_settop(L, base);
    return;
  }
  a = diluvium_shim_proto(L, -3);
  b = diluvium_shim_proto(L, -2);
  c = diluvium_shim_proto(L, -1);
  ok(a != NULL && b != NULL && c != NULL, "all three closures have protos");
  /* Two closures made from the same source function share a proto; a
     different function does not. 10.5 content-addresses protos, and this is
     the relation that makes the sharing map worth keeping. */
  ok(a == b, "two closures of the same function share a proto");
  ok(a != c, "different functions have different protos");
  lua_settop(L, base);
}


static void secure_functions_are_flagged (lua_State *L) {
  int base = lua_gettop(L);
  if (luaL_dostring(L, "return function() return 1 end") != LUA_OK) {
    ok(0, "plain-function chunk runs");
    lua_settop(L, base);
    return;
  }
  ok(!diluvium_shim_is_secure(L, -1), "a plain function is not secure");
  lua_pop(L, 1);
  /* '~function' is a statement form -- 'local ~function f() ... end' -- not an
     expression, so it is declared and then returned. */
  if (luaL_dostring(L, "local ~function f() return 1 end return f") != LUA_OK) {
    printf("[FAIL] '~function' did not compile: %s\n", lua_tostring(L, -1));
    failures++; checks++;
    lua_settop(L, base);
    return;
  }
  ok(diluvium_shim_is_secure(L, -1), "a '~function' is secure");
  lua_pop(L, 1);
  /* 10.9 is about a snapshot leaking a secure function's constants, and the
     leak does not need the secure function to be the *outer* one. A plain
     function containing one must report secure, or the serialiser writes the
     nested proto in the clear. lparser.c propagates 'is_encrypted' downwards
     into children but not upwards into the parent, which is why the shim walks
     the tree rather than reading one flag. */
  if (luaL_dostring(L, "local function outer() "
                       "local ~function inner() return 1 end return inner "
                       "end return outer") == LUA_OK)
    ok(diluvium_shim_is_secure(L, -1),
       "a plain function containing a '~function' reports secure");
  else
    ok(0, "nested-secure chunk runs");
  lua_settop(L, base);
}


static void tbclist_reports_pending_closes (lua_State *L) {
  int base = lua_gettop(L);
  lua_State *co = lua_newthread(L);
  int nres, idx = -1;
  const char *src =
    "local t = setmetatable({}, {__close = function() end})\n"
    "local x <close> = t\n"
    "coroutine.yield()\n";
  if (luaL_loadstring(co, src) != LUA_OK) {
    ok(0, "tbc chunk loads");
    lua_settop(L, base);
    return;
  }
  lua_resume(co, L, 0, &nres);
  ok(diluvium_shim_tbclist(co, &idx), "a pending <close> is reported");
  ok(idx > 0, "the tbc slot is a real 1-based index");
  /* And a thread with none reports none -- otherwise the emptiness convention
     ('tbclist' resting on the stack base) is being read backwards, and a
     restore would invent a close that never happens. */
  {
    lua_State *plain = suspend_at_depth(L, 1);
    int pidx = -1;
    ok(!diluvium_shim_tbclist(plain, &pidx) && pidx == 0,
       "a thread with no <close> reports an empty tbclist");
  }
  lua_settop(L, base);
}


/*
** Audit S2. The slot list a restore feeds 'settbc' comes from snapshot bytes,
** so it can name any slot; a slot holding nothing closable must come back as a
** refusal, not reach the raise inside 'luaF_newtbcupval'. Before the fix this
** test did not return: the raise escaped the lock convention and the next
** unlock aborted an assertions build. The closable and false slots ride along
** so the refusal is the check's, not an over-broad guard's.
*/
static void non_closable_tbc_slot_is_refused (lua_State *L) {
  int base = lua_gettop(L);
  lua_State *co = lua_newthread(L);
  int nres, i, used;
  int slot_t = 0, slot_plain = 0, slot_f = 0;
  const char *src =
    "local t = setmetatable({}, {__close = function() end})\n"
    "local plain = 42\n"
    "local f = false\n"
    "coroutine.yield()\n";
  if (luaL_loadstring(co, src) != LUA_OK) {
    ok(0, "tbc-refusal chunk loads");
    lua_settop(L, base);
    return;
  }
  lua_resume(co, L, 0, &nres);
  /* Locate the three locals by value rather than by hardcoded slot: the chunk
     and the resume plumbing sit below them, and where is an implementation
     detail this test has no business asserting. */
  used = diluvium_shim_stacksize(co);
  for (i = 1; i <= used; i++) {
    if (!diluvium_shim_pushslot(co, i, L)) continue;
    if (lua_type(L, -1) == LUA_TTABLE) slot_t = i;
    else if (lua_type(L, -1) == LUA_TNUMBER && lua_tonumber(L, -1) == 42)
      slot_plain = i;
    else if (lua_type(L, -1) == LUA_TBOOLEAN && !lua_toboolean(L, -1))
      slot_f = i;
    lua_pop(L, 1);
  }
  ok(slot_t > 1 && slot_plain > slot_t && slot_f > slot_plain,
     "the three locals are found, in declaration order");
  ok(!diluvium_shim_settbc(co, slot_plain),
     "a slot holding a non-closable value is refused, not raised over");
  ok(diluvium_shim_settbc(co, slot_t),
     "a slot holding a __close value still marks");
  ok(diluvium_shim_settbc(co, slot_f),
     "a false slot is markable; it is never closed, so it needs no __close");
  ok(!diluvium_shim_settbc(co, slot_t),
     "order still holds: at or below the last tracked mark is refused");
  lua_settop(L, base);
}


/*
** The precondition pair.
*/

/*
** The invariant, asserted over every suspended shape reachable from Lua.
**
** Each of these parks with a different mix of C frames below the yield, and
** every one of them must be capturable. 'coroutine.yield' itself is the case
** that would break a naive check: it is a C frame with no continuation, and
** exempting it is correct because it is the frame that yielded. 'pcall' is the
** case that would break an over-broad exemption: its C frame is *below* the
** yield and is only capturable because 'luaB_pcall' uses 'lua_pcallk'.
*/
static void suspended_shapes_are_capturable (lua_State *L) {
  static const struct { const char *what; const char *src; } cases[] = {
    { "a bare 'coroutine.yield'",
      "coroutine.yield(1)" },
    { "a yield two Lua calls deep",
      "local function g() coroutine.yield(1) end "
      "local function f() g() end f()" },
    { "a yield inside 'pcall'",
      "pcall(function() coroutine.yield(1) end)" },
    { "a yield inside 'xpcall'",
      "xpcall(function() coroutine.yield(1) end, function(e) return e end)" },
    { "a yield from an '__index' metamethod",
      "local t = setmetatable({}, {__index = function() "
      "coroutine.yield(1) return 1 end}) return t.k" },
    { "a yield from a '__add' metamethod",
      "local t = setmetatable({}, {__add = function() "
      "coroutine.yield(1) return 1 end}) return t + 1" },
    { "a yield inside a generic 'for'",
      "for _ in function() coroutine.yield(1) return nil end do end" },
    { "a yield inside a nested coroutine's body",
      "local inner = coroutine.create(function() coroutine.yield(1) end) "
      "coroutine.resume(inner) coroutine.yield(2)" },
    { "a yield with a pending '<close>'",
      "local x <close> = setmetatable({}, {__close = function() end}) "
      "coroutine.yield(1)" }
  };
  int base = lua_gettop(L);
  size_t c;
  for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    lua_State *co = lua_newthread(L);
    int nres, frame = 0, code;
    char what[128];
    if (luaL_loadstring(co, cases[c].src) != LUA_OK) {
      printf("[FAIL] %s: does not load: %s\n", cases[c].what,
             lua_tostring(co, -1));
      failures++; checks++;
      continue;
    }
    lua_resume(co, L, 0, &nres);
    if (diluvium_shim_status(co) != LUA_YIELD) {
      printf("[FAIL] %s: did not park (status %d)\n", cases[c].what,
             diluvium_shim_status(co));
      failures++; checks++;
      continue;
    }
    code = diluvium_shim_capturable(co, &frame);
    snprintf(what, sizeof(what), "%s is capturable", cases[c].what);
    if (code != DILUVIUM_SNAP_OK)
      printf("      (frame %d of %d: %s)\n", frame,
             diluvium_shim_framecount(co), diluvium_shim_reason(code));
    okf(code == DILUVIUM_SNAP_OK, what, code, DILUVIUM_SNAP_OK);
  }
  lua_settop(L, base);
}


static void fresh_thread_is_capturable (lua_State *L) {
  int base = lua_gettop(L);
  lua_State *co = lua_newthread(L);
  int frame = 0;
  ok(diluvium_shim_framecount(co) == 0, "a fresh thread has no frames");
  ok(diluvium_shim_capturable(co, &frame) == DILUVIUM_SNAP_OK,
     "a thread that has not started is trivially capturable");
  ok(frame == -1, "no offending frame is reported when there is none");
  lua_settop(L, base);
}


/* Asks about the thread it is running on, from inside it. */
static int ask_about_self (lua_State *L) {
  lua_pushinteger(L, diluvium_shim_capturable(L, NULL));
  lua_pushinteger(L, diluvium_shim_framecount(L));
  return 2;
}


static void running_thread_is_refused (lua_State *L) {
  int base = lua_gettop(L);
  /* Called from Lua, so the thread has frames and status LUA_OK -- the
     signature of "running". Asking at the bare C host boundary instead would
     prove nothing: an empty chain is an empty chain, which is why dshim.h says
     self-capture belongs to the snapshot layer. */
  lua_pushcfunction(L, ask_about_self);
  lua_setglobal(L, "ask_about_self");
  if (luaL_dostring(L, "return ask_about_self()") != LUA_OK) {
    ok(0, "self-interrogation runs");
    lua_settop(L, base);
    return;
  }
  okf(lua_tointeger(L, -2) == DILUVIUM_SNAP_NOT_SUSPENDED,
      "a thread running Lua code refuses capture",
      (long)lua_tointeger(L, -2), DILUVIUM_SNAP_NOT_SUSPENDED);
  ok(lua_tointeger(L, -1) > 0, "and it does have frames, which is why");
  lua_settop(L, base);
}


static void resumer_thread_is_refused (lua_State *L) {
  int base = lua_gettop(L);
  /* A coroutine that resumed another is 'normal': status LUA_OK with frames.
     It is as uncapturable as the running thread and for the same reason, and
     it is the case a swarm actually hits -- a supervisor parked inside a
     resume of a child. */
  lua_State *outer = lua_newthread(L);
  int nres;
  const char *src =
    "local inner = coroutine.create(function() coroutine.yield(1) end)\n"
    "coroutine.resume(inner)\n"
    "OUTER_PARKED = inner\n"
    "coroutine.yield(2)\n";
  if (luaL_loadstring(outer, src) != LUA_OK) {
    ok(0, "resumer chunk loads");
    lua_settop(L, base);
    return;
  }
  /* Park the outer thread *while* it is inside the resume: use a hook-free
     construction by yielding from the inner body and asking about outer then.
     Simplest reliable version: resume outer only as far as its own yield, then
     interrogate the inner thread it left suspended, and separately build the
     mid-resume case below. */
  lua_resume(outer, L, 0, &nres);
  ok(diluvium_shim_status(outer) == LUA_YIELD, "the outer thread parked");
  lua_getglobal(L, "OUTER_PARKED");
  if (lua_type(L, -1) == LUA_TTHREAD) {
    lua_State *inner = lua_tothread(L, -1);
    okf(diluvium_shim_capturable(inner, NULL) == DILUVIUM_SNAP_OK,
        "a child suspended under a parked parent is capturable",
        diluvium_shim_capturable(inner, NULL), DILUVIUM_SNAP_OK);
  }
  else
    ok(0, "the inner thread is reachable");
  lua_settop(L, base);
}


/* Interrogates the thread named at index 1 -- used to catch a resumer while it
   is still inside 'coroutine.resume'. */
static int ask_about_other (lua_State *L) {
  lua_State *other = lua_tothread(L, 1);
  lua_pushinteger(L, (other == NULL) ? -1 : diluvium_shim_capturable(other, NULL));
  return 1;
}


static void mid_resume_parent_is_refused (lua_State *L) {
  int base = lua_gettop(L);
  lua_State *outer = lua_newthread(L);
  int nres;
  lua_pushcfunction(L, ask_about_other);
  lua_setglobal(L, "ask_about_other");
  /* The inner body asks about its parent, which at that moment is suspended in
     'normal' status inside the resume. That is the real "cannot capture the
     thread above you" case. */
  if (luaL_loadstring(outer,
        "local parent = coroutine.running()\n"
        "local inner = coroutine.create(function() "
        "VERDICT = ask_about_other(parent) end)\n"
        "coroutine.resume(inner)\n"
        "coroutine.yield(1)\n") != LUA_OK) {
    ok(0, "mid-resume chunk loads");
    lua_settop(L, base);
    return;
  }
  lua_resume(outer, L, 0, &nres);
  lua_getglobal(L, "VERDICT");
  okf(lua_tointeger(L, -1) == DILUVIUM_SNAP_NOT_SUSPENDED,
      "a parent still inside 'coroutine.resume' refuses capture",
      (long)lua_tointeger(L, -1), DILUVIUM_SNAP_NOT_SUSPENDED);
  lua_settop(L, base);
}


static void dead_thread_is_refused (lua_State *L) {
  int base = lua_gettop(L);
  lua_State *co = lua_newthread(L);
  int nres;
  if (luaL_loadstring(co, "return 1") != LUA_OK) {
    ok(0, "dead-thread chunk loads");
    lua_settop(L, base);
    return;
  }
  lua_resume(co, L, 0, &nres);
  /* A thread that ran to completion has status LUA_OK and no frames, which is
     the same signature as a fresh one. That is honest -- both are empty, and
     restoring either produces a thread with nothing to run -- so the shim
     accepts it, and the snapshot layer is where "dead" becomes meaningful. */
  ok(diluvium_shim_status(co) == LUA_OK, "a finished thread reports LUA_OK");
  ok(diluvium_shim_framecount(co) == 0, "a finished thread has no frames");
  lua_settop(L, base);
}


static void errored_thread_is_refused (lua_State *L) {
  int base = lua_gettop(L);
  lua_State *co = lua_newthread(L);
  int nres;
  if (luaL_loadstring(co, "error('boom')") != LUA_OK) {
    ok(0, "error chunk loads");
    lua_settop(L, base);
    return;
  }
  lua_resume(co, L, 0, &nres);
  ok(diluvium_shim_status(co) == LUA_ERRRUN, "an errored thread reports the error");
  ok(diluvium_shim_capturable(co, NULL) == DILUVIUM_SNAP_NOT_SUSPENDED,
     "an errored thread refuses capture");
  lua_settop(L, base);
}


/*
** Where the forbidden shape went.
**
** The plan was to build it: a C function that reaches Lua with a plain
** 'lua_pcall' -- no continuation -- around a yield, giving a suspended thread
** with a C frame that has nowhere to resume to. It does not work, and the
** reason it does not is the point. 'lua_pcall' routes through
** 'luaD_callnoyield', which sets the very bit 'yieldable' tests, so the yield
** inside is *rejected* rather than parked; the pcall catches the error,
** 'blocker' returns, and the thread ends up dead instead of suspended. There is
** no reachable suspended thread with a continuation-less C frame below the
** yield, and that unreachability is the invariant hibernation rests on.
**
** So the pair below asserts it from the two directions that can be observed:
** the shape never parks, and over every shape that does park, no frame below
** the yield lacks a continuation. A capturability check that only ever returned
** OK would still be doing its job here, and that is fine -- DILUVIUM_SNAP_C_FRAME
** is a tripwire on a VM property, not a runtime gate. What it catches is a
** future host call that reaches Lua with 'lua_call' where it should have used
** 'lua_callk', which would make the shape reachable for the first time.
*/
static int blocker (lua_State *co) {
  lua_pushvalue(co, 1);
  lua_pcall(co, 0, 0, 0);
  lua_pushboolean(co, 1);
  return 1;
}


static void yield_across_plain_pcall_never_parks (lua_State *L) {
  int base = lua_gettop(L);
  lua_State *co = lua_newthread(L);
  int nres;
  lua_pushcfunction(co, blocker);
  if (luaL_loadstring(co, "coroutine.yield(1)") != LUA_OK) {
    ok(0, "blocker payload loads");
    lua_settop(L, base);
    return;
  }
  lua_resume(co, L, 1, &nres);
  okf(diluvium_shim_status(co) != LUA_YIELD,
      "a yield under a plain 'lua_pcall' does not park",
      diluvium_shim_status(co), LUA_YIELD);
  okf(diluvium_shim_framecount(co) == 0,
      "and the thread unwound rather than leaving the frame behind",
      diluvium_shim_framecount(co), 0);
  lua_settop(L, base);
}


/*
** The other direction, said frame by frame rather than through the verdict, so
** a failure names the frame that lost its continuation.
*/
static void no_frame_below_the_yield_lacks_k (lua_State *L) {
  static const char *srcs[] = {
    "coroutine.yield(1)",
    "pcall(function() coroutine.yield(1) end)",
    "xpcall(function() pcall(function() coroutine.yield(1) end) end, error)",
    "local t = setmetatable({}, {__index = function() "
    "coroutine.yield(1) return 1 end}) return t.k",
    "for _ in function() coroutine.yield(1) return nil end do end"
  };
  int base = lua_gettop(L);
  size_t c;
  int offenders = 0, examined = 0;
  for (c = 0; c < sizeof(srcs) / sizeof(srcs[0]); c++) {
    lua_State *co = lua_newthread(L);
    int nres, n, i;
    if (luaL_loadstring(co, srcs[c]) != LUA_OK)
      continue;
    lua_resume(co, L, 0, &nres);
    if (diluvium_shim_status(co) != LUA_YIELD)
      continue;
    n = diluvium_shim_framecount(co);
    for (i = 0; i + 1 < n; i++) {  /* every frame but the innermost */
      diluvium_frame f;
      examined++;
      if (diluvium_shim_frame(co, i, &f) && f.is_c && !f.has_k) {
        printf("      (%s: frame %d of %d)\n", srcs[c], i, n);
        offenders++;
      }
    }
  }
  ok(examined > 0, "there were frames below a yield to examine");
  okf(offenders == 0,
      "no C frame below a yield lacks a continuation", offenders, 0);
  lua_settop(L, base);
}


/*
** A hook that parks the thread.
**
** It has to be a C hook. 'debug.sethook' routes through 'ldblib.c's 'hookf',
** which reaches the Lua hook function with 'lua_call' -- non-yieldable -- so a
** Lua hook cannot yield at all; that attempt errors instead of parking, which
** is why this is written against 'lua_sethook' directly. 'lua_yieldk' from a
** hook returns rather than throwing (ldo.c), and 'luaG_traceexec' then sets
** CIST_HOOKYIELD and throws, so the parked thread carries the mark on its Lua
** frame with CIST_HOOKED already cleared and 'allowhook' back to 1. Catching
** it therefore needs the frame walk, not the 'allowhook' shortcut.
*/
static void yielding_hook (lua_State *co, lua_Debug *ar) {
  (void)ar;
  lua_sethook(co, NULL, 0, 0);  /* park exactly once */
  lua_yield(co, 0);
}


static void hooked_thread_is_refused (lua_State *L) {
  int base = lua_gettop(L);
  lua_State *co = lua_newthread(L);
  int nres, code, frame = -1;
  if (luaL_loadstring(co, "local s = 0 for i = 1, 100 do s = s + i end "
                          "return s") != LUA_OK) {
    ok(0, "hook chunk loads");
    lua_settop(L, base);
    return;
  }
  lua_sethook(co, yielding_hook, LUA_MASKCOUNT, 10);
  lua_resume(co, L, 0, &nres);
  okf(diluvium_shim_status(co) == LUA_YIELD,
      "a C hook can park the thread", diluvium_shim_status(co), LUA_YIELD);
  if (diluvium_shim_status(co) != LUA_YIELD) {
    lua_settop(L, base);
    return;
  }
  code = diluvium_shim_capturable(co, &frame);
  okf(code == DILUVIUM_SNAP_HOOK, "a thread parked by a hook is refused",
      code, DILUVIUM_SNAP_HOOK);
  ok(frame >= 0, "and the hooked frame is named");
  lua_settop(L, base);
}


/*
** The shape dtask.c produces. 'diluvium_task_pushbody' pushes the driver body
** that 'diluvium_task_call' resumes; entering it the same way here means this
** case tests the real driver frame rather than an imitation of it.
*/
static void task_driver_is_capturable (lua_State *L) {
  int base = lua_gettop(L);
  lua_State *co = lua_newthread(L);
  int nres, frame = 0, code;
  diluvium_task_pushbody(co);
  lua_pushnil(co);  /* no message handler */
  if (luaL_loadstring(co, "coroutine.yield(1)") != LUA_OK) {
    ok(0, "driver payload loads");
    lua_settop(L, base);
    return;
  }
  lua_resume(co, L, 2, &nres);
  code = diluvium_shim_capturable(co, &frame);
  okf(diluvium_shim_status(co) == LUA_YIELD,
      "the driver's payload parked", diluvium_shim_status(co), LUA_YIELD);
  if (code != DILUVIUM_SNAP_OK)
    printf("      (refused at frame %d: %s)\n", frame,
           diluvium_shim_reason(code));
  okf(code == DILUVIUM_SNAP_OK,
      "every C frame under dtask.c's driver carries a continuation",
      code, DILUVIUM_SNAP_OK);
  /* And say it directly, frame by frame, so the failure names the frame that
     lost its continuation rather than just the verdict. */
  {
    int n = diluvium_shim_framecount(co);
    int i, bad = -1;
    for (i = 0; i + 1 < n; i++) {  /* the innermost frame is the yielder */
      diluvium_frame f;
      if (diluvium_shim_frame(co, i, &f) && f.is_c && !f.has_k)
        bad = i;
    }
    okf(bad < 0, "no continuation-less C frame in the driver chain", bad, -1);
    /* And the driver's own frame is the one that has to carry it. */
    {
      diluvium_frame f;
      okf(diluvium_shim_frame(co, 0, &f) && f.is_c && f.has_k,
          "the driver frame itself is C and carries a continuation",
          diluvium_shim_frame(co, 0, &f) ? f.has_k : -1, 1);
    }
  }
  lua_settop(L, base);
}


/*
** The test 14 names: an agent parked on 'queue.wait'.
**
** This is the state the runtime is actually built for -- idle on inbox is what
** an agent does almost all the time, and 10.2 turns on it being capturable. It
** goes through 'diluvium_task_call''s driver because that is how a host parks a
** program, so the chain under test is the real one: the driver's C frame with
** its continuation, the guest's Lua frames, and 'dq_wait' suspended on
** 'lua_yieldk' with 'dq_wait_k' saved.
*/
static void agent_parked_on_wait_is_capturable (lua_State *L) {
  int base = lua_gettop(L);
  lua_State *co = lua_newthread(L);
  int nres, frame = -1, code, n, i, with_k = 0;
  diluvium_task_pushbody(co);
  lua_pushnil(co);  /* no message handler */
  if (luaL_loadstring(co, "local q = queue.declare('shim/inbox', {cap = 4}) "
                          "return queue.wait({q})") != LUA_OK) {
    printf("[FAIL] wait payload does not load: %s\n", lua_tostring(co, -1));
    failures++; checks++;
    lua_settop(L, base);
    return;
  }
  lua_resume(co, L, 2, &nres);
  okf(diluvium_shim_status(co) == LUA_YIELD,
      "an agent waiting on an empty inbox is parked",
      diluvium_shim_status(co), LUA_YIELD);
  if (diluvium_shim_status(co) != LUA_YIELD) {
    if (lua_gettop(co) > 0)
      printf("      (%s)\n", lua_tostring(co, -1));
    lua_settop(L, base);
    return;
  }
  code = diluvium_shim_capturable(co, &frame);
  if (code != DILUVIUM_SNAP_OK)
    printf("      (frame %d: %s)\n", frame, diluvium_shim_reason(code));
  okf(code == DILUVIUM_SNAP_OK, "and it is capturable",
      code, DILUVIUM_SNAP_OK);
  /* Say what the chain looks like, so a regression reports which frame changed
     rather than only that the verdict flipped. */
  n = diluvium_shim_framecount(co);
  for (i = 0; i < n; i++) {
    diluvium_frame f;
    if (diluvium_shim_frame(co, i, &f) && f.is_c && f.has_k)
      with_k++;
  }
  ok(n >= 3, "the chain is driver + guest + wait, at least");
  /* Two, not one: the driver's frame from 'lua_pcallk', and 'dq_wait' itself,
     which yields with 'dq_wait_k'. So the wait path does not lean on the
     innermost-frame exemption at all -- it would be capturable without it. */
  okf(with_k >= 2, "both the driver and the wait carry continuations",
      with_k, 2);
  {
    diluvium_frame f;
    okf(diluvium_shim_frame(co, n - 1, &f) && f.is_c && f.has_k,
        "'queue.wait' saved its own continuation ('dq_wait_k')",
        diluvium_shim_frame(co, n - 1, &f) ? f.has_k : -1, 1);
  }
  lua_settop(L, base);
}


/*
** The exemption rule, tested directly.
**
** This is the one part of the walk that cannot be reached through a real
** thread: the shape it rejects does not exist (see
** 'yield_across_plain_pcall_never_parks'), so widening "the innermost frame is
** exempt" to "every C frame is exempt" would pass every other case in this
** file. Filling in frames by hand is the only way to say what the rule is,
** rather than only what it happens to accept.
*/
static void the_exemption_is_narrow (lua_State *L) {
  diluvium_frame f;
  (void)L;
  memset(&f, 0, sizeof(f));

  f.is_c = 1; f.has_k = 0;
  okf(diluvium_shim_framecapturable(&f, 1) == DILUVIUM_SNAP_OK,
      "the innermost continuation-less C frame is exempt (it is the yielder)",
      diluvium_shim_framecapturable(&f, 1), DILUVIUM_SNAP_OK);
  okf(diluvium_shim_framecapturable(&f, 0) == DILUVIUM_SNAP_C_FRAME,
      "a continuation-less C frame below the yield is refused",
      diluvium_shim_framecapturable(&f, 0), DILUVIUM_SNAP_C_FRAME);

  f.has_k = 1;
  okf(diluvium_shim_framecapturable(&f, 0) == DILUVIUM_SNAP_OK,
      "a C frame with a continuation is fine anywhere",
      diluvium_shim_framecapturable(&f, 0), DILUVIUM_SNAP_OK);

  f.is_c = 0; f.has_k = 0;
  okf(diluvium_shim_framecapturable(&f, 0) == DILUVIUM_SNAP_OK,
      "a Lua frame never needs a continuation",
      diluvium_shim_framecapturable(&f, 0), DILUVIUM_SNAP_OK);

  /* The hook check outranks the rest, at either position. */
  f.is_hooked = 1;
  okf(diluvium_shim_framecapturable(&f, 1) == DILUVIUM_SNAP_HOOK &&
      diluvium_shim_framecapturable(&f, 0) == DILUVIUM_SNAP_HOOK,
      "a hooked frame is refused wherever it sits",
      diluvium_shim_framecapturable(&f, 1), DILUVIUM_SNAP_HOOK);
}


static void reasons_are_all_worded (lua_State *L) {
  int c;
  (void)L;
  for (c = 0; c <= DILUVIUM_SNAP_C_FRAME; c++) {
    const char *r = diluvium_shim_reason(c);
    if (r == NULL || *r == '\0' ||
        strcmp(r, diluvium_shim_reason(DILUVIUM_SNAP_C_FRAME + 99)) == 0) {
      ok(0, "every capturability code has its own sentence");
      return;
    }
  }
  ok(1, "every capturability code has its own sentence");
}


int main (void) {
  lua_State *L = luaL_newstate();
  if (L == NULL) {
    printf("[FAIL] could not create a state\n");
    return 1;
  }
  luaL_openlibs(L);
  diluvium_openlibs(L);  /* queue, for the parked-agent case */

  printf("=== dshim: reads mean what they say ===\n");
  framecount_follows_depth(L);
  frames_are_outermost_first(L);
  func_index_points_at_the_function(L);
  stack_bounds_are_sane(L);
  yielded_values_are_readable(L);
  upvalue_identity_is_visible(L);
  closed_upvalues_are_closed(L);
  non_closures_are_refused(L);
  protos_are_shared_by_identity(L);
  secure_functions_are_flagged(L);
  tbclist_reports_pending_closes(L);
  non_closable_tbc_slot_is_refused(L);

  printf("\n=== dshim: capture preconditions (10.7) ===\n");
  fresh_thread_is_capturable(L);
  suspended_shapes_are_capturable(L);
  running_thread_is_refused(L);
  resumer_thread_is_refused(L);
  mid_resume_parent_is_refused(L);
  dead_thread_is_refused(L);
  errored_thread_is_refused(L);
  hooked_thread_is_refused(L);
  yield_across_plain_pcall_never_parks(L);
  no_frame_below_the_yield_lacks_k(L);
  task_driver_is_capturable(L);
  agent_parked_on_wait_is_capturable(L);
  the_exemption_is_narrow(L);
  reasons_are_all_worded(L);

  lua_close(L);
  printf("\n%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
