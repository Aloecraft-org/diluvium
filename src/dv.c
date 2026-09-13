/*
** dv.c
** The Diluvium instance ABI. See dv.h for the contract.
**
** The shape worth understanding before reading: 'dv_run' and 'dv_resume' are
** steps, not loops. The CLI's driver in dtask.c has a loop inside it because a
** CLI can afford to block; a host with an event loop of its own cannot, so this
** hands control back the moment the program parks and waits to be told what
** happened. Both drive the same body -- 'diluvium_task_pushbody' -- so the
** subtleties about continuations and non-yieldable protected calls live in one
** file rather than two.
*/

#define dv_c

#include "lprefix.h"

#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"
#include "dlibs.h"
#include "dendpoint.h"
#include "dqueue.h"
#include "dshim.h"
#include "dsnap.h"
#include "dtask.h"
#include "dnumeric.h"
#include "dv.h"


struct dv_instance {
  lua_State *L;
  int chunk_ref;              /* the loaded program, LUA_NOREF until loaded */
  int co_ref;                 /* the running thread */
  lua_State *co;
  int started;
  int finished;
  int parked;
  int pending;                /* values the park left on the thread's stack */
  diluvium_waitset ws;        /* what it is waiting for, while parked */
  char *error;
  void (*notify) (void *ud, dv_queue_id id);
  void *notify_ud;
  int (*endpoint_bind) (void *ud, const uint8_t *ref, size_t len,
                        uint32_t *token);
  void *endpoint_ud;
  uint32_t flags;
  /* Budgets (9.4). Zero means no limit. */
  uint64_t insn_limit;
  uint64_t insn_used;
  uint64_t mem_limit;          /* bytes */
  uint64_t mem_used;
  uint64_t mem_peak;
  int exceeded;
  /* Numeric bounds (Plan-2026-09 3.1). Stored here, enforced by the kernels
     when they land; 'max_elements' 0 means no limit. */
  uint64_t numeric_max_elements;
  dv_tier numeric_max_tier;
  int numeric_touched_fast;
};


uint32_t dv_abi_version (void) {
  return DV_ABI_VERSION;
}


int dv_build (void) {
  return DV_BUILD;
}


/*
** The feature string. Assembled by the preprocessor so it is one constant in
** the binary and the pointer is good for the life of the process.
**
** The unconditional four are here rather than left implicit because the reader
** of this string is a DRT profile line, and "what does this build carry" is
** answered badly by a list that only names the optional parts. The order is
** the header's contract; a new name is appended to its group, never inserted.
**
** Only facts this file can state exactly. Line editing and the threading arm
** are deliberately absent: both are chosen inside another translation unit
** ('dline.c', 'dsync.h'), and a condition retyped here would be a second
** definition free to drift from the first -- and 'dv.c' is compiled both
** standalone and inside the amalgamation, where the two would not even see
** the same macros. Neither is a property of an instance in any case.
*/
static const char dv_feature_string[] =
  "regex\n"
  "json\n"
  "msgpack\n"
  "snapshot"
#if defined(DV_NUMERIC)
  "\nnumeric"
#endif
  ;


const char *dv_features (void) {
  return dv_feature_string;
}


const char *dv_status_name (dv_status s) {
  switch (s) {
    case DV_OK: return "DV_OK";
    case DV_QUEUE_FULL: return "DV_QUEUE_FULL";
    case DV_QUEUE_DISABLED: return "DV_QUEUE_DISABLED";
    case DV_QUEUE_UNKNOWN: return "DV_QUEUE_UNKNOWN";
    case DV_QUEUE_EMPTY: return "DV_QUEUE_EMPTY";
    case DV_QUEUE_GONE: return "DV_QUEUE_GONE";
    case DV_IDLE: return "DV_IDLE";
    case DV_DONE: return "DV_DONE";
    case DV_ERROR: return "DV_ERROR";
    case DV_ABI_MISMATCH: return "DV_ABI_MISMATCH";
    case DV_SNAPSHOT_MISMATCH: return "DV_SNAPSHOT_MISMATCH";
    case DV_BUSY: return "DV_BUSY";
    case DV_BUFFER_TOO_SMALL: return "DV_BUFFER_TOO_SMALL";
    case DV_QUEUE_DROPPED: return "DV_QUEUE_DROPPED";
  }
  return "DV_UNKNOWN_STATUS";
}


/* ---------------------------------------------------------------- errors -- */

static void set_error (dv_instance *inst, const char *msg) {
  free(inst->error);
  inst->error = NULL;
  if (msg != NULL) {
    size_t n = strlen(msg);
    inst->error = (char *)malloc(n + 1);
    if (inst->error != NULL)
      memcpy(inst->error, msg, n + 1);
  }
}


/* Take the error off the thread's stack and keep a copy the host can read. */
/*
** Forget the last error, so 'dv_last_error' describes the step that just ran.
**
** dv.h already says the message is "valid until the next call on it", so this is
** conformance rather than a change of contract -- but nothing enforced it, and the
** buffer was in fact sticky. That made a *clean* exit indistinguishable from a
** faulted one to anything reading the error afterwards: the swarm layer decides
** between its "exited" and "faulted" events that way, so a supervisor restarted
** healthy children whenever they had recovered from an error earlier in their life.
**
** Called immediately before Lua runs, not at the top of the entry points, because
** the early returns there set errors of their own and a caller reading one after a
** repeated call should still see it.
*/
static void clear_error (dv_instance *inst) {
  free(inst->error);
  inst->error = NULL;
}


static void set_error_from (dv_instance *inst, lua_State *from) {
  const char *msg = lua_tostring(from, -1);
  set_error(inst, (msg != NULL) ? msg : "(error object is not a string)");
  lua_pop(from, 1);
}


const char *dv_last_error (dv_instance *inst) {
  return (inst != NULL) ? inst->error : NULL;
}


/*
** The traceback handler, installed on the thread where the error is raised so
** the trace describes the program's frames rather than this file's.
*/
static int dv_msghandler (lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  if (msg == NULL) {
    if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING)
      return 1;
    msg = lua_pushfstring(L, "(error object is a %s value)",
                          luaL_typename(L, 1));
  }
  luaL_traceback(L, L, msg, 1);
  return 1;
}


/* -------------------------------------------------------------- lifecycle -- */

/*
** The counting allocator. 9.4 lists "the allocator, already pluggable" as the
** memory mechanism, and this is that: refusing an allocation past the limit is
** reported by Lua as an ordinary out-of-memory error, which a program can even
** catch -- so a memory budget is a limit rather than an execution.
**
** The high-water mark is tracked as well as the current figure, because a
** supervisor deciding whether a child needs a larger budget wants the peak and not
** whatever happened to be live when it asked.
**
** **'osize' is only a size when 'ptr' is not NULL.** On a fresh allocation Lua
** passes the *type tag* of the object being made in that argument -- 'luaM_malloc_'
** in lmem.c calls 'firsttry(g, NULL, cast_sizet(tag), size)' -- and the tag is a
** small integer, not a byte count. Subtracting it anyway cost a few bytes of
** accounting per allocation, always in the same direction, so the counter drifted
** downward against the truth and a long-running instance eventually read zero while
** holding megabytes.
**
** That made 9.4's memory limit evadable rather than merely inaccurate: churn enough
** allocations and the counter is back at zero, and the next 'mem_limit' bytes are
** granted on top of everything already held. Repeating the churn repeats the grant.
** 'a_memory_budget_survives_allocation_churn' in dv_check.c is the test, and it
** fails on the previous line.
*/
static void *dv_alloc (void *ud, void *ptr, size_t osize, size_t nsize) {
  dv_instance *inst = (dv_instance *)ud;
  /* The old block's size, which is zero when there is no old block whatever the
     tag in 'osize' says. */
  uint64_t old = (ptr != NULL) ? (uint64_t)osize : 0u;
  if (nsize == 0) {
    free(ptr);
    if (inst != NULL) {
      inst->mem_used -= (old < inst->mem_used) ? old : inst->mem_used;
    }
    return NULL;
  }
  if (inst != NULL && inst->mem_limit != 0) {
    uint64_t after = inst->mem_used - old + (uint64_t)nsize;
    if (after > inst->mem_limit) {
      inst->exceeded = 1;
      return NULL;                /* Lua turns this into an out-of-memory error */
    }
  }
  {
    void *p = realloc(ptr, nsize);
    if (p == NULL)
      return NULL;
    if (inst != NULL) {
      inst->mem_used = inst->mem_used - old + (uint64_t)nsize;
      if (inst->mem_used > inst->mem_peak)
        inst->mem_peak = inst->mem_used;
    }
    return p;
  }
}


/* How many VM instructions between hook calls. */
#define DV_HOOK_STEP	1000

/*
** The instruction hook. Raises, never yields -- 9.4 says abort rather than
** schedule, and the reason is 10.7: a yield from a hook leaves CIST_HOOKYIELD on
** the frame and makes the instance uncapturable. Budgeting by yielding would have
** cost hibernation silently.
*/
static void dv_insn_hook (lua_State *L, lua_Debug *ar) {
  dv_instance *inst;
  (void)ar;
  lua_getfield(L, LUA_REGISTRYINDEX, "diluvium.instance");
  inst = (dv_instance *)lua_touserdata(L, -1);
  lua_pop(L, 1);
  if (inst == NULL)
    return;
  inst->insn_used += DV_HOOK_STEP;
  if (inst->insn_limit != 0 && inst->insn_used >= inst->insn_limit) {
    inst->exceeded = 1;
    /*
    ** The hook stays armed, and that is the whole point of this branch.
    **
    ** It used to clear itself here -- "once is enough; the error is on its
    ** way" -- which was true only if nothing caught the error. 'luaL_error'
    ** raises an ordinary Lua error, so a guest's own 'pcall' catches it, and
    ** with the hook already gone nothing re-armed it: 'dv_run' and
    ** 'dv_restore' are the only other sites that arm it and neither is
    ** reachable again on a running instance. Two lines of Lua switched the
    ** budget off permanently:
    **
    **   pcall(function() while true do end end)   -- trips it once
    **   while true do end                         -- then runs unbounded
    **
    ** 'insn_used' stopped advancing with the hook, so 'dv_usage' reported the
    ** instance sitting exactly at its limit while it ran on -- which blinded
    ** the one measurement a supervisor would have used to notice.
    **
    ** Left armed, the hook fires again DV_HOOK_STEP instructions later, so a
    ** catch cannot buy the program more than that before the next raise. The
    ** budget bounds the work again and the count keeps counting.
    **
    ** What this does NOT do, stated here because it is easy to assume
    ** otherwise: it does not make the error uncatchable, and it does not
    ** return control to the host. 'while true do pcall(f) end' still spins
    ** forever, taking DV_HOOK_STEP instructions per raise and never leaving
    ** 'dv_run'. Lua has no uncatchable error; bounding that needs either a
    ** 'pcall' that refuses to catch once 'exceeded' is set (a core-file patch,
    ** so an allowlist decision) or a process-level watchdog. See the tests in
    ** test/dv_check.c that fence this in.
    */
    luaL_error(L, "instruction budget of %I exceeded",
               (lua_Integer)inst->insn_limit);
  }
}


dv_status dv_set_budget (dv_instance *inst, uint64_t instructions,
                         uint64_t memory_kb) {
  if (inst == NULL)
    return DV_ERROR;
  if (inst->started) {
    set_error(inst, "dv_set_budget: the instance is already running; a budget "
                    "that changed mid-flight would make 'exceeded' mean nothing");
    return DV_BUSY;
  }
  inst->insn_limit = instructions;
  inst->mem_limit = memory_kb * 1024u;
  return DV_OK;
}


dv_status dv_usage (dv_instance *inst, uint64_t *instructions,
                    uint64_t *memory_kb) {
  if (inst == NULL)
    return DV_ERROR;
  if (instructions != NULL) *instructions = inst->insn_used;
  if (memory_kb != NULL) *memory_kb = inst->mem_peak / 1024u;
  return DV_OK;
}


/*
** The resting figure, which 'dv_usage' deliberately does not report. See dv.h for
** why both exist; the short version is that a peak in kilobytes answers "does this
** child need more" and cannot answer "what does an idle agent cost", which is the
** question a host sizing a swarm is actually asking.
*/
dv_status dv_memory (dv_instance *inst, uint64_t *bytes_now,
                     uint64_t *bytes_peak) {
  if (inst == NULL)
    return DV_ERROR;
  if (bytes_now != NULL) *bytes_now = inst->mem_used;
  if (bytes_peak != NULL) *bytes_peak = inst->mem_peak;
  return DV_OK;
}


int dv_exceeded (dv_instance *inst) {
  return (inst != NULL && inst->exceeded) ? 1 : 0;
}


/* ---------------------------------------------------------------- numeric -- */

/*
** The budget seam for numeric kernels (doc/Plan-2026-09.md 3.4).
**
** Kernels charge the instruction budget by element count and never by
** time: one instruction per 64 elements, checked at a block boundary. So
** 'exceeded' fires at the same element of the same kernel on every
** target, which is what makes a replay of a budget-exceeded run mean
** anything.
**
** Two calls rather than one because the alternative is a registry lookup
** per block. 'diluvium_budget_open' does that lookup once per kernel and
** hands back a cookie; 'diluvium_budget_charge' is then two additions and
** a compare. A cookie of NULL means this state is not running under an
** instance -- the standalone interpreter, or a host embedding Lua
** directly -- and charging it is a no-op rather than an error, because
** the kernels are the same code in both.
**
** Here rather than in dnumeric.c because 'dv_instance' is private to this
** file, and rather than in dv.h because that header is the published ABI
** and this is not part of it.
*/
LUA_API void *diluvium_budget_open (lua_State *L) {
  dv_instance *inst;
  lua_getfield(L, LUA_REGISTRYINDEX, "diluvium.instance");
  inst = (dv_instance *)lua_touserdata(L, -1);
  lua_pop(L, 1);
  return (void *)inst;
}


LUA_API void diluvium_budget_charge (lua_State *L, void *cookie,
                                     uint64_t n) {
  dv_instance *inst = (dv_instance *)cookie;
  if (inst == NULL)
    return;
  inst->insn_used += n;
  if (inst->insn_limit != 0 && inst->insn_used >= inst->insn_limit) {
    inst->exceeded = 1;
    /* The same error the instruction hook raises, for the same reason and
       catchable in the same way; see 'dv_insn_hook' above. */
    luaL_error(L, "instruction budget of %I exceeded",
               (lua_Integer)inst->insn_limit);
  }
}


/*
** A fast-tier kernel ran. Sticky; see the header for why.
*/
LUA_API void diluvium_numeric_touched (lua_State *L) {
  dv_instance *inst = (dv_instance *)diluvium_budget_open(L);
  if (inst != NULL)
    inst->numeric_touched_fast = 1;
}


/*
** Bytes per element, or 0 for a dtype this build does not know.
**
** The dispatch point for element types. Every place that has to reason about a
** dtype goes through here rather than switching again.
**
** Stage 2 added a 'c128' dtype and it is deliberately not here. 'dv.h' spells
** this call's dtype argument out as "0=f64 1=i64 2=u8" and session B compiled
** against that header at A0; a fourth number would move a published contract,
** and it would move it for a type no host has a buffer of -- complex data
** arrives as pairs of doubles, which is 'f64' and then 'array.complex'. So
** 'c128' lives in the guest library only, and 'diluvium_array_adopt' refuses
** it by name rather than by falling off this switch.
*/
static size_t dv_dtype_width (int dtype) {
  switch (dtype) {
    case DV_DTYPE_F64: return 8;
    case DV_DTYPE_I64: return 8;
    case DV_DTYPE_U8:  return 1;
    default: return 0;
  }
}


/*
** Which stack an adopted value is pushed onto.
**
** The instance's thread while there is one, because that is where a parked
** program's frames are and a hostcall reply is being assembled for it. Before
** the thread exists -- a host adopting into an instance it has loaded but not
** run -- the main state is the only stack there is.
*/
static lua_State *dv_valuestack (dv_instance *inst) {
  return (inst->co != NULL) ? inst->co : inst->L;
}


/*
** Take on bytes the instance holds that its allocator never saw.
**
** depth: the accounting, which is not decoration. Every other byte an instance
** holds arrives through 'dv_alloc' and is counted there. An adopted buffer
** does not -- the host allocated it outside the instance -- so 'dv_memory'
** would report an instance holding a gigabyte column as holding the hundred
** bytes of header that point at it, and dv.h's promise that adopted bytes
** count against the memory limit would be false exactly where it matters.
** This is the only place that promise is kept, and the credit in
** 'diluvium_memory_credit' below is the only place it is unwound.
*/
static void dv_charge_adopted (dv_instance *inst, uint64_t n) {
  inst->mem_used += n;
  if (inst->mem_used > inst->mem_peak)
    inst->mem_peak = inst->mem_used;
}


static void dv_release_adopted (dv_instance *inst, void *bytes, size_t len) {
  lua_Alloc allocf;
  void *ud;
  if (bytes == NULL)
    return;
  /*
  ** A balancing entry, and deliberately not 'dv_charge_adopted': the free
  ** below goes through 'dv_alloc', which subtracts 'len' from 'mem_used'
  ** whether or not anything ever added it, so without this the counter walks
  ** downward against the truth. The high-water mark must not move with it.
  ** These bytes were the host's and are about to be nobody's; the instance
  ** never held them as its own, and a peak that said otherwise would tell a
  ** supervisor sizing this child's budget to make room for a column that was
  ** copied or refused rather than kept.
  */
  inst->mem_used += (uint64_t)len;
  allocf = lua_getallocf(inst->L, &ud);
  allocf(ud, bytes, len, 0);
}


/*
** The seam the two ends of an adopted buffer's life reach this accounting
** through.
**
** 'dv_release_adopted' above balances within one call because it takes and
** frees in the same breath. A buffer that was really adopted cannot: the
** instance holds it from the handover until the guest's last reference to its
** array is collected, which is the whole point of adopting. So the charge is
** made where ownership is taken and the credit where it ends, and both of
** those are in dnumeric.c beside the 'owns' field they mirror -- see
** dnumeric.h for why they live there rather than here.
**
** The credit clamps rather than wrapping, exactly as 'dv_alloc' does on a
** free: a counter that went below zero would read as an enormous positive
** number and hand the instance an unlimited budget.
*/
LUA_API void diluvium_memory_charge (lua_State *L, uint64_t n) {
  dv_instance *inst = (dv_instance *)diluvium_budget_open(L);
  if (inst != NULL)
    dv_charge_adopted(inst, n);
}


LUA_API void diluvium_memory_credit (lua_State *L, uint64_t n) {
  dv_instance *inst = (dv_instance *)diluvium_budget_open(L);
  if (inst != NULL)
    inst->mem_used -= (n < inst->mem_used) ? n : inst->mem_used;
}


/*
** Both handovers, so they can run inside 'lua_pcall'. Arguments in, the value
** and a flag saying which handover happened out.
**
** depth: why this is protected at all. Every way the bytes can reach the guest
** allocates -- 'lua_newuserdatauv' for the array's header on the adopt path,
** 'lua_pushlstring' for the string on the copy path -- and an allocation that
** fails inside an unprotected C call has nowhere to throw to: 'luaD_throw'
** with no error jump calls the panic function, which aborts the process. So a
** host that budgeted an instance at 256 KB and handed it a 240 KB column did
** not get a refusal, it got no return at all. That is the one failure mode an
** embedding ABI must not have, because the host is the thing that was supposed
** to survive its guest running out of memory.
**
** Three plain arguments rather than upvalues because 'lua_pushcclosure' with
** upvalues allocates, and this runs precisely when allocation is what failed;
** a light C function, a light userdata and two integers are all stores into a
** stack slot. Same idiom as 'dv_save_body' further down.
**
** One invariant this rests on: nothing after 'diluvium_array_adopt' has taken
** ownership may raise. A raise there unwinds past this and the caller frees a
** buffer the array is already going to free, which is a double free rather
** than a refusal. 'lua_pushboolean' cannot raise; the charge at the end of
** 'diluvium_array_adopt' is the other statement inside that window, and
** dnumeric.h says why it cannot either.
*/
static int dv_adopt_body (lua_State *L) {
  void *bytes = lua_touserdata(L, 1);
  size_t len = (size_t)lua_tointeger(L, 2);
  int dtype = (int)lua_tointeger(L, 3);
  int adopted = (diluvium_array_adopt(L, dtype, len, bytes) == 0);
  if (!adopted)
    lua_pushlstring(L, (const char *)bytes, len);
  lua_pushboolean(L, adopted);
  return 2;
}


int dv_array_adopt (dv_instance *inst, int dtype, size_t len, void *bytes) {
  size_t width;
  lua_State *L;
  if (inst == NULL)
    return 1;
  width = dv_dtype_width(dtype);
  if (width == 0) {
    set_error(inst, "dv_array_adopt: unknown dtype");
    return 1;
  }
  if (len % width != 0) {
    set_error(inst, "dv_array_adopt: length is not a whole number of elements");
    return 1;
  }
  if (bytes == NULL && len != 0) {
    set_error(inst, "dv_array_adopt: NULL buffer with a non-zero length");
    return 1;
  }
  /*
  ** Every refusal above describes an argument and is the caller's to read.
  ** From here the arguments are known good, so whatever 'dv_last_error' says
  ** afterwards is about this handover and nothing earlier -- which is what
  ** makes it the flag for "1, and nothing was pushed". Not at the top of the
  ** function, for the reason 'clear_error' gives: the argument refusals set
  ** errors of their own and a caller reading one back should still find it.
  */
  clear_error(inst);
  L = dv_valuestack(inst);
  /*
  ** Adoption proper, where the feature is built: the buffer becomes the
  ** array's elements with no copy at all, and the array's finaliser is
  ** what releases it. 'diluvium_array_adopt' reports 1 when it did not
  ** take the buffer, which is every build without DV_NUMERIC and any
  ** state the library was never opened in; the copy into a string is what
  ** happens then. The bytes reach the guest either way, and the return
  ** value is how the caller learns which shape the guest is about to see,
  ** so nothing a host wrote has to change when the feature is turned on.
  **
  ** Both run in 'dv_adopt_body' under 'lua_pcall', and on the main state
  ** rather than on 'L'. Protected because either can fail; see the body.
  ** On the main state because 'L' is the parked thread whenever there is
  ** one, and lapi.c refuses a call on a suspended thread ("cannot do calls
  ** on non-normal thread"). The value crosses afterwards, which is a stack
  ** store and cannot fail once the room is reserved -- so the room is
  ** reserved on both stacks before anything is attempted.
  */
  {
    lua_State *M = inst->L;
    int base = lua_gettop(M);
    int adopted;
    const char *why = NULL;
    if (!lua_checkstack(M, 5) || (M != L && !lua_checkstack(L, 1)))
      why = "dv_array_adopt: the stack cannot grow enough to take the buffer";
    else {
      lua_pushcfunction(M, dv_adopt_body);
      lua_pushlightuserdata(M, bytes);
      lua_pushinteger(M, (lua_Integer)len);
      lua_pushinteger(M, (lua_Integer)dtype);
      if (lua_pcall(M, 3, 2, 0) != LUA_OK) {
        /* Only when it is already a string: converting the error object
           would allocate, and allocation is what just failed. */
        const char *msg = (lua_type(M, -1) == LUA_TSTRING)
                          ? lua_tostring(M, -1) : NULL;
        why = (msg != NULL) ? msg : "dv_array_adopt: the handover was refused";
      }
    }
    if (why != NULL) {
      /* Copied out before the stack is cut back, because 'why' may point
         into the error object sitting on it. */
      set_error(inst, why);
      lua_settop(M, base);
      dv_release_adopted(inst, bytes, len);
      return 1;
    }
    adopted = lua_toboolean(M, -1);
    lua_pop(M, 1);                        /* the flag; the value is on top */
    if (M != L)
      lua_xmove(M, L, 1);
    if (adopted)
      return 0;                 /* the array owns the buffer from here */
    dv_release_adopted(inst, bytes, len);
    return 1;
  }
}


void dv_numeric_set_max_elements (dv_instance *inst, uint64_t n) {
  if (inst != NULL)
    inst->numeric_max_elements = n;
}


void dv_numeric_set_max_tier (dv_instance *inst, dv_tier t) {
  if (inst == NULL)
    return;
  /* Clamp rather than refuse: the setter cannot report, and the weakest tier
     is the safe reading of a value this build does not recognise. */
  {
    int v = (int)t;
    if (v < (int)DV_TIER_EXACT) v = (int)DV_TIER_EXACT;
    if (v > (int)DV_TIER_FAST) v = (int)DV_TIER_FAST;
    inst->numeric_max_tier = (dv_tier)v;
  }
}


int dv_numeric_touched_fast (dv_instance *inst) {
  return (inst != NULL && inst->numeric_touched_fast) ? 1 : 0;
}


dv_instance *dv_new (const dv_config *cfg) {
  dv_instance *inst;
  if (cfg != NULL && cfg->abi_version != 0 &&
      cfg->abi_version != DV_ABI_VERSION)
    return NULL;  /* a stale binding, caught before it can misread anything */
  inst = (dv_instance *)calloc(1, sizeof(*inst));
  if (inst == NULL)
    return NULL;
  inst->chunk_ref = LUA_NOREF;
  inst->co_ref = LUA_NOREF;
  inst->flags = (cfg != NULL) ? cfg->flags : 0u;
  /* Not calloc's zero, which is DV_TIER_EXACT and would silently forbid every
     floating-point kernel in an instance nobody had configured. The weakest
     tier is the open default; a supervisor narrows it, never widens it. */
  inst->numeric_max_tier = DV_TIER_FAST;
  /* 'lua_newstate' rather than 'luaL_newstate', so the allocator is ours and a
     memory budget is possible at all. The instance is anchored in the registry
     because the instruction hook is handed a 'lua_State' and nothing else. */
  inst->L = lua_newstate(dv_alloc, inst, 0);
  if (inst->L == NULL) {
    free(inst);
    return NULL;
  }
  lua_pushlightuserdata(inst->L, inst);
  lua_setfield(inst->L, LUA_REGISTRYINDEX, "diluvium.instance");
  /* The standard libraries with 'debug' narrowed, then msgpack and queue,
     including inbox/outbox. 'DV_FLAG_UNSAFE_DEBUG' is the host saying the
     program is its own and it wants the whole library; see dlibs.c. */
  diluvium_openguestlibs(
    inst->L,
    ((inst->flags & DV_FLAG_UNSAFE_DEBUG) ? DILUVIUM_GUEST_FULL_DEBUG : 0u) |
    ((inst->flags & DV_FLAG_UNSAFE_STDLIB) ? DILUVIUM_GUEST_UNSAFE_STDLIB : 0u) |
    ((inst->flags & DV_FLAG_TEXT_ONLY) ? DILUVIUM_GUEST_TEXT_ONLY : 0u));
  /*
  ** Name this library's own C function, per 10.4: the message handler sits in the
  ** driver's frame on every instance's thread, so without a name no parked
  ** instance could be snapshotted at all.
  **
  ** Registered here, at creation, and not lazily when the thread is built --
  ** because the permanents *fingerprint* travels in the snapshot header and 10.4
  ** requires the set to be identical on save and restore. Registering it on first
  ** use made the set depend on whether an instance had run yet, so a snapshot from
  ** a started instance was refused by a fresh one with "the permanents set
  ** differs". Found exactly that way.
  */
  lua_pushcfunction(inst->L, dv_msghandler);
  diluvium_snap_addpermanent(inst->L, "dv.msghandler");
  return inst;
}


void dv_free (dv_instance *inst) {
  if (inst == NULL)
    return;
  if (inst->L != NULL)
    lua_close(inst->L);   /* closes the thread and releases every message */
  free(inst->error);
  free(inst);
}


dv_status dv_load (dv_instance *inst, const uint8_t *code, size_t len,
                   const char *name) {
  const char *mode;
  int st;
  /* The guard has to come before the first read of 'inst', which it did not: the
     'mode' initialiser dereferenced it two lines above this check, so a host that
     passed NULL crashed rather than being told. */
  if (inst == NULL || code == NULL)
    return DV_ERROR;
  mode = (inst->flags & DV_FLAG_TEXT_ONLY) ? "t" : "bt";
  if (inst->started) {
    set_error(inst, "dv_load: the program has already started");
    return DV_BUSY;
  }
  st = luaL_loadbufferx(inst->L, (const char *)code, len,
                        (name != NULL) ? name : "=(dv_load)", mode);
  if (st != LUA_OK) {
    set_error_from(inst, inst->L);
    return DV_ERROR;
  }
  luaL_unref(inst->L, LUA_REGISTRYINDEX, inst->chunk_ref);
  inst->chunk_ref = luaL_ref(inst->L, LUA_REGISTRYINDEX);
  return DV_OK;
}


/* ---------------------------------------------------------------- queues -- */

static dv_status from_q (int qstatus) {
  switch (qstatus) {
    case DILUVIUM_Q_OK: return DV_OK;
    case DILUVIUM_Q_FULL: return DV_QUEUE_FULL;
    case DILUVIUM_Q_DISABLED: return DV_QUEUE_DISABLED;
    case DILUVIUM_Q_EMPTY: return DV_QUEUE_EMPTY;
    case DILUVIUM_Q_DROPPED: return DV_QUEUE_DROPPED;
    case DILUVIUM_Q_GONE: return DV_QUEUE_GONE;
    default: return DV_QUEUE_UNKNOWN;
  }
}


dv_queue_id dv_queue_lookup (dv_instance *inst, const char *name) {
  if (inst == NULL || name == NULL)
    return 0;
  return (dv_queue_id)diluvium_queue_find(inst->L, name);
}


dv_status dv_queue_state (dv_instance *inst, dv_queue_id id,
                          dv_queue_info *out) {
  lua_Integer cap, len;
  int enabled, exported, direction, on_full;
  if (inst == NULL || out == NULL)
    return DV_ERROR;
  if (!diluvium_queue_stat(inst->L, (lua_Integer)id, &cap, &len,
                           &enabled, &exported, &direction, &on_full))
    return DV_QUEUE_UNKNOWN;
  out->capacity = (uint32_t)cap;
  out->len = (uint32_t)len;
  out->enabled = (uint8_t)enabled;
  out->exported = (uint8_t)exported;
  out->direction = (uint8_t)direction;
  out->on_full = (uint8_t)on_full;
  return DV_OK;
}


dv_status dv_queue_push (dv_instance *inst, dv_queue_id id,
                         const uint8_t *msgpack, size_t len) {
  if (inst == NULL || msgpack == NULL)
    return DV_ERROR;
  return from_q(diluvium_queue_push_bytes(inst->L, (lua_Integer)id,
                                          (const char *)msgpack, len));
}


dv_status dv_queue_pop (dv_instance *inst, dv_queue_id id,
                        uint8_t *buf, size_t cap, size_t *out_len) {
  const char *s;
  size_t n;
  int st;
  if (inst == NULL || out_len == NULL)
    return DV_ERROR;
  st = diluvium_queue_peek_bytes(inst->L, (lua_Integer)id, &s, &n);
  if (st != DILUVIUM_Q_OK)
    return from_q(st);
  *out_len = n;
  if (buf == NULL || cap < n) {
    /* Leave the message where it is. A host that guessed its buffer size can
       size it from '*out_len' and ask again; losing the message to a short
       read would be the one outcome it could not recover from. */
    diluvium_queue_peek_bytes(inst->L, (lua_Integer)id, &s, &n);  /* re-anchor */
    return DV_BUFFER_TOO_SMALL;
  }
  memcpy(buf, s, n);
  diluvium_queue_drop(inst->L, (lua_Integer)id);
  return DV_OK;
}


dv_status dv_queue_peek (dv_instance *inst, dv_queue_id id,
                         const uint8_t **ptr, size_t *out_len) {
  const char *s;
  size_t n;
  int st;
  if (inst == NULL || ptr == NULL || out_len == NULL)
    return DV_ERROR;
  st = diluvium_queue_peek_bytes(inst->L, (lua_Integer)id, &s, &n);
  if (st != DILUVIUM_Q_OK)
    return from_q(st);
  *ptr = (const uint8_t *)s;
  *out_len = n;
  return DV_OK;
}


void dv_queue_release (dv_instance *inst, dv_queue_id id) {
  if (inst != NULL)
    diluvium_queue_drop(inst->L, (lua_Integer)id);
}


/* Bridge the runtime's notification shape to the ABI's. */
static void dv_notify_bridge (lua_State *L, lua_Integer id, void *ud) {
  dv_instance *inst = (dv_instance *)ud;
  (void)L;
  if (inst != NULL && inst->notify != NULL)
    inst->notify(inst->notify_ud, (dv_queue_id)id);
}


void dv_set_notify (dv_instance *inst,
                    void (*cb)(void *ud, dv_queue_id id), void *ud) {
  if (inst == NULL)
    return;
  inst->notify = cb;
  inst->notify_ud = ud;
  diluvium_queue_setnotify(inst->L, (cb != NULL) ? dv_notify_bridge : NULL,
                           inst);
}


/* ------------------------------------------------------------- endpoints -- */

/* The host's callback, plus the instance, so the bridge can find both. */
static int dv_bind_bridge (const unsigned char *ref, size_t len,
                           unsigned int *token, void *ud) {
  dv_instance *inst = (dv_instance *)ud;
  if (inst == NULL || inst->endpoint_bind == NULL)
    return 0;
  return inst->endpoint_bind(inst->endpoint_ud, (const uint8_t *)ref, len,
                             token);
}


void dv_set_endpoint_handler (dv_instance *inst,
                              int (*bind)(void *ud, const uint8_t *ref,
                                          size_t len, uint32_t *token),
                              void *ud) {
  if (inst == NULL)
    return;
  inst->endpoint_bind = bind;
  inst->endpoint_ud = ud;
  diluvium_endpoint_sethandler(inst->L, (bind != NULL) ? dv_bind_bridge : NULL,
                               inst);
}


void dv_endpoint_allow (dv_instance *inst, const uint8_t *ref, size_t len,
                        uint32_t token) {
  if (inst == NULL || ref == NULL || token == 0)
    return;
  diluvium_endpoint_allow(inst->L, (const char *)ref, len,
                          (unsigned int)token);
}


dv_queue_id dv_endpoint_queue (dv_instance *inst, uint32_t token) {
  if (inst == NULL)
    return 0;
  return (dv_queue_id)diluvium_endpoint_queue(inst->L, (unsigned int)token);
}


dv_status dv_endpoint_close (dv_instance *inst, dv_queue_id id) {
  if (inst == NULL)
    return DV_ERROR;
  if (!diluvium_endpoint_setlive(inst->L, (lua_Integer)id, 0))
    return DV_QUEUE_UNKNOWN;
  return DV_OK;
}


/* ---------------------------------------------------------------- layout -- */

uint32_t dv_layout (uint32_t *out, size_t n) {
  /* Written with 'offsetof' rather than by hand, so this is measured by the
     compiler that built the runtime the binding is actually talking to. That is
     the whole point: on wasm32 these differ from the LP64 numbers a developer
     would get from running a test locally. */
  static const uint32_t table[DV_LAYOUT_COUNT] = {
    (uint32_t)sizeof(dv_config),
    (uint32_t)offsetof(dv_config, abi_version),
    (uint32_t)offsetof(dv_config, flags),
    (uint32_t)sizeof(dv_queue_info),
    (uint32_t)offsetof(dv_queue_info, capacity),
    (uint32_t)offsetof(dv_queue_info, len),
    (uint32_t)offsetof(dv_queue_info, enabled),
    (uint32_t)offsetof(dv_queue_info, exported),
    (uint32_t)offsetof(dv_queue_info, direction),
    (uint32_t)offsetof(dv_queue_info, on_full),
    (uint32_t)sizeof(dv_waitset),
    (uint32_t)offsetof(dv_waitset, n),
    (uint32_t)offsetof(dv_waitset, ids),
    (uint32_t)offsetof(dv_waitset, timeout_ms),
    (uint32_t)offsetof(dv_waitset, for_write)
  };
  size_t i;
  size_t want = (n < DV_LAYOUT_COUNT) ? n : DV_LAYOUT_COUNT;
  if (out == NULL)
    return DV_LAYOUT_COUNT;
  for (i = 0; i < want; i++)
    out[i] = table[i];
  return (uint32_t)want;
}


/* ------------------------------------------------------------ scheduling -- */

static void export_waitset (const diluvium_waitset *ws, dv_waitset *out);


/* Turn what 'lua_resume' said into what the host is told. */
static dv_status settle (dv_instance *inst, int status, int nres,
                         dv_waitset *out) {
  if (status == LUA_OK) {
    inst->finished = 1;
    inst->parked = 0;
    lua_settop(inst->co, 0);
    return DV_DONE;
  }
  if (status == LUA_YIELD) {
    if (!diluvium_queue_waitset(inst->co, nres, &inst->ws)) {
      /* Not a wait-set. A host cannot know what an ordinary top-level
         'coroutine.yield' was for, and guessing would be an invention. */
      lua_settop(inst->co, 0);
      inst->finished = 1;
      set_error(inst, "the program yielded something that is not a wait-set; "
                      "a top-level coroutine.yield has no host to answer it");
      return DV_ERROR;
    }
    inst->parked = 1;
    inst->pending = nres;
    if (out != NULL)
      export_waitset(&inst->ws, out);
    return DV_IDLE;
  }
  inst->finished = 1;
  inst->parked = 0;
  set_error_from(inst, inst->co);
  lua_settop(inst->co, 0);
  return DV_ERROR;
}


dv_status dv_run (dv_instance *inst, dv_waitset *out_waitset) {
  int status, nres;
  if (inst == NULL)
    return DV_ERROR;
  if (inst->finished)
    return DV_DONE;
  if (inst->parked) {
    /* The program is waiting to be told what happened. Running it again would
       mean answering on the host's behalf, which is exactly the decision this
       ABI declines to make. */
    set_error(inst, "dv_run: the program is parked; answer it with dv_resume");
    return DV_BUSY;
  }
  if (inst->started)
    return DV_BUSY;
  if (inst->chunk_ref == LUA_NOREF) {
    set_error(inst, "dv_run: nothing loaded");
    return DV_ERROR;
  }
  clear_error(inst);
  inst->co = lua_newthread(inst->L);
  inst->co_ref = luaL_ref(inst->L, LUA_REGISTRYINDEX);
  if (!lua_checkstack(inst->co, 4)) {
    set_error(inst, "dv_run: cannot grow the thread's stack");
    return DV_ERROR;
  }
  if (inst->insn_limit != 0) {
    /* Armed on the thread, not on the main state: the program runs there, and
       'diluvium_task_sethook' exists because a hook set on one does not follow
       the other. */
    lua_sethook(inst->co, dv_insn_hook, LUA_MASKCOUNT, DV_HOOK_STEP);
  }
  diluvium_task_pushbody(inst->co);
  lua_pushcfunction(inst->co, dv_msghandler);
  lua_rawgeti(inst->L, LUA_REGISTRYINDEX, inst->chunk_ref);
  lua_xmove(inst->L, inst->co, 1);
  inst->started = 1;
  status = lua_resume(inst->co, inst->L, 2, &nres);
  return settle(inst, status, nres, out_waitset);
}


/* Fill a host-facing wait-set from the one the runtime handed us. */
static void export_waitset (const diluvium_waitset *ws, dv_waitset *out) {
  int i;
  out->n = (uint32_t)ws->n;
  out->timeout_ms = (int64_t)ws->timeout_ms;
  out->for_write = (uint8_t)(ws->mode == DILUVIUM_WAIT_WRITE);
  for (i = 0; i < ws->n; i++)
    out->ids[i] = (dv_queue_id)ws->ids[i];
}


dv_status dv_waitset_get (dv_instance *inst, dv_waitset *out) {
  if (inst == NULL || out == NULL)
    return DV_ERROR;
  if (!inst->parked)
    return DV_BUSY;
  export_waitset(&inst->ws, out);
  return DV_OK;
}


dv_status dv_resume (dv_instance *inst, dv_queue_id fired) {
  int status, nres, why;
  if (inst == NULL)
    return DV_ERROR;
  if (inst->finished)
    return DV_DONE;
  if (!inst->parked) {
    set_error(inst, "dv_resume: the program is not parked");
    return DV_BUSY;
  }
  /*
  ** The host names a handle; the runtime works out what that means. A host
  ** should not have to model the difference between "a message arrived" and
  ** "that queue has gone away" -- it has the same information either way, and
  ** the queue itself is the authority.
  */
  if (fired == 0)
    why = DILUVIUM_FIRED_TIMEOUT;
  else {
    diluvium_waitset one;
    one.mode = inst->ws.mode;
    one.timeout_ms = 0;
    one.n = 1;
    one.ids[0] = (lua_Integer)fired;
    if (diluvium_queue_ready(inst->co, &one, &why) == 0) {
      /*
      ** The named handle is neither ready nor closed, so nothing has happened to
      ** it. This used to report a timeout, and that was a lie the guest could not
      ** detect: 6.3 defines "timeout" as "queue.wait elapsed", and a program that
      ** passed no timeout would receive one anyway -- then index the nil value it
      ** was handed, which is what any program written to the documented contract
      ** does next.
      **
      ** 'fired == 0' is already how a host says the timeout elapsed, so naming a
      ** live empty queue is either a host mistake or a race between two threads
      ** that both saw a message. Both are answered the same way: stay parked and
      ** report DV_IDLE, so the resume is a no-op the host may retry. Nothing is
      ** consumed -- the park's description stays on the stack and 'parked' stays
      ** set -- which is why this returns before the three lines below.
      **
      ** This is also what the CLI host has always done over the same protocol:
      ** dtask.c loops rather than synthesising a reason.
      */
      return DV_IDLE;
    }
  }
  clear_error(inst);
  lua_pop(inst->co, inst->pending);   /* the park's description */
  inst->pending = 0;
  inst->parked = 0;
  diluvium_queue_fire(inst->co, (lua_Integer)fired, why);
  status = lua_resume(inst->co, inst->L, 2, &nres);
  return settle(inst, status, nres, NULL);
}


/* ======================================================================
** Hibernate and wake (10.1, 10.6, 10.10)
** ====================================================================== */

/*
** A snapshot is taken of a *parked* instance, and that is not a limitation of
** this implementation -- it is 10.2. A program that is running has state on the
** machine's C stack, and nothing can write that down. A parked one has all of it
** in the thread.
**
** Everything below goes through a protected call, because the snapshot layer
** raises: it is written for a caller that can let an error escape, and the ABI is
** the one caller that cannot.
*/

static int dv_save_body (lua_State *L) {
  diluvium_snap_opts opts;
  const char *host = (const char *)lua_touserdata(L, 2);
  memset(&opts, 0, sizeof(opts));
  opts.host = host;
  opts.insn_used = (uint64_t)lua_tointeger(L, 3);
  diluvium_snap_save(L, 1, &opts);
  return 1;
}


dv_status dv_snapshot (dv_instance *inst, const char *host,
                       uint8_t *out, size_t cap, size_t *len) {
  lua_State *L;
  int base;
  size_t n;
  const char *bytes;
  if (len != NULL) *len = 0;
  if (inst == NULL)
    return DV_ERROR;
  L = inst->L;
  if (!inst->started || inst->finished) {
    set_error(inst, "dv_snapshot: nothing is parked; a snapshot is taken of a "
                    "program waiting on a queue, not of one that has not run or "
                    "has finished");
    return DV_ERROR;
  }
  if (!inst->parked) {
    set_error(inst, "dv_snapshot: the program is running; only a parked program "
                    "has all of its state written down");
    return DV_ERROR;
  }
  base = lua_gettop(L);
  lua_pushcfunction(L, dv_save_body);
  lua_rawgeti(L, LUA_REGISTRYINDEX, inst->co_ref);
  lua_pushlightuserdata(L, (void *)host);
  lua_pushinteger(L, (lua_Integer)inst->insn_used);
  if (lua_pcall(L, 3, 1, 0) != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    set_error(inst, (msg != NULL) ? msg : "dv_snapshot: refused");
    lua_settop(L, base);
    return DV_ERROR;
  }
  bytes = lua_tolstring(L, -1, &n);
  if (len != NULL) *len = n;
  if (out == NULL) {
    lua_settop(L, base);
    return DV_OK;                       /* size enquiry */
  }
  if (n > cap) {
    lua_settop(L, base);
    return DV_BUFFER_TOO_SMALL;
  }
  memcpy(out, bytes, n);
  lua_settop(L, base);
  return DV_OK;
}


dv_status dv_restore (dv_instance *inst, const char *host,
                      const uint8_t *s, size_t len) {
  lua_State *L;
  diluvium_snap_opts opts;
  const char *msg = NULL;
  int base, rc, nres;
  if (inst == NULL || s == NULL)
    return DV_ERROR;
  L = inst->L;
  if (inst->started || inst->chunk_ref != LUA_NOREF) {
    set_error(inst, "dv_restore: this instance has already been used; restore "
                    "into a fresh one");
    return DV_ERROR;
  }
  memset(&opts, 0, sizeof(opts));
  opts.host = host;
  base = lua_gettop(L);
  rc = diluvium_snap_load(L, &opts, (const char *)s, len, &msg);
  if (rc != DILUVIUM_SNAP_ACCEPT) {
    set_error(inst, (msg != NULL) ? msg : "dv_restore: refused");
    lua_settop(L, base);
    return (rc == DILUVIUM_SNAP_BAD_PAYLOAD) ? DV_ERROR : DV_SNAPSHOT_MISMATCH;
  }
  /*
  ** The budget carries on from where the snapshot left it (9.4: the budget is
  ** the program's, not one residency's). The counter comes out of the header
  ** the load just accepted, so the reader failing here means the header
  ** changed between the two reads -- refuse rather than wake unmetered. Read
  ** before the instance takes the thread, so the refusal at least leaves the
  ** instance unstarted; the load's queue state is already installed, which is
  ** one more reason this branch existing beats it ever running.
  */
  {
    uint64_t used = 0;
    if (!diluvium_snap_headerusage(L, (const char *)s, len, &used)) {
      set_error(inst, "dv_restore: the accepted header would not answer for "
                      "its instruction count");
      lua_settop(L, base);
      return DV_ERROR;
    }
    inst->insn_used = used;
  }
  /* The restored thread becomes this instance's, and the instance takes on the
     state the snapshot's was in: started, parked, with a wait-set to report. */
  inst->co = lua_tothread(L, -1);
  if (inst->co == NULL) {
    set_error(inst, "dv_restore: the snapshot layer accepted the bytes but "
                    "produced no thread");
    lua_settop(L, base);
    return DV_ERROR;
  }
  inst->co_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  /*
  ** The count hook is the other half of finding 1: 'dv_run' is the only other
  ** place that arms it, and a restored instance can never reach 'dv_run' --
  ** 'started' below makes it refuse -- so without this a woken instance keeps
  ** its limit readable and loses its enforcement. Armed on the thread, not the
  ** main state, for dv_run's reason: the program runs there. The ordering is
  ** forced, not conventional: 'dv_set_budget' refuses a started instance and
  ** this function marks it started, so set-budget-then-restore is the only
  ** order a host can write, and 'insn_limit' is already right here.
  */
  if (inst->insn_limit != 0)
    lua_sethook(inst->co, dv_insn_hook, LUA_MASKCOUNT, DV_HOOK_STEP);
  inst->started = 1;
  inst->finished = 0;
  inst->parked = 1;
  nres = diluvium_shim_nyield(inst->co);
  inst->pending = nres;
  memset(&inst->ws, 0, sizeof(inst->ws));
  if (!diluvium_queue_waitset(inst->co, nres, &inst->ws)) {
    /*
    ** Parked on something other than a queue -- a bare 'coroutine.yield', say.
    ** Restoring it is fine and resuming it is the host's business, but there is
    ** no wait-set to report, so say so rather than hand back a zeroed one that
    ** looks like a wait on nothing.
    */
    inst->ws.n = 0;
    inst->ws.timeout_ms = -1;
  }
  lua_settop(L, base);
  return DV_OK;
}


dv_status dv_register_code (dv_instance *inst, const uint8_t *code, size_t len,
                           const char *name) {
  lua_State *L;
  int base;
  if (inst == NULL || code == NULL)
    return DV_ERROR;
  L = inst->L;
  base = lua_gettop(L);
  if (luaL_loadbufferx(L, (const char *)code, len,
                       (name != NULL) ? name : "=registered",
                       (inst->flags & DV_FLAG_TEXT_ONLY) ? "t" : NULL)
      != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    set_error(inst, (msg != NULL) ? msg : "dv_register_code: would not load");
    lua_settop(L, base);
    return DV_ERROR;
  }
  if (!diluvium_snap_register(L, -1)) {
    set_error(inst, "dv_register_code: the chunk is not a Lua function");
    lua_settop(L, base);
    return DV_ERROR;
  }
  lua_settop(L, base);
  return DV_OK;
}
