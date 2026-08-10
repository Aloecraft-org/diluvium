/*
** dshim.h
** The one place that reads Lua's internals.
**
** doc/Messaging.md 3.1 draws a line that matters: *patching* core files creates
** merge conflicts on every upstream sync and is forbidden, while *depending on
** core internal headers* creates a compile break when upstream moves a field --
** loud, localised, and fixable in one place. Hibernation needs the second kind,
** because a coroutine's call chain and its open upvalues are not in the public
** API and cannot be.
**
** So this is the whole of that dependency. dshim.c includes lstate.h, lobject.h
** and lfunc.h; nothing else in the tree may. When upstream renames a CallInfo
** field, exactly one file fails to compile, and the failure says where.
**
** Everything here reports plain data -- ints, offsets, flags. No Lua type
** crosses this interface except by being pushed onto a state's stack, which is
** the same shape the public API uses. That is what keeps the serialiser in
** dsnap.c free of internal headers even though it is the thing that needs the
** information.
*/

#ifndef dshim_h
#define dshim_h

#include <stddef.h>

#include "lua.h"


/*
** One frame of a coroutine's call chain, flattened.
**
** 'has_k' is the field that matters most, and it is why 10.2 was rewritten. A C
** frame *without* a continuation cannot be captured: its working state is on the
** machine's C stack and there is nothing to write down. A C frame *with* one
** carries its resumption point explicitly -- k, ctx, funcidx, old_errfunc -- all
** plain data plus a function pointer the permanents table can name. The first is
** impossible; the second is merely out of scope for this milestone.
*/
typedef struct diluvium_frame {
  int is_c;              /* CIST_C is set */
  int has_k;             /* a continuation is present (C frames only) */
  int is_fresh;          /* CIST_FRESH: a fresh luaV_execute frame */
  int is_tail;           /* CIST_TAIL: reached by a tail call */
  int is_hooked;         /* CIST_HOOKED or CIST_HOOKYIELD: a hook is involved */
  int is_ypcall;         /* CIST_YPCALL: a yieldable protected call */
  int func_index;        /* 1-based index of the function in the value stack */
  int top_index;         /* 1-based index of this frame's top */
  int pc;                /* savedpc as an offset into the proto's code, or -1 */
  int nextraargs;        /* vararg count, Lua frames only */
  int nresults;          /* expected results, decoded from callstatus */
  unsigned long callstatus;
  long ctx;              /* u.c.ctx, C frames only */
  int funcidx;           /* u2.funcidx */
  long old_errfunc;      /* u.c.old_errfunc, C frames only */
} diluvium_frame;


/* Frames above 'base_ci', outermost first. 0 when the thread has not started. */
LUA_API int diluvium_shim_framecount (lua_State *co);

/* Fill 'out' for frame 'i', counting from 0 at the outermost. 1 on success. */
LUA_API int diluvium_shim_frame (lua_State *co, int i, diluvium_frame *out);

/* Slots in use on the value stack, so 1..n are readable. */
LUA_API int diluvium_shim_stacksize (lua_State *co);

/* Total slots allocated, which a restore has to be able to reach. */
LUA_API int diluvium_shim_stackcapacity (lua_State *co);

/*
** Copy value stack slot 'i' (1-based) of 'co' onto the top of 'L'.
**
** Both states must belong to the same Lua state, which they do: a snapshot is
** taken of a thread by the instance that owns it. 1 on success.
*/
LUA_API int diluvium_shim_pushslot (lua_State *co, int i, lua_State *L);

/* Where 'tbclist' points, as a 1-based stack index, or 0 when empty. */
LUA_API int diluvium_shim_tbclist (lua_State *co, int *out_index);

/* 'co->status': LUA_OK, LUA_YIELD, or an error status. */
LUA_API int diluvium_shim_status (lua_State *co);


/*
** Upvalues.
**
** 10.3 requires upvalue *identity* to survive: two closures sharing an upvalue
** must still share it after a round trip. That needs two things the public API
** does not give -- whether an upvalue is open (pointing into the stack) and, if
** so, which slot -- because a closed upvalue is copied by value while an open one
** has to be re-opened against the reconstructed stack.
**
** 'lua_upvalueid' is public and gives identity, so the serialiser uses that for
** the sharing map; this is only for the open/closed distinction.
*/

/* Is upvalue 'n' (1-based) of the closure at 'idx' open? */
LUA_API int diluvium_shim_upisopen (lua_State *L, int idx, int n);

/* Which 1-based stack slot of 'co' an open upvalue points at, or 0. */
LUA_API int diluvium_shim_upslot (lua_State *L, int idx, int n, lua_State *co);


/*
** Prototypes.
**
** 10.5 content-addresses a proto by the hash of its stripped dump, so the
** serialiser needs to know which proto a closure has and whether two closures
** share one. A pointer is the identity; it is never written to a snapshot.
*/

/* The Proto behind the Lua closure at 'idx', as an opaque identity. */
LUA_API const void *diluvium_shim_proto (lua_State *L, int idx);

/*
** Does this closure, or anything nested inside it, come from a secure
** (`~function`) prototype?
**
** 10.9: a snapshot walks the closure graph and writes protos, so a naive
** implementation would put a secure function's constants in a plain stream --
** exactly what the feature exists to prevent. The serialiser needs to know, and
** `is_encrypted` is not reachable from the public API.
*/
LUA_API int diluvium_shim_is_secure (lua_State *L, int idx);


/*
** Why a thread cannot be captured right now, or 0 when it can.
**
** Numbered in the order they are checked, most fundamental first: a running
** thread has nothing written down at all, so asking about its frames is not
** even meaningful. These are the checks 10.7 demands, and the reason they live
** here is that all but one of them need the CallInfo chain.
**
** 'out_frame' receives the 0-based index of the offending frame when there is
** one, and -1 otherwise.
*/
#define DILUVIUM_SNAP_OK		0
#define DILUVIUM_SNAP_NOT_SUSPENDED	1  /* running, normal, or dead */
#define DILUVIUM_SNAP_HOOK		2  /* inside a debug hook */
#define DILUVIUM_SNAP_C_FRAME		3  /* a C frame with no continuation */

/*
** On DILUVIUM_SNAP_C_FRAME and what "below" means.
**
** The innermost frame of a suspended thread is the one that called
** 'lua_yieldk', and a NULL k there is the ordinary case, not a defect: it means
** resume hands the yielded values back to that frame's caller, so there is no
** C state to save. 'coroutine.yield' is built this way. Only frames *below*
** the yield are checked.
**
** Given that, this code is an invariant assertion rather than a gate that
** fires in practice: the VM already refuses to yield across a
** continuation-less C frame ("attempt to yield across a C-call boundary"), so
** the shape should be unreachable. That is precisely why it is worth checking.
** The invariant is what makes hibernation possible, nothing in the type system
** enforces it, and a future host call that reaches Lua with 'lua_call' instead
** of 'lua_callk' would erode it quietly.
*/

/*
** NOT_SUSPENDED covers the running thread only when it has Lua frames. A
** thread sitting at the C host boundary with an empty call chain is
** indistinguishable from one that has never started, because both are empty;
** capturing either yields a thread with nothing to run, which is harmless. The
** snapshot layer, which knows which thread it was called on, is where refusing
** to capture yourself belongs.
*/

LUA_API int diluvium_shim_capturable (lua_State *co, int *out_frame);

/*
** The per-frame rule 'diluvium_shim_capturable' applies, on its own.
**
** It is separate because it is the one part of the walk that cannot be tested
** through the walk. The rule says "a C frame with no continuation is fatal
** unless it is the innermost frame", and widening that to "unless it is a C
** frame" is a mistake no test over real threads can catch -- the offending
** shape is unreachable, so both versions accept everything that exists. Given a
** frame and whether it is innermost, the rule is checkable directly, which is
** what dshim_check.c does with frames it fills in itself.
**
** Returns a DILUVIUM_SNAP_* code. 'diluvium_shim_capturable' is this applied to
** every frame, after the whole-thread checks.
*/
LUA_API int diluvium_shim_framecapturable (const diluvium_frame *f,
                                           int is_innermost);

/* A sentence naming the reason, for the diagnostic a programmer actually sees. */
LUA_API const char *diluvium_shim_reason (int code);

#endif
