/*
** dmsgpack.h
** Diluvium msgpack codec.
**
** One wire format serves three jobs: values crossing a queue, values
** crossing the host boundary, and (later) the snapshot stream. Having one
** codec rather than three is why it is compiled in rather than left to each
** host binding.
**
** Derived from lua-cmsgpack by Salvatore Sanfilippo (MIT); see the notice in
** dmsgpack.c for what was kept and what was rewritten.
**
** On-top code: public Lua C API only, nothing in the core patch series.
*/

#ifndef dmsgpack_h
#define dmsgpack_h

#include <stddef.h>
#include <stdint.h>

#include "lua.h"


/* Opens the 'msgpack' library. Registered through 'diluvium_openlibs'. */
LUAMOD_API int luaopen_dmsgpack (lua_State *L);


/*
** The codec from C, for callers that are not the guest: queues encode at push
** and decode at pop (6.5), and the instance ABI moves msgpack bytes in and out
** without ever seeing a Lua value. Both raise on failure rather than
** returning a status, so a caller that cannot let an error escape must be
** inside a protected call already.
**
** 'encode' pushes one string; 'decode' pushes one value.
*/
LUA_API void diluvium_msgpack_encode (lua_State *L, int idx);
LUA_API void diluvium_msgpack_decode (lua_State *L, const char *s, size_t len);


/*
** Ext 0x02 is an endpoint reference, and resolving one means asking the
** swarm layer's instance table -- which the codec must not depend on, or the
** layering in doc/Messaging.md 4.1 would be a fiction. So the dependency is
** injected here instead of linked.
**
** 'decode' is handed the raw payload and should push one value, returning 1;
** returning 0 means "not mine", and the codec falls back to surfacing an
** opaque ext value. With no resolver installed at all, every 0x02 decodes
** opaque, which is what lets a single-instance embedder link the codec and
** nothing else.
**
** Encoding is the resolver's business too: an endpoint reference is an opaque
** value the guest never constructs, so the codec cannot know its shape.
** 'encode' is offered each value the codec does not otherwise recognise, and
** should return 1 having appended a complete ext object, or 0 to let the
** codec raise its ordinary "cannot encode" error.
*/
typedef struct diluvium_msgpack_resolver {
  int (*decode) (lua_State *L, const unsigned char *data, size_t len, void *ud);
  int (*encode) (lua_State *L, int idx, void *ud);
  void *ud;
} diluvium_msgpack_resolver;

LUA_API void diluvium_msgpack_setresolver (lua_State *L,
                                    const diluvium_msgpack_resolver *r);

#endif
