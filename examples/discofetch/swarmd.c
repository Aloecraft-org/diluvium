/*
** swarmd.c
** A toy Discofetch swarm: one host, one supervisor, one coordinator, and a
** handler instance per connected client.
**
** This is the host, and the host is deliberately stupid. It does four things:
**
**   1. Creates the swarm and puts the supervisor in as its root.
**   2. Steps the swarm, over and over.
**   3. Moves bytes: whatever an instance writes to its 'outbox', the host pushes
**      into the coordinator's 'inbox'. It does not look inside.
**   4. Prints whatever an instance writes to its exported 'log' queue.
**
** Nothing in here knows what a client is, what a match is, or that a coordinator
** exists. Topology, routing and restart policy are the Lua programs' business --
** doc/Messaging.md 9.1.2 lists six things the swarm layer owns and routing is not
** among them, which means it is not the host's either.
**
** Written against 'dvs.h' and 'dv.h', plus the msgpack token cursor for reading
** the two fields the host genuinely needs ('log' lines and an event's name). The
** cursor is in the instance ABI's own header precisely so a host can read a
** message without linking the codec's Lua side.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dv.h"
#include "dvs.h"
#include "dmsgpack.h"


/* ===================================================================== */
/* The host vtable                                                        */
/* ===================================================================== */

/*
** One step of one instance. A parked instance is answered with the first queue
** that has something in it; an unstarted one is run.
**
** Answering the wait is the *host's* decision, not the swarm's: 8.3 gives the host
** the clock and the choice of which ready queue to deliver. "Nothing is ready" is
** a legitimate step and not a stall -- the instance simply stays parked, which at
** scale is where almost every instance is.
*/
static int host_drive (void *ud, dvs_id id, dv_instance *inst, void *ctx) {
  dv_waitset ws;
  dv_status st;
  (void)ud; (void)id; (void)ctx;
  memset(&ws, 0, sizeof(ws));
  st = dv_waitset_get(inst, &ws);
  if (st == DV_OK && ws.n > 0) {
    uint32_t i;
    for (i = 0; i < ws.n; i++) {
      dv_queue_info info;
      memset(&info, 0, sizeof(info));
      if (dv_queue_state(inst, ws.ids[i], &info) == DV_OK && info.len > 0) {
        st = dv_resume(inst, ws.ids[i]);
        return (st == DV_IDLE);
      }
    }
    return 1;                          /* parked, alive, nothing to deliver */
  }
  memset(&ws, 0, sizeof(ws));
  st = dv_run(inst, &ws);
  if (st == DV_BUSY)
    return 1;
  return (st == DV_IDLE);
}


static int total_created = 0, total_destroyed = 0, peak_live = 0;

/*
** Whatever the host wants to keep alongside an instance -- a socket, a task
** handle, a deadline. Here it is just the id, so there is something to free.
**
** Returning a real pointer rather than NULL is load-bearing, and not obviously so:
** 'release' in dvs.c guards the destroy callback with 'sl->ctx != NULL', so a host
** whose 'create' returns NULL is never told that an instance went away. dvs.h
** presents exactly that -- create as a no-op -- as the normal single-threaded case,
** and the in-tree tests do it, which is why their 'destroyed' counter is always
** zero and is never asserted. If you want 'destroy' as a lifecycle notification
** rather than only as context cleanup, you must return something.
*/
typedef struct slot_ctx { dvs_id id; } slot_ctx;

static void *host_create (void *ud, dvs_id id, dv_instance *inst) {
  slot_ctx *c = (slot_ctx *)malloc(sizeof(slot_ctx));
  (void)ud; (void)inst;
  total_created++;
  if (c != NULL) c->id = id;
  printf("  [host] instance %u created\n", (unsigned)id);
  return c;
}

static void host_destroy (void *ud, dvs_id id, void *ctx) {
  (void)ud;
  total_destroyed++;
  printf("  [host] instance %u destroyed; its state is gone with it\n",
         (unsigned)id);
  free(ctx);
}


/* ===================================================================== */
/* Reading the two fields a host actually needs                           */
/* ===================================================================== */

/*
** Copy the string at the top level of 'msg' out, or return 0.
**
** Through the cursor rather than by skipping a header byte: a msgpack string
** shorter than 32 bytes has a one-byte header and a longer one has two, so
** "skip the first byte" is right until a log line grows past 31 characters. That
** mistake cost three rounds of debugging elsewhere in this tree.
*/
static int mp_string (const void *msg, size_t len, char *out, size_t cap) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  diluvium_mp_open(&c, msg, len);
  if (!diluvium_mp_read(&c, &t) || t.kind != DILUVIUM_MP_STR)
    return 0;
  if (t.len >= cap)
    return 0;
  memcpy(out, t.p, t.len);
  out[t.len] = '\0';
  return 1;
}


/* A named string field of a map, or 0. Used for an event's 'event' and 'detail'. */
static int mp_field_str (const void *msg, size_t len, const char *key,
                         char *out, size_t cap) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  diluvium_mp_open(&c, msg, len);
  if (!diluvium_mp_field(&c, key))
    return 0;
  if (!diluvium_mp_read(&c, &t) || t.kind != DILUVIUM_MP_STR || t.len >= cap)
    return 0;
  memcpy(out, t.p, t.len);
  out[t.len] = '\0';
  return 1;
}


static int mp_field_int (const void *msg, size_t len, const char *key,
                         long long *out) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  diluvium_mp_open(&c, msg, len);
  if (!diluvium_mp_field(&c, key))
    return 0;
  if (!diluvium_mp_read(&c, &t) || t.kind != DILUVIUM_MP_INT)
    return 0;
  *out = t.i;
  return 1;
}


/* ===================================================================== */
/* Loading the programs                                                   */
/* ===================================================================== */

static char *slurp (const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  char *buf;
  long n;
  if (f == NULL) {
    fprintf(stderr, "swarmd: cannot open %s\n", path);
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = (char *)malloc((size_t)n + 1);
  if (buf == NULL) { fclose(f); return NULL; }
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fclose(f); free(buf); return NULL;
  }
  buf[n] = '\0';
  fclose(f);
  *len = (size_t)n;
  return buf;
}


/* ===================================================================== */
/* Injecting clients                                                      */
/* ===================================================================== */

/*
** A client "arriving". In the real thing this is a request off a socket; here it
** is a msgpack map the host pushes into the coordinator's 'clients' queue.
**
** Hand-encoded, because the host has no Lua state and does not need one. Only
** three shapes are used: a map header, a short string, and that is all -- which is
** worth noticing, since it is the whole vocabulary a host needs to talk to a
** program.
*/
static void put_str (char **p, char *end, const char *s) {
  size_t n = strlen(s);
  if (n > 31 || (size_t)(end - *p) < n + 1) return;   /* fixstr only; toy */
  *(*p)++ = (char)(0xa0 | n);
  memcpy(*p, s, n);
  *p += n;
}

/*
** A msgpack string of arbitrary length: str32, so a program's source fits.
**
** This is how the host hands a program its code -- as an ordinary message on an
** ordinary queue. Nothing distinguishes a string that happens to be a program from
** any other string, which for a system meant to rewrite its own agents at runtime
** is the feature and not a curiosity. The alternative, pasting the source into
** another program's source before loading it, works but puts the host in the
** business of quoting Lua.
*/
static size_t str_msg (char *buf, size_t cap, const char *s, size_t len) {
  if (cap < len + 5) return 0;
  buf[0] = (char)0xdb;
  buf[1] = (char)((len >> 24) & 0xff);
  buf[2] = (char)((len >> 16) & 0xff);
  buf[3] = (char)((len >> 8) & 0xff);
  buf[4] = (char)(len & 0xff);
  memcpy(buf + 5, s, len);
  return len + 5;
}


static size_t client_msg (char *buf, size_t cap, const char *name,
                          const char *want, const char *nat) {
  char *p = buf, *end = buf + cap;
  *p++ = (char)(0x80 | 3);
  put_str(&p, end, "client"); put_str(&p, end, name);
  put_str(&p, end, "want");   put_str(&p, end, want);
  put_str(&p, end, "nat");    put_str(&p, end, nat);
  return (size_t)(p - buf);
}


struct arrival { int step; const char *name, *want, *nat; };

/*
** The scenario: two pairs that should match, one client nobody matches, one whose
** spawn asks for authority the coordinator does not hold, and one deliberately
** hostile client whose handler will not stop on its own.
*/
static const struct arrival ARRIVALS[] = {
  { 2,  "alice",  "chess",  "cone"      },
  { 3,  "bob",    "chess",  "symmetric" },
  { 4,  "carol",  "go",     "cone"      },
  { 6,  "dave",   "go",     "cone"      },
  { 7,  "erin",   "shogi",  "cone"      },
  /* The coordinator will ask for a wider capability than it holds for this one,
     to show 9.3 refusing an attempt to widen a grant. */
  { 9,  "greedy",  "chess",  "cone"      },
  { 8,  "runaway","chess",  "spin"      },   /* its handler loops forever */
  { 0, NULL, NULL, NULL }
};


/* ===================================================================== */
/* main                                                                   */
/* ===================================================================== */

int main (int argc, char **argv) {
  dvs_host host;
  dvs_swarm *sw;
  dvs_id root = 0, coordinator = 0;
  int sent_coord_src = 0, sent_handler_src = 0, arrival_base = 0;
  char *sup_src, *coord_src, *handler_src;
  size_t sup_len, coord_len, handler_len;
  int step, max_steps = (argc > 1) ? atoi(argv[1]) : 40;
  /*
  ** The root's capabilities, and the ceiling for everything below it: 9.3 makes
  ** every descendant's set a subset of this one, so a swarm cannot grow a privilege
  ** it was not started with.
  */
  static const char *ROOT_CAPS[] = { "lifecycle", "queue:*" };

  sup_src     = slurp("supervisor.lua", &sup_len);
  coord_src   = slurp("coordinator.lua", &coord_len);
  handler_src = slurp("handler.lua", &handler_len);
  if (sup_src == NULL || coord_src == NULL || handler_src == NULL)
    return 1;

  memset(&host, 0, sizeof(host));
  host.create  = host_create;
  host.destroy = host_destroy;
  host.drive   = host_drive;

  /* 64 instances, 4 spawns per step. Both are bounds rather than defaults: 9.5
     rate-limits the lifecycle capability because a self-rewriting system produces
     a fork bomb eventually, as a bug rather than an attack. */
  sw = dvs_new(&host, 64, 4);
  if (sw == NULL) { fprintf(stderr, "swarmd: no swarm\n"); return 1; }

  printf("=== discofetch toy swarm ===\n");
  printf("hibernation: off (doc/Messaging.md 18.1) -- every instance is resident\n");
  printf("root caps: lifecycle, queue:*   -- the ceiling for the whole swarm.\n");
  printf("           A trailing '*' is a prefix; a bare \"*\" is a literal name,\n");
  printf("           on purpose, so nothing can widen a grant by wildcard.\n\n");

  if (dvs_root(sw, sup_src, sup_len, ROOT_CAPS,
               sizeof(ROOT_CAPS) / sizeof(ROOT_CAPS[0]),
               2000000, 4096, &root) != DVS_OK) {
    fprintf(stderr, "swarmd: root refused: %s\n", dvs_last_error(sw));
    return 1;
  }

  for (step = 1; step <= max_steps; step++) {
    int alive;
    dvs_id id;

    printf("--- step %d ---\n", step);

    /*
    ** Hand each program its code, as a message.
    **
    ** A queue has to exist before anything can be pushed into it, and a queue
    ** exists once the program that declares it has run -- so this cannot happen
    ** before the first step. That ordering is not a wart: it is the same reason a
    ** host cannot push to an instance it has not driven yet, and being forced to
    ** notice it here is better than discovering it under load.
    */
    if (!sent_coord_src && dvs_instance(sw, root) != NULL) {
      char *msg = (char *)malloc(coord_len + 8);
      if (msg != NULL) {
        size_t n = str_msg(msg, coord_len + 8, coord_src, coord_len);
        if (n > 0 && dvs_push(sw, root, "code", msg, n) == DVS_OK) {
          sent_coord_src = 1;
          printf("  [host] handed the supervisor the coordinator program\n");
        }
        free(msg);
      }
    }
    if (!sent_handler_src && coordinator != 0) {
      char *msg = (char *)malloc(handler_len + 8);
      if (msg != NULL) {
        size_t n = str_msg(msg, handler_len + 8, handler_src, handler_len);
        if (n > 0 && dvs_push(sw, coordinator, "code", msg, n) == DVS_OK) {
          sent_handler_src = 1;
          arrival_base = step;    /* clients start arriving after this */
          printf("  [host] handed the coordinator the handler template\n");
        }
        free(msg);
      }
    }

    /* Clients arrive, once there is somewhere to put them. Relative to when the
       coordinator became reachable, so the scenario does not depend on how many
       steps the handshake above took. */
    if (arrival_base != 0) {
      int i;
      for (i = 0; ARRIVALS[i].name != NULL; i++) {
        if (arrival_base + ARRIVALS[i].step == step) {
          char msg[128];
          size_t n = client_msg(msg, sizeof(msg), ARRIVALS[i].name,
                                ARRIVALS[i].want, ARRIVALS[i].nat);
          dvs_status st = dvs_push(sw, coordinator, "clients", msg, n);
          printf("  [host] %s arrives (wants %s, nat %s) -> %s\n",
                 ARRIVALS[i].name, ARRIVALS[i].want, ARRIVALS[i].nat,
                 dvs_status_name(st));
        }
      }
    }

    alive = dvs_step(sw);
    if (alive > peak_live) peak_live = alive;

    /*
    ** Drain what the programs wrote. Two queues, by convention between the host and
    ** the programs -- the runtime has no idea these names mean anything:
    **
    **   'log'     a string to print
    **   'outbox'  a message for the coordinator; the host moves it, unread.
    **             Reserved and exported per instance by 6.6, so no program had to
    **             declare it.
    **
    ** Walking every id from 1 is fine for a toy. A real host would keep its own
    ** list, which is what the 'create' hook's return value is for.
    */
    for (id = 1; id <= 64; id++) {
      dv_instance *inst = dvs_instance(sw, id);
      dv_queue_id q;
      uint8_t buf[4096];
      size_t n;
      if (inst == NULL)
        continue;

      q = dv_queue_lookup(inst, "log");
      if (q != 0) {
        while (dv_queue_pop(inst, q, buf, sizeof(buf), &n) == DV_OK) {
          char line[512];
          if (mp_string(buf, n, line, sizeof(line)))
            printf("  [%u] %s\n", (unsigned)id, line);
        }
      }

      q = dv_queue_lookup(inst, "outbox");
      if (q != 0 && coordinator != 0) {
        while (dv_queue_pop(inst, q, buf, sizeof(buf), &n) == DV_OK) {
          if (id != coordinator)
            dvs_push(sw, coordinator, "inbox", buf, n);
        }
      }

      /*
      ** The supervisor tells the host which instance became the coordinator. A
      ** program naming a peer to the host is the only way the host can route to
      ** it, since ids are the swarm's to assign.
      **
      ** Drained on every step, not just the first: a restarted coordinator is a
      ** *different instance* with a new id, and handles are never reused. A host
      ** that cached the first one keeps pushing at a dead handle and gets
      ** DVS_GONE forever -- which is what the first version of this file did.
      */
      if (id == root) {
        q = dv_queue_lookup(inst, "roles");
        if (q != 0) {
          while (dv_queue_pop(inst, q, buf, sizeof(buf), &n) == DV_OK) {
            long long who = 0;
            char role[32];
            if (mp_field_str(buf, n, "role", role, sizeof(role)) &&
                mp_field_int(buf, n, "id", &who) &&
                strcmp(role, "coordinator") == 0 &&
                coordinator != (dvs_id)who) {
              coordinator = (dvs_id)who;
              /* A fresh coordinator has none of the old one's state, including
                 the handler template -- so send it again. */
              sent_handler_src = 0;
              printf("  [host] the coordinator is instance %u\n",
                     (unsigned)coordinator);
            }
          }
        }
      }
    }

    if (alive == 0) {
      printf("  [host] nothing alive; stopping\n");
      break;
    }
  }

  /*
  ** Freed before the summary is printed, so the last few 'destroyed' lines land
  ** above the counts they are counted in. 'dvs_free' releases every live instance,
  ** which is the swarm shutting down and is worth seeing.
  */
  {
    int alive_at_end = dvs_alive(sw);
    printf("\n  [host] shutting the swarm down\n");
    dvs_free(sw);
    sw = NULL;
    printf("\n=== summary ===\n");
    printf("alive when shutdown began     : %d\n", alive_at_end);
  }
  printf("instances created over the run : %d\n", total_created);
  printf("instances destroyed            : %d\n", total_destroyed);
  printf("peak live at once              : %d\n", peak_live);

  /*
  ** The point of the numbers. A client's data lived in that client's instance and
  ** nowhere else, and when the instance went away so did the data -- there is no
  ** table in this host holding anything about alice, and nothing to clear.
  */
  printf("\nEach client's state lived in that client's own instance. The host holds\n");
  printf("no client data at all -- there is nothing here to leak or to clear.\n");
  printf("At ~46 KB per resident instance (make footprint), %d concurrent\n", peak_live);
  printf("sessions cost about %d KB of guest heap.\n", peak_live * 46);

  free(sup_src);
  free(coord_src);
  free(handler_src);
  return 0;
}
