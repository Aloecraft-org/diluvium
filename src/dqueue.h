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


/*
** Byte-level access, for the instance ABI.
**
** A host already holds msgpack, so moving a message through the codec would
** decode and re-encode it for nothing -- and 11.1 says bytes in, bytes out.
** These are the paths that keep that promise literal.
**
** Nothing here validates the bytes. That is deliberate and worth knowing: a
** host that pushes malformed msgpack will have the guest raise when it pops,
** and validating would mean a full decode on every message to catch a case
** only a broken host can create. The alternative -- decode to check, discard,
** then store the bytes -- costs the thing the ABI exists to avoid.
*/
#define DILUVIUM_Q_OK		0
#define DILUVIUM_Q_FULL		1
#define DILUVIUM_Q_DISABLED	2
#define DILUVIUM_Q_EMPTY	3
#define DILUVIUM_Q_DROPPED	4
#define DILUVIUM_Q_UNKNOWN	5
#define DILUVIUM_Q_GONE		6

/* Name to handle, or 0 when there is no such queue. */
LUA_API lua_Integer diluvium_queue_find (lua_State *L, const char *name);

/*
** Declare a queue from C, for the endpoint library.
**
** 6.1 says queues are declared by the guest and never by the host, and that
** still holds: this is reached only through 'endpoint.bind', which the *guest*
** calls. What the host supplies is the answer to "where does this reference
** point", never the decision to create a queue.
**
** 'on_full' is a DQ policy index, 'endpoint' marks the queue as having a far end
** somebody else owns. Returns the handle, or 0 if the name is taken.
*/
LUA_API lua_Integer diluvium_queue_declare (lua_State *L, const char *name,
                                            lua_Integer capacity, int on_full,
                                            int exported, int endpoint);

/* Is this handle an endpoint, and is its far end still there? */
LUA_API int diluvium_queue_is_endpoint (lua_State *L, lua_Integer id);
LUA_API int diluvium_queue_is_gone (lua_State *L, lua_Integer id);
LUA_API int diluvium_queue_set_gone (lua_State *L, lua_Integer id, int gone);

/* One of the DILUVIUM_Q_* codes above. */
LUA_API int diluvium_queue_push_bytes (lua_State *L, lua_Integer id,
                                       const char *s, size_t len);

/*
** Borrow the message at the head without removing it. The bytes stay valid
** until 'diluvium_queue_drop' or the next 'diluvium_queue_peek_bytes' on this
** state, because the peeked string is anchored in the registry -- not merely
** because the queue still holds it. That distinction matters: without the
** anchor, a guest that ran in between and popped the message would leave the
** host holding a pointer into collectable memory.
*/
LUA_API int diluvium_queue_peek_bytes (lua_State *L, lua_Integer id,
                                       const char **s, size_t *len);

/* Remove the head, which is what a peek/drop pair together amount to. */
LUA_API int diluvium_queue_drop (lua_State *L, lua_Integer id);

/*
** Everything a host might want to know about a queue. Out params rather than a
** struct so this header stays independent of the ABI's. Returns 1 for a live
** handle, 0 otherwise; any out pointer may be NULL.
*/
LUA_API int diluvium_queue_stat (lua_State *L, lua_Integer id,
                                 lua_Integer *capacity, lua_Integer *len,
                                 int *enabled, int *exported,
                                 int *direction, int *on_full);

/*
** Called when the guest pushes to an exported queue, so a host need not poll.
**
** Only exported queues, and only guest pushes: a host that pushed a message
** does not need telling about it. Fires synchronously, inside whatever call the
** guest was making, so a callback that re-enters the runtime is a re-entrancy
** bug waiting to happen -- the ABI's wrapper says so in its own words.
**
** The callback and its user data are held in a userdata in the registry, so
** there is no allocation to free and no function pointer squeezed through a
** 'void *'.
*/
typedef void (*diluvium_queue_notify) (lua_State *L, lua_Integer id, void *ud);

LUA_API void diluvium_queue_setnotify (lua_State *L, diluvium_queue_notify fn,
                                       void *ud);

#endif
