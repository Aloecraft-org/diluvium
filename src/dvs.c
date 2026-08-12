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
/*
** The largest lifecycle request 'drain' will read, which is in practice the
** largest program a spawn can carry.
**
** It was 8192 and undocumented, which is a poor combination for a design whose
** 9.1 says code arrives as a message: a request over the limit is answered with
** "the request is too large" at run time and nothing warns before that.
** a coordinator program reached 7791 bytes in practice, so four lines of
** comment were enough to push a spawn over and bring the swarm up with one
** instance instead of eight -- which is how this number got noticed at all.
**
** Two of these are live at once in the worst case (this and 'do_spawn''s copy),
** so it is bounded by what is reasonable on a stack rather than by what a
** program might want. A host that needs more than this should be sending a
** reference to code rather than the code.
*/
#define DVS_MAX_REQUEST		32768

#define DVS_MAX_PENDING		16
#define DVS_MAX_QNAME		64


/*
** What a spawn asked for, as 'build' needs it.
**
** Internal. It was in dvs.h with a comment saying it was "handed to the host so it
** can size its own context", and that was simply false: the vtable's 'create'
** receives (ud, id, inst) and no public function takes one of these. A public
** struct that nothing public accepts is a reader's dead end, so it moved here.
** What a host actually wanted from it -- the budget, the capability list -- is now
** 'dvs_budget' and 'dvs_caps'.
**
** It no longer carries the capability list either. 'setcaps' has already put those
** on the slot by the time 'build' runs, so a second copy in the request was a field
** 'dvs_root' never filled in and nothing ever read: two ways to say one thing, one
** of them wrong. Removed rather than populated.
*/
typedef struct dvs_spawn {
  const char *code;             /* the program's source or bytecode */
  size_t code_len;
  uint64_t instructions;        /* budget, 0 for none */
  uint64_t memory_kb;
  int wake_on_message;
} dvs_spawn;


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
  /* The dv_config flags this instance was built with. Carried on the slot
     because 9.3's attenuation applies to them like everything else: a child
     inherits its parent's set and may narrow it, never widen it. */
  uint32_t flags;
  int wake_on_message;
  int alive;
  int started;
  int doomed;                   /* scratch, used only by 'kill_subtree' */
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
  int allow_hibernation;        /* on by default; a host may opt out: dvs.h */
  char *host_identity;          /* the stamp for 10.10; NULL = unstamped */
  uint32_t unsafe_stdlib;       /* DV_FLAG_UNSAFE_STDLIB, or 0. See dvs.h. */
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
  sw->allow_hibernation = 1;
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
  free(sw->host_identity);
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
  /*
  ** A trailing '*' is a prefix wildcard, and the prefix must be non-empty. A bare
  ** "*" would otherwise match every capability there is, via 'strncmp(held, want,
  ** 0)', which returns 0 for any pair of strings -- and that is worse than a
  ** god-mode capability, because it can be reached by *attenuation*: a parent
  ** holding "**" implies "*" (prefix "*" matches the want "*"), so it could grant a
  ** child the bare "*" and the child would then hold everything the parent did not.
  ** 9.3 says a grant may only narrow, so a rule that lets one widen is a break in
  ** the model rather than a sharp edge. A one-character "*" is therefore a literal
  ** name, matching only itself.
  */
  if (n > 1 && held[n - 1] == '*')
    return strncmp(held, want, n - 1) == 0;
  return strcmp(held, want) == 0;
}


dvs_status dvs_budget (dvs_swarm *sw, dvs_id id, uint64_t *instructions,
                       uint64_t *memory_kb) {
  dvs_slot *sl = find(sw, id);
  if (sl == NULL)
    return DVS_UNKNOWN;
  if (instructions != NULL) *instructions = sl->instructions;
  if (memory_kb != NULL) *memory_kb = sl->memory_kb;
  /* Answered for a cached instance too: the budget outlives residency, and a host
     deciding whether it can afford to wake something needs it precisely then. */
  return DVS_OK;
}


size_t dvs_caps (dvs_swarm *sw, dvs_id id, const char **out, size_t max) {
  dvs_slot *sl = find(sw, id);
  size_t i;
  if (sl == NULL)
    return 0;
  for (i = 0; i < sl->ncaps && i < max; i++)
    out[i] = sl->caps[i];
  return sl->ncaps;              /* the true count, so max == 0 sizes a buffer */
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
  /* The slot's own set, which the caller has already attenuated against its
     parent's. Before this the config was zeroed here and the parent's flags were
     never consulted at all, so a sealed supervisor spawned unsealed children --
     the one authority in this layer that did not narrow. */
  cfg.flags = sl->flags;
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
  sl->flags = sw->unsafe_stdlib;
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
    /*
    ** str8 carries one length byte, so 255 is the most it can describe. Writing
    ** '(char)n' for a longer string truncated the length and then copied the whole
    ** thing: a message claiming 44 bytes followed by 300 of them. Clamp the string
    ** instead of lying about it, and clamp again to the room left, so the header
    ** below always matches what actually gets written.
    */
    if (n > 255) n = 255;
    if (*p + 2 + n > end) {
      if ((size_t)(end - *p) < 3) return;
      n = (size_t)(end - *p) - 2;
    }
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
  char det[192];
  char *p = buf;
  char *end = buf + sizeof(buf);
  if (sl == NULL || sl->inst == NULL)
    return;
  q = dv_queue_lookup(sl->inst, "system/events");
  if (q == 0)
    return;                     /* it did not declare one; nothing to say */
  /*
  ** The detail is copied into a bounded buffer before the map header is written,
  ** and that ordering is the fix rather than a tidiness: the header commits to a
  ** pair count, 'put_str' returns silently when it runs out of room, and a fault
  ** detail is a Lua error with a traceback, which is easily longer than this whole
  ** buffer.
  **
  ** Honest limits on this one. The fault path cannot actually reach it: 'dv_last_
  ** error' is itself bounded to about 192 bytes, so no test drives a longer detail
  ** through here, and removing either this copy or 'put_str''s clamp turns nothing
  ** red. The reachable trigger is 'sw->error', which is 512 bytes and is passed as a
  ** detail by 'do_hibernate'. Both are kept anyway, and the copy is kept in
  ** preference to relying on the other buffer's size: the invariant "the pair count
  ** matches what was written" should be checkable in this function, not by reading
  ** another file and hoping it does not change. 192 bytes always fits alongside the
  ** keys and is under str8's 255-byte ceiling.
  */
  if (detail != NULL) {
    size_t i = 0;
    while (i + 1 < sizeof(det) && detail[i] != '\0') { det[i] = detail[i]; i++; }
    det[i] = '\0';
    detail = det;
  }
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

/*
** Kill an instance and everything below it, without recursion.
**
** The comment that used to be here claimed exactly that -- "by repeated scan rather
** than a recursive walk ... a recursive walk would meet the C stack at a depth a
** program is entitled to reach" -- above a body that called itself once per level.
** 9.1.1 says delegation depth is unbounded, and a guest can build the chain, so the
** claim was right and the code was not.
**
** Mark and sweep instead. Marking is a fixed point over a flat table: a slot is
** doomed if it is the target or if its parent is doomed, and passes repeat until one
** changes nothing. That terminates at nslots passes at worst and cannot recurse; it
** also cannot hang on a parentage cycle, which should not exist but which nothing
** currently prevents.
*/
static void kill_subtree (dvs_swarm *sw, dvs_id id, int notify_parent) {
  dvs_slot *sl = find(sw, id);
  uint32_t i;
  dvs_id parent;
  if (sl == NULL)
    return;
  parent = sl->parent;
  for (i = 0; i < sw->nslots; i++)
    sw->slots[i].doomed = 0;
  sl->doomed = 1;
  for (;;) {
    int changed = 0;
    for (i = 0; i < sw->nslots; i++) {
      dvs_slot *s = &sw->slots[i];
      if (s->id == 0 || s->doomed || s->parent == 0)
        continue;
      if (find(sw, s->parent) != NULL && find(sw, s->parent)->doomed) {
        s->doomed = 1;
        changed = 1;
      }
    }
    if (!changed)
      break;
  }
  /* Release after marking, never during: 'release' clears the handle, so a slot
     freed mid-walk would break the parent lookups the walk still depends on. */
  for (i = 0; i < sw->nslots; i++) {
    if (sw->slots[i].id != 0 && sw->slots[i].doomed)
      release(sw, &sw->slots[i]);
  }
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


void dvs_allow_hibernation (dvs_swarm *sw, int allow) {
  if (sw != NULL)
    sw->allow_hibernation = allow ? 1 : 0;
}


dvs_status dvs_set_host_identity (dvs_swarm *sw, const char *identity) {
  char *copy = NULL;
  if (sw == NULL)
    return DVS_ERROR;
  if (identity != NULL && identity[0] != '\0') {
    size_t n = strlen(identity) + 1;
    copy = (char *)malloc(n);
    if (copy == NULL) {
      /* Refused rather than silently unstamped: a stamp that quietly failed
         to apply is the advisory stamping 10.10 exists to rule out. */
      set_error(sw, "no memory to hold the host identity");
      return DVS_ERROR;
    }
    memcpy(copy, identity, n);
  }
  free(sw->host_identity);
  sw->host_identity = copy;     /* NULL when clearing: "" and NULL both mean
                                   unstamped, and the header writes "" for
                                   either, so one spelling is stored */
  return DVS_OK;
}


void dvs_allow_unsafe_stdlib (dvs_swarm *sw, int allow) {
  if (sw != NULL)
    sw->unsafe_stdlib = allow ? DV_FLAG_UNSAFE_STDLIB : 0u;
}


dvs_status dvs_hibernate (dvs_swarm *sw, dvs_id id) {
  dvs_slot *sl = find(sw, id);
  size_t need = 0;
  uint8_t *buf;
  if (sl == NULL || !sl->alive)
    return DVS_GONE;
  /*
  ** On by default since the profile-C block closed (doc/Hibernate.md); this
  ** branch is the host's opt-out. The message names who turned it off, because
  ** "DVS_ERROR" on a call that works everywhere else is exactly the sort of
  ** thing a host wastes an afternoon on.
  */
  if (!sw->allow_hibernation) {
    set_error(sw, "hibernation is switched off for this swarm: this host "
                  "called dvs_allow_hibernation(sw, 0). Call it with 1 to "
                  "switch it back on.");
    return DVS_ERROR;
  }
  if (sl->inst == NULL)
    return DVS_OK;              /* already cached; asking twice is not an error */
  /*
  ** Size first, then write. 'dv_snapshot' answers the size for a NULL buffer, which
  ** is the only honest way to allocate for something whose size is the reachable
  ** value graph -- a guess large enough for the common case is a guess that fails on
  ** the agent that matters.
  */
  if (dv_snapshot(sl->inst, sw->host_identity, NULL, 0, &need) != DV_OK) {
    set_error(sw, "instance %u will not hibernate: %s", (unsigned)id,
              dv_last_error(sl->inst) ? dv_last_error(sl->inst) : "not parked");
    return DVS_ERROR;
  }
  buf = (uint8_t *)malloc(need != 0 ? need : 1);
  if (buf == NULL) {
    set_error(sw, "no memory for a %lu-byte snapshot", (unsigned long)need);
    return DVS_ERROR;
  }
  if (dv_snapshot(sl->inst, sw->host_identity, buf, need, &sl->snaplen) != DV_OK) {
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
  /* The same set it was captured under. A snapshot does not cross
     DV_FLAG_UNSAFE_STDLIB anyway -- the permanents fingerprint differs -- so
     waking into a different set would turn a wake into a refusal. */
  cfg.flags = sl->flags;
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
  if (dv_restore(inst, sw->host_identity, sl->snap, sl->snaplen) != DV_OK) {
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
/*
** Read a string field. 'outlen', when given, receives the true byte count.
**
** The NUL termination is a convenience for the fields that really are text (an op
** name, a capability); the length is what a caller must use for anything that may
** contain a zero byte. 'do_spawn' used 'strlen' on the result and so truncated a
** *compiled chunk* at its first zero byte -- bytecode is full of them -- then
** reported the spawn as successful, with the child running a prefix of its program.
*/
static int field_str (const char *msg, size_t len, const char *key,
                      char *out, size_t cap, size_t *outlen) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  diluvium_mp_open(&c, msg, len);
  if (!diluvium_mp_field(&c, key) || !diluvium_mp_read(&c, &t) ||
      t.kind != DILUVIUM_MP_STR || t.len >= cap)
    return 0;
  memcpy(out, t.p, t.len);
  out[t.len] = '\0';
  if (outlen != NULL)
    *outlen = t.len;
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


/*
** Read an instance handle.
**
** Separate from 'field_int' because a handle is a 'dvs_id', which is 32 bits, and
** the wire carries 64. Casting the wider value was a real bug rather than an
** untidiness: a request naming 0x100000002 truncated to 2, 'find' located instance
** 2, and the request was carried out against **a different live instance** -- a kill
** that destroyed the wrong subtree, reported as success. A value that does not
** survive the round trip is refused instead.
**
** Zero is refused here too, because 0 is documented as never a valid handle, so
** every caller would otherwise repeat the check.
*/
static int field_id (const char *msg, size_t len, const char *key, dvs_id *out) {
  uint64_t v = 0;
  if (!field_int(msg, len, key, &v))
    return 0;
  if (v == 0 || v > (uint64_t)0xffffffffu)
    return 0;
  *out = (dvs_id)v;
  return (uint64_t)*out == v;   /* states the round trip rather than assuming it */
}


/*
** The budget, read the way 9.1 writes it.
**
**   budget = { instructions = 5e6, memory_kb = 512 }
**
** Nested, and 'do_spawn' read flat top-level 'instructions' and 'memory_kb' -- so a
** request written exactly as the document shows it gave the child **no budget at
** all**, silently, which then let a runaway child hang 'dvs_step' with nothing to
** stop it. The document is the contract here; the code was wrong.
**
** The flat form is still accepted, because it is what this layer has understood
** until now and someone may already have written it. Nested wins where both appear,
** since that is the documented spelling.
*/
static void field_budget (const char *msg, size_t len, uint64_t *insns,
                          uint64_t *mem) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  field_int(msg, len, "instructions", insns);   /* the flat form, if present */
  field_int(msg, len, "memory_kb", mem);
  diluvium_mp_open(&c, msg, len);
  if (!diluvium_mp_field(&c, "budget"))
    return;
  /* The cursor is on the value; 'diluvium_mp_field' reads a map header at the
     current position, so looking inside is the same call again -- which is why the
     cursor's entry points compose without a second parser. */
  {
    const char *inner = (const char *)c.p;
    size_t innerlen = c.left;
    if (!diluvium_mp_read(&c, &t) || t.kind != DILUVIUM_MP_MAP)
      return;
    field_int(inner, innerlen, "instructions", insns);
    field_int(inner, innerlen, "memory_kb", mem);
  }
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
  char code[DVS_MAX_REQUEST];
  size_t code_len = 0;
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
  if (!field_str(msg, len, "code", code, sizeof(code), &code_len)) {
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
  field_budget(msg, len, &insns, &mem);
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
  req.code_len = code_len;      /* not strlen: a compiled chunk is full of zeroes */
  req.instructions = insns;
  req.memory_kb = mem;
  req.wake_on_message = field_bool(msg, len, "wake_on_message");
  /* 9.3 for flags: inherit the parent's set, and let the request drop
     DV_FLAG_UNSAFE_STDLIB but never add it. A supervisor that needs `os` itself
     can still hand its workers an instance that does not. */
  sl->flags = parent->flags;
  if (field_bool(msg, len, "sealed"))
    sl->flags &= ~(uint32_t)DV_FLAG_UNSAFE_STDLIB;
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
  dvs_id target = 0;
  dvs_slot *sl;
  dvs_id walk;
  if (!field_id(msg, len, "id", &target)) {
    emit_event(sw, parent->id, "denied", 0,
               "no usable id in the kill request");
    return;
  }
  sl = find(sw, target);
  if (sl == NULL) {
    emit_event(sw, parent->id, "denied", target, "no such instance");
    return;
  }
  /*
  ** Only an ancestor may kill. Parentage is the only relation this layer knows
  ** (9.1.2 item 2), and "any instance may kill any other" would make the
  ** capability set meaningless -- a child could kill its own supervisor.
  */
  for (walk = sl->parent; walk != 0; walk = dvs_parent(sw, walk)) {
    if (walk == parent->id) {
      kill_subtree(sw, target, 1);
      return;
    }
  }
  emit_event(sw, parent->id, "denied", target, "not a descendant");
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
  dvs_id target = 0;
  dvs_id who = parent->id;
  dvs_slot *subject;
  /* The comparison is against the validated handle, not a truncation of it: a
     request naming 0x100000000 + parent->id used to compare equal to the parent and
     silently hibernate the requester instead of being refused. */
  if (field_id(msg, len, "id", &target) && target != parent->id) {
    dvs_slot *sl = find(sw, target);
    dvs_id walk;
    if (sl == NULL) {
      emit_event(sw, parent->id, "denied", target, "no such instance");
      return;
    }
    for (walk = sl->parent; walk != 0; walk = dvs_parent(sw, walk)) {
      if (walk == parent->id)
        break;
    }
    if (walk == 0) {
      emit_event(sw, parent->id, "denied", target, "not a descendant");
      return;
    }
    who = target;
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
  dvs_id target = 0;
  dvs_slot *sl;
  char detail[128];
  uint64_t insns = 0, mem = 0;
  if (!field_id(msg, len, "id", &target)) {
    emit_event(sw, parent->id, "denied", 0, "no usable id in the query");
    return;
  }
  sl = find(sw, target);
  if (sl == NULL) {
    /*
    ** "status" with a detail, not an event named "gone": 9.2 lists the events this
    ** queue carries and "gone" is not among them, so emitting one made a program
    ** read an event the design does not describe. The information is the same.
    */
    emit_event(sw, parent->id, "status", target, "gone");
    return;
  }
  /* A cached instance has no 'dv_instance' to ask, and asking anyway would be a
     NULL dereference on the one path where a supervisor most wants an answer. */
  if (sl->inst != NULL)
    dv_usage(sl->inst, &insns, &mem);
  snprintf(detail, sizeof(detail), "%s insns=%lu mem_kb=%lu",
           sl->inst == NULL ? "cached" : sl->alive ? "alive" : "dead",
           (unsigned long)insns, (unsigned long)mem);
  emit_event(sw, parent->id, "status", target, detail);
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
    uint8_t buf[DVS_MAX_REQUEST];
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
      if (field_str((const char *)buf, n, "op", op, sizeof(op), NULL) &&
          strcmp(op, "spawn") == 0 && sw->spawns_this_step >= sw->spawn_rate) {
        emit_event(sw, sl->id, "throttled", 0, "spawn rate limit");
        return;
      }
      dv_queue_release(sl->inst, q);
    }
    if (!field_str((const char *)buf, n, "op", op, sizeof(op), NULL)) {
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
      /*
      ** Copied, not pointed at. 'dv_last_error' returns a pointer into the
      ** instance's own error buffer, and 'kill_subtree' below calls 'dv_free' on
      ** that instance -- so passing the pointer on to 'emit_event' afterwards read
      ** freed memory. The sanitizers never saw it because no test had a child that
      ** *faulted* while it had a parent to be told: the children in dvs_check.c are
      ** killed by the test or exit cleanly.
      */
      char why[192];
      why[0] = '\0';
      if (faulted) {
        const char *e = dv_last_error(sl->inst);
        size_t i = 0;
        if (e != NULL)
          while (i + 1 < sizeof(why) && e[i] != '\0') { why[i] = e[i]; i++; }
        why[i] = '\0';
      }
      kill_subtree(sw, id, 0);
      if (parent != 0)
        emit_event(sw, parent, what, id, faulted ? why : NULL);
    }
  }
  return dvs_alive(sw);
}
