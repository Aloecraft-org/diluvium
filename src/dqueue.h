/*
** dqueue.h
** Diluvium queues: named, guest-declared, bounded, dumb.
**
** The model, from doc/Messaging.md 6, restated because each clause is load
** bearing and none of it is the obvious default:
**
**   Declared by the guest, not the host. The host attaches to queues that
**   exist; it never creates one. The host is more likely to be handed a new
**   program than the reverse.
**
**   Volatile. A queue does not outlive its instance except through a snapshot.
**
**   Bounded, always. There is no unbounded option, which is what makes the
**   total queued message count computable and therefore enforceable.
**
**   Dumb. No designated producer or consumer, no acknowledgement, no
**   redelivery. Either side may push and pop. Routing, broadcast, retry and
**   discovery are Diluvium programs, not runtime features (7.4).
**
**   The only guarantee is that push reports whether the message was accepted.
**
** This milestone is local only: no host boundary, no endpoints, and no
** waiting. 'queue.pop' never yields, and 'on_full = "block"' is rejected at
** declare time rather than silently treated as something else -- see the note
** on that in dqueue.c.
**
** On-top code: public Lua C API only, nothing in the core patch series.
*/

#ifndef dqueue_h
#define dqueue_h

#include "lua.h"


/* Opens the 'queue' library. Registered through 'diluvium_openlibs'. */
LUAMOD_API int luaopen_dqueue (lua_State *L);


/*
** The wait-set protocol (8.3).
**
** When a program parks -- on 'queue.wait' for a message, or on a push to a
** queue declared 'on_full = "block"' for space -- it yields a description of
** what it is waiting for. The host reads it, decides, and resumes.
**
** The host owns the clock. There is no timer in the runtime: a timeout is a
** relative duration handed over here, and what to do about it is the host's
** business. That is what makes the same guest code work under a CLI, a tokio
** reactor and a browser event loop without changing.
*/

/* Handles per wait-set. A program waiting on more queues than this is
   describing a routing problem that 7.4 says belongs in a program. */
#define DILUVIUM_WAIT_MAX	32

/* What the parked program is waiting for. */
#define DILUVIUM_WAIT_READ	0  /* a message to arrive */
#define DILUVIUM_WAIT_WRITE	1  /* space to appear */

/* How the host answers. */
#define DILUVIUM_FIRED_READY	0  /* the named handle is ready */
#define DILUVIUM_FIRED_TIMEOUT	1  /* the timeout elapsed */
#define DILUVIUM_FIRED_CLOSED	2  /* the named handle went away */

typedef struct diluvium_waitset {
  int mode;                             /* DILUVIUM_WAIT_* */
  int n;
  lua_Integer ids[DILUVIUM_WAIT_MAX];
  lua_Integer timeout_ms;               /* negative means no timeout */
} diluvium_waitset;


/*
** Read the wait-set out of what a parked thread yielded. 'nres' is the count
** 'lua_resume' reported. Returns 1 when this yield was the queue library's, 0
** when it was something else -- an ordinary 'coroutine.yield', say, which a
** host has no business interpreting as a wait.
*/
LUA_API int diluvium_queue_waitset (lua_State *co, int nres,
                                    diluvium_waitset *ws);

/*
** Which handle in the set can fire right now, or 0 if none can. Also reports
** whether the reason is that a queue has gone away rather than become ready,
** since 6.4 distinguishes those.
*/
LUA_API lua_Integer diluvium_queue_ready (lua_State *co,
                                          const diluvium_waitset *ws,
                                          int *why);

/*
** Push the answer for 'lua_resume' to deliver: exactly two values, so a host
** binding never has to know the shape. 'id' is ignored for a timeout.
*/
LUA_API void diluvium_queue_fire (lua_State *co, lua_Integer id, int why);

#endif
