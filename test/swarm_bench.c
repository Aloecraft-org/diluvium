/*
** swarm_bench.c
** What a swarm costs: how many agents, how much memory each, how fast they
** spawn, how fast messages cross their queues, and what hibernating and waking
** them is worth.
**
** This is a measurement tool and not a test. Nothing here asserts a number, for
** the reason test/footprint.c gives about itself: a figure baked into an
** assertion becomes a check that fails when someone adds a library rather than
** when something is wrong. What it does assert is *progress* -- every scenario
** runs under a hard wall-clock deadline and fails loudly when it stops making
** any, because doc/Hibernate.md records that the worst defect this subsystem
** ever had presented as a livelock in the release build and as a wrong answer
** under the sanitizers. A hang must not read as a slow run.
**
** Reporting follows script/bench.lua's doctrine, which is worth restating
** because it decides what the numbers here mean:
**
**   Counts are comparable, times are advisory. Bytes and instruction counts
**   are deterministic and reproduce anywhere; wall-clock on a shared runner
**   varies by more than most regressions worth catching.
**
** So every scenario reports a byte figure or a count as its headline and its
** timing beside it, and the JSON has both. Instruction counts are only
** collected under '--count', because 'dv_usage' counts nothing at all unless a
** non-zero instruction budget armed the hook -- and arming it costs a registry
** lookup every thousand instructions, so a counted run is not the same run as a
** timed one and their times must never be compared.
**
** Built with test/footprint.c's flag set rather than TEST_CFLAGS: that build
** installs ltests.h's own tracking allocator, whose bookkeeping would be counted
** as the instance's, at -O0. See the Makefile target.
*/

/* clock_gettime and sysconf, under -std=c99. src/analyze.c does the same. */
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dvs.h"


/* ====================================================================== */
/* Options                                                                */
/* ====================================================================== */

static int as_json = 0;
static double scale = 1.0;
static const char *only = NULL;
static uint64_t seed = 1;
static double deadline_s = 120.0;
static int count_mode = 0;
static int failures = 0;

/* A budget large enough never to bind, present only to arm the count hook.
   Zero everywhere else, so the hook is not installed and the run is honest. */
#define COUNT_BUDGET	((uint64_t)1 << 50)

static uint64_t budget_insns (void) {
  return count_mode ? COUNT_BUDGET : 0;
}


/* ====================================================================== */
/* Reporting                                                              */
/* ====================================================================== */

#define MAX_METRICS	40

typedef struct metric {
  char key[56];
  double value;
  const char *unit;             /* NULL for a bare count */
} metric;

typedef struct bench_case {
  const char *name;
  metric m[MAX_METRICS];
  int n;
  char note[1024];
} bench_case;

static bench_case cases[16];
static int ncases = 0;

static bench_case *open_case (const char *name) {
  bench_case *c = &cases[ncases++];
  memset(c, 0, sizeof(*c));
  c->name = name;
  return c;
}

static void put (bench_case *c, const char *key, double v, const char *unit) {
  metric *m;
  if (c->n >= MAX_METRICS)
    return;
  m = &c->m[c->n++];
  snprintf(m->key, sizeof(m->key), "%s", key);
  m->value = v;
  m->unit = unit;
}

static void note (bench_case *c, const char *fmt, ...) {
  va_list ap;
  size_t at = strlen(c->note);
  va_start(ap, fmt);
  if (at + 1 < sizeof(c->note)) {
    if (at > 0) { c->note[at++] = '\n'; c->note[at] = '\0'; }
    vsnprintf(c->note + at, sizeof(c->note) - at, fmt, ap);
  }
  va_end(ap);
}

/* A scenario that could not run at all. Reported, not fatal: one broken
   scenario should not cost the numbers from the others. */
static void broke (bench_case *c, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "swarm_bench: %s: ", c->name);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  note(c, "DID NOT COMPLETE");
  failures++;
}


static void report (void) {
  int i, j;
  if (as_json) {
    printf("{\n");
    printf("  \"tool\": \"swarm_bench\",\n");
    printf("  \"scale\": %g,\n", scale);
    printf("  \"seed\": %llu,\n", (unsigned long long)seed);
    printf("  \"counted\": %s,\n", count_mode ? "true" : "false");
    printf("  \"cases\": {\n");
    for (i = 0; i < ncases; i++) {
      printf("    \"%s\": {", cases[i].name);
      for (j = 0; j < cases[i].n; j++)
        printf("%s\n      \"%s\": %.4f", j ? "," : "", cases[i].m[j].key,
               cases[i].m[j].value);
      printf("%s    }%s\n", cases[i].n ? "\n" : "",
             i + 1 < ncases ? "," : "");
    }
    printf("  }\n}\n");
    return;
  }
  for (i = 0; i < ncases; i++) {
    printf("\n=== %s ===\n", cases[i].name);
    for (j = 0; j < cases[i].n; j++) {
      const metric *m = &cases[i].m[j];
      if (m->value == (double)(long long)m->value && m->value < 1e15)
        printf("  %-44s %14lld %s\n", m->key, (long long)m->value,
               m->unit ? m->unit : "");
      else
        printf("  %-44s %14.2f %s\n", m->key, m->value,
               m->unit ? m->unit : "");
    }
    if (cases[i].note[0] != '\0') {
      const char *p = cases[i].note;
      printf("\n");
      while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        printf("  %.*s\n", nl ? (int)(nl - p) : (int)strlen(p), p);
        if (nl == NULL) break;
        p = nl + 1;
      }
    }
  }
  printf("\nCounts and byte figures are deterministic and comparable across\n"
         "machines. Times are advisory: a shared runner varies by more than\n"
         "most regressions worth catching.\n");
}


/* ====================================================================== */
/* Clock, memory, randomness                                              */
/* ====================================================================== */

static double now_s (void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
** Resident set size, in bytes, or 0 where it cannot be had.
**
** Reported beside the allocator's own figures rather than instead of them: RSS
** includes the swarm's slot table, the process, and whatever the allocator has
** not returned to the system, so it answers "what does the host machine see"
** while 'dv_memory' answers "what is this agent holding". Both are wanted, and
** conflating them is how a per-agent figure ends up including the interpreter.
*/
static uint64_t statm_field (int which) {
#ifdef __linux__
  FILE *f = fopen("/proc/self/statm", "r");
  unsigned long total = 0, res = 0;
  if (f == NULL)
    return 0;
  if (fscanf(f, "%lu %lu", &total, &res) != 2)
    { total = 0; res = 0; }
  fclose(f);
  return (uint64_t)(which == 0 ? total : res) * (uint64_t)sysconf(_SC_PAGESIZE);
#else
  (void)which;
  return 0;                     /* Darwin has no statm; the figure is omitted */
#endif
}

static uint64_t rss_bytes (void) { return statm_field(1); }

/*
** Address space rather than resident pages, which is the only honest way to
** measure the slot table: 'dvs_new' callocs every slot and touches none of
** them, so a fresh table of a hundred thousand slots is a hundred and sixty
** megabytes the process has reserved and the resident figure cannot see. It
** becomes resident the moment instances start landing in it.
*/
static uint64_t vsz_bytes (void) { return statm_field(0); }

/* xorshift64*, so a --seed reproduces a churn run exactly. */
static uint64_t rng_state = 1;

static uint64_t rng (void) {
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  return rng_state * 2685821657736338717ull;
}

static uint32_t rng_below (uint32_t n) {
  return (n == 0) ? 0 : (uint32_t)(rng() % n);
}


/* ====================================================================== */
/* The host                                                               */
/* ====================================================================== */

/*
** A single-threaded host, which dvs.h is explicit is a legitimate host and not
** a stub: 'create' has nothing to associate, 'drive' is one 'dv_run' or one
** 'dv_resume'.
**
** It returns a context from 'create' for two reasons. The first is that
** 'release' guards the destroy callback on 'ctx != NULL', so a host returning
** NULL is never told an instance went away and cannot keep a roster. The second
** is that a parked program with a timeout needs somewhere to keep its deadline,
** and per-instance state is what the context is for.
**
** One 'dv_resume' per instance per step is the throughput unit of the whole
** system, so 'resumes' is reported: a scenario's messages-per-second divided by
** its resumes-per-second says whether the work or the scheduling is the cost.
*/

typedef struct slot_ctx {
  double deadline;              /* monotonic seconds; 0 for none */
  dvs_id id;
} slot_ctx;

typedef struct bench_host {
  dvs_swarm *sw;
  uint64_t steps;
  uint64_t drives;
  uint64_t resumes;
  uint64_t starts;
  uint32_t created;
  uint32_t destroyed;
} bench_host;


static void *host_create (void *ud, dvs_id id, dv_instance *inst) {
  bench_host *h = (bench_host *)ud;
  slot_ctx *sc = (slot_ctx *)calloc(1, sizeof(*sc));
  (void)inst;
  h->created++;
  if (sc != NULL)
    sc->id = id;
  return sc;
}

static void host_destroy (void *ud, dvs_id id, void *ctx) {
  bench_host *h = (bench_host *)ud;
  (void)id;
  h->destroyed++;
  free(ctx);
}

static int host_drive (void *ud, dvs_id id, dv_instance *inst, void *ctx) {
  bench_host *h = (bench_host *)ud;
  slot_ctx *sc = (slot_ctx *)ctx;
  dv_waitset ws;
  dv_status st;
  uint32_t i;
  (void)id;
  h->drives++;
  memset(&ws, 0, sizeof(ws));
  st = dv_waitset_get(inst, &ws);
  if (st == DV_OK && ws.n > 0) {
    for (i = 0; i < ws.n; i++) {
      dv_queue_info info;
      memset(&info, 0, sizeof(info));
      if (dv_queue_state(inst, ws.ids[i], &info) != DV_OK)
        continue;
      /* 8.3 leaves the choice to the host, and a wait for space is a different
         question from a wait for a message. Answering the wrong one resumes a
         program into a push that fails again. */
      if (ws.for_write ? (info.len < info.capacity) : (info.len > 0)) {
        if (sc != NULL) sc->deadline = 0;
        h->resumes++;
        return dv_resume(inst, ws.ids[i]) == DV_IDLE;
      }
    }
    /* Nothing ready. Time the park, because a program that asked for a timeout
       and never gets one is a deadlock this harness would report as a stall. */
    if (ws.timeout_ms >= 0 && sc != NULL) {
      double t = now_s();
      if (sc->deadline == 0)
        sc->deadline = t + (double)ws.timeout_ms / 1000.0;
      else if (t >= sc->deadline) {
        sc->deadline = 0;
        h->resumes++;
        return dv_resume(inst, 0) == DV_IDLE;
      }
    }
    return 1;                   /* still parked, still alive */
  }
  memset(&ws, 0, sizeof(ws));
  h->starts++;
  st = dv_run(inst, &ws);
  if (st == DV_BUSY)
    return 1;
  return st == DV_IDLE;
}


static dvs_swarm *swarm_open (bench_host *h, uint32_t max, uint32_t rate) {
  dvs_host vt;
  memset(h, 0, sizeof(*h));
  memset(&vt, 0, sizeof(vt));
  vt.create = host_create;
  vt.destroy = host_destroy;
  vt.drive = host_drive;
  vt.ud = h;
  h->sw = dvs_new(&vt, max, rate);
  return h->sw;
}

static int step (bench_host *h) {
  h->steps++;
  return dvs_step(h->sw);
}


/* ====================================================================== */
/* Small encoders and a template renderer                                 */
/* ====================================================================== */

/* A msgpack string of 'len' bytes of 'fill', into 'b'. Returns the length. */
static size_t mp_filler (uint8_t *b, size_t cap, size_t len, char fill) {
  size_t off;
  if (len < 32) { b[0] = (uint8_t)(0xa0 | len); off = 1; }
  else if (len < 256) { b[0] = 0xd9; b[1] = (uint8_t)len; off = 2; }
  else { b[0] = 0xda; b[1] = (uint8_t)(len >> 8); b[2] = (uint8_t)len; off = 3; }
  if (off + len > cap)
    return 0;
  memset(b + off, fill, len);
  return off + len;
}

/* A msgpack unsigned integer. */
static size_t mp_uint (uint8_t *b, uint64_t v) {
  if (v < 128) { b[0] = (uint8_t)v; return 1; }
  b[0] = 0xce;
  b[1] = (uint8_t)(v >> 24); b[2] = (uint8_t)(v >> 16);
  b[3] = (uint8_t)(v >> 8);  b[4] = (uint8_t)v;
  return 5;
}

/*
** Replace every '@KEY@' in 'tmpl' with its value. Arguments are key/value
** pairs, terminated by a NULL key. The result is malloc'd.
**
** A benchmark generates programs, which is what Guide.md 8 calls the ordinary
** shape for this system -- a coordinator writes a worker's parameters into the
** worker's source, because it cannot send a program a setup message before the
** program exists.
*/
static char *render (const char *tmpl, ...) {
  size_t cap = strlen(tmpl) + 4096;
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
        const char *key;
        const char *found = NULL;
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

/*
** A Lua long-bracket literal for 'src', so a supervisor can carry a worker's
** source without quoting every byte of it.
**
** Guide.md 8 says to use '%q' for values interpolated into generated code, and
** that is right for a value. This is a whole program, and a long bracket with a
** level chosen not to occur inside it is the shape that stays readable. The
** level is searched rather than assumed, because a program containing ']]'
** would otherwise end its own literal early -- which is the same injection the
** '%q' rule is about.
*/
static char *lua_longbracket (const char *src) {
  int level = 0;
  char open[16], close[16];
  char *out;
  size_t n;
  for (level = 0; level < 12; level++) {
    snprintf(close, sizeof(close), "]%.*s]", level,
             "============");
    if (strstr(src, close) == NULL)
      break;
  }
  snprintf(open, sizeof(open), "[%.*s[", level, "============");
  snprintf(close, sizeof(close), "]%.*s]", level, "============");
  n = strlen(src) + 2 * sizeof(open) + 8;
  out = (char *)malloc(n);
  if (out == NULL)
    return NULL;
  /* A leading newline is skipped by Lua's long-bracket rule, so the worker's
     first line is not indented by the opening bracket. */
  snprintf(out, n, "%s\n%s%s", open, src, close);
  return out;
}


/*
** Step until 'want' returns non-zero, or the deadline passes.
**
** The deadline is the point of this function. Every previous defect in this
** subsystem that mattered showed up as something that never finished, so a
** benchmark that loops "until done" with no bound converts a livelock into a
** hung CI job and a mystery. Returns 0 on timeout, having said so.
*/
static int spin_until (bench_host *h, int (*want) (bench_host *, void *),
                       void *ud, const char *what, uint64_t max_steps) {
  double until = now_s() + deadline_s;
  uint64_t taken = 0;
  while (!want(h, ud)) {
    if (taken++ >= max_steps) {
      fprintf(stderr, "swarm_bench: gave up after %llu steps waiting for %s\n",
              (unsigned long long)taken, what);
      return 0;
    }
    if (now_s() > until) {
      fprintf(stderr, "swarm_bench: gave up after %.0fs waiting for %s "
                      "(%llu steps)\n", deadline_s, what,
              (unsigned long long)taken);
      return 0;
    }
    if (step(h) <= 0) {
      fprintf(stderr, "swarm_bench: the swarm died while waiting for %s\n",
              what);
      return 0;
    }
  }
  return 1;
}


/* How many of ids[0..n) the swarm still knows. 'dvs_budget' answers
   DVS_UNKNOWN for a handle that is gone, which is the cheapest liveness
   question this ABI has -- and it is still a linear scan of the slot table,
   which is itself one of the things being measured. */
static uint32_t alive_of (dvs_swarm *sw, const dvs_id *ids, uint32_t n) {
  uint32_t i, live = 0;
  for (i = 0; i < n; i++)
    if (ids[i] != 0 && dvs_budget(sw, ids[i], NULL, NULL) == DVS_OK)
      live++;
  return live;
}


/*
** Collect the children of 'root', in spawn order.
**
** Handles are allocated from a counter and never reused, so the children of a
** root that spawned N of them are the live handles above the root's, and
** probing upward finds them all. It is a linear probe over a linear scan and
** therefore quadratic; it is done once, outside every timed region, and the
** cost of doing it is itself reported by the 'roster' scenario because a host
** that keeps no roster of its own has to pay exactly this.
*/
static uint32_t roster (dvs_swarm *sw, dvs_id root, dvs_id *out, uint32_t max) {
  uint32_t n = 0;
  dvs_id id;
  for (id = root + 1; id < root + 1 + max * 4 && n < max; id++) {
    if (dvs_budget(sw, id, NULL, NULL) == DVS_OK)
      out[n++] = id;
  }
  return n;
}


/* ====================================================================== */
/* The programs                                                           */
/* ====================================================================== */

/*
** A supervisor that spawns N copies of one worker and then parks.
**
** 'system/lifecycle' is declared with room for every request, which matters:
** the default capacity is 64, 'queue.push' answers false rather than raising
** when it is full, and a supervisor that ignores that answer comes up with 64
** agents however many it asked for. The count is checked and reported through
** 'log', so a short swarm is visible rather than silently halving the divisor
** of every per-agent figure.
**
** It parks on a queue nothing pushes to, because an instance that finishes is
** released and takes its queues with it -- and the whole point here is to
** measure a swarm while it is standing still.
*/
static const char SUPERVISOR[] =
  "local sys  = queue.declare('system/lifecycle', {capacity = @CAP@})\n"
  "local ev   = queue.declare('system/events', {capacity = 256})\n"
  "local log  = queue.declare('log', {capacity = 8, exported = true})\n"
  "local hold = queue.declare('hold', {capacity = 1})\n"
  "local WORKER = @WORKER@\n"
  "local asked = 0\n"
  "for i = 1, @N@ do\n"
  "  if queue.push(sys, {op = 'spawn', code = WORKER,\n"
  "                      caps = {@CAPS@},\n"
  "                      budget = {instructions = @BUDGET@},\n"
  "                      wake_on_message = @WAKE@}) then\n"
  "    asked = asked + 1\n"
  "  end\n"
  "end\n"
  "queue.push(log, 'asked:' .. asked)\n"
  "queue.wait({hold})\n";

/* The smallest agent that is still an agent: an inbox, and a loop that reads
   it. Everything the density figure measures beyond this is the interpreter. */
static const char WORKER_IDLE[] =
  "local inbox = queue.declare('work', {capacity = 4})\n"
  "local done  = queue.declare('done', {capacity = 8, exported = true})\n"
  "local seen = 0\n"
  "while true do\n"
  "  local id, v = queue.wait({inbox})\n"
  "  seen = seen + 1\n"
  "  queue.push(done, seen)\n"
  "end\n";

/* An agent that echoes what it is sent, for measuring what a queue costs. */
static const char WORKER_ECHO[] =
  "local inbox = queue.declare('work', {capacity = 8})\n"
  "local done  = queue.declare('done', {capacity = 8, exported = true})\n"
  "while true do\n"
  "  local id, v = queue.wait({inbox})\n"
  "  queue.push(done, v)\n"
  "end\n";

/*
** An agent that mints and verifies HS256 tokens.
**
** HMAC-SHA256 in Diluvium, because there is no crypto in a sealed guest at all:
** src/dhash.c's SHA-256 is reachable from the snapshot layer and from the
** generic host's crypto connector, and from nowhere a program can call. So this
** measures the interpreter doing the work, which is the honest thing to call it
** -- the same workload through 'host.crypto.jwt_sign' measures a hostcall round
** trip and a C HMAC, and the two numbers must not be quoted as one.
**
** The implementation is cross-checked against Python's hmac/hashlib: sha256
** ("abc") and HMAC-SHA256("secret", "abc") match byte for byte.
*/
static const char WORKER_JWT[] =
  "local inbox = queue.declare('work', {capacity = 8})\n"
  "local done  = queue.declare('done', {capacity = 8, exported = true})\n"
  "local M32 = 0xffffffff\n"
  "local K = {\n"
  "0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,\n"
  "0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,\n"
  "0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,\n"
  "0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,\n"
  "0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,\n"
  "0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,\n"
  "0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,\n"
  "0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,\n"
  "0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,\n"
  "0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,\n"
  "0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2}\n"
  "local function rotr (x, n) return ((x >> n) | (x << (32 - n))) & M32 end\n"
  "local w = {}\n"
  "local function sha256 (msg)\n"
  "  local h0,h1,h2,h3 = 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a\n"
  "  local h4,h5,h6,h7 = 0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19\n"
  "  local len = #msg\n"
  "  local pad = (56 - (len + 1) % 64) % 64\n"
  "  msg = msg .. '\\128' .. string.rep('\\0', pad) ..\n"
  "        string.pack('>I8', len * 8)\n"
  "  for base = 1, #msg, 64 do\n"
  "    for i = 0, 15 do w[i] = string.unpack('>I4', msg, base + i * 4) end\n"
  "    for i = 16, 63 do\n"
  "      local a = w[i-15]\n"
  "      local b = w[i-2]\n"
  "      local s0 = rotr(a, 7) ~ rotr(a, 18) ~ (a >> 3)\n"
  "      local s1 = rotr(b, 17) ~ rotr(b, 19) ~ (b >> 10)\n"
  "      w[i] = (w[i-16] + s0 + w[i-7] + s1) & M32\n"
  "    end\n"
  "    local a,b,c,d,e,f,g,hh = h0,h1,h2,h3,h4,h5,h6,h7\n"
  "    for i = 0, 63 do\n"
  "      local S1 = rotr(e, 6) ~ rotr(e, 11) ~ rotr(e, 25)\n"
  "      local ch = (e & f) ~ ((~e & M32) & g)\n"
  "      local t1 = (hh + S1 + ch + K[i + 1] + w[i]) & M32\n"
  "      local S0 = rotr(a, 2) ~ rotr(a, 13) ~ rotr(a, 22)\n"
  "      local mj = (a & b) ~ (a & c) ~ (b & c)\n"
  "      local t2 = (S0 + mj) & M32\n"
  "      hh = g g = f f = e e = (d + t1) & M32\n"
  "      d = c c = b b = a a = (t1 + t2) & M32\n"
  "    end\n"
  "    h0=(h0+a)&M32 h1=(h1+b)&M32 h2=(h2+c)&M32 h3=(h3+d)&M32\n"
  "    h4=(h4+e)&M32 h5=(h5+f)&M32 h6=(h6+g)&M32 h7=(h7+hh)&M32\n"
  "  end\n"
  "  return string.pack('>I4I4I4I4I4I4I4I4',h0,h1,h2,h3,h4,h5,h6,h7)\n"
  "end\n"
  "local function hmac (key, msg)\n"
  "  if #key > 64 then key = sha256(key) end\n"
  "  key = key .. string.rep('\\0', 64 - #key)\n"
  "  local o, i = {}, {}\n"
  "  for n = 1, 64 do\n"
  "    local b = key:byte(n)\n"
  "    o[n] = string.char(b ~ 0x5c)\n"
  "    i[n] = string.char(b ~ 0x36)\n"
  "  end\n"
  "  return sha256(table.concat(o) .. sha256(table.concat(i) .. msg))\n"
  "end\n"
  "local HEADER = bytes.tobase64url('{\"alg\":\"HS256\",\"typ\":\"JWT\"}')\n"
  "local KEY = @KEY@\n"
  "local function sign (claims)\n"
  "  local signing = HEADER .. '.' .. bytes.tobase64url(json.encode(claims))\n"
  "  return signing .. '.' .. bytes.tobase64url(hmac(KEY, signing))\n"
  "end\n"
  "local function verify (token)\n"
  "  local dot = token:find('.', 1, true)\n"
  "  local last = token:find('.', dot + 1, true)\n"
  "  if not dot or not last then return false end\n"
  "  local signing = token:sub(1, last - 1)\n"
  "  if token:sub(last + 1) ~= bytes.tobase64url(hmac(KEY, signing)) then\n"
  "    return false\n"
  "  end\n"
  "  return json.decode(bytes.frombase64url(token:sub(dot + 1, last - 1)))\n"
  "end\n"
  "local minted, verified, bad = 0, 0, 0\n"
  "while true do\n"
  "  local id, v = queue.wait({inbox})\n"
  "  local n = tonumber(v) or 0\n"
  "  local token = sign({sub = 'agent/' .. tostring(n), seq = n,\n"
  "                      scope = 'bench', ver = 1})\n"
  "  minted = minted + 1\n"
  "  local claims = verify(token)\n"
  "  if claims and claims.seq == n then verified = verified + 1\n"
  "  else bad = bad + 1 end\n"
  /* The totals as a string, so the harness can check that the work it is
     timing actually succeeded. A throughput figure for tokens that did not
     verify is not a throughput figure. */
  "  queue.push(done, minted .. '/' .. verified .. '/' .. bad)\n"
  "end\n";


/* ====================================================================== */
/* Shared scaffolding for a fan-out                                       */
/* ====================================================================== */

typedef struct fanout {
  bench_host h;
  dvs_swarm *sw;
  dvs_id root;
  dvs_id *ids;
  uint32_t n;                   /* how many children the roster found */
  uint32_t asked;               /* how many the supervisor asked for */
  char *supervisor;
} fanout;

static int want_children (bench_host *h, void *ud) {
  fanout *f = (fanout *)ud;
  return dvs_alive(h->sw) >= (int)f->asked + 1;
}

/*
** Bring up a root and 'n' workers of the given source. Returns 0 and says why
** on failure; the caller reports the scenario as not run.
*/
static int fanout_open (fanout *f, bench_case *c, const char *worker,
                        uint32_t n, uint32_t max_instances, uint32_t rate,
                        int wake_on_message) {
  static const char *caps[] = { "lifecycle", "queue:work/*", "queue:log" };
  char nbuf[32], capbuf[32], budbuf[32];
  char *quoted;
  memset(f, 0, sizeof(*f));
  f->asked = n;
  snprintf(nbuf, sizeof(nbuf), "%u", n);
  snprintf(capbuf, sizeof(capbuf), "%u", n + 8);
  snprintf(budbuf, sizeof(budbuf), "%llu",
           (unsigned long long)budget_insns());
  quoted = lua_longbracket(worker);
  if (quoted == NULL) { broke(c, "out of memory"); return 0; }
  f->supervisor = render(SUPERVISOR, "N", nbuf, "CAP", capbuf,
                         "WORKER", quoted, "BUDGET", budbuf,
                         "CAPS", "'queue:work/*'",
                         "WAKE", wake_on_message ? "true" : "false", NULL);
  free(quoted);
  if (f->supervisor == NULL) { broke(c, "out of memory"); return 0; }
  f->sw = swarm_open(&f->h, max_instances, rate);
  if (f->sw == NULL) { broke(c, "could not create a swarm"); return 0; }
  if (dvs_root(f->sw, f->supervisor, strlen(f->supervisor), caps, 3,
               budget_insns(), 0, &f->root) != DVS_OK) {
    broke(c, "the supervisor did not start: %s", dvs_last_error(f->sw));
    return 0;
  }
  f->ids = (dvs_id *)calloc(n, sizeof(dvs_id));
  if (f->ids == NULL) { broke(c, "out of memory"); return 0; }
  if (!spin_until(&f->h, want_children, f, "the workers to come up",
                  (uint64_t)n * 8 + 4096)) {
    broke(c, "only %d of %u instances came up", dvs_alive(f->sw), n + 1);
    return 0;
  }
  f->n = roster(f->sw, f->root, f->ids, n);
  if (f->n != n)
    note(c, "the roster found %u of %u children", f->n, n);
  return 1;
}

static void fanout_close (fanout *f) {
  if (f->sw != NULL)
    dvs_free(f->sw);
  free(f->ids);
  free(f->supervisor);
  memset(f, 0, sizeof(*f));
}

/* Drain every worker's 'done' queue, counting what came out. */
static uint64_t drain_done (fanout *f) {
  uint64_t got = 0;
  uint32_t i;
  for (i = 0; i < f->n; i++) {
    dv_instance *inst = dvs_instance(f->sw, f->ids[i]);
    dv_queue_id q;
    if (inst == NULL)
      continue;                 /* cached: its replies wait with it */
    q = dv_queue_lookup(inst, "done");
    if (q == 0)
      continue;
    for (;;) {
      const uint8_t *p = NULL;
      size_t len = 0;
      if (dv_queue_peek(inst, q, &p, &len) != DV_OK)
        break;
      dv_queue_release(inst, q);
      got++;
    }
  }
  return got;
}

/* The payload of a msgpack string, NUL-terminated into 'out'. Short forms
   only, which is all any worker here emits. */
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

/*
** Ask every worker for one more token and read the totals it reports back.
**
** Outside every timed region, and it exists because a benchmark that measures
** work which silently failed measures nothing. The JWT worker verifies each
** token it mints and counts what did not check out; this is where that count is
** read. 'bad' is reported as a metric rather than asserted, in keeping with the
** rest of the file -- but it is reported, so a run that produced nothing but
** failures cannot be quoted as a throughput number.
*/
static void jwt_health (fanout *f, uint64_t *minted, uint64_t *verified,
                        uint64_t *bad) {
  uint8_t msg[8];
  size_t msglen = mp_uint(msg, 7);
  uint32_t i, round;
  uint8_t *heard = (uint8_t *)calloc(f->n ? f->n : 1, 1);
  *minted = *verified = *bad = 0;
  if (heard == NULL)
    return;
  for (i = 0; i < f->n; i++)
    dvs_push(f->sw, f->ids[i], "work", msg, msglen);
  for (round = 0; round < 256; round++) {
    uint32_t outstanding = 0;
    step(&f->h);
    for (i = 0; i < f->n; i++) {
      dv_instance *inst = dvs_instance(f->sw, f->ids[i]);
      dv_queue_id q;
      if (inst == NULL) { heard[i] = 1; continue; }
      q = dv_queue_lookup(inst, "done");
      if (q == 0) { heard[i] = 1; continue; }
      for (;;) {
        const uint8_t *p = NULL;
        size_t len = 0;
        char text[64];
        unsigned long m = 0, v = 0, b = 0;
        if (dv_queue_peek(inst, q, &p, &len) != DV_OK)
          break;
        if (sscanf(mp_str(p, len, text, sizeof(text)), "%lu/%lu/%lu",
                   &m, &v, &b) == 3) {
          /* Cumulative, so the last one seen from each worker is the total. */
          heard[i] = 1;
        }
        dv_queue_release(inst, q);
      }
      if (!heard[i])
        outstanding++;
    }
    if (outstanding == 0)
      break;
  }
  /* Read the settled totals from each worker's final report. */
  for (i = 0; i < f->n; i++) {
    dv_instance *inst = dvs_instance(f->sw, f->ids[i]);
    dv_queue_id q;
    char text[64];
    unsigned long m = 0, v = 0, b = 0;
    if (inst == NULL)
      continue;
    q = dv_queue_lookup(inst, "done");
    if (q == 0)
      continue;
    dvs_push(f->sw, f->ids[i], "work", msg, msglen);
    for (round = 0; round < 8; round++) {
      const uint8_t *p = NULL;
      size_t len = 0;
      step(&f->h);
      if (dv_queue_peek(inst, q, &p, &len) == DV_OK) {
        if (sscanf(mp_str(p, len, text, sizeof(text)), "%lu/%lu/%lu",
                   &m, &v, &b) == 3) {
          *minted += m;
          *verified += v;
          *bad += b;
        }
        dv_queue_release(inst, q);
        break;
      }
    }
  }
  free(heard);
}

/* The instructions every instance has spent, or 0 when the hook was never
   armed -- which is the default, and why this is only reported under
   '--count'. */
static uint64_t total_insns (fanout *f) {
  uint64_t total = 0, one = 0;
  uint32_t i;
  dv_instance *inst = dvs_instance(f->sw, f->root);
  if (inst != NULL && dv_usage(inst, &one, NULL) == DV_OK)
    total += one;
  for (i = 0; i < f->n; i++) {
    inst = dvs_instance(f->sw, f->ids[i]);
    if (inst != NULL && dv_usage(inst, &one, NULL) == DV_OK)
      total += one;
  }
  return total;
}


/* ====================================================================== */
/* Scenario: density -- what a swarm costs standing still                 */
/* ====================================================================== */

static void bench_density (void) {
  bench_case *c = open_case("density");
  uint32_t n = (uint32_t)(512 * scale);
  fanout f;
  uint64_t rss0, rss_table, rss_up, rss_cached;
  uint64_t held = 0, peak = 0, cached = 0, root_held = 0;
  uint32_t i, swapped = 0;
  double t0, t_hib, t_wake;
  bench_host probe;

  /* The slot table first and on its own: 'dvs_new' callocs every slot up
     front, so a swarm's floor is set by 'max_instances' and not by how many
     agents ever exist. A per-agent figure that quietly includes it is wrong in
     the direction that flatters. */
  {
    const uint32_t probe_slots = 16384;   /* big enough to read off cleanly */
    dvs_swarm *empty;
    rss0 = vsz_bytes();
    empty = swarm_open(&probe, probe_slots, 64);
    rss_table = vsz_bytes();
    if (empty != NULL)
      dvs_free(empty);
    put(c, "agents", n, NULL);
    if (rss0 != 0 && rss_table > rss0)
      put(c, "slot_table_bytes_per_slot",
          (double)(rss_table - rss0) / (double)probe_slots, "B");
  }

  rss0 = rss_bytes();
  if (!fanout_open(&f, c, WORKER_IDLE, n, n + 1, 64, 1))
    { fanout_close(&f); return; }
  /* Let every worker reach its park, so what is measured is an idle agent and
     not one halfway through starting. */
  for (i = 0; i < 4; i++)
    step(&f.h);
  rss_up = rss_bytes();

  for (i = 0; i < f.n; i++) {
    dv_instance *inst = dvs_instance(f.sw, f.ids[i]);
    uint64_t now = 0, hi = 0;
    if (inst != NULL && dv_memory(inst, &now, &hi) == DV_OK) {
      held += now;
      peak += hi;
    }
  }
  {
    dv_instance *inst = dvs_instance(f.sw, f.root);
    dv_memory(inst, &root_held, NULL);
  }

  put(c, "resident_bytes_per_agent", f.n ? (double)held / f.n : 0, "B");
  put(c, "resident_peak_per_agent", f.n ? (double)peak / f.n : 0, "B");
  put(c, "supervisor_bytes", (double)root_held, "B");
  if (rss_up > rss0)
    put(c, "rss_bytes_per_agent", (double)(rss_up - rss0) / (f.n ? f.n : 1),
        "B");

  /* Swap them all out. 'dvs_hibernate' needs a parked instance, which every
     worker is; the request is refused rather than forced otherwise. */
  t0 = now_s();
  for (i = 0; i < f.n; i++)
    if (dvs_hibernate(f.sw, f.ids[i]) == DVS_OK)
      swapped++;
  t_hib = now_s() - t0;
  for (i = 0; i < f.n; i++)
    cached += dvs_cached_size(f.sw, f.ids[i]);
  rss_cached = rss_bytes();

  put(c, "hibernated", swapped, NULL);
  put(c, "cached_bytes_per_agent", swapped ? (double)cached / swapped : 0, "B");
  if (swapped && held)
    put(c, "resident_over_cached",
        ((double)held / f.n) / ((double)cached / swapped), "x");
  put(c, "hibernate_us_each", swapped ? t_hib * 1e6 / swapped : 0, "us");
  if (rss_up > rss_cached)
    put(c, "rss_freed_per_agent",
        (double)(rss_up - rss_cached) / (swapped ? swapped : 1), "B");

  t0 = now_s();
  for (i = 0; i < f.n; i++)
    dvs_wake(f.sw, f.ids[i]);
  t_wake = now_s() - t0;
  put(c, "wake_us_each", f.n ? t_wake * 1e6 / f.n : 0, "us");
  if (t_hib > 0)
    put(c, "wake_over_hibernate", t_hib > 0 ? t_wake / t_hib : 0, "x");

  /* Per gibibyte, which is the figure a deployment actually asks for. */
  if (held && f.n)
    put(c, "agents_per_GiB_resident",
        (double)(1ull << 30) / ((double)held / f.n), NULL);
  if (cached && swapped)
    put(c, "agents_per_GiB_cached",
        (double)(1ull << 30) / ((double)cached / swapped), NULL);

  note(c, "'resident' is the guest heap the counting allocator sees, which is "
          "the agent itself.");
  note(c, "'rss' is what the machine sees and includes the slot table, the "
          "interpreter and whatever the allocator has not given back.");
  note(c, "The supervisor holds one copy of the worker's source per queued "
          "spawn request, which is why its own figure is reported.");
  fanout_close(&f);
}


/* ====================================================================== */
/* Scenario: spawn -- how fast a supervisor can bring agents up           */
/* ====================================================================== */

typedef struct spawn_probe { uint32_t want; } spawn_probe;

static int want_n_alive (bench_host *h, void *ud) {
  return dvs_alive(h->sw) >= (int)((spawn_probe *)ud)->want;
}

static void spawn_at (bench_case *c, const char *label, const char *worker,
                      uint32_t n, uint32_t rate) {
  fanout f;
  spawn_probe want;
  double t0, elapsed;
  uint64_t steps0;
  char key[56];
  static const char *caps[] = { "lifecycle", "queue:work/*", "queue:log" };
  char nbuf[32], capbuf[32], budbuf[32];
  char *quoted;

  memset(&f, 0, sizeof(f));
  f.asked = n;
  snprintf(nbuf, sizeof(nbuf), "%u", n);
  snprintf(capbuf, sizeof(capbuf), "%u", n + 8);
  snprintf(budbuf, sizeof(budbuf), "%llu", (unsigned long long)budget_insns());
  quoted = lua_longbracket(worker);
  if (quoted == NULL) { broke(c, "out of memory"); return; }
  f.supervisor = render(SUPERVISOR, "N", nbuf, "CAP", capbuf, "WORKER", quoted,
                        "BUDGET", budbuf, "CAPS", "'queue:work/*'",
                        "WAKE", "false", NULL);
  free(quoted);
  if (f.supervisor == NULL) { broke(c, "out of memory"); return; }
  f.sw = swarm_open(&f.h, n + 1, rate);
  if (f.sw == NULL) { broke(c, "could not create a swarm"); return; }

  want.want = n + 1;
  t0 = now_s();
  if (dvs_root(f.sw, f.supervisor, strlen(f.supervisor), caps, 3,
               budget_insns(), 0, &f.root) != DVS_OK) {
    broke(c, "the supervisor did not start: %s", dvs_last_error(f.sw));
    fanout_close(&f);
    return;
  }
  steps0 = f.h.steps;
  if (!spin_until(&f.h, want_n_alive, &want, "the spawn to finish",
                  (uint64_t)n * 8 + 4096)) {
    broke(c, "%s: only %d of %u came up", label, dvs_alive(f.sw), n + 1);
    fanout_close(&f);
    return;
  }
  elapsed = now_s() - t0;

  snprintf(key, sizeof(key), "%s_spawns_per_s", label);
  put(c, key, elapsed > 0 ? n / elapsed : 0, "/s");
  snprintf(key, sizeof(key), "%s_us_per_spawn", label);
  put(c, key, n ? elapsed * 1e6 / n : 0, "us");
  snprintf(key, sizeof(key), "%s_steps", label);
  put(c, key, (double)(f.h.steps - steps0), NULL);
  snprintf(key, sizeof(key), "%s_source_bytes", label);
  put(c, key, (double)strlen(worker), "B");

  /* Tearing the whole subtree down from the top, which is the shape a
     supervisor's own death takes. */
  t0 = now_s();
  dvs_kill(f.sw, f.root);
  elapsed = now_s() - t0;
  snprintf(key, sizeof(key), "%s_subtree_kill_ms", label);
  put(c, key, elapsed * 1e3, "ms");
  snprintf(key, sizeof(key), "%s_subtree_kill_us_each", label);
  put(c, key, n ? elapsed * 1e6 / n : 0, "us");
  fanout_close(&f);
}

static void bench_spawn (void) {
  bench_case *c = open_case("spawn");
  uint32_t n = (uint32_t)(512 * scale);
  char *big = render(WORKER_JWT, "KEY", "'a benchmark signing key'", NULL);
  if (n < 4) n = 4;
  spawn_at(c, "small", WORKER_IDLE, n, 64);
  if (big != NULL) {
    spawn_at(c, "large", big, n, 64);
    free(big);
  }
  spawn_at(c, "rate8", WORKER_IDLE, n, 8);
  note(c, "'small' and 'large' differ only in the size of the program the "
          "spawn request carries.");
  note(c, "'rate8' is the same spawn at the default rate limit, which defers "
          "rather than denying: the burst arrives over the following steps.");
  note(c, "A subtree kill also runs on every ordinary exit, so its per-agent "
          "cost is paid by any swarm whose agents come and go.");
}


/* ====================================================================== */
/* Scenario: queue -- what a message costs                                */
/* ====================================================================== */

static void queue_at (bench_case *c, uint32_t agents, size_t payload,
                      uint32_t rounds) {
  fanout f;
  uint8_t *msg;
  size_t msglen;
  uint32_t r, i;
  uint64_t delivered = 0, pushed = 0, full = 0;
  double t0, elapsed;
  char key[56];

  msg = (uint8_t *)malloc(payload + 8);
  if (msg == NULL) { broke(c, "out of memory"); return; }
  msglen = mp_filler(msg, payload + 8, payload, 'x');

  if (!fanout_open(&f, c, WORKER_ECHO, agents, agents + 1, 64, 0)) {
    free(msg);
    fanout_close(&f);
    return;
  }

  t0 = now_s();
  for (r = 0; r < rounds; r++) {
    for (i = 0; i < f.n; i++) {
      dvs_status st = dvs_push(f.sw, f.ids[i], "work", msg, msglen);
      if (st == DVS_OK) pushed++;
      else full++;
    }
    step(&f.h);
    delivered += drain_done(&f);
  }
  /* Let what is in flight land, so the count is of round trips completed and
     not of messages abandoned mid-flight. */
  for (r = 0; r < 64 && delivered < pushed; r++) {
    step(&f.h);
    delivered += drain_done(&f);
  }
  elapsed = now_s() - t0;

  snprintf(key, sizeof(key), "p%zu_roundtrips_per_s", payload);
  put(c, key, elapsed > 0 ? (double)delivered / elapsed : 0, "/s");
  snprintf(key, sizeof(key), "p%zu_us_per_roundtrip", payload);
  put(c, key, delivered ? elapsed * 1e6 / delivered : 0, "us");
  snprintf(key, sizeof(key), "p%zu_MiB_per_s", payload);
  put(c, key, elapsed > 0
      ? (double)delivered * payload * 2 / elapsed / (1024 * 1024) : 0, "MiB/s");
  snprintf(key, sizeof(key), "p%zu_refused_pushes", payload);
  put(c, key, (double)full, NULL);
  if (count_mode && delivered) {
    snprintf(key, sizeof(key), "p%zu_insns_per_roundtrip", payload);
    put(c, key, (double)total_insns(&f) / delivered, NULL);
  }
  free(msg);
  fanout_close(&f);
}

static void bench_queue (void) {
  bench_case *c = open_case("queue");
  uint32_t agents = (uint32_t)(64 * scale);
  uint32_t rounds = (uint32_t)(64 * scale);
  if (agents < 1) agents = 1;
  if (rounds < 1) rounds = 1;
  put(c, "agents", agents, NULL);
  put(c, "rounds", rounds, NULL);
  queue_at(c, agents, 16, rounds);
  queue_at(c, agents, 256, rounds);
  queue_at(c, agents, 4096, rounds);
  note(c, "A round trip is a host push in, a guest wait, a guest push out and "
          "a host drain.");
  note(c, "Everything crossing a queue is msgpack-encoded and decoded even "
          "when it never leaves the instance, so this is partly a codec "
          "measurement -- which is what a queue costs here.");
  note(c, "A refused push is a full queue answering rather than growing, "
          "which is 6.2's backpressure being visible.");
}


/* ====================================================================== */
/* Scenario: jwt -- agents doing real work                                */
/* ====================================================================== */

static void bench_jwt (void) {
  bench_case *c = open_case("jwt");
  uint32_t agents = (uint32_t)(64 * scale);
  uint32_t tokens = (uint32_t)(64 * scale);
  fanout f;
  uint8_t msg[8];
  size_t msglen;
  uint32_t r, i;
  uint64_t done = 0, want;
  double t0, elapsed;
  char *worker;
  uint64_t held = 0;

  if (agents < 1) agents = 1;
  if (tokens < 1) tokens = 1;
  want = (uint64_t)agents * tokens;

  worker = render(WORKER_JWT, "KEY", "'a benchmark signing key'", NULL);
  if (worker == NULL) { broke(c, "out of memory"); return; }
  if (!fanout_open(&f, c, worker, agents, agents + 1, 64, 0)) {
    free(worker);
    fanout_close(&f);
    return;
  }
  put(c, "agents", f.n, NULL);
  put(c, "tokens_per_agent", tokens, NULL);
  put(c, "worker_source_bytes", (double)strlen(worker), "B");

  msglen = mp_uint(msg, 42);
  t0 = now_s();
  for (r = 0; r < tokens; r++) {
    for (i = 0; i < f.n; i++)
      dvs_push(f.sw, f.ids[i], "work", msg, msglen);
    /* One resume per instance per step is the unit, so a round of one message
       each takes a round of steps to come back. */
    while (done < (uint64_t)(r + 1) * f.n) {
      uint64_t before = done;
      if (step(&f.h) <= 0)
        break;
      done += drain_done(&f);
      if (done == before && now_s() - t0 > deadline_s) {
        broke(c, "stalled at %llu of %llu tokens",
              (unsigned long long)done, (unsigned long long)want);
        free(worker);
        fanout_close(&f);
        return;
      }
    }
  }
  elapsed = now_s() - t0;

  for (i = 0; i < f.n; i++) {
    dv_instance *inst = dvs_instance(f.sw, f.ids[i]);
    uint64_t one = 0;
    if (inst != NULL && dv_memory(inst, &one, NULL) == DV_OK)
      held += one;
  }

  put(c, "tokens", (double)done, NULL);
  put(c, "tokens_per_s", elapsed > 0 ? (double)done / elapsed : 0, "/s");
  put(c, "us_per_token", done ? elapsed * 1e6 / done : 0, "us");
  put(c, "tokens_per_s_per_agent",
      elapsed > 0 && f.n ? (double)done / elapsed / f.n : 0, "/s");
  put(c, "steps", (double)f.h.steps, NULL);
  put(c, "resumes_per_token", done ? (double)f.h.resumes / done : 0, NULL);
  put(c, "working_bytes_per_agent", f.n ? (double)held / f.n : 0, "B");
  if (count_mode && done)
    put(c, "insns_per_token", (double)total_insns(&f) / done, NULL);
  {
    uint64_t m = 0, v = 0, b = 0;
    jwt_health(&f, &m, &v, &b);
    put(c, "minted", (double)m, NULL);
    put(c, "verified", (double)v, NULL);
    put(c, "failed_verification", (double)b, NULL);
    if (b > 0)
      note(c, "WARNING: %llu tokens did not verify. The throughput above is "
              "not a throughput figure.", (unsigned long long)b);
  }

  note(c, "One token is an HS256 mint and a full verify -- four SHA-256 "
          "compressions of the signing input, in Diluvium, plus base64url and "
          "JSON both ways.");
  note(c, "There is no crypto a sealed guest can call, so this measures the "
          "interpreter. The same workload through the generic host's crypto "
          "connector measures a hostcall round trip and a C HMAC instead, and "
          "the two are not the same number.");
  note(c, "'resumes_per_token' is the scheduling cost of a token: the swarm "
          "drives every resident instance once per step.");
  free(worker);
  fanout_close(&f);
}


/* ====================================================================== */
/* Scenario: churn -- more agents than fit, woken at random               */
/* ====================================================================== */

/*
** Oversubscription. The swarm layer has no residency cap of its own -- its
** table bounds how many instances exist, resident or cached alike -- so the
** policy is the host's, which is where 9.1.2 says it belongs. This host keeps
** at most 'resident' agents in memory and swaps out the least recently used
** when a wake would pass that.
**
** Work arrives at a uniformly random agent, which is the worst case for any
** cache and the honest one to quote: a real workload with any locality does
** better, and a benchmark that assumed locality would be measuring its own
** assumption.
**
** Note what does *not* wake an agent: 'op = "query"' is answered from the slot
** and never restores anything, so a supervisor polling its children does not
** thrash them. A message does, and only when the agent asked to be woken.
*/
static void churn_at (bench_case *c, uint32_t total, uint32_t resident,
                      uint32_t ops, const char *label) {
  fanout f;
  uint8_t msg[8];
  size_t msglen;
  uint32_t i;
  uint64_t woke = 0, swapped = 0, hits = 0, misses = 0, refused = 0;
  uint64_t delivered = 0, cached_total = 0, cached_n = 0;
  uint8_t *is_resident;
  uint64_t *used_at;
  double t0, elapsed;
  char key[56];

  if (!fanout_open(&f, c, WORKER_IDLE, total, total + 1, 64, 1))
    { fanout_close(&f); return; }

  is_resident = (uint8_t *)calloc(f.n, 1);
  used_at = (uint64_t *)calloc(f.n, sizeof(uint64_t));
  if (is_resident == NULL || used_at == NULL) {
    broke(c, "out of memory");
    free(is_resident); free(used_at); fanout_close(&f);
    return;
  }
  for (i = 0; i < f.n; i++)
    is_resident[i] = 1;

  /* Down to the residency budget before the clock starts, so the measurement
     is of the steady state and not of the descent into it. */
  for (i = 0; i + resident < f.n; i++)
    if (dvs_hibernate(f.sw, f.ids[i]) == DVS_OK) {
      is_resident[i] = 0;
      swapped++;
    }
  swapped = 0;

  msglen = mp_uint(msg, 1);
  rng_state = seed;
  t0 = now_s();
  for (i = 0; i < ops; i++) {
    uint32_t pick = rng_below(f.n);
    dvs_status st;
    if (is_resident[pick]) hits++;
    else misses++;
    st = dvs_push(f.sw, f.ids[pick], "work", msg, msglen);
    if (st == DVS_LIMIT) refused++;
    else if (st != DVS_OK) { misses--; refused++; }
    used_at[pick] = i;
    if (!is_resident[pick]) {
      /* The swarm restores it on the next step, ahead of any live push. */
      if (step(&f.h) <= 0) break;
      if (dvs_resident(f.sw, f.ids[pick])) {
        is_resident[pick] = 1;
        woke++;
      }
    }
    else if ((i & 7) == 0) {
      if (step(&f.h) <= 0) break;
    }
    /* Hold the residency budget: evict the least recently used. */
    {
      uint32_t live = 0, j;
      for (j = 0; j < f.n; j++)
        live += is_resident[j];
      while (live > resident) {
        uint32_t oldest = f.n;
        uint64_t oldest_at = (uint64_t)-1;
        for (j = 0; j < f.n; j++)
          if (is_resident[j] && used_at[j] <= oldest_at) {
            oldest_at = used_at[j];
            oldest = j;
          }
        if (oldest == f.n)
          break;
        if (dvs_hibernate(f.sw, f.ids[oldest]) == DVS_OK) {
          is_resident[oldest] = 0;
          swapped++;
          live--;
        }
        else
          break;                /* still running: it will be evicted later */
      }
    }
    if (now_s() - t0 > deadline_s) {
      note(c, "%s stopped at %u of %u ops on the deadline", label, i, ops);
      ops = i + 1;
      break;
    }
  }
  for (i = 0; i < 32; i++) {
    step(&f.h);
    delivered += drain_done(&f);
  }
  elapsed = now_s() - t0;

  for (i = 0; i < f.n; i++) {
    size_t sz = dvs_cached_size(f.sw, f.ids[i]);
    if (sz > 0) { cached_total += sz; cached_n++; }
  }

  snprintf(key, sizeof(key), "%s_ops_per_s", label);
  put(c, key, elapsed > 0 ? ops / elapsed : 0, "/s");
  snprintf(key, sizeof(key), "%s_hit_rate", label);
  put(c, key, ops ? (double)hits / ops : 0, NULL);
  snprintf(key, sizeof(key), "%s_wakes", label);
  put(c, key, (double)woke, NULL);
  snprintf(key, sizeof(key), "%s_hibernates", label);
  put(c, key, (double)swapped, NULL);
  snprintf(key, sizeof(key), "%s_us_per_wake", label);
  put(c, key, woke ? elapsed * 1e6 / woke : 0, "us");
  snprintf(key, sizeof(key), "%s_refused_pushes", label);
  put(c, key, (double)refused, NULL);
  snprintf(key, sizeof(key), "%s_cached_bytes_each", label);
  put(c, key, cached_n ? (double)cached_total / cached_n : 0, "B");
  snprintf(key, sizeof(key), "%s_steps", label);
  put(c, key, (double)f.h.steps, NULL);
  snprintf(key, sizeof(key), "%s_us_per_op", label);
  put(c, key, ops ? elapsed * 1e6 / ops : 0, "us");

  /*
  ** The wake buffer's bound, measured rather than quoted. A cached agent that
  ** asked to be woken takes messages into a bounded host buffer; a full one is
  ** refused, because 6.2's bounded queues exist so that backpressure is visible
  ** and an unbounded wake buffer would be the one place in the system where it
  ** was not. Pushing hard at one sleeping agent without stepping is the only way
  ** to reach it -- everywhere else in this scenario the next step drains it.
  */
  if (f.n > 0) {
    uint32_t accepted = 0, denied = 0, j;
    /* Any agent that is currently cached will do; make one if none is. */
    uint32_t victim = f.n - 1;
    for (j = 0; j < f.n; j++)
      if (!dvs_resident(f.sw, f.ids[j])) { victim = j; break; }
    if (dvs_resident(f.sw, f.ids[victim]))
      dvs_hibernate(f.sw, f.ids[victim]);
    for (j = 0; j < 64; j++) {
      dvs_status st = dvs_push(f.sw, f.ids[victim], "work", msg, msglen);
      if (st == DVS_OK) accepted++;
      else denied++;
    }
    snprintf(key, sizeof(key), "%s_wake_buffer_accepted", label);
    put(c, key, (double)accepted, NULL);
    snprintf(key, sizeof(key), "%s_wake_buffer_refused_of_64", label);
    put(c, key, (double)denied, NULL);
  }
  free(is_resident);
  free(used_at);
  fanout_close(&f);
}

static void bench_churn (void) {
  bench_case *c = open_case("churn");
  uint32_t total = (uint32_t)(256 * scale);
  uint32_t ops = (uint32_t)(1024 * scale);
  if (total < 8) total = 8;
  if (ops < 16) ops = 16;
  put(c, "agents", total, NULL);
  put(c, "ops", ops, NULL);
  churn_at(c, total, total, ops, "all_resident");
  churn_at(c, total, total / 2, ops, "half_resident");
  churn_at(c, total, total / 8, ops, "eighth_resident");
  note(c, "Every agent asked to be woken by a message; work goes to a "
          "uniformly random one, which is the worst case for any cache.");
  note(c, "A push to a cached agent is buffered and the agent is restored on "
          "the next step, ahead of any live push. The buffer is bounded at 16 "
          "and a full one is refused rather than grown.");
  note(c, "'query' does not wake anything -- it is answered from the slot -- "
          "so polling children is not what thrashes them. A message is.");
}


/* ====================================================================== */
/* Scenario: step -- what an idle swarm costs                             */
/* ====================================================================== */

/*
** The step cost against two axes at once, because they are separate and the
** obvious experiment confounds them: 'dvs_step' walks the whole slot array
** three times whatever fraction of it is in use, and the lifecycle drain costs
** a slot-table scan per living instance. So a swarm with a big table and few
** agents can cost as much per step as a full one, and doubling the agents in a
** fixed table more than doubles the step.
*/
static void step_at (bench_case *c, uint32_t agents, uint32_t table,
                     const char *label) {
  fanout f;
  double t0, elapsed;
  uint32_t i;
  const uint32_t rounds = 200;
  char key[56];
  if (!fanout_open(&f, c, WORKER_IDLE, agents, table, 64, 0))
    { fanout_close(&f); return; }
  for (i = 0; i < 4; i++)
    step(&f.h);
  t0 = now_s();
  for (i = 0; i < rounds; i++)
    step(&f.h);
  elapsed = now_s() - t0;
  snprintf(key, sizeof(key), "%s_us_per_step", label);
  put(c, key, elapsed * 1e6 / rounds, "us");
  snprintf(key, sizeof(key), "%s_us_per_step_per_agent", label);
  put(c, key, agents ? elapsed * 1e6 / rounds / agents : 0, "us");
  fanout_close(&f);
}

static void bench_step (void) {
  bench_case *c = open_case("step");
  uint32_t n = (uint32_t)(256 * scale);
  if (n < 8) n = 8;
  step_at(c, n, n + 1, "tight_table");
  step_at(c, n, n * 8 + 1, "table_8x");
  step_at(c, n, n * 64 + 1, "table_64x");
  step_at(c, n / 4, n + 1, "quarter_agents");
  note(c, "Nothing is happening in any of these: every agent is parked on an "
          "empty queue and the step does no work at all.");
  note(c, "The three 'table' rows hold the agents fixed and vary only "
          "'max_instances'. What separates them is that 'dvs_step' walks the "
          "whole slot array three times whether or not the slots are in use, "
          "so a swarm sized for growth pays for the room it is not using on "
          "every step.");
  note(c, "'quarter_agents' varies the agents instead, against a tight table.");
}


/* ====================================================================== */
/* Scenario: roster -- what asking about instances costs                  */
/* ====================================================================== */

static void bench_roster (void) {
  bench_case *c = open_case("roster");
  uint32_t n = (uint32_t)(256 * scale);
  fanout f;
  double t0, elapsed;
  uint32_t i, reps = 200;
  volatile int sink = 0;
  if (n < 8) n = 8;
  if (!fanout_open(&f, c, WORKER_IDLE, n, n + 1, 64, 0))
    { fanout_close(&f); return; }
  t0 = now_s();
  for (i = 0; i < reps; i++)
    sink += (int)alive_of(f.sw, f.ids, f.n);
  elapsed = now_s() - t0;
  (void)sink;
  put(c, "agents", f.n, NULL);
  put(c, "us_per_full_walk", elapsed * 1e6 / reps, "us");
  put(c, "us_per_lookup", elapsed * 1e6 / reps / (f.n ? f.n : 1), "us");
  note(c, "One lookup by handle is a linear scan of the slot table, and every "
          "call that takes a handle pays it -- 'dvs_instance', 'dvs_push', "
          "'dvs_resident', 'dvs_budget'.");
  note(c, "So a host walking its own roster is quadratic in the swarm size. "
          "This is what that costs, and it is why the roster in this harness "
          "is built once and outside every timed region.");
  fanout_close(&f);
}


/* ====================================================================== */
/* main                                                                   */
/* ====================================================================== */

static const struct {
  const char *name;
  void (*run) (void);
} SCENARIOS[] = {
  { "density", bench_density },
  { "spawn",   bench_spawn },
  { "queue",   bench_queue },
  { "jwt",     bench_jwt },
  { "churn",   bench_churn },
  { "step",    bench_step },
  { "roster",  bench_roster },
};

static void usage (void) {
  printf("usage: swarm_bench [--json] [--scale F] [--only NAME] [--seed N]\n"
         "                   [--deadline SEC] [--count]\n\n"
         "  --json      machine-readable output\n"
         "  --scale F   multiply every size by F (default 1)\n"
         "  --only N    run one scenario: density spawn queue jwt churn "
         "step roster\n"
         "  --seed N    the churn scenario's random order (default 1)\n"
         "  --deadline  seconds before a scenario is called stalled "
         "(default 120)\n"
         "  --count     arm the instruction hook and report instruction "
         "counts.\n"
         "              Times from a counted run are not comparable with "
         "times from\n"
         "              an ordinary one.\n");
}

int main (int argc, char **argv) {
  int i;
  size_t s;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--json") == 0) as_json = 1;
    else if (strcmp(argv[i], "--count") == 0) count_mode = 1;
    else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
      scale = atof(argv[++i]);
    else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc)
      only = argv[++i];
    else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
      seed = strtoull(argv[++i], NULL, 10);
    else if (strcmp(argv[i], "--deadline") == 0 && i + 1 < argc)
      deadline_s = atof(argv[++i]);
    else { usage(); return (strcmp(argv[i], "--help") == 0) ? 0 : 2; }
  }
  if (scale <= 0) scale = 1;
  if (seed == 0) seed = 1;

  if (!as_json) {
    printf("Diluvium swarm benchmarks  (scale %g, seed %llu%s)\n",
           scale, (unsigned long long)seed,
           count_mode ? ", counted" : "");
    printf("dvs ABI %u, dv ABI %u\n", dvs_abi_version(), dv_abi_version());
  }
  for (s = 0; s < sizeof(SCENARIOS) / sizeof(SCENARIOS[0]); s++) {
    if (only != NULL && strcmp(only, SCENARIOS[s].name) != 0)
      continue;
    SCENARIOS[s].run();
  }
  report();
  if (failures > 0)
    fprintf(stderr, "\nswarm_bench: %d scenario(s) did not complete\n",
            failures);
  return failures > 0 ? 1 : 0;
}
