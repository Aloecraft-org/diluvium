/*
** fm4-probe.c
** One door per run: does a budgeted instance come back to the host?
**
** The measurements in doc/FM-4.md come from this file. Not a test: a test
** asserts, and half of these doors hang the tree as it stands.
**
** Build, after 'make _build_step0' (the same flags as 'make dv_check'):
**
**   gcc -DLUA_USER_H='"ltests.h"' -O1 -g -DLUA_USE_LINUX -DMAKE_LIB -I.data \
**       -o dist/fm4_probe doc/attic/fm4-probe.c .data/onelua.c -lm -ldl
**
** Usage: fm4_probe <door> [seconds]
**   Loads DOORS[door] into a fresh instance under BUDGET_INSNS, runs it, then
**   frees it. If either phase is still going after 'seconds', a SIGALRM
**   handler prints the instance's usage and exits 124. FM4_FULL=1 in the
**   environment prints the whole error message rather than its first line.
**
** To measure a prototype, point -I and onelua.c at a tree with
** doc/attic/fm4-prototype.diff applied.
*/

#include <stddef.h>

/* -- entry points ---------------------------------------------------- */
int main (int argc, char **argv);

/* -- configurable values --------------------------------------------- */
#define BUDGET_INSNS   1000000u   /* the budget dv_check.c's escape tests use */
#define DEFAULT_ALARM  10         /* seconds before a phase is called a hang */

/* -- the doors: name, program ---------------------------------------- */
#define SPIN "function() while true do end end"
static const char *const DOORS[] = {
  "control",   "while true do end",
  "pcall",     "while true do pcall(" SPIN ") end",
  "xpcall",    "while true do xpcall(" SPIN ", function(e) return e end) end",
  "sortpcall", "while true do pcall(table.sort, {3,2,1}, function(a,b) "
               "pcall(" SPIN ") return a < b end) end",
  "coresume",  "local f = " SPIN " while true do "
               "coroutine.resume(coroutine.create(f)) end",
  "coclose",   "while true do local co = coroutine.create(function() "
               "local x <close> = setmetatable({}, {__close = " SPIN "}) "
               "coroutine.yield() end) coroutine.resume(co) coroutine.close(co) end",
  "load",      "while true do load(" SPIN ") end",
  "gc_run",    "setmetatable({}, {__gc = " SPIN "}) collectgarbage() collectgarbage()",
  "gc_free",   "KEEP = setmetatable({}, {__gc = " SPIN "})",
  "matcher",   "return string.find(('a'):rep(300), '(.-)(.-)(.-)(.-)(.-)b')",
  "xhandler",  "xpcall(" SPIN ", " SPIN ")",
  "gc_remark", "local mt = {} mt.__gc = function(o) setmetatable(o, mt) "
               "while true do end end setmetatable({}, mt) "
               "while true do collectgarbage() end",
  "gc_alloc",  "local mt = {} mt.__gc = function(o) setmetatable(o, mt) "
               "while true do end end setmetatable({}, mt) "
               "while true do local s = ('x'):rep(9000) end",
  "memcatch",  "while true do pcall(function() local t = {} "
               "while true do t[#t+1] = ('x'):rep(100) end end) end",
  NULL, NULL
};

/* depth: harness */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dv.h"

static dv_instance *probe_inst;
static const char *probe_phase = "setup";
static struct timespec t0;

static double ms_since (const struct timespec *from) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (now.tv_sec - from->tv_sec) * 1000.0 +
         (now.tv_nsec - from->tv_nsec) / 1e6;
}

static void on_alarm (int sig) {
  uint64_t used = 0;
  char buf[160];
  (void)sig;
  if (probe_inst != NULL)
    dv_usage(probe_inst, &used, NULL);
  snprintf(buf, sizeof(buf),
           "  HANG in %s after %.0f ms: insn_used=%llu exceeded=%d\n",
           probe_phase, ms_since(&t0), (unsigned long long)used,
           probe_inst ? dv_exceeded(probe_inst) : -1);
  if (write(1, buf, strlen(buf)) < 0) _exit(125);
  _exit(124);
}

int main (int argc, char **argv) {
  const char *src = NULL;
  int i, seconds = DEFAULT_ALARM;
  dv_waitset ws;
  dv_status st;
  uint64_t used = 0;
  double run_ms, free_ms;
  if (argc < 2) { fprintf(stderr, "usage: fm4_probe <door> [seconds]\n"); return 2; }
  for (i = 0; DOORS[i] != NULL; i += 2)
    if (strcmp(DOORS[i], argv[1]) == 0) src = DOORS[i + 1];
  if (src == NULL) { fprintf(stderr, "no such door: %s\n", argv[1]); return 2; }
  if (argc > 2) seconds = atoi(argv[2]);
  printf("door %-9s ", argv[1]);
  fflush(stdout);
  signal(SIGALRM, on_alarm);
  probe_inst = dv_new(NULL);
  if (probe_inst == NULL) { printf("no instance\n"); return 1; }
  if (dv_set_budget(probe_inst, BUDGET_INSNS, 0) != DV_OK) { printf("no budget\n"); return 1; }
  if (dv_load(probe_inst, (const uint8_t *)src, strlen(src), "=door") != DV_OK) {
    printf("load: %s\n", dv_last_error(probe_inst));
    return 1;
  }
  memset(&ws, 0, sizeof(ws));
  probe_phase = "dv_run";
  clock_gettime(CLOCK_MONOTONIC, &t0);
  alarm(seconds);
  st = dv_run(probe_inst, &ws);
  run_ms = ms_since(&t0);
  alarm(0);
  dv_usage(probe_inst, &used, NULL);
  printf("run=%-9s exceeded=%d insn_used=%-8llu run_ms=%-8.1f",
         dv_status_name(st), dv_exceeded(probe_inst),
         (unsigned long long)used, run_ms);
  if (st == DV_ERROR) {
    const char *m = dv_last_error(probe_inst);
    if (getenv("FM4_FULL")) printf("\n--- full error ---\n%s\n---", m ? m : "(null)"); else printf(" err=%.40s", m ? m : "(null)");
  }
  fflush(stdout);
  probe_phase = "dv_free";
  clock_gettime(CLOCK_MONOTONIC, &t0);
  alarm(seconds);
  dv_free(probe_inst);
  free_ms = ms_since(&t0);
  alarm(0);
  probe_inst = NULL;
  printf(" free_ms=%.1f\n", free_ms);
  return 0;
}
