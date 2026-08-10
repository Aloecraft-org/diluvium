/*
** dtask.c
** Diluvium coroutine-hosted execution. See dtask.h for why this is a new
** entry path rather than a conversion of the existing ones.
*/

#define dtask_c

#include "lprefix.h"

#include <stddef.h>

#include "lua.h"

#include "lauxlib.h"
#include "dtask.h"


static diluvium_task_hook task_hook = NULL;
static void *task_hook_ud = NULL;


LUA_API void diluvium_task_sethook (diluvium_task_hook hook, void *ud) {
  task_hook = hook;
  task_hook_ud = ud;
}


static void notify (lua_State *co) {
  if (task_hook != NULL)
    (*task_hook)(co, task_hook_ud);
}


/*
** Finish the protected call, on every path out of it.
**
** All of the completion logic has to be here rather than after the
** 'lua_pcallk' below, and getting this wrong is the single worst mistake
** available in this file, because it produces a build that looks green.
**
** A continuation runs on error as well as on yield. When the call raises,
** 'precover' finds this frame (it carries CIST_YPCALL, set by 'lua_pcallk')
** and unrolls into it, which calls 'finishpcallk' and then this function
** with the error status -- and 'lua_pcallk' itself never returns that
** status, because its continuation branch assigns LUA_OK unconditionally
** and the raise long-jumps clean out of the C frame. So the code after the
** call is unreachable on the error path. A continuation that simply
** returned its results would turn every error into a successful resume,
** with exit status 0 and no message.
**
** Re-raising is what gets the error back out: the handler has already run
** at the throw point, 'finishpcallk' has restored the previous one so it
** cannot run twice, and the message is on top. 'lua_error' from here finds
** no further recovery point, so 'lua_resume' returns the real status with
** the message on the thread.
*/
static int taskcont (lua_State *co, int status, lua_KContext ctx) {
  (void)ctx;  /* not UNUSED: that lives in llimits.h, a core header */
  if (status != LUA_OK && status != LUA_YIELD)
    return lua_error(co);
  /* Slot 1 holds the message handler (or nil); LUA_MULTRET replaced the
     function and its arguments with the results, so everything above it
     is a result. */
  return lua_gettop(co) - 1;
}


/*
** The thread's body. This C frame is not incidental: 'lua_pcallk' only
** takes its continuation branch when a continuation is given AND the state
** is already yieldable, and a thread that has never been resumed is not.
** Called directly on a fresh thread it would fall through to the
** conventional protected call, whose 'f_call' uses 'luaD_callnoyield' --
** which sets the very bit 'yieldable' tests, making everything beneath it
** non-yieldable and quietly defeating the point of running here at all.
** The pcall has to happen inside a function the resume is running.
*/
static int taskbody (lua_State *co) {
  int nargs = lua_gettop(co) - 2;  /* [1] handler, [2] function, [3..] args */
  int ef = lua_isfunction(co, 1) ? 1 : 0;
  lua_pcallk(co, nargs, LUA_MULTRET, ef, 0, taskcont);
  return taskcont(co, LUA_OK, 0);
}


/*
** Make the caller's stack hold exactly 'nres' of the values now on it,
** padding with nil or dropping the excess, the way 'lua_pcall' leaves it.
*/
static void adjust (lua_State *L, int got, int nres) {
  if (nres == LUA_MULTRET)
    return;
  for (; got < nres; got++)
    lua_pushnil(L);
  if (got > nres)
    lua_pop(L, got - nres);
}


LUA_API int diluvium_task_call (lua_State *L, int nargs, int nres,
                                int errfunc) {
  lua_State *co;
  int ref, status, got, result;
  int handler = (errfunc != 0) ? lua_absindex(L, errfunc) : 0;
  /* A fresh thread per call. Reusing one is not an optimisation worth
     having: a thread that has raised is dead and cannot be resumed again,
     so a cache would have to detect that anyway, and the allocation is
     about a kilobyte against whatever the call itself does. */
  co = lua_newthread(L);
  /* Anchor it in the registry rather than leaving it on the caller's stack.
     Callers do arithmetic against 'lua_gettop' and some assert on it, so an
     extra slot is not free -- and the thread must be reachable for the
     collector while it runs, since a GC step triggered inside the body
     would otherwise be free to collect the thread it is running on. */
  ref = luaL_ref(L, LUA_REGISTRYINDEX);  /* pops the thread */
  /* Room for the body, the handler slot, the function and its arguments.
     A fresh thread starts with far fewer free slots than a script may have
     arguments, and 'lua_xmove' does not grow the destination: past the
     limit it writes beyond the stack, which 'api_check' catches only in a
     debug build. */
  if (!lua_checkstack(co, nargs + 3)) {
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    lua_pop(L, nargs + 1);  /* the function and its arguments */
    lua_pushliteral(L, "stack overflow preparing a task");
    return DILUVIUM_TASK_ERROR;
  }
  lua_pushcfunction(co, taskbody);
  if (handler != 0) {
    lua_pushvalue(L, handler);
    lua_xmove(L, co, 1);
  }
  else
    lua_pushnil(co);
  lua_xmove(L, co, nargs + 1);  /* the function and its arguments */

  notify(co);
  status = lua_resume(co, L, nargs + 2, &got);
  notify(NULL);

  if (status == LUA_OK) {
    if (!lua_checkstack(L, got + 1)) {
      luaL_unref(L, LUA_REGISTRYINDEX, ref);
      lua_pushliteral(L, "stack overflow returning from a task");
      return DILUVIUM_TASK_ERROR;
    }
    lua_xmove(co, L, got);
    adjust(L, got, nres);
    result = DILUVIUM_TASK_OK;
  }
  else if (status == LUA_YIELD) {
    /* Unreachable until something parks. Reported rather than ignored so
       that an untaught caller fails loudly instead of mistaking a suspended
       thread for a finished one; see dtask.h. */
    lua_settop(co, 0);
    lua_pushliteral(L, "task yielded, but this caller cannot park it");
    result = DILUVIUM_TASK_YIELD;
  }
  else {
    lua_checkstack(L, 1);
    lua_xmove(co, L, 1);  /* the error message, already handled on 'co' */
    result = DILUVIUM_TASK_ERROR;
  }
  luaL_unref(L, LUA_REGISTRYINDEX, ref);
  return result;
}
