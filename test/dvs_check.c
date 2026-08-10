/*
** dvs_check.c
** The swarm layer: the instance table, attenuation, lifecycle, and events.
**
** doc/Messaging.md 14's acceptance criteria for M7 are that a supervisor spawns and
** restarts children, that a child cannot be granted capability the supervisor
** lacks, and that a message to a swapped-out instance wakes it in order. The first
** two are here. The third is the snapshot cache, and 9.5 is where it lives.
**
** The host below is a real host and not a stub, which is the point of the vtable:
** 'create' returns nothing because a single-threaded host has nothing to associate,
** and 'drive' is one 'dv_run' or 'dv_resume'. A tokio host would return a task
** handle and drive it differently; nothing above the vtable would change.
**
** Everything a supervisor does here is written in Diluvium, because 9.1 says there
** is no supervisor type -- a program holding the lifecycle capability is what the
** word describes. If any of these tests needed a C-side restart policy, that would
** be evidence the layer had grown something 9.1.2 says belongs in a program.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dvs.h"

/*
** The cursor cross-check below reads the tokens directly, so it needs the codec
** header. That is a test reaching past the layer boundary on purpose -- 'dvs.h'
** itself still includes only 'dv.h', which is the property being kept.
*/
#include "lua.h"
#include "dmsgpack.h"


static int failures = 0;
static int checks = 0;

static void ok (int cond, const char *what) {
  checks++;
  if (cond)
    printf("[PASS] %s\n", what);
  else {
    printf("[FAIL] %s\n", what);
    failures++;
  }
}


static void okf (int cond, const char *what, long got, long want) {
  checks++;
  if (cond)
    printf("[PASS] %s\n", what);
  else {
    printf("[FAIL] %s (got %ld, wanted %ld)\n", what, got, want);
    failures++;
  }
}


/* ---------------------------------------------------------------- the host */

/*
** A single-threaded host. 'drive' advances an instance one step and answers
** whether to keep it: DV_IDLE means parked and still alive, DV_DONE and DV_ERROR
** mean finished. A parked instance is answered with whatever queue is ready, which
** is the host's decision to make and not the swarm's -- 8.3 is explicit that the
** host owns the clock and the choice.
*/
static int host_drive (void *ud, dvs_id id, dv_instance *inst, void *ctx) {
  dv_waitset ws;
  dv_status st;
  (void)ud; (void)id; (void)ctx;
  memset(&ws, 0, sizeof(ws));
  st = dv_waitset_get(inst, &ws);
  if (st == DV_OK && ws.n > 0) {
    /* Answer with the first queue that has something. Nothing ready means the
       instance stays parked, which is a legitimate step and not a stall. */
    int i;
    for (i = 0; i < ws.n; i++) {
      dv_queue_info info;
      memset(&info, 0, sizeof(info));
      if (dv_queue_state(inst, ws.ids[i], &info) == DV_OK && info.len > 0) {
        st = dv_resume(inst, ws.ids[i]);
        return (st == DV_IDLE);
      }
    }
    return 1;                   /* still parked, still alive */
  }
  memset(&ws, 0, sizeof(ws));
  st = dv_run(inst, &ws);
  if (st == DV_BUSY) {
    /* Already started and not parked: nothing for this host to do this step. */
    return 1;
  }
  return (st == DV_IDLE);
}


static int created = 0, destroyed = 0;

static void *host_create (void *ud, dvs_id id, dv_instance *inst) {
  (void)ud; (void)id; (void)inst;
  created++;
  return NULL;
}

static void host_destroy (void *ud, dvs_id id, void *ctx) {
  (void)ud; (void)id; (void)ctx;
  destroyed++;
}


static dvs_swarm *swarm_with (uint32_t rate) {
  dvs_host h;
  memset(&h, 0, sizeof(h));
  h.create = host_create;
  h.destroy = host_destroy;
  h.drive = host_drive;
  return dvs_new(&h, 16, rate);
}


/*
** A swarm with hibernation switched on.
**
** It is off by default in this release, because 18.1's snapshot defect makes the
** wake-then-error path corrupt memory. The tests below still turn it on, so the
** machinery does not rot while it is disabled -- what they must NOT do is stop
** asserting that it is off by default, which is the point of
** 'hibernation_is_off_unless_asked_for'.
*/
static dvs_swarm *swarm_with_hibernation (uint32_t rate) {
  dvs_swarm *sw = swarm_with(rate);
  if (sw != NULL)
    dvs_allow_hibernation(sw, 1);
  return sw;
}


/*
** Off by default, refused by name, and the refusal says why.
**
** 18.2's profile A depends on this: a defect that cannot be reached is worth more
** than one that is merely written down, and a host that hits this should be told
** what it hit rather than left with a bare DVS_ERROR on a call that reads as though
** it should work.
*/
static void hibernation_is_off_unless_asked_for (void) {
  dvs_swarm *sw = swarm_with(4);
  dvs_id root = 0;
  static const char *caps[] = { "queue:work" };
  static const char *src = "local q = queue.declare('work', {cap = 4}) "
                           "queue.wait({q})";
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, src, strlen(src), caps, 1, 0, 0, &root) != DVS_OK) {
    ok(0, "a program starts");
    dvs_free(sw);
    return;
  }
  dvs_step(sw);                 /* it parks, so 'not parked' is not the reason */
  ok(dvs_hibernate(sw, root) == DVS_ERROR,
     "a parked instance is not hibernated by default");
  ok(dvs_resident(sw, root), "and stays resident");
  {
    const char *e = dvs_last_error(sw);
    ok(e != NULL && strstr(e, "18.1") != NULL,
       "and the refusal points at the defect that motivates it");
    ok(e != NULL && strstr(e, "dvs_allow_hibernation") != NULL,
       "and names the call that overrides it");
  }
  dvs_allow_hibernation(sw, 1);
  ok(dvs_hibernate(sw, root) == DVS_OK, "asking for it explicitly works");
  ok(!dvs_resident(sw, root), "and then the instance is cached");
  dvs_free(sw);
}


/*
** The payload of a msgpack string message, NUL-terminated.
**
** Read through the cursor rather than by skipping a header byte. Skipping one is
** right only up to 31 characters: past that the codec emits str8 and 'buf + 1' lands
** on the length byte, which is how a "denied:..." event once read as
** "\x25denied:...". Using the cursor also means these tests exercise the reader the
** swarm layer itself uses.
*/
static const char *msg_str (const uint8_t *b, size_t n, char *out, size_t cap) {
  diluvium_mp_cursor c;
  diluvium_mp_token t;
  out[0] = '\0';
  diluvium_mp_open(&c, b, n);
  if (diluvium_mp_read(&c, &t) && t.kind == DILUVIUM_MP_STR) {
    size_t m = (t.len < cap - 1) ? t.len : cap - 1;
    memcpy(out, t.p, m);
    out[m] = '\0';
  }
  return out;
}


/* Run up to 'n' steps, or until nothing is alive. */
static int spin (dvs_swarm *sw, int n) {
  int i, alive = dvs_alive(sw);
  for (i = 0; i < n && alive > 0; i++)
    alive = dvs_step(sw);
  return alive;
}


/* ------------------------------------------------------- the token cursor */

/*
** The cursor is cross-checked against the codec that wrote the bytes, because a
** second entry point over one format is only trustworthy if the two agree. The
** messages come from a real instance's 'msgpack.encode', so the encoder under test
** is the one programs actually use.
*/
static void the_cursor_agrees_with_the_encoder (void) {
  static const char *src =
    "local out = queue.declare('probe', {cap = 8})\n"
    "queue.push(out, {op = 'spawn', n = 42, neg = -7, big = 70000,\n"
    "  f = 2.5, yes = true, no = false, s = 'hello',\n"
    "  caps = {'a', 'queue:work/*'}, nested = {x = {y = 1}}})\n";
  dv_instance *inst = dv_new(NULL);
  dv_waitset ws;
  uint8_t buf[1024];
  size_t n = 0;
  if (inst == NULL) { ok(0, "an instance"); return; }
  if (dv_load(inst, (const uint8_t *)src, strlen(src), "=probe") != DV_OK) {
    printf("      (%s)\n", dv_last_error(inst));
    ok(0, "the probe loads");
    dv_free(inst);
    return;
  }
  memset(&ws, 0, sizeof(ws));
  dv_run(inst, &ws);
  if (dv_queue_pop(inst, dv_queue_lookup(inst, "probe"), buf, sizeof(buf), &n)
      != DV_OK) {
    ok(0, "the probe pushed a message");
    dv_free(inst);
    return;
  }
  {
    diluvium_mp_cursor c;
    diluvium_mp_token t;
    diluvium_mp_open(&c, buf, n);
    ok(diluvium_mp_read(&c, &t) && t.kind == DILUVIUM_MP_MAP,
       "the cursor reads a map header");
    /* Every field, by name, each from a fresh cursor -- which is the documented
       way to use it and also what proves 'skip' walks a value correctly. */
    diluvium_mp_open(&c, buf, n);
    ok(diluvium_mp_field(&c, "s") && diluvium_mp_read(&c, &t) &&
       t.kind == DILUVIUM_MP_STR && t.len == 5 &&
       memcmp(t.p, "hello", 5) == 0, "and finds a string field");
    diluvium_mp_open(&c, buf, n);
    ok(diluvium_mp_field(&c, "n") && diluvium_mp_read(&c, &t) &&
       t.kind == DILUVIUM_MP_INT && t.i == 42, "a small integer");
    diluvium_mp_open(&c, buf, n);
    ok(diluvium_mp_field(&c, "neg") && diluvium_mp_read(&c, &t) &&
       t.kind == DILUVIUM_MP_INT && t.i == -7, "a negative integer");
    diluvium_mp_open(&c, buf, n);
    ok(diluvium_mp_field(&c, "big") && diluvium_mp_read(&c, &t) &&
       t.kind == DILUVIUM_MP_INT && t.i == 70000, "a wide integer");
    diluvium_mp_open(&c, buf, n);
    ok(diluvium_mp_field(&c, "f") && diluvium_mp_read(&c, &t) &&
       t.kind == DILUVIUM_MP_FLOAT && t.f == 2.5, "a float");
    diluvium_mp_open(&c, buf, n);
    ok(diluvium_mp_field(&c, "yes") && diluvium_mp_read(&c, &t) &&
       t.kind == DILUVIUM_MP_BOOL && t.b == 1, "true");
    diluvium_mp_open(&c, buf, n);
    ok(diluvium_mp_field(&c, "no") && diluvium_mp_read(&c, &t) &&
       t.kind == DILUVIUM_MP_BOOL && t.b == 0, "and false as false");
    /* An array of strings, which is what a caps list is. */
    diluvium_mp_open(&c, buf, n);
    if (diluvium_mp_field(&c, "caps") && diluvium_mp_read(&c, &t) &&
        t.kind == DILUVIUM_MP_ARRAY && t.len == 2) {
      diluvium_mp_token e;
      ok(1, "an array reports its length");
      diluvium_mp_read(&c, &e);
      ok(e.kind == DILUVIUM_MP_STR && e.len == 1 && e.p[0] == 'a',
         "and its elements follow it");
      diluvium_mp_read(&c, &e);
      ok(e.kind == DILUVIUM_MP_STR && e.len == 12 &&
         memcmp(e.p, "queue:work/*", 12) == 0, "including the second");
    }
    else {
      ok(0, "an array reports its length");
      ok(0, "and its elements follow it");
      ok(0, "including the second");
    }
    /* Finding a field *after* a nested table proves 'skip' descends properly: the
       nested map has to be stepped over as one value, not as three tokens. */
    diluvium_mp_open(&c, buf, n);
    ok(diluvium_mp_field(&c, "op") && diluvium_mp_read(&c, &t) &&
       t.kind == DILUVIUM_MP_STR && memcmp(t.p, "spawn", 5) == 0,
       "a field found past a nested table, so 'skip' descends");
    diluvium_mp_open(&c, buf, n);
    ok(!diluvium_mp_field(&c, "absent"), "and an absent field is not found");
  }
  dv_free(inst);
}


/* ------------------------------------------------------------ the swarm */

/*
** A supervisor, written in Diluvium. It spawns one child, waits for events, and
** restarts the child when it hears that it stopped -- which is 9.1's claim made
** concrete: the restart policy is these eight lines of Lua, not a C flag.
*/
static const char SUPERVISOR[] =
  "local sys = queue.declare('system/lifecycle', {cap = 8})\n"
  "local ev  = queue.declare('system/events', {cap = 16})\n"
  "local log = queue.declare('log', {cap = 16})\n"
  "local CHILD = \"local q = queue.declare('work', {cap = 4})\\n\"\n"
  "           .. \"local id, v = queue.wait({q})\\n\"\n"
  "local starts = 0\n"
  "local function start()\n"
  "  starts = starts + 1\n"
  "  queue.push(sys, {op = 'spawn', code = CHILD, caps = {'queue:work/*'}})\n"
  "end\n"
  "start()\n"
  "while starts <= 3 do\n"
  "  local id, e = queue.wait({ev})\n"
  "  queue.push(log, tostring(e.event) .. ':' .. tostring(e.id))\n"
  "  if e.event == 'exited' or e.event == 'faulted' or e.event == 'exceeded' then\n"
  "    start()\n"
  "  end\n"
  "end\n"
  "queue.push(log, 'done:' .. starts)\n";


static void a_supervisor_spawns_and_restarts (void) {
  dvs_swarm *sw = swarm_with(4);
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle", "queue:work/*", "queue:log" };
  dv_instance *sup;
  if (sw == NULL) { ok(0, "a swarm"); return; }
  ok(dvs_root(sw, SUPERVISOR, sizeof(SUPERVISOR) - 1, caps, 3, 0, 0, &root)
     == DVS_OK, "a supervisor goes in as the root");
  if (root == 0) {
    printf("      (%s)\n", dvs_last_error(sw));
    dvs_free(sw);
    return;
  }
  sup = dvs_instance(sw, root);
  ok(sup != NULL, "and the host can reach its instance");
  ok(dvs_parent(sw, root) == 0, "the root has no parent");

  /*
  ** Step until the supervisor is done. Each child parks on an empty queue, so the
  ** host never answers it -- which is how a child "finishes" here: the supervisor
  ** kills it, hears the event, and starts another. That is a restart loop with no
  ** restart mechanism anywhere in C.
  **
  ** The log is drained *inside* the loop, because a finished instance is released
  ** the moment 'drive' says it is done and everything in it goes with it. A handle
  ** outlives the instance behind it; a 'dv_instance *' does not. So the pointer is
  ** re-fetched every step rather than cached, which is the contract 'dvs_instance'
  ** documents and the reason it takes a handle.
  */
  {
    int i;
    int spawned = 0;
    int lines = 0;
    int exits = 0;
    char first[128];
    first[0] = '\0';
    for (i = 0; i < 40; i++) {
      dvs_step(sw);
      if (dvs_alive(sw) > 1 && !spawned)
        spawned = 1;
      /* Kill the child, standing in for it crashing. A real supervisor would hear
         about a fault; here the point is that it hears at all. */
      if (dvs_alive(sw) > 1) {
        uint32_t k;
        for (k = 2; k < 40; k++) {
          if (dvs_instance(sw, k) != NULL && dvs_parent(sw, k) == root) {
            dvs_kill(sw, k);
            break;
          }
        }
      }
      sup = dvs_instance(sw, root);
      if (sup != NULL) {
        dv_queue_id log = dv_queue_lookup(sup, "log");
        uint8_t out[192];
        size_t n = 0;
        while (log != 0 &&
               dv_queue_pop(sup, log, out, sizeof(out), &n) == DV_OK && n > 1) {
          size_t m = (n - 1 < sizeof(first) - 1) ? n - 1 : sizeof(first) - 1;
          char line[128];
          memcpy(line, out + 1, m);
          line[m] = '\0';
          if (lines == 0)
            memcpy(first, line, m + 1);
          if (strncmp(line, "exited", 6) == 0)
            exits++;
          lines++;
        }
      }
    }
    ok(spawned, "the supervisor's spawn request created a child");
    /* What the supervisor logged is the evidence: it saw events and acted. */
    okf(lines > 0, "and it logged the events it was told about", lines, 1);
    if (lines > 0)
      printf("      (first log line: %s, %d in all)\n", first, lines);
    /* The first thing it hears is that the spawn succeeded, which is an event in
       its own right: a supervisor that cannot tell "spawned" from "denied" cannot
       implement a backoff. */
    ok(strncmp(first, "spawned:", 8) == 0,
       "the first line is the spawn it asked for");
    /* Restarting is the criterion, not merely spawning: hearing 'exited' more than
       once means it started a replacement after each one. */
    okf(exits >= 2, "and it restarted the child each time one exited", exits, 2);
  }
  dvs_free(sw);
}


static void a_child_cannot_be_granted_more_than_its_parent (void) {
  /*
  ** 9.3, and the acceptance criterion 14 names. The supervisor holds
  ** 'queue:work/*' and asks for 'queue:secret' as well; the second must be
  ** refused, and refused *before* anything is built, so a denied spawn leaves
  ** nothing behind.
  */
  static const char OVERREACH[] =
    "local sys = queue.declare('system/lifecycle', {cap = 8})\n"
    "local ev  = queue.declare('system/events', {cap = 8})\n"
    "local log = queue.declare('log', {cap = 8})\n"
    /* The child parks rather than returning, so that "no child was created" is a
       real check: a child that exits on its first step is reaped before anyone
       looks, and the assertion would hold whether the grant was refused or not. */
    "local KID = \"local q = queue.declare('hold', {cap = 2}) \"\n"
    "         .. \"queue.wait({q})\"\n"
    "queue.push(sys, {op = 'spawn', code = KID,\n"
    "                 caps = {'queue:work/x', 'queue:secret'}})\n"
    "local hold = queue.declare('hold', {cap = 2})\n"
    "local id, e = queue.wait({ev})\n"
    "queue.push(log, tostring(e.event) .. '/' .. tostring(e.detail))\n"
    /* Park rather than return, so the log survives to be read: an instance that
       finishes is released, and its queues go with it. */
    "queue.wait({hold})\n";
  dvs_swarm *sw = swarm_with(4);
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle", "queue:work/*" };
  dv_instance *sup;
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, OVERREACH, sizeof(OVERREACH) - 1, caps, 2, 0, 0, &root)
      != DVS_OK) {
    printf("      (%s)\n", dvs_last_error(sw));
    ok(0, "the overreaching supervisor starts");
    dvs_free(sw);
    return;
  }
  sup = dvs_instance(sw, root);
  ok(!dvs_holds(sw, root, "queue:secret"),
     "the supervisor does not hold the capability it is about to ask for");
  ok(dvs_holds(sw, root, "queue:work/anything"),
     "but does hold the pattern it was given");
  ok(!dvs_may_grant(sw, root, "queue:secret"),
     "so granting it is refused");
  spin(sw, 6);
  okf(dvs_alive(sw) == 1, "no child was created", dvs_alive(sw), 1);
  {
    dv_queue_id log = dv_queue_lookup(sup, "log");
    uint8_t out[128];
    size_t n = 0;
    if (log != 0 && dv_queue_pop(sup, log, out, sizeof(out), &n) == DV_OK &&
        n > 1) {
      printf("      (%.*s)\n", (int)n - 1, (const char *)out + 1);
      ok(memcmp(out + 1, "denied", 6) == 0,
         "and the supervisor was told, with the capability named");
      ok(memmem(out + 1, n - 1, "queue:secret", 12) != NULL,
         "naming which capability was refused");
    }
    else {
      ok(0, "and the supervisor was told, with the capability named");
      ok(0, "naming which capability was refused");
    }
  }
  dvs_free(sw);
}


static void a_program_without_the_capability_is_not_drained (void) {
  /*
  ** 9.3 by mechanism rather than by a special case: a program that does not hold
  ** the lifecycle capability may declare 'system/lifecycle' and push to it, and
  ** nothing will ever read it. No error, no refusal at declare time -- the request
  ** simply goes nowhere, which is what "it holds no capability for the target
  ** queue" means.
  */
  static const char SNEAK[] =
    "local sys = queue.declare('system/lifecycle', {cap = 8})\n"
    "local log = queue.declare('log', {cap = 8})\n"
    "local hold = queue.declare('hold', {cap = 2})\n"
    "queue.push(sys, {op = 'spawn', code = 'return 1', caps = {}})\n"
    "queue.push(log, 'asked:' .. queue.len(sys))\n"
    "queue.wait({hold})\n";
  dvs_swarm *sw = swarm_with(4);
  dvs_id root = 0;
  static const char *caps[] = { "queue:log" };
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, SNEAK, sizeof(SNEAK) - 1, caps, 1, 0, 0, &root) != DVS_OK) {
    ok(0, "the program starts");
    dvs_free(sw);
    return;
  }
  spin(sw, 6);
  okf(dvs_alive(sw) <= 1, "a program without the lifecycle capability spawns "
      "nothing", dvs_alive(sw), 1);
  {
    dv_instance *inst = dvs_instance(sw, root);
    if (inst != NULL) {
      dv_queue_info info;
      memset(&info, 0, sizeof(info));
      dv_queue_state(inst, dv_queue_lookup(inst, "system/lifecycle"), &info);
      okf(info.len == 1, "and its request is still sitting in the queue, unread",
          (long)info.len, 1);
    }
    else
      ok(0, "and its request is still sitting in the queue, unread");
  }
  dvs_free(sw);
}


static void killing_a_parent_kills_the_subtree (void) {
  /*
  ** 9.5's default, and it is not negotiable in this layer: "reparenting is harder
  ** to remove once depended on". Three generations, so the test is about a subtree
  ** and not just about children.
  */
  static const char CHAIN[] =
    "local sys = queue.declare('system/lifecycle', {cap = 8})\n"
    "local ev  = queue.declare('system/events', {cap = 8})\n"
    "local q   = queue.declare('hold', {cap = 2})\n"
    "local KID = \"local sys = queue.declare('system/lifecycle', {cap = 8})\\n\"\n"
    "         .. \"local ev = queue.declare('system/events', {cap = 8})\\n\"\n"
    "         .. \"local q = queue.declare('hold', {cap = 2})\\n\"\n"
    "         .. \"queue.push(sys, {op = 'spawn', code = \\\"local q = \"\n"
    "         .. \"queue.declare('hold', {cap = 2}) queue.wait({q})\\\",\"\n"
    "         .. \" caps = {'lifecycle'}})\\n\"\n"
    "         .. \"queue.wait({q})\\n\"\n"
    "queue.push(sys, {op = 'spawn', code = KID, caps = {'lifecycle'}})\n"
    "queue.wait({q})\n";
  dvs_swarm *sw = swarm_with(4);
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle" };
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, CHAIN, sizeof(CHAIN) - 1, caps, 1, 0, 0, &root) != DVS_OK) {
    printf("      (%s)\n", dvs_last_error(sw));
    ok(0, "the chain starts");
    dvs_free(sw);
    return;
  }
  spin(sw, 8);
  okf(dvs_alive(sw) == 3, "three generations are alive", dvs_alive(sw), 3);
  /* Find the middle one and kill it: the grandchild must go too. */
  {
    uint32_t k, middle = 0;
    for (k = 1; k < 40; k++) {
      if (dvs_instance(sw, k) != NULL && dvs_parent(sw, k) == root) {
        middle = k;
        break;
      }
    }
    ok(middle != 0, "the middle generation is findable by parentage");
    if (middle != 0) {
      ok(dvs_kill(sw, middle) == DVS_OK, "and can be killed");
      okf(dvs_alive(sw) == 1, "which takes its subtree with it",
          dvs_alive(sw), 1);
    }
  }
  dvs_free(sw);
}


static void a_child_cannot_kill_its_supervisor (void) {
  /*
  ** Parentage is the only relation this layer knows, so "who may kill whom" has to
  ** follow from it. Without the ancestry check a child could kill the supervisor
  ** that spawned it, which would make the capability set decorative.
  */
  static const char REBEL[] =
    "local sys = queue.declare('system/lifecycle', {cap = 8})\n"
    "local ev  = queue.declare('system/events', {cap = 8})\n"
    "local q   = queue.declare('hold', {cap = 2})\n"
    "local KID = \"local sys = queue.declare('system/lifecycle', {cap = 8})\\n\"\n"
    "         .. \"local ev = queue.declare('system/events', {cap = 8})\\n\"\n"
    "         .. \"local q = queue.declare('hold', {cap = 2})\\n\"\n"
    "         .. \"queue.push(sys, {op = 'kill', id = 1})\\n\"\n"
    "         .. \"queue.wait({q})\\n\"\n"
    "queue.push(sys, {op = 'spawn', code = KID, caps = {'lifecycle'}})\n"
    "queue.wait({q})\n";
  dvs_swarm *sw = swarm_with(4);
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle" };
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, REBEL, sizeof(REBEL) - 1, caps, 1, 0, 0, &root) != DVS_OK) {
    ok(0, "the rebel starts");
    dvs_free(sw);
    return;
  }
  spin(sw, 8);
  ok(dvs_instance(sw, root) != NULL,
     "a child's kill request against its own supervisor is refused");
  okf(dvs_alive(sw) == 2, "and both are still running", dvs_alive(sw), 2);
  dvs_free(sw);
}


static void the_spawn_rate_is_limited (void) {
  /*
  ** 9.5: "a self-rewriting system will produce a fork bomb eventually, as a bug
  ** rather than an attack". A rate limit is therefore a default and not an option,
  ** and the requester is told so it can back off -- what it does about that is
  ** policy.
  */
  static const char BOMB[] =
    "local sys = queue.declare('system/lifecycle', {cap = 64})\n"
    "local ev  = queue.declare('system/events', {cap = 64})\n"
    "local q   = queue.declare('hold', {cap = 2})\n"
    "for i = 1, 10 do\n"
    "  queue.push(sys, {op = 'spawn', code = \"local q = \"\n"
    "    .. \"queue.declare('hold', {cap = 2}) queue.wait({q})\", caps = {}})\n"
    "end\n"
    "queue.wait({q})\n";
  dvs_swarm *sw = swarm_with(3);          /* three spawns per step */
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle" };
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, BOMB, sizeof(BOMB) - 1, caps, 1, 0, 0, &root) != DVS_OK) {
    ok(0, "the fork bomb starts");
    dvs_free(sw);
    return;
  }
  /*
  ** Two steps, not one, and the distinction matters: a step drains before it
  ** drives, so on the very first step the root has not run yet and its queue is
  ** empty. Checking the limit after one step would pass no matter what the limit
  ** was -- which is exactly what it did until removing the limit turned nothing
  ** red.
  */
  dvs_step(sw);
  okf(dvs_alive(sw) == 1, "the root pushes its ten requests on the first step",
      dvs_alive(sw), 1);
  dvs_step(sw);
  okf(dvs_alive(sw) <= 4, "and the first drain spawns at most the rate limit",
      dvs_alive(sw), 4);
  spin(sw, 6);
  ok(dvs_alive(sw) > 4, "with the rest arriving over later steps");
  printf("      (%d alive after eight steps of a ten-child request)\n",
         dvs_alive(sw));
  dvs_free(sw);
}


static void the_table_is_bounded_and_handles_are_not_reused (void) {
  dvs_host h;
  dvs_swarm *sw;
  dvs_id a = 0, b = 0;
  static const char *caps[] = { "lifecycle" };
  static const char *src = "local q = queue.declare('hold', {cap = 2}) "
                           "return queue.wait({q})";
  memset(&h, 0, sizeof(h));
  h.drive = host_drive;
  sw = dvs_new(&h, 1, 0);                 /* room for exactly one */
  if (sw == NULL) { ok(0, "a swarm of one"); return; }
  ok(dvs_root(sw, src, strlen(src), caps, 1, 0, 0, &a) == DVS_OK,
     "one instance fits");
  ok(dvs_root(sw, src, strlen(src), caps, 1, 0, 0, &b) == DVS_LIMIT,
     "and a second is refused rather than overrunning the table");
  ok(dvs_kill(sw, a) == DVS_OK, "the first is killed");
  ok(dvs_root(sw, src, strlen(src), caps, 1, 0, 0, &b) == DVS_OK,
     "which frees the slot");
  ok(b != a, "but the handle is new: a stale handle never names a live instance");
  dvs_free(sw);
}


static void a_swarm_needs_a_drive_function (void) {
  dvs_host h;
  memset(&h, 0, sizeof(h));
  /* No 'drive'. A swarm that cannot advance anything is a caller's bug, and
     saying so at construction beats a step that silently does nothing. */
  ok(dvs_new(&h, 4, 4) == NULL, "a swarm with no 'drive' is refused");
  ok(dvs_abi_version() == DVS_ABI_VERSION, "the ABI version is reported");
}


/* ------------------------------------------------- the snapshot cache (9.5) */

/*
** A program that hibernates itself, and counts what it has been sent across the
** swap. The counter is an ordinary local, so its surviving is the whole claim: the
** instance the messages are delivered into is a different one, restored from bytes.
*/
static const char SLEEPER[] =
  "local sys = queue.declare('system/lifecycle', {cap = 4})\n"
  "local inbox = queue.declare('work', {cap = 8})\n"
  "local log = queue.declare('log', {cap = 16})\n"
  "local seen = {}\n"
  /* Asking to be woken is the program's own decision (8.4), so it says so in the
     request rather than having a parent guess at spawn time. */
  "queue.push(sys, {op = 'hibernate', wake_on_message = true})\n"
  "while true do\n"
  "  local id, v = queue.wait({inbox})\n"
  "  seen[#seen + 1] = tostring(v)\n"
  "  queue.push(log, table.concat(seen, ','))\n"
  "end\n";


/* The newest line in an instance's 'log', or "" if there is none. */
static void last_log (dv_instance *inst, char *out, size_t cap) {
  uint8_t buf[256];
  size_t n = 0;
  dv_queue_id log;
  out[0] = '\0';
  if (inst == NULL)
    return;
  log = dv_queue_lookup(inst, "log");
  if (log == 0)
    return;
  while (dv_queue_pop(inst, log, buf, sizeof(buf), &n) == DV_OK && n > 1) {
    size_t m = (n - 1 < cap - 1) ? n - 1 : cap - 1;
    memcpy(out, buf + 1, m);
    out[m] = '\0';
  }
}


static void an_instance_swaps_out_and_a_message_wakes_it (void) {
  dvs_swarm *sw = swarm_with_hibernation(4);
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle", "queue:work", "queue:log" };
  char line[128];
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, SLEEPER, sizeof(SLEEPER) - 1, caps, 3, 0, 0, &root)
      != DVS_OK) {
    printf("      (%s)\n", dvs_last_error(sw));
    ok(0, "the sleeper starts");
    dvs_free(sw);
    return;
  }
  /* Step one runs it: it asks to hibernate and parks on 'work'. Step two drains
     that request, and 'dv_snapshot' needs it parked, which by then it is. */
  dvs_step(sw);
  ok(dvs_resident(sw, root), "the sleeper is resident while it runs");
  dvs_step(sw);
  ok(!dvs_resident(sw, root), "and non-resident after it asks to hibernate");
  okf(dvs_alive(sw) == 1, "still alive, though: a cached instance is not a dead "
      "one", dvs_alive(sw), 1);
  ok(dvs_cached_size(sw, root) > 0, "with its whole state in the cache");
  printf("      (%lu bytes cached)\n", (unsigned long)dvs_cached_size(sw, root));
  ok(dvs_instance(sw, root) == NULL,
     "and no instance behind the handle at all, which is the point");

  /*
  ** 8.4's row: a push to a non-resident instance with 'wake_on_message' is accepted
  ** and answered "ok". Three of them, so that ordering is a claim about a sequence.
  */
  ok(dvs_push(sw, root, "work", "\xa3one", 4) == DVS_OK,
     "a push to a cached instance is accepted rather than refused");
  ok(dvs_push(sw, root, "work", "\xa3two", 4) == DVS_OK, "and a second");
  ok(!dvs_resident(sw, root),
     "the push does not restore it inline -- 8.4 calls that asynchronous");
  dvs_step(sw);
  ok(dvs_resident(sw, root), "the next step wakes it");
  okf(dvs_cached_size(sw, root) == 0, "and the cache is emptied",
      (long)dvs_cached_size(sw, root), 0);
  last_log(dvs_instance(sw, root), line, sizeof(line));
  printf("      (log: %s)\n", line);
  ok(strcmp(line, "one,two") == 0,
     "both buffered messages arrived, in the order they were sent");

  /* A live push now goes straight in, and must land *after* what was buffered --
     which the log shows, because the local 'seen' survived the swap. */
  ok(dvs_push(sw, root, "work", "\xa5three", 6) == DVS_OK, "a live push lands");
  dvs_step(sw);
  last_log(dvs_instance(sw, root), line, sizeof(line));
  printf("      (log: %s)\n", line);
  ok(strcmp(line, "one,two,three") == 0,
     "behind the buffered ones, and the program's locals survived the swap");
  dvs_free(sw);
}


static void a_cached_instance_without_wake_on_message_is_gone (void) {
  /*
  ** 8.4's other row, and the reason both exist: waking on a message is a property
  ** of the destination, not a favour the sender may ask for. An agent that did not
  ** ask to be woken is, to a sender, not there -- and "gone" says so immediately
  ** instead of buffering something nothing will ever read.
  */
  dvs_swarm *sw = swarm_with_hibernation(4);
  dvs_id root = 0, kid = 0;
  static const char *caps[] = { "lifecycle", "queue:work" };
  static const char PARENT[] =
    "local sys = queue.declare('system/lifecycle', {cap = 8})\n"
    "local ev  = queue.declare('system/events', {cap = 8})\n"
    "local hold = queue.declare('hold', {cap = 2})\n"
    "local KID = \"local sys = queue.declare('system/lifecycle', {cap = 4})\\n\"\n"
    "         .. \"local w = queue.declare('work', {cap = 4})\\n\"\n"
    "         .. \"queue.push(sys, {op = 'hibernate'})\\n\"\n"
    "         .. \"queue.wait({w})\\n\"\n"
    /* No 'wake_on_message', which is the default and the point of this test. */
    "queue.push(sys, {op = 'spawn', code = KID,\n"
    "                 caps = {'lifecycle', 'queue:work'}})\n"
    "queue.wait({hold})\n";
  uint32_t k;
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, PARENT, sizeof(PARENT) - 1, caps, 2, 0, 0, &root) != DVS_OK) {
    printf("      (%s)\n", dvs_last_error(sw));
    ok(0, "the parent starts");
    dvs_free(sw);
    return;
  }
  spin(sw, 5);
  for (k = 1; k < 40; k++) {
    if (dvs_parent(sw, k) == root) { kid = k; break; }
  }
  ok(kid != 0, "a child that hibernates itself");
  ok(kid != 0 && !dvs_resident(sw, kid), "is swapped out");
  okf(kid != 0 && dvs_push(sw, kid, "work", "\xa1x", 2) == DVS_GONE,
      "and a push to it is 'gone', not buffered",
      kid != 0 ? dvs_push(sw, kid, "work", "\xa1x", 2) : -1, DVS_GONE);
  dvs_free(sw);
}


static void the_wake_buffer_is_bounded (void) {
  /*
  ** 6.2's bounded queues exist so backpressure is visible, and an unbounded wake
  ** buffer would be the one place in the system where it was not. So a full buffer
  ** is DVS_LIMIT, and the sender learns it the same way a push to a full live queue
  ** does.
  */
  dvs_swarm *sw = swarm_with_hibernation(4);
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle", "queue:work", "queue:log" };
  int i, accepted = 0, refused = 0;
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, SLEEPER, sizeof(SLEEPER) - 1, caps, 3, 0, 0, &root)
      != DVS_OK) {
    ok(0, "the sleeper starts");
    dvs_free(sw);
    return;
  }
  dvs_step(sw);
  dvs_step(sw);
  if (dvs_resident(sw, root)) {
    ok(0, "the sleeper hibernated");
    dvs_free(sw);
    return;
  }
  for (i = 0; i < 64; i++) {
    if (dvs_push(sw, root, "work", "\xa1x", 2) == DVS_OK)
      accepted++;
    else
      refused++;
  }
  ok(accepted > 0, "the wake buffer accepts messages");
  ok(refused > 0, "and refuses rather than growing without bound");
  printf("      (%d accepted, %d refused of 64)\n", accepted, refused);
  dvs_free(sw);
}


static void pushing_to_a_dead_instance_is_gone (void) {
  dvs_swarm *sw = swarm_with(4);
  dvs_id root = 0;
  static const char *caps[] = { "queue:work" };
  static const char *src = "local w = queue.declare('work', {cap = 4}) "
                           "queue.wait({w})";
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, src, strlen(src), caps, 1, 0, 0, &root) != DVS_OK) {
    ok(0, "a program starts");
    dvs_free(sw);
    return;
  }
  dvs_step(sw);
  ok(dvs_push(sw, root, "work", "\xa1x", 2) == DVS_OK,
     "a push to a resident instance goes into the queue");
  ok(dvs_push(sw, root, "nosuch", "\xa1x", 2) == DVS_UNKNOWN,
     "a push to a queue it never declared is unknown, not gone");
  dvs_kill(sw, root);
  ok(dvs_push(sw, root, "work", "\xa1x", 2) == DVS_GONE,
     "and once it is dead, every push is 'gone' immediately");
  ok(dvs_push(sw, 9999, "work", "\xa1x", 2) == DVS_GONE,
     "as is a push to a handle that never existed");
  dvs_free(sw);
}


/*
** What a host can learn about an instance it was handed.
**
** These exist because 'dvs_spawn' was a public struct whose comment said it was
** "handed to the host so it can size its own context" -- and no public function
** took one, so the host never saw it. The information was genuinely missing rather
** than merely mislabelled: the instance ABI has 'dv_set_budget' but no getter, so a
** host had no way at all to learn the budget an instance was configured with.
*/
static void a_host_can_read_an_instance_s_budget_and_capabilities (void) {
  dvs_swarm *sw = swarm_with_hibernation(4);
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle", "queue:work/*", "queue:log" };
  static const char *src = "local q = queue.declare('hold', {cap = 2}) "
                           "queue.wait({q})";
  uint64_t insns = 0, mem = 0;
  const char *got[8];
  size_t n;
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, src, strlen(src), caps, 3, 5000000, 4096, &root) != DVS_OK) {
    printf("      (%s)\n", dvs_last_error(sw));
    ok(0, "a budgeted root starts");
    dvs_free(sw);
    return;
  }
  ok(dvs_budget(sw, root, &insns, &mem) == DVS_OK, "the budget reads back");
  okf(insns == 5000000, "with the instruction count it was given",
      (long)insns, 5000000);
  okf(mem == 4096, "and the memory limit", (long)mem, 4096);
  /* Either pointer may be NULL, because a host usually wants one of the two. */
  ok(dvs_budget(sw, root, NULL, NULL) == DVS_OK,
     "and both outputs are optional");
  ok(dvs_budget(sw, 9999, &insns, &mem) == DVS_UNKNOWN,
     "an unknown handle is unknown, not zero");

  /* The count comes back even when nothing is copied, so a caller can size. */
  okf((long)dvs_caps(sw, root, NULL, 0), "asking with max 0 gives the count",
      (long)dvs_caps(sw, root, NULL, 0), 3);
  n = dvs_caps(sw, root, got, 8);
  okf(n == 3, "and the names copy out", (long)n, 3);
  ok(n == 3 && strcmp(got[0], "lifecycle") == 0 &&
     strcmp(got[1], "queue:work/*") == 0 && strcmp(got[2], "queue:log") == 0,
     "in the order they were granted, so an audit log is reproducible");
  /* A short buffer truncates rather than overflowing, and still reports the truth. */
  got[1] = NULL;
  okf(dvs_caps(sw, root, got, 1) == 3,
      "a buffer too small still reports the real count",
      (long)dvs_caps(sw, root, got, 1), 3);
  ok(got[1] == NULL, "and writes no more than it was allowed");

  /*
  ** The budget survives hibernation, which is the case that motivated making this a
  ** query rather than an argument to 'create': a host deciding whether it can
  ** afford to wake a cached instance needs the number precisely when there is no
  ** instance to ask.
  */
  /* One step first: 'dv_snapshot' requires a *parked* instance, and a root that
     has not been driven yet has not reached its 'queue.wait'. Refusing to snapshot
     it is correct, so the step is the test's obligation and not a workaround. */
  ok(dvs_hibernate(sw, root) != DVS_OK,
     "an instance that has not run yet cannot be hibernated, since it is not "
     "parked");
  dvs_step(sw);
  ok(dvs_hibernate(sw, root) == DVS_OK, "once parked, the instance hibernates");
  ok(!dvs_resident(sw, root), "and is not resident");
  insns = 0;
  ok(dvs_budget(sw, root, &insns, NULL) == DVS_OK && insns == 5000000,
     "its budget is still readable while it is only bytes");
  okf((long)dvs_caps(sw, root, got, 8), "as are its capabilities",
      (long)dvs_caps(sw, root, got, 8), 3);
  dvs_free(sw);
}


/*
** A child that FAULTS while it has a parent to be told.
**
** No test had this shape, and three separate bugs were hiding in it. The children
** here are killed by the test or exit cleanly, so 'faulted' was never true for an
** instance with a parent, and the sanitizers never reached the path:
**
**   1. 'dv_last_error' returns a pointer into the instance's error buffer, and the
**      fault path freed the instance before handing that pointer to 'emit_event' --
**      a use-after-free on exactly the interesting case.
**   2. 'emit_event' wrote a map header claiming three pairs and then dropped the
**      third when it ran out of room, and a fault detail is a Lua error with a
**      traceback, so the supervisor met a truncated message.
**   3. 'put_str' wrote str8's single length byte from a size_t, so a 300-byte
**      detail announced 44 bytes and copied 300.
**
** The child below raises an error with a deliberately enormous message, which is
** what a real traceback looks like to this code.
*/
static void a_faulting_child_reports_a_readable_reason (void) {
  static const char SUP[] =
    "local sys = queue.declare('system/lifecycle', {cap = 8})\n"
    "local ev  = queue.declare('system/events', {cap = 8})\n"
    "local log = queue.declare('log', {cap = 8})\n"
    "local hold = queue.declare('hold', {cap = 2})\n"
    /* The child errors immediately, with a message far longer than the event
       buffer, so the truncation paths are the ones under test. */
    "local KID = \"error(string.rep('x', 4000))\"\n"
    "queue.push(sys, {op = 'spawn', code = KID, caps = {}})\n"
    "while true do\n"
    "  local id, e = queue.wait({ev})\n"
    "  queue.push(log, tostring(e.event) .. '|' .. tostring(e.id) .. '|' ..\n"
    "    tostring(e.detail and #e.detail or 0))\n"
    "end\n";
  dvs_swarm *sw = swarm_with(4);
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle", "queue:log" };
  int i, saw_fault = 0;
  char line[256];
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, SUP, sizeof(SUP) - 1, caps, 2, 0, 0, &root) != DVS_OK) {
    printf("      (%s)\n", dvs_last_error(sw));
    ok(0, "the supervisor starts");
    dvs_free(sw);
    return;
  }
  line[0] = '\0';
  for (i = 0; i < 12; i++) {
    dv_instance *sup;
    dvs_step(sw);
    sup = dvs_instance(sw, root);
    if (sup == NULL) break;
    {
      dv_queue_id log = dv_queue_lookup(sup, "log");
      uint8_t out[512];
      size_t n = 0;
      while (log != 0 &&
             dv_queue_pop(sup, log, out, sizeof(out), &n) == DV_OK && n > 1) {
        /* Short strings are fixstr; these are, being 'faulted|2|191' or so. */
        size_t m = (n - 1 < sizeof(line) - 1) ? n - 1 : sizeof(line) - 1;
        memcpy(line, out + 1, m);
        line[m] = '\0';
        if (strncmp(line, "faulted|", 8) == 0) saw_fault = 1;
      }
    }
  }
  ok(saw_fault, "the supervisor is told its child faulted");
  printf("      (event: %s)\n", line[0] ? line : "(none)");
  /*
  ** The decisive part: the supervisor DECODED the event. A map header promising a
  ** field the message does not carry makes 'queue.wait' raise inside the program,
  ** so reaching the log at all means the message was well formed -- and the length
  ** it reports proves the detail arrived rather than being dropped.
  */
  ok(saw_fault && strstr(line, "|0") == NULL,
     "and the detail survived, so the event was not a truncated map");
  dvs_free(sw);
}


/*
** A bare "*" is a name, not a licence.
**
** 'strncmp(held, want, 0)' returns 0 for any pair of strings, so a one-character
** "*" matched every capability. Worse, it was reachable by attenuation rather than
** only by a host granting it: "**" implies "*", so a parent holding "**" could hand
** a child the bare "*" and the child would hold more than the parent. 9.3 says a
** grant may only narrow.
*/
static void a_wildcard_cannot_widen_a_grant (void) {
  dvs_host h;
  dvs_swarm *sw;
  dvs_id star = 0, dstar = 0, scoped = 0;
  static const char *just_star[] = { "*" };
  static const char *double_star[] = { "**" };
  static const char *scoped_caps[] = { "queue:work/*" };
  static const char *src = "local q = queue.declare('hold', {cap = 2}) "
                           "return queue.wait({q})";
  memset(&h, 0, sizeof(h));
  h.drive = host_drive;
  sw = dvs_new(&h, 8, 4);
  if (sw == NULL) { ok(0, "a swarm"); return; }
  dvs_root(sw, src, strlen(src), just_star, 1, 0, 0, &star);
  dvs_root(sw, src, strlen(src), double_star, 1, 0, 0, &dstar);
  dvs_root(sw, src, strlen(src), scoped_caps, 1, 0, 0, &scoped);

  ok(!dvs_holds(sw, star, "lifecycle"),
     "holding \"*\" does not grant the lifecycle capability");
  ok(!dvs_holds(sw, star, "queue:anything"), "nor any queue");
  ok(dvs_holds(sw, star, "*"), "it grants only the literal name it is");
  /*
  ** "**" may still pass "*" on, and that is now a *narrowing* rather than the
  ** escalation it used to be: the child ends up holding the literal "*", which
  ** grants nothing. What matters is not whether the grant is refused but whether it
  ** can buy the child anything, so that is what is asserted.
  */
  ok(dvs_holds(sw, dstar, "*anything"),
     "\"**\" is a prefix pattern over names starting with a star");
  ok(!dvs_holds(sw, dstar, "lifecycle"),
     "and does not reach a name that does not start with one");
  ok(!dvs_holds(sw, star, "queue:work/a") && !dvs_holds(sw, star, "lifecycle"),
     "so a child handed \"*\" holds nothing its parent could not already reach");
  /* The legitimate pattern still works, in both directions. */
  ok(dvs_holds(sw, scoped, "queue:work/a"), "a real prefix pattern still matches");
  ok(!dvs_holds(sw, scoped, "queue:other"), "and still does not over-match");
  ok(dvs_may_grant(sw, scoped, "queue:work/a"),
     "and can still be narrowed for a child");
  dvs_free(sw);
}


/* ------------------------------------------------- the Profile A defect set */

/*
** A wire id that does not fit a handle is refused, not truncated.
**
** 'dvs_id' is 32 bits and the wire carries 64. Casting the wider value meant a
** request naming 0x100000002 became 2, 'find' located instance 2, and the kill was
** carried out **against a different live instance** and reported as success. The
** supervisor below asks to kill 0x100000000 + the child's id, which is not a handle
** and must not resolve to one.
*/
static void a_wire_id_that_does_not_fit_is_refused (void) {
  static const char SUP[] =
    "local sys = queue.declare('system/lifecycle', {cap = 8})\n"
    "local ev  = queue.declare('system/events', {cap = 8})\n"
    "local log = queue.declare('log', {cap = 8})\n"
    "local hold = queue.declare('hold', {cap = 2})\n"
    "local KID = \"local q = queue.declare('hold', {cap = 2}) queue.wait({q})\"\n"
    "queue.push(sys, {op = 'spawn', code = KID, caps = {}})\n"
    "local _, e = queue.wait({ev})\n"
    "queue.push(log, 'spawned:' .. tostring(e.id))\n"
    /* 2^32 + the child's handle: the low 32 bits are a live instance. */
    "queue.push(sys, {op = 'kill', id = 4294967296 + e.id})\n"
    "local _, e2 = queue.wait({ev})\n"
    "queue.push(log, tostring(e2.event) .. ':' .. tostring(e2.detail))\n"
    "queue.wait({hold})\n";
  dvs_swarm *sw = swarm_with(4);
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle", "queue:log" };
  int i;
  char first[128], second[128];
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, SUP, sizeof(SUP) - 1, caps, 2, 0, 0, &root) != DVS_OK) {
    printf("      (%s)\n", dvs_last_error(sw));
    ok(0, "the supervisor starts");
    dvs_free(sw);
    return;
  }
  first[0] = second[0] = '\0';
  for (i = 0; i < 10; i++) {
    dv_instance *sup = dvs_instance(sw, root);
    dvs_step(sw);
    sup = dvs_instance(sw, root);
    if (sup == NULL) break;
    {
      dv_queue_id log = dv_queue_lookup(sup, "log");
      uint8_t out[192];
      size_t n = 0;
      while (log != 0 &&
             dv_queue_pop(sup, log, out, sizeof(out), &n) == DV_OK && n > 1) {
        char *slot = (first[0] == '\0') ? first : second;
        msg_str(out, n, slot, sizeof(first));
      }
    }
  }
  printf("      (%s / %s)\n", first, second);
  ok(strncmp(first, "spawned:", 8) == 0, "a child is spawned");
  ok(strncmp(second, "denied:", 7) == 0,
     "and a kill naming 2^32 + its handle is denied, not carried out");
  /* The decisive part: the child is still alive, because the truncated id named it. */
  okf(dvs_alive(sw) == 2, "the instance the truncation would have named survives",
      dvs_alive(sw), 2);
  dvs_free(sw);
}


/*
** A spawned program is not cut at its first zero byte.
**
** 'do_spawn' took the code with 'field_str' and then measured it with 'strlen', so a
** compiled chunk -- which is full of zeroes -- ran as a prefix of itself and the
** spawn was reported as successful. Source with an embedded zero is the same bug in
** miniature and needs no compiler to demonstrate.
*/
static void a_spawn_is_not_truncated_at_a_zero_byte (void) {
  static const char SUP[] =
    "local sys = queue.declare('system/lifecycle', {cap = 8})\n"
    "local ev  = queue.declare('system/events', {cap = 8})\n"
    "local log = queue.declare('log', {cap = 8})\n"
    "local hold = queue.declare('hold', {cap = 2})\n"
    /* The zero byte sits inside a string literal, so a truncated chunk is a syntax
       error and a whole one runs. The child reports which happened. */
    "local KID = \"local marker = 'a\\0b'\\n\"\n"
    "         .. \"local q = queue.declare('hold', {cap = 2})\\n\"\n"
    "         .. \"queue.wait({q})\\n\"\n"
    "queue.push(sys, {op = 'spawn', code = KID, caps = {}})\n"
    "local _, e = queue.wait({ev})\n"
    "queue.push(log, tostring(e.event))\n"
    "queue.wait({hold})\n";
  dvs_swarm *sw = swarm_with(4);
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle", "queue:log" };
  int i;
  char line[128];
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, SUP, sizeof(SUP) - 1, caps, 2, 0, 0, &root) != DVS_OK) {
    ok(0, "the supervisor starts");
    dvs_free(sw);
    return;
  }
  line[0] = '\0';
  for (i = 0; i < 8; i++) {
    dv_instance *sup;
    dvs_step(sw);
    sup = dvs_instance(sw, root);
    if (sup == NULL) break;
    {
      dv_queue_id log = dv_queue_lookup(sup, "log");
      uint8_t out[192];
      size_t n = 0;
      if (log != 0 && dv_queue_pop(sup, log, out, sizeof(out), &n) == DV_OK &&
          n > 1)
        msg_str(out, n, line, sizeof(line));
    }
  }
  printf("      (event: %s)\n", line[0] ? line : "(none)");
  ok(strcmp(line, "spawned") == 0,
     "a program containing a zero byte spawns whole rather than as a prefix");
  okf(dvs_alive(sw) == 2, "and the child is running", dvs_alive(sw), 2);
  dvs_free(sw);
}


/*
** A chain is killed from the top, at whatever depth it has.
**
** 'kill_subtree' carried a comment saying it did not recurse, above a body that
** called itself once per level. The comment is now true -- it marks and sweeps over
** the flat table -- but the crash the finding described is **not reachable**, and
** saying so is more useful than a test pretending otherwise: ~131,000 nested frames
** fit in an 8 MB stack, and 131,000 instances would need something like 6 GB of
** lua_States to exist at all. The machine runs out of instances long before the
** stack runs out of frames.
**
** So what changed is that the code matches its comment and the depth limit is gone
** in principle, not that an exploitable crash was closed. What is testable is the
** behaviour, which the mark-and-sweep must preserve: a chain dies from its root
** whatever its length, and nothing outside the chain is touched.
*/
static void a_chain_is_killed_from_the_top (void) {
  dvs_swarm *sw = swarm_with(4);
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle" };
  /* Three generations by delegation, plus an unrelated root that must survive --
     the sweep marks by parentage, so a bug that marked too much would take it. */
  static const char CHAIN[] =
    "local sys = queue.declare('system/lifecycle', {cap = 8})\n"
    "local q   = queue.declare('hold', {cap = 2})\n"
    "local KID = \"local sys = queue.declare('system/lifecycle', {cap = 8})\\n\"\n"
    "         .. \"local q = queue.declare('hold', {cap = 2})\\n\"\n"
    "         .. \"queue.push(sys, {op = 'spawn', code = \\\"local q = \"\n"
    "         .. \"queue.declare('hold', {cap = 2}) queue.wait({q})\\\",\"\n"
    "         .. \" caps = {'lifecycle'}})\\n\"\n"
    "         .. \"queue.wait({q})\\n\"\n"
    "queue.push(sys, {op = 'spawn', code = KID, caps = {'lifecycle'}})\n"
    "queue.wait({q})\n";
  static const char *lone = "local q = queue.declare('hold', {cap = 2}) "
                            "return queue.wait({q})";
  dvs_id bystander = 0;
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, CHAIN, sizeof(CHAIN) - 1, caps, 1, 0, 0, &root) != DVS_OK ||
      dvs_root(sw, lone, strlen(lone), caps, 1, 0, 0, &bystander) != DVS_OK) {
    printf("      (%s)\n", dvs_last_error(sw) ? dvs_last_error(sw) : "?");
    ok(0, "a chain and an unrelated instance start");
    dvs_free(sw);
    return;
  }
  spin(sw, 8);
  okf(dvs_alive(sw) == 4, "three generations plus one bystander",
      dvs_alive(sw), 4);
  ok(dvs_kill(sw, root) == DVS_OK, "the chain's root is killed");
  okf(dvs_alive(sw) == 1, "the whole chain goes with it", dvs_alive(sw), 1);
  ok(dvs_instance(sw, bystander) != NULL,
     "and the unrelated instance is untouched, so the sweep marked by parentage");
  dvs_free(sw);
}


/*
** A budget written the way 9.1 writes it actually reaches the child.
**
** 9.1's example nests it -- 'budget = { instructions = 5e6, memory_kb = 512 }' --
** and 'do_spawn' read flat top-level fields, so a request copied out of the document
** gave the child **no budget at all**, silently. The consequence is not cosmetic: a
** runaway child then has nothing to stop it, and 9.4 is explicit that a guest cannot
** limit itself because a loop that never yields never gives anything the chance.
*/
static void a_documented_budget_reaches_the_child (void) {
  static const char SUP[] =
    "local sys = queue.declare('system/lifecycle', {cap = 8})\n"
    "local ev  = queue.declare('system/events', {cap = 8})\n"
    "local log = queue.declare('log', {cap = 8})\n"
    "local hold = queue.declare('hold', {cap = 2})\n"
    /*
    ** A child that parks, so its budget can be read back from outside while it is
    ** still alive.
    **
    ** Deliberately NOT a runaway child here. Spawning 'while true do end' would make
    ** this test hang rather than fail whenever the budget does not reach the child --
    ** which is exactly what happened when the fix was pulled out to verify it, and a
    ** test that burns a 30-minute CI timeout instead of failing in a second is a bad
    ** trade for coverage that lives elsewhere. Enforcement is covered by
    ** 'a_host_can_budget_every_child_from_create', where the host always sets a
    ** budget and so nothing can spin.
    */
    "queue.push(sys, {op = 'spawn', code = \"local q = \"\n"
    "  .. \"queue.declare('hold', {cap = 2}) queue.wait({q})\", caps = {},\n"
    "  budget = {instructions = 200000, memory_kb = 4096}})\n"
    "local _, e = queue.wait({ev})\n"
    "queue.push(log, 'spawned:' .. tostring(e.id))\n"
    "queue.wait({hold})\n";
  dvs_swarm *sw = swarm_with(4);
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle", "queue:log" };
  int i;
  char first[128], second[128];
  dvs_id kid = 0;
  uint64_t insns = 0, mem = 0;
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, SUP, sizeof(SUP) - 1, caps, 2, 0, 0, &root) != DVS_OK) {
    printf("      (%s)\n", dvs_last_error(sw));
    ok(0, "the supervisor starts");
    dvs_free(sw);
    return;
  }
  first[0] = second[0] = '\0';
  /* Bounded: if the budget did not reach the child, the loop below is where a
     runaway 'while true do end' hangs the whole swarm -- which is the failure this
     test exists to catch, and why it cannot be an unbounded spin. */
  for (i = 0; i < 12; i++) {
    dv_instance *sup;
    uint32_t k;
    dvs_step(sw);
    for (k = 2; k < 24 && kid == 0; k++) {
      if (dvs_parent(sw, k) == root) {
        kid = k;
        /* Read the budget while the child is still alive: it is about to exceed and
           be reaped, and a released slot answers DVS_UNKNOWN. */
        dvs_budget(sw, kid, &insns, &mem);
      }
    }
    sup = dvs_instance(sw, root);
    if (sup == NULL) break;
    {
      dv_queue_id log = dv_queue_lookup(sup, "log");
      uint8_t out[192];
      size_t n = 0;
      while (log != 0 &&
             dv_queue_pop(sup, log, out, sizeof(out), &n) == DV_OK && n > 1)
        msg_str(out, n, (first[0] == '\0') ? first : second, sizeof(first));
    }
  }
  printf("      (%s / %s)\n", first, second);
  ok(strncmp(first, "spawned:", 8) == 0, "the child is spawned");
  /* The budget was recorded on the slot, which is what 'dvs_budget' reads -- and
     also proves the nested form was parsed rather than defaulted to zero. */
  if (kid != 0)
    printf("      (child budget: insns=%lu mem_kb=%lu)\n",
           (unsigned long)insns, (unsigned long)mem);
  okf(insns == 200000, "with the instruction budget the request nested",
      (long)insns, 200000);
  okf(mem == 4096, "and the memory limit", (long)mem, 4096);
  /* The recorded budget is what 'build' hands to 'dv_set_budget', and that call's
     enforcement is asserted where nothing can hang. Together they cover the path. */
  dvs_free(sw);
}


/*
** The flat form still works, because it is what this layer understood until now.
*/
static void the_flat_budget_form_is_still_accepted (void) {
  static const char SUP[] =
    "local sys = queue.declare('system/lifecycle', {cap = 8})\n"
    "local ev  = queue.declare('system/events', {cap = 8})\n"
    "local hold = queue.declare('hold', {cap = 2})\n"
    "queue.push(sys, {op = 'spawn', code = \"local q = \"\n"
    "  .. \"queue.declare('hold', {cap = 2}) queue.wait({q})\", caps = {},\n"
    "  instructions = 123456, memory_kb = 77})\n"
    "queue.wait({hold})\n";
  dvs_swarm *sw = swarm_with(4);
  dvs_id root = 0, kid = 0;
  static const char *caps[] = { "lifecycle" };
  uint64_t insns = 0, mem = 0;
  uint32_t k;
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, SUP, sizeof(SUP) - 1, caps, 1, 0, 0, &root) != DVS_OK) {
    ok(0, "the supervisor starts");
    dvs_free(sw);
    return;
  }
  spin(sw, 5);
  for (k = 2; k < 24 && kid == 0; k++)
    if (dvs_parent(sw, k) == root) kid = k;
  ok(kid != 0, "a child spawned with a flat budget");
  dvs_budget(sw, kid, &insns, &mem);
  okf(insns == 123456, "carries the flat instruction budget", (long)insns, 123456);
  okf(mem == 77, "and the flat memory limit", (long)mem, 77);
  dvs_free(sw);
}


/*
** A host can set a budget itself, from 'create'.
**
** 18.2's profile A leans on this: rather than trust the request path, a host sets
** every child's budget as it is handed the instance. 'create' is called after
** 'dv_load' and before anything runs, which is exactly the window 'dv_set_budget'
** requires -- it refuses a running instance, because "a budget that changed
** mid-flight would make 'exceeded' mean nothing". Asserted rather than assumed,
** because a workaround nobody has run is a claim.
*/
static uint64_t host_budget_insns = 0;

static void *host_create_budgeted (void *ud, dvs_id id, dv_instance *inst) {
  (void)ud; (void)id;
  if (inst != NULL && host_budget_insns != 0)
    dv_set_budget(inst, host_budget_insns, 0);
  return NULL;
}

static void a_host_can_budget_every_child_from_create (void) {
  dvs_host h;
  dvs_swarm *sw;
  dvs_id root = 0;
  static const char *caps[] = { "lifecycle", "queue:log" };
  static const char SUP[] =
    "local sys = queue.declare('system/lifecycle', {cap = 8})\n"
    "local ev  = queue.declare('system/events', {cap = 8})\n"
    "local log = queue.declare('log', {cap = 8})\n"
    "local hold = queue.declare('hold', {cap = 2})\n"
    /* No budget in the request at all: the host supplies it. */
    "queue.push(sys, {op = 'spawn', code = 'while true do end', caps = {}})\n"
    "local _, e = queue.wait({ev})\n"
    "local _, e2 = queue.wait({ev})\n"
    "queue.push(log, tostring(e2.event))\n"
    "queue.wait({hold})\n";
  int i;
  char line[128];
  memset(&h, 0, sizeof(h));
  h.create = host_create_budgeted;
  h.drive = host_drive;
  host_budget_insns = 200000;
  sw = dvs_new(&h, 16, 4);
  if (sw == NULL) { ok(0, "a swarm"); return; }
  if (dvs_root(sw, SUP, sizeof(SUP) - 1, caps, 2, 0, 0, &root) != DVS_OK) {
    ok(0, "the supervisor starts");
    dvs_free(sw);
    host_budget_insns = 0;
    return;
  }
  line[0] = '\0';
  for (i = 0; i < 12; i++) {
    dv_instance *sup;
    dvs_step(sw);
    sup = dvs_instance(sw, root);
    if (sup == NULL) break;
    {
      dv_queue_id log = dv_queue_lookup(sup, "log");
      uint8_t out[192];
      size_t n = 0;
      if (log != 0 && dv_queue_pop(sup, log, out, sizeof(out), &n) == DV_OK &&
          n > 1)
        msg_str(out, n, line, sizeof(line));
    }
  }
  printf("      (event: %s)\n", line[0] ? line : "(none)");
  ok(strcmp(line, "exceeded") == 0,
     "a host-set budget stops a runaway child that asked for none");
  host_budget_insns = 0;
  dvs_free(sw);
}


int main (void) {
  printf("=== the msgpack token cursor ===\n");
  the_cursor_agrees_with_the_encoder();

  printf("\n=== the swarm layer (9.1 - 9.5) ===\n");
  a_swarm_needs_a_drive_function();
  the_table_is_bounded_and_handles_are_not_reused();
  a_supervisor_spawns_and_restarts();
  a_child_cannot_be_granted_more_than_its_parent();
  a_program_without_the_capability_is_not_drained();
  killing_a_parent_kills_the_subtree();
  a_child_cannot_kill_its_supervisor();
  a_faulting_child_reports_a_readable_reason();
  a_wildcard_cannot_widen_a_grant();
  a_wire_id_that_does_not_fit_is_refused();
  a_spawn_is_not_truncated_at_a_zero_byte();
  a_chain_is_killed_from_the_top();
  a_documented_budget_reaches_the_child();
  the_flat_budget_form_is_still_accepted();
  a_host_can_budget_every_child_from_create();

  a_host_can_read_an_instance_s_budget_and_capabilities();

  printf("\n=== the snapshot cache and wake_on_message (8.4, 9.5) ===\n");
  hibernation_is_off_unless_asked_for();
  pushing_to_a_dead_instance_is_gone();
  an_instance_swaps_out_and_a_message_wakes_it();
  a_cached_instance_without_wake_on_message_is_gone();
  the_wake_buffer_is_bounded();
  the_spawn_rate_is_limited();

  printf("\n%d checks, %d failed\n", checks, failures);
  return failures != 0;
}
