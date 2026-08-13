/*
** host_check.c
** The generic host against its contract: doc/Host.md's duties and
** doc/Hostcall.md's encoding, driven end to end -- real config files, real
** guest programs making real hostcalls, and for the listener a real socket.
**
** The fixtures live in a mkdtemp directory so a failed run leaves evidence
** and a clean one leaves nothing. Every reply-shape assertion is made by
** the GUEST, in Lua, and reported through an exported 'log' queue -- the
** guest is the party the encoding exists for, so the guest is the party
** whose reading of it counts.
*/

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <sqlite3.h>

#include "dmsgpack.h"
#include "dhost.h"

static int checks = 0, failures = 0;

static void ok (int cond, const char *what) {
  checks++;
  if (cond)
    printf("[PASS] %s\n", what);
  else {
    printf("[FAIL] %s\n", what);
    failures++;
  }
}

static char tmpdir[128];

static const char *fixture (const char *name, const char *content) {
  static char path[512];
  FILE *f;
  snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
  f = fopen(path, "w");
  if (f == NULL) {
    fprintf(stderr, "cannot write fixture %s\n", path);
    exit(1);
  }
  fputs(content, f);
  fclose(f);
  return path;
}

static void tiny_sleep (void) {
  struct timespec ts = { 0, 5 * 1000 * 1000 };   /* 5ms */
  nanosleep(&ts, NULL);
}

/* Pop one string off the root's exported 'log' queue, NUL-terminated into
   'out'. 0 when there was nothing. */
static int pop_log (dh_host *h, char *out, size_t cap) {
  dv_instance *root = dvs_instance(h->sw, h->root);
  dv_queue_id q;
  uint8_t buf[512];
  size_t n = 0;
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  if (root == NULL)
    return 0;
  q = dv_queue_lookup(root, "log");
  if (q == 0)
    return 0;
  if (dv_queue_pop(root, q, buf, sizeof(buf), &n) != DV_OK)
    return 0;
  diluvium_mp_open(&c, buf, n);
  if (!diluvium_mp_read(&c, &t) || t.kind != DILUVIUM_MP_STR ||
      t.len + 1 > cap)
    return 0;
  memcpy(out, t.p, t.len);
  out[t.len] = '\0';
  return 1;
}

/* Turn the host until the log speaks or patience runs out. */
static int run_until_log (dh_host *h, char *out, size_t cap, int max_turns) {
  int i;
  for (i = 0; i < max_turns; i++) {
    dh_host_turn(h);
    if (pop_log(h, out, cap))
      return 1;
    tiny_sleep();
  }
  return 0;
}


/* ---------------------------------------------------------------- dh_buf */

static void emitters_round_trip_through_the_codec (void) {
  /* The host's writer against the runtime's reader, which is the pair that
     actually meets in production. */
  dh_buf b;
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  dh_buf_init(&b);
  dh_map(&b, 5);
  dh_str(&b, "i");    dh_int(&b, -42);
  dh_str(&b, "u");    dh_uint(&b, 1234567890123ull);
  dh_str(&b, "s");    dh_str(&b, "hello");
  dh_str(&b, "yes");  dh_bool(&b, 1);
  dh_str(&b, "none"); dh_nil(&b);
  diluvium_mp_open(&c, b.p, b.len);
  ok(diluvium_mp_field(&c, "i") && diluvium_mp_read(&c, &t) &&
     t.kind == DILUVIUM_MP_INT && t.i == -42,
     "a negative int round-trips through the runtime's cursor");
  diluvium_mp_open(&c, b.p, b.len);
  ok(diluvium_mp_field(&c, "u") && diluvium_mp_read(&c, &t) &&
     t.kind == DILUVIUM_MP_INT && t.i == 1234567890123ll,
     "a 64-bit uint round-trips");
  diluvium_mp_open(&c, b.p, b.len);
  ok(diluvium_mp_field(&c, "s") && diluvium_mp_read(&c, &t) &&
     t.kind == DILUVIUM_MP_STR && t.len == 5 && memcmp(t.p, "hello", 5) == 0,
     "a string round-trips");
  diluvium_mp_open(&c, b.p, b.len);
  ok(diluvium_mp_field(&c, "none") && diluvium_mp_read(&c, &t) &&
     t.kind == DILUVIUM_MP_NIL, "nil round-trips");
  dh_buf_free(&b);
}


/* ---------------------------------------------------------------- config */

static void a_config_is_typed_data_and_typos_are_refused (void) {
  dh_config cfg;
  char err[512];
  const char *p;

  p = fixture("good.host.lua",
    "return {\n"
    "  supervisor = 'sup.lua',\n"
    "  max_instances = 32,\n"
    "  identity = 'host-check',\n"
    "  hibernation = 'off',\n"
    "  caps = { 'lifecycle', 'host:time' },\n"
    "  budget = { instructions = 1000000 },\n"
    "  connectors = { time = true },\n"
    "}\n");
  ok(dh_config_load(p, &cfg, err, sizeof(err)) == 0,
     "a well-typed config loads");
  ok(cfg.max_instances == 32 && cfg.hibernation == 0 &&
     cfg.time_connector == 1 && cfg.ncaps == 2 &&
     strcmp(cfg.caps[1], "host:time") == 0 &&
     cfg.instructions == 1000000 &&
     strcmp(cfg.identity, "host-check") == 0,
     "and every field lands where the struct says");

  p = fixture("typo.host.lua",
    "return { supervisor = 's.lua', max_instance = 9 }\n");
  ok(dh_config_load(p, &cfg, err, sizeof(err)) != 0 &&
     strstr(err, "max_instance") != NULL,
     "an unknown key is refused BY NAME, not defaulted around");

  p = fixture("compute.host.lua",
    "return { supervisor = tostring(1) }\n");
  ok(dh_config_load(p, &cfg, err, sizeof(err)) != 0 &&
     strstr(err, "environment is empty") != NULL,
     "a config that computes is refused: the environment is empty on purpose");

  p = fixture("badhib.host.lua",
    "return { supervisor = 's.lua', hibernation = 'sideways' }\n");
  ok(dh_config_load(p, &cfg, err, sizeof(err)) != 0,
     "hibernation must be \"on\" or \"off\"");

  p = fixture("noscope.host.lua",
    "return { supervisor = 's.lua', connectors = { sql = { access = 'read' } } }\n");
  ok(dh_config_load(p, &cfg, err, sizeof(err)) != 0 &&
     strstr(err, "scope") != NULL,
     "a sql block without a granted scope is refused");

  /* The build6 keys teach their replacements rather than reading as typos. */
  p = fixture("oldsql.host.lua",
    "return { supervisor = 's.lua', connectors = {\n"
    "  sql = { path = 'example.db' } } }\n");
  ok(dh_config_load(p, &cfg, err, sizeof(err)) != 0 &&
     strstr(err, "scope") != NULL && strstr(err, "build7") != NULL,
     "the old sql.path is refused with directions to 'scope'");

  p = fixture("badcreate.host.lua",
    "return { supervisor = 's.lua', connectors = {\n"
    "  sql = { scope = '.', access = 'read', create = true } } }\n");
  ok(dh_config_load(p, &cfg, err, sizeof(err)) != 0 &&
     strstr(err, "readwrite") != NULL,
     "create under a read grant is a contradiction, refused");

  /* One listen block, the back-compatible shape, is one listener. */
  p = fixture("one_listen.host.lua",
    "return { supervisor = 's.lua', connectors = {\n"
    "  listen = { port = 8080, queue = 'in' } } }\n");
  ok(dh_config_load(p, &cfg, err, sizeof(err)) == 0 &&
     cfg.nlisteners == 1 && cfg.listeners[0].port == 8080 &&
     strcmp(cfg.listeners[0].queue, "in") == 0 &&
     cfg.listeners[0].max_conns == 64,
     "a single listen block is one listener, defaults filled");

  /* An array of blocks pre-binds a block of ports. */
  p = fixture("many_listen.host.lua",
    "return { supervisor = 's.lua', connectors = { listen = {\n"
    "  { port = 8080 }, { port = 8081 }, { port = 8082, max_conns = 8 },\n"
    "} } }\n");
  ok(dh_config_load(p, &cfg, err, sizeof(err)) == 0 &&
     cfg.nlisteners == 3 && cfg.listeners[0].port == 8080 &&
     cfg.listeners[2].port == 8082 && cfg.listeners[2].max_conns == 8 &&
     strcmp(cfg.listeners[1].queue, "http_in") == 0,
     "an array of listen blocks pre-binds a block of ports");

  /* Two listeners on one port name the mistake instead of failing at bind. */
  p = fixture("dupport.host.lua",
    "return { supervisor = 's.lua', connectors = { listen = {\n"
    "  { port = 8080 }, { port = 8080 } } } }\n");
  ok(dh_config_load(p, &cfg, err, sizeof(err)) != 0 &&
     strstr(err, "repeats a port") != NULL,
     "two listeners on one port are refused by name, not left to bind");

  /* A block with no port is refused whether alone or in the array. */
  p = fixture("noport.host.lua",
    "return { supervisor = 's.lua', connectors = { listen = {\n"
    "  { queue = 'in' } } } }\n");
  ok(dh_config_load(p, &cfg, err, sizeof(err)) != 0 &&
     strstr(err, "port is required") != NULL,
     "a listener without a port is refused");

  /* Past the host's port block, refused rather than silently clipped. */
  p = fixture("toomany.host.lua",
    "return { supervisor = 's.lua', connectors = { listen = {\n"
    "  {port=1},{port=2},{port=3},{port=4},{port=5},\n"
    "  {port=6},{port=7},{port=8},{port=9} } } }\n");
  ok(dh_config_load(p, &cfg, err, sizeof(err)) != 0 &&
     strstr(err, "more than") != NULL,
     "more listeners than the host can hold are refused, not clipped");
}


/* ------------------------------------------------------------- hostcalls */

static void a_guest_calls_time_and_reads_the_reply (void) {
  dh_config cfg;
  dh_host h;
  char err[512], log[256];
  fixture("sup_time.lua",
    "local calls = queue.declare('host/calls', {capacity = 4, exported = true})\n"
    "local replies = queue.declare('host/replies', {capacity = 4})\n"
    "local log = queue.declare('log', {capacity = 4, exported = true})\n"
    "local park = queue.declare('park', {capacity = 1})\n"
    "queue.push(calls, {tok = 7, call = 'time'})\n"
    "local _, m, why = queue.wait({replies}, 5000)\n"
    "if why == 'ok' and m.tok == 7 and m.status == 'ok'\n"
    "   and type(m.value) == 'number' and m.value > 0 then\n"
    "  queue.push(log, 'good')\n"
    "else\n"
    "  queue.push(log, 'bad: ' .. tostring(why) .. '/' ..\n"
    "             tostring(m and m.status) .. '/' ..\n"
    "             tostring(m and m.detail))\n"
    "end\n"
    "queue.wait({park})\n");
  {
    char cfgsrc[512];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_time.lua',\n"
             "  caps = { 'host:time' },\n"
             "  connectors = { time = true } }\n", tmpdir);
    fixture("time.host.lua", cfgsrc);
  }
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/time.host.lua", tmpdir);
    if (dh_config_load(path, &cfg, err, sizeof(err)) != 0 ||
        dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
      printf("      (%s)\n", err);
      ok(0, "the time deployment opens");
      return;
    }
  }
  ok(run_until_log(&h, log, sizeof(log), 50) && strcmp(log, "good") == 0,
     "a guest pushes {tok, call='time'} and reads back {tok, 'ok', a moment}");
  if (log[0] != '\0' && strcmp(log, "good") != 0)
    printf("      (guest said: %s)\n", log);
  dh_host_close(&h);
}

static void the_grant_gates_and_the_refusals_name_themselves (void) {
  dh_config cfg;
  dh_host h;
  char err[512], log[256];
  fixture("sup_gate.lua",
    "local calls = queue.declare('host/calls', {capacity = 8, exported = true})\n"
    "local replies = queue.declare('host/replies', {capacity = 8})\n"
    "local log = queue.declare('log', {capacity = 8, exported = true})\n"
    "local park = queue.declare('park', {capacity = 1})\n"
    "local verdict = {}\n"
    "-- a call outside the grant\n"
    "queue.push(calls, {tok = 1, call = 'time'})\n"
    "local _, m = queue.wait({replies}, 5000)\n"
    "verdict[1] = (m and m.status == 'denied'\n"
    "  and string.find(m.detail, 'host:time', 1, true) ~= nil)\n"
    "-- a call inside the grant that nothing answers\n"
    "queue.push(calls, {tok = 2, call = 'frob'})\n"
    "_, m = queue.wait({replies}, 5000)\n"
    "verdict[2] = (m and m.status == 'denied'\n"
    "  and string.find(m.detail, 'no connector', 1, true) ~= nil)\n"
    "-- a request with no call at all\n"
    "queue.push(calls, {tok = 3})\n"
    "_, m = queue.wait({replies}, 5000)\n"
    "verdict[3] = (m and m.status == 'malformed' and m.tok == 3)\n"
    "if verdict[1] and verdict[2] and verdict[3] then\n"
    "  queue.push(log, 'good')\n"
    "else\n"
    "  queue.push(log, 'bad: ' .. tostring(verdict[1]) .. '/'\n"
    "    .. tostring(verdict[2]) .. '/' .. tostring(verdict[3]))\n"
    "end\n"
    "queue.wait({park})\n");
  {
    char cfgsrc[512];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_gate.lua',\n"
             "  caps = { 'host:frob' },\n"        /* held, but never wired */
             "  connectors = { time = true } }\n", tmpdir);
    fixture("gate.host.lua", cfgsrc);
  }
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/gate.host.lua", tmpdir);
    if (dh_config_load(path, &cfg, err, sizeof(err)) != 0 ||
        dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
      printf("      (%s)\n", err);
      ok(0, "the gate deployment opens");
      return;
    }
  }
  ok(run_until_log(&h, log, sizeof(log), 100) && strcmp(log, "good") == 0,
     "denied outside the grant, denied when unwired, malformed when tokless "
     "-- each named, none dropped");
  if (log[0] != '\0' && strcmp(log, "good") != 0)
    printf("      (guest said: %s)\n", log);
  dh_host_close(&h);
}

static void a_guest_wait_timeout_is_fired_by_the_host_clock (void) {
  dh_config cfg;
  dh_host h;
  char err[512], log[256];
  fixture("sup_timeout.lua",
    "local q = queue.declare('never', {capacity = 1})\n"
    "local log = queue.declare('log', {capacity = 4, exported = true})\n"
    "local park = queue.declare('park', {capacity = 1})\n"
    "local id, m, why = queue.wait({q}, 60)\n"
    "queue.push(log, why)\n"
    "queue.wait({park})\n");
  {
    char cfgsrc[512];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_timeout.lua' }\n", tmpdir);
    fixture("timeout.host.lua", cfgsrc);
  }
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/timeout.host.lua", tmpdir);
    if (dh_config_load(path, &cfg, err, sizeof(err)) != 0 ||
        dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
      printf("      (%s)\n", err);
      ok(0, "the timeout deployment opens");
      return;
    }
  }
  /* 60ms of guest patience against 5ms turns: it must wake as 'timeout'
     without anything ever being pushed. */
  ok(run_until_log(&h, log, sizeof(log), 100) && strcmp(log, "timeout") == 0,
     "a wait with a timeout wakes as 'timeout': the guest expressed the "
     "bound, the host owned the clock");
  if (log[0] != '\0' && strcmp(log, "timeout") != 0)
    printf("      (guest said: %s)\n", log);
  dh_host_close(&h);
}


/* ------------------------------------------------------------------- sql */

static void sql_query_and_exec_split_along_the_grant (void) {
  dh_config cfg;
  dh_host h;
  char err[512], log[256];
  fixture("sup_sql.lua",
    "local calls = queue.declare('host/calls', {capacity = 8, exported = true})\n"
    "local replies = queue.declare('host/replies', {capacity = 8})\n"
    "local log = queue.declare('log', {capacity = 8, exported = true})\n"
    "local park = queue.declare('park', {capacity = 1})\n"
    "local function ask(req)\n"
    "  queue.push(calls, req)\n"
    "  local _, m = queue.wait({replies}, 5000)\n"
    "  return m\n"
    "end\n"
    "local v = {}\n"
    "local m = ask({tok = 1, call = 'sql/exec',\n"
    "  args = {db = 'check.db', sql = 'CREATE TABLE t (a INTEGER, b TEXT)'}})\n"
    "v[1] = (m.status == 'ok')\n"
    "m = ask({tok = 2, call = 'sql/exec',\n"
    "  args = {db = 'check.db', sql = 'INSERT INTO t VALUES (?, ?)',\n"
    "          params = {42, 'x'}}})\n"
    "v[2] = (m.status == 'ok' and m.value.changes == 1)\n"
    "m = ask({tok = 3, call = 'sql/query',\n"
    "  args = {db = 'check.db', sql = 'SELECT a, b FROM t'}})\n"
    "v[3] = (m.status == 'ok' and m.value.cols[1] == 'a'\n"
    "  and m.value.rows[1][1] == 42 and m.value.rows[1][2] == 'x')\n"
    "-- a write wearing query's grant\n"
    "m = ask({tok = 4, call = 'sql/query',\n"
    "  args = {db = 'check.db', sql = 'DELETE FROM t'}})\n"
    "v[4] = (m.status == 'denied'\n"
    "  and string.find(m.detail, 'sql/exec', 1, true) ~= nil)\n"
    "-- two statements wearing one authorization\n"
    "m = ask({tok = 5, call = 'sql/query',\n"
    "  args = {db = 'check.db', sql = 'SELECT 1; SELECT 2'}})\n"
    "v[5] = (m.status == 'error'\n"
    "  and string.find(m.detail, 'one statement', 1, true) ~= nil)\n"
    "-- a call that names no database is told whose job naming is\n"
    "m = ask({tok = 6, call = 'sql/query', args = {sql = 'SELECT 1'}})\n"
    "v[6] = (m.status == 'error'\n"
    "  and string.find(m.detail, 'name their database', 1, true) ~= nil)\n"
    "local all = v[1] and v[2] and v[3] and v[4] and v[5] and v[6]\n"
    "if all then queue.push(log, 'good')\n"
    "else queue.push(log, 'bad: ' .. tostring(v[1]) .. tostring(v[2])\n"
    "  .. tostring(v[3]) .. tostring(v[4]) .. tostring(v[5])\n"
    "  .. tostring(v[6])) end\n"
    "queue.wait({park})\n");
  {
    char cfgsrc[768];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_sql.lua',\n"
             "  caps = { 'host:sql/*' },\n"
             "  connectors = { sql = { scope = '%s',\n"
             "    access = 'readwrite', max_result_rows = 8 } } }\n",
             tmpdir, tmpdir);
    fixture("sql.host.lua", cfgsrc);
  }
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/sql.host.lua", tmpdir);
    if (dh_config_load(path, &cfg, err, sizeof(err)) != 0 ||
        dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
      printf("      (%s)\n", err);
      ok(0, "the sql deployment opens");
      return;
    }
  }
  ok(run_until_log(&h, log, sizeof(log), 200) && strcmp(log, "good") == 0,
     "create, insert with params, select back, a write refused under "
     "query's grant, a second statement refused whole, and a nameless "
     "call told to name its database");
  if (log[0] != '\0' && strcmp(log, "good") != 0)
    printf("      (guest said: %s)\n", log);
  dh_host_close(&h);
}

/*
** The same ground as the raw tests above, walked through the 'host' guest
** library -- which is the point: this guest is what an ordinary program
** looks like after build7. No queue pair, no tokens, no op-strings; a non-ok
** status raises with the connector's sentence, and try_* hands a denial back
** as a value where the program expects one. If this program stops reading
** like an ordinary program, the library's API is wrong, not the test.
*/
static void the_host_library_speaks_hostcall (void) {
  dh_config cfg;
  dh_host h;
  char err[512], log[512];
  fixture("sup_hostlib.lua",
    "local log = queue.declare('log', {capacity = 8, exported = true})\n"
    "local park = queue.declare('park', {capacity = 1})\n"
    "local okrun, why = pcall(function()\n"
    "  assert(host.time() > 0, 'the clock answers in ms')\n"
    "  local db = host.sql.open('hostlib.db')\n"
    "  db.exec('CREATE TABLE t (a INTEGER, b TEXT)')\n"
    "  local r = db.exec('INSERT INTO t VALUES (?, ?)', 42, 'x')\n"
    "  assert(r.changes == 1, 'insert reports its changes')\n"
    "  local q = db.query('SELECT a, b FROM t')\n"
    "  assert(q.cols[1] == 'a' and q.rows[1][1] == 42\n"
    "    and q.rows[1][2] == 'x', 'select round-trips')\n"
    "  -- a write wearing query's grant: try_query returns the denial\n"
    "  local v, st, detail = db.try_query('DELETE FROM t')\n"
    "  assert(v == nil and st == 'denied'\n"
    "    and string.find(detail, 'sql/exec', 1, true), 'try hands back denials')\n"
    "  -- a second database in the same scope is its own file\n"
    "  local other = host.sql.open('other.db')\n"
    "  local v2, st2 = other.try_query('SELECT a FROM t')\n"
    "  assert(v2 == nil and st2 == 'error', 'databases in a scope isolate')\n"
    "  -- a name that is a path is an escape, denied not clamped\n"
    "  local v3, st3, d3 = host.sql.open('../escape.db').try_query('SELECT 1')\n"
    "  assert(v3 == nil and st3 == 'denied'\n"
    "    and string.find(d3, 'scope', 1, true), 'an escaping name is denied')\n"
    "  -- a dangling symlink planted in the scope must not become a\n"
    "  -- creatable name whose creation lands at the link's target\n"
    "  local v4, st4 = host.sql.open('dangling.db').try_exec('CREATE TABLE d (a)')\n"
    "  assert(v4 == nil and st4 == 'denied',\n"
    "    'a dangling symlink is denied, not created through')\n"
    "  -- outside the grant entirely: the default raises, naming the call\n"
    "  local okc, e = pcall(host.crypto.hash, 'x')\n"
    "  assert(okc == false\n"
    "    and string.find(e, 'host.crypto.hash: denied:', 1, true),\n"
    "    'a raise names the call and carries the refusal')\n"
    "end)\n"
    "if okrun then queue.push(log, 'good')\n"
    "else queue.push(log, 'bad: ' .. tostring(why)) end\n"
    "queue.wait({park})\n");
  {
    char cfgsrc[768];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_hostlib.lua',\n"
             "  caps = { 'host:sql/*', 'host:time' },\n"
             "  connectors = { time = true, sql = { scope = '%s',\n"
             "    access = 'readwrite', max_result_rows = 8 } } }\n",
             tmpdir, tmpdir);
    fixture("hostlib.host.lua", cfgsrc);
  }
  {
    /* The dangling symlink the guest's last sql check trips over: its
       target is outside the scope and does not exist, so a stat-based
       exists check would have called it creatable. */
    char lp[512];
    snprintf(lp, sizeof(lp), "%s/dangling.db", tmpdir);
    (void)symlink("/nonexistent-target/held.db", lp);
  }
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/hostlib.host.lua", tmpdir);
    if (dh_config_load(path, &cfg, err, sizeof(err)) != 0 ||
        dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
      printf("      (%s)\n", err);
      ok(0, "the hostlib deployment opens");
      return;
    }
  }
  ok(run_until_log(&h, log, sizeof(log), 200) && strcmp(log, "good") == 0,
     "an ordinary program: host.time, host.sql round-trip, a denial handed "
     "back by try_query, a raise that names host.crypto.hash");
  if (log[0] != '\0' && strcmp(log, "good") != 0)
    printf("      (guest said: %s)\n", log);
  dh_host_close(&h);
}

/*
** The confinement escapes a review found, each now refused. A guest holding
** only host:sql/query, mode=read, must not be able to ATTACH another file,
** open a transaction, run a PRAGMA, or under-supply parameters. A secret
** file is planted so a successful ATTACH would be provable exfiltration
** rather than a guess.
*/
static void sql_confinement_holds (void) {
  dh_config cfg;
  dh_host h;
  char err[512], log[256], secretpath[512], dbpath[512];
  FILE *sf;
  sqlite3 *sdb;
  /* A second database with a secret, and the deployment's own db, both made
     with the library directly so the guest never had a hand in them. */
  snprintf(secretpath, sizeof(secretpath), "%s/secret.db", tmpdir);
  snprintf(dbpath, sizeof(dbpath), "%s/main.db", tmpdir);
  if (sqlite3_open(secretpath, &sdb) == SQLITE_OK) {
    sqlite3_exec(sdb, "CREATE TABLE s (v TEXT); INSERT INTO s VALUES "
                      "('TOP-SECRET')", NULL, NULL, NULL);
    sqlite3_close(sdb);
  }
  if (sqlite3_open(dbpath, &sdb) == SQLITE_OK) {
    sqlite3_exec(sdb, "CREATE TABLE t (a INTEGER)", NULL, NULL, NULL);
    sqlite3_close(sdb);
  }
  (void)sf;
  {
    char src[2200];
    snprintf(src, sizeof(src),
      "local calls = queue.declare('host/calls', {capacity=8, exported=true})\n"
      "local replies = queue.declare('host/replies', {capacity=8})\n"
      "local log = queue.declare('log', {capacity=8, exported=true})\n"
      "local park = queue.declare('park', {capacity=1})\n"
      "local function ask(req)\n"
      "  queue.push(calls, req); local _, m = queue.wait({replies}, 5000)\n"
      "  return m end\n"
      "local v = {}\n"
      "v[1] = (ask({tok=1, call='sql/query', args={db='main.db',\n"
      "  sql=\"ATTACH DATABASE '%s/secret.db' AS x\"}}).status ~= 'ok')\n"
      "v[2] = (ask({tok=2, call='sql/query',\n"
      "  args={db='main.db', sql='BEGIN'}}).status ~= 'ok')\n"
      "v[3] = (ask({tok=3, call='sql/query',\n"
      "  args={db='main.db', sql='PRAGMA foreign_keys=OFF'}}).status ~= 'ok')\n"
      "-- a select that under-supplies its parameters\n"
      "v[4] = (ask({tok=4, call='sql/query', args={db='main.db',\n"
      "  sql='SELECT a FROM t WHERE a = ?'}}).status ~= 'ok')\n"
      "-- the secret db exists IN scope, but a read grant does not create\n"
      "-- and does open: reading a sibling file is legitimate under the\n"
      "-- scope model, so confinement rests on scope choice -- prove the\n"
      "-- ESCAPING name is what fails\n"
      "v[5] = (ask({tok=5, call='sql/query', args={db='sub/secret.db',\n"
      "  sql='SELECT v FROM s'}}).status == 'denied')\n"
      "-- and a plain read still works, so the gate is not just off\n"
      "v[6] = (ask({tok=6, call='sql/query',\n"
      "  args={db='main.db', sql='SELECT count(*) FROM t'}}).status == 'ok')\n"
      "if v[1] and v[2] and v[3] and v[4] and v[5] and v[6]\n"
      "then queue.push(log,'good')\n"
      "else queue.push(log, 'bad: '..tostring(v[1])..tostring(v[2])\n"
      "  ..tostring(v[3])..tostring(v[4])..tostring(v[5])\n"
      "  ..tostring(v[6])) end\n"
      "queue.wait({park})\n", tmpdir);
    fixture("sup_conf.lua", src);
  }
  {
    char cfgsrc[768];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_conf.lua',\n"
             "  caps = { 'host:sql/query' },\n"
             "  connectors = { sql = { scope = '%s',\n"
             "    access = 'read' } } }\n", tmpdir, tmpdir);
    fixture("conf.host.lua", cfgsrc);
  }
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/conf.host.lua", tmpdir);
    if (dh_config_load(path, &cfg, err, sizeof(err)) != 0 ||
        dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
      printf("      (%s)\n", err);
      ok(0, "the confinement deployment opens");
      return;
    }
  }
  ok(run_until_log(&h, log, sizeof(log), 200) && strcmp(log, "good") == 0,
     "ATTACH, BEGIN, PRAGMA, an under-supplied param and an escaping name "
     "are each refused, and a plain read still works");
  if (log[0] != '\0' && strcmp(log, "good") != 0)
    printf("      (guest said: %s)\n", log);
  dh_host_close(&h);
}



/* ------------------------------------------------------------- fs, exec */

/*
** The fs connector under the same scope discipline as sql: reads and writes
** land inside the granted directory and nowhere else. A secret is planted
** outside the scope and a symlink to it inside, so a successful escape
** would be provable exfiltration; the byte cap and the no-structure rule
** each refuse with their own sentence. All through the host library, which
** is how a program will actually write it.
*/
static void fs_reads_and_writes_inside_its_scope (void) {
  dh_config cfg;
  dh_host h;
  char err[512], log[512];
  char scopedir[256], outside[512], linkpath[512];
  snprintf(scopedir, sizeof(scopedir), "%s/fs_scope", tmpdir);
  mkdir(scopedir, 0700);
  snprintf(outside, sizeof(outside), "%s/fs_secret.txt", tmpdir);
  {
    FILE *f = fopen(outside, "w");
    if (f != NULL) { fputs("TOP-SECRET", f); fclose(f); }
  }
  snprintf(linkpath, sizeof(linkpath), "%s/leak.txt", scopedir);
  (void)symlink(outside, linkpath);
  {
    /* A FIFO in the scope, reached through a symlink: opening it would
       block the single-threaded host forever, so it must be refused by
       target type before any open. */
    char fifopath[512], fifolink[512];
    snprintf(fifopath, sizeof(fifopath), "%s/pipe.fifo", scopedir);
    (void)mkfifo(fifopath, 0600);
    snprintf(fifolink, sizeof(fifolink), "%s/fifolink.txt", scopedir);
    (void)symlink(fifopath, fifolink);
  }
  fixture("sup_fs.lua",
    "local log = queue.declare('log', {capacity = 8, exported = true})\n"
    "local park = queue.declare('park', {capacity = 1})\n"
    "local okrun, why = pcall(function()\n"
    "  local w = host.fs.write('note.txt', 'hello, ')\n"
    "  assert(w.bytes == 7, 'a write reports its bytes')\n"
    "  host.fs.write('note.txt', 'scope', { append = true })\n"
    "  assert(host.fs.read('note.txt') == 'hello, scope', 'append appends')\n"
    "  local v, st = host.fs.try_read('../fs_secret.txt')\n"
    "  assert(v == nil and st == 'denied', 'dot-dot is denied')\n"
    "  v, st = host.fs.try_read('leak.txt')\n"
    "  assert(v == nil and st == 'denied',\n"
    "    'a symlink out of the scope is denied')\n"
    "  v, st, d = host.fs.try_read('fifolink.txt')\n"
    "  assert(v == nil and st == 'error'\n"
    "    and string.find(d, 'regular', 1, true),\n"
    "    'a symlink to a non-regular file is refused, not opened')\n"
    "  local d\n"
    "  v, st, d = host.fs.try_write('big.txt', string.rep('x', 65))\n"
    "  assert(v == nil and st == 'error'\n"
    "    and string.find(d, 'byte cap', 1, true), 'the write cap refuses')\n"
    "  v, st = host.fs.try_read('absent.txt')\n"
    "  assert(v == nil and st == 'error', 'a missing file is an error')\n"
    "  v, st, d = host.fs.try_write('nodir/x.txt', 'y')\n"
    "  assert(v == nil and st == 'error'\n"
    "    and string.find(d, 'creates', 1, true),\n"
    "    'structure is not created on the way')\n"
    "end)\n"
    "if okrun then queue.push(log, 'good')\n"
    "else queue.push(log, 'bad: ' .. tostring(why)) end\n"
    "queue.wait({park})\n");
  {
    char cfgsrc[768];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_fs.lua',\n"
             "  caps = { 'host:fs/*' },\n"
             "  connectors = { fs = { scope = '%s/fs_scope',\n"
             "    access = 'readwrite', max_bytes = 64 } } }\n",
             tmpdir, tmpdir);
    fixture("fs.host.lua", cfgsrc);
  }
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/fs.host.lua", tmpdir);
    if (dh_config_load(path, &cfg, err, sizeof(err)) != 0 ||
        dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
      printf("      (%s)\n", err);
      ok(0, "the fs deployment opens");
      return;
    }
  }
  ok(run_until_log(&h, log, sizeof(log), 200) && strcmp(log, "good") == 0,
     "fs: write, append, read back; dot-dot and a symlink escape denied; "
     "the byte cap and missing structure refuse");
  if (log[0] != '\0' && strcmp(log, "good") != 0)
    printf("      (guest said: %s)\n", log);
  dh_host_close(&h);
}

/*
** exec, the honest escape hatch, bounded: argv is a vector (a shell only by
** name), a nonzero exit is an answer, the deadline kills, the output cap
** refuses, and a per-call timeout cannot pass the host's ceiling.
*/
static void exec_is_bounded_and_shell_free (void) {
  dh_config cfg;
  dh_host h;
  char err[512], log[512];
  fixture("sup_exec.lua",
    "local log = queue.declare('log', {capacity = 8, exported = true})\n"
    "local park = queue.declare('park', {capacity = 1})\n"
    "local okrun, why = pcall(function()\n"
    "  local r = host.exec.run({ 'echo', 'hi' })\n"
    "  assert(r.status == 0 and r.stdout == 'hi\\n', 'echo echoes')\n"
    "  r = host.exec.run({ 'sh', '-c', 'echo out; echo err 1>&2; exit 3' })\n"
    "  assert(r.status == 3, 'a nonzero exit is an answer, not a raise')\n"
    "  assert(r.stdout == 'out\\n' and r.stderr == 'err\\n',\n"
    "    'the streams arrive separately')\n"
    "  r = host.exec.run({ 'cat' }, { stdin = 'fed' })\n"
    "  assert(r.stdout == 'fed', 'stdin reaches the child')\n"
    "  local v, st, d = host.exec.try_run({ 'sleep', '5' },\n"
    "                                     { timeout_ms = 200 })\n"
    "  assert(v == nil and st == 'error'\n"
    "    and string.find(d, 'deadline', 1, true), 'the deadline kills')\n"
    "  v, st, d = host.exec.try_run({ 'true' }, { timeout_ms = 99999 })\n"
    "  assert(v == nil and st == 'error'\n"
    "    and string.find(d, 'ceiling', 1, true),\n"
    "    'a call cannot ask past the ceiling')\n"
    "  v, st, d = host.exec.try_run({ 'sh', '-c',\n"
    "    'head -c 8192 /dev/zero' })\n"
    "  assert(v == nil and st == 'error'\n"
    "    and string.find(d, 'byte cap', 1, true), 'the output cap refuses')\n"
    "end)\n"
    "if okrun then queue.push(log, 'good')\n"
    "else queue.push(log, 'bad: ' .. tostring(why)) end\n"
    "queue.wait({park})\n");
  {
    char cfgsrc[512];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_exec.lua',\n"
             "  caps = { 'host:exec/run' },\n"
             "  connectors = { exec = { max_timeout_ms = 2000,\n"
             "    max_output_bytes = 4096 } } }\n", tmpdir);
    fixture("exec.host.lua", cfgsrc);
  }
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/exec.host.lua", tmpdir);
    if (dh_config_load(path, &cfg, err, sizeof(err)) != 0 ||
        dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
      printf("      (%s)\n", err);
      ok(0, "the exec deployment opens");
      return;
    }
  }
  ok(run_until_log(&h, log, sizeof(log), 600) && strcmp(log, "good") == 0,
     "exec: argv runs without a shell, exit and streams answer, the "
     "deadline kills, the ceiling and output cap refuse");
  if (log[0] != '\0' && strcmp(log, "good") != 0)
    printf("      (guest said: %s)\n", log);
  dh_host_close(&h);
}

/* --------------------------------------------------------------- crypto */

/*
** The crypto connector, cross-checked against an independent implementation.
** The reference tokens below were minted by Python's stdlib hmac/base64, so
** the C verifier reading them proves it speaks standard JWT-HS256 -- not
** merely that it agrees with itself. The guest also mints a token the test
** prints, which the surrounding script feeds back to Python: C -> Python
** interop, the other direction. Every verdict is the guest's, in Lua, because
** the guest is who these calls serve.
**
** The host does not sign with the configured secret directly: it derives one
** subkey for crypto/hmac and another for the JWT MAC (so crypto/hmac cannot
** forge a JWT). The reference values below are therefore keyed by those
** DERIVED subkeys, computed the same deterministic way -- the tokens are
** still bit-for-bit standard HS256, their secret is just HMAC(master, label).
**   k_hmac = HMAC-SHA256(KEY_STR, "diluvium/crypto/hmac/v1")
**   k_jwt  = HMAC-SHA256(KEY_STR, "diluvium/crypto/jwt-hs256/v1")
** JWT_WRONGKEY is signed with an unrelated key, so it must fail on signature.
*/
#define KEY_STR "test-signing-key-at-least-16-bytes-long"
#define SHA_ABC "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define HMAC_ABC "cafdbc2f03808a8615e9f159cdb630c498bd8b637d0ca1ecf14ad1d280d1c942"
#define JWT_VALID "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJib2IiLCJyb2xlIjoidXNlciIsImlhdCI6MTAwMDAwMDAwMCwiZXhwIjo0MTAyNDQ0ODAwfQ.eys7cRcxPhiyFeD-vCiCRK6hAO-C603ji9_zhpragYw"
#define JWT_EXPIRED "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJvbGQiLCJpYXQiOjEwMDAsImV4cCI6MTU3NzgzNjgwMH0.fbO8Q4-YQr6OAyVQr3_f3SCc6n4mSPoMExFVVdODwis"
#define JWT_WRONGKEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJldmlsIiwiZXhwIjo0MTAyNDQ0ODAwfQ.otYbev9GDLSM3sRSwbG4k4KD2D8yitSCp46ClmEHQa0"
/* Signed with the RAW configured secret, not the derived JWT subkey. A design
   that signed JWTs with the master directly -- or that let crypto/hmac and the
   JWT MAC share a key -- would accept this. The fixed host must reject it on
   signature: that is the regression guard for the forging-oracle fix. */
#define JWT_RAWKEY "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJtYWxsb3J5Iiwicm9sZSI6ImFkbWluIiwiZXhwIjo0MTAyNDQ0ODAwfQ.1HQKQlHpUfXhWSpqvc5WFhXOvE_0Go8W7PW2HmnfeLA"

static void crypto_signs_and_verifies_standard_jwts (void) {
  dh_config cfg;
  dh_host h;
  char err[512], line[512];
  {
    char src[3600];
    snprintf(src, sizeof(src),
      "local calls = queue.declare('host/calls', {capacity=16, exported=true})\n"
      "local replies = queue.declare('host/replies', {capacity=16})\n"
      "local log = queue.declare('log', {capacity=16, exported=true})\n"
      "local park = queue.declare('park', {capacity=1})\n"
      "local function ask(r) queue.push(calls, r)\n"
      "  local _, m = queue.wait({replies}, 5000); return m end\n"
      "local v = {}\n"
      "v[#v+1] = (ask({tok=1, call='crypto/hash', args={data='abc'}}).value\n"
      "  == '%s')\n"
      "v[#v+1] = (ask({tok=2, call='crypto/hmac', args={data='abc'}}).value\n"
      "  == '%s')\n"
      "local r1 = ask({tok=3, call='crypto/random', args={bytes=16}}).value\n"
      "local r2 = ask({tok=4, call='crypto/random', args={bytes=16}}).value\n"
      "v[#v+1] = (type(r1)=='string' and #r1==32 and r1 ~= r2)\n"
      "local tok = ask({tok=5, call='crypto/jwt_sign',\n"
      "  args={claims={sub='alice', role='admin'}}}).value\n"
      "local ver = ask({tok=6, call='crypto/jwt_verify',\n"
      "  args={token=tok}}).value\n"
      "v[#v+1] = (ver.valid==true and ver.claims.sub=='alice'\n"
      "  and ver.claims.role=='admin' and type(ver.claims.exp)=='number'\n"
      "  and ver.claims.iat ~= nil)\n"
      "local vt = ask({tok=7, call='crypto/jwt_verify',\n"
      "  args={token=tok..'x'}}).value\n"
      "v[#v+1] = (vt.valid==false and vt.reason=='signature')\n"
      "local vp = ask({tok=8, call='crypto/jwt_verify',\n"
      "  args={token='%s'}}).value\n"
      "v[#v+1] = (vp.valid==true and vp.claims.sub=='bob'\n"
      "  and vp.claims.role=='user')\n"
      "local ve = ask({tok=9, call='crypto/jwt_verify',\n"
      "  args={token='%s'}}).value\n"
      "v[#v+1] = (ve.valid==false and ve.reason=='expired')\n"
      "local vw = ask({tok=10, call='crypto/jwt_verify',\n"
      "  args={token='%s'}}).value\n"
      "v[#v+1] = (vw.valid==false and vw.reason=='signature')\n"
      "-- a token signed with the RAW secret must be rejected: the JWT MAC key\n"
      "-- is derived, so crypto/hmac cannot be used to forge one\n"
      "local vr = ask({tok=11, call='crypto/jwt_verify',\n"
      "  args={token='%s'}}).value\n"
      "v[#v+1] = (vr.valid==false and vr.reason=='signature')\n"
      "queue.push(log, 'TOKEN:' .. tok)\n"
      "local good = true\n"
      "for i=1,#v do if not v[i] then good=false end end\n"
      "if good then queue.push(log, 'good') else\n"
      "  local t={} for i=1,#v do t[i]=tostring(v[i]) end\n"
      "  queue.push(log, 'bad:'..table.concat(t,',')) end\n"
      "queue.wait({park})\n",
      SHA_ABC, HMAC_ABC, JWT_VALID, JWT_EXPIRED, JWT_WRONGKEY, JWT_RAWKEY);
    fixture("sup_crypto.lua", src);
  }
  {
    char cfgsrc[640];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_crypto.lua',\n"
             "  caps = { 'host:crypto/*' },\n"
             "  connectors = { crypto = { key = '%s', default_ttl = 3600 } }\n"
             "}\n", tmpdir, KEY_STR);
    fixture("crypto.host.lua", cfgsrc);
  }
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/crypto.host.lua", tmpdir);
    if (dh_config_load(path, &cfg, err, sizeof(err)) != 0 ||
        dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
      printf("      (%s)\n", err);
      ok(0, "the crypto deployment opens");
      return;
    }
  }
  /* First log line is the guest-minted token (printed for the script's
     Python cross-check), second is the verdict. */
  if (run_until_log(&h, line, sizeof(line), 200) &&
      strncmp(line, "TOKEN:", 6) == 0) {
    printf("MINTED %s\n", line + 6);      /* the script greps this line */
  }
  else {
    ok(0, "the crypto guest produced a token");
    dh_host_close(&h);
    return;
  }
  {
    int got = pop_log(&h, line, sizeof(line));
    ok(got && strcmp(line, "good") == 0,
       "hash and hmac match a reference impl, random is fresh hex, a signed "
       "token round-trips, tampering and a wrong key and an expired token "
       "are each refused by reason, and a Python-minted token verifies");
    if (got && strcmp(line, "good") != 0)
      printf("      (guest said: %s)\n", line);
  }
  dh_host_close(&h);
}


/* ------------------------------------------------------------------ http */

static void a_request_becomes_a_message_and_a_message_an_answer (void) {
  dh_config cfg;
  dh_host h;
  char err[512];
  int cfd;
  struct sockaddr_in addr;
  char resp[2048];
  size_t got = 0;
  int i, sent = 0;
  fixture("sup_http.lua",
    "local inq = queue.declare('http_in', {capacity = 8})\n"
    "local outq = queue.declare('http_out', {capacity = 8, exported = true})\n"
    "while true do\n"
    "  local _, m, why = queue.wait({inq})\n"
    "  if why == 'ok' then\n"
    "    queue.push(outq, {conn = m.conn, status = 200,\n"
    "      body = m.method .. ' ' .. m.path .. ' seen',\n"
    "      content_type = 'text/plain'})\n"
    "  end\n"
    "end\n");
  {
    char cfgsrc[512];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_http.lua',\n"
             "  connectors = { listen = { port = 18471,\n"
             "    deadline_ms = 3000 } } }\n", tmpdir);
    fixture("http.host.lua", cfgsrc);
  }
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/http.host.lua", tmpdir);
    if (dh_config_load(path, &cfg, err, sizeof(err)) != 0 ||
        dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
      printf("      (%s)\n", err);
      ok(0, "the http deployment opens (is port 18471 free?)");
      return;
    }
  }
  cfd = socket(AF_INET, SOCK_STREAM, 0);
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(18471);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    ok(0, "a client connects to the listener");
    dh_host_close(&h);
    return;
  }
  fcntl(cfd, F_SETFL, O_NONBLOCK);
  resp[0] = '\0';
  for (i = 0; i < 400 && got < sizeof(resp) - 1; i++) {
    ssize_t n;
    dh_host_turn(&h);
    dh_http_poll(&h, 5);
    if (!sent) {
      static const char req[] = "GET /hello HTTP/1.1\r\n"
                                "Host: check\r\n\r\n";
      if (write(cfd, req, sizeof(req) - 1) == (ssize_t)(sizeof(req) - 1))
        sent = 1;
    }
    n = read(cfd, resp + got, sizeof(resp) - 1 - got);
    if (n > 0) {
      got += (size_t)n;
      resp[got] = '\0';
    }
    else if (n == 0 && got > 0)
      break;                           /* Connection: close, delivered */
  }
  ok(strstr(resp, "HTTP/1.1 200 OK") != NULL,
     "a socket request comes back answered by a sealed Lua program");
  ok(strstr(resp, "GET /hello seen") != NULL,
     "and the body is the guest's, path and all");
  if (strstr(resp, "GET /hello seen") == NULL && got > 0)
    printf("      (response was: %.120s...)\n", resp);
  close(cfd);
  dh_host_close(&h);
}

/* Send one raw request to a running host on 'port', drive, collect the raw
   response. Returns bytes read. */
static size_t http_exchange (dh_host *h, int port, const char *req,
                             size_t reqlen, char *resp, size_t cap) {
  int cfd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  size_t got = 0;
  int i, sent = 0;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(cfd);
    return 0;
  }
  fcntl(cfd, F_SETFL, O_NONBLOCK);
  resp[0] = '\0';
  for (i = 0; i < 400 && got < cap - 1; i++) {
    ssize_t n;
    dh_host_turn(h);
    dh_http_poll(h, 5);
    if (!sent && write(cfd, req, reqlen) == (ssize_t)reqlen)
      sent = 1;
    n = read(cfd, resp + got, cap - 1 - got);
    if (n > 0) { got += (size_t)n; resp[got] = '\0'; }
    else if (n == 0 && got > 0) break;
  }
  close(cfd);
  return got;
}

/*
** The listener against a hostile guest and a hostile client, both refused.
** The guest is inside the sandbox, so its reply's content_type is untrusted;
** a CRLF in it must not inject a header. The client is outside the LB's TLS,
** so a smuggling-shaped request -- two Content-Lengths -- must be a refusal,
** not a guess about which the LB believed.
*/
static void the_listener_refuses_injection_and_smuggling (void) {
  dh_config cfg;
  dh_host h;
  char err[512], resp[2048];
  fixture("sup_evil.lua",
    "local inq = queue.declare('http_in', {capacity = 8})\n"
    "local outq = queue.declare('http_out', {capacity = 8, exported = true})\n"
    "while true do\n"
    "  local _, m, why = queue.wait({inq})\n"
    "  if why == 'ok' then\n"
    "    queue.push(outq, {conn = m.conn, status = 200, body = 'ok',\n"
    "      content_type = 'x/y\\r\\nEvil-Injected: 1'})\n"   /* the attack */
    "  end\n"
    "end\n");
  {
    char cfgsrc[512];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_evil.lua',\n"
             "  connectors = { listen = { port = 18473,\n"
             "    deadline_ms = 3000 } } }\n", tmpdir);
    fixture("evil.host.lua", cfgsrc);
  }
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/evil.host.lua", tmpdir);
    if (dh_config_load(path, &cfg, err, sizeof(err)) != 0 ||
        dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
      printf("      (%s)\n", err);
      ok(0, "the evil deployment opens (is port 18473 free?)");
      return;
    }
  }
  {
    static const char req[] = "GET /x HTTP/1.1\r\nHost: c\r\n\r\n";
    size_t got = http_exchange(&h, 18473, req, sizeof(req) - 1, resp,
                               sizeof(resp));
    ok(got > 0 && strstr(resp, "Evil-Injected") == NULL,
       "a control character in the guest's content_type injects no header");
    if (got > 0 && strstr(resp, "Evil-Injected") != NULL)
      printf("      (INJECTED: %.160s)\n", resp);
  }
  {
    static const char req[] = "POST /x HTTP/1.1\r\nHost: c\r\n"
                              "Content-Length: 5\r\nContent-Length: 6\r\n\r\n"
                              "hello";
    size_t got = http_exchange(&h, 18473, req, sizeof(req) - 1, resp,
                               sizeof(resp));
    ok(got > 0 && strstr(resp, "HTTP/1.1 400") != NULL,
       "two Content-Length headers are a 400, not a smuggling gamble");
    if (got > 0 && strstr(resp, "HTTP/1.1 400") == NULL)
      printf("      (response: %.120s)\n", resp);
  }
  dh_host_close(&h);
}


/*
** Two pre-bound ports feeding one shared queue. This is the shape a
** deployment reaches for when it cannot bind at runtime: bind the block up
** front, let one supervisor loop serve them all. The crux is that both
** connections' replies travel through a single reply queue, so each must
** reach the connection its 'conn' token names and no other -- a token that
** is unique across listeners, not merely within one.
*/
/*
** Part 3.3: an allowlisted subset of request headers reaches the guest,
** lowercased; repeats join per RFC 7230; everything else stays host-side.
** With an allowlist configured the 'headers' map is always present, so the
** message's shape is the config's decision, not the traffic's.
*/
static void the_listener_forwards_allowlisted_headers (void) {
  dh_config cfg;
  dh_host h;
  char err[512];
  fixture("sup_hdrs.lua",
    "local inq = queue.declare('http_in', {capacity = 8})\n"
    "local outq = queue.declare('http_out', {capacity = 8, exported = true})\n"
    "while true do\n"
    "  local _, m, why = queue.wait({inq})\n"
    "  if why == 'ok' then\n"
    "    queue.push(outq, {conn = m.conn, status = 200,\n"
    "      body = tostring(m.headers.authorization) .. '|' ..\n"
    "             tostring(m.headers['x-thing']) .. '|' ..\n"
    "             tostring(m.headers.cookie),\n"
    "      content_type = 'text/plain'})\n"
    "  end\n"
    "end\n");
  {
    char cfgsrc[512];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_hdrs.lua',\n"
             "  connectors = { listen = { port = 18478, deadline_ms = 3000,\n"
             "    headers = { 'authorization', 'x-thing' } } } }\n", tmpdir);
    fixture("hdrs.host.lua", cfgsrc);
  }
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/hdrs.host.lua", tmpdir);
    if (dh_config_load(path, &cfg, err, sizeof(err)) != 0 ||
        dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
      printf("      (%s)\n", err);
      ok(0, "the headers deployment opens (is port 18478 free?)");
      return;
    }
  }
  {
    static const char req[] =
      "GET /h HTTP/1.1\r\nHost: check\r\n"
      "Authorization: Bearer tok123\r\n"
      "X-Thing: a\r\nX-THING: b\r\n"
      "Cookie: secret=1\r\n\r\n";
    char resp[2048];
    size_t got = http_exchange(&h, 18478, req, sizeof(req) - 1, resp,
                               sizeof(resp));
    ok(got > 0 && strstr(resp, "Bearer tok123|a, b|nil") != NULL,
       "allowlisted headers arrive lowercased, repeats joined, and an "
       "unlisted one does not");
    if (got > 0 && strstr(resp, "Bearer tok123|a, b|nil") == NULL)
      printf("      (response was: %.160s)\n", resp);
  }
  {
    /* Without any Authorization sent, the map is present and empty -- the
       guest indexing it must not blow up on a shape that changed. */
    static const char req[] = "GET /h HTTP/1.1\r\nHost: check\r\n\r\n";
    char resp[2048];
    size_t got = http_exchange(&h, 18478, req, sizeof(req) - 1, resp,
                               sizeof(resp));
    ok(got > 0 && strstr(resp, "nil|nil|nil") != NULL,
       "with none sent the headers map is present and empty, same shape");
  }
  dh_host_close(&h);
}

static void two_ports_pre_bound_route_by_token (void) {
  dh_config cfg;
  dh_host h;
  char err[512];
  int a, b, i;
  struct sockaddr_in aa, ba;
  char ra[2048], rb[2048];
  size_t ga = 0, gb = 0;
  int sa = 0, sb = 0;
  fixture("sup_two.lua",
    "local inq = queue.declare('http_in', {capacity = 8})\n"
    "local outq = queue.declare('http_out', {capacity = 8, exported = true})\n"
    "while true do\n"
    "  local _, m, why = queue.wait({inq})\n"
    "  if why == 'ok' then\n"
    "    queue.push(outq, {conn = m.conn, status = 200,\n"
    "      body = 'saw ' .. m.path, content_type = 'text/plain'})\n"
    "  end\n"
    "end\n");
  {
    char cfgsrc[512];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_two.lua',\n"
             "  connectors = { listen = {\n"
             "    { port = 18475, deadline_ms = 3000 },\n"
             "    { port = 18476, deadline_ms = 3000 } } } }\n", tmpdir);
    fixture("two.host.lua", cfgsrc);
  }
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/two.host.lua", tmpdir);
    if (dh_config_load(path, &cfg, err, sizeof(err)) != 0 ||
        dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
      printf("      (%s)\n", err);
      ok(0, "the two-port deployment opens (are 18475-6 free?)");
      return;
    }
  }
  a = socket(AF_INET, SOCK_STREAM, 0);
  b = socket(AF_INET, SOCK_STREAM, 0);
  memset(&aa, 0, sizeof(aa)); aa.sin_family = AF_INET;
  aa.sin_port = htons(18475); inet_pton(AF_INET, "127.0.0.1", &aa.sin_addr);
  memset(&ba, 0, sizeof(ba)); ba.sin_family = AF_INET;
  ba.sin_port = htons(18476); inet_pton(AF_INET, "127.0.0.1", &ba.sin_addr);
  if (connect(a, (struct sockaddr *)&aa, sizeof(aa)) != 0 ||
      connect(b, (struct sockaddr *)&ba, sizeof(ba)) != 0) {
    ok(0, "clients connect to both pre-bound ports");
    close(a); close(b); dh_host_close(&h); return;
  }
  fcntl(a, F_SETFL, O_NONBLOCK);
  fcntl(b, F_SETFL, O_NONBLOCK);
  ra[0] = rb[0] = '\0';
  for (i = 0; i < 600 && (ga < sizeof(ra) - 1 || gb < sizeof(rb) - 1); i++) {
    ssize_t n;
    dh_host_turn(&h);
    dh_http_poll(&h, 5);
    if (!sa) {
      static const char req[] = "GET /alpha HTTP/1.1\r\nHost: c\r\n\r\n";
      if (write(a, req, sizeof(req) - 1) == (ssize_t)(sizeof(req) - 1)) sa = 1;
    }
    if (!sb) {
      static const char req[] = "GET /beta HTTP/1.1\r\nHost: c\r\n\r\n";
      if (write(b, req, sizeof(req) - 1) == (ssize_t)(sizeof(req) - 1)) sb = 1;
    }
    n = read(a, ra + ga, sizeof(ra) - 1 - ga);
    if (n > 0) { ga += (size_t)n; ra[ga] = '\0'; }
    n = read(b, rb + gb, sizeof(rb) - 1 - gb);
    if (n > 0) { gb += (size_t)n; rb[gb] = '\0'; }
    if (strstr(ra, "saw /alpha") && strstr(rb, "saw /beta"))
      break;
  }
  ok(strstr(ra, "HTTP/1.1 200 OK") != NULL && strstr(ra, "saw /alpha") != NULL,
     "a request on the first pre-bound port is answered with its own path");
  ok(strstr(rb, "HTTP/1.1 200 OK") != NULL && strstr(rb, "saw /beta") != NULL,
     "a request on the second pre-bound port is answered with its own path");
  ok(strstr(ra, "/beta") == NULL && strstr(rb, "/alpha") == NULL,
     "replies route by token, so two ports sharing one queue do not cross");
  if (strstr(ra, "saw /alpha") == NULL || strstr(rb, "saw /beta") == NULL)
    printf("      (A: %.80s | B: %.80s)\n", ra, rb);
  close(a); close(b);
  dh_host_close(&h);
}


int main (void) {
  snprintf(tmpdir, sizeof(tmpdir), "/tmp/host_check_XXXXXX");
  if (mkdtemp(tmpdir) == NULL) {
    fprintf(stderr, "mkdtemp failed\n");
    return 1;
  }
  printf("=== the generic host against doc/Host.md ===\n");
  emitters_round_trip_through_the_codec();
  a_config_is_typed_data_and_typos_are_refused();
  a_guest_calls_time_and_reads_the_reply();
  the_grant_gates_and_the_refusals_name_themselves();
  a_guest_wait_timeout_is_fired_by_the_host_clock();
  sql_query_and_exec_split_along_the_grant();
  the_host_library_speaks_hostcall();
  sql_confinement_holds();
  fs_reads_and_writes_inside_its_scope();
  exec_is_bounded_and_shell_free();
  crypto_signs_and_verifies_standard_jwts();
  a_request_becomes_a_message_and_a_message_an_answer();
  the_listener_refuses_injection_and_smuggling();
  two_ports_pre_bound_route_by_token();
  the_listener_forwards_allowlisted_headers();
  printf("\n%d checks, %d failed\n", checks, failures);
  return (failures == 0) ? 0 : 1;
}
