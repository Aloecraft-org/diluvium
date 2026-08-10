/*
** dendpoint.h
** Endpoints: queue handles whose far end somebody else owns.
**
** doc/Messaging.md 7 is the shortest part of the design and the most important,
** so the reasoning is repeated here rather than referred to.
**
** A destination may be resident in this process, hibernated to a cache, in
** another worker, on another machine, or gone. If the queue layer tried to know
** the difference it would grow a router, a discovery mechanism, a retry policy
** and a timeout policy -- all in C, all in the binary, all wrong for somebody.
** So it does not try. The guarantee is:
**
**   push reports whether the message was accepted into the NEXT HOP. Nothing
**   more. No delivery confirmation, no ordering across endpoints, no retry.
**
** An endpoint is therefore an ordinary bounded queue with one extra property:
** its far end can close, and when it has, a push answers "gone" rather than
** waiting or failing. Accepting is O(1) and never depends on the destination
** being reachable (7.5) -- a broadcaster fanning out to two hundred agents
** cannot be stalled by one slow or absent peer.
**
** Liveness is a flag the host maintains, not a callback the runtime makes. The
** host is the only thing that knows when a far end died, and putting a call in
** the push path would make the cheapest operation in the system depend on
** whatever the host does in that callback.
**
** Routing, broadcast, retry, discovery and store-and-forward are Diluvium
** programs (7.4). Nothing in this file helps with any of them, on purpose: that
** is what makes them replaceable at runtime instead of frozen into C.
**
** On-top code: public Lua C API only, nothing in the core patch series.
*/

#ifndef dendpoint_h
#define dendpoint_h

#include "lua.h"


/* Opens the 'endpoint' library. Registered through 'diluvium_openlibs'. */
LUAMOD_API int luaopen_dendpoint (lua_State *L);


/*
** How a host answers a guest that is binding a reference.
**
** The guest never constructs a reference (7.3): it receives one in a message, as
** msgpack ext 0x02, and hands it back here. What the bytes mean is entirely the
** host's business -- an index into an instance table, a socket address, a name to
** look up later. The runtime carries them and does not read them.
**
** Return 1 having set '*token' to whatever the host wants to identify this
** endpoint by, or 0 to refuse the bind. A refused bind is an error to the guest,
** because it asked for something specific and did not get it -- unlike a push to
** a bound-but-dead endpoint, which is the ordinary "gone".
*/
typedef int (*diluvium_endpoint_bind) (const unsigned char *ref, size_t len,
                                       unsigned int *token, void *ud);

LUA_API void diluvium_endpoint_sethandler (lua_State *L,
                                           diluvium_endpoint_bind fn, void *ud);

/*
** Pre-authorise a reference, without a callback.
**
** A host that already knows what its references mean can say so up front, and
** then binding needs no call out at all. That is the common case -- a host hands
** out references it minted -- and it is the *only* workable case for a host
** reaching the ABI through WebAssembly, where a C function pointer is an index
** into the module's function table and installing one from outside means
** exporting a mutable table and reserving a slot.
**
** Registered references are consulted before the callback, so a host may use
** either or both.
*/
LUA_API void diluvium_endpoint_allow (lua_State *L, const char *ref, size_t len,
                                      unsigned int token);

/*
** The queue handle a token was bound to, or 0. This is how a host finds the
** buffer to drain: an endpoint is a real local queue, so 'queue pop' on this
** handle is how messages leave the instance.
*/
LUA_API lua_Integer diluvium_endpoint_queue (lua_State *L, unsigned int token);

/*
** Mark a bound endpoint live or gone. Once gone it stays gone: an endpoint names
** a particular far end, and a new one at the same address is a new endpoint.
** Returns 1 if the handle was an endpoint, 0 otherwise.
*/
LUA_API int diluvium_endpoint_setlive (lua_State *L, lua_Integer id, int live);

#endif
