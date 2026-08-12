/*
** dhost.h
** The generic host: doc/Host.md's seven duties as one configurable binary.
**
** This is the C implementation of the host protocol -- the same contract
** lab's JavaScript host implements over the wasm boundary -- and the whole
** point of it being generic is that a deployment is a supervisor program plus
** a *.host.lua configuration, never C. It replaces the bespoke per-deployment
** C host each deployment used to hand-write.
**
** Layering: this sits where any host sits, outside the sandbox, speaking
** dv_* and dvs_* plus the msgpack codec's public cursor. It includes no Lua
** core header for *instance* work; the one lua_State it makes for itself is
** for reading its own configuration, which is host property, evaluated in an
** empty environment and refused unless it is data all the way down.
*/

#ifndef dhost_h
#define dhost_h

#include <stddef.h>
#include <stdint.h>

#include "dv.h"
#include "dvs.h"


/* ======================================================================
** A growable byte buffer, with msgpack emitters.
**
** The runtime's public codec surface is a read cursor; writers in this tree
** are hand-rolled where they are needed (dvs.c's emit_event is the
** precedent). These are that pattern given a home, bounds-checked by growth
** rather than by clamping: a reply that will not fit grows the buffer, and
** only allocation failure refuses.
** ====================================================================== */

typedef struct dh_buf {
  unsigned char *p;
  size_t len;
  size_t cap;
} dh_buf;

void dh_buf_init (dh_buf *b);
void dh_buf_free (dh_buf *b);
/* All return 0, or -1 on allocation failure with the buffer unchanged in
   length (partial growth is invisible to the caller). */
int dh_raw (dh_buf *b, const void *p, size_t n);
int dh_map (dh_buf *b, unsigned n);
int dh_array (dh_buf *b, unsigned n);
int dh_str (dh_buf *b, const char *s);
int dh_lstr (dh_buf *b, const char *s, size_t n);
int dh_uint (dh_buf *b, uint64_t v);
int dh_int (dh_buf *b, int64_t v);
int dh_double (dh_buf *b, double v);
int dh_bool (dh_buf *b, int v);
int dh_nil (dh_buf *b);


/* ======================================================================
** Configuration: a *.host.lua data literal, typed by host/types/host.lua.
**
** Loaded in an empty environment with text mode only, then refused unless
** the result is data all the way down -- no functions, no userdata, no
** metatables -- and unless every key is one this struct knows. An unknown
** key is a typo about to become a silent default, which is why it is an
** error and names itself.
** ====================================================================== */

#define DH_MAX_CAPS      16
#define DH_NAME_MAX      128
#define DH_PATH_MAX      512
#define CRYPTO_KEY_INLINE 256
#define DH_MAX_LISTENERS 8

typedef struct dh_listener_cfg {
  int port;
  char bind_addr[DH_NAME_MAX];          /* default 127.0.0.1: the LB's side */
  char queue[DH_NAME_MAX];              /* requests land here, on the root */
  char reply_queue[DH_NAME_MAX];        /* responses drain from here */
  long max_body;                        /* refuse bigger request bodies */
  long conn_deadline_ms;                /* the host-side timeout, per conn */
  int max_conns;
} dh_listener_cfg;

typedef struct dh_sql_cfg {
  int enabled;
  char path[DH_PATH_MAX];               /* the database file */
  int readwrite;                        /* 0: host:sql/read only */
  long max_rows;                        /* result cap; the fork bomb, in rows */
} dh_sql_cfg;

typedef struct dh_crypto_cfg {
  int enabled;
  char key[CRYPTO_KEY_INLINE];          /* inline key (dev); "" if unset */
  size_t keylen;
  char key_env[DH_NAME_MAX];            /* env var holding the key */
  char key_file[DH_PATH_MAX];           /* file holding the key */
  long default_ttl;                     /* jwt_sign ttl when the call omits it */
} dh_crypto_cfg;

typedef struct dh_config {
  char supervisor[DH_PATH_MAX];         /* required: the root program's file */
  uint32_t max_instances;
  uint32_t spawns_per_step;
  char identity[DH_NAME_MAX];           /* "" = unstamped */
  int hibernation;                      /* default on, like the runtime */
  char caps[DH_MAX_CAPS][DH_NAME_MAX];  /* the root's ceiling */
  size_t ncaps;
  uint64_t instructions;                /* the root's budget; 0 = none */
  uint64_t memory_kb;
  int time_connector;                   /* connectors = { time = true } */
  dh_listener_cfg listeners[DH_MAX_LISTENERS]; /* connectors.listen: a block,
                                           or an array of blocks (pre-bind a
                                           block of ports) */
  size_t nlisteners;                    /* 0 = no listener */
  dh_sql_cfg sql;                       /* connectors = { sql = {...} } */
  dh_crypto_cfg crypto;                 /* connectors = { crypto = {...} } */
} dh_config;

/* 0 on success. Nonzero leaves a sentence in 'err' saying which key and why
   -- the config is the one interface a non-programmer touches, so its
   refusals are worded harder than most. */
int dh_config_load (const char *path, dh_config *out, char *err, size_t errcap);


/* ======================================================================
** Connectors: doc/Hostcall.md's host half.
**
** A connector owns a family of call names ("sql" owns "sql/query"); the
** pump routes on the segment before the first '/'. By the time a connector
** runs, the request has already parsed and the capability check has already
** passed -- "host:" + the call name, against the calling instance's grant,
** via dvs_holds -- so a connector only ever sees calls it is entitled to
** answer. It appends its reply *value* as msgpack and returns OK, or fills
** 'detail' and returns ERROR/DENIED; the pump owns the envelope.
** ====================================================================== */

typedef enum dh_call_status {
  DH_CALL_OK = 0,
  DH_CALL_ERROR,
  DH_CALL_DENIED
} dh_call_status;

typedef dh_call_status (*dh_call_fn) (void *ud, dvs_id id, const char *call,
                                      const unsigned char *args, size_t argslen,
                                      dh_buf *value,
                                      char *detail, size_t detailcap);

#define DH_MAX_CONNECTORS 8

typedef struct dh_connector {
  char prefix[32];
  dh_call_fn fn;
  void *ud;
} dh_connector;


/* ======================================================================
** The host itself.
** ====================================================================== */

#define DH_CALLS_QUEUE   "host/calls"    /* guest declares exported; host drains */
#define DH_REPLIES_QUEUE "host/replies"  /* host pushes; guest waits on it */

typedef struct dh_slot dh_slot;

typedef struct dh_host {
  dvs_swarm *sw;
  dvs_id root;                          /* the supervisor's id */
  dh_config cfg;
  dh_connector conns[DH_MAX_CONNECTORS];
  size_t nconns;
  int64_t now_ms;                       /* monotonic, refreshed per loop turn */
  dh_slot *slots;                       /* roster head; entries are the ctx
                                           pointers dvs hands back */
  void *listener;                       /* the http connector's state, or NULL */
  void *sqlctx;                         /* the sql connector's state, or NULL */
  void *cryptoctx;                      /* the crypto connector's state, or NULL */
} dh_host;

/* Roster entry -- also the per-instance ctx 'create' returns, which must be
   non-NULL or 'destroy' is never called (dvs guards it on a non-NULL ctx). */
struct dh_slot {
  dvs_id id;
  int has_deadline;                     /* a guest wait with a timeout */
  int64_t deadline_ms;
  uint32_t rr_next;                     /* round-robin over the wait-set */
  dh_slot *next;
};

/* Wire a swarm to a loaded config: dvs_new with the host vtable, identity,
   hibernation policy, connectors from the config, and the supervisor as
   root. 0 on success; nonzero with a sentence in 'err'. */
int dh_host_open (dh_host *h, const dh_config *cfg, char *err, size_t errcap);
void dh_host_close (dh_host *h);

/* One turn of duty 2 and duty 5: dvs_step, fire elapsed guest wait
   timeouts, drain every instance's host/calls. Returns dvs_alive. */
int dh_host_turn (dh_host *h);

/* The next deadline this host is waiting on (guest wait timeouts), as a
   poll() timeout in ms; -1 when there is none. The listener shortens it
   further with its own connection deadlines. */
int dh_host_poll_timeout (dh_host *h);

int64_t dh_now_ms (void);

/* Register a connector; 0 on success, nonzero when the table is full or the
   prefix is already taken. Exposed for tests and for connectors that live
   outside this file. */
int dh_register (dh_host *h, const char *prefix, dh_call_fn fn, void *ud);

/* The built-in listener and sql connectors, implemented in dhost_http.c and
   dhost_sql.c; opened by dh_host_open when their config blocks say so. */
int dh_http_open (dh_host *h, char *err, size_t errcap);
void dh_http_close (dh_host *h);
/* Service sockets: accept, read, parse, push requests in; pop replies out,
   write, enforce per-connection deadlines. 'timeout_ms' is how long poll may
   sleep when nothing is ready. */
int dh_http_poll (dh_host *h, int timeout_ms);

/* Milliseconds until the earliest live connection deadline, or -1 when the
   listener holds none. Lets dh_host_poll_timeout bound a quiet-socket sleep
   so a slow client's deadline fires near its moment. */
int dh_http_next_timeout (dh_host *h);

int dh_sql_open (dh_host *h, char *err, size_t errcap);
void dh_sql_close (dh_host *h);

int dh_crypto_open (dh_host *h, char *err, size_t errcap);
void dh_crypto_close (dh_host *h);

#endif
