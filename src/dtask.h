/*
** dtask.h
** Diluvium coroutine-hosted execution.
**
** Waiting has to be a yield rather than a block: it is the only form that
** works on a browser main thread and the only form a snapshot can capture,
** since a blocked C frame's state lives on the machine stack. Lua will not
** let the main thread yield, so anything that may wait has to run on a
** thread entered by 'lua_resume'.
**
** This is that entry point, and it is deliberately a NEW path rather than a
** conversion of the existing ones. Running ordinary script and REPL chunks
** on a coroutine is observable from Lua -- 'coroutine.running' stops
** reporting the main thread and 'coroutine.isyieldable' starts returning
** true at the top level -- and the upstream suite asserts the opposite in
** four places (test/coroutine.lua:10-14 and :159-163, test/errors.lua:399,
** test/locals.lua:1177). Those are Lua conformance tests, and a library
** branching on 'coroutine.isyieldable()' to decide whether to yield would
** change behaviour in ordinary scripts. So 'lua.c' keeps stock semantics
** and this door is separate: the CLI is the conformance surface, and the
** host ABI is where coroutine-hosted tasks run.
**
** This is on-top code: nothing here is in the core patch series, and it
** uses only the public C API.
*/

#ifndef dtask_h
#define dtask_h

#include "lua.h"


/* Results of 'diluvium_task_call'. */
#define DILUVIUM_TASK_OK      0  /* ran to completion; results on the stack */
#define DILUVIUM_TASK_ERROR   1  /* raised; the message is on the stack */
#define DILUVIUM_TASK_YIELD   2  /* parked; see the note below */


/*
** Run a function on a fresh thread and bring the outcome back.
**
** The stack contract is 'lua_pcall's, so a caller can be switched over
** without rearranging anything: push the function, then 'nargs' arguments.
** On return those are gone, and either 'nres' results are on the stack
** (all of them if 'nres' is LUA_MULTRET) or a single error message is.
** 'errfunc' is the stack index of a message handler, or 0 for none; it is
** installed on the thread, where the error is actually raised, so a
** traceback is built at the throw point exactly as 'lua_pcall' would.
**
** DILUVIUM_TASK_YIELD cannot happen yet, because nothing in the runtime
** parks. It is returned rather than asserted so that the day 'queue.wait'
** lands, a caller that has not been taught to park fails loudly instead of
** treating a suspended thread as a finished one.
*/
LUA_API int diluvium_task_call (lua_State *L, int nargs, int nres,
                                int errfunc);


/*
** Called around the resume with the thread that is about to run, and again
** with NULL when it stops.
**
** This exists for one specific hazard. 'lua.c' keeps a static 'globalL' and
** its SIGINT handler installs a debug hook on it; if the code is running on
** a thread, the hook lands on the main state and Ctrl-C silently stops
** interrupting anything. Nothing in the suite presses Ctrl-C, so that
** regression would ship green. No caller needs this yet -- the CLI is not
** converted -- so the mitigation is present and UNTESTED, which is worth
** knowing before relying on it.
**
** Process-wide, not per-state. The ABI is one-instance-one-thread and this
** is an embedder setting, not a guest-visible one.
*/
typedef void (*diluvium_task_hook) (lua_State *co, void *ud);

LUA_API void diluvium_task_sethook (diluvium_task_hook hook, void *ud);


/*
** How the host waits.
**
** 8.3 puts the clock on the host side, so the driver never sleeps: when a
** program parks with a timeout, this is asked to let that much time pass.
** 'ms' is a relative duration in milliseconds, or negative for "no timeout,
** wait until something happens".
**
** Return 1 when something may have changed and the wait-set should be
** re-examined, 0 when the timeout elapsed with nothing arriving, and -1 when
** the host knows the wait can never be satisfied -- a single-threaded CLI
** waiting on local queues nothing else can write to, for instance. A -1 turns
** into an error naming the deadlock, which is the honest answer and much
** better than hanging.
**
** With no wait function installed, an indefinite park is reported as a
** deadlock and a finite one times out immediately without any time passing.
*/
typedef int (*diluvium_task_wait) (lua_Integer ms, void *ud);

LUA_API void diluvium_task_setwait (diluvium_task_wait fn, void *ud);

#endif
