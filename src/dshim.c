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

#include <stddef.h>

#include "lua.h"

#include "lfunc.h"
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
    out->has_k = (ci->u.c.k != NULL) ? 1 : 0;
    out->ctx = cast(long, ci->u.c.ctx);
    out->old_errfunc = cast(long, ci->u.c.old_errfunc);
    out->pc = -1;
    out->nextraargs = 0;
  }
  else {
    /* 'savedpc' points at the *next* instruction to execute, which is what a
       restore wants to resume from, so it is reported without adjustment. */
    const Proto *p = clLvalue(s2v(ci->func.p))->p;
    out->has_k = 0;
    out->ctx = 0;
    out->old_errfunc = 0;
    out->pc = cast_int(ci->u.l.savedpc - p->code);
    out->nextraargs = ci->u.l.nextraargs;
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


LUA_API int diluvium_shim_status (lua_State *co) {
  return cast_int(co->status);
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
