/*
** dtask_check.c
** Contract tests for 'diluvium_task_call'.
**
** These are in C rather than in a .lua file for a plain reason: the driver
** has no Lua binding, on purpose. Its callers are the host ABI and, later,
** an opt-in task mode on the CLI -- not guest code. Until one of those
** exists there is nothing for the .lua suite to call, and inventing a
** global to make it testable would add a guest-visible surface for the sake
** of a test.
**
** Every case here fails if its mitigation is removed from dtask.c; that was
** verified by removing each one in turn, not assumed. The three that matter:
**
**   'errors_propagate' fails (reports OK, exit status 0, no message) if
**   'taskcont' returns its results instead of re-raising on an error
**   status. That is the failure mode that produces a green build.
**
**   'top_level_is_yieldable' fails if the pcall inside 'taskbody' is a
**   plain 'lua_pcall', or if the body is called directly rather than being
**   entered by 'lua_resume'. It is the only case that proves the driver
**   achieved anything at all.
**
**   'many_arguments' aborts under the debug build's 'api_check' if
**   'lua_checkstack' is dropped before the inbound 'lua_xmove'.
*/

#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
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


/* The message handler 'lua.c' uses, near enough: a traceback at the throw
   point. What matters is that it runs on the thread, where the error is
   raised, so the traceback describes the failing frames rather than the
   driver. */
static int msghandler (lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  if (msg == NULL)
    msg = "(non-string error)";
  luaL_traceback(L, L, msg, 1);
  return 1;
}


static int load_chunk (lua_State *L, const char *src) {
  return luaL_loadstring(L, src) == LUA_OK;
}


static void errors_propagate (lua_State *L) {
  int base, status;
  const char *msg;
  lua_pushcfunction(L, msghandler);
  base = lua_gettop(L);
  if (!load_chunk(L, "local function inner() error('boom', 0) end inner()")) {
    ok(0, "chunk compiles");
    return;
  }
  status = diluvium_task_call(L, 0, LUA_MULTRET, base);
  ok(status == DILUVIUM_TASK_ERROR, "an erroring chunk reports ERROR");
  msg = lua_tostring(L, -1);
  ok(msg != NULL && strstr(msg, "boom") != NULL,
     "the error message survives the state hop");
  ok(msg != NULL && strstr(msg, "stack traceback") != NULL,
     "and the handler ran, on the thread, so there is a traceback");
  ok(msg != NULL && strstr(msg, "inner") != NULL,
     "the traceback names the failing frame, not the driver");
  lua_settop(L, 0);
}


static void top_level_is_yieldable (lua_State *L) {
  int status;
  if (!load_chunk(L, "return coroutine.isyieldable(), (select(2, coroutine.running()))")) {
    ok(0, "chunk compiles");
    return;
  }
  status = diluvium_task_call(L, 0, 2, 0);
  ok(status == DILUVIUM_TASK_OK, "the yieldability probe runs");
  ok(lua_toboolean(L, -2) != 0,
     "coroutine.isyieldable() is TRUE inside a task");
  ok(lua_toboolean(L, -1) == 0,
     "and the task is not the main thread");
  lua_settop(L, 0);
}


static void main_thread_is_unchanged (lua_State *L) {
  /* The other half of the conformance story: entering a task must not make
     the caller's own state yieldable, or the CLI would drift anyway. */
  ok(lua_isyieldable(L) == 0, "the calling state stays non-yieldable");
}


static void results_come_back (lua_State *L) {
  int status;
  if (!load_chunk(L, "return 1, 2, 3")) { ok(0, "chunk compiles"); return; }
  status = diluvium_task_call(L, 0, LUA_MULTRET, 0);
  ok(status == DILUVIUM_TASK_OK && lua_gettop(L) == 3,
     "LUA_MULTRET brings every result back");
  ok(lua_tointeger(L, 1) == 1 && lua_tointeger(L, 3) == 3,
     "in order");
  lua_settop(L, 0);

  if (!load_chunk(L, "return 1, 2, 3")) { ok(0, "chunk compiles"); return; }
  status = diluvium_task_call(L, 0, 1, 0);
  ok(status == DILUVIUM_TASK_OK && lua_gettop(L) == 1,
     "a fixed result count is honoured");
  lua_settop(L, 0);

  if (!load_chunk(L, "return 1")) { ok(0, "chunk compiles"); return; }
  status = diluvium_task_call(L, 0, 3, 0);
  ok(status == DILUVIUM_TASK_OK && lua_gettop(L) == 3 && lua_isnil(L, 3),
     "and padded with nil when the chunk returns fewer");
  lua_settop(L, 0);
}


static void many_arguments (lua_State *L) {
  /* A fresh thread has around 20 free slots. Anything past that reaches
     beyond the stack unless the driver grows it first, and in a release
     build 'api_check' is compiled out, so this is a memory-safety case
     rather than a tidiness one. 64 is comfortably past the edge; a script
     invoked with that many command-line arguments is not exotic. */
  int i, status;
  if (!load_chunk(L, "return select('#', ...)")) {
    ok(0, "chunk compiles");
    return;
  }
  ok(lua_checkstack(L, 70) != 0, "the caller can make room for 64 arguments");
  for (i = 0; i < 64; i++)
    lua_pushinteger(L, i);
  status = diluvium_task_call(L, 64, 1, 0);
  ok(status == DILUVIUM_TASK_OK && lua_tointeger(L, -1) == 64,
     "64 arguments cross to the thread intact");
  lua_settop(L, 0);
}


static void many_results (lua_State *L) {
  int status;
  if (!load_chunk(L, "return table.unpack({}, 1, 64)")) {
    ok(0, "chunk compiles");
    return;
  }
  status = diluvium_task_call(L, 0, LUA_MULTRET, 0);
  ok(status == DILUVIUM_TASK_OK && lua_gettop(L) == 64,
     "64 results cross back intact");
  lua_settop(L, 0);
}


static void caller_stack_is_untouched (lua_State *L) {
  /* 'lua_pcall's contract, which is what lets a caller be switched over
     without rearranging anything: the function and its arguments are gone,
     and nothing else moved. The thread itself must not be left behind. */
  int before;
  lua_pushliteral(L, "sentinel");
  before = lua_gettop(L);
  if (!load_chunk(L, "return 'x'")) { ok(0, "chunk compiles"); return; }
  lua_pushinteger(L, 7);
  diluvium_task_call(L, 1, 1, 0);
  ok(lua_gettop(L) == before + 1, "exactly one result replaced the call");
  ok(lua_type(L, before) == LUA_TSTRING &&
     strcmp(lua_tostring(L, before), "sentinel") == 0,
     "and what was under it is undisturbed");
  lua_settop(L, 0);
}


static void a_yield_is_loud (lua_State *L) {
  /* Nothing parks yet, so this is the one case that exercises the branch
     which exists to stop a suspended thread being mistaken for a finished
     one. A guest can reach it today only by yielding on purpose. */
  int status;
  if (!load_chunk(L, "coroutine.yield() return 'unreachable'")) {
    ok(0, "chunk compiles");
    return;
  }
  status = diluvium_task_call(L, 0, LUA_MULTRET, 0);
  ok(status == DILUVIUM_TASK_YIELD,
     "a top-level yield reports YIELD rather than OK");
  ok(lua_type(L, -1) == LUA_TSTRING,
     "with a message saying the caller cannot park it");
  lua_settop(L, 0);
}


static void nested_tasks (lua_State *L) {
  /* Errors have to survive two hops, not one. */
  int base, status;
  const char *msg;
  lua_pushcfunction(L, msghandler);
  base = lua_gettop(L);
  if (!load_chunk(L, "return (coroutine.wrap(function()"
                     "  return coroutine.isyieldable() end)())")) {
    ok(0, "chunk compiles");
    return;
  }
  status = diluvium_task_call(L, 0, 1, base);
  ok(status == DILUVIUM_TASK_OK && lua_toboolean(L, -1) != 0,
     "a coroutine inside a task still works");
  lua_settop(L, 0);

  lua_pushcfunction(L, msghandler);
  base = lua_gettop(L);
  if (!load_chunk(L, "error('deep', 0)")) { ok(0, "chunk compiles"); return; }
  status = diluvium_task_call(L, 0, LUA_MULTRET, base);
  msg = lua_tostring(L, -1);
  ok(status == DILUVIUM_TASK_ERROR && msg != NULL &&
     strstr(msg, "deep") != NULL,
     "and a raise from the outer chunk still reports");
  lua_settop(L, 0);
}


static void no_handler_is_fine (lua_State *L) {
  int status;
  const char *msg;
  if (!load_chunk(L, "error('bare', 0)")) { ok(0, "chunk compiles"); return; }
  status = diluvium_task_call(L, 0, LUA_MULTRET, 0);
  msg = lua_tostring(L, -1);
  ok(status == DILUVIUM_TASK_ERROR && msg != NULL &&
     strcmp(msg, "bare") == 0,
     "errfunc 0 gives the raw message, with no traceback appended");
  lua_settop(L, 0);
}


static lua_State *hook_saw = NULL;
static int hook_calls = 0;

static void record_hook (lua_State *co, void *ud) {
  (void)ud;
  hook_calls++;
  if (co != NULL)
    hook_saw = co;
}

static void hook_reports_the_thread (lua_State *L) {
  /* The SIGINT mitigation point. Nothing uses it yet, so this asserts only
     that it fires with the running thread and then clears -- enough that a
     caller wiring 'globalL' to it has something that works. */
  int status;
  diluvium_task_sethook(record_hook, NULL);
  hook_saw = NULL;
  hook_calls = 0;
  if (!load_chunk(L, "return 1")) { ok(0, "chunk compiles"); return; }
  status = diluvium_task_call(L, 0, 1, 0);
  ok(status == DILUVIUM_TASK_OK, "the hooked call runs");
  ok(hook_calls == 2, "the hook fires on entry and on exit");
  ok(hook_saw != NULL && hook_saw != L,
     "and it names the thread, not the calling state");
  diluvium_task_sethook(NULL, NULL);
  lua_settop(L, 0);
}


int main (void) {
  lua_State *L = luaL_newstate();
  if (L == NULL) {
    printf("[FAIL] could not create a state\n");
    return 1;
  }
  luaL_openlibs(L);

  printf("=== dtask contract ===\n");
  top_level_is_yieldable(L);
  main_thread_is_unchanged(L);
  errors_propagate(L);
  no_handler_is_fine(L);
  results_come_back(L);
  many_arguments(L);
  many_results(L);
  caller_stack_is_untouched(L);
  a_yield_is_loud(L);
  nested_tasks(L);
  hook_reports_the_thread(L);

  lua_close(L);
  printf("\n%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
