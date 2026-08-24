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

#include <poll.h>
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
#define DH_MAX_TURN_URIS   8
#define DH_TURN_URI_MAX  160
#define DH_MAX_SECRETS     8
#define DH_SECRET_NAME_MAX 32
#define DH_MAX_LISTENERS 8

#define DH_MAX_HDRS      16
#define DH_HDR_NAME_MAX  64

typedef struct dh_listener_cfg {
  int port;
  char bind_addr[DH_NAME_MAX];          /* default 127.0.0.1: the LB's side */
  char queue[DH_NAME_MAX];              /* requests land here, on the root */
  char reply_queue[DH_NAME_MAX];        /* responses drain from here */
  long max_body;                        /* refuse bigger request bodies */
  long conn_deadline_ms;                /* the host-side timeout, per conn */
  int max_conns;
  char headers[DH_MAX_HDRS][DH_HDR_NAME_MAX];  /* lowercased allowlist of
                                           request headers forwarded to the
                                           guest; empty = none (default) */
  size_t nheaders;
  char resp_headers[DH_MAX_HDRS][DH_HDR_NAME_MAX];  /* lowercased allowlist
                                           of response headers a guest reply
                                           may set; empty = none (default) */
  size_t nresp_headers;
} dh_listener_cfg;

typedef struct dh_sql_cfg {
  int enabled;
  char scope[DH_PATH_MAX];              /* the granted directory; programs
                                           name their databases within it */
  int readwrite;                        /* access: 0 leaves sql/exec unwired */
  int create;                           /* may a named database be created?
                                           needs readwrite; defaults to it */
  long max_result_rows;                 /* per-query result cap */
} dh_sql_cfg;

typedef struct dh_crypto_cfg {
  int enabled;
  char key[CRYPTO_KEY_INLINE];          /* inline key (dev); "" if unset */
  size_t keylen;
  char key_env[DH_NAME_MAX];            /* env var holding the key */
  char key_file[DH_PATH_MAX];           /* file holding the key */
  long default_ttl;                     /* jwt_sign ttl when the call omits it */
  /* connectors.crypto.turn: the TURN REST shared secret (coturn's
     use-auth-secret). Held raw, not derived -- see dhost_crypto.c. */
  int turn_enabled;
  char turn_secret[CRYPTO_KEY_INLINE];  /* inline secret (dev); "" if unset */
  size_t turn_secretlen;
  char turn_secret_env[DH_NAME_MAX];    /* env var holding the secret */
  char turn_secret_file[DH_PATH_MAX];   /* file holding the secret */
  long turn_ttl;                        /* credential ttl when a call omits it */
  char turn_uris[DH_MAX_TURN_URIS][DH_TURN_URI_MAX];  /* echoed verbatim in
                                           turn_credential replies: where the
                                           TURN server lives is deployment
                                           data, not program code */
  int turn_nuris;
  /* connectors.crypto.secrets: named raw secrets for crypto/hmac interop
     (webhook verification), each the same three-source shape. */
  struct dh_named_secret_cfg {
    char name[DH_SECRET_NAME_MAX];
    char secret[CRYPTO_KEY_INLINE];     /* inline (dev); "" if unset */
    size_t secretlen;
    char secret_env[DH_NAME_MAX];
    char secret_file[DH_PATH_MAX];
  } secrets[DH_MAX_SECRETS];
  int nsecrets;
} dh_crypto_cfg;

typedef struct dh_fs_cfg {
  int enabled;
  char scope[DH_PATH_MAX];              /* the granted directory */
  int readwrite;                        /* access: 0 leaves fs/write unwired */
  long max_bytes;                       /* cap on a read and on a write */
} dh_fs_cfg;

typedef struct dh_exec_cfg {
  int enabled;
  long max_timeout_ms;                  /* ceiling; a call may ask for less */
  long max_output_bytes;                /* per stream, and on stdin */
} dh_exec_cfg;


/* ----------------------------------------------------------------------
** Plugins: a capability that lives in another program.
**
** The deployment names a plugin and points at its manifest; the manifest
** -- a self-contained '<name>.plugin.json' -- says what the plugin is and
** what it answers. The split is deliberate: the manifest is the plugin
** author's document and travels with the plugin, the config is the
** operator's and says which plugins this deployment wires and how hard it
** will lean on them.
**
** The runtime reads only the flat metadata here. A capability's 'args' and
** 'result' schemas are read past without being looked at: validation is the
** plugin's job and the tooling's, and no JSON Schema validator belongs in a
** host that has to stay small enough to static-link.
** ---------------------------------------------------------------------- */

#define DH_MAX_PLUGINS      8
#define DH_MAX_PLUGIN_CAPS  32
#define DH_CHECKSUM_MAX     96


/* ----------------------------------------------------------------------
** Visibility: what a caller is told EXISTS, which is not what it may DO.
**
** doc/Capabilities.md keeps three axes apart -- capability (what a host can
** do; the menu, declared once, and "restricting a program can never shrink
** it"), permission (grant or deny, per instance, attenuated), and scope
** (what a permission applies to). Discovery reads the first. Conflating it
** with the second would make the host lie about itself: a program without
** the grant would be told the capability does not exist, when what is true
** is that it exists and is not theirs.
**
** So the default is 'public', and a listing entry carries 'granted'
** alongside the name. A caller can then tell "this host cannot do that"
** from "this host can, and I may not" -- two different problems with two
** different fixes, and today a program cannot distinguish them at all.
** It also makes an auditor agent possible: one permitted to SEE the menu
** and not to use it.
**
** 'inherit' is the field's future. Once a configuration is the same object
** at every depth (Capabilities.md section 2) and the host is simply the
** root's parent, visibility propagates and attenuates down the tree like
** everything else, and only the enclosing default changes -- not the shape
** of this field.
** ---------------------------------------------------------------------- */
typedef enum dh_visibility {
  DH_VIS_INHERIT = 0,                   /* take the enclosing default */
  DH_VIS_PUBLIC,                        /* listed to any caller, held or not */
  DH_VIS_PRIVATE,                       /* listed only to callers that hold it */
  DH_VIS_HIDDEN                         /* never listed; callable if held */
} dh_visibility;

/* "public" | "private" | "hidden" | "inherit" -> the enum; -1 for anything
   else, so a caller can refuse it by name. */
int dh_visibility_of (const char *s);
const char *dh_visibility_name (dh_visibility v);

/* What to do with a call that was in flight when its instance hibernated.
** The host-side pending state is not in the snapshot -- it cannot be, it
** lives outside the sandbox -- so a wake has to decide, and the capability
** is the only party that knows whether reissuing is safe. ERROR is the
** default because it is the answer that is never silently wrong. */
typedef enum dh_wake_policy {
  DH_WAKE_ERROR = 0,
  DH_WAKE_REISSUE,                      /* idempotent: ask again */
  DH_WAKE_CACHED                        /* completed already: replay it */
} dh_wake_policy;

typedef struct dh_plugin_cap {
  char name[DH_NAME_MAX];               /* "get"; the call is "<plugin>/get" */
  dh_wake_policy wake;
} dh_plugin_cap;

typedef struct dh_plugin_cfg {
  char name[DH_NAME_MAX];               /* the connector prefix: "rest" */
  char manifest[DH_PATH_MAX];           /* resolved against the config file */
  char exec[DH_PATH_MAX];               /* ABSOLUTE; execv, never execvp */
  char checksum[DH_CHECKSUM_MAX];       /* logged at startup, not enforced */
  long max_inflight;                    /* frames written before waiting */
  long call_timeout_ms;                 /* host-side backstop, per call */
  dh_visibility visibility;             /* default inherit -> the host's */
  dh_plugin_cap caps[DH_MAX_PLUGIN_CAPS];
  size_t ncaps;
} dh_plugin_cfg;

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
  dh_fs_cfg fs;                         /* connectors = { fs = {...} } */
  dh_exec_cfg exec;                     /* connectors = { exec = {...} } */
  dh_plugin_cfg plugins[DH_MAX_PLUGINS];   /* plugins = { name = {...} } */
  size_t nplugins;
  dh_visibility visibility;             /* the deployment's default; public */
  int insecure_plugins;                 /* --insecure-plugins; see doc/BUILD8 */
} dh_config;

/* Read a '<name>.plugin.json' manifest into 'out', which the caller has
   already filled with the deployment's own fields (name, manifest path, and
   any max_inflight override). Manifest values that the config did not
   override win; the config's overrides survive. 0 on success. */
int dh_plugin_manifest_load (const char *path, dh_plugin_cfg *out,
                             char *err, size_t errcap);

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
**
** Or it returns PENDING, and answers later. That is build8's seam: a
** connector that cannot answer now says so, the pump writes nothing, and
** the host goes back to driving the swarm. The connector then owes exactly
** one 'dh_reply' for the (id, tok) it was handed -- never zero, never two.
** The debt is why the pump keeps a table of them (see dh_pending): a reply
** that never comes is a leak, and a leak nobody counts is invisible.
**
** 'tok' is the request's correlation token, passed so a connector that
** defers can name the call it is answering. A connector that answers inline
** does not need it.
** ====================================================================== */

typedef enum dh_call_status {
  DH_CALL_OK = 0,
  DH_CALL_ERROR,
  DH_CALL_DENIED,
  DH_CALL_PENDING                       /* taken; one dh_reply is owed */
} dh_call_status;

typedef dh_call_status (*dh_call_fn) (void *ud, dvs_id id, int64_t tok,
                                      const char *call,
                                      const unsigned char *args, size_t argslen,
                                      dh_buf *value,
                                      char *detail, size_t detailcap);

/* Told that a pending call will never be collected -- its instance died, or
** the host is closing. The connector abandons whatever it started; it must
** NOT call dh_reply for that call, because the entry is already gone. */
typedef void (*dh_cancel_fn) (void *ud, dvs_id id, int64_t tok, void *callud);

#define DH_MAX_CONNECTORS 16

typedef struct dh_connector {
  char prefix[32];
  dh_call_fn fn;
  dh_cancel_fn cancel;                  /* optional; NULL when inline-only */
  /* The exact call names this connector answers, NULL-terminated -- what
     discovery reports. The router only ever matches the prefix, so without
     this a listing could say "sql" and not "sql/query", which is the shape
     of answer a person cannot act on. NULL means the connector did not say,
     and the prefix is reported alone. */
  const char *const *calls;
  dh_visibility visibility;
  void *ud;
} dh_connector;


/* ======================================================================
** Pending calls: the deferred-reply ledger.
**
** One entry per call a connector took as PENDING. The key is (id, tok) --
** two integers, not a pointer and not a registry reference: the host lives
** outside the sandbox and addresses instances by id, so a parked coroutine
** is kept alive by its instance and the instance by the swarm, exactly as
** it is for a guest parked on any other queue. Nothing here anchors a Lua
** object, so nothing here can leak one.
**
** What CAN leak is the entry itself, if a connector never answers. Two
** sweeps prevent it: the instance's death (host_destroy) drops its entries,
** and an entry with a deadline is reclaimed when it elapses.
** ====================================================================== */

typedef struct dh_pending dh_pending;

struct dh_pending {
  dvs_id id;
  int64_t tok;
  size_t conn;                          /* index into dh_host.conns */
  void *callud;                         /* the connector's handle for it */
  int64_t deadline_ms;                  /* 0 = no host-side backstop */
  dh_pending *next;
};


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
  dh_pending *pending;                  /* deferred-reply ledger head */
  size_t npending;                      /* entries, for the leak assertion */
  void *listener;                       /* the http connector's state, or NULL */
  void *sqlctx;                         /* the sql connector's state, or NULL */
  void *cryptoctx;                      /* the crypto connector's state, or NULL */
  void *fsctx;                          /* the fs connector's state, or NULL */
  void *execctx;                        /* the exec connector's state, or NULL */
  void *plugctx;                        /* the plugin channel's state, or NULL */
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
   outside this file. 'cancel' may be NULL for a connector that only ever
   answers inline. */
int dh_register (dh_host *h, const char *prefix, dh_call_fn fn, void *ud);
int dh_register_deferrable (dh_host *h, const char *prefix, dh_call_fn fn,
                            dh_cancel_fn cancel, void *ud);
/* The full form. 'calls' is a NULL-terminated list of the exact names this
   connector answers, for discovery; 'visibility' is DH_VIS_INHERIT to take
   the deployment's default. The two short forms above are this one with
   NULL and INHERIT. */
int dh_register_full (dh_host *h, const char *prefix, dh_call_fn fn,
                      dh_cancel_fn cancel, const char *const *calls,
                      dh_visibility visibility, void *ud);

/* ----------------------------------------------------------------------
** The deferred-reply half of the connector contract.
** ---------------------------------------------------------------------- */

/* Take ownership of the call currently being answered. Called by a connector
   from inside its dh_call_fn, immediately before it returns DH_CALL_PENDING.
   'callud' is the connector's own handle for the call and comes back to it
   in 'cancel'. 'deadline_ms' is an absolute dh_now_ms() moment after which
   the host reclaims the entry itself; 0 means the connector is trusted to
   answer. Returns 0, or nonzero when the entry could not be recorded -- in
   which case the connector must answer inline instead, because an unrecorded
   pending call is one nothing will ever collect. */
int dh_defer (dh_host *h, dvs_id id, int64_t tok, void *callud,
              int64_t deadline_ms);

/* Answer a call taken with dh_defer. Returns 0 when the reply was delivered,
   and nonzero when there was no such entry -- the normal outcome after the
   instance died or the deadline fired, and not an error the caller must
   handle beyond freeing its own state. 'value' is msgpack, or NULL with
   'detail' for the not-ok statuses. */
int dh_reply (dh_host *h, dvs_id id, int64_t tok, dh_call_status st,
              const unsigned char *value, size_t vlen, const char *detail);

/* How many replies this instance is still owed. The pump subtracts this from
   its reply-queue headroom: a pending call has been drained but has produced
   no reply yet, so without it the queue looks emptier than it is and a burst
   of deferrals can compute answers that have nowhere to land. */
size_t dh_pending_count (dh_host *h, dvs_id id);

/* The built-in listener and sql connectors, implemented in dhost_http.c and
   dhost_sql.c; opened by dh_host_open when their config blocks say so. */
int dh_http_open (dh_host *h, char *err, size_t errcap);
void dh_http_close (dh_host *h);
/* Service sockets: accept, read, parse, push requests in; pop replies out,
   write, enforce per-connection deadlines. 'timeout_ms' is how long poll may
   sleep when nothing is ready. */
int dh_http_poll (dh_host *h, int timeout_ms);
/* The same, but folding 'nextra' caller-owned descriptors into the single
   poll() and returning their revents. This is how the plugin channel joins
   the listener's wait instead of racing it for the sleep. */
int dh_http_poll_with (dh_host *h, int timeout_ms, struct pollfd *extra,
                       size_t nextra);

/* Milliseconds until the earliest live connection deadline, or -1 when the
   listener holds none. Lets dh_host_poll_timeout bound a quiet-socket sleep
   so a slow client's deadline fires near its moment. */
int dh_http_next_timeout (dh_host *h);

int dh_sql_open (dh_host *h, char *err, size_t errcap);
void dh_sql_close (dh_host *h);

int dh_crypto_open (dh_host *h, char *err, size_t errcap);
void dh_crypto_close (dh_host *h);

int dh_fs_open (dh_host *h, char *err, size_t errcap);
void dh_fs_close (dh_host *h);

int dh_exec_open (dh_host *h, char *err, size_t errcap);
void dh_exec_close (dh_host *h);

/* ----------------------------------------------------------------------
** The plugin channel (dhost_plugin.c).
**
** Spawns each configured plugin over a socketpair it inherits as fd 3, and
** registers one deferring connector per plugin. Every call it takes goes
** through the Part-1 ledger, so a plugin that is slow, wedged or dead
** costs latency and never the host thread.
** ---------------------------------------------------------------------- */

int  dh_plug_open (dh_host *h, char *err, size_t errcap);
void dh_plug_close (dh_host *h);

/* Contribute channel descriptors to the host's single poll(), and act on
   what came back. 'cap' bounds how many entries 'arm' may fill; it returns
   how many it did. The two calls must bracket exactly one poll(). */
size_t dh_plug_arm (dh_host *h, struct pollfd *fds, size_t cap);
void   dh_plug_fire (dh_host *h, struct pollfd *fds, size_t n);

/* Nonzero while any plugin still owes a reply, so a drained swarm with work
   outstanding does not exit underneath it. */
int dh_plug_busy (dh_host *h);

/* Sleep until something happens or 'timeout_ms' elapses, in exactly one
   place: the listener's sockets, the plugin channels and a plain timeout
   are all the same wait. Build 8's whole claim is that nothing blocks the
   shared thread, and that is only checkable if there is one place it
   stops. */
void dh_host_sleep (dh_host *h, int timeout_ms);

#endif
