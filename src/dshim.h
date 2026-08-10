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
** So this is the whole of that dependency. dshim.c includes lstate.h, lobject.h,
** lfunc.h and lgc.h; nothing else in the tree may. When upstream renames a CallInfo
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
  int is_vararg;         /* the prototype takes '...', Lua frames only */
  /*
  ** Vararg count -- and only meaningful when 'is_vararg'. 'u.l.nextraargs' is
  ** written by 'luaT_adjustvarargs' and by nothing else, so for a Lua frame whose
  ** prototype takes no '...' it holds whatever the last user of that recycled
  ** CallInfo left there. Reported as 0 in that case rather than passed on, which
  ** is what the first version did -- and it made every real thread fail its own
  ** range check on a garbage value.
  */
  int nextraargs;
  int nresults;          /* expected results, decoded from callstatus */
  unsigned long callstatus;
  long ctx;              /* u.c.ctx, C frames only */
  int funcidx;           /* u2.funcidx */
  long old_errfunc;      /* u.c.old_errfunc, C frames only */
  /*
  ** The continuation itself, C frames only, NULL when there is none.
  **
  ** A bare function pointer, which is plain data and so belongs here -- but it is
  ** not writable to a snapshot, because the address differs between processes.
  ** The snapshot layer looks it up in its named-continuation table; see
  ** 'diluvium_snap_addcont'. Exposed rather than kept private because the
  ** alternative was a callback threaded through the capture, and a pointer the
  ** caller may only compare is simpler than that.
  */
  lua_KFunction k;
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

/*
** Walk the to-be-closed chain, innermost (highest slot) first.
**
** Pass 0 to start; returns the next marked slot, or 0 when there are none left.
** 'diluvium_shim_tbclist' gives only the head, and the rest of the chain is a
** run of deltas stored in the slots themselves -- including dummy nodes the VM
** inserts when a gap exceeds what a delta can hold, which this skips. A snapshot
** needs the whole list, since 'defer' puts a to-be-closed local in scope and
** 14 makes that round trip an acceptance criterion.
*/
LUA_API int diluvium_shim_tbcnext (lua_State *co, int from);

/* 'co->status': LUA_OK, LUA_YIELD, or an error status. */
LUA_API int diluvium_shim_status (lua_State *co);

/*
** 'u2.nyield' of the innermost frame: how many values the thread yielded.
**
** 'lua_resume' reports this as the result count when a thread yields, and for a
** frame whose continuation is NULL it is what 'luaD_poscall' moves. Restoring it
** is the difference between a resumed thread handing back the values it yielded
** and handing back nothing.
*/
LUA_API int diluvium_shim_nyield (lua_State *co);


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


/*
** Named C continuations.
**
** 10.4 covers C *functions*; a continuation is the other half, and the draft did
** not mention it. A suspended agent's chain has C frames in it -- dtask.c's
** driver and 'queue.wait' both yield with a continuation saved -- and a
** continuation is a bare function pointer with nowhere to be written down. So it
** is named, exactly as a C function is.
**
** A fixed array rather than a table keyed by the pointer, because casting a
** function pointer to 'void *' for light userdata is not something C promises,
** and there are a handful of these rather than hundreds. No 'lua_State' is
** involved, so a library can register from wherever it installs the
** continuation -- which is the right place, since registering next to the
** 'lua_yieldk' call is an ordering that cannot be got wrong.
**
** Returns 1 on success, 0 if the table is full or the name is already bound to a
** different function. Idempotent.
*/
LUA_API int diluvium_shim_addcont (const char *name, lua_KFunction k);

/* The name bound to 'k', or NULL. */
LUA_API const char *diluvium_shim_contname (lua_KFunction k);

/* The function bound to 'name' (not NUL-terminated; 'len' bytes), or NULL. */
LUA_API lua_KFunction diluvium_shim_contfunc (const char *name, size_t len);


/*
** ---------------------------------------------------------------------------
** Rebuilding a suspended thread.
** ---------------------------------------------------------------------------
**
** This is the write half, and it is the part doc/Messaging.md 10.10 warns about:
** it puts a CallInfo chain, stack offsets, pc offsets, upvalue slot indices and
** 'tbclist' straight into a 'lua_State'. Inline prototypes at least pass through
** the loader and reach lverify.c; none of this reaches anything. The measured
** baseline for why that matters is in the roadmap -- one flipped byte in a dump
** crashed the release interpreter about 7% of the time.
**
** So the shape here is validate-then-build, and the split is the whole point.
** 'diluvium_shim_checkframes' answers every range and consistency question with
** nothing written; only after it accepts does 'diluvium_shim_restore' touch the
** thread. A caller cannot get a half-built thread out of a malformed snapshot,
** because the two are separate calls and the first one cannot fail halfway.
**
** The frames are handed over as an array in the same order 'diluvium_shim_frame'
** reports them: outermost first.
*/

/*
** Why a frame array cannot be restored, or 0 when it can.
**
** Every one of these is a thing a malformed or hostile snapshot can say, and
** every one of them would otherwise be a wild pointer or an out-of-bounds read.
*/
#define DILUVIUM_RES_OK			0
#define DILUVIUM_RES_NO_FRAMES		1   /* an empty chain */
#define DILUVIUM_RES_STACK		2   /* a stack size out of bounds */
#define DILUVIUM_RES_FUNC_INDEX		3   /* func outside the stack */
#define DILUVIUM_RES_TOP_INDEX		4   /* top outside the stack, or below func */
#define DILUVIUM_RES_NOT_MONOTONIC	5   /* frames not in increasing order */
#define DILUVIUM_RES_NOT_FUNCTION	6   /* func slot holds no function */
#define DILUVIUM_RES_C_MISMATCH		7   /* is_c disagrees with the slot */
#define DILUVIUM_RES_PC			8   /* pc outside the prototype's code */
#define DILUVIUM_RES_NRESULTS		9   /* an unrepresentable result count */
#define DILUVIUM_RES_CALLSTATUS		10  /* flags this runtime does not know */
#define DILUVIUM_RES_NEXTRAARGS		11  /* a vararg count that cannot fit */
#define DILUVIUM_RES_NO_CONTINUATION	12  /* a C frame below the top with no k */
#define DILUVIUM_RES_TBC		13  /* a tbc index out of range or unordered */

/*
** Check an array of 'n' frames against the thread they will be built into, with
** the value stack already populated to 'nslots' live slots.
**
** 'out_frame' receives the offending frame index, or -1. Returns
** DILUVIUM_RES_OK or a code above; 'diluvium_shim_resreason' words it.
**
** The value stack must be in place first, because half these checks are about
** what the frames *say* about it: that 'func_index' holds a function, that
** 'is_c' agrees with what is actually there, that 'pc' is inside the prototype
** the function actually has.
*/
LUA_API int diluvium_shim_checkframes (lua_State *co,
                                       const diluvium_frame *frames, int n,
                                       int nslots, int *out_frame);

/* Slots the frames need reserved: the highest 'top_index' any of them names,
   and the highest register any Lua frame's prototype uses. A caller sizes the
   stack with this before writing slots, since 'checkframes' then holds the
   frames to it. */
LUA_API int diluvium_shim_neededstack (const diluvium_frame *frames, int n);

LUA_API const char *diluvium_shim_resreason (int code);

/*
** Reserve 'capacity' slots on 'co' and set its live top at 'nslots'.
**
** The two numbers are different and both are needed, which was not obvious: a
** frame's 'ci->top' is 'func + 1 + maxstacksize', which sits *above* the live
** top -- those are the frame's unused registers. So the stack has to be
** allocated for the highest frame top while 'top' itself marks only the live
** slots. Validating 'top_index' against the live count instead of the capacity
** was the first version, and it refused every real thread.
**
** Called before the slots are written and before the frames are checked. Fails
** and returns 0 rather than raising, since it is reached from a decode that owes
** its caller a status.
*/
LUA_API int diluvium_shim_prepstack (lua_State *co, int nslots, int capacity);

/* Write value stack slot 'i' (1-based) of 'co' from the top of 'L'; pops it. */
LUA_API int diluvium_shim_setslot (lua_State *co, int i, lua_State *L);

/*
** Build the CallInfo chain and set the thread suspended. Only after
** 'diluvium_shim_checkframes' has accepted the same array.
**
** 'ks' holds the continuation for each C frame, NULL where there is none -- and
** a NULL for a C frame that is not the innermost is refused by 'checkframes',
** so this cannot build a chain that resume would walk into and find nothing.
*/
LUA_API int diluvium_shim_restore (lua_State *co,
                                   const diluvium_frame *frames, int n,
                                   lua_KFunction *ks, int nyield);

/*
** Re-open upvalue 'n' of the closure at 'idx' against slot 'slot' of 'co'.
**
** 10.3 needs this separately from the closed case because an open upvalue is not
** a value: it is an alias for a stack slot, and two closures over the same slot
** must come back sharing one. They do, because this goes through the same
** 'luaF_findupval' the VM uses, which returns the existing upvalue for a level
** if there is one -- so sharing is a consequence of the mechanism rather than
** something the snapshot has to describe.
*/
LUA_API int diluvium_shim_reopenupval (lua_State *L, int idx, int n,
                                       lua_State *co, int slot);

/* Mark slot 'slot' of 'co' to-be-closed. Slots must be given in order. */
LUA_API int diluvium_shim_settbc (lua_State *co, int slot);

#endif
