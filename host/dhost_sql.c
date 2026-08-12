/*
** dhost_sql.c
** The sql connector: the 'host:sql' capability family over the system SQLite.
**
** Two calls, split so the capability grammar can split with them:
**
**   sql/query  {sql = "...", params = {...}}  -> {cols = {...}, rows = {{...}}}
**   sql/exec   {sql = "...", params = {...}}  -> {changes = n, rowid = n}
**
** 'query' refuses a statement that writes -- 'sqlite3_stmt_readonly' answers
** after prepare, so the classification is SQLite's own rather than a regex --
** and 'exec' exists only when the config says mode = "readwrite". So a grant
** of 'host:sql/query' against a read-only deployment is exactly what it
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

#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

#include "dmsgpack.h"
#include "dhost.h"


typedef struct dh_sql {
  sqlite3 *db;
  long max_rows;
  int readwrite;
} dh_sql;


/*
** The confinement authorizer, and it is load-bearing rather than defensive.
** 'sqlite3_stmt_readonly' answers "does this modify the database", and a
** review found that is the wrong question for a sandbox: ATTACH, DETACH,
** BEGIN, COMMIT and SAVEPOINT all answer readonly=1, so the write-gate below
** let them through -- and ATTACH escapes the one configured file (an
** arbitrary-file read, or write on a readwrite deployment), while BEGIN
** leaves a transaction open across calls on the single shared handle, its
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

static dh_call_status conn_sql (void *ud, dvs_id id, const char *call,
                                const unsigned char *args, size_t argslen,
                                dh_buf *value, char *detail,
                                size_t detailcap) {
  dh_sql *s = (dh_sql *)ud;
  const char *sql = NULL;
  size_t sqllen = 0;
  char *zsql;
  const char *tail = NULL;
  sqlite3_stmt *st = NULL;
  int is_exec;
  int rc;
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
    snprintf(detail, detailcap, "this deployment's database is read-only "
                                "(config mode \"read\"), so 'sql/exec' is "
                                "not wired");
    return DH_CALL_DENIED;
  }
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
  zsql = (char *)sqlite3_malloc64(sqllen + 1);
  if (zsql == NULL) {
    snprintf(detail, detailcap, "no memory for the statement");
    return DH_CALL_ERROR;
  }
  memcpy(zsql, sql, sqllen);
  zsql[sqllen] = '\0';
  rc = sqlite3_prepare_v2(s->db, zsql, (int)sqllen + 1, &st, &tail);
  if (rc != SQLITE_OK || st == NULL) {
    snprintf(detail, detailcap, "the statement would not prepare: %s",
             sqlite3_errmsg(s->db));
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
               sqlite3_errmsg(s->db));
      sqlite3_finalize(st);
      return DH_CALL_ERROR;
    }
    sqlite3_finalize(st);
    dh_map(value, 2);
    dh_str(value, "changes");
    dh_int(value, (int64_t)sqlite3_changes64(s->db));
    dh_str(value, "rowid");
    dh_int(value, (int64_t)sqlite3_last_insert_rowid(s->db));
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
                 sqlite3_errmsg(s->db));
        dh_buf_free(&rows);
        sqlite3_finalize(st);
        return DH_CALL_ERROR;
      }
      if (++nrows > s->max_rows) {
        snprintf(detail, detailcap, "the result passed this deployment's "
                                    "row cap (%ld); refused rather than "
                                    "truncated -- page with LIMIT/OFFSET",
                 s->max_rows);
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
  dh_sql *s = (dh_sql *)sqlite3_malloc64(sizeof(dh_sql));
  int flags;
  if (s == NULL) {
    snprintf(err, errcap, "no memory for the sql connector");
    return -1;
  }
  memset(s, 0, sizeof(*s));
  s->max_rows = h->cfg.sql.max_rows;
  s->readwrite = h->cfg.sql.readwrite;
  flags = s->readwrite ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
                       : SQLITE_OPEN_READONLY;
  if (sqlite3_open_v2(h->cfg.sql.path, &s->db, flags, NULL) != SQLITE_OK) {
    snprintf(err, errcap, "could not open the database '%s': %s",
             h->cfg.sql.path,
             (s->db != NULL) ? sqlite3_errmsg(s->db) : "no memory");
    if (s->db != NULL) sqlite3_close(s->db);
    sqlite3_free(s);
    return -1;
  }
  sqlite3_busy_timeout(s->db, 2000);
  sqlite3_set_authorizer(s->db, sql_authorizer, s);
  /* Belt to the authorizer's braces: no ATTACH means no reaching another
     file even if a future SQLite classified one differently. */
  sqlite3_limit(s->db, SQLITE_LIMIT_ATTACHED, 0);
  if (dh_register(h, "sql", conn_sql, s) != 0) {
    snprintf(err, errcap, "could not register the sql connector");
    sqlite3_close(s->db);
    sqlite3_free(s);
    return -1;
  }
  h->sqlctx = s;
  return 0;
}

void dh_sql_close (dh_host *h) {
  dh_sql *s = (dh_sql *)h->sqlctx;
  if (s == NULL)
    return;
  sqlite3_close(s->db);
  sqlite3_free(s);
  h->sqlctx = NULL;
}
