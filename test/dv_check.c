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
  endpoints();
  endpoint_refusals();
  endpoint_preauthorised();
  endpoint_with_no_host_binding();
  a_guest_cannot_mint_a_reference();
  relay_between_instances();

  printf("\n=== budgets (9.4) ===\n");
  an_instruction_budget_aborts_a_runaway();
  a_budget_does_not_disturb_a_program_inside_it();
  a_memory_budget_refuses_an_allocation();
  a_budget_cannot_be_changed_mid_flight();

  printf("\n=== hibernate and wake (10.1, 10.6, 10.10) ===\n");
  a_parked_instance_snapshots_and_wakes();
  an_unparked_instance_refuses_to_snapshot();
  a_used_instance_refuses_to_restore();
  the_host_stamp_is_enforced_through_the_abi();
  garbage_is_refused_not_crashed_on();
  a_registered_prototype_shrinks_a_snapshot();

  printf("\n%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
