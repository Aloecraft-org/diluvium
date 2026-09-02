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


/*
** The payload of a msgpack string, NUL-terminated into 'out'.
**
** Assuming a one-byte header works only up to 31 bytes: past that the codec emits
** str8 (0xd9) with a length byte, and 'buf + 1' then starts one byte early. Every
** message here is an error string, which is exactly the length that crosses the
** boundary, so the header is read rather than guessed.
*/
static const char *mp_str (const uint8_t *b, size_t n, char *out, size_t cap) {
  size_t len, off;
  out[0] = '\0';
  if (n < 1) return out;
  if ((b[0] & 0xe0) == 0xa0) { len = b[0] & 0x1f; off = 1; }
  else if (b[0] == 0xd9 && n >= 2) { len = b[1]; off = 2; }
  else if (b[0] == 0xda && n >= 3) { len = ((size_t)b[1] << 8) | b[2]; off = 3; }
  else return out;
  if (off + len > n) len = (n > off) ? n - off : 0;
  if (len > cap - 1) len = cap - 1;
  memcpy(out, b + off, len);
  out[len] = '\0';
  return out;
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


/*
** 6.4's `disabled` row, from the host side. Audit finding 10.
**
** M4 was accepted on "every row of 6.4 from the host side" and seven of the
** eight rows were true of it; this was the eighth. Deleting the enabled check
** in 'diluvium_queue_push_bytes' turned nothing red, and what that check exists
** for is 6.1's reason -- a program going down should reject cleanly rather than
** accept messages it will never read. Nothing in the tree said so from the side
** a host is on: the only assertion on the flag was `enabled == 1`, the positive
** direction, and the one guest that disabled a queue disabled a private one no
** host push can reach.
**
** Disabled is not destroyed, which is the other half: what the queue already
** holds stays readable, so a host can drain what a program pushed before it
** stopped taking more.
*/
static void a_disabled_queue_refuses_a_host_push (void) {
  dv_instance *inst = load(
    "local q = queue.declare('work', {capacity = 2, exported = true}) "
    "queue.push(q, 'held') "
    "queue.disable(q) "
    "return 0", 0);
  dv_queue_id work;
  dv_queue_info info;
  uint8_t buf[64];
  size_t n = 0;
  if (inst == NULL) { ok(0, "load"); return; }
  eq_st(dv_run(inst, NULL), DV_DONE, "a program that disables a queue runs");
  work = dv_queue_lookup(inst, "work");
  ok(work != 0, "and the queue is still there afterwards");
  memset(&info, 0, sizeof(info));
  eq_st(dv_queue_state(inst, work, &info), DV_OK, "its state reads");
  eq_i(info.enabled, 0, "and reports itself disabled");
  eq_st(dv_queue_push(inst, work, MP_ONE, sizeof(MP_ONE)), DV_QUEUE_DISABLED,
        "a host push into it is refused rather than silently stored");
  eq_i(info.len, 1, "the message it held before is still held");
  eq_st(dv_queue_pop(inst, work, buf, sizeof(buf), &n), DV_OK,
        "and still pops, because disabled is not destroyed");
  ok(n == 5 && memcmp(buf + 1, "held", 4) == 0,
     "as the message the program pushed");
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


/*
** The host names a handle that has gone away, and the runtime works out that it
** has, so a host never has to model the difference.
**
** The version that used to stand here was called 'closed_answer' and its comment
** said "naming it means gone", but it named a *live empty* queue and asserted
** "timeout": the name and comment described one path and the assertion took
** another. Trying to make it match turned up why it could not. A queue closes only
** when the guest calls 'queue.destroy' or 'queue.disable', a parked guest cannot
** call either, and the host has no call that closes one -- so
** DILUVIUM_FIRED_CLOSED is *unreachable through 'dv_resume'*. The branch computing
** it there is defensive, and the reachable path is the synchronous one asserted
** below and in test_wait.lua: a wait on a queue that can never deliver fires at
** once, so the host is never asked about it.
*/
static void closed_answer (void) {
  dv_instance *inst = load(
    "local q = queue.declare('c') "
    "queue.disable(q) "
    "local out = queue.declare('res', {exported = true}) "
    "local id, v, status = queue.wait({q}) "
    "queue.push(out, tostring(status)) return 0", 0);
  dv_waitset ws;
  uint8_t buf[32];
  size_t n = 0;
  if (inst == NULL) { ok(0, "load"); return; }
  /* No park at all: a queue that can never deliver fires immediately, so the
     program is finished by the time 'dv_run' returns and the host is never asked
     to answer anything. */
  eq_st(dv_run(inst, &ws), DV_DONE,
        "a wait on a queue that can never deliver finishes without parking");
  dv_queue_pop(inst, dv_queue_lookup(inst, "res"), buf, sizeof(buf), &n);
  ok(n >= 7 && memcmp(buf + 1, "closed", 6) == 0,
     "and the program is told the queue is gone");
  if (!(n >= 7 && memcmp(buf + 1, "closed", 6) == 0))
    printf("      (got %.*s)\n", (int)(n > 0 ? n - 1 : 0), (const char *)buf + 1);
  dv_free(inst);
}


/*
** Naming a live, empty queue does nothing at all.
**
** It used to synthesise a timeout, and that was a lie the program could not
** detect: 6.3 defines "timeout" as 'queue.wait' having elapsed, and this program
** passes no timeout, so it would be told one elapsed and then index the nil it was
** handed. Passing 0 is the only way to say the timeout elapsed.
*/
static void a_spurious_resume_invents_nothing (void) {
  dv_instance *inst = load(
    "local q = queue.declare('c') "
    "local out = queue.declare('res', {exported = true}) "
    "local id, v, status = queue.wait({q}) "        /* no timeout */
    "queue.push(out, tostring(status) .. '/' .. tostring(v)) return 0", 0);
  dv_waitset ws;
  uint8_t buf[64];
  size_t n = 0;
  if (inst == NULL) { ok(0, "load"); return; }
  eq_st(dv_run(inst, &ws), DV_IDLE, "it parks with no timeout");
  eq_st(dv_resume(inst, ws.ids[0]), DV_IDLE,
        "naming a live empty queue leaves it parked rather than resuming it");
  ok(dv_waitset_get(inst, &ws) == DV_OK && ws.n == 1,
     "and it is still waiting on the same handle, so the host may retry");
  /* A real message then arrives and is delivered normally, which is the proof that
     the no-op did not consume the park. */
  dv_queue_push(inst, ws.ids[0], (const uint8_t *)"\xa2hi", 3);
  eq_st(dv_resume(inst, ws.ids[0]), DV_DONE, "a real message resumes it");
  dv_queue_pop(inst, dv_queue_lookup(inst, "res"), buf, sizeof(buf), &n);
  {
    char text[64];
    const char *msg = mp_str(buf, n, text, sizeof(text));
    ok(strstr(msg, "ok/hi") != NULL,
       "and the program sees the message, never a phantom timeout");
    if (strstr(msg, "ok/hi") == NULL) printf("      (got %s)\n", msg);
  }
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


/* ----------------------------------------------------------- endpoints -- */

/* The host's idea of what a reference means. Deliberately trivial: the runtime
   carries the bytes and never reads them, so a host is free to make them an
   index, an address, or a name. Here they are just a token in decimal. */
static int bind_ref (void *ud, const uint8_t *ref, size_t len,
                     uint32_t *token) {
  char buf[16];
  size_t i;
  (void)ud;
  if (len == 0 || len >= sizeof(buf)) return 0;
  for (i = 0; i < len; i++) buf[i] = (char)ref[i];
  buf[len] = '\0';
  if (buf[0] == 'x') return 0;   /* a reference the host refuses */
  *token = (uint32_t)atoi(buf);
  return (*token != 0);
}


/* msgpack fixext1 with code 0x02 and one payload byte: an endpoint reference as
   it arrives in a message. */
static void ref_message (uint8_t *out, char c) {
  out[0] = 0xd4;
  out[1] = 0x02;
  out[2] = (uint8_t)c;
}


static void endpoints (void) {
  dv_instance *inst = load(
    "local inb = queue.lookup('inbox') "
    "local _, ref = queue.wait({inb}) "
    "local ep = endpoint.bind(ref, 'peer') "
    "local out = queue.declare('log', {exported = true}) "
    "queue.push(out, endpoint.status(ep)) "
    "queue.push(out, select(2, queue.push(ep, 'one'))) "
    "local _, dead = queue.wait({inb}) "
    "queue.push(out, endpoint.status(ep)) "
    "queue.push(out, select(2, queue.push(ep, 'two'))) "
    "return 0", 0);
  dv_waitset ws;
  dv_queue_id inbox, log, epq;
  uint8_t ref[3], buf[64];
  size_t n;
  if (inst == NULL) { ok(0, "load"); return; }
  dv_set_endpoint_handler(inst, bind_ref, NULL);
  inbox = dv_queue_lookup(inst, "inbox");

  eq_st(dv_run(inst, &ws), DV_IDLE, "the program waits for a reference");
  ref_message(ref, '7');
  dv_queue_push(inst, inbox, ref, sizeof(ref));
  eq_st(dv_resume(inst, inbox), DV_IDLE, "it binds and carries on");

  epq = dv_endpoint_queue(inst, 7);
  ok(epq != 0, "the host can find the queue its token was bound to");

  log = dv_queue_lookup(inst, "log");
  dv_queue_pop(inst, log, buf, sizeof(buf), &n);
  ok(n == 5 && memcmp(buf + 1, "live", 4) == 0, "a fresh endpoint is live");
  dv_queue_pop(inst, log, buf, sizeof(buf), &n);
  ok(n == 3 && memcmp(buf + 1, "ok", 2) == 0,
     "and a push to it is accepted into the next hop");

  /* The message really is sitting in a bounded local buffer for the host. */
  eq_st(dv_queue_pop(inst, epq, buf, sizeof(buf), &n), DV_OK,
        "the host drains the endpoint like any other queue");
  ok(n == 4 && memcmp(buf + 1, "one", 3) == 0, "getting what was pushed");

  /* Now the far end dies. */
  eq_st(dv_endpoint_close(inst, epq), DV_OK, "the host closes the far end");
  ref_message(ref, '7');
  dv_queue_push(inst, inbox, ref, sizeof(ref));
  eq_st(dv_resume(inst, inbox), DV_DONE, "the program runs to the end");
  dv_queue_pop(inst, log, buf, sizeof(buf), &n);
  ok(n == 5 && memcmp(buf + 1, "gone", 4) == 0, "status is now gone");
  dv_queue_pop(inst, log, buf, sizeof(buf), &n);
  ok(n == 5 && memcmp(buf + 1, "gone", 4) == 0,
     "and a push answers gone, immediately and without raising");

  /* A host push to a closed endpoint gets the same answer. */
  eq_st(dv_queue_push(inst, epq, MP_ONE, 1), DV_QUEUE_GONE,
        "so does a push from the host side");
  dv_free(inst);
}


static void endpoint_refusals (void) {
  dv_instance *inst = load(
    "local inb = queue.lookup('inbox') "
    "local _, ref = queue.wait({inb}) "
    "local out = queue.declare('log', {exported = true}) "
    "local okk, err = pcall(endpoint.bind, ref, 'nope') "
    "queue.push(out, okk) "
    "queue.push(out, tostring(err):match('refused') ~= nil) "
    "return 0", 0);
  dv_waitset ws;
  dv_queue_id inbox, log;
  uint8_t ref[3], buf[64];
  size_t n;
  if (inst == NULL) { ok(0, "load"); return; }
  dv_set_endpoint_handler(inst, bind_ref, NULL);
  inbox = dv_queue_lookup(inst, "inbox");
  dv_run(inst, &ws);
  ref_message(ref, 'x');   /* the host refuses this one */
  dv_queue_push(inst, inbox, ref, sizeof(ref));
  eq_st(dv_resume(inst, inbox), DV_DONE, "the program handles a refusal");
  log = dv_queue_lookup(inst, "log");
  dv_queue_pop(inst, log, buf, sizeof(buf), &n);
  eq_i(buf[0], 0xc2, "a refused bind raises rather than returning a dead handle");
  dv_queue_pop(inst, log, buf, sizeof(buf), &n);
  eq_i(buf[0], 0xc3, "and says the host refused it");
  dv_free(inst);
}


/*
** The milestone's real acceptance criterion: a relay agent forwarding between
** two instances, with no runtime support for routing.
**
** Nothing in the runtime knows that B exists, that A's endpoint leads to it, or
** that R is relaying. A pushes to an endpoint; the host moves bytes; R is an
** ordinary program holding two handles. Every piece of the routing decision is
** either in a program or in the host, which is the whole argument of 7.4 -- and
** the reason none of it is in C.
*/
/*
** The callback-free path, which is the one a wasm host has to use: a C function
** pointer there is a function-table index, not something a host can hand over.
** Found while writing the wasmtime binding.
*/
static void endpoint_preauthorised (void) {
  dv_instance *inst = load(
    "local inb = queue.lookup('inbox') "
    "local _, ref = queue.wait({inb}) "
    "local ep = endpoint.bind(ref, 'peer') "
    "local out = queue.declare('log', {exported = true}) "
    "queue.push(out, endpoint.status(ep)) "
    "return 0", 0);
  dv_waitset ws;
  dv_queue_id inbox, log;
  uint8_t ref[3], buf[64];
  size_t n;
  if (inst == NULL) { ok(0, "load"); return; }
  /* No handler installed at all -- only a pre-authorised reference. */
  dv_endpoint_allow(inst, (const uint8_t *)"9", 1, 42);
  inbox = dv_queue_lookup(inst, "inbox");
  dv_run(inst, &ws);
  ref_message(ref, '9');
  dv_queue_push(inst, inbox, ref, sizeof(ref));
  eq_st(dv_resume(inst, inbox), DV_DONE,
        "a pre-authorised reference binds with no callback");
  ok(dv_endpoint_queue(inst, 42) != 0, "under the token the host chose");
  log = dv_queue_lookup(inst, "log");
  dv_queue_pop(inst, log, buf, sizeof(buf), &n);
  ok(n == 5 && memcmp(buf + 1, "live", 4) == 0, "and it is live");
  dv_free(inst);
}


/*
** Bind, destroy, bind again. Audit finding 11.
**
** Not on any profile's path, and here rather than under one because it is
** reachable by accident: 'endpoint.bind' and 'queue.destroy' are both in the
** guest table, and a program that finishes with a peer and tidies up is doing
** the ordinary thing. The token-to-handle map had no way for an entry to leave
** it, so the second bind returned the destroyed handle and said it had
** succeeded; every push through it raised, and the token stayed unusable for the
** life of the instance. The host saw it too -- 'dv_endpoint_queue' went on
** naming a dead handle as the buffer to drain.
**
** The program pushes through the rebound handle rather than only inspecting it,
** because "bind returned something" was true of the broken version as well.
*/
static void a_destroyed_endpoint_can_be_bound_again (void) {
  dv_instance *inst = load(
    "local inb = queue.lookup('inbox') "
    "local out = queue.declare('log', {exported = true}) "
    "local _, ref = queue.wait({inb}) "
    "local first = endpoint.bind(ref, 'peer') "
    "queue.destroy(first) "
    "local ok, second = pcall(endpoint.bind, ref, 'peer') "
    "if not ok then queue.push(out, 'rebind failed: ' .. tostring(second)) "
    "  return 0 end "
    "local okp, err = pcall(queue.push, second, 'through the new handle') "
    "queue.push(out, tostring(okp) .. '|' .. tostring(second ~= first) "
    "  .. '|' .. tostring(err)) "
    "return 0", 0);
  dv_waitset ws;
  dv_queue_id inbox, log;
  uint8_t ref[3], buf[128];
  size_t n = 0;
  if (inst == NULL) { ok(0, "load"); return; }
  dv_endpoint_allow(inst, (const uint8_t *)"9", 1, 42);
  inbox = dv_queue_lookup(inst, "inbox");
  memset(&ws, 0, sizeof(ws));
  dv_run(inst, &ws);
  ref_message(ref, '9');
  dv_queue_push(inst, inbox, ref, sizeof(ref));
  eq_st(dv_resume(inst, inbox), DV_DONE,
        "a program can destroy an endpoint queue and bind the token again");
  log = dv_queue_lookup(inst, "log");
  if (dv_queue_pop(inst, log, buf, sizeof(buf), &n) == DV_OK && n > 1) {
    char text[128];
    const char *msg = mp_str(buf, n, text, sizeof(text));
    ok(strstr(msg, "true|true|") == msg,
       "and the handle it gets back is a new, live one it can push through");
    if (strstr(msg, "true|true|") != msg) printf("      (%s)\n", msg);
  }
  else ok(0, "and the handle it gets back is a new, live one it can push through");
  /* And the host is told the truth about which buffer to drain. */
  ok(dv_endpoint_queue(inst, 42) != 0,
     "and the host's endpoint handle names the live queue, not the dead one");
  dv_free(inst);
}


/*
** A real reference, and a host that binds nothing.
**
** This lives here rather than in test_endpoint.lua because it needs a genuine
** reference to get as far as the message, and the only way to make one from Lua
** was the forgery that 'msgpack.decode' used to allow. Keeping that open in order
** to test the error message behind it would have been testing through the hole.
**
** The reference is real: the host pushes ext 0x02 bytes into the inbox, delivery
** resolves them because the bytes came from outside the guest, and the program
** binds what it received. What is missing is any binding for it -- no
** 'dv_endpoint_allow' and no handler -- which is the case a host hits on its first
** run, so the message it gets should say what is actually wrong.
*/
static void endpoint_with_no_host_binding (void) {
  dv_instance *inst = load(
    "local inb = queue.lookup('inbox') "
    "local _, ref = queue.wait({inb}) "
    "local out = queue.declare('log', {exported = true}) "
    "local ok, err = pcall(endpoint.bind, ref, 'peer') "
    "queue.push(out, tostring(ok) .. '|' .. tostring(err)) "
    "return 0", 0);
  dv_waitset ws;
  dv_queue_id inbox, log;
  uint8_t ref[3], buf[256];
  size_t n = 0;
  if (inst == NULL) { ok(0, "load"); return; }
  /* Deliberately nothing: no allow, no handler. */
  inbox = dv_queue_lookup(inst, "inbox");
  dv_run(inst, &ws);
  ref_message(ref, 'Z');
  dv_queue_push(inst, inbox, ref, sizeof(ref));
  eq_st(dv_resume(inst, inbox), DV_DONE, "the program runs to completion");
  log = dv_queue_lookup(inst, "log");
  if (dv_queue_pop(inst, log, buf, sizeof(buf), &n) == DV_OK && n > 1) {
    char text[256];
    const char *msg = mp_str(buf, n, text, sizeof(text));
    ok(strstr(msg, "false|") == msg, "binding a real reference fails");
    ok(strstr(msg, "binds no endpoints") != NULL,
       "and says this host binds no endpoints, rather than something vaguer");
    if (strstr(msg, "binds no endpoints") == NULL)
      printf("      (%s)\n", msg);
  }
  else {
    ok(0, "binding a real reference fails");
    ok(0, "and says this host binds no endpoints, rather than something vaguer");
  }
  dv_free(inst);
}


/*
** The forgery that 'msgpack.decode' used to permit, asserted from the host's side.
**
** 7.3 says a reference "cannot be forged" and that a program "receives one in a
** message and never builds one". That was untrue: the resolver ran on any bytes
** handed to guest-callable 'msgpack.decode', so a program could mint a reference
** to any pre-authorised peer by naming it. This is that attack, and it must fail.
*/
static void a_guest_cannot_mint_a_reference (void) {
  dv_instance *inst = load(
    "local out = queue.declare('log', {exported = true}) "
    "local forged = msgpack.decode('\\xd4\\x029') "
    "local ok, err = pcall(endpoint.bind, forged, 'peer') "
    "queue.push(out, tostring(ok) .. '|' .. tostring(err)) "
    "return 0", 0);
  dv_waitset ws;
  uint8_t buf[256];
  size_t n = 0;
  if (inst == NULL) { ok(0, "load"); return; }
  /* The host authorises one peer and hands the program nothing. */
  dv_endpoint_allow(inst, (const uint8_t *)"9", 1, 42);
  memset(&ws, 0, sizeof(ws));
  dv_run(inst, &ws);
  if (dv_queue_pop(inst, dv_queue_lookup(inst, "log"), buf, sizeof(buf), &n)
      == DV_OK && n > 1) {
    char text[256];
    const char *msg = mp_str(buf, n, text, sizeof(text));
    ok(strstr(msg, "false|") == msg,
       "a guest cannot bind a reference it decoded itself");
    ok(strstr(msg, "never builds one") != NULL,
       "and the refusal names 7.3's rule");
    if (strstr(msg, "never builds one") == NULL)
      printf("      (%s)\n", msg);
  }
  else {
    ok(0, "a guest cannot bind a reference it decoded itself");
    ok(0, "and the refusal names 7.3's rule");
  }
  /* The decisive check: nothing was bound, so the authorised peer is untouched. */
  ok(dv_endpoint_queue(inst, 42) == 0,
     "and no queue exists for the peer it tried to reach");
  dv_free(inst);
}


/*
** A reference forwarded inside a message, which is 9.1's router.
**
** 7.3's shape is "a program receives an endpoint reference in a message", and the
** encode side of that had never been exercised: a reference is a table with a
** private metatable, so it went onto the wire as a plain one-element array. The
** bytes still arrived, they just arrived as data -- so 'endpoint.bind' on the far
** side refused them, and a router could use an endpoint but never hand one on.
** That is not an exotic case; it is what a coordinator handing work to a swarm of
** handlers does.
**
** Two hops, both through the host, because that is the only path there is: the
** router's bytes leave through an exported queue and the host pushes them into
** the handler. Nothing in the runtime knows a router exists.
*/
static void a_reference_survives_being_forwarded (void) {
  /* Receives a destination and passes it on, with a job attached. */
  static const char *ROUTER =
    "local inb = queue.lookup('inbox') "
    "local out = queue.declare('assign', {capacity = 4, exported = true}) "
    "local _, ref = queue.wait({inb}) "
    "queue.push(out, {to = ref, job = 'fetch'}) "
    "return 0";
  /* Never told where its work goes: it binds what it was handed. */
  static const char *HANDLER =
    "local inb = queue.lookup('inbox') "
    "local _, m = queue.wait({inb}) "
    "local dest = endpoint.bind(m.to, 'dest') "
    "queue.push(dest, m.job .. '-done') "
    "return 0";
  dv_instance *r = load(ROUTER, 0);
  dv_instance *h = load(HANDLER, 0);
  dv_waitset ws;
  uint8_t ref[3], buf[256];
  size_t n = 0;
  dv_queue_id assign, h_in, h_ep;
  int has_ext = 0;
  if (r == NULL || h == NULL) { ok(0, "the router and the handler load"); return; }
  dv_set_endpoint_handler(h, bind_ref, NULL);

  memset(&ws, 0, sizeof(ws));
  dv_run(r, &ws);
  dv_run(h, &ws);
  ref_message(ref, '5');
  dv_queue_push(r, dv_queue_lookup(r, "inbox"), ref, sizeof(ref));
  dv_resume(r, dv_queue_lookup(r, "inbox"));

  assign = dv_queue_lookup(r, "assign");
  if (dv_queue_pop(r, assign, buf, sizeof(buf), &n) != DV_OK) {
    ok(0, "the router emits an assignment");
    ok(0, "the assignment carries an ext 0x02 rather than an array");
    ok(0, "the handler binds the destination it was handed");
    dv_free(r); dv_free(h);
    return;
  }
  ok(1, "the router emits an assignment");
  /* Asserted on the wire, not only through the far side, because this is the
     byte that was wrong: fixext1 is 0xd4 then the code. */
  {
    size_t i;
    for (i = 0; i + 1 < n; i++) {
      if (buf[i] == 0xd4 && buf[i + 1] == 0x02) { has_ext = 1; break; }
    }
  }
  ok(has_ext, "the assignment carries an ext 0x02 rather than an array");

  /* The host's whole job again: move the bytes. It does not look inside. */
  h_in = dv_queue_lookup(h, "inbox");
  dv_queue_push(h, h_in, buf, n);
  dv_resume(h, h_in);

  h_ep = dv_endpoint_queue(h, 5);
  ok(h_ep != 0, "the handler binds the destination it was handed");
  if (h_ep != 0 &&
      dv_queue_pop(h, h_ep, buf, sizeof(buf), &n) == DV_OK && n > 1) {
    char text[64];
    const char *msg = mp_str(buf, n, text, sizeof(text));
    ok(strcmp(msg, "fetch-done") == 0,
       "and its work reaches the far end the router chose");
    if (strcmp(msg, "fetch-done") != 0)
      printf("      (%s)\n", msg);
  }
  else {
    ok(0, "and its work reaches the far end the router chose");
  }
  dv_free(r);
  dv_free(h);
}


/*
** The forwarding above must not become a way to mint a reference.
**
** 'a_guest_cannot_mint_a_reference' checks the decode side; this checks that the
** encode side did not open a second door. Three ways a guest might write the bytes
** itself, and all three must fail before they become bytes:
**
**   'msgpack.ext(0x02, ...)' -- refused, because 5.5 reserves every code below
**   0x10 and leaves 0x10..0x7F for application use.
**
**   'msgpack.ext(0x10, ...)' and then assigning 0x02 over its code -- which
**   worked, and is the defect 18.1 records as "a guest can mint reserved ext
**   codes". The wrapper is an ordinary table; a constructor cannot vouch for a
**   value that stays mutable, so the encoder checks the code as it writes it.
**
**   an ordinary table with a lookalike metatable -- offered to the resolver now,
**   which is the new path, and refused because the resolver compares against a
**   metatable held in the registry that no guest can name.
*/
static void a_forged_ext_is_not_a_reference (void) {
  dv_instance *inst = load(
    "local out = queue.declare('log', {exported = true}) "
    "local ok1 = pcall(msgpack.ext, 0x02, '9') "
    /* The wrapper is an ordinary table, so the constructor's refusal was only as
       good as the value staying as it was made. It was not. */
    "local w = msgpack.ext(0x10, '9') w[3] = 0x02 "
    "local ok3, err3 = pcall(msgpack.encode, w) "
    /* A look-alike: the reference's shape, with a metatable of the guest's own.
       It must encode as an ordinary array -- no 0xd4 0x02 anywhere in it. */
    "local fake = setmetatable({'9'}, {__name = 'diluvium.endpoint.ref'}) "
    "local bytes = msgpack.encode(fake) "
    "local ok2 = pcall(endpoint.bind, msgpack.decode(bytes), 'p') "
    "local m3 = tostring(err3):match('application range') or 'unnamed' "
    "queue.push(out, tostring(ok1) .. '|' .. tostring(ok2) .. '|' .. "
    "  tostring(bytes:find('\\xd4\\x02', 1, true) ~= nil) .. '|' .. "
    "  tostring(ok3) .. '|' .. m3) "
    "return 0", 0);
  dv_waitset ws;
  uint8_t buf[256];
  size_t n = 0;
  if (inst == NULL) { ok(0, "load"); return; }
  dv_endpoint_allow(inst, (const uint8_t *)"9", 1, 91);
  memset(&ws, 0, sizeof(ws));
  dv_run(inst, &ws);
  if (dv_queue_pop(inst, dv_queue_lookup(inst, "log"), buf, sizeof(buf), &n)
      == DV_OK && n > 1) {
    char text[64];
    const char *msg = mp_str(buf, n, text, sizeof(text));
    ok(strcmp(msg, "false|false|false|false|application range") == 0,
       "a guest can neither write an ext 0x02 nor fake a reference into one");
    if (strcmp(msg, "false|false|false|false|application range") != 0)
      printf("      (%s)\n", msg);
  }
  else {
    ok(0, "a guest can neither write an ext 0x02 nor fake a reference into one");
  }
  ok(dv_endpoint_queue(inst, 91) == 0,
     "and the authorised peer has no queue");
  dv_free(inst);
}


/*
** The laundering route, which the encode-side code check closes.
**
** 18.1 records two ways a reference could still be forged. This is the first: a
** guest decodes '\xd4\x02' plus a name, which is inert in its own state, and then
** *pushes it onto a queue*. The delivery path is trusted and decodes it again, so
** the far side -- or the same program on the next hop -- would receive a genuine
** reference to a peer nobody handed it.
**
** It is closed at the narrowest point, which is the encoder: an opaque ext value
** carrying a reserved code cannot be written back onto the wire at all. So the
** bytes never leave the state that made them up.
**
** The second route -- reaching the reference metatable through the 'debug' library
** -- is not closed, and 18.2 makes restricting that library a profile B
** requirement rather than something this asserts.
*/
static void the_laundering_route_is_closed (void) {
  dv_instance *inst = load(
    "local out = queue.declare('log', {capacity = 4, exported = true}) "
    "local inert = msgpack.decode('\\xd4\\x029') "
    "local ok, err = pcall(queue.push, out, inert) "
    "queue.push(out, tostring(ok) .. '|' .. "
    "  (tostring(err):match('application range') or 'unnamed')) "
    "return 0", 0);
  dv_waitset ws;
  uint8_t buf[256];
  size_t n = 0;
  if (inst == NULL) { ok(0, "load"); return; }
  memset(&ws, 0, sizeof(ws));
  dv_run(inst, &ws);
  if (dv_queue_pop(inst, dv_queue_lookup(inst, "log"), buf, sizeof(buf), &n)
      == DV_OK && n > 1) {
    char text[64];
    const char *msg = mp_str(buf, n, text, sizeof(text));
    ok(strcmp(msg, "false|application range") == 0,
       "a decoded reference cannot be pushed back onto a queue");
    if (strcmp(msg, "false|application range") != 0)
      printf("      (%s)\n", msg);
  }
  else {
    ok(0, "a decoded reference cannot be pushed back onto a queue");
  }
  dv_free(inst);
}


static void relay_between_instances (void) {
  static const char *SENDER =
    "local ref_msg = select(2, queue.wait({queue.lookup('inbox')})) "
    "local up = endpoint.bind(ref_msg, 'upstream') "
    "for i = 1, 3 do queue.push(up, {n = i}) end "
    "return 0";
  /* The relay does not know where its output goes either: it holds an endpoint
     it was handed and forwards, which is 7.4's store-and-forward in five lines
     of Lua and no C. */
  static const char *RELAY =
    "local inb = queue.lookup('inbox') "
    "local ref_msg = select(2, queue.wait({inb})) "
    "local out = endpoint.bind(ref_msg, 'downstream') "
    "local seen = 0 "
    "while seen < 3 do "
    "  local _, m = queue.wait({inb}) "
    "  seen = seen + 1 "
    "  queue.push(out, {n = m.n, hops = (m.hops or 0) + 1}) "
    "end return 0";
  static const char *RECEIVER =
    "local inb = queue.lookup('inbox') "
    "local out = queue.declare('final', {capacity = 8, exported = true}) "
    "for _ = 1, 3 do "
    "  local _, m = queue.wait({inb}) "
    "  queue.push(out, m.n * 10 + m.hops) "
    "end return 0";

  dv_instance *a = load(SENDER, 0);
  dv_instance *r = load(RELAY, 0);
  dv_instance *b = load(RECEIVER, 0);
  dv_waitset ws;
  uint8_t ref[3], buf[64];
  size_t n;
  int moved = 0, guard = 0;
  dv_queue_id a_ep, r_ep, a_in, r_in, b_in, final;
  if (a == NULL || r == NULL || b == NULL) { ok(0, "the three programs load"); return; }
  dv_set_endpoint_handler(a, bind_ref, NULL);
  dv_set_endpoint_handler(r, bind_ref, NULL);

  a_in = dv_queue_lookup(a, "inbox");
  r_in = dv_queue_lookup(r, "inbox");
  b_in = dv_queue_lookup(b, "inbox");

  /* Start all three; each parks waiting for its reference or its work. */
  eq_st(dv_run(a, &ws), DV_IDLE, "the sender waits for an endpoint");
  eq_st(dv_run(r, &ws), DV_IDLE, "so does the relay");
  eq_st(dv_run(b, &ws), DV_IDLE, "and the receiver waits for work");

  /* Hand each its reference. The host chose the tokens; the guests never see
     what they mean. */
  ref_message(ref, '1');
  dv_queue_push(a, a_in, ref, sizeof(ref));
  dv_resume(a, a_in);
  ref_message(ref, '2');
  dv_queue_push(r, r_in, ref, sizeof(ref));
  dv_resume(r, r_in);

  a_ep = dv_endpoint_queue(a, 1);
  r_ep = dv_endpoint_queue(r, 2);
  ok(a_ep != 0 && r_ep != 0, "both bound their endpoints");

  /* The host's whole job: move bytes from an endpoint buffer to an inbox. It
     knows nothing about what the messages mean. */
  while (moved < 3 && guard++ < 40) {
    if (dv_queue_pop(a, a_ep, buf, sizeof(buf), &n) == DV_OK) {
      dv_queue_push(r, r_in, buf, n);
      dv_resume(r, r_in);
    }
    if (dv_queue_pop(r, r_ep, buf, sizeof(buf), &n) == DV_OK) {
      dv_queue_push(b, b_in, buf, n);
      dv_resume(b, b_in);
      moved++;
    }
  }
  eq_i(moved, 3, "three messages crossed two instance boundaries");

  final = dv_queue_lookup(b, "final");
  ok(final != 0, "the receiver's output queue exists");
  {
    int i, values[3] = {0, 0, 0};
    for (i = 0; i < 3; i++) {
      if (dv_queue_pop(b, final, buf, sizeof(buf), &n) == DV_OK)
        values[i] = buf[0];
    }
    /* n*10 + hops, so 11, 21, 31: the hop count proves each message really
       went through the relay rather than straight across. */
    eq_i(values[0], 11, "the first arrived, having been relayed once");
    eq_i(values[1], 21, "the second");
    eq_i(values[2], 31, "the third");
  }
  dv_free(a);
  dv_free(r);
  dv_free(b);
}




/* ======================================================================
** Hibernate and wake (10.1, 10.6, 10.10)
** ====================================================================== */

/* Park an instance on a queue and leave it parked. 0 on failure. */
static int park_on_queue (dv_instance *inst, const char *src) {
  dv_waitset ws;
  dv_status st;
  if (dv_load(inst, (const uint8_t *)src, strlen(src), "=agent") != DV_OK) {
    printf("[FAIL] agent would not load: %s\n", dv_last_error(inst));
    failures++; checks++;
    return 0;
  }
  memset(&ws, 0, sizeof(ws));
  st = dv_run(inst, &ws);
  if (st != DV_IDLE) {
    printf("[FAIL] the agent did not park (status %s: %s)\n",
           dv_status_name(st), dv_last_error(inst));
    failures++; checks++;
    return 0;
  }
  return 1;
}


static void a_parked_instance_snapshots_and_wakes (void) {
  static const char *src =
    "local work = queue.declare('work', {cap = 4})\n"
    "local log = queue.declare('log', {cap = 4})\n"
    "local seen = 0\n"
    "queue.push(log, 'before')\n"
    "local id, v = queue.wait({work})\n"
    "seen = seen + 1\n"
    "queue.push(log, 'woke:' .. tostring(v) .. ':' .. seen)\n";
  dv_instance *a = dv_new(NULL);
  dv_instance *b;
  uint8_t *buf = NULL;
  size_t need = 0, got = 0;
  dv_status st;
  if (a == NULL) { ok(0, "an instance is created"); return; }
  if (!park_on_queue(a, src)) { dv_free(a); return; }

  /* A size enquiry first, which is how a host that has to allocate finds out. */
  st = dv_snapshot(a, NULL, NULL, 0, &need);
  ok(st == DV_OK && need > 0, "a parked instance reports its snapshot size");
  if (st != DV_OK) {
    printf("      (%s: %s)\n", dv_status_name(st), dv_last_error(a));
    dv_free(a);
    return;
  }
  /* And a short buffer is reported rather than overrun. */
  {
    uint8_t small[8];
    size_t n = 0;
    ok(dv_snapshot(a, NULL, small, sizeof(small), &n) == DV_BUFFER_TOO_SMALL &&
       n == need, "a short buffer is refused and the needed size reported");
  }
  buf = (uint8_t *)malloc(need);
  if (buf == NULL) { ok(0, "room for the snapshot"); dv_free(a); return; }
  ok(dv_snapshot(a, NULL, buf, need, &got) == DV_OK && got == need,
     "the snapshot is written");
  printf("      (%lu bytes)\n", (unsigned long)got);

  /*
  ** The original is deliberately freed before the restore. A snapshot that only
  ** works while its source is alive would be no use for hibernation, and sharing
  ** a Lua state between the two would hide exactly that.
  */
  dv_free(a);

  b = dv_new(NULL);
  if (b == NULL) { ok(0, "a fresh instance"); free(buf); return; }
  st = dv_restore(b, NULL, buf, got);
  ok(st == DV_OK, "it restores into a fresh instance");
  if (st != DV_OK) {
    printf("      (%s: %s)\n", dv_status_name(st), dv_last_error(b));
    dv_free(b); free(buf);
    return;
  }
  /* The queues came with it, contents and all -- 'log' already holds a message
     the program pushed before it parked. */
  {
    dv_queue_info info;
    dv_queue_id log = 0, work = 0;
    memset(&info, 0, sizeof(info));
    log = dv_queue_lookup(b, "log");
    work = dv_queue_lookup(b, "work");
    ok(log != 0 && work != 0, "both queues are there, found by name");
    ok(dv_queue_state(b, log, &info) == DV_OK && info.len == 1,
       "and 'log' still holds what the program pushed before parking");
  }
  /* The wait-set survives, which is what a host asks for first. */
  {
    dv_waitset ws;
    memset(&ws, 0, sizeof(ws));
    ok(dv_waitset_get(b, &ws) == DV_OK && ws.n == 1,
       "the restored instance reports what it is waiting for");
    ok(ws.n == 1 && ws.ids[0] == dv_queue_lookup(b, "work"),
       "and it is the queue it parked on");
  }
  /* And it wakes: push a message, resume, and the program runs on. */
  {
    static const uint8_t msg[] = { 0xa5, 'h', 'e', 'l', 'l', 'o' };
    dv_queue_id work = dv_queue_lookup(b, "work");
    dv_queue_id log = dv_queue_lookup(b, "log");
    ok(dv_queue_push(b, work, msg, sizeof(msg)) == DV_OK,
       "a message can be pushed to the restored queue");
    st = dv_resume(b, work);
    ok(st == DV_DONE, "and the restored program runs to completion");
    if (st != DV_DONE)
      printf("      (%s: %s)\n", dv_status_name(st), dv_last_error(b));
    {
      dv_queue_info info;
      memset(&info, 0, sizeof(info));
      ok(dv_queue_state(b, log, &info) == DV_OK && info.len == 2,
         "having pushed its second log line");
      /* The value proves the locals survived: 'seen' was 0 at the snapshot and
         the program incremented it after waking. */
      {
        uint8_t out[64];
        size_t n = 0;
        dv_queue_pop(b, log, out, sizeof(out), &n);      /* 'before' */
        n = 0;
        if (dv_queue_pop(b, log, out, sizeof(out), &n) == DV_OK && n > 8) {
          /* msgpack str: skip the one-byte header for a short string. */
          ok(memcmp(out + 1, "woke:hello:1", 12) == 0,
             "and the line says the message and the local it kept");
          if (memcmp(out + 1, "woke:hello:1", 12) != 0)
            printf("      (got %.*s)\n", (int)n - 1, (const char *)out + 1);
        }
        else
          ok(0, "and the line says the message and the local it kept");
      }
    }
  }
  dv_free(b);
  free(buf);
}


static void an_unparked_instance_refuses_to_snapshot (void) {
  dv_instance *inst = dv_new(NULL);
  size_t n = 0;
  if (inst == NULL) { ok(0, "an instance"); return; }
  ok(dv_snapshot(inst, NULL, NULL, 0, &n) == DV_ERROR,
     "an instance that has not run refuses to snapshot");
  {
    static const char *src = "return 1";
    dv_waitset ws;
    memset(&ws, 0, sizeof(ws));
    dv_load(inst, (const uint8_t *)src, strlen(src), "=done");
    dv_run(inst, &ws);
    ok(dv_snapshot(inst, NULL, NULL, 0, &n) == DV_ERROR,
       "and so does one that has finished");
  }
  dv_free(inst);
}


static void a_used_instance_refuses_to_restore (void) {
  static const char *src = "local q = queue.declare('sq', {cap = 2}) "
                           "return queue.wait({q})";
  dv_instance *a = dv_new(NULL);
  dv_instance *b = dv_new(NULL);
  uint8_t buf[8192];
  size_t n = 0;
  if (a == NULL || b == NULL) { ok(0, "two instances"); return; }
  if (!park_on_queue(a, src)) { dv_free(a); dv_free(b); return; }
  if (dv_snapshot(a, NULL, buf, sizeof(buf), &n) != DV_OK) {
    ok(0, "the snapshot is taken");
    dv_free(a); dv_free(b);
    return;
  }
  /* 'b' has a program of its own, so restoring into it would merge two handle
     spaces and hand one program the other's queues. */
  dv_load(b, (const uint8_t *)"return 1", 8, "=other");
  ok(dv_restore(b, NULL, buf, n) == DV_ERROR,
     "restoring into an instance that has already been used is refused");
  dv_free(a);
  dv_free(b);
}


static void the_host_stamp_is_enforced_through_the_abi (void) {
  static const char *src = "local q = queue.declare('sq', {cap = 2}) "
                           "return queue.wait({q})";
  dv_instance *a = dv_new(NULL);
  uint8_t buf[8192];
  size_t n = 0;
  if (a == NULL) { ok(0, "an instance"); return; }
  if (!park_on_queue(a, src)) { dv_free(a); return; }
  if (dv_snapshot(a, "host-alpha", buf, sizeof(buf), &n) != DV_OK) {
    printf("      (%s)\n", dv_last_error(a));
    ok(0, "a stamped snapshot is taken");
    dv_free(a);
    return;
  }
  dv_free(a);
  {
    dv_instance *b = dv_new(NULL);
    dv_instance *c = dv_new(NULL);
    dv_instance *d = dv_new(NULL);
    if (b == NULL || c == NULL || d == NULL) { ok(0, "three instances"); return; }
    ok(dv_restore(b, "host-alpha", buf, n) == DV_OK,
       "a stamped snapshot restores under its own host");
    ok(dv_restore(c, "host-beta", buf, n) == DV_SNAPSHOT_MISMATCH,
       "and is refused under another");
    ok(dv_restore(d, NULL, buf, n) == DV_SNAPSHOT_MISMATCH,
       "and by a host that expects no stamp at all");
    dv_free(b); dv_free(c); dv_free(d);
  }
}


static void garbage_is_refused_not_crashed_on (void) {
  static const char *cases[] = {
    "", "not a snapshot", "\xc1\xc1\xc1", "\x80", "\x81\xa1k\xc0"
  };
  size_t i;
  int all = 1;
  /* 10.10's floor: any byte string at all is a refusal. The structure-aware
     version of this is script/fuzz_snapshot.py, which mutates a real snapshot;
     these are the shapes a fuzzer takes a long time to reach by chance. */
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    dv_instance *inst = dv_new(NULL);
    if (inst == NULL) { all = 0; break; }
    if (dv_restore(inst, NULL, (const uint8_t *)cases[i],
                   strlen(cases[i])) == DV_OK)
      all = 0;
    dv_free(inst);
  }
  ok(all, "garbage is refused rather than restored");
}


static void a_registered_prototype_shrinks_a_snapshot (void) {
  /* The swarm case: many agents run the *same* chunk, and the host registers it
     once. The registered prototype has to be the very one the agents run -- the
     first version of this registered a different chunk that merely contained
     similar code, and the two snapshots came out the same size, which is what a
     hash reference not matching looks like. */
  static const char *src = "local q = queue.declare('agentq', {cap = 2})\n"
                           "local double = function(x) return x * 2 end\n"
                           "STASH = double\n"
                           "return queue.wait({q})\n";
  const char *lib = src;
  dv_instance *a = dv_new(NULL);
  dv_instance *b = dv_new(NULL);
  size_t plain = 0, shrunk = 0;
  if (a == NULL || b == NULL) { ok(0, "two instances"); return; }
  if (!park_on_queue(a, src)) { dv_free(a); dv_free(b); return; }
  dv_snapshot(a, NULL, NULL, 0, &plain);
  /* The same code registered up front, which is what a host does for a shared
     library a swarm of agents draws on. */
  ok(dv_register_code(b, (const uint8_t *)lib, strlen(lib), "=lib") == DV_OK,
     "a host can register a shared chunk");
  if (park_on_queue(b, src))
    dv_snapshot(b, NULL, NULL, 0, &shrunk);
  printf("      (unregistered %lu bytes, registered %lu bytes)\n",
         (unsigned long)plain, (unsigned long)shrunk);
  ok(shrunk > 0 && shrunk < plain,
     "and a snapshot then references the prototype instead of carrying it");
  dv_free(a);
  dv_free(b);
}




/* ======================================================================
** Budgets (9.4)
** ====================================================================== */

static void an_instruction_budget_aborts_a_runaway (void) {
  /* The case 9.4 exists for: a loop that never yields, which nothing cooperative
     can stop. The budget has to abort it, and abort is the word -- 9.4 forbids
     budgeting by yielding, because a yield from a hook leaves CIST_HOOKYIELD on
     the frame and 10.7 then refuses to hibernate the instance. */
  static const char *src = "local n = 0 while true do n = n + 1 end";
  dv_instance *inst = dv_new(NULL);
  dv_waitset ws;
  dv_status st;
  uint64_t used = 0;
  if (inst == NULL) { ok(0, "an instance"); return; }
  ok(dv_set_budget(inst, 200000, 0) == DV_OK, "an instruction budget is set");
  dv_load(inst, (const uint8_t *)src, strlen(src), "=runaway");
  memset(&ws, 0, sizeof(ws));
  st = dv_run(inst, &ws);
  ok(st == DV_ERROR, "a runaway loop stops with an error rather than hanging");
  ok(dv_exceeded(inst), "and the instance says it was the budget");
  dv_usage(inst, &used, NULL);
  ok(used >= 200000, "and reports what it spent");
  {
    const char *msg = dv_last_error(inst);
    ok(msg != NULL && strstr(msg, "budget") != NULL,
       "with a message naming the budget");
  }
  dv_free(inst);
}


static void a_budget_survives_a_guest_pcall (void) {
  /*
  ** The budget used to be switched off by its own first firing. The hook
  ** cleared itself before raising, 'luaL_error' raises an ordinary catchable
  ** error, and nothing re-arms the hook on a running instance -- so a guest
  ** that tripped the budget inside a 'pcall' ran unbounded from then on, for
  ** the life of the instance.
  **
  ** 30,000,000 instructions of work against a 1,000,000 budget: this returned
  ** DV_DONE before the fix, which is the whole bug in one assertion. The
  ** margin is deliberately wide so the case cannot pass by accident.
  */
  static const char *src =
    "pcall(function() while true do end end)\n"
    "local n = 0\n"
    "for i = 1, 30000000 do n = n + 1 end\n"
    "return n\n";
  dv_instance *inst = dv_new(NULL);
  dv_waitset ws;
  dv_status st;
  if (inst == NULL) { ok(0, "an instance"); return; }
  dv_set_budget(inst, 1000000, 0);
  dv_load(inst, (const uint8_t *)src, strlen(src), "=pcall-escape");
  memset(&ws, 0, sizeof(ws));
  st = dv_run(inst, &ws);
  ok(st == DV_ERROR,
     "a runaway caught by the guest's own pcall still stops the program");
  ok(dv_exceeded(inst), "and the instance is marked exceeded");
  dv_free(inst);
}


static void usage_keeps_counting_past_the_budget (void) {
  /*
  ** The other half, and the one a supervisor sees. With the hook cleared,
  ** 'insn_used' froze at the limit: an escaped instance reported itself
  ** sitting exactly at budget while it ran on, which is the healthiest
  ** possible reading. SPEC-level health checks measure saturation from
  ** 'dv_usage', so the escape blinded the instrument meant to catch it.
  **
  ** Asserting strictly greater than the limit is what distinguishes a hook
  ** that kept counting from one that stopped: the frozen value was exactly
  ** the limit.
  */
  static const char *src =
    "pcall(function() while true do end end)\n"
    "local n = 0\n"
    "for i = 1, 30000000 do n = n + 1 end\n";
  dv_instance *inst = dv_new(NULL);
  dv_waitset ws;
  uint64_t used = 0;
  if (inst == NULL) { ok(0, "an instance"); return; }
  dv_set_budget(inst, 1000000, 0);
  dv_load(inst, (const uint8_t *)src, strlen(src), "=usage-past-budget");
  memset(&ws, 0, sizeof(ws));
  dv_run(inst, &ws);
  dv_usage(inst, &used, NULL);
  ok(used > 1000000,
     "usage keeps counting past a budget the guest caught and tried to ignore");
  if (used <= 1000000)
    printf("      (used %lu, which is the frozen-at-the-limit reading)\n",
           (unsigned long)used);
  dv_free(inst);
}


static void a_budget_does_not_disturb_a_program_inside_it (void) {
  static const char *src = "local n = 0 for i = 1, 1000 do n = n + i end return n";
  dv_instance *inst = dv_new(NULL);
  dv_waitset ws;
  uint64_t used = 0;
  if (inst == NULL) { ok(0, "an instance"); return; }
  dv_set_budget(inst, 10000000, 0);
  dv_load(inst, (const uint8_t *)src, strlen(src), "=fine");
  memset(&ws, 0, sizeof(ws));
  ok(dv_run(inst, &ws) == DV_DONE, "a program inside its budget runs normally");
  ok(!dv_exceeded(inst), "and is not marked as exceeded");
  dv_usage(inst, &used, NULL);
  ok(used > 0, "and its usage is counted anyway");
  dv_free(inst);
}


static void a_memory_budget_refuses_an_allocation (void) {
  /* 9.4's memory mechanism is the allocator. Refusing is reported as an ordinary
     out-of-memory error, which is why this program can even catch it -- a limit
     rather than an execution. */
  static const char *src =
    "local ok, err = pcall(function()\n"
    "  local t = {}\n"
    "  for i = 1, 1e7 do t[i] = ('x'):rep(64) end\n"
    "end)\n"
    "return ok and 'no limit' or 'refused'\n";
  dv_instance *inst = dv_new(NULL);
  dv_waitset ws;
  uint64_t peak = 0;
  if (inst == NULL) { ok(0, "an instance"); return; }
  ok(dv_set_budget(inst, 0, 512) == DV_OK, "a memory budget is set");
  dv_load(inst, (const uint8_t *)src, strlen(src), "=hungry");
  memset(&ws, 0, sizeof(ws));
  if (dv_run(inst, &ws) == DV_DONE) {
    uint8_t out[64];
    size_t n = 0;
    dv_queue_id outbox = dv_queue_lookup(inst, "outbox");
    (void)outbox; (void)out; (void)n;
    ok(1, "the program finishes, having been refused the memory");
  }
  else
    ok(1, "the program stops, having been refused the memory");
  dv_usage(inst, NULL, &peak);
  ok(peak > 0 && peak <= 512 + 64,
     "and its peak stays inside the budget it was given");
  if (peak > 512 + 64)
    printf("      (peak %lu KB against a 512 KB budget)\n",
           (unsigned long)peak);
  dv_free(inst);
}


static void the_memory_counter_agrees_with_the_collector (void) {
  /*
  ** The counting allocator against a program that allocates *and frees*, rather
  ** than only allocating -- which every memory test in this file used to avoid, and
  ** which is what ordinary code does.
  **
  ** Lua hands the allocator a *type tag* in the 'osize' argument when 'ptr' is NULL
  ** (lmem.c, 'luaM_malloc_'). Counting it as a size undercharged every allocation
  ** by a handful of bytes and credited the full size back on the matching free, so
  ** the counter walked downward against the truth: 400k allocations and it read 216
  ** bytes where the collector read 63,549.
  **
  ** The ground truth is the collector's own accounting, which this counter is meant
  ** to shadow and which no part of this ABI feeds: 'collectgarbage("count")' is Lua
  ** counting the same bytes for its own reasons.
  **
  ** The program parks straight after reporting, so both figures are read at the
  ** same instant. Reading the counter after the program had *finished* compared two
  ** different moments and made the tolerance a guess.
  */
  static const char *src =
    "local out = queue.lookup('outbox')\n"    /* 6.6: reserved, already there */
    "local gate = queue.declare('gate', {capacity = 1})\n"
    /* Hold something, so there is a real figure to be wrong about, then let it go
       again -- the drift shows up sharpest against a small resting figure. */
    "local held = {}\n"
    "for i = 1, 20000 do held[i] = ('x'):rep(80) end\n"
    /* 400k short-lived strings. Each alloc/free pair is where the drift accrued. */
    "for r = 1, 4 do\n"
    "  for i = 1, 100000 do local s = ('y'):rep(80) end\n"
    "end\n"
    "held = nil\n"
    "collectgarbage('collect')\n"
    "collectgarbage('collect')\n"
    /* As a string, so the test needs no integer decoder: 'mp_str' above is the one
       reader this file has, and the figure is only printed and compared. */
    "queue.push(out, tostring(math.floor(collectgarbage('count') * 1024)))\n"
    "queue.wait({gate})\n";
  dv_instance *inst = dv_new(NULL);
  dv_waitset ws;
  uint64_t now = 0, peak = 0;
  uint64_t lua_says = 0;
  if (inst == NULL) { ok(0, "an instance"); return; }
  dv_load(inst, (const uint8_t *)src, strlen(src), "=churn");
  memset(&ws, 0, sizeof(ws));
  ok(dv_run(inst, &ws) == DV_IDLE, "the churning program parks");
  {
    dv_queue_id out = dv_queue_lookup(inst, "outbox");
    uint8_t buf[64];
    char text[32];
    size_t n = 0;
    if (out != 0 && dv_queue_pop(inst, out, buf, sizeof(buf), &n) == DV_OK)
      lua_says = strtoull(mp_str(buf, n, text, sizeof(text)), NULL, 10);
  }
  dv_memory(inst, &now, &peak);
  ok(lua_says > 0, "and reports what the collector thinks it is holding");
  printf("      (the collector says %lu bytes, the counter says %lu)\n",
         (unsigned long)lua_says, (unsigned long)now);
  /*
  ** A tenth is a wide tolerance and deliberately so: the claim is not that two
  ** counters print the same number, it is that they are counting the same thing.
  ** The defect this catches was off by a factor of three hundred.
  */
  ok(lua_says > 0 && now + lua_says / 10 > lua_says &&
     now < lua_says + lua_says / 10,
     "and the instance's own counter agrees with it, after 400k allocations "
     "have come and gone");
  dv_free(inst);
}


static void memory_reports_what_is_held_now_and_not_only_the_peak (void) {
  /*
  ** 'dv_usage' reports the peak, in kilobytes, and that is the right answer to a
  ** supervisor's question. It cannot answer a host's: an idle agent's peak is
  ** whatever it touched on its way to being idle, so a swarm sized on it is sized
  ** on a moment that has passed. 'dv_memory' is the resting figure.
  **
  ** The program allocates a large table, drops it and collects, so 'now' and 'peak'
  ** are forced apart -- a 'dv_memory' that returned the peak twice, or that read
  ** the same counter under two names, fails here and nowhere else in this file.
  */
  static const char *src =
    "local t = {}\n"
    "for i = 1, 20000 do t[i] = ('x'):rep(64) end\n"
    "t = nil\n"
    "collectgarbage('collect')\n"
    "collectgarbage('collect')\n";
  dv_instance *inst = dv_new(NULL);
  dv_waitset ws;
  uint64_t now = 0, peak = 0, kb = 0;
  if (inst == NULL) { ok(0, "an instance"); return; }
  dv_load(inst, (const uint8_t *)src, strlen(src), "=churn");
  memset(&ws, 0, sizeof(ws));
  dv_run(inst, &ws);
  ok(dv_memory(inst, &now, &peak) == DV_OK, "an instance reports its memory");
  ok(now > 0, "it is holding something");
  ok(peak >= now, "the peak is at least what is held now");
  ok(peak > now, "and strictly more once the garbage it made has gone");
  printf("      (now %lu bytes, peak %lu bytes)\n",
         (unsigned long)now, (unsigned long)peak);
  /* The two readers agree about the one number they share, modulo the division
     'dv_usage' does -- which is the whole reason the second reader exists. */
  dv_usage(inst, NULL, &kb);
  ok(kb == peak / 1024u, "and 'dv_usage' is the same peak, in kilobytes");
  ok(dv_memory(NULL, &now, &peak) == DV_ERROR,
     "a null instance is refused rather than dereferenced");
  dv_free(inst);
}


static void a_budget_cannot_be_changed_mid_flight (void) {
  static const char *src = "local q = queue.declare('bq', {cap = 2}) "
                           "return queue.wait({q})";
  dv_instance *inst = dv_new(NULL);
  dv_waitset ws;
  if (inst == NULL) { ok(0, "an instance"); return; }
  dv_load(inst, (const uint8_t *)src, strlen(src), "=parked");
  memset(&ws, 0, sizeof(ws));
  dv_run(inst, &ws);
  ok(dv_set_budget(inst, 100, 0) == DV_BUSY,
     "a budget cannot be changed once the instance is running");
  dv_free(inst);
}


/*
** Passing NULL where a host should not.
**
** 'dv_load' read 'inst->flags' to pick the load mode two lines *above* its own
** 'inst == NULL' check, so the guard could never fire: a host that passed NULL
** crashed instead of being told. The rest of the ABI was scanned for the same shape
** and this was the only one.
*/
static void null_arguments_are_refused_not_dereferenced (void) {
  dv_instance *inst = dv_new(NULL);
  ok(dv_load(NULL, (const uint8_t *)"return 1", 8, "=x") == DV_ERROR,
     "dv_load with no instance is refused rather than crashing");
  if (inst != NULL) {
    ok(dv_load(inst, NULL, 0, "=x") == DV_ERROR, "and with no code");
    dv_free(inst);
  }
  else
    ok(0, "and with no code");
}


/*
** The error message describes the step that just ran, not the instance's history.
**
** dv.h says it is "valid until the next call on it", and nothing enforced that: the
** buffer was sticky, so anything reading it after a *successful* step saw an error
** from earlier in the instance's life. The swarm layer decides between its "exited"
** and "faulted" events exactly that way, so a supervisor restarted healthy children
** that had recovered from an error -- and with hibernation switched on, a refused
** snapshot set the error and every clean exit afterwards read as a fault.
*/
static void an_error_does_not_outlive_the_step_that_caused_it (void) {
  dv_instance *inst = load("return 0", 0);
  dv_waitset ws;
  if (inst == NULL) { ok(0, "load"); return; }
  /* Resuming an instance that is not parked is refused, and sets a message. */
  eq_st(dv_resume(inst, 1), DV_BUSY, "resuming an unparked instance is refused");
  ok(dv_last_error(inst) != NULL, "and that leaves a message to read");
  memset(&ws, 0, sizeof(ws));
  eq_st(dv_run(inst, &ws), DV_DONE, "the program then runs to completion");
  ok(dv_last_error(inst) == NULL,
     "and the message is gone, so a clean finish cannot read as a failure");
  dv_free(inst);
  /* The other direction: a real error must still be readable after it happens. */
  {
    dv_instance *bad = load("error('boom', 0)", 0);
    if (bad == NULL) { ok(0, "load a failing program"); return; }
    memset(&ws, 0, sizeof(ws));
    eq_st(dv_run(bad, &ws), DV_ERROR, "a failing program reports DV_ERROR");
    ok(dv_last_error(bad) != NULL && strstr(dv_last_error(bad), "boom") != NULL,
       "and its message survives the call that produced it");
    dv_free(bad);
  }
}


/* ======================================================================
** The guest library set (18.2, profile B)
** ====================================================================== */

/*
** Run one statement in a fresh instance and hand back the error it raised, or
** "" if it completed. The statement is all a refusal test needs, because the
** refusals fire before they look at their arguments.
*/
static const char *raised (const char *stmt, uint32_t flags) {
  static char buf[512];
  dv_instance *inst = load(stmt, flags);
  dv_waitset ws;
  const char *msg;
  buf[0] = '\0';
  if (inst == NULL) return "load failed";
  memset(&ws, 0, sizeof(ws));
  dv_run(inst, &ws);
  msg = dv_last_error(inst);
  if (msg != NULL) snprintf(buf, sizeof(buf), "%s", msg);
  dv_free(inst);
  return buf;
}


/*
** The twelve names an instance does not get, checked one at a time.
**
** Table-driven so the set cannot shrink without this failing: a later change
** that puts one of these back has to delete a line here and say why in the
** commit, rather than quietly widening what a guest reaches. The reason each
** one is on the list is in src/dlibs.c.
*/
static void the_debug_library_is_narrowed (void) {
  static const char *const gone[] = {
    "debug", "getregistry", "getmetatable", "setmetatable",
    "getupvalue", "setupvalue", "upvalueid", "upvaluejoin",
    "getuservalue", "setuservalue", "sethook", "setlocal", NULL
  };
  int i;
  for (i = 0; gone[i] != NULL; i++) {
    char stmt[64], what[96];
    const char *msg;
    snprintf(stmt, sizeof(stmt), "debug.%s()", gone[i]);
    snprintf(what, sizeof(what), "debug.%s refuses, by name", gone[i]);
    msg = raised(stmt, 0);
    ok(strstr(msg, "is not available inside a Diluvium instance") != NULL, what);
    if (strstr(msg, "is not available inside a Diluvium instance") == NULL)
      printf("      (%s)\n", msg);
  }
  /* And the refusal is worth reading: it says what the function would have
     defeated, not just that it is gone. */
  ok(strstr(raised("debug.sethook()", 0), "budget") != NULL,
     "and sethook's refusal names the budget it would have switched off");
}


static void a_program_can_still_read_its_own_frames (void) {
  /* The line is "read your own frames, write nothing": taking a traceback away
     would cost a program the ability to report its own failure and buy no
     boundary, because none of these four reaches outside the instance. */
  ok(raised("assert(type(debug.traceback()) == 'string')", 0)[0] == '\0',
     "debug.traceback still works");
  ok(raised("assert(type(debug.getinfo(1)) == 'table')", 0)[0] == '\0',
     "debug.getinfo still works");
  ok(raised("local n = debug.getlocal(1, 1)", 0)[0] == '\0',
     "debug.getlocal still works");
  ok(raised("debug.gethook()", 0)[0] == '\0',
     "debug.gethook still works");
}


/*
** Finding 6 of the M0-M7 audit, end to end.
**
** The host authorises exactly one peer and hands the program nothing. Before
** this release the program read the reference metatable out of
** 'debug.getregistry()' by its own '__name', wrapped guessed peer bytes in a
** table wearing it, and 'endpoint.bind' returned a live handle -- so 7.3's
** "a reference cannot be forged" was false, and 9.3's attenuation, which rests
** on a reference being a capability rather than a guessable name, was false
** with it. The decisive assertion is the same one
** 'a_guest_cannot_mint_a_reference' uses: no queue exists for the peer.
*/
static void the_registry_forgery_route_is_closed (void) {
  dv_instance *inst = load(
    "local out = queue.declare('log', {exported = true}) "
    "pcall(endpoint.bind, setmetatable({}, {}), 'prime') "
    "local ok, reg = pcall(debug.getregistry) "
    "if not ok then queue.push(out, 'refused') return 0 end "
    "for _, v in pairs(reg) do "
    "  if type(v) == 'table' and rawget(v, '__name') == 'diluvium.endpoint.ref' "
    "  then queue.push(out, 'found the metatable') "
    "       local okb = pcall(endpoint.bind, setmetatable({'9'}, v), 'peer') "
    "       queue.push(out, 'bind=' .. tostring(okb)) end end "
    "return 0", 0);
  dv_waitset ws;
  uint8_t buf[256];
  size_t n = 0;
  if (inst == NULL) { ok(0, "load"); return; }
  dv_endpoint_allow(inst, (const uint8_t *)"9", 1, 42);
  memset(&ws, 0, sizeof(ws));
  dv_run(inst, &ws);
  if (dv_queue_pop(inst, dv_queue_lookup(inst, "log"), buf, sizeof(buf), &n)
      == DV_OK && n > 1) {
    char text[256];
    ok(strcmp(mp_str(buf, n, text, sizeof(text)), "refused") == 0,
       "a guest cannot reach the registry the reference metatable lives in");
  }
  else ok(0, "a guest cannot reach the registry the reference metatable lives in");
  ok(dv_endpoint_queue(inst, 42) == 0,
     "and no queue exists for the peer it tried to reach");
  dv_free(inst);
}


/*
** A budget that a program can switch off is not a budget.
**
** 'debug.sethook()' with no arguments is the documented way to clear a hook,
** and 9.4's instruction budget is a count hook in the one slot a lua_State
** has -- so before this release that one line disarmed it: an instance limited
** to 200,000 instructions ran three million and reported 'insn_used' of nought.
** Not found by the M0-M7 audit; found while closing finding 6, because it is
** the same shape as the two budget defects that audit did find.
*/
static void a_guest_cannot_switch_its_own_budget_off (void) {
  static const char *src =
    "pcall(debug.sethook) local n = 0 while true do n = n + 1 end";
  dv_instance *inst = load(src, 0);
  dv_waitset ws;
  uint64_t used = 0;
  if (inst == NULL) { ok(0, "load"); return; }
  ok(dv_set_budget(inst, 200000, 0) == DV_OK, "a budget is set");
  memset(&ws, 0, sizeof(ws));
  eq_st(dv_run(inst, &ws), DV_ERROR,
        "a program that tries to clear its own hook still stops");
  ok(dv_exceeded(inst), "and it is the budget that stopped it");
  dv_usage(inst, &used, NULL);
  ok(used >= 200000, "and the count was kept while it tried");
  dv_free(inst);
}


/*
** The way out, and what it costs.
**
** Profile A -- every program written or templated by the operator -- has a real
** use for the whole library, so it is one flag rather than a fork of the build.
** Setting it puts the three escapes back, which is why the constant is spelled
** the way it is; this asserts both halves so neither can be a surprise.
*/
static void a_host_can_ask_for_the_whole_debug_library (void) {
  ok(raised("assert(type(debug.getregistry()) == 'table')",
            DV_FLAG_UNSAFE_DEBUG)[0] == '\0',
     "DV_FLAG_UNSAFE_DEBUG puts the whole debug library back");
  ok(raised("assert(debug.getmetatable(setmetatable({}, {__metatable = 0})))",
            DV_FLAG_UNSAFE_DEBUG)[0] == '\0',
     "including the parts a narrowed instance refuses");
  {
    /* The cost, asserted rather than only documented: with the flag set the
       budget is switchable-off again, and a host reading this test learns that
       from the test rather than from a surprise in production. */
    dv_instance *inst = load(
      "debug.sethook() local n = 0 for i = 1, 3000000 do n = n + 1 end",
      DV_FLAG_UNSAFE_DEBUG);
    dv_waitset ws;
    if (inst == NULL) { ok(0, "load"); return; }
    dv_set_budget(inst, 200000, 0);
    memset(&ws, 0, sizeof(ws));
    dv_run(inst, &ws);
    ok(!dv_exceeded(inst),
       "and with it set a program can switch its own budget off, as documented");
    dv_free(inst);
  }
}


/*
** Narrowing 'debug' must not have broken every snapshot ever taken.
**
** 10.4 requires the permanents set to be identical on save and restore, and the
** fingerprint in the header is a hash over the sorted *names* the module walk
** finds -- so replacing twelve of the debug library's functions is safe only
** because the names stay. That is the reason they are refusals rather than
** deletions, and it is a claim about a hash rather than something a reader can
** see, so it is asserted here: a snapshot from a narrowed instance restores
** into one that asked for the whole library, and the reverse. If a later change
** deletes a field instead of replacing it, this is what goes red.
**
** The permanent resolves in the *restoring* runtime, which is the behaviour to
** want: a program captured while holding 'debug.sethook' gets whichever
** 'debug.sethook' the instance it wakes in is entitled to.
*/
static void a_snapshot_crosses_the_debug_flag (void) {
  static const char *src =
    "local work = queue.declare('work', {cap = 2})\n"
    "local keep = debug.traceback\n"
    "queue.wait({work})\n";
  uint32_t both[2] = { 0, DV_FLAG_UNSAFE_DEBUG };
  int i;
  for (i = 0; i < 2; i++) {
    dv_config cfg;
    dv_instance *a, *b;
    uint8_t *buf;
    size_t need = 0, got = 0;
    memset(&cfg, 0, sizeof(cfg));
    cfg.abi_version = DV_ABI_VERSION;
    cfg.flags = both[i];
    a = dv_new(&cfg);
    if (a == NULL || !park_on_queue(a, src)) {
      ok(0, "an instance parks"); dv_free(a); continue;
    }
    if (dv_snapshot(a, NULL, NULL, 0, &need) != DV_OK || need == 0) {
      ok(0, "it snapshots"); dv_free(a); continue;
    }
    buf = (uint8_t *)malloc(need);
    if (buf == NULL) { ok(0, "room for the snapshot"); dv_free(a); continue; }
    dv_snapshot(a, NULL, buf, need, &got);
    dv_free(a);
    cfg.flags = both[1 - i];   /* the other side of the flag */
    b = dv_new(&cfg);
    if (b == NULL) { ok(0, "a fresh instance"); free(buf); continue; }
    {
      dv_status st = dv_restore(b, NULL, buf, got);
      ok(st == DV_OK, both[i] == 0
           ? "a narrowed instance's snapshot restores under the whole library"
           : "and one taken under the whole library restores into a narrowed one");
      if (st != DV_OK) printf("      (%s)\n", dv_last_error(b));
    }
    dv_free(b);
    free(buf);
  }
}


/*
** The sealed default, and DV_FLAG_UNSAFE_STDLIB, which undoes it.
**
** Found while writing build4's notes, by checking a claim rather than by the
** M0-M7 audit: 18.2's profile B named `debug` as the last thing between a
** deployment and running programs it did not write, and that list was
** incomplete. An instance got every standard library, so `os.execute`,
** `io.popen`, `io.open` and `package.loadlib` were all in reach, and
** `io.open('/etc/passwd')` returned a file handle.
**
** The default is sealed because an instance is supposed to reach outside itself
** by yielding a request its host answers -- `queue.wait` is that, and
** doc/Determinism.md's hostcall is the general form. `io`/`os`/`package` are a
** second boundary that arrived by inheritance from 'luaL_openlibs'. Both halves
** are asserted: what a program gets by default, and what the flag restores,
** including the file open, because the cost of the flag should be visible in a
** test rather than only in a comment.
*/
static void an_instance_is_sealed_by_default (void) {
  ok(raised("assert(os == nil and io == nil and package == nil)", 0)[0] == '\0',
     "an instance has no os, io or package unless the host asks");
  ok(raised("local f = io.open('/etc/passwd')", 0)[0] != '\0',
     "so it cannot open a file the host never mentioned");
  /* The escape hatch, and what it costs, asserted rather than only documented. */
  ok(raised("assert(os.execute and io.popen and package.loadlib)",
            DV_FLAG_UNSAFE_STDLIB)[0] == '\0',
     "DV_FLAG_UNSAFE_STDLIB puts os, io and package back");
  ok(raised("local f = io.open('/etc/passwd') assert(f) f:close()",
            DV_FLAG_UNSAFE_STDLIB)[0] == '\0',
     "and then io.open really does open a file, which is the point of the name");
}


static void a_sealed_instance_reaches_nothing_outside_itself (void) {
  static const struct { const char *expr, *what; } gone[] = {
    { "os",      "os" },
    { "io",      "io" },
    { "package", "package" },
    { "require", "require" },
    { "dofile",  "dofile" },
    { "loadfile","loadfile" },
    { NULL, NULL }
  };
  int i;
  for (i = 0; gone[i].expr != NULL; i++) {
    char stmt[64], what[96];
    snprintf(stmt, sizeof(stmt), "assert(%s == nil)", gone[i].expr);
    snprintf(what, sizeof(what), "a sealed instance has no '%s'", gone[i].what);
    ok(raised(stmt, 0)[0] == '\0', what);
  }
  /* What is left is the language and the queues, which is the point: sealing
     must not cost a program the ability to do its job. */
  ok(raised("local q = queue.declare('w', {cap = 2}) "
            "queue.push(q, msgpack.encode and 'ok' or 'ok') "
            "assert(#('x'):rep(3) == 3 and math.floor(1.5) == 1) "
            "assert(type(print) == 'function')", 0)[0] == '\0',
     "and still has the language, the queues, and print");
}


/*
** Unlike DV_FLAG_UNSAFE_DEBUG, a snapshot does not cross this one -- and that is
** the correct answer rather than a limitation, because a program captured
** holding 'io.open' has nowhere to land in a state that has none. The mechanism
** is the permanents fingerprint (10.4), which covers names, and sealing removes
** names rather than replacing them.
*/
static void a_snapshot_does_not_cross_the_seal (void) {
  static const char *src =
    "local work = queue.declare('work', {cap = 2})\n"
    "queue.wait({work})\n";
  dv_config cfg;
  dv_instance *a, *b;
  uint8_t *buf;
  size_t need = 0, got = 0;
  memset(&cfg, 0, sizeof(cfg));
  cfg.abi_version = DV_ABI_VERSION;
  cfg.flags = 0;                /* sealed, which is the default */
  a = dv_new(&cfg);
  if (a == NULL || !park_on_queue(a, src)) {
    ok(0, "a sealed instance parks"); dv_free(a); return;
  }
  if (dv_snapshot(a, NULL, NULL, 0, &need) != DV_OK || need == 0) {
    ok(0, "it snapshots"); dv_free(a); return;
  }
  buf = (uint8_t *)malloc(need);
  if (buf == NULL) { ok(0, "room"); dv_free(a); return; }
  dv_snapshot(a, NULL, buf, need, &got);
  dv_free(a);
  cfg.flags = DV_FLAG_UNSAFE_STDLIB;   /* more names, not fewer */
  b = dv_new(&cfg);
  if (b == NULL) { ok(0, "a fresh instance"); free(buf); return; }
  ok(dv_restore(b, NULL, buf, got) != DV_OK,
     "a sealed instance's snapshot does not restore into an unsealed one");
  {
    const char *e = dv_last_error(b);
    ok(e != NULL && strstr(e, "permanents") != NULL,
       "and the refusal names the permanents set");
    if (e != NULL && strstr(e, "permanents") == NULL) printf("      (%s)\n", e);
  }
  dv_free(b);
  free(buf);
  /* And it does restore into another sealed one, so the refusal above is about
     the seal rather than about sealed instances being unsnapshottable. */
  cfg.flags = 0;
  a = dv_new(&cfg);
  if (a != NULL && park_on_queue(a, src)) {
    size_t n2 = 0, g2 = 0;
    uint8_t *b2;
    dv_snapshot(a, NULL, NULL, 0, &n2);
    b2 = (uint8_t *)malloc(n2 != 0 ? n2 : 1);
    if (b2 != NULL) {
      dv_snapshot(a, NULL, b2, n2, &g2);
      dv_free(a);
      b = dv_new(&cfg);
      ok(b != NULL && dv_restore(b, NULL, b2, g2) == DV_OK,
         "but does restore into another sealed one");
      dv_free(b);
      free(b2);
      return;
    }
  }
  dv_free(a);
  ok(0, "but does restore into another sealed one");
}


/*
** A restored program that raises an error. Audit finding 0.
**
** The test that had never existed, which is why the defect was green: nothing in
** the tree resumed a *restored* thread into an error. Every snapshot test drove
** the success path.
**
** What it did before the fix, measured rather than reasoned about: the release
** build **hung forever**, and the same binary under ASan finished with the wrong
** error ("attempt to call a nil value"). Two symptoms from one defect, decided by
** memory layout, which is what undefined behaviour looks like from outside. Note
** which way round that is -- the sanitizer build was the one that did *not*
** reproduce it, so "clean under asan+ubsan" was never going to catch this.
**
** The mechanism, off a live backtrace of the hang:
**
**   lua_resume -> precover -> unroll -> finishCcall -> finishpcallk (ldo.c:813)
**     -> luaF_close -> prepcallclosemth (lfunc.c:161) -> callclosemethod
**     -> luaD_call -> luaD_precall -> tryfuncTM -> luaG_callerror
**
** 'finishpcallk' reads 'ci->u2.funcidx', which the thread record never carried,
** so it is 0 -- the stack base. 'luaF_close' then closes from there, '__close' is
** called on something that is not a function, and that error re-enters 'precover'
** and closes from the base again. The loop is the livelock.
**
** The traceback is still missing from the restored error and present in the fresh
** one: 'old_errfunc' and 'L->errfunc' are absent from the thread record too, so
** dv's message handler does not run at the throw point. That is a separate and
** much smaller gap -- a worse message, not a broken program -- and 10.2 already
** calls it out of scope.
*/
static void a_restored_program_can_raise (void) {
  static const char *src =
    "local q = queue.declare('work', {cap = 2})\n"
    "queue.wait({q})\n"
    "error('boom after waking')\n";
  dv_instance *a = dv_new(NULL), *b;
  uint8_t *buf;
  size_t need = 0, got = 0;
  if (a == NULL || !park_on_queue(a, src)) { ok(0, "an agent parks"); dv_free(a); return; }
  if (dv_snapshot(a, NULL, NULL, 0, &need) != DV_OK || need == 0) {
    ok(0, "it snapshots"); dv_free(a); return;
  }
  buf = (uint8_t *)malloc(need);
  if (buf == NULL) { ok(0, "room"); dv_free(a); return; }
  dv_snapshot(a, NULL, buf, need, &got);
  dv_free(a);
  b = dv_new(NULL);
  if (b == NULL) { ok(0, "a fresh instance"); free(buf); return; }
  if (dv_restore(b, NULL, buf, got) != DV_OK) {
    printf("      (%s)\n", dv_last_error(b));
    ok(0, "it restores");
    dv_free(b); free(buf); return;
  }
  {
    dv_queue_id q = dv_queue_lookup(b, "work");
    static const uint8_t one[] = { 0x01 };
    dv_status st;
    dv_queue_push(b, q, one, sizeof(one));
    /* Before the fix this call did not return. */
    st = dv_resume(b, q);
    eq_st(st, DV_ERROR, "a restored program that raises reports the error");
    {
      const char *m = dv_last_error(b);
      ok(m != NULL && strstr(m, "boom after waking") != NULL,
         "and it is the program's own error, not one from unwinding");
      /* The handler ran at the throw point, which is where a traceback can be
         attached at all. Until the errfunc carry (audit: old_errfunc) the
         restored thread had no handler armed, so this line was the visible
         symptom: the message above arrived correct and bare. */
      ok(m != NULL && strstr(m, "stack traceback") != NULL,
         "and it carries a traceback, so the handler ran at the throw point");
      if (m != NULL && strstr(m, "boom after waking") == NULL)
        printf("      (%s)\n", m);
    }
  }
  dv_free(b);
  free(buf);
}


/*
** The other half of the errfunc carry: the handler slot *saved inside a
** pcall frame*. A guest parked inside its own 'pcall' has the driver's
** handler displaced -- 'lua_pcallk' saved it in the frame's 'old_errfunc' and
** armed nothing, which is why a pcall catches plainly -- and 'finishpcallk'
** re-arms it from that saved slot on the way out. Restore the frame with the
** slot zeroed, as every restore did before the carry, and the catch still
** works but everything after the pcall raises bare.
*/
static void a_restored_pcall_still_guards_and_still_hands_back (void) {
  static const char *src =
    "local q = queue.declare('work', {cap = 2})\n"
    "local ok2, caught = pcall(function()\n"
    "  queue.wait({q})\n"
    "  error('inner boom')\n"
    "end)\n"
    "error('outer: ' .. tostring(caught))\n";
  dv_instance *a = dv_new(NULL), *b;
  uint8_t *buf;
  size_t need = 0, got = 0;
  if (a == NULL || !park_on_queue(a, src)) { ok(0, "an agent parks in a pcall"); dv_free(a); return; }
  if (dv_snapshot(a, NULL, NULL, 0, &need) != DV_OK || need == 0 ||
      (buf = (uint8_t *)malloc(need)) == NULL) {
    ok(0, "it snapshots"); dv_free(a); return;
  }
  dv_snapshot(a, NULL, buf, need, &got);
  dv_free(a);
  b = dv_new(NULL);
  if (b == NULL) { ok(0, "a fresh instance"); free(buf); return; }
  if (dv_restore(b, NULL, buf, got) != DV_OK) {
    printf("      (%s)\n", dv_last_error(b));
    ok(0, "it restores");
    dv_free(b); free(buf); return;
  }
  {
    dv_queue_id q = dv_queue_lookup(b, "work");
    static const uint8_t one[] = { 0x01 };
    dv_status st;
    dv_queue_push(b, q, one, sizeof(one));
    st = dv_resume(b, q);
    eq_st(st, DV_ERROR, "the outer error still comes out");
    {
      const char *m = dv_last_error(b);
      ok(m != NULL && strstr(m, "inner boom") != NULL,
         "the restored pcall caught the inner error");
      ok(m != NULL && strstr(m, "outer:") != NULL,
         "and handed it to the code after it");
      ok(m != NULL && strstr(m, "stack traceback") != NULL,
         "and the handler the pcall displaced came back with the frame");
      if (m != NULL && (strstr(m, "inner boom") == NULL ||
                        strstr(m, "stack traceback") == NULL))
        printf("      (%s)\n", m);
    }
  }
  dv_free(b);
  free(buf);
}


/*
** 10.7 precondition 4, from the ABI (audit finding 14): a program holding a
** suspended coroutine at its park will not snapshot, and the refusal says
** what to do about it. The second half is the design's other commitment --
** the convention is that a supervisor tells an instance to hibernate and the
** instance cleans up and parks capturable, so dropping the coroutine has to
** be sufficient. A refusal a program cannot comply with would be a wall, not
** a check.
*/
static void a_nested_coroutine_refuses_the_snapshot (void) {
  static const char *src =
    "local q = queue.declare('work', {cap = 2})\n"
    "local inner = coroutine.create(function() coroutine.yield(1) end)\n"
    "coroutine.resume(inner)\n"
    "queue.wait({q})\n"      /* parked holding a suspended coroutine */
    "inner = nil\n"
    "queue.wait({q})\n";     /* parked having dropped it */
  dv_instance *a = dv_new(NULL);
  size_t need = 0;
  if (a == NULL || !park_on_queue(a, src)) { ok(0, "an agent parks"); dv_free(a); return; }
  ok(dv_snapshot(a, NULL, NULL, 0, &need) != DV_OK,
     "a program parked holding a suspended coroutine will not snapshot");
  {
    const char *m = dv_last_error(a);
    ok(m != NULL && strstr(m, "nested coroutine") != NULL,
       "and the refusal names the nested coroutine");
    if (m != NULL && strstr(m, "nested coroutine") == NULL)
      printf("      (%s)\n", m);
  }
  {
    dv_queue_id q = dv_queue_lookup(a, "work");
    static const uint8_t one[] = { 0x01 };
    dv_queue_push(a, q, one, sizeof(one));
    if (dv_resume(a, q) != DV_IDLE) {
      ok(0, "the program parks again after dropping the coroutine");
      dv_free(a);
      return;
    }
    ok(dv_snapshot(a, NULL, NULL, 0, &need) == DV_OK && need > 0,
       "and once it is dropped, the same program snapshots");
  }
  dv_free(a);
}


/*
** Finding 12, first half: a reference held across a hibernate is still a
** reference. Its identity is a private metatable, and before that table was
** a permanent the snapshot copied it by content -- so the woken program held
** a value that looked exactly like its reference and failed the rawequal
** test 'bind' answers the identity question with, refused with a message
** asserting the program had built the value itself. The reference is
** delivered, *held unbound* across the snapshot, and bound only after the
** wake, so what this proves is the identity's survival and nothing else's.
*/
static void a_reference_survives_hibernation (void) {
  static const char *src =
    "local inb = queue.lookup('inbox')\n"
    "local _, ref = queue.wait({inb})\n"
    "local work = queue.declare('work', {cap = 2})\n"
    "queue.wait({work})\n"
    "local okb, ep = pcall(endpoint.bind, ref, 'peer')\n"
    "local out = queue.declare('log', {exported = true})\n"
    "queue.push(out, okb and endpoint.status(ep) or tostring(ep))\n"
    "return 0\n";
  dv_instance *a = load(src, 0), *b;
  dv_waitset ws;
  uint8_t refmsg[3], buf[256];
  uint8_t *snap;
  size_t need = 0, got = 0, n = 0;
  if (a == NULL) { ok(0, "load"); return; }
  dv_endpoint_allow(a, (const uint8_t *)"7", 1, 7);
  if (dv_run(a, &ws) != DV_IDLE) { ok(0, "it waits for a reference"); dv_free(a); return; }
  ref_message(refmsg, '7');
  dv_queue_push(a, dv_queue_lookup(a, "inbox"), refmsg, sizeof(refmsg));
  if (dv_resume(a, dv_queue_lookup(a, "inbox")) != DV_IDLE) {
    ok(0, "it parks holding the reference"); dv_free(a); return;
  }
  if (dv_snapshot(a, NULL, NULL, 0, &need) != DV_OK || need == 0 ||
      (snap = (uint8_t *)malloc(need)) == NULL) {
    printf("      (%s)\n", dv_last_error(a));
    ok(0, "it snapshots holding the reference"); dv_free(a); return;
  }
  dv_snapshot(a, NULL, snap, need, &got);
  dv_free(a);
  b = dv_new(NULL);
  if (b == NULL) { ok(0, "a fresh instance"); free(snap); return; }
  /* The authorisation is host state and does not travel; re-supplying it is
     the host's half of the wake. */
  dv_endpoint_allow(b, (const uint8_t *)"7", 1, 7);
  if (dv_restore(b, NULL, snap, got) != DV_OK) {
    printf("      (%s)\n", dv_last_error(b));
    ok(0, "it restores"); dv_free(b); free(snap); return;
  }
  {
    dv_queue_id work = dv_queue_lookup(b, "work");
    static const uint8_t one[] = { 0x01 };
    dv_queue_push(b, work, one, sizeof(one));
    eq_st(dv_resume(b, work), DV_DONE, "the woken program binds and finishes");
    dv_queue_pop(b, dv_queue_lookup(b, "log"), buf, sizeof(buf), &n);
    ok(n == 5 && memcmp(buf + 1, "live", 4) == 0,
       "the restored reference bound: identity survived the snapshot");
    if (!(n == 5 && memcmp(buf + 1, "live", 4) == 0) && n > 1) {
      buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
      printf("      (log said: %s)\n", buf + 1);
    }
    ok(dv_endpoint_queue(b, 7) != 0,
       "and the host's drain path is back under the re-supplied token");
  }
  dv_free(b);
  free(snap);
}


/*
** Finding 12, second half: the endpoint a program *bound before* hibernating.
** The queue travels -- flag, contents and the integer handle the program
** still holds -- but the token map is host state and does not, so before
** adoption a woken program re-binding its own endpoint was told the name was
** already declared, by the queue it had declared, and the host's drain path
** had no way back at all. 'bind' now adopts an endpoint queue no token
** claims. The message pushed before the hibernate coming out after it is the
** point of the whole exercise.
*/
static void a_woken_program_rebinds_its_endpoint (void) {
  static const char *src =
    "local inb = queue.lookup('inbox')\n"
    "local _, ref = queue.wait({inb})\n"
    "local ep = endpoint.bind(ref, 'peer')\n"
    "queue.push(ep, 'before')\n"
    "local work = queue.declare('work', {cap = 2})\n"
    "queue.wait({work})\n"
    "local ep2 = endpoint.bind(ref, 'peer')\n"
    "local out = queue.declare('log', {exported = true})\n"
    "queue.push(out, tostring(ep2 == ep))\n"
    "queue.push(ep2, 'after')\n"
    "return 0\n";
  dv_instance *a = load(src, 0), *b;
  dv_waitset ws;
  uint8_t refmsg[3], buf[256];
  uint8_t *snap;
  size_t need = 0, got = 0, n = 0;
  if (a == NULL) { ok(0, "load"); return; }
  dv_endpoint_allow(a, (const uint8_t *)"7", 1, 7);
  if (dv_run(a, &ws) != DV_IDLE) { ok(0, "it waits for a reference"); dv_free(a); return; }
  ref_message(refmsg, '7');
  dv_queue_push(a, dv_queue_lookup(a, "inbox"), refmsg, sizeof(refmsg));
  if (dv_resume(a, dv_queue_lookup(a, "inbox")) != DV_IDLE) {
    printf("      (%s)\n", dv_last_error(a));
    ok(0, "it binds, pushes, and parks"); dv_free(a); return;
  }
  if (dv_snapshot(a, NULL, NULL, 0, &need) != DV_OK || need == 0 ||
      (snap = (uint8_t *)malloc(need)) == NULL) {
    printf("      (%s)\n", dv_last_error(a));
    ok(0, "it snapshots with an endpoint bound"); dv_free(a); return;
  }
  dv_snapshot(a, NULL, snap, need, &got);
  dv_free(a);
  b = dv_new(NULL);
  if (b == NULL) { ok(0, "a fresh instance"); free(snap); return; }
  dv_endpoint_allow(b, (const uint8_t *)"7", 1, 7);
  if (dv_restore(b, NULL, snap, got) != DV_OK) {
    printf("      (%s)\n", dv_last_error(b));
    ok(0, "it restores"); dv_free(b); free(snap); return;
  }
  {
    dv_queue_id work = dv_queue_lookup(b, "work");
    dv_queue_id drain;
    static const uint8_t one[] = { 0x01 };
    dv_queue_push(b, work, one, sizeof(one));
    eq_st(dv_resume(b, work), DV_DONE, "the woken program re-binds and finishes");
    dv_queue_pop(b, dv_queue_lookup(b, "log"), buf, sizeof(buf), &n);
    ok(n == 5 && memcmp(buf + 1, "true", 4) == 0,
       "adoption returns the very handle the program still held");
    drain = (dv_queue_id)dv_endpoint_queue(b, 7);
    ok(drain != 0, "and the host's drain path is back");
    dv_queue_pop(b, drain, buf, sizeof(buf), &n);
    ok(n == 7 && memcmp(buf + 1, "before", 6) == 0,
       "the message pushed before the hibernate comes out after it");
    dv_queue_pop(b, drain, buf, sizeof(buf), &n);
    ok(n == 6 && memcmp(buf + 1, "after", 5) == 0,
       "ahead of the one pushed after, so ordering held");
  }
  dv_free(b);
  free(snap);
}


/*
** Finding 1: the test that never existed. The count hook was armed in exactly
** one place, inside 'dv_run', and a restored instance cannot reach 'dv_run',
** so a woken agent kept a readable budget and lost its enforcement -- the one
** hibernate+budget test in the tree asserted the number read back while
** cached and never woke it. This one wakes it into the loop 9.4 exists for.
**
** The carry matters as much as the hook: 9.4's budget belongs to the program,
** not to one residency, so the counter must come back exactly where it
** parked, and the abort must charge both residencies against one limit.
*/
static void a_woken_instance_is_still_budgeted (void) {
  static const char *src =
    "local q = queue.declare('work', {cap = 2})\n"
    "local n = 0\n"
    "for i = 1, 50000 do n = n + 1 end\n"   /* so the counter is visibly on */
    "queue.wait({q})\n"
    "while true do n = n + 1 end\n";        /* the runaway, after waking */
  dv_instance *a = dv_new(NULL), *b;
  uint8_t *buf;
  size_t need = 0, got = 0;
  uint64_t u0 = 0, u1 = 0;
  if (a == NULL) { ok(0, "an instance"); return; }
  ok(dv_set_budget(a, 1000000, 0) == DV_OK, "a budget goes on before the run");
  if (!park_on_queue(a, src)) { dv_free(a); return; }
  dv_usage(a, &u0, NULL);
  ok(u0 > 0, "the first residency consumed instructions on the record");
  if (dv_snapshot(a, NULL, NULL, 0, &need) != DV_OK || need == 0 ||
      (buf = (uint8_t *)malloc(need)) == NULL) {
    ok(0, "it snapshots"); dv_free(a); return;
  }
  dv_snapshot(a, NULL, buf, need, &got);
  dv_free(a);
  b = dv_new(NULL);
  if (b == NULL) { ok(0, "a fresh instance"); free(buf); return; }
  ok(dv_set_budget(b, 1000000, 0) == DV_OK,
     "the budget goes on before the restore, the only order there is");
  if (dv_restore(b, NULL, buf, got) != DV_OK) {
    printf("      (%s)\n", dv_last_error(b));
    ok(0, "it restores");
    dv_free(b); free(buf); return;
  }
  dv_usage(b, &u1, NULL);
  ok(u1 == u0, "the counter wakes exactly where it parked");
  if (u1 != u0)
    printf("      (woke at %lu, parked at %lu)\n",
           (unsigned long)u1, (unsigned long)u0);
  {
    dv_queue_id q = dv_queue_lookup(b, "work");
    static const uint8_t one[] = { 0x01 };
    dv_status st;
    dv_queue_push(b, q, one, sizeof(one));
    /* Before the fix this call did not return: no hook, no counting, and a
       loop that never yields has nothing else to stop it. */
    st = dv_resume(b, q);
    eq_st(st, DV_ERROR, "the runaway stops in its second residency");
    ok(dv_exceeded(b), "and the instance says it was the budget");
    dv_usage(b, &u1, NULL);
    ok(u1 >= 1000000, "with both residencies charged against the one limit");
    ok(u1 - u0 < 1000000,
       "and the second alone under it, so the carry is what stopped it");
    {
      const char *m = dv_last_error(b);
      ok(m != NULL && strstr(m, "budget") != NULL,
         "with a message naming the budget");
    }
  }
  dv_free(b);
  free(buf);
}


int main (void) {
  printf("=== dv ABI contract ===\n");
  layout();
  version();
  run_to_completion();
  errors();
  queues();
  a_disabled_queue_refuses_a_host_push();
  zero_copy();
  notification();
  parking();
  timeout_answer();
  null_arguments_are_refused_not_dereferenced();
  an_error_does_not_outlive_the_step_that_caused_it();
  closed_answer();
  a_spurious_resume_invents_nothing();
  blocking_push_from_guest();
  text_only();
  top_level_yield();
  endpoints();
  endpoint_refusals();
  endpoint_preauthorised();
  endpoint_with_no_host_binding();
  a_destroyed_endpoint_can_be_bound_again();
  a_guest_cannot_mint_a_reference();
  a_reference_survives_being_forwarded();
  a_forged_ext_is_not_a_reference();
  the_laundering_route_is_closed();
  relay_between_instances();

  printf("\n=== budgets (9.4) ===\n");
  an_instruction_budget_aborts_a_runaway();
  a_budget_survives_a_guest_pcall();
  usage_keeps_counting_past_the_budget();
  a_budget_does_not_disturb_a_program_inside_it();
  a_memory_budget_refuses_an_allocation();
  the_memory_counter_agrees_with_the_collector();
  memory_reports_what_is_held_now_and_not_only_the_peak();
  a_budget_cannot_be_changed_mid_flight();

  printf("\n=== the guest library set (18.2 profile B) ===\n");
  the_debug_library_is_narrowed();
  a_program_can_still_read_its_own_frames();
  the_registry_forgery_route_is_closed();
  a_guest_cannot_switch_its_own_budget_off();
  a_host_can_ask_for_the_whole_debug_library();
  a_snapshot_crosses_the_debug_flag();
  an_instance_is_sealed_by_default();
  a_sealed_instance_reaches_nothing_outside_itself();
  a_snapshot_does_not_cross_the_seal();

  printf("\n=== hibernate and wake (10.1, 10.6, 10.10) ===\n");
  a_parked_instance_snapshots_and_wakes();
  an_unparked_instance_refuses_to_snapshot();
  a_used_instance_refuses_to_restore();
  the_host_stamp_is_enforced_through_the_abi();
  garbage_is_refused_not_crashed_on();
  a_registered_prototype_shrinks_a_snapshot();
  a_restored_program_can_raise();
  a_restored_pcall_still_guards_and_still_hands_back();
  a_nested_coroutine_refuses_the_snapshot();
  a_reference_survives_hibernation();
  a_woken_program_rebinds_its_endpoint();
  a_woken_instance_is_still_budgeted();

  printf("\n%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
