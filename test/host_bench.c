/*
** host_bench.c
** The other JWT number: agents minting and verifying tokens through the
** generic host's crypto connector, rather than in the interpreter.
**
** test/swarm_bench.c's 'jwt' scenario measures Diluvium doing HMAC-SHA256
** itself, because a sealed guest has no crypto it can call at all. This
** measures the path a deployment actually uses -- 'host/calls' out,
** 'crypto/jwt_sign' answered in C, 'host/replies' back -- and it is a
** different quantity with a different bottleneck. Quoting either as "how fast
** Diluvium does JWTs" without saying which would be a lie of omission, so both
** exist and both say what they are.
**
** What this one is really measuring is the hostcall round trip, because the
** HMAC at the far end is C and is not the cost. The number that matters is
** therefore the batch sweep: the host answers a whole instance's queued calls
** in one turn, but the guest is driven exactly once per turn, so a program
** that asks for one token and waits for it costs a full turn per token however
** cheap the token is. The same program asking for thirty-two at a time does
** not.
**
** Same reporting doctrine as swarm_bench: counts are comparable, times are
** advisory, nothing is asserted except progress.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "dhost.h"


static int as_json = 0;
static double scale = 1.0;
static double deadline_s = 120.0;
static int failures = 0;

static char tmpdir[128];

static double now_s (void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}


/* ---------------------------------------------------------------- output */

typedef struct row {
  char key[64];
  double value;
  const char *unit;
} row;

static row rows[64];
static int nrows = 0;

static void put (const char *key, double v, const char *unit) {
  row *r;
  if (nrows >= (int)(sizeof(rows) / sizeof(rows[0])))
    return;
  r = &rows[nrows++];
  snprintf(r->key, sizeof(r->key), "%s", key);
  r->value = v;
  r->unit = unit;
}

static void report (void) {
  int i;
  if (as_json) {
    printf("{\n  \"tool\": \"host_bench\",\n  \"scale\": %g,\n"
           "  \"cases\": {\n    \"jwt_connector\": {", scale);
    for (i = 0; i < nrows; i++)
      printf("%s\n      \"%s\": %.4f", i ? "," : "", rows[i].key,
             rows[i].value);
    printf("\n    }\n  }\n}\n");
    return;
  }
  printf("\n=== jwt through the crypto connector ===\n");
  for (i = 0; i < nrows; i++) {
    if (rows[i].value == (double)(long long)rows[i].value)
      printf("  %-40s %14lld %s\n", rows[i].key, (long long)rows[i].value,
             rows[i].unit ? rows[i].unit : "");
    else
      printf("  %-40s %14.2f %s\n", rows[i].key, rows[i].value,
             rows[i].unit ? rows[i].unit : "");
  }
}


/* -------------------------------------------------------------- fixtures */

static const char *fixture (const char *name, const char *content) {
  static char path[512];
  FILE *f;
  snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
  f = fopen(path, "w");
  if (f == NULL) {
    fprintf(stderr, "host_bench: cannot write %s\n", path);
    exit(1);
  }
  fputs(content, f);
  fclose(f);
  return path;
}


/*
** The worker.
**
** It declares 'host/calls' and 'host/replies' itself, with room for a whole
** batch. That is not tidiness: the 'host' guest library declares the pair at
** capacity 16 on first use, and the pump stops draining an instance the moment
** the reply queue's length plus its outstanding calls reach that capacity -- so
** a batch of 64 against a 16-deep reply queue is answered sixteen at a time, and
** the batch sweep would measure the queue rather than the batching.
**
** It also uses the raw queue idiom rather than 'host.crypto.jwt_sign', because
** the library's round trip sends one request and waits for that exact token.
** There is no pipelined form of it; batching has to be written out.
*/
static const char WORKER[] =
  "local calls   = queue.declare('host/calls', {capacity = @BATCH2@,\n"
  "                                             exported = true})\n"
  "local replies = queue.declare('host/replies', {capacity = @BATCH2@})\n"
  "local inbox   = queue.declare('work', {capacity = 4})\n"
  "local done    = queue.declare('done', {capacity = 8, exported = true})\n"
  "local BATCH = @BATCH@\n"
  "local tok = 0\n"
  "local minted, verified, bad = 0, 0, 0\n"
  "while true do\n"
  "  local _, want = queue.wait({inbox})\n"
  "  want = tonumber(want) or 1\n"
  "  local made = 0\n"
  "  while made < want do\n"
  "    local n = want - made\n"
  "    if n > BATCH then n = BATCH end\n"
  /* Ask for n signatures at once, then read n answers. */
  "    for i = 1, n do\n"
  "      tok = tok + 1\n"
  "      queue.push(calls, {tok = tok, call = 'crypto/jwt_sign',\n"
  "                         args = {claims = {sub = 'agent', seq = tok}}})\n"
  "    end\n"
  "    local tokens = {}\n"
  "    for i = 1, n do\n"
  "      local _, m = queue.wait({replies}, 30000)\n"
  "      if m and m.status == 'ok' then\n"
  "        minted = minted + 1\n"
  "        tokens[#tokens + 1] = m.value\n"
  "      else bad = bad + 1 end\n"
  "    end\n"
  /* And verify them the same way. */
  "    for i = 1, #tokens do\n"
  "      tok = tok + 1\n"
  "      queue.push(calls, {tok = tok, call = 'crypto/jwt_verify',\n"
  "                         args = {token = tokens[i]}})\n"
  "    end\n"
  "    for i = 1, #tokens do\n"
  "      local _, m = queue.wait({replies}, 30000)\n"
  /* jwt_verify answers 'ok' for a forgery too: the verdict is in the value,
     and counting the status alone would score every forgery a success. */
  "      if m and m.status == 'ok' and m.value and m.value.valid then\n"
  "        verified = verified + 1\n"
  "      else bad = bad + 1 end\n"
  "    end\n"
  "    made = made + n\n"
  "  end\n"
  /* Totals as a string, so the harness can check that the work it timed
     actually succeeded -- jwt_verify answers "ok" for a forgery, so the
     verdict has to be counted, not the status. */
  "  queue.push(done, minted .. '/' .. verified .. '/' .. bad)\n"
  "end\n";

static const char SUPERVISOR[] =
  "local sys  = queue.declare('system/lifecycle', {capacity = @CAP@})\n"
  "local ev   = queue.declare('system/events', {capacity = 256})\n"
  "local log  = queue.declare('log', {capacity = 8, exported = true})\n"
  "local hold = queue.declare('hold', {capacity = 1})\n"
  "local WORKER = @WORKER@\n"
  "local asked = 0\n"
  "for i = 1, @N@ do\n"
  "  if queue.push(sys, {op = 'spawn', code = WORKER,\n"
  "                      caps = {'host:crypto/*', 'queue:work/*'}}) then\n"
  "    asked = asked + 1\n"
  "  end\n"
  "end\n"
  "queue.push(log, 'asked:' .. asked)\n"
  "queue.wait({hold})\n";


static char *render (const char *tmpl, ...) {
  size_t cap = strlen(tmpl) + 8192;
  char *out = (char *)malloc(cap);
  size_t at = 0;
  const char *p = tmpl;
  if (out == NULL)
    return NULL;
  while (*p != '\0') {
    if (*p == '@') {
      const char *close = strchr(p + 1, '@');
      if (close != NULL) {
        size_t klen = (size_t)(close - p - 1);
        va_list ap;
        const char *key, *found = NULL;
        va_start(ap, tmpl);
        while ((key = va_arg(ap, const char *)) != NULL) {
          const char *val = va_arg(ap, const char *);
          if (strlen(key) == klen && memcmp(key, p + 1, klen) == 0) {
            found = val;
            break;
          }
        }
        va_end(ap);
        if (found != NULL) {
          size_t vlen = strlen(found);
          while (at + vlen + 1 > cap) {
            char *bigger = (char *)realloc(out, cap * 2);
            if (bigger == NULL) { free(out); return NULL; }
            out = bigger; cap *= 2;
          }
          memcpy(out + at, found, vlen);
          at += vlen;
          p = close + 1;
          continue;
        }
      }
    }
    if (at + 2 > cap) {
      char *bigger = (char *)realloc(out, cap * 2);
      if (bigger == NULL) { free(out); return NULL; }
      out = bigger; cap *= 2;
    }
    out[at++] = *p++;
  }
  out[at] = '\0';
  return out;
}


/* ------------------------------------------------------------- the sweep */

/*
** Count what came out of every worker's 'done' queue. The host's roster is the
** swarm's, so this walks handles rather than keeping its own -- the run is
** short and the walk is outside the timed region for the same reason
** swarm_bench's is.
*/
/* The payload of a msgpack string, NUL-terminated. Short forms only. */
static const char *mp_str (const uint8_t *b, size_t n, char *out, size_t cap) {
  size_t len, off;
  out[0] = '\0';
  if (n < 1) return out;
  if ((b[0] & 0xe0) == 0xa0) { len = b[0] & 0x1f; off = 1; }
  else if (b[0] == 0xd9 && n >= 2) { len = b[1]; off = 2; }
  else return out;
  if (off + len > n) len = (n > off) ? n - off : 0;
  if (len > cap - 1) len = cap - 1;
  memcpy(out, b + off, len);
  out[len] = '\0';
  return out;
}

/* What the workers reported, cumulatively, across the sweep just run. */
static uint64_t seen_minted, seen_verified, seen_bad;

static uint64_t drain_done (dh_host *h, const dvs_id *ids, uint32_t n) {
  uint64_t got = 0;
  uint32_t i;
  for (i = 0; i < n; i++) {
    dv_instance *inst = dvs_instance(h->sw, ids[i]);
    dv_queue_id q;
    if (inst == NULL)
      continue;
    q = dv_queue_lookup(inst, "done");
    if (q == 0)
      continue;
    for (;;) {
      const uint8_t *p = NULL;
      size_t len = 0;
      char text[64];
      unsigned long m = 0, v = 0, b = 0;
      if (dv_queue_peek(inst, q, &p, &len) != DV_OK)
        break;
      if (sscanf(mp_str(p, len, text, sizeof(text)), "%lu/%lu/%lu",
                 &m, &v, &b) == 3) {
        seen_minted += m;
        seen_verified += v;
        seen_bad += b;
      }
      dv_queue_release(inst, q);
      got++;
    }
  }
  return got;
}

static size_t mp_uint (uint8_t *b, uint64_t v) {
  if (v < 128) { b[0] = (uint8_t)v; return 1; }
  b[0] = 0xce;
  b[1] = (uint8_t)(v >> 24); b[2] = (uint8_t)(v >> 16);
  b[3] = (uint8_t)(v >> 8);  b[4] = (uint8_t)v;
  return 5;
}


static void sweep (uint32_t agents, uint32_t tokens, uint32_t batch) {
  dh_config cfg;
  dh_host h;
  char err[512];
  char cfgsrc[1024], path[512];
  char nbuf[32], capbuf[32], bbuf[32], b2buf[32];
  char *worker, *sup;
  dvs_id *ids;
  uint32_t i, found = 0;
  uint64_t turns = 0, done = 0;
  double t0, elapsed;
  char key[64];
  uint8_t msg[8];
  size_t msglen;

  seen_minted = seen_verified = seen_bad = 0;
  snprintf(bbuf, sizeof(bbuf), "%u", batch);
  snprintf(b2buf, sizeof(b2buf), "%u", batch * 2 + 4);
  worker = render(WORKER, "BATCH", bbuf, "BATCH2", b2buf, NULL);
  if (worker == NULL) { failures++; return; }
  {
    /* The worker's source goes into the supervisor as a long-bracket literal;
       it contains no ']]', which is checked rather than assumed by choosing a
       level that does not occur in it. */
    const char *open = "[==[", *close = "]==]";
    char *quoted;
    if (strstr(worker, "]==]") != NULL) { free(worker); failures++; return; }
    quoted = (char *)malloc(strlen(worker) + 16);
    if (quoted == NULL) { free(worker); failures++; return; }
    sprintf(quoted, "%s\n%s%s", open, worker, close);
    snprintf(nbuf, sizeof(nbuf), "%u", agents);
    snprintf(capbuf, sizeof(capbuf), "%u", agents + 8);
    sup = render(SUPERVISOR, "N", nbuf, "CAP", capbuf, "WORKER", quoted, NULL);
    free(quoted);
  }
  if (sup == NULL) { free(worker); failures++; return; }
  fixture("sup_jwt.lua", sup);

  snprintf(cfgsrc, sizeof(cfgsrc),
           "return {\n"
           "  supervisor = '%s/sup_jwt.lua',\n"
           "  max_instances = %u,\n"
           "  spawns_per_step = 64,\n"
           "  caps = { 'lifecycle', 'queue:*', 'host:crypto/*' },\n"
           /* No budget: 'instructions' is a lifetime cap, and a JWT loop that
              hits it is killed with an 'exceeded' event partway through the
              measurement. */
           "  connectors = { crypto = { key = 'a benchmark signing key, "
           "long enough', default_ttl = 3600 } }\n"
           "}\n", tmpdir, agents + 2);
  fixture("jwt.host.lua", cfgsrc);
  snprintf(path, sizeof(path), "%s/jwt.host.lua", tmpdir);

  if (dh_config_load(path, &cfg, err, sizeof(err)) != 0) {
    fprintf(stderr, "host_bench: config: %s\n", err);
    free(worker); free(sup); failures++; return;
  }
  if (dh_host_open(&h, &cfg, err, sizeof(err)) != 0) {
    fprintf(stderr, "host_bench: open: %s\n", err);
    free(worker); free(sup); failures++; return;
  }

  ids = (dvs_id *)calloc(agents, sizeof(dvs_id));
  if (ids == NULL) { dh_host_close(&h); free(worker); free(sup);
                     failures++; return; }

  /* Up. The root is handle 1 and children follow it in spawn order. */
  t0 = now_s();
  while (dvs_alive(h.sw) < (int)agents + 1) {
    dh_host_turn(&h);
    if (now_s() - t0 > deadline_s) {
      fprintf(stderr, "host_bench: only %d of %u instances came up\n",
              dvs_alive(h.sw), agents + 1);
      dh_host_close(&h); free(ids); free(worker); free(sup);
      failures++; return;
    }
  }
  for (i = 2; i < agents * 4 + 8 && found < agents; i++)
    if (dvs_budget(h.sw, i, NULL, NULL) == DVS_OK)
      ids[found++] = i;

  /* Ask each worker for its whole share in one message, so the pacing is the
     worker's batching and not the harness's feeding. */
  msglen = mp_uint(msg, tokens);
  t0 = now_s();
  for (i = 0; i < found; i++)
    dvs_push(h.sw, ids[i], "work", msg, msglen);
  while (done < found) {
    dh_host_turn(&h);
    turns++;
    done += drain_done(&h, ids, found);
    if (now_s() - t0 > deadline_s) {
      fprintf(stderr, "host_bench: batch %u stalled: %llu of %u workers "
                      "finished\n", batch, (unsigned long long)done, found);
      failures++;
      break;
    }
  }
  elapsed = now_s() - t0;

  {
    double total = (double)found * tokens;
    snprintf(key, sizeof(key), "batch%u_tokens_per_s", batch);
    put(key, elapsed > 0 ? total / elapsed : 0, "/s");
    snprintf(key, sizeof(key), "batch%u_us_per_token", batch);
    put(key, total > 0 ? elapsed * 1e6 / total : 0, "us");
    snprintf(key, sizeof(key), "batch%u_host_turns", batch);
    put(key, (double)turns, NULL);
    snprintf(key, sizeof(key), "batch%u_calls_per_turn", batch);
    /* Two hostcalls per token: one sign, one verify. */
    put(key, turns ? total * 2 / turns : 0, NULL);
    snprintf(key, sizeof(key), "batch%u_verified", batch);
    put(key, (double)seen_verified, NULL);
    snprintf(key, sizeof(key), "batch%u_failed", batch);
    put(key, (double)seen_bad, NULL);
    if (seen_bad > 0)
      fprintf(stderr, "host_bench: batch %u: %llu calls failed; the figures "
                      "above are not throughput figures\n", batch,
              (unsigned long long)seen_bad);
  }

  dh_host_close(&h);
  free(ids);
  free(worker);
  free(sup);
}


int main (int argc, char **argv) {
  int i;
  uint32_t agents, tokens;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0) as_json = 1;
    else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
      scale = atof(argv[++i]);
    else if (strcmp(argv[i], "--deadline") == 0 && i + 1 < argc)
      deadline_s = atof(argv[++i]);
    else {
      printf("usage: host_bench [--json] [--scale F] [--deadline SEC]\n\n"
             "  --scale F   multiply the agent count by F (default 1 = 32).\n"
             "              Tokens per agent are fixed at 1024, so the work\n"
             "              scales linearly and the turn counts stay\n"
             "              comparable across scales.\n");
      return strcmp(argv[i], "--help") == 0 ? 0 : 2;
    }
  }
  if (scale <= 0) scale = 1;
  /*
  ** Sized so that one sweep runs for about a second, and so that '--scale' is
  ** linear in the work done.
  **
  ** Both of those were wrong to begin with, and a run on a second machine is what
  ** showed it. The defaults were 16 agents of 64 tokens -- 1,024 tokens, about
  ** twenty milliseconds a sweep, because the HMAC at the far end is C and eats the
  ** work almost as fast as it can be handed over. At that length the three batch
  ** figures are noise: the two machines disagreed about batch 8 in one direction
  ** and about batch 1 and batch 32 in the other, which is not a coherent statement
  ** about either of them.
  **
  ** And scaling *both* terms made '--scale 4' sixteen times the work rather than
  ** four. So the tokens each agent asks for is fixed and only the agent count
  ** scales. That also leaves the turn count roughly invariant across scales --
  ** turns follow the tokens one agent asks for, not how many agents ask -- so the
  ** thing this benchmark exists to show, that batching collapses turns, stays
  ** legible as the swarm grows.
  */
  agents = (uint32_t)(32 * scale);
  tokens = 1024;
  if (agents < 1) agents = 1;

  snprintf(tmpdir, sizeof(tmpdir), "/tmp/host_bench.XXXXXX");
  if (mkdtemp(tmpdir) == NULL) {
    fprintf(stderr, "host_bench: no temporary directory\n");
    return 1;
  }

  if (!as_json) {
    printf("Diluvium host benchmarks  (scale %g)\n", scale);
    printf("%u agents, %u tokens each, through crypto/jwt_sign and "
           "crypto/jwt_verify\n", agents, tokens);
  }
  put("agents", agents, NULL);
  put("tokens_per_agent", tokens, NULL);
  sweep(agents, tokens, 1);
  sweep(agents, tokens, 8);
  sweep(agents, tokens, 32);
  report();
  if (!as_json) {
    printf("\n  One token is a sign and a verify: two hostcalls, each a push\n"
           "  out, a park, a C HMAC, and a resume. The batch is how many the\n"
           "  worker asks for before it waits.\n"
           "\n  The guest is driven once per host turn, so batch 1 costs a\n"
           "  turn per call however cheap the call is. That, and not the\n"
           "  HMAC, is what the batch sweep is measuring.\n");
  }
  if (failures > 0)
    fprintf(stderr, "\nhost_bench: %d sweep(s) did not complete\n", failures);
  return failures > 0 ? 1 : 0;
}
