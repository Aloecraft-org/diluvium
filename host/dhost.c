/*
** dhost.c
** The generic host's core: configuration, the roster, the drive loop, and
** the hostcall pump. doc/Host.md is the contract; dhost.h says where the
** pieces sit. The listener and sql connectors live in their own files, so
** this one stays the part every deployment runs.
*/

#include <errno.h>
#include <signal.h>
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
  if (n == 0)
    return 0;                           /* memcpy(_, NULL, 0) is UB; an empty
                                           string or array reaches here with a
                                           NULL, unallocated side buffer */
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

static void listener_defaults (dh_listener_cfg *l) {
  memset(l, 0, sizeof(*l));
  l->port = 0;
  strcpy(l->bind_addr, "127.0.0.1");
  strcpy(l->queue, "http_in");
  strcpy(l->reply_queue, "http_out");
  l->max_body = 65536;
  l->conn_deadline_ms = 10000;
  l->max_conns = 64;
}

static void cfg_defaults (dh_config *c) {
  memset(c, 0, sizeof(*c));
  c->max_instances = 64;
  c->spawns_per_step = 4;
  c->hibernation = 1;
  c->nlisteners = 0;
  c->sql.max_result_rows = 1024;
  c->fs.max_bytes = 1024 * 1024;
  c->exec.max_timeout_ms = 10000;
  c->exec.max_output_bytes = 1024 * 1024;
}

/* Parse one listener block at 'idx' into 'l' (defaults already applied). The
   keys have already been vetted against listen_keys. 'where' names the block
   for refusals -- "config.connectors.listen" for the single form, or
   "config.connectors.listen[2]" for an array element. */
static int cfg_listener (lua_State *L, int idx, dh_listener_cfg *l,
                         const char *where, char *err, size_t errcap) {
  int64_t n = 0;
  if (cfg_num(L, idx, "port", &n, 1, 65535, where, err, errcap) != 0)
    return -1;
  if (n == 0)
    return cfg_fail(err, errcap, "%s.port is required%s", where, "");
  l->port = (int)n;
  if (cfg_str(L, idx, "bind", l->bind_addr, sizeof(l->bind_addr), 0,
              where, err, errcap) != 0 ||
      cfg_str(L, idx, "queue", l->queue, sizeof(l->queue), 0,
              where, err, errcap) != 0 ||
      cfg_str(L, idx, "reply_queue", l->reply_queue, sizeof(l->reply_queue), 0,
              where, err, errcap) != 0)
    return -1;
  n = l->max_body;
  if (cfg_num(L, idx, "max_body", &n, 1, 16 * 1024 * 1024, where, err, errcap)
      != 0)
    return -1;
  l->max_body = (long)n;
  n = l->conn_deadline_ms;
  if (cfg_num(L, idx, "deadline_ms", &n, 1, 3600000, where, err, errcap) != 0)
    return -1;
  l->conn_deadline_ms = (long)n;
  n = l->max_conns;
  if (cfg_num(L, idx, "max_conns", &n, 1, 4096, where, err, errcap) != 0)
    return -1;
  l->max_conns = (int)n;
  /* The request-header allowlist: an array of LOWERCASE names, empty by
     default -- forwarding nothing is the safe default, and requiring the
     canonical spelling here beats normalizing it quietly. */
  lua_getfield(L, idx, "headers");
  if (!lua_isnil(L, -1)) {
    size_t i;
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      return cfg_fail(err, errcap, "%s.headers must be an array of header "
                                   "names%s", where, "");
    }
    for (i = 0; ; i++) {
      const char *s;
      size_t slen, j;
      lua_rawgeti(L, -1, (lua_Integer)i + 1);
      if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        break;
      }
      if (i >= DH_MAX_HDRS) {
        lua_pop(L, 2);
        return cfg_fail(err, errcap, "%s.headers lists more than 8 names%s",
                        where, "");
      }
      s = lua_tolstring(L, -1, &slen);
      if (s == NULL || slen == 0 || slen >= DH_HDR_NAME_MAX) {
        lua_pop(L, 2);
        return cfg_fail(err, errcap, "%s.headers entries must be short "
                                     "header names%s", where, "");
      }
      for (j = 0; j < slen; j++) {
        if (s[j] >= 'A' && s[j] <= 'Z') {
          lua_pop(L, 2);
          return cfg_fail(err, errcap, "%s.headers names are lowercase "
                                       "('%s' is not): the guest sees them "
                                       "lowercased, so the config spells "
                                       "them that way", where, s);
        }
      }
      memcpy(l->headers[l->nheaders], s, slen);
      l->headers[l->nheaders][slen] = '\0';
      l->nheaders++;
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);
  return 0;
}

int dh_config_load (const char *path, dh_config *out, char *err,
                    size_t errcap) {
  static const char *const top_keys[] = {
    "supervisor", "max_instances", "spawns_per_step", "identity",
    "hibernation", "caps", "budget", "connectors", "plugins", NULL
  };
  static const char *const plugin_keys[] = { "manifest", "max_inflight",
                                             "call_timeout_ms", NULL };
  static const char *const budget_keys[] = { "instructions", "memory_kb", NULL };
  static const char *const conn_keys[] = { "time", "listen", "sql", "crypto",
                                           "fs", "exec", NULL };
  static const char *const fs_keys[] = { "scope", "access", "max_bytes",
                                         NULL };
  static const char *const exec_keys[] = { "max_timeout_ms",
                                           "max_output_bytes", NULL };
  static const char *const crypto_keys[] = { "key", "key_env", "key_file",
                                             "default_ttl", NULL };
  static const char *const listen_keys[] = {
    "port", "bind", "queue", "reply_queue", "max_body", "deadline_ms",
    "max_conns", "headers", NULL
  };
  /* 'path', 'mode' and 'max_rows' are build6's spellings, kept in the known
     list so a migrating deployment gets a sentence about what replaced them
     rather than a generic unknown-key refusal. */
  static const char *const sql_keys[] = { "scope", "access", "create",
                                          "max_result_rows", "path", "mode",
                                          "max_rows", NULL };
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
      int is_array;
      if (!lua_istable(L, -1)) {
        cfg_fail(err, errcap, "config.connectors.listen must be a table (one "
                              "listener) or an array of them%s%s", "", "");
        lua_pop(L, 2);
        goto done;
      }
      /* An array of blocks when [1] is a table -- the pre-bind-a-block-of-
         ports shape; a single block otherwise, the back-compatible one. */
      lua_rawgeti(L, -1, 1);
      is_array = lua_istable(L, -1);
      lua_pop(L, 1);
      if (is_array) {
        lua_Integer len = (lua_Integer)lua_rawlen(L, -1);
        lua_Integer i;
        if (len > DH_MAX_LISTENERS) {
          cfg_fail(err, errcap, "config.connectors.listen holds more than this "
                                "host's %s ports%s", "8", "");
          lua_pop(L, 2);
          goto done;
        }
        for (i = 1; i <= len; i++) {
          char where[48];
          dh_listener_cfg *l = &out->listeners[out->nlisteners];
          size_t j;
          snprintf(where, sizeof(where), "config.connectors.listen[%d]",
                   (int)i);
          lua_rawgeti(L, -1, i);
          if (!lua_istable(L, -1)) {
            cfg_fail(err, errcap, "%s must be a table%s", where, "");
            lua_pop(L, 3);
            goto done;
          }
          if (cfg_known_keys(L, -1, listen_keys, where, err, errcap) != 0) {
            lua_pop(L, 3);
            goto done;
          }
          listener_defaults(l);
          if (cfg_listener(L, -1, l, where, err, errcap) != 0) {
            lua_pop(L, 3);
            goto done;
          }
          /* No two listeners on one port: the second bind would fail with an
             errno, but a config refusal names the mistake instead. */
          for (j = 0; j < out->nlisteners; j++) {
            if (out->listeners[j].port == l->port) {
              cfg_fail(err, errcap, "%s repeats a port already bound%s",
                       where, "");
              lua_pop(L, 3);
              goto done;
            }
          }
          out->nlisteners++;
          lua_pop(L, 1);
        }
      }
      else {
        dh_listener_cfg *l = &out->listeners[0];
        if (cfg_known_keys(L, -1, listen_keys, "config.connectors.listen",
                           err, errcap) != 0) {
          lua_pop(L, 2);
          goto done;
        }
        listener_defaults(l);
        if (cfg_listener(L, -1, l, "config.connectors.listen", err, errcap)
            != 0) {
          lua_pop(L, 2);
          goto done;
        }
        out->nlisteners = 1;
      }
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
      /* The build6 keys, refused with directions rather than left to the
         generic unknown-key sentence: the shape changed, not just a name. */
      lua_getfield(L, -1, "path");
      if (!lua_isnil(L, -1)) {
        lua_pop(L, 3);
        cfg_fail(err, errcap, "config.connectors.sql.path was replaced in "
                              "build7: grant a directory with 'scope' and "
                              "let the program name its database "
                              "(host.sql.open)%s%s", "", "");
        goto done;
      }
      lua_pop(L, 1);
      lua_getfield(L, -1, "mode");
      if (!lua_isnil(L, -1)) {
        lua_pop(L, 3);
        cfg_fail(err, errcap, "config.connectors.sql.mode was split in "
                              "build7: 'access' (\"read\"/\"readwrite\") is "
                              "the grant, 'create' the open-mode detail%s%s",
                 "", "");
        goto done;
      }
      lua_pop(L, 1);
      lua_getfield(L, -1, "max_rows");
      if (!lua_isnil(L, -1)) {
        lua_pop(L, 3);
        cfg_fail(err, errcap, "config.connectors.sql.max_rows was renamed in "
                              "build7 to what it always was: "
                              "'max_result_rows', a per-query cap%s%s",
                 "", "");
        goto done;
      }
      lua_pop(L, 1);
      if (cfg_str(L, -1, "scope", out->sql.scope, sizeof(out->sql.scope), 1,
                  "config.connectors.sql", err, errcap) != 0) {
        lua_pop(L, 2);
        goto done;
      }
      out->sql.enabled = 1;
      lua_getfield(L, -1, "access");
      if (!lua_isnil(L, -1)) {
        const char *s = lua_tostring(L, -1);
        if (s == NULL || (strcmp(s, "read") != 0 &&
                          strcmp(s, "readwrite") != 0)) {
          lua_pop(L, 3);
          cfg_fail(err, errcap, "config.connectors.sql.access must be "
                                "\"read\" or \"readwrite\"%s%s", "", "");
          goto done;
        }
        out->sql.readwrite = (strcmp(s, "readwrite") == 0);
      }
      lua_pop(L, 1);
      /* Safe default: creation follows the write grant. Saying create=true
         under a read grant is a contradiction (a read-only open cannot
         create), so it is refused rather than half-honored. */
      out->sql.create = out->sql.readwrite;
      lua_getfield(L, -1, "create");
      if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1)) {
          lua_pop(L, 3);
          cfg_fail(err, errcap, "config.connectors.sql.create must be a "
                                "boolean%s%s", "", "");
          goto done;
        }
        out->sql.create = lua_toboolean(L, -1);
        if (out->sql.create && !out->sql.readwrite) {
          lua_pop(L, 3);
          cfg_fail(err, errcap, "config.connectors.sql.create needs access "
                                "\"readwrite\": a read-only open cannot "
                                "create a database%s%s", "", "");
          goto done;
        }
      }
      lua_pop(L, 1);
      n = out->sql.max_result_rows;
      if (cfg_num(L, -1, "max_result_rows", &n, 1, 1000000,
                  "config.connectors.sql", err, errcap) != 0) {
        lua_pop(L, 2); goto done;
      }
      out->sql.max_result_rows = (long)n;
    }
    lua_pop(L, 1);
    lua_getfield(L, -1, "crypto");
    if (!lua_isnil(L, -1)) {
      int sources = 0;
      if (!lua_istable(L, -1) ||
          cfg_known_keys(L, -1, crypto_keys, "config.connectors.crypto", err,
                         errcap) != 0) {
        if (!lua_istable(L, -1))
          cfg_fail(err, errcap, "config.connectors.crypto must be a "
                                "table%s%s", "", "");
        lua_pop(L, 2);
        goto done;
      }
      out->crypto.enabled = 1;
      out->crypto.default_ttl = 3600;
      /* Exactly one key source. Inline 'key' is for dev; key_file and key_env
         keep the secret out of the config file, which is the shape a real
         deployment wants. */
      lua_getfield(L, -1, "key");
      if (lua_type(L, -1) == LUA_TSTRING) {
        size_t kn;
        const char *ks = lua_tolstring(L, -1, &kn);
        if (kn > sizeof(out->crypto.key)) {
          lua_pop(L, 3);
          cfg_fail(err, errcap, "config.connectors.crypto.key is too long%s%s",
                   "", "");
          goto done;
        }
        memcpy(out->crypto.key, ks, kn);
        out->crypto.keylen = kn;
        sources++;
      }
      lua_pop(L, 1);
      if (cfg_str(L, -1, "key_env", out->crypto.key_env,
                  sizeof(out->crypto.key_env), 0, "config.connectors.crypto",
                  err, errcap) != 0) { lua_pop(L, 2); goto done; }
      if (out->crypto.key_env[0] != '\0') sources++;
      if (cfg_str(L, -1, "key_file", out->crypto.key_file,
                  sizeof(out->crypto.key_file), 0, "config.connectors.crypto",
                  err, errcap) != 0) { lua_pop(L, 2); goto done; }
      if (out->crypto.key_file[0] != '\0') sources++;
      if (sources != 1) {
        lua_pop(L, 2);
        cfg_fail(err, errcap, "config.connectors.crypto needs exactly one of "
                              "key_file, key_env or key%s%s", "", "");
        goto done;
      }
      n = out->crypto.default_ttl;
      if (cfg_num(L, -1, "default_ttl", &n, 1, 315360000,
                  "config.connectors.crypto", err, errcap) != 0) {
        lua_pop(L, 2); goto done;
      }
      out->crypto.default_ttl = (long)n;
    }
    lua_pop(L, 1);
    lua_getfield(L, -1, "fs");
    if (!lua_isnil(L, -1)) {
      if (!lua_istable(L, -1) ||
          cfg_known_keys(L, -1, fs_keys, "config.connectors.fs", err,
                         errcap) != 0) {
        if (!lua_istable(L, -1))
          cfg_fail(err, errcap, "config.connectors.fs must be a table%s%s",
                   "", "");
        lua_pop(L, 2);
        goto done;
      }
      if (cfg_str(L, -1, "scope", out->fs.scope, sizeof(out->fs.scope), 1,
                  "config.connectors.fs", err, errcap) != 0) {
        lua_pop(L, 2);
        goto done;
      }
      out->fs.enabled = 1;
      lua_getfield(L, -1, "access");
      if (!lua_isnil(L, -1)) {
        const char *s = lua_tostring(L, -1);
        if (s == NULL || (strcmp(s, "read") != 0 &&
                          strcmp(s, "readwrite") != 0)) {
          lua_pop(L, 3);
          cfg_fail(err, errcap, "config.connectors.fs.access must be "
                                "\"read\" or \"readwrite\"%s%s", "", "");
          goto done;
        }
        out->fs.readwrite = (strcmp(s, "readwrite") == 0);
      }
      lua_pop(L, 1);
      n = out->fs.max_bytes;
      if (cfg_num(L, -1, "max_bytes", &n, 1, 1000000000,
                  "config.connectors.fs", err, errcap) != 0) {
        lua_pop(L, 2); goto done;
      }
      out->fs.max_bytes = (long)n;
    }
    lua_pop(L, 1);
    lua_getfield(L, -1, "exec");
    if (!lua_isnil(L, -1)) {
      /* 'exec = true' wires it with every bound at its default; a table
         tightens them. Granting exec is leaving the sandbox either way
         (doc/Capabilities.md), so the spelling is kept deliberate. */
      if (lua_isboolean(L, -1)) {
        out->exec.enabled = lua_toboolean(L, -1);
      }
      else if (!lua_istable(L, -1) ||
               cfg_known_keys(L, -1, exec_keys, "config.connectors.exec", err,
                              errcap) != 0) {
        if (!lua_istable(L, -1))
          cfg_fail(err, errcap, "config.connectors.exec must be a table or "
                                "true%s%s", "", "");
        lua_pop(L, 2);
        goto done;
      }
      else {
        out->exec.enabled = 1;
        n = out->exec.max_timeout_ms;
        if (cfg_num(L, -1, "max_timeout_ms", &n, 1, 600000,
                    "config.connectors.exec", err, errcap) != 0) {
          lua_pop(L, 2); goto done;
        }
        out->exec.max_timeout_ms = (long)n;
        n = out->exec.max_output_bytes;
        if (cfg_num(L, -1, "max_output_bytes", &n, 1, 1000000000,
                    "config.connectors.exec", err, errcap) != 0) {
          lua_pop(L, 2); goto done;
        }
        out->exec.max_output_bytes = (long)n;
      }
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  /* ----------------------------------------------------------------
  ** plugins = { rest = { manifest = "rest.plugin.json", ... } }
  **
  ** Sibling to 'connectors' because that is what it is: a capability the
  ** deployment grants. The manifest resolves against the CONFIG file's
  ** directory, not the host's cwd -- the ambiguity build 7 removed from
  ** the sql scope is not one to reintroduce here.
  ** ---------------------------------------------------------------- */
  if (lua_getfield(L, -1, "plugins") != LUA_TNIL) {
    if (!lua_istable(L, -1)) {
      cfg_fail(err, errcap, "'plugins' must be a table of named plugins%s%s",
               "", "");
      goto done;
    }
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
      dh_plugin_cfg *pc;
      const char *pname;
      if (lua_type(L, -2) != LUA_TSTRING) {
        cfg_fail(err, errcap, "every plugin is named by a string key%s%s", "",
                 "");
        goto done;
      }
      pname = lua_tostring(L, -2);
      if (!lua_istable(L, -1)) {
        cfg_fail(err, errcap, "plugin '%s' must be a table%s", pname, "");
        goto done;
      }
      if (out->nplugins >= DH_MAX_PLUGINS) {
        cfg_fail(err, errcap, "more plugins than this host holds, starting at "
                              "'%s'%s", pname, "");
        goto done;
      }
      pc = &out->plugins[out->nplugins];
      memset(pc, 0, sizeof(*pc));
      if (strlen(pname) >= sizeof(pc->name) || strchr(pname, '/') != NULL) {
        cfg_fail(err, errcap, "'%s' is not usable as a plugin name: it becomes "
                              "the first segment of every call it answers%s",
                 pname, "");
        goto done;
      }
      strcpy(pc->name, pname);
      {
        char buf[DH_NAME_MAX + 16];
        snprintf(buf, sizeof(buf), "plugins.%s", pname);
        if (cfg_known_keys(L, -1, plugin_keys, buf, err, errcap) != 0)
          goto done;
        if (cfg_str(L, -1, "manifest", pc->manifest, sizeof(pc->manifest), 1,
                    buf, err, errcap) != 0)
          goto done;
        n = 0;
        if (cfg_num(L, -1, "max_inflight", &n, 1, 4096, buf, err, errcap) != 0)
          goto done;
        pc->max_inflight = (long)n;      /* 0 = defer to the manifest */
        n = 0;
        if (cfg_num(L, -1, "call_timeout_ms", &n, 1, 3600000, buf, err,
                    errcap) != 0)
          goto done;
        pc->call_timeout_ms = (long)n;
      }
      /* Resolve the manifest beside the config that named it, then read it
         now: a deployment whose plugin manifest is missing or malformed
         should fail at load with the rest of the config's refusals, not at
         the first call. */
      {
        char full[DH_PATH_MAX];
        if (pc->manifest[0] != '/') {
          const char *slash = strrchr(path, '/');
          if (slash != NULL) {
            size_t dlen = (size_t)(slash - path) + 1;
            if (dlen + strlen(pc->manifest) >= sizeof(full)) {
              cfg_fail(err, errcap, "the path to plugin '%s''s manifest is too "
                                    "long%s", pname, "");
              goto done;
            }
            memcpy(full, path, dlen);
            strcpy(full + dlen, pc->manifest);
          }
          else
            snprintf(full, sizeof(full), "%s", pc->manifest);
        }
        else
          snprintf(full, sizeof(full), "%s", pc->manifest);
        if (pc->call_timeout_ms <= 0)
          pc->call_timeout_ms = 30000;
        if (dh_plugin_manifest_load(full, pc, err, errcap) != 0)
          goto done;
      }
      out->nplugins++;
      lua_pop(L, 1);                     /* the plugin's table; key stays */
    }
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
     create that returns NULL is never told the instance went away, which is
     why this host keeps a roster. */
  dh_slot *sc = (dh_slot *)calloc(1, sizeof(dh_slot));
  (void)inst;
  if (sc == NULL)
    return NULL;
  sc->id = id;
  sc->next = h->slots;
  h->slots = sc;
  return sc;
}

static void pending_drop_instance (dh_host *h, dvs_id id, int all);

static void host_destroy (void *ud, dvs_id id, void *ctx) {
  dh_host *h = (dh_host *)ud;
  dh_slot **pp = &h->slots;
  /* Whatever this instance was owed, nobody will collect. Dropping the
     entries here is what keeps the ledger bounded by live instances rather
     than by the host's lifetime, and it is where a connector learns to
     abandon work whose answer now has nowhere to go. */
  pending_drop_instance(h, id, 0);
  while (*pp != NULL && *pp != ctx)
    pp = &(*pp)->next;
  if (*pp != NULL)
    *pp = (*pp)->next;
  free(ctx);
}

/*
** Duty 2's inner step, one instance: a single-instance drive loop, generalized.
** Deliver
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

int dh_register_deferrable (dh_host *h, const char *prefix, dh_call_fn fn,
                            dh_cancel_fn cancel, void *ud) {
  size_t i;
  if (h->nconns >= DH_MAX_CONNECTORS || strlen(prefix) >= 32)
    return -1;
  for (i = 0; i < h->nconns; i++) {
    if (strcmp(h->conns[i].prefix, prefix) == 0)
      return -1;
  }
  strcpy(h->conns[h->nconns].prefix, prefix);
  h->conns[h->nconns].fn = fn;
  h->conns[h->nconns].cancel = cancel;
  h->conns[h->nconns].ud = ud;
  h->nconns++;
  return 0;
}

int dh_register (dh_host *h, const char *prefix, dh_call_fn fn, void *ud) {
  return dh_register_deferrable(h, prefix, fn, NULL, ud);
}


/* ======================================================================
** The deferred-reply ledger.
**
** A list rather than a hash: the entries are bounded by max_inflight summed
** over the wired plugins, which is a small number by construction, and a
** list keeps the sweeps (by instance, by deadline) trivially correct. If
** that bound ever stops holding, this is the thing to index -- not before.
** ====================================================================== */

/* The connector currently being invoked, so dh_defer can attribute the entry
** without threading a handle through dh_call_fn. Set by 'answer' around the
** one call it makes, and read only there: a connector cannot reach dh_defer
** except from inside its own dh_call_fn, which is exactly when this is
** valid. */
static size_t dh_calling_conn = (size_t)-1;

static dh_pending **pending_find (dh_host *h, dvs_id id, int64_t tok) {
  dh_pending **pp = &h->pending;
  while (*pp != NULL) {
    if ((*pp)->id == id && (*pp)->tok == tok)
      return pp;
    pp = &(*pp)->next;
  }
  return NULL;
}

int dh_defer (dh_host *h, dvs_id id, int64_t tok, void *callud,
              int64_t deadline_ms) {
  dh_pending *e;
  if (dh_calling_conn >= h->nconns)
    return -1;                           /* not inside a connector call */
  if (pending_find(h, id, tok) != NULL)
    return -1;                           /* (id, tok) is already owed */
  e = (dh_pending *)calloc(1, sizeof(dh_pending));
  if (e == NULL)
    return -1;
  e->id = id;
  e->tok = tok;
  e->conn = dh_calling_conn;
  e->callud = callud;
  e->deadline_ms = deadline_ms;
  e->next = h->pending;
  h->pending = e;
  h->npending++;
  return 0;
}

size_t dh_pending_count (dh_host *h, dvs_id id) {
  dh_pending *e;
  size_t n = 0;
  for (e = h->pending; e != NULL; e = e->next) {
    if (e->id == id)
      n++;
  }
  return n;
}

/*
** Build the reply envelope. Shared by the inline path in 'answer' and the
** deferred path in 'dh_reply' so the two cannot drift: a guest must not be
** able to tell from the bytes whether its call was answered at once or an
** hour later.
*/
static void envelope (dh_buf *reply, int64_t tok, dh_call_status st,
                      const unsigned char *value, size_t vlen,
                      const char *detail) {
  dh_map(reply, 3);
  dh_str(reply, "tok"); dh_int(reply, tok);
  dh_str(reply, "status");
  dh_str(reply, (st == DH_CALL_OK) ? "ok"
               : (st == DH_CALL_DENIED) ? "denied" : "error");
  if (st == DH_CALL_OK) {
    dh_str(reply, "value");
    if (vlen > 0)
      dh_raw(reply, value, vlen);
    else
      dh_nil(reply);
  }
  else {
    dh_str(reply, "detail");
    dh_str(reply, (detail != NULL && detail[0] != '\0')
                  ? detail
                  : "the connector refused without saying why, which is its "
                    "bug");
  }
}

int dh_reply (dh_host *h, dvs_id id, int64_t tok, dh_call_status st,
              const unsigned char *value, size_t vlen, const char *detail) {
  dh_pending **pp = pending_find(h, id, tok);
  dh_pending *e;
  dv_instance *inst;
  dh_buf reply;
  int rc = -1;
  if (pp == NULL)
    return -1;   /* swept already: the instance died, or the deadline fired */
  e = *pp;
  *pp = e->next;
  h->npending--;
  free(e);
  /* The instance may have hibernated between the deferral and now, in which
     case its queues went with it and the reply waits for the wake the same
     way any other undelivered message does -- there is nothing to push to
     yet. And it may simply be gone, which is the drop the ledger exists to
     make silent rather than fatal. */
  inst = dvs_instance(h->sw, id);
  if (inst == NULL)
    return -1;
  {
    dv_queue_id replies = dv_queue_lookup(inst, DH_REPLIES_QUEUE);
    if (replies == 0)
      return -1;                         /* asked, declared nowhere to hear */
    dh_buf_init(&reply);
    envelope(&reply, tok, st, value, vlen, detail);
    if (reply.len > 0 && dv_queue_push(inst, replies, reply.p, reply.len)
        == DV_OK)
      rc = 0;
    dh_buf_free(&reply);
  }
  return rc;
}

/*
** Drop every entry a connector will never be able to collect, telling the
** connector so it can abandon the work. Called when an instance dies and
** when the host closes.
*/
static void pending_drop_instance (dh_host *h, dvs_id id, int all) {
  dh_pending **pp = &h->pending;
  while (*pp != NULL) {
    dh_pending *e = *pp;
    if (all || e->id == id) {
      dh_cancel_fn cancel = (e->conn < h->nconns) ? h->conns[e->conn].cancel
                                                  : NULL;
      void *ud = (e->conn < h->nconns) ? h->conns[e->conn].ud : NULL;
      *pp = e->next;
      h->npending--;
      if (cancel != NULL)
        cancel(ud, e->id, e->tok, e->callud);
      free(e);
    }
    else
      pp = &e->next;
  }
}

/*
** The host-side backstop. A connector that dies mid-call would otherwise
** leave its entry forever, and the guest -- whose own queue.wait timeout has
** long since fired -- would hold reply-queue headroom it will never use.
*/
static void pending_fire_deadlines (dh_host *h) {
  dh_pending **pp = &h->pending;
  while (*pp != NULL) {
    dh_pending *e = *pp;
    if (e->deadline_ms > 0 && e->deadline_ms <= h->now_ms) {
      dh_cancel_fn cancel = (e->conn < h->nconns) ? h->conns[e->conn].cancel
                                                  : NULL;
      void *ud = (e->conn < h->nconns) ? h->conns[e->conn].ud : NULL;
      dvs_id id = e->id;
      int64_t tok = e->tok;
      void *callud = e->callud;
      *pp = e->next;
      h->npending--;
      free(e);
      if (cancel != NULL)
        cancel(ud, id, tok, callud);
      /* Answer it, so the guest gets a sentence rather than a silence. The
         entry is already unlinked, so this cannot recurse into itself. */
      {
        dv_instance *inst = dvs_instance(h->sw, id);
        if (inst != NULL) {
          dv_queue_id replies = dv_queue_lookup(inst, DH_REPLIES_QUEUE);
          if (replies != 0) {
            dh_buf reply;
            dh_buf_init(&reply);
            envelope(&reply, tok, DH_CALL_ERROR, NULL, 0,
                     "the connector that took this call never answered it; "
                     "the host reclaimed the request at its deadline");
            if (reply.len > 0)
              dv_queue_push(inst, replies, reply.p, reply.len);
            dh_buf_free(&reply);
          }
        }
      }
    }
    else
      pp = &e->next;
  }
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

  dh_calling_conn = (size_t)(conn - h->conns);
  st = conn->fn(conn->ud, id, tok, call, args, argslen, &value, detail,
                sizeof(detail));
  dh_calling_conn = (size_t)-1;

  if (st == DH_CALL_PENDING) {
    /* Taken. The reply buffer stays empty, so pump_instance pushes nothing
       and the request is still consumed -- the calls queue cannot wedge on a
       call that is merely slow. The connector owes exactly one dh_reply for
       this (id, tok), and the ledger is what will notice if it never comes.

       A connector that returned PENDING without recording the entry has a
       bug that would otherwise be silent: it would never be answered and
       never be counted. Answer it here instead, because a guest deserves a
       sentence and the alternative is a hang. */
    if (pending_find(h, id, tok) == NULL) {
      envelope(reply, tok, DH_CALL_ERROR, NULL, 0,
               "the connector deferred this call without recording it, which "
               "is its bug: a deferred call must be taken with dh_defer");
    }
    dh_buf_free(&value);
    return;
  }

  envelope(reply, tok, st, value.p, value.len, detail);
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
      /* Pending calls count against the headroom. One has been drained but
         has produced no reply yet, so without this term the queue looks
         emptier than it is: defer twenty calls into a sixteen-deep queue and
         four answers have nowhere to land, and the guest waits out a timeout
         for a reply the host already computed. This is the same flow control
         a plugin's max_inflight applies at the pipe, applied here at the
         queue -- and this is the more fundamental of the two, because it
         bounds what the guest can be *owed* rather than what the far side
         can be sent. */
      if (dv_queue_state(inst, replies, &info) == DV_OK && info.capacity > 0 &&
          info.len + dh_pending_count(h, sc->id) >= (size_t)info.capacity)
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

static dh_call_status conn_time (void *ud, dvs_id id, int64_t tok,
                                 const char *call,
                                 const unsigned char *args, size_t argslen,
                                 dh_buf *value, char *detail,
                                 size_t detailcap) {
  struct timespec ts;
  (void)ud; (void)id; (void)tok; (void)args; (void)argslen;
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

  /* A peer closing a socket mid-write, or an exec child closing its stdin,
     must be an EPIPE the caller sees -- not a SIGPIPE that kills the whole
     host. Process-wide, set here once rather than inside whichever
     connector happened to notice first. */
  signal(SIGPIPE, SIG_IGN);

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
  if (cfg->crypto.enabled && dh_crypto_open(h, err, errcap) != 0) {
    dh_host_close(h);
    return -1;
  }
  if (cfg->fs.enabled && dh_fs_open(h, err, errcap) != 0) {
    dh_host_close(h);
    return -1;
  }
  if (cfg->exec.enabled && dh_exec_open(h, err, errcap) != 0) {
    dh_host_close(h);
    return -1;
  }
  if (cfg->nlisteners > 0 && dh_http_open(h, err, errcap) != 0) {
    dh_host_close(h);
    return -1;
  }
  if (cfg->nplugins > 0 && dh_plug_open(h, err, errcap) != 0) {
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
  /* Before the connectors go: every pending entry names one of them, and a
     cancel that ran after its connector closed would reach freed state. */
  pending_drop_instance(h, 0, 1);
  dh_plug_close(h);
  dh_http_close(h);
  dh_sql_close(h);
  dh_crypto_close(h);
  dh_fs_close(h);
  dh_exec_close(h);
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
  pending_fire_deadlines(h);
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
  dh_pending *e;
  int64_t next = -1;
  int http;
  for (sc = h->slots; sc != NULL; sc = sc->next) {
    if (sc->has_deadline && (next < 0 || sc->deadline_ms < next))
      next = sc->deadline_ms;
  }
  /* And the ledger's own backstops, for the same reason the guest timeouts
     are here: a sleep that outran one would let a dead connector's entry sit
     past the moment it was supposed to be reclaimed. */
  for (e = h->pending; e != NULL; e = e->next) {
    if (e->deadline_ms > 0 && (next < 0 || e->deadline_ms < next))
      next = e->deadline_ms;
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


/*
** The one place the host sleeps.
**
** The listener's sockets, the plugin channels and a plain timeout are the
** same wait, so they share one poll(). Calling two sleeping polls in turn
** would not deadlock, but the first to sleep would delay the second by its
** whole timeout -- and "nothing blocks the shared thread" is only checkable
** if there is a single place the thread stops.
*/
void dh_host_sleep (dh_host *h, int timeout_ms) {
  struct pollfd extra[DH_MAX_PLUGINS];
  size_t nx = dh_plug_arm(h, extra, DH_MAX_PLUGINS);
  if (h->listener != NULL) {
    /* The listener owns the array; the plugin fds ride along inside its
       single poll and come back with their revents filled in. */
    dh_http_poll_with(h, timeout_ms, extra, nx);
  }
  else if (nx > 0) {
    if (poll(extra, (nfds_t)nx, timeout_ms) < 0 && errno != EINTR) {
      size_t i;
      for (i = 0; i < nx; i++)
        extra[i].revents = 0;
    }
  }
  else if (timeout_ms != 0) {
    struct timespec ts;
    int t = (timeout_ms < 0 || timeout_ms > 50) ? 50 : timeout_ms;
    ts.tv_sec = t / 1000;
    ts.tv_nsec = (long)(t % 1000) * 1000000;
    nanosleep(&ts, NULL);
  }
  dh_plug_fire(h, extra, nx);
}
