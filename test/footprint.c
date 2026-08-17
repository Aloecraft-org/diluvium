/*
** footprint.c
** What an instance costs, measured rather than estimated.
**
** doc/Messaging.md 18.2's profile A drops hibernation, so a deployment's density is
** bounded by what a *resident* instance costs. That number had never been measured,
** and the one number that had -- a parked agent's snapshot, around 1.8 KB -- is the
** wrong one to plan with once instances stay resident.
**
** Measured through 'dv_usage', not from RSS. See 'measure' below for why.
**
** This prints; it does not assert a threshold. A footprint that drifts is worth
** knowing about, but a number baked into an assertion becomes a test that fails when
** someone adds a library rather than when something is wrong. What a reader wants is
** the figure and the arithmetic that follows from it.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dv.h"


/*
** Measured through 'dv_usage', which is the ABI's own answer to "what has this
** instance spent" and therefore the number a host actually plans with. It reports
** the high-water mark rather than the current figure, because 9.4's supervisor
** deciding whether a child needs more wants the peak -- and for capacity planning
** the peak is the honest figure too.
**
** Not RSS. RSS answers a different question (what the process asked the kernel for,
** including the allocator's slack and whatever it declines to return) and it varies
** by system allocator, so it would be a different number on every machine.
*/
static uint64_t measure (const char *what, const char *src, int run) {
  dv_instance *inst = dv_new(NULL);
  dv_waitset ws;
  dv_status st = DV_OK;
  uint64_t insns = 0, kb = 0, held = 0;
  if (inst == NULL) { printf("  %-34s could not create\n", what); return 0; }
  if (src != NULL &&
      dv_load(inst, (const uint8_t *)src, strlen(src), "=agent") != DV_OK) {
    printf("  %-34s would not load: %s\n", what, dv_last_error(inst));
    dv_free(inst);
    return 0;
  }
  if (run) {
    memset(&ws, 0, sizeof(ws));
    st = dv_run(inst, &ws);
  }
  /*
  ** Say what the program actually did, which this did not used to.
  **
  ** Every program below opened with 'queue.declare("inbox", ...)', and 'inbox'
  ** is one of 6.6's two reserved queues -- it exists from the moment the library
  ** opens, so declaring it raises. The run status was discarded, so an instance
  ** that had died on its first line was measured and reported as one parked on a
  ** wait, and the figure the density claim rests on was the cost of a raise.
  ** Printing the status is what makes that impossible to repeat quietly.
  */
  dv_usage(inst, &insns, &kb);
  dv_memory(inst, &held, NULL);
  printf("  %-34s peak %4lu KB   held %4lu KB   %lu instructions   %s\n", what,
         (unsigned long)kb, (unsigned long)(held / 1024),
         (unsigned long)insns, run ? dv_status_name(st) : "not run");
  if (run && st == DV_ERROR)
    printf("  %-34s   ^ raised: %s\n", "", dv_last_error(inst));
  dv_free(inst);
  return kb;
}


int main (void) {
  uint64_t parked = 0;

  printf("=== what one instance costs, by state ===\n");
  measure("dv_new only", NULL, 0);
  measure("loaded, not started", "local q = queue.lookup('inbox') "
          "queue.wait({q})", 0);
  /*
  ** The one that matters. 10.2 calls idle-on-inbox "the overwhelmingly common state
  ** at scale", and under profile A that state is resident rather than swapped out --
  ** so this is the figure a density claim rests on.
  */
  parked = measure("parked on queue.wait",
          "local q = queue.lookup('inbox') queue.wait({q})", 1);
  measure("parked, three queues declared",
          "local a = queue.lookup('inbox') "
          "local b = queue.lookup('outbox') "
          "local c = queue.declare('log', {capacity = 16}) "
          "queue.wait({a})", 1);
  measure("parked, holding 4 KB of state",
          "local s = {} for i = 1, 128 do s[i] = string.rep('x', 32) end "
          "local q = queue.lookup('inbox') "
          "queue.wait({q})", 1);

  /*
  ** What that means for a machine, spelled out. A reader planning capacity should not
  ** have to do this arithmetic from a KB figure, and writing it here means the
  ** assumptions are visible: this counts guest heap only, and says nothing about the
  ** host's own per-instance bookkeeping or the OS.
  */
  if (parked > 0) {
    double kb = (double)parked;
    printf("\n=== what that means, at the parked figure of %.1f KB ===\n", kb);
    printf("  guest heap only: no host bookkeeping, no allocator slack, no OS\n");
    printf("  %6d resident instances -> %8.1f MB\n", 100, 100 * kb / 1024.0);
    printf("  %6d resident instances -> %8.1f MB\n", 1000, 1000 * kb / 1024.0);
    printf("  %6d resident instances -> %8.1f MB\n", 10000, 10000 * kb / 1024.0);
    printf("  a 1 GB machine holds roughly %.0f, half of it %.0f\n",
           (1024.0 * 1024.0) / kb, (512.0 * 1024.0) / kb);
    /*
    ** And the comparison that decides whether profile C is worth building: a
    ** snapshot of the same agent is about 1.8 KB (measured in dvs_check.c), so
    ** hibernation buys roughly this ratio in memory -- and costs the snapshot
    ** layer's defects.
    */
    printf("  a snapshot of the same agent is ~1.8 KB, so hibernating trades\n"
           "  about %.0fx the memory for 18.1's snapshot defects\n", kb / 1.8);
  }
  printf("\nnothing is asserted here: this is a measurement, not a threshold\n");
  return 0;
}
