/*
** dhost_sql.c
** The sql connector: the 'host:sql' capability family over the system SQLite.
**
** The config grants a *scope* -- a directory -- and the program names the
** database it wants within it, so which database is an application detail
** living in the application. Two calls, split so the capability grammar can
** split with them:
**
**   sql/query  {db = "name", sql = "...", params = {...}}
**              -> {cols = {...}, rows = {{...}}}
**   sql/exec   {db = "name", sql = "...", params = {...}}
**              -> {changes = n, rowid = n}
**
** 'db' is a filename inside the granted scope, never a path: a separator, a
** '.' or '..', or an embedded NUL is refused, and a name that resolves
** (through a symlink) to somewhere outside the scope is DENIED, not
** clamped. Handles open on first use and stay cached -- multiple databases
** fall out for free, up to a small bound -- and nothing is preallocated: a
** deployment that grants a scope pays for the databases its programs
** actually name. Whether a missing name may be *created* is the config's
** 'create' (an open-mode detail, defaulting to the write grant).
**
** 'query' refuses a statement that writes -- 'sqlite3_stmt_readonly' answers
** after prepare, so the classification is SQLite's own rather than a regex --
** and 'exec' exists only when the config grants access = "readwrite". So a
** grant of 'host:sql/query' against a read deployment is exactly what it
** says, and the wildcard family grant on a readwrite one is the bigger
** thing it says.
**
** One statement per call, autocommit only: state *between* hostcalls means a
** handle the host holds against the guest, which is real design (the endpoint
** token shape) deliberately not smuggled in through v1. The row cap refuses
** rather than truncates -- a truncated result is a silent lie, and the guest
** can page with LIMIT/OFFSET like everything else that reads a database.
**
** Replay note, which doc/Host.md now carries: replies are in the message log,
** so a replay REPLAYS them; it does not re-execute against the database. The
** database is outside the replay boundary.
*/

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <sqlite3.h>

#include "dmsgpack.h"
#include "dhost.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Databases a deployment may have open at once. A bound, not a budget: the
   cache never evicts, because closing a handle out from under the autocommit
   discipline would be invisible state a guest cannot reason about. */
#define DH_SQL_MAX_DBS 8

typedef struct dh_sql_db {
  char name[DH_NAME_MAX];
  sqlite3 *db;
} dh_sql_db;

typedef struct dh_sql {
  char scope[PATH_MAX];                 /* realpath'd granted directory */
  size_t scopelen;
  long max_result_rows;
  int readwrite;
  int create;
  dh_sql_db dbs[DH_SQL_MAX_DBS];
  int ndbs;
} dh_sql;


/*
** The confinement authorizer, and it is load-bearing rather than defensive.
** 'sqlite3_stmt_readonly' answers "does this modify the database", and a
** review found that is the wrong question for a sandbox: ATTACH, DETACH,
** BEGIN, COMMIT and SAVEPOINT all answer readonly=1, so the write-gate below
** let them through -- and ATTACH escapes the scope's files (an
** arbitrary-file read, or write on a readwrite deployment), while BEGIN
** leaves a transaction open across calls on a shared handle, its
** locks and its state visible to every other guest. The write-gate answers
** "does it change data"; this answers "does it touch connection state or
** reach outside the file", which is the question confinement actually turns
** on. Everything a query or a data statement legitimately does -- SELECT,
** INSERT, UPDATE, DELETE, CREATE, the functions and reads under them --
** returns SQLITE_OK; the escape hatches return SQLITE_DENY and fail the
** statement at prepare. PRAGMA is denied wholesale: a guest has no read-only
** PRAGMA it needs, and several (writable_schema, foreign_keys) are
** connection-state changes that would leak the same way.
*/
static int sql_authorizer (void *ud, int action, const char *a, const char *b,
                           const char *c, const char *d) {
  (void)ud; (void)a; (void)b; (void)c; (void)d;
  switch (action) {
    case SQLITE_ATTACH:
    case SQLITE_DETACH:
    case SQLITE_TRANSACTION:
    case SQLITE_SAVEPOINT:
    case SQLITE_PRAGMA:
      return SQLITE_DENY;
    default:
      return SQLITE_OK;
  }
}


static int arg_str (const unsigned char *args, size_t argslen,
                    const char *key, const char **p, size_t *len) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  if (args == NULL)
    return 0;
  diluvium_mp_open(&c, args, argslen);
  if (!diluvium_mp_field(&c, key) || !diluvium_mp_read(&c, &t) ||
      t.kind != DILUVIUM_MP_STR)
    return 0;
  *p = t.p;
  *len = t.len;
  return 1;
}

/* Harden one just-opened handle. The discipline is per-connection state, so
   every cached handle carries all of it. */
static void harden (sqlite3 *db, dh_sql *s) {
  sqlite3_busy_timeout(db, 2000);
  /* SQL-level load_extension is off by default, but say so out loud: this
     turns off both the SQL function and the C entry point, so the dlopen
     path a stock libsqlite3.a links in is unreachable as well as
     unauthorized. (The link-time 'dlopen in a static binary' warning is
     about that path existing, not being taken; a static musl build does not
     even warn, and to remove the symbol entirely one compiles SQLite with
     -DSQLITE_OMIT_LOAD_EXTENSION.) */
  sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, NULL);
  sqlite3_set_authorizer(db, sql_authorizer, s);
  /* Belt to the authorizer's braces: no ATTACH means no reaching another
     file even if a future SQLite classified one differently. */
  sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, 0);
}

/*
** args.db -> an open handle, or NULL with 'detail' filled and *status set.
** The name is a filename within the scope: flat by construction (no
** separators, no '.'/'..'), which kills traversal before resolution ever
** runs; the realpath check afterwards is for the file itself being a
** symlink out of the scope -- escape is DENIED, not clamped.
*/
static sqlite3 *db_for (dh_sql *s, const unsigned char *args, size_t argslen,
                        char *detail, size_t detailcap,
                        dh_call_status *status) {
  const char *name = NULL;
  size_t namelen = 0;
  char path[PATH_MAX];
  char resolved[PATH_MAX];
  struct stat sb;
  int exists;
  int flags;
  sqlite3 *db = NULL;
  int i;

  if (!arg_str(args, argslen, "db", &name, &namelen) || namelen == 0) {
    snprintf(detail, detailcap, "a scoped sql deployment answers calls that "
                                "name their database: args.db = \"name\" "
                                "(host.sql.open picks it)");
    *status = DH_CALL_ERROR;
    return NULL;
  }
  if (namelen >= DH_NAME_MAX) {
    snprintf(detail, detailcap, "the database name is longer than a name");
    *status = DH_CALL_ERROR;
    return NULL;
  }
  if (memchr(name, '\0', namelen) != NULL ||
      memchr(name, '/', namelen) != NULL ||
      memchr(name, '\\', namelen) != NULL ||
      (name[0] == '.' && (namelen == 1 ||
                          (namelen == 2 && name[1] == '.')))) {
    snprintf(detail, detailcap, "the database name '%.*s' steps outside the "
                                "granted scope; a name is a filename within "
                                "it, not a path", (int)namelen, name);
    *status = DH_CALL_DENIED;
    return NULL;
  }
  for (i = 0; i < s->ndbs; i++) {
    if (strlen(s->dbs[i].name) == namelen &&
        memcmp(s->dbs[i].name, name, namelen) == 0)
      return s->dbs[i].db;
  }
  if (s->ndbs >= DH_SQL_MAX_DBS) {
    snprintf(detail, detailcap, "this deployment already has %d databases "
                                "open, which is the host's bound", s->ndbs);
    *status = DH_CALL_ERROR;
    return NULL;
  }
  if (s->scopelen + 1 + namelen + 1 > sizeof(path)) {
    snprintf(detail, detailcap, "the database name does not fit under the "
                                "scope");
    *status = DH_CALL_ERROR;
    return NULL;
  }
  memcpy(path, s->scope, s->scopelen);
  path[s->scopelen] = '/';
  memcpy(path + s->scopelen + 1, name, namelen);
  path[s->scopelen + 1 + namelen] = '\0';
  /* lstat, not stat: a DANGLING symlink fails stat, would look like a
     creatable name, and sqlite's open would follow it and create the file
     at the link's arbitrary target. lstat sees the link itself, and a link
     whose target does not resolve back under the scope is an escape. */
  exists = (lstat(path, &sb) == 0);
  if (exists) {
    /* The name is there: resolve it and require the scope as a prefix, so a
       symlink placed inside the scope cannot read a file outside it. */
    if (realpath(path, resolved) == NULL ||
        strncmp(resolved, s->scope, s->scopelen) != 0 ||
        resolved[s->scopelen] != '/') {
      snprintf(detail, detailcap, "the database name '%.*s' resolves outside "
                                  "the granted scope", (int)namelen, name);
      *status = DH_CALL_DENIED;
      return NULL;
    }
    if (stat(resolved, &sb) != 0 || !S_ISREG(sb.st_mode)) {
      snprintf(detail, detailcap, "'%.*s' is not a regular file",
               (int)namelen, name);
      *status = DH_CALL_ERROR;
      return NULL;
    }
  }
  else if (!s->create) {
    snprintf(detail, detailcap, "no database named '%.*s' in this "
                                "deployment's scope, and creating one is not "
                                "granted (config.connectors.sql.create)",
             (int)namelen, name);
    *status = DH_CALL_ERROR;
    return NULL;
  }
  flags = s->readwrite ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
                       : SQLITE_OPEN_READONLY;
  if (sqlite3_open_v2(path, &db, flags, NULL) != SQLITE_OK) {
    snprintf(detail, detailcap, "could not open '%.*s': %s",
             (int)namelen, name,
             (db != NULL) ? sqlite3_errmsg(db) : "no memory");
    if (db != NULL) sqlite3_close(db);
    *status = DH_CALL_ERROR;
    return NULL;
  }
  harden(db, s);
  memcpy(s->dbs[s->ndbs].name, name, namelen);
  s->dbs[s->ndbs].name[namelen] = '\0';
  s->dbs[s->ndbs].db = db;
  s->ndbs++;
  return db;
}

static void emit_row_value (dh_buf *value, sqlite3_stmt *st, int col) {
  switch (sqlite3_column_type(st, col)) {
    case SQLITE_INTEGER:
      dh_int(value, (int64_t)sqlite3_column_int64(st, col));
      break;
    case SQLITE_FLOAT:
      dh_double(value, sqlite3_column_double(st, col));
      break;
    case SQLITE_NULL:
      dh_nil(value);
      break;
    default: {
      /* TEXT and BLOB both: a Lua string is bytes, and the guest codec
         reads str and bin alike, so str is the honest common carrier. */
      const void *p = sqlite3_column_blob(st, col);
      int n = sqlite3_column_bytes(st, col);
      dh_lstr(value, (p != NULL) ? (const char *)p : "", (size_t)n);
      break;
    }
  }
}

/* Bind the 'params' array. Returns 0 or fills 'detail'. The count must match
   the statement's placeholders exactly: too few would silently NULL-bind the
   rest, which is the truncation the row cap refuses to do elsewhere wearing a
   quieter disguise. */
static int bind_params (sqlite3_stmt *st, const unsigned char *args,
                        size_t argslen, char *detail, size_t detailcap) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  size_t i, n = 0;
  int want = sqlite3_bind_parameter_count(st);
  int have_field = 0;
  if (args != NULL) {
    diluvium_mp_open(&c, args, argslen);
    if (diluvium_mp_field(&c, "params")) {
      have_field = 1;
      if (!diluvium_mp_read(&c, &t) || t.kind != DILUVIUM_MP_ARRAY) {
        snprintf(detail, detailcap, "'params' must be an array");
        return -1;
      }
      n = t.len;
    }
  }
  if ((size_t)want != n) {
    snprintf(detail, detailcap, "the statement has %d parameter(s) but %d "
             "were supplied; bind them all or none", want, (int)n);
    return -1;
  }
  if (!have_field)
    return 0;
  for (i = 0; i < n; i++) {
    int rc;
    if (!diluvium_mp_read(&c, &t)) {
      snprintf(detail, detailcap, "'params' ended before its own length");
      return -1;
    }
    switch (t.kind) {
      case DILUVIUM_MP_NIL:
        rc = sqlite3_bind_null(st, (int)i + 1);
        break;
      case DILUVIUM_MP_BOOL:
        rc = sqlite3_bind_int(st, (int)i + 1, t.b ? 1 : 0);
        break;
      case DILUVIUM_MP_INT:
        rc = sqlite3_bind_int64(st, (int)i + 1, (sqlite3_int64)t.i);
        break;
      case DILUVIUM_MP_FLOAT:
        rc = sqlite3_bind_double(st, (int)i + 1, t.f);
        break;
      case DILUVIUM_MP_STR:
        rc = sqlite3_bind_text(st, (int)i + 1, t.p, (int)t.len,
                               SQLITE_TRANSIENT);
        break;
      default:
        snprintf(detail, detailcap, "param %d is not a scalar; a parameter "
                                    "is nil, boolean, number or string",
                 (int)i + 1);
        return -1;
    }
    if (rc != SQLITE_OK) {
      snprintf(detail, detailcap, "param %d would not bind", (int)i + 1);
      return -1;
    }
  }
  return 0;
}

static dh_call_status conn_sql (void *ud, dvs_id id, const char *call,
                                const unsigned char *args, size_t argslen,
                                dh_buf *value, char *detail,
                                size_t detailcap) {
  dh_sql *s = (dh_sql *)ud;
  sqlite3 *db;
  const char *sql = NULL;
  size_t sqllen = 0;
  char *zsql;
  const char *tail = NULL;
  sqlite3_stmt *st = NULL;
  int is_exec;
  int rc;
  dh_call_status dbstatus = DH_CALL_ERROR;
  (void)id;

  if (strcmp(call, "sql/query") == 0)
    is_exec = 0;
  else if (strcmp(call, "sql/exec") == 0)
    is_exec = 1;
  else {
    snprintf(detail, detailcap, "the sql connector answers 'sql/query' and "
                                "'sql/exec'; '%s' is neither", call);
    return DH_CALL_DENIED;
  }
  if (is_exec && !s->readwrite) {
    snprintf(detail, detailcap, "this deployment grants read access "
                                "(config.connectors.sql.access \"read\"), so "
                                "'sql/exec' is not wired");
    return DH_CALL_DENIED;
  }
  db = db_for(s, args, argslen, detail, detailcap, &dbstatus);
  if (db == NULL)
    return dbstatus;
  if (!arg_str(args, argslen, "sql", &sql, &sqllen)) {
    snprintf(detail, detailcap, "args.sql must be the statement, as a string");
    return DH_CALL_ERROR;
  }
  /* An embedded NUL would end the statement early at prepare while the tail
     check below, walking a C string, could not see past it -- so the bytes
     after it would be neither run nor refused, a blind spot rather than a
     bug today but the wrong shape. Refuse it. And SQLite's lengths are int,
     so an implausible statement is refused before the cast can flip sign. */
  if (sqllen > (size_t)2000000000u) {
    snprintf(detail, detailcap, "the statement is implausibly long");
    return DH_CALL_ERROR;
  }
  if (memchr(sql, '\0', sqllen) != NULL) {
    snprintf(detail, detailcap, "the statement has an embedded NUL byte");
    return DH_CALL_ERROR;
  }
  /* NUL-terminate a copy: the cursor's bytes are a slice of the request. */
  zsql = (char *)sqlite3_malloc((int)(sqllen + 1));
  if (zsql == NULL) {
    snprintf(detail, detailcap, "no memory for the statement");
    return DH_CALL_ERROR;
  }
  memcpy(zsql, sql, sqllen);
  zsql[sqllen] = '\0';
  rc = sqlite3_prepare_v2(db, zsql, (int)sqllen + 1, &st, &tail);
  if (rc != SQLITE_OK || st == NULL) {
    snprintf(detail, detailcap, "the statement would not prepare: %s",
             sqlite3_errmsg(db));
    sqlite3_free(zsql);
    if (st != NULL) sqlite3_finalize(st);
    return DH_CALL_ERROR;
  }
  /* One statement per call. Anything after the first beyond whitespace is a
     second statement wearing the first one's authorization. */
  while (tail != NULL && (*tail == ' ' || *tail == '\t' || *tail == '\n' ||
                          *tail == '\r' || *tail == ';'))
    tail++;
  if (tail != NULL && *tail != '\0') {
    snprintf(detail, detailcap, "one statement per call; the text past the "
                                "first statement was not run and the whole "
                                "call is refused");
    sqlite3_finalize(st);
    sqlite3_free(zsql);
    return DH_CALL_ERROR;
  }
  sqlite3_free(zsql);
  if (!is_exec && !sqlite3_stmt_readonly(st)) {
    snprintf(detail, detailcap, "'sql/query' is for statements that read; "
                                "this one writes, which is 'sql/exec' and a "
                                "different grant");
    sqlite3_finalize(st);
    return DH_CALL_DENIED;
  }
  if (bind_params(st, args, argslen, detail, detailcap) != 0) {
    sqlite3_finalize(st);
    return DH_CALL_ERROR;
  }

  if (is_exec) {
    rc = sqlite3_step(st);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
      snprintf(detail, detailcap, "the statement failed: %s",
               sqlite3_errmsg(db));
      sqlite3_finalize(st);
      return DH_CALL_ERROR;
    }
    sqlite3_finalize(st);
    dh_map(value, 2);
    dh_str(value, "changes");
    dh_int(value, (int64_t)sqlite3_changes(db));
    dh_str(value, "rowid");
    dh_int(value, (int64_t)sqlite3_last_insert_rowid(db));
    return DH_CALL_OK;
  }

  {
    /* Two passes would need re-execution; instead rows are collected into a
       side buffer while counting, and the arrays are framed afterwards --
       msgpack needs counts up front and a query does not offer one. */
    dh_buf rows;
    long nrows = 0;
    int ncols = sqlite3_column_count(st);
    int i;
    dh_buf_init(&rows);
    for (;;) {
      rc = sqlite3_step(st);
      if (rc == SQLITE_DONE)
        break;
      if (rc != SQLITE_ROW) {
        snprintf(detail, detailcap, "the query failed mid-walk: %s",
                 sqlite3_errmsg(db));
        dh_buf_free(&rows);
        sqlite3_finalize(st);
        return DH_CALL_ERROR;
      }
      if (++nrows > s->max_result_rows) {
        snprintf(detail, detailcap, "the result passed this deployment's "
                                    "row cap (%ld); refused rather than "
                                    "truncated -- page with LIMIT/OFFSET",
                 s->max_result_rows);
        dh_buf_free(&rows);
        sqlite3_finalize(st);
        return DH_CALL_ERROR;
      }
      dh_array(&rows, (unsigned)ncols);
      for (i = 0; i < ncols; i++)
        emit_row_value(&rows, st, i);
    }
    dh_map(value, 2);
    dh_str(value, "cols");
    dh_array(value, (unsigned)ncols);
    for (i = 0; i < ncols; i++)
      dh_str(value, sqlite3_column_name(st, i));
    dh_str(value, "rows");
    dh_array(value, (unsigned)nrows);
    if (rows.len > 0)
      dh_raw(value, rows.p, rows.len);
    dh_buf_free(&rows);
    sqlite3_finalize(st);
    return DH_CALL_OK;
  }
}

int dh_sql_open (dh_host *h, char *err, size_t errcap) {
  dh_sql *s;
  struct stat sb;
  char resolved[PATH_MAX];
  /* The scope must exist and be a directory, resolved once here: a granted
     place is a real place, and every later name check compares against this
     canonical form. Nothing else is opened or created -- a scope grant
     preallocates nothing. */
  if (realpath(h->cfg.sql.scope, resolved) == NULL ||
      stat(resolved, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
    snprintf(err, errcap, "config.connectors.sql.scope '%s' does not resolve "
                          "to a directory", h->cfg.sql.scope);
    return -1;
  }
  s = (dh_sql *)sqlite3_malloc((int)sizeof(dh_sql));
  if (s == NULL) {
    snprintf(err, errcap, "no memory for the sql connector");
    return -1;
  }
  memset(s, 0, sizeof(*s));
  strncpy(s->scope, resolved, sizeof(s->scope) - 1);
  s->scopelen = strlen(s->scope);
  s->max_result_rows = h->cfg.sql.max_result_rows;
  s->readwrite = h->cfg.sql.readwrite;
  s->create = h->cfg.sql.create;
  if (dh_register(h, "sql", conn_sql, s) != 0) {
    snprintf(err, errcap, "could not register the sql connector");
    sqlite3_free(s);
    return -1;
  }
  h->sqlctx = s;
  return 0;
}

void dh_sql_close (dh_host *h) {
  dh_sql *s = (dh_sql *)h->sqlctx;
  int i;
  if (s == NULL)
    return;
  for (i = 0; i < s->ndbs; i++)
    sqlite3_close(s->dbs[i].db);
  sqlite3_free(s);
  h->sqlctx = NULL;
}
