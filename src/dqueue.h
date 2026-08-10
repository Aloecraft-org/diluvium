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

#endif
