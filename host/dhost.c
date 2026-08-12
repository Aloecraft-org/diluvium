/*
** dhost.c
** The generic host's core: configuration, the roster, the drive loop, and
** the hostcall pump. doc/Host.md is the contract; dhost.h says where the
** pieces sit. The listener and sql connectors live in their own files, so
** this one stays the part every deployment runs.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"

#include "dmsgpack.h"
#include "dhost.h"


/* ======================================================================
** dh_buf
** ====================================================================== */

void dh_buf_init (dh_buf *b) {
  b->p = NULL;
  b->len = 0;
  b->cap = 0;
}

void dh_buf_free (dh_buf *b) {
  free(b->p);
  dh_buf_init(b);
}

static int dh_grow (dh_buf *b, size_t need) {
  unsigned char *np;
  size_t cap = (b->cap != 0) ? b->cap : 256;
  /* Doubling with an overflow guard. Unbounded, 'cap *= 2' wraps through
     SIZE_MAX to 0 -- an infinite loop, or an undersized buffer that leaves
     b->p dangling on the realloc(,0) that follows. Unreachable on a 64-bit
     host where every caller bounds 'need' to megabytes, but dh_buf is
     exported for connectors that live elsewhere, and on a 32-bit size_t
     (the wasm32 target) the threshold is only 2 GB. Refuse rather than
     wrap. */
  if (need > SIZE_MAX - b->len)
    return -1;
  while (cap - b->len < need) {
    if (cap > SIZE_MAX / 2)
      return -1;
    cap *= 2;
  }
  if (cap == b->cap)
    return 0;
  np = (unsigned char *)realloc(b->p, cap);
  if (np == NULL)
    return -1;
  b->p = np;
  b->cap = cap;
  return 0;
}

int dh_raw (dh_buf *b, const void *p, size_t n) {
  if (dh_grow(b, n) != 0)
    return -1;
  memcpy(b->p + b->len, p, n);
  b->len += n;
  return 0;
}

static int dh_byte (dh_buf *b, unsigned char c) {
  return dh_raw(b, &c, 1);
}

static int dh_be (dh_buf *b, uint64_t v, int bytes) {
  unsigned char tmp[8];
  int i;
  for (i = bytes - 1; i >= 0; i--) {
    tmp[i] = (unsigned char)(v & 0xff);
    v >>= 8;
  }
  return dh_raw(b, tmp, (size_t)bytes);
}

int dh_map (dh_buf *b, unsigned n) {
  if (n <= 15) return dh_byte(b, (unsigned char)(0x80 | n));
  if (n <= 0xffff) return dh_byte(b, 0xde) || dh_be(b, n, 2);
  return dh_byte(b, 0xdf) || dh_be(b, n, 4);
}

int dh_array (dh_buf *b, unsigned n) {
  if (n <= 15) return dh_byte(b, (unsigned char)(0x90 | n));
  if (n <= 0xffff) return dh_byte(b, 0xdc) || dh_be(b, n, 2);
  return dh_byte(b, 0xdd) || dh_be(b, n, 4);
}

int dh_lstr (dh_buf *b, const char *s, size_t n) {
  int rc;
  if (n <= 31) rc = dh_byte(b, (unsigned char)(0xa0 | n));
  else if (n <= 0xff) rc = dh_byte(b, 0xd9) || dh_be(b, n, 1);
  else if (n <= 0xffff) rc = dh_byte(b, 0xda) || dh_be(b, n, 2);
  else rc = dh_byte(b, 0xdb) || dh_be(b, n, 4);
  return rc || dh_raw(b, s, n);
}

int dh_str (dh_buf *b, const char *s) {
  return dh_lstr(b, s, strlen(s));
}

int dh_uint (dh_buf *b, uint64_t v) {
  if (v <= 0x7f) return dh_byte(b, (unsigned char)v);
  if (v <= 0xff) return dh_byte(b, 0xcc) || dh_be(b, v, 1);
  if (v <= 0xffff) return dh_byte(b, 0xcd) || dh_be(b, v, 2);
  if (v <= 0xffffffffu) return dh_byte(b, 0xce) || dh_be(b, v, 4);
  return dh_byte(b, 0xcf) || dh_be(b, v, 8);
}

int dh_int (dh_buf *b, int64_t v) {
  if (v >= 0) return dh_uint(b, (uint64_t)v);
  if (v >= -32) return dh_byte(b, (unsigned char)(0xe0 | (v + 32)));
  if (v >= -128) return dh_byte(b, 0xd0) || dh_be(b, (uint64_t)(uint8_t)v, 1);
  if (v >= -32768) return dh_byte(b, 0xd1) || dh_be(b, (uint64_t)(uint16_t)v, 2);
  if (v >= -2147483647 - 1)
    return dh_byte(b, 0xd2) || dh_be(b, (uint64_t)(uint32_t)v, 4);
  return dh_byte(b, 0xd3) || dh_be(b, (uint64_t)v, 8);
}

int dh_double (dh_buf *b, double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  return dh_byte(b, 0xcb) || dh_be(b, bits, 8);
}

int dh_bool (dh_buf *b, int v) {
  return dh_byte(b, v ? 0xc3 : 0xc2);
}

int dh_nil (dh_buf *b) {
  return dh_byte(b, 0xc0);
}


/* ======================================================================
** The clock. Monotonic, because everything this host times -- connection
** deadlines, guest wait timeouts, poll sleeps -- is an interval, and wall
** time jumps. Wall time is a *connector* (the "time" call below), which is
** the design's whole point: a guest that wants the date asks for it, and
** the answer goes through the log.
** ====================================================================== */

int64_t dh_now_ms (void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


/* ======================================================================
** Configuration.
**
** The file is Lua syntax and deliberately not Lua power: it is loaded in an
** EMPTY environment, text mode only, so there is nothing to call -- no
** require, no os, not even pairs. That makes "data all the way down"
** enforced by construction; what remains for the walk below is shape.
** Every recognized key is type-checked, every unrecognized key is refused
** BY NAME, because an unknown key is a typo about to become a silent
** default -- the config is the one interface a non-programmer touches, and
** its refusals are worded accordingly.
** ====================================================================== */

static int cfg_fail (char *err, size_t cap, const char *fmt, const char *a,
                     const char *b) {
  if (err != NULL && cap > 0)
    snprintf(err, cap, fmt, a, b);
  return -1;
}

/* Refuse any key of the table at 'idx' that is not in 'known' (a
   NULL-terminated list). Also refuses non-string keys, which in a config
   are always a mistake. */
static int cfg_known_keys (lua_State *L, int idx, const char *const *known,
                           const char *where, char *err, size_t errcap) {
  int abs = lua_absindex(L, idx);
  lua_pushnil(L);
  while (lua_next(L, abs) != 0) {
    const char *const *k;
    const char *key;
    lua_pop(L, 1);                     /* the value; the key stays for next */
    if (lua_type(L, -1) != LUA_TSTRING) {
      lua_pop(L, 1);
      return cfg_fail(err, errcap, "%s has a non-string key, which a config "
                                   "never needs%s", where, "");
    }
    key = lua_tostring(L, -1);
    for (k = known; *k != NULL; k++) {
      if (strcmp(*k, key) == 0) break;
    }
    if (*k == NULL) {
      lua_pop(L, 1);
      return cfg_fail(err, errcap, "%s has no key named '%s' -- a typo here "
                                   "would otherwise become a silent default",
                      where, key);
    }
  }
  return 0;
}

static int cfg_str (lua_State *L, int idx, const char *key, char *out,
                    size_t cap, int required, const char *where,
                    char *err, size_t errcap) {
  int rc = 0;
  lua_getfield(L, idx, key);
  if (lua_isnil(L, -1)) {
    if (required)
      rc = cfg_fail(err, errcap, "%s.%s is required and missing", where, key);
  }
  else if (lua_type(L, -1) != LUA_TSTRING) {
    rc = cfg_fail(err, errcap, "%s.%s must be a string", where, key);
  }
  else {
    size_t n;
    const char *s = lua_tolstring(L, -1, &n);
    if (n + 1 > cap)
      rc = cfg_fail(err, errcap, "%s.%s is longer than this host can hold",
                    where, key);
    else
      memcpy(out, s, n + 1);
  }
  lua_pop(L, 1);
  return rc;
}

static int cfg_num (lua_State *L, int idx, const char *key, int64_t *out,
                    int64_t lo, int64_t hi, const char *where,
                    char *err, size_t errcap) {
  int rc = 0;
  lua_getfield(L, idx, key);
  if (!lua_isnil(L, -1)) {
    if (!lua_isinteger(L, -1))
      rc = cfg_fail(err, errcap, "%s.%s must be an integer", where, key);
    else {
      lua_Integer v = lua_tointeger(L, -1);
      if (v < lo || v > hi)
        rc = cfg_fail(err, errcap, "%s.%s is outside its sane range",
                      where, key);
      else
        *out = (int64_t)v;
    }
  }
  lua_pop(L, 1);
  return rc;
}

static void cfg_defaults (dh_config *c) {
  memset(c, 0, sizeof(*c));
  c->max_instances = 64;
  c->spawns_per_step = 4;
  c->hibernation = 1;
  c->listener.port = 0;
  strcpy(c->listener.bind_addr, "127.0.0.1");
  strcpy(c->listener.queue, "http_in");
  strcpy(c->listener.reply_queue, "http_out");
  c->listener.max_body = 65536;
  c->listener.conn_deadline_ms = 10000;
  c->listener.max_conns = 64;
  c->sql.max_rows = 1024;
}

int dh_config_load (const char *path, dh_config *out, char *err,
                    size_t errcap) {
  static const char *const top_keys[] = {
    "supervisor", "max_instances", "spawns_per_step", "identity",
    "hibernation", "caps", "budget", "connectors", NULL
  };
  static const char *const budget_keys[] = { "instructions", "memory_kb", NULL };
  static const char *const conn_keys[] = { "time", "listen", "sql", NULL };
  static const char *const listen_keys[] = {
    "port", "bind", "queue", "reply_queue", "max_body", "deadline_ms",
    "max_conns", NULL
  };
  static const char *const sql_keys[] = { "path", "mode", "max_rows", NULL };
  lua_State *L;
  int rc = -1;
  int64_t n;
  cfg_defaults(out);
  L = luaL_newstate();
  if (L == NULL)
    return cfg_fail(err, errcap, "no memory for a config reader%s%s", "", "");
  /* Text mode, and then an empty _ENV before the first instruction runs. */
  if (luaL_loadfilex(L, path, "t") != LUA_OK) {
    cfg_fail(err, errcap, "config did not parse: %s%s",
             lua_tostring(L, -1), "");
    goto done;
  }
  lua_newtable(L);
  lua_setupvalue(L, -2, 1);
  if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
    /* With an empty environment the only way to get here is calling
       something, and everything is nil -- which is the refusal working. */
    cfg_fail(err, errcap, "config did not run as data: %s (the environment "
                          "is empty on purpose: a config declares, it does "
                          "not compute)%s", lua_tostring(L, -1), "");
    goto done;
  }
  if (!lua_istable(L, -1)) {
    cfg_fail(err, errcap, "config must return a table%s%s", "", "");
    goto done;
  }
  if (cfg_known_keys(L, -1, top_keys, "config", err, errcap) != 0) goto done;
  if (cfg_str(L, -1, "supervisor", out->supervisor, sizeof(out->supervisor),
              1, "config", err, errcap) != 0) goto done;
  n = out->max_instances;
  if (cfg_num(L, -1, "max_instances", &n, 1, 100000, "config", err, errcap)
      != 0) goto done;
  out->max_instances = (uint32_t)n;
  n = out->spawns_per_step;
  if (cfg_num(L, -1, "spawns_per_step", &n, 1, 100000, "config", err, errcap)
      != 0) goto done;
  out->spawns_per_step = (uint32_t)n;
  if (cfg_str(L, -1, "identity", out->identity, sizeof(out->identity), 0,
              "config", err, errcap) != 0) goto done;
  lua_getfield(L, -1, "hibernation");
  if (!lua_isnil(L, -1)) {
    const char *s = lua_tostring(L, -1);
    if (s == NULL || (strcmp(s, "on") != 0 && strcmp(s, "off") != 0)) {
      lua_pop(L, 1);
      cfg_fail(err, errcap, "config.hibernation must be \"on\" or \"off\"%s%s",
               "", "");
      goto done;
    }
    out->hibernation = (strcmp(s, "on") == 0);
  }
  lua_pop(L, 1);
  lua_getfield(L, -1, "caps");
  if (!lua_isnil(L, -1)) {
    lua_Integer i, len;
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      cfg_fail(err, errcap, "config.caps must be an array of strings%s%s",
               "", "");
      goto done;
    }
    len = (lua_Integer)lua_rawlen(L, -1);
    if (len > DH_MAX_CAPS) {
      lua_pop(L, 1);
      cfg_fail(err, errcap, "config.caps holds more than this host's %s "
                            "slots%s", "16", "");
      goto done;
    }
    for (i = 1; i <= len; i++) {
      size_t sn;
      const char *s;
      lua_rawgeti(L, -1, i);
      s = (lua_type(L, -1) == LUA_TSTRING) ? lua_tolstring(L, -1, &sn) : NULL;
      if (s == NULL || sn + 1 > DH_NAME_MAX) {
        lua_pop(L, 2);
        cfg_fail(err, errcap, "a config.caps entry is not a short string%s%s",
                 "", "");
        goto done;
      }
      memcpy(out->caps[out->ncaps++], s, sn + 1);
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  lua_getfield(L, -1, "budget");
  if (!lua_isnil(L, -1)) {
    if (!lua_istable(L, -1) ||
        cfg_known_keys(L, -1, budget_keys, "config.budget", err, errcap) != 0) {
      if (!lua_istable(L, -1))
        cfg_fail(err, errcap, "config.budget must be a table%s%s", "", "");
      lua_pop(L, 1);
      goto done;
    }
    n = 0;
    if (cfg_num(L, -1, "instructions", &n, 0, INT64_MAX, "config.budget",
                err, errcap) != 0) { lua_pop(L, 1); goto done; }
    out->instructions = (uint64_t)n;
    n = 0;
    if (cfg_num(L, -1, "memory_kb", &n, 0, INT64_MAX, "config.budget",
                err, errcap) != 0) { lua_pop(L, 1); goto done; }
    out->memory_kb = (uint64_t)n;
  }
  lua_pop(L, 1);
  lua_getfield(L, -1, "connectors");
  if (!lua_isnil(L, -1)) {
    if (!lua_istable(L, -1) ||
        cfg_known_keys(L, -1, conn_keys, "config.connectors", err, errcap)
        != 0) {
      if (!lua_istable(L, -1))
        cfg_fail(err, errcap, "config.connectors must be a table%s%s", "", "");
      lua_pop(L, 1);
      goto done;
    }
    lua_getfield(L, -1, "time");
    out->time_connector = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "listen");
    if (!lua_isnil(L, -1)) {
      if (!lua_istable(L, -1) ||
          cfg_known_keys(L, -1, listen_keys, "config.connectors.listen",
                         err, errcap) != 0) {
        if (!lua_istable(L, -1))
          cfg_fail(err, errcap, "config.connectors.listen must be a "
                                "table%s%s", "", "");
        lua_pop(L, 2);
        goto done;
      }
      n = 0;
      if (cfg_num(L, -1, "port", &n, 1, 65535, "config.connectors.listen",
                  err, errcap) != 0 || n == 0) {
        if (n == 0)
          cfg_fail(err, errcap, "config.connectors.listen.port is "
                                "required%s%s", "", "");
        lua_pop(L, 2);
        goto done;
      }
      out->listener.port = (int)n;
      out->listener.enabled = 1;
      if (cfg_str(L, -1, "bind", out->listener.bind_addr,
                  sizeof(out->listener.bind_addr), 0,
                  "config.connectors.listen", err, errcap) != 0 ||
          cfg_str(L, -1, "queue", out->listener.queue,
                  sizeof(out->listener.queue), 0,
                  "config.connectors.listen", err, errcap) != 0 ||
          cfg_str(L, -1, "reply_queue", out->listener.reply_queue,
                  sizeof(out->listener.reply_queue), 0,
                  "config.connectors.listen", err, errcap) != 0) {
        lua_pop(L, 2);
        goto done;
      }
      n = out->listener.max_body;
      if (cfg_num(L, -1, "max_body", &n, 1, 16 * 1024 * 1024,
                  "config.connectors.listen", err, errcap) != 0) {
        lua_pop(L, 2); goto done;
      }
      out->listener.max_body = (long)n;
      n = out->listener.conn_deadline_ms;
      if (cfg_num(L, -1, "deadline_ms", &n, 1, 3600000,
                  "config.connectors.listen", err, errcap) != 0) {
        lua_pop(L, 2); goto done;
      }
      out->listener.conn_deadline_ms = (long)n;
      n = out->listener.max_conns;
      if (cfg_num(L, -1, "max_conns", &n, 1, 4096,
                  "config.connectors.listen", err, errcap) != 0) {
        lua_pop(L, 2); goto done;
      }
      out->listener.max_conns = (int)n;
    }
    lua_pop(L, 1);
    lua_getfield(L, -1, "sql");
    if (!lua_isnil(L, -1)) {
      if (!lua_istable(L, -1) ||
          cfg_known_keys(L, -1, sql_keys, "config.connectors.sql", err,
                         errcap) != 0) {
        if (!lua_istable(L, -1))
          cfg_fail(err, errcap, "config.connectors.sql must be a table%s%s",
                   "", "");
        lua_pop(L, 2);
        goto done;
      }
      if (cfg_str(L, -1, "path", out->sql.path, sizeof(out->sql.path), 1,
                  "config.connectors.sql", err, errcap) != 0) {
        lua_pop(L, 2);
        goto done;
      }
      out->sql.enabled = 1;
      lua_getfield(L, -1, "mode");
      if (!lua_isnil(L, -1)) {
        const char *s = lua_tostring(L, -1);
        if (s == NULL || (strcmp(s, "read") != 0 &&
                          strcmp(s, "readwrite") != 0)) {
          lua_pop(L, 3);
          cfg_fail(err, errcap, "config.connectors.sql.mode must be "
                                "\"read\" or \"readwrite\"%s%s", "", "");
          goto done;
        }
        out->sql.readwrite = (strcmp(s, "readwrite") == 0);
      }
      lua_pop(L, 1);
      n = out->sql.max_rows;
      if (cfg_num(L, -1, "max_rows", &n, 1, 1000000,
                  "config.connectors.sql", err, errcap) != 0) {
        lua_pop(L, 2); goto done;
      }
      out->sql.max_rows = (long)n;
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  rc = 0;
done:
  lua_close(L);
  return rc;
}


/* ======================================================================
** The roster and the vtable.
** ====================================================================== */

static void settle_deadline (dh_host *h, dh_slot *sc, dv_instance *inst) {
  dv_waitset ws;
  memset(&ws, 0, sizeof(ws));
  sc->has_deadline = 0;
  if (dv_waitset_get(inst, &ws) == DV_OK && ws.timeout_ms >= 0) {
    sc->has_deadline = 1;
    sc->deadline_ms = h->now_ms + ws.timeout_ms;
  }
}

static void *host_create (void *ud, dvs_id id, dv_instance *inst) {
  dh_host *h = (dh_host *)ud;
  /* Non-NULL is load-bearing: dvs guards its destroy callback on ctx, so a
     create that returns NULL is never told the instance went away --
     swarmd.c found that the hard way and this host keeps a roster. */
  dh_slot *sc = (dh_slot *)calloc(1, sizeof(dh_slot));
  (void)inst;
  if (sc == NULL)
    return NULL;
  sc->id = id;
  sc->next = h->slots;
  h->slots = sc;
  return sc;
}

static void host_destroy (void *ud, dvs_id id, void *ctx) {
  dh_host *h = (dh_host *)ud;
  dh_slot **pp = &h->slots;
  (void)id;
  while (*pp != NULL && *pp != ctx)
    pp = &(*pp)->next;
  if (*pp != NULL)
    *pp = (*pp)->next;
  free(ctx);
}

/*
** Duty 2's inner step, one instance: swarmd.c's loop generalized. Deliver
** the round-robin next queue that has something, or start the program, or
** report it finished. Whenever the program parks anew, its wait timeout is
** read and remembered, so dh_host_turn can answer "your timeout elapsed"
** at the right moment -- the guest expresses the bound, the host owns the
** clock (8.3).
*/
static int host_drive (void *ud, dvs_id id, dv_instance *inst, void *ctx) {
  dh_host *h = (dh_host *)ud;
  dh_slot *sc = (dh_slot *)ctx;
  dv_waitset ws;
  dv_status st;
  (void)id;
  memset(&ws, 0, sizeof(ws));
  st = dv_waitset_get(inst, &ws);
  if (st == DV_OK && ws.n > 0) {
    uint32_t k;
    for (k = 0; k < ws.n; k++) {
      uint32_t i = (sc != NULL) ? (sc->rr_next + k) % ws.n : k;
      dv_queue_info info;
      memset(&info, 0, sizeof(info));
      if (dv_queue_state(inst, ws.ids[i], &info) == DV_OK && info.len > 0) {
        if (sc != NULL)
          sc->rr_next = (i + 1) % ws.n;
        st = dv_resume(inst, ws.ids[i]);
        if (st == DV_IDLE && sc != NULL)
          settle_deadline(h, sc, inst);
        return (st == DV_IDLE);
      }
    }
    return 1;                          /* parked; nothing to deliver yet */
  }
  memset(&ws, 0, sizeof(ws));
  st = dv_run(inst, &ws);
  if (st == DV_BUSY)
    return 1;
  if (st == DV_IDLE && sc != NULL)
    settle_deadline(h, sc, inst);
  return (st == DV_IDLE);
}


/* ======================================================================
** The hostcall pump: doc/Hostcall.md's host half.
** ====================================================================== */

int dh_register (dh_host *h, const char *prefix, dh_call_fn fn, void *ud) {
  size_t i;
  if (h->nconns >= DH_MAX_CONNECTORS || strlen(prefix) >= 32)
    return -1;
  for (i = 0; i < h->nconns; i++) {
    if (strcmp(h->conns[i].prefix, prefix) == 0)
      return -1;
  }
  strcpy(h->conns[h->nconns].prefix, prefix);
  h->conns[h->nconns].fn = fn;
  h->conns[h->nconns].ud = ud;
  h->nconns++;
  return 0;
}

static dh_connector *route (dh_host *h, const char *call) {
  size_t i;
  const char *slash = strchr(call, '/');
  size_t plen = (slash != NULL) ? (size_t)(slash - call) : strlen(call);
  for (i = 0; i < h->nconns; i++) {
    if (strlen(h->conns[i].prefix) == plen &&
        strncmp(h->conns[i].prefix, call, plen) == 0)
      return &h->conns[i];
  }
  return NULL;
}

/*
** Answer one request. Every drained request is answered -- denied, error
** and malformed included -- because a dropped request is invisible
** backpressure (Hostcall.md). The reply says what happened and, when it is
** not "ok", why, worded for the program to read.
*/
static void answer (dh_host *h, dvs_id id, const unsigned char *req,
                    size_t reqlen, dh_buf *reply) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  int64_t tok = 0;
  int has_tok = 0;
  char call[DH_NAME_MAX];
  const unsigned char *args = NULL;
  size_t argslen = 0;
  char detail[256];
  dh_buf value;
  dh_call_status st;
  dh_connector *conn;

  dh_buf_init(&value);
  call[0] = '\0';
  detail[0] = '\0';

  diluvium_mp_open(&c, req, reqlen);
  if (diluvium_mp_field(&c, "tok") && diluvium_mp_read(&c, &t) &&
      t.kind == DILUVIUM_MP_INT && t.i >= 0) {
    tok = t.i;
    has_tok = 1;
  }
  diluvium_mp_open(&c, req, reqlen);
  if (diluvium_mp_field(&c, "call") && diluvium_mp_read(&c, &t) &&
      t.kind == DILUVIUM_MP_STR && t.len > 0 && t.len < sizeof(call)) {
    memcpy(call, t.p, t.len);
    call[t.len] = '\0';
  }
  diluvium_mp_open(&c, req, reqlen);
  if (diluvium_mp_field(&c, "args")) {
    const unsigned char *start = c.p;
    if (diluvium_mp_skip(&c)) {
      args = start;
      argslen = (size_t)(c.p - start);
    }
  }

  if (!has_tok || call[0] == '\0') {
    /* Malformed: answered, with whatever token was readable, because an
       uncorrelatable reply is the sender's own diagnostic where silence
       diagnoses nothing. */
    dh_map(reply, has_tok ? 3 : 2);
    if (has_tok) { dh_str(reply, "tok"); dh_int(reply, tok); }
    dh_str(reply, "status");
    dh_str(reply, "malformed");
    dh_str(reply, "detail");
    dh_str(reply, !has_tok
        ? "a hostcall request needs an integer 'tok' (Hostcall.md: the "
          "correlation token is required, from the first prototype on)"
        : "a hostcall request needs a 'call' string naming what is asked");
    return;
  }

  {
    char cap[DH_NAME_MAX + 8];
    snprintf(cap, sizeof(cap), "host:%s", call);
    if (!dvs_holds(h->sw, id, cap)) {
      dh_map(reply, 3);
      dh_str(reply, "tok"); dh_int(reply, tok);
      dh_str(reply, "status"); dh_str(reply, "denied");
      dh_str(reply, "detail");
      snprintf(detail, sizeof(detail),
               "this program does not hold '%s'; a call outside the grant "
               "is refused, never dropped", cap);
      dh_str(reply, detail);
      return;
    }
  }

  conn = route(h, call);
  if (conn == NULL) {
    dh_map(reply, 3);
    dh_str(reply, "tok"); dh_int(reply, tok);
    dh_str(reply, "status"); dh_str(reply, "denied");
    dh_str(reply, "detail");
    snprintf(detail, sizeof(detail),
             "no connector answers '%s' in this deployment; connectors are "
             "all off by default and wired by configuration", call);
    dh_str(reply, detail);
    return;
  }

  st = conn->fn(conn->ud, id, call, args, argslen, &value, detail,
                sizeof(detail));
  dh_map(reply, 3);
  dh_str(reply, "tok"); dh_int(reply, tok);
  dh_str(reply, "status");
  dh_str(reply, (st == DH_CALL_OK) ? "ok"
               : (st == DH_CALL_DENIED) ? "denied" : "error");
  if (st == DH_CALL_OK) {
    dh_str(reply, "value");
    if (value.len > 0)
      dh_raw(reply, value.p, value.len);
    else
      dh_nil(reply);
  }
  else {
    dh_str(reply, "detail");
    dh_str(reply, (detail[0] != '\0') ? detail : "the connector refused "
                                                 "without saying why, which "
                                                 "is its bug");
  }
  dh_buf_free(&value);
}

static void pump_instance (dh_host *h, dh_slot *sc) {
  dv_instance *inst = dvs_instance(h->sw, sc->id);
  dv_queue_id calls, replies;
  if (inst == NULL)
    return;                            /* hibernated: its calls wait with it */
  calls = dv_queue_lookup(inst, DH_CALLS_QUEUE);
  if (calls == 0)
    return;
  replies = dv_queue_lookup(inst, DH_REPLIES_QUEUE);
  for (;;) {
    const uint8_t *req = NULL;
    size_t reqlen = 0;
    dh_buf reply;
    /* Do not drain a request we cannot answer. If the reply queue is full,
       leave the request in place and stop pumping this instance this turn:
       "every drained request is answered" (Hostcall.md) is kept by not
       draining until there is room, which also means a stateful connector
       (sql/exec) is never re-run on a retry -- answering, then failing to
       deliver, then recomputing next turn would double-apply its write.
       Backpressure propagates backward to the guest, which is correct: it
       undersized a queue it owns, and it will drain its replies when driven. */
    if (replies != 0) {
      dv_queue_info info;
      memset(&info, 0, sizeof(info));
      if (dv_queue_state(inst, replies, &info) == DV_OK &&
          info.capacity > 0 && info.len >= info.capacity)
        break;
    }
    if (dv_queue_peek(inst, calls, &req, &reqlen) != DV_OK)
      break;
    dh_buf_init(&reply);
    answer(h, sc->id, req, reqlen, &reply);
    dv_queue_release(inst, calls);
    if (replies != 0 && reply.len > 0)
      dv_queue_push(inst, replies, reply.p, reply.len);
    /* No reply queue means the program asked and declared nowhere to hear
       the answer; the request is consumed so the queue cannot wedge, and
       the reply has nowhere to go. Its own diagnostic, eventually. */
    dh_buf_free(&reply);
  }
}


/* ======================================================================
** Built-in connector: time.
**
** Wall-clock milliseconds since the epoch. The one nondeterminism every
** program eventually wants, and the reason the hostcall shape wins: the
** answer arrives as a message, so it is in the log, so a replay replays
** the same moment instead of the replayer's.
** ====================================================================== */

static dh_call_status conn_time (void *ud, dvs_id id, const char *call,
                                 const unsigned char *args, size_t argslen,
                                 dh_buf *value, char *detail,
                                 size_t detailcap) {
  struct timespec ts;
  (void)ud; (void)id; (void)args; (void)argslen;
  if (strcmp(call, "time") != 0) {
    snprintf(detail, detailcap, "the time connector answers 'time' and "
                                "nothing else; '%s' is not it", call);
    return DH_CALL_ERROR;
  }
  clock_gettime(CLOCK_REALTIME, &ts);
  dh_uint(value, (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
  return DH_CALL_OK;
}


/* ======================================================================
** Open, turn, close.
** ====================================================================== */

static int read_file (const char *path, char **out, size_t *outlen) {
  FILE *f = fopen(path, "rb");
  long n;
  char *buf;
  if (f == NULL)
    return -1;
  if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
      fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return -1;
  }
  buf = (char *)malloc((size_t)n + 1);
  if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    fclose(f);
    return -1;
  }
  fclose(f);
  buf[n] = '\0';
  *out = buf;
  *outlen = (size_t)n;
  return 0;
}

int dh_host_open (dh_host *h, const dh_config *cfg, char *err, size_t errcap) {
  dvs_host vt;
  const char *caps[DH_MAX_CAPS];
  size_t i;
  char *code = NULL;
  size_t codelen = 0;
  dvs_id root = 0;

  memset(h, 0, sizeof(*h));
  h->cfg = *cfg;
  h->now_ms = dh_now_ms();

  memset(&vt, 0, sizeof(vt));
  vt.create = host_create;
  vt.destroy = host_destroy;
  vt.drive = host_drive;
  vt.ud = h;
  h->sw = dvs_new(&vt, cfg->max_instances, cfg->spawns_per_step);
  if (h->sw == NULL)
    return cfg_fail(err, errcap, "could not create a swarm%s%s", "", "");
  if (cfg->identity[0] != '\0' &&
      dvs_set_host_identity(h->sw, cfg->identity) != DVS_OK) {
    dh_host_close(h);
    return cfg_fail(err, errcap, "could not stamp the swarm's identity%s%s",
                    "", "");
  }
  dvs_allow_hibernation(h->sw, cfg->hibernation);

  if (cfg->time_connector)
    dh_register(h, "time", conn_time, NULL);
  if (cfg->sql.enabled && dh_sql_open(h, err, errcap) != 0) {
    dh_host_close(h);
    return -1;
  }
  if (cfg->listener.enabled && dh_http_open(h, err, errcap) != 0) {
    dh_host_close(h);
    return -1;
  }

  if (read_file(cfg->supervisor, &code, &codelen) != 0) {
    dh_host_close(h);
    return cfg_fail(err, errcap, "could not read the supervisor program "
                                 "'%s'%s", cfg->supervisor, "");
  }
  for (i = 0; i < cfg->ncaps; i++)
    caps[i] = cfg->caps[i];
  if (dvs_root(h->sw, code, codelen, caps, cfg->ncaps, cfg->instructions,
               cfg->memory_kb, &root) != DVS_OK) {
    cfg_fail(err, errcap, "the supervisor would not load: %s%s",
             dvs_last_error(h->sw), "");
    free(code);
    dh_host_close(h);
    return -1;
  }
  h->root = root;
  free(code);
  return 0;
}

void dh_host_close (dh_host *h) {
  dh_http_close(h);
  dh_sql_close(h);
  if (h->sw != NULL) {
    dvs_free(h->sw);                   /* releases slots via host_destroy */
    h->sw = NULL;
  }
  while (h->slots != NULL) {           /* only if dvs never owned them */
    dh_slot *n = h->slots->next;
    free(h->slots);
    h->slots = n;
  }
}

int dh_host_turn (dh_host *h) {
  dh_slot *sc;
  h->now_ms = dh_now_ms();
  dvs_step(h->sw);
  /* Elapsed guest wait timeouts. The resume happens outside dvs_step; the
     swarm reconciles the instance's fate on its next step, which is the
     next turn. */
  for (sc = h->slots; sc != NULL; sc = sc->next) {
    if (sc->has_deadline && sc->deadline_ms <= h->now_ms) {
      dv_instance *inst = dvs_instance(h->sw, sc->id);
      sc->has_deadline = 0;
      if (inst != NULL) {
        if (dv_resume(inst, 0) == DV_IDLE)
          settle_deadline(h, sc, inst);
      }
    }
  }
  for (sc = h->slots; sc != NULL; sc = sc->next)
    pump_instance(h, sc);
  return dvs_alive(h->sw);
}

int dh_host_poll_timeout (dh_host *h) {
  dh_slot *sc;
  int64_t next = -1;
  int http;
  for (sc = h->slots; sc != NULL; sc = sc->next) {
    if (sc->has_deadline && (next < 0 || sc->deadline_ms < next))
      next = sc->deadline_ms;
  }
  /* Fold in the listener's earliest connection deadline, so a caller that
     sleeps for this value still wakes to enforce a slow client's timeout --
     the comment used to claim this happened and it did not. */
  http = dh_http_next_timeout(h);
  if (next < 0) {
    if (http < 0)
      return -1;
    return http;
  }
  {
    int guest = (next <= h->now_ms) ? 0
              : (next - h->now_ms > 60000) ? 60000 : (int)(next - h->now_ms);
    if (http >= 0 && http < guest)
      return http;
    return guest;
  }
}
