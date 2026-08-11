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
};


uint32_t dv_abi_version (void) {
  return DV_ABI_VERSION;
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
*/
static void *dv_alloc (void *ud, void *ptr, size_t osize, size_t nsize) {
  dv_instance *inst = (dv_instance *)ud;
  if (nsize == 0) {
    free(ptr);
    if (inst != NULL) {
      inst->mem_used -= (osize < inst->mem_used) ? osize : inst->mem_used;
    }
    return NULL;
  }
  if (inst != NULL && inst->mem_limit != 0) {
    uint64_t after = inst->mem_used - (uint64_t)osize + (uint64_t)nsize;
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
      inst->mem_used = inst->mem_used - (uint64_t)osize + (uint64_t)nsize;
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
    lua_sethook(L, NULL, 0, 0);   /* once is enough; the error is on its way */
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


int dv_exceeded (dv_instance *inst) {
  return (inst != NULL && inst->exceeded) ? 1 : 0;
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
    ((inst->flags & DV_FLAG_UNSAFE_STDLIB) ? DILUVIUM_GUEST_UNSAFE_STDLIB : 0u));
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
