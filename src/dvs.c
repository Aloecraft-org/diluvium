/*
** dvs.c
** The swarm layer. See dvs.h.
**
** Nothing here includes 'lua.h' or any core header, and that is the layer boundary
** rather than a coincidence -- 4.1 puts this in a separate library above the
** instance ABI, and the moment this file needs a 'lua_State' something has been put
** in the wrong place. The one thing it reads that is not a 'dv_' call is the
** msgpack token cursor, which the codec exposes precisely so this file does not
** need a second parser or a state to run one in.
**
** On who declares the system queues. 6.1 says queues are declared by the guest and
** never by the host, and that still holds here: a program that wants to supervise
** declares 'system/lifecycle' itself, and this layer drains it only if the instance
** holds the lifecycle capability. A program without the capability may declare the
** queue and nothing will ever read it -- which is exactly 9.3's "an agent submits a
** request to a reviewer because it holds no capability for the target queue",
** arrived at by mechanism rather than by a special case.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmsgpack.h"
#include "dvs.h"


/* Defaults, used when the caller passes 0. Not "no limit": see dvs.h. */
#define DVS_DEFAULT_MAX		256
#define DVS_DEFAULT_SPAWN_RATE	8

/* The capability that gates 'system/lifecycle'. */
#define DVS_CAP_LIFECYCLE	"lifecycle"

#define DVS_MAX_CAPS		32
#define DVS_MAX_CAP_LEN		96

/* How many messages may wait for a non-resident instance, and how long a queue
   name may be. Bounded on purpose: see dvs.h on 'dvs_push'. */
#define DVS_MAX_PENDING		16
#define DVS_MAX_QNAME		64


/*
** One message held for a non-resident instance. The queue is recorded by *name*
** rather than by handle, because handles belong to a 'dv_instance' and the whole
** point is that there is not one right now -- the instance the message is delivered
** into is a different one, restored from bytes, and only the name survives that.
*/
typedef struct dvs_pending {
  char queue[DVS_MAX_QNAME];
  uint8_t *msg;
  size_t len;
} dvs_pending;


typedef struct dvs_slot {
  dvs_id id;                    /* 0 when the slot is free */
  dvs_id parent;
  dv_instance *inst;            /* NULL while the instance is cached */
  void *ctx;                    /* whatever the host's 'create' returned */
  char *caps[DVS_MAX_CAPS];
  size_t ncaps;
  uint64_t instructions;
  uint64_t memory_kb;
  int wake_on_message;
  int alive;
  int started;
  /* The cache. 'snap' is the instance's whole state while it is not resident, and
     'pend' is what arrived for it in the meantime, oldest first. */
  uint8_t *snap;
  size_t snaplen;
  dvs_pending pend[DVS_MAX_PENDING];
  size_t npend;
} dvs_slot;


struct dvs_swarm {
  dvs_host host;
  dvs_slot *slots;
  uint32_t nslots;
  dvs_id next_id;
  uint32_t spawn_rate;
  uint32_t spawns_this_step;
  char error[512];
};


uint32_t dvs_abi_version (void) {
  return DVS_ABI_VERSION;
}


const char *dvs_status_name (dvs_status s) {
  switch (s) {
    case DVS_OK: return "DVS_OK";
    case DVS_ERROR: return "DVS_ERROR";
    case DVS_UNKNOWN: return "DVS_UNKNOWN";
    case DVS_DENIED: return "DVS_DENIED";
    case DVS_LIMIT: return "DVS_LIMIT";
    case DVS_GONE: return "DVS_GONE";
    default: return "DVS_?";
  }
}


static void set_error (dvs_swarm *sw, const char *fmt, ...) {
  va_list ap;
  if (sw == NULL) return;
  va_start(ap, fmt);
  vsnprintf(sw->error, sizeof(sw->error), fmt, ap);
  va_end(ap);
}


const char *dvs_last_error (dvs_swarm *sw) {
  return (sw != NULL && sw->error[0] != '\0') ? sw->error : NULL;
}


static dvs_slot *find (dvs_swarm *sw, dvs_id id) {
  uint32_t i;
  if (sw == NULL || id == 0)
    return NULL;
  for (i = 0; i < sw->nslots; i++) {
    if (sw->slots[i].id == id)
      return &sw->slots[i];
  }
  return NULL;
}


dvs_swarm *dvs_new (const dvs_host *host, uint32_t max_instances,
                    uint32_t spawns_per_step) {
  dvs_swarm *sw;
  if (host == NULL || host->drive == NULL)
    return NULL;                /* a swarm that cannot drive anything is a bug */
  sw = (dvs_swarm *)calloc(1, sizeof(*sw));
  if (sw == NULL)
    return NULL;
  sw->host = *host;
  sw->nslots = (max_instances != 0) ? max_instances : DVS_DEFAULT_MAX;
  sw->spawn_rate = (spawns_per_step != 0) ? spawns_per_step
                                          : DVS_DEFAULT_SPAWN_RATE;
  sw->slots = (dvs_slot *)calloc(sw->nslots, sizeof(dvs_slot));
  if (sw->slots == NULL) {
    free(sw);
    return NULL;
  }
  sw->next_id = 1;
  return sw;
}


/* Drop everything the cache is holding for a slot. */
static void drop_cache (dvs_slot *sl) {
  size_t i;
  free(sl->snap);
  sl->snap = NULL;
  sl->snaplen = 0;
  for (i = 0; i < sl->npend; i++)
    free(sl->pend[i].msg);
  sl->npend = 0;
}


static void release (dvs_swarm *sw, dvs_slot *sl) {
  size_t i;
  if (sl->ctx != NULL && sw->host.destroy != NULL)
    sw->host.destroy(sw->host.ud, sl->id, sl->ctx);
  sl->ctx = NULL;
  if (sl->inst != NULL)
    dv_free(sl->inst);
  sl->inst = NULL;
  drop_cache(sl);
  for (i = 0; i < sl->ncaps; i++)
    free(sl->caps[i]);
  sl->ncaps = 0;
  sl->alive = 0;
  sl->id = 0;                   /* the slot is free; the handle is not reused */
}


void dvs_free (dvs_swarm *sw) {
  uint32_t i;
  if (sw == NULL)
    return;
  for (i = 0; i < sw->nslots; i++) {
    if (sw->slots[i].id != 0)
      release(sw, &sw->slots[i]);
  }
  free(sw->slots);
  free(sw);
}


/* ======================================================================
** Capabilities (9.3)
** ====================================================================== */

/*
** Does 'held' imply 'want'?
**
** Exact match, or a trailing '*' that covers a prefix -- 6.6 says names are
** namespaced with '/' and that "capability scoping over name patterns is future
** work and must not be designed away", so this implements the one pattern the
** design already shows ("queue:work/*") and nothing more. A '*' in the middle is
** not a pattern, it is a literal, because inventing a glob here would be designing
** the future work rather than leaving room for it.
*/
static int implies (const char *held, const char *want) {
  size_t n = strlen(held);
  if (n > 0 && held[n - 1] == '*')
    return strncmp(held, want, n - 1) == 0;
  return strcmp(held, want) == 0;
}


int dvs_holds (dvs_swarm *sw, dvs_id id, const char *cap) {
  dvs_slot *sl = find(sw, id);
  size_t i;
  if (sl == NULL || cap == NULL)
    return 0;
  for (i = 0; i < sl->ncaps; i++) {
    if (implies(sl->caps[i], cap))
      return 1;
  }
  return 0;
}


int dvs_may_grant (dvs_swarm *sw, dvs_id parent, const char *cap) {
  /*
  ** Attenuation only, no exceptions (9.3). Granting is holding: a parent may pass
  ** on anything it holds, exactly or narrowed, and nothing else. This is what makes
  ** a privilege hierarchy structural instead of conventional.
  */
  return dvs_holds(sw, parent, cap);
}


/* Copy a capability list into a slot. 0 if any of it is refused. */
static int setcaps (dvs_swarm *sw, dvs_slot *sl, const char *const *caps,
                    size_t ncaps) {
  size_t i;
  if (ncaps > DVS_MAX_CAPS) {
    set_error(sw, "a capability set of %lu is beyond the %d this layer holds",
              (unsigned long)ncaps, DVS_MAX_CAPS);
    return 0;
  }
  for (i = 0; i < ncaps; i++) {
    size_t n;
    if (caps[i] == NULL)
      return 0;
    n = strlen(caps[i]);
    if (n == 0 || n >= DVS_MAX_CAP_LEN) {
      set_error(sw, "a capability name of %lu characters is not usable",
                (unsigned long)n);
      return 0;
    }
    sl->caps[i] = (char *)malloc(n + 1);
    if (sl->caps[i] == NULL)
      return 0;
    memcpy(sl->caps[i], caps[i], n + 1);
    sl->ncaps = i + 1;
  }
  return 1;
}


/* ======================================================================
** The instance table (9.1.2 items 1 and 2)
** ====================================================================== */

static dvs_slot *claim (dvs_swarm *sw) {
  uint32_t i;
  for (i = 0; i < sw->nslots; i++) {
    if (sw->slots[i].id == 0) {
      memset(&sw->slots[i], 0, sizeof(dvs_slot));
      sw->slots[i].id = sw->next_id++;
      return &sw->slots[i];
    }
  }
  return NULL;
}


/* Make, load and budget an instance. The host's 'create' comes after. */
static dvs_status build (dvs_swarm *sw, dvs_slot *sl, const dvs_spawn *req) {
  dv_config cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.abi_version = DV_ABI_VERSION;
  sl->inst = dv_new(&cfg);
  if (sl->inst == NULL) {
    set_error(sw, "could not create an instance");
    return DVS_ERROR;
  }
  /* Budget before load, and load before running: 'dv_set_budget' refuses a
     running instance, and a program that allocates at load time should be inside
     its memory budget while doing so. */
  if (req->instructions != 0 || req->memory_kb != 0) {
    if (dv_set_budget(sl->inst, req->instructions, req->memory_kb) != DV_OK) {
      set_error(sw, "could not set a budget: %s", dv_last_error(sl->inst));
      return DVS_ERROR;
    }
  }
  if (dv_load(sl->inst, (const uint8_t *)req->code, req->code_len, "=agent")
      != DV_OK) {
    set_error(sw, "the program would not load: %s", dv_last_error(sl->inst));
    return DVS_ERROR;
  }
  sl->instructions = req->instructions;
  sl->memory_kb = req->memory_kb;
  sl->wake_on_message = req->wake_on_message;
  sl->alive = 1;
  if (sw->host.create != NULL)
    sl->ctx = sw->host.create(sw->host.ud, sl->id, sl->inst);
  return DVS_OK;
}


dvs_status dvs_root (dvs_swarm *sw, const char *code, size_t code_len,
                     const char *const *caps, size_t ncaps,
                     uint64_t instructions, uint64_t memory_kb, dvs_id *out) {
  dvs_slot *sl;
  dvs_spawn req;
  dvs_status st;
  if (out != NULL) *out = 0;
  if (sw == NULL || code == NULL)
    return DVS_ERROR;
  sl = claim(sw);
  if (sl == NULL) {
    set_error(sw, "the instance table is full (%u)", sw->nslots);
    return DVS_LIMIT;
  }
  if (!setcaps(sw, sl, caps, ncaps)) {
    release(sw, sl);
    return DVS_ERROR;
  }
  memset(&req, 0, sizeof(req));
  req.code = code;
  req.code_len = code_len;
  req.instructions = instructions;
  req.memory_kb = memory_kb;
  sl->parent = 0;               /* the root has none, so its set is the ceiling */
  st = build(sw, sl, &req);
  if (st != DVS_OK) {
    release(sw, sl);
    return st;
  }
  if (out != NULL) *out = sl->id;
  return DVS_OK;
}


dv_instance *dvs_instance (dvs_swarm *sw, dvs_id id) {
  dvs_slot *sl = find(sw, id);
  return (sl != NULL) ? sl->inst : NULL;
}


dvs_id dvs_parent (dvs_swarm *sw, dvs_id id) {
  dvs_slot *sl = find(sw, id);
  return (sl != NULL) ? sl->parent : 0;
}


int dvs_alive (dvs_swarm *sw) {
  uint32_t i;
  int n = 0;
  if (sw == NULL)
    return 0;
  for (i = 0; i < sw->nslots; i++) {
    if (sw->slots[i].id != 0 && sw->slots[i].alive)
      n++;
  }
  return n;
}


/* ======================================================================
** Events (9.2)
** ====================================================================== */

/*
** Push an event into an instance's 'system/events', if it declared one.
**
** Monitor semantics only, one-directional: 9.2 says do not implement Erlang-style
** bidirectional links, and this is where that restraint lives. A parent hears about
** a child; a child hears nothing about a parent. Links can be layered on later by a
** program, and a program cannot un-layer them.
**
** Encoded by hand rather than through the codec, because building a four-field map
** does not need a Lua state and reaching for one here would put this file on the
** wrong side of 4.1. The shapes are fixed and small; anything richer belongs in the
** message a program sends, not in an event this layer synthesises.
*/
static void put_str (char **p, char *end, const char *s) {
  size_t n = strlen(s);
  if (*p + 1 + n > end)
    return;
  if (n < 32) {
    *(*p)++ = (char)(0xa0 | n);
  }
  else {
    if (*p + 2 + n > end) return;
    *(*p)++ = (char)0xd9;
    *(*p)++ = (char)n;
  }
  memcpy(*p, s, n);
  *p += n;
}


static void put_uint (char **p, char *end, uint64_t v) {
  if (v < 128) {
    if (*p + 1 > end) return;
    *(*p)++ = (char)v;
    return;
  }
  if (*p + 5 > end) return;
  *(*p)++ = (char)0xce;
  *(*p)++ = (char)((v >> 24) & 0xff);
  *(*p)++ = (char)((v >> 16) & 0xff);
  *(*p)++ = (char)((v >> 8) & 0xff);
  *(*p)++ = (char)(v & 0xff);
}


static void emit_event (dvs_swarm *sw, dvs_id to, const char *what,
                        dvs_id about, const char *detail) {
  dvs_slot *sl = find(sw, to);
  dv_queue_id q;
  char buf[512];
  char *p = buf;
  char *end = buf + sizeof(buf);
  if (sl == NULL || sl->inst == NULL)
    return;
  q = dv_queue_lookup(sl->inst, "system/events");
  if (q == 0)
    return;                     /* it did not declare one; nothing to say */
  *p++ = (char)(0x80 | (detail != NULL ? 3 : 2));
  put_str(&p, end, "event");
  put_str(&p, end, what);
  put_str(&p, end, "id");
  put_uint(&p, end, about);
  if (detail != NULL) {
    put_str(&p, end, "detail");
    put_str(&p, end, detail);
  }
  /*
  ** A full events queue is not an error and not a retry. 6.1's only guarantee is
  ** that push reports whether the message was accepted, and a supervisor that does
  ** not drain its events has chosen to miss them -- which this layer must not
  ** decide to fix, because "what to do about a missed event" is policy.
  */
  dv_queue_push(sl->inst, q, (const uint8_t *)buf, (size_t)(p - buf));
}


/* ======================================================================
** Subtree kill (9.5)
** ====================================================================== */

static void kill_subtree (dvs_swarm *sw, dvs_id id, int notify_parent) {
  dvs_slot *sl = find(sw, id);
  uint32_t i;
  dvs_id parent;
  if (sl == NULL)
    return;
  parent = sl->parent;
  /*
  ** Children first, and by repeated scan rather than a recursive walk: the table
  ** is flat, a subtree can be as deep as delegation went, and 9.1.1 says depth is
  ** unbounded. A recursive walk would meet the C stack at a depth a program is
  ** entitled to reach.
  */
  for (;;) {
    int found = 0;
    for (i = 0; i < sw->nslots; i++) {
      if (sw->slots[i].id != 0 && sw->slots[i].parent == id) {
        kill_subtree(sw, sw->slots[i].id, 0);
        found = 1;
        break;
      }
    }
    if (!found)
      break;
  }
  release(sw, sl);
  if (notify_parent && parent != 0)
    emit_event(sw, parent, "exited", id, "killed");
}


dvs_status dvs_kill (dvs_swarm *sw, dvs_id id) {
  dvs_slot *sl = find(sw, id);
  if (sl == NULL)
    return DVS_UNKNOWN;
  kill_subtree(sw, id, 1);
  return DVS_OK;
}


/* ======================================================================
** The snapshot cache (9.1.2 item 6)
** ====================================================================== */

int dvs_resident (dvs_swarm *sw, dvs_id id) {
  dvs_slot *sl = find(sw, id);
  return (sl != NULL && sl->alive && sl->inst != NULL);
}


size_t dvs_cached_size (dvs_swarm *sw, dvs_id id) {
  dvs_slot *sl = find(sw, id);
  return (sl != NULL) ? sl->snaplen : 0;
}


dvs_status dvs_hibernate (dvs_swarm *sw, dvs_id id) {
  dvs_slot *sl = find(sw, id);
  size_t need = 0;
  uint8_t *buf;
  if (sl == NULL || !sl->alive)
    return DVS_GONE;
  if (sl->inst == NULL)
    return DVS_OK;              /* already cached; asking twice is not an error */
  /*
  ** Size first, then write. 'dv_snapshot' answers the size for a NULL buffer, which
  ** is the only honest way to allocate for something whose size is the reachable
  ** value graph -- a guess large enough for the common case is a guess that fails on
  ** the agent that matters.
  */
  if (dv_snapshot(sl->inst, NULL, NULL, 0, &need) != DV_OK) {
    set_error(sw, "instance %u will not hibernate: %s", (unsigned)id,
              dv_last_error(sl->inst) ? dv_last_error(sl->inst) : "not parked");
    return DVS_ERROR;
  }
  buf = (uint8_t *)malloc(need != 0 ? need : 1);
  if (buf == NULL) {
    set_error(sw, "no memory for a %lu-byte snapshot", (unsigned long)need);
    return DVS_ERROR;
  }
  if (dv_snapshot(sl->inst, NULL, buf, need, &sl->snaplen) != DV_OK) {
    set_error(sw, "instance %u will not hibernate: %s", (unsigned)id,
              dv_last_error(sl->inst) ? dv_last_error(sl->inst) : "?");
    free(buf);
    return DVS_ERROR;
  }
  sl->snap = buf;
  /*
  ** The host's context goes with the instance, because it was created for that
  ** instance and 11.5 gives the host no way to reattach one. Waking calls 'create'
  ** again, which is the same contract a spawn has.
  */
  if (sl->ctx != NULL && sw->host.destroy != NULL)
    sw->host.destroy(sw->host.ud, sl->id, sl->ctx);
  sl->ctx = NULL;
  dv_free(sl->inst);
  sl->inst = NULL;
  return DVS_OK;
}


dvs_status dvs_wake (dvs_swarm *sw, dvs_id id) {
  dvs_slot *sl = find(sw, id);
  dv_instance *inst;
  dv_config cfg;
  size_t i;
  if (sl == NULL || !sl->alive)
    return DVS_GONE;
  if (sl->inst != NULL)
    return DVS_OK;              /* already resident */
  if (sl->snap == NULL) {
    set_error(sw, "instance %u has no cached snapshot", (unsigned)id);
    return DVS_ERROR;
  }
  memset(&cfg, 0, sizeof(cfg));
  cfg.abi_version = DV_ABI_VERSION;
  inst = dv_new(&cfg);
  if (inst == NULL) {
    set_error(sw, "could not create an instance to wake into");
    return DVS_ERROR;
  }
  /* The budget goes back on before the restore, for the same reason it goes on
     before a load: 'dv_set_budget' refuses a running instance, and a woken agent
     that had a budget must not come back without one. */
  if (sl->instructions != 0 || sl->memory_kb != 0)
    dv_set_budget(inst, sl->instructions, sl->memory_kb);
  if (dv_restore(inst, NULL, sl->snap, sl->snaplen) != DV_OK) {
    set_error(sw, "instance %u will not restore: %s", (unsigned)id,
              dv_last_error(inst) ? dv_last_error(inst) : "?");
    dv_free(inst);
    return DVS_ERROR;
  }
  sl->inst = inst;
  free(sl->snap);
  sl->snap = NULL;
  sl->snaplen = 0;
  /*
  ** The buffer drains here, before this call returns and therefore before any live
  ** push can reach the instance -- which is 8.4's "drain the buffer ahead of live
  ** pushes", and it is a consequence of where the drain sits rather than of any
  ** ordering logic. A message that is refused now is dropped rather than kept: the
  ** queue it names is bounded, the sender was already told DVS_OK, and holding it
  ** for a queue that has no room would be an unbounded buffer wearing a different
  ** name.
  */
  for (i = 0; i < sl->npend; i++) {
    dv_queue_id q = dv_queue_lookup(inst, sl->pend[i].queue);
    if (q != 0)
      dv_queue_push(inst, q, sl->pend[i].msg, sl->pend[i].len);
    free(sl->pend[i].msg);
    sl->pend[i].msg = NULL;
  }
  sl->npend = 0;
  if (sw->host.create != NULL)
    sl->ctx = sw->host.create(sw->host.ud, sl->id, inst);
  return DVS_OK;
}


dvs_status dvs_push (dvs_swarm *sw, dvs_id id, const char *queue,
                     const void *msg, size_t len) {
  dvs_slot *sl = find(sw, id);
  dvs_pending *p;
  size_t nlen;
  if (queue == NULL || msg == NULL)
    return DVS_ERROR;
  /*
  ** 8.4: a push to a dead or unknown instance is "gone", immediately and without
  ** blocking. There is nothing to wait for and saying so late helps nobody.
  **
  ** 'find' does most of this by itself, because 'release' clears the handle and a
  ** handle is never reused -- removing '!sl->alive' turns no test red, and that is
  ** worth writing down rather than leaving as an apparently load-bearing check. It
  ** guards the window between 'claim' and 'build', where a slot has a handle and no
  ** instance yet.
  */
  if (sl == NULL || !sl->alive)
    return DVS_GONE;
  if (sl->inst != NULL) {
    dv_queue_id q = dv_queue_lookup(sl->inst, queue);
    if (q == 0)
      return DVS_UNKNOWN;
    return (dv_queue_push(sl->inst, q, (const uint8_t *)msg, len) == DV_OK)
           ? DVS_OK : DVS_LIMIT;
  }
  /* Cached. Without 'wake_on_message' this is "gone" -- an agent that did not ask
     to be woken is, from a sender's point of view, not there. */
  if (!sl->wake_on_message)
    return DVS_GONE;
  nlen = strlen(queue);
  if (nlen == 0 || nlen >= DVS_MAX_QNAME)
    return DVS_ERROR;
  if (sl->npend >= DVS_MAX_PENDING)
    return DVS_LIMIT;
  p = &sl->pend[sl->npend];
  p->msg = (uint8_t *)malloc(len != 0 ? len : 1);
  if (p->msg == NULL)
    return DVS_ERROR;
  memcpy(p->msg, msg, len);
  p->len = len;
  memcpy(p->queue, queue, nlen + 1);
  sl->npend++;
  return DVS_OK;
}


/* ======================================================================
** Draining system/lifecycle (9.1.2 item 4)
** ====================================================================== */

/* Read a string field into 'out'; 1 on success. */
static int field_str (const char *msg, size_t len, const char *key,
                      char *out, size_t cap) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  diluvium_mp_open(&c, msg, len);
  if (!diluvium_mp_field(&c, key) || !diluvium_mp_read(&c, &t) ||
      t.kind != DILUVIUM_MP_STR || t.len >= cap)
    return 0;
  memcpy(out, t.p, t.len);
  out[t.len] = '\0';
  return 1;
}


static int field_int (const char *msg, size_t len, const char *key,
                      uint64_t *out) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  diluvium_mp_open(&c, msg, len);
  if (!diluvium_mp_field(&c, key) || !diluvium_mp_read(&c, &t))
    return 0;
  if (t.kind == DILUVIUM_MP_INT && t.i >= 0) { *out = (uint64_t)t.i; return 1; }
  /* A budget written as 5e6 arrives as a float, which is what 9.1's own example
     shows -- so accepting one is reading the document rather than being lax. */
  if (t.kind == DILUVIUM_MP_FLOAT && t.f >= 0 && t.f < 1e18) {
    *out = (uint64_t)t.f;
    return 1;
  }
  return 0;
}


static int field_bool (const char *msg, size_t len, const char *key) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  diluvium_mp_open(&c, msg, len);
  if (!diluvium_mp_field(&c, key) || !diluvium_mp_read(&c, &t))
    return 0;
  return (t.kind == DILUVIUM_MP_BOOL) ? t.b : 0;
}


/* Read the 'caps' array. Returns the count, or -1 if the field is malformed. */
static int field_caps (const char *msg, size_t len,
                       char store[DVS_MAX_CAPS][DVS_MAX_CAP_LEN]) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  size_t i;
  int n = 0;
  diluvium_mp_open(&c, msg, len);
  if (!diluvium_mp_field(&c, "caps"))
    return 0;                   /* absent is an empty set, not an error */
  if (!diluvium_mp_read(&c, &t))
    return -1;
  /*
  ** An empty Lua table is a map on the wire, not an array -- 'mp_is_array' requires
  ** at least one element, and the codec's comment says so. So 'caps = {}', which is
  ** the obvious way for a program to say "no capabilities", arrives as an empty map
  ** and has to be read as an empty set rather than as malformed. A non-empty map is
  ** still an error: that is a table with names in it, and a request whose caps field
  ** is the wrong shape should be refused rather than silently taken as nothing.
  */
  if (t.kind == DILUVIUM_MP_MAP && t.len == 0)
    return 0;
  if (t.kind != DILUVIUM_MP_ARRAY)
    return -1;
  if (t.len > DVS_MAX_CAPS)
    return -1;
  for (i = 0; i < t.len; i++) {
    diluvium_mp_token e;
    if (!diluvium_mp_read(&c, &e) || e.kind != DILUVIUM_MP_STR ||
        e.len >= DVS_MAX_CAP_LEN)
      return -1;
    memcpy(store[n], e.p, e.len);
    store[n][e.len] = '\0';
    n++;
  }
  return n;
}


static void do_spawn (dvs_swarm *sw, dvs_slot *parent, const char *msg,
                      size_t len) {
  char caps[DVS_MAX_CAPS][DVS_MAX_CAP_LEN];
  const char *capv[DVS_MAX_CAPS];
  char code[8192];
  int ncaps, i;
  uint64_t insns = 0, mem = 0;
  dvs_slot *sl;
  dvs_spawn req;
  if (sw->spawns_this_step >= sw->spawn_rate) {
    /*
    ** 9.5's rate limit. 'drain' already declines to take a spawn request off the
    ** queue once the limit is reached, so in the normal path this is unreachable;
    ** it stays because a limit whose only enforcement lives in the caller is one
    ** refactor away from not being enforced at all.
    */
    emit_event(sw, parent->id, "denied", 0, "spawn rate limit");
    return;
  }
  if (!field_str(msg, len, "code", code, sizeof(code))) {
    emit_event(sw, parent->id, "denied", 0, "no code in the spawn request");
    return;
  }
  ncaps = field_caps(msg, len, caps);
  if (ncaps < 0) {
    emit_event(sw, parent->id, "denied", 0, "malformed caps");
    return;
  }
  /*
  ** Attenuation, before anything is built. 9.3 admits no exceptions, and refusing
  ** here rather than after creating the instance means a denied spawn costs nothing
  ** and leaves nothing behind.
  */
  for (i = 0; i < ncaps; i++) {
    if (!dvs_may_grant(sw, parent->id, caps[i])) {
      emit_event(sw, parent->id, "denied", 0, caps[i]);
      return;
    }
  }
  field_int(msg, len, "instructions", &insns);
  field_int(msg, len, "memory_kb", &mem);
  sl = claim(sw);
  if (sl == NULL) {
    emit_event(sw, parent->id, "denied", 0, "the instance table is full");
    return;
  }
  for (i = 0; i < ncaps; i++)
    capv[i] = caps[i];
  if (!setcaps(sw, sl, capv, (size_t)ncaps)) {
    release(sw, sl);
    emit_event(sw, parent->id, "denied", 0, "capabilities refused");
    return;
  }
  sl->parent = parent->id;
  memset(&req, 0, sizeof(req));
  req.code = code;
  req.code_len = strlen(code);
  req.caps = capv;
  req.ncaps = (size_t)ncaps;
  req.instructions = insns;
  req.memory_kb = mem;
  req.wake_on_message = field_bool(msg, len, "wake_on_message");
  if (build(sw, sl, &req) != DVS_OK) {
    dvs_id gone = sl->id;
    release(sw, sl);
    emit_event(sw, parent->id, "faulted", gone, sw->error);
    return;
  }
  sw->spawns_this_step++;
  emit_event(sw, parent->id, "spawned", sl->id, NULL);
}


static void do_kill (dvs_swarm *sw, dvs_slot *parent, const char *msg,
                     size_t len) {
  uint64_t target = 0;
  dvs_slot *sl;
  dvs_id walk;
  if (!field_int(msg, len, "id", &target) || target == 0) {
    emit_event(sw, parent->id, "denied", 0, "no id in the kill request");
    return;
  }
  sl = find(sw, (dvs_id)target);
  if (sl == NULL) {
    emit_event(sw, parent->id, "denied", (dvs_id)target, "no such instance");
    return;
  }
  /*
  ** Only an ancestor may kill. Parentage is the only relation this layer knows
  ** (9.1.2 item 2), and "any instance may kill any other" would make the
  ** capability set meaningless -- a child could kill its own supervisor.
  */
  for (walk = sl->parent; walk != 0; walk = dvs_parent(sw, walk)) {
    if (walk == parent->id) {
      kill_subtree(sw, (dvs_id)target, 1);
      return;
    }
  }
  emit_event(sw, parent->id, "denied", (dvs_id)target, "not a descendant");
}


/*
** '{op = "hibernate"}' swaps the requester out; with an 'id', a descendant.
**
** Self by default, because 10.1 makes hibernation self-initiated and a program is
** the only thing that knows when it is at a point it is willing to stop at. A
** supervisor may hibernate something below it, on the same ancestry rule as kill --
** but it can only do so while that instance is parked, so the descendant still
** chooses the moment by choosing when to park.
*/
static void do_hibernate (dvs_swarm *sw, dvs_slot *parent, const char *msg,
                          size_t len) {
  uint64_t target = 0;
  dvs_id who = parent->id;
  dvs_slot *subject;
  if (field_int(msg, len, "id", &target) && target != 0 &&
      (dvs_id)target != parent->id) {
    dvs_slot *sl = find(sw, (dvs_id)target);
    dvs_id walk;
    if (sl == NULL) {
      emit_event(sw, parent->id, "denied", (dvs_id)target, "no such instance");
      return;
    }
    for (walk = sl->parent; walk != 0; walk = dvs_parent(sw, walk)) {
      if (walk == parent->id)
        break;
    }
    if (walk == 0) {
      emit_event(sw, parent->id, "denied", (dvs_id)target, "not a descendant");
      return;
    }
    who = (dvs_id)target;
  }
  /*
  ** 'wake_on_message' may be set here as well as at spawn time, and this is the
  ** better place for it: 8.4 makes waking a property of the destination, and the
  ** program going to sleep is what knows whether it wants to be woken. A spawn-time
  ** flag is the parent's guess. Absent, the spawn-time value stands.
  */
  subject = find(sw, who);
  if (subject != NULL && field_bool(msg, len, "wake_on_message"))
    subject->wake_on_message = 1;
  if (dvs_hibernate(sw, who) != DVS_OK) {
    /* The event goes to the requester and not to the subject: a program that could
       not be swapped out is still running and will read its own queues, but the
       thing that needs to know is whatever asked. */
    emit_event(sw, parent->id, "denied", who, sw->error);
    return;
  }
  /* Only tell the parent, and only when it is not the subject -- an instance that
     hibernated itself is about to stop reading, and an event it will not see until
     it wakes is one that arrives out of order with whatever woke it. */
  if (who != parent->id)
    emit_event(sw, parent->id, "hibernated", who, NULL);
  else if (parent->parent != 0)
    emit_event(sw, parent->parent, "hibernated", who, NULL);
}


static void do_query (dvs_swarm *sw, dvs_slot *parent, const char *msg,
                      size_t len) {
  uint64_t target = 0;
  dvs_slot *sl;
  char detail[128];
  uint64_t insns = 0, mem = 0;
  if (!field_int(msg, len, "id", &target)) {
    emit_event(sw, parent->id, "denied", 0, "no id in the query");
    return;
  }
  sl = find(sw, (dvs_id)target);
  if (sl == NULL) {
    emit_event(sw, parent->id, "gone", (dvs_id)target, NULL);
    return;
  }
  dv_usage(sl->inst, &insns, &mem);
  snprintf(detail, sizeof(detail), "%s insns=%lu mem_kb=%lu",
           sl->alive ? "alive" : "dead", (unsigned long)insns,
           (unsigned long)mem);
  emit_event(sw, parent->id, "status", (dvs_id)target, detail);
}


/* Drain one instance's lifecycle queue. */
static void drain (dvs_swarm *sw, dvs_slot *sl) {
  dv_queue_id q;
  if (sl->inst == NULL || !sl->alive)
    return;
  q = dv_queue_lookup(sl->inst, "system/lifecycle");
  if (q == 0)
    return;                     /* it never declared one */
  /*
  ** The capability check is here and not at declare time, and that is 9.3 working
  ** by mechanism: a program without the lifecycle capability may declare the queue
  ** and write to it all it likes, and nothing will ever read it. There is no
  ** special case refusing the declaration, and no error to catch and work around.
  */
  if (!dvs_holds(sw, sl->id, DVS_CAP_LIFECYCLE))
    return;
  for (;;) {
    uint8_t buf[8192];
    size_t n = 0;
    char op[32];
    {
      const uint8_t *p = NULL;
      size_t len = 0;
      if (dv_queue_peek(sl->inst, q, &p, &len) != DV_OK)
        return;
      if (len > sizeof(buf)) {
        dv_queue_release(sl->inst, q);
        emit_event(sw, sl->id, "denied", 0, "the request is too large");
        continue;
      }
      memcpy(buf, p, len);
      n = len;
      /*
      ** The op is read before the message is taken off the queue, because a spawn
      ** that the rate limit will refuse must stay where it is. 9.5 rate-limits the
      ** lifecycle capability; a limit that consumed the request and answered
      ** "denied" would turn a burst of ten into three spawns and seven denials, and
      ** the denials would themselves overrun a bounded 'system/events'. Leaving the
      ** request queued makes it a rate rather than a filter: the burst arrives over
      ** the next few steps, in order, and nothing is lost.
      **
      ** One "throttled" event is emitted so the requester can back off -- once per
      ** step rather than once per request, for the same reason.
      */
      if (field_str((const char *)buf, n, "op", op, sizeof(op)) &&
          strcmp(op, "spawn") == 0 && sw->spawns_this_step >= sw->spawn_rate) {
        emit_event(sw, sl->id, "throttled", 0, "spawn rate limit");
        return;
      }
      dv_queue_release(sl->inst, q);
    }
    if (!field_str((const char *)buf, n, "op", op, sizeof(op))) {
      emit_event(sw, sl->id, "denied", 0, "no op in the request");
      continue;
    }
    if (strcmp(op, "spawn") == 0)
      do_spawn(sw, sl, (const char *)buf, n);
    else if (strcmp(op, "kill") == 0)
      do_kill(sw, sl, (const char *)buf, n);
    else if (strcmp(op, "query") == 0)
      do_query(sw, sl, (const char *)buf, n);
    else if (strcmp(op, "hibernate") == 0)
      do_hibernate(sw, sl, (const char *)buf, n);
    else
      emit_event(sw, sl->id, "denied", 0, op);
    /* A drain that acted on a request may have freed this slot -- a supervisor can
       kill its own subtree, and 'kill_subtree' does not spare the caller's
       children. Re-check before going round again. */
    if (sl->id == 0 || !sl->alive || sl->inst == NULL)
      return;
  }
}


/* ======================================================================
** One step
** ====================================================================== */

int dvs_step (dvs_swarm *sw) {
  uint32_t i;
  if (sw == NULL)
    return 0;
  sw->spawns_this_step = 0;
  /*
  ** Lifecycle first, then driving. A spawn requested this step therefore starts
  ** running in the same step, which is what makes a supervisor's restart feel
  ** immediate rather than arriving a step late -- and 'spawns_this_step' is what
  ** keeps that from being a fork bomb's best friend.
  **
  ** The slot array is walked by index and re-checked, because acting on a request
  ** can free slots, including ones ahead of this position.
  */
  /*
  ** Waking comes first, before either draining or driving, so that a woken instance
  ** gets a whole step in the same 'dvs_step' the message arrived in rather than one
  ** step later. 8.4 calls the restore asynchronous, and it is -- the sender got
  ** DVS_OK and went on -- but "asynchronous" should not mean "a step late for no
  ** reason".
  **
  ** A wake that fails is fatal to the instance and reported as a fault, because the
  ** alternative is a handle that is alive, non-resident and permanently unreachable:
  ** every later push would be buffered against a snapshot that will never restore.
  */
  for (i = 0; i < sw->nslots; i++) {
    dvs_slot *sl = &sw->slots[i];
    if (sl->id == 0 || !sl->alive || sl->inst != NULL || sl->npend == 0)
      continue;
    if (dvs_wake(sw, sl->id) != DVS_OK) {
      dvs_id gone = sl->id, parent = sl->parent;
      /* As wide as 'sw->error' itself: 'kill_subtree' below clears the slot and
         'emit_event' overwrites the swarm's error buffer, so the reason has to be
         copied out first -- and copying it into something narrower would truncate
         exactly the restore failures whose message is the longest. */
      char why[sizeof(sw->error)];
      snprintf(why, sizeof(why), "%s", sw->error);
      kill_subtree(sw, gone, 0);
      if (parent != 0)
        emit_event(sw, parent, "faulted", gone, why);
    }
  }
  for (i = 0; i < sw->nslots; i++) {
    if (sw->slots[i].id != 0 && sw->slots[i].alive)
      drain(sw, &sw->slots[i]);
  }
  for (i = 0; i < sw->nslots; i++) {
    dvs_slot *sl = &sw->slots[i];
    dvs_id id;
    int keep;
    /* A cached instance is not driven: there is nothing to drive. It is still
       alive, and 'dvs_alive' counts it, because a handle that names a snapshot is
       a handle a sender may legitimately push to. */
    if (sl->id == 0 || !sl->alive || sl->inst == NULL)
      continue;
    id = sl->id;
    keep = sw->host.drive(sw->host.ud, id, sl->inst, sl->ctx);
    sl = find(sw, id);          /* driving can free slots too */
    if (sl == NULL)
      continue;
    if (!keep) {
      dvs_id parent = sl->parent;
      int faulted = (dv_last_error(sl->inst) != NULL);
      int over = dv_exceeded(sl->inst);
      /*
      ** Three reasons an instance stops, and they are reported as three different
      ** events because a supervisor's response to each is different: a clean exit
      ** may need nothing, a fault may need a restart, and an exceeded budget may
      ** need a larger one. Collapsing them into "it stopped" would push that
      ** decision back into this layer, which is where 9.1.2 says it must not be.
      */
      const char *what = over ? "exceeded" : faulted ? "faulted" : "exited";
      const char *detail = faulted ? dv_last_error(sl->inst) : NULL;
      kill_subtree(sw, id, 0);
      if (parent != 0)
        emit_event(sw, parent, what, id, detail);
    }
  }
  return dvs_alive(sw);
}
