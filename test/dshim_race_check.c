/*
** dshim_race_check.c
** The named-continuation registries, under concurrent instance creation.
**
** Why this test exists, and why the ones that came before it could not have
** found what it looks for.
**
** dv.h's contract is per *instance*: "the host must not call any function here
** for a given instance from more than one thread at a time". The continuation
** registries in dshim.c and dsnap.c are per *process*, and 'diluvium_openlibs'
** appends to them on every 'dv_new' -- so a host that honours the contract
** exactly, one thread per instance and never two on one, still puts two
** threads inside the same unsynchronised append the first time it creates two
** instances at once. That crashed, in 'strcmp', on a slot whose name was still
** NULL. src/dsync.h carries the full account of the mechanism.
**
** The shape of the bug decides the shape of the test, and this is the part
** worth reading before changing anything here:
**
**   'addcont' only *writes* when the name is not already registered. Once
**   every name is in, every later call matches in the scan and returns early,
**   and the array is read-only for the rest of the process. So the window is
**   the first few microseconds of the first concurrent 'dv_new' calls in a
**   *fresh* process, and then it is shut for good.
**
** Which is why a stress loop finds nothing. Sixty-four thousand instances
** across twenty-four threads samples the window exactly once and then hammers
** an immutable array; so does running an existing binary two thousand times,
** since the iterations inside each process contribute nothing after the first.
** The axis that matters is *fresh processes with several threads entering
** 'dv_new' at the same moment*, which is what the Makefile's
** 'dshim_race_check' target runs this binary many times to get.
**
** And it is why the sanitizer sweep said nothing for four days: 'sanitize_checks'
** builds -fsanitize=address,undefined, and neither ASan nor UBSan detects a data
** race. Neither does valgrind's default tool. The gate for this is ThreadSanitizer,
** which 'dshim_race_tsan' runs -- and which reports the race on 'dshim_ncont' on
** an unfixed tree on essentially the first execution.
**
** Not the ltests.h build, deliberately, and so TEST_CFLAGS is not used for it:
** that build installs its own accounting allocator over process-global counters,
** which several threads creating states at once would themselves race on. The
** subject here is the registries, and a harness that races on its own bookkeeping
** would bury them.
**
** What is asserted after the threads join is not only "nothing crashed" -- a
** lost update does not crash, it loses a name, and a registry quietly missing
** 'baselib.pcall' surfaces much later as a snapshot refused for a continuation
** the process could perfectly well have known. So every name this build
** registers is looked up by name, and round-tripped back to its own pointer.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include "dv.h"
#include "dshim.h"

static int checks = 0, failures = 0;

static void ok (int cond, const char *what) {
  checks++;
  if (cond) printf("[PASS] %s\n", what);
  else { printf("[FAIL] %s\n", what); failures++; }
}


/*
** The gate.
**
** Threads created one after another do not race: the first is through
** 'diluvium_openlibs' before the last exists, the registry is full, and the
** window this test is about never opens. So every thread is started, parked
** here, and released together.
**
** A mutex and a condition variable rather than a spin on a flag, because a
** plain flag is itself a data race and would be the first thing ThreadSanitizer
** reported -- a false one, in the harness, on top of the real one.
** 'pthread_barrier_t' would say this more directly and is not portable: macOS
** ships none.
*/
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_open = PTHREAD_COND_INITIALIZER;
static int gate = 0;

static void wait_for_gate (void) {
  pthread_mutex_lock(&gate_lock);
  while (!gate)
    pthread_cond_wait(&gate_open, &gate_lock);
  pthread_mutex_unlock(&gate_lock);
}

static void open_gate (void) {
  pthread_mutex_lock(&gate_lock);
  gate = 1;
  pthread_cond_broadcast(&gate_open);
  pthread_mutex_unlock(&gate_lock);
}


/*
** One racer: an instance of its own, created and used and freed on this thread
** alone, which is precisely what dv.h permits and what crashed.
**
** It parks on a queue and takes a snapshot rather than stopping at 'dv_new',
** because 'dv_new' reaches only the shim registry. The snapshot path is what
** reaches dsnap.c's second registry and 'ds_learnconts', whose own once-flag
** was unsynchronised too -- so without this the test would cover one of the
** three defects.
**
** A failure here is recorded rather than asserted, because a thread cannot
** report into the counters above without racing on them.
*/
typedef struct racer {
  pthread_t id;
  int made;             /* the instance was created */
  int parked;           /* and reached its 'queue.wait' */
  int snapped;          /* and wrote a snapshot */
} racer;

static void *race (void *ud) {
  static const char *src =
    "local work = queue.declare('work', {cap = 2})\n"
    "local id, v = queue.wait({work})\n";
  racer *r = (racer *)ud;
  dv_instance *inst;
  dv_waitset ws;
  size_t need = 0;

  wait_for_gate();

  inst = dv_new(NULL);
  if (inst == NULL)
    return NULL;
  r->made = 1;
  if (dv_load(inst, (const uint8_t *)src, strlen(src), "racer") == DV_OK) {
    memset(&ws, 0, sizeof(ws));
    if (dv_run(inst, &ws) == DV_IDLE) {
      r->parked = 1;
      /* Size enquiry only: the bytes are not the subject, reaching the
         snapshot registry is. */
      if (dv_snapshot(inst, NULL, NULL, 0, &need) == DV_OK && need > 0)
        r->snapped = 1;
    }
  }
  dv_free(inst);
  return NULL;
}


/*
** Every continuation this build registers, and where each comes from. A name
** missing here is the lost-update half of the bug, which does not crash.
*/
static const char *const expected[] = {
  "dtask.driver",     /* dtask.c, via diluvium_task_registerconts */
  "dqueue.wait",      /* dqueue.c, via luaopen_dqueue */
  "baselib.pcall",    /* dsnap.c, learned by canary in ds_learnconts */
  "baselib.xpcall",
  NULL
};


static int threadcount (void) {
  const char *e = getenv("DILUVIUM_RACE_THREADS");
  int n = 0;
  if (e != NULL)
    n = atoi(e);
  if (n < 4)
    n = 8;          /* four is the documented floor; eight races harder */
  if (n > 64)
    n = 64;
  return n;
}


int main (void) {
  int n = threadcount();
  racer *rs = (racer *)calloc((size_t)n, sizeof(racer));
  int i, made = 0, parked = 0, snapped = 0, started = 0;

  if (rs == NULL) {
    printf("[FAIL] room for %d racers\n", n);
    return 1;
  }

  printf("=== dshim: the registries under %d concurrent dv_new ===\n", n);

  for (i = 0; i < n; i++) {
    if (pthread_create(&rs[i].id, NULL, race, &rs[i]) == 0)
      started++;
    else
      break;
  }
  open_gate();
  for (i = 0; i < started; i++)
    pthread_join(rs[i].id, NULL);

  for (i = 0; i < started; i++) {
    made += rs[i].made;
    parked += rs[i].parked;
    snapped += rs[i].snapped;
  }

  /* Reaching here at all is the crash half: the unfixed tree dies in 'strcmp'
     inside the registry scan, on this thread or another, before this line. */
  ok(started == n, "every racer thread started");
  ok(made == started, "every racer created its own instance");
  ok(parked == started, "every racer's program parked");
  ok(snapped == started, "every racer reached the snapshot path");

  /* And this is the lost-update half, which is silent. */
  for (i = 0; expected[i] != NULL; i++) {
    const char *name = expected[i];
    lua_KFunction k = diluvium_shim_contfunc(name, strlen(name));
    char what[128];
    snprintf(what, sizeof(what), "'%s' survived the race", name);
    ok(k != NULL, what);
    if (k != NULL) {
      /*
      ** Back to *a* name, not necessarily to this one: 'pcall' and 'xpcall'
      ** share one continuation ('finishpcall' in lbaselib.c), so the mapping
      ** from function to name is many-to-one and 'contname' answers with the
      ** first registered match. What has to hold is that the entry the reverse
      ** lookup lands on describes the same function -- an entry stitched
      ** together from two racing appends would not.
      */
      const char *back = diluvium_shim_contname(k);
      snprintf(what, sizeof(what), "and reverses to an entry with the same "
                                   "function ('%s')", name);
      ok(back != NULL && diluvium_shim_contfunc(back, strlen(back)) == k, what);
    }
  }

  /* A registry that answered everything would prove nothing about the scan. */
  ok(diluvium_shim_contfunc("no.such.continuation",
                            strlen("no.such.continuation")) == NULL,
     "a name that was never registered is still not found");

  free(rs);
  printf("\n%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
