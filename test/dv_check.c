/*
** dv_check.c
** Contract tests for the instance ABI.
**
** In C for the same reason dtask_check.c is: this surface has no guest binding
** and never will -- it is what a host calls, so a host is what has to call it
** in a test. The Rust wrapper in bindings/rust exercises the same ground from
** the other side; this one is here so the ABI is covered on every target
** whether or not a Rust toolchain is present.
**
** Written against dv.h alone, with no access to the runtime's internals, which
** is also a check that dv.h is sufficient on its own.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "dv.h"


static int checks = 0, failures = 0;

static void ok (int cond, const char *what) {
  checks++;
  if (cond) printf("[PASS] %s\n", what);
  else { printf("[FAIL] %s\n", what); failures++; }
}

static void eq_i (long long got, long long want, const char *what) {
  checks++;
  if (got == want) printf("[PASS] %s\n", what);
  else {
    printf("[FAIL] %s (got %lld, wanted %lld)\n", what, got, want);
    failures++;
  }
}

static void eq_st (dv_status got, dv_status want, const char *what) {
  checks++;
  if (got == want) printf("[PASS] %s\n", what);
  else {
    printf("[FAIL] %s (got %s, wanted %s)\n", what,
           dv_status_name(got), dv_status_name(want));
    failures++;
  }
}


/* msgpack for a few small values, so the test needs no encoder of its own. */
static const uint8_t MP_ONE[]   = { 0x01 };
static const uint8_t MP_TWO[]   = { 0x02 };
static const uint8_t MP_HI[]    = { 0xa2, 'h', 'i' };

static dv_instance *load (const char *src, uint32_t flags) {
  dv_config cfg;
  dv_instance *inst;
  memset(&cfg, 0, sizeof(cfg));
  cfg.abi_version = DV_ABI_VERSION;
  cfg.flags = flags;
  inst = dv_new(&cfg);
  if (inst == NULL) return NULL;
  if (dv_load(inst, (const uint8_t *)src, strlen(src), "=test") != DV_OK) {
    printf("       load failed: %s\n", dv_last_error(inst));
    dv_free(inst);
    return NULL;
  }
  return inst;
}


static void version (void) {
  eq_i(dv_abi_version(), DV_ABI_VERSION, "the library reports its ABI version");
  {
    /* A stale binding must be refused at creation, not left to misread a
       message later. Zero means "did not say", which stays allowed so a C
       caller can pass NULL or an empty config. */
    dv_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.abi_version = DV_ABI_VERSION + 99u;
    ok(dv_new(&cfg) == NULL, "a mismatched ABI version is refused by dv_new");
  }
  {
    dv_instance *inst = dv_new(NULL);
    ok(inst != NULL, "a NULL config means the defaults");
    dv_free(inst);
  }
  ok(strcmp(dv_status_name(DV_IDLE), "DV_IDLE") == 0,
     "statuses have names, for logs and binding error types");
}


static void run_to_completion (void) {
  dv_instance *inst = load("return 1", 0);
  ok(inst != NULL, "a program loads");
  if (inst == NULL) return;
  eq_st(dv_run(inst, NULL), DV_DONE, "a program with no waits runs to DV_DONE");
  eq_st(dv_run(inst, NULL), DV_DONE, "and stays DV_DONE when run again");
  dv_free(inst);
}


static void errors (void) {
  dv_instance *inst = load("local function inner() error('boom', 0) end inner()", 0);
  const char *msg;
  if (inst == NULL) { ok(0, "load"); return; }
  eq_st(dv_run(inst, NULL), DV_ERROR, "a raising program reports DV_ERROR");
  msg = dv_last_error(inst);
  ok(msg != NULL && strstr(msg, "boom") != NULL,
     "and the message crosses the boundary");
  ok(msg != NULL && strstr(msg, "stack traceback") != NULL,
     "with a traceback, built on the thread where it was raised");
  ok(msg != NULL && strstr(msg, "inner") != NULL,
     "naming the failing frame rather than the ABI");
  dv_free(inst);

  inst = dv_new(NULL);
  eq_st(dv_load(inst, (const uint8_t *)"this is not lua", 15, "=bad"),
        DV_ERROR, "a syntax error is reported by dv_load");
  ok(dv_last_error(inst) != NULL, "with a message");
  eq_st(dv_run(inst, NULL), DV_ERROR, "running with nothing loaded is an error");
  dv_free(inst);
}


static void queues (void) {
  dv_instance *inst = load(
    "local q = queue.declare('work', {capacity = 2, exported = true}) "
    "queue.push(q, 'from the guest') "
    "return 0", 0);
  dv_queue_id work, inbox;
  dv_queue_info info;
  uint8_t buf[64];
  size_t n;
  if (inst == NULL) { ok(0, "load"); return; }

  /* Queues are guest-declared (6.1), so nothing is visible until it has run. */
  eq_i(dv_queue_lookup(inst, "work"), 0,
       "a guest queue does not exist before the program runs");
  inbox = dv_queue_lookup(inst, "inbox");
  ok(inbox != 0, "but inbox and outbox are there from the start (6.6)");

  eq_st(dv_run(inst, NULL), DV_DONE, "the program runs");
  work = dv_queue_lookup(inst, "work");
  ok(work != 0, "and its queue is now visible to the host");
  eq_i(dv_queue_lookup(inst, "nope"), 0, "an unknown name is handle 0");

  eq_st(dv_queue_state(inst, work, &info), DV_OK, "state can be read");
  eq_i(info.capacity, 2, "capacity crosses");
  eq_i(info.len, 1, "so does the current length");
  eq_i(info.enabled, 1, "and enabled");
  eq_i(info.exported, 1, "and exported");
  eq_st(dv_queue_state(inst, 9999, &info), DV_QUEUE_UNKNOWN,
        "an unknown handle is DV_QUEUE_UNKNOWN");

  /* Bytes out. */
  eq_st(dv_queue_pop(inst, work, buf, sizeof(buf), &n), DV_OK,
        "the guest's message pops");
  /* 14 bytes of text behind a one-byte fixstr header. Asserting the length
     rather than only the content is what catches a header width mistake. */
  ok(n == 15 && buf[0] == (0xa0 | 14) &&
     memcmp(buf + 1, "from the guest", 14) == 0,
     "as the msgpack the guest encoded, header included");
  eq_st(dv_queue_pop(inst, work, buf, sizeof(buf), &n), DV_QUEUE_EMPTY,
        "then the queue is empty");

  /* Bytes in, and the boundaries of 6.4. */
  eq_st(dv_queue_push(inst, work, MP_ONE, sizeof(MP_ONE)), DV_OK,
        "the host can push");
  eq_st(dv_queue_push(inst, work, MP_TWO, sizeof(MP_TWO)), DV_OK, "twice");
  eq_st(dv_queue_push(inst, work, MP_HI, sizeof(MP_HI)), DV_QUEUE_FULL,
        "and is told when the queue is full rather than blocking");
  eq_st(dv_queue_push(inst, 9999, MP_ONE, 1), DV_QUEUE_UNKNOWN,
        "pushing to an unknown handle says so");

  /* A short buffer must not lose the message. */
  eq_st(dv_queue_pop(inst, work, buf, 0, &n), DV_BUFFER_TOO_SMALL,
        "a buffer that is too small reports it");
  eq_i(n, 1, "and says how much room the message needs");
  eq_st(dv_queue_pop(inst, work, buf, sizeof(buf), &n), DV_OK,
        "the message is still there afterwards");
  eq_i(buf[0], 1, "and it is the one that was pushed");
  dv_free(inst);
}


static void zero_copy (void) {
  dv_instance *inst = load(
    "local q = queue.declare('out', {exported = true}) "
    "queue.push(q, 'zero copy') return 0", 0);
  dv_queue_id q;
  const uint8_t *p = NULL;
  size_t n = 0;
  dv_queue_info info;
  if (inst == NULL) { ok(0, "load"); return; }
  dv_run(inst, NULL);
  q = dv_queue_lookup(inst, "out");
  eq_st(dv_queue_peek(inst, q, &p, &n), DV_OK, "peek borrows the message");
  ok(p != NULL && n == 10 && memcmp(p + 1, "zero copy", 9) == 0,
     "without copying it");
  dv_queue_state(inst, q, &info);
  eq_i(info.len, 1, "and leaves it in the queue");
  dv_queue_release(inst, q);
  dv_queue_state(inst, q, &info);
  eq_i(info.len, 0, "release removes it, so peek plus release is a pop");
  eq_st(dv_queue_peek(inst, q, &p, &n), DV_QUEUE_EMPTY,
        "peeking an empty queue says so");
  dv_free(inst);
}


static int notified_count = 0;
static dv_queue_id notified_id = 0;

static void on_message (void *ud, dv_queue_id id) {
  (void)ud;
  notified_count++;
  notified_id = id;
}

static void notification (void) {
  /* Two pushes, one to an exported queue and one not: only the first should
     reach the host, since a queue the host cannot see is none of its business. */
  dv_instance *inst = load(
    "local pub = queue.declare('pub', {exported = true}) "
    "local priv = queue.declare('priv') "
    "queue.push(priv, 1) "
    "queue.push(pub, 2) "
    "return 0", 0);
  if (inst == NULL) { ok(0, "load"); return; }
  notified_count = 0;
  dv_set_notify(inst, on_message, NULL);
  dv_run(inst, NULL);
  eq_i(notified_count, 1, "the host is told about a push to an exported queue");
  ok(notified_id == dv_queue_lookup(inst, "pub"),
     "and told which queue it was");
  dv_free(inst);
}


static void parking (void) {
  /*
  ** The whole point of the ABI: the program parks, the host is told what it is
  ** waiting for, the host decides, and the program continues. Nothing here
  ** sleeps -- the host answers immediately, because it can.
  */
  dv_instance *inst = load(
    "local inb = queue.lookup('inbox') "
    "local out = queue.declare('reply', {exported = true}) "
    "for _ = 1, 3 do "
    "  local id, msg = queue.wait({inb}) "
    "  queue.push(out, msg * 10) "
    "end "
    "return 0", 0);
  dv_waitset ws;
  dv_queue_id inbox, reply;
  dv_status st;
  uint8_t buf[32];
  size_t n;
  int rounds = 0;
  if (inst == NULL) { ok(0, "load"); return; }
  inbox = dv_queue_lookup(inst, "inbox");

  memset(&ws, 0, sizeof(ws));
  st = dv_run(inst, &ws);
  eq_st(st, DV_IDLE, "the program parks and the host is told");
  eq_i(ws.n, 1, "one queue in the wait-set");
  ok(ws.ids[0] == inbox, "and it is the inbox");
  eq_i(ws.for_write, 0, "waiting for a message, not for space");
  ok(ws.timeout_ms < 0, "with no timeout, since the program set none");

  eq_st(dv_run(inst, NULL), DV_BUSY,
        "running a parked program is DV_BUSY, not an answer on its behalf");

  while (st == DV_IDLE && rounds < 5) {
    rounds++;
    /* Answer it: put a message in, then say which handle fired. */
    eq_st(dv_queue_push(inst, inbox, MP_ONE, sizeof(MP_ONE)), DV_OK,
          "the host pushes a message in");
    st = dv_resume(inst, inbox);
  }
  eq_st(st, DV_DONE, "after three rounds the program finishes");
  eq_i(rounds, 3, "having parked exactly three times");

  reply = dv_queue_lookup(inst, "reply");
  eq_st(dv_queue_pop(inst, reply, buf, sizeof(buf), &n), DV_OK,
        "and its replies came back");
  eq_i(buf[0], 10, "with the value it computed");
  dv_free(inst);
}


static void timeout_answer (void) {
  /* A host that decides the timeout elapsed says so with handle 0. */
  dv_instance *inst = load(
    "local q = queue.declare('t') "
    "local id, v, status = queue.wait({q}, 5000) "
    "local out = queue.declare('res', {exported = true}) "
    "queue.push(out, status) return 0", 0);
  dv_waitset ws;
  uint8_t buf[32];
  size_t n;
  if (inst == NULL) { ok(0, "load"); return; }
  eq_st(dv_run(inst, &ws), DV_IDLE, "it parks with a timeout");
  eq_i(ws.timeout_ms, 5000, "and the host is told how long it asked for");
  /* The host does not have to wait the 5 seconds: it owns the clock, so it can
     decide the time is up whenever it likes. That is the point of 8.3. */
  eq_st(dv_resume(inst, 0), DV_DONE, "answering 0 means the timeout elapsed");
  eq_st(dv_queue_pop(inst, dv_queue_lookup(inst, "res"), buf, sizeof(buf), &n),
        DV_OK, "the program recorded what it saw");
  ok(n == 8 && memcmp(buf + 1, "timeout", 7) == 0,
     "and it saw a timeout");
  dv_free(inst);
}


static void closed_answer (void) {
  /* The host names a handle; the runtime works out that it has gone away, so a
     host never has to model the difference. */
  dv_instance *inst = load(
    "local q = queue.declare('c') "
    "local id, v, status = queue.wait({q}) "
    "local out = queue.declare('res', {exported = true}) "
    "queue.push(out, status) return 0", 0);
  dv_waitset ws;
  uint8_t buf[32];
  size_t n;
  if (inst == NULL) { ok(0, "load"); return; }
  eq_st(dv_run(inst, &ws), DV_IDLE, "it parks");
  /* Nothing was pushed, and the queue is empty, so naming it means "gone". */
  eq_st(dv_resume(inst, ws.ids[0]), DV_DONE, "the host names the handle anyway");
  dv_queue_pop(inst, dv_queue_lookup(inst, "res"), buf, sizeof(buf), &n);
  ok(n == 8 && memcmp(buf + 1, "timeout", 7) == 0,
     "and an unready handle reads as a timeout rather than a phantom message");
  dv_free(inst);
}


static void blocking_push_from_guest (void) {
  /* A guest pushing to a full 'block' queue parks for space, and the wait-set
     says so -- which is how a host knows to drain rather than to deliver. */
  dv_instance *inst = load(
    "local q = queue.declare('b', {capacity = 1, on_full = 'block', "
    "                              exported = true}) "
    "queue.push(q, 1) queue.push(q, 2) return 0", 0);
  dv_waitset ws;
  uint8_t buf[32];
  size_t n;
  if (inst == NULL) { ok(0, "load"); return; }
  eq_st(dv_run(inst, &ws), DV_IDLE, "the guest parks on a full block queue");
  eq_i(ws.for_write, 1, "and the wait-set says it wants space, not a message");
  eq_i(ws.n, 1, "for one queue");
  /* Drain it, then say the handle fired. */
  eq_st(dv_queue_pop(inst, ws.ids[0], buf, sizeof(buf), &n), DV_OK,
        "the host drains a slot");
  eq_st(dv_resume(inst, ws.ids[0]), DV_DONE,
        "and the parked push completes");
  dv_free(inst);
}


static void text_only (void) {
  /* A host that did not compile the bytes itself can refuse precompiled ones
     rather than trusting the loader's checks. */
  static const uint8_t fake_binary[] = { 0x1b, 'L', 'u', 'a', 0x55, 0x46 };
  dv_config cfg;
  dv_instance *inst;
  memset(&cfg, 0, sizeof(cfg));
  cfg.abi_version = DV_ABI_VERSION;
  cfg.flags = DV_FLAG_TEXT_ONLY;
  inst = dv_new(&cfg);
  eq_st(dv_load(inst, fake_binary, sizeof(fake_binary), "=bin"), DV_ERROR,
        "DV_FLAG_TEXT_ONLY refuses a precompiled chunk");
  dv_free(inst);
  inst = dv_new(NULL);
  eq_st(dv_load(inst, (const uint8_t *)"return 1", 8, "=src"), DV_OK,
        "and source still loads");
  dv_free(inst);
}


static void top_level_yield (void) {
  /* An ordinary yield is not a wait-set, and the host is told rather than
     having a meaning invented for it. */
  dv_instance *inst = load("coroutine.yield()", 0);
  if (inst == NULL) { ok(0, "load"); return; }
  eq_st(dv_run(inst, NULL), DV_ERROR,
        "a top-level coroutine.yield is reported, not interpreted");
  ok(dv_last_error(inst) != NULL &&
     strstr(dv_last_error(inst), "wait-set") != NULL,
     "with a message saying why");
  dv_free(inst);
}


static void layout (void) {
  /* The numbers a wasm binding depends on, checked here so a native build
     notices if a struct changes shape. It cannot catch a wasm32-versus-LP64
     difference -- nothing running on this machine can -- which is exactly why
     the binding asks the runtime rather than hardcoding them. */
  uint32_t v[DV_LAYOUT_COUNT];
  eq_i(dv_layout(NULL, 0), DV_LAYOUT_COUNT, "dv_layout reports how many it has");
  eq_i(dv_layout(v, DV_LAYOUT_COUNT), DV_LAYOUT_COUNT, "and fills them all in");
  eq_i(v[DV_LAYOUT_WAITSET_N], 0, "the wait-set count is first");
  ok(v[DV_LAYOUT_WAITSET_IDS] == 4, "the handles follow it");
  ok(v[DV_LAYOUT_WAITSET_TIMEOUT] > v[DV_LAYOUT_WAITSET_IDS],
     "the timeout comes after the handles");
  ok(v[DV_LAYOUT_WAITSET_SIZE] >= v[DV_LAYOUT_WAITSET_FOR_WRITE] + 1,
     "and every field is inside the struct");
  eq_i(dv_layout(v, 2), 2, "a short buffer is filled as far as it goes");
}


int main (void) {
  printf("=== dv ABI contract ===\n");
  layout();
  version();
  run_to_completion();
  errors();
  queues();
  zero_copy();
  notification();
  parking();
  timeout_answer();
  closed_answer();
  blocking_push_from_guest();
  text_only();
  top_level_yield();
  printf("\n%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
