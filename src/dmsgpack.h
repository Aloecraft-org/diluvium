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
** As 'decode', but also reports how many bytes the value occupied, so a caller
** reading a value followed by something else knows where the something else
** begins. 'msgpack.decode' has offered this to the guest since 5.4; the C
** entry point did not, and a snapshot header followed by a payload is exactly
** the case that needs it. Trailing bytes are not an error here, which is the
** difference from 'decode'.
*/
LUA_API void diluvium_msgpack_decode_n (lua_State *L, const char *s, size_t len,
                                        size_t *used);


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


/*
** Snapshot mode.
**
** A snapshot is the same wire format with three differences the plain codec
** must not have, which is why this is a separate entry point rather than a
** flag on 'encode':
**
**   Identity. Every table gets a position in the stream, and a table met twice
**   is written as an ext 0x04 backreference to its position. So sharing and
**   cycles survive, and 'msgpack.encode' keeps refusing a cycle -- which is
**   the right answer there, because a queue message with a cycle has no
**   agreed meaning for whoever receives it.
**
**   Metatables. Preserved, in a trailing section (see below).
**
**   Reach. Functions, userdata and threads are encodable, through the hooks.
**
** The graph is written as: the root value, then zero or more *fixups*, then a
** nil. Each fixup is a tag followed by its operands, all as consecutive
** top-level msgpack values:
**
**   DILUVIUM_FIX_META    position, metatable
**   DILUVIUM_FIX_UPVAL   position, index, value
**   DILUVIUM_FIX_UPJOIN  position, index, source position, source index
**
** A trailing section rather than inline, because neither a metatable nor an
** upvalue is part of its owner's contents, and the decoder cannot know one
** follows until after it has created the owner. A nil terminator rather than a
** length prefix because the list grows while it is being written -- a metatable
** may have a metatable, and an upvalue may be a table with a metatable -- so its
** length is unknown when a header would have to be emitted, and buffering the
** section to find out would copy the largest part of the graph once per level.
**
** Bare consecutive values rather than one array per fixup, because an array is a
** *table* on the wire and the decoder gives every table it creates a position. A
** wrapper would take a position the encoder never assigned, and everything after
** the first fixup would be off by one. That was found by a failing test, not by
** design.
**
** UPJOIN is what makes 10.3's upvalue identity survive: two closures sharing a
** variable are written as one UPVAL and one UPJOIN, and the decoder replays it
** with 'lua_upvaluejoin' so a write through one is still seen by the other.
** UPJOIN always follows the UPVAL it refers to, because the encoder claims an
** upvalue for the first closure that reaches it and drains closures in order.
**
** Both directions raise on failure. Encode leaves one string; decode leaves one
** value.
*/
/* Fixup tags. See the section comment above. */
#define DILUVIUM_FIX_META	1
#define DILUVIUM_FIX_UPVAL	2
#define DILUVIUM_FIX_UPJOIN	3

typedef struct diluvium_snap_hooks {
  /*
  ** Asked *first*, before the codec gives a value a position, for every table,
  ** function, userdata and thread.
  **
  ** This is where permanents live (10.4): a C function, '_G', or a standard
  ** library table is the same object in both processes and must be named rather
  ** than copied. Return 1 having appended one complete ext 0x07; the codec then
  ** treats the value as written and, crucially, gives it *no position*, because
  ** a named object is not part of the object graph. Return 0 and the codec
  ** carries on with its own handling.
  **
  ** Tables go through here too, and they have to: '_G' is a table, and an
  ** encoder that interned it would drag the entire global environment -- every C
  ** function in it included -- into the snapshot.
  */
  int (*permanent) (lua_State *L, int idx, void *ud);
  /*
  ** Offered each value the codec cannot write itself -- function, userdata,
  ** thread -- after the codec has given it a position, so a backreference to it
  ** works and its own contents may refer back to it.
  **
  ** Return 0 to let the codec raise its ordinary error naming the type and the
  ** key path. Return 1 having appended one complete msgpack object. Return 2 for
  ** a Lua closure whose upvalues the codec should queue as fixups -- the hook
  ** writes the code, the codec owns positions and the fixup list, and this is
  ** how the two say so to each other.
  **
  ** Light userdata never reaches here: 10.7 refuses it before the hook, because
  ** a bare pointer cannot be reconstituted by anyone, hook or not.
  */
  int (*encode) (lua_State *L, int idx, void *ud);
  /*
  ** The other direction, for ext codes 0x03, 0x05, 0x06 and 0x07. Push one
  ** value and return 1, or return 0 to have the codec raise. Ext 0x04 is the
  ** codec's own and is never offered.
  **
  ** The codec gives the result a position for 0x05 and 0x06 and withholds one
  ** for 0x03 and 0x07, mirroring what 'permanent' and 'encode' did on the way
  ** out. That asymmetry is the format, not an implementation detail: get it
  ** wrong in either direction and every position after the first named object
  ** shifts.
  */
  int (*decode) (lua_State *L, int code, const unsigned char *data, size_t len,
                 void *ud);
  void *ud;
} diluvium_snap_hooks;

/*
** Append one ext object to the snapshot encode currently in progress.
**
** This is how a hook writes its bytes. The encode buffer is the codec's, and a
** hook has no way to reach it -- so rather than handing the buffer out, the codec
** keeps a pointer to the encode in progress and this is the only door to it. A
** call outside an 'encode_graph' raises, since there is nothing to append to.
**
** Snapshot encoding is therefore not reentrant, which is stated rather than
** discovered: a hook must not start a second snapshot from inside the first.
*/
LUA_API void diluvium_msgpack_appendext (lua_State *L, int code,
                                         const char *data, size_t len);

LUA_API void diluvium_msgpack_encode_graph (lua_State *L, int idx,
                                    const diluvium_snap_hooks *h);
LUA_API void diluvium_msgpack_decode_graph (lua_State *L, const char *s,
                                    size_t len,
                                    const diluvium_snap_hooks *h);

#endif
