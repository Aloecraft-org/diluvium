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
#include "dqueue.h"
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
  uint32_t flags;
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
  inst->L = luaL_newstate();
  if (inst->L == NULL) {
    free(inst);
    return NULL;
  }
  luaL_openlibs(inst->L);
  diluvium_openlibs(inst->L);   /* msgpack and queue, including inbox/outbox */
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
  const char *mode = (inst->flags & DV_FLAG_TEXT_ONLY) ? "t" : "bt";
  int st;
  if (inst == NULL || code == NULL)
    return DV_ERROR;
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
  inst->co = lua_newthread(inst->L);
  inst->co_ref = luaL_ref(inst->L, LUA_REGISTRYINDEX);
  if (!lua_checkstack(inst->co, 4)) {
    set_error(inst, "dv_run: cannot grow the thread's stack");
    return DV_ERROR;
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
    if (diluvium_queue_ready(inst->co, &one, &why) == 0)
      why = DILUVIUM_FIRED_TIMEOUT;  /* named a handle that is not ready */
  }
  lua_pop(inst->co, inst->pending);   /* the park's description */
  inst->pending = 0;
  inst->parked = 0;
  diluvium_queue_fire(inst->co, (lua_Integer)fired, why);
  status = lua_resume(inst->co, inst->L, 2, &nres);
  return settle(inst, status, nres, NULL);
}
