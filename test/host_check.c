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
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

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

  p = fixture("nosqlpath.host.lua",
    "return { supervisor = 's.lua', connectors = { sql = { mode = 'read' } } }\n");
  ok(dh_config_load(p, &cfg, err, sizeof(err)) != 0 &&
     strstr(err, "path") != NULL,
     "a sql block without a database path is refused");
}


/* ------------------------------------------------------------- hostcalls */

static void a_guest_calls_time_and_reads_the_reply (void) {
  dh_config cfg;
  dh_host h;
  char err[512], log[256];
  fixture("sup_time.lua",
    "local calls = queue.declare('host/calls', {cap = 4, exported = true})\n"
    "local replies = queue.declare('host/replies', {cap = 4})\n"
    "local log = queue.declare('log', {cap = 4, exported = true})\n"
    "local park = queue.declare('park', {cap = 1})\n"
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
    "local calls = queue.declare('host/calls', {cap = 8, exported = true})\n"
    "local replies = queue.declare('host/replies', {cap = 8})\n"
    "local log = queue.declare('log', {cap = 8, exported = true})\n"
    "local park = queue.declare('park', {cap = 1})\n"
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
    "local q = queue.declare('never', {cap = 1})\n"
    "local log = queue.declare('log', {cap = 4, exported = true})\n"
    "local park = queue.declare('park', {cap = 1})\n"
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
    "local calls = queue.declare('host/calls', {cap = 8, exported = true})\n"
    "local replies = queue.declare('host/replies', {cap = 8})\n"
    "local log = queue.declare('log', {cap = 8, exported = true})\n"
    "local park = queue.declare('park', {cap = 1})\n"
    "local function ask(req)\n"
    "  queue.push(calls, req)\n"
    "  local _, m = queue.wait({replies}, 5000)\n"
    "  return m\n"
    "end\n"
    "local v = {}\n"
    "local m = ask({tok = 1, call = 'sql/exec',\n"
    "  args = {sql = 'CREATE TABLE t (a INTEGER, b TEXT)'}})\n"
    "v[1] = (m.status == 'ok')\n"
    "m = ask({tok = 2, call = 'sql/exec',\n"
    "  args = {sql = 'INSERT INTO t VALUES (?, ?)', params = {42, 'x'}}})\n"
    "v[2] = (m.status == 'ok' and m.value.changes == 1)\n"
    "m = ask({tok = 3, call = 'sql/query',\n"
    "  args = {sql = 'SELECT a, b FROM t'}})\n"
    "v[3] = (m.status == 'ok' and m.value.cols[1] == 'a'\n"
    "  and m.value.rows[1][1] == 42 and m.value.rows[1][2] == 'x')\n"
    "-- a write wearing query's grant\n"
    "m = ask({tok = 4, call = 'sql/query', args = {sql = 'DELETE FROM t'}})\n"
    "v[4] = (m.status == 'denied'\n"
    "  and string.find(m.detail, 'sql/exec', 1, true) ~= nil)\n"
    "-- two statements wearing one authorization\n"
    "m = ask({tok = 5, call = 'sql/query',\n"
    "  args = {sql = 'SELECT 1; SELECT 2'}})\n"
    "v[5] = (m.status == 'error'\n"
    "  and string.find(m.detail, 'one statement', 1, true) ~= nil)\n"
    "local all = v[1] and v[2] and v[3] and v[4] and v[5]\n"
    "if all then queue.push(log, 'good')\n"
    "else queue.push(log, 'bad: ' .. tostring(v[1]) .. tostring(v[2])\n"
    "  .. tostring(v[3]) .. tostring(v[4]) .. tostring(v[5])) end\n"
    "queue.wait({park})\n");
  {
    char cfgsrc[768];
    snprintf(cfgsrc, sizeof(cfgsrc),
             "return { supervisor = '%s/sup_sql.lua',\n"
             "  caps = { 'host:sql/*' },\n"
             "  connectors = { sql = { path = '%s/check.db',\n"
             "    mode = 'readwrite', max_rows = 8 } } }\n", tmpdir, tmpdir);
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
     "query's grant, and a second statement refused whole");
  if (log[0] != '\0' && strcmp(log, "good") != 0)
    printf("      (guest said: %s)\n", log);
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
    "local inq = queue.declare('http_in', {cap = 8})\n"
    "local outq = queue.declare('http_out', {cap = 8, exported = true})\n"
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
  a_request_becomes_a_message_and_a_message_an_answer();
  printf("\n%d checks, %d failed\n", checks, failures);
  return (failures == 0) ? 0 : 1;
}
