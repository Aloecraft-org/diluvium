/*
** dshim.c
** The one place that reads Lua's internals. See dshim.h.
**
** Everything below is a read of a struct field plus arithmetic. There is no
** allocation, no error raising, and no Lua call, which is deliberate: a
** snapshot walks a suspended thread, and anything that could reallocate that
** thread's stack while the walk holds an index into it would be a bug that
** only shows up under memory pressure. The one exception is
** 'diluvium_shim_pushslot', which pushes onto the *capturing* state and asks
** the caller to have reserved room first.
**
** Note on includes: the amalgamation makes every core header available to every
** file in it, so a missing '#include' here compiles there and fails only in the
** standalone build. Both 'LUAI_MAXSTACK' (which turns out to live in ldo.c, not
** in any header) and 'luaC_objbarrier' were found that way, by the release build
** and not by the test suite. 'make build_platform' belongs in the sweep for
** exactly this reason.
**
** Stack indices here are 1-based over the raw stack array: index 1 is
** 'co->stack.p[0]', the slot 'base_ci' calls its function. That is not the
** same numbering as the public API's stack indices, which are relative to the
** current frame; using the raw array is the only numbering that stays
** meaningful across a whole call chain, which is what a snapshot needs.
*/

#define dshim_c
#if !defined(LUA_CORE)
#define LUA_CORE
#endif

#include "lprefix.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "lua.h"

#include "lfunc.h"
#include "lgc.h"
#include "lobject.h"
#include "lstate.h"

#include "dshim.h"


/*
** Frames, outermost first.
**
** The CallInfo list is *cached*: 'ci->next' survives the frame it described
** being popped, so walking forward from 'base_ci' would report frames that are
** no longer live. Only the 'previous' chain from 'co->ci' is truthful, so both
** functions below walk backwards and convert.
*/

LUA_API int diluvium_shim_framecount (lua_State *co) {
  CallInfo *ci;
  int n = 0;
  for (ci = co->ci; ci != &co->base_ci; ci = ci->previous)
    n++;
  return n;
}


static CallInfo *getframe (lua_State *co, int i) {
  CallInfo *ci = co->ci;
  int n = diluvium_shim_framecount(co);
  int back = n - 1 - i;  /* frame 0 is the outermost */
  if (i < 0 || i >= n)
    return NULL;
  while (back-- > 0)
    ci = ci->previous;
  return ci;
}


LUA_API int diluvium_shim_frame (lua_State *co, int i, diluvium_frame *out) {
  CallInfo *ci = getframe(co, i);
  if (ci == NULL || out == NULL)
    return 0;
  out->callstatus = cast(unsigned long, ci->callstatus);
  out->is_c = (ci->callstatus & CIST_C) ? 1 : 0;
  out->is_fresh = (ci->callstatus & CIST_FRESH) ? 1 : 0;
  out->is_tail = (ci->callstatus & CIST_TAIL) ? 1 : 0;
  out->is_hooked = (ci->callstatus & (CIST_HOOKED | CIST_HOOKYIELD)) ? 1 : 0;
  out->is_ypcall = (ci->callstatus & CIST_YPCALL) ? 1 : 0;
  out->func_index = cast_int(ci->func.p - co->stack.p) + 1;
  out->top_index = cast_int(ci->top.p - co->stack.p) + 1;
  out->nresults = get_nresults(ci->callstatus);
  out->funcidx = ci->u2.funcidx;
  if (out->is_c) {
    out->k = ci->u.c.k;
    out->has_k = (ci->u.c.k != NULL) ? 1 : 0;
    out->ctx = cast(long, ci->u.c.ctx);
    out->old_errfunc = cast(long, ci->u.c.old_errfunc);
    out->pc = -1;
    out->is_vararg = 0;
    out->nextraargs = 0;
  }
  else {
    /* 'savedpc' points at the *next* instruction to execute, which is what a
       restore wants to resume from, so it is reported without adjustment. */
    const Proto *p = clLvalue(s2v(ci->func.p))->p;
    out->k = NULL;
    out->has_k = 0;
    out->ctx = 0;
    out->old_errfunc = 0;
    out->pc = cast_int(ci->u.l.savedpc - p->code);
    out->is_vararg = isvararg(p) ? 1 : 0;
    out->nextraargs = out->is_vararg ? ci->u.l.nextraargs : 0;
  }
  return 1;
}


LUA_API int diluvium_shim_stacksize (lua_State *co) {
  return cast_int(co->top.p - co->stack.p);
}


LUA_API int diluvium_shim_stackcapacity (lua_State *co) {
  return cast_int(co->stack_last.p - co->stack.p);
}


LUA_API int diluvium_shim_pushslot (lua_State *co, int i, lua_State *L) {
  if (i < 1 || i > diluvium_shim_stacksize(co))
    return 0;
  if (!lua_checkstack(L, 1))
    return 0;
  setobj2s(L, L->top.p, s2v(co->stack.p + (i - 1)));
  L->top.p++;
  return 1;
}


LUA_API int diluvium_shim_tbclist (lua_State *co, int *out_index) {
  /* An empty list is 'tbclist' resting on the stack base (lstate.c). */
  if (co->tbclist.p == co->stack.p) {
    if (out_index != NULL) *out_index = 0;
    return 0;
  }
  if (out_index != NULL)
    *out_index = cast_int(co->tbclist.p - co->stack.p) + 1;
  return 1;
}


/*
** The chain is a run of deltas held in the slots themselves: each marked slot
** stores the distance down to the previous one. 'luaF_newtbcupval' inserts dummy
** nodes with delta 0 whenever a gap exceeds what an unsigned short can hold, and
** those are stepped over by subtracting the maximum delta rather than the stored
** zero -- this mirrors 'poptbclist' in lfunc.c exactly, because getting it wrong
** in a different way than the VM does would produce a list that closes the wrong
** slots.
**
** MAXDELTA is a private '#define' in lfunc.c, not in lfunc.h, so the constant is
** repeated here. That is the one duplicated value in this file, and if upstream
** changes it the tbc round-trip test is what notices.
*/
#define DSHIM_MAXDELTA		USHRT_MAX

LUA_API int diluvium_shim_tbcnext (lua_State *co, int from) {
  StkId tbc;
  if (co->tbclist.p == co->stack.p)
    return 0;                          /* nothing marked at all */
  if (from <= 0)
    tbc = co->tbclist.p;               /* the head, which is never a dummy */
  else {
    tbc = co->stack.p + (from - 1);
    if (tbc <= co->stack.p || tbc > co->tbclist.p ||
        tbc->tbclist.delta == 0)
      return 0;
    tbc -= tbc->tbclist.delta;
    while (tbc > co->stack.p && tbc->tbclist.delta == 0)
      tbc -= DSHIM_MAXDELTA;
    if (tbc <= co->stack.p)
      return 0;
  }
  return cast_int(tbc - co->stack.p) + 1;
}


LUA_API int diluvium_shim_status (lua_State *co) {
  return cast_int(co->status);
}


LUA_API int diluvium_shim_nyield (lua_State *co) {
  if (co->ci == &co->base_ci)
    return 0;
  return co->ci->u2.nyield;
}


/*
** Upvalues.
*/

static UpVal *getupval (lua_State *L, int idx, int n) {
  const LClosure *cl;
  if (lua_type(L, idx) != LUA_TFUNCTION || lua_iscfunction(L, idx))
    return NULL;  /* only Lua closures have UpVal objects */
  cl = cast(const LClosure *, lua_topointer(L, idx));
  if (cl == NULL || n < 1 || n > cast_int(cl->nupvalues))
    return NULL;
  return cl->upvals[n - 1];
}


LUA_API int diluvium_shim_upisopen (lua_State *L, int idx, int n) {
  UpVal *up = getupval(L, idx, n);
  return (up != NULL && upisopen(up)) ? 1 : 0;
}


LUA_API int diluvium_shim_upslot (lua_State *L, int idx, int n,
                                  lua_State *co) {
  UpVal *up = getupval(L, idx, n);
  StkId level;
  if (up == NULL || !upisopen(up))
    return 0;
  level = uplevel(up);
  /* An open upvalue belongs to exactly one thread's stack. Report a slot only
     when it is this one's, so a caller that guesses wrong gets 0 rather than
     an offset into an unrelated stack. */
  if (level < co->stack.p || level >= co->stack_last.p)
    return 0;
  return cast_int(level - co->stack.p) + 1;
}


/*
** Prototypes.
*/

LUA_API const void *diluvium_shim_proto (lua_State *L, int idx) {
  const LClosure *cl;
  if (lua_type(L, idx) != LUA_TFUNCTION || lua_iscfunction(L, idx))
    return NULL;
  cl = cast(const LClosure *, lua_topointer(L, idx));
  return (cl == NULL) ? NULL : cast_voidp(cl->p);
}


static int protosecure (const Proto *p) {
  int i;
  if (p->is_encrypted)
    return 1;
  /* Nested protos are checked too. ldump.c taints a secure function's strings
     by inheriting the flag downwards rather than trusting every nested proto to
     carry it, so the same must hold here: a plain outer function can contain a
     '~function'. */
  for (i = 0; i < p->sizep; i++) {
    if (protosecure(p->p[i]))
      return 1;
  }
  return 0;
}


LUA_API int diluvium_shim_is_secure (lua_State *L, int idx) {
  const Proto *p = cast(const Proto *, diluvium_shim_proto(L, idx));
  return (p != NULL && protosecure(p)) ? 1 : 0;
}


/*
** Capturability.
**
** The checks run cheapest-and-most-fundamental first: a running thread has no
** written-down state at all, a hooked thread has state the hook owns, and only
** then is it worth walking frames.
*/

LUA_API int diluvium_shim_framecapturable (const diluvium_frame *f,
                                           int is_innermost) {
  if (f == NULL)
    return DILUVIUM_SNAP_OK;
  if (f->is_hooked)
    return DILUVIUM_SNAP_HOOK;
  /* The property doc/Messaging.md 14 warns will break silently during
  ** refactoring: every C frame *below* the yield must carry a continuation, or
  ** its working state is on the machine's C stack where nothing can reach it.
  ** dtask.c's driver exists to make this true of its own frame.
  **
  ** The innermost frame is exempt, and the exemption is not a loophole. A
  ** thread suspends by calling 'lua_yieldk', so the innermost frame is the one
  ** that yielded; when its k is NULL that means "resume returns my yielded
  ** values to my caller" (ldo.c's 'resume' takes the poscall path), which needs
  ** no saved C state at all. 'coroutine.yield' is exactly this, so without the
  ** exemption every ordinary suspended coroutine would be refused. */
  if (!is_innermost && f->is_c && !f->has_k)
    return DILUVIUM_SNAP_C_FRAME;
  return DILUVIUM_SNAP_OK;
}


LUA_API int diluvium_shim_capturable (lua_State *co, int *out_frame) {
  int n, i;
  if (out_frame != NULL) *out_frame = -1;
  n = diluvium_shim_framecount(co);
  /* Suspended means either yielded, or started-but-not-run. 'status == LUA_OK'
     with frames is the running thread or one that resumed another; neither can
     be captured, and both look identical from here, which is why the frame
     count is part of the test. */
  if (co->status != LUA_YIELD && !(co->status == LUA_OK && n == 0))
    return DILUVIUM_SNAP_NOT_SUSPENDED;
  if (!co->allowhook)
    return DILUVIUM_SNAP_HOOK;
  for (i = 0; i < n; i++) {
    diluvium_frame f;
    int code;
    if (!diluvium_shim_frame(co, i, &f))
      continue;
    code = diluvium_shim_framecapturable(&f, i + 1 == n);
    if (code != DILUVIUM_SNAP_OK) {
      if (out_frame != NULL) *out_frame = i;
      return code;
    }
  }
  return DILUVIUM_SNAP_OK;
}


LUA_API const char *diluvium_shim_reason (int code) {
  switch (code) {
    case DILUVIUM_SNAP_OK:
      return "thread can be captured";
    case DILUVIUM_SNAP_NOT_SUSPENDED:
      return "thread is not suspended (only a suspended coroutine has all of "
             "its state written down)";
    case DILUVIUM_SNAP_HOOK:
      return "thread is inside a debug hook";
    case DILUVIUM_SNAP_C_FRAME:
      return "thread has a C function frame with no continuation, whose state "
             "is on the C stack and cannot be written down";
    default:
      return "unknown reason";
  }
}


/* ======================================================================
** Rebuilding a suspended thread. See dshim.h for why this is validate-then-
** build rather than one call.
** ====================================================================== */

/*
** The callstatus bits this runtime understands. A snapshot carrying anything
** else was written by a different build, or is lying; either way the flags would
** be handed to 'unroll' and 'luaV_execute', which read them without checking.
**
** CIST_HOOKED and CIST_HOOKYIELD are deliberately *not* here: 10.7 refuses to
** capture a hooked thread, so a snapshot claiming one is inconsistent with the
** capture rules and is refused on the way back in as well.
*/
#define DSHIM_KNOWN_FLAGS  \
	(CIST_NRESULTS | MAX_CCMT | (7u << CIST_RECST) | CIST_C | CIST_FRESH | \
	 CIST_CLSRET | CIST_TBC | CIST_OAH | CIST_YPCALL | CIST_TAIL | CIST_FIN)


LUA_API int diluvium_shim_prepstack (lua_State *co, int nslots, int capacity) {
  if (nslots < 1 || capacity < nslots || capacity > INT_MAX / 2)
    return 0;
  /*
  ** 'lua_checkstack' grows the stack and is public, so neither the allocation nor
  ** the real limit is reimplemented here -- it returns 0 for a request past what
  ** this build allows, which is exactly the answer this function owes its caller.
  **
  ** The first version compared against LUAI_MAXSTACK, which looks like a
  ** configuration constant and is not: it is defined in ldo.c, not in any header.
  ** The debug build compiled anyway because ltests.h happens to define it, so the
  ** breakage only appeared in the release build. The bound above is just enough
  ** to keep the addition below from overflowing.
  */
  if (!lua_checkstack(co, capacity + EXTRA_STACK))
    return 0;
  {
    StkId p = co->stack.p;
    int i;
    for (i = 0; i < nslots; i++)
      setnilvalue2s(p + i);
    co->top.p = p + nslots;
  }
  return 1;
}


LUA_API int diluvium_shim_setslot (lua_State *co, int i, lua_State *L) {
  if (i < 1 || i > diluvium_shim_stacksize(co) || lua_gettop(L) < 1)
    return 0;
  setobjs2s(co, co->stack.p + (i - 1), L->top.p - 1);
  L->top.p--;
  return 1;
}


LUA_API int diluvium_shim_checkframes (lua_State *co,
                                       const diluvium_frame *frames, int n,
                                       int nslots, int *out_frame) {
  int i, cap;
  int prevfunc = 0;
  if (out_frame != NULL) *out_frame = -1;
  if (frames == NULL || n < 1)
    return DILUVIUM_RES_NO_FRAMES;
  if (nslots < 1 || nslots > diluvium_shim_stacksize(co))
    return DILUVIUM_RES_STACK;
  cap = diluvium_shim_stackcapacity(co);
  for (i = 0; i < n; i++) {
    const diluvium_frame *f = &frames[i];
    if (out_frame != NULL) *out_frame = i;
    /* Slot 1 is 'base_ci''s function slot, so a real frame starts at 2. */
    if (f->func_index < 2 || f->func_index > nslots)
      return DILUVIUM_RES_FUNC_INDEX;
    /* A frame's top is 'func + 1 + maxstacksize', so it sits above the live
       slots by however many registers the frame is not currently using. The
       bound is therefore the allocated capacity, not the live count. */
    if (f->top_index <= f->func_index || f->top_index > cap + 1)
      return DILUVIUM_RES_TOP_INDEX;
    /* Frames nest, so each one's function sits above the last one's. Without
       this a chain could be built whose frames overlap, and 'luaD_poscall'
       would move results into another frame's registers. */
    if (f->func_index <= prevfunc)
      return DILUVIUM_RES_NOT_MONOTONIC;
    prevfunc = f->func_index;
    if (f->nresults < -1 || f->nresults > MAXRESULTS)
      return DILUVIUM_RES_NRESULTS;
    if (f->callstatus & ~cast(unsigned long, DSHIM_KNOWN_FLAGS))
      return DILUVIUM_RES_CALLSTATUS;
    {
      const TValue *fn = s2v(co->stack.p + (f->func_index - 1));
      if (!ttisfunction(fn))
        return DILUVIUM_RES_NOT_FUNCTION;
      if (f->is_c) {
        if (ttisLclosure(fn))
          return DILUVIUM_RES_C_MISMATCH;
        /* The invariant 10.2 rests on, checked on the way in as well as out:
           only the innermost frame may lack a continuation. */
        if (!f->has_k && i + 1 < n)
          return DILUVIUM_RES_NO_CONTINUATION;
      }
      else {
        const Proto *p;
        if (!ttisLclosure(fn))
          return DILUVIUM_RES_C_MISMATCH;
        p = clLvalue(fn)->p;
        /* 'savedpc' points at the next instruction, so it may equal sizecode
           only if the frame is about to return -- which cannot be a suspended
           state, so the bound is exclusive. */
        if (f->pc < 0 || f->pc >= p->sizecode)
          return DILUVIUM_RES_PC;
        /*
        ** Only a vararg prototype has extras, and 'ldo.c' computes a stack
        ** delta from this field, so a value on a non-vararg frame would move
        ** results to a wrong address. Both halves are checked: the flag against
        ** the prototype, and the count against the room below the frame base.
        */
        if (f->is_vararg != (isvararg(p) ? 1 : 0))
          return DILUVIUM_RES_NEXTRAARGS;
        if (f->nextraargs < 0 ||
            (!f->is_vararg && f->nextraargs != 0) ||
            f->func_index + 1 + f->nextraargs + p->numparams > cap + 1)
          return DILUVIUM_RES_NEXTRAARGS;
        /* The frame must have room for the registers its prototype uses, or
           'luaV_execute' reads past the stack on its first instruction. */
        if (f->func_index + 1 + p->maxstacksize > cap + 1)
          return DILUVIUM_RES_TOP_INDEX;
      }
    }
  }
  if (out_frame != NULL) *out_frame = -1;
  return DILUVIUM_RES_OK;
}


LUA_API int diluvium_shim_neededstack (const diluvium_frame *frames, int n) {
  int i, need = 0;
  if (frames == NULL)
    return 0;
  for (i = 0; i < n; i++) {
    if (frames[i].top_index > need)
      need = frames[i].top_index;
  }
  return need;
}


LUA_API int diluvium_shim_restore (lua_State *co,
                                   const diluvium_frame *frames, int n,
                                   lua_KFunction *ks, int nyield) {
  int i;
  if (frames == NULL || n < 1)
    return 0;
  co->ci = &co->base_ci;
  co->base_ci.func.p = co->stack.p;
  co->base_ci.top.p = co->stack.p + 1 + LUA_MINSTACK;
  co->base_ci.callstatus = CIST_C;
  co->base_ci.u.c.k = NULL;
  co->base_ci.u.c.old_errfunc = 0;
  co->base_ci.u.c.ctx = 0;
  for (i = 0; i < n; i++) {
    const diluvium_frame *f = &frames[i];
    CallInfo *ci = luaE_extendCI(co, 1);
    ci->func.p = co->stack.p + (f->func_index - 1);
    ci->top.p = co->stack.p + (f->top_index - 1);
    ci->callstatus = cast(l_uint32, f->callstatus);
    ci->u2.funcidx = f->funcidx;
    if (f->is_c) {
      ci->u.c.k = (ks != NULL) ? ks[i] : NULL;
      ci->u.c.ctx = cast(lua_KContext, f->ctx);
      ci->u.c.old_errfunc = cast(ptrdiff_t, f->old_errfunc);
    }
    else {
      const Proto *p = clLvalue(s2v(ci->func.p))->p;
      ci->u.l.savedpc = p->code + f->pc;
      ci->u.l.trap = 0;
      ci->u.l.nextraargs = f->nextraargs;
    }
    co->ci = ci;
  }
  /*
  ** 'nyield' on the innermost frame is what 'lua_resume' reports as the number
  ** of results when the thread yields again -- and, for a frame whose
  ** continuation is NULL, what 'luaD_poscall' moves. Restoring it is the
  ** difference between a resumed thread handing back the values it yielded and
  ** handing back nothing.
  */
  co->ci->u2.nyield = nyield;
  co->status = LUA_YIELD;
  return 1;
}


LUA_API int diluvium_shim_reopenupval (lua_State *L, int idx, int n,
                                       lua_State *co, int slot) {
  const LClosure *cl;
  UpVal *up;
  if (lua_type(L, idx) != LUA_TFUNCTION || lua_iscfunction(L, idx))
    return 0;
  cl = cast(const LClosure *, lua_topointer(L, idx));
  if (cl == NULL || n < 1 || n > cast_int(cl->nupvalues))
    return 0;
  if (slot < 1 || slot > diluvium_shim_stacksize(co))
    return 0;
  /* The same call the VM makes when a closure captures a local: it returns the
     existing upvalue for that level if one is already open, which is why two
     closures over one slot come back sharing without the snapshot describing
     the sharing at all. */
  up = luaF_findupval(co, co->stack.p + (slot - 1));
  ((LClosure *)cl)->upvals[n - 1] = up;
  luaC_objbarrier(L, cl, up);
  return 1;
}


LUA_API int diluvium_shim_settbc (lua_State *co, int slot) {
  if (slot < 1 || slot > diluvium_shim_stacksize(co))
    return 0;
  /*
  ** Must sit strictly above whatever 'tbclist' currently points at -- which on a
  ** fresh thread is the stack base, so slot 1 is never markable.
  ** 'luaF_newtbcupval' asserts exactly this, and the first version skipped the
  ** check when the list was empty; slot 1 then tripped the assertion and took
  ** the process down instead of returning 0.
  */
  if (co->stack.p + (slot - 1) <= co->tbclist.p)
    return 0;
  luaF_newtbcupval(co, co->stack.p + (slot - 1));
  return 1;
}


LUA_API const char *diluvium_shim_resreason (int code) {
  switch (code) {
    case DILUVIUM_RES_OK:
      return "the frames can be restored";
    case DILUVIUM_RES_NO_FRAMES:
      return "a suspended thread with no frames";
    case DILUVIUM_RES_STACK:
      return "a stack size outside what was allocated";
    case DILUVIUM_RES_FUNC_INDEX:
      return "a frame's function is outside the value stack";
    case DILUVIUM_RES_TOP_INDEX:
      return "a frame's top is outside the value stack, or not above its "
             "function";
    case DILUVIUM_RES_NOT_MONOTONIC:
      return "frames do not nest: one does not sit above the last";
    case DILUVIUM_RES_NOT_FUNCTION:
      return "a frame's function slot does not hold a function";
    case DILUVIUM_RES_C_MISMATCH:
      return "a frame says C or Lua and its function slot says the other";
    case DILUVIUM_RES_PC:
      return "a frame's pc is outside its prototype's code";
    case DILUVIUM_RES_NRESULTS:
      return "a frame expects a number of results that cannot be represented";
    case DILUVIUM_RES_CALLSTATUS:
      return "a frame carries call-status flags this runtime does not know";
    case DILUVIUM_RES_NEXTRAARGS:
      return "a frame's vararg count does not fit its stack";
    case DILUVIUM_RES_NO_CONTINUATION:
      return "a C frame below the yield has no continuation, so resume would "
             "walk into it and find nothing";
    case DILUVIUM_RES_TBC:
      return "a to-be-closed slot is out of range or out of order";
    default:
      return "unknown reason";
  }
}


/* ======================================================================
** Named C continuations. See dshim.h.
** ====================================================================== */

#define DSHIM_MAXCONT	64

typedef struct dshim_cont { const char *name; lua_KFunction k; } dshim_cont;
static dshim_cont dshim_conts[DSHIM_MAXCONT];
static int dshim_ncont = 0;

LUA_API int diluvium_shim_addcont (const char *name, lua_KFunction k) {
  int i;
  if (name == NULL || k == NULL)
    return 0;
  for (i = 0; i < dshim_ncont; i++) {
    if (strcmp(dshim_conts[i].name, name) == 0)
      return (dshim_conts[i].k == k);  /* idempotent, never a silent rebind */
  }
  if (dshim_ncont >= DSHIM_MAXCONT)
    return 0;
  dshim_conts[dshim_ncont].name = name;
  dshim_conts[dshim_ncont].k = k;
  dshim_ncont++;
  return 1;
}


LUA_API const char *diluvium_shim_contname (lua_KFunction k) {
  int i;
  for (i = 0; i < dshim_ncont; i++) {
    if (dshim_conts[i].k == k)
      return dshim_conts[i].name;
  }
  return NULL;
}


LUA_API lua_KFunction diluvium_shim_contfunc (const char *name, size_t len) {
  int i;
  for (i = 0; i < dshim_ncont; i++) {
    if (strlen(dshim_conts[i].name) == len &&
        memcmp(dshim_conts[i].name, name, len) == 0)
      return dshim_conts[i].k;
  }
  return NULL;
}
