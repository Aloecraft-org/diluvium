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

/*
** The shape a msgpack.as_array/as_map wrapper declares for the value at
** 'idx': one of the DILUVIUM_MP_SHAPE_* values, or 0 for anything that is
** not a wrapper. Checked by metatable identity, so a lookalike table is 0.
** The wrapped table itself sits at index 1 of the wrapper. Exported for
** djson, so that one tag answers the empty-container question for both
** codecs instead of each inventing its own.
*/
#define DILUVIUM_MP_SHAPE_ARRAY	1
#define DILUVIUM_MP_SHAPE_MAP	2
LUA_API int diluvium_msgpack_shapeof (lua_State *L, int idx);

/*
** Is the value at 'idx' the msgpack.null sentinel? The VALUE that encodes
** as msgpack nil (a table cannot hold nil, so an intentional null in an
** array needs a spelling that is not a hole). Exported for djson, which
** encodes it as JSON null -- one null vocabulary for both codecs, like the
** shape wrappers.
*/
LUA_API int diluvium_msgpack_isnull (lua_State *L, int idx);
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
** 'encode' is offered every table carrying a metatable, and every value the
** codec cannot encode at all. It should either return 0 -- "not mine", and the
** codec carries on as if it were not installed -- or push an integer ext code
** and a payload string and return 1, and the codec writes that ext object.
**
** Returning the bytes rather than appending them keeps the encode buffer inside
** the codec, which is why this differs from the snapshot layer's hooks: those
** append through 'diluvium_msgpack_appendext'. A resolver is consulted only
** outside snapshot mode, where interning and positions do not apply.
**
** A table with a metatable is offered *before* it is encoded as a map or an
** array, which is what makes an endpoint reference survive a message: without
** the hook, 7.3's reference object -- a table the guest cannot inspect -- goes
** onto the wire as a plain one-element array, and a router cannot hand a handler
** its destination.
*/
typedef struct diluvium_msgpack_resolver {
  int (*decode) (lua_State *L, const unsigned char *data, size_t len, void *ud);
  int (*encode) (lua_State *L, int idx, void *ud);
  void *ud;
} diluvium_msgpack_resolver;

LUA_API void diluvium_msgpack_setresolver (lua_State *L,
                                    const diluvium_msgpack_resolver *r);


/*
** A read-only token cursor, with no 'lua_State' anywhere in sight.
**
** The swarm layer (11.5, symbol prefix 'dvs_') is a separate library built on the
** instance ABI, and it has to read the msgpack in a 'system/lifecycle' request to
** know what was asked for. It cannot use the decoder above, because that pushes
** Lua values and so needs a state -- and giving the swarm layer a state of its own
** just to parse a map would make 4.1's layer boundary a fiction.
**
** The alternative was a second msgpack reader inside the swarm library, which 5
** rejects on principle ("having one codec rather than three"). So the cursor lives
** here instead, beside the writer and the format tables it has to agree with: two
** entry points, one file that owns the wire format.
**
** It reports tokens rather than building values, which is all a caller reading a
** known shape needs, and it has no allocation and no failure mode beyond "the
** bytes ran out" or "that is not a type I know".
*/
typedef enum diluvium_mp_kind {
  DILUVIUM_MP_END = 0,          /* no bytes left */
  DILUVIUM_MP_BAD,              /* not a msgpack type this cursor reads */
  DILUVIUM_MP_NIL,
  DILUVIUM_MP_BOOL,
  DILUVIUM_MP_INT,              /* signed and unsigned both land here */
  DILUVIUM_MP_FLOAT,
  DILUVIUM_MP_STR,              /* also bin: both are 'p' and 'len' */
  DILUVIUM_MP_ARRAY,            /* 'len' elements follow */
  DILUVIUM_MP_MAP,              /* 'len' key/value pairs follow */
  DILUVIUM_MP_EXT               /* 'code', then 'p' and 'len' */
} diluvium_mp_kind;

typedef struct diluvium_mp_token {
  diluvium_mp_kind kind;
  int b;                        /* BOOL */
  long long i;                  /* INT */
  double f;                     /* FLOAT */
  const char *p;                /* STR, EXT: into the caller's buffer */
  size_t len;                   /* STR, EXT, ARRAY, MAP */
  int code;                     /* EXT */
} diluvium_mp_token;

typedef struct diluvium_mp_cursor {
  const unsigned char *p;
  size_t left;
} diluvium_mp_cursor;

/* Point a cursor at some bytes. */
LUA_API void diluvium_mp_open (diluvium_mp_cursor *c, const void *s, size_t len);

/*
** Read one token. Returns 1, or 0 at the end or on a byte the cursor does not
** read (with 'kind' set to END or BAD, so a caller can tell those apart).
**
** A container reports its length; its contents are the tokens that follow. A
** caller that wants to skip one calls 'diluvium_mp_skip'.
*/
LUA_API int diluvium_mp_read (diluvium_mp_cursor *c, diluvium_mp_token *out);

/* Skip the value that starts here, container and all. 1 on success. */
LUA_API int diluvium_mp_skip (diluvium_mp_cursor *c);

/*
** Find 'key' at the top level of the map the cursor is on, leaving the cursor at
** its value and returning 1. On 0 the cursor is spent, so a caller looking up
** several keys reopens between them -- which is what makes this a cursor and not
** an index, and is fine for maps of a dozen fields.
*/
LUA_API int diluvium_mp_field (diluvium_mp_cursor *c, const char *key);


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
/*
** Fixup tags, and the shape of each one's operands.
**
** The codec owns the *shape* -- how many integers a tag takes and whether a
** graph value follows -- because that is the format, and a reader has to be able
** to parse the section without knowing what any tag means. The snapshot layer
** owns the *meaning* of 4 through 6, which touch thread internals the codec
** cannot reach.
**
**   1 META    pos                      + value (the metatable)
**   2 UPVAL   pos, index               + value
**   3 UPJOIN  pos, index, pos, index
**   4 SLOT    pos, slot                + value (a thread's stack slot)
**   5 BUILD   pos                            (build a thread's frames)
**   6 UPOPEN  pos, index, pos, slot          (re-open against a thread's slot)
*/
#define DILUVIUM_FIX_META	1
#define DILUVIUM_FIX_UPVAL	2
#define DILUVIUM_FIX_UPJOIN	3
#define DILUVIUM_FIX_SLOT	4
#define DILUVIUM_FIX_BUILD	5
#define DILUVIUM_FIX_UPOPEN	6
#define DILUVIUM_FIX_LAST	6

typedef struct diluvium_snap_hooks {
  /*
  ** Called once at the start of a stream in *either* direction, before anything
  ** is written or read, so a layer can clear per-stream state. Return 1, or 0 to
  ** have the codec raise.
  **
  ** Both directions, because per-stream state is symmetric: what the encoder
  ** inlined earlier in this snapshot is what the decoder may find a reference to,
  ** and neither may carry over into the next stream.
  **
  ** The symmetric partner of 'finish', and it exists for the same kind of reason:
  ** prototype dedup within one snapshot has to be *per stream*. The first version
  ** deduped through the runtime's own prototype registry, which meant taking a
  ** snapshot registered the prototypes it inlined -- so the next snapshot of the
  ** same program referenced them instead of carrying them, silently stopped being
  ** self-contained, and could not be restored anywhere else. It also made two
  ** consecutive snapshots different sizes, which is how it was found: a size
  ** enquiry disagreed with the snapshot that followed it.
  */
  int (*begin) (lua_State *L, void *ud);
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
  ** Push slot 'i' (1-based) of the thread at 'idx' and return 1, or return 0
  ** when 'i' is past the end or the value is not a thread.
  **
  ** A thread's value stack is raw stack memory, which the codec has no way to
  ** reach -- so a thread is the one graph object whose contents the codec has to
  ** be handed. It queues one SLOT fixup per slot and fetches each value at drain
  ** time through this, exactly as it fetches an upvalue through
  ** 'lua_getupvalue'. Return 0 for a non-thread and this is never consulted.
  */
  int (*slot) (lua_State *L, int idx, int i, void *ud);
  /*
  ** Offered each upvalue of a closure before the codec writes a UPVAL or UPJOIN
  ** for it. Return 1 having appended a complete fixup of your own -- through
  ** 'diluvium_msgpack_appendfixup' -- or 0 to let the codec write the ordinary
  ** one.
  **
  ** This exists for one case: an upvalue that is *open* against the stack of a
  ** thread that is also in this graph. Such an upvalue is not a value, it is an
  ** alias for a stack slot, and writing its value would quietly sever the alias
  ** -- so the parked coroutine could later assign to that local and the closure
  ** would not see it. Only the snapshot layer can tell, because only it knows
  ** which threads are being captured.
  */
  int (*openupval) (lua_State *L, int idx, int n, void *ud);
  /*
  ** Handle a fixup the codec parsed but does not own: SLOT, BUILD or UPOPEN.
  ** 'ops' holds the integer operands and any graph value is on top of the stack
  ** (and must be left there; the codec pops it). Return 1 if handled.
  */
  int (*fixup) (lua_State *L, int tag, const lua_Integer *ops, int nops,
                void *ud);
  /*
  ** Called once, after the whole fixup section has been replayed, so a layer can
  ** do work that depends on the graph being complete. Return 1, or 0 to have the
  ** codec raise.
  **
  ** It exists because of one case, and the case is instructive: a to-be-closed
  ** slot can only be marked once its value has a '__close' metamethod, and a
  ** metatable arrives as a fixup of its own -- which for a 'defer' object (a
  ** self-referential 'setmetatable(t, t)') is queued *later* than the thread that
  ** holds it. Marking during the thread's own fixup therefore found a table with
  ** no metatable and refused it. Anything whose precondition is "the graph is
  ** finished" belongs here rather than in an ordering assumption.
  */
  int (*finish) (lua_State *L, void *ud);
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

/*
** Append a fixup to the snapshot encode in progress: the tag, then 'nops'
** integer operands. A tag whose shape includes a graph value cannot be written
** this way, because encoding a value is the codec's own recursion.
*/
LUA_API void diluvium_msgpack_appendfixup (lua_State *L, int tag,
                                           const lua_Integer *ops, int nops);

/*
** The position the object at 'idx' has in the encode in progress, or 0 if it has
** none. A hook writing its own fixup needs this: positions are the codec's, and
** the hook has no other way to name an object.
*/
LUA_API lua_Integer diluvium_msgpack_position (lua_State *L, int idx);

/*
** Push the object at 'pos' in the *decode* in progress. Raises if there is no
** decode running or the position is out of range.
**
** The mirror of 'position': a hook handling a fixup is handed positions, and
** positions are the codec's -- so this is the only way to turn one back into an
** object. Range-checked here rather than by the caller, because 10.10 says a
** snapshot is untrusted input and a position is exactly the sort of field a
** malformed one gets wrong.
*/
LUA_API void diluvium_msgpack_pushobject (lua_State *L, lua_Integer pos);

LUA_API void diluvium_msgpack_encode_graph (lua_State *L, int idx,
                                    const diluvium_snap_hooks *h);
LUA_API void diluvium_msgpack_decode_graph (lua_State *L, const char *s,
                                    size_t len,
                                    const diluvium_snap_hooks *h);

#endif
