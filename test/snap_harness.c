/*
** snap_harness.c
** A snapshot in, a verdict out. For script/fuzz_snapshot.py.
**
** doc/Messaging.md 10.10 states the requirement this exists to check: a
** malformed snapshot must be refused, never crash. That cannot be asserted from
** inside the process being tested -- a crash is the thing being looked for -- so
** the fuzzer runs this as a subprocess and reads its exit status. Anything that
** is not one of the two statuses below is the failure.
**
**   0  the snapshot was accepted
**   1  the snapshot was refused, with a message on stdout
**
** A signal, a timeout, or any other status is a failure of 10.10.
**
** Two modes, because a structure-aware fuzzer needs a real snapshot to mutate
** rather than a guess at the format:
**
**   --emit FILE   park a small agent and write its snapshot
**   --load FILE   try to restore FILE
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dv.h"


/*
** The agent whose snapshot is fuzzed. Chosen to put something of every kind into
** the graph -- two frames, a closure with an upvalue, a shared table, a metatable,
** a to-be-closed slot, and queues with contents -- because a fuzzer can only
** corrupt fields that are there.
*/
static const char AGENT[] =
  "local work = queue.declare('work', {cap = 4})\n"
  "local log = queue.declare('log', {cap = 4})\n"
  "local shared = setmetatable({n = 0}, {kind = 'counter'})\n"
  "local function bump() shared.n = shared.n + 1 end\n"
  "shared.self = shared\n"
  "queue.push(log, 'started')\n"
  "local function body()\n"
  "  defer bump()\n"
  "  local id, v = queue.wait({work})\n"
  "  return tostring(v) .. ':' .. shared.n\n"
  "end\n"
  "return body()\n";


static int emit (const char *path) {
  dv_instance *inst = dv_new(NULL);
  dv_waitset ws;
  uint8_t *buf;
  size_t need = 0;
  FILE *f;
  if (inst == NULL) {
    printf("no instance\n");
    return 1;
  }
  if (dv_load(inst, (const uint8_t *)AGENT, sizeof(AGENT) - 1, "=agent")
      != DV_OK) {
    printf("load: %s\n", dv_last_error(inst));
    dv_free(inst);
    return 1;
  }
  memset(&ws, 0, sizeof(ws));
  if (dv_run(inst, &ws) != DV_IDLE) {
    printf("run: %s\n", dv_last_error(inst));
    dv_free(inst);
    return 1;
  }
  if (dv_snapshot(inst, NULL, NULL, 0, &need) != DV_OK || need == 0) {
    printf("size: %s\n", dv_last_error(inst));
    dv_free(inst);
    return 1;
  }
  buf = (uint8_t *)malloc(need);
  if (buf == NULL || dv_snapshot(inst, NULL, buf, need, &need) != DV_OK) {
    printf("snapshot: %s\n", dv_last_error(inst));
    free(buf);
    dv_free(inst);
    return 1;
  }
  f = fopen(path, "wb");
  if (f == NULL || fwrite(buf, 1, need, f) != need) {
    printf("write failed\n");
    if (f != NULL) fclose(f);
    free(buf);
    dv_free(inst);
    return 1;
  }
  fclose(f);
  printf("emitted %lu bytes\n", (unsigned long)need);
  free(buf);
  dv_free(inst);
  return 0;
}


static int load (const char *path) {
  dv_instance *inst;
  uint8_t *buf = NULL;
  long size;
  size_t got;
  FILE *f = fopen(path, "rb");
  dv_status st;
  if (f == NULL) {
    printf("cannot open %s\n", path);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0) size = 0;
  buf = (uint8_t *)malloc((size_t)size + 1);
  if (buf == NULL) {
    fclose(f);
    printf("out of memory\n");
    return 1;
  }
  got = fread(buf, 1, (size_t)size, f);
  fclose(f);
  inst = dv_new(NULL);
  if (inst == NULL) {
    free(buf);
    printf("no instance\n");
    return 1;
  }
  st = dv_restore(inst, NULL, buf, got);
  if (st != DV_OK) {
    printf("refused: %s\n", dv_last_error(inst));
    free(buf);
    dv_free(inst);
    return 1;
  }
  /*
  ** Accepted -- so go further, because "restores without crashing" is a weaker
  ** claim than 10.10 needs. A mutated snapshot that reconstructs a *runnable*
  ** thread must also run without corrupting anything, and the interesting
  ** corruptions are the ones that survive validation and only bite when the VM
  ** executes the frames. Resuming is where that shows up.
  */
  {
    dv_waitset ws;
    memset(&ws, 0, sizeof(ws));
    dv_waitset_get(inst, &ws);
    if (ws.n > 0)
      dv_resume(inst, ws.ids[0]);
    else
      dv_resume(inst, 0);
  }
  printf("accepted\n");
  free(buf);
  dv_free(inst);
  return 0;
}


int main (int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--emit") == 0)
    return emit(argv[2]);
  if (argc == 3 && strcmp(argv[1], "--load") == 0)
    return load(argv[2]);
  fprintf(stderr, "usage: snap_harness --emit FILE | --load FILE\n");
  return 2;
}
